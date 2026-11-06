// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "data/frame.h"

#include <fcntl.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/clonefile.h>
#elif defined(__linux__)
#include <linux/fs.h>
#include <sys/ioctl.h>
#endif

#include "io/win_compat.h"  // pread/pwrite shim + UTF-8 path helpers (no-op on POSIX)
#include "core/error.h"     // EngineError("bad_input", ...) for the fweight honest-error paths
#include "core/eng_counters.h"  // W0: patch-vs-full write-plane ledger (P2PERF §W0)
#include "core/exact_sum.h"     // ExactSum: Stata's summary-statistic accumulation (BUG-1013)
#include "core/stata_arith.h"   // exp()/invlogit()/^ with StataBE's bits
#include "core/threads.h"   // MAXPERF Stage 3: ThreadPool for morsel-parallel collapse/merge
#include "data/radix_sort.h"  // C2-F8: value-identical LSD radix for gsort
#include "data/missing.h"     // Stata `.` / `.a`-`.z` kinds + their total order
#include "core/rng_uniform.h"
#include "core/rng_mt64.h"   // Stata-exact session stream (--rng-mt64)
#include "core/distributions.h"  // normal/invnormal/ttail/invttail/invt: one copy, shared
#include "data/stata_dates.h"  // BUG-371: %td calendar, shared with PIET's pyeval layer
#include "core/ident.h"        // Unicode-aware identifier byte tests (non-ASCII bytes are name bytes)
#include "core/expr_blanks.h"  // FU-875: the blanks Stata drops inside ( ) and [ ]
#include "data/ts_operators.h"  // BUG-369: L./F./D./S. inside expressions (gen/replace/if)

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <memory>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

#include "nlohmann/json.hpp"
#include "data/frame_internal.h"

