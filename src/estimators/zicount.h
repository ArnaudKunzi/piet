// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// cecon — zero-inflated count MLEs: `zip` (zero-inflated Poisson) and `zinb`
// (zero-inflated negative binomial), matching Stata `zip`/`zinb` with a
// CONSTANT-ONLY inflation equation (`inflate(_cons)`).
//
// The mixture. With inflation probability π = Λ(γ) (γ the single inflation
// coefficient) and count mean μ_i = exp(x_i'β):
//
//   Pr(y_i = 0) = π + (1-π) f(0 | μ_i)
//   Pr(y_i = y) =     (1-π) f(y | μ_i),   y > 0
//
// where f is Poisson(μ) for zip and NB2(μ, α) for zinb. The NB2 kernel — and its
// (β, lnα) scores — is the same one src/estimators/nbreg.cpp derives; only the
// zero cell is re-weighted by the inflation mixture.
//
// Parameterization: θ = [β (k, intercept included by the caller), γ] for zip and
// θ = [β, γ, lnα] for zinb, so np = k+1 / k+2. α is optimized in lnα (positive by
// construction) and reported in both metrics, se(α) = α·se(lnα) — the same delta
// rule tobit.h uses for σ, and the rule Stata's own /lnalpha ↔ alpha block prints.
//
// Optimizer: the tobit.h recipe — analytic gradient, central-difference numerical
// Hessian, symmetrize, spd_inverse(-H), backtracking line search. The FIT SEQUENCE
// mirrors the prefit chain Stata prints ("Fitting constant-only model:" then
// "Fitting full model:"), which is also what keeps the mixture away from the
// degenerate π→0/π→1 corners: a 2-parameter constant-only fit from a moment start
// supplies the intercept and γ that the full model starts from, and zinb starts
// from the converged zip plus its own constant-only lnα.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "core/linalg.h"

