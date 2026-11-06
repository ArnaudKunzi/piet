// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// do-executor — C++ port of piet/backend/piet/doexec.py: preprocess + brace-
// aggregate + replay .do source through the dispatcher, one top-level item
// (command line or aggregated loop/if block) at a time.
//
// Thread-lifetime discipline (ROADMAP "M0 shell-review carry-forward",
// binding at M3): executeDoSource does real background work when invoked
// as a run's progress owner (B1 liveness). It NEVER touches piet::Shell or
// spawns its own std::thread — callers that want live UI progress push
// (M4) are responsible for running executeDoSource on a worker thread they
// own and joining it before any Shell teardown; this module only publishes
// to SessionState's mutex-guarded RunProgress snapshot (safe to read from
// any thread) and never holds a dangling Shell&/window reference itself.
#ifndef PIET_NATIVE_DOEXEC_H
#define PIET_NATIVE_DOEXEC_H

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "piet/loops.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

namespace piet::doexec {

using ojson = nlohmann::ordered_json;
using DispatchFn = loops::DispatchFn;

// N6 (P8.1-W2, contract_B 2.4.0; extended B.3.0/F6): live block-streaming sink.
// When supplied by the TOP-LEVEL do/do_string entry point (runDoInBackground),
// it is invoked synchronously — on the run's worker thread — with each
// ResultBlock in order, so the shell can push a `{kind:"block", seq, block}`
// event over pietEvents as the run progresses. It fires for every block the run
// emits, INCLUDING those streamed after the stored-transcript ceiling trips
// (F6a: storage stops, streaming does not). Streaming changes only WHEN/WHETHER
// a block is delivered live, never WHAT the STORED blocks array holds
// (freeze/--do output unaffected). Default nullptr = no streaming (every non-
// interactive caller: --do, loop bodies, program bodies, run_block, selftest) —
// out.blocks is byte-identical with and without the hook.

struct ExecuteResult {
  ojson blocks = ojson::array();
  std::optional<int> stoppedAtLine;
  std::optional<std::string> stoppedAtReason;
  // BUG-139: set (0 for bare `exit`/`exit, clear`, N for `exit N`) only when
  // the halt above was a clean Stata `exit`, never for a genuine failure —
  // see the doexec.cpp anonymous-namespace helper (parseCleanExitMessage)
  // this is populated from. A caller that wants an honest process exit code
  // for `--do` (rather than collapsing every halt to failure) reads this
  // instead of trying to infer it from stoppedAtReason/the block's `code`
  // (CONTRACT.md's CommandErrorResult.code enum has no dedicated exit code,
  // so the block itself is indistinguishable from a real engine_error).
  std::optional<int> stoppedAtExitCode;
  // U7/S11: a continue/break sentinel that bubbled out of the WHOLE source
  // (only meaningful when this call is itself a loop/if body run — top-level
  // do-file execution never returns this to its caller).
  std::optional<std::string> control;
  // F6c: number of leading `blocks` entries that were ALSO delivered via
  // onBlock (a PREFIX — the only stored-but-unstreamed block is the terminal
  // cancel). runDoInBackground returns blocks[streamedCount:] as the response's
  // un-streamed tail so already-streamed blocks are not re-sent. 0 whenever no
  // onBlock hook was supplied (then the whole array is the "tail").
  int streamedCount = 0;
};

// execute_do_source: preprocess (comments/continuations/#delimit reject),
// capture `program define ... end` blocks, pre-split single-line loop forms,
// brace-aggregate, then replay each top-level item through dispatchFn.
// progressOwner=true resets/owns the session's RunProgress snapshot (B1) —
// only the top-level do-file/do_string entry points pass true; nested calls
// (loop bodies, program bodies, console loop headers) pass false but still
// check the cancel flag per top-level item.
using BlockSink = std::function<void(const ojson&)>;

ExecuteResult executeDoSource(const std::string& source, SessionState& session,
                               const DispatchFn& dispatchFn,
                               const std::vector<std::string>& args = {},
                               bool progressOwner = false,
                               std::optional<std::string> doPath = std::nullopt,
                               BlockSink onBlock = nullptr);

// execute_do: read a .do file from disk, resolve to an absolute path
// (stamped as resolvedPath regardless of read success — mirrors doexec.py's
// B2 resolved_path contract), then call executeDoSource with progressOwner
// = true. On a read failure returns a single engine_error block.
struct ExecuteFileResult {
  ExecuteResult result;
  std::string source;         // "" on read failure
  std::string resolvedPath;
};
ExecuteFileResult executeDo(const std::string& filepath, SessionState& session,
                            const DispatchFn& dispatchFn,
                            const std::vector<std::string>& args = {},
                            BlockSink onBlock = nullptr);

// F8 — thin a block array down to ONE `varlist_after` card, on the last
// varlist-bearing data_mutation block. executeDoSource applies this per
// TOP-LEVEL item, so a statement that mutates several times still emits a
// single variable-list card.
//
// Exported 2026-08-14 (P9 entry wave) for `loops::runParallelBlock`. A brace
// block is normally one top-level item, so a loop's whole body shares one card
// — but the parallel block's contract is that its tasks produce the output they
// would have produced as ordinary top-level statements (design brief §2.5,
// byte-identical to serial). It therefore has to apply this per TASK, and the
// probe's A0 arm is what proved it: without the per-task call, task 1-3's cards
// were thinned away and only task 4 kept one, so the wrapper changed output at
// every worker count while cmp(A1,AN) stayed perfectly blind to it.
// W2 doinclude (BUG-530): the PRIVATE local scope a nested `do`/`run` gets
// (StataBE 19.5, w2_include_scope.log arms do_basic/run_basic/do_args_private/
// do_then_include: the caller's locals are invisible inside, the file's locals
// are gone after, and positional args seed `0'/`1'.. in the callee only).
// `include` deliberately does NOT construct one -- it shares the caller's scope
// both ways. Constructed only by the dispatcher's do/run arm: the top-level
// `--do` and GUI routes call executeDo directly and keep the session's locals.
//
// Same push/pop shape as program.cpp's ScopeGuard (session.scopeStack, LIFO,
// restored on every exit path including a throw). `_rc` is carried both ways:
// Stata's _rc is one session-wide value, not a local, and PIET only stores it in
// the locals table (dispatcher.cpp's capture arm) -- so a private scope must not
// hide the caller's _rc from the file nor drop the file's last _rc on return.
class PrivateLocalScope {
 public:
  explicit PrivateLocalScope(SessionState& session);
  ~PrivateLocalScope();
  PrivateLocalScope(const PrivateLocalScope&) = delete;
  PrivateLocalScope& operator=(const PrivateLocalScope&) = delete;