namespace cecon {
namespace {

using frame_detail::NAd;  // R2: single source in frame_internal.h
constexpr double INF = std::numeric_limits<double>::infinity();
// Stata treats missing as +infinity in comparisons (e.g. `. > 2` is true).
// Kept for the ORDERING-only call sites (sort keys); the expression
// comparators below use cecon::stata_cmp instead, because `mi()` collapses
// every missing to one value and so cannot tell `.` from `.a` (Stata's
// equality is kind-exact and its missings are ordered `. < .a < … < .z`).
inline double mi(double v) { return std::isnan(v) ? INF : v; }
// Stata's total order, extended-missing aware — see data/missing.h.
inline int scmp(double a, double b) { return cecon::stata_cmp(a, b); }
// Stata has no user-visible IEEE Inf concept: overflow (exp of a huge number),
// divide-by-zero, and out-of-domain math (ln of a non-positive number, etc.)
// all become `.` (missing) rather than a raw +-Inf that then silently keeps
// propagating/comparing as if it were an ordinary finite number. Every
// arithmetic/math-function case in ev() that can produce a non-finite result
// must funnel it through this so the isnan()-based missing checks used
// everywhere downstream (summarize, keep_if, ...) actually catch it. A
// genuine NaN input (already missing) maps to NAd unchanged; a finite result
// passes through untouched (no-op for the overwhelming common case).
inline double fin(double v) { return std::isfinite(v) ? v : NAd; }

// log10, asinh and exp as StataBE 19.5 computes them (PIET's expression layer
// carries the same rules, piet/native/src/eval/exprfns.cpp): log10(x) is
// ln(x)/ln(10), so log10(1000) is 2.9999999999999996; asinh(x) is
// ln(x + sqrt(x^2 + 1)), odd-symmetric, ln|x| + ln 2 where x^2 overflows
// (asinh(1e-10) +1.b7cdffffa18d8X-022, not libm's ...fd9d7bdbb); exp past
// Stata's largest double 2^1023 is missing and a result below 1e-323 is 0
// (tests/eval/golden/aer_w2/w2x_fn_string_math.log fx_log10_gen/_edges,
// fx_asinh_edges, fx_exp_edges). The underflow edge is an argument below -709,
// not a result below 1e-323: exp(-717.5) is 0 in Stata (cecon::stata_exp_value,
// core/stata_arith.h, identified from StataBE bits).
inline double stata_log10(double x) {
    static const double kLn10 = std::log(10.0);
    return fin(std::log(x) / kLn10);
}
inline double stata_asinh(double x) {
    const double ax = std::fabs(x);
    const double sq = ax * ax + 1.0;
    const double r = std::isfinite(sq) ? std::log(ax + std::sqrt(sq)) : std::log(ax) + std::log(2.0);
    return fin(x < 0 ? -r : r);
}
inline double stata_exp(double x) {
    const double r = cecon::stata_exp_value(x);
    if (!(r < 0x1p1023)) return NAd;
    return r;
}
// The truth value `!`, `&` and `|` read from an operand: nonzero is true, and a
// missing value is nonzero, so it is TRUE ([U] 13.2.4). Oracle w2c_functions.log
// operators_missing: `!x` is 0 on x = . and .a, `x & 1` and `x | 0` are 1 there,
// `. & 1` is 1, `. | 0` is 1. The result of the three operators is always 0/1,
// never missing. (They used to read a missing operand as false, and `!` passed
// it through as missing: BUG-620.)
inline bool truthy(double v) { return std::isnan(v) || v != 0.0; }

// ---------- Stata-style expression evaluator (recursive descent) ----------
// Grammar: or > and > cmp > add > mul > pow(right) > unary > primary.
// Supports + - * / ^, comparisons (missing=+inf), & | !, n-ary functions, a bare
// `.` missing literal, _pi, numbers, variables, and (inside a bysort context)
// _n / _N / var[index] subscripting.

// Draw coordinates for this invocation. A draw is a pure function of
// (seed, stream, node, row) -- see core/rng_uniform.h for why that matters when two
// evaluators walk the rows in different orders. Set once from the CLI.
int64_t g_rng_seed = 123456789;
int64_t g_rng_stream = 0;
int64_t g_rng_node_counter = 0;
// Whether the caller supplied the coordinates at all. rnormal() and uniform()
// refuse without them (see the parser): PIET's argv injection is keyed on the
// spelling `runiform`, so without this guard `gen a = rnormal()` then
// `gen b = rnormal()` would each run at the default (seed, stream 0) and return
// the SAME column twice -- noise that is silently perfectly correlated, and a
// `set seed` that is silently ignored.
bool g_rng_coords_set = false;

// Stata's session stream (core/rng_mt64.h). When the caller hands the state over
// (--rng-mt64, PIET's injectRngContextForExpr) every draw comes from it in the
// order the evaluator reaches the draw sites -- observation order, the rows a
// generate actually evaluates, left to right within a row -- and g_mt64_draws
// counts the raw 64-bit draws taken so the caller can advance its own copy.
// Without it the Philox coordinates above stay in charge (direct CLI callers).
// Owned by ONE eval call: begin_rng_mt64/end_rng_mt64 bracket it, so a resident
// `cobra serve` process never hands a later request this request's stream.
// g_mt64.taken() is the count of raw draws since the state arrived.
cecon::Mt64 g_mt64;
bool g_mt64_on = false;
inline double mt64_uniform() { return g_mt64.runiform(); }

// The coordinates belong to ONE call, not to the process. Under `cobra serve` (PIET's
// default plane) one resident process answers every request, and a request that
// carries no --rng-seed/--rng-stream would otherwise inherit the previous request's
// coordinates: measured before this guard, `gen u = runiform()` then
// `gen z = rnormal()` in one PIET session answered z with rc 0 from u's stale
// (seed, stream), so the refusal above never fired. Every expression entry point of
// Frame opens one of these; when the OUTERMOST one returns, the coordinates are
// spent. (Consequence for a direct CLI caller: two --gen draws in one argv share one
// set of coordinates, so the second rnormal()/uniform() is refused -- PIET injects
// coordinates per expression and never sends that shape.)
int g_rng_call_depth = 0;
struct RngCallScope {
    RngCallScope() { ++g_rng_call_depth; }
    ~RngCallScope() {
        if (--g_rng_call_depth == 0) g_rng_coords_set = false;
    }
    RngCallScope(const RngCallScope&) = delete;
    RngCallScope& operator=(const RngCallScope&) = delete;
};

enum {
    F_ln, F_log, F_exp, F_sqrt, F_abs, F_sin, F_cos, F_tan, F_log10,
    F_asinh, F_ln1p, F_ln1m, F_int, F_float, F_round, F_floor, F_ceil,
    F_missing, F_cond, F_inlist, F_inrange, F_min, F_max, F_mod,
    // BUG-371 date family. Stata's %td value is a DAY COUNT from 1960-01-01=0.
    F_mdy, F_year, F_month, F_day, F_dow, F_doy, F_quarter, F_halfyear,
    // AER scope 2 / B3. The rest of Stata's time scales. Arithmetic lives in
    // data/stata_dates.h so PIET's pyeval layer computes the same digits from
    // the same code; every value is pinned to tests/validate/b3_funcs_oracle.json.
    F_ym, F_yq, F_yh, F_yw,
    F_mofd, F_qofd, F_hofd, F_yofd, F_wofd, F_week,
    F_dofm, F_dofq, F_dofh, F_dofy, F_dofw,
    F_cofd, F_Cofd, F_dofc, F_dofC,
    F_hh, F_mm, F_ss, F_hhC, F_mmC, F_ssC,
    F_hms, F_dhms,
    F_msofseconds, F_msofminutes, F_msofhours, F_seconds, F_minutes, F_hours,
    // Order-dependent: not a per-row function at all. See materialize_sums.
    F_sum,
    // Draws. Pure functions of (seed, stream, node, row) -- see core/rng_uniform.h.
    F_runiform, F_runiformint, F_rnormal, F_uniform,
    // Distribution functions (core/distributions.h) and sign().
    F_normal, F_invnormal, F_ttail, F_invttail, F_invt, F_sign, F_normalden,
    // invlogit()/logit() (w2c_ml_getmata.do:21 `generate yb = invlogit(...)`).
    F_invlogit, F_logit,
    // reldif(x,y) (BUG-1046: `generate r = reldif(x,2)` was rc 1).
    F_reldif,
    // group(k): needs _n/_N like a draw needs its row. See the ev() case.
    F_group
};
struct FnInfo { const char* name; int id; int amin; int amax; };
const FnInfo FNS[] = {
    {"ln", F_ln, 1, 1}, {"log", F_log, 1, 1}, {"exp", F_exp, 1, 1}, {"sqrt", F_sqrt, 1, 1},
    {"abs", F_abs, 1, 1}, {"sin", F_sin, 1, 1}, {"cos", F_cos, 1, 1}, {"tan", F_tan, 1, 1},
    {"log10", F_log10, 1, 1}, {"asinh", F_asinh, 1, 1}, {"ln1p", F_ln1p, 1, 1},
    {"ln1m", F_ln1m, 1, 1}, {"int", F_int, 1, 1}, {"round", F_round, 1, 2},
    // float(x): the value a float variable would hold — the store boundary's own
    // rule (frame_detail::round_to_store), so `count if u == float(1/3)` matches a
    // float column bit for bit. Oracle w2e_float_default fn_float_* arms.
    {"float", F_float, 1, 1},
    {"floor", F_floor, 1, 1}, {"ceil", F_ceil, 1, 1},
    // `mi()` is Stata's documented SYNONYM for missing() ([FN] Programming
    // functions) and the spelling replication do-files actually use; it was
    // absent, so `gen byte m = mi(x)` answered rc 1 while `missing(x)` on the
    // same line answered 0. Same entry, same arity — a synonym, not a second
    // implementation (BUG-431, found by the 2026-09-06 corpus census).
    {"missing", F_missing, 1, 256}, {"mi", F_missing, 1, 256},
    {"cond", F_cond, 3, 4}, {"inlist", F_inlist, 2, 256},
    {"inrange", F_inrange, 3, 3}, {"min", F_min, 1, 256}, {"max", F_max, 1, 256},
    // Stata's mod(x,y) is the FLOORED modulus x - y*floor(x/y), not C's fmod:
    // mod(-7,3) is 2, not -1. A non-positive modulus is missing, and so is a
    // missing operand. Oracle (StataBE 19.5, 2026-08-04): mod(7,3)=1,
    // mod(-7,3)=2, mod(7,-3)=., mod(7.5,2)=1.5, mod(7,0)=., mod(.,3)=.,
    // mod(7,.)=.
    {"mod", F_mod, 2, 2},
    // BUG-371 — the date family. `gen dt = mdy(1,1,year)` was an ordinary line a
    // researcher writes and the product refused it, which then cascaded: `format
    // %td dt` failed too, because dt never existed. Stata's %td scale is a signed
    // DAY COUNT with 1960-01-01 == 0 (so 1959-12-31 is -1), and every extractor
    // below is its exact inverse.
    {"mdy", F_mdy, 3, 3}, {"year", F_year, 1, 1}, {"month", F_month, 1, 1},
    {"day", F_day, 1, 1}, {"dow", F_dow, 1, 1}, {"doy", F_doy, 1, 1},
    {"quarter", F_quarter, 1, 1}, {"halfyear", F_halfyear, 1, 1},
    // B3. Note the case-sensitive pairs: %tC's family (Cofd/dofC/hhC/mmC/ssC)
    // is a DIFFERENT scale from %tc's (cofd/dofc/hh/mm/ss), differing by the
    // inserted leap seconds, and Stata spells the difference with a capital.
    {"ym", F_ym, 2, 2}, {"yq", F_yq, 2, 2}, {"yh", F_yh, 2, 2}, {"yw", F_yw, 2, 2},
    {"mofd", F_mofd, 1, 1}, {"qofd", F_qofd, 1, 1}, {"hofd", F_hofd, 1, 1},
    {"yofd", F_yofd, 1, 1}, {"wofd", F_wofd, 1, 1}, {"week", F_week, 1, 1},
    {"dofm", F_dofm, 1, 1}, {"dofq", F_dofq, 1, 1}, {"dofh", F_dofh, 1, 1},
    {"dofy", F_dofy, 1, 1}, {"dofw", F_dofw, 1, 1},
    {"cofd", F_cofd, 1, 1}, {"Cofd", F_Cofd, 1, 1},
    {"dofc", F_dofc, 1, 1}, {"dofC", F_dofC, 1, 1},
    {"hh", F_hh, 1, 1}, {"mm", F_mm, 1, 1}, {"ss", F_ss, 1, 1},
    {"hhC", F_hhC, 1, 1}, {"mmC", F_mmC, 1, 1}, {"ssC", F_ssC, 1, 1},
    {"hms", F_hms, 3, 3}, {"dhms", F_dhms, 4, 4},
    {"msofseconds", F_msofseconds, 1, 1}, {"msofminutes", F_msofminutes, 1, 1},
    {"msofhours", F_msofhours, 1, 1}, {"seconds", F_seconds, 1, 1},
    {"minutes", F_minutes, 1, 1}, {"hours", F_hours, 1, 1},
    {"sum", F_sum, 1, 1},
    {"runiform", F_runiform, 0, 2}, {"runiformint", F_runiformint, 2, 2},
    // Oracle tests/eval/golden/aer_w2/w2c_functions.log (StataBE 19.5, 2026-09-23),
    // data arms rnormal_dist / uniform_dist: rc 0, storage float, 200 draws. uniform()
    // is Stata's old spelling of runiform() and takes NO arguments (uniform(1,2) is
    // r(198), measured); rnormal() takes 0-2 (a third is r(198)).
    {"rnormal", F_rnormal, 0, 2}, {"uniform", F_uniform, 0, 0},
    // Same oracle, arms normal_data_hi/_lo, invnormal_data_tail/_ctl, ttail_data,
    // invttail_data, invt_data, sign_data, group_data.
    {"normal", F_normal, 1, 1}, {"invnormal", F_invnormal, 1, 1},
    {"ttail", F_ttail, 2, 2}, {"invttail", F_invttail, 2, 2}, {"invt", F_invt, 2, 2},
    {"sign", F_sign, 1, 1}, {"group", F_group, 1, 1},
    // normalden(x), normalden(x,s), normalden(x,m,s) (BUG-1124: generate refused
    // it, `generate d = normalden(x)` rc 1). Four arguments and none are r(198);
    // the bits are core/distributions.h stata_normalden's.
    {"normalden", F_normalden, 1, 3},
    // [FN] invlogit(x) = exp(x)/(1+exp(x)), range (0,1); logit(x) = ln(x/(1-x)),
    // missing outside the open interval (0,1). Oracle w2c_ml_getmata.do:21.
    {"invlogit", F_invlogit, 1, 1}, {"logit", F_logit, 1, 1},
    // [FN] reldif(x,y) = |x-y|/(|y|+1). StataBE 19.5 (2026-11-03): reldif(x,2) of
    // 1,2,3 is .33333333, 0, .33333333; reldif(.a,.a) 0, reldif(.,.a) `.`.
    {"reldif", F_reldif, 2, 2},
};

// invlogit/logit, shared by the scalar ev() and the vectorised ev_fn_vec().
// Two-branched with Stata's exp() (core/stata_arith.h): invlogit(-717.5) is 0,
// not the subnormal libm's exp(-717.5)/(1+...) gives.
inline double stata_invlogit(double x) {
    if (std::isnan(x)) return NAd;
    return cecon::stata_invlogit_value(x);
}
inline double stata_logit(double x) {
    if (!(x > 0.0 && x < 1.0)) return NAd;
    return std::log(x / (1.0 - x));
}

// reldif(x,y), shared by the scalar ev() and the vectorised ev_fn_vec(), and the
// rule PIET's display-layer fnReldif keeps: two missings are 0 when they are the
// same one (reldif(.a,.a), reldif(1e308,1e308)), any other missing operand is
// `.`. A value past c(maxdouble), such as the literal 1e308, is a missing too.
inline double stata_reldif(double x, double y) {
    constexpr double kMaxDouble = 0x1.fffffffffffffp+1022;
    const bool mx = std::isnan(x) || x > kMaxDouble, my = std::isnan(y) || y > kMaxDouble;
    if (mx || my) {
        if (!(mx && my)) return NAd;
        const bool same = std::isnan(x)
                              ? std::isnan(y) && cecon::missing_kind(x) == cecon::missing_kind(y)
                              : x == y;
        return same ? 0.0 : NAd;
    }
    return fin(std::fabs(x - y) / (std::fabs(y) + 1.0));
}

// One switch, two callers. frame.cpp's documented way of going wrong is a name
// that reaches FNS[] and the scalar ev() but not the vectorised ev_fn_vec(),
// because that combination returns a silent column of missings with no error and
// a one-row unit test still passes. Writing the mapping once, and the case-label
// list once (CECON_TIME_UNARY_CASES below), removes the opportunity.
inline double stata_time_unary(int fn, double x) {
    switch (fn) {
        case F_mofd: return cecon::stata_scale_of_day(x, cecon::DS_month);
        case F_qofd: return cecon::stata_scale_of_day(x, cecon::DS_quarter);
        case F_hofd: return cecon::stata_scale_of_day(x, cecon::DS_half);
        case F_yofd: return cecon::stata_scale_of_day(x, cecon::DS_year);
        case F_wofd: return cecon::stata_scale_of_day(x, cecon::DS_week);
        case F_week: return cecon::stata_week_of_day(x);
        case F_dofm: return cecon::stata_day_of_scale(x, cecon::DS_month);
        case F_dofq: return cecon::stata_day_of_scale(x, cecon::DS_quarter);
        case F_dofh: return cecon::stata_day_of_scale(x, cecon::DS_half);
        case F_dofy: return cecon::stata_day_of_scale(x, cecon::DS_year);
        case F_dofw: return cecon::stata_day_of_scale(x, cecon::DS_week);
        case F_cofd: return cecon::stata_cofd(x);
        case F_Cofd: return cecon::stata_Cofd(x);
        case F_dofc: return cecon::stata_dofc(x);
        case F_dofC: return cecon::stata_dofC(x);
        case F_hh:   return cecon::stata_clock_part(x, cecon::CLK_hour, false);
        case F_mm:   return cecon::stata_clock_part(x, cecon::CLK_minute, false);
        case F_ss:   return cecon::stata_clock_part(x, cecon::CLK_second, false);
        case F_hhC:  return cecon::stata_clock_part(x, cecon::CLK_hour, true);
        case F_mmC:  return cecon::stata_clock_part(x, cecon::CLK_minute, true);
        case F_ssC:  return cecon::stata_clock_part(x, cecon::CLK_second, true);
        case F_msofseconds: return cecon::stata_msofseconds(x);
        case F_msofminutes: return cecon::stata_msofminutes(x);
        case F_msofhours:   return cecon::stata_msofhours(x);
        case F_seconds:     return cecon::stata_seconds(x);
        case F_minutes:     return cecon::stata_minutes(x);
        case F_hours:       return cecon::stata_hours(x);
    }
    return NAd;
}

// sign(x): -1, 0 or 1; every missing (.a included, measured) answers `.`; sign(-0) is 0.
inline double stata_sign(double x) {
    if (std::isnan(x)) return NAd;
    return x > 0.0 ? 1.0 : (x < 0.0 ? -1.0 : 0.0);
}

// A distribution function's NaN (out of domain, or a missing argument) is Stata's `.`.
inline double stata_dist(double v) { return std::isnan(v) ? NAd : v; }

// group(k): splits the observations into k consecutive, nearly equal groups. It is
// absent from Stata 19's [FN] manual but still runs, so there is no written rule;
// the one below is read off StataBE 19.5 (2026-09-29, 78 (N, k) cells, N = 1..13,
// k = 1..5 and 2.5, plus by-groups and tiny k): observation n of N gets
//     1                                     if n == 1
//     int(k (n-1)/(N-1) + 1 - 1e-6)         otherwise,
// where the 1e-6 is Stata's own fuzz (k = 1 + 9.9e-7 on N = 2 answers 1, 1 + 1e-6
// answers 2; k = 1e-9 gives 1 0 0 0 ...). n and N are the observation number and
// count of the by-group under `by`, of the dataset otherwise; an `if` does not
// renumber (group(2) if _n > 2 on N = 6 answers 1 2 2 2 on rows 3-6; w2m_bymask
// G02 :482). k <= 0 or missing is `.`. Under `in a/b` Stata renumbers within the
// range: n = obs-a+1, N = b-a+1 (w2m_bymask G05 :514, `group(2) in 3/6` answers
// . . 1 1 2 2). The range reaches the engine apart from the `if`
// (GenQualifier, eval --gen-in) and bind_group_range stamps it on every
// group() node of the command. Inside sum() the same n and N apply
// (materialize_sums evaluates its argument per group).
inline double stata_group(double k, int64_t n, int64_t N) {
    if (std::isnan(k) || !(k > 0.0)) return NAd;
    if (n <= 1 || N <= 1) return 1.0;
    // BUG-577: the fuzz is folded as ONE constant, (1 - 1e-6). Written as
    // `+ 1.0 - 1e-6` it rounds twice, and group(1e-6) at n = N = 2 gave
    // floor(0.9999999999999999) = 0 where StataBE answers 1. The folded form
    // reproduces all 630 captured StataBE cells (N x k grid, tiny k, k near 1 and 1e6).
    return std::floor(k * static_cast<double>(n - 1) / static_cast<double>(N - 1) +
                      (1.0 - 1e-6));
}

#define CECON_TIME_UNARY_CASES                                                  \
    case F_mofd: case F_qofd: case F_hofd: case F_yofd: case F_wofd:            \
    case F_week: case F_dofm: case F_dofq: case F_dofh: case F_dofy:            \
    case F_dofw: case F_cofd: case F_Cofd: case F_dofc: case F_dofC:            \
    case F_hh: case F_mm: case F_ss: case F_hhC: case F_mmC: case F_ssC:        \
    case F_msofseconds: case F_msofminutes: case F_msofhours:                   \
    case F_seconds: case F_minutes: case F_hours
const FnInfo* find_fn(const std::string& s) {
    for (const FnInfo& f : FNS) if (s == f.name) return &f;
    return nullptr;
}

struct Node {
    enum K { Num, Var, ObsN, ObsBigN, Subscript, Neg, Not, Add, Sub, Mul, Div, Pow,
             Lt, Gt, Le, Ge, Eq, Ne, And, Or, Fn, Str } kind;
    double num = 0;
    const double* var = nullptr;
    int fn = 0;
    std::unique_ptr<Node> a, b;               // unary/binary children; Subscript: a=Var, b=index
    std::vector<std::unique_ptr<Node>> args;  // Fn arguments
    // String/factor equality support: a `Str` node carries a string literal; a
    // factor-column reference is a Var node with fcol set and var still null
    // until cmp() resolves `factorvar ==/!= "literal"` into an ordinary numeric
    // compare against the level code (materialised into owned). Any other use
    // of a factor/string operand hits ev()'s null/Str guard.
    std::string sval;
    const FrameColumn* fcol = nullptr;
    std::vector<double> owned;
    // group() under an `in a/b` range: the range's first and last observation
    // (1-based), 0 when there is none. Set by bind_group_range.
    int64_t in_first = 0, in_last = 0;
    // A function node one of whose arguments takes a draw: ev() then evaluates
    // every argument up front, right to left, as Stata does. Set by mark_arg_draws.
    bool arg_draws = false;
};

// Per-row evaluation context. order != null means we are inside a bysort group:
// _n/_N and x[k] resolve within [g0,g1) of *order in sort order.
struct EvalCtx {
    int64_t row = 0;
    const std::vector<int64_t>* order = nullptr;
    int64_t g0 = 0, g1 = 0, pos = 0;
    // BUG-364: dataset row count, for the NO-bysort case. Stata defines `_n` as
    // the observation number in the current sort order and `_N` as the number of
    // observations, both of which exist with or without a `by` prefix; this is
    // what makes them meaningful outside one.
    int64_t n_all = 0;
};

double ev(const Node* n, const EvalCtx& c) {
    switch (n->kind) {
        case Node::Num: return n->num;
        case Node::Var:
            if (!n->var) throw std::runtime_error(
                "expr: string/factor variable used where a number is required");
            return n->var[c.row];
        case Node::Str:
            throw std::runtime_error("expr: string literal used where a number is required");
        // BUG-364: outside a bysort group these resolve against the DATASET, which
        // is what Stata does — `keep if _n<=100`, `gen first = x[1]`, `di _N` are
        // ordinary unprefixed forms. They used to throw "requires a bysort context",
        // an honest error for a construct Stata accepts, and no gate covered the
        // plain form (the corpus's only `_n`-in-filter case was the bysort variant).
        // In-group behaviour is untouched.
        case Node::ObsN:
            return static_cast<double>(c.order ? c.pos : c.row + 1);
        case Node::ObsBigN:
            return static_cast<double>(c.order ? (c.g1 - c.g0) : c.n_all);
        case Node::Subscript: {
            double kd = ev(n->b.get(), c);
            if (std::isnan(kd)) return NAd;
            int64_t k = static_cast<int64_t>(kd);  // truncate toward zero
            if (!c.order) {
                if (k < 1 || k > c.n_all) return NAd;   // out of range = missing, as in Stata
                return n->a->var[k - 1];
            }
            if (k < 1 || k > (c.g1 - c.g0)) return NAd;
            return n->a->var[(*c.order)[c.g0 + k - 1]];
        }
        case Node::Neg: return -ev(n->a.get(), c);
        case Node::Not: return truthy(ev(n->a.get(), c)) ? 0.0 : 1.0;
        // Operands are evaluated left then right, explicitly: C++ leaves the order
        // of `f() + g()` unspecified, and with a draw on each side the order IS the
        // answer (Stata: `runiform() - 2*runiform()` takes the left draw first,
        // impl_C stata_probe P5).
        case Node::Add: { const double x = ev(n->a.get(), c); return fin(x + ev(n->b.get(), c)); }
        case Node::Sub: { const double x = ev(n->a.get(), c); return fin(x - ev(n->b.get(), c)); }
        case Node::Mul: { const double x = ev(n->a.get(), c); return fin(x * ev(n->b.get(), c)); }
        case Node::Div: { const double x = ev(n->a.get(), c); return fin(x / ev(n->b.get(), c)); }
        // `^` is Stata's (core/stata_arith.h): x^2 is x*x, x^-2 is (1/x)*(1/x), and a
        // non-integer power is exp(y*ln(x)); libm's pow differs in the last bit.
        case Node::Pow: { const double x = ev(n->a.get(), c); return fin(cecon::stata_pow_value(x, ev(n->b.get(), c))); }
        case Node::Lt: { const double x = ev(n->a.get(), c); return scmp(x, ev(n->b.get(), c)) <  0 ? 1.0 : 0.0; }
        case Node::Gt: { const double x = ev(n->a.get(), c); return scmp(x, ev(n->b.get(), c)) >  0 ? 1.0 : 0.0; }
        case Node::Le: { const double x = ev(n->a.get(), c); return scmp(x, ev(n->b.get(), c)) <= 0 ? 1.0 : 0.0; }
        case Node::Ge: { const double x = ev(n->a.get(), c); return scmp(x, ev(n->b.get(), c)) >= 0 ? 1.0 : 0.0; }
        case Node::Eq: { const double x = ev(n->a.get(), c); return scmp(x, ev(n->b.get(), c)) == 0 ? 1.0 : 0.0; }
        case Node::Ne: { const double x = ev(n->a.get(), c); return scmp(x, ev(n->b.get(), c)) != 0 ? 1.0 : 0.0; }
        case Node::And: { double x = ev(n->a.get(), c), y = ev(n->b.get(), c);
            return (truthy(x) && truthy(y)) ? 1.0 : 0.0; }
        case Node::Or: { double x = ev(n->a.get(), c), y = ev(n->b.get(), c);
            return (truthy(x) || truthy(y)) ? 1.0 : 0.0; }
        case Node::Fn: {
            const auto& A = n->args;
            // Stata evaluates a function's arguments right to left, every one of
            // them, cond()'s untaken branch included. Only a draw can tell (StataBE
            // 19.5, 2026-11-03, after `set seed 1`: `min(runiform(), runiform()+10)`
            // and `cond(1, runiform(), runiform()+10)` are both the SECOND draw, and
            // a row of `cond(mod(_n,2)==1, 0, runiform())` takes a draw whichever
            // branch wins, BUG-1050), so when an argument takes one they are all
            // evaluated here first, in that order, and the cases read them back.
            double pre_small[4];
            std::vector<double> pre_big;
            const double* pre = nullptr;
            if (n->arg_draws) {
                double* buf = pre_small;
                if (A.size() > 4) { pre_big.resize(A.size()); buf = pre_big.data(); }
                for (size_t k = A.size(); k-- > 0;) buf[k] = ev(A[k].get(), c);
                pre = buf;
            }
            auto arg = [&](size_t k) { return pre ? pre[k] : ev(A[k].get(), c); };
            auto a0 = [&] { return arg(0); };
            switch (n->fn) {
                case F_ln: case F_log: return fin(std::log(a0()));
                case F_exp: return stata_exp(a0());
                case F_sqrt: return fin(std::sqrt(a0()));
                case F_abs: { double x = a0(); return std::isnan(x) ? NAd : std::fabs(x); }
                case F_sin: return std::sin(a0());
                case F_cos: return std::cos(a0());
                case F_tan: return std::tan(a0());
                case F_log10: return stata_log10(a0());
                case F_asinh: return stata_asinh(a0());
                case F_ln1p: return fin(std::log1p(a0()));
                case F_ln1m: return fin(std::log1p(-a0()));
                // floor(), ceil(), int() and round() of a missing x are x itself, so
                // `.a` stays `.a` ([FN]: "x (not '.') if x is missing"), while abs()
                // of any missing is plain `.` (BUG-1371). round() is Stata's
                // y*floor(x/y + .5), cecon::stata_round_value (BUG-1364).
                case F_int: { double x = a0(); return std::isnan(x) ? x : std::trunc(x); }
                case F_float: return frame_detail::round_to_store(a0(), StoreType::Float);
                case F_round: {
                    double x = a0();
                    if (A.size() >= 2) {  // round(x, unit): a missing unit is `.`
                        double u = arg(1);
                        if (std::isnan(u)) return NAd;
                        return std::isnan(x) ? x : fin(cecon::stata_round_value(x, u));
                    }
                    return std::isnan(x) ? x : cecon::stata_round_value(x, 1.0);
                }
                case F_floor: { double x = a0(); return std::isnan(x) ? x : std::floor(x); }
                case F_ceil: { double x = a0(); return std::isnan(x) ? x : std::ceil(x); }
                case F_missing:
                    for (size_t k = 0; k < A.size(); ++k) if (std::isnan(arg(k))) return 1.0;
                    return 0.0;
                case F_cond: {
                    double cc = arg(0);
                    if (std::isnan(cc)) return A.size() == 4 ? arg(3) : arg(1);
                    return cc != 0.0 ? arg(1) : arg(2);
                }
                case F_inlist: {
                    double z = arg(0);
                    for (size_t k = 1; k < A.size(); ++k) {
                        double v = arg(k);
                        if ((std::isnan(z) && std::isnan(v)) || z == v) return 1.0;
                    }
                    return 0.0;
                }
                case F_inrange: {
                    double z = arg(0), lo = arg(1), hi = arg(2);
                    if (std::isnan(z)) return 0.0;
                    bool lm = std::isnan(lo), hm = std::isnan(hi);
                    if (lm && hm) return 1.0;
                    if (lm) return z <= hi ? 1.0 : 0.0;
                    if (hm) return z >= lo ? 1.0 : 0.0;
                    return (lo <= z && z <= hi) ? 1.0 : 0.0;
                }
                case F_mod: {
                    double x = a0(), y = arg(1);
                    if (std::isnan(x) || std::isnan(y) || y <= 0.0) return NAd;
                    return fin(x - y * std::floor(x / y));
                }
                case F_mdy:
                    return stata_mdy(a0(), arg(1), arg(2));
                case F_year: return stata_date_part(a0(), DP_year);
                case F_month: return stata_date_part(a0(), DP_month);
                case F_day: return stata_date_part(a0(), DP_day);
                case F_dow: return stata_date_part(a0(), DP_dow);
                case F_doy: return stata_date_part(a0(), DP_doy);
                case F_quarter: return stata_date_part(a0(), DP_quarter);
                case F_halfyear: return stata_date_part(a0(), DP_half);
                CECON_TIME_UNARY_CASES: return stata_time_unary(n->fn, a0());
                case F_sum:
                    // Filled by materialize_sums before evaluation starts.
                    return n->owned.empty() ? NAd : n->owned[static_cast<size_t>(c.row)];
                case F_runiform: {
                    if (g_mt64_on) {
                        // Stata: the bounds first; a degenerate or inverted range is
                        // `.` WITHOUT a draw (impl_C stata_probe P1), else a+(b-a)u (P2).
                        if (A.empty()) return mt64_uniform();
                        const double lo = a0(), hi = arg(1);
                        if (std::isnan(lo) || std::isnan(hi) || !(lo < hi)) return NAd;
                        return lo + (hi - lo) * mt64_uniform();
                    }
                    const double u = cecon::rng_uniform(g_rng_seed, g_rng_stream,
                                                        static_cast<int64_t>(n->num), c.row);
                    if (A.empty()) return u;
                    const double lo = a0(), hi = arg(1);
                    // Stata answers missing for a degenerate or inverted range
                    // (runiform(3,3) is `.`, measured).
                    if (std::isnan(lo) || std::isnan(hi) || !(lo < hi)) return NAd;
                    return lo + u * (hi - lo);
                }
                case F_runiformint: {
                    const double lo = a0(), hi = arg(1);
                    if (std::isnan(lo) || std::isnan(hi) || hi < lo) return NAd;
                    if (lo != std::floor(lo) || hi != std::floor(hi)) return NAd;
                    if (g_mt64_on) {
                        // Top-k-bit rejection, one raw draw per try (core/rng_mt64.h).
                        return g_mt64.runiformint(lo, hi);
                    }
                    return cecon::rng_uniform_int(g_rng_seed, g_rng_stream,
                                                  static_cast<int64_t>(n->num), c.row, lo, hi);
                }
                case F_uniform:
                    if (g_mt64_on) return mt64_uniform();  // Stata: runiform()'s stream and transform
                    return cecon::rng_uniform(g_rng_seed, g_rng_stream,
                                              static_cast<int64_t>(n->num), c.row);
                case F_rnormal: {
                    // Inverse CDF (AS241) over the same 53-bit word runiform() uses,
                    // centred into the open interval so the quantile is always finite.
                    // On the Stata stream this is ONE runiform() through the inverse
                    // CDF: Stata's own rnormal() is a Knuth Algorithm M variant that
                    // takes 3+ uniforms per normal and whose tables are unpublished
                    // (internal g13 report 1.1), so its values are not Stata's yet.
                    double u;
                    if (g_mt64_on) {
                        u = mt64_uniform();
                    } else {
                        const uint64_t bits = cecon::uniform_bits(g_rng_seed, g_rng_stream,
                                                                  static_cast<int64_t>(n->num), c.row);
                        u = (static_cast<double>(bits >> 11) + 0.5) * (1.0 / 9007199254740992.0);
                    }
                    const double z = cecon::normal_icdf(u);
                    if (A.empty()) return z;
                    const double m = a0();
                    const double sd = A.size() >= 2 ? arg(1) : 1.0;
                    // rnormal(.,1), rnormal(0,-1) are `.`; rnormal(1,0) is 1 (measured).
                    if (std::isnan(m) || std::isnan(sd) || sd < 0.0) return NAd;
                    return fin(m + sd * z);
                }
                case F_normal: return stata_dist(cecon::stata_normal(a0()));
                case F_invnormal: return stata_dist(cecon::stata_invnormal(a0()));
                case F_ttail: return stata_dist(cecon::stata_ttail(a0(), arg(1)));
                case F_invttail: return stata_dist(cecon::stata_invttail(a0(), arg(1)));
                case F_invt: return stata_dist(cecon::stata_invt(a0(), arg(1)));
                case F_normalden: {
                    const double x = a0();
                    if (A.size() == 1) return stata_dist(cecon::stata_normalden(x));
                    if (A.size() == 2) return stata_dist(cecon::stata_normalden(x, 0.0, arg(1)));
                    const double m = arg(1);
                    return stata_dist(cecon::stata_normalden(x, m, arg(2)));
                }
                case F_sign: return stata_sign(a0());
                case F_invlogit: return stata_invlogit(a0());
                case F_logit: return stata_logit(a0());
                case F_reldif: return stata_reldif(a0(), ev(A[1].get(), c));
                case F_group:
                    // An `in` range comes only without a by-list or sort key
                    // (Frame::bygen refuses the rest), so c.row is the observation.
                    if (n->in_first > 0)
                        return stata_group(a0(), c.row + 1 - n->in_first + 1,
                                           n->in_last - n->in_first + 1);
                    return stata_group(a0(), c.order ? c.pos : c.row + 1,
                                       c.order ? (c.g1 - c.g0) : c.n_all);
                case F_ym: return cecon::stata_ym(a0(), arg(1));
                case F_yq: return cecon::stata_yq(a0(), arg(1));
                case F_yh: return cecon::stata_yh(a0(), arg(1));
                case F_yw: return cecon::stata_yw(a0(), arg(1));
                case F_hms:
                    return cecon::stata_hms(a0(), arg(1), arg(2));
                case F_dhms:
                    return cecon::stata_dhms(a0(), arg(1), arg(2), arg(3));
                case F_min: { double m = NAd; for (size_t k = 0; k < A.size(); ++k) { double v = arg(k);
                    if (!std::isnan(v)) m = std::isnan(m) ? v : std::min(m, v); } return m; }
                case F_max: { double m = NAd; for (size_t k = 0; k < A.size(); ++k) { double v = arg(k);
                    if (!std::isnan(v)) m = std::isnan(m) ? v : std::max(m, v); } return m; }
            }
            return NAd;
        }
    }
    return NAd;
}

// ---------- Vectorized (column-at-a-time) evaluator ----------
// Byte-identical to the per-row ev() above: every element replays the exact same
// scalar operation, in the same nesting order, through the same fin()/mi() funnels —
// only the loop structure changes. The tree is walked ONCE (each subexpression
// materialised as a length-N column) instead of re-dispatching the whole AST n_ times,
// so the tight per-node loops autovectorise and shed the per-row switch/pointer-chase
// overhead. Used only for order-free contexts (generate / keepif / egen_by inner-expr);
// the bysort path (bygen, _n/_N/subscript) keeps ev().
//
// vectorizable() gates entry: cond() evaluates only the taken branch per row, so forcing
// an unselected branch column-wide could raise where the scalar never would — fall back
// to ev() for any tree containing cond() or a bysort-only node (_n/_N/subscript). Every
// other operator/function evaluates all its operands unconditionally in ev() too, so
// materialising them column-wide changes nothing observable.
bool vectorizable(const Node* n) {
    switch (n->kind) {
        case Node::ObsN: case Node::ObsBigN: case Node::Subscript: return false;
        case Node::Fn:
            // A draw needs its ROW index, which only the scalar evaluator carries.
            if (n->fn == F_cond || n->fn == F_sum || n->fn == F_runiform ||
                n->fn == F_runiformint || n->fn == F_rnormal || n->fn == F_uniform ||
                n->fn == F_group) return false;
            break;
        default: break;
    }
    if (n->a && !vectorizable(n->a.get())) return false;
    if (n->b && !vectorizable(n->b.get())) return false;
    for (const auto& q : n->args) if (!vectorizable(q.get())) return false;
    return true;
}

// group() under `in a/b` numbers within the range (w2m_bymask G01 :472, G03 :494,
// G04 :504 inside sum(), G05 :514). The range is its own channel
// (GenQualifier), so it can be told apart from an `if` such as
// `_n>=2 & _n<=4`, which does not renumber (G02 :482). Every group() node of the
// command takes it, the `if`'s included: they are evaluated over the same range.
void bind_group_range(Node* n, int64_t first, int64_t last) {
    if (!n || first <= 0) return;
    if (n->kind == Node::Fn && n->fn == F_group) { n->in_first = first; n->in_last = last; }
    bind_group_range(n->a.get(), first, last);
    bind_group_range(n->b.get(), first, last);
    for (auto& q : n->args) bind_group_range(q.get(), first, last);
}

void ev_vec(const Node* n, int64_t N, std::vector<double>& out);

void ev_fn_vec(const Node* n, int64_t N, std::vector<double>& out) {
    const auto& A = n->args;
    // Single-argument math functions: evaluate arg0 into out, transform in place.
    switch (n->fn) {
        case F_ln: case F_log: ev_vec(A[0].get(), N, out);
            for (int64_t i = 0; i < N; ++i) out[i] = fin(std::log(out[i])); return;
        case F_exp: ev_vec(A[0].get(), N, out);
            for (int64_t i = 0; i < N; ++i) out[i] = stata_exp(out[i]); return;
        case F_sqrt: ev_vec(A[0].get(), N, out);
            for (int64_t i = 0; i < N; ++i) out[i] = fin(std::sqrt(out[i])); return;
        case F_abs: ev_vec(A[0].get(), N, out);
            for (int64_t i = 0; i < N; ++i) out[i] = std::isnan(out[i]) ? NAd : std::fabs(out[i]); return;
        case F_sin: ev_vec(A[0].get(), N, out);
            for (int64_t i = 0; i < N; ++i) out[i] = std::sin(out[i]); return;
        case F_cos: ev_vec(A[0].get(), N, out);
            for (int64_t i = 0; i < N; ++i) out[i] = std::cos(out[i]); return;
        case F_tan: ev_vec(A[0].get(), N, out);
            for (int64_t i = 0; i < N; ++i) out[i] = std::tan(out[i]); return;
        case F_log10: ev_vec(A[0].get(), N, out);
            for (int64_t i = 0; i < N; ++i) out[i] = stata_log10(out[i]); return;
        case F_asinh: ev_vec(A[0].get(), N, out);
            for (int64_t i = 0; i < N; ++i) out[i] = stata_asinh(out[i]); return;
        case F_ln1p: ev_vec(A[0].get(), N, out);
            for (int64_t i = 0; i < N; ++i) out[i] = fin(std::log1p(out[i])); return;
        case F_ln1m: ev_vec(A[0].get(), N, out);
            for (int64_t i = 0; i < N; ++i) out[i] = fin(std::log1p(-out[i])); return;
        case F_int: ev_vec(A[0].get(), N, out);
            for (int64_t i = 0; i < N; ++i) { double x = out[i]; out[i] = std::isnan(x) ? x : std::trunc(x); } return;
        case F_float: ev_vec(A[0].get(), N, out);
            for (int64_t i = 0; i < N; ++i) out[i] = frame_detail::round_to_store(out[i], StoreType::Float); return;
        case F_floor: ev_vec(A[0].get(), N, out);
            for (int64_t i = 0; i < N; ++i) { double x = out[i]; out[i] = std::isnan(x) ? x : std::floor(x); } return;
        case F_ceil: ev_vec(A[0].get(), N, out);
            for (int64_t i = 0; i < N; ++i) { double x = out[i]; out[i] = std::isnan(x) ? x : std::ceil(x); } return;
        // BUG-371 date extractors: 1-arg, same shape as the math functions above.
        case F_year: case F_month: case F_day: case F_dow:
        case F_doy: case F_quarter: case F_halfyear: {
            DatePart part = DP_year;
            switch (n->fn) {
                case F_month: part = DP_month; break;
                case F_day: part = DP_day; break;
                case F_dow: part = DP_dow; break;
                case F_doy: part = DP_doy; break;
                case F_quarter: part = DP_quarter; break;
                case F_halfyear: part = DP_half; break;
                default: break;
            }
            ev_vec(A[0].get(), N, out);
            for (int64_t i = 0; i < N; ++i) out[i] = stata_date_part(out[i], part);
            return;
        }
        // B3 time scales: 1-arg, same shape as the extractors above and driven by
        // the SAME label list and the SAME mapping function as the scalar path.
        CECON_TIME_UNARY_CASES: {
            ev_vec(A[0].get(), N, out);
            for (int64_t i = 0; i < N; ++i) out[i] = stata_time_unary(n->fn, out[i]);
            return;
        }
        case F_normal: ev_vec(A[0].get(), N, out);
            for (int64_t i = 0; i < N; ++i) out[i] = stata_dist(cecon::stata_normal(out[i])); return;
        case F_invnormal: ev_vec(A[0].get(), N, out);
            for (int64_t i = 0; i < N; ++i) out[i] = stata_dist(cecon::stata_invnormal(out[i])); return;
        case F_sign: ev_vec(A[0].get(), N, out);
            for (int64_t i = 0; i < N; ++i) out[i] = stata_sign(out[i]); return;
        case F_invlogit: ev_vec(A[0].get(), N, out);
            for (int64_t i = 0; i < N; ++i) out[i] = stata_invlogit(out[i]); return;
        case F_logit: ev_vec(A[0].get(), N, out);
            for (int64_t i = 0; i < N; ++i) out[i] = stata_logit(out[i]); return;
        case F_round:
            if (A.size() < 2) {  // 1-arg round(x): a missing x keeps its kind
                ev_vec(A[0].get(), N, out);
                for (int64_t i = 0; i < N; ++i) { double x = out[i]; out[i] = std::isnan(x) ? x : cecon::stata_round_value(x, 1.0); }
                return;
            }
            break;  // 2-arg round(x,u) handled in the multi-arg section below
        default: break;
    }
    // Multi-arg / n-ary functions: materialise every argument as a column, then combine.
    std::vector<std::vector<double>> V(A.size());
    for (size_t k = 0; k < A.size(); ++k) ev_vec(A[k].get(), N, V[k]);
    out.resize(static_cast<size_t>(N));
    switch (n->fn) {
        case F_round:  // round(x, unit): a missing unit is `.`, a missing x keeps its kind
            for (int64_t i = 0; i < N; ++i) {
                double x = V[0][i];
                double u = V[1][i]; if (std::isnan(u)) { out[i] = NAd; continue; }
                out[i] = std::isnan(x) ? x : fin(cecon::stata_round_value(x, u));
            }
            return;
        case F_mdy:  // BUG-371 — same validation as the scalar path, one shared helper
            for (int64_t i = 0; i < N; ++i) out[i] = stata_mdy(V[0][i], V[1][i], V[2][i]);
            return;
        case F_ym:
            for (int64_t i = 0; i < N; ++i) out[i] = cecon::stata_ym(V[0][i], V[1][i]);
            return;
        case F_yq:
            for (int64_t i = 0; i < N; ++i) out[i] = cecon::stata_yq(V[0][i], V[1][i]);
            return;
        case F_yh:
            for (int64_t i = 0; i < N; ++i) out[i] = cecon::stata_yh(V[0][i], V[1][i]);
            return;
        case F_yw:
            for (int64_t i = 0; i < N; ++i) out[i] = cecon::stata_yw(V[0][i], V[1][i]);
            return;
        case F_hms:
            for (int64_t i = 0; i < N; ++i)
                out[i] = cecon::stata_hms(V[0][i], V[1][i], V[2][i]);
            return;
        case F_dhms:
            for (int64_t i = 0; i < N; ++i)
                out[i] = cecon::stata_dhms(V[0][i], V[1][i], V[2][i], V[3][i]);
            return;
        case F_ttail:
            for (int64_t i = 0; i < N; ++i) out[i] = stata_dist(cecon::stata_ttail(V[0][i], V[1][i]));
            return;
        case F_invttail:
            for (int64_t i = 0; i < N; ++i) out[i] = stata_dist(cecon::stata_invttail(V[0][i], V[1][i]));
            return;
        case F_invt:
            for (int64_t i = 0; i < N; ++i) out[i] = stata_dist(cecon::stata_invt(V[0][i], V[1][i]));
            return;
        case F_reldif:
            for (int64_t i = 0; i < N; ++i) out[i] = stata_reldif(V[0][i], V[1][i]);
            return;
        case F_normalden:
            for (int64_t i = 0; i < N; ++i) {
                const double m = V.size() == 3 ? V[1][i] : 0.0;
                const double sd = V.size() == 1 ? 1.0 : V.back()[i];
                out[i] = stata_dist(cecon::stata_normalden(V[0][i], m, sd));
            }
            return;
        case F_mod:  // floored modulus; y<=0 or any missing -> .
            for (int64_t i = 0; i < N; ++i) {
                double x = V[0][i], y = V[1][i];
                out[i] = (std::isnan(x) || std::isnan(y) || y <= 0.0)
                             ? NAd
                             : fin(x - y * std::floor(x / y));
            }
            return;
        case F_missing:
            for (int64_t i = 0; i < N; ++i) {
                double r = 0.0;
                for (const auto& v : V) if (std::isnan(v[i])) { r = 1.0; break; }
                out[i] = r;
            }
            return;
        case F_inlist:
            for (int64_t i = 0; i < N; ++i) {
                double z = V[0][i], r = 0.0;
                for (size_t k = 1; k < V.size(); ++k) {
                    double v = V[k][i];
                    if ((std::isnan(z) && std::isnan(v)) || z == v) { r = 1.0; break; }
                }
                out[i] = r;
            }
            return;
        case F_inrange:
            for (int64_t i = 0; i < N; ++i) {
                double z = V[0][i], lo = V[1][i], hi = V[2][i];
                if (std::isnan(z)) { out[i] = 0.0; continue; }
                bool lm = std::isnan(lo), hm = std::isnan(hi);
                if (lm && hm) { out[i] = 1.0; continue; }
                if (lm) { out[i] = z <= hi ? 1.0 : 0.0; continue; }
                if (hm) { out[i] = z >= lo ? 1.0 : 0.0; continue; }
                out[i] = (lo <= z && z <= hi) ? 1.0 : 0.0;
            }
            return;
        case F_min:
            for (int64_t i = 0; i < N; ++i) {
                double m = NAd;
                for (const auto& v : V) { double x = v[i]; if (!std::isnan(x)) m = std::isnan(m) ? x : std::min(m, x); }
                out[i] = m;
            }
            return;
        case F_max:
            for (int64_t i = 0; i < N; ++i) {
                double m = NAd;
                for (const auto& v : V) { double x = v[i]; if (!std::isnan(x)) m = std::isnan(m) ? x : std::max(m, x); }
                out[i] = m;
            }
            return;
        default: break;
    }
    // Unknown function id: ev() falls through to `return NAd;` — mirror that.
    std::fill(out.begin(), out.end(), NAd);
}

#undef CECON_TIME_UNARY_CASES

void ev_vec(const Node* n, int64_t N, std::vector<double>& out) {
    out.resize(static_cast<size_t>(N));
    switch (n->kind) {
        case Node::Num:
            std::fill(out.begin(), out.end(), n->num);
            return;
        case Node::Var:
            if (!n->var) throw std::runtime_error(
                "expr: string/factor variable used where a number is required");
            for (int64_t i = 0; i < N; ++i) out[i] = n->var[i];
            return;
        case Node::Str:
            throw std::runtime_error("expr: string literal used where a number is required");
        case Node::Neg:
            ev_vec(n->a.get(), N, out);
            for (int64_t i = 0; i < N; ++i) out[i] = -out[i];
            return;
        case Node::Not:
            ev_vec(n->a.get(), N, out);
            for (int64_t i = 0; i < N; ++i) out[i] = truthy(out[i]) ? 0.0 : 1.0;
            return;
        case Node::Add: case Node::Sub: case Node::Mul: case Node::Div: case Node::Pow:
        case Node::Lt: case Node::Gt: case Node::Le: case Node::Ge:
        case Node::Eq: case Node::Ne: case Node::And: case Node::Or: {
            ev_vec(n->a.get(), N, out);
            std::vector<double> rb;
            ev_vec(n->b.get(), N, rb);
            switch (n->kind) {
                case Node::Add: for (int64_t i = 0; i < N; ++i) out[i] = fin(out[i] + rb[i]); break;
                case Node::Sub: for (int64_t i = 0; i < N; ++i) out[i] = fin(out[i] - rb[i]); break;
                case Node::Mul: for (int64_t i = 0; i < N; ++i) out[i] = fin(out[i] * rb[i]); break;
                case Node::Div: for (int64_t i = 0; i < N; ++i) out[i] = fin(out[i] / rb[i]); break;
                case Node::Pow: for (int64_t i = 0; i < N; ++i) out[i] = fin(cecon::stata_pow_value(out[i], rb[i])); break;
                case Node::Lt: for (int64_t i = 0; i < N; ++i) out[i] = scmp(out[i], rb[i]) <  0 ? 1.0 : 0.0; break;
                case Node::Gt: for (int64_t i = 0; i < N; ++i) out[i] = scmp(out[i], rb[i]) >  0 ? 1.0 : 0.0; break;
                case Node::Le: for (int64_t i = 0; i < N; ++i) out[i] = scmp(out[i], rb[i]) <= 0 ? 1.0 : 0.0; break;
                case Node::Ge: for (int64_t i = 0; i < N; ++i) out[i] = scmp(out[i], rb[i]) >= 0 ? 1.0 : 0.0; break;
                case Node::Eq: for (int64_t i = 0; i < N; ++i) out[i] = scmp(out[i], rb[i]) == 0 ? 1.0 : 0.0; break;
                case Node::Ne: for (int64_t i = 0; i < N; ++i) out[i] = scmp(out[i], rb[i]) != 0 ? 1.0 : 0.0; break;
                case Node::And: for (int64_t i = 0; i < N; ++i)
                    out[i] = (truthy(out[i]) && truthy(rb[i])) ? 1.0 : 0.0; break;
                case Node::Or: for (int64_t i = 0; i < N; ++i)
                    out[i] = (truthy(out[i]) || truthy(rb[i])) ? 1.0 : 0.0; break;
                default: break;
            }
            return;
        }
        case Node::Fn:
            ev_fn_vec(n, N, out);
            return;
        default:
            // ObsN/ObsBigN/Subscript are excluded by vectorizable(); never reached here.
            throw std::runtime_error("expr: node not evaluable outside a bysort context");
    }
}

// Evaluate `ast` over all n rows in an order-free context, byte-identically to the
// per-row ev() loop but walking the tree once when the shape allows (vectorizable()).
// ---------- sum(): the one function whose answer depends on row ORDER --------
// Stata's sum(x) is a running total, so it is not a function of its arguments the
// way every other entry in FNS[] is -- row i's answer needs rows 0..i-1. It is
// materialised into the node BEFORE evaluation instead, once, in the order the
// caller is actually walking:
//   generate  natural row order, one running total
//   bysort    sort order, reset at every group boundary
// Semantics measured against StataBE (oracle keys sum.*): a missing input
// contributes 0 and the result is never missing -- sum() of a column whose first
// value is missing answers 0, not `.`.
// Under bysort the ARGUMENT is also evaluated in the group context (g0a/g1a/posa),
// so `_n`, `_N`, x[k] and group() inside sum() count within the by-group, as the
// same expression outside sum() does (BUG-576: `by g: gen t = sum(_n)` answered
// 1 3 6 4 9 15 instead of 1 3 6 1 3 6).
void materialize_sums(Node* n, int64_t N, const std::vector<int64_t>* order,
                      const std::vector<int64_t>* g0a, const std::vector<int64_t>* g1a,
                      const std::vector<int64_t>* posa, const std::vector<double>* mask);

// Whether an `if` qualifier's value selects its row. Stata's `if exp` keeps the
// rows where exp is nonzero, and a missing value is nonzero, so a missing
// condition SELECTS (w2m_bymask F03: `by gg: generate f03 = sum(x) if x[_n-1]`
// answers 20 60 120 | 10 40 90 -- the group's first row, whose x[_n-1] is
// missing, is in). The engine used to read a missing mask as false (BUG-837).
inline bool if_selects(double m) { return std::isnan(m) || m != 0.0; }

// The `in a/b` half of a generate/bygen selection (GenQualifier). The range
// is folded into the `if` mask as 0 outside it, so every mask consumer
// (materialize_sums, the output step) honours both; an absent mask selects every
// row inside the range.
void apply_in_range(std::vector<double>& mask, int64_t n, int64_t first, int64_t last) {
    if (first <= 0) return;
    if (mask.empty()) mask.assign(static_cast<size_t>(n), 1.0);
    for (int64_t i = 0; i < n; ++i)
        if (i + 1 < first || i + 1 > last) mask[static_cast<size_t>(i)] = 0.0;
}

std::vector<double> eval_column_node(const Node* ast, int64_t n);

void materialize_sums(Node* n, int64_t N, const std::vector<int64_t>* order,
                      const std::vector<int64_t>* g0a, const std::vector<int64_t>* g1a,
                      const std::vector<int64_t>* posa, const std::vector<double>* mask) {
    if (!n) return;
    // Depth first, so a nested sum(sum(x)) is already filled when the outer one runs.
    materialize_sums(n->a.get(), N, order, g0a, g1a, posa, mask);
    materialize_sums(n->b.get(), N, order, g0a, g1a, posa, mask);
    for (auto& q : n->args) materialize_sums(q.get(), N, order, g0a, g1a, posa, mask);
    if (n->kind != Node::Fn || n->fn != F_sum || n->args.empty()) return;

    std::vector<double> arg;
    if (order == nullptr || vectorizable(n->args[0].get())) {
        arg = eval_column_node(n->args[0].get(), N);
    } else {
        // Same per-row context Frame::bygen builds for the outer expression.
        arg.resize(static_cast<size_t>(N));
        EvalCtx c;
        c.n_all = N;
        c.order = order;
        for (int64_t i = 0; i < N; ++i) {
            const size_t r = static_cast<size_t>(i);
            c.row = i; c.g0 = (*g0a)[r]; c.g1 = (*g1a)[r]; c.pos = (*posa)[r];
            arg[r] = ev(n->args[0].get(), c);
        }
    }
    n->owned.assign(static_cast<size_t>(N), NAd);
    // A row the `if` qualifier excludes contributes NOTHING and gets no value --
    // Stata leaves it missing and carries the total across it untouched.
    auto included = [mask](int64_t row) {
        return mask == nullptr || if_selects((*mask)[static_cast<size_t>(row)]);
    };
    double acc = 0.0;
    if (order == nullptr) {
        for (int64_t i = 0; i < N; ++i) {
            if (!included(i)) continue;   // stays NAd
            const double v = arg[static_cast<size_t>(i)];
            if (!std::isnan(v)) acc = fin(acc + v);
            n->owned[static_cast<size_t>(i)] = acc;
        }
        return;
    }
    int64_t cur_g0 = -1;
    for (int64_t t = 0; t < N; ++t) {
        const int64_t row = (*order)[static_cast<size_t>(t)];
        const int64_t g0 = (*g0a)[static_cast<size_t>(row)];
        if (g0 != cur_g0) { cur_g0 = g0; acc = 0.0; }
        if (!included(row)) continue;   // stays NAd
        const double v = arg[static_cast<size_t>(row)];
        if (!std::isnan(v)) acc = fin(acc + v);
        n->owned[static_cast<size_t>(row)] = acc;
    }
}

// Stata's generate/replace runs observation by observation and stores each result
// before it evaluates the next one, so an expression that reads the target column at
// another observation sees the values already replaced: `replace c = c[_n-1] + c if
// _n > 1` is a running sum (1 3 6 10 ...) and `replace a = a[_n-1] + 2 in 2/6` steps
// by 2. Column evaluation reads a snapshot and answered 1 3 5 7 ... / 1 3 3 3 at rc 0
// (a silent wrong answer on the corpus' running-sum idiom). Only an explicit subscript
// of the target triggers the row-sequential path; every other expression keeps the
// column evaluator, whose result is identical when no row reads another row's target.
bool has_self_subscript(const Node* n, const double* target) {
    if (n == nullptr || target == nullptr) return false;
    if (n->kind == Node::Subscript && n->a && n->a->var == target) return true;
    if (has_self_subscript(n->a.get(), target) || has_self_subscript(n->b.get(), target))
        return true;
    for (const auto& q : n->args)
        if (has_self_subscript(q.get(), target)) return true;
    return false;
}

void rebind_var(Node* n, const double* from, const double* to) {
    if (n == nullptr) return;
    if (n->kind == Node::Var && n->var == from) n->var = to;
    rebind_var(n->a.get(), from, to);
    rebind_var(n->b.get(), from, to);
    for (auto& q : n->args) rebind_var(q.get(), from, to);
}

std::vector<double> eval_column_node(const Node* ast, int64_t n) {
    std::vector<double> out(static_cast<size_t>(n));
    if (vectorizable(ast)) {
        ev_vec(ast, n, out);
    } else {
        EvalCtx c;
        c.n_all = n;
        for (int64_t i = 0; i < n; ++i) { c.row = i; out[i] = ev(ast, c); }
    }
    return out;
}

// Whether the tree takes a draw anywhere (runiform/runiformint/rnormal/uniform).
bool has_draw(const Node* n) {
    if (!n) return false;
    if (n->kind == Node::Fn && (n->fn == F_runiform || n->fn == F_runiformint ||
                                n->fn == F_rnormal || n->fn == F_uniform))
        return true;
    if (has_draw(n->a.get()) || has_draw(n->b.get())) return true;
    for (const auto& q : n->args) if (has_draw(q.get())) return true;
    return false;
}

// Whether the tree calls function fn anywhere.
bool has_fn(const Node* n, int fn) {
    if (!n) return false;
    if (n->kind == Node::Fn && n->fn == fn) return true;
    if (has_fn(n->a.get(), fn) || has_fn(n->b.get(), fn)) return true;
    for (const auto& q : n->args) if (has_fn(q.get(), fn)) return true;
    return false;
}

// On Stata's stream a row's `if` is evaluated before its expression, and only a
// row it selects evaluates the expression; a row outside `in` evaluates neither
// (StataBE 19.5, 2026-11-03: `gen x = runiform() if runiform() < .3 in 2/4`
// hands row 2's `if` the first draw and its value the second, and leaves rows 1
// and 5 undrawn). When both take draws the mask cannot be computed ahead of the
// rows, so it is evaluated inline, row by row. A running sum needs the whole
// mask before the rows and keeps the column mask.
bool inline_if_draws(const Node* mast, const Node* ast) {
    return g_mt64_on && mast && has_draw(mast) && has_draw(ast) && !has_fn(ast, F_sum);
}

// Sets Node::arg_draws on every function node one of whose arguments takes a
// draw when ev() evaluates it, and answers whether n's own evaluation does.
// sum()'s argument is drawn ahead of the rows by materialize_sums, so a sum()
// node draws nothing when ev() reaches it.
bool mark_arg_draws(Node* n) {
    if (!n) return false;
    const bool ab = mark_arg_draws(n->a.get());
    const bool bb = mark_arg_draws(n->b.get());
    bool args = false;
    for (auto& q : n->args) args = mark_arg_draws(q.get()) || args;
    if (n->kind != Node::Fn) return ab || bb;
    if (n->fn == F_sum) return false;
    n->arg_draws = args;
    return args || n->fn == F_runiform || n->fn == F_runiformint || n->fn == F_rnormal ||
           n->fn == F_uniform;
}

std::vector<double> eval_column(const Node* ast, int64_t n) {
    std::vector<double> out(static_cast<size_t>(n));
    if (vectorizable(ast)) {
        ev_vec(ast, n, out);
    } else {
        EvalCtx c;
        c.n_all = n;  // BUG-364: `_N` / x[k] resolve against the dataset here
        for (int64_t i = 0; i < n; ++i) { c.row = i; out[i] = ev(ast, c); }
    }
    return out;
}

struct Parser {
    const std::string s;
    size_t p = 0;
    const Frame& fr;
    // The text as Stata reads it (core/expr_blanks.h): `abs(1 2)` is abs(12).
    Parser(const std::string& src, const Frame& f) : s(squeeze_bracketed_blanks(src)), fr(f) {}

