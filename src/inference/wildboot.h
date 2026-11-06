// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// cecon — wild cluster bootstrap-t (WCR: impose the null), fast algorithm.
// Demeans + fits once, then each replicate is O(G^2) in the per-cluster pieces —
// no per-replicate refit. Bit-reproducible via the Philox counter RNG.
#pragma once
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "core/model.h"
#include "core/rng_mt64.h"

namespace cecon {

// boottest's weighttype(): Rademacher (the default), Webb's
// six-point, Mammen's two-point, the standard normal and a centred gamma (BUG-1519).
enum class BootWeights { Rademacher, Webb, Mammen, Normal, Gamma };

// The name r(weighttype) posts ("rademacher", "webb", "mammen", "normal", "gamma"),
// and its inverse (false for any other spelling).
const char* boot_weights_name(BootWeights w);
bool boot_weights_from_name(const std::string& name, BootWeights& w);

// Stata's own boottest weight draws (boottest's MakeWildWeights,
// Rademacher): v = (runiform(G, B+1) :>= .5) :- .5, column 1 overwritten with the
// original, drawn from the session stream right after the ado's
// `set seed` and filled row by row -- row = the cluster's
// position in ascending cluster-value order, column b+1 = replicate b. Pass one
// to a kernel to replace the Philox draws; `taken` comes back as the raw draws
// consumed (G*(B+1), or 0 when the 2^G set is enumerated and nothing is drawn).
// Webb, Mammen, normal and gamma weights keep the Philox draws (their Stata order is
// not implemented).
struct StataWildDraws {
    Mt64 stream;                       // the state boottest draws from
    std::vector<int32_t> cluster_rank; // cluster code -> Stata row (0-based)
    uint64_t taken = 0;                // out: raw draws consumed
};

struct WildBootResult {
    int reps = 0;       // replications run: 2^G when the Rademacher set is enumerated
    std::string weights;
    std::string param;
    double null_beta = 0;
    double beta = 0;   // unrestricted point estimate of the tested coef
    double se = 0;      // its cluster-robust SE
    double t0 = 0;      // (beta - null)/se
    double t_ref = 0;   // t*(v = +1) from the draws' kernel: the p comparison reference
    // boottest's getp() (boottest's): #{|t*| > |t_ref|} / reps and
    // 2 * min(#{t* < t_ref}, #{t* > t_ref}) / reps, strict comparisons.
    double p_symmetric = 0;
    double p_equaltail = 0;
    int n_clusters = 0;
    uint64_t seed = 0;
    bool webb_fallback = false;  // always false: Webb only when asked for (as in boottest)
    std::vector<double> tstars;  // per-replicate bootstrap t (for tests/inspection)
    double score_numerator = 0;  // ML: sum of null-score influences, not beta-null
    // wild_restricted_efficient_bootstrap only: boottest's df_r (the smallest
    // error-clustering count less one) and the fit's kappa.
    double df_r = std::numeric_limits<double>::quiet_NaN();
    double kappa = std::numeric_limits<double>::quiet_NaN();
};

// boottest's WRE / multiway wild bootstrap-t (wild_restricted_efficient_bootstrap).
struct WreSpec {
    int param_index = 0;    // tested coefficient in [exog..., endog...]
    double null_beta = 0;
    int reps = 999;
    uint64_t seed = 0;
    BootWeights weights = BootWeights::Rademacher;
    bool liml = false;      // replication estimator LIML (kappa re-solved per draw), else 2SLS/OLS
    bool small = false;     // boottest `small` (e(df_r) set): N_c/(N_c-1) per clustering and
                            // smallsample (N - k - fe_dof_adj)/(N - 1) (as in boottest)
    double fe_dof_adj = 0;  // boottest's FEdfadj
};

// WCR wild cluster bootstrap-t for FE-OLS, testing H0: beta[param_index] = null_beta.
// Demeans md.dm in place. cluster_index selects the md.cluster dimension.
WildBootResult wild_cluster_bootstrap(ModelData& md, int param_index, double null_beta,
                                      int reps, uint64_t seed, int cluster_index = 0,
                                      BootWeights weights = BootWeights::Rademacher,
                                      StataWildDraws* stata_draws = nullptr);

// WCR wild cluster bootstrap-t for 2SLS, testing the single endogenous regressor
// = null_beta (the common IV case). Currently supports exactly one endogenous
// regressor, which is the parameter tested. Demeans md in place.
WildBootResult wild_cluster_bootstrap_iv(IvModelData& md, double null_beta, int reps,
                                         uint64_t seed, int cluster_index = 0,
                                         BootWeights weights = BootWeights::Rademacher);

// Wild bootstrap-t by direct refit of every replicate, boottest 4.5.3's
// algorithm (plus/b/boottest.mata) for two cases the fast kernel above does not
// cover:
//  - IV (p >= 1 endogenous): the wild restricted efficient bootstrap (Davidson &
//    MacKinnon 2010; boottest's WRE): the null-restricted model is fitted
//    by LIML (boottest's DGP.LIML = 1), the endogenous regressors' first
//    stage is re-estimated controlling for its residual (boottest's MakeResiduals),
//    and both equations' residuals are flipped by the same draw; each replicate
//    is re-fitted by 2SLS, or by LIML with kappa re-solved (spec.liml), and its
//    cluster-robust t computed with the P_W X score rows. The tested coefficient
//    may be exogenous or endogenous.
//  - OLS (p = 0, kz = 0) with several cluster variables: the WCR bootstrap with
//    boottest's multiway error variance (every non-empty intersection of the
//    clusterings, signed by parity) and bootstrap clusters = the intersection of
//    all of them (per boottest).
// No fixed effects or weights. Xexog / Y2 / Zexcl are column-major n x kx / p / kz;
// `clusters` are the error clusterings. reps is 2^G when the Rademacher set is
// enumerated (G <= log2(reps)), as in wcb_core.
WildBootResult wild_restricted_efficient_bootstrap(
    const std::vector<double>& y, int64_t n, const std::vector<double>& Xexog, int kx,
    const std::vector<std::string>& exog_names, const std::vector<double>& Y2, int p,
    const std::vector<std::string>& endog_names, const std::vector<double>& Zexcl, int kz,
    const std::vector<Factor>& clusters, const WreSpec& spec);

// Thread count the wild-cluster-bootstrap replicate loop uses for `reps`
// replicates over `G` clusters: threads_for_work with the loop's work model
// (each replicate is the G×G contraction) and its per-thread scratch (the
// transient v(G) plus allocator slack). Exposed for test_thread_determinism's
// anti-vacuity check.
int wildboot_fanout_threads(int reps, int G);

// Null-imposed ML score bootstrap, one tested slope and one error/bootstrap
// cluster. X is column-major with the constant LAST. Models: logit, probit,
// poisson, glm-poisson, glm-gaussian. Only the zero null is currently served.
// The full unconstrained information is evaluated at the constrained optimum.
WildBootResult wild_score_bootstrap(const std::vector<double>& X, int64_t n, int k,
                                    const std::vector<double>& y, const Factor& cluster,
                                    int param_index, const std::string& param,
                                    const std::string& model, double null_beta, int reps,
                                    uint64_t seed, BootWeights weights,
                                    StataWildDraws* stata_draws = nullptr);

}  // namespace cecon
