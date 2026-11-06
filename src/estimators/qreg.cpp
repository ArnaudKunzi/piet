// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "estimators/qreg.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "core/linalg.h"

namespace cecon {
namespace {

double npdf(double x) { return 0.3989422804014327 * std::exp(-0.5 * x * x); }
double ncdf(double x) { return 0.5 * std::erfc(-x * 0.7071067811865476); }

// Inverse standard normal (Acklam) + one Halley refinement (~1e-15).
double ninv(double p) {
    static const double a[] = {-3.969683028665376e+01, 2.209460984245205e+02, -2.759285104469687e+02,
                               1.383577518672690e+02, -3.066479806614716e+01, 2.506628277459239e+00};
    static const double b[] = {-5.447609879822406e+01, 1.615858368580409e+02, -1.556989798598866e+02,
                               6.680131188771972e+01, -1.328068155288572e+01};
    static const double c[] = {-7.784894002430293e-03, -3.223964580411365e-01, -2.400758277161838e+00,
                               -2.549732539343734e+00, 4.374664141464968e+00, 2.938163982698783e+00};
    static const double d[] = {7.784695709041462e-03, 3.224671290700398e-01, 2.445134137142996e+00,
                               3.754408661907416e+00};
    double plow = 0.02425, phigh = 1 - plow, q, r, x;
    if (p < plow) {
        q = std::sqrt(-2 * std::log(p));
        x = (((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) /
            ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1);
    } else if (p <= phigh) {
        q = p - 0.5; r = q * q;
        x = (((((a[0] * r + a[1]) * r + a[2]) * r + a[3]) * r + a[4]) * r + a[5]) * q /
            (((((b[0] * r + b[1]) * r + b[2]) * r + b[3]) * r + b[4]) * r + 1);
    } else {
        q = std::sqrt(-2 * std::log(1 - p));
        x = -(((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) /
            ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1);
    }
    double e = ncdf(x) - p, u = e * 2.5066282746310002 * std::exp(0.5 * x * x);
    return x - u / (1 + 0.5 * x * u);
}

// Hall-Sheather bandwidth (depends on level): h = n^(-1/3) z^(2/3) [1.5 phi(zt)^2/(2 zt^2+1)]^(1/3)
double hall_sheather(int64_t n, double tau, double level) {
    double alpha = 1.0 - level / 100.0;
    double zt = ninv(tau);
    double za = ninv(1.0 - alpha / 2.0);
    return std::pow((double)n, -1.0 / 3.0) * std::pow(za, 2.0 / 3.0) *
           std::pow(1.5 * npdf(zt) * npdf(zt) / (2.0 * zt * zt + 1.0), 1.0 / 3.0);
}

// The quantiles tau -/+ h the fitted density estimator differences, kept inside (0,1).
void bracket_tau(double tau, double h, double& tl, double& tu) {
    tl = tau - h;
    tu = tau + h;
    if (tl < 1e-6) tl = 1e-6;
    if (tu > 1 - 1e-6) tu = 1 - 1e-6;
}

// Σ w_i ρ_τ(y_i - x_i'β) = Σ ρ_τ(w_i y_i - w_i x_i'β) for w_i >= 0 (ρ_τ is positively
// homogeneous), so the pweighted problem is the unweighted one on the scaled rows.
std::vector<double> qreg_solve_w(const double* X, int64_t n, int k, const double* y,
                                 const double* w, double tau, bool* ok) {
    if (w == nullptr) return qreg_solve(X, n, k, y, tau, ok);
    std::vector<double> Xw(static_cast<size_t>(n) * k), yw(n);
    for (int64_t i = 0; i < n; ++i) yw[i] = w[i] * y[i];
    for (int p = 0; p < k; ++p)
        for (int64_t i = 0; i < n; ++i)
            Xw[static_cast<int64_t>(p) * n + i] = w[i] * X[static_cast<int64_t>(p) * n + i];
    return qreg_solve(Xw.data(), n, k, yw.data(), tau, ok);
}

}  // namespace

// Frisch-Newton (Mehrotra predictor-corrector) on the QR dual:
//   max y'a  s.t. X'a = (1-tau) X'1,  0 <= a <= 1.   beta = multiplier of X'a=b.
// Start a=(1-tau)1 is feasible (X'a=b exactly) and strictly interior.
std::vector<double> qreg_solve(const double* X, int64_t n, int k, const double* y, double tau, bool* ok) {
    std::vector<double> a(n), s(n), z(n, 1.0), w(n, 1.0), beta(k, 0.0), b(k, 0.0);
    for (int j = 0; j < k; ++j) {
        const double* Xj = X + static_cast<int64_t>(j) * n;
        double t = 0; for (int64_t i = 0; i < n; ++i) t += Xj[i];
        b[j] = (1.0 - tau) * t;
    }
    for (int64_t i = 0; i < n; ++i) { a[i] = 1.0 - tau; s[i] = tau; }

    std::vector<double> Xb(n), rho(n), D(n), da(n), dz(n), dw(n);
    std::vector<double> daa(n), dza(n), dwa(n);  // affine directions
    std::vector<double> A(static_cast<size_t>(k) * k), rhs(k), dbeta(k), dbeta0(k);
    // spd_inverse destroys its input, so solve_dir must hand it a COPY of A — that copy
    // is load-bearing and stays. What does not need to stay is a fresh k×k allocation
    // for it on each of the up-to-1000 solve_dir calls a fit makes, so the copy lands in
    // a hoisted buffer instead (assignment onto an equally-sized vector reuses storage
    // and copies the same k² doubles the copy-constructor did).
    std::vector<double> Ai(static_cast<size_t>(k) * k);
    const double BIG = 0.9999;
    bool good = false;

    auto solve_dir = [&](const std::vector<double>& rhov, std::vector<double>& dadir,
                         std::vector<double>& dbe) {
        // (X'DX) dbe = X'(D rho) ; da = D(rho - X dbe)   (rp == 0, feasible start)
        std::fill(A.begin(), A.end(), 0.0);
        std::fill(rhs.begin(), rhs.end(), 0.0);
        for (int64_t i = 0; i < n; ++i) {
            double di = D[i], ri = rhov[i];
            for (int p = 0; p < k; ++p) {
                double xip = X[static_cast<int64_t>(p) * n + i];
                rhs[p] += xip * di * ri;
                double t = di * xip;
                for (int q = p; q < k; ++q) A[p + q * k] += t * X[static_cast<int64_t>(q) * n + i];
            }
        }
        for (int p = 0; p < k; ++p) for (int q = 0; q < p; ++q) A[p + q * k] = A[q + p * k];
        Ai = A;
        if (la::spd_inverse(Ai.data(), k) != 0) return false;
        la::matvec(Ai.data(), k, rhs.data(), dbe.data());
        for (int64_t i = 0; i < n; ++i) {
            double xdb = 0; for (int p = 0; p < k; ++p) xdb += X[static_cast<int64_t>(p) * n + i] * dbe[p];
            dadir[i] = D[i] * (rhov[i] - xdb);
        }
        return true;
    };

    for (int it = 0; it < 500; ++it) {
        for (int64_t i = 0; i < n; ++i) {
            double v = 0; for (int p = 0; p < k; ++p) v += X[static_cast<int64_t>(p) * n + i] * beta[p];
            Xb[i] = v;
        }
        double gap = 0; for (int64_t i = 0; i < n; ++i) gap += a[i] * z[i] + s[i] * w[i];
        double mu = gap / (2.0 * n);
        if (mu < 1e-10) { good = true; break; }
        for (int64_t i = 0; i < n; ++i) D[i] = 1.0 / (z[i] / a[i] + w[i] / s[i]);

        // ---- affine predictor: rho_aff = y - Xb ----
        for (int64_t i = 0; i < n; ++i) rho[i] = y[i] - Xb[i];
        if (!solve_dir(rho, daa, dbeta0)) break;
        // dz_aff = -z - (z/a)daa ; dw_aff = -w + (w/s)daa ; ds_aff = -daa
        double ap = 1, ad = 1;
        for (int64_t i = 0; i < n; ++i) {
            dza[i] = -z[i] - (z[i] / a[i]) * daa[i];
            dwa[i] = -w[i] + (w[i] / s[i]) * daa[i];
            if (daa[i] < 0) ap = std::min(ap, -a[i] / daa[i]);
            if (daa[i] > 0) ap = std::min(ap, s[i] / daa[i]);  // ds = -da
            if (dza[i] < 0) ad = std::min(ad, -z[i] / dza[i]);
            if (dwa[i] < 0) ad = std::min(ad, -w[i] / dwa[i]);
        }
        double mu_aff = 0;
        for (int64_t i = 0; i < n; ++i)
            mu_aff += (a[i] + ap * daa[i]) * (z[i] + ad * dza[i]) + (s[i] - ap * daa[i]) * (w[i] + ad * dwa[i]);
        mu_aff /= (2.0 * n);
        double sigma = (mu > 0) ? std::pow(mu_aff / mu, 3) : 0.0;

        // ---- corrector (uses the AFFINE directions for the 2nd-order terms) ----
        // rho = (y-Xb) + sigma*mu*(1/a - 1/s) - (daa*dza)/a - (daa*dwa)/s
        for (int64_t i = 0; i < n; ++i)
            rho[i] = (y[i] - Xb[i]) + sigma * mu * (1.0 / a[i] - 1.0 / s[i])
                     - (daa[i] * dza[i]) / a[i] - (daa[i] * dwa[i]) / s[i];
        if (!solve_dir(rho, da, dbeta)) break;
        for (int64_t i = 0; i < n; ++i) {
            double RHSz = sigma * mu - a[i] * z[i] - daa[i] * dza[i];
            double RHSw = sigma * mu - s[i] * w[i] + daa[i] * dwa[i];  // ds_aff = -daa
            dz[i] = (RHSz - z[i] * da[i]) / a[i];
            dw[i] = (RHSw + w[i] * da[i]) / s[i];
        }
        // step lengths
        double ap2 = 1, ad2 = 1;
        for (int64_t i = 0; i < n; ++i) {
            if (da[i] < 0) ap2 = std::min(ap2, -a[i] / da[i]);
            if (da[i] > 0) ap2 = std::min(ap2, s[i] / da[i]);
            if (dz[i] < 0) ad2 = std::min(ad2, -z[i] / dz[i]);
            if (dw[i] < 0) ad2 = std::min(ad2, -w[i] / dw[i]);
        }
        ap2 *= BIG; ad2 *= BIG;
        for (int64_t i = 0; i < n; ++i) { a[i] += ap2 * da[i]; s[i] -= ap2 * da[i]; z[i] += ad2 * dz[i]; w[i] += ad2 * dw[i]; }
        for (int p = 0; p < k; ++p) beta[p] += ad2 * dbeta[p];
    }
    if (ok) *ok = good;
    return beta;
}

QregResult qreg_fit(const double* X, int64_t n, int k, const double* y, double tau,
                    const std::vector<std::string>& names, double level) {
    QregResult r;
    r.tau = tau; r.n = n; r.k = k; r.names = names;
    bool ok = false;
    r.beta = qreg_solve(X, n, k, y, tau, &ok);
    r.converged = ok;

    // objective Σ ρ_τ(resid)
    double obj = 0;
    for (int64_t i = 0; i < n; ++i) {
        double e = y[i];
        for (int p = 0; p < k; ++p) e -= X[static_cast<int64_t>(p) * n + i] * r.beta[p];
        obj += (e >= 0) ? tau * e : (tau - 1) * e;
    }
    r.sum_adev = obj;
    r.dof_resid = static_cast<double>(n - k);

    double h = hall_sheather(n, tau, level);
    r.bwidth = h;
    double tl, tu;
    bracket_tau(tau, h, tl, tu);
    std::vector<double> bl = qreg_solve(X, n, k, y, tl);
    std::vector<double> bu = qreg_solve(X, n, k, y, tu);
    // fitted sparsity: predict at mean covariate row; s = (xbar'bu - xbar'bl)/(tu - tl)
    std::vector<double> xbar(k, 0.0);
    for (int p = 0; p < k; ++p) {
        const double* Xp = X + static_cast<int64_t>(p) * n;
        double t = 0; for (int64_t i = 0; i < n; ++i) t += Xp[i];
        xbar[p] = t / static_cast<double>(n);
    }
    double ql = 0, qu = 0;
    for (int p = 0; p < k; ++p) { ql += xbar[p] * bl[p]; qu += xbar[p] * bu[p]; }
    double sparsity = (qu - ql) / (tu - tl);
    if (sparsity < 1e-10) sparsity = 1e-10;
    r.sparsity = sparsity;

    // V_iid = sparsity^2 * tau(1-tau) * (X'X)^-1
    std::vector<double> XtX(static_cast<size_t>(k) * k, 0.0);
    la::xtx(X, n, k, XtX.data());
    if (la::spd_inverse(XtX.data(), k) == 0) {
        double f = sparsity * sparsity * tau * (1.0 - tau);
        r.se.resize(k);
        for (int p = 0; p < k; ++p) r.se[p] = std::sqrt(f * XtX[p + p * k]);
    } else {
        r.se.assign(k, std::nan(""));
    }
    return r;
}

// vce(robust), Stata's default `fitted` density method with the `hsheather`
// bandwidth: the Hendricks-Koenker sandwich ([R] qreg, option vce(robust):
// "see Koenker (2005, sec. 4.2)"; Koenker 2005 eq. 3.9 / sec. 4.2):
//
//   h     = Hall-Sheather bandwidth at N = the number of OBSERVATIONS (not Σw)
//   d_i   = x_i'(β(τ+h) - β(τ-h)) / (2h)       per-observation sparsity, from
//           the two neighbouring quantile fits of the SAME (weighted) problem
//   f_i   = 1/d_i, or 0 where d_i <= 0 (crossing quantiles)
//   H     = Σ w_i f_i x_i x_i'      J = Σ w_i^2 x_i x_i'      (w_i = 1 unweighted)
//   V     = τ(1-τ) H^-1 J H^-1      no small-sample factor
//
// Oracle: tests/eval/golden/aer_w2/w2b_estim.log:818 qreg_vce_robust _se[x1]
// .0805316862193467 and :791 qreg_pw_vce_robust (pweight w = 1+mod(_n,4))
// .0854069206829948; this formula reproduces both to 7e-11 relative on the
// do-file's float-stored data (tests/unit/test_estim_w2b.cpp). The same data
// rule out the alternatives: h from N = Σw (150) gives .0837183; meat Σ w x x'
// gives .0492929; an N/(N-1) factor gives .0812113. Only τ = .5 is captured,
// where the score-based meat Σ w^2 ψ_i^2 x x' (ψ = τ - 1{e<0}) coincides with
// τ(1-τ) J; the τ(1-τ) J form is Koenker's, and τ != .5 is not oracle-pinned.
QregResult qreg_fit_robust(const double* X, int64_t n, int k, const double* y, const double* w,
                           double tau, const std::vector<std::string>& names, double level) {
    QregResult r;
    r.tau = tau; r.n = n; r.k = k; r.names = names;
    bool ok = false;
    r.beta = qreg_solve_w(X, n, k, y, w, tau, &ok);
    r.converged = ok;

    double obj = 0;
    for (int64_t i = 0; i < n; ++i) {
        double e = y[i];
        for (int p = 0; p < k; ++p) e -= X[static_cast<int64_t>(p) * n + i] * r.beta[p];
        obj += (w ? w[i] : 1.0) * ((e >= 0) ? tau * e : (tau - 1) * e);
    }
    r.sum_adev = obj;
    r.dof_resid = static_cast<double>(n - k);
    r.sparsity = std::nan("");

    const double h = hall_sheather(n, tau, level);
    r.bwidth = h;
    double tl, tu;
    bracket_tau(tau, h, tl, tu);
    std::vector<double> bl = qreg_solve_w(X, n, k, y, w, tl, nullptr);
    std::vector<double> bu = qreg_solve_w(X, n, k, y, w, tu, nullptr);

    std::vector<double> H(static_cast<size_t>(k) * k, 0.0), J(static_cast<size_t>(k) * k, 0.0);
    for (int64_t i = 0; i < n; ++i) {
        double dq = 0;
        for (int p = 0; p < k; ++p) dq += X[static_cast<int64_t>(p) * n + i] * (bu[p] - bl[p]);
        const double dt = tu - tl;  // == 2h unless a bracket was clamped
        double f = 0;
        if (dq > 0) f = dt / dq;
        else ++r.n_crossing;
        const double wi = w ? w[i] : 1.0;
        for (int p = 0; p < k; ++p) {
            const double xp = X[static_cast<int64_t>(p) * n + i];
            for (int q = p; q < k; ++q) {
                const double xpq = xp * X[static_cast<int64_t>(q) * n + i];
                H[p + q * k] += wi * f * xpq;
                J[p + q * k] += wi * wi * xpq;
            }
        }
    }
    for (int p = 0; p < k; ++p)
        for (int q = 0; q < p; ++q) { H[p + q * k] = H[q + p * k]; J[p + q * k] = J[q + p * k]; }
    if (la::spd_inverse(H.data(), k) != 0) {
        r.se.assign(k, std::nan(""));
        return r;
    }
    r.V.assign(static_cast<size_t>(k) * k, 0.0);
    la::sandwich(H.data(), J.data(), k, tau * (1.0 - tau), r.V.data());
    r.se.resize(k);
    for (int p = 0; p < k; ++p) r.se[p] = std::sqrt(std::max(0.0, r.V[p + p * k]));
    return r;
}

}  // namespace cecon