    void ws() { while (p < s.size() && std::isspace((unsigned char)s[p])) ++p; }
    bool eat(const char* tok) {
        ws();
        size_t n = std::strlen(tok);
        if (s.compare(p, n, tok) == 0) { p += n; return true; }
        return false;
    }
    [[noreturn]] void err(const std::string& m) {
        throw std::runtime_error("expr: " + m + " in '" + s + "'");
    }
    static std::unique_ptr<Node> mk(Node::K k) { auto n = std::make_unique<Node>(); n->kind = k; return n; }
    std::unique_ptr<Node> bin(Node::K k, std::unique_ptr<Node> a, std::unique_ptr<Node> b) {
        auto n = mk(k); n->a = std::move(a); n->b = std::move(b); return n;
    }

    int depth = 0;
    // RAII recursion-depth guard: every nesting level passes through neg() or unary(), so a cap there
    // bounds total recursion and rejects pathological input before the call stack overflows.
    struct DepthGuard { int& d; explicit DepthGuard(int& x) : d(x) { if (++d > 1000) throw std::runtime_error("expr: expression nested too deeply"); } ~DepthGuard() { --d; } };
    // Brackets that do not pair are Stata's r(132) "too few ')' or ']'" ahead of
    // any name the text holds: `generate g = nosuch + (1` and `nofn(1` (StataBE
    // 19.5; core/expr_blanks.h).
    std::unique_ptr<Node> parse() {
        if (const std::string u = bracket_imbalance(s); !u.empty()) err(u);
        auto n = orx();
        ws();
        if (p != s.size()) err("trailing input");
        mark_arg_draws(n.get());
        return n;
    }
    std::unique_ptr<Node> orx() {
        auto a = andx();
        while (eat("|")) a = bin(Node::Or, std::move(a), andx());
        return a;
    }
    std::unique_ptr<Node> andx() {
        auto a = cmp();
        while (eat("&")) a = bin(Node::And, std::move(a), cmp());
        return a;
    }
    // FU-844: each comparison operator is its own left-associative level, tightest
    // first != > < <= >= == (the order in help operators; oracle w2ac_expr_relational
    // gen_<o1>_<o2>, every ordered pair): `b==c>a` is b==(c>a), `1<2<3` is (1<2)<3.
    static constexpr int kCmpLevels = 6;
    // Eats the operator of comparison level `lvl` (0 = `==` ... 5 = `!=`); `<`/`>`
    // never eat the first char of `<=`/`>=`.
    bool eatCmp(int lvl) {
        ws();
        static const char* const kOp[kCmpLevels] = {"==", ">=", "<=", "<", ">", "!="};
        const char* op = kOp[lvl];
        size_t n = std::strlen(op);
        if (lvl == kCmpLevels - 1 && s.compare(p, 2, "~=") == 0) { p += 2; return true; }  // gen_tilde_ne_gt
        if (s.compare(p, n, op) != 0) return false;
        if (n == 1 && p + 1 < s.size() && s[p + 1] == '=') return false;
        p += n;
        return true;
    }
    std::unique_ptr<Node> cmp(int lvl = 0) {
        if (lvl == kCmpLevels) return add();
        auto a = cmp(lvl + 1);
        while (eatCmp(lvl)) {
            auto b = cmp(lvl + 1);
            switch (lvl) {
                case 0: a = eqne(Node::Eq, std::move(a), std::move(b)); break;
                case 1: a = bin(Node::Ge, std::move(a), std::move(b)); break;
                case 2: a = bin(Node::Le, std::move(a), std::move(b)); break;
                case 3: a = bin(Node::Lt, std::move(a), std::move(b)); break;
                case 4: a = bin(Node::Gt, std::move(a), std::move(b)); break;
                default: a = eqne(Node::Ne, std::move(a), std::move(b)); break;
            }
        }
        return a;
    }
    // Resolve `factorvar ==/!= "literal"` (either operand order) into a numeric
    // compare against the factor's 0-based level code. A literal that is not a
    // level makes == constant-false / != constant-true (no row can match).
    // Missing factor rows (code -1 -> NaN -> +inf under mi()) never equal a
    // finite level code, matching Stata's `"" == "x"` is false. A lone factor
    // or string operand, or a string/numeric mix, is a parse error.
    std::unique_ptr<Node> eqne(Node::K k, std::unique_ptr<Node> a, std::unique_ptr<Node> b) {
        Node* fac = nullptr; Node* lit = nullptr;
        if (a->fcol && b->kind == Node::Str) { fac = a.get(); lit = b.get(); }
        else if (b->fcol && a->kind == Node::Str) { fac = b.get(); lit = a.get(); }
        if (fac) {
            long idx = -1;
            for (size_t i = 0; i < fac->fcol->levels.size(); ++i)
                if (fac->fcol->levels[i] == lit->sval) { idx = static_cast<long>(i); break; }
            if (idx < 0) {  // literal is not a level of this factor
                auto cst = mk(Node::Num); cst->num = (k == Node::Eq) ? 0.0 : 1.0; return cst;
            }
            auto v = mk(Node::Var);
            const auto& codes = fac->fcol->codes;  // CowVec<int32_t> (borrowed or owned)
            v->owned.resize(codes.size());
            for (size_t i = 0; i < codes.size(); ++i)
                v->owned[i] = codes[i] < 0 ? NAd : static_cast<double>(codes[i]);
            v->var = v->owned.data();
            auto num = mk(Node::Num); num->num = static_cast<double>(idx);
            return bin(k, std::move(v), std::move(num));
        }
        if (a->fcol || b->fcol || a->kind == Node::Str || b->kind == Node::Str)
            err("string/factor comparison must be `factorvar ==/!= \"literal\"`");
        return bin(k, std::move(a), std::move(b));
    }
    // EM0: [U] 13.2.5 orders the binary operators ^, -(negation), /, *, -(subtraction), +,
    // each level left-associative, so a+b-c is a+(b-c) and a*b/c is a*(b/c)
    // (oracle w2am_expr_binary_order eo_gen_pm / eo_gen_md; 0.1+0.2-0.3 = 2^-55).
    std::unique_ptr<Node> add() {
        auto a = sub();
        for (;;) { ws(); if (eat("+")) a = bin(Node::Add, std::move(a), sub()); else break; }
        return a;
    }
    std::unique_ptr<Node> sub() {
        auto a = mul();
        for (;;) { ws(); if (eat("-")) a = bin(Node::Sub, std::move(a), mul()); else break; }
        return a;
    }
    std::unique_ptr<Node> mul() {
        auto a = div();
        for (;;) { ws(); if (eat("*")) a = bin(Node::Mul, std::move(a), div()); else break; }
        return a;
    }
    std::unique_ptr<Node> div() {
        auto a = neg();
        for (;;) { ws(); if (eat("/")) a = bin(Node::Div, std::move(a), neg()); else break; }
        return a;
    }
    // FU-811: negation and `!` bind looser than `^`, which is LEFT-associative
    // (oracle w2ac_expr_precedence, StataBE 19.5): -a^2 is -(a^2) (gen_neg_pow),
    // !a^0 is !(a^0) (gen_not_pow0), !a==b is (!a)==b (gen_not_eq), 2^3^2 is
    // (2^3)^2 = 64 (gen_chain_const). A sign or `!` right of `^` covers only the
    // next operand: 2^-a^2 is (2^(-a))^2 (gen_pow_neg_pow), -2^-a is -(2^(-a)).
    // help operators lists `!` ahead of `^`; the capture refutes that.
    // `~` is `!` (gen_tilde_pow0); a sign after `!`, spaces or not, is invalid
    // syntax in Stata (w2ac_expr_relational gen_not_neg `!-t` r(198), disp_not_gt;
    // w2ac_expr_valueref gen_not_neg_sp `! -t`, gen_not_plus `!+t`, gen_tilde_neg,
    // gen_not_not_neg `!!-t`, gen_neg_not_neg `-!-t`, gen_pow_not_neg `2^!-t`).
    bool eatNot() {
        if (p >= s.size() || (s[p] != '!' && s[p] != '~') || (p + 1 < s.size() && s[p + 1] == '=')) return false;
        ++p;
        ws();
        if (p < s.size() && (s[p] == '-' || s[p] == '+')) err("invalid syntax: a sign after `!`");
        return true;
    }
    std::unique_ptr<Node> neg() {
        DepthGuard _g(depth);
        ws();
        if (eatNot()) { auto n = mk(Node::Not); n->a = neg(); return n; }
        if (eat("-")) { auto n = mk(Node::Neg); n->a = neg(); return n; }
        if (eat("+")) return neg();
        return powx();
    }
    std::unique_ptr<Node> powx() {
        auto a = primary();
        for (;;) {
            ws();
            if (!eat("^")) break;
            a = bin(Node::Pow, std::move(a), unary());
        }
        return a;
    }
    // The right operand of `^`: leading signs / `!` apply to the next primary only.
    std::unique_ptr<Node> unary() {
        DepthGuard _g(depth);
        ws();
        if (eatNot()) { auto n = mk(Node::Not); n->a = unary(); return n; }
        if (eat("-")) { auto n = mk(Node::Neg); n->a = unary(); return n; }
        if (eat("+")) return unary();
        return primary();
    }
    std::vector<std::unique_ptr<Node>> arglist() {  // '(' already consumed
        std::vector<std::unique_ptr<Node>> v;
        ws();
        if (p < s.size() && s[p] == ')') { ++p; return v; }
        v.push_back(orx());
        while (eat(",")) v.push_back(orx());
        if (!eat(")")) err("expected , or )");
        return v;
    }
    std::unique_ptr<Node> primary() {
        ws();
        if (eat("(")) { auto n = orx(); if (!eat(")")) err("expected )"); return n; }
        if (p < s.size() && s[p] == '"') {  // string literal (factor equality only)
            ++p; size_t st = p;
            while (p < s.size() && s[p] != '"') ++p;
            if (p >= s.size()) err("unterminated string literal");
            auto n = mk(Node::Str); n->sval = s.substr(st, p - st); ++p; return n;
        }
        // Extended missing literal `.a` … `.z` — a '.' followed by exactly one
        // lowercase letter that does NOT continue into an identifier (so
        // `.abc` is not a missing, it stays an error, and a variable named
        // `a` is untouched because the '.' must lead).
        if (p + 1 < s.size() && s[p] == '.' && s[p + 1] >= 'a' && s[p + 1] <= 'z' &&
            (p + 2 >= s.size() ||
             (!cecon::ident::isIdentByte(s[p + 2]) && s[p + 2] != '.'))) {
            int kind = cecon::kMissA + (s[p + 1] - 'a');
            p += 2;
            auto n = mk(Node::Num);
            n->num = cecon::stata_missing(kind);
            return n;
        }
        // bare missing literal: a '.' not part of a number
        if (p < s.size() && s[p] == '.' && (p + 1 >= s.size() || !std::isdigit((unsigned char)s[p + 1]))) {
            ++p; auto n = mk(Node::Num); n->num = NAd; return n;
        }
        if (p < s.size() && (std::isdigit((unsigned char)s[p]) || s[p] == '.')) {
            size_t st = p;
            while (p < s.size() && (std::isalnum((unsigned char)s[p]) || s[p] == '.' ||
                                    ((s[p] == '+' || s[p] == '-') && (s[p - 1] == 'e' || s[p - 1] == 'E'))))
                ++p;
            // Strict numeric literal (C1-F11): the greedy scan above grabs the whole
            // [alnum.] run, but std::stod silently consumes only a valid prefix and
            // drops the rest (5.5.5->5.5, 5foo->5, 1e->1, 1.2.3->1.2). Reject any token
            // std::stod does not fully consume, matching Stata's r(198). Hex forms (0x10)
            // are consumed WHOLE by strtod but Stata rejects them, so bar 'x'/'X' too.
            std::string tok = s.substr(st, p - st);
            for (char c : tok)
                if (c == 'x' || c == 'X') err("invalid numeric literal '" + tok + "'");
            size_t consumed = 0;
            double val;
            try { val = std::stod(tok, &consumed); }
            catch (const std::exception&) { err("invalid numeric literal '" + tok + "'"); }
            if (consumed != tok.size()) err("invalid numeric literal '" + tok + "'");
            auto n = mk(Node::Num); n->num = val; return n;
        }
        if (p < s.size() && cecon::ident::isIdentStartByte(s[p])) {
            size_t st = p;
            while (p < s.size() && cecon::ident::isIdentByte(s[p])) ++p;
            std::string id = s.substr(st, p - st);
            if (id == "_pi") { auto n = mk(Node::Num); n->num = M_PI; return n; }
            if (id == "_n") return mk(Node::ObsN);
            if (id == "_N") return mk(Node::ObsBigN);
            // BUG-369: time-series operators INSIDE an expression. `regress y L.x`
            // has always worked (model.cpp's ts_prepass materialises the varlist),
            // but `gen ly = L.y` did not — the identifier scan above stops at '.',
            // leaving a bare "L" that failed as an unknown variable. Same operators,
            // same ts_apply, same tsset metadata: only this wiring was missing, which
            // is what made it a defect rather than an unimplemented feature.
            //
            // Deliberately conservative: the TS reading is taken ONLY when the whole
            // prefix is a pure L/F/D/S chain AND the base variable exists. Anything
            // else rewinds and falls through to the pre-existing behaviour, so no
            // expression that parsed before can change meaning here.
            if (p < s.size() && s[p] == '.' && cecon::is_ts_prefix(id)) {
                size_t save = p;
                std::string tok = id;
                size_t q = p;
                while (q < s.size() && (cecon::ident::isIdentByte(s[q]) || s[q] == '.')) {
                    tok += s[q];
                    ++q;
                }
                size_t dot = tok.rfind('.');
                std::string pre = (dot == std::string::npos) ? std::string() : tok.substr(0, dot);
                std::string base = (dot == std::string::npos) ? std::string() : tok.substr(dot + 1);
                if (!base.empty() && cecon::is_ts_prefix(pre) && fr.has(base)) {
                    p = q;
                    cecon::TsOpSpec spec = cecon::parse_tsop(tok);
                    if (!spec.ok) err("bad time-series operator in '" + tok + "': " + spec.err);
                    if (fr.panel_time().empty())
                        err("time-series operator '" + tok + "' used but no time variable is "
                            "set; run `tsset t` (or `xtset id t`) first");
                    // Time/panel/value must be numeric: a FACTOR column has an empty
                    // .f64, so .data() is nullptr and ts_apply would deref it. Same
                    // guard cmds_ts.cpp::cmd_tsops carries, same reason.
                    auto numeric = [&](const std::string& nm, const char* role) -> const double* {
                        if (fr.col(nm).is_factor)
                            err(std::string(role) + " '" + nm + "' is a factor but must be "
                                "numeric for a time-series operator");
                        return fr.col(nm).f64.data();
                    };
                    const double* basep = numeric(base, "variable");
                    const double* tmp = numeric(fr.panel_time(), "timevar");
                    const double* pnp = fr.panel_id().empty() ? nullptr
                                                              : numeric(fr.panel_id(), "panelvar");
                    std::vector<double> tsout;
                    std::string tserr;
                    // E1 holds by construction: this materialises over the frame as it
                    // stands, and `if`/keepif is a separate later op on the result.
                    if (!cecon::ts_apply(basep, fr.n_rows(), tmp, pnp, fr.ts_delta(), spec,
                                         tsout, tserr))
                        err("time-series operator '" + tok + "': " + tserr);
                    auto tsv = mk(Node::Var);
                    tsv->owned = std::move(tsout);
                    tsv->var = tsv->owned.data();
                    return tsv;
                }
                // Pure-TS prefix but the base column does not exist: name the BASE in
                // the error, not the operator letter. Falling through would report
                // "unknown variable L" for `L.nosuch`, which points at the wrong token.
                if (!base.empty() && cecon::is_ts_prefix(pre) && !fr.has(id))
                    err("unknown variable " + base);
                p = save;  // not a TS token after all — restore and fall through
            }
            ws();
            if (p < s.size() && s[p] == '(') {  // function call
                // B3(b): the LITERAL pseudo-functions. td/tc/tC/tm/tq/tw/th and
                // the single-letter synonyms d/m/q/w/h/y take an unquoted date
                // literal, not an expression: Stata answers r(198) "td(x)
                // invalid" for a variable argument while dofm(x) is fine. So the
                // argument text is lifted here, before the expression parser can
                // choke on `15mar2020`, and the whole call collapses to a
                // constant. Same table and same parser as PIET's copy
                // (cecon::literal_scale_for / stata_datetime_literal), so the two
                // evaluators cannot disagree about what `tq(2005q1)` means.
                // B3(c): tin(a,b) / twithin(a,b) -- "times in" / "times within".
                // Both take LITERALS on the tsset scale and expand, at parse
                // time, into an ordinary range test on the time variable, so no
                // opcode and no per-row machinery are needed. tin includes both
                // endpoints and twithin excludes them (measured 8 vs 6 over the
                // same 12-quarter series).
                //
                // The literal's meaning depends on the tsset variable's DISPLAY
                // FORMAT -- 2010q1 on a %tq series, 02jan2020 on a %td one --
                // which is a deliberate exception to the "format is carriage
                // only" rule in frame.h: for these two functions the format is
                // the semantics, and Stata reads it the same way. A format the
                // engine does not recognise falls back to a plain number, and a
                // literal that does not fit the scale is an error rather than a
                // quiet wrong window (Stata: r(198) for tin(2010,2011) on a
                // quarterly series). No tsset at all is r(101) there, whatever
                // the arguments, so it is tested first; PIET words each refusal
                // as Stata does (eval::timeSpanRefusal). A blank endpoint is no
                // bound on that side, and a blank list none at all ([FN]
                // tin(): "blank to indicate no minimum date"; StataBE 19.5 with a
                // missing t: tin(2010q3,) counts it, tin(,2010q3) does not, and
                // tin() and twithin(,) count every row).
                if (id == "tin" || id == "twithin") {
                    size_t q = p + 1;
                    int depth = 1;
                    while (q < s.size() && depth) {
                        if (s[q] == '(') ++depth;
                        else if (s[q] == ')') --depth;
                        if (depth) ++q;
                    }
                    if (depth) err("expected ) after " + id + "(");
                    const std::string raw = s.substr(p + 1, q - p - 1);
                    const std::string tv = fr.panel_time();
                    if (tv.empty())
                        err(id + "() requires tsset");
                    size_t comma = std::string::npos;
                    int d2 = 0;
                    for (size_t k = 0; k < raw.size(); ++k) {
                        if (raw[k] == '(') ++d2;
                        else if (raw[k] == ')') --d2;
                        else if (raw[k] == ',' && d2 == 0) { comma = k; break; }
                    }
                    const bool blankList = raw.find_first_not_of(" \t") == std::string::npos;
                    if (comma == std::string::npos && !blankList)
                        err(id + "() takes two arguments");
                    const std::string fmt = fr.ts_format();
                    auto conv = [&](std::string lit) -> double {
                        // trim
                        size_t b = lit.find_first_not_of(" \t");
                        size_t e2 = lit.find_last_not_of(" \t");
                        if (b == std::string::npos) return NAd;
                        lit = lit.substr(b, e2 - b + 1);
                        cecon::LiteralScale sc;
                        bool scaled = true;
                        if (fmt.rfind("%tq", 0) == 0) sc = cecon::LiteralScale::Quarter;
                        else if (fmt.rfind("%tm", 0) == 0) sc = cecon::LiteralScale::Month;
                        else if (fmt.rfind("%tw", 0) == 0) sc = cecon::LiteralScale::Week;
                        else if (fmt.rfind("%th", 0) == 0) sc = cecon::LiteralScale::Half;
                        else if (fmt.rfind("%tC", 0) == 0) sc = cecon::LiteralScale::ClockLeap;
                        else if (fmt.rfind("%tc", 0) == 0) sc = cecon::LiteralScale::Clock;
                        else if (fmt.rfind("%td", 0) == 0) sc = cecon::LiteralScale::Day;
                        else scaled = false;   // %ty, %9.0g, unknown: a plain number
                        if (!scaled) {
                            try {
                                size_t used = 0;
                                double v = std::stod(lit, &used);
                                return used == lit.size() ? v : NAd;
                            } catch (const std::exception&) { return NAd; }
                        }
                        return cecon::stata_datetime_literal(lit, sc);
                    };
                    // An endpoint: blank (no bound), else a literal on the scale.
                    const auto blank = [](const std::string& t) {
                        return t.find_first_not_of(" \t") == std::string::npos;
                    };
                    const std::string loRaw = blankList ? std::string() : raw.substr(0, comma);
                    const std::string hiRaw = blankList ? std::string() : raw.substr(comma + 1);
                    const double lo = blank(loRaw) ? NAd : conv(loRaw);
                    const double hi = blank(hiRaw) ? NAd : conv(hiRaw);
                    if ((!blank(loRaw) && std::isnan(lo)) || (!blank(hiRaw) && std::isnan(hi)))
                        err(id + "(" + raw + ") does not match the tsset scale");
                    p = q + 1;
                    auto mkvar = [&]() {
                        auto v = mk(Node::Var);
                        v->var = fr.col(tv).f64.data();
                        return v;
                    };
                    auto mknum = [&](double x) { auto n2 = mk(Node::Num); n2->num = x; return n2; };
                    const bool strict = (id == "twithin");
                    std::unique_ptr<Node> left, right;
                    if (!blank(loRaw)) {
                        left = mk(strict ? Node::Gt : Node::Ge);
                        left->a = mkvar(); left->b = mknum(lo);
                    }
                    if (!blank(hiRaw)) {
                        right = mk(strict ? Node::Lt : Node::Le);
                        right->a = mkvar(); right->b = mknum(hi);
                    }
                    if (!left && !right) return mknum(1.0);
                    if (!left) return right;
                    if (!right) return left;
                    auto both = mk(Node::And);
                    both->a = std::move(left); both->b = std::move(right);
                    return both;
                }
                cecon::LiteralScale ls;
                if (cecon::literal_scale_for(id, ls)) {
                    size_t q = p + 1;
                    int depth = 1;
                    while (q < s.size() && depth) {
                        if (s[q] == '(') ++depth;
                        else if (s[q] == ')') --depth;
                        if (depth) ++q;
                    }
                    if (depth) err("expected ) after " + id + "(");
                    const std::string raw = s.substr(p + 1, q - p - 1);
                    const double lit = cecon::stata_datetime_literal(raw, ls);
                    // An unparseable literal is an ERROR, not missing -- this is
                    // the one place in the date family where Stata refuses
                    // rather than answering `.` (r(198)).
                    if (std::isnan(lit)) err(id + "(" + raw + ") invalid");
                    p = q + 1;
                    auto n = mk(Node::Num);
                    n->num = lit;
                    return n;
                }
                const FnInfo* fi = find_fn(id);
                if (!fi) err("unknown function " + id);
                ++p;
                auto args = arglist();
                int na = static_cast<int>(args.size());
                if (na < fi->amin || na > fi->amax) err("wrong number of arguments to " + id);
                auto n = mk(Node::Fn); n->fn = fi->id; n->args = std::move(args);
                if ((n->fn == F_rnormal || n->fn == F_uniform) && !g_rng_coords_set && !g_mt64_on)
                    err(id + "() needs draw coordinates: the call carried no --rng-seed/"
                        "--rng-stream, and without them two successive draws would repeat "
                        "each other");
                // Every draw site gets its own ordinal, so two runiform() calls in
                // one expression are independent rather than identical.
                if (n->fn == F_runiform || n->fn == F_runiformint || n->fn == F_rnormal ||
                    n->fn == F_uniform)
                    n->num = static_cast<double>(g_rng_node_counter++);
                return n;
            }
            if (!fr.has(id)) err("unknown variable " + id);
            const FrameColumn& c = fr.col(id);
            if (c.is_factor) {
                // Factor reference: leave var null, carry the column so cmp()
                // can resolve `factorvar ==/!= "literal"`. Any numeric use of it
                // falls through to ev()'s null-var guard.
                auto fv = mk(Node::Var); fv->fcol = &c; return fv;
            }
            auto v = mk(Node::Var); v->var = c.f64.data();
            ws();
            if (p < s.size() && s[p] == '[') {  // subscript
                ++p; auto idx = orx(); if (!eat("]")) err("expected ]");
                auto sub = mk(Node::Subscript); sub->a = std::move(v); sub->b = std::move(idx);
                return sub;
            }
            return v;
        }
        err("unexpected token");
    }
};

// Stata percentile of an ascending-sorted non-missing vector (unit weights).
// Shared by egen median (p=50) and summarize,detail.
double stata_pctile(const std::vector<double>& sorted, double p) {
    int64_t N = static_cast<int64_t>(sorted.size());
    if (N == 0) return NAd;
    if (N == 1) return sorted[0];
    double Pi = N * p / 100.0;
    double fl = std::floor(Pi);
    if (fl == Pi && Pi >= 1 && static_cast<int64_t>(Pi) < N)
        return (sorted[static_cast<int64_t>(Pi) - 1] + sorted[static_cast<int64_t>(Pi)]) / 2.0;
    int64_t idx = static_cast<int64_t>(fl) + 1;
    if (idx > N) idx = N;
    if (idx < 1) idx = 1;
    return sorted[idx - 1];
}

// Weighted Stata percentile (W:P5). `sorted` is ascending non-missing values
// paired index-for-index with their (positive) weights; W is their sum.
//
// This is the same rule stata_pctile above implements, written in cumulative
// weight rather than in rank — [R] summarize's weighted case is the unweighted
// one with the running count replaced by the running weight. Target = p·W/100;
// walk until the cumulative weight reaches it. Landing EXACTLY on a boundary
// averages with the next distinct observation (the unweighted branch's
// `fl == Pi` case); otherwise the observation that straddles the target is the
// percentile. With every wᵢ == 1 this reduces to stata_pctile term for term,
// which is what the aw/fw parity checks pin.
double stata_pctile_weighted(const std::vector<double>& sorted, const std::vector<double>& w,
                             double W, double p) {
    const int64_t N = static_cast<int64_t>(sorted.size());
    if (N == 0 || W <= 0.0) return NAd;
    if (N == 1) return sorted[0];
    const double target = W * p / 100.0;
    // Tolerance for "cumulative weight landed exactly on the target": weights
    // are doubles, so an exact == would make the boundary case unreachable for
    // non-integer weights while firing for integer ones (fweight). Scale it to
    // the magnitudes involved rather than using a fixed epsilon.
    const double tol = 1e-9 * std::max(1.0, std::fabs(W));
    double cum = 0.0;
    for (int64_t i = 0; i < N; ++i) {
        cum += w[i];
        if (cum > target + tol) return sorted[i];
        if (std::fabs(cum - target) <= tol) {
            // Boundary: average with the next observation, mirroring the
            // unweighted midpoint rule. At the top edge there is no next one.
            return (i + 1 < N) ? (sorted[i] + sorted[i + 1]) / 2.0 : sorted[i];
        }
    }
    return sorted[N - 1];
}

}  // namespace

// The published view of FNS above — same table, opcode ids dropped. Built once, in
// table order. Defined here rather than beside the dump's caller so there is exactly
// one list of function names in the engine: the one the parser resolves against.
void set_rng_coords(long long seed, long long stream) {
    g_rng_seed = static_cast<int64_t>(seed);
    g_rng_stream = static_cast<int64_t>(stream);
    g_rng_coords_set = true;
    // Node ordinals restart with every set of coordinates, so a draw is a function
    // of this call's argv alone. They used to run on for the life of the process:
    // under `cobra serve` the n-th draw site of a request got ordinal (all draw sites
    // any earlier request had parsed) + n, so the same (seed, stream) answered
    // differently depending on the resident process's history. Unchanged for a
    // one-shot process, whose counter starts at 0 anyway.
    g_rng_node_counter = 0;
}

void clear_rng_coords() { g_rng_coords_set = false; }

bool begin_rng_mt64(const std::string& state) {
    if (!g_mt64.restore(state)) return false;
    g_mt64_on = true;
    return true;
}

uint64_t end_rng_mt64() {
    const uint64_t n = g_mt64_on ? g_mt64.taken() : 0;
    g_mt64_on = false;
    return n;
}

const std::vector<ExprFnInfo>& expr_function_table() {
    static const std::vector<ExprFnInfo> T = [] {
        std::vector<ExprFnInfo> v;
        v.reserve(sizeof(FNS) / sizeof(FNS[0]) + 12);
        for (const FnInfo& f : FNS) v.push_back({f.name, f.amin, f.amax});
        // B3(b): the literal pseudo-functions are resolved in the PARSER, not
        // through FNS[], so a census built from the table alone reports them as
        // unsupported while `gen x = tq(2005q1)` works. They are published here
        // because this list answers "what names does the expression language
        // accept", and the answer includes these. Arity is 1: one literal.
        static const char* kLiteralFns[] = {"td", "tc", "tC", "tm", "tq", "tw",
                                            "th", "d", "m", "q", "w", "h", "y"};
        for (const char* n : kLiteralFns) v.push_back({n, 1, 1});
        // tin()/twithin() are expanded in the parser too -- into a range test on
        // the tsset variable -- so they are invisible to a table-only census for
        // the same reason.
        v.push_back({"tin", 2, 2});
        v.push_back({"twithin", 2, 2});
        return v;
    }();
    return T;
}

// ONESHOT-RAMDIET A: which columns does a run actually READ? The wave's premise is
// that most of a wide frame is materialized and then discarded unread, and that is a
// claim about behaviour, not about the do-file — so it gets counted here, at the one
// accessor every engine-side read passes through. ACQUISITIONS only: the counter never
// touches a payload byte, which keeps it inside the rule a per-item diagnostic already
// broke once (a payload-touching walk cost a2_mini +11.6% wall / +98k faults).
// ONESHOT-RAMDIET WS-A census: the tally lives in io/columnar.cpp (both read gates
// need it, and piet_native compiles that TU standalone — a Frame symbol there is an
// unresolvable link). These are the Frame-side forwarding shims.
void Frame::note_materialization(int64_t ncols) {
    cecon_coltouch_generation(ncols);
    cecon_ladder_probe_close();   // LADDER PROBE rides the same generation boundary
}

void Frame::record_column_touch(const std::string& name) {
    cecon_coltouch_note(name);
}

void Frame::type_census(const std::string& path) const {
    std::FILE* f = std::fopen(path.c_str(), "a");   // APPEND: the resident changes shape
    if (f == nullptr) return;                      // through a run and the BIG frame is the target
    // Stata's ranges, copied from the writer's ladder (dta_writer.cpp:48-50) so the
    // census cannot promise a demotion the real ladder would refuse.
    const double kByteLo = -127, kByteHi = 100;
    const double kIntLo = -32767, kIntHi = 32740;
    const double kLongLo = -2147483647, kLongHi = 2147483620;
    int64_t tot8 = 0, totNarrow = 0;
    for (const FrameColumn& c : cols_) {
        if (c.is_factor) continue;  // already int32 codes; the ladder is about f64 columns
        const F64Reader v = c.f64.reader();   // LADDER: a diagnostic must not widen what it measures
        bool anyFinite = false, allIntegral = true, f32ok = true;
        double lo = 0, hi = 0;
        for (int64_t i = 0; i < n_; ++i) {
            const double x = v[static_cast<std::size_t>(i)];
            if (std::isnan(x)) continue;
            if (!anyFinite) { lo = hi = x; anyFinite = true; }
            else { if (x < lo) lo = x; if (x > hi) hi = x; }
            if (x != std::floor(x)) allIntegral = false;
            if (f32ok && static_cast<double>(static_cast<float>(x)) != x) f32ok = false;
        }
        const char* kind = "double";
        int width = 8;
        if (!anyFinite) { kind = "byte"; width = 1; }
        else if (allIntegral && lo >= kByteLo && hi <= kByteHi) { kind = "byte"; width = 1; }
        else if (allIntegral && lo >= kIntLo && hi <= kIntHi) { kind = "int"; width = 2; }
        else if (allIntegral && lo >= kLongLo && hi <= kLongHi) { kind = "long"; width = 4; }
        else if (f32ok) { kind = "float"; width = 4; }
        tot8 += n_ * 8;
        totNarrow += n_ * width;
        std::fprintf(f, "col %s rows=%lld now=8 narrow=%d kind=%s\n", c.name.c_str(),
                     static_cast<long long>(n_), width, kind);
    }
    std::fprintf(f, "TOTAL cols=%zu f64_bytes=%lld narrow_bytes=%lld saving_pct=%.1f\n", cols_.size(),
                 static_cast<long long>(tot8), static_cast<long long>(totNarrow),
                 tot8 > 0 ? 100.0 * (tot8 - totNarrow) / static_cast<double>(tot8) : 0.0);
    std::fclose(f);
}

const FrameColumn& Frame::col(const std::string& name) const {
    auto it = idx_.find(name);
    if (it == idx_.end()) throw std::runtime_error("no column: " + name);
    record_column_touch(name);
    return cols_[it->second];
}

const double* Frame::require_f64(const std::string& name) const {
    const FrameColumn& c = col(name);
    if (c.is_factor) throw std::runtime_error("column '" + name + "' is a factor, not f64");
    return c.f64.data();
}

void Frame::reindex() {
    idx_.clear();
    for (int i = 0; i < static_cast<int>(cols_.size()); ++i) idx_[cols_[i].name] = i;
}

// ---- sort-state plane (BUG-272; contract in frame.h) ------------------------------

// The claim's on-disk .hbin spelling — the varlist space-joined into ONE root-level
// string (Stata variable names cannot contain spaces, so the join is unambiguous and
// splits back losslessly). Shared by write_hbin_full / patch_hbin / as_dataset_view
// so the three carriers cannot drift.
// join_sort_keys: single source in frame_internal.h (R2 split)
using frame_detail::join_sort_keys;

void Frame::set_sorted_by(std::vector<std::string> keys) {
    // A claim naming a column this Frame does not have (or naming one twice) can
    // never be honored by a writer and could only have come from a malformed header
    // or a stale caller — install nothing rather than a claim we cannot stand behind.
    // "No claim" is always a safe answer; a wrong one never is.
    std::unordered_set<std::string> seen;
    for (const std::string& k : keys) {
        if (idx_.find(k) == idx_.end() || !seen.insert(k).second) {
            sort_keys_.clear();
            return;
        }
    }
    sort_keys_ = std::move(keys);
}

void Frame::clear_claim_if_key(const std::string& name) {
    if (sort_keys_.empty()) return;
    for (const std::string& k : sort_keys_) {
        if (k == name) {
            sort_keys_.clear();
            return;
        }
    }
}

FrameColumn& Frame::mutable_col(const std::string& name) {
    auto it = idx_.find(name);
    if (it == idx_.end()) throw std::runtime_error("no column: " + name);
    // BUG-272: the caller gets a MUTABLE reference and may rewrite the column's
    // values (the .dta reader's %tc rescale does exactly that) — if that column is a
    // sort-claim key, the claim is now in doubt, and doubt clears. Over-clearing here
    // costs a blank "Sorted by:"; metadata-only callers that must keep a claim can
    // re-install it afterwards (read_dta sets its claim AFTER its mutable_col passes).
    clear_claim_if_key(name);
    return cols_[it->second];
}

void Frame::append_f64_column(const std::string& name, std::vector<double> v) {
    FrameColumn c;
    c.name = name;
    c.is_factor = false;
    c.f64 = std::move(v);
    cols_.push_back(std::move(c));
}

// LADDER: bulk-append an already-narrow payload. Only take_rows calls this — the
// gathered values come from a column that already passed the float-exactness test,
// and a subset of exact values is exact.
void Frame::append_f64_column_narrow(const std::string& name, std::vector<float> v) {
    FrameColumn c;
    c.name = name;
    c.is_factor = false;
    c.f64.set_narrow_f32(std::move(v));
    cols_.push_back(std::move(c));
}

void Frame::append_factor_column(const std::string& name, const std::vector<std::string>& values,
                                 bool empty_is_missing) {
    FrameColumn c;
    c.name = name;
    c.is_factor = true;
    c.codes.resize(values.size());
    std::unordered_map<std::string, int32_t> dict;
    for (size_t i = 0; i < values.size(); ++i) {
        if (empty_is_missing && values[i].empty()) { c.codes[i] = -1; continue; }  // empty string ==
                                                                // missing (matches Frame::from_csv /
                                                                // pandas.factorize use_na_sentinel
                                                                // convention). Callers that need the
                                                                // .dta convention (empty string is a
                                                                // genuine, non-missing level — BUG-024)
                                                                // pass empty_is_missing=false.
        auto it = dict.find(values[i]);
        if (it == dict.end()) {
            int32_t code = static_cast<int32_t>(c.levels.size());
            dict.emplace(values[i], code);
            c.levels.push_back(values[i]);
            c.codes[i] = code;
        } else {
            c.codes[i] = it->second;
        }
    }
    cols_.push_back(std::move(c));
}

void Frame::append_factor_column_coded(const std::string& name, std::vector<int32_t> codes,
                                       std::vector<std::string> levels) {
    FrameColumn c;
    c.name = name;
    c.is_factor = true;
    c.codes = std::move(codes);
    c.levels = std::move(levels);
    cols_.push_back(std::move(c));
}

void Frame::borrow_columns_from(const Dataset& s) {
    n_ = s.n_rows();
    for (const Column& c : s.columns()) {
        FrameColumn fc;
        fc.name = c.name;
        fc.format = c.format;  // P1-A0: optional display format from the header ("" if absent)
        fc.label = c.label;    // BUG-198: same optionality, same "" = absent
        if (c.dtype == DType::Factor) {
            fc.is_factor = true;
            fc.codes.borrow(c.codes(), n_);  // clean: reference the source span, no copy
            // MAXPERF Stage 2 Wave β1: borrow the Dataset's own level strings instead of
            // deep-copying the dictionary (was ~0.4s for a 108K-string factor column).
            // Same lifetime contract as the codes/f64 borrows above: `s` (and whatever it
            // borrowed from) must outlive this Frame until a write materializes it.
            fc.levels.borrow(c.levels.data(), static_cast<int64_t>(c.levels.size()));
        } else if (c.narrow == Narrow::F32) {
            fc.f64.borrow_narrow_f32(c.f32(), n_);  // LADDER: borrow narrow, never widen here
        } else {
            fc.f64.borrow(c.f64(), n_);  // clean: reference the source span, no copy
        }
        cols_.push_back(std::move(fc));
    }
    reindex();
    // BUG-272: adopt the file's sort claim (root "sorted_by" header key, space-
    // joined). AFTER reindex(), because set_sorted_by validates every key against
    // idx_ — a malformed/stale claim installs nothing rather than a lie.
    if (!s.sorted_by().empty()) {
        std::vector<std::string> keys;
        const std::string& sb = s.sorted_by();
        size_t b = 0;
        while (b < sb.size()) {
            size_t e = sb.find(' ', b);
            if (e == std::string::npos) e = sb.size();
            if (e > b) keys.push_back(sb.substr(b, e - b));
            b = e + 1;
        }
        set_sorted_by(std::move(keys));
    }
}

Frame Frame::from_dataset(const Dataset& ds) {
    Frame f;
    f.src_path_ = ds.path();  // remember the source so a clean save can patch it
    if (ds.owns_mmap()) {
        // Hold an independent mmap of the same file alive for this Frame's lifetime, so
        // the borrowed column spans stay valid even after the caller's `ds` is destroyed
        // (e.g. a factory that returns the Frame — see tests/unit/test_d2.cpp makeframe).
        // mmap of the same file shares OS pages, so this is O(1) with no data copy. This
        // is the "shared_ptr<const Dataset> or equivalent" the design calls for.
        f.src_ = std::make_shared<Dataset>(Dataset::open(ds.path()));
        f.borrow_columns_from(*f.src_);
    } else {
        // `ds` is a non-owning view (Dataset::make_view — the serve resident frame's
        // view, contract_A §26): borrow its column spans directly. src_ stays null;
        // the caller keeps the backing buffers (the resident Frame) alive, and NOTHING
        // is re-opened — so a root file the client deleted mid-session can't break this.
        f.borrow_columns_from(ds);
    }
    return f;
}

Frame Frame::from_dataset_shared(std::shared_ptr<const Dataset> sp) {
    Frame f;
    f.src_ = std::move(sp);
    f.src_path_ = f.src_->path();
    f.borrow_columns_from(*f.src_);
    return f;
}

namespace {
// One view Column from one FrameColumn — shared by as_dataset_view (every
// column) and patch_dataset_view (changed columns only). For a factor column
// this is the expensive call: `.ref()` deep-copies the whole level dictionary
// into the Column's owned vector, and — required, not incidental — materializes
// fc's dictionary IN PLACE first if it was still borrowed, because a
// still-borrowed fc.levels may be borrowing the very view Dataset the caller is
// about to replace or overwrite (see ServeSession::rebuild_view). Build order
// in the patch path leans on exactly that: the new Column is FULLY built before
// the old view entry it replaces is touched.
Column view_column_of(const FrameColumn& fc) {
    Column c;
    c.name = fc.name;
    c.view_gen = fc.data->mut_gen;  // MECHOPT W2: the identity the patcher matches on
    c.format = fc.format;  // P1-A0: the resident view carries the format too, so a
                           // serve session's Datastore sees the same string a
                           // disk-loaded one does (contract_A §26 path parity)
    c.label = fc.label;    // BUG-198: the label layer rides the same parity
    if (fc.is_factor) {
        c.dtype = DType::Factor;
        c.data = static_cast<const void*>(fc.codes.data());  // const CowVec: no materialize
        c.nbytes = static_cast<int64_t>(fc.codes.size()) * 4;
        c.levels = fc.levels.ref();
    } else if (fc.f64.narrow()) {
        // LADDER: pass the narrow bytes through. Widening here is what made attempts
        // 1-6 vacuous — every view build re-widened the whole frame. The LOGICAL
        // dtype is unchanged; only storage narrows, and Column::f64() widens lazily
        // for the few consumers that genuinely need contiguous doubles.
        c.dtype = DType::F64;
        c.narrow = Narrow::F32;
        c.data = static_cast<const void*>(fc.f64.f32_ref().data());
        c.nbytes = static_cast<int64_t>(fc.f64.f32_ref().size()) * 4;
    } else {
        c.dtype = DType::F64;
        c.data = static_cast<const void*>(fc.f64.data());
        c.nbytes = static_cast<int64_t>(fc.f64.size()) * 8;
    }
    return c;
}
}  // namespace

Dataset Frame::as_dataset_view() const {
    std::vector<Column> cols;
    cols.reserve(cols_.size());
    for (const FrameColumn& fc : cols_) cols.push_back(view_column_of(fc));
    // BUG-272: the view carries the claim (same parity rationale as format/label
    // above — a Frame rebuilt over this view via from_dataset must see the same
    // claim a disk round trip would deliver).
    return Dataset::make_view(src_path_, n_, std::move(cols), join_sort_keys(sort_keys_));
}

bool Frame::patch_dataset_view(Dataset& view) const {
    // Preconditions under which an in-place patch is even meaningful. Row-count
    // change, a different source path, a different sort claim, or a real
    // mmap-backed Dataset all mean "this is not an incremental step of the same
    // view" — the caller full-rebuilds, which is also the only path that can
    // change those three fields (make_view stamps them at construction).
    if (view.owns_mmap()) return false;
    if (view.n_rows() != n_) return false;
    if (view.path() != src_path_) return false;
    if (view.sorted_by() != join_sort_keys(sort_keys_)) return false;

    // Assemble the new column vector in THIS frame's order, MOVING every entry
    // whose identity is unchanged and rebuilding the rest. Identity is the same
    // payload-pointer test the serve commit's changed/added delta uses
    // (cecon_cli fcol_ptr): an untouched column's pending twin borrows the very
    // buffer the old view entry points at, and adopt_resident_predecessor
    // preserves that pointer across the swap by SHARING the ColumnData — so
    // pointer equality here is exactly "nothing about this column's bytes
    // moved". format/label ride separately from the payload (a label/format
    // command changes them with the pointer unchanged), so they are compared
    // explicitly; nbytes changes only with row count or dtype width, both of
    // which also move the pointer, but the compare is one integer — kept for
    // the same self-healing reason. Add / drop / reorder need no special
    // cases: the walk is in frame order and anything unmatched is rebuilt.
    const std::vector<Column>& old = view.columns();
    std::unordered_map<uint64_t, const Column*> by_gen;
    by_gen.reserve(old.size());
    for (const Column& oc : old)
        if (oc.view_gen != 0) by_gen.emplace(oc.view_gen, &oc);

    std::vector<Column> next;
    next.reserve(cols_.size());
    for (const FrameColumn& fc : cols_) {
        const void* payload =
            fc.is_factor    ? static_cast<const void*>(fc.codes.data())
          : fc.f64.narrow() ? static_cast<const void*>(fc.f64.f32_ref().data())
                            : static_cast<const void*>(fc.f64.data());
        // Identity = the payload GENERATION, not the pointer. A pointer match
        // is vulnerable to allocator address reuse (drop a factor column, mint
        // another of the same rows: the codes buffer can land on the freed
        // address while the view entry still holds the OLD dictionary) and to
        // in-place writes on a uniquely-held column (same address, new bytes).
        // The generation is stamped at every ColumnData construction and
        // bumped at every mutable access (detach), so it never repeats; the
        // pointer/name/format/label compares below are belt over braces.
        auto it = by_gen.find(fc.data->mut_gen);
        const Column* oc = (it != by_gen.end()) ? it->second : nullptr;
        const DType want = fc.is_factor ? DType::Factor : DType::F64;
        const bool fc_narrow = !fc.is_factor && fc.f64.narrow();
        const int64_t wantBytes =
            fc.is_factor ? static_cast<int64_t>(fc.codes.size()) * 4
          : fc_narrow    ? static_cast<int64_t>(fc.f64.f32_ref().size()) * 4
                         : static_cast<int64_t>(fc.f64.size()) * 8;
        const Narrow wantNarrow = fc_narrow ? Narrow::F32 : Narrow::None;
        if (oc != nullptr && oc->data == payload && oc->dtype == want && oc->nbytes == wantBytes &&
            oc->narrow == wantNarrow &&
            oc->name == fc.name && oc->format == fc.format && oc->label == fc.label) {
            // Unchanged in every dimension the view carries — reuse the entry.
            // The move is O(1) (the level vector's buffer changes hands); the
            // source entry in `old` is left moved-from, which is fine: `old` is
            // wholesale-replaced by reset_view_columns below and never read
            // again. const_cast is confined to exactly this "about to be
            // discarded" vector.
            next.push_back(std::move(*const_cast<Column*>(oc)));
        } else {
            next.push_back(view_column_of(fc));
        }
    }
    return view.reset_view_columns(std::move(next));
}

void Frame::adopt_resident_predecessor(Frame& outgoing) {
    // Each pending column that still BORROWS a span whose bytes live in `outgoing`'s
    // OWNED buffer would dangle once `outgoing` is destroyed at the swap. Reattach it
    // by ADOPTING (sharing) outgoing's ColumnData: O(1), the data pointer is preserved
    // (the pending column borrowed exactly that buffer), and — unlike the old
    // buffer-level std::move steal — it is safe even when a stash / tempfile snapshot
    // ALSO shares that payload, because column-CoW then protects every sharer on the
    // next mutation (a steal-move would have emptied the shared ColumnData in place).
    // The pending→resident path builds pending's columns by BORROWING raw spans of the
    // resident's as_dataset_view() (fresh ColumnData that only borrows the bytes by
    // pointer), so the borrow-into-outgoing-owned-buffer hazard is NOT eliminated by
    // the sharing layer — this reattach stays required; only its mechanism simplifies.
    // Columns borrowing the shared ROOT mmap stay borrowed (the session keeps the root
    // Dataset alive; also inherited here via src_). Cost is O(columns touched).
    // Matched by PAYLOAD POINTER, not by name. The question this loop asks is
    // "whose buffer am I borrowing?", and a buffer's identity is its address —
    // the name was only ever a fast way to guess at it, and it guesses WRONG for
    // the one verb that changes a name while keeping the bytes. `rename` on the
    // resident frame produced a pending column whose new name matches no
    // outgoing column, so the lookup missed, the adopt was skipped, and the
    // column went on borrowing a buffer that was destroyed at the swap: the
    // renamed variable read back as uninitialised memory (measured:
    // `gen double side = …` then `rename side other_side` then `summarize`
    // returned 3.4e+110). The payload check below was already the real
    // predicate — the name lookup was the only thing standing in front of it.
    std::unordered_map<const void*, FrameColumn*> by_payload;
    by_payload.reserve(outgoing.cols_.size());
    for (FrameColumn& oc : outgoing.cols_) {
        if (oc.borrowed()) continue;  // outgoing also borrows (root) → nothing of its own to adopt
        by_payload.emplace(oc.payload(), &oc);
    }
    for (FrameColumn& nc : cols_) {
        if (!nc.borrowed()) continue;                 // we own it → nothing can dangle
        auto it = by_payload.find(nc.payload());
        if (it == by_payload.end()) continue;         // not borrowing an outgoing-owned buffer
        FrameColumn& oc = *it->second;
        if (oc.is_factor != nc.is_factor) continue;
        nc.data = oc.data;                            // adopt oc's payload by sharing (keeps its buffer alive)
    }
    // Inherit the root-mmap keepalive so columns still borrowing it survive the swap.
    if (!src_ && outgoing.src_) src_ = outgoing.src_;
    // BUG-272: the sort claim needs NO action here, and that is a judgment, not an
    // omission — this reattach changes buffer OWNERSHIP only (pointer-preserving
    // adopt), never a value, a name, or a row position. This frame keeps its own
    // claim; outgoing's claim dies with outgoing, exactly as its buffers would have.
}

namespace {
// Is a trimmed cell a "missing" marker for a numeric column?
bool csv_is_missing(const std::string& s) {
    return s.empty() || s == "." || s == "NA" || s == "N/A" || s == "na" ||
           s == "NaN" || s == "nan" || s == "null" || s == "NULL";
}
// Parse a full cell as a double; returns false if not entirely numeric.
bool csv_parse_num(const std::string& s, double& out) {
    const char* b = s.c_str();
    char* end = nullptr;
    out = std::strtod(b, &end);
    if (end == b) return false;
    while (*end == ' ' || *end == '\t') ++end;  // allow trailing space
    return *end == '\0';
}
// Read one logical CSV record into `fields`. Handles RFC-4180 quoting (a quoted
// field may contain the delimiter and embedded newlines; "" is a literal quote)
// and LF/CRLF endings. Returns false at clean EOF. `*was_quoted_or_delimited`
// (when non-null) is set true iff the record contains at least one delimiter
// or quote character — i.e. it is a REAL single-column-with-empty-value row,
// not a genuinely empty physical line. Distinguishing the two matters for a
// single-column CSV: `df.to_csv(na_rep="")` on a length-1 float column quotes
// the missing row as `""` (a real, delimited-looking field), which must NOT be
// treated the same as a truly blank line between records.
//
// BUG-292: `field_quoted` (when non-null) reports, PER FIELD, whether that field
// was written inside double quotes. RFC-4180 says a quoted field's content is
// literal, and this reader's caller trims every cell — so a value whose padding
// is part of the value (`tostring, usedisplayformat` under a left-justified
// format renders `42.50    `, len 9) arrived with its pad eaten. The flag is how
// the trim learns to leave a quoted cell alone.
bool csv_read_record(std::istream& in, char delim, std::vector<std::string>& fields,
                     bool* was_quoted_or_delimited = nullptr,
                     std::vector<char>* field_quoted = nullptr) {
    fields.clear();
    if (field_quoted) field_quoted->clear();
    std::string field;
    bool in_quotes = false, any = false, saw_structure = false, cur_quoted = false;
    int ch;
    auto endField = [&]() {
        fields.push_back(field);
        if (field_quoted) field_quoted->push_back(cur_quoted ? 1 : 0);
        field.clear();
        cur_quoted = false;
    };
    while ((ch = in.get()) != EOF) {
        any = true;
        char c = static_cast<char>(ch);
        if (in_quotes) {
            if (c == '"') {
                if (in.peek() == '"') { field.push_back('"'); in.get(); }
                else in_quotes = false;
            } else field.push_back(c);
        } else {
            if (c == '"') { in_quotes = true; saw_structure = true; cur_quoted = true; }
            else if (c == delim) { endField(); saw_structure = true; }
            else if (c == '\n') { endField(); if (was_quoted_or_delimited) *was_quoted_or_delimited = saw_structure; return true; }
            else if (c == '\r') { /* swallow; CRLF or lone CR */ }
            else field.push_back(c);
        }
    }
    if (!any) return false;
    endField();  // final record with no trailing newline
    if (was_quoted_or_delimited) *was_quoted_or_delimited = saw_structure;
    return true;
}
std::string csv_trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t");
    if (a == std::string::npos) return "";
    return s.substr(a, s.find_last_not_of(" \t") - a + 1);
}
// BUG-292: trim UNQUOTED cells only. A quoted cell is literal by RFC-4180, and
// PIET's setcol/tostring transport quotes every cell it writes, so this is what
// makes cell-edge whitespace survive the trip to the engine.
std::string csv_trim_unquoted(const std::string& s, bool quoted) {
    return quoted ? s : csv_trim(s);
}
}  // namespace

