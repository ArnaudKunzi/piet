// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// w2c_functions data arms — the >=2-paper expression functions in generate/replace.
//
// Oracle: tests/eval/golden/aer_w2/w2c_functions.log (StataBE 19.5). One check per
// `data`-context arm that the ENGINE evaluates (numeric results of numeric
// arguments). The string-valued data arms (itrim, regexs, reverse, strmatch,
// strofreal, strproper, strrpos, ustr*, usubinstr, wordcount) are evaluated by
// PIET's own row evaluator (piet/native/src/eval/exprfns.cpp via strmutate), not
// by this engine, and are not checked here.
//
// Fixture = the do-file's: set obs 20, x = _n - 10, xp = abs(x) + 1, xm = x with
// row 5 missing. Every record the oracle took from a default-typed `generate` is a
// FLOAT (%18.0g of the stored float), so per TOLERANCE.md §3.3 both sides are
// rounded to float and compared exactly. Where the same function and arguments also
// have a full-precision oracle record (the macro-context arm of the same log, e.g.
// invttail_macro_second), the double is additionally held to T2 (rtol 1e-9).
//
// Before this TU's change every function below except abs/max/min/round/sqrt/sum
// and the two runiform arms was an "unknown function" in the engine (fn-probe
// 2026-09-22: F038 group, F048 invnormal, F051 invt, F055 invttail, F094 normal,
// F116 rnormal, F134 sign, F178 ttail, F182 uniform), so each of those checks fails
// on the old engine by throwing.
//
// The MEASURED edge checks cite StataBE 19.5 runs made for this task on
// 2026-09-29 (artefacts under the task scratch dir: meas.log, grp*.log, edge*.log).
#include <algorithm>
#include <initializer_list>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "core/rng_mt64.h"
#include "data/frame.h"
#include "data/missing.h"

using namespace cecon;

static int g_fail = 0;

static void ok(bool cond, const std::string& what) {
    if (cond) {
        std::printf("  ok   %s\n", what.c_str());
    } else {
        std::printf("  FAIL %s\n", what.c_str());
        ++g_fail;
    }
}

static Frame fixture() {
    Frame fr;
    fr.set_n_rows_hint(20);
    std::vector<double> x(20), xp(20), xm(20);
    for (int i = 0; i < 20; ++i) {
        x[i] = (i + 1) - 10.0;
        xp[i] = std::fabs(x[i]) + 1.0;
        xm[i] = x[i];
    }
    xm[4] = stata_missing(kMissSys);
    fr.set_f64_column("x", x);
    fr.set_f64_column("xp", xp);
    fr.set_f64_column("xm", xm);
    return fr;
}

// Run `generate name = expr`; false (and a FAIL line) if the engine refuses.
static bool gen(Frame& fr, const std::string& name, const std::string& expr,
                const std::string& mask = "") {
    try {
        fr.generate(name, expr, true, mask);
        return true;
    } catch (const std::exception& e) {
        std::printf("  FAIL generate %s = %s threw: %s\n", name.c_str(), expr.c_str(), e.what());
        ++g_fail;
        return false;
    }
}

static double at(const Frame& fr, const std::string& name, int row1) {
    return fr.col(name).f64[static_cast<size_t>(row1 - 1)];
}

// TOLERANCE.md §3.3: a float-stored oracle value, compared after rounding both sides.
static void float_eq(const Frame& fr, const std::string& name, int row1, double oracle,
                     const std::string& arm) {
    const double v = at(fr, name, row1);
    const bool good = !std::isnan(v) && static_cast<float>(v) == static_cast<float>(oracle);
    char buf[256];
    std::snprintf(buf, sizeof buf, "%s: %s[%d] = %.17g, float %.9g vs oracle %.9g", arm.c_str(),
                  name.c_str(), row1, v, static_cast<double>(static_cast<float>(v)),
                  static_cast<double>(static_cast<float>(oracle)));
    ok(good, buf);
}

// T2 (rtol 1e-9) against a full-precision oracle record of the same quantity.
static void t2(const Frame& fr, const std::string& name, int row1, double oracle,
               const std::string& arm) {
    const double v = at(fr, name, row1);
    const bool good = !std::isnan(v) && std::fabs(v - oracle) <= 1e-9 * std::fabs(oracle);
    char buf[256];
    std::snprintf(buf, sizeof buf, "%s: %s[%d] = %.17g vs %.17g (T2)", arm.c_str(), name.c_str(),
                  row1, v, oracle);
    ok(good, buf);
}

static void exact(const Frame& fr, const std::string& name, int row1, double oracle,
                  const std::string& arm) {
    const double v = at(fr, name, row1);
    char buf[256];
    std::snprintf(buf, sizeof buf, "%s: %s[%d] = %.17g vs %.17g", arm.c_str(), name.c_str(), row1,
                  v, oracle);
    ok(std::isnan(oracle) ? (std::isnan(v) && missing_kind(v) == kMissSys) : v == oracle, buf);
}

static void column_is(const Frame& fr, const std::string& name, const std::vector<double>& want,
                      const std::string& what) {
    std::string got;
    bool good = true;
    for (size_t i = 0; i < want.size(); ++i) {
        const double v = fr.col(name).f64[i];
        const bool m = std::isnan(want[i]);
        if (m ? !std::isnan(v) : v != want[i]) good = false;
        char b[32];
        std::snprintf(b, sizeof b, " %g", v);
        got += b;
    }
    ok(good, what + " ->" + got);
}

static bool throws(Frame& fr, const std::string& name, const std::string& expr,
                   const std::string& needle) {
    try {
        fr.generate(name, expr, true);
    } catch (const std::exception& e) {
        return std::string(e.what()).find(needle) != std::string::npos;
    }
    return false;
}

// ---- the data arms --------------------------------------------------------------
static void data_arms() {
    std::printf("w2c_functions data arms (engine)\n");
    Frame fr = fixture();
    // abs_data: abs(x) -> rows 1, 10 = 9, 0
    if (gen(fr, "__d_abs", "abs(x)")) {
        exact(fr, "__d_abs", 1, 9, "abs_data");
        exact(fr, "__d_abs", 10, 0, "abs_data");
    }
    // group_data: group(2) -> row 1 = 1   (F038)
    if (gen(fr, "__d_group", "group(2)")) exact(fr, "__d_group", 1, 1, "group_data");
    // invnormal_data_tail / _ctl   (F048)
    if (gen(fr, "__d_invnorm1", "invnormal(.975)")) {
        float_eq(fr, "__d_invnorm1", 1, 1.959964036941528, "invnormal_data_tail");
        t2(fr, "__d_invnorm1", 1, 1.959963984540053605, "invnormal_data_tail (StataBE %24.0g)");
    }
    if (gen(fr, "__d_invnorm2", "invnormal(.025)")) {
        float_eq(fr, "__d_invnorm2", 1, -1.959964036941528, "invnormal_data_ctl");
        t2(fr, "__d_invnorm2", 1, -0x1.f5c0331eeff83p+0, "invnormal_data_ctl (StataBE %21x)");
    }
    // invt_data: invt(10,.975)   (F051); full precision = invt_macro_second
    if (gen(fr, "__d_invt", "invt(10,.975)")) {
        float_eq(fr, "__d_invt", 1, 2.22813892364502, "invt_data");
        t2(fr, "__d_invt", 1, 2.228138851986273, "invt_data vs invt_macro_second");
    }
    // invttail_data: invttail(10,.025)   (F055); full precision = invttail_macro_second
    if (gen(fr, "__d_invttail", "invttail(10,.025)")) {
        float_eq(fr, "__d_invttail", 1, 2.22813892364502, "invttail_data");
        t2(fr, "__d_invttail", 1, 2.228138851986273, "invttail_data vs invttail_macro_second");
    }
    // max_data / max_missing_arg, min_data / min_missing_arg
    if (gen(fr, "__d_max", "max(x,2)")) {
        exact(fr, "__d_max", 1, 2, "max_data");
        exact(fr, "__d_max", 20, 10, "max_data");
    }
    if (gen(fr, "__d_max_miss", "max(xm,2)")) exact(fr, "__d_max_miss", 5, 2, "max_missing_arg");
    if (gen(fr, "__d_min", "min(x,2)")) {
        exact(fr, "__d_min", 1, -9, "min_data");
        exact(fr, "__d_min", 20, 2, "min_data");
    }
    if (gen(fr, "__d_min_miss", "min(xm,2)")) exact(fr, "__d_min_miss", 5, 2, "min_missing_arg");
    // normal_data_hi / _lo   (F094); full precision of normal(1.96) = normal_macro
    if (gen(fr, "__d_normal1", "normal(1.96)")) {
        float_eq(fr, "__d_normal1", 1, .975002110004425, "normal_data_hi");
        t2(fr, "__d_normal1", 1, .9750021048517796, "normal_data_hi vs normal_macro");
    }
    if (gen(fr, "__d_normal2", "normal(-1.96)")) {
        float_eq(fr, "__d_normal2", 1, .0249978955835104, "normal_data_lo");
        t2(fr, "__d_normal2", 1, 0x1.9990c58859310p-6, "normal_data_lo (StataBE %21x)");
    }
    // round_unit_001 / _1 / _10
    if (gen(fr, "__d_round001", "round(x,.01)")) exact(fr, "__d_round001", 1, -9, "round_unit_001");
    if (gen(fr, "__d_round1", "round(x,1)")) exact(fr, "__d_round1", 1, -9, "round_unit_1");
    if (gen(fr, "__d_round10", "round(x,10)")) exact(fr, "__d_round10", 1, -10, "round_unit_10");
    // sign_data: sign(x-3) -> rows 1, 13 = -1, 0   (F134)
    if (gen(fr, "__d_sign", "sign(x-3)")) {
        exact(fr, "__d_sign", 1, -1, "sign_data");
        exact(fr, "__d_sign", 13, 0, "sign_data");
    }
    // sqrt_data: sqrt(xp) row 1 (float)
    if (gen(fr, "__d_sqrt", "sqrt(xp)")) float_eq(fr, "__d_sqrt", 1, 3.162277698516846, "sqrt_data");
    // sum_data_running: sum(x) rows 1, 20 = -9, 10
    if (gen(fr, "__d_sum", "sum(x)")) {
        exact(fr, "__d_sum", 1, -9, "sum_data_running");
        exact(fr, "__d_sum", 20, 10, "sum_data_running");
    }
    // ttail_data: ttail(10,2)   (F178); full precision = ttail_macro_second
    if (gen(fr, "__d_ttail", "ttail(10,2)")) {
        float_eq(fr, "__d_ttail", 1, .0366940163075924, "ttail_data");
        t2(fr, "__d_ttail", 1, .0366940173853702, "ttail_data vs ttail_macro_second");
    }
}

