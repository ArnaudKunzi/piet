// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// cecon — random-intercept multilevel (mixed-effects) regression by MAXIMUM
// LIKELIHOOD, matching Stata `mixed y x1 x2 || GROUP:,` (Stata's `mixed` default
// is ML; REML is a different objective and moves var(_cons) in the third digit,
// so it is NOT what this file computes — see the honest reject in cmds_mixedlm.cpp).
//
// MODEL.  y_ij = x_ij'β + u_j + e_ij, u_j ~ N(0, σ_u²) independent across groups,
// e_ij ~ N(0, σ_e²) independent of everything. Stacked within group j,
//
//     V_j = σ_e² I_{n_j} + σ_u² 1 1'                                   (n_j × n_j)
//
// which is never materialized. Everything below is the rank-one (Woodbury) closed
// form, with d_j ≡ σ_e² + n_j σ_u² and c_j ≡ σ_u²/d_j:
//
//     V_j⁻¹ a       = (a − c_j (1'a) 1) / σ_e²          for any n_j-vector a
//     log|V_j|      = (n_j − 1) log σ_e² + log d_j
//     e_j'V_j⁻¹e_j  = (Σe² − c_j (Σe)²) / σ_e²
//     1'V_j⁻¹e_j    = Σe / d_j
//     tr(V_j⁻¹)     = n_j(1 − c_j)/σ_e²        tr(V_j⁻¹11') = n_j/d_j
//
// so the whole log-likelihood and its gradient are functions of FIVE per-group
// scalars — n_j, Σe, Σe², and (per design column p) Σx_p and Σx_p·e — computed in a
// single O(n·k) pass. Cost is linear in n regardless of group size.
//
//     LL = −½ [ N log 2π + Σ_j log|V_j| + Σ_j e_j'V_j⁻¹e_j ]
//
// PARAMETRIZATION. θ = (β[0..k−1], ln σ_u, ln σ_e), np = k+2. The log metric keeps
// both variances positive without a constraint and is the metric the SEs are formed
// in; the σ² metric Stata prints is reached by the delta method at the very end
// (see mixed_var_se / mixed_var_ci below, which are the SINGLE source of that rule
// for both the command layer and the unit test).
//
// Gradient wrt the variance parameters, from ∂LL/∂φ = −½[tr(V⁻¹ ∂V/∂φ) − e'V⁻¹(∂V/∂φ)V⁻¹e]
// with ∂V_j/∂ln σ_u = 2σ_u² 11' and ∂V_j/∂ln σ_e = 2σ_e² I:
//
//     ∂LL/∂ln σ_u = −σ_u² Σ_j [ n_j/d_j − (Σe)²/d_j² ]
//     ∂LL/∂ln σ_e = −Σ_j [ n_j(1 − c_j) − (Σe² − 2c_j(Σe)² + c_j² n_j (Σe)²)/σ_e² ]
//
// Both are verified against a central-difference numerical gradient in
// tests/unit/test_mixedlm.cpp before any oracle digit is chased, and the Woodbury
// LL itself is cross-checked there against a brute-force dense V_j build.
//
// SCORES ARE PER GROUP, not per observation. The log-likelihood is a sum over
// INDEPENDENT groups (observations within a group are correlated through u_j and do
// not factor), so the group is the unit whose score rows are independent — which is
// exactly what makes Σ_j g_j g_j' the right robust meat. Stata agrees: under
// `vce(robust)` with `|| id:` it prints "(Std. err. adjusted for 200 clusters in id)".
//
// OPTIMIZER. A closed-form EM warm start (GLS β + the standard random-intercept
// variance updates — cheap, and it mirrors Stata's own "Performing EM optimization"
// then "Performing gradient-based optimization" sequence), then the house new-MLE
// recipe from estimators/tobit.h: analytic gradient, central-difference numerical
// Hessian of that gradient, symmetrize, 40-halving backtracking line search.
//
// The VCE is where this estimator DEPARTS from the house recipe. tobit/oglm report a
// plain (−H)⁻¹; Stata's `mixed` does not, and measurement (its own e(V), reproduced
// in tests/unit/test_mixedlm.cpp) says so unambiguously — the matrix is
// block-diagonal, with the β block inverted MARGINALLY and the variance block taken
// from the PROFILED (full) inverse. The construction and the numbers that force it
// are documented at the VCE step in mixedlm.cpp.
#pragma once
#include "core/threads.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "core/linalg.h"