Frame Frame::from_csv(const std::string& path, char delim,
                      const std::vector<std::string>& force_factor) {
    cecon::engctr::addDataFileOpen();  // WS-D2: a .csv/.tsv `using` file is a physical open too
    std::ifstream in(to_native_path(path), std::ios::binary);
    if (!in) throw std::runtime_error("cannot open CSV: " + path);
    std::vector<std::string> header;
    if (!csv_read_record(in, delim, header)) throw std::runtime_error("empty CSV: " + path);
    const int K = static_cast<int>(header.size());
    // Strip a leading UTF-8 BOM (EF BB BF) baked into the first header cell so the
    // first variable name is usable — Stata's `import delimited` drops it likewise.
    if (!header.empty() && header[0].size() >= 3 &&
        static_cast<unsigned char>(header[0][0]) == 0xEF &&
        static_cast<unsigned char>(header[0][1]) == 0xBB &&
        static_cast<unsigned char>(header[0][2]) == 0xBF)
        header[0].erase(0, 3);
    for (auto& h : header) h = csv_trim(h);

    std::vector<char> is_num(K, 1);  // pass-1: assume numeric until proven otherwise
    for (const auto& ff : force_factor) {
        for (int k = 0; k < K; ++k) if (header[k] == ff) is_num[k] = 0;
    }
    // --- pass 1: infer column types + count rows ---
    // A record is a genuinely blank line (skippable) only when it has a single
    // empty field AND that field carried no CSV structure (no delimiter, no
    // quotes) — for a single-column file (K==1), a real data row whose lone
    // value is empty (e.g. `df.to_csv(na_rep="")` quotes it as `""`, which DOES
    // carry structure) must still count as a row, or every all-missing row in
    // a 1-column CSV silently vanishes (undercounting nrows / misaligning any
    // caller that assumes CSV row order == source row order).
    int64_t nrows = 0;
    std::vector<std::string> rec;
    std::vector<char> quoted;
    bool structured;
    // The file line each record starts on (a quoted field may span lines), so a
    // refusal below can name it.
    auto newlines = [](const std::vector<std::string>& fields) {
        int64_t n = 0;
        for (const auto& s : fields) n += std::count(s.begin(), s.end(), '\n');
        return n;
    };
    int64_t line = 1 + newlines(header);
    while (csv_read_record(in, delim, rec, &structured, &quoted)) {
        const int64_t at = line + 1;
        line = at + newlines(rec);
        if (rec.size() == 1 && rec[0].empty() && !structured) continue;  // truly blank line
        ++nrows;
        // BUG-414: a record with MORE fields than the names line has no column
        // for the extra ones, and pass 2 never reads past K -- the value was
        // dropped at rc 0. Refuse instead, naming the line. Empty extras (a
        // trailing delimiter) hold nothing and stay accepted.
        for (size_t k = static_cast<size_t>(K); k < rec.size(); ++k) {
            const bool q = k < quoted.size() && quoted[k] != 0;
            if (csv_trim_unquoted(rec[k], q).empty()) continue;
            throw EngineError("bad_input",
                              "line " + std::to_string(at) + " has " +
                                  std::to_string(rec.size()) + " fields, but the names line has " +
                                  std::to_string(K),
                              path);
        }
        for (int k = 0; k < K && k < static_cast<int>(rec.size()); ++k) {
            if (!is_num[k]) continue;
            // Type INFERENCE still trims either way: csv_parse_num tolerates the
            // padding, and a quoted ` 42 ` is a number in any reading.
            std::string cell = csv_trim(rec[k]);
            if (csv_is_missing(cell)) continue;
            double v;
            if (!csv_parse_num(cell, v)) is_num[k] = 0;
        }
    }
    // --- pass 2: materialize ---
    in.clear();
    in.seekg(0);
    csv_read_record(in, delim, rec);  // skip header

    Frame f;
    f.n_ = nrows;
    f.cols_.resize(K);
    std::vector<std::unordered_map<std::string, int32_t>> dict(K);
    for (int k = 0; k < K; ++k) {
        f.cols_[k].name = header[k];
        if (is_num[k]) f.cols_[k].f64.resize(static_cast<size_t>(nrows));
        else { f.cols_[k].is_factor = true; f.cols_[k].codes.resize(static_cast<size_t>(nrows)); }
    }
    int64_t row = 0;
    while (csv_read_record(in, delim, rec, &structured, &quoted)) {
        if (rec.size() == 1 && rec[0].empty() && !structured) continue;  // truly blank line
        for (int k = 0; k < K; ++k) {
            // BUG-292: a QUOTED cell keeps its edge whitespace (it is part of the
            // value); an unquoted one is trimmed exactly as before.
            const bool q = k < static_cast<int>(quoted.size()) && quoted[static_cast<size_t>(k)] != 0;
            std::string cell = (k < static_cast<int>(rec.size()))
                                   ? csv_trim_unquoted(rec[k], q)
                                   : std::string();
            FrameColumn& c = f.cols_[k];
            if (!c.is_factor) {
                double v = std::numeric_limits<double>::quiet_NaN();
                if (!csv_is_missing(cell)) csv_parse_num(cell, v);
                c.f64[static_cast<size_t>(row)] = v;
            } else {
                if (cell.empty()) { c.codes[static_cast<size_t>(row)] = -1; continue; }
                auto it = dict[k].find(cell);
                int32_t code;
                if (it == dict[k].end()) {
                    code = static_cast<int32_t>(c.levels.size());
                    dict[k].emplace(cell, code);
                    c.levels.push_back(cell);
                } else code = it->second;
                c.codes[static_cast<size_t>(row)] = code;
            }
        }
        ++row;
    }
    f.reindex();
    return f;
}