// ---- RNG arms (T5, distributional: never Stata's draws, see core/rng_uniform.h) ----
struct Moments { double mn, mx, mean; bool finite; };
static Moments moments(const Frame& fr, const std::string& name) {
    Moments m{INFINITY, -INFINITY, 0.0, true};
    const int64_t n = fr.n_rows();
    for (int64_t i = 0; i < n; ++i) {
        const double v = fr.col(name).f64[static_cast<size_t>(i)];
        if (!std::isfinite(v)) m.finite = false;
        m.mn = std::min(m.mn, v);
        m.mx = std::max(m.mx, v);
        m.mean += v;
    }
    m.mean /= static_cast<double>(n);
    return m;
}
// One-sample Kolmogorov-Smirnov D against a continuous CDF.
template <class Cdf>
static double ks(const Frame& fr, const std::string& name, Cdf cdf) {
    const int64_t n = fr.n_rows();
    std::vector<double> v(static_cast<size_t>(n));
    for (int64_t i = 0; i < n; ++i) v[static_cast<size_t>(i)] = fr.col(name).f64[static_cast<size_t>(i)];
    std::sort(v.begin(), v.end());
    double d = 0.0;
    for (int64_t i = 0; i < n; ++i) {
        const double f = cdf(v[static_cast<size_t>(i)]);
        d = std::max(d, std::max(f - static_cast<double>(i) / n, static_cast<double>(i + 1) / n - f));
    }
    return d;
}

static void rng_arms() {
    std::printf("w2c_functions RNG arms (distributional)\n");
    Frame fr;
    fr.set_n_rows_hint(200);
    fr.set_f64_column("one", std::vector<double>(200, 1.0));
    // Without draw coordinates rnormal()/uniform() refuse: PIET injects them only for
    // the spelling `runiform`, and a default (seed, stream 0) would repeat the same
    // column on every call. Checked first, before this process sets coordinates.
    ok(throws(fr, "__r_nocoord", "rnormal()", "needs draw coordinates"),
       "rnormal() without --rng-seed/--rng-stream is an honest refusal");
    ok(throws(fr, "__r_nocoord2", "uniform()", "needs draw coordinates"),
       "uniform() without --rng-seed/--rng-stream is an honest refusal");
    // Coordinates as PIET sends them: the session seed (the do-file's `set seed 1`)
    // and a stream number that advances by one per drawing call.
    long long stream = 0;
    auto coords = [&stream]() { set_rng_coords(1, stream++); };
    const double crit = 1.63 / std::sqrt(200.0);  // KS, alpha = 0.01
    const double band = 4.0 * std::sqrt(2.0 / 200.0);  // 4 sd of a two-sample mean difference, unit sd
    // runiform_dist: float | .0006079551531002 | .9989203214645386 | .4739880738774082
    coords();
    if (gen(fr, "__r_unif", "runiform()")) {
        const Moments m = moments(fr, "__r_unif");
        ok(m.finite && m.mn >= 0.0 && m.mx < 1.0 && m.mn < 0.05 && m.mx > 0.95,
           "runiform_dist: support [0,1), both ends reached");
        ok(std::fabs(m.mean - .4739880738774082) <= band * std::sqrt(1.0 / 12.0),
           "runiform_dist: mean within 4 sd of the oracle mean");
        ok(ks(fr, "__r_unif", [](double u) { return u; }) < crit, "runiform_dist: KS vs U(0,1)");
    }
    // The coordinates belong to the call that carried them. Under `cobra serve` one
    // process answers every request; a later rnormal() whose request carried none
    // must still refuse, not reuse the runiform() call's (seed, stream).
    ok(throws(fr, "__r_stale", "rnormal()", "needs draw coordinates"),
       "rnormal() after a drawing call, with no coordinates of its own, is refused");
    // runiformint_dist: float | 0 | 5 | 2.815
    coords();
    if (gen(fr, "__r_unifint", "runiformint(0,5)")) {
        const Moments m = moments(fr, "__r_unifint");
        bool ints = true;
        for (int64_t i = 0; i < 200; ++i) {
            const double v = fr.col("__r_unifint").f64[static_cast<size_t>(i)];
            if (v != std::floor(v)) ints = false;
        }
        ok(m.finite && ints && m.mn == 0.0 && m.mx == 5.0, "runiformint_dist: integers, min 0, max 5");
        ok(std::fabs(m.mean - 2.815) <= band * std::sqrt(35.0 / 12.0),
           "runiformint_dist: mean within 4 sd of the oracle mean");
    }
    // rnormal_dist: float | -2.487671852111816 | 1.974770307540894 | -.1362921536155045   (F116)
    const long long norm_stream = stream;
    coords();
    if (gen(fr, "__r_norm", "rnormal()")) {
        const Moments m = moments(fr, "__r_norm");
        ok(m.finite && m.mn < -1.5 && m.mx > 1.5, "rnormal_dist: finite, both tails reached");
        ok(std::fabs(m.mean - -.1362921536155045) <= band, "rnormal_dist: mean within 4 sd of the oracle mean");
        ok(ks(fr, "__r_norm", [](double z) { return 0.5 * std::erfc(-z / std::sqrt(2.0)); }) < crit,
           "rnormal_dist: KS vs N(0,1)");
    }
    // A draw is a function of (seed, stream, node, row) alone: the same coordinates
    // give the same column however many draws this process made before, and the
    // next stream gives a different one.
    auto same_col = [&fr](const char* a, const char* b) {
        for (int64_t i = 0; i < 200; ++i)
            if (fr.col(a).f64[static_cast<size_t>(i)] != fr.col(b).f64[static_cast<size_t>(i)]) return false;
        return true;
    };
    set_rng_coords(1, norm_stream);
    if (gen(fr, "__r_norm_again", "rnormal()"))
        ok(same_col("__r_norm", "__r_norm_again"),
           "rnormal() at the same (seed, stream) reproduces its column after other draws");
    coords();
    if (gen(fr, "__r_norm_next", "rnormal()"))
        ok(!same_col("__r_norm", "__r_norm_next"), "rnormal() at a different --rng-stream differs");
    // uniform_dist: float | .0049175131134689 | .996803343296051 | .5206717907288112   (F182)
    coords();
    if (gen(fr, "__r_uniform", "uniform()")) {
        const Moments m = moments(fr, "__r_uniform");
        ok(m.finite && m.mn >= 0.0 && m.mx < 1.0 && m.mn < 0.05 && m.mx > 0.95,
           "uniform_dist: support [0,1), both ends reached");
        ok(std::fabs(m.mean - .5206717907288112) <= band * std::sqrt(1.0 / 12.0),
           "uniform_dist: mean within 4 sd of the oracle mean");
        ok(ks(fr, "__r_uniform", [](double u) { return u; }) < crit, "uniform_dist: KS vs U(0,1)");
        ok(!same_col("__r_uniform", "__r_unif"), "uniform() and runiform() on different streams differ");
    }
    // Two draw sites in one expression are two nodes, not one value used twice.
    coords();
    if (gen(fr, "__r_two", "uniform() - uniform()")) {
        bool nonzero = false;
        for (int64_t i = 0; i < 200; ++i)
            if (fr.col("__r_two").f64[static_cast<size_t>(i)] != 0.0) nonzero = true;
        ok(nonzero, "two uniform() sites in one expression draw at different nodes");
    }
    // rnormal(m,s): measured rnormal(0,-1) = ., rnormal(.,1) = ., rnormal(1,0) = 1.
    coords();
    if (gen(fr, "__r_neg", "rnormal(0,-1)")) exact(fr, "__r_neg", 1, NAN, "rnormal(0,-1) is .");
    coords();
    if (gen(fr, "__r_mis", "rnormal(.,1)")) exact(fr, "__r_mis", 1, NAN, "rnormal(.,1) is .");
    coords();
    if (gen(fr, "__r_zero", "rnormal(1,0)")) exact(fr, "__r_zero", 1, 1, "rnormal(1,0) is 1");
    ok(throws(fr, "__r_bad", "uniform(1,2)", "wrong number of arguments"),
       "uniform(1,2) is refused (Stata r(198))");
}

