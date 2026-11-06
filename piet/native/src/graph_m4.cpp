// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// M4a item 4: graph JSON surface — C++ port of piet/backend/piet/graph.py's
// run_scatter/run_histogram/run_kdensity/run_line/run_xtline/run_tsline,
// and dispatcher.py's _run_marginsplot. Block STRUCTURE/JSON only for the
// live console — the frontend renders SVG from these blocks client-side
// (graph.js) exactly as before the M4b transport swap (Doctrine §3, ported
// verbatim from graph.py's own module docstring).
//
// M4b addition (below the M4a marginsplot section): `graph save`/`graph
// combine`/`graph export` — the SVG *structural* builder here is a direct
// C++ port of graph_export.py's `_build_svg*` family (same geometry
// constants/tick math), used for `graph export FILE.svg` (server-side file
// write, as Stata's `graph export` always is) and reused by `graph_combine`
// blocks. `.png`/`.pdf` stay a documented `piet_gap` (matplotlib-identical
// rendering is out of reach without bundling matplotlib).
#include "piet/handlers.h"

#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <unordered_set>

#include "piet/cobra_runner.h"
#include "piet/dispatcher.h"
#include "piet/display_hardening.h"
#include "piet/eval/errors.h"
#include "piet/eval/macros.h"
#include "piet/inrange_filter.h"
#include "piet/mutation_result.h"
#include "piet/translate.h"
#include "piet/twoway_opts.h"
#include "piet/twoway_parse.h"
#include "piet/scratch_seq.h"
#include "piet/strutil.h"
#include "io/win_compat.h"  // cecon::native_exists / to_native_path (BUG-050 wide open)

// Stata's macro real-to-text rule (%18.0g text), defined in eval/macros.cpp
// beside the local/global store that applies it (loops.cpp declares it alike).
namespace piet::eval {
std::string stataMacroRealText(double d);
// eval/exprfns.cpp: Stata's invttail(df, p) (serset-twoway FIT's CI multiplier).
double stataInvttail(double df, double p);
}

namespace piet::handlers {

using ojson = nlohmann::ordered_json;

namespace {

ojson engineErrorBlockG(const std::string& message, const std::string& code = "engine_error") {
  return eval::engineError(message, code);
}

// _numeric_column: f64 columns as-is (NaN = missing); factor columns as
// their integer codes cast to double, with code<0 (missing) -> NaN.
std::vector<double> numericColumn(Datastore& ds, const std::string& name) {
  const cecon::Column& c = ds.col(name);
  int64_t n = ds.nRows();
  std::vector<double> out(static_cast<size_t>(n));
  if (ds.dtype(name) == "f64") {
    const double* d = c.f64();
    for (int64_t i = 0; i < n; ++i) out[static_cast<size_t>(i)] = d[i];
  } else {
    const int32_t* codes = c.codes();
    for (int64_t i = 0; i < n; ++i) {
      out[static_cast<size_t>(i)] = codes[i] < 0 ? std::nan("") : static_cast<double>(codes[i]);
    }
  }
  return out;
}

std::optional<std::string> optStr(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                   const std::string& key) {
  for (const auto& [k, v] : opts) {
    if (k == key && !v.isBool) return v.strValue;
  }
  return std::nullopt;
}

std::optional<bool> optBool(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                             const std::string& key) {
  for (const auto& [k, v] : opts) {
    if (k == key) {
      if (v.isBool) return v.boolValue;
      return true;  // a valued option is still "truthy" for a boolean check
    }
  }
  return std::nullopt;
}

// BS-052 (BETA E): `by(groupvar)` real Stata faceting (multiple small-
// multiple panels split by a grouping variable) has no renderer anywhere in
// PIET -- previously every graph command simply never read the `by` key, so
// `scatter y x, by(foreign)` silently rendered ONE ungrouped panel and threw
// the grouping away with no signal at all. Honest-reject it by name instead
// (Doctrine (1): an explicit gap, never a silently different answer) --
// shared by every bare graph-command entry point AND runTwowayLayers' global
// options below, since `by()` is always a whole-graph option, never a
// per-layer one.
std::optional<ojson> rejectByFacet(const std::vector<std::pair<std::string, eval::OptValue>>& opts) {
  for (const auto& [k, v] : opts) {
    if (k == "by") {
      return ojson(eval::unsupportedOption(
          "by()",
          "Graph faceting (splitting into multiple panels by a grouping variable) is not yet "
          "supported in PIET -- run a separate graph command per group instead."));
    }
  }
  return std::nullopt;
}

// U1 (BS-008) + U2 (BS-009): title()/subtitle()/xtitle()/ytitle()/note()
// text extraction. Stata's grammar is `title("text"[, suboptions])` --
// tokenizer.cpp's parseOpts (the shared quote/paren-aware option grammar)
// captures the WHOLE parenthesised content verbatim, so e.g. opts["title"]
// is literally `"text", suboptions` today: the unstripped quote characters
// AND any glued sub-option both reach the graph block and thence the
// exported SVG (BS-008 evidence: "corrupts ~100% of idiomatic quoted
// titles"). This is the ONE shared consumer (BS-099: "single title/axis/
// legend/name/note consumer") every title/subtitle/xtitle/ytitle/note
// callsite below routes through instead of raw optStr().
std::string dequoteTitleText(std::string s) {
  size_t a = s.find_first_not_of(" \t");
  if (a == std::string::npos) return std::string();
  size_t b = s.find_last_not_of(" \t");
  s = s.substr(a, b - a + 1);

  if (!s.empty() && (s.front() == '"' || s.front() == '\'')) {
    char q = s.front();
    size_t close = s.find(q, 1);
    if (close != std::string::npos) return s.substr(1, close - 1);
    // Unterminated quote (parseOpts is paren-balanced, not quote-aware) --
    // fall through to the bare-token handling below.
  }
  // Bare/unquoted value (or unterminated-quote fallback): still strip a
  // glued `, suboption(...)` tail at the first top-level comma so e.g.
  // `title(Wages, size(large))` doesn't leak `size(large)` into the text.
  size_t comma = s.find(',');
  if (comma != std::string::npos) s = s.substr(0, comma);
  a = s.find_first_not_of(" \t");
  if (a == std::string::npos) return std::string();
  b = s.find_last_not_of(" \t");
  return s.substr(a, b - a + 1);
}

// title/subtitle/xtitle/ytitle/note all share the exact same dequote +
// glued-suboption-strip rule -- one accessor for all five keys.
std::optional<std::string> optTitleStr(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                        const std::string& key) {
  auto raw = optStr(opts, key);
  if (!raw.has_value()) return std::nullopt;
  return dequoteTitleText(*raw);
}

// Stata's name(X, replace)/name(X): take the first comma/whitespace-
// delimited token as the registered name, discarding any glued suboption
// (e.g. `replace` -- BS-057). Mirrors dispatcher.cpp's identical twoway-
// path name() split (this codebase's per-TU convention: the same small
// parsing rule copied per call site rather than factored into a shared
// header -- see e.g. nextPathG's identical per-file duplication note above).
std::string firstNameToken(const std::string& raw) {
  static const std::regex reNameSplit(R"([,\s]+)");
  std::sregex_token_iterator it(raw.begin(), raw.end(), reNameSplit, -1), endIt;
  return (it != endIt) ? it->str() : raw;
}

}  // namespace

// ===========================================================================
// scatter

ojson runScatter(Datastore& ds, const std::vector<std::string>& varlist,
                 const std::vector<std::pair<std::string, eval::OptValue>>& opts) {
  if (varlist.size() < 2) {
    return engineErrorBlockG("scatter requires two variables: `scatter yvar xvar`.", "syntax_error");
  }
  if (auto err = rejectByFacet(opts)) return *err;

  // BS-050 (BETA E): Stata's `scatter y1 y2 ... x` puts the SHARED x
  // variable LAST -- every token before it is an independent y-series
  // plotted against that same x (previously only varlist[0]/varlist[1] were
  // ever read, so `scatter y1 y2 x` silently rendered y1-vs-y2 and dropped
  // the real x entirely). varlist.size()==2 is the single-series case and
  // keeps the EXACT pre-existing flat JSON shape (points/x_var/y_var)
  // untouched below -- only 3+ tokens engage the new "series" array, so no
  // existing consumer (buildSvgScatter's legacy branch, BS-010's selftest,
  // the not-yet-updated frontend) sees a shape change for the common case.
  std::string xvar = varlist.back();
  std::vector<std::string> yvars(varlist.begin(), varlist.end() - 1);
  for (const auto& yv : yvars) {
    if (!ds.has(yv)) return engineErrorBlockG("variable " + yv + " not found");
  }
  if (!ds.has(xvar)) return engineErrorBlockG("variable " + xvar + " not found");

  auto x = numericColumn(ds, xvar);
  double xMin = 0, xMax = 0, yMin = 0, yMax = 0;
  bool first = true;
  int64_t nTotal = 0;

  ojson out;
  out["type"] = "graph";
  out["kind"] = "scatter";

  if (yvars.size() == 1) {
    auto y = numericColumn(ds, yvars[0]);
    ojson points = ojson::array();
    for (size_t i = 0; i < y.size(); ++i) {
      if (!std::isfinite(y[i]) || !std::isfinite(x[i])) continue;
      double xv = x[i], yv = y[i];
      points.push_back(ojson::array({xv, yv}));
      if (first) {
        xMin = xMax = xv;
        yMin = yMax = yv;
        first = false;
      } else {
        xMin = std::min(xMin, xv);
        xMax = std::max(xMax, xv);
        yMin = std::min(yMin, yv);
        yMax = std::max(yMax, yv);
      }
      ++nTotal;
    }
    out["x_var"] = xvar;
    out["y_var"] = yvars[0];
    out["points"] = points;
    out["n"] = nTotal;
  } else {
    ojson seriesArr = ojson::array();
    for (size_t si = 0; si < yvars.size(); ++si) {
      auto y = numericColumn(ds, yvars[si]);
      ojson points = ojson::array();
      int64_t nSeries = 0;
      for (size_t i = 0; i < y.size(); ++i) {
        if (!std::isfinite(y[i]) || !std::isfinite(x[i])) continue;
        double xv = x[i], yv = y[i];
        points.push_back(ojson::array({xv, yv}));
        if (first) {
          xMin = xMax = xv;
          yMin = yMax = yv;
          first = false;
        } else {
          xMin = std::min(xMin, xv);
          xMax = std::max(xMax, xv);
          yMin = std::min(yMin, yv);
          yMax = std::max(yMax, yv);
        }
        ++nSeries;
        ++nTotal;
      }
      ojson s;
      s["y_var"] = yvars[si];
      s["points"] = points;
      s["style"] = twoway::layerStyle(opts, static_cast<int>(si), "scatter");
      s["n"] = nSeries;
      seriesArr.push_back(s);
    }
    out["x_var"] = xvar;
    out["y_var"] = yvars[0];  // first series, for any single-y-shaped reader
    out["y_vars"] = yvars;    // full typed-order list
    out["series"] = seriesArr;
    // Backward-compat flat "points": the union of every series, so a
    // consumer that only reads the flat field (an as-yet-unupdated
    // renderer) still degrades gracefully to "every point plotted" instead
    // of an empty graph.
    ojson flatPoints = ojson::array();
    for (const auto& s : seriesArr) {
      for (const auto& p : s["points"]) flatPoints.push_back(p);
    }
    out["points"] = flatPoints;
    out["n"] = nTotal;
  }

  std::string joinedY;
  for (size_t i = 0; i < yvars.size(); ++i) {
    if (i) joinedY += " ";
    joinedY += yvars[i];
  }
  out["title"] = optTitleStr(opts, "title").value_or(joinedY + " vs " + xvar);
  out["x_label"] = optTitleStr(opts, "xtitle").value_or(xvar);
  out["y_label"] = optTitleStr(opts, "ytitle").value_or(yvars[0]);
  out["note"] = optTitleStr(opts, "note").value_or(std::string());
  out["x_min"] = xMin;
  out["x_max"] = xMax;
  out["y_min"] = yMin;
  out["y_max"] = yMax;
  out["name"] = nullptr;  // filled by the dispatcher's name() capture (BS-010)
  return out;
}

// ===========================================================================
// histogram, twoway__histogram_gen and kdensity
//
// gfx-histogram-kdensity-family (oracle tests/eval/golden/aer_w2/
// w2aq_histogram_family, StataBE 19.5). One binning rule (histogramGen) and one
// density rule (kdensityEstimate), per [R] histogram and [R] kdensity, Methods
// and formulas; every caller reads them.

namespace {

// Stata's sysmiss as the hk_r records read it (+1.0000000000000X+3ff).
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// A refusal with the oracle's own rc (errors.h errorBlock, reason "r(#)": the
// shape coarseRc() honours, so `display _rc` is never 1).
ojson stataRefusal(const std::string& message, int rc) {
  return ojson(eval::errorBlock(rc == 198 ? "syntax_error" : "engine_error", message, std::nullopt,
                                std::nullopt, std::nullopt, "r(" + std::to_string(rc) + ")"));
}

// The LAST occurrence of an option: Stata's syntax keeps the last of a
// repeated option (hk_h_dupw2: width(0.5) ... width(2) gives r(width) 2).
std::optional<std::string> optLast(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                   const std::string& key) {
  std::optional<std::string> out;
  for (const auto& [k, v] : opts) {
    if (k == key) out = v.isBool ? std::string() : v.strValue;
  }
  return out;
}

bool hasOpt(const std::vector<std::pair<std::string, eval::OptValue>>& opts, const std::string& key) {
  for (const auto& [k, v] : opts) {
    if (k == key && (!v.isBool || v.boolValue)) return true;
  }
  return false;
}

std::string trimG(const std::string& s) {
  size_t a = s.find_first_not_of(" \t");
  if (a == std::string::npos) return std::string();
  size_t b = s.find_last_not_of(" \t");
  return s.substr(a, b - a + 1);
}

std::vector<std::string> splitWsG(const std::string& s) {
  std::istringstream is(s);
  std::vector<std::string> out;
  std::string t;
  while (is >> t) out.push_back(t);
  return out;
}

// A whole-text number (`1.5`, `-4`, `.75`), or nullopt.
std::optional<double> parseNumberG(const std::string& raw) {
  const std::string s = trimG(raw);
  if (s.empty()) return std::nullopt;
  char* end = nullptr;
  const double d = std::strtod(s.c_str(), &end);
  if (end == s.c_str() || *end != '\0' || !std::isfinite(d)) return std::nullopt;
  return d;
}

double storeAs(double v, bool asFloat) {
  if (!asFloat || std::isnan(v)) return v;
  return static_cast<double>(static_cast<float>(v));
}

// Stata's reldif(x, y) = |x - y| / (|y| + 1).
double reldifG(double x, double y) { return std::fabs(x - y) / (std::fabs(y) + 1.0); }

// The variable label, else the name (kdensity's x-axis title).
std::string varTitleG(SessionState& session, const std::string& v) {
  session.syncEvalVarLabels();
  std::string l = session.eval.varLabels.get(v, std::string());
  return l.empty() ? v : l;
}

std::string displayFormatOf(SessionState& session, Datastore& ds, const std::string& var) {
  if (ds.has(var)) {
    const std::vector<VarInfo> card = ds.formatCard(std::vector<std::string>{var});
    if (!card.empty()) return session.displayFormat(card.front());
  }
  VarInfo bare;
  bare.name = var;
  return session.displayFormat(bare);
}

// The weight word as the tokenizer keeps it (`fw`, `fweight`, ...) -> the full
// type name, or "" when there is none.
std::string weightTypeName(const eval::ParsedCommand& parsed) {
  if (!parsed.weightType.has_value()) return std::string();
  std::string w = strutil::lower(*parsed.weightType);
  if (w == "fw" || w == "fweight") return "fweight";
  if (w == "aw" || w == "aweight") return "aweight";
  if (w == "pw" || w == "pweight") return "pweight";
  if (w == "iw" || w == "iweight") return "iweight";
  return w;
}

// The estimation sample of one of these commands over `ds`: rows where the
// variable is nonmissing and, under a weight, the weight is nonmissing and
// nonzero (marksample: zero weights are out unless zeroweight). `rows` holds
// each kept row's index in `ds`.
struct GraphSample {
  std::vector<double> v;
  std::vector<double> w;  // empty when unweighted
  std::vector<int64_t> rows;
  std::string wtype;      // "" | fweight | aweight | iweight
  std::optional<ojson> err;
};

GraphSample graphSample(Datastore& ds, const std::string& var, const eval::ParsedCommand& parsed,
                        const std::string& wtype) {
  GraphSample s;
  s.wtype = wtype;
  const std::vector<double> col = numericColumn(ds, var);
  std::vector<double> wcol;
  if (!wtype.empty()) {
    const std::string wvar = trimG(parsed.weightVar.value_or(std::string()));
    if (!ds.has(wvar)) {
      s.err = stataRefusal("variable " + wvar + " not found", 111);
      return s;
    }
    if (ds.dtype(wvar) == "factor") {
      s.err = stataRefusal("type mismatch", 109);
      return s;
    }
    wcol = numericColumn(ds, wvar);
  }
  for (size_t i = 0; i < col.size(); ++i) {
    if (std::isnan(col[i])) continue;
    double wi = 1.0;
    if (!wtype.empty()) {
      wi = wcol[i];
      if (std::isnan(wi) || wi == 0.0) continue;
      if (wtype == "fweight" && wi != std::floor(wi)) {
        s.err = stataRefusal("may not use noninteger frequency weights", 401);
        return s;
      }
      if (wtype != "iweight" && wi < 0) {
        s.err = stataRefusal("negative weights encountered", 402);
        return s;
      }
    }
    s.v.push_back(col[i]);
    if (!wtype.empty()) s.w.push_back(wi);
    s.rows.push_back(static_cast<int64_t>(i));
  }
  return s;
}

// Writes one new numeric variable at the end of the session frame: the values
// as stored (float-rounded already when `stataType` is float), its storage
// type claim, display format and label.
std::optional<ojson> writeGenVariable(SessionState& session, const std::string& name,
                                      const std::vector<double>& vals, const std::string& stataType,
                                      const std::string& format, const std::string& label) {
  const std::string hbin = mintScratchPath(session.workdir(), "histgen");
  forgetScratchMint(hbin);
  const std::string csv = hbin.substr(0, hbin.size() - 5) + ".csv";  // 5 == strlen(".hbin")
  registerScratchMint(csv);
  {
    std::ofstream f(cecon::to_native_path(csv));
    if (!f) return stataRefusal("could not write the values payload", 693);
    f.setf(std::ios::fmtflags(0), std::ios::floatfield);
    f.precision(17);
    for (double d : vals) {
      if (!std::isnan(d)) f << d;
      f << "\n";
    }
    if (!f) return stataRefusal("could not write the values payload", 693);
  }
  const std::string out = mintScratchPath(session.workdir(), "histgen");
  const CobraRunResult r = runCobra({"setcol", "--data", *session.currentHbin(), "--var", name,
                                     "--values", csv, "--dtype", "f64", "--new", "--out", out});
  cecon::native_unlink(csv);
  if (!r.ok || !cecon::native_exists(out)) {
    return ojson(eval::engineError("could not create " + name + " (disk full or I/O error)", "data_io"));
  }
  session.load(out);
  session.implicitFloatVars.erase(name);
  session.varMeta[name].stataType = stataType;
  session.varMeta[name].format = format;
  session.setVarLabel(name, label);
  return std::nullopt;
}

// `confirm new var` for each name: an existing variable is r(110).
// BUG-1120 review: a reserved name is refused by the new-variable rule --
// `kdensity x, gen(_cons d)' "_cons invalid varname" r(198) -- or, where
// generate() is read as a newvarlist (`optionList`, twoway__histogram_gen), with
// if/in/using/with taken for qualifiers ("if not allowed" r(101); StataBE 19.5).
std::optional<ojson> confirmNewVars(SessionState& session, const std::vector<std::string>& names,
                                    bool optionList = false) {
  Datastore* ds = session.datastore();
  for (const auto& n : names) {
    if (!strutil::isValidStataIdentifier(n)) return stataRefusal(n + " invalid name", 198);
    if (auto r = optionList ? strutil::newVarOptionRefusal(n) : strutil::newVarNameRefusal(n))
      return stataRefusal(r->message, r->rc);
    if (ds != nullptr && ds->has(n)) return stataRefusal("variable " + n + " already defined", 110);
  }
  return std::nullopt;
}

// A number as twoway__histogram_gen's display note prints it: the most
// significant digits (at most 8) that fit nine characters once a fraction's
// leading 0 is dropped -- (bin=13, start=.125, width=.91346154) and (start=0,
// width=.33333325) (w2aq_histogram_family.log:556, :1699). piet::fmtStataDefaultG
// counts that 0 and prints .9134615 / .3333333 (a follow-up of this goal), so it
// is not used here; whole numbers and 9-character values agree with it.
std::string displayNum(double v) {
  if (std::isnan(v)) return ".";
  char buf[64];
  for (int prec = 8; prec >= 1; --prec) {
    std::snprintf(buf, sizeof buf, "%.*g", prec, v);
    std::string t(buf);
    const size_t signLen = (!t.empty() && t[0] == '-') ? 1 : 0;
    if (t.compare(signLen, 2, "0.") == 0) t.erase(signLen, 1);
    if (t.size() - signLen <= 9) return t;
  }
  return piet::fmtStataDefaultG(v);
}

}  // namespace

namespace {

// Total weight and the two extremes of a binning sample (unit weights when no
// weight vector is given).
struct BinExtent {
  double total = 0.0;
  double lo = std::numeric_limits<double>::infinity();
  double hi = -std::numeric_limits<double>::infinity();
};

BinExtent binExtent(const std::vector<double>& v, const std::vector<double>* w) {
  BinExtent e;
  for (size_t i = 0; i < v.size(); ++i) {
    e.total += (w != nullptr) ? (*w)[i] : 1.0;
    if (v[i] < e.lo) e.lo = v[i];
    if (v[i] > e.hi) e.hi = v[i];
  }
  return e;
}

// [R] histogram's default number of bins, k = min{sqrt(N), 10 ln(N)/ln(10)},
// truncated and at least 1. The log term is formed from natural logs as the
// manual writes it: StataBE gives 29 bins at N = 1000 and 59 at N = 1e6, where
// log10(N) would give 30 and 60 (measured on StataBE: bins1000, bins1e6fw).
int64_t defaultBinCount(double n) {
  const double viaRoot = std::sqrt(n);
  const double viaLog = 10.0 * std::log(n) / std::log(10.0);
  const auto k = static_cast<int64_t>(std::min(viaRoot, viaLog));
  return k < 1 ? 1 : k;
}

// The discrete bar width: the smallest positive step between neighbouring
// sorted values. Under c(type) float the values and the step are float
// quantities, so non-integer data give a float width (hk_h_discnonint_tw:
// .33333325 = float(7/3) - 2). No positive step means width 1.
double smallestStep(const std::vector<double>& v, bool asFloat) {
  std::vector<double> sorted(v);
  std::sort(sorted.begin(), sorted.end());
  double step = std::numeric_limits<double>::infinity();
  for (size_t i = 1; i < sorted.size(); ++i) {
    const double gap = storeAs(storeAs(sorted[i], asFloat) - storeAs(sorted[i - 1], asFloat), asFloat);
    if (gap > 0.0 && gap < step) step = gap;
  }
  return std::isfinite(step) ? step : 1.0;
}

// r(area): the factor that turns a density into the chosen height unit.
double heightUnitArea(const std::string& type, double total, double width) {
  if (type == "frequency") return total * width;
  if (type == "fraction") return width;
  if (type == "percent") return 100.0 * width;
  return 1.0;
}

// A bar's height from its stored count: density bars integrate to 1, fraction
// heights sum to 1, percent heights to 100, frequency is the count itself.
double barHeight(const std::string& type, double count, double total, double width) {
  if (type == "density") return count / (total * width);
  if (type == "fraction") return count / total;
  if (type == "percent") return 100.0 * count / total;
  return count;
}

}  // namespace

HistGenResult histogramGen(const std::vector<double>& v, const std::vector<double>* w,
                           const HistGenSpec& spec, int64_t nDataRows, bool wantGenerate) {
  HistGenResult res;
  res.type = spec.type;
  const bool asFloat = spec.genFloat;
  const BinExtent ext = binExtent(v, w);

  if (v.empty()) {
    // What Stata posts for an empty sample (hk_thg_empty_r): N and bin are 1,
    // the rest 0; the start() check is not reached.
    res.empty = true;
    res.area = res.max = res.min = res.start = res.width = 0.0;
    res.bin = 1;
    res.N = 1.0;
    res.nx = 1;
    return res;
  }

  // A constant variable without width() gets bars of width 1.
  std::optional<double> fixedWidth = spec.width;
  if (!fixedWidth.has_value() && ext.lo == ext.hi) fixedWidth = 1.0;

  // start() may sit at or below the minimum, or above it by a relative
  // difference of at most 1e-5 (probe startnear). It moves the lower edge of a
  // continuous histogram; for a discrete one it only changes r(start).
  double lowerEdge = ext.lo;
  double shownStart = ext.lo;
  if (spec.start.has_value()) {
    const double requested = *spec.start;
    if (ext.lo < requested && reldifG(ext.lo, requested) > 1e-5) {
      res.rc = 198;
      res.message = "option start() may not be larger than minimum of " + spec.varname;
      return res;
    }
    res.userStart = true;
    shownStart = requested;
    if (!spec.discrete) lowerEdge = requested;
  }

  double barWidth = 1.0;
  int64_t nBars = 1;
  if (spec.discrete) {
    // Bars centred on the values: the edge sits half a width below the minimum.
    barWidth = fixedWidth.has_value() ? *fixedWidth : smallestStep(v, asFloat);
    lowerEdge = ext.lo - barWidth / 2.0;
    nBars = static_cast<int64_t>((ext.hi - lowerEdge) / barWidth + 0.5);
  } else if (fixedWidth.has_value()) {
    barWidth = *fixedWidth;
    nBars = std::max<int64_t>(1, static_cast<int64_t>(std::ceil((ext.hi - lowerEdge) / barWidth)));
  } else {
    nBars = spec.bins.has_value() ? *spec.bins : defaultBinCount(ext.total);
    barWidth = (ext.hi - lowerEdge) / static_cast<double>(nBars);
  }

  res.area = heightUnitArea(spec.type, ext.total, barWidth);
  res.min = lowerEdge;
  res.max = lowerEdge + barWidth * static_cast<double>(nBars);
  res.start = shownStart;
  res.width = barWidth;
  res.bin = nBars;
  res.N = ext.total;
  if (!wantGenerate) return res;

  // Bin index floor((x - L)/h): bins are closed on the left, except that a
  // value on the top edge belongs to the last bin (probe edge) and anything
  // below the lower edge to the first.
  const double lastIndex = static_cast<double>(nBars);
  std::map<double, double> weightInBin;
  for (size_t i = 0; i < v.size(); ++i) {
    double idx = storeAs(std::floor((v[i] - lowerEdge) / barWidth), asFloat);
    if (idx < 0.0) idx = 0.0;
    if (idx == lastIndex) idx = lastIndex - 1.0;
    weightInBin[idx] += (w != nullptr) ? (*w)[i] : 1.0;
  }
  const double halfWidth = barWidth / 2.0;
  for (const auto& [idx, cnt] : weightInBin) {
    res.binIndex.push_back(idx);
    res.count.push_back(cnt);
    res.centre.push_back(storeAs(lowerEdge + halfWidth + barWidth * idx, asFloat));
    res.height.push_back(storeAs(barHeight(spec.type, storeAs(cnt, asFloat), ext.total, barWidth), asFloat));
  }
  // With a user start(), one more centre row holds the start itself when the
  // dataset has room for it (hk_thg_start: row 6 = (., -4)).
  if (res.userStart && static_cast<int64_t>(res.centre.size()) < nDataRows) {
    res.centre.push_back(storeAs(shownStart, asFloat));
    res.closingRow = true;
  }
  res.nx = static_cast<int64_t>(res.centre.size());
  return res;
}

namespace {

// The eight kernels of [R] kdensity, Methods and formulas. Each is zero
// outside its support; the support edge itself is outside, except parzen's.
using KernelFn = double (*)(double);

double kernelEpanechnikov(double z) {
  static const double root5 = std::sqrt(5.0);
  if (std::fabs(z) >= root5) return 0.0;
  return 0.75 * (1.0 - z * z / 5.0) / root5;
}
double kernelEpan2(double z) { return std::fabs(z) >= 1.0 ? 0.0 : 0.75 * (1.0 - z * z); }
double kernelBiweight(double z) {
  if (std::fabs(z) >= 1.0) return 0.0;
  const double u = 1.0 - z * z;
  return (15.0 / 16.0) * u * u;
}
double kernelCosine(double z) { return std::fabs(z) >= 0.5 ? 0.0 : 1.0 + std::cos(2.0 * M_PI * z); }
double kernelGaussian(double z) {
  static const double root2pi = std::sqrt(2.0 * M_PI);
  return std::exp(-0.5 * z * z) / root2pi;
}
double kernelParzen(double z) {
  const double a = std::fabs(z);
  if (a > 1.0) return 0.0;
  if (a > 0.5) {
    const double u = 1.0 - a;
    return 8.0 * u * u * u / 3.0;
  }
  return 4.0 / 3.0 - 8.0 * z * z + 8.0 * a * a * a;
}
double kernelRectangle(double z) { return std::fabs(z) >= 1.0 ? 0.0 : 0.5; }
double kernelTriangle(double z) {
  const double a = std::fabs(z);
  return a >= 1.0 ? 0.0 : 1.0 - a;
}
double kernelUnknown(double) { return kNaN; }

struct KernelEntry {
  const char* name;
  size_t shortest;  // the minimum abbreviation kdensity accepts for kernel()
  KernelFn fn;
};

// Alphabetical. No accepted abbreviation fits two kernels.
const std::array<KernelEntry, 8>& kernelTable() {
  static const std::array<KernelEntry, 8> table = {{{"biweight", 2, kernelBiweight},
                                                    {"cosine", 3, kernelCosine},
                                                    {"epan2", 5, kernelEpan2},
                                                    {"epanechnikov", 2, kernelEpanechnikov},
                                                    {"gaussian", 3, kernelGaussian},
                                                    {"parzen", 3, kernelParzen},
                                                    {"rectangle", 3, kernelRectangle},
                                                    {"triangle", 3, kernelTriangle}}};
  return table;
}

KernelFn kernelByFullName(const std::string& name) {
  for (const KernelEntry& k : kernelTable()) {
    if (name == k.name) return k.fn;
  }
  return kernelUnknown;
}

// True for the full name of one of the eight kernels.
bool isKernelName(const std::string& word) {
  return std::any_of(kernelTable().begin(), kernelTable().end(),
                     [&word](const KernelEntry& e) { return word == e.name; });
}

}  // namespace

double kdeKernel(const std::string& kernel, double z) { return kernelByFullName(kernel)(z); }

std::optional<std::string> kdensityKernelName(const std::string& typed) {
  for (const KernelEntry& k : kernelTable()) {
    const std::string full(k.name);
    const bool longEnough = typed.size() >= k.shortest;
    const bool isPrefix = typed.size() <= full.size() && full.compare(0, typed.size(), typed) == 0;
    if (longEnough && isPrefix) return full;
  }
  return std::nullopt;
}

// f(p) = sum_i w_i K((p - x_i)/h) / (q h), [R] kdensity Methods and formulas:
// q is the weight total (the observation count unweighted; aweights need no
// rescaling, it cancels), and 1 for iweights (hk_kd_iw). The sum runs in data
// order.
std::vector<double> kdensityAt(const std::vector<double>& x, const std::vector<double>& w,
                               const std::string& wtype, const std::string& kernel, double h,
                               const std::vector<double>& at) {
  std::vector<double> dens(at.size(), kNaN);
  if (x.empty() || std::isnan(h)) return dens;
  const bool weighted = !w.empty();
  const KernelFn K = kernelByFullName(kernel);
  double q = 0.0;
  for (size_t i = 0; i < x.size(); ++i) q += weighted ? w[i] : 1.0;
  const double divisor = (wtype == "iweight") ? h : h * q;
  for (size_t j = 0; j < at.size(); ++j) {
    const double p = at[j];
    if (std::isnan(p)) continue;
    if (h == 0.0) {  // hk_kd_const: a zero bandwidth estimates 0 at every point
      dens[j] = 0.0;
      continue;
    }
    double acc = 0.0;
    for (size_t i = 0; i < x.size(); ++i) {
      const double kv = K((p - x[i]) / h);
      acc += weighted ? w[i] * kv : kv;
    }
    dens[j] = acc / divisor;
  }
  return dens;
}

namespace {

// A double as Stata's macro text carries it (eval::stataMacroRealText), read
// back as a double.
double viaMacroText(double d) {
  if (std::isnan(d)) return d;
  return std::strtod(eval::stataMacroRealText(d).c_str(), nullptr);
}

}  // namespace

namespace {

// A running sum with Neumaier's correction term. Summarize's mean and variance
// (and so the default bandwidth) agree with the exactly rounded sums on every
// oracle bandwidth; a plain left-to-right sum is an ulp off on some.
class AccurateSum {
 public:
  void add(double term) {
    const double next = total_ + term;
    if (std::fabs(total_) >= std::fabs(term)) {
      carry_ += (total_ - next) + term;
    } else {
      carry_ += (term - next) + total_;
    }
    total_ = next;
  }
  double value() const { return total_ + carry_; }

