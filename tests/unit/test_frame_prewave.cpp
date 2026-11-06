// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// COUNCIL-20260710 pre-wave audit — correctness fixes in src/data/frame.cpp:
//   E2 (BUG-057) finite-or-missing evaluator: gen of an overflow/domain-error
//       expression (1/0, ln(0), exp(overflow), 0^(-1)) must store `.`
//       (missing), not a raw IEEE Inf/NaN that a plain isnan() check
//       downstream (summarize's N-count, keep_if, ...) fails to catch.
//   E3 (BUG-058) keep_if bare-truthy missing=TRUE: Stata's bare `if x` test
//       is `v != 0`, full stop — missing is represented as a value that is
//       never == 0, so missing rows are KEPT, not dropped. Relational `if
//       x>5` is a separate, already-correct code path (mi(), regression
//       guard here).
//   E4 (BUG-059) recode copy-through: `recode x (1=10)` with no else clause
//       must leave every unmatched value (including missing) unchanged,
//       not turn it into missing.
//   E8 save,replace via temp+rename: writing back onto the Frame's own
//       source path must not truncate-in-place; it streams a sibling temp
//       file from the still-valid source mmap and rename(2)s over the
//       source, byte-identical to a plain full rewrite of the same content.
// Hand-computed expected values match the Stata oracle collected for this
// sprint (internal note COUNCIL-20260710-prewave-audit/PLAYBOOK.md
// §5.E2/E3/E4/E8; oracle_E2_expredge.csv, oracle_E3_keepif.csv,
// oracle_E4_recode.csv in the sprint's eval intake).
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "data/frame.h"
#include "io/columnar.h"
#include "io/win_compat.h"

using namespace cecon;

int fail = 0;
static void check(bool cond, const std::string& msg) {
    if (!cond) { std::printf("  FAIL: %s\n", msg.c_str()); fail = 1; }
}
static bool eq(double a, double b, double tol = 1e-9) {
    if (std::isnan(a) && std::isnan(b)) return true;
    if (std::isnan(a) != std::isnan(b)) return false;
    return std::fabs(a - b) <= tol * (std::fabs(b) + 1.0);
}

static Frame make_frame(const std::vector<std::pair<std::string, std::vector<double>>>& cols, int64_t n) {
    Frame f;
    f.set_n_rows_hint(n);
    for (const auto& c : cols) f.append_f64_column(c.first, c.second);
    f.finalize_after_bulk_load();
    return f;
}

static std::vector<double> seq_vec(int64_t n) {
    std::vector<double> v(static_cast<size_t>(n));
    for (int64_t i = 0; i < n; ++i) v[static_cast<size_t>(i)] = static_cast<double>(i + 1);
    return v;
}

// ---------------------------------------------------------------------------
// E2 — finite-or-missing evaluator (BUG-057). All four probe expressions are
// constants (do not depend on row data); oracle_E2_expredge.csv pins every
// one to N=0 under Stata (mean is deliberately not golden-checked: Stata's
// r(mean) is undefined at N=0, see the oracle .do's note; separately cobra
// has an unrelated, out-of-sprint-scope "degenerate scalar" gap there).
// ---------------------------------------------------------------------------
static void test_e2_finite_or_missing() {
    std::printf("E2 finite-or-missing evaluator:\n");
    Frame f = make_frame({{"id", seq_vec(20)}}, 20);

    f.generate("z1", "1/0");
    f.generate("z2", "ln(0)");
    f.generate("z3", "exp(1000)");
    f.generate("z4", "0^(-1)");
    check(f.summarize("z1").n == 0, "gen z=1/0: summarize N==0 (was 20, counting a raw Inf as valid)");
    check(f.summarize("z2").n == 0, "gen z=ln(0): summarize N==0");
    check(f.summarize("z3").n == 0, "gen z=exp(1000): summarize N==0 (exp overflow)");
    check(f.summarize("z4").n == 0, "gen z=0^(-1): summarize N==0");
    for (int64_t i = 0; i < f.n_rows(); ++i) {
        check(std::isnan(f.col("z1").f64[i]), "1/0 stored as missing (NaN), not a raw Inf");
        check(std::isnan(f.col("z2").f64[i]), "ln(0) stored as missing (NaN), not a raw -Inf");
        check(std::isnan(f.col("z3").f64[i]), "exp(1000) stored as missing (NaN), not a raw Inf");
        check(std::isnan(f.col("z4").f64[i]), "0^(-1) stored as missing (NaN), not a raw Inf");
    }

    // The `(1/0)==.` incoherence: 1/0 must be genuinely missing, so this is
    // true for the right reason (both sides isnan-missing), not by
    // coincidentally colliding with the mi() sentinel's raw +Inf value.
    f.generate("eqmiss", "(1/0) == .");
    for (int64_t i = 0; i < f.n_rows(); ++i)
        check(eq(f.col("eqmiss").f64[i], 1.0), "(1/0) == . evaluates true (both sides genuinely missing)");

    // Ordinary finite arithmetic is a no-op under the isfinite wrap.
    f.generate("normal", "id * 2 + 1");
    check(f.summarize("normal").n == 20, "ordinary finite arithmetic unaffected: N==20, none wrongly dropped");
    check(eq(f.col("normal").f64[0], 3.0) && eq(f.col("normal").f64[19], 41.0),
          "ordinary finite arithmetic unaffected: values unchanged by the isfinite wrap");
}

