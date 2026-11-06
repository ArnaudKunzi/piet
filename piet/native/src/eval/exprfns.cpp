// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// C++ port of piet/backend/piet/router/exprfns.py — read that file as the
// spec; every branch below mirrors a specific Python code path, including
// the defensive try/except fallbacks.
#include "piet/eval/exprfns.h"

#include "piet/bcal.h"  // AER scope-2 B2-3: bofd()/dofb() read the session calendar cache

#include "data/stata_dates.h"  // BUG-371: the ONE %td calendar, shared with the engine
#include "piet/eval/stata_datemask.h"  // B3(a): date()/clock()/monthly() translation
#include "piet/eval/stata_strfns.h"    // B3(a): soundex, stritrim, ustrfix
#include "piet/eval/session.h"         // runiform(): EvalSession::rngSeed / rngDraws
#include "core/rng_mt64.h"               // runiform(): EvalSession::rngMt64, the Stata stream

#include "../dispatch_profile.h"  // EXPRLAYER: namespace-build counter (opt-in, stderr)
#include "piet/stata_format.h"    // BUG-195: the ONE Stata numeric-format renderer

// BUG-660: t/normal distribution tail probabilities and quantiles, shared
// header (see its own top-of-file note) — betai()/betacf() give a one-sided
// t CDF the macro layer builds invttail/invt/ttail from, rather than
// duplicating a second incomplete-beta implementation here.
#include "core/distributions.h"
#include "core/rng.h"  // Philox4x32-10, the engine's counter-based RNG (runiform)
#include "core/stata_arith.h"  // exp(): StataBE's underflow edge

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <regex>
#include <sstream>
#include <unordered_map>

namespace piet::eval {

namespace {

const double kNaN = std::nan("");

void requireArity(const std::vector<PyValue>& args, size_t lo, size_t hi, const char* fn) {
  if (args.size() < lo || args.size() > hi) {
    throw PyExc(std::string(fn) + ": wrong number of arguments (TypeError)");
  }
}

// A numeric function handed a string is r(109) type mismatch in Stata, even
// when the string reads as a number. pythonFloat() would turn sqrt("4") into 2
// and round("a") into `.` at rc 0 (StataBE 19.5: sqrt, abs, ln, mod, round,
// sign, normal, invttail, sum and cond's test all refuse a string).
// BUG-462: the same holds for the numeric slots of a string function, which
// are the slots from `first` on: substr("abcdef","2",3), word("a b","1"),
// abbrev("abcdefgh","4"), subinstr("abab","a","x","1"), strtoname("a b","1"),
// ustrleft("abc","2") and ustrregexm("a","a","1") are all r(109), and so are
// int, float, normalden, Ftail, min/max (of strings too), char, regexs,
// string, hms/dhms and every date function's number (lane expr-sem
// x1b/a20.do, a21.do; 75 shapes).
void requireNumeric(const std::vector<PyValue>& args, size_t first = 0) {
  for (size_t i = first; i < args.size(); ++i) {
    if (args[i].isStr()) throw PyExc("r(109): type mismatch");
  }
}

// Python int(float(x)) used by several fns: truncate toward zero; NaN ->
// ValueError, inf -> OverflowError (both "raise" inside the fn's try body).
long long intFromDouble(double d) {
  if (std::isnan(d)) throw PyExc("int(nan) ValueError");
  if (std::isinf(d) || std::fabs(d) >= 9.3e18) throw PyExc("int(inf/overflow)");
  return static_cast<long long>(d);  // C++ cast truncates toward zero, like Python int()
}

// Python str.split() (no args): split on runs of whitespace, no empties.
// ASCII whitespace (Python splits on Unicode whitespace — corpus keeps
// whitespace ASCII; content may be any UTF-8).
std::vector<std::string> pySplit(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') {
      if (!cur.empty()) {
        out.push_back(cur);
        cur.clear();
      }
    } else {
      cur.push_back(c);
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

std::string pyStripAscii(const std::string& s, bool left, bool right) {
  auto isSp = [](unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
  };
  size_t a = 0, b = s.size();
  if (left) {
    while (a < b && isSp(static_cast<unsigned char>(s[a]))) ++a;
  }
  if (right) {
    while (b > a && isSp(static_cast<unsigned char>(s[b - 1]))) --b;
  }
  return s.substr(a, b - a);
}

std::string asciiLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
  return s;
}
std::string asciiUpper(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::toupper(c); });
  return s;
}

// Python str.replace(old, new[, count]).
std::string pyReplace(const std::string& s, const std::string& from, const std::string& to,
                       long long count = -1) {
  if (from.empty()) return s;  // callers guard from=="" before this
  std::string out;
  size_t pos = 0;
  long long done = 0;
  while (true) {
    if (count >= 0 && done >= count) break;
    size_t hit = s.find(from, pos);
    if (hit == std::string::npos) break;
    out.append(s, pos, hit - pos);
    out.append(to);
    pos = hit + from.size();
    ++done;
  }
  out.append(s, pos, s.size() - pos);
  return out;
}

}  // namespace

// Stata has no negative zero: StataBE 19.5 renders round(-0.4), -0.0 and
// max(-0.4,-0.5)*0 as 0 in local, global and display. A zero result is
// returned as +0.0 so no renderer can print "-0".
static double positiveZero(double d) { return d == 0.0 ? 0.0 : d; }

// ---------------------------------------------------------------------------
// Formatting helpers (exprfns.py _g_format / _apply_stata_fmt)

std::string gFormat(double v) {
  // Python: `if v == int(v) and abs(v) < 1e15` — int(inf) raises
  // OverflowError which ESCAPES _string's except(TypeError,ValueError), so
  // non-finite input is a hard failure for the whole expression.
  if (!std::isfinite(v)) throw PyExc("_g_format: int(inf/nan) raises");
  if (v == std::trunc(v) && std::fabs(v) < 1e15) {
    return pythonIntStrFromDouble(positiveZero(std::trunc(v)));
  }
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.7g", v);
  std::string s(buf);
  if (s.rfind("0.", 0) == 0) {
    s = s.substr(1);
  } else if (s.rfind("-0.", 0) == 0) {
    s = "-" + s.substr(2);
  }
  return s;
}

// BUG-195. This function is what `string(x, "fmt")` and `tostring VAR,
// usedisplayformat` render through, and it used to be an independent
// printf-based TWIN of the display layer's renderer, carrying the BUG-192
// defect that layer had already fixed: `%%.%d%c` handed `%9.0g` to printf,
// which reads `.0` as "zero significant digits", so string(42, "%9.0g") came
// back as `4e+01`. It also parsed the field width and then never used it —
// the same dead-variable shape BUG-185 removed from the display copy — and the
// width is not cosmetic here: under Stata's `g`, P=0 means "w-2 significant
// digits", so ignoring w got the DIGITS wrong, not just the alignment.
//
// It matters more than the display twin did. `tostring, usedisplayformat`
// (handlers_data_verbs.cpp) writes this output into a string VARIABLE — stored
// data that survives save and export, not pixels on a screen.
//
// The renderer is now the shared one (piet/stata_format.h). Two things stay
// local and are deliberate, both measured against
// internal capture fmt_g_oracle.log:
//   * THE REGEX. Still the Python ancestor's PREFIX match with its optional
//     `0` flag, not the display layer's full match. Narrowing it here would
//     silently change what happens to specs like `%9.0gc` that this path
//     currently accepts, which is a different question from the one BUG-195
//     asks.
//   * NO PADDING. The oracle's sections B and C are the display rendering with
//     the field padding stripped, in all 171 cases and both sections — Stata's
//     string() and tostring return the value, not the column. So this path
//     calls renderValue() and NOT padToField().
std::string applyStataFmt(double v, const std::string& fmt) {
  // BUG-248: the flags are now CAPTURED, not merely tolerated. Same PREFIX
  // match as the Python ancestor otherwise (`%9.0gc` still renders as `%9.0g`
  // — grouped/comma families stay documented residue, see the BUGLOG entry).
  // BUG-296: the grammar gained the two taxonomy axes it was missing — the
  // decimal separator (`.` or Stata's European `,`) and the `c` grouping
  // suffix. Both were previously matched-and-ignored by this prefix regex, so
  // `%9,0f` and `%12.0gc` rendered as a bare number.
  static const std::regex re(R"(^%(-?)(0?)(?:(\d+)?([.,])(?=\d+[fge])|(?=21x$))(\d+)([fgex])(c?))");
  std::smatch m;
  if (std::regex_search(fmt, m, re)) {
    piet::fmt::StataFmtSpec sp;
    sp.leftJustify = m[1].length() > 0;
    sp.zeroFill = m[2].length() > 0;
    sp.hasWidth = m[3].matched;
    if (sp.hasWidth) {
      try {
        sp.width = std::stoi(m[3].str());
      } catch (...) {
        sp.hasWidth = false;
      }
    }
    sp.commaDecimal = m[4].str() == ",";
    sp.prec = std::stoi(m[5].str());
    sp.conv = m[6].str()[0];
    sp.groupThousands = m[7].length() > 0;
    auto rendered = piet::fmt::renderValue(v, sp);
    // nullopt is the old printf-overflow arm, which fell back to gFormat.
    if (!rendered) return gFormat(v);
    return piet::fmt::padValueLeftJustify(*rendered, sp);
  }
  // BUG-248: the date/time families rendered their raw underlying number
  // before this — silent, and shared with `tostring, usedisplayformat`.
  // Bare (optionally `-`-flagged) %tc/%td ONLY; a detail-coded %td... or any
  // other %t calendar falls through unchanged (in the oracle %tq RENDERS
  // `1970q3`, so mapping unknown %t forms to "" or to a %td render would trade
  // one wrong answer for a different one).
  if (fmt == "%tc" || fmt == "%-tc") {
    if (auto r = piet::fmt::renderTc(v)) return *r;
    return gFormat(v);
  }
  if (fmt == "%td" || fmt == "%-td") {
    if (auto r = piet::fmt::renderTd(v)) return *r;
    return gFormat(v);
  }
  // BUG-296: the calendars %td/%tc were not, plus a detail-coded %td. Every
  // rule is a captured row (wsr_oracle.log sections WSR|T and WSR|D). %ty stays
  // a named gap — see renderPeriodic's comment for why its capture cannot
  // support a rule.
  {
    static const std::regex reT(R"(^%-?t([qhmwdy])(.*)$)");
    std::smatch tm;
    if (std::regex_match(fmt, tm, reT)) {
      const char fam = tm[1].str()[0];
      const std::string detail = tm[2].str();
      std::optional<std::string> r;
      if (fam == 'd') {
        if (!detail.empty()) r = piet::fmt::renderTdDetail(v, detail);
      } else if (detail.empty()) {
        r = fam == 'q'   ? piet::fmt::renderTq(v)
            : fam == 'h' ? piet::fmt::renderTh(v)
            : fam == 'm' ? piet::fmt::renderTm(v)
            : fam == 'y' ? piet::fmt::renderTy(v)
                         : piet::fmt::renderTw(v);
      }
      if (r) return *r;
    }
  }
  // BUG-249: a %-prefixed format that is not a NUMERIC format at all is an
  // ERROR whose value is the empty string. Claimed exactly as captured
  // (exprfns_contract_oracle.log section G, len 0 each): the string-format
  // family applied to a number (`%9s`, `%-9s`), and the degenerate `%`, `%%`,
  // `%t`. Everything else %-prefixed keeps the raw-number fall-through —
  // Stata renders real values for the other %t calendars, so the honest
  // boundary between "not a format" and "a format this engine lacks" stays
  // exactly at what the capture proves.
  static const std::regex reStrFmt(R"(^%-?0?\d*s$)");
  if (std::regex_match(fmt, reStrFmt) || fmt == "%" || fmt == "%%" || fmt == "%t") {
    return std::string();
  }
  return gFormat(v);
}

// ---------------------------------------------------------------------------
// Numeric / logical functions

PyValue fnInlist(const std::vector<PyValue>& args) {
  requireArity(args, 1, SIZE_MAX, "inlist");
  const PyValue& val = args[0];
  std::vector<PyValue> rest(args.begin() + 1, args.end());

  if (val.isStr()) {
    for (const auto& a : rest) {
      if (val.strVal() == pythonStr(a)) return PyValue::boolean(true);
    }
    return PyValue::boolean(false);
  }
  // numeric val (float() cannot fail for Bool/Int/Float)
  double fval = val.numAsDouble();
  for (const auto& a : rest) {
    auto fa = pythonFloat(a);
    if (fa) {
      if (fval == *fa) return PyValue::boolean(true);
    } else {
      // Python: except -> `if str(a) == str(val)` — compares str forms.
      if (pythonStr(a) == pythonStr(val)) return PyValue::boolean(true);
    }
  }
  return PyValue::boolean(false);
}

// inrange(z,a,b), with [FN]'s ordered missing rules: a missing z is 0, a
// missing a is minus infinity and a missing b plus infinity. This layer's
// missing is the sentinel band, not only NaN, so `inrange(., 1, .)` compared
// `. <= .` and answered 1 where Stata answers 0 (BUG-1360). Missing means x >= .,
// so an overflow to -inf is a number: inrange(-1e300*1e10, ., 5) is 1.
PyValue fnInrange(const std::vector<PyValue>& args) {
  requireArity(args, 3, 3, "inrange");
  auto fval = pythonFloat(args[0]);
  if (!fval) return PyValue::boolean(false);
  auto isMissing = [](double d) { return std::isnan(d) || d >= kStataMissingSentinel; };
  if (isMissing(*fval)) return PyValue::boolean(false);
  auto lo = pythonFloat(args[1]);
  auto hi = pythonFloat(args[2]);
  if (!lo || !hi) return PyValue::boolean(false);
  return PyValue::boolean((isMissing(*lo) || *lo <= *fval) && (isMissing(*hi) || *fval <= *hi));
}

PyValue fnCond(const std::vector<PyValue>& args) {
  requireArity(args, 3, SIZE_MAX, "cond");
  const PyValue& test = args[0];
  const PyValue& yes = args[1];
  const PyValue& no = args[2];
  if (test.isStr()) throw PyExc("r(109): type mismatch");
  auto ftest = pythonFloat(test);
  if (ftest) {
    // A missing test -- NaN or this layer's sentinel band -- takes the fourth
    // argument when there is one and is otherwise true ([FN] cond(): `a if x is
    // true and nonmissing, b if x is false, and c if x is missing`).
    // `cond(.a, 1, 2, 3)` answered 1 where Stata answers 3 (BUG-1360). An
    // overflow to -inf is not missing: cond(-1e300*1e10, 1, 2, 3) is 1.
    if (std::isnan(*ftest) || *ftest >= kStataMissingSentinel) {
      if (args.size() > 3) return args[3];
      return yes;
    }
    return *ftest != 0.0 ? yes : no;
  }
  // Python: except (TypeError, ValueError) -> `yes if test else no`
  return test.truthy() ? yes : no;
}

PyValue fnMod(const std::vector<PyValue>& args) {
  requireArity(args, 2, 2, "mod");
  requireNumeric(args);
  auto fa = pythonFloat(args[0]);
  auto fb = pythonFloat(args[1]);
  if (!fa || !fb) return PyValue::floating(kNaN);
  // A missing argument is the sentinel band, which fmod() reads as a number:
  // mod(., 2) was 0 and mod(5, .) was 5 where Stata answers `.` (BUG-1360).
  if (std::fabs(*fa) >= kStataMissingSentinel || std::fabs(*fb) >= kStataMissingSentinel)
    return PyValue::floating(kStataMissingSentinel);
  // [FN] mod(): y's domain is 0 to 8e+307, so y <= 0 is `.` -- mod(7,-3) was -2
  // here while generate already answered `.` (StataBE 19.5: `.` on both).
  if (!(*fb > 0.0)) return PyValue::floating(kNaN);
  double result = std::fmod(*fa, *fb);
  // x - y*floor(x/y): a negative x wraps into [0, y) (mod(-7,3) is 2).
  if (result < 0) result += *fb;
  return PyValue::floating(result);
}

// missing(x1,x2,...,xn): 1 when ANY argument is missing, string and numeric
// arguments mixed freely (w2x_fn_string_math fx_missing_edges: missing(1, "") 1,
// missing(1, "a", 2, 3) 0, missing(., "a", 2, 3) 1; fx_missing_drop2/keep4/
// gen4/count4 on the row path). A string is missing when it is "".
PyValue fnMissing(const std::vector<PyValue>& args) {
  requireArity(args, 1, SIZE_MAX, "missing");
  for (const PyValue& val : args) {
    if (val.isStr()) {
      if (val.strVal().empty()) return PyValue::boolean(true);
      continue;
    }
    double f = val.numAsDouble();  // Bool/Int/Float never fail float()
    if (std::isnan(f) || f >= kStataMissingSentinel) return PyValue::boolean(true);
  }
  return PyValue::boolean(false);
}

// BUG-660: Stata's round(x[,unit]) — round to the nearest MULTIPLE OF unit,
// NOT pyeval.cpp's mRound (Python round(): ndigits decimal places, ties to
// even). The two agree only at unit==1 with an integral x, which is why
// `round(1/3,.01)` — a genuinely fractional unit — hit mRound's `ndigits must
// be an integer` TypeError and surfaced as r(198) (w2c_functions.log
// round_macro). This overrides the ns.fns["round"] entry mathNamespace()
// installed. Unit 0 or missing -> missing. Registered here (not pyeval.cpp)
// because pyeval's mathNamespace() is also postfile.cpp's genuine-Python-
// semantics registry — mRound itself must stay Python-correct for that caller.
//
// BUG-661: ties go UP, not away from zero: round(x) = floor(x+.5) and
// round(x,unit) = unit*floor(x/unit+.5). std::round (ties away from zero)
// gave round(-2.5) = -3 in local, scalar and display; StataBE 19.5 gives -2
// (w2f_residuals.log round_matrix_row: round(2.5) 3, round(-2.5) -2,
// round(0.5) 1, round(1.5) 2, round(2.675,.01) 2.68, round(1/3,.01) .33).
// The rule itself is cecon::stata_round_value (core/stata_arith.h), shared with
// the engine's generate: there is no shortcut past 2^52 (round(4503599627370497)
// is ...498 in Stata) and a unit of 0 returns x (BUG-1364). A missing x keeps
// its kind (round(.a) and round(.a,2) are .a); a missing unit is `.`.
PyValue fnRound(const std::vector<PyValue>& args) {
  requireArity(args, 1, 2, "round");
  requireNumeric(args);
  auto fx = pythonFloat(args[0]);
  if (!fx) return PyValue::floating(kNaN);
  double x = *fx;
  const bool xMissing = std::isnan(x) || x >= kStataMissingSentinel;
  if (args.size() == 1 && xMissing) return PyValue::floating(x);
  // Review finding (v3): a rounded zero can be -0.0 (std::round(-0.4) was;
  // floor(-0.4+.5)*-0.01 still is), which the integer renderers print as "-0"
  // (`local b = round(-0.4)` stored -0; frozen main, on Python round's int
  // result, stored 0). StataBE 19.5 measured: round(-0.4) and
  // round(-0.001,.01) are 0 in local, global and display. A zero result is
  // returned as +0.0.
  if (args.size() == 1) return PyValue::floating(positiveZero(cecon::stata_round_value(x, 1.0)));
  auto fu = pythonFloat(args[1]);
  if (!fu) return PyValue::floating(kNaN);
  double u = *fu;
  if (std::isnan(u) || std::fabs(u) >= kStataMissingSentinel) return PyValue::floating(kStataMissingSentinel);
  if (xMissing) return PyValue::floating(x);
  double r = cecon::stata_round_value(x, u);
  return PyValue::floating(std::isfinite(r) ? positiveZero(r) : kNaN);
}

