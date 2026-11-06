// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Borusyak, Jaravel & Spiess imputation estimator for staggered-adoption
// event studies, implemented from the published methodology paper:
//
//   [BJS24] Borusyak, K., X. Jaravel and J. Spiess (2024), "Revisiting
//           Event-Study Designs: Robust and Efficient Estimation", Review of
//           Economic Studies 91(6), 3253-3285.
//
// Method source: the paper cited above. Conventions the paper leaves open are
// pinned by this project's own committed goldens and unit tests.
//
// didimpute_run implements BJS24's imputation estimator (§2-3):
//
//   1. Fit the untreated-outcome model Y(0) = alpha_i + beta_t by least squares
//      on the UNTREATED observations only (t < E_i, plus every observation of a
//      never-treated unit).
//   2. Impute each treated cell's counterfactual and take tau_it = Y_it - Y(0)_it.
//   3. tau_h = the unweighted mean of tau_it over the cells at horizon h — the
//      efficient estimator for the "average effect at horizon h" target, whose
//      weights are uniform over that horizon's cells.
//   4. pre-trend coefficients (pretrends()) are NOT imputation estimates: they
//      are the coefficients on 1{t - E_i == -k} leads added to the SAME
//      untreated-sample regression (BJS24 §4's pre-trend test), so the cells
//      they measure remain inside the Y(0) estimation sample.
//
// Inference is the paper's asymptotically-linear representation: each estimate
// is a known linear combination v'Y, and the variance is the cluster (unit) sum
// of squares of v_it * eps_it. Conventions pinned by this project's goldens
// (tests/unit/test_eventstudy.cpp) rather than by any prior implementation:
//
//   * autosample drops a treated cell whose two fixed effects are not both
//     identified off the untreated sample (unit or period absent from it, or in
//     a different connected component), and names the coefficients that lost
//     sample. A unit with NO untreated observation at all — which includes a
//     unit whose treat_time precedes the first observed period — therefore
//     contributes nothing but its own dropped cells.
//   * The reported observation count is the Y(0) estimation sample plus the
//     retained treated cells at the requested horizons.
//   * For a treated cell, the residual entering the variance is its deviation
//     from its own (cohort, horizon) cell mean, so cross-cohort effect
//     heterogeneity is not counted as sampling noise. Measured against the
//     answer key alongside the pooled-horizon centering, which misses.
//   * The horizon estimates carry NO small-sample factor; the pre-trend
//     regression coefficients carry the cluster factor
//     G/(G-1) * (N-1)/(N-K) with K = rank(design) - #units. Inference is
//     normal (z), not t.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace cecon {

struct DidImputeConfig {
    std::vector<double> horizons;  // requested post-treatment event times (horizons())
    int pretrends = 0;             // number of pre-treatment leads (pretrends())
    bool autosample = false;       // autosample: drop cells whose FEs cannot be imputed
    double level = 95;
};

// One estimated relative-period coefficient: a post-treatment tau_h
// (is_pretrend == false) or a pre-treatment placebo (is_pretrend == true).
struct DidImputeCoef {
    std::string name;       // "tau0", "pre1", ...
    double horizon = 0;     // h for tau_h; k (positive) for pre_k
    bool is_pretrend = false;
    double estimate = 0;
    double se = 0;
    double z = 0;
    double p = 0;
    double ci_lo = 0, ci_hi = 0;
    int64_t n_cells = 0;      // cells actually used
    int64_t n_cells_all = 0;  // cells before the autosample drop
};

struct DidImputeResult {
    std::vector<DidImputeCoef> coefs;

    int64_t n_obs = 0;        // Stata "Number of obs"
    int64_t n_model = 0;      // Y(0) estimation-sample rows
    int64_t n_units = 0;      // units contributing to the Y(0) model
    int64_t n_clusters = 0;
    int64_t n_dropped = 0;    // treated cells dropped because an FE was unimputable
    double level = 95;

    // autosample() may drop cells; the printed table warns and NAMES them.
    bool autosample_dropped = false;
    std::string autosample_message;
    std::vector<std::string> autosample_coefs;

    bool ok = true;
    std::string error;
};

// y[n_obs]: outcome. ivar[n_obs]: unit id. time[n_obs]: period. treat_time[n_obs]:
// each unit's first-treated period (constant within unit); NaN marks a
// never-treated unit. An observation is treated iff time >= treat_time.
DidImputeResult didimpute_run(const double* y, const double* ivar, const double* time,
                              const double* treat_time, int64_t n_obs,
                              const DidImputeConfig& cfg = {});

}  // namespace cecon
