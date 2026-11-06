// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Sharp regression-discontinuity local-polynomial estimation and density
// manipulation testing, implemented from the published methodology papers:
//
//   [CCT14] Calonico, S., M.D. Cattaneo and R. Titiunik (2014), "Robust
//           Nonparametric Confidence Intervals for Regression-Discontinuity
//           Designs", Econometrica 82(6), 2295-2326 — the bias-corrected
//           point estimate, the robust (bias-corrected-variance) confidence
//           interval, and the fixed-J nearest-neighbor variance estimator
//           (its Section 5), all in rd_robust_run below.  Appendix A.1/A.2
//           supplies the finite-sample matrix objects this file reproduces
//           verbatim: Gamma_{s,p}(h), vartheta_{s,p,q}(h), Psi_{s,p,q}(h,b).
//   [CCFT19] Calonico, S., M.D. Cattaneo, M.H. Farrell and R. Titiunik (2019),
//           "Regression Discontinuity Designs Using Covariates", Review of
//           Economics and Statistics 101(3), 442-451 — supplemental appendix
//           Part IV ("Implementation Details", Section 12) is the source for
//           the CURRENT mserd bandwidth chain (v-hat rule of thumb -> d-hat
//           -> b-hat -> h-hat) that superseded CCT14's own selector.  Its
//           Section 12.1-12.3 formulas are implemented in rdlocal.cpp.
//   [CCFT17] Calonico, S., M.D. Cattaneo, M.H. Farrell and R. Titiunik (2017),
//           "rdrobust: Software for regression-discontinuity designs", Stata
//           Journal 17(2), 372-404 — the printed-output contract these
//           commands reproduce (which cells exist and what they mean), and
//           the statement that MSE-optimal selectors carry an Imbens-
//           Kalyanaraman-style regularization term in the denominator.
//   [CJM20] Cattaneo, M.D., M. Jansson and X. Ma (2020), "Simple Local
//           Polynomial Density Estimators", Journal of the American
//           Statistical Association 115(531), 1449-1455 — the manipulation
//           (density-discontinuity) test in rd_density_run: a local
//           polynomial fitted to the empirical distribution function on each
//           side of the cutoff, with the estimator's own influence-function
//           (jackknife) variance.
//   [CCT15] Calonico, S., M.D. Cattaneo and R. Titiunik (2015), "Optimal
//           Data-Driven Regression Discontinuity Plots", Journal of the
//           American Statistical Association 110(512), 1753-1769 — the
//           evenly-spaced IMSE-optimal and mimicking-variance bin-count
//           selectors in rd_plot_run (its equations (1), (3) and (14)-(16)).
//
// Method source: the papers cited above. Conventions the papers leave open
// (mass-point handling, bandwidth truncation at the support boundary, bin
// construction) are pinned by this project's own committed goldens and unit
// tests.
//
// rd_robust_run is the CCT14 driver: local-polynomial point estimate (order
// p) either side of the cutoff, a bias estimate from an auxiliary local
// polynomial (order q > p), and three inferential rows (conventional,
// bias-corrected, and the robust bias-corrected-variance interval).
//
// rd_density_run is the CJM20 manipulation test: a local-polynomial density
// estimator fit on each side of the cutoff, tested for a discontinuity at the
// cutoff via a robust bias-corrected t-statistic, plus the exact two-sided
// binomial counts over a ladder of symmetric windows around the cutoff.
//
// rd_plot_run is the companion RD plot: binned local averages of y over x
// either side of the cutoff (for the scatter) plus a global polynomial fit
// (for the smoothed line), both as in CCT15.
//
// MASS POINTS.  The running variable of a real RD design is very often
// discrete (test scores, vote shares on a finite grid, ages in years), and
// rdrobust's default is masspoints(adjust).  Every routine here implements
// the ADJUSTED path: the nearest-neighbor variance grows its neighbour set by
// whole mass points until it holds at least J observations, so tied running
// values contribute their within-tie dispersion instead of an arbitrary
// tie-break, and the per-side count of DISTINCT running values is reported.
// With no ties the rule degenerates to the ordinary J-nearest-neighbor
// estimator, so continuous running variables are unaffected.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace cecon {

enum class RdKernel { Triangular, Uniform };

// masspoints handling (rdrobust's masspoints() option): Adjust corrects the
// effective-N/bandwidth-selection calculations for repeated running-variable
// values, Check only warns, Off does neither.  Only Adjust is implemented;
// the other two honest-reject (see rdlocal.cpp).
enum class RdMasspoints { Adjust, Check, Off };

// Shared config for all three entry points below. Fields not meaningful to a
// given entry point (e.g. nbins_left/right for rd_robust_run) are ignored by
// it.
struct RdLocalConfig {
    RdKernel kernel = RdKernel::Triangular;
    int p = 1;                 // main local-polynomial order
    int q = 2;                 // bias local-polynomial order (rd_robust_run only)
    // BUG-186: p/q used to double as their own "unset" sentinel (p==1&&q==2
    // meant "leave alone"), which collides with a caller explicitly asking
    // for p=1. has_p/has_q say whether the CLI actually saw --p/--q, so
    // rd_density_run/rd_plot_run can tell "1" from "absent".
    bool has_p = false;
    bool has_q = false;
    double h_left = 0;         // 0 = auto (CCFT19 mserd MSE-optimal)
    double h_right = 0;
    double b_left = 0;         // 0 = auto bias bandwidth
    double b_right = 0;
    double c = 0;              // cutoff (running variable >= c is "treated")
    RdMasspoints masspoints = RdMasspoints::Adjust;
    std::string vce = "nn";    // rdrobust default: nearest-neighbor variance
    int nnmatch = 3;           // J, the fixed number of nearest neighbours
    double level = 95.0;       // confidence level for the reported intervals

