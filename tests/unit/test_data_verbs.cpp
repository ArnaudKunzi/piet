// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// MI-066 data verbs — Frame-level unit coverage for gsort/order/append/
// duplicates/recode/reshape/merge, plus the L1 M3 native-verb campaign's
// rename/clonevar/collapse(factor-by)/encode/setcol (whole-frame CSV
// round-trip retirement — see frame.h doc comments next to each). Hand-
// computed expected values (small, enumerable fixtures) mirroring the PIET
// pandas oracle semantics documented next to each Frame method in
// src/data/frame.h/.cpp. See also tools/validate_data_verbs.py for the
// differential harness vs the live pandas oracle on larger generated
// datasets.
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "data/frame.h"
#include "data/missing.h"  // BUG-272: stata_missing() for the gsort mint-refusal legs
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

// Return the output-frame factor label of column `col` at row r ("" = missing).
static std::string flabel(const Frame& f, const std::string& col, int64_t r) {
    const FrameColumn& c = f.col(col);
    int32_t code = c.codes[r];
    return code >= 0 ? c.levels[code] : std::string();
}

static void test_gsort() {
    std::printf("gsort:\n");
    Frame f = make_frame({{"g", {1, 1, 2, 2}}, {"v", {3, 1, 4, 2}}}, 4);
    Frame r = f.gsort({"g", "v"}, {true, false});  // ascending g, descending v within g
    // expect order: (g=1,v=3), (g=1,v=1), (g=2,v=4), (g=2,v=2)
    double expect_g[4] = {1, 1, 2, 2}, expect_v[4] = {3, 1, 4, 2};
    for (int i = 0; i < 4; ++i) {
        check(eq(r.col("g").f64[i], expect_g[i]), "gsort g order");
        check(eq(r.col("v").f64[i], expect_v[i]), "gsort v order");
    }
    // stability: equal keys keep original relative order
    Frame f2 = make_frame({{"k", {1, 1, 1}}, {"orig", {0, 1, 2}}}, 3);
    Frame r2 = f2.gsort({"k"}, {true});
    for (int i = 0; i < 3; ++i) check(eq(r2.col("orig").f64[i], i), "gsort stable on ties");

    // Missing (NaN) sorts LAST under BOTH directions — the contracted
    // np.lexsort semantics (0.18.0 Eval gate item 2; the maintainer ruling: frozen
    // 1.16.0 oracle behavior wins over Stata's missing-first-descending).
    Frame f3 = make_frame({{"v", {3, NAN, 1, 2}}}, 4);
    Frame r3d = f3.gsort({"v"}, {false});  // descending
    double expect_desc[4] = {3, 2, 1, NAN};
    for (int i = 0; i < 4; ++i) check(eq(r3d.col("v").f64[i], expect_desc[i]), "gsort desc: NaN sorts last");
    Frame r3a = f3.gsort({"v"}, {true});   // ascending
    double expect_asc[4] = {1, 2, 3, NAN};
    for (int i = 0; i < 4; ++i) check(eq(r3a.col("v").f64[i], expect_asc[i]), "gsort asc: NaN sorts last");
    // multi-key: NaN in the descending secondary key still sorts last within group
    Frame f4 = make_frame({{"g", {1, 1, 1, 2}}, {"v", {NAN, 5, 7, 1}}}, 4);
    Frame r4 = f4.gsort({"g", "v"}, {true, false});
    double eg[4] = {1, 1, 1, 2}, ev[4] = {7, 5, NAN, 1};
    for (int i = 0; i < 4; ++i) {
        check(eq(r4.col("g").f64[i], eg[i]), "gsort multi-key g order");
        check(eq(r4.col("v").f64[i], ev[i]), "gsort multi-key desc secondary: NaN last in group");
    }
}

static void test_order() {
    std::printf("order:\n");
    Frame f = make_frame({{"a", {1}}, {"b", {2}}, {"c", {3}}, {"d", {4}}}, 1);
    Frame r1 = f.order({"c"}, "first", "");
    std::vector<std::string> names1;
    for (const auto& c : r1.columns()) names1.push_back(c.name);
    check(names1 == std::vector<std::string>({"c", "a", "b", "d"}), "order first");

    Frame r2 = f.order({"a"}, "last", "");
    std::vector<std::string> names2;
    for (const auto& c : r2.columns()) names2.push_back(c.name);
    check(names2 == std::vector<std::string>({"b", "c", "d", "a"}), "order last");

    Frame r3 = f.order({"d"}, "before", "b");
    std::vector<std::string> names3;
    for (const auto& c : r3.columns()) names3.push_back(c.name);
    check(names3 == std::vector<std::string>({"a", "d", "b", "c"}), "order before(b)");

    Frame r4 = f.order({"a"}, "after", "b");
    std::vector<std::string> names4;
    for (const auto& c : r4.columns()) names4.push_back(c.name);
    check(names4 == std::vector<std::string>({"b", "a", "c", "d"}), "order after(b)");
}

static void test_append() {
    std::printf("append:\n");
    Frame m = make_frame({{"x", {1, 2}}, {"y", {10, 20}}}, 2);
    Frame u = make_frame({{"x", {3, 4}}, {"z", {100, 200}}}, 2);
    Frame r = m.append(u);
    check(r.n_rows() == 4, "append row count");
    check(r.has("x") && r.has("y") && r.has("z"), "append column union");
    check(eq(r.col("x").f64[0], 1) && eq(r.col("x").f64[3], 4), "append x values");
    check(std::isnan(r.col("y").f64[2]) && std::isnan(r.col("y").f64[3]), "append y missing in using-only rows");
    check(std::isnan(r.col("z").f64[0]) && std::isnan(r.col("z").f64[1]), "append z missing in master-only rows");
    check(eq(r.col("z").f64[2], 100) && eq(r.col("z").f64[3], 200), "append z values");
}

// L1b: factor+factor level-table union — master levels first, then using-only
// levels appended (frame.h Frame::append doc comment); using's own code for a
// level ALREADY in master's dictionary must remap onto master's EXISTING
// code, not get a fresh one.
static void test_append_factor_union() {
    std::printf("append (factor+factor level union):\n");
    Frame m, u;
    m.set_n_rows_hint(2);
    m.append_factor_column("k", {"a", "b"});
    m.append_f64_column("m", {1, 2});
    m.finalize_after_bulk_load();
    u.set_n_rows_hint(2);
    u.append_factor_column("k", {"b", "c"});  // "b" already in master's dict; "c" is new
    u.append_f64_column("u", {10, 20});
    u.finalize_after_bulk_load();

    Frame r = m.append(u);
    check(r.n_rows() == 4, "append factor union: row count");
    const FrameColumn& k = r.col("k");
    check(k.is_factor, "append factor union: OUTPUT k stays a factor");
    check(k.levels.size() == 3 && k.levels[0] == "a" && k.levels[1] == "b" && k.levels[2] == "c",
          "append factor union: master levels FIRST (a,b), then using-only levels appended (c)");
    check(flabel(r, "k", 0) == "a" && flabel(r, "k", 1) == "b" && flabel(r, "k", 2) == "b" && flabel(r, "k", 3) == "c",
          "append factor union: row labels correct");
    check(r.col("k").codes[2] == r.col("k").codes[1],
          "append factor union: using's 'b' remaps onto master's EXISTING code for 'b' (not a fresh one)");
    check(eq(r.col("m").f64[0], 1) && eq(r.col("m").f64[1], 2) &&
          std::isnan(r.col("m").f64[2]) && std::isnan(r.col("m").f64[3]),
          "append factor union: 'm' missing-fill on using-only rows (column union still applies)");
    check(std::isnan(r.col("u").f64[0]) && std::isnan(r.col("u").f64[1]) &&
          eq(r.col("u").f64[2], 10) && eq(r.col("u").f64[3], 20),
          "append factor union: 'u' missing-fill on master-only rows");
}

// L1b: f64-vs-factor dtype clash — BOTH sides promote to f64 (matches PIET
// append.py's _concat_mixed): a factor side's cells become their RAW
// DICTIONARY CODE as a double, NOT a parse of the level text. Both
// directions (master f64/using factor, and master factor/using f64), plus
// missing propagation through the clash (NaN on the f64 side, code -1 on the
// factor side).
static void test_append_mixed_dtype() {
    std::printf("append (f64-vs-factor dtype clash):\n");
    // direction 1: master f64, using factor
    {
        Frame m1 = make_frame({{"k", {1, 2}}}, 2);
        Frame u1;
        u1.set_n_rows_hint(2);
        u1.append_factor_column("k", {"x", "y"});  // x=code0, y=code1
        u1.finalize_after_bulk_load();
        Frame r1 = m1.append(u1);
        check(!r1.col("k").is_factor, "append dtype clash: OUTPUT column is f64 (either side factor -> f64)");
        check(eq(r1.col("k").f64[0], 1) && eq(r1.col("k").f64[1], 2),
              "append dtype clash: master f64 values pass through unchanged");
        check(eq(r1.col("k").f64[2], 0) && eq(r1.col("k").f64[3], 1),
              "append dtype clash: using factor cells become their RAW CODE as a double (x->0.0, y->1.0), "
              "not a numeric parse of the level text");
    }
    // direction 2 (other side factor) + missing propagation
    {
        Frame m2;
        m2.set_n_rows_hint(2);
        m2.append_factor_column("k", {"p", ""});  // "" -> code -1 (missing), default empty_is_missing=true
        m2.finalize_after_bulk_load();
        Frame u2 = make_frame({{"k", {NAN, 6}}}, 2);
        Frame r2 = m2.append(u2);
        check(!r2.col("k").is_factor, "append dtype clash (other direction): OUTPUT is f64 too");
        check(eq(r2.col("k").f64[0], 0), "append dtype clash: master factor code (p=code0) as a double");
        check(std::isnan(r2.col("k").f64[1]), "append dtype clash: master factor MISSING (code -1) -> NaN");
        check(std::isnan(r2.col("k").f64[2]) && eq(r2.col("k").f64[3], 6),
              "append dtype clash: using f64 values (incl. NaN) pass through unchanged");
    }
}

// ENG-APPEND: the one-shot `cobra append --out` path streams the column-union row
// concat straight to disk via Frame::write_hbin_append, never materializing the full
// (nm+nu)-row result Frame. The append RESULT frame carries no src_path_, so
// append(u).write_hbin(out) always takes the tight write_hbin_full(reserve=false)
// branch — the streaming writer must reproduce THAT file byte-for-byte. Covers all
// seven per-column kinds (f64-both, factor-both level-union, f64/factor mixed both
// directions, and factor/f64 present on only one side) plus multi-column alignment.
static bool files_identical(const std::string& a, const std::string& b) {
    std::ifstream fa(to_native_path(a), std::ios::binary);
    std::ifstream fb(to_native_path(b), std::ios::binary);
    if (!fa || !fb) return false;
    std::string sa((std::istreambuf_iterator<char>(fa)), std::istreambuf_iterator<char>());
    std::string sb((std::istreambuf_iterator<char>(fb)), std::istreambuf_iterator<char>());
    return sa.size() == sb.size() && sa == sb;
}