 private:
  double total_ = 0.0;
  double carry_ = 0.0;
};

// One (value, weight) observation of a summarize sample.
struct WeightedObs {
  double x;
  double w;
};

// Summarize's p-th percentile of `obs` sorted by x ([R] summarize, Methods and
// formulas): with cumulative weights W_(i) and P = W p/100, the first x_(i)
// whose W_(i) exceeds P; on an exact tie the midpoint of x_(i) and x_(i+1).
double weightedPercentile(const std::vector<WeightedObs>& obs, double totalWeight, double p) {
  if (obs.empty()) return kNaN;
  const double target = totalWeight * p / 100.0;
  double running = 0.0;
  for (size_t i = 0; i < obs.size(); ++i) {
    running += obs[i].w;
    if (running > target) return obs[i].x;
    if (running == target) {
      return (i + 1 < obs.size()) ? (obs[i].x + obs[i + 1].x) / 2.0 : obs[i].x;
    }
  }
  return obs.back().x;
}

}  // namespace

// kdensity's default bandwidth, [R] kdensity Methods and formulas:
// h = 0.9 m / N^(1/5) with m = min(sd, IQR/1.349) (m = sd when that is not
// positive). The moments and quartiles are summarize's with the weights;
// iweights do not enter the bandwidth. Stata carries m and h as macro text,
// which decides the last bit (W2AQ (10)); h is formed as 0.9 (m / N^0.2).
double kdensityDefaultWidth(const std::vector<double>& x, const std::vector<double>& w,
                            const std::string& wtype, double* outMin, double* outMax,
                            double* outMean, double* outSd) {
  const bool useWeights = !w.empty() && wtype != "iweight";
  auto rawWeight = [&](size_t i) { return useWeights ? w[i] : 1.0; };

  double lo = kNaN, hi = kNaN;
  AccurateSum rawTotal;
  for (size_t i = 0; i < x.size(); ++i) {
    rawTotal.add(rawWeight(i));
    if (std::isnan(lo) || x[i] < lo) lo = x[i];
    if (std::isnan(hi) || x[i] > hi) hi = x[i];
  }
  *outMin = lo;
  *outMax = hi;
  if (x.empty()) {
    *outMean = *outSd = kNaN;
    return kNaN;
  }

  const double nObs = static_cast<double>(x.size());
  // aweights are rescaled to sum to the number of observations.
  const double rescale = (useWeights && wtype == "aweight") ? nObs / rawTotal.value() : 1.0;
  std::vector<WeightedObs> obs(x.size());
  AccurateSum weightTotal, weightedX;
  for (size_t i = 0; i < x.size(); ++i) {
    obs[i] = {x[i], rawWeight(i) * rescale};
    weightTotal.add(obs[i].w);
    weightedX.add(obs[i].w * obs[i].x);
  }
  const double W = weightTotal.value();
  const double mean = weightedX.value() / W;
  AccurateSum squares;
  for (const WeightedObs& o : obs) {
    const double dev = o.x - mean;
    squares.add(o.w * dev * dev);
  }
  // Summarize's N: the fweight total, else the observation count.
  const double N = (useWeights && wtype == "fweight") ? rawTotal.value() : nObs;
  const double sd = N > 1.0 ? std::sqrt(squares.value() / (N - 1.0)) : kNaN;
  *outMean = mean;
  *outSd = sd;

  std::stable_sort(obs.begin(), obs.end(), [](const WeightedObs& a, const WeightedObs& b) { return a.x < b.x; });
  const double spreadByIqr =
      (weightedPercentile(obs, W, 75) - weightedPercentile(obs, W, 25)) / 1.349;
  // min() over the two spreads ignores a missing sd.
  double m = viaMacroText(std::isnan(sd) ? spreadByIqr : std::min(sd, spreadByIqr));
  if (m <= 0.0) m = viaMacroText(sd);
  return viaMacroText(0.9 * (m / std::pow(N, 0.2)));
}

KdensityResult kdensityEstimate(const std::vector<double>& x, const std::vector<double>& w,
                                const KdensitySpec& spec, int64_t nDataRows,
                                const std::vector<double>* at) {
  KdensityResult res;
  res.kernel = spec.kernel;
  double lo = kNaN, hi = kNaN;
  const double defaultH = kdensityDefaultWidth(x, w, spec.wtype, &lo, &hi, &res.mean, &res.sd);
  const double h = spec.width != 0.0 ? spec.width : defaultH;
  res.bwidth = h;

  if (at != nullptr) {
    // Evaluate at the given points (one per data row, missing ones kept);
    // r(n) counts the nonmissing ones but is at least 1 (probe kdatmiss).
    res.grid = *at;
    const auto present = std::count_if(at->begin(), at->end(), [](double a) { return !std::isnan(a); });
    res.n = std::max<int64_t>(1, static_cast<int64_t>(present));
  } else {
    // n() of 1 or less means the default 50; more points than data rows are
    // cut to _N with a note (probes kdn0, kdnneg, kdn20).
    int64_t points = spec.n > 1 ? spec.n : 50;
    if (points > nDataRows) {
      points = nDataRows;
      res.nCapped = true;
    }
    res.n = points;
    // Equally spaced from min - h to max + h.
    const double first = lo - h;
    const double step = ((hi - lo) + 2.0 * h) / static_cast<double>(points - 1);
    res.grid.resize(static_cast<size_t>(points));
    for (int64_t i = 0; i < points; ++i) res.grid[static_cast<size_t>(i)] = first + static_cast<double>(i) * step;
  }
  res.density = kdensityAt(x, w, spec.wtype, spec.kernel, h, res.grid);

  // r(scale) = 1 / (n * mean density); missing when there is no density or
  // the mean is 0 (hk_kd_const, hk_kd_empty). StataBE's value is the one an
  // accurately summed mean gives (probes kdn20, kdatpart).
  AccurateSum densSum;
  int64_t counted = 0;
  for (double d : res.density) {
    if (std::isnan(d)) continue;
    densSum.add(d);
    ++counted;
  }
  res.scale = kNaN;
  if (counted > 0) {
    const double meanDensity = densSum.value() / static_cast<double>(counted);
    if (meanDensity != 0.0) res.scale = 1.0 / (static_cast<double>(res.n) * meanDensity);
  }
  return res;
}

namespace {

// The histogram twin's note, as twoway__histogram_gen ..., display prints it.
std::string histogramNoteText(const HistGenResult& r, bool discrete) {
  if (discrete) return "(start=" + displayNum(r.start) + ", width=" + displayNum(r.width) + ")";
  return "(bin=" + std::to_string(r.bin) + ", start=" + displayNum(r.start) +
         ", width=" + displayNum(r.width) + ")";
}

ojson displayNoteBlock(const std::string& text) {
  ojson b;
  b["type"] = "display";
  b["text"] = text;
  return b;
}

// The bin options of histogram / twoway__histogram_gen / the twoway histogram
// layer, checked: the spec, or the refusal.
struct HistOpts {
  HistGenSpec spec;
  std::optional<ojson> err;
};

// The text of option `key`: its first occurrence when `first`, else its last
// (Stata's syntax keeps the last of a repeated option, hk_h_dupw2).
std::optional<std::string> optionText(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                      const std::string& key, bool first) {
  std::optional<std::string> found;
  for (const auto& [k, v] : opts) {
    if (k != key) continue;
    found = v.isBool ? std::string() : v.strValue;
    if (first) break;
  }
  return found;
}

// bins()/width() take one positive number, and bins() also an integer; the
// refusal names the option the way Stata's number-list check does.
std::optional<ojson> readPositiveScalar(const std::string& optName, const std::string& text,
                                        bool integerOnly, double* value) {
  const std::string prefix = optName + "() invalid -- ";
  const std::vector<std::string> words = splitWsG(text);
  if (words.size() != 1) return stataRefusal(prefix + "invalid numlist has too many elements", 123);
  const std::optional<double> d = parseNumberG(words.front());
  if (!d.has_value()) return stataRefusal(prefix + "invalid number", 121);
  if (integerOnly && std::floor(*d) != *d) return stataRefusal(prefix + "invalid numlist has noninteger elements", 126);
  if (!(*d > 0.0)) return stataRefusal(prefix + "invalid number, outside of allowed range", 125);
  *value = *d;
  return std::nullopt;
}

// The height types switched on, in the order typed, `freq` read as frequency.
std::vector<std::string> requestedHeightTypes(const std::vector<std::pair<std::string, eval::OptValue>>& opts) {
  static const std::set<std::string> kTypes = {"density", "fraction", "frequency", "percent"};
  std::vector<std::string> seen;
  for (const auto& [k, v] : opts) {
    if (v.isBool && !v.boolValue) continue;
    const std::string t = (k == "freq") ? std::string("frequency") : k;
    if (kTypes.count(t) != 0 && std::find(seen.begin(), seen.end(), t) == seen.end()) seen.push_back(t);
  }
  return seen;
}

HistOpts parseHistOpts(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                       const std::string& varname, bool genFloat, bool firstWins = false) {
  HistOpts out;
  HistGenSpec& sp = out.spec;
  sp.varname = varname;
  sp.genFloat = genFloat;
  sp.discrete = hasOpt(opts, "discrete");
  const std::optional<std::string> binText = optionText(opts, "bin", firstWins);
  const std::optional<std::string> widthText = optionText(opts, "width", firstWins);
  const std::optional<std::string> startText = optionText(opts, "start", firstWins);
  auto refuse = [&out](ojson block) {
    out.err = std::move(block);
    return out;
  };

  if (binText.has_value() && sp.discrete) {
    return refuse(stataRefusal("options discrete and bins() may not be combined", 198));
  }
  if (binText.has_value() && widthText.has_value()) {
    return refuse(stataRefusal("options bins() and width() may not be combined", 198));
  }
  const std::vector<std::string> types = requestedHeightTypes(opts);
  if (types.size() > 1) {
    std::string named;
    for (const std::string& t : types) named += (named.empty() ? "" : " ") + t;
    return refuse(stataRefusal("options " + named + " may not be combined", 198));
  }
  sp.type = types.empty() ? std::string("density") : types.front();

  if (binText.has_value()) {
    double b = 0.0;
    if (auto bad = readPositiveScalar("bins", *binText, true, &b)) return refuse(*bad);
    sp.bins = static_cast<int64_t>(b);
  }
  if (widthText.has_value()) {
    double wd = 0.0;
    if (auto bad = readPositiveScalar("width", *widthText, false, &wd)) return refuse(*bad);
    sp.width = wd;
  }
  if (startText.has_value()) {
    const std::vector<std::string> words = splitWsG(*startText);
    const std::optional<double> s = words.size() == 1 ? parseNumberG(words.front()) : std::nullopt;
    if (!s.has_value()) return refuse(stataRefusal("start() invalid -- invalid numlist", 121));
    sp.start = *s;
  }
  return out;
}

// serset-twoway HIST-1: a twoway function's (y, x) serset, `double double`
// (the normal overlays: w2aq hk_h_064normal_s2 log:2145, hk_kd_106norm_s2
// log:4414).
serset::Serset functionSerset(const ojson& points) {
  std::vector<double> y, x;
  for (const auto& p : points) {
    x.push_back(p[0].is_number() ? p[0].get<double>() : kNaN);
    y.push_back(p[1].is_number() ? p[1].get<double>() : kNaN);
  }
  serset::Serset s;
  s.n = static_cast<int64_t>(y.size());
  s.cols.push_back(serset::numericColumn(serset::tempName(0), "double", std::move(y)));
  s.cols.push_back(serset::numericColumn(serset::tempName(1), "double", std::move(x)));
  return s;
}

}  // namespace

namespace {

// A height type as its column label: `Density`, `Fraction`, ...
std::string capitalised(std::string s) {
  if (!s.empty()) s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
  return s;
}

// `length` values: `head` in the first rows, missing after.
std::vector<double> paddedColumn(const std::vector<double>& head, int64_t length) {
  std::vector<double> col(static_cast<size_t>(std::max<int64_t>(0, length)), kNaN);
  std::copy_n(head.begin(), std::min(head.size(), col.size()), col.begin());
  return col;
}

// 300 points of a normal density with the given mean and sd over [lo, hi]
// (point i at lo + (hi - lo) i/299), each times `area`.
ojson normalCurve(double lo, double hi, double mean, double sd, double area) {
  static const double root2pi = std::sqrt(2.0 * M_PI);
  constexpr int kPoints = 300;
  ojson pts = ojson::array();
  for (int i = 0; i < kPoints; ++i) {
    const double px = lo + (hi - lo) * static_cast<double>(i) / static_cast<double>(kPoints - 1);
    const double z = (px - mean) / sd;
    pts.push_back(ojson::array({px, std::exp(-0.5 * z * z) / (root2pi * sd) * area}));
  }
  return pts;
}

// A value written with 13 significant digits and read back. The drawn bars of
// `histogram` start at the note's r(min) and use its r(width) at this
// precision: it is what puts hk_h_discnonint's first serset centre at 5.0e-14
// (+1.c240000000000X-02d, w2aq_histogram_family.log:1712) instead of 0.
double thirteenDigits(double d) {
  if (std::isnan(d)) return d;
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.13g", d);
  return std::strtod(buf, nullptr);
}

// The overlays' scale factor r(area) at the precision StataBE scales them by:
// seven significant digits for the non-integer values measured (fraction
// 1.571429, percent 157.1429, frequency 73.33333 / 94.28571; measured on StataBE:
// nfr, npc, nb9, hnfq), whole numbers as they are (132, 78).
double overlayScale(double area) {
  if (std::isnan(area) || (area == std::floor(area) && std::fabs(area) < 1e8)) return area;
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.7g", area);
  return std::strtod(buf, nullptr);
}

// The occupied bins of a result as the renderers read them (graph.js,
// buildSvgHistogram): edges x0/x1 from r(min) and r(width), the centre, the raw
// count and the height. The closing row is not a bar.
ojson barList(const HistGenResult& g) {
  ojson bars = ojson::array();
  for (size_t i = 0; i < g.height.size(); ++i) {
    const double q = g.binIndex[i];
    bars.push_back(ojson{{"x0", g.min + g.width * q},
                         {"x1", g.min + g.width * (q + 1.0)},
                         {"x", g.centre[i]},
                         {"count", g.count[i]},
                         {"y", g.height[i]}});
  }
  return bars;
}

// The serset of a histogram plot: height, a byte 0, the bin centre, one row per
// occupied bin, plus (., 0, start) when the result has the closing row. Heights
// and centres are float under c(type) float (hk_histogram_bare `float byte
// float`, w2al tf_tw_073hist_s1).
serset::Serset barSerset(const HistGenResult& g, bool genFloat) {
  const size_t occupied = g.height.size();
  std::vector<double> heights(g.height);
  std::vector<double> centres(g.centre.begin(), g.centre.begin() + static_cast<std::ptrdiff_t>(
                                                                       std::min(occupied, g.centre.size())));
  if (g.closingRow && g.centre.size() > occupied) {
    heights.push_back(kNaN);
    centres.push_back(g.centre[occupied]);
  }
  std::vector<double> zeros(heights.size(), 0.0);
  const std::string type = genFloat ? "float" : "double";
  serset::Serset s;
  s.n = static_cast<int64_t>(heights.size());
  s.cols.push_back(serset::numericColumn(serset::tempName(0), type, std::move(heights)));
  s.cols.push_back(serset::numericColumn(serset::tempName(1), "byte", std::move(zeros)));
  s.cols.push_back(serset::numericColumn(serset::tempName(2), type, std::move(centres)));
  return s;
}

// A computed curve over the whole frame: `curve` in its first rows, the grid
// beside it, and then the plotted variable of the full dataset in data order;
// max(curve length, _N) rows (hk_h_044disc_s2 `double double <type>`, w2al
// tf_kd_n).
serset::Serset curveWithVariableSerset(SessionState& session, Datastore& full, const std::string& var,
                                       const std::vector<double>& curve, const std::vector<double>& grid) {
  const int64_t nData = full.nRows();
  const int64_t rows = std::max<int64_t>(static_cast<int64_t>(curve.size()), nData);
  std::vector<int64_t> dataRow(static_cast<size_t>(rows));
  for (int64_t i = 0; i < rows; ++i) dataRow[static_cast<size_t>(i)] = i < nData ? i : -1;
  serset::Serset s;
  s.n = rows;
  s.cols.push_back(serset::numericColumn(serset::tempName(0), "double", paddedColumn(curve, rows)));
  s.cols.push_back(serset::numericColumn(serset::tempName(1), "double", paddedColumn(grid, rows)));
  s.cols.push_back(serset::variableColumn(session, full, var, dataRow));
  return s;
}

// `count` points from `lo`, each the previous plus (hi - lo)/(count - 1): the
// accumulated grid of the kdensity curves (hnk_s2 point 7 -1.63beee05231a6X+001,
// tf_kd_n row 11 one ulp under 6.25).
std::vector<double> accumulatedGrid(double lo, double hi, int64_t count) {
  std::vector<double> grid(static_cast<size_t>(std::max<int64_t>(count, 0)), kNaN);
  if (grid.empty()) return grid;
  grid[0] = lo;
  const double step = count > 1 ? (hi - lo) / static_cast<double>(count - 1) : kNaN;
  for (size_t i = 1; i < grid.size(); ++i) grid[i] = grid[i - 1] + step;
  return grid;
}

// histogram's by(): repeated by() options merge; the variable is the first word
// of the last one that names any.
struct ByRequest {
  bool given = false;
  std::string var;
};

ByRequest readByRequest(const std::vector<std::pair<std::string, eval::OptValue>>& opts) {
  ByRequest by;
  for (const auto& [k, v] : opts) {
    if (k != "by" || v.isBool) continue;
    by.given = true;
    const std::string& text = v.strValue;
    const std::string head = trimG(text.substr(0, text.find(',')));
    if (!head.empty()) by.var = splitWsG(head).front();
  }
  return by;
}

// One by() panel of histogram: its key and its share of the sample.
struct HistPanel {
  ojson key;
  std::vector<double> v, w;
};

// The sample split by the by variable, in Stata's sort order (numeric
// ascending; a string variable by its text). Missing and "" groups are left
// out.
std::vector<HistPanel> splitByPanels(Datastore& ds, const std::string& byVar, const GraphSample& smp) {
  const std::vector<double> keys = numericColumn(ds, byVar);
  const bool weighted = !smp.w.empty();
  std::vector<std::string> levelText;
  const bool textual = ds.dtype(byVar) == "factor";
  if (textual) {
    const VarInfo vi = ds.varinfo(byVar);
    levelText.assign(vi.levels.begin(), vi.levels.end());
  }
  std::map<double, HistPanel> byNumber;
  std::map<std::string, HistPanel> byText;
  for (size_t i = 0; i < smp.v.size(); ++i) {
    const double key = keys[static_cast<size_t>(smp.rows[i])];
    if (std::isnan(key)) continue;
    HistPanel* panel = nullptr;
    if (textual) {
      if (key < 0 || static_cast<size_t>(key) >= levelText.size()) continue;
      const std::string& label = levelText[static_cast<size_t>(key)];
      if (label.empty()) continue;
      panel = &byText[label];
      panel->key = label;
    } else {
      panel = &byNumber[key];
      panel->key = key;
    }
    panel->v.push_back(smp.v[i]);
    if (weighted) panel->w.push_back(smp.w[i]);
  }
  std::vector<HistPanel> out;
  for (auto& entry : byNumber) out.push_back(std::move(entry.second));
  for (auto& entry : byText) out.push_back(std::move(entry.second));
  return out;
}

// The spec of the drawn bars: the continuous rule from the note's r(min) (as
// 13-digit text) with r(bin), or with r(width) under discrete, width() or by();
// a bin option repeated with a different value takes the last one (a repeated
// option is a graph option, hk_h_dupw2).
HistGenSpec drawnBarSpec(const HistGenResult& note, const HistGenSpec& firstSpec, const HistGenSpec& lastSpec,
                         bool widthOnly) {
  HistGenSpec bars;
  bars.varname = firstSpec.varname;
  bars.type = firstSpec.type;
  bars.genFloat = firstSpec.genFloat;
  bars.start = thirteenDigits(note.min);
  if (widthOnly || firstSpec.discrete || firstSpec.width.has_value()) {
    bars.width = thirteenDigits(note.width);
  } else {
    bars.bins = note.bin;
  }
  if (lastSpec.start != firstSpec.start) bars.start = lastSpec.start;
  if (lastSpec.width != firstSpec.width) {
    bars.width = lastSpec.width;
    bars.bins.reset();
  }
  if (lastSpec.bins != firstSpec.bins) {
    bars.bins = lastSpec.bins;
    bars.width.reset();
  }
  return bars;
}

double tallestBar(const HistGenResult& g, double floor) {
  for (double y : g.height) floor = std::max(floor, y);
  return floor;
}

// histogram's addplot(): the added plots are twoway plots of their own over the
// full frame; their layers and sersets join the histogram's. An error block
// from them is returned.
std::optional<ojson> addAddplot(SessionState& session, const std::string& text, ojson& graph,
                                std::vector<serset::Serset>* sersets) {
  std::string body = trimG(text);
  while (body.size() >= 2 && body.compare(body.size() - 2, 2, "||") == 0) body = trimG(body.substr(0, body.size() - 2));
  if (body.empty()) return std::nullopt;
  twoway::ParseResult parsedPlots =
      twoway::parseTwoway(twoway::hasToplevelParens(body) ? body : twoway::expandDoublePipes(body));
  if (parsedPlots.layers.empty()) return std::nullopt;
  for (auto& layer : parsedPlots.layers) eval::canonicalizeForCommand(layer.opts, "twoway_layer");
  eval::canonicalizeForCommand(parsedPlots.globalOpts, "twoway_layer");
  std::vector<serset::PlotSpec> plots;
  ojson drawn = runTwowayLayers(session.datastore(), session, parsedPlots.layers, parsedPlots.globalOpts,
                                parsedPlots.ifExpr, &plots, parsedPlots.inRange);
  if (drawn.is_object() && drawn.value("type", std::string()) == "error") return drawn;
  if (drawn.is_object() && drawn.contains("layers")) graph["addplot_layers"] = drawn["layers"];
  if (sersets != nullptr) {
    for (auto& s : serset::buildPlotSersets(std::move(plots))) sersets->push_back(std::move(s));
  }
  return std::nullopt;
}

}  // namespace

ojson runHistogram(SessionState& session, Datastore& ds, const std::vector<std::string>& varlist,
                   const eval::ParsedCommand& parsed, std::vector<ojson>* pre,
                   std::vector<serset::Serset>* sersets) {
  const auto& opts = parsed.opts;
  if (varlist.empty()) return stataRefusal("varlist required", 100);
  const std::string var = varlist.front();
  if (!ds.has(var)) return stataRefusal("variable " + var + " not found", 111);
  if (varlist.size() > 1) return stataRefusal("too many variables specified", 103);
  if (ds.dtype(var) == "factor") return stataRefusal(var + " is a string variable", 109);
  const std::string wtype = weightTypeName(parsed);
  if (!wtype.empty() && wtype != "fweight") return stataRefusal(wtype + "s not allowed", 101);
  Datastore* full = session.datastore();
  const int64_t nRows = full != nullptr ? full->nRows() : ds.nRows();
  if (nRows == 0) return stataRefusal("no observations", 2000);

  // The printed note reads the first bins()/width()/start(); the drawn bars
  // the last. Both must be valid.
  const bool genFloat = session.defaultStorageType != "double";
  const HistOpts firstOpts = parseHistOpts(opts, var, genFloat, /*firstWins=*/true);
  if (firstOpts.err) return *firstOpts.err;
  const HistOpts lastOpts = parseHistOpts(opts, var, genFloat, /*firstWins=*/false);
  if (lastOpts.err) return *lastOpts.err;
  const HistGenSpec& spec = firstOpts.spec;

  const GraphSample smp = graphSample(ds, var, parsed, wtype);
  if (smp.err) return *smp.err;
  if (smp.v.empty()) return stataRefusal("no observations", 2000);

  const ByRequest by = readByRequest(opts);
  if (by.given && by.var.empty()) return stataRefusal("varlist required in by()", 100);
  const bool wantKdensity = hasOpt(opts, "kdensity") || hasOpt(opts, "kdenopts");
  const bool wantNormal = hasOpt(opts, "normal") || hasOpt(opts, "normopts");
  if (by.given && spec.type == "frequency" && (wantKdensity || wantNormal)) {
    const std::string which = wantNormal && wantKdensity ? "normal kdensity" : (wantNormal ? "normal" : "kdensity");
    const std::string tail = wantNormal && wantKdensity ? " and by() options" : " and by() option";
    return stataRefusal("option frequency may not be combined with the " + which + tail, 191);
  }
  if (by.given && !ds.has(by.var)) return stataRefusal("variable " + by.var + " not found", 111);

  const std::vector<double>* weights = smp.w.empty() ? nullptr : &smp.w;
  const HistGenResult note = histogramGen(smp.v, weights, spec, nRows, false);
  if (note.rc != 0) return stataRefusal(note.message, note.rc);
  if (pre != nullptr && !by.given) pre->push_back(displayNoteBlock(histogramNoteText(note, spec.discrete)));
  // histogram leaves no numeric r() behind (measured on StataBE, probe hret: r(N) missing).
  eval::replaceRClass(session.eval);
  eval::declareRPosted(session.eval, "histogram", {});

  const HistGenSpec barSpec = drawnBarSpec(note, spec, lastOpts.spec, /*widthOnly=*/by.given);

  ojson graph;
  graph["type"] = "graph";
  graph["kind"] = "histogram";
  graph["title"] = optTitleStr(opts, "title").value_or(std::string());
  graph["x_label"] = optTitleStr(opts, "xtitle").value_or(varTitleG(session, var));
  graph["y_label"] = optTitleStr(opts, "ytitle").value_or(capitalised(spec.type));
  graph["note"] = optTitleStr(opts, "note").value_or(std::string());
  graph["x_var"] = var;
  graph["y_kind"] = spec.type;
  graph["n"] = note.N;
  graph["bin"] = note.bin;
  graph["start"] = note.start;
  graph["width"] = note.width;
  graph["discrete"] = spec.discrete;
  if (!wtype.empty()) graph["weight"] = wtype + "=" + trimG(parsed.weightVar.value_or(std::string()));

  double yTop = 0.0;
  if (!by.given) {
    const HistGenResult bars = histogramGen(smp.v, weights, barSpec, nRows, true);
    graph["bins"] = barList(bars);
    graph["layer_start"] = bars.start;
    yTop = tallestBar(bars, yTop);
    if (sersets != nullptr) sersets->push_back(barSerset(bars, genFloat));
  } else {
    ojson panels = ojson::array();
    for (const HistPanel& panel : splitByPanels(ds, by.var, smp)) {
      const HistGenResult bars =
          histogramGen(panel.v, weights != nullptr ? &panel.w : nullptr, barSpec, nRows, true);
      if (sersets != nullptr) sersets->push_back(barSerset(bars, genFloat));
      panels.push_back(ojson{{"by", panel.key}, {"n", bars.N}, {"bins", barList(bars)}});
      yTop = tallestBar(bars, yTop);
    }
    graph["by"] = by.var;
    graph["panels"] = panels;
    graph["bins"] = panels.empty() ? ojson::array() : panels[0]["bins"];
  }
  graph["x_min"] = note.min;
  graph["x_max"] = note.max;
  graph["y_min"] = 0.0;
  graph["y_max"] = yTop;
  graph["name"] = nullptr;  // filled by the dispatcher's name() capture (BS-010)

  // The overlays are scaled by r(area), so they share the bars' units.
  const double scale = overlayScale(note.area);
  double mean = kNaN, sd = kNaN;
  double ignoredLo = kNaN, ignoredHi = kNaN;
  const double defaultH = kdensityDefaultWidth(smp.v, smp.w, wtype, &ignoredLo, &ignoredHi, &mean, &sd);

  if (wantKdensity && !by.given) {
    // The epanechnikov density at the default bandwidth over 300 accumulated
    // points from r(min) to r(max).
    constexpr int64_t kCurvePoints = 300;
    const std::vector<double> grid =
        accumulatedGrid(thirteenDigits(note.min), thirteenDigits(note.max), kCurvePoints);
    std::vector<double> curve = kdensityAt(smp.v, smp.w, wtype, "epanechnikov", defaultH, grid);
    ojson pts = ojson::array();
    for (size_t i = 0; i < curve.size(); ++i) {
      curve[i] *= scale;
      pts.push_back(ojson::array({grid[i], curve[i]}));
      if (!std::isnan(curve[i])) yTop = std::max(yTop, curve[i]);
    }
    graph["y_max"] = yTop;
    graph["kdensity_points"] = pts;
    graph["kdensity_bwidth"] = defaultH;
    if (sersets != nullptr && full != nullptr) {
      sersets->push_back(curveWithVariableSerset(session, *full, var, curve, grid));
    }
  }

  if (wantNormal && !by.given && sd > 0) {
    // The normal density with the sample's (fweighted) mean and sd.
    ojson pts = normalCurve(note.min, note.max, mean, sd, scale);
    for (const auto& p : pts) yTop = std::max(yTop, p[1].get<double>());
    graph["y_max"] = yTop;
    graph["normal_points"] = pts;
    graph["normal_mean"] = mean;
    graph["normal_sd"] = sd;
    if (sersets != nullptr) {
      serset::Serset curve = functionSerset(pts);
      const std::string wvar = trimG(parsed.weightVar.value_or(std::string()));
      if (!wtype.empty() && full != nullptr && full->has(wvar)) {
        // Under fweights the curve's serset also keeps the weight variable, in
        // data order (measured on StataBE, probes hnfw, nfwf: `double double byte`).
        curve = curveWithVariableSerset(session, *full, wvar, curve.cols[0].num, curve.cols[1].num);
      }
      sersets->push_back(std::move(curve));
    }
  }

  if (const std::optional<std::string> added = optLast(opts, "addplot")) {
    graph["addplot"] = *added;
    if (auto bad = addAddplot(session, *added, graph, sersets)) return *bad;
  }
  return graph;
}

namespace {

// twoway__histogram_gen's generate(h x [, replace]): the two new names (height
// first, centre second), or none.
struct HistGenTargets {
  std::vector<std::string> names;
  bool replace = false;
  std::optional<ojson> err;
};

HistGenTargets readHistGenTargets(SessionState& session,
                                  const std::vector<std::pair<std::string, eval::OptValue>>& opts) {
  HistGenTargets t;
  const std::optional<std::string> text = optLast(opts, "generate");
  if (!text.has_value()) return t;
  std::string list = *text;
  const size_t comma = list.find(',');
  if (comma != std::string::npos) {
    const std::string sub = trimG(list.substr(comma + 1));
    if (!sub.empty() && sub != "replace") {
      t.err = stataRefusal("option " + sub + " not allowed", 198);
      return t;
    }
    t.replace = (sub == "replace");
    list.erase(comma);
  }
  t.names = splitWsG(list);
  if ((t.replace || !t.names.empty()) && t.names.size() != 2) {
    t.err = stataRefusal("option generate() incorrectly specified", 198);
    return t;
  }
  if (!t.names.empty() && !t.replace) t.err = confirmNewVars(session, t.names, /*optionList=*/true);
  return t;
}

// `drop` whichever of `names` exist, through the dispatcher; its error block
// when it fails.
std::optional<ojson> dropIfPresent(SessionState& session, const std::vector<std::string>& names) {
  std::string cmd;
  for (const std::string& n : names) {
    Datastore* cur = session.datastore();
    if (cur != nullptr && cur->has(n)) cmd += " " + n;
  }
  if (cmd.empty()) return std::nullopt;
  for (const auto& block : piet::dispatch("drop" + cmd, session)) {
    if (block.value("type", std::string()) == "error") return block;
  }
  return std::nullopt;
}

}  // namespace

// `twoway__histogram_gen`: Stata's internal histogram subroutine, served (registry
// row, floors row, pangram S24, freeze fixture).
ojson runTwowayHistogramGen(const eval::ParsedCommand& parsed, SessionState& session) {
  const auto& opts = parsed.opts;
  if (parsed.varlist.empty()) return stataRefusal("varlist required", 100);
  const std::string var = parsed.varlist.front();
  Datastore* full = session.hasData() ? session.datastore() : nullptr;
  if (full == nullptr || !full->has(var)) return stataRefusal("variable " + var + " not found", 111);
  if (parsed.varlist.size() > 1) return stataRefusal("too many variables specified", 103);
  if (full->dtype(var) == "factor") return stataRefusal("type mismatch", 109);
  const std::string wtype = weightTypeName(parsed);
  if (!wtype.empty() && wtype != "fweight") return stataRefusal(wtype + "s not allowed", 101);

  static const std::set<std::string> kAllowed = {"discrete", "bin",     "width",   "start",  "density",
                                                 "fraction", "frequency", "freq",  "percent", "return",
                                                 "generate", "display"};
  for (const auto& [k, v] : opts) {
    if (kAllowed.count(k) == 0) return stataRefusal("option " + k + (v.isBool ? "" : "()") + " not allowed", 198);
  }
  const HistGenTargets targets = readHistGenTargets(session, opts);
  if (targets.err) return *targets.err;
  const bool genFloat = session.defaultStorageType != "double";
  const HistOpts ho = parseHistOpts(opts, var, genFloat);
  if (ho.err) return *ho.err;

  const bool wantDisplay = hasOpt(opts, "display");
  const bool wantGenerate = !targets.names.empty();
  // From here the program counts as having run: r() is emptied, and a call
  // with nothing to return or generate leaves it empty (hk_thg_syntaxonly_r).
  eval::replaceRClass(session.eval);
  if (!wantGenerate && !wantDisplay && !hasOpt(opts, "return")) {
    eval::declareRPosted(session.eval, "twoway__histogram_gen", {});
    return ojson::array();
  }

  // The sample over the whole frame, `if` applied through a scratch copy.
  std::string ifCopy;
  std::unique_ptr<Datastore> restricted;
  if (parsed.ifExpr.has_value() && !parsed.ifExpr->empty()) {
    ifCopy = mintScratchPath(session.workdir(), "histgenif");
    const std::vector<std::string> args = translate::injectTsContextForExpr(
        {"eval", "--data", full->path(), "--keepif", *parsed.ifExpr, "--out", ifCopy}, *parsed.ifExpr, session);
    try {
      runCobraJson(args);
    } catch (const CobraError& ce) {
      cecon::native_unlink(ifCopy);
      return ojson(eval::engineError(cobraErrorMessage(ce), ce.code));
    }
    restricted = std::make_unique<Datastore>(ifCopy);
  }
  const GraphSample smp = graphSample(restricted ? *restricted : *full, var, parsed, wtype);
  const int64_t nRows = full->nRows();
  const std::string centreFormat = displayFormatOf(session, *full, var);
  const std::string centreLabel = varTitleG(session, var);
  restricted.reset();
  if (!ifCopy.empty()) cecon::native_unlink(ifCopy);
  if (smp.err) return *smp.err;

  const HistGenResult g = histogramGen(smp.v, smp.w.empty() ? nullptr : &smp.w, ho.spec, nRows, wantGenerate);
  if (g.rc != 0) return stataRefusal(g.message, g.rc);

  ojson blocks = ojson::array();
  if (wantDisplay && !g.empty) blocks.push_back(displayNoteBlock(histogramNoteText(g, ho.spec.discrete)));

  if (wantGenerate) {
    const std::string& heightVar = targets.names[0];
    const std::string& centreVar = targets.names[1];
    if (targets.replace) {
      if (auto bad = dropIfPresent(session, {heightVar, centreVar})) return *bad;
    }
    const std::string storage = genFloat ? "float" : "double";
    const std::string heightFormat = genFloat ? "%9.0g" : "%10.0g";
    const std::string heightLabel = capitalised(ho.spec.type);
    const std::vector<double> heights = paddedColumn(g.height, nRows);
    const std::vector<double> centres = paddedColumn(g.centre, nRows);
    // Creation order is observable (variable positions): the centre comes
    // first with the source variable's format and label; an empty sample makes
    // two all-missing columns, the height first and the centre unlabelled
    // (hk_thg_empty_g).
    std::vector<std::string> made;
    std::optional<ojson> bad;
    if (g.empty) {
      bad = writeGenVariable(session, heightVar, heights, storage, heightFormat, heightLabel);
      if (!bad) bad = writeGenVariable(session, centreVar, centres, storage, heightFormat, std::string());
      made = {heightVar, centreVar};
    } else {
      bad = writeGenVariable(session, centreVar, centres, storage, centreFormat, centreLabel);
      if (!bad) bad = writeGenVariable(session, heightVar, heights, storage, heightFormat, heightLabel);
      made = {centreVar, heightVar};
    }
    if (bad) return *bad;
    blocks.push_back(mutationResult(session, "twoway__histogram_gen", made, {}));
  }

  const std::vector<std::pair<std::string, double>> scalars = {
      {"area", g.area}, {"max", g.max},   {"min", g.min},
      {"start", g.start}, {"width", g.width}, {"bin", static_cast<double>(g.bin)}, {"N", g.N}};
  std::vector<std::string> posted;
  for (const auto& [name, value] : scalars) {
    session.eval.rValues.set(name, value);
    posted.push_back(name);
  }
  session.eval.rStrings.set("type", ho.spec.type);
  posted.push_back("type");
  if (wantGenerate || g.empty) {
    session.eval.rValues.set("n_x", static_cast<double>(g.nx));
    posted.push_back("n_x");
  }
  eval::declareRPosted(session.eval, "twoway__histogram_gen", posted);
  return blocks;
}

namespace {

// The kernel named by kernel() or by an old-syntax kernel option (a bare
// kernel name), shared by `kdensity` and the twoway kdensity layer.
// `strictWords`: kernel() with more than one word is refused (kdensity);
// otherwise its first word is read (the layer).
struct KernelChoice {
  std::string name = "epanechnikov";
  std::optional<ojson> err;
};

KernelChoice chooseKernel(const std::vector<std::pair<std::string, eval::OptValue>>& opts, bool strictWords) {
  KernelChoice c;
  std::vector<std::string> bareNames;
  for (const auto& [k, v] : opts) {
    if (!(v.isBool && !v.boolValue) && isKernelName(k)) bareNames.push_back(k);
  }
  const std::optional<std::string> typed = optLast(opts, "kernel");
  const std::vector<std::string> words = typed.has_value() ? splitWsG(*typed) : std::vector<std::string>{};
  if (words.empty()) {
    if (bareNames.size() > 1) {
      c.err = stataRefusal("only one kernel may be specified", 198);
    } else if (bareNames.size() == 1) {
      c.name = bareNames.front();
    }
    return c;
  }
  if (!bareNames.empty()) {
    c.err = stataRefusal("option kernel(): old syntax may not be combined with new syntax", 198);
    return c;
  }
  if (strictWords && words.size() > 1) {
    c.err = stataRefusal("only one kernel may be specified", 198);
    return c;
  }
  const std::optional<std::string> resolved = kdensityKernelName(words.front());
  if (!resolved.has_value()) {
    c.err = stataRefusal("invalid kernel function", 198);
    return c;
  }
  c.name = *resolved;
  return c;
}

// width() / bwidth() (the old and the new spelling of the bandwidth): the
// requested bandwidth, 0 for "the default". A zero one counts as absent.
struct BandwidthChoice {
  double h = 0.0;
  std::optional<ojson> err;
};

BandwidthChoice chooseBandwidth(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                const std::string& conflictMessage) {
  BandwidthChoice b;
  // The number in option `key`, 0 when absent; false when it is not a number.
  auto numberIn = [&](const std::string& key, double* out) {
    const std::optional<std::string> text = optLast(opts, key);
    if (!text.has_value()) return true;
    const std::optional<double> d = parseNumberG(*text);
    if (!d.has_value()) {
      b.err = stataRefusal("option " + key + "() incorrectly specified", 198);
      return false;
    }
    *out = *d;
    return true;
  };
  double byWidth = 0.0, byBwidth = 0.0;
  if (!numberIn("width", &byWidth) || !numberIn("bwidth", &byBwidth)) return b;
  if (byBwidth != 0.0 && byWidth != 0.0) {
    b.err = stataRefusal(conflictMessage, 198);
    return b;
  }
  b.h = byBwidth != 0.0 ? byBwidth : byWidth;
  return b;
}

// The bandwidth as kdensity's default note prints it: rounded to .0001 and
// shown %6.4f, or %10.3e when that rounding leaves 0; `0` for a zero bandwidth.
std::string bandwidthNoteText(double h) {
  if (std::isnan(h)) return ".";
  if (h == 0.0) return "0";
  const bool tiny = std::round(h / 0.0001) * 0.0001 == 0.0;
  char buf[64];
  std::snprintf(buf, sizeof buf, tiny ? "%10.3e" : "%6.4f", h);
  return trimG(buf);
}

// kdensity's density line serset (two `double` columns, density then grid):
// grid mode keeps the grid rows and adds one all-missing row when the grid is
// shorter than the data (hk_kd_bare); at() mode keeps every row, sorted by the
// evaluation point with the missing ones last (hk_kd_039aw).
serset::Serset kdensityLineSerset(const KdensityResult& k, bool atMode, int64_t nRows) {
  std::vector<std::pair<double, double>> rows;  // (point, density)
  rows.reserve(k.grid.size() + 1);
  for (size_t i = 0; i < k.grid.size(); ++i) rows.emplace_back(k.grid[i], i < k.density.size() ? k.density[i] : kNaN);
  if (atMode) {
    std::stable_sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
      if (std::isnan(b.first)) return !std::isnan(a.first);
      return !std::isnan(a.first) && a.first < b.first;
    });
  } else if (static_cast<int64_t>(rows.size()) < nRows) {
    rows.emplace_back(kNaN, kNaN);
  }
  std::vector<double> dens, pts;
  for (const auto& [p, d] : rows) {
    pts.push_back(p);
    dens.push_back(d);
  }
  serset::Serset s;
  s.n = static_cast<int64_t>(dens.size());
  s.cols.push_back(serset::numericColumn(serset::tempName(0), "double", std::move(dens)));
  s.cols.push_back(serset::numericColumn(serset::tempName(1), "double", std::move(pts)));
  return s;
}

}  // namespace

