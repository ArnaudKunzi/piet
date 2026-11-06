// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// display_hardening.cpp — C++ port of piet/backend/piet/display_hardening.py
// (M3a; the frozen Python file is the spec — see the header for design
// notes on why this module's helpers are NOT shared with
// piet/eval/macros.cpp's similarly-named anonymous-namespace helpers).
#include "piet/display_hardening.h"

#include "piet/bcal.h"  // AER scope-2 B2-3: %tbNAME rendering

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <map>
#include <optional>
#include <regex>
#include <string>
#include <vector>

#include "piet/cclass.h"
#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"
#include "piet/eval/macros.h"
#include "piet/stata_format.h"
#include "piet/strutil.h"
#include "piet/eval/pyvalue.h"
#include "core/expr_blanks.h"  // FU-875: the blanks Stata drops inside ( ) and [ ]
#include "data/missing.h"  // BUG-341 subscript: the extended-missing kind of a cell

// eval/macros.cpp (BUG-1545): an ancillary `_b[/name]`/`_se[/name]` read from the
// live fit's posted e(b)/e(V); the macro twin's resolveBRef reads it too.
namespace piet::eval {
std::optional<double> ancillaryEbCoef(const EvalSession& session, const std::string& name,
                                      const std::string& kind);
}  // namespace piet::eval

