// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// User-defined program support — C++ port of piet/backend/piet/program.py
// (S6 + S7). Provides:
//   captureProgams      — consume `program define…end` blocks from a line
//                          list (Python: capture_programs).
//   buildArgs            — build the positional-arg locals map from a raw
//                          argument string (Python: _build_args).
//   runProgram           — invoke a user program by name.
//   handleProgramManage  — `program drop` / `program dir` native handler.
//   handleReturnStmt     — `return scalar` / `return local` step-0 handler.
//
// Design notes carried over from the oracle (see program.py's module
// docstring for the full rationale):
//
//   CAPTURE TIMING
//     Bodies are captured POST-preprocess (after `///` joins and comment
//     strips) but BEFORE brace aggregation. Body lines are continuation-
//     joined, comment-stripped plain Stata lines — exactly what
//     executeDoSource can re-ingest. The brace aggregator never sees
//     `program`/`end` keywords.
//
//   INVOCATION POINT
//     runProgram is intended to be called by dispatcher.cpp's dispatch()
//     AFTER the bysort check (step 1b) and BEFORE the data-guard (step 2).
//     At that point macro expansion (step 0b) has already run, so the
//     matched command token carries the ORIGINAL-case program name. Stata
//     program names are case-insensitive in practice; the registered name
//     is preserved exactly and matched case-insensitively (see findProgram).
//
//   SCOPE MODEL
//     pushScope(args) / popScope() swap session.eval.locals — consumers
//     always read session.eval.locals live, so macros set in a caller scope
//     are hidden during the call (Stata's local-scoping rule). Globals and
//     scalars span all scopes (session-global, Stata-faithful).
//
//   RETURN VALUES (S7)
//     `return scalar NAME = EXPR` and `return local NAME value` write into
//     session.returnStaging. On normal return from runProgram, if the
//     program was declared rclass, the staging area is flushed into
//     session.eval.rValues / session.eval.rStrings AFTER popScope so the
//     caller can access them via r(). returnStaging accumulates across
//     nested calls; each runProgram snapshots and resets it on entry,
//     restoring the caller's staging on exit.
//
//   RECURSION GUARD
//     session.progCallDepth counts call depth. Exceeding kMaxRecursion
//     returns a one-element engine_error block array (see runProgram) —
//     this C++ port does NOT throw a dedicated exception type the way the
//     Python oracle raises _ProgramRecursionError; the caller-visible
//     surface (an engine_error block) is identical, and folding the guard
//     into the normal return path avoids a bespoke exception type for a
//     single call site. See the "judgment calls" note in the port report.
#ifndef PIET_NATIVE_PROGRAM_H
#define PIET_NATIVE_PROGRAM_H

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "piet/loops.h"  // piet::loops::DispatchFn
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