ojson runKdensity(SessionState& session, Datastore& ds, const std::vector<std::string>& varlist,
                  const eval::ParsedCommand& parsed, std::vector<ojson>* pre, bool* graphDrawn,
                  std::vector<serset::Serset>* sersets) {
  *graphDrawn = false;
  const auto& opts = parsed.opts;
  if (varlist.empty()) return stataRefusal("varlist required", 100);
  const std::string var = varlist.front();
  if (!ds.has(var)) return stataRefusal("variable " + var + " not found", 111);
  if (varlist.size() > 1) return stataRefusal("too many variables specified", 103);
  if (ds.dtype(var) == "factor") return stataRefusal(var + " is a string variable", 109);
  const std::string wtype = weightTypeName(parsed);
  if (wtype == "pweight") return stataRefusal("pweights not allowed", 101);

  // Under nograph only kdensity's own options are accepted (hk_r_kd_ngopt).
  const bool nograph = hasOpt(opts, "nograph");
  if (nograph) {
    static const std::set<std::string> kOwn = {"generate", "gen", "at", "n", "width", "bwidth", "nograph", "kernel"};
    for (const auto& [k, v] : opts) {
      if (kOwn.count(k) == 0 && !isKernelName(k)) {
        return stataRefusal("option " + k + (v.isBool ? "" : "()") + " not allowed", 198);
      }
    }
  }
  Datastore* full = session.datastore();
  const int64_t nRows = full != nullptr ? full->nRows() : ds.nRows();

  constexpr int64_t kDefaultPoints = 50;
  int64_t points = kDefaultPoints;
  if (const auto text = optLast(opts, "n")) {
    const std::optional<double> d = parseNumberG(*text);
    if (!d.has_value() || std::floor(*d) != *d) return stataRefusal("option n() incorrectly specified", 198);
    points = static_cast<int64_t>(*d);
  }
  const std::optional<std::string> atText = optLast(opts, "at");
  if (atText.has_value() && points != kDefaultPoints) {
    return stataRefusal("options at() and n() may not be specified together", 198);
  }
  const KernelChoice kc = chooseKernel(opts, /*strictWords=*/true);
  if (kc.err) return *kc.err;
  const BandwidthChoice bw = chooseBandwidth(opts, "options width() and bwidth() may not be specified together");
  if (bw.err) return *bw.err;
  if (bw.h < 0.0) return stataRefusal("bandwidth must be positive", 198);

  std::vector<std::string> newNames;
  for (const auto& [k, v] : opts) {
    if ((k == "generate" || k == "gen") && !v.isBool) newNames = splitWsG(v.strValue);
  }
  if (newNames.size() > 2 || (newNames.size() == 1 && !atText.has_value())) {
    return stataRefusal("invalid syntax", 198);
  }
  if (auto bad = confirmNewVars(session, newNames)) return *bad;

  std::vector<double> atPoints;
  if (atText.has_value()) {
    const std::string atVar = trimG(*atText);
    if (full == nullptr || !full->has(atVar)) return stataRefusal("variable " + atVar + " not found", 111);
    if (full->dtype(atVar) == "factor") return stataRefusal("type mismatch", 109);
    atPoints = numericColumn(*full, atVar);
  }

  const GraphSample smp = graphSample(ds, var, parsed, wtype);
  if (smp.err) return *smp.err;
  KdensitySpec ks;
  ks.kernel = kc.name;
  ks.width = bw.h;
  ks.n = points;
  ks.wtype = wtype;
  const KdensityResult k = kdensityEstimate(smp.v, smp.w, ks, nRows, atText.has_value() ? &atPoints : nullptr);
  if (k.nCapped && pre != nullptr) pre->push_back(displayNoteBlock("(n() set to " + std::to_string(k.n) + ")"));

  const std::string varTitle = varTitleG(session, var);
  ojson graph = ojson::object();
  if (!nograph) {
    *graphDrawn = true;
    std::vector<std::pair<double, double>> drawn;
    for (size_t i = 0; i < k.grid.size(); ++i) {
      if (!std::isnan(k.grid[i]) && !std::isnan(k.density[i])) drawn.emplace_back(k.grid[i], k.density[i]);
    }
    if (atText.has_value()) {
      std::stable_sort(drawn.begin(), drawn.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    }
    ojson pts = ojson::array();
    double yTop = 0.0;
    double xLow = 0.0, xHigh = 0.0;
    for (size_t i = 0; i < drawn.size(); ++i) {
      const auto& [px, py] = drawn[i];
      pts.push_back(ojson::array({px, py}));
      yTop = std::max(yTop, py);
      xLow = (i == 0) ? px : std::min(xLow, px);
      xHigh = (i == 0) ? px : std::max(xHigh, px);
    }
    graph["type"] = "graph";
    graph["kind"] = "line";
    graph["title"] = optTitleStr(opts, "title").value_or("Kernel density estimate");
    graph["x_label"] = optTitleStr(opts, "xtitle").value_or(varTitle);
    graph["y_label"] = optTitleStr(opts, "ytitle").value_or(std::string("Density"));
    graph["note"] = optTitleStr(opts, "note")
                        .value_or("kernel = " + kc.name + ", bandwidth = " + bandwidthNoteText(k.bwidth));
    graph["x_var"] = var;
    graph["n"] = static_cast<int64_t>(smp.v.size());
    graph["bwidth"] = k.bwidth;
    graph["n_grid"] = k.n;
    graph["kernel"] = kc.name;
    if (!wtype.empty()) graph["weight"] = wtype + "=" + trimG(parsed.weightVar.value_or(std::string()));
    graph["points"] = pts;
    graph["x_min"] = xLow;
    graph["x_max"] = xHigh;
    graph["y_min"] = 0.0;
    graph["y_max"] = yTop;
    graph["subkind"] = "kdensity";
    graph["name"] = nullptr;  // filled by the dispatcher's name() capture (BS-010)
    if (sersets != nullptr) sersets->push_back(kdensityLineSerset(k, atText.has_value(), nRows));
    // normal / normopts(): the normal density with the summarize mean and sd
    // over the drawn range.
    const bool wantNormal = hasOpt(opts, "normal") || hasOpt(opts, "normopts");
    if (wantNormal && k.sd > 0 && !drawn.empty()) {
      graph["normal_points"] = normalCurve(xLow, xHigh, k.mean, k.sd, 1.0);
      graph["normal_mean"] = k.mean;
      graph["normal_sd"] = k.sd;
      if (sersets != nullptr) sersets->push_back(functionSerset(graph["normal_points"]));
    }
  }

  // kdensity is r-class: r(kernel bwidth n scale), also under nograph.
  eval::replaceRClass(session.eval);
  session.eval.rStrings.set("kernel", kc.name);
  session.eval.rValues.set("bwidth", k.bwidth);
  session.eval.rValues.set("n", static_cast<double>(k.n));
  session.eval.rValues.set("scale", k.scale);
  eval::declareRPosted(session.eval, "kdensity", {"kernel", "bwidth", "n", "scale"});

  if (!newNames.empty()) {
    // The density column is created before the point column (hk_kd_*_g
    // positions 16 15); one name with at() keeps the density only.
    const std::string densVar = newNames.back();
    std::vector<std::string> made = {densVar};
    std::optional<ojson> bad =
        writeGenVariable(session, densVar, paddedColumn(k.density, nRows), "double", "%10.0g", "density: " + varTitle);
    if (!bad && newNames.size() == 2) {
      bad = writeGenVariable(session, newNames.front(), paddedColumn(k.grid, nRows), "double", "%10.0g", varTitle);
      made.push_back(newNames.front());
    }
    if (bad) return *bad;
    if (pre != nullptr) pre->push_back(mutationResult(session, "kdensity", made, {}));
  }
  return graph;
}

// ===========================================================================
// line (also serves bare `twoway line yvar xvar [if ...]`)

ojson runGraphLine(Datastore& ds, const std::vector<std::string>& varlist,
                   const std::vector<std::pair<std::string, eval::OptValue>>& opts) {
  if (varlist.size() < 2) {
    return engineErrorBlockG("line requires two variables: `line yvar xvar`.", "syntax_error");
  }
  if (auto err = rejectByFacet(opts)) return *err;
  std::string yvar = varlist[0], xvar = varlist[1];
  if (!ds.has(yvar)) return engineErrorBlockG("variable " + yvar + " not found");
  if (!ds.has(xvar)) return engineErrorBlockG("variable " + xvar + " not found");

  auto y = numericColumn(ds, yvar);
  auto x = numericColumn(ds, xvar);

  std::vector<std::pair<double, double>> pairs;
  for (size_t i = 0; i < y.size(); ++i) {
    if (std::isfinite(y[i]) && std::isfinite(x[i])) pairs.emplace_back(x[i], y[i]);
  }
  // Points are connected in data order; only `sort` orders them by x ([G-3]
  // connect_options; BUG-404).
  if (hasOpt(opts, "sort")) {
    std::stable_sort(pairs.begin(), pairs.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
  }

  ojson points = ojson::array();
  double xMin = 0, xMax = 0, yMin = 0, yMax = 0;
  bool first = true;
  for (const auto& [xv, yv] : pairs) {
    points.push_back(ojson::array({xv, yv}));
    if (first) {
      xMin = xMax = xv;
      yMin = yMax = yv;
      first = false;
    } else {
      xMin = std::min(xMin, xv);
      xMax = std::max(xMax, xv);
      yMin = std::min(yMin, yv);
      yMax = std::max(yMax, yv);
    }
  }
  int64_t n = static_cast<int64_t>(pairs.size());

  ojson out;
  out["type"] = "graph";
  out["kind"] = "line";
  out["title"] = optTitleStr(opts, "title").value_or(yvar + " vs " + xvar);
  out["x_label"] = optTitleStr(opts, "xtitle").value_or(xvar);
  out["y_label"] = optTitleStr(opts, "ytitle").value_or(yvar);
  out["note"] = optTitleStr(opts, "note").value_or(std::string());
  out["x_var"] = xvar;
  out["y_var"] = yvar;
  out["n"] = n;
  out["points"] = points;
  out["x_min"] = xMin;
  out["x_max"] = xMax;
  out["y_min"] = yMin;
  out["y_max"] = yMax;
  out["name"] = nullptr;  // filled by the dispatcher's name() capture (BS-010)
  return out;
}

// ===========================================================================
// xtline

ojson runXtline(Datastore& ds, const std::vector<std::string>& varlist,
                const std::optional<std::string>& panelvar, const std::optional<std::string>& timevar,
                const std::vector<std::pair<std::string, eval::OptValue>>& opts) {
  if (varlist.empty()) {
    return engineErrorBlockG("xtline requires a variable: `xtline yvar`.", "syntax_error");
  }
  if (auto err = rejectByFacet(opts)) return *err;
  std::string yvar = varlist[0];
  if (!panelvar.has_value() || panelvar->empty()) {
    return engineErrorBlockG("xtline: panel variable not set. Run `xtset ivar [tvar]` first.",
                             "syntax_error");
  }
  if (!ds.has(yvar)) return engineErrorBlockG("variable " + yvar + " not found");
  if (!ds.has(*panelvar)) return engineErrorBlockG("panel variable " + *panelvar + " not found");
  bool haveTime = timevar.has_value() && !timevar->empty();
  if (haveTime && !ds.has(*timevar)) {
    return engineErrorBlockG("time variable " + *timevar + " not found");
  }

  auto yArr = numericColumn(ds, yvar);
  auto idArr = numericColumn(ds, *panelvar);
  std::vector<double> tArr;
  if (haveTime) {
    tArr = numericColumn(ds, *timevar);
  }
  // When no time variable is set, the x-axis is a per-panel-local row index
  // (0, 1, 2, ... within each panel), NOT a global running index across all
  // panels — matching the Python oracle (graph.py run_xtline: np.arange over
  // panel_mask.sum(), computed fresh per panel). The local counter is
  // assigned below, inside the per-uid loop, in dataset row order.

  std::set<double> uniqueIds;
  for (double v : idArr) {
    if (std::isfinite(v)) uniqueIds.insert(v);
  }

  ojson seriesArr = ojson::array();
  std::vector<double> allX, allY;
  int64_t nObsTotal = 0;

  for (double uid : uniqueIds) {
    std::vector<std::pair<double, double>> pts;
    double localIdx = 0.0;
    for (size_t i = 0; i < idArr.size(); ++i) {
      if (idArr[i] != uid) continue;
      double tv = haveTime ? tArr[i] : localIdx;
      localIdx += 1.0;
      if (!std::isfinite(yArr[i]) || !std::isfinite(tv)) continue;
      pts.emplace_back(tv, yArr[i]);
    }
    std::stable_sort(pts.begin(), pts.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

    ojson s;
    s["id"] = uid;
    std::string label;
    if (uid == std::floor(uid)) {
      label = std::to_string(static_cast<long long>(uid));
    } else {
      char buf[64];
      std::snprintf(buf, sizeof(buf), "%g", uid);
      label = buf;
    }
    s["label"] = label;
    ojson pointsJ = ojson::array();
    for (const auto& [tv, yv] : pts) {
      pointsJ.push_back(ojson::array({tv, yv}));
      allX.push_back(tv);
      allY.push_back(yv);
    }
    s["points"] = pointsJ;
    seriesArr.push_back(s);
    nObsTotal += static_cast<int64_t>(pts.size());
  }

  if (seriesArr.empty()) {
    return engineErrorBlockG("no non-missing observations for xtline " + yvar);
  }

  std::string xLabelDefault = haveTime ? *timevar : "obs";
  ojson out;
  out["type"] = "graph";
  out["kind"] = "xtline";
  out["title"] = optTitleStr(opts, "title").value_or(yvar + " by " + *panelvar + " over " +
                                                      (haveTime ? *timevar : "obs"));
  out["x_label"] = optTitleStr(opts, "xtitle").value_or(xLabelDefault);
  out["y_label"] = optTitleStr(opts, "ytitle").value_or(yvar);
  out["note"] = optTitleStr(opts, "note").value_or(std::string());
  out["y_var"] = yvar;
  out["panel_var"] = *panelvar;
  out["time_var"] = haveTime ? *timevar : "";
  out["n_panels"] = static_cast<int64_t>(seriesArr.size());
  out["n_obs"] = nObsTotal;
  // BUG-179 (P2-PREP): `xtline y, overlay` — Stata's option for drawing every
  // panel on ONE set of axes instead of a grid of small multiples. It was
  // parsed by the shared option grammar and then read nowhere here, so the
  // request vanished silently (rc 0, ungrouped default layout).
  //
  // This is a RENDERING choice, and rendering is client-side in this
  // architecture — the kernel ships plot DATA (piet/frontend/js/graph.js's
  // own top-of-file note). The data side is what this file owns, so the flag
  // is published here; piet/frontend/js/graph.js has no consumer for it yet
  // and renderXtline still draws its single overlaid axis regardless.
  // Reported as such rather than silently half-done: after this change the
  // option is *reachable*, not yet *honored end-to-end*.
  //
  // Emitted ONLY when actually requested, not as an unconditional `false`.
  // An optional key is this contract's established idiom for exactly this
  // (internal note piet/CONTRACT.md already spells kdensity's own extras
  // `subkind?`/`normal_points?`), a missing key and `false` are the same
  // falsy value to the JS consumer, and the conditional form keeps every
  // xtline block that did NOT ask for overlay byte-identical — so the
  // frozen internal capture xtline.json capture does not move.
  if (optBool(opts, "overlay").value_or(false)) out["overlay"] = true;
  out["series"] = seriesArr;
  out["x_min"] = allX.empty() ? 0.0 : *std::min_element(allX.begin(), allX.end());
  out["x_max"] = allX.empty() ? 0.0 : *std::max_element(allX.begin(), allX.end());
  out["y_min"] = allY.empty() ? 0.0 : *std::min_element(allY.begin(), allY.end());
  out["y_max"] = allY.empty() ? 0.0 : *std::max_element(allY.begin(), allY.end());
  out["name"] = nullptr;  // filled by the dispatcher's name() capture (BS-010)
  return out;
}

// ===========================================================================
// tsline

ojson runTsline(Datastore& ds, const std::vector<std::string>& varlist,
               const std::optional<std::string>& timevar,
               const std::vector<std::pair<std::string, eval::OptValue>>& opts) {
  if (varlist.empty()) {
    return engineErrorBlockG("tsline requires at least one variable: `tsline yvar [yvar2 ...]`.",
                             "syntax_error");
  }
  if (auto err = rejectByFacet(opts)) return *err;
  if (!timevar.has_value() || timevar->empty()) {
    return engineErrorBlockG("tsline: time variable not set. Run `tsset tvar` or `xtset ivar tvar` first.",
                             "syntax_error");
  }
  if (!ds.has(*timevar)) return engineErrorBlockG("time variable " + *timevar + " not found");
  for (const auto& v : varlist) {
    if (!ds.has(v)) return engineErrorBlockG("variable " + v + " not found");
  }

  auto tArr = numericColumn(ds, *timevar);

  ojson seriesArr = ojson::array();
  std::vector<double> allX, allY;
  int64_t nObsTotal = 0;

  for (const auto& yvar : varlist) {
    auto yArr = numericColumn(ds, yvar);
    std::vector<std::pair<double, double>> pts;
    for (size_t i = 0; i < tArr.size(); ++i) {
      if (!std::isfinite(tArr[i]) || !std::isfinite(yArr[i])) continue;
      pts.emplace_back(tArr[i], yArr[i]);
    }
    std::stable_sort(pts.begin(), pts.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

    ojson s;
    s["var"] = yvar;
    ojson pointsJ = ojson::array();
    for (const auto& [tv, yv] : pts) {
      pointsJ.push_back(ojson::array({tv, yv}));
      allX.push_back(tv);
      allY.push_back(yv);
    }
    s["points"] = pointsJ;
    seriesArr.push_back(s);
    nObsTotal += static_cast<int64_t>(pts.size());
  }

  std::string yLabelDefault = varlist.size() == 1 ? varlist[0] : "value";
  std::string titleDefault;
  if (varlist.size() == 1) {
    titleDefault = varlist[0] + " over " + *timevar;
  } else {
    std::string joined;
    for (size_t i = 0; i < varlist.size(); ++i) {
      if (i) joined += ", ";
      joined += varlist[i];
    }
    titleDefault = joined + " over " + *timevar;
  }

  ojson out;
  out["type"] = "graph";
  out["kind"] = "tsline";
  out["title"] = optTitleStr(opts, "title").value_or(titleDefault);
  out["x_label"] = optTitleStr(opts, "xtitle").value_or(*timevar);
  out["y_label"] = optTitleStr(opts, "ytitle").value_or(yLabelDefault);
  out["note"] = optTitleStr(opts, "note").value_or(std::string());
  out["time_var"] = *timevar;
  ojson varsJ = ojson::array();
  for (const auto& v : varlist) varsJ.push_back(v);
  out["vars"] = varsJ;
  out["n_series"] = static_cast<int64_t>(seriesArr.size());
  out["n_obs"] = nObsTotal;
  out["series"] = seriesArr;
  out["x_min"] = allX.empty() ? 0.0 : *std::min_element(allX.begin(), allX.end());
  out["x_max"] = allX.empty() ? 0.0 : *std::max_element(allX.begin(), allX.end());
  out["y_min"] = allY.empty() ? 0.0 : *std::min_element(allY.begin(), allY.end());
  out["y_max"] = allY.empty() ? 0.0 : *std::max_element(allY.begin(), allY.end());
  out["name"] = nullptr;  // filled by the dispatcher's name() capture (BS-010)
  return out;
}

// ===========================================================================
// twoway (parenthesised multi-layer form) — C++ port of graph.py's
// run_twoway_layers() + its per-plottype layer producers (_layer_rarea/
// _layer_line_or_connected/_layer_scatter). This handles the parenthesised
// `twoway (plottype ...) (plottype ...) ...` form. The bare single-plot
// twoway form (no parens) for scatter/line/kdensity/histogram stays on the
// existing runScatter/runGraphLine/etc path (dispatcher.cpp); the bare
// twoway-SPECIFIC plot types (rarea + the range-plot family) route back
// INTO this function as a single synthesised layer (BUG-129/BUG-130), so
// rarea reaches the band builder and unsupported types hit the same
// reject-before-output guard below.

namespace {

// nextPathG: local copy of the "generate a fresh scratch .hbin path" helper
// every M2+ handler file carries its own copy of (see handlers.cpp/
// handlers_m3.cpp/egen.cpp's identically-named nextPath — kept file-local
// per this codebase's established per-TU convention rather than factored
// into a shared header).
std::string nextPathG(const std::string& workdir, const std::string& prefix) {
  return mintScratchPath(workdir, prefix);  // BUG-389: one body, registered at mint
}

// A parsed opt value in the shape Python's `dict.get('key')` would see:
// unset (nullopt-equivalent), a bare-flag True, or a string. Local
// equivalent of twoway_opts.cpp's (anonymous-namespace, not header-exposed)
// MaybeOpt — used here only for the twoway BLOCK-level options (xscale/
// yscale/xline/yline/legend/...), a distinct concern from twoway_opts.cpp's
// per-LAYER style options.
struct MaybeOptFromOpts {
  bool present = false;
  bool isTrue = false;
  std::string str;
};

MaybeOptFromOpts getOptG(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                         const std::string& key) {
  for (const auto& [k, v] : opts) {
    if (k == key) {
      if (v.isBool) return {true, v.boolValue, ""};
      return {true, false, v.strValue};
    }
  }
  return {false, false, ""};
}

// _finite_mask + boolean-index (+ a stable argsort(x) when `sorted`): returns
// (xs, ys), missing rows dropped. A line or connected plot joins its points in
// data order unless `sort` is given ([G-3] connect_options; BUG-404).
std::vector<std::pair<double, double>> maskSortXY(const std::vector<double>& y,
                                                   const std::vector<double>& x, bool sorted) {
  std::vector<std::pair<double, double>> pts;  // (x, y)
  for (size_t i = 0; i < y.size(); ++i) {
    if (!std::isfinite(y[i]) || !std::isfinite(x[i])) continue;
    pts.emplace_back(x[i], y[i]);
  }
  if (sorted)
    std::stable_sort(pts.begin(), pts.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  return pts;
}

// _layer_rarea: yhi ylo xvar -> {kind:"rarea", band:[[x,ylo,yhi],...], style, n}.
ojson layerRarea(Datastore& ds, const std::vector<std::string>& varlist,
                  const std::vector<std::pair<std::string, eval::OptValue>>& opts, int layerIdx) {
  if (varlist.size() < 3) return ojson{{"_error", "rarea requires three variables: yhi ylo xvar"}};
  const std::string &yhiVar = varlist[0], &yloVar = varlist[1], &xvar = varlist[2];
  for (const auto& v : {yhiVar, yloVar, xvar}) {
    if (!ds.has(v)) return ojson{{"_error", "variable " + v + " not found"}};
  }
  auto yhiArr = numericColumn(ds, yhiVar);
  auto yloArr = numericColumn(ds, yloVar);
  auto xArr = numericColumn(ds, xvar);
  std::vector<std::tuple<double, double, double>> rows;  // (x, ylo, yhi)
  for (size_t i = 0; i < xArr.size(); ++i) {
    if (!std::isfinite(yhiArr[i]) || !std::isfinite(yloArr[i]) || !std::isfinite(xArr[i])) continue;
    rows.emplace_back(xArr[i], yloArr[i], yhiArr[i]);
  }
  std::stable_sort(rows.begin(), rows.end(),
                    [](const auto& a, const auto& b) { return std::get<0>(a) < std::get<0>(b); });
  ojson band = ojson::array();
  for (const auto& [x, ylo, yhi] : rows) band.push_back(ojson::array({x, ylo, yhi}));
  ojson out;
  out["kind"] = "rarea";
  out["band"] = band;
  out["style"] = twoway::layerStyle(opts, layerIdx, "rarea");
  out["n"] = static_cast<int64_t>(rows.size());
  return out;
}

// _layer_line_or_connected: yvar xvar -> {kind:plottype, points:[[x,y],...], style, n}.
ojson layerLineOrConnected(Datastore& ds, const std::vector<std::string>& varlist,
                           const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                           int layerIdx, const std::string& plottype) {
  if (varlist.size() < 2) {
    return ojson{{"_error", plottype + " requires two variables: yvar xvar"}};
  }
  const std::string &yvar = varlist[0], &xvar = varlist[1];
  for (const auto& v : {yvar, xvar}) {
    if (!ds.has(v)) return ojson{{"_error", "variable " + v + " not found"}};
  }
  auto pts = maskSortXY(numericColumn(ds, yvar), numericColumn(ds, xvar), hasOpt(opts, "sort"));
  ojson pointsJ = ojson::array();
  for (const auto& [x, y] : pts) pointsJ.push_back(ojson::array({x, y}));
  ojson out;
  out["kind"] = plottype;
  out["points"] = pointsJ;
  out["style"] = twoway::layerStyle(opts, layerIdx, plottype);
  out["n"] = static_cast<int64_t>(pts.size());
  return out;
}

// _layer_scatter: yvar xvar -> {kind:"scatter", points:[[x,y],...] (unsorted), style, n}.
ojson layerScatter(Datastore& ds, const std::vector<std::string>& varlist,
                   const std::vector<std::pair<std::string, eval::OptValue>>& opts, int layerIdx) {
  if (varlist.size() < 2) return ojson{{"_error", "scatter requires two variables: yvar xvar"}};
  const std::string &yvar = varlist[0], &xvar = varlist[1];
  for (const auto& v : {yvar, xvar}) {
    if (!ds.has(v)) return ojson{{"_error", "variable " + v + " not found"}};
  }
  auto yArr = numericColumn(ds, yvar);
  auto xArr = numericColumn(ds, xvar);
  ojson pointsJ = ojson::array();
  int64_t n = 0;
  for (size_t i = 0; i < yArr.size(); ++i) {
    if (!std::isfinite(yArr[i]) || !std::isfinite(xArr[i])) continue;
    pointsJ.push_back(ojson::array({xArr[i], yArr[i]}));
    ++n;
  }
  ojson out;
  out["kind"] = "scatter";
  out["points"] = pointsJ;
  out["style"] = twoway::layerStyle(opts, layerIdx, "scatter");
  out["n"] = n;
  return out;
}

// ---------------------------------------------------------------------------
// serset-twoway FIT: what the lfit and qfit plots and their ci forms compute
// ([G-2] graph twoway lfit, qfit, lfitci, qfitci). `regress y x [x2] [w] if touse`,
// then the x grid `gen x = min + (_n-1)*(max-min)/(points-1)` (c(type), so a
// FLOAT under the default), `predict yhat` (c(type)) at the grid; lfitci/qfitci
// add `predict se, stdp|stdf|stdr` and `lcl/ucl = yhat -/+ invttail(e(df_r),
// (100-level)/200)*se` (each a c(type) store, so yhat and se are rounded BEFORE
// the band arithmetic). A regressor collinear with the constant (or x^2 with
// x) is omitted as regress omits it: constant x fits the mean (w2al
// tf_lfit_const, log:1768; tf_lfitci_const). Weights: aw normalised to sum N,
// fw replicate (N = sum w), pw the aw point estimates with the robust V.
struct FitRequest {
  int degree = 1;  // 1 lfit/lfitci, 2 qfit/qfitci
  bool ci = false;
  int64_t points = 3;
  std::optional<double> rangeMin, rangeMax;
  double level = 95.0;
  std::string stdKind = "stdp";
  std::string wtype;  // "" | aweight | fweight | pweight
  bool genFloat = true;
};
struct FitResult {
  bool ok = false;
  bool noObs = false;
  std::string message;
  std::vector<double> grid, yhat, lcl, ucl;  // stored values (c(type))
  std::vector<double> yhatD;                 // unrounded predictions at the grid
  double b0 = 0.0, b1 = 0.0, b2 = 0.0;
  bool keptX = false, keptX2 = false;
};

FitResult computeFit(const std::vector<double>& y, const std::vector<double>& x,
                     const std::vector<double>* w, const FitRequest& req) {
  FitResult r;
  const bool hasW = w != nullptr && !req.wtype.empty();
  std::vector<double> ys, xs, ws;
  for (size_t i = 0; i < y.size() && i < x.size(); ++i) {
    if (!std::isfinite(y[i]) || !std::isfinite(x[i])) continue;
    double wi = 1.0;
    if (hasW) {
      wi = i < w->size() ? (*w)[i] : kNaN;
      if (!std::isfinite(wi) || wi <= 0.0) continue;
    }
    ys.push_back(y[i]);
    xs.push_back(x[i]);
    ws.push_back(wi);
  }
  const size_t n = ys.size();
  if (n == 0) {
    r.noObs = true;
    r.message = "no observations";
    return r;
  }
  double sumW = 0.0;
  for (double v : ws) sumW += v;
  double nEff = static_cast<double>(n);  // e(N)
  if (hasW && req.wtype == "fweight") {
    nEff = sumW;
  } else if (hasW) {
    // aweight/pweight: normalised to sum to N
    for (double& v : ws) v = v * static_cast<double>(n) / sumW;
  }
  // Candidate columns in Stata's order of precedence: _cons, x, x2.
  const int kAll = req.degree == 2 ? 3 : 2;
  auto colVal = [&](int j, double xv) -> double {
    if (j == 0) return 1.0;
    if (j == 1) return xv;
    return xv * xv;
  };
  // Weighted modified Gram-Schmidt in that order: a column whose residual is
  // (numerically) zero is collinear with the kept ones and omitted.
  std::vector<int> kept;
  std::vector<std::vector<double>> qcols;  // orthonormal (in the w inner product)
  for (int j = 0; j < kAll; ++j) {
    std::vector<double> v(n);
    double ss0 = 0.0;
    for (size_t i = 0; i < n; ++i) {
      v[i] = colVal(j, xs[i]);
      ss0 += ws[i] * v[i] * v[i];
    }
    for (const auto& q : qcols) {
      double dot = 0.0;
      for (size_t i = 0; i < n; ++i) dot += ws[i] * q[i] * v[i];
      for (size_t i = 0; i < n; ++i) v[i] -= dot * q[i];
    }
    double ss = 0.0;
    for (size_t i = 0; i < n; ++i) ss += ws[i] * v[i] * v[i];
    if (!(ss > 1e-13 * ss0) || ss0 == 0.0) continue;
    const double nrm = std::sqrt(ss);
    for (double& e : v) e /= nrm;
    qcols.push_back(std::move(v));
    kept.push_back(j);
  }
  const size_t k = kept.size();
  // Normal equations over the kept columns, solved by Cholesky (k <= 3) after
  // the rank decision above; refined once against the residual.
  std::vector<double> A(k * k, 0.0), bvec(k, 0.0);
  for (size_t i = 0; i < n; ++i) {
    for (size_t a = 0; a < k; ++a) {
      const double va = colVal(kept[a], xs[i]);
      bvec[a] += ws[i] * va * ys[i];
      for (size_t c = 0; c < k; ++c) A[a * k + c] += ws[i] * va * colVal(kept[c], xs[i]);
    }
  }
  // Inverse of A by Gauss-Jordan with partial pivoting.
  std::vector<double> inv(k * k, 0.0), M = A;
  for (size_t a = 0; a < k; ++a) inv[a * k + a] = 1.0;
  for (size_t c = 0; c < k; ++c) {
    size_t piv = c;
    for (size_t rr = c + 1; rr < k; ++rr)
      if (std::fabs(M[rr * k + c]) > std::fabs(M[piv * k + c])) piv = rr;
    if (M[piv * k + c] == 0.0) {
      r.message = "singular fit";
      return r;
    }
    if (piv != c) {
      for (size_t cc = 0; cc < k; ++cc) {
        std::swap(M[c * k + cc], M[piv * k + cc]);
        std::swap(inv[c * k + cc], inv[piv * k + cc]);
      }
    }
    const double d = M[c * k + c];
    for (size_t cc = 0; cc < k; ++cc) {
      M[c * k + cc] /= d;
      inv[c * k + cc] /= d;
    }
    for (size_t rr = 0; rr < k; ++rr) {
      if (rr == c) continue;
      const double f = M[rr * k + c];
      if (f == 0.0) continue;
      for (size_t cc = 0; cc < k; ++cc) {
        M[rr * k + cc] -= f * M[c * k + cc];
        inv[rr * k + cc] -= f * inv[c * k + cc];
      }
    }
  }
  std::vector<double> beta(k, 0.0);
  for (size_t a = 0; a < k; ++a)
    for (size_t c = 0; c < k; ++c) beta[a] += inv[a * k + c] * bvec[c];
  // One step of iterative refinement: beta += inv * X'W(y - X beta).
  {
    std::vector<double> g(k, 0.0);
    for (size_t i = 0; i < n; ++i) {
      double fit = 0.0;
      for (size_t a = 0; a < k; ++a) fit += beta[a] * colVal(kept[a], xs[i]);
      const double e = ys[i] - fit;
      for (size_t a = 0; a < k; ++a) g[a] += ws[i] * colVal(kept[a], xs[i]) * e;
    }
    for (size_t a = 0; a < k; ++a)
      for (size_t c = 0; c < k; ++c) beta[a] += inv[a * k + c] * g[c];
  }
  for (size_t a = 0; a < k; ++a) {
    if (kept[a] == 0) r.b0 = beta[a];
    if (kept[a] == 1) {
      r.b1 = beta[a];
      r.keptX = true;
    }
    if (kept[a] == 2) {
      r.b2 = beta[a];
      r.keptX2 = true;
    }
  }
  // Residual variance and V.
  double rss = 0.0, minX = xs[0], maxX = xs[0];
  std::vector<double> resid(n);
  for (size_t i = 0; i < n; ++i) {
    double fit = 0.0;
    for (size_t a = 0; a < k; ++a) fit += beta[a] * colVal(kept[a], xs[i]);
    resid[i] = ys[i] - fit;
    rss += ws[i] * resid[i] * resid[i];
    minX = std::min(minX, xs[i]);
    maxX = std::max(maxX, xs[i]);
  }
  const double dfr = nEff - static_cast<double>(k);
  const double s2 = dfr > 0 ? rss / dfr : kNaN;
  std::vector<double> V(k * k, 0.0);
  if (hasW && req.wtype == "pweight") {
    // robust: inv * (sum (w e)^2 x x') * inv * N/(N-k)
    std::vector<double> meat(k * k, 0.0);
    for (size_t i = 0; i < n; ++i) {
      const double we = ws[i] * resid[i];
      for (size_t a = 0; a < k; ++a)
        for (size_t c = 0; c < k; ++c)
          meat[a * k + c] += we * we * colVal(kept[a], xs[i]) * colVal(kept[c], xs[i]);
    }
    const double adj = dfr > 0 ? static_cast<double>(n) / dfr : kNaN;
    for (size_t a = 0; a < k; ++a)
      for (size_t c = 0; c < k; ++c) {
        double v = 0.0;
        for (size_t p = 0; p < k; ++p)
          for (size_t q = 0; q < k; ++q) v += inv[a * k + p] * meat[p * k + q] * inv[q * k + c];
        V[a * k + c] = v * adj;
      }
  } else {
    for (size_t a = 0; a < k * k; ++a) V[a] = inv[a] * s2;
  }
  // The grid.
  double lo = req.rangeMin.has_value() && std::isfinite(*req.rangeMin) ? *req.rangeMin
                                                                         : viaMacroText(minX);
  double hi = req.rangeMax.has_value() && std::isfinite(*req.rangeMax) ? *req.rangeMax
                                                                         : viaMacroText(maxX);
  const int64_t pts = std::max<int64_t>(req.points, 1);
  for (int64_t i = 0; i < pts; ++i) {
    const double gx = lo + (static_cast<double>(i) * (hi - lo)) / static_cast<double>(pts - 1);
    r.grid.push_back(storeAs(pts == 1 ? kNaN : gx, req.genFloat));
  }
  const double tval = req.ci ? eval::stataInvttail(dfr, (100.0 - req.level) / 200.0) : kNaN;
  for (double g : r.grid) {
    double xb = 0.0;
    std::vector<double> x0(k);
    for (size_t a = 0; a < k; ++a) {
      x0[a] = colVal(kept[a], g);
    }
    // predict's xb: the slope terms, then the constant.
    for (size_t a = 0; a < k; ++a)
      if (kept[a] != 0) xb += beta[a] * x0[a];
    for (size_t a = 0; a < k; ++a)
      if (kept[a] == 0) xb += beta[a];
    r.yhatD.push_back(xb);
    const double yf = storeAs(xb, req.genFloat);
    r.yhat.push_back(yf);
    if (req.ci) {
      double q = 0.0;
      for (size_t a = 0; a < k; ++a)
        for (size_t c = 0; c < k; ++c) q += x0[a] * V[a * k + c] * x0[c];
      double se2 = q;
      if (req.stdKind == "stdf") se2 = q + s2;
      if (req.stdKind == "stdr") se2 = s2 - q;
      const double se = storeAs(se2 >= 0 ? std::sqrt(se2) : kNaN, req.genFloat);
      r.lcl.push_back(storeAs(yf - tval * se, req.genFloat));
      r.ucl.push_back(storeAs(yf + tval * se, req.genFloat));
    }
  }
  r.ok = true;
  return r;
}

// The fit options a layer carries: n(), range(), level(), stdp/stdf/stdr.
// Refusals worded as Stata words them for twoway lfit and lfitci.
struct FitOptsParsed {
  FitRequest req;
  std::optional<ojson> err;
};
FitOptsParsed parseFitOptions(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                              const std::string& pt, bool genFloat) {
  FitOptsParsed out;
  FitRequest& q = out.req;
  q.degree = (pt == "qfit" || pt == "qfitci") ? 2 : 1;
  q.ci = (pt == "lfitci" || pt == "qfitci");
  q.genFloat = genFloat;
  q.points = q.ci ? 100 : 3;
  if (auto nv = optLast(opts, "n")) {
    const auto d = parseNumberG(*nv);
    if (!d.has_value() || *d != std::floor(*d)) {
      out.err = stataRefusal("option n() incorrectly specified", 198);
      return out;
    }
    q.points = static_cast<int64_t>(*d);
  }
  // qfit: points 3 (the lfit default) becomes 100.
  if (q.degree == 2 && q.points == 3) q.points = 100;
  if (auto rv = optLast(opts, "range")) {
    const auto toks = splitWsG(*rv);
    if (toks.size() != 2) {
      out.err = stataRefusal("option range() incorrectly specified", 198);
      return out;
    }
    auto num = [](const std::string& t) -> std::optional<double> {
      if (t == ".") return kNaN;
      return parseNumberG(t);
    };
    const auto a = num(toks[0]), b = num(toks[1]);
    if (!a.has_value() || !b.has_value()) {
      out.err = stataRefusal("option range() incorrectly specified", 198);
      return out;
    }
    q.rangeMin = *a;
    q.rangeMax = *b;
  }
  if (q.ci) {
    if (auto lv = optLast(opts, "level")) {
      const auto d = parseNumberG(*lv);
      if (!d.has_value() || *d < 10.0 || *d > 99.99) {
        out.err = stataRefusal("level() must be between 10 and 99.99 inclusive", 198);
        return out;
      }
      q.level = *d;
    }
    std::vector<std::string> std;
    for (const char* k : {"stdp", "stdf", "stdr"})
      if (hasOpt(opts, k)) std.push_back(k);
    if (std.size() > 1) {
      out.err = stataRefusal("may not combine " + std[0] + " " + std[1], 198);
      return out;
    }
    if (!std.empty()) q.stdKind = std[0];
  }
  return out;
}

// The fit's serset: `__tmp <xname>` (lfit/qfit) or `__tmp __tmp __tmp <xname>`
// (lfitci/qfitci), every column c(type), sorted by x, always its own serset
// (w2al tf_lfit_n log:1742-1749, tf_lfitci_lvl log:2068-2098).
serset::Serset fitSerset(const FitResult& f, const std::string& xname, bool ci, bool genFloat) {
  const std::string t = genFloat ? "float" : "double";
  std::vector<size_t> order(f.grid.size());
  for (size_t i = 0; i < order.size(); ++i) order[i] = i;
  std::stable_sort(order.begin(), order.end(), [&f](size_t a, size_t b) {
    const double x = f.grid[a], y = f.grid[b];
    if (std::isnan(x) || std::isnan(y)) return !std::isnan(x) && std::isnan(y);
    return x < y;
  });
  auto pick = [&order](const std::vector<double>& v) {
    std::vector<double> o;
    for (size_t i : order) o.push_back(i < v.size() ? v[i] : kNaN);
    return o;
  };
  serset::Serset s;
  s.n = static_cast<int64_t>(f.grid.size());
  int ti = 0;
  s.cols.push_back(serset::numericColumn(serset::tempName(ti++), t, pick(f.yhat)));
  if (ci) {
    s.cols.push_back(serset::numericColumn(serset::tempName(ti++), t, pick(f.lcl)));
    s.cols.push_back(serset::numericColumn(serset::tempName(ti++), t, pick(f.ucl)));
  }
  s.cols.push_back(serset::numericColumn(xname, t, pick(f.grid)));
  return s;
}

// BUG-099 + serset-twoway FIT: `lfit|qfit|lfitci|qfitci yvar xvar` -- the fit
// kernel above feeds both the drawn layer and the serset. lfit draws its
// straight line by the grid's two end points (BUG-099's 2-point shape); qfit
// draws every grid point (n() / range() now applied: they were accepted and
// ignored, so the drawn line answered a different question); lfitci/qfitci add
// the confidence band [x, lcl, ucl]. `coef` keeps BUG-099's {cons, slope} /
// {cons, b1, b2}. A refusal comes back under "_refusal" (an error block).
ojson layerFit(Datastore& ds, const twoway::LayerSpec& spec, int layerIdx, const std::string& pt,
               bool genFloat, const std::vector<double>* w, const std::string& wtype,
               std::optional<serset::Serset>* sersetOut) {
  const auto& varlist = spec.varlist;
  if (varlist.size() < 2) return ojson{{"_error", pt + " requires two variables: yvar xvar"}};
  if (varlist.size() > 2) {
    return ojson{{"_refusal", stataRefusal("too many variables specified", 103)}};
  }
  const std::string &yvar = varlist[0], &xvar = varlist[1];
  for (const auto& v : {yvar, xvar}) {
    if (!ds.has(v)) return ojson{{"_error", "variable " + v + " not found"}};
  }
  FitOptsParsed fo = parseFitOptions(spec.opts, pt, genFloat);
  if (fo.err) return ojson{{"_refusal", *fo.err}};
  fo.req.wtype = wtype;
  const FitResult f = computeFit(numericColumn(ds, yvar), numericColumn(ds, xvar), w, fo.req);
  if (!f.ok) return ojson{{"_error", pt + ": " + f.message}};
  const bool ci = fo.req.ci;
  ojson pointsJ = ojson::array();
  int64_t nUsed = 0;
  {
    // the number of observations the fit used (BUG-099's `n`)
    auto yv = numericColumn(ds, yvar), xv = numericColumn(ds, xvar);
    for (size_t i = 0; i < yv.size(); ++i)
      if (std::isfinite(yv[i]) && std::isfinite(xv[i])) ++nUsed;
  }
  if (pt == "lfit" && !f.grid.empty()) {
    pointsJ.push_back(ojson::array({f.grid.front(), f.yhatD.front()}));
    pointsJ.push_back(ojson::array({f.grid.back(), f.yhatD.back()}));
  } else {
    for (size_t i = 0; i < f.grid.size(); ++i) {
      if (std::isnan(f.grid[i]) || std::isnan(f.yhatD[i])) continue;
      pointsJ.push_back(ojson::array({f.grid[i], f.yhatD[i]}));
    }
  }
  ojson out;
  out["kind"] = pt;
  out["points"] = pointsJ;
  if (ci) {
    ojson band = ojson::array();
    for (size_t i = 0; i < f.grid.size(); ++i) {
      if (std::isnan(f.grid[i]) || std::isnan(f.lcl[i]) || std::isnan(f.ucl[i])) continue;
      band.push_back(ojson::array({f.grid[i], f.lcl[i], f.ucl[i]}));
    }
    out["band"] = band;
    out["level"] = fo.req.level;
  }
  if (fo.req.degree == 1) {
    out["coef"] = ojson{{"cons", f.b0}, {"slope", f.b1}};
  } else {
    out["coef"] = ojson{{"cons", f.b0}, {"b1", f.b1}, {"b2", f.b2}};
  }
  out["style"] = twoway::layerStyle(spec.opts, layerIdx, pt);
  out["n"] = nUsed;
  if (sersetOut != nullptr) *sersetOut = fitSerset(f, xvar, ci, genFloat);
  return out;
}

// serset-twoway RNG: the range plots (rcap, rcapsym, rspike, rline,
// rconnected, rscatter: `y1 y2 x`) and the base plots (area, spike, dropline,
// bar: `y x`, drawn to base(), default 0). The variables are taken in the order
// written; the serset comes from the generic data builder (layerDataPlotSpec),
// so nothing plot-specific reaches it (w2al tf_rcap_079y `y xaxis lo hi`
// log:2279, tf_area_nosort log:2621). base() must be a number: Stata's three
// lines and r(4026) (tf_r_area_base, log:5347-5353).
ojson layerRangeOrBase(Datastore& ds, const twoway::LayerSpec& spec, int layerIdx,
                       const std::string& pt) {
  static const std::set<std::string> kRange = {"rcap", "rcapsym", "rspike", "rline", "rconnected",
                                               "rscatter"};
  const bool isRange = kRange.count(pt) != 0;
  const size_t need = isRange ? 3 : 2;
  const auto& vl = spec.varlist;
  if (vl.size() < need) {
    std::string listed;
    for (const auto& v : vl) listed += (listed.empty() ? "" : " ") + v;
    return ojson{{"_refusal", stataRefusal(pt + " requires " + std::to_string(need) + " variables: " + listed, 102)}};
  }
  if (isRange && vl.size() > 3) {
    return ojson{{"_refusal", stataRefusal("too many variables specified", 103)}};
  }
  double base = 0.0;
  if (!isRange) {
    if (auto b = optLast(spec.opts, "base")) {
      const std::string bt = trimG(*b);
      const auto d = parseNumberG(bt);
      if (!d.has_value()) {
        return ojson{{"_refusal", stataRefusal("option base() incorrectly specified, " + bt +
                                                   " is not a number\n" + bt + " not found\nrvalue invalid",
                                               4026)}};
      }
      base = *d;
    }
  }
  ojson out;
  out["kind"] = pt;
  out["style"] = twoway::layerStyle(spec.opts, layerIdx, isRange ? "rarea" : pt);
  if (isRange) {
    const auto a = numericColumn(ds, vl[0]), b = numericColumn(ds, vl[1]), x = numericColumn(ds, vl[2]);
    ojson segs = ojson::array();
    for (size_t i = 0; i < x.size(); ++i) {
      if (!std::isfinite(a[i]) || !std::isfinite(b[i]) || !std::isfinite(x[i])) continue;
      segs.push_back(ojson::array({x[i], a[i], b[i]}));
    }
    out["n"] = static_cast<int64_t>(segs.size());
    out["segments"] = segs;
  } else {
    // one y (area/spike/dropline/bar `y x`); a further y would be another series
    const auto y = numericColumn(ds, vl[0]), x = numericColumn(ds, vl[vl.size() - 1]);
    std::vector<std::pair<double, double>> pts;
    for (size_t i = 0; i < x.size(); ++i) {
      if (!std::isfinite(y[i]) || !std::isfinite(x[i])) continue;
      pts.emplace_back(x[i], y[i]);
    }
    if (hasOpt(spec.opts, "sort")) {
      std::stable_sort(pts.begin(), pts.end(), [](const auto& p, const auto& q) { return p.first < q.first; });
    }
    ojson pj = ojson::array();
    for (const auto& [px, py] : pts) pj.push_back(ojson::array({px, py}));
    out["points"] = pj;
    out["base"] = base;
    out["n"] = static_cast<int64_t>(pts.size());
  }
  return out;
}

// serset-twoway FN: `twoway function [[y] =] f(x)` ([G-2] graph twoway function):
// n() points (default 300) over range() (default 0
// 1; two numbers, sorted, or a numeric variable whose min and max over the plot's
// if-sample are used), x_1 = min and x_i = min + delta*(i-1) in double, y =
// f(x) in double with the expression evaluated by the row evaluator over a frame
// holding only the grid column (so a dataset variable named x cannot leak in);
// missing where f is undefined. Serset `__tmp __tmp` = (y, x) double double,
// sorted by x, its own serset (w2al tf_fn_miss log:2919-2928, tf_fn_rangevar
// :2948-2952). No expression: `expression required`, r(100) (tf_r_fn_empty); an
// unknown function: `error in expression: <exp>`, r(133) (tf_r_fn_badfn).
ojson layerFunction(SessionState& session, Datastore& ds, const twoway::LayerSpec& spec, int layerIdx,
                    std::optional<serset::Serset>* sersetOut) {
  std::string expr = trimG(spec.rawArgs);
  if (expr.empty()) return ojson{{"_refusal", stataRefusal("expression required", 100)}};
  // [[y] =] expression
  {
    static const std::regex reLhs(R"(^([A-Za-z_][A-Za-z0-9_]*)?\s*=\s*(.*)$)");
    std::smatch m;
    // `y = f`, `= f`; not `==` and not a name followed by an operator
    if (std::regex_match(expr, m, reLhs) && !(m.str(2).rfind("=", 0) == 0)) {
      expr = trimG(m.str(2));
    }
  }
  if (expr.empty()) return ojson{{"_refusal", stataRefusal("nothing found where expression expected", 111)}};
  const auto& opts = spec.opts;
  int64_t n = 300;
  if (auto nv = optLast(opts, "n")) {
    const auto d = parseNumberG(*nv);
    if (!d.has_value() || *d != std::floor(*d)) {
      return ojson{{"_refusal", stataRefusal("option n() incorrectly specified", 198)}};
    }
    n = static_cast<int64_t>(*d);
  }
  if (n < 1) return ojson{{"_refusal", stataRefusal("option n() requires a positive integer", 198)}};
  double lo = 0.0, hi = 1.0;
  if (auto rv = optLast(opts, "range")) {
    const auto toks = splitWsG(*rv);
    if (toks.size() == 1 && ds.has(toks[0])) {
      const auto col = numericColumn(ds, toks[0]);
      double mn = kNaN, mx = kNaN;
      for (double v : col) {
        if (std::isnan(v)) continue;
        if (std::isnan(mn) || v < mn) mn = v;
        if (std::isnan(mx) || v > mx) mx = v;
      }
      if (std::isnan(mn)) return ojson{{"_refusal", stataRefusal("no observations", 2000)}};
      lo = mn;
      hi = mx;
    } else {
      const auto a = toks.size() == 2 ? parseNumberG(toks[0]) : std::nullopt;
      const auto b = toks.size() == 2 ? parseNumberG(toks[1]) : std::nullopt;
      if (!a.has_value() || !b.has_value()) {
        return ojson{{"_refusal", stataRefusal("option range() requires two numbers", 198)}};
      }
      lo = std::min(*a, *b);
      hi = std::max(*a, *b);
    }
  }
  const double delta = n == 1 ? kNaN : (hi - lo) / static_cast<double>(n - 1);
  std::vector<double> grid(static_cast<size_t>(n));
  grid[0] = lo;
  for (int64_t i = 1; i < n; ++i) grid[static_cast<size_t>(i)] = lo + delta * static_cast<double>(i);
  // dropline(numlist): every value inside range()
  if (auto dl = optLast(opts, "dropline")) {
    for (const auto& t : splitWsG(*dl)) {
      const auto d = parseNumberG(t);
      if (!d.has_value() || *d < lo || *d > hi) {
        return ojson{{"_refusal", stataRefusal("some droplines not within range() option", 198)}};
      }
    }
  }
  std::string xname = "x";
  if (auto sc = optLast(opts, "subchar")) {
    if (!trimG(*sc).empty()) xname = trimG(*sc);
  }
  // Each grid point is substituted for the bare name x (a same-named function
  // call is spared) and the expression evaluated by the macro layer's evaluator,
  // which already answers Stata's missing for an out-of-domain argument (BUG-374(3):
  // sqrt(-1) is `.`).
  std::vector<double> y(static_cast<size_t>(n), kNaN);
  for (int64_t i = 0; i < n; ++i) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "(%.17g)", grid[static_cast<size_t>(i)]);
    const std::string e = eval::substituteBareName(expr, xname, buf);
    std::string raised;
    bool isStr = false;
    const std::optional<std::string> v = eval::evalArith(e, &session.eval, &raised, &isStr);
    if (!v.has_value() || isStr) {
      int rc = isStr ? 109 : 198;
      if (!raised.empty()) {
        static const std::regex reRc(R"(r\((\d+)\))");
        std::smatch m;
        if (std::regex_search(raised, m, reRc)) {
          rc = std::atoi(m.str(1).c_str());
        } else {
          rc = 133;  // an undefined name: Stata's `unknown function` for a call
        }
      }
      return ojson{{"_refusal", stataRefusal("error in expression: " + expr, rc)}};
    }
    const std::string t = trimG(*v);
    if (t.empty() || t[0] == '.') {
      y[static_cast<size_t>(i)] = kNaN;
    } else {
      y[static_cast<size_t>(i)] = std::strtod(t.c_str(), nullptr);
    }
  }
  ojson pts = ojson::array();
  for (size_t i = 0; i < y.size(); ++i) {
    if (std::isnan(grid[i]) || std::isnan(y[i])) continue;
    pts.push_back(ojson::array({grid[i], y[i]}));
  }
  ojson out;
  out["kind"] = "line";
  out["subkind"] = "function";
  out["points"] = pts;
  out["style"] = twoway::layerStyle(opts, layerIdx, "line");
  out["n"] = static_cast<int64_t>(pts.size());
  if (sersetOut != nullptr) {
    serset::Serset sf;
    sf.n = n;
    std::vector<size_t> order(static_cast<size_t>(n));
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&grid](size_t a, size_t b) {
      if (std::isnan(grid[a]) || std::isnan(grid[b])) return !std::isnan(grid[a]) && std::isnan(grid[b]);
      return grid[a] < grid[b];
    });
    std::vector<double> ys, xs;
    for (size_t i : order) {
      ys.push_back(y[i]);
      xs.push_back(grid[i]);
    }
    sf.cols.push_back(serset::numericColumn(serset::tempName(0), "double", std::move(ys)));
    sf.cols.push_back(serset::numericColumn(serset::tempName(1), "double", std::move(xs)));
    *sersetOut = std::move(sf);
  }
  return out;
}

// serset-twoway SCI: `scatteri y x [(#)] ["label"] ...` ([G-2] graph twoway
// scatteri): one point per y x pair, values stored
// as floats; a `(#)` clock position (0..12) and/or a quoted label after a pair.
// Serset y x float float, plus a byte position column when any point has one
// and a str# label column when any point has one; its own serset, in the order
// written (w2al tf_sci_008 log:3098-3100, tf_sci_lab log:3137-3139). recast(line)
// / connect(l) only change the drawing.
// serset-twoway HIST-5: `pci|pcarrowi y0 x0 y1 x1 [(#)] ["label"] ...` take the
// same path with four numbers per point ([G-2] graph twoway pci): serset
// y0 x0 y1 x1 float, then the position byte and
// the label str# when any point has them (w2aq hk_h_040addplot_s2 log:2040-2051,
// hk_h_064addplot_s2/_s3 log:2079-2107).
ojson layerScatteri(const twoway::LayerSpec& spec, int layerIdx, std::optional<serset::Serset>* sersetOut) {
  const std::string ptype = strutil::lower(spec.plottype);
  const size_t groupSize = (ptype == "pci" || ptype == "pcarrowi") ? 4 : 2;
  // Tokens: a quoted string ("..." or `"..."'), a (...) group, or a blank-free word.
  std::vector<std::pair<char, std::string>> toks;  // kind: 'q' quoted, 'p' paren, 'w' word
  const std::string& s = spec.rawArgs;
  size_t i = 0;
  while (i < s.size()) {
    if (std::isspace(static_cast<unsigned char>(s[i]))) {
      ++i;
      continue;
    }
    if (s[i] == '"') {
      const size_t e = s.find('"', i + 1);
      const size_t end = e == std::string::npos ? s.size() : e;
      toks.emplace_back('q', s.substr(i + 1, end - i - 1));
      i = end + 1;
    } else if (s[i] == '`' && i + 1 < s.size() && s[i + 1] == '"') {
      const size_t e = s.find("\"'", i + 2);
      const size_t end = e == std::string::npos ? s.size() : e;
      toks.emplace_back('q', s.substr(i + 2, end - i - 2));
      i = end + 2;
    } else if (s[i] == '(') {
      const size_t e = s.find(')', i + 1);
      const size_t end = e == std::string::npos ? s.size() : e;
      toks.emplace_back('p', trimG(s.substr(i + 1, end - i - 1)));
      i = end + 1;
    } else {
      size_t j = i;
      while (j < s.size() && !std::isspace(static_cast<unsigned char>(s[j]))) ++j;
      toks.emplace_back('w', s.substr(i, j - i));
      i = j;
    }
  }
  auto num = [](const std::string& t) -> std::optional<double> {
    if (t == ".") return kNaN;
    return parseNumberG(t);
  };
  std::vector<double> ys, xs, ybs, xbs, ps;
  std::vector<std::string> labels;
  bool anyPos = false, anyLbl = false;
  size_t k = 0;
  while (k < toks.size()) {
    if (k + groupSize > toks.size()) {
      return ojson{{"_refusal", stataRefusal("invalid point, " + toks[k].second, 198)}};
    }
    std::vector<double> vals;
    for (size_t q = 0; q < groupSize; ++q) {
      const auto v = num(toks[k + q].second);
      if (toks[k + q].first != 'w' || !v.has_value()) {
        return ojson{{"_refusal", stataRefusal("invalid point, " + toks[k + q].second, 198)}};
      }
      vals.push_back(*v);
    }
    const std::optional<double> yv = vals[0], xv = vals[1];
    if (groupSize == 4) {
      ybs.push_back(serset::toFloat(vals[2]));
      xbs.push_back(serset::toFloat(vals[3]));
    }
    k += groupSize;
    double pos = kNaN;
    std::string lbl;
    if (k < toks.size() && toks[k].first == 'p') {
      const auto pv = parseNumberG(toks[k].second);
      if (!pv.has_value() || *pv < 0 || *pv > 12 || *pv != std::floor(*pv)) {
        return ojson{{"_refusal", stataRefusal("invalid position, " + toks[k].second, 198)}};
      }
      pos = *pv;
      anyPos = true;
      ++k;
    }
    if (k < toks.size() && toks[k].first == 'q') {
      lbl = toks[k].second;
      anyLbl = true;
      ++k;
    }
    ys.push_back(serset::toFloat(*yv));
    xs.push_back(serset::toFloat(*xv));
    ps.push_back(pos);
    labels.push_back(lbl);
  }
  bool asLine = false;
  if (auto rc = optLast(spec.opts, "recast")) asLine = strutil::lower(trimG(*rc)) == "line";
  if (auto cn = optLast(spec.opts, "connect")) {
    const std::string c = strutil::lower(trimG(*cn));
    if (!c.empty() && c[0] == 'l') asLine = true;
  }
  ojson pts = ojson::array();
  for (size_t j = 0; j < ys.size(); ++j) {
    if (std::isnan(ys[j]) || std::isnan(xs[j])) continue;
    pts.push_back(ojson::array({xs[j], ys[j]}));
  }
  ojson out;
  if (groupSize == 4) {
    // drawn as segments (x0, y0) -> (x1, y1)
    ojson pc = ojson::array();
    for (size_t j = 0; j < ys.size(); ++j) pc.push_back(ojson::array({xs[j], ys[j], xbs[j], ybs[j]}));
    out["kind"] = ptype;
    out["pc"] = pc;
  } else {
    out["kind"] = asLine ? "line" : "scatter";
    out["subkind"] = "scatteri";
    out["points"] = pts;
  }
  out["style"] = twoway::layerStyle(spec.opts, layerIdx, asLine || groupSize == 4 ? "line" : "scatter");
  out["n"] = static_cast<int64_t>(ys.size());
  if (sersetOut != nullptr) {
    serset::Serset si;
    si.n = static_cast<int64_t>(ys.size());
    int t = 0;
    si.cols.push_back(serset::numericColumn(serset::tempName(t++), "float", ys));
    si.cols.push_back(serset::numericColumn(serset::tempName(t++), "float", xs));
    if (groupSize == 4) {
      si.cols.push_back(serset::numericColumn(serset::tempName(t++), "float", ybs));
      si.cols.push_back(serset::numericColumn(serset::tempName(t++), "float", xbs));
    }
    if (anyPos) si.cols.push_back(serset::numericColumn(serset::tempName(t++), "byte", ps));
    if (anyLbl) si.cols.push_back(serset::stringColumn(serset::tempName(t++), labels));
    *sersetOut = std::move(si);
  }
  return out;
}

// The rows of a one-variable plot layer that enter its estimate: the variable
// nonmissing and, with a layer weight, the weight nonmissing and nonzero (a row
// past the end of the weight column counts as a missing weight).
struct OneVarSample {
  std::vector<double> v, w;
};

OneVarSample oneVarLayerSample(Datastore& ds, const std::string& var, const std::vector<double>* w) {
  OneVarSample out;
  const std::vector<double> col = numericColumn(ds, var);
  for (size_t i = 0; i < col.size(); ++i) {
    if (std::isnan(col[i])) continue;
    if (w == nullptr) {
      out.v.push_back(col[i]);
      continue;
    }
    const double wi = i < w->size() ? (*w)[i] : kNaN;
    if (std::isnan(wi) || wi == 0.0) continue;
    out.v.push_back(col[i]);
    out.w.push_back(wi);
  }
  return out;
}

// A layer refusal, in the shape runTwowayLayers unwraps.
ojson layerRefusal(const std::string& message, int rc) { return ojson{{"_refusal", stataRefusal(message, rc)}}; }

// serset-twoway HKL: the `twoway histogram var` layer -- the binning rule over
// the layer sample, last occurrence of each option. start() is passed only
// when typed, so the closing row appears only then (w2al tf_hist_082 3 rows,
// tf_hist_085 `(.,0,-5)`).
ojson layerHistogram(Datastore& ds, const twoway::LayerSpec& spec, int layerIdx, bool genFloat,
                     int64_t nFull, const std::vector<double>* w, const std::string& wtype,
                     std::optional<serset::Serset>* sersetOut) {
  (void)wtype;  // runTwowayLayers has already refused anything but fweights
  if (spec.varlist.empty()) return layerRefusal("varlist required", 100);
  if (spec.varlist.size() > 1) return layerRefusal("too many variables specified", 103);
  const std::string& var = spec.varlist.front();
  const HistOpts ho = parseHistOpts(spec.opts, var, genFloat, /*firstWins=*/false);
  if (ho.err) return ojson{{"_refusal", *ho.err}};
  const OneVarSample smp = oneVarLayerSample(ds, var, w);
  const HistGenResult g = histogramGen(smp.v, w != nullptr ? &smp.w : nullptr, ho.spec, nFull, true);
  if (g.rc != 0) return layerRefusal(g.message, g.rc);
  ojson layer;
  layer["kind"] = "histogram";
  layer["bars"] = barList(g);
  layer["y_kind"] = ho.spec.type;
  layer["start"] = g.start;
  layer["width"] = g.width;
  layer["bin"] = g.bin;
  layer["style"] = twoway::layerStyle(spec.opts, layerIdx, "histogram");
  layer["n"] = static_cast<int64_t>(smp.v.size());
  if (sersetOut != nullptr) *sersetOut = barSerset(g, genFloat);
  return layer;
}

// serset-twoway HKL: the `twoway kdensity var` layer ([G-2] graph twoway
// kdensity) -- n() points (default 300) accumulated across range() (default
// the sample's min and max, not widened by the bandwidth: tf_kd_n), the
// kdensity estimate at them times area().
ojson layerKdensity(SessionState& session, Datastore& ds, const twoway::LayerSpec& spec,
                    int layerIdx, const std::vector<double>* w, const std::string& wtype,
                    std::optional<serset::Serset>* sersetOut) {
  if (spec.varlist.empty()) return layerRefusal("varlist required", 100);
  if (spec.varlist.size() > 1) return layerRefusal("too many variables specified", 103);
  const std::string& var = spec.varlist.front();
  const auto& opts = spec.opts;

  int64_t points = 300;
  if (const auto text = optLast(opts, "n")) {
    const std::optional<double> d = parseNumberG(*text);
    if (!d.has_value() || std::floor(*d) != *d) return layerRefusal("option n() not allowed", 198);  // probe tknf
    points = static_cast<int64_t>(*d);
  }
  const BandwidthChoice bw = chooseBandwidth(opts, "options width() and bwidth() may not be combined");
  if (bw.err) return ojson{{"_refusal", *bw.err}};
  double area = 1.0;
  if (const auto text = optLast(opts, "area")) {
    const std::optional<double> d = parseNumberG(*text);
    if (!d.has_value()) return layerRefusal("option area() incorrectly specified", 198);
    area = *d;
  }
  if (points < 1) return layerRefusal("option n() requires a positive integer", 198);
  if (area <= 0) return layerRefusal("option area() expects a positive number", 198);
  const KernelChoice kc = chooseKernel(opts, /*strictWords=*/false);
  if (kc.err) return ojson{{"_refusal", *kc.err}};

  const OneVarSample smp = oneVarLayerSample(ds, var, w);
  // The grid's ends.
  double lo = kNaN, hi = kNaN;
  auto widenTo = [&](double x) {
    if (std::isnan(x)) return;
    if (std::isnan(lo) || x < lo) lo = x;
    if (std::isnan(hi) || x > hi) hi = x;
  };
  const std::optional<std::string> rangeText = optLast(opts, "range");
  const std::vector<std::string> rangeWords = rangeText.has_value() ? splitWsG(*rangeText) : std::vector<std::string>{};
  if (rangeWords.size() == 1 && ds.has(rangeWords.front())) {
    // range(varname): that variable's extent over the rows where the plotted
    // variable is nonmissing (probe tkrv).
    const std::vector<double> rangeCol = numericColumn(ds, rangeWords.front());
    const std::vector<double> plotted = numericColumn(ds, var);
    for (size_t i = 0; i < rangeCol.size() && i < plotted.size(); ++i) {
      if (!std::isnan(plotted[i])) widenTo(rangeCol[i]);
    }
  } else if (!rangeWords.empty()) {
    // range(# #), in either order (probe tkrr); one or three numbers are
    // r(122) / r(123) as in Stata (probes tkr1, tkr3).
    std::vector<double> ends;
    for (const std::string& word : rangeWords) {
      const std::optional<double> d = parseNumberG(word);
      if (!d.has_value()) return layerRefusal("option range() requires two numbers", 198);
      ends.push_back(*d);
    }
    if (ends.size() < 2) return layerRefusal("option range() requires two numbers", 122);
    if (ends.size() > 2) return layerRefusal("option range() requires two numbers", 123);
    widenTo(ends[0]);
    widenTo(ends[1]);
  } else {
    for (double x : smp.v) widenTo(x);
  }

  const std::vector<double> grid = accumulatedGrid(lo, hi, points);
  KdensitySpec ks;
  ks.kernel = kc.name;
  ks.width = bw.h;
  ks.wtype = wtype;
  Datastore* full = session.datastore();
  const int64_t nFull = full != nullptr ? full->nRows() : ds.nRows();
  const KdensityResult k = kdensityEstimate(smp.v, smp.w, ks, nFull, &grid);
  std::vector<double> curve(grid.size(), kNaN);
  for (size_t i = 0; i < curve.size() && i < k.density.size(); ++i) curve[i] = k.density[i] * area;

  ojson pts = ojson::array();
  for (size_t i = 0; i < grid.size(); ++i) {
    if (!std::isnan(grid[i]) && !std::isnan(curve[i])) pts.push_back(ojson::array({grid[i], curve[i]}));
  }
  ojson layer;
  layer["kind"] = "kdensity";
  layer["points"] = pts;
  layer["kernel"] = kc.name;
  layer["bwidth"] = k.bwidth;
  layer["style"] = twoway::layerStyle(opts, layerIdx, "line");
  layer["n"] = static_cast<int64_t>(smp.v.size());
  if (sersetOut != nullptr && full != nullptr) *sersetOut = curveWithVariableSerset(session, *full, var, curve, grid);
  return layer;
}

// _parse_scale_range: extract (lo, hi) from `xscale(range(LO HI) ...)`.
std::optional<std::pair<double, double>> parseScaleRange(const MaybeOptFromOpts& scaleOpt) {
  if (!scaleOpt.present || scaleOpt.isTrue) return std::nullopt;
  static const std::regex reRange(R"(range\(\s*(-?\d+(?:\.\d+)?)\s+(-?\d+(?:\.\d+)?)\s*\))");
  std::smatch m;
  if (std::regex_search(scaleOpt.str, m, reRange)) {
    try {
      return std::make_pair(std::stod(m[1].str()), std::stod(m[2].str()));
    } catch (...) {
      return std::nullopt;
    }
  }
  return std::nullopt;
}

// _reflines: split a `xline(0 5 10)`-style option into a list of doubles.
ojson reflines(const MaybeOptFromOpts& raw) {
  ojson out = ojson::array();
  if (!raw.present || raw.isTrue) return out;
  std::string s = raw.str;
  size_t a = 0, b = s.size();
  auto isSp = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
  while (a < b && isSp(s[a])) ++a;
  while (b > a && isSp(s[b - 1])) --b;
  s = s.substr(a, b - a);
  static const std::regex reSplit(R"([,\s]+)");
  std::sregex_token_iterator it(s.begin(), s.end(), reSplit, -1), end;
  for (; it != end; ++it) {
    std::string tok = it->str();
    if (tok.empty()) continue;
    try {
      size_t pos = 0;
      double v = std::stod(tok, &pos);
      if (pos == tok.size()) out.push_back(v);
    } catch (...) {
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
// legend() content parser (BUG-796..799, FU-112/FU-113).
//
// The text of `legend(...)` used to be searched by regexes that ran over the
// RAW string, quotes included -- so `order(1 "a" 4 "All ages (pooled)")` was cut
// at the first `)` inside the label, a label containing `off`, `size(` or
// `cols(` bound to the wrong suboption, and `order(2 1)` (no text) vanished.
// There was no reusable top-level splitter in src/ or include/ (the only
// `splitTopLevel` is file-local to handlers_aer_tables.cpp), so this one is
// written here. It splits ONCE into top-level suboptions with a quote-aware
// (plain "..." and compound `"..."') and paren-balanced scan; every consumer
// below reads from that list.
// ---------------------------------------------------------------------------

struct LegendSub {
  std::string name;  // lower-cased suboption token; empty for a bare quoted string
  std::string arg;   // text between the matching parens, verbatim; empty when none
  bool hasParens = false;
};

// Returns the index one past the quoted string that starts at s[i] (a plain
// `"` or a compound `"`), or npos when s[i] does not start one. An unterminated
// quote consumes the rest of the text.
size_t skipLegendQuote(const std::string& s, size_t i) {
  if (i >= s.size()) return std::string::npos;
  if (s[i] == '"') {
    size_t j = s.find('"', i + 1);
    return j == std::string::npos ? s.size() : j + 1;
  }
  if (s[i] == '`' && i + 1 < s.size() && s[i + 1] == '"') {
    int depth = 1;
    size_t j = i + 2;
    while (j < s.size() && depth > 0) {
      if (s[j] == '`' && j + 1 < s.size() && s[j + 1] == '"') {
        ++depth;
        j += 2;
      } else if (s[j] == '"' && j + 1 < s.size() && s[j + 1] == '\'') {
        --depth;
        j += 2;
      } else {
        ++j;
      }
    }
    return j;
  }
  return std::string::npos;
}

// The text of a quoted string (the span returned by skipLegendQuote) without
// its delimiters.
std::string legendQuoteBody(const std::string& s, size_t i, size_t end) {
  if (s[i] == '"') {
    size_t hi = (end > i + 1 && s[end - 1] == '"') ? end - 1 : end;
    return s.substr(i + 1, hi - (i + 1));
  }
  size_t hi = (end >= i + 4 && s[end - 2] == '"' && s[end - 1] == '\'') ? end - 2 : end;
  return s.substr(i + 2, hi - (i + 2));
}

std::vector<LegendSub> splitLegendSubopts(const std::string& text) {
  std::vector<LegendSub> subs;
  size_t i = 0;
  const size_t n = text.size();
  auto isSp = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ','; };
  while (i < n) {
    if (isSp(text[i])) { ++i; continue; }
    LegendSub sub;
    size_t q = skipLegendQuote(text, i);
    if (q != std::string::npos) {  // a free-standing quoted string
      i = q;
      subs.push_back(sub);
      continue;
    }
    size_t j = i;
    while (j < n && !isSp(text[j]) && text[j] != '(' && text[j] != ')' && text[j] != '"' && text[j] != '`') ++j;
    if (j == i) { ++i; continue; }  // a stray `)` / quote char: step over it
    sub.name = strutil::lower(text.substr(i, j - i));
    i = j;
    if (i < n && text[i] == '(') {
      sub.hasParens = true;
      int depth = 1;
      size_t k = i + 1;
      while (k < n && depth > 0) {
        size_t qq = skipLegendQuote(text, k);
        if (qq != std::string::npos) { k = qq; continue; }
        if (text[k] == '(') ++depth;
        else if (text[k] == ')') --depth;
        ++k;
      }
      size_t close = (depth == 0) ? k - 1 : n;  // unbalanced: take the rest
      sub.arg = text.substr(i + 1, close - (i + 1));
      i = (depth == 0) ? k : n;
    }
    subs.push_back(sub);
  }
  return subs;
}

// One order() entry: a 1-based plot index and its key text, or a `-` entry:
// text inserted with no key ([G-3] legend_options orderinfo `{#|-} ["text"
// ["text" ...]]`). Each quoted string is one line of the text.
struct LegendOrderEntry {
  bool dash = false;
  int64_t index = 0;
  bool hasText = false;
  std::string text;  // the quoted strings, one per line, joined with "\n"
};

std::vector<LegendOrderEntry> parseLegendOrder(const std::string& inner) {
  std::vector<LegendOrderEntry> entries;
  size_t i = 0;
  const size_t n = inner.size();
  while (i < n) {
    char c = inner[i];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { ++i; continue; }
    size_t q = skipLegendQuote(inner, i);
    if (q != std::string::npos) {
      if (!entries.empty()) {
        LegendOrderEntry& e = entries.back();
        std::string body = legendQuoteBody(inner, i, q);
        // A `-` entry's "" inserts nothing (" " is a blank line), as Stata's
        // help says of order(1 - " " "Predicted" 2 3).
        if (!(e.dash && body.empty())) {
          if (e.hasText) e.text += "\n";
          e.text += body;
          e.hasText = true;
        }
      }
      i = q;
      continue;
    }
    if (c >= '0' && c <= '9') {
      size_t j = i;
      while (j < n && inner[j] >= '0' && inner[j] <= '9') ++j;
      LegendOrderEntry e;
      try {
        e.index = std::stoll(inner.substr(i, j - i));
      } catch (...) {
        e.index = 0;
      }
      entries.push_back(e);
      i = j;
      continue;
    }
    if (c == '-') {
      LegendOrderEntry e;
      e.dash = true;
      entries.push_back(e);
      ++i;
      continue;
    }
    ++i;  // anything else (`|`, stray text) is not an entry
  }
  return entries;
}

}  // namespace

// The legend block -- order, cols (rows when given), size, off -- from the text
// of legend(...); one top-level split (quote-aware, paren-balanced), nothing
// searched in the raw text: `off` is a standalone top-level token, so "Payoff"
// or `size(off)` inside a label or another suboption is not it. (Also called
// from handlers_aer_graphx.cpp's cdfplot block.) order() lists the keys as
// [plot index (0-based), text]: a bare index shows keyText(index), and one
// keyText has no plot for is dropped. A `-` entry is text with no key, [-1,
// text] (BUG-1311). Each quoted string is a line of the text, joined with "\n"
// (BUG-1311). cols() and rows() are also spelled by any prefix down to c() and
// r() ([G-3] legend_options `c:ols(#)`, `r:ows(#)`; BUG-1310). *hasOrder says
// whether order() was given.
ojson legendBlock(const std::string& legendText,
                  const std::function<std::optional<std::string>(int64_t)>& keyText, bool* hasOrder) {
  ojson legendOrder = ojson::array();
  bool legendOff = false;
  ojson legendCols = nullptr, legendRows = nullptr, legendSize = nullptr;
  bool sawOrder = false, sawCols = false, sawSize = false;
  auto prefixOf = [](const std::string& name, const std::string& full) {
    return !name.empty() && name.size() <= full.size() && full.compare(0, name.size(), name) == 0;
  };
  for (const auto& sub : splitLegendSubopts(legendText)) {
    if (sub.name == "off" && !sub.hasParens) {
      legendOff = true;
    } else if (sub.name == "order" && sub.hasParens && !sawOrder) {
      sawOrder = true;
      for (const auto& e : parseLegendOrder(sub.arg)) {
        if (e.dash) {
          if (e.hasText) legendOrder.push_back(ojson::array({-1, e.text}));
          continue;
        }
        if (e.index < 1) continue;
        std::string label;
        if (e.hasText) {
          label = e.text;
        } else {
          const std::optional<std::string> k = keyText(e.index - 1);
          if (!k.has_value()) continue;  // no such plot: ignored
          label = *k;
        }
        legendOrder.push_back(ojson::array({e.index - 1, label}));
      }
    } else if ((prefixOf(sub.name, "cols") || prefixOf(sub.name, "rows")) && sub.hasParens && !sawCols) {
      std::string a = strutil::trim(sub.arg);
      if (!a.empty() && std::all_of(a.begin(), a.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) {
        sawCols = true;
        (sub.name[0] == 'c' ? legendCols : legendRows) = std::stoi(a.substr(0, 9));
      }
    } else if (sub.name == "size" && sub.hasParens && !sawSize) {
      std::string a = strutil::trim(sub.arg);
      if (!a.empty()) {
        sawSize = true;
        legendSize = a;
      }
    }
  }
  ojson legend;
  legend["order"] = legendOrder;
  legend["cols"] = legendCols;
  if (!legendRows.is_null()) legend["rows"] = legendRows;
  legend["size"] = legendSize;
  legend["off"] = legendOff;
  if (hasOrder != nullptr) *hasOrder = sawOrder;
  return legend;
}

namespace {

// serset-twoway SS: a data plot's serset contribution (piet/serset.h rule (a)):
// its varlist in role order, then the mlabel() variable when the plot does not
// already hold it (w2al tf_sc_mlabel `y x s`, tf_tw_graph077 mlabel(y) -> `xaxis
// y`), over the layer's sample in data order. Plots drawn with `sort` get their
// own serset sorted by x, the last role variable.
std::optional<serset::PlotSpec> layerDataPlotSpec(SessionState& session, Datastore& ds,
                                                  const twoway::LayerSpec& spec,
                                                  const std::string& pt) {
  static const std::set<std::string> kDataPlotTypes = {
      "scatter", "line",   "connected", "rarea", "area",   "rcap",    "rcapsym", "rspike",
      "rline",   "rbar",   "rscatter",  "rconnected", "bar", "spike", "dropline", "dot"};
  if (kDataPlotTypes.count(pt) == 0) return std::nullopt;
  std::vector<std::string> vars = spec.varlist;
  const size_t roleCount = vars.size();
  // serset-twoway WT: the weight variable follows the plot's variables (w2al
  // tf_tw_072_s1 `y x w` byte, log:325).
  const std::string wvar = trimG(spec.weightExp);
  if (!spec.weightType.empty() && !wvar.empty() && ds.has(wvar) &&
      std::find(vars.begin(), vars.end(), wvar) == vars.end()) {
    vars.push_back(wvar);
  }
  if (auto ml = optLast(spec.opts, "mlabel")) {
    const std::string mv = trimG(*ml);
    if (!mv.empty() && ds.has(mv) && std::find(vars.begin(), vars.end(), mv) == vars.end()) {
      vars.push_back(mv);
    }
  }
  std::vector<int64_t> rows(static_cast<size_t>(ds.nRows()));
  for (size_t i = 0; i < rows.size(); ++i) rows[i] = static_cast<int64_t>(i);
  serset::PlotSpec p;
  for (const auto& v : vars) p.cols.push_back(serset::variableColumn(session, ds, v, rows));
  p.key = (spec.ifExpr.has_value() ? trimG(*spec.ifExpr) : std::string()) + "\x1f" +
          (spec.inRange.has_value() ? trimG(*spec.inRange) : std::string()) + "\x1f" +
          spec.weightType + "=" + wvar;
  p.sorted = hasOpt(spec.opts, "sort");
  p.sortCol = roleCount > 0 ? static_cast<int>(roleCount) - 1 : -1;
  return p;
}

}  // namespace

ojson runTwowayLayers(Datastore* dsPtr, SessionState& session,
                      const std::vector<twoway::LayerSpec>& layers,
                      const std::vector<std::pair<std::string, eval::OptValue>>& globalOpts,
                      const std::optional<std::string>& globalIf,
                      std::vector<serset::PlotSpec>* sersetPlots,
                      const std::optional<std::string>& globalIn) {
  if (dsPtr == nullptr) return ojson(eval::noDataLoaded());
  if (layers.empty()) return engineErrorBlockG("twoway: no layers specified", "syntax_error");
  if (auto err = rejectByFacet(globalOpts)) return *err;  // BS-052: by() is whole-graph, never per-layer

  // BUG-129 (graph-honesty): an unsupported/unrecognised twoway plot type is a
  // reject-BEFORE-output error — the WHOLE command fails, no partial plot,
  // exactly as the B.0.3 option contract rejects an unknown option before any
  // result is emitted. Pre-scan every layer's plot type up front so a lone
  // `(rcap ...)` no longer renders an empty framed plot that "succeeds" (rc=0)
  // and a `(scatter ...)(lfitci ...)` no longer draws the scatter with the
  // requested CI band silently missing (Doctrine ① — "answers a different
  // question"). Supported layer plot types = the six with real builders below.
  static const std::set<std::string> kSupportedLayerTypes = {
      "scatter", "line",  "connected", "lfit",   "qfit",    "lfitci",     "qfitci",
      "rarea",   "histogram", "kdensity", "rcap", "rcapsym", "rspike", "rline",
      "rconnected", "rscatter", "area", "spike", "dropline", "bar", "function", "scatteri",
      "pci", "pcarrowi"};
  for (const auto& spec : layers) {
    std::string pt = spec.plottype;
    std::transform(pt.begin(), pt.end(), pt.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (pt == "hist") pt = "histogram";
    // serset-twoway REF: a word that is no Stata twoway plot type at all is
    // r(198) `<word> is not a twoway plot type` (w2al tf_r_badtype,
    // log:5165-5172); a real Stata type PIET lacks keeps the honest r(199).
    static const std::set<std::string> kStataTwowayTypes = {
        "scatter", "line", "connected", "scatteri", "area", "bar", "spike", "dropline", "dot",
        "rarea", "rbar", "rspike", "rcap", "rcapsym", "rscatter", "rline", "rconnected",
        "pcspike", "pccapsym", "pcarrow", "pcbarrow", "pcscatter", "pci", "pcarrowi", "tsline",
        "tsrline", "contour", "contourline", "heatmap", "mband", "mspline", "lowess", "lpoly",
        "lpolyci", "lfit", "qfit", "fpfit", "lfitci", "qfitci", "fpfitci", "function",
        "histogram", "kdensity"};
    if (kSupportedLayerTypes.count(pt) == 0 && kStataTwowayTypes.count(pt) == 0) {
      return stataRefusal(pt + " is not a twoway plot type", 198);
    }
    if (kSupportedLayerTypes.count(pt) == 0) {
      return engineErrorBlockG(
          "twoway: plot type `" + pt +
              "' is not supported in this version of PIET — supported plot types: "
              "scatter line connected lfit qfit rarea.",
          "unsupported_command");
    }
  }

  // AER scope-2 W4 -- THE UNKNOWN-OPTION NET FOR THE MULTI-LAYER PATH.
  //
  // Measured on main before this landed: `twoway (scatter y x) (line y x),
  // xlabel(1 2) bogusopt(1)` was rc 0. The whole parenthesised/`||` route had no
  // net at all, on either plane, so a typed option name was accepted, dropped
  // and never mentioned -- the cardinal defect class, because the user gets a
  // plot and never learns the option did nothing. The single-plot arm
  // (dispatcher.cpp) has had a net since BUG-336; this is its twin.
  //
  // POSITION IS LOAD-BEARING, and it is why this sits here rather than at the
  // dispatcher's `merged`: the supported-plot-type pre-scan above owns a reject
  // about the COMMAND, and it must win. `twoway (rcap hi lo x, lstyle(ci))` has
  // to answer "plot type `rcap' is not supported", not "option `lstyle' not
  // allowed" -- the same ordering rule the bare arm's comment states, and the
  // same one four self-test legs already lock for `graph bar y, over(x)`.
  //
  // The vocabulary is read back from the registry rather than spelled again
  // here: `twoway_layer` is registered in dispatcher.cpp beside the plot-type
  // heads (`connected`, `lfit`, `rarea`, ...), which is the one copy of the set.
  // A non-command token in the registry has precedent (`mediate:tvar`).
  // rejectByFacet above already answered for `by()`, so a by() never reaches
  // this net and keeps its specific message.
  {
    const eval::OptVocab* lv = eval::findOptVocab("twoway_layer");
    if (lv != nullptr) {
      if (auto err = eval::rejectUnknownOptions(globalOpts, lv->known, lv->unimplemented)) {
        return ojson(*err);
      }
      for (const auto& spec : layers) {
        if (auto err = eval::rejectUnknownOptions(spec.opts, lv->known, lv->unimplemented)) {
          return ojson(*err);
        }
      }
    }
  }

  // serset-twoway REF: every plot's variables must exist (r(111) `variable
  // <v> not found`, the whole statement fails: w2al tf_r_novar, log:5180-5187)
  // and be numeric (r(109) `string variables not allowed in varlist;` / `<v> is
  // a string variable`: tf_r_sc_strx, tf_r_lfit_str, log:5315-5339). A layer
  // whose variable was missing used to be dropped and the rest drawn at rc 0.
  for (const auto& spec : layers) {
    // serset-twoway WT: the weights each plot type takes, else r(101) `<type>s
    // not allowed` (w2al tf_r_iw / tf_r_hist_pw / tf_r_kd_pw, log:5208-5245),
    // and the weight variable must exist (tf_r_wtvar `nosuchwt not found`).
    if (!spec.weightType.empty()) {
      std::string pt = strutil::lower(spec.plottype);
      if (pt == "hist") pt = "histogram";
      std::set<std::string> legal = {"aweight", "fweight", "pweight"};
      if (pt == "histogram") legal = {"fweight"};
      if (pt == "kdensity") legal = {"fweight", "aweight"};
      if (legal.count(spec.weightType) == 0) return stataRefusal(spec.weightType + "s not allowed", 101);
      const std::string wv = trimG(spec.weightExp);
      if (!dsPtr->has(wv)) return stataRefusal(wv + " not found", 111);
    }
    {
      const std::string ptl = strutil::lower(spec.plottype);
      if (ptl == "function" || ptl == "scatteri" || ptl == "pci" || ptl == "pcarrowi") continue;
    }
    for (const auto& v : spec.varlist) {
      if (!dsPtr->has(v)) return stataRefusal("variable " + v + " not found", 111);
      if (dsPtr->dtype(v) == "factor") {
        return stataRefusal("string variables not allowed in varlist;\n" + v + " is a string variable",
                            109);
      }
    }
  }

  ojson renderedLayers = ojson::array();
  std::vector<double> allX, allY;
  std::vector<std::string> layerErrors;

  for (size_t idx = 0; idx < layers.size(); ++idx) {
    const twoway::LayerSpec& spec = layers[idx];

    std::optional<std::string> effectiveIf;
    if (globalIf.has_value() && !globalIf->empty() && spec.ifExpr.has_value() && !spec.ifExpr->empty()) {
      effectiveIf = "(" + *globalIf + ") & (" + *spec.ifExpr + ")";
    } else if (globalIf.has_value() && !globalIf->empty()) {
      effectiveIf = globalIf;
    } else if (spec.ifExpr.has_value() && !spec.ifExpr->empty()) {
      effectiveIf = spec.ifExpr;
    }

    // serset-twoway IN: the layer's `in` and the global `in` select by position
    // in the full dataset and intersect each other and both ifs (w2al tf_sc_in
    // 8 rows log:770, tf_sc_ifin 9/4 rows :798-808, tf_q_globalin :4864-4868,
    // tf_q_both :4926-4934).
    std::optional<std::string> effectiveIn;
    {
      const bool li = spec.inRange.has_value() && !spec.inRange->empty();
      const bool gi = globalIn.has_value() && !globalIn->empty();
      if (li || gi) {
        const int64_t nAll = dsPtr->nRows();
        long long a = 1, b = nAll;
        for (const auto* r : {li ? &*spec.inRange : nullptr, gi ? &*globalIn : nullptr}) {
          if (r == nullptr) continue;
          const InRangeBounds bb = parseInRangeBounds(*r, nAll);
          if (!bb.ok) {
            InRangeFilterResult fr;
            fr.ok = false;
            fr.refusal = bb.refusal;
            return inRangeRejectError(fr, *r, "twoway");
          }
          a = std::max(a, bb.a);
          b = std::min(b, bb.b);
        }
        if (a > b) {
          // an empty intersection: no observation is plotted
          effectiveIf = std::string("0");
        } else {
          effectiveIn = std::to_string(a) + "/" + std::to_string(b);
        }
      }
    }

    Datastore* layerDs = dsPtr;
    std::string tmpPath;
    std::unique_ptr<Datastore> tmpDs;
    bool filterFailed = false;
    if (effectiveIn.has_value()) {
      try {
        InRangeFilterResult fr = applyIfAndInFilter(session, dsPtr->path(), effectiveIf, effectiveIn);
        if (!fr.ok) return inRangeRejectError(fr, *effectiveIn, "twoway");
        tmpDs = std::make_unique<Datastore>(fr.dataPath);
        layerDs = tmpDs.get();
        if (fr.dataPath != dsPtr->path()) cecon::native_unlink(fr.dataPath);
      } catch (const std::exception& e) {
        layerErrors.push_back("layer " + std::to_string(idx) + " (" + spec.plottype + "): " + e.what());
        filterFailed = true;
      }
    } else if (effectiveIf.has_value()) {
      tmpPath = nextPathG(session.workdir(), "twowayif");
      std::vector<std::string> args = {"eval", "--data", dsPtr->path(), "--keepif", *effectiveIf,
                                        "--out", tmpPath};
      try {
        runCobraJson(args);
        tmpDs = std::make_unique<Datastore>(tmpPath);
        layerDs = tmpDs.get();
      } catch (const std::exception& e) {
        layerErrors.push_back("layer " + std::to_string(idx) + " (" + spec.plottype + "): " + e.what());
        filterFailed = true;
      }
      if (!tmpPath.empty()) cecon::native_unlink(tmpPath);
    }
    if (filterFailed) continue;

    std::string pt = spec.plottype;
    std::transform(pt.begin(), pt.end(), pt.begin(), [](unsigned char c) { return std::tolower(c); });
    if (pt == "hist") pt = "histogram";
    // serset-twoway WT: the layer's weight over the layer sample.
    std::vector<double> layerW;
    const std::vector<double>* layerWp = nullptr;
    if (!spec.weightType.empty() && layerDs->has(trimG(spec.weightExp))) {
      layerW = numericColumn(*layerDs, trimG(spec.weightExp));
      layerWp = &layerW;
    }
    ojson lyr;
    if (pt == "histogram" || pt == "kdensity") {
      // serset-twoway HKL: the twoway histogram/kdensity plot types (their own
      // serset each, never shared).
      std::optional<serset::Serset> oneSet;
      Datastore* fullDs = session.datastore();
      const int64_t nFull = fullDs != nullptr ? fullDs->nRows() : layerDs->nRows();
      if (pt == "histogram") {
        lyr = layerHistogram(*layerDs, spec, static_cast<int>(idx), session.defaultStorageType != "double",
                             nFull, layerWp, spec.weightType, &oneSet);
      } else {
        lyr = layerKdensity(session, *layerDs, spec, static_cast<int>(idx), layerWp, spec.weightType,
                            &oneSet);
      }
      if (lyr.contains("_refusal")) return lyr["_refusal"];
      if (!lyr.contains("_error") && oneSet.has_value() && sersetPlots != nullptr) {
        serset::PlotSpec ps;
        ps.computed = std::move(*oneSet);
        sersetPlots->push_back(std::move(ps));
      }
    } else if (pt == "function" || pt == "scatteri" || pt == "pci" || pt == "pcarrowi") {
      // serset-twoway FN / SCI: their own serset each, never shared.
      std::optional<serset::Serset> oneSet;
      lyr = pt == "function" ? layerFunction(session, *layerDs, spec, static_cast<int>(idx), &oneSet)
                             : layerScatteri(spec, static_cast<int>(idx), &oneSet);
      if (lyr.contains("_refusal")) return lyr["_refusal"];
      if (!lyr.contains("_error") && oneSet.has_value() && sersetPlots != nullptr) {
        serset::PlotSpec ps;
        ps.computed = std::move(*oneSet);
        sersetPlots->push_back(std::move(ps));
      }
    } else if (pt == "rcap" || pt == "rcapsym" || pt == "rspike" || pt == "rline" ||
               pt == "rconnected" || pt == "rscatter" || pt == "area" || pt == "spike" ||
               pt == "dropline" || pt == "bar") {
      lyr = layerRangeOrBase(*layerDs, spec, static_cast<int>(idx), pt);
      if (lyr.contains("_refusal")) return lyr["_refusal"];
    } else if (pt == "rarea") {
      lyr = layerRarea(*layerDs, spec.varlist, spec.opts, static_cast<int>(idx));
    } else if (pt == "lfit" || pt == "qfit" || pt == "lfitci" || pt == "qfitci") {
      // BUG-099: was aliased straight into layerLineOrConnected -- a raw
      // connect-the-dots polyline through the DATA points, never an actual
      // fit. serset-twoway FIT: one kernel (computeFit) feeds the drawn layer
      // and the fit's own serset (see layerFit above).
      std::optional<serset::Serset> fitSet;
      lyr = layerFit(*layerDs, spec, static_cast<int>(idx), pt,
                     session.defaultStorageType != "double", layerWp, spec.weightType, &fitSet);
      if (lyr.contains("_refusal")) return lyr["_refusal"];
      if (!lyr.contains("_error") && fitSet.has_value() && sersetPlots != nullptr) {
        serset::PlotSpec ps;
        ps.computed = std::move(*fitSet);
        sersetPlots->push_back(std::move(ps));
      }
    } else if (pt == "line") {
      lyr = layerLineOrConnected(*layerDs, spec.varlist, spec.opts, static_cast<int>(idx), "line");
    } else if (pt == "connected") {
      lyr = layerLineOrConnected(*layerDs, spec.varlist, spec.opts, static_cast<int>(idx), "connected");
    } else if (pt == "scatter") {
      lyr = layerScatter(*layerDs, spec.varlist, spec.opts, static_cast<int>(idx));
    } else {
      // Unreachable: the reject-before-output pre-scan at the top of
      // runTwowayLayers already failed the whole command for any plot type
      // outside the supported set (BUG-129). Defensive guard only.
      return engineErrorBlockG("twoway: internal — unhandled plot type `" + pt + "'.",
                               "engine_error");
    }

    if (lyr.contains("_error")) {
      layerErrors.push_back("layer " + std::to_string(idx) + " (" + spec.plottype + "): " +
                            lyr["_error"].get<std::string>());
      continue;
    }

    if (sersetPlots != nullptr) {
      if (auto ps = layerDataPlotSpec(session, *layerDs, spec, pt)) sersetPlots->push_back(std::move(*ps));
    }

    std::string lkind = lyr.value("kind", std::string());
    if (lkind == "rarea" || lkind == "lfitci" || lkind == "qfitci") {
      for (const auto& row : lyr.value("band", ojson::array())) {
        allX.push_back(row[0].get<double>());
        allY.push_back(row[1].get<double>());
        allY.push_back(row[2].get<double>());
      }
    }
    if (lkind == "line" || lkind == "connected" || lkind == "scatter" || lkind == "lfit" ||
        lkind == "qfit" || lkind == "lfitci" || lkind == "qfitci" || lkind == "kdensity") {
      for (const auto& pt2 : lyr.value("points", ojson::array())) {
        allX.push_back(pt2[0].get<double>());
        allY.push_back(pt2[1].get<double>());
      }
    }
    if (lyr.contains("pc")) {
      for (const auto& row : lyr["pc"]) {
        allX.push_back(row[0].get<double>());
        allY.push_back(row[1].get<double>());
        allX.push_back(row[2].get<double>());
        allY.push_back(row[3].get<double>());
      }
    }
    if (lyr.contains("segments")) {
      for (const auto& row : lyr["segments"]) {
        allX.push_back(row[0].get<double>());
        allY.push_back(row[1].get<double>());
        allY.push_back(row[2].get<double>());
      }
    }
    if (lyr.contains("base") && lyr.contains("points")) {
      for (const auto& pt2 : lyr["points"]) {
        allX.push_back(pt2[0].get<double>());
        allY.push_back(pt2[1].get<double>());
      }
      allY.push_back(lyr.value("base", 0.0));
    }
    if (lkind == "histogram") {
      for (const auto& b : lyr.value("bars", ojson::array())) {
        allX.push_back(b.value("x0", 0.0));
        allX.push_back(b.value("x1", 0.0));
        allY.push_back(0.0);
        allY.push_back(b.value("y", 0.0));
      }
    }

    renderedLayers.push_back(lyr);
  }

  bool anyDataLayer = false;
  for (const auto& l : renderedLayers) {
    if (l.value("kind", std::string()) != "unsupported") {
      anyDataLayer = true;
      break;
    }
  }
  if (!anyDataLayer && !layerErrors.empty()) {
    std::string joined;
    for (size_t i = 0; i < layerErrors.size(); ++i) {
      if (i) joined += "; ";
      joined += layerErrors[i];
    }
    return engineErrorBlockG("twoway: all layers failed. " + joined, "engine_error");
  }

  double xMin, xMax, yMin, yMax;
  if (allX.empty()) {
    // No plottable points in any layer (e.g. save_empty_plot's all-missing
    // y) — Stata still draws the framed plot; use the explicit scale
    // range() if given, else a unit default. Never error.
    MaybeOptFromOpts xrOpt = getOptG(globalOpts, "xscale");
    MaybeOptFromOpts yrOpt = getOptG(globalOpts, "yscale");
    auto xr = parseScaleRange(xrOpt);
    auto yr = parseScaleRange(yrOpt);
    std::tie(xMin, xMax) = xr.value_or(std::make_pair(0.0, 1.0));
    std::tie(yMin, yMax) = yr.value_or(std::make_pair(0.0, 1.0));
  } else {
    xMin = *std::min_element(allX.begin(), allX.end());
    xMax = *std::max_element(allX.begin(), allX.end());
    yMin = *std::min_element(allY.begin(), allY.end());
    yMax = *std::max_element(allY.begin(), allY.end());
  }

  ojson out;
  out["type"] = "graph";
  out["kind"] = "twoway";
  out["title"] = optTitleStr(globalOpts, "title").value_or(std::string());
  out["subtitle"] = optTitleStr(globalOpts, "subtitle").value_or(std::string());
  out["x_label"] = optTitleStr(globalOpts, "xtitle").value_or(std::string());
  out["y_label"] = optTitleStr(globalOpts, "ytitle").value_or(std::string());
  out["note"] = optTitleStr(globalOpts, "note").value_or(std::string());
  out["x_min"] = xMin;
  out["x_max"] = xMax;
  out["y_min"] = yMin;
  out["y_max"] = yMax;
  out["layers"] = renderedLayers;

  ojson axes;
  axes["xline"] = reflines(getOptG(globalOpts, "xline"));
  axes["yline"] = reflines(getOptG(globalOpts, "yline"));
  {
    MaybeOptFromOpts xs = getOptG(globalOpts, "xscale");
    std::string xsLower = xs.present && !xs.isTrue ? xs.str : "";
    std::transform(xsLower.begin(), xsLower.end(), xsLower.begin(), [](unsigned char c) { return std::tolower(c); });
    axes["xscale_noline"] = xsLower.find("noline") != std::string::npos;
  }
  {
    MaybeOptFromOpts ys = getOptG(globalOpts, "yscale");
    std::string ysLower = ys.present && !ys.isTrue ? ys.str : "";
    std::transform(ysLower.begin(), ysLower.end(), ysLower.begin(), [](unsigned char c) { return std::tolower(c); });
    axes["yscale_noline"] = ysLower.find("noline") != std::string::npos;
  }
  {
    MaybeOptFromOpts xl = getOptG(globalOpts, "xlabel");
    std::string xlLower = xl.present && !xl.isTrue ? xl.str : "";
    std::transform(xlLower.begin(), xlLower.end(), xlLower.begin(), [](unsigned char c) { return std::tolower(c); });
    // strip() then compare, mirroring the oracle's `.strip().lower() == "none"`.
    size_t a = xlLower.find_first_not_of(" \t");
    size_t b = xlLower.find_last_not_of(" \t");
    std::string trimmed = (a == std::string::npos) ? std::string() : xlLower.substr(a, b - a + 1);
    axes["xlabel_none"] = (trimmed == "none");
  }
  {
    MaybeOptFromOpts yl = getOptG(globalOpts, "ylabel");
    std::string ylLower = yl.present && !yl.isTrue ? yl.str : "";
    std::transform(ylLower.begin(), ylLower.end(), ylLower.begin(), [](unsigned char c) { return std::tolower(c); });
    size_t a = ylLower.find_first_not_of(" \t");
    size_t b = ylLower.find_last_not_of(" \t");
    std::string trimmed = (a == std::string::npos) ? std::string() : ylLower.substr(a, b - a + 1);
    axes["ylabel_none"] = (trimmed == "none");
  }
  out["axes"] = axes;

  // Legend: legend(order(2 "pooled" 4 "h1") cols(1) size(vsmall)) / legend(off).
  MaybeOptFromOpts legendRaw = getOptG(globalOpts, "legend");
  bool labelsSynced = false;
  // A bare index keeps the key the legend shows for that plot without order():
  // the plot's y-variable label (or name); the fitted-line plot types read
  // "Fitted values", as in Stata.
  auto keyText = [&](int64_t idx) -> std::optional<std::string> {
    if (idx < 0 || static_cast<size_t>(idx) >= layers.size()) return std::nullopt;
    const twoway::LayerSpec& ls = layers[static_cast<size_t>(idx)];
    std::string pt = strutil::lower(ls.plottype);
    if (pt == "lfit" || pt == "qfit") return std::string("Fitted values");
    if (ls.varlist.empty()) return std::string();
    // eval.varLabels is a projection refreshed only on demand (see
    // handlers_aer_tables.cpp's esttab `label` note); pay for it once, and
    // only when a bare index actually needs a default.
    if (!labelsSynced) {
      session.syncEvalVarLabels();
      labelsSynced = true;
    }
    std::string label = session.eval.varLabels.get(ls.varlist[0], std::string());
    return label.empty() ? ls.varlist[0] : label;
  };
  out["legend"] = legendBlock(legendRaw.present && !legendRaw.isTrue ? legendRaw.str : std::string(), keyText,
                              nullptr);

  out["name"] = nullptr;  // filled by the dispatcher's name() capture
  int64_t nObs = 0;
  for (const auto& l : renderedLayers) nObs += l.value("n", 0);
  out["n_obs"] = nObs;
  // BS-047 (BETA A4): session.graphicsOff now genuinely tracks `set graphics
  // on|off` (dispatcher.cpp's `set` arm) — previously always false since
  // SessionState had no such field (see the superseded comment this
  // replaces).
  out["suppressed"] = session.graphicsOff;
  return out;
}

// ===========================================================================
// marginsplot

ojson runMarginsplot(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                     SessionState& session) {
  const double kZ975 = 1.959963984540054;
  // BUG-1248: with no margins result to plot StataBE 19.5 answers "previous
  // command was not margins" r(301), with or without estimates or data in memory.
  if (!session.lastMargins.has_value()) {
    return ojson(eval::errorBlock("engine_error", "previous command was not margins",
                                  std::nullopt, std::nullopt, std::nullopt, "r(301)"));
  }
  const ojson& raw = *session.lastMargins;
  ojson coefs = raw.value("coefs", ojson::array());

  if (!session.lastMarginsDydx.empty()) {
    std::set<std::string> dydxSet(session.lastMarginsDydx.begin(), session.lastMarginsDydx.end());
    ojson filtered = ojson::array();
    for (const auto& c : coefs) {
      if (c.contains("name") && dydxSet.count(c["name"].get<std::string>())) filtered.push_back(c);
    }
    coefs = filtered;
  }
  if (coefs.empty()) {
    return engineErrorBlockG("marginsplot: margins result has no estimates to plot.");
  }

  std::string model = raw.value("model", std::string());
  int64_t nObs = raw.contains("n_obs") ? raw["n_obs"].get<int64_t>() : 0;

  ojson points = ojson::array();
  std::vector<double> allLo, allHi;
  for (const auto& c : coefs) {
    std::string name = c.value("name", std::string("?"));
    ojson bVal = c.contains("dydx") ? c["dydx"] : (c.contains("b") ? c["b"] : ojson(nullptr));
    ojson seVal = c.contains("se") ? c["se"] : ojson(nullptr);
    if (bVal.is_null() || seVal.is_null()) continue;
    double b, se;
    try {
      b = bVal.get<double>();
      se = seVal.get<double>();
    } catch (...) {
      continue;
    }
    double ciLo = b - kZ975 * se, ciHi = b + kZ975 * se;
    ojson p;
    p["name"] = name;
    p["b"] = b;
    p["se"] = se;
    p["ci_lo"] = ciLo;
    p["ci_hi"] = ciHi;
    points.push_back(p);
    allLo.push_back(ciLo);
    allHi.push_back(ciHi);
  }
  if (points.empty()) {
    return engineErrorBlockG("marginsplot: cannot build plot — no numeric AME estimates found.");
  }

  double yMin = *std::min_element(allLo.begin(), allLo.end());
  double yMax = *std::max_element(allHi.begin(), allHi.end());
  double span = (yMax != yMin) ? (yMax - yMin) : 0.1;
  yMin -= 0.1 * span;
  yMax += 0.1 * span;
  yMin = std::min(yMin, 0.0);
  yMax = std::max(yMax, 0.0);

  std::string modelLabel = model.empty() ? "" : (" (" + model + ")");
  std::string titleDefault = "Average Marginal Effects" + modelLabel;

  ojson out;
  out["type"] = "graph";
  out["kind"] = "marginsplot";
  out["title"] = optTitleStr(opts, "title").value_or(titleDefault);
  out["y_label"] = "Average marginal effect (dydx)";
  out["x_label"] = "";
  out["note"] = optTitleStr(opts, "note").value_or(std::string());
  out["n"] = nObs;
  out["model"] = model;
  out["points"] = points;
  out["y_min"] = yMin;
  out["y_max"] = yMax;
  out["name"] = nullptr;  // filled by the dispatcher's name() capture (BS-011, mirrors BS-010)
  return out;
}

// ===========================================================================
// M4b: graph save / combine / export — C++ port of graph_combine.py +
// graph_export.py. SVG geometry constants and tick/scale helpers below
// MUST match graph_export.py's `_W/_H/_MT/_MR/_MB/_ML`/`_nice_ticks`/
// `_fmt_tick`/`_padded_range`/`_sx`/`_sy` byte-for-byte (same source of
// truth graph.js's frontend renderer already uses) — this is the
// "SVG structural" parity the M4 milestone gate gauges debug_all against.

namespace {

bool fileExistsG(const std::string& path) { return cecon::native_exists(path); }

// BS-007 (D3): the on-disk read-back half of runGraphSave's (below)
// {"_magic":"PIETGPH1","block":<block>} envelope -- a graph SPEC-JSON
// serializer+loader pair, NEVER a raster/frozen-pixel format (D3 hard-stop:
// growing this beyond serialize+load is out of scope). Returns nullopt for
// any of: file missing/unreadable, not valid JSON, wrong/missing magic, or
// no "block" key -- callers turn that into an honest reject, never a crash.
std::optional<nlohmann::ordered_json> loadGphFile(const std::string& path) {
  std::ifstream f(cecon::to_native_path(path), std::ios::binary);
  if (!f) return std::nullopt;
  // Whole-file slurp: the same `ostringstream << rdbuf()` idiom used
  // throughout this codebase (doexec.cpp/handlers.cpp/main.cpp/routes.cpp/
  // settings.cpp) rather than istreambuf_iterator, for consistency.
  std::ostringstream ss;
  ss << f.rdbuf();
  std::string text = ss.str();
  nlohmann::ordered_json doc;
  try {
    doc = nlohmann::ordered_json::parse(text);
  } catch (...) {
    return std::nullopt;
  }
  if (!doc.is_object() || doc.value("_magic", std::string()) != "PIETGPH1" || !doc.contains("block")) {
    return std::nullopt;
  }
  return doc["block"];
}

std::string svgEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      default: out.push_back(c);
    }
  }
  return out;
}

// SVG geometry constants (graph_export.py's _W/_H/_MT/_MR/_MB/_ML).
constexpr double kSvgW = 520.0, kSvgH = 360.0;
constexpr double kSvgMT = 34.0, kSvgMR = 18.0, kSvgMB = 48.0, kSvgML = 62.0;
constexpr double kSvgPlotW = kSvgW - kSvgML - kSvgMR;   // 440
constexpr double kSvgPlotH = kSvgH - kSvgMT - kSvgMB;   // 278

std::vector<double> niceTicksG(double lo, double hi, int target = 5) {
  if (!std::isfinite(lo) || !std::isfinite(hi)) return {0.0};
  if (lo == hi) return {lo};
  double span = hi - lo;
  double raw = span / target;
  double mag = std::pow(10.0, std::floor(std::log10(raw)));
  double norm = raw / mag;
  double step;
  if (norm < 1.5) step = 1;
  else if (norm < 3) step = 2;
  else if (norm < 7) step = 5;
  else step = 10;
  step *= mag;
  double start = std::ceil(lo / step) * step;
  std::vector<double> ticks;
  for (double t = start; t <= hi + step * 1e-9; t += step) {
    ticks.push_back(std::abs(t) < step * 1e-9 ? 0.0 : t);
  }
  if (ticks.empty()) ticks = {lo, hi};
  return ticks;
}

std::string trimTrailingZeros(std::string s) {
  if (s.find('.') == std::string::npos) return s;
  while (!s.empty() && s.back() == '0') s.pop_back();
  if (!s.empty() && s.back() == '.') s.pop_back();
  return s;
}

std::string fmtTickG(double v) {
  if (v == 0) return "0";
  double a = std::abs(v);
  char buf[64];
  if (a >= 1e6) {
    double f = v / 1e6;
    std::snprintf(buf, sizeof(buf), a >= 1e7 ? "%.0fM" : "%.1fM", f);
    return buf;
  }
  if (a >= 1e4) {
    std::snprintf(buf, sizeof(buf), "%.0fk", v / 1e3);
    return buf;
  }
  if (v == std::floor(v)) {
    std::snprintf(buf, sizeof(buf), "%.0f", v);
    return buf;
  }
  if (a >= 100) {
    std::snprintf(buf, sizeof(buf), "%.0f", v);
    return buf;
  }
  if (a >= 1) {
    std::snprintf(buf, sizeof(buf), "%.2f", v);
    return trimTrailingZeros(buf);
  }
  std::snprintf(buf, sizeof(buf), "%.2g", v);
  return trimTrailingZeros(buf);
}

std::pair<double, double> paddedRangeG(double lo, double hi, double padLo = 0.04, double padHi = 0.04) {
  if (!std::isfinite(lo) || !std::isfinite(hi)) return {0.0, 1.0};
  if (lo == hi) return {lo - 0.5, hi + 0.5};
  double span = hi - lo;
  return {lo - span * padLo, hi + span * padHi};
}

double svgSx(double x, double x0, double x1, double plotW = kSvgPlotW) {
  return kSvgML + ((x - x0) / (x1 - x0)) * plotW;
}

// Conservative advance-width estimate (SVG has no font metrics here): em
// fractions per character class, one em per non-ASCII code point.
double svgTextWidth(const std::string& s, double fontSize) {
  double em = 0.0;
  for (unsigned char c : s) {
    if (c >= 0x80) {
      if (c >= 0xC0) em += 1.0;
    } else if (c == 'm' || c == 'w' || c == 'M' || c == 'W') {
      em += 0.9;
    } else if (c >= 'A' && c <= 'Z') {
      em += 0.72;
    } else if (c == ' ' || c == 'i' || c == 'j' || c == 'l' || c == 't' || c == 'f' || c == 'r' || c == 'I' ||
               c == '.' || c == ',' || c == ':' || c == ';' || c == '|' || c == '\'' || c == '!') {
      em += 0.4;
    } else {
      em += 0.6;
    }
  }
  return em * fontSize;
}
double svgSy(double y, double y0, double y1) {
  return kSvgMT + kSvgPlotH - ((y - y0) / (y1 - y0)) * kSvgPlotH;
}

char fbuf[64];
std::string fmtNum(double v, const char* spec = "%.1f") {
  std::snprintf(fbuf, sizeof(fbuf), spec, v);
  return fbuf;
}

void svgFrameAndAxes(std::vector<std::string>& lines, std::pair<double, double> xr,
                      std::pair<double, double> yr, const std::string& xLabel,
                      const std::string& yLabel, bool suppressXTickLabels = false,
                      bool suppressYTickLabels = false, double plotW = kSvgPlotW) {
  auto [x0, x1] = xr;
  auto [y0, y1] = yr;

  lines.push_back("  <rect x=\"" + fmtNum(kSvgML, "%.0f") + "\" y=\"" + fmtNum(kSvgMT, "%.0f") +
                   "\" width=\"" + fmtNum(plotW, "%.0f") + "\" height=\"" + fmtNum(kSvgPlotH, "%.0f") +
                   "\" fill=\"none\" stroke=\"#aaa\" stroke-width=\"1\"/>");

  for (double t : niceTicksG(x0, x1)) {
    double sx = svgSx(t, x0, x1, plotW);
    if (sx < kSvgML - 0.5 || sx > kSvgML + plotW + 0.5) continue;
    double ty = kSvgMT + kSvgPlotH;
    lines.push_back("  <line x1=\"" + fmtNum(sx) + "\" y1=\"" + fmtNum(ty, "%.0f") + "\" x2=\"" +
                     fmtNum(sx) + "\" y2=\"" + fmtNum(ty + 5, "%.0f") +
                     "\" stroke=\"#555\" stroke-width=\"1\"/>");
    // marginsplot (BS-011) suppresses the numeric x tick text -- it places
    // its own categorical term-name labels instead (mirrors graph.js's
    // drawFrameAndAxes(..., {suppressXTickLabels:true}) for the identical
    // reason). The tick MARK still draws either way.
    if (!suppressXTickLabels) {
      lines.push_back("  <text x=\"" + fmtNum(sx) + "\" y=\"" + fmtNum(ty + 18, "%.0f") +
                       "\" text-anchor=\"middle\" font-size=\"10\" fill=\"#444\">" +
                       svgEscape(fmtTickG(t)) + "</text>");
    }
  }

  for (double t : niceTicksG(y0, y1)) {
    double sy = svgSy(t, y0, y1);
    if (sy < kSvgMT - 0.5 || sy > kSvgMT + kSvgPlotH + 0.5) continue;
    lines.push_back("  <line x1=\"" + fmtNum(kSvgML - 5, "%.0f") + "\" y1=\"" + fmtNum(sy) +
                     "\" x2=\"" + fmtNum(kSvgML, "%.0f") + "\" y2=\"" + fmtNum(sy) +
                     "\" stroke=\"#555\" stroke-width=\"1\"/>");
    // coefplot (BS-011) suppresses the numeric y tick text -- it places its
    // own per-coefficient name labels instead (mirrors graph.js's
    // drawFrameAndAxes(..., {suppressYTickLabels:true})).
    if (!suppressYTickLabels) {
      lines.push_back("  <text x=\"" + fmtNum(kSvgML - 9, "%.0f") + "\" y=\"" + fmtNum(sy + 3.5) +
                       "\" text-anchor=\"end\" font-size=\"10\" fill=\"#444\">" + svgEscape(fmtTickG(t)) +
                       "</text>");
    }
  }

  double midX = kSvgML + plotW / 2;
  lines.push_back("  <text x=\"" + fmtNum(midX) + "\" y=\"" + fmtNum(kSvgH - 10, "%.0f") +
                   "\" text-anchor=\"middle\" font-size=\"11\" fill=\"#333\">" + svgEscape(xLabel) +
                   "</text>");
  double midY = kSvgMT + kSvgPlotH / 2;
  lines.push_back("  <text x=\"14\" y=\"" + fmtNum(midY) +
                   "\" text-anchor=\"middle\" font-size=\"11\" fill=\"#333\" transform=\"rotate(-90 14 " +
                   fmtNum(midY) + ")\">" + svgEscape(yLabel) + "</text>");
}

void svgTitle(std::vector<std::string>& lines, const std::string& title, double width = kSvgW) {
  lines.push_back("  <text x=\"" + fmtNum(width / 2) + "\" y=\"20\" text-anchor=\"middle\" " +
                   "font-size=\"13\" font-weight=\"bold\" fill=\"#111\">" + svgEscape(title) + "</text>");
}

double jnum(const ojson& v, double dflt = 0.0) {
  if (v.is_null()) return dflt;
  try {
    return v.get<double>();
  } catch (...) {
    return dflt;
  }
}

// jnum's "read a possibly-absent/possibly-null object KEY" counterpart --
// used by buildSvgMarginsplot/buildSvgCoefplot below (BS-011) where a
// coefplot row's ci_lo/ci_hi may be JSON null (no CI for that coefficient;
// see eststo_esttab.cpp's runCoefplot, which stores `ojson(nullptr)` for a
// NaN bound) -- `obj[key]` alone would still be a legal (null) ojson, but
// jnum() only guards a value ALREADY in hand, not a missing key.
double jnumOr(const ojson& obj, const char* key, double dflt) {
  if (!obj.contains(key) || obj[key].is_null()) return dflt;
  return jnum(obj[key], dflt);
}

// BS-055 (BETA E): shared multi-series renderer -- xtline (`series:
// [{id,label,points}]`) and tsline (`series: [{var,points}]`) both carry
// their data in a top-level `series` array, NOT the flat `points` key every
// OTHER kind uses. buildSvgDispatch had no "xtline"/"tsline" branch at all,
// so both silently fell through to buildSvgScatter's DEFAULT arm, which
// reads a "points" key neither block shape has -- frame/axes/title render
// (svgFrameAndAxes doesn't care about kind), zero data (an empty `pts`
// array), exactly the bug's own description. Also reused by BS-050's
// multi-y `scatter y1 y2 x` -- see buildSvgScatter's delegation below.
// `asMarkers=true` draws small circles (scatter-style); false draws
// connected polylines (xtline/tsline-style) -- matching each kind's own
// natural rendering.
std::string buildSvgSeriesPlot(const ojson& block, bool asMarkers) {
  auto xr = paddedRangeG(jnum(block.value("x_min", ojson(0.0))), jnum(block.value("x_max", ojson(1.0))));
  auto yr = paddedRangeG(jnum(block.value("y_min", ojson(0.0))), jnum(block.value("y_max", ojson(1.0))));
  std::vector<std::string> lines = {"<svg viewBox=\"0 0 520 360\" xmlns=\"http://www.w3.org/2000/svg\">"};
  svgTitle(lines, block.value("title", std::string()));
  svgFrameAndAxes(lines, xr, yr, block.value("x_label", std::string()), block.value("y_label", std::string()));
  lines.push_back("  <clipPath id=\"sc\"><rect x=\"" + fmtNum(kSvgML, "%.0f") + "\" y=\"" +
                   fmtNum(kSvgMT, "%.0f") + "\" width=\"" + fmtNum(kSvgPlotW, "%.0f") + "\" height=\"" +
                   fmtNum(kSvgPlotH, "%.0f") + "\"/></clipPath>");
  lines.push_back("  <g clip-path=\"url(#sc)\">");
  ojson series = block.value("series", ojson::array());
  for (size_t si = 0; si < series.size(); ++si) {
    ojson pts = series[si].value("points", ojson::array());
    if (pts.empty()) continue;
    std::string color;
    if (series[si].contains("style") && series[si]["style"].contains("color") &&
        series[si]["style"]["color"].is_string()) {
      color = series[si]["style"]["color"].get<std::string>();
    } else {
      color = twoway::kSeriesColors[si % twoway::kSeriesColors.size()];
    }
    if (!asMarkers) {
      std::string d;
      for (size_t i = 0; i < pts.size(); ++i) {
        double sx = svgSx(jnum(pts[i][0]), xr.first, xr.second);
        double sy = svgSy(jnum(pts[i][1]), yr.first, yr.second);
        d += (i == 0 ? "M" : "L") + fmtNum(sx, "%.2f") + " " + fmtNum(sy, "%.2f") + " ";
      }
      if (!d.empty()) d.pop_back();
      lines.push_back("    <path d=\"" + d + "\" fill=\"none\" stroke=\"" + color +
                       "\" stroke-width=\"1.5\" stroke-linejoin=\"round\" stroke-linecap=\"round\"/>");
    } else {
      double r = pts.size() > 2000 ? 1.3 : (pts.size() > 500 ? 1.8 : 2.4);
      for (const auto& xy : pts) {
        double cx = svgSx(jnum(xy[0]), xr.first, xr.second);
        double cy = svgSy(jnum(xy[1]), yr.first, yr.second);
        lines.push_back("    <circle cx=\"" + fmtNum(cx) + "\" cy=\"" + fmtNum(cy) + "\" r=\"" +
                         fmtNum(r) + "\" fill=\"" + color + "\" opacity=\"0.8\"/>");
      }
    }
  }
  lines.push_back("  </g>");
  lines.push_back("</svg>");
  std::string out;
  for (auto& l : lines) { out += l; out += "\n"; }
  if (!out.empty()) out.pop_back();
  return out;
}

std::string buildSvgScatter(const ojson& block) {
  // BS-050: multi-y `scatter y1 y2 x` populates a "series" array (2+
  // entries) alongside the legacy flat "points" -- delegate to the shared
  // multi-series renderer (marker/dot style) so every y-series actually
  // gets its own distinguishable color instead of all points collapsing
  // into one flat #1f77b4 blob. The single-y case (no "series" key, the
  // overwhelmingly common path) is UNCHANGED below.
  if (block.contains("series") && block["series"].is_array() && !block["series"].empty()) {
    return buildSvgSeriesPlot(block, /*asMarkers=*/true);
  }
  auto xr = paddedRangeG(jnum(block.value("x_min", ojson(0.0))), jnum(block.value("x_max", ojson(1.0))));
  auto yr = paddedRangeG(jnum(block.value("y_min", ojson(0.0))), jnum(block.value("y_max", ojson(1.0))));
  std::vector<std::string> lines = {"<svg viewBox=\"0 0 520 360\" xmlns=\"http://www.w3.org/2000/svg\">"};
  svgTitle(lines, block.value("title", std::string()));
  svgFrameAndAxes(lines, xr, yr, block.value("x_label", std::string()), block.value("y_label", std::string()));
  ojson pts = block.value("points", ojson::array());
  double r = pts.size() > 2000 ? 1.3 : (pts.size() > 500 ? 1.8 : 2.4);
  lines.push_back("  <clipPath id=\"gc\"><rect x=\"" + fmtNum(kSvgML, "%.0f") + "\" y=\"" +
                   fmtNum(kSvgMT, "%.0f") + "\" width=\"" + fmtNum(kSvgPlotW, "%.0f") + "\" height=\"" +
                   fmtNum(kSvgPlotH, "%.0f") + "\"/></clipPath>");
  lines.push_back("  <g clip-path=\"url(#gc)\">");
  for (const auto& xy : pts) {
    double cx = svgSx(jnum(xy[0]), xr.first, xr.second);
    double cy = svgSy(jnum(xy[1]), yr.first, yr.second);
    lines.push_back("    <circle cx=\"" + fmtNum(cx) + "\" cy=\"" + fmtNum(cy) + "\" r=\"" +
                     fmtNum(r) + "\" fill=\"#1f77b4\" opacity=\"0.8\"/>");
  }
  lines.push_back("  </g>");
  lines.push_back("</svg>");
  std::string out;
  for (auto& l : lines) { out += l; out += "\n"; }
  if (!out.empty()) out.pop_back();
  return out;
}

std::string buildSvgHistogram(const ojson& block) {
  auto xr = paddedRangeG(jnum(block.value("x_min", ojson(0.0))), jnum(block.value("x_max", ojson(1.0))), 0.02, 0.02);
  double yMaxRaw = jnum(block.value("y_max", ojson(0.0)));
  double yMax = yMaxRaw > 0 ? yMaxRaw : 1.0;
  std::pair<double, double> yr = {0.0, yMax * 1.08};
  std::vector<std::string> lines = {"<svg viewBox=\"0 0 520 360\" xmlns=\"http://www.w3.org/2000/svg\">"};
  svgTitle(lines, block.value("title", std::string()));
  svgFrameAndAxes(lines, xr, yr, block.value("x_label", std::string()), block.value("y_label", std::string()));
  double baseY = svgSy(0, yr.first, yr.second);
  for (const auto& b : block.value("bins", ojson::array())) {
    double bx0 = svgSx(jnum(b["x0"]), xr.first, xr.second);
    double bx1 = svgSx(jnum(b["x1"]), xr.first, xr.second);
    double ytop = svgSy(jnum(b["y"]), yr.first, yr.second);
    double w = std::max(0.5, bx1 - bx0 - 0.6);
    double h = std::max(0.0, baseY - ytop);
    lines.push_back("  <rect x=\"" + fmtNum(bx0 + 0.3) + "\" y=\"" + fmtNum(ytop) + "\" width=\"" +
                     fmtNum(w) + "\" height=\"" + fmtNum(h) +
                     "\" fill=\"#1f77b4\" stroke=\"white\" stroke-width=\"0.5\" opacity=\"0.85\"/>");
  }
  lines.push_back("</svg>");
  std::string out;
  for (auto& l : lines) { out += l; out += "\n"; }
  if (!out.empty()) out.pop_back();
  return out;
}

std::string buildSvgLine(const ojson& block) {
  auto xr = paddedRangeG(jnum(block.value("x_min", ojson(0.0))), jnum(block.value("x_max", ojson(1.0))));
  auto yr = paddedRangeG(jnum(block.value("y_min", ojson(0.0))), jnum(block.value("y_max", ojson(1.0))));
  std::vector<std::string> lines = {"<svg viewBox=\"0 0 520 360\" xmlns=\"http://www.w3.org/2000/svg\">"};
  svgTitle(lines, block.value("title", std::string()));
  svgFrameAndAxes(lines, xr, yr, block.value("x_label", std::string()), block.value("y_label", std::string()));
  ojson pts = block.value("points", ojson::array());
  if (!pts.empty()) {
    std::string d;
    for (size_t i = 0; i < pts.size(); ++i) {
      double cx = svgSx(jnum(pts[i][0]), xr.first, xr.second);
      double cy = svgSy(jnum(pts[i][1]), yr.first, yr.second);
      d += (i == 0 ? "M" : "L") + fmtNum(cx, "%.2f") + " " + fmtNum(cy, "%.2f") + " ";
    }
    if (!d.empty()) d.pop_back();
    lines.push_back("  <path d=\"" + d + "\" fill=\"none\" stroke=\"#1f77b4\" stroke-width=\"1.5\"/>");
  }
  lines.push_back("</svg>");
  std::string out;
  for (auto& l : lines) { out += l; out += "\n"; }
  if (!out.empty()) out.pop_back();
  return out;
}

std::string buildSvgTwoway(const ojson& block) {
  auto xr = paddedRangeG(jnum(block.value("x_min", ojson(0.0))), jnum(block.value("x_max", ojson(1.0))));
  auto yr = paddedRangeG(jnum(block.value("y_min", ojson(0.0))), jnum(block.value("y_max", ojson(1.0))));
  auto [x0, x1] = xr;
  auto [y0, y1] = yr;
  // A right-hand legend needs room for its widest rendered label: shrink the
  // plot rect so label text ends inside the viewBox; only if the plot would
  // fall below kMinPlotW widen the viewBox instead.
  ojson layers = block.value("layers", ojson::array());
  ojson legend = block.value("legend", ojson::object());
  constexpr double kLegendGap = 4, kLegendText = 13, kLegendPad = 4, kLegendFont = 9, kMinPlotW = 160;
  double legendLabelW = 0.0;
  bool hasLegend = false;
  // A key's text lines are joined by "\n"; index -1 is text with no key (BUG-1311).
  auto labelLines = [](const ojson& v) {
    std::vector<std::string> out;
    std::stringstream ss(v.is_string() ? v.get<std::string>() : v.dump());
    std::string line;
    while (std::getline(ss, line, '\n')) out.push_back(line);
    if (out.empty()) out.emplace_back();
    return out;
  };
  if (!legend.value("off", false)) {
    for (const auto& pair : legend.value("order", ojson::array())) {
      int layerIdx = pair[0].get<int>();
      if (layerIdx < -1 || layerIdx >= static_cast<int>(layers.size())) continue;
      for (const auto& line : labelLines(pair[1]))
        legendLabelW = std::max(legendLabelW, svgTextWidth(line, kLegendFont));
      hasLegend = true;
    }
  }
  double plotW = kSvgPlotW, viewW = kSvgW;
  if (hasLegend) {
    double legendW = kLegendGap + kLegendText + legendLabelW + kLegendPad;
    plotW = std::min(kSvgPlotW, kSvgW - kSvgML - legendW);
    if (plotW < kMinPlotW) {
      plotW = kMinPlotW;
      viewW = std::ceil(kSvgML + plotW + legendW);
    }
  }
  std::vector<std::string> lines = {"<svg viewBox=\"0 0 " + fmtNum(viewW, "%.0f") +
                                     " 360\" xmlns=\"http://www.w3.org/2000/svg\">"};
  svgTitle(lines, block.value("title", std::string()), viewW);
  svgFrameAndAxes(lines, xr, yr, block.value("x_label", std::string()), block.value("y_label", std::string()),
                  false, false, plotW);

  lines.push_back("  <clipPath id=\"tw-clip\"><rect x=\"" + fmtNum(kSvgML, "%.0f") + "\" y=\"" +
                   fmtNum(kSvgMT, "%.0f") + "\" width=\"" + fmtNum(plotW, "%.0f") + "\" height=\"" +
                   fmtNum(kSvgPlotH, "%.0f") + "\"/></clipPath>");

  ojson axes = block.value("axes", ojson::object());
  for (const auto& yv : axes.value("yline", ojson::array())) {
    double syRef = svgSy(jnum(yv), y0, y1);
    lines.push_back("  <line x1=\"" + fmtNum(kSvgML, "%.0f") + "\" y1=\"" + fmtNum(syRef) + "\" x2=\"" +
                     fmtNum(kSvgML + plotW, "%.0f") + "\" y2=\"" + fmtNum(syRef) +
                     "\" stroke=\"#888\" stroke-width=\"1\" stroke-dasharray=\"4 3\"/>");
  }
  for (const auto& xv : axes.value("xline", ojson::array())) {
    double sxRef = svgSx(jnum(xv), x0, x1, plotW);
    lines.push_back("  <line x1=\"" + fmtNum(sxRef) + "\" y1=\"" + fmtNum(kSvgMT, "%.0f") + "\" x2=\"" +
                     fmtNum(sxRef) + "\" y2=\"" + fmtNum(kSvgMT + kSvgPlotH, "%.0f") +
                     "\" stroke=\"#888\" stroke-width=\"1\" stroke-dasharray=\"4 3\"/>");
  }

  lines.push_back("  <g clip-path=\"url(#tw-clip)\">");
  for (const auto& layer : layers) {
    std::string lkind = layer.value("kind", std::string("line"));
    ojson style = layer.value("style", ojson::object());

    // serset-twoway FIT: lfitci/qfitci draw their band like rarea, then their
    // fitted line like lfit/qfit (the two branches below are no longer
    // exclusive).
    const bool fitCi = lkind == "lfitci" || lkind == "qfitci";
    if (lkind == "rarea" || fitCi) {
      ojson band = layer.value("band", ojson::array());
      if (band.size() >= 2) {
        std::string color = style.value("fcolor", std::string("#aec7e8"));
        double opacity = jnum(style.value("opacity", ojson(0.3)), 0.3);
        std::string lcolor = style.value("lcolor", std::string("none"));
        double lwidth = jnum(style.value("lwidth", ojson(0.0)), 0.0);
        std::string fwd, rev;
        for (size_t i = 0; i < band.size(); ++i) {
          double sx = svgSx(jnum(band[i][0]), x0, x1, plotW);
          double sy = svgSy(jnum(band[i][2]), y0, y1);
          fwd += (i == 0 ? "M" : "L") + fmtNum(sx, "%.2f") + " " + fmtNum(sy, "%.2f") + " ";
        }
        for (size_t i = band.size(); i-- > 0;) {
          double sx = svgSx(jnum(band[i][0]), x0, x1, plotW);
          double sy = svgSy(jnum(band[i][1]), y0, y1);
          rev += "L" + fmtNum(sx, "%.2f") + " " + fmtNum(sy, "%.2f") + " ";
        }
        std::string d = fwd + rev + "Z";
        std::string strokeAttr = (lwidth > 0 && lcolor != "none")
                                      ? ("stroke=\"" + lcolor + "\" stroke-width=\"" + fmtNum(lwidth) + "\"")
                                      : "stroke=\"none\"";
        lines.push_back("    <path d=\"" + d + "\" fill=\"" + color + "\" fill-opacity=\"" +
                         fmtNum(opacity, "%.3g") + "\" " + strokeAttr + "/>");
      }
    }
    if (lkind == "histogram") {
      // serset-twoway HKL: a histogram layer's bars.
      std::string color = style.value("fcolor", style.value("color", std::string("#1f77b4")));
      for (const auto& b : layer.value("bars", ojson::array())) {
        const double bx0 = svgSx(b.value("x0", 0.0), x0, x1, plotW);
        const double bx1 = svgSx(b.value("x1", 0.0), x0, x1, plotW);
        const double by = svgSy(b.value("y", 0.0), y0, y1);
        const double by0 = svgSy(0.0, y0, y1);
        lines.push_back("    <rect x=\"" + fmtNum(std::min(bx0, bx1), "%.2f") + "\" y=\"" +
                        fmtNum(std::min(by, by0), "%.2f") + "\" width=\"" +
                        fmtNum(std::fabs(bx1 - bx0), "%.2f") + "\" height=\"" +
                        fmtNum(std::fabs(by0 - by), "%.2f") + "\" fill=\"" + color +
                        "\" fill-opacity=\"0.6\" stroke=\"#ffffff\" stroke-width=\"0.5\"/>");
      }
    }
    if (layer.contains("pc")) {
      // serset-twoway HIST-5: pci/pcarrowi segments.
      std::string color = style.value("color", style.value("lcolor", std::string("#1f77b4")));
      for (const auto& sg : layer["pc"]) {
        lines.push_back("    <line x1=\"" + fmtNum(svgSx(jnum(sg[0]), x0, x1, plotW), "%.2f") + "\" y1=\"" +
                        fmtNum(svgSy(jnum(sg[1]), y0, y1), "%.2f") + "\" x2=\"" +
                        fmtNum(svgSx(jnum(sg[2]), x0, x1, plotW), "%.2f") + "\" y2=\"" +
                        fmtNum(svgSy(jnum(sg[3]), y0, y1), "%.2f") + "\" stroke=\"" + color +
                        "\" stroke-width=\"1.5\"/>");
      }
    }
    if (layer.contains("segments")) {
      // serset-twoway RNG: one vertical segment per observation (rcap & co).
      std::string color = style.value("color", style.value("lcolor", std::string("#1f77b4")));
      for (const auto& sg : layer["segments"]) {
        const double sx = svgSx(jnum(sg[0]), x0, x1, plotW);
        lines.push_back("    <line x1=\"" + fmtNum(sx, "%.2f") + "\" y1=\"" +
                        fmtNum(svgSy(jnum(sg[1]), y0, y1), "%.2f") + "\" x2=\"" + fmtNum(sx, "%.2f") +
                        "\" y2=\"" + fmtNum(svgSy(jnum(sg[2]), y0, y1), "%.2f") + "\" stroke=\"" +
                        color + "\" stroke-width=\"1\"/>");
      }
    }
    if (layer.contains("base") && layer.contains("points")) {
      // serset-twoway RNG: area/spike/dropline/bar to their base.
      std::string color = style.value("fcolor", style.value("color", std::string("#1f77b4")));
      const double by = svgSy(layer.value("base", 0.0), y0, y1);
      ojson pts = layer["points"];
      if (lkind == "area" && pts.size() >= 2) {
        std::string d = "M" + fmtNum(svgSx(jnum(pts[0][0]), x0, x1, plotW), "%.2f") + " " + fmtNum(by, "%.2f") + " ";
        for (const auto& p : pts)
          d += "L" + fmtNum(svgSx(jnum(p[0]), x0, x1, plotW), "%.2f") + " " +
               fmtNum(svgSy(jnum(p[1]), y0, y1), "%.2f") + " ";
        d += "L" + fmtNum(svgSx(jnum(pts[pts.size() - 1][0]), x0, x1, plotW), "%.2f") + " " +
             fmtNum(by, "%.2f") + " Z";
        lines.push_back("    <path d=\"" + d + "\" fill=\"" + color + "\" fill-opacity=\"0.5\" stroke=\"none\"/>");
      } else {
        for (const auto& p : pts) {
          const double sx = svgSx(jnum(p[0]), x0, x1, plotW);
          lines.push_back("    <line x1=\"" + fmtNum(sx, "%.2f") + "\" y1=\"" + fmtNum(by, "%.2f") +
                          "\" x2=\"" + fmtNum(sx, "%.2f") + "\" y2=\"" +
                          fmtNum(svgSy(jnum(p[1]), y0, y1), "%.2f") + "\" stroke=\"" + color +
                          "\" stroke-width=\"1.5\"/>");
        }
      }
    }
    const bool kdLine = lkind == "kdensity";
    if (lkind == "line" || lkind == "connected" || lkind == "scatter" || lkind == "lfit" ||
        lkind == "qfit" || fitCi || kdLine) {
      // BUG-099: lfit/qfit's `points` shape is identical to line's (a
      // 2-point line / a 100-point sampled curve, both plain [x,y] pairs),
      // so they render through this SAME polyline branch -- no marker dots
      // (the `lkind == "scatter" || lkind == "connected"` guard below
      // deliberately excludes them, matching Stata: a fit line never plots
      // marker symbols).
      ojson pts = layer.value("points", ojson::array());
      if (pts.empty()) continue;
      std::string color = style.value("color", std::string("#1f77b4"));
      double lwidth = jnum(style.value("lwidth", ojson(1.5)), 1.5);
      std::string lpattern = style.contains("lpattern") && !style["lpattern"].is_null()
                                  ? style["lpattern"].get<std::string>()
                                  : std::string();
      std::string msymbol = style.contains("msymbol") && !style["msymbol"].is_null()
                                 ? style["msymbol"].get<std::string>()
                                 : std::string();
      double msize = jnum(style.value("msize", ojson(2.4)), 2.4);
      bool hollow = style.value("hollow", false);

      if ((lkind == "line" || lkind == "connected" || lkind == "lfit" || lkind == "qfit" || fitCi ||
           kdLine) &&
          lwidth > 0) {
        std::string d;
        for (size_t i = 0; i < pts.size(); ++i) {
          double sx = svgSx(jnum(pts[i][0]), x0, x1, plotW);
          double sy = svgSy(jnum(pts[i][1]), y0, y1);
          d += (i == 0 ? "M" : "L") + fmtNum(sx, "%.2f") + " " + fmtNum(sy, "%.2f") + " ";
        }
        if (!d.empty()) d.pop_back();
        std::string dashAttr = lpattern.empty() ? "" : (" stroke-dasharray=\"" + lpattern + "\"");
        lines.push_back("    <path d=\"" + d + "\" fill=\"none\" stroke=\"" + color +
                         "\" stroke-width=\"" + fmtNum(lwidth) +
                         "\" stroke-linejoin=\"round\" stroke-linecap=\"round\"" + dashAttr + "/>");
      }
      if ((lkind == "scatter" || lkind == "connected") && !msymbol.empty() && msymbol != "none") {
        std::string fill = hollow ? "none" : color;
        double sw = hollow ? lwidth : 0.0;
        for (const auto& pt : pts) {
          double cx = svgSx(jnum(pt[0]), x0, x1, plotW);
          double cy = svgSy(jnum(pt[1]), y0, y1);
          lines.push_back("    <circle cx=\"" + fmtNum(cx, "%.2f") + "\" cy=\"" + fmtNum(cy, "%.2f") +
                           "\" r=\"" + fmtNum(msize) + "\" fill=\"" + fill + "\" stroke=\"" + color +
                           "\" stroke-width=\"" + fmtNum(sw) + "\"/>");
        }
      }
    }
  }
  lines.push_back("  </g>");

  if (!legend.value("off", false)) {
    ojson order = legend.value("order", ojson::array());
    if (!order.empty()) {
      double lx = kSvgML + plotW + kLegendGap;
      int li = 0;
      for (const auto& pair : order) {
        int layerIdx = pair[0].get<int>();
        // FU-113: an index with no matching plot is ignored WITHOUT consuming a
        // row (the counter used to advance before this check, leaving a gap).
        if (layerIdx < -1 || layerIdx >= static_cast<int>(layers.size())) continue;
        double ly = kSvgMT + 14 + li * 14;
        if (layerIdx >= 0) {
          ojson lst = layers[static_cast<size_t>(layerIdx)].value("style", ojson::object());
          std::string color = lst.value("color", lst.value("fcolor", std::string("#1f77b4")));
          lines.push_back("  <line x1=\"" + fmtNum(lx, "%.0f") + "\" y1=\"" + fmtNum(ly - 3) + "\" x2=\"" +
                           fmtNum(lx + 10, "%.0f") + "\" y2=\"" + fmtNum(ly - 3) + "\" stroke=\"" + color +
                           "\" stroke-width=\"1.5\"/>");
        }
        // one row per text line, the key on the first (BUG-1311)
        for (const auto& line : labelLines(pair[1])) {
          ly = kSvgMT + 14 + li * 14;
          ++li;
          lines.push_back("  <text x=\"" + fmtNum(lx + kLegendText, "%.0f") + "\" y=\"" + fmtNum(ly) +
                           "\" text-anchor=\"start\" font-size=\"9\" fill=\"#333\">" + svgEscape(line) +
                           "</text>");
        }
      }
    }
  }
  lines.push_back("</svg>");
  std::string out;
  for (auto& l : lines) { out += l; out += "\n"; }
  if (!out.empty()) out.pop_back();
  return out;
}

// BS-011 (BETA B-graph-B): coefplot/marginsplot SVG export builders. Neither
// kind had a native (headless `--do`) SVG renderer before -- unreachable
// until now because session.lastGraph was never populated for these two
// kinds (see runCoefplot's/runMarginsplot's dispatcher call sites), so
// `graph export`/`graph save`+`graph combine` after a coefplot/marginsplot
// always hit the "no graph in memory" error and buildSvgDispatch's kind
// table never needed an entry for them. Wiring lastGraph WITHOUT also
// adding these two branches would have swapped that honest error for a
// crash/garbage render (buildSvgScatter indexing `points[i][0]` on a block
// whose points/models are shaped completely differently) -- so the two go
// together. Geometry mirrors piet/frontend/js/graph.js's renderMarginsplot/
// renderCoefplot exactly (categorical axis + whisker-and-dot layout); the
// color palette matches THIS file's existing scatter/line/twoway builders
// (#1f77b4 points, #888 dashed reference line) rather than graph.js's CSS
// theme tokens -- the native SVG export has always been a fixed, theme-
// independent palette (see buildSvgScatter et al. above), never the live
// console's --graph-accent-* variables.

std::string buildSvgMarginsplot(const ojson& block) {
  ojson pts = block.value("points", ojson::array());
  int n = static_cast<int>(pts.size());

  std::vector<std::string> lines = {"<svg viewBox=\"0 0 520 360\" xmlns=\"http://www.w3.org/2000/svg\">"};
  svgTitle(lines, block.value("title", std::string()));

  // Categorical x axis: one evenly-spaced slot per AME term (1..n); numeric
  // x tick text suppressed -- the term-name labels below take its place
  // (graph.js's identical suppressXTickLabels rationale).
  auto xr = paddedRangeG(0.5, n + 0.5, 0.05, 0.05);
  auto yr = paddedRangeG(jnum(block.value("y_min", ojson(0.0))), jnum(block.value("y_max", ojson(1.0))), 0.05, 0.05);
  svgFrameAndAxes(lines, xr, yr, block.value("x_label", std::string()), block.value("y_label", std::string()),
                  /*suppressXTickLabels=*/true, /*suppressYTickLabels=*/false);
  auto [x0, x1] = xr;
  auto [y0, y1] = yr;

  double zy = svgSy(0.0, y0, y1);
  if (zy >= kSvgMT - 0.5 && zy <= kSvgMT + kSvgPlotH + 0.5) {
    lines.push_back("  <line x1=\"" + fmtNum(kSvgML, "%.0f") + "\" y1=\"" + fmtNum(zy) + "\" x2=\"" +
                     fmtNum(kSvgML + kSvgPlotW, "%.0f") + "\" y2=\"" + fmtNum(zy) +
                     "\" stroke=\"#888\" stroke-width=\"1\" stroke-dasharray=\"3 3\"/>");
  }

  double labelY = kSvgMT + kSvgPlotH + 18;
  bool rotate = n > 4;
  for (int i = 0; i < n; ++i) {
    double sx = svgSx(i + 1.0, x0, x1);
    std::string name = pts[static_cast<size_t>(i)].value("name", std::string("?"));
    std::string transformAttr =
        rotate ? (" transform=\"rotate(-35 " + fmtNum(sx) + " " + fmtNum(labelY, "%.0f") + ")\"") : "";
    lines.push_back("  <text x=\"" + fmtNum(sx) + "\" y=\"" + fmtNum(labelY, "%.0f") + "\" text-anchor=\"" +
                     (rotate ? "end" : "middle") + "\" font-size=\"10\" fill=\"#444\"" + transformAttr + ">" +
                     svgEscape(name) + "</text>");
  }

  for (int i = 0; i < n; ++i) {
    const ojson& d = pts[static_cast<size_t>(i)];
    double sx = svgSx(i + 1.0, x0, x1);
    double b = jnumOr(d, "b", 0.0);
    double ciLo = jnumOr(d, "ci_lo", b);
    double ciHi = jnumOr(d, "ci_hi", b);
    double syLo = svgSy(ciLo, y0, y1), syHi = svgSy(ciHi, y0, y1), syB = svgSy(b, y0, y1);
    double capLen = 4;
    lines.push_back("  <line x1=\"" + fmtNum(sx) + "\" y1=\"" + fmtNum(syLo) + "\" x2=\"" + fmtNum(sx) +
                     "\" y2=\"" + fmtNum(syHi) + "\" stroke=\"#1f77b4\" stroke-width=\"1.2\"/>");
    lines.push_back("  <line x1=\"" + fmtNum(sx - capLen) + "\" y1=\"" + fmtNum(syLo) + "\" x2=\"" +
                     fmtNum(sx + capLen) + "\" y2=\"" + fmtNum(syLo) + "\" stroke=\"#1f77b4\" stroke-width=\"1.2\"/>");
    lines.push_back("  <line x1=\"" + fmtNum(sx - capLen) + "\" y1=\"" + fmtNum(syHi) + "\" x2=\"" +
                     fmtNum(sx + capLen) + "\" y2=\"" + fmtNum(syHi) + "\" stroke=\"#1f77b4\" stroke-width=\"1.2\"/>");
    lines.push_back("  <circle cx=\"" + fmtNum(sx) + "\" cy=\"" + fmtNum(syB) + "\" r=\"3\" fill=\"#1f77b4\"/>");
  }

  lines.push_back("</svg>");
  std::string out;
  for (auto& l : lines) { out += l; out += "\n"; }
  if (!out.empty()) out.pop_back();
  return out;
}

std::string buildSvgCoefplot(const ojson& block) {
  struct Row {
    std::string label;
    double b;
    bool hasCI;
    double ciLo, ciHi;
  };
  std::vector<Row> rows;
  ojson models = block.value("models", ojson::array());
  bool multi = models.size() > 1;
  for (const auto& m : models) {
    std::string mname = m.value("name", std::string());
    for (const auto& c : m.value("coefs", ojson::array())) {
      std::string cname = c.value("name", std::string());
      bool hasCI = c.contains("ci_lo") && !c["ci_lo"].is_null() && c.contains("ci_hi") && !c["ci_hi"].is_null();
      rows.push_back({multi ? (mname + ":" + cname) : cname, jnumOr(c, "b", std::nan("")), hasCI,
                      hasCI ? jnumOr(c, "ci_lo", 0.0) : 0.0, hasCI ? jnumOr(c, "ci_hi", 0.0) : 0.0});
    }
  }

  std::vector<std::string> lines = {"<svg viewBox=\"0 0 520 360\" xmlns=\"http://www.w3.org/2000/svg\">"};
  svgTitle(lines, block.value("title", std::string()));

  int n = static_cast<int>(rows.size());

  // X: coefficient value, always widened to include 0 so the zero reference
  // line is in frame (mirrors renderCoefplot's `Math.min(0, xlo)`); falls
  // back to deriving a range from the rows themselves only if the block's
  // own x_min/x_max are missing/non-finite (runCoefplot always sets both,
  // so this fallback is defensive, not the normal path).
  double xlo = jnum(block.value("x_min", ojson(std::nan(""))), std::nan(""));
  double xhi = jnum(block.value("x_max", ojson(std::nan(""))), std::nan(""));
  if (!std::isfinite(xlo) || !std::isfinite(xhi)) {
    bool any = false;
    xlo = xhi = 0.0;
    for (const auto& r : rows) {
      std::vector<double> vs = {r.b};
      if (r.hasCI) { vs.push_back(r.ciLo); vs.push_back(r.ciHi); }
      for (double v : vs) {
        if (!std::isfinite(v)) continue;
        if (!any) { xlo = xhi = v; any = true; } else { xlo = std::min(xlo, v); xhi = std::max(xhi, v); }
      }
    }
  }
  xlo = std::min(xlo, 0.0);
  xhi = std::max(xhi, 0.0);
  auto xr = paddedRangeG(xlo, xhi, 0.05, 0.05);
  auto yr = paddedRangeG(0.5, n + 0.5, 0.05, 0.05);
  svgFrameAndAxes(lines, xr, yr, block.value("x_label", std::string("Coefficient estimate")), std::string(),
                  /*suppressXTickLabels=*/false, /*suppressYTickLabels=*/true);
  auto [x0, x1] = xr;
  auto [y0, y1] = yr;

  double zx = svgSx(0.0, x0, x1);
  if (zx >= kSvgML - 0.5 && zx <= kSvgML + kSvgPlotW + 0.5) {
    lines.push_back("  <line x1=\"" + fmtNum(zx) + "\" y1=\"" + fmtNum(kSvgMT, "%.0f") + "\" x2=\"" +
                     fmtNum(zx) + "\" y2=\"" + fmtNum(kSvgMT + kSvgPlotH, "%.0f") +
                     "\" stroke=\"#888\" stroke-width=\"1\" stroke-dasharray=\"3 3\"/>");
  }

  for (int i = 0; i < n; ++i) {
    double sy = svgSy(n - i, y0, y1);
    lines.push_back("  <text x=\"" + fmtNum(kSvgML - 9, "%.0f") + "\" y=\"" + fmtNum(sy + 3.5) +
                     "\" text-anchor=\"end\" font-size=\"10\" fill=\"#444\">" + svgEscape(rows[static_cast<size_t>(i)].label) +
                     "</text>");
  }

  for (int i = 0; i < n; ++i) {
    const Row& r = rows[static_cast<size_t>(i)];
    double sy = svgSy(n - i, y0, y1);
    if (r.hasCI) {
      double sxLo = svgSx(r.ciLo, x0, x1), sxHi = svgSx(r.ciHi, x0, x1);
      double capLen = 4;
      lines.push_back("  <line x1=\"" + fmtNum(sxLo) + "\" y1=\"" + fmtNum(sy) + "\" x2=\"" + fmtNum(sxHi) +
                       "\" y2=\"" + fmtNum(sy) + "\" stroke=\"#1f77b4\" stroke-width=\"1.2\"/>");
      lines.push_back("  <line x1=\"" + fmtNum(sxLo) + "\" y1=\"" + fmtNum(sy - capLen) + "\" x2=\"" + fmtNum(sxLo) +
                       "\" y2=\"" + fmtNum(sy + capLen) + "\" stroke=\"#1f77b4\" stroke-width=\"1.2\"/>");
      lines.push_back("  <line x1=\"" + fmtNum(sxHi) + "\" y1=\"" + fmtNum(sy - capLen) + "\" x2=\"" + fmtNum(sxHi) +
                       "\" y2=\"" + fmtNum(sy + capLen) + "\" stroke=\"#1f77b4\" stroke-width=\"1.2\"/>");
    }
    if (std::isfinite(r.b)) {
      double sxB = svgSx(r.b, x0, x1);
      lines.push_back("  <circle cx=\"" + fmtNum(sxB) + "\" cy=\"" + fmtNum(sy) + "\" r=\"3\" fill=\"#1f77b4\"/>");
    }
  }

  lines.push_back("</svg>");
  std::string out;
  for (auto& l : lines) { out += l; out += "\n"; }
  if (!out.empty()) out.pop_back();
  return out;
}

std::string buildSvgDispatch(const ojson& block);  // fwd decl for combine's recursive call

std::string buildSvgCombine(const ojson& block) {
  ojson grid = block.value("grid", ojson::object());
  int rows = std::max(1, grid.value("rows", 1));
  int cols = std::max(1, grid.value("cols", 1));
  double totalW = cols * kSvgW, totalH = rows * kSvgH;

  std::vector<std::string> lines = {"<svg viewBox=\"0 0 " + fmtNum(totalW, "%.0f") + " " +
                                     fmtNum(totalH, "%.0f") + "\" xmlns=\"http://www.w3.org/2000/svg\">"};

  // graphregion_color is always present in the block dict but may be JSON null
  // (graph_combine.py: unset graphregion(color(...)) opt leaves it None). A
  // null-but-present key makes nlohmann::json::value()'s default inapplicable
  // (defaults only cover an ABSENT key), so read it null-safely here — matching
  // the oracle's `if bg and bg != "none"` falsy-check (None/empty ⇒ no rect
  // painted, i.e. no default color substitution when unspecified).
  ojson bgJ = block.value("graphregion_color", ojson());
  std::string bg = bgJ.is_string() ? bgJ.get<std::string>() : std::string();
  if (!bg.empty() && bg != "none") {
    lines.push_back("  <rect x=\"0\" y=\"0\" width=\"" + fmtNum(totalW, "%.0f") + "\" height=\"" +
                     fmtNum(totalH, "%.0f") + "\" fill=\"" + bg + "\"/>");
  }

  std::string title = block.value("title", std::string());
  if (!title.empty()) {
    lines.push_back("  <text x=\"" + fmtNum(totalW / 2) + "\" y=\"20\" text-anchor=\"middle\" " +
                     "font-size=\"13\" font-weight=\"bold\" fill=\"#111\">" + svgEscape(title) + "</text>");
  }

  ojson panels = block.value("panels", ojson::array());
  for (size_t idx = 0; idx < panels.size(); ++idx) {
    int row = static_cast<int>(idx) / cols;
    int col = static_cast<int>(idx) % cols;
    if (row >= rows) break;
    double tx = col * kSvgW, ty = row * kSvgH;
    std::string subSvg;
    try {
      subSvg = buildSvgDispatch(panels[idx]);
    } catch (...) {
      subSvg = "<svg viewBox=\"0 0 520 360\" xmlns=\"http://www.w3.org/2000/svg\">"
                "<text x=\"10\" y=\"20\" font-size=\"10\" fill=\"red\">render error</text></svg>";
    }
    size_t bodyStart = subSvg.find('>');
    size_t bodyEnd = subSvg.rfind("</svg>");
    std::string inner = (bodyStart != std::string::npos && bodyEnd != std::string::npos && bodyEnd > bodyStart)
                             ? subSvg.substr(bodyStart + 1, bodyEnd - bodyStart - 1)
                             : std::string();
    // A panel widened past one cell (long legend labels) is scaled to fit its cell.
    std::string scaleAttr;
    double subW = kSvgW;
    size_t vb = subSvg.find("viewBox=\"0 0 ");
    if (vb != std::string::npos) subW = std::atof(subSvg.c_str() + vb + 13);
    if (subW > kSvgW + 0.5) scaleAttr = " scale(" + fmtNum(kSvgW / subW, "%.4f") + ")";
    lines.push_back("  <g transform=\"translate(" + fmtNum(tx, "%.0f") + "," + fmtNum(ty, "%.0f") + ")" +
                     scaleAttr + "\">");
    lines.push_back(inner);
    lines.push_back("  </g>");
  }

  lines.push_back("</svg>");
  std::string out;
  for (auto& l : lines) { out += l; out += "\n"; }
  if (!out.empty()) out.pop_back();
  return out;
}

std::string buildSvgDispatch(const ojson& block) {
  std::string kind = block.value("kind", std::string("scatter"));
  if (kind == "histogram") return buildSvgHistogram(block);
  if (kind == "line") return buildSvgLine(block);
  // BS-055: xtline/tsline previously had NO branch here at all -- both fell
  // through to buildSvgScatter, which reads a top-level "points" key
  // neither block shape carries (their data lives in "series"). Result:
  // frame/axes/title rendered fine (kind-agnostic), zero data ever drawn.
  if (kind == "xtline" || kind == "tsline") return buildSvgSeriesPlot(block, /*asMarkers=*/false);
  if (kind == "twoway") return buildSvgTwoway(block);
  if (kind == "graph_combine") return buildSvgCombine(block);
  if (kind == "marginsplot") return buildSvgMarginsplot(block);
  if (kind == "coefplot") return buildSvgCoefplot(block);
  return buildSvgScatter(block);
}

}  // namespace

std::string buildGraphSvg(const ojson& block) { return buildSvgDispatch(block); }

namespace {
// graph save's confirmation names the file as typed (.gph added when it has no
// suffix), and a new file is noted first; a name that does
// not end in .gph is "saved as .gph format" (StataBE 19.5, BUG-1528/1529:
// `graph save g n2sv2, replace` and `twoway ..., saving(n2sv1, replace)` print
// "(file n2sv2.gph not found)" then "file n2sv2.gph saved", never the absolute
// path; saving(v11.eps, replace) ends "file v11.eps saved as .gph format").
ojson gphSavedNote(const std::string& typed, bool replace, bool existed) {
  ojson out;
  out["type"] = "display";
  out["class"] = "note";
  out["text"] = (replace && !existed ? "(file " + typed + " not found)\n" : std::string()) + "file " + typed +
                (typed.size() >= 4 && typed.compare(typed.size() - 4, 4, ".gph") == 0 ? " saved"
                                                                                       : " saved as .gph format");
  return out;
}
}  // namespace

ojson runGraphSave(const std::optional<std::string>& name, const std::optional<std::string>& filepath,
                    const std::vector<std::pair<std::string, eval::OptValue>>& opts, SessionState& session) {
  bool replace = optBool(opts, "replace").value_or(false);

  // The named graph, else the current one. graph save never
  // registers a memory graph of its own (w2ai gf_use_059_gd, log:1529-1532).
  std::string gname;
  const ojson* blk = nullptr;
  if (name.has_value() && !name->empty()) {
    auto it = session.namedGraphs.find(*name);
    if (it == session.namedGraphs.end()) return stataRefusal("graph " + *name + " not found", 111);
    blk = &it->second;
    gname = *name;
  } else if (!session.currentGraph.empty() && session.namedGraphs.count(session.currentGraph) != 0) {
    gname = session.currentGraph;
    blk = &session.namedGraphs[gname];
  } else if (session.lastGraph.has_value()) {
    blk = &*session.lastGraph;
    gname = session.sersets.lastOwner();
  }
  if (blk == nullptr) {
    return engineErrorBlockG("graph save: no graph in memory. Run a graph command first.");
  }
  if (!filepath.has_value() || filepath->empty()) return stataRefusal("filename name required", 198);
  // .gph is appended to a file name without a suffix (w2ai gf_save_gp3
  // `graph save gp3 "fC"` writes fC.gph, log:1485-1491) -- a dot ANYWHERE in the
  // name as typed counts as one: StataBE uni-oracle/gs.do saves
  // "../uni-oracle/gsub/fb" and "./gsub/fc" as fb and fc ("saved as .gph
  // format"), as 007's `graph save "$output/.../Figure_A3"` with $output "..".
  std::string fp = resolveUserPath(session, *filepath);
  std::string typed = *filepath;  // what the messages name (BUG-1528)
  if (filepath->find('.') == std::string::npos) {
    fp += ".gph";
    typed += ".gph";
  }
  const bool existed = fileExistsG(fp);
  if (existed && !replace) return stataRefusal("file " + typed + " already exists", 602);
  ojson doc;
  doc["_magic"] = "PIETGPH1";
  doc["block"] = *blk;
  {
    // serset-twoway GST: the graph's sersets travel with it.
    const std::vector<serset::Serset> sets = serset::ownedBy(session, gname);
    if (!sets.empty()) doc["sersets"] = serset::toJson(sets);
    // graph-family AI-4: and so does its describe record.
    auto mt = session.graphMeta.find(gname);
    if (mt != session.graphMeta.end()) doc["meta"] = mt->second;
  }
  std::ofstream f(cecon::to_native_path(fp), std::ios::binary);
  if (!f) return stataRefusal("file " + fp + " could not be opened", 603);
  std::string text = doc.dump();
  f.write(text.data(), static_cast<std::streamsize>(text.size()));
  return gphSavedNote(typed, replace, existed);
}

// gfx-histogram-kdensity-family: a graph command's own saving(filename[,
// replace]) -- the same {"_magic":"PIETGPH1","block":...} file `graph save`
// writes, under the name as given (Stata appends .gph only to a name without an
// extension: hk_h_031disc saving("hs2.eps") -> hs2.eps, hk_h_069by -> hs1.gph).
// Unlike `graph save` it registers no extra in-memory name. Null on success,
// when `savedNote` receives gr_save's confirmation (BUG-1529).
ojson saveGraphOption(const ojson& block, const std::string& savingText, SessionState& session,
                      const std::vector<serset::Serset>* sersets, ojson* savedNote) {
  std::string body = savingText;
  std::string sub;
  // The file name is the first token, quoted or bare; suboptions follow a comma.
  std::string fname;
  size_t i = body.find_first_not_of(" \t");
  if (i == std::string::npos) return ojson(eval::errorBlock("syntax_error", "saving(): file name required",
                                                            std::nullopt, std::nullopt, std::nullopt, "r(198)"));
  if (body[i] == '"') {
    const size_t close = body.find('"', i + 1);
    fname = body.substr(i + 1, close == std::string::npos ? std::string::npos : close - i - 1);
    sub = close == std::string::npos ? std::string() : body.substr(close + 1);
  } else {
    const size_t stop = body.find_first_of(", \t", i);
    fname = body.substr(i, stop == std::string::npos ? std::string::npos : stop - i);
    sub = stop == std::string::npos ? std::string() : body.substr(stop);
  }
  const size_t comma = sub.find(',');
  bool replace = false;
  if (comma != std::string::npos) {
    std::istringstream ss(sub.substr(comma + 1));
    std::string tok;
    while (ss >> tok) {
      if (tok == "replace") {
        replace = true;
      } else if (tok != "asis") {
        return ojson(eval::errorBlock("syntax_error", "saving(): option " + tok + " not allowed", std::nullopt,
                                      std::nullopt, std::nullopt, "r(198)"));
      }
    }
  }
  std::string fp = resolveUserPath(session, fname);
  std::string typed = fname;  // what the messages name (BUG-1529)
  if (fname.find('.') == std::string::npos) {  // as graph save
    fp += ".gph";
    typed += ".gph";
  }
  const bool existed = fileExistsG(fp);
  if (existed && !replace) {
    return ojson(eval::errorBlock("engine_error", "file " + typed + " already exists", std::nullopt, std::nullopt,
                                  std::nullopt, "r(602)"));
  }
  ojson doc;
  doc["_magic"] = "PIETGPH1";
  doc["block"] = block;
  if (sersets != nullptr && !sersets->empty()) doc["sersets"] = serset::toJson(*sersets);
  // graph-family AI-4: the describe record of the graph just registered.
  {
    auto mt = session.graphMeta.find(session.currentGraph);
    if (mt != session.graphMeta.end()) doc["meta"] = mt->second;
  }
  std::ofstream f(cecon::to_native_path(fp), std::ios::binary);
  if (!f) {
    return ojson(eval::errorBlock("engine_error", "file " + fp + " could not be opened", std::nullopt,
                                  std::nullopt, std::nullopt, "r(603)"));
  }
  const std::string text = doc.dump();
  f.write(text.data(), static_cast<std::streamsize>(text.size()));
  if (savedNote != nullptr) *savedNote = gphSavedNote(typed, replace, existed);
  return ojson();
}

ojson runGraphUse(const std::string& fileArg,
                  const std::vector<std::pair<std::string, eval::OptValue>>& opts, SessionState& session) {
  ojson out = ojson::array();
  std::string fname = trimG(fileArg);
  if (fname.size() >= 2 && fname.front() == '"' && fname.back() == '"') fname = fname.substr(1, fname.size() - 2);
  if (fname.empty()) {
    out.push_back(stataRefusal("invalid file specification", 198));
    return out;
  }
  std::string fp = resolveUserPath(session, fname);
  {
    const size_t slash = fp.find_last_of('/');
    const std::string base = slash == std::string::npos ? fp : fp.substr(slash + 1);
    if (base.find('.') == std::string::npos) fp += ".gph";
  }
  if (!fileExistsG(fp)) {
    out.push_back(stataRefusal("file " + fp + " not found", 601));
    return out;
  }
  std::ifstream f(cecon::to_native_path(fp), std::ios::binary);
  std::ostringstream ss;
  ss << f.rdbuf();
  nlohmann::ordered_json doc;
  try {
    doc = nlohmann::ordered_json::parse(ss.str());
  } catch (...) {
    doc = nlohmann::ordered_json();
  }
  if (!doc.is_object() || doc.value("_magic", std::string()) != "PIETGPH1" || !doc.contains("block")) {
    out.push_back(stataRefusal("file " + fp + " is not a Stata .gph file", 610));
    return out;
  }
  // name(): as given, else the file's base name.
  std::string nameOpt;
  for (const auto& [k, v] : opts) {
    if (k == "name" && !v.isBool) nameOpt = v.strValue;
  }
  if (nameOpt.empty()) {
    const size_t slash = fp.find_last_of('/');
    std::string base = slash == std::string::npos ? fp : fp.substr(slash + 1);
    const size_t dot = base.find_last_of('.');
    if (dot != std::string::npos) base = base.substr(0, dot);
    for (char& c : base)
      if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) c = '_';
    nameOpt = base + ", replace";
  }
  if (auto clash = serset::nameCollision(session, nameOpt)) {
    out.push_back(*clash);
    return out;
  }
  ojson block = doc["block"];
  std::vector<serset::Serset> sets = serset::fromJson(doc.value("sersets", ojson::array()));
  // graph-family AI-8: the graph keeps the command it was drawn with (w2ai
  // gf_use_059_d, log:1506-1527).
  const ojson meta = doc.contains("meta") && doc["meta"].is_object() ? doc["meta"]
                                                                       : serset::makeGraphMeta("twoway", "", "");
  serset::registerGraph(session, block, nameOpt, std::move(sets), &meta);
  decimateGraphBlockForView(block);
  out.push_back(block);
  return out;
}

ojson runGraphCombine(const std::vector<std::string>& names,
                      const std::vector<std::pair<std::string, eval::OptValue>>& opts, SessionState& session) {
  ojson panels = ojson::array();
  for (const auto& raw : names) {
    // BS-007 (D3): each combine token resolves as EITHER a bare in-memory
    // graph NAME (session.namedGraphs -- the pre-existing behavior) OR a
    // quoted on-disk `.gph` spec-JSON path. Real-world need: analysis.do's
    // loop-then-combine idiom `graph save` (above)s each iteration's graph
    // to a DISTINCT tempfile while reusing the SAME in-memory name every
    // iteration (a Stata `tempfile` local re-bound per loop pass), so by
    // the time `graph combine` runs, the in-memory name holds only the
    // LAST iteration's graph -- only the on-disk .gph files carry every
    // iteration's distinct graph (analysis.do:1533's `graph combine
    // `graph_list'` where `graph_list` accumulates quoted tempfile paths,
    // not names -- BS-007 evidence). dispatcher.cpp's varlist tokenizer is
    // NOT quote-aware (eval/tokenizer.cpp's splitWs is a bare `>>` split),
    // so a token can arrive as e.g. `"a.gph"` with the quote characters
    // still embedded -- one-layer dequote first (same single-pair-strip
    // rule as handlers.cpp's dequoteUserPath, replicated here per this
    // TU's established per-file small-helper convention -- see
    // firstNameToken above) before either lookup.
    std::string tok = raw;
    bool quotedTok = false;
    {
      size_t a = tok.find_first_not_of(" \t");
      size_t b = tok.find_last_not_of(" \t");
      tok = (a == std::string::npos) ? std::string() : tok.substr(a, b - a + 1);
      if (tok.size() >= 2 && ((tok.front() == '"' && tok.back() == '"') ||
                              (tok.front() == '\'' && tok.back() == '\''))) {
        tok = tok.substr(1, tok.size() - 2);
        quotedTok = true;
      }
    }

    auto it = session.namedGraphs.find(tok);
    if (it != session.namedGraphs.end()) {
      panels.push_back(it->second);
      continue;
    }
    // graph-family AI-11: gr_combine reads an unquoted word without a suffix or
    // a path as a memory graph name: `gf_nosuch is not a memory graph`, r(198)
    // (w2ai gf_cmb_absent, log:1817-1823).
    if (!quotedTok && tok.find('.') == std::string::npos && tok.find('/') == std::string::npos &&
        tok.find('\\') == std::string::npos) {
      return stataRefusal(tok + " is not a memory graph", 198);
    }

    std::string fp = resolveUserPath(session, tok);
    {
      const size_t slash = fp.find_last_of('/');
      const std::string base = slash == std::string::npos ? fp : fp.substr(slash + 1);
      if (base.find('.') == std::string::npos) fp += ".gph";  // gs_fileinfo, suffix
    }
    auto loaded = loadGphFile(fp);
    if (!loaded.has_value()) {
      return engineErrorBlockG("graph combine: no graph named '" + tok +
                                "' and no readable .gph file at '" + fp +
                                "' (use `graph save` first).");
    }
    panels.push_back(*loaded);
  }
  if (panels.empty()) {
    return engineErrorBlockG("graph combine: no graph names given.");
  }

  int n = static_cast<int>(panels.size());
  int cols = 0, rows = 0;
  auto colsOpt = optStr(opts, "cols");
  auto rowsOpt = optStr(opts, "rows");
  if (colsOpt.has_value()) {
    try { cols = std::stoi(*colsOpt); } catch (...) { cols = 0; }
  }
  if (rowsOpt.has_value()) {
    try { rows = std::stoi(*rowsOpt); } catch (...) { rows = 0; }
  }
  if (cols <= 0 && rows <= 0) {
    cols = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(n))));
    rows = static_cast<int>(std::ceil(static_cast<double>(n) / cols));
  } else if (cols <= 0) {
    cols = static_cast<int>(std::ceil(static_cast<double>(n) / rows));
  } else if (rows <= 0) {
    rows = static_cast<int>(std::ceil(static_cast<double>(n) / cols));
  }

  ojson out;
  out["type"] = "graph";
  out["kind"] = "graph_combine";
  out["title"] = optTitleStr(opts, "title").value_or(std::string());
  out["subtitle"] = optTitleStr(opts, "subtitle").value_or(std::string());
  out["note"] = optTitleStr(opts, "note").value_or(std::string());
  out["panels"] = panels;
  ojson grid;
  grid["rows"] = rows;
  grid["cols"] = cols;
  out["grid"] = grid;
  out["imargin"] = optStr(opts, "imargin").has_value() ? ojson(*optStr(opts, "imargin")) : ojson(nullptr);
  out["graphregion_color"] = optStr(opts, "graphregion").has_value() ? ojson(*optStr(opts, "graphregion"))
                                                                       : ojson(nullptr);
  out["n_panels"] = n;
  // BS-047 (BETA A4): see runTwowayLayers' identical comment above.
  out["suppressed"] = session.graphicsOff;

  session.lastGraph = out;
  auto nameOpt = optStr(opts, "name");
  if (nameOpt.has_value() && !nameOpt->empty()) {
    // BS-057: `name(X, replace)` -- strip the glued `, replace` (or any
    // other suboption) the same way the bare/twoway name() capture does, so
    // the stored key is the bare name "X", not the literal "X, replace"
    // (previously baked verbatim into session.namedGraphs' key).
    std::string nm = firstNameToken(*nameOpt);
    if (!nm.empty()) {
      session.namedGraphs[nm] = out;
      out["name"] = nm;
    } else {
      out["name"] = nullptr;
    }
  } else {
    out["name"] = nullptr;
  }
  return out;
}

