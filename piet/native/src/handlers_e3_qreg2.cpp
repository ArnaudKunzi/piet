// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Portions derived from qreg2 4.00 (GPL-3.0).
// See THIRD_PARTY_NOTICES.
//
// handlers_e3_qreg2.cpp -- `qreg2` (Machado, Parente & Santos Silva, SSC; qreg2.ado
// "Version 4.00 - 31 Aug 2020", as identified below), fitted natively and derived from
// the ado. Oracle:
// tests/eval/golden/aer_w2/w2j_e3_qreg2.{do,log}; method notes in
// internal note qreg2/IDENTITY.md.
//
// The ado, in order:
//   * ado `syntax [varlist(fv numeric)] [if] [in] [fweight] [, Quantile(real .5)
//     NOTest mss(string) WLSiter(integer 1) Cluster(string) Epsilon(real 1e-7)
//     Silverman]`: aw/pw/iw are r(101) (the tokenizer's kStataLegalWeights row,
//     R01-R03); any other option is r(198) (R04 vce(robust), R05, R07 robust).
//   * ado marksample + `markout touse cluster, s`: an unknown cluster
//     variable is r(111) (R06); a missing (or empty-string) cluster drops the row.
//   * ado: weights together with cluster() print "Options cluster and
//     weights are not compatible" and exit WITHOUT an rc; e() is untouched (R08).
//   * ado `_rmcoll rhs if touse, expand` then `qreg y rhss if touse
//     [fweight=_fw], quantile() wls()` under version 11: b is the LP vertex
//     (on a non-unique optimum PIET's own optimal vertex, with a note; Stata's
//     `_qreg` may pick another), e() is qreg's version<=12 set (qregOldVce below), including
//     _qreg's undocumented r(f_r) (qreg2Fr below; measured on StataBE 19.5).
//   * ado: residuals, zero rule, Hall-Sheather bandwidth x MAD (or the
//     Silverman scale), uniform-kernel density weights, the score stored as float,
//     and V = invsym(D) A invsym(D) with A summed over clusters (myopaccum).
//   * ado: a bandwidth outside (0,1) prints "Cannot compute bandwidth; too
//     few observations?" and exits rc 0 with qreg's e() live (R09, R11).
//   * ado: R-squared, objective function, the MSS (no cluster) and PSS
//     (cluster) tests, and the e() qreg2 adds.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "core/distributions.h"
#include "handlers_e3_common.h"
#include "piet/datastore.h"
#include "piet/eval/errors.h"
#include "piet/handlers.h"
#include "piet/session_state.h"
#include "piet/strutil.h"

