// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// cclass.h — BETA Phase A4 c()-class system-value registry (BS-091/BS-101).
//
// Single source for every `c(name)` read, whichever context it appears in:
// `display`'s bespoke tokenizer (display_hardening.cpp), a `gen`/`replace`
// RHS forwarded row-wise to cobra (dispatcher.cpp), an `if`/`while` condition
// (loops.cpp::evalBoolExpr), or a `scalar NAME = expr` / `local NAME = expr`
// capture (dispatcher.cpp's tryCaptureDefinition call site). Before this
// module, c(current_date)/c(current_time) were the ONLY two keys
// implemented, and only inside display's own resolver — every other context
// (`gen k = c(N)`) had no c() support at all (BS-091's exact finding).
//
// ~15-key spine this build implements: N, k, rc, os, machine_type, username,
// pwd, current_date, current_time, version, level, seed, changed, filename,
// obs (an alias of N — Stata's own canonical observation-count key is c(N);
// c(obs) is accepted as a harmless synonym rather than a distinct concept),
// sysdir_sample (PIET-specific, in Stata's sysdir_* naming family: the
// directory holding the bundled sample datasets, bundle-resolved with the
// same order as `sysuse` — lets a do-file reach the shipped fatalities.csv
// from ANY working directory, no trailing separator).
// BUG-1330 widened the spine with the keys a do-file reads that PIET can answer
// truthfully: the numeric constants (pi, the storage-type limits, stata_version),
// version/userversion (kVersionLocal), obs_t, the word lists
// (alpha ... Weekdays), dirsep, tmpdir, graphics, sysdir_*, and the settings PIET
// ignores at their StataBE default until a `set` of them runs (kSetMarkerPrefix).
// BUG-1463: and the edition PIET answers as, Stata/BE: processors,
// processors_lic, processors_max (1), processors_mach (.), MP and SE (0),
// edition and edition_real ("BE").
// Any OTHER c(...) name resolves to nullopt so the caller leaves the token
// untouched and it is refused downstream (the macro layer's classifier and
// delimited expansion, eval::isStataCClassName: `c(name) undefined` for a name
// Stata does not know either, a named refusal for one it does) instead of
// silently vanishing (matches the BUG-090/BUG-091 "never swallow the whole
// c-class into silence" philosophy).
//
// Known simplifications (documented, not hidden):
//   - c(seed): reflects the last `set seed #` value verbatim; no cobra
//     command in this build's dispatch surface yet consumes it as an actual
//     RNG seed (wildboot/mediate/bootstrap are later D-phase waves), so this
//     is state-tracking + readback only.
//   - c(machine_type): a coarse, compile-time-detected descriptive string,
//     not byte-identical to real Stata's own (machine-specific) text.
#ifndef PIET_NATIVE_CCLASS_H
#define PIET_NATIVE_CCLASS_H

#include <optional>
#include <string>

#include "piet/session_state.h"

namespace piet {

// One resolved c(name) value. `isString` distinguishes a bare numeric token
// (already formatted ready to splice into an arithmetic expression, e.g.
// "960") from text that must be quoted to be a valid string-literal token in
// that same grammar (e.g. text "MacOSX" -> substituteCRefs splices
// "\"MacOSX\""). `text` is always the RAW (unquoted) display-ready value —
// exactly what `display`'s resolver wants directly.
struct CValue {
  bool isString = false;
  std::string text;
};

// BUG-1330: the `version #` in force, held as a local no do-file can spell so
// it is scoped as Stata scopes it: a do-file or program starts with its
// caller's version (doexec's PrivateLocalScope and runProgram copy it into the
// fresh scope) and the caller's returns when it concludes (the scope pops).
// Absent = this build's own version, 19.5.
inline const char* const kVersionLocal = " __piet_version";
// Its twin for c(userversion), scoped the same way: set by `version #` outside a
// program or loop body and by `version #, user` anywhere (dispatcher.cpp).
inline const char* const kUserVersionLocal = " __piet_userversion";
// Set in a program body's own scope only (runProgram); a do/run file's fresh
// scope lacks it, so a `version #` there sets c(userversion) again.
inline const char* const kProgramBodyLocal = " __piet_progbody";

// BUG-1330: a global no do-file can spell, `<prefix>NAME`, set by the generic
// `set NAME ...` arm (dispatcher.cpp) for a setting PIET ignores; c(NAME) is
// refused from then on instead of answering the default.
inline const char* const kSetMarkerPrefix = " __piet_set_";

// Resolves ONE c(name) (case-sensitive, matching Stata's own mostly-lowercase
// c()-class naming). `session` is non-const: c(N)/c(k) may lazily construct
// the Datastore (SessionState::datastore() is non-const).
std::optional<CValue> resolveCValue(const std::string& name, SessionState& session);

// Substitutes every `c(name)` occurrence in `expr` outside double-quoted text
// with a literal token ready for the arithmetic-expression grammar (bare
// number for a numeric key, parenthesised when negative, a double-quoted
// string for a text key) — the ONE pre-substitution
// pass every expression-context caller applies before forwarding to cobra's
// row-wise evaluator or to the native Python-subset evaluator. An unresolved
// name is left as literal `c(name)` text, unchanged (never silently
// dropped) — mirrors piet::eval::resolveEstScalarRefs's own established
// per-call-site pre-substitution pattern (dispatcher.cpp's gen/replace arms
// and its row-expression pass, loops.cpp's evalBoolExpr).
std::string substituteCRefs(const std::string& expr, SessionState& session);

// BUG-1330: the r(133) message for the first `c(name)` outside quoted text that
// substituteCRefs left in `expr` (eval::cClassRefusalMessage), or nullopt. For
// the call sites that hand the expression to cobra, whose own answer was
// `function c() is not supported` at rc 1.
// `rc` gets the code it carries (eval::cClassRefusalRc).
std::optional<std::string> unservedCRefError(const std::string& expr, std::string* rc = nullptr);

}  // namespace piet

#endif  // PIET_NATIVE_CCLASS_H
