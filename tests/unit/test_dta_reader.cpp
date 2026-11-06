// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Native .dta reader parity test — validates format 115/117/118 parsing
// against hand-verified oracle values captured from
// pandas.read_stata(path, convert_categoricals=False) (see
// tests/validate/dta_fixtures/ and the mission's differential harness for the
// full pandas cross-check; this unit test pins the expected numbers so a
// regression is caught even without the Python oracle installed).
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "io/dta_reader.h"
#include "validate_dir.h"

using namespace cecon;

static bool eq(double a, double b, double tol = 1e-9) {
    if (std::isnan(a) && std::isnan(b)) return true;
    if (std::isnan(a) != std::isnan(b)) return false;
    return std::fabs(a - b) <= tol * (std::fabs(b) + 1.0);
}

int fail = 0;
static void check(bool cond, const std::string& msg) {
    if (!cond) { std::printf("  FAIL: %s\n", msg.c_str()); fail = 1; }
}

// Validate the "mixed" fixture (id, sex[value-labeled], city[string], x[double,
// var-labeled], y[float], big[long], med[int], d[%td date], missing_test) —
// identical logical content saved as 115 / 117 / 118.
static void check_mixed(const std::string& path, const std::string& tag) {
    DtaResult r = read_dta(path);
    const Frame& f = r.frame;
    std::printf("  %s: format=%d rows=%lld cols=%d\n", tag.c_str(), r.format_version,
               (long long)f.n_rows(), f.n_cols());

    check(f.n_rows() == 8, tag + ": n_rows == 8");
    check(f.n_cols() == 9, tag + ": n_cols == 9");

    // id: 1..8, plain f64
    check(!f.col("id").is_factor, tag + ": id is f64");
    for (int i = 0; i < 8; ++i) check(eq(f.col("id").f64[i], i + 1), tag + ": id value");

    // sex: value-labeled BYTE -> stays f64 numeric code (0/1), NOT a factor
    // (oracle: convert_categoricals=False keeps numeric storage as f64).
    check(!f.col("sex").is_factor, tag + ": sex stays f64 (value-labeled numeric)");
    int expect_sex[8] = {1, 0, 1, 0, 1, 0, 1, 0};
    for (int i = 0; i < 8; ++i) check(eq(f.col("sex").f64[i], expect_sex[i]), tag + ": sex code");

    // city: genuine string -> factor
    check(f.col("city").is_factor, tag + ": city is factor");
    check(f.col("city").levels.size() == 8, tag + ": city has 8 distinct levels");

    // x: double, var-labeled — 1.6, 3.1, 4.6, ...
    check(!f.col("x").is_factor, tag + ": x is f64");
    for (int i = 0; i < 8; ++i) check(eq(f.col("x").f64[i], 1.5 * (i + 1) + 0.1, 1e-6), tag + ": x value");

    // y: float, _n/3.0
    for (int i = 0; i < 8; ++i) check(eq(f.col("y").f64[i], (i + 1) / 3.0, 1e-5), tag + ": y value");

    // big: long, _n*1000000
    for (int i = 0; i < 8; ++i) check(eq(f.col("big").f64[i], (i + 1) * 1000000.0), tag + ": big value");

    // med: int, _n*100
    for (int i = 0; i < 8; ++i) check(eq(f.col("med").f64[i], (i + 1) * 100.0), tag + ": med value");

    // d: %td date, mdy(1,_n,2020) -> Stata epoch-days since 1960-01-01.
    // 2020-01-01 is 21915 days after 1960-01-01 (verified: 60 years incl. 15 leap days -> 21915).
    for (int i = 0; i < 8; ++i) check(eq(f.col("d").f64[i], 21915.0 + i), tag + ": d epoch-days");

    // missing_test: NaN for rows 0-3, then 5,6,7,8 for rows 4-7
    for (int i = 0; i < 4; ++i) check(std::isnan(f.col("missing_test").f64[i]), tag + ": missing_test NaN");
    for (int i = 4; i < 8; ++i) check(eq(f.col("missing_test").f64[i], i + 1), tag + ": missing_test value");

    // metadata: var label on x, value label on sex
    bool found_x_label = false, found_sex_vl = false;
    for (const auto& vm : r.var_meta) {
        if (vm.name == "x") { found_x_label = (vm.label == "an x variable label"); }
        if (vm.name == "sex") { found_sex_vl = !vm.value_label_name.empty(); }
    }
    check(found_x_label, tag + ": x variable label captured");
    check(found_sex_vl, tag + ": sex value-label attachment captured");

    bool found_sexlbl_map = false;
    for (const auto& vl : r.value_labels) {
        if (vl.name == "sexlbl") {
            found_sexlbl_map = (vl.map.count(0) && vl.map.at(0) == "Female" &&
                                vl.map.count(1) && vl.map.at(1) == "Male");
        }
    }
    check(found_sexlbl_map, tag + ": sexlbl value-label table {0:Female,1:Male}");
}

