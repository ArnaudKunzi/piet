// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// D2 unit tests: Stata-style expression funcs, egen by-group, bygen subscripting,
// summarize-detail percentiles. Vectors from the semantics spec. Exercises the
// real Frame path via tiny round-tripped .hbin files.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <vector>

#include "data/frame.h"
#include "io/columnar.h"
#include "io/win_compat.h"
#include "nlohmann/json.hpp"

using namespace cecon;
using nlohmann::json;
static int g_fail = 0;

static void approx(const char* what, double got, double want, double tol = 1e-12) {
    bool bad = std::isnan(want) ? !std::isnan(got)
                                : std::fabs(got - want) / (std::fabs(want) + 1e-30) > tol;
    if (bad) { std::printf("  FAIL %s: got %.15g want %.15g\n", what, got, want); ++g_fail; }
}

static void write_hbin_f64(const std::string& path, int64_t n,
                           const std::vector<std::pair<std::string, std::vector<double>>>& cols) {
    constexpr int64_t A = 64;
    auto up = [](int64_t x) { return (x + A - 1) / A * A; };
    json h; h["magic"] = "CECONHB1"; h["version"] = 1; h["n_rows"] = n;
    json jc = json::array(); int64_t rel = 0;
    for (auto& c : cols) {
        int64_t nb = (int64_t)c.second.size() * 8;
        jc.push_back({{"name", c.first}, {"dtype", "f64"}, {"offset", rel}, {"nbytes", nb}});
        rel = up(rel + nb);
    }
    h["columns"] = jc;
    std::string hj = h.dump();
    std::ofstream f(to_native_path(path), std::ios::binary);
    f.write("CECONHB1", 8);
    uint64_t hl = hj.size(); f.write((const char*)&hl, 8); f.write(hj.data(), hj.size());
    int64_t ds = up(16 + (int64_t)hl), pos = 16 + (int64_t)hl;
    std::vector<char> pad(A, 0);
    if (pos < ds) { f.write(pad.data(), ds - pos); pos = ds; }
    for (size_t i = 0; i < cols.size(); ++i) {
        int64_t want = ds + jc[i]["offset"].get<int64_t>();
        if (pos < want) { f.write(pad.data(), want - pos); pos = want; }
        f.write((const char*)cols[i].second.data(), (int64_t)cols[i].second.size() * 8);
        pos += (int64_t)cols[i].second.size() * 8;
    }
}

static Frame makeframe(const std::string& path, int64_t n,
                       const std::vector<std::pair<std::string, std::vector<double>>>& cols) {
    write_hbin_f64(path, n, cols);
    Dataset ds = Dataset::open(path);
    return Frame::from_dataset(ds);
}