namespace piet {

namespace {

// ---------------------------------------------------------------------------
// strip() — file-local, mirrors Python str.strip() (ASCII whitespace only;
// matches the other M1/M2/M3 modules' own local strip() ports).
std::string strip(const std::string& s) {
  size_t a = 0, b = s.size();
  auto isSp = [](unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
  };
  while (a < b && isSp(static_cast<unsigned char>(s[a]))) ++a;
  while (b > a && isSp(static_cast<unsigned char>(s[b - 1]))) --b;
  return s.substr(a, b - a);
}

// ---------------------------------------------------------------------------
// _E_SCALAR_KEY — Stata e()-name -> cobra JSON key (display_hardening.py).
const std::map<std::string, std::string>& eScalarKey() {
  static const std::map<std::string, std::string> k = {
      {"r2", "r2"},       {"r2_a", "adj_r2"}, {"adj_r2", "adj_r2"}, {"rmse", "rmse"},
      {"mss", "mss"},     {"rss", "rss"},     {"df_m", "df_m"},     {"df_r", "df_r"},
      {"F", "F"},         {"N", "n_obs"},     {"N_clust", "n_clusters"},
      {"rank", "k_vars"}, {"p", "p_F"},       {"ll", "ll"},
      // BUG-156 (wiring half, P4 wave): the engine ALREADY emits the correct
      // within-R2 as "r2_within" (measured 0.414404 against Stata's 0.4144 on
      // the P2 fixture) — it was simply unreachable under its Stata name
      // `e(r2_w)`, so a correct number read as missing. The rest of BUG-156
      // (poisson e(ll)/e(chi2)/e(r2_p), xtreg between/overall/rho) is engine
      // COMPUTE work and stays open; only this unreachable-but-correct entry
      // is closed here.
      // Duplicated table — the twin lives in the other of
      // {eval/macros.cpp, display_hardening.cpp}. Keep them in step or
      // `display e(r2_w)` and `scalar q = e(r2_w)` disagree.
      {"r2_w", "r2_within"},
      // BUG-156 (wiring half, P2-PREP) — TWIN of the same block in
      // eval/macros.cpp's eScalarKey(). See that copy for why the
      // identity-mapped names are listed rather than left to fall through.
      // The engine does not emit these keys yet (parallel workstream), so
      // `display e(r2_p)` still answers `.` today — by design; this closes
      // the NAME half so the compute half is a one-file change.
      {"ll_0", "ll_0"},     {"chi2", "chi2"},     {"r2_p", "r2_p"},
      {"sigma_u", "sigma_u"}, {"sigma_e", "sigma_e"}, {"rho", "rho"},
      {"corr", "corr_u_xb"},
  };
  return k;
}

// _RE_B_REF / _RE_SE_REF
const std::regex& reBRef() {
  // `_b[eq:name]` (AER scope-2 B2): twin of macros.cpp's reB -- keep in step.
  // The dot is B2-4's: `_b[ATE:r1vs0.t]`. Twin of macros.cpp's reB.
  // BUG-629 (display half): the head class takes a digit and the tail a `#`,
  // as macros.cpp's reB/reSe already do -- a factor-level coefficient is named
  // `2.c` (`1.c#c.x1` for an interaction cell), and `display _b[2.c]` fell
  // through to "matrix _b not found" at rc 1 while `local q = _b[2.c]` answered.
  // Unicode names: a byte >= 0x80 is a name byte (core/ident.h), so
  // `display _b[سعر2]` reads the coefficient. Twin of macros.cpp's reB.
  static const std::regex r(
      R"(_b\[((?:[A-Za-z_0-9/]|[^\x00-\x7F])(?:[A-Za-z0-9_:.#]|[^\x00-\x7F])*)\])");
  return r;
}
const std::regex& reSeRef() {
  static const std::regex r(
      R"(_se\[((?:[A-Za-z_0-9/]|[^\x00-\x7F])(?:[A-Za-z0-9_:.#]|[^\x00-\x7F])*)\])");
  return r;
}
// _RE_E_REF — e(name) reference, applied with fullmatch semantics in Python
// (re.match anchored via ^...$ in the pattern itself).
const std::regex& reERef() {
  static const std::regex r(R"(^e\(\s*(.+?)\s*\)$)");
  return r;
}
// identifier-shaped token check (step 7 of _resolve_token)
const std::regex& reIdent() {
  static const std::regex r(std::string("^") + eval::kRxName + "$");  // Unicode names too
  return r;
}
// _RE_C_REF — c(name) system-value reference (BUG-091). Whole-token match on a
// single identifier inside c(...); no nesting, matching Stata's c() namespace.
const std::regex& reCRef() {
  static const std::regex r(R"(^c\(\s*([A-Za-z_]\w*)\s*\)$)");
  return r;
}
// A string display format: `%20s`, `%-20s`, `%~20s` (FU-141's scalar gate).
const std::regex& reStringFmtSpec() {
  static const std::regex r(R"(^%[-~]?\d*s$)");
  return r;
}

// BUG-1345: whether a `%` word of a display line is a format StataBE 19.5 takes;
// display refuses any other with r(120) `invalid %format`. A width is one to four
// digits (a leading 0 counts: `%0010s`, `%0010.2f` and `%0021x` are formats,
// `%00010s` is not) worth 1 to 2045. The families:
//   %[-]w{.,}d{f,g}[c] and %[-]w{.,}de with d below w (`%9.2fc`, `%9,2f`; not
//     `%9.2ec`, `%3.3f`, `%9.f`, `%.2f`, `%9.2F`, `%+9.2f`);
//   %[-~]ws (`%-~10s`, `%~-10s`, `%10S`, `%10ss`, `%0s` are not);
//   %[-]21x, %[-]16H, %[-]16L, %[-]8H, %[-]8L (`%10x`, `%21X`, `%16h` are not);
//   %[-]t{c,C,d,w,m,q,h,y,g}[mask] and %tbNAME[:mask] (`%t`, `%tx`, `%10td`,
//     `%-tbNAME` are not). A mask is not checked: `%tdx` and `%td!` are r(120)
//     in StataBE and printed here.
// The legacy `%d` family is not decided here (the caller's own path).
bool isStataDisplayFormat(const std::string& fmt) {
  static const std::regex reNum(R"(^%-?(\d{1,4})[.,](\d+)(?:[fg]c?|e)$)");
  static const std::regex reWidthed(R"(^%(?:-|~(?=\d+s$))?(\d{1,4})([sxHL])$)");
  static const std::regex reTime(R"(^%-?t[cCdwmqhyg].*$)");
  static const std::regex reBcal(R"(^%tb([A-Za-z_][A-Za-z0-9_]*)(?::.*)?$)");
  std::smatch m;
  if (std::regex_match(fmt, m, reNum)) {
    const int w = std::stoi(m[1].str());
    const std::string d = m[2].str();
    const size_t lead = d.find_first_not_of('0');
    return w >= 1 && w <= 2045 &&
           (lead == std::string::npos || (d.size() - lead <= 4 && std::stoi(d.substr(lead)) < w));
  }
  if (std::regex_match(fmt, m, reWidthed)) {
    const int w = std::stoi(m[1].str());
    switch (m[2].str()[0]) {
      case 's': return w >= 1 && w <= 2045;
      case 'x': return w == 21;
      default: return w == 16 || w == 8;  // H, L
    }
  }
  if (std::regex_match(fmt, m, reBcal)) return bcal::isValidCalName(m[1].str());
  return std::regex_match(fmt, reTime);
}

// _RE_REQUEST_CALL — BUG-087/D9 (BETA Phase F, D9 locked decision):
// `_request(macroName)` call-syntax token. Deliberately NOT part of
// piet::eval::tryExtendedMacroFn()'s colon-form dispatch
// (`local NAME : func ...`, macros.cpp) — real Stata's `_request()` is
// call-syntax invoked from inside `display`, exactly the shape BUG-088
// traced (this file's resolveToken() falling through -> eval::evalArith()
// -> pyeval's NameError). Intercepted in flushUnquoted() below (NOT inside
// resolveToken()) for two reasons: (a) resolveToken() has no channel to
// report a genuine error back to its caller — BUG-088's fix added that
// channel to evalArith(), one layer up, and duplicating it into
// resolveToken() too would be a second, parallel error path; (b) the
// modal's "prompt" text is the PRECEDING display text on this same
// statement (BUG-087's own repro: `display as text "Include additional
// controls? ..." _request(controls_input)` — the string literal before the
// call IS the prompt a real Stata user sees) — only flushUnquoted() has
// that (via `parts`); resolveToken() only ever sees one bare token in
// isolation.
const std::regex& reRequestCall() {
  static const std::regex r(std::string(R"(^_request\(\s*()") + eval::kRxName + R"()\s*\)$)");
  return r;
}

// PIET_REQUEST_<NAME> env var name for the headless fallback (macroName
// uppercased, any non-alnum char -> '_') — exact spec BUG-087's brief gives.
std::string requestEnvVarName(const std::string& macroName) {
  std::string out = "PIET_REQUEST_";
  for (char c : macroName) {
    unsigned char uc = static_cast<unsigned char>(c);
    out += std::isalnum(uc) ? static_cast<char>(std::toupper(uc)) : '_';
  }
  return out;
}

// _request(macroName) — BUG-087/D9: blocks on a real modal round-trip
// (windowed, on the do-executor worker thread) or reads
// PIET_REQUEST_<NAME>/PIET_REQUEST_DEFAULT (headless: --do/--selftest/
// --run-block/--evalcase never construct a Shell at all, so
// session.hasRequestPostEvent() is false there), sets the GLOBAL macro
// $macroName, and returns "" — `_request()` itself contributes no further
// display text (matches real Stata: the visible prompt is whatever was
// already displayed BEFORE the call blocks, not a "return value" from the
// call). On failure (cancelled run, wrong thread, or headless with no
// matching env var) sets *error and returns nullopt; the caller
// (flushUnquoted) turns that into an honest engine_error — same BUG-088
// shape, never a hang, never a silent literal-text fallback.
std::optional<std::string> resolveRequestCall(const std::string& macroName,
                                               const std::string& promptSoFar,
                                               SessionState& session, std::string* error) {
  if (session.hasRequestPostEvent()) {
    // A single-line `display _request(x)` submitted via #cmd-input goes
    // through the SYNCHRONOUS "command" bindRoute, which runs ON the UI
    // thread (shell.h's threading note) — blocking there would deadlock the
    // whole app (postEvent()'s queued JS injection, and the user's eventual
    // click, both need that same thread's run loop pumping). Refuse
    // honestly instead; see session_state.h's isCurrentThreadADoWorker()
    // doc comment for the full rationale.
    if (!piet::isCurrentThreadADoWorker()) {
      if (error) {
        *error =
            "_request(" + macroName +
            "): interactive input is only supported while running a full .do file (Run .do / "
            "the do-file editor) — it cannot be used from the single command line.";
      }
      return std::nullopt;
    }
    auto answer = session.requestUserInput(macroName, promptSoFar);
    if (!answer.has_value()) {
      if (error) {
        *error = "_request(" + macroName +
                 ") was cancelled — the run stopped before an answer arrived. Global macro $" +
                 macroName + " was not set.";
      }
      return std::nullopt;
    }
    session.eval.globals.set(macroName, *answer);
    return std::string();
  }

  // Headless: NEVER block waiting for a frontend that doesn't exist.
  std::string envName = requestEnvVarName(macroName);
  const char* v = std::getenv(envName.c_str());
  if (!v) v = std::getenv("PIET_REQUEST_DEFAULT");
  if (v != nullptr) {
    session.eval.globals.set(macroName, std::string(v));
    return std::string();
  }
  if (error) {
    *error = "_request(" + macroName +
             "): interactive input is unsupported in a headless run. Set the environment "
             "variable " +
             envName +
             " (or PIET_REQUEST_DEFAULT) before running, or set the macro yourself first: "
             "global " +
             macroName + " <value>.";
  }
  return std::nullopt;
}

// _SPECIAL_SCALARS
bool isSpecialScalar(const std::string& tok) { return tok == "_N" || tok == "_n"; }

// jsonToFloat — mirrors Python's `float(val)` where val came from a JSON
// scalar (number/bool/string); null/object/array -> TypeError -> nullopt.
std::optional<double> jsonToFloat(const nlohmann::ordered_json& v) {
  if (v.is_number()) return v.get<double>();
  if (v.is_boolean()) return v.get<bool>() ? 1.0 : 0.0;
  if (v.is_string()) return eval::pythonFloat(v.get<std::string>());
  return std::nullopt;
}

// (BUG-554: the file-local `%.9g` fmtNumeric that stood here is gone -- every
// resolver below substitutes at full precision through fmtValueFullPrecision.)

// BUG-406 (rediagnosed 2026-09-06): a resolved VARIABLE VALUE is a token this
// function hands back as TEXT, which the display layer then re-renders through
// the user's format. Rendering it here at %.9g therefore threw away every digit
// past the ninth BEFORE `%22.17f` was ever applied — `display %22.17f v[1]`
// printed -77.55172410000000127 for a value the hbin stores exactly as
// -77.55172413793085, and the entry blamed the CSV transport for it. This is
// the same defect BUG-382 fixed for e() scalars in macros.cpp, in the other
// layer: substitute at FULL precision and let the format do the rounding.
// Bare `display v[1]` is unchanged — a substituted numeric literal is re-rendered
// through the %9.0g default (fmtStataDefaultG), which is what the freeze corpus pins.
std::string fmtValueFullPrecision(double v) {
  if (std::isnan(v)) return ".";
  if (std::isinf(v)) return v > 0 ? "+Inf" : "-Inf";
  return eval::pythonFloatRepr(v);
}

// W:P5 — Stata's DEFAULT display format for a numeric expression, `%9.0g`.
//
// `display <numeric>` in Stata renders into nine characters: the most
// significant digits that fit, in fixed notation when possible and
// e-notation when not, with the leading zero of a |x|<1 magnitude dropped.
// Measured against the answerbook's own Stata 19.5 log:
//   2.157789219986937   -> "2.1577892"   (9 chars)
//   0.5926470951388326  -> ".5926471"    (9 chars formatted, leading 0 cut)
//   6.277415369414958e-26 -> "6.277e-26" (9 chars — only 4 significant
//                                         digits survive once "e-26" is paid)
// PIET printed full `repr` precision instead, so a pin whose magnitude forces
// Stata into short e-notation compared RED at rtol 1e-6 even though the
// underlying value was exact to fifteen digits. The number was never wrong;
// the rendering was.
//
// Integers keep their plain form (Stata prints `5`, not `5.0000000`), and a
// value needing more than nine characters even at one significant digit
// (only ±inf-adjacent magnitudes) falls back to %.9g rather than truncating
// to something unreadable.
std::string fmtStataDefaultGImpl(double v) {
  if (std::isnan(v)) return ".";
  if (std::isinf(v)) return v > 0 ? "+Inf" : "-Inf";
  char buf[64];
  // Widths are counted the way Stata counts them: on the string INCLUDING a
  // leading "0." — which is then stripped — and excluding a minus sign.
  const int kWidth = 9;
  for (int prec = 8; prec >= 1; --prec) {
    std::snprintf(buf, sizeof(buf), "%.*g", prec, v);
    std::string s(buf);
    const size_t signLen = (!s.empty() && s[0] == '-') ? 1 : 0;
    if (s.size() - signLen <= static_cast<size_t>(kWidth)) {
      // Stata writes ".5" where C writes "0.5".
      if (s.compare(signLen, 2, "0.") == 0) s.erase(signLen, 1);
      return s;
    }
  }
  std::snprintf(buf, sizeof(buf), "%.9g", v);
  return std::string(buf);
}

// BUG-1016: `display <numeric>` with NO format, measured against StataBE 19.5:
// `display xd[1]` for .4583333333333333 prints `.45833333`
// (tests/eval/golden/aer_w2/w2i_varsub.log:85 j_01) and `display _b[ATE:r1vs0.t]`
// prints `.02559116` and `-.22631858` (w2g_ebmat.log:1728, :1807) -- eight
// significant digits in nine characters once the leading zero is DROPPED, where
// fmtStataDefaultGImpl counts the nine on `0.4583333` and printed `.4583333`.
// For |x| >= 1 the two agree (2.1577892, 6.277e-26 above). A separate function
// on purpose: fmtStataDefaultG (fmtStataDefaultGImpl) is also the `%9.0g` cell
// renderer of the resampling, rdrobust and table handlers, whose own pins
// (freeze rdrobust.json) print `.3842431`; the explicit `%9.0g` format is
// applyStataFmt's (fmt_g_oracle.log:142-160). Only display's unformatted item
// takes this one.
// BUG-1126: that rule is the shared `%10.0g` (stata_format.h general: eight
// significant digits in nine characters), whose small and large sides printf's
// %g did not follow: `display 0.00001` printed 1e-05, 1e-6 1e-06, 2e-5 2e-05,
// 1e20 1e+20 and 1e-300 1e-300, where StataBE prints .00001, 1.000e-06, .00002,
// 1.000e+20 and 1.00e-300 (file write's unformatted `(exp)`, documented as
// %10.0g, prints the same). The values above agree with it.
std::string fmtDisplayDefaultG(double v) {
  if (std::isnan(v)) return ".";
  if (std::isinf(v)) return v > 0 ? "+Inf" : "-Inf";
  return piet::fmt::general(v, 10, 0);
}

// BUG-444: Stata's `%21x` text, `[+-]1.<13 hex digits>X[+-]<3 hex digits>`.
// Zero is `+0.0000000000000X-3ff`. Stata's missing values are doubles at
// 2^1023 whose fraction carries the kind in its top 12 bits: `.` is
// `+1.0000000000000X+3ff`, `.a` `+1.0010000000000X+3ff`, `.b` `...0020...`
// (w2x_fn_string_math fx_fix_v, fx_fix_kt, fx_ceil_edges). The expression
// layer holds exactly those doubles (exprfns.h, stataMissingWithKind), and any
// other value in its missing band (|v| >= 2^1023: an overflow, the negative
// side) is plain `.`, so a missing value is spelled from its kind. c(maxdouble)
// is a real number and prints its own bits, +1.fffffffffffffX+3fe. A NaN or an
// inf reaching here is the data plane's system missing.
// The text is the IEEE fields read off directly (sign, leading bit, 52-bit
// fraction, unbiased exponent), which is how zero gets its `X-3ff`; a
// subnormal is spelled the same way, `+0.<fraction>X-3ff`. No oracle pins
// the subnormal spelling.
std::string renderHex21(double v) {
  char buf[48];
  if (std::isnan(v) || std::isinf(v)) v = eval::kStataMissingSentinel;
  if (const auto kind = eval::stataExtendedMissingKind(v)) {
    std::snprintf(buf, sizeof(buf), "+1.%013llxX+3ff",
                  static_cast<unsigned long long>(*kind) << 40);
    return buf;
  }
  // A negative zero prints as zero: Stata's `ceil(-0.5)` is
  // `+0.0000000000000X-3ff` (fx_ceil_edges), where C's ceil() gives -0.0.
  if (v == 0.0) return "+0.0000000000000X-3ff";
  const char sign = v < 0 ? '-' : '+';
  const double a = std::fabs(v);
  int e = 0;
  const double m = std::frexp(a, &e);  // a = m * 2^e, m in [0.5, 1)
  if (e - 1 < -1022) {
    const auto frac = static_cast<unsigned long long>(std::ldexp(a, 1022 + 52));
    std::snprintf(buf, sizeof(buf), "%c0.%013llxX-3ff", sign, frac);
    return buf;
  }
  const auto frac = static_cast<unsigned long long>(std::ldexp(m * 2.0 - 1.0, 52));
  const int x = e - 1;
  std::snprintf(buf, sizeof(buf), "%c1.%013llxX%c%03x", sign, frac, x < 0 ? '-' : '+',
                static_cast<unsigned>(x < 0 ? -x : x));
  return buf;
}

// _apply_stata_fmt (router/macros.py) — Python signature is
// (fmt: str, value_str: str) -> str. The C++ piet::eval::applyStataFmt has
// argument order (value, fmt) AND different fallback behavior (falls back
// to gFormat() and supports -/0 flags + a prefix-match regex) — it is a
// DIFFERENT function serving exprfns.string(), not this one. This adapter
// re-implements router/macros.py's _apply_stata_fmt faithfully, PLUS the
// BUG-185 width fix on top:
//   - regex is a FULL match: ^%(-)?(\d+)?\.(\d+)([fgeEFG])$ (no 0 flag, no
//     trailing garbage tolerated)
//   - BUG-185: width (group 2) used to be parsed but UNUSED (a dead
//     variable inherited from the Python port). Stata right-justifies to
//     the field width by default and left-justifies when the spec starts
//     with '-'; a value already wider than the field is never truncated
//     (printf semantics) — that's exactly what the padding step below does.
//   - ftype in {f} -> fixed; {g,e} -> %.<prec>g / %.<prec>e (case folded
//     via .lower(), so %9.2E and %9.2e behave the same, matching Python's
//     `ftype = m.group(3).lower()`)
//   - on regex-non-match OR float(value_str) failure -> return value_str
//     UNCHANGED (no gFormat fallback, unlike the exprfns.h sibling) — this
//     preserves every pre-existing non-dash case byte-identical (%9s,
//     %td-style time tokens, and any other letter this function never
//     understood keep silently passing the raw value through untouched).
//   - BUG-185: a '-'-prefixed spec is BRAND NEW surface (the tokenizer used
//     to never route it here at all — see the isFmtTok gate below — so it
//     fell through and got echoed as literal source text instead of being
//     applied). A malformed one now reports an honest error via `err`
//     rather than resurrecting that echo-as-literal failure mode. This
//     branch is unreachable for any non-dash spec Stata accepts that this
//     code already handled, since only fmt[1]=='-' can hit it.
// ---------------------------------------------------------------------------
// BUG-192 / BUG-195: Stata's `%w.Pg` is NOT printf's `%.Pg`.
//
// `display %9.0g 42` printed `4e+01`, because the spec was handed to printf and
// printf reads `%.0g` as "zero significant digits". Stata's `g` is a GENERAL
// format: `P` bounds the significant digits, `w` bounds the field, and a value
// that fits shows as itself. `%9.0g 42` is `       42`.
//
// The renderer that implements that rule USED TO LIVE HERE. It now lives in
// piet/stata_format.h and is shared with eval/exprfns.cpp's applyStataFmt, the
// string()/tostring layer, which carried an independent printf-based twin of
// the same format with the same defect (BUG-195 — and that twin renders into
// STORED data, not pixels). Two hand-written twins of Stata's `g` rule is how
// they drift, so there is one renderer now. The full rule, its seven clauses,
// the `%w.0e` rider and the oracle that forced each of them are documented on
// that header; this file's behaviour is unchanged, byte for byte.
//
// The oracle: internal capture fmt_g_oracle.{do,log},
// 513 StataBE 19.5 cases over all three surfaces. BUG-192's original capture
// was lost, which is why the rule spent a release with no on-disk authority
// behind it.
//
// What stays here rather than moving to the shared header is everything that
// is specific to formatting an ALREADY-STRINGIFIED value for `display`: this
// layer's own full-match spec grammar (so `%td`/`%9s` fall through untouched),
// the parse-failure and float()-failure fallbacks, the missing-token rider, and
// the right-justification into the field. string()/tostring share the rendering
// and none of those — measured, see padToField()'s comment.

std::string applyStataFmtLocal(const std::string& fmt, const std::string& valueStr,
                                std::string* err = nullptr) {
  // BUG-1345: a string format on a number prints the number as display prints
  // it with no format, in no field (StataBE 19.5: `%10s 3.14159265` 3.1415927,
  // `%-10s 1234567.891` 1234567.9, `%~9s -2.5` -2.5, `%10s .` .). `%-10s 3` was
  // rc 1 `invalid display format %-10s`; `%10s` printed the full-precision text.
  if (std::regex_match(fmt, reStringFmtSpec())) {
    const std::string t = strutil::trim(valueStr);
    if (piet::fmt::isMissingToken(t)) return t;
    auto v = eval::pythonFloat(valueStr);
    return v ? fmtDisplayDefaultG(*v) : valueStr;
  }
  // BUG-293(a): the bare date/time families. `display %td (20000)` printed the
  // raw 20000 because this layer's full-match grammar refuses every %t spec, so
  // the token never reached a renderer at all. Stata (19.5 IC, measured) prints
  // `04oct2014` and `%tc (1234567890000)` -> `13feb1999 23:31:30`. Same arm, same
  // renderers and the same "bare tc/td only" boundary as eval/exprfns.cpp's
  // string()/tostring half — the detail-coded and other-calendar forms stay a
  // named gap (BUG-296) rather than being mapped to a plausible wrong answer.
  // BUG-296 extends this arm past tc/td: the other %t calendars and a
  // detail-coded %td. One decode, shared with eval/exprfns.cpp's string()/
  // tostring half — the same renderers, the same named gap at %ty.
  {
    // AER scope-2 B2-3: `%tbNAME[:detail]`. A business-day INDEX inside the
    // calendar prints its calendar date; outside the calendar it prints the
    // raw number, and a calendar that cannot be loaded falls back to the
    // plain number -- Stata's own %g fallback ([D] Datetime business
    // calendars, item 5). Measured: `%tbNAME` of -1/99 outside the range
    // prints `-1`/`99`, of a missing value prints `.`.
    static const std::regex reTb(R"(^%-?\d{0,2}tb([A-Za-z_][A-Za-z0-9_]*)(?::(.*))?$)");
    std::smatch bm;
    if (std::regex_match(fmt, bm, reTb)) {
      const std::string detail = bm.size() > 2 ? bm[2].str() : std::string();
      const int fw = piet::fmt::tFamilyWidth('d', detail);
      // EVERY exit from this branch is RIGHT-ALIGNED in the family width, the
      // fallbacks included — measured this review (StataBE 19.5,
      // /private/tmp/fix-bcal/st/cap4.log): with `dcal` loaded,
      //   %tbdcal 3   `04jan2020`  ·  %tbdcal -1  `       -1`
      //   %tbdcal 99  `       99`  ·  %tbdcal .   `        .`
      // and with NO such calendar at all, %tbnosuchcl 3 is `        3` and
      // %tbnosuchcl -1 is `       -1`. The values were already right (the
      // review's non-blocking 4 says so); only the width was missing, so the
      // padding is hoisted out of the success path and applied to all of them.
      auto pad = [fw](const std::string& t) {
        if (fw > static_cast<int>(t.size()))
          return std::string(static_cast<size_t>(fw) - t.size(), ' ') + t;
        return t;
      };
      // The calendar-NAME rule on the third surface too (review 2026-09-09).
      // Behaviour-NEUTRAL on the VALUE and deliberately so: bcal::lookup()
      // already returns null for a name the rule refuses, so an over-long name
      // fell back to the raw number either way. It is here so the rule has ONE
      // predicate on every plane that reads a %tb name — the `format` plane is
      // where the missing check was an actual leniency divergence.
      if (!bcal::isValidCalName(bm[1].str())) return pad(valueStr);
      auto bv = eval::pythonFloat(valueStr);
      if (!bv) return pad(valueStr);
      auto cal = bcal::lookup(bm[1].str());
      if (!cal) return pad(valueStr);
      const double day = bcal::dofb(*cal, *bv);
      if (std::isnan(day)) return pad(valueStr);
      std::optional<std::string> r =
          detail.empty() ? piet::fmt::renderTd(day) : piet::fmt::renderTdDetail(day, detail);
      if (!r) return pad(valueStr);
      return pad(*r);
    }
  }
  {
    static const std::regex reT(R"(^%-?t([cCdqhmwy])(.*)$)");
    std::smatch tm;
    if (std::regex_match(fmt, tm, reT)) {
      auto tv = eval::pythonFloat(valueStr);
      if (!tv) return valueStr;
      const char fam = tm[1].str()[0];
      const std::string detail = tm[2].str();
      std::optional<std::string> r;
      if (detail.empty()) {
        switch (fam) {
          case 'c': r = piet::fmt::renderTc(*tv); break;
          case 'd': r = piet::fmt::renderTd(*tv); break;
          case 'q': r = piet::fmt::renderTq(*tv); break;
          case 'h': r = piet::fmt::renderTh(*tv); break;
          case 'm': r = piet::fmt::renderTm(*tv); break;
          case 'w': r = piet::fmt::renderTw(*tv); break;
          case 'y': r = piet::fmt::renderTy(*tv); break;
          default: break;  // %tC — named gap
        }
      } else if (fam == 'd') {
        r = piet::fmt::renderTdDetail(*tv, detail);
      }
      if (r) {
        // BUG-296: right-justify into the family's own field (see
        // piet::fmt::tFamilyWidth — the width is a property of the SPEC, not of
        // this value). string()/tostring do not pad; only display does.
        const int fw = piet::fmt::tFamilyWidth(fam, detail);
        if (fw > static_cast<int>(r->size())) {
          return std::string(static_cast<size_t>(fw) - r->size(), ' ') + *r;
        }
        return *r;
      }
      return valueStr;
    }
  }
  // BUG-444: `%21x`, Stata's hexadecimal view of the stored double. It used to
  // fail the grammar below and print the decimal text at rc 0, so a missing
  // value printed `.` where Stata prints `+1.0000000000000X+3ff` (`.a` is
  // `+1.0010000000000X+3ff`). Oracle: aer_w2 w2x_fn_string_math (every numeric
  // field is a %21x record; fx_fix_v/fx_fix_kt carry `.` and `.a`).
  {
    static const std::regex reX(R"(^%(-)?21x$)");
    std::smatch xm;
    if (std::regex_match(fmt, xm, reX)) {
      const std::string t = strutil::trim(valueStr);
      std::optional<double> v;
      if (piet::fmt::isMissingToken(t)) {
        v = eval::stataMissingWithKind(t.size() == 1 ? 0 : t[1] - 'a' + 1);
      } else {
        v = eval::pythonFloat(valueStr);
      }
      if (!v) return valueStr;
      return renderHex21(*v);
    }
  }
  // BUG-293(a): the `0` flag. The spec grammar had no group for it, so
  // `%09.2f` failed the full match and fell through unformatted — PIET printed
  // `    42.50` where Stata prints `000042.50`. StataFmtSpec::zeroFill already
  // exists and is already honored by the shared renderer (BUG-248 wired the
  // string()/tostring surface); only display's own grammar never spoke it.
  // BUG-296: `,` decimal separator and the `c` grouping suffix, the same two
  // axes the string()/tostring grammar gained.
  static const std::regex re(R"(^%(-)?(0)?(\d+)?([.,])(\d+)([fgeEFG])(c)?$)");
  std::smatch m;
  if (!std::regex_match(fmt, m, re)) {
    if (err && fmt.size() >= 2 && fmt[1] == '-') *err = "invalid display format " + fmt;
    return valueStr;
  }
  piet::fmt::StataFmtSpec sp;
  sp.leftJustify = m[1].matched;
  sp.zeroFill = m[2].matched;
  sp.hasWidth = m[3].matched;
  if (sp.hasWidth) {
    try {
      sp.width = std::stoi(m[3].str());
    } catch (...) {
      sp.hasWidth = false;
    }
  }
  sp.commaDecimal = m[4].str() == ",";
  try {
    sp.prec = std::stoi(m[5].str());
  } catch (...) {
    return valueStr;
  }
  sp.conv = m[6].str()[0];
  sp.groupThousands = m[7].matched;
  // .lower(), matching the Python port's `ftype = m.group(3).lower()` — %9.2E
  // and %9.2e behave the same.
  if (sp.conv >= 'A' && sp.conv <= 'Z') sp.conv = static_cast<char>(sp.conv - 'A' + 'a');

  // BUG-192 rider (same oracle capture): a Stata missing value IS a value in
  // a numeric field and Stata right-justifies it like any other — `%9.0g .`
  // is eight blanks then a dot. It used to fall out of pythonFloat() as
  // unparseable and return the bare token with no padding at all, so a table
  // column containing one lost its alignment. `.a`-`.z` (the extended missing
  // codes) take the same field. DISPLAY-ONLY: the oracle's section B has
  // `string(., "%9.0g")` as a bare `.` with no field at all.
  {
    const std::string t = strutil::trim(valueStr);
    if (piet::fmt::isMissingToken(t)) return piet::fmt::padToField(t, sp);
  }

  auto v = eval::pythonFloat(valueStr);
  if (!v) return valueStr;  // float(value_str) raised -> except -> fall through

  // The one renderer (piet/stata_format.h). nullopt is the printf-overflow and
  // unreachable-conversion case, where both this function and its Python
  // ancestor return the value untouched.
  auto rendered = piet::fmt::renderValue(*v, sp);
  if (!rendered) return valueStr;
  // BUG-296: when an `f` spec OVERFLOWED into scientific, the field it lands in
  // is at least 8 — measured, `%6.2f 1234567.89` displays ` 1.2e+06` (8) where
  // the value itself is 7, while `%9.2f`/`%12.2f` use their own wider fields.
  // Scoped to the overflow arm: the ordinary fixed and general renderings keep
  // their own field exactly as before.
  // BUG-1353: an `e` spec narrower than 8 lands in the same field: `%5.0e 1.5`
  // displays ` 1.5e+00` and `%5.0e 1.5e+300` ` 2.e+300` (StataBE 19.5).
  // A left-justified `e` keeps its own field: `%-7.2e 1.5` is `1.5e+00`, no pad.
  // A right-justified `e` also keeps a blank where a minus sign would go, which
  // only shows when rounding carried the exponent to three digits: `%9.0e
  // 9.9999e99` displays ` 1.00e+100`, `%5.0e` ` 1.0e+100` (string() has no field).
  const bool eField = sp.conv == 'e' && !sp.leftJustify;
  const int signSlot = eField && !rendered->empty() && (*rendered)[0] != '-'
                           ? static_cast<int>(rendered->size()) + 1
                           : 0;
  if (((sp.conv == 'f' && rendered->find('e') != std::string::npos) || eField) &&
      sp.width < 8) {
    piet::fmt::StataFmtSpec wide = sp;
    wide.hasWidth = true;
    wide.width = std::max(8, signSlot);
    return piet::fmt::padToField(*rendered, wide);
  }
  if (signSlot > sp.width) {
    piet::fmt::StataFmtSpec wide = sp;
    wide.width = signSlot;
    return piet::fmt::padToField(*rendered, wide);
  }
  // BUG-185: pad to field width; printf semantics — never truncate a value
  // already wider than the field.
  return piet::fmt::padToField(*rendered, sp);
}

// ---------------------------------------------------------------------------
// BUG-456 (2026-09-08): the e-MACRO arm of the display twin. TWIN of
// macros.cpp's resolveERef -- same rule, same literal shape, and the same
// "always a string, never a number" ruling measured on StataNow 19.5 BE
// (internal capture refute3_capture.log, 40/40 macro shapes). Returns the
// quoted literal when the name is a non-empty e-string, else nullopt so the
// caller falls through to resolveEScalar (a scalar, or numeric missing for an
// absent name). Keep in step with macros.cpp.
std::optional<std::string> resolveEStringLiteral(const std::string& name, SessionState& session) {
  const std::string* s = session.eval.eStrings.find(name);
  if (s == nullptr || s->empty()) return std::nullopt;
  std::string lit = "\"";
  for (char ch : *s) {
    if (ch == '"') lit += "\\\"";
    else lit += ch;
  }
  lit += "\"";
  return lit;
}

// _resolve_e_scalar
//
// BUG-554 (W2D-T03): the returned TEXT is %.9g (fmtNumeric) and stays so --
// bare `display e(r2)` and every expression built on it are byte-identical to
// before. `stored`, when non-null, receives the double that text was made
// from, so an explicit display format renders the stored value instead of its
// nine-digit rounding (`display %18.0g e(r2)`, `%21x e(ll)`). Left untouched
// when the answer is missing.
std::string resolveEScalar(const std::string& name, SessionState& session,
                           double* stored = nullptr) {
  // BUG-421: this twin reads SessionState::lastModel()->result; a posted
  // e-scalar (pca's e(rho), `estadd scalar`) lives in session.eval.eScalars and
  // reached neither. Checked LAST everywhere, so a fitted key always wins.
  const auto posted = [&]() -> std::string {
    // BUG-554: full precision; the display format does the rounding.
    if (const double* v = session.eval.eScalars.find(name)) {
      if (stored) *stored = *v;
      return fmtValueFullPrecision(*v);
    }
    return ".";
  };
  const auto& lm = session.lastModel();
  if (!lm.has_value() || !lm->result.has_value()) return posted();
  const auto& result = *lm->result;
  // Python: `if not result:` — falsy for {} or None. Treat empty object the
  // same as "no result".
  if (result.is_null() || (result.is_object() && result.empty())) return posted();

  std::string key = name;
  auto it = eScalarKey().find(name);
  if (it != eScalarKey().end()) key = it->second;

  if (!result.is_object() || !result.contains(key) || result.at(key).is_null()) return posted();
  auto val = jsonToFloat(result.at(key));
  // Engine n_clusters 0 = unclustered fit -> Stata e(N_clust) missing.
  if (val && name == "N_clust" && *val == 0.0) val.reset();
  if (!val) return posted();
  if (stored) *stored = *val;
  return fmtValueFullPrecision(*val);  // BUG-554
}

// The `<eq>:name` spelling of an equation-less `_b[name]`, appended to
// `tryNames` when the first coeftable row carries an equation.
void addFirstEquationName(const ojson& coeftable, const std::string& varname,
                          std::vector<std::string>& tryNames) {
  if (varname.find(':') != std::string::npos || coeftable.empty()) return;
  const auto& first = coeftable.front();
  if (!first.is_object() || !first.contains("name") || !first.at("name").is_string()) return;
  const std::string head = first.at("name").get<std::string>();
  const size_t colon = head.find(':');
  if (colon == std::string::npos || colon == 0) return;
  tryNames.push_back(head.substr(0, colon + 1) + varname);
}

// _resolve_b_ref
//
// BUG-162 (W:P5): `err` distinguishes the two ways this can fail to produce a
// number. A coefficient that EXISTS but whose value is missing is Stata's `.`
// — a legitimate result. A coefficient NAME that does not exist in the model
// is not: Stata raises r(111), and returning `.` for it turned a typo into a
// missing value that then propagated silently through every expression built
// on it. When `err` is non-null the caller surfaces an honest error; when it
// is null (paths that have no error channel) the historical `.` is kept, so
// no caller changes behaviour without opting in.
//
// W2D-T03 (FU-88 and its sibling): both refusals carry Stata's code in the
// `r(N): ` prefix handleDisplay turns into the block's `reason`, so
// `capture noisily display _b[x]` leaves _rc 111 instead of 1.
//   * no fitted model (a fresh session, after `ereturn clear`, or after a
//     posting command such as pca): r(111) "_b not allowed when e(b) is not
//     present" (w2g_ebmat.log:2099-2105 e15c_disp_bx1, :2181-2187
//     e15c_disp_bx1_times2, :2489-2495 e15d_disp_bx1_2; w2p_estim_ev.log:409-414).
//     r(301) "last estimates not found" belongs to `matrix b = e(b)` and
//     `matlist e(b)` (w2g_ebmat.log:2223-2238), not to this read.
//   * a model without that coefficient: r(111) "[x1] not found"
//     (w2g_ebmat.log:1689-1694 e15a_disp_bx1; also e15a_oaxaca_disp_bx1,
//     e15c_disp_bx1, e15d_disp_bx1).
// `stored` is resolveEScalar's BUG-554 channel: the coefficient's double, set
// only when a number is returned.
std::string resolveBRef(const std::string& varname, const std::string& kind,
                         SessionState& session, std::string* err = nullptr,
                         double* stored = nullptr) {
  // eb-order T-P3 (SPEC part 1): the same source as the macro twin and as
  // e(b) itself, EvalSession::coefSource() -- the live e()'s estimation block --
  // not lastModel()->result, a third store that estpost/erepost/`estimates use`
  // leave on an earlier fit.
  const nlohmann::ordered_json* source = session.eval.coefSource();
  if (source == nullptr) {
    // Stata (StataNow 19.5 BE, 2026-09-29, `display _b[x]` / `display _se[x]` after
    // `clear all`): r(111) "_b not allowed when e(b) is not present" / "_se not
    // allowed when e(V) is not present". The same text and rc for a live e()
    // that has no e(b) (w2g_ebmat.log:2105 e15c_disp_bx1 after pca, :2495
    // after rddensity). The `r(NNN): ` prefix is what display's splitter
    // (eval::splitErrCode, in handleDisplay) turns into the block's `reason`;
    // without it this was "last estimates not found" at rc 1.
    if (err)
      *err = kind == "b" ? "r(111): _b not allowed when e(b) is not present"
                         : "r(111): _se not allowed when e(V) is not present";
    return ".";
  }

  // BUG-158: intercept alias — see the twin comment on macros.cpp's
  // resolveBRef. These two copies must stay in step.
  std::vector<std::string> tryNames{varname};
  if (!varname.empty() && varname.front() == '/') {
    tryNames.push_back(varname.substr(1) + ":_cons");
    tryNames.push_back("/:" + varname.substr(1));
  }
  if (varname == "_cons") tryNames.push_back("(intercept)");
  else if (varname == "(intercept)") tryNames.push_back("_cons");
  // `_b[yb:x1]` after a depvar-equation fit is the bare row (BUG-1328); the
  // macro twin does the same through the same helper.
  if (const size_t colon = varname.find(':'); colon != std::string::npos && colon > 0) {
    const std::string eq = eval::depvarEquationOf(session.eval);
    if (!eq.empty() && varname.compare(0, colon, eq) == 0 && colon == eq.size()) {
      const std::string bare = varname.substr(colon + 1);
      tryNames.push_back(bare);
      if (bare == "_cons") tryNames.push_back("(intercept)");
    }
  }
  if (const std::string o = eval::omittedColumnSpelling(varname); !o.empty()) tryNames.push_back(o);
  {
    const auto& result = *source;
    if (result.is_object() && result.contains("coeftable") && result.at("coeftable").is_array()) {
      // A name with no equation is read from e(b)'s FIRST equation when no
      // row carries it bare: after `margins, dydx(x) at(z=(0 5)) post`, e(b)
      // is `x:1bn._at x:2._at` and `_b[1._at]` is `_b[x:1._at]` (StataBE 19.5,
      // w2v_margins_corpus F1_dydx_at_post_bname1/2). Tried last, so a bare
      // row always wins. The macro twin (macros.cpp resolveBRef) does the same.
      addFirstEquationName(result.at("coeftable"), varname, tryNames);
      for (const auto& want : tryNames) {
        for (const auto& row : result.at("coeftable")) {
          if (row.is_object() && row.contains("name") && row.at("name").is_string() &&
              row.at("name").get<std::string>() == want) {
            const char* field = (kind == "b") ? "b" : "se";
            // Found the coefficient; a missing VALUE here is a real `.`.
            if (!row.contains(field) || row.at(field).is_null()) return ".";
            auto val = jsonToFloat(row.at(field));
            if (!val) return ".";
            // BUG-554: this is TEXT the display layer re-renders through the
            // user's format, so `%.9g` here cut `display %18.0g _b[a]` to nine
            // digits (w2_cmdtriage.log after_restore). Full precision, as
            // BUG-406/BUG-382 did; a bare `display _b[a]` still renders %9.0g.
            if (stored) *stored = *val;
            return fmtValueFullPrecision(*val);
          }
        }
      }
    }
  }
  // A column only the fit's recorded stripe names: a factor's base level
  // (`_b[0b.c]` = `_b[0.c]` = 0, w2f_residuals.log e_b_0bc/e_b_0c) or an `o.`
  // spelling -- the macro twin reads the same eval::ebStripeCoef.
  if (const std::optional<double> v = eval::ebStripeCoef(*source, varname, kind)) {
    if (stored) *stored = *v;
    return fmtValueFullPrecision(*v);
  }
  // An ancillary parameter only the posted e(b)/e(V) carry (BUG-1545), as the
  // macro twin reads it.
  if (const std::optional<double> v = eval::ancillaryEbCoef(session.eval, varname, kind)) {
    if (stored) *stored = *v;
    return fmtValueFullPrecision(*v);
  }
  // An abbreviated / reordered spelling of a posted column -- the macro twin's
  // eval::coefFullName.
  if (const std::string full = eval::coefFullName(*source, varname); !full.empty())
    return resolveBRef(full, kind, session, err, stored);
  // Name not in the model — BUG-162. r(111), the code the macro twin carries
  // (macros.cpp resolveBRef via splitErrCode's default): `display _b[9.c]` was rc 1
  // where `display _b[9.c]+0` and `local q = _b[9.c]` were r(111) -- two codes
  // for one read once reBRef took level names (BUG-629). The base level
  // (`_b[0b.c]`, `_b[0.c]`) is 0 in Stata (w2f_residuals.log e_b_0bc/e_b_0c) but
  // has no coeftable row here, so it is refused at r(111) on both layers: an
  // honest refusal, named in the task report as an oracle-adjudicated follow-up.
  // Text: Stata brackets the name (StataNow 19.5 BE: `display _b[9.c]` and
  // `display %18.0g _se[9.c]` both print `[9.c] not found`); the macro twin agrees.
  if (err) *err = "r(111): [" + varname + "] not found";
  return ".";
}

// _resolve_N
// BUG-512: ONE source for `_N` -- SessionState::macroObsCount(), the same value
// the macro layer's hook reads. This used to answer `.` with no data in memory
// where Stata answers 0 (w2_smalls.log: `set obs 12` reports "_N was 0" after
// `clear all`), and once evalArith learned `_N` the two spellings of one line
// would have disagreed: `display _N` -> `.`, `display _N*2` -> 0. It also stops
// opening a Datastore for a count the resident card already carries.
std::string resolveN(const std::string& token, SessionState& session) {
  if (token == "_n") return "1";
  return std::to_string(session.macroObsCount());
}

// _resolve_c — c(name) system values. BETA A4 (BS-091/BS-101): the narrow
// current_date/current_time-only resolver that used to live here (BUG-091)
// is now folded into piet::resolveCValue (piet/cclass.h) — the single
// source EVERY expression context reads c() through (display here, plus
// gen/replace/if/scalar/local via piet::substituteCRefs), not just display's
// own tokenizer. See cclass.h for the full ~15-key spine and the "honest gap
// for anything else" philosophy this preserves verbatim.

// BUG-341: VAR[idx] (1-based) -- the value display prints for a variable
// subscript; nullopt when `name` is not a dataset variable. A string variable
// answers its text verbatim (isString), a numeric one its full-precision value,
// an extended missing its own `.a`-`.z` token (the data plane keeps the kind in
// the NaN payload, data/missing.h). Out of range and index 0 give `.`, or ""
// for a string column.
struct VarObs {
  std::string text;
  bool isString = false;
  bool inRange = true;      // BUG-718: false for an index outside 1.._N
  bool colString = false;   // BUG-718: the column is a string one (set in range or not)
};

// name-binding-display-literals: `name` may be a unique abbreviation of a
// variable (resolveVarName, the one rule); the column read is the variable's.
std::optional<VarObs> readVarObs(const std::string& nameIn, long long idx, SessionState& session) {
  const auto numText = [](double v) {
    return std::isnan(v) ? std::string(cecon::missing_text(cecon::missing_kind(v)))
                         : fmtValueFullPrecision(v);
  };
  const std::optional<std::string> resolvedName = resolveVarName(nameIn, session);
  if (!resolvedName) return std::nullopt;
  const std::string& name = *resolvedName;
  // The computed-column cache answers a freshly `gen`d / `destring`d column
  // without touching disk — the same surface every other read consumer uses.
  if (const auto* cc = session.residentComputedCol(name)) {
    const bool str = cc->dtype == "factor";
    // Out of range a string column reads "" (w2ar_data_io_family.log
    // dx_fx_fc15_L1map `0||`: dxmap[1] with _N 0), a numeric one `.`.
    if (idx < 1 || idx > cc->nRows) return VarObs{str ? "" : ".", str, false, str};
    const size_t k = static_cast<size_t>(idx - 1);
    // A string variable has no missing value: a cell with no text is "".
    if (str) return VarObs{k < cc->strs.size() ? cc->strs[k] : std::string(), true, true, true};
    return VarObs{k < cc->f64.size() ? numText(cc->f64[k]) : std::string("."), false};
  }
  Datastore* ds = session.datastore();  // materializes a virtual head if needed
  if (ds == nullptr || !ds->has(name)) return VarObs{".", false};
  const bool str = ds->dtype(name) == "factor";
  if (idx < 1 || idx > ds->nRows()) return VarObs{str ? "" : ".", str, false, str};
  const size_t k = static_cast<size_t>(idx - 1);
  const cecon::Column& col = ds->col(name);
  if (str) {
    const int32_t code = col.codes()[k];
    const auto& levels = col.levels;
    if (code >= 0 && static_cast<size_t>(code) < levels.size())
      return VarObs{levels[static_cast<size_t>(code)], true, true, true};
    return VarObs{std::string(), true, true, true};
  }
  return VarObs{numText(col.f64()[k]), false};
}

// A `,` outside quotes and brackets: `NAME[i,j]` is a matrix element, not a
// variable subscript (`x[max(1,2)]` is one index).
bool hasTopLevelComma(const std::string& inner) {
  int depth = 0;
  for (size_t k = 0; k < inner.size(); ++k) {
    const char c = inner[k];
    if (c == '"') {
      const size_t close = inner.find('"', k + 1);
      if (close == std::string::npos) return false;
      k = close;
    } else if (c == '(' || c == '[') {
      ++depth;
    } else if (c == ')' || c == ']') {
      --depth;
    } else if (c == ',' && depth == 0) {
      return true;
    }
  }
  return false;
}

// BUG-1339: the observation a variable subscript names. The index is any
// numeric expression (`x[1+1]`, `x[_N-1]`, `x[_n+1]`); StataBE 19.5 truncates a
// fractional one (`x[1.5]` is x[1], `x[2.9]` x[2]) and reads a missing one as
// out of range (`x[.]`, `x[1/0]` are `.`), so both come back as an index
// readVarObs answers. nullopt = not a numeric expression.
std::optional<long long> obsIndex(const std::string& raw, SessionState& session) {
  const std::string idx = strip(eval::substituteObsSpecials(raw, session.eval));
  if (idx.empty()) return std::nullopt;
  std::string raised;
  bool isStr = false;
  auto r = eval::evalArith(idx, &session.eval, &raised, &isStr);
  if (!r.has_value() || isStr || !raised.empty() || r->empty()) return std::nullopt;
  if (piet::fmt::isMissingToken(*r)) return 0;
  char* end = nullptr;
  const double v = std::strtod(r->c_str(), &end);
  if (end == nullptr || *end != '\0') return std::nullopt;
  if (!(std::fabs(v) < 9e15)) return 0;  // NaN and the missing band too
  return static_cast<long long>(std::trunc(v));
}

// FU-640: `name[i]` naming a STRING variable -> its text; nullopt otherwise.
std::optional<std::string> stringVarSubscript(const std::string& token, SessionState& session) {
  static const std::regex reVarSub(std::string("^(") + eval::kRxName +
                                   R"()\s*\[\s*(-?\d+)\s*\]$)");
  if (token.find('[') == std::string::npos) return std::nullopt;
  const std::string subTok = eval::substituteObsSpecials(token, session.eval);
  std::smatch vs;
  if (!std::regex_match(subTok, vs, reVarSub)) return std::nullopt;
  long long idx = 0;
  try {
    idx = std::stoll(vs[2].str());
  } catch (...) {
    return std::nullopt;
  }
  auto obs = readVarObs(vs[1].str(), idx, session);
  if (!obs || !obs->isString) return std::nullopt;
  return obs->text;
}

// FU-640: every bare identifier in `expr` that names a dataset variable is
// replaced by its observation-1 value: a number (parenthesised) or a quoted
// string. Quoted text, function names (`name(`), subscripted names (`name[`)
// and member names (`.name`) are left alone. nullopt when nothing was bound,
// or when a string value holds a `"` this layer's literals cannot carry.
std::optional<std::string> bindBareVarsToObs1(const std::string& expr, SessionState& session) {
  std::string out;
  bool any = false;
  size_t i = 0;
  // core/ident.h: a byte >= 0x80 is a name byte, so a Unicode name is one name.
  const auto isId0 = [](char c) { return cecon::ident::isIdentStartByte(c); };
  const auto isId = [](char c) { return cecon::ident::isIdentByte(c); };
  while (i < expr.size()) {
    const char c = expr[i];
    if (c == '"') {
      const size_t j = expr.find('"', i + 1);
      const size_t end = j == std::string::npos ? expr.size() : j + 1;
      out.append(expr, i, end - i);
      i = end;
      continue;
    }
    if (!isId0(c) || (i > 0 && (isId(expr[i - 1]) || expr[i - 1] == '.'))) {
      out += c;
      ++i;
      continue;
    }
    size_t j = i;
    while (j < expr.size() && isId(expr[j])) ++j;
    const std::string name = expr.substr(i, j - i);
    size_t k = j;
    while (k < expr.size() && std::isspace(static_cast<unsigned char>(expr[k]))) ++k;
    const bool callOrSub = k < expr.size() && (expr[k] == '(' || expr[k] == '[');
    std::optional<VarObs> obs;
    if (!callOrSub) obs = readVarObs(name, 1, session);
    if (!obs) {
      out += name;
    } else if (obs->isString) {
      if (obs->text.find('"') != std::string::npos) return std::nullopt;
      out += "\"" + obs->text + "\"";
      any = true;
    } else {
      out += "(" + obs->text + ")";
      any = true;
    }
    i = j;
  }
  if (!any) return std::nullopt;
  return out;
}

// FU-207: a variable subscript `name[i]` INSIDE an expression -- `missing(g[5])`,
// `(g[5] >= .)` -- is replaced by the observation's value, the same reading
// resolveToken's 8b arm gives a lone `g[5]` (readVarObs: a number parenthesised,
// `.`/`.a` for a missing cell). Before this only the lone token was read;
// inside a call the subscript reached the evaluator as text and every such line
// was r(198) "invalid syntax". The index is an integer literal once `_N`/`_n`
// are substituted (the 8b rule); a name that is not a dataset variable, a
// `[i,j]` matrix subscript and quoted text are left alone. So is a STRING
// variable's subscript: a bound `"ab"` beside unquoted text is not evaluated as
// a string expression downstream but echoed (`display s[2]+"c"` printed
// `"ab"+c` at rc 0 where Stata prints `abc`), so it keeps bin/main's path
// (r(111) there). nullopt when nothing was bound.
std::optional<std::string> bindVarSubscripts(const std::string& expr, SessionState& session) {
  if (expr.find('[') == std::string::npos) return std::nullopt;
  static const std::regex reSub(std::string("^(") + eval::kRxName + R"()\s*\[([^\[\],"]*)\])");
  std::string out;
  bool any = false;
  size_t i = 0;
  const auto isId = [](char c) { return cecon::ident::isIdentByte(c); };  // core/ident.h
  while (i < expr.size()) {
    const char c = expr[i];
    if (c == '"') {
      const size_t j = expr.find('"', i + 1);
      const size_t end = j == std::string::npos ? expr.size() : j + 1;
      out.append(expr, i, end - i);
      i = end;
      continue;
    }
    std::smatch m;
    const std::string rest = expr.substr(i);
    if (cecon::ident::isIdentStartByte(c) &&
        (i == 0 || (!isId(expr[i - 1]) && expr[i - 1] != '.')) &&
        std::regex_search(rest, m, reSub)) {
      const std::string idxText =
          strip(eval::substituteObsSpecials(m.str(2), session.eval));
      long long idx = 0;
      bool intLit = !idxText.empty();
      for (size_t k = 0; intLit && k < idxText.size(); ++k)
        intLit = std::isdigit(static_cast<unsigned char>(idxText[k])) != 0 ||
                 (k == 0 && idxText[k] == '-' && idxText.size() > 1);
      if (intLit) {
        try {
          idx = std::stoll(idxText);
        } catch (...) {
          intLit = false;
        }
      }
      std::optional<VarObs> obs;
      if (intLit) obs = readVarObs(m.str(1), idx, session);
      if (obs && obs->isString) {
        out += m.str(0);
        i += static_cast<size_t>(m.length(0));
        continue;
      }
      if (obs) {
        out += "(" + obs->text + ")";
        any = true;
        i += static_cast<size_t>(m.length(0));
        continue;
      }
      out += m.str(1);
      i += m.str(1).size();
      continue;
    }
    out += c;
    ++i;
  }
  if (!any) return std::nullopt;
  return out;
}