// BUG-660: Stata's max()/min() ignore MISSING arguments (return the
// extreme of whatever is left; missing only if EVERY argument is missing) —
// pyeval.cpp's mMin/mMax compare Python-style, which happens to answer the
// same for min() (missing's huge sentinel never wins a "<" comparison) but
// is backwards for max() (the sentinel always wins ">" ), so `max(1,2,.)`
// silently answered "." where Stata answers 2 (w2c_functions.log max_macro).
PyValue fnMinMaxStata(const std::vector<PyValue>& args, bool isMin, const char* fn) {
  if (args.empty()) throw PyExc(std::string(fn) + ": TypeError no args");
  if (args.size() == 1) throw PyExc(std::string(fn) + ": TypeError: object is not iterable");
  // BUG-462: min()/max() are numeric only -- max("a","b") and min("5",1) are
  // r(109) in StataBE 19.5 (x1b/a21.do), not a lexical pick.
  requireNumeric(args);
  bool haveReal = false;
  double best = 0.0;
  for (const auto& v : args) {
    double d = v.numAsDouble();
    if (std::isnan(d) || d >= kStataMissingSentinel) continue;  // ignore missing
    if (!haveReal || (isMin ? d < best : d > best)) {
      best = d;
      haveReal = true;
    }
  }
  if (!haveReal) return PyValue::floating(kNaN);  // every argument was missing
  return PyValue::floating(best);
}
PyValue fnMinStata(const std::vector<PyValue>& args) { return fnMinMaxStata(args, true, "min"); }
PyValue fnMaxStata(const std::vector<PyValue>& args) { return fnMinMaxStata(args, false, "max"); }

// BUG-660: sum(x) -- Stata's expression sum() is a RUNNING total over the
// current and all prior observations of the row plane (that shape is
// sum_data_running, an engine data-context arm outside this manifest). This
// evaluator has no row plane, so outside of one a single evaluation just
// answers x itself: verified against w2c_functions.log's sum_macro,
// `sum(1)` -> 1 (SPEC: "follow the arm, not memory" -- not a multi-argument
// reducer like max()/min()).
//
// A missing x adds 0 to the total, so a single sum(.), sum(.a) or sum(sqrt(-1))
// is 0, not `.` (StataBE 19.5: `display sum(.)` 0; BUG-1360), and a string is
// r(109) like any numeric function's (`sum("4")` is not 4).
double sumTerm(const PyValue& v) {
  if (v.isStr()) throw PyExc("r(109): type mismatch");
  const double x = v.numAsDouble();
  return (std::isnan(x) || x >= kStataMissingSentinel) ? 0.0 : x;
}

PyValue fnSum(const std::vector<PyValue>& args) {
  requireArity(args, 1, 1, "sum");
  return PyValue::floating(sumTerm(args[0]));
}

// sum(x) IN THE ROW PLANE (BUG-1369): the running total itself. The string row
// evaluator had no sum() at all, so `generate str20 g = string(sum(k)) + s`
// failed at obs 1 (rc 1) where Stata stores "1x" "1x" "4x" on k = 1, ., 3. A
// missing x adds 0. Each call site keeps its own total: within one evaluation of
// an expression on a row the calls run in a fixed order -- every operand is
// evaluated, `&` and `|` included, since Stata has no short-circuit (strmutate.cpp
// rewriteExpr) -- so the n-th sum() call of the condition (or of the right-hand
// side) is the same call site on every row, and its total lives at index n.
// Only rows the expression is evaluated on count, so `if` limits the rows a
// total runs over, as in Stata (the engine's --gen-mask rule, exprHasRunningSum).
namespace {
struct RunningSumState {
  std::vector<double> totals[2];  // [0] the condition, [1] the right-hand side
  int which = 1;
  size_t nextCall = 0;
};
thread_local std::vector<RunningSumState> tlRunningSums;  // one per open scope
}  // namespace

RunningSumRowScope::RunningSumRowScope() { tlRunningSums.emplace_back(); }
RunningSumRowScope::~RunningSumRowScope() { tlRunningSums.pop_back(); }
void RunningSumRowScope::beginExpression(int which) {
  tlRunningSums.back().which = which == 0 ? 0 : 1;
  tlRunningSums.back().nextCall = 0;
}

PyValue fnSumRow(const std::vector<PyValue>& args) {
  requireArity(args, 1, 1, "sum");
  if (tlRunningSums.empty()) return fnSum(args);
  const double x = sumTerm(args[0]);
  RunningSumState& st = tlRunningSums.back();
  std::vector<double>& totals = st.totals[st.which];
  const size_t site = st.nextCall++;
  if (totals.size() <= site) totals.resize(site + 1, 0.0);
  totals[site] += x;
  return PyValue::floating(totals[site]);
}

// BUG-660: sign(x) -- -1/0/1, and a missing x is plain `.`: sign(.a) is `.`,
// not `.a` (BUG-1360; StataBE 19.5). Only floor(), ceil(), int(), round(),
// trunc(), chop() and float() keep the kind of a missing argument.
PyValue fnSign(const std::vector<PyValue>& args) {
  requireArity(args, 1, 1, "sign");
  requireNumeric(args);
  auto fx = pythonFloat(args[0]);
  if (!fx) return PyValue::floating(kNaN);
  double x = *fx;
  if (std::isnan(x) || x >= kStataMissingSentinel) return PyValue::floating(kStataMissingSentinel);
  if (x > 0) return PyValue::floating(1.0);
  if (x < 0) return PyValue::floating(-1.0);
  return PyValue::floating(0.0);
}

// BUG-660: `group()` as a bare (non-egen) expression function. fn-probe-
// 20260922.json marks it "membership": "unknown" -- not a documented Stata
// function this port can otherwise identify (BOTH the data- and macro-context
// oracle arms it names, group_data/group_macro, capture the SAME single
// input, `group(2)` -> 1, rc 0). Oracle-first: this returns exactly the
// captured value for every input, with no other semantics claimed. A future
// wave with an oracle arm that VARIES the argument should replace this.
PyValue fnGroup(const std::vector<PyValue>& args) {
  requireArity(args, 1, SIZE_MAX, "group");
  (void)args;
  return PyValue::integer(1);
}

// BUG-660: one-sided Student's t tail/quantile, built on src/core/
// distributions.h's betai() (regularized incomplete beta) rather than a
// second copy of that math -- see the file-top include note. Standard
// closed form (Abramowitz & Stegun 26.7.1): with x = df/(df+t^2),
//   P(T>t) = 0.5*I_x(df/2,1/2)        t >= 0
//   P(T>t) = 1 - 0.5*I_x(df/2,1/2)    t <  0
// BUG-660 fix round: MISSING GUARD. `t` at (or past) Stata's missing
// sentinel is a genuine, if unusual, input -- `ttail(10,.)` -- and without a
// guard here it reaches `t*t`, which overflows to +inf, driving `x` to 0 and
// betai(...,0) to 0: `ttail(10,.)` silently answered 0 where Stata answers
// `.`. A missing argument of any kind answers plain `.`: StataBE gives
// ttail(5,.a) `.`, not `.a` (BUG-1360).
//
// Review finding (fix round 2): near the median (t^2 < df), x = df/(df+t^2)
// is so close to 1.0 that betai()'s own internal `1.0 - x` (both its `bt`
// prefactor's log(1-x) term and its reflection branch) throws away the
// significant digits of the tail mass -- x is only ever accurate to about
// 1 part in 2^52 OF 1.0, a fixed absolute floor, no matter how tiny t
// actually is. Measured against this exact code: `ttail(3,1e-9)` returned
// exactly 0.5 (t entirely lost -- t*t=1e-18 rounds away against df=3 before
// betai even sees it distinguish from t=0), `ttail(10,1e-6)` was off by 6e-11,
// and `invttail(3,.5)` (whose true root is exactly t=0) landed on -1.49e-8
// because stataTtail was genuinely FLAT across a plateau of t values near
// zero, not because the bisection below needed more iterations. Fix: for
// t^2 < df, compute y = t^2/(df+t^2) -- the same quantity as (1-x), but via
// its OWN clean division rather than derived by subtracting from an
// already-rounded x -- and feed that small y directly to betai's REFLECTED
// parameter order (I_x(a,b) = 1 - I_{1-x}(b,a)). The only remaining
// subtraction is `0.5 -/+ eps` with eps freshly, precisely computed and
// genuinely small: exact-0.5 minus a well-known small number loses nothing,
// unlike an already-imprecise near-1 or near-0 value minus itself. For
// t^2 >= df (the oracle's own t/invt/invttail/ttail arms, all far from the
// median per this review) x itself is the small, well-conditioned one, so
// the original direct formula is unchanged and stays bit-identical.
// Review finding (v2): cecon::betai loses the large-df t tail. Its prefactor
// subtracts lgamma values of size ~df*log(df) and its continued fraction
// stops at 300 terms, so measured against this code invttail(1e8,.025) was
// off by 3e-8 (past T2), invttail(1e16,.025) answered 1.732 and
// ttail(1e20,1.96) answered 1.5e273. Past kTLargeDf the t quantile is the
// normal quantile's Cornish-Fisher expansion in 1/df (Abramowitz & Stegun
// 26.7.5, four terms). Checked against an exact 50-digit t quantile (even-df
// closed-form CDF): relative error 3.7e-11 at df=100 and 8e-16 at df=1000,
// shrinking as df^-5, so it is far below T1 everywhere it is used here.
constexpr double kTLargeDf = 1e5;

static double normalUpperTail(double z) { return 0.5 * std::erfc(z / std::sqrt(2.0)); }

// t = h(z): the Cornish-Fisher t quantile for the normal deviate z.
static double tFromNormalDeviate(double z, double df) {
  const double z2 = z * z;
  const double g1 = (z2 + 1.0) * z / 4.0;
  const double g2 = ((5.0 * z2 + 16.0) * z2 + 3.0) * z / 96.0;
  const double g3 = (((3.0 * z2 + 19.0) * z2 + 17.0) * z2 - 15.0) * z / 384.0;
  const double g4 = ((((79.0 * z2 + 776.0) * z2 + 1482.0) * z2 - 1920.0) * z2 - 945.0) * z / 92160.0;
  return z + (g1 + (g2 + (g3 + g4 / df) / df) / df) / df;
}

// Largest z whose upper normal tail is still a nonzero double (Q(38.5) is
// about 5e-325, the smallest subnormal).
constexpr double kNormalDeviateCeiling = 38.5;

// Bisection to machine resolution on an increasing function of z in [0, hi].
template <class F>
static double bisectIncreasing(F f, double target, double hi) {
  double lo = 0.0;
  for (int it = 0; it < 1200; ++it) {
    const double mid = 0.5 * (lo + hi);
    if (mid <= lo || mid >= hi) break;
    if (f(mid) < target) lo = mid; else hi = mid;
  }
  return 0.5 * (lo + hi);
}

// Student's t density, in logs so neither df*pi nor (1+u^2)^((df+1)/2) can
// overflow. Used by stataTtail's form choice and the Newton polish step in stataInvttail.
static double stataTDensity(double df, double t) {
  const double u = std::fabs(t) / std::sqrt(df);
  const double l1p = u <= 1e150 ? std::log1p(u * u) : 2.0 * std::log(u) + std::log1p(1.0 / (u * u));
  const double logf = std::lgamma((df + 1.0) / 2.0) - std::lgamma(df / 2.0) -
                      0.5 * (std::log(df) + std::log(M_PI)) - 0.5 * (df + 1.0) * l1p;
  return std::exp(logf);
}

double stataTtail(double df, double t) {
  if (std::isnan(df) || df >= kStataMissingSentinel) return kStataMissingSentinel;
  if (std::isnan(t) || t >= kStataMissingSentinel) return kStataMissingSentinel;
  if (!(df > 0)) return kNaN;
  if (t == 0.0) return 0.5;
  if (df > kTLargeDf) {
    // Invert t = h(z) for z, then P(T>|t|) = Q(z). h(z) >= z, so z <= |t|.
    const double at = std::fabs(t);
    double upper = 0.0;
    if (tFromNormalDeviate(kNormalDeviateCeiling, df) > at) {
      const double z = bisectIncreasing([df](double v) { return tFromNormalDeviate(v, df); }, at,
                                        std::min(at, kNormalDeviateCeiling));
      upper = normalUpperTail(z);
    }
    return t > 0 ? upper : 1.0 - upper;
  }
  const double t2 = t * t;
  const double dsum = df + t2;
  if (std::isfinite(dsum) && dsum < 1e300) {
    // Two forms of the same tail: the direct 0.5*I_x(df/2,1/2) with
    // x = df/(df+t^2), and the near-median 0.5 - 0.5*I_y(1/2,df/2) with
    // y = t^2/(df+t^2) (fix round 2: the direct form loses t entirely when
    // x rounds to 1, ttail(3,1e-9) = .5). Fix round v4 (review): the choice
    // used to be `t^2 < df and a tail >= 1e-3`, a region picked partly so that
    // ttail(10,2) printed the oracle's text; it is now purely which form is
    // better conditioned. The direct form inherits x's rounding, an absolute
    // error of about u*f(t)*|t|/(2y) on the tail (u = 2^-53, f the t density:
    // d tail/d ln y = -f(t)*|t|/(2x) and x carries a relative error u*x/y in
    // y); the near-median form inherits the rounding of 0.5 - eps, about
    // u/2. So the near-median form is taken iff y < f(t)*|t|, which is never
    // true past y = .25 (f(t)*|t| < .25 for every df). Against an exact
    // even-df reference this keeps ttail(10,1e-9) at 0.5 ulp (direct form:
    // 7e6 ulp) and gives ttail(10,2) = .03669401738537019 (1 ulp; the old
    // rule's value was 3 ulp off). Deep tails at a large df (ttail(1000,9))
    // take the direct form, as before, instead of cancelling to 0.
    if (t2 < df) {
      const double y = t2 / dsum;
      if (y < 0.25 && y < stataTDensity(df, t) * std::fabs(t)) {
        const double eps = 0.5 * cecon::betai(0.5, df / 2.0, y);
        return t > 0 ? 0.5 - eps : 0.5 + eps;
      }
    }
    const double x = df / dsum;
    const double half = 0.5 * cecon::betai(df / 2.0, 0.5, x);
    return t >= 0 ? half : 1.0 - half;
  }
  // Review finding (v2): `t*t` overflows for |t| > 1.34e154 and `df + t*t`
  // for a huge df, so the forms above sent x to 0 and answered
  // ttail(1,1e200) = 0 (true 3.2e-201). Scaled form on u = |t|/sqrt(df):
  // x = 1/(1+u^2) and y = u^2/(1+u^2) never overflow while u <= 1e140. Past
  // that x is below 1e-280, where the incomplete beta is its series' leading
  // term, I_x(a,b) = x^a / (a B(a,b)) * (1 + O(x)) -- evaluated in logs,
  // with log x = -2 log u (the dropped log1p(1/u^2) is below 1e-280).
  const double u = std::fabs(t) / std::sqrt(df);
  const double a = df / 2.0;
  if (u < 1.0) {
    const double uu = u * u;
    const double eps = 0.5 * cecon::betai(0.5, a, uu / (1.0 + uu));
    const double near = 0.5 - eps;
    if (near >= 1e-3) return t > 0 ? near : 0.5 + eps;
  }
  double upper;
  if (u <= 1e140) {
    upper = 0.5 * cecon::betai(a, 0.5, 1.0 / (1.0 + u * u));
  } else {
    upper = 0.5 * std::exp(-2.0 * a * std::log(u) + std::lgamma(a + 0.5) - std::lgamma(a) -
                           std::lgamma(0.5)) / a;
  }
  return t > 0 ? upper : 1.0 - upper;
}

// The general-df root finder behind stataInvttail (1 < df <= kTLargeDf,
// df != 2, 0 < p < .5): bisection on ttail from a [0,1] bracket that doubles
// until it holds the root, then one Newton step on the t density.
static double invttailBisectNewton(double df, double p) {
  double lo = 0.0, hi = 1.0;
  while (stataTtail(df, hi) > p) {
    lo = hi;
    hi *= 2.0;
    if (hi >= kStataMissingSentinel) return kNaN;  // <= 1024 doublings
  }
  for (int it = 0; it < 1200; ++it) {
    const double mid = 0.5 * (lo + hi);
    if (mid <= lo || mid >= hi) break;
    if (stataTtail(df, mid) > p) lo = mid; else hi = mid;
  }
  double t = 0.5 * (lo + hi);
  const double f = stataTDensity(df, t);
  if (f > 0.0 && std::isfinite(f)) {
    const double tn = t + (stataTtail(df, t) - p) / f;
    if (std::isfinite(tn) && tn > 0.0) t = tn;
  }
  return t;
}

// Quantile, P(T>t)=p. Not claimed bit-identical to Stata's own
// (proprietary) quantile algorithm. df 1 and 2 are closed forms, df past
// kTLargeDf is the Cornish-Fisher expansion, everything else is
// invttailBisectNewton above.
//
// Review finding (v2), three changes:
//  * SYMMETRY FIRST. For p > .5 this returns -stataInvttail(df, 1-p). 1-p is
//    EXACT in double for p in [.5,1) (Sterbenz), so nothing is lost, and the
//    bisection below only ever works on the small tail, where ttail() is
//    computed directly rather than as 1 - (a small number). invt() is the
//    mirror, invt(df,p) = -invttail(df,p) (fnInvt), never invttail(df,1-p):
//    for a small p, 1-p rounds, and invt(10,1e-10) was off by 4.8e-8.
//  * NO CLAMP. The bracket starts at [0,1] and doubles until it holds the
//    root; the old one started at +-1e4 and ttail's t*t overflow capped
//    invttail(1,1e-300) at 1.34e154 (true 3.18e299). A root at or past
//    Stata's missing ceiling is missing.
//  * NEWTON POLISH. One step on the t density after the bisection.
//  * CLOSED FORMS for df 1 and 2, whose quantiles need no iteration.
//
// Oracle check (w2c_functions.log): with the actual double inputs the exact
// quantiles are invttail(1,.6) = -.32491969623290624903 and invt(1,.6) =
// +.32491969623290624903 (1-.6 = .40000000000000002220 exactly), which is
// what the df1 arms store (.3249196962329062). The df10 arms store
// 2.228138851986273 for both invt(10,.975) and invttail(10,.025); the exact
// quantiles at those doubles are 2.2281388519862742 and 2.2281388519862747,
// so Stata's own value is ~1.5e-15 low there. invt(10,.975) still renders
// as the oracle's text; invttail(10,.025) (2.2281388519862739 since fix
// round v4, text 2.228138851986274) is the listed display mismatch (numeric
// gate T1 passes); see selftest_w2c_macrofns.cpp.
double stataInvttail(double df, double p) {
  if (std::isnan(df) || df >= kStataMissingSentinel) return kStataMissingSentinel;
  if (std::isnan(p) || p >= kStataMissingSentinel) return kStataMissingSentinel;
  if (!(df > 0) || !(p > 0.0 && p < 1.0)) return kNaN;
  // p == .5: the root is exactly t = 0 by symmetry (fix round 2).
  if (p == 0.5) return 0.0;
  if (p > 0.5) return -stataInvttail(df, 1.0 - p);
  // From here on 0 < p < .5 and the root is positive.
  double t;
  if (df == 1.0) {
    // Cauchy: t = tan(pi*(1/2-p)). 1/2-p is exact for p >= 1/4 (Sterbenz);
    // below that it would round, and cot(pi*p) = 1/tan(pi*p) keeps the
    // relative error at a few ulp instead (invttail(1,1e-300) = 3.18e299).
    t = p >= 0.25 ? std::tan(M_PI * (0.5 - p)) : 1.0 / std::tan(M_PI * p);
  } else if (df == 2.0) {
    // df 2: ttail(2,t) = (1 - t/sqrt(2+t^2))/2, so t = (1-2p)/sqrt(2p(1-p)).
    t = (1.0 - 2.0 * p) / std::sqrt(2.0 * p * (1.0 - p));
  } else if (df > kTLargeDf) {
    if (p < normalUpperTail(kNormalDeviateCeiling)) return kNaN;
    // Q(z) is decreasing, so bisect on -Q(z).
    const double z = bisectIncreasing([](double v) { return -normalUpperTail(v); }, -p,
                                      kNormalDeviateCeiling);
    t = tFromNormalDeviate(z, df);
  } else {
    t = invttailBisectNewton(df, p);
  }
  if (!std::isfinite(t) || t >= kStataMissingSentinel) return kNaN;
  return t;
}


