// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Sun & Abraham interaction-weighted event-study estimator, implemented from
// the published methodology paper:
//
//   [SA21] Sun, L. and S. Abraham (2021), "Estimating dynamic treatment
//          effects in event studies with heterogeneous treatment effects",
//          Journal of Econometrics 225(2), 175-199.
//
// Method source: the paper cited above. Conventions the paper leaves open are
// pinned by this project's own committed goldens and unit tests.
//
// sunab_run implements the interaction-weighted (IW) estimator (SA21 §3) in
// two steps:
//
//   1. SATURATED STAGE. Regress the outcome on the full set of
//      (relative-period indicator) x (cohort) interactions, excluding the
//      control cohort — which contributes only through the absorbed fixed
//      effects and is therefore the reference group — under two-way (unit,
//      period) HDFE absorption with a cluster-robust VCE. This yields the
//      cohort-specific relative-period effects delta_{l,g} (SA21's CATT).
//
//   2. IW AGGREGATION. Each reported relative-period estimate is the
//      cohort-share-weighted average of that period's delta_{l,g}
//      (SA21 eq. 6):  tau_l = sum_g w_{l,g} delta_{l,g},  with w_{l,g} the
//      share of cohort g among the (non-control-cohort) observations at
//      relative period l. Because the weights are ESTIMATED, their sampling
//      variability enters the reported standard error alongside the
//      saturated stage's: the two influence functions are accumulated (the
//      regression's per cluster, the shares' per row) and combined,
//          Var(tau_l) = c_reg * Var_delta-part + Var_weight-part,
//      where c_reg is the regression cluster small-sample factor
//      G/(G-1) * (N-1)/(N-K) and the weight part — a sample-share estimator,
//      not a regression — carries no such factor. Both the block-diagonal
//      form (no delta/weight covariance term) and the K used by c_reg are
//      pinned by this project's goldens; see tests/unit/test_eventstudy.cpp.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace cecon {

struct SunabConfig {
    std::string vce = "cluster";  // eventstudyinteract requires vce(cluster ...)
    double level = 95;
};

// One aggregated relative-period estimate (the IW-pooled coefficient across
// cohorts sharing that relative period).
struct SunabCoef {
    int rel_index = 0;    // position of this indicator in the caller's varlist
    double coef = 0;
    double se = 0;
    double t = 0;
    double p = 0;
    double ci_lo = 0, ci_hi = 0;
    // The saturated stage's per-cohort pieces behind this row.
    std::vector<double> cohort_weight;  // w_{l,g}, sums to 1 over cohorts
    std::vector<double> cohort_coef;    // delta_{l,g}
};

struct SunabResult {
    std::vector<double> cohorts;   // interacted (non-control) cohort values, ascending
    std::vector<SunabCoef> coefs;  // one row per supplied relative-period indicator
    // e(V_iw): the n_rel x n_rel covariance of the coefs' estimates, row-major in
    // varlist order; its diagonal is each coef's se squared (BUG-1524).
    std::vector<double> V_iw;

    // Header scalars of the underlying absorbed regression.
    int64_t n_obs = 0;         // estimation rows (Stata "Number of obs")
    int64_t n_obs_weights = 0; // rows the cohort shares are formed on = the
                               // non-control-cohort part of the estimation
                               // sample (Stata's leading "(obs=...)" line)
    double sum_w = 0;          // weighted only: sum of the weights over the sample
    double sum_w_weights = 0;  // ... and over the share rows ("(sum of wgt is ...)")
    double F = 0;
    int df1 = 0, df2 = 0;
    double r2 = 0, r2_a = 0, rmse = 0;
    int64_t n_clusters = 0;
    int64_t n_interactions = 0;  // K = (#relative periods) x (#cohorts)
    int64_t df_absorb = 0;       // absorbed FE parameters (incl. the intercept)
    int64_t dof_resid = 0;
    double level = 95;

    bool ok = true;
    std::string error;
};

// y[n_obs]: outcome. rel_ind[n_obs * n_rel]: COLUMN-MAJOR matrix of the
// caller's relative-period indicators (eventstudyinteract's varlist) — column
// l is 1 on the rows at relative period l and 0 elsewhere; rows belonging to
// no supplied relative period are the omitted base. cohort[n_obs]: each unit's
// treatment cohort (constant within unit); NaN (missing) is allowed -- a control
// row ignores it, a treated row with it joins no cohort (BUG-1525, sunab.cpp).
// control[n_obs]: nonzero on the control-cohort rows (control_cohort()), which
// are excluded from the interaction set and from the cohort-share sample. fe_unit[n_obs],
// fe_time[n_obs]: the two absorbed fixed effects (absorb()). cluster[n_obs]:
// cluster id for vce(cluster); required by eventstudyinteract.
//
// weight[n_obs] (optional, BUG-1283): an aweight/pweight. Identified by StataBE 19.5
// measurement only: the saturated stage is the
// weighted absorbed regression (`reghdfe ... [aw=w], absorb() vce(cluster)`: e(b), e(V)
// equal), the cohort shares are weight shares (e(ff_w) .49074074 = 53/108 on the BUG-1283
// fixture), the share influence function of a row is w_i (1{g_i = g} - w_{l,g}) / sum_l w
// (e(Sigma_ff) .012977260763659555), and Var(tau_l) keeps its two-part form. Header
// sums are weighted with the weights normalized to sum to n_obs. nullptr: unweighted.
SunabResult sunab_run(const double* y, const double* rel_ind, int n_rel,
                      const double* cohort, const double* control,
                      const double* fe_unit, const double* fe_time,
                      const double* cluster, int64_t n_obs,
                      const SunabConfig& cfg = {}, const double* weight = nullptr);

}  // namespace cecon