static void test_append_streaming() {
    std::printf("append (streaming writer byte-identity):\n");
    // path::string() would hand write_hbin() the ACTIVE-CODEPAGE narrow spelling on
    // Windows (CP949 under a Korean %TEMP%), which write_hbin then re-decodes as UTF-8
    // via u8path and misses the file. Keep every engine-facing path in UTF-8 through
    // from_native_path(); on POSIX both spellings are the same bytes.
    const std::filesystem::path dir = std::filesystem::temp_directory_path();
    auto roundtrip = [&](const Frame& m, const Frame& u, const char* label) {
        const std::string pa = from_native_path(
            dir / to_native_path(std::string("cobra_append_ref_") + label + ".hbin"));
        const std::string pb = from_native_path(
            dir / to_native_path(std::string("cobra_append_stream_") + label + ".hbin"));
        Frame r = m.append(u);
        r.write_hbin(pa);             // reference: the real one-shot write path
        m.write_hbin_append(u, pb);   // streaming writer under test
        check(files_identical(pa, pb),
              std::string("append streaming byte-identical: ") + label);
        native_unlink(pa);
        native_unlink(pb);
    };

    // case 1: pure f64 with in_m-only (y) + in_u-only (z), asymmetric row counts
    {
        Frame m = make_frame({{"x", {1, 2, 3}}, {"y", {10, 20, 30}}}, 3);
        Frame u = make_frame({{"x", {4, 5}}, {"z", {100, 200}}}, 2);
        // BUG-363: the byte-identity arms above all carried EMPTY format/label, so
        // they could not witness the coupling their own comment demands — the
        // streaming writer's header keys must track Frame::append exactly. Give the
        // columns real metadata (master-side, using-only, and a shared name where the
        // MASTER must win) so a divergence between the two writers is a RED here
        // rather than a surprise in a freeze fixture.
        m.mutable_col("x").format = "%9.2f";  m.mutable_col("x").label = "Ex master";
        m.mutable_col("y").format = "%td";    m.mutable_col("y").label = "Why";
        u.mutable_col("x").format = "%12.0g"; u.mutable_col("x").label = "Ex using";
        u.mutable_col("z").format = "%8.3f";  u.mutable_col("z").label = "Zed";
        roundtrip(m, u, "f64_union");
    }
    // case 2: factor+factor level union (b shared, c new) + f64 in_m-only / in_u-only
    {
        Frame m, u;
        m.set_n_rows_hint(2);
        m.append_factor_column("k", {"a", "b"});
        m.append_f64_column("mm", {1, 2});
        m.finalize_after_bulk_load();
        u.set_n_rows_hint(2);
        u.append_factor_column("k", {"b", "c"});
        u.append_f64_column("uu", {10, 20});
        u.finalize_after_bulk_load();
        roundtrip(m, u, "factor_union");
    }
    // case 3: f64-vs-factor mixed clash, BOTH directions (k: f64/factor, j: factor/f64)
    {
        Frame m, u;
        m.set_n_rows_hint(2);
        m.append_f64_column("k", {1.5, 2.5});
        m.append_factor_column("j", {"p", ""});   // "" -> code -1 (missing)
        m.finalize_after_bulk_load();
        u.set_n_rows_hint(2);
        u.append_factor_column("k", {"x", "y"});
        u.append_f64_column("j", {7.0, NAN});
        u.finalize_after_bulk_load();
        roundtrip(m, u, "mixed_clash");
    }
    // case 4: factor in_m-only + factor in_u-only (no shared columns)
    {
        Frame m, u;
        m.set_n_rows_hint(3);
        m.append_factor_column("fm", {"a", "b", "a"});
        m.finalize_after_bulk_load();
        u.set_n_rows_hint(2);
        u.append_factor_column("fu", {"z", "w"});
        u.finalize_after_bulk_load();
        roundtrip(m, u, "factor_only_each_side");
    }
    // case 5: many mixed-kind columns — exercises per-column alignment padding runs
    {
        Frame m, u;
        m.set_n_rows_hint(5);
        m.append_f64_column("a", {1, 2, 3, 4, 5});
        m.append_factor_column("cat", {"lo", "hi", "lo", "mid", "hi"});
        m.append_f64_column("b", {0.1, 0.2, 0.3, 0.4, 0.5});
        m.append_f64_column("monly", {9, 8, 7, 6, 5});
        m.finalize_after_bulk_load();
        u.set_n_rows_hint(4);
        u.append_f64_column("a", {6, 7, 8, 9});
        u.append_factor_column("cat", {"hi", "new", "lo", "new2"});
        u.append_f64_column("b", {0.6, 0.7, 0.8, 0.9});
        u.append_f64_column("uonly", {-1, -2, -3, -4});
        u.finalize_after_bulk_load();
        roundtrip(m, u, "many_cols");
    }
}

// L1b: joinby k1 [k2 ...] using FILE — bare form, unmatched(none). f64 keys:
// m:m fan-out counts, key-dropped rows (both master-only AND using-only —
// joinby never outer-joins), column union + order, and the error paths.
static void test_joinby() {
    std::printf("joinby:\n");
    Frame master = make_frame({{"id", {1, 2, 2, 3}}, {"x", {10, 20, 21, 30}}}, 4);
    Frame usingf = make_frame({{"id", {2, 2, 4}}, {"z", {5, 6, 7}}, {"y", {200, 201, 400}}}, 3);
    Frame r = master.joinby(usingf, {"id"});

    check(r.n_rows() == 4, "joinby: m:m fan-out — 2 master id=2 rows x 2 using id=2 rows = 4 output rows");
    std::vector<std::string> names;
    for (const auto& c : r.columns()) names.push_back(c.name);
    check(names == std::vector<std::string>({"id", "x", "z", "y"}),
          "joinby: column union+order = master's own order (id,x) + using's non-key columns in USING's own order (z,y)");

    // row order: master row order OUTER, using row order INNER.
    double eid[4] = {2, 2, 2, 2}, ex[4] = {20, 20, 21, 21}, ez[4] = {5, 6, 5, 6}, ey[4] = {200, 201, 200, 201};
    for (int i = 0; i < 4; ++i) {
        check(eq(r.col("id").f64[i], eid[i]), "joinby: row order id");
        check(eq(r.col("x").f64[i], ex[i]), "joinby: row order x (master-row-major)");
        check(eq(r.col("z").f64[i], ez[i]) && eq(r.col("y").f64[i], ey[i]), "joinby: row order z,y (using-row-minor)");
    }

    bool saw1 = false, saw3 = false, saw4 = false;
    for (int64_t i = 0; i < r.n_rows(); ++i) {
        double v = r.col("id").f64[i];
        if (eq(v, 1)) saw1 = true;
        if (eq(v, 3)) saw3 = true;
        if (eq(v, 4)) saw4 = true;
    }
    check(!saw1 && !saw3, "joinby: master rows with zero using matches are DROPPED entirely (id=1, id=3 absent)");
    check(!saw4, "joinby: using-only rows never appear (id=4 absent — joinby never outer-joins, unlike merge)");

    // missing key matches missing key (both f64 NaN) — same convention as merge_frames.
    {
        Frame m4 = make_frame({{"id", {1, NAN}}, {"x", {10, 20}}}, 2);
        Frame u5 = make_frame({{"id", {NAN}}, {"y", {999}}}, 1);
        Frame r3 = m4.joinby(u5, {"id"});
        check(r3.n_rows() == 1, "joinby: missing key matches missing key (both f64 NaN)");
        check(std::isnan(r3.col("id").f64[0]) && eq(r3.col("x").f64[0], 20) && eq(r3.col("y").f64[0], 999),
              "joinby: missing-key match carries the right row payload");
    }

    // key dtype mismatch (f64 vs factor) -> zero matches, NOT an error (distinct
    // from merge_frames, which hard-errors on this same condition).
    {
        Frame m3 = make_frame({{"id", {1, 2}}, {"x", {10, 20}}}, 2);
        Frame u4;
        u4.set_n_rows_hint(2);
        u4.append_factor_column("id", {"1", "2"});
        u4.append_f64_column("y", {100, 200});
        u4.finalize_after_bulk_load();
        Frame r2 = m3.joinby(u4, {"id"});
        check(r2.n_rows() == 0, "joinby: key dtype mismatch (f64 vs factor) -> zero matches, NOT a thrown error");
        check(r2.has("id") && r2.has("x") && r2.has("y"), "joinby: dtype-mismatch empty result still has the full output schema");
    }

    // error paths
    {
        bool threw = false;
        try { master.joinby(usingf, {"nope"}); } catch (const std::exception&) { threw = true; }
        check(threw, "joinby: key not found in master throws bad_input");
    }
    {
        Frame u6 = make_frame({{"zzz", {1}}}, 1);
        bool threw = false;
        try { master.joinby(u6, {"id"}); } catch (const std::exception&) { threw = true; }
        check(threw, "joinby: key not found in using throws bad_input");
    }
    {
        bool threw = false;
        try { master.joinby(usingf, {}); } catch (const std::exception&) { threw = true; }
        check(threw, "joinby: empty key list throws bad_input");
    }
    {
        // A non-key name on BOTH sides: Stata's joinby takes it from the MASTER
        // unless update/replace is given ([D] joinby) — the same default merge
        // uses. This build used to reject it; the P1 wave answerbook proved the
        // reject wrong (`joinby id using wave_lookup` shares cluster/stratum/
        // group and real Stata 19.5 returns 20 rows with the master's copies).
        Frame u7 = make_frame({{"id", {2}}, {"x", {999}}}, 1);
        Frame r7 = master.joinby(u7, {"id"});
        check(r7.has("x"), "joinby: a shared non-key column survives (from the master)");
        // master has TWO id==2 rows (x = 20 and 21), so the inner join yields 2.
        check(r7.n_rows() == 2, "joinby: shared-name join still matches on the key (2 rows)");
        check(r7.col("x").f64[0] == 20.0 && r7.col("x").f64[1] == 21.0,
              "joinby: the MASTER's values win for a shared column (not the using's 999)");
    }
}

// L1b: joinby with factor keys, misaligned per-file level tables — the same
// "0.18.0 Eval gate item 1" scenario test_merge_factor_keys covers for merge,
// adapted to joinby's drop-unmatched (never outer-join) semantics.
static void test_joinby_factor_keys() {
    std::printf("joinby (factor keys, misaligned level tables):\n");
    Frame master, usingf;
    master.set_n_rows_hint(4);
    master.append_factor_column("k", {"a", "b", "zz", "c"});  // c has code 3
    master.append_f64_column("xm", {1, 2, 3, 4});
    master.finalize_after_bulk_load();
    usingf.set_n_rows_hint(4);
    usingf.append_factor_column("k", {"a", "b", "c", "d"});   // c has code 2 (DIFFERENT code, SAME string)
    usingf.append_f64_column("xu", {10, 20, 30, 40});
    usingf.finalize_after_bulk_load();

    Frame r = master.joinby(usingf, {"k"});
    check(r.n_rows() == 3, "joinby factor: 3 rows (a,b,c matched; 'zz' and 'd' DROPPED — not outer-joined like merge)");

    const FrameColumn& k = r.col("k");
    check(k.is_factor, "joinby factor: OUTPUT k stays a factor");
    check(k.levels.size() == 3 && k.levels[0] == "a" && k.levels[1] == "b" && k.levels[2] == "c",
          "joinby factor: fresh dictionary, first-seen OUTPUT order — 'zz' never entered it (its row was dropped)");

    for (int64_t i = 0; i < r.n_rows(); ++i) {
        std::string lbl = flabel(r, "k", i);
        if (lbl == "a") check(eq(r.col("xm").f64[i], 1) && eq(r.col("xu").f64[i], 10), "joinby factor: a joins by label");
        if (lbl == "b") check(eq(r.col("xm").f64[i], 2) && eq(r.col("xu").f64[i], 20), "joinby factor: b joins by label");
        if (lbl == "c")
            check(eq(r.col("xm").f64[i], 4) && eq(r.col("xu").f64[i], 30),
                  "joinby factor: c joins by LABEL despite DIFFERENT per-file codes (master code3, using code2) — "
                  "a raw-code join would wrongly match master 'zz'(code2) to using 'c'(code2) instead");
    }

    // m:m fan-out with factor keys: using repeats a key -> the master row fans out;
    // the unmatched master row ('p') contributes NOTHING to the output dictionary.
    Frame m2, u2;
    m2.set_n_rows_hint(2);
    m2.append_factor_column("k", {"p", "q"});
    m2.append_f64_column("xm", {1, 2});
    m2.finalize_after_bulk_load();
    u2.set_n_rows_hint(3);
    u2.append_factor_column("k", {"q", "q", "r"});
    u2.append_f64_column("xu", {10, 20, 30});
    u2.finalize_after_bulk_load();
    Frame r2 = m2.joinby(u2, {"k"});
    check(r2.n_rows() == 2, "joinby factor: m:m fan-out — master 'q' matches BOTH using 'q' rows ('p','r' drop)");
    check(eq(r2.col("xu").f64[0], 10) && eq(r2.col("xu").f64[1], 20),
          "joinby factor: fan-out preserves using row order for the repeated master key");
    check(eq(r2.col("xm").f64[0], 2) && eq(r2.col("xm").f64[1], 2),
          "joinby factor: fan-out — both output rows carry the SAME master row's xm");
    check(r2.col("k").levels.size() == 1 && r2.col("k").levels[0] == "q",
          "joinby factor: fan-out output dictionary shrinks to just 'q' — dropped 'p' row contributes nothing");
}

