// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "mixedlm.h"

// Random-intercept multilevel ML fit. The log-likelihood, gradient and per-group
// score machinery are inline in mixedlm.h (header-only, like tobit.h/oglm.h); what
// lives here is the driver — starting values, the EM warm start, and the
// Newton-Raphson loop that finishes the job — because it is a single non-inline
// entry point with no reason to be re-instantiated in every translation unit.

namespace cecon {

namespace {

// Per-group and whole-sample sums that do NOT depend on β or the variance
// parameters, so the EM loop computes them once instead of once per sweep.
struct Pre {
    int G = 0, k = 0;
    std::vector<double> nj;    // G
    std::vector<double> Sx;    // G*k col-major: Σ_i x_p within group
    std::vector<double> Sy;    // G      : Σ_i y within group
    std::vector<double> XtX;   // k*k
    std::vector<double> Xty;   // k
};

Pre precompute(const double* X, int64_t n, int k, const double* y, const int32_t* gcode, int G) {
    Pre P;
    P.G = G;
    P.k = k;
    P.nj.assign(static_cast<size_t>(G), 0.0);
    P.Sx.assign(static_cast<size_t>(G) * k, 0.0);
    P.Sy.assign(static_cast<size_t>(G), 0.0);
    P.XtX.assign(static_cast<size_t>(k) * k, 0.0);
    P.Xty.assign(static_cast<size_t>(k), 0.0);
    la::xtx(X, n, k, P.XtX.data());
    la::xtv(X, n, k, y, P.Xty.data());
    for (int64_t i = 0; i < n; ++i) {
        const int j = gcode[i];
        P.nj[static_cast<size_t>(j)] += 1.0;
        P.Sy[static_cast<size_t>(j)] += y[i];
        for (int p = 0; p < k; ++p)
            P.Sx[static_cast<size_t>(p) * G + j] += X[static_cast<int64_t>(p) * n + i];
    }
    return P;
}

// GLS β at the current variance ratio: β = (Σ_j X_j'V_j⁻¹X_j)⁻¹ Σ_j X_j'V_j⁻¹y_j.
// The common 1/σ_e² cancels between the two sides, so only c_j = σ_u²/d_j enters and
// the rank-one correction is a pure per-group outer product of the group's column
// sums — no V_j is formed. Returns false if the corrected cross-product is singular.
bool gls_beta(const Pre& P, double su2, double se2, std::vector<double>& beta) {
    const int k = P.k, G = P.G;
    std::vector<double> A = P.XtX, b = P.Xty;
    for (int j = 0; j < G; ++j) {
        const double m = P.nj[static_cast<size_t>(j)];
        if (m <= 0.0) continue;
        const double c = su2 / (se2 + m * su2);
        if (c == 0.0) continue;
        for (int p = 0; p < k; ++p) {
            const double sxp = P.Sx[static_cast<size_t>(p) * G + j];
            for (int q = 0; q < k; ++q)
                A[static_cast<size_t>(p) * k + q] -= c * sxp * P.Sx[static_cast<size_t>(q) * G + j];
            b[static_cast<size_t>(p)] -= c * sxp * P.Sy[static_cast<size_t>(j)];
        }
    }
    if (la::spd_inverse(A.data(), k) != 0) return false;
    beta.assign(static_cast<size_t>(k), 0.0);
    la::matvec(A.data(), k, b.data(), beta.data());
    return true;
}

// Residual sums Σe and Σe² per group at the given β.
void group_resid_sums(const double* X, int64_t n, int k, const double* y, const int32_t* gcode,
                      int G, const std::vector<double>& beta, std::vector<double>& S1,
                      std::vector<double>& S2) {
    S1.assign(static_cast<size_t>(G), 0.0);
    S2.assign(static_cast<size_t>(G), 0.0);
    for (int64_t i = 0; i < n; ++i) {
        double xb = 0;
        for (int p = 0; p < k; ++p)
            xb += X[static_cast<int64_t>(p) * n + i] * beta[static_cast<size_t>(p)];
        const double e = y[i] - xb;
        S1[static_cast<size_t>(gcode[i])] += e;
        S2[static_cast<size_t>(gcode[i])] += e * e;
    }
}

}  // namespace

MixedLmResult mixedlm_fit(const double* X, int64_t n, int k, const double* y,
                          const int32_t* gcode, int G) {
    MixedLmResult r;
    r.n = n;
    r.n_groups = G;
    if (n <= 0 || k <= 0 || G <= 0) return r;

    const Pre P = precompute(X, n, k, y, gcode, G);
    {   // observations per group, as Stata's "Obs per group: min/avg/max"
        int64_t mn = 0, mx = 0;
        bool first = true;
        for (int j = 0; j < G; ++j) {
            const int64_t m = static_cast<int64_t>(P.nj[static_cast<size_t>(j)]);
            if (first) { mn = mx = m; first = false; }
            mn = m < mn ? m : mn;
            mx = m > mx ? m : mx;
        }
        r.g_min = mn;
        r.g_max = mx;
        r.g_avg = static_cast<double>(n) / static_cast<double>(G);
    }

    // --- starting values -----------------------------------------------------
    // β from OLS; then a moments decomposition of the OLS residuals: the WITHIN
    // group sum of squares over (N−G) seeds σ_e², and the between-group variance of
    // the group residual means minus σ_e²/n̄ seeds σ_u² (the sampling variance of a
    // group mean under the null σ_u²=0 is exactly σ_e²/n_j, so subtracting it is what
    // isolates the true between component). Floored at a small positive multiple of
    // var(y) so ln σ_u is finite when the moment estimate lands at or below zero.
    std::vector<double> beta;
    {
        std::vector<double> A = P.XtX;
        if (la::spd_inverse(A.data(), k) != 0) return r;
        beta.assign(static_cast<size_t>(k), 0.0);
        la::matvec(A.data(), k, P.Xty.data(), beta.data());
    }
    double ybar = 0;
    for (int64_t i = 0; i < n; ++i) ybar += y[i];
    ybar /= static_cast<double>(n);
    double vary = 0;
    for (int64_t i = 0; i < n; ++i) { const double d = y[i] - ybar; vary += d * d; }
    vary = (n > 1) ? vary / static_cast<double>(n - 1) : 1.0;
    if (!(vary > 0)) vary = 1.0;

    double su2 = 0, se2 = 0;
    {
        std::vector<double> S1, S2;
        group_resid_sums(X, n, k, y, gcode, G, beta, S1, S2);
        double within = 0, sum_mean = 0, ngrp = 0;
        std::vector<double> gmean(static_cast<size_t>(G), 0.0);
        for (int j = 0; j < G; ++j) {
            const double m = P.nj[static_cast<size_t>(j)];
            if (m <= 0) continue;
            within += S2[static_cast<size_t>(j)] -
                      S1[static_cast<size_t>(j)] * S1[static_cast<size_t>(j)] / m;
            gmean[static_cast<size_t>(j)] = S1[static_cast<size_t>(j)] / m;
            sum_mean += gmean[static_cast<size_t>(j)];
            ngrp += 1.0;
        }
        se2 = (n > G) ? within / static_cast<double>(n - G) : vary;
        if (!(se2 > 0)) se2 = vary;
        double between = 0;
        if (ngrp > 1) {
            const double mbar = sum_mean / ngrp;
            for (int j = 0; j < G; ++j) {
                if (P.nj[static_cast<size_t>(j)] <= 0) continue;
                const double d = gmean[static_cast<size_t>(j)] - mbar;
                between += d * d;
            }
            between /= (ngrp - 1.0);
        }
        const double nbar = static_cast<double>(n) / static_cast<double>(G);
        su2 = between - se2 / nbar;
        const double floor_u = 1e-3 * vary;
        if (!(su2 > floor_u)) su2 = floor_u;
    }

    // --- EM warm start -------------------------------------------------------
    // Closed-form random-intercept EM, alternating a GLS β update with the E/M
    // variance updates. E-step: û_j = σ_u² Σe / d_j (posterior mean of the random
    // intercept) with posterior variance v_j = σ_u²σ_e²/d_j. M-step: σ_u² is the
    // mean of û_j² + v_j over groups, σ_e² the mean over observations of the
    // within-group residual-about-û sum plus n_j v_j. Monotone in the likelihood and
    // costs one O(n·k) residual pass per sweep, so it is a cheap way to hand Newton a
    // θ that is already close — the same EM-then-gradient sequence Stata runs.
    {
        std::vector<double> S1, S2;
        for (int it = 0; it < 200; ++it) {
            if (!gls_beta(P, su2, se2, beta)) break;
            group_resid_sums(X, n, k, y, gcode, G, beta, S1, S2);
            double acc_u = 0, acc_e = 0, ngrp = 0;
            for (int j = 0; j < G; ++j) {
                const double m = P.nj[static_cast<size_t>(j)];
                if (m <= 0) continue;
                const double d = se2 + m * su2;
                const double uh = su2 * S1[static_cast<size_t>(j)] / d;
                const double v = su2 * se2 / d;
                acc_u += uh * uh + v;
                acc_e += S2[static_cast<size_t>(j)] - 2.0 * uh * S1[static_cast<size_t>(j)] +
                         m * uh * uh + m * v;
                ngrp += 1.0;
            }
            if (ngrp <= 0) break;
            const double nsu2 = acc_u / ngrp;
            const double nse2 = acc_e / static_cast<double>(n);
            if (!(nsu2 > 0) || !(nse2 > 0)) break;
            const double rel = std::fabs(nsu2 - su2) / (su2 + 1e-300) +
                               std::fabs(nse2 - se2) / (se2 + 1e-300);
            su2 = nsu2;
            se2 = nse2;
            r.em_iters = it + 1;
            if (rel < 1e-12) break;
        }
    }

    // --- Newton-Raphson ------------------------------------------------------
    const int np = k + 2;
    std::vector<double> theta(static_cast<size_t>(np), 0.0);
    for (int p = 0; p < k; ++p) theta[static_cast<size_t>(p)] = beta[static_cast<size_t>(p)];
    theta[static_cast<size_t>(k)] = 0.5 * std::log(su2);
    theta[static_cast<size_t>(k) + 1] = 0.5 * std::log(se2);

    std::vector<double> grad(static_cast<size_t>(np)), H, negH, step(static_cast<size_t>(np));
    // Backtracking candidate, hoisted next to `step` for the same reason the others
    // are: every entry is rewritten from theta+t*step before ll_grad reads it.
    std::vector<double> cand(static_cast<size_t>(np));
    double prevLL = -1e300;
    int it = 0;
    for (; it < 200; ++it) {
        const double LL = mxd::ll_grad(X, n, k, y, gcode, G, theta, &grad);
        // Convergence on the GRADIENT, not on ΔLL. Near a maximum the likelihood is
        // flat to second order, so a ΔLL rule accepts a θ whose coefficients are
        // still ~1e-5 off — inside the LL tolerance and outside the 1e-6 coefficient
        // tolerance this wave pins. The gradient is the sharp instrument. The gate is
        // scaled by |LL| because the gradient's own summation noise floor is, and set
        // two decades above that floor (≈|LL|·ε·√N ≈ 1e-10 on the P7 fixture).
        double gmax = 0;
        for (int p = 0; p < np; ++p) gmax = std::max(gmax, std::fabs(grad[static_cast<size_t>(p)]));
        if (gmax <= 1e-10 * (1.0 + std::fabs(LL))) { r.converged = true; break; }
        mxd::hessian(X, n, k, y, gcode, G, theta, H);
        negH.assign(H.size(), 0.0);
        for (size_t i = 0; i < H.size(); ++i) negH[i] = -H[i];
        if (la::spd_inverse(negH.data(), np) != 0) break;
        la::matvec(negH.data(), np, grad.data(), step.data());  // step = (−H)⁻¹ grad
        double t = 1.0;
        double candLL = LL;
        for (int bt = 0; bt < 40; ++bt) {
            for (int i = 0; i < np; ++i)
                cand[static_cast<size_t>(i)] =
                    theta[static_cast<size_t>(i)] + t * step[static_cast<size_t>(i)];
            candLL = mxd::ll_grad(X, n, k, y, gcode, G, cand, nullptr);
            if (std::isfinite(candLL) && candLL >= LL) break;
            t *= 0.5;
        }
        theta = cand;
        // Stall guard: the line search could not improve the likelihood at all, so
        // another sweep would repeat this one. Not reported as converged — the
        // gradient test above is the only thing that sets that flag.
        if (candLL <= prevLL && it > 0) { prevLL = candLL; ++it; break; }
        prevLL = candLL;
    }
    r.iters = it;

    r.ll = mxd::ll_grad(X, n, k, y, gcode, G, theta, &grad);
    mxd::hessian(X, n, k, y, gcode, G, theta, H);

    // --- VCE: BLOCK-DIAGONAL, and the two blocks are inverted DIFFERENTLY -----
    // This is measured, not assumed, and it is NOT the plain (−H)⁻¹ the other
    // house MLEs use. Stata 19.5's own e(V) after `mixed y x1 x2 || id:` on the P7
    // fixture reports the β/lnσ cross block as EXACTLY zero, and its two diagonal
    // blocks pin the construction:
    //
    //     e(V)[y:x1, y:x1]              = .00044169596363  → se .021016564
    //     e(V)[lns1_1_1, lns1_1_1]      = .00544844550056  → se .073813586
    //     e(V)[lnsig_e,  lnsig_e]       = .000113442847389 → se .010650955
    //     e(V)[β, lnσ·]                 = 0                (all six entries)
    //
    // The β block is (I_ββ)⁻¹ — the MARGINAL fixed-effect information inverse, i.e.
    // the GLS variance (Σ_j X_j'V_j⁻¹X_j)⁻¹ treating the variance components as
    // known. It is NOT the β block of the full 5×5 inverse, which on this fixture is
    // .021151888 — visibly not what Stata prints.
    //
    // The variance-component block is the opposite choice: the φφ block of the FULL
    // inverse, which is (I_φφ − I_φβ I_ββ⁻¹ I_βφ)⁻¹ — the observed information of the
    // PROFILED log-likelihood LP(φ) = LL(β̂(φ), φ). It is NOT (I_φφ)⁻¹, which here
    // gives .066998 against Stata's .073814.
    //
    // Both choices carry straight over to the vce(robust) bread: with this bread and
    // meat Σ_j g_j g_j' × G/(G−1), all five robust SEs land on the oracle to <1e-5
    // relative (tests/unit/test_mixedlm.cpp). The cross block being zero is what
    // makes the two blocks separable at all — V_ββ = B_ββ M_ββ B_ββ does not involve
    // B_φφ, and vice versa.
    std::vector<double> I(H.size());
    for (size_t i = 0; i < I.size(); ++i) I[i] = -H[i];
    std::vector<double> Vfull = I;
    if (la::spd_inverse(Vfull.data(), np) != 0) return r;
    std::vector<double> Ibb(static_cast<size_t>(k) * k);
    for (int a = 0; a < k; ++a)
        for (int b = 0; b < k; ++b)
            Ibb[static_cast<size_t>(a) * k + b] = I[static_cast<size_t>(a) * np + b];
    if (la::spd_inverse(Ibb.data(), k) != 0) return r;
    std::vector<double> V(static_cast<size_t>(np) * np, 0.0);
    for (int a = 0; a < k; ++a)
        for (int b = 0; b < k; ++b)
            V[static_cast<size_t>(a) * np + b] = Ibb[static_cast<size_t>(a) * k + b];
    for (int a = 0; a < 2; ++a)
        for (int b = 0; b < 2; ++b)
            V[static_cast<size_t>(k + a) * np + (k + b)] =
                Vfull[static_cast<size_t>(k + a) * np + (k + b)];
    r.vcov = V;
    r.theta = theta;
    r.beta.assign(theta.begin(), theta.begin() + k);
    r.se.resize(static_cast<size_t>(k));
    for (int p = 0; p < k; ++p)
        r.se[static_cast<size_t>(p)] =
            std::sqrt(std::max(0.0, V[static_cast<size_t>(p) * np + p]));
    r.var_u = std::exp(2.0 * theta[static_cast<size_t>(k)]);
    r.var_e = std::exp(2.0 * theta[static_cast<size_t>(k) + 1]);
    r.se_ln_su = std::sqrt(std::max(0.0, V[static_cast<size_t>(k) * np + k]));
    r.se_ln_se = std::sqrt(std::max(0.0, V[static_cast<size_t>(k + 1) * np + (k + 1)]));
    r.ok = true;
    return r;
}

std::vector<double> mixedlm_group_scores(const double* X, int64_t n, int k, const double* y,
                                         const int32_t* gcode, int G, const MixedLmResult& r) {
    std::vector<double> S;
    if (r.theta.size() != static_cast<size_t>(k) + 2) return S;
    mxd::ll_grad(X, n, k, y, gcode, G, r.theta, nullptr, &S);
    return S;
}

}  // namespace cecon
