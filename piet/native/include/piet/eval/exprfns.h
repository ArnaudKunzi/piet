// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Stata expression function library — C++ port of
// piet/backend/piet/router/exprfns.py (the frozen 1.16.0 oracle).
//
// Python's functions are dynamically typed and DEFENSIVE: wrong argument
// "types" return NaN/"" instead of raising (Stata's silent-missing
// convention) — but wrong ARITY raises TypeError before the body runs,
// which the eval() callers catch as full-expression failure. The port
// mirrors both behaviors: body failures are absorbed per-function, arity
// failures throw PyExc.
//
// The functions plug into pyeval's PyNamespace (see addStataFns) so
// macros._eval_arith-style evaluation gets the same names Python injects
// via STATA_FNS / STATA_FNS_SESSION.
//
// Known parity caveats (documented in the M1 report; corpus scoped
// accordingly):
//   - lower/upper/proper/trim family: Python str methods are full-Unicode;
//     the port is ASCII-correct (identity on non-ASCII, which also matches
//     Python for caseless scripts like CJK).
//   - regexm/regexr: Python `re` vs std::regex ECMAScript dialect; and
//     re.sub uses \1 backreferences where ECMAScript uses $1. Corpus uses
//     dialect-neutral patterns and plain replacements.
#ifndef PIET_NATIVE_EVAL_EXPRFNS_H
#define PIET_NATIVE_EVAL_EXPRFNS_H

#include <cctype>
#include <cmath>
#include <cstdio>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "core/ident.h"  // the shared Unicode name rule (scanner byte tests)
#include "piet/eval/pyeval.h"
#include "piet/eval/pyvalue.h"

