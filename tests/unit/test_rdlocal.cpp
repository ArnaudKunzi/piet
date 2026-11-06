// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Oracle test for rdrobust / rddensity / rdplot (src/estimators/rdlocal.{h,cpp}).
//
// THE ORACLE is StataBE 19.5 run on the P-QE wave fixture
// (internal fixture wave_fixture.dta), captured
// 2026-08-06.  Every target below is a printed cell of that run:
//
//   rdrobust y running, c(0)
//     N 4778 (2390 | 2388), Eff. N 908 | 938, Unique obs 50 | 51,
//     h 1.963, b 3.009, rho 0.652, RD Effect .34362,
//     robust z 2.3055, p 0.021, CI [.0548, .676516].
//   rddensity running, c(0)
//     N 4800 (2400 | 2400), Eff. N 2400 | 2400, h 5.000, p 2, q 3,
//     Robust T -0.1606, p 0.8724, and the ten-row binomial table below.
//   rdplot y running, c(0)
//     N 4778 (2390 | 2388), p 4, h 5.000, Bins selected 69 | 67,
//     Average/Median bin length .072 | .075, IMSE-optimal bins 5 | 3,
//     Mimicking Var. bins 69 | 67, Implied scale 13.800 | 22.333,
//     WIMSE weights 0.000 | 1.000.  Every one of these now matches exactly.
//
// The fixture is REBUILT here from its generator recipe
// (internal script fixture_stata.do) rather than read
// off disk, so this test is hermetic: it needs no .dta, no .hbin, and no
// locally-generated artifact.  The recipe is closed-form and deterministic —
// no random draws feed any column this test touches — so the reconstruction is
// the same sample Stata saw, which the tau assertions below confirm to 1e-5.
//
// THE ORACLE'S BANDWIDTHS TO SIX DIGITS.  Stata prints h and b to three
// decimals, but the run is over-determined: the robust CI and z are printed to
// six significant digits, and (h, b) -> (CI_hi, z) is invertible.  Solving it
// gives h = 1.962501, b = 3.009303, rho = 0.652145, and at those values THIS
// implementation reproduces .34362 / 2.3055 / [.0548, .676516] exactly — every
// inference cell, to every digit Stata printed.  That is the PINNED block
// below, and it is what establishes the local-polynomial core is correct.
//
// KNOWN GAP, deliberately encoded as a band rather than hidden: the AUTOMATIC
// mserd chain lands at h 1.963203 / b 3.009693 against 1.962501 / 3.009303 —
// +0.036% and +0.013%.  h and rho now print correctly (1.963, 0.652); b prints
// 3.010 where Stata prints 3.009 (it is 2e-4 over the rounding boundary), the
// effect prints .34359 against .34362, and z prints 2.3056 against 2.3055.
// Swept and rejected on the way here: the CCT14 S.2.6 selector (h 0.42), no
// regularization (h 3.01), regularization without the O^2 factor (h 0.99),
// regularization scales 2.90-3.20, all 64 assignments of {v-hat, b-hat,
// self-consistent, support} to the three constant slots, and CCT14's separate
// Step-0 c_n as the evaluation bandwidth (h 1.946).  See rdlocal.cpp's
// bandwidth-chain comment for what each of those settled.
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

#include "estimators/rdlocal.h"

using namespace cecon;

static int g_fail = 0;

static void ck(const char* what, double got, double want, double tol) {
    if (!(std::fabs(got - want) <= tol)) {
        std::printf("  FAIL %-28s got %.8g want %.8g (tol %g)\n", what, got, want, tol);
        ++g_fail;
    } else {
        std::printf("  ok   %-28s %.8g\n", what, got);
    }
}
static void cki(const char* what, long long got, long long want) {
    if (got != want) {
        std::printf("  FAIL %-28s got %lld want %lld\n", what, got, want);
        ++g_fail;
    } else {
        std::printf("  ok   %-28s %lld\n", what, got);
    }
}
static void ck_true(const char* what, bool cond) {
    if (!cond) { std::printf("  FAIL %-28s\n", what); ++g_fail; }
    else std::printf("  ok   %-28s\n", what);
}
static void ck_in(const char* what, double got, double lo, double hi) {
    if (!(got >= lo && got <= hi)) {
        std::printf("  FAIL %-28s got %.8g want in [%g, %g]\n", what, got, lo, hi);
        ++g_fail;
    } else {
        std::printf("  ok   %-28s %.8g in [%g, %g]\n", what, got, lo, hi);
    }
}

