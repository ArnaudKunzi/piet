// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// cecon — variance estimation shared by OLS and 2SLS. The "design" passed in is
// X for OLS and the instrumented X-hat (= P_W X) for 2SLS; everything else
// (robust/cluster meats, small-sample factors, nested-dof, PSD repair) is common.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/model.h"

namespace cecon {

enum class Vcov { IID, HC1, Cluster, Multiway };

struct VcovResult {
    std::vector<double> V;  // k*k, column-major
    std::string type;
    int n_clusters = 0;
    int cluster_dof_k = 0;
    std::vector<std::string> nested_fe;
    bool npd_fix = false;
};

// One cluster combination of a multiway VCE: its name (`a`, `a#b`, ...) and the
// number of groups of that intersection.
struct ClusterCombo {
    std::string name;
    int levels = 0;
};

// All 2^p - 1 groupings a p-way cluster VCE is built from, one row per column of
// Stata's e(kcluster): singletons, then pairs, and so on, and within one size the
// grouping holding the earlier variable comes first. Empty for p outside 1..8.
// w2s_multiway_class.log:133 `30|7|180|id|id2|id#id2` (two-way) and :1126
// columns 1/4/6/7 `30|180|42|180`, cluster6 `id2#t`, cluster7 `id#id2#t` (three-way).
std::vector<ClusterCombo> multiway_cluster_combos(const std::vector<Factor>& cluster);

// e(rank) after a built-in multiway cluster fit: how many eigenvalues of the final
// k x k variance exceed 1e-12 (an absolute floor; w2s_multiway_class.log:116 two-way
// regress 2, :1115 three-way 1). -1 when V is the wrong size or the solver fails.
int multiway_vcov_rank(const std::vector<double>& V, int k);

// Stata's `ereturn post` screen of the variance it is handed (built-in, identified by
// StataBE 19.5 probes, BUG-1266): for every pair i != j whose two variances are
// positive, a covariance whose magnitude exceeds s = sqrt(V_ii V_jj) by more than a
// relative 1e-10 (a correlation beyond +-(1 + 1e-10)) makes Stata warn "variance
// matrix is nonsymmetric or highly singular" and post V = 0; a smaller excess is
// clamped to sign * s. Pairs with a non-positive variance are not examined, and an
// indefinite V whose pairwise correlations stay within [-1, 1] posts unchanged.
// V is k x k, symmetric. Returns false when V was rejected (and zeroed).
bool stata_post_vcov_screen(std::vector<double>& V, int k);

// Heteroskedasticity-robust meat: sum_i e_i^2 x_i x_i'  (k x k, full).
void hc_meat(const double* design, int64_t n, int k, const double* e, double* meat);

// Per-cluster score rows: S is G x k (column-major), row g = sum_{i in g} x_i e_i.
void cluster_scores(const double* design, int64_t n, int k, const double* e,
                    const Factor& cl, double* S);

// One-way cluster meat: sum_g sg sg', sg = sum_{i in g} x_i e_i. *n_clusters=G.
void cluster_meat(const double* design, int64_t n, int k, const double* e,
                  const Factor& cl, double* meat, int* n_clusters);

// Upper bound on multiway cluster dimensions: the meat sums 2^d - 1 subset meats, so
// the bound keeps that loop finite (255 subsets at 8). A design choice, not a Stata
// rule — reghdfe itself sets none; past it the engine refuses by name.
constexpr int kMaxMultiwayClusters = 8;

// Multiway (Cameron-Gelbach-Miller) meat over d = cls.size() dimensions:
// sum over non-empty subsets S of (-1)^(|S|+1) · meat(intersection of S). No
// small-sample factor is applied here (compute_vcov owns it).
// subset_scale (FU-365): when given, each subset's meat is multiplied by
// (*subset_scale)(G_S), G_S = the number of levels of that subset's intersection,
// before the signed sum. nullptr (the default) keeps the unscaled sum byte-identical.
void multiway_meat(const double* design, int64_t n, int k, const double* e,
                   const std::vector<const Factor*>& cls, double* meat,
                   const std::function<double(int)>* subset_scale = nullptr);

// True iff every level of `fe` maps to a single cluster level (nested).
bool fe_nested_in_cluster(const Factor& fe, const Factor& cl);

// Assemble the requested vcov. bread = (design'design)^{-1} (k x k). sigma2 is
// used only for IID (= ssr/dof_resid). k_full = k + absorbed FE params.
// n_eff: the observation count the HC1 n/(n-k) and cluster (N-1)/(N-k) small-sample
// factors are built from. It equals `n` for every unweighted fit and for analytic/
// probability weights (where Stata's N is the row count); under frequency/importance
// weights Stata's N is Σw instead, so the caller passes that. <=0 (the default) means
// "use n" — which is what keeps every pre-weights caller byte-identical.
//
// ml_cluster_factor (BUG-175): selects which small-sample multiplier the CLUSTER /
// multiway branch applies.
//   false (default) — the REGRESSION (Cameron-Gelbach-Miller) factor
//                     G/(G-1) · (N-1)/(N-K). Correct for feols/reghdfe and 2SLS,
//                     and what Stata's own `regress, vce(cluster)` uses.
//   true            — the PURE ML factor G/(G-1), with no (N-1)/(N-K) term. This is
//                     what Stata's likelihood commands (`poisson`, `logit`, `nbreg`,
//                     `mlogit`, …) use; carrying the regression term there inflates
//                     every SE by sqrt((N-1)/(N-K)) and deflates the model Wald by
//                     (N-1)/(N-K). Measured against the P2 answerbook — see
//                     BUG-175 and the sweep recorded there.
// Only the cluster/multiway branch reads it; IID and HC1 are unaffected. Defaulting
// to false keeps every existing caller byte-identical.
//
// keep_nested_fe_dof (BUG-568): which K the cluster/multiway (N-1)/(N-K) factor uses.
//   false (default) — reghdfe / xtreg,fe: an FE nested in a cluster is EXEMPT, its
//                     (levels-1) parameters leave K and it is listed in nested_fe.
//                     w2b_estim.log reghdfe_cluster_ctl / xtreg_fe_cluster_ctl:
//                     se[x1] .0663182832 (nested cluster id).
//   true            — areg: every absorbed parameter is charged, nested or not
//                     (K = k_full). Same fit, areg_a_cl: se[x1] .0722686736,
//                     N_clust 10, df_r 9. A non-nested cluster is unaffected
//                     (areg_cluster_nonnested agrees with reghdfe).
// IID and HC1 never read it; defaulting to false keeps every caller byte-identical.
//
// multiway_subset_factors (FU-365): which small-sample convention a MULTIWAY vcov uses.
//   false (default) — reghdfe's: one factor Gmin/(Gmin-1)·(N-1)/(N-K) on the whole
//                     inclusion-exclusion sum, then la::psd_clip (see compute_vcov).
//   true            — the convention of Stata's regress, areg, xtreg,fe and ivregress
//                     small ([R] regress, Methods and formulas, Multiway clustering):
//                     the signed sum of one-way cluster-robust sandwiches, each with
//                     its own group-count factor, projected onto the PSD cone only
//                     when it has a negative eigenvalue. areg charges every absorbed
//                     parameter in K (the caller passes keep_nested_fe_dof), xtreg,fe
//                     only those not nested in a cluster. w2l_vce_shape_det.log
//                     regress_vce_c2_se .0643525900770342, areg_vce_c2_se
//                     .0778721308909151; w2s_multiway_class.log:696 xtreg,fe
//                     vce(cluster cg1 cg2) .0532737214421747.
// One-way cluster, IID and HC1 never read it; false keeps every caller byte-identical.
//
// clip_scale (FU-389, reghdfe's convention): when given (length k), the multiway
// Gmin variance is made positive semidefinite the way reghdfe_fix_psd does it
// (as reghdfe does) on reghdfe's standardised data (standardize_data,
// undone by V :/ (s's)): V is scaled by s's, and only if its smallest
// eigenvalue is < 0 are the negative eigenvalues set to 0 over the whole matrix and
// its leading (k-1) x (k-1) block replaced by that block clipped on its own; then the
// scaling is undone. With no negative eigenvalue V is left exactly as computed.
// nullptr (the default) keeps the data-unit la::psd_clip, byte-identical.
// w2s_multiway_class.log:960 reghdfe two-way se(_cons) .4540988323750039 (the data-unit
// clip gave .4539871108580628). Only the Gmin multiway branch reads it.
//
// no_psd_fix: ivreg2's two-way variance (m_omega, S1 + S2 - S12) is posted as summed,
// with no repair of a negative eigenvalue, so a negative diagonal is a missing
// standard error (w2d_uncovered.log:1866 ivreg2_cluster2_aw_030). Only the Gmin
// multiway branch reads it; false keeps every caller byte-identical.
VcovResult compute_vcov(Vcov vt, const double* design, int64_t n, int k, const double* e,
                        const double* bread, int k_full, double sigma2,
                        const std::vector<Factor>& fe,
                        const std::vector<Factor>& cluster, int cluster_index,
                        double n_eff = -1.0, bool ml_cluster_factor = false,
                        bool keep_nested_fe_dof = false,
                        bool multiway_subset_factors = false,
                        const std::vector<double>* clip_scale = nullptr,
                        bool no_psd_fix = false);

}  // namespace cecon