namespace piet::eval {

// Unicode names (lane D): the std::regex spellings of core/ident.h's permissive
// scanner tests, for the patterns that bind a NAME. `[A-Za-z_]\w*` treats an
// Arabic byte as a word break, so `دخل_2020` scanned as the ASCII tail `_2020`
// and `xملاحظات` as `x`. A byte >= 0x80 is part of a name, as isIdentByte() says.
// Each is ONE negated bracket class -- every byte except the ASCII ones that are
// not name bytes -- so it costs what `[A-Za-z_]\w*` cost (an alternation would
// run std::regex's backtracking machinery per character), and it lists only
// ranges inside 0x00-0x7F, so it means the same thing whether `char` is signed
// or not (a `\x80-\xFF` range would not). On pure-ASCII text each pattern
// matches exactly what the ASCII class it replaces matched.
//   kRxNameChar  = isIdentByte:      not 00-2F, 3A-40 ([0-9] kept), 5B-5E, 60, 7B-7F
//   kRxName      = isIdentStartByte then isIdentByte* (digits excluded first)
inline constexpr char kRxNameChar[] = "[^\\x00-\\x2F\\x3A-\\x40\\x5B-\\x5E\\x60\\x7B-\\x7F]";
inline constexpr char kRxName[] =
    "[^\\x00-\\x40\\x5B-\\x5E\\x60\\x7B-\\x7F][^\\x00-\\x2F\\x3A-\\x40\\x5B-\\x5E\\x60\\x7B-\\x7F]*";

// Stata's missing values, in StataBE's own bits (BUG-905 round 3).
//
//   `.`              0x1p+1023                 %21x +1.0000000000000X+3ff
//                    (w2x_fn_string_math.log:931, w2_openbugs.log:45,
//                     pins_oracle_r.log:195)
//   `.a`..`.z`       0x1p+1023 + k * 0x1p+1011, k = 1..26
//                    `.a` +1.0010000000000X+3ff (w2i_varsub.log:161),
//                    `.z` +1.01a0000000000X+3ff
//
// A double is MISSING iff it is NaN or |d| >= 2^1023 (stataExtendedMissingKind
// below). Everything finite under 2^1023 is a real number, and that includes
// +/-c(maxdouble) = +/-0x1.fffffffffffffp+1022 = 8.988465674311579e+307:
// StataBE prints `display (-8.988465674311579e+307)` as -8.99e+307
// (internal capture missing_range_oracle.log:67/:70/:73). Until this
// round the constant WAS c(maxdouble), one ulp under 2^1023, so that value
// was read as `.`.
//
// The kinds are ordered `. < .a < ... < .z` by the arithmetic, and every
// `>= kStataMissingSentinel` missing test fires for all 27 of them. Arithmetic
// does not carry a kind: pyeval.cpp's operators return the plain sentinel when
// an operand or a finite result is in the band (Stata: `(.a + 1) == .` is 1,
// src/data/missing.h). Only direct reads (a `.a` literal or cell,
// `string(.a[, fmt])`) and comparisons see the kind.
constexpr double kStataMissingSentinel = 0x1p+1023;

// StataBE's kind step: `.a` is +1.0010000000000X+3ff.
constexpr double kStataMissingKindStep = 0x1p+1011;

inline double stataMissingWithKind(int kind) {  // kind 0 = `.`, 1..26 = .a...z
  return kStataMissingSentinel + kStataMissingKindStep * kind;
}

// nullopt = not a missing (a real number; NaN too, which each caller handles on
// its own path); 1..26 = exactly one of the 26 kind doubles; 0 = any other
// missing (`.`, an overflow, the negative band, inf), which is plain `.`.
inline std::optional<int> stataExtendedMissingKind(double v) {
  if (!(std::fabs(v) >= kStataMissingSentinel)) return std::nullopt;
  for (int k = 1; k <= 26; ++k)
    if (v == stataMissingWithKind(k)) return k;
  return 0;
}

// The text of a missing value of this layer: `.` for NaN and for every band
// value that is not one of the 26 kinds, `.a`..`.z` for the kinds; nullopt for a
// real number. For renderers that print a cell rather than evaluate it -- a
// matrix holding `.a` printed the 2^1023-sized integer (BUG-1399).
inline std::optional<std::string> stataMissingToken(double v) {
  if (std::isnan(v)) return std::string(".");
  const std::optional<int> kind = stataExtendedMissingKind(v);
  if (!kind) return std::nullopt;
  if (*kind == 0) return std::string(".");
  return std::string(".") + static_cast<char>('a' + *kind - 1);
}

// Text rewrite of Stata's missing literals into doubles the Python-style
// evaluator can lex: a bare `.` (not part of a number or an ident.member) and an
// extended `.a`..`.z` (one lowercase letter, non-word neighbours on both sides;
// `.ab`, `x.a` and `1.e5` pass through untouched). Each is spelled `%.17g`, which
// strtod reads back to the exact double. `substr(s, 3, .)` (missing length = to
// the end) then reaches fnSubstr as a huge length. Shared by macros.cpp's
// evalArith, loops.cpp's `if {}` and strmutate.cpp's string row evaluator so the
// three sites apply one word-boundary rule.
inline std::string stataMissingLiteral(int kind) {
  char buf[40];
  std::snprintf(buf, sizeof(buf), "%.17g", stataMissingWithKind(kind));
  return buf;
}

inline std::string replaceStataMissingLiterals(const std::string& s) {
  auto isWordOrDot = [](char c) {  // a byte >= 0x80 is a name byte (core/ident.h)
    return cecon::ident::isIdentByte(c) || c == '.';
  };
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '.') {
      bool prev = (i > 0 && isWordOrDot(s[i - 1]));
      bool next = (i + 1 < s.size() && isWordOrDot(s[i + 1]));
      if (!prev && !next) { out += stataMissingLiteral(0); continue; }
      if (!prev && i + 1 < s.size() && s[i + 1] >= 'a' && s[i + 1] <= 'z' &&
          !(i + 2 < s.size() && isWordOrDot(s[i + 2]))) {
        out += stataMissingLiteral(s[i + 1] - 'a' + 1);
        ++i;  // consume the kind letter
        continue;
      }
    }
    out += s[i];
  }
  return out;
}

// scalar(name) lookup: returns the stored string value or nullopt.
// TODO(M2): bind to the full SessionState scalar store once the C++
// session/datastore lands; M1 callers bind the EvalSession ordered map.
using ScalarLookupFn = std::function<std::optional<std::string>(const std::string&)>;

// AER scope 2 / B3(c). Does this expression contain a RUNNING sum -- Stata's
// sum(), the one expression function whose value depends on the rows before it?
//
// It matters at exactly one decision: `gen y = <rhs> if <c>` is normally sent to
// the engine as `y = cond((c),(rhs),.)`, which is equivalent for every other
// function but makes a running total accumulate over the rows the qualifier
// excluded. When this answers true the qualifier rides separately as --gen-mask.
//
// Quote-aware on purpose: a `sum(` inside a string literal is data, and reaching
// into literals is the defect class of BUG-443. Word-bounded on both sides, so
// `rowsum(` and `mysum(` do not match. A false positive would only take the
// (equally correct) mask path, so the bias is safe either way.
inline bool exprHasRunningSum(const std::string& e) {
  char quote = 0;
  for (size_t i = 0; i < e.size(); ++i) {
    const char c = e[i];
    if (quote) {
      if (c == quote) quote = 0;
      continue;
    }
    if (c == '"' || c == '\'') { quote = c; continue; }
    if (c != 's' || e.compare(i, 3, "sum") != 0) continue;
    if (i > 0) {
      const char p = e[i - 1];
      if (cecon::ident::isIdentByte(p) || p == '.') continue;  // `مsum(` is a name
    }
    size_t j = i + 3;
    while (j < e.size() && (e[j] == ' ' || e[j] == '\t')) ++j;
    if (j < e.size() && e[j] == '(') return true;
  }
  return false;
}