namespace cecon {

struct MixedLmResult {
    std::vector<double> beta;   // k fixed effects, X's column order (intercept LAST)
    std::vector<double> se;     // k model-based SEs
    // np*np, np = k+2, in the (β, ln σ_u, ln σ_e) metric — Stata's e(V) for `mixed`
    // WITHOUT a vce() option, and the bread of the vce(robust) sandwich. NOT a plain
    // (−H)⁻¹: it is BLOCK-DIAGONAL with the β block inverted marginally and the
    // variance block taken from the full (profiled) inverse. See the long comment at
    // the bottom of mixedlm.cpp for the Stata e(V) measurement that establishes it.
    std::vector<double> vcov;
    std::vector<double> theta;  // the converged θ itself — what the score/sandwich
                                // helpers re-enter with, so no exp/log round-trip
                                // can move the meat off the exact optimum
    double var_u = 0;           // σ_u² — Stata's var(_cons) for the group's Identity block
    double var_e = 0;           // σ_e² — Stata's var(Residual)
    double se_ln_su = 0;        // SE(ln σ_u) from `vcov` (model-based)
    double se_ln_se = 0;        // SE(ln σ_e) from `vcov` (model-based)
    double ll = 0;
    int64_t n = 0;
    int n_groups = 0;
    int64_t g_min = 0, g_max = 0;  // observations per group
    double g_avg = 0;
    int em_iters = 0, iters = 0;
    bool converged = false;
    bool ok = false;
};

namespace mxd {

// Log-likelihood, and optionally the analytic gradient and/or the PER-GROUP score
// matrix, at θ = (β[0..k−1], ln σ_u, ln σ_e).
//
// X is col-major n*k (intercept included — unlike the ordered models, the mixed
// model's constant IS identified). `gcode` holds a dense 0-based group code per row;
// rows of one group need NOT be contiguous.
//
// gscores: optional G*np COLUMN-major output (column p, group j at gscores[p*G+j] —
// the same layout convention as X), filled with the SAME per-group contribution that
// would otherwise only be summed into *grad, so la::xtx(gscores, G, np, …) gives the
// robust meat Σ_j g_j g_j' directly.
//
// Unlike tobit.h/oglm.h — whose grad and scores branches are deliberately duplicated
// because `acc += c * X[…]` is one FMA-fusable expression whose last bits move if the
// product is hoisted into a shared temp — every contribution here is already a
// division-terminated temp (`(T − c·Sx·Σe)/σ_e²`) computed once per GROUP, not per
// observation. Sharing that temp between the two sinks is bit-for-bit the same value
// the duplicated form would produce, so there is nothing to duplicate.
inline double ll_grad(const double* X, int64_t n, int k, const double* y,
                      const int32_t* gcode, int G, const std::vector<double>& theta,
                      std::vector<double>* grad, std::vector<double>* gscores = nullptr) {
    const int np = k + 2;
    const double lse = theta[static_cast<size_t>(k) + 1];
    const double su2 = std::exp(2.0 * theta[static_cast<size_t>(k)]);
    const double se2 = std::exp(2.0 * lse);
    const double log2pi = 1.8378770664093454836;
    const bool want = (grad != nullptr) || (gscores != nullptr);

    // Per-group sufficient statistics at this β.
    std::vector<double> nj(static_cast<size_t>(G), 0.0), S1(static_cast<size_t>(G), 0.0),
        S2(static_cast<size_t>(G), 0.0);
    std::vector<double> Sx, T;  // G*k col-major: Σ_i x_p and Σ_i x_p·e within group
    if (want) {
        Sx.assign(static_cast<size_t>(G) * k, 0.0);
        T.assign(static_cast<size_t>(G) * k, 0.0);
    }
    for (int64_t i = 0; i < n; ++i) {
        const int j = gcode[i];
        double xb = 0;
        for (int p = 0; p < k; ++p) xb += X[static_cast<int64_t>(p) * n + i] * theta[static_cast<size_t>(p)];
        const double e = y[i] - xb;
        nj[static_cast<size_t>(j)] += 1.0;
        S1[static_cast<size_t>(j)] += e;
        S2[static_cast<size_t>(j)] += e * e;
        if (want)
            for (int p = 0; p < k; ++p) {
                const double xv = X[static_cast<int64_t>(p) * n + i];
                Sx[static_cast<size_t>(p) * G + j] += xv;
                T[static_cast<size_t>(p) * G + j] += xv * e;
            }
    }

    if (grad) std::fill(grad->begin(), grad->end(), 0.0);
    if (gscores) gscores->assign(static_cast<size_t>(G) * np, 0.0);
    double* S = gscores ? gscores->data() : nullptr;

    double LL = -0.5 * static_cast<double>(n) * log2pi;
    const double log_se2 = 2.0 * lse;  // log σ_e² exactly — no log(exp(·)) round-trip
    for (int j = 0; j < G; ++j) {
        const double m = nj[static_cast<size_t>(j)];
        if (m <= 0.0) continue;  // defensive: a dense Factor never yields an empty level
        const double d = se2 + m * su2;
        const double c = su2 / d;
        const double s1 = S1[static_cast<size_t>(j)], s2 = S2[static_cast<size_t>(j)];
        LL += -0.5 * ((m - 1.0) * log_se2 + std::log(d) + (s2 - c * s1 * s1) / se2);
        if (!want) continue;
        for (int p = 0; p < k; ++p) {
            // (X_j' V_j⁻¹ e_j)_p = (Σ x_p e − c_j (Σ x_p)(Σ e)) / σ_e²
            const double gb = (T[static_cast<size_t>(p) * G + j] -
                               c * Sx[static_cast<size_t>(p) * G + j] * s1) / se2;
            if (grad) (*grad)[static_cast<size_t>(p)] += gb;
            if (S) S[static_cast<int64_t>(p) * G + j] = gb;
        }
        const double gu = -su2 * (m / d - (s1 * s1) / (d * d));
        const double ge = -(m * (1.0 - c) - (s2 - 2.0 * c * s1 * s1 + c * c * m * s1 * s1) / se2);
        if (grad) {
            (*grad)[static_cast<size_t>(k)] += gu;
            (*grad)[static_cast<size_t>(k) + 1] += ge;
        }
        if (S) {
            S[static_cast<int64_t>(k) * G + j] = gu;
            S[static_cast<int64_t>(k + 1) * G + j] = ge;
        }
    }
    return LL;
}

// Central-difference numerical Hessian of ll_grad's analytic gradient, symmetrized.
// Same recipe (and same 1e-6·(|θ_j|+1) step) as tobit.h / oglm.h.
inline void hessian(const double* X, int64_t n, int k, const double* y, const int32_t* gcode,
                    int G, const std::vector<double>& theta, std::vector<double>& H) {
    const int np = static_cast<int>(theta.size());
    H.assign(static_cast<size_t>(np) * np, 0.0);
    // Column j reads only theta and j and writes only H[:, j] -- no accumulator is
    // shared between columns -- so evaluating them concurrently returns the identical
    // matrix, with no summation order to reorder. These 2*np full-data gradient passes
    // are what a Newton step costs here. (PERFSWEEP H: same class as ivml/zicount.)
    parallel_for(np, [&](int64_t jj) {
        const int j = static_cast<int>(jj);
        thread_local std::vector<double> gp, gm, tp, tm;
        gp.resize(static_cast<size_t>(np));
        gm.resize(static_cast<size_t>(np));
        const double h = 1e-6 * (std::fabs(theta[static_cast<size_t>(j)]) + 1.0);
        tp = theta;
        tm = theta;
        tp[static_cast<size_t>(j)] += h;
        tm[static_cast<size_t>(j)] -= h;
        ll_grad(X, n, k, y, gcode, G, tp, &gp);
        ll_grad(X, n, k, y, gcode, G, tm, &gm);
        for (int i = 0; i < np; ++i)
            H[static_cast<size_t>(i) * np + j] =
                (gp[static_cast<size_t>(i)] - gm[static_cast<size_t>(i)]) / (2 * h);
    }, threads_for_work(static_cast<int64_t>(np) * n));
    for (int i = 0; i < np; ++i)
        for (int j = i + 1; j < np; ++j) {
            const double s =
                0.5 * (H[static_cast<size_t>(i) * np + j] + H[static_cast<size_t>(j) * np + i]);
            H[static_cast<size_t>(i) * np + j] = H[static_cast<size_t>(j) * np + i] = s;
        }
}

}  // namespace mxd

// --- variance-component reporting (Stata's σ² metric) ------------------------
// ONE definition of both rules, shared by cmds_mixedlm.cpp and test_mixedlm.cpp —
// two spellings is how a reported CI and the CI a test pins start disagreeing.
//
// (1) Point/SE: var = σ², and since ∂σ²/∂ln σ = 2σ², the delta method gives
//     se(var) = 2·var·se(ln σ). Verified on the oracle's own arithmetic:
//     .0181469/.1031043 = .17601 = 2·se(ln σ_u).
//
// (2) CI: LOGNORMAL, not symmetric — exp(ln var ± z·se(var)/var), i.e.
//     var·exp(±z·se/var). Verified: exp(ln .1031043 ± 1.96·.17601) =
//     [.0730233, .1455769], which is the oracle to the printed digit. A symmetric
//     var ± z·se would give [.0675, .1387] and fail that pin on sight.
inline double mixed_var_se(double var, double se_ln_sigma) { return 2.0 * var * se_ln_sigma; }

inline void mixed_var_ci(double var, double se_var, double z, double* lo, double* hi) {
    const double s = (var > 0.0) ? se_var / var : 0.0;
    *lo = var * std::exp(-z * s);
    *hi = var * std::exp(z * s);
}

// X is col-major n*k WITH an intercept column (the mixed model's constant is
// identified; unlike the ordered models nothing needs dropping). `gcode` is a dense
// 0-based group code per row, G = number of distinct groups.
MixedLmResult mixedlm_fit(const double* X, int64_t n, int k, const double* y,
                          const int32_t* gcode, int G);

// Per-GROUP score rows at the converged θ (G*np, col-major; parameter order matches
// r.vcov exactly: β, then ln σ_u, then ln σ_e). The raw material for the robust
// sandwich meat Σ_j g_j g_j'.
std::vector<double> mixedlm_group_scores(const double* X, int64_t n, int k, const double* y,
                                         const int32_t* gcode, int G, const MixedLmResult& r);

}  // namespace cecon
