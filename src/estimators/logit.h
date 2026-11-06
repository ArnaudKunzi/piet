// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Binary logit (MLE via IRLS / Newton-Raphson). A building block for the csdid
// DR-IPW propensity score; exposes the pieces the influence functions need.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace cecon {

struct LogitResult {
    int k = 0;
    std::vector<double> beta;    // coefficients (k)
    std::vector<double> p;       // fitted probabilities (n)
    std::vector<double> bread;   // (X'WX)^{-1}, k*k — the MLE asymptotic cov / inverse info
    int iters = 0;
    bool converged = false;
    bool separated = false;
    bool separated_perfect = false;
    int ic = 0;                  // e(ic): Newton iterations after iteration 0
    double ll = 0.0;             // e(ll): the ml objective at beta (see binary_ml_fit)
    std::vector<double> ilog;    // log likelihood at iteration 0..ic
    int64_t n_cdf = 0, n_cds = 0;  // e(N_cdf) / e(N_cds) (BinaryMlResult)
};

// Fit binary logit y ~ X. X is column-major n*k (caller includes an intercept
// column if wanted); y in {0,1}. Since FU-138 this is Stata's `logit` with its
// default maximize options: logit_fit_ml below with iterate(maxit), started at
// the constant-only fit when X's LAST column is the constant (load_glm's layout;
// any other layout starts at 0 and reaches the same optimum by another path).
// Every caller that refits a posted logit (cmds_postest's lincom/test/margins/
// predict refits) therefore reproduces the posted iterate (the stop rule is
// ml's, rule 3 below). A NON-binary y (fracreg's fractional outcome) keeps the
// pre-FU-138 deviance-convergence IRLS from beta = 0, whose relative deviance
// tolerance is `tol`; the binary ml objective does not define that model.
// weights (P5, pweight): optional per-row weight, length n, nullptr = unweighted;
// the objective is Sum_i weights_i * loglik_i and r.bread is the inverse of its
// Hessian, the weighted-observed-information bread a pweight sandwich needs
// (logit is a canonical link, so observed info == Fisher info, unlike probit's
// BUG-140 case).
// offset (BUG-331): optional per-row term ADDED to the linear predictor with a
// coefficient fixed at 1 — eta_i = X_i beta + offset_i — Stata's `offset()`.
LogitResult logit_fit(const double* X, int64_t n, int k, const double* y,
                      int maxit = 300, double tol = 1e-14,
                      const double* weights = nullptr,
                      const double* offset = nullptr);

// ---- maximum-likelihood fit of a binary-outcome model (logit / probit / cloglog) ----
// Maximize options of binary_ml_fit; the defaults are Stata's documented ones ([R] Maximize).
struct MlMaxOptions {
    double tol = 1e-6;     // tolerance()
    double ltol = 1e-7;    // ltolerance()
    double nrtol = 1e-5;   // nrtolerance()
    int maxiter = 300;     // iterate(), default c(maxiter)
    bool difficult = false;
    bool doopt = false;
    bool step_forward = true;
    bool stata_invsym = false;
    bool ignore_nrtol = false;
    // Concavity is "the Cholesky inverse of A = -H succeeds": a badly conditioned but
    // positive definite A stays concave whatever its pivots, and any other A takes the
    // Marquardt step rather than ending the fit (ivprobit / ivtobit).
    bool spd_concavity = false;
};

// ---- the maximizer behind binary_ml_fit, for any likelihood ------------------------
class MlObjective {
public:
    virtual ~MlObjective() = default;
    virtual double loglik(const std::vector<double>& b) = 0;
    // ll at b, its gradient g, and A = -H, both sized for b.
    virtual double evaluate(const std::vector<double>& b, std::vector<double>& g,
                            std::vector<double>& A) = 0;
};

struct MlPath {
    std::vector<double> b;              // the iterate where the fit ended
    std::vector<double> inv_curvature;  // inverse of A = -H at b, k*k
    std::vector<double> ilog;           // ll at iteration 0..ic
    double ll = 0.0;
    int ic = 0;
    bool converged = false;
    bool concave = true;                // A at b counted as concave
    bool failed = false;                // non-finite ll, or no usable direction
};

// Stata's modified Newton-Raphson from `start` under opt.
MlPath ml_newton_path(MlObjective& obj, std::vector<double> start, const MlMaxOptions& opt);

// The links binary_ml_fit fits: Pr(y != 0) = logistic, normal or 1 - exp(-exp(xb)).
enum class BinaryLink { Logit, Probit, Cloglog };