ojson runGraphExport(const std::string& filepathArg,
                      const std::vector<std::pair<std::string, eval::OptValue>>& opts, SessionState& session) {
  std::string filepath = resolveUserPath(session, filepathArg);

  // W:P8 (BUG-169): `name()` was parsed and then never read, so
  // `graph export FILE, name(X)` ALWAYS wrote session.lastGraph — the current
  // graph, not X. Exporting a name that does not exist returned rc 0 and a
  // file byte-identical to exporting a name that does, which is the cardinal
  // silent-wrong-answer shape: the P8 target's own
  // `graph export "_runs/p8_graph.svg", name(p8all)` would have shipped the
  // last histogram in place of the three-panel combine with every pin green.
  // Stata answers an unknown name with "could not find Graph window", r(693).
  const ojson* blk = nullptr;
  auto nameOpt = optStr(opts, "name");
  if (nameOpt.has_value()) {
    std::string want = *nameOpt;
    size_t a = want.find_first_not_of(" \t");
    size_t b = want.find_last_not_of(" \t");
    want = (a == std::string::npos) ? std::string() : want.substr(a, b - a + 1);
    if (!want.empty()) {
      auto it = session.namedGraphs.find(want);
      if (it == session.namedGraphs.end()) {
        return engineErrorBlockG("graph export: could not find Graph window " + want);
      }
      blk = &it->second;
    }
  }
  if (blk == nullptr && !session.lastGraph.has_value()) {
    return engineErrorBlockG(
        "graph export: no graph in memory. Run a graph command first (e.g. `scatter price mpg`).");
  }
  const ojson& exportBlock = blk != nullptr ? *blk : *session.lastGraph;

  // BUG-103: `dot` used to be `filepath.find_last_of('.')` over the WHOLE
  // path, not just the basename. Under a TMPDIR/workdir containing a dot in
  // some directory component (e.g. a scratch dir literally named
  // `.tmp-scratch`), that scan finds the directory's dot instead of a real
  // filename extension whenever the filename itself is extensionless --
  // `/…/.tmp-scratch/noext_plot` has its ONLY '.' inside `.tmp-scratch`.
  // Two consequences, both silent: (1) extFromName above would read back a
  // directory-path fragment instead of "" ; (2) far worse, the as(png)/pdf
  // fallback path below (`filepath.substr(0, dot) + ".svg"`) TRUNCATES THE
  // PATH at that same wrong dot, producing a garbage sibling path outside
  // the intended directory -- the write silently lands nowhere the caller
  // (or the BS-012 selftest leg) ever looks, i.e. "writes nothing" under a
  // dotted TMPDIR while working fine under `/tmp` (which has no dot in a
  // typical path). Fix: only recognise a '.' that appears AFTER the last
  // path separator (i.e. within the basename) as a real extension.
#ifdef _WIN32
  size_t baseStart = filepath.find_last_of("/\\");
#else
  size_t baseStart = filepath.find_last_of('/');
#endif
  baseStart = (baseStart == std::string::npos) ? 0 : baseStart + 1;
  size_t dot = filepath.find_last_of('.');
  if (dot != std::string::npos && dot < baseStart) dot = std::string::npos;
  std::string extFromName = dot == std::string::npos ? std::string() : filepath.substr(dot + 1);
  std::transform(extFromName.begin(), extFromName.end(), extFromName.begin(),
                 [](unsigned char c) { return std::tolower(c); });

  // BS-012 (D8): `as(fmt)` names the export format explicitly and wins over
  // the filename's own extension when given (Stata's `graph export FILE,
  // as(fmt)` semantics) -- previously parsed into `opts` by the shared
  // option grammar and then silently never read again.
  std::string fmt;
  auto asOpt = optStr(opts, "as");
  if (asOpt.has_value()) {
    fmt = *asOpt;
    size_t a = fmt.find_first_not_of(" \t");
    size_t b = fmt.find_last_not_of(" \t");
    fmt = (a == std::string::npos) ? std::string() : fmt.substr(a, b - a + 1);
    std::transform(fmt.begin(), fmt.end(), fmt.begin(), [](unsigned char c) { return std::tolower(c); });
  } else {
    fmt = extFromName;
  }

  if (fmt != "png" && fmt != "pdf" && fmt != "svg" && fmt != "eps" && fmt != "ps" && fmt != "jpg" &&
      fmt != "jpeg") {
    return eval::engineError(
        "graph export: unsupported format '" + fmt + "'. Use a .png/.jpg/.pdf/.eps/.svg filename, or `, as(png|jpg|pdf|eps|svg)`.",
        "syntax_error");
  }

  bool replace = optBool(opts, "replace").value_or(false);
  const bool existed = fileExistsG(filepath);
  if (existed && !replace) {
    // gr_export: r(602) `file ... already exists` (w2ai gf_exp_noreplace, log:2005-2011),
    // the file named as typed (StataBE 19.5, BUG-1528: "file ex1.png already exists").
    return stataRefusal("file " + filepathArg + " already exists", 602);
  }
  // BUG-1528: Stata's PNG, JPG and PDF writers name the file by its absolute path
  // and never note a new one; its EPS, PostScript and SVG writers open the file
  // as typed, so a new one under replace is noted first (StataBE 19.5,
  // scratch-laneA2/agents/gfx/g1.do, g2.do: `graph export ex1.png, replace`
  // prints only "file <abs>/ex1.png saved as PNG format"; ex4.eps prints
  // "(file ex4.eps not found)" then "file ex4.eps saved as EPS format", ex5.ps
  // "... saved as PostScript format", ex6.svg "... saved as SVG format").
  auto savedText = [&](const std::string& fmtName) {
    if (fmt != "eps" && fmt != "ps" && fmt != "svg") return "file " + filepath + " saved as " + fmtName + " format";
    return (replace && !existed ? "(file " + filepathArg + " not found)\n" : std::string()) + "file " +
           filepathArg + " saved as " + fmtName + " format";
  };

#ifdef _WIN32
  // Absolute Windows paths use '\\'; find_last_of('/') would miss it and treat
  // the whole path as the parent, so `graph export C:\dir\plot.svg` wrongly
  // reports "directory does not exist". Match either separator. POSIX unchanged.
  auto psep = filepath.find_last_of("/\\");
  std::string parent = (psep == std::string::npos) ? std::string() : filepath.substr(0, psep);
#else
  std::string parent = filepath.substr(0, filepath.find_last_of('/'));
#endif
  if (!parent.empty() && !cecon::native_exists(parent)) {
    return engineErrorBlockG("graph export: directory does not exist: " + parent);
  }

  if (fmt == "svg") {
    std::string svg = buildGraphSvg(exportBlock);
    std::ofstream f(cecon::to_native_path(filepath), std::ios::binary);
    if (!f) return engineErrorBlockG("graph export: could not write file: " + filepath);
    f.write(svg.data(), static_cast<std::streamsize>(svg.size()));
    f.close();
    ojson out;
    out["type"] = "display";
    out["text"] = savedText("SVG");
    return out;
  }

  // graph-family AI-13: png/pdf/eps are written in that format at the path AS
  // GIVEN -- `graph export "x2", as(png)` writes x2, never x2.png (w2ai
  // gf_exp_asonly_f 0 / _png_f 601, log:1987-2001); gf_exp_069 (eps),
  // gf_exp_004 (png, width/height), gf_exp_019 (pdf), log:1922-1983. The
  // drawing is the graph's structural SVG replayed by graph_export_formats.cpp
  // (a real PNG/PDF/EPS, never SVG bytes under another name).
  int widthPx = 0, heightPx = 0;
  if (auto w = optStr(opts, "width")) widthPx = std::atoi(w->c_str());
  if (auto h = optStr(opts, "height")) heightPx = std::atoi(h->c_str());
  std::string bytes;
  const std::string svg = buildGraphSvg(exportBlock);
  if (!graphSvgToFormat(svg, fmt, widthPx, heightPx, &bytes)) {
    return eval::engineError("graph export: unsupported format '" + fmt + "'", "syntax_error");
  }
  std::ofstream f(cecon::to_native_path(filepath), std::ios::binary);
  if (!f) return stataRefusal("file " + filepath + " could not be opened", 603);
  f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  f.close();
  // (StataBE 19.5, g1.do/g2.do: `.jpg` and as(jpg) are "JPG", `.ps` "PostScript".)
  const std::string fmtName = fmt == "png"   ? "PNG"
                              : fmt == "pdf" ? "PDF"
                              : fmt == "eps" ? "EPS"
                              : fmt == "ps"  ? "PostScript"
                                             : "JPG";
  ojson out;
  out["type"] = "display";
  out["text"] = savedText(fmtName);
  return out;
}

