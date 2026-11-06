// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// cecon — Heckman sample-selection model, two-step (Heckit) estimator matching
// Stata `heckman depvar x, select(s = z) twostep`. Step 1: probit s on Z → γ̂, V_p.
// Step 2: inverse Mills m_j=φ(z_jγ̂)/Φ(z_jγ̂), δ_j=m_j(m_j+z_jγ̂); augmented OLS of y
// on [X, m] over selected obs; σ̂²=(e'e+β_m²Σδ)/N_sel, ρ̂=β_m/σ̂. Heckman (1979)
// sandwich VCE V = σ̂²(W'W)⁻¹[W'RW + Q](W'W)⁻¹ with R=diag(1−ρ̂²δ), Q=ρ̂²(W'DZ)V_p(Z'DW).
#pragma once
#include <cmath>
#include <vector>

#include "core/linalg.h"
#include "estimators/probit.h"

namespace cecon {

struct HeckmanResult {
    std::vector<double> beta, se;          // outcome eq incl. constant (k_x), then λ appended
    std::vector<std::string> out_names;
    std::vector<double> sel_beta, sel_se;  // selection eq (k_z)
    double lambda = 0, se_lambda = 0, sigma = 0, rho = 0;
    int64_t n = 0, n_selected = 0;
    bool converged = false, ok = false;
};

namespace heck {
inline double ncdf(double x) { return 0.5 * std::erfc(-x * 0.70710678118654752440); }
inline double npdf(double x) { return 0.39894228040143267794 * std::exp(-0.5 * x * x); }
}  // namespace heck

// Z (n*kz col-major) + S (n, 0/1) for the probit; X (n*kx) + y (n, NaN where not
// selected) for the outcome. sel[j]=true marks selected rows (S==1 & y finite).
inline HeckmanResult heckman_twostep(const double* Z, int64_t n, int kz, const double* S,
                                     const double* X, int kx, const double* y,
                                     const std::vector<char>& sel) {
    HeckmanResult r;
    r.n = n;
    // Step 1 is NOT the default probit. Stata's heckman fits the selection probit
    // with asis, a capped iteration count and no coefficient table, and takes bprb = e(b),
    // Vprb = e(V). For a non-robust vce the probit runs with ltol(0) tol(1e-4) plus
    // halfsteponly (MlMaxOptions, logit.h), measured on StataBE. Under it the posted b is
    // p0 + delta (never evaluated) and V = invsym(-H(p0)) with rule-4 rows dropped:
    // the iteration's own Hessian, which BinaryMlResult::bread carries. Z's last
    // column is the selection constant (cmd_heckman), the start's b0 slot.
    MlMaxOptions popt;
    popt.tol = 1e-4;
    popt.ltol = 0.0;
    popt.nrtol = 1e-5;
    popt.maxiter = 300;   // min(1000, c(maxiter)) at the default c(maxiter)
    popt.doopt = true;
    popt.step_forward = false;
    popt.stata_invsym = true;
    ProbitResult pr = probit_fit_ml(Z, n, kz, S, popt, nullptr, last_column_is_constant(Z, n, kz));
    r.sel_beta = pr.beta;
    std::vector<double> Vp = pr.bread_oim;
    if (pr.separated || Vp.size() != static_cast<size_t>(kz) * kz) Vp = pr.V;  // H not invertible
    r.sel_se.resize(kz);
    for (int c = 0; c < kz; ++c) r.sel_se[c] = std::sqrt(std::max(0.0, Vp[static_cast<size_t>(c) * kz + c]));

    std::vector<int64_t> S1;
    for (int64_t i = 0; i < n; ++i) if (sel[i]) S1.push_back(i);
    int64_t ns = static_cast<int64_t>(S1.size());
    r.n_selected = ns;
    int kw = kx + 1;  // [X, mills]
    if (ns <= kw) return r;

    std::vector<double> m(ns), delta(ns);
    for (int64_t j = 0; j < ns; ++j) {
        int64_t i = S1[j];
        double zg = 0;
        for (int c = 0; c < kz; ++c) zg += Z[static_cast<int64_t>(c) * n + i] * pr.beta[c];
        double Phi = heck::ncdf(zg);
        if (Phi < 1e-300) Phi = 1e-300;
        m[j] = heck::npdf(zg) / Phi;
        delta[j] = m[j] * (m[j] + zg);
    }
    // augmented design W = [X_sel, m]
    std::vector<double> W(static_cast<size_t>(ns) * kw), ysel(ns);
    for (int64_t j = 0; j < ns; ++j) {
        int64_t i = S1[j];
        for (int c = 0; c < kx; ++c) W[static_cast<int64_t>(c) * ns + j] = X[static_cast<int64_t>(c) * n + i];
        W[static_cast<int64_t>(kx) * ns + j] = m[j];
        ysel[j] = y[i];
    }
    std::vector<double> WtW(static_cast<size_t>(kw) * kw), Wty(kw), b(kw);
    la::xtx(W.data(), ns, kw, WtW.data());
    la::xtv(W.data(), ns, kw, ysel.data(), Wty.data());
    if (la::spd_inverse(WtW.data(), kw) != 0) return r;
    std::vector<double> WtWinv = WtW;
    la::matvec(WtWinv.data(), kw, Wty.data(), b.data());
    double beta_m = b[kx];
    // residuals + sigma, rho
    double ess = 0, sumdelta = 0;
    for (int64_t j = 0; j < ns; ++j) {
        double f = 0;
        for (int c = 0; c < kw; ++c) f += W[static_cast<int64_t>(c) * ns + j] * b[c];
        double e = ysel[j] - f;
        ess += e * e;
        sumdelta += delta[j];
    }
    double sigma2 = (ess + beta_m * beta_m * sumdelta) / static_cast<double>(ns);
    double sigma = std::sqrt(sigma2);
    double rho = beta_m / sigma;
    // Stata's default `rhosigma` rule keeps the two-step VCE admissible when
    // the unconstrained rho estimate leaves [-1, 1]: truncate rho and make
    // sigma consistent with lambda = rho*sigma before assembling the VCE.
    // (The augmented-regression coefficients, including lambda, do not change.)
    if (std::fabs(rho) > 1.0) {
        rho = std::copysign(1.0, rho);
        sigma = beta_m / rho;
        sigma2 = sigma * sigma;
    }
    r.sigma = sigma; r.rho = rho; r.lambda = beta_m;

    // sandwich: V = sigma2 * WtWinv * [W'RW + Q] * WtWinv
    std::vector<double> WRW(static_cast<size_t>(kw) * kw, 0.0);
    for (int64_t j = 0; j < ns; ++j) {
        double rj = 1.0 - rho * rho * delta[j];
        for (int c = 0; c < kw; ++c) {
            double wc = W[static_cast<int64_t>(c) * ns + j];
            for (int d = 0; d < kw; ++d)
                WRW[static_cast<size_t>(c) * kw + d] += rj * wc * W[static_cast<int64_t>(d) * ns + j];
        }
    }
    // WDZ = sum delta_j w_j z_j'  (kw x kz)
    std::vector<double> WDZ(static_cast<size_t>(kw) * kz, 0.0);
    for (int64_t j = 0; j < ns; ++j) {
        int64_t i = S1[j];
        for (int c = 0; c < kw; ++c) {
            double wc = delta[j] * W[static_cast<int64_t>(c) * ns + j];
            for (int e = 0; e < kz; ++e) WDZ[static_cast<size_t>(c) * kz + e] += wc * Z[static_cast<int64_t>(e) * n + i];
        }
    }
    // Q = rho^2 * WDZ * V_p * WDZ'   (kw x kw)
    std::vector<double> tmp(static_cast<size_t>(kw) * kz, 0.0);  // WDZ * V_p
    for (int c = 0; c < kw; ++c)
        for (int e = 0; e < kz; ++e) {
            double s = 0;
            for (int f = 0; f < kz; ++f) s += WDZ[static_cast<size_t>(c) * kz + f] * Vp[static_cast<size_t>(f) * kz + e];
            tmp[static_cast<size_t>(c) * kz + e] = s;
        }
    std::vector<double> mid = WRW;  // W'RW + Q
    for (int c = 0; c < kw; ++c)
        for (int d = 0; d < kw; ++d) {
            double q = 0;
            for (int e = 0; e < kz; ++e) q += tmp[static_cast<size_t>(c) * kz + e] * WDZ[static_cast<size_t>(d) * kz + e];
            mid[static_cast<size_t>(c) * kw + d] += rho * rho * q;
        }
    // V = sigma2 * WtWinv * mid * WtWinv
    std::vector<double> t2(static_cast<size_t>(kw) * kw, 0.0);  // WtWinv * mid
    for (int c = 0; c < kw; ++c)
        for (int d = 0; d < kw; ++d) {
            double s = 0;
            for (int e = 0; e < kw; ++e) s += WtWinv[static_cast<size_t>(c) * kw + e] * mid[static_cast<size_t>(e) * kw + d];
            t2[static_cast<size_t>(c) * kw + d] = s;
        }
    std::vector<double> V(static_cast<size_t>(kw) * kw, 0.0);
    for (int c = 0; c < kw; ++c)
        for (int d = 0; d < kw; ++d) {
            double s = 0;
            for (int e = 0; e < kw; ++e) s += t2[static_cast<size_t>(c) * kw + e] * WtWinv[static_cast<size_t>(e) * kw + d];
            V[static_cast<size_t>(c) * kw + d] = sigma2 * s;
        }
    r.beta.assign(b.begin(), b.end());  // includes lambda as last
    r.se.resize(kw);
    for (int c = 0; c < kw; ++c) r.se[c] = std::sqrt(std::max(0.0, V[static_cast<size_t>(c) * kw + c]));
    r.se_lambda = r.se[kx];
    r.converged = pr.converged;
    r.ok = true;
    return r;
}

}  // namespace cecon