// FU-142 / FU-853: the index one past the end of the quoted span that starts
// at `i` -- a plain "..." literal or a compound `"..."' one (which nests) --
// or `i` itself when no span starts there. An unterminated span runs to the end.
size_t skipQuotedSpan(const std::string& s, size_t i) {
  if (i < s.size() && s[i] == '"') {
    const size_t j = s.find('"', i + 1);
    return j == std::string::npos ? s.size() : j + 1;
  }
  if (i + 1 < s.size() && s[i] == '`' && s[i + 1] == '"') {
    int depth = 0;
    size_t j = i;
    while (j < s.size()) {
      if (j + 1 < s.size() && s[j] == '`' && s[j + 1] == '"') {
        ++depth;
        j += 2;
      } else if (j + 1 < s.size() && s[j] == '"' && s[j + 1] == '\'') {
        j += 2;
        if (--depth == 0) return j;
      } else {
        ++j;
      }
    }
    return s.size();
  }
  return i;
}

// The index of the bracket closing the one opened at `open` (`(` or `[`),
// quote-aware and counting both kinds; npos when it never closes.
size_t matchingClose(const std::string& s, size_t open) {
  int depth = 0;
  size_t i = open;
  while (i < s.size()) {
    const size_t q = skipQuotedSpan(s, i);
    if (q != i) {
      i = q;
      continue;
    }
    const char c = s[i];
    if (c == '(' || c == '[') {
      ++depth;
    } else if (c == ')' || c == ']') {
      if (--depth == 0) return i;
    }
    ++i;
  }
  return std::string::npos;
}

