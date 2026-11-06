// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// strmutate.h — row-wise PIET evaluator for STRING-valued / STRING-gated
// `gen`/`replace`/`count`/`keep if`/`drop if`. C++ port of
// piet/backend/piet/strmutate.py (the frozen 1.16.0 oracle).
//
// cobra is an all-f64 engine: it cannot evaluate string functions (lower,
// substr, strtrim, regexr, ...) nor hold/compare string columns (strings live
// in hbin as *factors* = int32 codes + a levels list). So any `gen`/`replace`
// that (a) declares a string storage type, (b) produces a string (rhs uses a
// string function / literal), (c) is gated on a string condition, or (d)
// targets/reads an existing factor column MUST be evaluated row-by-row in
// this layer instead of shelling out to `cobra eval --gen`. The result is
// written back as a factor (string) column, which cobra then consumes.
//
// The pure-numeric, no-string path is untouched (still cobra) —
// `needsStringEval` only returns true when a string is genuinely involved, so
// the all-f64 corpus stays byte-identical.
//
// Reuses the M1 evaluator infrastructure (pyeval/exprfns/pyvalue) for the
// per-row expression evaluation — the same machinery loops.cpp's evalBoolExpr
// uses — rather than a fresh eval engine.
#ifndef PIET_NATIVE_STRMUTATE_H
#define PIET_NATIVE_STRMUTATE_H

#include <optional>
#include <string>
#include <vector>

#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

