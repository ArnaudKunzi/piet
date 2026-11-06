// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// BUG-369 — time-series operators INSIDE an expression (`gen ly = L.y`).
//
// The operators themselves were never in doubt: ts_operators.h has been driving
// `regress y L.x` (via model.cpp's ts_prepass) and `cobra tsops` for a long time.
// What was missing was the wiring into Frame's expression parser, whose
// identifier scan stopped at '.', so `L.y` decomposed into a bare `L` and failed
// as an unknown variable. This test pins the wiring AND the three semantics that
// a naive "previous row" implementation would get wrong.
//
// Fixture — deliberately adversarial on all three:
//   id t   y      L.y   D.y   F.y
//   1  1   10      .     .    20     <- first period of panel 1: no predecessor
//   1  2   20     10    10     .     <- successor t=3 does NOT exist (gap)
//   1  4   40      .     .     .     <- GAP: t=3 is absent, so L.y is MISSING,
//                                        NOT 20. This is the whole point: the
//                                        operator indexes by TIME, not by row.
//   2  1  100      .     .   200     <- PANEL BOUNDARY: must not reach back into
//                                        panel 1's last row (40)
//   2  2  200    100   100   300
//   2  3  300    200   100     .
//
// A row-based lag would give L.y[row 2] = 20 and L.y[row 3] = 40; both are wrong
// and both are what this test exists to catch.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "core/error.h"
#include "data/frame.h"

using namespace cecon;

static int g_fail = 0;

static void checkcol(const char* what, const Frame& fr, const std::string& col,
                     const std::vector<double>& want) {
    const auto& got = fr.col(col).f64;
    if (static_cast<int64_t>(got.size()) != static_cast<int64_t>(want.size())) {
        std::printf("  FAIL %-28s size got=%zu want=%zu\n", what, got.size(), want.size());
        ++g_fail;
        return;
    }
    for (size_t i = 0; i < want.size(); ++i) {
        const bool wantNan = std::isnan(want[i]);
        const bool gotNan = std::isnan(got[i]);
        if (wantNan != gotNan || (!wantNan && std::fabs(got[i] - want[i]) > 1e-12)) {
            std::printf("  FAIL %-28s row %zu got=%.12g want=%.12g\n", what, i, got[i], want[i]);
            ++g_fail;
            return;
        }
    }
    std::printf("  ok   %-28s\n", what);
}

static void checkthrows(const char* what, Frame& fr, const std::string& name,
                        const std::string& expr, const char* wantSubstr) {
    try {
        fr.generate(name, expr, false);
    } catch (const std::exception& e) {
        if (std::string(e.what()).find(wantSubstr) == std::string::npos) {
            std::printf("  FAIL %-28s wrong message: %s\n", what, e.what());
            ++g_fail;
            return;
        }
        std::printf("  ok   %-28s (honest error)\n", what);
        return;
    }
    std::printf("  FAIL %-28s did NOT throw\n", what);
    ++g_fail;
}

static Frame makePanel() {
    Frame fr;
    fr.set_n_rows_hint(6);
    fr.set_f64_column("id", {1, 1, 1, 2, 2, 2});
    fr.set_f64_column("t", {1, 2, 4, 1, 2, 3});
    fr.set_f64_column("y", {10, 20, 40, 100, 200, 300});
    return fr;
}

