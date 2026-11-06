// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Loop interpreter — C++ port of piet/backend/piet/router/loops.py:
// foreach / forvalues / while / if-else block execution.
//
// Design mirrors the oracle: brace aggregation happens in doexec.cpp BEFORE
// this module runs; execute_loop() receives an already-aggregated body (a
// flat list of source lines — nested blocks are re-serialized to their
// `header {` / body / `}` text form by doexec.cpp's flattenBody, exactly
// like doexec.py's _flatten_body) and re-parses it through
// doexec::executeDoSource for nested-block handling.
#ifndef PIET_NATIVE_LOOPS_H
#define PIET_NATIVE_LOOPS_H

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

namespace piet::loops {

using ojson = nlohmann::ordered_json;

// dispatch()-compatible callable: (raw_line, session) -> list[ResultBlock].
using DispatchFn = std::function<ojson(const std::string&, SessionState&)>;

// loops.is_loop_header: true if `line` starts with foreach/forvalues/while/if
// (keyword test only — brace detection is the caller's job).
bool isLoopHeader(const std::string& line);

// True iff `line` is a `parallel[, options] {` block head. Exposed for
// doexec.cpp, which must NOT apply its one-varlist-card-per-top-level-command
// thinning to a parallel block: runParallelBlock already thinned per TASK,
// because a task's output must match what the statement would have produced
// unwrapped (design brief §2.5). Uses the same headKeyword() the block
// recognizer uses, so the two can never disagree about what a parallel head is.
bool isParallelHeader(const std::string& line);

// True when a `{` somewhere in `line` actually OPENS a brace block, as opposed
// to merely appearing inside the arguments of the command the line runs.
//
// BUG-167: every brace-recognition site used to ask only
// `isLoopHeader(line) && line.find('{') != npos`, i.e. "first word is a block
// keyword AND a brace appears anywhere". That is wrong for the PREFIX-class
// keywords (capture/quietly/noisily): `capture noisily nl (y = {b0=2}+...)`
// has `capture` first and a brace later, so the aggregator built a block whose
// header was `capture noisily nl (y = {`, ran `b0=2` as the body, and DROPPED
// the rest of the line — a command silently replaced by a fragment of its own
// arguments. Stata opens a block only when the brace directly follows a
// complete header, which for the prefix class means the keyword(s) themselves.
//
// Rule: for foreach/forvalues/while/if the text before the brace is the header
// (condition/list included), so any brace opens a block. For the prefix class
// the text before the brace must consist SOLELY of prefix-class keywords
// (`capture {`, `quietly {`, `capture noisily {` are all legal headers).
bool opensBraceBlock(const std::string& line);

// loops.execute_loop: run a brace-aggregated loop/if block. `header` may or
// may not include the trailing `{` (stripped internally). Returns the
// accumulated result blocks; a `_control` sentinel block (type:"_control")
// may be the LAST element when a continue/break bubbles out of the taken
// branch (execute_do_source's caller filters and propagates it — U7/S11).
ojson executeLoop(const std::string& header, const std::vector<std::string>& bodyLines,
                   const std::vector<std::string>& elseBodyLines, SessionState& session,
                   const DispatchFn& dispatchFn);

// P4 wave: split Stata's BRACELESS one-line branch `if COND command ...` into
// its condition and its command. Returns false when the split cannot be made
// confidently — the caller must then fall through to its existing honest
// reject rather than guessing, because a mis-split would silently run a
// DIFFERENT command than the user wrote (or run one that should not have run
// at all). Never throws.
bool splitOneLineIf(const std::string& rest, std::string& cond, std::string& body);

// BUG-371: `foreach ... of varlist x*` has always expanded wildcards and `a-b`
// ranges through this; `ds` and `unab` are the SAME question asked directly, so
// they ask the same function rather than growing a second expander that could
// disagree about what `x*` means. Trailing-`*` prefixes and `first-last` ranges
// only — that is the whole vocabulary PIET has ever supported here.
struct VarlistExpandResult {
  bool isError = false;
  nlohmann::ordered_json error;
  std::vector<std::string> tokens;
};
VarlistExpandResult expandVarlist(const std::string& spec, SessionState& session);
// expandVarlist passes a literal name through unchecked; this is the check that
// every one is a variable, an unambiguous abbreviation (rewritten in place to its
// full name) or else r(111) "variable NAME not found" / "NAME ambiguous
// abbreviation" (foreach's own, checkKnownVarTokens).
std::optional<nlohmann::ordered_json> checkKnownVarlist(std::vector<std::string>& tokens,
                                                        SessionState& session);

// The numlist expander forvalues/foreach-of-numlist use, for a command option
// that takes a numlist: blank-separated numbers, `a(s)b` and `a/b` ranges. Each
// element comes back as its %15.0g text; false on a malformed element.
bool expandNumlist(const std::string& spec, std::vector<std::string>* elements);

// EXPRLAYER DRIFT GUARD (P2-PERF). evalBoolExpr collects the referenced-name
// set of a rewritten condition with a hand-written left-to-right scan instead
// of an sregex_iterator over `[A-Za-z_]\w*`, because the zone profile put
// 15.4 ms of one S01 run inside that iterator. Same contract as
// eval::selftestMacroDefScanMatchesRegex: re-run BOTH over a corpus of
// condition shapes and report the first disagreement. A gate, not a
// diagnostic.
bool selftestBoolIdentScanMatchesRegex(std::string* firstDisagreement);

// Run a braceless `if COND cmd` through the SAME machinery the braced form
// uses (macro expansion with unsetRAsMissing, then evalBoolExpr), so the two
// spellings can never disagree about what a condition means.
ojson runOneLineIf(const std::string& condition, const std::string& bodyLine,
                   SessionState& session, const DispatchFn& dispatchFn);

// BUG-547: Stata's legacy `for` command (the pre-version-8 loop that foreach
// and forvalues replaced; still valid Stata, and 34 corpus lines across 5
// papers use it). isLegacyForLine says whether a console/do line is a `for`
// statement; runLegacyFor runs one and returns its output blocks.
// Behaviour as measured on StataBE 19.5; oracle
// tests/eval/golden/aer_w2/w2_cmdtriage.log's `for_*` arms.
bool isLegacyForLine(const std::string& line);

ojson runLegacyFor(const std::string& rawLine, SessionState& session,
                   const DispatchFn& dispatchFn);

// BUG-483: is the statement now being dispatched inside a LOOP body?
//
// `continue` is the only thing that needs the answer, and nothing else could
// give it. Stata's contract, measured 2026-09-18
// (tests/eval/golden/bug483_484_control_oracle.log and
// tests/eval/golden/bug483b_loop_oracle.log): inside a loop `continue` advances
// the iteration; outside every loop it is "statement out of context", r(119),
// and execution GOES ON.
//
// The decision has to be made in the dispatcher, not later in doexec, because
// only there is the block still inside `capture`'s reach: an error synthesised
// after dispatch leaves `capture noisily continue` with _rc 0 AND an untagged
// error that halts the file — both wrong, and the second one is BUG-483 again
// wearing a different hat.
//
// Only the three LOOP constructs count. `if`, `capture`, `quietly` and
// `parallel` run their bodies through the same runBody() and must NOT: the
// oracle shows `if` is transparent in one direction (a `continue` inside a
// TOP-LEVEL `if` block is still r(119)) and `quietly` in the other (a loop
// inside `quietly` still takes its `continue` — 3 of 4 iterations).
int loopBodyDepth();

// BUG-1434: what a quiet scope (`quietly`, or a `capture` no `noisily` follows)
// keeps of the output under it: command echoes, errors, control markers, and
// the output of a `noisily` nested inside it. StataBE 19.5: `quietly {
// noisily display "a5" }` and `capture { noisily display "a4" }` both print
// their line; `quietly pn`, where the program pn says `noisily display`, does
// not -- the nested noisily reaches no further than its own program.
//
// markNoisily tags such output ("noisily": true) when a quiet scope encloses
// it; keptWhenQuiet is the filter every quiet scope applies; settleNoisily
// drops the tag once no quiet scope is left around the blocks (or at a program
// boundary), so it never reaches the transcript.
void markNoisily(ojson& blocks, const SessionState& session);
bool keptWhenQuiet(const ojson& block);
void settleNoisily(ojson& blocks, const SessionState& session, bool force = false);

// RAII, and thread_local behind it: a parallel worker is its own process, but
// nothing here promises the loop interpreter is only ever entered from one
// thread of one process, and a depth counter that could be shared across
// threads would answer for the wrong stack.
struct LoopBodyScope {
  LoopBodyScope();
  ~LoopBodyScope();
  LoopBodyScope(const LoopBodyScope&) = delete;
  LoopBodyScope& operator=(const LoopBodyScope&) = delete;
};

}  // namespace piet::loops

#endif  // PIET_NATIVE_LOOPS_H