// Add the STATA_FNS entries (exprfns.py) to `ns`.
void addStataFns(PyNamespace& ns);

// The row plane's regexs() (strmutate.cpp baseNamespace): fnRegexs, which
// reads the RegexsRowScope snapshot while a scope is open. Not part of
// addStataFns() because only a row loop that opens the scope may carry it.
void addRowPlaneRegexs(PyNamespace& ns);

// Open for the duration of a row loop (strmutate.cpp evalRows). Call
// beginExpression() before each expression evaluated on a row: regexs() then
// answers from the regexm() state as it stood at that moment, not from a
// regexm() inside the same expression (w2c_functions.log regexs_whole :599,
// regexs_group_matched :609, regexs_group_unmatched :619 -- see fnRegexs).
class RegexsRowScope {
 public:
  RegexsRowScope();
  ~RegexsRowScope();
  RegexsRowScope(const RegexsRowScope&) = delete;
  RegexsRowScope& operator=(const RegexsRowScope&) = delete;
  void beginExpression();
};

// The row plane's sum() (strmutate.cpp baseNamespace): a running total per call
// site, kept by the RunningSumRowScope a row loop opens (BUG-1369). Outside a
// scope it answers like the macro layer's single-evaluation sum().
void addRowPlaneSum(PyNamespace& ns);

// Open for the duration of a row loop (strmutate.cpp evalRows). Call
// beginExpression(0) before the condition and beginExpression(1) before the
// right-hand side is evaluated on a row: the n-th sum() call of that expression
// adds the row's value to the n-th running total of that expression.
class RunningSumRowScope {
 public:
  RunningSumRowScope();
  ~RunningSumRowScope();
  RunningSumRowScope(const RunningSumRowScope&) = delete;
  RunningSumRowScope& operator=(const RunningSumRowScope&) = delete;
  void beginExpression(int which);
};

// Add the session-bound scalar() entry (exprfns.STATA_FNS_SESSION).
void addScalarFn(PyNamespace& ns, ScalarLookupFn lookup);

// Individual functions, exposed for the selftest smoke leg and the
// macro-engine helpers that call them directly.
PyValue fnInlist(const std::vector<PyValue>& args);
PyValue fnInrange(const std::vector<PyValue>& args);
PyValue fnCond(const std::vector<PyValue>& args);
PyValue fnMod(const std::vector<PyValue>& args);
PyValue fnMissing(const std::vector<PyValue>& args);
PyValue fnLower(const std::vector<PyValue>& args);
PyValue fnUpper(const std::vector<PyValue>& args);
PyValue fnStrtrim(const std::vector<PyValue>& args);
PyValue fnLtrim(const std::vector<PyValue>& args);
PyValue fnRtrim(const std::vector<PyValue>& args);
PyValue fnSubstr(const std::vector<PyValue>& args);
PyValue fnStrlen(const std::vector<PyValue>& args);
PyValue fnStrpos(const std::vector<PyValue>& args);
PyValue fnRegexm(const std::vector<PyValue>& args);
PyValue fnRegexr(const std::vector<PyValue>& args);
PyValue fnRegexs(const std::vector<PyValue>& args);
PyValue fnUstrlen(const std::vector<PyValue>& args);
PyValue fnUstrleft(const std::vector<PyValue>& args);
PyValue fnUstrtrim(const std::vector<PyValue>& args);
PyValue fnUstrregexm(const std::vector<PyValue>& args);
PyValue fnReal(const std::vector<PyValue>& args);
PyValue fnWord(const std::vector<PyValue>& args);
PyValue fnSubinstr(const std::vector<PyValue>& args);
PyValue fnProper(const std::vector<PyValue>& args);
PyValue fnString(const std::vector<PyValue>& args);
PyValue fnChar(const std::vector<PyValue>& args);
PyValue fnAbbrev(const std::vector<PyValue>& args);

// _g_format / _apply_stata_fmt (exprfns.py) — also used by string().
// gFormat throws PyExc on non-finite input (mirrors int(inf) OverflowError
// escaping Python's except (TypeError, ValueError) clause in _g_format).
std::string gFormat(double v);
std::string applyStataFmt(double v, const std::string& fmt);

}  // namespace piet::eval

#endif  // PIET_NATIVE_EVAL_EXPRFNS_H
