// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// tabulate/tab/tab1/tab2 + table — C++ port of piet/backend/piet/tabulate.py
// and piet/backend/piet/table.py (ROADMAP M4a item 1: data-summary output).
// Pure computation over a Datastore — no cobra call. Mirrors the oracle's
// numpy-based grouping bug-for-bug (label-string formatting, chi2 via a
// scipy-free continued-fraction/series expansion, N/A vs 0 vs null semantics
// for degenerate cells).
#include "piet/handlers.h"
#include "piet/eval/errors.h"

#include "data/missing.h"  // cecon::missing_kind: the `.a`-`.z` kind of a cell
#include "piet/eval/exprfns.h"  // stataMissingWithKind / stataExtendedMissingKind

#include <algorithm>
#include <limits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <set>

namespace piet::handlers {

using ojson = nlohmann::ordered_json;

namespace {

// pythonFloatRepr-style label for a numeric value (tabulate.py's rows.append
// via `str(v)` on a numpy float64 — Python's str(float) repr, e.g. "1.0",
// "2.5"). Mirrors eval::pythonFloatRepr used elsewhere in this codebase for
// the same str(float) contract; declared here as a local shim so this file
// has no eval/ dependency for a single helper.
std::string pyFloatStr(double v) {
  if (std::isnan(v)) return "nan";
  // Python's str(float): a mathematically-integer value always renders
  // "N.0"; otherwise the shortest decimal that round-trips back to the
  // same double (approximated here via increasing %g precision until
  // strtod reproduces the input exactly).
  char buf[64];
  if (v == std::floor(v) && std::abs(v) < 1e16) {
    std::snprintf(buf, sizeof(buf), "%.1f", v);
    return std::string(buf);
  }
  // Shortest round-trip decimal via increasing precision until it re-parses
  // to the same double (approximates Python's repr for non-integer floats).
  for (int prec = 1; prec <= 17; ++prec) {
    std::snprintf(buf, sizeof(buf), "%.*g", prec, v);
    if (std::strtod(buf, nullptr) == v) return std::string(buf);
  }
  std::snprintf(buf, sizeof(buf), "%.17g", v);
  return std::string(buf);
}

// log Gamma(x) via the Lanczos g=7 approximation — matches tabulate.py's
// _log_gamma exactly (same coefficient table) so chi2 p-values reproduce
// the oracle bit-for-bit within float tolerance.
double logGamma(double x) {
  if (x < 0.5) {
    return std::log(M_PI) - std::log(std::abs(std::sin(M_PI * x))) - logGamma(1.0 - x);
  }
  x -= 1.0;
  static const double g = 7.0;
  static const double c[] = {
      0.99999999999980993,  676.5203681218851,    -1259.1392167224028,
      771.32342877765313,   -176.61502916214059,  12.507343278686905,
      -0.13857109526572012, 9.9843695780195716e-6, 1.5056327351493116e-7,
  };
  double t = x + g + 0.5;
  double s = 0.0;
  for (int i = 1; i <= 8; ++i) s += c[i] / (x + i);
  return 0.5 * std::log(2 * M_PI) + (x + 0.5) * std::log(t) - t + std::log(c[0] + s);
}

// Upper-tail chi-squared survival function P(X > x), dof k — port of
// tabulate.py's _chi2_sf (series expansion below x2<a+1, Lentz continued
// fraction above).
double chi2Sf(double x, double k) {
  if (x <= 0) return 1.0;
  double a = k / 2.0;
  double x2 = x / 2.0;
  if (x2 < a + 1) {
    // BUG-1012: the series is P(a,x) = e^-x x^a / Gamma(a+1) * sum, and it used
    // Gamma(a): the lower tail came out `a` times too large, so every p below the
    // continued-fraction switch (x/2 < k/2 + 1) was wrong. w2as sx_tab_two_chi2,
    // chi2 1.2446 on 3 df: Stata p 0.7425, this 0.6137 (1 - 1.5 x 0.2575).
    double s = std::exp(-x2 + a * std::log(x2) - logGamma(a)) / a;
    double term = s;
    for (int n = 1; n <= 300; ++n) {
      term *= x2 / (a + n);
      s += term;
      if (std::abs(term) < 1e-14 * std::abs(s)) break;
    }
    return std::max(0.0, 1.0 - s);
  }
  double fpmin = 1e-300;
  double b = x2 + 1.0 - a;
  double c = 1.0 / fpmin;
  double d = 1.0 / b;
  double h = d;
  for (int i = 1; i <= 300; ++i) {
    double an = -static_cast<double>(i) * (i - a);
    b += 2.0;
    d = an * d + b;
    if (std::abs(d) < fpmin) d = fpmin;
    c = b + an / c;
    if (std::abs(c) < fpmin) c = fpmin;
    d = 1.0 / d;
    double delta = d * c;
    h *= delta;
    if (std::abs(delta - 1.0) < 1e-14) break;
  }
  return std::exp(-x2 + a * std::log(x2) - logGamma(a)) * h;
}

// Sample std dev (ddof=1), matching np.std(vals, ddof=1) — 0.0 for n==1,
// null (NaN sentinel, caller substitutes JSON null) for n==0 handled by
// caller.

// numpy's pairwise (cascade) summation algorithm — reproduced EXACTLY
// (blocksize=128, 8-way-unrolled naive base case, recursive halving aligned
// to 8) so np.mean()/np.std() reproduce bit-for-bit against the Python
// oracle. A naive sequential sum diverges from numpy in the last ULP for
// N in the low hundreds (verified empirically against tabulate_summary's
// corpus cells), which is enough to break the debug_all stream-replay
// gate's byte-identical comparison even though it is well within the
// cross_impl numeric-tolerance profile — matching exactly is strictly
// better and cheap, so we do it here rather than accept the drift.
double pairwiseSum(const double* arr, size_t n) {
  if (n <= 8) {
    double s = 0.0;
    for (size_t i = 0; i < n; ++i) s += arr[i];
    return s;
  }
  if (n <= 128) {
    double r[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    size_t i = 0;
    while (i + 8 <= n) {
      for (int k = 0; k < 8; ++k) r[static_cast<size_t>(k)] += arr[i + static_cast<size_t>(k)];
      i += 8;
    }
    double s = ((r[0] + r[1]) + (r[2] + r[3])) + ((r[4] + r[5]) + (r[6] + r[7]));
    while (i < n) {
      s += arr[i];
      ++i;
    }
    return s;
  }
  size_t half = n / 2;
  half -= half % 8;
  return pairwiseSum(arr, half) + pairwiseSum(arr + half, n - half);
}

double pairwiseSum(const std::vector<double>& v) { return pairwiseSum(v.data(), v.size()); }

double mean(const std::vector<double>& vals) {
  return pairwiseSum(vals) / static_cast<double>(vals.size());
}

double sampleSd(const std::vector<double>& vals) {
  size_t n = vals.size();
  if (n <= 1) return 0.0;
  double m = mean(vals);
  std::vector<double> sq(n);
  for (size_t i = 0; i < n; ++i) {
    double d = vals[i] - m;
    sq[i] = d * d;
  }
  double ss = pairwiseSum(sq);
  return std::sqrt(ss / static_cast<double>(n - 1));
}

// A "column view" abstracting factor vs f64 columns for the grouping logic
// tabulate/table share (mirrors the oracle's dtype branch duplicated in both
// tabulate.py and table.py).
struct GroupCol {
  bool isFactor = false;
  std::vector<double> raw;         // f64 values (NaN = missing) OR factor codes as double
  std::vector<std::string> levels; // factor dictionary (empty for f64)
  // BUG-371 (`tabulate, missing`): when set, readGroupCol has already rewritten
  // every missing cell to its category key (see readGroupCol), and missing is an
  // ORDINARY CATEGORY from here on.
  bool includeMissing = false;
};

GroupCol readGroupCol(Datastore& ds, const std::string& name, bool includeMissing = false) {
  GroupCol gc;
  const cecon::Column& c = ds.col(name);
  int64_t n = ds.nRows();
  gc.isFactor = ds.dtype(name) == "factor";
  gc.includeMissing = includeMissing;
  gc.raw.resize(static_cast<size_t>(n));
  // BUG-371: a numeric missing becomes a finite key rather than staying NaN, for
  // two reasons that both matter. (1) The category set is a std::set<double>,
  // and NaN is not a valid key for a strict weak ordering. (2) Stata orders
  // missing ABOVE every number and prints it LAST.
  // BUG-1012: each missing KIND is its own category, `.` before `.a` before ... `.z`
  // (StataBE 19.5, w2as_summary_stats_family sx_tab_m_plain: x holds `.` twice and
  // `.a` once, r(r) 16 = 14 values + 2 missing rows; matrow sx_tab_m_R2 reads
  // `.` = +1.0000000000000X+3ff and `.a` = +1.0010000000000X+3ff). The key is
  // exactly the double Stata's matrow() holds for that kind
  // (eval::stataMissingWithKind), so ordering and the matrow() read-back come
  // from one representation. Before, every kind became +inf: one merged row.
  // A string column's missing is "" (code < 0): with `missing` it stays -1, an
  // ordinary category whose label is "" and which sorts first, as "" does in
  // Stata's byte order (sx_tab_gen_m: sm1 is `s==`).
  if (gc.isFactor) {
    const int32_t* codes = c.codes();
    for (int64_t i = 0; i < n; ++i) {
      const double v = static_cast<double>(codes[i]);
      gc.raw[static_cast<size_t>(i)] = (includeMissing && v < 0) ? -1.0 : v;
    }
    gc.levels = c.levels;
  } else {
    const double* d = c.f64();
    for (int64_t i = 0; i < n; ++i) {
      double v = d[i];
      if (includeMissing && std::isnan(v)) {
        const int kind = cecon::missing_kind(v);
        v = eval::stataMissingWithKind(kind < 0 ? 0 : kind);
      }
      gc.raw[static_cast<size_t>(i)] = v;
    }
  }
  return gc;
}

bool validAt(const GroupCol& gc, size_t i) {
  // With `missing`, every row is a valid row — the missing ones just belong to the
  // +inf category. This is the ONE place the three tabulate forms (one-way,
  // two-way, summary) all funnel their row filter through, so honoring the flag
  // here is what makes them agree.
  if (gc.includeMissing) return true;
  return gc.isFactor ? gc.raw[i] >= 0 : !std::isnan(gc.raw[i]);
}

// Label for a category value — factor: levels[code] (or str(code) if OOB);
// f64: pyFloatStr(v).
std::string labelFor(const GroupCol& gc, double v) {
  if (gc.isFactor) {
    int code = static_cast<int>(v);
    if (gc.includeMissing && code < 0) return "";  // a string's missing category
    if (code >= 0 && static_cast<size_t>(code) < gc.levels.size()) return gc.levels[static_cast<size_t>(code)];
    return std::to_string(code);
  }
  // BUG-371/BUG-1012: a missing category, labelled by its kind.
  if (gc.includeMissing) {
    if (auto kind = eval::stataExtendedMissingKind(v)) {
      return *kind == 0 ? std::string(".") : std::string(".") + static_cast<char>('a' + *kind - 1);
    }
  }
  return pyFloatStr(v);
}

// Sorted-unique category (value, label) pairs among valid rows.
// `weightValid`, when non-null, further restricts to rows where
// (*weightValid)[i] is true (Step-1 D3: excludes fweight-missing rows
// from the category set entirely, not just from the cell counts — a category
// that only occurs in weight-missing rows must not appear in the table at
// all, mirroring Stata's implicit `if !missing(weightvar)`). Defaulted to
// nullptr so every pre-existing (unweighted) call site is byte-identical.
std::vector<std::pair<double, std::string>> uniqueCats(
    const GroupCol& gc, const std::vector<bool>* weightValid = nullptr) {
  std::set<double> uniq;
  for (size_t i = 0; i < gc.raw.size(); ++i) {
    if (!validAt(gc, i)) continue;
    if (weightValid != nullptr && !(*weightValid)[i]) continue;
    uniq.insert(gc.raw[i]);
  }
  std::vector<std::pair<double, std::string>> out;
  out.reserve(uniq.size());
  for (double v : uniq) out.emplace_back(v, labelFor(gc, v));
  // A string variable's categories are in Stata's byte order of the strings, not
  // in the order the factor dictionary happened to assign codes.
  if (gc.isFactor) {
    std::stable_sort(out.begin(), out.end(),
                     [](const auto& a, const auto& b) { return a.second < b.second; });
  }
  return out;
}

// Step-1 D3: parallel weight-array read alongside GroupCol/readGroupCol.
// A weight column is plain numeric (Stata errors on a string/factor weight
// var), so this is deliberately simpler than GroupCol — no factor/levels
// branch. `outW` is resized to ds.nRows(); NaN marks a missing weight (caller
// drops that row from every computation, matching cobra's fweight contract:
// "missing-weight rows dropped"). Returns an error block if `weightVar`
// doesn't exist, isn't numeric, or contains a non-integer non-missing value
// (fweight must be integer) — native tabulate has no cobra call to delegate
// this validation to, so PIET must enforce it here itself.
// The four weight kinds tabulate serves. Freq keeps every pre-existing
// fweight answer byte-identical (integer cells); Analytic and Importance are
// the [aw=] and [iw=] tables of StataBE 19.5 (w2aj_qualifiers qf_tb_aw_sp,
// qf_tb_iw_sp): an aweight table's cells are Σw rescaled so the weights sum to
// the tabulated N (r(N) 12 on 12 rows), an iweight table's cells are the raw
// Σw (r(N) 28.5 = Σwf).
enum class TabWeight { None, Freq, Analytic, Importance };

TabWeight tabWeightKind(const std::optional<std::string>& weightType) {
  if (!weightType) return TabWeight::Freq;
  std::string t = *weightType;
  std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c) { return std::tolower(c); });
  if (t == "aw" || t == "aweight") return TabWeight::Analytic;
  if (t == "iw" || t == "iweight") return TabWeight::Importance;
  return TabWeight::Freq;
}

std::optional<ojson> readFweightCol(Datastore& ds, const std::string& weightVar,
                                     std::vector<double>& outW, TabWeight kind = TabWeight::Freq) {
  if (!ds.has(weightVar)) {
    ojson err;
    err["type"] = "error";
    err["code"] = "engine_error";
    err["message"] = "tabulate: weight variable not found: '" + weightVar + "'";
    return err;
  }
  if (ds.dtype(weightVar) != "f64") {
    ojson err;
    err["type"] = "error";
    err["code"] = "engine_error";
    err["message"] = "tabulate: weight variable '" + weightVar + "' must be numeric";
    return err;
  }
  const cecon::Column& c = ds.col(weightVar);
  int64_t n = ds.nRows();
  outW.assign(static_cast<size_t>(n), 0.0);
  const double* d = c.f64();
  for (int64_t i = 0; i < n; ++i) {
    double v = d[i];
    outW[static_cast<size_t>(i)] = v;
    if (std::isnan(v)) continue;  // missing weight -> row dropped by callers
    if (kind == TabWeight::Analytic || kind == TabWeight::Importance) {
      // Stata's mark: a zero weight leaves the sample (w2aj_qualifiers
      // qf_cl_aw_wzero counts 5, not 6, in the group holding the zero); a
      // negative aweight is r(402), a negative iweight is legal.
      if (v == 0.0) {
        outW[static_cast<size_t>(i)] = std::numeric_limits<double>::quiet_NaN();
      } else if (v < 0.0 && kind == TabWeight::Analytic) {
        return ojson(eval::errorBlock("engine_error", "negative weights encountered", std::nullopt,
                                      std::nullopt, std::nullopt, "r(402)"));
      }
      continue;
    }
    if (v != std::floor(v)) {
      ojson err;
      err["type"] = "error";
      err["code"] = "engine_error";
      err["message"] = "tabulate: frequency weights (fweight) must be integer";
      return err;
    }
    // A zero fweight leaves the sample too, so a category holding only
    // zero-weight rows is no row of the table (BUG-1153, StataBE: r(r) 1).
    if (v == 0.0) outW[static_cast<size_t>(i)] = std::numeric_limits<double>::quiet_NaN();
  }
  return std::nullopt;
}

// ---------------------------------------------------------------------------
// tabulate_one / tabulate_two / tabulate_summary / tab1 / tab2

// `weight`, when non-null, is a Σw-sized-to-nRows fweight array (NaN =
// missing -> row dropped): each cell's freq becomes Σw of its rows instead of
// a plain row count (Step-1 D3). Defaulted to nullptr so the unweighted
// call sites are byte-identical to before this change.
//
// `sortByFreq` (B5-D-TAB-SORT, one-way ONLY — real Stata's `sort` option
// exists only on the one-way form): measured StataBE 19.5, tests/eval/
// golden/aer_b4/b5_importcase_pins.log:103-117 — `tabulate rep78, sort`
// reorders the rows by DESCENDING frequency (3/4/5/2/1 at freq 30/18/11/8/2)
// instead of ascending category value. Defaulted to false so every
// pre-existing call site is byte-identical.
ojson tabulateOne(Datastore& ds, const std::string& varname,
                   const std::vector<double>* weight = nullptr, bool includeMissing = false,
                   bool sortByFreq = false, TabWeight kind = TabWeight::Freq) {
  GroupCol gc = readGroupCol(ds, varname, includeMissing);
  std::vector<bool> weightValid;
  const std::vector<bool>* weightValidPtr = nullptr;
  if (weight != nullptr) {
    weightValid.resize(gc.raw.size());
    for (size_t i = 0; i < gc.raw.size(); ++i) weightValid[i] = !std::isnan((*weight)[i]);
    weightValidPtr = &weightValid;
  }
  auto cats = uniqueCats(gc, weightValidPtr);
  // (label, count) built in ascending-value order first — Stata's default row
  // order, and the order `sort` below reorders FROM (not a fresh grouping).
  struct CatCount {
    std::string label;
    int64_t count;
    double wsum;  // Σw of the category (aweight/iweight tables only)
  };
  std::vector<CatCount> catCounts;
  catCounts.reserve(cats.size());
  int64_t total = 0;
  double wtotal = 0.0;
  for (const auto& [v, label] : cats) {
    int64_t cnt = 0;
    double wsum = 0.0;
    for (size_t i = 0; i < gc.raw.size(); ++i) {
      if (!validAt(gc, i) || gc.raw[i] != v) continue;
      if (weight == nullptr) {
        ++cnt;
      } else if (!std::isnan((*weight)[i])) {
        if (kind == TabWeight::Freq) {
          cnt += static_cast<int64_t>((*weight)[i]);
        } else {
          ++cnt;
          wsum += (*weight)[i];
        }
      }
    }
    catCounts.push_back({label, cnt, wsum});
    total += cnt;
    wtotal += wsum;
  }
  const bool realWeighted =
      weight != nullptr && (kind == TabWeight::Analytic || kind == TabWeight::Importance);
  // An aweight cell is its Σw rescaled to the tabulated N; an iweight cell is
  // its Σw. `count` stays the row count, so the sort below orders the shown
  // frequencies through `wsum`.
  auto shownFreq = [&](const CatCount& cc) -> double {
    if (kind == TabWeight::Analytic) return wtotal != 0.0 ? cc.wsum * static_cast<double>(total) / wtotal : 0.0;
    return cc.wsum;
  };
  // stable_sort: ties keep the ascending-value order as their tie-break. The
  // measured fixture (rep78) has no tied frequencies, so a real Stata
  // tie-break rule is not something this cell can confirm either way — this
  // is the one choice that leaves the UNSORTED row order provably unchanged.
  if (sortByFreq) {
    std::stable_sort(catCounts.begin(), catCounts.end(), [&](const CatCount& a, const CatCount& b) {
      return realWeighted ? a.wsum > b.wsum : a.count > b.count;
    });
  }
  ojson rows = ojson::array();
  ojson values = ojson::object();
  for (const auto& cc : catCounts) {
    rows.push_back(cc.label);
    if (realWeighted) {
      values[cc.label] = shownFreq(cc);
    } else {
      values[cc.label] = cc.count;
    }
  }
  ojson table;
  table["rows"] = rows;
  table["values"] = values;
  ojson margins;
  // The aweight total is the row count itself (the rescaled cells sum to it
  // only up to rounding); the iweight total is Σw.
  if (realWeighted && kind == TabWeight::Importance) {
    margins["total"] = wtotal;
  } else {
    margins["total"] = total;
  }
  table["margins"] = margins;

  ojson out;
  out["type"] = "tabulate";
  out["vars"] = ojson::array({varname});
  out["table"] = table;
  return out;
}

// `weight`: see tabulateOne's comment — same Σw-per-cell contract, applied to
// the joint (row, col) contingency table (cell/row-margin/col-margin/grand
// total all become Σw sums; chi2 below then auto-operates on the weighted
// table with no further changes needed). Defaulted to nullptr so unweighted
// call sites stay byte-identical.
ojson tabulateTwo(Datastore& ds, const std::string& rowVar, const std::string& colVar, bool chi2,
                   const std::vector<double>* weight = nullptr, bool includeMissing = false,
                   TabWeight kind = TabWeight::Freq, unsigned stats = 0) {
  GroupCol rc = readGroupCol(ds, rowVar, includeMissing);
  GroupCol cc = readGroupCol(ds, colVar, includeMissing);
  size_t n = rc.raw.size();

  std::vector<bool> weightValid;
  const std::vector<bool>* weightValidPtr = nullptr;
  if (weight != nullptr) {
    weightValid.resize(n);
    for (size_t i = 0; i < n; ++i) weightValid[i] = !std::isnan((*weight)[i]);
    weightValidPtr = &weightValid;
  }

  auto rowCats = uniqueCats(rc, weightValidPtr);
  auto colCats = uniqueCats(cc, weightValidPtr);

  // Contingency counts (Σw per cell/margin/total when weighted). Under
  // [aw=]/[iw=] the cells are real-valued: Σw per cell, and for an aweight
  // rescaled so the weights sum to the tabulated N (tabulateOne's contract).
  const bool realWeighted =
      weight != nullptr && (kind == TabWeight::Analytic || kind == TabWeight::Importance);
  if (realWeighted) {
    std::map<std::pair<double, double>, double> wc;
    std::map<double, double> wr, wcol;
    double wtot = 0.0;
    int64_t nrows = 0;
    for (size_t i = 0; i < n; ++i) {
      if (!validAt(rc, i) || !validAt(cc, i) || std::isnan((*weight)[i])) continue;
      double w = (*weight)[i];
      wc[{rc.raw[i], cc.raw[i]}] += w;
      wr[rc.raw[i]] += w;
      wcol[cc.raw[i]] += w;
      wtot += w;
      ++nrows;
    }
    const double scale =
        kind == TabWeight::Analytic ? (wtot != 0.0 ? static_cast<double>(nrows) / wtot : 0.0) : 1.0;
    ojson rowsArr = ojson::array();
    ojson colsArr = ojson::array();
    for (const auto& [v, l] : rowCats) rowsArr.push_back(l);
    for (const auto& [v, l] : colCats) colsArr.push_back(l);
    ojson values = ojson::object();
    for (const auto& [rv, rl] : rowCats) {
      ojson rowObj = ojson::object();
      for (const auto& [cv, cl] : colCats) {
        auto it = wc.find({rv, cv});
        rowObj[cl] = (it == wc.end()) ? 0.0 : it->second * scale;
      }
      values[rl] = rowObj;
    }
    ojson marginsRow = ojson::object();
    for (const auto& [rv, rl] : rowCats) marginsRow[rl] = wr.count(rv) ? wr[rv] * scale : 0.0;
    ojson marginsCol = ojson::object();
    for (const auto& [cv, cl] : colCats) marginsCol[cl] = wcol.count(cv) ? wcol[cv] * scale : 0.0;
    ojson table;
    table["rows"] = rowsArr;
    table["cols"] = colsArr;
    table["values"] = values;
    ojson margins;
    margins["row"] = marginsRow;
    margins["col"] = marginsCol;
    if (kind == TabWeight::Analytic) {
      margins["total"] = nrows;
    } else {
      margins["total"] = wtot;
    }
    table["margins"] = margins;
    ojson result;
    result["type"] = "tabulate";
    result["vars"] = ojson::array({rowVar, colVar});
    result["table"] = table;
    if (chi2) {
      // Stata: "chi2 may not be used with aweights or iweights" is not a
      // statistic this table computes; refuse by name rather than report an
      // unweighted or a rescaled-count chi2.
      return ojson(eval::unsupportedOption(
          "chi2", "`tabulate ..., chi2` with [aw=]/[iw=] is not supported in this version of PIET."));
    }
    return result;
  }

  std::map<std::pair<double, double>, int64_t> counts;
  std::map<double, int64_t> rowTotals, colTotals;
  int64_t grandTotal = 0;
  for (size_t i = 0; i < n; ++i) {
    if (!validAt(rc, i) || !validAt(cc, i)) continue;
    int64_t inc = 1;
    if (weight != nullptr) {
      if (std::isnan((*weight)[i])) continue;
      inc = static_cast<int64_t>((*weight)[i]);
    }
    double rv = rc.raw[i], cv = cc.raw[i];
    counts[{rv, cv}] += inc;
    rowTotals[rv] += inc;
    colTotals[cv] += inc;
    grandTotal += inc;
  }

  ojson rowsArr = ojson::array();
  ojson colsArr = ojson::array();
  for (const auto& [v, l] : rowCats) rowsArr.push_back(l);
  for (const auto& [v, l] : colCats) colsArr.push_back(l);

  ojson values = ojson::object();
  for (const auto& [rv, rl] : rowCats) {
    ojson rowObj = ojson::object();
    for (const auto& [cv, cl] : colCats) {
      auto it = counts.find({rv, cv});
      rowObj[cl] = (it == counts.end()) ? 0 : it->second;
    }
    values[rl] = rowObj;
  }

  ojson marginsRow = ojson::object();
  for (const auto& [rv, rl] : rowCats) marginsRow[rl] = rowTotals.count(rv) ? rowTotals[rv] : 0;
  ojson marginsCol = ojson::object();
  for (const auto& [cv, cl] : colCats) marginsCol[cl] = colTotals.count(cv) ? colTotals[cv] : 0;

  ojson table;
  table["rows"] = rowsArr;
  table["cols"] = colsArr;
  table["values"] = values;
  ojson margins;
  margins["row"] = marginsRow;
  margins["col"] = marginsCol;
  margins["total"] = grandTotal;
  table["margins"] = margins;

  ojson result;
  result["type"] = "tabulate";
  result["vars"] = ojson::array({rowVar, colVar});
  result["table"] = table;

  // BUG-1514: no Pearson chi2 for a table with one row or one column either:
  // StataBE 19.5 `tabulate one d, chi2` (1 x 3) and `tabulate c one, chi2 V`
  // (4 x 1) print no statistic and post only r(N) r(r) r(c); this posted r(chi2) 0
  // and r(p) 1.
  if (chi2 && grandTotal > 0 && rowCats.size() >= 2 && colCats.size() >= 2) {
    double stat = 0.0;
    for (const auto& [rv, rl] : rowCats) {
      for (const auto& [cv, cl] : colCats) {
        double expected = static_cast<double>(rowTotals[rv]) * static_cast<double>(colTotals[cv]) /
                           static_cast<double>(grandTotal);
        if (expected > 0) {
          auto it = counts.find({rv, cv});
          int64_t observed = (it == counts.end()) ? 0 : it->second;
          double diff = static_cast<double>(observed) - expected;
          stat += diff * diff / expected;
        }
      }
    }
    double dof = static_cast<double>((rowCats.size() - 1) * (colCats.size() - 1));
    double p = dof > 0 ? chi2Sf(stat, dof) : 1.0;
    ojson chi2Obj;
    chi2Obj["statistic"] = stat;
    chi2Obj["df"] = static_cast<int64_t>(dof);
    chi2Obj["p_value"] = p;
    result["chi2"] = chi2Obj;
  }
  // BUG-1514: lrchi2, V, gamma and taub, by [R] tabulate twoway's Methods and
  // formulas on the cells n_ij (Σw under fweights). Stata computes none of them
  // for a table with one row or one column: it prints no statistic line and
  // posts only r(N) r(r) r(c) (StataBE 19.5, `tabulate one d, chi2 lrchi2 V
  // gamma taub` on a 1 x 3 table). On the 4 x 3 probe table every value below
  // matches StataBE's to the last digit or two (r(chi2_lr) 1.5348030177726524,
  // r(CramersV) .11171749536720448, r(gamma) -.031133250311332503, r(ase_gam)
  // .17468596948023596, r(taub) -.020830078887740824, r(ase_taub)
  // .11692105991378575).
  const size_t nr = rowCats.size(), nc = colCats.size();
  if (stats != 0 && grandTotal > 0 && nr >= 2 && nc >= 2) {
    const double n = static_cast<double>(grandTotal);
    std::vector<double> cell(nr * nc, 0.0), ri(nr, 0.0), cj(nc, 0.0);
    for (size_t i = 0; i < nr; ++i) {
      for (size_t j = 0; j < nc; ++j) {
        auto it = counts.find({rowCats[i].first, colCats[j].first});
        cell[i * nc + j] = it == counts.end() ? 0.0 : static_cast<double>(it->second);
      }
      ri[i] = static_cast<double>(rowTotals[rowCats[i].first]);
    }
    for (size_t j = 0; j < nc; ++j) cj[j] = static_cast<double>(colTotals[colCats[j].first]);
    const double dof = static_cast<double>((nr - 1) * (nc - 1));
    if (stats & kTabLrchi2) {
      // G2 = 2 Σ n_ij ln(n_ij / m_ij), m_ij = n_i. n_.j / n; an empty cell adds 0.
      double g2 = 0.0;
      for (size_t i = 0; i < nr; ++i)
        for (size_t j = 0; j < nc; ++j) {
          const double o = cell[i * nc + j];
          if (o > 0) g2 += o * std::log(o / (ri[i] * cj[j] / n));
        }
      g2 *= 2.0;
      result["lrchi2"] = ojson{{"statistic", g2}, {"df", static_cast<int64_t>(dof)},
                               {"p_value", chi2Sf(g2, dof)}};
    }
    if (stats & kTabCramersV) {
      // Signed on a 2 x 2 table, (n11 n22 - n12 n21) / sqrt(n1. n2. n.1 n.2)
      // (StataBE: -.0091535337745252288 where Pearson's chi2 is .0044);
      // otherwise sqrt((X2 / n) / min(I - 1, J - 1)).
      double v;
      if (nr == 2 && nc == 2) {
        v = (cell[0] * cell[3] - cell[1] * cell[2]) / std::sqrt(ri[0] * ri[1] * cj[0] * cj[1]);
      } else {
        double x2 = 0.0;
        for (size_t i = 0; i < nr; ++i)
          for (size_t j = 0; j < nc; ++j) {
            const double m = ri[i] * cj[j] / n;
            const double d = cell[i * nc + j] - m;
            x2 += d * d / m;
          }
        v = std::sqrt((x2 / n) / static_cast<double>(std::min(nr, nc) - 1));
      }
      result["cramers_v"] = v;
    }
    if (stats & (kTabGamma | kTabTaub)) {
      // A_ij / D_ij: the counts concordant / discordant with cell (i, j), from
      // the 2-D prefix sums S[a][b] = Σ_{k<a, l<b} n_kl. P = Σ n_ij A_ij and
      // Q = Σ n_ij D_ij are twice the concordant and discordant pairs.
      std::vector<double> S((nr + 1) * (nc + 1), 0.0);
      for (size_t a = 1; a <= nr; ++a)
        for (size_t b = 1; b <= nc; ++b)
          S[a * (nc + 1) + b] = cell[(a - 1) * nc + (b - 1)] + S[(a - 1) * (nc + 1) + b] +
                                S[a * (nc + 1) + (b - 1)] - S[(a - 1) * (nc + 1) + (b - 1)];
      auto at = [&](size_t a, size_t b) { return S[a * (nc + 1) + b]; };
      std::vector<double> A(nr * nc), D(nr * nc);
      double P = 0.0, Q = 0.0;
      for (size_t i = 0; i < nr; ++i)
        for (size_t j = 0; j < nc; ++j) {
          const double below = n - at(i + 1, nc) - at(nr, j + 1) + at(i + 1, j + 1);  // k>i, l>j
          const double above = at(i, j);                                              // k<i, l<j
          const double belowLeft = at(nr, j) - at(i + 1, j);                          // k>i, l<j
          const double aboveRight = at(i, nc) - at(i, j + 1);                         // k<i, l>j
          A[i * nc + j] = below + above;
          D[i * nc + j] = belowLeft + aboveRight;
          P += cell[i * nc + j] * A[i * nc + j];
          Q += cell[i * nc + j] * D[i * nc + j];
        }
      if (stats & kTabGamma) {
        // gamma = (P - Q)/(P + Q), asymptotic variance 16 Σ n_ij (Q A_ij - P D_ij)^2 / (P + Q)^4.
        double s = 0.0;
        for (size_t k = 0; k < nr * nc; ++k) {
          const double t = Q * A[k] - P * D[k];
          s += cell[k] * t * t;
        }
        const double pq = P + Q;
        result["gamma"] = ojson{{"value", (P - Q) / pq},
                                {"ase", std::sqrt(16.0 * s / (pq * pq * pq * pq))}};
      }
      if (stats & kTabTaub) {
        // tau_b = (P - Q)/sqrt(wr wc), wr = n^2 - Σ n_i.^2, wc = n^2 - Σ n_.j^2, with
        // asymptotic variance {Σ n_ij (2 sqrt(wr wc) d_ij + tau_b v_ij)^2 - n^3 tau_b^2
        // (wr + wc)^2} / (wr wc)^2, d_ij = A_ij - D_ij, v_ij = n_i. wc + n_.j wr.
        double wr = n * n, wc = n * n;
        for (double r : ri) wr -= r * r;
        for (double c : cj) wc -= c * c;
        const double w = std::sqrt(wr * wc);
        const double tau = (P - Q) / w;
        double s = 0.0;
        for (size_t i = 0; i < nr; ++i)
          for (size_t j = 0; j < nc; ++j) {
            const double t = 2.0 * w * (A[i * nc + j] - D[i * nc + j]) + tau * (ri[i] * wc + cj[j] * wr);
            s += cell[i * nc + j] * t * t;
          }
        const double var = (s - n * n * n * tau * tau * (wr + wc) * (wr + wc)) / (wr * wc * wr * wc);
        result["taub"] = ojson{{"value", tau}, {"ase", std::sqrt(var)}};
      }
    }
    if (stats & kTabExact) {
      // Fisher's exact test, served on a 2 x 2 table: with the margins fixed, n11
      // is hypergeometric, P(a+1)/P(a) = (n1.-a)(n.1-a) / ((a+1)(n2.-n.1+a+1)). The
      // two-sided p sums the tables no more probable than the observed one (a 1e-7
      // relative tie tolerance), the one-sided p the observed table's tail: below
      // it when n11 n22 < n12 n21, above it otherwise. StataBE 19.5 on
      // [[12,18],[9,13]]: r(p_exact) .9999999999999787, r(p1_exact)
      // .5858028964541573 (the lower tail). An r x c table needs the network
      // algorithm (FEXACT), which this build does not have: refused by name.
      if (nr != 2 || nc != 2)
        return ojson(eval::unsupportedOption(
            "exact", "Fisher's exact test is served for 2 x 2 tables only; this table is " +
                         std::to_string(nr) + " x " + std::to_string(nc) + "."));
      const double r1 = ri[0], r2 = ri[1], c1 = cj[0];
      const double lo = std::max(0.0, c1 - r2), hi = std::min(r1, c1);
      std::vector<double> lw(1, 0.0);
      for (double a = lo; a < hi; a += 1.0)
        lw.push_back(lw.back() + std::log((r1 - a) * (c1 - a)) - std::log((a + 1.0) * (r2 - c1 + a + 1.0)));
      const double top = *std::max_element(lw.begin(), lw.end());
      double total = 0.0;
      for (double& x : lw) total += (x = std::exp(x - top));
      const size_t obs = static_cast<size_t>(cell[0] - lo);
      double two = 0.0, one = 0.0;
      const bool lower = cell[0] * cell[3] < cell[1] * cell[2];
      for (size_t k = 0; k < lw.size(); ++k) {
        if (lw[k] <= lw[obs] * (1.0 + 1e-7)) two += lw[k];
        if (lower ? k <= obs : k >= obs) one += lw[k];
      }
      result["exact"] = ojson{{"p", two / total}, {"p1", one / total}};
    }
  }
  return result;
}

ojson tabulateSummary(Datastore& ds, const std::string& var, const std::string& sumvar,
                       bool includeMissing = false) {
  GroupCol gc = readGroupCol(ds, var, includeMissing);
  const cecon::Column& sc = ds.col(sumvar);
  size_t n = gc.raw.size();
  std::vector<double> sumCol(n);
  {
    const double* d = sc.f64();
    for (size_t i = 0; i < n; ++i) sumCol[i] = d[i];
  }

  // group -> collected sumvar values (insertion order doesn't matter; we
  // sort categories below like the oracle's np.unique does).
  std::map<double, std::vector<double>> byGroup;
  std::vector<double> allVals;
  for (size_t i = 0; i < n; ++i) {
    if (!validAt(gc, i) || std::isnan(sumCol[i])) continue;
    byGroup[gc.raw[i]].push_back(sumCol[i]);
    allVals.push_back(sumCol[i]);
  }

  ojson rows = ojson::array();
  for (const auto& [v, vals] : byGroup) {
    ojson r;
    r["level"] = labelFor(gc, v);
    int64_t cnt = static_cast<int64_t>(vals.size());
    r["mean"] = cnt > 0 ? ojson(mean(vals)) : ojson(nullptr);
    r["sd"] = cnt > 1 ? ojson(sampleSd(vals)) : (cnt == 1 ? ojson(0.0) : ojson(nullptr));
    r["N"] = cnt;
    rows.push_back(r);
  }

  ojson overall;
  int64_t overallN = static_cast<int64_t>(allVals.size());
  overall["mean"] = overallN > 0 ? ojson(mean(allVals)) : ojson(nullptr);
  overall["sd"] = overallN > 1 ? ojson(sampleSd(allVals)) : (overallN == 1 ? ojson(0.0) : ojson(nullptr));
  overall["N"] = overallN;

  ojson out;
  out["type"] = "tabulate";
  out["mode"] = "summarize";
  out["var"] = var;
  out["sumvar"] = sumvar;
  out["rows"] = rows;
  out["overall"] = overall;
  return out;
}

// BS-027 (BETA Phase C batch C1): `tabulate v1 v2, summarize(v3)` — the
// two-way form. Previously runTabulate's sumvar branch always called
// tabulateSummary(ds, varlist[0], sumvar) regardless of how many grouping
// vars were given, silently dropping v2 entirely and returning a one-way
// table. This mirrors tabulateTwo's row/col grouping (same GroupCol/
// uniqueCats infra) but computes mean/sd/N per (row,col) cell — plus
// row/col/grand marginal stats — instead of tabulateTwo's plain frequency
// counts. Verified against the CSV directly (python, sample stdev, n-1
// denominator — same formula as sampleSd() below): 1982/high=0 income ->
// mean=13502.35147633272, sd=1675.2810011922925, N=34, matching the oracle
// exactly.
ojson tabulateSummaryTwo(Datastore& ds, const std::string& rowVar, const std::string& colVar,
                          const std::string& sumvar) {
  GroupCol rc = readGroupCol(ds, rowVar);
  GroupCol cc = readGroupCol(ds, colVar);
  const cecon::Column& sc = ds.col(sumvar);
  size_t n = rc.raw.size();
  std::vector<double> sumCol(n);
  {
    const double* d = sc.f64();
    for (size_t i = 0; i < n; ++i) sumCol[i] = d[i];
  }

  auto rowCats = uniqueCats(rc);
  auto colCats = uniqueCats(cc);

  std::map<std::pair<double, double>, std::vector<double>> cellVals;
  std::map<double, std::vector<double>> rowVals, colVals;
  std::vector<double> allVals;
  for (size_t i = 0; i < n; ++i) {
    if (!validAt(rc, i) || !validAt(cc, i) || std::isnan(sumCol[i])) continue;
    double rv = rc.raw[i], cv = cc.raw[i];
    cellVals[{rv, cv}].push_back(sumCol[i]);
    rowVals[rv].push_back(sumCol[i]);
    colVals[cv].push_back(sumCol[i]);
    allVals.push_back(sumCol[i]);
  }

  auto statObj = [](const std::vector<double>& vals) {
    ojson o;
    int64_t cnt = static_cast<int64_t>(vals.size());
    o["mean"] = cnt > 0 ? ojson(mean(vals)) : ojson(nullptr);
    o["sd"] = cnt > 1 ? ojson(sampleSd(vals)) : (cnt == 1 ? ojson(0.0) : ojson(nullptr));
    o["N"] = cnt;
    return o;
  };

  ojson rowsArr = ojson::array();
  ojson colsArr = ojson::array();
  for (const auto& [v, l] : rowCats) rowsArr.push_back(l);
  for (const auto& [v, l] : colCats) colsArr.push_back(l);

  ojson cells = ojson::object();
  for (const auto& [rv, rl] : rowCats) {
    ojson rowObj = ojson::object();
    for (const auto& [cv, cl] : colCats) {
      auto it = cellVals.find({rv, cv});
      rowObj[cl] = statObj(it == cellVals.end() ? std::vector<double>{} : it->second);
    }
    cells[rl] = rowObj;
  }

  ojson rowMargins = ojson::object();
  for (const auto& [rv, rl] : rowCats) {
    auto it = rowVals.find(rv);
    rowMargins[rl] = statObj(it == rowVals.end() ? std::vector<double>{} : it->second);
  }
  ojson colMargins = ojson::object();
  for (const auto& [cv, cl] : colCats) {
    auto it = colVals.find(cv);
    colMargins[cl] = statObj(it == colVals.end() ? std::vector<double>{} : it->second);
  }

  ojson out;
  out["type"] = "tabulate";
  // Deliberately NOT "summarize" (the one-way mode string): formatTabulate's
  // one-way summarize renderer (console.js) reads block.rows[].level/mean/
  // sd/N — a single flat list — and would misrender this compound (row,
  // col) shape. No frontend renderer for "summarize_twoway" exists yet
  // either (out of scope, piet/frontend/** forbidden this batch), but with
  // no "table" key present formatTabulate() safely falls through to its
  // honest "(no tabulation)" default instead of a plausible-looking wrong
  // rendering — the data layer is correct and ready for when a renderer is
  // added.
  out["mode"] = "summarize_twoway";
  out["vars"] = ojson::array({rowVar, colVar});
  out["sumvar"] = sumvar;
  out["rows"] = rowsArr;
  out["cols"] = colsArr;
  out["cells"] = cells;
  out["row_margins"] = rowMargins;
  out["col_margins"] = colMargins;
  out["grand"] = statObj(allVals);
  return out;
}

}  // namespace

