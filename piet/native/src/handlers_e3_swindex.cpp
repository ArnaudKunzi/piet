// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Portions derived from swindex SSC-2021-02-24 (GPL-3.0).
// See THIRD_PARTY_NOTICES.
//
// handlers_e3_swindex.cpp -- `swindex` (Schwab, Janzen, Magnan, Thompson; SSC s458912,
// SJ 20-4 st0622; GPL v3 per its SSC listing): the Anderson (2008)
// inverse-covariance-weighted index, derived from swindex.ado (the 273-line ado in
// the PLUS dir; ADO:line cites below) and scope-ge2/e3/swindex/IDENTITY.md.
// Oracle: w2j_e3_swindex.{do,log}.
//
// What the ado does, in its own order (each step names its line):
//   syntax varlist(min=1 numeric) [if] [in] [, Generate(name) replace FLip(varlist)
//     NORMby(varname) NOREscale FULLRescale NOStd Numvars(name) Displayw]   ADO:9
//   marksample touse, novarlist -- if/in only                              ADO:11
//   gsort -touse: the if/in sample moves to the top (ties keep their order
//     here; Stata's tie order is its sort jumbler's, never pinned)        ADO:12
//   components = varlist minus flip(), then -flip in REVERSED flip order   ADO:17-23
//   refusals: confirm new var (7/110), normby 0/1 (198), nostd+normby (198),
//     an all-missing component (198), normby collinear with the components
//     (_rmcollright, 198), a constant component (198)                     ADO:26-99
//   z = (x - m)/s over the control rows (normby==1) or all of touse, m/s/z in
//     c(type) temporaries (float unless `set type double`)                ADO:130-149
//   S = variance(Z) over the touse rows with every z nonmissing (casewise,
//     n-1), editmissing(S,0), w = rowsum(invsym(S))                       ADO:159-181
//   index = sum(z*float(w)) / sum(float(w)) over the nonmissing z; 0 -> .;
//     every row outside touse is missing                                  ADO:187-220
//   numvars() = rownonmiss(varlist)                                       ADO:223-225
//   pw = wt/sum(wt) (r(504) when the sum is 0, after the variable exists) ADO:229-233
//   rescale to mean 0 / sd 1 over the control rows (or touse; skipped by
//     norescale, and by nostd without fullrescale)                        ADO:243-250
// r(wt), r(pw) are returned; matrices wt sum pw aw and scalar s stay behind
// (w2j_e3_swindex.log:1828 X01).
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

#include "core/exact_sum.h"
#include "handlers_e3_common.h"
#include "piet/datastore.h"
#include "piet/eval/errors.h"
#include "piet/eval/pyvalue.h"
#include "piet/handlers.h"
#include "piet/loops.h"
#include "piet/mutation_result.h"
#include "piet/session_state.h"
#include "piet/strutil.h"
#include "vendor/nlohmann/json.hpp"

