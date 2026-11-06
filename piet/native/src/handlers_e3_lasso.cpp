// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Portions derived from lassopack 1.3 (rlasso.ado / lassoutils.ado) (GPL-3.0).
// See THIRD_PARTY_NOTICES.
//
// handlers_e3_lasso.cpp -- `rlasso` (lassopack 1.3: rlasso.ado 1.0.10 + lassoutils.ado
// 1.1.03, the copy package 077 ships) and Stata's own `lasso`.
//
// rlasso: a native port of the default (post-lasso penalty loadings, lasso
// coefficients) path, derived from the Mata in lassoutils.ado (L:line) and the
// ado bookkeeping in rlasso.ado (R:line); scope-ge2/e3/rlasso/METHOD.md is the map.
// Upstream: lassopack, Ahrens, Hansen & Schaffer, GPL v3 per its SSC listing
// (SSC s458458; the package header there reads 1.4.3, rlasso 1.0.11 29jul2020).
// Oracle: w2j_e3_rlasso.{do,log}.
//   _rlasso syntax varlist(numeric fv ts min=2) (R:86-108): 102/109/111
//   fe needs xtset (459, R:141-148); pnotpen()/partial() outside the regressors,
//     the xtset variable in an fe model, partial(_cons)+fe, fe+noconstant: 198
//     (R:265-308)
//   fvexpand + duplicates dropped (R:241-248); pminus = the base/omitted model
//     columns (R:373-391); p = p0 - pminus
//   fe: within transform over the xtset panel variable (lassoutils _fe), the data
//     are then zero-mean, no constant (R:179-193, :416-430)
//   MakeData (L:1687-1830), RLasso (L:2373-2536): InitialResiduals on the 5
//     regressors most correlated with y (L:2238-2330), MakeScoreStdVec
//     (L:2663-2745: homoskedastic sd*rmse, robust, cluster sums / n), lambdaCalc
//     2c sqrt(n) invnormal(1-gamma/(2p)) with gamma = 0.1/ln(n or nclust)
//     (L:2188-2236, :2410-2416), DoLasso's start luqrsolve(XX + lambda/2 diag(Psi))
//     and cyclic shooting to sum|db| < 1e-10 (L:3197-3480), Psi iterated maxpsiiter
//     times on the post-lasso residuals; ReturnResults (L:3880-3990)
//   e(b) = the selected lasso coefficients + _cons (none and no e(b) at all under fe
//     when nothing is selected), e() scalars/macros/matrices (R:567-835)
// Not ported (refused by name): weights, sqrt, xdep, lalternative, supscore/testonly,
// pnotpen()/partial() that pass the checks, prestd, ols/postall, lassopsi, center,
// nclust1, sigma(), lambda0(), corrnumber(), tolerances, verbose.
//
// Built-in linear/Poisson lasso: plugin/BIC/none use their own standardized
// objective and selection logic below; CV (linear/Poisson) and adaptive CV
// draw their folds from the session's Stata mt64 stream. Other models remain
// explicit refusals.
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "core/distributions.h"
#include "core/rng.h"
#include "piet/storage_type.h"
#include "piet/mutation_result.h"
#include "piet/preflight.h"
#include "handlers_e3_common.h"
#include "piet/datastore.h"
#include "piet/eval/errors.h"
#include "piet/eval/pyvalue.h"
#include "piet/handlers.h"
#include "piet/session_state.h"
#include "piet/strutil.h"
#include "vendor/nlohmann/json.hpp"