namespace piet::strmutate {

// needs_string_eval(session, varname, rhs, if_expr, str_typed, is_replace):
// decide whether a gen/replace must be evaluated row-wise (string path).
bool needsStringEval(SessionState& session, const std::string& varname, const std::string& rhs,
                     const std::optional<std::string>& ifExpr, bool strTyped, bool isReplace);

// needs_string_count(session, if_expr): decide whether a `count if` (or
// keep/drop if) condition must be evaluated row-wise (references a factor
// column / string fn / string literal).
bool needsStringCount(SessionState& session, const std::optional<std::string>& ifExpr);

// run_string_gen / run_string_replace: row-wise gen/replace. Returns the
// data_mutation block (or an error block on a bad expression, and r(109) when
// the expression's type is not the variable's: BUG-1122).
// `storeType`: the numeric storage type the new variable takes when the result is
// numeric (storage::resolveCreateType: the declared type, else c(type)); "" keeps
// the double the evaluator produced. `numTyped`: the generate declared a numeric
// type, so a string expression is r(109).
// `inRange`: the command's own `in a/b`, which the caller has already folded into
// `ifExpr`; it is read again only where the `if` holds a form the engine computes
// (a running sum() starts at the range's first row: see materializeTsIf).
nlohmann::ordered_json runStringGen(SessionState& session, const std::string& expr,
                                    const std::optional<std::string>& ifExpr, bool strTyped,
                                    const std::string& storeType = std::string(),
                                    bool numTyped = false,
                                    const std::optional<std::string>& inRange = std::nullopt);
// `noPromote` (replace's `nopromote`): an integer target keeps its own type and
// the stored values truncate (w2e_float_default replace_byte_nopromote :827); a
// str# target keeps its width and longer values truncate (BUG-1143), and the
// "(N values truncated because of storage type)" line is appended to `notes`.
nlohmann::ordered_json runStringReplace(SessionState& session, const std::string& expr,
                                        const std::optional<std::string>& ifExpr, bool strTyped,
                                        bool noPromote = false,
                                        std::vector<std::string>* notes = nullptr,
                                        const std::optional<std::string>& inRange = std::nullopt);

// count_rows_if: number of rows where a (possibly string-referencing)
// condition is true. Sets `ok=false` on an un-evaluatable condition (message
// carried in `errMsg`). `inRange` (BS-090, BETA Phase G): an optional `in
// a/b` row-range, ANDed into the SAME per-row mask this already builds
// (Doctrine ①: this path is only reachable when `if` references a
// string/factor column, and dispatcher.cpp's caller must never silently
// drop a co-occurring `in` just because the `if` half took the row-wise
// detour — a malformed or out-of-range `in` sets `ok=false` and
// `inRangeError`, with `errMsg` holding Stata's refusal sentence (empty for a
// malformed string) for the caller to hand to piet::handlers::inRangeRejectError).
struct CountResult {
  bool ok = true;
  bool inRangeError = false;  // true iff a failing `ok=false` is the `in` bound, not `if`
  long long n = 0;
  std::string errMsg;
  // "r(NNN)" when the row evaluator named Stata's code for the failure (r(109):
  // a string used as a truth value, `count if s`), and `errMsg` is then its own
  // words ("type mismatch") when it gave any; "" when it could not read the
  // condition at all (a subscript, `x[_n-1]`).
  std::string errReason;
};
CountResult countRowsIf(SessionState& session, const std::string& ifExpr,
                        const std::optional<std::string>& inRange = std::nullopt);

// filter_rows_if: row-wise `keep if` (keep=true) / `drop if` (keep=false) for
// conditions cobra can't evaluate. Returns the data_mutation block (or
// error). `inRange` (BS-090) ANDs the same row-range restriction into the
// mask before the keep/drop split is applied.
nlohmann::ordered_json filterRowsIf(SessionState& session, const std::string& ifExpr, bool keep,
                                    const std::optional<std::string>& inRange = std::nullopt);

// The ONE route for an `if` on an engine-routed command whose condition the
// engine's f64 evaluator cannot read: a string function (strpos, regexm, substr,
// ...), a compare against the empty string `""` (the engine reads `s != ""` on
// a string column as true for the empty row: w2aj_qualifiers qf_if_str_ne_empty
// r(N) 11, PIET 12), or a string column used any other way than `s ==/!=
// "text"` (`missing(s)`, `inlist(s, ...)`, `s > "a"`). count/keep/drop/generate/
// replace keep their own row-wise detours above; every other command takes this
// one, and so do the by: prefix's sub-commands.
//
// needsConditionMask: does `cond` take the route. A plain `s == "text"` does not
// (the engine's factor compare serves it).
bool needsConditionMask(SessionState& session, const std::string& cond);
// installConditionMask: evaluates `cond` with the evaluator countRowsIf uses (no
// second evaluator), installs the row mask on the working frame as the hidden f64
// column `column` (piet/datastore.h kQualifierColumnPrefix) and returns the
// condition every engine `--keepif` and applyIfAndInFilter then receive,
// `(column!=0)`. c(changed) is left as it was. On failure `ok` is false and
// `error` is the block to show (the evaluator's own message and rc). With
// `groupKeys` (a by-prefixed generate/replace), `_n` and `_N` count within each
// group of those keys over the rows as they stand (handlers::groupObsNumberFrame).
struct ConditionMaskInstall {
  bool ok = true;
  std::string column;
  std::string condition;
  nlohmann::ordered_json error;
  // installRowExpression only: the row evaluator could not read the expression
  // (it has no subscript, `x[_n-1]`); `error` is its message, and the caller
  // keeps the engine's own answer instead.
  bool declined = false;
};
ConditionMaskInstall installConditionMask(SessionState& session, const std::string& cond,
                                          const std::vector<std::string>& groupKeys = {});
// writeConditionMaskColumn: the same evaluation over the frame at `dataPath` (a
// by: driver's sorted scratch copy, not the head), whose mask is written as the
// f64 0/1 column `column` onto a copy at `outPath`; the session's frame is left
// alone. `_n`/`_N` are that frame's own row numbers. Same `ok`/`error` contract.
ConditionMaskInstall writeConditionMaskColumn(SessionState& session, const std::string& dataPath,
                                              const std::string& cond, const std::string& column,
                                              const std::string& outPath);
// installWeightExpression: a weight that is an expression, not a variable
// (`[pw=1/wf]`, w2aj_qualifiers qf_rw_pw_expr, qf_sw_aw_expr), is generated once
// by the engine into the hidden column `column`, which the command then receives
// as its weight variable. Same `ok`/`error` contract as installConditionMask.
ConditionMaskInstall installWeightExpression(SessionState& session, const std::string& expr);
// installRowExpression: an egen argument evaluated once, by the row evaluator
// installConditionMask uses, over the whole frame into the hidden f64 column
// `column`, which the egen function then receives as its argument -- the egen
// ados generate the expression into a temporary first. It serves an argument
// the engine's f64 evaluator cannot read (a string function, bofd(): BUG-464),
// and one naming _n or _N under a by-list or if/in, which the engine evaluated
// on the if/in-filtered copy and over the whole frame under by(). `byVars` (the
// by() option's text, for the functions whose ados generate the argument by the
// groups; the caller passes "" for the others) makes _n and _N count within
// each by-group. A string value is r(109), except for `countValues` (count()),
// where it counts when it is not "". Same `ok`/`error` contract as
// installConditionMask, plus `declined`.
ConditionMaskInstall installRowExpression(SessionState& session, const std::string& expr,
                                          const std::string& byVars, bool countValues);
// namesObsPosition: `expr` names _n or _N outside a "..." literal.
bool namesObsPosition(const std::string& expr);
// removeConditionMask: drops a qualifier column from the working frame when it is
// still there (a command that rebuilt the data, collapse, has none left).
// c(changed) is left as the command left it.
void removeConditionMask(SessionState& session, const std::string& column);

// --- Selftest instrumentation (FLOOR campaign: slot-compiled evalRows) ------
//
// evalRowsMapWriteCount() counts per-row namespace-map WRITES on the row-eval
// path (evalRows). The slot-compiled fast path performs ZERO of them; the
// legacy per-row rebind performs (referenced-columns + row-specials) writes
// per row. Process-wide, env-independent — same idiom as
// Datastore::constructCount() / SessionState::cardFoldCount().
//
// The mechanism gate reads 0 here after a slot-compiled gen/count on a
// multi-row fixture. A nonzero count is the ONLY signal that slot-compilation
// silently stopped applying (e.g. compilePreboundExpr began rejecting an
// expression that used to compile): every value pin stays green either way,
// because the two paths compute identical answers.
int64_t evalRowsMapWriteCount();
void resetEvalRowsMapWriteCount();

// Force evalRows onto the legacy per-row namespace-bind path, bypassing
// slot-compilation. Selftest hook ONLY (default false; no production caller):
// lets a gate obtain the legacy answer for the SAME expression the slot path
// computed and assert the fallback still evaluates correctly with map
// writes > 0.
void setForceLegacyEvalRowsForTest(bool on);

}  // namespace piet::strmutate

#endif  // PIET_NATIVE_STRMUTATE_H