// `setcol` support: a HEADERLESS single-column CSV (one raw value per row) —
// reuses from_csv's own record reader (csv_read_record) and cell helpers
// (csv_trim/csv_is_missing/csv_parse_num) above, so the quoting/missing rules
// stay a single source of truth. See frame.h doc comment for the exact
// f64-vs-factor missing convention.
FrameColumn Frame::read_csv_column(const std::string& path, const std::string& dtype, int64_t expect_n) {
    if (dtype != "f64" && dtype != "factor")
        throw EngineError("bad_input", "setcol: --dtype must be 'f64' or 'factor' (got '" + dtype + "')", dtype);
    std::ifstream in(to_native_path(path), std::ios::binary);
    if (!in) throw EngineError("data_io", "setcol: cannot open --values file: " + path, path);

    FrameColumn c;
    c.is_factor = (dtype == "factor");
    std::unordered_map<std::string, int32_t> dict;
    std::vector<double> f64vals;
    std::vector<int32_t> codevals;
    std::vector<std::string> rec;
    std::vector<char> quoted;
    bool structured;
    int64_t n = 0;
    while (csv_read_record(in, ',', rec, &structured, &quoted)) {
        // Headerless SINGLE-column payload: every record is a data row — a bare
        // blank line and a quoted "" both mean a missing cell (there is no
        // "skippable line break" here; a stray trailing blank line is caught by
        // the row-count check below as an honest mismatch).
        (void)structured;
        // BUG-292: the setcol transport. PIET quotes EVERY cell it writes here
        // (handlers_data_verbs.cpp csvEscape), so this is the site where a
        // left-justified `tostring, usedisplayformat` render lost its pad and
        // came back at lens {5,6,4,5,9} where Stata stores all 9.
        const bool q0 = !quoted.empty() && quoted[0] != 0;
        std::string cell = rec.empty() ? std::string() : csv_trim_unquoted(rec[0], q0);
        if (c.is_factor) {
            if (cell.empty()) { codevals.push_back(-1); ++n; continue; }  // empty -> missing; "." etc. stay literal levels
            auto it = dict.find(cell);
            int32_t code;
            if (it == dict.end()) {
                code = static_cast<int32_t>(c.levels.size());
                dict.emplace(cell, code);
                c.levels.push_back(cell);
            } else {
                code = it->second;
            }
            codevals.push_back(code);
        } else {
            double v = std::numeric_limits<double>::quiet_NaN();
            if (!csv_is_missing(cell) && !csv_parse_num(cell, v))
                throw EngineError("bad_input", "setcol: --values row " + std::to_string(n + 1) +
                    " is not numeric: '" + cell + "'", cell);
            f64vals.push_back(v);
        }
        ++n;
    }
    if (n != expect_n)
        throw EngineError("bad_input", "setcol: --values has " + std::to_string(n) +
            " row(s), the dataset has " + std::to_string(expect_n) + " — row counts must match exactly", path);
    if (c.is_factor) c.codes = std::move(codevals);
    else c.f64 = std::move(f64vals);
    return c;
}

void Frame::setcol(const std::string& name, FrameColumn&& newcol, bool is_new) {
    // BUG-272: the !is_new branch replaces an existing column's payload wholesale —
    // a sort claim keyed on it is void. (is_new appends; a fresh name can't be a key.)
    clear_claim_if_key(name);
    auto it = idx_.find(name);
    newcol.name = name;
    if (is_new) {
        if (it != idx_.end())
            throw EngineError("bad_input", "setcol: --new given but variable '" + name + "' already exists", name);
        cols_.push_back(std::move(newcol));
        reindex();
    } else {
        if (it == idx_.end())
            throw EngineError("missing_column",
                "setcol: variable '" + name + "' not found (pass --new to append it instead)", name);
        // Move-assign the payload+dtype into the EXISTING slot: every other column's
        // position/bytes are untouched, and write_hbin's patch path (name-keyed)
        // reuses this column's on-disk slot whenever the new dtype/size still fits,
        // falling back to a full rewrite automatically otherwise (patch_hbin's own
        // dtype/nbytes mismatch guard) — no structure_dirty_ needed either way.
        cols_[static_cast<size_t>(it->second)] = std::move(newcol);
    }
}

double Frame::store_round(double v, StoreType st) {
    const double r = frame_detail::round_to_store(v, st);
    if (std::isnan(r) && !std::isnan(v)) ++store_overflow_;
    return r;
}

void Frame::set_or_replace(const std::string& name, std::vector<double> v, StoreType st) {
    // BUG-471 T1: the store boundary. Only the gen/egen/row-egen/bygen verbs pass a
    // non-Double type (cobra eval --store-type); Double skips the loop entirely.
    if (st != StoreType::Double)
        for (double& x : v) x = store_round(x, st);
    // BUG-272: replacing a column's VALUES invalidates any sort claim keyed on it
    // (gen/replace/egen/predict/... all funnel through here). A brand-new column can
    // never be a claim key (claims only ever name existing columns), so the append
    // branch below is untouched by construction and this check is a cheap no-op there.
    clear_claim_if_key(name);
    auto it = idx_.find(name);
    if (it != idx_.end()) {
        FrameColumn& c = cols_[it->second];
        c.is_factor = false;
        c.codes.clear();
        c.levels.clear();
        c.f64 = std::move(v);
    } else {
        FrameColumn c;
        c.name = name;
        c.f64 = std::move(v);
        cols_.push_back(std::move(c));
        idx_[name] = static_cast<int>(cols_.size()) - 1;
    }
}

void Frame::set_or_replace_factor(const std::string& name, std::vector<int32_t> codes,
                                 std::vector<std::string> levels) {
    clear_claim_if_key(name);  // same reason as set_or_replace: the VALUES changed
    auto it = idx_.find(name);
    if (it != idx_.end()) {
        FrameColumn& c = cols_[it->second];
        c.is_factor = true;
        c.f64 = std::vector<double>();
        c.codes = std::move(codes);
        c.levels = std::move(levels);
        return;
    }
    FrameColumn c;
    c.name = name;
    c.is_factor = true;
    c.codes = std::move(codes);
    c.levels = std::move(levels);
    cols_.push_back(std::move(c));
    idx_[name] = static_cast<int>(cols_.size()) - 1;
}

void Frame::set_f64_column(const std::string& name, std::vector<double> v) {
    set_or_replace(name, std::move(v));
}

void Frame::check_gen_qualifier(const std::string& name, const GenQualifier& q) const {
    if (q.in_first != 0 && (q.in_first < 1 || q.in_last < q.in_first || q.in_last > n_))
        throw EngineError("bad_input", "--gen-in: range " + std::to_string(q.in_first) + "/" +
                                           std::to_string(q.in_last) + " is not within 1/" +
                                           std::to_string(n_));
    if (q.keep_unselected && has(name) && col(name).is_factor)
        throw EngineError("bad_input", "--gen-replace: '" + name + "' is a string variable", name);
}

// The output step shared by generate and bygen: a row the qualifiers exclude is
// missing, or under replace (q.keep_unselected) keeps the column's current value.
void Frame::store_qualified(const std::string& name, std::vector<double> out,
                            const std::vector<double>& mask, StoreType st,
                            const GenQualifier& q) {
    if (!mask.empty()) {
        const bool keep = q.keep_unselected && has(name);
        F64Reader old;
        if (keep) old = col(name).f64.reader();
        for (int64_t i = 0; i < n_; ++i)
            if (!if_selects(mask[static_cast<size_t>(i)]))
                out[static_cast<size_t>(i)] = keep ? old[static_cast<size_t>(i)] : NAd;
    }
    set_or_replace(name, std::move(out), st);
}

void Frame::generate(const std::string& name, const std::string& expr, bool require_new,
                     const std::string& mask_expr, StoreType st, const GenQualifier& q) {
    RngCallScope rng_call;  // draw coordinates are spent when this call returns
    if (require_new && idx_.find(name) != idx_.end())
        throw EngineError("bad_input", "generate: variable '" + name + "' already exists", name);
    check_gen_qualifier(name, q);
    // Without `by` the frame is the one group, so the mask's _n/_N/subscripts are
    // the dataset's and the column evaluator is the right one. A sum() inside the
    // mask is a plain running total, as it is inside the folded cond() form.
    std::unique_ptr<Node> mast;
    if (!mask_expr.empty()) {
        Parser mp(mask_expr, *this);
        mast = mp.parse();
        bind_group_range(mast.get(), q.in_first, q.in_last);
        materialize_sums(mast.get(), n_, nullptr, nullptr, nullptr, nullptr, nullptr);
    }
    Parser parser(expr, *this);
    auto ast = parser.parse();
    bind_group_range(ast.get(), q.in_first, q.in_last);
    const bool inline_if = inline_if_draws(mast.get(), ast.get());
    std::vector<double> mask;
    if (mast && !inline_if) mask = eval_column(mast.get(), n_);
    apply_in_range(mask, n_, q.in_first, q.in_last);
    if (inline_if && mask.empty()) mask.assign(static_cast<size_t>(n_), 1.0);
    // Whether row c.row is selected; under inline_if its `if` is evaluated now.
    auto selects = [&](const EvalCtx& c) {
        double& m = mask[static_cast<size_t>(c.row)];
        if (inline_if && if_selects(m)) m = ev(mast.get(), c);
        return if_selects(m);
    };
    materialize_sums(ast.get(), n_, nullptr, nullptr, nullptr, nullptr,
                     mask.empty() ? nullptr : &mask);
    if (auto it = idx_.find(name); it != idx_.end() && !cols_[it->second].is_factor) {
        const double* target = static_cast<const FrameColumn&>(cols_[it->second]).f64.data();
        if (has_self_subscript(ast.get(), target)) {
            F64Reader old = cols_[it->second].f64.reader();
            std::vector<double> buf(static_cast<size_t>(n_));
            for (int64_t i = 0; i < n_; ++i) buf[static_cast<size_t>(i)] = old[static_cast<size_t>(i)];
            rebind_var(ast.get(), target, buf.data());
            EvalCtx c;
            c.n_all = n_;
            for (int64_t i = 0; i < n_; ++i) {
                c.row = i;
                if (!mask.empty() && !selects(c)) continue;
                double v = ev(ast.get(), c);
                buf[static_cast<size_t>(i)] = st == StoreType::Double ? v : store_round(v, st);
            }
            store_qualified(name, std::move(buf), mask, st, q);
            return;
        }
    }
    // On Stata's stream a row outside `if`/`in` draws nothing (`gen f = runiform()
    // in 4/6` hands row 4 the stream's FIRST draw, impl_C stata_probe P4), so the
    // rows are walked one at a time and the unselected ones are never evaluated.
    if (g_mt64_on && !mask.empty() && has_draw(ast.get())) {
        std::vector<double> out(static_cast<size_t>(n_), NAd);
        EvalCtx c;
        c.n_all = n_;
        for (int64_t i = 0; i < n_; ++i) {
            c.row = i;
            if (!selects(c)) continue;
            out[static_cast<size_t>(i)] = ev(ast.get(), c);
        }
        store_qualified(name, std::move(out), mask, st, q);
        return;
    }
    store_qualified(name, eval_column(ast.get(), n_), mask, st, q);
}

void Frame::xtset(const std::string& id_col, const std::string& time_col) {
    require_f64(id_col);
    require_f64(time_col);
    panel_id_col_ = id_col;
    panel_time_col_ = time_col;
}

void Frame::tsset(const std::string& time_col) {
    require_f64(time_col);
    panel_id_col_.clear();  // pure time series: ts_apply gets a null panel pointer
    panel_time_col_ = time_col;
}

void Frame::set_ts_delta(double d) {
    if (!(d > 0))
        throw EngineError("bad_input", "tsset delta must be positive");
    ts_delta_ = d;
}

void Frame::tssmooth_ma(const std::string& output_name, const std::string& input_name,
                        int lags, int leads, bool include_focal) {
    // BUG-370: a PANEL is not a requirement, only a possibility. This used to demand
    // xtset outright, so `tsset t` — an ordinary single time series, which is what
    // `tssmooth ma` is for in the first place — could not reach it at all. A pure
    // time series is simply the one-group case: `id == nullptr` below means every
    // row is the same panel, and the windowing math is untouched by that.
    if (panel_time_col_.empty())
        throw std::runtime_error(
            "tssmooth ma requires tsset (time) or xtset (panel id + time) first");
    if (lags < 0 || leads < 0) throw std::runtime_error("tssmooth ma: negative window");
    const double* id = panel_id_col_.empty() ? nullptr : require_f64(panel_id_col_);
    const double* tm = require_f64(panel_time_col_);
    const double* in = require_f64(input_name);

    std::vector<int64_t> order(n_);
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](int64_t a, int64_t b) {
        if (id != nullptr && id[a] != id[b]) return id[a] < id[b];
        return tm[a] < tm[b];
    });

    std::vector<double> out(n_, NAd);
    int64_t g0 = 0;
    while (g0 < n_) {
        int64_t g1 = g0 + 1;
        while (g1 < n_ && (id == nullptr || id[order[g1]] == id[order[g0]])) ++g1;
        const int64_t glen = g1 - g0;
        int64_t lo = 0, hi = -1;
        for (int64_t j = 0; j < glen; ++j) {
            double tj = tm[order[g0 + j]];
            double tmin = tj - lags, tmax = tj + leads;
            while (lo < glen && tm[order[g0 + lo]] < tmin) ++lo;
            if (hi < lo - 1) hi = lo - 1;
            while (hi + 1 < glen && tm[order[g0 + hi + 1]] <= tmax) ++hi;
            double sum = 0;
            int64_t k = 0;
            for (int64_t w = lo; w <= hi; ++w) {
                // BUG-378: Stata's window(#l #c #f) has a CENTRE weight, and an
                // omitted #c defaults to 0 — measured live, not assumed:
                //   window(1 1 1) -> (1/3)*[x(t-1) + 1*x(t) + x(t+1)]
                //   window(1)     -> (1/1)*[x(t-1) + 0*x(t)]
                // So window(1) is a trailing average that EXCLUDES the focal
                // observation, a shape this function could not express at all.
                if (!include_focal && w == j) continue;
                double v = in[order[g0 + w]];
                if (!std::isnan(v)) { sum += v; ++k; }
            }
            if (k > 0) out[order[g0 + j]] = sum / static_cast<double>(k);
        }
        g0 = g1;
    }
    set_or_replace(output_name, std::move(out));
}

int64_t Frame::keep_if(const std::string& expr) {
    RngCallScope rng_call;
    note_materialization(static_cast<int64_t>(cols_.size()));  // WS-A census: close the generation
    Parser parser(expr, *this);
    auto ast = parser.parse();
    std::vector<double> vv = eval_column(ast.get(), n_);
    std::vector<char> keep(n_);
    int64_t m = 0;
    for (int64_t i = 0; i < n_; ++i) {
        double v = vv[i];
        // Stata's bare-expression `if` truthy test is `v != 0`, full stop — it does
        // NOT additionally require non-missing. Missing is represented internally
        // as a value that is never == 0, so `if x` keeps missing rows too (a classic
        // Stata gotcha; relational `if x>5` is unaffected since ev() never returns
        // NaN for a comparison node — see mi() above — so this predicate reduces to
        // the same `v != 0.0` there either way).
        keep[i] = (std::isnan(v) || v != 0.0) ? 1 : 0;
        m += keep[i];
    }
    // COMPACTION: a fresh buffer per column, and it stays that way (RAMDIET-2 null).
    //
    // The obvious suspicion was that this loop is the production line for the
    // retained-but-free pages the campaign has been chasing — 33 full-frame
    // compactions x ~205 columns = ~4,500 allocate-fill-free cycles at 31 different
    // sizes as the row count walks down, and `malloc_zone_pressure_relief` provably
    // will not take those regions back (0.0 MB on twelve consecutive calls).
    //
    // MEASURED, TWICE, AND NEITHER SHAPE WINS. A kept subsequence writes strictly
    // behind its own read (k <= i), so an owned buffer can be compacted forward onto
    // itself with no temporary and no allocation at all. Compacting in place and
    // keeping the capacity cost +11.5% peak footprint (586.7/589.5 -> 654.2/657.8 MB):
    // pinning every column at the pre-filter row count is worse than the churn it
    // avoids. Compacting in place and shrinking whenever capacity exceeded 2x the live
    // rows landed exactly on the baseline (589.8/586.4 MB), i.e. removing 30 of every
    // 31 allocations per cycle changes the peak by nothing measurable.
    //
    // So the peak is not set by this loop's allocation traffic. It is set by bytes
    // that are genuinely live (WS-A: 96.8% of every column materialized here is named
    // again before the next compaction) plus allocator retention that neither fewer
    // allocations nor pressure relief returns. The lever that remains is making the
    // DATA smaller, not making its allocation cheaper.
    // NMISS-CARRY (2026-08-20): count the survivors' missings AS THEY ARE GATHERED.
    // This loop already visits every kept element of every column, so the count is
    // free here — a branch on a value the iteration has in hand. Seeding the cache
    // from it means the header writer downstream states the number instead of
    // rescanning for it, and does so WITHOUT the write-time scan that made the
    // first version of this carry a wash: `drop|if` (434 calls, each writing a
    // frame this very filter had just invalidated) went 18.5 -> 43.6 s when the
    // writer forced the count, and the joinby/merge win it bought was cancelled.
    // Counted per column below, seeded after the row count is updated (n_ = m),
    // because seed_n_missing writes through to the same cache detach() clears.
    std::vector<int64_t> kept_missing;
    kept_missing.reserve(cols_.size());
    for (FrameColumn& col : cols_) {
        const FrameColumn& cc = col;  // const view: read borrowed/owned without materializing
        int64_t miss = 0;
        if (col.is_factor) {
            std::vector<int32_t> nc;
            nc.reserve(m);
            for (int64_t i = 0; i < n_; ++i) if (keep[i]) { if (cc.codes[i] < 0) ++miss; nc.push_back(cc.codes[i]); }
            col.codes = std::move(nc);
            kept_missing.push_back(miss);
        } else if (cc.f64.narrow()) {
            // LADDER: already narrow — gather narrow to narrow. The kept values are a
            // subset of values already proved float-exact, so no re-test is needed.
            const CowVec<float>& src = cc.f64.f32_ref();
            std::vector<float> nn;
            nn.reserve(m);
            for (int64_t i = 0; i < n_; ++i) if (keep[i]) { if (std::isnan(src[i])) ++miss; nn.push_back(src[i]); }
            col.f64.set_narrow_f32(std::move(nn));
            kept_missing.push_back(miss);
        } else {
            // LADDER: this loop is where a frame's owned bytes are born, so it is
            // where the width is decided. Pass one asks whether every KEPT value
            // survives a float round-trip EXACTLY; pass two writes at that width.
            // NaN disqualifies outright — a missing's NaN payload bits carry Stata's
            // extended missing (.a-.z) and float32 does not preserve them, so a
            // narrowed column would silently turn `.a` into plain `.`.
            const F64Reader r = cc.f64.reader();
            bool f32_ok = true;
            for (int64_t i = 0; f32_ok && i < n_; ++i) {
                if (!keep[i]) continue;
                const double x = r[static_cast<std::size_t>(i)];
                if (std::isnan(x) || static_cast<double>(static_cast<float>(x)) != x) f32_ok = false;
            }
            if (f32_ok) {
                // f32_ok implies NO NaN survived (isnan disqualifies the width
                // outright, just above), so this branch's missing count is 0 by
                // construction — not by counting.
                std::vector<float> nn;
                nn.reserve(m);
                for (int64_t i = 0; i < n_; ++i)
                    if (keep[i]) nn.push_back(static_cast<float>(r[static_cast<std::size_t>(i)]));
                col.f64.set_narrow_f32(std::move(nn));
            } else {
                std::vector<double> nf;
                nf.reserve(m);
                for (int64_t i = 0; i < n_; ++i)
                    if (keep[i]) {
                        const double x = r[static_cast<std::size_t>(i)];
                        if (std::isnan(x)) ++miss;
                        nf.push_back(x);
                    }
                col.f64 = std::move(nf);
            }
            kept_missing.push_back(miss);
        }
    }
    n_ = m;
    // Seed AFTER n_ is updated: the counts describe the post-filter column.
    for (std::size_t i = 0; i < cols_.size() && i < kept_missing.size(); ++i) {
        cols_[i].seed_n_missing(kept_missing[i]);
    }
    structure_dirty_ = true;  // row count changed → can't patch onto the source
    // BUG-272: the sort claim SURVIVES keep_if, and that is a theorem, not a guess —
    // the gathers above take rows in ascending index order (stable subsequence), and
    // a subsequence of a lexicographically sorted sequence is itself sorted by the
    // same keys. Stata agrees: `drop if`/`keep if` leaves its sortlist standing.
    return m;
}

Frame Frame::project(const std::vector<std::string>& names) const {
    Frame out;
    out.n_ = n_;
    out.src_ = src_;   // keepalive for any column still borrowing the root mapping
    out.cols_.reserve(names.size());
    for (const std::string& nm : names) {
        auto it = idx_.find(nm);
        if (it == idx_.end())
            throw EngineError("missing_column",
                              "project: no column '" + nm + "' in the resident frame", nm);
        out.cols_.push_back(cols_[it->second]);  // shares ColumnData; no payload copy
    }
    out.reindex();
    return out;
}

void Frame::drop(const std::string& name) {
    // BUG-272: a claim naming a dropped variable clears — the WHOLE claim, not a
    // truncation (a prefix would stay true, but the design rule is "doubt clears",
    // and nothing downstream needs the weaker prefix badly enough to reason about it).
    // Dropping a non-key column leaves the claim intact: row order is untouched and
    // every key still names a live column.
    clear_claim_if_key(name);
    auto it = idx_.find(name);
    if (it == idx_.end()) return;
    cols_.erase(cols_.begin() + it->second);
    reindex();
    structure_dirty_ = true;  // column set changed → can't patch onto the source
}

// ---- by-group helpers ----
namespace frame_detail {  // R2: shared by both frame TUs (see frame_internal.h)
// Materialize columns as doubles (factor codes cast to double) for grouping/sorting.
std::vector<std::vector<double>> cols_as_double(const Frame& fr, int64_t n,
                                                const std::vector<std::string>& names) {
    std::vector<std::vector<double>> out;
    for (const std::string& nm : names) {
        const FrameColumn& c = fr.col(nm);
        std::vector<double> v(n);
        if (c.is_factor) for (int64_t i = 0; i < n; ++i) v[i] = static_cast<double>(c.codes[i]);
        else {
            const F64Reader r = c.f64.reader();   // LADDER: no widening for key columns
            for (int64_t i = 0; i < n; ++i) v[i] = r[static_cast<std::size_t>(i)];
        }
        out.push_back(std::move(v));
    }
    return out;
}
// Compare rows a,b on key columns: NaN sorts last, NaN==NaN.
bool key_less(const std::vector<std::vector<double>>& K, int64_t a, int64_t b) {
    for (const auto& col : K) {
        double x = mi(col[a]), y = mi(col[b]);
        if (x < y) return true;
        if (x > y) return false;
    }
    return false;
}
bool key_eq(const std::vector<std::vector<double>>& K, int64_t a, int64_t b) {
    for (const auto& col : K) {
        double x = col[a], y = col[b];
        if (std::isnan(x) && std::isnan(y)) continue;
        if (x != y) return false;
    }
    return true;
}
}  // namespace frame_detail
using namespace frame_detail;

