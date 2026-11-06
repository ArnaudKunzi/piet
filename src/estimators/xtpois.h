// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Conditional fixed-effects Poisson (xtpoisson, fe in Stata).
//
// Conditional on the panel sum Σ_t y_it, the unit-specific intercept α_i drops
// out; the conditional log-likelihood reduces to a multinomial-logit form
//   L(β) = Σ_i [ logΓ(n_i + 1) - Σ_t logΓ(y_it + 1) + Σ_t y_it log p_it ]
// with p_it = exp(x_it β + offset_it) / Σ_ℓ exp(x_iℓ β + offset_iℓ).
//
// Newton-Raphson on β. Robust VCE (Wooldridge 1999) is cluster-by-panel:
//   V_robust = (-H)^{-1} [Σ_i s_i s_i'] (-H)^{-1}
// with the standard cluster small-sample correction
//   M/(M-1) * (N-1)/(N-k).
//
// Panels with sum(y)==0 or size==1 are dropped (no information for β).
#pragma once
#include <cstdint>
#include <vector>

namespace cecon {

struct XtpoisFeResult {
    int k = 0;
    int64_t n_obs = 0;          // observations used (after drops)
    int64_t n_groups = 0;       // panels used (after drops)
    int64_t n_dropped_zero = 0; // panels dropped: sum(y) == 0
    int64_t n_dropped_size = 0; // panels dropped: size == 1
    // Row counts of the panels used (after the drops): Stata's e(g_min)/e(g_avg)/
    // e(g_max) (xtpoisson). 0 when no panel is kept.
    int64_t g_min = 0, g_max = 0;
    double g_avg = 0;
    std::vector<double> beta;
    std::vector<double> se_iid;
    std::vector<double> se_robust;
    // The full covariances the se come from (k*k, symmetric): (-H)^{-1} and the
    // Wooldridge sandwich -- e(V) for --emit-v (margins reads it, ME06).
    std::vector<double> V_iid;
    std::vector<double> V_robust;
    double loglik = 0;
    int iters = 0;
    bool converged = false;
};

// Estimate xtpoisson, fe by Newton-Raphson on the conditional log-likelihood.
// X     : column-major n_obs * k covariates (no intercept; FE absorbs it)
// y     : n_obs counts (non-negative integers; treated as doubles)
// ivar  : panel id (any double; rows grouped by equality)
// offset: optional log-offset, length n_obs, or nullptr
XtpoisFeResult xtpoisson_fe(const double* X, int64_t n_obs, int k,
                            const double* y, const double* ivar,
                            const double* offset = nullptr,
                            int maxit = 100, double tol = 1e-10);

}  // namespace cecon
