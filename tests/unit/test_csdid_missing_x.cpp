// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// csdid cell-sample rule under missing covariates — MI-129 regression gate.
//
// Stata's csdid drops rows with a missing covariate over the FULL panel
// (observed behavior, pinned vs the StataBE oracle) before pair-balancing, so a unit
// whose X is missing at EITHER endpoint row (base or t) of a (g,t) cell's
// window is dropped from that cell whole — even though dripw only ever *uses*
// the base-row X. cobra used to check X at the base row only, which kept units
// Stata drops: on kitchensink_panel (debug_all L152) that emitted e(N)=948 vs
// Stata 940 and diverged 13/21 ATT(g,t) cells (MSG-MI-129). This test pins the
// rule structurally (cell membership + the e(N) row union); the Stata-value
// parity for the kitchensink repro itself is Eval's golden.
//
// Panel: times {1,2,3}, 9 never-treated (g=0) + 8 treated (g=2), one covariate.
// Default (varying-base) cells for g=2: (base=1,t=2) and (base=1,t=3).
//   unit 0 (control): x missing ONLY at t=3 (a target-only row)
//       -> in cell (1,2) under both rules; dropped from (1,3) by the fix.
//   unit 1 (control): x missing at t=1 (the base row of both cells)
//       -> dropped from both cells under both old and new rules.
// Expected with the fix:  n_cell(1,2)=16, n_cell(1,3)=15,
//   n_obs_est = 15 complete units x 3 rows + unit0's {t1,t2} = 47.
// Under the pre-MI-129 rule this reads 16 / 16 / 48 (the test REDs there).
#include <cstdio>
#include <limits>
#include <vector>

#include "estimators/csdid.h"

using namespace cecon;

namespace {

int fails = 0;
void check(bool cond, const char* msg) {
    if (!cond) { std::printf("  FAIL: %s\n", msg); ++fails; }
    else       { std::printf("  ok  : %s\n", msg); }
}

}  // namespace

int test_csdid_missing_x_main() {
    std::printf("test_csdid_missing_x:\n");
    const double kNaN = std::numeric_limits<double>::quiet_NaN();

    std::vector<double> ivar, time, gvar, y, x;
    auto add = [&](int u, double t, double g, double yy, double xx) {
        ivar.push_back(u); time.push_back(t); gvar.push_back(g);
        y.push_back(yy); x.push_back(xx);
    };
    int u = 0;
    // 9 never-treated controls (g=0).
    for (int i = 0; i < 9; ++i, ++u) {
        for (double t = 1; t <= 3; ++t) {
            double xx = static_cast<double>((u + static_cast<int>(t)) % 2);
            if (u == 0 && t == 3) xx = kNaN;  // target-only missing (the MI-129 case)
            if (u == 1 && t == 1) xx = kNaN;  // base missing (dropped under both rules)
            add(u, t, 0.0, 0.4 * t + 0.6 * (u % 4) + 0.01 * ((u * 3 + static_cast<int>(t)) % 7), xx);
        }
    }
    // 8 treated at g=2 (complete data).
    for (int i = 0; i < 8; ++i, ++u) {
        for (double t = 1; t <= 3; ++t) {
            double bump = (t >= 2) ? 1.2 : 0.0;
            add(u, t, 2.0, 0.4 * t + 0.6 * (u % 4) + bump + 0.01 * ((u * 3 + static_cast<int>(t)) % 7),
                static_cast<double>((u + static_cast<int>(t)) % 2));
        }
    }
    const int64_t n = static_cast<int64_t>(ivar.size());

    CsdidConfig cfg;
    cfg.comparison = CsdidComparison::Never;
    auto R = csdid_dripw_run(ivar.data(), time.data(), gvar.data(), y.data(),
                             x.data(), 1, n, cfg);
    check(R.ok, "run ok");

    // Locate the two g=2 cells.
    int64_t n12 = -1, n13 = -1;
    bool ok12 = false, ok13 = false;
    for (const auto& ce : R.cells) {
        if (ce.g == 2.0 && ce.base == 1.0 && ce.t == 2.0) { n12 = ce.n_cell; ok12 = ce.ok; }
        if (ce.g == 2.0 && ce.base == 1.0 && ce.t == 3.0) { n13 = ce.n_cell; ok13 = ce.ok; }
    }
    check(ok12 && n12 == 16,
          "cell (base1,t2): unit0 kept (X complete on this window), unit1 dropped -> n_cell 16");
    check(ok13 && n13 == 15,
          "cell (base1,t3): unit0 ALSO dropped (X missing at target row) -> n_cell 15");
    check(R.n_obs_est == 47,
          "e(N) row union == 47 (15 complete units x 3 rows + unit0's 2; unit1 contributes 0)");

    std::printf("%s\n", fails ? "FAILED" : "PASSED");
    return fails ? 1 : 0;
}