struct BinaryMlResult {
    std::vector<double> beta;    // k
    std::vector<double> p;       // fitted probabilities at beta (n)
    std::vector<double> bread;   // inv(H) at beta, k*k (symmetric)
    std::vector<double> ilog;    // ll at iteration 0..ic
    int ic = 0;
    double ll = 0.0;
    bool converged = false;
    bool singular = false;       // H could not be inverted; iteration stopped
    // Rows completely determined at the final beta (logit / probit): e(N_cdf)
    // failures and e(N_cds) successes.
    int64_t n_cdf = 0, n_cds = 0;
};

// X column-major n*k; when has_constant the LAST column is the constant (the
// load_glm layout) and starts at b0. weights: pweight (ll = sum w_i ll_i),
// nullptr = unweighted. offset: added to eta with coefficient 1, nullptr = none.
// start (size k): ml's init() instead of that start (cloglog's logit start).
BinaryMlResult binary_ml_fit(BinaryLink link, const double* X, int64_t n, int k, const double* y,
                             const double* weights, const double* offset, bool has_constant,
                             const MlMaxOptions& opt, const std::vector<double>* start = nullptr);

// Generalized inverse of a symmetric k x k A (shared with ivml): the columns kept
// (largest pivot swept first; a pivot <= 1e-9 x its original diagonal or below
// 1e-19 is dropped), the inverse built on them (false when the kept block does not
// invert), and the concavity test of A = -H against V = invsym(A).
std::vector<char> invsym_kept_columns(const std::vector<double>& A, size_t k);
bool invsym_generalized(const std::vector<double>& A, size_t k, std::vector<double>& out);
bool ml_check_concave(const std::vector<double>& A, const std::vector<double>& V, size_t k);

// ---- perfect-prediction screen for binary outcomes ---------------------------------
struct BinaryPerfectRule {
    int col = -1;          // design column the rule names
    int op = 0;            // e(rules)[.,1]: 1 `!=`, 2 `>`, 3 `<`; 4 omitted because of
                           // collinearity (cmds_mle.cpp binary_screen_glm, not this screen)
    double rhs = 0.0;      // e(rules)[.,2]
    int interp = 0;        // e(rules)[.,3]: 0 failure, 1 success, -1 keep x == rhs, -2 inestimable
    int64_t dropped = 0;   // e(rules)[.,4]
    // A rule on a factor's base level (BinaryBaseLevel) names no design column:
    // col -1, the indicator `base_var == base_level`, e(rules) row `<level>b.<var>`.
    std::string base_var;
    double base_level = 0.0;
};

// The base level of a factor main effect (`i.g`, `ib#.g`) as a column the screen
// tests: Stata's `_rmcoll ..., logit expand` lists `0b.g` ahead of `1.g ...`, and a
// base level that predicts the outcome perfectly leaves with its rows like any
// other level (StataBE `logit d0 x i.g` with d0 = 0 on every g == 0 row: "0.g != 0
// predicts failure perfectly; 0.g omitted and 10 obs not used", e(N) 30, then 3.g
// omitted because of collinearity). The design carries only the non-base levels.
struct BinaryBaseLevel {
    std::string var;          // the factor variable
    double level = 0.0;       // its base level
    int before = 0;           // the design column it precedes in the expanded varlist
    std::vector<double> col;  // per design row: 1 where var == level, else 0
};

struct BinaryPerfectScreen {
    std::vector<BinaryPerfectRule> rules;
    std::vector<char> keep_row;   // n: the row survives every rule
    std::vector<char> omit_col;   // k: the column was dropped by a rule
    bool inestimable = false;     // rule 3 fired: r(2000)
};

// X column-major n*k; only the first `ncheck` columns are screened (the
// constant, the LAST column of the load_glm layout, is never a candidate).
BinaryPerfectScreen binary_perfect_screen(const double* X, int64_t n, int k, int ncheck,
                                          const double* y);

// The IRLS fit's separated_perfect screen (every row on its own side of 1/2 and
// a mean deviance below 1e-6), applied to the ml path's probabilities.
bool binary_predicts_perfectly(const std::vector<double>& p, const double* y, int64_t n);

// True when X's last column is identically 1 (the constant binary_ml_fit starts at b0).
bool last_column_is_constant(const double* X, int64_t n, int k);

// True when every y is exactly 0 or 1 (binary_ml_fit's domain; logit_fit /
// probit_fit route any other outcome to their IRLS fallback).
bool outcome_is_binary(const double* y, int64_t n);

// logit through binary_ml_fit. bread = inv(H); iters = ic; separated = singular.
LogitResult logit_fit_ml(const double* X, int64_t n, int k, const double* y,
                         const MlMaxOptions& opt, const double* weights = nullptr,
                         const double* offset = nullptr, bool has_constant = true);

}  // namespace cecon
