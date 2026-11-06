// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// cecon — fixed-effects OLS: HDFE demean (FWL within transform) + OLS on the
// demeaned data, with iid / HC1 / one-way-cluster standard errors.
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "core/model.h"
#include "hdfe/dof.h"
#include "vcov/vcov.h"

namespace cecon {

struct FeolsResult {
    int64_t n = 0;
    int k = 0;  // number of coefficients; under FE this includes the reconstructed
                // reghdfe "_cons" row (GAP B), which fill_fit_scalars treats as the
                // intercept so df_m/F stay over the slope set only.
    std::vector<std::string> names;
    std::vector<double> beta;
    std::vector<double> se;
    std::vector<double> vcov_matrix;  // k*k, column-major
    std::string vcov_type;

    double ssr = 0;
    double tss_within = 0;
    double tss_total = 0;  // centered Σ(y_raw-ȳ)² on the ORIGINAL y (pre-within); FE only, else 0
    double r2_within = 0;
    double sigma2 = 0;

    // Model-fit scalars (Stata `regress` e()), filled post-vcov by fill_fit_scalars.
    // For the no-FE OLS path these match Stata `regress` exactly (Doctrine ①); under
    // real FE r2/r2_a follow the within convention (feols is the OLS/no-FE path, §7).
    double mss = 0;        // model SS = tss_within - ssr (centered when no FE)
    double rss = 0;        // residual SS (== ssr; explicit Stata-named scalar)
    double r2 = 0;         // centered R² (no-FE: == r2_within; FE: reghdfe FULL-MODEL e(r2) via tss_total)
    double r2_a = 0;       // adjusted R² = 1 - (1-r2)*(n-1)/dof_resid
    double rmse = 0;       // root MSE = sqrt(sigma2)
    double f_stat = 0;     // model Wald F over the slopes (vce-dependent); NaN if undefined
    int df_m = 0;          // model dof = # non-intercept slopes (Stata e(df_m))
    int64_t df_r = 0;      // F reference resid dof (Stata e(df_r)): G-1 cluster else dof_resid
    double df_r_exact = 0; // df_r without the iw rounding; drives Prob>F and the t CIs

    // Stata weights ([aw=]/[fw=]/[iw=]/[pw=]). All four stay 0/None on the
    // unweighted path, which is how every consumer tells the two apart — an
    // unweighted fit is byte-identical to before weights existed.
    WeightKind weight_kind = WeightKind::None;
    double sum_w = 0;            // Σ of the RAW weights over the estimation sample
    double n_eff = 0;            // Stata e(N): row count under aw/pw, Σw under fw/iw
    double dof_resid_exact = 0;  // n_eff - k_full; FRACTIONAL under iw (dof.dof_resid is its rounding)

    DofInfo dof;
    int n_clusters = 0;
    int cluster_dof_k = 0;              // params counted in the cluster (N-1)/(N-k) factor
    std::vector<std::string> nested_fe;  // FE nested within a cluster (dof-exempt)
    bool vcov_npd_fix = false;           // true if multiway vcov needed PSD repair
    // Stata's built-in multiway convention only (multiway_subset_factors under
    // Vcov::Multiway): e(rank) from the clipped (slopes, _cons) variance
    // (multiway_vcov_rank) and the e(kcluster) combinations (multiway_cluster_combos).
    // -1 / empty on every other fit.
    int vcov_rank = -1;
    std::vector<ClusterCombo> kcluster;
    // The same unweighted multiway fit's e(V_modelbased): the bread, V/s2
    // (regress's, and areg/xtreg,fe's under FE), laid out like vcov_matrix (the reconstructed _cons last
    // under FE). Empty on every other fit.
    std::vector<double> vcov_modelbased;
    // Stata `regress`'s e(beta) ([R] regress, Stored results): b_j*sd(x_j)/sd(y)
    // over the estimation sample, one entry per kept non-intercept column, in
    // `names` order. Filled only on the unweighted no-FE fit with an intercept.
    std::vector<std::pair<std::string, double>> beta_std;
    // Stata `regress`'s exact-fit rule fired (feols_fit's regress_exact_fit): RSS,
    // sigma2, V and se are 0. The reply then posts t, p and the CI as missing
    // (Stata's r(table) prints `.` there). False on every other fit.
    bool exact_fit = false;
    // On that fit, the bread inv(X'X) that V = 0 scales, laid out like vcov_matrix:
    // `test` still tells dependent constraints apart under it. Empty otherwise.
    std::vector<double> exact_fit_bread;