namespace piet {
namespace handlers {

using e3::ojson;
using e3::rcError;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
const char* const kTouse = "__qreg2_touse";
const char* const kOrder = "__qreg2_order";

std::string lowerQ(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::vector<std::string> splitWsQ(const std::string& s) {
  std::istringstream is(s);
  std::vector<std::string> out;
  for (std::string t; is >> t;) out.push_back(t);
  return out;
}

std::string levelText(double v) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.0f", v);
  return buf;
}

double normalden(double x) { return std::exp(-0.5 * x * x) / std::sqrt(2.0 * M_PI); }

// Upper tail of chi2(df) (regularized upper incomplete gamma Q(df/2, x/2)).
double chi2tailQ(double df, double x) {
  if (!(x > 0)) return 1.0;
  const double a = df / 2.0, z = x / 2.0;
  const double gln = std::lgamma(a);
  if (z < a + 1.0) {
    double ap = a, sum = 1.0 / a, del = sum;
    for (int n = 0; n < 1000; ++n) {
      ap += 1.0;
      del *= z / ap;
      sum += del;
      if (std::fabs(del) < std::fabs(sum) * 1e-16) break;
    }
    return 1.0 - sum * std::exp(-z + a * std::log(z) - gln);
  }
  double b = z + 1.0 - a, c = 1.0 / 1e-300, d = 1.0 / b, h = d;
  for (int i = 1; i < 1000; ++i) {
    const double an = -i * (i - a);
    b += 2.0;
    d = an * d + b;
    if (std::fabs(d) < 1e-300) d = 1e-300;
    c = b + an / c;
    if (std::fabs(c) < 1e-300) c = 1e-300;
    d = 1.0 / d;
    const double del = d * c;
    h *= del;
    if (std::fabs(del - 1.0) < 1e-16) break;
  }
  return std::exp(-z + a * std::log(z) - gln) * h;
}

// ---------------------------------------------------------------------------
// The design: Stata's `fvexpand` / `_rmcoll ..., expand` column list.

struct FvAtom {
  std::string var;
  bool factor = false;
  bool hasBase = true;  // i. / ib#.: a base level exists (ibn. has none)
  double base = kNaN;   // ib#.: the requested base level; NaN = the smallest
};
struct FvTerm {
  std::vector<FvAtom> atoms;  // one = a main effect, more = an interaction
};
struct DesignCol {
  std::string name;
  size_t term = 0;
  std::vector<double> lev;  // per atom: the factor level (NaN for a continuous atom)
  int kind = 0;             // 0 estimable, 1 base, 2 omitted (collinear or empty cell)
};

std::string termKey(const std::vector<FvAtom>& atoms) {
  std::vector<std::string> k;
  for (const auto& a : atoms) k.push_back((a.factor ? "i." : "c.") + a.var);
  std::sort(k.begin(), k.end());
  std::string s;
  for (const auto& x : k) s += x + "#";
  return s;
}

bool resolveVarQ(Datastore& ds, const std::string& raw, std::string& out, ojson& err) {
  if (ds.has(raw) && !isHiddenColumn(raw)) {
    out = raw;
    return true;
  }
  std::string hit;
  int hits = 0;
  for (const auto& n : ds.names()) {
    if (isHiddenColumn(n)) continue;
    if (n.size() > raw.size() && n.compare(0, raw.size(), raw) == 0) {
      hit = n;
      ++hits;
    }
  }
  if (hits == 1) {
    out = hit;
    return true;
  }
  err = rcError(hits > 1 ? raw + " ambiguous abbreviation" : "variable " + raw + " not found", 111);
  return false;
}

// One `op.var` atom: i. ib#. ibn. c. or a bare name (`bareFactor` inside `#`).
bool parseAtom(Datastore& ds, const std::string& raw, bool bareFactor, FvAtom& a, ojson& err) {
  const size_t dot = raw.find('.');
  std::string name = raw;
  a.factor = bareFactor;
  a.hasBase = true;
  if (dot != std::string::npos) {
    const std::string op = lowerQ(raw.substr(0, dot));
    name = raw.substr(dot + 1);
    if (op == "c") {
      a.factor = false;
    } else if (op == "i") {
      a.factor = true;
    } else if (op == "ibn") {
      a.factor = true;
      a.hasBase = false;
    } else if (op.size() > 2 && op.compare(0, 2, "ib") == 0 &&
               op.find_first_not_of("0123456789", 2) == std::string::npos) {
      a.factor = true;
      a.base = std::strtod(op.c_str() + 2, nullptr);
    } else {
      err = rcError("qreg2: the factor-variable operator `" + op +
                        ".` is not implemented in this version of PIET; nothing was estimated",
                    198, "unsupported_option");
      return false;
    }
  }
  if (name.empty() || name.find_first_of("()*?~-.#") != std::string::npos) {
    err = rcError("qreg2: the varlist term `" + raw +
                      "` is not implemented in this version of PIET; nothing was estimated",
                  198, "unsupported_option");
    return false;
  }
  if (!resolveVarQ(ds, name, a.var, err)) return false;
  if (ds.dtype(a.var) != "f64") {
    err = rcError("string variables not allowed in varlist", 109);
    return false;
  }
  return true;
}

// A varlist token -> terms (in Stata's fvexpand order).
bool parseTermToken(Datastore& ds, SessionState& session, const std::string& tok,
                    std::vector<FvTerm>& out, ojson& err) {
  if (tok.find('#') != std::string::npos) {
    const bool full = tok.find("##") != std::string::npos;
    std::vector<std::string> parts;
    size_t p = 0;
    while (true) {
      const size_t q = tok.find('#', p);
      parts.push_back(tok.substr(p, q == std::string::npos ? std::string::npos : q - p));
      if (q == std::string::npos) break;
      p = q + 1;
      if (full) {
        if (p >= tok.size() || tok[p] != '#') {
          err = rcError("qreg2: a mixed `#`/`##` interaction is not implemented in this version "
                        "of PIET; nothing was estimated",
                        198, "unsupported_option");
          return false;
        }
        ++p;
      }
    }
    std::vector<FvAtom> atoms;
    for (const auto& part : parts) {
      FvAtom a;
      if (part.empty()) {
        err = rcError("invalid syntax", 198);
        return false;
      }
      if (!parseAtom(ds, part, true, a, err)) return false;
      atoms.push_back(a);
    }
    std::vector<std::vector<FvAtom>> terms;
    if (!full) {
      terms.push_back(atoms);
    } else {
      for (const auto& a : atoms) {
        const size_t n0 = terms.size();
        terms.push_back({a});
        for (size_t i = 0; i < n0; ++i) {
          std::vector<FvAtom> c = terms[i];
          c.push_back(a);
          terms.push_back(std::move(c));
        }
      }
    }
    for (auto& t : terms) out.push_back(FvTerm{t});
    return true;
  }
  if (tok.find('.') != std::string::npos) {
    FvAtom a;
    if (!parseAtom(ds, tok, false, a, err)) return false;
    out.push_back(FvTerm{{a}});
    return true;
  }
  // A plain name, wildcard or range: `syntax varlist`'s own expansion.
  std::vector<std::string> names;
  if (!e3::expandVars(tok, session, &names, &err)) return false;
  for (const auto& nm : names) {
    if (ds.dtype(nm) != "f64") {
      err = rcError("string variables not allowed in varlist", 109);
      return false;
    }
    FvAtom a;
    a.var = nm;
    a.factor = false;
    out.push_back(FvTerm{{a}});
  }
  return true;
}

// The value of one design column in row r.
double colValue(const DesignCol& c, const std::vector<FvTerm>& terms,
                const std::map<std::string, std::vector<double>>& data, size_t r) {
  const FvTerm& t = terms[c.term];
  double v = 1.0;
  for (size_t a = 0; a < t.atoms.size(); ++a) {
    const double x = data.at(t.atoms[a].var)[r];
    if (t.atoms[a].factor) {
      if (x != c.lev[a]) return 0.0;
    } else {
      v *= x;
    }
  }
  return v;
}

// ---------------------------------------------------------------------------
// Small dense linear algebra (row-major).

// Gauss-Jordan inverse with partial pivoting; false when singular.
bool invertGJ(std::vector<double> A, size_t p, std::vector<double>& inv) {
  inv.assign(p * p, 0.0);
  for (size_t i = 0; i < p; ++i) inv[i * p + i] = 1.0;
  for (size_t c = 0; c < p; ++c) {
    size_t piv = c;
    double best = std::fabs(A[c * p + c]);
    for (size_t r = c + 1; r < p; ++r)
      if (std::fabs(A[r * p + c]) > best) {
        best = std::fabs(A[r * p + c]);
        piv = r;
      }
    if (!(best > 0)) return false;
    if (piv != c) {
      for (size_t j = 0; j < p; ++j) {
        std::swap(A[c * p + j], A[piv * p + j]);
        std::swap(inv[c * p + j], inv[piv * p + j]);
      }
    }
    const double d = A[c * p + c];
    for (size_t j = 0; j < p; ++j) {
      A[c * p + j] /= d;
      inv[c * p + j] /= d;
    }
    for (size_t r = 0; r < p; ++r) {
      if (r == c) continue;
      const double f = A[r * p + c];
      if (f == 0.0) continue;
      for (size_t j = 0; j < p; ++j) {
        A[r * p + j] -= f * A[c * p + j];
        inv[r * p + j] -= f * inv[c * p + j];
      }
    }
  }
  return true;
}

// Solve A x = b by LU with partial pivoting and one refinement step.
bool solveLU(const std::vector<double>& A0, size_t p, const std::vector<double>& b0,
             std::vector<double>& x) {
  std::vector<double> A = A0;
  std::vector<size_t> perm(p);
  std::iota(perm.begin(), perm.end(), 0);
  for (size_t c = 0; c < p; ++c) {
    size_t piv = c;
    double best = std::fabs(A[c * p + c]);
    for (size_t r = c + 1; r < p; ++r)
      if (std::fabs(A[r * p + c]) > best) {
        best = std::fabs(A[r * p + c]);
        piv = r;
      }
    if (!(best > 0)) return false;
    if (piv != c) {
      for (size_t j = 0; j < p; ++j) std::swap(A[c * p + j], A[piv * p + j]);
      std::swap(perm[c], perm[piv]);
    }
    for (size_t r = c + 1; r < p; ++r) {
      const double f = A[r * p + c] / A[c * p + c];
      A[r * p + c] = f;
      if (f == 0.0) continue;
      for (size_t j = c + 1; j < p; ++j) A[r * p + j] -= f * A[c * p + j];
    }
  }
  auto luSolve = [&](const std::vector<double>& rhs, std::vector<double>& out) {
    std::vector<double> z(p);
    for (size_t i = 0; i < p; ++i) {
      double s = rhs[perm[i]];
      for (size_t j = 0; j < i; ++j) s -= A[i * p + j] * z[j];
      z[i] = s;
    }
    out.assign(p, 0.0);
    for (size_t ii = p; ii-- > 0;) {
      double s = z[ii];
      for (size_t j = ii + 1; j < p; ++j) s -= A[ii * p + j] * out[j];
      out[ii] = s / A[ii * p + ii];
    }
  };
  luSolve(b0, x);
  std::vector<double> r(p), dx;
  for (size_t i = 0; i < p; ++i) {
    double s = b0[i], c = 0;
    for (size_t j = 0; j < p; ++j) {
      // compensated residual b - A x
      const double prod = A0[i * p + j] * x[j];
      const double perr = std::fma(A0[i * p + j], x[j], -prod);
      const double t = s - prod;
      c += (std::fabs(s) >= std::fabs(prod) ? (s - t) - prod : (-prod - t) + s) - perr;
      s = t;
    }
    r[i] = s + c;
  }
  luSolve(r, dx);
  for (size_t i = 0; i < p; ++i) x[i] += dx[i];
  return true;
}

// ---------------------------------------------------------------------------
// The quantile-regression LP, solved to its vertex (what _qreg's simplex
// returns): min sum_i w_i rho_tau(y_i - x_i'b). A basis is p observations with
// zero residual; each pivot moves along the edge that frees one of them, in the
// direction of steepest descent, to the breakpoint where the slope turns
// nonnegative (the weighted-median line search of Barrodale-Roberts /
// Koenker-d'Orey). Every pivot strictly lowers the objective, so it ends.
struct LpResult {
  std::vector<double> b;
  std::vector<size_t> basis;
  bool ok = false;
  bool unique = true;
  std::string why;
};

LpResult solveQuantileLp(const std::vector<double>& Xr, size_t n, size_t p,
                         const std::vector<double>& y, const std::vector<double>& w, double tau) {
  LpResult out;
  double ymax = 0;
  for (double v : y) ymax = std::max(ymax, std::fabs(v));
  const double tolR = 1e-11 * (1.0 + ymax);
  // ---- starting basis: weighted OLS residuals, smallest first, independent rows
  std::vector<double> b(p, 0.0);
  {
    std::vector<double> XtX(p * p, 0.0), Xty(p, 0.0);
    for (size_t i = 0; i < n; ++i) {
      const double* xi = &Xr[i * p];
      for (size_t a = 0; a < p; ++a) {
        if (xi[a] == 0.0) continue;
        Xty[a] += w[i] * xi[a] * y[i];
        for (size_t c = 0; c < p; ++c) XtX[a * p + c] += w[i] * xi[a] * xi[c];
      }
    }
    if (!solveLU(XtX, p, Xty, b)) b.assign(p, 0.0);
  }
  std::vector<double> r(n);
  for (size_t i = 0; i < n; ++i) {
    double s = 0;
    for (size_t a = 0; a < p; ++a) s += Xr[i * p + a] * b[a];
    r[i] = y[i] - s;
  }
  std::vector<size_t> order(n);
  std::iota(order.begin(), order.end(), 0);
  std::stable_sort(order.begin(), order.end(),
                   [&r](size_t a, size_t c) { return std::fabs(r[a]) < std::fabs(r[c]); });
  std::vector<size_t> basis;
  {
    std::vector<std::vector<double>> Q;
    for (size_t i : order) {
      if (basis.size() == p) break;
      std::vector<double> v(Xr.begin() + static_cast<std::ptrdiff_t>(i * p),
                            Xr.begin() + static_cast<std::ptrdiff_t>(i * p + p));
      double n0 = 0;
      for (double x : v) n0 += x * x;
      n0 = std::sqrt(n0);
      if (!(n0 > 0)) continue;
      for (int pass = 0; pass < 2; ++pass)
        for (const auto& q : Q) {
          double d = 0;
          for (size_t a = 0; a < p; ++a) d += q[a] * v[a];
          for (size_t a = 0; a < p; ++a) v[a] -= d * q[a];
        }
      double nv = 0;
      for (double x : v) nv += x * x;
      nv = std::sqrt(nv);
      if (nv > 1e-9 * n0) {
        for (auto& x : v) x /= nv;
        Q.push_back(std::move(v));
        basis.push_back(i);
      }
    }
  }
  if (basis.size() != p) {
    out.why = "the design is not of full column rank";
    return out;
  }
  std::vector<char> inB(n, 0);
  for (size_t i : basis) inB[i] = 1;
  std::vector<double> Binv;
  auto refactor = [&]() {
    std::vector<double> XB(p * p);
    for (size_t j = 0; j < p; ++j)
      for (size_t a = 0; a < p; ++a) XB[j * p + a] = Xr[basis[j] * p + a];
    return invertGJ(XB, p, Binv);
  };
  if (!refactor()) {
    out.why = "a singular basis";
    return out;
  }
  const size_t maxIter = 50 * (n + p) + 1000;
  size_t sinceRefactor = 0;
  std::vector<double> col(p), g(p), S(p), Zp(p), Zm(p), m(p), z(n, 0.0);
  // A degenerate vertex (non-basic rows at zero residual too) has more edges than
  // the basis's 2p: any p-1 of its zero-residual rows fix a direction d along which
  // their residuals stay 0. When none of the basis's own edges descends, the
  // vertex is optimal only if none of these does either (BUG-1040: on the
  // w2p_estim_ev fixture, tau .45 stopped at a vertex 0.019% above StataBE's
  // optimum). The steepest descending edge is walked to its breakpoint like any
  // pivot, so the loss strictly falls; false = no edge descends (or too many
  // subsets to enumerate, when the vertex is kept as before).
  auto escapeDegenerate = [&]() -> bool {
    std::vector<size_t> Zr(basis.begin(), basis.end());
    for (size_t i = 0; i < n; ++i)
      if (!inB[i] && std::fabs(r[i]) <= tolR) Zr.push_back(i);
    const size_t k = Zr.size(), q = p - 1;
    if (k <= p) return false;
    double subsets = 1;
    for (size_t t = 0; t < q; ++t) subsets = subsets * static_cast<double>(k - t) / static_cast<double>(t + 1);
    if (subsets > 20000) return false;
    std::vector<char> inP(n, 0);
    std::vector<size_t> pick(q);
    std::iota(pick.begin(), pick.end(), 0);
    double best = 0;
    std::vector<size_t> bestP;
    std::vector<double> bestD, d(p), zz(n);
    for (;;) {
      // d: the null direction of the picked rows (Gram-Schmidt on them, then the
      // unit vector with the largest remainder).
      std::vector<std::vector<double>> Q;
      bool independent = true;
      for (size_t t = 0; t < q && independent; ++t) {
        std::vector<double> v(Xr.begin() + static_cast<std::ptrdiff_t>(Zr[pick[t]] * p),
                              Xr.begin() + static_cast<std::ptrdiff_t>(Zr[pick[t]] * p + p));
        double n0 = 0;
        for (double x : v) n0 += x * x;
        for (int pass = 0; pass < 2; ++pass)
          for (const auto& qq : Q) {
            double dt = 0;
            for (size_t a = 0; a < p; ++a) dt += qq[a] * v[a];
            for (size_t a = 0; a < p; ++a) v[a] -= dt * qq[a];
          }
        double nv = 0;
        for (double x : v) nv += x * x;
        if (!(nv > 1e-18 * n0) || !(n0 > 0)) {
          independent = false;
          break;
        }
        nv = std::sqrt(nv);
        for (auto& x : v) x /= nv;
        Q.push_back(std::move(v));
      }
      if (independent) {
        double dn = -1;
        for (size_t a = 0; a < p; ++a) {
          std::vector<double> e(p, 0.0);
          e[a] = 1.0;
          for (int pass = 0; pass < 2; ++pass)
            for (const auto& qq : Q) {
              double s2 = 0;
              for (size_t c = 0; c < p; ++c) s2 += qq[c] * e[c];
              for (size_t c = 0; c < p; ++c) e[c] -= s2 * qq[c];
            }
          double ne = 0;
          for (double x : e) ne += x * x;
          if (ne > dn) {
            dn = ne;
            d = e;
          }
        }
        dn = std::sqrt(dn);
        for (auto& x : d) x /= dn;
        for (size_t t = 0; t < q; ++t) inP[Zr[pick[t]]] = 1;
        for (double sign : {1.0, -1.0}) {
          double D = 0, scale = 0;
          for (size_t i = 0; i < n; ++i) {
            if (inP[i]) continue;
            double s2 = 0;
            for (size_t a = 0; a < p; ++a) s2 += Xr[i * p + a] * d[a];
            const double zi = -sign * s2;  // the residual's rate along sign*d
            const double rr = inB[i] ? 0.0 : r[i];
            const double psi = std::fabs(rr) > tolR ? w[i] * (rr > 0 ? tau : tau - 1.0)
                                                    : w[i] * (zi > 0 ? tau : tau - 1.0);
            D += psi * zi;
            scale += w[i] * std::fabs(zi);
          }
          if (D < -1e-10 * scale && D < best) {
            best = D;
            bestP.assign(pick.size(), 0);
            for (size_t t = 0; t < q; ++t) bestP[t] = Zr[pick[t]];
            bestD = d;
            for (auto& x : bestD) x *= sign;
          }
        }
        for (size_t t = 0; t < q; ++t) inP[Zr[pick[t]]] = 0;
      }
      // the next q-subset of 0..k-1
      size_t t = q;
      while (t > 0 && pick[t - 1] == k - q + (t - 1)) --t;
      if (t == 0) break;
      ++pick[t - 1];
      for (size_t u = t; u < q; ++u) pick[u] = pick[u - 1] + 1;
    }
    if (bestP.empty()) return false;
    // walk the edge to the breakpoint where the slope turns nonnegative
    std::vector<char> keep(n, 0);
    for (size_t i : bestP) keep[i] = 1;
    std::vector<std::pair<double, size_t>> bps;
    for (size_t i = 0; i < n; ++i) {
      if (keep[i]) continue;
      double s2 = 0;
      for (size_t a = 0; a < p; ++a) s2 += Xr[i * p + a] * bestD[a];
      zz[i] = -s2;
      const double rr = inB[i] ? 0.0 : r[i];
      if (std::fabs(rr) > tolR && rr * zz[i] < 0) bps.emplace_back(-rr / zz[i], i);
    }
    std::sort(bps.begin(), bps.end());
    double slope = best;
    size_t enterRow = n;
    for (const auto& [t, i] : bps) {
      slope += w[i] * std::fabs(zz[i]);
      if (slope >= 0) {
        enterRow = i;
        break;
      }
    }
    if (enterRow == n) return false;
    std::fill(inB.begin(), inB.end(), 0);
    basis = bestP;
    basis.push_back(enterRow);
    for (size_t i : basis) inB[i] = 1;
    return true;
  };
  for (size_t iter = 0;; ++iter) {
    if (iter > maxIter) {
      out.why = "the simplex did not converge";
      return out;
    }
    // b = X_B^{-1} y_B and the residuals
    for (size_t a = 0; a < p; ++a) {
      double s = 0;
      for (size_t j = 0; j < p; ++j) s += Binv[a * p + j] * y[basis[j]];
      b[a] = s;
    }
    for (size_t i = 0; i < n; ++i) {
      if (inB[i]) {
        r[i] = 0;
        continue;
      }
      double s = 0;
      for (size_t a = 0; a < p; ++a) s += Xr[i * p + a] * b[a];
      r[i] = y[i] - s;
    }
    // directional derivatives of the 2p edges
    std::fill(g.begin(), g.end(), 0.0);
    std::fill(Zp.begin(), Zp.end(), 0.0);
    std::fill(Zm.begin(), Zm.end(), 0.0);
    for (size_t i = 0; i < n; ++i) {
      if (inB[i]) continue;
      const double* xi = &Xr[i * p];
      if (std::fabs(r[i]) > tolR) {
        const double psi = w[i] * (r[i] > 0 ? tau : tau - 1.0);
        for (size_t a = 0; a < p; ++a) g[a] += psi * xi[a];
      } else {
        for (size_t j = 0; j < p; ++j) {
          double s = 0;
          for (size_t a = 0; a < p; ++a) s += xi[a] * Binv[a * p + j];
          m[j] = s;
        }
        for (size_t j = 0; j < p; ++j) {
          const double am = std::fabs(m[j]);
          Zp[j] += w[i] * (m[j] > 0 ? tau : 1.0 - tau) * am;
          Zm[j] += w[i] * (m[j] < 0 ? tau : 1.0 - tau) * am;
        }
      }
    }
    for (size_t j = 0; j < p; ++j) {
      double s = 0;
      for (size_t a = 0; a < p; ++a) s += g[a] * Binv[a * p + j];
      S[j] = s;
    }
    size_t jBest = p;
    double sigma = 0, dBest = 0;
    bool flatEdge = false;
    for (size_t j = 0; j < p; ++j) {
      const double wj = w[basis[j]];
      const double dp = S[j] + Zp[j] + wj * tau;
      const double dm = -S[j] + Zm[j] + wj * (1.0 - tau);
      const double tol = 1e-10 * (std::fabs(S[j]) + Zp[j] + Zm[j] + wj);
      if (std::fabs(dp) <= tol || std::fabs(dm) <= tol) flatEdge = true;
      if (dp < -tol && dp < dBest) {
        dBest = dp;
        jBest = j;
        sigma = 1.0;
      }
      if (dm < -tol && dm < dBest) {
        dBest = dm;
        jBest = j;
        sigma = -1.0;
      }
    }
    if (jBest == p) {
      if (sinceRefactor == 0) {
        if (escapeDegenerate()) {
          if (!refactor()) {
            out.why = "a singular basis";
            return out;
          }
          continue;
        }
        // At an optimum a zero one-sided derivative leaves an interval of
        // equally good vertices. Its b and residual-dependent covariance need
        // the original solver's tie rule; an arbitrary vertex is insufficient.
        out.unique = !flatEdge;
        break;  // optimal on a fresh inverse
      }
      if (!refactor()) {
        out.why = "a singular basis";
        return out;
      }
      sinceRefactor = 0;
      continue;
    }
    // the edge: residual changes z_i = sigma * x_i' Binv e_j
    for (size_t a = 0; a < p; ++a) col[a] = Binv[a * p + jBest];
    std::vector<std::pair<double, size_t>> bp;
    for (size_t i = 0; i < n; ++i) {
      if (inB[i]) continue;
      double s = 0;
      for (size_t a = 0; a < p; ++a) s += Xr[i * p + a] * col[a];
      z[i] = sigma * s;
      if (std::fabs(r[i]) > tolR && r[i] * z[i] < 0) bp.emplace_back(-r[i] / z[i], i);
    }
    std::sort(bp.begin(), bp.end());
    double slope = dBest;
    size_t enter = n;
    for (const auto& [t, i] : bp) {
      slope += w[i] * std::fabs(z[i]);
      if (slope >= 0) {
        enter = i;
        break;
      }
    }
    if (enter == n) {
      if (sinceRefactor > 0 && refactor()) {
        sinceRefactor = 0;
        continue;
      }
      out.why = "the LP is unbounded along an edge";
      return out;
    }
    // Sherman-Morrison: row jBest of X_B becomes x_enter.
    {
      std::vector<double> mk(p);
      for (size_t j = 0; j < p; ++j) {
        double s = 0;
        for (size_t a = 0; a < p; ++a) s += Xr[enter * p + a] * Binv[a * p + j];
        mk[j] = s;
      }
      const double piv = mk[jBest];
      inB[basis[jBest]] = 0;
      basis[jBest] = enter;
      inB[enter] = 1;
      if (std::fabs(piv) < 1e-12 || ++sinceRefactor >= 32) {
        if (!refactor()) {
          out.why = "a singular basis";
          return out;
        }
        sinceRefactor = 0;
      } else {
        mk[jBest] -= 1.0;
        for (size_t a = 0; a < p; ++a) {
          const double f = col[a] / piv;
          if (f == 0.0) continue;
          for (size_t j = 0; j < p; ++j) Binv[a * p + j] -= f * mk[j];
        }
      }
    }
  }
  // The vertex itself, solved directly.
  std::vector<double> XB(p * p), yB(p);
  for (size_t j = 0; j < p; ++j) {
    yB[j] = y[basis[j]];
    for (size_t a = 0; a < p; ++a) XB[j * p + a] = Xr[basis[j] * p + a];
  }
  if (!solveLU(XB, p, yB, out.b)) {
    out.why = "a singular basis";
    return out;
  }
  out.basis = basis;
  out.ok = true;
  return out;
}

// ---------------------------------------------------------------------------
// summarize's percentiles ([R] summarize Methods and formulas): P = W p/100 over
// the cumulative weights W(i) of the sorted values; the first i with W(i) > P,
// the average with the previous value when W(i-1) == P.
double pctileW(const std::vector<std::pair<double, double>>& sortedVW, double pct) {
  double W = 0;
  for (const auto& vw : sortedVW) W += vw.second;
  const double P = W * pct / 100.0;
  double cum = 0;
  for (size_t i = 0; i < sortedVW.size(); ++i) {
    const double prev = cum;
    cum += sortedVW[i].second;
    if (cum > P) {
      if (prev == P && i > 0) return (sortedVW[i - 1].first + sortedVW[i].first) / 2.0;
      return sortedVW[i].first;
    }
  }
  return sortedVW.empty() ? kNaN : sortedVW.back().first;
}

// _qreg's r(f_r), the version<=12 "residual density estimate". Measured on
// StataBE 19.5 (impl/qreg2/stata p01-p07: 245 fits, unweighted and fweighted,
// k = 1..21, n = 5..500, q = .05..9): with N = sum w, s = sqrt(N),
//   lo = N q - s + 1,  hi = N q + s,
//   c  = (min(hi, N) - max(lo, 1) - (k - 1)) / N     (k = basis observations),
//   f_r = c / (r[ih] - r[il]),
// where r is the sorted residuals (fweights as frequencies), il the observation
// holding position max(floor(lo), 1) and ih the one holding min(ceil(hi), N). When
// the two order statistics coincide inside the basis-zero block, Stata may use
// neighbouring nonzero residuals or distinguish the ends by solver roundoff
// (w2j F01, -6.5e13). This LP does not reproduce that numerical tie rule, so the
// collapsed window is refused before any e() results are posted.
double qreg2Fr(const std::vector<double>& resid, const std::vector<double>& w,
               const std::vector<char>& isZero, double q, double k) {
  const size_t n = resid.size();
  std::vector<size_t> idx(n);
  std::iota(idx.begin(), idx.end(), 0);
  auto val = [&](size_t i) { return isZero[i] ? 0.0 : resid[i]; };
  std::stable_sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return val(a) < val(b); });
  double N = 0;
  for (double x : w) N += x;
  const double s = std::sqrt(N);
  const double lo = N * q - s + 1.0, hi = N * q + s;
  const double c = (std::min(hi, N) - std::max(lo, 1.0) - (k - 1.0)) / N;
  const double pl = std::max(std::floor(lo), 1.0), ph = std::min(std::ceil(hi), N);
  auto rowAt = [&](double pos) {
    double cum = 0;
    for (size_t t = 0; t < n; ++t) {
      cum += w[idx[t]];
      if (cum >= pos) return t;
    }
    return n - 1;
  };
  size_t I = rowAt(pl), J = rowAt(ph);
  double vI = val(idx[I]), vJ = val(idx[J]);
  if (vI == vJ) return kNaN;
  return c / (vJ - vI);
}