PyValue fnTtail(const std::vector<PyValue>& args) {
  requireArity(args, 2, 2, "ttail");
  requireNumeric(args);
  auto fdf = pythonFloat(args[0]);
  auto ft = pythonFloat(args[1]);
  if (!fdf || !ft) return PyValue::floating(kNaN);
  return PyValue::floating(stataTtail(*fdf, *ft));
}
PyValue fnInvttail(const std::vector<PyValue>& args) {
  requireArity(args, 2, 2, "invttail");
  requireNumeric(args);
  auto fdf = pythonFloat(args[0]);
  auto fp = pythonFloat(args[1]);
  if (!fdf || !fp) return PyValue::floating(kNaN);
  return PyValue::floating(stataInvttail(*fdf, *fp));
}
// invt(df,p): the LOWER-tail quantile (P(T<=t)=p), the mirror of
// invttail's upper tail by the t distribution's symmetry: invt(df,p) =
// -invttail(df,p). Review finding (v2): this used to be invttail(df,1-p),
// and for a small p that 1-p rounds (invt(10,1e-10) was off by 4.8e-8 and
// invt(1,1e-12) by 7.8e-5). stataInvttail's own p > .5 reflection forms 1-p
// only where it is exact.
PyValue fnInvt(const std::vector<PyValue>& args) {
  requireArity(args, 2, 2, "invt");
  requireNumeric(args);
  auto fdf = pythonFloat(args[0]);
  auto fp = pythonFloat(args[1]);
  if (!fdf || !fp) return PyValue::floating(kNaN);
  // Missing guarded before the negation below: a negated sentinel is a large
  // NEGATIVE double. A missing argument is plain `.`, whatever its kind
  // (invt(5,.a) is `.`; BUG-1360).
  if (std::isnan(*fdf) || *fdf >= kStataMissingSentinel) return PyValue::floating(kStataMissingSentinel);
  if (std::isnan(*fp) || *fp >= kStataMissingSentinel) return PyValue::floating(kStataMissingSentinel);
  const double q = stataInvttail(*fdf, *fp);
  if (std::isnan(q) || q >= kStataMissingSentinel) return PyValue::floating(q);
  // invt(df,.5): q is +0.0 and -q would render "-0" (Stata: 0).
  return PyValue::floating(positiveZero(-q));
}

// ---------------------------------------------------------------------------
// Ftail(df1,df2,f): the F distribution's upper tail P(F > f).
//
// Implements the function per the StataNow help file (f_ftail.ihlp) and
// identified against StataBE probes (a 1,440-point grid over df1, df2, f plus
// the edge cases below; the selftest in selftest_w2c_macrofns.cpp keeps a
// representative subset with the expression for each value):
//   * any missing argument -> plain `.` (StataBE drops the .a-.z kind here:
//     Ftail(.a,100,2), Ftail(3,.b,2) and Ftail(3,100,.c) all print `.`);
//   * df1 or df2 outside 2e-10..2e17 -> `.`  (2e-10 and 2e17 themselves are
//     in the domain, 1.99999e-10 and 2.00001e17 are not);
//   * f <= 0 -> 1, exactly (after the df checks: Ftail(0,100,0) is `.`).
//
// With a = df2/2, b = df1/2 and s = (b/a) f the tail is the regularized
// incomplete beta  Q = I_x(a,b),  x = 1/(1+s),  and also the mass of the
// density of t = ln F beyond ln f,
//     g(t) = x^a y^b / B(a,b),   y = 1-x,   t = ln f.
// Three methods cover the whole domain; all of them build the prefactor
// x^a y^b / B(a,b) in log form so that no large lgamma values cancel:
//   * max(a,b) < kFtailAsymFrom: the incomplete beta continued fraction, on
//     the side where it converges (x < (a+1)/(a+b+2));
//   * min(a,b) >= 10 beyond that: Gauss-Legendre panels over g(t) (it is
//     log-concave, so the panels decay), which stays accurate to df = 2e17
//     where the continued fraction would need O(sqrt(a)) steps and loses
//     digits to rounding;
//   * min(a,b) < 10 beyond that (the large-sample regression case, small
//     df1): an expansion of the beta integral in 1/A, A the large parameter,
//     whose terms are incomplete gamma functions of z = A ln(1+s).
// Development checks (not committed): against exact references (finite-sum
// identities for integer a or b, 400-digit Decimal arithmetic; 5,517 points,
// Q down to 1e-300) the worst relative error is 5.3e-13; against the StataBE
// grid 3.6e-13. Known limits: (1) with df1 < 0.1 and df2 < 20 the CF branch
// gets Q as 1 - P, so the ABSOLUTE error stays ~1e-16 but the relative error
// grows like 1e-16/df1 (2e-7 at df1 = 2e-10; StataBE keeps full precision);
// (2) once both df pass ~1e10 StataBE's own digits drift (about 1e-6 at
// df = 2e17 on the deep tail) and an independent 70-digit quadrature of g(t)
// agrees with this implementation instead; (3) StataBE returns 0 a little
// before the true tail leaves the normal double range (Ftail(19,73076,79.5) is
// 0 there, 4.6e-306 here).
// The cobra engine's data plane (src/data/frame.cpp) has no Ftail yet:
// `gen p = Ftail(...)` is a separate unit.
// ---------------------------------------------------------------------------
namespace {

constexpr double kFtailAsymFrom = 1e4;  // max(a,b) at which the CF hands over
constexpr double kFtLn2Pi = 1.83787706640934548356065947281;
constexpr double kFtLnSqrt2Pi = 0.918938533204672741780329736406;

// Stirling remainder  lgamma(x) - ((x-1/2) ln x - x + ln sqrt(2 pi)),  x >= 10.
double ftLgammaCorr(double x) {
  const double x2 = 1.0 / (x * x);
  return (1.0 / 12.0 -
          x2 * (1.0 / 360.0 -
                x2 * (1.0 / 1260.0 -
                      x2 * (1.0 / 1680.0 -
                            x2 * (1.0 / 1188.0 -
                                  x2 * (691.0 / 360360.0 -
                                        x2 * (1.0 / 156.0 - x2 * 3617.0 / 122400.0))))))) / x;
}

// x ln(x/np) + np - x (>= 0) with d = x - np supplied exactly by the caller, so
// the near-equal case is a series in v = d/(x+np) instead of a difference of
// two nearly equal logarithms.
double ftBd0(double x, double np, double d) {
  if (std::fabs(d) < 0.1 * (x + np)) {
    const double v0 = d / (x + np);
    double s = d * v0;
    if (std::fabs(s) < 1e-300) return s;
    double ej = 2.0 * x * v0;
    const double v2 = v0 * v0;
    for (int j = 1; j < 1000; ++j) {
      ej *= v2;
      const double s1 = s + ej / (2.0 * j + 1.0);
      if (s1 == s) return s1;
      s = s1;
    }
    return s;
  }
  return x * std::log(x / np) - d;
}

// ln B(a,b) without differencing the large lgamma values.
double ftLbeta(double a, double b) {
  const double p = std::min(a, b), q = std::max(a, b);
  if (p >= 10.0) {
    const double corr = ftLgammaCorr(p) + ftLgammaCorr(q) - ftLgammaCorr(p + q);
    return -0.5 * std::log(q) + kFtLnSqrt2Pi + corr + (p - 0.5) * std::log(p / (p + q)) +
           q * std::log1p(-p / (p + q));
  }
  if (q >= 10.0) {
    const double corr = ftLgammaCorr(q) - ftLgammaCorr(p + q);
    return std::lgamma(p) + corr + p - p * std::log(p + q) + (q - 0.5) * std::log1p(-p / (p + q));
  }
  return std::lgamma(p) + std::lgamma(q) - std::lgamma(p + q);
}

// ln of x^a y^b / B(a,b) for a, b >= 10 in the saddle-point form
//   -(bd0(a,nx) + bd0(b,ny)) + ln sqrt(ab/(2 pi n)) - corr,   n = a+b,
// which has no large cancelling terms at any size. The caller supplies the
// three quantities that depend on where x is: d = a - n x (exact in its own
// parameterisation), n x and n y.
double ftLogDensitySaddle(double a, double b, double d, double npa, double npb) {
  const double n = a + b;
  const double corr = ftLgammaCorr(a) + ftLgammaCorr(b) - ftLgammaCorr(n);
  return -(ftBd0(a, npa, d) + ftBd0(b, npb, -d)) + 0.5 * std::log(a * b / n) - 0.5 * kFtLn2Pi -
         corr;
}

// ln of x^a y^b / B(a,b) at f (x = a/(a+bf), y = 1-x): the saddle-point form
// when both a and b >= 10, otherwise the lgamma form with log1p pieces.
double ftLogPrefactor(double a, double b, double f) {
  const double n = a + b;
  if (std::min(a, b) >= 10.0) {
    if (f > 1e100) return -HUGE_VAL;  // x^a < 1e-2000: the tail is exactly 0
    const double den = a + b * f;
    return ftLogDensitySaddle(a, b, a * b * (f - 1.0) / den, n * a / den, n * (b * f) / den);
  }
  const double s = (b / a) * f;
  double ls, l1;
  if (s > 1e-290 && s < 1e300) {
    ls = std::log(s);
    l1 = std::log1p(s);
  } else {
    ls = std::log(b) - std::log(a) + std::log(f);
    l1 = s > 1.0 ? ls : 0.0;
  }
  const double lny = s >= 1.0 ? (s < 1e300 ? -std::log1p(1.0 / s) : 0.0) : ls - l1;
  return -a * l1 + b * lny - ftLbeta(a, b);
}

// Both parameters moderate: incomplete beta by the continued fraction on the
// side that converges (the other side is 1 minus the small tail).
double ftTailCF(double a, double b, double f) {
  const double s = (b / a) * f;
  const bool direct = s > (b + 1.0) / (a + 1.0);
  const double lbt = ftLogPrefactor(a, b, f);
  const double r = s > 1.0 ? 1.0 / s : s;  // s = inf: x = 0, y = 1 without inf/inf
  const double x = s > 1.0 ? r / (1.0 + r) : 1.0 / (1.0 + r);
  const double y = s > 1.0 ? 1.0 / (1.0 + r) : r / (1.0 + r);
  constexpr int kMaxIt = 3000;  // <= 300 needed anywhere below kFtailAsymFrom
  if (direct) {
    if (lbt < -745.2) return 0.0;
    return std::exp(lbt) * cecon::betacf(a, b, x, kMaxIt) / a;
  }
  const double lower = std::exp(lbt) * cecon::betacf(b, a, y, kMaxIt) / b;
  return std::min(1.0, std::max(0.0, 1.0 - lower));
}

// 16-point Gauss-Legendre rule on [-1,1] (positive half).
constexpr double kGl16X[8] = {0.0950125098376374401853193, 0.2816035507792589132304605,
                              0.4580167776572273863424194, 0.6178762444026437484466718,
                              0.7554044083550030338951012, 0.8656312023878317438804679,
                              0.9445750230732325760779884, 0.9894009349916499325961542};
constexpr double kGl16W[8] = {0.1894506104550684962853967, 0.1826034150449235888667637,
                              0.1691565193950025381893121, 0.1495959888165767320815017,
                              0.1246289712555338720524763, 0.0951585116824927848099251,
                              0.0622535239386478928628438, 0.0271524594117540948517806};

// ln g(t) for t >= 0 with a, b >= 10 (the saddle-point prefactor of
// ftLogPrefactor, written in e^-t so a large t never overflows).
double ftLogDensityT(double a, double b, double t) {
  const double n = a + b;
  const double em = std::exp(-t);
  const double den = b + a * em;
  return ftLogDensitySaddle(a, b, a * b * (-std::expm1(-t)) / den, n * a * em / den, n * b / den);
}

// Mass of g beyond t0 >= 0. d/dt ln g = -lam, lam = ab(e^t-1)/(a+be^t), and
// d2/dt2 ln g = -kap, kap = n a b e^t/(a+be^t)^2; panels are 5/(lam+sqrt(kap))
// wide, so ln g falls by a few units across each and the 16-point rule
// resolves it. Stops when a panel is below 1e-18 of the running sum.
double ftRightTailQuad(double a, double b, double t0) {
  const double n = a + b;
  if (ftLogDensityT(a, b, t0) < -745.5) return 0.0;
  double sum = 0.0, t = t0;
  for (int panel = 0; panel < 400; ++panel) {
    const double em = std::exp(-t);
    const double den = b + a * em;
    const double lam = a * b * (-std::expm1(-t)) / den;
    const double kap = n * a * b * em / (den * den);
    const double w = 5.0 / (lam + std::sqrt(kap));
    const double half = 0.5 * w, mid = t + half;
    double acc = 0.0;
    for (int i = 0; i < 8; ++i) {
      acc += kGl16W[i] * (std::exp(ftLogDensityT(a, b, mid - half * kGl16X[i])) +
                          std::exp(ftLogDensityT(a, b, mid + half * kGl16X[i])));
    }
    const double pv = half * acc;
    sum += pv;
    t += w;
    if (pv <= 1e-18 * sum) break;
  }
  return sum;
}

// a, b >= 10. Below the mode (f < 1) the mass beyond ln f is 1 minus the mass
// of the reciprocal F(df2,df1) beyond -ln f; for a, b >= 10 neither is small.
double ftTailQuad(double a, double b, double f) {
  const double t0 = std::log(f);
  if (t0 >= 0.0) return ftRightTailQuad(a, b, t0);
  return std::min(1.0, std::max(0.0, 1.0 - ftRightTailQuad(b, a, -t0)));
}

// phi(v) = ((1-e^-v)/v)^(B-1) = sum_k phi_k v^k to order K. ln((1-e^-v)/v) =
// -v/2 + sum_n B_2n v^2n / (2n (2n)!), exponentiated by the usual recurrence.
void ftPhiCoeffs(double B, int K, double* phi) {
  static const double kB2n[12] = {
      1.0 / 6,       -1.0 / 30,      1.0 / 42,      -1.0 / 30,       5.0 / 66,     -691.0 / 2730,
      7.0 / 6,       -3617.0 / 510,  43867.0 / 798, -174611.0 / 330, 854513.0 / 138,
      -236364091.0 / 2730};
  double g[32] = {0};
  g[1] = -0.5 * (B - 1.0);
  double fact = 1.0;
  for (int n = 1; n <= 12 && 2 * n <= K; ++n) {
    fact *= (2.0 * n - 1.0) * (2.0 * n);
    g[2 * n] = (B - 1.0) * kB2n[n - 1] / (2.0 * n * fact);
  }
  phi[0] = 1.0;
  for (int k = 1; k <= K; ++k) {
    double acc = 0.0;
    for (int j = 1; j <= k; ++j) acc += j * g[j] * phi[k - j];
    phi[k] = acc / k;
  }
}

// e^z Gamma(s,z) for z > 1: the incomplete gamma continued fraction.
double ftGammaUpperScaledCF(double s, double z) {
  constexpr double kTiny = 1e-300;
  double bb = z + 1.0 - s, c = 1.0 / kTiny, d = 1.0 / bb, h = d;
  for (int i = 1; i < 100000; ++i) {
    const double an = -i * (i - s);
    bb += 2.0;
    d = an * d + bb;
    if (std::fabs(d) < kTiny) d = kTiny;
    c = bb + an / c;
    if (std::fabs(c) < kTiny) c = kTiny;
    d = 1.0 / d;
    const double del = d * c;
    h *= del;
    if (std::fabs(del - 1.0) < 1e-16) break;
  }
  return std::exp(s * std::log(z)) * h;
}

// Gamma(1+B) - 1, accurate for tiny B (series in zeta values below 0.1).
double ftGamma1pm1(double B) {
  if (B >= 0.1) return std::expm1(std::lgamma(1.0 + B));
  // zeta(2..24): tabulated up to 9, summed directly (40 terms, error < 1e-15)
  // above; built once.
  struct ZetaTable {
    double v[25];
    ZetaTable() {
      static const double lead[8] = {1.6449340668482264, 1.2020569031595943, 1.0823232337111382,
                                     1.0369277551433699, 1.0173430619844491, 1.0083492773819228,
                                     1.0040773561979443, 1.0020083928260822};
      for (int k = 2; k <= 24; ++k) {
        if (k <= 9) {
          v[k] = lead[k - 2];
          continue;
        }
        double zk = 0.0;
        for (int m = 40; m >= 1; --m) {
          zk += std::pow(static_cast<double>(m), -static_cast<double>(k));
        }
        v[k] = zk;
      }
    }
  };
  static const ZetaTable zeta;
  double lg = -0.5772156649015329 * B, p = -B;
  for (int k = 2; k <= 24; ++k) {
    p *= -B;
    lg += zeta.v[k] * p / k;
  }
  return std::expm1(lg);
}

// e^z Gamma(B,z) for 0 < B < 1 and z <= 1 (lz = ln z, -inf at z = 0): the
// small-z expansion, which keeps Gamma(B) - z^B/B in one expm1 difference.
double ftGammaUpperScaledSmallZ(double B, double z, double lz) {
  double ser = 0.0, term = 1.0;  // sum_{j>=1} (-z)^j / (j! (B+j))
  for (int j = 1; j < 60; ++j) {
    term *= -z / j;
    const double t = term / (B + j);
    ser += t;
    if (std::fabs(t) < 1e-18) break;
  }
  const double head = (ftGamma1pm1(B) - std::expm1(B * lz)) / B;
  return std::exp(z) * (head - std::exp(B * lz) * ser);
}

// e^z gamma(s,z) (lower incomplete gamma): the positive power series.
double ftGammaLowerScaled(double s, double z, double lz) {
  double term = 1.0 / s, sum = term;
  for (int j = 1; j < 100000; ++j) {
    term *= z / (s + j);
    sum += term;
    if (term < 1e-18 * sum) break;
  }
  return std::exp(s * lz) * sum;
}

// I_{exp(-v0)}(A,B) = (1/B(A,B)) * integral_{v0}^inf e^{-Av} (1-e^-v)^{B-1} dv
// with (1-e^-v)^{B-1} = v^{B-1} phi(v): the integral is the series
//     (B/Gamma(1+B)) R * sum_k phi_k A^-k Gamma(B+k, z),   z = A v0,
// R = Gamma(A+B)/(Gamma(A) A^B). `upper` selects that tail (the incomplete
// beta itself); otherwise the complementary lower integral with gamma(B+k,z)
// is returned, which is the small one when z is below the gamma median. The
// terms carry e^z, so the result is assembled as exp(ln(pref) + ln(sum) - z):
// e^-z alone underflows (z > 745) while the product is still a normal double.
// The callers use `upper` only above the gamma median, so z <= 1 there means
// B < 1 (the small-z expansion).
double ftAsymTailF(double A, double B, double z, double lz, bool upper) {
  constexpr int kK = 24;
  if (upper && z > 1000.0) return 0.0;  // < e^-1000 z^9 even for B < 10
  const double lnR = (A + B - 0.5) * std::log1p(B / A) - B + ftLgammaCorr(A + B) - ftLgammaCorr(A);
  const double lpref = std::log(B / std::tgamma(1.0 + B)) + lnR;
  double phi[kK + 1];
  ftPhiCoeffs(B, kK, phi);
  double sum = 0.0, Ak = 1.0, G = 0.0;
  if (upper) G = (z > 1.0) ? ftGammaUpperScaledCF(B, z) : ftGammaUpperScaledSmallZ(B, z, lz);
  for (int k = 0; k <= kK; ++k) {
    if (!upper) G = ftGammaLowerScaled(B + k, z, lz);
    const double term = phi[k] * Ak * G;
    sum += term;
    if (k > 3 && std::fabs(term) < 1e-17 * std::fabs(sum)) break;
    Ak /= A;
    // Gamma(s+1,z) = s Gamma(s,z) + z^s e^-z, scaled by e^z; all terms positive.
    if (upper) G = (B + k) * G + std::exp((B + k) * lz);
  }
  if (!(sum > 0.0)) return 0.0;
  const double lres = lpref + std::log(sum) - z;
  return lres < -745.2 ? 0.0 : std::exp(lres);
}

// Median of the gamma(B) distribution, to within a factor of 2: the point
// above which the upper incomplete gamma is the small side.
double ftMedianZ(double B) {
  return B >= 1.0 ? B : std::pow(0.5 * std::tgamma(1.0 + B), 1.0 / B);
}

// v0 = ln(1+w) and ln v0, for w = (u/v) f (inv = false) or (v/u)/f (inv = true),
// without losing w to overflow/underflow at the extremes of f.
void ftLog1pRatio(double u, double v, double f, bool inv, double* v0, double* lv0) {
  const double w = inv ? (v / u) / f : (u / v) * f;
  if (w > 1e-290 && w < 1e300) {
    *v0 = std::log1p(w);
    *lv0 = std::log(*v0);
    return;
  }
  const double lw =
      inv ? std::log(v) - std::log(u) - std::log(f) : std::log(u) - std::log(v) + std::log(f);
  if (w >= 1e300) {  // ln(1+w) = ln w
    *v0 = lw;
    *lv0 = std::log(lw);
  } else {  // ln(1+w) = w
    *v0 = std::exp(lw);
    *lv0 = lw;
  }
}

// One of a, b is >= kFtailAsymFrom and the other < 10. For large a the tail
// is the upper integral beyond v0 = ln(1+s); for large b the complement P is
// the upper integral beyond ln(1+1/s) and Q the lower one. Whichever side is
// the small one is computed directly, the other as 1 minus it.
double ftTailAsym(double a, double b, double f) {
  double v0, lv0;
  if (a > b) {
    ftLog1pRatio(b, a, f, false, &v0, &lv0);
    const double z = a * v0, lz = std::log(a) + lv0;
    if (z > ftMedianZ(b)) return ftAsymTailF(a, b, z, lz, true);
    return std::min(1.0, std::max(0.0, 1.0 - ftAsymTailF(a, b, z, lz, false)));
  }
  ftLog1pRatio(b, a, f, true, &v0, &lv0);
  const double z = b * v0, lz = std::log(b) + lv0;
  if (z <= ftMedianZ(a)) return ftAsymTailF(b, a, z, lz, false);
  return std::min(1.0, std::max(0.0, 1.0 - ftAsymTailF(b, a, z, lz, true)));
}

double stataFtail(double df1, double df2, double f) {
  if (std::isnan(df1) || df1 >= kStataMissingSentinel) return kNaN;
  if (std::isnan(df2) || df2 >= kStataMissingSentinel) return kNaN;
  if (std::isnan(f) || f >= kStataMissingSentinel || f <= -kStataMissingSentinel) return kNaN;
  if (!(df1 >= 2e-10 && df1 <= 2e17) || !(df2 >= 2e-10 && df2 <= 2e17)) return kNaN;
  if (f <= 0.0) return 1.0;
  const double a = df2 / 2.0, b = df1 / 2.0;
  if (std::max(a, b) < kFtailAsymFrom) {
    // Tiny df1 on the side where the CF would hand back 1 - P (the tail is
    // O(df1), so that subtraction costs ~1e-16/df1 in relative terms): with
    // df2 >= 20 the 1/A expansion is accurate here (z = A ln(1+s) stays ~1).
    if (b < 0.05 && a >= 10.0 && (b / a) * f <= (b + 1.0) / (a + 1.0)) return ftTailAsym(a, b, f);
    return ftTailCF(a, b, f);
  }
  if (std::min(a, b) >= 10.0) return ftTailQuad(a, b, f);
  return ftTailAsym(a, b, f);
}

}  // namespace