// A display item that is a parenthesised group `(...)` or a function call
// `name(...)`, its opening parenthesis closed by the item's last character.
bool isGroupItem(const std::string& item) {
  size_t open = 0;
  if (!item.empty() && item[0] != '(') {
    if (!cecon::ident::isIdentStartByte(item[0])) return false;
    while (open < item.size() && cecon::ident::isIdentByte(item[open])) ++open;
  }
  if (open >= item.size() || item[open] != '(') return false;
  return matchingClose(item, open) == item.size() - 1;
}

// display's own DIRECTIVES (`_n`, `_col(#)`, `_dup(#)`, ...): display's
// language, not expressions, and a named gap of this layer rather than an
// error to classify (BUG-293(b)).
bool isDisplayDirective(const std::string& item) {
  static const std::regex reDisplayDirective(
      R"(^_(n|newline|continue|asis|request|skip|col|column|dup|char)\b.*)");
  return std::regex_match(item, reDisplayDirective);
}

// FU-142: the items of an unquoted display segment when it is two or more
// ADJACENT parenthesised groups or function calls -- `("a"=="b") ("a"=="a")`,
// `(1)(2)`, `missing(x[1]) missing(x[2])`. Stata evaluates each as its own
// expression and prints them back to back (w2f_residuals.log:76 lt_01 ends
// `|001|`); joined into one string they are no expression at all, and display
// echoed the source. Tokens are already cut at top-level whitespace; a token is
// cut again where a group closes at depth 0 and the next one opens at once.
// nullopt for anything else (one item, or any item that is a bare word, an
// operator or a subscript), so prose and ordinary expressions keep their route.
std::optional<std::vector<std::string>> splitAdjacentGroups(
    const std::vector<std::string>& tokens) {
  std::vector<std::string> items;
  for (const auto& tok : tokens) {
    size_t start = 0;
    size_t i = 0;
    int depth = 0;
    while (i < tok.size()) {
      const size_t q = skipQuotedSpan(tok, i);
      if (q != i) {
        i = q;
        continue;
      }
      const char c = tok[i];
      if (c == '(' || c == '[') {
        ++depth;
      } else if (c == ')' || c == ']') {
        --depth;
        if (depth == 0 && c == ')' && i + 1 < tok.size() && tok[i + 1] == '(') {
          items.push_back(tok.substr(start, i + 1 - start));
          start = i + 1;
        }
      }
      ++i;
    }
    items.push_back(tok.substr(start));
  }
  if (items.size() < 2) return std::nullopt;
  // A subscript `NAME[...]` is an item of its own as well: `display t[1] t[2]
  // t[3]` prints `123` (w2_smalls.log:148-149 sort_stable).
  // `_b[...]`/`_se[...]` keep their verbatim (%.9g) route as one segment.
  const auto isSubscriptItem = [](const std::string& item) {
    size_t j = 0;
    if (item.empty() || !cecon::ident::isIdentStartByte(item[0])) return false;
    if (item.rfind("_b[", 0) == 0 || item.rfind("_se[", 0) == 0 || item.rfind("_coef[", 0) == 0)
      return false;
    while (j < item.size() && cecon::ident::isIdentByte(item[j])) ++j;
    if (j >= item.size() || item[j] != '[' || item.back() != ']') return false;
    int depth = 0;
    for (size_t k = j; k < item.size(); ++k) {
      if (item[k] == '[') ++depth;
      else if (item[k] == ']' && --depth == 0 && k + 1 != item.size()) return false;
    }
    return depth == 0;
  };
  for (const auto& item : items)
    if ((!isGroupItem(item) && !isSubscriptItem(item)) || isDisplayDirective(item))
      return std::nullopt;
  return items;
}

// BUG-1333: the expressions of an unquoted display segment. Stata's display
// reads the longest expression it can and starts the next item where one
// operand is followed, across a blank, by another: `display %9.2f price xd`
// formats price and prints xd at the default (`    10.00.33333334`), `1 2` is
// `12`, `1 -2` is one expression (-1) and `abs (-1)` is `abs not found`
// (StataBE 19.5). The segment's tokens were joined into one expression, which
// failed and echoed the format and the source (`%9.2f10.0 0.3333333432674408`
// at rc 0). Tokens are already cut at top-level blanks; a token ending an
// operand followed by one opening an operand (not a binary operator) starts a
// new expression. A `!` or `~` does not open one: Stata keeps `1 !2` in one
// expression and refuses it (`1!2 invalid name` r(198), `1 !(2)` r(133)).
std::vector<std::vector<std::string>> splitExpressions(const std::vector<std::string>& tokens) {
  const auto opensOperand = [](const std::string& t) {
    const unsigned char c = static_cast<unsigned char>(t[0]);
    return std::isalnum(c) || c == '_' || c == '.' || c == '(' || c == '"' || c == '`' || c >= 0x80;
  };
  const auto endsOperand = [](const std::string& t) {
    const unsigned char c = static_cast<unsigned char>(t.back());
    return std::isalnum(c) || c == '_' || c == '.' || c == ')' || c == ']' || c == '"' ||
           c == '\'' || c >= 0x80;
  };
  std::vector<std::vector<std::string>> out;
  for (const auto& tok : tokens) {
    if (tok.empty()) continue;
    if (out.empty() || (endsOperand(out.back().back()) && opensOperand(tok)))
      out.emplace_back();
    out.back().push_back(tok);
  }
  return out;
}

// BUG-1333: an expression where `!` or `~` follows an operand across a blank
// (`1 !2`, `x !x`, `3 !abs(2)`). Stata reads the operand's last word, the
// operator and the next word as one name and refuses it: `1!2 invalid name`
// r(198), `unknown function 3!abs()` r(133) (StataBE 19.5; `1+1 !2` is `1!2`
// too). The message with its r(#) prefix, or nothing when the expression has no
// such juxtaposition or the operand does not end in a word (`(1) !2`, `x[1] !2`
// take other paths in Stata).
std::optional<std::string> juxtaposedNotError(const std::vector<std::string>& expr) {
  const auto isWordByte = [](char c) {
    const unsigned char u = static_cast<unsigned char>(c);
    return std::isalnum(u) || c == '_' || c == '.' || u >= 0x80;
  };
  for (size_t j = 1; j < expr.size(); ++j) {
    const std::string& tok = expr[j];
    if (tok.empty() || (tok[0] != '!' && tok[0] != '~')) continue;
    if (tok.size() > 1 && tok[1] == '=') continue;
    const std::string& prev = expr[j - 1];
    size_t w = prev.size();
    while (w > 0 && isWordByte(prev[w - 1])) --w;
    if (w == prev.size()) continue;
    std::string tail = tok;
    for (size_t k = j + 1; k < expr.size() && tail.find_first_not_of("!~") == std::string::npos; ++k)
      tail += expr[k];
    size_t e = tail.find_first_not_of("!~");
    if (e == std::string::npos) e = tail.size();
    while (e < tail.size() && isWordByte(tail[e])) ++e;
    const std::string name = prev.substr(w) + tail.substr(0, e);
    if (e < tail.size() && tail[e] == '(') return "r(133): unknown function " + name + "()";
    return "r(198): " + name + " invalid name";
  }
  return std::nullopt;
}

// FU-855 / FU-201: true when a lone display item is an expression by Stata's
// grammar -- a parenthesised group, a function call, a subscript, or operators
// over an operand that starts like a number (`1/0`, `-x*2`). A bare word
// (`display "text" foo`) is not, nor is a word run into punctuation (`a-b`,
// `/usr/bin`, the `one>|` a mangled quote leaves): that is the prose echo this
// layer keeps on purpose.
bool isExpressionItem(const std::string& item) {
  if (item.empty()) return false;
  if (isGroupItem(item)) return true;
  bool op = false;
  size_t i = 0;
  while (i < item.size()) {
    const size_t q = skipQuotedSpan(item, i);
    if (q != i) {
      i = q;
      continue;
    }
    if (item[i] == '[') return true;
    if (std::string("+-*/^<>=!&|").find(item[i]) != std::string::npos) op = true;
    ++i;
  }
  const unsigned char c0 = static_cast<unsigned char>(item[0]);
  const bool numberLike =
      std::isdigit(c0) || c0 == '-' || c0 == '+' || c0 == '!' ||
      (c0 == '.' && item.size() > 1 && std::isdigit(static_cast<unsigned char>(item[1])));
  return op && numberLike;
}

// FU-853 / FU-226 / FU-349: the subscripts inside a display expression,
// resolved by the display layer before evalArith sees it. evalArith reads a
// `NAME[...]` as a matrix element with literal indices only, so a dataset cell
// inside a call or an operation (`year(d[1])`, `missing(x[2])`, `t[2]*2`) was
// `t not found` or invalid syntax, and a matrix index that is itself an
// expression (`c[1,colsof(c)-1]`) was echoed.
//   * `VAR[i]`, VAR a dataset variable and i an integer-valued expression
//     (`_n`/`_N` included): the cell's value, a number parenthesised (so a
//     negative cell keeps its sign under `^`: `t[1]^2` is `(-1)^2`) at full
//     precision, or a quoted string. Out of range is `.`, as in 8b.
//   * `MAT[i,j]`, MAT a defined matrix: each index evaluated to its integer,
//     leaving `MAT[2,3]` for evalArith's own matrix pass.
// `_b[...]`/`_se[...]` and anything inside quotes are left alone, and so is a
// subscript whose index does not evaluate to an integer. nullopt when nothing
// changed, or when a string cell holds a `"` this layer's literals cannot carry.
// `unknown`, when non-null, receives the first `NAME[...]` whose NAME is neither
// a variable nor a matrix: Stata's r(111) `NAME not found` (the one-index form
// is w2b_dataio.log:110-113; the `[i,j]` form takes the same code, unpinned).
std::optional<std::string> bindSubscripts(const std::string& expr, SessionState& session,
                                          std::string* unknown = nullptr) {
  // core/ident.h: a byte >= 0x80 is a name byte, so a Unicode name is one name.
  const auto isId0 = [](char c) { return cecon::ident::isIdentStartByte(c); };
  const auto isId = [](char c) { return cecon::ident::isIdentByte(c); };
  const auto intIndex = [&](const std::string& raw) -> std::optional<long long> {
    const std::string idx = strip(eval::substituteObsSpecials(raw, session.eval));
    if (idx.empty()) return std::nullopt;
    std::string raised;
    bool isStr = false;
    auto r = eval::evalArith(idx, &session.eval, &raised, &isStr);
    if (!r.has_value() || isStr || !raised.empty()) return std::nullopt;
    char* end = nullptr;
    const double v = std::strtod(r->c_str(), &end);
    if (r->empty() || end == nullptr || *end != '\0' || !std::isfinite(v) ||
        v != std::floor(v) || std::fabs(v) > 9e15)
      return std::nullopt;
    return static_cast<long long>(v);
  };
  const auto isMatrix = [&](const std::string& name) {
    for (const auto& kv : session.eval.matrices)
      if (kv.first == name) return true;
    for (const auto& kv : session.eval.eMatrices)
      if (kv.first == name) return true;
    return false;
  };
  std::string out;
  bool any = false;
  size_t i = 0;
  while (i < expr.size()) {
    const size_t q = skipQuotedSpan(expr, i);
    if (q != i) {
      out.append(expr, i, q - i);
      i = q;
      continue;
    }
    const char c = expr[i];
    if (!isId0(c) || (i > 0 && (isId(expr[i - 1]) || expr[i - 1] == '.'))) {
      out += c;
      ++i;
      continue;
    }
    size_t j = i;
    while (j < expr.size() && isId(expr[j])) ++j;
    const std::string name = expr.substr(i, j - i);
    const size_t close = (j < expr.size() && expr[j] == '[') ? matchingClose(expr, j)
                                                              : std::string::npos;
    if (close == std::string::npos || name == "_b" || name == "_se" || name == "_coef") {
      out += name;
      i = j;
      continue;
    }
    const std::string inner = expr.substr(j + 1, close - j - 1);
    // A top-level comma splits a matrix's row and column index.
    size_t comma = std::string::npos;
    {
      int depth = 0;
      size_t k = 0;
      while (k < inner.size()) {
        const size_t qq = skipQuotedSpan(inner, k);
        if (qq != k) {
          k = qq;
          continue;
        }
        if (inner[k] == '(' || inner[k] == '[') ++depth;
        if (inner[k] == ')' || inner[k] == ']') --depth;
        if (inner[k] == ',' && depth == 0) {
          comma = k;
          break;
        }
        ++k;
      }
    }
    std::optional<std::string> replacement;
    if (comma != std::string::npos) {
      if (isMatrix(name)) {
        const auto r = intIndex(inner.substr(0, comma));
        const auto cIdx = intIndex(inner.substr(comma + 1));
        if (r && cIdx) {
          replacement = name + "[" + std::to_string(*r) + "," + std::to_string(*cIdx) + "]";
          if (*replacement == expr.substr(i, close + 1 - i)) replacement.reset();
        }
      } else if (unknown != nullptr && unknown->empty() && !readVarObs(name, 1, session)) {
        *unknown = name;
      }
    } else if (auto idx = obsIndex(inner, session)) {
      if (auto obs = readVarObs(name, *idx, session)) {
        if (obs->isString) {
          if (obs->text.find('"') != std::string::npos) return std::nullopt;
          replacement = "\"" + obs->text + "\"";
        } else {
          replacement = "(" + obs->text + ")";
        }
      } else if (unknown != nullptr && unknown->empty() && !isMatrix(name)) {
        *unknown = name;
      }
    }
    if (replacement) {
      out += *replacement;
      any = true;
    } else {
      out.append(expr, i, close + 1 - i);
    }
    i = close + 1;
  }
  if (!any) return std::nullopt;
  return out;
}

