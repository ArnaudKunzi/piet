// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// cecon — `mi impute regress`: multiple imputation of one continuous variable by
// chained linear regression (Rubin's PROPER imputation, [MI] mi impute regress).
//
// WHAT IS PINNED AND WHAT IS OURS. The oracle is StataBE 19.5 on the P7 pangram
// wave (internal capture p7_stata.log,
// `mi impute regress x2 = x1 y, add(2) rseed(20260802) noisily`). Everything
// DETERMINISTIC there is pinned exactly — the inner regression of x2 on x1 y over
// the complete cases (N = 4,649, F(2,4646) = 54.92, R2 = .0231, Root MSE = .73482,
// every coefficient and standard error) and the accounting box (4,671 complete /
// 129 incomplete / 129 imputed / 4,800 total).
//
// The imputed VALUES are OURS, not Stata's. That is settled law from the P6 wave:
// a fixed seed makes our own stream reproducible, correctness is judged on the
// MATHEMATICS, and only the presentation follows Stata. Reproducing Stata's
// pseudo-random draws is not attempted and is not a goal.
//
// THE ALGORITHM (one imputation m, given the complete-case fit):
//   1. b_hat, RSS, (X'X)^-1 from OLS of y on X over the complete cases;
//      k parameters, n complete cases, nu = n - k residual degrees of freedom.
//   2. sigma*^2 = RSS / chi2(nu)                         -- the posterior of sigma^2
//   3. beta*    = b_hat + sigma* L u,  L L' = (X'X)^-1,  u ~ N(0,1)^k
//   4. y*_i     = x_i' beta* + sigma* eps_i,             eps_i ~ N(0,1)
//   Steps 2-4 are exactly the "proper" scheme: a new (beta*, sigma*) per
//   imputation, so the between-imputation variance carries the parameter
//   uncertainty and Rubin's rules apply downstream.
//
// THE COUNTER LAYOUT (this is what makes the reproducibility claim checkable).
// Every draw is addressed (userSeed, stream = m, counter), m = 1..add:
//     counter 0            the chi-square draw for sigma*
//     counter 1 .. k       the k standard normals behind beta*
//     counter k + 1 + j    the residual normal for the j-th imputed ROW
// Nothing is drawn sequentially: draw the m-th imputation alone and it is
// bit-identical to drawing 1..m and keeping the last. Asserted, not claimed, in
// tests/unit/test_miimpute.cpp.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace cecon {

// ---------------------------------------------------------------------------
// Sample classification — the accounting box, computed ONCE, here
// ---------------------------------------------------------------------------
//
// Stata's box is three counts over the SAME 4,800 rows, and they are not the same
// three sets the estimation sample uses:
//   Complete   = rows whose imputed variable is observed          (4,671)
//   Incomplete = rows whose imputed variable is missing             (129)
//   Imputed    = of those, the ones a value could actually be filled in for,
//                i.e. every predictor is observed too               (129)
// and, separately, the inner regression runs over the rows where the imputed
// variable AND every predictor are observed (4,649) — which is neither Complete
// nor Total, because 22 of the complete rows have a missing predictor. Stata says
// so in its own footnote ("Right-hand-side variables ... have missing values;
// model parameters estimated using listwise deletion"), and 4,671 - 4,649 = 22 is
// exactly the y-missing count in this fixture.
struct MiSample {
    int64_t n_total = 0;       // rows in
    int64_t n_complete = 0;    // imputed variable observed
    int64_t n_incomplete = 0;  // imputed variable missing
    int64_t n_imputable = 0;   // imputed variable missing AND every predictor observed
    // Row indices into the ORIGINAL data, ascending.
    std::vector<int64_t> fit_rows;     // imputed variable and every predictor observed
    std::vector<int64_t> impute_rows;  // imputed variable missing, every predictor observed
};

// `y` is the variable being imputed; `xs` are its predictors (raw column pointers,
// NaN = missing). No intercept column is passed — it is implied and never missing.
MiSample mi_classify(const double* y, const std::vector<const double*>& xs, int64_t n);

// ---------------------------------------------------------------------------
// The result
// ---------------------------------------------------------------------------
struct MiImputeResult {
    // --- the inner regression Stata prints under `noisily` -------------------
    int64_t n_fit = 0;      // complete cases used (4,649)
    int k = 0;              // parameters, intercept included (3)
    int64_t df_model = 0;   // k - 1
    int64_t df_resid = 0;   // n_fit - k  == the chi-square nu
    std::vector<double> beta;  // length k, predictors then intercept
    std::vector<double> se;    // length k
    std::vector<double> xtx_inv;  // k*k column-major (X'X)^-1 — the draw's covariance shape
    double rss = 0, tss = 0, mss = 0;
    double r2 = 0, r2_adj = 0, rmse = 0, f_stat = 0, p_f = 0;

    // --- the accounting box --------------------------------------------------
    MiSample sample;
    int add = 0;             // imputations added
    int updated = 0;         // imputations updated (always 0: `add`, never `update`)
    int64_t n_imputed = 0;   // Stata's "Imputed" column = min over m of filled-in rows

    // --- the draws (OURS, see the header note) -------------------------------
    // values[(m-1) * n_imputed + j] is imputation m's value for
    // sample.impute_rows[j]. Laid out imputation-major so a consumer materializing
    // mlong rows walks one imputation at a time.
    std::vector<double> values;
    std::vector<double> sigma_star;  // length add
    std::vector<double> beta_star;   // add * k, imputation-major

    bool ok = false;
    std::string err;
};

// Fit + draw. `Xfit` is the col-major n_fit*k complete-case design with the
// INTERCEPT LAST (load_glm's contract); `yfit` the outcome over those rows.
// `Ximp` is the col-major n_imp*k design at the rows to impute, same column order.
// `seed` is the user's rseed; `add` the number of imputations.
//
// Filling `out.sample` is the caller's job (mi_classify above) — this entry point
// only needs the row COUNT to size the draws, so the mathematics stays testable
// without a dataset.
MiImputeResult mi_impute_regress(const double* Xfit, const double* yfit, int64_t n_fit, int k,
                                 const double* Ximp, int64_t n_imp, int add, uint64_t seed);

// The draws for ONE imputation, given an already-fitted model. Exposed because it
// is what makes order-independence assertable: calling it for m = 2 without ever
// calling it for m = 1 must produce the same numbers.
//   `out_values` is resized to n_imp.
// Returns false (and leaves `out_*` untouched) if the fit is unusable.
bool mi_impute_draw(const MiImputeResult& fit, const double* Ximp, int64_t n_imp, int m,
                    uint64_t seed, std::vector<double>* out_values, double* out_sigma_star,
                    std::vector<double>* out_beta_star);

}  // namespace cecon