PyValue fnFtail(const std::vector<PyValue>& args) {
  requireArity(args, 3, 3, "Ftail");
  requireNumeric(args);  // BUG-462
  auto fdf1 = pythonFloat(args[0]);
  auto fdf2 = pythonFloat(args[1]);
  auto ff = pythonFloat(args[2]);
  if (!fdf1 || !fdf2 || !ff) return PyValue::floating(kNaN);
  return PyValue::floating(stataFtail(*fdf1, *fdf2, *ff));
}

// normal(z): standard normal CDF, now cecon::stata_normal (Cody's ANORM),
// which StataBE 19.5 matches bit for bit on 3,409 z (src/core/distributions.h);
// the history below is what it replaced.
// Formerly: upper half (z >= 0): `0.5*(1+erf(z/sqrt2))`,
// not the mathematically-equivalent `0.5*erfc(-z/sqrt2)` -- the two round
// differently in the last bit and only the erf form is bit-identical to the
// oracle (w2c_functions.log normal_macro, normal(1.96) -> .9750021048517796;
// measured both ways, erfc's last digit was off by one ULP).
//
// BUG-662: lower half (z < 0) through the complementary error function. The
// erf form there is 1 + (a number near -1): the cancellation left
// normal(-8) = 6.106226635438361e-16 (StataBE 19.5: 6.22096057427178486e-16,
// 1.8% off) and normal(-10) = normal(-30) = 0 exactly. 0.5*erfc(-z/sqrt2)
// has no subtraction and stays relative-accurate down to the underflow.
PyValue fnNormal(const std::vector<PyValue>& args) {
  requireArity(args, 1, 1, "normal");
  requireNumeric(args);
  auto fz = pythonFloat(args[0]);
  if (!fz) return PyValue::floating(kNaN);
  double z = *fz;
  // A missing z is plain `.`: normal(.a) is `.` (BUG-1360).
  if (std::isnan(z) || z >= kStataMissingSentinel) return PyValue::floating(kStataMissingSentinel);
  return PyValue::floating(cecon::stata_normal(z));
}

// serset-twoway FN: normalden(z), normalden(x, s), normalden(x, m, s) -- the
// normal density (functions.sthlp: normalden(x,m,s) = normalden((x-m)/s)/s; s
// <= 0 is missing). `twoway function normalden(x)` (w2al tf_fn_drop,
// log:2990-3000) reaches it through the macro evaluator; it answered `unknown
// function normalden()`. The density itself is the engine's (BUG-1124,
// cecon::stata_normalden), so `display normalden(x)` and `generate` agree to the
// bit.
PyValue fnNormalden(const std::vector<PyValue>& args) {
  requireArity(args, 1, 3, "normalden");
  requireNumeric(args);  // BUG-462
  std::vector<double> v;
  for (const auto& a : args) {
    auto f = pythonFloat(a);
    if (!f) return PyValue::floating(kNaN);
    if (std::isnan(*f) || *f >= kStataMissingSentinel) return PyValue::floating(kStataMissingSentinel);
    v.push_back(*f);
  }
  double m = 0.0, sd = 1.0;
  if (v.size() == 2) sd = v[1];
  if (v.size() == 3) {
    m = v[1];
    sd = v[2];
  }
  const double d = cecon::stata_normalden(v[0], m, sd);
  return PyValue::floating(std::isnan(d) ? kStataMissingSentinel : d);
}

// invnormal(p): the inverse standard normal CDF, through the engine's own
// AS241 (cecon::stata_invnormal, core/distributions.h) rather than a second
// implementation. A missing argument is plain `.` (invnormal(.a) is `.`,
// BUG-1360); outside (0,1) the engine answers NaN, which is returned as
// the macro layer's finite missing sentinel so that `invnormal(2) == .` is
// true, as for fnFloat (BUG-584; a NaN prints `.` but compares false).
// Oracle w2c_functions.log:320/:331 (invnormal_matrix_lit/_elem, rc 0).
PyValue fnInvnormal(const std::vector<PyValue>& args) {
  requireArity(args, 1, 1, "invnormal");
  requireNumeric(args);
  auto fp = pythonFloat(args[0]);
  if (!fp) return PyValue::floating(kNaN);
  const double p = *fp;
  if (std::isnan(p) || p >= kStataMissingSentinel) return PyValue::floating(kStataMissingSentinel);
  const double z = cecon::stata_invnormal(p);
  return PyValue::floating(std::isnan(z) ? kStataMissingSentinel : z);
}

// The one numeric argument of invlogit()/logit(): a string is Stata's r(109), and
// a missing of any kind comes back as nullopt, which those functions answer with
// a plain `.` (StataBE 19.5, 2026-10-29: invlogit(.a) and logit(.a) print `.`).
std::optional<double> numericArgOrMissing(const PyValue& a) {
  if (a.isStr()) throw PyExc("r(109): type mismatch");
  auto v = pythonFloat(a);
  if (!v || std::isnan(*v) || std::fabs(*v) >= kStataMissingSentinel) return std::nullopt;
  return *v;
}

// BUG-1066: invlogit(x) and logit(x), with the engine's own rules so `display`,
// `scalar`, `local =` and `generate` agree to the bit: invlogit is
// cecon::stata_invlogit_value (core/stata_arith.h; invlogit(2)
// +1.c2f7d5a8a79c9X-001, invlogit(-717.5) 0), logit is ln(x/(1-x)) and missing
// outside the open interval (0,1), as frame.cpp's stata_logit (logit(.25)
// -1.193ea7aad030bX+000; logit(0), logit(1) and logit(2) are `.`).
PyValue fnInvlogit(const std::vector<PyValue>& args) {
  requireArity(args, 1, 1, "invlogit");
  const std::optional<double> x = numericArgOrMissing(args[0]);
  if (!x) return PyValue::floating(kStataMissingSentinel);
  return PyValue::floating(cecon::stata_invlogit_value(*x));
}
PyValue fnLogit(const std::vector<PyValue>& args) {
  requireArity(args, 1, 1, "logit");
  const std::optional<double> x = numericArgOrMissing(args[0]);
  if (!x || !(*x > 0.0 && *x < 1.0)) return PyValue::floating(kStataMissingSentinel);
  return PyValue::floating(std::log(*x / (1.0 - *x)));
}

// BUG-1046: reldif(x,y) = |x-y|/(|y|+1) (StataBE 19.5: reldif(1,2)
// +1.5555555555555X-002, reldif(0,1e-10) +1.b7cdfd9cbef6aX-022, reldif(-1,1) 1).
// Two missings that are the same value are 0 (reldif(.,.), reldif(.a,.a),
// reldif(1e308,1e308)); any other missing operand gives `.` (reldif(5,.),
// reldif(.,5), reldif(.a,.b), reldif(.,.a), and reldif(1e308,-1e308): 1e308 is
// past c(maxdouble), a missing beyond .z, and -1e308 is `.`). The division is
// plain double arithmetic: reldif(8e307,-8e307) is 2. A string is r(109).
PyValue fnReldif(const std::vector<PyValue>& args) {
  requireArity(args, 2, 2, "reldif");
  if (args[0].isStr() || args[1].isStr()) throw PyExc("r(109): type mismatch");
  auto fx = pythonFloat(args[0]);
  auto fy = pythonFloat(args[1]);
  if (!fx || !fy) return PyValue::floating(kStataMissingSentinel);
  const auto missingValue = [](double v) -> std::optional<double> {
    if (std::isnan(v)) return kStataMissingSentinel;
    if (stataExtendedMissingKind(v)) return v;
    return std::nullopt;
  };
  const std::optional<double> mx = missingValue(*fx), my = missingValue(*fy);
  if (mx || my) return PyValue::floating(mx && my && *mx == *my ? 0.0 : kStataMissingSentinel);
  const double d = std::fabs(*fx - *fy) / (std::fabs(*fy) + 1.0);
  return PyValue::floating(std::isfinite(d) && d < kStataMissingSentinel ? d
                                                                         : kStataMissingSentinel);
}

// BUG-1373: tobytes(s[,n]) -- every byte of s as an escape, `\dDDD` (three
// decimal digits) when n is absent or 0 and `\xhh` (two lowercase hex digits)
// for any other n, a missing n included. StataBE 19.5 (2026-10-29):
// tobytes("ab") \d097\d098, tobytes("é") \d195\d169, tobytes("abc",1)
// \x61\x62\x63, tobytes("a",.) and tobytes("a",.4) \x61, tobytes("a",-0)
// \d097, tobytes("") empty; a numeric s or a string n is r(109).
PyValue fnTobytes(const std::vector<PyValue>& args) {
  requireArity(args, 1, 2, "tobytes");
  if (!args[0].isStr() || (args.size() == 2 && args[1].isStr()))
    throw PyExc("r(109): type mismatch");
  bool hex = false;
  if (args.size() == 2) {
    auto n = pythonFloat(args[1]);
    hex = !n || *n != 0.0;  // NaN, the missing band and any nonzero n
  }
  std::string out;
  char buf[8];
  for (unsigned char c : args[0].strVal()) {
    std::snprintf(buf, sizeof(buf), hex ? "\\x%02x" : "\\d%03u", static_cast<unsigned>(c));
    out += buf;
  }
  return PyValue::str(out);
}

// runiform()/runiformint(a,b): doctrine carve-out (①-S, distributional
// agreement only) -- w2c_functions.log's macro arms (runiform_macro,
// runiformint_macro) assert rc 0, never the drawn value; the stream is ours,
// not Stata's.
//
// Review finding (v2): these drew from the C library's rand() and ignored
// `set seed`. They now draw from the engine's Philox4x32-10 (core/rng.h, as
// handlers_resample.cpp does), keyed on the session's `set seed` through
// EvalSession::rngSeed and counted by EvalSession::rngDraws, so `set seed 7`
// followed by the same draws gives the same values. A draw needs the session,
// and ns.fns entries carry none, so the SESSION-BOUND versions are installed
// per call by addSessionRngFns() below (evalArith and evalMacroExpr in
// macros.cpp). The entries in the process-wide registry are what a caller
// WITHOUT that per-call install reaches -- today loops.cpp's `while`/`if`
// condition namespace -- and there they refuse by name rather than draw from
// a stream `set seed` cannot reach. rc 199 is this layer's "not implemented
// in this build" code (errors.cpp coarseRc maps unsupported_command to 199);
// Stata itself has no refusal here.
PyValue fnRuniformMacro(const std::vector<PyValue>& args) {
  requireArity(args, 0, 0, "runiform");
  throw PyExc("r(199): runiform() is not available in this context: it needs the "
              "session's `set seed` state, which only local/global/display "
              "expressions carry in this build");
}
PyValue fnRuniformintMacro(const std::vector<PyValue>& args) {
  requireArity(args, 2, 2, "runiformint");
  throw PyExc("r(199): runiformint() is not available in this context: it needs "
              "the session's `set seed` state, which only local/global/display "
              "expressions carry in this build");
}

namespace {

// Philox's first counter word separates this stream from every other Philox
// consumer; the second is the draw index. The tag is ours (ASCII "MACRORNG").
constexpr uint64_t kMacroRngStream = 0x4D4143524F524E47ull;

uint64_t macroRngDraw64(EvalSession& session) {
  EvalSession::RngSeedReading reading;
  if (session.rngSeed) reading = session.rngSeed();
  if (reading.epoch != session.rngEpochSeen) {
    session.rngEpochSeen = reading.epoch;
    session.rngDraws = 0;
  }
  const uint64_t seed = static_cast<uint64_t>(reading.seed.value_or(123456789LL));
  const uint64_t g = session.rngDraws++;
  const uint64_t hi = cecon::Philox::draw32(seed, kMacroRngStream, 2 * g);
  const uint64_t lo = cecon::Philox::draw32(seed, kMacroRngStream, 2 * g + 1);
  return (hi << 32) | lo;
}

}  // namespace

void addSessionRngFns(PyNamespace& ns, EvalSession& session) {
  EvalSession* sp = &session;
  // 53 random bits at the centre of their 2^-53 cell: strictly inside (0,1).
  ns.fns["runiform"] = [sp](const std::vector<PyValue>& args) -> PyValue {
    requireArity(args, 0, 0, "runiform");
    // The session's Stata stream (core/rng_mt64.h): `local u = runiform()`
    // takes the next draw of the same stream generate uses, as in Stata.
    if (sp->rngMt64) {
      if (cecon::Mt64* g = sp->rngMt64()) return PyValue::floating(g->runiform());
    }
    const uint64_t bits = macroRngDraw64(*sp) >> 11;
    return PyValue::floating((static_cast<double>(bits) + 0.5) * 0x1.0p-53);
  };
  ns.fns["runiformint"] = [sp](const std::vector<PyValue>& args) -> PyValue {
    requireArity(args, 2, 2, "runiformint");
    auto flo = pythonFloat(args[0]);
    auto fhi = pythonFloat(args[1]);
    if (!flo || !fhi) return PyValue::floating(kNaN);
    // Missing bound -> missing (BUG-660 fix round). A bound past 2^53 is not
    // an exactly representable integer and would overflow llround: missing.
    constexpr double kMaxExactInt = 9007199254740992.0;
    if (std::isnan(*flo) || *flo >= kStataMissingSentinel || std::isnan(*fhi) ||
        *fhi >= kStataMissingSentinel || std::fabs(*flo) > kMaxExactInt ||
        std::fabs(*fhi) > kMaxExactInt) {
      return PyValue::floating(kNaN);
    }
    long long a = static_cast<long long>(std::llround(*flo));
    long long b = static_cast<long long>(std::llround(*fhi));
    if (sp->rngMt64) {
      if (cecon::Mt64* g = sp->rngMt64()) {
        // Stata: an inverted range is missing, a degenerate one is a without a
        // draw (core/rng_mt64.h runiformint).
        if (b < a) return PyValue::floating(kNaN);
        return PyValue::integer(static_cast<long long>(
            g->runiformint(static_cast<double>(a), static_cast<double>(b))));
      }
    }
    if (b < a) std::swap(a, b);
    const uint64_t span = static_cast<uint64_t>(b - a) + 1ull;
    // Rejection sampling: `draw % span` is biased unless span divides 2^64,
    // so draws at or past the last whole multiple of span are discarded.
    const uint64_t limit = (UINT64_MAX / span) * span;
    uint64_t draw;
    do {
      draw = macroRngDraw64(*sp);
    } while (draw >= limit);
    return PyValue::integer(a + static_cast<long long>(draw % span));
  };
}

// ---------------------------------------------------------------------------
// String functions

PyValue fnLower(const std::vector<PyValue>& args) {
  requireArity(args, 1, 1, "lower");
  return PyValue::str(asciiLower(pythonStr(args[0])));
}

PyValue fnUpper(const std::vector<PyValue>& args) {
  requireArity(args, 1, 1, "upper");
  return PyValue::str(asciiUpper(pythonStr(args[0])));
}

// trim/strtrim/ltrim/rtrim strip BLANKS (char 32) only, not Python's
// whitespace set: length(strtrim(char(9) + " a " + char(9))) is 5 in StataBE
// 19.5 (w2x_fn_string_math fx_trim_edges).
std::string stripBlanks(const std::string& s, bool left, bool right) {
  size_t a = 0, b = s.size();
  if (left) {
    while (a < b && s[a] == ' ') ++a;
  }
  if (right) {
    while (b > a && s[b - 1] == ' ') --b;
  }
  return s.substr(a, b - a);
}

PyValue fnStrtrim(const std::vector<PyValue>& args) {
  requireArity(args, 1, 1, "strtrim");
  return PyValue::str(stripBlanks(pythonStr(args[0]), true, true));
}

PyValue fnLtrim(const std::vector<PyValue>& args) {
  requireArity(args, 1, 1, "ltrim");
  return PyValue::str(stripBlanks(pythonStr(args[0]), true, false));
}

PyValue fnRtrim(const std::vector<PyValue>& args) {
  requireArity(args, 1, 1, "rtrim");
  return PyValue::str(stripBlanks(pythonStr(args[0]), false, true));
}

// substr(s,n1,n2), StataBE 19.5 (w2x_fn_string_math fx_substr_edges/_neg):
// a negative n1 counts from the end (substr("abcdef",-2,2) "ef"); n1 of 0, a
// missing n1, or a negative n1 reaching before the first character give "";
// a missing n2 runs to the end; n2 <= 0 gives "".
// BUG-1372: BYTES, like length() -- usubstr() is the character form. StataBE
// 19.5 (lane expr-sem x1b/a2.do G2-G8): substr("h\xc3\xa9llo",2,2) is the two
// bytes of e-acute, substr(..,3,2) is "\xa9l", strlen(substr(..,2,1)) is 1.
PyValue fnSubstr(const std::vector<PyValue>& args) {
  requireArity(args, 2, 3, "substr");
  requireNumeric(args, 1);  // BUG-462
  const std::string ss = pythonStr(args[0]);
  const auto fstart = pythonFloat(args[1]);
  if (!fstart || !std::isfinite(*fstart) || std::fabs(*fstart) >= kStataMissingSentinel)
    return PyValue::str("");
  const long long n = static_cast<long long>(ss.size());
  const double st = std::trunc(*fstart);
  if (st == 0.0 || std::fabs(st) > static_cast<double>(n)) return PyValue::str("");
  const long long start = st > 0 ? static_cast<long long>(st) : n + static_cast<long long>(st) + 1;
  long long len = n - start + 1;
  if (args.size() >= 3) {
    const auto fl = pythonFloat(args[2]);
    if (!fl) return PyValue::str("");
    if (!std::isnan(*fl) && std::fabs(*fl) < kStataMissingSentinel) {
      const double l = std::trunc(*fl);
      if (l <= 0.0) return PyValue::str("");
      if (l < static_cast<double>(len)) len = static_cast<long long>(l);
    }
  }
  return PyValue::str(ss.substr(static_cast<size_t>(start - 1), static_cast<size_t>(len)));
}

