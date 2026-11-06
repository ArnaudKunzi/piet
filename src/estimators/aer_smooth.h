// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Smoothers behind AER bundle H1: the Hodrick-Prescott filter, LOWESS, and
// local polynomial regression.
//
// All three are specified against StataNow 19.5 and were verified BEFORE they
// were written -- see internal note ERRATA-AER-H-SPECS-20260904.md. Where the manual
// and the shipped ado disagree, the ado wins and the divergence is named at the
// site; where the plan's transcription was wrong, the correction carries the
// measurement that forced it.
#pragma once
#include <string>
#include <vector>

namespace cecon {

// ---------------------------------------------------------------------------
// Hodrick-Prescott

struct HpResult {
    std::vector<double> cycle;
    std::vector<double> trend;
};

// tau = M^-1 y with M = I_T + lambda*K'K (K the (T-2)xT second-difference
// matrix), cycle = y - tau. M is pentadiagonal SPD, so this is an exact O(T)
// banded LDL' solve rather than a dense factorization.
//
// M IS ASSEMBLED GENERATIVELY, never from the manual's displayed band template
// (first/last diagonal 1+lambda, second/second-to-last 1+5*lambda, interior
// 1+6*lambda). At T=3 those patterns collide -- the middle entry is
// simultaneously "second" and "second-to-last" -- and the template gives
// 1+5*lambda where I+lambda*K'K gives 1+4*lambda. Measured with y=(1,5,2),
// lambda=1600: the template yields cycle = (1.494543, 4.995010, 1.494543) while
// the generative form and live StataBE both yield
// (-1.16654515154671, 2.33309030309343, -1.16654515154671) -- wrong by an order
// of magnitude AND a sign.
//
// Requires T >= 3; the caller refuses shorter samples (Stata errors rather than
// returning a zero cycle).
HpResult hp_filter(const std::vector<double>& y, double lambda);

// The Ravn-Uhlig rescaled default for a Stata time-unit format, or -1 when the
// format carries no rule (the caller then uses 1600).
//   %ty 6.25 · %th 100 · %tq 1600 · %tm 129,600 · %tw 33,177,600 (= 1600*12^4)
//   %td 110,930,628,906.25 (= 1600*(365/4)^4)
// %tw is 12^4 and NOT 13^4: [TS] tsfilter hp p.699 prints "1600 x 12^4" (byte-
// inspected to rule out a superscript artifact) and live r(smooth) returns
// 33177600. The plan's 45,697,600 would inflate lambda by 37.74% -- (13/12)^4 =
// 1.377363 -- silently changing every cycle value on weekly data.
double hp_lambda_for_format(const std::string& fmt);

// ---------------------------------------------------------------------------
// LOWESS

struct LowessOpts {
    double bwidth = 0.8;
    bool mean = false;      // running mean instead of a running line
    bool noweight = false;  // drop the tricube weights
};

// Cleveland's LOWESS as Stata's lowess computes it: tricube weights over a
// nearest-neighbour window, a weighted straight-line prediction at each point,
// and NO robustness iterations (as documented for lowess; adding even one Cleveland
// biweight pass moves the auto fit by 1.26 mpg).
//
// `x` must be ascending. Returns smoothed values in the same order.
//
// THE EFFECTIVE BANDWIDTH IS NOT THE REQUESTED ONE. Stata's lowess replaces a
// bandwidth that is <= 0 or >= 1 by 0.8, so any bwidth outside the OPEN interval (0,1) is silently replaced by 0.8 --
// no warning, no error. `effective_bwidth` exposes that rule because the value
// is observable: the graph note prints the substituted bandwidth, and a caller
// pinning `bwidth(2)` sees ".8". Measured N=3: bwidth 1, 0.8 and 2 all give the
// identity (k=0) and only 0.99 gives the 3-wide moving average.
double lowess_effective_bwidth(double requested);

// Degenerate input is all-missing with rc 0, not an error: Stata's lowess skips
// any by-group with fewer than 3 points or an x range under 1e-30.
std::vector<double> lowess_fit(const std::vector<double>& x, const std::vector<double>& y,
                               const LowessOpts& o);

// ---------------------------------------------------------------------------
// Local polynomial

enum class LpolyKernel {
    Epanechnikov,  // Stata's DEFAULT, the sqrt(5)-rescaled one
    Epan2,
    Biweight,
    Cosine,
    Gaussian,
    Parzen,
    Rectangle,
    Triangle,
};

bool lpoly_kernel_from_name(const std::string& name, LpolyKernel* out);
const char* lpoly_kernel_name(LpolyKernel k);

struct LpolyResult {
    std::vector<double> grid;
    std::vector<double> smooth;
    std::vector<double> se;  // empty unless requested
    double bwidth = 0;
    double pwidth = 0;
    int ngrid = 0;  // grid points where the local fit SUCCEEDED, which can be < grid.size()
    bool n_capped = false;  // n() exceeded n_cap and was set to it ("(n() set to #)")
};

// Rule-of-thumb bandwidth, formula (3) of [R] lpoly p.1615, with three
// corrections the plan's transcription needed:
//
//   1. sigma^2 = RSS/n, not RSS/(n-p-4). The manual names no divisor; live
//      back-solving pins n (with RSS/n the implied kernel constants reproduce
//      their exact values to 2e-8..6.3e-6; with RSS/(n-1) every one is off by
//      2.4e-3, and with RSS/(n-p-4) by 1.38e-2).
//   2. the denominator is n times a CONTINUOUS INTEGRAL of the squared
//      (p+1)th derivative over the w0 interval -- not a sum over observations,
//      and with no design-density factor. On auto.dta at p=1 the plan's sum is
//      2.020365e6 against Stata's effective 74 x 5.750245e5 = 4.2552e7, a factor
//      of 21.06, which inflates h by 86.5%.
//   3. Stata's DEFAULT epanechnikov divides the constant by sqrt(5); the plan
//      multiplied. Manual examples 1 and 3 give bandwidth 3.08 and 6.88 on the
//      same data (ratio 2.23377 ~ sqrt(5)), and live h_epan2/h_epan = 2.23632.
//
// The global polynomial is fitted on raw powers of x with invsym's column
// drops, as Stata fits it, so r(bwidth) moves with a shift of x the way
// Stata's does (aer_smooth.cpp rot_poly_fit, on the rows in stable x order,
// as lpoly sorts them).
//
// `w` (optional, same length as x) are the observation weights as Stata's lpoly
// uses them: [aw=] already rescaled to w*N/sum(w).
// They enter the global polynomial as WLS weights and sigma^2 = sum(w e^2)/n;
// measured on the w2ae_lpoly_lowess capture (lpl_aw r(bwidth), 1.6e-12; an
// unweighted sigma^2 is 9.5e-3 off).
//
// Returns -1 when no rule-of-thumb constant is pinned for this (kernel, degree).
double lpoly_rot_bandwidth(const std::vector<double>& x, const std::vector<double>& y,
                           LpolyKernel k, int degree, const std::vector<double>* w = nullptr);

// C_{0,p}(K), the kernel constant of the rule of thumb, or -1 when unpinned.
double lpoly_rot_constant(LpolyKernel k, int p);

struct LpolyOpts {
    LpolyKernel kernel = LpolyKernel::Epanechnikov;
    int degree = 0;
    double bwidth = -1;  // <0 = use the rule of thumb
    double pwidth = -1;  // <0 = 1.5 * the rule-of-thumb bandwidth (BUG-417)
    int n = -1;          // <=1 = 50
    // Stata's lpoly caps n() at _N, the DATASET's row count, not the
    // sample's: `if n > _N, n = _N` runs before any `if` is applied. <=0 = no
    // cap (the caller does not know _N).
    int n_cap = -1;
    bool want_se = false;
    // Observation weights, same length as x (nullptr = unweighted); see
    // lpoly_rot_bandwidth for the scaling the caller applies first.
    const std::vector<double>* weights = nullptr;
    // at(): evaluate at these points instead of the n-point grid. NaN entries
    // are not evaluated and stay missing; the result vectors keep this length.
    const std::vector<double>* at = nullptr;
};

// Empty result (grid empty) when the bandwidth could not be determined; the
// caller turns that into a named refusal.
//
// Standard errors (want_se), as Stata's lpoly computes them -- verified on the
// w2ae_lpoly_lowess capture (lpl_se_at, lpl_ci_se) before this was written:
//   Var(m(x0)) = e1'(X'WX)^-1 (X'W^2X) (X'WX)^-1 e1 * sigma^2(x0),
// where sigma^2(x0) is the normalized weighted RSS of a degree p+2 local fit at
// the pilot bandwidth: sum K* r^2 / (tr W* - tr((X*'W*X*)^-1 X*'W*^2X*)). The
// plain kernel-weighted mean of r^2 is 2.4% off lpl_se_at.
LpolyResult lpoly_fit(const std::vector<double>& x, const std::vector<double>& y,
                      const LpolyOpts& o);

}  // namespace cecon
