// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// cecon — instrumental-variables MLE: `ivprobit` and `ivtobit`, Stata's
// full-information maximum-likelihood "model with endogenous regressors".
//
// MODEL. One endogenous regressor y2, structural exogenous regressors x (which
// include the constant), excluded instrument(s) z. Write the structural design as
// Xs = [y2, x…, 1] and the reduced-form design as Xw = [x…, z…, 1] (that order is
// what Stata's footer prints as "Endogenous: y2 / Exogenous: x z"):
//
//     y1* = Xs·δ + u        (δ = (α, β), α the coefficient on y2)
//     y2  = Xw·Π + v
//     (u, v) ~ BVN(0, [[σ_u², ρ σ_u σ_v], [ρ σ_u σ_v, σ_v²]])
//
// ivprobit observes y1 = 1{y1* > 0} and fixes σ_u = 1; ivtobit observes y1*
// censored at ll/ul and estimates σ_u = σ_e.
//
// LIKELIHOOD. The joint density factors as f(y1 | y2)·f(y2). With v = y2 − Xw·Π
// the marginal term is the plain normal log-density of v,
//
//     ln f(y2) = −ln σ_v − ½ln 2π − ½ (v/σ_v)²
//
// and the conditional term follows from u | v ~ N(ρ σ_u v/σ_v, σ_u²(1−ρ²)):
//
//   ivprobit:  m  = (Xs·δ + (ρ/σ_v)·v) / √(1−ρ²)
//              ln f(y1|y2) = ln Φ(q·m),  q = 2y1 − 1
//   ivtobit:   μ_c = Xs·δ + (ρ σ_e/σ_v)·v,   σ_c = σ_e √(1−ρ²)
//              uncensored → normal log-density of (y1−μ_c)/σ_c minus ln σ_c
//              left-censored at ll  → ln Φ((ll−μ_c)/σ_c)
//              right-censored at ul → ln(1 − Φ((ul−μ_c)/σ_c))
//
// The ivtobit censored branches are the SAME censored-normal forms as
// estimators/tobit.h, evaluated at the conditional mean/scale instead of the
// unconditional ones.
//
// PARAMETRIZATION. θ = (δ[0..ks−1], Π[0..kw−1], atanh ρ, ln σ_v) for ivprobit and
// θ = (…, atanh ρ, ln σ_v, ln σ_e) for ivtobit, so np = ks+kw+2 (+1 for ivtobit).
// ρ = tanh(athrho) keeps |ρ| < 1 with no constraint machinery, and — this is the
// error-prone part — athrho enters m BOTH through the numerator (ρ v/σ_v) and
// through the 1/√(1−ρ²) scale. Working the chain rule through gives the pleasantly
// compact ∂m/∂athrho = (v/σ_v + ρ·Xs·δ)/√(1−ρ²); the gradient self-check at the top
// of tests/unit/test_ivml.cpp is what certifies it (and every other partial)
// against a central difference of the very same log-likelihood.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "core/linalg.h"

namespace cecon {

struct IvmlResult {
    // Structural coefficients in Xs column order: [endog, x…, intercept].
    std::vector<double> beta;
    std::vector<double> se;     // model-based (observed information); the command
                                // layer overrides these from a sandwich under --vce
    // Reduced-form coefficients in Xw column order: [x…, instruments…, intercept].
    std::vector<double> pi;
    std::vector<double> se_pi;

    double athrho = 0, se_athrho = 0;   // atanh ρ and its SE (the metric of θ)
    double rho = 0, se_rho = 0;         // ρ = tanh(athrho); SE by delta method
    double rho_lo = 0, rho_hi = 0;      // tanh-transformed CI (filled by the caller)

    double ln_sigma_v = 0, se_ln_sigma_v = 0;
    double sigma_v = 0, se_sigma_v = 0;
    double ln_sigma_e = 0, se_ln_sigma_e = 0;  // ivtobit only
    double sigma_e = 0, se_sigma_e = 0;        // ivtobit only