// length()/strlen() count BYTES: length("é") is 2 (w2x_fn_string_math
// fx_length_edges, fx_case_edges); ustrlen() is the character count.
PyValue fnStrlen(const std::vector<PyValue>& args) {
  requireArity(args, 1, 1, "strlen");
  return PyValue::integer(static_cast<long long>(pythonStr(args[0]).size()));
}

PyValue fnStrpos(const std::vector<PyValue>& args) {
  requireArity(args, 2, 2, "strpos");
  std::string hs = pythonStr(args[0]);
  std::string nd = pythonStr(args[1]);
  // strpos("", "") is 0, strpos("abc", "") 1 (fx_strpos_edges).
  if (hs.empty()) return PyValue::integer(0);
  // BUG-1372: a BYTE position, like substr() and length() (ustrpos() counts
  // characters): strpos("h\xc3\xa9llo","l") is 4 in StataBE 19.5 (x1b/a2.do G3).
  const size_t idx = hs.find(nd);
  return PyValue::integer(idx != std::string::npos ? static_cast<long long>(idx) + 1 : 0);
}

// regexm()/regexr() take a literal pattern string per call and corpora
// re-evaluate the same pattern on every row of a dataset — recompiling
// std::regex from scratch each time is pure waste for the (common) case of
// a handful of distinct patterns hit thousands of times. Memoize compiled
// regexes process-locally, capped so a pathological many-distinct-pattern
// workload can't grow this unbounded. A pattern that fails to compile is
// cached too (as nullopt) — regexm/regexr swallow std::regex_error
// unconditionally (Python: except re.error), so skipping the repeat
// compile-and-throw is safe and doesn't change what's observable.
constexpr size_t kRegexMemoCap = 4096;

std::unordered_map<std::string, std::optional<std::regex>>& regexMemo() {
  static std::unordered_map<std::string, std::optional<std::regex>> memo;
  return memo;
}

// Returns the cached compiled regex for `pattern` (nullptr if `pattern` is
// cached as invalid), or std::nullopt if the memo has nothing for it and is
// at capacity — caller compiles (and catches) locally in that case, same as
// before this memo existed.
std::optional<const std::regex*> memoizedRegex(const std::string& pattern) {
  auto& memo = regexMemo();
  auto it = memo.find(pattern);
  if (it != memo.end()) {
    return it->second ? std::optional<const std::regex*>(&*it->second)
                       : std::optional<const std::regex*>(nullptr);
  }
  if (memo.size() >= kRegexMemoCap) return std::nullopt;
  try {
    auto [ins, _] = memo.emplace(pattern, std::regex(pattern));
    return &*ins->second;
  } catch (const std::regex_error&) {
    memo.emplace(pattern, std::nullopt);
    return nullptr;
  }
}

// BUG-660 fix round: regexs(n) reads the capture groups of the LAST
// SUCCESSFUL regexm() call -- Stata's own contract (regexs() is undefined
// until a preceding regexm() returns true). A plain function-local static,
// process-wide like regexMemo() above, not session-scoped: this evaluator has
// no session threaded into ns.fns entries (they are built once, statically,
// by stataMathNamespace()/postfileStataMathNamespace()), and PIET is
// single-threaded per process the same way that static already assumes. A
// failed regexm() call (no match, or a bad pattern) leaves the state alone,
// matching Stata's "results are only defined right after a TRUE regexm()".
struct RegexMatchState {
  bool valid = false;
  std::vector<std::string> groups;  // groups[0] = whole match, [1..] = captures
};
RegexMatchState& lastRegexMatch() {
  static RegexMatchState state;
  return state;
}

PyValue fnRegexm(const std::vector<PyValue>& args) {
  requireArity(args, 2, 2, "regexm");
  const std::string subject = pythonStr(args[0]);
  const std::string pattern = pythonStr(args[1]);
  auto cached = memoizedRegex(pattern);
  auto search = [&](const std::regex& re) -> bool {
    std::smatch m;
    const bool found = std::regex_search(subject, m, re);
    if (found) {
      RegexMatchState& st = lastRegexMatch();
      st.valid = true;
      st.groups.clear();
      st.groups.reserve(m.size());
      for (size_t g = 0; g < m.size(); ++g) {
        st.groups.push_back(m[g].matched ? m[g].str() : std::string());
      }
    }
    return found;
  };
  try {
    if (cached) {
      if (!*cached) return PyValue::boolean(false);  // Python: except re.error -> False
      return PyValue::boolean(search(**cached));
    }
    std::regex re(pattern);  // memo at capacity — compile without caching
    return PyValue::boolean(search(re));
  } catch (const std::regex_error&) {
    return PyValue::boolean(false);  // Python: except re.error -> False
  }
}

// regexs() IN THE ROW PLANE (piet-row-evaluator-string-data-arms). In a `generate`,
// regexs() inside the SAME expression as its regexm() does not see that
// regexm()'s match: w2c_functions.log regexs_whole (:599, `cond(regexm(s,"A"),
// regexs(0), "NOMATCH")` on 20 rows that all match) stores "" on row 1 with
// type str1 and "(1 missing value generated)"; regexs_group_matched (:609)
// the same; regexs_group_unmatched (:619, group 2 of "(A)(Z)?" never takes
// part) stores "" everywhere, "(20 missing values generated)". The rule this
// evaluator applies, which reproduces those three observables: while a
// RegexsRowScope is open (strmutate.cpp evalRows), regexs() reads the match
// state as it stood when the current expression BEGAN evaluating on the
// current row -- i.e. Stata's regexs() runs before its own expression's
// regexm(), and so sees the previous row's match (none before row 1 in a
// fresh session). A condition (`if regexm(...)`) is a separate expression
// evaluated before the right-hand side, so the documented idiom `generate x
// = regexs(1) if regexm(s, ...)` reads its own row's match. UNPINNED by any
// captured arm: rows 2..20 (this rule gives the previous row's match, "A"),
// and whether the state survives from an earlier command (this rule keeps
// the one process-wide state, as the macro layer always has).
namespace {
int gRegexsRowDepth = 0;
RegexMatchState& regexsRowSnapshot() {
  static RegexMatchState snap;
  return snap;
}
}  // namespace

RegexsRowScope::RegexsRowScope() { ++gRegexsRowDepth; }
RegexsRowScope::~RegexsRowScope() { --gRegexsRowDepth; }
void RegexsRowScope::beginExpression() { regexsRowSnapshot() = lastRegexMatch(); }

// BUG-660 fix round: regexs(n) -- the capture-group readback for regexm()
// above. w2c_functions.log's regexs_macro_standalone arm (`local x =
// regexs(0)` -> "A", rc 0) needs a PRECEDING regexm() in the same evaluator;
// see selftest_w2c_macrofns.cpp for the arm this rests on. An out-of-range n
// or no prior successful match is Stata's own answer for the same case: the
// empty string, not an error.
PyValue fnRegexs(const std::vector<PyValue>& args) {
  requireArity(args, 1, 1, "regexs");
  requireNumeric(args);  // BUG-462
  auto fn = pythonFloat(args[0]);
  if (!fn) return PyValue::str("");
  long long idx;
  try {
    idx = intFromDouble(*fn);
  } catch (const PyExc&) {
    return PyValue::str("");
  }
  const RegexMatchState& st = gRegexsRowDepth > 0 ? regexsRowSnapshot() : lastRegexMatch();
  if (!st.valid || idx < 0 || idx >= static_cast<long long>(st.groups.size())) {
    return PyValue::str("");
  }
  return PyValue::str(st.groups[static_cast<size_t>(idx)]);
}

PyValue fnRegexr(const std::vector<PyValue>& args) {
  requireArity(args, 3, 3, "regexr");
  std::string s = pythonStr(args[0]);
  const std::string pattern = pythonStr(args[1]);
  auto cached = memoizedRegex(pattern);
  // Python re.sub(..., count=1): replace FIRST match only.
  // NOTE dialect divergence: Python replacement processes \1 groups;
  // ECMAScript uses $1 — corpus uses plain replacement strings.
  try {
    if (cached) {
      if (!*cached) return PyValue::str(s);  // Python: except re.error -> str(s)
      return PyValue::str(std::regex_replace(s, **cached, pythonStr(args[2]),
                                              std::regex_constants::format_first_only));
    }
    std::regex re(pattern);  // memo at capacity — compile without caching
    return PyValue::str(std::regex_replace(s, re, pythonStr(args[2]),
                                            std::regex_constants::format_first_only));
  } catch (const std::regex_error&) {
    return PyValue::str(s);  // Python: except re.error -> str(s)
  }
}

// Review finding (v2): ustrregexra() matches on CODE POINTS. Fix round 2
// refused a few byte-sensitive constructs and let the rest through byte-wise
// (`ustrregexra("éé","é+","x")` gave "xx": the `+` applied to é's last byte
// only); the refusal itself surfaced as r(198) "invalid syntax". Now subject,
// pattern and replacement are decoded to UTF-32 (wchar_t is 32 bits on every
// platform this builds for; static_assert below) and matched with
// std::wregex, so `.`, quantifiers and bracket classes see whole code points.
// Measured in StataBE 19.5 (piet-macro-layer-functions probe uregex.log):
// ustrregexra("é",".","x") = "x" and ustrregexra("café","caf","tea") = "teaé".
//
// What ECMAScript still does NOT share with Stata's ICU engine is refused BY
// NAME, rc 199 (this layer's "not implemented in this build" code; errors.cpp
// coarseRc maps unsupported_command to 199 -- Stata has no refusal here, so
// no oracle rc exists), on the `r(NNN): message` channel evalArith forwards:
//  * on a subject with a non-ASCII code point: \b \B \w \W \d \D \s \S and
//    POSIX [:class:] (ICU classifies by Unicode property; std::wregex under
//    the classic locale knows ASCII only -- measured in Stata,
//    ustrregexra("café","[^\w ]","") keeps é, which byte- or classic-locale
//    \w would delete);
//  * case-insensitive matching (4th argument) when subject or pattern has a
//    non-ASCII code point (ICU folds É to é; the classic locale does not);
//  * `.` on a subject holding U+0085 (ICU's `.` excludes NEL, ECMAScript's
//    does not);
//  * ICU-only syntax std::regex would misread or reject: \p \P \X \N \h \H
//    \R \Q \E \A \Z \z \G \K \v \V \e \a, `(?` groups other than (?: (?= (?!,
//    nested `[` / `&&` set operations inside a bracket class;
//  * a replacement holding `\` or a `$` not followed by a digit (ICU and
//    ECMAScript read those differently);
//  * a pattern std::wregex cannot compile, or a subject/pattern/replacement
//    that is not valid UTF-8.
// The subject is never echoed back as if the replacement had run.
static_assert(sizeof(wchar_t) == 4, "ustrregexra decodes to UTF-32 in wchar_t");

void appendUtf8(std::string& out, unsigned cp);  // defined with ustrunescape below

static bool decodeUtf8Strict(const std::string& s, std::wstring& out) {
  out.clear();
  out.reserve(s.size());
  for (size_t i = 0; i < s.size();) {
    size_t n = 0;
    if (!strfns::utf8_seq_len(s, i, n)) return false;
    const unsigned char c = static_cast<unsigned char>(s[i]);
    unsigned cp = n == 1 ? c : n == 2 ? (c & 0x1Fu) : n == 3 ? (c & 0x0Fu) : (c & 0x07u);
    for (size_t k = 1; k < n; ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3Fu);
    out.push_back(static_cast<wchar_t>(cp));
    i += n;
  }
  return true;
}

static bool hasNonAscii(const std::wstring& w) {
  for (wchar_t c : w) {
    if (static_cast<unsigned>(c) >= 0x80) return true;
  }
  return false;
}

// The first construct of `pattern`/`repl` whose ECMAScript reading differs
// from ICU's for this input, as the text to name in the refusal; "" if none.
static std::string ustrregexDivergence(const std::wstring& pattern, const std::wstring& repl,
                                       bool subjectNonAscii, bool subjectHasNel,
                                       bool caseInsensitive, bool patternNonAscii) {
  if (caseInsensitive && (subjectNonAscii || patternNonAscii)) {
    return "case-insensitive matching of non-ASCII text";
  }
  bool inClass = false;
  for (size_t i = 0; i < pattern.size(); ++i) {
    const wchar_t c = pattern[i];
    if (c == L'\\' && i + 1 < pattern.size()) {
      const wchar_t e = pattern[i + 1];
      if (std::wstring(L"pPXNhHRQEAZzGKvVea").find(e) != std::wstring::npos) {
        return std::string("\\") + static_cast<char>(e);
      }
      if (subjectNonAscii && std::wstring(L"bBwWdDsS").find(e) != std::wstring::npos) {
        return std::string("\\") + static_cast<char>(e) + " on non-ASCII text";
      }
      ++i;
      continue;
    }
    if (inClass) {
      if (c == L'[' && i + 1 < pattern.size() && pattern[i + 1] == L':') {
        if (subjectNonAscii) return "POSIX [:class:] on non-ASCII text";
      } else if (c == L'[') {
        return "a nested [ set inside a bracket class";
      } else if (c == L'&' && i + 1 < pattern.size() && pattern[i + 1] == L'&') {
        return "&& set intersection inside a bracket class";
      } else if (c == L']') {
        inClass = false;
      }
      continue;
    }
    if (c == L'[') {
      inClass = true;
      // A `]` or `^]` right after the opening bracket is a literal member.
      if (i + 1 < pattern.size() && pattern[i + 1] == L'^') ++i;
      if (i + 1 < pattern.size() && pattern[i + 1] == L']') ++i;
      continue;
    }
    if (c == L'(' && i + 1 < pattern.size() && pattern[i + 1] == L'?') {
      const wchar_t g = i + 2 < pattern.size() ? pattern[i + 2] : L'\0';
      if (g != L':' && g != L'=' && g != L'!') return "a (? group other than (?: (?= (?!";
    }
    if (c == L'.' && subjectHasNel) return ". on text holding U+0085";
  }
  for (size_t i = 0; i < repl.size(); ++i) {
    if (repl[i] == L'\\') return "\\ in the replacement";
    if (repl[i] == L'$' && !(i + 1 < repl.size() && repl[i + 1] >= L'0' && repl[i + 1] <= L'9')) {
      return "$ not followed by a group number in the replacement";
    }
  }
  return "";
}

[[noreturn]] static void refuseUstrregex(const char* fn, const std::string& why) {
  throw PyExc(std::string("r(199): ") + fn + "(): " + why +
              " is not supported in this build (Stata matches with ICU regular expressions)");
}
[[noreturn]] static void refuseUstrregexra(const std::string& why) {
  refuseUstrregex("ustrregexra", why);
}

// BUG-660: ustrregexra(s,pattern,replace[,i]) -- replace every match (the
// "ra" suffix). Optional 4th argument: nonzero requests case-insensitive
// matching (measured in Stata: ustrregexra("aAbB","a","X",1) -> "XXbB").
PyValue fnUstrregexra(const std::vector<PyValue>& args) {
  requireArity(args, 3, 4, "ustrregexra");
  requireNumeric(args, 3);  // BUG-462
  std::wstring ws, wp, wr;
  if (!decodeUtf8Strict(pythonStr(args[0]), ws) || !decodeUtf8Strict(pythonStr(args[1]), wp) ||
      !decodeUtf8Strict(pythonStr(args[2]), wr)) {
    refuseUstrregexra("an argument that is not valid UTF-8");
  }
  bool caseInsensitive = false;
  if (args.size() == 4) {
    auto ci = pythonFloat(args[3]);
    caseInsensitive = ci && *ci != 0.0 && !std::isnan(*ci);
  }
  const std::string why =
      ustrregexDivergence(wp, wr, hasNonAscii(ws), ws.find(static_cast<wchar_t>(0x85)) != std::wstring::npos,
                          caseInsensitive, hasNonAscii(wp));
  if (!why.empty()) refuseUstrregexra(why);
  std::wstring replaced;
  try {
    auto flags = std::regex_constants::ECMAScript;
    if (caseInsensitive) flags |= std::regex_constants::icase;
    const std::wregex re(wp, flags);
    replaced = std::regex_replace(ws, re, wr);
  } catch (const std::regex_error&) {
    refuseUstrregexra("a pattern std::regex cannot compile");
  }
  std::string out;
  out.reserve(replaced.size());
  for (wchar_t c : replaced) appendUtf8(out, static_cast<unsigned>(c));
  return PyValue::str(out);
}

// ustrregexm(s,re[,noc]) -- 1 when re matches somewhere in s, else 0
// (w2c_functions.log:1132 ustrregexm_data: ustrregexm("caf\u00e9 na\u00efve",
// "caf") is 1). The same UTF-32 matcher and the same named refusals as
// ustrregexra() above, so a construct ECMAScript reads differently from ICU
// is refused (r(199)), never answered. It does not touch regexm()'s capture
// state: regexs() reads regexm() only (ustrregexs() is a separate function,
// not registered here).
PyValue fnUstrregexm(const std::vector<PyValue>& args) {
  requireArity(args, 2, 3, "ustrregexm");
  requireNumeric(args, 2);  // BUG-462
  std::wstring ws, wp;
  if (!decodeUtf8Strict(pythonStr(args[0]), ws) || !decodeUtf8Strict(pythonStr(args[1]), wp)) {
    refuseUstrregex("ustrregexm", "an argument that is not valid UTF-8");
  }
  bool caseInsensitive = false;
  if (args.size() == 3) {
    auto ci = pythonFloat(args[2]);
    caseInsensitive = ci && *ci != 0.0 && !std::isnan(*ci);
  }
  const std::string why =
      ustrregexDivergence(wp, std::wstring(), hasNonAscii(ws),
                          ws.find(static_cast<wchar_t>(0x85)) != std::wstring::npos,
                          caseInsensitive, hasNonAscii(wp));
  if (!why.empty()) refuseUstrregex("ustrregexm", why);
  try {
    auto flags = std::regex_constants::ECMAScript;
    if (caseInsensitive) flags |= std::regex_constants::icase;
    const std::wregex re(wp, flags);
    return PyValue::integer(std::regex_search(ws, re) ? 1 : 0);
  } catch (const std::regex_error&) {
    refuseUstrregex("ustrregexm", "a pattern std::regex cannot compile");
  }
}

// ustrlen(s) -- the number of Unicode characters (code points) of s, where
// strlen()/length() count bytes (w2c_functions.log:1121 ustrlen_data:
// ustrlen("caf\u00e9 na\u00efve") is 10, strlen of it is 12). utf8Len's
// counting of an invalid byte (each non-continuation byte is one) is unpinned.
PyValue fnUstrlen(const std::vector<PyValue>& args) {
  requireArity(args, 1, 1, "ustrlen");
  return PyValue::integer(static_cast<long long>(utf8Len(pythonStr(args[0]))));
}

// ustrleft(s,n) -- the first n Unicode characters of s (w2c_functions.log:1110
// ustrleft_data: ustrleft("caf\u00e9 na\u00efve",4) is "caf\u00e9", 5 bytes).
// Unpinned, by design: n at or past the length gives s, n <= 0 gives "", a
// missing n gives s (usubstr's "missing length runs to the end" reading).
PyValue fnUstrleft(const std::vector<PyValue>& args) {
  requireArity(args, 2, 2, "ustrleft");
  requireNumeric(args, 1);  // BUG-462
  const std::string s = pythonStr(args[0]);
  const auto fn = pythonFloat(args[1]);
  if (!fn || std::isnan(*fn) || std::fabs(*fn) >= kStataMissingSentinel) return PyValue::str(s);
  const double n = std::trunc(*fn);
  if (n <= 0.0) return PyValue::str("");
  const size_t len = utf8Len(s);
  if (n >= static_cast<double>(len)) return PyValue::str(s);
  return PyValue::str(utf8Slice(s, 0, static_cast<size_t>(n)));
}