// ---------------------------------------------------------------------------
// E3 — keep_if bare-truthy missing=TRUE (BUG-058). Fixture mirrors
// e3_keepif.csv / oracle_E3_keepif.csv exactly: x has 4 missing rows
// (positions 13-16) and one exact-zero row (position 4); every other value
// is a nonzero real number.
// ---------------------------------------------------------------------------
static void test_e3_keepif_missing_truthy() {
    std::printf("E3 keep_if bare-truthy missing=TRUE:\n");
    std::vector<double> x = {-3, -2, -1, 0, 1, 2, 3, 4, 6, 7, 8, 9,
                             NAN, NAN, NAN, NAN, 5, 5, 10, 10};
    check(static_cast<int64_t>(x.size()) == 20, "E3 fixture sanity: 20 rows");

    // (i) bare truthy: FALSE only at x==0 (row4); the 4 missing rows count
    // as TRUE alongside the 15 other nonzero rows => kept = 19 (was 15,
    // wrongly also dropping the 4 missing rows).
    {
        Frame f = make_frame({{"x", x}}, 20);
        int64_t kept = f.keep_if("x");
        check(kept == 19, "keep if x (bare truthy): returns 19");
        check(f.n_rows() == 19, "keep if x (bare truthy): n_rows == 19 after filtering");
    }

    // (ii) relational: missing sorts as +infinity, so x>5 is TRUE for the 4
    // missing rows plus {6,7,8,9,10,10} = 10 total. This path (mi()) is
    // untouched by the E3 fix — regression guard.
    {
        Frame f = make_frame({{"x", x}}, 20);
        int64_t kept = f.keep_if("x>5");
        check(kept == 10, "keep if x>5 (relational): returns 10, unaffected by the bare-truthy fix");
    }

    // (iii) ONESHOT-FIXMEM — a filter that keeps EVERY row. This case had no
    // coverage and is worth pinning on its own: 101 of the acceptance corpus's 308
    // `keep_if` calls keep every row, and the expensive ones are precisely those
    // (~51 copies of a 205-column × 105k-row frame per slice run).
    //
    // It was NOT made a fast path, and the reason is measured, not aesthetic: an
    // early return leaves the columns BORROWED from the source mmap, and on the
    // reference machine — data on an external volume, memory pressured — that costs
    // 58% wall serially and 87% under fan-out, with 13-20x the page faults, while
    // saving 25% maxRSS (instruments/KEEPIF-RESIDENCY-20260814.md §5). The copy is
    // not waste here; it is an implicit pin of the working set. Left as a copy, and
    // this leg guards the values through it.
    {
        Frame f = make_frame({{"x", x}}, 20);
        int64_t kept = f.keep_if("1");   // constant-truthy: keeps every row, missing included
        check(kept == 20, "keep-all: returns the full row count (20)");
        check(f.n_rows() == 20, "keep-all: n_rows unchanged");
        const FrameColumn& cx = f.col("x");
        bool same = true;
        for (int64_t i = 0; i < 20; ++i) {
            const double got = cx.f64[static_cast<size_t>(i)];
            const double want = x[static_cast<size_t>(i)];
            const bool bothNan = std::isnan(got) && std::isnan(want);
            if (!bothNan && got != want) same = false;
        }
        check(same, "keep-all: every value survives byte-for-byte (NaNs included)");
    }
}