// ---- measured edges (StataBE 19.5, 2026-09-29) -------------------------------------
static void measured_edges() {
    std::printf("measured edges\n");
    Frame fr;
    fr.set_n_rows_hint(1);
    fr.set_f64_column("one", {1.0});
    struct Case { const char* expr; double want; };
    const double M = NAN;
    const Case cases[] = {
        {"invttail(10,0)", M}, {"invttail(10,1)", M}, {"invttail(10,.5)", 0.0},
        {"invnormal(0)", M}, {"invnormal(1)", M}, {"invnormal(.5)", 0.0},
        {"ttail(0,1)", M}, {"ttail(.,1)", M}, {"ttail(10,.)", M}, {"ttail(-1,2)", M},
        {"ttail(3e17,1)", M}, {"ttail(4,0)", 0.5}, {"normal(0)", 0.5}, {"normal(-38)", 0.0},
        {"normal(40)", 1.0}, {"ttail(1,1.4e154)", 0.0}, {"ttail(1,-1e300)", 1.0},
        {"sign(.a)", M}, {"sign(-0)", 0.0}, {"group(0)", M}, {"group(.)", M}, {"group(-1)", M},
        // invttail's `.` cut (df = 1): root 9.40e153 (p 3.3863e-155) and ~9.37e153
        // (p 3.3971e-155) are `.`; the value side of the bracket is pinned below.
        {"invttail(1,3.2e-155)", M}, {"invttail(1,3.3863e-155)", M}, {"invttail(1,3.3971e-155)", M},
    };
    int k = 0;
    for (const Case& c : cases) {
        const std::string nm = "__e" + std::to_string(k++);
        if (gen(fr, nm, c.expr)) exact(fr, nm, 1, c.want, c.expr);
    }
    // Full-precision grid points (%24.0g / %21x from the same runs).
    struct Pt { const char* expr; double want; };
    const Pt pts[] = {
        {"ttail(1,-1)", .750000000000000222},     {"ttail(2.5,1.3)", .1502433946353539662},
        {"invttail(2.5,.1)", 1.730250928807175015}, {"invttail(1e6,.025)", 1.959966356814105248},
        {"ttail(1e6,1.96)", .02499803379263489111}, {"invttail(1,.6)", -.3249196962329061744},
        {"invttail(2,.05)", 2.919985580353723797}, {"ttail(10,-2)", .9633059826146297411},
        {"invnormal(1e-300)", -37.04709629936120052}, {"invttail(3,1e-12)", 10331.10824429249806},
        {"ttail(3,1e5)", 1.10265779044662529e-15}, {"invttail(.5,.3)", 1.00952587860716525},
        {"invttail(1e10,.025)", 1.9599639847772794}, {"ttail(1e10,1.96)", .02499789516208482734},
        {"invttail(30,.999999)", -5.871117120408625212}, {"invt(3,.01)", -4.540702858568131717},
        {"invttail(10,1e-300)", 2.56452571894819124e+30}, {"ttail(100,-7.5)", .999999999986713739},
        {"ttail(1e17,1)", .1586552539314571297}, {"normal(-8)", 6.22096057427178486e-16},
        {"invttail(1,3.4226e-155)", 9.3002362585107928e+153}, {"invttail(3,1e-231)", 1.03311083604466729e+77},
        {"invttail(1,3.4044e-155)", 9.3499555335386770e+153},  // last measured value below the cut
    };
    for (const Pt& p : pts) {
        const std::string nm = "__p" + std::to_string(k++);
        if (gen(fr, nm, p.expr)) {
            const double v = at(fr, nm, 1);
            char buf[160];
            std::snprintf(buf, sizeof buf, "%s = %.17g vs %.17g (1e-12)", p.expr, v, p.want);
            ok(std::fabs(v - p.want) <= 1e-12 * std::fabs(p.want), buf);
        }
    }

    // normal() in the far lower tail keeps its relative precision. References are
    // 400-digit values (Python decimal, erf series) rounded to double; StataBE is
    // within 4 ulp of them on a 1,221-point grid, while the plain
    // 0.5*erfc(-z*0.7071...) form was up to 1,570 ulp off below z = -30.
    struct Ref { const char* expr; double want; };
    const Ref refs[] = {
        {"normal(-37.5)", 0x1.08eda98086fd1p-1021}, {"normal(-35)", 0x1.db6d661d5b3eep-891},
        {"normal(-30)", 0x1.7795ad05ea397p-656},    {"normal(-25)", 0x1.2334bd061877ep-457},
        {"normal(-20)", 0x1.c0bd0f1880783p-295},    {"normal(-10)", 0x1.26c75e84fb10dp-77},
        {"normal(-5)", 0x1.33ca2f2133831p-22},      {"normal(-1.96)", 0x1.9990c58859312p-6},
        {"normal(1.96)", 0x1.f33379d3bd367p-1},
    };
    for (const Ref& r : refs) {
        const std::string nm = "__n" + std::to_string(k++);
        if (gen(fr, nm, r.expr)) {
            const double v = at(fr, nm, 1);
            const double ulp = std::nextafter(r.want, INFINITY) - r.want;
            char buf[160];
            std::snprintf(buf, sizeof buf, "%s = %a vs reference %a (<= 8 ulp)", r.expr, v, r.want);
            ok(std::fabs(v - r.want) <= 8.0 * ulp, buf);
        }
    }

    // Student t against independent references: a 60-70 digit incomplete-beta
    // continued fraction (Python decimal; ttail(1e7,30) confirmed a second way by
    // Fisher's expansion to g8 in decimal) and Newton on it for the quantiles. These
    // are the cells where the previous forms were measurably off: ttail at large df
    // on the ill-conditioned continued fraction (2.4e-11 at df = 1e7) or through
    // exp(log(tail)) (3.3e-12 at t = 30), and invttail near the median iterating on
    // 1/2 - eps (2.8e-13 at p = .4999). Checked at 1e-14 relative.
    const Ref trefs[] = {
        {"ttail(1e6,10)", 0x1.2788041c66369p-77},  {"ttail(1e7,10)", 0x1.26da9cea992fbp-77},
        {"ttail(1e5,10)", 0x1.2e62f13a8b172p-77},  {"ttail(1e7,30)", 0x1.7f48d905d5449p-656},
        {"invttail(1e9,.4999)", 0x1.06d6ca866e34bp-12}, {"invttail(1e7,.4999)", 0x1.06d6caf3921ccp-12},
        {"invttail(1,.4999)", 0x1.496b7d0998272p-12},
    };
    for (const Ref& r : trefs) {
        const std::string nm = "__t" + std::to_string(k++);
        if (gen(fr, nm, r.expr)) {
            const double v = at(fr, nm, 1);
            char buf[160];
            std::snprintf(buf, sizeof buf, "%s = %a vs reference %a (1e-14)", r.expr, v, r.want);
            ok(std::fabs(v - r.want) <= 1e-14 * std::fabs(r.want), buf);
        }
    }
    // A subnormal normal density must not let the large-df expansion accept a
    // diverging series: StataBE invttail(1000,1e-300) = 54.29 (grid, %21x).
    {
        const std::string nm = "__t" + std::to_string(k++);
        if (gen(fr, nm, "invttail(1000,1e-300)")) {
            const double v = at(fr, nm, 1), want = 0x1.b254c3858e49ap+5;
            ok(std::fabs(v - want) <= 1e-12 * want, "invttail(1000,1e-300) = 54.29 (StataBE), not the normal quantile");
        }
    }
    // invt(df,p) is exactly -invttail(df,p), with no 1-p rounding, at small p.
    for (const char* dfs : {"1", "2", "10", "1e3", "1e7"})
        for (const char* ps : {"1e-6", "1e-10"}) {
            const std::string a = "__s" + std::to_string(k++), b = "__s" + std::to_string(k++);
            const std::string ea = std::string("invt(") + dfs + "," + ps + ")";
            const std::string eb = std::string("invttail(") + dfs + "," + ps + ")";
            if (gen(fr, a, ea) && gen(fr, b, eb))
                ok(at(fr, a, 1) == -at(fr, b, 1), ea + " == -" + eb);
        }

    // group(): the rule is read off 78 (N, k) cells; four shapes here.
    auto grp = [](int N, const std::string& expr, const std::vector<double>& want,
                  const std::string& mask = "") {
        Frame g;
        g.set_n_rows_hint(N);
        g.set_f64_column("one", std::vector<double>(static_cast<size_t>(N), 1.0));
        try {
            g.generate("g", expr, true, mask);
        } catch (const std::exception& e) {
            std::printf("  FAIL %s threw: %s\n", expr.c_str(), e.what());
            ++g_fail;
            return;
        }
        column_is(g, "g", want, "N=" + std::to_string(N) + " " + expr + (mask.empty() ? "" : " if " + mask));
    };
    grp(5, "group(3)", {1, 1, 2, 3, 3});
    grp(7, "group(3)", {1, 1, 1, 2, 2, 3, 3});
    grp(2, "group(3)", {1, 3});
    grp(13, "group(2.5)", {1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3});
    grp(13, "group(5)", {1, 1, 1, 2, 2, 3, 3, 3, 4, 4, 5, 5, 5});
    grp(7, "group(1e-9)", {1, 0, 0, 0, 0, 0, 0});
    grp(7, "group(.5)", {1, 1, 1, 1, 1, 1, 1});
    grp(7, "group(_n)", {1, 1, 1, 2, 4, 5, 7});
    grp(7, "group(2) + group(3)", {2, 2, 2, 3, 4, 5, 5});
    grp(2, "group(1+9.9e-7)", {1, 1});
    grp(2, "group(1+1e-6)", {1, 2});
    // BUG-577: StataBE grp4/grp5 (z = group(1e-6), N = 2, row 2 = 1). The unfolded
    // `+ 1.0 - 1e-6` rounded to 0.9999999999999999 and floored to 0.
    grp(2, "group(1e-6)", {1, 1});
    grp(6, "group(2)", {M, M, 1, 2, 2, 2}, "_n > 2");
    // BUG-842: an `if` that names a range does not renumber either; only `in` does
    // (w2m_bymask G02 :482, `group(2) if _n>=2 & _n<=4` on N = 6 gives . 1 1 2 . .,
    // the dataset's numbering on rows 2-4). This shape used to be refused as a
    // presumed folded `in` range.
    grp(6, "group(2)", {M, 1, 1, 2, M, M}, "_n>=2 & _n<=4");
    grp(7, "cond((_n>=3 & _n<=7),(group(3)),.)", {M, M, 1, 2, 2, 3, 3});
    // The `in` range rides its own channel (GenQualifier, eval --gen-in), and
    // group() numbers within it: w2m_bymask G01 :472 (`in 2/4` -> . 1 1 2 . .) and
    // G05 :514 (`in 3/6` -> . . 1 1 2 2) on N = 6, under generate and under an
    // empty by-list bygen (PIET's route for `in`).
    auto grpin = [](int N, const std::string& expr, int64_t first, int64_t last,
                    const std::vector<double>& want, const std::string& label) {
        GenQualifier q;
        q.in_first = first;
        q.in_last = last;
        for (int path = 0; path < 2; ++path) {
            Frame g;
            g.set_n_rows_hint(N);
            g.set_f64_column("one", std::vector<double>(static_cast<size_t>(N), 1.0));
            try {
                if (path == 0) g.generate("g", expr, true, "", StoreType::Double, q);
                else g.bygen("g", expr, {}, {}, "", StoreType::Double, q);
                column_is(g, "g", want, label + (path == 0 ? " (generate)" : " (bygen)"));
            } catch (const std::exception& e) {
                std::printf("  FAIL %s threw: %s\n", label.c_str(), e.what());
                ++g_fail;
            }
        }
    };
    grpin(6, "group(2)", 2, 4, {M, 1, 1, 2, M, M}, "w2m_bymask G01 (:472): group(2) in 2/4");
    grpin(6, "group(2)", 3, 6, {M, M, 1, 1, 2, 2}, "w2m_bymask G05 (:514): group(2) in 3/6");
    grpin(6, "sum(group(2))", 2, 4, {M, 1, 2, 4, M, M},
          "w2m_bymask G04 (:504): sum(group(2)) in 2/4");
    grpin(6, "group(2)", 1, 6, {1, 1, 1, 2, 2, 2}, "w2m_bymask G00 (:462): group(2) in 1/6 = no range");
    {
        // bysort grp: gen d = group(2), grp = (_n > 4) on N = 7 -> 1 1 2 2 | 1 1 2
        Frame g;
        g.set_n_rows_hint(7);
        g.set_f64_column("grp", {0, 0, 0, 0, 1, 1, 1});
        try {
            g.bygen("d", "group(2)", {"grp"}, {});
            column_is(g, "d", {1, 1, 2, 2, 1, 1, 2}, "by grp: group(2)");
        } catch (const std::exception& e) {
            std::printf("  FAIL bygen group(2) threw: %s\n", e.what());
            ++g_fail;
        }
    }

    // BUG-576: under `by`, sum()'s ARGUMENT counts _n/_N and group() within the
    // by-group too. The per-group values rest on the measured by-group cell above
    // (StataBE `by grp, sort: generate d = group(2)` -> 1 1 2 2 | 1 1 2) and on _n
    // being the within-group observation number under `by`; sum() then runs over
    // them. The old engine evaluated the argument over the dataset (1 2 3 | 2 4 6
    // and 1 3 6 4 9 15).
    auto bysum = [](const std::vector<double>& gg, const std::string& expr,
                    const std::vector<double>& want, const std::string& label) {
        Frame g;
        g.set_n_rows_hint(static_cast<int64_t>(gg.size()));
        g.set_f64_column("gg", gg);
        try {
            g.bygen("t", expr, {"gg"}, {});
            column_is(g, "t", want, label);
        } catch (const std::exception& e) {
            std::printf("  FAIL %s threw: %s\n", label.c_str(), e.what());
            ++g_fail;
        }
    };
    bysum({0, 0, 0, 1, 1, 1}, "sum(group(2))", {1, 2, 4, 1, 2, 4}, "by gg: sum(group(2))");
    bysum({0, 0, 0, 1, 1, 1}, "sum(_n)", {1, 3, 6, 1, 3, 6}, "by gg: sum(_n)");
    bysum({0, 0, 0, 1, 1, 1}, "sum(_N)", {3, 6, 9, 3, 6, 9}, "by gg: sum(_N)");
    // Interleaved groups: the group position follows the sort order, not the row.
    bysum({1, 0, 1, 0, 1, 0}, "sum(_n)", {1, 1, 3, 3, 6, 6}, "by gg (interleaved): sum(_n)");
    // No `by`: the dataset numbering is the right one, and an `if` mask does not
    // renumber (StataBE GRPIF: group(2) if _n > 2 on N = 6 -> . 1 2 2 2 on rows 3-6).
    grp(6, "sum(group(2))", {M, M, 1, 3, 5, 7}, "_n > 2");
    {
        // BUG-842: a mask that names a range is an `if` and does not renumber (G02
        // :482), under generate and under bygen; it used to be refused as a presumed
        // `in` range. N = 7: the dataset's group(3) is 1 1 1 2 2 3 3, so rows 3-7
        // accumulate 1 2 2 3 3. The `in` range itself is the grpin arms above (G04).
        Frame g;
        g.set_n_rows_hint(7);
        g.set_f64_column("gg", std::vector<double>(7, 1.0));
        if (gen(g, "s", "sum(group(3))", "_n>=3 & _n<=7"))
            column_is(g, "s", {M, M, 1, 3, 5, 8, 11}, "generate sum(group(3)) if _n>=3 & _n<=7");
        try {
            g.bygen("s2", "sum(group(3))", {}, {}, "_n>=3 & _n<=7");
            column_is(g, "s2", {M, M, 1, 3, 5, 8, 11}, "bygen sum(group(3)) if _n>=3 & _n<=7");
        } catch (const std::exception& e) {
            std::printf("  FAIL bygen sum(group(3)) if-range threw: %s\n", e.what());
            ++g_fail;
        }
        // Stata's `in` is a range of the dataset's observations: with a by-list or a
        // sort key it would be renumbered, so the engine refuses that combination.
        GenQualifier q;
        q.in_first = 2;
        q.in_last = 4;
        bool refused = false;
        try {
            g.bygen("s3", "group(2)", {"gg"}, {}, "", StoreType::Double, q);
        } catch (const std::exception& e) {
            refused = std::string(e.what()).find("--gen-in") != std::string::npos;
        }
        ok(refused, "bygen with a by-list and an `in` range is refused by name");
        q.in_last = 8;
        refused = false;
        try {
            g.generate("s4", "group(2)", true, "", StoreType::Double, q);
        } catch (const std::exception& e) {
            refused = std::string(e.what()).find("is not within 1/7") != std::string::npos;
        }
        ok(refused, "an `in` range past _N is refused by name");
    }

    // BUG-836: the `if` of a by-prefixed running sum (PIET sends it as --gen-mask)
    // is evaluated inside each by-group, so its _n, _N, x[1], x[_N], x[_n-1] and
    // x[_n+1] count within the group. Oracle tests/eval/golden/aer_w2/w2m_bymask.log,
    // fixture F6 (gg = mod(_n,2), x = _n*10, sorted by gg x). The old engine
    // evaluated the mask over the dataset before the groups existed.
    auto bymask = [](const std::vector<double>& gg, const std::vector<double>& x,
                     const std::vector<std::string>& by, const std::vector<std::string>& sort,
                     const std::string& expr, const std::string& mask,
                     const std::vector<double>& want, const std::string& label) {
        Frame g;
        g.set_n_rows_hint(static_cast<int64_t>(gg.size()));
        g.set_f64_column(by.empty() ? "gg" : by[0], gg);
        g.set_f64_column("x", x);
        try {
            g.bygen("t", expr, by, sort, mask);
            column_is(g, "t", want, label);
        } catch (const std::exception& e) {
            std::printf("  FAIL %s threw: %s\n", label.c_str(), e.what());
            ++g_fail;
        }
    };
    const std::vector<double> f6g{0, 0, 0, 1, 1, 1}, f6x{20, 40, 60, 10, 30, 50};
    const std::vector<std::string> byg{"gg"};
    bymask(f6g, f6x, byg, {}, "sum(_n)", "_n > 1", {M, 2, 5, M, 2, 5},
           "w2m_bymask A01 (:69): by gg: sum(_n) if _n > 1");
    bymask(f6g, f6x, byg, {}, "sum(x)", "_n > 1", {M, 40, 100, M, 30, 80},
           "w2m_bymask A02 (:79): by gg: sum(x) if _n > 1");
    bymask(f6g, f6x, byg, {}, "sum(x)", "_n == _N", {M, M, 60, M, M, 50},
           "w2m_bymask A03 (:89): by gg: sum(x) if _n == _N");
    bymask(f6g, f6x, byg, {}, "sum(x)", "_n < _N", {20, 60, M, 10, 40, M},
           "w2m_bymask A04 (:99): by gg: sum(x) if _n < _N");
    bymask(f6g, f6x, byg, {}, "sum(x)", "x[_N] > 50", {20, 60, 120, M, M, M},
           "w2m_bymask A07 (:129): by gg: sum(x) if x[_N] > 50");
    bymask(f6g, f6x, byg, {}, "sum(x)", "_n == 1", {20, M, M, 10, M, M},
           "w2m_bymask A10 (:158): by gg: sum(x) if _n == 1");
    bymask(f6g, f6x, byg, {}, "sum(x)", "x >= x[1]", {20, 60, 120, 10, 40, 90},
           "w2m_bymask A11 (:167): by gg: sum(x) if x >= x[1]");
    bymask(f6g, f6x, byg, {}, "sum(x)", "x[_n-1] < 100", {M, 40, 100, M, 30, 80},
           "w2m_bymask F02 (:433): by gg: sum(x) if x[_n-1] < 100");
    // BUG-837: a missing `if` value selects the row (Stata keeps rows where the
    // condition is nonzero, and missing is nonzero). F03: each group's first row has
    // x[_n-1] missing and is IN, in the accumulation and in the output.
    bymask(f6g, f6x, byg, {}, "sum(x)", "x[_n-1]", {20, 60, 120, 10, 40, 90},
           "w2m_bymask F03 (:442): by gg: sum(x) if x[_n-1] (missing selects)");
    {
        // F12, stored unsorted: bysort g (x). The values are the oracle's sorted
        // records (D10 :626 10 28 53 . . | 3 10 . . | 12 24 .; D02 :598 . . . . 0 |
        // . . . 0 | . . 40; D01 :584 . 18 43 73 73 | . 7 16 16 | . 12 52) mapped back
        // to the stored rows, since bygen does not move rows (the tied x = 12 rows
        // keep their stored order). x[_n+1] is the next row in SORT order within the
        // group, and a missing x sorts last.
        const double X = stata_missing(kMissSys);
        const std::vector<double> f12g{2, 1, 3, 1, 2, 3, 1, 2, 1, 3, 1, 2},
            f12x{7, 30, 12, 10, X, 12, 25, 3, X, 40, 18, 9};
        const std::vector<std::string> by12{"g"}, sort12{"x"};
        bymask(f12g, f12x, by12, sort12, "sum(x)", "x[_n+1] < .",
               {10, M, 12, 10, M, 24, 53, 3, M, M, 28, M},
               "w2m_bymask D10 (:626): bysort g (x): sum(x) if x[_n+1] < .");
        bymask(f12g, f12x, by12, sort12, "sum(x)", "_n == _N",
               {M, M, M, M, 0, M, M, M, 0, 40, M, M},
               "w2m_bymask D02 (:598): bysort g (x): sum(x) if _n == _N");
        bymask(f12g, f12x, by12, sort12, "sum(x)", "_n > 1",
               {7, 73, M, M, 16, 12, 43, M, 73, 52, 18, 16},
               "w2m_bymask D01 (:584): bysort g (x): sum(x) if _n > 1");
    }
    {
        // An empty by-list is one group over the dataset, so the mask PIET folds an
        // `in 3/5` range into (`_n>=3 & _n<=5`, egen.cpp runBysortGen; the only path
        // that sends a range, and it never carries a by-list) counts the dataset's
        // rows: bygen and generate agree, and only rows 3-5 accumulate.
        Frame g;
        g.set_n_rows_hint(6);
        g.set_f64_column("x", f6x);
        try {
            g.bygen("b", "sum(x)", {}, {}, "_n>=3 & _n<=5");
            column_is(g, "b", {M, M, 60, 70, 100, M}, "bygen, empty by-list: sum(x) in 3/5 (folded)");
        } catch (const std::exception& e) {
            std::printf("  FAIL empty by-list bygen threw: %s\n", e.what());
            ++g_fail;
        }
        if (gen(g, "n", "sum(x)", "_n>=3 & _n<=5"))
            column_is(g, "n", {M, M, 60, 70, 100, M}, "generate: sum(x) in 3/5 (folded)");
        // BUG-837, the same rule without `by` (Frame::generate's mask): the frame is
        // the one group, so x[_n-1] is missing on row 1 only, and row 1 is IN. The
        // empty-by-list bygen gives the same column.
        if (gen(g, "f", "sum(x)", "x[_n-1]"))
            column_is(g, "f", {20, 60, 120, 130, 160, 210}, "generate: sum(x) if x[_n-1] (missing selects)");
        try {
            g.bygen("fb", "sum(x)", {}, {}, "x[_n-1]");
            column_is(g, "fb", {20, 60, 120, 130, 160, 210},
                      "bygen, empty by-list: sum(x) if x[_n-1] (missing selects)");
        } catch (const std::exception& e) {
            std::printf("  FAIL empty by-list bygen x[_n-1] threw: %s\n", e.what());
            ++g_fail;
        }
    }
    // BUG-841: replace with a running sum sums the selected rows only and leaves the
    // others at their old value (GenQualifier::keep_unselected, eval --gen-replace).
    // The folded cond(if, sum(x), old) form summed the unselected rows too (C09 gave
    // -1 60 120 130 160 210). F6, target = -1 everywhere.
    {
        GenQualifier keep;
        keep.keep_unselected = true;
        auto rep = [&](const std::vector<std::string>& by, const std::string& mask,
                       const GenQualifier& q, const std::vector<double>& want,
                       const std::string& label) {
            Frame g;
            g.set_n_rows_hint(6);
            g.set_f64_column("gg", f6g);
            g.set_f64_column("x", f6x);
            g.set_f64_column("t", std::vector<double>(6, -1.0));
            try {
                if (by.empty()) g.generate("t", "sum(x)", false, mask, StoreType::Double, q);
                else g.bygen("t", "sum(x)", by, {}, mask, StoreType::Double, q);
                column_is(g, "t", want, label);
            } catch (const std::exception& e) {
                std::printf("  FAIL %s threw: %s\n", label.c_str(), e.what());
                ++g_fail;
            }
        };
        rep({}, "_n > 1", keep, {-1, 40, 100, 110, 140, 190},
            "w2m_bymask C09 (:361): replace c09 = sum(x) if _n > 1");
        rep(byg, "_n > 1", keep, {-1, 40, 100, -1, 30, 80},
            "w2m_bymask B09 (:265): by gg: replace b09 = sum(x) if _n > 1");
        // Without keep_unselected the same call is generate's: excluded rows missing.
        rep({}, "_n > 1", GenQualifier(), {M, 40, 100, 110, 140, 190},
            "generate (no keep): sum(x) if _n > 1 (= C04 :307)");
        // The `in` range under replace: w2m_bymask G03 :494 (`replace g03 = group(2)
        // in 2/4`, g03 = x) gives 20 1 1 2 30 50.
        GenQualifier r;
        r.in_first = 2;
        r.in_last = 4;
        r.keep_unselected = true;
        Frame g;
        g.set_n_rows_hint(6);
        g.set_f64_column("g03", f6x);
        try {
            g.bygen("g03", "group(2)", {}, {}, "", StoreType::Double, r);
            column_is(g, "g03", {20, 1, 1, 2, 30, 50},
                      "w2m_bymask G03 (:494): replace g03 = group(2) in 2/4");
        } catch (const std::exception& e) {
            std::printf("  FAIL G03 threw: %s\n", e.what());
            ++g_fail;
        }
    }
    {
        // D11 (:728): bysort g (x): replace d11 = sum(x) if _n > 1 on F12 (stored
        // unsorted; values mapped back to the stored rows as for D01 above).
        const double X = stata_missing(kMissSys);
        GenQualifier keep;
        keep.keep_unselected = true;
        Frame g;
        g.set_n_rows_hint(12);
        g.set_f64_column("g", {2, 1, 3, 1, 2, 3, 1, 2, 1, 3, 1, 2});
        g.set_f64_column("x", {7, 30, 12, 10, X, 12, 25, 3, X, 40, 18, 9});
        g.set_f64_column("t", std::vector<double>(12, -1.0));
        try {
            g.bygen("t", "sum(x)", {"g"}, {"x"}, "_n > 1", StoreType::Double, keep);
            column_is(g, "t", {7, 73, -1, -1, 16, 12, 43, -1, 73, 52, 18, 16},
                      "w2m_bymask D11 (:728): bysort g (x): replace d11 = sum(x) if _n > 1");
        } catch (const std::exception& e) {
            std::printf("  FAIL D11 threw: %s\n", e.what());
            ++g_fail;
        }
    }

    // BUG-838: a sum() inside the mask was never materialised, so it read as missing
    // and (with any comparison) selected every row. It is now the running total the
    // folded cond() form already computes -- per group under `by`. On F6, sum(x) > 30
    // selects the rows `_n > 1` selects, so the answers are the oracle's A02 (:79)
    // and C04 (:307) records.
    bymask(f6g, f6x, byg, {}, "sum(x)", "sum(x) > 30", {M, 40, 100, M, 30, 80},
           "by gg: sum(x) if sum(x) > 30 (= A02 :79)");
    {
        Frame g;
        g.set_n_rows_hint(6);
        g.set_f64_column("x", f6x);
        if (gen(g, "c", "sum(x)", "sum(x) > 30"))
            column_is(g, "c", {M, 40, 100, 110, 140, 190}, "generate: sum(x) if sum(x) > 30 (= C04 :307)");
    }

    // The column-at-a-time path and the per-row path are the same numbers: cond()
    // forces the per-row evaluator.
    Frame v;
    std::vector<double> ps, ts;
    for (int i = 1; i <= 50; ++i) {
        ps.push_back(i / 51.0);
        ts.push_back(-6.0 + 0.25 * i);
    }
    v.set_n_rows_hint(50);
    v.set_f64_column("p", ps);
    v.set_f64_column("t", ts);
    const char* fns[][2] = {{"invttail(7.5,p)", "cond(1,invttail(7.5,p),.)"},
                            {"invt(7.5,p)", "cond(1,invt(7.5,p),.)"},
                            {"ttail(7.5,t)", "cond(1,ttail(7.5,t),.)"},
                            {"normal(t)", "cond(1,normal(t),.)"},
                            {"invnormal(p)", "cond(1,invnormal(p),.)"},
                            {"sign(t)", "cond(1,sign(t),.)"}};
    for (auto& f : fns) {
        const std::string a = std::string("__va") + std::to_string(k), b = std::string("__vb") + std::to_string(k);
        ++k;
        if (!gen(v, a, f[0]) || !gen(v, b, f[1])) continue;
        bool same = true;
        for (int i = 0; i < 50; ++i) {
            const double x = v.col(a).f64[static_cast<size_t>(i)], y = v.col(b).f64[static_cast<size_t>(i)];
            if (!(x == y || (std::isnan(x) && std::isnan(y)))) same = false;
        }
        ok(same, std::string("vector == scalar path: ") + f[0]);
    }
}

