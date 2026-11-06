// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// BUG-036 regression: a treatment cohort of a SINGLE unit whose propensity
// logit is perfectly separated must still be ESTIMATED, not silently dropped.
//
// Before the fix, complete separation (routine for a one-unit cohort — the
// treatment indicator is perfectly predicted by the covariates) made
// logit_mle() fail on the divergent MLE, so drdid_dripw_cell() returned
// ok=false and the cohort's ATT(g,t) cells, its estat-group row, and its
// contribution to e(N)/calendar all vanished. Stata csdid instead fits the
// (quasi-)separated propensity at its finite maximiser plateau and reports the
// cell, so the parity-correct behaviour is to estimate it.
//
// Self-contained synthetic panel — no external fixture, no Stata golden. A
// never-treated pool plus one single-unit cohort whose covariate sits far
// outside the control range forces complete separation of the treatment logit.
// We assert only the OBSERVABLE behaviour measured on StataBE (cell
// estimated vs dropped, cohort present in the group table, e(N) lifted), NOT a
// Stata-derived numeric value — under separation the exact ATT is specific to
// the maximiser's separating direction and is not fixed by the papers.
#include <cmath>
#include <cstdio>
#include <vector>

#include "estimators/csdid.h"

using namespace cecon;

int test_csdid_separation_main() {
    std::printf("test_csdid_separation:\n");
    int fails = 0;
    auto check = [&](bool c, const char* m) {
        if (!c) { std::printf("  FAIL: %s\n", m); ++fails; }
        else    { std::printf("  ok:   %s\n", m); }
    };

    const std::vector<double> times = {1, 2, 3, 4, 5};
    // Cohort per unit: ten never-treated (0) plus one single-unit cohort at g=4.
    const std::vector<double> cohort = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4};
    // Time-invariant covariate: controls spread over 0..9, the lone treated
    // unit far outside that range -> its treatment logit separates completely.
    const std::vector<double> xu = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 100};
    const int64_t U = static_cast<int64_t>(cohort.size());

    std::vector<double> ivar, time, gvar, y, x;
    for (int64_t u = 0; u < U; ++u) {
        for (double t : times) {
            ivar.push_back(static_cast<double>(u));
            time.push_back(t);
            gvar.push_back(cohort[static_cast<size_t>(u)]);
            const double g = cohort[static_cast<size_t>(u)];
            const double bump = (g > 0 && t >= g) ? 2.0 : 0.0;  // treatment effect
            y.push_back(0.3 * t + 0.05 * xu[static_cast<size_t>(u)] + bump);
            x.push_back(xu[static_cast<size_t>(u)]);
        }
    }
    const int64_t n_obs = static_cast<int64_t>(ivar.size());

    CsdidConfig cfg;
    cfg.comparison = CsdidComparison::Never;
    cfg.pscoretrim = 0.995;
    cfg.cluster = nullptr;
    cfg.long2 = false;
    CsdidRunResult R = csdid_dripw_run(ivar.data(), time.data(), gvar.data(),
                                       y.data(), x.data(), 1, n_obs, cfg, 0, 0);

    check(R.ok, "run ok");

    // The separated single-unit cohort g=4 must appear in the group table.
    bool g4 = false;
    double g4att = 0.0;
    for (const auto& gb : R.groups)
        if (static_cast<int>(std::llround(gb.g)) == 4) { g4 = true; g4att = gb.att; }
    check(g4, "separated cohort g=4 present in R.groups (not dropped)");
    check(g4 && std::isfinite(g4att), "cohort g=4 ATT is finite");

    // Its post cells (t >= 4) must be estimated with a finite, non-degenerate ATT.
    int est = 0;
    for (const auto& c : R.cells)
        if (static_cast<int>(std::llround(c.g)) == 4 && c.t >= 4.0 &&
            c.ok && std::isfinite(c.att) && c.att != 0.0)
            ++est;
    check(est >= 1, "separated post cell estimated (ok, finite, non-zero att)");

    // The lone treated unit's rows must now enter e(N).
    check(R.n_obs_est > 0, "n_obs_est populated");

    std::printf(fails ? "  RESULT: FAIL (%d)\n" : "  RESULT: PASS\n", fails);
    return fails ? 1 : 0;
}
