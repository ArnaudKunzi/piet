// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// cecon — `xtabond`: Arellano–Bond linear dynamic panel-data estimation
// (one-step difference GMM), Stata-compatible.
//
// THE MODEL
//
//     y_it = SUM_{j=1..p} alpha_j y_{i,t-j} + x_it' beta + nu_i + eps_it
//
// The unit effect nu_i is swept out by first-differencing
//
//     D.y_it = SUM_j alpha_j D.y_{i,t-j} + D.x_it' beta + D.eps_it
//
// and the differenced equation is fitted by GMM on the Arellano–Bond moment
// set: deep lags of y (levels) instrument the differenced equation, the
// strictly exogenous covariates instrument themselves in differences.
//
// The GMM-type set is L(2/.).y for EVERY lag order p, not L(p+1/.).y: D.eps_t
// is MA(1) in eps whatever p is, so y_{t-2} is a valid instrument regardless.
// Only the estimation sample moves with p. Measured against Stata, which
// prints "GMM-type: L(2/.).y" under lags(2) as well as lags(1).
//
// THE CONSTANT. `_cons` cannot be identified from the differenced equation (it
// differences away), so — exactly as Stata does — the system is augmented with
// a LEVEL equation instrumented only by `_cons`. Because the weight matrix H is
// BLOCK-diagonal between the differenced and the level rows (see below), the
// level block adds one moment and one parameter: the slopes are algebraically
// unchanged and `_cons` is pinned by the level moment alone. That is what the
// Stata manual means by "Including the constant does not affect the other
// parameter estimates because it is identified only by the level errors"
// ([XT] xtabond, example 5) — and it is asserted, not assumed, in
// tests/unit/test_xtabond.cpp.
//
// THE ARITHMETIC (from [XT] xtdpd, Methods and formulas — xtabond delegates to
// xtdpd, so xtdpd's formulas ARE xtabond's):
//
//     Q_xz = SUM_i X_i' Z_i        Q_zy = SUM_i Z_i' y_i
//     A1   = (SUM_i Z_i' H_i Z_i)^-1
//     W1   = Q_xz A1 Q_xz'
//     beta = W1^-1 Q_xz A1 Q_zy
//
//     H_i  = [ H_d   0  ]   H_d = tridiagonal(-.5, 1, -.5) over PERIOD-adjacent
//            [  0   H_L ]         sample rows;  H_L = .5 * I
//
// H_d's off-diagonal follows cov(D.eps_t, D.eps_{t-1}) = -sigma^2, so the -.5
// entry belongs to a pair of rows one PERIOD apart — never merely to a pair of
// rows adjacent in the (gap-ridden) sample. On the P7 fixture the two readings
// are wildly different (L1.y .7018551 vs .7353502); the period reading is the
// one that reproduces Stata.
//
// The one-step robust VCE is the cluster-on-panel sandwich
//
//     V = M (SUM_i Z_i' u_i u_i' Z_i) M'      M = W1^-1 Q_xz A1
//
// with u_i the stacked (differenced, level) one-step residuals and NO
// finite-sample multiplier. There is no Windmeijer correction here — that is a
// two-step object, and two-step estimation is not implemented; the command
// rejects any VCE it cannot compute rather than dressing one-step numbers up
// under another name.
//
// Every number is pinned against StataBE 19.5 on the P7 pangram wave
// (internal capture p7_stata.log, `xtset id t` then
// `xtabond y L.y x1 x2, lags(1) vce(robust)`); the assertions live in
// tests/unit/test_xtabond.cpp.
#pragma once
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace cecon {

struct XtabondSpec {
    int lags = 1;          // lags(p): the p autoregressive terms the command creates
    double delta = 1.0;    // xtset delta — one period
    bool constant = true;  // estimate `_cons` off the level equation
};

struct XtabondResult {
    bool ok = false;
    std::string err;

    // --- coefficients, in Stata's print order: L1.y..Lp.y, the surviving
    // covariates in typed order, then `_cons` (when requested).
    std::vector<std::string> names;
    std::vector<double> beta, se;
    std::vector<double> vcov;  // k*k, row-major (symmetric) — per the vce chosen at fit
    // BUG-383 #5: Stata's bare `xtabond` is the one-step CONVENTIONAL VCE;
    // vce(robust) is opt-in. rss = the differenced residual sum of squares
    // (Stata e(rss)); sig2 = rss / (2(N-k)) (e(sig2) — the /2 maps the
    // differenced-residual variance back to the level-residual one). Backed out
    // of the live capture exactly: rss/sig2 == 2(N-k) == 474 on the fixture.
    double rss = 0.0;
    double sig2 = 0.0;
    int k = 0;

    // Regressors dropped because they were collinear with an earlier column of
    // the DIFFERENCED design — typically an explicitly typed `L.y` duplicating
    // the lag that lags() already created. A dropped regressor keeps its
    // STANDARD INSTRUMENT column: it is excluded from the model, not from the
    // moment set (which is why the oracle's instrument count is 257, not 256).
    std::vector<std::string> omitted;

    // --- sample ------------------------------------------------------------
    int64_t n_obs = 0;        // rows of the differenced (estimation) equation
    int n_groups = 0;         // panels contributing at least one differenced row
    int min_obs = 0;          // obs per group, min / avg / max (Stata's header)
    int max_obs = 0;
    double avg_obs = 0.0;
    int64_t n_level_obs = 0;  // rows of the level equation (a SUPERSET: it needs
                              // one fewer lag of y and no lagged covariates)

    // Consecutive differenced-sample rows within a panel whose periods are NOT
    // one delta apart. > 0 means the panel has interior gaps, so the two
    // readings of H_d's off-diagonal (period-adjacent vs row-adjacent) are
    // observationally distinguishable on this data.
    int64_t n_period_gaps = 0;

    // --- instruments -------------------------------------------------------
    int n_instruments = 0;
    int n_gmm_instruments = 0;    // the block-diagonal L(2/.).y set (depth 2 for every p)
    int n_std_instruments = 0;    // the typed covariates, in differences
    int n_level_instruments = 0;  // `_cons`, 0 or 1
    std::string gmm_label;                 // "L(2/.).y"
    std::vector<std::string> std_labels;   // "LD.y", "D.x1", "D.x2"

    // --- Wald chi2 over the slopes (Stata's header excludes `_cons`) --------
    double chi2 = std::numeric_limits<double>::quiet_NaN();
    int chi2_df = 0;
    double p_chi2 = std::numeric_limits<double>::quiet_NaN();

    // --- moment diagnostics ------------------------------------------------
    // zu = SUM_i Z_i' u_i, the m sample moments at the solution (exactly zero
    // only when the system is just identified). foc = Q_xz A1 zu, the k GMM
    // first-order conditions, which are zero at ANY solution — the oracle-free
    // check that the linear algebra actually solved the problem it claims to.
    std::vector<double> zu;
    std::vector<double> foc;
};

// id/time/y are length-n columns in the caller's row order; X is column-major
// n*kx and holds the typed covariates IN LEVELS (already factor/TS expanded by
// the command layer — this routine does its own differencing and lagging by
// TIME VALUE, never by row position). Rows with a missing id or time are
// dropped. A repeated (id, time) pair is an error: the panel would not be
// uniquely keyed and every lag would be ambiguous.
//
// rowmask (optional, length n) is Stata's `if` qualifier: a 0 row is excluded
// from BOTH estimation samples but stays fully visible as a lag source and as
// an instrument value — exactly as a time-series operator behaves under `if`.
// Filtering such rows out of the data instead would silently punch holes in
// every panel and move every lag.
XtabondResult xtabond_fit(const double* id, const double* time, const double* y, const double* X,
                          int kx, const std::vector<std::string>& xnames, int64_t n,
                          const XtabondSpec& spec, const char* rowmask = nullptr,
                          bool conventional_vce = true);

}  // namespace cecon
