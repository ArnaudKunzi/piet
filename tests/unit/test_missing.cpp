// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Stata missing-value semantics — the 27 missings (`.` plus `.a`..`.z`), their
// total order, kind-exact equality, and the expression parser's literals.
//
// Every expectation below is an ORACLE value captured from StataBE 19.5 on the
// P1 wave fixture (internal fixture wave_fixture.dta,
// whose `y` column holds 22 `.a` cells among 4,800 rows), 2026-08-04:
//
//     count if y==.a  → 22     count if y==.   → 0      count if y==.b → 0
//     count if y>=.   → 22     count if y>.    → 22     count if y<.   → 4778
//     count if missing(y) → 22
//     display (. < .a) → 1     (.a < .b) → 1            (5 < .) → 1
//     display (.a + 1) → .     ((.a+1) == .) → 1
//
// The frame-level legs run the same expressions through the real expression
// evaluator over a synthetic frame, so a regression in either the parser or the
// comparison path fails here without needing the .dta fixture corpus.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "data/frame.h"
#include "data/missing.h"

using namespace cecon;

static int fail = 0;
static void check(bool cond, const std::string& msg) {
    if (!cond) {
        std::printf("  FAIL: %s\n", msg.c_str());
        fail = 1;
    }
}

// Count rows satisfying `expr` through the real expression evaluator: generate
// the predicate as a column, then count its true rows. (A copy of the frame is
// used so each probe is independent.)
static int64_t count_if(const Frame& base, const std::string& expr) {
    Frame fr = base;
    fr.generate("__probe", "(" + expr + ")");
    const auto& c = fr.col("__probe").f64;
    int64_t n = 0;
    for (std::size_t i = 0; i < c.size(); ++i)
        if (!std::isnan(c[i]) && c[i] != 0.0) ++n;
    return n;
}

// The generated column's raw values, for the arithmetic/kind probes.
static std::vector<double> gen_values(const Frame& base, const std::string& expr) {
    Frame fr = base;
    fr.generate("__probe", expr);
    const auto& c = fr.col("__probe").f64;
    return std::vector<double>(c.begin(), c.end());
}