namespace piet {
namespace handlers {

namespace {

using e3::ojson;
using e3::rcError;
using Vec = std::vector<double>;
using Mat = std::vector<std::vector<double>>;
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

double qmean(const Vec& v) {
  long double s = 0;
  for (double x : v) s += x;
  return v.empty() ? kNaN : static_cast<double>(s / static_cast<long double>(v.size()));
}

// Least squares by Householder QR (Mata qrsolve); columns given as vectors.
// A column that adds no rank gets coefficient 0.
Vec qrsolve(const std::vector<Vec>& A, const Vec& b) {
  const size_t k = A.size();
  Vec out(k, 0.0);
  if (k == 0) return out;
  const size_t n = b.size();
  std::vector<Vec> R = A;  // columns
  Vec y = b;
  std::vector<size_t> perm(k);
  std::iota(perm.begin(), perm.end(), 0);
  std::vector<double> diag(k, 0.0);
  size_t rank = 0;
  double maxNorm = 0;
  for (size_t j = 0; j < k; ++j) {
    double s = 0;
    for (double x : R[j]) s += x * x;
    maxNorm = std::max(maxNorm, std::sqrt(s));
  }
  for (size_t j = 0; j < k && j < n; ++j) {
    // column pivoting: the remaining column with the largest norm below row j
    size_t best = j;
    double bestN = -1;
    for (size_t c = j; c < k; ++c) {
      double s = 0;
      for (size_t i = j; i < n; ++i) s += R[c][i] * R[c][i];
      if (s > bestN) {
        bestN = s;
        best = c;
      }
    }
    if (!(std::sqrt(bestN) > 1e-12 * maxNorm) || bestN <= 0) break;
    std::swap(R[j], R[best]);
    std::swap(perm[j], perm[best]);
    const double alpha = (R[j][j] > 0 ? -1.0 : 1.0) * std::sqrt(bestN);
    Vec v(n, 0.0);
    for (size_t i = j; i < n; ++i) v[i] = R[j][i];
    v[j] -= alpha;
    double vn = 0;
    for (size_t i = j; i < n; ++i) vn += v[i] * v[i];
    if (vn > 0) {
      for (size_t c = j; c < k; ++c) {
        double d = 0;
        for (size_t i = j; i < n; ++i) d += v[i] * R[c][i];
        const double f = 2 * d / vn;
        for (size_t i = j; i < n; ++i) R[c][i] -= f * v[i];
      }
      double d = 0;
      for (size_t i = j; i < n; ++i) d += v[i] * y[i];
      const double f = 2 * d / vn;
      for (size_t i = j; i < n; ++i) y[i] -= f * v[i];
    }
    diag[j] = R[j][j];
    ++rank;
  }
  Vec x(rank, 0.0);
  for (size_t jj = rank; jj-- > 0;) {
    double s = y[jj];
    for (size_t c = jj + 1; c < rank; ++c) s -= R[c][jj] * x[c];
    x[jj] = s / R[jj][jj];
  }
  for (size_t jj = 0; jj < rank; ++jj) out[perm[jj]] = x[jj];
  return out;
}

// Mata luqrsolve(A, b): LU with partial pivoting; a singular A falls back to QR.
Vec luqrsolve(Mat A, Vec b) {
  const size_t k = A.size();
  std::vector<size_t> piv(k);
  bool singular = false;
  Mat M = A;
  Vec y = b;
  for (size_t c = 0; c < k; ++c) {
    size_t p = c;
    for (size_t r = c + 1; r < k; ++r)
      if (std::fabs(M[r][c]) > std::fabs(M[p][c])) p = r;
    if (M[p][c] == 0) {
      singular = true;
      break;
    }
    std::swap(M[c], M[p]);
    std::swap(y[c], y[p]);
    for (size_t r = c + 1; r < k; ++r) {
      const double f = M[r][c] / M[c][c];
      if (f == 0) continue;
      for (size_t j = c; j < k; ++j) M[r][j] -= f * M[c][j];
      y[r] -= f * y[c];
    }
  }
  if (!singular) {
    // a pivot tiny relative to the matrix is LU's singularity too (luinv's rule)
    double amax = 0;
    for (const auto& r : A)
      for (double x : r) amax = std::max(amax, std::fabs(x));
    for (size_t c = 0; c < k; ++c)
      if (std::fabs(M[c][c]) <= 1e-13 * amax) singular = true;
  }
  if (singular) {
    std::vector<Vec> cols(k, Vec(k));
    for (size_t r = 0; r < k; ++r)
      for (size_t c = 0; c < k; ++c) cols[c][r] = A[r][c];
    return qrsolve(cols, b);
  }
  Vec x(k, 0.0);
  for (size_t r = k; r-- > 0;) {
    double s = y[r];
    for (size_t j = r + 1; j < k; ++j) s -= M[r][j] * x[j];
    x[r] = s / M[r][r];
  }
  return x;
}

struct RlData {
  size_t n = 0, p = 0;
  std::vector<Vec> X;  // columns
  Vec y;
  bool cons = true;    // d.cons
  Vec mvec;
  double ymvec = 0;
  Vec sdvec;
  double ysd = 0;
  Mat XX;
  Vec Xy;
  std::vector<size_t> cl;  // cluster index per row (sorted groups), empty if none
  size_t nclust = 0;
};

// MakeData (L:1687-1830) without unpenalised regressors.
void makeData(RlData& d, bool dmflag) {
  d.n = d.y.size();
  d.p = d.X.size();
  d.mvec.assign(d.p, 0.0);
  d.ymvec = 0;
  if (!dmflag) {
    for (size_t j = 0; j < d.p; ++j) d.mvec[j] = qmean(d.X[j]);
    d.ymvec = qmean(d.y);
  }
  auto msq = [](const Vec& v, double m) {
    long double s = 0;
    for (double x : v) s += static_cast<long double>(x - m) * (x - m);
    return static_cast<double>(s / static_cast<long double>(v.size()));
  };
  d.ysd = std::sqrt(msq(d.y, d.ymvec));
  d.sdvec.assign(d.p, 0.0);
  for (size_t j = 0; j < d.p; ++j) d.sdvec[j] = std::sqrt(msq(d.X[j], d.mvec[j]));
  const bool dev = d.cons;
  d.XX.assign(d.p, Vec(d.p, 0.0));
  d.Xy.assign(d.p, 0.0);
  for (size_t a = 0; a < d.p; ++a) {
    const double ma = dev ? d.mvec[a] : 0.0;
    for (size_t b = a; b < d.p; ++b) {
      const double mb = dev ? d.mvec[b] : 0.0;
      long double s = 0;
      for (size_t i = 0; i < d.n; ++i)
        s += static_cast<long double>(d.X[a][i] - ma) * (d.X[b][i] - mb);
      d.XX[a][b] = d.XX[b][a] = static_cast<double>(s);
    }
    long double s = 0;
    const double my = dev ? d.ymvec : 0.0;
    for (size_t i = 0; i < d.n; ++i) s += static_cast<long double>(d.X[a][i] - ma) * (d.y[i] - my);
    d.Xy[a] = static_cast<double>(s);
  }
}

// centerpartial with nothing partialled out
Vec centered(const Vec& v, double m, bool cons) {
  Vec o(v);
  if (cons)
    for (double& x : o) x -= m;
  return o;
}

// InitialResiduals (L:2238-2330), corrnumber regressors by |m_quadcorr|.
Vec initialResiduals(const RlData& d, int corrnumber) {
  const int k = std::min(corrnumber, static_cast<int>(d.p));
  const Vec yc = centered(d.y, d.ymvec, d.cons);
  if (k <= 0) return yc;
  // m_quadcorr(.) of (yc, Xc): means taken again (cons), then crossdev
  std::vector<Vec> cols;
  cols.push_back(yc);
  for (size_t j = 0; j < d.p; ++j) cols.push_back(centered(d.X[j], d.mvec[j], d.cons));
  std::vector<double> mm(cols.size(), 0.0);
  if (d.cons)
    for (size_t j = 0; j < cols.size(); ++j) mm[j] = qmean(cols[j]);
  auto cross = [&](size_t a, size_t b) {
    long double s = 0;
    for (size_t i = 0; i < d.n; ++i)
      s += static_cast<long double>(cols[a][i] - mm[a]) * (cols[b][i] - mm[b]);
    return static_cast<double>(s);
  };
  const double syy = std::sqrt(cross(0, 0));
  std::vector<double> z(d.p);
  for (size_t j = 0; j < d.p; ++j) {
    const double sjj = std::sqrt(cross(j + 1, j + 1));
    const double r = cross(0, j + 1) / (syy * sjj);
    z[j] = std::isfinite(r) ? std::fabs(r) : kNaN;
  }
  std::vector<size_t> ix(d.p);
  std::iota(ix.begin(), ix.end(), 0);
  std::stable_sort(ix.begin(), ix.end(), [&z](size_t a, size_t b) {
    // order(-z, 1): missing sorts last
    const bool ma = std::isnan(z[a]), mb = std::isnan(z[b]);
    if (ma || mb) return !ma && mb;
    return -z[a] < -z[b];
  });
  ix.resize(static_cast<size_t>(k));
  std::vector<Vec> A;
  for (size_t j : ix) A.push_back(centered(d.X[j], d.mvec[j], d.cons));
  const Vec bb = qrsolve(A, yc);
  Vec r = yc;
  for (size_t t = 0; t < ix.size(); ++t)
    for (size_t i = 0; i < d.n; ++i) r[i] -= A[t][i] * bb[t];
  return r;
}

// MakeScoreStdVec (L:2663-2745), center = nclust1 = 0.
Vec scoreStd(const RlData& d, const Vec& v, double rmse, bool hetero) {
  Vec out(d.p, 0.0);
  if (d.nclust == 0) {
    if (!hetero) {
      for (size_t j = 0; j < d.p; ++j) out[j] = d.sdvec[j] * rmse;
    } else {
      for (size_t j = 0; j < d.p; ++j) {
        long double s = 0;
        for (size_t i = 0; i < d.n; ++i) {
          const long double t = static_cast<long double>((d.cons ? d.X[j][i] - d.mvec[j] : d.X[j][i])) * v[i];
          s += t * t;
        }
        out[j] = std::sqrt(static_cast<double>(s) / static_cast<double>(d.n));
      }
    }
    return out;
  }
  for (size_t j = 0; j < d.p; ++j) {
    std::vector<long double> g(d.nclust, 0);
    for (size_t i = 0; i < d.n; ++i)
      g[d.cl[i]] += static_cast<long double>((d.cons ? d.X[j][i] - d.mvec[j] : d.X[j][i])) * v[i];
    long double s = 0;
    for (long double x : g) s += x * x;
    out[j] = std::sqrt(static_cast<double>(s) / static_cast<double>(d.n));
  }
  return out;
}

double lambdaCalc(const RlData& d, int pminus, double gamma, double c) {
  const double p = static_cast<double>(d.p) - pminus;
  return 2 * c * std::sqrt(static_cast<double>(d.n)) * cecon::stata_invnormal(1 - gamma / (2 * p));
}

struct LassoOut {
  Vec betaAll, betaAllPL, beta, betaPL;
  std::vector<char> index;
  size_t s = 0;
  long long niter = 0;
  Vec v, vPL;
  double rmse = 0, rmsePL = 0, intercept = 0, interceptPL = 0, objfn = 0;
  Vec Psi;
  double lambda = 0;
  double loglik = kNaN, loglikPL = kNaN;  // nonlinear likelihoods
};

// DoLasso (L:3197-3480), alpha = 1.
std::pair<Vec,long long> linearCoordinates(const RlData& d, const Vec& Psi, double lambda,
                                              double optTol, long long maxIter,
                                              const Vec* initial, bool relative) {
  const size_t p = d.p;
  Mat A = d.XX;
  for (size_t j = 0; j < p; ++j) A[j][j] += lambda / 2 * Psi[j];
  Vec beta = initial ? *initial : luqrsolve(A, d.Xy);
  Mat XX2 = d.XX;
  for (auto& r : XX2)
    for (double& x : r) x *= 2;
  Vec Xy2 = d.Xy;
  for (double& x : Xy2) x *= 2;
  long long m = 0;
  while (m < maxIter) {
    const Vec old = beta;
    for (size_t j = 0; j < p; ++j) {
      double dot = 0;
      for (size_t q = 0; q < p; ++q) dot += XX2[j][q] * beta[q];
      const double S0 = dot - XX2[j][j] * beta[j] - Xy2[j];
      const double lp = lambda * Psi[j];
      if (S0 > lp) beta[j] = (lp - S0) / XX2[j][j];
      else if (S0 < -lp) beta[j] = (-lp - S0) / XX2[j][j];
      else beta[j] = 0;
    }
    ++m;
    long double ch = 0;
    for (size_t j = 0; j < p; ++j) {
      const double change = std::fabs(beta[j] - old[j]);
      if (relative) ch = std::max(ch, static_cast<long double>(change / (std::fabs(beta[j]) + 1)));
      else ch += change;
    }
    if (ch < optTol) break;
  }
  return {std::move(beta), m};
}

LassoOut doLasso(const RlData& d, const Vec& Psi, double lambda, double optTol, long long maxIter,
                 double zeroTol, const Vec* initial = nullptr, bool relative = false) {
  const size_t p = d.p, n = d.n;
  auto solution = linearCoordinates(d, Psi, lambda, optTol, maxIter, initial, relative);
  Vec beta = std::move(solution.first);
  LassoOut t;
  t.niter = solution.second;
  t.betaAll = beta;
  t.index.assign(p, 0);
  for (size_t j = 0; j < p; ++j) t.index[j] = std::fabs(beta[j]) > zeroTol;
  t.s = static_cast<size_t>(std::count(t.index.begin(), t.index.end(), 1));
  std::vector<size_t> sel;
  for (size_t j = 0; j < p; ++j)
    if (t.index[j]) sel.push_back(j);
  for (size_t j : sel) t.beta.push_back(beta[j]);
  const Vec yc = centered(d.y, d.ymvec, d.cons);
  std::vector<Vec> Xs;
  for (size_t j : sel) Xs.push_back(centered(d.X[j], d.mvec[j], d.cons));
  if (!sel.empty() && sel.size() < n) {
    t.betaPL = qrsolve(Xs, d.cons ? yc : d.y);
  } else if (!sel.empty()) {
    t.betaPL.assign(sel.size(), kNaN);
  }
  if (!d.cons) {
    t.intercept = t.interceptPL = 0;
  } else {
    const double my = qmean(d.y);
    double a = my, aPL = my;
    for (size_t q = 0; q < sel.size(); ++q) {
      const double mx = qmean(d.X[sel[q]]);
      a -= mx * t.beta[q];
      aPL -= mx * t.betaPL[q];
    }
    t.intercept = a;
    t.interceptPL = aPL;
  }
  t.betaAllPL.assign(p, 0.0);
  for (size_t q = 0; q < sel.size(); ++q) t.betaAllPL[sel[q]] = t.betaPL[q];
  t.v = d.cons ? yc : d.y;
  t.vPL = t.v;
  for (size_t q = 0; q < sel.size(); ++q)
    for (size_t i = 0; i < n; ++i) {
      t.v[i] -= Xs[q][i] * t.beta[q];
      t.vPL[i] -= Xs[q][i] * t.betaPL[q];
    }
  auto rms = [n](const Vec& v) {
    long double s = 0;
    for (double x : v) s += static_cast<long double>(x) * x;
    return std::sqrt(static_cast<double>(s / static_cast<long double>(n)));
  };
  t.rmse = rms(t.v);
  t.rmsePL = rms(t.vPL);
  long double pen = 0;
  for (size_t j = 0; j < p; ++j) pen += static_cast<long double>(Psi[j]) * std::fabs(beta[j]);
  t.objfn = t.rmse * t.rmse + lambda / static_cast<double>(d.n) * static_cast<double>(pen);
  t.Psi = Psi;
  t.lambda = lambda;
  return t;
}

std::string fmtLevel(double lv) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.17g", lv);
  return buf;
}

ojson runRlasso(const eval::ParsedCommand& parsed, SessionState& session) {
  ojson blocks = ojson::array();
  auto fail = [&blocks](ojson e) {
    blocks.push_back(std::move(e));
    return blocks;
  };
  if (!session.hasData()) return fail(ojson(eval::noDataLoaded()));
  if (parsed.weightType.has_value())
    return fail(rcError("rlasso with weights is not implemented in this version of PIET; "
                        "nothing was estimated",
                        198, "unsupported_option"));

  // ---- syntax (R:86-108) -----------------------------------------------------------
  ojson err;
  struct Term {
    std::string var;
    bool factor = false;
  };
  std::vector<Term> terms;
  for (const auto& tk : parsed.varlist) {
    if (tk.size() > 2 && (tk.compare(0, 2, "i.") == 0)) {
      std::vector<std::string> v;
      if (!e3::expandVars(tk.substr(2), session, &v, &err)) return fail(err);
      for (const auto& x : v) terms.push_back({x, true});
      continue;
    }
    if (tk.find_first_of(".#") != std::string::npos)
      return fail(rcError("rlasso: the term `" + tk + "` (a time-series or interaction operator) "
                          "is not implemented in this version of PIET",
                          198, "unsupported_option"));
    std::vector<std::string> v;
    if (!e3::expandVars(tk, session, &v, &err)) return fail(err);
    for (const auto& x : v) terms.push_back({x, false});
  }
  if (terms.size() < 2) return fail(rcError("too few variables specified", 102));
  {
    Datastore* ds = session.datastore();
    for (const auto& t : terms)
      if (ds->dtype(t.var) != "f64") return fail(rcError("type mismatch", 109));
  }
  static const std::vector<e3::OptSpec> kSpecs = {
      {"pnotpen", 4, true},     {"partial", 3, true},    {"fe", 2, false},
      {"noconstant", 6, false}, {"cluster", 2, true},    {"ols", 3, false},
      {"pols", 4, false},       {"prestd", 6, false},    {"dm", 2, false},
      {"verbose", 3, false},    {"vverbose", 8, false},  {"dots", 4, false},
      {"postall", 7, false},    {"testonly", 8, false},  {"noftools", 8, false},
      {"displayall", 10, false}, {"varwidth", 8, true},  {"supscore", 8, false},
      {"robust", 3, false},     {"c", 1, true},          {"gamma", 5, true},
      {"maxpsiiter", 8, true},  {"pminus", 6, true}};
  std::string pnotpen, partial, cluster;
  bool fe = false, noconstant = false, robust = false, noftools = false;
  double cOpt = 1.1, gammaOpt = 0;
  long long maxPsiIter = 2;
  for (const auto& [k, v] : parsed.opts) {
    const int idx = e3::matchOpt(kSpecs, k);
    if (idx < 0)
      return fail(rcError("rlasso, " + k + "(): option not implemented in this version of PIET "
                          "(or not an rlasso option); nothing was estimated",
                          198, "unsupported_option"));
    const e3::OptSpec& s = kSpecs[static_cast<size_t>(idx)];
    if (s.takesArg && v.isBool) return fail(rcError("option " + s.full + "() required", 198));
    const std::string arg = strutil::trim(v.strValue);
    const std::string& o = s.full;
    if (o == "pnotpen") {
      pnotpen = arg;
    } else if (o == "partial") {
      partial = arg;
    } else if (o == "fe") {
      fe = true;
    } else if (o == "noconstant") {
      noconstant = true;
    } else if (o == "cluster") {
      std::vector<std::string> cv;
      if (!e3::expandVars(arg, session, &cv, &err)) return fail(err);
      if (cv.size() != 1) return fail(rcError("too many variables specified", 103));
      cluster = cv[0];
    } else if (o == "robust") {
      robust = true;
    } else if (o == "noftools") {
      noftools = true;
    } else if (o == "displayall" || o == "varwidth") {
      // display only
    } else if (o == "c" || o == "gamma" || o == "maxpsiiter") {
      char* end = nullptr;
      const double d = std::strtod(arg.c_str(), &end);
      if (arg.empty() || end == nullptr || *end != '\0')
        return fail(rcError("option " + o + "() incorrectly specified", 198));
      if (o == "c") cOpt = d;
      else if (o == "gamma") gammaOpt = d;
      else maxPsiIter = static_cast<long long>(d);
    } else {
      return fail(rcError("rlasso, " + o + ": not implemented in this version of PIET; nothing "
                          "was estimated",
                          198, "unsupported_option"));
    }
  }

  // ---- marksample (R:116-119), fe needs xtset (R:141-148) -----------------------------
  Datastore* ds = session.datastore();
  const size_t Nall = static_cast<size_t>(ds->nRows());
  auto column = [&ds, Nall](const std::string& name) {
    const double* p = ds->col(name).f64();
    return std::vector<double>(p, p + Nall);
  };
  std::string ivar;
  if (fe) {
    if (!session.panelvar.has_value() || session.panelvar->empty())
      return fail(rcError("Error: fe option requires data to be xtset", 459));
    ivar = *session.panelvar;
  }
  std::vector<char> touse(Nall, 1);
  {
    std::string qual;
    if (parsed.ifExpr.has_value()) qual += " if " + *parsed.ifExpr;
    if (parsed.inRange.has_value()) qual += " in " + *parsed.inRange;
    if (!qual.empty()) {
      const std::string tv = "__rlasso_touse";
      if (!e3::runLine("quietly generate double " + tv + " = 1" + qual, session, &err))
        return fail(err);
      ds = session.datastore();
      const Vec t = column(tv);
      {
        SessionState::TempDropScope td(session);
        (void)e3::runLine("capture drop " + tv, session, nullptr);
      }
      ds = session.datastore();
      for (size_t i = 0; i < Nall; ++i) touse[i] = t[i] == 1.0;
    }
  }
  for (const auto& t : terms) {
    const Vec v = column(t.var);
    for (size_t i = 0; i < Nall; ++i)
      if (std::isnan(v[i])) touse[i] = 0;
  }
  Vec clv;
  bool clStr = false;
  std::vector<std::string> clStrVals;
  if (!cluster.empty()) {
    if (ds->dtype(cluster) == "f64") {
      clv = column(cluster);
      for (size_t i = 0; i < Nall; ++i)
        if (std::isnan(clv[i])) touse[i] = 0;
    } else {
      clStr = true;  // a string cluster variable is accepted (R:21 changelog)
      // A string column is a Factor at this layer: its code names the group, a
      // negative code is missing (handlers_aer_estim.cpp numericColumn's reading).
      const cecon::Column& c = ds->col(cluster);
      const int32_t* codes = c.codes();
      clStrVals.resize(Nall);
      for (size_t i = 0; i < Nall; ++i) {
        if (codes == nullptr || codes[i] < 0 ||
            (static_cast<size_t>(codes[i]) < c.levels.size() && c.levels[codes[i]].empty())) {
          touse[i] = 0;
          continue;
        }
        clStrVals[i] = std::to_string(codes[i]);
      }
    }
  }
  Vec ivv;
  if (fe) {
    ivv = column(ivar);
    for (size_t i = 0; i < Nall; ++i)
      if (std::isnan(ivv[i])) touse[i] = 0;
  }

  // ---- fvexpand, duplicates, the checks (R:241-308) ---------------------------------
  struct XCol {
    std::string name;  // display name (`x1`, `1939b.year`, `1940.year`)
    std::string var;
    double level = kNaN;
    bool omitted = false;
  };
  std::vector<XCol> cols;
  std::set<std::string> seenNames;
  for (size_t ti = 0; ti < terms.size(); ++ti) {
    const Term& t = terms[ti];
    if (!t.factor) {
      if (seenNames.insert(t.var).second) cols.push_back({t.var, t.var, kNaN, false});
      continue;
    }
    const Vec v = column(t.var);
    std::set<double> lv;
    for (size_t i = 0; i < Nall; ++i)
      if (touse[i] && !std::isnan(v[i])) lv.insert(v[i]);
    bool first = true;
    for (double l : lv) {
      const std::string nm = fmtLevel(l) + (first ? "b." : ".") + t.var;
      if (seenNames.insert(nm).second) cols.push_back({nm, t.var, l, first});
      first = false;
    }
  }
  const std::string depvar = cols.front().name;
  if (!terms.front().factor) cols.erase(cols.begin());
  if (!pnotpen.empty()) {
    std::vector<std::string> pv;
    if (!e3::expandVars(pnotpen, session, &pv, &err)) return fail(err);
    for (const auto& x : pv) {
      bool in = false;
      for (const auto& c : cols) in = in || c.name == x;
      if (!in)
        return fail(rcError("syntax error - " + x + " in notpen(.) but not in list of regressors",
                            198));
    }
  }
  bool partialCons = false;
  {
    std::vector<std::string> pw;
    std::istringstream is(partial);
    for (std::string w; is >> w;) {
      if (w == "_cons") {
        partialCons = true;
        continue;
      }
      pw.push_back(w);
    }
    for (const auto& x : pw) {
      bool in = false;
      for (const auto& c : cols) in = in || c.name == x;
      if (!in)
        return fail(rcError("syntax error - " + x + " in partial(.) but not in list of regressors",
                            198));
    }
  }
  if (fe) {
    bool inModel = ivar == depvar;
    for (const auto& c : cols) inModel = inModel || c.var == ivar;
    if (inModel)
      return fail(rcError("syntax error - " + ivar + " is xtset variable and cannot be used in "
                          "model",
                          198));
  }
  if (partialCons && fe) return fail(rcError("error: incompatible options, partial(_cons) and fe", 198));
  if (fe && noconstant) return fail(rcError("error: incompatible options, fe and nocons", 198));
  if (!pnotpen.empty() || !partial.empty())
    return fail(rcError("rlasso with pnotpen()/partial() is not implemented in this version of "
                        "PIET; nothing was estimated",
                        198, "unsupported_option"));
  size_t N = 0;
  for (char t : touse) N += t ? 1 : 0;
  if (N == 0) return fail(rcError("no observations", 2000));

  int pminus = 0;
  for (const auto& c : cols) pminus += c.omitted ? 1 : 0;
  const bool consmodel = !noconstant && !fe;
  const bool consflag = consmodel;
  const bool dmflag = !consmodel;

  // ---- the estimation data (sample rows, cluster-sorted as lassoutils sorts) ---------
  std::vector<size_t> rows;
  for (size_t i = 0; i < Nall; ++i)
    if (touse[i]) rows.push_back(i);
  RlData d;
  d.cons = consflag;
  {
    const Vec yv = column(depvar);
    d.y.resize(N);
    for (size_t t = 0; t < N; ++t) d.y[t] = yv[rows[t]];
  }
  for (const auto& c : cols) {
    const Vec v = column(c.var);
    Vec x(N);
    for (size_t t = 0; t < N; ++t) {
      const double val = v[rows[t]];
      x[t] = std::isnan(c.level) ? val : (c.omitted ? 0.0 : (val == c.level ? 1.0 : 0.0));
    }
    d.X.push_back(std::move(x));
  }
  long long Ng = 0;
  if (fe) {
    // within transform over the panel variable (lassoutils _fe), groups over the sample
    std::map<double, std::vector<size_t>> grp;
    for (size_t t = 0; t < N; ++t) grp[ivv[rows[t]]].push_back(t);
    Ng = static_cast<long long>(grp.size());
    auto demean = [&grp](Vec& v) {
      for (const auto& [g, idx] : grp) {
        long double s = 0;
        for (size_t t : idx) s += v[t];
        const double m = static_cast<double>(s / static_cast<long double>(idx.size()));
        for (size_t t : idx) v[t] -= m;
      }
    };
    demean(d.y);
    for (auto& x : d.X) demean(x);
  }
  if (!cluster.empty()) {
    std::map<std::string, size_t> gs;
    std::map<double, size_t> gn;
    d.cl.resize(N);
    for (size_t t = 0; t < N; ++t) {
      if (clStr) gs.emplace(clStrVals[rows[t]], 0);
      else gn.emplace(clv[rows[t]], 0);
    }
    size_t k = 0;
    for (auto& kv : gs) kv.second = k++;
    for (auto& kv : gn) kv.second = k++;
    for (size_t t = 0; t < N; ++t)
      d.cl[t] = clStr ? gs[clStrVals[rows[t]]] : gn[clv[rows[t]]];
    d.nclust = k;
  }
  makeData(d, dmflag);
  const int p = static_cast<int>(d.p) - pminus;
  if (p <= 0)
    return fail(rcError("rlasso with no penalized variable is not implemented in this version of "
                        "PIET",
                        198, "unsupported_option"));

  // ---- RLasso (L:2373-2536) --------------------------------------------------------------
  const bool hetero = robust && cluster.empty();
  const double gammad = d.nclust == 0 ? std::log(static_cast<double>(d.n))
                                      : std::log(static_cast<double>(d.nclust));
  const double gamma = gammaOpt > 0 ? gammaOpt : 0.1 / gammad;
  const double c = cOpt, c0 = cOpt;
  const double optTol = 1e-10, zeroTol = 1e-4, psiTol = 1e-4;
  const long long maxIter = 10000;
  Vec v = initialResiduals(d, 5);
  double s1 = 0;
  {
    long double s = 0;
    for (double x : v) s += static_cast<long double>(x) * x;
    s1 = std::sqrt(static_cast<double>(s / static_cast<long double>(d.n)));
  }
  Vec Psi = scoreStd(d, v, s1, hetero);
  double lambda = lambdaCalc(d, pminus, gamma, c0);
  long long iter = 1;
  LassoOut betas = doLasso(d, Psi, lambda, optTol, maxIter, zeroTol);
  double Delta = 0;
  do {
    ++iter;
    const double s0 = s1;
    v = betas.vPL;
    s1 = betas.rmsePL;
    Delta = std::fabs(s1 - s0);
    Psi = scoreStd(d, v, s1, hetero);
    lambda = lambdaCalc(d, pminus, gamma, c);
    betas = doLasso(d, Psi, lambda, optTol, maxIter, zeroTol);
  } while (iter < maxPsiIter && Delta > psiTol);
  const double lambda0 = lambda;
  const double lambdaRep = lambda * s1;
  const double slambda = lambda * s1 / d.ysd;  // ysdp = ysd without unpenalised regressors
  Vec PsiRep(d.p), sPsi(d.p);
  for (size_t j = 0; j < d.p; ++j) {
    PsiRep[j] = betas.Psi[j] / s1;
    const double sp = PsiRep[j] / d.sdvec[j];
    sPsi[j] = std::isfinite(sp) ? sp : 0.0;
  }

  // ---- ReturnResults + rlasso.ado's e() (L:3880-3990, R:567-835) ---------------------
  std::vector<std::string> selected;
  for (size_t j = 0; j < d.p; ++j)
    if (betas.index[j]) selected.push_back(cols[j].name);
  std::vector<std::string> bNames = selected;
  Vec bVals = betas.beta, bOLS = betas.betaPL;
  if (consflag) {
    bNames.push_back("_cons");
    bVals.push_back(betas.intercept);
    bOLS.push_back(betas.interceptPL);
  }
  std::vector<std::string> allNames;
  for (const auto& col : cols) allNames.push_back(col.name);
  Vec bAll = betas.betaAll, bAllOLS = betas.betaAllPL;
  if (consflag) {
    allNames.push_back("_cons");
    bAll.push_back(betas.intercept);
    bAllOLS.push_back(betas.interceptPL);
  }
  const bool betaEmpty = bNames.empty();
  std::vector<std::string> selected0 = selected;
  if (consmodel) selected0.push_back("_cons");
  auto joinW = [](const std::vector<std::string>& v) {
    std::string s;
    for (const auto& w : v) s += (s.empty() ? "" : " ") + w;
    return s;
  };

  ojson block;
  block["type"] = "estimation";
  block["estimator"] = "rlasso";
  block["depvar"] = depvar;
  block["n_obs"] = static_cast<long long>(N);
  {
    ojson ct = ojson::array();
    for (size_t j = 0; j < bNames.size(); ++j) {
      ojson r;
      r["name"] = bNames[j];
      r["b"] = bVals[j];
      r["se"] = nullptr;
      r["t"] = nullptr;
      r["p"] = nullptr;
      r["ci_lo"] = nullptr;
      r["ci_hi"] = nullptr;
      ct.push_back(r);
    }
    block["coeftable"] = ct;
  }
  block["posts_e_b"] = !betaEmpty;
  LastModel lm;
  lm.cobraCmd = "rlasso";
  lm.stataCmd = "rlasso";
  lm.y = depvar;
  lm.depvar = depvar;
  lm.vcov = "";
  lm.result = block;
  (void)e3::installEsampleMask(session, lm, touse);
  session.setLastModel(lm);
  session.lastEstBlock = block;
  auto rowMat = [&depvar](const Vec& vals, const std::vector<std::string>& names) {
    eval::EvalMatrix m;
    m.data.push_back(vals);
    m.rownames = {depvar};
    m.colnames = names;
    return m;
  };
  auto putM = [&session](const std::string& nm, eval::EvalMatrix m) {
    for (auto& kv : session.eval.eMatrices)
      if (kv.first == nm) {
        kv.second = std::move(m);
        session.noteEOnFit(nm);
        return;
      }
    session.eval.eMatrices.emplace_back(nm, std::move(m));
    session.noteEOnFit(nm);
  };
  if (!betaEmpty) {
    for (auto& kv : session.eval.eMatrices)
      if (kv.first == "b") kv.first = "__stale_b";
    putM("b", rowMat(bVals, bNames));
    putM("beta", rowMat(bVals, bNames));
    putM("betaOLS", rowMat(bOLS, bNames));
  } else {
    // ereturn post with no b (R:717-719): e(b) does not exist; r(beta) is a 1x1 missing
    putM("beta", rowMat({kNaN}, {"c1"}));
    putM("betaOLS", rowMat({kNaN}, {"c1"}));
  }
  {
    std::vector<std::string> pn;
    for (const auto& col : cols) pn.push_back(col.name);
    putM("betaAll", rowMat(bAll, allNames));
    putM("betaAllOLS", rowMat(bAllOLS, allNames));
    putM("Psi", rowMat(PsiRep, pn));
    putM("sPsi", rowMat(sPsi, pn));
    putM("ePsi", rowMat(PsiRep, pn));
  }
  using e3::setEScalarOnFit;
  using e3::setEStringOnFit;
  setEScalarOnFit(session, "N", static_cast<double>(N));
  setEStringOnFit(session, "noftools", fe && noftools ? "noftools" : "");
  setEStringOnFit(session, "postall", "");
  setEScalarOnFit(session, "niter", static_cast<double>(betas.niter));
  setEScalarOnFit(session, "maxiter", static_cast<double>(maxIter));
  setEScalarOnFit(session, "npsiiter", static_cast<double>(iter));
  setEScalarOnFit(session, "maxpsiiter", static_cast<double>(maxPsiIter));
  setEStringOnFit(session, "robust", robust ? "robust" : "");
  setEStringOnFit(session, "ivar", ivar);
  setEStringOnFit(session, "selected", joinW(selected));
  {
    std::vector<std::string> vx;
    for (const auto& col : cols) vx.push_back(col.name);
    setEStringOnFit(session, "varXmodel", joinW(vx));
    setEStringOnFit(session, "varX", joinW(vx));
  }
  setEStringOnFit(session, "estimator", "lasso");
  setEStringOnFit(session, "method", "lasso");
  setEStringOnFit(session, "predict", "rlasso_p");
  setEScalarOnFit(session, "center", 0);
  setEScalarOnFit(session, "cons", consmodel ? 1 : 0);
  setEScalarOnFit(session, "lambda", lambdaRep);
  setEScalarOnFit(session, "lambda0", lambda0);
  setEScalarOnFit(session, "slambda", slambda);
  setEScalarOnFit(session, "c", c);
  setEScalarOnFit(session, "gamma", gamma);
  if (!cluster.empty()) {
    setEStringOnFit(session, "clustvar", cluster);
    setEScalarOnFit(session, "N_clust", static_cast<double>(d.nclust));
  }
  if (fe) setEScalarOnFit(session, "N_g", static_cast<double>(Ng));
  setEScalarOnFit(session, "fe", fe ? 1 : 0);
  setEScalarOnFit(session, "rmse", betas.rmse);
  setEScalarOnFit(session, "rmseOLS", betas.rmsePL);
  setEScalarOnFit(session, "pmse", betas.objfn);
  setEScalarOnFit(session, "pminus", pminus);
  setEScalarOnFit(session, "p", p);
  setEScalarOnFit(session, "s", static_cast<double>(selected.size()));
  setEStringOnFit(session, "selected0", joinW(selected0));
  setEStringOnFit(session, "partial", "");
  setEScalarOnFit(session, "partial_ct", 0);
  setEScalarOnFit(session, "s0", static_cast<double>(selected0.size()));
  setEStringOnFit(session, "pnotpen", consmodel ? "_cons" : "");
  setEScalarOnFit(session, "pnotpen_ct", consmodel ? 1 : 0);
  setEStringOnFit(session, "properties", betaEmpty ? "" : "b");
  {
    std::string text = "Selected: " + (selected.empty() ? std::string("(none)") : joinW(selected));
    blocks.push_back(e3::displayText(text));
  }
  blocks.push_back(block);
  return blocks;
}

}  // namespace

// Returns an ARRAY of blocks; the dispatcher pushes each element (never the
// array as one block -- the type_error.306 class).
namespace {

// Stata's built-in `lasso`: the estimator is not ported (refused:e3_deferred), but
// the input checks it makes before it fits are, with Stata's rc and message
// (oracle tests/eval/golden/aer_w2/w2j_e3_builtin.log:3401-3461):
//   * the model word: one of linear/logit/probit/poisson, else r(198) "invalid
//     model" (L_r_nomodel, `lasso y x1 x2`);
//   * rseed(#): a positive integer, else r(198) (L_r_rseedxxx, L_r_rseed0);
//   * selection(adaptive, unpenalized ridge): r(198) (L_r_unpen_ridge);
//   * selection(..., power(#)): # in [0.25, 2], else r(198) (L_r_power);
//   * selection(..., folds(#)): an integer >= 3, else r(198) (L_r_folds1);
//   * poisson: a negative depvar in the sample is r(459) (L_r_poisneg).
std::optional<ojson> lassoRefusal(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.hasData()) return ojson(eval::noDataLoaded());
  std::vector<std::string> toks = parsed.varlist;
  static const std::vector<std::string> kModels = {"linear", "logit", "probit", "poisson"};
  std::string model;
  if (!toks.empty()) {
    std::string w = toks[0];
    std::transform(w.begin(), w.end(), w.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (std::find(kModels.begin(), kModels.end(), w) != kModels.end()) model = w;
  }
  if (model.empty())
    return rcError("invalid model\n    Only one of linear, logit, probit, or poisson is allowed.",
                   198);
  toks.erase(toks.begin());
  auto parseNum = [](const std::string& t, double* out) {
    char* end = nullptr;
    const std::string a = strutil::trim(t);
    *out = std::strtod(a.c_str(), &end);
    return !a.empty() && end != nullptr && *end == '\0';
  };
  static const std::vector<e3::OptSpec> kSpecs = {{"rseed", 5, true}, {"selection", 3, true}};
  for (const auto& [k, v] : parsed.opts) {
    const int idx = e3::matchOpt(kSpecs, k);
    if (idx < 0 || v.isBool) continue;  // the estimator's (refused below)
    const std::string arg = strutil::trim(v.strValue);
    if (kSpecs[static_cast<size_t>(idx)].full == "rseed") {
      double d = 0;
      if (!parseNum(arg, &d) || !(d > 0) || d != std::floor(d))
        return rcError("option rseed() allows only a positive integer number", 198);
      continue;
    }
    // selection(METHOD [, suboptions])
    const size_t comma = arg.find(',');
    std::string method = strutil::trim(comma == std::string::npos ? arg : arg.substr(0, comma));
    std::transform(method.begin(), method.end(), method.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const std::string subs = comma == std::string::npos ? std::string() : arg.substr(comma + 1);
    bool unpen = false, ridge = false;
    size_t i = 0;
    while (i < subs.size()) {
      while (i < subs.size() && std::isspace(static_cast<unsigned char>(subs[i]))) ++i;
      size_t j = i;
      while (j < subs.size() && !std::isspace(static_cast<unsigned char>(subs[j])) && subs[j] != '(') ++j;
      std::string name = subs.substr(i, j - i);
      std::transform(name.begin(), name.end(), name.begin(),
                     [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      std::string sarg;
      bool hasArg = false;
      if (j < subs.size() && subs[j] == '(') {
        const size_t close = subs.find(')', j);
        sarg = subs.substr(j + 1, (close == std::string::npos ? subs.size() : close) - j - 1);
        hasArg = true;
        j = close == std::string::npos ? subs.size() : close + 1;
      }
      i = j;
      if (name.empty()) continue;
      if (name.size() >= 5 && std::string("unpenalized").compare(0, name.size(), name) == 0)
        unpen = true;
      else if (name == "ridge")
        ridge = true;
      else if (name == "power" && hasArg) {
        double d = 0;
        if (!parseNum(sarg, &d) || d < 0.25 || d > 2)
          return rcError("option power() must specify a real number between 0.25 and 2", 198);
      } else if (name == "folds" && hasArg) {
        double d = 0;
        if (!parseNum(sarg, &d) || d != std::floor(d) || d < 3)
          return rcError("option folds() must be an integer >=3", 198);
      }
    }
    if (method == "adaptive" && unpen && ridge)
      return rcError("invalid suboption in selection(adaptive,  )\n    Option unpenalized cannot "
                     "be specified with option ridge.",
                     198);
  }
  if (model == "poisson" && !toks.empty()) {
    ojson err;
    std::vector<std::string> vars;
    std::string spec;
    for (const auto& t : toks)
      if (t.find_first_of(".#()") == std::string::npos) spec += (spec.empty() ? "" : " ") + t;
    if (!spec.empty() && e3::expandVars(spec, session, &vars, &err) && !vars.empty()) {
      Datastore* ds = session.datastore();
      const size_t n = static_cast<size_t>(ds->nRows());
      std::vector<const double*> cols;
      for (const auto& v : vars) {
        if (ds->dtype(v) != "f64") return std::nullopt;
        cols.push_back(ds->col(v).f64());
      }
      for (size_t r = 0; r < n; ++r) {
        bool complete = true;
        for (const double* c : cols) complete = complete && c != nullptr && !std::isnan(c[r]);
        if (complete && cols[0][r] < 0)
          return rcError("variable " + vars[0] + " must be greater than or equal to zero", 459);
      }
    }
  }
  return std::nullopt;
}


struct PoissonCoordinateFit {
  Vec beta, mu;
  double intercept = 0, ll = kNaN;
  bool converged = false;
  long long iterations = 0;
};

// Internal convergence floor of the Poisson lasso solvers: the largest relative
// coefficient change at which an IRLS/coordinate-descent iterate is accepted.
// Stata's default tolerance(1e-7) stops its compiled solver 4e-9..3.5e-8 from
// its own optimum; stopping PIET at the same 1e-7 leaves PIET's iterate on the
// other side of the optimum, doubling the gap to the oracle and violating the
// unpenalized intercept score equation sum(y-mu)=0 at 1e-9. At 1e-11 PIET's
// gap to the oracle equals Stata's own early-stop error and the score
// residual is 2.5e-13. The reported e() fields keep Stata's semantics: none of
// them records the tolerance, and tolerance() above the floor is honoured.
constexpr double kPoissonConvergenceFloor = 1e-11;

// [LASSO] lasso pp.163,166: Poisson negative likelihood with an L1 penalty,
// solved by IRLS and coordinate descent on the weighted quadratic objective.
// The constant is unpenalized. Backtracking protects the actual convex
// objective when a quadratic Newton approximation proposes an oversized step.
PoissonCoordinateFit poissonCoordinates(const RlData& d, double lambda, const Vec& loadings,
                                        const Vec& initial, double initialIntercept, double tol) {
  // tolerance() is Stata's stopping rule for ITS iterate; PIET answers its own
  // optimum, so a user tolerance never loosens the internal floor.
  tol = std::min(tol, kPoissonConvergenceFloor);
  PoissonCoordinateFit fit;
  fit.beta = initial; fit.intercept = initialIntercept;
  const size_t n = d.y.size(), p = d.X.size();
  auto objective = [&](const Vec& beta, double intercept, Vec* means) {
    long double loss = 0;
    if (means) means->resize(n);
    for (size_t i = 0; i < n; ++i) {
      double eta = intercept;
      for (size_t j = 0; j < p; ++j) eta += d.X[j][i]*beta[j];
      const double mu = std::exp(eta);
      if (!std::isfinite(mu) || !(mu > 0)) return std::numeric_limits<double>::infinity();
      loss += mu-d.y[i]*eta;
      if (means) (*means)[i] = mu;
    }
    double value = static_cast<double>(loss/n);
    for (size_t j = 0; j < p; ++j) value += lambda*loadings[j]*std::fabs(beta[j]);
    return value;
  };
  double oldObjective = objective(fit.beta, fit.intercept, &fit.mu);
  if (!std::isfinite(oldObjective)) return fit;
  for (int outer = 0; outer < 100; ++outer) {
    const Vec old = fit.beta;
    const double oldIntercept = fit.intercept;
    Vec residual(n);
    long double weightSum = 0;
    for (size_t i = 0; i < n; ++i) {
      residual[i] = (d.y[i]-fit.mu[i])/fit.mu[i];
      weightSum += fit.mu[i];
    }
    bool innerConverged = false;
    for (int inner = 0; inner < 100000; ++inner) {
      const Vec previous = fit.beta;
      const double previousIntercept = fit.intercept;
      long double score = 0;
      for (size_t i = 0; i < n; ++i) score += fit.mu[i]*residual[i];
      const double increment = static_cast<double>(score/weightSum);
      fit.intercept += increment;
      for (double& value : residual) value -= increment;
      for (size_t j = 0; j < p; ++j) {
        long double hessian = 0, cross = 0;
        for (size_t i = 0; i < n; ++i) {
          hessian += fit.mu[i]*d.X[j][i]*d.X[j][i];
          cross += fit.mu[i]*d.X[j][i]*residual[i];
        }
        if (!(hessian > 0)) return fit;
        const double rho = static_cast<double>(cross+hessian*fit.beta[j]);
        const double next = std::copysign(std::max(std::fabs(rho)-n*lambda*loadings[j], 0.0), rho)/static_cast<double>(hessian);
        const double change = next-fit.beta[j]; fit.beta[j] = next;
        for (size_t i = 0; i < n; ++i) residual[i] -= d.X[j][i]*change;
      }
      double relative = std::fabs(fit.intercept-previousIntercept)/(std::fabs(fit.intercept)+1);
      for (size_t j = 0; j < p; ++j)
        relative = std::max(relative, std::fabs(fit.beta[j]-previous[j])/(std::fabs(fit.beta[j])+1));
      ++fit.iterations;
      if (relative < tol) { innerConverged = true; break; }
    }
    if (!innerConverged) return fit;
    const Vec proposed = fit.beta;
    const double proposedIntercept = fit.intercept;
    double step = 1, nextObjective = objective(fit.beta, fit.intercept, nullptr);
    while ((!std::isfinite(nextObjective) || nextObjective > oldObjective+1e-12*(1+std::fabs(oldObjective))) && step > 1e-10) {
      step *= 0.5;
      for (size_t j = 0; j < p; ++j) fit.beta[j] = old[j]+step*(proposed[j]-old[j]);
      fit.intercept = oldIntercept+step*(proposedIntercept-oldIntercept);
      nextObjective = objective(fit.beta, fit.intercept, nullptr);
    }
    if (!std::isfinite(nextObjective)) return fit;
    objective(fit.beta, fit.intercept, &fit.mu);
    oldObjective = nextObjective;
    double relative = std::fabs(fit.intercept-oldIntercept)/(std::fabs(fit.intercept)+1);
    for (size_t j = 0; j < p; ++j)
      relative = std::max(relative, std::fabs(fit.beta[j]-old[j])/(std::fabs(fit.beta[j])+1));
    if (relative < tol) { fit.converged = true; break; }
  }
  long double ll = 0;
  for (size_t i = 0; i < n; ++i) ll += d.y[i]*std::log(fit.mu[i])-fit.mu[i]-std::lgamma(d.y[i]+1);
  fit.ll = static_cast<double>(ll);
  return fit;
}

// The unpenalized Poisson maximum likelihood fit on the selected columns, by
// Newton-Raphson with step halving, to machine precision. This is what
// Stata's lasso reports as e(b_postselection): a plain `poisson` fit of the
// selected variables, independent of the lasso tolerance() (measured: the
// postselection vector does not move under any lasso option, and agrees with
// a tight `poisson` to 3e-14). The Newton step solves the Poisson normal
// equations X'diag(mu)X delta = X'(y-mu), so the iterate converges
// quadratically: a relative step below 1e-12 leaves an error far under the
// double-precision rounding of the score. Returns converged=false when the
// information matrix is singular or the iteration stalls, so the caller can
// fall back to the coordinate solver.
PoissonCoordinateFit poissonNewton(const RlData& d, double initialIntercept) {
  PoissonCoordinateFit fit;
  const size_t n = d.y.size(), q = d.X.size(), m = q+1;
  Vec theta(m, 0.0); theta[0] = initialIntercept;
  auto linear = [&](const Vec& coefficients, Vec& eta) {
    eta.assign(n, coefficients[0]);
    for (size_t j = 0; j < q; ++j) for (size_t i = 0; i < n; ++i) eta[i] += d.X[j][i]*coefficients[j+1];
  };
  auto logLikelihood = [&](const Vec& eta) {
    long double total = 0;
    for (size_t i = 0; i < n; ++i) {
      const double mu = std::exp(eta[i]);
      if (!std::isfinite(mu)) return -std::numeric_limits<double>::infinity();
      total += d.y[i]*eta[i]-mu;
    }
    return static_cast<double>(total);
  };
  Vec eta; linear(theta, eta);
  double value = logLikelihood(eta);
  if (!std::isfinite(value)) return fit;
  for (int iteration = 0; iteration < 100; ++iteration) {
    std::vector<long double> information(m*m, 0.0L), score(m, 0.0L);
    for (size_t i = 0; i < n; ++i) {
      const long double mu = std::exp(eta[i]), residual = d.y[i]-mu;
      std::vector<long double> row(m, 1.0L);
      for (size_t j = 0; j < q; ++j) row[j+1] = d.X[j][i];
      for (size_t a = 0; a < m; ++a) {
        score[a] += row[a]*residual;
        for (size_t b = 0; b <= a; ++b) information[a*m+b] += mu*row[a]*row[b];
      }
    }
    // Cholesky factorization of the lower triangle, then the two solves.
    long double largest = 0;
    for (size_t a = 0; a < m; ++a) largest = std::max(largest, information[a*m+a]);
    std::vector<long double> factor(m*m, 0.0L);
    for (size_t a = 0; a < m; ++a) {
      for (size_t b = 0; b <= a; ++b) {
        long double sum = information[a*m+b];
        for (size_t c = 0; c < b; ++c) sum -= factor[a*m+c]*factor[b*m+c];
        if (a == b) {
          if (!(sum > 1e-14L*largest)) return fit;
          factor[a*m+a] = std::sqrt(sum);
        } else factor[a*m+b] = sum/factor[b*m+b];
      }
    }
    std::vector<long double> step(score);
    for (size_t a = 0; a < m; ++a) {
      for (size_t c = 0; c < a; ++c) step[a] -= factor[a*m+c]*step[c];
      step[a] /= factor[a*m+a];
    }
    for (size_t a = m; a-- > 0;) {
      for (size_t c = a+1; c < m; ++c) step[a] -= factor[c*m+a]*step[c];
      step[a] /= factor[a*m+a];
    }
    double scale = 1, next = -std::numeric_limits<double>::infinity();
    Vec proposal(m), proposalEta;
    bool accepted = false;
    for (int halving = 0; halving < 60 && !accepted; ++halving) {
      for (size_t a = 0; a < m; ++a) proposal[a] = theta[a]+scale*static_cast<double>(step[a]);
      linear(proposal, proposalEta);
      next = logLikelihood(proposalEta);
      accepted = std::isfinite(next) && next >= value-1e-13*(1+std::fabs(value));
      if (!accepted) scale *= 0.5;
    }
    if (!accepted) {
      // No step improves the likelihood: only the rounding floor of the score
      // allows that, so the current iterate is the optimum if its full Newton
      // step is already tiny; anything else is a failure for the caller.
      double full = 0;
      for (size_t a = 0; a < m; ++a) full = std::max(full, std::fabs(static_cast<double>(step[a]))/(std::fabs(theta[a])+1));
      if (!(full < 1e-9)) return fit;
      fit.converged = true;
      break;
    }
    double relative = 0;
    for (size_t a = 0; a < m; ++a)
      relative = std::max(relative, std::fabs(proposal[a]-theta[a])/(std::fabs(proposal[a])+1));
    theta = proposal; eta = proposalEta; value = next;
    ++fit.iterations;
    if (relative < 1e-12) { fit.converged = true; break; }
  }
  if (!fit.converged) return fit;
  fit.intercept = theta[0];
  fit.beta.assign(theta.begin()+1, theta.end());
  fit.mu.resize(n);
  long double ll = 0;
  for (size_t i = 0; i < n; ++i) {
    fit.mu[i] = std::exp(eta[i]);
    ll += d.y[i]*eta[i]-fit.mu[i]-std::lgamma(d.y[i]+1);
  }
  fit.ll = static_cast<double>(ll);
  return fit;
}

LassoOut poissonLasso(const RlData& d, double lambda, const Vec& loadings,
                      Vec& warm, double& warmIntercept, double tolerance) {
  LassoOut out;
  const auto fit = poissonCoordinates(d, lambda, loadings, warm, warmIntercept, tolerance);
  if (!fit.converged) { out.niter = 100000; return out; }
  warm = fit.beta; warmIntercept = fit.intercept;
  out.betaAll = fit.beta; out.intercept = fit.intercept; out.niter = fit.iterations;
  out.index.resize(d.p); out.betaAllPL.assign(d.p, 0);
  RlData post; post.y = d.y;
  for (size_t j = 0; j < d.p; ++j) {
    out.index[j] = std::fabs(fit.beta[j]) > 1e-12;
    if (out.index[j]) { ++out.s; post.X.push_back(d.X[j]); out.beta.push_back(fit.beta[j]); }
  }
  // e(b_postselection) is Stata's plain `poisson` fit of the selected set, not a
  // refit under the lasso tolerance: converge it with Newton to machine precision.
  auto refit = poissonNewton(post, std::log(qmean(d.y)));
  if (!refit.converged) refit = poissonCoordinates(post, 0, Vec(out.s, 0), Vec(out.s, 0), std::log(qmean(d.y)), tolerance);
  if (!refit.converged) { out.niter = 100000; return out; }
  out.interceptPL = refit.intercept; out.betaPL = refit.beta;
  size_t column = 0;
  for (size_t j = 0; j < d.p; ++j) if (out.index[j]) out.betaAllPL[j] = refit.beta[column++];
  out.v.resize(d.n); out.vPL.resize(d.n);
  for (size_t i = 0; i < d.n; ++i) { out.v[i] = d.y[i]-fit.mu[i]; out.vPL[i] = d.y[i]-refit.mu[i]; }
  long double ss = 0, postss = 0;
  for (size_t i = 0; i < d.n; ++i) { ss += out.v[i]*out.v[i]; postss += out.vPL[i]*out.vPL[i]; }
  out.rmse = std::sqrt(static_cast<double>(ss/d.n));
  out.rmsePL = std::sqrt(static_cast<double>(postss/d.n));
  out.loglik = fit.ll; out.loglikPL = refit.ll;
  return out;
}

// Built-in linear/Poisson lasso method identity: Stata's lasso (penalized GLM path);
// [LASSO] lasso, Methods and formulas pp.163,165-173, plus grid_long.ihlp,
// stop_long.ihlp and sel_bic_opts_long.ihlp. Unlike rlasso's output contract,
// lambda is on the normalized 1/(2N) RSS scale, X is standardized, and BIC
// traverses its own logarithmic grid. doLasso is only the shared convex solver:
// its unnormalized RSS objective receives 2*N*lambda, not rlasso defaults.
ojson runBuiltinLasso(const eval::ParsedCommand& parsed, SessionState& session) {
  auto fail = [](const ojson& error) { return ojson::array({error}); };
  auto deferred = [&]() { return fail(ojson(eval::refusedCommand("lasso", "e3_deferred"))); };
  if (parsed.varlist.empty()) return deferred();
  const std::string modelName = strutil::lower(parsed.varlist.front());
  const bool poisson = modelName == "poisson";
  if (!poisson && modelName != "linear") return deferred();
  if (parsed.weightType.has_value()) return fail(rcError("lasso: weights are not implemented", 198, "unsupported_option"));
  std::string method = "cv", subs;
  double stop = 1e-5, tolerance = 1e-7, gridRatio = kNaN, gridMin = kNaN;
  int gridSize = 100, folds = 10;
  bool seRule = false;
  // selection(adaptive): adaptive lasso -- steps(#) counts every lasso
  // (default 2 = one adaptive step), power(#) is delta in
  // kappa_j = 1/|beta_j|^delta ([LASSO] lasso p.169), and samegroup is the
  // DEFAULT: the user option is `nosamegroup`.
  bool sameGroup = true;
  int adaptiveSteps = 2;
  double adaptivePower = 1;
  std::optional<uint64_t> localSeed;
  bool postselection = false, allLambdas = false, strict = false, gridminok = false;
  bool hetero = true, requestedHomo = false, requestedHetero = false, stopok = false;
  auto number = [](const std::string& raw, double* out) {
    const std::string text = strutil::trim(raw);
    if (text.empty() || text.find_first_of("xX") != std::string::npos) return false;
    char* end = nullptr;
    *out = std::strtod(text.c_str(), &end);
    return end == text.c_str() + text.size() && std::isfinite(*out);
  };
  const std::vector<e3::OptSpec> specs = {{"selection", 3, true}, {"grid", 4, true},
      {"stop", 4, true}, {"tolerance", 3, true}, {"nolog", 5, false}, {"rseed", 5, true}};
  std::string grid;
  for (const auto& [key, value] : parsed.opts) {
    const int idx = e3::matchOpt(specs, key);
    if (idx < 0) return fail(rcError("lasso: option " + key + " not allowed", 198));
    const auto& spec = specs[static_cast<size_t>(idx)];
    if (spec.takesArg == value.isBool) return fail(rcError("option " + key + " incorrectly specified", 198));
    if (spec.full == "selection") {
      const size_t comma = value.strValue.find(',');
      method = strutil::lower(strutil::trim(value.strValue.substr(0, comma)));
      subs = comma == std::string::npos ? "" : value.strValue.substr(comma + 1);
    } else if (spec.full == "rseed") {
      double seed = 0;
      if (!number(value.strValue, &seed) || seed <= 0 || seed != std::floor(seed) || seed >= 0x1p+63)
        return fail(rcError("rseed() requires a positive integer below 2^63", 198));
      localSeed = static_cast<uint64_t>(seed);
    } else if (spec.full == "grid") grid = value.strValue;
    else if (spec.full == "stop") {
      if (!number(value.strValue, &stop) || stop < 0) return fail(rcError("invalid stop()", 198));
    } else if (spec.full == "tolerance") {
      if (!number(value.strValue, &tolerance) || tolerance <= 0) return fail(rcError("invalid tolerance()", 198));
    }
  }
  if (method.size() >= 4 && std::string("plugin").compare(0, method.size(), method) == 0) method = "plugin";
  if (method.size() >= 5 && std::string("adaptive").compare(0, method.size(), method) == 0) method = "adaptive";
  if (method != "plugin" && method != "bic" && method != "none" && method != "cv" && method != "adaptive") return deferred();
  const bool crossValidation = method == "cv" || method == "adaptive";
  for (const auto& [key, value] : eval::parseOpts(subs)) {
    if (crossValidation && key == "folds" && !value.isBool) {
      double count = 0;
      if (!number(value.strValue, &count) || count < 3 || count > 100000 || count != std::floor(count))
        return fail(rcError("folds() requires an integer >= 3", 198));
      folds = static_cast<int>(count); continue;
    }
    if (method == "adaptive" && (key == "steps" || key == "power") && !value.isBool) {
      double amount = 0;
      if (!number(value.strValue, &amount) || !(amount > 0) ||
          (key == "steps" && (amount != std::floor(amount) || amount > 1000)))
        return fail(rcError("invalid selection(adaptive) suboption " + key + "()", 198));
      if (key == "steps") adaptiveSteps = static_cast<int>(amount); else adaptivePower = amount;
      continue;
    }
    // ridge/unpenalized initial weights are a different first step; refused, not ignored.
    if (method == "adaptive" && (key == "ridge" ||
        (key.size() >= 5 && std::string("unpenalized").compare(0, key.size(), key) == 0))) return deferred();
    if (!value.isBool || !value.boolValue) return fail(rcError("invalid selection() suboption " + key, 198));
    if (method == "plugin" && key.size() >= 3 && std::string("homoskedastic").compare(0, key.size(), key) == 0) { hetero = false; requestedHomo = true; }
    else if (method == "plugin" && key.size() >= 3 && std::string("heteroskedastic").compare(0, key.size(), key) == 0) { hetero = true; requestedHetero = true; }
    else if ((method == "bic" || crossValidation) && key == "alllambdas") allLambdas = true;
    else if (method == "bic" && key.size() >= 7 && std::string("postselection").compare(0, key.size(), key) == 0) postselection = true;
    else if (crossValidation && key == "serule") seRule = true;
    else if (method == "adaptive" && key == "nosamegroup") sameGroup = false;
    else if ((method == "bic" || crossValidation) && key == "strict") strict = true;
    else if ((method == "bic" || crossValidation) && key == "gridminok") gridminok = true;
    else if ((method == "bic" || crossValidation) && key == "stopok") stopok = true;
    else return fail(rcError("invalid selection() suboption " + key, 198));
  }
  // rseed(#) is literally `set seed #`,
  // done before the fit for every selection method.
  if (localSeed.has_value()) session.seed = static_cast<long long>(*localSeed);
  if (poisson && (requestedHomo || requestedHetero)) return fail(rcError("Poisson plugin does not take an error-variance assumption", 198));
  if (requestedHomo && requestedHetero) return fail(rcError("homoskedastic and heteroskedastic may not be combined", 198));
  if (static_cast<int>(strict) + static_cast<int>(gridminok) + static_cast<int>(stopok) > 1)
    return fail(rcError("specify only one of strict, stopok and gridminok", 198));
  if (!grid.empty()) {
    const size_t comma = grid.find(',');
    const std::string size = strutil::trim(grid.substr(0, comma));
    if (!size.empty()) {
      double value = 0;
      if (!number(size, &value) || value < 2 || value > 100000 || value != std::floor(value))
        return fail(rcError("grid() requires an integer number of points >= 2", 198));
      gridSize = static_cast<int>(value);
    }
    if (comma != std::string::npos) for (const auto& [key, value] : eval::parseOpts(grid.substr(comma + 1))) {
      double num = 0;
      if (value.isBool || !number(value.strValue, &num) || num <= 0)
        return fail(rcError("invalid grid() suboption " + key, 198));
      if (key == "ratio" && num < 1) gridRatio = num;
      else if (key == "min") gridMin = num;
      else return fail(rcError("invalid grid() suboption " + key, 198));
    }
    if (std::isfinite(gridMin) && std::isfinite(gridRatio)) return fail(rcError("grid min() and ratio() may not be combined", 198));
  }
  ojson error;
  if (parsed.varlist.size() < 3) return fail(rcError("too few variables specified", 102));
  Datastore* ds = session.datastore();
  const std::string depvar = parsed.varlist[1];
  if (!ds->has(depvar)) return fail(rcError("variable " + depvar + " not found", 111));
  // One term of the expanded covariate list: a single variable or a `#`
  // product of variables, each component continuous (c.) or factor (i.).
  struct Component { std::string variable; bool factor; };
  struct Candidate { std::vector<Component> components; };
  std::vector<Candidate> predictors;
  std::vector<std::string> names{depvar};
  std::string specification;
  for (size_t i = 2; i < parsed.varlist.size(); ++i) specification += (specification.empty() ? "" : " ") + parsed.varlist[i];
  // _lasso_fvexpand.ado sets fvbase off: include every observed dummy level.
  // Grouped i.(g d), ordinary i.g and `#` products (i.g#i.d, c.x#i.d) use the
  // same all-level expansion; a product's columns are the cell indicators
  // (times any continuous component), ordered with the first component
  // varying slowest, as fvexpand lists them. `##` (implied main effects) and
  // explicit base/level operators stay refused rather than ignored.
  std::string expanded;
  for (size_t i = 0; i < specification.size();) {
    if (specification.compare(i, 3, "i.(") == 0) {
      const size_t close = specification.find(')', i+3);
      if (close == std::string::npos) return fail(rcError("unbalanced factor group", 198));
      std::istringstream group(specification.substr(i+3, close-i-3));
      for (std::string variable; group >> variable;) expanded += " i." + variable;
      i = close+1;
    } else expanded += specification[i++];
  }
  std::istringstream tokens(expanded);
  std::set<std::string> seen;
  for (std::string token; tokens >> token;) {
    if (token.find("##") != std::string::npos || token.front() == '#' || token.back() == '#') return deferred();
    std::vector<std::string> pieces;
    std::istringstream productTokens(token);
    for (std::string piece; std::getline(productTokens, piece, '#');) pieces.push_back(piece);
    const bool product = pieces.size() > 1;
    std::vector<Candidate> terms(1);
    for (std::string piece : pieces) {
      // Inside an interaction an unprefixed variable is a factor ([U] 11.4.3).
      bool factor = product;
      if (piece.rfind("i.", 0) == 0) { factor = true; piece.erase(0, 2); }
      else if (piece.rfind("c.", 0) == 0) { factor = false; piece.erase(0, 2); }
      if (piece.empty() || piece.find_first_of(".#()") != std::string::npos) return deferred();
      std::vector<std::string> variables;
      if (!e3::expandVars(piece, session, &variables, &error)) return fail(error);
      std::vector<Candidate> next;
      for (const auto& term : terms) for (const auto& variable : variables) {
        Candidate item = term;
        item.components.push_back({variable, factor});
        next.push_back(std::move(item));
      }
      terms = std::move(next);
    }
    for (auto& term : terms) {
      std::string key;
      for (const auto& component : term.components) key += (key.empty() ? "" : "#") + std::string(component.factor ? "i." : "c.") + component.variable;
      if (!product && term.components.front().variable == depvar) continue;
      if (!seen.insert(key).second) continue;
      for (const auto& component : term.components)
        if (std::find(names.begin(), names.end(), component.variable) == names.end()) names.push_back(component.variable);
      predictors.push_back(std::move(term));
    }
  }
  for (const auto& name : names) if (ds->dtype(name) != "f64") return fail(rcError("type mismatch", 109));
  const size_t nall = static_cast<size_t>(ds->nRows());
  std::vector<char> touse(nall, 1);
  if (parsed.ifExpr.has_value() || parsed.inRange.has_value()) {
    std::string marker;
    do { marker = "__piet_lasso_" + std::to_string(session.tempSeq++); } while (ds->has(marker));
    std::string qual;
    if (parsed.ifExpr.has_value()) qual += " if " + *parsed.ifExpr;
    if (parsed.inRange.has_value()) qual += " in " + *parsed.inRange;
    if (!e3::runLine("quietly generate byte " + marker + " = 1" + qual, session, &error)) return fail(error);
    ds = session.datastore();
    const double* values = ds->col(marker).f64();
    for (size_t i = 0; i < nall; ++i) touse[i] = values[i] == 1;
    SessionState::TempDropScope td(session);
    if (!e3::runLine("quietly drop " + marker, session, &error)) return fail(error);
    ds = session.datastore();
  }
  for (const auto& name : names) {
    const double* values = ds->col(name).f64();
    for (size_t i = 0; i < nall; ++i) if (!std::isfinite(values[i])) touse[i] = 0;
  }
  const size_t n = static_cast<size_t>(std::count(touse.begin(), touse.end(), 1));
  if (!n) return fail(rcError("no observations", 2000));
  if (n < 2 || predictors.empty()) return fail(rcError("insufficient observations or covariates", 2001));
  RlData data;
  data.cons = true;
  for (size_t i = 0; i < nall; ++i) if (touse[i]) data.y.push_back(ds->col(depvar).f64()[i]);
  if (poisson) {
    if (!(qmean(data.y) > 0)) return fail(rcError("Poisson outcome must have a positive mean", 498));
    for (double value : data.y) if (value < 0) return fail(rcError("variable " + depvar + " must be greater than or equal to zero", 459));
  }
  Vec means, scales, factorLevels;
  std::vector<std::string> activeNames, selectionNames, sourceNames;
  std::vector<ojson> componentSpecs;
  for (const auto& candidate : predictors) {
    // Each component contributes its observed levels (factor) or its values
    // (continuous); the column is the product over components, built on the
    // estimation sample. e(b) marks `bn` on a factor's first observed level
    // (gold: 1bn.g#0bn.d 3.g#0bn.d 3.g#1.d); allvars_sel marks every level bn.
    struct Column { Vec values; std::string name, selection; ojson parts; double level = kNaN; };
    std::vector<Column> columns(1);
    columns[0].values.assign(n, 1.0); columns[0].parts = ojson::array();
    const bool product = candidate.components.size() > 1;
    for (const auto& component : candidate.components) {
      const std::string& name = component.variable;
      Vec rawValues;
      const double* raw = ds->col(name).f64();
      std::set<double> observed;
      for (size_t i = 0; i < nall; ++i) if (touse[i]) {
        rawValues.push_back(raw[i]);
        if (component.factor) {
          if (raw[i] < 0 || raw[i] != std::floor(raw[i])) return fail(rcError("factor variables may not contain negative or noninteger values", 452));
          observed.insert(raw[i]);
        }
      }
      Vec levels;
      if (component.factor) levels.assign(observed.begin(), observed.end());
      else levels.push_back(kNaN);
      std::vector<Column> next;
      for (const auto& column : columns) for (size_t index = 0; index < levels.size(); ++index) {
        const double level = levels[index];
        Column item = column;
        for (size_t i = 0; i < n; ++i) item.values[i] *= component.factor ? (rawValues[i] == level ? 1.0 : 0.0) : rawValues[i];
        const std::string separator = item.name.empty() ? "" : "#";
        const std::string continuous = (product ? "c." : "")+name;
        item.name += separator+(component.factor ? fmtLevel(level)+(index == 0 ? "bn." : ".")+name : continuous);
        item.selection += separator+(component.factor ? fmtLevel(level)+"bn."+name : continuous);
        item.parts.push_back({{"variable", name}, {"level", component.factor ? ojson(level) : ojson(nullptr)}});
        if (!product && component.factor) item.level = level;
        next.push_back(std::move(item));
      }
      columns = std::move(next);
    }
    for (auto& column : columns) {
      Vec& values = column.values;
      const double mean = qmean(values);
      long double ss = 0;
      for (double value : values) ss += static_cast<long double>(value - mean)*(value - mean);
      const double scale = std::sqrt(static_cast<double>(ss / n));
      if (!(scale > 0)) continue;
      for (double& value : values) value = (value - mean)/scale;
      data.X.push_back(std::move(values)); means.push_back(mean); scales.push_back(scale);
      sourceNames.push_back(candidate.components.front().variable);
      factorLevels.push_back(column.level);
      activeNames.push_back(std::move(column.name)); selectionNames.push_back(std::move(column.selection));
      componentSpecs.push_back(std::move(column.parts));
    }
  }
  if (activeNames.empty()) return fail(rcError("lasso: all covariates are constant", 498));
  makeData(data, false);
  if (!(data.ysd > 0)) return fail(rcError("lasso: dependent variable has no variation", 498));
  const size_t p = data.p;
  const Vec unit(p, 1.0);
  const double gamma = 0.1 / std::log(static_cast<double>(std::max(n, p)));
  const double lambdaPlugin = 1.1 / std::sqrt(static_cast<double>(n)) * cecon::stata_invnormal(1 - gamma / (2*p));
  double lambdaMax = 0;
  for (double value : data.Xy) lambdaMax = std::max(lambdaMax, std::fabs(value) / n);
  if (!(lambdaMax > 0)) return fail(rcError("lasso: no nonzero score for lambda grid", 498));
  if (!std::isfinite(gridRatio)) gridRatio = p < n ? 1e-4 : 1e-2;
  if (std::isfinite(gridMin)) {
    if (gridMin >= lambdaMax) return fail(rcError("minimum lambda must be smaller than maximum", 198));
    gridRatio = gridMin / lambdaMax;
  }
  const double lambdaMin = lambdaMax * gridRatio;
  const long long maxIter = 100000;
  Vec warm(p, 0.0);
  double warmIntercept = poisson ? std::log(data.ymvec) : 0;
  auto fit = [&](double lambda, const Vec& loadings) {
    if (poisson) return poissonLasso(data, lambda, loadings, warm, warmIntercept, tolerance);
    // [LASSO] p.166: maximum relative coefficient change, tolerance(1e-7).
    // Successive grid points start at the preceding solution.
    LassoOut result = doLasso(data, loadings, 2.0*n*lambda, tolerance, maxIter, 1e-12, &warm, true);
    warm = result.betaAll;
    return result;
  };
  auto likelihood = [n, poisson](const LassoOut& result, bool post) {
    if (poisson) return post ? result.loglikPL : result.loglik;
    const double rmse = post ? result.rmsePL : result.rmse;
    return -0.5*n*(std::log(2*std::acos(-1.0)) + 1 + 2*std::log(rmse));
  };
  double saturated = 0, nullLikelihood = 0;
  if (poisson) for (double value : data.y) {
    saturated += (value > 0 ? value*std::log(value) : 0)-value-std::lgamma(value+1);
    nullLikelihood += value*std::log(data.ymvec)-data.ymvec-std::lgamma(value+1);
  }
  std::optional<LassoOut> selectedFit;
  Vec lambdas, loglik, bic, cvMeans, cvSE, pathNonzero;
  std::vector<size_t> foldId;
  size_t cvMinimum = 0, cvOneSE = 0;
  std::string cvRngIdentity;
  double stageLambdaMax = lambdaMax, stageLambdaMin = lambdaMin;
  size_t selected = 0;
  bool stopped = false, minimum = false;
  if (method == "plugin") {
    Vec residual;
    std::vector<char> previousSelection(p, 0);
    if (poisson) {
      std::vector<size_t> order(p); std::iota(order.begin(), order.end(), 0);
      std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return std::fabs(data.Xy[a]) > std::fabs(data.Xy[b]); });
      RlData initial; initial.y = data.y;
      for (size_t j = 0; j < std::min<size_t>(5, p); ++j) { initial.X.push_back(data.X[order[j]]); previousSelection[order[j]] = 1; }
      const auto start = poissonCoordinates(initial, 0, Vec(initial.X.size(), 0), Vec(initial.X.size(), 0), std::log(data.ymvec), tolerance);
      if (!start.converged) return fail(rcError("initial Poisson fit failed to converge", 430));
      for (size_t i = 0; i < n; ++i) residual.push_back(data.y[i]-start.mu[i]);
    } else residual = initialResiduals(data, 5);
    double sigma = 0;
    for (double value : residual) sigma += value*value;
    sigma = std::sqrt(sigma/n);
    Vec loadings = (hetero || poisson) ? scoreStd(data, residual, sigma, true) : unit;
    LassoOut result;
    double lambda = lambdaPlugin;
    for (int iteration = 0; iteration < 15; ++iteration) {
      if (!poisson && !hetero) lambda = lambdaPlugin*sigma;
      result = fit(lambda, loadings);
      if (result.niter >= maxIter) return fail(rcError("lasso failed to converge", 430));
      double change = 0;
      if (hetero || poisson) {
        Vec next = scoreStd(data, result.vPL, result.rmsePL, true);
        const double correction = poisson ? 1 : std::sqrt(static_cast<double>(n)/(n-result.s));
        for (size_t j = 0; j < p; ++j) { next[j] *= correction; change = std::max(change, std::fabs(next[j]-loadings[j])); }
        loadings = std::move(next);
      } else { change = std::fabs(sigma-result.rmse); sigma = result.rmse; }
      if (poisson) {
        if (result.index == previousSelection) break;
        previousSelection = result.index;
      } else if (change < 1e-8) break;
    }
    lambdas.push_back(lambda); selectedFit = std::move(result);
    loglik.push_back(likelihood(*selectedFit, false));
  } else if (crossValidation) {
    if (static_cast<size_t>(folds) > n) return fail(rcError("folds() may not exceed the estimation sample", 198));
    struct FoldFit { RlData data; Vec means, scales, warm; double intercept = 0; std::vector<size_t> columns, validation; };
    // Check the complete covariance/context/path envelope before any fold
    // allocation. Long-double arithmetic avoids size_t multiplication overflow;
    // the existing Settings budget is a hard boundary for this bounded path.
    // Poisson folds keep their training X for IRLS; linear folds keep only
    // covariance/scores.
    const long double pp = static_cast<long double>(p), nn = static_cast<long double>(n);
    const long double kk = static_cast<long double>(folds);
    const long double estimatedBytes = kk*sizeof(FoldFit) + sizeof(double)*
        ((kk+4)*pp*pp + (poisson ? kk+7 : 7)*nn*pp + static_cast<long double>(gridSize)*pp + (poisson ? 5*kk+12 : 12)*nn + 12*kk*pp);
    const int64_t configuredBudget = preflight::budgetBytes();
    const long double budget = configuredBudget > 0 ? static_cast<long double>(configuredBudget) : 256.0L*1024*1024;
    if (!std::isfinite(estimatedBytes) || estimatedBytes > budget ||
        estimatedBytes > static_cast<long double>(std::numeric_limits<size_t>::max()))
      return fail(rcError("lasso CV's fold covariance and working-data estimate exceeds the configured memory budget", 901, "memory_preflight"));
    // Fold partition, identified against the oracle machine's StataBE 19.5
    // (savecvtouse, internal g03 probes 3/5): one runiform() per estimation-sample
    // observation in data order from the SESSION stream (rseed(#) is `set seed
    // #`), observations ranked by that draw, then consecutive
    // blocks -- the first mod(N,K) folds hold floor(N/K)+1 observations, the
    // rest floor(N/K). Leave-one-out draws N too (fold = rank). e(rngstate) is
    // c(rngstate) before the draws.
    cecon::Mt64& stream = session.seed.stream();
    const cecon::Mt64 startState = stream;
    cvRngIdentity = startState.state_string();
    foldId.assign(n, 0);
    auto drawFolds = [&]() {
      Vec u(n);
      for (size_t i = 0; i < n; ++i) u[i] = stream.runiform();
      std::vector<size_t> order(n); std::iota(order.begin(), order.end(), 0);
      std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return u[a] < u[b]; });
      const size_t K = static_cast<size_t>(folds), q = n/K, extra = n%K;
      size_t rank = 0;
      for (size_t fold = 0; fold < K; ++fold)
        for (size_t count = 0; count < q+(fold < extra ? 1 : 0); ++count) foldId[order[rank++]] = fold;
    };
    // A CV minimum is identified after three consecutive smaller lambdas rise
    // above it by more than cvtolerance(1e-3). tolerance_long.ihlp says five
    // for nonlinear models, but the oracle machine's own Poisson CV stops at
    // the third (StataBE, `lasso poisson cnt i.(g d) x1-x8, selection(cv)
    // rseed(1)`: minimum ID 44, rises > 1e-3 at 48, 49, 50, n_lambda 50;
    // impl_D/stata_probe/probe.log), so the measured rule is used for both.
    const size_t risesNeeded = 3;
    // Adaptive lasso (lasso's adaptive step): step 1 is the CV lasso; each later
    // step refits on the previous step's selected covariates with penalty
    // weights 1/|beta_j|^power, on the same sample. With samegroup (default)
    // every step restores the RNG state first (`set rngstate`, lines 29-32,
    // 61, 96) so every step uses the same folds, and the state is restored at
    // the end (line 116): the command takes no net draws. n_lambda and ID
    // accumulate over the steps (preklp, line 103).
    std::vector<size_t> eligible(p); std::iota(eligible.begin(), eligible.end(), 0);
    Vec penalties(p, 1);
    const int steps = method == "adaptive" ? adaptiveSteps : 1;
    const bool restoreEachStep = method == "adaptive" && sameGroup;
    for (int step = 0; step < steps; ++step) {
      if (step > 0) {
        // adaptive lasso: a further step runs only when the previous one
        // selected covariates.
        std::vector<size_t> next;
        Vec nextPenalties;
        for (size_t j : eligible) if (selectedFit->index[j]) {
          next.push_back(j);
          nextPenalties.push_back(1/std::pow(std::fabs(selectedFit->betaAll[j]), adaptivePower));
        }
        if (next.empty()) break;
        // The weights are on the STANDARDIZED coefficients and are rescaled
        // to mean 1 over the retained covariates: with Stata's own step-1 CV
        // coefficients (e(b_standardized), StataBE probe impl_D/stata_probe)
        // this reproduces the step-2 lambda_gmax 11.66966259467304 to 1e-15;
        // raw coefficients or unnormalized weights give 17.08 / 0.132.
        long double weightSum = 0;
        for (double weight : nextPenalties) weightSum += weight;
        const double weightMean = static_cast<double>(weightSum/nextPenalties.size());
        for (double& weight : nextPenalties) weight /= weightMean;
        eligible = std::move(next); penalties = std::move(nextPenalties);
      }
      if (restoreEachStep) stream = startState;
      drawFolds();
      RlData stage; stage.cons = true; stage.y = data.y;
      for (size_t j : eligible) stage.X.push_back(data.X[j]);
      makeData(stage, false);
      double stageMax = 0;
      for (size_t j = 0; j < stage.p; ++j) {
        if (!std::isfinite(penalties[j]) || !(penalties[j] > 0)) return fail(rcError("nonfinite adaptive penalty weight", 498));
        stageMax = std::max(stageMax, std::fabs(stage.Xy[j])/(n*penalties[j]));
      }
      if (!(stageMax > 0)) return fail(rcError("lasso: no nonzero score for lambda grid", 498));
      double ratio = gridRatio;
      if (std::isfinite(gridMin)) {
        if (gridMin >= stageMax) return fail(rcError("minimum lambda must be smaller than maximum", 198));
        ratio = gridMin/stageMax;
      }
      stageLambdaMax = stageMax; stageLambdaMin = stageMax*ratio;
      // Each fold owns training-only centering/scaling and a warm-start path.
      // Validation outcomes never enter its transformation or fit.
      std::vector<FoldFit> training(static_cast<size_t>(folds));
      for (int fold = 0; fold < folds; ++fold) {
        auto& train = training[static_cast<size_t>(fold)]; train.data.cons = true;
        for (size_t i = 0; i < n; ++i) {
          if (foldId[i] == static_cast<size_t>(fold)) train.validation.push_back(i);
          else train.data.y.push_back(stage.y[i]);
        }
        for (size_t j = 0; j < stage.p; ++j) {
          Vec values;
          for (size_t i = 0; i < n; ++i) if (foldId[i] != static_cast<size_t>(fold)) values.push_back(stage.X[j][i]);
          const double mean = qmean(values);
          long double ss = 0;
          for (double value : values) ss += static_cast<long double>(value-mean)*(value-mean);
          const double scale = std::sqrt(static_cast<double>(ss/values.size()));
          if (!(scale > 0)) continue;
          for (double& value : values) value = (value-mean)/scale;
          train.data.X.push_back(std::move(values)); train.means.push_back(mean); train.scales.push_back(scale); train.columns.push_back(j);
        }
        makeData(train.data, false); train.warm.assign(train.columns.size(), 0);
        if (poisson) {
          if (!(train.data.ymvec > 0)) return fail(rcError("a Poisson training fold has no positive outcomes", 498));
          train.intercept = std::log(train.data.ymvec);
        } else {
          // CV needs covariance/scores only; retain no K copies of the N-row data.
          // Held-out predictions read the stage X through column indices.
          train.data.X.clear(); Vec().swap(train.data.y);
        }
      }
      Vec stageWarm(stage.p, 0);
      double stageIntercept = poisson ? std::log(stage.ymvec) : 0;
      auto stageFit = [&](double lambda) {
        if (poisson) return poissonLasso(stage, lambda, penalties, stageWarm, stageIntercept, tolerance);
        LassoOut result = doLasso(stage, penalties, 2.0*n*lambda, tolerance, maxIter, 1e-12, &stageWarm, true);
        stageWarm = result.betaAll;
        return result;
      };
      const size_t offset = lambdas.size();
      std::vector<Vec> fullStarts;
      Vec interceptStarts;
      double best = std::numeric_limits<double>::infinity(), previousFraction = 0;
      size_t rises = 0;
      minimum = false; stopped = false;
      // An adaptive step after the first runs the full-sample path to the
      // stop(#) rule first and then cross-validates EVERY lambda of it
      // (StataBE log: "Evaluating up to 100 lambdas in grid ... change in
      // deviance stopping tolerance reached / 10-fold cross-validation with
      // 98 lambdas"); the selected lambda is the CV minimum over that path
      // (oracle L_pois_adapt: ID 130 of 148, 98 step-2 lambdas). Step 1 and
      // plain CV interleave and stop once the minimum is identified.
      const bool wholePath = allLambdas || step > 0;
      for (int point = 0; point < gridSize; ++point) {
        const double lambda = std::exp(std::log(stageMax)+static_cast<double>(point)/(gridSize-1)*std::log(ratio));
        fullStarts.push_back(stageWarm); interceptStarts.push_back(stageIntercept);
        LassoOut full = stageFit(lambda);
        if (full.niter >= maxIter) return fail(rcError("full-sample lasso failed to converge", 430));
        Vec foldMeans(static_cast<size_t>(folds));
        long double totalLoss = 0;
        for (int fold = 0; fold < folds; ++fold) {
          auto& train = training[static_cast<size_t>(fold)];
          Vec weights;
          for (size_t column : train.columns) weights.push_back(penalties[column]);
          double intercept = 0;
          if (poisson) {
            auto estimate = poissonCoordinates(train.data, lambda, weights, train.warm, train.intercept, tolerance);
            if (!estimate.converged) return fail(rcError("cross-validation Poisson lasso failed to converge", 430));
            train.warm = std::move(estimate.beta); train.intercept = estimate.intercept;
            intercept = train.intercept;
          } else {
            auto estimate = linearCoordinates(train.data, weights, 2.0*train.data.n*lambda,
                                                tolerance, maxIter, &train.warm, true);
            if (estimate.second >= maxIter) return fail(rcError("cross-validation lasso failed to converge", 430));
            train.warm = std::move(estimate.first);
            intercept = train.data.ymvec;
            for (size_t j = 0; j < train.columns.size(); ++j) intercept -= train.data.mvec[j]*train.warm[j];
          }
          // Held-out deviance ([LASSO] lasso, How CV is performed, step 2b):
          // squared error (linear) or the Poisson unit deviance.
          long double loss = 0;
          for (size_t i : train.validation) {
            double prediction = intercept;
            for (size_t j = 0; j < train.columns.size(); ++j)
              prediction += train.warm[j]*(stage.X[train.columns[j]][i]-train.means[j])/train.scales[j];
            const double y = stage.y[i];
            if (poisson) {
              const double mu = std::exp(prediction);
              if (!std::isfinite(mu)) return fail(rcError("nonfinite held-out Poisson mean", 498));
              loss += 2*((y > 0 ? y*(std::log(y)-prediction) : 0)-(y-mu));
            } else {
              const double residual = y-prediction; loss += residual*residual;
            }
          }
          totalLoss += loss;
          foldMeans[static_cast<size_t>(fold)] = static_cast<double>(loss/train.validation.size());
        }
        const double cv = static_cast<double>(totalLoss/n);
        const double foldMean = qmean(foldMeans);
        long double variance = 0;
        for (double value : foldMeans) variance += static_cast<long double>(value-foldMean)*(value-foldMean);
        // The one-SE band is the sample standard deviation of the K fold
        // means, NOT divided by sqrt(K): it reproduces StataBE's e(ID_serule)
        // in 16 of 16 linear fits (rseed 2..9, two designs; sd/sqrt(K) in 0
        // of 16, the K-denominator sd in 9 of 16; impl_D/stata_probe/probe3).
        const double se = std::sqrt(static_cast<double>(variance/(folds-1)));
        lambdas.push_back(lambda); loglik.push_back(likelihood(full, false)); pathNonzero.push_back(static_cast<double>(full.s));
        cvMeans.push_back(cv); cvSE.push_back(se);
        if (point == 0 || cv < best) { best = cv; cvMinimum = offset+static_cast<size_t>(point); rises = 0; }
        else if ((cv-best)/(std::fabs(best)+1e-30) > 1e-3) ++rises;
        else rises = 0;
        // Deviance ratio of the full-sample fit (R-squared when linear).
        const double fraction = poisson ? 1-(saturated-full.loglik)/(saturated-nullLikelihood)
                                        : 1-full.rmse*full.rmse/(stage.ysd*stage.ysd);
        if (rises >= risesNeeded) { minimum = true; if (!wholePath) break; }
        if (point > 0 && stop > 0 && fraction > 0 && (fraction-previousFraction)/fraction < stop) { stopped = true; break; }
        previousFraction = fraction;
      }
      if (!minimum && strict) return fail(rcError("CV minimum not identified", 498));
      if (!minimum && !stopped && !gridminok && cvMinimum+1 == lambdas.size())
        return fail(rcError("CV minimum not identified before grid minimum", 498));
      cvOneSE = cvMinimum;
      const double threshold = cvMeans[cvMinimum]+cvSE[cvMinimum];
      for (size_t point = offset; point <= cvMinimum; ++point) if (cvMeans[point] <= threshold) { cvOneSE = point; break; }
      selected = seRule ? cvOneSE : cvMinimum;
      if (!minimum && stopped && !gridminok && !seRule) selected = lambdas.size()-1;
      stageWarm = fullStarts[selected-offset]; stageIntercept = interceptStarts[selected-offset];
      LassoOut chosen = stageFit(lambdas[selected]);
      if (chosen.niter >= maxIter) return fail(rcError("selected lasso failed to converge", 430));
      // Publish against the original design, with zero slots for covariates
      // an earlier adaptive step eliminated.
      const Vec beta = chosen.betaAll, postBeta = chosen.betaAllPL;
      const std::vector<char> chosenColumns = chosen.index;
      chosen.betaAll.assign(p, 0); chosen.betaAllPL.assign(p, 0); chosen.index.assign(p, 0);
      for (size_t j = 0; j < eligible.size(); ++j) {
        chosen.betaAll[eligible[j]] = beta[j]; chosen.betaAllPL[eligible[j]] = postBeta[j];
        chosen.index[eligible[j]] = chosenColumns[j];
      }
      selectedFit = std::move(chosen);
    }
    if (restoreEachStep) stream = startState;
  } else {
    size_t rises = 0;
    double best = std::numeric_limits<double>::infinity(), previousFraction = 0;
    for (int point = 0; point < gridSize; ++point) {
      const double lambda = std::exp(std::log(lambdaMax) + static_cast<double>(point)/(gridSize-1)*std::log(gridRatio));
      LassoOut result = fit(lambda, unit);
      if (result.niter >= maxIter) return fail(rcError("lasso failed to converge", 430));
      const double ll = likelihood(result, postselection);
      const double criterion = -2*ll + (result.s+1)*std::log(static_cast<double>(n));
      // Fraction of null deviance explained (R-squared in a linear lasso).
      const double fraction = poisson ? 1-(saturated-result.loglik)/(saturated-nullLikelihood)
                                      : 1-result.rmse*result.rmse/(data.ysd*data.ysd);
      lambdas.push_back(lambda); loglik.push_back(ll); bic.push_back(criterion);
      // Keep only the selected solution: retaining two N-row residual vectors
      // at every grid point needlessly multiplies dataset memory by grid size.
      if (point == 0 || criterion < best) {
        best = criterion; selected = static_cast<size_t>(point); rises = 0; selectedFit = std::move(result);
      }
      else if ((criterion-best)/(std::fabs(best)+1e-30) > 1e-2) ++rises;
      else rises = 0;
      if (method == "bic" && rises >= 2) { minimum = true; if (!allLambdas) break; }
      if (point > 0 && stop > 0 && fraction > 0 && (fraction-previousFraction)/fraction < stop) { stopped = true; break; }
      previousFraction = fraction;
    }
    if (method == "bic" && !minimum && strict) return fail(rcError("BIC minimum not identified", 498));
    if (method == "bic" && !minimum && !stopped && !gridminok && selected+1 == lambdas.size())
      return fail(rcError("BIC minimum not identified before grid minimum", 498));
  }
  const bool hasSelection = method != "none";
  const LassoOut& result = *selectedFit;
  std::vector<std::string> selectedNames, selectedTerms, postVariables{depvar};
  Vec coefficients, standardized, postCoefficients;
  double intercept = poisson ? result.intercept : data.ymvec;
  double postIntercept = poisson ? result.interceptPL : data.ymvec;
  if (hasSelection) for (size_t j = 0; j < p; ++j) if (result.index[j]) {
    selectedNames.push_back(activeNames[j]); selectedTerms.push_back(selectionNames[j]);
    for (const auto& component : componentSpecs[j]) {
      const std::string variable = component.at("variable").get<std::string>();
      if (std::find(postVariables.begin(), postVariables.end(), variable) == postVariables.end()) postVariables.push_back(variable);
    }
    const double b = result.betaAll[j]/scales[j], bp = result.betaAllPL[j]/scales[j];
    coefficients.push_back(b); standardized.push_back(result.betaAll[j]); postCoefficients.push_back(bp);
    intercept -= means[j]*b; postIntercept -= means[j]*bp;
  }
  std::vector<std::string> coefficientNames = selectedNames;
  if (hasSelection) { coefficientNames.push_back("_cons"); coefficients.push_back(intercept); standardized.push_back(poisson ? result.intercept : 0); postCoefficients.push_back(postIntercept); }
  ojson block = {{"type", "estimation"}, {"estimator", "lasso"}, {"depvar", depvar}, {"n_obs", n}, {"posts_e_b", hasSelection}};
  ojson table = ojson::array();
  for (size_t j = 0; j < coefficients.size(); ++j)
    table.push_back({{"name", coefficientNames[j]}, {"b", coefficients[j]}, {"se", nullptr}, {"t", nullptr}, {"p", nullptr}, {"ci_lo", nullptr}, {"ci_hi", nullptr}});
  block["coeftable"] = table;
  LastModel model;
  model.cobraCmd = "lasso"; model.stataCmd = "lasso"; model.y = depvar; model.depvar = depvar; model.result = block;
  ojson cached = {{"model", modelName}, {"selected", hasSelection}, {"intercept", intercept}, {"post_intercept", postIntercept}, {"terms", ojson::array()}};
  if (hasSelection) for (size_t j = 0; j < p; ++j) if (result.index[j]) {
    ojson term = {{"variable", sourceNames[j]}, {"name", activeNames[j]}, {"b", result.betaAll[j]/scales[j]}, {"post_b", result.betaAllPL[j]/scales[j]}, {"std_b", result.betaAll[j]}};
    term["level"] = std::isfinite(factorLevels[j]) ? ojson(factorLevels[j]) : ojson(nullptr);
    term["components"] = componentSpecs[j];
    cached["terms"].push_back(std::move(term));
  }
  (*model.result)["lasso_fit"] = std::move(cached);
  if (!e3::installEsampleMask(session, model, touse)) return fail(rcError("cannot install lasso estimation sample", 498));
  session.setLastModel(model);
  session.lastEstBlock = block;
  auto matrix = [&](const std::string& name, const Vec& values) {
    eval::EvalMatrix m; m.data = {values}; m.colnames = coefficientNames; m.rownames = {depvar};
    session.eval.eMatrices.emplace_back(name, std::move(m)); session.noteEOnFit(name);
  };
  if (hasSelection) {
    // An older model's matrices must not shadow this fit's names.
    for (const std::string name : {"b", "b_standardized", "b_postselection"})
      session.eval.eMatrices.erase(std::remove_if(session.eval.eMatrices.begin(), session.eval.eMatrices.end(), [&](const auto& entry) { return entry.first == name; }), session.eval.eMatrices.end());
    matrix("b", coefficients); matrix("b_standardized", standardized); matrix("b_postselection", postCoefficients);
  }
  using e3::setEScalarOnFit; using e3::setEStringOnFit;
  auto scalar = [&](const std::string& name, double value) { setEScalarOnFit(session, name, value); };
  auto text = [&](const std::string& name, const std::string& value) { setEStringOnFit(session, name, value); };
  auto joined = [](const std::vector<std::string>& values) { std::string out; for (const auto& value : values) out += (out.empty() ? "" : " ") + value; return out; };
  scalar("N", n); scalar("k_allvars", p); scalar("k_nonzero_sel", hasSelection ? result.s : kNaN);
  scalar("lambda_sel", hasSelection ? lambdas[selected] : kNaN); scalar("ID_sel", hasSelection ? selected+1 : kNaN);
  scalar("n_lambda", lambdas.size()); scalar("lambda_gmax", method == "plugin" ? lambdas[0] : crossValidation ? stageLambdaMax : lambdaMax);
  // CV/adaptive post the grid of the final lasso step (oracle: L_pois_adapt
  // lambda_gmin = 1e-4 x the step-2 lambda_gmax).
  scalar("lambda_gmin", method == "plugin" ? lambdas[0] : crossValidation ? stageLambdaMin : lambdaMin); scalar("lambda_last", lambdas.back());
  scalar("n_fold", crossValidation ? folds : kNaN); scalar("cvm_min", crossValidation ? cvMeans[cvMinimum] : kNaN); scalar("ll_sel", hasSelection ? loglik[selected] : kNaN); scalar("N_clust", kNaN);
  scalar("lambda_bic", method == "bic" ? lambdas[selected] : kNaN); scalar("k_nonzero_bic", method == "bic" ? result.s : kNaN); scalar("ID_bic", method == "bic" ? selected+1 : kNaN);
  text("model", modelName); text("lasso_selection", method); text("sel_criterion", method == "plugin" ? "plugin" : method == "none" ? "not sel." : crossValidation ? (seRule ? "CV 1SE" : "CV min.") : "BIC min.");
  text("allvars_sel", joined(selectedTerms)); text("othervars_sel", joined(selectedTerms));
  text("post_sel_vars", hasSelection ? joined(postVariables) : "");
  text("alwaysvars", ""); text("properties", hasSelection ? "b" : ""); text("predict", "lasso_p");
  if (crossValidation) {
    scalar("lambda_cv", lambdas[cvMinimum]); scalar("lambda_serule", lambdas[cvOneSE]);
    scalar("ID_cv", cvMinimum+1); scalar("ID_serule", cvOneSE+1);
    scalar("k_nonzero_cv", pathNonzero[cvMinimum]); scalar("k_nonzero_serule", pathNonzero[cvOneSE]);
    scalar("cvm_serule", cvMeans[cvOneSE]); text("rngstate", cvRngIdentity); text("cv_rng", cvRngIdentity);
    eval::EvalMatrix curve;
    curve.colnames = {"lambda", "cvm", "cvse", "k_nonzero"};
    for (size_t point = 0; point < lambdas.size(); ++point) {
      curve.data.push_back({lambdas[point], cvMeans[point], cvSE[point], pathNonzero[point]}); curve.rownames.push_back(std::to_string(point+1));
    }
    session.eval.eMatrices.emplace_back("cv_curve", std::move(curve)); session.noteEOnFit("cv_curve");
    eval::EvalMatrix ids; ids.colnames = {"fold"};
    size_t row = 0;
    for (size_t i = 0; i < nall; ++i) ids.data.push_back({touse[i] ? static_cast<double>(foldId[row++]+1) : kNaN});
    session.eval.eMatrices.emplace_back("cv_foldid", std::move(ids)); session.noteEOnFit("cv_foldid");
  }
  return ojson::array({block});
}

}  // namespace