// ---------------------------------------------------------------------------
// The front half qreg2 and the native qreg share: the varlist (depvar, then the
// fv terms), the estimation sample, Stata's fvexpand/_rmcoll design and the
// estimable design matrix.
bool qrFail(ojson& err, ojson e) {
  err = std::move(e);
  return false;
}

bool qrParse(const eval::ParsedCommand& parsed, SessionState& session, std::string& depvar,
             std::vector<FvTerm>& terms, ojson& err) {
  Datastore* ds = session.datastore();
  std::string joined;
  for (const auto& t : parsed.varlist) joined += (joined.empty() ? "" : " ") + t;
  std::vector<std::string> toks = splitWsQ(joined);
  if (toks.empty()) return qrFail(err, rcError("varlist required", 100));
  {
    if (toks[0].find_first_of(".#") != std::string::npos)
      return qrFail(err, rcError("depvar may not be a factor variable", 198));
    std::vector<std::string> dv;
    if (!e3::expandVars(toks[0], session, &dv, &err)) return qrFail(err, err);
    if (dv.empty()) return qrFail(err, rcError("varlist required", 100));
    depvar = dv[0];
    toks.erase(toks.begin());
    if (dv.size() > 1) toks.insert(toks.begin(), dv.begin() + 1, dv.end());
    if (ds->dtype(depvar) != "f64") return qrFail(err, rcError("string variables not allowed in varlist", 109));
  }
  for (const auto& t : toks)
    if (!parseTermToken(*ds, session, t, terms, err)) return qrFail(err, err);

  return true;
}

struct QrData {
  std::map<std::string, std::vector<double>> data;
  std::vector<DesignCol> cols;
  std::vector<std::string> names;
  std::vector<int> estIdx;  // K: index into the estimable design, -1 for base/omitted
  size_t p = 0, K = 0, n = 0, Nall = 0;
  bool fvops = false;
  std::vector<size_t> rows;
  std::vector<double> X, Y, W;
  double N = 0;
};

