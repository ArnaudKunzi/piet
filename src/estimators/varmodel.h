// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// cecon — varmodel: vector autoregression matching Stata `var`, `vargranger`
// and `irf create` (the plain-VAR branch).
//
// ONE engine verb serves all three Stata commands. `var_fit` returns the
// per-equation OLS results, the Granger-causality Wald block, and the full IRF
// array set in a single payload; the frontend renders `vargranger` from the
// stored fit and materializes the `.irf` file (a release-118 .dta) from the IRF
// cells, whose field names below are EXACTLY the .irf column names so no
// translation table is needed on either side.
//
// Estimation is equation-by-equation OLS — every equation has the identical
// regressor list (`lags` lags of each of the K variables plus a constant), so
// the SUR/GLS refinement is algebraically the OLS answer and the system
// covariance is the exact Kronecker Sigma_ml (x) (X'X)^-1.
//
// Oracle: Stata 19.5, internal capture p7_stata.log plus
// the .irf artifact it writes. Assertions in tests/unit/test_varmodel.cpp,
// which also names the quantities this fixture CANNOT pin and why.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace cecon {

// One equation of the system. `b`/`se` follow VarResult::rnames order.
struct VarEquation {
    std::string name;
    std::vector<double> b;
    std::vector<double> se;
    double rmse = 0;    // sqrt(RSS / (T - m)) — Stata's per-equation RMSE (df-adjusted)
    double r2 = 0;
    double chi2 = 0;    // Wald over every coefficient EXCEPT the constant, df = K*lags
    int chi2_df = 0;
    double p = 0;
    // The equation's residual variance is numerically zero (the fit is exact to
    // double precision), so the Wald statistic is 0/0. Stata prints `.` with df
    // 0 in exactly this case; we report chi2_ok = false and never manufacture a
    // number by pseudo-inverting a singular block.
    bool chi2_ok = true;
    bool degenerate = false;
};

// One row of the `vargranger` table: within `equation`, are the lags of
// `excluded` jointly zero? `excluded == "ALL"` is the joint row over every
// other variable's lags.
struct VarGrangerRow {
    std::string equation;
    std::string excluded;
    double chi2 = 0;
    int df = 0;
    double p = 0;
    bool ok = false;  // false => Stata's `.` with df 0 (degenerate equation)
};

// One (step, response, impulse) cell. Field names are the `.irf` file's column
// names verbatim. `mse` is the h-step forecast STANDARD error of `response`
// (sqrt of the MSE), which is what Stata stores; it does not vary with impulse.
// `sirf`/`dm`/`cdm`/`sfevd` and their std* partners are structural-VAR columns
// and stay missing for a plain `var`, so they are not carried here.
struct VarIrfCell {
    int step = 0;
    int response = 0;
    int impulse = 0;
    double irf = 0, cirf = 0, oirf = 0, coirf = 0;
    double stdirf = 0, stdcirf = 0, stdoirf = 0, stdcoirf = 0;
    double fevd = 0, stdfevd = 0, mse = 0;
    // Orthogonalized quantities need the Cholesky factor of Sigma; when Sigma is
    // not numerically positive definite they are undefined rather than zero.
    bool orth_ok = true;
};

struct VarResult {
    enum Fail { NONE, TOO_FEW, SINGULAR, BAD_SPEC };
    bool ok = false;
    Fail fail = NONE;

    int K = 0;         // number of endogenous variables
    int lags = 0;      // p
    int m = 0;         // regressors per equation = K*lags + 1
    int64_t n_obs = 0; // T — usable rows after the lag sample is formed
    double t_first = 0, t_last = 0;  // Stata's "Sample: <first> thru <last>"

    std::vector<std::string> names;   // K variable names, equation order
    std::vector<std::string> rnames;  // m regressor names: L1.y ... Lp.xK, _cons
    std::vector<VarEquation> eqs;

    std::vector<double> sigma;  // K*K row-major, ML divisor T (E'E / T)
    // Full system covariance, row-major, index (a*m + r) for equation a,
    // regressor r: Sigma_ml (x) (X'X)^-1. Stata's `var` uses the ML Sigma here
    // (measured: it reproduces every printed Std. err. on the P7 oracle; the
    // T - m divisor does not).
    std::vector<double> vcov;

    double ll = 0, det_sigma = 0, fpe = 0, aic = 0, hqic = 0, sbic = 0;
    bool sigma_pd = false;  // Cholesky of sigma succeeded

    std::vector<VarGrangerRow> granger;
    int irf_steps = 0;
    std::vector<VarIrfCell> irf;  // (steps+1) * K * K cells, step-major
};

// Y is n x K COLUMN-major (column c = variable c, K = names.size()) in the
// caller's row order; `time` holds the tsset time value of each row. The
// estimation sample is every row whose own values AND all `lags` lagged values
// (at time - j*delta) are present and non-missing — Stata's tsset semantics,
// gaps included. `names` labels the equations, so every string the result
// carries (equation names, regressor names, Granger rows) is already the
// caller's varlist and nothing downstream has to relabel.
//
// irf_steps < 0 skips the IRF block entirely (var/vargranger only).
VarResult var_fit(const double* Y, const double* time, int64_t n,
                  const std::vector<std::string>& names, int lags, int irf_steps, double delta);

}  // namespace cecon
