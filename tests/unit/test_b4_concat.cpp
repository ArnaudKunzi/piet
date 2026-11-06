// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// B4/W7: `egen concat` and the Stata %w.0g renderer it stands on.
//
// W6 declined to land concat and said why: its output is a STRING whose numeric
// fields are rendered by Stata's `string()` = %w.0g, cobra had no %g renderer, and
// the four cells then in hand were too thin to write one against. This test is the
// unblocking capture turned into a gate.
//
// Two oracles, both under tests/eval/golden/aer_b4/:
//   pin-oracle log g2  21 values x {default, %9.0g, %10.0g, %12.0g} = 84 cells,
//                           plus string(.) — the whole %g rule. EVERY cell that log
//                           carries is asserted below, entered by hand.
//   egen-six.log            the concat arms: kL_list's five rows x k1/k2/k8/k5/k9,
//                           the `k3` no-punct form, the batch-A `string()` cells, and
//                           k4's rc-198 maxlength rejection.
// A third, INDEPENDENT source is asserted too: StataCorp's own worked examples for
// strofreal() in fn.pdf p.171 (printed 167), which the egen-six REFUTE quoted.
//
// Anything below marked ADO is derived from egen concat's documented behaviour, not
// from a captured cell, and says so at the assertion.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "core/error.h"
#include "data/frame.h"

using namespace cecon;

static int g_fail = 0;
static const double NA = std::nan("");

static void eq(const std::string& got, const std::string& want, const std::string& tag) {
    if (got != want) {
        std::printf("  FAIL: %s: got=[%s] want=[%s]\n", tag.c_str(), got.c_str(), want.c_str());
        g_fail = 1;
    }
}
static void check(bool cond, const std::string& msg) {
    if (!cond) { std::printf("  FAIL: %s\n", msg.c_str()); g_fail = 1; }
}
// Read a text (factor) column's value at row i, the way a consumer would.
static std::string sval(const Frame& f, const std::string& name, int64_t i) {
    const FrameColumn& c = f.col(name);
    if (!c.is_factor) return "<NOT-A-STRING-COLUMN>";
    const int32_t k = c.codes[i];
    return k < 0 ? std::string() : c.levels[static_cast<size_t>(k)];
}

// ---- pin-oracle log g2, entered cell for cell -----------------------
// tag | value | default | %9.0g | %10.0g | %12.0g
struct GRow { const char* tag; double v; const char* d; const char* w9;
              const char* w10; const char* w12; };
static const GRow kG2[] = {
    {"int_1",           1,                  "1",        "1",        "1",         "1"},
    {"int_12",          12,                 "12",       "12",       "12",        "12"},
    {"int_123456",      123456,             "123456",   "123456",   "123456",    "123456"},
    {"int_1234567",     1234567,            "1234567",  "1234567",  "1234567",   "1234567"},
    {"int_12345678",    12345678,           "1.23e+07", "1.23e+07", "12345678",  "12345678"},
    {"int_123456789",   123456789,          "1.23e+08", "1.23e+08", "1.235e+08", "123456789"},
    {"int_1234567890",  1234567890.0,       "1.23e+09", "1.23e+09", "1.235e+09", "1234567890"},
    {"int_12345678901", 12345678901.0,      "1.23e+10", "1.23e+10", "1.235e+10", "1.23457e+10"},
    {"neg_12345678",    -12345678,          "-1.23e+07","-1.23e+07","-12345678", "-12345678"},
    {"pi",              3.14159265358979,   "3.141593", "3.141593", "3.1415927", "3.141592654"},
    {"small_p5",        0.5,                ".5",       ".5",       ".5",        ".5"},
    {"small_p25",       0.25,               ".25",      ".25",      ".25",       ".25"},
    {"tiny_1e_4",       0.0001,             ".0001",    ".0001",    ".0001",     ".0001"},
    {"tiny_1e_5",       0.00001,            ".00001",   ".00001",   ".00001",    ".00001"},
    {"tiny_1e_10",      0.0000000001,       "1.00e-10", "1.00e-10", "1.000e-10", "1.00000e-10"},
    {"big_1e15",        1000000000000000.0, "1.00e+15", "1.00e+15", "1.000e+15", "1.00000e+15"},
    {"big_1e20",        1e20,               "1.00e+20", "1.00e+20", "1.000e+20", "1.00000e+20"},
    {"third",           0.333333333333333,  ".3333333", ".3333333", ".33333333", ".3333333333"},
    {"near_int",        2.0000001,          "2",        "2",        "2.0000001", "2.0000001"},
    {"exact_half",      2.5,                "2.5",      "2.5",      "2.5",       "2.5"},
    {"neg_pi",          -3.14159265358979,  "-3.141593","-3.141593","-3.1415927","-3.141592654"},
};