// extraOk: per-row markout (qreg2's cluster), empty = none; extraVars are read
// into data as well (qreg2's mss()).
bool qrSample(const eval::ParsedCommand& parsed, SessionState& session, const std::string& depvar,
              const std::vector<FvTerm>& terms, const std::string& wvar,
              const std::vector<std::string>& extraVars, const std::vector<char>& extraOk,
              QrData& d, ojson& err, bool frequencyWeights = true) {
  const bool weighted = !wvar.empty();

  std::string qual;
  if (parsed.ifExpr.has_value()) qual += " if " + *parsed.ifExpr;
  if (parsed.inRange.has_value()) qual += " in " + *parsed.inRange;
  if (!qual.empty() &&
      !e3::runLine(std::string("quietly generate double ") + kTouse + " = 1" + qual, session, &err))
    return false;
  Datastore* ds = session.datastore();
  const size_t Nall = static_cast<size_t>(ds->nRows());
  d.Nall = Nall;
  auto column = [&](const std::string& name) {
    const double* p = ds->col(name).f64();
    return std::vector<double>(p, p + Nall);
  };
  std::vector<char> touse(Nall, 1);
  if (!qual.empty()) {
    const std::vector<double> t = column(kTouse);
    for (size_t i = 0; i < Nall; ++i) touse[i] = t[i] == 1.0;
    SessionState::TempDropScope td(session);
    if (!e3::runLine(std::string("quietly drop ") + kTouse, session, &err)) return false;
    ds = session.datastore();
  }
  auto& data = d.data;
  data[depvar] = column(depvar);
  for (const auto& t : terms)
    for (const auto& a : t.atoms)
      if (!data.count(a.var)) data[a.var] = column(a.var);
  for (const auto& v : extraVars)
    if (!data.count(v)) data[v] = column(v);
  std::vector<double> fw(Nall, 1.0);
  if (weighted) fw = column(wvar);
  for (size_t i = 0; i < Nall; ++i) {
    if (!touse[i]) continue;
    bool ok = !std::isnan(data[depvar][i]);
    for (const auto& t : terms)
      for (const auto& a : t.atoms) ok = ok && !std::isnan(data[a.var][i]);
    if (weighted) ok = ok && !std::isnan(fw[i]) && fw[i] != 0.0;
    if (!extraOk.empty()) ok = ok && extraOk[i];
    touse[i] = ok;
  }
  for (size_t i = 0; i < Nall; ++i) {
    if (!touse[i]) continue;
    if (weighted) {
      if (fw[i] < 0) return qrFail(err, rcError("negative weights encountered", 402));
      if (frequencyWeights && fw[i] != std::floor(fw[i]))
        return qrFail(err, rcError("may not use noninteger frequency weights", 401));
    }
    for (const auto& t : terms)
      for (const auto& a : t.atoms) {
        if (!a.factor) continue;
        const double v = data[a.var][i];
        if (v < 0) return qrFail(err, rcError(a.var + ":  factor variables may not contain negative values", 452));
        if (v != std::floor(v))
          return qrFail(err, rcError(a.var + ":  factor variables may not contain noninteger values", 452));
      }
  }
  auto& rows = d.rows;
  for (size_t i = 0; i < Nall; ++i)
    if (touse[i]) rows.push_back(i);
  const size_t n = rows.size();
  d.n = n;
  if (n == 0) return qrFail(err, rcError("no observations", 2000));
  if (n <= 2) return qrFail(err, rcError("insufficient observations", 2001));

  // ---- fvexpand + _rmcoll, expand (ado) ----------------------------------------
  std::map<std::string, std::vector<double>> levels;  // factor -> sorted levels in sample
  std::map<std::string, double> baseOf;
  for (const auto& t : terms)
    for (const auto& a : t.atoms) {
      if (!a.factor || levels.count(a.var)) continue;
      std::set<double> s;
      for (size_t r : rows) s.insert(data[a.var][r]);
      levels[a.var] = std::vector<double>(s.begin(), s.end());
    }
  for (const auto& t : terms)
    for (const auto& a : t.atoms) {
      if (!a.factor || baseOf.count(a.var)) continue;
      double base = kNaN;
      if (a.hasBase) {
        const auto& L = levels[a.var];
        base = L.front();
        if (!std::isnan(a.base) && std::find(L.begin(), L.end(), a.base) != L.end()) base = a.base;
      }
      baseOf[a.var] = base;
    }
  std::set<std::string> present;
  for (const auto& t : terms) present.insert(termKey(t.atoms));
  auto& cols = d.cols;
  {
    std::set<std::string> seenTerm;
    for (size_t ti = 0; ti < terms.size(); ++ti) {
      const FvTerm& t = terms[ti];
      const std::string key = termKey(t.atoms);
      if (!seenTerm.insert(key).second) continue;  // a repeated term (`x x`, `a##b a`)
      // all level combinations, the first atom slowest
      std::vector<std::vector<double>> combos{{}};
      for (const auto& a : t.atoms) {
        std::vector<std::vector<double>> next;
        for (const auto& c : combos) {
          if (a.factor) {
            for (double L : levels[a.var]) {
              auto d = c;
              d.push_back(L);
              next.push_back(std::move(d));
            }
          } else {
            auto d = c;
            d.push_back(kNaN);
            next.push_back(std::move(d));
          }
        }
        combos = std::move(next);
      }
      for (const auto& combo : combos) {
        DesignCol dc;
        dc.term = ti;
        dc.lev = combo;
        bool isBaseCell = false;
        bool allBase = true;
        for (size_t a = 0; a < t.atoms.size(); ++a) {
          const FvAtom& at = t.atoms[a];
          allBase = allBase && at.factor && !std::isnan(baseOf[at.var]) &&
                    combo[a] == baseOf[at.var];
          if (!at.factor || std::isnan(baseOf[at.var]) || combo[a] != baseOf[at.var]) continue;
          std::vector<FvAtom> sub;
          for (size_t c = 0; c < t.atoms.size(); ++c)
            if (c != a) sub.push_back(t.atoms[c]);
          if (sub.empty() || present.count(termKey(sub))) isBaseCell = true;
        }
        // A pure factor interaction still has one reference cell when no
        // lower-order effects are written: its all-base cell aliases _cons.
        // Stata keeps the other cells, including those with one base atom.
        isBaseCell = isBaseCell || allBase;
        bool empty = false;
        if (t.atoms.size() > 1) {
          empty = true;
          for (size_t r : rows) {
            bool hit = true;
            for (size_t a = 0; a < t.atoms.size() && hit; ++a)
              if (t.atoms[a].factor && data[t.atoms[a].var][r] != combo[a]) hit = false;
            if (hit) {
              empty = false;
              break;
            }
          }
        }
        dc.kind = isBaseCell ? 1 : (empty ? 2 : 0);
        std::string nm;
        for (size_t a = 0; a < t.atoms.size(); ++a) {
          const FvAtom& at = t.atoms[a];
          if (a > 0) nm += "#";
          if (!at.factor) {
            nm += (t.atoms.size() > 1 ? "c." : "") + at.var;
            continue;
          }
          const bool atBase = !std::isnan(baseOf[at.var]) && combo[a] == baseOf[at.var];
          nm += levelText(combo[a]) + (atBase ? "b" : (dc.kind != 0 ? "o" : "")) + "." + at.var;
        }
        dc.name = nm;
        cols.push_back(std::move(dc));
      }
    }
  }
  // collinearity, the constant first, then the columns in order (sample rows,
  // unweighted: ado calls _rmcoll without the weights)
  {
    std::vector<std::vector<double>> basisQ;  // orthonormal kept columns over the sample
    basisQ.emplace_back(n, 1.0 / std::sqrt(static_cast<double>(n)));
    for (auto& c : cols) {
      if (c.kind != 0) continue;
      std::vector<double> v(n);
      double n0 = 0;
      for (size_t t = 0; t < n; ++t) {
        v[t] = colValue(c, terms, data, rows[t]);
        n0 += v[t] * v[t];
      }
      if (!(n0 > 0)) {
        c.kind = 2;
        continue;
      }
      for (int pass = 0; pass < 2; ++pass)
        for (const auto& qv : basisQ) {
          double d = 0;
          for (size_t t = 0; t < n; ++t) d += qv[t] * v[t];
          for (size_t t = 0; t < n; ++t) v[t] -= d * qv[t];
        }
      double nv = 0;
      for (double x : v) nv += x * x;
      if (nv <= 1e-13 * n0) {
        c.kind = 2;
        continue;
      }
      nv = std::sqrt(nv);
      for (auto& x : v) x /= nv;
      basisQ.push_back(std::move(v));
    }
    for (auto& c : cols) {
      if (c.kind != 2) continue;
      // an omitted column: `o.x`, `18o.district`, `1o.a#2o.b`
      const FvTerm& t = terms[c.term];
      std::string nm;
      for (size_t a = 0; a < t.atoms.size(); ++a) {
        const FvAtom& at = t.atoms[a];
        if (a > 0) nm += "#";
        if (!at.factor) {
          nm += (t.atoms.size() > 1 ? "co." : "o.") + at.var;
          continue;
        }
        const bool atBase = !std::isnan(baseOf[at.var]) && c.lev[a] == baseOf[at.var];
        nm += levelText(c.lev[a]) + (atBase ? "b" : "o") + "." + at.var;
      }
      c.name = nm;
    }
  }
  auto& names = d.names;
  auto& estIdx = d.estIdx;
  size_t& p = d.p;
  bool& fvops = d.fvops;
  for (const auto& c : cols) {
    names.push_back(c.name);
    if (c.name.find_first_of(".#") != std::string::npos) fvops = true;
    estIdx.push_back(c.kind == 0 ? static_cast<int>(p++) : -1);
  }
  names.push_back("_cons");
  estIdx.push_back(static_cast<int>(p++));
  d.K = names.size();

  // the estimable design over the sample rows (row-major n x p, constant last)
  auto& X = d.X;
  auto& Y = d.Y;
  auto& W = d.W;
  X.assign(n * p, 0.0);
  Y.assign(n, 0.0);
  W.assign(n, 0.0);
  for (size_t t = 0; t < n; ++t) {
    const size_t r = rows[t];
    Y[t] = data[depvar][r];
    W[t] = fw[r];
    for (size_t c = 0; c < cols.size(); ++c)
      if (estIdx[c] >= 0) X[t * p + static_cast<size_t>(estIdx[c])] = colValue(cols[c], terms, data, r);
    X[t * p + p - 1] = 1.0;
  }
  d.N = 0;
  for (double x : W) d.N += x;

  return true;
}

// The covariance, rank and residual degrees of freedom Stata's qreg posts under
// version 12 or earlier (measured on StataBE).
// resid: the n residuals at the LP vertex; fr: e(f_r) (qreg2Fr).
struct QregOldVce {
  std::vector<double> V;   // K x K row-major; base/omitted rows and columns 0
  double rank = kNaN;      // e(rank); posted only with factor-variable notation
  double dfR = kNaN;       // e(df_r)
  double lincomDof = kNaN; // the dof lincom/test use after this posting
};
QregOldVce qregOldVce(const QrData& d, const std::vector<double>& resid, double tau, double fr) {
  const size_t rowCount = d.n, cols = d.p, posted = d.K;

  // A residual this close to zero, relative to the mean absolute residual over the rows
  // (rows, not weighted observations), scores 0 (measured: oracle Q05, Q06).
  double absTotal = 0.0;
  for (double r : resid) absTotal += std::fabs(r);
  const double zeroBelow = 1e-9 * (absTotal / static_cast<double>(rowCount));
  const double aboveScore = tau / fr, belowScore = (tau - 1.0) / fr;

  // bread = sum w x x', meat = sum w psi^2 x x': the weight enters each once (oracle Q04).
  using Matrix = std::vector<std::vector<double>>;
  Matrix bread(cols, std::vector<double>(cols, 0.0)), meat = bread;
  for (size_t row = 0; row < rowCount; ++row) {
    const double* x = d.X.data() + row * cols;
    const double residual = resid[row];
    const double psi = std::fabs(residual) < zeroBelow ? 0.0 : (residual > 0 ? aboveScore : belowScore);
    const double meatWeight = d.W[row] * psi * psi;
    for (size_t a = 0; a < cols; ++a) {
      for (size_t b = 0; b <= a; ++b) {
        const double outer = x[a] * x[b];
        bread[a][b] += d.W[row] * outer;
        meat[a][b] += meatWeight * outer;
      }
    }
  }
  for (size_t a = 0; a < cols; ++a) {
    for (size_t b = 0; b < a; ++b) {
      bread[b][a] = bread[a][b];
      meat[b][a] = meat[a][b];
    }
  }

  // The sandwich inv(bread) meat inv(bread), symmetric by construction: only one triangle
  // is computed and it is copied across when posted.
  const Matrix inverse = e3::invsym(std::move(bread));
  const auto innerProduct = [cols](const std::vector<double>& left, const std::vector<double>& right) {
    double total = 0.0;
    for (size_t c = 0; c < cols; ++c) total += left[c] * right[c];
    return total;
  };
  Matrix meatTimesInverse(cols, std::vector<double>(cols));  // indexed [column of inverse][row of meat]
  for (size_t c = 0; c < cols; ++c)
    for (size_t r = 0; r < cols; ++r) meatTimesInverse[c][r] = innerProduct(meat[r], inverse[c]);

  // Map onto the posted columns: base and omitted ones stay zero rows and columns.
  QregOldVce out;
  out.V.assign(posted * posted, 0.0);
  for (size_t i = 0; i < posted; ++i) {
    if (d.estIdx[i] < 0) continue;
    const size_t ei = static_cast<size_t>(d.estIdx[i]);
    for (size_t j = 0; j <= i; ++j) {
      if (d.estIdx[j] < 0) continue;
      const double entry = innerProduct(inverse[ei], meatTimesInverse[static_cast<size_t>(d.estIdx[j])]);
      out.V[i * posted + j] = out.V[j * posted + i] = entry;
    }
  }

  // Factor-variable notation: e(rank) is posted, e(df_r) follows it, and lincom/test use N.
  // Without it no e(rank), e(df_r) counts the estimable columns, and lincom/test use e(df_r).
  if (d.fvops) {
    out.rank = static_cast<double>(glmVceRank(out.V, posted));
    out.dfR = d.N - out.rank;
    out.lincomDof = d.N;
  } else {
    out.dfR = d.N - static_cast<double>(cols);
    out.lincomDof = out.dfR;
  }
  return out;
}

}  // namespace