// The refusal display gives an expression that failed: the BUG-246
// classifier's code when it claims the shape; otherwise an unbound name is
// Stata's r(111) `NAME not found` (display already tried it as a variable and
// a scalar) and anything else r(198) `invalid syntax`. Never the evaluator's
// raw NameError text, which carries no code and answered `_rc` 1 (`display
// nosuchfn(1)`; StataBE: r(133) unknown function, exprfns_contract_oracle.log H).
std::string namedRefusal(const std::string& expr, const std::string& raised) {
  eval::MacroFnError me;
  if (eval::classifyFailedExpr(expr, raised, &me) && me.failed())
    return "r(" + me.rc + "): " + me.message;
  static const std::string kNameErr = "NameError: name '";
  if (raised.rfind(kNameErr, 0) == 0) {
    const size_t end = raised.find('\'', kNameErr.size());
    if (end != std::string::npos && end > kNameErr.size())
      return "r(111): " + raised.substr(kNameErr.size(), end - kNameErr.size()) + " not found";
  }
  return "r(198): invalid syntax";
}

// _resolve_token
//
// BUG-554 (W2D-T03): `stored`, when non-null, receives the double behind a
// `_b[]`, `_se[]`, `e(name)` or `e(name)[i,j]` answer. The text is unchanged
// (see resolveEScalar); handleDisplay renders an explicitly formatted bare item
// from `stored` so the format sees every bit.
//
// `strValue`, when non-null, is set when the answer is a STRING variable's cell
// text, which the caller must carry as a string literal, not as expression text.
std::string resolveToken(const std::string& token, SessionState& session,
                          std::string* err = nullptr, std::optional<double>* stored = nullptr,
                          bool* strValue = nullptr) {
  std::smatch m;
  double sv = 0.0;
  const auto keep = [&](const std::string& text, bool got) {
    if (got && stored) *stored = sv;
    return text;
  };

  // `[eq]_b[name]` / `[eq]_se[name]` are `_b[eq:name]` / `_se[eq:name]`
  // (BUG-1328), matched before the `[eq]column` form below.
  static const std::regex eqBRef(std::string(R"(\[()") + eval::kRxName +
                                 R"()\]_(b|se)\[([^\]]+)\])");
  if (std::regex_match(token, m, eqBRef)) {
    sv = std::nan("");
    const std::string t = resolveBRef(m[1].str() + ":" + m[3].str(), m[2].str(), session, err, &sv);
    return keep(t, !std::isnan(sv));
  }
  // Equation shorthand [eq]column is the same coefficient lookup.
  static const std::regex equationCoef(
      std::string(R"(\[()") + eval::kRxName +
      R"()\]((?:[A-Za-z_0-9]|[^\x00-\x7F])(?:[\w.#]|[^\x00-\x7F])*))");
  if (std::regex_match(token, m, equationCoef)) {
    sv = std::nan("");
    const std::string t = resolveBRef(m[1].str() + ":" + m[2].str(), "b", session, err, &sv);
    return keep(t, !std::isnan(sv));
  }
  // BUG-1545: an ancillary parameter named with parentheses (`_b[/var(e.yc)]`
  // after tobit), which reBRef/reSeRef do not take -- the macro twin's reAnc.
  {
    static const std::regex reAnc(R"(_(b|se)\[(/[A-Za-z_]\w*\([^()\[\]]*\))\])");
    if (std::regex_match(token, m, reAnc)) {
      sv = std::nan("");
      std::string t = resolveBRef(m[2].str(), m[1].str(), session, err, &sv);
      return keep(t, !std::isnan(sv));
    }
  }
  // 1. _b[var]
  if (std::regex_match(token, m, reBRef())) {
    sv = std::nan("");
    std::string t = resolveBRef(m[1].str(), "b", session, err, &sv);
    return keep(t, !std::isnan(sv));
  }
  // 2. _se[var]
  if (std::regex_match(token, m, reSeRef())) {
    sv = std::nan("");
    std::string t = resolveBRef(m[1].str(), "se", session, err, &sv);
    return keep(t, !std::isnan(sv));
  }
  // 3. e(name) — collapse redundant double-wrapping e(e(r2)).
  //    BUG-663: only when the token IS one reference. The anchored pattern also
  //    matches `e(cmd)==e(cmd)` (inner `cmd)==e(cmd`), which read as an absent
  //    e-scalar and printed `.` where StataBE prints 1; a `)` that closes before
  //    its `(` opens leaves the token to the expression path.
  const auto oneReference = [](const std::string& inner) {
    int depth = 0;
    for (char c : inner) {
      if (c == '(') ++depth;
      if (c == ')' && --depth < 0) return false;
    }
    return depth == 0;
  };
  if (std::regex_match(token, m, reERef()) && oneReference(m[1].str())) {
    std::string inner = m[1].str();
    while (true) {
      std::smatch mm;
      if (!std::regex_match(inner, mm, reERef())) break;
      inner = mm[1].str();
    }
    // e(sample) T2 (internal note esample/SPEC.md §5.4): the
    // estimation-sample mask is not an e-scalar, so resolveEScalar answered `.`
    // at rc 0 here (`display e(sample)`, measured on main; Stata prints 0 with no
    // results, w2_esample.log:100 A2, and observation 1's value after a fit,
    // :365 B16). Same reading as the macro layer (eval/macros.cpp
    // resolveEstScalarRefs' e(sample) arm): SessionState::esampleObs1, whose
    // refusal (a model PIET has no mask for) is the named r(198) on `err`.
    if (inner == "sample") {
      std::string why;
      if (std::optional<int> v = session.esampleObs1(&why)) return std::to_string(*v);
      if (err != nullptr && err->empty()) *err = "r(198): " + why;
      return token;
    }
    // BUG-456: string -> scalar (the e-matrix names never reach display's
    // token resolver as a bare value; `display e(b)` is refused upstream).
    if (auto lit = resolveEStringLiteral(inner, session)) return *lit;
    sv = std::nan("");
    std::string t = resolveEScalar(inner, session, &sv);
    return keep(t, !std::isnan(sv));
  }
  // 3b. W2D-T03 (FU-45): `e(NAME)[i,j]`, one element of an e-class matrix. It
  //     used to match nothing here and printed its own source text at rc 0.
  //     The element comes from the POSTED e-matrix store -- the same store the
  //     macro layer's `e(NAME)[i,j]` / `el(e(NAME),i,j)` substitution reads
  //     (eval/macros.cpp substituteMatrixRefs) -- so display and `scalar s =`
  //     agree. A fit that has not posted the matrix is refused by name rather
  //     than answered from the coeftable: the coeftable carries neither Stata's
  //     column order (_cons last, base and omitted columns) nor V's
  //     off-diagonal, so any element built from it could be silently wrong.
  //     Unpinned (no golden displays e(b)[i,j]): the r(503) out-of-range and the
  //     r(111) not-posted refusals.
  {
    static const std::regex reEElem(
        std::string(R"(^e\(\s*()") + eval::kRxName +
        R"()\s*\)\s*\[\s*(\d+)\s*,\s*(\d+)\s*\]$)");
    std::smatch em;
    if (std::regex_match(token, em, reEElem)) {
      const std::string name = em[1].str();
      const eval::EvalMatrix* mat = session.eval.findClassMatrix('e', name);
      // BUG-1350: a fit's own e(b)/e(V) come from the one e() matrix resolver
      // the macro layer's `e(NAME)[i,j]` and el() read (Stata's column order,
      // base and omitted columns, e(V) only where the engine sent it), so
      // display answers what `local x = e(b)[1,2]` answers.
      std::optional<eval::EvalMatrix> fitMat;
      bool diagonalOnly = false;
      if (mat == nullptr) {
        auto a = session.eval.resolveEMatrixFor(name, eval::EvalSession::EReader::kOperand);
        if (a.matrix.has_value()) {
          fitMat = std::move(a.matrix);
          diagonalOnly = a.diagonalOnly;
          mat = &*fitMat;
        } else if (!a.absent) {
          if (err) *err = "r(" + std::to_string(a.rc) + "): " + a.why;
          return ".";
        }
      }
      if (mat == nullptr) {
        if (err) *err = "r(111): matrix e(" + name + ") not found";
        return ".";
      }
      long long i = 0, j = 0;
      try {
        i = std::stoll(em[2].str());
        j = std::stoll(em[3].str());
      } catch (...) {
        i = 0;
      }
      if (diagonalOnly && i != j) {
        if (err) {
          const std::string* cmd = session.eval.eStrings.find("cmd");
          *err = "r(198): e(V)[" + em[2].str() + "," + em[3].str() +
                 "] is not available in this version of PIET after `" +
                 (cmd != nullptr ? *cmd : std::string()) +
                 "`: the engine sent the fit's standard errors (the diagonal of e(V)), not "
                 "its covariances";
        }
        return ".";
      }
      if (i < 1 || j < 1 || static_cast<size_t>(i) > mat->data.size() ||
          static_cast<size_t>(j) > mat->data[static_cast<size_t>(i - 1)].size()) {
        if (err) *err = "r(503): conformability error";
        return ".";
      }
      sv = mat->data[static_cast<size_t>(i - 1)][static_cast<size_t>(j - 1)];
      if (std::isnan(sv)) return ".";
      return keep(fmtValueFullPrecision(sv), true);
    }
  }
  // 4. _N / _n
  if (isSpecialScalar(token)) {
    return resolveN(token, session);
  }
  // 4b. _pi, Stata's built-in constant: it printed its own name at rc 0
  //     (w2av_name_binding bd_dy_pi `[3.1415927]`, bd_dy_fpi `%21x _pi`).
  if (token == "_pi") return fmtValueFullPrecision(M_PI);
  // 4c. A dataset variable, by its name or a unique abbreviation of it,
  //     before a stored scalar of the same name, as in Stata: observation 1,
  //     a string variable's cell text (w2av_name_binding bd_dy_varsc `[10]`
  //     with scalar v = 5 beside variable v; bd_dy_strvar `[ab1]`, where the
  //     factor code 0 printed; bd_dy_abbr `lng` -> lngdp; bd_dy_abbrsc, where
  //     scalar lngd = 3 lost to the variable lngdp it abbreviates).
  if (std::regex_match(token, reIdent())) {
    if (auto obs = readVarObs(token, 1, session)) {
      if (obs->isString && strValue != nullptr) *strValue = true;
      return obs->text;
    }
  }
  // 5. User-defined named scalars (whole-token match) take precedence over
  //    e()-class bare aliases.
  {
    const std::string* sv = session.eval.scalars.find(token);
    if (sv != nullptr) return *sv;
  }
  // 6. Bare e()-class alias.
  if (eScalarKey().count(token) > 0) {
    if (auto lit = resolveEStringLiteral(token, session)) return *lit;  // BUG-456
    sv = std::nan("");
    std::string t = resolveEScalar(token, session, &sv);
    return keep(t, !std::isnan(sv));
  }
  // 7. (The dataset variable is step 4c, ahead of the scalars.)
  // 8. c(name) — system c-class values (BUG-091; BETA A4 BS-091/BS-101:
  //    the spine of the shared piet::resolveCValue, cclass.h). Any other
  //    c(...) name falls through to evalArith and is refused downstream, r(133)
  //    as the classifier words it (BUG-1330) — never silently swallowed.
  if (std::regex_match(token, m, reCRef())) {
    auto cv = resolveCValue(m[1].str(), session);
    if (cv.has_value()) {
      // BUG-1345: a string value is a string to the format that takes it:
      // `%-20s c(current_date)` fills the field (it was `invalid display
      // format %-20s`, rc 1).
      if (cv->isString && strValue != nullptr) *strValue = true;
      return cv->text;
    }
  }
  // 8b. BUG-341 — `var[i]` variable subscript. MUST precede the matrix arm
  //     below: that arm matches any `NAME[...]`, so a variable subscript fell
  //     into it, found no matrix of that name, and answered `matrix z2 not
  //     found` — wrong twice over (wrong object kind, and the variable exists).
  //     The form was never served by the resident plane at all: before subscript
  //     support arrived PIET echoed `z2[1]` literally, and when it arrived it
  //     read a surface the virtual head does not populate.
  //
  //     Semantics measured on StataBE 2026-08-14 (not inferred): in-range gives
  //     the row's value (`v[1]`→10, `v[5]`→50 on v=_n*10); OUT of range and
  //     index 0 give missing (`v[6]`→`.`, `v[0]`→`.`); a STRING variable gives
  //     the string, not a code (`s[1]`→`ab`). A `[i,j]` index keeps the matrix
  //     route.
  //
  //     BUG-512: `display x[_N]` -- the last observation, the commonest subscript
  //     there is -- never reached this arm, because `_N` is not an integer
  //     literal; it fell into the matrix arm below and answered `matrix x not
  //     found`. The index is resolved with the macro layer's own substitution
  //     (`_N` -> the count, `_n` -> 1: w2_smalls.log section A).
  //
  //     BUG-1339: the index is any expression (obsIndex): a lone `x[1+1]`,
  //     `x[_N-1]` or `x[`k'+1]` answered `matrix x not found` at rc 1, because
  //     only an integer literal was claimed here and bindSubscripts never sees a
  //     lone item that this arm has already refused. A string variable's cell is
  //     a string to the format that takes it (`%5s s[2]` fills the field).
  {
    // `display قيمة[2]`: a Unicode variable name is one name (core/ident.h).
    static const std::regex reVarSub(std::string("^(") + eval::kRxName + R"()\s*\[(.*)\]$)");
    std::smatch vs;
    if (std::regex_match(token, vs, reVarSub) && !hasTopLevelComma(vs[2].str()) &&
        resolveVarName(vs[1].str(), session)) {
      if (std::optional<long long> idx = obsIndex(vs[2].str(), session)) {
        if (auto obs = readVarObs(vs[1].str(), *idx, session)) {
          if (obs->colString && strValue != nullptr) *strValue = true;
          return obs->text;
        }
      }
    }
  }

  // 9. BUG-162 (W:P5) — `NAME[i,j]` matrix subscript. A DEFINED matrix is
  //    resolved by the arithmetic layer downstream; an UNDEFINED one used to
  //    fall through to the `return token` below and print its own source text
  //    (`ZZZNOSUCH[1,1]`) with rc 0, which reads like output. Stata raises
  //    r(111). Only the undefined case is claimed here — a defined matrix
  //    still reaches its existing resolver untouched.
  {
    static const std::regex reMatSub(std::string("^(") + eval::kRxName + R"()\s*\[[^\]]*\]$)");
    std::smatch mm;
    if (std::regex_match(token, mm, reMatSub)) {
      const std::string name = mm[1].str();
      bool defined = false;
      for (const auto& kv : session.eval.matrices) {
        if (kv.first == name) { defined = true; break; }
      }
      if (!defined) {
        for (const auto& kv : session.eval.eMatrices) {
          if (kv.first == name) { defined = true; break; }
        }
      }
      if (!defined) {
        // `NAME[...]` that names no matrix and that the variable arm (8b) did not
        // read is Stata's r(111) `NAME not found` whatever the index:
        // w2b_dataio.log:110-113 (`display ... xs[1]` after a tostring that
        // skipped generate(xs)), and StataBE 19.5 (2026-11-03) `nosuch[1+1]`,
        // `ZZZNOSUCH[1,1]` and, for a variable x, `x[]` and `x[1,1]`; a name that
        // begins two variables' names is `NAME ambiguous abbreviation`, r(111).
        // Each was `matrix NAME not found` at rc 1 but for a one-integer index.
        bool ambiguous = false;
        (void)resolveVarName(name, session, &ambiguous);
        if (err) *err = "r(111): " + name + (ambiguous ? " ambiguous abbreviation" : " not found");
        return ".";
      }
    }
  }

  return token;
}

