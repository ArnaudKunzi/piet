// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// cecon — quantile regression (Koenker-Bassett). Frisch-Newton primal-dual
// interior-point solver for the check-loss min Σ ρ_τ(y - Xβ); default i.i.d.
// standard errors via the sparsity function (fitted method, Hall-Sheather
// bandwidth) — matches Stata `qreg` (vce(iid, fitted, hsheather)).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace cecon {

struct QregResult {
    std::vector<double> beta;   // k coefficients
    std::vector<double> se;     // k standard errors (iid, fitted sparsity)
    std::vector<std::string> names;
    double tau = 0.5;
    double sparsity = 0;        // ŝ_τ = 1/f_Y(ξ_τ)   (e(sparsity))
    double bwidth = 0;          // Hall-Sheather bandwidth h   (e(bwidth))
    int64_t n = 0;
    int k = 0;
    double sum_adev = 0;        // Σ ρ_τ(resid) (the minimized objective)
    double dof_resid = 0;       // N - rank
    bool converged = false;
    // qreg_fit_robust only: the full k*k column-major VCE (empty from qreg_fit),
    // and the count of observations whose fitted quantile spread
    // x_i'(β(τ+h) - β(τ-h)) was not positive (crossing quantiles; their density
    // is taken as 0 -- see qreg_fit_robust).
    std::vector<double> V;
    int64_t n_crossing = 0;
};

// Solve the quantile-regression coefficients at `tau`. X is n*k column-major
// (include an intercept column). Returns beta (length k); sets *ok if provided.
std::vector<double> qreg_solve(const double* X, int64_t n, int k, const double* y, double tau,
                               bool* ok = nullptr);

// Full fit with i.i.d. (fitted-sparsity, Hall-Sheather) SEs at confidence `level`
// (default 95 -> alpha 0.05). `names` length k.
QregResult qreg_fit(const double* X, int64_t n, int k, const double* y, double tau,
                    const std::vector<std::string>& names, double level = 95.0);

// Stata `qreg ..., vce(robust)` and `qreg ... [pweight=w], vce(robust)`: the
// default denmethod `fitted` with the default `hsheather` bandwidth. `w` is the
// pweight column (length n, every entry > 0) or nullptr for an unweighted fit.
// beta minimizes Σ w_i ρ_τ(y_i - x_i'β); V is the Hendricks-Koenker sandwich
// (formula and oracle in qreg.cpp). sparsity is not computed on this path
// (NaN): no captured Stata record pins e(sparsity) under vce(robust).
QregResult qreg_fit_robust(const double* X, int64_t n, int k, const double* y, const double* w,
                           double tau, const std::vector<std::string>& names,
                           double level = 95.0);

}  // namespace cecon