namespace cecon {

struct ZiResult {
    bool nb = false;               // false = zip, true = zinb
    int k = 0;                     // count-equation columns (intercept included)
    int np = 0;                    // k+1 (zip) or k+2 (zinb)
    int64_t n = 0, n_zero = 0, n_nonzero = 0;
    std::vector<double> beta;      // length k — count equation
    double gamma0 = 0;             // inflate _cons
    double lnalpha = 0, alpha = 0; // zinb only
    double ll = 0;                 // full-model log-likelihood
    double ll_const = 0;           // constant-only prefit log-likelihood (Stata's prefit chain)
    int iters = 0;
    bool converged = false;
    bool ok = false;
    std::vector<double> theta;     // np — the optimizer's parameter vector
    std::vector<double> vcov;      // np*np observed-information inverse, (β, γ[, lnα]) metric
};

namespace zi {

// ψ(x) — digamma, needed by the zinb lnα score. A local copy of the Cephes-style
// shift-and-asymptotic-series digamma in nbreg.cpp (which is a file-local symbol,
// not exported through nbreg.h): same recurrence, same Bernoulli coefficients, so
// the zinb lnα score is built from exactly the approximation nbreg's is.
inline double digamma(double x) {
    double sum = 0;
    while (x < 6.0) { sum -= 1.0 / x; x += 1.0; }
    double inv = 1.0 / x;
    double inv2 = inv * inv;
    return sum + std::log(x) - 0.5 * inv
         - inv2 * (1.0 / 12.0 - inv2 * (1.0 / 120.0 - inv2 * (1.0 / 252.0)));
}

inline double safe_log(double x) { return std::log(std::max(x, 1e-300)); }

// Log-likelihood, analytic gradient, and per-observation scores at θ.
//
//   X       col-major n*k (the caller supplies the intercept column)
//   nb      false = zip (Poisson kernel), true = zinb (NB2 kernel)
//   theta   [β_0..β_{k-1}, γ] or [β_0..β_{k-1}, γ, lnα]
//   grad    optional summed analytic gradient (length np)
//   scores  optional n*np COLUMN-major per-observation score matrix — same layout
//           convention as X (column p, row i at scores[p*n+i]) — so
//           la::xtx(scores, n, np, ...) is directly the robust meat Σ_i s_i s_i'.
//
// grad and scores are filled by two INDEPENDENT blocks that each re-form the
// `coefficient * X[...]` product rather than sharing a hoisted temp: per the note
// at tobit.h:64-73, hoisting that product into a local forces an extra rounding
// step at the temp's assignment and moves the last bits of every downstream SE.
// The per-observation derivative COEFFICIENTS (ce/cg/ca) are plain scalars, not
// products with X, so they are computed once — the hazard tobit documents is
// specific to the X product, which stays duplicated below. No call site ever asks
// for both at once (the Newton loop passes grad only; zi_scores passes scores only).
inline double ll_grad(const double* X, int64_t n, int k, const double* y, bool nb,
                      const std::vector<double>& theta,
                      std::vector<double>* grad, std::vector<double>* scores = nullptr) {
    const int np = k + (nb ? 2 : 1);
    const double g = theta[static_cast<size_t>(k)];
    // π = Λ(γ) and log(1-π), both in the overflow-free branch of the logistic.
    double pi, log1mpi;
    if (g >= 0) {
        double e = std::exp(-g);
        pi = 1.0 / (1.0 + e);
        log1mpi = -g - std::log1p(e);
    } else {
        double e = std::exp(g);
        pi = e / (1.0 + e);
        log1mpi = -std::log1p(e);
    }
    const double omp = 1.0 - pi;      // 1-π
    const double dpi = pi * omp;      // ∂π/∂γ
    const double a = nb ? std::exp(theta[static_cast<size_t>(k) + 1]) : 0.0;
    const double r = nb ? 1.0 / a : 0.0;
    const double lgr = nb ? std::lgamma(r) : 0.0;
    const double dgr = nb ? digamma(r) : 0.0;

    double LL = 0;
    if (grad) std::fill(grad->begin(), grad->end(), 0.0);
    if (scores) scores->assign(static_cast<size_t>(n) * np, 0.0);
    double* S = scores ? scores->data() : nullptr;

    for (int64_t i = 0; i < n; ++i) {
        double eta = 0;
        for (int p = 0; p < k; ++p) eta += X[static_cast<int64_t>(p) * n + i] * theta[static_cast<size_t>(p)];
        // A trial θ from deep inside the line search can drive η past the exp
        // overflow edge; clamping keeps μ finite so the candidate is REJECTED on
        // its (finite, terrible) likelihood instead of on a NaN.
        if (eta > 500.0) eta = 500.0;
        else if (eta < -500.0) eta = -500.0;
        const double mu = std::exp(eta);
        const double yi = y[i];
        double ce, cg, ca = 0;  // ∂ll_i/∂η, ∂ll_i/∂γ, ∂ll_i/∂lnα
        if (yi == 0.0) {
            // f(0|μ): exp(-μ) for zip, (r/(r+μ))^r for zinb.
            double f0, df0de, df0da = 0;
            if (nb) {
                double rpm = r + mu;
                double lg0 = std::log(r) - safe_log(rpm);
                f0 = std::exp(r * lg0);
                df0de = f0 * (-r * mu / rpm);
                df0da = f0 * (-r) * (lg0 + mu / rpm);
            } else {
                f0 = std::exp(-mu);
                df0de = -mu * f0;
            }
            double D = pi + omp * f0;           // ≥ π > 0
            LL += safe_log(D);
            ce = omp * df0de / D;
            cg = dpi * (1.0 - f0) / D;
            if (nb) ca = omp * df0da / D;
        } else {
            if (nb) {
                double rpm = r + mu;
                LL += log1mpi + std::lgamma(yi + r) - lgr - std::lgamma(yi + 1.0)
                    + r * (std::log(r) - safe_log(rpm))
                    + yi * (safe_log(mu) - safe_log(rpm));
                ce = (yi - mu) / (1.0 + a * mu);
                // ∂ll/∂lnα = -r ∂ll/∂r, the NB2 form nbreg.cpp derives.
                double drdL = (digamma(yi + r) - dgr)
                            + std::log(r) - safe_log(rpm)
                            + (mu - yi) / rpm;
                ca = -r * drdL;
            } else {
                LL += log1mpi - mu + yi * safe_log(mu) - std::lgamma(yi + 1.0);
                ce = yi - mu;
            }
            cg = -pi;
        }
        if (grad) {
            for (int p = 0; p < k; ++p) (*grad)[static_cast<size_t>(p)] += ce * X[static_cast<int64_t>(p) * n + i];
            (*grad)[static_cast<size_t>(k)] += cg;
            if (nb) (*grad)[static_cast<size_t>(k) + 1] += ca;
        }
        if (S) {
            for (int p = 0; p < k; ++p) S[static_cast<int64_t>(p) * n + i] = ce * X[static_cast<int64_t>(p) * n + i];
            S[static_cast<int64_t>(k) * n + i] = cg;
            if (nb) S[static_cast<int64_t>(k + 1) * n + i] = ca;
        }
    }
    return LL;
}

// Model Wald χ² on the SLOPE block of a fitted covariance: q = b_s' V_ss^{-1} b_s
// over the leading `ks` parameters (the design-matrix contract puts the expanded
// slopes first and the intercept last, and the inflation/lnα rows after that, so
// the leading ks×ks block IS the slope block for zip/zinb/fracreg alike).
//
// Returns false — "no testable statistic" — when that block is numerically
// degenerate, and only then. TWO independent detectors, because either can be the
// one that notices:
//   (a) a relative floor on the trace: a variance block whose total is ~0 next to
//       the coefficients it is supposed to describe carries no information, no
//       matter how cleanly it factorizes;
//   (b) spd_inverse refusing the block outright.
// The floor is set at 1e-16 of (Σ b_s² + 1) — 1e8 times below the ~1e-8 ratio a
// merely ill-conditioned real fit produces, and 1e8 above the ~1e-24 ratio a
// machine-epsilon-residual quasi-MLE produces (`fracreg logit` on an outcome that
// IS the model's own inverse-logit: Stata itself prints `Wald chi2(0) = .` there).
// A honest missing statistic beats a number read off a singular inverse.
inline bool wald_chi2(const double* V, int np, const double* b, int ks, double& chi2_out) {
    if (ks <= 0 || np < ks) return false;
    std::vector<double> A(static_cast<size_t>(ks) * ks);
    double tr = 0, scale = 1.0;
    for (int c = 0; c < ks; ++c) {
        for (int rr = 0; rr < ks; ++rr)
            A[static_cast<size_t>(c) * ks + rr] = V[static_cast<size_t>(c) * np + rr];
        tr += V[static_cast<size_t>(c) * np + c];
        scale += b[c] * b[c];
    }
    if (!(tr > 1e-16 * scale)) return false;
    if (la::spd_inverse(A.data(), ks) != 0) return false;
    double q = 0;
    for (int c = 0; c < ks; ++c)
        for (int rr = 0; rr < ks; ++rr)
            q += b[c] * A[static_cast<size_t>(c) * ks + rr] * b[rr];
    if (!std::isfinite(q) || q < 0) return false;
    chi2_out = q;
    return true;
}

}  // namespace zi

// `fracreg logit` — the fractional-response quasi-MLE. It needs no estimator core
// of its own (logit.cpp's IRLS already maximizes the Bernoulli quasi-likelihood
// for any y in [0,1]; only the binary-y validation differs), so what lives here is
// just its two likelihood scalars. They sit in this header because
// apps/cmds/cmds_zicount.cpp — the P7 command TU — is where `fracreg` is wired.
namespace frac {

// Quasi log-likelihood Σ y log p + (1-y) log(1-p).
inline double loglik(const double* p, const double* y, int64_t n) {
    const double eps = 1e-12;
    double ll = 0;
    for (int64_t i = 0; i < n; ++i) {
        double pi = std::min(1.0 - eps, std::max(eps, p[i]));
        ll += y[i] * std::log(pi) + (1.0 - y[i]) * std::log(1.0 - pi);
    }
    return ll;
}

// Constant-only quasi-MLE log-likelihood. For the canonical (logit) link the
// intercept-only fit is closed form at p = ȳ, so no second IRLS is needed —
// the same identity add_mle_fit() uses for logit/probit's ll_0.
inline double null_loglik(const double* y, int64_t n) {
    double sy = 0;
    for (int64_t i = 0; i < n; ++i) sy += y[i];
    double ybar = (n > 0) ? sy / static_cast<double>(n) : 0.0;
    if (!(ybar > 0.0 && ybar < 1.0)) return 0.0;  // degenerate outcome — null undefined
    return static_cast<double>(n) * (ybar * std::log(ybar) + (1.0 - ybar) * std::log(1.0 - ybar));
}

}  // namespace frac

// zip: constant-only prefit (moment start) → full model. X col-major n*k with the
// caller's intercept column; y must be non-negative counts.
ZiResult zip_fit(const double* X, int64_t n, int k, const double* y);

// zinb: zip_fit → constant-only zinb prefit (supplies lnα) → full model.
ZiResult zinb_fit(const double* X, int64_t n, int k, const double* y);

// n*np column-major per-observation score matrix at the converged θ — the robust /
// cluster meat's raw material. Empty when r is not a converged fit.
std::vector<double> zi_scores(const double* X, int64_t n, int k, const double* y,
                              const ZiResult& r);

// Robust (sandwich) VCE over the FULL θ: V = factor · bread · meat · bread, with
// meat = Σ_i s_i s_i' from zi_scores and bread = r.vcov (the converged
// observed-information inverse, reused as-is). The full-θ form is deliberate — the
// inflation γ and lnα rows are what the /lnalpha SE is read from, and dropping
// them would still leave the β SEs correct, so only that row catches the mistake.
// `factor` is the caller-supplied ML small-sample multiplier (see cmds_zicount.cpp
// for which one matches Stata, verified against the P7 oracle rather than assumed).
// Empty when r is not a converged fit.
std::vector<double> zi_robust_vcov(const double* X, int64_t n, int k, const double* y,
                                   const ZiResult& r, double factor);

// Cluster-robust VCE: identical sandwich, but the meat sums the per-CLUSTER score
// totals. `codes` is a dense 0..n_clusters-1 group code per row.
std::vector<double> zi_cluster_vcov(const double* X, int64_t n, int k, const double* y,
                                    const ZiResult& r, const int* codes, int n_clusters,
                                    double factor);

}  // namespace cecon