static void test_duplicates() {
    std::printf("duplicates:\n");
    Frame f = make_frame({{"k", {1, 1, 2, 3, 3, 3}}}, 6);
    auto report = f.duplicates_report({"k"});
    // groups: k=1 (2 rows), k=2 (1 row), k=3 (3 rows)
    int64_t total_surplus = 0;
    for (const auto& row : report) total_surplus += row.surplus;
    check(total_surplus == 1 + 0 + 2, "duplicates report total surplus (1+0+2=3)");

    int64_t n_dropped = -1;
    Frame dropped = f.duplicates_drop({"k"}, &n_dropped);
    check(n_dropped == 3, "duplicates drop count (6 rows -> 3 unique keys, 3 dropped)");
    check(dropped.n_rows() == 3, "duplicates drop result row count");
    check(eq(dropped.col("k").f64[0], 1) && eq(dropped.col("k").f64[1], 2) && eq(dropped.col("k").f64[2], 3),
         "duplicates drop keeps first occurrence per key, in order");

    auto tag = f.duplicates_tag({"k"});
    double expect_tag[6] = {1, 1, 0, 2, 2, 2};
    for (int i = 0; i < 6; ++i) check(eq(tag[i], expect_tag[i]), "duplicates tag = group_size-1");
}

static void test_recode() {
    std::printf("recode:\n");
    // recode rep78 (1 2=1)(3 4=2)(5=3)
    Frame f = make_frame({{"rep78", {1, 2, 3, 4, 5, NAN}}}, 6);
    std::vector<Frame::RecodeRule> rules = {
        {0, 1, 0, false, 1}, {0, 2, 0, false, 1},   // (1 2 = 1) split into two eq rules
        {0, 3, 0, false, 2}, {0, 4, 0, false, 2},   // (3 4 = 2)
        {0, 5, 0, false, 3},                         // (5 = 3)
    };
    auto out = f.recode_apply("rep78", rules);
    double expect[6] = {1, 1, 2, 2, 3, NAN};
    for (int i = 0; i < 6; ++i) check(eq(out[i], expect[i]) || (std::isnan(out[i]) && std::isnan(expect[i])), "recode value list");

    // range + else + missing
    Frame f2 = make_frame({{"v", {1, 5, 10, 15, NAN}}}, 5);
    std::vector<Frame::RecodeRule> rules2 = {
        {1, 0, 9, false, 100},   // (0/9 = 100)
        {2, 0, 0, false, -1},    // (missing = -1)
        {3, 0, 0, false, 999},   // (else = 999)
    };
    auto out2 = f2.recode_apply("v", rules2);
    check(eq(out2[0], 100), "recode range hit (1 in [0,9])");
    check(eq(out2[1], 100), "recode range hit (5 in [0,9])");
    check(eq(out2[2], 999), "recode else (10 not in [0,9])");
    check(eq(out2[3], 999), "recode else (15 not in [0,9])");
    check(eq(out2[4], -1), "recode missing rule");
}

static void test_reshape() {
    std::printf("reshape:\n");
    // wide: id, inc1980, inc1981  ->  long: id, year, inc
    Frame wide = make_frame({{"id", {1, 2}}, {"inc1980", {5000, 6000}}, {"inc1981", {5500, 6500}}}, 2);
    auto rl = wide.reshape_long({"inc"}, {"id"}, "year");
    check(rl.frame.n_rows() == 4, "reshape long row count (2 ids x 2 waves)");
    check(rl.j_values.size() == 2, "reshape long j_values count");
    // row order: suffix-major then id-minor (matches our implementation's loop order)
    // find id=1,year=1980 and check inc
    bool found = false;
    for (int64_t r = 0; r < rl.frame.n_rows(); ++r) {
        if (eq(rl.frame.col("id").f64[r], 1) && eq(rl.frame.col("year").f64[r], 1980)) {
            check(eq(rl.frame.col("inc").f64[r], 5000), "reshape long inc value id=1,year=1980");
            found = true;
        }
    }
    check(found, "reshape long found id=1,year=1980 row");

    // round-trip: long -> wide should recover the original wide shape
    auto rw = rl.frame.reshape_wide({"inc"}, {"id"}, "year");
    check(rw.frame.n_rows() == 2, "reshape wide row count round-trip");
    check(rw.frame.has("inc1980") && rw.frame.has("inc1981"), "reshape wide recovers stub columns");
    for (int64_t r = 0; r < rw.frame.n_rows(); ++r) {
        double id = rw.frame.col("id").f64[r];
        if (eq(id, 1)) check(eq(rw.frame.col("inc1980").f64[r], 5000), "reshape wide round-trip value");
    }
}