// name-binding-display-literals: a string value in a `%#s` / `%-#s` / `%~#s`
// field (reStringFmtSpec): blanks to the left, to the right, or split with the
// odd one on the right. A value as wide as the field or wider prints whole.
std::string padStringField(const std::string& fmt, const std::string& text) {
  size_t k = 1;
  const char align = (k < fmt.size() && (fmt[k] == '-' || fmt[k] == '~')) ? fmt[k++] : '\0';
  size_t width = 0;
  while (k < fmt.size() && std::isdigit(static_cast<unsigned char>(fmt[k])))
    width = width * 10 + static_cast<size_t>(fmt[k++] - '0');
  size_t len = 0;
  for (char c : text)
    if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++len;
  if (len >= width) return text;
  const size_t fill = width - len;
  if (align == '-') return text + std::string(fill, ' ');
  if (align == '~') return std::string(fill / 2, ' ') + text + std::string(fill - fill / 2, ' ');
  return std::string(fill, ' ') + text;
}

// name-binding-display-literals: the first bare name in display text that binds
// to nothing -- no variable (exact or unique abbreviation), stored scalar,
// matrix or built-in -- as Stata's refusal: r(111) `NAME not found`, or `NAME
// ambiguous abbreviation` when it begins two variables' names (w2av_name_binding
// bd_dy_unb, bd_dy_ambig, bd_dy_errkw: `display "[" foo "]"` printed `[foo]` at
// rc 0). Quoted text, calls (`name(`), subscripts (`name[`), members and
// missing codes (`.a`) and the inside of numbers are not names. nullopt when
// every name binds.
std::optional<std::string> unboundNameRefusal(const std::string& text, SessionState& session) {
  // core/ident.h: a byte >= 0x80 is a name byte, so a Unicode name is one name.
  const auto isId0 = [](char c) { return cecon::ident::isIdentStartByte(c); };
  const auto isId = [](char c) { return cecon::ident::isIdentByte(c); };
  size_t i = 0;
  while (i < text.size()) {
    const size_t q = skipQuotedSpan(text, i);
    if (q != i) {
      i = q;
      continue;
    }
    const char c = text[i];
    if (!isId0(c) || (i > 0 && (isId(text[i - 1]) || text[i - 1] == '.'))) {
      ++i;
      continue;
    }
    size_t j = i;
    while (j < text.size() && isId(text[j])) ++j;
    const std::string name = text.substr(i, j - i);
    size_t k = j;
    while (k < text.size() && std::isspace(static_cast<unsigned char>(text[k]))) ++k;
    i = j;
    if (k < text.size() && (text[k] == '(' || text[k] == '[')) continue;
    if (name == "_n" || name == "_N" || name == "_pi" || name == "_rc") continue;
    if (session.eval.scalars.find(name) != nullptr) continue;
    if (session.eval.findMatrix(name) != nullptr) continue;
    bool ambiguous = false;
    if (resolveVarName(name, session, &ambiguous)) continue;
    return ambiguous ? "r(111): " + name + " ambiguous abbreviation" : "r(111): " + name + " not found";
  }
  return std::nullopt;
}

// A resolved _b[]/_se[]/e() token as display's VERBATIM fallback prints it (the
// line did not evaluate as one expression: `display _col(3) e(N)`, `display "E|"
// _b[x1] _b[2.c]`). The resolvers hand back full-precision text (BUG-554) because
// an expression or a %fmt re-renders it; the verbatim fallback renders nothing, so
// there the value prints as it did before BUG-554, `%.9g`: `60`, not `60.0`, and
// nine significant digits, not seventeen. Non-numeric text is returned as is.
std::string verbatimCoefText(const std::string& token, const std::string& resolved) {
  if (!(std::regex_match(token, reBRef()) || std::regex_match(token, reSeRef()) ||
        std::regex_match(token, reERef()) || eScalarKey().count(token) > 0))
    return resolved;
  char* end = nullptr;
  const double v = std::strtod(resolved.c_str(), &end);
  if (resolved.empty() || end == nullptr || *end != '\0' || !std::isfinite(v)) return resolved;
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.9g", v);
  return std::string(buf);
}

}  // namespace

// BUG-1248: see display_hardening.h.
std::optional<std::string> exprUnboundNameRefusal(const std::string& text, SessionState& session) {
  return unboundNameRefusal(text, session);
}

// name-binding-display-literals: see display_hardening.h.
std::optional<std::string> resolveVarName(const std::string& name, SessionState& session,
                                          bool* ambiguous) {
  if (ambiguous != nullptr) *ambiguous = false;
  // Stata's reserved names ([U] 11.3) are never a variable's, so never an
  // abbreviation of one either (`_pi` must not read a variable `_pix`).
  static const char* const kReserved[] = {"_all", "_b",    "byte",   "_coef", "_cons", "double",
                                          "float", "if",   "in",     "int",   "long",  "_n",
                                          "_N",    "_pi",  "_pred",  "_rc",   "_se",   "_skip",
                                          "strL",  "using", "with"};
  if (name.empty()) return std::nullopt;
  for (const char* r : kReserved)
    if (name == r) return std::nullopt;
  if (session.residentComputedCol(name) != nullptr) return name;
  // The shadow card knows without an mmap; datastore() is the honest fallback
  // when there is no current card (and for an exact name the card lacks).
  const std::vector<VarInfo>* card = session.residentReadCard();
  if (card != nullptr) {
    for (const auto& vi : *card)
      if (vi.name == name) return name;
  }
  Datastore* ds = nullptr;
  if (session.currentHbin().has_value()) {
    ds = session.datastore();
    if (ds != nullptr && ds->has(name)) return name;
  }
  std::optional<std::string> hit;
  bool many = false;
  const auto consider = [&](const std::string& v) {
    if (v.size() <= name.size() || v.compare(0, name.size(), name) != 0) return;
    if (hit) many = true;
    else hit = v;
  };
  if (card != nullptr) {
    for (const auto& vi : *card) consider(vi.name);
  } else if (ds != nullptr) {
    for (const auto& v : ds->names()) consider(v);
  }
  if (many) {
    if (ambiguous != nullptr) *ambiguous = true;
    return std::nullopt;
  }
  return hit;
}

// BUG-718: the macro layer's reader of `VAR[idx]` (SessionState::macroVarObs)
// -- display's own readVarObs, so the two spellings of one subscript agree.
std::optional<VarObservation> readVarObservation(const std::string& name, long long idx,
                                                 SessionState& session) {
  std::optional<VarObs> o = readVarObs(name, idx, session);
  if (!o) return std::nullopt;
  // Outside 1.._N display keeps its `.`; the macro layer is told the column's
  // kind so it can give a string column its "" (VarObservation).
  return VarObservation{o->text, o->inRange ? o->isString : o->colString, o->inRange};
}

// Exported at the P6 wave (see display_hardening.h) so the resampling tables
// share this one implementation instead of growing a second copy.
std::string fmtStataDefaultG(double v) { return fmtStataDefaultGImpl(v); }

// display_hardening.h: `file write`'s numeric %fmt, through display's renderer.
std::string formatNumberAsDisplayed(const std::string& fmt, const std::string& valueText) {
  return applyStataFmtLocal(fmt, valueText);
}

// ---------------------------------------------------------------------------
// Public entry point — handle_display

