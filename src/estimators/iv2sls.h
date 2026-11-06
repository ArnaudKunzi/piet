// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// cecon — 2SLS / IV with high-dimensional fixed effects (FWL within transform).
#pragma once
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "core/model.h"
#include "hdfe/dof.h"
#include "vcov/vcov.h"

namespace cecon {

// One endogenous regressor's SAVED FIRST STAGE — the object `ivreg2 …, savefirst`
// stores under `e(firsteqs)` and a paper then `estimates restore`s and publishes as
// a table column (paper 068 `01_analysis_main_tables.do:253-266`).
//
// It is NOT a bespoke variance. The receipt that first specified this said e(V) was
// the system's robust/cluster block and would DIFFER from `regress`; REFUTE.md
// showed the opposite, and the captured oracle
// (tests/eval/golden/aer_b4/ivreg2-savefirst.log) settles it:
//   * arm A (robust): mreldif(V_first, V_regress_robust) = 0x1.4a01fb62d38e5p-43
//     ~ 1.5e-13 and ratio_V11 = 1 - 1e-13 — the same matrix. The `:3092` rescale in
//     ivreg2.ado is exactly HC0 -> HC1, i.e. it MAKES the first stage `regress,
//     robust` rather than moving it away.
//   * arm E (cluster): mreldif(V) = 0x1.5e33e2d8538f1p-47 ~ 1e-14 vs
//     `regress …, cluster(cl)`. Same again, through the DIFFERENT `:3095-3096`
//     branch. The nulls in the same log (a permuted regressor order, and classical
//     OLS) come back at mreldif ~1.0 and ~0.93, so the comparator is not vacuous.
//   * arm F: the first stage is INVARIANT to the parent's `small`
//     (mreldif = +0.0000000000000X-3ff, i.e. exactly 0, and dfr unchanged) while the
//     PARENT's own V moves by a factor 1.042. ivreg2's help (help ivreg2) states that the first-stage
//     results are always reported with small-sample statistics.
// So: the stored first stage is `regress <endog> <exexog> <inexog>, <parent vce>`
// with the small-sample correction always on, whatever the parent asked for.
//
// The ONE place it is not `regress`: `df_r`. ivreg2 posts N - iv1_ct -
// dofminus - sdofminus unconditionally, so under cluster() it is N-K where Stata's
// own `regress, cluster()` would post M-1 — measured in arm E as dfr_fs = 70 against
// dfr_ols = 8 on the same fit. Every t, p and CI a consumer computes off a restored
// first stage therefore uses N-K. b and V match; the reference distribution does not.
struct IvFirstStage {
    std::string depvar;              // the endogenous regressor this stage explains
    std::vector<std::string> names;  // ivreg2 cnZ1 order: exexog, inexog, then the constant
    std::vector<double> beta, se;
    std::vector<double> vcov_matrix;  // kw*kw, column-major, in `names` order
    std::string vcov_type;
    int n_clusters = 0;
    int64_t n = 0;
    int64_t df_r = 0;   // ivreg2 posts N - iv1_ct - dofminus, EVEN under cluster
    int df_m = 0;       // slopes only (arm A: 4 of 5 columns)
    int k_eq = 0;       // ivreg2 posts the ENDOGENOUS COUNT here, not this eq's k
    double rmse = 0;    // ivreg2: sqrt(rss/(N-L-dofminus-sdofminus))
    double rss = 0;
    // ivreg2's e(first) column for this regressor (s_ffirst),
    // the 21 rows in ivreg2_first_stat_rows() order. Empty when not computed: absorbed
    // fixed effects (no captured dofminus/sdofminus regime) or a multiway VCE.
    std::vector<double> ffirst;
};

// Row names of ivreg2's e(first), in order (as ivreg2 posts them).
inline const std::vector<std::string>& ivreg2_first_stat_rows() {
    static const std::vector<std::string> rows = {
        "rmse", "sheapr2", "pr2", "F", "df", "df_r", "pvalue",
        "SWF", "SWFdf1", "SWFdf2", "SWFp", "SWchi2", "SWchi2p", "SWr2",
        "APF", "APFdf1", "APFdf2", "APFp", "APchi2", "APchi2p", "APr2"};
    return rows;
}

struct IvResult {
    int64_t n = 0;
    int k = 0;  // kexog + kendog
    std::vector<std::string> names;  // exog (incl intercept if any) then endog
    std::vector<double> beta;
    std::vector<double> se;
    std::vector<double> vcov_matrix;
    std::string vcov_type;

    double ssr = 0;
    double sigma2 = 0;
    DofInfo dof;
    int n_clusters = 0;
    int cluster_dof_k = 0;
    std::vector<std::string> nested_fe;
    bool vcov_npd_fix = false;

    int demean_iters = 0;
    double demean_resid = 0;
    bool demean_converged = true;
    int64_t n_dropped = 0;