void Frame::egen_by(const std::string& out, EgenFn fn, const std::string& expr,
                    const std::vector<std::string>& by_cols, double pctile_p, StoreType st,
                    const EgenOpts& eo) {
    // The store type: every function here ends in ONE generate of the chosen type
    // (egen's mean, total, max, ...), so the result is rounded once, like a generate —
    // except default rank. Stata's rank stores `_n`, then adds (_N-1)/2
    // over a tie with replace, and replace WIDENS an integer variable (byte 2.7 -> float,
    // byte 200 -> int) where a fixed store type would truncate or store `.`: refused.
    // Float stays float under replace, so float rank is the one final rounding.
    // track/field/unique only copy an earlier row's `_n` over a tie, which a single
    // store of the final rank reproduces (an out-of-range `_n` is `.` either way).
    if (fn == EgenFn::Rank && frame_detail::is_integer_store(st))
        throw EngineError("bad_input",
                          "egen rank(): --store-type byte|int|long is not supported (Stata builds "
                          "tied ranks with replace, which widens an integer variable)");
    RngCallScope rng_call;
    std::vector<double> v;
    const size_t b0 = expr.find_first_not_of(" \t");
    const std::string bare =
        b0 == std::string::npos ? std::string() : expr.substr(b0, expr.find_last_not_of(" \t") - b0 + 1);
    if (fn == EgenFn::Count && has(bare) && col(bare).is_factor) {
        // BUG-1002: egen count counts the non-missing values, and a string's missing is
        // "" (a factor's code -1 is that same empty string). The numeric
        // evaluator below cannot read a factor at all, so count(s) was refused.
        const FrameColumn& c = col(bare);
        v.assign(static_cast<size_t>(n_), NAd);
        for (int64_t i = 0; i < n_; ++i) {
            const int32_t k = c.codes[i];
            if (k >= 0 && !c.levels[static_cast<size_t>(k)].empty()) v[static_cast<size_t>(i)] = 1.0;
        }
    } else {
        Parser parser(expr, *this);
        auto ast = parser.parse();
        v = eval_column(ast.get(), n_);
    }

    std::vector<int64_t> order(n_);
    std::iota(order.begin(), order.end(), 0);
    std::vector<std::vector<double>> K;
    if (!by_cols.empty()) {
        K = cols_as_double(*this, n_, by_cols);
        std::stable_sort(order.begin(), order.end(),
                         [&](int64_t a, int64_t b) { return key_less(K, a, b); });
    }

    // BUG-1150: without by() the official egen functions read `summarize`
    // (total, mean, sd, std, pc run summarize and return r(sum) / r(mean) / r(sd)),
    // so their sums are summarize's exactly
    // rounded ones (core/exact_sum.h): total(x) of 1e16, twenty-two 1s and -1e16
    // is 22 in StataBE 19.5. With by() they run `by g: gen double = sum(x)`, a
    // running sum in data order, and that stays the naive sum below. gtools'
    // gegen sums in data order with or without by() (EgenOpts::data_order).
    const bool exact = by_cols.empty() && !eo.data_order;
    auto sum_of = [exact](const std::vector<double>& s) {
        if (exact) { ExactSum a; for (double x : s) a.add(x); return a.value(); }
        double t = 0; for (double x : s) t += x; return t;
    };
    // The sum of squared deviations from `mean`: summarize's ExactSqDev without
    // by() (r(sd) = sqrt(ss/(N-1))), the running `sum((x - m[_N])^2)` with it.
    auto ssq_of = [exact](const std::vector<double>& s, double mean) {
        if (exact) { ExactSqDev a(mean); for (double x : s) a.add(x); return a.value(); }
        double ss = 0; for (double x : s) ss += (x - mean) * (x - mean); return ss;
    };

    std::vector<double> out_col(n_, NAd);
    int64_t g0 = 0;
    while (g0 < n_) {
        int64_t g1 = g0 + 1;
        while (g1 < n_ && (by_cols.empty() || key_eq(K, order[g1], order[g0]))) ++g1;
        // gather non-missing v over the group
        std::vector<double> s;
        s.reserve(g1 - g0);
        for (int64_t q = g0; q < g1; ++q) { double x = v[order[q]]; if (!std::isnan(x)) s.push_back(x); }
        const int64_t cnt = static_cast<int64_t>(s.size());

        // Per-row (non-broadcast) functions: write each row of the group individually.
        if (fn == EgenFn::Rank || fn == EgenFn::RankTrack || fn == EgenFn::RankField ||
            fn == EgenFn::RankUnique) {
            // The four egen rank() flavours, all off ONE ascending stable sort of the
            // group's non-missing values. Missing input => missing rank in every one.
            // Oracle captures b and c on 10,10,20,30,30,30,40 (+3 missing):
            //   default    10 -> 1.5   30 -> 5     mean/midrank over the ranks spanned
            //   track      10 -> 1     30 -> 4     minimum rank, shared by every tied
            //                                      member (oracle_c: obs1 == obs2 == 1)
            //   field      10 -> 6     30 -> 2     descending competition rank
            //   unique     10 -> 1, and the SECOND 10 -> 2 (oracle_c is the capture
            //                                      that separates unique from track)
            std::vector<std::pair<double, int64_t>> vr;
            vr.reserve(cnt);
            for (int64_t q = g0; q < g1; ++q) { double x = v[order[q]]; if (!std::isnan(x)) vr.emplace_back(x, order[q]); }
            std::stable_sort(vr.begin(), vr.end(),
                             [](const std::pair<double, int64_t>& a, const std::pair<double, int64_t>& b) { return a.first < b.first; });
            int64_t m = static_cast<int64_t>(vr.size());
            if (fn == EgenFn::RankUnique) {
                // Sequential 1..m in ascending order; the stable sort leaves tied rows
                // in OBSERVATION order, which is what breaks the tie.
                for (int64_t t = 0; t < m; ++t) out_col[vr[t].second] = static_cast<double>(t + 1);
                g0 = g1;
                continue;
            }
            int64_t i = 0;
            while (i < m) {
                int64_t j = i + 1;
                while (j < m && vr[j].first == vr[i].first) ++j;  // [i,j) is a tie block
                double rank;
                switch (fn) {
                    // ranks are (i+1)..(j) inclusive (1-based); mean = (i+1 + j)/2
                    case EgenFn::Rank: rank = (static_cast<double>(i + 1) + static_cast<double>(j)) / 2.0; break;
                    case EgenFn::RankTrack: rank = static_cast<double>(i + 1); break;
                    // field: 1 + (number of values strictly GREATER) = m - j + 1.
                    default: rank = static_cast<double>(m - j + 1); break;
                }
                for (int64_t t = i; t < j; ++t) out_col[vr[t].second] = rank;
                i = j;
            }
            g0 = g1;
            continue;
        }
        if (fn == EgenFn::Std) {
            // Standardize: (x - groupmean)/groupSd, sample SD (N-1). Missing rows stay
            // missing; if SD is 0 or <2 non-missing, all rows missing (Stata behavior).
            double agg_std = NAd;  // group SD
            double mean = NAd;
            if (cnt >= 2) {
                mean = sum_of(s) / cnt;
                agg_std = std::sqrt(ssq_of(s, mean) / (cnt - 1));
            }
            // std(): sd*(x - mean)/s + mean, in that order (the defaults
            // sd 1 and mean 0 leave (x - mean)/s bit-for-bit).
            for (int64_t q = g0; q < g1; ++q) {
                double x = v[order[q]];
                if (!std::isnan(x) && !std::isnan(agg_std) && agg_std != 0.0)
                    out_col[order[q]] = eo.std_sd * (x - mean) / agg_std + eo.std_mean;
                // else leave missing
            }
            g0 = g1;
            continue;
        }
        if (fn == EgenFn::Pc) {
            // pc(): p*x divided by the group's sum of non-missing values (plain
            // and by() agree on this).
            // A zero total divides by zero, which is Stata's missing.
            const double t = sum_of(s);
            const double p = eo.pc_prop ? 1.0 : 100.0;
            for (int64_t q = g0; q < g1; ++q) {
                const double x = v[order[q]];
                if (!std::isnan(x) && t != 0.0) out_col[order[q]] = p * x / t;
            }
            g0 = g1;
            continue;
        }

        // Broadcast (scalar-per-group) functions.
        double agg = NAd;
        switch (fn) {
            case EgenFn::Count: agg = static_cast<double>(cnt); break;
            // 0 if empty; `missing` makes an empty group missing .
            case EgenFn::Sum: {
                if (eo.missing && cnt == 0) break;
                agg = sum_of(s); break;
            }
            case EgenFn::Max: if (cnt) agg = *std::max_element(s.begin(), s.end()); break;
            case EgenFn::Min: if (cnt) agg = *std::min_element(s.begin(), s.end()); break;
            case EgenFn::Mean: if (cnt) agg = sum_of(s) / cnt; break;
            case EgenFn::Sd:
                if (cnt >= 2) {
                    const double mean = sum_of(s) / cnt;
                    agg = std::sqrt(ssq_of(s, mean) / (cnt - 1));
                }
                break;
            // egenmore `var` — the SAME sum-of-squares as Sd without the root, so the
            // two can never disagree about the divisor or the singleton rule. Oracle
            // capture c: var(r) == summarize's r(Var)
            // == %21x +1.0124924924925X+007 == 900/7.
            case EgenFn::Var:
                if (cnt >= 2) {
                    const double mean = sum_of(s) / cnt;
                    agg = ssq_of(s, mean) / (cnt - 1);
                }
                break;
            case EgenFn::Median:
                if (cnt) { std::sort(s.begin(), s.end()); agg = stata_pctile(s, 50.0); }
                break;
            case EgenFn::Pctile:
                if (cnt) { std::sort(s.begin(), s.end()); agg = stata_pctile(s, pctile_p); }
                break;
            case EgenFn::Iqr:
                if (cnt) { std::sort(s.begin(), s.end());
                           agg = stata_pctile(s, 75.0) - stata_pctile(s, 25.0); }
                break;
            case EgenFn::Rank: case EgenFn::Std: case EgenFn::Pc:
            case EgenFn::RankTrack: case EgenFn::RankField: case EgenFn::RankUnique:
                break;  // handled above
        }
        for (int64_t q = g0; q < g1; ++q) out_col[order[q]] = agg;
        g0 = g1;
    }
    set_or_replace(out, std::move(out_col), st);
}

// egen tag(varlist): 1 on the first obs (current data order) of each distinct key
// combination, 0 elsewhere; missing-key rows get missing. Distinct from group():
// tag scans in DATA order and marks the first occurrence of each unseen key.
void Frame::egen_tag(const std::string& out, const std::vector<std::string>& by_cols) {
    if (by_cols.empty()) throw std::runtime_error("egen tag(): requires a varlist");
    std::vector<std::vector<double>> K = cols_as_double(*this, n_, by_cols);
    // Stable-sort row indices by key to detect the first row of each key group, but
    // mark in the ORIGINAL data order: a key is "tagged" at its lowest original index.
    std::vector<int64_t> order(n_);
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(),
                     [&](int64_t a, int64_t b) {
                         if (key_less(K, a, b)) return true;
                         if (key_less(K, b, a)) return false;
                         return a < b;  // tie-break by original index → lowest index first
                     });
    std::vector<double> out_col(n_, 0.0);
    int64_t g0 = 0;
    while (g0 < n_) {
        int64_t g1 = g0 + 1;
        while (g1 < n_ && key_eq(K, order[g1], order[g0])) ++g1;
        // Is this group's key missing in any component? → tag missing for all its rows.
        bool key_missing = false;
        for (const auto& col : K) if (std::isnan(col[order[g0]])) { key_missing = true; break; }
        if (key_missing) {
            for (int64_t q = g0; q < g1; ++q) out_col[order[q]] = NAd;
        } else {
            // first row of the group in ORIGINAL order = the smallest original index
            int64_t first = order[g0];
            for (int64_t q = g0 + 1; q < g1; ++q) if (order[q] < first) first = order[q];
            out_col[first] = 1.0;
        }
        g0 = g1;
    }
    set_or_replace(out, std::move(out_col));
}

// egen group(varlist): dense 1..K over sorted distinct key combinations. Missing-key
// rows get missing (Stata default) unless missing_as_group (missings sort last and
// form their own group ids).
void Frame::egen_group(const std::string& out, const std::vector<std::string>& by_cols,
                       bool missing_as_group, StoreType st) {
    if (by_cols.empty()) throw std::runtime_error("egen group(): requires a varlist");
    // Stata `egen group()` numbers levels in SORTED value order — strings
    // alphabetically, numerics ascending (C1-F8 / golden G7). For a factor
    // (string) by-column the sort key must be the LABEL's rank, NOT the stored
    // first-seen factor code that cols_as_double() would hand back. Build the
    // keys here so a factor code maps to its label's alphabetical rank; missing
    // (code < 0) keeps its negative sentinel (unchanged missing semantics).
    std::vector<std::vector<double>> K;
    K.reserve(by_cols.size());
    for (const auto& nm : by_cols) {
        const FrameColumn& c = col(nm);
        std::vector<double> v(n_);
        if (c.is_factor) {
            std::vector<int> perm(c.levels.size());
            std::iota(perm.begin(), perm.end(), 0);
            std::stable_sort(perm.begin(), perm.end(),
                             [&](int a, int b) { return c.levels[a] < c.levels[b]; });
            std::vector<int> rank(c.levels.size());
            for (int r = 0; r < static_cast<int>(perm.size()); ++r) rank[perm[r]] = r;
            for (int64_t i = 0; i < n_; ++i) {
                int32_t cd = c.codes[i];
                v[i] = (cd < 0) ? static_cast<double>(cd) : static_cast<double>(rank[cd]);
            }
        } else {
            for (int64_t i = 0; i < n_; ++i) v[i] = c.f64[i];
        }
        K.push_back(std::move(v));
    }
    std::vector<int64_t> order(n_);
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(),
                     [&](int64_t a, int64_t b) { return key_less(K, a, b); });  // NaN sorts last
    std::vector<double> out_col(n_, NAd);
    int64_t g0 = 0;
    int64_t gid = 0;
    while (g0 < n_) {
        int64_t g1 = g0 + 1;
        while (g1 < n_ && key_eq(K, order[g1], order[g0])) ++g1;
        bool key_missing = false;
        for (const auto& col : K) if (std::isnan(col[order[g0]])) { key_missing = true; break; }
        if (key_missing && !missing_as_group) {
            // leave the whole group as missing, do NOT consume a group id
        } else {
            ++gid;
            for (int64_t q = g0; q < g1; ++q) out_col[order[q]] = static_cast<double>(gid);
        }
        g0 = g1;
    }
    set_or_replace(out, std::move(out_col), st);
}

// egen seq() ([D] egen, Example 7): a sawtooth over a group's rows. Every `block` rows
// the value moves one step, it wraps after covering the closed range between from() and
// to(), and it runs downward when from() is the larger bound. to() defaults to _N.
void Frame::egen_seq(const std::string& out, const std::vector<std::string>& by_cols,
                     double from, double to, bool has_to, int64_t block) {
    if (block < 1) {
        throw EngineError("bad_input",
                          "egen seq(): block(" + std::to_string(block) + ") must be at least 1");
    }
    const double stop = has_to ? to : static_cast<double>(n_);
    const double smaller = std::min(from, stop);
    const double larger = std::max(from, stop);
    const int64_t wrap_after = static_cast<int64_t>(larger - smaller) + 1;

    // A stable sort on the keys walks each group in the frame's current order.
    const auto keys = cols_as_double(*this, n_, by_cols);
    std::vector<int64_t> walk(n_);
    std::iota(walk.begin(), walk.end(), 0);
    if (!keys.empty()) {
        std::stable_sort(walk.begin(), walk.end(),
                         [&](int64_t a, int64_t b) { return key_less(keys, a, b); });
    }

    std::vector<double> sawtooth(n_);
    int64_t rows_into_group = 0;
    for (int64_t j = 0; j < n_; ++j) {
        if (j > 0 && !key_eq(keys, walk[j], walk[j - 1])) rows_into_group = 0;
        const double offset = static_cast<double>((rows_into_group / block) % wrap_after);
        sawtooth[walk[j]] = from <= stop ? smaller + offset : larger - offset;
        ++rows_into_group;
    }
    set_or_replace(out, std::move(sawtooth));
}

// BUG-1002: egen mode() over a STRING source. Stata's egen mode runs the same
// frequency count for a string, with "" as the missing value, and orders the
// text by bytes: maxmode takes the largest tied mode, minmode the smallest ("" -- the
// smallest string -- sorts last in that order, so under `missing` it wins a tie),
// nummode(k) the k-th smallest; a plain tie stores "". The answer is the source's
// own text. Oracle w2ap_egen_family eg_mode_byp_str (`bys g:
// egen mo11 = mode(s), maxmode`: a a a a a a | b b b b | c | c c c | d d d d | e e).
void Frame::egen_mode_text(const std::string& out, const FrameColumn& src,
                           const std::vector<int64_t>& order,
                           const std::vector<std::vector<double>>& K, bool one_group,
                           const ModeOpts& mo) {
    auto text_of = [&](int64_t r) -> const std::string& {
        static const std::string kEmpty;
        const int32_t k = src.codes[r];
        return k >= 0 ? src.levels[static_cast<size_t>(k)] : kEmpty;
    };
    std::vector<std::string> answer_of(static_cast<size_t>(n_));
    int64_t max_modes_any_group = 0;
    int64_t g0 = 0;
    while (g0 < n_) {
        int64_t g1 = g0 + 1;
        while (g1 < n_ && (one_group || key_eq(K, order[g1], order[g0]))) ++g1;
        std::map<std::string, int64_t> freq;  // byte order, "" first
        for (int64_t q = g0; q < g1; ++q) {
            const std::string& t = text_of(order[q]);
            if (mo.missing_as_value || !t.empty()) ++freq[t];
        }
        std::string answer;
        if (!freq.empty()) {
            int64_t best = 0;
            for (const auto& f : freq) best = std::max(best, f.second);
            std::vector<const std::string*> modes;  // ascending
            for (const auto& f : freq) if (f.second == best) modes.push_back(&f.first);
            max_modes_any_group = std::max(max_modes_any_group,
                                           static_cast<int64_t>(modes.size()));
            if (mo.nummode > 0) {
                if (static_cast<size_t>(mo.nummode) <= modes.size())
                    answer = *modes[static_cast<size_t>(mo.nummode) - 1];
            } else if (mo.minmode) {
                answer = *modes.front();
            } else if (mo.maxmode) {
                answer = *modes.back();
            } else if (modes.size() == 1) {
                answer = *modes.front();
            }
        }
        for (int64_t q = g0; q < g1; ++q) answer_of[static_cast<size_t>(order[q])] = answer;
        g0 = g1;
    }
    if (mo.nummode > 0 && max_modes_any_group > 0 &&
        static_cast<int64_t>(mo.nummode) > max_modes_any_group)
        throw EngineError("bad_input",
                          "egen mode(): nummode(" + std::to_string(mo.nummode) +
                          ") is greater than the number of modes in every group; the largest "
                          "number of modes in a group is " +
                          std::to_string(max_modes_any_group));
    std::vector<int32_t> codes(static_cast<size_t>(n_));
    std::vector<std::string> levels;
    std::unordered_map<std::string, int32_t> dict;
    for (size_t i = 0; i < answer_of.size(); ++i) {
        auto it = dict.find(answer_of[i]);
        if (it == dict.end()) {
            const int32_t k = static_cast<int32_t>(levels.size());
            dict.emplace(answer_of[i], k);
            levels.push_back(answer_of[i]);
            codes[i] = k;
        } else {
            codes[i] = it->second;
        }
    }
    set_or_replace_factor(out, std::move(codes), std::move(levels));
}

// egen mode(x) [, by() minmode|maxmode|nummode(#) missing] — B4/W6.
// Matches Stata's egen mode(), method identified from its ado and held by the oracle:
// it counts each value's frequency within
// the by-group, zeroes the frequency of missing unless `missing` is given, and then
// picks among the values that share the maximum frequency. minmode/maxmode/nummode()
// are opts_exclusive there and here. In the ado's sorts missing orders LAST ascending
// (Stata's `. > any number`), so it is the largest candidate for maxmode and never the
// smallest for minmode — reproduced below by sorting NaN to the end.
//
// Oracle capture b. On t = 1,1,2,2 and six missings, whose modes TIE:
//   default -> missing everywhere · maxmode -> 2 · minmode -> 1 · missing -> `.` itself
//              wins with frequency 6, so the answer is missing.
// On m = 1,1,2,2,3,3,3,4,4,. (a unique mode) -> 3.
void Frame::egen_mode(const std::string& out, const std::string& expr,
                      const std::vector<std::string>& by_cols, const ModeOpts& mo) {
    RngCallScope rng_call;
    int excl = (mo.minmode ? 1 : 0) + (mo.maxmode ? 1 : 0) + (mo.nummode > 0 ? 1 : 0);
    if (excl > 1)
        throw EngineError("bad_input",
                          "egen mode(): minmode, maxmode and nummode() are mutually exclusive");
    if (mo.nummode < 0)
        throw EngineError("bad_input", "egen mode(): nummode() must be a positive integer");
    std::vector<int64_t> order(n_);
    std::iota(order.begin(), order.end(), 0);
    std::vector<std::vector<double>> K;
    if (!by_cols.empty()) {
        K = cols_as_double(*this, n_, by_cols);
        std::stable_sort(order.begin(), order.end(),
                         [&](int64_t a, int64_t b) { return key_less(K, a, b); });
    }
    const size_t b0 = expr.find_first_not_of(" \t");
    const std::string bare =
        b0 == std::string::npos ? std::string() : expr.substr(b0, expr.find_last_not_of(" \t") - b0 + 1);
    if (has(bare) && col(bare).is_factor) {
        egen_mode_text(out, col(bare), order, K, by_cols.empty(), mo);
        return;
    }
    Parser parser(expr, *this);
    auto ast = parser.parse();
    std::vector<double> v = eval_column(ast.get(), n_);
    // NaN sorts last — see the header comment. std::sort's comparator must be a strict
    // weak ordering, and `a < b` with NaN operands is not, so it is spelled out.
    auto less_nan_last = [](double a, double b) {
        if (std::isnan(a)) return false;
        if (std::isnan(b)) return true;
        return a < b;
    };
    std::vector<double> out_col(n_, NAd);
    int64_t max_modes_any_group = 0;
    int64_t g0 = 0;
    while (g0 < n_) {
        int64_t g1 = g0 + 1;
        while (g1 < n_ && (by_cols.empty() || key_eq(K, order[g1], order[g0]))) ++g1;
        std::vector<double> s;
        s.reserve(g1 - g0);
        for (int64_t q = g0; q < g1; ++q) {
            double x = v[order[q]];
            if (mo.missing_as_value || !std::isnan(x)) s.push_back(x);
        }
        std::sort(s.begin(), s.end(), less_nan_last);
        // runs of equal values -> (value, frequency); NaNs form one trailing run.
        std::vector<std::pair<double, int64_t>> freq;
        for (size_t i = 0; i < s.size();) {
            size_t j = i + 1;
            while (j < s.size() &&
                   ((std::isnan(s[i]) && std::isnan(s[j])) || s[j] == s[i])) ++j;
            freq.emplace_back(s[i], static_cast<int64_t>(j - i));
            i = j;
        }
        double answer = NAd;
        if (!freq.empty()) {
            int64_t best = 0;
            for (const auto& f : freq) best = std::max(best, f.second);
            std::vector<double> modes;  // already ascending, NaN last
            for (const auto& f : freq) if (f.second == best) modes.push_back(f.first);
            max_modes_any_group = std::max(max_modes_any_group,
                                           static_cast<int64_t>(modes.size()));
            if (mo.nummode > 0) {
                if (static_cast<size_t>(mo.nummode) <= modes.size())
                    answer = modes[static_cast<size_t>(mo.nummode) - 1];
                // else: fewer modes than asked for -> missing (Stata warns and does
                // the same); NOT an error unless EVERY group is short (checked below).
            } else if (mo.minmode) {
                answer = modes.front();
            } else if (mo.maxmode) {
                answer = modes.back();
            } else if (modes.size() == 1) {
                answer = modes.front();
            }
            // plain mode with a tie -> missing (Stata also warns)
        }
        for (int64_t q = g0; q < g1; ++q) out_col[order[q]] = answer;
        g0 = g1;
    }
    // Stata: r(198) when nummode() exceeds
    // the mode count of EVERY group (egen-six captured `nummode(4)` at rc 198 with
    // "The largest number of modes in a group is 2.").
    if (mo.nummode > 0 && max_modes_any_group > 0 &&
        static_cast<int64_t>(mo.nummode) > max_modes_any_group)
        throw EngineError("bad_input",
                          "egen mode(): nummode(" + std::to_string(mo.nummode) +
                          ") is greater than the number of modes in every group; the largest "
                          "number of modes in a group is " +
                          std::to_string(max_modes_any_group));
    set_or_replace(out, std::move(out_col));
}

// egen nvals(varlist) [, by() missing] — egenmore, ACCEPTED-NAMED (see frame.h).
// The number of DISTINCT values, or of distinct combinations for a multi-variable
// varlist. Oracle capture c: nvals(r) = 4 over 10,10,20,30,30,30,40;
// the egen-six capture's nvals(g v) = 5 is the varlist form, and nvals(s) = 5 shows
// it counts a string variable's distinct values too (a factor here).
void Frame::egen_nvals(const std::string& out, const std::vector<std::string>& vars,
                       const std::vector<std::string>& by_cols, bool missing_as_value,
                       StoreType st) {
    if (vars.empty()) throw EngineError("bad_input", "egen nvals(): requires a varlist");
    // A factor column's CODE identifies its level, so cols_as_double is a faithful
    // key for both kinds (missing is NaN for f64 and a negative code for a factor —
    // normalized to NaN here so one missing test covers both).
    std::vector<std::vector<double>> V;
    V.reserve(vars.size());
    for (const auto& nm : vars) {
        const FrameColumn& c = col(nm);
        std::vector<double> x(n_);
        for (int64_t i = 0; i < n_; ++i)
            x[i] = c.is_factor ? (c.codes[i] < 0 ? NAd : static_cast<double>(c.codes[i]))
                               : c.f64[i];
        V.push_back(std::move(x));
    }
    std::vector<int64_t> order(n_);
    std::iota(order.begin(), order.end(), 0);
    std::vector<std::vector<double>> K;
    if (!by_cols.empty()) {
        K = cols_as_double(*this, n_, by_cols);
        std::stable_sort(order.begin(), order.end(),
                         [&](int64_t a, int64_t b) { return key_less(K, a, b); });
    }
    std::vector<double> out_col(n_, NAd);
    int64_t g0 = 0;
    while (g0 < n_) {
        int64_t g1 = g0 + 1;
        while (g1 < n_ && (by_cols.empty() || key_eq(K, order[g1], order[g0]))) ++g1;
        std::vector<std::vector<double>> rows;
        rows.reserve(g1 - g0);
        for (int64_t q = g0; q < g1; ++q) {
            int64_t r = order[q];
            bool any_missing = false;
            std::vector<double> key(V.size());
            for (size_t k = 0; k < V.size(); ++k) {
                key[k] = V[k][r];
                if (std::isnan(key[k])) any_missing = true;
            }
            if (any_missing && !missing_as_value) continue;
            rows.push_back(std::move(key));
        }
        // NaN-safe lexicographic ordering (NaN last), then count the distinct runs.
        auto key_cmp = [](const std::vector<double>& a, const std::vector<double>& b) {
            for (size_t k = 0; k < a.size(); ++k) {
                bool na = std::isnan(a[k]), nb = std::isnan(b[k]);
                if (na && nb) continue;
                if (na) return false;
                if (nb) return true;
                if (a[k] < b[k]) return true;
                if (a[k] > b[k]) return false;
            }
            return false;
        };
        std::sort(rows.begin(), rows.end(), key_cmp);
        int64_t distinct = 0;
        for (size_t i = 0; i < rows.size(); ++i)
            if (i == 0 || key_cmp(rows[i - 1], rows[i])) ++distinct;
        double answer = rows.empty() ? NAd : static_cast<double>(distinct);
        for (int64_t q = g0; q < g1; ++q) out_col[order[q]] = answer;
        g0 = g1;
    }
    // egenmore's nvals builds the count as a running sum. Under float the last running
    // sum is stored once, so the count is rounded once; an integer type WIDENS under
    // replace (w2e_float_default replace_byte_overflow, replace_int_overflow),
    // so the count is exact.
    if (frame_detail::is_integer_store(st)) st = StoreType::Double;
    set_or_replace(out, std::move(out_col), st);
}

// ---- Stata's %w.0g, and string()/strofreal() on top of it (B4/W7) ----------
// Contract, the cell-by-cell derivation and the one UNPINNED edge are documented at
// the declaration in frame.h. Oracle: B4 oracle capture g2.
std::string stata_format_g(double v, int width) {
    if (std::isnan(v)) return ".";          // g2 `missing` cell: string(.) = "."
    if (width < 7) width = 7;               // below w7 the budget degenerates; no cell pins it
    const int S = width - 2;                // significant-digit budget: 7/8/10 at w9/w10/w12
    if (v == 0.0) return "0";               // catches -0.0 too (-0.0 == 0.0)
    const bool neg = std::signbit(v);
    const double a = std::fabs(v);
    char buf[64];
    // Round to S significant digits FIRST, then read the decimal exponent off the
    // rounded value — so a carry (9.9999e6 -> 1.0e7 at S=7) moves the branch too.
    std::snprintf(buf, sizeof buf, "%.*e", S - 1, a);
    const char* ep = std::strchr(buf, 'e');
    const int e = ep ? std::atoi(ep + 1) : 0;
    std::string s;
    if (e >= S || e < -5) {
        int md = S - 5;                     // mantissa DECIMALS = budget-5 (budget-4 sig digits)
        if (md < 0) md = 0;
        std::snprintf(buf, sizeof buf, "%.*e", md, a);
        s = buf;
    } else {
        const int n = (e >= 0) ? (e + 1) : 0;   // digits left of the point ("" for |x|<1)
        int d = S - n;
        if (d < 0) d = 0;
        std::snprintf(buf, sizeof buf, "%.*f", d, a);
        s = buf;
        if (s.find('.') != std::string::npos) {   // strip trailing zeros, then a bare "."
            size_t last = s.find_last_not_of('0');
            if (s[last] == '.') --last;
            s.erase(last + 1);
        }
        if (s.size() >= 2 && s[0] == '0' && s[1] == '.') s.erase(0, 1);  // .5, never 0.5
    }
    return neg ? "-" + s : s;
}

std::string stata_string_of(double v, const std::string& fmt) {
    if (fmt.empty()) return stata_format_g(v, 9);   // g2: the default IS %9.0g
    // Parse %[w][.d]<conv>. Only the two forms the captures pin are accepted.
    size_t p = 0;
    if (p < fmt.size() && fmt[p] == '%') ++p;
    const size_t wst = p;
    while (p < fmt.size() && std::isdigit(static_cast<unsigned char>(fmt[p]))) ++p;
    const std::string ws = fmt.substr(wst, p - wst);
    std::string ds;
    if (p < fmt.size() && fmt[p] == '.') {
        ++p;
        const size_t dst = p;
        while (p < fmt.size() && std::isdigit(static_cast<unsigned char>(fmt[p]))) ++p;
        ds = fmt.substr(dst, p - dst);
    }
    const std::string tail = fmt.substr(p);
    const int w = ws.empty() ? 9 : std::stoi(ws);
    const int d = ds.empty() ? 0 : std::stoi(ds);
    if (tail == "g" && d == 0) return stata_format_g(v, w);
    if (tail == "f") {
        // Missing prints as "." under any numeric format. NOT blank-padded to w:
        // egen-six `len_f92` = 4 for string(3,"%9.2f"), fn.pdf p.171 agrees.
        if (std::isnan(v)) return ".";
        char buf[512];
        std::snprintf(buf, sizeof buf, "%.*f", d, v);
        return buf;
    }
    throw EngineError("unsupported",
        "string()/egen concat format(): only %w.0g and %w.Nf are supported — '" + fmt +
        "' has no captured Stata rendering in this build, and guessing one would put a "
        "silently wrong value in a join key");
}

