// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Portions derived from acreg 1.1.0 (GPL-3.0).
// See THIRD_PARTY_NOTICES.
//
// handlers_e3_acreg.cpp -- `acreg` (Colella, Lalive, Sakalli, Thoenig; SSC,
// acreg.ado "Version December 2020 (1.1.0)"): OLS/2SLS with arbitrary-cluster
// (Conley spatial + within-id temporal) standard errors, derived from
// acreg.ado and scope-ge2/e3/acreg/IDENTITY.md. Oracle:
// w2j_e3_acreg.{do,log}.
//
// The corpus paths only (IDENTITY.md section 0): acreg -> acreg_core ->
// acreg_no_corr (ado) / acreg_spat_ll_no_bart (ado) /
// acreg_spat_ll_bart (ado), pfe1()/pfe2() absorbed by an exact two-way
// within transform (hdfe's job, ado), and the Kleibergen-Paap weak-id
// Wald F (acreg_ranktest, ado) for one endogenous regressor and one
// excluded instrument. Every other kernel (network, weights(), cluster(),
// dist_mat(), links_mat(), storeweights ...) is refused by name, never guessed.
//
// Check order and return codes follow the ado line by line: the syntax and
// varlist parse (198/111/109), repeated variables (198, ado), lat/lon
// numeric/missing/range (198, ado), the id/time/lag/hac rules (498,
// ado) -- all BEFORE `sort id time` -- then the sort (ado, the
// data stay sorted, arms' `nsrt` 0), the spatial companion rules (498,
// ado), factor variables with pfe1 (198, ado) and pfe2 without
// pfe1 (498, ado).
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "handlers_e3_common.h"
#include "piet/datastore.h"
#include "piet/eval/errors.h"
#include "piet/handlers.h"
#include "piet/loops.h"
#include "piet/session_state.h"
#include "piet/strutil.h"
#include "vendor/nlohmann/json.hpp"