nlohmann::ordered_json runLasso(const eval::ParsedCommand& parsed, SessionState& session) {
  std::string typed = parsed.cmd;
  std::transform(typed.begin(), typed.end(), typed.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  while (!typed.empty() && typed.back() == ':') typed.pop_back();
  if (typed == "rlasso") return runRlasso(parsed, session);
  nlohmann::ordered_json blocks = nlohmann::ordered_json::array();
  if (auto e = lassoRefusal(parsed, session)) {
    blocks.push_back(*e);
    return blocks;
  }
  return runBuiltinLasso(parsed, session);
}

// Prediction evaluates the fitted coefficient snapshot on the live data. It
// never re-fits an OLS/Poisson model or substitutes postselection coefficients.
nlohmann::ordered_json runLassoPredict(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.lastModel().has_value() || !session.lastModel()->result.has_value() || !session.lastModel()->result->contains("lasso_fit"))
    return rcError("last estimates are not a supported lasso fit", 301);
  const auto& fit = session.lastModel()->result->at("lasso_fit");
  if (!fit.value("selected", false)) return rcError("no lambda selected", 301);
  bool post = false, penalized = false, xb = false, count = false;
  const std::vector<e3::OptSpec> specs = {{"postselection", 7, false}, {"penalized", 3, false}, {"xb", 2, false}, {"n", 1, false}};
  for (const auto& [key, value] : parsed.opts) {
    const int idx = e3::matchOpt(specs, key);
    if (idx < 0 || !value.isBool || !value.boolValue) return rcError("predict: option "+key+" not allowed after lasso", 198);
    const std::string& name = specs[static_cast<size_t>(idx)].full;
    if (name == "postselection") post = true;
    else if (name == "penalized") penalized = true;
    else if (name == "xb") xb = true;
    else count = true;
  }
  const bool poisson = fit.value("model", std::string()) == "poisson";
  if ((post && penalized) || (xb && count) || (!poisson && count)) return rcError("incompatible lasso prediction options", 198);
  if (parsed.varlist.empty() || parsed.varlist.size() > 2) return rcError("predict requires a new variable name", 198);
  std::string declared;
  std::string variable = parsed.varlist.back();
  if (parsed.varlist.size() == 2) {
    declared = strutil::lower(parsed.varlist.front());
    if (!storage::isNumericType(declared)) return rcError("predict requires a numeric storage type", 198);
  }
  Datastore* ds = session.datastore();
  if (!ds) return ojson(eval::noDataLoaded());
  std::string expression = eval::pythonFloatRepr(fit.at(post ? "post_intercept" : "intercept").get<double>());
  std::string missing;
  std::set<std::string> seen;
  for (const auto& term : fit.at("terms")) {
    // A term is a product of components (one for a plain variable or factor
    // level), evaluated from the source variables on every row, so rows
    // outside the estimation sample are predicted too.
    const ojson components = term.contains("components") ? term.at("components") :
        ojson::array({{{"variable", term.at("variable")}, {"level", term.at("level")}}});
    std::string value;
    for (const auto& component : components) {
      const std::string name = component.at("variable").get<std::string>();
      if (!ds->has(name)) return rcError("variable "+name+" not found", 111);
      if (ds->dtype(name) != "f64") return rcError("type mismatch: "+name, 109);
      if (seen.insert(name).second) missing += (missing.empty() ? "" : " | ")+std::string("missing(")+name+")";
      std::string part = name;
      if (!component.at("level").is_null()) part = "("+name+"=="+eval::pythonFloatRepr(component.at("level").get<double>())+")";
      value += (value.empty() ? "" : "*")+part;
    }
    expression += " + ("+eval::pythonFloatRepr(term.at(post ? "post_b" : "b").get<double>())+")*("+value+")";
  }
  if (poisson && !xb) expression = "exp("+expression+")";
  if (!missing.empty()) expression = "cond("+missing+",.,("+expression+"))";
  std::string command = "generate "+(declared.empty() ? "" : declared+" ")+variable+" = "+expression;
  if (parsed.ifExpr.has_value()) command += " if "+*parsed.ifExpr;
  if (parsed.inRange.has_value()) command += " in "+*parsed.inRange;
  ojson error;
  if (!e3::runLine(command, session, &error)) return error;
  session.setVarLabel(variable, (poisson && !xb ? "Predicted number of events, " : "Fitted values, ")+
                                  std::string(post ? "postselection" : "penalized"));
  return mutationResult(session, "gen", {variable});
}