// ---- FU-811: operator grouping (oracle w2ac_expr_precedence) ---------------------
// Oracle: internal capture
// w2ac_expr_precedence.log (StataBE 19.5; the eval-lane golden copies it). Every value
// below is the %21x record at the cited log line. StataBE reads negation and `!`
// looser than `^` (-a^2 = -(a^2), !a^0 = !(a^0)), `^` LEFT to right (2^3^2 = 64), and a
// sign right of `^` covers the next operand only (2^-a^2 = (2^(-a))^2). The engine
// read (-a)^2, (!a)^0, 2^(3^2) and 2^((-a)^2) before FU-811, so every non-control arm
// below fails on the old engine (controls: sub_pow, pow_negexp, not_pow2, neg_neg,
// not_eq, lt_eq). Each shape is checked on the column path, on the per-row path
// (cond() forces it) and as a keep-if condition.
// FU-811 arms: one row per gen_<shape> record of the oracle (log line, text, rows 1..10).
struct PrecGen { const char* arm; int line; const char* expr; double v[10]; };
static const PrecGen kPrecGen[] = {
    {"gen_neg_pow", 238, "-a^2",
     {-0x1.2000000000000p+3, -0x1.0000000000000p+2, 0x0.0p+0, NAN, -0x1.2000000000000p+1, -0x1.0000000000000p-2, -0x1.0000000000000p+2, -0x1.2000000000000p+3, -0x1.0000000000000p+0, -0x1.0000000000000p-4}},
    {"gen_neg_pow_ws", 249, "- a ^ 2",
     {-0x1.2000000000000p+3, -0x1.0000000000000p+2, 0x0.0p+0, NAN, -0x1.2000000000000p+1, -0x1.0000000000000p-2, -0x1.0000000000000p+2, -0x1.2000000000000p+3, -0x1.0000000000000p+0, -0x1.0000000000000p-4}},
    {"gen_negpar_pow", 260, "-(a)^2",
     {-0x1.2000000000000p+3, -0x1.0000000000000p+2, 0x0.0p+0, NAN, -0x1.2000000000000p+1, -0x1.0000000000000p-2, -0x1.0000000000000p+2, -0x1.2000000000000p+3, -0x1.0000000000000p+0, -0x1.0000000000000p-4}},
    {"gen_h7", 270, "-(1-a)^2*b",
     {-0x1.0000000000000p+3, 0x1.2000000000000p+3, -0x1.0000000000000p+2, NAN, NAN, -0x1.b000000000000p+2, 0x0.0p+0, -0x1.0000000000000p+5, 0x0.0p+0, -0x1.2000000000000p-1}},
    {"gen_exp_negpow", 281, "exp(-a^2/2)",
     {0x1.6c0504695c417p-7, 0x1.152aaa3bf81ccp-3, 0x1.0000000000000p+0, NAN, 0x1.4c71b2477ab20p-2, 0x1.c3d6a24ed8222p-1, 0x1.152aaa3bf81ccp-3, 0x1.6c0504695c417p-7, 0x1.368b2fc6f960ap-1, 0x1.f03f56a88b5d8p-1}},
    {"gen_sub_pow", 292, "1-a^2",
     {-0x1.0000000000000p+3, -0x1.8000000000000p+1, 0x1.0000000000000p+0, NAN, -0x1.4000000000000p+0, 0x1.8000000000000p-1, -0x1.8000000000000p+1, -0x1.0000000000000p+3, 0x0.0p+0, 0x1.e000000000000p-1}},
    {"gen_chain_const", 302, "2^3^2",
     {0x1.0000000000000p+6, 0x1.0000000000000p+6, 0x1.0000000000000p+6, 0x1.0000000000000p+6, 0x1.0000000000000p+6, 0x1.0000000000000p+6, 0x1.0000000000000p+6, 0x1.0000000000000p+6, 0x1.0000000000000p+6, 0x1.0000000000000p+6}},
    {"gen_chain_data", 313, "a^2^3",
     {0x1.6c80000000000p+9, 0x1.0000000000000p+6, 0x0.0p+0, NAN, 0x1.6c80000000000p+3, 0x1.0000000000000p-6, 0x1.0000000000000p+6, 0x1.6c80000000000p+9, 0x1.0000000000000p+0, 0x1.0000000000000p-12}},
    {"gen_pow_negexp", 324, "2^-a",
     {0x1.0000000000000p-3, 0x1.0000000000000p+2, 0x1.0000000000000p+0, NAN, 0x1.6a09e667f3bcdp-2, 0x1.6a09e667f3bccp+0, 0x1.0000000000000p-2, 0x1.0000000000000p+3, 0x1.0000000000000p-1, 0x1.ae89f995ad3adp-1}},
    {"gen_neg_pow_negexp", 335, "-2^-a",
     {-0x1.0000000000000p-3, -0x1.0000000000000p+2, -0x1.0000000000000p+0, NAN, -0x1.6a09e667f3bcdp-2, -0x1.6a09e667f3bccp+0, -0x1.0000000000000p-2, -0x1.0000000000000p+3, -0x1.0000000000000p-1, -0x1.ae89f995ad3adp-1}},
    {"gen_pow_neg_pow", 346, "2^-a^2",
     {0x1.0000000000000p-6, 0x1.0000000000000p+4, 0x1.0000000000000p+0, NAN, 0x1.0000000000001p-3, 0x1.ffffffffffffep+0, 0x1.0000000000000p-4, 0x1.0000000000000p+6, 0x1.0000000000000p-2, 0x1.6a09e667f3bccp-1}},
    {"gen_not_pow0", 356, "!a^0",
     {0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0}},
    {"gen_not_pow2", 366, "!a^2",
     {0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0}},
    {"gen_neg_neg", 377, "- -a",
     {0x1.8000000000000p+1, -0x1.0000000000000p+1, 0x0.0p+0, NAN, 0x1.8000000000000p+0, -0x1.0000000000000p-1, 0x1.0000000000000p+1, -0x1.8000000000000p+1, 0x1.0000000000000p+0, 0x1.0000000000000p-2}},
    {"gen_not_eq", 387, "!a==b",
     {0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0}},
    {"gen_lt_eq", 396, "a<b==c",
     {0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0}},
    {"gen_eq_gt", 405, "b==c>a",
     {0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0}},
};
// keep_<shape> records: rows left by `keep if <cond>` (the oracle counts after the keep).
struct PrecKeep { const char* arm; int line; const char* cond; int64_t n; };
static const PrecKeep kPrecKeep[] = {
    {"keep_neg_pow", 1036, "-a^2 < 0", 8},
    {"keep_neg_pow_ws", 1068, "- a ^ 2 < 0", 8},
    {"keep_negpar_pow", 1100, "-(a)^2 < 0", 8},
    {"keep_h7", 1132, "-(1-a)^2*b < 0", 5},
    {"keep_exp_negpow", 1164, "exp(-a^2/2) < 1", 8},
    {"keep_sub_pow", 1196, "1-a^2 < 0", 5},
    {"keep_chain_const", 1228, "2^3^2 > 100", 0},
    {"keep_chain_data", 1260, "a^2^3 > 20", 5},
    {"keep_pow_negexp", 1292, "2^-a < 1", 5},
    {"keep_neg_pow_negexp", 1324, "-2^-a < 0", 9},
    {"keep_pow_neg_pow", 1356, "2^-a^2 < .1", 2},
    {"keep_not_pow0", 1388, "!a^0", 0},
    {"keep_not_pow2", 1420, "!a^2", 1},
    {"keep_neg_neg", 1452, "- -a < 0", 3},
    {"keep_not_eq", 1484, "!a==b", 1},
    {"keep_lt_eq", 1516, "a<b==c", 3},
    {"keep_eq_gt", 1548, "b==c>a", 1},
};