// BUG-024 / BUG-153 re-adjudication (2026-08-06): a .dta string cell of ""
// reads back as factor code -1 -- the SAME convention Frame::from_csv uses --
// because in this engine `code -1` on a string column IS Stata's "".
//
// This REPLACES BUG-024's original expectation ("" as its own 6th level). That
// expectation was derived from pandas.read_stata, which is not the parity
// oracle. Re-measured against live StataNow 19.5 (import a CSV with one blank
// string cell of five, save .dta, use it back):
//     count if grp == ""  -> 1     count if missing(grp) -> 1
//     tabulate grp        -> 2 rows, Total 4 (the blank row is EXCLUDED)
//     codebook grp        -> "Unique values: 2   Missing \"\": 1/5"
// Stata counts "" as missing for every AGGREGATE while still comparing equal to
// "" -- exactly what code -1 gives us (PIET renders a -1 string code as "").
// Under the old level-per-"" reading, PIET answered tabulate Total 5 and
// codebook Unique 3 / Missing 0: two silently wrong aggregates.
//
// Fixture `empty_str_*.dta`: 6 rows, `name` = Name1, Name2, "", Name4, Name5,
// Name6 (row index 2 is the empty cell). What BUG-024 was right about is
// preserved and still asserted below: row 2 must READ BACK as "", never as a
// dropped row or as some other level's text.
static void check_empty_string_factor(const std::string& path, const std::string& tag) {
    DtaResult r = read_dta(path);
    const Frame& f = r.frame;
    std::printf("  %s: rows=%lld cols=%d\n", tag.c_str(), (long long)f.n_rows(), f.n_cols());

    check(f.n_rows() == 6, tag + ": n_rows == 6");
    check(f.col("name").is_factor, tag + ": name is factor");
    check(f.col("name").levels.size() == 5,
          tag + ": name has 5 non-empty levels (\"\" is code -1, not a level)");
    for (const std::string& lv : f.col("name").levels.ref())
        check(!lv.empty(), tag + ": no empty level string in the dictionary");

    // Exactly ONE row is missing, and it is row index 2 -- the aggregate Stata
    // reports as `Missing "": 1/6`. Any other count means the reader either
    // invented a missing cell or swallowed the blank into a neighbour's level.
    int64_t n_missing = 0;
    for (int64_t i = 0; i < f.n_rows(); ++i)
        if (f.col("name").codes[i] < 0) ++n_missing;
    check(n_missing == 1, tag + ": exactly 1 missing string cell");
    check(f.col("name").codes[2] < 0, tag + ": row 2 (the \"\" cell) is code -1");

    // Every row still READS BACK as its source text, with -1 rendering as ""
    // (the BUG-024 half that stands: the blank cell is not lost, and it is not
    // some other row's text either).
    const char* expect[6] = {"Name1", "Name2", "", "Name4", "Name5", "Name6"};
    for (int64_t i = 0; i < f.n_rows(); ++i) {
        const int32_t code = f.col("name").codes[i];
        std::string got = (code < 0) ? std::string() : f.col("name").levels[code];
        check(got == expect[i], tag + ": name row " + std::to_string(i) + " == \"" + expect[i] + "\"");
    }

    // val: plain double, unaffected by the string-column fix (sanity check
    // that only the string/factor path changed).
    check(!f.col("val").is_factor, tag + ": val stays f64");
    for (int64_t i = 0; i < f.n_rows(); ++i) {
        check(eq(f.col("val").f64[i], (i + 1) * 2.5), tag + ": val value");
    }
}

// BUG-072 regression: a format-117 strL cell whose GSO observation offset
// o >= 65536 must decode to its real value, not "". The data-side reference and
// the GSO header are both (v:u4, o:u4) for 117, but the old decoder read the
// ref as u32(o) + u16(v) + u16(pad) and dropped o's high 16 bits into `pad`, so
// any o >= 65536 missed the GSO map and decoded to "" (silent data loss, exit 0).
// Fixtures (frozen, from internal capture):
// strL column `s` = alpha,bravo,charlie,delta over 4 rows.
//   probe{117,118}.dta            o = 1,2,3,4                (small-o control)
//   bug072_fmt{117,118}_o65536.dta o = 65535,65536,65537,65538 (straddles 2**16)
// Row 0 (o=65535) is the in-file boundary control (last value < 2**16, decodes
// OK even pre-fix); rows 1-3 are the bug. The 118 twin must be unaffected
// (its (v:u2,o:u6) packing already survives o < 2**32), pinning that the fix is
// 117-scoped and does not regress the 118 path.
static void check_strl_o_boundary(const std::string& path, const std::string& tag) {
    DtaResult r = read_dta(path);
    const Frame& f = r.frame;
    std::printf("  %s: format=%d rows=%lld cols=%d\n", tag.c_str(), r.format_version,
               (long long)f.n_rows(), f.n_cols());
    const char* expect[4] = {"alpha", "bravo", "charlie", "delta"};
    check(f.n_rows() == 4, tag + ": n_rows == 4");
    check(f.has("s") && f.col("s").is_factor, tag + ": s is a strL factor column");
    for (int i = 0; i < 4; ++i) {
        int code = f.col("s").codes[i];
        std::string got = (code >= 0) ? f.col("s").levels[code] : std::string("<missing>");
        check(got == expect[i],
              tag + ": s row " + std::to_string(i) + " == \"" + expect[i] + "\" (got \"" + got + "\")");
    }
}