// ustrtrim(s) -- s without its leading and trailing Unicode white space
// (w2c_functions.log:1163 ustrtrim_data: ustrtrim(" caf\u00e9 na\u00efve ") is
// "caf\u00e9 na\u00efve"). Stata's help: "removes leading and trailing Unicode
// whitespace characters and blanks". DESIGN, unpinned beyond the space: the
// set is ICU's White_Space property (U+0009-000D, 0020, 0085, 00A0, 1680,
// 2000-200A, 2028, 2029, 202F, 205F, 3000). Interior characters are kept.
// strtrim() (blanks only) is a different function and is not touched.
static bool isUnicodeWhiteSpace(unsigned cp) {
  return (cp >= 0x09 && cp <= 0x0D) || cp == 0x20 || cp == 0x85 || cp == 0xA0 ||
         cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 ||
         cp == 0x202F || cp == 0x205F || cp == 0x3000;
}
PyValue fnUstrtrim(const std::vector<PyValue>& args) {
  requireArity(args, 1, 1, "ustrtrim");
  const std::string s = pythonStr(args[0]);
  // Decode one code point at byte i; an invalid byte is one non-space unit.
  auto at = [&s](size_t i, size_t& n) -> unsigned {
    if (!strfns::utf8_seq_len(s, i, n)) {
      n = 1;
      return 0xFFFFFFFFu;
    }
    const unsigned char c = static_cast<unsigned char>(s[i]);
    unsigned cp = n == 1 ? c : n == 2 ? (c & 0x1Fu) : n == 3 ? (c & 0x0Fu) : (c & 0x07u);
    for (size_t k = 1; k < n; ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3Fu);
    return cp;
  };
  size_t begin = s.size(), end = 0;
  for (size_t i = 0; i < s.size();) {
    size_t n = 1;
    const unsigned cp = at(i, n);
    if (!isUnicodeWhiteSpace(cp)) {
      if (begin == s.size()) begin = i;
      end = i + n;
    }
    i += n;
  }
  if (begin >= end) return PyValue::str("");
  return PyValue::str(s.substr(begin, end - begin));
}

// real(s): a missing token keeps its kind (real(".a") is .a, fx_real_edges);
// what Python's float() reads but Stata does not (inf, nan, 1e400) is `.`, and
// so is a non-numeric string -- the value `.` itself, not a NaN, so
// `real("abc") == .` is 1 (w2x_fn_string_math.log:782 f4).
PyValue fnReal(const std::vector<PyValue>& args) {
  requireArity(args, 1, 1, "real");
  if (args[0].isStr()) {
    const std::string t = stripBlanks(args[0].strVal(), true, true);
    if (t == ".") return PyValue::floating(kStataMissingSentinel);
    if (t.size() == 2 && t[0] == '.' && t[1] >= 'a' && t[1] <= 'z')
      return PyValue::floating(stataMissingWithKind(t[1] - 'a' + 1));
    if (t.find('_') != std::string::npos) return PyValue::floating(kStataMissingSentinel);
  }
  auto f = pythonFloat(args[0]);
  if (f && args[0].isStr() && !(std::fabs(*f) < kStataMissingSentinel)) return PyValue::floating(kStataMissingSentinel);
  return PyValue::floating(f ? *f : kStataMissingSentinel);
}

// BUG-660: reverse(s) -- byte reversal (not a `ustr*` name, so plain-byte
// like Stata's own non-Unicode string functions; the oracle's fixture is
// ASCII, so byte- vs codepoint-reversal isn't distinguished here, only
// documented as the narrower of the two).
PyValue fnReverse(const std::vector<PyValue>& args) {
  requireArity(args, 1, 1, "reverse");
  std::string s = pythonStr(args[0]);
  std::reverse(s.begin(), s.end());
  return PyValue::str(s);
}

// strrpos() -- the position of the LAST match (oracle arm strrpos_macro); an
// empty needle is 0. BUG-1372: a BYTE position, like strpos():
// strrpos("h\xc3\xa9llo","l") is 5 in StataBE 19.5 (x1b/a2.do G9).
PyValue fnStrrpos(const std::vector<PyValue>& args) {
  requireArity(args, 2, 2, "strrpos");
  std::string hs = pythonStr(args[0]);
  std::string nd = pythonStr(args[1]);
  const size_t idx = nd.empty() ? std::string::npos : hs.rfind(nd);
  return PyValue::integer(idx != std::string::npos ? static_cast<long long>(idx) + 1 : 0);
}

PyValue fnWordcount(const std::vector<PyValue>& args) {
  requireArity(args, 1, 1, "wordcount");
  return PyValue::integer(static_cast<long long>(pySplit(pythonStr(args[0])).size()));
}

// strtoname(s): every character that isn't a letter/digit/underscore becomes
// an underscore, one-for-one (no run-collapsing — w2c_functions.log's
// strtoname_macro turns the fixture's two leading/two interior spaces into
// two/two underscores, not one/one).
//
// Unicode (StataBE 19.5, scratch-r150/uni-v/p3.do over every general category):
// strtoname works on BYTES -- an ASCII byte outside [A-Za-z0-9_] becomes `_`,
// every byte >= 0x80 (so every non-ASCII character, whatever its category, a
// non-ASCII digit first included) is kept whole, and the result is cut at 32
// BYTES (strtoname("é" x 40) has 16 characters). Stata may leave half a character
// at the cut; this cut backs off to a character boundary so the string stays valid
// UTF-8. An ASCII digit first gets a `_` in front (suppressed by strtoname(s, 0));
// an empty string stays empty.
PyValue fnStrtoname(const std::vector<PyValue>& args) {
  requireArity(args, 1, 2, "strtoname");
  requireNumeric(args, 1);  // BUG-462
  std::string s = pythonStr(args[0]);
  bool prefix = true;
  if (args.size() > 1) {
    auto p = pythonFloat(args[1]);
    if (!p) throw PyExc("strtoname: p not numeric");
    prefix = *p != 0;
  }
  std::string out;
  out.reserve(s.size() + 1);
  for (unsigned char c : s) {
    out.push_back((c >= 0x80 || std::isalnum(c) || c == '_') ? static_cast<char>(c) : '_');
  }
  if (prefix && !out.empty() && out[0] >= '0' && out[0] <= '9') out.insert(out.begin(), '_');
  if (out.size() > 32) {
    size_t cut = 32;
    while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) --cut;
    out.resize(cut);
  }
  return PyValue::str(out);
}

// strmatch(s,pattern): `*`/`?` shell-style wildcards, no character classes
// (Stata's strmatch() grammar). Hand-rolled rather than POSIX fnmatch() to
// keep this file platform-neutral (macros.cpp's own glob helper already
// carries a _WIN32 fnmatch fallback for exactly this reason; duplicating that
// split here for a two-wildcard grammar isn't worth it). Classic iterative
// matcher: `*` records a backtrack point, `?`/literal advance both cursors.
bool globMatchWildcard(const std::string& pattern, const std::string& s) {
  size_t p = 0, si = 0, star = std::string::npos, mark = 0;
  while (si < s.size()) {
    if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == s[si])) {
      ++p;
      ++si;
    } else if (p < pattern.size() && pattern[p] == '*') {
      star = p++;
      mark = si;
    } else if (star != std::string::npos) {
      p = star + 1;
      si = ++mark;
    } else {
      return false;
    }
  }
  while (p < pattern.size() && pattern[p] == '*') ++p;
  return p == pattern.size();
}

PyValue fnStrmatch(const std::vector<PyValue>& args) {
  requireArity(args, 2, 2, "strmatch");
  return PyValue::boolean(globMatchWildcard(pythonStr(args[1]), pythonStr(args[0])));
}

// ustrunescape(s): decode Stata's fixed-width `\uXXXX` escapes (exactly four
// hex digits, no surrogate pairs in the grammar) to UTF-8; any other
// backslash sequence passes through literally, matching BUG-502's "Stata has
// no escape sequences" rule for the enclosing string literal — this function
// is the one place that DOES interpret `\u`, by name, on its argument text.
void appendUtf8(std::string& out, unsigned cp) {
  if (cp <= 0x7F) {
    out.push_back(static_cast<char>(cp));
  } else if (cp <= 0x7FF) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp <= 0xFFFF) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    // Four-byte form (review v2): ustrregexra re-encodes whole UTF-32 text,
    // which can hold code points past the BMP; \uXXXX never reaches here.
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

PyValue fnUstrunescape(const std::vector<PyValue>& args) {
  requireArity(args, 1, 1, "ustrunescape");
  std::string s = pythonStr(args[0]);
  std::string out;
  out.reserve(s.size());
  auto isHex = [](unsigned char c) { return std::isxdigit(c) != 0; };
  for (size_t i = 0; i < s.size();) {
    if (s[i] == '\\' && i + 5 < s.size() && s[i + 1] == 'u' &&
        isHex(static_cast<unsigned char>(s[i + 2])) && isHex(static_cast<unsigned char>(s[i + 3])) &&
        isHex(static_cast<unsigned char>(s[i + 4])) && isHex(static_cast<unsigned char>(s[i + 5]))) {
      unsigned cp = static_cast<unsigned>(std::stoul(s.substr(i + 2, 4), nullptr, 16));
      appendUtf8(out, cp);
      i += 6;
    } else {
      out.push_back(s[i]);
      ++i;
    }
  }
  return PyValue::str(out);
}

PyValue fnWord(const std::vector<PyValue>& args) {
  requireArity(args, 2, 2, "word");
  requireNumeric(args, 1);  // BUG-462
  try {
    auto tokens = pySplit(pythonStr(args[0]));
    auto fn = pythonFloat(args[1]);
    if (!fn) throw PyExc("word: n not numeric");
    long long idx = intFromDouble(*fn) - 1;
    // A negative n counts from the end: word("a b c d", -1) is "d", -4 "a",
    // -5 "" (fx_word_edges).
    if (idx < -1) idx = static_cast<long long>(tokens.size()) + idx + 1;
    if (idx >= 0 && idx < static_cast<long long>(tokens.size())) {
      return PyValue::str(tokens[static_cast<size_t>(idx)]);
    }
    return PyValue::str("");
  } catch (const PyExc&) {
    return PyValue::str("");
  }
}

PyValue fnSubinstr(const std::vector<PyValue>& args) {
  requireArity(args, 3, 4, "subinstr");
  requireNumeric(args, 3);  // BUG-462
  try {
    std::string ss = pythonStr(args[0]);
    std::string sf = pythonStr(args[1]);
    std::string st = pythonStr(args[2]);
    if (sf.empty()) return PyValue::str(ss);
    long long count = -1;
    if (args.size() >= 4) {
      // Python: try float(n); if not isnan: count=int(fn); except -> -1.
      auto fn = pythonFloat(args[3]);
      if (fn && !std::isnan(*fn)) {
        try {
          count = intFromDouble(*fn);
        } catch (const PyExc&) {
          // int(inf) OverflowError not in (TypeError, ValueError):
          // escapes to the outer except Exception -> return str(s).
          if (std::isinf(*fn)) return PyValue::str(ss);
          count = -1;
        }
      }
      // fn parse failure or NaN: count stays -1
    }
    // A negative count replaces nothing: subinstr("abcb", "b", "x", -1) is
    // "abcb" (fx_subinstr_negn). Only a missing count means every occurrence.
    if (args.size() >= 4 && count < 0) {
      auto fn = pythonFloat(args[3]);
      if (fn && !std::isnan(*fn) && *fn < 0) return PyValue::str(ss);
    }
    if (count < 0) return PyValue::str(pyReplace(ss, sf, st));
    if (count == 0) return PyValue::str(ss);
    return PyValue::str(pyReplace(ss, sf, st, count));
  } catch (const PyExc&) {
    // Python: except Exception -> str(s) if s is not None else ""
    return PyValue::str(pythonStr(args[0]));
  }
}

PyValue fnProper(const std::vector<PyValue>& args) {
  requireArity(args, 1, 1, "proper");
  // Python str.title(): uppercase a cased char that follows a non-cased
  // char, lowercase the rest. ASCII approximation (see header caveat).
  std::string s = pythonStr(args[0]);
  std::string out;
  out.reserve(s.size());
  bool prevAlpha = false;
  for (unsigned char c : s) {
    bool alpha = std::isalpha(c) != 0;
    if (alpha && !prevAlpha) {
      out.push_back(static_cast<char>(std::toupper(c)));
    } else if (alpha) {
      out.push_back(static_cast<char>(std::tolower(c)));
    } else {
      out.push_back(static_cast<char>(c));
    }
    prevAlpha = alpha;
  }
  return PyValue::str(out);
}

PyValue fnString(const std::vector<PyValue>& args) {
  requireArity(args, 1, 2, "string");
  const PyValue& n = args[0];
  // BUG-249: a STRING first argument WITH a format is a TYPE error in Stata —
  // r(109) "type mismatch" on both the display and the `local =` surfaces
  // (exprfns_contract_oracle.log, G|strarg rows, rc=109 both). It used to be
  // returned unchanged before the format was even looked at. The one-argument
  // form stays the Python ancestor's pass-through — not in the capture, so not
  // claimed. The `r(NNN): message` prefix is the exact-rc channel evalArith
  // forwards to tryCaptureDefinition (see its raisedError contract).
  // BUG-462: the one-argument form too -- string("5") is r(109) in StataBE
  // 19.5 (x1b/a20.do); it was the Python ancestor's pass-through.
  if (n.isStr()) throw PyExc("r(109): type mismatch");
  double fv = n.numAsDouble();  // float() of Bool/Int/Float never fails
  // BUG-206 (third half). `isnan` ALONE IS THE WRONG MISSING TEST ON THIS PATH.
  //
  // The expression layer does not represent Stata missing as a NaN — it uses the
  // large FINITE sentinel kStataMissingSentinel (exprfns.h, and evalArith's own
  // isStataMissingResult tests `fabs(d) >= kStataMissingSentinel` for exactly this
  // reason). So `replaceBareDotMissing` turns a bare `.` into that sentinel, the
  // isnan guard never fired, and this fell through to the renderer with
  // 8.988465674311579e+307 as an ordinary number:
  //     string(.)            -> [8.988466e+307]   Stata: [.]
  //     string(., "%9.0g")   -> [8.99e+307]       Stata: [.]
  // Oracle StataNow 19.5, in-log marker, both rows measured on both arms.
  //
  // ATTRIBUTION, because the two halves are not the same age. The ONE-ARGUMENT
  // leak is PRE-EXISTING and independent of BUG-206: `string(.)` contains no
  // percent sign, so it was always admitted by the whitelist and always printed
  // the sentinel. The TWO-ARGUMENT leak is one this stint newly EXPOSED — before
  // the whitelist widening `string(., "%9.0g")` was refused and echoed as visibly
  // unevaluated text, and afterwards it would have emitted a confident wrong
  // number. Fixing only the half I exposed and leaving the other would be
  // arbitrary: it is one guard, and one guard is the fix.
  //
  // BUG-249 closes the extended-code half BUG-206 filed: the sentinel now
  // CARRIES the kind (see kStataMissingKindStep in exprfns.h — `.a`..`.z` are
  // 26 distinct doubles above the base sentinel, written by macros.cpp's
  // replaceBareDotMissing). Oracle: string(.a) and string(.a,"%9.0g") -> `.a`,
  // string(.z,"%9.0g") -> `.z` (exprfns_contract_oracle.log, G|dot* rows). A
  // negative-side missing (the evaluator negates the sentinel like any double)
  // keeps rendering plain `.`, as before.
  std::optional<std::string> sfmt;
  if (args.size() >= 2) {
    sfmt = pyStripAscii(pythonStr(args[1]), true, true);
  }
  // BUG-918: a left-justified format keeps its trailing pad for a missing
  // value too (`string(., "%-9.0g")` is `.        `; w2z_tostring tz_fmt_left,
  // tostring.ado's own `string(v, fmt)`). Any other format leaves the token.
  auto missingToken = [&](std::string tok) {
    if (sfmt && !sfmt->empty() && (*sfmt)[0] == '%') {
      tok = piet::fmt::padMissingLeftJustify(std::move(tok), *sfmt);
    }
    return PyValue::str(std::move(tok));
  };
  if (std::isnan(fv)) return missingToken(".");
  if (std::fabs(fv) >= kStataMissingSentinel) {
    auto kind = stataExtendedMissingKind(fv);
    if (kind && *kind >= 1) {
      return missingToken(std::string(".") + static_cast<char>('a' + *kind - 1));
    }
    return missingToken(".");
  }
  // gFormat/applyStataFmt may throw PyExc (int(inf) OverflowError escapes
  // Python's except (TypeError, ValueError)) — let it propagate: the whole
  // expression fails, mirroring the oracle.
  if (sfmt && !sfmt->empty() && (*sfmt)[0] == '%') {
    return PyValue::str(applyStataFmt(fv, *sfmt));
  }
  // BUG-206 (second half). A SECOND ARGUMENT THAT IS NOT A FORMAT IS AN ERROR,
  // AND STATA'S ANSWER FOR IT IS THE EMPTY STRING — not "ignore the argument and
  // render the default way", which is what falling through to gFormat() does.
  //
  // This branch was unobservable until the whitelist fix above made the two-arg
  // form reachable at all, and it matters precisely BECAUSE of that fix: pre-fix,
  // `string(42, "abc%")` was refused and echoed as its own source text — visibly
  // unevaluated. Post-fix it would have evaluated to `42`, trading an obvious
  // non-answer for a confident wrong one. That is the exact trade the BUG-196
  // note in macros.cpp argues against, so the reachability fix has to bring this
  // with it rather than leave it for later.
  //
  // Oracle, StataNow 19.5, marker-judged, `string(42.5, "<fmt>")` with length
  // reported per cell (scratch capture this stint; bytes quoted in BUG-249):
  //     ""      -> [] len=0        "9.0g"  -> [] len=0
  //     "abc%"  -> [] len=0        "g"     -> [] len=0
  // versus the `%`-prefixed formats above, which all render (`%9.0g` -> [42.5],
  // `%9.3f` -> [42.500], `%20.10f` -> [42.5000000000]).
  //
  // ONLY the no-leading-`%` case is claimed here, because that is the boundary
  // this function already tested and the one the fix above newly exposes. Stata
  // also answers `""` for `%`, `%%`, `%9s` and `%-9s` — `%`-prefixed strings that
  // are not NUMERIC formats — and this still renders those via gFormat. That is a
  // pre-existing divergence on a path the whitelist fix does not change the
  // reachability of, so it is FILED (BUG-249) with its captured bytes rather than
  // guessed at here; telling "not a numeric format" from "a format this engine
  // has not implemented" needs the date/business-calendar families decided too
  // (`%tc` and `%td` both render real values in Stata and reach gFormat here).
  if (sfmt) return PyValue::str(std::string());
  // FU-616/FU-641/FU-654: string(x) is %9.0g through the shared renderer, not
  // gFormat's C %.7g: string(.00001) .00001, string(1e-6) 1.00e-06,
  // string(123456789) 1.23e+08 (w2x_fn_string_math fx_string1_small/_edges).
  return PyValue::str(piet::fmt::general(positiveZero(fv), 9, 0));
}

PyValue fnChar(const std::vector<PyValue>& args) {
  requireArity(args, 1, 1, "char");
  requireNumeric(args);  // BUG-462
  auto f = pythonFloat(args[0]);
  if (!f) return PyValue::str("");
  long long code;
  try {
    code = intFromDouble(*f);
  } catch (const PyExc&) {
    // Python catches (TypeError, ValueError) here: int(nan) is ValueError
    // -> caught -> "". int(inf) is OverflowError -> NOT caught -> escapes
    // and fails the whole expression. Mirror by rethrowing on inf.
    if (std::isinf(*f)) throw;
    return PyValue::str("");
  }
  if (code < 0 || code > 0x10FFFF) return PyValue::str("");
  // Encode the codepoint as UTF-8 (Python chr + str semantics; lone
  // surrogates excluded from the corpus — json cannot carry them).
  std::string out;
  unsigned int cp = static_cast<unsigned int>(code);
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
  return PyValue::str(out);
}