ojson runTabulate(Datastore& ds, const std::vector<std::string>& varlist, bool chi2,
                   const std::string& cmd, const std::optional<std::string>& sumvar,
                   const std::optional<std::string>& weightVar, bool includeMissing,
                   bool sortByFreq, const std::optional<std::string>& weightType,
                   unsigned twoWayStats) {
  // Step-1 D3: native tabulate has no cobra call, so — unlike
  // summarize/collapse — PIET must read, validate, and apply the fweight
  // itself. Read once up front; `weightPtr` (non-null only on success) is
  // threaded into every branch below that computes a frequency table.
  std::vector<double> weightVals;
  const std::vector<double>* weightPtr = nullptr;
  const TabWeight kind = tabWeightKind(weightType);
  if (weightVar.has_value() && !weightVar->empty()) {
    std::optional<ojson> werr = readFweightCol(ds, *weightVar, weightVals, kind);
    if (werr.has_value()) return *werr;
    weightPtr = &weightVals;
  }
  if (sumvar.has_value() && !sumvar->empty()) {
    // The `sum()` per-category mean/sd/N variant is a separate native
    // computation (tabulateSummary) that D3's anchors didn't cover — reject
    // honestly instead of silently answering unweighted (Doctrine ①: never
    // silently drop a qualifier that was accepted upstream).
    if (weightPtr != nullptr) {
      ojson err;
      err["type"] = "error";
      err["code"] = "unsupported_option";
      err["message"] = "tabulate: weights are not yet supported with the sum() option";
      return err;
    }
    // BS-027: two-way form (`tabulate v1 v2, summarize(v3)`) — one-way
    // behavior (varlist.size() == 1) stays byte-identical to before.
    if (varlist.size() >= 2) {
      return tabulateSummaryTwo(ds, varlist[0], varlist[1], *sumvar);
    }
    return tabulateSummary(ds, varlist[0], *sumvar, includeMissing);
  }
  if (cmd == "tab1") {
    ojson tables = ojson::array();
    for (const auto& v : varlist)
      tables.push_back(tabulateOne(ds, v, weightPtr, includeMissing, sortByFreq, kind));
    ojson out;
    out["type"] = "tabulate";
    out["mode"] = "oneway_multi";
    out["tables"] = tables;
    return out;
  }
  if (cmd == "tab2") {
    ojson tables = ojson::array();
    // BUG-379: with a SINGLE variable the pair loop produces nothing, so `tab2 x`
    // returned an empty table where Stata falls back to a one-way tabulation
    // (measured: its capture on `tab2 beertax` reports r(N)=336 r(r)=302, which
    // are one-way numbers). An empty result is worse than a wrong one here — it
    // printed nothing and set no r(), so a script reading r(r) got an error for a
    // command Stata answers.
    if (varlist.size() == 1) {
      tables.push_back(tabulateOne(ds, varlist[0], weightPtr, includeMissing, sortByFreq, kind));
    }
    for (size_t i = 0; i < varlist.size(); ++i) {
      for (size_t j = i + 1; j < varlist.size(); ++j) {
        ojson t = tabulateTwo(ds, varlist[i], varlist[j], chi2, weightPtr, includeMissing, kind, twoWayStats);
        if (t.value("type", std::string()) == "error") return t;  // BUG-1514: an r x c `exact`
        tables.push_back(std::move(t));
      }
    }
    ojson out;
    out["type"] = "tabulate";
    out["mode"] = "pairwise";
    out["tables"] = tables;
    return out;
  }
  if (varlist.size() == 1) return tabulateOne(ds, varlist[0], weightPtr, includeMissing, sortByFreq, kind);
  // `sort` is a one-way-only Stata option (kVocabTabulate's comment, dispatcher.cpp):
  // accepted-but-inert here is intentional, not a silent drop of a two-way
  // semantic — see kVocabTabulate's own comment for cell/col/row/nofreq, the
  // four two-way display options in the identical position.
  return tabulateTwo(ds, varlist[0], varlist[1], chi2, weightPtr, includeMissing, kind, twoWayStats);
}