namespace {

// The bytes of a string column's cell; "" for a missing (negative) code.
const std::string& factor_text(const FrameColumn& c, int64_t row) {
    static const std::string kNothing;
    const int32_t code = c.codes[row];
    return code < 0 ? kNothing : c.levels[static_cast<std::size_t>(code)];
}

// string(x[, format]) of one number. stata_string_of writes every missing as ".", while
// Stata's own text for an extended missing names its letter (.a).
std::string numeric_field(double x, const std::string& format) {
    const int kind = missing_kind(x);
    if (kind > kMissSys) return std::string(".") + static_cast<char>('a' + kind - kMissA);
    return stata_string_of(x, format);
}

std::string trim_blanks(const std::string& text) {
    const std::size_t first = text.find_first_not_of(' ');
    if (first == std::string::npos) return std::string();
    return text.substr(first, text.find_last_not_of(' ') - first + 1);
}

// The largest i with x >= edges[i] for edges in non-decreasing order, so among tied
// edges the higher index wins; -1 below the first edge.
int64_t interval_of(const std::vector<double>& edges, double x) {
    return (std::upper_bound(edges.begin(), edges.end(), x) - edges.begin()) - 1;
}

// Row functions over one row's cells, given in variable order. `kept_as` is the type the
// running value is held in at every step: under Float a partial sum or an interim extreme
// past float's range becomes missing right there (measured, StataBE 19.5).
double row_mean_of(const std::vector<double>& cells) {
    double sum = 0.0;
    int64_t count = 0;
    for (double x : cells) {
        if (std::isnan(x)) continue;
        sum += x;
        ++count;
    }
    return count == 0 ? NAd : sum / static_cast<double>(count);
}

// The textbook two-pass sample deviation: deviations from the row mean are squared and
// added in variable order, which is what StataBE's bits agree with.
double row_sd_of(const std::vector<double>& cells) {
#if defined(__clang__)
#pragma clang fp contract(off)  // a fused multiply-add here changes the last bit
#endif
    const double centre = row_mean_of(cells);
    double squares = 0.0;
    int64_t count = 0;
    for (double x : cells) {
        if (std::isnan(x)) continue;
        const double gap = x - centre;
        squares += gap * gap;
        ++count;
    }
    return count < 2 ? NAd : std::sqrt(squares / static_cast<double>(count - 1));
}

// `blank_when_empty` is rowtotal's missing option: an all-missing row stays missing, and
// a sum knocked out of range restarts at the next value. Without it the sum starts at 0
// and missing absorbs whatever follows.
double row_total_of(const std::vector<double>& cells, bool blank_when_empty, StoreType kept_as) {
    double total = blank_when_empty ? NAd : 0.0;
    for (double x : cells) {
        if (std::isnan(x)) continue;
        total = round_to_store(blank_when_empty && std::isnan(total) ? x : total + x, kept_as);
    }
    return total;
}

// A missing running value yields to the next cell even when that cell is missing too,
// so rowmax(., .a) is .a; only a real number can then be replaced by a larger one.
double row_max_of(const std::vector<double>& cells, StoreType kept_as) {
    double top = round_to_store(cells.front(), kept_as);
    for (std::size_t j = 1; j < cells.size(); ++j) {
        const double x = cells[j];
        if (std::isnan(top) || x > top) top = round_to_store(x, kept_as);
    }
    return top;
}

double row_min_of(const std::vector<double>& cells, StoreType kept_as) {
    double bottom = NAd;
    for (double x : cells) {
        if (std::isnan(x)) continue;
        if (std::isnan(bottom) || x < bottom) bottom = round_to_store(x, kept_as);
    }
    return bottom;
}

}  // namespace

// egen concat(): each variable's text with `punct` after it, the separator after the
// last field taken off again, then blanks trimmed from both ends of the result. An
// empty last field therefore leaves the separator before it in place.
void Frame::egen_concat(const std::string& out, const std::vector<std::string>& vars,
                        const ConcatOpts& co) {
    if (vars.empty()) throw EngineError("bad_input", "egen concat(): requires a varlist");
    if (co.decode) {
        throw EngineError("unsupported",
                          "egen concat(): decode is not available, engine columns carry no "
                          "value labels");
    }
    if (co.maxlength != 0 && (co.maxlength < 1 || co.maxlength > 32000))
        throw EngineError("bad_input", "egen concat(): invalid maxlength( )");

    std::vector<const FrameColumn*> sources;
    sources.reserve(vars.size());
    for (const std::string& name : vars) sources.push_back(&col(name));

    std::vector<int32_t> codes(n_);
    std::vector<std::string> levels;
    std::unordered_map<std::string, int32_t> code_of;
    for (int64_t row = 0; row < n_; ++row) {
        std::string text;
        for (const FrameColumn* source : sources) {
            text += source->is_factor ? factor_text(*source, row)
                                      : numeric_field(source->f64[row], co.format);
            text += co.punct;
        }
        text.resize(text.size() - co.punct.size());
        text = trim_blanks(text);
        // "" gets a level like any other text: an all-empty concat is "", not missing.
        const auto seen = code_of.emplace(std::move(text), static_cast<int32_t>(levels.size()));
        if (seen.second) levels.push_back(seen.first->first);
        codes[row] = seen.first->second;
    }
    set_or_replace_factor(out, std::move(codes), std::move(levels));
}

// egen cut(): left-closed intervals between ascending edges. group(k) takes its edges
// from the data: the minimum, the k-1 inner quantiles by [D] pctile's default definition
// (p = 100*i/k), and the maximum plus one. The result is an edge or, under icodes, an
// interval index; `label` gives a string column whose levels name every edge.
void Frame::egen_cut(const std::string& out, const std::string& var, const CutOpts& co,
                     StoreType st) {
    const bool from_quantiles = co.group != 0;
    if (co.at.empty() && !from_quantiles)
        throw EngineError("bad_input", "egen cut(): one of at() or group() must be specified");
    if (!co.at.empty() && from_quantiles)
        throw EngineError("bad_input", "egen cut(): cannot specify both at() and group() option");
    for (std::size_t j = 1; j < co.at.size(); ++j) {
        if (!(co.at[j - 1] < co.at[j]))
            throw EngineError("bad_input", "egen cut(): at() invalid -- elements out of order");
    }
    const FrameColumn& source = col(var);
    if (source.is_factor) {
        throw EngineError("bad_input",
                          "egen cut(): string variable used where a number is required: " + var);
    }
    const F64Reader x = source.f64.reader();

    std::vector<double> edges = co.at;
    if (from_quantiles) {
        if (co.group < 2) throw EngineError("bad_input", "egen cut(): group() must be >= 2");
        std::vector<double> ascending;
        ascending.reserve(n_);
        for (int64_t row = 0; row < n_; ++row) {
            if (!std::isnan(x[row])) ascending.push_back(x[row]);
        }
        if (ascending.empty()) {
            throw EngineError("bad_input",
                              "egen cut(): " + var + " has no non-missing values");
        }
        std::sort(ascending.begin(), ascending.end());
        edges.push_back(ascending.front());
        for (int i = 1; i < co.group; ++i) {
            edges.push_back(stata_pctile(ascending, 100.0 * i / co.group));
        }
        edges.push_back(ascending.back() + 1.0);
    }

    // A value at or past the last at() edge has no interval; group()'s top edge lies
    // above every value, so this test never fires for it.
    const auto bin_of = [&](double v) -> int64_t {
        if (std::isnan(v) || (!from_quantiles && v >= edges.back())) return -1;
        return interval_of(edges, v);
    };

    if (co.label) {
        std::vector<int32_t> codes(n_);
        for (int64_t row = 0; row < n_; ++row) codes[row] = static_cast<int32_t>(bin_of(x[row]));
        std::vector<std::string> names;
        names.reserve(edges.size());
        for (double edge : edges) names.push_back(stata_string_of(edge, "") + "-");
        set_or_replace_factor(out, std::move(codes), std::move(names));
        return;
    }

    const bool as_index = co.icodes || from_quantiles;
    std::vector<double> stored(n_, NAd);
    for (int64_t row = 0; row < n_; ++row) {
        const int64_t bin = bin_of(x[row]);
        if (bin >= 0) stored[row] = as_index ? static_cast<double>(bin) : edges[bin];
    }
    if (!is_integer_store(st)) {
        set_or_replace(out, std::move(stored), st);
        return;
    }
    for (double v : stored) {
        if (!std::isnan(v) && (v != std::trunc(v) || v < kStoreLongLo || v > kStoreLongHi)) {
            throw EngineError("bad_input",
                              "egen cut(): --store-type byte|int|long with a non-integer cut "
                              "point is not supported (Stata's replace widens the variable)");
        }
    }
    set_or_replace(out, std::move(stored));
}

// The row functions (help egen). Cells are read in the order the variables were written;
// a string column is a value only to rowmiss and to rownonmiss with strok, where its
// text being "" is what missing means.
void Frame::row_egen(const std::string& out, RowFn fn, const std::vector<std::string>& cols,
                     StoreType st, bool missing, bool strok) {
    if (cols.empty()) throw EngineError("bad_input", "egen row function: empty varlist");
    const bool counts = fn == RowFn::Miss || fn == RowFn::NonMiss;
    const bool folds_while_storing = fn == RowFn::Total || fn == RowFn::Max || fn == RowFn::Min;
    if (is_integer_store(st) && folds_while_storing) {
        const char* const shown = fn == RowFn::Total ? "rowtotal"
                                  : fn == RowFn::Max ? "rowmax"
                                                     : "rowmin";
        throw EngineError("bad_input",
                          std::string("egen ") + shown + "(): --store-type byte|int|long is not "
                          "supported (Stata builds it with replace, which widens an integer "
                          "variable)");
    }
    const bool strings_allowed = fn == RowFn::Miss || (fn == RowFn::NonMiss && strok);
    std::vector<const FrameColumn*> sources;
    sources.reserve(cols.size());
    for (const std::string& name : cols) {
        const FrameColumn& source = col(name);
        if (source.is_factor && !strings_allowed) {
            throw EngineError("bad_input",
                              "egen row function: string variable used where a number is "
                              "required: " + name);
        }
        sources.push_back(&source);
    }

    std::vector<double> cells(sources.size());
    std::vector<double> result(n_);
    for (int64_t row = 0; row < n_; ++row) {
        if (counts) {
            int64_t blanks = 0;
            for (const FrameColumn* source : sources) {
                const bool blank = source->is_factor ? factor_text(*source, row).empty()
                                                     : std::isnan(source->f64[row]);
                blanks += blank ? 1 : 0;
            }
            result[row] = static_cast<double>(fn == RowFn::Miss
                                                  ? blanks
                                                  : static_cast<int64_t>(sources.size()) - blanks);
            continue;
        }
        for (std::size_t j = 0; j < sources.size(); ++j) cells[j] = sources[j]->f64[row];
        switch (fn) {
            case RowFn::Mean: result[row] = row_mean_of(cells); break;
            case RowFn::Sd: result[row] = row_sd_of(cells); break;
            case RowFn::Total: result[row] = row_total_of(cells, missing, st); break;
            case RowFn::Max: result[row] = row_max_of(cells, st); break;
            default: result[row] = row_min_of(cells, st); break;
        }
    }
    // Mean and sd are computed in double and rounded once on store; the folds already hold
    // their storage type, and the counts are exact whatever was declared.
    set_or_replace(out, std::move(result),
                   (fn == RowFn::Mean || fn == RowFn::Sd) ? st : StoreType::Double);
}

namespace {
// Shared by both xtile spellings: group = 1 + (number of cutpoints strictly below
// the value), missing in -> missing out. Keeping this in one place is what makes
// `nquantiles()` and `cutpoints()` provably the same command with a different
// source of edges.
std::vector<double> xtile_assign(const double* x, int64_t n, const std::vector<double>& cut) {
    std::vector<double> o(static_cast<size_t>(n), NAd);
    for (int64_t i = 0; i < n; ++i) {
        if (std::isnan(x[i])) continue;
        int g = 1;
        for (double c : cut) if (x[i] > c) ++g;
        o[static_cast<size_t>(i)] = static_cast<double>(g);
    }
    return o;
}
}  // namespace

void Frame::xtile(const std::string& out, const std::string& var, int nq, const std::string& wvar) {
    const double* x = require_f64(var);
    if (nq < 2) throw std::runtime_error("xtile: nq must be >= 2");
    std::vector<double> cut;
    if (wvar.empty()) {
        std::vector<double> sorted;
        for (int64_t i = 0; i < n_; ++i) if (!std::isnan(x[i])) sorted.push_back(x[i]);
        std::sort(sorted.begin(), sorted.end());
        for (int i = 1; i < nq; ++i) cut.push_back(stata_pctile(sorted, 100.0 * i / nq));
        set_or_replace(out, xtile_assign(x, n_, cut));
        return;
    }
    const double* w = require_f64(wvar);
    std::vector<std::pair<double, double>> vw;
    std::vector<double> xs(x, x + n_);
    for (int64_t i = 0; i < n_; ++i) {
        if (std::isnan(w[i]) || !(w[i] > 0.0)) { xs[static_cast<size_t>(i)] = NAd; continue; }
        if (!std::isnan(x[i])) vw.emplace_back(x[i], w[i]);
    }
    std::stable_sort(vw.begin(), vw.end(),
                     [](const std::pair<double, double>& a, const std::pair<double, double>& b) {
                         return a.first < b.first;
                     });
    std::vector<double> pv, pw;
    ExactSum W;
    for (const auto& p : vw) { pv.push_back(p.first); pw.push_back(p.second); W.add(p.second); }
    for (int i = 1; i < nq; ++i) cut.push_back(stata_pctile_weighted(pv, pw, W.value(), 100.0 * i / nq));
    set_or_replace(out, xtile_assign(xs.data(), n_, cut));
}

void Frame::xtile_cuts(const std::string& out, const std::string& var,
                       const std::string& cutvar) {
    const double* x = require_f64(var);
    const double* c = require_f64(cutvar);
    // The cutpoints ARE the non-missing values of cutvar, deduplicated and sorted.
    // Stata reads them the same way, which is why a cutpoints() variable is
    // normally short and padded with missings.
    std::vector<double> cut;
    for (int64_t i = 0; i < n_; ++i) if (!std::isnan(c[i])) cut.push_back(c[i]);
    std::sort(cut.begin(), cut.end());
    cut.erase(std::unique(cut.begin(), cut.end()), cut.end());
    if (cut.empty())
        throw std::runtime_error("xtile: cutpoints(" + cutvar + ") has no non-missing values");
    set_or_replace(out, xtile_assign(x, n_, cut));
}

void Frame::ipolate(const std::string& out, const std::string& yvar, const std::string& xvar,
                    bool epolate, const std::vector<std::string>& by) {
    const double* y = require_f64(yvar);
    const double* x = require_f64(xvar);
    std::vector<int64_t> order(n_);
    std::iota(order.begin(), order.end(), 0);
    std::vector<std::vector<double>> K;
    if (!by.empty()) {
        K = cols_as_double(*this, n_, by);
        std::stable_sort(order.begin(), order.end(),
                         [&](int64_t a, int64_t b) { return key_less(K, a, b); });
    }
    std::vector<double> o(n_, NAd);
    int64_t g0 = 0;
    while (g0 < n_) {
        int64_t g1 = g0 + 1;
        while (g1 < n_ && (by.empty() || key_eq(K, order[g1], order[g0]))) ++g1;
        // anchors: (x,y) both non-missing, one per DISTINCT x — Stata averages y across
        // tied x values before building the interpolation grid.
        std::vector<std::pair<double, double>> pts;
        for (int64_t q = g0; q < g1; ++q) {
            int64_t r = order[q];
            if (!std::isnan(x[r]) && !std::isnan(y[r])) pts.emplace_back(x[r], y[r]);
        }
        std::sort(pts.begin(), pts.end());
        std::vector<std::pair<double, double>> anc;
        for (size_t p = 0; p < pts.size(); ) {
            size_t e = p + 1;
            double sum = pts[p].second;
            while (e < pts.size() && pts[e].first == pts[p].first) { sum += pts[e].second; ++e; }
            anc.emplace_back(pts[p].first, sum / static_cast<double>(e - p));
            p = e;
        }
        for (int64_t q = g0; q < g1; ++q) {
            int64_t r = order[q];
            if (!std::isnan(y[r])) { o[r] = y[r]; continue; }  // observed y copies through regardless of x
            if (std::isnan(x[r])) continue;        // can't place a missing-y row without x
            if (anc.empty()) continue;
            double xr = x[r];
            // locate first anchor with x >= xr
            size_t hi = 0;
            while (hi < anc.size() && anc[hi].first < xr) ++hi;
            if (hi < anc.size() && anc[hi].first == xr) { o[r] = anc[hi].second; continue; }
            if (hi == 0) {  // below the range
                if (epolate && anc.size() >= 2) {
                    auto &a = anc[0], &b = anc[1];
                    o[r] = a.second + (b.second - a.second) * (xr - a.first) / (b.first - a.first);
                }
            } else if (hi == anc.size()) {  // above the range
                if (epolate && anc.size() >= 2) {
                    auto &a = anc[anc.size() - 2], &b = anc[anc.size() - 1];
                    o[r] = a.second + (b.second - a.second) * (xr - a.first) / (b.first - a.first);
                }
            } else {  // interpolate between anc[hi-1] and anc[hi]
                auto &a = anc[hi - 1], &b = anc[hi];
                o[r] = a.second + (b.second - a.second) * (xr - a.first) / (b.first - a.first);
            }
        }
        g0 = g1;
    }
    set_or_replace(out, std::move(o));
}

std::vector<std::string> Frame::mkspline_cubic(const std::string& stub, const std::string& var,
                                               int nknots, std::vector<double> knots) {
    const double* xp = require_f64(var);
    std::vector<double> x(xp, xp + n_);  // stable copy (set_or_replace may grow cols_)

    std::vector<double> tk;
    if (!knots.empty()) {
        tk = std::move(knots);  // user-supplied knots: used as-is, no clamping
    } else {
        std::vector<double> s;
        s.reserve(n_);
        for (int64_t i = 0; i < n_; ++i) if (!std::isnan(x[i])) s.push_back(x[i]);
        std::sort(s.begin(), s.end());
        const int m = static_cast<int>(s.size());
        if (m < 5) throw std::runtime_error("mkspline: too few non-missing observations");
        // Stata centile (default (n+1)p interpolation)
        auto cent = [&](double p) -> double {
            double R = (m + 1.0) * p / 100.0;
            int r = static_cast<int>(std::floor(R));
            double fr = R - r;
            if (r < 1) return s.front();
            if (r >= m) return s.back();
            return s[r - 1] + fr * (s[r] - s[r - 1]);
        };
        static const std::vector<double> P3{10, 50, 90}, P4{5, 35, 65, 95},
            P5{5, 27.5, 50, 72.5, 95}, P6{5, 23, 41, 59, 77, 95},
            P7{2.5, 18.33, 34.17, 50, 65.83, 81.67, 97.5};
        const std::vector<double>* pp = nullptr;
        switch (nknots) {
            case 3: pp = &P3; break; case 4: pp = &P4; break; case 5: pp = &P5; break;
            case 6: pp = &P6; break; case 7: pp = &P7; break;
            default: throw std::runtime_error("mkspline: nknots must be 3-7 for default knots");
        }
        for (double p : *pp) tk.push_back(cent(p));
        // outer-knot boundary clamping (default placement only)
        double mn = cent(5.0 / (m + 1) * 100.0), mx = cent((m - 4.0) / (m + 1) * 100.0);
        if (tk.front() < mn) tk.front() = mn;
        if (tk.back() > mx) tk.back() = mx;
    }

    const int nk = static_cast<int>(tk.size());
    if (nk < 3) throw std::runtime_error("mkspline: need at least 3 knots");
    if (!(tk[0] < tk[1]) || !(tk[nk - 1] > tk[nk - 2]))
        throw std::runtime_error("mkspline: sample size too small for this many knots");
    const double t1 = tk[0], tnk = tk[nk - 1], tkm1 = tk[nk - 2];
    const double span2 = (tnk - t1) * (tnk - t1);

    std::vector<std::string> names;
    set_or_replace(stub + "1", x);  // first term = raw x (linear)
    names.push_back(stub + "1");
    for (int j = 1; j <= nk - 2; ++j) {
        const double tj = tk[j - 1];
        const double c2 = (tnk - tj) / (tnk - tkm1);
        const double c3 = (tkm1 - tj) / (tnk - tkm1);
        std::vector<double> v(n_);
        for (int64_t i = 0; i < n_; ++i) {
            double xi = x[i];
            if (std::isnan(xi)) { v[i] = NAd; continue; }
            double uj = xi - tj;     if (uj < 0) uj = 0;
            double ukm1 = xi - tkm1; if (ukm1 < 0) ukm1 = 0;
            double unk = xi - tnk;   if (unk < 0) unk = 0;
            v[i] = (uj * uj * uj - ukm1 * ukm1 * ukm1 * c2 + unk * unk * unk * c3) / span2;
        }
        std::string nm = stub + std::to_string(j + 1);
        set_or_replace(nm, std::move(v));
        names.push_back(nm);
    }
    return names;
}

void Frame::bygen(const std::string& out, const std::string& expr,
                  const std::vector<std::string>& by_cols,
                  const std::vector<std::string>& sort_cols,
                  const std::string& mask_expr, StoreType st, const GenQualifier& qual) {
    RngCallScope rng_call;
    check_gen_qualifier(out, qual);
    // Stata's `in` is a range of the dataset's observations, which a by-group or a
    // sort key would renumber; PIET sends a range only with neither.
    if (qual.in_first != 0 && !(by_cols.empty() && sort_cols.empty()))
        throw EngineError("bad_input", "--gen-in: an `in` range cannot be combined with by() "
                                       "or sort() in --bygen");
    Parser parser(expr, *this);
    auto ast = parser.parse();
    bind_group_range(ast.get(), qual.in_first, qual.in_last);
    std::unique_ptr<Node> mast;
    if (!mask_expr.empty()) {
        Parser mp(mask_expr, *this);
        mast = mp.parse();
        bind_group_range(mast.get(), qual.in_first, qual.in_last);
    }

    std::vector<std::string> sortkeys = by_cols;
    sortkeys.insert(sortkeys.end(), sort_cols.begin(), sort_cols.end());
    std::vector<std::vector<double>> KS = cols_as_double(*this, n_, sortkeys);
    std::vector<std::vector<double>> KB = cols_as_double(*this, n_, by_cols);

    std::vector<int64_t> order(n_);
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(),
                     [&](int64_t a, int64_t b) { return key_less(KS, a, b); });

    std::vector<int64_t> g0a(n_), g1a(n_), posa(n_);
    int64_t q = 0;
    while (q < n_) {
        int64_t r = q + 1;
        while (r < n_ && (by_cols.empty() || key_eq(KB, order[r], order[q]))) ++r;
        for (int64_t t = q; t < r; ++t) {
            int64_t row = order[t];
            g0a[row] = q; g1a[row] = r; posa[row] = t - q + 1;
        }
        q = r;
    }

    EvalCtx c;
    c.n_all = n_;
    c.order = &order;

    // The `if` of a by-prefixed command is evaluated inside each by-group, like the
    // expression: its _n, _N, x[1], x[_N], x[_n-1] and x[_n+1] count within the
    // group (w2m_bymask A01: `by gg: generate a01 = sum(_n) if _n > 1` answers
    // . 2 5 | . 2 5). It used to be evaluated over the dataset before the groups
    // existed, which answered . 2 5 | 1 3 6 at rc 0 (BUG-836). A sum() inside it
    // runs per group, as it does inside the folded cond() form.
    // A replace walks the rows in place (see has_self_subscript below), and an
    // `if` that reads the target through a subscript must see the rows already
    // replaced: `replace y = runiform() + 1 if y[_n-1] != 0` after `replace y = 1
    // in 1` replaces every row in StataBE 19.5. A drawing right-hand side sends
    // its `if` here as the mask, so that mask is evaluated inline, row by row,
    // against the buffer being written.
    const double* target = nullptr;
    if (auto it = idx_.find(out); it != idx_.end() && !cols_[it->second].is_factor)
        target = static_cast<const FrameColumn&>(cols_[it->second]).f64.data();
    const bool self_mask = target && mast && has_self_subscript(mast.get(), target) &&
                           !has_fn(ast.get(), F_sum);
    std::vector<double> mask;
    const bool inline_if = self_mask || inline_if_draws(mast.get(), ast.get());
    if (mast) {
        materialize_sums(mast.get(), n_, &order, &g0a, &g1a, &posa, nullptr);
        mask.assign(static_cast<size_t>(n_), 1.0);
        for (int64_t i = 0; i < n_ && !inline_if; ++i) {
            c.row = i; c.g0 = g0a[i]; c.g1 = g1a[i]; c.pos = posa[i];
            mask[static_cast<size_t>(i)] = ev(mast.get(), c);
        }
    }
    apply_in_range(mask, n_, qual.in_first, qual.in_last);
    // Whether row c.row is selected; under inline_if its `if` is evaluated now.
    auto selects = [&](const EvalCtx& rc) {
        double& m = mask[static_cast<size_t>(rc.row)];
        if (inline_if && if_selects(m)) m = ev(mast.get(), rc);
        return if_selects(m);
    };

    // sum() restarts at every group boundary and walks in SORT order, so it is
    // materialised here where that order exists -- not row by row below, which
    // iterates in original order.
    materialize_sums(ast.get(), n_, &order, &g0a, &g1a, &posa,
                     mask.empty() ? nullptr : &mask);

    // Row-sequential replace (see has_self_subscript): walk the observations in
    // the by-sort order, as Stata's bysort processes them, so `by g: replace x =
    // x[_n-1] + 1 if _n > 1` counts up within each group.
    if (target) {
        if (has_self_subscript(ast.get(), target) || self_mask) {
            F64Reader old = cols_[idx_.at(out)].f64.reader();
            std::vector<double> buf(static_cast<size_t>(n_));
            for (int64_t i = 0; i < n_; ++i) buf[static_cast<size_t>(i)] = old[static_cast<size_t>(i)];
            rebind_var(ast.get(), target, buf.data());
            if (self_mask) rebind_var(mast.get(), target, buf.data());
            for (int64_t t = 0; t < n_; ++t) {
                const int64_t i = order[static_cast<size_t>(t)];
                c.row = i; c.g0 = g0a[i]; c.g1 = g1a[i]; c.pos = posa[i];
                if (!mask.empty() && !selects(c)) continue;
                double v = ev(ast.get(), c);
                buf[static_cast<size_t>(i)] = st == StoreType::Double ? v : store_round(v, st);
            }
            store_qualified(out, std::move(buf), mask, st, qual);
            return;
        }
    }
    std::vector<double> out_col(n_, NAd);
    // On Stata's stream an unselected row draws nothing (see Frame::generate).
    const bool skip_unselected = g_mt64_on && !mask.empty() && has_draw(ast.get());
    for (int64_t i = 0; i < n_; ++i) {
        c.row = i; c.g0 = g0a[i]; c.g1 = g1a[i]; c.pos = posa[i];
        if (skip_unselected && !selects(c)) continue;
        out_col[i] = ev(ast.get(), c);
    }
    store_qualified(out, std::move(out_col), mask, st, qual);
}

