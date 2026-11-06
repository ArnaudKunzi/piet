// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// B4/W6 riders — `merge ..., update [replace] generate()` and the new egen
// functions. Every expected value below comes from the captured Stata oracle in
// tests/eval/golden/aer_b4/, never from a hand-derivation:
//
//   pin-oracle log i  merge update / update replace on a CLEAN fixture
//                          (one overlapping variable). The code distribution
//                          1/1/1/2/2 over codes 1..5, and the two data columns.
//   pin-oracle log h  the SAME test with a str3 marker that conflicts in
//                          every matched row. It is the falsifier that proves the
//                          _merge code is assigned per ROW over all overlapping
//                          variables: n3=0, n4=0, n5=5, while x2 and x4 are still
//                          FILLED from the using. Also generate(mm) (after which
//                          `confirm variable _merge` is r(111)) and
//                          keep(match master) nogenerate -> 6 of 7 rows.
//   pin-oracle log b  egen rank ties (default/track/field/unique), mode ties
//                          (default/maxmode/minmode/missing), cut at() boundaries.
//   pin-oracle log c  track-vs-unique on the SECOND tied member (batch B could
//                          not separate them: both give the first member the min),
//                          concat's string conventions, egenmore nvals/var.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "data/frame.h"

using namespace cecon;

static int g_fail = 0;
static void check(bool cond, const std::string& msg) {
    if (!cond) { std::printf("  FAIL: %s\n", msg.c_str()); g_fail = 1; }
}
static const double NA = std::nan("");
static bool same(double a, double b) {
    if (std::isnan(a) && std::isnan(b)) return true;
    if (std::isnan(a) || std::isnan(b)) return false;
    return std::fabs(a - b) <= 1e-12 * (std::fabs(b) + 1.0);
}
static void check_col(const Frame& f, const std::string& name,
                      const std::vector<double>& want, const std::string& tag) {
    if (!f.has(name)) { std::printf("  FAIL: %s: no column '%s'\n", tag.c_str(), name.c_str()); g_fail = 1; return; }
    const FrameColumn& c = f.col(name);
    if (f.n_rows() != static_cast<int64_t>(want.size())) {
        std::printf("  FAIL: %s: n_rows=%lld want=%zu\n", tag.c_str(),
                    static_cast<long long>(f.n_rows()), want.size());
        g_fail = 1;
        return;
    }
    for (size_t i = 0; i < want.size(); ++i)
        if (!same(c.f64[i], want[i])) {
            std::printf("  FAIL: %s: %s[%zu] got=%.17g want=%.17g\n", tag.c_str(), name.c_str(),
                        i, c.f64[i], want[i]);
            g_fail = 1;
        }
}
static std::string sval(const Frame& f, const std::string& name, int64_t row) {
    const FrameColumn& c = f.col(name);
    int32_t code = c.codes[row];
    return code < 0 ? std::string("<.>") : c.levels[static_cast<size_t>(code)];
}

// ---- the oracle_i fixture, exactly ------------------------------------------
// master: id 1..6, x = 10 . 30 . 50 60   using: id 1,2,3,4,5,7, x = 99 20 30 40 55 70
static Frame i_master() {
    Frame f;
    f.set_n_rows_hint(6);
    f.append_f64_column("id", {1, 2, 3, 4, 5, 6});
    f.append_f64_column("x", {10, NA, 30, NA, 50, 60});
    f.finalize_after_bulk_load();
    return f;
}
static Frame i_using() {
    Frame f;
    f.set_n_rows_hint(6);
    f.append_f64_column("id", {1, 2, 3, 4, 5, 7});
    f.append_f64_column("x", {99, 20, 30, 40, 55, 70});
    f.finalize_after_bulk_load();
    return f;
}
static MergeOpts base_opts() {
    MergeOpts o;
    o.type = "1:1";
    o.keylist = {"id"};
    return o;
}