    int demean_iters = 0;
    double demean_resid = 0;
    bool demean_converged = true;
    int64_t n_dropped = 0;
    std::vector<std::string> omitted;  // collinear columns dropped (Stata omits, coef 0)
    std::vector<std::string> full_names;  // pre-drop design-matrix column order (BUG-112(6))
};

// Demeans md.dm in place. cluster_index selects which md.cluster dimension to
// use for Vcov::Cluster.
// no_intercept (Stata `regress ..., noconstant`): drops the constant column
// before fitting. Only meaningful when md.fe is empty (FE already absorb any
// constant); the caller is responsible for rejecting --no-intercept with --fe.
// Default false leaves md/the fit byte-identical to before this parameter existed.
// keep_nested_fe_dof (BUG-568, Stata `areg`): the cluster/multiway small-sample
// factor charges every absorbed parameter, including an FE nested in the cluster
// (compute_vcov's switch of the same name). Default false = reghdfe/xtreg,fe.
// multiway_subset_factors (FU-365, Stata `regress`/`areg` with vce(cluster a b)):
// compute_vcov's switch of the same name — Stata's built-in multiway convention
// (per-subset small-sample factors, then the negative-eigenvalue clip). Under
// absorbed FE the clip acts on the full (slopes, _cons) matrix, as Stata's does.
// Read only by Vcov::Multiway. Default false = reghdfe's Gmin convention. With it,
// K follows keep_nested_fe_dof: true is areg's minus = rank + dfa, false xtreg,fe's
// rank + dfax (an FE nested in a cluster is exempt). An [iweight=] is normalised to
// mean 1, as regress does before its multiway VCE: e(N) is the row count and the factor's N the rows
// (w2s_multiway_class.log:1320 mw_reg_c2_iw e(N) 180, se(x1) = the pweight fit's).
// Without the switch, a multiway Gmin fit with absorbed FE clips its _cons row in
// reghdfe's standardised units (compute_vcov clip_scale; FU-389); its slopes keep
// the data-unit clip they always had.
// regress_exact_fit (cmd_feols `--regress-exact-fit`): Stata's builtin `regress`
// treats a fit with e(r2) > 1 - 1e-14 as exact (RSS 0; see feols.cpp). Only the
// regress route asks for it: reghdfe, areg and xtreg,fe keep the computed RSS
// (StataBE `reghdfe yx x, noabsorb` e(rss) 8.526e-28, _se[x] 1.72e-16). It acts on
// an unweighted no-FE fit under the iid VCE only. Default false.
FeolsResult feols_fit(ModelData& md, Vcov vt, int cluster_index = 0, bool no_intercept = false,
                      bool keep_nested_fe_dof = false, bool multiway_subset_factors = false,
                      bool regress_exact_fit = false);

// Multi-outcome batch: fit the SAME design (X/fe/cluster in md) against several
// outcomes, sharing the expensive HDFE absorption of X and the (X'X)⁻¹ factor —
// only the per-outcome demean(y)/solve/residuals/vcov are recomputed. `ys[o]`
// is outcome o's values aligned to md's kept rows (length md.dm.n), assumed
// jointly observed (the caller enforces a common sample). md.dm.X is demeaned in
// place once. Returns one FeolsResult per outcome. no_intercept: see feols_fit.
std::vector<FeolsResult> feols_fit_multi(ModelData& md, std::vector<std::vector<double>>& ys,
                                         Vcov vt, int cluster_index = 0,
                                         bool no_intercept = false);

}  // namespace cecon