int test_d2_main() {
    std::printf("test_d2:\n");
    const double NA = std::nan("");

    // ---- §5.1 expression functions (1-row frame; expressions are literal) ----
    {
        Frame fr = makeframe(native_temp_path("cecon_d2_e.hbin"), 1, {{"d", {0}}});
        auto E = [&](const char* expr) { fr.generate("r", expr); return fr.col("r").f64[0]; };
        approx("cond t", E("cond(1>0,10,20)"), 10);
        approx("cond f", E("cond(0>1,10,20)"), 20);
        approx("cond miss4", E("cond(.,10,20,99)"), 99);
        approx("cond 0->b", E("cond(0,1,0,.)"), 0);
        approx("cond 1->a", E("cond(1,1,0,.)"), 1);
        approx("cond .>2", E("cond(.>2,50,70)"), 50);   // Option B: .>2 is true
        approx(".>2", E(".>2"), 1);
        approx("2>.", E("2>."), 0);
        approx(".==.", E(".==."), 1);
        approx(".<.", E(".<."), 0);
        approx("inlist hit", E("inlist(1,0,1,2)"), 1);
        approx("inlist miss", E("inlist(3,0,1,2)"), 0);
        approx("inlist . .", E("inlist(.,.,.)"), 1);
        approx("inrange in", E("inrange(1,0,2)"), 1);
        approx("inrange z.", E("inrange(.,-1,1)"), 0);
        approx("inrange lo.", E("inrange(0,.,1)"), 1);
        approx("inrange hi.", E("inrange(1,0,.)"), 1);
        approx("inrange both.", E("inrange(0,.,.)"), 1);
        approx("min skipmiss", E("min(2,10,.,7)"), 2);
        approx("max skipmiss", E("max(2,10,.,7)"), 10);
        approx("min all .", E("min(.,.,.)"), NA);
        approx("missing(1)", E("missing(1)"), 0);
        approx("missing(.)", E("missing(.)"), 1);
        approx("missing(1,.,3)", E("missing(1,.,3)"), 1);
        approx("asinh(2)", E("asinh(2)"), 1.4436354751788103);
        approx("ln1p", E("ln1p(0.001)"), 0.0009995003330835332);
        approx("ln1m", E("ln1m(-0.001)"), 0.0009995003330835332);
    }

    // ---- §5.2 egen by-group ----
    {
        Frame fr = makeframe(native_temp_path("cecon_d2_g1.hbin"), 5,
                             {{"x", {1, 2, NA, 4, 5}}, {"grp", {1, 1, 1, 1, 1}}});
        fr.egen_by("m", Frame::EgenFn::Mean, "x", {"grp"});
        for (int i = 0; i < 5; ++i) approx("egen mean", fr.col("m").f64[i], 3);
    }
    {
        Frame fr = makeframe(native_temp_path("cecon_d2_g2.hbin"), 5,
                             {{"x", {1, 2, NA, NA, NA}}, {"grp", {1, 1, 2, 2, 2}}});
        fr.egen_by("s", Frame::EgenFn::Sum, "x", {"grp"});
        approx("egen sum g1", fr.col("s").f64[0], 3);
        approx("egen sum allmiss=0", fr.col("s").f64[2], 0);  // all-missing sum is 0
    }
    {
        Frame fr = makeframe(native_temp_path("cecon_d2_med.hbin"), 4,
                             {{"x", {10, 20, 30, 40}}});
        fr.egen_by("md", Frame::EgenFn::Median, "x", {});
        approx("egen median even", fr.col("md").f64[0], 25);
    }
    {
        Frame fr = makeframe(native_temp_path("cecon_d2_mo.hbin"), 5,
                             {{"x", {10, 20, 30, 40, 50}}});
        fr.egen_by("md", Frame::EgenFn::Median, "x", {});
        approx("egen median odd", fr.col("md").f64[0], 30);
    }
    {
        Frame fr = makeframe(native_temp_path("cecon_d2_sd.hbin"), 3,
                             {{"x", {10, 20, 30}}, {"grp", {1, 2, 3}}});
        fr.egen_by("sdv", Frame::EgenFn::Sd, "x", {"grp"});
        approx("egen sd singleton=NA", fr.col("sdv").f64[0], NA);
    }
    {
        // onset = min(cond(la==1, wv, .)) by h_pid
        Frame fr = makeframe(native_temp_path("cecon_d2_on.hbin"), 6,
                             {{"h_pid", {1, 1, 1, 2, 2, 2}}, {"wv", {1, 2, 3, 1, 2, 3}},
                              {"la", {0, 1, 1, 0, 0, 1}}});
        fr.egen_by("onset", Frame::EgenFn::Min, "cond(la==1, wv, .)", {"h_pid"});
        double w[6] = {2, 2, 2, 3, 3, 3};
        for (int i = 0; i < 6; ++i) approx("egen onset", fr.col("onset").f64[i], w[i]);
    }

    // ---- §5.3 bygen subscripting ----
    {
        Frame fr = makeframe(native_temp_path("cecon_d2_by.hbin"), 5,
                             {{"id", {1, 1, 1, 2, 2}}, {"t", {1, 2, 3, 1, 2}},
                              {"x", {10, 20, 30, 100, 200}}});
        fr.bygen("first_x", "x[1]", {"id"}, {"t"});
        fr.bygen("last_x", "x[_N]", {"id"}, {"t"});
        fr.bygen("prev_x", "x[_n-1]", {"id"}, {"t"});
        fr.bygen("rn", "_n", {"id"}, {"t"});
        fr.bygen("rN", "_N", {"id"}, {"t"});
        fr.bygen("oob", "x[0]", {"id"}, {"t"});
        double fx[5] = {10, 10, 10, 100, 100}, lx[5] = {30, 30, 30, 200, 200};
        double px[5] = {NA, 10, 20, NA, 100}, rn[5] = {1, 2, 3, 1, 2}, rN[5] = {3, 3, 3, 2, 2};
        for (int i = 0; i < 5; ++i) {
            approx("bygen x[1]", fr.col("first_x").f64[i], fx[i]);
            approx("bygen x[_N]", fr.col("last_x").f64[i], lx[i]);
            approx("bygen x[_n-1]", fr.col("prev_x").f64[i], px[i]);
            approx("bygen _n", fr.col("rn").f64[i], rn[i]);
            approx("bygen _N", fr.col("rN").f64[i], rN[i]);
            approx("bygen oob x[0]", fr.col("oob").f64[i], NA);
        }
    }

    // ---- §5.4 summarize, detail percentiles ----
    {
        Frame fr = makeframe(native_temp_path("cecon_d2_su.hbin"), 10,
                             {{"x", {1, 2, 3, 4, 5, 6, 7, 8, 9, 10}}});
        Frame::Summary s = fr.summarize("x", true);
        approx("summ mean", s.mean, 5.5);
        approx("summ sd", s.sd, 3.0276503540974917);
        approx("summ p25", s.p25, 3);
        approx("summ p50", s.p50, 5.5);
        approx("summ p75", s.p75, 8);
        approx("summ p99", s.p99, 10);
    }

    // ---- BUG-371: the Stata date family -------------------------------------
    // Every expected value below was computed OUTSIDE this codebase (Python's
    // datetime against the 1960-01-01 epoch) and is pinned here as a literal.
    // The cases are chosen to be the ones a wrong implementation gets wrong:
    // the epoch itself, a NEGATIVE date, a leap day, a day-of-year in a leap
    // year, and a weekday that is independently known (the moon landing was a
    // Sunday, so dow == 0).
    {
        Frame fr = makeframe(native_temp_path("cecon_d2_dt.hbin"), 1, {{"d", {0}}});
        auto E = [&](const char* expr) { fr.generate("r", expr); return fr.col("r").f64[0]; };
        approx("mdy epoch",      E("mdy(1,1,1960)"), 0);
        approx("mdy unix",       E("mdy(1,1,1970)"), 3653);
        approx("mdy negative",   E("mdy(12,31,1959)"), -1);
        approx("mdy moon",       E("mdy(7,20,1969)"), 3488);
        approx("mdy leapday",    E("mdy(2,29,2020)"), 21974);
        approx("mdy y2k",        E("mdy(1,1,2000)"), 14610);
        approx("year",           E("year(21974)"), 2020);
        approx("month",          E("month(21974)"), 2);
        approx("day",            E("day(21974)"), 29);
        approx("dow friday0",    E("dow(0)"), 5);        // 01jan1960 was a Friday
        approx("dow moon sunday", E("dow(3488)"), 0);
        approx("doy leapyear",   E("doy(23741)"), 366);  // 31dec2024
        approx("doy epoch",      E("doy(0)"), 1);
        approx("quarter",        E("quarter(21974)"), 1);
        approx("halfyear",       E("halfyear(23741)"), 2);
        // Impossible dates are MISSING. Without the round-trip check inside
        // stata_mdy, days_from_civil would silently normalise Feb 30 to Mar 1 and
        // hand back a perfectly plausible wrong number.
        approx("mdy feb30 miss",  E("mdy(2,30,2020)"), NA);
        approx("mdy month13 miss", E("mdy(13,1,2020)"), NA);
        approx("mdy apr31 miss",  E("mdy(4,31,2020)"), NA);
        approx("mdy nonleap229",  E("mdy(2,29,2021)"), NA);
        approx("mdy missing arg", E("mdy(1,1,.)"), NA);
        approx("year of missing", E("year(.)"), NA);
    }
    // The SAME functions over a multi-row frame, which takes the VECTORIZED
    // evaluator instead of the per-row one. The two are separate switch
    // statements in frame.cpp; a case added to one and forgotten in the other
    // returns all-missing, and only a multi-row frame can see that.
    {
        Frame fr = makeframe(native_temp_path("cecon_d2_dtv.hbin"), 3,
                             {{"m", {1, 7, 2}}, {"dd", {1, 20, 29}}, {"yy", {1960, 1969, 2020}}});
        fr.generate("t", "mdy(m,dd,yy)");
        const auto& t = fr.col("t").f64;
        approx("vec mdy[0]", t[0], 0);
        approx("vec mdy[1]", t[1], 3488);
        approx("vec mdy[2]", t[2], 21974);
        fr.generate("yr", "year(t)");
        fr.generate("w", "dow(t)");
        const auto& yr = fr.col("yr").f64;
        const auto& w = fr.col("w").f64;
        approx("vec year[0]", yr[0], 1960);
        approx("vec year[2]", yr[2], 2020);
        approx("vec dow[0]", w[0], 5);
        approx("vec dow[1]", w[1], 0);
    }

    // ---- AER scope 2 / B3: the rest of the Stata time scales ----------------
    // Every literal below is a CAPTURED StataBE 19.5 value, not a derivation:
    // tests/validate/b3_funcs_oracle.{do,json}, key names in the comments. The
    // tolerance is 0, not the file's 1e-12 default, because these are exactly
    // representable doubles and 1e-12 RELATIVE slack on cofd's 1.9e12 would let
    // an off-by-one second through.
    {
        Frame fr = makeframe(native_temp_path("cecon_d2_b3.hbin"), 1, {{"d", {0}}});
        auto E = [&](const char* expr) { fr.generate("r", expr); return fr.col("r").f64[0]; };
        auto X = [&](const char* what, const char* expr, double want) { approx(what, E(expr), want, 0); };
        // builders — out-of-range periods are missing, not wrapped
        X("ym",         "ym(2020,3)", 722);
        X("ym epoch",   "ym(1960,1)", 0);
        X("ym month13", "ym(2020,13)", NA);
        X("ym month0",  "ym(2020,0)", NA);
        X("ym missing", "ym(.,3)", NA);
        X("yq",         "yq(2020,1)", 240);
        X("yq q5",      "yq(2020,5)", NA);
        X("yw",         "yw(2020,10)", 3129);
        X("yh",         "yh(2020,1)", 120);
        // %td -> coarser scale. The negative cases are the ones a truncating
        // division gets wrong: 31dec1959 is period -1 on every scale.
        X("mofd",       "mofd(21989)", 722);
        X("mofd frac",  "mofd(21989.7)", 722);
        X("mofd neg",   "mofd(-1)", -1);
        X("mofd miss",  "mofd(.)", NA);
        X("qofd",       "qofd(21989)", 240);
        X("qofd neg",   "qofd(-1)", -1);
        X("yofd",       "yofd(21989)", 2020);
        X("yofd epoch", "yofd(0)", 1960);
        X("yofd neg",   "yofd(-1)", 1959);
        X("wofd",       "wofd(21989)", 3130);
        X("wofd neg",   "wofd(-1)", -1);
        X("hofd",       "hofd(21989)", 120);
        X("hofd neg",   "hofd(-1)", -1);
        X("week",       "week(21989)", 11);
        // scale -> first day of the period
        X("dofm",       "dofm(722)", 21975);
        X("dofm neg",   "dofm(-1)", -31);
        X("dofq",       "dofq(240)", 21915);
        X("dofq neg",   "dofq(-1)", -92);
        X("dofy",       "dofy(2020)", 21915);
        X("dofy 1959",  "dofy(1959)", -365);
        X("dofw",       "dofw(3129)", 21978);
        X("dofw neg",   "dofw(-1)", -8);
        X("dofh",       "dofh(120)", 21915);
        X("dofh neg",   "dofh(-1)", -184);
        // %tc / %tC. The 27000 ms between cofd and Cofd IS the leap-second
        // table; get one row of it wrong and only this check notices.
        X("cofd",       "cofd(21989)", 1899849600000.0);
        X("Cofd",       "Cofd(21989)", 1899849627000.0);
        X("dofc",       "dofc(1899899156000)", 21989);
        X("dofC",       "dofC(1899899183000)", 21989);
        X("dofc partial", "dofc(500)", 0);
        X("hh",         "hh(1899899156000)", 13);
        X("mm",         "mm(1899899156000)", 45);
        X("ss",         "ss(1899899156000)", 56);
        X("hhC",        "hhC(1899899183000)", 13);
        X("mmC",        "mmC(1899899183000)", 45);
        X("ssC",        "ssC(1899899183000)", 56);
        X("hh miss",    "hh(.)", NA);
        // constructors validate rather than normalise
        X("hms",        "hms(13,45,56)", 49556000);
        X("hms hour25", "hms(25,0,0)", NA);
        X("hms sec90",  "hms(0,0,90)", NA);
        X("hms neg",    "hms(-1,0,0)", NA);
        X("dhms",       "dhms(21989,13,45,56)", 1899899156000.0);
        X("dhms frac",  "dhms(0,0,0,0.5)", 500);
        // unit conversions. hours() DIVIDES: v * (1.0/3600000.0) answers
        // 0.9999999999999999 here, which is how the header's own check caught it.
        X("msofseconds", "msofseconds(90)", 90000);
        X("msofminutes", "msofminutes(1.5)", 90000);
        X("msofhours",   "msofhours(1)", 3600000);
        X("seconds",     "seconds(90000)", 90);
        X("minutes",     "minutes(90000)", 1.5);
        X("hours",       "hours(3600000)", 1);
    }
    // The SAME names over a multi-row frame. frame.cpp has TWO switches over
    // Node::Fn and a name reaching only the scalar one returns an all-missing
    // column with rc 0 and no error, which a 1-row frame cannot see. Both are
    // driven from one CECON_TIME_UNARY_CASES label list, and this block is what
    // proves the list is actually shared.
    {
        Frame fr = makeframe(native_temp_path("cecon_d2_b3v.hbin"), 3,
                             {{"t", {21989, -1, 0}}, {"yy", {2020, 1960, 2020}}, {"pp", {3, 1, 13}}});
        fr.generate("m", "mofd(t)");
        fr.generate("q", "qofd(t)");
        fr.generate("y", "yofd(t)");
        fr.generate("w", "wofd(t)");
        fr.generate("c", "cofd(t)");
        fr.generate("k", "ym(yy,pp)");
        fr.generate("hx", "hh(cofd(t) + 49556000)");
        const auto& m = fr.col("m").f64; const auto& q = fr.col("q").f64;
        const auto& y = fr.col("y").f64; const auto& w = fr.col("w").f64;
        const auto& c = fr.col("c").f64; const auto& k = fr.col("k").f64;
        const auto& hx = fr.col("hx").f64;
        approx("vec mofd[0]", m[0], 722, 0);
        approx("vec mofd[1]", m[1], -1, 0);
        approx("vec qofd[1]", q[1], -1, 0);
        approx("vec yofd[0]", y[0], 2020, 0);
        approx("vec yofd[1]", y[1], 1959, 0);
        approx("vec wofd[0]", w[0], 3130, 0);
        approx("vec cofd[0]", c[0], 1899849600000.0, 0);
        approx("vec cofd[2]", c[2], 0, 0);
        approx("vec ym[0]", k[0], 722, 0);
        approx("vec ym[1]", k[1], 0, 0);
        approx("vec ym month13", k[2], NA, 0);   // out of range stays missing column-wide
        approx("vec hh[0]", hx[0], 13, 0);
    }

    // ---- w2y_fn_datetime: the calendar's edges and fractional arguments ------
    // StataNow 19.5 captures: tests/eval/golden/aer_w2/w2y_fn_datetime.log (arm
    // named) and the rule probe internal capture
    // w2y_fn_datetime_probe (p_* keys). Every row below answered differently
    // before: years outside 100..9999 built and extracted dates, mdy() refused a
    // fraction Stata truncates, ym()/yq() refused the fractions Stata takes
    // linearly and built years 100..999, and hh() read a timestamp past the
    // calendar. Through the engine (`generate`), the layer frame.cpp serves.
    // Rows labelled "ctl" are boundary CONTROLS: they held before too, and pin the
    // inside of each new edge so a later off-by-one cannot move it unseen.
    {
        Frame fr = makeframe(native_temp_path("cecon_d2_w2y.hbin"), 1, {{"d", {0}}});
        auto E = [&](const char* expr) { fr.generate("r", expr); return fr.col("r").f64[0]; };
        auto X = [&](const char* what, const char* expr, double want) { approx(what, E(expr), want, 0); };
        X("ctl mdy year100",      "mdy(1,1,100)", -679350);          // p_mdy_100
        X("mdy year99",       "mdy(1,1,99)", NA);                // p_mdy_99
        X("ctl mdy year10000",    "mdy(1,1,10000)", NA);             // p_mdy_10000
        X("mdy frac month",   "mdy(1.5,1,2000)", 14610);         // fd_mdy_edges2
        X("mdy frac day",     "mdy(1,1.9,2000)", 14610);         // fd_mdy_edges2
        X("mdy frac year",    "mdy(1,1,2000.5)", 14610);         // fd_mdy_edges2
        X("mdy frac leap",    "mdy(2,29.5,2000)", 14669);        // p_mdy_d299
        X("ctl mdy frac zero day", "mdy(1,0.5,2000)", NA);           // p_mdy_d0p5
        X("dofy year99",      "dofy(99)", NA);                   // p_dofy_99
        X("ctl dofy year100.5",   "dofy(100.5)", -679350);           // p_dofy_100.5
        X("ym linear month",  "ym(2000,2.5)", 481.5);            // fd_ym_edges
        X("ym linear year",   "ym(2000.5,2)", 487);              // fd_ym_edges
        X("ctl ym month 0.99",    "ym(1960,0.99)", NA);              // p_ym_1960_0.99
        X("ctl ym month 12.5",    "ym(1960,12.5)", NA);              // p_ym_1960_12.5
        X("ym year100",       "ym(100,1)", NA);                  // p_ym_100_1
        X("ym year101",       "ym(101,1)", NA);                  // p_ym_101_1
        X("ctl ym year9999",      "ym(9999,12)", 96479);             // p_ym_9999_12
        X("ctl ym year9999.5",    "ym(9999.5,1)", NA);               // p_ym_9999.5_1
        X("yq linear",        "yq(2020.5,3)", 244);              // fd_yq_real
        X("yq year100",       "yq(100,1)", NA);                  // p_yq_100_1
        X("year below",       "year(-679351)", NA);              // p_year_-679351
        X("year floor below", "year(-679350.5)", NA);            // p_year_-679350.5
        X("ctl year top",         "year(2936549.9)", 9999);          // p_year_2936549.9
        X("year above",       "year(2936550)", NA);              // p_year_2936550
        X("ctl dow above",        "dow(2936550)", 6);                // p_dow_2936550
        X("dow below",        "dow(-679351)", NA);               // p_dow_-679351
        X("mofd below",       "mofd(-679351)", NA);              // p_mofd_-679351
        X("qofd above",       "qofd(2936550)", NA);              // p_qofd_2936550
        X("cofd frac top",    "cofd(2936549.9)", NA);            // p_cofd_2936549.9
        X("dofc below",       "dofc(-58695840000001)", NA);      // p_dofc_-58695840000001
        X("hh above",         "hh(253717920000000)", NA);        // p_hh_253717920000000
        X("hh 1e16",          "hh(1e16)", NA);                   // p_hh_1e16
        X("ss millis",        "ss(253717919999999)", 59.999);    // p_ss_253717919999999
        X("ctl tm literal 0100",  "tm(0100m1)", -22320);             // p_lit_2: literals keep year 100
    }

    if (g_fail) { std::printf("FAILED (%d)\n", g_fail); return 1; }
    std::printf("  ok — exprs, egen, bygen, summarize-detail, date family + B3 time scales\n"
                "       (scalar + vectorized, pinned to the StataBE oracle)\n"
                "PASSED\n");
    return 0;
}