 private:
  SessionState& session_;
  size_t depth_;  // scopeStack size after our push: restore only our own frame
};

// W2 doinclude (BUG-528): the nesting limit for do/run/include files ([R] limits:
// 64 nested do-files). executeDo refuses one level deeper instead of recursing
// without bound (a self-including file used to overflow the C++ stack). The rc
// Stata answers at depth 65 is NOT oracle-covered; PIET answers engine_error.
constexpr int kMaxDoNesting = 64;

// W2 doinclude (BUG-529): "If filename is specified without an extension, .do
// is assumed" ([R] do, [P] include; w2_include_scope.log do_noext /
// include_noext / include_gbrace_noext answer rc 0). Only the LAST path
// component is inspected, so a dotted directory (`v1.2/run`) still gets the
// default and a name with any extension (`inner.do`, BUG-174's leg) is returned
// unchanged. This is the COMMAND's rule, applied by the dispatcher's
// do/run/include arm -- not inside executeDo, whose top-level `--do`/GUI callers
// open exactly the file they were handed (an extensionless script, /dev/stdin).
std::string withDefaultDoExtension(const std::string& filepath);

void coalesceMutationVarlists(ojson& blocks);

}  // namespace piet::doexec

#endif  // PIET_NATIVE_DOEXEC_H