// 0.18.1 PATCH — reshape wide duplicate-(i,j) collapse + dropna-driven group
// survival (contract §22 divergence-ledger item). Root cause of the frozen
// oracle's kitchensink_panel 119-vs-120 divergence: pandas.pivot_table(...,
// aggfunc="first") collapses literal duplicate (i,j) keys to the FIRST
// occurrence in row order (Stata errors here; the frozen 1.16.0 oracle is
// the 2.0 parity law) AND, independently, defaults to dropna=True — an
// i-group with EVERY value missing for a given stub is dropped from that
// stub's pivot entirely. With multiple stubs, reshape.py recombines the
// per-stub pivots via pd.merge(..., how="outer"), so a group dropped by one
// stub can be resurrected by another (NaN-filled for the dropping stub).
static void test_reshape_dup_and_dropna() {
    std::printf("reshape (duplicate i,j + dropna group survival — 0.18.1):\n");

    // --- single duplicate (i,j): last-listed dup row must NOT win ---------
    // id=1,year=2000 appears twice (values 10 then 99); first-in-row-order
    // (10) must win, matching pivot_table(aggfunc="first").
    {
        Frame f = make_frame({{"id", {1, 1, 2}}, {"year", {2000, 2000, 2000}}, {"y", {10, 99, 30}}}, 3);
        auto rw = f.reshape_wide({"y"}, {"id"}, "year");
        check(rw.frame.n_rows() == 2, "single dup: row count == 2 unique ids");
        for (int64_t r = 0; r < rw.frame.n_rows(); ++r) {
            double id = rw.frame.col("id").f64[r];
            if (eq(id, 1)) check(eq(rw.frame.col("y2000").f64[r], 10), "single dup: first occurrence (10) wins, not last (99)");
            if (eq(id, 2)) check(eq(rw.frame.col("y2000").f64[r], 30), "single dup: non-dup id unaffected");
        }
    }

    // --- multiple dups across different ids, dup at FIRST row -------------
    {
        // row0,1 = id1 dup (first row is the dup's first occurrence);
        // row2,3 = id2 dup; row4 = id3 single.
        Frame f = make_frame({{"id", {1, 1, 2, 2, 3}}, {"year", {2000, 2000, 2000, 2000, 2000}},
                              {"y", {7, 8, 70, 80, 300}}}, 5);
        auto rw = f.reshape_wide({"y"}, {"id"}, "year");
        check(rw.frame.n_rows() == 3, "multi dup: row count == 3 unique ids");
        for (int64_t r = 0; r < rw.frame.n_rows(); ++r) {
            double id = rw.frame.col("id").f64[r];
            if (eq(id, 1)) check(eq(rw.frame.col("y2000").f64[r], 7), "multi dup: id1 first occurrence (7) wins");
            if (eq(id, 2)) check(eq(rw.frame.col("y2000").f64[r], 70), "multi dup: id2 first occurrence (70) wins");
            if (eq(id, 3)) check(eq(rw.frame.col("y2000").f64[r], 300), "multi dup: id3 (no dup) unaffected");
        }
    }

    // --- dup at LAST row of the frame --------------------------------------
    {
        Frame f = make_frame({{"id", {1, 2, 2}}, {"year", {2000, 2000, 2000}}, {"y", {1, 20, 21}}}, 3);
        auto rw = f.reshape_wide({"y"}, {"id"}, "year");
        check(rw.frame.n_rows() == 2, "dup-at-last-row: row count == 2 unique ids");
        for (int64_t r = 0; r < rw.frame.n_rows(); ++r) {
            if (eq(rw.frame.col("id").f64[r], 2))
                check(eq(rw.frame.col("y2000").f64[r], 20), "dup-at-last-row: first occurrence (20) wins over trailing dup (21)");
        }
    }

    // --- multiple stub vars, dup in ONE stub's key row only ----------------
    // id=1,year=2000 has TWO rows (dup) differing in both y and w; first
    // occurrence must win independently for each stub.
    {
        Frame f = make_frame({{"id", {1, 1, 2}}, {"year", {2000, 2000, 2000}},
                              {"y", {5, 55, 6}}, {"w", {9, 99, 10}}}, 3);
        auto rw = f.reshape_wide({"y", "w"}, {"id"}, "year");
        check(rw.frame.n_rows() == 2, "multi-stub dup: row count == 2 unique ids");
        for (int64_t r = 0; r < rw.frame.n_rows(); ++r) {
            if (eq(rw.frame.col("id").f64[r], 1)) {
                check(eq(rw.frame.col("y2000").f64[r], 5), "multi-stub dup: y first-occurrence (5) wins");
                check(eq(rw.frame.col("w2000").f64[r], 9), "multi-stub dup: w first-occurrence (9) wins");
            }
        }
    }

    // --- dropna: single stub, one group ALL-missing -> group dropped -------
    // (this is the exact mechanism behind the kitchensink_panel 119-vs-120
    // divergence: id has no literal duplicate (i,j) rows at all — id=1's `y`
    // is simply missing at every year, so pivot_table(dropna=True) drops it.)
    {
        Frame f = make_frame({{"id", {1, 1, 2, 2, 3, 3}}, {"year", {2000, 2001, 2000, 2001, 2000, 2001}},
                              {"y", {NAN, NAN, 3, 4, 5, 6}}}, 6);
        auto rw = f.reshape_wide({"y"}, {"id"}, "year");
        check(rw.frame.n_rows() == 2, "dropna single-stub: all-missing id=1 group dropped (3 ids -> 2 rows)");
        bool saw_id1 = false;
        for (int64_t r = 0; r < rw.frame.n_rows(); ++r) if (eq(rw.frame.col("id").f64[r], 1)) saw_id1 = true;
        check(!saw_id1, "dropna single-stub: id=1 absent from output entirely");
        // row order must be ASCENDING by i_vars (pivot_table's sorted index),
        // not first-occurrence order (which would be id=2 before id=3 here
        // too, coincidentally — see the unsorted-input case below for a
        // discriminating check).
        check(eq(rw.frame.col("id").f64[0], 2) && eq(rw.frame.col("id").f64[1], 3),
              "dropna single-stub: surviving rows in ascending id order");
    }

    // --- dropna: row order is ASCENDING even when source rows arrive out of
    //     order (discriminates sorted-by-key from first-occurrence order) ---
    {
        Frame f = make_frame({{"id", {3, 3, 1, 1, 2, 2}}, {"year", {2000, 2001, 2000, 2001, 2000, 2001}},
                              {"y", {1, 2, 3, 4, 5, 6}}}, 6);
        auto rw = f.reshape_wide({"y"}, {"id"}, "year");
        check(rw.frame.n_rows() == 3, "dropna order test: no group dropped (all have data) -> 3 rows");
        check(eq(rw.frame.col("id").f64[0], 1) && eq(rw.frame.col("id").f64[1], 2) && eq(rw.frame.col("id").f64[2], 3),
              "dropna order test: rows sorted ascending by id despite arriving as 3,1,2 in source");
        for (int64_t r = 0; r < 3; ++r) {
            double id = rw.frame.col("id").f64[r];
            if (eq(id, 1)) { check(eq(rw.frame.col("y2000").f64[r], 3), "order test id1 y2000"); check(eq(rw.frame.col("y2001").f64[r], 4), "order test id1 y2001"); }
            if (eq(id, 2)) { check(eq(rw.frame.col("y2000").f64[r], 5), "order test id2 y2000"); check(eq(rw.frame.col("y2001").f64[r], 6), "order test id2 y2001"); }
            if (eq(id, 3)) { check(eq(rw.frame.col("y2000").f64[r], 1), "order test id3 y2000"); check(eq(rw.frame.col("y2001").f64[r], 2), "order test id3 y2001"); }
        }
    }

    // --- dropna: multi-stub RESURRECTION — a group all-missing in stub A
    //     but present in stub B must survive, with stub A's cells NaN -------
    {
        Frame f = make_frame({{"id", {1, 1, 2, 2, 3, 3}}, {"year", {2000, 2001, 2000, 2001, 2000, 2001}},
                              {"y", {NAN, NAN, 3, 4, 5, 6}}, {"w", {7, 8, 70, 80, NAN, NAN}}}, 6);
        auto rw = f.reshape_wide({"y", "w"}, {"id"}, "year");
        // id=1: y all-NaN (dropped from y-pivot) but w has data -> resurrected by w.
        // id=3: w all-NaN (dropped from w-pivot) but y has data -> survives via y.
        // Nobody is all-NaN in BOTH -> nothing fully drops. 3 rows.
        check(rw.frame.n_rows() == 3, "dropna multi-stub resurrection: all 3 ids present (none all-missing in every stub)");
        for (int64_t r = 0; r < rw.frame.n_rows(); ++r) {
            double id = rw.frame.col("id").f64[r];
            if (eq(id, 1)) {
                check(std::isnan(rw.frame.col("y2000").f64[r]) && std::isnan(rw.frame.col("y2001").f64[r]),
                      "dropna resurrection: id=1 y-columns NaN-filled (y was the all-missing stub)");
                check(eq(rw.frame.col("w2000").f64[r], 7) && eq(rw.frame.col("w2001").f64[r], 8),
                      "dropna resurrection: id=1 w-columns keep real values (resurrected by w)");
            }
            if (eq(id, 3)) {
                check(eq(rw.frame.col("y2000").f64[r], 5) && eq(rw.frame.col("y2001").f64[r], 6),
                      "dropna resurrection: id=3 y-columns keep real values (resurrected by y)");
                check(std::isnan(rw.frame.col("w2000").f64[r]) && std::isnan(rw.frame.col("w2001").f64[r]),
                      "dropna resurrection: id=3 w-columns NaN-filled (w was the all-missing stub)");
            }
        }
    }

    // --- dropna: ALL stubs all-missing for a group -> group fully dropped --
    {
        Frame f = make_frame({{"id", {1, 1, 2, 2}}, {"year", {2000, 2001, 2000, 2001}},
                              {"y", {NAN, NAN, 3, 4}}, {"w", {NAN, NAN, 30, 40}}}, 4);
        auto rw = f.reshape_wide({"y", "w"}, {"id"}, "year");
        check(rw.frame.n_rows() == 1, "dropna full-drop: id=1 all-missing in EVERY stub -> fully absent (1 row)");
        check(eq(rw.frame.col("id").f64[0], 2), "dropna full-drop: only id=2 survives");
    }

    // --- kitchensink_panel.dta repro shape: 120 ids, id=1 all-missing y ----
    // (n_wide must be 119, matching the frozen 1.16.0 oracle exactly — this
    // is the literal M3b-reported divergence, reproduced at unit scale.)
    {
        std::vector<double> ids, years, ys;
        for (int id = 1; id <= 120; ++id) {
            for (int yr = 2014; yr <= 2021; ++yr) {
                ids.push_back(id);
                years.push_back(yr);
                ys.push_back(id == 1 ? NAN : (id * 10 + yr - 2014));
            }
        }
        Frame f = make_frame({{"id", ids}, {"year", years}, {"y", ys}}, static_cast<int64_t>(ids.size()));
        auto rw = f.reshape_wide({"y"}, {"id"}, "year");
        check(rw.frame.n_rows() == 119, "kitchensink-shape repro: n_wide == 119 (matches frozen 1.16.0 oracle)");
        check(rw.j_values.size() == 8, "kitchensink-shape repro: 8 j-values (years)");
    }
}

static void test_merge() {
    std::printf("merge:\n");
    Frame master = make_frame({{"id", {1, 2, 3}}, {"x", {10, 20, 30}}}, 3);
    Frame usingf = make_frame({{"id", {2, 3, 4}}, {"y", {200, 300, 400}}}, 3);

    MergeOpts opts;
    opts.type = "1:1";
    opts.keylist = {"id"};
    MergeResult r = merge_frames(master, usingf, opts);
    check(r.master_only == 1, "merge master_only count (id=1)");
    check(r.using_only == 1, "merge using_only count (id=4)");
    check(r.matched == 2, "merge matched count (id=2,3)");
    check(r.frame.n_rows() == 4, "merge total row count");
    check(r.frame.has("_merge"), "merge _merge column present");

    // spot check: id=2 row has x=20,y=200,_merge=3
    for (int64_t i = 0; i < r.frame.n_rows(); ++i) {
        if (eq(r.frame.col("id").f64[i], 2)) {
            check(eq(r.frame.col("x").f64[i], 20), "merge id=2 x value");
            check(eq(r.frame.col("y").f64[i], 200), "merge id=2 y value");
            check(eq(r.frame.col("_merge").f64[i], 3), "merge id=2 _merge=matched");
        }
        if (eq(r.frame.col("id").f64[i], 1)) {
            check(std::isnan(r.frame.col("y").f64[i]), "merge id=1 (master-only) y is missing");
            check(eq(r.frame.col("_merge").f64[i], 1), "merge id=1 _merge=master-only");
        }
        if (eq(r.frame.col("id").f64[i], 4)) {
            check(std::isnan(r.frame.col("x").f64[i]), "merge id=4 (using-only) x is missing");
            check(eq(r.frame.col("_merge").f64[i], 2), "merge id=4 _merge=using-only");
        }
    }

    // 1:1 cardinality violation should throw
    Frame dup_master = make_frame({{"id", {1, 1}}, {"x", {1, 2}}}, 2);
    bool threw = false;
    try { merge_frames(dup_master, usingf, opts); } catch (const std::exception&) { threw = true; }
    check(threw, "merge 1:1 throws on duplicate master key");

    // nogenerate
    MergeOpts opts2 = opts;
    opts2.nogenerate = true;
    MergeResult r2 = merge_frames(master, usingf, opts2);
    check(!r2.frame.has("_merge"), "merge nogenerate omits _merge column");

    // keep(match)
    MergeOpts opts3 = opts;
    opts3.keep_codes = {3};
    MergeResult r3 = merge_frames(master, usingf, opts3);
    check(r3.frame.n_rows() == 2, "merge keep(match) filters to matched rows only");
}