// ===========================================================================
// L2-FIX-B (BUG-066 / COUNCIL-20260710 C3-F5): LIVE-VIEW point decimation.
//
// The graph pipeline shipped and rendered one JSON+SVG node per data point,
// unbounded -- a 108K/1M-point scatter is 108K/1M DOM nodes plus a
// multi-megabyte block copied across the native->frontend bridge, which the
// webview cannot absorb (BUG-066). C3-F5's ruling: density-preserving
// decimation for the LIVE VIEW ONLY with an honest "(showing N of M points)"
// caption; `graph export` keeps ALL points (the golden-pinned SVG builder =
// the REPIN seam) and so does everything else that reads session.lastGraph /
// namedGraphs (graph export/save/combine).
//
// The caller (dispatcher's graph blocks.push_back seams) invokes this AFTER
// the FULL block has already been deep-copied into session.lastGraph (and
// namedGraphs), so thinning the console-bound copy here shrinks ONLY the live
// view -- the export path is untouched, byte-for-byte, no re-pin.
//
// Method: an order-preserving proportional stride (keep every ceil(n/target)-th
// point, always anchoring the last one). Proportional subsampling preserves the
// cloud's relative DENSITY by construction (a region with k times more points
// keeps k times more samples), and order-preservation keeps connected-line /
// tsline shapes intact -- so one primitive serves scatter and line kinds alike.
// The axis ranges (x_min..y_max) are computed by the handler from the FULL data
// and left untouched, so the frame still spans the true extent even when a
// corner point is sampled out.
namespace {
// Thin a [[x,y],...] array to <= `target` entries in place (order-preserving,
// last point always retained). Returns the resulting (shown) count.
size_t strideThinPoints(ojson& arr, size_t target) {
  if (!arr.is_array()) return 0;
  const size_t n = arr.size();
  if (target == 0 || n <= target) return n;
  const size_t step = (n + target - 1) / target;  // ceil(n/target), always >= 2
  ojson kept = ojson::array();
  for (size_t i = 0; i < n; i += step) kept.push_back(arr[i]);
  if ((n - 1) % step != 0) kept.push_back(arr[n - 1]);  // anchor the last point
  arr = std::move(kept);
  return arr.size();
}
}  // namespace