    std::vector<std::string> endog_names;
    // Excluded instruments dropped as collinear AFTER the FE absorption (§35).
    // Stata's ivreghdfe silently omits these and proceeds; empty on every fit that
    // has nothing to omit, which is what keeps the JSON byte-identical there.
    std::vector<std::string> omitted;
    // BUG-1225: included exogenous regressors dropped as collinear after the FE
    // absorption (ivreghdfe posts them as omitted, b 0 and se 0, and leaves them out
    // of K), and the pre-omission [exog, endog] names that place them in the reply.
    // Both empty on every fit with nothing to omit.
    std::vector<std::string> omitted_exog;
    std::vector<std::string> full_names;
    std::vector<double> first_stage_F;  // per endogenous regressor (homoskedastic)
    // `ivreg2 …, savefirst`: the full per-endogenous first-stage estimate, so a
    // consumer can register it as a stored estimate. Empty unless savefirst was
    // asked for, which is what keeps every existing fit byte-identical.
    std::vector<IvFirstStage> first_stage;

    // Weak-instrument + overidentification stats (ivreg2/ivreghdfe parity).
    // Computed for the robust/cluster cases; NaN otherwise or when not applicable.
    double kp_wald_F = std::numeric_limits<double>::quiet_NaN();  // Kleibergen-Paap rk Wald F (e(widstat))
    double hansen_j = std::numeric_limits<double>::quiet_NaN();   // Hansen J overid stat (e(j))
    int hansen_j_df = 0;                                          // overid dof = kinst - kendog
    double hansen_j_p = std::numeric_limits<double>::quiet_NaN(); // chi2 p-value
    double kp_lm = std::numeric_limits<double>::quiet_NaN();      // Kleibergen-Paap rk LM underid (e(idstat)); kendog==1
    int kp_lm_df = 0;                                             // iddf = kinst (for kendog==1)
    double kp_lm_p = std::numeric_limits<double>::quiet_NaN();    // chi2 p-value (e(idp))
    double eff_F = std::numeric_limits<double>::quiet_NaN();      // Montiel-Olea-Pflueger effective F (weakivtest r(F_eff)); kendog==1

    // Model Wald chi2 (Stata ivregress "Wald chi2(df_m)"): joint test that every
    // slope coefficient (excludes the constant) is 0, using the ACTIVE vcov
    // (whatever vt/small combination was requested). NaN if there are no slopes
    // or the slope vcov block is not invertible.
    double chi2 = std::numeric_limits<double>::quiet_NaN();  // e(chi2)
    int chi2_df = 0;                                          // e(df_m)
    double rmse = std::numeric_limits<double>::quiet_NaN();   // sqrt(RSS/N), e(rmse) (asymptotic, always N not N-K)
};

// Demeans md in place. cluster_index selects the cluster for Vcov::Cluster.
// gmm=true selects the efficient two-step GMM estimator (Stata `ivregress gmm`):
// 2SLS first step → robust efficient weight matrix Omega^-1 (default wmatrix(robust),
// cluster-robust when vt=Cluster) → beta=(C'Omega^-1 C)^-1 C'Omega^-1 d, C=W'X, d=W'y,
// with robust GMM SEs V=(C'Omega^-1 C)^-1. Just-identified GMM coincides with 2SLS.
// small=false (default) matches Stata `ivregress`'s ASYMPTOTIC default: sigma2=RSS/N
// (iid), HC0 (no N/(N-K) factor, robust), no G/(G-1)*(N-1)/(N-K) factor (cluster).
// small=true restores the small-sample factors (Stata `ivregress ..., small`).
// Ignored when gmm=true (GMM's own sandwich VCE is untouched either way).
// `savefirst` additionally fills IvResult::first_stage (see IvFirstStage above).
// Default false leaves the fit and its JSON byte-identical to before it existed.
// `multiway_subset_factors` (FU-387): with small=true and a multiway VCE, Stata
// ivregress's `small` multiway variance -- compute_vcov's built-in convention
// (per-combination G_S/(G_S-1)*(N-1)/(N-K), the full-matrix clip). Read nowhere else;
// default false keeps every fit byte-identical.
// md.weights (aweight/pweight only): the fit is weighted the way ivreg2 weights it,
// with a weighted within transform under --fe (iv_apply_weights in iv2sls.cpp);
// empty leaves the fit untouched.
IvResult iv2sls_fit(IvModelData& md, Vcov vt, int cluster_index = 0, bool gmm = false,
                    bool small = false, bool savefirst = false,
                    bool multiway_subset_factors = false);

// Multi-outcome batch: fit the SAME 2SLS design (exog/endog/instruments/fe in
// md) against several outcomes, sharing the expensive FE absorption of the X
// side, (W'W)⁻¹, the projected Xhat, the first-stage gammas and bread, and all
// the weak-instrument stats (which depend only on the instruments, not y). Only
// the per-outcome demean(y)/solve/residuals/vcov/Hansen-J are recomputed.
// `ys[o]` is outcome o's values aligned to md's kept rows (length md.n), assumed
// jointly observed (the caller enforces a common sample). md.Xexog/Xendog/Zinst
// are demeaned in place once. md.weights weight every outcome alike (the X side
// once, each y in turn). Returns one IvResult per outcome.
std::vector<IvResult> iv2sls_fit_multi(IvModelData& md, std::vector<std::vector<double>>& ys,
                                       Vcov vt, int cluster_index = 0, bool gmm = false,
                                       bool small = false, bool savefirst = false);

}  // namespace cecon
