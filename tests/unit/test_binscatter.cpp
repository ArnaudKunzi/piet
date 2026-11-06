// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// binscatter (engine half) — ring-1 hand-verifiable checks plus the oracle pins.
//
// Drives the production kernel itself: apps/cmds/cmds_binscatter.cpp is
// compiled into this TU with COBRA_BINSCATTER_KERNEL_ONLY, which leaves out the
// CLI wrapper (flag parsing, acquire_data, emit) and keeps binscatter_kernel(),
// the function `cobra binscatter` calls after its if/in mask. So a regression
// in the sample, the residualization, the bins or the fits fails here.
//
// Case A — 8 rows, y == x, no missing, nquantiles(4):
//   x = 1..8. _pctile's cut points at p=25/50/75 for N=8 land EXACTLY on a rank
//   boundary each time, so each averages the two straddling values:
//     p25 = (2+3)/2 = 2.5, p50 = (4+5)/2 = 4.5, p75 = (6+7)/2 = 6.5
//   fastxtile's rule is 1 + #cut points strictly below x (ties go low), so
//   x=1,2 -> bin 1, 3,4 -> bin 2, 5,6 -> bin 3, 7,8 -> bin 4; every bin has two
//   rows and, since y == x, x mean == y mean: 1.5 3.5 5.5 7.5. The fit line is
//   the OLS of y on x with y == x exactly: slope 1, constant 0.
//
// Case B — the same 8 rows PLUS a 9th (x=9, y missing): marksample drops it
//   before either the fit or the bins see it, so case B reproduces case A with
//   e(N) = 8 (leaking x=9 into the top bin, or fitting on 9 rows, is the defect
//   class this pins against).
//
// Case C — the oracle's own fixture (tests/eval/golden/aer_w2/w2k_binscatter.do,
//   the 120-row closed-form `data` block, ties in x on purpose) against the
//   landed log tests/eval/golden/aer_w2/w2k_binscatter.log: e(N), e(y#_coefs)
//   (%21x, rtol 1e-9, the T2 row of tests/eval/TOLERANCE.md) and the bin points
//   the savedata CSV prints (8 significant digits there, so 5e-8 relative).
//   Each arm cites its log lines. Before gfx-binscatter the engine refused
//   controls()/absorb() by name and had no by(), several y, weights, medians,
//   discrete or line types, so every arm but BS_ctl0/BS_n10 was unreachable.
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#define COBRA_BINSCATTER_KERNEL_ONLY
#include "../../apps/cmds/cmds_binscatter.cpp"
#undef COBRA_BINSCATTER_KERNEL_ONLY

// The fixture is built with Stata's arithmetic: every product rounded (no FMA).
#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(__GNUC__)
#pragma GCC optimize("fp-contract=off")
#endif

using namespace cecon;

