// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// B4/W6 egen riders: rank track/field/unique, var, nvals, mode, cut.
// Every expected value is a cell of the captured Stata oracle under
// tests/eval/golden/aer_b4/ — see the file header of test_b4_riders.cpp for which
// log carries which. Two fixtures are used, both entered exactly:
//
//   oracle_b/_c   10 rows built to have HEAVY TIES, because the first attempt used
//                 price/rep78, which have none — there `track` equalled the default
//                 and `maxmode` equalled the default, and nine records proved nothing.
//   egen-six      the 5 rows Stata's `input` actually produced in that capture (the
//                 do-file wrote 14 observations across 5 lines and Stata kept the
//                 first of each line; the log's own `list` is the authority, and it
//                 shows 5). It carries the by-group, icodes/label and group(2) cells.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

#include "data/frame.h"

using namespace cecon;

static int g_fail = 0;
static const double NA = std::nan("");
static bool same(double a, double b) {
    if (std::isnan(a) && std::isnan(b)) return true;
    if (std::isnan(a) || std::isnan(b)) return false;
    return std::fabs(a - b) <= 1e-12 * (std::fabs(b) + 1.0);
}
static void check(bool cond, const std::string& msg) {
    if (!cond) { std::printf("  FAIL: %s\n", msg.c_str()); g_fail = 1; }
}
static void want(const Frame& f, const std::string& name,
                 const std::vector<double>& gold, const std::string& tag) {
    const FrameColumn& c = f.col(name);
    for (size_t i = 0; i < gold.size(); ++i)
        if (!same(c.f64[i], gold[i])) {
            std::printf("  FAIL: %s: %s[%zu] got=%.17g want=%.17g\n", tag.c_str(),
                        name.c_str(), i, c.f64[i], gold[i]);
            g_fail = 1;
        }
}

// oracle_b's fixture, verbatim.
static Frame fixture_b() {
    Frame f;
    f.set_n_rows_hint(10);
    f.append_f64_column("m", {1, 1, 2, 2, 3, 3, 3, 4, 4, NA});
    f.append_f64_column("t", {1, 1, 2, 2, NA, NA, NA, NA, NA, NA});
    f.append_f64_column("r", {10, 10, 20, 30, 30, 30, 40, NA, NA, NA});
    f.append_f64_column("c", {0, 5, 10, 15, 20, 25, NA, NA, NA, NA});
    f.finalize_after_bulk_load();
    return f;
}
// The 5 observations the egen-six capture actually held (its log's `list`).
static Frame fixture_six() {
    Frame f;
    f.set_n_rows_hint(5);
    f.append_f64_column("g", {1, 1, 2, 2, 5});
    f.append_f64_column("v", {3, 1, 2, 7, 4});
    f.append_factor_column("s", {"b", "c", "d", "f", "y"});
    f.finalize_after_bulk_load();
    return f;
}

// Stata's %21x text -> the double (`.` and the extended missings -> NaN).
static double hx(const char* t) {
    const std::string s(t);
    const size_t xp = s.find('X');
    const long e = std::strtol(s.substr(xp + 1).c_str(), nullptr, 16);
    if (e == 0x3ff) return NA;
    const double m = std::strtod(("0x" + s.substr(1, xp - 1) + "p0").c_str(), nullptr);
    if (m == 0.0) return 0.0;
    return (s[0] == '-' ? -1.0 : 1.0) * std::ldexp(m, static_cast<int>(e));
}
static std::vector<double> hxs(const std::vector<const char*>& v) {
    std::vector<double> o;
    for (const char* t : v) o.push_back(hx(t));
    return o;
}
// Bit-for-bit: a float result is the float() of Stata's value, so nothing looser.
static void want_bits(const Frame& f, const std::string& name, const std::vector<double>& gold,
                      const std::string& tag) {
    const FrameColumn& c = f.col(name);
    for (size_t i = 0; i < gold.size(); ++i) {
        const double g = c.f64[i];
        const bool ok = (std::isnan(g) && std::isnan(gold[i])) || g == gold[i];
        if (!ok) {
            std::printf("  FAIL: %s: %s[%zu] got=%a want=%a\n", tag.c_str(), name.c_str(), i, g,
                        gold[i]);
            g_fail = 1;
        }
    }
}