namespace piet::program {

using ojson = nlohmann::ordered_json;
using DispatchFn = piet::loops::DispatchFn;

// Consume `program [define] NAME … end` blocks from *lines*, mutating
// session.programs (adding/replacing definitions) and returning a new line
// list with all program-definition lines removed. Other lines are preserved
// verbatim and in order.
//
// Called from doexec::executeDoSource AFTER preprocessDo and BEFORE brace
// aggregation.
//
// Handles:
//   - `program define NAME` / `program NAME` headers, optionally `, rclass`.
//   - `program drop NAME` / `program dir` are left as pass-through lines
//     (handled by the dispatcher's native handler when they reach dispatch
//     as plain commands) EXCEPT for the redefine-idiom stripping below.
//   - The redefine idiom: a `[capture] program drop NAME` that immediately
//     precedes (skipping blank/comment lines) a `program define NAME` for
//     the SAME name is stripped from the output. In sequential Stata that
//     drop targets a pre-existing NAME; in this whole-file pre-pass capture
//     it would otherwise run LATER (after defines are pulled out, it
//     becomes adjacent to the driver) and delete the just-captured program.
//     A drop AFTER a define (genuine mid-script undefine) is left in place.
//   - Nested `program … end` blocks are NOT supported (Stata does not allow
//     them either). A bare `end` without a matching `program` header is
//     silently dropped (defensive — Stata does the same).
std::vector<std::string> captureProgams(const std::vector<std::string>& lines, SessionState& session);

// BUG-342 — the INSTALL sentinel.
//
// captureProgams registers definitions at PREPROCESS time, i.e. before any
// statement of the do-file has run. Anything executed later that clears the
// program table therefore deletes definitions that appear AFTER it in the
// source and have not been reached yet. `clear all` (handlers_m3.cpp, the
// `program drop _all` half) is exactly that, so the near-universal Stata
// prologue
//     clear all
//     program define foo ... end
//     foo
// left `foo` undefined at the call. The redefine-idiom stripping described
// above is the same defect handled as a special case for ONE statement
// (`program drop NAME`); the sentinel generalizes it instead of growing that
// list one clearing statement at a time.
//
// captureProgams leaves one sentinel line where each consumed block stood and
// keeps an untouched copy in session.programArchive. Reaching the sentinel
// during execution re-installs that copy — so a program becomes defined at the
// point its `program define` appears, which is Stata's own rule. The line
// produces no output and does not consume a line number, so a do-file that
// never clears the table is byte-identical and stopped_at_line-identical to
// before.
extern const std::string kProgInstallSentinelPrefix;

// If `line` is an install sentinel, return true and write the program name to
// `nameOut`. Cheap prefix test first — this runs once per top-level item.
bool isProgInstallSentinel(const std::string& line, std::string* nameOut);

// Build the positional-arg locals map from a raw argument string. Sets:
//   "0" -> full argstr (Stata `0')
//   "1" -> first token
//   "2" -> second token
//   ...
// At most 20 positional args are extracted (Stata supports `1'..`9' plus
// more via `args`; 20 is a safe generous cap). Uses the shared, quote-aware
// scanOneToken (same tokenizer gettoken uses) so program arg-splitting and
// gettoken peel tokens identically.
//
// Empty tokens ARE stored (an explicit `""` argument is a real positional
// that holds a slot) — dropping them would shift every later `N' reference.
std::map<std::string, std::string> buildArgs(const std::string& argstr);

// Execute a user-defined program body.
//
// Parameters:
//   progDef    — the ProgramDef to execute.
//   argstr     — the argument string after the program name (i.e. `0').
//   session    — current session (mutated in place).
//   dispatchFn — the dispatch callable to replay body lines.
//
// Returns: array of result blocks (echo + output/error blocks from the
// body), OR a one-element engine_error block array if the recursion guard
// tripped (see kMaxRecursion note above).
//
// Scope contract:
//   - A new local scope is pushed before the body runs and popped via an
//     RAII guard so it is always released even if dispatchFn / executeDoSource
//     throws.
//   - Error blocks from the body propagate upward unchanged so an enclosing
//     `capture` can set _rc correctly.
//   - On successful return from an rclass program the staging area is
//     flushed into session.eval.rValues / session.eval.rStrings.
ojson runProgram(const ProgramDef& progDef, const std::string& argstr, SessionState& session,
                  const DispatchFn& dispatchFn);

// BUG-922: restore every preserved copy taken deeper than `callerLevel`
// (SessionState::preserveLevel() before the program or nested do/run file
// started) -- what Stata does when that program or do-file concludes, on
// success and on error alike. Only a failed restore leaves a block, appended
// to `blocks`; it stops the unwinding there. The one unwinder (session_state.h
// PreserveFrame I3); defined in handlers_m3.cpp beside runRestore.
void restorePreservesDeeperThan(int callerLevel, SessionState& session, ojson& blocks);

// BUG-922: everything Stata undoes when a program returns or a nested do/run
// file or `parallel` child concludes, on success and on error alike: the
// preserved copies deeper than `callerLevel` (restorePreservesDeeperThan),
// then the tempfiles, tempname scalars/matrices and tempvars minted deeper
// than it (oracles w2_prog_controlflow pc_pr_in_program*, pc_tv_gone;
// w2ad_prog_preserve_nest pn_d_leave*, pn_tv_do_gone).
void concludeLevel(int callerLevel, SessionState& session, ojson& blocks);
// The same on a C++ exception path: never throws, drops any block.
void concludeLevelAfterThrow(int callerLevel, SessionState& session) noexcept;

// BUG-920: a program or nested do/run file starts outside any loop. While one
// lives, SessionState::loopBodyFloor is the loop depth it started at, so a
// `continue` in it outside its own loops is r(119) "statement out of context"
// (oracle w2ad_prog_preserve_nest pn_lm_cb_prog, pn_lm_cb_do).
class LoopFloorGuard {
 public:
  explicit LoopFloorGuard(SessionState& session);
  ~LoopFloorGuard();
  LoopFloorGuard(const LoopFloorGuard&) = delete;
  LoopFloorGuard& operator=(const LoopFloorGuard&) = delete;

 private:
  SessionState& session_;
  int saved_;
};

// Native handler for `program drop NAME` / `program dir`. Returns a single
// result block. sub is "drop" or "dir"; names is the list of names for
// "drop" (ignored for "dir").
ojson handleProgramManage(const std::string& sub, const std::vector<std::string>& names,
                           SessionState& session);

// Step-0 handler for `return scalar NAME=EXPR` / `return local NAME val`.
//
// Returns true if the line was handled (a return statement, staged into
// session.returnStaging) — the caller should emit no console output ([]) —
// and false if the line is not a return statement at all (caller proceeds
// to try_capture_definition / normal dispatch).
//
// When the line IS a malformed/erroring return statement that itself must
// surface as an error block (mirrors the Python sentinel carrying a
// {"type":"error",...} dict instead of the {"__captured__": true} sentinel),
// this returns true and populates outErrorBlock; callers must check
// `!outErrorBlock.empty()` to distinguish "captured, silent" from
// "captured, but surface this error". In practice (see program.py) neither
// return-scalar nor return-local branch currently produces an error block —
// evaluation failures degrade to NaN / best-effort string rather than
// erroring — so outErrorBlock is always left empty by this port; the
// parameter exists so the C++ signature has the same extensibility as the
// Python `dict | None` return and dispatcher.cpp can wire it uniformly with
// the "malformed forms surface an error" contract described in the task
// brief.
bool handleReturnStmt(const std::string& rawLine, SessionState& session, ojson& outErrorBlock);

// Case-insensitive lookup; returns the canonical stored name or nullopt.
std::optional<std::string> findProgram(const SessionState& session, const std::string& name);

}  // namespace piet::program

#endif  // PIET_NATIVE_PROGRAM_H