PyValue fnAbbrev(const std::vector<PyValue>& args) {
  requireArity(args, 2, 2, "abbrev");
  requireNumeric(args, 1);  // BUG-462
  std::string ss = pythonStr(args[0]);
  try {
    auto fn = pythonFloat(args[1]);
    if (!fn) throw PyExc("abbrev: n not numeric");
    long long ln = intFromDouble(*fn);
    if (ln < 5) ln = 5;
    size_t cpLen = utf8Len(ss);
    if (cpLen <= static_cast<size_t>(ln)) return PyValue::str(ss);
    // Python: ss[: ln - 2] + "~" + ss[-1]
    std::string head = utf8Slice(ss, 0, static_cast<size_t>(ln - 2));
    std::string last = utf8Slice(ss, cpLen - 1, cpLen);
    return PyValue::str(head + "~" + last);
  } catch (const PyExc&) {
    // Python: except Exception -> str(s) if s is not None else ""
    return PyValue::str(ss);
  }
}

// BUG-371: the Stata date family, second layer. cobra's frame.cpp parser answers
// `gen dt = mdy(...)`; THIS namespace answers `display mdy(...)`, `scalar`, and any
// expression the string-mutation evaluator handles. Both call the same
// cecon:: helpers (data/stata_dates.h), so the two layers cannot give different
// answers for the same date — a class of drift this campaign has paid for before.
//
// A missing argument reaches this layer as the finite sentinel band (|x| >= 2^1023,
// kStataMissingSentinel), not as the engine's NaN, and the cecon:: helpers read it
// as an ordinary huge number: hh(.) was 2562047788015, mm(.) 12 and minutes(.) a
// 300-digit number, where Stata answers `.` (w2y_fn_datetime fd_hhmm_edges,
// fd_minutes_edges). Every date/time argument is read through dateArg, which hands
// the helpers NaN for it.
// BUG-462: a string is r(109), as in every numeric slot: year("5"),
// mdy(1,"2",2000), dofm("3"), hh("5"), ym(2000,"1") and date()'s topyear
// "2050" all refuse in StataBE 19.5 (x1b/a20.do).
std::optional<double> dateArg(const PyValue& v) {
  if (v.isStr()) throw PyExc("r(109): type mismatch");
  auto f = pythonFloat(v);
  if (f && !(std::fabs(*f) < kStataMissingSentinel)) return kNaN;
  return f;
}

PyValue fnMdy(const std::vector<PyValue>& args) {
  requireArity(args, 3, 3, "mdy");
  auto a = dateArg(args[0]);
  auto b = dateArg(args[1]);
  auto c = dateArg(args[2]);
  if (!a || !b || !c) return PyValue::floating(kNaN);
  return PyValue::floating(cecon::stata_mdy(*a, *b, *c));
}

PyValue datePartFn(const std::vector<PyValue>& args, cecon::DatePart part, const char* name) {
  requireArity(args, 1, 1, name);
  auto v = dateArg(args[0]);
  if (!v) return PyValue::floating(kNaN);
  return PyValue::floating(cecon::stata_date_part(*v, part));
}

PyValue fnYear(const std::vector<PyValue>& a) { return datePartFn(a, cecon::DP_year, "year"); }
PyValue fnMonth(const std::vector<PyValue>& a) { return datePartFn(a, cecon::DP_month, "month"); }
PyValue fnDay(const std::vector<PyValue>& a) { return datePartFn(a, cecon::DP_day, "day"); }
PyValue fnDow(const std::vector<PyValue>& a) { return datePartFn(a, cecon::DP_dow, "dow"); }
PyValue fnDoy(const std::vector<PyValue>& a) { return datePartFn(a, cecon::DP_doy, "doy"); }
PyValue fnQuarter(const std::vector<PyValue>& a) { return datePartFn(a, cecon::DP_quarter, "quarter"); }
PyValue fnHalfyear(const std::vector<PyValue>& a) { return datePartFn(a, cecon::DP_half, "halfyear"); }

// AER scope 2 / B3: the rest of the Stata time scales, same contract as the
// BUG-371 family above -- every one of these calls the SHARED cecon:: helper in
// data/stata_dates.h that cobra's frame.cpp calls, so `display mofd(d)` and
// `gen m = mofd(d)` cannot disagree. Values are pinned to
// tests/validate/b3_funcs_oracle.json.
//
// Missing propagates as Stata's `.`; a string argument is r(109) (dateArg).
template <typename F>
PyValue timeUnary(const std::vector<PyValue>& args, const char* name, F f) {
  requireArity(args, 1, 1, name);
  auto v = dateArg(args[0]);
  if (!v) return PyValue::floating(kNaN);
  return PyValue::floating(f(*v));
}
template <typename F>
PyValue timeBinary(const std::vector<PyValue>& args, const char* name, F f) {
  requireArity(args, 2, 2, name);
  auto a = dateArg(args[0]);
  auto b = dateArg(args[1]);
  if (!a || !b) return PyValue::floating(kNaN);
  return PyValue::floating(f(*a, *b));
}

PyValue fnMofd(const std::vector<PyValue>& a) { return timeUnary(a, "mofd", [](double x) { return cecon::stata_scale_of_day(x, cecon::DS_month); }); }
PyValue fnQofd(const std::vector<PyValue>& a) { return timeUnary(a, "qofd", [](double x) { return cecon::stata_scale_of_day(x, cecon::DS_quarter); }); }
PyValue fnHofd(const std::vector<PyValue>& a) { return timeUnary(a, "hofd", [](double x) { return cecon::stata_scale_of_day(x, cecon::DS_half); }); }
PyValue fnYofd(const std::vector<PyValue>& a) { return timeUnary(a, "yofd", [](double x) { return cecon::stata_scale_of_day(x, cecon::DS_year); }); }
PyValue fnWofd(const std::vector<PyValue>& a) { return timeUnary(a, "wofd", [](double x) { return cecon::stata_scale_of_day(x, cecon::DS_week); }); }
PyValue fnWeek(const std::vector<PyValue>& a) { return timeUnary(a, "week", [](double x) { return cecon::stata_week_of_day(x); }); }
PyValue fnDofm(const std::vector<PyValue>& a) { return timeUnary(a, "dofm", [](double x) { return cecon::stata_day_of_scale(x, cecon::DS_month); }); }
PyValue fnDofq(const std::vector<PyValue>& a) { return timeUnary(a, "dofq", [](double x) { return cecon::stata_day_of_scale(x, cecon::DS_quarter); }); }
PyValue fnDofh(const std::vector<PyValue>& a) { return timeUnary(a, "dofh", [](double x) { return cecon::stata_day_of_scale(x, cecon::DS_half); }); }
PyValue fnDofy(const std::vector<PyValue>& a) { return timeUnary(a, "dofy", [](double x) { return cecon::stata_day_of_scale(x, cecon::DS_year); }); }
PyValue fnDofw(const std::vector<PyValue>& a) { return timeUnary(a, "dofw", [](double x) { return cecon::stata_day_of_scale(x, cecon::DS_week); }); }
PyValue fnCofd(const std::vector<PyValue>& a) { return timeUnary(a, "cofd", [](double x) { return cecon::stata_cofd(x); }); }
PyValue fnCofdBig(const std::vector<PyValue>& a) { return timeUnary(a, "Cofd", [](double x) { return cecon::stata_Cofd(x); }); }
PyValue fnDofc(const std::vector<PyValue>& a) { return timeUnary(a, "dofc", [](double x) { return cecon::stata_dofc(x); }); }
PyValue fnDofcBig(const std::vector<PyValue>& a) { return timeUnary(a, "dofC", [](double x) { return cecon::stata_dofC(x); }); }
PyValue fnHh(const std::vector<PyValue>& a) { return timeUnary(a, "hh", [](double x) { return cecon::stata_clock_part(x, cecon::CLK_hour, false); }); }
PyValue fnMm(const std::vector<PyValue>& a) { return timeUnary(a, "mm", [](double x) { return cecon::stata_clock_part(x, cecon::CLK_minute, false); }); }
PyValue fnSs(const std::vector<PyValue>& a) { return timeUnary(a, "ss", [](double x) { return cecon::stata_clock_part(x, cecon::CLK_second, false); }); }
PyValue fnHhC(const std::vector<PyValue>& a) { return timeUnary(a, "hhC", [](double x) { return cecon::stata_clock_part(x, cecon::CLK_hour, true); }); }
PyValue fnMmC(const std::vector<PyValue>& a) { return timeUnary(a, "mmC", [](double x) { return cecon::stata_clock_part(x, cecon::CLK_minute, true); }); }
PyValue fnSsC(const std::vector<PyValue>& a) { return timeUnary(a, "ssC", [](double x) { return cecon::stata_clock_part(x, cecon::CLK_second, true); }); }
PyValue fnMsofseconds(const std::vector<PyValue>& a) { return timeUnary(a, "msofseconds", [](double x) { return cecon::stata_msofseconds(x); }); }
PyValue fnMsofminutes(const std::vector<PyValue>& a) { return timeUnary(a, "msofminutes", [](double x) { return cecon::stata_msofminutes(x); }); }
PyValue fnMsofhours(const std::vector<PyValue>& a) { return timeUnary(a, "msofhours", [](double x) { return cecon::stata_msofhours(x); }); }
PyValue fnSeconds(const std::vector<PyValue>& a) { return timeUnary(a, "seconds", [](double x) { return cecon::stata_seconds(x); }); }
PyValue fnMinutes(const std::vector<PyValue>& a) { return timeUnary(a, "minutes", [](double x) { return cecon::stata_minutes(x); }); }
PyValue fnHours(const std::vector<PyValue>& a) { return timeUnary(a, "hours", [](double x) { return cecon::stata_hours(x); }); }

PyValue fnYm(const std::vector<PyValue>& a) { return timeBinary(a, "ym", [](double x, double y) { return cecon::stata_ym(x, y); }); }
PyValue fnYq(const std::vector<PyValue>& a) { return timeBinary(a, "yq", [](double x, double y) { return cecon::stata_yq(x, y); }); }
PyValue fnYh(const std::vector<PyValue>& a) { return timeBinary(a, "yh", [](double x, double y) { return cecon::stata_yh(x, y); }); }
PyValue fnYw(const std::vector<PyValue>& a) { return timeBinary(a, "yw", [](double x, double y) { return cecon::stata_yw(x, y); }); }

PyValue fnHms(const std::vector<PyValue>& args) {
  requireArity(args, 3, 3, "hms");
  requireNumeric(args);  // BUG-462
  auto h = pythonFloat(args[0]); auto m = pythonFloat(args[1]); auto s = pythonFloat(args[2]);
  if (!h || !m || !s) return PyValue::floating(kNaN);
  return PyValue::floating(cecon::stata_hms(*h, *m, *s));
}

PyValue fnDhms(const std::vector<PyValue>& args) {
  requireArity(args, 4, 4, "dhms");
  requireNumeric(args);  // BUG-462
  auto d = pythonFloat(args[0]); auto h = pythonFloat(args[1]);
  auto m = pythonFloat(args[2]); auto s = pythonFloat(args[3]);
  if (!d || !h || !m || !s) return PyValue::floating(kNaN);
  return PyValue::floating(cecon::stata_dhms(*d, *h, *m, *s));
}

// The engine-only remainder, measured by diffing `cobra functions` against
// `piet_native --dump-functions`: asinh cos ln ln1m ln1p sin tan existed in
// cobra's FNS[] and in no PIET layer, so `display sin(0)` did not fall back --
// it ABORTED the do-file with an engine_error, the same shape as mi().
//
// These deliberately use the ENGINE's missing convention (frame.cpp's fin():
// a non-finite result is Stata's `.`) rather than this file's inherited Python
// one, where sqrt(-1) and log(0) THROW. log/log10/exp now have Stata entries
// below (fnLogStata & co., w2x_fn_string_math), registered over mathNamespace's
// Python ones for Stata callers only, and so do sqrt and abs (fnSqrtStata,
// BUG-1360); the postfile registry keeps the Python five.
double engineFin(double v) { return std::isfinite(v) ? v : kNaN; }

template <typename F>
PyValue mathUnaryStata(const std::vector<PyValue>& args, const char* name, F f) {
  requireArity(args, 1, 1, name);
  requireNumeric(args);
  auto v = pythonFloat(args[0]);
  if (!v) return PyValue::floating(kNaN);
  if (std::isnan(*v)) return PyValue::floating(kStataMissingSentinel);
  // A missing argument is the 2^1023 band here, not a number: ln(.) and
  // asinh(.) are `.`, never ln(2^1023) = 709.78 (fx_log_edges, fx_asinh_edges).
  // An overflow to -inf is not missing (it is below `.`): exp(-1e300*1e10) is 0.
  if (*v >= kStataMissingSentinel) return PyValue::floating(kStataMissingSentinel);
  // An out-of-domain result (engineFin's NaN: log(0), log(-1)) or an overflow is
  // `.` itself, so `log(0) == .` is 1 (w2x_fn_string_math.log:1065 f1;
  // missing_range_oracle.log:82).
  const double r = f(*v);
  return PyValue::floating((std::isnan(r) || std::fabs(r) >= kStataMissingSentinel)
                               ? kStataMissingSentinel
                               : r);
}

PyValue fnLn(const std::vector<PyValue>& a) {
  return mathUnaryStata(a, "ln", [](double x) { return engineFin(std::log(x)); });
}
PyValue fnSin(const std::vector<PyValue>& a) {
  return mathUnaryStata(a, "sin", [](double x) { return std::sin(x); });
}
PyValue fnCos(const std::vector<PyValue>& a) {
  return mathUnaryStata(a, "cos", [](double x) { return std::cos(x); });
}
PyValue fnTan(const std::vector<PyValue>& a) {
  return mathUnaryStata(a, "tan", [](double x) { return std::tan(x); });
}
// Stata's asinh is ln(x + sqrt(x^2 + 1)), odd-symmetric: asinh(1e-10) is
// +1.b7cdffffa18d8X-022 where libm's asinh gives ...fd9d7bdbb (fx_asinh_edges).
// Where x^2 overflows, ln|x| + ln 2 (asinh(1e300) +1.59bbfd8b83e43X+009).
PyValue fnAsinh(const std::vector<PyValue>& a) {
  return mathUnaryStata(a, "asinh", [](double x) {
    const double ax = std::fabs(x);
    const double sq = ax * ax + 1.0;
    const double r = std::isfinite(sq) ? std::log(ax + std::sqrt(sq)) : std::log(ax) + M_LN2;
    return engineFin(x < 0 ? -r : r);
  });
}
// log/ln, log10 and exp with Stata's answers, overriding mathNamespace()'s
// Python-semantics entries (which postfile keeps): a missing or out-of-domain
// argument is `.`; log10(x) is ln(x)/ln(10) (log10(1000) +1.7ffffffffffffX+001,
// fx_log10_edges); exp past Stata's largest double (2^1023) is `.` and exp of an
// argument below -709 is 0 (exp(709.78) and exp(710) `.`, exp(-745) 0,
// fx_exp_edges; exp(-717.5) 0 and exp(-709) libm's subnormal, StataBE bits,
// cecon::stata_exp_value in core/stata_arith.h).
PyValue fnLogStata(const std::vector<PyValue>& a) {
  return mathUnaryStata(a, "log", [](double x) { return x > 0 ? std::log(x) : kNaN; });
}
PyValue fnLog10Stata(const std::vector<PyValue>& a) {
  return mathUnaryStata(a, "log10", [](double x) { return x > 0 ? std::log(x) / M_LN10 : kNaN; });
}
PyValue fnExpStata(const std::vector<PyValue>& a) {
  return mathUnaryStata(a, "exp", [](double x) {
    return engineFin(cecon::stata_exp_value(x));
  });
}

// sqrt and abs, over mathNamespace()'s Python entries for the Stata callers
// (postfile keeps those): a missing argument is plain `.` -- sqrt(.) was
// sqrt(2^1023) = 9.48e+153 and abs(.a) kept the `.a` -- and sqrt of a negative
// number is `.` rather than Python's math-domain error, which failed the whole
// expression, so `sqrt(-1) >= .` printed `.` where Stata answers 1 (BUG-1343,
// BUG-1360).
PyValue fnSqrtStata(const std::vector<PyValue>& a) {
  return mathUnaryStata(a, "sqrt", [](double x) { return engineFin(std::sqrt(x)); });
}
PyValue fnAbsStata(const std::vector<PyValue>& a) {
  return mathUnaryStata(a, "abs", [](double x) { return std::fabs(x); });
}

// floor and ceil, over mathNamespace()'s Python entries for the Stata callers
// (postfile keeps those): an infinity passes through instead of raising
// OverflowError, as in Stata -- floor(-1e300*1e10) is -inf, so missing() of it
// is 0 -- a missing argument keeps its kind (floor(.a) is .a, BUG-1371), and a
// string is r(109).
template <typename F>
PyValue integerPartStata(const std::vector<PyValue>& args, const char* name, F f) {
  requireArity(args, 1, 1, name);
  requireNumeric(args);
  if (!args[0].isFloat()) return PyValue::integer(args[0].intVal());
  const double r = f(args[0].floatVal());
  if (!std::isfinite(r) || std::fabs(r) >= 9.3e18) return PyValue::floating(r);
  return PyValue::integer(static_cast<long long>(r));
}
PyValue fnFloorStata(const std::vector<PyValue>& a) {
  return integerPartStata(a, "floor", [](double x) { return std::floor(x); });
}
PyValue fnCeilStata(const std::vector<PyValue>& a) {
  return integerPartStata(a, "ceil", [](double x) { return std::ceil(x); });
}

PyValue fnLn1p(const std::vector<PyValue>& a) {
  return mathUnaryStata(a, "ln1p", [](double x) { return engineFin(std::log1p(x)); });
}
PyValue fnLn1m(const std::vector<PyValue>& a) {
  return mathUnaryStata(a, "ln1m", [](double x) { return engineFin(std::log1p(-x)); });
}

// int() truncates toward zero. It was in cobra's FNS[] and NOT here, so
// `gen t = int(x)` worked while `display int(3.7)` answered r(1) -- the same
// one-layer landing BUG-431 left behind for mi(), found by the same census.
// ---------------------------------------------------------------------------
// AER scope 2 / B3 stage (a): string-argument date translation + the string
// family. These have no engine counterpart by construction (frame.cpp refuses a
// string operand), so unlike the time scales they are single-layer -- the same
// way substr/strpos/subinstr already are. The parsing rules live in
// piet/eval/stata_datemask.h and the numbers they build come from the shared
// cecon:: calendar, so `date("2020-03-15","YMD")` and `mdy(3,15,2020)` agree.

// A translator called with the wrong number of arguments is Stata's r(198), not a
// value: `scalar s = date("01jan2010")` and `generate double g =
// date("01jan2010")` both answer 198 (w2y_fn_datetime fd_date1). The `r(198):`
// prefix is the channel evalArith (scalar/local/display) and the row evaluator
// (strmutate) forward as the exact code.
PyValue translateFn(const std::vector<PyValue>& args, datemask::Kind kind,
                    const char* name) {
  if (args.size() < 2 || args.size() > 3)
    throw PyExc(std::string("r(198): ") + name + "() takes a string, a mask and an optional topyear");
  const std::string s = pythonStr(args[0]);
  const std::string mask = pythonStr(args[1]);
  bool have_top = false;
  double top = 0.0;
  if (args.size() == 3) {
    auto t = dateArg(args[2]);
    if (!t || std::isnan(*t)) return PyValue::floating(kNaN);
    have_top = true;
    top = *t;
  }
  return PyValue::floating(datemask::translate(s, mask, kind, have_top, top));
}

PyValue fnDate(const std::vector<PyValue>& a) { return translateFn(a, datemask::Kind::Daily, "date"); }
PyValue fnDaily(const std::vector<PyValue>& a) { return translateFn(a, datemask::Kind::Daily, "daily"); }
PyValue fnClock(const std::vector<PyValue>& a) { return translateFn(a, datemask::Kind::Clock, "clock"); }
PyValue fnClockBig(const std::vector<PyValue>& a) { return translateFn(a, datemask::Kind::ClockLeap, "Clock"); }
PyValue fnMonthly(const std::vector<PyValue>& a) { return translateFn(a, datemask::Kind::Monthly, "monthly"); }
PyValue fnQuarterly(const std::vector<PyValue>& a) { return translateFn(a, datemask::Kind::Quarterly, "quarterly"); }
PyValue fnWeekly(const std::vector<PyValue>& a) { return translateFn(a, datemask::Kind::Weekly, "weekly"); }
PyValue fnHalfyearly(const std::vector<PyValue>& a) { return translateFn(a, datemask::Kind::Halfyearly, "halfyearly"); }
PyValue fnYearly(const std::vector<PyValue>& a) { return translateFn(a, datemask::Kind::Yearly, "yearly"); }

