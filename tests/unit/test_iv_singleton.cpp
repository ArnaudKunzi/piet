// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Regression test for BUG-021: iv/ivregress FE path must drop singleton FE groups
// by default (like feols/reghdfe/ivreghdfe, Correia 2015). Exercises the low-level
// drop_singletons_iv() directly on a hand-built IvModelData so the test needs no
// on-disk fixture. The CLI default + --keep-singletons opt-out is covered by the
// companion shell gate (iv_singleton_cli).
#include <cmath>
#include <cstdio>
#include <vector>

#include "core/model.h"

using namespace cecon;

static int g_fail = 0;
static void expect(const char* what, long long got, long long want) {
    if (got != want) {
        std::printf("  FAIL %-28s got=%lld want=%lld\n", what, got, want);
        ++g_fail;
    } else {
        std::printf("  ok   %-28s = %lld\n", what, got);
    }
}

// Build a Factor from an explicit code vector, deriving n_levels and per-level counts.
static Factor make_factor(const std::string& name, const std::vector<int32_t>& codes) {
    Factor f;
    f.name = name;
    f.codes = codes;
    int32_t maxc = -1;
    for (int32_t c : codes) if (c > maxc) maxc = c;
    f.n_levels = maxc + 1;
    f.count.assign(static_cast<size_t>(f.n_levels), 0.0);
    for (int32_t c : codes) f.count[static_cast<size_t>(c)] += 1.0;
    return f;
}

int test_iv_singleton_main() {
    std::printf("test_iv_singleton:\n");

    // 6 rows. FE has 3 levels: level 0 = rows {0,1}, level 1 = rows {2,3,4},
    // level 2 = row {5} (a SINGLETON). reghdfe/ivreghdfe drop the singleton row,
    // leaving 5 rows. No second FE, so exactly one pass removes exactly one row.
    const int64_t n = 6;
    IvModelData md;
    md.n = n;
    md.n_dropped = 0;
    md.has_intercept = false;  // FE present
    md.kexog = 1;
    md.kendog = 1;
    md.kinst = 1;
    // Column-major single columns (length n each).
    md.Xexog = {10, 11, 12, 13, 14, 15};
    md.Xendog = {20, 21, 22, 23, 24, 25};
    md.Zinst = {30, 31, 32, 33, 34, 35};
    md.y = {1, 2, 3, 4, 5, 6};
    md.exog_names = {"xc"};
    md.endog_names = {"d"};
    md.keep = {100, 101, 102, 103, 104, 105};  // arbitrary original row ids
    md.fe.push_back(make_factor("g", {0, 0, 1, 1, 1, 2}));

    int64_t dropped = drop_singletons_iv(md);

    expect("rows dropped", dropped, 1);
    expect("n after drop", md.n, 5);
    expect("n_dropped bumped", md.n_dropped, 1);
    // The singleton row (index 5) is gone; surviving keep ids are 100..104.
    bool keep_ok = (md.keep.size() == 5) && md.keep.back() == 104;
    expect("keep compacted (last==104)", keep_ok ? 1 : 0, 1);
    // FE recompressed to 2 dense levels with counts {2,3}.
    expect("fe n_levels after drop", md.fe[0].n_levels, 2);
    bool counts_ok = md.fe[0].count.size() == 2 &&
                     md.fe[0].count[0] == 2.0 && md.fe[0].count[1] == 3.0;
    expect("fe level counts {2,3}", counts_ok ? 1 : 0, 1);
    // Parallel arrays subset to the same 5 rows (last surviving endog value == 24).
    bool arr_ok = md.Xendog.size() == 5 && md.Xendog.back() == 24.0 &&
                  md.Xexog.size() == 5 && md.Zinst.size() == 5 && md.y.size() == 5;
    expect("parallel arrays subset", arr_ok ? 1 : 0, 1);

    // Control: a model whose FE has NO singleton must be untouched.
    IvModelData md2;
    md2.n = 4; md2.has_intercept = false;
    md2.kexog = 1; md2.kendog = 1; md2.kinst = 1;
    md2.Xexog = {1, 2, 3, 4}; md2.Xendog = {1, 2, 3, 4};
    md2.Zinst = {1, 2, 3, 4}; md2.y = {1, 2, 3, 4};
    md2.keep = {0, 1, 2, 3};
    md2.fe.push_back(make_factor("g", {0, 0, 1, 1}));
    int64_t d2 = drop_singletons_iv(md2);
    expect("no-singleton: dropped", d2, 0);
    expect("no-singleton: n unchanged", md2.n, 4);

    if (g_fail) { std::printf("FAILED (%d)\n", g_fail); return 1; }
    std::printf("PASSED\n");
    return 0;
}