Frame::Summary Frame::summarize(const std::string& var, bool detail, const std::string& wvar,
                                bool analytic, const std::vector<char>* keep, bool importance) const {
    // A row outside `keep` is treated exactly as a row with a missing value.
    auto out = [keep](int64_t i) { return keep != nullptr && !(*keep)[static_cast<size_t>(i)]; };
    // W:P5: weighted detail/percentiles now land (stata_pctile_weighted, above)
    // for BOTH weight kinds — the previous honest reject was a real gap, not a
    // semantic one: `summarize [aw=w] var, detail` is a legal Stata command and
    // the answerbook pins its full table.
    // A string/factor variable has no numeric observations. Stata's `summarize` on a string
    // variable SUCCEEDS with r(N)=0 (every value is numerically missing) — it is NOT an error —
    // so mirror that instead of throwing require_f64's "is a factor" runtime_error.
    if (col(var).is_factor) {
        Summary sf; sf.detail = detail;
        sf.mean = sf.sd = sf.var = sf.min = sf.max = NAd;
        if (detail) {
            sf.p1 = sf.p5 = sf.p10 = sf.p25 = sf.p50 = sf.p75 = sf.p90 = sf.p95 = sf.p99 = NAd;
            sf.skewness = sf.kurtosis = NAd;
        }
        return sf;
    }
    const double* x = require_f64(var);
    Summary s;
    s.detail = detail;
    std::vector<double> vals;
    // BUG-1013 (FU-1190): r(sum), and r(mean) = r(sum)/N, are Stata's exactly
    // accumulated sum rounded once (core/exact_sum.h), not a data-order double
    // sum: 1e16 + 22 ones - 1e16 is 22 in StataBE 19.5 and was 0 here
    // (w2as_summary_stats_family sx_sum_acc3 / acc4 / su_acc5 / *_mo).
    ExactSum acc;
    s.min = INF; s.max = -INF;
    if (wvar.empty()) {
        // ---- unweighted path ----
        for (int64_t i = 0; i < n_; ++i) {
            if (std::isnan(x[i]) || out(i)) continue;
            ++s.n;
            acc.add(x[i]);
            s.min = std::min(s.min, x[i]);
            s.max = std::max(s.max, x[i]);
            if (detail) vals.push_back(x[i]);
        }
        // No non-missing observations: Stata posts r(N)=0 but does NOT post r(mean)/r(sd)
        // (they read as missing) — emit missing rather than the struct-default 0.0.
        if (s.n == 0) { s.mean = s.sd = s.var = s.min = s.max = NAd; return s; }
        const double sum = acc.value();
        s.sum = sum;
        // BUG-344: Σw with w≡1 IS the observation count, and that is exactly what Stata
        // posts in r(sum_w) on an unweighted summarize. Leaving the struct default (0)
        // here made "unweighted" indistinguishable from "no weight total", so the only
        // honest value a caller could publish was none at all.
        s.sum_w = static_cast<double>(s.n);
        s.mean = sum / s.n;
        if (s.n >= 2) {
            // The squared deviations accumulate exactly too (core/exact_sum.h
            // ExactSqDev): a standardized variable's near-zero mean takes its
            // sign from r(sd)'s last bits (AER 007's control means printed
            // -0.000 for Stata's 0.000).
            ExactSqDev sq(s.mean);
            for (int64_t i = 0; i < n_; ++i)
                if (!std::isnan(x[i]) && !out(i)) sq.add(x[i]);
            const double ss = sq.value();
            s.var = ss / (s.n - 1);
            s.sd = std::sqrt(s.var);
        } else {
            s.var = s.sd = NAd;
        }
        if (detail) {
            std::sort(vals.begin(), vals.end());
            s.p1 = stata_pctile(vals, 1);   s.p5 = stata_pctile(vals, 5);
            s.p10 = stata_pctile(vals, 10); s.p25 = stata_pctile(vals, 25);
            s.p50 = stata_pctile(vals, 50); s.p75 = stata_pctile(vals, 75);
            s.p90 = stata_pctile(vals, 90); s.p95 = stata_pctile(vals, 95);
            s.p99 = stata_pctile(vals, 99);
            // population moments for skewness/kurtosis (Stata convention)
            double m2 = 0, m3 = 0, m4 = 0;
            for (double v : vals) { double d = v - s.mean; m2 += d * d; m3 += d * d * d; m4 += d * d * d * d; }
            m2 /= s.n; m3 /= s.n; m4 /= s.n;
            double sp = std::sqrt(m2);
            s.skewness = sp > 0 ? m3 / (sp * sp * sp) : NAd;
            s.kurtosis = m2 > 0 ? m4 / (m2 * m2) : NAd;
        }
        return s;
    }
    // ---- weighted path (Stata fweight): N=Sum(w), mean=Sum(w*x)/Sum(w), sd denom Sum(w)-1 ----
    const double* w = require_f64(wvar);
    // Validate the WHOLE weight column up front (Stata checks fweight integrality over the
    // sample regardless of which other variable ends up missing on a given row): any non-missing
    // weight must be a non-negative integer, else honest bad_input (never silently rounds).
    // BUG-107: a NEGATIVE weight is a hard error too (Stata "negative weights encountered") —
    // previously only integrality was checked, so w=-1 silently made sumwx go negative → a
    // reported mean/sum OUTSIDE the data range.
    for (int64_t i = 0; i < n_; ++i) {
        if (std::isnan(w[i]) || importance) continue;
        if (w[i] < 0.0)
            throw EngineError("bad_input",
                "summarize: " + std::string(analytic ? "aweight" : "fweight") + " '" + wvar +
                "' has a negative value (" + std::to_string(w[i]) +
                "); " + (analytic ? "analytic" : "frequency") + " weights must be non-negative", wvar);
        // Integrality is a frequency-weight rule only: an analytic weight is a
        // precision/importance scale and 1.5 is a perfectly legal value.
        if (!analytic && w[i] != std::floor(w[i]))
            throw EngineError("bad_input",
                "summarize: fweight '" + wvar + "' has a non-integer value (" +
                std::to_string(w[i]) + "); Stata requires integer frequency weights", wvar);
    }
    ExactSum accw, accwx;  // BUG-1013: the weighted sums accumulate as the unweighted one
    int64_t nrows = 0;
    // W:P5 detail: collect the (value, weight) pairs the moment loop below
    // would otherwise have to re-derive, so percentiles and the weighted
    // moments read the SAME included-row set as N/mean/var.
    std::vector<std::pair<double, double>> wvals;
    for (int64_t i = 0; i < n_; ++i) {
        // A missing weight drops the row outright (as if absent from the dataset). A missing `x`
        // excludes the row from N/mean/var (its already-validated weight contributes nowhere).
        // BUG-107: a zero weight = zero frequency = the obs is not in the dataset, so it must be
        // excluded from min/max too (it already contributes nothing to sumw/sumwx/N).
        if (std::isnan(w[i]) || std::isnan(x[i]) || w[i] == 0.0 || out(i)) continue;
        ++nrows;
        accw.add(w[i]);
        accwx.add(w[i] * x[i]);
        s.min = std::min(s.min, x[i]);
        s.max = std::max(s.max, x[i]);
        if (detail) wvals.emplace_back(x[i], w[i]);
    }
    const double sumw = accw.value(), sumwx = accwx.value();
    if (sumw == 0.0) { s.n = 0; s.mean = s.sd = s.var = s.min = s.max = NAd; return s; }
    // The one place the two weight kinds genuinely diverge: a frequency weight IS the
    // observation count, an analytic weight is a relative precision over `nrows` real
    // observations. Stata prints the former as Obs and the latter as Obs + Weight.
    s.n = (analytic || importance) ? nrows : static_cast<int64_t>(std::llround(sumw));
    s.sum_w = sumw;
    s.sum = sumwx;
    s.mean = sumwx / sumw;
    // Denominator: fw divides by Sum(w)-1 (the replicated sample size); aw rescales the
    // weights to sum to n first, so the divisor is n-1 on the ORIGINAL observation count.
    const double vscale = analytic ? static_cast<double>(nrows) / sumw : 1.0;
    const double vdenom = analytic ? static_cast<double>(nrows) - 1.0 : sumw - 1.0;
    if (vdenom >= 1.0) {
        double ss = 0;
        for (int64_t i = 0; i < n_; ++i) {
            if (std::isnan(w[i]) || std::isnan(x[i]) || out(i)) continue;
            double d = x[i] - s.mean;
            ss += w[i] * vscale * d * d;
        }
        s.var = ss / vdenom;
        s.sd = std::sqrt(s.var);
    } else {
        s.var = s.sd = NAd;
    }
    if (detail) {
        std::sort(wvals.begin(), wvals.end(),
                  [](const std::pair<double, double>& a, const std::pair<double, double>& b) {
                      return a.first < b.first;
                  });
        std::vector<double> pv, pw;
        pv.reserve(wvals.size());
        pw.reserve(wvals.size());
        for (const auto& vw : wvals) { pv.push_back(vw.first); pw.push_back(vw.second); }
        s.p1 = stata_pctile_weighted(pv, pw, sumw, 1);
        s.p5 = stata_pctile_weighted(pv, pw, sumw, 5);
        s.p10 = stata_pctile_weighted(pv, pw, sumw, 10);
        s.p25 = stata_pctile_weighted(pv, pw, sumw, 25);
        s.p50 = stata_pctile_weighted(pv, pw, sumw, 50);
        s.p75 = stata_pctile_weighted(pv, pw, sumw, 75);
        s.p90 = stata_pctile_weighted(pv, pw, sumw, 90);
        s.p95 = stata_pctile_weighted(pv, pw, sumw, 95);
        s.p99 = stata_pctile_weighted(pv, pw, sumw, 99);
        // Weighted population moments about the weighted mean — the same
        // Stata convention the unweighted branch uses, with the row count
        // replaced by the weight total. Both weight kinds share this: the
        // fw/aw divergence lives in the VARIANCE denominator (above), not in
        // these shape statistics, which are scale-free ratios.
        double m2 = 0, m3 = 0, m4 = 0;
        for (const auto& vw : wvals) {
            const double d = vw.first - s.mean;
            const double dd = d * d;
            m2 += vw.second * dd;
            m3 += vw.second * dd * d;
            m4 += vw.second * dd * dd;
        }
        m2 /= sumw; m3 /= sumw; m4 /= sumw;
        const double sp = std::sqrt(m2);
        s.skewness = sp > 0 ? m3 / (sp * sp * sp) : NAd;
        s.kurtosis = m2 > 0 ? m4 / (m2 * m2) : NAd;
    }
    return s;
}

namespace {

// Weighted percentiles of one collapse group, per [R] summarize, Methods and formulas:
// invert the weighted empirical distribution, averaging two neighbouring order
// statistics when the cumulative weight sits on the target. Measured on StataBE
// (collapse with each weight type) beyond what the manual states: the weights stay in
// raw units, aweights included; a negative total (iweights only) flips every sign; a
// zero total has no percentile; the target comes from the total as accumulated in
// dataset order, so the caller hands in that sum rather than a re-summed one.
class GroupWeightedPercentiles {
public:
    GroupWeightedPercentiles(const std::vector<double>& values, const std::vector<double>& weights,
                             double total_in_dataset_order)
        : total_(std::fabs(total_in_dataset_order)) {
        const double sign = total_in_dataset_order < 0.0 ? -1.0 : 1.0;
        std::vector<std::pair<double, double>> by_value(values.size());
        for (size_t i = 0; i < values.size(); ++i) by_value[i] = {values[i], sign * weights[i]};
        std::sort(by_value.begin(), by_value.end());  // inside a tie the order cannot change a result
        ascending_.reserve(by_value.size());
        running_.reserve(by_value.size());
        double accumulated = 0.0;
        for (const auto& [value, weight] : by_value) {
            accumulated += weight;
            ascending_.push_back(value);
            running_.push_back(accumulated);
        }
    }

    bool defined() const { return total_ != 0.0 && !ascending_.empty(); }

    // Requires defined().
    double at(double percent) const {
        constexpr double kOnTargetBelow = 1e-6;  // raw weight units; only below the target
        const double target = total_ * percent / 100.0;
        // Mixed-sign iweights can make running_ non-monotone: scan rather than bisect.
        // No cumulative above the target can happen only through rounding: take the last.
        auto past = std::find_if(running_.begin(), running_.end(),
                                 [target](double cumulative) { return cumulative > target; });
        if (past == running_.end()) --past;
        const size_t rank = static_cast<size_t>(past - running_.begin());
        if (rank > 0 && target - running_[rank - 1] <= kOnTargetBelow)
            return (ascending_[rank - 1] + ascending_[rank]) / 2.0;
        return ascending_[rank];
    }

private:
    double total_;
    std::vector<double> ascending_;  // the values, sorted
    std::vector<double> running_;    // running_[i] = cumulative weight through ascending_[i]
};

}  // namespace

Frame Frame::collapse(const std::vector<CollapseSpec>& specs,
                      const std::vector<std::string>& by, const std::string& wvar,
                      const std::string& wtype_in, bool stata_by_order) const {
    // Weight kind: "" with a weight variable is the historical fweight path.
    const std::string wtype = wvar.empty() ? std::string() : (wtype_in.empty() ? std::string("fw") : wtype_in);
    if (!wtype.empty() && wtype != "fw" && wtype != "aw" && wtype != "iw" && wtype != "pw")
        throw EngineError("bad_input", "collapse: unknown weight type '" + wtype + "'", wvar);
    const bool fw = wtype == "fw";
    std::vector<int64_t> order(n_);
    std::iota(order.begin(), order.end(), 0);
    std::vector<std::vector<double>> K;
    // by-columns may be factor OR numeric: K casts a factor by-column to its int32
    // code as a double (cols_as_double). Under stata_by_order a factor by-column
    // groups and sorts by its TEXT instead (Stata's `sort by`: "" first, then
    // byte order; w2au_collapse_family.log cz_c_bystr_v1c `[]|[a]|[c]|[d]|[eee]`),
    // and "" and a missing code are the same group.
    std::vector<char> by_is_factor(by.size());
    for (size_t bc = 0; bc < by.size(); ++bc) by_is_factor[bc] = col(by[bc]).is_factor ? 1 : 0;
    if (!by.empty()) {
        K = cols_as_double(*this, n_, by);
        if (stata_by_order) {
            for (size_t bc = 0; bc < by.size(); ++bc) {
                if (!by_is_factor[bc]) continue;
                const FrameColumn& fc = col(by[bc]);
                const std::vector<std::string>& lv = fc.levels.ref();
                std::vector<int32_t> idx(lv.size());
                std::iota(idx.begin(), idx.end(), 0);
                std::sort(idx.begin(), idx.end(), [&](int32_t a, int32_t b) { return lv[a] < lv[b]; });
                std::vector<double> rank(lv.size(), -1.0);
                double r = 0.0;
                for (size_t k = 0; k < idx.size(); ++k) {
                    if (lv[idx[k]].empty()) { rank[idx[k]] = -1.0; continue; }
                    if (k > 0 && !lv[idx[k - 1]].empty() && lv[idx[k - 1]] == lv[idx[k]]) {
                        rank[idx[k]] = rank[idx[k - 1]];
                        continue;
                    }
                    rank[idx[k]] = r;
                    r += 1.0;
                }
                for (int64_t i = 0; i < n_; ++i) {
                    const double c = K[bc][i];
                    K[bc][i] = c < 0 ? -1.0 : rank[static_cast<size_t>(c)];
                }
            }
        }
    }
    // A string (factor) spec column serves first/last/firstnm/lastnm only
    // (Stata refuses the rest with r(109); PIET checks first).
    auto is_string_stat = [](const std::string& st) {
        return st == "first" || st == "last" || st == "firstnm" || st == "lastnm";
    };
    std::vector<char> spec_is_factor(specs.size(), 0);
    std::vector<const double*> vp;
    std::vector<const int32_t*> vc;
    for (size_t si = 0; si < specs.size(); ++si) {
        const auto& s = specs[si];
        if (has(s.var) && col(s.var).is_factor) {
            if (!is_string_stat(s.statistic))
                throw EngineError("bad_input", "collapse: string variable " + s.var +
                                                   " not allowed with statistic " + s.statistic, s.var);
            spec_is_factor[si] = 1;
            vp.push_back(nullptr);
            vc.push_back(col(s.var).codes.data());
        } else {
            vp.push_back(require_f64(s.var));
            vc.push_back(nullptr);
        }
    }
    // The weight (optional). Grouping itself is NOT affected by weights, but an
    // observation with a missing or zero weight is marked out before the groups
    // form (Stata's collapse marks them out), so it enters no statistic.
    const double* wcol = nullptr;
    if (!wvar.empty()) {
        wcol = require_f64(wvar);
        for (int64_t i = 0; i < n_; ++i) {
            if (std::isnan(wcol[i])) continue;
            if (wcol[i] < 0.0 && wtype != "iw")
                throw EngineError("bad_input",
                    "collapse: " + std::string(fw ? "fweight" : (wtype == "aw" ? "aweight" : "pweight")) +
                    " '" + wvar + "' has a negative value (" +
                    std::to_string(wcol[i]) + "); frequency weights must be non-negative", wvar);
            if (fw && wcol[i] != std::floor(wcol[i]))
                throw EngineError("bad_input",
                    "collapse: fweight '" + wvar + "' has a non-integer value (" +
                    std::to_string(wcol[i]) + "); Stata requires integer frequency weights", wvar);
        }
        if (!fw) {
            std::vector<int64_t> kept;
            kept.reserve(order.size());
            for (int64_t i : order)
                if (!std::isnan(wcol[i]) && wcol[i] != 0.0) kept.push_back(i);
            order.swap(kept);
        }
    }
    const int64_t nact = static_cast<int64_t>(order.size());
    if (!by.empty()) {
        std::stable_sort(order.begin(), order.end(),
                         [&](int64_t a, int64_t b) { return key_less(K, a, b); });
    }

    // group boundaries over `order`
    std::vector<std::pair<int64_t, int64_t>> groups;
    int64_t g0 = 0;
    while (g0 < nact) {
        int64_t g1 = g0 + 1;
        while (g1 < nact && (by.empty() || key_eq(K, order[g1], order[g0]))) ++g1;
        groups.emplace_back(g0, g1);
        g0 = g1;
    }
    int64_t G = static_cast<int64_t>(groups.size());

    std::vector<std::pair<std::string, std::vector<double>>> outcols;
    for (size_t b = 0; b < by.size(); ++b) outcols.push_back({by[b], std::vector<double>(G, NAd)});
    for (const auto& s : specs) outcols.push_back({s.out.empty() ? s.var : s.out, std::vector<double>(G, NAd)});

    static const std::unordered_set<std::string> kAggKnown = {
        "sum", "count", "n", "max", "min", "mean", "sd",
        "first", "last", "firstnm", "lastnm",
        "semean", "sebinomial", "sepoisson", "sepoisson_nocap", "rawsum", "percent", "iqr"};
    // MAXPERF Stage 3 — exception hoist: pre-validate each spec on the caller (the
    // group loop runs on ThreadPool workers, where a throw would std::terminate).
    auto pctile_of = [](const std::string& st, double* pp) -> bool {
        if (st == "median" || st == "p50") { *pp = 50.0; return true; }
        if (st.size() > 1 && st[0] == 'p') {
            const std::string pstr = st.substr(1);
            size_t consumed = 0;
            try { *pp = std::stod(pstr, &consumed); }
            catch (const std::exception&) { return false; }
            return consumed == pstr.size();
        }
        return false;
    };
    if (G > 0) {
        for (const auto& s : specs) {
            const std::string& st = s.statistic;
            double pp;
            if (kAggKnown.count(st) || pctile_of(st, &pp)) continue;
            throw std::runtime_error("collapse: unknown stat '" + st + "'");
        }
    }
    // percent's denominator: the whole sample's nonmissing count (or weight) of
    // the spec's variable (Stata's collapse, _percent).
    std::vector<double> pct_total(specs.size(), 0.0);
    for (size_t si = 0; si < specs.size(); ++si) {
        if (specs[si].statistic != "percent" || spec_is_factor[si]) continue;
        double t = 0.0;
        for (int64_t q = 0; q < nact; ++q) {
            const int64_t r = order[q];
            if (std::isnan(vp[si][r])) continue;
            if (wcol != nullptr) {
                if (std::isnan(wcol[r]) || wcol[r] == 0.0) continue;
                t += wcol[r];
            } else {
                t += 1.0;
            }
        }
        pct_total[si] = t;
    }

    // MAXPERF Stage 3 — morsel-parallel collapse, BY GROUP (bit-identical to
    // serial: each group is aggregated whole by one worker, in order[a..b)).
    auto collapse_body = [&](int64_t glo, int64_t ghi, int) {
      for (int64_t gi = glo; gi < ghi; ++gi) {
        int64_t a = groups[gi].first, b = groups[gi].second;
        for (size_t bc = 0; bc < by.size(); ++bc)
            outcols[bc].second[gi] = by_is_factor[bc] ? static_cast<double>(col(by[bc]).codes[order[a]])
                                                      : K[bc][order[a]];
        for (size_t si = 0; si < specs.size(); ++si) {
            const std::string& st = specs[si].statistic;
            double agg = NAd;
            if (spec_is_factor[si]) {
                // A string: the code of the first/last (nonmissing) row; "" is missing.
                const FrameColumn& fc = col(specs[si].var);
                auto is_missing = [&](int64_t r) {
                    const int32_t c = vc[si][r];
                    return c < 0 || fc.levels[static_cast<size_t>(c)].empty();
                };
                auto included = [&](int64_t r) {
                    return wcol == nullptr || (!std::isnan(wcol[r]) && wcol[r] != 0.0);
                };
                int64_t pick = -1;
                if (st == "first" || st == "firstnm") {
                    for (int64_t q = a; q < b; ++q) {
                        const int64_t r = order[q];
                        if (!included(r)) continue;
                        if (pick < 0) pick = r;
                        if (st == "first" || !is_missing(r)) { pick = r; break; }
                    }
                } else {
                    for (int64_t q = b; q > a; --q) {
                        const int64_t r = order[q - 1];
                        if (!included(r)) continue;
                        if (pick < 0) pick = r;
                        if (st == "last" || !is_missing(r)) { pick = r; break; }
                    }
                }
                agg = (pick < 0 || is_missing(pick)) ? -1.0 : static_cast<double>(vc[si][pick]);
                outcols[by.size() + si].second[gi] = agg;
                continue;
            }
            double pp = 0.0;
            if (wvar.empty()) {
                // ---- unweighted path: existing statistics BYTE-UNCHANGED ----
                std::vector<double> s;
                for (int64_t q = a; q < b; ++q) { double x = vp[si][order[q]]; if (!std::isnan(x)) s.push_back(x); }
                const int64_t cnt = static_cast<int64_t>(s.size());
                if (st == "sum") { double t = 0; for (double x : s) t += x; agg = t; }
                else if (st == "rawsum") { double t = 0; for (double x : s) t += x; agg = t; }
                else if (st == "count" || st == "n") agg = static_cast<double>(cnt);
                else if (st == "percent") { if (pct_total[si] > 0) agg = (static_cast<double>(cnt) / pct_total[si]) * 100.0; }
                else if (st == "max") { if (cnt) agg = *std::max_element(s.begin(), s.end()); }
                else if (st == "min") { if (cnt) agg = *std::min_element(s.begin(), s.end()); }
                else if (st == "mean") { if (cnt) { double t = 0; for (double x : s) t += x; agg = t / cnt; } }
                else if (st == "sd" || st == "semean") {
                    if (cnt >= 2) { double t = 0; for (double x : s) t += x; double mn = t / cnt;
                        double ss = 0; for (double x : s) ss += (x - mn) * (x - mn);
                        agg = std::sqrt(ss / (cnt - 1));
                        if (st == "semean") agg = (1.0 / std::sqrt(static_cast<double>(cnt))) * agg; }
                } else if (st == "sebinomial") {
                    bool ok = true;
                    for (double x : s) if (x != 0.0 && x != 1.0) { ok = false; break; }
                    if (ok && cnt) { double t = 0; for (double x : s) t += x; double m = t / cnt;
                        agg = std::sqrt((m * (1.0 - m)) / static_cast<double>(cnt)); }
                } else if (st == "sepoisson" || st == "sepoisson_nocap") {
                    // Stata's built-in collapse saturates k at 2^31 (cz_c_sepoisson_g_v6c
                    // sqrt(2^31)/4); gtools does not (cz_g_sepoisson_g_v6c), and asks
                    // for "sepoisson_nocap".
                    bool ok = true;
                    for (double x : s) if (x < 0.0) { ok = false; break; }
                    if (ok && cnt) { double t = 0; for (double x : s) t += x; double m = t / cnt;
                        double k = std::floor(m * static_cast<double>(cnt) + 0.5);
                        if (st == "sepoisson" && k > 2147483648.0) k = 2147483648.0;
                        agg = std::sqrt(k) / static_cast<double>(cnt); }
                } else if (st == "first") { agg = vp[si][order[a]]; }        // RAW: first row, missing included
                else if (st == "last") { agg = vp[si][order[b - 1]]; }        // RAW: last row, missing included
                else if (st == "firstnm") { agg = cnt ? s.front() : vp[si][order[a]]; }  // all missing: the first row's own missing
                else if (st == "lastnm") { agg = cnt ? s.back() : vp[si][order[b - 1]]; }
                else if (st == "iqr") { if (cnt) { std::sort(s.begin(), s.end()); agg = stata_pctile(s, 75.0) - stata_pctile(s, 25.0); } }
                else if (pctile_of(st, &pp)) { if (cnt) { std::sort(s.begin(), s.end()); agg = stata_pctile(s, pp); } }
                else throw std::runtime_error("collapse: unknown stat '" + st + "'");
            } else {
                // ---- weighted path, per group ----
                // fw (byte-unchanged for the statistics it always served):
                // count=Sum(w), sum=Sum(w*x), mean=Sum(w*x)/Sum(w), sd denom Sum(w)-1.
                // aw rescales the weights to sum to the group's count (remakew in
                // Stata's collapse); iw/pw use the raw weights. min/max/
                // first/last stay plain unweighted row values among included rows.
                std::vector<double> s, sw;  // parallel: s[i] is the value, sw[i] its weight
                for (int64_t q = a; q < b; ++q) {
                    double x = vp[si][order[q]];
                    double wi = wcol[order[q]];
                    if (std::isnan(x) || std::isnan(wi) || wi == 0.0) continue;
                    s.push_back(x);
                    sw.push_back(wi);
                }
                const int64_t cnt = static_cast<int64_t>(s.size());
                double sumw = 0.0, sumwx = 0.0;
                for (int64_t i = 0; i < cnt; ++i) {
                    sumw += sw[static_cast<size_t>(i)];
                    sumwx += sw[static_cast<size_t>(i)] * s[static_cast<size_t>(i)];
                }
                // remakew: w* = w*obs/Sum(w), computed only where aw needs it.
                auto normalised = [&]() {
                    std::vector<double> ws(static_cast<size_t>(cnt));
                    for (int64_t i = 0; i < cnt; ++i)
                        ws[static_cast<size_t>(i)] = sw[static_cast<size_t>(i)] * static_cast<double>(cnt) / sumw;
                    return ws;
                };
                // Weighted sd-type spread: sqrt(Sum(w(x-m)^2)/(Sum(w)-1)).
                auto wspread = [&](const std::vector<double>& w) -> double {
                    double tw = 0.0, twx = 0.0;
                    for (int64_t i = 0; i < cnt; ++i) { tw += w[static_cast<size_t>(i)]; twx += w[static_cast<size_t>(i)] * s[static_cast<size_t>(i)]; }
                    if (cnt == 0 || tw == 0.0) return NAd;
                    const double mn = twx / tw;
                    double ss = 0.0;
                    for (int64_t i = 0; i < cnt; ++i) { double d = s[static_cast<size_t>(i)] - mn; ss += w[static_cast<size_t>(i)] * d * d; }
                    const double den = tw - 1.0;
                    if (!(den > 0.0)) return NAd;
                    return std::sqrt(ss / den);
                };
                if (st == "sum") {
                    if (wtype == "aw") { const auto ws = normalised(); double t = 0; for (int64_t i = 0; i < cnt; ++i) t += ws[static_cast<size_t>(i)] * s[static_cast<size_t>(i)]; agg = t; }
                    else agg = sumwx;
                }
                else if (st == "rawsum") { double t = 0; for (double x : s) t += x; agg = t; }
                else if (st == "count" || st == "n") agg = (wtype == "aw") ? static_cast<double>(cnt) : sumw;
                else if (st == "percent") { if (pct_total[si] != 0.0) agg = (sumw / pct_total[si]) * 100.0; }
                else if (st == "max") { if (cnt) agg = *std::max_element(s.begin(), s.end()); }
                else if (st == "min") { if (cnt) agg = *std::min_element(s.begin(), s.end()); }
                else if (st == "mean") { if (sumw != 0.0 && cnt) agg = sumwx / sumw; }
                else if (st == "sd") {
                    if (fw) {
                        if (sumw >= 2.0) {
                            double mn = sumwx / sumw, ss = 0;
                            for (int64_t i = 0; i < cnt; ++i) {
                                double d = s[static_cast<size_t>(i)] - mn;
                                ss += sw[static_cast<size_t>(i)] * d * d;
                            }
                            agg = std::sqrt(ss / (sumw - 1.0));
                        }
                    } else if (wtype == "aw") {
                        agg = wspread(normalised());
                    } else {
                        agg = wspread(sw);
                    }
                } else if (st == "semean") {
                    // fw: obs = Sum(w); aw: obs = the count over rescaled weights.
                    if (wtype == "aw") {
                        const double sp = wspread(normalised());
                        if (!std::isnan(sp)) agg = (1.0 / std::sqrt(static_cast<double>(cnt))) * sp;
                    } else {
                        const double sp = wspread(sw);
                        if (!std::isnan(sp) && sumw > 0.0) agg = (1.0 / std::sqrt(sumw)) * sp;
                    }
                } else if (st == "sebinomial") {
                    bool ok = true;
                    for (double x : s) if (x != 0.0 && x != 1.0) { ok = false; break; }
                    if (ok && cnt && sumw > 0.0) { const double m = sumwx / sumw;
                        agg = std::sqrt((m * (1.0 - m)) / std::trunc(sumw)); }
                } else if (st == "sepoisson" || st == "sepoisson_nocap") {
                    bool ok = true;
                    for (double x : s) if (x < 0.0) { ok = false; break; }
                    if (ok && cnt && sumw > 0.0) { const double obs = std::trunc(sumw); const double m = sumwx / sumw;
                        double k = std::floor(m * obs + 0.5);
                        if (st == "sepoisson" && k > 2147483648.0) k = 2147483648.0;
                        agg = std::sqrt(k) / obs; }
                } else if (st == "first" || st == "last") {
                    // RAW first/last row among weight-INCLUDED rows (missing x kept).
                    for (int64_t q = a; q < b; ++q) {
                        double wi = wcol[order[q]];
                        if (std::isnan(wi) || wi == 0.0) continue;
                        agg = vp[si][order[q]];
                        if (st == "first") break;
                    }
                }
                else if (st == "firstnm" || st == "lastnm") {
                    if (cnt) agg = st == "firstnm" ? s.front() : s.back();
                    else {
                        for (int64_t q = a; q < b; ++q) {
                            double wi = wcol[order[q]];
                            if (std::isnan(wi) || wi == 0.0) continue;
                            agg = vp[si][order[q]];
                            if (st == "firstnm") break;
                        }
                    }
                }
                else if (st == "iqr" || pctile_of(st, &pp)) {
                    const GroupWeightedPercentiles percentiles(s, sw, sumw);
                    if (percentiles.defined())
                        agg = st == "iqr" ? percentiles.at(75.0) - percentiles.at(25.0)
                                          : percentiles.at(pp);
                }
                else throw std::runtime_error("collapse: unknown stat '" + st + "'");
            }
            outcols[by.size() + si].second[gi] = agg;
        }
      }
    };
    if (G < 16) collapse_body(0, G, 0);
    else ThreadPool::global().run(G, collapse_body);
    Frame out;
    out.n_ = G;
    out.cols_.reserve(outcols.size());
    for (size_t oc = 0; oc < outcols.size(); ++oc) {
        auto& nc = outcols[oc];
        const bool by_factor = oc < by.size() && by_is_factor[oc];
        const bool spec_factor = oc >= by.size() && spec_is_factor[oc - by.size()];
        if (by_factor || spec_factor) {
            // Rebuild a FRESH, self-consistent factor dictionary from just the codes
            // actually present among the G output rows, in FIRST-SEEN output-row order
            // (same convention `cobra convert --factor` produces) — NOT alphabetical
            // (contrast egen_group(), which intentionally re-sorts alphabetically).
            const FrameColumn& src_col = by_factor ? col(by[oc]) : col(specs[oc - by.size()].var);
            std::vector<int32_t> new_codes(static_cast<size_t>(G));
            std::vector<std::string> new_levels;
            std::unordered_map<std::string, int32_t> dict;
            for (int64_t gi = 0; gi < G; ++gi) {
                double raw = nc.second[static_cast<size_t>(gi)];
                int32_t orig_code = (std::isnan(raw) || raw < 0.0) ? -1 : static_cast<int32_t>(std::llround(raw));
                if (orig_code < 0) { new_codes[static_cast<size_t>(gi)] = -1; continue; }
                const std::string& label = src_col.levels[static_cast<size_t>(orig_code)];
                auto it = dict.find(label);
                if (it == dict.end()) {
                    int32_t code = static_cast<int32_t>(new_levels.size());
                    dict.emplace(label, code);
                    new_levels.push_back(label);
                    new_codes[static_cast<size_t>(gi)] = code;
                } else {
                    new_codes[static_cast<size_t>(gi)] = it->second;
                }
            }
            FrameColumn c;
            c.name = nc.first;
            c.is_factor = true;
            c.codes = std::move(new_codes);
            c.levels = std::move(new_levels);
            out.cols_.push_back(std::move(c));
        } else {
            FrameColumn c;
            c.name = nc.first;
            c.is_factor = false;
            c.f64 = std::move(nc.second);
            out.cols_.push_back(std::move(c));
        }
    }
    out.reindex();
    return out;
}


}  // namespace cecon
