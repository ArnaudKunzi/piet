// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Generalized linear models, fitted the way Stata's DEFAULT `glm` fits them.
//
// This is not "a GLM fitter". Stata's default glm is a specific optimizer
// lifecycle, and every non-converged `iterate(k)` answer, every e(V), and every
// e(ic) depends on reproducing that lifecycle step for step, not on reaching the
// same maximum. The lifecycle below is pinned against two StataBE captures:
// tests/eval/golden/aer_w2/w2_glm_lifecycle.log (the arms named below) and
// tests/eval/golden/aer_estim2_glm_oracle.log.
//
//   1. START. mu0 = (y + ybar)/2 (binomial: (y + .5)/2),
//      eta0 = g(mu0), one weighted least-squares step of the working response
//      z = eta0 + (y-mu0)/g'(mu0) - offset on X with weight w*dmu^2/V(mu0).
//      That solution is b_0, which is what `iterate(0)` reports (gam_it0).
//      The Pearson scale is SEEDED as sum w*(y-mu(b_0))^2 / V(mu0) / (N-k) --
//      the variance is the START one, not V(mu(b_0)). Only `iterate(0)`'s e(V)
//      can see that (gam_it0: measured 1.2e-14 with V(mu0), 1.5e-2 without).
//   2. NEWTON-RAPHSON (ml, technique nr). At each point the log likelihood,
//      score and OIM Hessian are divided by a phi SNAPSHOT taken BEFORE the
//      Pearson scale is updated at that point (fixed at 1 for Poisson,
//      Bernoulli and fixed-alpha negative binomial). The step is the full
//      Newton step, then:
//        - if it did not improve the log likelihood, halve toward the old point
//          until it does;
//        - otherwise try to step FORWARD: add 1/8, 1/4, 1/2, ... of the Newton
//          step while each addition strictly improves (bin_logit_it1 takes
//          1.375, the gam_trace log prints "(1) Stepping forward").
//   3. STOP (ml's rule): (mreldif(b) <= tolerance OR reldif(ll) <= ltolerance)
//      AND g*inv(H)*g' < nrtolerance. Defaults 1e-6 / 1e-7 / 1e-5.
//   4. e(V) is inv(H) at the final point -- with the STALE phi of step 2, i.e.
//      the Pearson scale of the PREVIOUS iterate, not the reported e(phi)
//      (w2_glm_lifecycle STALEPHI: 6e-13 against phi(b_{ic-1}), 3e-3 against
//      phi(b_ic)). Cluster: inv(H) S'S inv(H) * G/(G-1), S_g = sum over the
//      cluster of the per-observation scores (also divided by the stale phi).
//   5. gaussian/identity with no offset is a closed form: Stata forces
//      iterate(0) and reports the WLS start as converged (e(ic) 0).
//
// Precision (TOLERANCE.md): every w2_glm_lifecycle arm reproduces to <= 5e-12
// relative in b and V. Three arms of the first capture (auto_B, auto_H_offset,
// auto_H_exposure: the weighted price-Poisson fit) land at ~1e-7 because one
// forward-step comparison in that fit is decided by roundoff (the two log
// likelihoods agree to ~4e-15 relative); that is Stata's own precision there,
// not a defect to fit to (T3).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace cecon {

// The families and links this fitter has an oracle for. Everything else glm
// accepts (igaussian, binomial with a denominator, nbinomial ml, power/opower/
// cloglog/loglog/logc/reciprocal links, a non-default link for the covered
// families) is refused by name at the caller, never approximated here.
enum class GlmFamily { Gaussian, Bernoulli, Poisson, Gamma, NegBinomial };
enum class GlmLinkFn { Identity, Log, Logit, Probit };

struct GlmOptions {
    GlmFamily family = GlmFamily::Gaussian;
    GlmLinkFn link = GlmLinkFn::Identity;
    double alpha = 1.0;    // negative binomial alpha, FIXED (family(nbinomial #)); default 1
    double tol = 1e-6;     // tolerance()   -- mreldif of the coefficient vector
    double ltol = 1e-7;    // ltolerance()  -- reldif of the log likelihood
    double nrtol = 1e-5;   // nrtolerance() -- g*inv(H)*g'
    int maxiter = 300;     // iterate(), default c(maxiter)
};

struct GlmResult {
    bool ok = false;
    std::string error;       // set when !ok (singular start, non-finite likelihood)
    int64_t n = 0;           // observations used
    int k = 0;               // columns of X (all estimable -- the caller drops base levels)
    std::vector<double> beta;       // k
    std::vector<double> V;          // k*k row-major
    std::vector<double> gradient;   // k, the score at the final point (e(gradient))
    std::vector<double> ilog;       // log likelihood after iteration 0..ic (e(ilog) head)
    std::vector<double> steps;      // step multiplier taken at iteration 1..ic
    int ic = 0;                     // e(ic)
    bool converged = false;         // e(converged)
    double ll = 0.0;                // e(ll)
    double phi = 1.0;               // e(phi): Pearson scale at the final point (1 when fixed)
    double deviance = 0.0;          // e(deviance)
    double deviance_p = 0.0;        // e(deviance_p), the Pearson chi2
    int64_t n_clusters = 0;         // e(N_clust); 0 = no cluster VCE
    double sum_w = 0.0;             // sum of the raw analytic weights (0 when unweighted)
};

// X: column-major n*k design (the caller appends the constant column itself).
// y: n responses. aweight: raw analytic weights (normalized to mean 1 here, as
// Stata does), nullptr = unweighted. offset: added to the linear predictor
// with coefficient 1, nullptr = none. cluster: dense group ids 0..G-1, nullptr
// = the OIM VCE. The caller has already removed rows with missing values.
GlmResult glm_fit(const double* X, int64_t n, int k, const double* y,
                  const double* aweight, const double* offset, const int64_t* cluster,
                  const GlmOptions& opt);

// Inverse link and its first two derivatives in eta -- what `margins` after
// glm differentiates (dmu/deta for a continuous dy/dx, the delta-method
// gradient for both continuous and factor effects).
double glm_mean(GlmLinkFn link, double eta);
double glm_dmean(GlmLinkFn link, double eta);
double glm_d2mean(GlmLinkFn link, double eta);

}  // namespace cecon
