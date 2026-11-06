// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// stats-summary-tabulate-family (BUG-1013, BUG-1015): the engine kernels behind
// summarize/tabstat and xtile's weighted cutpoints, pinned to the StataBE 19.5
// capture tests/eval/golden/aer_w2/w2as_summary_stats_family.log.
//
// Each case FAILS on the pre-fix engine:
//   (a) ExactSum / Frame::summarize: 1e16 + 22 ones - 1e16 summed in data order is
//       0; Stata's r(sum) is 22 (sx_sum_acc3, sx_sum_acc4 with 1e20, sx_su_acc5
//       with 1e100) and r(mean) is 22/24 (+1.d555555555555X-001);
//   (b) Frame::summarize with a row mask (tabstat's per-group summarize) equals
//       the summary of the rows copied out -- the mask did not exist;
//   (c) Frame::xtile with an aweight: the groups of sx_xtile_n_aw_v1..v3 (the
//       weight was refused).
// Counts are compared exactly, values at 1e-12 relative (TOLERANCE.md T1).

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "core/exact_sum.h"
#include "data/frame.h"

using namespace cecon;

static int g_fail = 0;
static int g_checks = 0;

static void check(const std::string& what, double got, double want) {
    ++g_checks;
    const bool bothMissing = std::isnan(got) && std::isnan(want);
    const double tol = 1e-12 * std::max(1.0, std::fabs(want));
    if (!bothMissing && !(std::fabs(got - want) <= tol)) {
        std::printf("  FAIL %s: got %a want %a\n", what.c_str(), got, want);
        ++g_fail;
    }
}

static void checkExact(const std::string& what, double got, double want) {
    ++g_checks;
    const bool bothMissing = std::isnan(got) && std::isnan(want);
    if (!bothMissing && got != want) {
        std::printf("  FAIL %s: got %a want %a (bit-exact)\n", what.c_str(), got, want);
        ++g_fail;
    }
}

// The capture's 24-row fixture: acc3/acc4/acc5 are 1 everywhere but rows 1 and 24
// (+big, -big); y and aw as w2as_summary_stats_family.do generates them.
static Frame build_fixture() {
    const int n = 24;
    std::vector<double> acc3(n, 1.0), acc4(n, 1.0), acc5(n, 1.0), y, aw, g;
    acc3[0] = 1e16;  acc3[n - 1] = -1e16;
    acc4[0] = 1e20;  acc4[n - 1] = -1e20;
    acc5[0] = 1e100; acc5[n - 1] = -1e100;
    const double NA = std::nan("");
    const double yv[24] = {2.1, 0.4, 3.3, 1.2, NA,  4.8, 3.9, 4.4, 2.2, -1.5, 6.1, 1.1,
                           2.9, 2.6, 5.7, 0.9, 3.1, 7.7, 4.0, 1.8, NA,  8.8, 5.0, 2.3};
    const double gv[24] = {1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3, 4, NA};
    for (int i = 1; i <= n; ++i) {
        y.push_back(yv[i - 1]);
        g.push_back(gv[i - 1]);
        aw.push_back(0.5 + std::fmod(i * 3.0, 7.0) / 4.0);
    }
    Frame fr;
    fr.set_n_rows_hint(n);
    fr.append_f64_column("acc3", acc3);
    fr.append_f64_column("acc4", acc4);
    fr.append_f64_column("acc5", acc5);
    fr.append_f64_column("y", y);
    fr.append_f64_column("aw", aw);
    fr.append_f64_column("g", g);
    fr.finalize_after_bulk_load();
    return fr;
}

int main() {
    std::printf("test_summary_kernels:\n");

    // (a) the accumulator itself, then through Frame::summarize (sx_sum_acc3/4, su_acc5).
    for (double big : {1e16, 1e20, 1e100}) {
        ExactSum s;
        s.add(big);
        for (int i = 0; i < 22; ++i) s.add(1.0);
        s.add(-big);
        checkExact("ExactSum big=" + std::to_string(big), s.value(), 22.0);
    }
    Frame fr = build_fixture();
    for (const char* v : {"acc3", "acc4", "acc5"}) {
        const Frame::Summary sm = fr.summarize(v, false);
        checkExact(std::string(v) + " N", static_cast<double>(sm.n), 24.0);
        checkExact(std::string(v) + " r(sum)", sm.sum, 22.0);
        checkExact(std::string(v) + " r(mean)", sm.mean, 0x1.d555555555555p-1);
    }
    {   // meanonly reads the same sum (sx_sum_acc3_mo).
        const Frame::Summary sm = fr.summarize("acc3", true);
        checkExact("acc3 detail r(sum)", sm.sum, 22.0);
    }

    // (b) the row mask: group g == 1 of y (tabstat x, by(g)).
    {
        std::vector<char> keep(24, 0);
        std::vector<double> sub;
        const double gv[24] = {1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3, 4, -1};
        const auto& y = fr.col("y").f64;
        for (int i = 0; i < 24; ++i)
            if (gv[i] == 1) {
                keep[static_cast<size_t>(i)] = 1;
                if (!std::isnan(y[i])) sub.push_back(y[i]);
            }
        const Frame::Summary sm = fr.summarize("y", true, "", false, &keep);
        checkExact("mask N", static_cast<double>(sm.n), static_cast<double>(sub.size()));
        ExactSum es;
        for (double v : sub) es.add(v);
        check("mask r(sum)", sm.sum, es.value());
        check("mask r(mean)", sm.mean, es.value() / static_cast<double>(sub.size()));
        checkExact("mask r(min)", sm.min, 0.4);
        checkExact("mask r(max)", sm.max, 4.8);
        // An empty mask is N 0, sum 0, mean missing (sx_sum_empty).
        std::vector<char> none(24, 0);
        const Frame::Summary s0 = fr.summarize("y", false, "", false, &none);
        checkExact("empty N", static_cast<double>(s0.n), 0.0);
        checkExact("empty r(sum)", s0.sum, 0.0);
        check("empty r(mean)", s0.mean, std::nan(""));
        // aw through the mask: tabstat x [aw=aw], by(d) is summarize [aw] per group.
        const Frame::Summary sw = fr.summarize("y", false, "aw", true, &keep);
        checkExact("mask aw N (rows)", static_cast<double>(sw.n), static_cast<double>(sub.size()));
    }

    // (c) xtile qw = y [aw=aw], n(3): sx_xtile_n_aw_v1..v3, ids 1..24 (. at 5 and 21).
    {
        fr.xtile("qw", "y", 3, "aw");
        const double NA = std::nan("");
        const double want[24] = {1, 1, 2, 1, NA, 3, 2, 2, 2, 1, 3, 1,
                                 2, 2, 3, 1, 2, 3, 2, 1, NA, 3, 3, 2};
        const auto& q = fr.col("qw").f64;
        for (int i = 0; i < 24; ++i)
            checkExact("xtile aw id " + std::to_string(i + 1), q[static_cast<size_t>(i)], want[i]);
    }

    std::printf("  %d checks, %d failed\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
