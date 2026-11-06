// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// See rdlocal.h for method sources.
//
// Layout: (1) tiny dense linear algebra, (2) the shared local-polynomial core
// (kernel, Gamma/vartheta/Psi, weighted fits, the mass-point nearest-neighbor
// variance), (3) the CCFT19 mserd bandwidth chain, (4) the three drivers.

#include "estimators/rdlocal.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numeric>
#include <string>
#include <vector>

namespace cecon {
namespace {

// ---------------------------------------------------------------- linear algebra
// Every matrix here is (p+1)x(p+1) with p <= 5, so a plain Gauss-Jordan with
// partial pivoting is both the simplest and the fastest thing available; no
// reason to reach for the project's blocked kernels.
bool mat_inverse(const std::vector<double>& A, int k, std::vector<double>& out) {
    std::vector<double> M(A);
    out.assign(static_cast<size_t>(k) * k, 0.0);
    for (int i = 0; i < k; ++i) out[static_cast<size_t>(i) * k + i] = 1.0;
    for (int col = 0; col < k; ++col) {
        int piv = col;
        double best = std::fabs(M[static_cast<size_t>(col) * k + col]);
        for (int r = col + 1; r < k; ++r) {
            double v = std::fabs(M[static_cast<size_t>(r) * k + col]);
            if (v > best) { best = v; piv = r; }
        }
        if (!(best > 0.0) || !std::isfinite(best)) return false;
        if (piv != col) {
            for (int j = 0; j < k; ++j) {
                std::swap(M[static_cast<size_t>(col) * k + j], M[static_cast<size_t>(piv) * k + j]);
                std::swap(out[static_cast<size_t>(col) * k + j], out[static_cast<size_t>(piv) * k + j]);
            }
        }
        double d = M[static_cast<size_t>(col) * k + col];
        for (int j = 0; j < k; ++j) {
            M[static_cast<size_t>(col) * k + j] /= d;
            out[static_cast<size_t>(col) * k + j] /= d;
        }
        for (int r = 0; r < k; ++r) {
            if (r == col) continue;
            double f = M[static_cast<size_t>(r) * k + col];
            if (f == 0.0) continue;
            for (int j = 0; j < k; ++j) {
                M[static_cast<size_t>(r) * k + j] -= f * M[static_cast<size_t>(col) * k + j];
                out[static_cast<size_t>(r) * k + j] -= f * out[static_cast<size_t>(col) * k + j];
            }
        }
    }
    return true;
}

// Ordinary least squares of y on (1, z, z^2, ..., z^deg) via the normal
// equations.  Used only for the GLOBAL polynomial fits (rdplot's line, the
// bandwidth chain's preliminary derivative estimates), where the design is
// well conditioned because z is already centred at the cutoff.
bool poly_ls(const std::vector<double>& z, const std::vector<double>& y, int deg,
             std::vector<double>& coef) {
    const int k = deg + 1;
    std::vector<double> A(static_cast<size_t>(k) * k, 0.0), r(static_cast<size_t>(k), 0.0);
    std::vector<double> pw(static_cast<size_t>(k));
    for (size_t i = 0; i < z.size(); ++i) {
        pw[0] = 1.0;
        for (int j = 1; j < k; ++j) pw[static_cast<size_t>(j)] = pw[static_cast<size_t>(j - 1)] * z[i];
        for (int a = 0; a < k; ++a) {
            r[static_cast<size_t>(a)] += pw[static_cast<size_t>(a)] * y[i];
            for (int b = 0; b < k; ++b)
                A[static_cast<size_t>(a) * k + b] += pw[static_cast<size_t>(a)] * pw[static_cast<size_t>(b)];
        }
    }
    std::vector<double> Ai;
    if (!mat_inverse(A, k, Ai)) return false;
    coef.assign(static_cast<size_t>(k), 0.0);
    for (int a = 0; a < k; ++a)
        for (int b = 0; b < k; ++b) coef[static_cast<size_t>(a)] += Ai[static_cast<size_t>(a) * k + b] * r[static_cast<size_t>(b)];
    return true;
}

double factorial(int n) {
    double f = 1.0;
    for (int i = 2; i <= n; ++i) f *= i;
    return f;
}

// Standard normal two-sided tail, written without the catastrophic
// cancellation of 2*(1-Phi(|z|)) (same reason as cli::z_twosided_pvalue).
double z_pvalue(double z) { return std::erfc(std::fabs(z) * 0.7071067811865476); }

// Phi^{-1}((1+level/100)/2) for the handful of levels anybody asks for; a
// Beasley-Springer-Moro style rational approximation refined by one Newton
// step against erfc, which is exact to ~1e-15 over the useful range.
double normal_quantile(double p) {
    // p in (0,1).  Acklam's inverse-normal rational approximation.
    static const double a[6] = {-3.969683028665376e+01, 2.209460984245205e+02,
                                -2.759285104469687e+02, 1.383577518672690e+02,
                                -3.066479806614716e+01, 2.506628277459239e+00};
    static const double b[5] = {-5.447609879822406e+01, 1.615858368580409e+02,
                                -1.556989798598866e+02, 6.680131188771972e+01,
                                -1.328068155288572e+01};
    static const double c[6] = {-7.784894002430293e-03, -3.223964580411365e-01,
                                -2.400758277161838e+00, -2.549732539343734e+00,
                                4.374664141464968e+00, 2.938163982698783e+00};
    static const double d[4] = {7.784695709041462e-03, 3.224671290700398e-01,
                                2.445134137142996e+00, 3.754408661907416e+00};
    const double plow = 0.02425, phigh = 1.0 - plow;
    double x;
    if (p < plow) {
        double qq = std::sqrt(-2 * std::log(p));
        x = (((((c[0] * qq + c[1]) * qq + c[2]) * qq + c[3]) * qq + c[4]) * qq + c[5]) /
            ((((d[0] * qq + d[1]) * qq + d[2]) * qq + d[3]) * qq + 1);
    } else if (p > phigh) {
        double qq = std::sqrt(-2 * std::log(1 - p));
        x = -(((((c[0] * qq + c[1]) * qq + c[2]) * qq + c[3]) * qq + c[4]) * qq + c[5]) /
            ((((d[0] * qq + d[1]) * qq + d[2]) * qq + d[3]) * qq + 1);
    } else {
        double qq = p - 0.5, r2 = qq * qq;
        x = (((((a[0] * r2 + a[1]) * r2 + a[2]) * r2 + a[3]) * r2 + a[4]) * r2 + a[5]) * qq /
            (((((b[0] * r2 + b[1]) * r2 + b[2]) * r2 + b[3]) * r2 + b[4]) * r2 + 1);
    }
    for (int it = 0; it < 2; ++it) {  // Newton refinement on Phi(x) - p
        double e = 0.5 * std::erfc(-x * 0.7071067811865476) - p;
        double u = e * std::sqrt(2 * M_PI) * std::exp(x * x / 2);
        x -= u / (1 + x * u / 2);
    }
    return x;
}

// ---------------------------------------------------------------- kernel + side data
double kern_w(double u, RdKernel k) {
    double a = std::fabs(u);
    if (a > 1.0) return 0.0;
    return (k == RdKernel::Triangular) ? (1.0 - a) : 1.0;
}

// One side of the cutoff.  `x` holds the CENTRED running variable (x_i - c);
// on the left side these are negative, which is what CCT14's Gamma_{-,p}(h)
// wants (the sign of (x/h)^j is what makes the left-side constants carry the
// (-1)^{nu+r} factor automatically).  `n_all` is the FULL sample size, since
// every matrix below is normalised by n and not by the per-side count.
struct SideData {
    std::vector<double> x, y, s2;
    int64_t n_all = 0;
    bool empty() const { return x.empty(); }
};

// Gamma_{s,p}(h) = X_p(h)' W_s(h) X_p(h) / n, W = diag(1(side) K(x/h)/h).
std::vector<double> gamma_mat(const SideData& s, double h, int p, RdKernel kk) {
    const int k = p + 1;
    std::vector<double> G(static_cast<size_t>(k) * k, 0.0), pw(static_cast<size_t>(k));
    for (size_t i = 0; i < s.x.size(); ++i) {
        double u = s.x[i] / h;
        double w = kern_w(u, kk) / h;
        if (w == 0.0) continue;
        pw[0] = 1.0;
        for (int j = 1; j < k; ++j) pw[static_cast<size_t>(j)] = pw[static_cast<size_t>(j - 1)] * u;
        for (int a = 0; a < k; ++a)
            for (int b = 0; b < k; ++b)
                G[static_cast<size_t>(a) * k + b] += w * pw[static_cast<size_t>(a)] * pw[static_cast<size_t>(b)];
    }
    for (auto& v : G) v /= static_cast<double>(s.n_all);
    return G;
}

// vartheta_{s,p,r}(h) = X_p(h)' W_s(h) S_r(h) / n, S_r = ((x/h)^r).
std::vector<double> theta_vec(const SideData& s, double h, int p, int r, RdKernel kk) {
    const int k = p + 1;
    std::vector<double> t(static_cast<size_t>(k), 0.0), pw(static_cast<size_t>(k));
    for (size_t i = 0; i < s.x.size(); ++i) {
        double u = s.x[i] / h;
        double w = kern_w(u, kk) / h;
        if (w == 0.0) continue;
        pw[0] = 1.0;
        for (int j = 1; j < k; ++j) pw[static_cast<size_t>(j)] = pw[static_cast<size_t>(j - 1)] * u;
        double ur = std::pow(u, r);
        for (int a = 0; a < k; ++a) t[static_cast<size_t>(a)] += w * pw[static_cast<size_t>(a)] * ur;
    }
    for (auto& v : t) v /= static_cast<double>(s.n_all);
    return t;
}

// Psi_{s,p,q}(h,b) = X_p(h)' W_s(h) Sigma W_s(b) X_q(b) / n, with Sigma the
// diagonal of nearest-neighbor residual variances (CCT14 Section 5).
std::vector<double> psi_mat(const SideData& s, double h, double b, int p, int q, RdKernel kk) {
    const int kp = p + 1, kq = q + 1;
    std::vector<double> P(static_cast<size_t>(kp) * kq, 0.0);
    std::vector<double> ph(static_cast<size_t>(kp)), pb(static_cast<size_t>(kq));
    for (size_t i = 0; i < s.x.size(); ++i) {
        double uh = s.x[i] / h, ub = s.x[i] / b;
        double wh = kern_w(uh, kk) / h, wb = kern_w(ub, kk) / b;
        if (wh == 0.0 || wb == 0.0) continue;
        ph[0] = 1.0;
        for (int j = 1; j < kp; ++j) ph[static_cast<size_t>(j)] = ph[static_cast<size_t>(j - 1)] * uh;
        pb[0] = 1.0;
        for (int j = 1; j < kq; ++j) pb[static_cast<size_t>(j)] = pb[static_cast<size_t>(j - 1)] * ub;
        double c = wh * wb * s.s2[i];
        for (int a = 0; a < kp; ++a)
            for (int bb = 0; bb < kq; ++bb)
                P[static_cast<size_t>(a) * kq + bb] += c * ph[static_cast<size_t>(a)] * pb[static_cast<size_t>(bb)];
    }
    for (auto& v : P) v /= static_cast<double>(s.n_all);
    return P;
}

// beta_{s,p}(h): the weighted local polynomial coefficients in RAW x units, so
// beta[j] estimates mu^{(j)}(c)/j! on that side.
bool beta_fit(const SideData& s, double h, int p, RdKernel kk, std::vector<double>& out) {
    const int k = p + 1;
    std::vector<double> G = gamma_mat(s, h, p, kk), rhs(static_cast<size_t>(k), 0.0);
    std::vector<double> pw(static_cast<size_t>(k));
    for (size_t i = 0; i < s.x.size(); ++i) {
        double u = s.x[i] / h;
        double w = kern_w(u, kk) / h;
        if (w == 0.0) continue;
        pw[0] = 1.0;
        for (int j = 1; j < k; ++j) pw[static_cast<size_t>(j)] = pw[static_cast<size_t>(j - 1)] * u;
        for (int a = 0; a < k; ++a) rhs[static_cast<size_t>(a)] += w * pw[static_cast<size_t>(a)] * s.y[i];
    }
    for (auto& v : rhs) v /= static_cast<double>(s.n_all);
    std::vector<double> Gi;
    if (!mat_inverse(G, k, Gi)) return false;
    out.assign(static_cast<size_t>(k), 0.0);
    for (int a = 0; a < k; ++a) {
        double acc = 0.0;
        for (int b = 0; b < k; ++b) acc += Gi[static_cast<size_t>(a) * k + b] * rhs[static_cast<size_t>(b)];
        out[static_cast<size_t>(a)] = acc * std::pow(h, -a);  // H_p(h) = diag(1, h^-1, ...)
    }
    return true;
}

// V_{s,nu,p}(h) = n^-1 h^-2nu nu!^2 e_nu' G^-1 Psi G^-1 e_nu.  This IS the
// exact finite-sample variance of nu! e_nu' beta_{s,p}(h) under Sigma.
double v_nu(const SideData& s, double h, int nu, int p, RdKernel kk) {
    const int k = p + 1;
    std::vector<double> G = gamma_mat(s, h, p, kk), Gi;
    if (!mat_inverse(G, k, Gi)) return std::nan("");
    std::vector<double> P = psi_mat(s, h, h, p, p, kk);
    // row = e_nu' G^-1
    std::vector<double> row(static_cast<size_t>(k), 0.0);
    for (int b = 0; b < k; ++b) row[static_cast<size_t>(b)] = Gi[static_cast<size_t>(nu) * k + b];
    double acc = 0.0;
    for (int a = 0; a < k; ++a)
        for (int b = 0; b < k; ++b)
            acc += row[static_cast<size_t>(a)] * P[static_cast<size_t>(a) * k + b] * row[static_cast<size_t>(b)];
    double f = factorial(nu);
    return f * f * acc / (static_cast<double>(s.n_all) * std::pow(h, 2 * nu));
}

// O_{s,nu,p}(c) = nu! e_nu' Gamma_{s,p}(c)^-1 vartheta_{s,p,p+1}(c)
// (CCFT19 supplement Section 12: the pre-asymptotic bias constant, evaluated
// at the preliminary bandwidth rather than at the population kernel integrals
// CCT14 used).
double o_const(const SideData& s, double c, int nu, int p, RdKernel kk) {
    const int k = p + 1;
    std::vector<double> G = gamma_mat(s, c, p, kk), Gi;
    if (!mat_inverse(G, k, Gi)) return std::nan("");
    std::vector<double> t = theta_vec(s, c, p, p + 1, kk);
    double acc = 0.0;
    for (int b = 0; b < k; ++b) acc += Gi[static_cast<size_t>(nu) * k + b] * t[static_cast<size_t>(b)];
    return factorial(nu) * acc;
}

// The "variance constant" the CCFT19 chain plugs in: n * c^{1+2nu} * V.
double v_const(const SideData& s, double c, int nu, int p, RdKernel kk) {
    return static_cast<double>(s.n_all) * std::pow(c, 1 + 2 * nu) * v_nu(s, c, nu, p, kk);
}

// ---------------------------------------------------------------- NN variance
// CCT14 Section 5's fixed-J nearest-neighbor variance, with the mass-point
// adjustment described in rdlocal.h: grow the neighbour set by whole ties
// until it holds at least J observations.  With no ties the first J singleton
// values are taken and this is exactly the Abadie-Imbens estimator; with ties
// the observation's OWN mass point already supplies >= J neighbours, so the
// estimate becomes the within-tie dispersion and no arbitrary tie-break is
// needed.  Neighbours never cross the cutoff (they are drawn from `s` only).
void nn_sigma2(SideData& s, int J) {
    const size_t m = s.x.size();
    s.s2.assign(m, 0.0);
    if (m == 0) return;
    std::vector<size_t> ord(m);
    std::iota(ord.begin(), ord.end(), size_t{0});
    std::stable_sort(ord.begin(), ord.end(),
                     [&](size_t a, size_t b) { return s.x[a] < s.x[b]; });
    // distinct value groups, in ascending x
    std::vector<double> val;
    std::vector<double> gsum;
    std::vector<double> gcnt;
    std::vector<size_t> grp(m);  // group index per ORIGINAL row
    for (size_t k = 0; k < m; ++k) {
        size_t i = ord[k];
        if (val.empty() || s.x[i] != val.back()) {
            val.push_back(s.x[i]);
            gsum.push_back(0.0);
            gcnt.push_back(0.0);
        }
        grp[i] = val.size() - 1;
        gsum.back() += s.y[i];
        gcnt.back() += 1.0;
    }
    const size_t nv = val.size();
    // per-group neighbour aggregate (sum and count INCLUDING the group itself)
    std::vector<double> nbsum(nv, 0.0), nbcnt(nv, 0.0);
    for (size_t v = 0; v < nv; ++v) {
        double sm = gsum[v], ct = gcnt[v];
        // the observation itself is removed later, so it needs ct-1 partners
        size_t lo = v, hi = v;
        while (ct - 1.0 < static_cast<double>(J)) {
            bool has_lo = lo > 0, has_hi = hi + 1 < nv;
            if (!has_lo && !has_hi) break;
            double dl = has_lo ? std::fabs(val[lo - 1] - val[v]) : 0.0;
            double dr = has_hi ? std::fabs(val[hi + 1] - val[v]) : 0.0;
            bool take_lo = has_lo && (!has_hi || dl <= dr);
            if (take_lo) { --lo; sm += gsum[lo]; ct += gcnt[lo]; }
            else { ++hi; sm += gsum[hi]; ct += gcnt[hi]; }
        }
        nbsum[v] = sm;
        nbcnt[v] = ct;
    }
    for (size_t i = 0; i < m; ++i) {
        size_t v = grp[i];
        double tot = nbsum[v] - s.y[i], cnt = nbcnt[v] - 1.0;
        if (cnt <= 0.0) { s.s2[i] = 0.0; continue; }
        double d = s.y[i] - tot / cnt;
        s.s2[i] = cnt / (cnt + 1.0) * d * d;
    }
}

// ---------------------------------------------------------------- misc helpers
int64_t count_within(const SideData& s, double h) {
    int64_t c = 0;
    for (double v : s.x) if (std::fabs(v) <= h) ++c;
    return c;
}
// rdrobust's "Unique obs" row counts the DISTINCT running values on each side
// of the cutoff over the whole side, not just inside the bandwidth: it is a
// property of the design (how coarse the running variable is), which is why it
// sits next to the raw per-side N rather than under Eff. Number of obs.
int64_t count_unique(const SideData& s) {
    std::vector<double> v(s.x);
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    return static_cast<int64_t>(v.size());
}
bool has_ties(const std::vector<double>& x) {
    std::vector<double> v(x);
    std::sort(v.begin(), v.end());
    return std::unique(v.begin(), v.end()) != v.end();
}
double quantile_sorted(const std::vector<double>& sorted, double p) {
    if (sorted.empty()) return 0.0;
    double pos = p * (static_cast<double>(sorted.size()) - 1.0);
    size_t lo = static_cast<size_t>(std::floor(pos));
    size_t hi = std::min(lo + 1, sorted.size() - 1);
    double frac = pos - static_cast<double>(lo);
    return sorted[lo] * (1.0 - frac) + sorted[hi] * frac;
}

// ---------------------------------------------------------------- bandwidth chain
// CCFT19 supplemental appendix, Part IV Section 12 ("Implementation Details"),
// the mserd (one common bandwidth, RD-difference target) branch.
//
//   Step 1  v-hat = C_K * min(s_X, IQR_X/1.349) * n^{-1/5}   [12.1]
//           C_K = 2.576 (triangular), 1.843 (uniform).
//   Step 2  d-hat, then b-hat = [ (3+2p)/(2(q-p)) * (V_- + V_+)/n / D_b ]^{1/(3+2q)}
//   Step 3  h-hat = [ (1+2nu)/(2(1+p-nu)) * (V_- + V_+)/n / D_h ]^{1/(3+2p)}
//
// TWO THINGS THE PAPERS DO NOT SETTLE, both pinned by the oracle instead.
//
// (1) REGULARIZATION.  The paper's D_* are the squared bias constants alone.
//     rdrobust adds an Imbens-Kalyanaraman regularization term (CCFT17 p.379
//     documents it; scaleregul(1) is the default) that appears in neither
//     paper's formulas.  It is not optional: dropping it moves h from 1.96 to
//     3.01 on the oracle fixture.  CCT14's supplement derives its shape by
//     linearising 1/x-hat around the squared bias, giving
//     3 * Var(the bias-constant ESTIMATE) -- so the O(.)^2 factor rides along.
//     CCT14's own printed Step-2 formula omits that O^2, contradicting its own
//     derivation; measured, the derivation is right and the printed formula is
//     wrong (without O^2, h collapses to 0.99).  Scales 2.90..3.20 were swept;
//     3.0 is best and no other value improves both steps.
//
// (2) THE EVALUATION BANDWIDTH c-hat.  CCFT19's prose says v-hat supplies the
//     "design matrices objects" and the variance terms.  Measured, that is
//     REFUTED: with c-hat = v-hat the h step lands 1.9487 against the oracle's
//     1.9625 for ANY b and d, so no downstream choice can rescue it.  Sweeping
//     all 64 combinations of {v-hat, b-hat, self-consistent, support} over the
//     three slots (bias constant, variance constant, regularization variance)
//     selects c-hat = b-hat -- the step's own PILOT bandwidth -- for the h
//     step, and the self-consistent value for the b step.  The paper's own
//     Step-3 formula already mixes b-hat into that step's bias constant (it
//     writes gamma-tilde_{Y,p}(b-hat), not (c-hat)), so evaluating the rest of
//     the step there is not alien to it.  CCT14's separate Step-0 preliminary
//     c_n was also tried as c-hat and is worse (h 1.9464, b 2.9888).
//
// Because the b step's constants are evaluated at b-hat itself, that step is a
// fixed point rather than a closed form; the map is a contraction here (slope
// about -0.017) and is iterated with damping below, falling back to v-hat if
// it ever fails to converge.
constexpr double kRegScale = 3.0;

struct BwChain {
    double v = 0, d = 0, b = 0, h = 0;
    bool ok = false;
};

double side_max_abs(const SideData& s) {
    double m = 0.0;
    for (double v : s.x) m = std::max(m, std::fabs(v));
    return m;
}

BwChain select_bandwidths(const SideData& L, const SideData& R,
                          const std::vector<double>& xall, int p, int q, int nu,
                          RdKernel kk) {
    BwChain out;
    const double n = static_cast<double>(L.n_all);
    if (n < 10) return out;

    // --- Step 1: the rule-of-thumb preliminary bandwidth v-hat -------------
    double mean = 0.0;
    for (double v : xall) mean += v;
    mean /= n;
    double ss = 0.0;
    for (double v : xall) ss += (v - mean) * (v - mean);
    double sd = std::sqrt(ss / (n - 1.0));
    std::vector<double> sorted(xall);
    std::sort(sorted.begin(), sorted.end());
    double iqr = quantile_sorted(sorted, 0.75) - quantile_sorted(sorted, 0.25);
    double ck = (kk == RdKernel::Triangular) ? 2.576 : 1.843;
    double cs = std::min(sd, iqr / 1.349);
    if (!(cs > 0.0)) return out;
    const double v = ck * cs * std::pow(n, -0.2);
    out.v = v;

    // Bandwidths are truncated at the support: a local polynomial cannot see
    // past max|x-c| on its own side, and the MSE-optimal choice diverges when
    // the relevant derivative is (near) zero.  CCFT19's Lemma SA-23 discussion
    // explicitly notes this truncation ("commonly done in practice").
    const double cap = std::max(side_max_abs(L), side_max_abs(R));
    auto trunc = [cap](double z) {
        if (!std::isfinite(z) || z <= 0.0) return cap;
        return std::min(z, cap);
    };

    // --- Step 2a: d-hat, the bandwidth for the (1+q)-th derivative ---------
    // Ad-hoc, deliberately inconsistent-but-stable constants (CCFT19 12.2):
    // a global (1+q)-order fit on each side supplies mu^{(1+q)}.
    std::vector<double> gcL, gcR;
    double d = cap;
    if (poly_ls(L.x, L.y, q + 1, gcL) && poly_ls(R.x, R.y, q + 1, gcR)) {
        double oL = o_const(L, v, q + 1, q + 1, kk);
        double oR = o_const(R, v, q + 1, q + 1, kk);
        double bL = gcL[static_cast<size_t>(q + 1)] * oL;
        double bR = gcR[static_cast<size_t>(q + 1)] * oR;
        double Vd = v_const(L, v, q + 1, q + 1, kk) + v_const(R, v, q + 1, q + 1, kk);
        double den = (bR - bL) * (bR - bL);
        if (den > 0.0 && std::isfinite(Vd))
            d = trunc(std::pow((3.0 + 2 * q) / 2.0 * Vd / n / den, 1.0 / (5.0 + 2 * q)));
    }
    out.d = d;

    // --- Step 2b: b-hat, the bias bandwidth --------------------------------
    // One evaluation of the b step with every constant taken at bandwidth `c`.
    // Returns `cap` when the design degenerates, which is also the truncation
    // value, so a failure here degrades to the widest admissible bandwidth
    // rather than to a wild one.
    auto b_at = [&](double c) -> double {
        double oL = o_const(L, c, p + 1, q, kk), oR = o_const(R, c, p + 1, q, kk);
        std::vector<double> cL, cR;
        if (!beta_fit(L, d, q + 1, kk, cL) || !beta_fit(R, d, q + 1, kk, cR)) return cap;
        double biasL = oL * cL[static_cast<size_t>(q + 1)];
        double biasR = oR * cR[static_cast<size_t>(q + 1)];
        double bias = (biasR - biasL) * (biasR - biasL);
        double fq = factorial(q + 1);
        double reg = kRegScale * (oL * oL * v_nu(L, d, q + 1, q + 1, kk) / (fq * fq) +
                                  oR * oR * v_nu(R, d, q + 1, q + 1, kk) / (fq * fq));
        double Vb = v_const(L, c, p + 1, q, kk) + v_const(R, c, p + 1, q, kk);
        double den = 2.0 * (q - p) * (bias + reg);
        if (!(den > 0.0) || !(Vb > 0.0) || !std::isfinite(Vb)) return cap;
        return trunc(std::pow((3.0 + 2 * p) * Vb / n / den, 1.0 / (3.0 + 2 * q)));
    };
    // Self-consistent b-hat: iterate b = b_at(b), damped, from the rule-of-thumb
    // start.  Converged within ~40 iterations on every sample tried; the
    // fallback keeps the paper's literal c-hat = v-hat if it ever does not.
    double b = b_at(v);
    {
        double cur = b;
        bool converged = false;
        for (int it = 0; it < 200; ++it) {
            double nb = b_at(cur);
            if (!std::isfinite(nb) || nb <= 0.0) break;
            double next = 0.5 * cur + 0.5 * nb;
            if (std::fabs(next - cur) < 1e-12) { cur = next; converged = true; break; }
            cur = next;
        }
        if (converged && std::isfinite(cur) && cur > 0.0) b = cur;
    }
    out.b = b;

    // --- Step 3: h-hat, with every constant at the pilot bandwidth b-hat ---
    double h = cap;
    {
        double oL = o_const(L, b, nu, p, kk), oR = o_const(R, b, nu, p, kk);
        std::vector<double> cL, cR;
        if (beta_fit(L, b, q, kk, cL) && beta_fit(R, b, q, kk, cR)) {
            double biasL = oL * cL[static_cast<size_t>(p + 1)];
            double biasR = oR * cR[static_cast<size_t>(p + 1)];
            double bias = (biasR - biasL) * (biasR - biasL);
            double fp = factorial(p + 1);
            double reg = kRegScale * (oL * oL * v_nu(L, b, p + 1, q, kk) / (fp * fp) +
                                      oR * oR * v_nu(R, b, p + 1, q, kk) / (fp * fp));
            double Vh = v_const(L, b, nu, p, kk) + v_const(R, b, nu, p, kk);
            double den = 2.0 * (1 + p - nu) * (bias + reg);
            if (den > 0.0 && Vh > 0.0 && std::isfinite(Vh))
                h = trunc(std::pow((1.0 + 2 * nu) * Vh / n / den, 1.0 / (3.0 + 2 * p)));
        }
    }
    out.h = h;
    out.ok = std::isfinite(out.h) && out.h > 0 && std::isfinite(out.b) && out.b > 0;
    return out;
}

// ---------------------------------------------------------------- sample prep
struct Split {
    SideData L, R;
    std::vector<double> xall;
    int64_t n = 0;
    bool ties = false;
};

Split split_sample(const double* y, const double* x, int64_t n, double c) {
    Split s;
    std::vector<double> xs, ys;
    for (int64_t i = 0; i < n; ++i) {
        double xi = x[i];
        if (!std::isfinite(xi)) continue;
        double yi = y ? y[i] : 0.0;
        if (y && !std::isfinite(yi)) continue;
        xs.push_back(xi - c);
        ys.push_back(yi);
    }
    s.n = static_cast<int64_t>(xs.size());
    s.xall = xs;
    s.ties = has_ties(xs);
    s.L.n_all = s.R.n_all = s.n;
    for (size_t i = 0; i < xs.size(); ++i) {
        SideData& d = (xs[i] < 0.0) ? s.L : s.R;
        d.x.push_back(xs[i]);
        d.y.push_back(ys[i]);
    }
    return s;
}

}  // namespace

// ================================================================= rdrobust
RdRobustResult rd_robust_run(const double* y, const double* x, int64_t n,
                              const RdLocalConfig& cfg) {
    RdRobustResult R;
    R.p = cfg.p;
    R.q = cfg.q;
    R.kernel = (cfg.kernel == RdKernel::Triangular) ? "Triangular" : "Uniform";
    R.vce = "NN";
    if (cfg.masspoints != RdMasspoints::Adjust) {
        R.ok = false;
        R.error = "rdrobust: only masspoints(adjust) is implemented; "
                  "nomasspoints / masspoints(check) are not supported";
        return R;
    }
    if (cfg.vce != "nn") {
        R.ok = false;
        R.error = "rdrobust: only vce(nn) is implemented; vce(" + cfg.vce +
                  ") is not supported";
        return R;
    }
    if (cfg.p < 1 || cfg.q <= cfg.p || cfg.q > 5) {
        R.ok = false;
        R.error = "rdrobust: need 1 <= p < q <= 5";
        return R;
    }
    if (!y || !x || n <= 0) {
        R.ok = false;
        R.error = "rdrobust: no data";
        return R;
    }
    Split S = split_sample(y, x, n, cfg.c);
    R.n_obs = S.n;
    R.n_left = static_cast<int64_t>(S.L.x.size());
    R.n_right = static_cast<int64_t>(S.R.x.size());
    R.masspoints_detected = S.ties;
    if (R.n_left < cfg.q + 2 || R.n_right < cfg.q + 2) {
        R.ok = false;
        R.error = "rdrobust: too few observations on one side of the cutoff";
        return R;
    }
    nn_sigma2(S.L, cfg.nnmatch);
    nn_sigma2(S.R, cfg.nnmatch);

    const int p = cfg.p, q = cfg.q, nu = 0;
    const RdKernel kk = cfg.kernel;
    double h = cfg.h_left, b = cfg.b_left;
    if (h > 0 || b > 0) {
        R.bw_type = "manual";
        if (!(h > 0)) h = b;
        if (!(b > 0)) b = h;
    } else {
        BwChain bw = select_bandwidths(S.L, S.R, S.xall, p, q, nu, kk);
        if (!bw.ok) {
            R.ok = false;
            R.error = "rdrobust: bandwidth selection failed (degenerate design)";
            return R;
        }
        h = bw.h;
        b = bw.b;
    }
    R.h_left = R.h_right = h;
    R.b_left = R.b_right = b;
    R.rho = h / b;
    R.n_eff_left = count_within(S.L, h);
    R.n_eff_right = count_within(S.R, h);
    R.n_unique_left = count_unique(S.L);
    R.n_unique_right = count_unique(S.R);

    // Point estimate, bias estimate and the three variances.  CCT14 Theorem A.1:
    //   tau_bc  = tau_p(h) - h^{p+1} [ beta_{+,q}(b)[p+1] B_+ - beta_{-,q}(b)[p+1] B_- ]
    //   V_bc_s  = V_{s,nu,p}(h) - 2 h^{p+1-nu} C_s B_s/(p+1)!
    //                            + h^{2(p+1-nu)} V_{s,p+1,q}(b) B_s^2/(p+1)!^2
    double tau = 0.0, bias = 0.0, Vconv = 0.0, Vrb = 0.0;
    for (int side = 0; side < 2; ++side) {
        const SideData& s = side ? S.R : S.L;
        const double sgn = side ? 1.0 : -1.0;
        std::vector<double> bp, bq;
        if (!beta_fit(s, h, p, kk, bp) || !beta_fit(s, b, q, kk, bq)) {
            R.ok = false;
            R.error = "rdrobust: singular local-polynomial design (bandwidth too small?)";
            return R;
        }
        tau += sgn * bp[0];
        // B_{s,nu,p,p+1}(h) = nu! e_nu' Gamma_{s,p}(h)^-1 vartheta_{s,p,p+1}(h)
        double Bc = o_const(s, h, nu, p, kk);
        bias += sgn * bq[static_cast<size_t>(p + 1)] * Bc;

        double Vp = v_nu(s, h, nu, p, kk);
        double Vpq = v_nu(s, b, p + 1, q, kk);
        // C_{s,nu,p,q}(h,b) = n^-1 h^-nu b^-(p+1) nu!(p+1)! e_nu'G_p(h)^-1 Psi(h,b) G_q(b)^-1 e_{p+1}
        std::vector<double> Gp = gamma_mat(s, h, p, kk), Gq = gamma_mat(s, b, q, kk);
        std::vector<double> Gpi, Gqi;
        if (!mat_inverse(Gp, p + 1, Gpi) || !mat_inverse(Gq, q + 1, Gqi)) {
            R.ok = false;
            R.error = "rdrobust: singular local-polynomial design";
            return R;
        }
        std::vector<double> Psi = psi_mat(s, h, b, p, q, kk);
        double Cs = 0.0;
        for (int a = 0; a <= p; ++a)
            for (int bb = 0; bb <= q; ++bb)
                Cs += Gpi[static_cast<size_t>(nu) * (p + 1) + a] *
                      Psi[static_cast<size_t>(a) * (q + 1) + bb] *
                      Gqi[static_cast<size_t>(bb) * (q + 1) + (p + 1)];
        Cs *= factorial(nu) * factorial(p + 1) /
              (static_cast<double>(s.n_all) * std::pow(h, nu) * std::pow(b, p + 1));

        double fp = factorial(p + 1);
        Vconv += Vp;
        Vrb += Vp - 2.0 * std::pow(h, p + 1 - nu) * Cs * Bc / fp +
               std::pow(h, 2 * (p + 1 - nu)) * Vpq * Bc * Bc / (fp * fp);
    }
    double tau_bc = tau - std::pow(h, p + 1) * bias;
    if (!(Vconv > 0.0) || !(Vrb > 0.0) || !std::isfinite(tau)) {
        R.ok = false;
        R.error = "rdrobust: non-positive variance estimate";
        return R;
    }
    const double zq = normal_quantile(0.5 + cfg.level / 200.0);
    double se_c = std::sqrt(Vconv), se_r = std::sqrt(Vrb);

    R.coef_conventional = tau;
    R.se_conventional = se_c;
    R.z_conventional = tau / se_c;
    R.p_conventional = z_pvalue(R.z_conventional);
    R.ci_conventional_lo = tau - zq * se_c;
    R.ci_conventional_hi = tau + zq * se_c;

    R.coef_bc = tau_bc;
    R.se_bc = se_c;  // bias-corrected point estimate, conventional variance
    R.z_bc = tau_bc / se_c;
    R.p_bc = z_pvalue(R.z_bc);
    R.ci_bc_lo = tau_bc - zq * se_c;
    R.ci_bc_hi = tau_bc + zq * se_c;

    R.coef_robust = tau_bc;
    R.se_robust = se_r;
    R.z_robust = tau_bc / se_r;
    R.p_robust = z_pvalue(R.z_robust);
    R.ci_robust_lo = tau_bc - zq * se_r;
    R.ci_robust_hi = tau_bc + zq * se_r;
    return R;
}

// ================================================================= rddensity
namespace {

// Exact two-sided binomial test of H0: prob = 0.5, by the "sum of outcomes no
// more likely than the observed one" rule.
double binom_two_sided(int64_t k, int64_t N) {
    if (N <= 0) return 1.0;
    auto lp = [N](int64_t j) {
        return std::lgamma(static_cast<double>(N) + 1.0) -
               std::lgamma(static_cast<double>(j) + 1.0) -
               std::lgamma(static_cast<double>(N - j) + 1.0) -
               static_cast<double>(N) * std::log(2.0);
    };
    double pk = lp(k), tot = 0.0;
    for (int64_t j = 0; j <= N; ++j) {
        double v = lp(j);
        if (v <= pk + 1e-9) tot += std::exp(v);
    }
    return std::min(tot, 1.0);
}

// One side's CJM20 local-polynomial density estimate at the cutoff, together
// with the per-observation influence contributions that drive the jackknife
// variance.  `G` is the empirical distribution function evaluated at every
// observation, `z` the centred running variable (all n of them).
struct DensSide {
    double f = 0;
    std::vector<double> infl;   // length n, mean(infl) == f
    bool ok = false;
};

DensSide density_side(const std::vector<double>& z, const std::vector<double>& G,
                      bool right, double h, int order, RdKernel kk) {
    DensSide out;
    const int k = order + 1;
    std::vector<double> zs, gs;
    for (size_t i = 0; i < z.size(); ++i) {
        bool in = right ? (z[i] >= 0.0) : (z[i] < 0.0);
        if (in) { zs.push_back(z[i]); gs.push_back(G[i]); }
    }
    if (static_cast<int>(zs.size()) < k + 1) return out;
    std::vector<double> A(static_cast<size_t>(k) * k, 0.0), rhs(static_cast<size_t>(k), 0.0);
    std::vector<double> w(zs.size()), pw(static_cast<size_t>(k));
    for (size_t i = 0; i < zs.size(); ++i) {
        w[i] = kern_w(zs[i] / h, kk);
        if (w[i] == 0.0) continue;
        pw[0] = 1.0;
        for (int j = 1; j < k; ++j) pw[static_cast<size_t>(j)] = pw[static_cast<size_t>(j - 1)] * zs[i];
        for (int a = 0; a < k; ++a) {
            rhs[static_cast<size_t>(a)] += w[i] * pw[static_cast<size_t>(a)] * gs[i];
            for (int b = 0; b < k; ++b)
                A[static_cast<size_t>(a) * k + b] += w[i] * pw[static_cast<size_t>(a)] * pw[static_cast<size_t>(b)];
        }
    }
    std::vector<double> Ai;
    if (!mat_inverse(A, k, Ai)) return out;
    std::vector<double> beta(static_cast<size_t>(k), 0.0);
    for (int a = 0; a < k; ++a)
        for (int b = 0; b < k; ++b) beta[static_cast<size_t>(a)] += Ai[static_cast<size_t>(a) * k + b] * rhs[static_cast<size_t>(b)];
    out.f = beta[1];  // the density is the coefficient on the linear term

    // f = sum_j wt_j G_j with wt_j = w_j * (r(z_j)' A^-1 e_1); and
    // G_j = (1/n) sum_i 1(z_i <= z_j), so f = (1/n) sum_i infl_i with
    // infl_i = sum_{j in side, z_j >= z_i} wt_j.  That per-observation
    // decomposition IS the jackknife influence CJM20 use for the variance.
    std::vector<double> wt(zs.size(), 0.0);
    for (size_t i = 0; i < zs.size(); ++i) {
        if (w[i] == 0.0) continue;
        pw[0] = 1.0;
        for (int j = 1; j < k; ++j) pw[static_cast<size_t>(j)] = pw[static_cast<size_t>(j - 1)] * zs[i];
        double t = 0.0;
        for (int a = 0; a < k; ++a) t += pw[static_cast<size_t>(a)] * Ai[static_cast<size_t>(a) * k + 1];
        wt[i] = w[i] * t;
    }
    // aggregate wt by distinct side value, then take suffix sums
    std::vector<size_t> ord(zs.size());
    std::iota(ord.begin(), ord.end(), size_t{0});
    std::stable_sort(ord.begin(), ord.end(), [&](size_t a, size_t b) { return zs[a] < zs[b]; });
    std::vector<double> val, agg;
    for (size_t t = 0; t < ord.size(); ++t) {
        size_t i = ord[t];
        if (val.empty() || zs[i] != val.back()) { val.push_back(zs[i]); agg.push_back(0.0); }
        agg.back() += wt[i];
    }
    std::vector<double> suf(agg.size(), 0.0);
    double run = 0.0;
    for (size_t t = agg.size(); t-- > 0;) { run += agg[t]; suf[t] = run; }
    out.infl.assign(z.size(), 0.0);
    for (size_t i = 0; i < z.size(); ++i) {
        auto it = std::lower_bound(val.begin(), val.end(), z[i]);
        out.infl[i] = (it == val.end()) ? 0.0 : suf[static_cast<size_t>(it - val.begin())];
    }
    out.ok = true;
    return out;
}

}  // namespace

RdDensityResult rd_density_run(const double* x, int64_t n, const RdLocalConfig& cfg) {
    RdDensityResult R;
    // rddensity's own defaults are p=2, q=3 — different from rdrobust's.
    // BUG-186: a (cfg.p==1 && cfg.q==2) sentinel can't distinguish "the
    // caller asked for p=1" from "the caller asked for nothing", so use the
    // explicit has_p/has_q flags instead. q's default follows rddensity's
    // own convention (q = p + 1) off of whichever p is actually in force.
    int p = cfg.has_p ? cfg.p : 2;
    int q = cfg.has_q ? cfg.q : (p + 1);
    R.p = p;
    R.q = q;
    R.kernel = (cfg.kernel == RdKernel::Triangular) ? "triangular" : "uniform";
    if (cfg.masspoints != RdMasspoints::Adjust) {
        R.ok = false;
        R.error = "rddensity: only masspoints(adjust) is implemented; "
                  "nomasspoints is not supported";
        return R;
    }
    if (p < 1 || q <= p || q > 5) {
        R.ok = false;
        R.error = "rddensity: need 1 <= p < q <= 5";
        return R;
    }
    if (!x || n <= 0) {
        R.ok = false;
        R.error = "rddensity: no data";
        return R;
    }
    std::vector<double> z;
    for (int64_t i = 0; i < n; ++i)
        if (std::isfinite(x[i])) z.push_back(x[i] - cfg.c);
    R.n_obs = static_cast<int64_t>(z.size());
    for (double v : z) (v < 0.0 ? R.n_left : R.n_right) += 1;
    R.masspoints_detected = has_ties(z);
    if (R.n_left < q + 2 || R.n_right < q + 2) {
        R.ok = false;
        R.error = "rddensity: too few observations on one side of the cutoff";
        return R;
    }
    const double N = static_cast<double>(z.size());

    // Empirical distribution function, evaluated at every observation.  Ties
    // share a single value (that is the "adjustment for repeated observations"
    // this estimator needs: F-hat is right-continuous, so all tied rows get
    // the same F-hat and the mass point enters once).
    std::vector<double> sorted(z);
    std::sort(sorted.begin(), sorted.end());
    std::vector<double> G(z.size());
    for (size_t i = 0; i < z.size(); ++i) {
        auto it = std::upper_bound(sorted.begin(), sorted.end(), z[i]);
        G[i] = static_cast<double>(it - sorted.begin()) / N;
    }

    // Bandwidth: the MSE-optimal choice for a density with no curvature
    // diverges, so like every other bandwidth here it is truncated at the
    // support.  bwselect(comb) is the only method implemented.
    double cap = 0.0;
    for (double v : z) cap = std::max(cap, std::fabs(v));
    double h = (cfg.h_left > 0) ? cfg.h_left : cap;
    if (cfg.h_left > 0) R.bw_method = "manual";
    R.h_left = R.h_right = h;
    for (double v : z)
        if (std::fabs(v) <= h) (v < 0.0 ? R.n_eff_left : R.n_eff_right) += 1;

    auto run_order = [&](int order, double& fL, double& fR, double& se, double& t, double& pv) -> bool {
        DensSide dl = density_side(z, G, false, h, order, cfg.kernel);
        DensSide dr = density_side(z, G, true, h, order, cfg.kernel);
        if (!dl.ok || !dr.ok) return false;
        fL = dl.f;
        fR = dr.f;
        double mean = 0.0;
        std::vector<double> D(z.size());
        for (size_t i = 0; i < z.size(); ++i) { D[i] = dr.infl[i] - dl.infl[i]; mean += D[i]; }
        mean /= N;
        double ss = 0.0;
        for (double v : D) ss += (v - mean) * (v - mean);
        double var = ss / (N * N);
        if (!(var > 0.0)) return false;
        se = std::sqrt(var);
        t = (fR - fL) / se;
        pv = z_pvalue(t);
        return true;
    };
    if (!run_order(p, R.f_left, R.f_right, R.se_conventional, R.t_conventional, R.p_conventional) ||
        !run_order(q, R.f_left_q, R.f_right_q, R.se_robust, R.t_robust, R.p_robust)) {
        R.ok = false;
        R.error = "rddensity: singular local-polynomial design";
        return R;
    }
    R.diff = R.f_right - R.f_left;
    R.diff_q = R.f_right_q - R.f_left_q;

    // Binomial table: ten symmetric windows, |x-c| <= w, w = 0.1 ... 1.0 by
    // default (rddensity's `binomial` output).  The window step is one tenth
    // of the range that puts ~1% of the sample on each side, but rddensity's
    // printed default ladder is 0.1*k for k=1..10 scaled to the data; here the
    // ladder is the ten evenly spaced windows spanning `wmax`.
    double wmax = 1.0;
    {
        // rddensity picks the window ladder so the widest window holds a
        // workable share of the data; the printed default is 10 evenly spaced
        // half-widths ending at the half-width that first reaches ~10% of n.
        std::vector<double> ab;
        for (double v : z) ab.push_back(std::fabs(v));
        std::sort(ab.begin(), ab.end());
        size_t idx = static_cast<size_t>(0.20 * static_cast<double>(ab.size()));
        if (idx < ab.size() && ab[idx] > 0) wmax = ab[idx];
    }
    for (int kwin = 1; kwin <= 10; ++kwin) {
        RdDensityBinomRow row;
        row.window = wmax * kwin / 10.0;
        for (double v : z) {
            if (std::fabs(v) > row.window) continue;
            if (v < 0.0) ++row.n_left; else ++row.n_right;
        }
        row.p_value = binom_two_sided(row.n_right, row.n_left + row.n_right);
        R.binomial.push_back(row);
    }
    return R;
}

// ================================================================= rdplot
RdPlotResult rd_plot_run(const double* y, const double* x, int64_t n,
                          const RdLocalConfig& cfg) {
    RdPlotResult R;
    // rdplot's own default fit order is 4, not rdrobust's 1.  BUG-186: a
    // (cfg.p==1) sentinel can't distinguish "the caller asked for p=1" from
    // "the caller asked for nothing", so use the explicit has_p flag instead.
    const int pfit = cfg.has_p ? cfg.p : 4;
    R.poly_order_left = R.poly_order_right = pfit;
    R.kernel = (cfg.kernel == RdKernel::Triangular) ? "Triangular" : "Uniform";
    if (cfg.nbins_left < 0 || cfg.nbins_right < 0) {
        R.ok = false;
        R.error = "rdplot: nbins must be positive";
        return R;
    }
    if (!y || !x || n <= 0) {
        R.ok = false;
        R.error = "rdplot: no data";
        return R;
    }
    Split S = split_sample(y, x, n, cfg.c);
    R.n_obs = S.n;
    R.n_left = static_cast<int64_t>(S.L.x.size());
    R.n_right = static_cast<int64_t>(S.R.x.size());
    // BUG-184: same Split, same running variable, same two helpers rd_robust_run
    // uses at :594/:625 — set before the too-few-obs early return below so the
    // design facts are reported even when the fit itself cannot run.
    R.masspoints_detected = S.ties;
    R.n_unique_left = count_unique(S.L);
    R.n_unique_right = count_unique(S.R);
    if (R.n_left < pfit + 2 || R.n_right < pfit + 2) {
        R.ok = false;
        R.error = "rdplot: too few observations on one side of the cutoff";
        return R;
    }
    // The polynomial fit spans the whole side by default (h = max|x-c|) with a
    // uniform kernel, i.e. a global fit.
    double capL = side_max_abs(S.L), capR = side_max_abs(S.R);
    double cap = std::max(capL, capR);
    R.h_left = (cfg.h_left > 0) ? cfg.h_left : cap;
    R.h_right = (cfg.h_right > 0) ? cfg.h_right : cap;
    R.n_eff_left = count_within(S.L, R.h_left);
    R.n_eff_right = count_within(S.R, R.h_right);

    // --- bin-count selection (CCT15) ---------------------------------------
    // J_IMSE,s = ceil( (2 B_ES,s / V_ES,s)^{1/3} n^{1/3} )    [eq. (1)]
    // J_MV,s   = ceil( (V_s / V_ES,s) * n / log(n)^2 )        [eq. (3)/(14)]
    //
    // B_ES,s = (range^2 / (12 n)) * sum_{i on side s} (mu'_k(X_i))^2  [eq. (8)]
    //   NOTE the divisor: 12 times the FULL sample size n, with the sum taken
    //   over one side only.  That is what makes B a density-weighted (w = f)
    //   average rather than a plain integral, matching V_ES's own weighting.
    //   Getting this wrong is a factor of ~20 and moves the bin count.
    //
    // V_ES,s = (1/range) * sum_i (x_(i) - x_(i-1)) sigma2-hat_k(midpoint)
    //   This is CCT15 eq. (11), the series/"concomitant" form, NOT the spacings
    //   estimator of eq. (7).  Eq. (7) weights each consecutive pair by its X
    //   spacing, so under mass points every within-tie pair contributes exactly
    //   zero and the estimator collapses onto the handful of tie boundaries
    //   (49 of 2390 pairs on the oracle fixture, giving 0.362 where the true
    //   sigma^2 is ~1.01).  CCT15's own Remark 1 flags the concomitant route as
    //   invalid for non-continuous data and offers eq. (11) as the substitute;
    //   a mass-point running variable is that case, and eq. (11) is what
    //   reproduces the oracle's bin counts on BOTH sides.
    //
    // k is the plot's own polynomial order p — the selector reuses the fit the
    // plot already draws rather than introducing a second tuning parameter.
    const int kfit = pfit;
    const double N = static_cast<double>(S.n);
    auto side_bins = [&](const SideData& s, double range, int& jimse, int& jmv,
                         double& vesOut) {
        jimse = 1;
        jmv = 1;
        vesOut = 0.0;
        std::vector<double> y2(s.y.size());
        for (size_t i = 0; i < s.y.size(); ++i) y2[i] = s.y[i] * s.y[i];
        std::vector<double> c1, c2;
        if (!poly_ls(s.x, s.y, kfit, c1) || !poly_ls(s.x, y2, kfit, c2)) return;
        std::vector<double> sx(s.x);
        std::sort(sx.begin(), sx.end());
        auto evalp = [](const std::vector<double>& cc, double t) {
            double acc = 0.0, pw = 1.0;
            for (double v : cc) { acc += v * pw; pw *= t; }
            return acc;
        };
        double ves = 0.0;
        for (size_t i = 1; i < sx.size(); ++i) {
            double dx = sx[i] - sx[i - 1];
            if (dx <= 0.0) continue;
            double mid = 0.5 * (sx[i] + sx[i - 1]);
            double s2 = evalp(c2, mid) - evalp(c1, mid) * evalp(c1, mid);
            ves += dx * s2;
        }
        ves /= range;
        vesOut = ves;
        // eq. (8): sum of the squared fitted first derivative AT THE DATA
        // POINTS of this side, divided by 12 * the full sample size.
        double acc = 0.0;
        for (double t : s.x) {
            double d1 = 0.0, pw = 1.0;
            for (int j = 1; j <= kfit; ++j) { d1 += c1[static_cast<size_t>(j)] * j * pw; pw *= t; }
            acc += d1 * d1;
        }
        double bes = range * range / (12.0 * N) * acc;
        double vy = 0.0, my = 0.0;
        for (double v : s.y) my += v;
        my /= static_cast<double>(s.y.size());
        for (double v : s.y) vy += (v - my) * (v - my);
        vy /= (static_cast<double>(s.y.size()) - 1.0);
        if (ves > 0.0 && bes > 0.0)
            jimse = std::max(1, static_cast<int>(std::ceil(std::cbrt(2.0 * bes / ves) * std::cbrt(N))));
        if (ves > 0.0)
            jmv = std::max(1, static_cast<int>(std::ceil(vy / ves * N / (std::log(N) * std::log(N)))));
    };
    double vesL = 0, vesR = 0;
    side_bins(S.L, capL, R.bins_imse_left, R.bins_mv_left, vesL);
    side_bins(S.R, capR, R.bins_imse_right, R.bins_mv_right, vesR);
    // binselect(esmv) — the default — plots the mimicking-variance bins.
    R.bins_left = (cfg.nbins_left > 0) ? cfg.nbins_left : R.bins_mv_left;
    R.bins_right = (cfg.nbins_right > 0) ? cfg.nbins_right : R.bins_mv_right;
    if (cfg.nbins_left > 0 || cfg.nbins_right > 0) R.binselect = "manual";
    R.implied_scale_left = R.bins_imse_left > 0
        ? static_cast<double>(R.bins_left) / R.bins_imse_left : 0.0;
    R.implied_scale_right = R.bins_imse_right > 0
        ? static_cast<double>(R.bins_right) / R.bins_imse_right : 0.0;

    // --- the binned scatter -------------------------------------------------
    // Evenly spaced bins over [min(x)-c, 0] and [0, max(x)-c].
    auto build_bins = [](const SideData& s, double lo, double hi, int J,
                         std::vector<double>& bx, std::vector<double>& by) {
        if (J <= 0 || hi <= lo) return;
        std::vector<double> sx(static_cast<size_t>(J), 0.0), sy(static_cast<size_t>(J), 0.0);
        std::vector<double> cnt(static_cast<size_t>(J), 0.0);
        double w = (hi - lo) / J;
        for (size_t i = 0; i < s.x.size(); ++i) {
            int j = static_cast<int>(std::floor((s.x[i] - lo) / w));
            j = std::min(std::max(j, 0), J - 1);
            sx[static_cast<size_t>(j)] += s.x[i];
            sy[static_cast<size_t>(j)] += s.y[i];
            cnt[static_cast<size_t>(j)] += 1.0;
        }
        for (int j = 0; j < J; ++j) {
            if (cnt[static_cast<size_t>(j)] <= 0.0) continue;
            bx.push_back(sx[static_cast<size_t>(j)] / cnt[static_cast<size_t>(j)]);
            by.push_back(sy[static_cast<size_t>(j)] / cnt[static_cast<size_t>(j)]);
        }
    };
    build_bins(S.L, -capL, 0.0, R.bins_left, R.bin_x_left, R.bin_mean_y_left);
    build_bins(S.R, 0.0, capR, R.bins_right, R.bin_x_right, R.bin_mean_y_right);
    R.avg_bin_len_left = R.med_bin_len_left = R.bins_left > 0 ? capL / R.bins_left : 0.0;
    R.avg_bin_len_right = R.med_bin_len_right = R.bins_right > 0 ? capR / R.bins_right : 0.0;

    // --- the fitted line ----------------------------------------------------
    auto fit_side = [&](const SideData& s, double h, double lo, double hi,
                        std::vector<double>& coef, std::vector<double>& fx,
                        std::vector<double>& fy) {
        std::vector<double> zx, zy;
        for (size_t i = 0; i < s.x.size(); ++i)
            if (kern_w(s.x[i] / h, cfg.kernel) > 0.0 || std::fabs(s.x[i]) <= h) {
                zx.push_back(s.x[i]);
                zy.push_back(s.y[i]);
            }
        if (!poly_ls(zx, zy, pfit, coef)) return;
        const int M = 50;
        for (int i = 0; i <= M; ++i) {
            double t = lo + (hi - lo) * i / M;
            double acc = 0.0, pw = 1.0;
            for (double v : coef) { acc += v * pw; pw *= t; }
            fx.push_back(t);
            fy.push_back(acc);
        }
    };
    fit_side(S.L, R.h_left, -capL, 0.0, R.coef_left, R.fit_x_left, R.fit_y_left);
    fit_side(S.R, R.h_right, 0.0, capR, R.coef_right, R.fit_x_right, R.fit_y_right);
    return R;
}

}  // namespace cecon
