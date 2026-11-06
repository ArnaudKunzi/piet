// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// cecon — `nl` (nonlinear least squares) and `gmm` (single-equation nonlinear
// GMM), the P7 nonlinear pair.
//
// Both verbs are driven by a USER EXPRESSION compiled through the shared
// evaluator (src/core/expr_eval.h); the command layer turns that expression into
// the one thing this header needs — a residual e_i(b) for row i at parameter
// vector b — and everything below is estimator mathematics with no parsing in
// it. Keeping the callback at that boundary is what lets `nl` and `gmm` share a
// Jacobian routine, a line search and a convergence rule instead of two.
//
// Every formula here is pinned against StataBE 19.5 on the P7 pangram wave
// (internal capture p7_stata.log, lines 833-886); the
// assertions live in tests/unit/test_nlgmm.cpp.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace cecon {

// Residual for row `i` at parameter vector `b`. Both verbs supply one:
//   nl   e_i(b) = y_i - f(x_i; b)      (f is the right side of the `=` split)
//   gmm  e_i(b) = the user's expression, written as a residual
using NlResidFn = std::function<double(const std::vector<double>& b, int64_t i)>;

// One line of the iteration log. `backed_up` reproduces Stata's "(backed up)"
// annotation: the full Gauss-Newton step did not improve the objective and the
// step had to be shortened. The iteration COUNT is deliberately not a parity
// target — our line search is our own; only the converged numbers are pinned.
struct NlIter {
    double obj = 0;         // nl: residual SS.  gmm: the GMM criterion Q(b).
    bool backed_up = false;
};

// ---------------------------------------------------------------------------
// Shared numeric Jacobian
// ---------------------------------------------------------------------------

// J[j*n + i] = d e_i / d b_j by CENTRAL differences with step
//     h_j = cbrt(DBL_EPSILON) * max(|b_j|, 1)
// (~6.06e-6 * scale), the step that balances the O(h^2) truncation error of a
// central difference against the O(eps/h) cancellation error. Stata's `nl` uses
// forward differences internally; central is used here because it is the safer
// choice for a hand-rolled Gauss-Newton, and the converged (J'J)^-1 it produces
// matches Stata's reported SEs well past the wave's 1e-5 relative tolerance
// (measured: worst leg 3e-7 relative — tests/unit/test_nlgmm.cpp).
//
// The divisor is the REALIZED step (b_j+h) - (b_j-h) as stored, not the nominal
// 2h: those differ by a rounding when b_j is far from 0, and using the nominal
// step puts that rounding straight into every Jacobian entry.
void nl_jacobian(const NlResidFn& resid, const std::vector<double>& b, int64_t n, int k,
                 std::vector<double>* J);

// ---------------------------------------------------------------------------
// nl — nonlinear least squares
// ---------------------------------------------------------------------------

struct NlResult {
    std::vector<std::string> names;  // parameter names, in first-appearance order
    std::vector<double> beta, se;
    std::vector<double> vcov;  // k*k, column-major

    int64_t n = 0;
    int k = 0;

    // Stata's ANOVA block. `df_m`/`df_t` follow the constant-term convention:
    // with a constant the total is CENTRED (df_t = n-1, df_m = k-1); without
    // one it is uncentred (df_t = n, df_m = k).
    double rss = 0, tss = 0, mss = 0;
    double df_r = 0, df_m = 0, df_t = 0;
    double r2 = 0, r2_adj = 0, rmse = 0;
    double sigma2 = 0;  // rss / df_r — the multiplier on (J'J)^-1
    double res_dev = 0;  // Stata "Res. dev." = n(1 + ln 2pi + ln(rss/n)), i.e. -2 lnL

    // Index of the parameter whose derivative column is CONSTANT — Stata's
    // "Parameter b0 is used as a constant term during estimation." -1 when the
    // model has no such parameter, in which case tss above is UNCENTRED.
    int cons_param = -1;

    std::vector<NlIter> iters;
    bool converged = false, ok = false;
    std::string err;
};

// `y` is the dependent variable on the estimation sample (length n) — needed
// only for the total sum of squares, since the residual callback already
// carries it. `b0` is the vector of {name=init} starting values.
//
// `w` (optional, length n; BUG-1283): Stata's aweight/pweight normalized to mean 1
// (nl.ado normalizes them to the observation count). The fit minimises sum w e^2,
// rss/tss/mss are the weighted sums (tss centred about the weighted mean), the
// constant-term test reads the unweighted derivative, and Res. dev. subtracts n times
// the mean log weight (nl.ado e(dev)). `robust` (Stata's pweight VCE): the sandwich
// (J'WJ)^-1 sum (w e J)(w e J)' (J'WJ)^-1 * n/(n-k) in place of sigma2 (J'WJ)^-1.
// StataBE `nl (y = {b0} + {b1}*x1 + {b2}*x2) [aw=w]` / `[pw=w]` on probe BUG-1283.do.
NlResult nl_fit(const NlResidFn& resid, const double* y, int64_t n, const std::vector<double>& b0,
                const std::vector<std::string>& names, const double* w = nullptr,
                bool robust = false);

// ---------------------------------------------------------------------------
// gmm — single-equation GMM, one-step, unadjusted initial weight matrix
// ---------------------------------------------------------------------------

struct GmmResult {
    std::vector<std::string> names;
    std::vector<double> beta, se;
    std::vector<double> vcov;  // k*k, column-major

    int64_t n = 0;
    int k = 0;        // parameters
    int n_moments = 0;  // instrument columns
    double q = 0;     // GMM criterion at the optimum

    std::vector<NlIter> iters;
    bool converged = false, ok = false;
    std::string err;
};

// Z is col-major n*L (the instrument matrix INCLUDING whatever constant column
// the caller decided to add). The estimator minimises
//
//     Q(b) = g(b)' W g(b),   g(b) = (1/N) Z'e(b),   W = (Z'Z/N)^-1
//
// — Stata's `winitial(unadjusted)`, which is what `onestep` leaves in place.
//
// VCE is Stata `gmm`'s DEFAULT, which is robust:
//
//     V = (1/N) (G'WG)^-1 G'W S W G (G'WG)^-1,   S = (1/N) sum_i e_i^2 z_i z_i'
//
// with G = (1/N) Z' de/db'. There is NO small-sample factor: the oracle pair
// (gmm's .0182848 for b0 against nl's iid .0183731 on the same sample) is what
// calibrates that, and an N/(N-1) factor would put the SE at .0182868 — five
// digits in, and outside the wave's 1e-5 relative tolerance. Measured, not
// assumed; tests/unit/test_nlgmm.cpp asserts the unfactored value.
GmmResult gmm_fit(const NlResidFn& resid, const double* Z, int64_t n, int n_moments,
                  const std::vector<double>& b0, const std::vector<std::string>& names);

}  // namespace cecon