ojson handleDisplay(const std::string& rawArgsIn, SessionState& session) {
  std::string rawArgs = rawArgsIn;

  // U7/S10 (RISK-2): strip a leading `as text|txt|result|res|error|err`
  // modifier (optionally `_continue`/`_n`), case-insensitive.
  {
    static const std::regex reAsMod(
        R"(^as\s+(?:text|txt|result|res|error|err)(?:_(?:continue|n))?\s+)",
        std::regex::icase);
    std::string stripped0 = strip(rawArgs);
    std::smatch m0;
    if (std::regex_search(stripped0, m0, reAsMod) && m0.position(0) == 0) {
      rawArgs = stripped0.substr(m0.length(0));
    }
  }

  std::string s = strip(rawArgs);

  std::vector<std::string> parts;
  std::vector<std::string> unquotedTokens;
  std::optional<std::string> pendingFmt;
  // BUG-088: set when an unresolved call-syntax token (an extended macro
  // function PIET doesn't support) raised a genuine error — surfaced as an
  // honest engine_error instead of echoing the literal source text.
  std::optional<std::string> displayError;
  // How many parts were printed before the piece that raised displayError:
  // Stata writes each piece as it evaluates it and stops at the failing one,
  // so these stay on the line ahead of the message (the dispatcher's
  // displayPartialBlock reads them off the error block as `partial_text`).
  size_t partsBeforeError = 0;

  auto flushUnquotedPiece = [&]() {
    if (unquotedTokens.empty()) return;

    // BUG-087/D9 (BETA Phase F): a LONE `_request(macroName)` unquoted token
    // is handled as a dedicated case, not through the generic per-token
    // resolveToken() loop below — see reRequestCall()'s doc comment for why.
    // Scoped deliberately narrow (exactly one token, this exact shape) —
    // real Stata usage is always `_request(name)` standalone at the end of
    // a display statement (BUG-087's own repro); a token combined with
    // other unquoted text (e.g. `_request(x) foo`) falls through to the
    // generic path unchanged, where it hits the SAME pre-existing BUG-088
    // syntax-error-is-silent-literal-fallthrough behavior any other
    // malformed call-syntax expression gets — not a new gap introduced here.
    if (unquotedTokens.size() == 1) {
      std::smatch rm;
      if (std::regex_match(unquotedTokens[0], rm, reRequestCall())) {
        // Extract the macro name BEFORE clearing unquotedTokens: `rm` holds
        // iterators into unquotedTokens[0]'s string storage, so clearing the
        // vector first (destroying that string) would leave rm[1].str()
        // reading freed memory (caught live: a dangling-iterator bug that
        // produced garbage macro names during manual --do verification of
        // this exact code path before this fix).
        std::string macroName = rm[1].str();
        unquotedTokens.clear();
        std::string promptSoFar;
        for (const auto& p : parts) promptSoFar += p;
        std::string err;
        auto result = resolveRequestCall(macroName, promptSoFar, session, &err);
        if (result.has_value()) {
          // _request() contributes no further display text. A dangling
          // pending %fmt (e.g. `display %9.2f _request(x)`, not a real
          // usage anywhere in this codebase's corpus) has nothing numeric
          // left to format against — drop it rather than inventing a
          // formatting rule for a non-existent case.
          pendingFmt.reset();
          return;
        }
        if (!displayError) displayError = err;
        return;
      }
    }

    // FU-640: a lone string-variable subscript prints its text verbatim. It
    // used to be substituted unquoted and then evaluated as arithmetic, so
    // `" 12 "` printed `12`, `"1e3"` printed `1000` and `""` printed `.`
    // (w2x_fn_string_math fx_fix_s).
    if (unquotedTokens.size() == 1 && !pendingFmt.has_value()) {
      if (auto sv = stringVarSubscript(unquotedTokens[0], session)) {
        parts.push_back(*sv);
        unquotedTokens.clear();
        return;
      }
    }

    std::vector<std::string> resolved;
    resolved.reserve(unquotedTokens.size());
    std::vector<bool> resolvedString;  // a string variable's cell (resolveToken's strValue)
    std::optional<double> loneStored;  // BUG-554: the stored double of a lone item
    for (const auto& tok : unquotedTokens) {
      // BUG-162: an unresolvable coefficient name is an honest error, not a
      // silent `.`. First error wins, matching the displayError convention
      // the _request path above already uses.
      std::string tokErr;
      bool isStr = false;
      resolved.push_back(resolveToken(tok, session, &tokErr, &loneStored, &isStr));
      resolvedString.push_back(isStr);
      if (!tokErr.empty() && !displayError) displayError = tokErr;
    }
    // BUG-554 / FU-45 (W2D-T03): a bare `_b[x]`, `_se[x]`, `e(name)` or
    // `e(name)[i,j]` under an explicit format renders the STORED double, the
    // same text the same value held in a scalar gets. Its resolved text is the
    // nine-digit %.9g form, which is what `display %18.0g _b[x]` printed
    // (.333333333) and why `%21x _b[x]` printed a decimal. Only the lone,
    // formatted item takes this route: an unformatted item and any expression
    // built on one keep the text exactly as before.
    if (pendingFmt.has_value() && unquotedTokens.size() == 1 && loneStored.has_value() &&
        std::isfinite(*loneStored)) {
      unquotedTokens.clear();
      std::string fmtErr;
      // The text is the one `scalar s = <value>` stores (evalArith's number
      // form, eval/macros.cpp: an integer-valued double as its exact integer,
      // anything else as repr), so a spec this layer does not claim (`%20x`,
      // `%9s`, `%21X`) echoes `12`, never the repr `12.0` -- the same text the
      // scalar prints. Both forms parse back to the same double, so the
      // claimed specs are unaffected.
      const std::string storedText = eval::pyIsInteger(*loneStored)
                                         ? eval::pythonIntStrFromDouble(*loneStored)
                                         : eval::pythonFloatRepr(*loneStored);
      parts.push_back(applyStataFmtLocal(*pendingFmt, storedText, &fmtErr));
      if (!fmtErr.empty() && !displayError) displayError = fmtErr;
      pendingFmt.reset();
      return;
    }
    // BUG-293(b) needs to know whether ANY resolution actually happened: a
    // token that resolved to real data (`c(current_date)` -> a date string) is
    // a value to print, not an expression that failed to parse.
    std::string origJoined;
    for (size_t i = 0; i < unquotedTokens.size(); ++i) {
      if (i > 0) origJoined += " ";
      origJoined += unquotedTokens[i];
    }
    std::vector<std::string> srcTokens;  // for the verbatim fallback below
    srcTokens.swap(unquotedTokens);

    // BUG-710 / FU-1324: a STRING scalar's value is a string literal here, not
    // expression text: `scalar s2 = "d13[1]*3"` then `display s2` printed 1 (the
    // cell read FU-207's binder found inside the value) and `scalar s = "1+1"`
    // then `display s` printed 2, where StataBE prints the text. The macro
    // layer types the store (EvalSession::stringScalars); a value holding a
    // `"` keeps the old form.
    // name-binding-display-literals: a string variable's cell is the same kind of
    // value (w2av_name_binding bd_dy_strvar `[ab1]`, bd_dy_fstr `%9s s`).
    std::string joined;
    for (size_t i = 0; i < resolved.size(); ++i) {
      if (i > 0) joined += " ";
      if (resolvedString[i] && resolved[i].find('"') == std::string::npos) {
        joined += "\"" + resolved[i] + "\"";
        continue;
      }
      const std::string* stored = session.eval.scalars.find(srcTokens[i]);
      if (stored != nullptr && *stored == resolved[i] && session.eval.isStringScalar(srcTokens[i]) &&
          resolved[i].find('"') == std::string::npos) {
        joined += "\"" + resolved[i] + "\"";
        continue;
      }
      joined += resolved[i];
    }

    // BS-091/BS-101 (BETA A4): resolveToken's step 8 (above) only catches a
    // BARE whole-token `c(name)` (its regex is a full-string match) — a
    // c(name) reference COMBINED with real arithmetic (`c(N)*2`) never
    // matches that whole-token pattern and would otherwise reach evalArith
    // as literal, unresolved text. substituteCRefs is a general find-
    // anywhere pass (piet::eval::resolveEstScalarRefs's own established
    // shape), so it also correctly handles the plain bare-token case
    // resolveToken already resolved (a no-op there — no more literal
    // "c(...)" text remains to find).
    joined = substituteCRefs(joined, session);
    // The text before FU-207's binding: what the refusal gate below compares
    // with the source, what a refusal message names and what an echo prints
    // (never the bound `missing((5.0))`).
    const std::string sourceJoined = joined;
    if (auto bound = bindVarSubscripts(joined, session)) joined = *bound;  // FU-207

    std::string arithRaised;
    bool arithIsString = false;
    auto arithResult = eval::evalArith(joined, &session.eval, &arithRaised, &arithIsString);
    // FU-853 / FU-226 / FU-349: the subscripts FU-207's literal-index pass
    // leaves (an index that is an expression, `x[_n+1]`; a matrix index that
    // is one, `c[1,colsof(c)-1]`; a string cell, `length(s[1])`) are bound here
    // and the expression evaluated again (bindSubscripts). Only on failure, so
    // every expression that already evaluated keeps its answer, and only an
    // answer is taken from the bound form: when it fails too the original error
    // stands (`display s[2]+"c"` stays r(111), selftest_w2b_misc (9b)). Only
    // SOURCE tokens are bound here: a token that resolved is a value (a
    // scalar's stored text), not an expression of this line.
    std::string evalText = joined;
    std::string unknownSub;
    if (!arithResult.has_value()) {
      std::optional<std::string> bound;
      {
        std::string rebuilt;
        bool changed = false;
        for (size_t i = 0; i < resolved.size(); ++i) {
          std::string piece = resolved[i];
          if (piece == srcTokens[i]) {
            if (auto b = bindSubscripts(piece, session, &unknownSub)) {
              piece = *b;
              changed = true;
            }
          }
          if (i > 0) rebuilt += " ";
          rebuilt += piece;
        }
        if (changed) bound = substituteCRefs(rebuilt, session);
      }
      if (bound) {
        std::string raised2;
        bool isStr2 = false;
        auto r2 = eval::evalArith(*bound, &session.eval, &raised2, &isStr2);
        evalText = *bound;
        if (r2.has_value()) {
          arithResult = r2;
          arithIsString = isStr2;
          arithRaised.clear();
        }
      }
    }
    // FU-640: a bare variable name in a display expression reads observation 1,
    // as in Stata (`display "Growth: " string(v, "%9.1f")` prints v[1]; w2x
    // fx_string2_disp). Only on the NameError path, so a scalar or any name the
    // macro layer already binds keeps its meaning.
    if (!arithResult.has_value() && arithRaised.rfind("NameError", 0) == 0) {
      if (auto bound = bindBareVarsToObs1(evalText, session)) {
        std::string raised2;
        bool isStr2 = false;
        auto r2 = eval::evalArith(*bound, &session.eval, &raised2, &isStr2);
        if (r2.has_value()) {
          arithResult = r2;
          arithIsString = isStr2;
          arithRaised.clear();
        }
      }
    }

    if (arithResult.has_value()) {
      if (pendingFmt.has_value() && arithIsString && std::regex_match(*pendingFmt, reStringFmtSpec())) {
        // name-binding-display-literals: a string value under `%#s` fills the
        // field, right-justified (`%-#s` left, `%~#s` centred); it printed bare
        // (w2av_name_binding bd_dy_fstr `%9s s` -> `      ab1`).
        parts.push_back(padStringField(*pendingFmt, *arithResult));
        pendingFmt.reset();
      } else if (pendingFmt.has_value()) {
        // BUG-185: a malformed '-'-prefixed spec reports itself via fmtErr
        // instead of silently dropping the format or echoing the spec text.
        std::string fmtErr;
        parts.push_back(applyStataFmtLocal(*pendingFmt, *arithResult, &fmtErr));
        if (!fmtErr.empty() && !displayError) displayError = fmtErr;
        pendingFmt.reset();
      } else {
        // W:P5: no explicit %fmt -> Stata's default `%9.0g` for a numeric
        // result. Non-numeric results (string concatenation, an unresolved
        // literal) pass through untouched — only a value that round-trips
        // through strtod as the WHOLE string is reformatted.
        // BUG-293: a STRING-valued expression is printed verbatim, never
        // re-formatted as a number. `display string(42.5,"%9.2f")` is `42.50`
        // in Stata; routing it through the default %9.0g gave `42.5` — the
        // format the user asked string() for, undone one layer later.
        const std::string& ar = *arithResult;
        char* endp = nullptr;
        const double av = std::strtod(ar.c_str(), &endp);
        const bool wholeNumber = !arithIsString && !ar.empty() && endp != nullptr &&
                                 *endp == '\0' && std::isfinite(av);
        parts.push_back(wholeNumber ? fmtDisplayDefaultG(av) : ar);  // BUG-1016
      }
    } else {
      // BUG-1420: a c(NAME) that is no c() key at all is Stata's r(133)
      // "c(NAME) undefined" wherever it stands (`display c(bogus_setting)`,
      // `display 1 + c(bogus_setting)`); it was the evaluator's NameError at
      // rc 1 (lane expr-sem x1b/a25.do).
      if (!displayError) {
        if (auto undef = eval::undefinedCRef(sourceJoined)) displayError = "r(133): " + *undef;
      }
      // BUG-088: a genuine NameError on a single call-syntax token `name(args)`
      // (an unsupported extended macro function) surfaces honestly rather than
      // being echoed as literal text. Narrowed to a lone call-syntax token so
      // ordinary literal display (`foo bar`, bare words) stays byte-identical.
      // An evaluator error that names Stata's exact code (`r(NNN): ...`) is
      // claimed in every shape below: prose never raises one. So is scalar() of
      // a scalar that does not exist, which is r(111) "NAME not found" in
      // StataBE (w2ac_expr_valueref scalar_not_neg: the failed `scalar s = !-1`
      // leaves s unset), not the evaluator's NameError text at rc 1.
      const bool exactCode = arithRaised.rfind("r(", 0) == 0 ||
                             arithRaised.rfind("NameError: name 'scalar' ", 0) == 0;
      // FU-418: a CALL to a function this evaluator does not know is Stata's
      // r(133) "unknown function NAME()" -- the code `local x = f(1)` already
      // answers through the same classifier -- in every shape, never the
      // evaluator's raw `NameError: name 'f' is not defined` at rc 1 (`display
      // "x" nosuchfn_x(1)`; also a real Stata function this build lacks, such as
      // reldif(), until it is implemented). A bare-name NameError is
      // not a call and keeps its paths below. A `c(...)` left unresolved is no
      // call either: the classifier words it as StataBE does, r(133) `c(name)
      // undefined`, or refuses a real c() name this build does not serve
      // (BUG-1330; it was the evaluator's NameError at rc 1). Nor is a display
      // DIRECTIVE (`_col(10)`, `_dup(3)`, ...): that is display's language, not
      // a function; its gap (FU-984) keeps bin/main's path, as the directive
      // guard below does.
      static const std::regex reDirectiveName(
          R"(^NameError: name '_(n|newline|continue|asis|request|skip|col|column|dup|char)' )");
      if (!displayError && !exactCode && arithRaised.rfind("NameError", 0) == 0 &&
          !std::regex_search(arithRaised, reDirectiveName)) {
        eval::MacroFnError me;
        if (eval::classifyFailedExpr(joined, arithRaised, &me) && me.rc == "133")
          displayError = "r(133): " + me.message;
      }
      if (!arithRaised.empty() && !displayError && !exactCode) {
        static const std::regex reCallSyntax(
            std::string(R"(^\s*)") + eval::kRxName + R"(\s*\(.*\)\s*$)");
        // A display directive and a c()/r()/e()/s() reference are no function
        // calls: they keep the evaluator's text (BUG-088) rather than be called
        // an unknown function.
        static const std::regex reClassRef(R"(^\s*[cres]\s*\(.*)");
        if (std::regex_match(sourceJoined, reCallSyntax))
          displayError = isDisplayDirective(sourceJoined) || std::regex_match(sourceJoined, reClassRef)
                             ? arithRaised
                             : namedRefusal(sourceJoined, arithRaised);
      }
      // BUG-293(b): an unevaluable expression stops being echoed back as its
      // own source text at rc 0. Reuses the BUG-246 classifier, so display and
      // `local NAME = <expr>` answer ONE mapping — including its two
      // deliberately-unclaimed shapes (string-literal RHS, bare-identifier
      // NameError = the dataset-variable read), which keep the legacy echo.
      //
      // Gated on there being exactly ONE unquoted segment with no quoted parts
      // beside it, and on the segment not being one of display's own
      // DIRECTIVES (`_n`, `_col(#)`, `_dup(#)`, …) — those are display's
      // language, not expressions, and are a named gap rather than an error.
      // `display "text" foo` and multi-token prose keep echoing: this claims
      // the shape the oracle measured (`display 1%2` -> r(198)), not every
      // line that ever reached this fallback.
      //
      // `joined == origJoined` is the load-bearing half, found by the selftest:
      // when resolveToken/substituteCRefs DID resolve the token, what reaches
      // evalArith is the resolved VALUE, and a value that is not arithmetic
      // (`display c(current_date)` -> `12 Aug 2026`) must print, not error.
      // Only a token that survived resolution untouched is a candidate.
      //
      // exactCode (above) is claimed in every shape: `display ! -1` splits into
      // `!` and `-1` here, and `display "x|" (!-1>0)` has a part before it; both
      // echoed the source at rc 0 where StataBE gives r(198) (w2ac_expr_valueref
      // disp_not_neg_sp, w2ac_expr_relational disp_not_gt).
      //
      // FU-141 / FU-855 / FU-201: the same claim for a lone unresolved item
      // under a pending %fmt (`display %21x d13[1]*3` printed `d13[1]*3`) and for
      // one with parts before it when the item is an expression by grammar
      // (isExpressionItem: `display "a|" (1+) "|"` printed `a|(1+)|`, `display
      // "d10|" 1/0` printed `d10|1/0`). The rule that keeps the prose echo: a
      // bare word, or several tokens that are not all groups, is text; a
      // parenthesised group, a call, or text with an operator or subscript is
      // an expression, and an expression that fails is an error. A group the
      // classifier leaves unclaimed (`(foo)`, an unbound bare name) is refused
      // too (namedRefusal), since a parenthesis is never prose.
      const bool loneUnresolved = resolved.size() == 1 && sourceJoined == origJoined;
      if (!displayError &&
          (exactCode || (loneUnresolved && (parts.empty() || pendingFmt.has_value() ||
                                            isExpressionItem(sourceJoined))))) {
        if (!isDisplayDirective(sourceJoined)) {
          eval::MacroFnError me;
          if (!exactCode && !unknownSub.empty()) {
            // `missing(nosuchvar[1])`: the name, not the call, is what failed.
            displayError = "r(111): " + unknownSub + " not found";
          } else if (eval::classifyFailedExpr(sourceJoined, arithRaised, &me) && me.failed()) {
            // A shape this gate newly claims (parts before it, or a pending
            // %fmt) says what StataBE says, `invalid syntax`, and does not
            // quote the item back (w2ac_expr_relational.log:2602-2606
            // disp_not_gt); the lone item keeps the classifier's wording.
            const bool newShape = !parts.empty() || pendingFmt.has_value();
            displayError = "r(" + me.rc + "): " +
                           (newShape && !exactCode && me.rc == "198" ? std::string("invalid syntax")
                                                                     : me.message);
          } else if (loneUnresolved && (isGroupItem(sourceJoined) || pendingFmt.has_value())) {
            // Under a pending %fmt a bare item is an expression too, never
            // prose: `display %9.2f foo` is StataBE's `foo not found` r(111),
            // after the parts before it (`a|foo not found`, measured 19.5).
            displayError = namedRefusal(sourceJoined, arithRaised);
          }
        }
      }
      // name-binding-display-literals: a source token that resolved to nothing
      // and holds a name that binds to nothing is Stata's r(111), never its own
      // text at rc 0 (unboundNameRefusal). A token that resolved to a value
      // (`c(current_date)`, a string e()) is that value and keeps printing.
      for (size_t t = 0; !displayError && t < resolved.size(); ++t) {
        if (resolved[t] != srcTokens[t] || isDisplayDirective(srcTokens[t])) continue;
        if (auto refusal = unboundNameRefusal(srcTokens[t], session)) displayError = *refusal;
      }
      // FU-141: a scalar the macro layer could not evaluate when it was set
      // holds its source text (`scalar b_04 = xd[_N-1]` stores `xd[_N-1]`), and
      // a pending numeric %fmt printed that text at rc 0 where StataBE prints
      // the number (w2i_varsub b_04; w2e_float_default fn_expr_reads_double
      // `%21x se_d`). The store keeps no type, and a string scalar under a
      // numeric format prints its text in StataBE (`scalar ss = "abc"`, `display
      // %9.2f ss` -> `abc`, measured 19.5), so only stored text that is an
      // expression by grammar (isExpressionItem) is refused, and never under a
      // string format. Until the assignment evaluates it
      // (varsub-assign-from-variable-bug710) this is a named gap, not the number.
      // The cost is one-sided and loud: a string scalar that holds such text
      // (`scalar sx = "xd[_N-1]"`, `display %21x sx`) is refused where StataBE
      // prints the text.
      if (!displayError && pendingFmt.has_value() && resolved.size() == 1 &&
          !std::regex_match(*pendingFmt, reStringFmtSpec()) && isExpressionItem(resolved[0])) {
        const std::string* stored = session.eval.scalars.find(srcTokens[0]);
        if (stored != nullptr && *stored == resolved[0])
          displayError = "r(198): scalar " + srcTokens[0] + " holds the unevaluated expression " +
                         resolved[0] + "; PIET cannot format it as a number";
      }
      if (pendingFmt.has_value() && resolved.size() == 1) {
        std::string fmtErr;
        parts.push_back(applyStataFmtLocal(*pendingFmt, resolved[0], &fmtErr));
        if (!fmtErr.empty() && !displayError) displayError = fmtErr;
        pendingFmt.reset();
      } else {
        if (pendingFmt.has_value()) {
          parts.push_back(*pendingFmt);
          pendingFmt.reset();
        }
        // Verbatim: coefficient and e() values print at %.9g (verbatimCoefText).
        std::string verbatim;
        bool legacy = false;
        for (size_t i = 0; i < resolved.size(); ++i) {
          const std::string t = verbatimCoefText(srcTokens[i], resolved[i]);
          legacy = legacy || t != resolved[i];
          if (i > 0) verbatim += " ";
          verbatim += t;
        }
        parts.push_back(legacy ? substituteCRefs(verbatim, session) : sourceJoined);
      }
    }
  };
  auto flushUnquoted = [&]() {
    // BUG-1333 / FU-142: each expression of the segment (splitExpressions), and
    // each of several adjacent groups inside one (splitAdjacentGroups), is
    // evaluated one by one, exactly as a lone item is. A pending %fmt formats
    // the first; the first that fails stops the line with the items before it
    // printed.
    std::vector<std::vector<std::string>> items;
    std::optional<std::string> juxtaposed;  // stops the line after `items`
    for (auto& expr : splitExpressions(unquotedTokens)) {
      if ((juxtaposed = juxtaposedNotError(expr))) break;
      if (auto split = splitAdjacentGroups(expr)) {
        for (auto& item : *split) items.push_back({std::move(item)});
      } else {
        items.push_back(std::move(expr));
      }
    }
    unquotedTokens.clear();
    for (const auto& item : items) {
      unquotedTokens = item;
      const size_t before = parts.size();
      const bool hadError = displayError.has_value();
      flushUnquotedPiece();
      if (!hadError && displayError.has_value()) {
        partsBeforeError = before;
        return;
      }
    }
    if (juxtaposed && !displayError) {
      if (pendingFmt.has_value()) pendingFmt.reset();
      displayError = *juxtaposed;
      partsBeforeError = parts.size();
    }
  };

  // name-binding-display-literals: display's own DIRECTIVES ([P] display:
  // `as style`, `in smcl`, `_asis`, `_skip(#)`, `_column(#)`, `_newline[(#)]`,
  // `_continue`, `_dup(#)`, `_char(#)`, `,` and `,,`), each in the spelling
  // Stata accepts (`_n`, `_col`, `_c`). They used to reach the expression path
  // as words: `as text`, `in red` and `_continue` printed as text, `_n` read as
  // the observation number 1, `_col(30)`/`_skip(3)` were refused at rc 1
  // (w2av_name_binding bd_dy_astext, bd_dy_inred, bd_dy_cont, bd_dy_nl,
  // bd_dy_col, bd_dy_skip). A directive ends the expression before it.
  size_t i = 0;  // the scan position in `s` (takeStyleWord reads past it)
  std::optional<long long> dupCount;  // `_dup(#)`: repeat the next directive
  size_t dupMark = 0;
  bool continueLine = false;  // `_continue`: the display block says so (doexec joins)
  auto applyDup = [&]() {
    if (!dupCount.has_value() || parts.size() <= dupMark) return;
    const std::vector<std::string> unit(parts.begin() + static_cast<std::ptrdiff_t>(dupMark),
                                        parts.end());
    parts.resize(dupMark);
    for (long long r = 0; r < *dupCount; ++r) parts.insert(parts.end(), unit.begin(), unit.end());
    dupCount.reset();
  };
  auto flushDirective = [&]() {
    flushUnquoted();
    applyDup();
  };
  // The output column the next character lands in (0-based), for `_column(#)`.
  auto currentColumn = [&]() {
    size_t col = 0;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
      const size_t nl = it->rfind('\n');
      for (size_t k = nl == std::string::npos ? 0 : nl + 1; k < it->size(); ++k)
        if ((static_cast<unsigned char>((*it)[k]) & 0xC0) != 0x80) ++col;
      if (nl != std::string::npos) break;
    }
    return col;
  };
  auto directiveArg = [&](const std::string& inner) -> std::optional<long long> {
    std::string raised;
    bool isStr = false;
    auto r = eval::evalArith(strip(inner), &session.eval, &raised, &isStr);
    if (!r.has_value() || isStr) return std::nullopt;
    char* end = nullptr;
    const double v = std::strtod(r->c_str(), &end);
    if (r->empty() || end == nullptr || *end != '\0' || !std::isfinite(v) || std::fabs(v) > 1e6)
      return std::nullopt;
    return static_cast<long long>(v);
  };
  // The style word after `as` / `in`, consumed from `s` at `i` when it is one.
  auto takeStyleWord = [&](bool afterAs) {
    size_t a = i;
    while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    size_t b = a;
    while (b < s.size() && std::isalpha(static_cast<unsigned char>(s[b]))) ++b;
    if (b == a || (b < s.size() && !std::isspace(static_cast<unsigned char>(s[b])) &&
                   s[b] != '"' && s[b] != '`' && s[b] != '_' && s[b] != '%' && s[b] != ','))
      return false;
    const std::string w = s.substr(a, b - a);
    const auto abbrevOf = [&w](const std::string& full, size_t minLen) {
      return w.size() >= minLen && w.size() <= full.size() && full.compare(0, w.size(), w) == 0;
    };
    const bool ok = afterAs ? (w == "text" || w == "txt" || abbrevOf("result", 3) ||
                               abbrevOf("error", 3) || abbrevOf("input", 3))
                            : true;  // `in smcl`, and the old `in red`/`in gr`... colours
    if (ok) i = b;
    return ok;
  };
  // BUG-1345: every `%` word is a format to StataBE 19.5, and one it does not
  // take (isStataDisplayFormat) is r(120) `invalid %format` after the parts
  // already printed. `%10S`, `%-~10s`, `%10ss`, a bare `%-`, `%9.2F` and `%.2f`
  // printed their item at rc 0, `%abc` was r(111) and `%-~10s 3` rc 1. The
  // legacy `%d` family keeps its old paths: `%d` reads as a name and `%-d` as
  // a format applyStataFmtLocal refuses (BUG-185).
  auto isFmtToken = [](const std::string& token) {
    return !token.empty() && token[0] == '%' && token.rfind("%d", 0) != 0;
  };
  auto refuseFormat = [&]() {
    flushDirective();
    if (displayError.has_value()) return;
    partsBeforeError = parts.size();
    displayError = "r(120): invalid %format";
  };
  // A comma at depth 0 outside quotes (npos when none).
  auto topLevelComma = [](const std::string& token) {
    int depth = 0;
    size_t k = 0;
    while (k < token.size()) {
      const size_t q = skipQuotedSpan(token, k);
      if (q != k) {
        k = q;
        continue;
      }
      if (token[k] == '(' || token[k] == '[') ++depth;
      if (token[k] == ')' || token[k] == ']') --depth;
      if (token[k] == ',' && depth == 0) return k;
      ++k;
    }
    return std::string::npos;
  };
  // A directive token: true when consumed (its output is in `parts`).
  auto takeDirective = [&](const std::string& token) {
    static const std::regex reDir(R"(^(_[A-Za-z]+)(?:\(([^()]*)\))?$)");
    std::smatch dm;
    if (!std::regex_match(token, dm, reDir)) return false;
    const std::string name = dm[1].str();
    const bool hasArg = dm[2].matched;
    const auto abbrevOf = [&name](const std::string& full, size_t minLen) {
      return name.size() >= minLen && name.size() <= full.size() &&
             full.compare(0, name.size(), name) == 0;
    };
    const bool known = abbrevOf("_newline", 2) || (abbrevOf("_column", 4) && hasArg) ||
                       (name == "_char" && hasArg) || (abbrevOf("_continue", 2) && !hasArg) ||
                       (abbrevOf("_skip", 2) && hasArg) || (abbrevOf("_dup", 2) && hasArg) ||
                       (abbrevOf("_asis", 2) && !hasArg);
    if (!known) return false;  // `_request(...)` and any other name keep their paths
    std::optional<long long> arg;
    if (hasArg) {
      arg = directiveArg(dm[2].str());
      if (!arg.has_value()) return false;
    }
    if (abbrevOf("_newline", 2)) {
      flushDirective();
      parts.push_back(std::string(static_cast<size_t>(std::max<long long>(hasArg ? *arg : 1, 0)), '\n'));
      return true;
    }
    if (abbrevOf("_column", 4) && hasArg) {
      flushDirective();
      const size_t col = currentColumn();
      if (*arg > 1 && static_cast<size_t>(*arg - 1) > col)
        parts.push_back(std::string(static_cast<size_t>(*arg - 1) - col, ' '));
      return true;
    }
    if (name == "_char" && hasArg) {
      flushDirective();
      if (*arg > 0 && *arg < 128) parts.push_back(std::string(1, static_cast<char>(*arg)));
      applyDup();
      return true;
    }
    if (abbrevOf("_continue", 2) && !hasArg) {
      flushDirective();
      continueLine = true;
      return true;
    }
    if (abbrevOf("_skip", 2) && hasArg) {
      flushDirective();
      parts.push_back(std::string(static_cast<size_t>(std::max<long long>(*arg, 0)), ' '));
      return true;
    }
    if (abbrevOf("_dup", 2) && hasArg) {
      flushDirective();
      dupCount = std::max<long long>(*arg, 0);
      dupMark = parts.size();
      return true;
    }
    if (abbrevOf("_asis", 2) && !hasArg) {
      flushDirective();
      return true;
    }
    return false;
  };
  std::function<void(const std::string&)> takeToken = [&](const std::string& token) {
    if (token.empty() || displayError.has_value()) return;
    if (isFmtToken(token)) {
      if (token.rfind("%-d", 0) != 0 && !isStataDisplayFormat(token)) {
        refuseFormat();
        return;
      }
      flushDirective();
      pendingFmt = token;
      return;
    }
    // `,` displays one blank between two directives, `,,` none.
    const size_t comma = topLevelComma(token);
    if (comma != std::string::npos) {
      takeToken(token.substr(0, comma));
      if (displayError.has_value()) return;
      size_t k = comma;
      while (k < token.size() && token[k] == ',') ++k;
      flushDirective();
      if (k - comma == 1) parts.push_back(" ");
      takeToken(token.substr(k));
      return;
    }
    if ((token == "as" && takeStyleWord(true)) || (token == "in" && takeStyleWord(false))) {
      flushDirective();
      return;
    }
    if (takeDirective(token)) return;
    unquotedTokens.push_back(token);
  };

  // BUG-1345: a quoted literal is the item a pending format applies to, and it
  // uses it up: `%-20s "abc"` fills a 20-wide field, while a numeric format
  // leaves the text as it is (`%9.2f "abc" 3.14159` prints abc3.14159, StataBE
  // 19.5). The format used to stay pending and land on the next item, or vanish.
  auto formatLiteral = [&](std::string text) {
    if (!pendingFmt.has_value()) return text;
    if (std::regex_match(*pendingFmt, reStringFmtSpec())) text = padStringField(*pendingFmt, text);
    pendingFmt.reset();
    return text;
  };

  // A quoted literal right after a binary operator is that operator's operand,
  // not an item of its own: `display s[1+1] + "z"` is dez and `display c(os) ==
  // "MacOSX"` is 1 (StataBE 19.5). The expression before it was cut off at the
  // quote and printed as its own text (`"de" +z`, `"MacOSX" ==MacOSX`). An
  // operator with no operand before it (`display "a" + "b"`) keeps its path.
  auto takesQuotedOperand = [&]() {
    std::string pending;
    for (const auto& t : unquotedTokens) pending += t;
    const size_t opEnd = pending.find_last_not_of(" \t");
    if (opEnd == std::string::npos || std::strchr("+-*/^<>=&|", pending[opEnd]) == nullptr)
      return false;
    return pending.find_last_not_of("+-*/^<>=&|!~ \t", opEnd) != std::string::npos;
  };

  // Evaluation stops at the first failing piece, as Stata's does: a later
  // piece's runiform() draw or _request() prompt never runs.
  while (i < s.size() && !displayError.has_value()) {
    // BUG-1419: a string literal right after a binary operator is that
    // operator's operand, not a new display item: `display s1 + "x"` (string
    // scalar s1 "-3") prints -3x, and `t + "x" + t`, `e(cmd) + "z"`, `s[1] +
    // "z"`, `s[1] == "ab"`, `2 * "ab"` are one expression each (StataBE 19.5,
    // lane expr-sem x1b/a26.do). It printed `"-3" +x`. A pending format applies
    // to the whole expression (`display %9s s1 + "x"` is `      -3x`), and a
    // compound-quoted literal is an operand the same way (`display s1 +
    // `"q"a"'` is -3q"a). A literal that STARTS an item stays a display string,
    // as in Stata (`display "a" -5` is a-5), and so does one after an operator
    // with nothing on its left: `display "a" + "b"` is Stata's refusal (`+"b"
    // invalid name`, r(198)), never `a+ "b"`.
    if ((s[i] == '"' || (s[i] == '`' && i + 1 < s.size() && s[i + 1] == '"')) &&
        !unquotedTokens.empty()) {
      const std::string& prev = unquotedTokens.back();
      size_t j = std::string::npos;  // one past the literal
      if (s[i] == '"') {
        const size_t close = s.find('"', i + 1);
        if (close != std::string::npos) j = close + 1;
      } else {
        const size_t q = skipQuotedSpan(s, i);
        if (q >= i + 4 && s.compare(q - 2, 2, "\"'") == 0) j = q;
      }
      const bool leftOperand =
          unquotedTokens.size() > 1 || prev.find_first_not_of("+-*/^=<>&|!~") != std::string::npos;
      if (j != std::string::npos && !prev.empty() && leftOperand &&
          std::string("+-*/^=<>&|").find(prev.back()) != std::string::npos) {
        unquotedTokens.push_back(s.substr(i, j - i));
        i = j;
        continue;
      }
    }
    // EM4: a compound-quoted `"..."' piece prints its body verbatim, inner quotes
    // and nested spans included (w2af_macro_text_file.log:955 `<impute must be
    // 'all', "none">`, :761 `<x `"in "q""' y>`).
    if (i + 1 < s.size() && s[i] == '`' && s[i + 1] == '"') {
      const size_t j = skipQuotedSpan(s, i);
      if (j >= i + 4 && s.compare(j - 2, 2, "\"'") == 0 && takesQuotedOperand()) {
        unquotedTokens.push_back(s.substr(i, j - i));
        i = j;
        continue;
      }
      if (j >= i + 4 && s.compare(j - 2, 2, "\"'") == 0) {
        flushDirective();
        if (displayError.has_value()) break;
        parts.push_back(formatLiteral(s.substr(i + 2, j - i - 4)));
        applyDup();
        i = j;
        continue;
      }
      // BUG-1523: an unterminated compound quote runs to the end of the line
      // (StataBE 19.5: `display `"cq` prints `cq` at rc 0). The token scanner
      // below stops at the opening tick without taking a byte, so this piece
      // used to loop forever.
      flushDirective();
      if (displayError.has_value()) break;
      parts.push_back(formatLiteral(s.substr(i + 2)));
      break;
    }
    if (s[i] == '"' && takesQuotedOperand() && s.find('"', i + 1) != std::string::npos) {
      const size_t j = s.find('"', i + 1);
      unquotedTokens.push_back(s.substr(i, j + 1 - i));
      i = j + 1;
      continue;
    }
    if (s[i] == '"') {
      flushDirective();
      if (displayError.has_value()) break;
      size_t j = s.find('"', i + 1);
      if (j == std::string::npos) {
        parts.push_back(formatLiteral(s.substr(i + 1)));
        break;
      }
      parts.push_back(formatLiteral(s.substr(i + 1, j - (i + 1))));
      applyDup();
      i = j + 1;
    } else if (std::isspace(static_cast<unsigned char>(s[i]))) {
      ++i;
    } else {
      // BUG-293(c): inside a PARENTHESIZED argument list a `"` is part of the
      // expression, not a segment delimiter. The scanner used to break on every
      // quote at any depth, so `display string(42.5,"%9.2f")` was shredded into
      // `string(42.5,` + a string part `%9.2f` + `)`, evalArith never saw a
      // callable expression, and display echoed the fragments back joined at rc
      // 0 (`string(42.5,%9.2f)`) where Stata prints `42.50`. Whitespace inside
      // the parens is swallowed for the same reason (`string(42.5, "%9.2f")`).
      // Deliberately one-sided: if the parens never close, the token is re-cut
      // under the ORIGINAL rule, so every unbalanced/degenerate line this
      // function ever handled stays byte-identical.
      // BUG-1352: a subscript's brackets count the same way, so the quoted row
      // name of `delta["d2",1]` is part of the token (StataBE prints 2); the
      // cut at its quote left `delta[` to fail as `delta not found`. They hold
      // their blanks too (BUG-1402): `NoSuchM[1, 1]` is one piece, r(111)
      // `NoSuchM not found` as without the blank, and `A[ 1, 2]` reads the
      // element, where the cut pieces echoed their own text at rc 0.
      size_t j = i;
      {
        int depth = 0;
        bool closed = true;
        while (j < s.size()) {
          const char c = s[j];
          if (c == '(' || c == '[') {
            ++depth;
          } else if (c == ')' || c == ']') {
            if (depth > 0) --depth;
          } else if (c == '`' && j + 1 < s.size() && s[j + 1] == '"') {
            // EM4: a compound literal inside an expression piece is one operand
            // (`strpos(`"a "b" c"', "b")`, w2af :1065 mt_fn_strpos_disp `<4>`).
            if (depth == 0) break;
            const size_t q = skipQuotedSpan(s, j);
            if (q < j + 4 || s.compare(q - 2, 2, "\"'") != 0) {  // unterminated
              closed = false;
              break;
            }
            j = q - 1;
          } else if (c == '"') {
            if (depth == 0) break;
            const size_t close = s.find('"', j + 1);
            if (close == std::string::npos) {  // unterminated — old rule wins
              closed = false;
              break;
            }
            j = close;
          } else if (std::isspace(static_cast<unsigned char>(c))) {
            if (depth == 0) break;
          }
          ++j;
        }
        if (!closed || depth != 0) {
          j = i;
          while (j < s.size() && s[j] != '"' && !std::isspace(static_cast<unsigned char>(s[j]))) {
            ++j;
          }
        }
      }
      std::string token = s.substr(i, j - i);
      i = j;
      // BUG-1345: a format word runs to a blank, so one with a quote right after
      // it (`%9.2f"a"`) is no format either (StataBE r(120)).
      if (isFmtToken(token) && j < s.size() && (s[j] == '"' || s[j] == '`')) {
        refuseFormat();
        break;
      }
      takeToken(token);
    }
  }
  if (!displayError.has_value()) flushDirective();

  // BUG-088: an unsupported extended macro function was hit — fail loudly at the
  // point of failure instead of emitting confidently-wrong literal text.
  if (displayError.has_value()) {
    // BUG-293: an expression-layer error that already NAMES its Stata code
    // ("r(109): type mismatch" — evalArith's BUG-246/249 channel) must carry
    // that code into the block's `reason`, which is the only field
    // eval::coarseRc() reads as exact. Without it `capture noisily display
    // string("abc","%9.0g")` printed the right message and then answered
    // `_rc == 1` where Stata answers 109, so a do-file branching on the code
    // took the wrong branch off an error whose own text named it.
    //
    // Review finding NB1 (2026-09-09): this used to READ the prefix and leave
    // it in the message, so display was the one surface that printed
    // `r(109): type mismatch: ...` where the count/local/gen/if twins print
    // `type mismatch: ...` with the code carried in `reason` (Stata prints the
    // bare sentence). Measured before: `display strlen(e(nosuch))` ->
    // "r(109): type mismatch: `strlen(.)` ...", `display e(cmd)+1` ->
    // "r(109): type mismatch"; rc was 109 in both, so this was cosmetic
    // double-printing, not a wrong answer. The splitter is now the SHARED
    // eval::splitErrCode, so the prefix cannot be consumed two ways -- but its
    // r(111) DEFAULT belongs to the resolver's unprefixed channel, not to
    // display, so it is only consulted when a prefix is really there.
    std::optional<std::string> reason;
    std::string de = *displayError;
    if (de.rfind("r(", 0) == 0) {
      std::string stripped = de;
      const std::string code = eval::splitErrCode(&stripped);
      if (stripped != de) {  // splitErrCode only rewrites when the prefix parsed
        reason = code;
        de = stripped;
      }
    }
    ojson errBlock(eval::errorBlock("engine_error", de, std::nullopt, std::nullopt, std::nullopt,
                                    reason));
    std::string partial;
    for (size_t k = 0; k < partsBeforeError && k < parts.size(); ++k) partial += parts[k];
    if (!partial.empty()) errBlock["partial_text"] = partial;
    return errBlock;
  }

  std::string text;
  for (const auto& p : parts) text += p;

  ojson out;
  out["type"] = "display";
  out["text"] = text;
  // `_continue`: the next display output joins this line (doexec's pushBlock).
  if (continueLine) out["continue"] = true;
  return out;
}

