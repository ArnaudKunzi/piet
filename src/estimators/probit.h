// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Binary probit (MLE via IRLS / Fisher scoring). Mirrors logit_fit.
#pragma once
#include <cstdint>
#include <vector>

#include "estimators/logit.h"  // MlMaxOptions, binary_ml_fit

namespace cecon {

struct ProbitResult {
    int k = 0;
    std::vector<double> beta;    // coefficients (k)
    std::vector<double> p;       // fitted Φ(Xβ) (n)
    std::vector<double> V;       // (X'WX)^{-1}, k*k — asymptotic covariance
    int iters = 0;
    bool converged = false;
    bool separated = false;
    bool separated_perfect = false;
    int ic = 0;                    // e(ic)
    double ll = 0.0;               // e(ll), the ml objective (logit.h rule 4)
    std::vector<double> ilog;      // log likelihood at iteration 0..ic
    std::vector<double> bread_oim; // inv(observed information) of the ml Hessian, k*k
    int64_t n_cdf = 0, n_cds = 0;  // e(N_cdf) / e(N_cds) (BinaryMlResult, logit.h)
};

// Fit binary probit y ~ X. X is column-major n*k (caller includes an intercept
// column if wanted); y in {0,1}. Since FU-138 this is Stata's `probit` with its
// default maximize options: probit_fit_ml below with iterate(maxit), started at
// the constant-only fit when X's LAST column is the constant (logit_fit's twin;
// see logit.h). Every refit of a posted probit (cmds_postest, heckman's first
// step, ivprobit's start) therefore reproduces Stata's iterate. A NON-binary y
// keeps the pre-FU-138 Fisher-scoring IRLS from beta = 0 (relative deviance `tol`).
// offset (BUG-331): optional per-row term ADDED to the linear predictor with a
// coefficient fixed at 1 — eta_i = X_i beta + offset_i — Stata's `offset()`;
// probit_observed_info must be given the SAME offset or its eta (and therefore
// the SEs Stata reports) would be computed at the wrong point.
ProbitResult probit_fit(const double* X, int64_t n, int k, const double* y,
                        int maxit = 300, double tol = 1e-14,
                        const double* offset = nullptr);

// probit through binary_ml_fit (Stata's ml path, logit.h). V is the Fisher
// bread (X'WX)^{-1} at beta (clamps: Phi in [1e-12, 1-1e-12]) so the reply's
// `se_iid` key is unchanged in kind; the reported default SE comes from
// bread_oim, the inverse of the Hessian the iteration itself used, which drops
// completely determined rows exactly as Stata's does (w2b_estim.log:762 se
// .4459396018157188 needs it; the all-rows observed information lands 1e-8 off).
// probit_observed_info hands bread_oim out as its `bread`. weights: an fweight or
// iweight (ll = sum w_i ll_i, the Hessian and V weighted alike), nullptr = none.
ProbitResult probit_fit_ml(const double* X, int64_t n, int k, const double* y,
                           const MlMaxOptions& opt, const double* offset = nullptr,
                           bool has_constant = true, const double* weights = nullptr);

// ---- Observed-information covariance for a FITTED probit (BUG-140 / BUG-210) ----
//
// probit's link is NON-canonical, so the Fisher/expected information (X'WX)^{-1}
// that ProbitResult::V carries is NOT the covariance Stata reports. Stata uses the
// OBSERVED Hessian, whose per-row weight is lambda_i(lambda_i + eta_i), where
// lambda_i = (y_i - Phi_i) phi_i / (Phi_i(1 - Phi_i)) is the generalized residual
// and eta_i = x_i'beta. (For logit the link IS canonical and the two coincide,
// which is why LogitResult::bread needs no analogue of this.)
//
// BUG-140 fixed `probit`'s own default SE this way. BUG-210 is the SAME defect
// surviving in `margins`' two iid probit branches, and the mechanism was that the
// loop had been copy-pasted into three call sites and only one of them was swept.
// This is now the ONE definition; every site delegates here.
struct ProbitObservedInfo {
    std::vector<double> bread;   // (observed information)^{-1}, k*k column-major: r.bread_oim
                                 // when the fit carries it (the ml Hessian, logit.h rule 4)
    std::vector<double> meat;    // Sum_i s_i s_i' (s_i = lambda_i x_i), k*k — sandwich meat
    std::vector<double> score;   // lambda_i per row (n): the scalar a clustered meat sums (FU-464)
    bool observed_spd = true;    // false => observed Hessian not SPD, `bread` fell back to r.V
};

// Both pieces are accumulated in ONE pass in the row order i = 0..n-1, with the
// (c, d) inner order the former copies used, so the floating-point summation order
// is unchanged from the loops this replaces. `meat` is filled unconditionally; it
// is cheap relative to the pass and callers that only want `bread` just ignore it.
ProbitObservedInfo probit_observed_info(const ProbitResult& r, const double* X,
                                        int64_t n, int k, const double* y,
                                        const double* offset = nullptr);

}  // namespace cecon