int test_ts_expr_main() {
    const double N = std::nan("");
    std::printf("[ts_expr] BUG-369 — L./F./D./S. inside gen/keepif expressions\n");

    // ---- 1. panel-aware, time-indexed, gap-aware --------------------------
    {
        Frame fr = makePanel();
        fr.xtset("id", "t");
        fr.generate("ly", "L.y", false);
        fr.generate("dy", "D.y", false);
        fr.generate("fy", "F.y", false);
        checkcol("L.y (gap + boundary)", fr, "ly", {N, 10, N, N, 100, 200});
        checkcol("D.y (gap + boundary)", fr, "dy", {N, 10, N, N, 100, 100});
        checkcol("F.y (gap + end)", fr, "fy", {20, N, N, 200, 300, N});
    }

    // ---- 2. composed and numbered operators, single time series -----------
    {
        Frame fr;
        fr.set_n_rows_hint(5);
        fr.set_f64_column("t", {1, 2, 3, 4, 5});
        fr.set_f64_column("y", {5, 7, 11, 13, 17});
        fr.tsset("t");
        fr.generate("l2", "L2.y", false);
        fr.generate("ld", "LD.y", false);   // L applied to D: D.y = {.,2,4,2,4}
        fr.generate("s2", "S2.y", false);   // y_t - y_{t-2}
        fr.generate("g", "(y-L.y)/L.y", false);  // the idiom the probe found broken
        checkcol("L2.y", fr, "l2", {N, N, 5, 7, 11});
        checkcol("LD.y", fr, "ld", {N, N, 2, 4, 2});
        checkcol("S2.y", fr, "s2", {N, N, 6, 6, 6});
        checkcol("(y-L.y)/L.y", fr, "g", {N, 2.0 / 5, 4.0 / 7, 2.0 / 11, 4.0 / 13});
    }

    // ---- 3. delta(): the step is in TIME UNITS, not rows -------------------
    {
        Frame fr;
        fr.set_n_rows_hint(5);
        fr.set_f64_column("t", {1, 2, 3, 4, 5});
        fr.set_f64_column("y", {5, 7, 11, 13, 17});
        fr.tsset("t");
        fr.set_ts_delta(2);
        fr.generate("l1", "L.y", false);
        checkcol("L.y under delta(2)", fr, "l1", {N, N, 5, 7, 11});
    }

    // ---- 4. no tsset is an HONEST ERROR, never a silent row-lag ------------
    {
        Frame fr = makePanel();
        checkthrows("L.y with no tsset", fr, "z", "L.y", "no time variable is set");
    }

    // ---- 5. the conservative fallback: a TS reading is taken ONLY when the
    //         prefix is pure L/F/D/S *and* the base column exists. A real column
    //         named `L` (or `d`, or `sd` — all spelled from operator letters)
    //         must keep resolving as itself, or this fix would silently break
    //         ordinary variable names.
    {
        Frame fr;
        fr.set_n_rows_hint(3);
        fr.set_f64_column("t", {1, 2, 3});
        fr.set_f64_column("y", {5, 7, 11});
        fr.set_f64_column("L", {100, 200, 300});
        fr.set_f64_column("d", {2, 4, 6});
        fr.set_f64_column("sd", {1.5, 2.5, 3.5});
        fr.tsset("t");
        fr.generate("a", "L", false);        // the COLUMN L, not an operator
        fr.generate("b", "d+sd", false);     // columns spelled from TS letters
        fr.generate("c", "L.y", false);      // still the operator, same frame
        checkcol("column named L", fr, "a", {100, 200, 300});
        checkcol("columns d + sd", fr, "b", {3.5, 6.5, 9.5});
        checkcol("L.y alongside column L", fr, "c", {N, 5, 7});
        // With a real column named `L` in scope, an unresolvable `L.nosuch` is
        // genuinely ambiguous, and the fallback reads it as the column followed by
        // junk. Pinned so the choice is deliberate rather than incidental.
        checkthrows("L.nosuch when column L exists", fr, "z", "L.nosuch", "trailing input");
    }

    // ---- 7. diagnostics name the BASE variable, not the operator letter ----
    //         Before the fix `L.y` reported "unknown variable L", pointing at a
    //         token the user never wrote as a variable.
    {
        Frame fr = makePanel();
        fr.xtset("id", "t");
        checkthrows("L.nosuch names the base", fr, "z", "L.nosuch", "unknown variable nosuch");
        checkthrows("Q.y is not an operator", fr, "z", "Q.y", "unknown variable Q");
    }

    // ---- 6. keep_if runs the same parser, so `keep if L.y > 6` works -------
    {
        Frame fr;
        fr.set_n_rows_hint(4);
        fr.set_f64_column("t", {1, 2, 3, 4});
        fr.set_f64_column("y", {5, 7, 11, 13});
        fr.tsset("t");
        fr.keep_if("L.y > 6");
        // L.y = {., 5, 7, 11}; keep_if keeps missing predicates (frame.cpp's
        // documented rule), so rows 0 (missing), 2 and 3 survive.
        checkcol("keep if L.y > 6", fr, "y", {5, 11, 13});
    }

    std::printf(g_fail == 0 ? "[ts_expr] ALL PASSED\n" : "[ts_expr] %d FAILURE(S)\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