namespace piet {
namespace handlers {

namespace {

using e3::ojson;
using e3::rcError;
using Mat = std::vector<std::vector<double>>;
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
const char* const kTouse = "__acreg_touse";

ojson displayBlockA(const std::string& text) { return e3::displayText(text); }

// Words of a string.
std::vector<std::string> words(const std::string& s) {
  std::istringstream is(s);
  std::vector<std::string> out;
  for (std::string t; is >> t;) out.push_back(t);
  return out;
}
std::string joinWords(const std::vector<std::string>& v) {
  std::string out;
  for (const auto& w : v) out += (out.empty() ? "" : " ") + w;
  return out;
}

// Stata's `list dups` (each repeated occurrence, in order) and acreg's literal
// `subinstr("`list'","`dupvars' ","",1)` de-duplication (ado).
std::vector<std::string> acregDedup(const std::vector<std::string>& in) {
  std::vector<std::string> dups;
  std::set<std::string> seen;
  for (const auto& w : in) {
    if (seen.count(w)) dups.push_back(w);
    seen.insert(w);
  }
  if (dups.empty()) return in;
  std::string s = joinWords(in);
  const std::string pat = joinWords(dups) + " ";
  const size_t at = s.find(pat);
  if (at != std::string::npos) s.erase(at, pat.size());
  return words(s);
}

// Cross products in extended precision (Mata's quadcross).
Mat crossLD(const Mat& A, const Mat& B) {  // A' B for column-stored N x p, N x q (A[p][i])
  const size_t p = A.size(), q = B.size();
  Mat out(p, std::vector<double>(q, 0.0));
  for (size_t a = 0; a < p; ++a)
    for (size_t b = 0; b < q; ++b) {
      long double s = 0;
      const auto& x = A[a];
      const auto& y = B[b];
      for (size_t i = 0; i < x.size(); ++i) s += static_cast<long double>(x[i]) * y[i];
      out[a][b] = static_cast<double>(s);
    }
  return out;
}
Mat matmul(const Mat& A, const Mat& B) {  // row-major A (r x k), B (k x c)
  const size_t r = A.size(), k = B.size(), c = B.empty() ? 0 : B[0].size();
  Mat out(r, std::vector<double>(c, 0.0));
  for (size_t i = 0; i < r; ++i)
    for (size_t t = 0; t < k; ++t) {
      const double a = A[i][t];
      if (a == 0) continue;
      for (size_t j = 0; j < c; ++j) out[i][j] += a * B[t][j];
    }
  return out;
}
Mat transpose(const Mat& A) {
  if (A.empty()) return {};
  Mat out(A[0].size(), std::vector<double>(A.size()));
  for (size_t i = 0; i < A.size(); ++i)
    for (size_t j = 0; j < A[i].size(); ++j) out[j][i] = A[i][j];
  return out;
}

// Residualise each column (over the sample rows) on one or two sets of fixed
// effects: the exact within transform hdfe produces (ado,
// `keepsingletons`), by alternating projections run to machine precision.
void absorbFixedEffects(std::vector<std::vector<double>>& cols, const std::vector<double>& fe1,
                        const std::vector<double>* fe2) {
  const size_t n = fe1.size();
  auto groupIndex = [n](const std::vector<double>& f) {
    std::map<double, size_t> m;
    std::vector<size_t> g(n);
    for (size_t i = 0; i < n; ++i) {
      auto it = m.find(f[i]);
      if (it == m.end()) it = m.emplace(f[i], m.size()).first;
      g[i] = it->second;
    }
    return std::make_pair(g, m.size());
  };
  const auto [g1, n1] = groupIndex(fe1);
  std::vector<size_t> g2;
  size_t n2 = 0;
  if (fe2 != nullptr) {
    auto r = groupIndex(*fe2);
    g2 = r.first;
    n2 = r.second;
  }
  auto demean = [n](std::vector<double>& v, const std::vector<size_t>& g, size_t ng) {
    std::vector<long double> sum(ng, 0);
    std::vector<size_t> cnt(ng, 0);
    for (size_t i = 0; i < n; ++i) {
      sum[g[i]] += v[i];
      ++cnt[g[i]];
    }
    for (size_t i = 0; i < n; ++i) v[i] -= static_cast<double>(sum[g[i]] / cnt[g[i]]);
  };
  for (auto& v : cols) {
    if (fe2 == nullptr || n2 <= 1) {
      demean(v, g1, n1);
      continue;
    }
    for (int it = 0; it < 100000; ++it) {
      std::vector<double> before = v;
      demean(v, g1, n1);
      demean(v, g2, n2);
      double diff = 0, scale = 0;
      for (size_t i = 0; i < n; ++i) {
        diff = std::max(diff, std::fabs(v[i] - before[i]));
        scale = std::max(scale, std::fabs(v[i]));
      }
      if (diff <= 1e-15 * std::max(scale, 1e-300)) break;
    }
  }
}

}  // namespace

// Returns an ARRAY of blocks; the dispatcher pushes each element (never the
// array as one block -- the type_error.306 class).
nlohmann::ordered_json runAcreg(const eval::ParsedCommand& parsed, SessionState& session) {
  ojson blocks = ojson::array();
  auto fail = [&blocks](ojson e) {
    blocks.push_back(std::move(e));
    return blocks;
  };
  // ado: replay prints "No Variables" and leaves e() alone (log:3183 R14).
  const std::string argText = strutil::trim(parsed.argText);
  if (argText.empty() && parsed.opts.empty() && !parsed.ifExpr.has_value() &&
      !parsed.inRange.has_value()) {
    blocks.push_back(displayBlockA("No Variables"));
    return blocks;
  }
  if (!session.hasData()) return fail(ojson(eval::noDataLoaded()));

  // ---- syntax (ado) ------------------------------------------------------
  static const std::vector<e3::OptSpec> kSpecs = {
      {"dist_mat", 8, true},        {"links_mat", 9, true},     {"weights", 7, true},
      {"cluster", 7, true},         {"latitude", 8, true},      {"longitude", 9, true},
      {"id", 2, true},              {"time", 4, true},          {"lagcutoff", 3, true},
      {"distcutoff", 4, true},      {"lagdistcutoff", 7, true}, {"bartlett", 8, false},
      {"partial", 7, false},        {"correction", 10, false},  {"network", 7, false},
      {"spatial", 7, false},        {"storeweights", 12, false}, {"storedistances", 14, false},
      {"small", 5, false},          {"ak0", 3, false},          {"pfe1", 4, true},
      {"pfe2", 4, true},            {"hac", 3, false},          {"correctr2", 9, false},
      {"nbclust", 7, true},         {"dropsingletons", 14, false}};
  std::string latitude, longitude, idv, timev, pfe1, pfe2;
  long long lagcutoff = -1, lagdistcutoff = -1, nbclust = -1;
  double distcutoff = -1;
  bool bartlett = false, spatial = false, hac = false;
  ojson err;
  auto varnameOpt = [&](const std::string& arg, std::string* out) -> bool {
    std::vector<std::string> v;
    if (!e3::expandVars(arg, session, &v, &err)) return false;
    if (v.size() != 1) {
      err = rcError("too many variables specified", 103);
      return false;
    }
    *out = v[0];
    return true;
  };
  auto intOpt = [&](const std::string& name, const std::string& arg, long long* out) -> bool {
    char* end = nullptr;
    const double d = std::strtod(arg.c_str(), &end);
    if (arg.empty() || end == nullptr || *end != '\0' || d != std::floor(d)) {
      err = rcError("option " + name + "() incorrectly specified", 198);
      return false;
    }
    *out = static_cast<long long>(d);
    return true;
  };
  for (const auto& [k, v] : parsed.opts) {
    const int idx = e3::matchOpt(kSpecs, k);
    if (idx < 0) return fail(rcError("option " + k + " not allowed", 198));
    const e3::OptSpec& s = kSpecs[static_cast<size_t>(idx)];
    if (s.takesArg && v.isBool) return fail(rcError("option " + s.full + "() required", 198));
    if (!s.takesArg && !v.isBool) return fail(rcError("option " + s.full + " not allowed", 198));
    const std::string arg = strutil::trim(v.strValue);
    const std::string& o = s.full;
    if (o == "latitude") {
      if (!varnameOpt(arg, &latitude)) return fail(err);
    } else if (o == "longitude") {
      if (!varnameOpt(arg, &longitude)) return fail(err);
    } else if (o == "id") {
      if (!varnameOpt(arg, &idv)) return fail(err);
    } else if (o == "time") {
      if (!varnameOpt(arg, &timev)) return fail(err);
    } else if (o == "pfe1") {
      if (!varnameOpt(arg, &pfe1)) return fail(err);
    } else if (o == "pfe2") {
      if (!varnameOpt(arg, &pfe2)) return fail(err);
    } else if (o == "lagcutoff") {
      if (!intOpt(o, arg, &lagcutoff)) return fail(err);
    } else if (o == "lagdistcutoff") {
      if (!intOpt(o, arg, &lagdistcutoff)) return fail(err);
    } else if (o == "nbclust") {
      if (!intOpt(o, arg, &nbclust)) return fail(err);
    } else if (o == "distcutoff") {
      char* end = nullptr;
      distcutoff = std::strtod(arg.c_str(), &end);
      if (arg.empty() || end == nullptr || *end != '\0')
        return fail(rcError("option distcutoff() incorrectly specified", 198));
    } else if (o == "bartlett") {
      bartlett = true;
    } else if (o == "spatial") {
      spatial = true;
    } else if (o == "hac") {
      hac = true;
    } else {
      // Real acreg options whose kernels/corrections this port does not carry
      // (IDENTITY.md section 0): refused by name, never silently ignored.
      return fail(rcError("acreg, " + o + (s.takesArg ? "()" : "") +
                              ": not implemented in this version of PIET (only the "
                              "heteroskedastic, temporal and spatial latitude/longitude kernels "
                              "are); nothing was estimated",
                          198, "unsupported_option"));
    }
  }

  // ---- the varlist with its one `(end = iv)` group (ado) ------------------
  std::vector<std::string> exogTok, endTok, ivTok;
  int groups = 0;
  {
    size_t i = 0;
    const std::string& t = argText;
    while (i < t.size()) {
      if (std::isspace(static_cast<unsigned char>(t[i]))) {
        ++i;
        continue;
      }
      if (t[i] == '(') {
        int depth = 0;
        size_t j = i;
        for (; j < t.size(); ++j) {
          if (t[j] == '(') ++depth;
          else if (t[j] == ')' && --depth == 0) break;
        }
        const std::string inner = t.substr(i + 1, (j < t.size() ? j : t.size()) - i - 1);
        if (++groups > 1)
          return fail(rcError("syntax is \"(all instrumented variables = instrument variables)\"",
                              198));
        const size_t eq = inner.find('=');
        if (eq == std::string::npos)
          return fail(rcError("the equal sign \"=\" is required", 198));
        endTok = words(inner.substr(0, eq));
        ivTok = words(inner.substr(eq + 1));
        i = j + 1;
        continue;
      }
      size_t j = i;
      while (j < t.size() && !std::isspace(static_cast<unsigned char>(t[j])) && t[j] != '(') ++j;
      exogTok.push_back(t.substr(i, j - i));
      i = j;
    }
  }
  if (exogTok.empty()) return fail(rcError("varlist required", 100));
  // prog_fv_unab (ado): factor terms stay `i.x`, the rest tsunab'ed;
  // `syntax varlist(numeric fv)` makes a string variable r(109).
  std::vector<std::string> factorTerms;
  auto unab = [&](const std::vector<std::string>& toks, std::vector<std::string>* out) -> bool {
    for (const auto& tk : toks) {
      if (tk.size() > 2 && (tk.compare(0, 2, "i.") == 0 || tk.compare(0, 2, "I.") == 0)) {
        std::vector<std::string> v;
        if (!e3::expandVars(tk.substr(2), session, &v, &err)) return false;
        for (const auto& x : v) {
          out->push_back("i." + x);
          factorTerms.push_back("i." + x);
        }
        continue;
      }
      if (tk.find('.') != std::string::npos || tk.find('#') != std::string::npos) {
        err = rcError("acreg: the term `" + tk + "` (a time-series or factor operator) is not "
                      "implemented in this version of PIET; nothing was estimated",
                      198, "unsupported_option");
        return false;
      }
      std::vector<std::string> v;
      if (!e3::expandVars(tk, session, &v, &err)) return false;
      out->insert(out->end(), v.begin(), v.end());
    }
    Datastore* ds = session.datastore();
    for (const auto& x : *out) {
      const std::string nm = x.compare(0, 2, "i.") == 0 ? x.substr(2) : x;
      if (ds->dtype(nm) != "f64") {
        err = rcError("type mismatch", 109);
        return false;
      }
    }
    return true;
  };
  std::vector<std::string> end1, ivv1, exogAll;
  if (!endTok.empty() && !unab(endTok, &end1)) return fail(err);
  if (!ivTok.empty() && !unab(ivTok, &ivv1)) return fail(err);
  if (!unab(exogTok, &exogAll)) return fail(err);
  const std::string depvar = exogAll.front();
  std::vector<std::string> exog1(exogAll.begin() + 1, exogAll.end());
  // ado: each list de-duplicated, then a variable in two roles is r(198).
  exog1 = acregDedup(exog1);
  end1 = acregDedup(end1);
  ivv1 = acregDedup(ivv1);
  {
    std::vector<std::string> all = exog1;
    all.push_back(depvar);
    all.insert(all.end(), end1.begin(), end1.end());
    all.insert(all.end(), ivv1.begin(), ivv1.end());
    std::vector<std::string> dups;
    std::set<std::string> seen;
    for (const auto& w : all) {
      if (seen.count(w)) dups.push_back(w);
      seen.insert(w);
    }
    if (!dups.empty())
      return fail(rcError("Error: there are repeated variables: <" + joinWords(dups) + ">", 198));
  }
  // ado: latitude/longitude numeric, nonmissing in ALL the data, in [-180, 180].
  {
    Datastore* ds = session.datastore();
    for (const std::string* v : {&latitude, &longitude}) {
      if (v->empty()) continue;
      if (ds->dtype(*v) != "f64") return fail(rcError(*v + " is not a numeric variable", 198));
    }
    for (const std::string* v : {&latitude, &longitude}) {
      if (v->empty()) continue;
      const double* p = ds->col(*v).f64();
      for (int64_t i = 0; i < ds->nRows(); ++i)
        if (std::isnan(p[i])) return fail(rcError(*v + " contains missing values", 198));
    }
    for (const std::string* v : {&latitude, &longitude}) {
      if (v->empty()) continue;
      const double* p = ds->col(*v).f64();
      for (int64_t i = 0; i < ds->nRows(); ++i)
        if (p[i] > 180 || p[i] < -180) return fail(rcError(*v + " must be between -180 and 180", 198));
    }
  }

  // ---- the cutoff signals and the id/time rules (ado) --------------------
  const bool distYn = distcutoff >= 0;
  if (!distYn) distcutoff = 0;
  const bool lagYn = lagcutoff >= 0;
  if (!lagYn) lagcutoff = 0;
  const bool lagdistYn = lagdistcutoff >= 0;
  if (!lagdistYn) lagdistcutoff = 0;
  const bool hasId = !idv.empty(), hasTime = !timev.empty();
  if (!hasId) {
    if (hasTime) return fail(rcError("ID is required if Time dimension is specified", 498));
    if (lagYn) return fail(rcError("Lagcutoff requires Id and Time to be specified", 498));
    if (lagdistYn) return fail(rcError("Lagdistcutoff requires Id and Time to be specified", 498));
    if (hac) return fail(rcError("Hac option requires Id and Time to be specified", 498));
  } else {
    if (!hasTime) return fail(rcError("Time dimesion is required if id is specified", 498));
    if (!hac) {
      if (!lagYn) lagcutoff = 0;
      if (!lagdistYn) lagdistcutoff = 0;
    } else if (!lagYn) {
      return fail(rcError("Lagcutoff required with the HAC option", 498));
    }
  }

  // if/in: marksample over no varlist (ado) -- the rows the qualifier keeps.
  struct TempGuard {
    SessionState& s;
    bool active = false;
    ~TempGuard() {
      if (!active) return;
      try {
        SessionState::TempDropScope td(s);
        (void)piet::dispatch(std::string("capture drop ") + kTouse, s);
      } catch (...) {
      }
    }
  } guard{session};
  std::string qual;
  if (parsed.ifExpr.has_value()) qual += " if " + *parsed.ifExpr;
  if (parsed.inRange.has_value()) qual += " in " + *parsed.inRange;
  const bool restricted = !qual.empty();
  if (restricted) {
    guard.active = true;
    if (!e3::runLine(std::string("quietly generate double ") + kTouse + " = 1" + qual, session,
                     &err))
      return fail(err);
  }
  // ado: `sort id time`, before preserve -- the data stay sorted.
  if (hasId && !e3::runLine("quietly sort " + idv + " " + timev, session, &err)) return fail(err);

  // ---- the kernel's companion rules (ado; no cluster/weights/network) -----
  if (!spatial) {
    if (!latitude.empty() || !longitude.empty())
      return fail(rcError("Latitude and Longitude require Spatial Option to be specified", 498));
    if (distYn) return fail(rcError("Distance cutoff requires Network or Spatial Option", 498));
    if (lagdistYn) return fail(rcError("Lag Distance cutoff requires Network or Spatial Option", 498));
  } else {
    if (latitude.empty() || longitude.empty())
      return fail(rcError("Latitude and Longitude are required with the Spatial Option if "
                          "Distance Matrix is not specified",
                          498));
    if (!distYn)
      return fail(rcError(" Distance cutoff is required with the Spatial Option. Distcutoff must "
                          "be a nonnegative real number",
                          498));
  }
  // ado: pfe1 (factor variables refused: use xi:), pfe2 alone.
  if (!pfe1.empty()) {
    if (!factorTerms.empty())
      return fail(rcError("factor variables not allowed with pfe1 option, please specify xi: "
                          "before acreg",
                          198));
  } else if (!pfe2.empty()) {
    return fail(rcError("pfe2 requires pfe1. Please, specify pfe1 instead of pfe2 if you want to "
                        "partial out only one fixed effect.",
                        498));
  }
  if (!factorTerms.empty())
    return fail(rcError("acreg with factor-variable terms (" + joinWords(factorTerms) +
                            ") is not implemented in this version of PIET; use `xi:` before "
                            "acreg (the corpus form); nothing was estimated",
                        198, "unsupported_option"));

  // ---- the data, in (id, time) order --------------------------------------------
  Datastore* ds = session.datastore();
  const size_t Nall = static_cast<size_t>(ds->nRows());
  auto column = [&ds, Nall](const std::string& name) {
    const double* p = ds->col(name).f64();
    return std::vector<double>(p, p + Nall);
  };
  std::vector<char> touse(Nall, 1);
  if (restricted) {
    const std::vector<double> t = column(kTouse);
    for (size_t i = 0; i < Nall; ++i) touse[i] = t[i] == 1.0;
  }
  for (const std::string* v : {&idv, &timev, &pfe1, &pfe2})
    if (!v->empty() && ds->dtype(*v) != "f64")
      return fail(rcError("acreg: a string " + *v + "() is not implemented in this version of "
                              "PIET; encode it first",
                          198, "unsupported_option"));
  std::vector<double> yv = column(depvar);
  std::vector<std::vector<double>> endv, exogv, ivv;
  for (const auto& v : end1) endv.push_back(column(v));
  for (const auto& v : exog1) exogv.push_back(column(v));
  for (const auto& v : ivv1) ivv.push_back(column(v));
  std::vector<double> idc, timec, latc, lonc, fe1c, fe2c;
  if (hasId) {
    idc = column(idv);
    timec = column(timev);
  }
  if (spatial) {
    latc = column(latitude);
    lonc = column(longitude);
  }
  if (!pfe1.empty()) fe1c = column(pfe1);
  if (!pfe2.empty()) fe2c = column(pfe2);

  // The estimation sample: `reg depvar regs end iv if in; keep if e(sample)`
  // (ado); with pfe the hdfe sample (fe nonmissing too) is partialled
  // and the core runs on it (ado).
  std::vector<size_t> rows;
  for (size_t i = 0; i < Nall; ++i) {
    if (!touse[i] || std::isnan(yv[i])) continue;
    bool ok = true;
    for (const auto* g : {&endv, &exogv, &ivv})
      for (const auto& c : *g) ok = ok && !std::isnan(c[i]);
    if (!pfe1.empty()) ok = ok && !std::isnan(fe1c[i]);
    if (!pfe2.empty()) ok = ok && !std::isnan(fe2c[i]);
    if (hasId) ok = ok && !std::isnan(idc[i]);
    if (ok) rows.push_back(i);
  }
  const size_t N = rows.size();
  if (N == 0) return fail(rcError("no observations", 2000));
  auto sub = [&rows](const std::vector<double>& c) {
    std::vector<double> o(rows.size());
    for (size_t t = 0; t < rows.size(); ++t) o[t] = c[rows[t]];
    return o;
  };
  std::vector<double> Y = sub(yv);
  std::vector<std::vector<double>> En, Ex, Iv;
  for (const auto& c : endv) En.push_back(sub(c));
  for (const auto& c : exogv) Ex.push_back(sub(c));
  for (const auto& c : ivv) Iv.push_back(sub(c));
  if (!pfe1.empty()) {
    std::vector<std::vector<double>> all;
    all.push_back(Y);
    for (auto* g : {&En, &Ex, &Iv})
      for (auto& c : *g) all.push_back(c);
    const std::vector<double> f1 = sub(fe1c);
    std::vector<double> f2;
    if (!pfe2.empty()) f2 = sub(fe2c);
    absorbFixedEffects(all, f1, pfe2.empty() ? nullptr : &f2);
    size_t at = 0;
    Y = all[at++];
    for (auto* g : {&En, &Ex, &Iv})
      for (auto& c : *g) c = all[at++];
  }
  std::vector<double> idS(N), timeS(N, 1.0), latS, lonS;
  for (size_t t = 0; t < N; ++t) {
    idS[t] = hasId ? idc[rows[t]] : static_cast<double>(t + 1);
    if (hasId) timeS[t] = timec[rows[t]];
  }
  if (spatial) {
    latS = sub(latc);
    lonS = sub(lonc);
  }

  // ---- acreg_core (ado): b, P, residuals -------------------------------
  const std::vector<double> ones(N, 1.0);
  std::vector<std::vector<double>> Xc, Zc;  // columns
  for (const auto& c : En) Xc.push_back(c);
  for (const auto& c : Ex) Xc.push_back(c);
  Xc.push_back(ones);
  for (const auto& c : Iv) Zc.push_back(c);
  for (const auto& c : Ex) Zc.push_back(c);
  Zc.push_back(ones);
  const size_t K = Xc.size();
  const bool ols = En.empty() && Iv.empty();
  const Mat ZZin = e3::invsym(crossLD(Zc, Zc));
  const Mat ZY = crossLD(Zc, {Y});
  std::vector<double> b(K, 0.0);
  Mat P;  // K x N
  const Mat Zt = Zc;  // rows = columns of Z (each N long): Z' as a matrix
  if (ols) {
    const Mat bm = matmul(ZZin, ZY);
    for (size_t j = 0; j < K; ++j) b[j] = bm[j][0];
    P = matmul(ZZin, Zt);
  } else {
    const Mat XZ = crossLD(Xc, Zc);
    const Mat ZX = transpose(XZ);
    Mat XZZZZX = matmul(matmul(XZ, ZZin), ZX);
    for (size_t i = 0; i < K; ++i)
      for (size_t j = 0; j < i; ++j) XZZZZX[j][i] = XZZZZX[i][j];  // makesymmetric
    const Mat inv = e3::invsym(XZZZZX);
    const Mat bm = matmul(matmul(matmul(inv, XZ), ZZin), ZY);
    for (size_t j = 0; j < K; ++j) b[j] = bm[j][0];
    P = matmul(matmul(matmul(inv, XZ), ZZin), Zt);
  }
  std::vector<double> res(N);
  for (size_t t = 0; t < N; ++t) {
    double xb = 0;
    for (size_t j = 0; j < K; ++j) xb += Xc[j][t] * b[j];
    res[t] = Y[t] - xb;
  }

  // ---- the weights w_pos(j) and V = sum_pos P (res .* res[pos] .* w) P[.,pos]' -----
  // acreg_no_corr (ado), acreg_spat_ll_no_bart (ado),
  // acreg_spat_ll_bart (ado). Rows of one id are contiguous (sorted);
  // the reference point of id i is its FIRST row (ado).
  struct IdBlock {
    size_t s, e;  // [s, e)
  };
  std::vector<IdBlock> blocksById;
  {
    std::map<double, std::vector<size_t>> byId;
    for (size_t t = 0; t < N; ++t) byId[idS[t]].push_back(t);
    size_t ind = 0;
    for (const auto& [k, v] : byId) {
      blocksById.push_back({ind, ind + v.size()});
      ind += v.size();
    }
  }
  const bool isIv = !ols;
  // KP pieces (ado): first stage after partialling KX = [exog, 1].
  const bool kp = isIv && En.size() == 1 && Iv.size() == 1;
  std::vector<double> Zp, Yp, u1res;
  if (kp) {
    std::vector<std::vector<double>> KX = Ex;
    KX.push_back(ones);
    const Mat KXKXin = e3::invsym(crossLD(KX, KX));
    auto partial = [&](const std::vector<double>& v) {
      const Mat c = matmul(KXKXin, crossLD(KX, {v}));
      std::vector<double> o(N);
      for (size_t t = 0; t < N; ++t) {
        double s = 0;
        for (size_t j = 0; j < KX.size(); ++j) s += KX[j][t] * c[j][0];
        o[t] = v[t] - s;
      }
      return o;
    };
    Zp = partial(Iv[0]);
    Yp = partial(En[0]);
    long double zz = 0, zy = 0;
    for (size_t t = 0; t < N; ++t) {
      zz += static_cast<long double>(Zp[t]) * Zp[t];
      zy += static_cast<long double>(Zp[t]) * Yp[t];
    }
    const double pi1 = zz > 0 ? static_cast<double>(zy / zz) : 0.0;
    u1res.resize(N);
    for (size_t t = 0; t < N; ++t) u1res[t] = Yp[t] - Zp[t] * pi1;
  }
  Mat V(K, std::vector<double>(K, 0.0));
  long double avcv = 0;  // AVCVb_fs (1 x 1 here)
  const double PI = 3.14159265358979323846;
  std::vector<double> w(N, 0.0);
  std::vector<size_t> nz;
  nz.reserve(N);
  for (const auto& blk : blocksById) {
    std::vector<double> dist;
    if (spatial && distcutoff > 0) {
      const double lats = latS[blk.s], lons = lonS[blk.s];
      const double lonScale = std::cos(lats * PI / 180) * 111, latScale = 111;
      dist.resize(N);
      for (size_t j = 0; j < N; ++j) {
        const double a = latScale * (lats - latS[j]);
        const double c = lonScale * (lons - lonS[j]);
        dist[j] = std::pow(a * a + c * c, 0.5);
      }
    }
    for (size_t pos = blk.s; pos < blk.e; ++pos) {
      std::fill(w.begin(), w.end(), 0.0);
      if (spatial && distcutoff > 0) {
        for (size_t j = 0; j < N; ++j) {
          const double gap = std::fabs(timeS[pos] - timeS[j]);
          const double inTime = gap <= static_cast<double>(lagdistcutoff) ? 1.0 : 0.0;
          const double inDist = dist[j] <= distcutoff ? 1.0 : 0.0;
          double wt = inTime * inDist;
          if (hac) wt *= (1 - gap / (static_cast<double>(lagdistcutoff) + 1));
          if (bartlett) wt *= (1 - std::fabs(dist[j] / distcutoff));
          w[j] = wt;
        }
      }
      for (size_t j = blk.s; j < blk.e; ++j) {
        if (lagcutoff > 0) {
          const double gap = std::fabs(timeS[pos] - timeS[j]);
          const double L = static_cast<double>(lagcutoff);
          w[j] = hac ? (1 - gap / (L + 1)) * (gap <= L ? 1.0 : 0.0) : (gap <= L ? 1.0 : 0.0);
        } else {
          w[j] = timeS[j] == timeS[pos] ? 1.0 : 0.0;
        }
      }
      nz.clear();
      for (size_t j = 0; j < N; ++j)
        if (w[j] != 0) nz.push_back(j);
      // V1 = P * (res .* res[pos] * w) * P[.,pos]'
      std::vector<double> a(K, 0.0);
      for (size_t j : nz) {
        const double uj = res[j] * res[pos] * w[j];
        for (size_t r = 0; r < K; ++r) a[r] += P[r][j] * uj;
      }
      for (size_t r = 0; r < K; ++r)
        for (size_t c = 0; c < K; ++c) V[r][c] += a[r] * P[c][pos];
      if (kp) {
        long double s = 0;
        for (size_t j : nz) s += static_cast<long double>(Zp[j]) * u1res[j] * w[j];
        avcv += s * u1res[pos] * Zp[pos] / static_cast<long double>(N);
      }
    }
  }
  for (size_t i = 0; i < K; ++i)
    for (size_t j = i + 1; j < K; ++j) V[i][j] = V[j][i];  // _makesymmetric: lower -> upper

  // ---- the e() scalars (ado) ------------------------------------------------
  double rss = 0, tssu = 0, ymean = 0;
  for (size_t t = 0; t < N; ++t) {
    rss += res[t] * res[t];
    tssu += Y[t] * Y[t];
    ymean += Y[t];
  }
  ymean /= static_cast<double>(N);
  double tss = 0;
  for (size_t t = 0; t < N; ++t) tss += (Y[t] - ymean) * (Y[t] - ymean);
  const double r2 = 1 - rss / tss, r2u = 1 - rss / tssu;
  // Kleibergen-Paap rk Wald F (ado; acreg_s_rkstat, fullrank wald with
  // mat_clst = AVCVb_fs): for one endogenous regressor and one excluded
  // instrument rk = (zhat'yhat)^2 / (N * AVCVb_fs).
  double widstat = kNaN;
  bool widWarn = false;
  if (isIv) {
    if (kp) {
      long double zy = 0;
      for (size_t t = 0; t < N; ++t) zy += static_cast<long double>(Zp[t]) * Yp[t];
      const double chi2 = avcv > 0 ? static_cast<double>(zy * zy / (avcv * N)) : kNaN;
      const double ivCt = static_cast<double>(Iv.size() + Ex.size() + 1);
      const double sdofminus = static_cast<double>(Ex.size() + 1);
      const double nClust = nbclust < 0 ? 100.0 : static_cast<double>(nbclust);
      const double Nd = static_cast<double>(N);
      if (std::isfinite(chi2))
        widstat = chi2 / (Nd - 1) * (Nd - ivCt - sdofminus) * (nClust - 1) / nClust /
                  static_cast<double>(Iv.size());
      else
        widWarn = true;
    } else {
      widWarn = true;  // more than one endogenous regressor or instrument: not ported
    }
  }

  // ---- names and the posting (ado) --------------------------------------
  std::vector<std::string> names = end1;
  names.insert(names.end(), exog1.begin(), exog1.end());
  names.push_back("_cons");
  ojson block;
  block["type"] = "estimation";
  block["estimator"] = "acreg";
  block["depvar"] = depvar;
  block["n_obs"] = static_cast<long long>(N);
  block["coeftable"] = e3::coeftableFromPosting(names, b, V, kNaN);
  block["rss"] = rss;
  block["r2"] = r2;

  // The `sort id time` above is the only data change; the temporary qualifier
  // column leaves before e(sample) is installed on the working frame.
  if (restricted) {
    SessionState::TempDropScope td(session);
    if (!e3::runLine(std::string("quietly drop ") + kTouse, session, &err)) return fail(err);
    guard.active = false;
  }
  LastModel lm;
  lm.cobraCmd = "acreg";
  lm.stataCmd = "acreg";
  lm.y = depvar;
  lm.depvar = depvar;
  lm.x = names;
  lm.x.pop_back();
  lm.vcov = "";  // acreg posts no e(vce)/e(vcetype) (IDENTITY.md 1.1)
  lm.result = block;
  // e(sample) = the if/in rows (ado `ereturn repost, esample(touse)`), rows
  // dropped for a missing value included (log:3101 D02 e(N) 56, e(sample) 57).
  (void)e3::installEsampleMask(session, lm, touse);
  session.setLastModel(lm);
  session.lastEstBlock = block;
  e3::postBV(session, names, b, V);
  e3::setEScalarOnFit(session, "rss", rss);
  e3::setEScalarOnFit(session, "tss", tss);
  e3::setEScalarOnFit(session, "tssu", tssu);
  e3::setEScalarOnFit(session, "mss", tss - rss);
  e3::setEScalarOnFit(session, "mssu", tssu - rss);
  e3::setEScalarOnFit(session, "r2", r2);
  e3::setEScalarOnFit(session, "r2u", r2u);
  e3::setEScalarOnFit(session, "Fdf1", static_cast<double>(K) - 1);
  e3::setEScalarOnFit(session, "Fdf2", static_cast<double>(N) - static_cast<double>(K));
  if (isIv) e3::setEScalarOnFit(session, "widstat", widstat);
  if (!end1.empty()) blocks.push_back(displayBlockA("Instrumented: " + joinWords(end1)));
  if (widWarn)
    blocks.push_back(displayBlockA(
        "warning: -ranktest- error in calculating KP weak identification test statistics;\n"
        "may be caused by collinearities or the presence of factor variables or weights"));
  blocks.push_back(block);
  return blocks;
}

}  // namespace handlers
}  // namespace piet