// ---------------------------------------------------------------------------
// table (table.py's run_table — aggregate statistic by cross-classification)

namespace {

std::string normalizeStat(std::string stat) {
  std::transform(stat.begin(), stat.end(), stat.begin(), [](unsigned char c) { return std::tolower(c); });
  size_t a = stat.find_first_not_of(" \t");
  size_t b = stat.find_last_not_of(" \t");
  stat = (a == std::string::npos) ? "" : stat.substr(a, b - a + 1);
  // P4 wave: `count` is NOT a synonym for `frequency` in Stata's `table`.
  // `statistic(frequency)` counts the CELL's rows; `statistic(count y)` is
  // Stata's "Number of nonmissing values" of y — different questions whenever y
  // has a missing value (the P4 answerbook cell: 524 rows but 520 non-missing).
  // Collapsing them silently answered the frequency question under the count
  // label. `n` follows `count`, since it likewise names a variable.
  if (stat == "frequency") return "freq";
  if (stat == "n") return "count";
  if (stat == "std" || stat == "stdev") return "sd";
  return stat;
}

// _stat_value: compute the requested statistic on a vector of non-missing
// values; returns nullopt for n==0 (caller emits JSON null).
std::optional<double> statValue(const std::vector<double>& vals, const std::string& stat) {
  if (vals.empty()) return std::nullopt;
  if (stat == "freq" || stat == "n" || stat == "count") return static_cast<double>(vals.size());
  if (stat == "mean") return mean(vals);
  if (stat == "sum") {
    double s = 0.0;
    for (double v : vals) s += v;
    return s;
  }
  if (stat == "min") return *std::min_element(vals.begin(), vals.end());
  if (stat == "max") return *std::max_element(vals.begin(), vals.end());
  if (stat == "sd") return vals.size() > 1 ? sampleSd(vals) : 0.0;
  return std::nullopt;
}

// table.py's _col_labels: sorted-unique label strings. For f64: integer
// values render WITHOUT the ".0" suffix ("1" not "1.0") — distinct from
// tabulate.py's convention; ported literally from table.py's `str(int(v))
// if float(v).is_integer() else str(v)`.
std::string tableF64Label(double v) {
  if (v == std::floor(v) && std::abs(v) < 1e16) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
    return std::string(buf);
  }
  return pyFloatStr(v);
}