    // rdplot-only: 0 = automatic bin count each side (esmv: mimicking variance).
    int nbins_left = 0;
    int nbins_right = 0;
};

struct RdRobustResult {
    // Three inferential rows for the same underlying point estimate, as
    // printed by rdrobust: Conventional, Bias-corrected, Robust.
    double coef_conventional = 0, se_conventional = 0, z_conventional = 0,
           p_conventional = 0, ci_conventional_lo = 0, ci_conventional_hi = 0;
    double coef_bc = 0, se_bc = 0, z_bc = 0, p_bc = 0, ci_bc_lo = 0, ci_bc_hi = 0;
    double coef_robust = 0, se_robust = 0, z_robust = 0, p_robust = 0,
           ci_robust_lo = 0, ci_robust_hi = 0;

    double h_left = 0, h_right = 0;   // point-estimation bandwidths used
    double b_left = 0, b_right = 0;   // bias bandwidths used
    double rho = 0;                   // h/b ratio

    int64_t n_obs = 0;                // total N supplied (after listwise drop)
    int64_t n_left = 0, n_right = 0;          // raw obs each side of the cutoff
    int64_t n_eff_left = 0, n_eff_right = 0;  // effective (within-h) obs each side
    int64_t n_unique_left = 0, n_unique_right = 0;  // DISTINCT running values within h

    int p = 1, q = 2;
    std::string bw_type = "mserd";    // "mserd" when auto, "manual" when supplied
    std::string kernel = "Triangular";
    std::string vce = "NN";
    bool masspoints_detected = false;

    bool ok = true;
    std::string error;
};

// y[n], x[n] (the running variable), both length n. cfg.c is the cutoff.
// Rows with a non-finite y or x are dropped listwise before anything else.
RdRobustResult rd_robust_run(const double* y, const double* x, int64_t n,
                              const RdLocalConfig& cfg = {});

// One row of rddensity's binomial (H0: prob = .5) table.
struct RdDensityBinomRow {
    double window = 0;                // half-length of the symmetric window
    int64_t n_left = 0, n_right = 0;  // obs in [c-w, c) and [c, c+w]
    double p_value = 0;               // exact two-sided binomial test
};

struct RdDensityResult {
    double f_left = 0, f_right = 0;   // order-p density estimates either side
    double diff = 0;                  // f_right - f_left (order p)
    double se_conventional = 0, t_conventional = 0, p_conventional = 0;

    double f_left_q = 0, f_right_q = 0, diff_q = 0;
    double se_robust = 0, t_robust = 0, p_robust = 0;

    double h_left = 0, h_right = 0;
    int64_t n_obs = 0;
    int64_t n_left = 0, n_right = 0;
    int64_t n_eff_left = 0, n_eff_right = 0;

    int p = 2, q = 3;
    std::string bw_method = "comb";
    std::string kernel = "triangular";
    std::string vce = "jackknife";
    std::string model = "unrestricted";
    bool masspoints_detected = false;

    std::vector<RdDensityBinomRow> binomial;

    bool ok = true;
    std::string error;
};

// x[n]: the running variable only (no outcome — this is a manipulation test
// on the running-variable density itself).
RdDensityResult rd_density_run(const double* x, int64_t n,
                                const RdLocalConfig& cfg = {});

struct RdPlotResult {
    int bins_left = 0, bins_right = 0;              // scatter bin counts used
    int bins_imse_left = 0, bins_imse_right = 0;    // IMSE-optimal bin counts
    int bins_mv_left = 0, bins_mv_right = 0;        // mimicking-variance counts
    double scale_left = 1, scale_right = 1;         // requested bin scale
    double implied_scale_left = 0, implied_scale_right = 0;  // bins_used/bins_imse
    double wimse_var_weight = 0, wimse_bias_weight = 1;

    double avg_bin_len_left = 0, avg_bin_len_right = 0;
    double med_bin_len_left = 0, med_bin_len_right = 0;

    int poly_order_left = 0, poly_order_right = 0;  // global polynomial fit order
    double h_left = 0, h_right = 0;                 // polynomial-fit bandwidths

    int64_t n_obs = 0;
    int64_t n_left = 0, n_right = 0;
    int64_t n_eff_left = 0, n_eff_right = 0;
    // BUG-184: the same design facts rdrobust/rddensity already report. rdplot
    // computes them from the identical Split, on the identical running variable —
    // omitting them was what cost rdplot its "Mass points detected in the running
    // variable." preamble, which Stata prints and PIET was ready to print.
    int64_t n_unique_left = 0, n_unique_right = 0;  // DISTINCT running values, per side
    bool masspoints_detected = false;

    // Scatter: one entry per non-empty bin (mean of x and of y in the bin).
    std::vector<double> bin_x_left, bin_mean_y_left;
    std::vector<double> bin_x_right, bin_mean_y_right;
    // Smoothed line: the fitted polynomial sampled on a regular grid.
    std::vector<double> fit_x_left, fit_y_left;
    std::vector<double> fit_x_right, fit_y_right;
    // The fitted polynomial coefficients themselves (ascending powers of x-c).
    std::vector<double> coef_left, coef_right;

    std::string kernel = "Uniform";
    std::string binselect = "esmv";

    bool ok = true;
    std::string error;
};

RdPlotResult rd_plot_run(const double* y, const double* x, int64_t n,
                          const RdLocalConfig& cfg = {});

}  // namespace cecon