int test_dta_reader_main() {
    std::printf("test_dta_reader:\n");
    const cobra_test::ValidateBase base{"dta_fixtures/"};

    check_mixed(base + "mixed_115.dta", "mixed_115");
    check_mixed(base + "mixed_117.dta", "mixed_117");
    check_mixed(base + "mixed_118.dta", "mixed_118");

    check_empty_string_factor(base + "empty_str_115.dta", "empty_str_115");
    check_empty_string_factor(base + "empty_str_117.dta", "empty_str_117");
    check_empty_string_factor(base + "empty_str_118.dta", "empty_str_118");

    // Cross-format spot check on a REAL Stata-written dataset, one legacy-binary
    // container and one XML one.
    //
    // BUG-375: this used to read auto_{115,118}.dta — StataCorp's own bundled
    // `auto` dataset. It was the ONLY Stata-bundled asset anywhere in the test
    // tree, and it was the whole reason "can we ship the fixtures publicly" was a
    // LICENSING question rather than an engineering one (ADJ1 A4). Replaced on the
    // User's ruling with `fatalities`, a public-domain-derived panel this project
    // already ships in its own demo package.
    //
    // The replacement deliberately keeps every STRUCTURAL feature the swap could
    // otherwise have quietly dropped, because the point of this block is the
    // reader, not the data:
    //     a string column          auto: `make`     -> `state`      (str2)
    //     a value-labeled NUMERIC  auto: `foreign`  -> `breathcode` (long, 1=no 2=yes)
    //     a declared sort claim    auto: on foreign -> on state
    // Every expected value below was printed by the User's own StataBE run that
    // produced these files (tests/validate/fatalities_fixtures.log), so they are
    // pinned from the generating session rather than from a re-derivation here.
    for (const auto& p : {std::make_pair(base + "fatalities_115.dta", 115),
                          std::make_pair(base + "fatalities_118.dta", 118)}) {
        DtaResult r = read_dta(p.first);
        std::printf("  fatalities (%d): rows=%lld cols=%d\n", p.second,
                    (long long)r.frame.n_rows(), r.frame.n_cols());
        check(r.frame.n_rows() == 336, "fatalities: n_rows == 336");
        check(r.frame.has("state"), "fatalities: has state");
        check(r.frame.col("state").is_factor, "fatalities: state is factor (string)");
        check(r.frame.has("breathcode"), "fatalities: has breathcode");
        check(!r.frame.col("breathcode").is_factor,
              "fatalities: breathcode stays f64 (value-labeled NUMERIC, the `foreign` role)");
        // beertax is stored float, so the double read back is the float-rounded
        // value — 1.7142856121063232, not the 1.714285612 Stata printed at %20.10g.
        check(std::fabs(r.frame.col("beertax").f64[0] - 1.7142856121063232) < 1e-12,
              "fatalities: beertax[0] == 1.7142856121063232 (float storage)");
        // BUG-199: <timestamp> used to be parsed and discarded (dta_reader.cpp
        // c.raw(slen) with no assignment); both fixtures carry a real save
        // timestamp that must now reach DtaResult.
        check(r.timestamp == "31 Aug 2026 12:22",
              "fatalities: timestamp carried (BUG-199), got '" + r.timestamp + "'");
        // BUG-272 (closes BUG-200): both fixtures carry Stata's own sort claim,
        // previously parsed to advance the cursor and discarded ("skip; unused").
        // Pins BOTH container paths: 115 exercises parse_old_header's srtlist,
        // 118 the XML <sortlist>.
        check(r.frame.sorted_by() == std::vector<std::string>{"state"},
              "fatalities: sort claim 'state' carried (BUG-272)");
    }

    // BUG-272: the mixed fixtures were saved UNSORTED (all-zero sortlist) — the
    // no-claim side of the same read, all three container versions.
    for (const auto& p : {base + "mixed_115.dta", base + "mixed_117.dta", base + "mixed_118.dta"}) {
        check(read_dta(p).frame.sorted_by().empty(), "mixed: unsorted source carries no claim");
    }

    // BUG-072: strL GSO offset >= 65536, per-format decode.
    check_strl_o_boundary(base + "probe117.dta", "strl_o_probe117");            // small-o control
    check_strl_o_boundary(base + "probe118.dta", "strl_o_probe118");            // small-o control
    check_strl_o_boundary(base + "bug072_fmt117_o65536.dta", "strl_o_117");     // the bug (117)
    check_strl_o_boundary(base + "bug072_fmt118_o65536.dta", "strl_o_118");     // 118 twin, no regression

    std::printf("%s\n", fail ? "FAILED" : "PASSED");
    return fail;
}