// 0.18.0 Eval gate item 1 (BLOCKER): factor keys must join by LABEL, not by
// per-file int32 dictionary code — the two files' code spaces are unrelated.
// Master levels [a,b,zz,c] vs using [a,b,c,d]: a raw-code join falsely matched
// zz(2)<->c(2) and c(3)<->d(3), lost the true c<->c match, and dropped the
// using-only d row entirely. Covers all three cardinalities.
static void test_merge_factor_keys() {
    std::printf("merge (factor keys, misaligned level tables):\n");

    // --- 1:1 ---
    Frame master, usingf;
    master.set_n_rows_hint(4);
    master.append_factor_column("k", {"a", "b", "zz", "c"});   // c has code 3
    master.append_f64_column("xm", {1, 2, 3, 4});
    master.finalize_after_bulk_load();
    usingf.set_n_rows_hint(4);
    usingf.append_factor_column("k", {"a", "b", "c", "d"});    // c has code 2
    usingf.append_f64_column("xu", {10, 20, 30, 40});
    usingf.finalize_after_bulk_load();

    MergeOpts opts;
    opts.type = "1:1";
    opts.keylist = {"k"};
    MergeResult r = merge_frames(master, usingf, opts);
    check(r.frame.n_rows() == 5, "merge 1:1 factor: 5 rows (a,b,c matched + zz + d)");
    check(r.matched == 3 && r.master_only == 1 && r.using_only == 1,
          "merge 1:1 factor: counts matched=3 master_only=1 using_only=1");
    for (int64_t i = 0; i < r.frame.n_rows(); ++i) {
        std::string k = flabel(r.frame, "k", i);
        if (k == "c") {
            check(eq(r.frame.col("xm").f64[i], 4) && eq(r.frame.col("xu").f64[i], 30),
                  "merge 1:1 factor: c joins by LABEL (xm=4, xu=30)");
            check(eq(r.frame.col("_merge").f64[i], 3), "merge 1:1 factor: c matched");
        }
        if (k == "zz") {
            check(std::isnan(r.frame.col("xu").f64[i]) && eq(r.frame.col("_merge").f64[i], 1),
                  "merge 1:1 factor: zz is master-only (no false code-match to c)");
        }
        if (k == "d") {
            check(std::isnan(r.frame.col("xm").f64[i]) && eq(r.frame.col("_merge").f64[i], 2) &&
                  eq(r.frame.col("xu").f64[i], 40),
                  "merge 1:1 factor: using-only d row present with _merge=2");
        }
    }

    // --- m:1 (master repeats keys; using unique; levels misaligned) ---
    Frame m2, u2;
    m2.set_n_rows_hint(4);
    m2.append_factor_column("k", {"x", "y", "y", "w"});        // levels [x,y,w]
    m2.append_f64_column("xm", {1, 2, 3, 4});
    m2.finalize_after_bulk_load();
    u2.set_n_rows_hint(3);
    u2.append_factor_column("k", {"y", "w", "q"});             // levels [y,w,q]
    u2.append_f64_column("xu", {100, 200, 300});
    u2.finalize_after_bulk_load();
    MergeOpts om = opts;
    om.type = "m:1";
    MergeResult rm = merge_frames(m2, u2, om);
    check(rm.frame.n_rows() == 5, "merge m:1 factor: 5 rows (x, y, y, w + using-only q)");
    check(rm.matched == 3 && rm.master_only == 1 && rm.using_only == 1,
          "merge m:1 factor: counts matched=3 master_only=1 using_only=1");
    int y_hits = 0;
    for (int64_t i = 0; i < rm.frame.n_rows(); ++i) {
        std::string k = flabel(rm.frame, "k", i);
        if (k == "y") { ++y_hits; check(eq(rm.frame.col("xu").f64[i], 100), "merge m:1 factor: y rows get y payload"); }
        if (k == "w") check(eq(rm.frame.col("xu").f64[i], 200), "merge m:1 factor: w joins by label");
        if (k == "q") check(eq(rm.frame.col("_merge").f64[i], 2), "merge m:1 factor: q using-only");
    }
    check(y_hits == 2, "merge m:1 factor: both master y rows survive");

    // --- 1:m (using repeats keys; master unique; levels misaligned) ---
    Frame m3, u3;
    m3.set_n_rows_hint(2);
    m3.append_factor_column("k", {"p", "q"});                  // levels [p,q] (q = code 1)
    m3.append_f64_column("xm", {1, 2});
    m3.finalize_after_bulk_load();
    u3.set_n_rows_hint(3);
    u3.append_factor_column("k", {"q", "q", "r"});             // levels [q,r] (q = code 0)
    u3.append_f64_column("xu", {10, 20, 30});
    u3.finalize_after_bulk_load();
    MergeOpts o1m = opts;
    o1m.type = "1:m";
    MergeResult r1m = merge_frames(m3, u3, o1m);
    check(r1m.frame.n_rows() == 4, "merge 1:m factor: 4 rows (p + q x2 + using-only r)");
    check(r1m.matched == 2 && r1m.master_only == 1 && r1m.using_only == 1,
          "merge 1:m factor: counts matched=2 master_only=1 using_only=1");
    int q_hits = 0;
    for (int64_t i = 0; i < r1m.frame.n_rows(); ++i) {
        std::string k = flabel(r1m.frame, "k", i);
        if (k == "q") { ++q_hits; check(eq(r1m.frame.col("xm").f64[i], 2), "merge 1:m factor: q rows carry master payload"); }
        if (k == "r") check(eq(r1m.frame.col("_merge").f64[i], 2), "merge 1:m factor: r using-only");
    }
    check(q_hits == 2, "merge 1:m factor: q fan-out to both using rows");

    // --- missing keys: -1 codes match each other (both-missing), matching the
    //     oracle's None-as-hashable convention; a one-sided missing key is
    //     unmatched (nothing to pair with). ---
    Frame m4, u4;
    m4.set_n_rows_hint(2);
    m4.append_factor_column("k", {"a", ""});   // "" == missing (-1)
    m4.append_f64_column("xm", {1, 2});
    m4.finalize_after_bulk_load();
    u4.set_n_rows_hint(1);
    u4.append_factor_column("k", {"a"});
    u4.append_f64_column("xu", {10});
    u4.finalize_after_bulk_load();
    MergeResult r4 = merge_frames(m4, u4, opts);
    check(r4.frame.n_rows() == 2 && r4.matched == 1 && r4.master_only == 1,
          "merge factor: one-sided missing key is master-only, not spuriously matched");

    // --- mixed dtype key (factor vs f64) is a hard error ---
    Frame m5 = make_frame({{"k", {1, 2}}, {"xm", {1, 2}}}, 2);
    bool threw = false;
    try { merge_frames(m5, u3, opts); } catch (const std::exception&) { threw = true; }
    check(threw, "merge factor-vs-numeric key throws (matches pandas oracle's merge type error)");
}

// duplicates on a factor column groups by label — within a single frame the
// code<->label mapping is bijective so code-space grouping IS label-space
// grouping; pinned here so a future refactor can't regress it.
static void test_duplicates_factor() {
    std::printf("duplicates (factor column):\n");
    Frame f;
    f.set_n_rows_hint(6);
    f.append_factor_column("k", {"a", "b", "a", "c", "b", "a"});
    f.append_f64_column("v", {1, 2, 3, 4, 5, 6});
    f.finalize_after_bulk_load();

    auto tag = f.duplicates_tag({"k"});
    double expect_tag[6] = {2, 1, 2, 0, 1, 2};  // group sizes: a=3, b=2, c=1
    for (int i = 0; i < 6; ++i) check(eq(tag[i], expect_tag[i]), "duplicates factor tag = group_size-1 by label");

    int64_t n_dropped = -1;
    Frame dropped = f.duplicates_drop({"k"}, &n_dropped);
    check(n_dropped == 3 && dropped.n_rows() == 3, "duplicates factor drop: 3 unique labels kept");
    check(flabel(dropped, "k", 0) == "a" && flabel(dropped, "k", 1) == "b" && flabel(dropped, "k", 2) == "c",
          "duplicates factor drop keeps first occurrence per label, in order");
    check(eq(dropped.col("v").f64[0], 1) && eq(dropped.col("v").f64[1], 2) && eq(dropped.col("v").f64[2], 4),
          "duplicates factor drop keeps the FIRST row's payload");
}

// L1 M3 native-verb campaign: rename/clonevar/collapse(factor-by)/encode/setcol —
// see the frame.h doc comment next to each Frame method for the exact rule.

static void test_rename() {
    std::printf("rename:\n");
    // basic: column order preserved, values byte-preserved, only names change
    {
        Frame f = make_frame({{"a", {1, 2}}, {"b", {3, 4}}, {"c", {5, 6}}}, 2);
        f.rename({{"a", "x"}, {"c", "z"}});
        std::vector<std::string> names;
        for (const auto& c : f.columns()) names.push_back(c.name);
        check(names == std::vector<std::string>({"x", "b", "z"}), "rename: column order preserved, names updated");
        check(eq(f.col("x").f64[0], 1) && eq(f.col("x").f64[1], 2), "rename: renamed column values preserved (a->x)");
        check(eq(f.col("z").f64[0], 5) && eq(f.col("z").f64[1], 6), "rename: renamed column values preserved (c->z)");
        check(eq(f.col("b").f64[0], 3), "rename: untouched column unaffected");
    }
    // swap: a<->b applies as if simultaneous
    {
        Frame f = make_frame({{"a", {1, 2}}, {"b", {10, 20}}}, 2);
        f.rename({{"a", "b"}, {"b", "a"}});
        check(eq(f.col("a").f64[0], 10) && eq(f.col("a").f64[1], 20), "rename: swap a<-b");
        check(eq(f.col("b").f64[0], 1) && eq(f.col("b").f64[1], 2), "rename: swap b<-a");
    }
    // factor preservation: dtype + dictionary + codes untouched by rename
    {
        Frame f;
        f.set_n_rows_hint(3);
        f.append_factor_column("k", {"x", "y", "x"});
        f.finalize_after_bulk_load();
        f.rename({{"k", "kk"}});
        check(f.has("kk") && !f.has("k"), "rename: factor column renamed");
        const FrameColumn& c = f.col("kk");
        check(c.is_factor, "rename: factor dtype preserved");
        check(c.levels.size() == 2 && c.levels[0] == "x" && c.levels[1] == "y",
              "rename: factor dictionary byte-preserved");
        check(c.codes[0] == 0 && c.codes[1] == 1 && c.codes[2] == 0, "rename: factor codes byte-preserved");
    }
    // atomicity: missing old throws, nothing applied
    {
        Frame f = make_frame({{"a", {1}}, {"b", {2}}}, 1);
        bool threw = false;
        try { f.rename({{"a", "x"}, {"nope", "y"}}); } catch (const std::exception&) { threw = true; }
        check(threw, "rename: missing old throws");
        check(f.has("a") && !f.has("x"), "rename: atomic — a valid-looking pair before the bad one is NOT applied");
    }
    // atomicity: duplicate/colliding new name throws
    {
        Frame f = make_frame({{"a", {1}}, {"b", {2}}, {"c", {3}}}, 1);
        bool threw = false;
        try { f.rename({{"a", "z"}, {"b", "z"}}); } catch (const std::exception&) { threw = true; }
        check(threw, "rename: duplicate target name throws");
        threw = false;
        try { f.rename({{"a", "c"}}); } catch (const std::exception&) { threw = true; }  // 'c' survives (untouched)
        check(threw, "rename: target colliding with a surviving column throws");
    }
}

static void test_clonevar() {
    std::printf("clonevar:\n");
    // basic: appended at END, values match source, source untouched
    {
        Frame f = make_frame({{"a", {1, 2, 3}}, {"b", {9, 8, 7}}}, 3);
        f.clonevar({{"a", "a2"}});
        std::vector<std::string> names;
        for (const auto& c : f.columns()) names.push_back(c.name);
        check(names == std::vector<std::string>({"a", "b", "a2"}), "clonevar: appended at END of column order");
        check(eq(f.col("a2").f64[0], 1) && eq(f.col("a2").f64[1], 2) && eq(f.col("a2").f64[2], 3),
              "clonevar: clone values match source");
        check(eq(f.col("a").f64[0], 1) && eq(f.col("a").f64[1], 2) && eq(f.col("a").f64[2], 3),
              "clonevar: source untouched");
    }
    // factor clone: dtype + full dictionary + codes preserved
    {
        Frame f;
        f.set_n_rows_hint(4);
        f.append_factor_column("k", {"lo", "hi", "lo", "mid"});
        f.finalize_after_bulk_load();
        f.clonevar({{"k", "k2"}});
        const FrameColumn& src = f.col("k");
        const FrameColumn& dst = f.col("k2");
        check(dst.is_factor, "clonevar: factor clone stays a factor");
        check(dst.levels.size() == src.levels.size(), "clonevar: factor clone dictionary same size");
        bool levels_match = true;
        for (size_t i = 0; i < src.levels.size(); ++i) if (dst.levels[i] != src.levels[i]) levels_match = false;
        check(levels_match, "clonevar: factor clone dictionary byte-preserved (same level strings)");
        bool codes_match = true;
        for (int64_t i = 0; i < f.n_rows(); ++i) if (dst.codes[i] != src.codes[i]) codes_match = false;
        check(codes_match, "clonevar: factor clone codes byte-preserved");
    }
    // atomic validation: dst exists / src missing -> throws, nothing appended
    {
        Frame f = make_frame({{"a", {1}}, {"b", {2}}}, 1);
        bool threw = false;
        try { f.clonevar({{"a", "b"}}); } catch (const std::exception&) { threw = true; }  // dst already exists
        check(threw, "clonevar: dst already exists throws");
        threw = false;
        try { f.clonevar({{"nope", "c"}}); } catch (const std::exception&) { threw = true; }  // src missing
        check(threw, "clonevar: missing src throws");
        check(f.n_cols() == 2, "clonevar: atomic — a failed call appends nothing");
    }
}

