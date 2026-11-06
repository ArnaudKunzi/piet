// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Subprocess wrapper around the cobra C++ engine binary — the native-side
// analog of piet/backend/piet/cobra_runner.py. cobra stays a subprocess in
// 2.0 (Master Plan, Boundary A untouched); M0 wired `cobra version --json`;
// M2 upgrades the drain loop (concurrent stdout/stderr via poll(), fd-leak
// fix on the pipe-failure branch — the ROADMAP "M0 shell-review
// carry-forward" that binds at M2, see cobra_runner.cpp) and adds
// runCobraJson(), the JSON-parsing convenience wrapper every M2 handler
// uses (mirrors cobra_runner.py's run(parse=True)).
#ifndef PIET_NATIVE_COBRA_RUNNER_H
#define PIET_NATIVE_COBRA_RUNNER_H

#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "vendor/nlohmann/json.hpp"

namespace piet {

class SessionState;  // fwd: the dispatch-active session the runCobra intercept routes to.

struct CobraRunResult {
  bool ok = false;             // process launched AND exited 0
  int exit_code = -1;          // -1 if the process could not be launched at all
  std::string stdout_text;
  std::string stderr_text;
  std::string error;           // launch-time error message (empty if ok launching)
};

// Resolves the cobra binary path. Order (mirrors config.py's documented
// order, Art 7 provenance): $COBRA_BIN env var (if it exists on disk) >
// the pinned Art 7 reference (releases/engine/current/cobra in the private
// coordination tree, when one exists on the build machine) >
// bare `cobra` on $PATH. Never resolves to an op-cobra dev build.
std::string resolveCobraBin();

// Runs cobra with the given argv (NOT including the binary itself) and
// captures stdout/stderr/exit code. Drains stdout+stderr CONCURRENTLY via
// poll() (M2: replaces M0's sequential read-to-EOF, which deadlocks once
// either pipe fills its OS buffer — e.g. large --tidy JSON on stdout while
// cobra writes a warning to stderr). Blocks the calling thread — callers on
// the UI thread must offload to a worker thread (see piet/shell.h's
// threading note).
CobraRunResult runCobra(const std::vector<std::string>& args);

// MAXPERF Stage 1b: the ORIGINAL per-verb fork/exec+pipes one-shot. runCobra
// (above) is now a thin router — when a dispatch-active SessionState is
// installed (see below) it routes through that session's persistent
// `cobra serve` worker (spawn-tax elimination + resident-frame data plane,
// cobra_session.h); otherwise, and as the universal fallback whenever the
// worker is unavailable, it calls this. Byte-for-byte the legacy behavior.
CobraRunResult runCobraOneShot(const std::vector<std::string>& args);

// Dispatch-active session install (thread_local stack). dispatcher.cpp's
// dispatch() wrapper brackets EVERY (possibly recursive) call via
// SessionState::enterDispatch()/exitDispatch(), which push/pop the driving
// session here so the free runCobra() — called from 90+ handler sites that
// never received a SessionState& — can find the worker to route to. A
// thread_local stack (not a global) keeps the live UI thread and the
// do-executor worker thread independent, and nests correctly. activeSession()
// is null outside any dispatch (startup version probe, some direct routes) —
// runCobra then uses runCobraOneShot, exactly as before this stage.
void pushActiveSession(SessionState* s);
void popActiveSession();
SessionState* activeSession();

// CobraError — native analog of cobra_runner.py's CobraError dataclass.
// Thrown by runCobraJson() on any failure (non-zero exit, malformed JSON,
// launch failure); carries the engine's stable `code` when the failure was
// a structured EngineError on stderr ({"error":{"code","message","detail"?}}).
struct CobraError : std::runtime_error {
  std::string code;      // e.g. "factor_misuse", "internal", or "engine_error"
  std::string detail;
  std::vector<std::string> argv;
  int returncode = 0;

  CobraError(std::string code_, const std::string& message, std::string detail_ = "",
             std::vector<std::string> argv_ = {}, int rc = 0)
      : std::runtime_error(message), code(std::move(code_)), detail(std::move(detail_)),
        argv(std::move(argv_)), returncode(rc) {}
};

// Runs cobra and parses stdout as JSON on success; throws CobraError on any
// failure (mirrors cobra_runner.py's run()). `args` does NOT include the
// binary name.
nlohmann::json runCobraJson(const std::vector<std::string>& args);

// BUG-038(a) FIX (W3 fuzzer stabilization): renders a CobraError as the
// final, user-facing message string — the ONE mapping point every
// "[code] message" formatter scattered across the handler files
// (handlers.cpp/handlers_m3.cpp/handlers_data_verbs.cpp/egen.cpp's own
// `cobraErrorStr`, estimation_m4.cpp's `cobraErrorStrM4`, winsor2.cpp's
// `cobraErrorStrW`, dispatcher.cpp's inline copy) now forwards to, instead
// of each independently formatting "[" + ce.code + "] " + ce.what(). cobra's
// `code: "internal"` is its generic C++-exception catch-all (any expression
// error the evaluator itself doesn't recognize, e.g. an unimplemented
// builtin — `errors.h`'s frozen ERROR_CODES allow-list does not even
// include "internal") — it is NEVER a stable, documented error shape, so it
// must never reach a user labeled as such (Doctrine I-3). Two honest
// rewrites for that one code; every OTHER (stable) code keeps the existing
// bracketed "[code] message" shape, unchanged:
//   - "expr: unknown function NAME in '...'" -> "function NAME() is not
//     supported" (the exact shape the fuzzer campaign's mod()-gap finding,
//     BUG-038(b), hit — naming the missing builtin honestly).
//   - any OTHER `internal` message -> the detail text verbatim, wrapped as
//     an honest engine-error message with no "[internal]" tag at all.
std::string cobraErrorMessage(const CobraError& ce);

}  // namespace piet

#endif  // PIET_NATIVE_COBRA_RUNNER_H
