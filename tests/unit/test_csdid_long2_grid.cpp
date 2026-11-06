// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// csdid long2 filled-grid + (g - gdelta) base — deterministic structural gate.
//
// The main csdid golden (test_csdid) exercises only the varying-base (non-long2)
// path on a contiguous panel. This test pins the long2 grid/base fix
// (csdid.cpp: filled range(min,max,gdelta) grid + arithmetic base = g - gdelta)
// that closes the gapped-panel parity gap (defdefprop / reddefgkpfreq). It needs
// no external Stata golden: it asserts the observable grid (R.tlev) and per-cell
// bases (ce.base) that the fix must produce, plus a byte-identical no-op proof on
// a contiguous companion panel.
//
// Panels (tiny, synthetic):
//   GAPPED     times {1,2,3,5,6}  (single gap at t=4), cohort g=5.
//              gdelta = min step = 1; filled long2 grid = {1,2,3,4,5,6}; the base
//              for g=5 is g-gdelta = 4 (ABSENT) — old code wrongly used t=3.
//   CONTIGUOUS times {1,2,3,4,5,6}, cohort g=5. Filled grid == observed; base
//              g-gdelta = 4 is present. The fix is a strict no-op here.
#include <cmath>
#include <cstdio>
#include <vector>

#include "estimators/csdid.h"

using namespace cecon;

namespace {

// Build a balanced panel: for each unit u in [0,n_units), one row per time in
// `times`. gvar[u] assigns the cohort. y is arbitrary-but-deterministic (the
// structural asserts below do not depend on the outcome values). Returns SoA
// arrays via out-params (column-major single covariate).
struct Panel {
    std::vector<double> ivar, time, gvar, y, x;
    int64_t n_obs = 0;
};

Panel make_panel(const std::vector<double>& times,
                 const std::vector<double>& cohort_of_unit) {
    Panel p;
    const int64_t U = static_cast<int64_t>(cohort_of_unit.size());
    for (int64_t u = 0; u < U; ++u) {
        for (double t : times) {
            p.ivar.push_back(static_cast<double>(u));
            p.time.push_back(t);
            p.gvar.push_back(cohort_of_unit[static_cast<size_t>(u)]);
            // Deterministic outcome with a treatment-ish bump after g.
            double g = cohort_of_unit[static_cast<size_t>(u)];
            double treated_post = (g > 0 && t >= g) ? 1.5 : 0.0;
            p.y.push_back(0.3 * t + 0.7 * static_cast<double>(u % 3) + treated_post
                          + 0.01 * static_cast<double>((static_cast<int>(t) * 7 + u) % 5));
            p.x.push_back(static_cast<double>((u + static_cast<int64_t>(t)) % 2));
        }
    }
    p.n_obs = static_cast<int64_t>(p.ivar.size());
    return p;
}

bool approx(double a, double b, double tol = 1e-9) { return std::fabs(a - b) <= tol; }

int fails = 0;
void check(bool cond, const char* msg) {
    if (!cond) { std::printf("  FAIL: %s\n", msg); ++fails; }
    else       { std::printf("  ok  : %s\n", msg); }
}

}  // namespace

