// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// display_hardening — C++ port of piet/backend/piet/display_hardening.py
// (the M3a "display/di" hardening module; frozen Python oracle, read-only
// spec — do NOT edit the .py).
//
// This is a single-entry-point module: handleDisplay() is the only public
// symbol. All of the Python file's `_`-prefixed helpers (_resolve_token,
// _resolve_e_scalar, _resolve_b_ref, _resolve_N) become file-local static
// functions in display_hardening.cpp (_fmt_numeric's `%.9g` substitution is
// gone since BUG-554: the resolvers substitute at full precision, and only
// display's verbatim fallback still prints `%.9g`, verbatimCoefText) —
// deliberately NOT shared with piet/eval/macros.cpp's own (anonymous-namespace,
// so unreachable from here) resolveBRef/resolveEScalar/fmtNumeric, which serve
// a different call site (_eval_arith's est-scalar substitution reading
// EvalSession::lastModelResult) and read from a DIFFERENT data path than
// this module (SessionState::lastModel()->result). Duplicating here avoids
// an accidental coupling between the two call sites.
//
// Four correctness targets (Stata parity gate), ported verbatim from the
// Python docstring:
//   T1: di "N = " _N          ->  "N = 74"   (_N from session data)
//   T2: di %9.2f 3.14159      ->  "3.14"     (format round-trip)
//   T3: scalar a=5; di "a=" a ->  "a=5"      (scalar sub in UNQUOTED tokens only)
//   T4: after regress, di _b[mpg] -> coef    (_b[]/_se[] refs from last_model)
#ifndef PIET_NATIVE_DISPLAY_HARDENING_H
#define PIET_NATIVE_DISPLAY_HARDENING_H

#include <optional>
#include <string>

#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

namespace piet {

using ojson = nlohmann::ordered_json;

// display_hardening.handle_display(raw_args, session) -> {"type":"display",
// "text":<str>}. `session` is non-const: token resolution may lazily
// construct the Datastore (SessionState::datastore() is non-const).
ojson handleDisplay(const std::string& rawArgs, SessionState& session);

// Stata's DEFAULT numeric display format, `%9.0g` — the nine-character rule
// measured against the answerbook's own Stata 19.5 log (see the definition in
// display_hardening.cpp for the worked examples). Exported at the P6 wave
// because the resampling tables must render their numbers the way `display`
// already does; a second copy would be a second thing to keep in step, which is
// how the two-layer divergences in this codebase have started before.
// NOTE this is deliberately NOT the `%.9g` text of display's verbatim fallback
// (display_hardening.cpp verbatimCoefText) — do not conflate them.
std::string fmtStataDefaultG(double v);

// A number (`valueText`: its digits, or `.`/`.a`-`.z`) under a numeric %fmt,
// placed in its field exactly as `display %fmt exp` places it. `file write`
// renders through this: StataBE 19.5 writes the same bytes as it displays
// (`%08.2f` 00003.14, `%6.2f 1234567.89` ` 1.2e+06`, `%21x .` the hex view),
// so the two must not be two formatters (BUG-1128).
std::string formatNumberAsDisplayed(const std::string& fmt, const std::string& valueText);

// AER-SCOPE1 bundle E (TABLES): resolve ONE scalar expression to a number.
// `outreg2 ..., addstat(F, e(F))` and `estadd scalar k = e(N)/2` need exactly
// what `display` already computes, so this is the same chain display runs --
// whole-token resolution (`e(F)`, `_b[x]`, `_N`, a named scalar, `c(name)`),
// then e()/_b[] substitution find-anywhere, then the arithmetic evaluator --
// rather than a second, quietly divergent reader of e(). Returns nullopt when
// the expression does not resolve to a number (including when it resolves to a
// STRING), so a caller can say so instead of printing a zero.
// IO-TEXT's `putexcel (expr)` is the other consumer; that is why TABLES merges
// before IO-TEXT.
std::optional<double> evalScalarExpr(const std::string& expr, SessionState& session);

// BUG-718: `VAR[idx]` (1-based) as `display VAR[idx]` reads it -- a string
// variable's text verbatim (isString), a numeric one's full-precision value or
// its `.`/`.a`-`.z` token. nullopt when `name` is not a dataset variable.
// inRange is false for an index outside 1.._N: the text is then display's `.`
// and isString says whether the COLUMN is a string one. The macro layer's EvalSession::varObs hook
// (SessionState::macroVarObs) reads through this, so `` `=VAR[i]' `` and
// `display VAR[i]` are one reader.
struct VarObservation {
  std::string text;
  bool isString = false;
  bool inRange = true;
};
std::optional<VarObservation> readVarObservation(const std::string& name, long long idx,
                                                 SessionState& session);

// name-binding-display-literals: THE binding of a bare name to a dataset
// variable -- its exact name, else the one variable whose name it begins
// ([U] 11.2 abbreviation; Stata's reserved names never abbreviate). nullopt when
// it names no variable; `*ambiguous` (when non-null) is set when it begins two
// or more. display (readVarObs), the macro layer (EvalSession::varObs, through
// readVarObservation) and the row plane's rewrite read names through this one
// rule, and a variable it finds wins over a stored scalar of the same name
// (w2av_name_binding bd_*_abbr, bd_*_abbrsc, bd_*_ambig).
std::optional<std::string> resolveVarName(const std::string& name, SessionState& session,
                                          bool* ambiguous = nullptr);

// The first bare name in an expression that binds to no variable, scalar, matrix
// or built-in, as `r(111): NAME not found` (or `... ambiguous abbreviation`);
// display's own check (unboundNameRefusal), for a command that reads an `if`
// before anything else (predict with no estimates, BUG-1248). nullopt when every
// name binds.
std::optional<std::string> exprUnboundNameRefusal(const std::string& text, SessionState& session);

}  // namespace piet

#endif  // PIET_NATIVE_DISPLAY_HARDENING_H