std::string tableLabelFor(const GroupCol& gc, double v) {
  if (gc.isFactor) return labelFor(gc, v);
  return tableF64Label(v);
}

// BUG-160: a genuine factor/string column carries its own dictionary
// (GroupCol::levels, handled by tableLabelFor above) but a plain NUMERIC
// column with a Stata value label attached (e.g. `treated` 0/1 labeled
// "no"/"yes" via `label values`) has none — tableLabelFor rendered it as a
// bare numeric string ("0"/"1"), which is what the P4 oracle's column
// headers caught (`table (region) (treated)` prints "no"/"yes"). `labels`
// is session->valueLabels[varname] (int code -> text) when the caller has a
// SessionState to consult; nullptr (or a code with no entry) falls back to
// the factor/numeric rendering exactly as before, so every non-labeled call
// site is byte-identical.
std::string tableAxisLabelFor(const GroupCol& gc, double v, const std::map<int, std::string>* labels) {
  if (!gc.isFactor && labels != nullptr) {
    auto it = labels->find(static_cast<int>(v));
    if (it != labels->end()) return it->second;
  }
  return tableLabelFor(gc, v);
}

}  // namespace

// BUG-160 CLOSURE: Stata's `table` prints ONE table with every requested
// statistic nested under each row level, plus `Total` row/column margins
// and value-label-aware headers — for ONE statistic exactly as much as for
// several (verified against the P4 wave oracle, `waves/_oracle/
// p4_stata.log:271-296`, and against a live StataBE probe of the plain
// one-way `table region` case, which prints a `Total` row too even with no
// `statistic()` at all). The old code had TWO bugs, not one: the dispatcher
// looped runTable once per statistic (dispatcher.cpp, this bug's namesake),
// and runTable itself never computed a Total margin or consulted value
// labels regardless of how many statistics were asked for. Both paths are
// fixed here in ONE function so they can't independently drift again;
// runTable (below) is now a thin reshape over this with a single spec.
//
// A `Total` cell for a given axis is computed by literally collapsing that
// axis — e.g. the "Total" ROW is the one-way statistic on colvar alone,
// ignoring rowvar's validity entirely, and the grand Total/Total corner is
// the statistic over every row with a valid statvar, ignoring BOTH axes.
// This is Stata's own margin semantics (not an average-of-per-category-
// stats): confirmed by hand against the P4 fixture directly (region-1's
// Total/Total-adjacent cells: 520+435=955 nonmissing y, mean 3.136847,
// matching table region's live StataBE probe finding the same "Total" row
// on a plain one-way call with no statistic() option at all).
ojson runTableMulti(Datastore& ds, const std::string& rowvar,
                     const std::optional<std::string>& colvar,
                     const std::vector<std::pair<std::string, std::optional<std::string>>>& statSpecsIn,
                     SessionState* session) {
  if (!ds.has(rowvar)) {
    ojson err;
    err["type"] = "error";
    err["code"] = "engine_error";
    err["message"] = "table: variable not found: '" + rowvar + "'";
    return err;
  }
  if (colvar.has_value() && !ds.has(*colvar)) {
    ojson err;
    err["type"] = "error";
    err["code"] = "engine_error";
    err["message"] = "table: variable not found: '" + *colvar + "'";
    return err;
  }

  std::vector<std::pair<std::string, std::optional<std::string>>> statSpecs;
  statSpecs.reserve(statSpecsIn.size());
  for (const auto& [statIn, var] : statSpecsIn) {
    std::string stat = normalizeStat(statIn);
    if (stat != "freq" && !var.has_value()) {
      ojson err;
      err["type"] = "error";
      err["code"] = "engine_error";
      err["message"] = "table: statistic(" + stat + ") requires a variable name";
      return err;
    }
    if (var.has_value() && !ds.has(*var)) {
      ojson err;
      err["type"] = "error";
      err["code"] = "engine_error";
      err["message"] = "table: statistic variable not found: '" + *var + "'";
      return err;
    }
    statSpecs.emplace_back(stat, var);
  }

  GroupCol rc = readGroupCol(ds, rowvar);
  size_t n = rc.raw.size();

  const std::map<int, std::string>* rowValueLabels = nullptr;
  if (session != nullptr) {
    auto it = session->valueLabels.find(rowvar);
    if (it != session->valueLabels.end()) rowValueLabels = &it->second;
  }

  // Sorted-unique row labels (table.py's _col_labels: sort by underlying
  // value, not string).
  std::set<double> rowUniq;
  for (size_t i = 0; i < n; ++i) {
    if (validAt(rc, i)) rowUniq.insert(rc.raw[i]);
  }
  std::vector<std::pair<double, std::string>> rowCats;
  for (double v : rowUniq) rowCats.emplace_back(v, tableAxisLabelFor(rc, v, rowValueLabels));

  // Each distinct statvar's f64 array, read once regardless of how many
  // statSpecs reference it (mirrors the old single-read behavior).
  std::map<std::string, std::vector<double>> statArrs;
  for (const auto& [stat, var] : statSpecs) {
    if (var.has_value() && statArrs.find(*var) == statArrs.end()) {
      const cecon::Column& sc = ds.col(*var);
      const double* d = sc.f64();
      statArrs.emplace(*var, std::vector<double>(d, d + n));
    }
  }

  // One cell: `stat` over the rows selected by BOTH predicates. statValue's
  // contract is a vector of NON-missing values (BUG-161: pushing the raw
  // value let one missing y poison the whole cell into NaN/null).
  auto cellValue = [&](const std::string& stat, const std::optional<std::string>& var,
                       const std::function<bool(size_t)>& rowPred,
                       const std::function<bool(size_t)>& colPred) -> ojson {
    int64_t nCell = 0;
    std::vector<double> vals;
    const std::vector<double>* arr = var.has_value() ? &statArrs.at(*var) : nullptr;
    for (size_t i = 0; i < n; ++i) {
      if (!rowPred(i) || !colPred(i)) continue;
      ++nCell;
      if (arr != nullptr && !std::isnan((*arr)[i])) vals.push_back((*arr)[i]);
    }
    if (stat == "freq") return nCell > 0 ? ojson(static_cast<double>(nCell)) : ojson(nullptr);
    auto sv = nCell > 0 ? statValue(vals, stat) : std::nullopt;
    return sv.has_value() ? ojson(*sv) : ojson(nullptr);
  };
  auto alwaysTrue = [](size_t) { return true; };

  ojson statsArr = ojson::array(), statvarsArr = ojson::array();
  for (const auto& [stat, var] : statSpecs) {
    statsArr.push_back(stat);
    statvarsArr.push_back(var.has_value() ? ojson(*var) : ojson(nullptr));
  }

  ojson cells = ojson::object();
  int64_t nObs = 0;

  if (colvar.has_value()) {
    GroupCol cc = readGroupCol(ds, *colvar);
    const std::map<int, std::string>* colValueLabels = nullptr;
    if (session != nullptr) {
      auto it = session->valueLabels.find(*colvar);
      if (it != session->valueLabels.end()) colValueLabels = &it->second;
    }
    std::set<double> colUniq;
    for (size_t i = 0; i < n; ++i) {
      if (validAt(cc, i)) colUniq.insert(cc.raw[i]);
    }
    std::vector<std::pair<double, std::string>> colCats;
    for (double v : colUniq) colCats.emplace_back(v, tableAxisLabelFor(cc, v, colValueLabels));

    auto emitRow = [&](const std::string& rowLabel, const std::function<bool(size_t)>& rowPred) {
      ojson rowObj = ojson::object();
      for (const auto& [stat, var] : statSpecs) {
        ojson statRow = ojson::object();
        for (const auto& colCat : colCats) {
          double cv = colCat.first;
          const std::string& cl = colCat.second;
          auto colPred = [&cc, cv](size_t i) { return validAt(cc, i) && cc.raw[i] == cv; };
          statRow[cl] = cellValue(stat, var, rowPred, colPred);
        }
        // Total column: collapse colvar entirely (no requirement on cc's
        // validity), matching the row-margin computation below.
        statRow["Total"] = cellValue(stat, var, rowPred, alwaysTrue);
        rowObj[stat] = statRow;
      }
      cells[rowLabel] = rowObj;
    };
    for (const auto& rowCat : rowCats) {
      double rv = rowCat.first;
      const std::string& rl = rowCat.second;
      emitRow(rl, [&rc, rv](size_t i) { return validAt(rc, i) && rc.raw[i] == rv; });
    }
    // Total row: collapse rowvar entirely.
    emitRow("Total", alwaysTrue);

    for (size_t i = 0; i < n; ++i) {
      if (validAt(rc, i) && validAt(cc, i)) ++nObs;
    }

    ojson rowsArr = ojson::array(), colsArr = ojson::array();
    for (const auto& [v, l] : rowCats) rowsArr.push_back(l);
    rowsArr.push_back("Total");
    for (const auto& [v, l] : colCats) colsArr.push_back(l);
    colsArr.push_back("Total");

    ojson out;
    out["type"] = "table_result";
    out["rowvar"] = rowvar;
    out["colvar"] = *colvar;
    out["stats"] = statsArr;
    out["statvars"] = statvarsArr;
    out["row_labels"] = rowsArr;
    out["col_labels"] = colsArr;
    out["cells"] = cells;
    out["n_obs"] = nObs;
    return out;
  }

  // Oneway: Total row collapses rowvar entirely (same alwaysTrue predicate
  // used as the "no column dimension" colPred throughout).
  for (const auto& rowCat : rowCats) {
    double rv = rowCat.first;
    const std::string& rl = rowCat.second;
    auto rowPred = [&rc, rv](size_t i) { return validAt(rc, i) && rc.raw[i] == rv; };
    ojson rowObj = ojson::object();
    for (const auto& [stat, var] : statSpecs) rowObj[stat] = cellValue(stat, var, rowPred, alwaysTrue);
    cells[rl] = rowObj;
  }
  {
    ojson rowObj = ojson::object();
    for (const auto& [stat, var] : statSpecs) rowObj[stat] = cellValue(stat, var, alwaysTrue, alwaysTrue);
    cells["Total"] = rowObj;
  }
  for (size_t i = 0; i < n; ++i) {
    if (validAt(rc, i)) ++nObs;
  }

  ojson rowsArr = ojson::array();
  for (const auto& [v, l] : rowCats) rowsArr.push_back(l);
  rowsArr.push_back("Total");

  ojson out;
  out["type"] = "table_result";
  out["rowvar"] = rowvar;
  out["colvar"] = nullptr;
  out["stats"] = statsArr;
  out["statvars"] = statvarsArr;
  out["row_labels"] = rowsArr;
  out["col_labels"] = nullptr;
  out["cells"] = cells;
  out["n_obs"] = nObs;
  return out;
}