namespace piet {
namespace handlers {

namespace {

using e3::ojson;
using e3::rcError;
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

const char* const kTouse = "__swix_touse";
const char* const kVals = "__swix_vals";

// Stata's summarize over a row set: n, mean = exact sum / n, sd with divisor
// n-1 from the exactly accumulated squared deviations -- the engine's
// Frame::summarize rule, from the same core/exact_sum.h.
struct Summ {
  int64_t n = 0;
  double mean = kNaN;
  double sd = kNaN;
};
Summ summarize(const std::vector<double>& x, const std::vector<char>& in) {
  Summ s;
  cecon::ExactSum acc;
  for (size_t i = 0; i < x.size(); ++i) {
    if (!in[i] || std::isnan(x[i])) continue;
    ++s.n;
    acc.add(x[i]);
  }
  if (s.n == 0) return s;
  s.mean = acc.value() / static_cast<double>(s.n);
  if (s.n >= 2) {
    cecon::ExactSqDev sq(s.mean);
    for (size_t i = 0; i < x.size(); ++i)
      if (in[i] && !std::isnan(x[i])) sq.add(x[i]);
    s.sd = std::sqrt(sq.value() / static_cast<double>(s.n - 1));
  }
  return s;
}

// A Stata division: missing when either side is missing or the result is not finite.
double sdiv(double a, double b) {
  if (std::isnan(a) || std::isnan(b)) return kNaN;
  const double r = a / b;
  return std::isfinite(r) ? r : kNaN;
}

// A scalar of the same name is destroyed: scalars and matrices share one
// namespace ([P] matrix define, "Namespace").
void setUserMatrix(SessionState& session, const std::string& name, eval::EvalMatrix m) {
  session.eval.scalars.erase(name);
  session.eval.stringScalars.erase(name);
  for (auto& p : session.eval.matrices) {
    if (p.first == name) {
      p.second = std::move(m);
      return;
    }
  }
  session.eval.matrices.emplace_back(name, std::move(m));
}

// A k x 1 matrix as st_matrix() makes it: rows r1..rk, column c1.
eval::EvalMatrix columnMatrix(const std::vector<double>& v) {
  eval::EvalMatrix m;
  for (size_t i = 0; i < v.size(); ++i) {
    m.data.push_back({v[i]});
    m.rownames.push_back("r" + std::to_string(i + 1));
  }
  m.colnames = {"c1"};
  return m;
}

// Is `y` (on `rows`) a linear combination of a constant and the columns `xs`?
// _rmcollright keeps the earlier variables, so it drops the normby() variable
// (named last) exactly then (ADO:82-87).
bool collinearWithConstAnd(const std::vector<std::vector<double>>& xs,
                           const std::vector<double>& y, const std::vector<size_t>& rows) {
  const size_t n = rows.size();
  if (n == 0) return false;
  double ym = 0;
  for (size_t r : rows) ym += y[r];
  ym /= static_cast<double>(n);
  std::vector<double> res(n);
  double tss = 0;
  for (size_t t = 0; t < n; ++t) {
    res[t] = y[rows[t]] - ym;
    tss += res[t] * res[t];
  }
  if (!(tss > 0)) return true;  // constant: collinear with _cons
  std::vector<std::vector<double>> basis;
  for (const auto& x : xs) {
    std::vector<double> q(n);
    double xm = 0;
    for (size_t r : rows) xm += x[r];
    xm /= static_cast<double>(n);
    double xss = 0;
    for (size_t t = 0; t < n; ++t) {
      q[t] = x[rows[t]] - xm;
      xss += q[t] * q[t];
    }
    for (const auto& b : basis) {
      double dot = 0;
      for (size_t t = 0; t < n; ++t) dot += q[t] * b[t];
      for (size_t t = 0; t < n; ++t) q[t] -= dot * b[t];
    }
    double nq = 0;
    for (size_t t = 0; t < n; ++t) nq += q[t] * q[t];
    if (!(nq > 0) || !(nq > 1e-13 * xss)) continue;  // collinear among the components
    const double s = std::sqrt(nq);
    for (size_t t = 0; t < n; ++t) q[t] /= s;
    basis.push_back(std::move(q));
  }
  for (const auto& b : basis) {
    double dot = 0;
    for (size_t t = 0; t < n; ++t) dot += res[t] * b[t];
    for (size_t t = 0; t < n; ++t) res[t] -= dot * b[t];
  }
  double rss = 0;
  for (size_t t = 0; t < n; ++t) rss += res[t] * res[t];
  return !(rss > 1e-13 * tss);
}

}  // namespace

// Returns an ARRAY of blocks; the dispatcher pushes each element (never the
// array as one block -- the type_error.306 class).
nlohmann::ordered_json runSwindex(const eval::ParsedCommand& parsed, SessionState& session) {
  ojson blocks = ojson::array();
  auto fail = [&blocks](ojson e) {
    blocks.push_back(std::move(e));
    return blocks;
  };
  if (!session.hasData()) return fail(ojson(eval::noDataLoaded()));

  // ---- syntax (ADO:9): the varlist first, then the options -------------------
  if (parsed.varlist.empty()) return fail(rcError("varlist required", 100));
  std::vector<std::string> varlist;
  ojson err;
  {
    std::string spec;
    for (const auto& t : parsed.varlist) spec += (spec.empty() ? "" : " ") + t;
    if (!e3::expandVars(spec, session, &varlist, &err)) return fail(err);
  }
  {
    Datastore* ds = session.datastore();
    for (const auto& v : varlist) {
      if (ds == nullptr || !ds->has(v)) return fail(rcError("variable " + v + " not found", 111));
      if (ds->dtype(v) != "f64") return fail(rcError("type mismatch", 109));
    }
  }
  static const std::vector<e3::OptSpec> kSpecs = {
      {"generate", 1, true},  {"replace", 7, false},   {"flip", 2, true},
      {"normby", 4, true},    {"norescale", 4, false}, {"fullrescale", 5, false},
      {"nostd", 3, false},    {"numvars", 1, true},    {"displayw", 1, false}};
  std::string generate, normby, numvars;
  std::vector<std::string> flip;
  bool replace = false, norescale = false, fullrescale = false, nostd = false, displayw = false;
  for (const auto& [k, v] : parsed.opts) {
    const int idx = e3::matchOpt(kSpecs, k);
    if (idx < 0) return fail(rcError("option " + k + " not allowed", 198));
    const e3::OptSpec& s = kSpecs[static_cast<size_t>(idx)];
    if (s.takesArg && v.isBool) return fail(rcError("option " + s.full + "() required", 198));
    if (!s.takesArg && !v.isBool) return fail(rcError("option " + s.full + " not allowed", 198));
    const std::string arg = strutil::trim(v.strValue);
    if (s.full == "generate") {
      if (!strutil::isValidStataIdentifier(arg)) return fail(rcError(arg + " invalid name", 198));
      generate = arg;
    } else if (s.full == "replace") {
      replace = true;
    } else if (s.full == "flip") {
      if (!e3::expandVars(arg, session, &flip, &err)) return fail(err);
    } else if (s.full == "normby") {
      if (arg.empty()) return fail(rcError("option normby() required", 198));
      std::vector<std::string> nbv;
      if (!e3::expandVars(arg, session, &nbv, &err)) return fail(err);
      if (nbv.size() != 1) return fail(rcError("too many variables specified", 103));
      normby = nbv[0];
    } else if (s.full == "norescale") {
      norescale = true;
    } else if (s.full == "fullrescale") {
      fullrescale = true;
    } else if (s.full == "nostd") {
      nostd = true;
    } else if (s.full == "numvars") {
      if (!strutil::isValidStataIdentifier(arg)) return fail(rcError(arg + " invalid name", 198));
      numvars = arg;
    } else if (s.full == "displayw") {
      displayw = true;
    }
  }
  {
    Datastore* ds = session.datastore();
    for (const auto& v : flip)
      if (ds->dtype(v) != "f64") return fail(rcError("type mismatch", 109));
  }

  // The helper columns leave the frame whatever happens below.
  struct TempGuard {
    SessionState& s;
    std::vector<std::string> names;
    ~TempGuard() {
      if (names.empty()) return;
      std::string line = "capture drop";
      for (const auto& n : names) line += " " + n;
      try {
        SessionState::TempDropScope td(s);
        (void)piet::dispatch(line, s);
      } catch (...) {
      }
    }
  } guard{session, {}};

  // ---- marksample touse, novarlist (ADO:11); gsort -touse (ADO:12) ------------
  std::string qual;
  if (parsed.ifExpr.has_value()) qual += " if " + *parsed.ifExpr;
  if (parsed.inRange.has_value()) qual += " in " + *parsed.inRange;
  const bool restricted = !qual.empty();
  if (restricted) {
    guard.names.push_back(kTouse);
    if (!e3::runLine(std::string("quietly generate double ") + kTouse + " = 1" + qual, session,
                     &err))
      return fail(err);
    // A partition only: the if/in rows go first, every other order is kept.
    Datastore* ds = session.datastore();
    const double* t = ds->col(kTouse).f64();
    bool any = false, none = false;
    for (int64_t i = 0; i < ds->nRows(); ++i) (t[i] == 1.0 ? any : none) = true;
    if (any && none && !e3::runLine(std::string("quietly gsort -") + kTouse, session, &err))
      return fail(err);
  }

  // ---- read the frame -----------------------------------------------------------
  Datastore* ds = session.datastore();
  const size_t N = static_cast<size_t>(ds->nRows());
  std::vector<char> touse(N, 1);
  if (restricted) {
    const double* t = ds->col(kTouse).f64();
    for (size_t i = 0; i < N; ++i) touse[i] = t[i] == 1.0;
  }
  auto column = [&ds, N](const std::string& name) {
    const double* p = ds->col(name).f64();
    return std::vector<double>(p, p + N);
  };
  std::map<std::string, std::vector<double>> data;
  for (const auto& v : varlist) data[v] = column(v);
  for (const auto& v : flip) data[v] = column(v);
  std::vector<double> nb;
  if (!normby.empty()) {
    if (ds->dtype(normby) != "f64") return fail(rcError("type mismatch", 109));
    nb = column(normby);
  }
  const bool genExists = !generate.empty() && ds->has(generate);

  // ---- ADO:26-35: generate()/replace ----------------------------------------------
  if (!replace && generate.empty())
    blocks.push_back(e3::displayText("Error: You must specify either generate or replace"));
  if (!replace) {
    if (generate.empty()) return fail(rcError("nothing found where name expected", 7));
    if (genExists) return fail(rcError("variable " + generate + " already defined", 110));
  }
  // ADO:45-49: normby() is 0/1/. on touse.
  if (!normby.empty()) {
    for (size_t i = 0; i < N; ++i)
      if (touse[i] && !std::isnan(nb[i]) && nb[i] != 0.0 && nb[i] != 1.0)
        return fail(rcError("Control variable is not binary", 198));
  }
  // ADO:54-62: nostd.
  if (nostd) {
    if (!normby.empty())
      return fail(rcError("You cannot specify nostd if normby is also specified", 198));
    blocks.push_back(e3::displayText(
        "Warning: The nostd option is not recommended, as the components of varlist are not "
        "normalized prior to calculation of the index.  Proceed with caution."));
  }
  // ADO:68-78: a component with no observation on touse (only the last one is
  // named: the ado's own `mvs`/`bvs` slip, IDENTITY.md section 6 item 7).
  {
    std::string mvs;
    for (const auto& v : varlist)
      if (summarize(data[v], touse).n == 0) mvs = v;
    if (!mvs.empty())
      return fail(rcError("Warning: The standardized index cannot be calculated because the "
                          "following variables have all missing values: " + mvs,
                          198));
  }
  if (!nostd) {
    // ADO:81-88: _rmcollright varlist normby if touse (rows complete on all of them).
    if (!normby.empty()) {
      std::vector<size_t> rows;
      for (size_t i = 0; i < N; ++i) {
        if (!touse[i] || std::isnan(nb[i])) continue;
        bool ok = true;
        for (const auto& v : varlist) ok = ok && !std::isnan(data[v][i]);
        if (ok) rows.push_back(i);
      }
      std::vector<std::vector<double>> xs;
      for (const auto& v : varlist) xs.push_back(data[v]);
      if (collinearWithConstAnd(xs, nb, rows))
        return fail(rcError("Warning: The standardized index cannot be calculated because one "
                            "or more of the variables in varlist is perfectly collinear with the "
                            "normby() standardization variable",
                            198));
    }
    // ADO:89-99: a constant component on touse.
    std::string bvs;
    for (const auto& v : varlist)
      if (summarize(data[v], touse).sd == 0.0) bvs += (bvs.empty() ? "" : " ") + v;
    if (!bvs.empty())
      return fail(rcError("Warning: The standardized index cannot be calculated because the "
                          "following variables are constant: " + bvs,
                          198));
  }
  if (fullrescale)
    blocks.push_back(e3::displayText(
        "Warning: Your index is normalized to the full sample even though the weighting "
        "procedure standardized the outcomes based on a subsample."));

  // ---- the components (ADO:17-23) ------------------------------------------------
  std::vector<std::vector<double>> comp;
  {
    const std::set<std::string> fl(flip.begin(), flip.end());
    for (const auto& v : varlist)
      if (!fl.count(v)) comp.push_back(data[v]);
    for (auto it = flip.rbegin(); it != flip.rend(); ++it) {
      std::vector<double> neg = data[*it];
      for (double& x : neg)
        if (!std::isnan(x)) x = -x;
      comp.push_back(std::move(neg));
    }
  }
  const size_t k = comp.size();
  // c(type) temporaries (ADO:132-134, 151, 165, 202): float unless `set type double`
  // (w2j_e3_swindex.log:1682 T03 differs from V02 by exactly that).
  const bool tempFloat = session.defaultStorageType != "double";
  auto T = [tempFloat](double v) { return tempFloat ? e3::toFloat(v) : v; };

  // ---- STEP 1 (ADO:130-149): z ------------------------------------------------------
  std::vector<char> ctl = touse;
  if (!normby.empty())
    for (size_t i = 0; i < N; ++i) ctl[i] = touse[i] && nb[i] == 1.0;
  std::vector<std::vector<double>> z(k, std::vector<double>(N, kNaN));
  for (size_t j = 0; j < k; ++j) {
    if (!nostd) {
      const Summ s = summarize(comp[j], ctl);
      const double m = T(s.mean), sd = T(s.sd);
      for (size_t i = 0; i < N; ++i)
        if (touse[i]) z[j][i] = T(sdiv(comp[j][i] - m, sd));
    } else {
      for (size_t i = 0; i < N; ++i)
        if (touse[i]) z[j][i] = T(comp[j][i]);
    }
  }

  // ---- STEP 2 (ADO:159-181): variance(), editmissing(), invsym(), rowsum() -----------
  std::vector<std::vector<double>> S(k, std::vector<double>(k, 0.0));
  {
    std::vector<size_t> rows;
    for (size_t i = 0; i < N; ++i) {
      if (!touse[i]) continue;
      bool ok = true;
      for (size_t j = 0; j < k; ++j) ok = ok && !std::isnan(z[j][i]);
      if (ok) rows.push_back(i);
    }
    const size_t nc = rows.size();
    if (nc >= 2) {  // otherwise every cell is missing, and editmissing() makes it 0
      std::vector<double> mean(k, 0.0);
      for (size_t j = 0; j < k; ++j) {
        cecon::ExactSum acc;
        for (size_t r : rows) acc.add(z[j][r]);
        mean[j] = acc.value() / static_cast<double>(nc);
      }
      for (size_t a = 0; a < k; ++a)
        for (size_t b = a; b < k; ++b) {
          double s = 0;
          for (size_t r : rows) s += (z[a][r] - mean[a]) * (z[b][r] - mean[b]);
          S[a][b] = S[b][a] = s / static_cast<double>(nc - 1);
        }
    }
  }
  const auto Sinv = e3::invsym(S);  // Mata invsym (handlers_e3_common.h)
  std::vector<double> w(k, 0.0);
  for (size_t a = 0; a < k; ++a)
    for (size_t b = 0; b < k; ++b) w[a] += Sinv[a][b];

  // ---- STEP 3 (ADO:187-220): the index --------------------------------------------
  // A new variable is double (ADO:190, 198); `replace` keeps an existing
  // variable's storage, and a byte/int is promoted to float by its first
  // non-integer value (w2j_e3_swindex.log:1351 P02 / :1410 P03 `float`).
  if (replace && generate.empty()) return fail(rcError("varlist required", 100));  // ADO:194
  std::string targetType = "double";
  if (replace && genExists) {
    if (ds->dtype(generate) != "f64") return fail(rcError("type mismatch", 109));  // R12
    session.syncEvalVarLabels();
    const std::string* ty =
        session.eval.varLabels.find(std::string(SessionState::kEvalVarTypePrefix) + generate);
    targetType = ty != nullptr ? *ty : std::string("float");
  }
  const bool targetFloat = targetType == "float" || targetType == "byte" || targetType == "int";
  auto TG = [targetFloat](double v) { return targetFloat ? e3::toFloat(v) : v; };
  std::vector<double> fw(k);
  for (size_t j = 0; j < k; ++j) fw[j] = T(w[j]);
  std::vector<double> g(N, kNaN);
  for (size_t i = 0; i < N; ++i) {
    if (!touse[i]) continue;
    double G = 0.0, SW = 0.0;
    for (size_t j = 0; j < k; ++j) {
      if (std::isnan(z[j][i])) continue;
      SW = SW + fw[j];
      G = TG(G + z[j][i] * fw[j]);
    }
    G = TG(sdiv(G, SW));
    if (G == 0.0) G = kNaN;
    g[i] = G;
  }

  // ---- the scaled weights (ADO:229-233) --------------------------------------------
  double wsum = 0;
  for (double x : w) wsum += x;
  const bool pwMissing = !std::isfinite(wsum) || wsum == 0.0;
  std::vector<double> pw(k);
  for (size_t j = 0; j < k; ++j) pw[j] = sdiv(w[j], wsum);

  // ---- STEP 4 (ADO:243-250): rescale --------------------------------------------------
  const bool nz = nostd && !fullrescale;
  if (!pwMissing && !norescale && !nz) {
    std::vector<char> R = touse;
    if (!normby.empty() && !fullrescale)
      for (size_t i = 0; i < N; ++i) R[i] = touse[i] && nb[i] == 1.0;
    const Summ s = summarize(g, R);
    for (size_t i = 0; i < N; ++i)
      if (touse[i]) g[i] = TG(sdiv(g[i] - s.mean, s.sd));
  }

  // ---- write the variable --------------------------------------------------------------
  guard.names.push_back(kVals);
  if (!e3::writeColumn(session, kVals, g, &err)) return fail(err);
  if (replace && !genExists)
    blocks.push_back(
        e3::displayText("Note: " + generate + " not found in existing data, so has been created"));
  if (!genExists) {
    if (!e3::runLine("quietly generate double " + generate + " = " + kVals, session, &err))
      return fail(err);
  } else if (!e3::runLine("quietly replace " + generate + " = " + kVals, session, &err)) {
    return fail(err);
  }
  // ADO:223-225: numvars() = the nonmissing count of varlist on touse.
  if (!numvars.empty()) {
    std::string vl;
    for (const auto& v : varlist) vl += " " + v;
    std::string line = "quietly egen " + numvars + " = rownonmiss(" + vl + ")";
    if (restricted) line += std::string(" if ") + kTouse + " == 1";
    if (!e3::runLine(line, session, &err)) return fail(err);
  }

  // wt (the copy of WT, ADO:180), sum and s (ADO:229-230) stay behind.
  setUserMatrix(session, "wt", columnMatrix(w));
  {
    eval::EvalMatrix sm;
    sm.data = {{wsum}};
    sm.rownames = {"r1"};
    sm.colnames = {"c1"};
    setUserMatrix(session, "sum", sm);
    session.eval.scalars.set("s", eval::pythonFloatRepr(wsum));
  }
  if (pwMissing) {
    // `mat prow = wt/s` with s = 0 is Stata's r(504), after the variable exists
    // (w2j_e3_swindex.log:732 V11).
    return fail(rcError("matrix has missing values", 504));
  }
  setUserMatrix(session, "pw", columnMatrix(pw));
  if (varlist.size() != w.size()) return fail(rcError("conformability error", 503));
  {
    eval::EvalMatrix aw;
    aw.data.push_back(w);
    aw.rownames = {"Weights"};
    aw.colnames = varlist;
    setUserMatrix(session, "aw", aw);
  }
  if (displayw) {
    // ADO:256-268: pw[i] printed against the i-th NAME of varlist (with flip()
    // the labels do not follow the weights; IDENTITY.md section 6 item 6).
    std::string text = "Weights\n";
    for (size_t i = 0; i < varlist.size() && i < pw.size(); ++i) {
      const std::string nm = varlist[i].size() > 15 ? varlist[i].substr(0, 15) : varlist[i];
      char buf[96];
      std::snprintf(buf, sizeof buf, "%12s       %.9g\n", nm.c_str(), pw[i]);
      text += buf;
      session.eval.scalars.set("vw" + std::to_string(i + 1), eval::pythonFloatRepr(pw[i]));
    }
    blocks.push_back(e3::displayText(text));
  }

  ojson mut = mutationResult(session, "swindex",
                             genExists ? std::vector<std::string>{}
                                       : std::vector<std::string>{generate});
  // r() last: an rclass program's return set is what r() holds when it ends
  // (r(wt), r(pw); w2j_e3_swindex.log:1828 X01 `4|1|4|1|`).
  eval::replaceRClass(session.eval);
  {
    eval::EvalMatrix rw = columnMatrix(w);
    rw.gen = eval::nextStoreGeneration();
    session.eval.rMatrices.emplace_back("wt", std::move(rw));
    eval::EvalMatrix rp = columnMatrix(pw);
    rp.gen = eval::nextStoreGeneration();
    session.eval.rMatrices.emplace_back("pw", std::move(rp));
  }
  blocks.push_back(mut);
  return blocks;
}

}  // namespace handlers
}  // namespace piet