// tests/eval/golden/aer_w2/w2ap_egen_family.do's fixture (20 rows), entered here:
// x and w1-w3 are float variables there, so they hold the float of each value; .a
// and .b are missing (NaN here). Only the columns the pins below read.
static Frame fixture_w2ap() {
    const auto fl = [](double v) { return static_cast<double>(static_cast<float>(v)); };
    Frame f;
    f.set_n_rows_hint(20);
    f.append_f64_column("g", {1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 3, 4, 4, 4, 5, 5, 5, 5, NA, NA});
    f.append_factor_column("s", {"a", "a", "a", "", "b", "b", "", "", "b", "b", "c", "c", "",
                                 "", "d", "d", "d", "d", "e", " "});
    f.append_f64_column("x", {3, 1, 3, NA, fl(0.7), 5, 4, 4, NA, -2, 7, NA, NA, NA, fl(1.5),
                              fl(2.1), fl(1.5), 10, 6, 8});
    f.append_f64_column("xd", {1.0 / 3, 2.0 / 3, 1.0 / 3, NA, 5.0 / 3, 2, 7.0 / 3, 7.0 / 3, NA,
                               -1.0 / 3, 11.0 / 3, NA, NA, NA, 5, 16.0 / 3, 5, 6, 19.0 / 3,
                               20.0 / 3});
    f.append_f64_column("w1", {1, NA, NA, NA, 2, 3, -1, 4, 5, 6, 7, NA, fl(1.5), 0, 2, 2, NA, 3,
                               1, fl(0.1)});
    f.append_f64_column("w2", {2, 2, NA, NA, 2, 1, -1, NA, 5, 7, NA, 8, fl(2.5), 0, 3, NA, NA,
                               3, 2, fl(0.2)});
    f.finalize_after_bulk_load();
    return f;
}