// Thin reshape over runTableMulti for the one-statistic caller: same Total-
// margin/label-header fix, flattened back to the legacy single-stat shape
// (`stat`/`statvar` singular keys, `cells[row]` — or `cells[row][col]` —
// holding the value directly rather than nested one level deeper under a
// stat name) so every pre-existing one-statistic consumer keeps reading the
// same shape it always has, just with the two bugs fixed.
ojson runTable(Datastore& ds, const std::string& rowvar, const std::optional<std::string>& colvar,
               const std::string& statIn, const std::optional<std::string>& statvar,
               SessionState* session) {
  ojson multi = runTableMulti(ds, rowvar, colvar, {{statIn, statvar}}, session);
  if (multi.value("type", std::string()) != "table_result") return multi;  // error block, verbatim

  const std::string stat = multi["stats"][0].get<std::string>();
  ojson cells = ojson::object();
  for (auto it = multi["cells"].begin(); it != multi["cells"].end(); ++it) {
    cells[it.key()] = it.value()[stat];
  }

  ojson out;
  out["type"] = "table_result";
  out["rowvar"] = multi["rowvar"];
  out["colvar"] = multi["colvar"];
  out["statvar"] = multi["statvars"][0];
  out["stat"] = stat;
  out["row_labels"] = multi["row_labels"];
  out["col_labels"] = multi["col_labels"];
  out["cells"] = cells;
  out["n_obs"] = multi["n_obs"];
  return out;
}

}  // namespace piet::handlers