// One-group weighted percentile through Frame::collapse.
static double weighted_pct(const std::vector<double>& x, const std::vector<double>& w,
                           const std::string& wtype, const std::string& stat) {
    Frame f;
    f.set_n_rows_hint(static_cast<int64_t>(x.size()));
    f.append_f64_column("x", x);
    f.append_f64_column("w", w);
    f.finalize_after_bulk_load();
    Frame r = f.collapse({{stat, "x", "out"}}, {}, "w", wtype);
    return r.col("out").f64[0];
}

// StataBE 19.5 oracle each row separates the
// measured percentile rule from a near miss. Exact comparison: the rule is exact.
static void test_collapse_weighted_percentiles() {
    std::printf("collapse (weighted percentiles):\n");
    const std::vector<double> x12 = {1, 2};
    // Within 1e-6 below the target averages; just over it does not.
    check(weighted_pct(x12, {1, 1.000001}, "aw", "p50") == 1.5, "wpct: 5e-7 below target averages");
    check(weighted_pct(x12, {1, 1.000002}, "aw", "p50") == 2.0, "wpct: 1e-6+ below target selects");
    // Above the target by any amount selects: no tolerance on that side.
    check(weighted_pct(x12, {1.0000001, 1}, "aw", "p50") == 1.0, "wpct: 5e-8 above target selects");
    check(weighted_pct(x12, {1.000000000002, 1}, "pw", "p50") == 1.0, "wpct: 1e-12 above target selects");
    // Raw weight units for every type: no rescaling to the count, not relative to P.
    check(weighted_pct(x12, {1000, 1000.000004}, "aw", "p50") == 2.0, "wpct: tolerance is absolute (large)");
    check(weighted_pct(x12, {0.001, 0.001001}, "aw", "p50") == 1.5, "wpct: tolerance is absolute (small)");
    // Negative iweight total keeps the magnitudes; zero total has no percentile.
    check(weighted_pct(x12, {-1000, -1000.001}, "iw", "p50") == 2.0, "wpct: negative total negates");
    check(weighted_pct(x12, {-1.0000001, -1}, "iw", "p50") == 1.0, "wpct: negative total, above target");
    check(std::isnan(weighted_pct(x12, {1, -1}, "iw", "p50")), "wpct: zero total is missing");
    check(std::isnan(weighted_pct(x12, {1, -1}, "iw", "iqr")), "wpct: zero total iqr is missing");
    // A rounded cumulative above the target is above it: p30 selects, p10 averages.
    const std::vector<double> feq_x = {10, 20, 30, 40}, feq_w = {.1, .2, .3, .4};
    check(weighted_pct(feq_x, feq_w, "aw", "p10") == 15.0, "wpct: FEQ p10");
    check(weighted_pct(feq_x, feq_w, "aw", "p30") == 20.0, "wpct: FEQ p30");
    check(weighted_pct(feq_x, feq_w, "aw", "p60") == 30.0, "wpct: FEQ p60");
    check(weighted_pct(feq_x, feq_w, "aw", "iqr") == 20.0, "wpct: FEQ iqr");
    // The same pairs stored the other way round: the total sums in dataset order.
    check(weighted_pct({4, 3, 2, 1}, {.4, .3, .2, .1}, "aw", "p10") == 1.0, "wpct: descending storage p10");
    check(weighted_pct({1, 2, 3, 4}, {.1, .2, .3, .4}, "aw", "p10") == 1.5, "wpct: ascending storage p10");
    // 16+ groups run on pool workers, where a throw would terminate; a zero-total group
    // among them is simply missing.
    Frame many;
    std::vector<double> g, v, w;
    for (int grp = 0; grp < 20; ++grp) {
        g.insert(g.end(), {double(grp), double(grp)});
        v.insert(v.end(), {1.0, 2.0});
        w.insert(w.end(), {1.0, grp == 7 ? -1.0 : 3.0});
    }
    many.set_n_rows_hint(static_cast<int64_t>(g.size()));
    many.append_f64_column("g", g);
    many.append_f64_column("v", v);
    many.append_f64_column("w", w);
    many.finalize_after_bulk_load();
    Frame r = many.collapse({{"p50", "v", "m"}, {"iqr", "v", "q"}}, {"g"}, "w", "iw");
    check(r.n_rows() == 20, "wpct: 20 groups on the pool");
    // w = 1,3: p25 lands on W(1) (1.5), p50 and p75 select 2.
    check(r.col("m").f64[0] == 2.0 && r.col("q").f64[0] == 0.5, "wpct: pooled group value");
    check(std::isnan(r.col("m").f64[7]) && std::isnan(r.col("q").f64[7]), "wpct: pooled zero-total group");
}

static void test_collapse_factor_by() {
    std::printf("collapse (factor by-var):\n");
    // A factor by-column ("region") and a pre-converted f64 equivalent ("region_code",
    // the SAME grouping via raw first-seen codes) side by side — the two collapse()
    // calls MUST produce identical groups/stats (this is exactly the CSV-preconvert
    // round-trip PIET used to need for a factor by-var).
    Frame f;
    f.set_n_rows_hint(6);
    f.append_factor_column("region", {"east", "west", "east", "north", "west", "east"});
    f.append_f64_column("region_code", {0, 1, 0, 2, 1, 0});  // matches region's own first-seen codes
    f.append_f64_column("val", {10, 20, 30, 40, 50, 60});
    f.finalize_after_bulk_load();

    std::vector<Frame::CollapseSpec> specs = {{"sum", "val", "sum_val"}, {"mean", "val", "mean_val"}};
    Frame r_factor = f.collapse(specs, {"region"}, "");
    Frame r_numeric = f.collapse(specs, {"region_code"}, "");

    check(r_factor.n_rows() == r_numeric.n_rows(), "collapse factor-by: same group count as pre-converted f64");
    check(r_factor.n_rows() == 3, "collapse factor-by: 3 groups (east/west/north)");

    const FrameColumn& out_by = r_factor.col("region");
    check(out_by.is_factor, "collapse factor-by: OUTPUT by-column stays a factor");
    check(out_by.levels.size() == 3 && out_by.levels[0] == "east" && out_by.levels[1] == "west" &&
          out_by.levels[2] == "north",
          "collapse factor-by: fresh dictionary in first-seen output-row order (not alphabetical)");

    // Row-by-row equivalence vs the pre-converted-f64 path (both sort by the same
    // underlying codes, so row i lines up across the two results).
    for (int64_t i = 0; i < r_factor.n_rows(); ++i) {
        check(eq(r_factor.col("sum_val").f64[i], r_numeric.col("sum_val").f64[i]),
              "collapse factor-by: sum matches pre-converted-f64 equivalent");
        check(eq(r_factor.col("mean_val").f64[i], r_numeric.col("mean_val").f64[i]),
              "collapse factor-by: mean matches pre-converted-f64 equivalent");
    }
    // spot check actual values: east(rows0,2,5)=10+30+60=100; west(rows1,4)=20+50=70; north(row3)=40
    for (int64_t i = 0; i < r_factor.n_rows(); ++i) {
        std::string lbl = out_by.levels[static_cast<size_t>(out_by.codes[i])];
        if (lbl == "east") check(eq(r_factor.col("sum_val").f64[i], 100), "collapse factor-by: east sum=100");
        if (lbl == "west") check(eq(r_factor.col("sum_val").f64[i], 70), "collapse factor-by: west sum=70");
        if (lbl == "north") check(eq(r_factor.col("sum_val").f64[i], 40), "collapse factor-by: north sum=40");
    }

    // Mixed factor + numeric by-vars still group correctly (no double-count).
    Frame f2;
    f2.set_n_rows_hint(4);
    f2.append_factor_column("g", {"a", "a", "b", "b"});
    f2.append_f64_column("yr", {2000, 2001, 2000, 2001});
    f2.append_f64_column("v", {1, 2, 3, 4});
    f2.finalize_after_bulk_load();
    Frame r2 = f2.collapse({{"sum", "v", "sum_v"}}, {"g", "yr"}, "");
    check(r2.n_rows() == 4, "collapse factor-by: mixed factor+numeric by -> 4 groups");

    // Missing factor by-key forms its own group.
    Frame f3;
    f3.set_n_rows_hint(4);
    f3.append_factor_column("g", {"a", "", "a", ""}, /*empty_is_missing=*/true);
    f3.append_f64_column("v", {1, 2, 3, 4});
    f3.finalize_after_bulk_load();
    Frame r3 = f3.collapse({{"sum", "v", "sum_v"}}, {"g"}, "");
    check(r3.n_rows() == 2, "collapse factor-by: missing factor key forms its OWN group");
    bool saw_missing_group = false;
    for (int64_t i = 0; i < r3.n_rows(); ++i) {
        if (r3.col("g").codes[i] < 0) {
            saw_missing_group = true;
            check(eq(r3.col("sum_v").f64[i], 6), "collapse factor-by: missing-key group sum (2+4=6)");
        } else {
            check(eq(r3.col("sum_v").f64[i], 4), "collapse factor-by: 'a' group sum (1+3=4)");
        }
    }
    check(saw_missing_group, "collapse factor-by: a missing-key output row exists (itself factor-missing, code<0)");
}

static void test_encode() {
    std::printf("encode:\n");
    // alphabetical 1-based codes; an unused dictionary entry is excluded; an empty
    // level string counts as missing (encode's OWN rule — distinct from the general
    // engine convention where "" can be a genuine non-missing level, BUG-024).
    {
        Frame f;
        f.set_n_rows_hint(6);
        f.append_factor_column("fruit", {"banana", "apple", "", "cherry", "apple", "banana"},
                               /*empty_is_missing=*/false);  // "" is a REAL level here (like a .dta reader)
        f.finalize_after_bulk_load();
        f.mutable_col("fruit").levels.push_back("durian");  // in the dictionary, but never referenced -> unused

        auto levels = f.encode("fruit", "fruit_code");
        check(levels.size() == 3, "encode: n_levels excludes the unused level AND the empty-string level");
        check(levels[0] == "apple" && levels[1] == "banana" && levels[2] == "cherry",
              "encode: levels[] in alphabetical order");

        const FrameColumn& code = f.col("fruit_code");
        check(!code.is_factor, "encode: NEW column is f64");
        double expect[6] = {2, 1, NAN, 3, 1, 2};  // banana=2,apple=1,""=missing,cherry=3,apple=1,banana=2
        for (int i = 0; i < 6; ++i)
            check(eq(code.f64[i], expect[i]), "encode: alphabetical 1-based code, row " + std::to_string(i));
    }
    // SRC missing (negative code) -> NEW missing
    {
        Frame f;
        f.set_n_rows_hint(3);
        f.append_factor_column("k", {"b", "", "a"});  // default empty_is_missing=true -> "" is code -1
        f.finalize_after_bulk_load();
        auto levels = f.encode("k", "kc");
        check(levels.size() == 2 && levels[0] == "a" && levels[1] == "b",
              "encode: missing-coded row's level excluded from levels[]");
        const FrameColumn& code = f.col("kc");
        check(eq(code.f64[0], 2) && std::isnan(code.f64[1]) && eq(code.f64[2], 1),
              "encode: SRC missing (code -1) -> NEW missing");
    }
    // errors: non-factor SRC, dst already exists
    {
        Frame f = make_frame({{"x", {1, 2}}}, 2);
        bool threw = false;
        try { f.encode("x", "y"); } catch (const std::exception&) { threw = true; }
        check(threw, "encode: non-factor SRC throws bad_input");

        Frame f2;
        f2.set_n_rows_hint(2);
        f2.append_factor_column("k", {"a", "b"});
        f2.append_f64_column("existing", {1, 2});
        f2.finalize_after_bulk_load();
        threw = false;
        try { f2.encode("k", "existing"); } catch (const std::exception&) { threw = true; }
        check(threw, "encode: dst already exists throws");
    }
}