nlohmann::ordered_json runLassoCoef(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!parsed.varlist.empty()) return rcError("lassocoef: only the current fit is supported", 198);
  if (!session.lastModel().has_value() || !session.lastModel()->result.has_value() || !session.lastModel()->result->contains("lasso_fit"))
    return rcError("last estimates are not a supported lasso fit", 301);
  const auto& fit = session.lastModel()->result->at("lasso_fit");
  if (!fit.value("selected", false)) return rcError("no lambda selected", 301);
  bool coefficients = false, sortNames = false;
  std::string field = "std_b";
  for (const auto& [key, value] : parsed.opts) {
    if (key == "sort" && !value.isBool) {
      const std::string order = strutil::lower(strutil::trim(value.strValue));
      if (order != "none" && order != "names") return rcError("lassocoef: sort() supports none or names", 198);
      sortNames = order == "names";
    } else if ((key == "display" || key == "di") && !value.isBool) {
      const size_t comma = value.strValue.find(',');
      const std::string what = strutil::lower(strutil::trim(value.strValue.substr(0, comma)));
      if (what != "x" && what != "coef") return rcError("lassocoef: display() supports x or coef", 198);
      coefficients = what == "coef";
      const auto options = eval::parseOpts(comma == std::string::npos ? "" : value.strValue.substr(comma+1));
      if ((!coefficients && !options.empty()) || options.size() > 1) return rcError("lassocoef: specify one coefficient representation", 198);
      for (const auto& [option, v] : options) {
        if (!v.isBool || !v.boolValue) return rcError("invalid coefficient display option", 198);
        if (option.size() >= 5 && std::string("standardized").compare(0,option.size(),option) == 0) field = "std_b";
        else if (option.size() >= 3 && std::string("penalized").compare(0,option.size(),option) == 0) field = "b";
        else if (option.size() >= 4 && std::string("postselection").compare(0,option.size(),option) == 0) field = "post_b";
        else return rcError("lassocoef: display option "+option+" not supported", 198);
      }
    } else return rcError("lassocoef: option "+key+" not allowed", 198);
  }
  std::vector<std::pair<std::string,double>> terms;
  for (const auto& term : fit.at("terms")) terms.emplace_back(term.at("name").get<std::string>(),term.at(field).get<double>());
  if (sortNames) std::sort(terms.begin(),terms.end(),[](const auto& a,const auto& b){return a.first<b.first;});
  std::string text = coefficients ? "Selected coefficients\n" : "Selected variables\n";
  for (const auto& [name, value] : terms) text += name+"  "+(coefficients ? eval::pythonFloatRepr(value) : "x")+"\n";
  return e3::displayText(text);
}

}  // namespace handlers
}  // namespace piet
