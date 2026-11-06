// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// xtreg, re — random-effects (Swamy-Arora) panel-data linear regression.
//
// Variance components (Stata default, "σ_uT"):
//   σ_e² = Σ_it e_it² / (N - n - K + 1)       e = within-FE residual
//   σ_u² = max{0, SSR_b/(n - K) - σ_e² / T_h}  SSR_b on group means
//                                              T_h = harmonic mean of T_i
//   θ_i  = 1 - σ_e / sqrt(T_i σ_u² + σ_e²)
// Then OLS on transformed (y - θ_i ȳ_i, X - θ_i X̄_i, (1 - θ_i)).
#pragma once
#include <cstdint>
#include <vector>

namespace cecon {

struct XtregReResult {
    int k = 0;                   // # covariates EXCLUDING intercept
    int64_t n_obs = 0, n_groups = 0;
    std::vector<double> beta;    // length k+1; LAST entry is the intercept
    std::vector<double> se;      // length k+1, same order
    std::vector<double> vcov_matrix;  // (k+1)*(k+1) col-major FGLS covariance
    double sigma_e = 0;          // sqrt(σ_e²)
    double sigma_u = 0;          // sqrt(σ_u²)
    double rho = 0;              // σ_u² / (σ_u² + σ_e²)
    double T_h = 0;              // harmonic mean of T_i
    bool ok = true;
};

// Inputs: X is n_obs * k column-major (NO intercept — added internally).
XtregReResult xtreg_re(const double* X, int64_t n_obs, int k,
                       const double* y, const double* ivar);

// xtreg, be — between effects (BUG-1515): OLS of the panel means ȳ_i on X̄_i and
// a constant, one row per panel, unweighted (xtreg, be without `wls`).
// V = s² (X̄'X̄)^-1 with s² = rss / (n_groups - k - 1).
struct XtregBeResult {
    int k = 0;                   // # covariates EXCLUDING intercept
    int64_t n_obs = 0, n_groups = 0;
    std::vector<double> beta;    // length k+1; LAST entry is the intercept
    std::vector<double> se;      // length k+1, same order
    std::vector<double> vcov_matrix;  // (k+1)*(k+1) col-major
    double rss = 0, mss = 0;     // of the regression on the means
    double df_r = 0;             // n_groups - k - 1
    bool ok = false;
};
XtregBeResult xtreg_be(const double* X, int64_t n_obs, int k,
                       const double* y, const double* ivar);

}  // namespace cecon