// ---- FU-844: comparison levels and `!` (oracle w2ac_expr_relational) -------------
// Oracle: internal capture
// w2ac_expr_relational.log (StataBE 19.5). Rows: (u, v, w) over {0,1,2}^3, t = u - 1.
// Each comparison operator is its own left-associative level, tightest first
// != > < <= >= == (`u==v>w` is u==(v>w)); `!` (or `~`) binds at negation level, so
// `!t*2` is (!t)*2; `~=` is `!=`. The engine read every comparison at one level and
// refused `~` before FU-844: 15 of the 36 generate pairs, 12 of the count conditions
// and the two `~` arms fail there.
// FU-844 arms: gen_<arm> records of w2ac_expr_relational (log line, text, rows 1..27).
struct RelGen { const char* arm; int line; const char* expr; double v[27]; };
static const RelGen kRelGen[] = {
    {"gen_eq_eq", 93, "u == v == w",
     {0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0}},
    {"gen_eq_ne", 106, "u == v != w",
     {0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0}},
    {"gen_eq_gt", 119, "u == v > w",
     {0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0}},
    {"gen_eq_lt", 132, "u == v < w",
     {0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0}},
    {"gen_eq_ge", 145, "u == v >= w",
     {0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0}},
    {"gen_eq_le", 158, "u == v <= w",
     {0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0}},
    {"gen_ne_eq", 171, "u != v == w",
     {0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0}},
    {"gen_ne_ne", 184, "u != v != w",
     {0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0}},
    {"gen_ne_gt", 197, "u != v > w",
     {0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0}},
    {"gen_ne_lt", 210, "u != v < w",
     {0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0}},
    {"gen_ne_ge", 223, "u != v >= w",
     {0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0}},
    {"gen_ne_le", 236, "u != v <= w",
     {0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0}},
    {"gen_gt_eq", 249, "u > v == w",
     {0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0}},
    {"gen_gt_ne", 262, "u > v != w",
     {0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0}},
    {"gen_gt_gt", 275, "u > v > w",
     {0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0}},
    {"gen_gt_lt", 288, "u > v < w",
     {0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0}},
    {"gen_gt_ge", 301, "u > v >= w",
     {0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0}},
    {"gen_gt_le", 314, "u > v <= w",
     {0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0}},
    {"gen_lt_eq", 327, "u < v == w",
     {0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0}},
    {"gen_lt_ne", 340, "u < v != w",
     {0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0}},
    {"gen_lt_gt", 353, "u < v > w",
     {0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0}},
    {"gen_lt_lt", 366, "u < v < w",
     {0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0}},
    {"gen_lt_ge", 379, "u < v >= w",
     {0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0}},
    {"gen_lt_le", 392, "u < v <= w",
     {0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0}},
    {"gen_ge_eq", 405, "u >= v == w",
     {0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0}},
    {"gen_ge_ne", 418, "u >= v != w",
     {0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0}},
    {"gen_ge_gt", 431, "u >= v > w",
     {0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0}},
    {"gen_ge_lt", 444, "u >= v < w",
     {0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0}},
    {"gen_ge_ge", 457, "u >= v >= w",
     {0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0}},
    {"gen_ge_le", 470, "u >= v <= w",
     {0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0}},
    {"gen_le_eq", 483, "u <= v == w",
     {0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0}},
    {"gen_le_ne", 496, "u <= v != w",
     {0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0}},
    {"gen_le_gt", 509, "u <= v > w",
     {0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0}},
    {"gen_le_lt", 522, "u <= v < w",
     {0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0}},
    {"gen_le_ge", 535, "u <= v >= w",
     {0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0}},
    {"gen_le_le", 548, "u <= v <= w",
     {0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0}},
    {"gen_not_mul", 925, "!t*2",
     {0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+1, 0x1.0000000000000p+1, 0x1.0000000000000p+1, 0x1.0000000000000p+1, 0x1.0000000000000p+1, 0x1.0000000000000p+1, 0x1.0000000000000p+1, 0x1.0000000000000p+1, 0x1.0000000000000p+1, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0}},
    {"gen_not_div", 938, "!t/2",
     {0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p-1, 0x1.0000000000000p-1, 0x1.0000000000000p-1, 0x1.0000000000000p-1, 0x1.0000000000000p-1, 0x1.0000000000000p-1, 0x1.0000000000000p-1, 0x1.0000000000000p-1, 0x1.0000000000000p-1, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0}},
    {"gen_not_add", 951, "!t+1",
     {0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+1, 0x1.0000000000000p+1, 0x1.0000000000000p+1, 0x1.0000000000000p+1, 0x1.0000000000000p+1, 0x1.0000000000000p+1, 0x1.0000000000000p+1, 0x1.0000000000000p+1, 0x1.0000000000000p+1, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0}},
    {"gen_not_sub", 964, "!t-1",
     {-0x1.0000000000000p+0, -0x1.0000000000000p+0, -0x1.0000000000000p+0, -0x1.0000000000000p+0, -0x1.0000000000000p+0, -0x1.0000000000000p+0, -0x1.0000000000000p+0, -0x1.0000000000000p+0, -0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, -0x1.0000000000000p+0, -0x1.0000000000000p+0, -0x1.0000000000000p+0, -0x1.0000000000000p+0, -0x1.0000000000000p+0, -0x1.0000000000000p+0, -0x1.0000000000000p+0, -0x1.0000000000000p+0, -0x1.0000000000000p+0}},
    {"gen_not_gt", 977, "!t>0",
     {0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0}},
    {"gen_not_and", 990, "!t&v",
     {0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0}},
    {"gen_not_or", 1003, "!t|v",
     {0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0}},
    {"gen_tilde_pow0", 1016, "~t^0",
     {0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0}},
    {"gen_not_not", 1029, "!!t",
     {0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0}},
    {"gen_neg_not", 1042, "-!t",
     {0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, -0x1.0000000000000p+0, -0x1.0000000000000p+0, -0x1.0000000000000p+0, -0x1.0000000000000p+0, -0x1.0000000000000p+0, -0x1.0000000000000p+0, -0x1.0000000000000p+0, -0x1.0000000000000p+0, -0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0}},
    {"gen_pow_not", 1068, "2^!t",
     {0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+1, 0x1.0000000000000p+1, 0x1.0000000000000p+1, 0x1.0000000000000p+1, 0x1.0000000000000p+1, 0x1.0000000000000p+1, 0x1.0000000000000p+1, 0x1.0000000000000p+1, 0x1.0000000000000p+1, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0}},
    {"gen_pow_negneg", 1081, "2^- -t",
     {0x1.0000000000000p-1, 0x1.0000000000000p-1, 0x1.0000000000000p-1, 0x1.0000000000000p-1, 0x1.0000000000000p-1, 0x1.0000000000000p-1, 0x1.0000000000000p-1, 0x1.0000000000000p-1, 0x1.0000000000000p-1, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+1, 0x1.0000000000000p+1, 0x1.0000000000000p+1, 0x1.0000000000000p+1, 0x1.0000000000000p+1, 0x1.0000000000000p+1, 0x1.0000000000000p+1, 0x1.0000000000000p+1, 0x1.0000000000000p+1}},
    {"gen_and_or", 1094, "u|v&w",
     {0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0, 0x1.0000000000000p+0}},
    {"gen_tilde_ne_gt", 1108, "u~=v>w",
     {0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0}},
};
// count_<arm> records: r(N) of `count if <cond>` over the same 27 rows.
struct RelCount { const char* arm; int line; const char* cond; int64_t n; };
static const RelCount kRelCount[] = {
    {"count_eq_eq", 562, "u == v == w", 9},
    {"count_eq_ne", 572, "u == v != w", 9},
    {"count_eq_gt", 582, "u == v > w", 9},
    {"count_eq_lt", 592, "u == v < w", 9},
    {"count_eq_ge", 602, "u == v >= w", 9},
    {"count_eq_le", 612, "u == v <= w", 9},
    {"count_ne_eq", 622, "u != v == w", 9},
    {"count_ne_ne", 632, "u != v != w", 18},
    {"count_ne_gt", 642, "u != v > w", 6},
    {"count_ne_lt", 652, "u != v < w", 12},
    {"count_ne_ge", 662, "u != v >= w", 15},
    {"count_ne_le", 672, "u != v <= w", 21},
    {"count_gt_eq", 682, "u > v == w", 9},
    {"count_gt_ne", 692, "u > v != w", 12},
    {"count_gt_gt", 702, "u > v > w", 3},
    {"count_gt_lt", 712, "u > v < w", 15},
    {"count_gt_ge", 722, "u > v >= w", 12},
    {"count_gt_le", 732, "u > v <= w", 24},
    {"count_lt_eq", 742, "u < v == w", 9},
    {"count_lt_ne", 752, "u < v != w", 6},
    {"count_lt_gt", 762, "u < v > w", 3},
    {"count_lt_lt", 772, "u < v < w", 15},
    {"count_lt_ge", 782, "u < v >= w", 12},
    {"count_lt_le", 792, "u < v <= w", 24},
    {"count_ge_eq", 802, "u >= v == w", 9},
    {"count_ge_ne", 812, "u >= v != w", 21},
    {"count_ge_gt", 822, "u >= v > w", 24},
    {"count_ge_lt", 832, "u >= v < w", 24},
    {"count_ge_ge", 842, "u >= v >= w", 15},
    {"count_ge_le", 852, "u >= v <= w", 21},
    {"count_le_eq", 862, "u <= v == w", 9},
    {"count_le_ne", 872, "u <= v != w", 15},
    {"count_le_gt", 882, "u <= v > w", 12},
    {"count_le_lt", 892, "u <= v < w", 12},
    {"count_le_ge", 902, "u <= v >= w", 15},
    {"count_le_le", 912, "u <= v <= w", 21},
};