// The 5 observations egen-six's `input` actually produced (its own kL_list is the
// authority: the do-file wrote 14 across 5 lines and Stata kept the first of each).
static Frame fixture_six() {
    Frame f;
    f.set_n_rows_hint(5);
    f.append_f64_column("g", {1, 1, 2, 2, 5});
    f.append_f64_column("v", {3, 1, 2, 7, 4});
    f.append_factor_column("s", {"b", "c", "d", "f", "y"});
    f.append_f64_column("big", {1234567, 7, 7, 7, 7});
    f.finalize_after_bulk_load();
    return f;
}

int test_b4_concat_main() {
    std::printf("test_b4_concat:\n");

    // ---- 1. every %g cell of pin-oracle log g2 --------------------------
    int cells = 0;
    for (const GRow& r : kG2) {
        eq(stata_string_of(r.v, ""),        r.d,   std::string(r.tag) + " default");
        eq(stata_format_g(r.v, 9),          r.w9,  std::string(r.tag) + " %9.0g");
        eq(stata_format_g(r.v, 10),         r.w10, std::string(r.tag) + " %10.0g");
        eq(stata_format_g(r.v, 12),         r.w12, std::string(r.tag) + " %12.0g");
        // The log's headline finding, asserted as its own claim rather than left
        // implicit in the two columns above: the DEFAULT is exactly %9.0g.
        eq(stata_string_of(r.v, ""), std::string(r.w9), std::string(r.tag) + " default==w9");
        cells += 4;
    }
    eq(stata_string_of(NA, ""), ".", "g2 missing: string(.)");
    ++cells;
    std::printf("  g2 cells asserted: %d\n", cells);

    // ---- 2. egen-six batch A: string() with and without a format -------------
    eq(stata_string_of(3, "%9.2f"), "3.00", "len_f92: string(3,\"%9.2f\") is NOT padded");
    check(stata_string_of(3, "%9.2f").size() == 4, "len_f92: length is 4, not 9");
    eq(stata_string_of(3, "%20.15f"), "3.000000000000000", "v2015");
    check(stata_string_of(3, "%20.15f").size() == 17, "len_f2015: length is 17, not 20");
    eq(stata_string_of(3, ""), "3", "v3");
    eq(stata_string_of(NA, ""), ".", "vmiss");
    eq(stata_string_of(1234567, ""), "1234567", "batch-A a");
    eq(stata_string_of(12345678, ""), "1.23e+07", "batch-A b");
    eq(stata_string_of(123456789, ""), "1.23e+08", "batch-A c");
    eq(stata_string_of(12345678, "%9.0g"), "1.23e+07", "batch-A g9");
    eq(stata_string_of(12345678, "%10.0g"), "12345678", "batch-A g10");
    check(stata_string_of(12345678, "").size() == 8, "len_1e7: length 8");

    // ---- 3. fn.pdf p.171, an INDEPENDENT source (StataCorp's own examples) ----
    eq(stata_string_of(4, "%9.2f"), "4.00", "fn.pdf strofreal(4,\"%9.2f\")");
    eq(stata_string_of(123456789, "%11.0g"), "123456789", "fn.pdf strofreal(...,\"%11.0g\")");
    eq(stata_string_of(1234567, ""), "1234567", "fn.pdf strofreal(1234567)");
    eq(stata_string_of(12345678, ""), "1.23e+07", "fn.pdf strofreal(12345678)");

    // ---- 4. egen-six kL_list: the concat arms, row by row ---------------------
    {
        Frame f = fixture_six();
        Frame::ConcatOpts o1; o1.punct = "_";
        f.egen_concat("k1", {"v", "s"}, o1);
        Frame::ConcatOpts o2; o2.punct = ","; o2.format = "%9.2f";
        f.egen_concat("k2", {"s", "v"}, o2);       // formatted number in the INTERIOR
        f.egen_concat("k8", {"v", "s"}, o2);       // ... and in the LEADING position
        Frame::ConcatOpts o5; o5.punct = ","; o5.format = "%20.15f";
        f.egen_concat("k5", {"s", "v"}, o5);
        Frame::ConcatOpts o3;                       // no punct at all
        f.egen_concat("k3", {"g", "v", "s"}, o3);
        Frame::ConcatOpts o9; o9.punct = "_";
        f.egen_concat("k9", {"g", "big"}, o9);

        const char* k1[] = {"3_b", "1_c", "2_d", "7_f", "4_y"};
        const char* k2[] = {"b,3.00", "c,1.00", "d,2.00", "f,7.00", "y,4.00"};
        const char* k8[] = {"3.00,b", "1.00,c", "2.00,d", "7.00,f", "4.00,y"};
        const char* k5[] = {"b,3.000000000000000", "c,1.000000000000000",
                            "d,2.000000000000000", "f,7.000000000000000",
                            "y,4.000000000000000"};
        const char* k3[] = {"13b", "11c", "22d", "27f", "54y"};
        const char* k9[] = {"1_1234567", "1_7", "2_7", "2_7", "5_7"};
        for (int64_t i = 0; i < 5; ++i) {
            eq(sval(f, "k1", i), k1[i], "kL_list k1[" + std::to_string(i + 1) + "]");
            eq(sval(f, "k2", i), k2[i], "kL_list k2[" + std::to_string(i + 1) + "]");
            eq(sval(f, "k8", i), k8[i], "kL_list k8[" + std::to_string(i + 1) + "]");
            eq(sval(f, "k5", i), k5[i], "kL_list k5[" + std::to_string(i + 1) + "]");
            eq(sval(f, "k3", i), k3[i], "kM k3[" + std::to_string(i + 1) + "]");
            eq(sval(f, "k9", i), k9[i], "kL_list k9[" + std::to_string(i + 1) + "]");
        }
        // The three captured LENGTH cells (%21x in the log): k2_len1 = 6,
        // k5_len1 = 19 (0x1.3p+4), k9_len2 = 3 (0x1.8p+1).
        check(sval(f, "k2", 0).size() == 6, "k2_len1 == 6");
        check(sval(f, "k5", 0).size() == 19, "k5_len1 == 19");
        check(sval(f, "k9", 1).size() == 3, "k9_len2 == 3");
        // The whole point of k2-vs-k8: format() reaches the NUMERIC field only, and
        // the string field is appended raw wherever it sits.
        check(sval(f, "k2", 0) == "b,3.00" && sval(f, "k8", 0) == "3.00,b",
              "format() applies to the numeric in either position, never to the string");
    }

    // ---- 5. the sci-notation switch INSIDE a join key -------------------------
    // The REFUTE's corpus-critical shape: every concat key in the 12 papers is built
    // from numerics, so a key silently becomes scientific past 7 significant digits.
    // Both renderings here are captured cells (batch-A `a` and `b`).
    {
        Frame f;
        f.set_n_rows_hint(2);
        f.append_f64_column("id", {1234567, 12345678});
        f.append_factor_column("t", {"x", "x"});
        f.finalize_after_bulk_load();
        Frame::ConcatOpts o; o.punct = "_";
        f.egen_concat("key", {"id", "t"}, o);
        eq(sval(f, "key", 0), "1234567_x", "join key below the budget");
        eq(sval(f, "key", 1), "1.23e+07_x", "join key ABOVE the budget goes scientific");
    }

    // ---- 6. ADO-derived shapes (mechanism, not a captured cell) --------------
    // egen concat removes exactly ONE punct copy and then trims the two ends.
    // The 6-row fixture that would have measured this never materialised (Stata kept
    // the first observation of each `input` line), so these assert the mechanism.
    {
        Frame f;
        f.set_n_rows_hint(2);
        f.append_f64_column("v", {NA, 3});
        // empty_is_missing=false: Stata's "" is genuine string data (BUG-024).
        f.append_factor_column("s", {"", "  pad  "}, false);
        f.finalize_after_bulk_load();
        Frame::ConcatOpts o; o.punct = "_";
        f.egen_concat("k", {"v", "s"}, o);
        // ADO (egen concat): "" + "." + "_" + "" + "_" -> ".__", minus ONE
        // punct copy -> "._", trim -> "._". An empty LAST field keeps a dangling
        // separator, so "12345_" and "12345" are different join keys.
        eq(sval(f, "k", 0), "._", "ADO: missing numeric + empty trailing field -> '._'");
        // ADO: trim() strips the two ENDS only; the interior blank survives.
        eq(sval(f, "k", 1), "3_  pad", "ADO: trim() is end-only, interior blanks survive");
    }

    // ---- 7. the refusals ------------------------------------------------------
    {
        Frame f = fixture_six();
        // egen-six k4: maxlength(99999) is "invalid maxlength( )", rc 198.
        bool threw = false;
        try { Frame::ConcatOpts o; o.maxlength = 99999; f.egen_concat("bad", {"v"}, o); }
        catch (const EngineError&) { threw = true; }
        check(threw, "k4: maxlength(99999) is rejected");
        // ... and a VALID one is accepted (the same branch must not over-reject).
        threw = false;
        try { Frame::ConcatOpts o; o.maxlength = 32000; f.egen_concat("ok", {"v"}, o); }
        catch (const EngineError&) { threw = true; }
        check(!threw, "maxlength(32000) is accepted (c(maxvlabellen))");
        // decode renders through a VALUE LABEL, which this build does not carry.
        threw = false;
        try { Frame::ConcatOpts o; o.decode = true; f.egen_concat("d", {"v"}, o); }
        catch (const EngineError&) { threw = true; }
        check(threw, "decode is refused, not silently ignored");
        // A format with no captured rendering is refused rather than guessed.
        threw = false;
        try { (void)stata_string_of(1.5, "%9.3e"); }
        catch (const EngineError&) { threw = true; }
        check(threw, "an unpinned %e format is refused, not guessed");
    }

    std::printf(g_fail ? "  RESULT: FAIL\n" : "  RESULT: ok\n");
    return g_fail;
}