static void test_setcol() {
    std::printf("setcol:\n");
    const std::string csv_f64 = native_temp_path("cobra_test_setcol_f64.csv");
    const std::string csv_factor = native_temp_path("cobra_test_setcol_factor.csv");
    const std::string csv_short = native_temp_path("cobra_test_setcol_short.csv");
    const std::string csv_2rows = native_temp_path("cobra_test_setcol_2rows.csv");

    // Every fixture below opens BINARY so the exact byte stream written is the one
    // read back (Windows text mode would turn each \n into \r\n, and these cases pin
    // blank-line / quoted-"" record semantics). No-op on POSIX.

    // f64 patch: byte-preserves neighboring columns, replaces the target's values.
    {
        std::ofstream f(to_native_path(csv_f64), std::ios::binary);
        f << "100\n.\n300\n";  // 3 rows: 100, missing ("."), 300
    }
    Frame fr = make_frame({{"a", {1, 2, 3}}, {"b", {9, 8, 7}}}, 3);
    FrameColumn col = Frame::read_csv_column(csv_f64, "f64", fr.n_rows());
    check(!col.is_factor, "setcol: --dtype f64 read back as f64");
    check(eq(col.f64[0], 100) && std::isnan(col.f64[1]) && eq(col.f64[2], 300),
          "setcol: f64 values + '.' missing parsed correctly");
    fr.setcol("a", std::move(col), false);
    check(eq(fr.col("a").f64[0], 100) && std::isnan(fr.col("a").f64[1]) && eq(fr.col("a").f64[2], 300),
          "setcol: patched column has the new values");
    check(eq(fr.col("b").f64[0], 9) && eq(fr.col("b").f64[1], 8) && eq(fr.col("b").f64[2], 7),
          "setcol: NEIGHBOR column byte-preserved");
    check(fr.n_cols() == 2, "setcol: patch (no --new) doesn't change column count");

    // factor -> f64 retype ("destring"): dtype flips on an in-place patch.
    {
        Frame fr2;
        fr2.set_n_rows_hint(3);
        fr2.append_factor_column("k", {"x", "y", "z"});
        fr2.append_f64_column("other", {1, 2, 3});
        fr2.finalize_after_bulk_load();
        check(fr2.col("k").is_factor, "setcol: pre-check, 'k' starts as factor");
        FrameColumn newk = Frame::read_csv_column(csv_f64, "f64", fr2.n_rows());
        fr2.setcol("k", std::move(newk), false);
        check(!fr2.col("k").is_factor, "setcol: destring — dtype flips factor->f64 in place");
        check(eq(fr2.col("k").f64[0], 100) && eq(fr2.col("other").f64[1], 2),
              "setcol: destring values correct + neighbor untouched");
    }

    // factor dtype: fresh first-seen dictionary; empty cell -> missing; --new appends.
    {
        std::ofstream f(to_native_path(csv_factor), std::ios::binary);
        // Headerless single-column payload: a bare blank line AND a quoted ""
        // are both a missing-cell ROW (row 2 bare, row 4's sibling below covers
        // the quoted form) — never a skippable line break; a stray trailing
        // blank line trips the row-count check instead.
        f << "red\n\nblue\nred\n";  // 4 rows: red, <missing>, blue, red
        f.close();
        Frame fr3 = make_frame({{"a", {1, 2, 3, 4}}}, 4);
        FrameColumn newc = Frame::read_csv_column(csv_factor, "factor", fr3.n_rows());
        check(newc.is_factor, "setcol: --dtype factor read back as factor");
        check(newc.levels.size() == 2 && newc.levels[0] == "red" && newc.levels[1] == "blue",
              "setcol: factor dictionary built fresh, first-seen order");
        check(newc.codes[0] == 0 && newc.codes[1] == -1 && newc.codes[2] == 1 && newc.codes[3] == 0,
              "setcol: factor codes correct, empty cell -> missing (-1)");
        fr3.setcol("clr", std::move(newc), true);  // --new: append
        check(fr3.has("clr") && fr3.n_cols() == 2, "setcol: --new appends a brand-new column");
        check(fr3.col("clr").is_factor, "setcol: appended column keeps factor dtype");
    }

    // quoted "" is ALSO a missing-cell row (both spellings accepted).
    {
        const std::string csv_qe = native_temp_path("cobra_test_setcol_qe.csv");
        std::ofstream f(to_native_path(csv_qe), std::ios::binary);
        f << "\"\"\nx\n";  // 2 rows: <missing>, x
        f.close();
        FrameColumn qc = Frame::read_csv_column(csv_qe, "factor", 2);
        check(qc.codes.size() == 2 && qc.codes[0] == -1 && qc.codes[1] == 0 && qc.levels.size() == 1,
              "setcol: quoted \"\" row is missing, same as a bare blank line");
    }

    // row-count mismatch -> reject (bad_input), never silently truncate/pad.
    {
        std::ofstream f(to_native_path(csv_short), std::ios::binary);
        f << "1\n2\n";  // 2 rows; dataset has 3
        f.close();
        bool threw = false;
        try { Frame::read_csv_column(csv_short, "f64", 3); } catch (const std::exception&) { threw = true; }
        check(threw, "setcol: row-count mismatch rejected (bad_input)");
    }

    // --new on an existing NAME, and no --new on a missing NAME, both throw.
    {
        std::ofstream f(to_native_path(csv_2rows), std::ios::binary);
        f << "1\n2\n";
        f.close();
        Frame fr4 = make_frame({{"a", {1, 2}}}, 2);
        bool threw = false;
        try {
            FrameColumn c = Frame::read_csv_column(csv_2rows, "f64", 2);
            fr4.setcol("a", std::move(c), true);  // --new but 'a' already exists
        } catch (const std::exception&) { threw = true; }
        check(threw, "setcol: --new on an already-existing NAME throws bad_input");
        threw = false;
        try {
            FrameColumn c = Frame::read_csv_column(csv_2rows, "f64", 2);
            fr4.setcol("nope", std::move(c), false);  // no --new but 'nope' missing
        } catch (const std::exception&) { threw = true; }
        check(threw, "setcol: no --new on a missing NAME throws (missing_column)");
    }
}

// BUG-112(1) engine-half: `generate(name, expr, require_new)` — a bare (default-false)
// call keeps the pre-existing silent create-or-replace behavior (PIET's `replace` verb,
// and every other internal caller — winsor/bygen/egen/encode/reshape/merge cardinality —
// relies on exactly this, since ALL of them route through the same Frame::generate/
// cobra `--gen`); `require_new=true` (the engine's new opt-in, wired to `--gen-strict`
// on the CLI) is the ONLY thing that rejects re-declaring an existing name.
static void test_generate_exists_guard() {
    std::printf("generate (exists-guard, BUG-112(1)):\n");
    {
        Frame f = make_frame({{"a", {1, 2, 3}}}, 3);
        bool threw = false;
        try { f.generate("a", "a + 1", /*require_new=*/true); }
        catch (const std::exception&) { threw = true; }
        check(threw, "generate: require_new on an already-existing NAME throws");
        check(eq(f.col("a").f64[0], 1) && eq(f.col("a").f64[1], 2) && eq(f.col("a").f64[2], 3),
              "generate: a rejected require_new call leaves the existing column untouched");
        check(f.n_cols() == 1, "generate: a rejected require_new call appends nothing");
    }
    {
        Frame f = make_frame({{"a", {1, 2, 3}}}, 3);
        f.generate("b", "a + 1", /*require_new=*/true);
        check(f.n_cols() == 2, "generate: require_new on a genuinely NEW name succeeds");
        check(eq(f.col("b").f64[0], 2) && eq(f.col("b").f64[2], 4),
              "generate: require_new new-column values correct");
    }
    {
        // Default (no require_new) — unchanged silent replace, the behavior `replace`
        // (and every other internal --gen caller) depends on.
        Frame f = make_frame({{"a", {1, 2, 3}}}, 3);
        f.generate("a", "a * 10");
        check(eq(f.col("a").f64[0], 10) && eq(f.col("a").f64[2], 30),
              "generate: default (no require_new) still silently replaces — replace path unaffected");
        check(f.n_cols() == 1, "generate: default replace doesn't append a column");
    }
}