static Frame prec_fixture() {
    const double M = stata_missing(kMissSys);
    Frame fr;
    fr.set_n_rows_hint(10);
    fr.set_f64_column("a", {3, -2, 0, M, 1.5, -0.5, 2, -3, 1, 0.25});
    fr.set_f64_column("b", {2, -1, 4, 1, M, 3, 0, 2, -2, 1});
    fr.set_f64_column("c", {1, 0, 1, 0, 1, 0, 1, 0, 1, 0});
    return fr;
}

// T2 (rtol 1e-9, TOLERANCE.md) per row; a missing oracle row must be missing.
static void prec_rows(const Frame& fr, const std::string& name, const PrecGen& g,
                      const std::string& path) {
    bool good = true;
    std::string got;
    for (int i = 0; i < 10; ++i) {
        const double v = fr.col(name).f64[static_cast<size_t>(i)], o = g.v[i];
        const bool row_ok = std::isnan(o) ? std::isnan(v)
                                          : (!std::isnan(v) && std::fabs(v - o) <= 1e-9 * std::fabs(o));
        if (!row_ok) good = false;
        char b[32];
        std::snprintf(b, sizeof b, " %.17g", v);
        got += b;
    }
    char head[160];
    std::snprintf(head, sizeof head, "%s (:%d) %s: generate double = %s ->", g.arm, g.line,
                  path.c_str(), g.expr);
    ok(good, head + got);
}

