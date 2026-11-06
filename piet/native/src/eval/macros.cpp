// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// C++ port of piet/backend/piet/router/macros.py + postfile.eval_macro_expr
// + matrix_op.substitute_matrix_refs + the display_hardening est-scalar
// resolvers _eval_arith depends on. The Python sources are the spec — every
// branch below mirrors a specific Python code path, bug-for-bug (including
// single-pass RHS expansion in definitions and first-occurrence string
// replacement during substitution).
#include "piet/eval/macros.h"
#include "estimators/postest_names.h"  // coefFullName: the engine's coefficient-name rule

#include "../dispatch_profile.h"  // WS-D1: per-line regex counter (opt-in, stderr)

#ifndef _WIN32
#include <fnmatch.h>  // WP-W5: POSIX-only; the Windows branch uses globMatchPeriod below
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <regex>
#include <set>
#include <unordered_set>
#include <vector>

#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"
#include "piet/eval/pyeval.h"
#include "piet/eval/pyvalue.h"
#include "piet/strutil.h"
#include "core/ident.h"  // Unicode names: the shared identifier byte tests
#include "core/expr_blanks.h"  // FU-875: the blanks Stata drops inside ( ) and [ ]
#include "data/stata_dates.h"  // the literal pseudo-function scan (liftDateLiterals)
#include "io/win_compat.h"  // cecon::to_native_path (BUG-050 wide open)

namespace piet::eval {

// BUG-1545: `_b[/name]`/`_se[/name]` (`kind` "b"/"se") for an ancillary column
// only the live fit's posted e(b)/e(V) carry (tobit's `/var(e.yc)`, ologit/oprobit's
// `/cut1`, nbreg's `/lnalpha`; estimation_m4.cpp postAncillaryEbM4): Stata answers
// them from e(b) and e(V) (StataBE 19.5, p-ancillary/side.do: `_b[/cut1]`
// -.091028014590738324 and `_se[/cut1]` .45734276246127026 after ologit), and a factor
// level such as proportion's `0bn.d` (`_b[0.d]`). nullopt = no such column. Defined
// beside ebStripeCoef; display_hardening.cpp's resolveBRef declares it alike.
std::optional<double> ancillaryEbCoef(const EvalSession& session, const std::string& name,
                                      const std::string& kind);

namespace {

// ---------------------------------------------------------------------------
// Small Python-string helpers

bool isPySpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

std::string strip(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && isPySpace(s[a])) ++a;
  while (b > a && isPySpace(s[b - 1])) --b;
  return s.substr(a, b - a);
}

// ---------------------------------------------------------------------------
// D1-①: the `local NAME rest` / `global NAME rest` recogniser, as a single
// left-to-right pass instead of a std::regex match.
//
// WHY. WS-D1 profiled dispatch()'s ~3.3 us per-line fixed cost (two prior
// sessions guessed at it, implemented the guess and measured zero). The
// profile named this function: `tryCaptureDefinition` was 67.4% of the whole
// per-line budget by the zone timers and 59.6% by the sampling profiler, and
// 94.4% of ITS subtree was one symbol — std::basic_regex::__search. Two regex
// matches ran on every line that reached step 0a, and `local q 5` — the
// cheapest statement the language has — paid both.
//
// EQUIVALENCE, not approximation. These two functions reproduce
//     reLocalDef()  = ^local\s+([A-Za-z_][A-Za-z0-9_]*|[0-9]+(?![A-Za-z0-9_]))\s*(.*)?$
//                                                                  (icase; BUG-923)
//     reGlobalDef() = ^global\s+([A-Za-z_][A-Za-z0-9_]*)\s*(.*)?$  (icase)
// (each identifier class also admits the bytes \x80-\xff -- Unicode names)
// exactly, including the greedy-then-no-backtracking parse (the trailing
// `(.*)$` can always match, so the greedy first pass is the only pass the
// regex engine ever needs). Three details are load-bearing:
//   * `\s` in ECMAScript is [ \t\n\v\f\r] — isRegexSpace below, NOT isPySpace
//     (which is the same set here, but the two classes are separate laws and
//     are kept separate on purpose).
//   * `.` does NOT match a newline, so the trailing `(.*)$` cannot span one:
//     `local x a\nb` does NOT match the regex, while `local\nx 3` DOES (there
//     the newline is inside the `\s+` run, and \s includes \n). Reproduced
//     exactly below — the scan is newline-agnostic, and the tail is then
//     rejected if it carries one. Only `#delimit ;` can deliver such a line
//     here, and it is not the hot path either way.
//   * the keyword compare is case-insensitive (`LOCAL x 3` is legal Stata).
// selftest.cpp cross-checks this function against the regexes over a corpus of
// forms, so the two cannot drift (the same discipline the plan requires of a
// generated perfect-hash table).
bool isRegexSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}
// Unicode names (lane M): the identifier classes are the ONE shared rule's
// permissive byte tests (src/core/ident.h) -- a byte >= 0x80 is an identifier
// byte, so `local م 1`, `` `م' ``, `scalar سعر = 2` and the mixed-script
// `دخل_2020` are each ONE name (before, the Arabic bytes were word breaks and
// the ASCII tail `_2020` was bound on its own). Every regex twin of a scan
// below carries the same `\x80-\xff` widening, so the selftest cross-checks
// still hold the pairs together.
bool isIdentStart(char c) { return cecon::ident::isIdentStartByte(c); }
bool isIdentChar(char c) { return cecon::ident::isIdentByte(c); }
// Unicode names (lane M): a GLOBAL reference must START with a letter. StataBE
// 19.5 (scratch-r150/uni-m/m2.do): `$ق` expands, but `$€5`, `$٥٠` (Arabic-Indic
// digits) and `$<NBSP>` print literally, and `global ٥ 3` is r(198); a LOCAL
// reference is looser (`` `€' `` expands to "" and `local €x 3` is rc 0), so
// only the `$` forms take this test. Every later character stays permissive --
// `$ق€` reads the global `ق€`, as Stata does. The regexes admit any byte
// >= 0x80 as the start; this is the filter on what they found.
bool globalNameStartOk(const std::string& name) {
  if (name.empty()) return false;
  if (static_cast<unsigned char>(name[0]) < 0x80) return isIdentStart(name[0]);
  char32_t cp = 0;
  return cecon::ident::decodeUtf8(name, 0, &cp) != 0 && cecon::ident::isUnicodeLetter(cp);
}
bool ciStartsWith(const std::string& s, const char* kw, size_t kwLen) {
  if (s.size() < kwLen) return false;
  for (size_t i = 0; i < kwLen; ++i) {
    char c = s[i];
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    if (c != kw[i]) return false;
  }
  return true;
}

// Pre-filter for reMdyn = ^((?:local|global)\s+)(\S+)(.*)$ : its body only
// does anything when the NAME TOKEN (group 2, the maximal non-space run after
// the keyword) carries a '`' or a '$'. This answers that question with one
// scan of that token. Deliberately conservative — when it says yes the regex
// still runs and still decides; it can only skip a match whose body would have
// been a no-op.
bool macroDefNameTokenHasMacroChar(const std::string& s) {
  size_t kwLen;
  if (ciStartsWith(s, "local", 5)) {
    kwLen = 5;
  } else if (ciStartsWith(s, "global", 6)) {
    kwLen = 6;
  } else {
    return false;
  }
  size_t i = kwLen;
  if (i >= s.size() || !isRegexSpace(s[i])) return false;  // the \s+ is mandatory
  while (i < s.size() && isRegexSpace(s[i])) ++i;
  size_t j = i;
  while (j < s.size() && !isRegexSpace(s[j])) ++j;
  if (j == i) return false;  // \S+ needs at least one character
  for (size_t k = i; k < j; ++k) {
    if (s[k] == '`' || s[k] == '$') return true;
  }
  return false;
}

// Returns true iff `s` is `<kw> NAME [rest]`; fills name/rest with what the
// equivalent regex's groups 1 and 2 would hold.
bool matchMacroDef(const std::string& s, const char* kw, size_t kwLen, std::string& name,
                   std::string& rest) {
  if (!ciStartsWith(s, kw, kwLen)) return false;
  size_t i = kwLen;
  if (i >= s.size() || !isRegexSpace(s[i])) return false;  // \s+ needs at least one
  while (i < s.size() && isRegexSpace(s[i])) ++i;
  if (i >= s.size()) return false;
  size_t j = i;
  if (isIdentStart(s[i])) {
    while (j < s.size() && isIdentChar(s[j])) ++j;
    // Unicode names: a global's name starts with a letter (`global ٥ 3` is
    // r(198) in StataBE; `local €x 3` is rc 0) -- see globalNameStartOk.
    if (kwLen == 6 && !globalNameStartOk(s.substr(i, j - i))) return false;
  } else if (kwLen == 5 && std::isdigit(static_cast<unsigned char>(s[i]))) {
    // BUG-923: a positional local (`local 1 "alpha"`, the do-file-level form
    // `args` reads) is a digit run; `local 1x 3` stays a non-match.
    while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j]))) ++j;
    if (j < s.size() && isIdentChar(s[j])) return false;
  } else {
    return false;
  }
  name.assign(s, i, j - i);
  size_t k = j;
  while (k < s.size() && isRegexSpace(s[k])) ++k;
  // `(.*)$` cannot cross a newline — see the note above.
  if (s.find('\n', k) != std::string::npos) return false;
  rest.assign(s, k, s.size() - k);
  return true;
}

// A5-04: forwards to the shared strutil copy (piet/strutil.h) — kept as a
// thin wrapper (not removed) so this file's many `toLower(...)` call sites
// didn't need touching. Pure dedup, no behavior change. (Note: strip()
// above is a distinct, wider-whitespace helper and is left untouched.)
std::string toLower(std::string s) { return strutil::lower(std::move(s)); }

std::vector<std::string> pySplit(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (isPySpace(c)) {
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

// Python str.replace(a, b, 1) — replace FIRST occurrence anywhere.
std::string replaceFirst(const std::string& s, const std::string& from, const std::string& to) {
  if (from.empty()) return s;
  size_t pos = s.find(from);
  if (pos == std::string::npos) return s;
  return s.substr(0, pos) + to + s.substr(pos + from.size());
}

std::string regexEscape(const std::string& s) {
  static const std::string meta = R"(\^$.|?*+()[]{})";
  std::string out;
  for (char c : s) {
    if (meta.find(c) != std::string::npos) out.push_back('\\');
    out.push_back(c);
  }
  return out;
}

// re.sub with a callback (replaces ALL matches).
std::string regexSubFn(const std::regex& re, const std::string& input,
                        const std::function<std::string(const std::smatch&)>& fn) {
  std::string out;
  auto begin = std::sregex_iterator(input.begin(), input.end(), re);
  auto end = std::sregex_iterator();
  size_t last = 0;
  for (auto it = begin; it != end; ++it) {
    const std::smatch& m = *it;
    out.append(input, last, static_cast<size_t>(m.position(0)) - last);
    out.append(fn(m));
    last = static_cast<size_t>(m.position(0)) + static_cast<size_t>(m.length(0));
  }
  out.append(input, last, input.size() - last);
  return out;
}

// Snapshot of finditer results: (full match text, group1 text).
std::vector<std::pair<std::string, std::string>> findAll(const std::regex& re,
                                                          const std::string& s) {
  std::vector<std::pair<std::string, std::string>> out;
  auto begin = std::sregex_iterator(s.begin(), s.end(), re);
  for (auto it = begin; it != std::sregex_iterator(); ++it) {
    out.emplace_back(it->str(0), it->size() > 1 ? it->str(1) : std::string());
  }
  return out;
}

std::vector<std::pair<std::string, std::string>> findAllGlobalRefs(const std::regex& re,
                                                                   const std::string& s) {
  std::vector<std::pair<std::string, std::string>> out = findAll(re, s);
  out.erase(std::remove_if(out.begin(), out.end(),
                           [](const std::pair<std::string, std::string>& r) {
                             return !globalNameStartOk(r.second);
                           }),
            out.end());
  return out;
}
bool hasGlobalRef(const std::regex& re, const std::string& s) {
  for (auto it = std::sregex_iterator(s.begin(), s.end(), re); it != std::sregex_iterator(); ++it)
    if (globalNameStartOk(it->str(1))) return true;
  return false;
}

// Python repr(str) — used in the non-convergence error message ({line!r}).
std::string pyRepr(const std::string& s) {
  bool hasSingle = s.find('\'') != std::string::npos;
  bool hasDouble = s.find('"') != std::string::npos;
  char quote = (hasSingle && !hasDouble) ? '"' : '\'';
  std::string out(1, quote);
  for (unsigned char c : s) {
    if (c == '\\') {
      out += "\\\\";
    } else if (c == static_cast<unsigned char>(quote)) {
      out += "\\";
      out += static_cast<char>(c);
    } else if (c == '\n') {
      out += "\\n";
    } else if (c == '\r') {
      out += "\\r";
    } else if (c == '\t') {
      out += "\\t";
    } else if (c < 0x20 || c == 0x7f) {
      char buf[8];
      std::snprintf(buf, sizeof(buf), "\\x%02x", c);
      out += buf;
    } else {
      // ASCII printable and UTF-8 continuation/lead bytes: Python 3 repr
      // keeps printable non-ASCII literal (corpus avoids non-printable
      // unicode).
      out += static_cast<char>(c);
    }
  }
  out += quote;
  return out;
}

// ---------------------------------------------------------------------------
// Patterns (macros.py) — names mirror the Python constants.

const std::regex& reScalarDef() {
  static const std::regex r(R"(^scalar\s+(?:define\s+)?([A-Za-z_\x80-\xff][A-Za-z0-9_\x80-\xff]*)\s*=\s*(.+)$)",
                             std::regex::icase);
  return r;
}
const std::regex& reLocalDef() {
  // BUG-923: or a positional digit run (`local 1 "alpha"`); see matchMacroDef.
  static const std::regex r(R"(^local\s+([A-Za-z_\x80-\xff][A-Za-z0-9_\x80-\xff]*|[0-9]+(?![A-Za-z0-9_\x80-\xff]))\s*(.*)?$)",
                             std::regex::icase);
  return r;
}
const std::regex& reGlobalDef() {
  static const std::regex r(R"(^global\s+([A-Za-z_\x80-\xff][A-Za-z0-9_\x80-\xff]*)\s*(.*)?$)", std::regex::icase);
  return r;
}
const std::regex& reLocalRef() {
  static const std::regex r(R"(`([A-Za-z_\x80-\xff][A-Za-z0-9_\x80-\xff]*|[0-9]+)')");
  return r;
}
const std::regex& reGlobalRefBrace() {
  static const std::regex r(R"(\$\{([A-Za-z_\x80-\xff][A-Za-z0-9_\x80-\xff]*)\})");
  return r;
}
const std::regex& reGlobalRefBare() {
  static const std::regex r(R"(\$([A-Za-z_\x80-\xff][A-Za-z0-9_\x80-\xff]*))");
  return r;
}
const std::regex& reRRef() {
  static const std::regex r(R"(\br\(([A-Za-z_\x80-\xff][A-Za-z0-9_\x80-\xff]*)\))");
  return r;
}
const std::regex& reRRefDelim() {
  static const std::regex r(R"(`r\(([A-Za-z_\x80-\xff][A-Za-z0-9_\x80-\xff]*)\)')");
  return r;
}
const std::regex& reERef() {
  // RISK-A: negative-lookahead keeps e(b)/e(V) for the matrix layer.
  static const std::regex r(R"(\be\((?!b\)|V\))([A-Za-z_\x80-\xff][A-Za-z0-9_\x80-\xff]*)\))");
  return r;
}
// BUG-409: `e(name)' — the DELIMITED (macro-expansion) form, the twin of
// reRRefDelim. Stata's e() holds macros as well as scalars and `estadd local
// controls "Yes"` is the standard way a paper puts a "Controls: Yes" row into a
// table; without this arm the backtick survived expansion and tripped the
// generic "does not expand macros" tripwire, so the ONE spelling every estout
// example uses was a hard error. Unlike reERef this has no b/V exclusion: a
// delimited reference is never the matrix layer's.
const std::regex& reERefDelim() {
  static const std::regex r(R"(`e\(([A-Za-z_\x80-\xff][A-Za-z0-9_\x80-\xff]*)\)')");
  return r;
}
// P4: `s(name)' — sreturn-local read (sclass programs). Delimited form
// ONLY: Stata's s() is a string-only namespace here, and the P4 task scope
// is the minimal subset the answerbook needs — no bare `s(name)` form, no
// numeric variant (unlike r(), which has both — see reRRef/reRRefDelim
// just below what this sits next to).
const std::regex& reSRefDelim() {
  static const std::regex r(R"(`s\(([A-Za-z_\x80-\xff][A-Za-z0-9_\x80-\xff]*)\)')");
  return r;
}
// BUG-1006: `c(name)' -- the delimited c-class reference. Like `e(name)' it is
// macro expansion (Stata expands it inside double quotes); the value comes from
// EvalSession::cValue.
const std::regex& reCRefDelim() {
  static const std::regex r(R"(`c\(([A-Za-z_\x80-\xff][A-Za-z0-9_\x80-\xff]*)\)')");
  return r;
}
// MUST stay byte-identical to program.cpp's kSClassKeyPrefix — see that
// file's top-of-file note for the full rationale (no shared header exists
// for this constant; sreturn-local values live in session.rStrings, the
// SAME store `return local`/r() uses, under this reserved key prefix
// rather than a new session store).
const char* const kSClassKeyPrefix = "\x01s:";
const std::regex& reSafeArith() {
  static const std::regex r(R"(^[0-9\s\+\-\*/\(\)\.\^eE]+$)");
  return r;
}
// BUG-206: `%` is in the EXTENDED alphabet, and only the extended one.
//
// Every Stata display format begins with `%`, so while this class refused the
// character the entire two-argument `string(value, "fmt")` form was dead — and
// with it every OTHER expression whose text happens to contain a percent sign:
// measured pre-fix, `strpos("50%","%")`, `subinstr("a%b","%","pct",1)`,
// `upper("x%y")` and `length("%9.0g")` were all refused before evaluation and
// echoed as their own literal source text. The defect was never about `string()`;
// `string()` is just where it was noticed.
//
// BUG-206's entry warned that admitting `%` would also admit Python's modulo
// operator, and that the one-line widening therefore needed a probe rather than
// an assertion. The probe says the hazard does not exist in THIS engine:
// pyeval.cpp's token set (`enum class Tok`) has no modulo token at all, and its
// lexer's `switch` ends in `default: fail("unexpected character ...")`. So a `%`
// outside a string literal reaches the evaluator, raises SyntaxError, and
// evalArith returns the SAME nullopt the whitelist used to return — identical
// observable behaviour, not new semantics. Corroborated from two more
// directions: a sweep of `tests/**/*.do` + `waves/**/*.do` finds zero uses of
// `%` as an operator (two hits, both inside comment text — "95% CI"), and real
// Stata answers `1%2` and `5 % 3` with r(198), so there is no Stata modulo
// semantics to preserve in the first place.
//
// Deliberately NOT added to reSafeArith()/basic. That is evalArith's FAST path
// (25.6% of S02's profile) and nothing needs `%` there: a `%` expression is by
// construction a formatting or string expression, which needs the extended
// namespace anyway. Keeping the basic alphabet byte-identical means the hot path
// cannot move at all — no added test, no added branch, no reordering.
const std::regex& reSafeArithExt() {
  // BUG-660 fix round: `&`/`|` added — Stata's logical AND/OR (`display
  // . & 1`, `local n = . & 1`) reach evalPyExprDetailed via pyeval.cpp's own
  // new Tok::Amp/Tok::Pipe now, and both must be admitted here or the
  // whitelist rejects the expression before the parser ever sees it. Must
  // stay in lockstep with SafeArithTables::ext below (selftestSafeArithScan-
  // MatchesRegex cross-checks the two).
  // FU-844: `~` too -- Stata's other spelling of `!` and `!=` (pyeval.cpp lexer).
  // Unicode names (lane M): `\x80-\xff` -- see SafeArithTables::ext.
  static const std::regex r(R"(^[0-9\s\+\-\*/\(\)\.\^eEa-zA-Z_,"'<>=!%&\|~\x80-\xff]+$)");
  return r;
}
const std::regex& rePostfileSafeArith() {
  // Unicode names (lane M): `\x80-\xff`, so a UTF-8 scalar name (`=سعر*2')
  // is admitted as the identifier it is.
  static const std::regex r(R"(^[0-9\s\+\-\*/\(\)\.\^eEa-zA-Z_,\x80-\xff]+$)");
  return r;
}
const std::regex& reArithMacro() {
  static const std::regex r(R"(`=([^']+)')");
  return r;
}

// ---------------------------------------------------------------------------
// EXPRLAYER-2: the two whitelist gates as character scans.
//
// WHY. reSafeArith() and reSafeArithExt() are `^[<set>]+$` — a pure character
// class, no alternation, no backtracking, no groups. std::regex_match still
// builds per-execution state for them, and the EXPRLAYER zone profile measured
// 81.8 ms of S01 and 18.7 ms of S02 inside these two calls alone (70,582 and
// 6,008 executions). A 256-entry table lookup answers the same question.
//
// EQUIVALENCE. `regex_match` requires the WHOLE string to match and `+`
// requires at least one character, so an empty string does NOT match — both
// reproduced below. ECMAScript `\s` is [ \t\n\v\f\r] (isRegexSpace above, the
// same class this file already transcribed for matchMacroDef), and `$` without
// the multiline flag anchors at end-of-string, so a trailing newline is simply
// another `\s` member. selftestSafeArithScanMatchesRegex() cross-checks both
// scans against both regexes over a corpus, so they cannot drift.
struct SafeArithTables {
  bool basic[256] = {false};
  bool ext[256] = {false};
  SafeArithTables() {
    // ^[0-9\s\+\-\*/\(\)\.\^eE]+$
    for (char c : std::string("0123456789 \t\n\v\f\r+-*/().^eE")) {
      basic[static_cast<unsigned char>(c)] = true;
    }
    // ^[0-9\s\+\-\*/\(\)\.\^eEa-zA-Z_,"'<>=!%]+$   (eE are inside a-zA-Z)
    // `%` is BUG-206 — see reSafeArithExt() above for the measurement. It must
    // be added to BOTH this table and that regex or selftestSafeArithScanMatches-
    // Regex reports the drift, which is the whole point of the twin.
    for (int i = 0; i < 256; ++i) ext[i] = basic[i];
    for (char c = 'a'; c <= 'z'; ++c) ext[static_cast<unsigned char>(c)] = true;
    for (char c = 'A'; c <= 'Z'; ++c) ext[static_cast<unsigned char>(c)] = true;
    // BUG-660 fix round: `&`/`|` — see reSafeArithExt()'s comment above.
    for (char c : std::string("_,\"'<>=!%&|~")) ext[static_cast<unsigned char>(c)] = true;  // ~: FU-844
    // Unicode names (lane M): every byte >= 0x80 -- a UTF-8 name such as
    // `ملاحظات[1]` or `سعر` reaches this gate as identifier bytes (a continuation
    // byte never collides with an ASCII operator), so `` `=ملاحظات[1]' `` was
    // refused here before anything evaluated it. `basic` stays ASCII: the fast
    // path is the empty-namespace arithmetic one and has no names at all.
    for (int i = 0x80; i < 256; ++i) ext[i] = true;
  }
};
const SafeArithTables& safeArithTables() {
  static const SafeArithTables t;
  return t;
}
// BUG-501. Everything inside a BALANCED string literal is DATA, and every
// pre-pass in evalArith rewrites OPERATORS and REFERENCES -- so each one was
// mangling literal content, silently. Measured against
// tests/eval/golden/aer_w1/w1b_quoteblind.log and w1b_braceliteral.log:
//
//   strpos("a^b", "b")          the `^` became `**`
//   strpos("A[1,1]", "1")       substituteMatrixRefs substituted the subscript
//   strpos("sc", "c")           resolveEstScalarRefs substituted the scalar
//   strpos("r(mean)", "mean")   the r() resolver RAISED
//   strpos("a{b}c", "b")        isSafeArithExt rejected the whole expression,
//                               so `display` echoed the source text at rc 0
//
// Protected ONCE here rather than by teaching four pre-passes (and their four
// selftest surfaces) about quotes. Each literal is swapped for a placeholder
// built only from characters the whitelists already allow and that NO resolver
// can match: `0L<k>L0` cannot be a Stata name, which must start with a letter or
// an underscore. So the filter keeps its exact character set -- and its regex
// twin keeps holding -- because it never sees literal content at all. Restored
// immediately before the evaluator, whose own lexer handles string literals
// correctly (pyeval.cpp says so at its date-literal case).
//
// ONE-SIDED, like the display scanner's BUG-293(c) fix: an UNTERMINATED quote is
// left exactly where it is, so every degenerate expression this function ever
// judged is judged the same way.
// EM3b: a compound-quoted `"..."' literal (nesting, inner quotes) is ONE literal
// too: `local v = stritrim(`"cmd  a  b"')` (w2af mt_fn_stritrim) reached the
// pre-passes as raw text and failed as r(198).
std::string protectStringLiterals(const std::string& s, std::vector<std::string>* out) {
  out->clear();
  if (s.find('"') == std::string::npos) return s;  // the common case, untouched
  std::string r;
  r.reserve(s.size());
  size_t i = 0;
  while (i < s.size()) {
    if (startsCompound(s, i)) {
      const size_t end = compoundSpanEnd(s, i);
      if (end != std::string::npos) {
        out->push_back(s.substr(i, end - i));
        r += "\"0L" + std::to_string(out->size() - 1) + "L0\"";
        i = end;
        continue;
      }
    }
    if (s[i] == '"') {
      const size_t close = s.find('"', i + 1);
      if (close != std::string::npos) {
        out->push_back(s.substr(i, close - i + 1));  // the quotes travel with it
        r += "\"0L" + std::to_string(out->size() - 1) + "L0\"";
        i = close + 1;
        continue;
      }
    }
    r += s[i++];
  }
  return r;
}

// BUG-446. The literal pseudo-functions take unquoted date TEXT, and a clock
// literal has a ':' in it -- which the extended whitelist below refuses, so
// `display tc(11:00)` echoed its own text at rc 0, `local w = tc(12:00)` was
// r(198), and hh(tc(10:30)) never reached hh() (w2y_fn_datetime fd_tc_edges,
// fd_tc_full, fd_hhmm_edges, fd_minutes_edges). Each call is replaced by its
// value, parenthesised, before any pre-pass sees it -- the value the evaluator's
// own lexer would have lifted (pyeval.cpp's date-literal case), from the same
// cecon:: parser. An impossible literal is Stata's r(198) (fd_tq_bad, fd_tm_bad,
// fd_td_bad): `*bad` names it and the caller raises. Runs after
// protectStringLiterals, so text inside a string literal is never read.
std::string liftDateLiterals(const std::string& s, std::string* bad) {
  if (s.find('(') == std::string::npos) return s;
  std::string out;
  size_t copied = 0;
  cecon::for_each_datetime_literal(
      s, [&](const std::string& name, const std::string& raw, size_t begin, size_t end) {
        cecon::LiteralScale ls;
        cecon::literal_scale_for(name, ls);
        const double v = cecon::stata_datetime_literal(raw, ls);
        if (std::isnan(v)) {
          *bad = name + "(" + raw + ")";
          return false;
        }
        out.append(s, copied, begin - copied);
        out += "(" + pythonFloatRepr(v) + ")";
        copied = end;
        return true;
      });
  if (copied == 0) return s;
  out.append(s, copied, std::string::npos);
  return out;
}

// BUG-507, the convergence guard's half. `hasMacroRefs` reports an r()/e()/s()
// reference through its `X(` patterns, and the splices above now correctly LEAVE
// one that sits inside a string literal -- so the guard would read that literal as
// unexpanded work and report a circular macro definition for
// `display strpos("r(mean)", "mean")`.
//
// Masking the whole literal is wrong here: Stata DOES expand `` `x' `` and `$x`
// inside double quotes, so an unexpanded MACRO reference in a literal is still a
// real failure and has to stay visible. Only the PARENTHESES are blanked, which
// is exactly the discrimination needed -- every class-reference pattern needs one
// and no macro pattern does (`${...}` uses braces).
std::string maskParensInQuotes(const std::string& s) {
  if (s.find('"') == std::string::npos) return s;
  std::string out = s;
  bool inQ = false;
  for (size_t i = 0; i < out.size(); ++i) {
    if (out[i] == '"') {
      inQ = !inQ;
      continue;
    }
    // LBL-DOLLAR: '#', not '_' -- a blanked `$(r)` must not read as the global
    // `$_r_` (w2ao lb_var_dollar: `"... $(r)$"` died "did not converge").
    if (inQ && (out[i] == '(' || out[i] == ')')) out[i] = '#';
  }
  return out;
}

std::string restoreStringLiterals(const std::string& s, const std::vector<std::string>& spans) {
  if (spans.empty()) return s;
  std::string r;
  r.reserve(s.size());
  size_t i = 0;
  while (i < s.size()) {
    if (s[i] == '"' && i + 4 < s.size() && s[i + 1] == '0' && s[i + 2] == 'L') {
      size_t j = i + 3;
      std::string digits;
      while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j]))) digits += s[j++];
      if (!digits.empty() && j + 2 < s.size() && s[j] == 'L' && s[j + 1] == '0' &&
          s[j + 2] == '"') {
        size_t k = std::string::npos;
        try {
          k = static_cast<size_t>(std::stoul(digits));
        } catch (const std::exception&) {
        }
        if (k < spans.size()) {
          r += spans[k];
          i = j + 3;
          continue;
        }
      }
    }
    r += s[i++];
  }
  return r;
}

bool isSafeArith(const std::string& s) {
  if (s.empty()) return false;
  const bool* tbl = safeArithTables().basic;
  for (unsigned char c : s) {
    if (!tbl[c]) return false;
  }
  return true;
}
bool isSafeArithExt(const std::string& s) {
  if (s.empty()) return false;
  const bool* tbl = safeArithTables().ext;
  for (unsigned char c : s) {
    if (!tbl[c]) return false;
  }
  return true;
}

// EXPRLAYER-3: a strict pre-filter for reRRef() = `\br\(...`.
//
// WHY. expandPass guarded its whole r() block with `line.find("r(")`, and the
// literal "r(" occurs inside `scalar(`, `subinstr(`, `strvar(` — any identifier
// ending in r. S02's hot line is
//   scalar bp_s = scalar(bp_s) + sqrt(`i') + log(`i'+1) - exp(-`i'/1000)
// which contains no r() reference at all and paid TWO full sregex_iterator
// scans per evaluation to discover that: 44.1 ms of expandPass's own 44.9 ms in
// the S02 zone profile — 98% of the function. This asks the question the regex
// actually asks: is there an `r` that is (a) at a \b, i.e. at the start or
// preceded by a non-[A-Za-z0-9_] character, and (b) followed by '('.
// Also used for reERef() = `\be\((?!b\)|V\))...` — same shape, and "e(" is even
// more common as a suffix ("the(", "true(", "size(").
bool hasBareLetterParen(const std::string& s, char letter) {
  const char pat[3] = {letter, '(', '\0'};
  for (size_t i = s.find(pat); i != std::string::npos; i = s.find(pat, i + 1)) {
    if (i == 0) return true;
    const unsigned char p = static_cast<unsigned char>(s[i - 1]);
    const bool wordChar = (p >= 'A' && p <= 'Z') || (p >= 'a' && p <= 'z') ||
                          (p >= '0' && p <= '9') || p == '_';
    if (!wordChar) return true;
  }
  return false;
}
// EXPRLAYER-7: `` `NAME' `` detection as a scan, for hasMacroRefs.
//
// WHY. reLocalRef() = `` `([A-Za-z_][A-Za-z0-9_]*|[0-9]+)' `` is the ONE regex
// every ordinary macro line still executed, and hasMacroRefs is called about
// three times per such line (tryCaptureDefinition once, expandMacros once per
// pass). S01's form 1 — 30,000 iterations of `local acc = `acc' + `i'` — paid
// all of them. The grammar is the same one expandPass's splicer already walks
// character by character a few dozen lines below; this is that walk, asking
// only whether a reference exists.
//
// EQUIVALENCE: regex_search finds the leftmost match, retrying one character
// later on failure. So does this. selftestLocalRefScanMatchesRegex holds it to
// reLocalRef() over the corpus.
bool hasLocalMacroRef(const std::string& s) {
  for (size_t t = s.find('`'); t != std::string::npos; t = s.find('`', t + 1)) {
    size_t j = t + 1;
    if (j < s.size() && isIdentStart(s[j])) {
      ++j;
      while (j < s.size() && isIdentChar(s[j])) ++j;
    } else if (j < s.size() && s[j] >= '0' && s[j] <= '9') {
      ++j;
      while (j < s.size() && s[j] >= '0' && s[j] <= '9') ++j;
    } else {
      continue;  // nothing after the backtick can start either alternative
    }
    if (j < s.size() && s[j] == '\'') return true;
  }
  return false;
}

// hasBareRParen / hasBareEParen: defined at namespace scope just below the
// anonymous namespace (BUG-456 made hasBareEParen the dispatcher's row-predicate
// trigger too; declared in eval/macros.h). hasBareLetterParen stays file-local.

// ---------------------------------------------------------------------------
// EXPRLAYER-5: the three remaining hot regexes on the `scalar NAME = expr`
// path, as left-to-right scans. S02's whole body is 3,000 evaluations of
//   scalar bp_s = scalar(bp_s) + sqrt(`i') + log(`i'+1) - exp(-`i'/1000)
// and the zone profile put 6.5 us/line inside resolveEstScalarRefs' scalar()
// substitution and ~9 us/line inside tryCaptureDefinition's two unguarded
// regex_match calls — the two case-insensitive `scalar ...` recognisers.
//
// Each function below is cross-checked against the regex it replaces, case by
// case, by selftestScalarScansMatchRegex() — the same discipline
// selftestMacroDefScanMatchesRegex() already applies to matchMacroDef.

// `\bscalar\(\s*([A-Za-z_]\w*)\s*\)` -> the stored scalar's value, or the
// matched text unchanged when the name is not a defined scalar.
// BUG-1419: a STRING scalar's value goes in as a string literal, as an e()
// string does (resolveERef): `scalar t = scalar(s1) + "y"` with s1 = "-3" is
// -3y in StataBE 19.5 and `gen str5 g = scalar(s1) + "x"` -3x; both read the
// text as the number -3 and failed r(109). A value holding `"` is carried
// compound-quoted; one that cannot be (`"'` inside) is left as the call.
std::string substituteScalarCalls(const std::string& in, const EvalSession& session) {
  std::string out;
  size_t i = 0, last = 0;
  bool any = false;
  while (true) {
    const size_t p = in.find("scalar(", i);
    if (p == std::string::npos) break;
    // The pattern's leading \b: 's' is a word char, so the preceding character
    // must be a non-word one (or the match must start the string). On failure
    // the regex engine retries one character later — so does this scan.
    if (p > 0 && isIdentChar(in[p - 1])) {
      i = p + 1;
      continue;
    }
    size_t q = p + 7;  // one past the '('
    while (q < in.size() && isRegexSpace(in[q])) ++q;
    const size_t nameStart = q;
    if (q < in.size() && isIdentStart(in[q])) {
      ++q;
      while (q < in.size() && isIdentChar(in[q])) ++q;
    }
    if (q == nameStart) {
      i = p + 1;
      continue;
    }
    size_t r = q;
    while (r < in.size() && isRegexSpace(in[r])) ++r;
    if (r >= in.size() || in[r] != ')') {
      i = p + 1;
      continue;
    }
    const size_t end = r + 1;
    const std::string name = in.substr(nameStart, q - nameStart);
    const std::string* v = session.scalars.find(name);
    out.append(in, last, p - last);
    if (v != nullptr && session.isStringScalar(name)) {
      if (v->find('"') == std::string::npos) {
        out.append("\"" + *v + "\"");
      } else if (v->find("\"'") == std::string::npos && v->find("`\"") == std::string::npos) {
        out.append("`\"" + *v + "\"'");
      } else {
        out.append(in, p, end - p);
      }
    } else if (v != nullptr) {
      out.append(valueRefText(*v));  // FU-842: one operand
    } else {
      out.append(in, p, end - p);  // undefined: leave the token verbatim
    }
    last = end;
    i = end;
    any = true;
  }
  if (!any) return in;
  out.append(in, last, in.size() - last);
  return out;
}


// Shared tail of `... \s*(.+)$` with no multiline flag: `.` never matches a
// newline and `$` anchors at end-of-string, so the greedy `\s*` gives back at
// most one character to satisfy `.+`. `wsFrom` is where the optional/mandatory
// whitespace run starts, `minWs` how many characters of it are mandatory.
bool matchGreedyTail(const std::string& s, size_t wsFrom, size_t minWs, std::string& rest) {
  size_t m = wsFrom;
  while (m < s.size() && isRegexSpace(s[m])) ++m;
  if (m - wsFrom < minWs) return false;
  if (m < s.size()) {
    if (s.find('\n', m) != std::string::npos) return false;  // `.` cannot cross a newline
    rest.assign(s, m, s.size() - m);
    return true;
  }
  // Nothing left: `.+` must take back one whitespace character, and only a
  // non-newline one, and only if the run can still satisfy its own minimum.
  if (m == wsFrom || (m - 1) - wsFrom < minWs || s[m - 1] == '\n') return false;
  rest.assign(1, s[m - 1]);
  return true;
}

// `^scalar\s+drop\s+(.+)$` (icase).
bool matchScalarDrop(const std::string& s, std::string& rest) {
  if (!ciStartsWith(s, "scalar", 6)) return false;
  size_t i = 6;
  if (i >= s.size() || !isRegexSpace(s[i])) return false;
  while (i < s.size() && isRegexSpace(s[i])) ++i;
  if (!ciStartsWith(s.substr(i, 4), "drop", 4)) return false;
  i += 4;
  return matchGreedyTail(s, i, 1, rest);
}

// `^scalar\s+(?:define\s+)?([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.+)$` (icase).
bool matchScalarDef(const std::string& s, std::string& name, std::string& rest) {
  if (!ciStartsWith(s, "scalar", 6)) return false;
  size_t i0 = 6;
  if (i0 >= s.size() || !isRegexSpace(s[i0])) return false;
  while (i0 < s.size() && isRegexSpace(s[i0])) ++i0;

  // The optional `define\s+` group is greedy: the engine tries it first and
  // falls back to skipping it, which is how `scalar define = 3` parses with
  // NAME="define". Both arms are tried here, in that order.
  for (int withDefine = 1; withDefine >= 0; --withDefine) {
    size_t i = i0;
    if (withDefine == 1) {
      if (!ciStartsWith(s.substr(i, 6), "define", 6)) continue;
      size_t j = i + 6;
      if (j >= s.size() || !isRegexSpace(s[j])) continue;
      while (j < s.size() && isRegexSpace(s[j])) ++j;
      i = j;
    }
    if (i >= s.size() || !isIdentStart(s[i])) continue;
    size_t j = i;
    ++j;
    while (j < s.size() && isIdentChar(s[j])) ++j;
    size_t k = j;
    while (k < s.size() && isRegexSpace(s[k])) ++k;
    if (k >= s.size() || s[k] != '=') continue;
    std::string tail;
    if (!matchGreedyTail(s, k + 1, 0, tail)) continue;
    name.assign(s, i, j - i);
    rest = tail;
    return true;
  }
  return false;
}

constexpr int kMaxExpansionPasses = 16;

// ---------------------------------------------------------------------------
// display_hardening resolvers (the est-scalar subset _eval_arith needs)

// _E_SCALAR_KEY: MOVED out of this anonymous namespace to piet::eval proper
// (AER-SCOPE1 bundle E, 2026-09-04) so handlers_aer_tables.cpp can freeze the
// same e() scalar names into an eststo snapshot. The body is unchanged and now
// lives just below the anonymous namespace's closing brace; declared in
// eval/macros.h. Exposing it was the alternative to a THIRD copy of the table --
// there are already two (the twin is display_hardening.cpp's std::map version,
// identical in content today and commented as such in both files).

std::optional<double> jsonToFloat(const nlohmann::ordered_json& v) {
  if (v.is_number()) return v.get<double>();
  if (v.is_boolean()) return v.get<bool>() ? 1.0 : 0.0;
  if (v.is_string()) return pythonFloat(v.get<std::string>());
  return std::nullopt;  // null / object / array -> TypeError
}

// A posted e() scalar as expression text: Stata's `.` for a missing one, never
// pythonFloatRepr's "nan" (which the evaluator reads as a variable name).
static std::string postedEText(double v) {
  return std::isnan(v) ? std::string(".") : pythonFloatRepr(v);
}

std::string resolveEScalar(const std::string& name, const EvalSession& session) {
  // BUG-421 (2026-09-06): a handler-POSTED e-scalar lives in `eScalars`, not in
  // the estimation JSON. `pca` posts e(rho)/e(f)/e(trace) there and `estadd
  // scalar` posts whatever the user names; reading only lastModelResult made
  // every one of them answer `.` on a surface that had the value in hand.
  // The model result still wins when both carry the name, so a fitted e(N)
  // cannot be shadowed by a posted one.
  if (const double* posted = session.eScalars.find(name);
      posted != nullptr && !session.lastModelResult) {
    return postedEText(*posted);
  }
  if (!session.lastModelResult) return ".";
  const auto& result = *session.lastModelResult;
  if (result.is_null() || (result.is_object() && result.empty())) {
    if (const double* p2 = session.eScalars.find(name)) return postedEText(*p2);
    return ".";
  }
  std::string key = name;
  for (const auto& [k, v] : eScalarKey()) {
    if (k == name) {
      key = v;
      break;
    }
  }
  if (!result.contains(key) || result.at(key).is_null()) {
    if (const double* p3 = session.eScalars.find(name)) return postedEText(*p3);
    return ".";
  }
  auto f = jsonToFloat(result.at(key));
  // A clustered fit cannot have 0 clusters: the engine's n_clusters 0 means
  // "unclustered", which Stata reports as e(N_clust) missing.
  if (f && name == "N_clust" && *f == 0.0) f.reset();
  if (!f) {
    if (const double* p4 = session.eScalars.find(name)) return postedEText(*p4);
    return ".";
  }
  // BUG-382 (off-pin sweep, 2026-09-01): substitute at FULL precision, exactly
  // as the r-class resolver always has. fmtNumeric is %.9g, so `scalar s =
  // e(ll)` received 9 significant digits and no later format could recover the
  // rest (measured: %20.15e of the scalar disagreed with the block's own value
  // in the 10th digit). Bare `display e(ll)` is unchanged by this: display
  // RE-RENDERS a substituted numeric literal through its own %9.0g default —
  // the r-class precedent proves it, since r() substitutes pythonFloatRepr and
  // bare `display r(mean)` still prints Stata's short form (and 173 freeze
  // fixtures pin exactly that rendering). The entry's original one-line-fix
  // refutation reasoned display would echo the text; the architecture says it
  // re-renders, and the freeze battery is the verifier.
  return pythonFloatRepr(*f);
}

// BUG-158 (P4 wave): Stata names the intercept `_cons`; this engine's no-FE
// coeftable names it `(intercept)` (build_model's design-matrix column name),
// while the FE path already emits `_cons` (feols.cpp's reconstructed reghdfe
// intercept). Both spellings therefore coexist in ONE build, so the alias has
// to be resolved at the consumer. Without it `_b[_cons]` — the spelling almost
// all real Stata code uses — matched nothing and fell through to ".", i.e. a
// silent missing with rc 0.
// NOTE: this resolver is DUPLICATED (macros.cpp for expressions/if-conditions,
// display_hardening.cpp for `display`). Both copies must carry this alias or
// `display _b[_cons]` and `scalar q = _b[_cons]` disagree — the exact
// two-layer split that makes a wrong answer silent. Change them together.
// BUG-456 (2026-09-08): `err` is the r(111) channel the display twin has had
// since BUG-162 (display_hardening.cpp resolveBRef). A coefficient NAME that is
// not in the model is r(111) "[nosuch] not found" on EVERY Stata surface --
// local, scalar, gen, count, assert, if (refute3_capture.log V3-V8) -- and
// returning `.` for it turned a typo into a missing that `assert _b[nosuch]==0`
// then reported as r(9), a wrong-reason error. When `err` is null the historical
// `.` is kept so no caller changes behaviour without opting in.
// eb-order T-P3 (SPEC part 1): the coefficients come from coefSource() -- the
// live e()'s estimation block, the store e(b) is built from and the one the
// display twin reads -- never from lastModelResult while a session is attached
// (after estpost, erepost and `estimates use` that mirror describes an earlier
// e()). A null source -- a live e() without an e(b) (pca, rdrobust, rddensity,
// estpost) or no e() at all -- is Stata's r(111) "_b not allowed when e(b) is
// not present" on every surface, never `.` at rc 0 (w2g_ebmat.log:2155
// e15c_local_bx1, :2169, :2187 after pca; w2v_estpost_ematrix.log
// ep15f_local_bx1 and ep15f_disp_bx1_times2 after `ereturn clear`, ep13_local_bx1
// after estpost). The no-e() `.` the SPEC first kept as a named divergence is
// gone: the capture refuted it (FU-949).
std::string resolveBRef(const std::string& varname, const std::string& kind,
                         const EvalSession& session, std::string* err = nullptr) {
  const nlohmann::ordered_json* source = session.coefSource();
  if (source == nullptr) {
    if (err != nullptr && err->empty())
      *err = kind == "b" ? "_b not allowed when e(b) is not present"
                         : "_se not allowed when e(V) is not present";
    return ".";
  }
  const auto& result = *source;
  // Try the requested spelling first, then its intercept alias.
  std::vector<std::string> tryNames{varname};
  if (!varname.empty() && varname.front() == '/') {
    tryNames.push_back(varname.substr(1) + ":_cons");
    tryNames.push_back("/:" + varname.substr(1));
  }
  if (varname == "_cons") tryNames.push_back("(intercept)");
  else if (varname == "(intercept)") tryNames.push_back("_cons");
  // `_b[yb:x1]` after a depvar-equation fit is the bare row (BUG-1328); the
  // display twin does the same through the same helper.
  if (const size_t colon = varname.find(':'); colon != std::string::npos && colon > 0) {
    const std::string eq = depvarEquationOf(session);
    if (!eq.empty() && varname.compare(0, colon, eq) == 0 && colon == eq.size()) {
      const std::string bare = varname.substr(colon + 1);
      tryNames.push_back(bare);
      if (bare == "_cons") tryNames.push_back("(intercept)");
    }
  }
  if (const std::string o = omittedColumnSpelling(varname); !o.empty()) tryNames.push_back(o);
  if (!result.is_null() && result.contains("coeftable")) {
    // An equation-less name falls back to e(b)'s FIRST equation, tried last
    // (`_b[1._at]` after `margins, dydx(x) at() post` is `_b[x:1._at]`,
    // w2v_margins_corpus F1_dydx_at_post_bname1/2) -- the display twin's
    // addFirstEquationName.
    const auto& ct = result.at("coeftable");
    if (varname.find(':') == std::string::npos && ct.is_array() && !ct.empty() &&
        ct.front().is_object() && ct.front().contains("name") && ct.front().at("name").is_string()) {
      const std::string head = ct.front().at("name").get<std::string>();
      const size_t colon = head.find(':');
      if (colon != std::string::npos && colon > 0) tryNames.push_back(head.substr(0, colon + 1) + varname);
    }
    for (const auto& want : tryNames) {
      for (const auto& row : result.at("coeftable")) {
        if (row.contains("name") && row.at("name") == want) {
          const char* field = (kind == "b") ? "b" : "se";
          if (!row.contains(field) || row.at(field).is_null()) return ".";
          auto f = jsonToFloat(row.at(field));
          if (!f) return ".";
          return pythonFloatRepr(*f);  // BUG-382: _b[]/_se[] carry full precision too
        }
      }
    }
  }
  // A column only the fit's recorded stripe names (a factor's base level, an
  // `o.` spelling): eval::ebStripeCoef, shared with the display twin.
  if (const std::optional<double> v = ebStripeCoef(*source, varname, kind))
    return pythonFloatRepr(*v);
  // An ancillary parameter only the posted e(b)/e(V) carry (BUG-1545).
  if (const std::optional<double> v = ancillaryEbCoef(session, varname, kind))
    return pythonFloatRepr(*v);
  // An abbreviated / reordered spelling of a posted column (coefFullName).
  if (const std::string full = coefFullName(*source, varname); !full.empty())
    return resolveBRef(full, kind, session, err);
  // Name not in the model -- r(111), same text as the display twin (BUG-162):
  // Stata's `[name] not found`, bracketed.
  if (err != nullptr && err->empty()) *err = "[" + varname + "] not found";
  return ".";
}

// BUG-456 (2026-09-08): the bare-form e(name) resolver's STRING arm. Measured
// on StataNow 19.5 BE (internal capture refute3_capture.log, 40/40 macro
// shapes): a bare e-MACRO is a STRING in every expression context -- `ereturn
// local m "5"` then `display e(m)+1` is r(109), `assert e(m)=="5"` rc 0,
// `assert e(m)==5` r(109), `local t = e(m)` keeps the TEXT. [U] 18.9 rule 1.3
// as printed ("examined to determine whether its contents can be interpreted
// as a number") is NOT what the binary does, so the numeric-looking text is
// never turned into a number here. The literal is built exactly like the bare
// r(name) string arm in expandPass (BUG-374): a double-quoted literal with any
// interior `"` escaped, so it reaches evalArith / the row fold as a string
// VALUE and not as text to re-evaluate. MEASURED CORRECTION (2026-09-09, review
// finding 2): the ESCAPE does not survive the evaluator -- `estadd local qq
// "a\"b"` then `local t = e(qq)` is r(198) "invalid syntax in 'e(qq)'", where
// Stata passes `a"b` through at rc 0 (REFUTE-0 §3, T1-T9). It is an honest
// error and it is unreachable today (PIET refuses compound double quotes
// outright -- `estadd local qq `"a"b"'` is an engine_error piet_gap -- so no
// e-string can currently carry a `"`), but the escape is a claim this code
// cannot yet make: named, not fixed. An ABSENT name (and an empty macro,
// which setEString never stores) is numeric missing, never "" -- `assert
// e(nosuch)==.` rc 0, `assert e(nosuch)==""` r(109) (A8/A9/N1) -- which is
// what falling through to resolveEScalar answers. Guard order at the caller is
// matrix -> string -> scalar: after `ereturn local b "x"` the bare e(b) is
// still the coefficient matrix in Stata (W1-W8).
std::string resolveERef(const std::string& name, const EvalSession& session) {
  if (const std::string* s = session.eStrings.find(name); s != nullptr && !s->empty()) {
    std::string lit = "\"";
    for (char ch : *s) {
      if (ch == '"') lit += "\\\"";
      else lit += ch;
    }
    lit += "\"";
    return lit;
  }
  return resolveEScalar(name, session);
}

// BUG-456 follow-up (2026-09-09) — A NUMBER IN A STRING-TYPED ARGUMENT SLOT.
//
// The e()/`_b[]` resolvers above substitute a NUMBER for everything that is not
// an e-string: an absent name becomes `.`, an e-scalar becomes its value. That
// is right for arithmetic and for the row plane's constant compares, and WRONG
// the moment the reference sits inside a STRING function -- `strpos(e(nosuch),
// "reg")` became `strpos(.,"reg")`, which the expression evaluator's pythonStr()
// coercion answered 0 at rc 0, and `strlen(e(N))` answered the decimal width of
// the substituted number. Stata refuses every one of those shapes.
//
// MEASURED on StataNow 19.5 BE (scratch oracle.log, 2026-09-09, sysuse auto +
// regress price mpg): `count if strpos(e(nosuch),"reg")` r(109) · `inlist(
// e(nosuch),"regress")` r(109) · `strlen(e(nosuch))==7` r(109) · `substr(
// e(nosuch),1,3)=="reg"` r(109) · `upper(e(nosuch))=="REG"` r(109) · `local p =
// strpos(e(nosuch),"reg")` r(109) · `gen za = strpos(e(nosuch),"reg")` r(109) ·
// `replace price = 1 if strpos(e(nosuch),"reg")` r(109) · with a PRESENT
// e-scalar the same: `strpos(e(N),"7")` r(109), `inlist(e(N),"74")` r(109),
// `local q = strlen(e(N))` r(109), `display strlen(e(nosuch))` r(109). The
// accepting arm is unchanged: `strpos(e(cmd),"reg")` 74, `inlist(e(cmd),
// "regress")` 74, `substr(e(cmd),1,3)=="reg"` 74, `local c = strpos(e(cmd),
// "gres")` 3 -- an e-STRING is a string, so a string function takes it.
//
// The rule is the ARGUMENT's type, not where it came from: Stata gives r(109)
// for a numeric LITERAL (`strpos(5,"a")`, `strlen(5)`), for a numeric VARIABLE
// (`strpos(mpg,"7")`) and for `_b[mpg]`/`_se[]` too (O17-O21 in the same log).
// So the test here is on the resolved TEXT and claims every numeric CONSTANT in
// a string slot, whoever wrote it. A numeric VARIABLE is a data-plane type the
// text cannot see -- named gap, not claimed.
//
// inlist()/inrange() are polymorphic in Stata (all arguments one type): a
// MIXED call is the r(109) (`inlist(e(N),"74")`), while all-numeric is fine
// (`inlist(e(N),74)` -> 74 rows, measured O23).
//
// cond() (review finding 1, 2026-09-09) is polymorphic in a SUBSET of its
// slots, which is why the whole-arglist `poly` flag above could not simply be
// reused for it: slot 0 is the numeric CONDITION and slots 1/2 are the value
// pair that must agree with each other. MEASURED on StataNow 19.5 BE
// (/private/tmp/bug456rev/cond_oracle.log, sentinel CONDQ-ORACLE-END, sysuse
// auto + regress price mpg): `local cd = cond(1,e(nosuch),"x")` r(109) (Q1) ·
// `count if cond(1,e(nosuch),"x")=="x"` r(109) (Q2) · `local c5 =
// cond(1,5,"x")` r(109) (Q3) · `local c6 = cond(1,e(N),"x")` r(109) (Q4). The
// ACCEPTING arms are rc 0 and must stay: `count if cond(1,e(cmd),"x")==
// "regress"` -> 74 (Q5, an e-string is a string) and `local c7 =
// cond(1,"a","b")` -> "a" (Q6) -- a whole-arg-list poly test would turn Q6
// into a false r(109) because slot 0 is the number 1.

bool numericConstToken(const std::string& t) {
  if (t.empty()) return false;
  // `.`, `.a`..`.z` (Stata's extended missings) and any ordinary numeric
  // literal, with the sign the substituted value may carry.
  if (t == ".") return true;
  if (t.size() == 2 && t[0] == '.' && std::isalpha(static_cast<unsigned char>(t[1]))) return true;
  static const std::regex reNum(R"(^[+-]?([0-9]+\.?[0-9]*|\.[0-9]+)([eE][+-]?[0-9]+)?$)");
  return std::regex_match(t, reNum);
}

// The string-typed argument slots of the string family this engine registers
// (exprfns.cpp addStataFns). Three kinds of row, checked in this order:
//   `poly`      -- EVERY argument must be one type (inlist/inrange).
//   `polySlots` -- the listed slots must agree with EACH OTHER, the rest are
//                  not claimed (cond: the value pair 1/2, never the condition).
//   `stringSlots` -- fixed string-typed slots; a numeric constant there is the
//                  r(109).
struct StringFnArgSpec {
  const char* name;
  std::vector<size_t> stringSlots;  // empty + poly=true -> the mixed-type rule
  bool poly = false;
  std::vector<size_t> polySlots;  // the mixed-type rule over a SUBSET of slots
};

const std::vector<StringFnArgSpec>& stringFnArgSpecs() {
  static const std::vector<StringFnArgSpec> specs = {
      {"strlen", {0}},   {"length", {0}},     {"strpos", {0, 1}},  {"substr", {0}},
      {"upper", {0}},    {"lower", {0}},      {"strupper", {0}},   {"strlower", {0}},
      {"proper", {0}},   {"trim", {0}},       {"strtrim", {0}},    {"ltrim", {0}},
      {"rtrim", {0}},    {"strltrim", {0}},   {"strrtrim", {0}},   {"stritrim", {0}},
      {"word", {0}},     {"abbrev", {0}},     {"real", {0}},       {"regexm", {0, 1}},
      {"regexr", {0, 1, 2}}, {"subinstr", {0, 1, 2}}, {"soundex", {0}}, {"soundex_nara", {0}},
      {"ustrfix", {0}},
      {"inlist", {}, true}, {"inrange", {}, true},
      // AER scope-2 B2-3: the business-calendar pair. bofd(calname, date) and
      // dofb(bdate, calname) each carry ONE string slot, and a numeric
      // constant there is Stata's r(109) -- which is also how a SWAPPED call
      // (`bofd(td(15jan2020), "sp500")`) is caught, measured rc 109.
      {"bofd", {0}}, {"dofb", {1}},
      // cond(c, a, b [, missval]): slots 1 and 2 are the polymorphic pair.
      {"cond", {}, false, {1, 2}},
      // BUG-660 fix round: the new macro-layer string functions this
      // exprfns.cpp landing added. strproper/usubinstr are literal ALIASES
      // of proper/subinstr (same function pointer, exprfns.cpp's own
      // registration comment) -- omitting them here would have made
      // `strproper(5)` rc 0 while `proper(5)` is r(109), a contradiction for
      // the SAME underlying call. reverse/strrpos/wordcount/strtoname/
      // strmatch/ustrunescape/ustrregexra are new functions with no sibling
      // entry; added by the same general rule (a string function given a
      // numeric CONSTANT is r(109), the BUG-456 contract this whole table
      // implements) rather than left unclaimed.
      {"strproper", {0}}, {"usubinstr", {0, 1, 2}}, {"reverse", {0}},
      {"strrpos", {0, 1}}, {"wordcount", {0}}, {"strtoname", {0}},
      {"strmatch", {0, 1}}, {"ustrunescape", {0}}, {"ustrregexra", {0, 1, 2}},
  };
  return specs;
}

// Splits the argument list of the call whose '(' is at `open`. Returns false if
// the parentheses do not close (malformed text this pass does not own).
bool splitCallArgs(const std::string& e, size_t open, std::vector<std::string>* args,
                   size_t* closeOut) {
  int depth = 0;
  bool inStr = false;
  size_t argStart = open + 1;
  for (size_t i = open; i < e.size(); ++i) {
    const char c = e[i];
    if (inStr) {
      if (c == '\\' && i + 1 < e.size()) { ++i; continue; }
      if (c == '"') inStr = false;
      continue;
    }
    if (c == '"') { inStr = true; continue; }
    if (c == '(') { ++depth; continue; }
    if (c == ')') {
      --depth;
      if (depth == 0) {
        args->push_back(e.substr(argStart, i - argStart));
        *closeOut = i;
        return true;
      }
      continue;
    }
    if (c == ',' && depth == 1) {
      args->push_back(e.substr(argStart, i - argStart));
      argStart = i + 1;
    }
  }
  return false;
}

// Argument kinds for the type test: 1 = string, 2 = numeric constant, 0 = the
// pass does not know (a variable, a call, arithmetic) and does not claim it.
int argKind(const std::string& raw) {
  std::string t = strip(raw);
  if (t.empty()) return 0;
  if (t.front() == '"') return 1;  // a literal (or a literal-headed concat)
  if (numericConstToken(t)) return 2;
  return 0;
}

// Finds the first string-typed slot holding a numeric constant. Returns true
// and fills *call with the offending call's text.
bool stringFnNumericArgImpl(const std::string& e, std::string* call) {
  if (e.find('(') == std::string::npos) return false;
  bool inStr = false;
  for (size_t i = 0; i < e.size(); ++i) {
    const char c = e[i];
    if (inStr) {
      if (c == '\\' && i + 1 < e.size()) { ++i; continue; }
      if (c == '"') inStr = false;
      continue;
    }
    if (c == '"') { inStr = true; continue; }
    if (!isIdentStart(c)) continue;  // a Unicode name is skipped whole, too
    if (i > 0 && isIdentChar(e[i - 1])) continue;  // mid-identifier
    size_t j = i;
    while (j < e.size() && isIdentChar(e[j])) ++j;
    const std::string name = e.substr(i, j - i);
    size_t k = j;
    while (k < e.size() && std::isspace(static_cast<unsigned char>(e[k]))) ++k;
    if (k >= e.size() || e[k] != '(') { i = j - 1; continue; }
    const StringFnArgSpec* spec = nullptr;
    for (const auto& s : stringFnArgSpecs())
      if (name == s.name) { spec = &s; break; }
    if (spec == nullptr) { i = j - 1; continue; }
    std::vector<std::string> args;
    size_t close = 0;
    if (!splitCallArgs(e, k, &args, &close)) { i = j - 1; continue; }
    bool bad = false;
    if (spec->poly) {
      bool anyStr = false, anyNum = false;
      for (const auto& a : args) {
        const int kd = argKind(a);
        if (kd == 1) anyStr = true;
        else if (kd == 2) anyNum = true;
      }
      bad = anyStr && anyNum;
    } else if (!spec->polySlots.empty()) {
      // The mixed-type rule over the named slots only. Slots the call does not
      // supply are simply absent from the group (`cond(1,"a")` is a malformed
      // call this pass does not own).
      bool anyStr = false, anyNum = false;
      for (size_t slot : spec->polySlots) {
        if (slot >= args.size()) continue;
        const int kd = argKind(args[slot]);
        if (kd == 1) anyStr = true;
        else if (kd == 2) anyNum = true;
      }
      bad = anyStr && anyNum;
    } else {
      for (size_t slot : spec->stringSlots)
        if (slot < args.size() && argKind(args[slot]) == 2) { bad = true; break; }
    }
    if (bad) {
      std::string text = e.substr(i, close + 1 - i);
      if (text.size() > 72) text = text.substr(0, 69) + "...";
      *call = text;
      return true;
    }
    i = j - 1;  // keep scanning: nested calls are found by this same walk
  }
  return false;
}

}  // namespace

bool hasBareRParen(const std::string& s) { return hasBareLetterParen(s, 'r'); }

// eb-order T-P1 (SPEC B.4 "One builder", rules 1-3 and 5; declared in
// eval/session.h). Stata's rule, from the oracle records only (SPEC A):
// `_cons` is the last column of its equation and is never spelled
// `(intercept)` (w2g_ebmat.log:230 `x1 x2 _cons` after `regress y x1 x2`, where
// the engine's feols coeftable puts `(intercept)` first); the other columns
// keep the order the engine emitted (IV: endogenous, exogenous, then `_cons`,
// w2g_ebmat.log:536); e(V) is the fit's full covariance in that same order with
// its real off-diagonals (:365 V[1,2] = -1.f7d0ccaeed4b1X-00a). Equation-
// prefixed names (`eq:name`) are grouped by equation in the order the
// equations first appear. Base/omitted factor columns are not in the engine's
// coeftable yet (SPEC Q1, task T-E2) and are not invented here.
std::optional<EvalMatrix> canonicalEMatrix(const nlohmann::ordered_json& block, char which,
                                           std::string* why) {
  const auto fail = [why](std::string w) -> std::optional<EvalMatrix> {
    if (why != nullptr) *why = std::move(w);
    return std::nullopt;
  };
  const std::string label = std::string("e(") + which + ")";
  // A reg3/sureg system: no single table, one `outcomes` entry per equation
  // ({depvar, coeftable}); each column is `depvar:coef` (Stata's reg3/sureg
  // e(b) stripe, `y:x1 y:x2 y:_cons x3:x2 x3:_cons`).
  nlohmann::ordered_json systemTable;
  const bool system = block.is_object() && block.value("estimator", std::string()) == "reg3" &&
                      !block.contains("coeftable") && block.contains("outcomes") &&
                      block.at("outcomes").is_array();
  if (system) {
    systemTable = nlohmann::ordered_json::array();
    for (const auto& o : block.at("outcomes")) {
      if (!o.is_object() || !o.contains("coeftable") || !o.at("coeftable").is_array()) continue;
      const std::string eq = o.value("depvar", std::string());
      for (const auto& row : o.at("coeftable")) {
        if (!row.is_object() || !row.contains("name") || !row.at("name").is_string()) continue;
        nlohmann::ordered_json r = row;
        r["name"] = eq + ":" + row.at("name").get<std::string>();
        systemTable.push_back(std::move(r));
      }
    }
  }
  const nlohmann::ordered_json* table =
      system ? &systemTable
             : (block.is_object() && block.contains("coeftable") ? &block.at("coeftable") : nullptr);
  if (table == nullptr || !table->is_array() || table->empty())
    return fail("matrix " + label + " not found");
  struct Col {
    std::string engine;  // the coeftable's own spelling
    std::string eq;      // "" for a single-equation name
    std::string bare;    // the name after the equation, `(intercept)` spelled `_cons`
    double b = std::numeric_limits<double>::quiet_NaN();
  };
  std::vector<Col> cols;
  for (const auto& row : *table) {
    if (!row.is_object() || !row.contains("name") || !row.at("name").is_string()) continue;
    Col c;
    c.engine = row.at("name").get<std::string>();
    const size_t colon = c.engine.find(':');
    c.eq = colon == std::string::npos ? std::string() : c.engine.substr(0, colon);
    c.bare = colon == std::string::npos ? c.engine : c.engine.substr(colon + 1);
    if (c.bare == "(intercept)") c.bare = "_cons";
    if (row.contains("b") && !row.at("b").is_null()) {
      if (auto f = jsonToFloat(row.at("b"))) c.b = *f;
    }
    cols.push_back(std::move(c));
  }
  if (cols.empty()) return fail("matrix " + label + " not found");
  // Equation order of first appearance; within each, `_cons` after the rest.
  std::vector<std::string> eqs;
  for (const auto& c : cols)
    if (std::find(eqs.begin(), eqs.end(), c.eq) == eqs.end()) eqs.push_back(c.eq);
  std::vector<size_t> order;
  for (const auto& eq : eqs)
    for (int consPass = 0; consPass < 2; ++consPass)
      for (size_t i = 0; i < cols.size(); ++i)
        if (cols[i].eq == eq && (cols[i].bare == "_cons") == (consPass == 1)) order.push_back(i);
  EvalMatrix m;
  for (size_t i : order)
    m.colnames.push_back(cols[i].eq.empty() ? cols[i].bare : cols[i].eq + ":" + cols[i].bare);
  const size_t k = order.size();
  if (which == 'b') {
    m.rownames = {"y1"};
    std::vector<double> row;
    for (size_t i : order) row.push_back(cols[i].b);
    m.data.push_back(std::move(row));
    return m;
  }
  // e(V): the engine's full covariance (`e_V` = {names, matrix}, the shape of
  // apps/cmds/common.cpp add_e_v), permuted by name. Without it there is no
  // e(V) to give: the coeftable's standard errors are only its diagonal.
  const auto eVit = block.find("e_V");
  if (eVit == block.end() || !eVit->is_object() || !eVit->contains("names") ||
      !eVit->contains("matrix") || !eVit->at("names").is_array() || !eVit->at("matrix").is_array())
    return fail("no-e_V");
  const auto& vnames = eVit->at("names");
  const auto& vmat = eVit->at("matrix");
  std::vector<size_t> at(k, std::string::npos);
  for (size_t a = 0; a < k; ++a) {
    const Col& c = cols[order[a]];
    for (size_t j = 0; j < vnames.size(); ++j) {
      if (!vnames[j].is_string()) continue;
      const std::string vn = vnames[j].get<std::string>();
      if (vn == c.engine || (c.bare == "_cons" && (vn == (c.eq.empty() ? "_cons" : c.eq + ":_cons") ||
                                                   vn == (c.eq.empty() ? "(intercept)"
                                                                       : c.eq + ":(intercept)")))) {
        at[a] = j;
        break;
      }
    }
    if (at[a] == std::string::npos || at[a] >= vmat.size())
      return fail("the fit's covariance matrix does not name the coefficient `" + c.engine + "`");
  }
  const auto cell = [&vmat](size_t r, size_t c) -> double {
    if (r >= vmat.size() || !vmat[r].is_array() || c >= vmat[r].size()) return std::nan("");
    const auto& v = vmat[r][c];
    if (v.is_null()) return std::numeric_limits<double>::quiet_NaN();
    auto f = jsonToFloat(v);
    return f ? *f : std::numeric_limits<double>::quiet_NaN();
  };
  m.rownames = m.colnames;
  m.data.assign(k, std::vector<double>(k, 0.0));
  for (size_t a = 0; a < k; ++a)
    for (size_t c = 0; c < k; ++c)
      // Stata's e(V) is exactly symmetric; an engine sandwich can differ from
      // its transpose in the last bit, and the mean of the two is the same
      // value whenever the engine's matrix is symmetric already.
      m.data[a][c] = 0.5 * (cell(at[a], at[c]) + cell(at[c], at[a]));
  return m;
}

std::optional<std::vector<EbStripeEntry>> ebStripeOf(const nlohmann::ordered_json& block,
                                                     const std::vector<std::string>& canonNames) {
  if (!block.is_object()) return std::nullopt;
  const auto it = block.find("eb_stripe");
  if (it == block.end() || !it->is_array() || it->empty()) return std::nullopt;
  std::vector<EbStripeEntry> out;
  std::vector<int> used(canonNames.size(), 0);
  for (const auto& e : *it) {
    if (!e.is_object() || !e.contains("name") || !e.at("name").is_string()) return std::nullopt;
    EbStripeEntry en;
    en.name = e.at("name").get<std::string>();
    if (e.contains("src") && e.at("src").is_string()) en.src = e.at("src").get<std::string>();
    if (!en.src.empty()) {
      const auto f = std::find(canonNames.begin(), canonNames.end(), en.src);
      if (f == canonNames.end()) return std::nullopt;
      if (++used[static_cast<size_t>(f - canonNames.begin())] > 1) return std::nullopt;
    }
    out.push_back(std::move(en));
  }
  for (int u : used)
    if (u != 1) return std::nullopt;
  return out;
}

bool applyEbStripe(const nlohmann::ordered_json& block, char which, EvalMatrix& m) {
  const std::optional<std::vector<EbStripeEntry>> st = ebStripeOf(block, m.colnames);
  if (!st.has_value()) return false;
  const size_t k = st->size();
  std::vector<long> at(k, -1);
  for (size_t a = 0; a < k; ++a) {
    if ((*st)[a].src.empty()) continue;
    at[a] = static_cast<long>(std::find(m.colnames.begin(), m.colnames.end(), (*st)[a].src) -
                              m.colnames.begin());
  }
  EvalMatrix out;
  for (const auto& e : *st) out.colnames.push_back(e.name);
  if (which == 'b') {
    out.rownames = m.rownames;
    out.data.assign(m.data.size(), std::vector<double>(k, 0.0));
    for (size_t r = 0; r < m.data.size(); ++r)
      for (size_t a = 0; a < k; ++a)
        if (at[a] >= 0) out.data[r][a] = m.data[r][static_cast<size_t>(at[a])];
  } else {
    out.rownames = out.colnames;
    out.data.assign(k, std::vector<double>(k, 0.0));
    for (size_t a = 0; a < k; ++a)
      for (size_t c = 0; c < k; ++c)
        if (at[a] >= 0 && at[c] >= 0)
          out.data[a][c] = m.data[static_cast<size_t>(at[a])][static_cast<size_t>(at[c])];
  }
  m = std::move(out);
  return true;
}

std::vector<EbColumnRow> stataEbColumns(const nlohmann::ordered_json& block) {
  std::vector<EbColumnRow> out;
  if (!block.is_object() || !block.contains("coeftable") || !block.at("coeftable").is_array())
    return out;
  // canonicalEMatrix's order, by the same rule: equations in order of first
  // appearance, `_cons` last within each.
  struct C {
    std::string full;
    std::string eq;
    bool cons = false;
    const nlohmann::ordered_json* row = nullptr;
  };
  std::vector<C> cols;
  for (const auto& r : block.at("coeftable")) {
    if (!r.is_object() || !r.contains("name") || !r.at("name").is_string()) continue;
    const std::string n = r.at("name").get<std::string>();
    const size_t colon = n.find(':');
    C c;
    c.eq = colon == std::string::npos ? std::string() : n.substr(0, colon);
    std::string bare = colon == std::string::npos ? n : n.substr(colon + 1);
    if (bare == "(intercept)") bare = "_cons";
    c.cons = bare == "_cons";
    c.full = c.eq.empty() ? bare : c.eq + ":" + bare;
    c.row = &r;
    cols.push_back(std::move(c));
  }
  std::vector<std::string> eqs;
  for (const auto& c : cols)
    if (std::find(eqs.begin(), eqs.end(), c.eq) == eqs.end()) eqs.push_back(c.eq);
  std::vector<std::string> names;
  std::vector<const nlohmann::ordered_json*> rows;
  for (const auto& eq : eqs)
    for (int pass = 0; pass < 2; ++pass)
      for (const auto& c : cols)
        if (c.eq == eq && c.cons == (pass == 1)) {
          names.push_back(c.full);
          rows.push_back(c.row);
        }
  if (const auto st = ebStripeOf(block, names)) {
    for (const auto& e : *st) {
      EbColumnRow cr;
      cr.name = e.name;
      if (!e.src.empty())
        cr.row = rows[static_cast<size_t>(std::find(names.begin(), names.end(), e.src) -
                                          names.begin())];
      out.push_back(std::move(cr));
    }
    return out;
  }
  for (size_t i = 0; i < names.size(); ++i) out.push_back({names[i], rows[i]});
  return out;
}

std::string coefFullName(const nlohmann::ordered_json& block, const std::string& name) {
  if (!block.is_object()) return std::string();
  std::vector<std::string> names;
  for (const char* key : {"coeftable", "eb_stripe"}) {
    const auto it = block.find(key);
    if (it == block.end() || !it->is_array()) continue;
    for (const auto& r : *it)
      if (r.is_object() && r.contains("name") && r.at("name").is_string()) names.push_back(r.at("name").get<std::string>());
  }
  const cecon::postest::CoefRef r = cecon::postest::resolve_coef(names, name, nullptr);
  if (r.kind != cecon::postest::CoefKind::Column) return std::string();
  const std::string& full = names[static_cast<size_t>(r.idx)];
  return full == name ? std::string() : full;
}

std::optional<double> ebStripeCoef(const nlohmann::ordered_json& block, const std::string& name,
                                   const std::string& kind) {
  if (!block.is_object()) return std::nullopt;
  const auto it = block.find("eb_stripe");
  if (it == block.end() || !it->is_array()) return std::nullopt;
  const size_t colon = name.find(':');
  const std::string bare = colon == std::string::npos ? name : name.substr(colon + 1);
  // `0b.g` is also read as `0.g`: the level number, then the variable.
  const auto unmarked = [](const std::string& n) {
    size_t i = 0;
    while (i < n.size() && std::isdigit(static_cast<unsigned char>(n[i])) != 0) ++i;
    if (i > 0 && i + 1 < n.size() && n[i] == 'b' && n[i + 1] == '.') return n.substr(0, i) + n.substr(i + 1);
    return n;
  };
  // Every atom's base/omitted marker dropped: `1o.t#0b.e` is read as `1.t#0.e`,
  // `0b.a#1o.b` as `0.a#1.b` (StataBE bc3.do: `_b[1.t#0.e]` is 0 after reghdfe
  // omits that cell; the engine sends no row for it since BUG-1476).
  const auto plain = [](const std::string& n) {
    static const std::regex reLevel(R"(^(\d+)(?:bn|b|o|bo|ob)\.)");
    std::string out;
    size_t start = 0;
    for (;;) {
      const size_t hash = n.find('#', start);
      std::string atom = n.substr(start, hash == std::string::npos ? std::string::npos : hash - start);
      atom = std::regex_replace(atom, reLevel, "$1.");
      if (atom.rfind("co.", 0) == 0) atom = "c." + atom.substr(3);
      else if (atom.rfind("o.", 0) == 0) atom = atom.substr(2);
      out += atom;
      if (hash == std::string::npos) break;
      out += '#';
      start = hash + 1;
    }
    return out;
  };
  const std::string barePlain = plain(bare);
  for (const auto& e : *it) {
    if (!e.is_object() || !e.contains("name") || !e.at("name").is_string()) continue;
    const std::string nm = e.at("name").get<std::string>();
    const std::string src = e.contains("src") && e.at("src").is_string()
                                ? e.at("src").get<std::string>()
                                : std::string();
    if (src.empty()) {
      if (bare == nm || bare == unmarked(nm) || barePlain == plain(nm)) return 0.0;
      continue;
    }
    if (bare != nm || nm == src) continue;
    // An `o.` spelling: the engine's row under its bare name.
    if (!block.contains("coeftable") || !block.at("coeftable").is_array()) return std::nullopt;
    for (const auto& r : block.at("coeftable")) {
      if (!r.is_object() || r.value("name", std::string()) != src) continue;
      const char* field = kind == "b" ? "b" : "se";
      if (!r.contains(field) || r.at(field).is_null()) return std::nullopt;
      return jsonToFloat(r.at(field));
    }
  }
  return std::nullopt;
}

std::optional<double> ancillaryEbCoef(const EvalSession& session, const std::string& name,
                                      const std::string& kind) {
  // Also a factor level, `#.var` or `#bn.var`, which proportion's posted columns name
  // without its engine row (`0bn.d 1.d`): StataBE 19.5 answers `_b[0.d]`, `_b[0bn.d]`,
  // `_b[1bn.d]` and `_se[1.d]` alike after `proportion d` (scratch-laneA2/agents/epost/
  // s11.log), and `_b[d:0]` is r(111).
  static const std::regex reLevel(R"(^(\d+)(?:bn)?\.([A-Za-z_][A-Za-z0-9_]*)$)");
  const auto plainLevel = [](const std::string& n) {
    std::smatch m;
    return std::regex_match(n, m, reLevel) ? m.str(1) + "." + m.str(2) : std::string();
  };
  const std::string level = plainLevel(name);
  if (name.size() < 2 || (name.front() != '/' && level.empty()) ||
      session.coefOverride != nullptr)
    return std::nullopt;
  const EvalMatrix* m = session.findClassMatrix('e', kind == "b" ? "b" : "V");
  if (m == nullptr) return std::nullopt;
  for (size_t j = 0; j < m->colnames.size(); ++j) {
    if (m->colnames[j] != name && (level.empty() || plainLevel(m->colnames[j]) != level)) continue;
    if (kind == "b") {
      if (m->data.size() != 1 || m->data[0].size() <= j) return std::nullopt;
      return m->data[0][j];
    }
    if (m->data.size() <= j || m->data[j].size() <= j || !(m->data[j][j] >= 0.0))
      return std::nullopt;
    return std::sqrt(m->data[j][j]);
  }
  return std::nullopt;
}

namespace {

// eb-order ROUND 3: the CLOSED stripe table (internal capture
// DIAGNOSIS.md section 3). canonicalEMatrix keeps the
// engine's names, so what Stata's e(b)/e(V) is called -- and whether the
// engine's coefficient table holds all of its columns at all -- depends on the
// command. A command is BUILT only when a row below names it with the record
// that resolves its stripe; every other e(cmd) refuses every e(b)/e(V) reader
// by name (the last line of eStripeRowFor). Rounds 1 and 2 of this goal were
// one class: a deny list whose default answered as emitted (csdid's one `ATT`
// column where Stata's e(b) has one column per ATT(g,t) cell).
//   kAsEmitted          -- the coeftable's names (equation-less in Stata).
//   kDepvarEq           -- every column in one equation named after the depvar.
//   kDepvarEqUnderscore -- the same, the depvar's `.` spelled `_`.
//   kAbsent             -- Stata's e() has no e(b)/e(V) (the 0x0 path:
//                          EvalSession::eMatrixAbsentFromLiveE).
//   kRefuse             -- a named r(198) with `reason`.
// A matrix POSTED for the live e() (glm, oaxaca, teffects, `ereturn post`,
// erepost, estadd matrix, `estat ..., post`) is answered before this table.
enum class EStripe { kAsEmitted, kDepvarEq, kDepvarEqUnderscore, kAbsent, kRefuse };

// What the coefficient table must say about `_cons` for the matrix to be whole:
//   kNeedCons   -- Stata keeps `_cons` unless the fit was noconstant; a table
//                  without one is built only when the fit's block records
//                  `noconstant` (runRegress), since areg/xtreg fe/reghdfe drop
//                  it from the table when a regressor is collinear with the
//                  absorbed effects while Stata's e(b) keeps it
//                  (w2d_uncovered.log:3195-3221; FU-986);
//   kNoCons     -- Stata's e(b) has no `_cons` (the conditional fe fits);
//   kAnyCons    -- the names are Stata's own (the margins/nlcom postings).
enum class ConsRule { kNeedCons, kNoCons, kAnyCons };

struct EStripeRow {
  const char* key;
  EStripe rule;
  ConsRule cons;
  const char* reason;  // kRefuse only
};

constexpr const char* kNoStripeRecord = "";  // reason filled from the key

// Section 3 rows. Each answering row cites the record that resolves it.
const EStripeRow kEStripeTable[] = {
    // w2g_ebmat.log:402 `e(b)[1,3]` x1 x2 _cons; :230 e1_colnames_eb `x1 x2 _cons`
    {"regress", EStripe::kAsEmitted, ConsRule::kNeedCons, nullptr},
    // w2g_ebmat.log:798 `symmetric e(V)[3,3]` (x1 x2 _cons)
    {"areg", EStripe::kAsEmitted, ConsRule::kNeedCons, nullptr},
    // w2g_ebmat.log:860 `symmetric e(V)[3,3]`
    {"reghdfe", EStripe::kAsEmitted, ConsRule::kNeedCons, nullptr},
    // w2g_ebmat.log:594 `symmetric e(V)[3,3]` after `ivregress 2sls y x2 (x1 = id)`
    {"ivregress", EStripe::kAsEmitted, ConsRule::kNeedCons, nullptr},
    // aer_b4/ivreg2-partial.log:121 `e(b)[1,4]`
    {"ivreg2", EStripe::kAsEmitted, ConsRule::kNeedCons, nullptr},
    // ivreghdfe.ado (corpus 065) absorb() sets noconstant,
    // `_cons` only `if `cons''; the absorb rule is in resolveEMatrixFor
    {"ivreghdfe", EStripe::kAsEmitted, ConsRule::kNeedCons, nullptr},
    // w2p_estim_ev.log:755 B_xtreg_fe `|3|3|`; xtreg,re est post, no coleq
    {"xtreg", EStripe::kAsEmitted, ConsRule::kNeedCons, nullptr},
    // qreg posts `b' and `V' without coleq; w2j_e3_qreg2.log:229-232
    {"qreg", EStripe::kAsEmitted, ConsRule::kNeedCons, nullptr},
    // newey: bare coefficient names (the
    // census fit esc_newey failed r(198), so this stays the row's evidence)
    {"newey", EStripe::kAsEmitted, ConsRule::kNeedCons, nullptr},
    // prais: bare coefficient names
    {"prais", EStripe::kAsEmitted, ConsRule::kNeedCons, nullptr},
    // ppmlhdfe (PIET's e(cmd) spells it `fepois`, FU-967): ppmlhdfe.ado (corpus
    // 090) :298-302 `matrix colnames `b' = `indepvars'` (no coleq). Element
    // (1,1) is pinned by w2l_vce_shape_det.log ppmlhdfe_*_se, which bin/main
    // (FU-118) answers; w2v_estripe_census.log:1112 esc_ppmlhdfe `x1 x2 _cons`.
    {"fepois", EStripe::kAsEmitted, ConsRule::kNeedCons, nullptr},
    {"ppmlhdfe", EStripe::kAsEmitted, ConsRule::kNeedCons, nullptr},
    // w2g_ebmat.log:653-656 `yb:x1 yb:x2 yb:_cons`; logistic fits `logit`
    {"logit", EStripe::kDepvarEq, ConsRule::kNeedCons, nullptr},
    // e(cmd) `logistic` (BUG-1192): logit's fit, logit's stripe
    {"logistic", EStripe::kDepvarEq, ConsRule::kNeedCons, nullptr},
    // w2g_ebmat.log:723
    {"probit", EStripe::kDepvarEq, ConsRule::kNeedCons, nullptr},
    {"cloglog", EStripe::kDepvarEq, ConsRule::kNeedCons, nullptr},
    // poisson: a depvar's `.` becomes `_` in the equation name
    {"poisson", EStripe::kDepvarEqUnderscore, ConsRule::kNeedCons, nullptr},
    // xtlogit ,fe fits clogit and reposts nothing; the equation is
    // named `depvar:`.
    // The fe fit only (block estimator xtlogit_fe): re has /lnsig2u.
    {"xtlogit_fe", EStripe::kDepvarEq, ConsRule::kNoCons, nullptr},
    // xtpoisson ,fe: a depvar's `.` becomes `_` in the equation name.
    // The fe fit only.
    {"xtpoisson_fe", EStripe::kDepvarEqUnderscore, ConsRule::kNoCons, nullptr},
    // margins/nlcom `..., post`: the block's names are Stata's (w2v_margins_vce /
    // w2v_margins_corpus readers, 37/37 on bin/main)
    // reg3/sureg: one equation per depvar, each `depvar:coef` with `_cons`
    // last (reg3.ado's `y:x1 y:x2 y:_cons x3:x2 x3:_cons`); canonicalEMatrix
    // builds the columns from the block's per-equation tables and e(V) is the
    // engine's stacked system covariance. Neither accepts noconstant or factor
    // terms here (runReg3 refuses them), so the table's names are the stripe.
    {"reg3", EStripe::kAsEmitted, ConsRule::kAnyCons, nullptr},
    {"sureg", EStripe::kAsEmitted, ConsRule::kAnyCons, nullptr},
    {"margins", EStripe::kAsEmitted, ConsRule::kAnyCons, nullptr},
    {"nlcom", EStripe::kAsEmitted, ConsRule::kAnyCons, nullptr},
    // No e(b): w2g_ebmat.log:2087 e15c_matrix_b `0|`, :2096 (pca);
    // w2v_estpost_ematrix ep13_b_shape `0|0|0||`; w2g_ebmat.log:2486
    // e15d_list_eb2 `111` (rddensity)
    {"pca", EStripe::kAbsent, ConsRule::kAnyCons, nullptr},
    {"estpost", EStripe::kAbsent, ConsRule::kAnyCons, nullptr},
    {"rddensity", EStripe::kAbsent, ConsRule::kAnyCons, nullptr},
    // REFUSE rows with a reason of their own (owners: DIAGNOSIS section 3).
    {"csdid", EStripe::kRefuse, ConsRule::kAnyCons,
     "Stata's e(b) after csdid has one column per ATT(g,t) cell (`g####:t_####_####`, "
     "csdid_kitchensink_oracle.log:61-87, :107), which the engine does not send"},
    {"drdid", EStripe::kRefuse, ConsRule::kAnyCons,
     "Stata posts e(b) as `ATET:r1vs0.<treatment>` (drdid), which this version "
     "does not build"},
    {"rdrobust", EStripe::kRefuse, ConsRule::kAnyCons,
     // runRdrobust posts e(b)/e(V) as e() matrices, which answer before this
     // table; the row answers only an rdrobust e() that carries neither.
     "Stata's e(b) after rdrobust is 1x3 (Conventional, Bias-corrected, Robust; "
     "w2g_ebmat.log:2305), and these results carry none"},
    {"mean", EStripe::kRefuse, ConsRule::kAnyCons, kNoStripeRecord},
    {"proportion", EStripe::kRefuse, ConsRule::kAnyCons, kNoStripeRecord},
    {"did_imputation", EStripe::kRefuse, ConsRule::kAnyCons, kNoStripeRecord},
    {"eventstudyinteract", EStripe::kRefuse, ConsRule::kAnyCons, kNoStripeRecord},
    {"sdid", EStripe::kRefuse, ConsRule::kAnyCons, kNoStripeRecord},
    {"stcox", EStripe::kRefuse, ConsRule::kAnyCons, kNoStripeRecord},
    {"fracreg", EStripe::kRefuse, ConsRule::kAnyCons, kNoStripeRecord},
    {"xtabond", EStripe::kRefuse, ConsRule::kAnyCons, kNoStripeRecord},
    {"mediate", EStripe::kRefuse, ConsRule::kAnyCons, kNoStripeRecord},
    // nl's free parameters are the engine's own row names (`/b0 /b1`), which is
    // e(b)'s stripe in StataBE 19.5 (BUG-1246: colfullnames `/b0 /b1`, el(e(b),1,2)
    // = _b[/b1]); no `_cons` column.
    {"nl", EStripe::kAsEmitted, ConsRule::kAnyCons, nullptr},
};

// Ancillary parameters or extra equations the engine's table does not carry
// (tobit/ivtobit var(e.y), nbreg lnalpha, ologit/oprobit cuts, mixed variance
// components, streg ancillaries, the selection/inflation equations of
// heckman/zip/zinb/ivprobit, the outcome equations of mlogit/var, the
// parameter equations of gmm; w2u_binary_ml_edges.log heckman `yh:... s:...
// /mills`; owner FU-968).
const char* const kAncillaryCmds[] = {"nbreg", "tobit", "ivtobit", "ologit", "oprobit", "mlogit",
                                      "zip",   "zinb",  "heckman", "ivprobit", "var",
                                      "gmm",   "mixed",   "streg"};

// The block spellings of a key, for the scoped coefOverride (no e(cmd)) and for
// the fe fits whose e(cmd) does not say fe.
std::string eStripeKeyOfEstimator(const std::string& est) {
  if (est == "feols") return "regress";
  if (est == "logistic") return "logit";
  const std::string head = est.substr(0, est.find(' '));  // "ivregress 2sls|gmm|liml"
  return head;
}

EStripeRow eStripeRowFor(const std::string& eCmd, const std::string& blockEstimator) {
  std::string key = eCmd.empty() ? eStripeKeyOfEstimator(blockEstimator) : eCmd;
  // e(cmd) `xtlogit`/`xtpoisson` covers re and fe; only the fe fit has a row.
  if (key == "xtlogit" || key == "xtpoisson")
    key = blockEstimator == key + "_fe" ? blockEstimator : key + " (not fe)";
  // xtlogit, fe with an empty cluster() posts the underlying clogit e(cmd).
  // The fitted block still identifies the conditional FE estimator and now
  // carries its full covariance, so it uses that same no-constant stripe.
  if (key == "clogit" && blockEstimator == "xtlogit_fe") key = "xtlogit_fe";
  for (const EStripeRow& r : kEStripeTable)
    if (key == r.key) return r;
  for (const char* c : kAncillaryCmds)
    if (key == c)
      return {c, EStripe::kRefuse, ConsRule::kAnyCons,
              "Stata's e(b) has equations or ancillary parameters the engine does not send"};
  return {nullptr, EStripe::kRefuse, ConsRule::kAnyCons, kNoStripeRecord};
}

// A factor-variable column (`2.g`, `1b.g`, `x1#2.g`, `2.g#c.x`): any `#`, or a
// part that starts with a level number followed by `.` (FU-118's rule).
bool isFactorColumn(const std::string& name) {
  if (name.find('#') != std::string::npos) return true;
  static const std::regex reLevel(R"((^|[:#])\d+[A-Za-z]*\.)");
  return std::regex_search(name, reLevel);
}

// Whether a column name already carries Stata's base (`1b.`, `1bn.`) or
// omitted (`o.`, `2o.`, `co.`) marker in any `:`/`#`-separated part.
bool hasBaseOrOmittedMarker(const std::string& name) {
  static const std::regex reMark(R"((^|[:#])(\d*(b|bn|o)|co)\.)");
  return std::regex_search(name, reMark);
}

}  // namespace

std::string depvarEquationOf(const EvalSession& session) {
  const nlohmann::ordered_json* source = session.coefSource();
  if (source == nullptr || !source->is_object()) return std::string();
  const std::string* eCmd = session.eStrings.find("cmd");
  const EStripeRow row = eStripeRowFor(eCmd != nullptr ? *eCmd : std::string(),
                                       source->value("estimator", std::string()));
  if (row.rule != EStripe::kDepvarEq && row.rule != EStripe::kDepvarEqUnderscore)
    return std::string();
  std::string eq = source->value("depvar", std::string());
  if (eq.empty())
    if (const std::string* dv = session.eStrings.find("depvar")) eq = *dv;
  if (row.rule == EStripe::kDepvarEqUnderscore) std::replace(eq.begin(), eq.end(), '.', '_');
  return eq;
}

std::string omittedColumnSpelling(const std::string& name) {
  const size_t colon = name.find(':');
  const std::string eq = colon == std::string::npos ? std::string() : name.substr(0, colon + 1);
  const std::string bare = colon == std::string::npos ? name : name.substr(colon + 1);
  if (bare.empty() || std::isdigit(static_cast<unsigned char>(bare[0])) != 0) return std::string();
  for (char c : bare)
    if (!(std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_')) return std::string();
  if (bare == "_cons") return std::string();
  return eq + "o." + bare;
}

bool ebHasUnsentColumns(const EvalSession& session) {
  const nlohmann::ordered_json* source = session.coefSource();
  const std::string* eCmd = session.eStrings.find("cmd");
  const std::string key =
      eCmd != nullptr && !eCmd->empty()
          ? *eCmd
          : (source != nullptr && source->is_object()
                 ? eStripeKeyOfEstimator(source->value("estimator", std::string()))
                 : std::string());
  for (const char* c : kAncillaryCmds)
    if (key == c) return true;
  return false;
}

EvalSession::EMatrixAnswer EvalSession::resolveEMatrixFor(const std::string& name,
                                                          EReader reader) const {
  EMatrixAnswer a;
  const auto fail = [&a](int code, std::string w, bool absent) {
    a.rc = code;
    a.why = std::move(w);
    a.absent = absent;
    return a;
  };
  if (const EvalMatrix* posted = findClassMatrix('e', name)) {
    a.matrix = *posted;
    return a;
  }
  // e(beta) and e(V_modelbased) of a fit whose block records them (regress's
  // `beta_std`, a built-in multiway fit's `e_Vmb`; estimation_m4.cpp
  // postENameListsRegressM4): read at the moment of need from a local copy of
  // the live block with its coefficients (covariance) replaced, through the very
  // e(b) (e(V)) reader below, so they carry e(b)'s (e(V)'s) stripe -- the base
  // and `o.` columns annotateEbStripe records after the fit included (StataBE
  // 19.5 `regress y x1 i.g`: e(beta) `x1 1b.g 2.g 3.g` with a 0 base column;
  // the multiway-cluster e(V_modelbased) on e(V)'s stripe). e(beta)
  // drops `_cons` ([R] regress, Stored results) and its row is `r1` (StataBE
  // `matrix list e(beta)`).
  if ((name == "beta" || name == "V_modelbased") && eLive()) {
    const nlohmann::ordered_json* live = coefSource();
    const char* key = name == "beta" ? "beta_std" : "e_Vmb";
    if (live != nullptr && live->is_object() && live->contains(key) && live->at(key).is_object()) {
      nlohmann::ordered_json copy = *live;
      if (name == "V_modelbased") {
        copy["e_V"] = live->at(key);
        return resolveFitEMatrixFrom("V", reader, &copy);
      }
      const auto& beta = live->at(key);
      if (!copy.contains("coeftable") || !copy["coeftable"].is_array())
        return fail(111, "matrix e(beta) not found", true);
      for (auto& r : copy["coeftable"]) {
        if (!r.is_object() || !r.contains("name") || !r["name"].is_string())
          return fail(111, "matrix e(beta) not found", true);
        const std::string nm = r["name"].get<std::string>();
        if (nm == "(intercept)" || nm == "_cons") continue;
        const auto it = beta.find(nm);
        if (it != beta.end() && it->is_number()) {
          r["b"] = *it;
          continue;
        }
        // A column the fit omitted (b 0): Stata's e(beta) is 0 there too.
        const auto b = r.find("b");
        if (b == r.end() || !b->is_number() || b->get<double>() != 0.0)
          return fail(111, "matrix e(beta) not found", true);
      }
      EMatrixAnswer bm = resolveFitEMatrixFrom("b", reader, &copy);
      if (!bm.matrix.has_value()) {
        if (!bm.why.empty()) {
          const size_t at = bm.why.find("e(b)");
          if (at != std::string::npos) bm.why.replace(at, 4, "e(beta)");
        }
        return bm;
      }
      if (bm.matrix->data.size() != 1) return fail(111, "matrix e(beta) not found", true);
      EvalMatrix out;
      out.rownames = {"r1"};
      out.data.emplace_back();
      for (size_t j = 0; j < bm.matrix->colnames.size(); ++j) {
        const std::string& c = bm.matrix->colnames[j];
        const size_t colon = c.find(':');
        const std::string bare = colon == std::string::npos ? c : c.substr(colon + 1);
        if (bare == "_cons") continue;
        out.colnames.push_back(c);
        out.data[0].push_back(bm.matrix->data[0][j]);
      }
      if (out.colnames.empty()) return fail(111, "matrix e(beta) not found", true);
      a.matrix = std::move(out);
      return a;
    }
  }
  if (name != "b" && name != "V") return fail(111, "matrix e(" + name + ") not found", true);
  return resolveFitEMatrixFrom(name, reader, coefSource());
}

EvalSession::EMatrixAnswer EvalSession::resolveFitEMatrixFrom(
    const std::string& name, EReader reader, const nlohmann::ordered_json* source) const {
  EMatrixAnswer a;
  const auto fail = [&a](int code, std::string w, bool absent) {
    a.rc = code;
    a.why = std::move(w);
    a.absent = absent;
    return a;
  };
  if (name != "b" && name != "V") return fail(111, "matrix e(" + name + ") not found", true);
  // No e() at all: Stata's r(301) "last estimates not found" (w2g_ebmat.log:2229
  // e15c_fresh_matrix_b, :2238 e15c_fresh_matlist_eb).
  if (!eLive()) return fail(301, "last estimates not found", true);
  const std::string* eCmdP = coefOverride == nullptr ? eStrings.find("cmd") : nullptr;
  const std::string eCmd = eCmdP != nullptr ? *eCmdP : std::string();
  const std::string blockEst = source != nullptr && source->is_object()
                                   ? source->value("estimator", std::string())
                                   : std::string();
  const EStripeRow row = eStripeRowFor(eCmd, blockEst);
  const std::string cmdName = !eCmd.empty() ? eCmd : blockEst;
  const auto refuse = [&](const std::string& reason) {
    return fail(198,
                "e(" + name + ") is not available in this version of PIET after `" +
                    (cmdName.empty() ? std::string("this estimation") : cmdName) + "`: " + reason,
                false);
  };
  const auto rowReason = [&row, &cmdName]() -> std::string {
    if (row.reason != nullptr && *row.reason != '\0') return row.reason;
    return "Stata's stripe for " + (cmdName.empty() ? std::string("this estimation") : cmdName) +
           " is not recorded here";
  };
  if (source == nullptr) {
    // A live e() without this matrix (pca, estpost, rddensity, an `ereturn post`
    // without b: w2g_ebmat.log:2486 e15d_list_eb2 `matrix e(b) not found`,
    // r(111)). A named row Stata DOES post an e(b) for (rdrobust) refuses.
    if (row.key != nullptr && row.rule == EStripe::kRefuse) return refuse(rowReason());
    return fail(111, "matrix e(" + name + ") not found", true);
  }
  // `erepost b = M` (handlers_aer_tables.cpp runErepost marks the block): on a
  // posting with no e(b) (estpost) the rows ARE M -- its names, its order, no
  // e(V) (erepost.ado `eret post `b''; w2v_estpost_ematrix.log
  // ep14e_colnames_eb `c1 c2`, ep14e2_colnames_eb `x1 x2`). On a fit the rows
  // keep the fit's names and only the values change, so the stripe row and
  // both guards still apply (G1 reads the row's `omitted` mark there).
  const auto repIt = source->find("reposted_b");
  const bool reposted = repIt != source->end() && repIt->is_boolean() && repIt->get<bool>();
  if (row.rule == EStripe::kAbsent && reposted) {
    if (name != "b") return fail(111, "matrix e(" + name + ") not found", true);
    EvalMatrix pm;
    pm.rownames = {"y1"};
    pm.data.emplace_back();
    if (source->contains("coeftable") && source->at("coeftable").is_array()) {
      for (const auto& r : source->at("coeftable")) {
        if (!r.is_object() || !r.contains("name") || !r.at("name").is_string()) continue;
        pm.colnames.push_back(r.at("name").get<std::string>());
        double v = std::numeric_limits<double>::quiet_NaN();
        if (r.contains("b") && !r.at("b").is_null())
          if (auto f = jsonToFloat(r.at("b"))) v = *f;
        pm.data[0].push_back(v);
      }
    }
    a.matrix = std::move(pm);
    return a;
  }
  if (row.rule == EStripe::kRefuse || row.rule == EStripe::kAbsent) return refuse(rowReason());
  // The coefficient table's terms, in the engine's spelling.
  struct Term {
    std::string name;
    bool zero = false;  // b == 0 exactly and se 0 or missing, or marked omitted
  };
  std::vector<Term> terms;
  if (source->is_object() && source->contains("coeftable") && source->at("coeftable").is_array()) {
    for (const auto& r : source->at("coeftable")) {
      if (!r.is_object() || !r.contains("name") || !r.at("name").is_string()) continue;
      Term t;
      t.name = r.at("name").get<std::string>();
      std::optional<double> b, se;
      if (r.contains("b") && !r.at("b").is_null()) b = jsonToFloat(r.at("b"));
      if (r.contains("se") && !r.at("se").is_null()) se = jsonToFloat(r.at("se"));
      // A reposted row's se is null and its b is M's value, so the fit's
      // omission survives only as the row's `omitted` mark (runErepost sets it
      // before it nulls se); a reposted 0 alone is not an omission.
      const auto omIt = r.find("omitted");
      const bool marked = omIt != r.end() && omIt->is_boolean() && omIt->get<bool>();
      t.zero = marked ||
               (!reposted && b.has_value() && *b == 0.0 && (!se.has_value() || *se == 0.0));
      terms.push_back(std::move(t));
    }
  }
  // (G1) an omitted (collinear) column: the engine keeps it at b 0 / se 0 under
  // its bare name, where Stata marks the omitted column `o.` (w2u_binary_ml_edges.log:215
  // bm_l_dz0_names `x1 o.dz0 _cons`). Stata may omit a different member of the
  // collinear set than the engine (w2v_estripe_census.log:274 esc_regress_omit
  // `o.x1 x2 x3c _cons` where the engine dropped x3c), so the refusal names
  // only the engine's column. Owner: T-E2.
  // eb-order T-E2: a stripe the fit recorded (ebStripeOf) names the base
  // columns and the `o.` spelling of a screen-omitted column itself.
  std::vector<std::string> stripeNames;  // the stripe's Stata names, when it has one
  std::vector<std::string> stripeSrcOmitted;  // canonical columns it spells `o.`
  if (const auto stIt = source->find("eb_stripe"); stIt != source->end() && stIt->is_array()) {
    for (const auto& e : *stIt) {
      if (!e.is_object() || !e.contains("name") || !e.at("name").is_string()) continue;
      const std::string nm = e.at("name").get<std::string>();
      stripeNames.push_back(nm);
      if (e.contains("src") && e.at("src").is_string() && hasBaseOrOmittedMarker(nm))
        stripeSrcOmitted.push_back(e.at("src").get<std::string>());
    }
  }
  for (const Term& t : terms)
    if (t.zero && !hasBaseOrOmittedMarker(t.name) &&
        std::find(stripeSrcOmitted.begin(), stripeSrcOmitted.end(), t.name) ==
            stripeSrcOmitted.end())
      return refuse("the fit omitted the collinear column `" + t.name +
                    "` (Stata marks an omitted column `o.`; the engine and Stata may omit "
                    "different columns of a collinear set)");
  // Factor-variable terms whose base levels the engine does not send (`2.g 3.g`
  // where Stata has `1b.g 2.g 3.g`, w2g_ebmat.log:452 e2_colnames_eb). Owner
  // T-E2; `matrix b = e(b)` and `matrix list e(b)` keep bin/main's rc-0 answer
  // (e2_list_eb, w2g_ebmat.log:495), the name and operand readers refuse.
  if (row.cons != ConsRule::kAnyCons && reader != EReader::kCopy) {
    const bool anyFactor = std::any_of(terms.begin(), terms.end(),
                                       [](const Term& t) { return isFactorColumn(t.name); });
    const bool anyBase = std::any_of(terms.begin(), terms.end(), [](const Term& t) {
      return hasBaseOrOmittedMarker(t.name);
    }) || std::any_of(stripeNames.begin(), stripeNames.end(), [](const std::string& n) {
      return hasBaseOrOmittedMarker(n);
    });
    // A recorded stripe placed every column under Stata's name (a `c.x#c.z`
    // cell has no base level to mark).
    if (anyFactor && !anyBase && stripeNames.empty())
      return refuse("the fit has factor-variable terms, and the engine does not send their "
                    "base-level columns (`1b.` ...) that Stata's e(" + name + ") carries");
  }
  const auto isCons = [](const std::string& n) {
    const size_t colon = n.find(':');
    const std::string bare = colon == std::string::npos ? n : n.substr(colon + 1);
    return bare == "_cons" || bare == "(intercept)";
  };
  const bool hasCons =
      std::any_of(terms.begin(), terms.end(), [&isCons](const Term& t) { return isCons(t.name); });
  if (row.cons == ConsRule::kNoCons && hasCons)
    return refuse("the fit's coefficient table has a `_cons` that Stata's e(" + name +
                  ") does not");
  if (row.cons == ConsRule::kNeedCons) {
    // ivreghdfe under absorb() partials the constant out (w2b_estim.log:1168
    // `Partialled-out: _cons`); the block names the absorbed effects.
    const auto absorbedIt = source->find("absorbed");
    const bool consPartialled = std::string(row.key) == "ivreghdfe" &&
                                absorbedIt != source->end() && absorbedIt->is_object() &&
                                !absorbedIt->empty();
    const auto ncIt = source->find("noconstant");
    const bool noConstant = ncIt != source->end() && ncIt->is_boolean() && ncIt->get<bool>();
    if (consPartialled) {
      if (hasCons)
        return refuse("the fit's coefficient table has a `_cons` that ivreghdfe partials out");
    } else if (!hasCons && !noConstant) {
      return refuse("the fit's coefficient table has no `_cons`, and this build cannot tell a "
                    "noconstant fit from a constant the engine dropped, so the columns of "
                    "Stata's e(" + name + ") are unknown");
    }
  }
  std::string w;
  std::optional<EvalMatrix> m = canonicalEMatrix(*source, name[0], &w);
  if (!m.has_value() && w == "no-e_V") {
    if (reader == EReader::kOperand) {
      // Only the coefficient table's standard errors exist: V's DIAGONAL. The
      // operand caller answers el(e(V),i,i) = se_i^2 and the dimensions, and
      // refuses an off-diagonal cell by name (FU-118), never the silent 0 of a
      // diagonal synthesis (FU-98).
      std::optional<EvalMatrix> bm = canonicalEMatrix(*source, 'b', &w);
      // (a reg3/sureg system has no top-level table to read the se's from)
      if (bm.has_value() && source->contains("coeftable") && source->at("coeftable").is_array()) {
        EvalMatrix d;
        d.colnames = bm->colnames;
        d.rownames = bm->colnames;
        const size_t k = d.colnames.size();
        d.data.assign(k, std::vector<double>(k, 0.0));
        // canonicalEMatrix's column order, read back by name from the table.
        for (size_t i = 0; i < k; ++i) {
          d.data[i][i] = std::numeric_limits<double>::quiet_NaN();
          for (const auto& r : source->at("coeftable")) {
            if (!r.is_object() || !r.contains("name") || !r.at("name").is_string()) continue;
            std::string n = r.at("name").get<std::string>();
            const size_t colon = n.find(':');
            std::string bare = colon == std::string::npos ? n : n.substr(colon + 1);
            if (bare == "(intercept)") bare = "_cons";
            const std::string full =
                colon == std::string::npos ? bare : n.substr(0, colon + 1) + bare;
            if (full != d.colnames[i]) continue;
            if (r.contains("se") && !r.at("se").is_null())
              if (auto se = jsonToFloat(r.at("se"))) d.data[i][i] = *se * *se;
            break;
          }
        }
        m = std::move(d);
        a.diagonalOnly = true;
      }
    } else {
      // SPEC Q2 interim, an open completion gap: the engine sent no covariance
      // for this fit, and a diagonal of se^2 would answer every off-diagonal
      // with a silent 0. A named refusal, never a diagonal.
      return refuse("the engine did not send the fit's covariance matrix");
    }
  }
  if (!m.has_value()) return fail(111, w, false);  // defensive: G1 runs first
  // The fit's own stripe (base columns, `o.` names): e(b) and e(V) alike. A
  // diagonal-only e(V) keeps zeros off the diagonal there too.
  if (!applyEbStripe(*source, name[0], *m) && !stripeNames.empty() &&
      row.cons != ConsRule::kAnyCons && reader != EReader::kCopy &&
      std::any_of(terms.begin(), terms.end(), [](const Term& t) { return isFactorColumn(t.name); }))
    return refuse("the fit's recorded e(" + name + ") stripe no longer matches its coefficient "
                  "table, so the base-level columns Stata's e(" + name + ") carries are unknown");
  if (row.rule == EStripe::kDepvarEq || row.rule == EStripe::kDepvarEqUnderscore) {
    std::string eq = source->value("depvar", std::string());
    if (eq.empty() && coefOverride == nullptr) {
      if (const std::string* dv = eStrings.find("depvar")) eq = *dv;
    }
    if (eq.empty()) {
      a.diagonalOnly = false;
      return refuse("the fit did not record its dependent variable");
    }
    if (row.rule == EStripe::kDepvarEqUnderscore) std::replace(eq.begin(), eq.end(), '.', '_');
    const auto stripeOf = [&eq](std::vector<std::string>& names) {
      for (auto& n : names)
        if (n.find(':') == std::string::npos) n = eq + ":" + n;
    };
    stripeOf(m->colnames);
    if (name == "V") stripeOf(m->rownames);
  }
  a.matrix = std::move(m);
  return a;
}

// B4-W3b: is this line one of the MATRIX verbs — the only surface that can
// consume an r-class matrix reference (handlers_m3.cpp:1978 / :1955)? Stata's
// own abbreviations of `matrix` are mat/matr/matri/matrix; `matlist` is the
// separate display verb that reads the same store.
bool isMatrixVerbLine(const std::string& line) {
  size_t i = line.find_first_not_of(" \t");
  if (i == std::string::npos) return false;
  size_t j = i;
  while (j < line.size() && (std::isalpha(static_cast<unsigned char>(line[j])) != 0)) ++j;
  std::string verb = line.substr(i, j - i);
  for (auto& c : verb) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return verb == "mat" || verb == "matr" || verb == "matri" || verb == "matrix" ||
         verb == "matlist";
}
// W2AK: an `estadd` line (after any capture/noisily/quietly prefixes). estadd
// reads `r(name)` ITSELF -- `estadd r(mean)` posts e(mean) (estadd)
// and `estadd scalar r(sd)` posts e(sd) (:249-255) -- so the bare r() pass
// leaves those references for the handler instead of splicing a number into
// the subcommand position (w2ak ek_ad_r_bare, ek_ad_scalar_rname).
// AER tier-1: `matselrc` too -- its row()/col() options abbreviate to r()/c(),
// so `matselrc A B, r(r3)` names a row and must not become `r(.)` (StataBE
// 2026-10-28: rc 0, one row r3; internal capture
// t1_matselrc_onename.log).
bool isEstaddLine(const std::string& line) {
  static const std::set<std::string> kPrefix = {
      "cap",  "capt", "captu", "captur", "capture", "n",      "no",     "noi",    "nois",
      "noisi", "noisil", "noisily", "qui",  "quie",  "quiet", "quietl", "quietly"};
  size_t i = 0;
  for (;;) {
    i = line.find_first_not_of(" \t", i);
    if (i == std::string::npos) return false;
    size_t j = i;
    while (j < line.size() && std::isalpha(static_cast<unsigned char>(line[j])) != 0) ++j;
    std::string w = line.substr(i, j - i);
    for (auto& c : w) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (w == "estadd" || w == "matselrc") {
      return j >= line.size() || line[j] == ' ' || line[j] == '\t';
    }
    if (kPrefix.count(w) == 0) return false;
    i = j;
    if (i < line.size() && line[i] == ':') ++i;
  }
}
bool hasBareEParen(const std::string& s) { return hasBareLetterParen(s, 'e'); }

namespace {
// e(sample) T2: the one spelling of the token, applied to text whose string
// literals protectStringLiterals has already swapped out.
const std::regex& reEsampleToken() {
  static const std::regex re(R"(\be\(\s*sample\s*\))");
  return re;
}
}  // namespace

bool hasEsampleToken(const std::string& s) {
  if (s.find("sample") == std::string::npos || !hasBareEParen(s)) return false;
  std::vector<std::string> spans;
  const std::string bare = protectStringLiterals(s, &spans);
  return std::regex_search(bare, reEsampleToken());
}

std::string replaceEsampleToken(const std::string& s, const std::string& with) {
  if (!hasEsampleToken(s)) return s;
  std::vector<std::string> spans;
  const std::string bare = protectStringLiterals(s, &spans);
  // `with` is spliced verbatim: regex_replace would read a `$` in it as a group.
  std::string out;
  auto it = std::sregex_iterator(bare.begin(), bare.end(), reEsampleToken());
  size_t last = 0;
  for (; it != std::sregex_iterator(); ++it) {
    out.append(bare, last, static_cast<size_t>(it->position(0)) - last);
    out += with;
    last = static_cast<size_t>(it->position(0) + it->length(0));
  }
  out.append(bare, last, std::string::npos);
  return restoreStringLiterals(out, spans);
}

namespace {

// BUG-629 (eval half): the spellings in which a bare r(NAME) is a MATRIX operand
// that substituteMatrixRefs answers on every verb -- r(NAME)[i,j],
// rowsof(r(NAME)), colsof(r(NAME)) and el(r(NAME),i,j). w2c_postest.log family
// (1) reads a posted r(table) as `display rowsof(r(table))`, `display %18.0g
// r(table)[1,1]` and `local x : display ...`; family (5) reads nlcom's r(b) as
// `r(b)[1,1]`. Returns `s` with each operand occurrence of r(NAME) cut out, so an
// r(NAME) still present afterwards is one used as a SCALAR. NAME is an
// identifier (reRRef's grammar), so it is safe inside a pattern.
std::string cutRMatrixOperands(const std::string& s, const std::string& name) {
  if (s.find("r(" + name + ")") == std::string::npos) return s;
  const std::string ref = R"(r\()" + name + R"(\))";
  std::string out = std::regex_replace(s, std::regex(R"(\b)" + ref + R"((?=\s*\[))"), "");
  out = std::regex_replace(
      out, std::regex(R"(\b(colsof|rowsof)(\s*\(\s*))" + ref + R"((?=\s*\)))", std::regex::icase),
      "$1$2");
  out = std::regex_replace(
      out, std::regex(R"(\b(el)(\s*\(\s*))" + ref + R"((?=\s*,))", std::regex::icase), "$1$2");
  return out;
}

// The inline extended macro functions, `` `: fn args' `` anywhere in a line, all
// answered by tryExtendedMacroFn -- the arm that answers `local x : fn args`.
// BUG-1384/1379/1380: this was six per-function patterns (the matrix stripes of
// BUG-629, `: word`, the variable readers of BUG-719 with `: label (V) #` only,
// `: sortedby`, `: e(kind)`, `: char`), and every other inline function tripped
// the tokenizer's "does not expand macros" refusal (r(198)) although its two-line
// form answered: `` `: label ul 1' `` (StataBE 19.5 `Yes`), `` `: var lab u' ``,
// `` `: list A - B' ``, `` `: pwd' ``, and `` `: word count `"a b"' c' `` (2), whose
// compound quotes the `: word` pattern could not hold.
//
// A span is READY once it holds no macro reference of its own: `` `: word `i' of
// `plist'' `` waits for `i' and `plist' to expand on the pass before (w2x
// fx_nullmat_macro), and a $global in it likewise. A compound-quoted string
// inside it is an argument, not a reference. `display` is not this layer's: the
// dispatcher splices `` `: display ...' `` before the line is expanded
// (substituteInlineDisplay), so it is left alone here. Nor is a span escaped as
// `` \`: fn' ``: Stata keeps it literal (`display "\`: list sizeof A'"` prints
// `` `: list sizeof A' ``), which this layer cannot express, so it stays for the
// tokenizer's refusal rather than turning into `\3`. Returns the start of the
// rightmost ready span beginning before `before` (its closing quote in *end when
// `end` is given), or npos.
size_t findReadyInlineMacroFn(const std::string& line, size_t before, size_t* end) {
  static const std::regex reDisplayHead(R"(^\s*(?:display|displa|displ|disp|dis|di)(?:\s|$))",
                                        std::regex::icase);
  for (size_t p = before == 0 ? std::string::npos : line.rfind("`:", before - 1);
       p != std::string::npos; p = p == 0 ? std::string::npos : line.rfind("`:", p - 1)) {
    if (p > 0 && line[p - 1] == '\\') continue;
    size_t i = p + 2, quote = std::string::npos;
    int depth = 0;
    while (i < line.size()) {
      if (startsCompound(line, i)) {
        ++depth;
        i += 2;
      } else if (line[i] == '`') {
        break;  // a macro reference still to expand
      } else if (depth > 0 && line[i] == '\'') {
        // BUG-1340: a nested pair closes at its first quote, `"' or not:
        // `"[`: word count `"a b c'"'|"` prints [1| in StataBE 19.5 (the list
        // is `"a b c'"`).
        --depth;
        ++i;
      } else if (line[i] == '\'' && depth == 0) {
        quote = i;
        break;
      } else {
        ++i;
      }
    }
    if (quote == std::string::npos) continue;
    const std::string body = line.substr(p + 2, quote - p - 2);
    if (body.find('$') != std::string::npos &&
        (hasGlobalRef(reGlobalRefBrace(), body) || hasGlobalRef(reGlobalRefBare(), body)))
      continue;
    if (std::regex_search(body, reDisplayHead)) continue;
    if (end != nullptr) *end = quote;
    return p;
  }
  return std::string::npos;
}

bool usesRRefAsScalar(const std::string& s, const std::string& name) {
  const std::string cut = cutRMatrixOperands(s, name);
  if (cut.find("r(" + name + ")") == std::string::npos) return false;
  return std::regex_search(cut, std::regex(R"(\br\()" + name + R"(\))"));
}

// Stata's code for reading an r() matrix that is not there. r(NAME) is then the
// missing scalar, so `r(NAME)[i,j]` is `.[i,j]` -- r(198) "invalid syntax"
// (w2c_postest.log:363-370, rtable_survives_summarize) -- and colsof/rowsof of it
// is a scalar where a matrix is required -- r(109) "type mismatch"
// (w2c_postest.log:377-384, rtable_replaced_by_test). Decided by the FIRST bare
// reference, the one Stata's parser stops at. "" = neither spelling.
std::string absentClassMatrixRc(const std::string& s, char cls, const std::string& name) {
  std::smatch m;
  if (!std::regex_search(s, m, std::regex(R"(\b)" + std::string(1, cls) + R"(\()" + name + R"(\))")))
    return std::string();
  const size_t at = static_cast<size_t>(m.position(0));
  size_t k = at + static_cast<size_t>(m.length(0));
  while (k < s.size() && std::isspace(static_cast<unsigned char>(s[k]))) ++k;
  if (k < s.size() && s[k] == '[') return "198";
  size_t j = at;
  while (j > 0 && std::isspace(static_cast<unsigned char>(s[j - 1]))) --j;
  if (j == 0 || s[j - 1] != '(') return std::string();
  --j;
  while (j > 0 && std::isspace(static_cast<unsigned char>(s[j - 1]))) --j;
  size_t w = j;
  while (w > 0 && (std::isalnum(static_cast<unsigned char>(s[w - 1])) || s[w - 1] == '_')) --w;
  std::string fn = s.substr(w, j - w);
  for (auto& c : fn) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return (fn == "colsof" || fn == "rowsof" || fn == "el") ? "109" : std::string();
}

std::string absentRMatrixRc(const std::string& s, const std::string& name) {
  return absentClassMatrixRc(s, 'r', name);
}

ExpandResult rcErrorResult(const std::string& message, const std::string& rc) {
  ExpandResult r;
  r.isError = true;
  r.error = nlohmann::ordered_json(
      errorBlock("engine_error", message, std::nullopt, std::nullopt, std::nullopt,
                 "r(" + rc + ")"));
  return r;
}

// The absent r(NAME) rule (internal capture
// RULING.md C.1). Both are asked only for a name in neither rStrings
// nor rValues; a posted r() matrix is never theirs (it has its own 109 below).
//
// Rule 1: Stata posts NAME for the live r-class command and PIET did not
// (EvalSession::rUnposted, recorded by handlers::rTableAfterCommand). The read is
// a named r(111) in every form and on every path -- `.` there would be a silent
// wrong answer (lincom's r(level), pinned 95 at w2h_postest_refit.log:191).
std::optional<std::string> unpostedRRefusal(const EvalSession& session, const std::string& name) {
  if (session.findClassMatrix('r', name) != nullptr) return std::nullopt;
  if (std::find(session.rUnposted.begin(), session.rUnposted.end(), name) ==
      session.rUnposted.end())
    return std::nullopt;
  if (name == "table" && !session.rTableRefusal.empty()) return session.rTableRefusal;
  return "r(" + name + ") after `" + session.rClassCmd +
         "`: Stata posts this result and this version of PIET does not, so it is refused "
         "rather than read as missing (.)";
}

// Rule 2: the live r-class command's Stata set is not recorded (class U), so an
// absent bare or delimited read cannot be told apart from a result PIET omitted.
// The if/while path keeps `.` (it never asks).
std::optional<std::string> unknownSetRRefusal(const EvalSession& session,
                                              const std::string& name) {
  if (!session.rClassSetUnknown || session.findClassMatrix('r', name) != nullptr)
    return std::nullopt;
  return "r(" + name + ") is not set after `" + session.rClassCmd +
         "`, and this version of PIET does not record which results Stata posts there; "
         "refused rather than read as missing (.)";
}

// BUG-629: `local x = r(table)[1,1]` with no r(table) died in expansion, and the
// `= expr` forms answer an expansion error by STORING THE TEXT at rc 0 -- here the
// seven characters `r(table)[1,1]`, a silent wrong value where Stata stops at
// r(198). An expansion error that carries Stata's own code (reason "r(NNN)", only
// the coded refusals above) is surfaced instead; every other expansion error keeps
// the legacy fallback. Gated on err like classifyFailedEqExpr.
bool takeCodedExpandError(const ExpandResult& exp, MacroFnError* err) {
  if (err == nullptr || !exp.isError) return false;
  const std::string reason = exp.error.value("reason", std::string());
  if (reason.size() < 4 || reason.compare(0, 2, "r(") != 0 || reason.back() != ')') return false;
  err->rc = reason.substr(2, reason.size() - 3);
  err->message = exp.error.value("message", std::string());
  return true;
}

}  // namespace

// MOVED OUT of the anonymous namespace 2026-09-06 (BUG-432): the dispatcher's
// row-predicate substitution point needs this exact function, and a second copy
// of a `\b` rule is how two layers start disagreeing about what a bare name is.
// Declared in eval/macros.h; nothing else about it changed.
// `\bNAME\b(?!\s*\()` -> `value`, all occurrences, value inserted literally.
// NAME is a scalar name, so the old code built (and COMPILED) one std::regex
// per scalar per expression — the single most expensive shape a hot loop can
// have. The \b rule is implemented in general form rather than assuming NAME
// begins and ends with a word character, so a store key that somehow does not
// still behaves exactly like the regex.
//
// BUG-512: the scan is a template over WHERE THE VALUE COMES FROM, so `_N` can
// ask for the observation count only once a bare `_N` has actually been found
// (substituteObsSpecials below). Same `\b` rule, still one copy of it; the
// public function is the eager instantiation and is byte-for-byte what it was.
namespace {
template <typename ValueFn>
std::string substituteBareNameWith(const std::string& in, const std::string& name,
                                   ValueFn&& valueOf) {
  if (name.empty()) return in;  // the regex `\b\b(?!...)` case never arises
  const bool nameStartsWord = isIdentChar(name.front());
  const bool nameEndsWord = isIdentChar(name.back());
  std::string out;
  size_t i = 0, last = 0;
  bool any = false;
  while (true) {
    const size_t p = in.find(name, i);
    if (p == std::string::npos) break;
    const size_t e = p + name.size();
    // \b holds iff exactly one side of the position is a word character.
    const bool leftWord = (p > 0) && isIdentChar(in[p - 1]);
    const bool rightWord = (e < in.size()) && isIdentChar(in[e]);
    if (leftWord == nameStartsWord || rightWord == nameEndsWord) {
      i = p + 1;
      continue;
    }
    // (?!\s*\() — a following call-parenthesis spares the name.
    size_t q = e;
    while (q < in.size() && isRegexSpace(in[q])) ++q;
    if (q < in.size() && in[q] == '(') {
      i = p + 1;
      continue;
    }
    out.append(in, last, p - last);
    out.append(valueOf());
    last = e;
    i = e;
    any = true;
  }
  if (!any) return in;
  out.append(in, last, in.size() - last);
  return out;
}
}  // namespace

std::string substituteBareName(const std::string& in, const std::string& name,
                               const std::string& value) {
  return substituteBareNameWith(in, name, [&value]() -> const std::string& { return value; });
}

// name-binding-display-literals: see macros.h.
std::string protectLiterals(const std::string& s, std::vector<std::string>* spans) {
  return protectStringLiterals(s, spans);
}
std::string restoreLiterals(const std::string& s, const std::vector<std::string>& spans) {
  return restoreStringLiterals(s, spans);
}

// FU-842: see macros.h.
std::string valueRefText(const std::string& text) {
  if (text.empty() || text[0] != '-' || !pythonFloat(text).has_value()) return text;
  return "(" + text + ")";
}

// BUG-512: `_N` / `_n` in a MACRO-context expression.
//
// Stata evaluates `local n = _N`, `scalar s = _N`, `global G = _N`,
// `` `=_N-1' `` and `return scalar N = _N` like any other expression, on
// observation 1: `_N` is the observation count and `_n` is 1
// (tests/eval/golden/aer_w2/w2_smalls.log section A -- local_N 12, local_n 1,
// local_N_expr 25, scalar_N 12, inline_N 12|11, global_N 12). Here neither name
// existed below SessionState, so both reached the evaluator as undefined
// identifiers: the `=` forms fell into classifyFailedEqExpr's unclaimed
// bare-identifier arm and stored their own SOURCE TEXT at rc 0 (`_N*2 + 1`),
// `scalar s = _N` stored `_N`, `return scalar N = _N` posted missing, `display
// _N*2` echoed `_N*2`, and the inline form never converged.
//
// ONE substitution, called by both expression entry points of this layer
// (evalArith and evalMacroExpr), which between them are every consumer:
// local/global/scalar `=`, display, `return scalar`, the resampling exp_list and
// every expandMacros call site. A text substitution like `_rc` beside it, and
// for `_rc`'s reason -- `_N*2 + 1` becomes `12*2 + 1` and stays on evalArith's
// fast path, where a namespace binding would have pushed it off.
//
// Quote-aware on its own account (a caller outside this file has not run
// protectStringLiterals): `"_N"` inside a literal is data and stays text.
// The count is PULLED through EvalSession::obsCount, and only once a bare `_N`
// is really there -- `my_N` or `r2_n` must not cost a Datastore open.
// No hook (evalcase harness, session-less callers) = unchanged input.
std::string substituteObsSpecials(const std::string& in, const EvalSession& session) {
  if (!session.obsCount) return in;
  const bool hasBigN = in.find("_N") != std::string::npos;
  const bool hasSmallN = in.find("_n") != std::string::npos;
  if (!hasBigN && !hasSmallN) return in;  // memchr-class, the common case
  std::vector<std::string> spans;
  std::string s = protectStringLiterals(in, &spans);
  if (hasBigN) {
    // BUG-1449: `_N` as a stored result's NAME -- margins' r(_N), e(_N) after
    // `margins, post` -- is not the observation count (StataBE 19.5
    // `colsof(r(_N))` after `margins` is 1): such an `_N` is masked from the
    // count's substitution below and put back after it.
    bool resultN = false;
    for (size_t p = s.find("_N"); p != std::string::npos; p = s.find("_N", p + 1)) {
      size_t q = p;
      while (q > 0 && (s[q - 1] == ' ' || s[q - 1] == '\t')) --q;
      size_t e = p + 2;
      while (e < s.size() && (s[e] == ' ' || s[e] == '\t')) ++e;
      if (q < 2 || s[q - 1] != '(' || (s[q - 2] != 'r' && s[q - 2] != 'e') ||
          (q >= 3 && isIdentChar(s[q - 3])) || e >= s.size() || s[e] != ')')
        continue;
      s.replace(p, 2, "\x1f" "N");
      resultN = true;
    }
    std::string countText;
    bool asked = false;
    s = substituteBareNameWith(s, "_N", [&]() -> const std::string& {
      if (!asked) {
        countText = std::to_string(session.obsCount());
        asked = true;
      }
      return countText;
    });
    if (resultN)
      for (size_t p = s.find("\x1f" "N"); p != std::string::npos; p = s.find("\x1f" "N", p))
        s.replace(p, 2, "_N");
  }
  if (hasSmallN) {
    static const std::string kOne = "1";
    s = substituteBareNameWith(s, "_n", []() -> const std::string& { return kOne; });
  }
  return restoreStringLiterals(s, spans);
}

// _E_SCALAR_KEY (display_hardening.py)
const std::vector<std::pair<std::string, std::string>>& eScalarKey() {
  static const std::vector<std::pair<std::string, std::string>> k = {
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
      // BUG-156 (wiring half, P2-PREP) — the rest of the names, so the Stata
      // spellings stop being unreachable the moment the engine emits the
      // keys. `ll` is already above; `ll_0`/`chi2` happen to map onto their
      // own cobra key names, which is why they need an entry at all (the
      // lookup below only rewrites names it FINDS in this table — an absent
      // name falls through as key==name, so these two are strictly
      // documentation; sigma_u/sigma_e/rho likewise). `r2_p` and `corr` are
      // the two that genuinely rename: poisson's pseudo-R2 and feols'
      // corr(u_i, Xb).
      {"ll_0", "ll_0"},     {"chi2", "chi2"},     {"r2_p", "r2_p"},
      // W1a-5d (2026-09-18): `r2_pseudo` is the SECOND name the same quantity
      // arrives under, and it was absent from this table, so it never reached
      // a STORED estimate's escalars -- `esttab <logit>, pr2` rendered a blank
      // cell. Measured on this engine: poisson publishes `r2_p` (.0126126) and
      // logit/probit publish `r2_pseudo` (.0321488 / .0345996), each leaving
      // the other missing, and nbreg publishes neither. `estimation_m4.cpp:430`
      // already emits it; only the travel was missing.
      {"r2_pseudo", "r2_pseudo"},
      {"sigma_u", "sigma_u"}, {"sigma_e", "sigma_e"}, {"rho", "rho"},
      {"corr", "corr_u_xb"},
  };
  return k;
}

// macros._resolve_est_scalar_refs — exposed (not file-local) for loops.cpp's
// if/while condition evaluator (M3a). Helpers above (resolveBRef,
// resolveEScalar, regexSubFn) stay file-local; only this entry point is
// part of the public macros.h surface.
std::string resolveEstScalarRefs(std::string expr, const EvalSession& session,
                                 std::string* err) {
  dprof::EScope _z(dprof::kEResolveEst);
  // AER scope-2 B2 (2026-09-08): `_b[eq:name]` -- Stata's equation-qualified
  // spelling (`_b[overall:difference]` after oaxaca, `_b[ATE:r1vs0.t]` after
  // teffects). The coeftable row NAME carries the same `eq:name` string, so the
  // lookup stays an exact match; only the reference grammar widens. The twin in
  // display_hardening.cpp (reBRef/reSeRef) MUST stay in step.
  // B2-4 (2026-09-09) widened the tail class by ONE character, `.`: the comment
  // above already named `_b[ATE:r1vs0.t]` as the target spelling, but `[\w:]`
  // has no dot, so the reference fell through to the undefined-matrix arm and
  // came back "matrix _b not found" (r(111)) -- an honest error, and the wrong
  // one. Stata's teffects colname carries the treatment VARIABLE after the
  // r#vs#, so the dot is structural, not decorative.
  // BUG-629 (eval half) widened the HEAD class by the digits and the tail by `#`:
  // a factor-level coefficient is named `2.c` (`0b.c` for the base, `1.c#c.x1`
  // for an interaction cell), so `local q = _b[2.c]` and `generate g = _b[2.c]`
  // fell through to "_b not found" / "unknown variable _b" while `test 2.c` and
  // `lincom _b[2.c]` answered (w2c_postest.log families (2)-(4)). The display
  // twin (display_hardening.cpp reBRef/reSeRef) carries the same classes.
  static const std::regex reB(R"(_b\[([A-Za-z_0-9/\x80-\xff][\w:.#\x80-\xff]*)\])");
  static const std::regex reSe(R"(_se\[([A-Za-z_0-9/\x80-\xff][\w:.#\x80-\xff]*)\])");
  static const std::regex reECall(R"(\be\(\s*([A-Za-z_\x80-\xff][\w\x80-\xff]*)\s*\))");
  // BUG-1518: a `_b[`/`_se[` that ends a longer name is that name's subscript,
  // not a coefficient: `g_b[1]` is observation 1 of the variable g_b (StataBE
  // 19.5: `assert g_b[1] == 1` and `generate z = g_b[1]*3` are rc 0). reB and
  // reSe have no left word boundary, so a match preceded by a name byte is left
  // as it is.
  auto endsName = [](const std::smatch& m) {
    return m.prefix().length() > 0 && isIdentChar(*(m.prefix().second - 1));
  };

  // A3: each of these four ran a full regex scan over EVERY expression, whether
  // or not the text could possibly contain its construct. A profile of a pure
  // macro-arithmetic loop (200k iterations, symbolized `sample`, 2026-08-06) put
  // ~18% of samples inside libc++'s regex engine and ~40% in malloc/free, most
  // of the latter being the regex engine's own per-execution state vectors —
  // so the cheapest scan is the one that never runs. A literal `find()` is a
  // memchr-class loop and is a strict pre-filter: every one of these patterns
  // requires its literal substring to appear, so no match can be missed.
  // (Deliberately NOT a single combined regex: the substitutions must stay
  // ordered and independent, and combining them would be a semantic change
  // dressed as an optimization.)
  if (expr.find(']') != std::string::npos) {
    // `[eq]_b[name]` / `[eq]_se[name]` are `_b[eq:name]` / `_se[eq:name]`
    // (BUG-1328: `[yb]_b[x1]` after logit), read before the `[eq]name` form,
    // which would take `_b` for the coefficient name.
    static const std::regex eqBRef(R"(\[([A-Za-z_\x80-\xff][\w\x80-\xff]*)\]_(b|se)\[([^\]]+)\])");
    expr = regexSubFn(eqBRef, expr, [&session, err](const std::smatch& m) {
      return valueRefText(resolveBRef(m.str(1) + ":" + m.str(3), m.str(2), session, err));
    });
    static const std::regex equationCoef(R"(\[([A-Za-z_\x80-\xff][\w\x80-\xff]*)\]([A-Za-z_0-9\x80-\xff][\w.#\x80-\xff]*))");
    expr = regexSubFn(equationCoef, expr, [&session, err](const std::smatch& m) {
      return valueRefText(resolveBRef(m.str(1) + ":" + m.str(2), "b", session, err));
    });
  }
  // BUG-1545: an ancillary parameter's name can carry parentheses (`_b[/var(e.yc)]`
  // after tobit, StataBE 19.5 1.5244086035135405), which reB's tail class does not
  // take; left as text, the row layer then bound `e` to the variable `eps`
  // (expandRowVarAbbrevs) and the engine refused `_b`. Read before reB.
  if (expr.find("[/") != std::string::npos) {
    static const std::regex reAnc(R"(_(b|se)\[(/[A-Za-z_]\w*\([^()\[\]]*\))\])");
    expr = regexSubFn(reAnc, expr, [&session, err](const std::smatch& m) {
      return valueRefText(resolveBRef(m.str(2), m.str(1), session, err));
    });
  }
  if (expr.find("_b[") != std::string::npos) {
    expr = regexSubFn(reB, expr, [&session, err, &endsName](const std::smatch& m) {
      if (endsName(m)) return m.str(0);
      return valueRefText(resolveBRef(m.str(1), "b", session, err));  // FU-842
    });
  }
  if (expr.find("_se[") != std::string::npos) {
    expr = regexSubFn(reSe, expr, [&session, err, &endsName](const std::smatch& m) {
      if (endsName(m)) return m.str(0);
      return valueRefText(resolveBRef(m.str(1), "se", session, err));  // FU-842
    });
  }
  // scalar(NAME) function form -> the stored scalar value (or the literal
  // token unchanged when undefined).
  if (expr.find("scalar(") != std::string::npos) {
    expr = substituteScalarCalls(expr, session);  // EXPRLAYER-5: was reScalarCall
  }
  // e(name) scalar — NEVER a matrix. The exclusion used to be the literal pair
  // b/V, which was complete while those were the only e-matrices that existed.
  // Once pca posts e(Ev)/e(L)/e(C)/e(Psi)/e(means)/e(sds)/e(components), the
  // literal list silently under-covers: this pass would resolve e(Ev) to
  // MISSING before the matrix pass ever saw it, so `e(Ev)[1,2]` evaluated as
  // `.[1,2]` and `colsof(e(Ev))` as `colsof(.)`. Asking the store instead of a
  // hardcoded pair keeps the same rule ("a matrix is not a scalar") true for
  // every matrix, including ones added later.
  if (hasBareEParen(expr)) {  // EXPRLAYER-3: reECall is \be\( — the \b is the guard
    const std::string whole = expr;
    expr = regexSubFn(reECall, expr, [&session, err, &whole](const std::smatch& m) {
      std::string inner = m.str(1);
      if (inner == "b" || inner == "V") {
        // eb-order T-P1: substituteMatrixRefs ran first and answered el()/
        // colsof()/rowsof() of an e(b)/e(V) that resolves, so one still here
        // is absent. Stata then reads e(b) as an e() macro, and the function
        // gets a string: r(109) "type mismatch", with or without a live e()
        // (w2v_estpost_ematrix.log ep13_el_eb_11 / ep13_colsof_eb after estpost,
        // ep15c_* after pca, ep15f_* with no e()). An e(V) the engine did not
        // send keeps its own named refusal.
        if (err != nullptr && err->empty() && absentClassMatrixRc(whole, 'e', inner) == "109") {
          const EvalSession::EMatrixAnswer a =
              session.resolveEMatrixFor(inner, EvalSession::EReader::kOperand);
          if (!a.matrix.has_value())
            *err = !a.absent ? "r(" + std::to_string(a.rc) + "): " + a.why
                             : "r(109): type mismatch: e(" + inner + ") is not a matrix here";
        }
        return m.str(0);
      }
      if (session.findClassMatrix('e', inner) != nullptr) return m.str(0);
      // W2 predictframe (2026-09-19): e(sample) is Stata's per-ROW estimation
      // sample mask, not a scalar, and this build keeps no such mask. It used to
      // fall through to resolveERef as an ABSENT name -> `.`, and a missing
      // condition is TRUE in every row plane, so after `regress y x2 t if id < 4`
      // (9 in-sample rows) `predict p if e(sample)` filled all 12 at rc 0, and
      // `count`/`gen`/`summarize`/`regress`/`keep if e(sample)` all answered
      // over 12 (measured on the frozen main binary, 2026-09-19; Stata: 9 --
      // esttab-beta.log:488 `predict ... if e(sample)` is the served shape).
      // Refused BY NAME on the same channel as r(111)/r(109) below, so every
      // caller maps it the same way; with no channel the token is left as text
      // for the evaluator to refuse in its own words -- never a silent `.`.
      //
      // e(sample) T2: the mask now exists (SessionState::esampleReading), and
      // every ROW caller rewrites the token before it gets here
      // (dispatcher.cpp rewriteEsampleToken: `(__piet_esample__==SERIAL)` or `0`).
      // What still arrives is a SCALAR context -- display, `local =`, `scalar
      // =`, `if e(sample) {` -- where Stata reads a variable at observation 1
      // (w2_esample.log B16-B19, B29-B29c; 0 with no estimation results,
      // A2-A4). The hook (EvalSession::esampleObs1) answers that value, or the
      // named r(198) for a model PIET has no mask for; with no hook (a
      // session-less harness) it is the same refusal.
      if (inner == "sample") {
        // The session decides (SessionState::esampleReading: no model or a
        // posted e() -> 0, a live mask -> observation 1, a model PIET has no
        // mask for -> the named r(198)). Without a session (a bare evaluator)
        // this layer's own absent-e() predicate, resolveEScalar's, says 0 for
        // "no estimation results" and anything else is refused by name.
        std::string why;
        if (session.esampleObs1) {
          if (std::optional<int> v = session.esampleObs1(&why)) return std::to_string(*v);
        } else if (!session.lastModelResult) {
          return std::string("0");
        } else {
          why = "e(sample) is not available in this version of PIET here: no data session is "
                "attached to evaluate the estimation sample.";
        }
        if (err != nullptr && err->empty()) *err = "r(198): " + why;
        return m.str(0);
      }
      // BUG-456: matrix (above) -> string -> scalar. The string arm is what
      // `local c = e(cmd)`, `if e(cmd)=="regress" {`, `assert e(cmd)=="regress"`
      // and `gen z = (e(cmd)=="regress")` all lacked; every one of them saw `.`.
      return valueRefText(resolveERef(inner, session));  // FU-842
    });
  }
  // BUG-456 follow-up: a NUMBER that has landed in a STRING-typed argument slot
  // is r(109) "type mismatch" (see stringFnNumericArgImpl's measurement block).
  // Reported on the SAME `err` channel as the r(111) above, distinguished by an
  // explicit `r(NNN): ` prefix that splitErrCode() hands the callers -- an
  // r(111) already found wins, it was raised earlier in the expression.
  if (err != nullptr && err->empty()) {
    std::string call;
    if (stringFnNumericArgImpl(expr, &call)) *err = stringSlotMismatchError(call);
  }
  return expr;
}

// The same test, exposed for the ROW/GEN planes. They cannot rely on the call
// above: a PRESENT e-scalar is substituted by the MACRO expansion layer before
// the dispatcher ever sees the line (macros.cpp expandPass), so by then the
// text reads `strpos(20.0,"2")` with no `e(` left in it -- the dispatcher's
// `wantsEst` prefilter is false and the resolver never runs. Measured with a
// debug print at the row call site (2026-09-09): `count if strpos(e(N),"2")`
// arrives as `strpos(20.0,"2")`, while `strpos(e(nosuch),"x")` (an ABSENT name,
// which the macro layer leaves alone) still arrives unresolved. One test, two
// entry points -- never a second copy of the rule.
bool stringFnNumericArg(const std::string& expr, std::string* call) {
  return stringFnNumericArgImpl(expr, call);
}

std::string stringSlotMismatchError(const std::string& call) {
  return "r(109): type mismatch: `" + call + "` passes a number where a string is required";
}

// BUG-456 follow-up: the resolver's `err` channel carries TWO codes now -- the
// historical r(111) (unknown coefficient name, no prefix) and the r(109) the
// string-slot type test raises with an explicit `r(NNN): ` prefix. One splitter
// so the five call sites (evalArith, `scalar =`, if/while, the row plane, gen)
// cannot drift into two spellings of the same mapping.
std::string splitErrCode(std::string* msg) {
  if (msg == nullptr || msg->rfind("r(", 0) != 0) return "r(111)";
  const size_t close = msg->find(')');
  if (close == std::string::npos || close + 1 >= msg->size() || (*msg)[close + 1] != ':') {
    return "r(111)";
  }
  const std::string digits = msg->substr(2, close - 2);
  if (digits.empty() || digits.find_first_not_of("0123456789") != std::string::npos) {
    return "r(111)";
  }
  const std::string code = msg->substr(0, close + 1);
  *msg = strip(msg->substr(close + 2));
  return code;
}

std::string fmtNumeric(double v) {
  if (std::isnan(v)) return ".";
  if (std::isinf(v)) return v > 0 ? "+Inf" : "-Inf";
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.9g", v);
  return std::string(buf);
}

// ---------------------------------------------------------------------------
// matrix_op.substitute_matrix_refs + get_matrix_element

namespace {

// Throws PyExc on out-of-range (ValueError) — caller treats like Python.
double readMatrixElement(const EvalMatrix* mat, long long i, long long j) {
  if (mat == nullptr) throw PyExc("KeyError: matrix not defined");
  long long nrows = static_cast<long long>(mat->data.size());
  long long ncols = nrows > 0 ? static_cast<long long>(mat->data[0].size()) : 0;
  if (!(1 <= i && i <= nrows)) throw PyExc("ValueError: row index out of range");
  if (!(1 <= j && j <= ncols)) throw PyExc("ValueError: column index out of range");
  return mat->data[static_cast<size_t>(i - 1)][static_cast<size_t>(j - 1)];
}

double getMatrixElement(const EvalSession& session, const std::string& name, long long i,
                         long long j) {
  return readMatrixElement(session.findMatrix(name), i, j);
}

// FU-118: el(M,i,j) is MISSING for an index outside M ([FN] Matrix functions,
// el(): "missing if i or j are out of range"), unlike the M[i,j] subscript,
// which refuses. A 0x0 M (`matrix b = e(b)` after estpost) reads `.` at any
// index (w2v_estpost_ematrix.log ep13_b_11 +1.0000000000000X+3ff). An
// undefined M still throws, so the caller leaves the text and
// unresolvedMatrixOperandError names it. (The same page says el() of a matrix
// that does not exist is missing too, but StataBE refuses it: w2f_residuals.log
// :1261-1267 b560_unknown, `NoSuchMatrix not found`, rc 111.)
double readElFunction(const EvalMatrix* mat, long long i, long long j) {
  if (mat == nullptr) throw PyExc("KeyError: matrix not defined");
  const long long nrows = static_cast<long long>(mat->data.size());
  const long long ncols = nrows > 0 ? static_cast<long long>(mat->data[0].size()) : 0;
  if (!(1 <= i && i <= nrows) || !(1 <= j && j <= ncols))
    return std::numeric_limits<double>::quiet_NaN();
  return mat->data[static_cast<size_t>(i - 1)][static_cast<size_t>(j - 1)];
}

// FU-118 / FU-178 / FU-190: e(b) and e(V) as an OPERAND of el(), colsof() and
// rowsof() in an expression (display, `local =`, `scalar =`). eb-order: the
// operand is EvalSession::resolveEMatrixFor's (reader kOperand), the same one
// source `matrix X = e(b)`, `matrix list` and `: colnames e(b)` read -- a posted
// matrix, else the live fit's block in Stata's order under the closed stripe
// table -- so the two can never disagree. Its two operand-only answers: e(V)
// known only from the standard errors (`diagonalOnly`: el(e(V),i,i) is se_i^2,
// an off-diagonal cell is refused by name, never the silent 0 of FU-98), and an
// absent operand, which the caller reads as Stata's r(109).
// mat() is the matrix, or nullptr with `why` set: rc "198" (a refusal) or ""
// (absent -- the caller applies Stata's absent-operand rc).
struct EOperand {
  std::optional<EvalMatrix> hold;
  bool diagonalOnly = false;
  const EvalMatrix* mat() const { return hold.has_value() ? &*hold : nullptr; }
  std::string rc;   // "198" refused; "" absent (when mat() is null)
  std::string why;
};

EOperand resolveEOperand(const EvalSession& session, const std::string& name) {
  EOperand out;
  EvalSession::EMatrixAnswer a = session.resolveEMatrixFor(name, EvalSession::EReader::kOperand);
  if (a.matrix.has_value()) {
    out.hold = std::move(a.matrix);
    out.diagonalOnly = a.diagonalOnly;
    return out;
  }
  out.why = std::move(a.why);
  if (!a.absent) out.rc = std::to_string(a.rc);
  return out;
}

}  // namespace

// FU-118: the named error for a matrix operand substituteMatrixRefs left in
// `expr` (it answers every operand it can, so one still present cannot be
// read). Before this, the text reached the evaluator as a bare call and came
// back as "NameError: name 'el' is not defined" -- rc 1 on display, r(133)
// "unknown function el()" on `local =`, and the expression TEXT stored at rc 0
// by `scalar =` (FU-348). Returns "r(NNN): message" for the leftmost operand,
// or "" when none is left. The codes:
//   * an e()/r() operand that is absent -- r(109) "type mismatch" inside el(),
//     colsof(), rowsof(): Stata then reads e(V) as an (empty) e() macro, a
//     string where a matrix is required. Pinned: w2l_vce_shape.log:280-283
//     (`se11=type mismatch`, display_rc=109, after a failed fit under
//     `ereturn clear`), and the r() twin's w2c_postest.log:377-384;
//   * a subscript `e(NAME)[i,j]` / `r(NAME)[i,j]`: e() keeps the r(111) of
//     display's own whole-token arm (display_hardening.cpp resolveToken 3b,
//     posted matrices only, W2D-T03), r() keeps absentRMatrixRc's r(198)
//     (w2c_postest.log:363-370); an index outside a present matrix is r(503);
//   * a refusal from resolveEOperand, and an off-diagonal cell of an e(V) that
//     is known only on its diagonal: r(198), named;
//   * el()/colsof()/rowsof() of a bare NAME that is not a matrix: r(111)
//     "NAME not found", StataBE's text and code for el() (w2f_residuals.log
//     :1261-1267 b560_unknown, `NoSuchMatrix not found` / rc 111); colsof()/
//     rowsof() share it (their own rc is unpinned). A NAME that is a scalar is
//     r(109) "type mismatch" for all three (StataBE 19.5, 2026-11-03): scalars
//     and matrices share one namespace, so it is the wrong kind, not absent.
// el() with an index that is not an integer literal (`el(M,1,`z'+1)`,
// `el(vecdiag(M),1,3)`) is not claimed: it keeps the unknown-function r(133)
// classifyFailedEqExpr gives it (w2c_functions.log:110 vecdiag_el_local 133).
static std::string unresolvedMatrixOperandError(const std::string& expr,
                                                const EvalSession& session) {
  static const std::regex reClsEl(
      R"(\bel\s*\(\s*([er])\(\s*([A-Za-z_\x80-\xff][\w\x80-\xff]*)\s*\)\s*,\s*(\d+)\s*,\s*(\d+)\s*\))",
      std::regex::icase);
  static const std::regex reClsDim(
      R"(\b(colsof|rowsof)\s*\(\s*([er])\(\s*([A-Za-z_\x80-\xff][\w\x80-\xff]*)\s*\)\s*\))", std::regex::icase);
  static const std::regex reClsElem(
      R"(\b([er])\(\s*([A-Za-z_\x80-\xff][\w\x80-\xff]*)\s*\)\s*\[\s*(\d+)\s*,\s*(\d+)\s*\])");
  static const std::regex reEl(
      R"(\bel\s*\(\s*([A-Za-z_\x80-\xff][\w\x80-\xff]*)\s*,\s*(\d+)\s*,\s*(\d+)\s*\))", std::regex::icase);
  static const std::regex reDim(R"(\b(colsof|rowsof)\s*\(\s*([A-Za-z_\x80-\xff][\w\x80-\xff]*)\s*\))",
                                std::regex::icase);
  const std::string lower = toLower(expr);
  if (lower.find("el") == std::string::npos && lower.find("sof") == std::string::npos &&
      expr.find('[') == std::string::npos)
    return std::string();
  struct Hit {
    size_t pos = std::string::npos;
    std::string err;
  };
  Hit best;
  const auto consider = [&best, &expr](const std::regex& re,
                                       const std::function<std::string(const std::smatch&)>& fn) {
    std::smatch m;
    if (std::regex_search(expr, m, re)) {
      const size_t pos = static_cast<size_t>(m.position(0));
      if (pos < best.pos) {
        std::string e = fn(m);
        if (!e.empty()) best = Hit{pos, std::move(e)};
      }
    }
  };
  const auto classAbsent = [&session](char cls, const std::string& name, bool subscript) {
    if (cls == 'r') {
      if (auto why = unpostedRRefusal(session, name)) return "r(111): " + *why;
      return subscript ? "r(198): invalid syntax: r(" + name + ") is not a matrix"
                       : std::string("r(109): type mismatch");
    }
    if (subscript)
      return session.lastModelResult.has_value()
                 ? "r(111): matrix e(" + name + ") has not been posted by the last estimation "
                   "in this version of PIET; read single coefficients with _b[name] / _se[name]"
                 : "r(111): matrix e(" + name + ") not found";
    return std::string("r(109): type mismatch");
  };
  consider(reClsEl, [&](const std::smatch& m) -> std::string {
    const char cls = static_cast<char>(std::tolower(static_cast<unsigned char>(m.str(1)[0])));
    const std::string name = m.str(2);
    if (cls == 'r') {
      if (session.findClassMatrix('r', name) != nullptr) return std::string();
      return classAbsent('r', name, false);
    }
    const EOperand op = resolveEOperand(session, name);
    if (op.mat() == nullptr)
      return op.rc.empty() ? classAbsent('e', name, false) : "r(" + op.rc + "): " + op.why;
    if (op.diagonalOnly && m.str(3) != m.str(4))
      return "r(198): e(V)[" + m.str(3) + "," + m.str(4) +
             "] is not available in this version of PIET after `" +
             *session.eStrings.find("cmd") +
             "`: the engine sent the fit's standard errors (the diagonal of e(V)), not its "
             "covariances";
    return std::string();
  });
  consider(reClsDim, [&](const std::smatch& m) -> std::string {
    const char cls = static_cast<char>(std::tolower(static_cast<unsigned char>(m.str(2)[0])));
    const std::string name = m.str(3);
    if (cls == 'r')
      return session.findClassMatrix('r', name) != nullptr ? std::string()
                                                           : classAbsent('r', name, false);
    const EOperand op = resolveEOperand(session, name);
    if (op.mat() == nullptr)
      return op.rc.empty() ? classAbsent('e', name, false) : "r(" + op.rc + "): " + op.why;
    return std::string();
  });
  consider(reClsElem, [&](const std::smatch& m) -> std::string {
    const char cls = m.str(1)[0];
    const std::string name = m.str(2);
    const EvalMatrix* mat = session.findClassMatrix(cls, name);
    if (mat == nullptr) return classAbsent(cls, name, true);
    return "r(503): conformability error";
  });
  consider(reEl, [&](const std::smatch& m) -> std::string {
    if (session.findMatrix(m.str(1)) != nullptr) return std::string();
    if (session.scalars.find(m.str(1)) != nullptr) return "r(109): type mismatch";
    return "r(111): " + m.str(1) + " not found";
  });
  consider(reDim, [&](const std::smatch& m) -> std::string {
    if (session.findMatrix(m.str(2)) != nullptr) return std::string();
    if (session.scalars.find(m.str(2)) != nullptr) return "r(109): type mismatch";
    return "r(111): " + m.str(2) + " not found";
  });
  return best.err;
}

static std::string stataMissingText(double d);

// A matrix cell can hold Stata's missing, and pythonFloatRepr spells that "nan",
// which is a NameError once it is substituted back into the expression. Emit the
// missing TOKEN instead (`.`, or `.a` for a `.a` cell, handlers_m3.cpp stores
// stataMissingWithKind) -- replaceStataMissingLiterals runs after this pass and
// turns it back into the exact double. A decimal spelling of a band value would
// be read by the next layer as a number. Reachable from B3(c) onward because
// `J(3,2,.)` is the corpus's own spelling (24 uses) and paper 049 reads those
// cells straight back. r() values go out through here too.
std::string matrixElementText(double v) {
  if (std::isnan(v) || std::fabs(v) >= kStataMissingSentinel) return stataMissingText(v);
  return pythonFloatRepr(v);
}

std::string substituteMatrixRefs(const std::string& expr, const EvalSession& session) {
  static const std::regex reElem(R"((?:\b|(?=[\x80-\xff]))([A-Za-z_\x80-\xff][\w\x80-\xff]*)\s*\[\s*(\d+)\s*,\s*(\d+)\s*\])");
  static const std::regex reColsof(R"(\bcolsof\s*\(\s*([A-Za-z_\x80-\xff][\w\x80-\xff]*)\s*\))", std::regex::icase);
  static const std::regex reRowsof(R"(\browsof\s*\(\s*([A-Za-z_\x80-\xff][\w\x80-\xff]*)\s*\))", std::regex::icase);
  // AER scope 2 / B3(c): colnumb(M,"name") -> the 1-based column index, or
  // missing when the name is not a column. Paper 020 drives an entire loop off
  // it (`local idx_row = colnumb(RHO_D`dd'_res, "rho_Dy`cc'_Dr`cc'")`). It
  // joins the family here rather than in the function namespace because that is
  // where every other matrix reference in an expression is resolved -- element
  // access, colsof and rowsof are all text substitutions against the session
  // store, and a fourth spelling of the same idea would be a second mechanism.
  // Stata takes either the bare column name or the equation-qualified one
  // ("a" and "eq1:a" both answer 1), so both are compared.
  static const std::regex reColnumb(
      // Custom raw-string delimiter: the pattern contains `)"`, which would close
      // a plain R"( ... )" early.
      R"RX(\bcolnumb\s*\(\s*([A-Za-z_\x80-\xff][\w\x80-\xff]*)\s*,\s*"([^"]*)"\s*\))RX", std::regex::icase);
  // e(<name>)[i,j] / r(<name>)[i,j], and the same wrapper inside colsof/rowsof.
  // These run FIRST because the three bare-name patterns above cannot express
  // them: `\b([A-Za-z_]\w*)\s*\[` stops at the ')' in `e(Ev)[1,1]`, so before
  // AER bundle H there was no syntax at all for reading an element out of a
  // posted class matrix -- pca could publish e(Ev) and nothing could read it.
  static const std::regex reClsElem(
      R"(\b([er])\(\s*([A-Za-z_\x80-\xff][\w\x80-\xff]*)\s*\)\s*\[\s*(\d+)\s*,\s*(\d+)\s*\])");
  static const std::regex reClsColsof(
      R"(\bcolsof\s*\(\s*([er])\(\s*([A-Za-z_\x80-\xff][\w\x80-\xff]*)\s*\)\s*\))", std::regex::icase);
  static const std::regex reClsRowsof(
      R"(\browsof\s*\(\s*([er])\(\s*([A-Za-z_\x80-\xff][\w\x80-\xff]*)\s*\)\s*\))", std::regex::icase);
  // el(NAME,i,j) / el(e(NAME),i,j) / el(r(NAME),i,j) -- Stata's function form
  // of NAME[i,j] (papers 058 and 044). Joins the same family for the reason
  // colsof/rowsof/colnumb do: every matrix reference in an expression is a
  // text substitution against the session store. The class-wrapped form runs
  // first for the same reason reClsElem does: `el(e(Ev),1,2)`'s inner
  // identifier is "e" up to the "(", so the bare pattern below never actually
  // matches it (the literal "," it requires next is a "(" instead), but
  // running the specific form first keeps that guarantee explicit rather than
  // incidental. Both corpus call sites (058's `` `z' ``, 044's r()-matrix
  // index) have already gone through macro expansion by the time evalArith
  // reaches this pass, so both indices are plain integer literals -- only
  // \d+ is accepted here, exactly like reElem. An arithmetic index such as
  // `el(M,1,`z'+1)` is a named omission, not a silent wrong value: it fails
  // to match, is left unsubstituted, and the parser refuses it the same way
  // an out-of-range NAME[i,j] does below.
  static const std::regex reClsEl(
      R"(\bel\s*\(\s*([er])\(\s*([A-Za-z_\x80-\xff][\w\x80-\xff]*)\s*\)\s*,\s*(\d+)\s*,\s*(\d+)\s*\))",
      std::regex::icase);
  static const std::regex reEl(
      R"(\bel\s*\(\s*([A-Za-z_\x80-\xff][\w\x80-\xff]*)\s*,\s*(\d+)\s*,\s*(\d+)\s*\))", std::regex::icase);

  // csdid-ematrix-post: an el() index written as integer arithmetic once the
  // macros are expanded (`el(e(V),`__w',`__w'+1)` -> `el(e(V),13,13+1)`,
  // w2ag_csdid_drdid.do:75/:112, StataBE rc 0 at log ag_defx_vw / ag_cl_vw) is
  // folded to its literal so the passes below read it. Only digits joined by
  // + - * (Stata's precedence: * first); anything else -- `el(vecdiag(M),1,3)`,
  // a name, a function -- is left as it was.
  static const std::regex reElArith(
      R"(\bel\s*\(\s*((?:[er]\(\s*[A-Za-z_\x80-\xff][\w\x80-\xff]*\s*\))|[A-Za-z_\x80-\xff][\w\x80-\xff]*)\s*,\s*([0-9][0-9 +*-]*)\s*,\s*([0-9][0-9 +*-]*)\s*\))",
      std::regex::icase);
  const auto foldIntArith = [](const std::string& s, long long& out) -> bool {
    // a sum of products of non-negative integer literals
    std::string t;
    for (char c : s)
      if (c != ' ') t += c;
    if (t.empty()) return false;
    long long total = 0, prod = 1;
    int sign = 1;
    size_t i = 0;
    bool needNum = true;
    while (i < t.size()) {
      if (needNum) {
        if (!std::isdigit(static_cast<unsigned char>(t[i]))) return false;
        long long v = 0;
        while (i < t.size() && std::isdigit(static_cast<unsigned char>(t[i]))) {
          v = v * 10 + (t[i] - '0');
          if (v > 1000000000LL) return false;
          ++i;
        }
        prod *= v;
        needNum = false;
      } else {
        const char op = t[i++];
        if (op == '*') {
          needNum = true;
        } else if (op == '+' || op == '-') {
          total += sign * prod;
          prod = 1;
          sign = op == '+' ? 1 : -1;
          needNum = true;
        } else {
          return false;
        }
      }
    }
    if (needNum) return false;
    out = total + sign * prod;
    return true;
  };
  const std::string folded =
      regexSubFn(reElArith, expr, [&foldIntArith](const std::smatch& m) -> std::string {
        long long i = 0, j = 0;
        if (!foldIntArith(m.str(2), i) || !foldIntArith(m.str(3), j) || i < 0 || j < 0)
          return m.str(0);
        return "el(" + m.str(1) + "," + std::to_string(i) + "," + std::to_string(j) + ")";
      });

  // eb-order T-P1: an e() matrix is EvalSession::resolveEMatrix's -- a posted
  // one, else e(b)/e(V) in Stata's canonical order from the live fit's own
  // block (w2g_ebmat.log:260 el(e(b),1,1) = _b[x1], :305 colsof(e(b)) = 3) --
  // and an r() matrix the posted store's. Unresolved stays text for the
  // parser to refuse, as before.
  std::string pre = regexSubFn(reClsElem, folded, [&session](const std::smatch& m) -> std::string {
    try {
      const long long i = std::stoll(m.str(3)), j = std::stoll(m.str(4));
      const EvalMatrix* mat = nullptr;
      EOperand op;
      if (m.str(1)[0] == 'e') {
        op = resolveEOperand(session, m.str(2));
        if (op.diagonalOnly && i != j) return m.str(0);  // a covariance the engine did not send
        mat = op.mat();
      } else {
        mat = session.findClassMatrix(m.str(1)[0], m.str(2));
      }
      double val = readMatrixElement(mat, i, j);
      return valueRefText(matrixElementText(val));  // FU-842
    } catch (const PyExc&) {
      return m.str(0);  // KeyError/ValueError — leave unchanged, as below
    } catch (const std::exception&) {
      return m.str(0);
    }
  });
  // FU-118: an e() operand of el()/colsof()/rowsof() is resolveEOperand's (a
  // posted matrix, else e(b)/e(V) built from the live fit under Stata's rules);
  // an r() operand is the posted store's. An operand left unsubstituted is
  // named by unresolvedMatrixOperandError, never left for the evaluator's
  // NameError. The subscript form above stays posted-only (W2D-T03).
  pre = regexSubFn(reClsEl, pre, [&session](const std::smatch& m) -> std::string {
    try {
      const long long i = std::stoll(m.str(3)), j = std::stoll(m.str(4));
      if (std::tolower(static_cast<unsigned char>(m.str(1)[0])) == 'e') {
        const EOperand op = resolveEOperand(session, m.str(2));
        if (op.mat() == nullptr || (op.diagonalOnly && i != j)) return m.str(0);
        return valueRefText(matrixElementText(readElFunction(op.mat(), i, j)));  // FU-842
      }
      const EvalMatrix* mat = session.findClassMatrix(m.str(1)[0], m.str(2));
      return valueRefText(matrixElementText(readElFunction(mat, i, j)));  // FU-842
    } catch (const PyExc&) {
      return m.str(0);  // undefined matrix — leave unchanged, as below
    } catch (const std::exception&) {
      return m.str(0);
    }
  });
  const auto classDims = [&session](const std::smatch& m, bool cols) -> std::string {
    EOperand op;
    const EvalMatrix* mat = nullptr;
    if (std::tolower(static_cast<unsigned char>(m.str(1)[0])) == 'e') {
      op = resolveEOperand(session, m.str(2));
      mat = op.mat();
    } else {
      mat = session.findClassMatrix(m.str(1)[0], m.str(2));
    }
    if (mat == nullptr) return m.str(0);
    if (!cols) return pythonFloatRepr(static_cast<double>(mat->data.size()));
    long long ncols = mat->data.empty() ? 0 : static_cast<long long>(mat->data[0].size());
    return pythonFloatRepr(static_cast<double>(ncols));
  };
  pre = regexSubFn(reClsColsof, pre,
                   [&classDims](const std::smatch& m) { return classDims(m, true); });
  pre = regexSubFn(reClsRowsof, pre,
                   [&classDims](const std::smatch& m) { return classDims(m, false); });

  pre = regexSubFn(reColnumb, pre, [&session](const std::smatch& m) -> std::string {
    const EvalMatrix* mat = nullptr;
    for (const auto& p : session.matrices)
      if (p.first == m.str(1)) { mat = &p.second; break; }
    if (mat == nullptr) return m.str(0);  // unknown matrix: leave for the parser to reject
    const std::string want = m.str(2);
    // colnumb(M, "") is 1 in StataBE 19.5 (w2x_fn_string_math fx_colnumb_empty).
    if (want.empty() && !mat->data.empty() && !mat->data[0].empty()) return pythonFloatRepr(1.0);
    for (size_t i = 0; i < mat->colnames.size(); ++i) {
      const std::string& c = mat->colnames[i];
      if (c == want) return pythonFloatRepr(static_cast<double>(i + 1));
      const size_t colon = c.find(':');
      if (colon != std::string::npos && c.substr(colon + 1) == want)
        return pythonFloatRepr(static_cast<double>(i + 1));
    }
    return ".";  // a name that is not a column is MISSING, not an error
  });
  // rownumb(M,"name"): colnumb's row twin, on the row names (w2j_e3_qreg2.do's
  // `rownumb(_e3_T, "df")` on a copy of margins' r(table): StataBE 7, M01 log:2915).
  static const std::regex reRownumb(
      R"RX(\brownumb\s*\(\s*([A-Za-z_\x80-\xff][\w\x80-\xff]*)\s*,\s*"([^"]*)"\s*\))RX", std::regex::icase);
  pre = regexSubFn(reRownumb, pre, [&session](const std::smatch& m) -> std::string {
    const EvalMatrix* mat = nullptr;
    for (const auto& p : session.matrices)
      if (p.first == m.str(1)) { mat = &p.second; break; }
    if (mat == nullptr) return m.str(0);  // unknown matrix: leave for the parser to reject
    const std::string want = m.str(2);
    if (want.empty() && !mat->data.empty()) return pythonFloatRepr(1.0);
    for (size_t i = 0; i < mat->rownames.size(); ++i) {
      const std::string& c = mat->rownames[i];
      if (c == want) return pythonFloatRepr(static_cast<double>(i + 1));
      const size_t colon = c.find(':');
      if (colon != std::string::npos && c.substr(colon + 1) == want)
        return pythonFloatRepr(static_cast<double>(i + 1));
    }
    return ".";
  });

  std::string out = regexSubFn(reElem, pre, [&session](const std::smatch& m) -> std::string {
    try {
      double val = getMatrixElement(session, m.str(1), std::stoll(m.str(2)), std::stoll(m.str(3)));
      return valueRefText(matrixElementText(val));  // FU-842
    } catch (const PyExc&) {
      return m.str(0);  // KeyError/ValueError — leave unchanged
    } catch (const std::exception&) {
      return m.str(0);  // stoll overflow etc. — leave unchanged
    }
  });
  out = regexSubFn(reEl, out, [&session](const std::smatch& m) -> std::string {
    try {
      double val = readElFunction(session.findMatrix(m.str(1)), std::stoll(m.str(2)),
                                  std::stoll(m.str(3)));  // FU-118: out of range -> missing
      return valueRefText(matrixElementText(val));  // FU-842
    } catch (const PyExc&) {
      return m.str(0);  // KeyError/ValueError — leave unchanged
    } catch (const std::exception&) {
      return m.str(0);  // stoll overflow etc. — leave unchanged
    }
  });
  // mreldif(X,Y): max_ij |x_ij - y_ij| / (|y_ij| + 1) over two named matrices
  // (w2j_e3_rlasso.do:406 D1_seed_invariant, mreldif of two tempname copies of
  // e(b)/e(Psi): StataBE 0). Unknown names are left for the parser to refuse;
  // matrices of different shape are Stata's r(503), left the same way.
  static const std::regex reMreldif(
      R"(\bmreldif\s*\(\s*([A-Za-z_\x80-\xff][\w\x80-\xff]*)\s*,\s*([A-Za-z_\x80-\xff][\w\x80-\xff]*)\s*\))", std::regex::icase);
  if (out.find("reldif") != std::string::npos) {
    out = regexSubFn(reMreldif, out, [&session](const std::smatch& m) -> std::string {
      const EvalMatrix* x = session.findMatrix(m.str(1));
      const EvalMatrix* y = session.findMatrix(m.str(2));
      if (x == nullptr || y == nullptr || x->data.size() != y->data.size()) return m.str(0);
      double mx = 0.0;
      for (size_t i = 0; i < x->data.size(); ++i) {
        if (x->data[i].size() != y->data[i].size()) return m.str(0);
        for (size_t j = 0; j < x->data[i].size(); ++j) {
          const double a = x->data[i][j];
          const double b = y->data[i][j];
          if (std::isnan(a) && std::isnan(b)) continue;
          const double d = std::fabs(a - b) / (std::fabs(b) + 1.0);
          if (std::isnan(d)) return std::string(".");
          if (d > mx) mx = d;
        }
      }
      return valueRefText(matrixElementText(mx));
    });
  }
  out = regexSubFn(reColsof, out, [&session](const std::smatch& m) -> std::string {
    const EvalMatrix* mat = session.findMatrix(m.str(1));
    if (mat == nullptr) return m.str(0);
    long long ncols = mat->data.empty() ? 0 : static_cast<long long>(mat->data[0].size());
    return pythonFloatRepr(static_cast<double>(ncols));  // repr(float(ncols))
  });
  out = regexSubFn(reRowsof, out, [&session](const std::smatch& m) -> std::string {
    const EvalMatrix* mat = session.findMatrix(m.str(1));
    if (mat == nullptr) return m.str(0);
    return pythonFloatRepr(static_cast<double>(mat->data.size()));
  });
  return out;
}

// ---------------------------------------------------------------------------
// has_macro_refs

bool hasMacroRefs(const std::string& rawLine) {
  // A3: every macro reference this function can report needs one of exactly
  // three characters to be present — a backtick (`local'), a dollar ($global,
  // ${global}), or a parenthesis (r()/e()/s() refs, whose patterns all match
  // `X(`). A line with none of them cannot contain a reference, and the seven
  // regex searches below were being run on such lines anyway. The guard is a
  // three-character scan and it can only skip work the regexes would have
  // reported false for.
  if (rawLine.find_first_of("`$(") == std::string::npos) return false;
  // LBL-DOLLAR: `\$` is Stata's escape for a literal dollar (the backslash goes):
  // `label var m "Risk Aversion \$(r)\$"` stores `Risk Aversion $(r)$`
  // (w2ao_label_family.log:176). A line carrying one is the expander's to rewrite.
  if (rawLine.find("\\$") != std::string::npos) return true;
  // P2-PERF ARC-A/A3-b: the same pre-filter argument, applied PER PATTERN
  // rather than once for all seven. The blanket guard above lets a line
  // through on any one of three characters and then runs all seven regexes,
  // so `local acc = \`acc' + \`i'` — one backtick, no dollar, no paren — paid
  // for the two global patterns, the three class-reference patterns and the
  // arithmetic-macro pattern as well as the one that could match. Each guard
  // below is strict: the pattern's own literal must be present for it to have
  // any chance of matching.
  const bool tick = rawLine.find('`') != std::string::npos;
  const bool dollar = rawLine.find('$') != std::string::npos;
  std::smatch m;
  if (tick && hasLocalMacroRef(rawLine)) return true;  // EXPRLAYER-7: was reLocalRef()
  // EM3a: `macval(NAME)' is a local reference too (expandPass splices it).
  if (tick && rawLine.find("`macval(") != std::string::npos) {
    for (size_t t = rawLine.find("`macval("); t != std::string::npos;
         t = rawLine.find("`macval(", t + 1)) {
      size_t j = t + 8;
      const size_t st = j;
      while (j < rawLine.size() && isIdentChar(rawLine[j])) ++j;
      if (j > st && j + 1 < rawLine.size() && rawLine[j] == ')' && rawLine[j + 1] == '\'')
        return true;
    }
  }
  if (dollar && hasGlobalRef(reGlobalRefBrace(), rawLine)) return true;  // Unicode names
  if (dollar && hasGlobalRef(reGlobalRefBare(), rawLine)) return true;
  // EXPRLAYER-3: the guards are the patterns' own `\b` requirement, not the
  // bare two-character literal — "r(" and "e(" both occur inside ordinary
  // identifiers (`scalar(`, `subinstr(`, `true(`, `size(`), and every one of
  // those was buying a full regex scan.
  if (hasBareRParen(rawLine) && std::regex_search(rawLine, m, reRRef())) return true;
  if (hasBareEParen(rawLine) && std::regex_search(rawLine, m, reERef())) return true;
  if (tick && rawLine.find("s(") != std::string::npos &&
      std::regex_search(rawLine, m, reSRefDelim()))
    return true;
  if (tick && rawLine.find("`c(") != std::string::npos &&
      std::regex_search(rawLine, m, reCRefDelim()))
    return true;  // BUG-1006
  if (rawLine.find("`=") != std::string::npos && std::regex_search(rawLine, m, reArithMacro()))
    return true;
  if (rawLine.find("`:") != std::string::npos &&
      findReadyInlineMacroFn(rawLine, rawLine.size(), nullptr) != std::string::npos)
    return true;  // BUG-629, BUG-719, EM13, BUG-1384
  return false;
}

// ---------------------------------------------------------------------------
// EM3a/EM3b: verbatim sentinels and compound-quoted spans (see macros.h).

bool hasVerbatim(const std::string& s) {
  return s.find_first_of(std::string{kVerbatimTick, kVerbatimDollar}) != std::string::npos;
}

std::string encodeVerbatim(const std::string& s) {
  if (s.find_first_of("`$") == std::string::npos) return s;
  std::string out = s;
  for (char& c : out) {
    if (c == '`') c = kVerbatimTick;
    else if (c == '$') c = kVerbatimDollar;
  }
  return out;
}

std::string decodeVerbatim(std::string s) {
  if (!hasVerbatim(s)) return s;
  for (char& c : s) {
    if (c == kVerbatimTick) c = '`';
    else if (c == kVerbatimDollar) c = '$';
  }
  return s;
}

bool startsCompound(const std::string& s, size_t i) {
  return i + 1 < s.size() && (s[i] == '`' || s[i] == kVerbatimTick) && s[i + 1] == '"';
}

size_t compoundSpanEnd(const std::string& s, size_t i) {
  if (!startsCompound(s, i)) return std::string::npos;
  int depth = 0;
  size_t j = i;
  while (j < s.size()) {
    if (startsCompound(s, j)) {
      ++depth;
      j += 2;
    } else if (j + 1 < s.size() && s[j] == '"' && s[j + 1] == '\'') {
      j += 2;
      if (--depth == 0) return j;
    } else {
      ++j;
    }
  }
  return std::string::npos;
}

std::string normalizeCompoundQuotes(const std::string& s) {
  if (s.find('"') == std::string::npos) return s;
  if (s.find("`\"") == std::string::npos &&
      s.find(std::string{kVerbatimTick, '"'}) == std::string::npos)
    return s;
  std::string out;
  out.reserve(s.size());
  size_t i = 0;
  while (i < s.size()) {
    if (startsCompound(s, i)) {
      const size_t end = compoundSpanEnd(s, i);
      if (end == std::string::npos) {  // unterminated: left exactly as written
        out.append(s, i, std::string::npos);
        break;
      }
      const std::string body = s.substr(i + 2, end - i - 4);
      if (body.find('"') == std::string::npos) {
        out += '"';
        out += body;
        out += '"';
      } else {
        out.append(s, i, end - i);
      }
      i = end;
      continue;
    }
    if (s[i] == '"') {  // a plain literal: copied through to its closing quote
      const size_t close = s.find('"', i + 1);
      if (close == std::string::npos) {
        out.append(s, i, std::string::npos);
        break;
      }
      out.append(s, i, close + 1 - i);
      i = close + 1;
      continue;
    }
    out += s[i++];
  }
  return out;
}

// ---------------------------------------------------------------------------
// _eval_arith (macros.py)

// A bare `.` and an extended `.a`..`.z` become their exact doubles (exprfns.h
// replaceStataMissingLiterals, shared with loops.cpp and strmutate.cpp) so e.g.
// `substr(s, 3, .)` (missing length == "to end of string") reaches fnSubstr as a
// huge length and arithmetic on missing reaches pyeval's collapse rather than a
// Python SyntaxError. BUG-249: each kind keeps its own double, so string(.a)
// answers `.a`.
static std::string replaceBareDotMissing(const std::string& s) {
  return replaceStataMissingLiterals(s);
}

// BUG-249: the way OUT of the evaluator for a kind-carrying missing — `.a`
// renders `.a`, anything else in the missing band renders `.` (including the
// negated band, which Stata shows as `.`; see isStataMissingResult below).
static std::string stataMissingText(double d) {
  auto kind = stataExtendedMissingKind(d);
  if (kind && *kind >= 1) return std::string(".") + static_cast<char>('a' + *kind - 1);
  return ".";
}

// BUG-660: evalArith's non-integer-real return uses pythonFloatRepr —
// Python's shortest-round-trip repr(), up to 17 significant digits — for
// BOTH its fast-arithmetic and extended (function-call) paths, and stays
// round-trip-exact THERE (see evalArith's own comment: `scalar s = expr`
// needs the full precision, since std::stod reads it back at full width
// downstream). Stata's own local/global `= <realexpr>` STORAGE is not
// round-trip-exact, though: it caps at 16 significant digits (verified
// against four independent w2c_functions.log macro arms with genuinely
// irrational values — sqrt_macro, normal_macro, invt/invttail/ttail's df1
// and df10 points — every one needs exactly 16 sig figs, never 17) and,
// like every other Stata real renderer in this file (see gFormat above),
// drops the leading `0` before the decimal point (`.33`, not `0.33`).
// `%.16g` already implements Python's own fixed/scientific threshold (switch
// to scientific at exponent>=precision), so this is gFormat's own "strip one
// leading 0" idiom at a different precision, not a new algorithm. Applied at
// the local/global STORE call sites via capNumericTextToStataLocalDigits
// below (BUG-660 fix round), not inside evalArith itself — a `scalar =` call
// site shares evalArith and must NOT be capped (see that function's
// evalArith(d) comment for the regression this move fixes).
//
// Fix round 2: ALSO applied to evalMacroExpr (the `` `=expr' `` twin) below,
// for the same reason -- see that function's own comment at its float-return
// arm. The original BUG-660 landing note said this surface stayed on
// pythonFloatRepr because "no oracle arm exercises it with an irrational
// value"; that was true only because postfileMathNamespace() had no Stata
// functions to BE irrational with. This same fix round widened it to
// postfileStataMathNamespace(), which does, so the gap became live.
// Review finding (v3): pythonIntStrFromDouble renders -0.0 as "-0" (%.0f),
// and -0.0 reaches the renderers below from ordinary arithmetic
// (`max(-0.4,-0.5)*0`, `-0.0`, std::round(-0.4)). StataBE 19.5 measured:
// all of these are 0 in local, global and display -- Stata has no negative
// zero. Every integer rendering in this layer goes through here.
static std::string stataIntText(double d) { return pythonIntStrFromDouble(d == 0.0 ? 0.0 : d); }

// Fix round v4 (review): the 16-significant-digit rule above was not Stata's
// rule. StataBE 19.5 measured on 35 values (local, global, `=exp' and
// `=scalar(s)' all agree): the stored text is exactly
// strtrim(string(x, "%18.0g")), i.e. at most 17 characters for the magnitude
// (plus a leading `-`):
//   - fixed point when the integer part has at most 16 digits and, for
//     |x| < 1, when the 16-decimal form keeps at least as many significant
//     digits as e-notation would (.0000333333333333 fixed; 1e-5/3 ->
//     3.33333333333e-06); trailing fraction zeros are dropped, no leading 0;
//   - otherwise e-notation with the mantissa filling the 17 characters
//     (11 decimals for a 2-digit exponent, 10 for a 3-digit one), zeros kept:
//     1e16 -> 1.00000000000e+16, 1e-300 -> 1.0000000000e-300.
// The table-driven selftest arm local_text_rule_18_0g pins every measured row.
static std::string stataLocalRealText(double d) {
  if (d == 0.0) return "0";  // also -0.0: Stata has no negative zero
  const std::string sign = d < 0 ? "-" : "";
  const double a = std::fabs(d);
  char buf[400];
  // e-notation candidate: 12 significant digits, 11 for a 3-digit exponent.
  std::snprintf(buf, sizeof(buf), "%.11e", a);
  std::string eText(buf);
  if (eText.size() > 17) {
    std::snprintf(buf, sizeof(buf), "%.10e", a);
    eText = buf;
  }
  const int eDigits = static_cast<int>(eText.find('e')) - 1;  // mantissa digits (minus the '.')
  if (a < 1.0) {
    std::snprintf(buf, sizeof(buf), "%.16f", a);
    std::string f(buf);  // "0.dddddddddddddddd" or "1.0000000000000000" after rounding
    if (f[0] == '0') {
      const size_t firstNz = f.find_first_not_of('0', 2);
      const int fixedSig = firstNz == std::string::npos ? 0 : static_cast<int>(f.size() - firstNz);
      if (fixedSig < eDigits) return sign + eText;
      f.erase(f.find_last_not_of('0') + 1);
      if (f.back() == '.') f.pop_back();
      return sign + f.substr(1);
    }
    return sign + "1";  // rounded up to 1 at 16 decimals
  }
  // |x| >= 1: integer digits k (after rounding at 16-k decimals) must be <= 16.
  std::snprintf(buf, sizeof(buf), "%.0f", std::floor(a));
  const int k = static_cast<int>(std::strlen(buf));
  if (k > 16) return sign + eText;
  std::snprintf(buf, sizeof(buf), "%.*f", 16 - k, a);
  std::string f(buf);
  const size_t dot = f.find('.');
  if ((dot == std::string::npos ? f.size() : dot) > 16) return sign + eText;
  if (dot != std::string::npos) {
    f.erase(f.find_last_not_of('0') + 1);
    if (f.back() == '.') f.pop_back();
  }
  return sign + f;
}

// BUG-920: forvalues stores each loop value as macro text by this same rule
// (oracle w2_prog_controlflow pc_fv_frac: 0(0.1)1 assigns `.3`, not
// `0.30000000000000004`, and `.7999999999999999`). loops.cpp declares this
// wrapper itself (macros.h is not the loop runner's to widen), so the loop
// and the local/global store share one rule instead of a second copy of it.
std::string stataMacroRealText(double d) { return stataLocalRealText(d); }

// BUG-660 fix round: the local/global STORAGE sites apply Stata's macro
// real-to-text rule (stataLocalRealText, %18.0g) on top of evalArith's own
// round-trip-exact text, instead of evalArith doing it for every caller (see
// evalArith's own comment on its two float-return arms). Re-parses `text`
// (evalArith's numeric output, pythonFloatRepr-formatted) and re-renders it;
// anything that does not parse as a bare finite double whole-string — a
// STRING result, a missing token (".", ".a") — is returned unchanged, since
// only a genuinely computed real needs the rule.
static std::string capNumericTextToStataLocalDigits(const std::string& text) {
  // An INTEGER rendering of at most 16 digits is already Stata's text
  // (%18.0g keeps up to 16 integer digits). Longer integers are re-rendered:
  // StataBE stores `local x = 1e16` as 1.00000000000e+16 (fix round v4).
  {
    const size_t start = (!text.empty() && text[0] == '-') ? 1 : 0;
    if (text.size() > start && text.size() - start <= 16 &&
        text.find_first_not_of("0123456789", start) == std::string::npos) {
      return text;
    }
  }
  // strtod, not std::stod: stod throws out_of_range on a subnormal (ERANGE),
  // and `local x = 5e-324` is an ordinary value (StataBE: 4.9406564584e-324).
  if (text.empty() || std::isspace(static_cast<unsigned char>(text[0]))) return text;
  char* end = nullptr;
  const double d = std::strtod(text.c_str(), &end);
  if (end != text.c_str() + text.size() || !std::isfinite(d)) return text;
  return stataLocalRealText(d);
}

// BUG-1006: a numeric r()/e() spliced by MACRO expansion (`` `r(N)' ``,
// `` `e(r2)' ``) is macro text, so it takes the same %18.0g rule as a stored
// local: count's r(N) is `8`, not `8.0` (w2aq_histogram_family.log every
// hk_*_g record's nonmissing field; w2af_macro_text_file.log mt_num_r_count
// `<8>`, mt_num_r_summean `<6.791666666666667>`, mt_num_e_r2
// `<.0004984216647284>`). A missing value stays its token. The BARE r(N) is an
// expression operand and keeps matrixElementText.
static std::string macroRealText(double v) {
  if (std::isnan(v) || std::fabs(v) >= kStataMissingSentinel) return stataMissingText(v);
  return stataLocalRealText(v);
}

// BUG-196: inside the evaluator missing is the band |d| >= 2^1023 (exprfns.h);
// on the way out it has to be a token again. MAGNITUDE, not signed value: Stata
// renders `-.` as `.` (internal capture missing_range_oracle.log,
// row `negated-dot`), and an out-of-range result is `.` on either side
// (`scalar h8 = -1e308`, w2e_float_followups.log:1770). `2*.` and inf are
// admitted too (row `dot-times-two`). c(maxdouble) is one ulp under 2^1023 and
// stays a real number (rows sentinel-exact / sentinel-negated). NaN compares
// false and is handled by its own paths.
//
// Applied at all three of evalArith's exits (fast path, bare-dot retry,
// extended path), so a band value never leaves as a decimal.
static bool isStataMissingResult(double d) {
  return std::fabs(d) >= kStataMissingSentinel;
}

// Review finding (v2): defined in exprfns.cpp next to runiform()'s draw
// helper; forward-declared here for the same reason addStataMacroOnlyFns is
// (piet/eval/exprfns.h is outside this task's write manifest).
void addSessionRngFns(PyNamespace& ns, EvalSession& session);

// BUG-718: defined with the extended macro functions below. BUG-710: with
// `outerSpans`, evalArith's mode -- see the definition.
namespace {
std::string substituteVarSubscripts(const std::string& in, EvalSession& session,
                                    std::optional<std::string>* whole,
                                    bool* stringLeft = nullptr,
                                    std::vector<std::string>* outerSpans = nullptr);

// BUG-710: a STRING cell (or a string scalar's value) read into an expression
// becomes a literal the evaluator lexes itself: pushed onto the caller's
// protectStringLiterals spans and referenced by its `"0L<k>L0"` placeholder, so
// no later pre-pass reads its content. nullopt for text holding a `"`, which
// this layer's literals cannot carry; the read is then left in place and the
// line is refused downstream, the rule display's bindBareVarsToObs1 keeps.
std::optional<std::string> stringReadPlaceholder(const std::string& text,
                                                 std::vector<std::string>& spans) {
  if (text.find('"') != std::string::npos) {
    // A value holding `"` is carried as Stata's compound-quoted literal `"..."',
    // which the evaluator lexes verbatim (pyeval.cpp, EM3b). 007's Wave 3 reads
    // `local value = G4_1[1]` from a cell holding the survey question, quotes and
    // newlines included, and StataBE takes it. Only text that would itself close
    // or open a compound level (`"'` or `` `" ``) cannot be wrapped and stays
    // refused.
    if (text.find("\"'") != std::string::npos || text.find("`\"") != std::string::npos)
      return std::nullopt;
    spans.push_back("`\"" + text + "\"'");
    return "\"0L" + std::to_string(spans.size() - 1) + "L0\"";
  }
  spans.push_back("\"" + text + "\"");
  return "\"0L" + std::to_string(spans.size() - 1) + "L0\"";
}

// BUG-710: one EvalSession::varObs reading as expression text -- a number
// parenthesised (a negative value stays one operand), a missing value as its
// `.`/`.a` token in parentheses (replaceBareDotMissing turns it into the
// evaluator's double), a string as a literal. Outside 1.._N the value is
// missing: `.` for a numeric column, "" for a string one (Stata [U] 13.7;
// w2i_varsub a_05..a_07, d_03, d_04). nullopt = the text cannot be carried.
std::optional<std::string> varReadText(const EvalSession::VarObsReading& r,
                                       std::vector<std::string>& spans) {
  if (!r.inRange) return r.isString ? stringReadPlaceholder("", spans) : std::string("(.)");
  if (r.isString) return stringReadPlaceholder(r.text, spans);
  if (r.text.empty()) return std::string("(.)");
  return "(" + r.text + ")";
}

// BUG-710: a BARE dataset variable in a scalar expression (`local k = xd`,
// `scalar k2 = xd`, `local ks = s`) is its first observation, as in StataBE
// (w2i_varsub f_01..f_04; `if xd > 0` reads it the same way, loops.cpp
// BUG-921). Every identifier that is not a call (`name(`), a subscript
// (`name[`, substituteVarSubscripts' business), a member or missing-kind
// letter (`.name`, `.a`), or part of a number or a literal placeholder, and that
// EvalSession::varObs knows as a variable, is replaced by varReadText. A name
// the reader does not know is left exactly where it was, so a scalar, an e()
// alias already resolved or an unknown name reaches the evaluator as before.
// Unset hook (session-less harnesses) = unchanged input.
std::string bindBareVariables(const std::string& in, EvalSession& session,
                              std::vector<std::string>& spans) {
  if (!session.varObs) return in;
  std::string out;
  size_t i = 0, last = 0;
  bool any = false;
  while (i < in.size()) {
    const char c = in[i];
    if (c == '"') {  // a placeholder (or an unterminated quote): data
      const size_t close = in.find('"', i + 1);
      i = close == std::string::npos ? in.size() : close + 1;
      continue;
    }
    if (!isIdentStart(c) || (i > 0 && (isIdentChar(in[i - 1]) || in[i - 1] == '.'))) {
      ++i;
      continue;
    }
    size_t j = i + 1;
    while (j < in.size() && isIdentChar(in[j])) ++j;
    size_t k = j;
    while (k < in.size() && (in[k] == ' ' || in[k] == '\t')) ++k;
    if (k < in.size() && (in[k] == '(' || in[k] == '[')) {
      i = j;
      continue;
    }
    const std::string name = in.substr(i, j - i);
    std::optional<EvalSession::VarObsReading> r = session.varObs(name, 1);
    std::optional<std::string> text;
    if (r) text = varReadText(*r, spans);
    if (!text) {
      i = j;
      continue;
    }
    out.append(in, last, i - last);
    out += *text;
    last = j;
    i = j;
    any = true;
  }
  if (!any) return in;
  out.append(in, last, std::string::npos);
  return out;
}
}  // namespace

namespace {
// name-binding-display-literals: two literal forms Stata reads and this
// evaluator's Python lexer does not.
//  * `_pi`, the built-in constant: `local l = _pi`, `scalar s = _pi` and
//    `` `=_pi' `` were r(111) / unexpanded, where Stata reads 3.141592653589793
//    (w2av_name_binding bd_loc_pi, bd_sca_pi, bd_inl_pi, bd_dy_ilpi).
//  * A decimal literal with leading zeros, `05`: Python refuses it, so `local l
//    = 05+1` and `display 05+1` were r(198) where Stata reads 6
//    (bd_loc_lead0, bd_sca_lead0, bd_inl_lead0, bd_dy_lead0). The zeros before
//    another digit are dropped; `0`, `0.5`, `.05` and `1e05` are untouched.
// Quoted text (and a protected literal's placeholder) is data and is skipped.
std::string normalizeStataLiterals(const std::string& in) {
  std::string s = in;
  if (s.find("_pi") != std::string::npos) {
    static const std::string kPiText = pythonFloatRepr(3.14159265358979323846);
    std::vector<std::string> spans;
    s = restoreStringLiterals(substituteBareName(protectStringLiterals(s, &spans), "_pi", kPiText),
                              spans);
  }
  if (s.find('0') == std::string::npos) return s;
  std::string out;
  out.reserve(s.size());
  bool inQuote = false;
  for (size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (c == '"') inQuote = !inQuote;
    if (!inQuote && c == '0' && i + 1 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 1]))) {
      // The first digit of a number: no identifier character or `.` before it,
      // and no digit either (a zero inside `105` is not leading).
      size_t b = out.size();
      const bool startsNumber = b == 0 || !(isIdentChar(out[b - 1]) || out[b - 1] == '.');
      if (startsNumber) continue;  // drop this leading zero; the next digit decides again
    }
    out.push_back(c);
  }
  return out;
}
}  // namespace

namespace {
// name-binding-display-literals: a matrix subscript that names its row or
// column, `M["d1",1]` / `M[1,"x"]`, is the index of that name in the matrix's
// stripe ([P] matrix subscripting; Stata takes the bare name or `eq:name`).
// The literal reaches here as a protected placeholder (`"0L<k>L0"`, its text in
// `spans`); the subscript became `M[1,1]` for the element pass, and a name the
// stripe lacks stays as typed for that pass's refusal. Before, the subscript
// was no element at all and `local l = delta["d1",1]` was r(111) `delta not
// found` where StataBE reads 1 (w2av_name_binding bd_loc_matrow; bd_*_matrowa,
// where a scalar d1 beside the row name d1 must not reach the literal).
std::string resolveMatrixStripeSubscripts(const std::string& in, const EvalSession& session,
                                          const std::vector<std::string>& spans) {
  if (in.find('[') == std::string::npos || in.find("\"0L") == std::string::npos) return in;
  static const std::regex reSub(
      R"RX((?:\b|(?=[\x80-\xff]))([A-Za-z_\x80-\xff][\w\x80-\xff]*)\s*\[\s*("0L\d+L0"|\d+)\s*,\s*("0L\d+L0"|\d+)\s*\])RX");
  const auto stripeIndex = [&spans](const std::string& tok,
                                    const std::vector<std::string>& names) -> std::string {
    if (tok.empty() || tok[0] != '"') return tok;
    const std::string lit = restoreStringLiterals(tok, spans);
    if (lit.size() < 2 || lit.front() != '"' || lit.back() != '"') return std::string();
    const std::string want = lit.substr(1, lit.size() - 2);
    for (size_t k = 0; k < names.size(); ++k) {
      const std::string& n = names[k];
      const size_t colon = n.rfind(':');
      if (n == want || (colon != std::string::npos && n.compare(colon + 1, std::string::npos, want) == 0))
        return std::to_string(k + 1);
    }
    return std::string();
  };
  return regexSubFn(reSub, in, [&](const std::smatch& m) -> std::string {
    if (m.str(2)[0] != '"' && m.str(3)[0] != '"') return m.str(0);
    const EvalMatrix* mat = session.findMatrix(m.str(1));
    if (mat == nullptr) return m.str(0);
    const std::string r = stripeIndex(m.str(2), mat->rownames);
    const std::string c = stripeIndex(m.str(3), mat->colnames);
    if (r.empty() || c.empty()) return m.str(0);
    return m.str(1) + "[" + r + "," + c + "]";
  });
}
}  // namespace

std::optional<std::string> evalArith(const std::string& expr, EvalSession* session,
                                     std::string* raisedError, bool* resultIsString) {
  dprof::EScope _z(dprof::kEEvalArith);
  // name-binding-display-literals: x/0 is missing here, as in Stata (display
  // 1/0 `.`, `local r = 1/0` `.`; w2av_name_binding bd_dy_div0, bd_loc_div0,
  // bd_sca_div0). It was a ZeroDivisionError and so r(198).
  StataDivisionScope stataDivision;
  dprof::ezoneBegin(dprof::kEEvalArithPre);
  std::string cleaned = strip(expr);
  // BUG-501: literals out of harm's way before any pre-pass runs (see
  // protectStringLiterals). No-op for an expression with no quote in it.
  std::vector<std::string> litSpans;
  cleaned = protectStringLiterals(cleaned, &litSpans);
  // FU-875: the blanks inside ( ) and [ ] go, as Stata reads the expression
  // (core/expr_blanks.h): `display abs(1 2)` is 12 and `local m = max(1 2, 3)`
  // 12, as cobra's generate answers them. The literals are placeholders here.
  cleaned = cecon::squeeze_bracketed_blanks(cleaned);
  // ... and brackets that do not pair are r(132) "too few ')' or ']'" before any
  // name is read, as in generate (`display tq(2010q1`, `local m = max(1 2`).
  if (const std::string unbalanced = cecon::bracket_imbalance(cleaned); !unbalanced.empty()) {
    if (raisedError) *raisedError = "r(132): " + unbalanced;
    dprof::ezoneEnd(dprof::kEEvalArithPre);
    return std::nullopt;
  }
  // BUG-1463: a c(name) still here is read through the session's c-class hook
  // (cclass.cpp). display/local/scalar substitute it before they call in, typed
  // (piet::substituteCRefs); the inline `=exp' expansion did not, so `set
  // processors `=min(4, c(processors_max))'' died as "c() is a Stata function
  // this version of PIET does not implement". The hook gives text: a number (or
  // a missing `.`) splices as one, anything else as a string literal.
  if (session != nullptr && session->cValue && cleaned.find("c(") != std::string::npos) {
    static const std::regex reCRef(R"((^|[^\w\x80-\xff.])c\(([A-Za-z_]\w*)\))");
    static const std::regex reNumber(R"(^-?(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?$|^\.[a-z]?$)");
    cleaned = regexSubFn(reCRef, cleaned, [&](const std::smatch& m) -> std::string {
      const std::optional<std::string> v = session->cValue(m.str(2));
      if (!v.has_value()) return m.str(0);
      if (std::regex_match(*v, reNumber)) return m.str(1) + "(" + *v + ")";
      const std::optional<std::string> lit = stringReadPlaceholder(*v, litSpans);
      return lit.has_value() ? m.str(1) + *lit : m.str(0);
    });
  }
  // BUG-446: td()/tc()/tq()... literals become their value (see liftDateLiterals).
  {
    std::string badLiteral;
    cleaned = liftDateLiterals(cleaned, &badLiteral);
    if (!badLiteral.empty()) {
      if (raisedError) *raisedError = "r(198): " + badLiteral + " invalid";
      dprof::ezoneEnd(dprof::kEEvalArithPre);
      return std::nullopt;
    }
  }
  // BUG-512: `_N` / `_n` (see substituteObsSpecials). Ahead of the matrix
  // pre-pass on purpose: that pass only recognises LITERAL subscripts, and an
  // index is an expression like any other.
  if (session != nullptr) cleaned = substituteObsSpecials(cleaned, *session);
  cleaned = normalizeStataLiterals(cleaned);  // `_pi`, `05`
  if (session != nullptr) cleaned = resolveMatrixStripeSubscripts(cleaned, *session, litSpans);
  // BUG-710: a dataset-variable subscript `VAR[i]` is read NOW, through the one
  // cell reader (EvalSession::varObs), and its value travels on as text: `local a
  // = xd[1]` was r(111) (the matrix pass below named xd a missing matrix) and
  // `scalar b = xd[1]` stored the source text `xd[1]`, which every later read
  // re-evaluated against the data of THAT moment -- a replace, a gsort or a
  // restore moved a scalar defined before it (w2i_varsub A-E, i_02..i_20;
  // FU-1324). After `_N`/`_n` so an index may use them, and ahead of the matrix
  // pre-pass, which keeps every name that is a matrix and every two-index
  // subscript (substituteVarSubscripts skips both).
  if (session != nullptr && session->varObs && cleaned.find('[') != std::string::npos)
    cleaned = substituteVarSubscripts(cleaned, *session, nullptr, nullptr, &litSpans);
  // ^ -> ** (Stata exponentiation)
  {
    std::string tmp;
    tmp.reserve(cleaned.size() + 8);
    for (char c : cleaned) {
      if (c == '^') {
        tmp += "**";
      } else {
        tmp.push_back(c);
      }
    }
    cleaned = tmp;
  }
  // \bln\b -> log
  {
    // A3: `ln` is a two-character literal; the regex only fires when it is
    // present, so let a find() decide instead of scanning every expression.
    // Unicode names: identifier-class boundaries (`lnدخل` is a name, not ln).
    static const std::regex reLn(R"((^|[^\w\x80-\xff])ln(?![\w\x80-\xff]))");
    if (cleaned.find("ln") != std::string::npos)
      cleaned = std::regex_replace(cleaned, reLn, "$1log");
  }

  // colnumb(M, "name") reads its name out of a string literal, which
  // protectStringLiterals has already swapped for a `0L<k>L0` placeholder, so
  // substituteMatrixRefs never matched and every colnumb() answered `.`
  // (w2x_fn_string_math fx_colnumb_macro/_fv/_disp/_if). The call's own literal
  // is put back here, and only when the matrix exists -- the substitution then
  // consumes it. colnumb() of a matrix that does not exist is r(111)
  // (fx_colnumb_nomat), never a value.
  if (session != nullptr && !litSpans.empty() &&
      (toLower(cleaned).find("colnumb") != std::string::npos ||
       toLower(cleaned).find("rownumb") != std::string::npos)) {
    static const std::regex reColnumbLit(
        R"RX(\b((?:col|row)numb\s*\(\s*)([A-Za-z_\x80-\xff][\w\x80-\xff]*)(\s*,\s*)("0L\d+L0")(\s*\)))RX", std::regex::icase);
    std::string missingMat;
    cleaned = regexSubFn(reColnumbLit, cleaned, [&](const std::smatch& m) -> std::string {
      if (session->findMatrix(m.str(2)) == nullptr) {
        if (missingMat.empty()) missingMat = m.str(2);
        return m.str(0);
      }
      return m.str(1) + m.str(2) + m.str(3) + restoreStringLiterals(m.str(4), litSpans) + m.str(5);
    });
    if (!missingMat.empty()) {
      if (raisedError) *raisedError = "r(111): matrix " + missingMat + " not found";
      dprof::ezoneEnd(dprof::kEEvalArithPre);
      return std::nullopt;
    }
  }

  // WS-C matrix pre-substitution.
  if (session != nullptr) {
    std::string lower = toLower(cleaned);
    if (cleaned.find('[') != std::string::npos || lower.find("colsof") != std::string::npos ||
        lower.find("rowsof") != std::string::npos || lower.find("colnumb") != std::string::npos ||
        lower.find("rownumb") != std::string::npos ||
        lower.find("el(") != std::string::npos || lower.find("mreldif") != std::string::npos) {
      cleaned = substituteMatrixRefs(cleaned, *session);
      // FU-118: a matrix operand the pass could not answer is a named error
      // here, on the r(NNN) channel every caller already maps (display,
      // `local =`, `scalar =` -- which then creates no scalar, FU-348).
      const std::string matErr = unresolvedMatrixOperandError(cleaned, *session);
      if (!matErr.empty()) {
        if (raisedError) *raisedError = matErr;
        dprof::ezoneEnd(dprof::kEEvalArithPre);
        return std::nullopt;
      }
    }
  }

  // BUG-1419: scalar(NAME) of a STRING scalar is that string, as the bare name
  // is below: `display scalar(s1) + "x"` with s1 = "-3" is -3x in StataBE 19.5
  // (x1b/a1.do C2). Here it is a protected literal (stringReadPlaceholder), so
  // no later pre-pass reads inside it, as resolveEstScalarRefs' raw one would be.
  if (session != nullptr && !session->stringScalars.empty() &&
      cleaned.find("scalar(") != std::string::npos) {
    static const std::regex reStrScalarCall(
        R"((^|[^\w\x80-\xff])scalar\(\s*([A-Za-z_\x80-\xff][\w\x80-\xff]*)\s*\))");
    cleaned = regexSubFn(reStrScalarCall, cleaned, [&](const std::smatch& m) -> std::string {
      const std::string* v = session->scalars.find(m.str(2));
      if (v == nullptr || !session->isStringScalar(m.str(2))) return m.str(0);
      if (auto lit = stringReadPlaceholder(*v, litSpans)) return m.str(1) + *lit;
      return m.str(0);
    });
  }
  if (session != nullptr) {
    std::string bErr;
    cleaned = resolveEstScalarRefs(cleaned, *session, &bErr);
    if (!bErr.empty()) {
      // BUG-456: `_b[nosuch]` is r(111) on the local/scalar/display surfaces
      // too, not a silent `.` -- forwarded on the BUG-246/249 `r(NNN): ...`
      // channel that classifyFailedEqExpr already honours.
      // BUG-456 follow-up: r(111) (unknown coefficient) or r(109) (a number
      // in a string-typed slot); splitErrCode keeps whichever the resolver
      // raised instead of stamping r(111) on both.
      if (raisedError) {
        const std::string code = splitErrCode(&bErr);
        *raisedError = code + ": " + bErr;
      }
      dprof::ezoneEnd(dprof::kEEvalArithPre);
      return std::nullopt;
    }
    // _rc — the bare last-command return code local.
    // A3: same shape — `_rc` cannot match without the literal.
    // Unicode names (lane M): the boundary is the identifier class, not
    // ECMAScript's ASCII `\b` -- `\b_rc\b` matched the tail of a variable
    // named `دخل_rc` (an Arabic byte is a non-word character to `\b`).
    static const std::regex reRc(R"((^|[^\w\x80-\xff])_rc(?![\w\x80-\xff]))");
    std::smatch m;
    if (cleaned.find("_rc") != std::string::npos && std::regex_search(cleaned, m, reRc)) {
      std::string rcv = session->locals.get("_rc", "0");
      if (!pythonFloat(rcv)) rcv = "0";
      // A callback, not a "$1..." format: rcv is digits, and "$10" reads as group 10.
      cleaned = regexSubFn(reRc, cleaned, [&rcv](const std::smatch& mm) { return mm.str(1) + rcv; });
    }
    // Bare scalar-name substitution (insertion order; (?!\s*\() spares
    // same-named function CALLS; value inserted literally).
    dprof::EScope _zs(dprof::kEEvalArithScalars);
    for (const auto& [sn, sv] : session->scalars.items()) {
      if (cleaned.find(sn) != std::string::npos) {
        // BUG-710: a dataset variable wins over a same-named scalar (Stata's
        // rule, which loops.cpp's condition binder and the dispatcher's
        // substituteStoredScalars already keep); bindBareVariables reads it.
        if (session->varObs && session->varObs(sn, 1)) continue;
        // BUG-710: a STRING scalar is a literal, not a bare name: `local x =
        // d_st` substituted `ab2` and stored the source text `d_st`
        // (w2i_varsub d_02).
        if (session->isStringScalar(sn)) {
          if (auto lit = stringReadPlaceholder(sv, litSpans)) {
            const std::string litText = *lit;
            cleaned = substituteBareNameWith(cleaned, sn,
                                             [&litText]() -> const std::string& { return litText; });
            continue;
          }
        }
        // EXPRLAYER-5: was a std::regex CONSTRUCTED (i.e. compiled) per scalar
        // per expression — `\bNAME\b(?!\s*\()` walked directly instead.
        cleaned = substituteBareName(cleaned, sn, valueRefText(sv));  // FU-842
      }
    }
    // BUG-710: a bare dataset variable reads observation 1 (bindBareVariables).
    cleaned = bindBareVariables(cleaned, *session, litSpans);
    // BUG-456 follow-up (2026-09-09): the string-slot type test runs AGAIN here,
    // after the stored scalars have been substituted -- `scalar one = 1` then
    // `local sf = strpos(one,"1")` answered 1 at rc 0 where Stata is r(109)
    // (measured, oracle2.log P10), because the resolver's own copy of this test
    // ran before this loop existed to substitute anything.
    if (raisedError != nullptr && raisedError->empty()) {
      std::string badCall;
      if (stringFnNumericArgImpl(cleaned, &badCall)) {
        *raisedError = stringSlotMismatchError(badCall);
        dprof::ezoneEnd(dprof::kEEvalArithPre);
        return std::nullopt;
      }
    }
  }

  dprof::ezoneEnd(dprof::kEEvalArithPre);

  // Fast path: pure-arithmetic whitelist, EMPTY eval namespace.
  dprof::ezoneBegin(dprof::kEEvalArithWhitelist);
  const bool safeArith = isSafeArith(cleaned);  // EXPRLAYER-2: was reSafeArith()
  dprof::ezoneEnd(dprof::kEEvalArithWhitelist);
  if (safeArith) {
    PyNamespace empty;
    PyEvalOutcome out = evalPyExprDetailed(cleaned, empty);
    if (out.status != PyEvalOutcome::Status::Value) {
      // BUG-196: `display (.)` printed the literal `(.)`.
      //
      // A bare `.` is Stata system-missing and has to reach the evaluator as
      // the missing sentinel, or Python raises SyntaxError on it. That
      // substitution lived below, on the EXTENDED path only — and `.`, `(.)`
      // and `2+.` are built entirely from fast-path characters, so they took
      // THIS branch, failed, returned nullopt, and `display` fell through to
      // its echo-the-literal-text fallback. `display .` looked right the whole
      // time purely because echoing the literal text of `.` happens to be the
      // correct answer; nothing was evaluating it.
      //
      // Applied as a RETRY rather than hoisted above the whitelist fork,
      // because evalArith is a hot path (25.6% of S02's profile) and this
      // substitution allocates and walks char by char, with no cheap guard
      // available — `find('.')` does not discriminate, since ordinary decimal
      // literals contain dots. On this shape the success path pays nothing at
      // all: the only inputs that reach the retry are the ones that return
      // nullopt today, so the change is strictly additive in cost as well as
      // in behaviour.
      //
      // It is additive in behaviour because within the fast path's alphabet,
      // [0-9\s+\-*/().^eE], a bare `.` cannot occur in anything Python
      // evaluates successfully: `.` is legal there only inside a float literal
      // — which replaceBareDotMissing leaves alone, since it rewrites a `.`
      // only when neither neighbour is alnum/`_`/`.`, so `3.14`, `.5` and `1.`
      // all pass through untouched — or as attribute access, which needs an
      // identifier, and the only letters admitted here are e/E, undefined in
      // the empty namespace.
      //
      // The retry is TRUSTED ONLY WHEN IT LANDS ON A CLEAN MISSING. PIET has
      // no missing TYPE — the sentinel is an ordinary double and Python
      // arithmetic treats it as one — so an expression like `./2` comes back
      // as 4.49e+307, a number Stata would never print (Stata says `.`; row
      // `dot-div-two`). Emitting it would trade a visibly-unevaluated echo for
      // a confidently wrong number, which is the worse of the two failures. So
      // anything that is not recognisably missing falls back to the nullopt
      // this branch already returned before BUG-196, leaving those cases
      // exactly as they were. Real missing propagation belongs to a missing
      // type in the expression layer and is not claimed here.
      // BUG-1388: the only names this alphabet admits are runs of e/E, and
      // Stata defines none of them (`local d = e + 1` is `e not found`,
      // r(111)). The NameError goes out on the channel the extended path
      // below uses, so the caller's classifier answers r(111) rather than
      // r(198) invalid syntax. A bare-dot retry cannot cure an unbound name.
      if (raisedError && out.status == PyEvalOutcome::Status::Raised &&
          out.error.rfind("NameError", 0) == 0) {
        *raisedError = out.error;
        return std::nullopt;
      }
      const std::string withMissing = replaceBareDotMissing(cleaned);
      if (withMissing == cleaned) return std::nullopt;  // no bare dot to retry with
      const PyEvalOutcome retry = evalPyExprDetailed(withMissing, empty);
      if (retry.status != PyEvalOutcome::Status::Value) return std::nullopt;
      std::optional<double> rd;
      if (retry.value.isFloat()) rd = retry.value.floatVal();
      else if (retry.value.isInt()) rd = static_cast<double>(retry.value.intVal());
      // BUG-249: kind-preserving — a bare `.e` (the one extended code whose
      // letter is inside the fast alphabet) must come back `.e`, not `.`.
      if (rd && isStataMissingResult(*rd)) return stataMissingText(*rd);
      return std::nullopt;
    }
    const PyValue& r = out.value;
    // Python: isinstance(result,(int,float)) — bool unreachable on this
    // path (no comparison chars in the whitelist).
    if (r.isInt() || r.isBool()) return std::to_string(r.intVal());
    if (r.isFloat()) {
      double d = r.floatVal();
      // The same exit test as the retry and the extended path: a missing result
      // (`-1e308`, r(sd) after meanonly substituted as `.`) leaves as its token.
      if (std::isnan(d) || isStataMissingResult(d)) return stataMissingText(d);
      if (pyIsInteger(d)) return stataIntText(d);
      // BUG-660 fix round: evalArith itself stays ROUND-TRIP-EXACT
      // (pythonFloatRepr) for every caller -- `scalar s = expr` stores this
      // text straight into session.scalars, and program.cpp's return/
      // ereturn scalar and handlers_resample.cpp's exp_list both parse it
      // back with std::stod expecting the full precision. Only local/global
      // storage wants Stata's capped 16-sig-fig macro convention, applied by
      // their own call sites (see capNumericTextToStataLocalDigits below),
      // not here.
      return pythonFloatRepr(d);
    }
    return std::nullopt;
  }

  // Extended path (string fns like substr, math namespace): a bare `.` missing
  // must reach the evaluator as the Stata sentinel, not a Python SyntaxError.
  // Unconditional here — this path is not the hot one, and substr(s, 3, .)
  // depends on it (missing length == "to end of string").
  cleaned = replaceBareDotMissing(cleaned);

  dprof::ezoneBegin(dprof::kEEvalArithWhitelist);
  const bool safeArithExt = isSafeArithExt(cleaned);  // EXPRLAYER-2: was reSafeArithExt()
  dprof::ezoneEnd(dprof::kEEvalArithWhitelist);
  if (!safeArithExt) {
    return std::nullopt;  // contains characters that are unsafe
  }
  // BUG-501: the literals go back BEFORE the evaluator, which lexes them itself.
  cleaned = restoreStringLiterals(cleaned, litSpans);

  // EXPRLAYER-1c: this path added NOTHING to the registry, so it can use the
  // process-wide one directly instead of rebuilding 39 map entries per call.
  PyNamespace extNs;
  const PyNamespace* nsPtr = &stataMathNamespace();
  // BUG-660: fileexists() needs the session's cwd, which the process-wide,
  // session-less registry above cannot close over. Added as a per-call
  // override ONLY when the expression actually names it, so the
  // EXPRLAYER-1c fast path stays untouched (no map rebuild) for every other
  // extended-path call — the same "own map wins over base" lookup order
  // PyNamespace::base documents.
  //
  // Review finding (v2): runiform()/runiformint() likewise need the session
  // (its `set seed`, EvalSession::rngSeed) and get the same per-call install.
  if (session != nullptr && cleaned.find("runiform") != std::string::npos) {
    extNs.base = &stataMathNamespace();
    addSessionRngFns(extNs, *session);
    nsPtr = &extNs;
  }
  if (session != nullptr && cleaned.find("fileexists(") != std::string::npos) {
    extNs.base = &stataMathNamespace();
    const std::string cwd = session->cwd;
    extNs.fns["fileexists"] = [cwd](const std::vector<PyValue>& args) -> PyValue {
      if (args.size() != 1) throw PyExc("fileexists: TypeError arity");
      namespace fs = std::filesystem;
      std::string base = !cwd.empty() ? cwd : fs::current_path().u8string();
      fs::path p(pythonStr(args[0]));
      if (p.is_relative()) p = cecon::to_native_path(base) / p;
      std::error_code ec;
      return PyValue::boolean(fs::exists(p, ec));
    };
    nsPtr = &extNs;
  }
  PyEvalOutcome out = evalPyExprDetailed(cleaned, *nsPtr);
  // BUG-374(3): Python's math functions RAISE for an out-of-domain argument
  // (pyeval's mSqrt/mLog/mLog2/mLog10 throw "ValueError: math domain error");
  // Stata returns MISSING. `display sqrt(-1)` was reported as a parse gap and is
  // not one — the grammar handles unary minus fine (parseFactor), the call just
  // threw, and evalArith forwards only NameError/r(NNN) to the caller, so the
  // ValueError was dropped and display classified the empty error as r(198)
  // "invalid syntax". The engine's own expression evaluator already takes the
  // Stata side of this (frame.cpp's fin(): out-of-domain math becomes missing),
  // so this makes the display layer agree with it.
  if (out.status == PyEvalOutcome::Status::Raised &&
      out.error.find("math domain error") != std::string::npos) {
    return std::string(".");
  }
  if (out.status != PyEvalOutcome::Status::Value) {
    // BUG-088: distinguish a genuine raised error (an undefined name — e.g. an
    // unsupported extended macro function `_request(...)`) from a legitimately
    // non-arithmetic expression. Only a NameError is signalled to the caller; a
    // FnObject, a SyntaxError (`foo bar` trailing tokens), or a bad-arity/other
    // failure stays a silent nullopt so ordinary display output is byte-identical.
    // BUG-246/249 widen the channel: an `r(NNN): message` error is a function
    // announcing Stata's EXACT code (fnString's r(109) type mismatch is the
    // first) — forwarded whole, same contract as the NameError arm.
    if (raisedError && out.status == PyEvalOutcome::Status::Raised &&
        (out.error.rfind("NameError", 0) == 0 || out.error.rfind("r(", 0) == 0)) {
      *raisedError = out.error;
    }
    return std::nullopt;
  }
  const PyValue& r = out.value;
  // Stata fns return Python bools; coerce to 1/0 BEFORE numeric handling.
  if (r.isBool()) return r.boolVal() ? "1" : "0";
  if (r.isInt()) return std::to_string(r.intVal());
  if (r.isFloat()) {
    double d = r.floatVal();
    // BUG-249: a result still in the missing band comes back as its missing
    // TOKEN, kind preserved. Before the kind sentinels existed, `.a` reached
    // Python as a SyntaxError and fell back to the caller's literal echo —
    // rewriting it into a finite double (replaceBareDotMissing above) without
    // this gate would have PRINTED that double (`local x = .a` storing
    // 8.99e+307), a confident wrong number replacing a visible non-answer.
    // Any value in the band |d| >= 2^1023 is missing in StataBE's own
    // representation; c(maxdouble), one ulp below, is a real number.
    if (isStataMissingResult(d)) return stataMissingText(d);
    // BUG-374(2): NaN is Stata MISSING and prints as ".", not "nan". It reaches
    // here from any legitimately-missing result — `display mod(7,0)` is the
    // reported case — and slipped through because isStataMissingResult is
    // magnitude-based (its own comment says NaN escapes it) and pyIsInteger
    // rejects non-finite, so the value fell to pythonFloatRepr, which is
    // faithfully Python and answers "nan". The `` `=expr' `` twin in this same
    // file has always had this arm (see evalMacroExpr's `if (std::isnan(fval))
    // return "."`), so the two expression entry points disagreed about what a
    // missing result looks like.
    if (std::isnan(d)) return ".";
    if (pyIsInteger(d)) return stataIntText(d);
    // BUG-660 fix round: round-trip-exact here too — see the fast-path arm's
    // comment above.
    return pythonFloatRepr(d);
  }
  if (r.isStr()) {
    if (resultIsString != nullptr) *resultIsString = true;  // BUG-293
    return r.strVal();
  }
  return std::nullopt;
}

// BUG-660 fix round: addStataMacroOnlyFns is defined in exprfns.cpp (this
// TU's namespace-mate) but not declared in piet/eval/exprfns.h — that header
// is outside this task's write manifest. Same signature as the definition;
// forward-declared locally rather than left unreachable from here.
void addStataMacroOnlyFns(PyNamespace& ns);

// BUG-660: evalMacroExpr's (the `` `=expr' `` inline form) own combined
// registry — mathNamespace(true) (postfile semantics, inf/nan bound) +
// addStataFns(), the same pairing exprfns.cpp's stataMathNamespace() builds
// for evalArith's withInfNan=false base. Before this, `local x = round(...)`
// and the inline `` `=round(...)' `` disagreed on whether round()/scalar()/
// invttail() etc. existed at all — postfileMathNamespace() alone has none of
// them, so every Stata-function call through the inline form was a
// NameError -> r(133) no matter what the function was. Static/process-wide
// for the same EXPRLAYER-1c reason stataMathNamespace() is: rebuilding
// ~115 fn-map entries on every `` `=expr' `` call would undo the whole
// optimization.
static const PyNamespace& postfileStataMathNamespace() {
  static const PyNamespace kNs = [] {
    PyNamespace n = mathNamespace(/*withInfNan=*/true);
    addStataFns(n);
    addStataMacroOnlyFns(n);  // BUG-660 fix round: sum/group/runiform(int) here only
    return n;
  }();
  return kNs;
}

// ---------------------------------------------------------------------------
// eval_macro_expr (postfile.py)

std::optional<std::string> evalMacroExpr(const std::string& expr, EvalSession& session) {
  dprof::EScope _z(dprof::kEEvalMacroExpr);
  StataDivisionScope stataDivision;  // `=1/0' is `.' (w2av_name_binding bd_inl_div0)
  std::string cleaned = strip(expr);
  if (cleaned.empty()) return std::nullopt;
  // BUG-512: `` `=_N' `` / `` `=_N-1' `` (w2_smalls.log inline_N 12|11). The
  // name passed this function's whitelist and then raised in the evaluator, so
  // the reference was left in the line and expandMacros reported a circular
  // definition after 16 passes -- `forvalues i = 1/`=_N'` died on it.
  cleaned = substituteObsSpecials(cleaned, session);
  cleaned = normalizeStataLiterals(cleaned);  // `_pi`, `05` (name-binding-display-literals)
  // BUG-718: `` `=__d_itrim[1]' `` -- a dataset variable read at an observation
  // (w2c_functions.log itrim_data and twelve sibling `data` records). The name
  // used to reach the whitelist and the evaluator as an unknown identifier, the
  // reference stayed in the line, and expandMacros reported non-convergence
  // after 16 passes (rc 1). After `_N`/`_n` above, so `x[_N]` reads row _N.
  {
    std::optional<std::string> whole;
    cleaned = substituteVarSubscripts(cleaned, session, &whole);
    if (whole) return whole;
  }

  {
    std::string tmp;
    tmp.reserve(cleaned.size() + 8);
    for (char c : cleaned) {
      if (c == '^') {
        tmp += "**";
      } else {
        tmp.push_back(c);
      }
    }
    cleaned = tmp;
  }
  {
    // A3: `ln` is a two-character literal; the regex only fires when it is
    // present, so let a find() decide instead of scanning every expression.
    // Unicode names: identifier-class boundaries (`lnدخل` is a name, not ln).
    static const std::regex reLn(R"((^|[^\w\x80-\xff])ln(?![\w\x80-\xff]))");
    if (cleaned.find("ln") != std::string::npos)
      cleaned = std::regex_replace(cleaned, reLn, "$1log");
  }

  {
    std::string lower = toLower(cleaned);
    if (cleaned.find('[') != std::string::npos || lower.find("colsof") != std::string::npos ||
        lower.find("rowsof") != std::string::npos || lower.find("el(") != std::string::npos ||
        lower.find("mreldif") != std::string::npos) {
      cleaned = substituteMatrixRefs(cleaned, session);
    }
  }

  // Namespace: postfile._MATH_NS (has inf/nan) + session scalars + r()
  // values as bare float names. dict-update order means a scalar named
  // after a function SHADOWS the function (consts win in evalPyExpr).
  // EXPRLAYER-1c: registry by reference; the per-call scalar/r() bindings go
  // into the own map, which outranks the base — the same shadowing the
  // dict-update order produced ("a scalar named after a function SHADOWS the
  // function": a const always outranks a fn, at any level).
  PyNamespace ns;
  ns.base = &postfileStataMathNamespace();  // BUG-660: was postfileMathNamespace() (no Stata fns)
  // Review finding (v2): session-bound runiform()/runiformint() (see
  // evalArith's own install for the rationale).
  if (expr.find("runiform") != std::string::npos) addSessionRngFns(ns, session);
  for (const auto& [sname, sval] : session.scalars.items()) {
    // name-binding-display-literals: a dataset variable (or a unique
    // abbreviation of one) wins over a same-named scalar, as in evalArith; the
    // name is left unbound here, so the expression falls to expandPass's
    // evalArith fallback, which reads the variable (w2av_name_binding
    // bd_inl_varsc `=v'` 10, not scalar v's 5; bd_inl_abbrsc `=lngd'`).
    if (session.varObs && cleaned.find(sname) != std::string::npos && session.varObs(sname, 1))
      continue;
    auto f = pythonFloat(sval);
    if (f) ns.consts[sname] = PyValue::floating(*f);
  }
  for (const auto& [rname, rval] : session.rValues.items()) {
    ns.consts[rname] = PyValue::floating(rval);
  }

  // Whitelist on the SPACE-stripped text (spaces only — not tabs; mirrors
  // cleaned.replace(" ", "")).
  {
    std::string nospace;
    nospace.reserve(cleaned.size());
    for (char c : cleaned) {
      if (c != ' ') nospace.push_back(c);
    }
    std::smatch wm;
    if (!std::regex_match(nospace, wm, rePostfileSafeArith())) return std::nullopt;
  }

  PyEvalOutcome out = evalPyExprDetailed(cleaned, ns);
  if (out.status == PyEvalOutcome::Status::Raised) return std::nullopt;
  if (out.status == PyEvalOutcome::Status::FnObject) {
    // Python: `return str(result)` — a bare builtin name evaluates to the
    // function object; str() of the math builtins is deterministic.
    return "<built-in function " + out.fnName + ">";
  }
  const PyValue& r = out.value;
  // bool -> int, then int/float formatting.
  if (r.isBool()) return r.boolVal() ? "1" : "0";
  if (r.isInt() || r.isFloat()) {
    double fval = r.isFloat() ? r.floatVal() : static_cast<double>(r.intVal());
    if (std::isnan(fval)) return ".";
    // A finite band value (pyeval's collapse and result rule return 2^1023)
    // is missing; printed as a decimal it would be a number.
    if (std::isfinite(fval) && std::fabs(fval) >= kStataMissingSentinel) return stataMissingText(fval);
    // Python: `if fval == int(fval) and not isinf(fval)` — int(inf) raises
    // OverflowError BEFORE the isinf guard, and the whole body sits in a
    // broad try/except -> ±inf yields None (the guard is dead code).
    if (std::isinf(fval)) return std::nullopt;
    // str(int(fval)), no "-0"; 17+ integer digits take Stata's e-notation
    // (fix round v4: `=1e16' is 1.00000000000e+16 in StataBE, fmt2.log).
    if (pyIsInteger(fval) && std::fabs(fval) < 1e16) return stataIntText(fval);
    // BUG-660 fix round 2: was pythonFloatRepr (Python's 17-sig-fig
    // round-trip repr) unconditionally -- fine for a plain arithmetic
    // expression, but this namespace now also reaches Stata's own functions
    // (round/normal/invttail/... -- postfileStataMathNamespace() above), and
    // Stata renders THEIR results the same 16-sig-fig, no-leading-zero way
    // local/global do (stataLocalRealText, applied to that surface via
    // capNumericTextToStataLocalDigits). Before this namespace widening these
    // calls were a NameError this function turned into std::nullopt (left
    // unexpanded); giving them Python-repr precision now would be a NEW
    // silent wrong answer, not the honest refusal they used to be.
    return stataLocalRealText(fval);
  }
  if (r.isStr()) return r.strVal();  // unreachable (whitelist has no quotes)
  return std::nullopt;
}

// ---------------------------------------------------------------------------
// _expand_pass

// Defined below with the rest of the extended macro functions; the inline
// matrix-stripe pass in expandPass answers through it (BUG-629).
namespace {
std::optional<std::string> tryExtendedMacroFn(const std::string& rhsIn, EvalSession& session,
                                              MacroFnError* err);
bool classifyFailedEqExpr(const std::string& exprIn, const std::string& raised,
                          MacroFnError* err, const EvalSession* session);

// The extended macro functions tryExtendedMacroFn answers, by Stata's name and
// shortest abbreviation ([P] macro): `tok` -> the full name, "" for any other
// token. Case-sensitive, as Stata is: StataBE 19.5 answers `: t u`, `: fo u`,
// `: lab ul 1`, `: sort`, `: rown M`, `: colf M`, `: rowe M` and refuses `: T u`,
// `: TYPE u`, `: Word count a`, `: COLNAMES M`, `: colso M`, `: sor`, `: la ul 1`,
// `: dat label`, `: pw`, `: subinst` with "<tok> not allowed", r(101).
std::string extMacroFnName(const std::string& tok) {
  struct Name {
    const char* full;
    size_t shortest;
  };
  static constexpr Name kNames[] = {
      {"type", 1},     {"format", 1},   {"value", 3},        {"variable", 3},     {"data", 4},
      {"sortedby", 4}, {"label", 3},    {"char", 4},         {"list", 4},         {"word", 4},
      {"length", 6},   {"subinstr", 8}, {"pwd", 3},          {"dir", 3},          {"rownames", 4},
      {"colnames", 4}, {"roweq", 4},    {"rowfullnames", 4}, {"colfullnames", 4}, {"coleq", 4},
      {"rowsof", 6},   {"colsof", 6}};
  for (const auto& n : kNames) {
    const std::string full = n.full;
    if (tok.size() >= n.shortest && full.compare(0, tok.size(), tok) == 0) return full;
  }
  return std::string();
}

// Stata's check of a macro NAME an extended function reads or writes (`: list`
// operands, `: length`, `: subinstr` and its count()): a local is stored as `_`
// plus its name, so it may start with a digit but holds at most 31 characters; a
// global is a Stata name of up to 32. StataBE 19.5, r(198): "_abcdefghijabcdefgh
// ijabcdefghijab invalid name" for a 32-character local, "1A invalid name" for
// global(1A), "_) invalid name" for local(). "" when the name is good.
std::string macroNameError(bool global, const std::string& name) {
  const std::string stored = global ? name : "_" + name;
  bool ok = !stored.empty() && utf8Len(stored) <= 32 && cecon::ident::isIdentStartByte(stored[0]);
  for (size_t i = 0; ok && i < stored.size(); ++i) ok = isIdentChar(stored[i]);
  return ok ? std::string() : stored + " invalid name";
}

// name-binding-display-literals: the messages of inline `=exp' references that
// failed and expanded to nothing (expandPass), for the dispatcher to print
// (takeInlineExpansionNotes). Bounded: a caller that never drains cannot grow it.
thread_local std::vector<std::string> tl_inlineNotes;
void noteInlineFailure(const std::string& message) {
  if (tl_inlineNotes.size() < 64) tl_inlineNotes.push_back(message);
}

// name-binding-display-literals (review, round 5): the names of Stata 19.5's
// expression functions. An inline `=exp' whose evaluation stops on an unknown
// function expands to nothing at rc 0 only when Stata does not know the
// function either (bd_inl_nofn, `=nosuchfn_q(1)'); a NameError here only says
// that THIS evaluator lacks the name. Design decision: no list existed in
// piet/native, so this one is read off StataNow 19.5 itself -- every
// `{cmd:NAME(}` signature in ado/base/f/f_*.ihlp, the f_*.sthlp entries
// (binomiallc; the d/h/m/q/w anachronisms; normprob, uniform), the out-of-date
// table in f_normprob.sthlp (nchi ... tprob), the _by() family (byable), the
// pre-14 string names (whatsnew10to11: lower ... upper; length, index, trim,
// itrim) and invnorm/normd/normden/issym/uniform0, plus ty. Case matters, as
// it does in Stata (Ftail, not ftail). Erring toward membership costs a loud
// refusal; erring the other way is a silent empty expansion.
// BUG-1420: the names StataBE 19.5 answers in c() -- the 196 keys `creturn list`
// prints, plus the hidden ones measured beside them (seed, namelen,
// rngstate_mt64s; lane expr-sem x1b/a24.do, a25.do), plus 27 more that
// `creturn list` does not print and StataBE 19.5 still answers at rc 0
// (c(matsize) 400, c(flavor) IC, c(timeout1) 30, c(sortseed), c(java) 1, ...;
// over 500 names tried on 2026-11-03, every c(NAME) and `set NAME` in the
// manuals and variants of them), plus 248 more found in the identifier strings
// of Stata's own library. Any other c(NAME) is r(133) "c(NAME)
// undefined" in display, local, scalar and generate alike (c(bogus_setting),
// c(Linesize), c(n), c(seed0), c(dockable)), and `c(NAME)' is "". A hidden key
// missing here would expand to "" where Stata has a value, so a name found
// later belongs in this list. Case matters. The names resolveCValue serves are
// answered before this list is asked, so PIET's own c(obs) and c(sysdir_sample)
// keep their values.
bool isStataCName(const std::string& name) {
  static const std::unordered_set<std::string> kNames = {
      "ALPHA", "MP", "Mons", "Months", "N", "SE", "Wdays", "Weekdays", "adopath", "adosize",
      "alpha", "bit", "born_date", "byteorder", "cformat", "changed", "charlen", "clevel",
      "cmdlen", "coeftabresults", "collect_double", "collect_label", "collect_style",
      "collect_warn", "console", "copycolor", "current_date", "current_time", "dirsep",
      "docx_hardbreak", "docx_maxtable", "docx_paramode", "doeditbackup", "dots", "dp",
      "dtable_style", "dtascomplevel", "dyndoc_version", "edition", "edition_real", "emptycells",
      "epsdouble", "epsfloat", "eqlen", "etable_style", "filedate", "filename", "frame", "fredkey",
      "fvbase", "fvlabel", "fvtrack", "fvwrap", "fvwrapon", "graphics", "hostname", "httpproxy",
      "httpproxyauth", "httpproxyhost", "httpproxyport", "httpproxypw", "httpproxyuser",
      "include_bitmap", "iterlog", "java_heapmax", "java_home", "k", "lapack_openblas", "level",
      "linegap", "linesize", "locale_functions", "locale_icudflt", "locale_ui", "logmsg",
      "logtype", "lstretch", "machine_type", "macrolen", "matacache", "matafavor", "matalibs",
      "matalnum", "matamofirst", "mataoptimize", "matasolvetol", "matastrict", "max_N_theory",
      "max_cmdlen", "max_graphsize", "max_it_cvars", "max_it_fvars", "max_k_theory",
      "max_macrolen", "max_matdim", "max_memory", "max_width_theory", "maxbezierpath", "maxbyte",
      "maxdb", "maxdouble", "maxfloat", "maxint", "maxiter", "maxlong", "maxstrlvarlen",
      "maxstrvarlen", "maxvar", "maxvlabellen", "memory", "min_graphsize", "min_memory", "minbyte",
      "mindouble", "minfloat", "minint", "minlong", "mode", "more", "namelen", "namelenbyte",
      "namelenchar", "niceness", "noisily", "notifyuser", "obs_t", "odbcdriver", "odbcmgr", "os",
      "osdtl", "pagesize", "pdf_maxtable", "pformat", "pi", "playsnd", "printcolor", "processors",
      "processors_lic", "processors_mach", "processors_max", "pwd", "python_exec",
      "python_userpath", "rc", "reshape_favor", "reventries", "revkeyboard", "rmsg", "rmsg_time",
      "rng", "rng_current", "rngseed_mt64s", "rngstate", "rngstate_mt64s", "rngstream", "scheme",
      "scrollbufsize", "searchdefault", "seed", "segmentsize", "sformat", "showbaselevels",
      "showemptycells", "showomitted", "smallestdouble", "smoothfonts", "sort_current",
      "sortmethod", "sortrngstate", "stata_name", "stata_version", "svy_tab_comps", "sysdir_base",
      "sysdir_oldplace", "sysdir_personal", "sysdir_plus", "sysdir_site", "sysdir_stata",
      "table_style", "tabulate_comps", "tabulate_style", "tmpdir", "trace", "tracedepth",
      "traceexpand", "tracehilite", "traceindent", "tracenumber", "tracesep", "type",
      "update_interval", "update_prompt", "update_query", "username", "userversion", "varabbrev",
      "varkeyboard", "version", "width",
      // Hidden: answered by StataBE 19.5, not printed by `creturn list`.
      "charset", "checksum", "debug", "flavor", "flavor_real", "java", "java_version",
      "lapack_mkl", "matsize", "max_N_current", "max_k_current", "max_matsize",
      "max_preservemem", "max_width_current", "min_matsize", "rngstate_kiss32",
      "rngstate_mt64", "seed_kiss32", "seed_mt64", "seed_mt64s", "sortseed", "sysdir_updates",
      "timeout1", "timeout2", "unicode", "varlabelpos", "virtual",
      // Hidden as well, found by asking StataBE 19.5 for c(NAME) over the identifier
      // strings of its own library (review of lane expr-sem, 2026-11-03): rc 0 for
      // each, c(framesmax) 100, c(osclass) MACARM64, c(hasicu) 1, c(tol_1) 1e-09, the
      // collect_dlg_* family "". Their neighbours c(collect_dlg_bogus), c(tol_3) and
      // c(framesmaxx) are r(133), so no prefix stands in for the list.
      "UseConsoleFontLength", "bayesmhtmpfn", "buildfvinfo", "c_compiler", "c_compiler_fullvers",
      "c_compiler_vers", "cmdtitle", "coeftabfail", "collect_catch_backup", "collect_current",
      "collect_dlg__cons_position", "collect_dlg_cell_abbreviate", "collect_dlg_cell_bground_color",
      "collect_dlg_cell_border_color", "collect_dlg_cell_border_pattern",
      "collect_dlg_cell_border_smcl", "collect_dlg_cell_border_width",
      "collect_dlg_cell_cidelimiter", "collect_dlg_cell_cridelimiter",
      "collect_dlg_cell_dborder_color", "collect_dlg_cell_dborder_pattern",
      "collect_dlg_cell_empty", "collect_dlg_cell_fground_color", "collect_dlg_cell_fill_pattern",
      "collect_dlg_cell_font_color", "collect_dlg_cell_font_family", "collect_dlg_cell_font_size",
      "collect_dlg_cell_font_strikeout", "collect_dlg_cell_font_style",
      "collect_dlg_cell_font_underline", "collect_dlg_cell_font_variant",
      "collect_dlg_cell_font_weight", "collect_dlg_cell_halign", "collect_dlg_cell_latex",
      "collect_dlg_cell_margin_width", "collect_dlg_cell_maxLabel", "collect_dlg_cell_maximum",
      "collect_dlg_cell_minLabel", "collect_dlg_cell_minimum", "collect_dlg_cell_nformat",
      "collect_dlg_cell_sformat", "collect_dlg_cell_smcl", "collect_dlg_cell_valign",
      "collect_dlg_cid", "collect_dlg_col_atDelimiter", "collect_dlg_col_barDelimiter",
      "collect_dlg_col_binder", "collect_dlg_col_delimiter", "collect_dlg_col_dups",
      "collect_dlg_col_extraspace", "collect_dlg_col_nodelimiter", "collect_dlg_col_position",
      "collect_dlg_col_width", "collect_dlg_dim", "collect_dlg_dim1", "collect_dlg_dim10",
      "collect_dlg_dim2", "collect_dlg_dim3", "collect_dlg_dim4", "collect_dlg_dim5",
      "collect_dlg_dim6", "collect_dlg_dim7", "collect_dlg_dim8", "collect_dlg_dim9",
      "collect_dlg_dim_autolevels", "collect_dlg_dim_label", "collect_dlg_dim_level",
      "collect_dlg_dim_level_label", "collect_dlg_dim_spanheader", "collect_dlg_export_style_key",
      "collect_dlg_export_style_type", "collect_dlg_export_style_value",
      "collect_dlg_header_fvlevels", "collect_dlg_header_level", "collect_dlg_header_nformat",
      "collect_dlg_header_title", "collect_dlg_item_base", "collect_dlg_item_empty",
      "collect_dlg_item_omit", "collect_dlg_note", "collect_dlg_notes_bground_color",
      "collect_dlg_notes_count", "collect_dlg_notes_fground_color",
      "collect_dlg_notes_fill_pattern", "collect_dlg_notes_font_color",
      "collect_dlg_notes_font_family", "collect_dlg_notes_font_size",
      "collect_dlg_notes_font_strikeout", "collect_dlg_notes_font_style",
      "collect_dlg_notes_font_underline", "collect_dlg_notes_font_variant",
      "collect_dlg_notes_font_weight", "collect_dlg_notes_latex", "collect_dlg_notes_smcl",
      "collect_dlg_notes_tags", "collect_dlg_row_abbreviate", "collect_dlg_row_arrangement",
      "collect_dlg_row_atDelimiter", "collect_dlg_row_barDelimiter", "collect_dlg_row_binder",
      "collect_dlg_row_delimiter", "collect_dlg_row_dups", "collect_dlg_row_indent",
      "collect_dlg_row_length", "collect_dlg_row_nobinder", "collect_dlg_row_nodelimiter",
      "collect_dlg_row_position", "collect_dlg_row_spacer", "collect_dlg_row_span",
      "collect_dlg_row_truncate", "collect_dlg_row_wrap", "collect_dlg_row_wrapon",
      "collect_dlg_show_ci", "collect_dlg_side", "collect_dlg_stars_attach",
      "collect_dlg_stars_delimiter", "collect_dlg_stars_fortags", "collect_dlg_stars_label1",
      "collect_dlg_stars_label2", "collect_dlg_stars_label3", "collect_dlg_stars_label4",
      "collect_dlg_stars_label5", "collect_dlg_stars_nformat", "collect_dlg_stars_order",
      "collect_dlg_stars_prefix", "collect_dlg_stars_pvname", "collect_dlg_stars_results",
      "collect_dlg_stars_shownote", "collect_dlg_stars_suffix", "collect_dlg_stars_type",
      "collect_dlg_stars_value1", "collect_dlg_stars_value2", "collect_dlg_stars_value3",
      "collect_dlg_stars_value4", "collect_dlg_stars_value5", "collect_dlg_tab_atDelimiter",
      "collect_dlg_tab_barDelimiter", "collect_dlg_tab_binder", "collect_dlg_tab_delimiter",
      "collect_dlg_tab_dimBinder", "collect_dlg_tab_dimDelimiter", "collect_dlg_tab_nodelimiter",
      "collect_dlg_tag", "collect_dlg_title", "collect_dlg_title_bground_color",
      "collect_dlg_title_fground_color", "collect_dlg_title_fill_pattern",
      "collect_dlg_title_font_color", "collect_dlg_title_font_family",
      "collect_dlg_title_font_size", "collect_dlg_title_font_strikeout",
      "collect_dlg_title_font_style", "collect_dlg_title_font_underline",
      "collect_dlg_title_font_variant", "collect_dlg_title_font_weight", "collect_dlg_title_latex",
      "collect_dlg_title_smcl", "collect_export_version", "compactvarlist", "compile_number",
      "curl_version", "curscm", "dark_mode", "di_g", "excelsupport", "expr_revar", "filenamelen",
      "framesmax", "getsockopt", "graphexttext", "guixeqcmd", "hasharu", "hasicu", "helpcolor",
      "httpsendport", "httpuseragent", "importcase", "irngalways64", "java_debugger",
      "java_heap_committed", "java_heap_max", "java_heap_usage", "java_initialized",
      "java_javacall_classpath", "java_jconsole", "java_library_path", "java_system_classpath",
      "java_system_java", "java_vendor", "jitterseed", "jvm_override_options", "loghelp",
      "mapsymbols", "marginsatvars", "marginscmd", "marginsdxvars", "marginsmvars", "marginsrevars",
      "mata_hdfe", "matadebug", "matawarningaserror", "max_intpoints", "max_rngstreams",
      "maxoptions", "min_k", "netdebug", "osclass", "pollrate", "polltime", "postunlinktimeout",
      "prefix", "preserve_ttl", "procs_license", "procs_mach", "procs_use", "profiling",
      "regex_current", "regexmethod", "scheme_cscript", "slowrun", "spsssas_support", "sslrelax",
      "stata_serno", "stata_version_major", "staticlink", "suppress_statanow_update",
      "sysdir_sjplus", "sysdir_stbplus", "textresultsmode", "textsize", "tokenlen", "tol_0",
      "tol_1", "tol_2", "tol_r", "tracecode", "tracetime", "unlinktimeout", "update_oride",
      "url_allow_redirects", "useoldlapack", "varlabellen", "varlabellenbyte", "version_rng",
      "version_rng_kiss32", "version_rng_mt64", "version_rng_mt64s", "visible_graph_names"};
  return kNames.count(name) != 0;
}

bool isStataFunctionName(const std::string& name) {
  static const std::unordered_set<std::string> kNames = {
      "Cdhms", "Chms", "Clock", "Clockdiff", "Clockdiff_frac", "Clockpart", "Cmdyhms", "Cofc",
      "Cofd", "F", "Fden", "Ftail", "I", "J", "_by", "_byindex", "_bylastcall", "_byn1", "_byn2",
      "_caller", "_frval", "abbrev", "abs", "acos", "acosh", "age", "age_frac", "asin", "asinh",
      "atan", "atan2", "atanh", "autocode", "betaden", "binomial", "binomiallc", "binomialp",
      "binomialtail", "binormal", "birthday", "bofd", "byteorder", "c", "cauchy", "cauchyden",
      "cauchytail", "ceil", "char", "chi2", "chi2den", "chi2tail", "chiprob", "cholesky", "chop",
      "clip", "clock", "clockdiff", "clockdiff_frac", "clockpart", "cloglog", "cofC", "cofd",
      "coleqnumb", "collatorlocale", "collatorversion", "colnfreeparms", "colnumb", "colsof",
      "comb", "cond", "corr", "cos", "cosh", "d", "daily", "date", "datediff", "datediff_frac",
      "datepart", "day", "daysinmonth", "dayssincedow", "dayssinceweekday", "daysuntildow",
      "daysuntilweekday", "det", "dgammapda", "dgammapdada", "dgammapdadx", "dgammapdx",
      "dgammapdxdx", "dhms", "diag", "diag0cnt", "digamma", "dmy", "dofC", "dofb", "dofc", "dofh",
      "dofm", "dofq", "dofw", "dofy", "dow", "doy", "dunnettprob", "e", "el", "epsdouble",
      "epsfloat", "exp", "expm1", "exponential", "exponentialden", "exponentialtail", "fileexists",
      "fileread", "filereaderror", "filewrite", "firstdayofmonth", "firstdowofmonth",
      "firstweekdayofmonth", "float", "floor", "fmtwidth", "fprob", "frval", "gammaden", "gammap",
      "gammaptail", "get", "h", "hadamard", "halfyear", "halfyearly", "has_eprop", "hh", "hhC",
      "hms", "hofd", "hours", "hypergeometric", "hypergeometricp", "ibeta", "ibetatail",
      "igaussian", "igaussianden", "igaussiantail", "index", "indexnot", "inlist", "inrange", "int",
      "inv", "invF", "invFtail", "invbinomial", "invbinomialtail", "invcauchy", "invcauchytail",
      "invchi", "invchi2", "invchi2tail", "invcloglog", "invdunnettprob", "invexponential",
      "invexponentialtail", "invfprob", "invgammap", "invgammaptail", "invibeta", "invibetatail",
      "invigaussian", "invigaussiantail", "invlaplace", "invlaplacetail", "invlogistic",
      "invlogistictail", "invlogit", "invnF", "invnFtail", "invnbinomial", "invnbinomialtail",
      "invnchi", "invnchi2", "invnchi2tail", "invnibeta", "invnorm", "invnormal", "invnt",
      "invnttail", "invpoisson", "invpoissontail", "invsym", "invt", "invttail", "invtukeyprob",
      "invvech", "invvecp", "invweibull", "invweibullph", "invweibullphtail", "invweibulltail",
      "irecode", "isleapsecond", "isleapyear", "issym", "issymmetric", "itrim", "laplace",
      "laplaceden", "laplacetail", "lastdayofmonth", "lastdowofmonth", "lastweekdayofmonth",
      "length", "ln", "ln1m", "ln1p", "lncauchyden", "lndirichletden", "lnfactorial", "lngamma",
      "lngeometricp", "lnigammaden", "lnigaussianden", "lniwishartden", "lnlaplaceden",
      "lnmvnormalden", "lnnormal", "lnnormalden", "lnparetoden", "lnwishartden", "log", "log10",
      "log1m", "log1p", "logistic", "logisticden", "logistictail", "logit", "lower", "ltrim", "m",
      "matmissing", "matrix", "matuniform", "max", "maxbyte", "maxdouble", "maxfloat", "maxint",
      "maxlong", "mdy", "mdyhms", "mi", "min", "minbyte", "mindouble", "minfloat", "minint",
      "minlong", "minutes", "missing", "mm", "mmC", "mod", "mofd", "month", "monthly", "mreldif",
      "msofhours", "msofminutes", "msofseconds", "nF", "nFden", "nFtail", "nbetaden", "nbinomial",
      "nbinomialp", "nbinomialtail", "nchi", "nchi2", "nchi2den", "nchi2tail", "nextbirthday",
      "nextdow", "nextleapyear", "nextweekday", "nibeta", "normal", "normalden", "normd", "normden",
      "normprob", "now", "npnF", "npnchi", "npnchi2", "npnt", "nt", "ntden", "nttail", "nullmat",
      "plural", "poisson", "poissonp", "poissontail", "previousbirthday", "previousdow",
      "previousleapyear", "previousweekday", "proper", "q", "qofd", "quarter", "quarterly", "r",
      "rbeta", "rbinomial", "rcauchy", "rchi2", "real", "recode", "regexcapture",
      "regexcapturenamed", "regexm", "regexmatch", "regexr", "regexreplace", "regexreplaceall",
      "regexs", "reldif", "replay", "return", "reverse", "rexponential", "rgamma", "rgeometric",
      "rhypergeometric", "rigaussian", "rlaplace", "rlogistic", "rnbinomial", "rnormal", "round",
      "roweqnumb", "rownfreeparms", "rownumb", "rowsof", "rpareto", "rpoisson", "rt", "rtrim",
      "runiform", "runiformint", "rweibull", "rweibullph", "s", "scalar", "seconds", "sign", "sin",
      "sinh", "smallestdouble", "soundex", "soundex_nara", "sqrt", "ss", "ssC", "strcat", "strdup",
      "string", "stritrim", "strlen", "strlower", "strltrim", "strmatch", "strofreal", "strpos",
      "strproper", "strreverse", "strrpos", "strrtrim", "strtoname", "strtrim", "strupper",
      "subinstr", "subinword", "substr", "sum", "sweep", "t", "tC", "tan", "tanh", "tc", "td",
      "tden", "th", "tin", "tm", "tobytes", "today", "tprob", "tq", "trace", "trigamma", "trim",
      "trunc", "ttail", "tukeyprob", "tw", "twithin", "ty", "uchar", "udstrlen", "udsubstr",
      "uisdigit", "uisletter", "uniform", "uniform0", "upper", "ustrcompare", "ustrcompareex",
      "ustrfix", "ustrfrom", "ustrinvalidcnt", "ustrleft", "ustrlen", "ustrlower", "ustrltrim",
      "ustrnormalize", "ustrpos", "ustrregexm", "ustrregexra", "ustrregexrf", "ustrregexs",
      "ustrreverse", "ustrright", "ustrrpos", "ustrrtrim", "ustrsortkey", "ustrsortkeyex",
      "ustrtitle", "ustrto", "ustrtohex", "ustrtoname", "ustrtrim", "ustrunescape", "ustrupper",
      "ustrword", "ustrwordcount", "usubinstr", "usubstr", "vec", "vecdiag", "vech", "vecp", "w",
      "week", "weekly", "weibull", "weibullden", "weibullph", "weibullphden", "weibullphtail",
      "weibulltail", "wofd", "word", "wordbreaklocale", "wordcount", "year", "yearly", "yh", "ym",
      "yofd", "yq", "yw"};
  return kNames.count(name) != 0;
}
}  // namespace

std::optional<std::string> undefinedCRef(const std::string& expr) {
  if (expr.find("c(") == std::string::npos) return std::nullopt;
  std::vector<std::string> spans;
  const std::string bare = protectStringLiterals(expr, &spans);
  static const std::regex reC(R"(\bc\(\s*([A-Za-z_]\w*)\s*\))");
  for (std::sregex_iterator it(bare.begin(), bare.end(), reC), end; it != end; ++it) {
    const std::string name = (*it)[1].str();
    if (!isStataCName(name)) return "c(" + name + ") undefined";
  }
  return std::nullopt;
}

// BUG-1330: a name StataBE's c() answers. isStataCName holds the list (BUG-1420
// measured it, hidden keys included); this is the name the c() refusal and the
// delimited expansion ask by.
bool isStataCClassName(const std::string& name) { return isStataCName(name); }

namespace {
// BUG-1330: the c() names whose value is a string (quoted in `creturn list`,
// StataNow 19.5 BE), so a value that happens to read as a number (a digits-only
// c(username)) is still spliced as a string.
bool isStataCClassStringName(const std::string& name) {
  static const std::unordered_set<std::string> kNames = {
      "current_date", "current_time", "stata_name", "born_date", "edition", "edition_real",
      "mode", "console", "os", "osdtl", "hostname", "machine_type", "byteorder", "username",
      "sysdir_stata", "sysdir_base", "sysdir_site", "sysdir_plus", "sysdir_personal",
      "sysdir_oldplace", "sysdir_sample", "tmpdir", "adopath", "pwd", "dirsep", "frame",
      "filename", "filedate", "more", "rmsg", "dp", "logtype", "logmsg", "notifyuser", "playsnd",
      "include_bitmap", "iterlog", "showbaselevels", "showemptycells", "showomitted", "fvlabel",
      "fvwrapon", "lstretch", "cformat", "sformat", "pformat", "coeftabresults", "dots",
      "collect_label", "collect_style", "table_style", "etable_style", "dtable_style",
      "tabulate_style", "tabulate_comps", "svy_tab_comps", "collect_warn", "revkeyboard",
      "varkeyboard", "smoothfonts", "graphics", "scheme", "printcolor", "copycolor", "httpproxy",
      "httpproxyhost", "httpproxyauth", "httpproxyuser", "httpproxypw", "update_query",
      "update_prompt", "trace", "tracesep", "traceindent", "traceexpand", "tracenumber",
      "tracehilite", "matastrict", "matalnum", "mataoptimize", "matafavor", "matalibs",
      "matamofirst", "java_heapmax", "java_home", "lapack_openblas", "docx_hardbreak",
      "docx_paramode", "python_exec", "python_userpath", "rng", "rng_current", "rngstate",
      "sortmethod", "sort_current", "sortrngstate", "locale_ui", "locale_functions",
      "locale_icudflt", "type", "searchdefault", "varabbrev", "emptycells", "fvtrack", "fvbase",
      "odbcmgr", "odbcdriver", "fredkey", "collect_double", "reshape_favor", "doeditbackup",
      "alpha", "ALPHA", "Mons", "Months", "Wdays", "Weekdays", "obs_t", "seed", "flavor",
      "virtual", "sysdir_updates", "sortseed"};
  return kNames.count(name) != 0;
}

// BUG-1330: every bare c(name) outside string literals in an inline `=exp'`,
// replaced by its value through the c() hook (piet::substituteCRefs, which the
// other expression sites run, is beyond this layer): a number as itself, any
// other value as a string literal. A name the hook does not answer stays for
// the classifier's refusal.
std::string substituteBareCRefs(const std::string& expr, EvalSession& session) {
  if (!session.cValue || expr.find("c(") == std::string::npos) return expr;
  static const std::regex reBareC(R"(\bc\(\s*([A-Za-z_]\w*)\s*\))");
  std::vector<std::string> spans;
  std::string work = protectLiterals(expr, &spans);
  std::string out;
  size_t last = 0;
  for (auto it = std::sregex_iterator(work.begin(), work.end(), reBareC);
       it != std::sregex_iterator(); ++it) {
    const std::string name = (*it)[1].str();
    std::optional<std::string> cv = session.cValue(name);
    if (!cv || cv->find('"') != std::string::npos) continue;
    char* end = nullptr;
    const double d = std::strtod(cv->c_str(), &end);
    const bool number = !isStataCClassStringName(name) && !cv->empty() &&
                        ((end == cv->c_str() + cv->size() && std::isfinite(d)) || *cv == ".");
    out.append(work, last, static_cast<size_t>(it->position(0)) - last);
    out += number ? "(" + *cv + ")" : "\"" + *cv + "\"";
    last = static_cast<size_t>(it->position(0) + it->length(0));
  }
  if (last == 0) return expr;
  out.append(work, last, std::string::npos);
  return restoreLiterals(out, spans);
}
}  // namespace

std::string cClassRefusalRc(const std::string& name) {
  return isStataCClassName(name) ? "198" : "133";
}

std::string cClassRefusalMessage(const std::string& name) {
  return isStataCClassName(name)
             ? "c(" + name + ") is a Stata system value this version of PIET does not provide"
             : "c(" + name + ") undefined";
}

std::vector<std::string> takeInlineExpansionNotes() {
  std::vector<std::string> out;
  out.swap(tl_inlineNotes);
  return out;
}

ExpandResult expandPass(const std::string& lineIn, EvalSession& session,
                        bool unsetRAsMissing, bool verbatim) {
  dprof::EScope _z(dprof::kEExpandPass);
  std::string line = lineIn;

  // The inline extended macro functions (findReadyInlineMacroFn), the leftmost
  // ready span first: Stata expands a line left to right, and a span is ready
  // only once the spans inside it are answered, so an inner span still comes
  // before the span around it. The order shows when a span writes a macro
  // (`: subinstr ..., count(local n)'): it runs after every reference to its
  // left and before every one to its right. With x = `a b a' and n = `old',
  // StataBE 19.5 prints O1|old|b b b|2| for `display "O1|`n'|`: subinstr local
  // x "a" "b", all count(local n)'|`n'|"', and 3|z b a|1 for `: length local
  // n' on either side of the count; so such a span's prefix is expanded first,
  // and the references to its right wait for the spans and passes after it.
  // They run FIRST so a class argument (`` `: colnames r(table)' ``, `` `:
  // e(scalars)' ``) is consumed before the bare-r()/e() passes would read it as
  // a scalar (BUG-629, w2c_postest.log:102/107/757). A function that fails stops
  // the line at its rc, as the `local x : fn` form does -- but for BUG-1338: a
  // reader that fails as Stata's own reader fails (`: type nosuch', `: format
  // nosuch', `: char nosuch[x]', `: label (nosuch) 1', a stripe or `: word' of
  // nothing; r(111) or r(198)) prints its message and expands to NOTHING at rc 0,
  // as StataBE does on any line and under any prefix (`display "P|" 7 "|`: type
  // nosuch'|"` prints `variable nosuch not found` and then `P|7||`; `local a
  // "A`: type nosuch'B"` stores AB). It stopped the line at r(111). e() name
  // lists and the other functions keep their refusal.
  if (line.find("`:") != std::string::npos) {
    // BUG-719: the variable readers see a plane refreshed first
    // (EvalSession::syncVarPlane: the dispatcher syncs it only before
    // `local`/`global`/`label` lines), once per line.
    bool planeSynced = false;
    static const std::regex reCountOpt(R"(,[\s\S]*\bc[a-z]*\s*\()");
    for (;;) {
      size_t p = std::string::npos, quote = 0;
      for (size_t b = line.size(), at, q; (at = findReadyInlineMacroFn(line, b, &q)) != std::string::npos;
           b = at) {
        p = at;
        quote = q;
      }
      if (p == std::string::npos) break;
      const std::string body = line.substr(p + 2, quote - p - 2);
      size_t h = 0;
      while (h < body.size() && isPySpace(body[h])) ++h;
      size_t he = h;
      while (he < body.size() && isIdentChar(body[he])) ++he;
      const std::string head = extMacroFnName(body.substr(h, he - h));
      if (!planeSynced && session.syncVarPlane &&
          (head == "type" || head == "format" || head == "label" || head == "char" ||
           head == "variable" || head == "value")) {
        session.syncVarPlane();
        planeSynced = true;
      }
      if (head == "subinstr" && std::regex_search(body, reCountOpt)) {
        std::string prefix = line.substr(0, p);
        for (int pass = 0; pass < kMaxExpansionPasses && hasMacroRefs(prefix); ++pass) {
          ExpandResult done = expandPass(prefix, session, unsetRAsMissing, verbatim);
          if (done.isError) return done;
          if (done.line == prefix) break;
          prefix = done.line;
        }
        quote = quote - p + prefix.size();
        line = prefix + line.substr(p);
        p = prefix.size();
      }
      MacroFnError fe;
      auto v = tryExtendedMacroFn(": " + body, session, &fe);
      if (!v) {
        if (!fe.failed()) break;  // left for the tokenizer's refusal
        static const std::unordered_set<std::string> kReaders = {
            "type", "format", "variable", "value", "label", "char", "sortedby", "word",
            "rownames", "colnames", "roweq", "coleq", "rowfullnames", "colfullnames"};
        if ((fe.rc != "111" && fe.rc != "198") || kReaders.count(head) == 0)
          return rcErrorResult(fe.message, fe.rc);
        noteInlineFailure(fe.message);  // BUG-1338: expands to nothing at rc 0
        line.erase(p, quote - p + 1);
        continue;
      }
      // EM13: a characteristic's text is spliced verbatim.
      line.replace(p, quote - p + 1, verbatim && head == "char" ? encodeVerbatim(*v) : *v);
    }
  }

  // A3 pre-filters. Each block below opens with a full regex scan of the line;
  // every one of these constructs requires a literal character that a memchr-
  // class find() settles first (a backtick for `local'/`=expr'/`r()'/`s()', a
  // dollar for $global, `e(`/`r(` for the bare class refs). A profile of a pure
  // macro loop put ~18% of samples in libc++'s regex engine and ~40% in
  // malloc/free — largely that engine's own per-execution state vectors — so
  // the scan that never runs is the one worth removing. Strict pre-filters:
  // a pattern cannot match text that lacks its own literal.
  // Recomputed at each site, never hoisted: a substitution can INTRODUCE a
  // marker the line did not have (a local whose VALUE contains `$G` puts a
  // dollar into the line during the local pass, and the bare-global pass below
  // must still see it). find() is a memchr-class scan, so paying it per site is
  // still orders of magnitude below the regex it guards.

  // --- `=expr' arithmetic-macro form — runs FIRST ---
  // P2-PERF ARC-A/A3-b: the guard is "`=", not "`". The pattern is `=…' and a
  // bare backtick — every `local' reference in the language — cannot match it,
  // so the ordinary macro loop was paying a full regex scan per line to learn
  // that.
  for (const auto& [full, inner] : line.find("`=") != std::string::npos
                                       ? findAll(reArithMacro(), line)
                                           : std::vector<std::pair<std::string, std::string>>{}) {
    std::smatch m;
    // `=e(N)'` / `=r(mean)'` inside a double-quoted string: the e()/r() layers
    // below protect string literals (BUG-507), so a bare reference INSIDE an
    // inline expression in quotes was never substituted, this guard skipped it
    // forever, and `display "N=`=e(N)'"` died on "did not converge" (w2j_e3_rlasso
    // _e3_x `s'=`=e(`s')'). Stata evaluates the expression: a scalar is its value,
    // a macro its string, and a name that is neither is missing (`.`). A complete
    // inner text only (no macro reference left); a posted matrix stays for the
    // matrix layer.
    std::string innerSub = inner;
    if (innerSub.find('`') == std::string::npos && innerSub.find('$') == std::string::npos &&
        (std::regex_search(innerSub, m, reERef()) || std::regex_search(innerSub, m, reRRef()))) {
      bool matrixLeft = false;
      for (const std::regex* rx : {&reERef(), &reRRef()}) {
        const char cls = (rx == &reERef()) ? 'e' : 'r';
        for (const auto& [ref, nm] : findAll(*rx, innerSub)) {
          std::string text;
          if (session.findClassMatrix(cls, nm) != nullptr || nm == "sample") {
            matrixLeft = true;  // the matrix layer's / e(sample)'s, as before
            continue;
          }
          if (cls == 'e') {
            // The evaluator's own e() reading (resolveEstScalarRefs' arm):
            // string -> scalar -> `.` for an absent name.
            text = valueRefText(resolveERef(nm, session));
          } else if (const double* v = session.rValues.find(nm)) {
            text = std::isnan(*v) ? std::string(".") : valueRefText(pythonFloatRepr(*v));
          } else if (const std::string* sv = session.rStrings.find(nm);
                     sv != nullptr && sv->find('"') == std::string::npos) {
            text = "\"" + *sv + "\"";
          } else {
            matrixLeft = true;  // an r() the r layer refuses by name: its path
            continue;
          }
          innerSub = replaceFirst(innerSub, ref, text);
        }
      }
      if (matrixLeft) innerSub = inner;
    }
    if (innerSub.find('`') == std::string::npos && innerSub.find('$') == std::string::npos)
      innerSub = substituteBareCRefs(innerSub, session);  // BUG-1330
    if (!std::regex_search(innerSub, m, reLocalRef()) &&
        !hasGlobalRef(reGlobalRefBrace(), innerSub) &&
        !hasGlobalRef(reGlobalRefBare(), innerSub) &&
        !std::regex_search(innerSub, m, reRRef()) && !std::regex_search(innerSub, m, reERef())) {
      auto result = evalMacroExpr(innerSub, session);
      // name-binding-display-literals: `=exp' is the expression evaluator's,
      // as `local l = exp` is. evalMacroExpr's namespace holds numbers only, so
      // a string (`="a:b"'`), a variable (`=v'`, `=s'`), scalar() and a
      // variable before a same-named scalar were left unexpanded, and the line
      // died on "Macro expansion did not converge" (w2av_name_binding
      // bd_inl_lit, bd_inl_varsc, bd_inl_scfn, bd_inl_strvar, bd_inl_abbr ...).
      // evalArith answers them, a number in the local's own 16-digit form.
      // An expression that fails as Stata's does -- a name that binds to
      // nothing or to two variables r(111), a function Stata lacks r(133), a type
      // mismatch r(109) -- prints its message and expands to NOTHING at rc 0,
      // as StataBE does (bd_inl_unb, bd_inl_ambig, bd_inl_nofn, bd_inl_strunb,
      // bd_inl_strnum, bd_dy_ilunb: `[]` at rc 0 after `foo not found`). Any
      // other failure keeps its loud path below.
      // Only a COMPLETE expression: an inner text that still holds a macro
      // reference (`=x[`i']' is matched as `x[`i` before `i' expands) waits
      // for the pass that expands it, as before.
      if (!result && innerSub.find('`') == std::string::npos && innerSub.find('$') == std::string::npos) {
        std::string raised;
        bool isStr = false;
        if (auto ar = evalArith(innerSub, &session, &raised, &isStr)) {
          result = isStr ? *ar : capNumericTextToStataLocalDigits(*ar);
        } else {
          MacroFnError fe;
          if (classifyFailedEqExpr(innerSub, raised, &fe, &session)) {
            // The classifier names an unknown function `unknown function NAME()`.
            // A NAME that is one of Stata's functions is a capability PIET
            // lacks, not Stata's r(133): `=invFtail(2,3,.5)'` and `=invlogit(0)'
            // expanded to nothing at rc 0 where Stata gives the value
            // (corpus: 073 greshape `=ustrregexrf(...)'). That is
            // refused by name and stops the line, at the rc its `local l =`
            // sibling gives.
            std::string fn;
            static const std::string kUnknownFn = "unknown function ";
            if (fe.rc == "133" && fe.message.rfind(kUnknownFn, 0) == 0 &&
                fe.message.size() > kUnknownFn.size() + 2 &&
                fe.message.compare(fe.message.size() - 2, 2, "()") == 0) {
              fn = fe.message.substr(kUnknownFn.size(),
                                     fe.message.size() - kUnknownFn.size() - 2);
            }
            // BUG-1330: a c() nothing answered. `c(name) undefined` is
            // Stata's own failure, printed and then nothing at rc 0 as for a
            // function Stata lacks (the arm below); a real c() name this build
            // does not serve is refused by name.
            static const std::string kUndefined = ") undefined";
            const bool cRef = fe.rc == "133" && fe.message.rfind("c(", 0) == 0;
            const bool cUndefined =
                cRef && fe.message.size() > kUndefined.size() &&
                fe.message.compare(fe.message.size() - kUndefined.size(), kUndefined.size(),
                                   kUndefined) == 0;
            if (cRef && !cUndefined) {
              return rcErrorResult(fe.message + " (in `" + inner + "')", "133");
            } else if (!cRef && fe.rc == "133" && (fn.empty() || isStataFunctionName(fn))) {
              return rcErrorResult(
                  (fn.empty() ? std::string("this function") : fn + "()") +
                      " is a Stata function this version of PIET does not implement (in `" +
                      inner + "')",
                  "133");
            }
            if (fe.rc == "111" || fe.rc == "133" || fe.rc == "109") {
              noteInlineFailure(fe.message);
              result = std::string();
            }
          }
        }
      }
      if (result) {
        line = replaceFirst(line, full, *result);
      } else if (session.varObs && inner.find('[') != std::string::npos) {
        // BUG-718: a string (or missing) `VAR[i]` inside a larger expression
        // (`` `=s[1]+"z"' ``) is beyond this layer's string-free whitelist. Left
        // in place it never converged (rc 1); it is refused by name instead.
        std::optional<std::string> whole;
        bool stringLeft = false;
        substituteVarSubscripts(substituteObsSpecials(strip(inner), session), session, &whole,
                                &stringLeft);
        if (stringLeft) {
          ExpandResult r;
          r.isError = true;
          r.error = nlohmann::ordered_json(unsupportedMacro(full));
          return r;
        }
      }
    }
  }

  // --- e() scalar references (WS-C) ---
  // Substitute ONLY names present in the estat-posted _e_scalars store;
  // anything else stays for the display-path resolver.
  // BUG-507: `r(name)`/`e(name)` in their BARE form are NOT macros -- Stata
  // resolves them in the EXPRESSION evaluator, so a double-quoted literal never
  // sees one: `strpos("r(mean)", "mean")` is 3 there and was 0 here, the literal
  // having become its own value before the expression was ever evaluated
  // (tests/eval/golden/aer_w1/w1b_quoteblind.log QB|rclass|3| / QB|eclass|3|).
  //
  // The DELIMITED forms are the opposite case and are NOT protected: `` `r(name)' ``
  // IS macro expansion, Stata expands it inside double quotes, and that is how a
  // varlist gets spliced into a command -- selftest BUG-374(1) holds
  // `display "x: `r(levels)'"` to exactly that and caught a first version of this
  // fix which protected both forms at once.
  //
  // Protection rather than a position-aware skip because the substitution is
  // text-based: `replaceFirst(line, full, value)` rewrites the FIRST occurrence of
  // the matched TEXT, so with `"r(mean)" + string(r(mean))` it would hit the quoted
  // one even when the match that drove it was not.
  {
    std::smatch m;
    if (hasBareEParen(line) && std::regex_search(line, m, reERef())) {  // EXPRLAYER-3
      std::vector<std::string> eSpans;
      line = protectStringLiterals(line, &eSpans);
      for (const auto& [full, ename] : findAll(reERef(), line)) {
        const double* v = session.eScalars.find(ename);
        if (v != nullptr) {
          // A missing e() scalar is Stata's `.`, never the text "nan" (which the
          // evaluator then read as a variable name: `display %21x e(slambda)` r(111)).
          line = replaceFirst(line, full,
                              std::isnan(*v) ? std::string(".")
                                             : valueRefText(pythonFloatRepr(*v)));  // FU-842
        }
      }
      line = restoreStringLiterals(line, eSpans);
    }
  }

  // --- r() references: delimited form `r(name)' FIRST, then bare r(name).
  // String r() (from `return local`) takes precedence over numeric.
  // EXPRLAYER-3: one guard per pattern, each the literal that pattern requires
  // — the delimited form needs "`r(", the bare form needs an `r` at a word
  // boundary followed by '('. The old single `find("r(")` let `scalar(`,
  // `subinstr(` and every other identifier ending in r through to two full
  // regex scans. The guard is recomputed per pass because the delimited pass
  // rewrites the line.
  dprof::ezoneBegin(dprof::kEExpandRBlock);
  const bool matrixVerbLine = isMatrixVerbLine(line);
  static const std::regex reMatrixList(R"(^\s*mat(?:r|ri|rix)?\s+l(?:i|is|ist)?\s)");
  const bool matrixListLine = matrixVerbLine && std::regex_search(line, reMatrixList);
  const bool estaddLine = isEstaddLine(line);
  for (const std::regex* rx : {&reRRefDelim(), &reRRef()}) {
    if (rx == &reRRef() && estaddLine) continue;
    const bool reachable = (rx == &reRRefDelim()) ? line.find("`r(") != std::string::npos
                                                  : hasBareRParen(line);
    if (!reachable) continue;
    // BUG-507: the BARE pass only (see the e() block above for why, and why the
    // delimited pass must NOT be protected).
    std::vector<std::string> rSpans;
    const bool barePass = (rx == &reRRef());
    if (barePass) line = protectStringLiterals(line, &rSpans);
    struct RRestore {
      std::string* l;
      const std::vector<std::string>* sp;
      bool on;
      ~RRestore() {
        if (on) *l = restoreStringLiterals(*l, *sp);
      }
    } rRestore{&line, &rSpans, barePass};
    for (const auto& [full, name] : findAll(*rx, line)) {
      // B4-W3b: on a MATRIX verb, an r-class matrix reference is left in
      // place, exactly as the e() layer below leaves e(b) for the matrix layer
      // ("splicing it here would take e(b) away from the matrix layer"). It was
      // not: `matrix T = r(table)` — sktest's own posted matrix, which
      // handlers_m3.cpp:1978 already knows how to copy — died HERE with
      // "r(table) is not set", because this loop only ever looked in
      // rStrings/rValues and then declared the name unknown; nothing
      // downstream could see the reference to answer it.
      //
      // Keyed on the VERB, not on the name alone, so the change reaches
      // exactly the surface that can consume a matrix. `display r(table)` is
      // still this loop's to refuse, and keeps the refusal it always gave —
      // widening the exemption to every line would have swapped that for a
      // stranger message on a line the matrix layer cannot help either.
      if (rx == &reRRef() && matrixVerbLine && session.findClassMatrix('r', name) != nullptr)
        continue;
      // `matrix list r(NAME)` names a MATRIX: r(mean) after summarize is none,
      // and StataBE answers r(111) `matrix r(mean) not found` (2026-11-03
      // oracle, BUG-1409), so the handler answers it, never a spliced value.
      if (rx == &reRRef() && matrixListLine) continue;
      // BUG-560 / BUG-629: the same exemption on ANY verb when every reference
      // to the posted matrix on the line is a matrix OPERAND (el(r(NAME),i,j),
      // r(NAME)[i,j], rowsof/colsof(r(NAME)); see cutRMatrixOperands) --
      // substituteMatrixRefs answers those, and only ever sees them if this loop
      // leaves them in place. `local F1=el(r(singleresults),1,4)` (paper 044),
      // `display rowsof(r(table))` and `local x = r(table)[1,1]` (w2c_postest.log
      // family (1)). A bare `display r(table)` still refuses, below.
      if (rx == &reRRef() && session.findClassMatrix('r', name) != nullptr &&
          !usesRRefAsScalar(line, name))
        continue;
      const std::string* s = session.rStrings.find(name);
      if (s != nullptr) {
        // BUG-374(1): the two spellings mean different things and used to share
        // this arm.
        //   `r(name)'  is MACRO EXPANSION — raw text substitution, Stata's own
        //              semantics, and it must stay raw (it is how a varlist gets
        //              spliced into a command).
        //   r(name)    is a VALUE reference in an EXPRESSION, so a string value
        //              has to arrive as a string LITERAL.
        // Splicing it raw made `display r(...)` re-evaluate the text, which
        // failed loudly for a value the arithmetic grammar cannot parse
        // (r(datasignature): "336:34(...)" — ':' is outside the whitelist) and,
        // worse, could go SILENT: a single-word value such as r(varlist) with one
        // variable resolved as a DATASET VARIABLE and printed that column's first
        // observation instead of the name.
        // AN-6: a value holding '"' arrives as Stata's compound literal `"..."',
        // the one spelling that can carry it (`strlen(r(labels))` over coefplot's
        // `1 `"First regressor"' ...`, w2an_coefplot_family.log:355). The old
        // `\"` escape is not Stata (BUG-502: no escapes in a literal) and ended
        // the string at the first inner quote. A value is never macro text, so
        // its own ` and $ are spliced encoded.
        if (rx == &reRRef()) {
          const std::string body = verbatim ? encodeVerbatim(*s) : *s;
          const std::string lit = s->find('"') == std::string::npos
                                      ? "\"" + body + "\""
                                      : "`\"" + body + "\"'";
          line = replaceFirst(line, full, lit);
        } else {
          line = replaceFirst(line, full, *s);
        }
      } else if (const double* v = session.rValues.find(name)) {
        // repr(float(val)); a missing r() (meanonly's r(sd)) goes in as its token.
        // FU-842: the bare r(NAME) is a value (one operand); `r(NAME)' is macro text.
        line = replaceFirst(line, full,
                            barePass ? valueRefText(matrixElementText(*v)) : macroRealText(*v));
      } else if (auto why = unpostedRRefusal(session, name)) {
        // RULING.md C.1 rule 1: a result Stata posts for the live r-class command
        // and PIET did not -- refused in every form and on every path.
        dprof::ezoneEnd(dprof::kEExpandRBlock);
        return rcErrorResult(*why, "111");
      } else if (unsetRAsMissing) {
        // Stata: an unset r() in a numeric expression is missing (`.`). Used by
        // the if/while condition path so `!missing(r(estimate))` guards work
        // after a failed capture (the `.` becomes the missing sentinel in
        // evalBoolExpr's replaceStataMissingLiterals). No error — this is legal Stata.
        line = replaceFirst(line, full, ".");
      } else if (rx == &reRRef()) {
        // BUG-629: a posted MATRIX used as a scalar (`display r(table)`). It is
        // set, so "is not set" was the wrong reason, and rc 1 no code at all.
        // r(109) "type mismatch": w2f_residuals.log:1625
        // `PREC|e_rtable_display|0|109|` (and the mirror case, a scalar where a
        // matrix is needed, w2c_postest.log:377-384).
        if (session.findClassMatrix('r', name) != nullptr) {
          dprof::ezoneEnd(dprof::kEExpandRBlock);
          return rcErrorResult("type mismatch: r(" + name +
                                   ") is a matrix; read it as r(" + name +
                                   ")[i,j], rowsof(), colsof(), or copy it with `matrix`",
                               "109");
        }
        // An estimation (or margins, or a replicating prefix) after which Stata
        // posts r(table) but this build does not -- interaction (#) columns, say --
        // leaves the reason in EvalSession::rTableRefusal (handlers_aer_estim.cpp),
        // so a read is a NAMED refusal instead of the r(198)/r(109) of a table
        // Stata would not have either.
        if (name == "table" && !session.rTableRefusal.empty()) {
          dprof::ezoneEnd(dprof::kEExpandRBlock);
          return rcErrorResult(session.rTableRefusal, "111");
        }
        // On a MATRIX verb an absent name is the matrix layer's to answer, as a
        // posted one is (the exemption at the top of this loop): `matrix T =
        // r(nosuch)` is rc 0 in Stata (w2f_residuals.log:1675) with content no arm
        // pins yet, and that grammar lives in handlers_m3.cpp runMatrixDef.
        // A `matrix NAME = ...` definition reads an absent r(NAME) as the missing
        // value, a 1x1 `.` matrix (w2f_residuals.log:1675 `matrix T = r(nosuch)`
        // rc 0; w2n_followups.log:938 E05, :1027 E10, :1043 E11 `capture matrix
        // __w2nT = r(table)` rc 0 after r-class commands that post no table).
        if (matrixVerbLine) {
          static const std::regex reMatDefine(R"(^\s*mat(?:r|ri|rix)?\s+[A-Za-z_\x80-\xff][\w\x80-\xff]*\s*=)");
          if (std::regex_search(line, reMatDefine)) line = replaceFirst(line, full, ".");
          continue;
        }
        // RULING.md C.1 rule 2, after the matrix-verb hand-off (R5) and before
        // the 198/109 below, which assert a Stata error this build cannot know
        // Stata raises after an undeclared command.
        if (auto why = unknownSetRRefusal(session, name)) {
          dprof::ezoneEnd(dprof::kEExpandRBlock);
          return rcErrorResult(*why, "111");
        }
        const std::string rc = absentRMatrixRc(line, name);
        if (!rc.empty()) {
          dprof::ezoneEnd(dprof::kEExpandRBlock);
          return rcErrorResult(rc == "198" ? "invalid syntax: r(" + name + ") is not set"
                                           : "type mismatch: r(" + name + ") is not set",
                               rc);
        }
        // An r() name outside the posted set of the live r-class command (or
        // after a class-X command: an rclass program's return set, an emptied
        // r()) is the missing value in an expression, at rc 0 (w2n_followups.log:968
        // E07 prints `.` for r(N)/r(mean) after `regress`; RULING.md C.1 rule 3).
        // It was a refusal at rc 1 here, and `local v = r(N)` then stored the TEXT
        // `r(N)` (FU-91). The display twin, `generate` and the `count if` CiParser
        // see the line after this pass, so they inherit the `.`.
        line = replaceFirst(line, full, ".");
      } else {
        // The delimited form `r(NAME)' is MACRO expansion: outside the live
        // command's posted set an unset name expands to the empty string at rc 0,
        // as an undefined local and an unset `e(NAME)' do (the e() block below;
        // BUG-294). After an undeclared command it refuses (C.1 rule 2).
        if (auto why = unknownSetRRefusal(session, name)) {
          dprof::ezoneEnd(dprof::kEExpandRBlock);
          return rcErrorResult(*why, "111");
        }
        line = replaceFirst(line, full, std::string());
      }
    }
  }
  dprof::ezoneEnd(dprof::kEExpandRBlock);

  // --- e() references: the DELIMITED form `e(name)' only (BUG-409). The BARE
  // form e(name) is an EXPRESSION reference and belongs to resolveEstScalarRefs
  // further down, which knows to leave a matrix name alone; splicing it here
  // would take e(b) away from the matrix layer. Strings win over scalars, as
  // they do for r(): `e(cmd)` is a macro, `e(N)` is a number, and the store a
  // name lives in is what decides.
  if (line.find("`e(") != std::string::npos) {
    for (const auto& [full, name] : findAll(reERefDelim(), line)) {
      if (const std::string* es = session.eStrings.find(name)) {
        line = replaceFirst(line, full, *es);
      } else if (const double* ev = session.eScalars.find(name)) {
        line = replaceFirst(line, full, macroRealText(*ev));  // BUG-1006
      } else {
        // Stata macro semantics: an unset e() macro reference expands to the
        // empty string at rc 0 — the same rule BUG-294 measured for an
        // undefined local, and the reason `if "`e(nosuch)'" == ""` works.
        const std::string sub = resolveEScalar(name, session);
        line = replaceFirst(line, full,
                            sub == "." ? std::string() : capNumericTextToStataLocalDigits(sub));
      }
    }
  }

  // --- c() references (BUG-1006): delimited form `c(name)' only. The bare
  // c(name) is an expression reference and belongs to substituteCRefs at the
  // expression call sites. Every hk_*_g record of w2aq_histogram_family.log
  // prints `c(k)' inside a display string. A name Stata has no c() value for
  // expands to "", as in Stata (isStataCClassName). A real c() name outside
  // resolveCValue's spine is refused here, by name and with its code, as the
  // bare form is: left in the line it was stored as raw text by `local x
  // "`c(name)'"` (FU-1413), and a nested `c(`k')' never converged.
  // A numeric value is spliced as macro text, the stored-local rule `r(N)'
  // takes: `c(maxdouble)' is 8.9884656743e+307 and `c(pi)' 3.141592653589793
  // (StataBE 19.5). The hook's own text keeps every bit, for the inline
  // `=c(epsdouble)*2^52' (substituteBareCRefs), which is 1.
  if (session.cValue && line.find("`c(") != std::string::npos) {
    for (const auto& [full, name] : findAll(reCRefDelim(), line)) {
      std::optional<std::string> cv = session.cValue(name);
      if (!cv && !isStataCClassName(name)) cv = std::string();
      if (!cv) return rcErrorResult(cClassRefusalMessage(name), cClassRefusalRc(name));  // coded: `local` stops too
      if (!isStataCClassStringName(name) && !cv->empty()) {
        char* end = nullptr;
        const double d = std::strtod(cv->c_str(), &end);
        if (end == cv->c_str() + cv->size() && std::isfinite(d)) cv = stataMacroRealText(d);
      }
      line = replaceFirst(line, full, *cv);
    }
  }

  // --- s() references (P4, sreturn local): delimited form `s(name)' only —
  // same site as r()/e() just above. Values live in session.rStrings (the
  // same store r()-strings use) under kSClassKeyPrefix so they can never
  // collide with a real r()-name (those are captured by an identifier-only
  // regex — reRRefDelim/reRRef above — so no r()-set key can ever equal our
  // prefixed form). An unset s() name is a macro that expands to nothing, on
  // every path: the s() store holds exactly what sclass programs posted, and
  // regress, logit, lincom, ... empty it (BUG-1512; StataBE 19.5, rlife/s3.do:
  // `[`s(nosuch)']` is `[]` and `if "`s(foo)'" == ""` is true after `regress`).
  for (const auto& [full, name] : (line.find('`') != std::string::npos &&
                                   line.find("s(") != std::string::npos)
                                     ? findAll(reSRefDelim(), line)
                                     : std::vector<std::pair<std::string, std::string>>{}) {
    const std::string* s = session.rStrings.find(std::string(kSClassKeyPrefix) + name);
    line = replaceFirst(line, full, s != nullptr ? *s : std::string());
  }

  // --- local macro references: `name' ---
  //
  // P2-PERF ARC-A/A3-②: ONE left-to-right splice into one output buffer, in
  // place of a regex scan that collected every match plus a replaceFirst per
  // match — and replaceFirst built a whole new string each time
  // (`s.substr(0,pos) + to + s.substr(pos+n)`), so `local acc = `acc' + `i''
  // allocated three strings to substitute two macros. This is the single
  // hottest text operation in the interpreter: it runs on every line of every
  // loop body.
  //
  // The name grammar is reLocalRef()'s, transcribed rather than approximated:
  // an identifier ([A-Za-z_][A-Za-z0-9_]*, plus the bytes >= 0x80 of a
  // Unicode name -- isIdentStart/isIdentChar) or a positional digit run ([0-9]+),
  // closed by an apostrophe. Anything else after a backtick is not a reference,
  // and — exactly like the regex iterator — scanning resumes one character
  // past that backtick, so a nested `` `x' `` inside a non-reference is still
  // found.
  //
  // Ordering note, deliberate and equivalent: the old code searched the
  // PROGRESSIVELY MODIFIED line for each match's literal text, so a macro whose
  // VALUE happened to contain another reference could have that inner reference
  // consumed in this same pass. The splicer never rescans a substituted value.
  // Both converge to the same line, because expandMacros loops until no
  // reference remains — only the intermediate differs.
  if (line.find('`') != std::string::npos) {
    std::string out;
    out.reserve(line.size() + 32);
    size_t i = 0;
    bool spliced = false;
    while (i < line.size()) {
      const size_t tick = line.find('`', i);
      if (tick == std::string::npos) {
        out.append(line, i, std::string::npos);
        i = line.size();
        break;
      }
      size_t j = tick + 1;
      // EM3a: `macval(NAME)' inserts NAME's value WITHOUT expanding the
      // references inside it (w2af_macro_text_file.log:983 mt_mv_tick `<p`mtl'q>`,
      // :995 mt_mv_dollar `<p$mtg q>`, mt_mv_undef `<>` for an undefined local).
      // A plain `NAME' re-expands them on the next pass (mt_mv_tick_plain
      // `<pLVALq>`). The NAME grammar is the reference's own; anything else after
      // `macval(` is not this form and is copied through like any non-reference.
      if (line.compare(j, 7, "macval(") == 0) {
        size_t k = j + 7;
        const size_t mvStart = k;
        if (k < line.size() && isIdentStart(line[k])) {  // Unicode names: shared rule
          ++k;
          while (k < line.size() && isIdentChar(line[k])) ++k;
        } else {
          while (k < line.size() && std::isdigit(static_cast<unsigned char>(line[k]))) ++k;
        }
        if (k > mvStart && k + 1 < line.size() && line[k] == ')' && line[k + 1] == '\'') {
          const std::string* mv = session.locals.find(line.substr(mvStart, k - mvStart));
          out.append(line, i, tick - i);
          if (mv != nullptr) out += verbatim ? encodeVerbatim(*mv) : *mv;
          i = k + 2;
          spliced = true;
          continue;
        }
      }
      const size_t nameStart = j;
      if (j < line.size() && isIdentStart(line[j])) {  // Unicode names: shared rule
        ++j;
        while (j < line.size() && isIdentChar(line[j])) ++j;
      } else if (j < line.size() && std::isdigit(static_cast<unsigned char>(line[j]))) {
        ++j;
        while (j < line.size() && std::isdigit(static_cast<unsigned char>(line[j]))) ++j;
      }
      if (j == nameStart || j >= line.size() || line[j] != '\'') {
        out.append(line, i, tick + 1 - i);  // not a reference: copy through the backtick
        i = tick + 1;
        continue;
      }
      const std::string name = line.substr(nameStart, j - nameStart);
      const std::string* v = session.locals.find(name);
      // BUG-294: an UNDEFINED macro reference expands to the empty string at
      // rc 0 — measured against Stata 19.5 IC (`display "[\`nosuchlocal']"` ->
      // `[]`, _rc 0; the global arms below likewise). PIET used to hard-error
      // and STOP the do-file here, which turned BUG-216/246/247's deliberate
      // leave-unset-on-error into a downstream halt the oracle does not have,
      // and derailed the `capture` + inspect idiom real do-files are written
      // in. Nothing downstream is weakened: an empty expansion that leaves a
      // syntactically broken line still errors, on the line Stata errors on.
      static const std::string kEmpty;
      if (v == nullptr) v = &kEmpty;
      out.append(line, i, tick - i);
      out += *v;
      i = j + 1;
      spliced = true;
    }
    if (spliced) {
      if (i < line.size()) out.append(line, i, std::string::npos);
      line = std::move(out);
    }
  }

  // --- global macro references: ${name} then $name ---
  for (const auto& [full, name] : line.find('$') != std::string::npos
                                      ? findAllGlobalRefs(reGlobalRefBrace(), line)
                                      : std::vector<std::pair<std::string, std::string>>{}) {
    const std::string* v = session.globals.find(name);
    static const std::string kEmpty;  // BUG-294 — see the local arm above.
    if (v == nullptr) v = &kEmpty;
    line = replaceFirst(line, full, *v);
  }
  for (const auto& [full, name] : line.find('$') != std::string::npos
                                      ? findAllGlobalRefs(reGlobalRefBare(), line)
                                      : std::vector<std::pair<std::string, std::string>>{}) {
    const std::string* v = session.globals.find(name);
    static const std::string kEmpty;  // BUG-294 — see the local arm above.
    if (v == nullptr) v = &kEmpty;
    line = replaceFirst(line, full, *v);
  }

  ExpandResult r;
  r.line = line;
  return r;
}

// ---------------------------------------------------------------------------
// expand_macros

// LBL-DOLLAR: Stata's `\$` escape -- a literal dollar that is never a global
// reference. It travels as the verbatim-dollar sentinel and decodes to `$`.
static std::string encodeDollarEscapes(const std::string& s) {
  if (s.find("\\$") == std::string::npos) return s;
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == '$') {
      out += kVerbatimDollar;
      ++i;
    } else {
      out += s[i];
    }
  }
  return out;
}

ExpandResult expandMacros(const std::string& rawLine, EvalSession& session,
                          bool unsetRAsMissing, bool keepVerbatim) {
  std::string line = encodeDollarEscapes(rawLine);

  for (int pass = 0; pass < kMaxExpansionPasses; ++pass) {
    if (!hasMacroRefs(line)) break;  // fully resolved
    ExpandResult result = expandPass(line, session, unsetRAsMissing, /*verbatim=*/true);
    if (result.isError) return result;  // propagate error immediately
    if (result.line == line) break;     // no progress — stuck
    line = result.line;
  }

  // Compound double-quotes `"..."' are STRING delimiters. EM3b: a span whose body
  // holds no '"' becomes a plain "..." (what the tokenizer and every literal
  // scanner read); one that holds a quote, or nests, stays compound -- the old
  // non-greedy regex ended such a span at its first inner "' and handed the
  // evaluator `strlen("a "b" c")` (w2af mt_fn_strlen, w2k do:697). The scan
  // counts a macval()-encoded tick as a tick, so a value's own `"..."' nests.
  // BUG-1527: a fold only shortens the line; the unfolded one is kept for r.typed.
  std::string typedLine;
  {
    std::string folded = normalizeCompoundQuotes(line);
    if (folded.size() != line.size()) typedLine.swap(line);
    line = std::move(folded);
  }

  std::string lineNoEBuf;  // storage for the stripped copy, when one is needed
  // Convergence guard — e() refs not in _e_scalars are NOT a failure (the
  // display-path resolver owns them); strip them before the check.
  //
  // P2-PERF ARC-A/A3-b: this regex_replace ran on EVERY expanded line,
  // unguarded — the one regex execution a line with no macro references at all
  // could not avoid. `local q = 5` never enters the pass loop (hasMacroRefs is
  // false on the first look) and still paid for it. A line with no "e(" cannot
  // contain an e() reference, so the strip is the identity and the check can
  // read the line itself.
  // BUG-507: a class reference inside a string literal is TEXT, and the splices
  // above now leave it -- so the guard reads a copy with the parentheses inside
  // literals blanked. Macro references there stay visible on purpose.
  // BUG-1006: a `c(name)' the c() pass could not resolve is left for the
  // unsupported-macro refusal downstream, which names it; it is not a runaway.
  // Cut before the mask: blanking its parentheses would leave `c_name_', which
  // reads as a local reference.
  std::string lineNoCBuf;
  if (line.find("`c(") != std::string::npos) {
    lineNoCBuf = std::regex_replace(line, reCRefDelim(), "");
  }
  const std::string lineMasked = maskParensInQuotes(lineNoCBuf.empty() ? line : lineNoCBuf);
  const std::string& lineNoE = hasBareEParen(lineMasked)  // EXPRLAYER-3
                                   ? (lineNoEBuf = std::regex_replace(lineMasked, reERef(), ""))
                                   : lineMasked;
  // B4-W3b: the SAME exemption for an r-class MATRIX reference. The bare-r()
  // pass above deliberately leaves `r(table)` / `r(Stat1)` in place for the
  // matrix layer (see its comment), so without this the guard would read an
  // intentionally-unexpanded reference as a runaway macro and report a
  // circular definition for a line that is simply not the macro layer's to
  // finish. On a matrix verb the pass leaves every bare r(NAME) it does not
  // splice -- a posted matrix, and an absent name, which the handler answers
  // (BUG-1409: `matrix list r(nosuch)` is r(111) `matrix r(nosuch) not found`
  // in StataBE, whether or not any r() matrix is posted) -- so all of them go.
  std::string lineNoRBuf;
  const std::string* checkLine = &lineNoE;
  if (hasBareRParen(*checkLine) && (isEstaddLine(line) || isMatrixVerbLine(line))) {
    // W2AK: the bare r() pass leaves an estadd line's r() references for the
    // handler (isEstaddLine); they are not a runaway.
    lineNoRBuf = std::regex_replace(*checkLine, reRRef(), "");
    checkLine = &lineNoRBuf;
  } else if (hasBareRParen(*checkLine) && !session.rMatrices.empty()) {
    // BUG-560 / BUG-629: the bare-r() pass also leaves a posted matrix's OPERAND
    // spellings (el(r(NAME),i,j), r(NAME)[i,j], rowsof/colsof(r(NAME))) for
    // substituteMatrixRefs. Strip only those, the same cut the pass decided by,
    // so any other r() left on the line still reads as unexpanded.
    lineNoRBuf = *checkLine;
    for (const auto& [nm, mtx] : session.rMatrices) {
      (void)mtx;
      lineNoRBuf = cutRMatrixOperands(lineNoRBuf, nm);
    }
    checkLine = &lineNoRBuf;
  }
  if (hasMacroRefs(*checkLine)) {
    ExpandResult r;
    r.isError = true;
    r.error = nlohmann::ordered_json{
        {"type", "error"},
        {"code", "engine_error"},
        {"message",
         "Macro expansion did not converge after " + std::to_string(kMaxExpansionPasses) +
             " passes. This usually indicates a self-referential or circular macro "
             "definition. Line after expansion: " + pyRepr(decodeVerbatim(line))}};
    return r;
  }

  ExpandResult r;
  r.line = keepVerbatim ? line : decodeVerbatim(line);
  if (!typedLine.empty()) r.typed = keepVerbatim ? typedLine : decodeVerbatim(typedLine);
  return r;
}

ExpandResult expandDefinitionText(const std::string& text, EvalSession& session) {
  std::string line = encodeDollarEscapes(text);
  for (int pass = 0; pass < kMaxExpansionPasses; ++pass) {
    if (!hasMacroRefs(line)) break;
    ExpandResult result = expandPass(line, session, false, /*verbatim=*/true);
    if (result.isError) {
      if (result.error.is_object() && result.error.contains("message") &&
          result.error["message"].is_string())
        result.error["message"] = decodeVerbatim(result.error["message"].get<std::string>());
      return result;
    }
    if (result.line == line) break;
    line = result.line;
  }
  ExpandResult r;
  r.line = decodeVerbatim(std::move(line));
  return r;
}

// ---------------------------------------------------------------------------
// _try_extended_macro_fn

namespace {

// BUG-216/217 helpers: the variable-metadata plane SessionState projects into
// eval.varLabels under reserved keys (see SessionState::syncEvalVarLabels).
//
// MUST stay byte-identical to session_state.h's kEvalVar*Prefix constants — the
// same discipline kSClassKeyPrefix above documents for the s-class store, and for
// the same reason: two spellings of one key is a silent-miss generator.
const char* const kVarExistsPrefix = "\x01v:x:";
const char* const kVarFormatPrefix = "\x01v:f:";
const char* const kVarLblsetPrefix = "\x01v:l:";
const char* const kVarTypePrefix = "\x01v:t:";  // BUG-250: declared storage subtype
// The AUTHORITY sentinel. Its presence means "a SessionState has projected this
// plane, so absence of a variable's exists-key really does mean the variable does
// not exist". Absence means no SessionState ever synced — the evalcase harness
// and the session-less selftest callers, which write eval.varLabels by hand — and
// there r(111) is UNDECIDABLE, exactly as it was before this fix. So the reject
// is gated on it and the fallback is the pre-fix lenient answer: one-sided by
// construction, and it cannot turn a harness fixture into an error.
const char* const kVarPlaneKey = "\x01v:@";

bool varPlaneIsAuthoritative(const EvalSession& session) {
  return session.varLabels.find(std::string(kVarPlaneKey)) != nullptr;
}
// true = the variable is known ABSENT (and only ever true when the plane is
// authoritative, so a session-less caller never reaches an r(111)).
bool varKnownMissing(const EvalSession& session, const std::string& name) {
  if (!varPlaneIsAuthoritative(session)) return false;
  return session.varLabels.find(std::string(kVarExistsPrefix) + name) == nullptr;
}

// BUG-718: every `VAR[i]` in a macro-context expression that names a dataset
// variable (EvalSession::varObs) and not a matrix (a one-index subscript of a
// matrix stays for substituteMatrixRefs, as before) is read at observation i:
//   - a numeric value becomes `(value)` and the expression evaluates on;
//   - a STRING value, or a missing one, is answered only when the subscript is
//     the WHOLE expression (`` `=s[1]' `` -- the form the w2c_functions records
//     print): *whole gets the text verbatim and the caller returns it. This
//     layer's whitelist admits no string literal, so inside a larger expression
//     the reference is left in place, exactly as before this fix.
// The index is an integer literal (after `_N`/`_n`) or an expression this layer
// evaluates to an integer; anything else is left in place. Outside 1.._N the
// value is missing (Stata [U] 13.7: `x[0]`, `x[100]` copy a missing value) --
// `.` for a numeric column, "" for a string one (the reader reports the kind).
// Quoted text is data: string literals are protected around the scan.
// `stringLeft` (optional) is set when a string or missing reading was left in
// place inside a larger expression -- the caller's cue to refuse by name.
// BUG-710: `outerSpans` set = evalArith's mode. Its input has had its literals
// protected already (they ARE `outerSpans`), and EVERY reading is substituted,
// in any position, through varReadText: a string becomes a literal on
// `outerSpans`, a missing value its token, so `s[1] + 1` reaches the evaluator
// as `"ab1" + 1` and is its r(109) (w2i_varsub d_05), and `xd[_N]` is `.a`
// (a_03). `whole` is not used in that mode.
std::string substituteVarSubscripts(const std::string& in, EvalSession& session,
                                    std::optional<std::string>* whole, bool* stringLeft,
                                    std::vector<std::string>* outerSpans) {
  if (!session.varObs || in.find('[') == std::string::npos) return in;
  static const std::regex reSub(R"((^|[^\w.$`\x80-\xff])([A-Za-z_\x80-\xff][\w\x80-\xff]*)\s*\[([^\[\],]+)\])");
  static const std::regex reInt(R"(^\s*(-?\d+)\s*$)");
  std::vector<std::string> spans;
  const std::string s = outerSpans != nullptr ? in : protectStringLiterals(in, &spans);
  std::string out;
  size_t last = 0;
  bool changed = false;
  for (auto it = std::sregex_iterator(s.begin(), s.end(), reSub); it != std::sregex_iterator();
       ++it) {
    const std::smatch& m = *it;
    const std::string name = m.str(2);
    const size_t refPos = static_cast<size_t>(m.position(2));
    const size_t refEnd = static_cast<size_t>(m.position(0) + m.length(0));
    if (session.findMatrix(name) != nullptr) continue;
    std::string idxText = m.str(3);
    std::smatch im;
    if (!std::regex_match(idxText, im, reInt)) {
      std::optional<std::string> v = evalMacroExpr(idxText, session);
      if (!v || !std::regex_match(*v, im, reInt)) continue;
      idxText = *v;
      std::regex_match(idxText, im, reInt);
    }
    long long idx = 0;
    try {
      idx = std::stoll(im.str(1));
    } catch (...) {
      continue;
    }
    std::optional<EvalSession::VarObsReading> r = session.varObs(name, idx);
    if (!r) continue;
    if (outerSpans != nullptr) {
      std::optional<std::string> lit = varReadText(*r, *outerSpans);
      if (!lit) continue;
      out += s.substr(last, refPos - last);
      out += *lit;
      last = refEnd;
      changed = true;
      continue;
    }
    std::string text = r->text;
    bool isString = r->isString;
    if (!r->inRange) text = isString ? std::string() : std::string(".");
    if (isString || text.empty() || text[0] == '.') {
      if (refPos == 0 && strip(s.substr(refEnd)).empty() && spans.empty()) {
        *whole = text;
        return in;
      }
      if (stringLeft != nullptr) *stringLeft = true;
      continue;
    }
    out += s.substr(last, refPos - last);
    out += "(" + text + ")";
    last = refEnd;
    changed = true;
  }
  if (!changed) return in;
  out += s.substr(last);
  return outerSpans != nullptr ? out : restoreStringLiterals(out, spans);
}

std::optional<std::string> tryExtendedMacroFn(const std::string& rhsIn, EvalSession& session,
                                              MacroFnError* err) {
  std::string rhs = strip(rhsIn);
  if (rhs.empty() || rhs[0] != ':') return std::nullopt;  // not a function form

  // BUG-217: `variable X not found` + r(111) — Stata's own wording and code for
  // every one of these readers, captured this stint (StataNow 19.5, `capture
  // noisily`, in-log marker): `: variable label nosuchvar` and
  // `: format nosuchvar` BOTH print "variable nosuchvar not found" at r(111).
  auto rejectMissingVar = [&](const std::string& name) {
    if (err != nullptr) {
      err->rc = "111";
      err->message = "variable " + name + " not found";
    }
  };

  // r(198): a form whose arguments do not parse, a bad macro name
  // (macroNameError), or a variable reader with no variable (StataBE 19.5:
  // `: type`, `: format`, `: var l` "nothing found where name expected").
  auto rejectSyntax = [&](const std::string& message = "invalid syntax") {
    if (err != nullptr) {
      err->rc = "198";
      err->message = message;
    }
    return std::nullopt;
  };

  // The function's name, written out in full (extMacroFnName): `: lab ul 1` is
  // answered as `: label ul 1`. The second word of `value label`, `variable
  // label` and `data label` abbreviates to `l`; after value/variable anything
  // else is "<word> not allowed" r(101) (StataBE 19.5 `: variable LABEL u`,
  // `: var lbl u`), after data it is r(198).
  size_t tokEnd = 1;
  while (tokEnd < rhs.size() && isPySpace(rhs[tokEnd])) ++tokEnd;
  const size_t tokStart = tokEnd;
  while (tokEnd < rhs.size() && isIdentChar(rhs[tokEnd])) ++tokEnd;
  const std::string tok = rhs.substr(tokStart, tokEnd - tokStart);
  const std::string fn = extMacroFnName(tok);
  if (!fn.empty()) {
    std::string rest = rhs.substr(tokEnd);
    if (fn == "value" || fn == "variable" || fn == "data") {
      size_t w = 0;
      while (w < rest.size() && isPySpace(rest[w])) ++w;
      size_t we = w;
      while (we < rest.size() && isIdentChar(rest[we])) ++we;
      const std::string word = rest.substr(w, we - w);
      if (word.empty() || std::string("label").compare(0, word.size(), word) != 0) {
        if (fn == "data") return rejectSyntax();
        if (err != nullptr) {
          err->rc = "101";
          err->message = word + " not allowed";
        }
        return std::nullopt;
      }
      rest = " label" + rest.substr(we);
    }
    rhs = ": " + fn + rest;
  }
  // A form Stata knows that this layer does not answer: a refusal naming it, the
  // r(101) an unknown function gets.
  auto refuseUnanswered = [&]() {
    if (err != nullptr) {
      std::string head = strip(rhs.substr(1));
      head = head.substr(0, head.find_first_of(" \t"));
      err->rc = "101";
      err->message = (head.empty() ? std::string("extended macro function") : head) + " not allowed";
    }
    return std::nullopt;
  };

  std::smatch m;

  // These are names of the actual live postings, in publication order. No
  // sorting, deleted entries, or zero/missing placeholders are manufactured.
  // Fixed oracle: w2v_estpost_ematrix.log ep13_ematrices / ep13_escalars.
  // `e(` with anything else is r(198) (StataBE 19.5 `: e(SCALARS)`, `: e(foo)`,
  // `: e()`, `: e`), except Stata's own `: e(functions)`, not answered here.
  static const std::regex reENames(R"(^:\s*e\(\s*(scalars|macros|matrices)\s*\)\s*$)");
  if (tok == "e" && !std::regex_match(rhs, m, reENames)) {
    static const std::regex reEFunctions(R"(^:\s*e\(\s*functions\s*\)\s*$)");
    return std::regex_match(rhs, reEFunctions) ? refuseUnanswered() : rejectSyntax();
  }
  if (std::regex_match(rhs, m, reENames)) {
    std::string kind = m.str(1), out;
    // A fit that published Stata's ordered list for this kind (and whose e() is
    // still exactly that posting) answers in Stata's order, which is not the
    // stores' insertion order (SessionState::eNameOrderFor).
    if (session.eNameOrder) {
      if (const auto names = session.eNameOrder(kind)) {
        for (const auto& n : *names) {
          if (!out.empty()) out += ' ';
          out += n;
        }
        return out;
      }
    }
    const bool complete = session.eNameListsComplete
                              ? session.eNameListsComplete(kind)
                              : !session.lastModelResult.has_value();
    if (!complete) {
      if (err != nullptr) {
        err->rc = "111";
        err->message = ": e(" + m.str(1) + ") is not available for this fit: its complete "
                       "ordered result namespace has not been published";
      }
      return std::nullopt;
    }
    auto append = [&](const std::string& name) {
      if (name.empty() || isReservedStoreKey(name)) return;
      if (!out.empty()) out += ' ';
      out += name;
    };
    if (kind == "scalars") {
      for (const auto& item : session.eScalars.items()) append(item.first);
    } else if (kind == "macros") {
      for (const auto& item : session.eStrings.items()) append(item.first);
    } else {
      for (const auto& item : session.eMatrices) append(item.first);
    }
    return out;
  }

  // name-binding-display-literals: Stata's words -- blanks separate them and a
  // double-quoted string, simple or compound, is ONE word without its quotes.
  // The split on blanks alone kept the quotes, so `local w : word 2 of `"
  // "a-b" "c-d" "'` stored `"c-d"`, which display then had to echo as a bare
  // name (w2af_macro_text_file.log:780 `<Clickbait-Incentives>`, :840 `<10>|<0.2>`
  // for a list of quoted labels, :939 `<2>|<one two>` for `"one two" three`).
  // Each element is kept both as written (`raw`, quotes on: what `: list uniq`
  // and its siblings hand back) and as compared (`text`, quotes off).
  struct ListElem {
    std::string raw, text;
  };
  const auto stataElems = [](const std::string& lst) {
    std::vector<ListElem> words;
    size_t i = 0;
    while (i < lst.size()) {
      if (std::isspace(static_cast<unsigned char>(lst[i]))) {
        ++i;
      } else if (lst[i] == '"') {
        const size_t j = lst.find('"', i + 1);
        if (j == std::string::npos) {  // unclosed: the rest is the string (StataBE
          // 19.5: `a "b c` has sizeof 2, word 2 `b c`, retok `a "b c"`)
          words.push_back({lst.substr(i) + "\"", lst.substr(i + 1)});
          break;
        }
        words.push_back({lst.substr(i, j + 1 - i), lst.substr(i + 1, j - i - 1)});
        i = j + 1;
      } else if (lst[i] == '`' && i + 1 < lst.size() && lst[i + 1] == '"') {
        int depth = 0;
        size_t j = i;
        while (j < lst.size()) {
          if (j + 1 < lst.size() && lst[j] == '`' && lst[j + 1] == '"') {
            ++depth;
            j += 2;
          } else if (j + 1 < lst.size() && lst[j] == '"' && lst[j + 1] == '\'') {
            j += 2;
            if (--depth == 0) break;
          } else {
            ++j;
          }
        }
        const size_t innerEnd = depth == 0 ? j - 2 : lst.size();
        words.push_back({lst.substr(i, j - i),
                         lst.substr(i + 2, innerEnd > i + 2 ? innerEnd - (i + 2) : 0)});
        i = j;
      } else {
        size_t j = i;
        while (j < lst.size() && !std::isspace(static_cast<unsigned char>(lst[j]))) ++j;
        words.push_back({lst.substr(i, j - i), lst.substr(i, j - i)});
        i = j;
      }
    }
    return words;
  };
  const auto stataWords = [&stataElems](const std::string& lst) {
    std::vector<std::string> words;
    for (auto& e : stataElems(lst)) words.push_back(std::move(e.text));
    return words;
  };

  // word count of <list>
  // serset-twoway: the list may be EMPTY after expansion (`: word count `vl''` with
  // vl empty), which leaves no blank after `count`; that is 0, not `word not
  // allowed` (the same for `word # of` below: w2al tf_ss's `local sv : word `j' of
  // `svl'' with no sort-variable list, w2al_twoway_family.do:125). `of` here is
  // a word of the list (StataBE 19.5 `: word count of a b` is 3).
  static const std::regex reWordCount(R"(^:\s*word\s+count(?:\s+([\s\S]*))?$)");
  if (std::regex_match(rhs, m, reWordCount)) {
    std::string lst = strip(m.str(1));
    auto words = lst.empty() ? std::vector<std::string>{} : stataWords(lst);
    return std::to_string(words.size());
  }

  // word N of <list>; N is a positive integer (StataBE 19.5: `: word 0 of a b`,
  // `-1`, `1.5` r(198); `01` is 1).
  static const std::regex reWordN(R"(^:\s*word\s+(\d+)\s+of(?:\s+([\s\S]*))?$)");
  if (std::regex_match(rhs, m, reWordN)) {
    long long n = 0;
    try {
      n = std::stoll(m.str(1));
    } catch (const std::exception&) {
      return std::string();  // absurd index -> out of range -> ""
    }
    if (n == 0) return rejectSyntax();
    std::string lst = strip(m.str(2));
    auto words = lst.empty() ? std::vector<std::string>{} : stataWords(lst);
    if (1 <= n && n <= static_cast<long long>(words.size())) {
      return words[static_cast<size_t>(n - 1)];
    }
    return std::string();  // out of range -> empty (Stata-faithful)
  }

  // `local` and `global`, as `: length` and `: subinstr` take them: StataBE 19.5
  // answers `l`, `lo`, `loc`, `g`, `gl`, `glob` and refuses `L`, `LOCAL` r(198).
  static const std::string kLocalOrGlobal = R"((l(?:o(?:c(?:al?)?)?)?|g(?:l(?:o(?:b(?:al?)?)?)?)?))";

  // length local|global <name>. No string form: `: length "abc"` is r(198) in
  // StataBE 19.5.
  static const std::regex reLength(R"(^:\s*length\s+)" + kLocalOrGlobal +
                                   R"(\s+([\w\x80-\xff]+)\s*$)");
  if (std::regex_match(rhs, m, reLength)) {
    const bool global = m.str(1)[0] == 'g';
    const std::string bad = macroNameError(global, m.str(2));
    if (!bad.empty()) return rejectSyntax(bad);
    return std::to_string(
        utf8Len(global ? session.globals.get(m.str(2), "") : session.locals.get(m.str(2), "")));
  }

  // subinstr local|global <name> <from> <to> [, all word count(local|global <n>)]
  // EM10: <from>/<to> are "..." or compound `"..."' (nesting, inner quotes kept),
  // and the options come in any order (w2af_macro_text_file.log :1393
  // mt_xmf_subinstr_q `say QhiQ now` for `"""' -> "Q"; :1407 count(local nch)).
  // Measured on StataBE 19.5: `word` abbreviates to `w` and `count` to `c`, `all`
  // not at all, and the names are case-sensitive (`, a`, `, ALL`, `, Word`:
  // "option a not allowed" r(198)); an option given twice is "option all not
  // allowed" / "option c() not allowed"; count() holds `local|global NAME` or is
  // r(198), or "option count() incorrectly specified" with more after the name;
  // text after <to> that is not an option is "varlist not allowed" r(101).
  static const std::regex reSubinstrHead(R"(^:\s*subinstr\s+)" + kLocalOrGlobal +
                                         R"(\s+([\w\x80-\xff]+)\s+)");
  if (fn == "subinstr") {
    if (!std::regex_search(rhs, m, reSubinstrHead)) return rejectSyntax();
    const bool fromGlobal = m.str(1)[0] == 'g';
    const std::string srcName = m.str(2);
    if (const std::string bad = macroNameError(fromGlobal, srcName); !bad.empty())
      return rejectSyntax(bad);
    size_t pos = static_cast<size_t>(m.length(0));
    auto readArg = [&](std::string* out) -> bool {
      while (pos < rhs.size() && isPySpace(rhs[pos])) ++pos;
      if (startsCompound(rhs, pos)) {
        const size_t e = compoundSpanEnd(rhs, pos);
        if (e == std::string::npos) return false;
        *out = rhs.substr(pos + 2, e - pos - 4);
        pos = e;
        return true;
      }
      if (pos < rhs.size() && rhs[pos] == '"') {
        const size_t e = rhs.find('"', pos + 1);
        if (e == std::string::npos) return false;
        *out = rhs.substr(pos + 1, e - pos - 1);
        pos = e + 1;
        return true;
      }
      return false;
    };
    std::string oldS, newS;
    if (!readArg(&oldS) || !readArg(&newS)) return rejectSyntax();
    bool allFlag = false, wordFlag = false, countFlag = false;
    std::string countKind, countName;
    while (pos < rhs.size() && isPySpace(rhs[pos])) ++pos;
    if (pos < rhs.size() && rhs[pos] != ',') {
      if (err != nullptr) {
        err->rc = "101";
        err->message = "varlist not allowed";
      }
      return std::nullopt;
    }
    const std::string opts = pos < rhs.size() ? rhs.substr(pos + 1) : std::string();
    static const std::regex reOpt(R"(\s*([\w\x80-\xff]+)(\s*\(([^)]*)\))?\s*)");
    static const std::regex reCountArg(R"(^\s*)" + kLocalOrGlobal + R"(\s+([\w\x80-\xff]+)\s*([\s\S]*)$)");
    size_t op = 0;
    std::smatch om, cm;
    while (op < opts.size() && !strip(opts.substr(op)).empty()) {
      const std::string tail = opts.substr(op);
      if (!std::regex_search(tail, om, reOpt, std::regex_constants::match_continuous))
        return rejectSyntax();
      const std::string name = om.str(1);
      const bool isAll = name == "all" && !om[2].matched;
      const bool isWord = !om[2].matched && std::string("word").compare(0, name.size(), name) == 0;
      const bool isCount = om[2].matched && std::string("count").compare(0, name.size(), name) == 0;
      bool* flag = isAll ? &allFlag : isWord ? &wordFlag : isCount ? &countFlag : nullptr;
      if (flag == nullptr || *flag)
        return rejectSyntax("option " + name + (om[2].matched ? "()" : "") + " not allowed");
      *flag = true;
      if (isCount) {
        const std::string arg = om.str(3);
        if (!std::regex_match(arg, cm, reCountArg)) return rejectSyntax();
        if (!cm.str(3).empty()) return rejectSyntax("option count() incorrectly specified");
        countKind = cm.str(1)[0] == 'g' ? "global" : "local";
        countName = cm.str(2);
        if (const std::string bad = macroNameError(countKind == "global", countName); !bad.empty())
          return rejectSyntax(bad);
      }
      op += static_cast<size_t>(om.length(0));
    }
    const std::string* srcVal = fromGlobal ? session.globals.find(srcName)
                                           : session.locals.find(srcName);
    std::string val = srcVal != nullptr ? *srcVal : std::string();
    long long nSubs = 0;
    auto setCount = [&]() {
      if (countKind.empty()) return;
      if (countKind == "global") session.globals.set(countName, std::to_string(nSubs));
      else session.locals.set(countName, std::to_string(nSubs));
    };
    if (wordFlag && !oldS.empty()) {
      // whole blank-delimited words only
      std::string out;
      size_t i = 0;
      bool done = false;
      while (i < val.size()) {
        if (val[i] == ' ') {
          out += val[i++];
          continue;
        }
        size_t e = val.find(' ', i);
        if (e == std::string::npos) e = val.size();
        const std::string w = val.substr(i, e - i);
        if (!done && w == oldS) {
          out += newS;
          ++nSubs;
          if (!allFlag) done = true;
        } else {
          out += w;
        }
        i = e;
      }
      setCount();
      return out;
    }
    if (oldS.empty()) {
      // Python str.replace("", new): inserts `new` at every position
      // (between chars and at both ends); with count=1, only at the start.
      // Codepoint boundaries, like Python.
      if (!allFlag) return newS + val;
      std::string out = newS;
      size_t i = 0;
      while (i < val.size()) {
        size_t step = 1;
        unsigned char c = static_cast<unsigned char>(val[i]);
        if (c >= 0xF0) step = 4;
        else if (c >= 0xE0) step = 3;
        else if (c >= 0xC0) step = 2;
        out += val.substr(i, step) + newS;
        i += step;
      }
      return out;
    }
    if (allFlag) {
      // replace all occurrences
      std::string out;
      size_t at = 0;
      while (true) {
        size_t hit = val.find(oldS, at);
        if (hit == std::string::npos) break;
        out.append(val, at, hit - at);
        out.append(newS);
        at = hit + oldS.size();
        ++nSubs;
      }
      out.append(val, at, val.size() - at);
      setCount();
      return out;
    }
    nSubs = val.find(oldS) == std::string::npos ? 0 : 1;
    setCount();
    return replaceFirst(val, oldS, newS);
  }

  // BUG-1377/1378: `: list` ([P] macro lists) as ONE grammar. It served uniq,
  // posof and the set operators over blank-split words with set semantics; every
  // other function (sizeof ...) was `list not allowed` r(101), and duplicates and
  // quoted elements came out unlike Stata's. Measured on StataBE 19.5:
  //  * an element is a Stata word: a "..." or `"..."' string is one element,
  //    compared by its text and handed back as written (`a "b c" d "b c"` uniq ->
  //    `a "b c" d`; `a "a" b` uniq -> `a b`; sizeof of `a "b c" d "b c"` is 4);
  //  * the operators count duplicates: `A - B` takes one copy out of A per copy in
  //    B, from A's right end (`a b c d b` - `b d e` -> `a b c`; `a b a c a b` -
  //    `a b` -> `a b a c`); `A & B` keeps A's first min(#A, #B) copies (`a a b` &
  //    `a a a` -> `a a`); `A | B` is A then what `B - A` leaves (`a a b` | `a a a`
  //    -> `a a b a`); `A === B` is the same copies in any order (`a a b` === `a b`
  //    is 0) and `A in B` needs every copy (`a a` in `a b` is 0);
  //  * sort/rsort order by the text, stably (`z "b c" a "a"` -> `a "a" "b c" z`);
  //    clean re-adorns each element minimally (`"zz"` -> `zz`; `` `"x y"' `` ->
  //    `"x y"`) and an empty one as nothing, with one blank before every element
  //    once the result is non-empty (`"" "" a` -> `a`; `a "" b` -> `a  b`; `a ""`
  //    -> `a `, blank kept); retok[enize] hands every element back as written;
  //  * an operand is a macro NAME (bare or local(), or global()); a missing one is
  //    an empty list (`: list sizeof` is 0, `: list A -` is A); a name Stata
  //    cannot store is r(198) "<name> invalid name" (macroNameError: `global(1A)`,
  //    a 32-character local); posof's element must be quoted; the function names
  //    are case-sensitive and anything else (`: list foo A`, `: list Sort A`,
  //    `: list A ~ B`, `: list A - B C`) is r(198) "invalid syntax".
  // The w2al helper tf_ss1 runs `local rest : list vl - sv` outside capture for
  // every sorted serset (w2al_twoway_family.do:54); posof is BUG-1006's
  // (w2aq_histogram_family.log, the positions field of every hk_*_g record).
  static const std::regex reListFn(R"(^:\s*list(?:\s+([\s\S]*))?$)");
  if (std::regex_match(rhs, m, reListFn)) {
    const std::string body = strip(m.str(1));
    // Stata's tokens here: a run of name characters, `==`, or any other single
    // character; an operand is the token where one is due, so `- A` is "_-
    // invalid name" and `A ====B` "_== invalid name" (the operand after `==` is
    // the token `==`), while `A ===B` is `===`. posof's quoted element is read
    // before the rest is cut into tokens.
    std::vector<std::string> toks;
    std::string posofElem;
    bool posof = false;
    {
      size_t from = 0;
      if (body.compare(0, 5, "posof") == 0 && (body.size() == 5 || isPySpace(body[5]))) {
        posof = true;
        size_t q = 5;
        while (q < body.size() && isPySpace(body[q])) ++q;
        if (startsCompound(body, q)) {
          const size_t e = compoundSpanEnd(body, q);
          if (e == std::string::npos) return rejectSyntax();
          posofElem = body.substr(q + 2, e - q - 4);
          from = e;
        } else if (q < body.size() && body[q] == '"') {
          const size_t e = body.find('"', q + 1);
          if (e == std::string::npos) return rejectSyntax();
          posofElem = body.substr(q + 1, e - q - 1);
          from = e + 1;
        } else {
          return rejectSyntax();
        }
      }
      for (size_t i = from; i < body.size();) {
        if (isPySpace(body[i])) {
          ++i;
          continue;
        }
        size_t j = i;
        while (j < body.size() && isIdentChar(body[j])) ++j;
        if (j == i) j = i + (body.compare(i, 2, "==") == 0 ? 2 : 1);
        toks.push_back(body.substr(i, j - i));
        i = j;
      }
    }
    size_t k = 0;  // the next token
    // The operand at toks[k] (a local NAME, local(NAME) or global(NAME)) into
    // *out; none left is an empty list. False when it is not one (err is set).
    std::string operandError;
    const auto operand = [&](std::vector<ListElem>* out) {
      out->clear();
      if (k == toks.size()) return true;
      std::string name = toks[k++];
      const bool global = name == "global";
      const bool wrapped = (global || name == "local") && k < toks.size() && toks[k] == "(";
      if (wrapped) {
        ++k;
        name = k < toks.size() ? toks[k++] : std::string();
      }
      operandError = macroNameError(global && wrapped, name);
      if (operandError.empty() && wrapped) {
        if (k < toks.size() && toks[k] == ")") ++k;
        else operandError = "invalid syntax";
      }
      if (!operandError.empty()) return false;
      *out = stataElems(global && wrapped ? session.globals.get(name, "") : session.locals.get(name, ""));
      return true;
    };
    const auto join = [](const std::vector<ListElem>& es) {
      std::string out;
      for (const auto& e : es) {
        if (!out.empty()) out += ' ';
        out += e.raw;
      }
      return out;
    };
    const auto counts = [](const std::vector<ListElem>& es) {
      std::map<std::string, size_t> n;
      for (const auto& e : es) ++n[e.text];
      return n;
    };
    // The first quota[text] copies of each element of `es`, in order.
    const auto keepFirst = [](const std::vector<ListElem>& es, std::map<std::string, size_t> quota) {
      std::vector<ListElem> out;
      for (const auto& e : es) {
        auto it = quota.find(e.text);
        if (it != quota.end() && it->second > 0) {
          out.push_back(e);
          --it->second;
        }
      }
      return out;
    };
    // What is left of `a` once one copy per copy in `b` is taken out (from the right).
    const auto minus = [&](const std::vector<ListElem>& a, const std::vector<ListElem>& b) {
      auto quota = counts(a);
      const auto nb = counts(b);
      for (auto& [text, n] : quota) {
        const auto it = nb.find(text);
        n -= std::min(n, it == nb.end() ? size_t{0} : it->second);
      }
      return keepFirst(a, quota);
    };
    const auto isListFn = [](const std::string& t) {
      return t == "uniq" || t == "dups" || t == "sort" || t == "rsort" || t == "clean" ||
             t == "sizeof" || (t.size() >= 5 && std::string("retokenize").compare(0, t.size(), t) == 0);
    };
    std::vector<ListElem> a, b;
    if (posof) {
      if (toks.empty() || toks[k++] != "in") return rejectSyntax();
      if (!operand(&a)) return rejectSyntax(operandError);
      if (k != toks.size()) return rejectSyntax();
      for (size_t i = 0; i < a.size(); ++i)
        if (a[i].text == posofElem) return std::to_string(i + 1);
      return std::string("0");
    }
    if (!toks.empty() && isListFn(toks[0])) {
      const std::string lf = toks[k++];
      if (!operand(&a)) return rejectSyntax(operandError);
      if (k != toks.size()) return rejectSyntax();
      if (lf == "sizeof") return std::to_string(a.size());
      if (lf == "uniq" || lf == "dups") {
        std::vector<ListElem> out;
        std::set<std::string> seen;
        for (const auto& e : a)
          if (seen.insert(e.text).second == (lf == "uniq")) out.push_back(e);
        return join(out);
      }
      if (lf == "sort" || lf == "rsort") {
        const bool up = lf == "sort";
        std::stable_sort(a.begin(), a.end(), [up](const ListElem& x, const ListElem& y) {
          return up ? x.text < y.text : y.text < x.text;
        });
        return join(a);
      }
      if (lf == "clean") {
        std::string out;
        for (const auto& e : a) {
          if (!out.empty()) out += ' ';
          const bool blank = std::any_of(e.text.begin(), e.text.end(), [](unsigned char c) {
            return std::isspace(c) != 0;
          });
          const bool quote = e.text.find('"') != std::string::npos;
          out += quote ? "`\"" + e.text + "\"'" : blank ? "\"" + e.text + "\"" : e.text;
        }
        return out;
      }
      return join(a);  // retokenize
    }
    if (toks.empty()) return rejectSyntax();
    if (!operand(&a)) return rejectSyntax(operandError);
    if (k == toks.size()) return rejectSyntax();
    std::string op = toks[k++];
    if (op == "==" && k < toks.size() && toks[k] == "=") {
      op = "===";
      ++k;
    }
    if (op != "-" && op != "|" && op != "&" && op != "==" && op != "===" && op != "in")
      return rejectSyntax();
    if (!operand(&b)) return rejectSyntax(operandError);
    if (k != toks.size()) return rejectSyntax();
    if (op == "-") return join(minus(a, b));
    if (op == "|") {
      auto out = a;
      for (const auto& e : minus(b, a)) out.push_back(e);
      return join(out);
    }
    if (op == "&") {
      auto quota = counts(a);
      const auto nb = counts(b);
      for (auto& [text, n] : quota) {
        const auto it = nb.find(text);
        n = std::min(n, it == nb.end() ? size_t{0} : it->second);
      }
      return join(keepFirst(a, quota));
    }
    if (op == "==") {
      bool same = a.size() == b.size();
      for (size_t i = 0; same && i < a.size(); ++i) same = a[i].text == b[i].text;
      return std::string(same ? "1" : "0");
    }
    if (op == "===") return std::string(counts(a) == counts(b) ? "1" : "0");
    return std::string(minus(a, b).empty() ? "1" : "0");  // in
  }

  // The variable readers given no variable (StataBE 19.5 r(198) for `: type`,
  // `: format`, `: var l`, `: val l`).
  static const std::regex reNoVar(R"(^:\s*(?:type|format|variable label|value label)\s*$)");
  if (std::regex_match(rhs, reNoVar)) return rejectSyntax("nothing found where name expected");

  // variable label <varname> — NOTE: requires the leading ':' (^: not ^:?).
  //
  // BUG-208: `session.varLabels` is NOT a store this layer's callers are expected
  // to have filled by hand. Until that fix its only writer in the whole tree was
  // the evalcase test harness, so under the live dispatcher it was a permanently
  // empty map and this returned "" for a label the session had just set and
  // `describe` was displaying — a legal value, indistinguishable from "no label",
  // and `if "\`l'" == ""` is the idiomatic use, so do-files branched wrong in
  // silence. It is now a PROJECTION of SessionState's live varMeta/resident card,
  // refreshed by SessionState::syncEvalVarLabels() at the dispatcher call site
  // just above tryCaptureDefinition. This layer stays as it was, and deliberately:
  // piet::eval sits BELOW SessionState and must not learn about it.
  //
  // The "" return therefore now means what it says for a variable that EXISTS and
  // has no label, which is Stata-faithful.
  //
  // BUG-217 closes the other half: for a variable that does NOT exist this used to
  // return "" as well, conflating "no label" with "no variable", so a loop over a
  // mistyped varlist produced empty labels instead of stopping at the typo. Stata
  // raises r(111). The distinction is decidable now only because the projection
  // carries an explicit existence marker — the label map alone could not express
  // it, since an empty label was simply not stored.
  //
  // BUG-1379: both words abbreviate, `var`..`variable` then `l`..`label` (paper
  // 069 writes `: var lab y`; extMacroFnName writes them out); StataBE 19.5
  // serves `var l`, `variable la`, `var labe` and refuses `va lab` ("va not
  // allowed") and `VAR LAB` ("VAR not allowed"), r(101).
  static const std::regex reVarLabel(R"(^:\s*variable label\s+([\w\x80-\xff]+)\s*$)");
  if (std::regex_match(rhs, m, reVarLabel)) {
    if (varKnownMissing(session, m.str(1))) {
      rejectMissingVar(m.str(1));
      return std::nullopt;
    }
    return session.varLabels.get(m.str(1), "");
  }

  // BUG-216: `: format <varname>` and `: value label <varname>`.
  //
  // Both were unimplemented, and the unrecognized-function tail below returned the
  // LITERAL MACRO TEXT — so `local f : format x` put `: format x` into `f`, which
  // `format v \`f'` then applies as a garbage format with no diagnostic anywhere.
  // `describe` renders the format correctly in the same session, so the
  // information always existed and was reachable; this was a missing READER, and
  // it now reads the same projected card `describe` renders.
  //
  // Oracle, StataNow 19.5, `StataBE -b`, in-log marker, on
  // `gen double x = _n` + `format x %9.3f` + a `gv` labelled `mylbl`:
  //     : format x            -> [%9.3f]      : format nosuchvar     -> r(111)
  //     : value label x       -> []  rc 0     : value label gv       -> [mylbl]
  //                                           : value label nosuchvar-> r(111)
  // Note `: value label x` on a variable with no value label is "" at rc 0, NOT an
  // error — the same shape as `: variable label` on an unlabelled variable, which
  // is why the absent-key default is "" and only a MISSING VARIABLE rejects.
  //
  // `: type` is deliberately absent, and its absence is a measurement, not an
  // oversight: this engine does not record Stata numeric subtypes for
  // engine-created variables at all. Live-probed this stint — `gen double x` and
  // `gen float y` both land as dtype "f64" and `describe` reports BOTH as "float",
  // because VarMeta::stataType is populated only by a .dta import. Stata answers
  // `: type x` with "double". Implementing it off what we have would emit "float"
  // there: a silent wrong answer replacing a visible non-answer, which is the
  // trade this whole stint exists to avoid. Filed as BUG-250 with the probe.
  // Until then `: type` takes the r(101) tail — honest, and wrong in the direction
  // that stops a do-file instead of feeding it a bad value.
  static const std::regex reVarFormat(R"(^:\s*format\s+([\w\x80-\xff]+)\s*$)");
  if (std::regex_match(rhs, m, reVarFormat)) {
    if (varKnownMissing(session, m.str(1))) {
      rejectMissingVar(m.str(1));
      return std::nullopt;
    }
    return session.varLabels.get(std::string(kVarFormatPrefix) + m.str(1), "%9.0g");
  }
  // EM13 / LBL-CHAR: `: char NAME[CHAR]` -- the characteristic's text, "" when it
  // is unset ([P] char; notes.ado keeps noteN/note0 there: w2ao log:1630
  // lb_note_gereg; after `xtset, clear` `_dta[_TSpanel]` is "", w2d_uncovered.log
  // :314 xtset_clear). A NAME that is neither _dta nor a variable is r(111).
  static const std::regex reCharFn(
      R"(^:\s*char\s+([A-Za-z_\x80-\xff][\w\x80-\xff]*)\s*\[\s*([\w\x80-\xff]*)\s*\]\s*$)");
  if (std::regex_match(rhs, m, reCharFn)) {
    const std::string target = m.str(1);
    if (target != "_dta" && varKnownMissing(session, target)) {
      rejectMissingVar(target);
      return std::nullopt;
    }
    if (m.str(2).empty() || !session.characteristic) return std::string();
    return session.characteristic(target, m.str(2)).value_or(std::string());
  }
  // LBL-DATALABEL: `: data label` -- the dataset label, "" when none is set
  // (w2ao_label_family.log lb_data, lb_data_clear).
  // `: sortedby` -- the variables the data are sorted by, "" when none
  // (w2d_uncovered.log merge_sorted_ctl `|id|`, merge_sorted_unsorted_master `||`).
  static const std::regex reSortedBy(R"(^:\s*sortedby\s*$)");
  if (std::regex_match(rhs, m, reSortedBy)) {
    return session.sortedBy ? session.sortedBy() : std::string();
  }
  // BUG-441: `: pwd` -- the working directory, what `pwd` and c(pwd) answer
  // (paper 020's master opens with `global rootdir : pwd`). StataBE 19.5 ignores
  // anything after the word (`: pwd x`, `: pwd(x)`) and refuses `: PWD` r(101).
  if (fn == "pwd") {
    if (!session.cwd.empty()) return session.cwd;
    std::error_code ec;
    const std::filesystem::path cwd = std::filesystem::current_path(ec);
    return ec ? std::string() : cwd.u8string();
  }
  static const std::regex reDataLabel(R"(^:\s*data label\s*$)");
  if (std::regex_match(rhs, m, reDataLabel)) {
    return session.dataLabel ? session.dataLabel() : std::string();
  }
  // BUG-1379: `val`..`value` then `l`..`label` (StataBE 19.5: `val l u`, `value
  // labe u` -> the set name; `va l u` r(101)).
  static const std::regex reVarLblset(R"(^:\s*value label\s+([\w\x80-\xff]+)\s*$)");
  if (std::regex_match(rhs, m, reVarLblset)) {
    if (varKnownMissing(session, m.str(1))) {
      rejectMissingVar(m.str(1));
      return std::nullopt;
    }
    return session.varLabels.get(std::string(kVarLblsetPrefix) + m.str(1), "");
  }

  // FU-1111: `: label lblname # [#2] [, strict]`, `: label (varname) ...` and
  // `: label ... maxlength` ([P] macro, "label valuelabelname"). The text # maps
  // to in the set; # itself when the set is undefined, the variable has no value
  // label or # is unmapped (nothing under `strict'); #2 truncates. The returned
  // text is trimmed: w2ah_data_verbs.log ah_encode_spaces reads a label whose
  // text is two blanks as [] and w2af_macro_text_file.log mt_lab_def_069 reads
  // `"Other "' as <Other>. Oracle arms: ah_rename_lab, ah_ren_lab, ah_clone_byte,
  // ah_encode_gen/g/generate/label_new/label_again/label_exist/spaces,
  // ah_recode_g_lab. Before, this fell to the r(101) tail below. Layer: the
  // macro/arith layer only (the set is read through EvalSession::valueLabelSet).
  // Measured on StataBE 19.5: # is a number or a missing code (`: label ul abc`
  // is r(198), `1e0`, `+1`, `01` read as 1, `.a` its own code); `maxlength` and
  // `strict` are case-sensitive, strict abbreviates to `s`, and maxlength takes
  // no #2 or option (`MAXLENGTH`, `maxl`, `maxlength, strict` r(198)); a third
  // word is "varlist not allowed" r(101); `: label(u) 1` needs no blank.
  static const std::regex reLabelFn(
      R"(^:\s*label\s*(\(\s*[\w\x80-\xff]+\s*\)|[\w\x80-\xff]+)\s+([^,]*?)\s*(?:,\s*(\w*)\s*)?$)");
  if (std::regex_match(rhs, m, reLabelFn)) {
    std::string setName = m.str(1);
    const std::string args = strip(m.str(2));
    const std::string opt = m.str(3);
    if (!opt.empty() && std::string("strict").compare(0, opt.size(), opt) != 0)
      return rejectSyntax("option " + opt + " not allowed");
    const bool strict = !opt.empty();
    if (setName.front() == '(') {
      const std::string var = strip(setName.substr(1, setName.size() - 2));
      if (varKnownMissing(session, var)) {
        rejectMissingVar(var);
        return std::nullopt;
      }
      setName = session.varLabels.get(std::string(kVarLblsetPrefix) + var, "");
    }
    const std::map<std::string, std::string>* set =
        setName.empty() || !session.valueLabelSet ? nullptr : session.valueLabelSet(setName);
    const std::vector<std::string> words = args.empty() ? std::vector<std::string>{} : pySplit(args);
    if (words.size() == 1 && words[0] == "maxlength" && !m[3].matched) {
      size_t mx = 0;
      if (set != nullptr)
        for (const auto& kv : *set) mx = std::max(mx, kv.second.size());
      return std::to_string(mx);
    }
    if (words.size() > 2) {
      if (err != nullptr) {
        err->rc = "101";
        err->message = "varlist not allowed";
      }
      return std::nullopt;
    }
    static const std::regex reCode(R"(^([+-]?(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?|\.[a-z]?)$)");
    if (words.empty() || !std::regex_match(words[0], reCode)) return rejectSyntax();
    std::string key = words[0];
    {
      // labelDefs keys are the integer codes as std::to_string writes them.
      char* end = nullptr;
      const double v = std::strtod(words[0].c_str(), &end);
      if (end != nullptr && *end == '\0' && std::isfinite(v) && v == std::floor(v) &&
          std::fabs(v) < 2147483648.0)
        key = std::to_string(static_cast<long long>(v));
    }
    std::string text;
    bool mapped = false;
    if (set != nullptr) {
      auto it = set->find(key);
      if (it != set->end()) {
        text = strip(it->second);
        mapped = true;
      }
    }
    if (!mapped) text = strict ? std::string() : words[0];
    if (words.size() == 2) {
      char* end = nullptr;
      const long n2 = std::strtol(words[1].c_str(), &end, 10);
      if (end == nullptr || *end != '\0' || n2 < 0) {
        if (err != nullptr) {
          err->rc = "198";
          err->message = "invalid syntax";
        }
        return std::nullopt;
      }
      if (text.size() > static_cast<size_t>(n2)) text = text.substr(0, static_cast<size_t>(n2));
    }
    return text;
  }

  // BUG-250: `: type <varname>` — the reader half of the storage-subtype plane.
  //
  // BUG-216 left this at the r(101) tail ON PURPOSE, because the engine
  // recorded no subtype for its own columns and answering `float` for a
  // `gen double` would have been a silent wrong value. The plane now records
  // the DECLARED subtype at creation (dispatcher gen/egen arms ->
  // VarMeta::stataType) and SessionState projects it here under kVarTypePrefix
  // with describe's own fallback (numeric-without-declaration = "float", which
  // is also Stata's `gen` default). Oracle (exprfns_contract_oracle.log, J
  // rows): double/float/byte/int/long/str8 answered verbatim; undeclared `gen
  // z = _n` -> float; `: type nosuchvar` -> r(111) "variable ... not found".
  //
  // A variable the projection has NO type row for — a string column whose
  // width was never declared or imported (encode/tostring/CSV output) — is
  // refused "type not allowed" r(101), deliberately: its true `str#` width is
  // not recorded anywhere in this engine, and a fabricated one would be the
  // exact silent-wrong-answer BUG-216 exists to prevent. Named in BUG-250's
  // ledger close as the measured remainder.
  static const std::regex reVarType(R"(^:\s*type\s+([\w\x80-\xff]+)\s*$)");
  if (std::regex_match(rhs, m, reVarType)) {
    if (varKnownMissing(session, m.str(1))) {
      rejectMissingVar(m.str(1));
      return std::nullopt;
    }
    if (const std::string* t = session.varLabels.find(std::string(kVarTypePrefix) + m.str(1))) {
      return *t;
    }
    // An unrecorded subtype (or a plane that never synced) keeps the honest
    // r(101) reject.
    return refuseUnanswered();
  }

  // WS-C (C-5): matrix introspection. BUG-629 adds the FULL-name forms
  // (`: rowfullnames M` / `: colfullnames M`, w2c_postest.log:102/107/757 over
  // copies of r(table) and of nlcom's posted e(b)) and a class-matrix argument
  // (`r(table)`, `e(NAME)`). The stripes are stored as Stata prints them,
  // equation prefix included, so a full name is the stored string.
  // eb-order T-P1: the short forms answer the name WITHOUT its equation, as
  // Stata does (w2g_ebmat.log:963 e6_te_colnames_eb `r1vs0.yb` for the posted
  // `ATE:r1vs0.yb`), and an e() matrix is EvalSession::resolveEMatrix's: e(b)
  // and e(V) of a fit are its canonical stripes (w2g_ebmat.log:230 `x1 x2 _cons`)
  // at rc 0, no longer the BUG-629 r(111) "copy it first" divergence.
  // gfx-binscatter: `: roweq M` / `: coleq M` answer the equation part of each
  // stripe name and `_` for a name without one (w2k_binscatter.log:248
  // BS_ctl0_c1_names `_`, :621 BS_by_c1_names `by1 by2` over `by1:= by2:=`).
  // `, quoted` (`q`..`quoted`) quotes each name as StataBE 19.5 does: `"r1"' `"r
  // 2"' for rownames, "e1" "e2" for coleq, `""e1":c1"' for colfullnames over
  // e1:c1 and `"x"' for a full name with no equation; `, QUOTED` is "option
  // QUOTED not allowed" r(198).
  static const std::regex reMatNames(
      R"(^:\s*(rownames|colnames|rowfullnames|colfullnames|roweq|coleq)\s+([A-Za-z_\x80-\xff][\w\x80-\xff]*|[er]\(\s*[A-Za-z_\x80-\xff][\w\x80-\xff]*\s*\))\s*(?:,\s*(\w*)\s*)?$)");
  if (std::regex_match(rhs, m, reMatNames)) {
    const std::string arg = m.str(2);
    const std::string opt = m.str(3);
    if (!opt.empty() && std::string("quoted").compare(0, opt.size(), opt) != 0)
      return rejectSyntax("option " + opt + " not allowed");
    const bool quoted = !opt.empty();
    const EvalMatrix* mat = nullptr;
    std::optional<EvalMatrix> resolved;
    if (arg.find('(') == std::string::npos) {
      mat = session.findMatrix(arg);
      if (mat == nullptr) return std::string();  // undefined matrix -> ""
    } else {
      const size_t open = arg.find('(');
      const std::string inner = strip(arg.substr(open + 1, arg.size() - open - 2));
      int rc = 111;
      std::string why;
      if (arg[0] == 'e') {
        resolved = session.resolveEMatrix(inner, &rc, &why, EvalSession::EReader::kNames);
        if (resolved.has_value()) mat = &*resolved;
        // `: colnames e(b)` with no e(b) is r(111) "matrix e(b) not found"
        // with or without a live e() (w2v_estpost_ematrix.log ep13_colnames_eb,
        // ep15c_colnames_eb, ep15f_colnames_eb) -- not matrix-copy's r(301).
        if (mat == nullptr && rc == 301) {
          rc = 111;
          why = "matrix e(" + inner + ") not found";
        }
      } else {
        mat = session.findClassMatrix(arg[0], inner);
        why = "matrix r(" + inner + ") not found";
      }
      // A class matrix that is not there is a named refusal at the resolver's
      // rc (111, or an e(V) the engine did not send), never the "" of an empty
      // stripe.
      if (mat == nullptr) {
        if (err != nullptr) {
          err->rc = std::to_string(rc);
          err->message = why;
        }
        return std::nullopt;
      }
    }
    const std::string which = m.str(1);
    const bool full = which.find("full") != std::string::npos;
    const bool eq = which.compare(which.size() - 2, 2, "eq") == 0;
    const std::vector<std::string>& names = which.rfind("row", 0) == 0 ? mat->rownames : mat->colnames;
    std::string out;
    for (size_t i = 0; i < names.size(); ++i) {
      if (i) out += " ";
      const size_t colon = names[i].find(':');
      if (eq) {
        const std::string e = colon == std::string::npos ? std::string("_") : names[i].substr(0, colon);
        out += quoted ? "\"" + e + "\"" : e;
        continue;
      }
      std::string name = colon == std::string::npos || full ? names[i] : names[i].substr(colon + 1);
      if (quoted && full && colon != std::string::npos)
        name = "\"" + names[i].substr(0, colon) + "\"" + names[i].substr(colon);
      out += quoted ? "`\"" + name + "\"'" : name;
    }
    return out;
  }
  static const std::regex reExtColsof(R"(^:\s*colsof\s+([A-Za-z_\x80-\xff][\w\x80-\xff]*)$)");
  if (std::regex_match(rhs, m, reExtColsof)) {
    const EvalMatrix* mat = session.findMatrix(m.str(1));
    if (mat == nullptr) return std::string("0");
    return std::to_string(mat->data.empty() ? 0 : mat->data[0].size());
  }
  static const std::regex reExtRowsof(R"(^:\s*rowsof\s+([A-Za-z_\x80-\xff][\w\x80-\xff]*)$)");
  if (std::regex_match(rhs, m, reExtRowsof)) {
    const EvalMatrix* mat = session.findMatrix(m.str(1));
    if (mat == nullptr) return std::string("0");
    return std::to_string(mat->data.size());
  }

#ifdef _WIN32
  // WP-W5: MinGW-w64 has no <fnmatch.h>. Minimal glob matcher for the ONE
  // call site below (`: dir ... files "PATTERN"`): `*`, `?`, `[set]` with
  // `!`/`^` negation and ranges, plus the FNM_PERIOD rule via the wrapper
  // (a leading '.' in the name is only matched by a literal '.' in the
  // pattern — glob.glob parity, same as the POSIX branch). The POSIX branch
  // keeps calling the system fnmatch — bit-identical macOS behavior.
  struct GlobWin {
    static bool chars(const char* p, const char* s) {
      for (;;) {
        char pc = *p;
        if (pc == '\0') return *s == '\0';
        if (pc == '*') {
          while (*p == '*') ++p;  // collapse consecutive '*'
          if (*p == '\0') return true;
          for (const char* t = s;; ++t) {
            if (chars(p, t)) return true;
            if (*t == '\0') return false;
          }
        }
        if (*s == '\0') return false;
        if (pc == '?') { ++p; ++s; continue; }
        if (pc == '[') {
          const char* q = p + 1;
          bool neg = (*q == '!' || *q == '^');
          if (neg) ++q;
          bool matched = false;
          bool first = true;
          char c = *s;
          const char* scan = q;
          while (*scan != '\0' && (first || *scan != ']')) {
            first = false;
            char lo = *scan;
            if (scan[1] == '-' && scan[2] != '\0' && scan[2] != ']') {
              if (lo <= c && c <= scan[2]) matched = true;
              scan += 3;
            } else {
              if (lo == c) matched = true;
              ++scan;
            }
          }
          if (*scan != ']') {  // unterminated set: '[' is a literal (fnmatch behavior)
            if (c != '[') return false;
            ++p; ++s; continue;
          }
          if (matched == neg) return false;
          p = scan + 1; ++s; continue;
        }
        if (pc != *s) return false;
        ++p; ++s;
      }
    }
    static bool matchPeriod(const std::string& pattern, const std::string& name) {
      if (!name.empty() && name[0] == '.' &&
          (pattern.empty() || pattern[0] != '.')) {
        return false;  // FNM_PERIOD: wildcards never match a leading dot
      }
      return chars(pattern.c_str(), name.c_str());
    }
  };
#endif

  // U7/S9: `: dir "DIRNAME" files "PATTERN"` — directory glob.
  // `file`..`files`; `dir`..`dirs` and `other` are Stata's too and not answered
  // here (StataBE 19.5: `: dir . file *` answers, `: dir . FILES *` r(198)).
  static const std::regex reDir(
      R"re(^:\s*dir\s+(?:"([^"]*)"|(\S+))\s+(files?|dirs?|other)\s+(?:"([^"]*)"|(\S+)))re");
  if (std::regex_search(rhs, m, reDir)) {  // re.match = prefix match
    if (m.str(3)[0] != 'f') return refuseUnanswered();
    std::string dirPart = m[1].matched ? m.str(1) : (m[2].matched ? m.str(2) : ".");
    std::string pattern = m[4].matched ? m.str(4) : (m[5].matched ? m.str(5) : "*");
    try {
      namespace fs = std::filesystem;
      std::string base = !session.cwd.empty() ? session.cwd : fs::current_path().u8string();
      fs::path dir(dirPart);
      if (dir.is_relative()) dir = cecon::to_native_path(base) / dir;
      dir = dir.lexically_normal();
      std::vector<std::string> names;
      std::error_code ec;
      for (const auto& entry : fs::directory_iterator(dir, ec)) {
        std::string name = entry.path().filename().u8string();
        // FNM_PERIOD: '*'/'?' don't match a leading '.', like glob.glob.
#ifdef _WIN32
        if (GlobWin::matchPeriod(pattern, name)) {
#else
        if (fnmatch(pattern.c_str(), name.c_str(), FNM_PERIOD) == 0) {
#endif
          names.push_back(name);
        }
      }
      std::sort(names.begin(), names.end());
      std::string out;
      for (size_t i = 0; i < names.size(); ++i) {
        if (i) out += " ";
        out += names[i];
      }
      return out;
    } catch (const std::exception&) {
      return std::string();
    }
  }

  // BUG-216, and this is the more important half because it is GENERIC: it applies
  // to EVERY extended macro function this engine has not implemented, not only the
  // three named in the entry.
  //
  // This used to `return std::nullopt`, which sent the caller on to its
  // store-the-literal-RHS path — so `local f : notafunction x` stored the ten
  // characters `: notafunction x`. That is a third outcome, and worse than either
  // of the other two: not the answer, not empty, not an error, and no consumer can
  // tell it from data. BUG-208's failure at least returned "", a value the variable
  // might legitimately have had.
  //
  // Stata's answer is an ERROR — so the fall-through was never a lenient version
  // of Stata's behaviour, it was a different one. Oracle (StataNow 19.5, `capture
  // noisily`, in-log marker): `: notafunction x` -> "notafunction not allowed",
  // r(101); a bare `: nosuchfn` with no argument -> r(101) as well.
  //
  // BLAST RADIUS, measured before landing rather than assumed. Every
  // `local|global NAME : ...` line in tests/ and waves/ was enumerated and
  // classified. The only ones PIET actually executes are
  // internal script p2perf_bench.do:144-149 — `word count`, `word N of`,
  // `length local`, `subinstr local`, `list uniq`, `variable label` — all six
  // implemented arms above. The forms that are NOT implemented
  // (`: list xvars - v`, `: colnames e(b)`, `: colfullnames e(b)`) appear only in
  // tests/validate/*.do and tests/eval/golden/*.do, which are STATA-side truth
  // capture scripts: each declares "Stata truth" in its own header, each has a
  // sibling .log, they use Stata-only commands (`sysuse`, SSC `csdid`), and
  // run_eval.py never invokes a .do file at all (it reads golden CSVs). So zero
  // PIET-executed lines depended on the literal echo, and nothing was consuming
  // the echoed text as data.
  //
  // A function this layer answers whose arguments did not parse is Stata's
  // r(198) "invalid syntax" (`: word COUNT a`, `: data LABEL`, `: sortedby x`).
  if (!fn.empty()) return rejectSyntax();
  // Stata names the offending FUNCTION token, not the whole tail, cut at the
  // first character a name cannot hold (`: E(scalars)` "E not allowed"). The
  // r()/s() readers are Stata's and not answered here: named in full.
  if (tok.empty() || ((tok == "r" || tok == "s") && tokEnd < rhs.size() && rhs[tokEnd] == '('))
    return refuseUnanswered();
  if (err != nullptr) {
    err->rc = "101";
    err->message = tok + " not allowed";
  }
  return std::nullopt;  // unknown function form
}

// BUG-246: `local/global NAME = <expr>` where <expr> fails to evaluate.
//
// The fall-through used to store the raw text — so `local e = 1%2` put the
// three characters `1%2` into `e`, the same literal-echo family BUG-216 closed
// on the colon forms, and Stata's answer is an ERROR that stops the do-file.
// The fallback is still load-bearing for exactly two shapes, and only those:
//   * a STRING-literal RHS (`local x = "a:b c[1]"` — starts with `"` or the
//     compound `` `" ``): a legal Stata string expression this evaluator may
//     not cover, so it keeps evaluating-by-unquoting exactly as before;
//   * a BARE-IDENTIFIER NameError (`local x = mpg`): in Stata that reads the
//     variable's first observation — a capability this layer does not have,
//     and the variable plane is not synced at every call site, so erroring
//     here could reject a legitimate dataset variable. Unclaimed, unchanged.
// Everything else converts to the rc the oracle assigns
// (exprfns_contract_oracle.log, section H — each mapped row is a captured
// fact): `r(NNN):` channel from a function (string("abc",fmt) -> 109) wins;
// `scalar(UNDEF)` -> 111 "UNDEF not found"; unknown FUNCTION -> 133 "unknown
// function f()"; NAME[...] with no such matrix -> 111 "NAME not found"; an
// empty or otherwise unparseable expression -> 198 "invalid syntax" (Stata's
// own message for the bare `local x =`; its 198 texts vary by shape — `1%2
// invalid name`, `invalid '%'` — and only the CODE is claimed here, the
// message stays the generic one).
//
// Returns true when converted (err filled); false = keep the legacy fallback.
// Gated on err != nullptr, so every session-less caller (the evalcase
// differential harness, the selftest corpus) keeps byte-identical behaviour —
// the same gate BUG-216 used for the colon forms.
// Review finding (v3): true when `e` uses one of Stata's logical operators
// `&`, `|` or a bare `!` (not `!=`) outside a "..." string literal.
static bool hasStataLogicalOperator(const std::string& e) {
  bool inStr = false;
  for (size_t i = 0; i < e.size(); ++i) {
    const char c = e[i];
    if (c == '"') inStr = !inStr;
    if (inStr) continue;
    if (c == '&' || c == '|') return true;
    if (c == '!' && (i + 1 >= e.size() || e[i + 1] != '=')) return true;
  }
  return false;
}

// BUG-710: `session` (optional) is the caller's EvalSession. When it carries the
// variable reader (EvalSession::varObs, installed by SessionState), the bare-
// identifier shape below is no longer undecidable: evalArith has already read
// every name that IS a dataset variable (bindBareVariables), so a NameError on a
// bare name means no variable, scalar or result of that name exists, and Stata's
// answer is `NAME not found`, r(111), with nothing assigned (w2i_varsub h_03;
// h_02 for `scalar =`). Without the reader the arm stays unclaimed, as before.
bool classifyFailedEqExpr(const std::string& exprIn, const std::string& raised,
                          MacroFnError* err, const EvalSession* session = nullptr) {
  if (err == nullptr) return false;
  const std::string e = strip(exprIn);
  const bool stringLiteralRhs = !e.empty() && (e[0] == '"' || e.rfind("`\"", 0) == 0);
  // Review finding (v3): the `r(NNN):` channel is checked BEFORE the
  // string-literal exemption. A function or operator that raised it named a
  // Stata error on purpose, and a leading string literal does not make that
  // a form this evaluator failed to cover: `local a = "a" & 1` and
  // `local k = "a" + 1` are r(109) in StataBE 19.5, but the exemption stored
  // the literal text `a & 1` / `"a" + 1` with rc 0.
  static const char* kNameErrPrefix = "NameError: name '";
  // BUG-1330: a bare c(NAME) that the c() pre-pass (piet::substituteCRefs) left
  // in place is no function call. StataBE answers r(133) `c(NAME) undefined` for
  // a name it has no value for; a real name this build does not serve is
  // refused by name, with PIET's unsupported code (cClassRefusalRc; `unknown
  // function c()` before).
  const auto cClassRefusal = [&]() {
    static const std::regex reCRef(R"(\bc\(\s*([A-Za-z_]\w*)\s*\))");
    std::smatch cm;
    if (raised.rfind(std::string(kNameErrPrefix) + "c'", 0) != 0 ||
        !std::regex_search(e, cm, reCRef))
      return false;
    err->rc = cClassRefusalRc(cm.str(1));
    err->message = cClassRefusalMessage(cm.str(1));
    return true;
  };
  if (cClassRefusal()) return true;
  if (stringLiteralRhs && raised.rfind("r(", 0) != 0) {
    // FU-225 sweep (BUG-710): an unknown FUNCTION is r(133) whatever the
    // right-hand side starts with. `local z = "a" + nosuchfn_q(1)` stored the
    // text `"a" + nosuchfn_q(1)` at rc 0 under the exemption below, which is
    // for a string expression this evaluator does not cover, not for a call to
    // a function that does not exist (exprfns_contract_oracle.log:454-458).
    if (raised.rfind(kNameErrPrefix, 0) == 0) {
      const size_t start = std::string(kNameErrPrefix).size();
      const size_t end = raised.find('\'', start);
      const std::string fn = end == std::string::npos ? std::string()
                                                      : raised.substr(start, end - start);
      if (!fn.empty() && fn != "scalar" && e.find(fn + "(") != std::string::npos) {
        err->rc = "133";
        err->message = "unknown function " + fn + "()";
        return true;
      }
      // name-binding-display-literals (FU-1356): a bare name that binds to
      // nothing is r(111) beside a literal too. `local l = "a" + foo` stored
      // its own text at rc 0 and `scalar s = "a" + foo` was r(198), where
      // StataBE answers `foo not found` (w2av_name_binding bd_loc_strunb,
      // bd_sca_strunb). The reader decides, as for the bare-name shape below.
      if (!fn.empty() && fn != "scalar" && session != nullptr && session->varObs) {
        err->rc = "111";
        err->message = fn + " not found";
        return true;
      }
    }
    return false;  // string-literal RHS
  }
  if (raised.rfind("r(", 0) == 0) {
    const size_t close = raised.find(')');
    if (close != std::string::npos && close > 2) {
      const std::string digits = raised.substr(2, close - 2);
      if (!digits.empty() && digits.find_first_not_of("0123456789") == std::string::npos) {
        const size_t colon = raised.find(": ");
        err->rc = digits;
        err->message = (colon != std::string::npos) ? raised.substr(colon + 2)
                                                    : std::string("expression error");
        return true;
      }
    }
  }
  if (raised.rfind(kNameErrPrefix, 0) == 0) {
    const size_t start = std::string(kNameErrPrefix).size();
    const size_t end = raised.find('\'', start);
    const std::string name = (end == std::string::npos) ? std::string()
                                                        : raised.substr(start, end - start);
    if (name == "scalar") {
      static const std::regex reScalarArg(R"(scalar\(\s*([A-Za-z_\x80-\xff][\w\x80-\xff]*)\s*\))");
      std::smatch sm;
      if (std::regex_search(e, sm, reScalarArg)) {
        err->rc = "111";
        err->message = sm.str(1) + " not found";
        return true;
      }
    }
    if (!name.empty()) {
      if (e.find(name + "(") != std::string::npos) {
        err->rc = "133";
        err->message = "unknown function " + name + "()";
        return true;
      }
      // Review finding (v3): an unbound name under `&`, `|` or `!` keeps the
      // r(198) refusal these operators had before the lexer learned them.
      // `local c = foo | bar` over dataset variables is 1 in Stata (the
      // first observation); the literal echo stored `foo | bar` with rc 0.
      // Reading the first observation is the missing capability named
      // above, so this refuses rather than echo.
      if (!hasStataLogicalOperator(e)) {
        if (session != nullptr && session->varObs) {  // BUG-710
          err->rc = "111";
          err->message = name + " not found";
          return true;
        }
        return false;  // bare identifier — the dataset-variable shape, unclaimed
      }
      err->rc = "198";
      err->message = "invalid syntax in " + pyRepr(e);
      return true;
    }
  }
  static const std::regex reSubscript(R"(^([A-Za-z_\x80-\xff][\w\x80-\xff]*)\s*\[)");
  std::smatch sm;
  if (std::regex_search(e, sm, reSubscript)) {
    err->rc = "111";
    err->message = sm.str(1) + " not found";
    return true;
  }
  // name-binding-display-literals (FU-1359): a subscripted name that is no
  // matrix and no variable is r(111) inside a larger expression too, as it is
  // alone above: `local l = 1 + Vr[1]` and `scalar s = max(0, abs(Vr[1,1]))`
  // were r(198) (w2av_name_binding bd_loc_subabs, bd_sca_subabs2). Only with a
  // session, which can tell an absent name from a present one.
  if (session != nullptr) {
    static const std::regex reAnySub(R"((?:^|[^A-Za-z0-9_.\x80-\xff])([A-Za-z_\x80-\xff][\w\x80-\xff]*)\s*\[)");
    for (std::sregex_iterator it(e.begin(), e.end(), reAnySub), end; it != end; ++it) {
      const std::string name = (*it)[1].str();
      if (name == "_b" || name == "_se" || name == "_coef") continue;
      if (session->findMatrix(name) != nullptr) continue;
      if (session->varObs && session->varObs(name, 1)) continue;
      err->rc = "111";
      err->message = name + " not found";
      return true;
    }
  }
  // BUG-339 diagnostic. An expression that still carries a macro reference is a
  // different fact from "invalid syntax", and saying so is what turns a
  // three-week log-archaeology into a one-line read. The rc stays 198 — only
  // the CODE is claimed to match Stata (see the note above this function), the
  // message text is ours — and the offending text is named rather than
  // discarded, since it is the one thing the reader needs and the one thing
  // this function had in hand and threw away.
  err->rc = "198";
  if (e.find('`') != std::string::npos || e.find("${") != std::string::npos) {
    err->message = "invalid syntax — unresolved macro reference in " + pyRepr(e);
  } else {
    err->message = "invalid syntax in " + pyRepr(e);
  }
  return true;
}

// FU-225 sweep (BUG-710): the `= expr` forms' answer to an expansion error that
// carries no Stata code of its own (takeCodedExpandError took those): a named
// refusal at the block's coarse code, r(198) where that is the generic 1.
// Without an errorOut (session-less callers) the line is still consumed and
// nothing is stored. true = the error was taken.
bool refuseFailedExpansion(const ExpandResult& exp, MacroFnError* err) {
  if (!exp.isError) return false;
  if (err != nullptr) {
    std::string rc = coarseRc(exp.error);
    if (rc.empty() || rc == "1") rc = "198";
    err->rc = rc;
    err->message = exp.error.value("message", std::string("macro expansion failed"));
  }
  return true;
}

// _strip_outer_dquotes
std::string stripOuterDquotes(const std::string& rhs) {
  if (rhs.size() >= 2 && rhs.front() == '"' && rhs.back() == '"') {
    std::string inner = rhs.substr(1, rhs.size() - 2);
    if (inner.find('"') == std::string::npos) return inner;
  }
  return rhs;
}

}  // namespace

// BUG-293(b): public face of classifyFailedEqExpr (see macros.h).
bool classifyFailedExpr(const std::string& expr, const std::string& raised, MacroFnError* err,
                        const EvalSession* session) {
  return classifyFailedEqExpr(expr, raised, err, session);
}

// ---------------------------------------------------------------------------
// try_capture_definition

namespace {

// The corpus every EXPRLAYER drift guard below runs over. Deliberately wider
// than the benchmark: the whole point of a hand-written twin is that it must
// agree on the shapes the benchmark never produces.
const std::vector<std::string>& exprLayerCorpus() {
  static const std::vector<std::string> k = {
      // plain arithmetic (reSafeArith territory)
      "1+1", "0 + 1", "3.14*2", "2e3", "  12 / 4  ", "(1+2)^3", "1E-9", "-5", "",
      " ", "\t\n", "1\n+2",
      // extended (letters, quotes, comparisons)
      "sqrt(4)", "upper(\"abc\")", "cond(1>0,1,0)", "a<b", "x!=y", "mod(17,5)",
      "substr(\"benchmark1\",1,7)", "1,2", "'q'", "x_1", "log(2)+exp(-1)",
      // BUG-206: `%` is in the EXTENDED alphabet and NOT the basic one, so each
      // of these must answer ext=1 / basic=0 in both the scan and the regex.
      // `1%2` moved here FROM the outside-both group below when the character was
      // admitted — left in the corpus rather than deleted, because the boundary it
      // probes (a bare `%` as an operator) is exactly the one the widening had to
      // be measured against, and it is now an ext case rather than a rejected one.
      "1%2", "5 % 3", "string(42, \"%9.0g\")", "\"50%\"", "%", "strpos(\"50%\",\"%\")",
      "%9.0g", "upper(\"x%y\")",
      // characters OUTSIDE both whitelists
      "a[1]", "a{1}", "1;2", "x@y", "a\\b", "a#b", "a&b", "a|b", "a:b", "a$b",
      "`x'", "a?b", "1~2",
      // r()/e() shapes, both real and decoy
      "r(mean)", "`r(mean)'", "scalar(bp_s)", "subinstr(\"a\",\"b\",\"c\",1)",
      "e(N)", "true(1)", "size(x)", "myr(3)", "r (mean)", "_r(x)", "xe(1)",
      "e(b)", "e(V)", "1+r(N)", "sqrt(r(N))", "counter(3)",
      // scalar-command shapes
      "scalar x = 1", "scalar define x = 1", "scalar  define   x  =  1",
      "SCALAR X = 1", "Scalar define = 3", "scalar define = 3", "scalar x=1",
      "scalar x =", "scalar x =  ", "scalar x = \n", "scalar x", "scalar",
      "scalar drop x", "scalar drop x y", "scalar drop _all", "SCALAR DROP x",
      "scalar  drop   x  ", "scalar drop", "scalar drop ", "scalardrop x",
      "scalar\tdrop\tx", "scalar\ndrop\nx", "scalar list", "scalar dir",
      "scalar x = scalar(y) + 1", "scalar 1x = 2", "scalar _x = 2",
      "scalar x = a\nb", "scalar define define = 1",
      // Unicode names (lane M): UTF-8 names in every shape above. Each must
      // answer ext=1 / basic=0, and the scans must agree with their regex twins
      // (which carry the same `\x80-\xff` widening) name for name -- the
      // mixed-script `دخل_2020` is one name, never `دخل` + `_2020`.
      "ملاحظات[1]", "قيمة + 1", "upper(\"مرحبا\")", "دخل_2020 * 2", "x_ق", "ق1",
      "scalar(سعر)", "scalar(سعر) + 1", "scalar( سعر )", "مscalar(y)", "xscalar(y)",
      "scalar سعر = 5", "scalar define قيمة = 1", "SCALAR سعر=1", "scalar 1ق = 2",
      "scalar _ق = 2", "scalar drop سعر", "y ملاحظات", "yملاحظات", "ملاحظاتy", "y+سعر",
      "سعر + 1", "سعرx", "xسعر", "سعر(1)", "دخل_2020 + _2020", "_2020", "_2020y",
      "lnسعر", "log(سعر)",
  };
  return k;
}

}  // namespace

bool selftestSafeArithScanMatchesRegex(std::string* firstDisagreement) {
  for (const auto& c : exprLayerCorpus()) {
    std::smatch m;
    const bool reBasic = std::regex_match(c, m, reSafeArith());
    const bool reExt = std::regex_match(c, m, reSafeArithExt());
    if (reBasic != isSafeArith(c) || reExt != isSafeArithExt(c)) {
      if (firstDisagreement) {
        *firstDisagreement = "line=[" + c + "] regex=(" + (reBasic ? "1" : "0") +
                             (reExt ? "1" : "0") + ") scan=(" + (isSafeArith(c) ? "1" : "0") +
                             (isSafeArithExt(c) ? "1" : "0") + ")";
      }
      return false;
    }
  }
  return true;
}

bool selftestLocalRefScanMatchesRegex(std::string* firstDisagreement) {
  static const std::vector<std::string> kExtra = {
      "`x'", "`acc' + `i'", "local acc = `acc' + `i'", "`_u'", "`1'", "`12'", "`x",
      "``x''", "`'", "``'", "`x`y''", "a`b'c", "`9x'", "`x9'", "`x-y'", "`x y'",
      "no ticks here", "`", "``", "`\"a b\"'", "`=2+3'", "`r(mean)'", "`s(k)'",
      "`x' `y'", "``", "`0'", "`x_1'", "`X'", "`_'",
      // Unicode names (lane M)
      "`م'", "`ملاحظات'", "`دخل_2020'", "`ق1'", "`1ق'", "`x م'", "``م''", "`م",
  };
  auto one = [&](const std::string& c) {
    std::smatch m;
    const bool re = std::regex_search(c, m, reLocalRef());
    const bool scan = hasLocalMacroRef(c);
    if (re != scan) {
      if (firstDisagreement) {
        *firstDisagreement = "line=[" + c + "] regex=" + (re ? "yes" : "no") + " scan=" +
                             (scan ? "yes" : "no");
      }
      return false;
    }
    return true;
  };
  for (const auto& c : exprLayerCorpus()) {
    if (!one(c)) return false;
  }
  for (const auto& c : kExtra) {
    if (!one(c)) return false;
  }
  return true;
}

bool selftestBarePrefilterIsStrict(std::string* firstDisagreement) {
  // A pre-filter is sound iff it never says "no" to text the regex matches.
  // (Saying "yes" where the regex does not match only costs a wasted scan.)
  for (const auto& c : exprLayerCorpus()) {
    std::smatch m;
    if (std::regex_search(c, m, reRRef()) && !hasBareRParen(c)) {
      if (firstDisagreement) *firstDisagreement = "reRRef matched but guard said no: [" + c + "]";
      return false;
    }
    if (std::regex_search(c, m, reERef()) && !hasBareEParen(c)) {
      if (firstDisagreement) *firstDisagreement = "reERef matched but guard said no: [" + c + "]";
      return false;
    }
    if (std::regex_search(c, m, reRRefDelim()) && c.find("`r(") == std::string::npos) {
      if (firstDisagreement) {
        *firstDisagreement = "reRRefDelim matched but guard said no: [" + c + "]";
      }
      return false;
    }
  }
  return true;
}

bool selftestScalarScansMatchRegex(std::string* firstDisagreement) {
  static const std::regex reScalarDropRx(R"(^scalar\s+drop\s+(.+)$)", std::regex::icase);
  // Unicode names (lane M): `\b` is ECMAScript's ASCII word boundary, under
  // which an Arabic byte is a NON-word character -- so the twin spells the
  // boundary as the identifier class itself (group 1 = the character before),
  // which is what the scan's isIdentChar test is.
  static const std::regex reScalarCallRx(
      R"((^|[^A-Za-z0-9_\x80-\xff])scalar\(\s*([A-Za-z_\x80-\xff][\w\x80-\xff]*)\s*\))");

  // A session with a couple of scalars so the substitutions have something to
  // find, plus one name that also occurs as a function call (`scalar(...)`) so
  // the (?!\s*\() lookahead is exercised on both arms.
  EvalSession s;
  s.scalars.set("bp_s", "12.5");
  s.scalars.set("y", "7");
  s.scalars.set("log", "99");  // shadows a function name on purpose
  s.scalars.set("سعر", "3");  // Unicode names (lane M)
  s.scalars.set("_2020", "99");  // the ASCII tail of the mixed-script `دخل_2020`

  for (const auto& c : exprLayerCorpus()) {
    std::smatch m;

    // scalar( ... ) substitution
    const std::string viaRegex = regexSubFn(reScalarCallRx, c, [&s](const std::smatch& mm) {
      const std::string* v = s.scalars.find(mm.str(2));
      return mm.str(1) + (v ? valueRefText(*v) : mm.str(0).substr(mm.length(1)));
    });
    const std::string viaScan = substituteScalarCalls(c, s);
    if (viaRegex != viaScan) {
      if (firstDisagreement) {
        *firstDisagreement = "scalar() subst [" + c + "] regex=[" + viaRegex + "] scan=[" +
                             viaScan + "]";
      }
      return false;
    }

    // bare-name substitution, once per stored scalar
    for (const auto& [sn, sv] : s.scalars.items()) {
      // Identifier-class boundaries for the same reason as reScalarCallRx
      // (every name in this session begins and ends with an identifier byte).
      std::regex reName("(^|[^A-Za-z0-9_\\x80-\\xff])" + regexEscape(sn) +
                        "(?![A-Za-z0-9_\\x80-\\xff])(?!\\s*\\()");
      std::string value = sv;
      const std::string bRegex =
          regexSubFn(reName, c, [&value](const std::smatch& mm) { return mm.str(1) + value; });
      const std::string bScan = substituteBareName(c, sn, sv);
      if (bRegex != bScan) {
        if (firstDisagreement) {
          *firstDisagreement = "bare-name subst name=" + sn + " [" + c + "] regex=[" + bRegex +
                               "] scan=[" + bScan + "]";
        }
        return false;
      }
    }

    // scalar drop / scalar define recognisers
    const bool dropRe = std::regex_match(c, m, reScalarDropRx);
    const std::string dropReRest = dropRe ? m.str(1) : std::string();
    std::string dropScanRest;
    const bool dropScan = matchScalarDrop(c, dropScanRest);
    if (dropRe != dropScan || (dropRe && dropReRest != dropScanRest)) {
      if (firstDisagreement) {
        *firstDisagreement = "scalar drop [" + c + "] regex=" +
                             (dropRe ? "MATCH(" + dropReRest + ")" : "no") + " scan=" +
                             (dropScan ? "MATCH(" + dropScanRest + ")" : "no");
      }
      return false;
    }

    const bool defRe = std::regex_match(c, m, reScalarDef());
    const std::string defReName = defRe ? m.str(1) : std::string();
    const std::string defReRest = defRe ? m.str(2) : std::string();
    std::string defScanName, defScanRest;
    const bool defScan = matchScalarDef(c, defScanName, defScanRest);
    if (defRe != defScan || (defRe && (defReName != defScanName || defReRest != defScanRest))) {
      if (firstDisagreement) {
        *firstDisagreement =
            "scalar def [" + c + "] regex=" +
            (defRe ? "MATCH(" + defReName + "|" + defReRest + ")" : "no") + " scan=" +
            (defScan ? "MATCH(" + defScanName + "|" + defScanRest + ")" : "no");
      }
      return false;
    }
  }
  return true;
}

bool selftestExpansionSemantics(std::string* firstFailure) {
  // The S32-class parity corpus. Each leg is (setup-free) input -> expected
  // outcome of expandMacros on a session seeded below. These assert TODAY's
  // behavior, whatever it is — the point is that no performance change may move
  // any of them, not that any particular one is Stata-perfect.
  EvalSession s;
  s.locals.set("x", "5");
  s.locals.set("y", "mpg");
  s.locals.set("inner", "x");
  s.locals.set("empty", "");
  s.locals.set("withdollar", "$G");
  s.locals.set("1", "first");
  s.globals.set("G", "gval");
  s.rValues.set("mean", 3.5);
  s.rStrings.set("lbl", "hello");
  s.eScalars.set("N", 42.0);

  struct Leg {
    const char* in;
    bool wantError;
    const char* want;  // meaningful only when !wantError
  };
  static const Leg kLegs[] = {
      // plain reference, and a line with none at all
      {"display `x'", false, "display 5"},
      {"summarize mpg", false, "summarize mpg"},
      // an EMPTY macro substitutes nothing (not a space, not a quote)
      {"display [`empty']", false, "display []"},
      // nested: `\`inner'' resolves through two passes
      {"display ``inner''", false, "display 5"},
      // a macro whose VALUE carries a $global is expanded on the next pass
      {"display `withdollar'", false, "display gval"},
      // positional macro
      {"display `1'", false, "display first"},
      // `=expr' inline arithmetic, including one that references a macro
      {"display `=2+3'", false, "display 5"},
      {"display `=sqrt(16)'", false, "display 4"},
      // global forms, braced and bare
      {"display $G", false, "display gval"},
      {"display ${G}", false, "display gval"},
      // r() numeric and r() string (the string store wins)
      {"display r(mean)", false, "display 3.5"},
      {"display `r(lbl)'", false, "display hello"},
      // e() scalar out of the posted store
      {"display e(N)", false, "display 42.0"},
      // BUG-294: an UNDEFINED macro expands to the EMPTY STRING at rc 0 —
      // Stata 19.5 IC, measured (`display "[`nosuchlocal']"` -> `[]`, _rc 0;
      // the global forms identically). These two rows used to pin the opposite
      // (`true` = error), which is what stopped a do-file dead one line after a
      // deliberately-captured failed definition.
      {"display `nosuch'", false, "display "},
      {"display $NOSUCH", false, "display "},
      // a bare backtick that is not a reference is copied through
      {"display `", false, "display `"},
      {"display a`b", false, "display a`b"},
      // compound double-quotes are unwrapped to plain quotes
      {"display `\"a b\"'", false, "display \"a b\""},
      // an identifier ending in r/e followed by '(' is NOT a class reference
      {"display subinstr(\"a\",\"b\",\"c\",1)", false, "display subinstr(\"a\",\"b\",\"c\",1)"},
      {"display size(3)", false, "display size(3)"},
      {"scalar q = scalar(bp_s) + 1", false, "scalar q = scalar(bp_s) + 1"},
  };

  for (const auto& leg : kLegs) {
    ExpandResult r = expandMacros(leg.in, s);
    if (leg.wantError) {
      if (!r.isError) {
        if (firstFailure) *firstFailure = std::string("[") + leg.in + "] expected error, got [" +
                                          r.line + "]";
        return false;
      }
      continue;
    }
    if (r.isError || r.line != leg.want) {
      if (firstFailure) {
        *firstFailure = std::string("[") + leg.in + "] want [" + leg.want + "] got " +
                        (r.isError ? std::string("ERROR") : "[" + r.line + "]");
      }
      return false;
    }
  }
  return true;
}

bool selftestMacroDefScanMatchesRegex(std::string* firstDisagreement) {
  static const std::vector<std::string> kCases = {
      // ordinary forms
      "local q 5", "local acc = `acc' + `i'", "local x", "local x=3", "local x  =  3",
      "local mylist \"alpha beta\"", "local _u 1", "local x9 hello", "local ++ctr",
      "global G 7", "global G = 2+3", "global _g x", "global g",
      // case variants (Stata accepts them)
      "LOCAL Q 5", "Local q 5", "GLOBAL G 7", "gLoBaL g 1",
      // whitespace shapes
      "local\tq\t5", "local\t \tq 5", "local   q   5   ", "local\nq 5", "global\vg 1",
      // non-matches
      "localq 5", "local", "local ", "local  ", "local 1x 3", "local -x 3", "local `k' 5",
      // BUG-923: positional locals match `local`, never `global`
      "local 1 alpha", "local 12 \"b c\"", "local 1=3", "local 12abc 1", "local 1_ 2", "global 1 a",
      "globalg 1", "global", "global 9z 1", "scalar a = 1", "display 1", "",
      // the newline boundary: `.` cannot cross one, so the tail decides
      "local q a\nb", "global g a\nb", "local q 5\n",
      // name/rest boundaries
      "local q:word count x", "local q:", "local q,", "local q)", "global g:",
      // Unicode names (lane M): local/global names are UTF-8 too
      "local م \"abc\"", "global ج \"gval\"", "local دخل_2020 = 5", "global ق1 x",
      "local 1ق 3", "local مx=1", "LOCAL م 1", "global ٥ 3", "global €x 1", "local €x 3",
  };
  for (const auto& c : kCases) {
    for (int which = 0; which < 2; ++which) {
      const char* kw = which == 0 ? "local" : "global";
      const size_t kwLen = which == 0 ? 5 : 6;
      const std::regex& re = which == 0 ? reLocalDef() : reGlobalDef();
      std::smatch rm;
      // The `^global` regex cannot say "a Unicode LETTER"; matchMacroDef adds
      // globalNameStartOk on top of it, so the twin applies the same filter.
      const bool reOk = std::regex_match(c, rm, re) && (which == 0 || globalNameStartOk(rm.str(1)));
      std::string sName, sRest;
      const bool scanOk = matchMacroDef(c, kw, kwLen, sName, sRest);
      bool agree = (reOk == scanOk);
      if (agree && reOk) agree = (rm.str(1) == sName && rm.str(2) == sRest);
      if (!agree) {
        if (firstDisagreement) {
          *firstDisagreement = std::string(kw) + " | line=[" + c + "] regex=" +
                               (reOk ? "MATCH(" + rm.str(1) + "|" + rm.str(2) + ")" : "no") +
                               " scan=" + (scanOk ? "MATCH(" + sName + "|" + sRest + ")" : "no");
        }
        return false;
      }
    }
  }
  return true;
}

bool tryCaptureDefinition(const std::string& rawLine, EvalSession& session,
                          MacroFnError* errorOut) {
  std::string stripped = strip(rawLine);
  // AER-SCOPE1 bundle A: `loc x = 1`, `gl G "..."` and `sca de p = .05` never
  // reach a registry lookup -- a macro DEFINITION is captured here, before
  // dispatch, so an abbreviation the registry knows would still not be seen.
  // Folded onto the canonical head once, so every matcher below (the scalar
  // prefix test, reMdyn, and the two matchMacroDef calls keyed to "local" and
  // "global") keeps working unchanged. `de`/`def` are folded to `define` for
  // the same reason: matchScalarDef compares the six-character word.
  {
    size_t sp = stripped.find_first_of(" \t");
    std::string head = sp == std::string::npos ? stripped : stripped.substr(0, sp);
    std::string headLower;
    for (char c : head) headLower += static_cast<char>(std::tolower((unsigned char)c));
    const char* canon = nullptr;
    // BUG-1381: every abbreviation Stata takes (loc/loca, gl/glo/glob/globa).
    if (headLower == "loc" || headLower == "loca") canon = "local";
    else if (headLower == "gl" || headLower == "glo" || headLower == "glob" ||
             headLower == "globa")
      canon = "global";
    else if (headLower == "sca") canon = "scalar";
    if (canon != nullptr) {
      stripped = std::string(canon) + (sp == std::string::npos ? std::string() : stripped.substr(sp));
    }
    // `scalar de|def NAME = expr` -> `scalar define NAME = expr`.
    static const std::regex reScalarDefAbbrev(R"(^scalar\s+def?\s+)", std::regex::icase);
    std::smatch dm;
    if (std::regex_search(stripped, dm, reScalarDefAbbrev)) {
      stripped = "scalar define " + stripped.substr(dm[0].str().size());
    }
  }
  std::smatch m;

  // --- scalar commands: define / drop / list ---
  // A3: `^scalar\b` is a prefix test, not a search — a compare answers it
  // without building regex state. Case-insensitive like the pattern it replaces,
  // and the \b is reproduced by requiring a non-identifier char after it.
  auto isScalarPrefix = [](const std::string& t) {
    if (t.size() < 6) return false;
    static const char* kS = "scalar";
    for (int i = 0; i < 6; ++i)
      if (std::tolower(static_cast<unsigned char>(t[i])) != kS[i]) return false;
    if (t.size() == 6) return true;
    char c = t[6];
    return !isIdentChar(c);  // Unicode names: `scalarم` is one word
  };
  if (isScalarPrefix(stripped)) {
    std::string scLine = stripped;
    std::optional<ExpandResult> failedExp;
    if (hasMacroRefs(scLine)) {
      ExpandResult exp = expandPass(scLine, session);
      if (!exp.isError) scLine = exp.line;
      else failedExp = std::move(exp);
    }
    // EXPRLAYER-5: matchScalarDrop/matchScalarDef walk the same two grammars
    // left to right; selftestScalarScansMatchRegex() holds them to the regexes.
    std::string sdRest;
    if (matchScalarDrop(scLine, sdRest)) {
      auto targets = pySplit(sdRest);
      if (targets.size() == 1 && targets[0] == "_all") {
        session.scalars.clear();
        session.stringScalars.clear();  // BUG-710
      } else {
        for (const auto& t : targets) {
          session.scalars.erase(t);
          session.stringScalars.erase(t);  // BUG-710
        }
      }
      return true;
    }
    // BS-038 (BETA campaign C3): `scalar list`/`scalar dir` used to be
    // intercepted HERE and swallowed — this function's signature is
    // bool-only (tryCaptureDefinition's "did I handle this line" success
    // signal), with no way to carry a display block back to dispatch()'s
    // caller, so returning true unconditionally silenced the command
    // entirely (SCOPE BS-038: "deliberate silent no-op today"). Falling
    // through instead (removed the match here) lets the line reach the
    // dispatcher's own EXISTING `cmd=="scalar" && varlist[0] in
    // {list,dir}` arm (dispatcher.cpp), which already calls
    // handlers::runScalarList — that handler's own comment covers the
    // rest of the fix (oracle-format column alignment). reScalarDef()
    // below requires an `=`, so it does not accidentally re-swallow
    // "scalar list"/"scalar dir" either — both fall all the way through
    // this function to `return false` at the very end, unchanged.
    std::string sdName, sdExpr;
    if (matchScalarDef(scLine, sdName, sdExpr)) {
      // BUG-1490: an expansion that failed with a Stata code (the named r(111) for
      // an r() name PIET cannot read as missing) is that refusal, as on `local =`
      // and `display`. It used to reach evalArith unexpanded and come back r(133)
      // "unknown function r()".
      if (failedExp.has_value() && takeCodedExpandError(*failedExp, errorOut)) return true;
      std::string name = sdName;
      std::string expr = strip(sdExpr);
      {
        // BUG-456: an unknown coefficient name is r(111) here as everywhere
        // else (Stata V4: `scalar s = _b[nosuch]` -> r(111)); the scalar stays
        // unset, as after any failed `scalar =` under capture.
        std::string bErr;
        expr = resolveEstScalarRefs(expr, session, &bErr);
        if (!bErr.empty()) {
          if (errorOut != nullptr) {
            // BUG-456 follow-up: r(109) too (`scalar s = strlen(e(nosuch))`).
            const std::string code = splitErrCode(&bErr);
            errorOut->rc = code.substr(2, code.size() - 3);
            errorOut->message = bErr;
          }
          return true;
        }
      }
      // name-binding-display-literals (FU-1360): the stored scalars are
      // evalArith's to substitute, over literal-protected text. This branch ran
      // its own pass over the RAW right-hand side first, so a scalar's name
      // inside a string literal was replaced: with `scalar a = 7`, `scalar s =
      // "a:b"` stored `7:b` and `scalar s = "x is a"` stored `x is 7`, where
      // `local l = "a:b"` was right (w2av_name_binding bd_sca_lit, bd_sca_lit2,
      // bd_sca_litN, bd_sca_litcat, bd_sca_strvarcat).
      std::string raisedSc;
      bool scIsString = false;
      auto arith = evalArith(expr, &session, &raisedSc, &scIsString);
      // BUG-710 / FU-225 / FU-1324: an expression that FAILED to evaluate is an
      // error, never a value. The store used to keep the expression's SOURCE
      // TEXT whenever evalArith failed without an r(NNN) code (`scalar fx =
      // nosuchfn_q(1)` at rc 0, `scalar b = xd[1]` holding `xd[1]`), and every
      // later read re-evaluated that text against the data of its own moment.
      // The rc is the `local =` sibling's (classifyFailedEqExpr, the one
      // classifier; exprfns_contract_oracle.log:454-458 gives an unknown
      // function r(133)), and the scalar is neither created nor changed, as
      // after any failed `scalar =` under capture (w2i_varsub h_02). What the
      // classifier leaves unclaimed -- a string-literal right-hand side this
      // evaluator cannot finish -- is refused by name at r(198) rather than
      // stored as text; no oracle line pins that shape's rc.
      if (!arith) {
        MacroFnError scErr;
        if (!classifyFailedEqExpr(expr, raisedSc, &scErr, &session)) {
          scErr.rc = "198";
          scErr.message = "scalar " + name + " = " + expr +
                          ": the expression could not be evaluated; no scalar was stored";
        }
        if (errorOut != nullptr) *errorOut = scErr;
        return true;
      }
      session.scalars.set(name, *arith);
      // Scalars and matrices share one namespace ([P] matrix define,
      // "Namespace"): defining the scalar destroys a matrix of the same name.
      session.matrices.erase(std::remove_if(session.matrices.begin(), session.matrices.end(),
                                            [&name](const auto& p) { return p.first == name; }),
                             session.matrices.end());
      // BUG-710: the type travels with the value (EvalSession::stringScalars).
      if (scIsString) {
        session.stringScalars[name] = *arith;
      } else {
        session.stringScalars.erase(name);
      }
      return true;
    }
    // unrecognized `scalar` line — fall through (don't swallow)
  }

  // --- dynamic macro-named local/global: `local cell_`k' = ...` ---
  static const std::regex reMdyn(R"(^((?:local|global)\s+)(\S+)(.*)$)", std::regex::icase);
  // D1-①: the body below fires only when the NAME token carries a '`' or a
  // '$', so scan that token instead of running a case-insensitive alternation
  // regex over the whole line to discover it. Same A3 idiom this file already
  // uses ahead of its other guardable regexes; the regex itself is untouched
  // and still decides every line that gets past the scan.
  //
  // The first cut of this guard tested the WHOLE line for '`'/'$' and left
  // 41.8% of tryCaptureDefinition still inside std::regex on S01's form 1
  // (`local acc = `acc' + `i''), whose backtick is in the VALUE, never the
  // name — measured, then narrowed to the token that actually gates the body.
  if (macroDefNameTokenHasMacroChar(stripped)) {
  dprof::noteRegex();
  if (std::regex_match(stripped, m, reMdyn)) {
    std::string nameTok = m.str(2);
    if ((nameTok.find('`') != std::string::npos || nameTok.find('$') != std::string::npos) &&
        hasMacroRefs(nameTok)) {
      ExpandResult nameExp = expandPass(nameTok, session);
      if (!nameExp.isError && nameExp.line.find('`') == std::string::npos &&
          nameExp.line.find('$') == std::string::npos) {
        stripped = m.str(1) + nameExp.line + m.str(3);
      } else {
        // Name macro didn't resolve: surface the real undefined-macro error
        // at step 0b instead of capturing a truncated name.
        return false;
      }
    }
  }
  }  // D1-①: backtick/'$' pre-filter

  // --- local ++NAME / --NAME (case-SENSITIVE PREFIX form only) ---
  // BS-035 (D10 locked decision, BETA campaign C3): the POSTFIX form
  // (`local i++` / `local i--`) used to ALSO increment/decrement here —
  // invented semantics with no real-Stata counterpart. Real Stata has no
  // postfix `local` operator at all: `local i++` is just an ordinary
  // `local NAME rest` assignment where NAME="i" and the LITERAL text "++"
  // becomes i's new value (live-verified this stint, StataBE: `local i =
  // 7` then `local i++` -> i=[++], NOT i=[8]). Removed the postfix regex/
  // branch entirely — reLocalDef() below (`^local\s+(\w+)\s*(.*)?$`)
  // already correctly parses "local i++" as NAME="i", rest="++" (no space
  // needed — `\w*` simply stops at the non-word "+"), and its own generic
  // "no `=`, no macro refs, no quotes -> store rest verbatim" fallback
  // stores "++" byte-for-byte, so this compat change needed no new code,
  // only deleting the old postfix branch. PREFIX (`local ++i`) is
  // UNCHANGED — it IS real Stata increment syntax.
  // A3: the prefix-increment form needs a literal `++` or `--`.
  static const std::regex reIncPre(R"(^local\s+(\+\+|--)\s*([A-Za-z_\x80-\xff][\w\x80-\xff]*)\s*$)");
  if ((stripped.find("++") != std::string::npos || stripped.find("--") != std::string::npos) &&
      std::regex_match(stripped, m, reIncPre)) {
    std::string incOp = m.str(1);
    std::string incName = m.str(2);
    double val = 0.0;
    auto f = pythonFloat(session.locals.get(incName, "0"));
    if (f) val = *f;
    val += (incOp == "++") ? 1.0 : -1.0;
    session.locals.set(incName,
                        pyIsInteger(val) ? pythonIntStrFromDouble(val) : pythonFloatRepr(val));
    return true;
  }

  // --- local NAME rest ---
  // D1-①: `reLocalDef()` is still the spec (and selftest still compares this
  // scan against it, form by form); matchMacroDef is the same grammar walked
  // once, left to right, with no regex state to build and tear down.
  std::string mdName, mdRest;
  if (matchMacroDef(stripped, "local", 5, mdName, mdRest)) {
    std::string name = mdName;
    std::string rhs = strip(mdRest);

    // Compound double-quote RHS: `local x `"...."'` -- ONE span from the first
    // `" to the end (EM3b: nesting-aware, so `"a"' `"b"' is not one span). EM3a:
    // the body is expanded to a fixed point with macval() verbatim
    // (expandDefinitionText; w2af mt_mv_copy `<7>|<6>`).
    if (rhs.size() >= 4 && rhs.rfind("`\"", 0) == 0 && compoundSpanEnd(rhs, 0) == rhs.size()) {
      std::string inner = rhs.substr(2, rhs.size() - 4);
      if (hasMacroRefs(inner)) {
        ExpandResult exp = expandDefinitionText(inner, session);
        if (takeCodedExpandError(exp, errorOut)) return true;
        if (!exp.isError) inner = exp.line;
      }
      session.locals.set(name, inner);
      return true;
    }
    // Python slice rhs[2:-2] on a 3-char rhs (`"') yields "" — cover the
    // startswith/endswith overlap explicitly.
    if (rhs.size() == 3 && rhs.rfind("`\"", 0) == 0 && rhs.compare(1, 2, "\"'") == 0) {
      session.locals.set(name, "");
      return true;
    }

    // Extended macro function form: local NAME : <func> ...
    std::string extRhs = rhs;
    if (!rhs.empty() && rhs[0] == ':' && hasMacroRefs(rhs)) {
      ExpandResult extExp = expandPass(rhs, session);
      if (!extExp.isError) extRhs = extExp.line;
    }
    auto fnResult = tryExtendedMacroFn(extRhs, session, errorOut);
    if (fnResult) {
      session.locals.set(name, *fnResult);
      return true;
    }
    // BUG-216/217: a colon form that FAILED is still a form this layer consumed.
    // Return true so the caller stops treating the line as "not a definition" and
    // falling through to store-the-literal-text, and let it read errorOut to
    // surface the rc. The local is deliberately left UNSET — Stata's own behaviour
    // (measured: after `capture local z : notafunction x`, `z` is empty), so a
    // do-file that captures the error does not then find garbage in the macro.
    if (errorOut != nullptr && errorOut->failed()) return true;

    // `= expr` form
    if (!rhs.empty() && rhs[0] == '=') {
      std::string expr = strip(rhs.substr(1));
      if (hasMacroRefs(expr)) {
        // BUG-339: expandMacros, not expandPass. One pass resolves one LEVEL of
        // nesting, and `local rr = `cell_rel_`i''` has two: the pass copies the
        // outer backtick through untouched while resolving the inner `i', so
        // the RHS arrives at evalArith as `cell_rel_1' — still a live macro
        // reference, whose backtick is in neither arith whitelist. evalArith
        // returns nullopt, classifyFailedEqExpr falls through to its last
        // branch, and the line dies as a bare r(198) "invalid syntax".
        //
        // Every other consumer of a possibly-nested right-hand side already
        // loops (dispatcher's step 0b, loops.cpp's three sites). This branch
        // and its global twin were the only single-pass ones; the fix is to
        // stop being the exception, not to special-case the shape.
        //
        // The bug was invisible for three weeks because it is only reachable
        // through a do-file that nests twice: BUG-246's own reliance sweep
        // enumerated all 364 in-repo `local NAME =` lines and found no
        // double-nested RHS, and the acceptance corpus (which has exactly two)
        // is outside the repo. Before BUG-246 an unevaluable `= expr` stored
        // its literal text and dispatcher's LOOPING step-0b resolved it later —
        // so the corpus was accidentally right by deferral, and making the
        // failure honest is what exposed the missing pass here.
        ExpandResult exp = expandMacros(expr, session);
        if (takeCodedExpandError(exp, errorOut)) return true;  // BUG-629
        // FU-225 sweep (BUG-710): an expansion that failed is an error, not a
        // value. This stored the expression's own text at rc 0 (`local x =
        // `=s[1]+"z"'` kept those characters); it is refused by name now, at the
        // block's code (198 when it names none), and the local stays unset.
        if (refuseFailedExpansion(exp, errorOut)) return true;
        expr = exp.line;
      }
      std::string raised246;
      bool isStrResult246 = false;
      auto arith = evalArith(expr, &session, &raised246, &isStrResult246);
      // BUG-246: an expression that FAILED to evaluate is an error, not a
      // value — see classifyFailedEqExpr for the mapping and for the two
      // shapes that deliberately keep the legacy literal fallback. The local
      // stays UNSET on the error path (Stata: after `capture local e = 1%2`,
      // `e` is empty — oracle section H).
      if (!arith && classifyFailedEqExpr(expr, raised246, errorOut, &session)) return true;
      // NEVER store the `= expr` form with the leading `=`.
      //
      // stripOuterDquotes on the non-arithmetic fallback: in Stata `local x =
      // "abc"` stores abc, not "abc" — the `=` form EVALUATES its right-hand
      // side, and a bare string literal evaluates to its contents. Without
      // this the quotes were stored as part of the value, which `display`
      // hides (adjacent quoted segments concatenate) but a path expansion
      // does not: `local k = c(pwd)` then `use "`k'/f.csv"` produced
      //   File not found: <cwd>/"<cwd>"/f.csv
      // because c() string values arrive here already quoted by
      // substituteCRefs (correct for expression contexts, wrong to keep in a
      // macro body). The helper only strips a WHOLE "..." with no interior
      // quote, so `= "a" + "b"` and every arithmetic form are untouched.
      //
      // BUG-660 fix round: a NUMERIC arith result (not resultIsString) gets
      // Stata's 16-sig-fig macro cap here, at the local STORE site — not
      // inside evalArith, which other callers (`scalar =`) need round-trip-
      // exact (see evalArith's own comment).
      session.locals.set(name, arith ? (isStrResult246 ? *arith
                                                         : capNumericTextToStataLocalDigits(*arith))
                                      : stripOuterDquotes(expr));
      return true;
    }

    // Append / nested idiom: expand RHS refs before storing -- to a fixed point
    // (EM3a, expandDefinitionText).
    if (hasMacroRefs(rhs)) {
      ExpandResult exp = expandDefinitionText(rhs, session);
      // name-binding-display-literals (review, round 5): an expansion that
      // failed with a Stata code (an inline `=invFtail(2,3,.5)'` PIET refuses by
      // name) is that refusal, not the source text stored at rc 0. Under
      // `capture` the line reached here unexpanded and `_rc` came back 0.
      if (takeCodedExpandError(exp, errorOut)) return true;
      // X-1: Stata trims the blanks an expansion leaves at either end of an
      // unquoted value: `local tl `tl' `t'` from an empty tl stores `double`,
      // not ` double` (w2ai_graph_family.log:219; w2an cf_cmd_subgr_gb :742).
      if (!exp.isError) rhs = strip(exp.line);
      // expansion failure (undefined ref): store the literal RHS
    }

    rhs = stripOuterDquotes(rhs);
    session.locals.set(name, rhs);
    return true;
  }

  // --- global NAME rest ---
  // D1-① (same as local above).
  if (matchMacroDef(stripped, "global", 6, mdName, mdRest)) {
    std::string name = mdName;
    std::string rhs = strip(mdRest);

    // BUG-247: extended macro function form `global NAME : <func> ...` — the
    // `local` twin's dispatch, which this branch never had: every colon form
    // that answered under `local` stored its own source text under `global`
    // (`global G1 : word count a b c` -> `$G1` = `: word count a b c`, where
    // Stata answers 3 — oracle section I). Mirrors the local branch exactly,
    // including BUG-216/217's error propagation: an unimplemented function is
    // r(101), a missing variable r(111), and the global is left UNSET on the
    // error path (oracle: `$G4` is empty after the captured r(101)).
    // Blast radius measured before landing: zero `global NAME : ...` lines
    // exist in any PIET-executed do-file in the tree (the BUG-247 entry's
    // sweep, re-run this stint — the only extended-macro forms the corpus
    // executes are the six `local` ones in p2perf_bench.do).
    std::string extRhsG = rhs;
    if (!rhs.empty() && rhs[0] == ':' && hasMacroRefs(rhs)) {
      ExpandResult extExp = expandPass(rhs, session);
      if (!extExp.isError) extRhsG = extExp.line;
    }
    auto fnResultG = tryExtendedMacroFn(extRhsG, session, errorOut);
    if (fnResultG) {
      session.globals.set(name, *fnResultG);
      return true;
    }
    if (errorOut != nullptr && errorOut->failed()) return true;

    // `= expr` form — same contract as local's above, which globals were
    // missing entirely: `global x = "abc"` stored the literal `= abc` (the
    // `=` kept as part of the value and nothing evaluated), so `global n = 2+3`
    // stored `= 2+3` instead of 5. Mirror local's branch exactly.
    if (!rhs.empty() && rhs[0] == '=') {
      std::string expr = strip(rhs.substr(1));
      if (hasMacroRefs(expr)) {
        // BUG-339, global twin — see the local branch above for the mechanism.
        ExpandResult exp = expandMacros(expr, session);
        if (takeCodedExpandError(exp, errorOut)) return true;  // BUG-629, global twin
        if (refuseFailedExpansion(exp, errorOut)) return true;  // FU-225 sweep, global twin
        expr = exp.line;
      }
      std::string raised246g;
      bool isStrResult246g = false;
      auto arith = evalArith(expr, &session, &raised246g, &isStrResult246g);
      // BUG-246, global twin — same conversion, same two retained fallbacks.
      if (!arith && classifyFailedEqExpr(expr, raised246g, errorOut, &session)) return true;
      // BUG-660 fix round: same cap-at-store-site rule as local's branch above.
      session.globals.set(name, arith ? (isStrResult246g
                                              ? *arith
                                              : capNumericTextToStataLocalDigits(*arith))
                                       : stripOuterDquotes(expr));
      return true;
    }

    // EM3b: the compound-quoted RHS, as `local`'s (`global g `"g "q" x"'` stores
    // g "q" x, w2af_macro_text_file.log:887).
    if (rhs.size() >= 4 && rhs.rfind("`\"", 0) == 0 && compoundSpanEnd(rhs, 0) == rhs.size()) {
      std::string inner = rhs.substr(2, rhs.size() - 4);
      if (hasMacroRefs(inner)) {
        ExpandResult exp = expandDefinitionText(inner, session);
        if (takeCodedExpandError(exp, errorOut)) return true;
        if (!exp.isError) inner = exp.line;
      }
      session.globals.set(name, inner);
      return true;
    }
    if (hasMacroRefs(rhs)) {
      ExpandResult exp = expandDefinitionText(rhs, session);
      if (takeCodedExpandError(exp, errorOut)) return true;  // as `local` above
      if (!exp.isError) rhs = strip(exp.line);              // X-1, as `local` above
    }
    rhs = stripOuterDquotes(rhs);
    session.globals.set(name, rhs);
    return true;
  }

  return false;
}

// ---------------------------------------------------------------------------
// capture_r_from_block

void captureRFromBlock(const nlohmann::ordered_json& block, EvalSession& session) {
  const std::string blockType = block.value("type", "");

  // --- Branch 1: summarize -> r() from the LAST variable's stats ---
  if (blockType == "summarize") {
    if (!block.contains("stats") || !block.at("stats").is_object() || block.at("stats").empty()) {
      return;
    }
    const auto& stats = block.at("stats");
    // Stata stores r() from the last variable in a multi-var summarize —
    // last key in dict order (ordered_json preserves document order).
    std::string lastVar;
    for (const auto& [k, v] : stats.items()) lastVar = k;
    const auto& varStats = stats.at(lastVar);

    static const std::vector<std::pair<std::string, std::string>> kSummarizeRMap = {
        {"N", "N"},           {"mean", "mean"},   {"sd", "sd"},   {"min", "min"},
        {"max", "max"},       {"sum", "sum"},     {"sum_w", "sum_w"}, {"Var", "Var"},
        {"p1", "p1"},         {"p5", "p5"},       {"p10", "p10"}, {"p25", "p25"},
        {"p50", "p50"},       {"p75", "p75"},     {"p90", "p90"}, {"p95", "p95"},
        {"p99", "p99"},       {"skewness", "skewness"}, {"kurtosis", "kurtosis"},
    };

    OrderedFloatDict newR;
    for (const auto& [statKey, rName] : kSummarizeRMap) {
      if (varStats.contains(statKey)) {
        auto f = jsonToFloat(varStats.at(statKey));
        if (f) newR.set(rName, *f);  // skip non-numeric (TypeError/ValueError)
      }
    }
    // Var = sd^2 when sd present but Var not already in stats.
    if (newR.contains("sd") && !newR.contains("Var")) {
      double sd = *newR.find("sd");
      newR.set("Var", sd * sd);
    }
    // No r(median): Stata's summarize posts r(p50) only, and a read of r(median)
    // after `summarize, detail` is `.` (FU-268, BUG-1013; w2o_rnames N16,
    // w2as_summary_stats_family sx_sum_d_u). The p50 alias this branch used to
    // post answered 5.25 there.
    session.rValues = newR;  // replace entire r() store (Stata semantics)
    return;
  }

  // --- Branch 3: test -> r-class Wald result (W:P5) ---
  // `test` is r-class in Stata (r(F), r(p), r(df), r(df_r), r(chi2), r(drop),
  // r(dropped)) but this codebase only ever wired summarize and lincom/nlcom,
  // so `test 1.treated` followed by `display r(p)` errored with "r(p) is not
  // set". The engine's test block already carries every value under its own
  // key names (F/p_F/df1/df2, plus chi2/p_chi2 on the chi-square form) — this
  // maps them onto Stata's r() spellings. Keep the lincom branch below
  // untouched: the two estimators publish different shapes.
  if (blockType == "estimation" && block.value("estimator", "") == "test") {
    // BUG-205(3), CORRECTED at the p5-wave regression (2026-08-08): the first
    // version mapped `p_chi2 -> p` unconditionally, on the stated premise that
    // "only one of the two source keys is ever present on a given block".
    // MEASURED FALSE: `test 1.treated` after `regress ..., vce(cluster id)`
    // emits BOTH (p_F=6.277e-26, p_chi2=9.705e-40), and the chi2 mapping ran
    // last, so r(p) silently became the chi2 tail on an F-form test — caught
    // by the p5 answerbook (want = Stata's F-based 6.277e-26). Stata's rule:
    // the DISPLAYED form owns r() — F-form sets {F, df, df_r, p}; the ML/chi2
    // form (no F on the block) sets {chi2, df, p}. The chi2 keys are now a
    // FALLBACK, applied only when the block carries no F.
    OrderedFloatDict newR;
    // The least-squares form carries F and p_F (both null when R V R' is exactly
    // zero: an exact `regress` fit, where Stata prints F(1, 48) = . and posts
    // r(F) = r(p) = missing at rc 0). A null there is Stata's missing value, so it
    // is posted as `.`; the chi2 form (no p_F key) keeps its present-only map.
    const bool fZero = block.contains("p_F") && block.at("p_F").is_null() &&
                       block.contains("F") && block.at("F").is_null();
    const bool fForm = block.contains("p_F") && (!block.at("p_F").is_null() || fZero);
    static const std::vector<std::pair<std::string, std::string>> kFMap = {
        {"F", "F"}, {"p_F", "p"}, {"df1", "df"}, {"df2", "df_r"}};
    static const std::vector<std::pair<std::string, std::string>> kChi2Map = {
        {"chi2", "chi2"}, {"p_chi2", "p"}, {"df1", "df"}};
    // BUG-1276: with every constraint dropped (df1 0) the chi2 form's statistic and
    // p are missing too, and Stata still posts them (`test x1` after `logit y x1 xs,
    // asis`: r(chi2) = r(p) = ., r(df) 0, r(dropped_1) 1).
    const bool allDropped = block.contains("dropped_rows") && block.contains("df1") &&
                            block.at("df1").is_number() && block.at("df1").get<double>() == 0;
    for (const auto& [src, dst] : (fForm ? kFMap : kChi2Map)) {
      if (block.contains(src) && !block.at(src).is_null()) {
        auto f = jsonToFloat(block.at(src));
        if (f) newR.set(dst, *f);
      } else if ((fZero && (src == "F" || src == "p_F")) ||
                 (allDropped && (src == "chi2" || src == "p_chi2"))) {
        newR.set(dst, std::nan(""));
      }
    }
    // r(dropped_#): the number of each constraint the engine dropped, in order.
    if (block.contains("dropped_rows") && block.at("dropped_rows").is_array()) {
      int k = 0;
      for (const auto& row : block.at("dropped_rows"))
        if (row.is_number()) newR.set("dropped_" + std::to_string(++k), row.get<double>());
    }
    // A malformed/empty test block must leave r() alone rather than clearing
    // it — F is the one key every shape of this block carries.
    if (!newR.contains("F") && !newR.contains("chi2")) return;
    session.rValues = newR;
    return;
  }

  // --- Branch 2: lincom / nlcom -> r-class post-estimation (WS-E) ---
  if (blockType == "estimation") {
    const std::string estimator = block.value("estimator", "");
    if (estimator != "lincom" && estimator != "nlcom") return;
    if (!block.contains("coeftable") || !block.at("coeftable").is_array() ||
        block.at("coeftable").empty()) {
      return;
    }
    const auto& row = block.at("coeftable").at(0);
    static const std::vector<std::pair<std::string, std::string>> kMap = {
        {"b", "estimate"}, {"se", "se"}, {"t", "t"}, {"p", "p"}, {"ci_lo", "lb"}, {"ci_hi", "ub"},
    };
    OrderedFloatDict newR;
    // A zero-variance combination (an exact `regress` fit): the engine posts se 0
    // with t, p and the interval null, and Stata's lincom posts r(se) 0 with r(t),
    // r(p), r(lb), r(ub) missing (StataBE `lincom x` after `regress yx x`).
    const bool zeroSe = row.contains("se") && row.at("se").is_number() &&
                        row.at("se").get<double>() == 0.0;
    for (const auto& [src, dst] : kMap) {
      if (row.contains(src) && !row.at(src).is_null()) {
        auto f = jsonToFloat(row.at(src));
        if (f) newR.set(dst, *f);
      } else if (zeroSe && row.contains(src) && src != "b" && src != "se") {
        newR.set(dst, std::nan(""));
      }
    }
    if (block.contains("df") && !block.at("df").is_null()) {
      auto f = jsonToFloat(block.at("df"));
      if (f) newR.set("df", *f);
    }
    session.rValues = newR;
    return;
  }

  // All other block types: do not touch r().
}

}  // namespace piet::eval