void decimateGraphBlockForView(ojson& block) {
  if (!block.is_object() || block.value("type", std::string()) != "graph") return;

  static constexpr size_t kMaxViewPoints = 5000;  // DOM/bridge budget per graph
  size_t total = 0, shown = 0;
  auto thinCounted = [&](ojson& pts) {
    if (!pts.is_array()) return;
    total += pts.size();
    shown += strideThinPoints(pts, kMaxViewPoints);
  };

  if (block.contains("layers") && block["layers"].is_array()) {
    // twoway: real data lives in each rendered layer's own points array.
    for (auto& lyr : block["layers"])
      if (lyr.is_object() && lyr.contains("points")) thinCounted(lyr["points"]);
  } else if (block.contains("series") && block["series"].is_array()) {
    // multi-y scatter / xtline / tsline: data lives in series[].points.
    for (auto& s : block["series"])
      if (s.is_object() && s.contains("points")) thinCounted(s["points"]);
    // The flat top-level "points" is native's backward-compat UNION of the
    // series; thin it too so the bridge copy shrinks, but do NOT count it --
    // that would double-count the series points.
    if (block.contains("points") && block["points"].is_array())
      strideThinPoints(block["points"], kMaxViewPoints);
  } else if (block.contains("points") && block["points"].is_array()) {
    // single scatter / line / connected.
    thinCounted(block["points"]);
  }

  if (shown < total) {
    block["n_shown_points"] = shown;
    block["n_total_points"] = total;
  }
}

}  // namespace piet::handlers