int test_csdid_long2_grid_main() {
    std::printf("test_csdid_long2_grid:\n");

    // Mix of treated (g=5) and never-treated (g=0) units so drdid cells can run.
    // 8 never-treated + 8 treated-at-5 gives enough rows per cell (p+2 guard).
    std::vector<double> cohorts;
    for (int i = 0; i < 8; ++i) cohorts.push_back(0.0);   // never treated
    for (int i = 0; i < 8; ++i) cohorts.push_back(5.0);   // cohort g=5

    // ---- 1. GAPPED panel: times {1,2,3,5,6}, gap at t=4 --------------------
    {
        Panel p = make_panel({1, 2, 3, 5, 6}, cohorts);
        CsdidConfig cfg;
        cfg.comparison = CsdidComparison::Never;
        cfg.long2 = true;
        auto R = csdid_dripw_run(p.ivar.data(), p.time.data(), p.gvar.data(),
                                 p.y.data(), p.x.data(), 1, p.n_obs, cfg);
        check(R.ok, "gapped: run ok");

        // gdelta = 1 → filled long2 grid must be the REGULAR range {1..6},
        // i.e. it must INCLUDE the unobserved t=4 (this is the grid-fill fix).
        std::vector<double> want_grid = {1, 2, 3, 4, 5, 6};
        bool grid_ok = (R.tlev.size() == want_grid.size());
        for (size_t i = 0; grid_ok && i < want_grid.size(); ++i)
            grid_ok = approx(R.tlev[i], want_grid[i]);
        check(grid_ok, "gapped: long2 tlev == filled range {1,2,3,4,5,6} (includes gap t=4)");

        // Base for cohort g=5 must be g - gdelta = 4 (arithmetic), NOT the largest
        // observed time < 5 (which was t=3 under the old code). Every cell of the
        // g=5 cohort carries base==4.
        bool base_ok = !R.cells.empty();
        bool saw_g5 = false;
        for (const auto& ce : R.cells) {
            if (approx(ce.g, 5.0)) {
                saw_g5 = true;
                if (!approx(ce.base, 4.0)) base_ok = false;
            }
        }
        check(saw_g5 && base_ok, "gapped: cohort g=5 base == g-gdelta == 4 (not observed t=3)");

        // Since t=4 is absent from the data, no unit has a base-period row, so
        // every g=5 cell blanks out (n_cell==0, ok==false) — the correct
        // Stata `_blank_` behavior when g-gdelta is empty. (Under the OLD code the
        // base would have been t=3, which IS present, so cells would wrongly run.)
        bool all_g5_blank = true;
        for (const auto& ce : R.cells)
            if (approx(ce.g, 5.0) && ce.ok) all_g5_blank = false;
        check(all_g5_blank, "gapped: g=5 cells blank (base t=4 absent) — matches Stata _blank_");
    }

    // ---- 2. CONTIGUOUS companion: times {1..6}, no gap ---------------------
    // The fix must be a strict no-op: filled grid == observed, base still 4.
    {
        Panel p = make_panel({1, 2, 3, 4, 5, 6}, cohorts);
        CsdidConfig cfg;
        cfg.comparison = CsdidComparison::Never;
        cfg.long2 = true;
        auto R = csdid_dripw_run(p.ivar.data(), p.time.data(), p.gvar.data(),
                                 p.y.data(), p.x.data(), 1, p.n_obs, cfg);
        check(R.ok, "contiguous: run ok");

        std::vector<double> want_grid = {1, 2, 3, 4, 5, 6};
        bool grid_ok = (R.tlev.size() == want_grid.size());
        for (size_t i = 0; grid_ok && i < want_grid.size(); ++i)
            grid_ok = approx(R.tlev[i], want_grid[i]);
        check(grid_ok, "contiguous: long2 tlev == observed {1..6} (no-op)");

        bool base_ok = true, saw_g5 = false, g5_ran = false;
        for (const auto& ce : R.cells)
            if (approx(ce.g, 5.0)) {
                saw_g5 = true;
                if (!approx(ce.base, 4.0)) base_ok = false;
                if (ce.ok) g5_ran = true;   // base t=4 present → cells run
            }
        check(saw_g5 && base_ok, "contiguous: cohort g=5 base == 4 (present)");
        check(g5_ran, "contiguous: g=5 cells run (base t=4 present, not blanked)");
    }

    // ---- 3. Exact-zero cell drop (Stata csdid_estat b[k]!=0) ---------------
    // A group-time cell whose ATT(g,t) is exactly 0.0 (a degenerate drdid fit —
    // in practice the last-cohort/last-period cell of a long2 design) must be
    // excluded from every aggregation, exactly as Stata's csdid_estat gates each
    // cell on `b[k]!=0`. This is the mechanism behind the 4-depvar long2 sweep
    // failures (reddefgkpfreq/defdefprop/goalfreq_agk/goalshare_agk): the phantom
    // all-blank event-times and the diluted shared event-times were all cells whose
    // ATT was exactly 0 that cobra used to include.
    //
    // Panel: never-treated g=0, cohort g=3, cohort g=4. Integer outcomes so the
    // long differences are bit-exact.
    //   - g=4 at its contemporaneous period t=4 (event e=0) has the SAME y4-y3
    //     shift as the controls  =>  att(g4,t4) == 0.0 exactly (degenerate cell).
    //   - g=3 at its contemporaneous period t=3 (event e=0) has a real jump
    //     =>  att(g3,t3) != 0.
    // The e=0 event (Tp0) is therefore shared between a real cell (g3) and an
    // exact-zero cell (g4). With the fix, Tp0 == att(g3,t3) alone (un-diluted);
    // without it, Tp0 would be the weight-mixed average of {att(g3,t3), 0}.
    {
        std::vector<double> ivar, time, gvar, y;
        int u = 0;
        auto add = [&](int uu, double t, double g, double yy) {
            ivar.push_back(uu); time.push_back(t); gvar.push_back(g); y.push_back(yy);
        };
        // Controls (g=0): integer y, y4-y3 == 5 exactly for every unit.
        for (int i = 0; i < 10; ++i, ++u) { add(u,1,0,i); add(u,2,0,i+1); add(u,3,0,i+2); add(u,4,0,i+7); }
        // g=4: y4-y3 == 5 exactly too (identical to controls) -> att(g4,t4)==0.
        for (int i = 0; i < 10; ++i, ++u) { add(u,1,4,i); add(u,2,4,i+1); add(u,3,4,i+2); add(u,4,4,i+7); }
        // g=3: real +18 jump at t>=3 -> att(g3,t3) != 0.
        for (int i = 0; i < 10; ++i, ++u) { add(u,1,3,i); add(u,2,3,i+1); add(u,3,3,i+20); add(u,4,3,i+25); }
        int64_t n = static_cast<int64_t>(ivar.size());

        CsdidConfig cfg; cfg.comparison = CsdidComparison::Never; cfg.long2 = true;
        auto R = csdid_dripw_run(ivar.data(), time.data(), gvar.data(), y.data(),
                                 nullptr, 0, n, cfg);
        check(R.ok, "zerocell: run ok");

        // The (g=4, t=4) cell must be a genuine exact-zero contributor: ok==true
        // (drdid ran) but att bit-exactly 0.0.
        bool found_zero = false, zero_is_exact = false;
        double att_g3t3 = 0; bool found_g3t3 = false;
        for (const auto& ce : R.cells) {
            if (approx(ce.g, 4.0) && approx(ce.t, 4.0)) {
                found_zero = true;
                zero_is_exact = (ce.att == 0.0) && ce.ok;
            }
            if (approx(ce.g, 3.0) && approx(ce.t, 3.0)) {
                found_g3t3 = true; att_g3t3 = ce.att;
            }
        }
        check(found_zero && zero_is_exact,
              "zerocell: (g4,t4) is an exact-0.0 ATT cell with ok==true");
        check(found_g3t3 && att_g3t3 != 0.0,
              "zerocell: (g3,t3) is a real non-zero ATT cell");

        // Tp0 (event e==0) must equal att(g3,t3) ALONE — the exact-zero (g4,t4)
        // cell is dropped from the aggregation, so no dilution.
        bool tp0_ok = false;
        for (const auto& ev : R.events)
            if (ev.e == 0) tp0_ok = approx(ev.att, att_g3t3, 1e-9);
        check(tp0_ok, "zerocell: Tp0 == att(g3,t3) alone (exact-0.0 g4 cell dropped, no dilution)");

        // No emitted event-time may carry an att of exactly 0.0 (a phantom
        // all-blank / all-zero event time must not be enumerated at all).
        bool no_phantom = true;
        for (const auto& ev : R.events)
            if (ev.att == 0.0) no_phantom = false;
        check(no_phantom, "zerocell: no phantom event-time with att==0.0 is emitted");
    }

    std::printf("%s\n", fails ? "FAILED" : "PASSED");
    return fails ? 1 : 0;
}