static void precedence_arms() {
    std::printf("w2ac_expr_precedence arms (engine Parser, FU-811)\n");
    int k = 0;
    for (const PrecGen& g : kPrecGen) {
        Frame fr = prec_fixture();
        const std::string a = "__pa" + std::to_string(k), b = "__pb" + std::to_string(k);
        ++k;
        if (gen(fr, a, g.expr)) prec_rows(fr, a, g, "column path");
        if (gen(fr, b, std::string("cond(1,") + g.expr + ",.)")) prec_rows(fr, b, g, "per-row path");
    }
    for (const PrecKeep& q : kPrecKeep) {
        Frame fr = prec_fixture();
        char buf[160];
        try {
            fr.keep_if(q.cond);
            std::snprintf(buf, sizeof buf, "%s (:%d): keep if %s leaves %lld (oracle %lld)", q.arm,
                          q.line, q.cond, static_cast<long long>(fr.n_rows()),
                          static_cast<long long>(q.n));
            ok(fr.n_rows() == q.n, buf);
        } catch (const std::exception& e) {
            std::printf("  FAIL %s keep if %s threw: %s\n", q.arm, q.cond, e.what());
            ++g_fail;
        }
    }
}

static Frame rel_fixture() {
    Frame fr;
    fr.set_n_rows_hint(27);
    std::vector<double> u, v, w, t;
    for (int i = 0; i < 27; ++i) {
        u.push_back(i / 9);
        v.push_back((i / 3) % 3);
        w.push_back(i % 3);
        t.push_back(i / 9 - 1);
    }
    fr.set_f64_column("u", u);
    fr.set_f64_column("v", v);
    fr.set_f64_column("w", w);
    fr.set_f64_column("t", t);
    return fr;
}

static void relational_arms() {
    std::printf("w2ac_expr_relational arms (engine Parser, FU-844)\n");
    int k = 0;
    for (const RelGen& g : kRelGen) {
        Frame fr = rel_fixture();
        const std::string name = "__ra" + std::to_string(k++);
        if (!gen(fr, name, g.expr)) continue;
        bool good = true;
        for (int i = 0; i < 27; ++i)
            if (fr.col(name).f64[static_cast<size_t>(i)] != g.v[i]) good = false;
        char head[160];
        std::snprintf(head, sizeof head, "%s (:%d): generate double = %s", g.arm, g.line, g.expr);
        ok(good, head);
    }
    for (const RelCount& q : kRelCount) {
        Frame fr = rel_fixture();
        char buf[160];
        try {
            fr.keep_if(q.cond);
            std::snprintf(buf, sizeof buf, "%s (:%d): keep if %s leaves %lld (oracle %lld)", q.arm,
                          q.line, q.cond, static_cast<long long>(fr.n_rows()),
                          static_cast<long long>(q.n));
            ok(fr.n_rows() == q.n, buf);
        } catch (const std::exception& e) {
            std::printf("  FAIL %s keep if %s threw: %s\n", q.arm, q.cond, e.what());
            ++g_fail;
        }
    }
}

// ---- `!` before a sign (oracle w2ac_expr_valueref) -------------------------------
// Oracle: internal capture
// w2ac_expr_valueref.log (StataBE 19.5, the same 27 rows u v w t). A sign after `!` or
// `~`, spaces or not, is invalid syntax (r(198)) in generate and count; `!(-t)` is
// legal. The engine read every one of these as !(sign t) before. PIET reports the
// engine's syntax error at rc 1, not 198 (follow-up); here the refusal itself is pinned.
struct NotSign { const char* arm; int line; const char* expr; };
static const NotSign kNotSign[] = {
    {"gen_not_neg_sp", 491, "! -t"},
    {"gen_not_plus", 502, "!+t"},
    {"gen_tilde_neg", 522, "~-t"},
    {"gen_not_not_neg", 533, "!!-t"},
    {"gen_neg_not_neg", 544, "-!-t"},
    {"gen_pow_not_neg", 555, "2^!-t"},
};

static void not_sign_arms() {
    std::printf("w2ac_expr_valueref arms (engine Parser, `!` before a sign)\n");
    for (const NotSign& a : kNotSign) {
        Frame fr = rel_fixture();
        char buf[160];
        std::snprintf(buf, sizeof buf, "%s (:%d): generate = %s is invalid syntax", a.arm, a.line, a.expr);
        ok(throws(fr, "g", a.expr, "a sign after `!`"), buf);
        Frame fk = rel_fixture();
        bool threw = false;
        try { fk.keep_if(a.expr); } catch (const std::exception&) { threw = true; }
        std::snprintf(buf, sizeof buf, "%s: keep if %s is invalid syntax, rows kept", a.arm, a.expr);
        ok(threw && fk.n_rows() == 27, buf);
    }
    // count_not_neg (:566) / count_not_neg_sp (:576): r(198) and r(N) untouched.
    // gen_not_paren_neg (:512): !(-t) = 0, 1, 0 at rows 1, 10, 19.
    Frame fr = rel_fixture();
    fr.generate("g", "!(-t)", true);
    const auto& g = fr.col("g").f64;
    ok(g[0] == 0.0 && g[9] == 1.0 && g[18] == 0.0, "gen_not_paren_neg (:512): generate = !(-t)");
}