// ---------------------------------------------------------------------------
// E4 — recode copy-through (BUG-059). `recode x (1=10)` with no else clause;
// fixture mirrors e4_recode.csv / oracle_E4_recode.csv: x in {1,2,3,.,5},
// four rows each. Only x==1 rows should change; 2, 3, 5, and missing must
// copy through unchanged. Covers both the gen() and in-place forms — both
// go through the exact same Frame::recode_apply (apps/cecon_cli.cpp's
// cmd_recode calls it once and either creates a new column or overwrites
// the source column with the identical result vector).
// ---------------------------------------------------------------------------
static void test_e4_recode_copy_through() {
    std::printf("E4 recode copy-through (unmatched values unchanged):\n");
    std::vector<double> x = {1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
                             NAN, NAN, NAN, NAN, 5, 5, 5, 5};
    std::vector<Frame::RecodeRule> rules = { {0, 1, 0, false, 10} };  // (1=10), no else/missing rule

    double expect[20] = {10, 10, 10, 10, 2, 2, 2, 2, 3, 3, 3, 3,
                         NAN, NAN, NAN, NAN, 5, 5, 5, 5};

    // gen() form.
    {
        Frame f = make_frame({{"x", x}}, 20);
        auto xr = f.recode_apply("x", rules);
        check(xr.size() == 20, "recode gen: output has 20 rows");
        bool values_ok = true;
        for (int i = 0; i < 20; ++i) if (!eq(xr[i], expect[i])) values_ok = false;
        check(values_ok, "recode gen: unmatched (2,3,5) and missing copy through unchanged, only 1->10");

        f.set_f64_column("xr", xr);
        Frame::Summary s = f.summarize("xr");
        check(s.n == 16, "recode gen: summarize(xr).N == 16 (was 4, treating unmatched as missing)");
        check(eq(s.mean, 5.0), "recode gen: summarize(xr).mean == 5.0 (was 10.0, degenerate)");
        check(eq(s.sd, 3.18328970301689), "recode gen: summarize(xr).sd matches Stata oracle");
    }

    // in-place form: identical recode_apply result, written back onto the
    // source column instead of a new one.
    {
        Frame f = make_frame({{"x", x}}, 20);
        auto result = f.recode_apply("x", rules);
        f.set_f64_column("x", result);
        Frame::Summary s = f.summarize("x");
        check(s.n == 16, "recode in-place: summarize(x).N == 16, same as gen()");
        check(eq(s.mean, 5.0), "recode in-place: summarize(x).mean == 5.0, same as gen()");
    }
}