int test_missing_main() {
    std::printf("test_missing:\n");

    // --- 1. Representation: kinds round-trip, and only OUR bit patterns decode.
    check(missing_kind(1.0) == -1, "a finite value has no missing kind");
    check(missing_kind(0.0) == -1, "zero has no missing kind");
    check(missing_kind(std::numeric_limits<double>::quiet_NaN()) == kMissSys,
          "a plain library quiet NaN reads as system missing `.`");
    for (int k = kMissSys; k <= kMissZ; ++k) {
        double v = stata_missing(k);
        check(std::isnan(v), "stata_missing(" + std::to_string(k) + ") is a NaN");
        check(missing_kind(v) == k,
              "kind " + std::to_string(k) + " round-trips through the f64 carrier");
    }
    check(missing_kind(stata_missing(99)) == kMissSys, "an out-of-range kind collapses to `.`");
    check(std::string(missing_text(kMissSys)) == "." && std::string(missing_text(kMissA)) == ".a" &&
              std::string(missing_text(kMissZ)) == ".z",
          "missing_text renders `.`/`.a`/`.z`");
    check(missing_letter(kMissA) == 'a' && missing_letter(kMissZ) == 'z' &&
              missing_letter(kMissSys) == '\0',
          "missing_letter maps kinds to their letters");

    // --- 2. Total order: every real < `.` < `.a` < … < `.z` (Stata oracle).
    check(stata_cmp(5.0, stata_missing(kMissSys)) < 0, "(5 < .) is true");
    check(stata_cmp(stata_missing(kMissSys), stata_missing(kMissA)) < 0, "(. < .a) is true");
    check(stata_cmp(stata_missing(kMissA), stata_missing(kMissA + 1)) < 0, "(.a < .b) is true");
    check(stata_cmp(stata_missing(kMissA), stata_missing(kMissA)) == 0, "(.a == .a) is true");
    check(stata_cmp(stata_missing(kMissA), stata_missing(kMissSys)) > 0, "(.a > .) is true");
    check(stata_cmp(1.0, 2.0) < 0 && stata_cmp(2.0, 1.0) > 0 && stata_cmp(2.0, 2.0) == 0,
          "two finite values compare as ordinary doubles (unchanged fast path)");

    // --- 3. The expression evaluator over a frame whose `y` mirrors the P1
    //        fixture's shape: 22 `.a` cells, no other missings, 4,800 rows.
    Frame fr;
    {
        std::vector<double> y(4800);
        for (std::size_t i = 0; i < y.size(); ++i) y[i] = static_cast<double>(i);
        for (int j = 0; j < 22; ++j) y[static_cast<std::size_t>(j) * 211] = stata_missing(kMissA);
        fr.set_n_rows_hint(4800);
        fr.append_f64_column("y", y);
        fr.finalize_after_bulk_load();
    }
    check(count_if(fr, "y==.a") == 22, "count if y==.a  → 22 (Stata oracle)");
    check(count_if(fr, "y==.") == 0, "count if y==.   → 0  (equality is kind-exact)");
    check(count_if(fr, "y==.b") == 0, "count if y==.b  → 0");
    check(count_if(fr, "y>=.") == 22, "count if y>=.   → 22 (the any-missing idiom)");
    check(count_if(fr, "y>.") == 22, "count if y>.    → 22 (.a sorts above .)");
    check(count_if(fr, "y<.") == 4778, "count if y<.    → 4778");
    check(count_if(fr, "missing(y)") == 22, "count if missing(y) → 22 (kind-blind)");
    check(count_if(fr, "y!=.a") == 4778, "count if y!=.a  → 4778");

    // --- 4. Arithmetic collapses any missing back to plain `.` (Stata oracle:
    //        `.a + 1` displays `.` and `(.a+1)==.` is 1).
    {
        std::vector<double> sum = gen_values(fr, "y + 1");
        int64_t sysmiss = 0, extmiss = 0;
        for (double v : sum) {
            int k = missing_kind(v);
            if (k == kMissSys) ++sysmiss;
            else if (k > kMissSys) ++extmiss;
        }
        check(sysmiss == 22 && extmiss == 0,
              "`.a + 1` is plain `.` — arithmetic never carries an extended code");
    }
    check(count_if(fr, "(y + 1) == .") == 22, "count if (y+1)==. → 22 (the collapse is observable)");

    // --- 5. Literal parsing: `.a`..`.z` only for a lone letter after the dot.
    {
        std::vector<double> all = gen_values(fr, ".z == .z");
        check(!all.empty() && all[0] == 1.0, "`.z == .z` parses and is true");
    }
    {
        bool threw = false;
        try {
            (void)gen_values(fr, "y == .abc");
        } catch (const std::exception&) {
            threw = true;
        }
        check(threw, "`.abc` is not a missing literal — it stays a parse error");
    }

    // --- 6. `!`, `&`, `|` read a missing operand as TRUE, and their result is
    //        always 0/1 (BUG-620). Oracle: tests/eval/golden/aer_w2/w2c_functions.log
    //        section operators_missing, fixture x = . 0 3 .a 5, y = 1 . 0 2 . :
    //        op_not_x 0 1 0 0 0 · op_x_and_1 1 0 1 1 1 · op_x_or_0 1 0 1 1 1 ·
    //        op_x_and_y 1 0 0 1 1 · op_x_or_y 1 1 1 1 1 · op_not_x_gt4 0 1 1 0 0 ·
    //        op_keep_x_and_1 _N 4 · op_drop_x_or_0 _N 1 · op_disp_not_dot 0 ·
    //        op_disp_dot_and1 1 · op_disp_dot_and0 0 · op_disp_dot_or0 1 ·
    //        op_disp_0_or_dot 1 · op_disp_dota_and1 1.
    //        The main binary gave `!x` = . on the missing rows and 0 for every
    //        missing `&`/`|` operand (keep if x & 1 kept 2 rows, not 4).
    {
        Frame lf;
        const double M = stata_missing(kMissSys), MA = stata_missing(kMissA);
        lf.set_n_rows_hint(5);
        lf.append_f64_column("x", {M, 0.0, 3.0, MA, 5.0});
        lf.append_f64_column("y", {1.0, M, 0.0, 2.0, M});
        lf.finalize_after_bulk_load();
        struct Arm {
            const char* tag;
            const char* expr;
            std::vector<double> want;
        };
        const std::vector<Arm> arms = {
            {"op_not_x", "!x", {0, 1, 0, 0, 0}},
            {"op_x_and_1", "x & 1", {1, 0, 1, 1, 1}},
            {"op_x_or_0", "x | 0", {1, 0, 1, 1, 1}},
            {"op_x_and_y", "x & y", {1, 0, 0, 1, 1}},
            {"op_x_or_y", "x | y", {1, 1, 1, 1, 1}},
            {"op_not_x_gt4", "!(x>4)", {0, 1, 1, 0, 0}},
            {"op_disp_not_dot", "!.", {0, 0, 0, 0, 0}},
            {"op_disp_dot_and1", ". & 1", {1, 1, 1, 1, 1}},
            {"op_disp_dot_and0", ". & 0", {0, 0, 0, 0, 0}},
            {"op_disp_dot_or0", ". | 0", {1, 1, 1, 1, 1}},
            {"op_disp_0_or_dot", "0 | .", {1, 1, 1, 1, 1}},
            {"op_disp_dota_and1", ".a & 1", {1, 1, 1, 1, 1}},
        };
        for (const Arm& a : arms) {
            // The plain expression takes ev_vec(); `* (_n > 0)` (always 1) makes
            // the tree non-vectorizable, so the per-row ev() answers too.
            const std::vector<double> vec = gen_values(lf, a.expr);
            const std::vector<double> row = gen_values(lf, std::string("(") + a.expr + ") * (_n > 0)");
            check(vec == a.want, std::string(a.tag) + ": ev_vec `" + a.expr + "` matches the oracle");
            check(row == a.want, std::string(a.tag) + ": ev `" + a.expr + "` matches the oracle");
            // keep_if on the same predicate keeps exactly the oracle's true rows.
            int64_t want_kept = 0;
            for (double w : a.want) want_kept += (w != 0.0) ? 1 : 0;
            Frame kf = lf;
            check(kf.keep_if(a.expr) == want_kept && kf.n_rows() == want_kept,
                  std::string(a.tag) + ": keep if " + a.expr + " keeps " + std::to_string(want_kept));
        }
        // op_drop_x_or_0: `drop if x | 0` leaves 1 row -- the drop complement
        // `(EXP)==0` over the new 0/1 result.
        {
            Frame kf = lf;
            check(kf.keep_if("(x | 0) == 0") == 1, "op_drop_x_or_0: drop if x | 0 leaves _N 1");
        }
    }

    std::printf(fail ? "test_missing: FAILED\n" : "test_missing: OK\n");
    return fail;
}