// ---- The same arms on Stata's own stream (--rng-mt64, core/rng_mt64.h) ----
// w2c_functions.log runiform_dist / runiformint_dist pin ONE realization: `set seed 1`,
// 200 rows, stored as float. With the session state PIET sends, the engine must give
// Stata's numbers exactly, consume draws in row order, and report how many it took.
static void rng_mt64_arms() {
    std::printf("w2c_functions RNG arms (Stata mt64 stream)\n");
    Frame fr;
    fr.set_n_rows_hint(200);
    fr.set_f64_column("one", std::vector<double>(200, 1.0));
    cecon::Mt64 session(1);  // `set seed 1`
    auto fmoments = [&fr](const char* name) {
        float mn = INFINITY, mx = -INFINITY;
        double sum = 0.0;
        for (int64_t i = 0; i < 200; ++i) {
            const float v = static_cast<float>(fr.col(name).f64[static_cast<size_t>(i)]);
            mn = std::min(mn, v); mx = std::max(mx, v); sum += v;
        }
        return Moments{mn, mx, sum / 200.0, true};
    };
    ok(begin_rng_mt64(session.state_string()), "--rng-mt64 accepts the session state string");
    bool g1 = gen(fr, "__m_unif", "runiform()");
    uint64_t took = end_rng_mt64();
    session.discard(took);
    if (g1) {
        const Moments m = fmoments("__m_unif");
        ok(took == 200 && m.mn == static_cast<float>(.0006079551531002) &&
               m.mx == static_cast<float>(.9989203214645386) &&
               std::fabs(m.mean - .4739880738774082) < 1e-15,
           "runiform_dist on mt64: Stata's min/max/mean exactly, 200 draws");
    }
    ok(begin_rng_mt64(session.state_string()), "the advanced state round-trips");
    const bool g2 = gen(fr, "__m_unifint", "runiformint(0,5)");
    took = end_rng_mt64();
    if (g2) {
        const Moments m = fmoments("__m_unifint");
        ok(took >= 200 && m.mn == 0.0f && m.mx == 5.0f && std::fabs(m.mean - 2.815) < 1e-12,
           "runiformint_dist on mt64: Stata's 0 / 5 / 2.815 (top-3-bit rejection)");
    }
    // Rows outside `in` draw nothing (StataBE: `gen f = runiform() in 4/6` gives
    // row 4 the stream's first draw).
    cecon::Mt64 ref(5);
    const double first = ref.runiform();
    ok(begin_rng_mt64(cecon::Mt64(5).state_string()), "seed 5 state");
    GenQualifier q;
    q.in_first = 4; q.in_last = 6;
    bool g3 = true;
    try { fr.generate("__m_in", "runiform()", true, "", StoreType::Double, q); }
    catch (const std::exception&) { g3 = false; }
    took = end_rng_mt64();
    ok(g3 && took == 3 && fr.col("__m_in").f64[3] == first,
       "an `in` range draws only its rows, in order");
}

// exp() and invlogit() at the underflow edge, StataBE 19.5 %21x bits measured
// 2026-10-27 (scratch-r150/tail_fixes/stata_probe/il.do, ex2.do, ex3.do): Stata's
// exp() is 0 for an argument below -709 and libm's (subnormal) value from -709 up;
// invlogit() is exp(z)/(1+exp(z)) for z < 0 with that exp(), so it is 0 at
// z = -717.5, -734.2, -740.5 where libm's exp leaves a subnormal. Both the
// whole-column and the masked (row) evaluator are checked.
static void tail_exp_arms() {
    Frame fr;
    fr.set_n_rows_hint(7);
    const std::vector<double> z = {-709.0, -0x1.6280000000001p+9, -717.5, -734.2, -740.5,
                                   -708.5, -0x1.627ffffffffffp+9};
    fr.set_f64_column("z", z);
    const std::vector<double> want = {0x0.8bfe55de02338p-1022, 0.0, 0.0, 0.0, 0.0,
                                      0x0.e6cf6d08897acp-1022, 0x0.8bfe55de02450p-1022};
    for (const char* mask : {"", "z < 0"}) {
        const std::string m(mask);
        if (gen(fr, "e_" + std::to_string(m.size()), "exp(z)", m))
            column_is(fr, "e_" + std::to_string(m.size()), want,
                      std::string("exp() is 0 below -709, libm's subnormal from -709 up") + (m.empty() ? "" : " (if)"));
        if (gen(fr, "l_" + std::to_string(m.size()), "invlogit(z)", m))
            column_is(fr, "l_" + std::to_string(m.size()), want,
                      std::string("invlogit() far tail = exp(z)/(1+exp(z)) with Stata's exp()") + (m.empty() ? "" : " (if)"));
    }
}

// `^` as StataBE 19.5 computes it, %21x bits measured 2026-10-27 on 20,000 random
// doubles per exponent (scratch-r150/tail_fixes/stata_probe/pw.do, pw2.do): an
// integer power |n| <= 64 is a left-to-right product (x^-n of 1/x), anything else
// exp(y*ln(x)) with Stata's exp() (core/stata_arith.h). Every row below is a point
// where libm's pow() differs from Stata in the last bit, or a missing/edge rule.
static void stata_pow_arms() {
    const double M = stata_missing(kMissSys);
    Frame fr;
    fr.set_n_rows_hint(15);
    const std::vector<double> x = {-0x1.0f78eedfcc470p+2, -0x1.782c44600f812p+0, 0x1.639dc2926a306p+2,
                                   0x1.644d631edf3edp+1, 0x1.290ee71eca18bp+3, 0x1.639dc2926a306p+2,
                                   -2.0, 2.0, 2.0, 1.0, M, 0.0, 0.0, -8.0, 0.1};
    const std::vector<double> y = {2.0, 3.0, 4.0, -2.0, 0.5, 1.5, 65.0, -1022.0, -1023.0, M, 0.0, 0.0,
                                   -1.0, 1.0 / 3.0, -2.0};
    fr.set_f64_column("px", x);
    fr.set_f64_column("py", y);
    const std::vector<double> want = {
        0x1.1fe142dea332cp+4,      // x*x            (libm pow ...32bp+4)
        -0x1.961e4e9e30138p+1,     // x*x*x          (libm ...139p+1)
        0x1.dc9fe931d87efp+9,      // ((x*x)*x)*x    (libm ...7f0p+9)
        0x1.084f36b13ee24p-3,      // (1/x)*(1/x)    (libm ...e25p-3)
        0x1.85fdf78a52137p+1,      // exp(.5*ln x), not sqrt
        0x1.a3222fa09f15ep+3,      // exp(1.5*ln x)  (libm ...160p+3)
        -0x1.0000000000002p+65,    // (-2)^65 = -exp(65*ln 2)
        0x1.000000000007cp-1022,   // 2^-1022 = exp(-1022*ln 2)
        0.0,                       // 2^-1023: exp() of an argument below -709
        M, M,                      // 1^. and .^0 are missing (C pow gives 1)
        1.0, M, M,                 // 0^0 = 1, 0^-1 and (-8)^(1/3) missing
        100.0};                    // .1^-2 = (1/.1)*(1/.1) (libm 99.99999999999999)
    for (const char* mask : {"", "px < 1000"}) {
        const std::string m(mask), nm = "pw_" + std::to_string(m.size());
        if (gen(fr, nm, "px^py", m))
            column_is(fr, nm, want, std::string("x^y is Stata's power, not libm pow") + (m.empty() ? "" : " (if)"));
    }
    if (gen(fr, "pw_c", "px^2"))
        exact(fr, "pw_c", 1, 0x1.1fe142dea332cp+4, "x^2 with a literal exponent is x*x");
}

// The overflow edge, StataBE 19.5 %21x bits measured 2026-10-27 (scratch-r150/
// tail_fixes/stata_probe2/probe.do, p2.do, p3.do): exp() is `.` for an argument
// above 709 (exp(709) = +1.d422d2be5dc9bX+3fe is finite), and `^` is `.` for a
// result of 2^1023 or more while a negative result is not clamped
// ((-4.5e102)^3 = -1.038859727d162X+3ff). 9.4e153^2 = +1.f750b55516b59X+3fe.
static void overflow_edge_arms() {
    const double M = stata_missing(kMissSys);
    Frame fr;
    fr.set_n_rows_hint(4);
    fr.set_f64_column("z", {709.0, 0x1.6280000000001p+9, 709.5, -709.0});
    const std::vector<double> ez = {0x1.d422d2be5dc9bp+1022, M, M, 0x0.8bfe55de02338p-1022};
    for (const char* mask : {"", "z < 1000"}) {
        const std::string m(mask), nm = "eo_" + std::to_string(m.size());
        if (gen(fr, nm, "exp(z)", m))
            column_is(fr, nm, ez, std::string("exp() is `.` above 709, finite at 709") + (m.empty() ? "" : " (if)"));
    }
    Frame f2;
    f2.set_n_rows_hint(3);
    f2.set_f64_column("px", {0x1.66f47c5e4f8e7p+511, 0x1.6ac610c15b4bep+511, -0x1.012c12e07bfd1p+341});
    f2.set_f64_column("py", {2.0, 2.0, 3.0});
    const std::vector<double> want = {0x1.f750b55516b59p+1022, M, -0x1.038859727d162p+1023};
    for (const char* mask : {"", "px < 1e200"}) {
        const std::string m(mask), nm = "po_" + std::to_string(m.size());
        if (gen(f2, nm, "px^py", m))
            column_is(f2, nm, want,
                      std::string("x^y is `.` from 2^1023 up, a negative result is not clamped") + (m.empty() ? "" : " (if)"));
    }
}

int main() {
    data_arms();
    rng_arms();
    rng_mt64_arms();
    measured_edges();
    precedence_arms();
    relational_arms();
    not_sign_arms();
    tail_exp_arms();
    stata_pow_arms();
    overflow_edge_arms();
    std::printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