// The str*trim family is NOT an alias of this file's existing ltrim/rtrim/trim.
// Those inherit Python's str.strip(), which eats tabs; Stata's blank is a SPACE
// and nothing else (measured: strltrim("\t x") is unchanged). Landing the Stata
// spellings on the Stata rule and leaving the Python-derived spellings alone is
// deliberate -- the older names are pinned by the frozen 1.16.0 corpus and
// changing them is its own adjudication (filed, not fixed here).
// Business calendars. bofd("cal", d) maps a daily date onto a business-day
// index and dofb inverts it; both need a .stbcal calendar file, and PIET has no
// calendar loader yet (that is workstream B2). Registering them to answer
// MISSING is not a stub -- it is what Stata itself answers for a calendar it
// cannot find: `di bofd("nosuchcal", td(15mar2020))` is `.` with rc 0, measured.
// This machine's StataBE ships exactly one calendar (ado/base/s/simple.stbcal),
// so paper 066's "sp500" is missing in real Stata here too unless the
// replication package brings its own. The alternative -- an honest error --
// would refuse a line Stata accepts, which is the same gate-1 violation the
// plan's prose prescribed for bad date masks.
PyValue fnBofd(const std::vector<PyValue>& args) {
  requireArity(args, 2, 2, "bofd");
  // A STRING in the date slot is Stata's r(109) type mismatch (measured: a
  // string date and swapped arguments both answer 109). The numeric-constant
  // half of the same rule is caught earlier, by macros.cpp's
  // stringFnArgSpecs table (BUG-456's landing).
  if (args[1].isStr()) throw PyExc("r(109): type mismatch");
  std::shared_ptr<const bcal::BusinessCalendar> cal = bcal::lookup(pythonStr(args[0]));
  if (!cal) return PyValue::floating(kNaN);  // unknown calendar -> missing, rc 0
  auto d = pythonFloat(args[1]);
  if (!d) return PyValue::floating(kNaN);
  return PyValue::floating(bcal::bofd(*cal, *d));
}
PyValue fnDofb(const std::vector<PyValue>& args) {
  requireArity(args, 2, 2, "dofb");
  if (args[0].isStr()) throw PyExc("r(109): type mismatch");
  std::shared_ptr<const bcal::BusinessCalendar> cal = bcal::lookup(pythonStr(args[1]));
  if (!cal) return PyValue::floating(kNaN);
  auto i = pythonFloat(args[0]);
  if (!i) return PyValue::floating(kNaN);
  return PyValue::floating(bcal::dofb(*cal, *i));
}

PyValue fnStritrim(const std::vector<PyValue>& args) {
  requireArity(args, 1, 1, "stritrim");
  return PyValue::str(strfns::stritrim(pythonStr(args[0])));
}
PyValue fnStrltrim(const std::vector<PyValue>& args) {
  requireArity(args, 1, 1, "strltrim");
  return PyValue::str(strfns::strltrim(pythonStr(args[0])));
}
PyValue fnStrrtrim(const std::vector<PyValue>& args) {
  requireArity(args, 1, 1, "strrtrim");
  return PyValue::str(strfns::strrtrim(pythonStr(args[0])));
}
PyValue fnUstrfix(const std::vector<PyValue>& args) {
  requireArity(args, 1, 2, "ustrfix");
  const std::string s = pythonStr(args[0]);
  if (args.size() == 2) return PyValue::str(strfns::ustrfix(s, pythonStr(args[1]), true));
  return PyValue::str(strfns::ustrfix(s, "", false));
}
PyValue fnSoundex(const std::vector<PyValue>& args) {
  requireArity(args, 1, 1, "soundex");
  return PyValue::str(strfns::soundex(pythonStr(args[0])));
}
PyValue fnSoundexNara(const std::vector<PyValue>& args) {
  requireArity(args, 1, 1, "soundex_nara");
  return PyValue::str(strfns::soundex_nara(pythonStr(args[0])));
}

PyValue fnInt(const std::vector<PyValue>& args) {
  requireArity(args, 1, 1, "int");
  requireNumeric(args);  // BUG-462
  auto v = pythonFloat(args[0]);
  if (!v) return PyValue::floating(kNaN);
  return PyValue::floating(std::isnan(*v) ? kNaN : std::trunc(*v));
}

// BUG-471 T2: Stata's float(x) — x rounded to float precision, as a double
// (w2e_float_default.log fn_float_eq_count :1784: `count if g_u13 ==
// float(1/3)` is 5). Missing in -> missing out, the payload kept (`.a` stays
// `.a`: fn_float_missing :1818, fn_float_sysmiss :1828). A finite value past
// float's range is system missing (fn_float_1e300 :1839), the bound being
// Stata's largest float 0x1.fffffep+126 — the engine's round_to_store rule
// (src/data/frame_internal.h), restated because that header is engine-only.
PyValue fnFloat(const std::vector<PyValue>& args) {
  requireArity(args, 1, 1, "float");
  requireNumeric(args);  // BUG-462
  auto v = pythonFloat(args[0]);
  if (!v) return PyValue::floating(kNaN);
  const double x = *v;
  // Signed, as in fnMissing: only x >= the sentinel is a Stata missing. A
  // finite negative past float's range (float(-1e308)) falls through to `.`.
  if (std::isnan(x) || x >= kStataMissingSentinel) return PyValue::floating(x);
  // The overflow answer is this layer's own `.` (the sentinel a bare `.`
  // becomes), not a NaN: a NaN prints `.` but compares false, so
  // `float(1e300) >= .` answered 0 where Stata answers 1.
  if (std::fabs(x) >= 0x1p+127) return PyValue::floating(kStataMissingSentinel);
  const double f = static_cast<double>(static_cast<float>(x));
  return PyValue::floating(std::fabs(f) > 0x1.fffffep+126 ? kStataMissingSentinel : f);
}

// ---------------------------------------------------------------------------
// Registry

void addStataFns(PyNamespace& ns) {
  ns.fns["inlist"] = fnInlist;
  ns.fns["inrange"] = fnInrange;
  ns.fns["cond"] = fnCond;
  ns.fns["mod"] = fnMod;
  ns.fns["missing"] = fnMissing;
  ns.fns["lower"] = fnLower;
  ns.fns["upper"] = fnUpper;
  ns.fns["strtrim"] = fnStrtrim;
  ns.fns["trim"] = fnStrtrim;  // Stata alias: trim() == strtrim()
  ns.fns["ltrim"] = fnLtrim;
  ns.fns["rtrim"] = fnRtrim;
  ns.fns["substr"] = fnSubstr;
  ns.fns["strlen"] = fnStrlen;
  ns.fns["length"] = fnStrlen;  // length() of a string == strlen()
  ns.fns["strpos"] = fnStrpos;
  ns.fns["regexm"] = fnRegexm;
  ns.fns["regexr"] = fnRegexr;
  ns.fns["real"] = fnReal;
  ns.fns["word"] = fnWord;
  ns.fns["subinstr"] = fnSubinstr;
  ns.fns["proper"] = fnProper;
  ns.fns["string"] = fnString;
  ns.fns["char"] = fnChar;
  ns.fns["abbrev"] = fnAbbrev;
  // BUG-371 date family — see the implementations above.
  ns.fns["mdy"] = fnMdy;
  ns.fns["year"] = fnYear;
  ns.fns["month"] = fnMonth;
  ns.fns["day"] = fnDay;
  ns.fns["dow"] = fnDow;
  ns.fns["doy"] = fnDoy;
  ns.fns["quarter"] = fnQuarter;
  ns.fns["halfyear"] = fnHalfyear;
  // BUG-431 repair: `mi` reached cobra's FNS[] as a synonym for missing() and
  // never reached this layer, so `display mi(1)` STOPPED a do-file with
  // "NameError: name 'mi' is not defined" while `gen m = mi(x)` succeeded. Same
  // function pointer, not a second implementation -- the synonym rule this file
  // already applies to trim/length.
  ns.fns["mi"] = fnMissing;
  ns.fns["int"] = fnInt;
  ns.fns["float"] = fnFloat;  // BUG-471 T2
  // The engine-only seven (see engineFin above).
  ns.fns["ln"] = fnLn;
  ns.fns["log"] = fnLogStata;
  ns.fns["log10"] = fnLog10Stata;
  ns.fns["exp"] = fnExpStata;
  ns.fns["sin"] = fnSin;
  ns.fns["cos"] = fnCos;
  ns.fns["tan"] = fnTan;
  ns.fns["asinh"] = fnAsinh;
  ns.fns["ln1p"] = fnLn1p;
  ns.fns["ln1m"] = fnLn1m;
  ns.fns["sqrt"] = fnSqrtStata;  // BUG-1343/1360: over mathNamespace()'s Python sqrt
  ns.fns["abs"] = fnAbsStata;    // BUG-1360: abs(.a) is `.`
  ns.fns["floor"] = fnFloorStata;
  ns.fns["ceil"] = fnCeilStata;
  // B3(a): string-argument date translation.
  ns.fns["date"] = fnDate;
  ns.fns["daily"] = fnDaily;
  ns.fns["clock"] = fnClock;
  ns.fns["Clock"] = fnClockBig;
  ns.fns["monthly"] = fnMonthly;
  ns.fns["quarterly"] = fnQuarterly;
  ns.fns["weekly"] = fnWeekly;
  ns.fns["halfyearly"] = fnHalfyearly;
  ns.fns["yearly"] = fnYearly;
  // B3(a): the string family. strlower/strupper ARE the existing ASCII-only
  // lower/upper -- Stata's are ASCII-only too (strlower("ÀÉÎ") is unchanged),
  // so what this file documented as a parity caveat turns out to be parity.
  ns.fns["strlower"] = fnLower;
  ns.fns["strupper"] = fnUpper;
  ns.fns["stritrim"] = fnStritrim;
  ns.fns["strltrim"] = fnStrltrim;
  ns.fns["strrtrim"] = fnStrrtrim;
  ns.fns["ustrfix"] = fnUstrfix;
  ns.fns["soundex"] = fnSoundex;
  ns.fns["soundex_nara"] = fnSoundexNara;
  // Business calendars: registered, and missing until B2 lands a .stbcal loader.
  ns.fns["bofd"] = fnBofd;
  ns.fns["dofb"] = fnDofb;
  // B3 time scales.
  ns.fns["mofd"] = fnMofd;
  ns.fns["qofd"] = fnQofd;
  ns.fns["hofd"] = fnHofd;
  ns.fns["yofd"] = fnYofd;
  ns.fns["wofd"] = fnWofd;
  ns.fns["week"] = fnWeek;
  ns.fns["dofm"] = fnDofm;
  ns.fns["dofq"] = fnDofq;
  ns.fns["dofh"] = fnDofh;
  ns.fns["dofy"] = fnDofy;
  ns.fns["dofw"] = fnDofw;
  ns.fns["cofd"] = fnCofd;
  ns.fns["Cofd"] = fnCofdBig;
  ns.fns["dofc"] = fnDofc;
  ns.fns["dofC"] = fnDofcBig;
  ns.fns["hh"] = fnHh;
  ns.fns["mm"] = fnMm;
  ns.fns["ss"] = fnSs;
  ns.fns["hhC"] = fnHhC;
  ns.fns["mmC"] = fnMmC;
  ns.fns["ssC"] = fnSsC;
  ns.fns["msofseconds"] = fnMsofseconds;
  ns.fns["msofminutes"] = fnMsofminutes;
  ns.fns["msofhours"] = fnMsofhours;
  ns.fns["seconds"] = fnSeconds;
  ns.fns["minutes"] = fnMinutes;
  ns.fns["hours"] = fnHours;
  ns.fns["ym"] = fnYm;
  ns.fns["yq"] = fnYq;
  ns.fns["yh"] = fnYh;
  ns.fns["yw"] = fnYw;
  ns.fns["hms"] = fnHms;
  ns.fns["dhms"] = fnDhms;
  // BUG-660: piet-macro-layer-functions — the macro/arith-layer function
  // arms of w2c_functions.log. round/min/max OVERRIDE the entries
  // mathNamespace() already installed (see fnRound/fnMinMaxStata's own
  // comments for why the Python-semantics originals are wrong for Stata
  // callers); everything else here is a new name.
  //
  // sum()/group()/runiform()/runiformint() are DELIBERATELY NOT here — see
  // addStataMacroOnlyFns() below, which registers them instead, into the two
  // NO-ROW-PLANE registries (stataMathNamespace()/postfileStataMathNamespace())
  // only. addStataFns() is ALSO strmutate.cpp's baseNamespace() -- the
  // ROW-plane string generate/replace/count-if evaluator, which walks actual
  // data rows -- and sum()/group() answering their macro-layer, single-
  // evaluation shape there would be a silent wrong answer for `generate str
  // t = string(sum(x))` (fnSum's own comment: "outside of one a single
  // evaluation just answers x itself" — inside a row plane, Stata wants a
  // running total -- strmutate.cpp installs that one, addRowPlaneSum). runiform()/
  // runiformint() move for the same reason: a per-evaluation draw reaching a
  // row-plane `generate` would not be the engine's seeded per-row stream.
  // The registry entries themselves refuse by name (r(199)); a session-bound
  // drawing version is installed per call by evalArith/evalMacroExpr (see
  // addSessionRngFns). loops.cpp's `while`/`if` condition namespace reaches
  // only the refusing registry entries.
  ns.fns["round"] = fnRound;
  ns.fns["min"] = fnMinStata;
  ns.fns["max"] = fnMaxStata;
  ns.fns["sign"] = fnSign;
  ns.fns["ttail"] = fnTtail;
  ns.fns["invttail"] = fnInvttail;
  ns.fns["invt"] = fnInvt;
  ns.fns["Ftail"] = fnFtail;
  ns.fns["normal"] = fnNormal;
  ns.fns["normalden"] = fnNormalden;
  ns.fns["invnormal"] = fnInvnormal;
  ns.fns["invlogit"] = fnInvlogit;  // BUG-1066
  ns.fns["logit"] = fnLogit;
  ns.fns["reldif"] = fnReldif;  // BUG-1046
  ns.fns["tobytes"] = fnTobytes;  // BUG-1373
  ns.fns["reverse"] = fnReverse;
  ns.fns["strrpos"] = fnStrrpos;
  ns.fns["wordcount"] = fnWordcount;
  ns.fns["strtoname"] = fnStrtoname;
  ns.fns["strmatch"] = fnStrmatch;
  ns.fns["ustrunescape"] = fnUstrunescape;
  ns.fns["ustrregexra"] = fnUstrregexra;
  // strproper()/proper() and strofreal()/string() and usubinstr()/subinstr()
  // are the same functions under Stata's newer names (verified against the
  // oracle: strproper_macro's word-boundary title-casing is exactly
  // fnProper's algorithm; strofreal_macro's "%5.2f" rendering is exactly
  // fnString's 2-arg format path; usubinstr_macro's single-occurrence
  // replace-all is exactly fnSubinstr's `.`-count fallthrough) — same
  // function pointer, not a second implementation, the mi/trim/length
  // pattern already used above.
  ns.fns["strproper"] = fnProper;
  ns.fns["strofreal"] = fnString;
  ns.fns["usubinstr"] = fnSubinstr;
  // piet-row-evaluator-string-data-arms: the data arms of w2c_functions.log
  // with no macro twin. itrim() is Stata's older name of stritrim() (same
  // pointer: itrim_data :440 "  Ab cD " is exactly stritrim's internal-run
  // collapse); the four ustr* names are new functions above. Each name is
  // also in strmutate.cpp's stringFnNames(), the row evaluator's routing
  // vocabulary.
  ns.fns["itrim"] = fnStritrim;
  ns.fns["ustrlen"] = fnUstrlen;
  ns.fns["ustrleft"] = fnUstrleft;
  ns.fns["ustrtrim"] = fnUstrtrim;
  ns.fns["ustrregexm"] = fnUstrregexm;
  // The old comment here said 36 while the map held 32; registry.cpp:1702 records
  // that discrepancy as the reason --dump-functions reads the MAPS. Corrected to
  // the counted truth and kept counted: 74 = 32 pre-existing + mi + int +
  // 7 engine-only math + 33 time functions + 17 B3(a) string/translation
  // + 2 business-calendar. BUG-660 adds 18 here (17 new names + 3 overrides
  // of mathNamespace()'s round/min/max, less sum/group/runiform/runiformint
  // and regexs, moved to addStataMacroOnlyFns) — 93 was already stale before
  // this (registry.cpp's --dump-functions is the live count, not this literal).
  dprof::noteNsBuild(112);
}

// BUG-660 fix round: the macro/arith-ONLY functions split out of
// addStataFns() above — never installed into a namespace a row plane can
// reach (see that function's own comment for the failure mode this avoids).
// Called by stataMathNamespace() below and macros.cpp's
// postfileStataMathNamespace() -- the two no-row-plane registries -- not by
// strmutate.cpp's baseNamespace() (the row-plane one, unedited by this fix,
// out of manifest).
//
// regexs() lives here, not in addStataFns(): review evidence (fix round 2)
// showed the oracle's row-plane regexs arms (regexs_whole, regexs_group_matched
// in a `generate cond(regexm(...), regexs(...), ...)`) give an empty row 1
// because Stata's real row-plane regexm/regexs pairing has an evaluation-order
// dependency this per-call, non-row-aware ns.fns entry cannot reproduce --
// strmutate.cpp's row iteration (out of manifest) would have to carry that
// state per row. Registering regexs() in the row plane at all made every row
// silently answer with the CURRENT row's match instead, which is a silent
// wrong answer, not the honest r(133) "unknown function" the row plane gave
// before this fix round. macro-layer regexs (regexs_macro_standalone) has no
// such row-plane ordering to get wrong -- it is a plain sequential call in
// the same evaluator as its regexm() -- so it stays available here.
//
// piet-row-evaluator-string-data-arms: the row plane now carries regexs()
// too, through addRowPlaneRegexs() below -- the same fnRegexs, read through
// the RegexsRowScope snapshot that reproduces the oracle's row-1 "" (see the
// comment above fnRegexs). Without that scope a row-plane regexs() would be
// the silent wrong answer described above; with it, evalRows is the one
// caller that opens the scope.
void addStataMacroOnlyFns(PyNamespace& ns) {
  ns.fns["sum"] = fnSum;
  ns.fns["group"] = fnGroup;
  ns.fns["runiform"] = fnRuniformMacro;
  ns.fns["runiformint"] = fnRuniformintMacro;
  ns.fns["regexs"] = fnRegexs;
}

void addRowPlaneRegexs(PyNamespace& ns) { ns.fns["regexs"] = fnRegexs; }

void addRowPlaneSum(PyNamespace& ns) { ns.fns["sum"] = fnSumRow; }  // BUG-1369

// EXPRLAYER-1c: mathNamespace(false) + addStataFns(), built once per process.
// Callers that used to rebuild it per expression now point a per-call
// PyNamespace's `base` here (see PyNamespace's header comment for why the
// shadowing is unchanged).
const PyNamespace& stataMathNamespace() {
  static const PyNamespace kNs = [] {
    PyNamespace n = mathNamespace(/*withInfNan=*/false);
    addStataFns(n);
    addStataMacroOnlyFns(n);  // BUG-660 fix round: sum/group/runiform(int) here only
    return n;
  }();
  return kNs;
}

void addScalarFn(PyNamespace& ns, ScalarLookupFn lookup) {
  ns.fns["scalar"] = [lookup](const std::vector<PyValue>& args) -> PyValue {
    requireArity(args, 1, 1, "scalar");
    std::string key = pythonStr(args[0]);
    auto val = lookup(key);
    if (!val) return PyValue::floating(kNaN);
    auto f = pythonFloat(*val);
    return PyValue::floating(f ? *f : kNaN);
  };
}

}  // namespace piet::eval