namespace {

int g_fail = 0;
void check(const std::string& what, double got, double want, double rtol) {
    const double err = std::fabs(got - want);
    const double scale = std::fabs(want);
    if (!(err <= rtol * scale) && !(err == 0.0)) {
        std::printf("  FAIL %-34s got=%.17g want=%.17g (rel=%.3g > %.3g)\n", what.c_str(), got, want,
                    scale > 0 ? err / scale : err, rtol);
        ++g_fail;
    } else {
        std::printf("  ok   %-34s = %.17g\n", what.c_str(), got);
    }
}
void checki(const std::string& what, long long got, long long want) {
    if (got != want) {
        std::printf("  FAIL %-34s got=%lld want=%lld\n", what.c_str(), got, want);
        ++g_fail;
    } else {
        std::printf("  ok   %-34s = %lld\n", what.c_str(), got);
    }
}

json run(const Dataset& ds, const cli::BinscatterSpec& spec, std::vector<char> ifin = {}) {
    if (ifin.empty()) ifin.assign(static_cast<size_t>(ds.n_rows()), 1);
    return cli::binscatter_kernel(ds, std::move(ifin), spec);
}

cli::BinscatterSpec spec_of(std::vector<std::string> ys, const std::string& x, int nq) {
    cli::BinscatterSpec s;
    s.ys = std::move(ys);
    s.x = x;
    s.nquantiles = nq;
    return s;
}

void case_ab(const char* label, const Dataset& ds) {
    std::printf("%s:\n", label);
    const json r = run(ds, spec_of({"y"}, "x", 4));
    checki("N", r["N"].get<long long>(), 8);
    checki("nquantiles", r["nquantiles"].get<long long>(), 4);
    const json& pts = r["points"][0][0];
    const double xm[] = {1.5, 3.5, 5.5, 7.5};
    for (int b = 0; b < 4; ++b) {
        checki("bin" + std::to_string(b + 1) + ".n", r["bin_counts"][0][b].get<long long>(), 2);
        check("bin" + std::to_string(b + 1) + ".x_mean", pts["x"][b].get<double>(), xm[b], 1e-12);
        check("bin" + std::to_string(b + 1) + ".y_mean", pts["y"][b].get<double>(), xm[b], 1e-12);
    }
    check("fit.slope", r["coefs"][0][0][0].get<double>(), 1.0, 1e-12);
    if (std::fabs(r["coefs"][0][0][1].get<double>()) > 1e-12) {
        std::printf("  FAIL fit.constant not 0\n");
        ++g_fail;
    }
}

// --- case C: the oracle fixture ------------------------------------------------
struct Fixture {
    Frame fr;
    Dataset ds;
};

// w2k_binscatter.do:143-158 (`set obs 120` ... `generate int t = _n`).
void build_fixture(Frame& fr) {
    const int n = 120;
    std::vector<double> id, g, grp, x, z, y, y2, xd, w;
    for (int i = 1; i <= n; ++i) {
        const double gi = std::fmod(i - 1, 4) + 1;
        const double grpi = std::fmod(i, 2);
        const double xi = std::floor(std::fmod(i * 37.0, 101.0) / 4) / 2 + grpi;
        const double zi = std::fmod(i * 13.0, 17.0) / 3;
        double yi = 1 + 0.5 * xi;
        yi = yi + 0.3 * zi;
        yi = yi + gi / 2;
        yi = yi + std::fmod(i * 7.0, 11.0) / 5;
        yi = yi - grpi * xi / 4;
        double y2i = 2 - 0.25 * xi;
        y2i = y2i + zi / 2;
        y2i = y2i + std::fmod(i * 5.0, 7.0) / 3;
        id.push_back(i);
        g.push_back(gi);
        grp.push_back(grpi);
        x.push_back(xi);
        z.push_back(zi);
        y.push_back(yi);
        y2.push_back(y2i);
        xd.push_back(std::fmod(i * 7.0, 6.0));
        w.push_back(1 + std::fmod(i, 3));
    }
    fr.set_n_rows_hint(n);
    fr.append_f64_column("id", id);
    fr.append_f64_column("g", g);
    fr.append_f64_column("grp", grp);
    fr.append_f64_column("x", x);
    fr.append_f64_column("z", z);
    fr.append_f64_column("y", y);
    fr.append_f64_column("y2", y2);
    fr.append_f64_column("xd", xd);
    fr.append_f64_column("w", w);
    fr.finalize_after_bulk_load();
}

// %21x of the log, as a C99 hex-float literal (e.g. +1.67ce762c0391dX-002 ==
// 0x1.67ce762c0391dp-2).
void pin_coefs(const std::string& arm, const json& r, int yi, const std::vector<std::vector<double>>& want) {
    const json& rows = r["coefs"][static_cast<size_t>(yi)];
    checki(arm + " coef rows", static_cast<long long>(rows.size()), static_cast<long long>(want.size()));
    for (size_t b = 0; b < want.size() && b < rows.size(); ++b)
        for (size_t j = 0; j < want[b].size(); ++j)
            check(arm + " c" + std::to_string(yi + 1) + "_r" + std::to_string(b + 1) + "[" + std::to_string(j + 1) + "]",
                  rows[b][j].get<double>(), want[b][j], 1e-9);
}

// CSV points: x then y per by-group column, 8 significant digits in the log.
void pin_points(const std::string& arm, const json& r, int yi, int by, const std::vector<double>& xs,
                const std::vector<double>& ys) {
    const json& p = r["points"][static_cast<size_t>(yi)][static_cast<size_t>(by)];
    checki(arm + " bins", static_cast<long long>(p["x"].size()), static_cast<long long>(xs.size()));
    for (size_t i = 0; i < xs.size() && i < p["x"].size(); ++i) {
        check(arm + " x[" + std::to_string(i + 1) + "]", p["x"][i].get<double>(), xs[i], 5e-8);
        check(arm + " y[" + std::to_string(i + 1) + "]", p["y"][i].get<double>(), ys[i], 5e-8);
    }
}

void case_c() {
    std::printf("case C (oracle fixture, w2k_binscatter.log):\n");
    Frame fr;
    build_fixture(fr);
    const Dataset ds = fr.as_dataset_view();

    {   // BS_ctl0 (log 238, 249, 252-257): binscatter y x, nquantiles(5)
        const json r = run(ds, spec_of({"y"}, "x", 5));
        checki("BS_ctl0 N", r["N"].get<long long>(), 120);
        pin_coefs("BS_ctl0", r, 0, {{0x1.67ce762c0391dp-2, 0x1.0b9f0cb5e3dd6p+2}});
        pin_points("BS_ctl0", r, 0, 0, {1.5208333, 4.2857143, 7, 9.5208333, 11.85},
                   {4.1125, 6.1258929, 7.190625, 7.446875, 7.8925});
    }
    {   // BS_n10 (log 322, 333, 337-346): n(10)
        const json r = run(ds, spec_of({"y"}, "x", 10));
        pin_coefs("BS_n10", r, 0, {{0x1.67ce762c0391dp-2, 0x1.0b9f0cb5e3dd6p+2}});
        pin_points("BS_n10", r, 0, 0,
                   {.92307692, 2.2272727, 3.4615385, 5, 6.1666667, 7.5, 8.7222222, 10, 11.277778, 12.318182},
                   {3.9326923, 4.325, 5.5038462, 6.665, 7.0583333, 7.27, 7.1444444, 7.6283333, 7.75, 8.0090909});
    }
    {   // BS_controls (log 416, 427, 431-435): controls(z), mean added back
        cli::BinscatterSpec s = spec_of({"y"}, "x", 5);
        s.controls = {"z"};
        const json r = run(ds, s);
        pin_coefs("BS_controls", r, 0, {{0x1.608530279b341p-2, 0x1.0e9e882aef299p+2}});
        pin_points("BS_controls", r, 0, 0, {1.5411668, 4.0874, 6.5406331, 9.1468669, 11.6006},
                   {4.1531827, 6.0945948, 6.9739414, 7.2739753, 7.9782641});
    }
    {   // BS_absorb (log 533, 544, 548-552): absorb(g), areg residuals y - xb - d
        cli::BinscatterSpec s = spec_of({"y"}, "x", 5);
        s.absorb = "g";
        const json r = run(ds, s);
        pin_coefs("BS_absorb", r, 0, {{0x1.7f81e82ce71eep-2, 0x1.01de8c9279454p+2}});
        pin_points("BS_absorb", r, 0, 0, {1.5913333, 4.1181159, 6.5833333, 9.1041668, 11.625},
                   {4.1075251, 5.9711685, 7.0239583, 7.421875, 8.0270833});
    }
    {   // BS_by (log 611, 622-623, 627-628, 632-636): by(grp), pooled bins
        cli::BinscatterSpec s = spec_of({"y"}, "x", 5);
        s.by = "grp";
        const json r = run(ds, s);
        checki("BS_by byvals", static_cast<long long>(r["byvals"].size()), 2);
        check("BS_by byval1", r["byvals"][0].get<double>(), 0.0, 0);
        check("BS_by byval2", r["byvals"][1].get<double>(), 1.0, 0);
        pin_coefs("BS_by", r, 0, {{0x1.e45b308d08c60p-2, 0x1.1ce5a288f7c6cp+2}, {0x1.1975e25788d9bp-2, 0x1.d6110a3bebf0fp+1}});
        pin_points("BS_by", r, 0, 0, {1.3461538, 4.3, 7.125, 9.6666667, 11.75}, {4.75, 6.6166667, 8.5125, 8.65, 9.8375});
        pin_points("BS_by", r, 0, 1, {1.7272727, 4.2692308, 6.875, 9.375, 11.916667},
                   {3.3590909, 5.5596154, 5.86875, 6.24375, 6.5958333});
    }
    {   // BS_median (log 707, 718, 722-726): medians; the fit is BS_ctl0's
        cli::BinscatterSpec s = spec_of({"y"}, "x", 5);
        s.medians = true;
        const json r = run(ds, s);
        pin_coefs("BS_median", r, 0, {{0x1.67ce762c0391dp-2, 0x1.0b9f0cb5e3dd6p+2}});
        pin_points("BS_median", r, 0, 0, {1.5, 4.5, 7, 9.5, 12}, {3.825, 6.175, 7.25, 7.825, 7.175});
    }
    {   // BS_disc (log 746, 757-762) and BS_disc_auto (log 782, 793-798): xd has
        // 6 values; explicit discrete and the automatic rule (6 <= 20) agree
        cli::BinscatterSpec s = spec_of({"y"}, "xd", 20);
        s.discrete = true;
        const json r = run(ds, s);
        checki("BS_disc nquantiles", r["nquantiles"].get<long long>(), 6);
        pin_points("BS_disc", r, 0, 0, {0, 1, 2, 3, 4, 5}, {7.9525, 5.79625, 7.425, 5.59875, 6.905, 5.29125});
        const json ra = run(ds, spec_of({"y"}, "xd", 20));
        checki("BS_disc_auto discrete", ra["discrete"].get<bool>() ? 1 : 0, 1);
        checki("BS_disc_auto nquantiles", ra["nquantiles"].get<long long>(), 6);
        pin_points("BS_disc_auto", ra, 0, 0, {0, 1, 2, 3, 4, 5}, {7.9525, 5.79625, 7.425, 5.59875, 6.905, 5.29125});
    }
    {   // BS_aw (log 1335, 1346, 1350-1354): [aw=w]
        cli::BinscatterSpec s = spec_of({"y"}, "x", 5);
        s.weight = "w";
        const json r = run(ds, s);
        pin_coefs("BS_aw", r, 0, {{0x1.66160d9d4b57bp-2, 0x1.0affee87c3f86p+2}});
        pin_points("BS_aw", r, 0, 0, {1.375, 4.1607143, 6.8541667, 9.3854167, 11.7},
                   {4.0328125, 6.0272321, 7.2026042, 7.3848958, 7.76875});
    }
    {   // BS_multiy_if (log 1292, 1303, 1307, 1311-1315): y y2 x if g <= 3, qfit
        cli::BinscatterSpec s = spec_of({"y", "y2"}, "x", 5);
        s.linetype = "qfit";
        std::vector<char> ifin(120, 0);
        for (int i = 0; i < 120; ++i) ifin[static_cast<size_t>(i)] = (i % 4) + 1 <= 3;
        const json r = run(ds, s, ifin);
        checki("BS_multiy_if N", r["N"].get<long long>(), 90);
        pin_coefs("BS_multiy_if", r, 0, {{-0x1.25bbbf86120e6p-5, 0x1.9dfc47ef54582p-1, 0x1.59a01562894a2p+1}});
        pin_coefs("BS_multiy_if", r, 1, {{0x1.6da90a7ecc43cp-8, -0x1.3198bcad270d7p-2, 0x1.16c93a9026582p+2}});
        pin_points("BS_multiy_if", r, 1, 0, {1.6388889, 4.3, 6.9722222, 9.4722222, 11.875},
                   {3.6550926, 3.4083333, 2.6828704, 1.8171296, 1.6666667});
    }
    {   // BS_077 (log 1416, 1427-1431): if id <= 100, by(grp) controls(i.g)
        // n(5) line(none): float residuals plus the float mean (x_by1 1.6209999)
        cli::BinscatterSpec s = spec_of({"y"}, "x", 5);
        s.by = "grp";
        s.controls = {"i.g"};
        s.linetype = "none";
        std::vector<char> ifin(120, 0);
        for (int i = 0; i < 100; ++i) ifin[static_cast<size_t>(i)] = 1;
        const json r = run(ds, s, ifin);
        checki("BS_077 N", r["N"].get<long long>(), 100);
        checki("BS_077 no coefs", r.contains("coefs") ? 1 : 0, 0);
        pin_points("BS_077", r, 0, 0, {1.6209999, 4.0116666, 6.4709999, 8.9704545, 11.555},
                   {3.91955, 5.4029722, 7.0845501, 7.5938409, 8.4037501});
    }
}

// Case D — regress's solve (solve_regress) to the last bit, against StataBE
// 19.5 %21x answers. `==` on purpose: this is the arithmetic the text arms
// (BS_lt_qfit, BS_fw, ...) pin through the twoway command string.
void exact(const std::string& what, double got, double want) {
    if (got != want) {
        std::printf("  FAIL %-34s got=%a want=%a\n", what.c_str(), got, want);
        ++g_fail;
    } else {
        std::printf("  ok   %-34s = %a\n", what.c_str(), got);
    }
}
void case_d() {
    std::printf("case D (regress solve, bit-exact):\n");
    // `set seed 777; set obs 2; x = (0, 1+runiform()*7); y = (0, runiform()*5-1);
    // reg y x`, first draw: centered A = sum dx^2, c = sum dx*dy. Here
    // fl(sqrt(A))^2 > A, so the unit-diagonal scaling leaves A/(D*D) = 1-2^-53
    // and Stata's slope is 2 ulps above the plain Cholesky (c/D)/D = ...710p-3.
    {
        std::vector<double> b;
        const bool ok = cli::solve_regress({0x1.dfe17b12ab7cap+3}, {-0x1.3883902248ed5p+1}, 1, {0}, b);
        checki("D1 solved", ok ? 1 : 0, 1);
        if (ok) exact("D1 _b[x]", b[0], -0x1.4d6e558273712p-3);
    }
    // w2k_binscatter fixture, `reg y x2 x` (BS_lt_qfit's fit line): centered
    // moments of (x2, x) and y; e(b) = -0x1.224ff0a799468p-5, 0x1.a290f386be741p-1.
    {
        std::vector<double> b;
        const bool ok = cli::solve_regress({0x1.1eb043333332bp+18, 0x1.4841000000001p+14, 0x1.4841000000001p+14,
                                            0x1.8f4aaaaaaaaa8p+10},
                                           {0x1.a72d99999999bp+12, 0x1.1899ddddddddep+9}, 2, {0, 1}, b);
        checki("D2 solved", ok ? 1 : 0, 1);
        if (ok) {
            exact("D2 _b[x2]", b[0], -0x1.224ff0a799468p-5);
            exact("D2 _b[x]", b[1], 0x1.a290f386be741p-1);
        }
    }
}

}  // namespace

int test_binscatter_main() {
    std::printf("test_binscatter:\n");
    const double NA = std::numeric_limits<double>::quiet_NaN();
    {
        Frame fr;
        fr.set_n_rows_hint(8);
        fr.append_f64_column("x", {1, 2, 3, 4, 5, 6, 7, 8});
        fr.append_f64_column("y", {1, 2, 3, 4, 5, 6, 7, 8});
        fr.finalize_after_bulk_load();
        case_ab("case A (clean, n=8, nq=4)", fr.as_dataset_view());
    }
    {
        Frame fr;
        fr.set_n_rows_hint(9);
        fr.append_f64_column("x", {1, 2, 3, 4, 5, 6, 7, 8, 9});
        fr.append_f64_column("y", {1, 2, 3, 4, 5, 6, 7, 8, NA});
        fr.finalize_after_bulk_load();
        case_ab("case B (9th row y-missing, n=8, nq=4)", fr.as_dataset_view());
    }
    case_c();
    case_d();
    if (g_fail) {
        std::printf("FAILED (%d checks)\n", g_fail);
        return 1;
    }
    std::printf("PASSED\n");
    return 0;
}