// AER-SCOPE1 bundle E (TABLES) -- see the header for why this reuses display's
// own chain instead of growing a second e() reader.
std::optional<double> evalScalarExpr(const std::string& expr, SessionState& session) {
  auto toNumber = [](const std::string& t) -> std::optional<double> {
    if (t.empty()) return std::nullopt;
    const char* b = t.c_str();
    char* endp = nullptr;
    double v = std::strtod(b, &endp);
    if (endp == b || endp == nullptr || *endp != '\0') return std::nullopt;
    if (!std::isfinite(v)) return std::nullopt;
    return v;
  };

  std::string e = strutil::trim(expr);
  if (e.empty()) return std::nullopt;

  // A Stata missing value is a legitimate resolution, not a number: `.`/`.a`
  // must NOT come back as 0, which is the shape a table cell would print.
  if (e == "." || (e.size() == 2 && e[0] == '.' && std::isalpha(static_cast<unsigned char>(e[1])))) {
    return std::nullopt;
  }

  // 1. Whole-token forms, exactly the ones display resolves before arithmetic.
  //    A resolution failure leaves the token unchanged, which `toNumber` then
  //    rejects -- so an unknown name falls through to step 2 rather than being
  //    reported as a value.
  {
    std::string err;
    std::string tok = resolveToken(e, session, &err);
    if (err.empty()) {
      if (auto v = toNumber(tok)) return v;
    }
  }

  // 2. Real arithmetic. resolveEstScalarRefs is the find-anywhere pass for
  //    _b[]/_se[]/scalar()/e() (never e(b)/e(V), which are matrices) and
  //    substituteCRefs the same for c(); evalArith then evaluates. `raised`
  //    non-empty means the expression named something undefined -- an honest
  //    nullopt, not a zero.
  // e(sample) T2: with an error channel, so a refused e() read (the named
  // r(198) of a model with no sample mask, an unknown _b[] name) is an honest
  // nullopt here rather than text left for evalArith to misread.
  std::string resolveErr;
  std::string joined =
      substituteCRefs(eval::resolveEstScalarRefs(e, session.eval, &resolveErr), session);
  if (!resolveErr.empty()) return std::nullopt;
  std::string raised;
  bool isString = false;
  auto r = eval::evalArith(joined, &session.eval, &raised, &isString);
  if (!r.has_value() || isString || !raised.empty()) return std::nullopt;
  return toNumber(*r);
}

}  // namespace piet