    double ll = 0;
    double ll_exog = 0;                  // the exogenous prefit's LL (starting values)
    int64_t n = 0, n_unc = 0, n_lc = 0, n_rc = 0;
    int ks = 0, kw = 0, np = 0;
    int i_athrho = 0, i_lnsv = 0, i_lnse = -1;  // θ / vcov indices of the ancillaries
    int iters = 0;
    bool converged = false;
    bool ok = false;
    std::vector<double> theta;  // np — the converged parameter vector
    std::vector<double> vcov;   // np*np in θ's metric
};

namespace ivm {

inline double ncdf(double x) { return 0.5 * std::erfc(-x * 0.70710678118654752440); }
inline double npdf(double x) { return 0.39894228040143267794 * std::exp(-0.5 * x * x); }

// Log-likelihood + analytic gradient for ivprobit at θ = (δ, Π, athrho, lnσ_v).
// Xs is col-major n*ks (its FIRST column is y2, the endogenous regressor; the
// intercept is LAST), Xw is col-major n*kw. y1 is in {0,1}; y2 is passed
// separately because the reduced-form residual v = y2 − Xw·Π needs it in its own
// right (Xs's first column holds the same numbers).
//
// scores: optional n*np COLUMN-major output (same layout as X — column p, row i at
// scores[p*n+i]); when non-null it is filled with the SAME per-observation gradient
// contribution that would otherwise only be summed into *grad, so la::xtx(scores,
// n, np, …) gives the robust meat Σ_i s_i s_i' directly (and summing score ROWS
// within a cluster first gives the cluster meat).
//
// As in tobit.h and oglm.h the grad and scores accumulations are written as two
// INDEPENDENT branches that deliberately do not share an intermediate: `(*grad)[p]
// += c * X[…]` is one fused expression eligible for -ffp-contract FMA fusion, and
// hoisting the product into a shared temp forces an extra rounding step that moves
// the last bits of every downstream SE. No call site ever asks for both at once
// (the Newton loop passes grad only, the sandwich passes scores only), so the
// duplicated recompute costs nothing in practice.
inline double ivprobit_ll_grad(const double* Xs, const double* Xw, int64_t n, int ks, int kw,
                               const double* y1, const double* y2,
                               const std::vector<double>& theta, std::vector<double>* grad,
                               std::vector<double>* scores = nullptr) {
    const int ia = ks + kw, ig = ks + kw + 1, np = ks + kw + 2;
    const double rho = std::tanh(theta[static_cast<size_t>(ia)]);
    const double s2 = 1.0 - rho * rho;  // = sech²(athrho) > 0 for every finite athrho
    const double s = std::sqrt(s2);
    const double g = theta[static_cast<size_t>(ig)];
    const double sv = std::exp(g);
    const double log2pi = 1.8378770664093454836;
    double LL = 0;
    if (grad) std::fill(grad->begin(), grad->end(), 0.0);
    if (scores) scores->assign(static_cast<size_t>(n) * np, 0.0);
    double* S = scores ? scores->data() : nullptr;
    for (int64_t i = 0; i < n; ++i) {
        double d = 0;
        for (int p = 0; p < ks; ++p) d += Xs[static_cast<int64_t>(p) * n + i] * theta[static_cast<size_t>(p)];
        double wp = 0;
        for (int p = 0; p < kw; ++p) wp += Xw[static_cast<int64_t>(p) * n + i] * theta[static_cast<size_t>(ks + p)];
        const double c = (y2[i] - wp) / sv;  // v/σ_v
        const double m = (d + rho * c) / s;
        const double q = (y1[i] > 0.5) ? 1.0 : -1.0;
        double Phi = ncdf(q * m);
        if (Phi < 1e-300) Phi = 1e-300;
        LL += -g - 0.5 * log2pi - 0.5 * c * c + std::log(Phi);
        if (grad) {
            const double lam = q * npdf(q * m) / Phi;  // ∂LL/∂m
            const double cd = lam / s;
            for (int p = 0; p < ks; ++p)
                (*grad)[static_cast<size_t>(p)] += cd * Xs[static_cast<int64_t>(p) * n + i];
            const double cw = (c - lam * rho / s) / sv;
            for (int p = 0; p < kw; ++p)
                (*grad)[static_cast<size_t>(ks + p)] += cw * Xw[static_cast<int64_t>(p) * n + i];
            (*grad)[static_cast<size_t>(ia)] += lam * (c + rho * d) / s;
            (*grad)[static_cast<size_t>(ig)] += -1.0 + c * c - lam * rho * c / s;
        }
        if (S) {
            const double lam = q * npdf(q * m) / Phi;
            const double cd = lam / s;
            for (int p = 0; p < ks; ++p)
                S[static_cast<int64_t>(p) * n + i] = cd * Xs[static_cast<int64_t>(p) * n + i];
            const double cw = (c - lam * rho / s) / sv;
            for (int p = 0; p < kw; ++p)
                S[static_cast<int64_t>(ks + p) * n + i] = cw * Xw[static_cast<int64_t>(p) * n + i];
            S[static_cast<int64_t>(ia) * n + i] = lam * (c + rho * d) / s;
            S[static_cast<int64_t>(ig) * n + i] = -1.0 + c * c - lam * rho * c / s;
        }
    }
    return LL;
}

// Log-likelihood + analytic gradient for ivtobit at θ = (δ, Π, athrho, lnσ_v, lnσ_e).
// `cls` is the per-row censoring class (0 uncensored, 1 left, 2 right) — the same
// encoding tobit.h uses, and classified the same way (y <= ll / y >= ul), so the
// counts Stata prints come straight out of it.
inline double ivtobit_ll_grad(const double* Xs, const double* Xw, int64_t n, int ks, int kw,
                              const double* y1, const double* y2, const int8_t* cls,
                              double ll_lim, double ul_lim, const std::vector<double>& theta,
                              std::vector<double>* grad, std::vector<double>* scores = nullptr) {
    const int ia = ks + kw, ig = ks + kw + 1, ih = ks + kw + 2, np = ks + kw + 3;
    const double rho = std::tanh(theta[static_cast<size_t>(ia)]);
    const double s2 = 1.0 - rho * rho;
    const double s = std::sqrt(s2);
    const double g = theta[static_cast<size_t>(ig)], h = theta[static_cast<size_t>(ih)];
    const double sv = std::exp(g), se = std::exp(h);
    const double sc = se * s;  // conditional SD σ_c
    const double log2pi = 1.8378770664093454836;
    double LL = 0;
    if (grad) std::fill(grad->begin(), grad->end(), 0.0);
    if (scores) scores->assign(static_cast<size_t>(n) * np, 0.0);
    double* S = scores ? scores->data() : nullptr;
    for (int64_t i = 0; i < n; ++i) {
        double d = 0;
        for (int p = 0; p < ks; ++p) d += Xs[static_cast<int64_t>(p) * n + i] * theta[static_cast<size_t>(p)];
        double wp = 0;
        for (int p = 0; p < kw; ++p) wp += Xw[static_cast<int64_t>(p) * n + i] * theta[static_cast<size_t>(ks + p)];
        const double c = (y2[i] - wp) / sv;   // v/σ_v
        const double mu = d + rho * se * c;   // = Xs·δ + (ρσ_e/σ_v)·v
        // Gm = ∂(conditional LL)/∂μ_c, Gs = ∂(conditional LL)/∂σ_c — the only two
        // channels through which every parameter reaches the conditional term.
        double Lc, Gm, Gs;
        if (cls[i] == 0) {  // uncensored
            const double r = (y1[i] - mu) / sc;
            Lc = -std::log(sc) - 0.5 * log2pi - 0.5 * r * r;
            Gm = r / sc;
            Gs = (r * r - 1.0) / sc;
        } else if (cls[i] == 1) {  // left-censored at ll
            const double A = (ll_lim - mu) / sc;
            double Phi = ncdf(A);
            if (Phi < 1e-300) Phi = 1e-300;
            Lc = std::log(Phi);
            const double lam = npdf(A) / Phi;
            Gm = -lam / sc;
            Gs = -lam * A / sc;
        } else {  // right-censored at ul
            const double A = (ul_lim - mu) / sc;
            double Sv = 1.0 - ncdf(A);
            if (Sv < 1e-300) Sv = 1e-300;
            Lc = std::log(Sv);
            const double del = npdf(A) / Sv;
            Gm = del / sc;
            Gs = del * A / sc;
        }
        LL += -g - 0.5 * log2pi - 0.5 * c * c + Lc;
        if (grad) {
            for (int p = 0; p < ks; ++p)
                (*grad)[static_cast<size_t>(p)] += Gm * Xs[static_cast<int64_t>(p) * n + i];
            const double cw = (c - Gm * rho * se) / sv;
            for (int p = 0; p < kw; ++p)
                (*grad)[static_cast<size_t>(ks + p)] += cw * Xw[static_cast<int64_t>(p) * n + i];
            (*grad)[static_cast<size_t>(ia)] += Gm * s2 * se * c - Gs * rho * sc;
            (*grad)[static_cast<size_t>(ig)] += -1.0 + c * c - Gm * rho * se * c;
            (*grad)[static_cast<size_t>(ih)] += Gm * rho * se * c + Gs * sc;
        }
        if (S) {
            for (int p = 0; p < ks; ++p)
                S[static_cast<int64_t>(p) * n + i] = Gm * Xs[static_cast<int64_t>(p) * n + i];
            const double cw = (c - Gm * rho * se) / sv;
            for (int p = 0; p < kw; ++p)
                S[static_cast<int64_t>(ks + p) * n + i] = cw * Xw[static_cast<int64_t>(p) * n + i];
            S[static_cast<int64_t>(ia) * n + i] = Gm * s2 * se * c - Gs * rho * sc;
            S[static_cast<int64_t>(ig) * n + i] = -1.0 + c * c - Gm * rho * se * c;
            S[static_cast<int64_t>(ih) * n + i] = Gm * rho * se * c + Gs * sc;
        }
    }
    return LL;
}

}  // namespace ivm

// Fit ivprobit. Xs col-major n*ks = [y2, x…, intercept]; Xw col-major n*kw =
// [x…, z…, intercept]. y1 must be in {0,1}. Returns ok=false if the information
// matrix is singular or a starting value could not be formed.
IvmlResult ivprobit_fit(const double* Xs, const double* Xw, int64_t n, int ks, int kw,
                        const double* y1, const double* y2);

// Fit ivtobit. Same design convention; y1 is the (censored) continuous outcome.
IvmlResult ivtobit_fit(const double* Xs, const double* Xw, int64_t n, int ks, int kw,
                       const double* y1, const double* y2, bool has_ll, double ll_lim,
                       bool has_ul, double ul_lim);

// Per-observation score rows at the converged θ (n*np, col-major) — the sandwich
// meat's raw material for vce(robust)/vce(cluster). Parameter order matches
// r.vcov exactly.
std::vector<double> ivprobit_scores(const double* Xs, const double* Xw, int64_t n, int ks, int kw,
                                    const double* y1, const double* y2, const IvmlResult& r);
std::vector<double> ivtobit_scores(const double* Xs, const double* Xw, int64_t n, int ks, int kw,
                                   const double* y1, const double* y2, bool has_ll, double ll_lim,
                                   bool has_ul, double ul_lim, const IvmlResult& r);

}  // namespace cecon