int test_b4_merge_update_main() {
    std::printf("test_b4_merge_update:\n");

    // ---- oracle_i, `merge 1:1 id using ..., update` --------------------------
    // B4VAL|code1..code5| = 1 1 1 2 2 ; x = 10 20 30 40 50 60 70 ; N = 7.
    // x1 and x5 KEEP the master's 10 and 50 on a nonmissing conflict.
    {
        MergeOpts o = base_opts();
        o.update = true;
        MergeResult r = merge_frames(i_master(), i_using(), o);
        check(r.frame.n_rows() == 7, "update: N==7");
        check_col(r.frame, "x", {10, 20, 30, 40, 50, 60, 70}, "update");
        check_col(r.frame, "_merge", {5, 4, 3, 4, 5, 1, 2}, "update");
        check(r.master_only == 1 && r.using_only == 1 && r.matched == 5, "update: 1/1/5 split");
        check(r.matched_notupdated == 1, "update: code3 count == 1");
        check(r.matched_missupdated == 2, "update: code4 count == 2");
        check(r.matched_conflict == 2, "update: code5 count == 2");
    }

    // ---- oracle_i, `update replace` -----------------------------------------
    // B4VAL|x1_overwritten| = 99, |x5_overwritten| = 55, and the code counts are
    // UNCHANGED (code4=2, code5=2): replace changes data, not classification.
    {
        MergeOpts o = base_opts();
        o.update = true;
        o.replace = true;
        MergeResult r = merge_frames(i_master(), i_using(), o);
        check_col(r.frame, "x", {99, 20, 30, 40, 55, 60, 70}, "update replace");
        check_col(r.frame, "_merge", {5, 4, 3, 4, 5, 1, 2}, "update replace");
        check(r.matched_missupdated == 2 && r.matched_conflict == 2,
              "update replace: code distribution unchanged");
    }

    // `--replace` with no `--update` is the same update merge ([D] merge: "update
    // and replace both perform an update merge rather than a standard merge").
    {
        MergeOpts o = base_opts();
        o.replace = true;
        MergeResult r = merge_frames(i_master(), i_using(), o);
        check_col(r.frame, "x", {99, 20, 30, 40, 55, 60, 70}, "replace alone");
    }

    // ---- oracle_h: the per-ROW classification falsifier ----------------------
    // Same fixture + a str3 tag that is "M" in every master row and "U" in every
    // using row, and a using x that is MISSING at id 3. Stata: n1=1 n2=1 n3=0
    // n4=0 n5=5 — every match is a conflict because of tag — yet x2 and x4 are
    // still filled (20 and 40) and x3 keeps the master's 30. A per-VARIABLE code
    // would have printed n4=2 here.
    auto h_master = [] {
        Frame f;
        f.set_n_rows_hint(6);
        f.append_f64_column("id", {1, 2, 3, 4, 5, 6});
        f.append_f64_column("x", {10, NA, 30, NA, 50, 60});
        f.append_factor_column("tag", {"M", "M", "M", "M", "M", "M"});
        f.finalize_after_bulk_load();
        return f;
    };
    auto h_using = [] {
        Frame f;
        f.set_n_rows_hint(6);
        f.append_f64_column("id", {1, 2, 3, 4, 5, 7});
        f.append_f64_column("x", {99, 20, NA, 40, 55, 70});
        f.append_factor_column("tag", {"U", "U", "U", "U", "U", "U"});
        f.finalize_after_bulk_load();
        return f;
    };
    {
        MergeOpts o = base_opts();
        o.update = true;
        MergeResult r = merge_frames(h_master(), h_using(), o);
        check_col(r.frame, "x", {10, 20, 30, 40, 50, 60, 70}, "h update");
        check_col(r.frame, "_merge", {5, 5, 5, 5, 5, 1, 2}, "h update");
        check(r.matched_notupdated == 0 && r.matched_missupdated == 0 && r.matched_conflict == 5,
              "h update: 0/0/5 — the code is per ROW");
        check(sval(r.frame, "tag", 0) == "M", "h update: tag[1] stays M");
        check(sval(r.frame, "tag", 6) == "U", "h update: using-only row takes U");
    }
    {
        MergeOpts o = base_opts();
        o.update = true;
        o.replace = true;
        MergeResult r = merge_frames(h_master(), h_using(), o);
        check_col(r.frame, "x", {99, 20, 30, 40, 55, 60, 70}, "h replace");
        check(sval(r.frame, "tag", 0) == "U", "h replace: tag[1] becomes U");
        check(sval(r.frame, "tag", 4) == "U", "h replace: tag[5] becomes U");
        check(r.matched_conflict == 5, "h replace: code distribution unchanged");
    }

    // ---- oracle_h: generate(mm) RENAMES the marker ---------------------------
    // `confirm variable _merge` after generate(mm) is r(111): there is no _merge.
    {
        MergeOpts o = base_opts();
        o.generate = "mm";
        MergeResult r = merge_frames(h_master(), h_using(), o);
        check(r.frame.has("mm"), "generate(mm): marker named mm");
        check(!r.frame.has("_merge"), "generate(mm): _merge does NOT exist (Stata r(111))");
        check_col(r.frame, "mm", {3, 3, 3, 3, 3, 1, 2}, "generate(mm)");
    }

    // ---- oracle_h: keep(match master) nogenerate -> 6 of 7 rows, no marker ----
    {
        MergeOpts o = base_opts();
        o.keep_codes = {3, 1};
        o.nogenerate = true;
        MergeResult r = merge_frames(h_master(), h_using(), o);
        check(r.frame.n_rows() == 6, "keep(match master) nogenerate: 6 rows");
        check(!r.frame.has("_merge"), "keep(...) nogenerate: no marker column");
    }
    // With `update`, keep(match) must still mean codes 3, 4 AND 5 — an update
    // merge splits one match three ways, and dropping 4/5 would silently delete
    // the rows the update was for.
    {
        MergeOpts o = base_opts();
        o.update = true;
        o.keep_codes = {3};
        MergeResult r = merge_frames(i_master(), i_using(), o);
        check(r.frame.n_rows() == 5, "update + keep(match): all 5 matches survive");
        MergeOpts o2 = base_opts();
        o2.update = true;
        o2.keep_codes = {4};
        MergeResult r2 = merge_frames(i_master(), i_using(), o2);
        check(r2.frame.n_rows() == 2, "update + keep(4): only the two fills");
    }

    // ---- the standard merge is UNCHANGED -------------------------------------
    // No update: the using's copy of x is dropped, so the using-only row's x is
    // missing and every match is code 3.
    {
        MergeOpts o = base_opts();
        MergeResult r = merge_frames(i_master(), i_using(), o);
        check_col(r.frame, "x", {10, NA, 30, NA, 50, 60, NA}, "standard");
        check_col(r.frame, "_merge", {3, 3, 3, 3, 3, 1, 2}, "standard");
        check(r.matched_notupdated == 0 && r.matched_missupdated == 0 && r.matched_conflict == 0,
              "standard: no update split reported");
    }

    std::printf(g_fail ? "  test_b4_merge_update FAILED\n" : "  test_b4_merge_update ok\n");
    return g_fail;
}