// BUG-272 (closes BUG-200): the Frame sort-state plane. One invariant — a carried
// claim is never FALSE; blank is always acceptable. Covers the mint conditions
// (gsort), the clear funnel (value writes on a key / drop-of-key), the carriers
// (keep_if / rename / order / clonevar), and the .hbin round trip of the claim.
static void test_sort_state_plane() {
    std::printf("sort-state plane (BUG-272):\n");
    using KV = std::vector<std::pair<std::string, std::vector<double>>>;
    const KV base = {{"g", {2, 1, 2, 1}}, {"v", {4, 3, 2, 1}}};

    // fresh frames carry no claim
    check(make_frame(base, 4).sorted_by().empty(), "fresh frame: no claim");

    // gsort mints on ascending f64 keys — single and multi
    {
        Frame r = make_frame(base, 4).gsort({"g"}, {true});
        check(r.sorted_by() == std::vector<std::string>{"g"}, "gsort asc mints claim");
        Frame r2 = make_frame(base, 4).gsort({"g", "v"}, {true, true});
        check((r2.sorted_by() == std::vector<std::string>{"g", "v"}), "gsort multi-key claim");
    }
    // Mixed directions retain their leading ascending prefix (Stata capture
    // w2e_probes.log:845 bo_gsort_marker); a descending-first sort has none.
    // Strings and extended missings use Stata order and carry truthful claims.
    {
        Frame r = make_frame(base, 4).gsort({"g", "v"}, {true, false});
        check(r.sorted_by() == std::vector<std::string>{"g"},
              "mixed directions: leading ascending prefix remains a truthful claim");
        Frame descending = make_frame(base, 4).gsort({"g", "v"}, {false, true});
        check(descending.sorted_by().empty(), "descending-first key has no ascending claim");
        Frame f;
        f.set_n_rows_hint(3);
        f.append_factor_column("s", {"b", "a", "c"});
        f.append_f64_column("x", {1, 2, 3});
        f.finalize_after_bulk_load();
        Frame fs = f.gsort({"s"}, {true});
        check(fs.sorted_by() == std::vector<std::string>{"s"} &&
                  fs.col("x").f64[0] == 2 && fs.col("x").f64[1] == 1 && fs.col("x").f64[2] == 3,
              "factor key: lexical order, independent of dictionary insertion; truthful claim");
        Frame fm = make_frame({{"m", {1, stata_missing(1), 2, stata_missing(0)}}}, 4);
        Frame ms = fm.gsort({"m"}, {true});
        check(ms.sorted_by() == std::vector<std::string>{"m"} &&
                  ms.col("m").f64[0] == 1 && ms.col("m").f64[1] == 2 &&
                  missing_kind(ms.col("m").f64[2]) == 0 && missing_kind(ms.col("m").f64[3]) == 1,
              "extended-missing key: finite < . < .a; truthful claim");
        Frame fp = make_frame({{"m", {1, stata_missing(0), 2}}}, 3);
        check(fp.gsort({"m"}, {true}).sorted_by() == std::vector<std::string>{"m"},
              "plain '.' missing alone still mints (NaN-last matches Stata)");
    }
    // set_sorted_by validates: unknown / duplicate keys install nothing
    {
        Frame f = make_frame(base, 4);
        f.set_sorted_by({"nope"});
        check(f.sorted_by().empty(), "set_sorted_by: unknown key installs nothing");
        f.set_sorted_by({"g", "g"});
        check(f.sorted_by().empty(), "set_sorted_by: duplicate key installs nothing");
        f.set_sorted_by({"g", "v"});
        check(f.sorted_by().size() == 2, "set_sorted_by: valid keys install");
    }
    // the clear funnel: value writes on a key clear, elsewhere preserve
    {
        Frame f = make_frame(base, 4).gsort({"g"}, {true});
        f.generate("w", "v*2");
        check(f.sorted_by() == std::vector<std::string>{"g"}, "gen NEW column preserves claim");
        f.generate("v", "v*2");
        check(f.sorted_by() == std::vector<std::string>{"g"}, "replacing a NON-key preserves claim");
        f.generate("g", "g*2");
        check(f.sorted_by().empty(), "replacing the KEY clears the claim");
    }
    {
        Frame f = make_frame(base, 4).gsort({"g"}, {true});
        f.drop("v");
        check(f.sorted_by() == std::vector<std::string>{"g"}, "drop non-key preserves");
        f.drop("g");
        check(f.sorted_by().empty(), "drop of a key clears");
    }
    // carriers: keep_if (stable subsequence), rename (payload identity), order,
    // clonevar/encode (pure appends)
    {
        Frame f = make_frame(base, 4).gsort({"g", "v"}, {true, true});
        f.keep_if("v > 1");
        check((f.sorted_by() == std::vector<std::string>{"g", "v"}),
              "keep_if preserves (subsequence of sorted is sorted)");
        f.rename({{"g", "grp"}});
        check((f.sorted_by() == std::vector<std::string>{"grp", "v"}),
              "renamed key follows the rename (BUG-191's payload-identity law)");
        f.clonevar({{"v", "v2"}});
        check(f.sorted_by().size() == 2, "clonevar (pure append) preserves");
        Frame o = f.order({"v"}, "first", "");
        check((o.sorted_by() == std::vector<std::string>{"grp", "v"}),
              "order (column permutation) carries the claim");
        Frame o2 = std::move(f).order({"v"}, "last", "");
        check((o2.sorted_by() == std::vector<std::string>{"grp", "v"}),
              "order rvalue overload carries too");
    }
    // fresh-frame transforms stay claimless (spot checks; safety is by construction)
    {
        Frame a = make_frame(base, 4).gsort({"g"}, {true});
        Frame b = make_frame({{"g", {3}}, {"v", {9}}}, 1);
        check(a.append(b).sorted_by().empty(), "append result: no claim (row concat)");
        int64_t added = 0;
        check(a.expand(2, &added).sorted_by().empty(), "expand result: no claim");
        int64_t dropped = 0;
        check(a.duplicates_drop({"g"}, &dropped).sorted_by().empty(),
              "duplicates_drop result: no claim (over-clearing is the safe default)");
    }
    // .hbin carriage: claim round-trips; claimless header carries NO key at all
    {
        const std::filesystem::path dir = std::filesystem::temp_directory_path();
        const std::string p1 = from_native_path(dir / "cobra_sortplane_claim.hbin");
        const std::string p2 = from_native_path(dir / "cobra_sortplane_none.hbin");
        Frame f = make_frame(base, 4).gsort({"g", "v"}, {true, true});
        f.write_hbin(p1);
        make_frame(base, 4).write_hbin(p2);
        Dataset d1 = Dataset::open(p1);
        check(d1.sorted_by() == "g v", "hbin header carries 'sorted_by' (space-joined)");
        Frame f1 = Frame::from_dataset(d1);
        check((f1.sorted_by() == std::vector<std::string>{"g", "v"}),
              "from_dataset re-installs the claim");
        Dataset d2 = Dataset::open(p2);
        check(d2.sorted_by().empty(), "claimless file: sorted_by absent");
        std::ifstream hf(to_native_path(p2), std::ios::binary);
        std::string bytes((std::istreambuf_iterator<char>(hf)), std::istreambuf_iterator<char>());
        check(bytes.find("sorted_by") == std::string::npos,
              "claimless header emits NO key (byte discipline: absent != empty)");
        // view parity: as_dataset_view carries the claim like a disk round trip
        Dataset v = f1.as_dataset_view();
        check(v.sorted_by() == "g v", "as_dataset_view carries the claim");
        native_unlink(p1);
        native_unlink(p2);
    }
    // BUG-532: sorting on a leading prefix of the claim moves no row (the prefix path
    // skips the radix pass) and the claim becomes the requested keys, as Stata's
    // `: sortedby` does (w2e_probes.log bo_sort_prefix_noop: "g x" -> `sort g` -> "g").
    // The rows must equal what the full sort returns on the same frame.
    {
        const KV tied = {{"g", {2, 1, 2, 1, 1, 2}}, {"b", {1, 1, 1, 2, 1, 2}},
                         {"x", {6, 5, 4, 3, 2, 1}}, {"w", {10, 20, 30, 40, 50, 60}}};
        const Frame s = make_frame(tied, 6).gsort({"g", "b", "x"}, {true, true, true});
        check((s.sorted_by() == std::vector<std::string>{"g", "b", "x"}), "prefix: base claim g b x");
        Frame unclaimed = s;
        unclaimed.clear_sorted_by();
        const std::vector<std::vector<std::string>> prefixes = {{"g"}, {"g", "b"}, {"g", "b", "x"}};
        for (const auto& keys : prefixes) {
            const std::vector<bool> asc(keys.size(), true);
            Frame fast = s.gsort(keys, asc);
            Frame full = unclaimed.gsort(keys, asc);
            check(fast.sorted_by() == keys,
                  "prefix: sort on a leading prefix claims the requested keys (bo_sort_prefix_noop)");
            check(full.sorted_by() == keys, "prefix: the full path mints the requested keys");
            bool same = fast.n_rows() == full.n_rows() && fast.n_cols() == full.n_cols();
            for (const char* c : {"g", "b", "x", "w"})
                for (int64_t r = 0; same && r < full.n_rows(); ++r)
                    same = eq(fast.col(c).f64[r], full.col(c).f64[r]);
            check(same, "prefix: rows equal the full sort's rows, column by column");
        }
        // not a leading prefix, or descending: the old path (sorts, mints or clears)
        check((s.gsort({"g", "x"}, {true, true}).sorted_by() == std::vector<std::string>{"g", "x"}),
              "prefix: a non-leading key list re-sorts and mints its own claim");
        check((s.gsort({"b"}, {true}).sorted_by() == std::vector<std::string>{"b"}),
              "prefix: a key that is not the claim's first re-sorts");
        Frame d = s.gsort({"g"}, {false});
        check(d.sorted_by().empty(), "prefix: a descending key takes the old path (no claim)");
        check(eq(d.col("g").f64[0], 2), "prefix: a descending key really re-sorts the rows");
        // longer than the claim: the old path
        Frame g1 = make_frame(tied, 6).gsort({"g"}, {true});
        check((g1.gsort({"g", "b"}, {true, true}).sorted_by() == std::vector<std::string>{"g", "b"}),
              "prefix: a key list longer than the claim sorts and mints it");
    }
    // BUG-532: a claim adopted from a .dta whose leading key is a STRING (factor)
    // column: the file's rows are in Stata's byte order. The dictionary here is in
    // another order ("c","a","b"), so a re-sort by dictionary code would move "c"
    // first and mint nothing; the prefix path returns the rows as they are (Stata's
    // answer) and claims the requested key, a prefix of a true claim.
    {
        Frame f;
        f.set_n_rows_hint(4);
        f.append_factor_column_coded("s", {1, 2, 2, 0}, {"c", "a", "b"});  // a b b c
        f.append_f64_column("v", {1, 2, 3, 4});
        f.finalize_after_bulk_load();
        f.set_sorted_by({"s", "v"});  // what dta_reader installs from <sortlist>
        check((f.sorted_by() == std::vector<std::string>{"s", "v"}), "factor claim: adopted");
        Frame r = f.gsort({"s"}, {true});
        check(r.sorted_by() == std::vector<std::string>{"s"},
              "factor claim: sort on its leading string key claims s");
        const char* want[4] = {"a", "b", "b", "c"};
        bool rows = r.n_rows() == 4;
        for (int64_t i = 0; rows && i < 4; ++i)
            rows = flabel(r, "s", i) == want[i] && eq(r.col("v").f64[i], static_cast<double>(i + 1));
        check(rows, "factor claim: rows stay in the file's (Stata's) order a b b c");
    }
}


// BUG-364: `_n` / `_N` / x[k] OUTSIDE a bysort context resolve against the
// DATASET, as Stata defines them. They used to throw "requires a bysort context"
// — an honest error for constructs Stata accepts (`keep if _n<=100` is a
// beginner idiom) — and the whole gate surface covered the plain form ZERO
// times: the only `_n`-in-a-filter case anywhere was the WORKING bysort variant.
// This test is that missing coverage, so the hole closes with the fix.
static void test_plain_obs_refs() {
    // v = 10,20,30,40,50 in row order.
    Frame f = make_frame({{"v", {10, 20, 30, 40, 50}}}, 5);

    // _n is the 1-based observation number.
    Frame a = f;
    a.generate("idx", "_n");
    check(a.col("idx").f64[0] == 1.0 && a.col("idx").f64[4] == 5.0,
          "BUG-364: bare _n is the 1-based row number");

    // _N is the row count, on every row.
    Frame b = f;
    b.generate("nn", "_N");
    check(b.col("nn").f64[0] == 5.0 && b.col("nn").f64[4] == 5.0,
          "BUG-364: bare _N is the dataset row count");

    // x[k] is the k-th observation, same value on every row; out of range = missing.
    Frame c = f;
    c.generate("first", "v[1]");
    c.generate("last", "v[_N]");
    c.generate("oob", "v[99]");
    check(c.col("first").f64[0] == 10.0 && c.col("first").f64[4] == 10.0,
          "BUG-364: v[1] is the first observation on every row");
    check(c.col("last").f64[0] == 50.0 && c.col("last").f64[4] == 50.0,
          "BUG-364: v[_N] is the last observation on every row");
    check(std::isnan(c.col("oob").f64[0]),
          "BUG-364: an out-of-range subscript is missing, not an error");

    // The filter form that started this: keep the first three rows.
    Frame d = f;
    d.keep_if("_n <= 3");
    check(d.n_rows() == 3, "BUG-364: `keep if _n <= 3` keeps exactly three rows");
    check(d.col("v").f64[0] == 10.0 && d.col("v").f64[2] == 30.0,
          "BUG-364: and they are the FIRST three, in order");
}

int test_data_verbs_main() {
    std::printf("test_data_verbs:\n");
    test_plain_obs_refs();
    test_gsort();
    test_order();
    test_append();
    test_append_factor_union();
    test_append_mixed_dtype();
    test_append_streaming();
    test_joinby();
    test_joinby_factor_keys();
    test_duplicates();
    test_recode();
    test_reshape();
    test_reshape_dup_and_dropna();
    test_merge();
    test_merge_factor_keys();
    test_duplicates_factor();
    test_rename();
    test_clonevar();
    test_collapse_factor_by();
    test_collapse_weighted_percentiles();
    test_encode();
    test_setcol();
    test_generate_exists_guard();
    test_sort_state_plane();
    std::printf("%s\n", fail ? "FAILED" : "PASSED");
    return fail;
}