// ---------------------------------------------------------------------------
// E8 — save,replace via temp+rename. write_hbin(path) where path == the
// Frame's own source .hbin used to force_own_all()+truncate-in-place; it now
// streams a sibling temp file from the still-valid source mmap and
// rename(2)s it over the source. Verifies round-trip correctness (including
// a mixed dirty+borrowed-column save) and byte-for-byte equivalence to an
// ordinary fresh write of the same logical content.
// ---------------------------------------------------------------------------
static void test_e8_save_replace_temp_rename() {
    std::printf("E8 save,replace via temp+rename:\n");
    // The save,replace target carries a Hangul leaf on purpose: the temp+replace
    // path has to resolve BOTH the sibling temp and the destination wide, so a
    // non-ASCII name is the regression gate for BUG-050 on that path. On POSIX it
    // is an ordinary UTF-8 name under /tmp — same code path, same bytes.
    const std::string p_replace = native_temp_path(u8"cecon_한글_prewave_e8_replace.hbin");
    const std::string p_reference = native_temp_path("cecon_test_prewave_e8_reference.hbin");
    native_unlink(p_replace);
    native_unlink(p_reference);

    std::vector<double> a = {1, 2, 3, 4, 5};
    std::vector<double> b = {10, 20, 30, 40, 50};

    // Seed p_replace via a plain (non-hbin-backed) write.
    {
        Frame f = make_frame({{"a", a}, {"b", b}}, 5);
        f.write_hbin(p_replace);
    }

    // Load it back (source_path() == p_replace, every column still borrowed
    // from p_replace's mmap) and save,replace it UNCHANGED -- exercises
    // streaming straight from the source mmap with nothing dirtied.
    {
        Dataset ds = Dataset::open(p_replace);
        Frame f = Frame::from_dataset(ds);
        check(f.source_path() == p_replace, "E8 setup: loaded Frame's source_path() == p_replace");
        f.write_hbin(p_replace);
    }

    // Round-trip correctness after the unchanged save,replace.
    {
        Dataset ds = Dataset::open(p_replace);
        Frame f = Frame::from_dataset(ds);
        check(f.n_rows() == 5, "E8 round-trip: row count intact after save,replace");
        bool a_ok = true, b_ok = true;
        for (int i = 0; i < 5; ++i) {
            if (!eq(f.col("a").f64[i], a[i])) a_ok = false;
            if (!eq(f.col("b").f64[i], b[i])) b_ok = false;
        }
        check(a_ok, "E8 round-trip: column a values intact");
        check(b_ok, "E8 round-trip: column b values intact");
    }

    // Byte-identical: an ordinary fresh write of the SAME logical content
    // (no source file, so this always takes the plain write_hbin_full path)
    // must produce byte-for-byte the same file as the save,replace above --
    // the target path/filename never affects header or data bytes.
    {
        Frame f2 = make_frame({{"a", a}, {"b", b}}, 5);
        f2.write_hbin(p_reference);
        std::ifstream f1s(to_native_path(p_replace), std::ios::binary);
        std::ifstream f2s(to_native_path(p_reference), std::ios::binary);
        std::vector<char> d1((std::istreambuf_iterator<char>(f1s)), std::istreambuf_iterator<char>());
        std::vector<char> d2((std::istreambuf_iterator<char>(f2s)), std::istreambuf_iterator<char>());
        check(!d1.empty() && d1.size() == d2.size(), "E8 byte-identical: file sizes match (and nonempty)");
        check(d1 == d2, "E8 byte-identical: save,replace output == fresh full-rewrite of same content");
    }

    // Mixed dirty+borrowed save: dirty column a via a real mutation, leave b
    // borrowed/clean, save,replace again, and confirm both persist correctly.
    {
        Dataset ds = Dataset::open(p_replace);
        Frame f = Frame::from_dataset(ds);
        f.generate("a", "a * 2");  // dirties column a; b stays borrowed from the mmap
        f.write_hbin(p_replace);

        Dataset ds2 = Dataset::open(p_replace);
        Frame f2 = Frame::from_dataset(ds2);
        bool a_ok = true, b_ok = true;
        for (int i = 0; i < 5; ++i) {
            if (!eq(f2.col("a").f64[i], a[i] * 2)) a_ok = false;
            if (!eq(f2.col("b").f64[i], b[i])) b_ok = false;
        }
        check(a_ok, "E8 mixed dirty+clean: dirtied column a doubled and persisted");
        check(b_ok, "E8 mixed dirty+clean: untouched borrowed column b still correct");
    }

    native_unlink(p_replace);
    native_unlink(p_reference);
}

int test_frame_prewave_main() {
    std::printf("test_frame_prewave:\n");
    test_e2_finite_or_missing();
    test_e3_keepif_missing_truthy();
    test_e4_recode_copy_through();
    test_e8_save_replace_temp_rename();
    std::printf("%s\n", fail ? "FAILED" : "PASSED");
    return fail;
}
