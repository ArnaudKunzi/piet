// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// AER bundle H1 — the four smoothers/decompositions, pinned against StataBE
// 19.5 oracles and against the specific defects the pre-implementation spec
// review found (internal note ERRATA-AER-H-SPECS-20260904.md).
//
// Every numeric pin below is an ORACLE value. Loosening a tolerance here is not
// a fix: these specs were verified BEFORE the code was written precisely so a
// golden could not be captured from the same transcription as the thing it is
// supposed to check.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "core/error.h"
#include "estimators/aer_smooth.h"
#include "estimators/pca.h"

using namespace cecon;

namespace {

int failures = 0;

void check(bool ok, const std::string& label) {
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", label.c_str());
    if (!ok) ++failures;
}

bool close_rel(double a, double b, double rtol) {
    if (std::isnan(a) || std::isnan(b)) return false;
    return std::fabs(a - b) <= rtol * std::max(1.0, std::fabs(b));
}

// ---------------------------------------------------------------------------

void test_hp_t3_generative() {
    std::printf("test_aer_estim / hp T=3 generative assembly:\n");
    // THE defect the manual's displayed band template produces. At T=3 the
    // "second" and "second-to-last" patterns collide: the template puts
    // 1+5*lambda in the middle where I+lambda*K'K puts 1+4*lambda. The template
    // gives cycle = (1.494543, 4.995010, 1.494543); live StataBE gives the
    // values below -- a different order of magnitude AND a different sign.
    const std::vector<double> y{1.0, 5.0, 2.0};
    HpResult r = hp_filter(y, 1600.0);
    check(close_rel(r.cycle[0], -1.16654515154671, 1e-12), "cycle[0] == -1.16654515154671");
    check(close_rel(r.cycle[1], 2.33309030309343, 1e-12), "cycle[1] == 2.33309030309343");
    check(close_rel(r.cycle[2], -1.16654515154671, 1e-12), "cycle[2] == -1.16654515154671");
    // Symmetric input, near-symmetric answer -- an assembly that gets the middle
    // row wrong breaks this badly. RELATIVE, not exact: a banded LDL' is not
    // bit-symmetric because the forward and backward substitutions visit the
    // ends in opposite orders. Measured residual 1.52e-13 relative, so 1e-12
    // carries ~6x headroom. This is the same shape mistake the errata caught in
    // the plan's "cycle == 0 exactly" assertion, made once more here and fixed
    // by measuring instead of guessing.
    check(std::fabs(r.cycle[0] - r.cycle[2]) <= 1e-12 * std::fabs(r.cycle[0]),
          "cycle is symmetric for symmetric y (relative, not bitwise)");
}

void test_hp_oracle_series() {
    std::printf("test_aer_estim / hp vs StataBE on the plan's series:\n");
    // set obs 60; gen t=_n; gen double y = 100 + .5*t + 3*sin(t/3) + .01*t^2
    std::vector<double> y;
    for (int i = 1; i <= 60; ++i) {
        const double t = static_cast<double>(i);
        y.push_back(100.0 + 0.5 * t + 3.0 * std::sin(t / 3.0) + 0.01 * t * t);
    }
    HpResult r = hp_filter(y, 1600.0);
    check(close_rel(r.cycle[0], -2.41008177095163e-01, 1e-9), "cycle[0] oracle (rtol 1e-9)");
    check(close_rel(r.cycle[29], -1.52377459864197e+00, 1e-9), "cycle[29] oracle (rtol 1e-9)");
    // trend + cycle must reconstruct y exactly, whatever the solve did.
    double worst = 0;
    for (std::size_t i = 0; i < y.size(); ++i)
        worst = std::max(worst, std::fabs(r.trend[i] + r.cycle[i] - y[i]));
    check(worst < 1e-9, "trend + cycle reconstructs y");
}

void test_hp_near_zero_cycle() {
    std::printf("test_aer_estim / hp constant and linear:\n");
    // The plan asserted cycle == 0 EXACTLY here. That is true of Stata and NOT
    // achievable by a banded LDL' in doubles: measured max|cycle| = 1.23e-07 for
    // a linear series of magnitude 1e6. The honest assertion is the near-zero
    // floor from TOLERANCE sec.3 rule 2 -- atol = rtol_tier x a NAMED scale --
    // and an EXPECT_EQ or a bare atol would fail on realistic magnitudes.
    std::vector<double> flat(60, 42.0), lin(60, 0.0);
    for (int i = 0; i < 60; ++i) lin[static_cast<std::size_t>(i)] = 1000000.0 + 1234.5 * i;
    for (const auto* pv : {&flat, &lin}) {
        HpResult r = hp_filter(*pv, 1600.0);
        double maxy = 0, maxc = 0;
        for (std::size_t i = 0; i < pv->size(); ++i) {
            maxy = std::max(maxy, std::fabs((*pv)[i]));
            maxc = std::max(maxc, std::fabs(r.cycle[i]));
        }
        check(maxc <= 1e-12 * maxy, "|cycle| <= 1e-12 * max|y| (NOT exact zero)");
    }
}

void test_hp_lambda_defaults() {
    std::printf("test_aer_estim / hp lambda per time-unit format:\n");
    // %tw is 12^4, not 13^4. The plan's 45,697,600 inflates lambda by 37.74%
    // ((13/12)^4 = 1.377363) and silently changes every weekly cycle value.
    check(hp_lambda_for_format("%tw") == 33177600.0, "%tw -> 33,177,600 (= 1600 * 12^4)");
    check(hp_lambda_for_format("%td") == 110930628906.25, "%td -> 110,930,628,906.25");
    check(hp_lambda_for_format("%tq") == 1600.0, "%tq -> 1600");
    check(hp_lambda_for_format("%tm") == 129600.0, "%tm -> 129,600");
    check(hp_lambda_for_format("%ty") == 6.25, "%ty -> 6.25");
    check(hp_lambda_for_format("%th") == 100.0, "%th -> 100");
    check(hp_lambda_for_format("%9.0g") < 0, "a non-time format has no rule");
    // Real formats carry width/detail after the letters.
    check(hp_lambda_for_format("%tqCCYY!qQ") == 1600.0, "%tq with a detail suffix still matches");
}

void test_lowess_bwidth_substitution() {
    std::printf("test_aer_estim / lowess effective bandwidth:\n");
    // Stata's lowess replaces a bandwidth <= 0 or >= 1 by .8. Silent, no warning.
    check(lowess_effective_bwidth(1.0) == 0.8, "bwidth(1) -> 0.8");
    check(lowess_effective_bwidth(2.0) == 0.8, "bwidth(2) -> 0.8");
    check(lowess_effective_bwidth(0.0) == 0.8, "bwidth(0) -> 0.8");
    check(lowess_effective_bwidth(-1.0) == 0.8, "a negative bwidth -> 0.8");
    check(lowess_effective_bwidth(0.99) == 0.99, "bwidth(0.99) is kept");

    // The consequence the plan's own ctest got backwards: at N=3, bwidth(1) is
    // the IDENTITY (k = floor((3*0.8 - 0.5)/2) = 0), and only a value strictly
    // inside (0,1) gives the 3-wide moving average.
    const std::vector<double> x{1, 2, 3}, y{1, 4, 9};
    LowessOpts o;
    o.mean = true;
    o.noweight = true;
    o.bwidth = 1.0;
    std::vector<double> ident = lowess_fit(x, y, o);
    check(ident[0] == 1 && ident[1] == 4 && ident[2] == 9, "bwidth(1) is the identity, not a mean");
    o.bwidth = 0.99;
    std::vector<double> ma = lowess_fit(x, y, o);
    check(close_rel(ma[0], 2.5, 1e-12) && close_rel(ma[1], 14.0 / 3.0, 1e-12) &&
              close_rel(ma[2], 6.5, 1e-12),
          "bwidth(0.99) is the 3-wide moving average (2.5, 4.6667, 6.5)");
}

void test_lowess_degenerate() {
    std::printf("test_aer_estim / lowess degenerate input:\n");
    // Stata's lowess skips a by-group with fewer than 3 points or an x range under 1e-30
    // -- a short or constant-x group is left ALL MISSING with rc 0, not an error.
    LowessOpts o;
    const std::vector<double> x2{1, 2}, y2{3, 4};
    std::vector<double> r2 = lowess_fit(x2, y2, o);
    check(r2.size() == 2 && std::isnan(r2[0]) && std::isnan(r2[1]), "n=2 -> all missing");
    const std::vector<double> xc{5, 5, 5}, yc{1, 2, 3};
    std::vector<double> rc = lowess_fit(xc, yc, o);
    check(std::isnan(rc[0]) && std::isnan(rc[2]), "constant x -> all missing");
}

void test_lpoly_constants_and_rot() {
    std::printf("test_aer_estim / lpoly rule-of-thumb:\n");
    // A degree with no pinned constant refuses (-1) rather than inventing one.
    const std::vector<double> x{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    const std::vector<double> y{1, 3, 2, 5, 4, 6, 8, 7, 9, 11, 10, 12};
    check(lpoly_rot_bandwidth(x, y, LpolyKernel::Gaussian, 7) < 0,
          "degree 7 has no pinned constant -> -1 (refuse, do not invent)");
    // Even degrees are promoted to p+1 before the constant is looked up, so
    // degree 0 (Stata's default) and degree 1 must give the SAME bandwidth.
    const double h0 = lpoly_rot_bandwidth(x, y, LpolyKernel::Epanechnikov, 0);
    const double h1 = lpoly_rot_bandwidth(x, y, LpolyKernel::Epanechnikov, 1);
    check(h0 > 0 && close_rel(h0, h1, 1e-15), "degree 0 is promoted to p=1");
}

void test_lpoly_kernels_normalize() {
    std::printf("test_aer_estim / lpoly kernels:\n");
    // Every kernel must integrate to ~1 over its support; a dropped scale factor
    // is invisible in the smooth (the weights renormalize) but corrupts the ROT.
    LpolyOpts o;
    const char* names[] = {"epanechnikov", "epan2",  "biweight",  "cosine",
                           "gaussian",     "parzen", "rectangle", "triangle"};
    for (const char* nm : names) {
        LpolyKernel k;
        check(lpoly_kernel_from_name(nm, &k), std::string("kernel name '") + nm + "' resolves");
        check(std::string(lpoly_kernel_name(k)) == nm, std::string("...and round-trips: ") + nm);
    }
    LpolyKernel bad;
    check(!lpoly_kernel_from_name("nosuch", &bad), "an unknown kernel name is rejected");
    (void)o;
}

// ---------------------------------------------------------------------------
// w2ae_lpoly_lowess (goal gfx-lpoly-lowess, 2026-10-15). Every pin below is a
// %21x value from internal capture
// w2ae_lpoly_lowess.log (the arm id is named at each check), on the case's own
// 120-row fixture, rebuilt here exactly as the do-file generates it.

double rel_err(double a, double b) { return std::fabs(a - b) / std::fabs(b); }

struct W2aeFixture {
    std::vector<double> x, y, w, xat, xat2;
    W2aeFixture() {
        for (int n = 1; n <= 120; ++n) {
            const int grp = n % 2, g = (n - 1) % 4 + 1;
            const double xx = std::floor(((n * 37) % 101) / 4.0) / 2.0 + grp;
            const double z = ((n * 13) % 17) / 3.0;
            x.push_back(xx);
            y.push_back(1 + 0.5 * xx + 0.3 * z + g / 2.0 + ((n * 7) % 11) / 5.0 - grp * xx / 4.0);
            w.push_back(1 + n % 3);
        }
        xat.assign(120, std::nan(""));
        for (int i = 0; i < 5; ++i) xat[static_cast<std::size_t>(i)] = x[static_cast<std::size_t>(i)];
        xat2.assign(120, std::nan(""));
        xat2[0] = 3;
        xat2[1] = 7.5;
        xat2[3] = 12;
        xat2[4] = 7.5;
        xat2[5] = 100;
    }
};

void test_lpoly_rot_trapezoid_constants() {
    std::printf("test_aer_estim / lpoly ROT constants = Stata's trapezoid C_{0,p}(K):\n");
    // FU-902: the 6-digit constants put r(bwidth) 7.1e-8 above Stata (2.5e-7 at
    // p=3, 3.1e-7 gaussian), outside T2. Back-outs from the probe_rot_constants
    // capture (two fixtures each, agreeing to 2.7e-12 at p=1).
    check(rel_err(lpoly_rot_constant(LpolyKernel::Epanechnikov, 1), 0.7688119451950) < 1e-11,
          "epanechnikov p=1 = 0.76881194519 (101-point trapezoid on [-sqrt5,sqrt5])");
    check(rel_err(lpoly_rot_constant(LpolyKernel::Gaussian, 1), 0.7763932398192) < 1e-11,
          "gaussian p=1 = 0.77639323982 (101 points on [-5,5])");
    check(rel_err(lpoly_rot_constant(LpolyKernel::Cosine, 1), 4.2613254616173) < 1e-11,
          "cosine p=1 = 4.26132546162 (81 points on [-1,1])");
    check(rel_err(lpoly_rot_constant(LpolyKernel::Rectangle, 3), 2.8495987164854) < 1e-9,
          "rectangle p=3 = 2.84959871649 (81 points on [-1,1])");
    const W2aeFixture f;
    // lpl_ctl r(bwidth) +1.db10f29beda63X-001; the old constant gave +1.db10f4d416dd7X-001.
    const double h = lpoly_rot_bandwidth(f.x, f.y, LpolyKernel::Epanechnikov, 0);
    check(rel_err(h, 0x1.db10f29beda63p-1) < 1e-9, "lpl_ctl ROT bandwidth within T2 of Stata");
    // lpl_ci_se degree(2): p=3, r(bwidth) +1.1acbfaa004c39X+000.
    const double h3 = lpoly_rot_bandwidth(f.x, f.y, LpolyKernel::Epanechnikov, 2);
    check(rel_err(h3, 0x1.1acbfaa004c39p+0) < 1e-9, "lpl_ci_se p=3 ROT bandwidth within T2");
    // lpl_dkn kernel(gaussian) degree(1): +1.dfc037ffb7003X-001.
    const double hg = lpoly_rot_bandwidth(f.x, f.y, LpolyKernel::Gaussian, 1);
    check(rel_err(hg, 0x1.dfc037ffb7003p-1) < 1e-9, "lpl_dkn gaussian ROT bandwidth within T2");
}

void test_lpoly_rot_raw_power_fit() {
    std::printf("test_aer_estim / lpoly ROT global fit on raw powers, invsym drops:\n");
    // FU-905. Pins from internal capture
    // probe_rot_conditioning.log, same 120-row fixture. Stata fits the degree p+3
    // polynomial on raw powers of x, so r(bwidth) is not translation invariant;
    // the centred QR fit this replaced was 3.4% off on cond_shift100_d0 and 54%
    // off on cond_scale0.001_d2.
    const W2aeFixture f;
    std::vector<double> xs, xk;
    for (double v : f.x) {
        xs.push_back(v + 100);
        xk.push_back(v * 0.001);
    }
    // cond_shift100_d0 +1.ea34fcbb2759bX-001: the exact fit WITHOUT the constant.
    check(rel_err(lpoly_rot_bandwidth(xs, f.y, LpolyKernel::Epanechnikov, 0), 0x1.ea34fcbb2759bp-1) < 1e-6,
          "cond_shift100_d0: x+100 drops the constant column as Stata does");
    // cond_scale0.001_d2 +1.50d3f8c7245e8X-008: the exact fit without x^5 and x^6.
    check(rel_err(lpoly_rot_bandwidth(xk, f.y, LpolyKernel::Epanechnikov, 2), 0x1.50d3f8c7245e8p-8) < 1e-9,
          "cond_scale0.001_d2: x*0.001 drops x^5 and x^6 as Stata does");
    // lpl_ci_se_if / cond_if_shift0 +1.eee81eb9ddad3X-001 (grp == 1: odd rows).
    std::vector<double> xi, yi;
    for (std::size_t i = 0; i < f.x.size(); i += 2) {
        xi.push_back(f.x[i]);
        yi.push_back(f.y[i]);
    }
    check(rel_err(lpoly_rot_bandwidth(xi, yi, LpolyKernel::Epanechnikov, 2), 0x1.eee81eb9ddad3p-1) < 1e-9,
          "lpl_ci_se_if p=3 ROT bandwidth within T2 (QR gave 1.11e-9)");
}

void test_lpoly_at_points() {
    std::printf("test_aer_estim / lpoly at() evaluates at the given points:\n");
    const W2aeFixture f;
    LpolyOpts o;
    o.at = &f.xat;
    const LpolyResult r = lpoly_fit(f.x, f.y, o);
    // lpl_at: r(ngrid) 5, s[1] = s[4] (x = 5.5 twice) +1.b3a0199bbe118X+002, s[6] missing.
    check(r.ngrid == 5, "lpl_at r(ngrid) = 5 at points, not the 50-point grid");
    check(r.smooth.size() == 120 && rel_err(r.smooth[0], 0x1.b3a0199bbe118p+2) < 1e-9 &&
              r.smooth[3] == r.smooth[0] && rel_err(r.smooth[4], 0x1.fde5676bd433bp+2) < 1e-9,
          "lpl_at_o1/o4/o5 smooth at the at values");
    check(std::isnan(r.smooth[5]) && std::isnan(r.smooth[119]), "lpl_at_o6: rows without an at value stay missing");
    o.at = &f.xat2;
    const LpolyResult r2 = lpoly_fit(f.x, f.y, o);
    // lpl_at_miss: 4 successful points; at = 100 has no data in its window.
    check(r2.ngrid == 4 && std::isnan(r2.smooth[2]) && std::isnan(r2.smooth[5]) &&
              rel_err(r2.smooth[3], 0x1.fda2bc2de65f9p+2) < 1e-9,
          "lpl_at_miss: r(ngrid) 4, missing and out-of-data points stay missing");
}

void test_lpoly_aweight_and_se() {
    std::printf("test_aer_estim / lpoly [aw=] and se():\n");
    const W2aeFixture f;
    std::vector<double> wn;
    double sw = 0;
    for (double v : f.w) sw += v;
    for (double v : f.w) wn.push_back(v * 120.0 / sw);
    LpolyOpts o;
    o.weights = &wn;
    const LpolyResult r = lpoly_fit(f.x, f.y, o);
    // lpl_aw: r(bwidth) +1.dc86d96f02f92X-001 (unweighted +1.db10f29beda63X-001), o1 smooth.
    check(rel_err(r.bwidth, 0x1.dc86d96f02f92p-1) < 1e-9, "lpl_aw weighted ROT bandwidth");
    check(rel_err(r.smooth[0], 0x1.f728dae9b0061p+1) < 1e-9, "lpl_aw_o1 weighted smooth");
    LpolyOpts s;
    s.at = &f.xat;
    s.want_se = true;
    const LpolyResult rs = lpoly_fit(f.x, f.y, s);
    // lpl_se_at: se o1 +1.b9867e18e25bdX-003, o3 +1.c8cc509815480X-003; r(pwidth) +1.644cb5f4f23caX+000.
    check(rs.se.size() == 120 && rel_err(rs.se[0], 0x1.b9867e18e25bdp-3) < 1e-9 &&
              rel_err(rs.se[2], 0x1.c8cc509815480p-3) < 1e-9 && std::isnan(rs.se[5]),
          "lpl_se_at standard errors at the at points");
    check(rel_err(rs.pwidth, 0x1.644cb5f4f23cap+0) < 1e-9, "lpl_se_at pilot bandwidth");
    LpolyOpts d2;
    d2.degree = 2;
    d2.want_se = true;
    const LpolyResult r2 = lpoly_fit(f.x, f.y, d2);
    // lpl_ci_se degree(2): se o1 +1.0f3bf670f1df5X+000, o50 +1.115a206f00684X+000.
    check(rel_err(r2.se[0], 0x1.0f3bf670f1df5p+0) < 1e-9 && rel_err(r2.se[49], 0x1.115a206f00684p+0) < 1e-9,
          "lpl_ci_se degree(2) standard errors");
}

void test_lpoly_grid_n() {
    std::printf("test_aer_estim / lpoly n() follows Stata's rule:\n");
    const W2aeFixture f;
    LpolyOpts o;
    o.bwidth = 1.5;
    o.n = 500;
    o.n_cap = 120;
    const LpolyResult r = lpoly_fit(f.x, f.y, o);
    check(r.grid.size() == 120 && r.n_capped, "n() above _N is set to _N");
    LpolyOpts o1;
    o1.bwidth = 1.5;
    o1.n = 1;
    check(lpoly_fit(f.x, f.y, o1).grid.size() == 50, "n(1) means 50");
}

// ---------------------------------------------------------------------------
// pca's measured cutoffs (StataBE 19.5 probe).
// Eight rows of mutually orthogonal, mean-zero +/-s columns make C exactly diagonal, so
// each eigenvalue is the chosen variance and lands on the side of a cutoff it was chosen for.

struct PcaColumns {
    std::vector<std::vector<double>> data;
    std::vector<const double*> cols;
    std::vector<std::string> names;
    std::vector<char> keep = std::vector<char>(8, 1);
};

PcaColumns orthogonal_columns(const std::vector<double>& variances) {
    PcaColumns out;
    out.data.reserve(variances.size());
    for (size_t j = 0; j < variances.size(); ++j) {
        const double s = std::sqrt(variances[j] * 7.0 / 8.0);
        std::vector<double> col(8);
        for (size_t i = 0; i < 8; ++i) col[i] = ((i >> j) & 1U) ? s : -s;
        out.data.push_back(col);
        out.names.push_back("v" + std::to_string(j + 1));
    }
    for (const auto& col : out.data) out.cols.push_back(col.data());
    return out;
}

PcaResult pca_on_variances(const std::vector<double>& variances, int components = -1) {
    const PcaColumns c = orthogonal_columns(variances);
    PcaOpts o;
    o.covariance = true;
    o.components = components;
    return pca_fit(c.names, c.cols, c.keep, o);
}

std::string pca_refusal(const std::vector<const double*>& cols, const std::vector<char>& keep) {
    const std::vector<std::string> names(cols.size(), "v");
    try {
        pca_fit(names, cols, keep, PcaOpts{});
    } catch (const EngineError& e) {
        return e.detail;
    }
    return "";
}

void test_pca_eigenvalue_zeroing() {
    std::printf("test_aer_estim / pca eigenvalue zeroing at ratio 1e-8:\n");
    const PcaResult below = pca_on_variances({1.0, 9e-9});
    check(below.ev[1] == 0.0 && !std::signbit(below.ev[1]), "ratio 9e-9 is exactly +0");
    check(below.lndet_missing && below.cond_missing, "a zeroed eigenvalue leaves lndet and cond missing");
    const PcaResult above = pca_on_variances({1.0, 1.1e-8});
    check(close_rel(above.ev[1], 1.1e-8, 1e-20), "ratio 1.1e-8 is kept");
    check(!above.lndet_missing && !above.cond_missing, "lndet and cond are posted when every eigenvalue is positive");
    const PcaResult tiny = pca_on_variances({1e-12, 1e-12});
    check(tiny.ev[0] > 0.0 && tiny.ev[1] > 0.0, "the rule is relative: two eigenvalues of 1e-12 stay");
}

void test_pca_trace_snap() {
    std::printf("test_aer_estim / pca trace snap at 1e-6 * |trace|:\n");
    const PcaResult snapped = pca_on_variances({999.0009, 1.0});
    check(snapped.trace == 1000.0, "1000.0009 snaps to exactly 1000");
    const PcaResult kept = pca_on_variances({999.0011, 1.0});
    check(close_rel(kept.trace, 1000.0011, 1e-12), "1000.0011 is kept");
    check(kept.rho > 0.0 && close_rel(kept.rho, 1.0, 1e-12), "rho is over the trace");
    check(snapped.rho > 1.0, "rho divides by the snapped trace, so it can pass 1");
}

void test_pca_psi_snap() {
    std::printf("test_aer_estim / pca psi snap within 1e-6 of an integer:\n");
    check(pca_on_variances({2.0, 5e-7}, 1).psi[1] == 0.0, "5e-7 snaps to 0");
    check(close_rel(pca_on_variances({2.0, 2e-6}, 1).psi[1], 2e-6, 1e-15), "2e-6 is kept");
    check(pca_on_variances({2.0, 1.0000005}, 1).psi[1] == 1.0, "1.0000005 snaps to exactly 1");
    check(close_rel(pca_on_variances({2.0, 1.000002}, 1).psi[1], 1.000002, 1e-13), "1.000002 is kept");
    check(pca_on_variances({2.0, 1.0}, 2).psi[0] == 0.0, "all components retained: exact zeros");
}

void test_pca_retention() {
    std::printf("test_aer_estim / pca retention by mineigen and components:\n");
    check(pca_on_variances({1.1e-5, 1e-6}).f == 1, "1.1e-5 is retained under the default mineigen");
    const PcaResult none = pca_on_variances({9e-6, 1e-6});
    check(none.f == 0, "9e-6 is not retained: f can be 0");
    check(none.loadings.empty() && none.rho == 0.0, "f == 0: no loadings, rho 0");
    check(none.psi.size() == 2 && none.components_tbl.size() == 8, "f == 0: psi and the table are still filled");
    check(pca_on_variances({3.0, 2.0, 1.0}, 2).f == 2, "components() caps f");
    check(std::isnan(none.components_tbl[5]), "the last row's difference is missing");
}

void test_pca_screening_order() {
    std::printf("test_aer_estim / pca screening order:\n");
    const std::vector<double> o1{1, 2, 3, 4}, o2{4, 1, 3, 2}, k1{5, 5, 5, 5};
    const std::vector<char> all(4, 1), first_only{1, 0, 0, 0}, none(4, 0);
    check(pca_refusal({o1.data(), k1.data()}, none) == "r(2000)", "no rows: r(2000)");
    check(pca_refusal({o1.data(), k1.data()}, first_only) == "r(2001)", "one row comes before zero variance");
    check(pca_refusal({k1.data(), k1.data()}, all) == "r(498)", "every column dropped: r(498)");
    check(pca_refusal({o1.data(), k1.data()}, all) == "r(102)", "one column left: r(102)");
    const PcaResult fit = pca_fit({"o1", "k1", "o2"}, {o1.data(), k1.data(), o2.data()}, all, PcaOpts{});
    check(fit.p == 2 && fit.dropped == std::vector<std::string>{"k1"}, "a constant column is dropped by name");
    check(!fit.sds.empty() && pca_on_variances({1.0, 1.0}).sds.empty(), "sds only for a correlation PCA");
}

}  // namespace

int main() {
    test_hp_t3_generative();
    test_hp_oracle_series();
    test_hp_near_zero_cycle();
    test_hp_lambda_defaults();
    test_lowess_bwidth_substitution();
    test_lowess_degenerate();
    test_lpoly_constants_and_rot();
    test_lpoly_kernels_normalize();
    test_lpoly_rot_trapezoid_constants();
    test_lpoly_rot_raw_power_fit();
    test_lpoly_at_points();
    test_lpoly_aweight_and_se();
    test_lpoly_grid_n();
    test_pca_eigenvalue_zeroing();
    test_pca_trace_snap();
    test_pca_psi_snap();
    test_pca_retention();
    test_pca_screening_order();
    if (failures) {
        std::printf("test_aer_estim: %d FAILURE(S)\n", failures);
        return 1;
    }
    std::printf("test_aer_estim: all checks passed\n");
    return 0;
}