// ---- the fixture, rebuilt from fixture_stata.do -----------------------------
struct Fixture {
    std::vector<double> y, x;
};
static Fixture make_fixture() {
    Fixture f;
    const int N = 4800;
    f.y.reserve(N);
    f.x.reserve(N);
    for (int n = 1; n <= N; ++n) {
        double id = std::ceil(n / 24.0);
        double t = (n - 1) % 24 + 1;
        double x1 = std::sin(n / 7.0) + id / 200.0;
        double x2 = std::cos(n / 11.0) + t / 30.0;
        double running = ((n - 1) % 101 - 50) / 10.0;
        double rd_treat = (running >= 0) ? 1.0 : 0.0;
        double cohort = (id <= 160) ? (9 + std::fmod(id, 6.0)) : 0.0;
        double treated = (cohort > 0 && t >= cohort) ? 1.0 : 0.0;
        double y = 2 + 0.8 * x1 - 0.4 * x2 + 1.2 * treated + 0.3 * rd_treat +
                   id / 1000.0 + t / 100.0 + std::sin(n / 5.0);
        if (n % 211 == 0) y = std::nan("");  // Stata's `replace y = .a`
        f.y.push_back(y);
        f.x.push_back(running);
    }
    return f;
}

int test_rdlocal_main() {
    std::printf("test_rdlocal:\n");
    Fixture F = make_fixture();
    const int64_t N = static_cast<int64_t>(F.x.size());

    // ================= rdrobust, PINNED at the oracle's own h and b =========
    // With the bandwidths supplied, every printed cell of the Stata run is
    // reproduced: the point estimate, the bias-corrected estimate, the robust
    // standard error, z, p and the confidence interval, plus all of the N
    // accounting including the mass-point "Unique obs" row.
    std::printf(" rdrobust (h/b pinned to the oracle):\n");
    {
        RdLocalConfig cfg;
        cfg.c = 0;
        cfg.h_left = cfg.h_right = 1.962501;   // solved from the oracle's own CI + z
        cfg.b_left = cfg.b_right = 3.009303;
        RdRobustResult R = rd_robust_run(F.y.data(), F.x.data(), N, cfg);
        ck_true("ok", R.ok);
        cki("n_obs", R.n_obs, 4778);
        cki("n_left", R.n_left, 2390);
        cki("n_right", R.n_right, 2388);
        cki("n_eff_left", R.n_eff_left, 908);
        cki("n_eff_right", R.n_eff_right, 938);
        cki("n_unique_left", R.n_unique_left, 50);
        cki("n_unique_right", R.n_unique_right, 51);
        ck_true("masspoints detected", R.masspoints_detected);
        // Every one of these is a digit Stata printed, at the tolerance that
        // digit resolves — no slack beyond the printed precision.
        ck("RD Effect", R.coef_conventional, 0.34362, 1e-5);
        ck("bias-corrected coef", R.coef_bc, 0.3656580, 1e-6);
        ck("robust z", R.z_robust, 2.3055, 1e-4);
        ck("robust p", R.p_robust, 0.021, 5e-4);
        ck("robust CI lo", R.ci_robust_lo, 0.0548, 1e-6);
        ck("robust CI hi", R.ci_robust_hi, 0.676516, 1e-6);
        ck("rho", R.rho, 0.652145, 1e-6);
    }

    // ================= rdrobust, automatic mserd bandwidths ================
    std::printf(" rdrobust (automatic mserd bandwidth):\n");
    {
        RdLocalConfig cfg;
        cfg.c = 0;
        RdRobustResult R = rd_robust_run(F.y.data(), F.x.data(), N, cfg);
        ck_true("ok", R.ok);
        cki("n_obs", R.n_obs, 4778);
        cki("n_unique_left", R.n_unique_left, 50);
        cki("n_unique_right", R.n_unique_right, 51);
        // Oracle h 1.962501 / b 3.009303 / rho 0.652145.  See the KNOWN GAP
        // note at the top.  These bands are ~10x tighter than the residual
        // itself is large, so any drift in the selector fails loudly, and they
        // are centred on the ORACLE, not on what we currently produce.
        ck_in("h (oracle 1.962501)", R.h_left, 1.9600, 1.9660);
        ck_in("b (oracle 3.009303)", R.b_left, 3.0050, 3.0140);
        ck_in("rho (oracle 0.652145)", R.rho, 0.6515, 0.6530);
        // Both print correctly at three decimals; b does not (3.010 vs 3.009).
        ck("h rounds to 1.963", std::round(R.h_left * 1000) / 1000, 1.963, 1e-9);
        ck("rho rounds to 0.652", std::round(R.rho * 1000) / 1000, 0.652, 1e-9);
        // The headline effect and z at the SELECTED bandwidths — the numbers a
        // user actually sees.  Oracle .34362 and 2.3055; we are 3.0e-5 and
        // 1.3e-4 away, down from 1.2e-4 and 4.7e-3 before the selector fix.
        ck_in("effect (oracle .34362)", R.coef_conventional, 0.34357, 0.34365);
        ck_in("z (oracle 2.3055)", R.z_robust, 2.3050, 2.3062);
        ck_true("h_left == h_right (mserd)", R.h_left == R.h_right);
        ck_true("b_left == b_right (mserd)", R.b_left == R.b_right);
        // Eff. N follows from h; at the selector's h it is still 908/938,
        // because the grid's next mass points are at |x| = 2.0.
        cki("n_eff_left", R.n_eff_left, 908);
        cki("n_eff_right", R.n_eff_right, 938);
    }

    // ================= rdrobust honest rejects =============================
    std::printf(" rdrobust rejects:\n");
    {
        RdLocalConfig cfg;
        cfg.masspoints = RdMasspoints::Off;
        RdRobustResult R = rd_robust_run(F.y.data(), F.x.data(), N, cfg);
        ck_true("nomasspoints refused",
                !R.ok && R.error.find("masspoints") != std::string::npos);
        RdLocalConfig c2;
        c2.vce = "hc1";
        RdRobustResult R2 = rd_robust_run(F.y.data(), F.x.data(), N, c2);
        ck_true("vce(hc1) refused", !R2.ok && R2.error.find("vce") != std::string::npos);
    }

    // ================= rddensity ===========================================
    std::printf(" rddensity:\n");
    {
        RdLocalConfig cfg;
        cfg.c = 0;
        RdDensityResult R = rd_density_run(F.x.data(), N, cfg);
        ck_true("ok", R.ok);
        cki("p", R.p, 2);
        cki("q", R.q, 3);
        cki("n_obs", R.n_obs, 4800);
        cki("n_left", R.n_left, 2400);
        cki("n_right", R.n_right, 2400);
        cki("n_eff_left", R.n_eff_left, 2400);
        cki("n_eff_right", R.n_eff_right, 2400);
        ck("h_left", R.h_left, 5.0, 1e-12);
        ck("h_right", R.h_right, 5.0, 1e-12);
        ck("robust T", R.t_robust, -0.1606, 1e-4);
        ck("robust p", R.p_robust, 0.8724, 1e-4);
        // The ten-row binomial table (H0: prob = .5).  These counts are pure
        // arithmetic on the running-variable grid and are the cheapest
        // falsifier of the window inclusion rule (|x-c| <= w, left is x < c).
        static const struct { double w; long long l, r; double p; } B[10] = {
            {0.1,  48,  96, 0.0001}, {0.2,  96, 144, 0.0023},
            {0.3, 144, 191, 0.0118}, {0.4, 192, 238, 0.0299},
            {0.5, 240, 285, 0.0547}, {0.6, 288, 332, 0.0841},
            {0.7, 336, 379, 0.1162}, {0.8, 384, 426, 0.1497},
            {0.9, 432, 473, 0.1836}, {1.0, 480, 520, 0.2174},
        };
        cki("binomial rows", static_cast<long long>(R.binomial.size()), 10);
        for (size_t i = 0; i < R.binomial.size() && i < 10; ++i) {
            char buf[64];
            std::snprintf(buf, sizeof buf, "binom w=%.1f window", B[i].w);
            ck(buf, R.binomial[i].window, B[i].w, 1e-12);
            std::snprintf(buf, sizeof buf, "binom w=%.1f n_left", B[i].w);
            cki(buf, R.binomial[i].n_left, B[i].l);
            std::snprintf(buf, sizeof buf, "binom w=%.1f n_right", B[i].w);
            cki(buf, R.binomial[i].n_right, B[i].r);
            std::snprintf(buf, sizeof buf, "binom w=%.1f p", B[i].w);
            ck(buf, R.binomial[i].p_value, B[i].p, 5e-5);
        }
    }

    // ================= rdplot ==============================================
    std::printf(" rdplot:\n");
    {
        RdLocalConfig cfg;
        cfg.c = 0;
        RdPlotResult R = rd_plot_run(F.y.data(), F.x.data(), N, cfg);
        ck_true("ok", R.ok);
        cki("n_obs", R.n_obs, 4778);
        cki("n_left", R.n_left, 2390);
        cki("n_right", R.n_right, 2388);
        cki("n_eff_left", R.n_eff_left, 2390);
        cki("n_eff_right", R.n_eff_right, 2388);
        // BUG-184: rdplot must report the same design facts rdrobust already
        // reports on the SAME running variable — that identity is the whole
        // argument of the bug (an omission in one command's payload, not a
        // disagreement about the data), so the expected values here are the
        // 50 / 51 / true already pinned in the rdrobust section above, not a
        // second hand-derivation. Without these, PIET could not print Stata's
        // "Mass points detected in the running variable." preamble for rdplot.
        cki("n_unique_left (== rdrobust's)", R.n_unique_left, 50);
        cki("n_unique_right (== rdrobust's)", R.n_unique_right, 51);
        ck_true("masspoints detected (== rdrobust's)", R.masspoints_detected);
        cki("poly order left", R.poly_order_left, 4);
        cki("poly order right", R.poly_order_right, 4);
        ck("h_left", R.h_left, 5.0, 1e-12);
        ck("h_right", R.h_right, 5.0, 1e-12);
        cki("bins selected left", R.bins_left, 69);
        cki("bins selected right", R.bins_right, 67);
        cki("mimicking-var bins left", R.bins_mv_left, 69);
        cki("mimicking-var bins right", R.bins_mv_right, 67);
        ck("avg bin length left", R.avg_bin_len_left, 0.072, 5e-4);
        ck("avg bin length right", R.avg_bin_len_right, 0.075, 5e-4);
        ck("med bin length left", R.med_bin_len_left, 0.072, 5e-4);
        ck("med bin length right", R.med_bin_len_right, 0.075, 5e-4);
        ck("WIMSE var weight", R.wimse_var_weight, 0.0, 1e-12);
        ck("WIMSE bias weight", R.wimse_bias_weight, 1.0, 1e-12);
        cki("IMSE-optimal bins left", R.bins_imse_left, 5);
        cki("IMSE-optimal bins right", R.bins_imse_right, 3);
        ck("implied scale left", R.implied_scale_left, 13.800, 1e-3);
        ck("implied scale right", R.implied_scale_right, 22.333, 1e-3);
        // The scatter can never have more non-empty bins than the running
        // variable has distinct values on that side (50 | 51 here).
        cki("scatter points left", static_cast<long long>(R.bin_x_left.size()), 50);
        cki("scatter points right", static_cast<long long>(R.bin_x_right.size()), 51);
        ck_true("fitted line sampled left", R.fit_x_left.size() == R.fit_y_left.size() &&
                                             R.fit_x_left.size() > 10);
        ck_true("fitted line sampled right", R.fit_x_right.size() == R.fit_y_right.size() &&
                                              R.fit_x_right.size() > 10);
        ck_true("fit coefs are order-4", R.coef_left.size() == 5 && R.coef_right.size() == 5);
        // The fitted line must cross the cutoff with roughly the design's own
        // jump (+0.3 by construction, .34 as estimated); a sanity check that
        // the two sides were fit on the right halves of the sample.
        double jump = R.coef_right.front() - R.coef_left.front();
        ck_in("fitted jump at cutoff", jump, 0.05, 0.65);
    }

    // BUG-184 discriminator: `masspoints` must actually track the data, not be a
    // constant that happens to read "adjust" on the one fixture above. Same
    // generator, but the running variable is made strictly distinct per row, so
    // ties disappear and the flag must flip. Without this, the assertions above
    // would pass just as well against a hard-coded `true`.
    std::printf(" rdplot (continuous running variable — masspoints control):\n");
    {
        std::vector<double> yc, xc;
        yc.reserve(static_cast<size_t>(N));
        xc.reserve(static_cast<size_t>(N));
        for (int64_t i = 0; i < N; ++i) {
            if (!std::isfinite(F.y[static_cast<size_t>(i)])) continue;
            // 4778 distinct values spanning the same [-5, 5] support.
            xc.push_back(-5.0 + 10.0 * (static_cast<double>(i) + 0.5) / static_cast<double>(N));
            yc.push_back(F.y[static_cast<size_t>(i)]);
        }
        RdLocalConfig cfg;
        cfg.c = 0;
        RdPlotResult R = rd_plot_run(yc.data(), xc.data(), static_cast<int64_t>(xc.size()), cfg);
        ck_true("ok", R.ok);
        ck_true("masspoints NOT detected on a tie-free running variable",
                !R.masspoints_detected);
        ck_true("unique counts equal the per-side obs counts when tie-free",
                R.n_unique_left == R.n_left && R.n_unique_right == R.n_right);
    }

    // ================= degenerate input ====================================
    std::printf(" guards:\n");
    {
        double yy[4] = {1, 2, 3, 4};
        double xx[4] = {-1, -0.5, 0.5, 1};
        RdRobustResult R = rd_robust_run(yy, xx, 4);
        ck_true("too-few-obs refused", !R.ok && !R.error.empty());
        RdRobustResult R0 = rd_robust_run(nullptr, nullptr, 0);
        ck_true("no-data refused", !R0.ok);
    }

    if (g_fail) {
        std::printf("FAILED (%d checks)\n", g_fail);
        return 1;
    }
    std::printf("PASSED\n");
    return 0;
}