// Returns an ARRAY of blocks; the dispatcher pushes each element (never the
// array as one block -- the type_error.306 class).
ojson runQreg2(const eval::ParsedCommand& parsed, SessionState& session) {
  ojson blocks = ojson::array();
  auto fail = [&blocks](ojson e) {
    blocks.push_back(std::move(e));
    return blocks;
  };
  if (!session.hasData()) return fail(ojson(eval::noDataLoaded()));

  // ---- ado syntax ------------------------------------------------------------
  static const std::vector<e3::OptSpec> kSpecs = {
      {"quantile", 1, true}, {"notest", 5, false}, {"mss", 3, true},       {"wlsiter", 3, true},
      {"cluster", 1, true},  {"epsilon", 1, true}, {"silverman", 1, false}};
  std::string cluster, mssText;
  double q = 0.5, epsilon = 1e-7;
  std::string qText = ".5";
  bool notest = false, silverman = false;
  for (const auto& [k, v] : parsed.opts) {
    const int idx = e3::matchOpt(kSpecs, k);
    if (idx < 0) {
      return fail(rcError("option " + lowerQ(k) + (v.isBool ? "" : "()") + " not allowed", 198));
    }
    const e3::OptSpec& s = kSpecs[static_cast<size_t>(idx)];
    if (s.takesArg && v.isBool) return fail(rcError("option " + s.full + "() required", 198));
    const std::string arg = strutil::trim(v.strValue);
    if (s.full == "cluster") {
      cluster = arg;
    } else if (s.full == "quantile") {
      char* end = nullptr;
      q = std::strtod(arg.c_str(), &end);
      if (arg.empty() || end == nullptr || *end != '\0')
        return fail(rcError("option quantile() incorrectly specified", 198));
      qText = arg;
    } else if (s.full == "epsilon") {
      char* end = nullptr;
      epsilon = std::strtod(arg.c_str(), &end);
      if (arg.empty() || end == nullptr || *end != '\0')
        return fail(rcError("option epsilon() incorrectly specified", 198));
    } else if (s.full == "wlsiter") {
      char* end = nullptr;
      const long wl = std::strtol(arg.c_str(), &end, 10);
      if (arg.empty() || end == nullptr || *end != '\0')
        return fail(rcError("option wlsiter() incorrectly specified", 198));
      if (wl < 1) return fail(rcError("wlsiter(" + arg + ") must be a positive integer", 198));
    } else if (s.full == "notest") {
      notest = true;
    } else if (s.full == "silverman") {
      silverman = true;
    } else if (s.full == "mss") {
      mssText = arg;
    }
  }
  Datastore* ds = session.datastore();
  ojson err;
  std::string depvar;
  std::vector<FvTerm> terms;
  if (!qrParse(parsed, session, depvar, terms, err)) return fail(err);

  // ---- ado markout touse cluster, s --------------------------------------------
  std::string clusterVar;
  if (!cluster.empty()) {
    std::vector<std::string> cv;
    if (!e3::expandVars(cluster, session, &cv, &err)) return fail(err);
    if (cv.size() != 1) return fail(rcError("too many variables specified", 103));
    clusterVar = cv[0];
  }
  std::vector<std::string> mssVars;
  if (!mssText.empty()) {
    if (!e3::expandVars(mssText, session, &mssVars, &err)) return fail(err);
    for (const auto& v : mssVars)
      if (ds->dtype(v) != "f64") return fail(rcError("string variables not allowed in varlist", 109));
  }

  // ---- ado the weights; weights with cluster: a message and a silent exit ---
  const bool weighted = parsed.weightType.has_value() && parsed.weightVar.has_value() &&
                        !parsed.weightVar->empty();
  std::string wvar, wexpText;
  if (weighted) {
    const std::string wt = lowerQ(*parsed.weightType);
    if (wt.empty() || wt[0] != 'f') return fail(rcError(*parsed.weightType + "s not allowed", 101));
    wvar = strutil::trim(parsed.weightVar.value_or(std::string()));
    wexpText = parsed.weightExp.value_or(wvar);
    std::vector<std::string> wv;
    if (!e3::expandVars(wvar, session, &wv, &err)) return fail(err);
    if (wv.size() != 1) return fail(rcError("invalid weight expression", 198));
    wvar = wv[0];
  }
  if (weighted && !cluster.empty()) {
    blocks.push_back(e3::displayText("Options cluster and weights are not compatible"));
    return blocks;
  }

  // ---- ado qreg's quantile() (QREG:45: >= 1 is a percent) ------------------------
  const double tau = q >= 1 ? q / 100.0 : q;
  if (!(tau > 0 && tau < 1)) return fail(rcError("quantile(" + qText + ") is out of range", 198));

  // ---- the sample ----------------------------------------------------------------
  std::vector<double> clNum;
  std::vector<std::string> clStr;
  bool clIsStr = false;
  std::vector<char> clOk;
  if (!clusterVar.empty()) {
    const size_t Nall0 = static_cast<size_t>(ds->nRows());
    clOk.assign(Nall0, 1);
    if (ds->dtype(clusterVar) == "f64") {
      const double* cp = ds->col(clusterVar).f64();
      clNum.assign(cp, cp + Nall0);
      for (size_t i = 0; i < Nall0; ++i) clOk[i] = !std::isnan(clNum[i]);
    } else {
      clIsStr = true;
      const cecon::Column& c = ds->col(clusterVar);
      const int32_t* codes = c.codes();
      clStr.assign(Nall0, std::string());
      for (size_t i = 0; i < Nall0; ++i)
        if (codes != nullptr && codes[i] >= 0 && static_cast<size_t>(codes[i]) < c.levels.size())
          clStr[i] = c.levels[static_cast<size_t>(codes[i])];
      for (size_t i = 0; i < Nall0; ++i) clOk[i] = !clStr[i].empty();
    }
  }
  QrData d;
  if (!qrSample(parsed, session, depvar, terms, weighted ? wvar : std::string(), mssVars, clOk, d, err))
    return fail(err);
  auto& data = d.data;
  auto& cols = d.cols;
  auto& names = d.names;
  auto& estIdx = d.estIdx;
  auto& rows = d.rows;
  const auto& X = d.X;
  const auto& Y = d.Y;
  const auto& W = d.W;
  const size_t p = d.p, K = d.K, n = d.n, Nall = d.Nall;
  const bool fvops = d.fvops;
  const double N = d.N;

  // ---- qreg (version 11): the LP vertex -------------------------------------------
  const LpResult lp = solveQuantileLp(X, n, p, Y, W, tau);
  if (!lp.ok)
    return fail(rcError("qreg2: the quantile regression did not reach its optimum (" + lp.why +
                            "); nothing was estimated",
                        430, "engine_error"));
  // A non-unique optimum (all-factor / all-dummy median designs, w2j M07 and F04)
  // is posted from PIET's own optimal vertex, exactly as a unique one: b, the
  // residuals, f_r, the sandwich V, R-squared and the tests below all come from
  // that vertex (User decision 2026-10-27). Stata's closed-source `_qreg` (AFK
  // simplex) may land on another optimal vertex, so the vertex-dependent results
  // can differ from Stata's; the block's note says so. Records that do not depend
  // on the vertex (w2j M08: margins' metadata, byte-identical to the unique-fit
  // M01) then match.
  const bool nonUnique = !lp.unique;
  const std::vector<double>& bEst = lp.b;
  std::vector<double> e(n), xb(n);
  for (size_t t = 0; t < n; ++t) {
    double s = 0;
    for (size_t a = 0; a < p; ++a) s += X[t * p + a] * bEst[a];
    xb[t] = s;
    e[t] = Y[t] - s;
  }
  std::vector<char> inBasis(n, 0);
  for (size_t i : lp.basis) inBasis[i] = 1;
  double ymax = 0;
  for (double v : Y) ymax = std::max(ymax, std::fabs(v));
  std::vector<char> lpZero(n, 0);
  for (size_t t = 0; t < n; ++t) lpZero[t] = inBasis[t] || std::fabs(e[t]) <= 1e-11 * (1.0 + ymax);
  // e(sum_adev), e(sum_rdev), e(q_v)
  double sumAdev = 0;
  for (size_t t = 0; t < n; ++t)
    if (!lpZero[t]) sumAdev += W[t] * (e[t] >= 0 ? tau * e[t] : (tau - 1) * e[t]);
  double qv = kNaN, sumRdev = 0;
  {
    std::vector<std::pair<double, double>> yw;
    for (size_t t = 0; t < n; ++t) yw.emplace_back(Y[t], W[t]);
    std::stable_sort(yw.begin(), yw.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    double cum = 0;
    for (const auto& v : yw) {
      cum += v.second;
      if (cum >= tau * N) {
        qv = v.first;
        break;
      }
    }
    for (size_t t = 0; t < n; ++t) {
      const double d = Y[t] - qv;
      sumRdev += W[t] * (d >= 0 ? tau * d : (tau - 1) * d);
    }
  }
  const double fr = qreg2Fr(e, W, lpZero, tau, static_cast<double>(p));
  if (!std::isfinite(fr))
    return fail(rcError("qreg2: the residual-density window collapses or its estimate is undefined; "
                        "this version cannot reproduce the solver-dependent covariance and degrees of freedom; "
                        "nothing was estimated",
                        198, "unsupported_option"));
  const QregOldVce oldVce = qregOldVce(d, e, tau, fr);
  const std::vector<double>& Vold = oldVce.V;
  const double dfM = static_cast<double>(p) - 1.0;
  const double rankV = oldVce.rank, dfR = oldVce.dfR;
  std::vector<double> bPost(K, 0.0);
  for (size_t i = 0; i < K; ++i)
    if (estIdx[i] >= 0) bPost[i] = bEst[static_cast<size_t>(estIdx[i])];

  auto installEsample = [&](LastModel& lm) {
    std::vector<char> mask(static_cast<size_t>(session.datastore()->nRows()), 0);
    for (size_t r : rows) mask[r] = 1;
    (void)e3::installEsampleMask(session, lm, mask);
  };
  auto toMat = [K](const std::vector<double>& V) {
    std::vector<std::vector<double>> M(K, std::vector<double>(K));
    for (size_t i = 0; i < K; ++i)
      for (size_t j = 0; j < K; ++j) M[i][j] = V[i * K + j];
    return M;
  };
  // The qreg e() both exits share; qreg2_lincom_dof is read by runLincom.
  auto postQregE = [&](LastModel& lm, ojson& block) {
    block["df_m"] = dfM;
    block["df_r"] = dfR;
    if (fvops) block["k_vars"] = rankV;
    block["qreg2_lincom_dof"] = oldVce.lincomDof;
    lm.result = block;
    installEsample(lm);
    session.setLastModel(lm);
    session.lastEstBlock = block;
    e3::setEScalarOnFit(session, "N", N);
    e3::setEScalarOnFit(session, "df_r", dfR);
    e3::setEScalarOnFit(session, "df_m", dfM);
    if (fvops) e3::setEScalarOnFit(session, "rank", rankV);
    e3::setEScalarOnFit(session, "f_r", fr);
    e3::setEScalarOnFit(session, "sum_w", N);
    e3::setEScalarOnFit(session, "q_v", qv);
    e3::setEScalarOnFit(session, "q", tau);
    e3::setEScalarOnFit(session, "sum_rdev", sumRdev);
    e3::setEScalarOnFit(session, "sum_adev", sumAdev);
    e3::setEScalarOnFit(session, "convcode", 0.0);
    e3::setEScalarOnFit(session, "r2_p", 1.0 - sumAdev / sumRdev);
    e3::setEStringOnFit(session, "predict", "qreg_p");
    e3::setEStringOnFit(session, "properties", "b V");
    e3::setEStringOnFit(session, "marginsnotok", "stdp stddp residuals");
  };

  // ---- ado: residuals, zeros, the bandwidth ---------------------------------
  std::vector<double> ez = e;  // _e (predict, res), zeroed below
  double aeP50;
  {
    std::vector<std::pair<double, double>> v;
    for (size_t t = 0; t < n; ++t) v.emplace_back(std::fabs(e[t]), 1.0);
    std::stable_sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    aeP50 = pctileW(v, 50);
  }
  double zeros = 0;
  for (size_t t = 0; t < n; ++t)
    if (std::fabs(e[t]) < aeP50 * epsilon) {
      ez[t] = 0.0;
      zeros += 1;
    }
  const double z975 = cecon::stata_invnormal(0.975);
  const double iq = cecon::stata_invnormal(tau);
  const double hs = std::pow(z975, 2.0 / 3.0) *
                    std::pow((1.5 * std::pow(normalden(iq), 2)) / (2 * std::pow(iq, 2) + 1), 1.0 / 3.0);
  std::vector<std::pair<double, double>> ezW;
  for (size_t t = 0; t < n; ++t) ezW.emplace_back(ez[t], W[t]);
  std::stable_sort(ezW.begin(), ezW.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  double ah;
  if (silverman) {
    double mean = 0;
    for (size_t t = 0; t < n; ++t) mean += W[t] * ez[t];
    mean /= N;
    double ss = 0;
    for (size_t t = 0; t < n; ++t) ss += W[t] * (ez[t] - mean) * (ez[t] - mean);
    const double sd = std::sqrt(ss / (N - 1));
    ah = std::min(sd, (pctileW(ezW, 75) - pctileW(ezW, 25)) / 1.34);
  } else {
    const double med = pctileW(ezW, 50);
    std::vector<std::pair<double, double>> d;
    for (size_t t = 0; t < n; ++t) d.emplace_back(std::fabs(ez[t] - med), W[t]);
    std::stable_sort(d.begin(), d.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    ah = pctileW(d, 50);
  }
  const double hN = hs * std::pow(N, -1.0 / 3.0);
  if ((tau + hN) > 1 || (tau - hN) < 0) {
    // ado: an error message and `exit` without an rc; e() is qreg's (R09/R11:
    // e(cmd) qreg, e(wexp) "= __00000F", qreg2's _fw tempvar).
    LastModel lm;
    lm.cobraCmd = "qreg2";
    lm.stataCmd = "qreg";
    lm.y = depvar;
    lm.depvar = depvar;
    lm.vcov = "";
    lm.fitWeight = "__00000F";
    lm.fitWtype = "fw";
    lm.fitWexp = "__00000F";
    for (size_t i = 0; i + 1 < K; ++i) lm.x.push_back(names[i]);
    ojson block;
    block["type"] = "estimation";
    block["estimator"] = "qreg";
    block["depvar"] = depvar;
    block["n_obs"] = N;
    block["coeftable"] = e3::coeftableFromPosting(names, bPost, toMat(Vold), dfR);
    postQregE(lm, block);
    e3::postBV(session, names, bPost, toMat(Vold));
    blocks.push_back(e3::displayText("Cannot compute bandwidth; too few observations?"));
    return blocks;
  }
  const double hsi = ah * (cecon::stata_invnormal(tau + hN) - cecon::stata_invnormal(tau - hN));
  std::vector<double> dens(n), psi(n);
  for (size_t t = 0; t < n; ++t) {
    dens[t] = std::sqrt(W[t] * (std::fabs(ez[t]) < hsi ? 1.0 : 0.0) / (2 * hsi));
    // ado `qui g _w = ...`: an untyped generate, stored as float (c(type))
    psi[t] = e3::toFloat(std::sqrt(W[t]) * (tau - (ez[t] <= 0 ? 1.0 : 0.0)));
  }

  // ---- ado: D, A, V ------------------------------------------------------
  // cluster ids (ado): _n, float(cluster*sqrt(fw)), or encode's codes
  std::vector<double> gid(n);
  if (clusterVar.empty()) {
    for (size_t t = 0; t < n; ++t) gid[t] = static_cast<double>(t);
  } else if (!clIsStr) {
    for (size_t t = 0; t < n; ++t) gid[t] = e3::toFloat(clNum[rows[t]] * std::sqrt(W[t]));
  } else {
    std::set<std::string> distinct;
    for (size_t r : rows) distinct.insert(clStr[r]);
    std::map<std::string, double> code;
    double c = 0;
    for (const auto& s : distinct) code[s] = ++c;
    for (size_t t = 0; t < n; ++t) gid[t] = code[clStr[rows[t]]] * std::sqrt(W[t]);
  }
  std::vector<size_t> ord(n);
  std::iota(ord.begin(), ord.end(), 0);
  std::stable_sort(ord.begin(), ord.end(), [&gid](size_t a, size_t b) { return gid[a] < gid[b]; });
  std::vector<double> Am(p * p, 0.0), Dm(p * p, 0.0);
  {
    size_t s = 0;
    std::vector<double> sg(p), cg(p);
    while (s < n) {
      size_t e2 = s;
      while (e2 < n && gid[ord[e2]] == gid[ord[s]]) ++e2;
      std::fill(sg.begin(), sg.end(), 0.0);
      std::fill(cg.begin(), cg.end(), 0.0);
      for (size_t k2 = s; k2 < e2; ++k2) {
        const size_t t = ord[k2];
        for (size_t a = 0; a < p; ++a) {
          // Neumaier-compensated (quadcross)
          const double term = X[t * p + a] * psi[t];
          const double sum = sg[a] + term;
          cg[a] += std::fabs(sg[a]) >= std::fabs(term) ? (sg[a] - sum) + term : (term - sum) + sg[a];
          sg[a] = sum;
        }
      }
      for (size_t a = 0; a < p; ++a) sg[a] += cg[a];
      for (size_t a = 0; a < p; ++a) {
        if (sg[a] == 0.0) continue;
        for (size_t c = 0; c < p; ++c) Am[a * p + c] += sg[a] * sg[c];
      }
      s = e2;
    }
    for (size_t t = 0; t < n; ++t) {
      if (dens[t] == 0.0) continue;
      for (size_t a = 0; a < p; ++a) {
        const double xa = X[t * p + a] * dens[t];
        if (xa == 0.0) continue;
        for (size_t c = 0; c < p; ++c) Dm[a * p + c] += xa * (X[t * p + c] * dens[t]);
      }
    }
  }
  std::vector<double> Vm(K * K, 0.0);
  {
    std::vector<std::vector<double>> Dd(p, std::vector<double>(p));
    for (size_t a = 0; a < p; ++a)
      for (size_t c = 0; c < p; ++c) Dd[a][c] = Dm[a * p + c];
    const auto Di = e3::invsym(Dd);
    std::vector<double> T(p * p, 0.0), Ve(p * p, 0.0);
    for (size_t a = 0; a < p; ++a)
      for (size_t c = 0; c < p; ++c) {
        double s = 0;
        for (size_t d = 0; d < p; ++d) s += Di[a][d] * Am[d * p + c];
        T[a * p + c] = s;
      }
    for (size_t a = 0; a < p; ++a)
      for (size_t c = 0; c < p; ++c) {
        double s = 0;
        for (size_t d = 0; d < p; ++d) s += T[a * p + d] * Di[d][c];
        Ve[a * p + c] = s;
      }
    for (size_t i = 0; i < K; ++i)
      for (size_t j = 0; j < K; ++j)
        if (estIdx[i] >= 0 && estIdx[j] >= 0)
          Vm[i * K + j] = Ve[static_cast<size_t>(estIdx[i]) * p + static_cast<size_t>(estIdx[j])];
    for (size_t i = 0; i < K; ++i)
      for (size_t j = i + 1; j < K; ++j) Vm[i * K + j] = Vm[j * K + i];
  }

  // ---- ado the number of clusters ---------------------------------------
  double nClust = 0;
  std::map<std::string, std::vector<size_t>> byCluster;  // cluster value -> sample rows
  if (!clusterVar.empty()) {
    for (size_t t = 0; t < n; ++t) {
      std::string key;
      if (clIsStr) {
        key = "s" + clStr[rows[t]];
      } else {
        char buf[40];
        std::snprintf(buf, sizeof(buf), "%a", clNum[rows[t]]);
        key = buf;
      }
      byCluster[key].push_back(t);
    }
    nClust = static_cast<double>(byCluster.size());
  }

  // ---- ado R-squared and the objective function ------------------------
  double r2 = kNaN;
  {
    double mf = 0, my = 0;
    for (size_t t = 0; t < n; ++t) {
      mf += W[t] * xb[t];
      my += W[t] * Y[t];
    }
    mf /= N;
    my /= N;
    double sff = 0, syy = 0, sfy = 0;
    for (size_t t = 0; t < n; ++t) {
      sff += W[t] * (xb[t] - mf) * (xb[t] - mf);
      syy += W[t] * (Y[t] - my) * (Y[t] - my);
      sfy += W[t] * (xb[t] - mf) * (Y[t] - my);
    }
    if (sff > 1e-20 * (mf * mf * N) && syy > 0) {
      const double rho = sfy / std::sqrt(sff * syy);
      r2 = rho * rho;
    }
  }
  std::vector<double> check(n);
  double objFunc = 0;
  for (size_t t = 0; t < n; ++t) {
    check[t] = ez[t] * (tau - (ez[t] <= 0 ? 1.0 : 0.0));
    objFunc += W[t] * check[t];
  }
  objFunc /= N;

  // ---- ado the Machado-Santos Silva test (no cluster) ---------------------
  double mssChi2 = kNaN, mssDf = kNaN;
  if (!notest && clusterVar.empty()) {
    std::vector<std::vector<double>> Z;
    if (mssVars.empty()) {
      std::vector<double> f2(n);
      for (size_t t = 0; t < n; ++t) f2[t] = xb[t] * xb[t];
      Z.push_back(xb);
      Z.push_back(f2);
    } else {
      for (const auto& v : mssVars) {
        std::vector<double> zc(n);
        for (size_t t = 0; t < n; ++t) zc[t] = data[v][rows[t]];
        Z.push_back(zc);
      }
    }
    mssDf = static_cast<double>(Z.size());
    // R^2 of `reg check Z [fw]`: centred cross products, collinear columns dropped
    const size_t m = Z.size();
    double mc = 0;
    for (size_t t = 0; t < n; ++t) mc += W[t] * check[t];
    mc /= N;
    std::vector<double> mz(m, 0.0);
    for (size_t a = 0; a < m; ++a) {
      for (size_t t = 0; t < n; ++t) mz[a] += W[t] * Z[a][t];
      mz[a] /= N;
    }
    std::vector<std::vector<double>> S(m, std::vector<double>(m, 0.0));
    std::vector<double> sc(m, 0.0);
    double tss = 0;
    for (size_t t = 0; t < n; ++t) {
      const double dc = check[t] - mc;
      tss += W[t] * dc * dc;
      for (size_t a = 0; a < m; ++a) {
        const double da = Z[a][t] - mz[a];
        sc[a] += W[t] * da * dc;
        for (size_t c = 0; c < m; ++c) S[a][c] += W[t] * da * (Z[c][t] - mz[c]);
      }
    }
    const auto Si = e3::invsym(S);
    double ess = 0;
    for (size_t a = 0; a < m; ++a)
      for (size_t c = 0; c < m; ++c) ess += sc[a] * Si[a][c] * sc[c];
    const double r2m = tss > 0 ? ess / tss : 0.0;
    mssChi2 = N * r2m;
  }

  // ---- ado the Parente-Santos Silva test (cluster) ------------------------
  double pssT = kNaN;
  if (!notest && !clusterVar.empty() && nClust < N) {
    double sumNum = 0;
    double cnt = 0;
    for (const auto& [key, members] : byCluster) {
      const double ct = static_cast<double>(members.size());
      if (!(ct > 1)) continue;
      double s1 = 0, s2 = 0;
      for (size_t t : members) {
        const double ph = tau - (ez[t] < 0 ? 1.0 : 0.0);
        s1 += ph;
        s2 += ph * ph;
      }
      const double sphi = e3::toFloat(s1), sphi2 = e3::toFloat(s2);  // untyped egen: float
      const double numt =
          (sphi * sphi - sphi2) / std::sqrt(2 * (tau * tau) * ((1 - tau) * (1 - tau)) * ct * (ct - 1));
      sumNum += numt;
      cnt += 1;
    }
    if (cnt > 0) pssT = (sumNum / cnt) * std::sqrt(cnt);
  }

  // ---- ado the sort order the ado leaves --------------------------------------
  // Without a cluster the last sort is `sort _gv` (_gv = _n if touse): the sample
  // rows first in their order, the others after (I02 nid 3). With one the ado
  // restores the original order.
  if (clusterVar.empty() && n < Nall) {
    std::vector<double> key(Nall);
    double k1 = 0, k2 = static_cast<double>(n);
    std::vector<char> inS(Nall, 0);
    for (size_t r : rows) inS[r] = 1;
    for (size_t i = 0; i < Nall; ++i) key[i] = inS[i] ? ++k1 : ++k2;
    if (!e3::writeColumn(session, kOrder, key, &err)) return fail(err);
    if (!e3::runLine(std::string("quietly sort ") + kOrder, session, &err)) return fail(err);
    SessionState::TempDropScope td(session);
    if (!e3::runLine(std::string("quietly drop ") + kOrder, session, &err)) return fail(err);
    for (size_t t = 0; t < n; ++t) rows[t] = t;
  }

  // ---- ado posting ----------------------------------------------------------
  LastModel lm;
  lm.cobraCmd = "qreg2";
  lm.stataCmd = "qreg2";
  lm.y = depvar;
  lm.depvar = depvar;
  lm.vcov = "";
  if (weighted) {
    lm.fitWeight = wvar;
    lm.fitWtype = "fw";
    lm.fitWexp = wexpText;
  }
  for (size_t i = 0; i + 1 < K; ++i) lm.x.push_back(names[i]);
  ojson block;
  block["type"] = "estimation";
  block["estimator"] = "qreg2";
  block["depvar"] = depvar;
  block["n_obs"] = N;
  block["coeftable"] = e3::coeftableFromPosting(names, bPost, toMat(Vm), dfR);
  if (std::isfinite(r2)) block["r2"] = r2;
  if (nonUnique)
    block["warnings"] = ojson::array(
        {"the quantile regression has more than one optimal solution; the coefficients are one of "
         "them and may differ from Stata's, and so may e(V), e(f_r), e(r2) and the tests built on "
         "the residuals"});
  // margins after qreg2 reads its dy/dx columns from here (runQreg2Margins)
  {
    ojson fv = ojson::array();
    for (const auto& c : cols) {
      const FvTerm& t = terms[c.term];
      if (t.atoms.size() != 1) {
        fv.push_back(ojson{{"name", c.name}, {"var", ""}, {"factor", false}});
        continue;
      }
      fv.push_back(ojson{{"name", c.name}, {"var", t.atoms[0].var}, {"factor", t.atoms[0].factor}});
    }
    block["qreg2_cols"] = fv;
  }
  postQregE(lm, block);
  e3::postBV(session, names, bPost, toMat(Vm));
  e3::setEScalarOnFit(session, "obj_func", objFunc);
  if (std::isfinite(r2)) e3::setEScalarOnFit(session, "r2", r2);
  e3::setEScalarOnFit(session, "zeros", zeros);
  if (std::isfinite(mssChi2)) {
    e3::setEScalarOnFit(session, "mss_chi2", mssChi2);
    e3::setEScalarOnFit(session, "mss_df", mssDf);
    e3::setEScalarOnFit(session, "mss_p", chi2tailQ(mssDf, mssChi2));
  }
  if (std::isfinite(pssT)) {
    e3::setEScalarOnFit(session, "pss_t", pssT);
    e3::setEScalarOnFit(session, "pss_p", 2 * cecon::stata_normal(-std::fabs(pssT)));
  }

  // ---- the display ------------------------------------------------------------
  {
    std::string text;
    char buf[256];
    if (tau == 0.5) {
      text = "Median regression\n";
    } else {
      std::snprintf(buf, sizeof(buf), "%g Quantile regression\n", tau);
      text = buf;
    }
    std::snprintf(buf, sizeof(buf), "R-squared = %.8g\nNumber of obs = %.0f\nObjective function = %.8g\n",
                  r2, N, objFunc);
    text += buf;
    if (!clusterVar.empty()) {
      std::snprintf(buf, sizeof(buf), "Standard errors adjusted for %.0f clusters in %s", nClust,
                    clusterVar.c_str());
      text += buf;
    } else if (p == 1) {
      text += "                     Standard errors valid with i.i.d. errors";
    } else {
      text += "                     Heteroskedasticity robust standard errors";
    }
    blocks.push_back(e3::displayText(text));
  }
  blocks.push_back(block);
  if (std::isfinite(mssChi2)) {
    char buf[512];
    const std::string vars =
        mssVars.empty() ? "Fitted values of " + depvar + " and its squares" : mssText;
    std::snprintf(buf, sizeof(buf),
                  "Machado-Santos Silva test for heteroskedasticity\n         Ho: Constant variance\n"
                  "         Variables: %s\n\n         chi2(%.0f)      =  %6.3f\n         Prob > chi2  =  %6.3f",
                  vars.c_str(), mssDf, mssChi2, chi2tailQ(mssDf, mssChi2));
    blocks.push_back(e3::displayText(buf));
  }
  if (std::isfinite(pssT)) {
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "Parente-Santos Silva test for intra-cluster correlation\n"
                  "         Ho: No intra-cluster correlation\n\n            T   =  %6.3f\n"
                  "         P>|T|  =  %6.3f",
                  pssT, 2 * cecon::stata_normal(-std::fabs(pssT)));
    blocks.push_back(e3::displayText(buf));
  }
  return blocks;
}

// Stata 19's `qreg` (the version-13-and-later path) for its default VCE,
// vce(iid) with the `fitted` density and the `hsheather` bandwidth, fitted here
// with the exact LP vertex Stata's qreg returns. Method:
// h = N^(-1/3) z^(2/3) [1.5 phi(z_tau)^2/(2 z_tau^2+1)]^(1/3)
// with z = invnormal(1 - alpha/2), the fits at tau -/+ h, s = (mean xb_u - mean
// xb_l)/(2h), V = tau(1-tau) s^2 invsym(X'X); e(f_r) = 1/s, e(rank) the
// rank of V, e(df_r) = N - rank. Factor-variable designs carry their
// base/omitted columns (w2j_e3_qreg2.log A01 `qreg|iid||201|.|196|4|5|`, F00 107
// columns, rank 105). Weights (`[fw iw pw]`, the weighted LP): fweight N = sum
// of the weights (`r(sum_w)`), so the bandwidth and e(N) are the replicated-data ones; iweight N = the row count
// (`count`), and the iid Gram is X'W^2X (`weights2 = [iw=w*w]`).
// Both take the tau-/+h means weighted (`sum ... [w], meanonly`) and
// V = tau(1-tau) s^2 invsym(J). StataBE probe (w2p_estim_ev fixture, fw
// and iw at q .25/.37/.5/.8): e(q_v) is the first sorted y whose cumulative weight
// reaches q*sum(w). Returns null for every other form (vce(robust), pweights,
// options this path does not read): runQreg's engine path answers those.
ojson runQregNative(const eval::ParsedCommand& parsed, SessionState& session) {
  const bool weighted = parsed.weightType.has_value() && parsed.weightVar.has_value() &&
                        !parsed.weightVar->empty();
  if (!session.hasData()) return ojson();
  std::string wvar, wexpText, wtype;
  bool importanceWeights = false;
  if (weighted) {
    const std::string wt = lowerQ(*parsed.weightType);
    if (wt != "fw" && wt != "fweight" && wt != "iw" && wt != "iweight") return ojson();
    importanceWeights = wt == "iw" || wt == "iweight";
    wtype = importanceWeights ? "iw" : "fw";
    wvar = strutil::trim(*parsed.weightVar);
    wexpText = parsed.weightExp.value_or(wvar);
    std::vector<std::string> wvars;
    ojson weightErr;
    if (!e3::expandVars(wvar, session, &wvars, &weightErr)) return ojson::array({weightErr});
    if (wvars.size() != 1) return ojson::array({rcError("invalid weight expression", 198)});
    wvar = wvars.front();
    if (session.datastore()->dtype(wvar) != "f64")
      return ojson::array({rcError("type mismatch", 109)});
  }
  double q = 0.5;
  std::string qText = "0.5";
  double level = static_cast<double>(session.level);
  for (const auto& [k, v] : parsed.opts) {
    const std::string key = lowerQ(k);
    const std::string arg = strutil::trim(v.strValue);
    if (!key.empty() && std::string("quantile").compare(0, key.size(), key) == 0) {
      if (v.isBool) return ojson();
      char* end = nullptr;
      q = std::strtod(arg.c_str(), &end);
      if (arg.empty() || end == nullptr || *end != '\0') return ojson();
      qText = arg;
    } else if (key.size() >= 1 && std::string("level").compare(0, key.size(), key) == 0) {
      if (v.isBool) return ojson();
      char* end = nullptr;
      level = std::strtod(arg.c_str(), &end);
      if (arg.empty() || end == nullptr || *end != '\0' || !(level >= 10 && level <= 99.99))
        return ojson();
    } else if (key.size() >= 3 && std::string("wlsiter").compare(0, key.size(), key) == 0) {
      // the WLS start only seeds the LP; the vertex is the same
    } else if (key == "nolog" || key == "nolo" || key == "log") {
    } else if (key == "vce") {
      if (v.isBool || lowerQ(arg) != "iid") return ojson();
    } else {
      return ojson();
    }
  }
  ojson blocks = ojson::array();
  auto fail = [&blocks](ojson e) {
    blocks.push_back(std::move(e));
    return blocks;
  };
  const double tau = q >= 1 ? q / 100.0 : q;
  if (!(tau > 0 && tau < 1)) return fail(rcError("quantile(" + qText + ") is out of range", 198));
  ojson err;
  std::string depvar;
  std::vector<FvTerm> terms;
  if (!qrParse(parsed, session, depvar, terms, err)) return fail(err);
  QrData d;
  if (!qrSample(parsed, session, depvar, terms, wvar, {}, {}, d, err, !importanceWeights)) return fail(err);
  const size_t p = d.p, K = d.K, n = d.n;
  const double sumW = d.N;
  const double N = importanceWeights ? static_cast<double>(n) : sumW;
  const LpResult lp = solveQuantileLp(d.X, n, p, d.Y, d.W, tau);
  if (!lp.ok)
    return fail(rcError("qreg: the quantile regression did not reach its optimum (" + lp.why +
                            "); nothing was estimated",
                        430, "engine_error"));
  // A non-unique optimum (all-dummy / all-factor median designs, w2j M10/F05) is
  // still posted from this fit. Its iid e(V) is tau(1-tau) s^2 invsym(X'X) with s
  // from the tau-/+h fits, and e(sum_adev) is the optimal objective, so neither
  // depends on which optimal vertex the tau fit lands on. Measured: PIET's 1/s is
  // bit-identical to Stata's e(f_r) on M10 (+1.bb66808e4ef86X-004) and F05
  // (+1.1ca4865a82c82X+002). The coefficients are one optimal vertex. Stata's
  // closed-source `_qreg` may pick another one, and the block's note says so.
  // The engine path this replaces posted an arbitrary vertex too, but no e(V).
  const bool nonUnique = !lp.unique;
  // the bandwidth and the two neighbouring fits (VCE_fitted)
  const double alpha = 1.0 - level / 100.0;
  const double z = cecon::stata_invnormal(1.0 - alpha / 2.0);
  const double invtau = cecon::stata_invnormal(tau);
  const double h = std::pow(N, -1.0 / 3.0) * std::pow(z, 2.0 / 3.0) *
                   std::pow(1.5 * std::pow(normalden(invtau), 2) / (2 * std::pow(invtau, 2) + 1), 1.0 / 3.0);
  const double tauL = tau - h, tauU = tau + h;
  if (tauL <= 0 || tauU >= 1)
    return fail(rcError("VCE computation failed; try a different bandwidth or bsqreg", 498));
  const LpResult lpL = solveQuantileLp(d.X, n, p, d.Y, d.W, tauL);
  const LpResult lpU = solveQuantileLp(d.X, n, p, d.Y, d.W, tauU);
  if (!lpL.ok || !lpU.ok)
    return fail(rcError("VCE computation failed; try increasing the maximum number of iterations or "
                        "try bsqreg",
                        498));
  double FL = 0, FU = 0;
  for (size_t t = 0; t < n; ++t) {
    double sl = 0, su = 0;
    for (size_t a = 0; a < p; ++a) {
      sl += d.X[t * p + a] * lpL.b[a];
      su += d.X[t * p + a] * lpU.b[a];
    }
    FL += d.W[t] * sl;
    FU += d.W[t] * su;
  }
  FL /= sumW;
  FU /= sumW;
  const double s = (FU - FL) / (2 * h);
  // V = tau(1-tau) s^2 invsym(X'X)
  std::vector<std::vector<double>> J(p, std::vector<double>(p, 0.0));
  for (size_t t = 0; t < n; ++t)
    for (size_t a = 0; a < p; ++a) {
      const double xa = d.X[t * p + a];
      if (xa == 0.0) continue;
      const double weight = importanceWeights ? d.W[t] * d.W[t] : d.W[t];
      for (size_t c = 0; c < p; ++c) J[a][c] += weight * xa * d.X[t * p + c];
    }
  const auto Ji = e3::invsym(J);
  std::vector<double> V(K * K, 0.0);
  const double f = tau * (1 - tau) * s * s;
  for (size_t i = 0; i < K; ++i)
    for (size_t j = 0; j < K; ++j)
      if (d.estIdx[i] >= 0 && d.estIdx[j] >= 0)
        V[i * K + j] = f * Ji[static_cast<size_t>(d.estIdx[i])][static_cast<size_t>(d.estIdx[j])];
  for (size_t i = 0; i < K; ++i)
    for (size_t j = i + 1; j < K; ++j) V[i * K + j] = V[j * K + i];
  const double rank = static_cast<double>(glmVceRank(V, K));
  const double dfR = N - rank, dfM = static_cast<double>(p) - 1.0;
  std::vector<double> bPost(K, 0.0);
  for (size_t i = 0; i < K; ++i)
    if (d.estIdx[i] >= 0) bPost[i] = lp.b[static_cast<size_t>(d.estIdx[i])];
  double sumAdev = 0, sumRdev = 0, qv = kNaN;
  {
    double ymax = 0;
    for (double v : d.Y) ymax = std::max(ymax, std::fabs(v));
    for (size_t t = 0; t < n; ++t) {
      double s2 = 0;
      for (size_t a = 0; a < p; ++a) s2 += d.X[t * p + a] * lp.b[a];
      const double e = d.Y[t] - s2;
      if (std::fabs(e) <= 1e-11 * (1.0 + ymax)) continue;
      sumAdev += d.W[t] * (e >= 0 ? tau * e : (tau - 1) * e);
    }
    // _qreg's raw quantile without weights is the order statistic floor(q (n+1))
    // (measured: impl/qreg2/stata p01, 56 constant-only fits; w2j Q04 / M06
    // e(sum_rdev) at q .25 / .37 sit there, not at the LP minimizer).
    // Weighted: the first sorted y whose cumulative weight reaches tau*sum(w)
    // (StataBE fw/iw probe above, 12 of 12 q_v; at q .37 the unweighted
    // floor(q(sum(w)+1)) rule picks the row before).
    if (weighted) {
      std::vector<std::pair<double, double>> yw;
      for (size_t i = 0; i < n; ++i) yw.emplace_back(d.Y[i], d.W[i]);
      std::stable_sort(yw.begin(), yw.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
      const double target = tau * sumW;
      double cumul = 0;
      qv = yw.back().first;
      for (const auto& v : yw) {
        cumul += v.second;
        if (cumul >= target) {
          qv = v.first;
          break;
        }
      }
    } else {
      std::vector<double> ys(d.Y.begin(), d.Y.end());
      std::sort(ys.begin(), ys.end());
      double pos = std::floor(tau * (static_cast<double>(n) + 1.0));
      pos = std::min(std::max(pos, 1.0), static_cast<double>(n));
      qv = ys[static_cast<size_t>(pos) - 1];
    }
    for (size_t t = 0; t < n; ++t) {
      const double dd = d.Y[t] - qv;
      sumRdev += d.W[t] * (dd >= 0 ? tau * dd : (tau - 1) * dd);
    }
  }
  // the coefficient table at `level`, t(df_r)
  ojson ct = ojson::array();
  const double crit = cecon::t_critical(level == 95.0 ? 0.05 : alpha, dfR);
  for (size_t i = 0; i < K; ++i) {
    ojson r;
    r["name"] = d.names[i];
    r["b"] = bPost[i];
    const double v = V[i * K + i];
    const double se = v > 0 ? std::sqrt(v) : 0.0;
    r["se"] = se;
    if (se > 0) {
      const double t = bPost[i] / se;
      r["t"] = t;
      r["p"] = cecon::t_twosided_pvalue(t, dfR);
      r["ci_lo"] = bPost[i] - crit * se;
      r["ci_hi"] = bPost[i] + crit * se;
    } else {
      r["t"] = nullptr;
      r["p"] = nullptr;
      r["ci_lo"] = nullptr;
      r["ci_hi"] = nullptr;
    }
    ct.push_back(r);
  }
  ojson block;
  block["type"] = "estimation";
  block["estimator"] = "qreg";
  block["depvar"] = depvar;
  block["n_obs"] = N;
  block["coeftable"] = ct;
  block["tau"] = tau;
  block["sparsity"] = s;
  block["bwidth"] = h;
  block["sum_adev"] = sumAdev;
  block["df_m"] = dfM;
  block["df_r"] = dfR;
  block["k_vars"] = rank;
  if (nonUnique)
    block["warnings"] = ojson::array(
        {"the quantile regression has more than one optimal solution; the coefficients are one of "
         "them and may differ from Stata's (e(V), e(f_r) and e(sum_adev) do not depend on the choice)"});
  LastModel lm;
  lm.cobraCmd = "qreg";
  lm.stataCmd = "qreg";
  lm.y = depvar;
  lm.depvar = depvar;
  lm.x.assign(parsed.varlist.begin() + 1, parsed.varlist.end());
  lm.vcov = "iid";
  lm.fitWeight = wvar;
  lm.fitWtype = wtype;
  lm.fitWexp = wexpText;
  if (parsed.ifExpr.has_value()) lm.fitKeepif = *parsed.ifExpr;
  lm.result = block;
  {
    std::vector<char> mask(static_cast<size_t>(session.datastore()->nRows()), 0);
    for (size_t r : d.rows) mask[r] = 1;
    (void)e3::installEsampleMask(session, lm, mask);
  }
  session.setLastModel(lm);
  session.lastEstBlock = block;
  std::vector<std::vector<double>> Vm(K, std::vector<double>(K));
  for (size_t i = 0; i < K; ++i)
    for (size_t j = 0; j < K; ++j) Vm[i][j] = V[i * K + j];
  e3::postBV(session, d.names, bPost, Vm);
  e3::setEScalarOnFit(session, "N", N);
  e3::setEScalarOnFit(session, "df_r", dfR);
  e3::setEScalarOnFit(session, "df_m", dfM);
  e3::setEScalarOnFit(session, "rank", rank);
  e3::setEScalarOnFit(session, "f_r", 1.0 / s);
  e3::setEScalarOnFit(session, "sparsity", s);
  e3::setEScalarOnFit(session, "bwidth", h);
  e3::setEScalarOnFit(session, "sum_w", sumW);
  e3::setEScalarOnFit(session, "q_v", qv);
  e3::setEScalarOnFit(session, "q", tau);
  e3::setEScalarOnFit(session, "sum_rdev", sumRdev);
  e3::setEScalarOnFit(session, "sum_adev", sumAdev);
  e3::setEScalarOnFit(session, "convcode", 0.0);
  e3::setEScalarOnFit(session, "r2_p", 1.0 - sumAdev / sumRdev);
  e3::setEStringOnFit(session, "bwmethod", "hsheather");
  e3::setEStringOnFit(session, "denmethod", "fitted");
  e3::setEStringOnFit(session, "predict", "qreg_p");
  e3::setEStringOnFit(session, "properties", "b V");
  e3::setEStringOnFit(session, "marginsnotok", "stdp stddp residuals");
  {
    char buf[512];
    std::string head = tau == 0.5 ? std::string("Median regression")
                                   : [&] {
                                       char b2[64];
                                       std::snprintf(b2, sizeof(b2), "%g Quantile regression", tau);
                                       return std::string(b2);
                                     }();
    std::snprintf(buf, sizeof(buf),
                  "%-52sNumber of obs = %10.0f\n  Raw sum of deviations %9.6g (about %.8g)\n"
                  "  Min sum of deviations %9.6g%23sPseudo R2     = %10.4f",
                  head.c_str(), N, sumRdev, qv, sumAdev, "", 1.0 - sumAdev / sumRdev);
    blocks.push_back(e3::displayText(buf));
  }
  blocks.push_back(block);
  return blocks;
}

// `margins, dydx(varlist) [post]` after qreg2: qreg_p's default prediction is the
// linear index, so the average marginal effect of a main-effect term is its own
// coefficient (a factor level: its coefficient, the discrete change from the base,
// whose own column is 0) and the delta-method V is e(V)'s block. z statistics
// (margins posts no e(df_r)): w2j M01 `margins|qreg2|241|.|.|delta|Delta-method|||
// 0b.clinic 1.clinic 2.clinic|dy/dx|241|3|...|z|.|.|` (log:2915).
ojson runQreg2Margins(const eval::ParsedCommand& parsed, SessionState& session) {
  ojson blocks = ojson::array();
  auto fail = [&blocks](ojson e) {
    blocks.push_back(std::move(e));
    return blocks;
  };
  const auto& lmOpt = session.lastModel();
  if (!lmOpt.has_value() || !session.lastEstBlock.has_value() ||
      !session.lastEstBlock->contains("qreg2_cols"))
    return fail(rcError("last estimates not found", 301));
  const LastModel lm = *lmOpt;
  const ojson est = *session.lastEstBlock;
  std::string dydxRaw;
  bool post = false;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "dydx" && !v.isBool) {
      dydxRaw = v.strValue;
    } else if (k == "post" && (!v.isBool || v.boolValue)) {
      post = true;
    } else {
      return fail(ojson(eval::refusedCommand("margins", "qreg2_margins_" + k)));
    }
  }
  if (!parsed.varlist.empty() || strutil::trim(dydxRaw).empty() || parsed.ifExpr.has_value() ||
      parsed.inRange.has_value() || (parsed.weightVar.has_value() && !parsed.weightVar->empty()))
    return fail(ojson(eval::refusedCommand("margins", "qreg2_margins_form")));
  const eval::EvalMatrix* eb = session.eval.findClassMatrix('e', "b");
  const eval::EvalMatrix* eV = session.eval.findClassMatrix('e', "V");
  if (eb == nullptr || eV == nullptr || eb->data.empty())
    return fail(rcError("last estimates not found", 301));
  const auto& cols = est["qreg2_cols"];
  std::vector<size_t> pick;
  std::vector<std::string> xvars;
  bool anyFactor = false;
  for (std::string tok : splitWsQ(dydxRaw)) {
    if (tok.rfind("i.", 0) == 0 || tok.rfind("c.", 0) == 0) tok = tok.substr(2);
    bool found = false;
    for (size_t c = 0; c < cols.size(); ++c) {
      const std::string var = cols[c].value("var", std::string());
      const std::string nm = cols[c].value("name", std::string());
      if (var.empty()) {
        // the variable inside an interaction: its dy/dx is not the coefficient
        if (nm.find("." + tok + "#") != std::string::npos || nm.find("#" + tok) != std::string::npos ||
            (nm.size() > tok.size() && nm.compare(nm.size() - tok.size() - 1, std::string::npos, "." + tok) == 0))
          return fail(ojson(eval::refusedCommand("margins", "qreg2_margins_interaction")));
        continue;
      }
      if (var != tok) continue;
      found = true;
      pick.push_back(c);
      xvars.push_back(nm);
      anyFactor = anyFactor || cols[c].value("factor", false);
    }
    if (!found) return fail(rcError("variable " + tok + " not found in list of covariates", 322));
  }
  const size_t M = pick.size();
  std::vector<std::string> names;
  std::vector<double> b(M);
  std::vector<std::vector<double>> V(M, std::vector<double>(M, 0.0));
  for (size_t a = 0; a < M; ++a) {
    names.push_back(eb->colnames[pick[a]]);
    b[a] = eb->data[0][pick[a]];
    for (size_t c = 0; c < M; ++c) V[a][c] = eV->data[pick[a]][pick[c]];
  }
  const double N = est.value("n_obs", 0.0);
  ojson ct = ojson::array(), tableRows = ojson::array();
  const double crit = cecon::stata_invnormal(0.975);
  for (size_t a = 0; a < M; ++a) {
    const double se = std::sqrt(std::max(0.0, V[a][a]));
    ojson row;
    row["name"] = names[a];
    row["b"] = b[a];
    if (se > 0) {
      const double zst = b[a] / se;
      row["se"] = se;
      row["t"] = zst;
      row["p"] = 2 * cecon::stata_normal(-std::fabs(zst));
      row["ci_lo"] = b[a] - crit * se;
      row["ci_hi"] = b[a] + crit * se;
    } else {
      row["se"] = nullptr;
      row["t"] = nullptr;
      row["p"] = nullptr;
      row["ci_lo"] = nullptr;
      row["ci_hi"] = nullptr;
    }
    tableRows.push_back(row);
    ojson shown = row;
    if (shown["se"].is_null()) shown["se"] = 0.0;
    ct.push_back(shown);
  }
  ojson block;
  block["type"] = "estimation";
  block["estimator"] = "margins";
  block["depvar"] = lm.depvar;
  block["n_obs"] = N;
  block["coeftable"] = ct;
  {
    std::string wrt;
    for (size_t a = 0; a < M; ++a)
      if (!tableRows[a]["se"].is_null()) wrt += (wrt.empty() ? "" : " ") + names[a];
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%.0f", N);
    blocks.push_back(e3::displayText("Average marginal effects" + std::string(42, ' ') +
                                     "Number of obs = " + buf +
                                     "\n\nExpression: Linear prediction, predict()\ndy/dx wrt:  " + wrt));
  }
  blocks.push_back(block);
  if (anyFactor)
    blocks.push_back(
        e3::displayText("Note: dy/dx for factor levels is the discrete change from the base level."));
  std::string xv;
  for (const auto& s : xvars) xv += (xv.empty() ? "" : " ") + s;
  if (post) {
    LastModel lm2;
    lm2.cobraCmd = "qreg2_margins";
    lm2.stataCmd = "margins";
    lm2.depvar = lm.depvar;
    lm2.dataPath = lm.dataPath;
    lm2.vcov = "delta";
    lm2.result = block;
    lm2.esample = lm.esample;
    lm2.esampleSerial = lm.esampleSerial;
    lm2.esampleInstallPath = lm.esampleInstallPath;
    session.setLastModel(lm2);
    session.lastEstBlock = block;
    session.estAddedClear();
    e3::postBV(session, names, b, V);
    e3::setEStringOnFit(session, "vcetype", "Delta-method");
    e3::setEStringOnFit(session, "est_cmd", "qreg2");
    e3::setEStringOnFit(session, "derivatives", "dy/dx");
    e3::setEStringOnFit(session, "xvars", xv);
    e3::setEScalarOnFit(session, "N", N);
  }
  postGlmDydxRClass(session, block, names, b, V, tableRows, xv, false);
  return blocks;
}

}  // namespace handlers
}  // namespace piet