int test_b4_egen_main() {
    std::printf("test_b4_egen:\n");
    const std::vector<std::string> nby;

    // ---- rank: four tie rules off one fixture --------------------------------
    // oracle_b: rk_def[1]=1.5 rk_def[4]=5 · rk_trk[1]=1 rk_trk[4]=4 ·
    //           rk_fld[1]=6 rk_fld[4]=2 · rk_unq[1]=1 rk_unq[4]=4
    // oracle_c closes the track-vs-unique hole on the SECOND tied member:
    //           track_obs2=1 but unique_obs2=2, and track_obs5=4 vs unique_obs5=5.
    {
        Frame f = fixture_b();
        f.egen_by("rk", Frame::EgenFn::Rank, "r", nby);
        f.egen_by("rt", Frame::EgenFn::RankTrack, "r", nby);
        f.egen_by("rf", Frame::EgenFn::RankField, "r", nby);
        f.egen_by("ru", Frame::EgenFn::RankUnique, "r", nby);
        want(f, "rk", {1.5, 1.5, 3, 5, 5, 5, 7, NA, NA, NA}, "rank default");
        want(f, "rt", {1, 1, 3, 4, 4, 4, 7, NA, NA, NA}, "rank track");
        want(f, "rf", {6, 6, 5, 2, 2, 2, 1, NA, NA, NA}, "rank field");
        want(f, "ru", {1, 2, 3, 4, 5, 6, 7, NA, NA, NA}, "rank unique");
    }

    // ---- var / nvals (egenmore, ACCEPTED-NAMED) ------------------------------
    // oracle_c: egen var(r) equals `summarize`'s r(Var) = +1.0124924924925X+007
    // (the log's own egen cell is the FLOAT rounding of it — Stata's egen default
    // storage type is float — so the double is pinned against r(Var), which is the
    // same number at full precision). nvals(r) = 4.
    {
        Frame f = fixture_b();
        f.egen_by("vv", Frame::EgenFn::Var, "r", nby);
        f.egen_nvals("nv", {"r"}, nby);
        // %21x +1.0124924924925X+007 == 128.57142857142858 == 900/7 exactly:
        // sum=170, sum of squares=4900, SS = 4900 - 170^2/7 = 5400/7, /6 = 900/7.
        const double rvar = 900.0 / 7.0;
        check(std::fabs(f.col("vv").f64[0] - rvar) <= 1e-13 * rvar,
              "var(r) == summarize r(Var)");
        want(f, "nv", {4, 4, 4, 4, 4, 4, 4, 4, 4, 4}, "nvals(r)");
    }

    // ---- mode ----------------------------------------------------------------
    // oracle_b on t (modes 1 and 2 TIE, plus six missings):
    //   default -> missing · maxmode -> 2 · minmode -> 1 · missing -> `.` wins with 6
    // and on m (a unique mode) -> 3.
    {
        Frame f = fixture_b();
        Frame::ModeOpts plain, mx, mn, ms;
        mx.maxmode = true;
        mn.minmode = true;
        ms.missing_as_value = true;
        f.egen_mode("t_def", "t", nby, plain);
        f.egen_mode("t_max", "t", nby, mx);
        f.egen_mode("t_min", "t", nby, mn);
        f.egen_mode("t_mis", "t", nby, ms);
        f.egen_mode("m_def", "m", nby, plain);
        check(std::isnan(f.col("t_def").f64[0]), "mode tie -> missing");
        check(same(f.col("t_max").f64[0], 2), "mode maxmode -> 2");
        check(same(f.col("t_min").f64[0], 1), "mode minmode -> 1");
        check(std::isnan(f.col("t_mis").f64[0]), "mode missing: `.` wins with 6");
        check(same(f.col("m_def").f64[0], 3), "mode unique -> 3");
        // minmode and maxmode are mutually exclusive, as in Stata
        Frame::ModeOpts both;
        both.minmode = both.maxmode = true;
        bool threw = false;
        try { f.egen_mode("bad", "t", nby, both); } catch (const std::exception&) { threw = true; }
        check(threw, "mode: minmode+maxmode rejected");
    }

    // ---- cut at() boundaries -------------------------------------------------
    // oracle_b, at(0 10 20 30) on c = 0,5,10,15,20,25:
    //   0->0 5->0 10->10 15->10 20->20 25->20, and the missing tail stays missing.
    // The top edge is EXCLUSIVE (nothing can be 30), which is what makes the
    // interval left-closed/right-open rather than the reverse.
    {
        Frame f = fixture_b();
        Frame::CutOpts co;
        co.at = {0, 10, 20, 30};
        f.egen_cut("cc", "c", co);
        want(f, "cc", {0, 0, 10, 10, 20, 20, NA, NA, NA, NA}, "cut at()");
    }

    // ---- the egen-six by-group cells ----------------------------------------
    {
        Frame f = fixture_six();
        const std::vector<std::string> byg{"g"};
        f.egen_by("r1", Frame::EgenFn::Rank, "v", byg);
        f.egen_by("r3", Frame::EgenFn::RankField, "v", byg);
        f.egen_by("w1", Frame::EgenFn::Var, "v", byg);
        f.egen_nvals("n1", {"v"}, byg);
        f.egen_nvals("n3", {"s"}, {});
        f.egen_nvals("n4", {"g", "v"}, {});
        want(f, "r1", {2, 1, 1, 2, 1}, "six rank by(g)");
        want(f, "r3", {1, 2, 2, 1, 1}, "six field by(g)");
        want(f, "w1", {2, 2, 12.5, 12.5, NA}, "six var by(g)");
        want(f, "n1", {2, 2, 2, 2, 1}, "six nvals by(g)");
        want(f, "n3", {5, 5, 5, 5, 5}, "six nvals(s) — a string variable");
        want(f, "n4", {5, 5, 5, 5, 5}, "six nvals(g v) — distinct COMBINATIONS");

        Frame::ModeOpts mx, mn, n1, n2;
        mx.maxmode = true;
        mn.minmode = true;
        n1.nummode = 1;
        n2.nummode = 2;
        f.egen_mode("m3", "v", byg, mx);
        f.egen_mode("m2", "v", byg, mn);
        f.egen_mode("m4", "v", byg, n1);
        f.egen_mode("m5", "v", byg, n2);
        want(f, "m3", {3, 3, 7, 7, 4}, "six mode maxmode");
        want(f, "m2", {1, 1, 2, 2, 4}, "six mode minmode");
        want(f, "m4", {1, 1, 2, 2, 4}, "six mode nummode(1) == minmode");
        want(f, "m5", {3, 3, 7, 7, NA}, "six mode nummode(2)");
        // nummode(4) exceeds the mode count of EVERY group -> Stata r(198).
        Frame::ModeOpts n4o;
        n4o.nummode = 4;
        bool threw = false;
        try { f.egen_mode("m7", "v", byg, n4o); } catch (const std::exception&) { threw = true; }
        check(threw, "six mode nummode(4) rejected (Stata r(198))");
    }
    {
        // cut's three output shapes on the same edges: at() -> left edge,
        // icodes -> 0-based interval, label -> icodes plus one value label per
        // at() value INCLUDING the unreachable ceiling ("5-").
        Frame f = fixture_six();
        Frame::CutOpts at_only, ico, lab, grp;
        at_only.at = {1, 2, 3, 5};
        ico.at = at_only.at;
        ico.icodes = true;
        lab.at = at_only.at;
        lab.label = true;
        grp.group = 2;
        f.egen_cut("c1", "v", at_only);
        f.egen_cut("c2", "v", ico);
        f.egen_cut("c3", "v", lab);
        f.egen_cut("c4", "v", grp);
        want(f, "c1", {3, 1, 2, NA, 3}, "six cut at()");
        want(f, "c2", {2, 0, 1, NA, 2}, "six cut icodes");
        want(f, "c4", {1, 0, 0, 1, 1}, "six cut group(2)");
        const FrameColumn& c3 = f.col("c3");
        check(c3.is_factor, "cut label -> a labelled column");
        const std::vector<int32_t> gold_codes{2, 0, 1, -1, 2};
        for (size_t i = 0; i < gold_codes.size(); ++i)
            check(c3.codes[i] == gold_codes[i], "cut label icode row " + std::to_string(i));
        const std::vector<std::string> gold_lab{"1-", "2-", "3-", "5-"};
        check(c3.levels.size() == gold_lab.size(), "cut label: one label per at() value");
        for (size_t i = 0; i < gold_lab.size() && i < c3.levels.size(); ++i)
            check(c3.levels[i] == gold_lab[i],
                  "cut label text " + std::to_string(i) + " == " + gold_lab[i]);
        // at() and group() together is Stata's r(198).
        Frame::CutOpts bad;
        bad.at = at_only.at;
        bad.group = 2;
        bool threw = false;
        try { f.egen_cut("c8", "v", bad); } catch (const std::exception&) { threw = true; }
        check(threw, "cut: at()+group() rejected");
    }

    // ---- StataBE 19.5 oracle arms: engine cut labels and refusals ------------------
    {
        Frame f;
        f.set_n_rows_hint(4);
        f.append_f64_column("x", {0, 1, 13000000, NA});
        f.append_factor_column("s", {"a", "b", "c", "d"});
        f.finalize_after_bulk_load();
        // A_lab_big: a label is string(edge), %9.0g, integers included.
        Frame::CutOpts big;
        big.at = {0, 1, 12345678, 20000000};
        big.label = true;
        f.egen_cut("lab", "x", big);
        const FrameColumn& lab = f.col("lab");
        const std::vector<std::string> names{"0-", "1-", "1.23e+07-", "2.00e+07-"};
        check(lab.levels.size() == names.size(), "cut label: every edge is a level");
        for (size_t i = 0; i < names.size() && i < lab.levels.size(); ++i)
            check(lab.levels[i] == names[i], "cut label text " + names[i]);
        const std::vector<int32_t> codes{0, 1, 2, -1};
        for (size_t i = 0; i < codes.size(); ++i)
            check(lab.codes[i] == codes[i], "cut label code row " + std::to_string(i));
        // G_at1: one at() value is accepted and every row is missing.
        Frame::CutOpts one;
        one.at = {0};
        f.egen_cut("single", "x", one);
        want(f, "single", {NA, NA, NA, NA}, "cut, at(0)");
        // G_str: a string argument is r(109)'s text; D_str_tot, D_str_max: so are the row
        // functions'.
        const auto refuses_string = [&](const std::function<void()>& call, const std::string& tag) {
            bool named = false;
            try { call(); } catch (const std::exception& e) {
                named = std::string(e.what()).find("variable used where a number is required") !=
                        std::string::npos;
            }
            check(named, tag + ": type-mismatch text");
        };
        refuses_string([&] { f.egen_cut("c", "s", big); }, "cut(s)");
        refuses_string([&] { f.row_egen("t", Frame::RowFn::Total, {"x", "s"}); }, "rowtotal(x s)");
        refuses_string([&] { f.row_egen("m", Frame::RowFn::Max, {"x", "s"}); }, "rowmax(x s)");
    }

    // ---- w2ap_egen_family (StataNow 19.5): BUG-1001..1003 kernels ------------
    // Each `gold` is the arm's 20 records of w2ap_egen_family.log in %21x (log line
    // of its first record cited); float results are pinned bit-for-bit.
    {
        Frame f = fixture_w2ap();
        const std::vector<std::string> byg{"g"};
        Frame::EgenOpts prop;
        prop.pc_prop = true;
        // eg_pc_plain_prop (log:1689) `egen pc2 = pc(xd), prop`.
        f.egen_by("pc2", Frame::EgenFn::Pc, "xd", nby, 50.0, StoreType::Float, prop);
        want_bits(f, "pc2", hxs({"+1.cd85680000000X-008", "+1.cd85680000000X-007", "+1.cd85680000000X-008", "+1.0000000000000X+3ff", "+1.2073620000000X-005", "+1.5a240e0000000X-005", "+1.93d4bc0000000X-005", "+1.93d4bc0000000X-005", "+1.0000000000000X+3ff", "-1.cd85680000000X-008", "+1.3d4bb80000000X-004", "+1.0000000000000X+3ff", "+1.0000000000000X+3ff", "+1.0000000000000X+3ff", "+1.b0ad120000000X-004", "+1.cd85680000000X-004", "+1.b0ad120000000X-004", "+1.039b0a0000000X-003", "+1.1207360000000X-003", "+1.2073620000000X-003"}),
                  "pc(xd), prop");
        // eg_pc_byp_prop (log:1757) `bysort g: egen pc4 = pc(x), prop`: g3's single
        // row is 1, g4 (all missing) is missing.
        f.egen_by("pc4", Frame::EgenFn::Pc, "x", byg, 50.0, StoreType::Float, prop);
        want_bits(f, "pc4", hxs({"+1.e3c7900000000X-003", "+1.42850a0000000X-004", "+1.e3c7900000000X-003", "+1.0000000000000X+3ff", "+1.c3870e0000000X-005", "+1.93264c0000000X-002", "+1.5555560000000X-001", "+1.5555560000000X-001", "+1.0000000000000X+3ff", "-1.5555560000000X-002", "+1.0000000000000X+000", "+1.0000000000000X+3ff", "+1.0000000000000X+3ff", "+1.0000000000000X+3ff", "+1.96e32e0000000X-004", "+1.1cd2380000000X-003", "+1.96e32e0000000X-004", "+1.5312a60000000X-001", "+1.b6db6e0000000X-002", "+1.24924a0000000X-001"}),
                  "pc(x), prop by(g)");
        // eg_sum_byp_miss (log:3817) `by g: egen sm6 = sum(x), missing`: the
        // all-missing g4 is `.`; without `missing` it is 0.
        Frame::EgenOpts miss;
        miss.missing = true;
        f.egen_by("sm6", Frame::EgenFn::Sum, "x", byg, 50.0, StoreType::Float, miss);
        want_bits(f, "sm6", hxs({"+1.9666660000000X+003", "+1.9666660000000X+003", "+1.9666660000000X+003", "+1.9666660000000X+003", "+1.9666660000000X+003", "+1.9666660000000X+003", "+1.8000000000000X+002", "+1.8000000000000X+002", "+1.8000000000000X+002", "+1.8000000000000X+002", "+1.c000000000000X+002", "+1.0000000000000X+3ff", "+1.0000000000000X+3ff", "+1.0000000000000X+3ff", "+1.e333340000000X+003", "+1.e333340000000X+003", "+1.e333340000000X+003", "+1.e333340000000X+003", "+1.c000000000000X+003", "+1.c000000000000X+003"}),
                  "sum(x), missing by(g)");
        f.egen_by("sm0", Frame::EgenFn::Sum, "x", byg, 50.0, StoreType::Float);
        check(f.col("sm0").f64[11] == 0.0, "sum(x) by(g) without missing: all-missing g4 is 0");
        // eg_count_byopt_str (log:418) `egen c3 = count(s), by(g)`: "" is missing,
        // " " is not (the g == . group holds "e" and " ": 2).
        f.egen_by("c3", Frame::EgenFn::Count, "s", byg, 50.0, StoreType::Float);
        want_bits(f, "c3", {5, 5, 5, 5, 5, 5, 2, 2, 2, 2, 1, 1, 1, 1, 4, 4, 4, 4, 2, 2},
                  "count(s) by(g)");
        // eg_std_plain_ms2 (log:3547) `egen st6 = std(xd), mean(10) std(2)`.
        Frame::EgenOpts ms;
        ms.std_mean = 10.0;
        ms.std_sd = 2.0;
        f.egen_by("st6", Frame::EgenFn::Std, "xd", nby, 50.0, StoreType::Float, ms);
        want_bits(f, "st6", hxs({"+1.ea4d320000000X+002", "+1.fbfb820000000X+002", "+1.ea4d320000000X+002", "+1.0000000000000X+3ff", "+1.18833a0000000X+003", "+1.215a620000000X+003", "+1.2a318c0000000X+003", "+1.2a318c0000000X+003", "+1.0000000000000X+3ff", "+1.c6f0900000000X+002", "+1.4d8e2c0000000X+003", "+1.0000000000000X+3ff", "+1.0000000000000X+3ff", "+1.0000000000000X+3ff", "+1.70eace0000000X+003", "+1.79c1f60000000X+003", "+1.70eace0000000X+003", "+1.8b70480000000X+003", "+1.9447700000000X+003", "+1.9d1e980000000X+003"}),
                  "std(xd), mean(10) std(2)");
        // eg_rsum_m (log:2839) `egen rt2 = rsum(w1 w2), m`: an all-missing row is `.`.
        f.row_egen("rt2", Frame::RowFn::Total, {"w1", "w2"}, StoreType::Float, true);
        want_bits(f, "rt2", hxs({"+1.8000000000000X+001", "+1.0000000000000X+001", "+1.0000000000000X+3ff", "+1.0000000000000X+3ff", "+1.0000000000000X+002", "+1.0000000000000X+002", "-1.0000000000000X+001", "+1.0000000000000X+002", "+1.4000000000000X+003", "+1.a000000000000X+003", "+1.c000000000000X+002", "+1.0000000000000X+003", "+1.0000000000000X+002", "+0.0000000000000X-3ff", "+1.4000000000000X+002", "+1.0000000000000X+001", "+1.0000000000000X+3ff", "+1.8000000000000X+002", "+1.8000000000000X+001", "+1.3333340000000X-002"}),
                  "rsum(w1 w2), missing");
        // rownonmiss(), strok (007 Wave 3 cleaning.do:1463
        // `egen mobility_miss = rownonmiss($mobility), strok`). No oracle arm prints
        // it; the documented behaviour is the receipt: over numbers strok is inert ((x<.) and
        // !missing(x) agree), and it counts a string as !missing(s), i.e. s != "".
        f.row_egen("nm1", Frame::RowFn::NonMiss, {"w1", "w2"}, StoreType::Float, false, true);
        want_bits(f, "nm1", {2, 1, 0, 0, 2, 2, 2, 1, 2, 2, 1, 1, 2, 2, 2, 1, 0, 2, 2, 2},
                  "rownonmiss(w1 w2), strok");
        f.row_egen("nm2", Frame::RowFn::NonMiss, {"w1", "s"}, StoreType::Float, false, true);
        want_bits(f, "nm2", {2, 1, 1, 0, 2, 2, 1, 1, 2, 2, 2, 1, 1, 1, 2, 2, 1, 2, 2, 2},
                  "rownonmiss(w1 s), strok");
        // _growmiss.ado adds missing(x), which a string takes too.
        f.row_egen("ms2", Frame::RowFn::Miss, {"w1", "s"}, StoreType::Float);
        want_bits(f, "ms2", {0, 1, 1, 2, 0, 0, 1, 1, 0, 0, 0, 1, 1, 1, 0, 0, 1, 0, 0, 0},
                  "rowmiss(w1 s)");
        {
            bool threw_s = false;
            try {
                f.row_egen("nm3", Frame::RowFn::NonMiss, {"w1", "s"}, StoreType::Float);
            } catch (const std::exception& e) {
                threw_s = std::string(e.what()).find("variable used where a number is required") !=
                          std::string::npos;
            }
            check(threw_s, "rownonmiss(w1 s) without strok: type mismatch ((x<.) is r(109))");
        }
        // eg_seq_plain_rev (log:3277) `egen sq6 = seq(), from(5) to(1) block(2)`.
        f.egen_seq("sq6", nby, 5, 1, true, 2);
        want_bits(f, "sq6", {5, 5, 4, 4, 3, 3, 2, 2, 1, 1, 5, 5, 4, 4, 3, 3, 2, 2, 1, 1},
                  "seq(), from(5) to(1) block(2)");
        // No to(): Stata sets to = _N, so from(15) cycles 15..20 (no oracle
        // arm prints this form; the documented rule is the receipt).
        f.egen_seq("sq15", nby, 15, 0, false, 1);
        want_bits(f, "sq15", {15, 16, 17, 18, 19, 20, 15, 16, 17, 18, 19, 20, 15, 16, 17, 18, 19,
                              20, 15, 16},
                  "seq(), from(15): to() defaults to _N");
        bool threw = false;
        try { f.egen_seq("sq0", nby, 1, 0, false, 0); } catch (const std::exception&) { threw = true; }
        check(threw, "seq(), block(0) refused (eg_ref_seq_block: Stata exits 498)");
        // eg_mode_byp_str (log:1621) `bys g: egen mo11 = mode(s), maxmode`.
        Frame::ModeOpts mx;
        mx.maxmode = true;
        f.egen_mode("mo11", "s", byg, mx);
        const FrameColumn& mo = f.col("mo11");
        check(mo.is_factor, "mode(s): a string result");
        const char* gold_mo[] = {"a", "a", "a", "a", "a", "a", "b", "b", "b", "b",
                                 "c", "c", "c", "c", "d", "d", "d", "d", "e", "e"};
        for (size_t i = 0; i < 20; ++i) {
            const int32_t k = mo.codes[i];
            const std::string got = k >= 0 ? mo.levels[static_cast<size_t>(k)] : std::string();
            check(got == gold_mo[i], "mode(s), maxmode by(g) row " + std::to_string(i + 1) +
                                         " == " + gold_mo[i] + " (got '" + got + "')");
        }
    }

    std::printf(g_fail ? "  test_b4_egen FAILED\n" : "  test_b4_egen ok\n");
    return g_fail;
}
