// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Ring-1 synthetic tests for HDFE demeaning and degrees-of-freedom accounting.
#include <cmath>
#include <cstdio>
#include <vector>

#include "core/model.h"
#include "hdfe/demean.h"
#include "hdfe/dof.h"

using namespace cecon;

static int g_fail = 0;
static void check(const char* what, double got, double want, double tol) {
    if (std::fabs(got - want) > tol) {
        std::printf("  FAIL %-22s got=%.10g want=%.10g\n", what, got, want);
        ++g_fail;
    } else {
        std::printf("  ok   %-22s = %.10g\n", what, got);
    }
}

static Factor mkf(std::vector<int32_t> codes, int K) {
    Factor f;
    f.codes = std::move(codes);
    f.n_levels = K;
    f.count.assign(K, 0.0);
    for (int32_t c : f.codes) f.count[c] += 1.0;
    return f;
}

int test_hdfe_main() {
    std::printf("test_hdfe:\n");

    // --- dof: connected 2-way (full 2x2 design) -> C=1, k_fe = 2+2-1 = 3 ---
    {
        Factor a = mkf({0, 0, 1, 1}, 2), b = mkf({0, 1, 0, 1}, 2);
        DofInfo d = compute_dof(4, 1, {a, b});
        check("dof connected k_fe", d.k_fe, 3, 0);
        check("dof connected resid", d.dof_resid, 4 - 1 - 3, 0);
    }
    // --- dof: disconnected 2-way (block diagonal) -> C=2, k_fe = 2+2-2 = 2 ---
    {
        Factor a = mkf({0, 0, 1, 1}, 2), b = mkf({0, 0, 1, 1}, 2);
        DofInfo d = compute_dof(4, 1, {a, b});
        check("dof disconnected k_fe", d.k_fe, 2, 0);
    }
    // --- dof: single FE -> k_fe = levels ---
    {
        Factor a = mkf({0, 0, 1, 1, 2}, 3);
        DofInfo d = compute_dof(5, 1, {a});
        check("dof single k_fe", d.k_fe, 3, 0);
    }
    // --- dof: 3 FE, reghdfe pairwise. c and b split into 2 components with each
    //     other, so c's redundancy is 2, not the 1 the old bound charged:
    //     a 1 level (M=0), b 2 levels (M=1), c 2 levels (M=max(1,1,2)=2) -> 2 ---
    {
        Factor a = mkf({0, 0, 0, 0}, 1), b = mkf({0, 0, 1, 1}, 2), c = mkf({0, 0, 1, 1}, 2);
        DofInfo d = compute_dof(4, 1, {a, b, c});
        check("dof pairwise3 k_fe", d.k_fe, 1 + 1 + 0, 0);
        check("dof pairwise3 inexact", d.fe_exact ? 1 : 0, 0, 0);
    }
    // --- absorbed_dof with cluster nesting: a nested drops all its levels, b (the
    //     first free effect) still loses one, c loses its 2 components with b:
    //     df_a = 0 + (2-1) + (2-2) = 1, df_a_nested = a's 1 level ---
    {
        Factor a = mkf({0, 0, 0, 0}, 1), b = mkf({0, 0, 1, 1}, 2), c = mkf({0, 0, 1, 1}, 2);
        AbsorbedDof d = absorbed_dof({a, b, c}, {1, 0, 0});
        check("absorbed nested df_a", d.df_a, 1, 0);
        check("absorbed nested count", d.df_a_nested, 1, 0);
    }

    // --- demean by 1 FE: groups {0,1} mean 2, {2,3} mean 12 -> centered, sums 0 ---
    {
        std::vector<double> v = {1, 3, 10, 14};
        Factor a = mkf({0, 0, 1, 1}, 2);
        demean(v.data(), 4, 1, {a});
        check("demean1 v0", v[0], -1.0, 1e-12);
        check("demean1 v3", v[3], 2.0, 1e-12);
        check("demean1 grp0 sum", v[0] + v[1], 0.0, 1e-12);
        check("demean1 grp1 sum", v[2] + v[3], 0.0, 1e-12);
    }

    // --- demean by 2 FE on a connected 2x2 design: result orthogonal to BOTH FE
    // (all level sums ~0) ---
    {
        std::vector<double> v = {4, 1, 2, 7};  // rows: (a,b)=(0,0),(0,1),(1,0),(1,1)
        Factor a = mkf({0, 0, 1, 1}, 2), b = mkf({0, 1, 0, 1}, 2);
        demean(v.data(), 4, 1, {a, b});
        check("demean2 a0 sum", v[0] + v[1], 0.0, 1e-9);
        check("demean2 a1 sum", v[2] + v[3], 0.0, 1e-9);
        check("demean2 b0 sum", v[0] + v[2], 0.0, 1e-9);
        check("demean2 b1 sum", v[1] + v[3], 0.0, 1e-9);
    }

    if (g_fail) {
        std::printf("FAILED (%d)\n", g_fail);
        return 1;
    }
    std::printf("PASSED\n");
    return 0;
}
