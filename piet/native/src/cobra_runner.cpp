// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/cobra_runner.h"

#include <chrono>
#include "piet/counters.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <regex>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <thread>
#else
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <sstream>

#include "piet/bundle_paths.h"
#include "piet/eval/errors.h"  // invalidDateLiteral, unbalancedExpr, timeSpanRefusal (cobraErrorMessage)
#include "piet/session_state.h"  // Stage 1b: the intercept routes to the active session's worker
#include "io/win_compat.h"  // cecon::native_exists (BUG-050 wide _waccess)

namespace piet {

namespace {

bool fileExists(const std::string& path) {
  if (path.empty()) return false;
#ifdef _WIN32
  return cecon::native_exists(path);
#else
  return access(path.c_str(), F_OK) == 0;
#endif
}

// Art 7 (CONSTITUTION Part 2, §7): PIET calls only the main build or a pinned
// bin/cobra-X.Y.Z reference — never an op-cobra dev build. This mirrors the
// $COBRA_BIN > pinned-reference > $PATH order documented in
// piet/backend/piet/config.py.
// GA sweep 2026-08-22 (leg ⑤, clonable build). This used to be a hardcoded
// absolute path into ONE developer's disk layout — it broadcast that layout in
// shipped source and, worse, resolved to nothing on any other machine, so a
// stranger who cloned the repo got a silent fall-through to $PATH. The pin is
// now build-injectable (-DPIET_PINNED_COBRA=…), with an empty default meaning
// "no dev pin", which is the correct state for a clone. Order is unchanged for
// this machine, because the packaging/dev builds define it.
#ifndef PIET_PINNED_COBRA
#define PIET_PINNED_COBRA ""
#endif
constexpr const char* kPinnedReference = PIET_PINNED_COBRA;

#ifndef _WIN32
// Close a pipe's both ends, ignoring an already-closed/invalid fd (-1).
void closePipe(int fds[2]) {
  if (fds[0] >= 0) close(fds[0]);
  if (fds[1] >= 0) close(fds[1]);
}
#endif

}  // namespace

std::string resolveCobraBin() {
  // $COBRA_BIN always wins when set and present — the escape hatch Eval/the
  // acceptance harness use to point a headless run at a specific engine
  // build, same as the frozen Python backend's config.py.
  if (const char* env = std::getenv("COBRA_BIN")) {
    if (fileExists(env)) return env;
  }
  // M5 GA: when piet_native is running FROM INSIDE a packaged .app
  // (PIET.app/Contents/MacOS/piet_native), prefer the cobra binary bundled
  // alongside it at Contents/Resources/cobra over the dev-tree Art 7 pin. A
  // packaged .app must be self-contained and reproducible off the machine
  // that built it — the dev pin path (releases/engine/current/cobra in the
  // private coordination tree) is a worktree-relative dev convenience that won't exist on a
  // machine that only has the .app installed. bundleResourcesDir() returns
  // std::nullopt for any non-bundled invocation (dev build run in place,
  // --run-block/--selftest headless harness against piet/native/build/
  // piet_native directly), so this branch is a no-op there and falls
  // through to the existing dev-pin resolution unchanged.
  if (std::optional<std::string> resources = bundleResourcesDir()) {
#ifdef _WIN32
    std::string bundled = *resources + "\\cobra.exe";  // WP-W5: Windows bundle name
#else
    std::string bundled = *resources + "/cobra";
#endif
    if (fileExists(bundled)) return bundled;
  }
  if (kPinnedReference[0] != '\0' && fileExists(kPinnedReference)) return kPinnedReference;
  // A clone with no pin and no bundle: look beside the running binary the way a
  // built tree lays itself out (piet/native/build/piet_native alongside a
  // repo-root build/cobra). This is what makes `cmake && ctest` work for someone
  // who is not on the author's machine — previously the only paths were an env
  // var they had not been told about and a $PATH lookup that usually failed.
  if (std::optional<std::string> exe = executablePath()) {
    const std::size_t slash = exe->find_last_of("/\\");
    if (slash != std::string::npos) {
      const std::string selfDir = exe->substr(0, slash);
      // piet/native/build/piet_native -> ../../../build/cobra (repo-root build),
      // and a co-located cobra for a flat install.
#ifdef _WIN32
      const char* rel[] = {"/../../../build/cobra.exe", "/../../../../build/cobra.exe", "/cobra.exe"};
#else
      const char* rel[] = {"/../../../build/cobra", "/../../../../build/cobra", "/cobra"};
#endif
      for (const char* r : rel) {
        const std::string cand = selfDir + r;
        if (fileExists(cand)) return cand;
      }
    }
  }
  return "cobra";  // fall back to $PATH; runCobra will report failure if absent
}

#ifdef _WIN32

namespace {

// Quote one argv element per the CommandLineToArgvW / MSVCRT parsing rules so
// CreateProcess's single command-line string round-trips back to the exact
// argv the child sees (spaces, quotes, backslash-before-quote). Mirrors the
// intent of the POSIX path's argv array (no shell, no interpolation).
std::string windowsQuoteArg(const std::string& arg) {
  if (!arg.empty() &&
      arg.find_first_of(" \t\n\v\"") == std::string::npos) {
    return arg;  // no quoting needed
  }
  std::string out = "\"";
  for (auto it = arg.begin();; ++it) {
    size_t backslashes = 0;
    while (it != arg.end() && *it == '\\') {
      ++it;
      ++backslashes;
    }
    if (it == arg.end()) {
      out.append(backslashes * 2, '\\');  // escape trailing backslashes
      break;
    } else if (*it == '"') {
      out.append(backslashes * 2 + 1, '\\');  // escape backslashes + the quote
      out.push_back('"');
    } else {
      out.append(backslashes, '\\');
      out.push_back(*it);
    }
  }
  out.push_back('"');
  return out;
}

// Drain a read pipe handle to EOF into `sink`. Run on its own thread for the
// pipe we are NOT reading on the calling thread, so a full OS pipe buffer on
// one stream can never deadlock the other (the Win32 analogue of the POSIX
// path's poll()-based concurrent drain).
void drainHandle(HANDLE h, std::string* sink) {
  char buf[4096];
  DWORD n = 0;
  while (ReadFile(h, buf, sizeof(buf), &n, nullptr) && n > 0) {
    sink->append(buf, n);
  }
}

}  // namespace

CobraRunResult runCobraOneShot(const std::vector<std::string>& args) {
  // T6: the true one-shot spawn — the other lowest-layer engine entry.
  // W0 (P2PERF §W0): a nonzero one_shot_spawns is the honest witness that this
  // session was NOT on the persistent-worker plane (PIET_NO_SERVE, or the worker
  // died twice). Any timing taken beside it is a different experiment.
  piet::counters::addOneShotSpawn();
  struct Bill {
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    ~Bill() {
      piet::counters::addEngineWaitNs(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now() - t0).count());
    }
  } __bill;
  CobraRunResult result;
  std::string bin = resolveCobraBin();

  // Windows subprocess path (WP-W2): CreateProcess + anonymous pipes replaces
  // the POSIX fork/exec/poll. No shell (no cmd.exe) — the command line is built
  // by explicit per-arg quoting (windowsQuoteArg), so args are never subject to
  // shell interpolation, matching the POSIX argv-array guarantee.
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;  // child inherits the pipe WRITE ends
  sa.lpSecurityDescriptor = nullptr;

  HANDLE outRd = nullptr, outWr = nullptr, errRd = nullptr, errWr = nullptr;
  if (!CreatePipe(&outRd, &outWr, &sa, 0)) {
    result.error = "CreatePipe() failed (stdout)";
    return result;
  }
  if (!CreatePipe(&errRd, &errWr, &sa, 0)) {
    result.error = "CreatePipe() failed (stderr)";
    CloseHandle(outRd);
    CloseHandle(outWr);
    return result;
  }
  // The parent's READ ends must NOT be inherited by the child (else the child
  // holds a write-side reference and our reads never see EOF).
  SetHandleInformation(outRd, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(errRd, HANDLE_FLAG_INHERIT, 0);

  std::string cmdline = windowsQuoteArg(bin);
  for (const auto& a : args) {
    cmdline.push_back(' ');
    cmdline.append(windowsQuoteArg(a));
  }
  std::wstring wcmd = [&]() {
    if (cmdline.empty()) return std::wstring();
    int len = MultiByteToWideChar(CP_UTF8, 0, cmdline.c_str(),
                                  static_cast<int>(cmdline.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, cmdline.c_str(),
                        static_cast<int>(cmdline.size()), &w[0], len);
    return w;
  }();

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  si.hStdOutput = outWr;
  si.hStdError = errWr;
  PROCESS_INFORMATION pi{};

  // CreateProcessW mutates the command-line buffer in place, so pass a
  // writable copy.
  std::vector<wchar_t> cmdbuf(wcmd.begin(), wcmd.end());
  cmdbuf.push_back(L'\0');

  // CREATE_NO_WINDOW: cobra.exe is a CONSOLE-subsystem binary, so without this
  // Windows allocates a console for it — a black cmd window that flashes up on
  // every single dispatch (and stays for the duration of a long estimation).
  // piet_native is a GUI app; its engine calls must be invisible. We already
  // own all three std handles via the pipes above, so the child has nothing to
  // show. Not DETACHED_PROCESS: that also detaches from the job/console the
  // parent may need for handle inheritance semantics, and is stronger than the
  // problem. Harmless on a console-less service launch too (nothing to hide).
  BOOL ok = CreateProcessW(/*lpApplicationName=*/nullptr, cmdbuf.data(),
                           /*procAttrs=*/nullptr, /*threadAttrs=*/nullptr,
                           /*bInheritHandles=*/TRUE, /*flags=*/CREATE_NO_WINDOW,
                           /*env=*/nullptr, /*cwd=*/nullptr, &si, &pi);
  // Parent closes the write ends regardless — otherwise our reads never hit
  // EOF (the child is the only writer that matters).
  CloseHandle(outWr);
  CloseHandle(errWr);
  if (!ok) {
    CloseHandle(outRd);
    CloseHandle(errRd);
    result.error = "CreateProcess() failed";
    result.exit_code = -1;
    return result;
  }

  // Concurrent drain: read stderr on a worker thread, stdout on this thread.
  std::string outText, errText;
  std::thread errThread(drainHandle, errRd, &errText);
  drainHandle(outRd, &outText);
  errThread.join();
  CloseHandle(outRd);
  CloseHandle(errRd);

  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 0;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);

  result.stdout_text = std::move(outText);
  result.stderr_text = std::move(errText);
  result.exit_code = static_cast<int>(code);
  result.ok = (result.exit_code == 0);
  return result;
}

#else  // POSIX (macOS / Linux)

CobraRunResult runCobraOneShot(const std::vector<std::string>& args) {
  // T6: the true one-shot spawn — the other lowest-layer engine entry.
  // W0 (P2PERF §W0): a nonzero one_shot_spawns is the honest witness that this
  // session was NOT on the persistent-worker plane (PIET_NO_SERVE, or the worker
  // died twice). Any timing taken beside it is a different experiment.
  piet::counters::addOneShotSpawn();
  struct Bill {
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    ~Bill() {
      piet::counters::addEngineWaitNs(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now() - t0).count());
    }
  } __bill;
  CobraRunResult result;
  std::string bin = resolveCobraBin();

  // Build the shell-safe argv via POSIX pipe + fork/exec (no shell
  // interpolation — args are passed as an argv array, never concatenated
  // into a command string). This is macOS/Linux only; the Windows subprocess
  // path is the CreateProcess branch above (WP-W2).
  int outPipe[2] = {-1, -1};
  int errPipe[2] = {-1, -1};
  if (pipe(outPipe) != 0) {
    result.error = "pipe() failed (stdout)";
    return result;
  }
  if (pipe(errPipe) != 0) {
    // M2 fd-leak fix (ROADMAP M0 carry-forward, self-flagged in the M0
    // commit): the outPipe fds were leaked on this branch — close them
    // before returning.
    result.error = "pipe() failed (stderr)";
    closePipe(outPipe);
    return result;
  }

  pid_t pid = fork();
  if (pid < 0) {
    result.error = "fork() failed";
    closePipe(outPipe);
    closePipe(errPipe);
    return result;
  }

  if (pid == 0) {
    // Child: wire pipes to stdout/stderr, exec cobra.
    dup2(outPipe[1], STDOUT_FILENO);
    dup2(errPipe[1], STDERR_FILENO);
    close(outPipe[0]);
    close(outPipe[1]);
    close(errPipe[0]);
    close(errPipe[1]);

    // RAMDIET-AFTERCARE D: same knob, same reason as cobra_session.cpp's serve spawn
    // (see the measurement there). Stateless `cobra <verb>` invocations hold the same
    // frames and pay the same retention, so both engine entry points get it.
    // OFF arm, same binary, one branch apart — the discipline COBRA_NO_CARRY /
    // COBRA_NO_SPAN already set here: a knob whose effect can only be measured by
    // rebuilding cannot be re-measured cheaply, and this one WILL need re-measuring
    // when the allocator or the workload moves.
    if (std::getenv("PIET_NO_SPACE_EFFICIENT") == nullptr)
      ::setenv("MallocSpaceEfficient", "1", 1);

    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(bin.c_str()));
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    execvp(bin.c_str(), argv.data());
    // execvp only returns on failure.
    _exit(127);
  }

  // Parent.
  close(outPipe[1]);
  close(errPipe[1]);
  outPipe[1] = -1;
  errPipe[1] = -1;

  // M2: concurrent stdout/stderr drain via poll() (ROADMAP M0 carry-forward
  // — "cobra_runner concurrent drain": sequential read-to-EOF on one pipe
  // then the other deadlocks once the OTHER pipe's OS buffer fills while its
  // writer blocks on write(2), e.g. a large --tidy coeftable JSON on stdout
  // interleaved with a warning on stderr). Both fds are read as they become
  // ready; each is marked done at EOF (read()==0) and dropped from the poll
  // set; the loop exits once both are done.
  std::ostringstream outBuf, errBuf;
  std::array<char, 4096> chunk{};
  bool outDone = false, errDone = false;
  while (!outDone || !errDone) {
    std::vector<struct pollfd> fds;
    std::vector<int> kinds;  // 0=stdout, 1=stderr, parallel to fds
    if (!outDone) {
      fds.push_back({outPipe[0], POLLIN, 0});
      kinds.push_back(0);
    }
    if (!errDone) {
      fds.push_back({errPipe[0], POLLIN, 0});
      kinds.push_back(1);
    }
    int rc = poll(fds.data(), fds.size(), -1);
    if (rc < 0) {
      if (errno == EINTR) continue;
      result.error = "poll() failed while draining cobra output";
      break;
    }
    for (size_t i = 0; i < fds.size(); ++i) {
      if (!(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
      int fd = fds[i].fd;
      ssize_t n = read(fd, chunk.data(), chunk.size());
      if (n > 0) {
        (kinds[i] == 0 ? outBuf : errBuf).write(chunk.data(), n);
      } else {
        // n==0 (EOF) or n<0 (error, e.g. the far end closed) — either way
        // this pipe is done.
        if (kinds[i] == 0) {
          outDone = true;
        } else {
          errDone = true;
        }
      }
    }
  }
  close(outPipe[0]);
  close(errPipe[0]);

  int status = 0;
  waitpid(pid, &status, 0);

  result.stdout_text = outBuf.str();
  result.stderr_text = errBuf.str();
  if (WIFEXITED(status)) {
    result.exit_code = WEXITSTATUS(status);
    result.ok = (result.exit_code == 0);
  } else {
    result.exit_code = -1;
    result.error = "cobra process did not exit normally (signaled/stopped)";
  }
  return result;
}

#endif  // _WIN32

// -- dispatch-active session install + the runCobra router (Stage 1b) -------
namespace {
// thread_local so the UI thread and the do-executor worker thread each carry
// their own active-session stack (a single AppState session is driven by both
// over its lifetime, but never concurrently). A stack (not a scalar) nests
// correctly under dispatch()'s recursive capture/quietly/by/do-file replay.
thread_local std::vector<SessionState*> tl_sessionStack;
}  // namespace

void pushActiveSession(SessionState* s) { tl_sessionStack.push_back(s); }
void popActiveSession() {
  if (!tl_sessionStack.empty()) tl_sessionStack.pop_back();
}
SessionState* activeSession() {
  return tl_sessionStack.empty() ? nullptr : tl_sessionStack.back();
}

// The router every one of the 90+ handler call sites reaches. When a session
// is dispatch-active, route through its persistent worker (resident-frame
// intercept, session_state.cpp); otherwise the legacy one-shot spawn.
CobraRunResult runCobra(const std::vector<std::string>& args) {
  // T6 NOTE: engine-wait is billed at the LOWEST layer only — CobraSession::
  // request (serve) and runCobraOneShot (spawn). Billing here as well would
  // double-count every serve call, because runViaCobraSession funnels into
  // request(). Measured while building this: it reported 71.4s of engine wait
  // inside a 54.2s run, which is how the double-count announced itself.
  if (SessionState* s = activeSession()) {
    return s->runViaCobraSession(args);
  }
  return runCobraOneShot(args);
}

namespace {

// Build a CobraError from a failed run — prefers the structured JSON error
// on stderr ({"error":{"code","message","detail"?}}), same shape cecon_cli
// emits (see apps/cecon_cli.cpp), falling back to the first stderr line.
// Mirrors cobra_runner.py's _error_from_proc().
CobraError errorFromResult(const CobraRunResult& r, const std::vector<std::string>& argv) {
  std::string stderrText = r.stderr_text;
  // trim trailing whitespace
  while (!stderrText.empty() && (stderrText.back() == '\n' || stderrText.back() == '\r' ||
                                  stderrText.back() == ' ')) {
    stderrText.pop_back();
  }
  try {
    nlohmann::json payload = nlohmann::json::parse(stderrText);
    nlohmann::json err = payload.contains("error") ? payload.at("error") : payload;
    if (err.is_object() && err.contains("message")) {
      return CobraError(err.value("code", std::string("engine_error")),
                         err.at("message").get<std::string>(), err.value("detail", std::string()),
                         argv, r.exit_code);
    }
  } catch (const nlohmann::json::exception&) {
    // fall through to plain-text handling
  }
  std::string firstLine = stderrText;
  auto nl = firstLine.find('\n');
  if (nl != std::string::npos) firstLine = firstLine.substr(0, nl);
  if (firstLine.empty()) firstLine = "cobra exited " + std::to_string(r.exit_code);
  return CobraError("engine_error", firstLine, stderrText, argv, r.exit_code);
}

}  // namespace

nlohmann::json runCobraJson(const std::vector<std::string>& args) {
  CobraRunResult r = runCobra(args);
  if (!r.error.empty() && r.exit_code < 0) {
    // Launch-time failure (pipe/fork/exec — execvp failure surfaces as exit
    // code 127, handled by the ok==false branch below, not here).
    throw CobraError("engine_error", "cobra binary not found or could not launch", r.error, args);
  }
  // BUG-039 FIX (W3 fuzzer stabilization): cobra can legitimately exit
  // NONZERO while still printing a complete, valid result JSON to stdout —
  // an estimator that ran to completion but did not CONVERGE (verified
  // directly: `cobra logit --data ... --y treat --x region,cl --tidy` on a
  // degenerate/collinear draw exits 1, stderr is EMPTY, stdout is a full
  // `{"coeftable":[...], "converged": false, "iters": 0, ...}` payload —
  // the fuzzer's seed-1 `logit female cohort gv2` (gv2 constant) and
  // seed-999 `logit treat region cl` repros both hit this exact shape).
  // The pre-fix code below (`if (!r.ok) throw errorFromResult(...)`) never
  // looked at stdout at all on a nonzero exit, so this real, honest
  // non-convergence diagnostic was discarded in favor of a generic
  // "[engine_error] cobra exited 1" — a Doctrine I-3 violation (a lossy
  // failure summary standing in for the real diagnostics that were right
  // there). This is SAFE to do unconditionally before checking r.ok: cobra
  // NEVER writes its `{"error": {...}}` envelope to stdout (verified
  // across usage errors/exit 2, internal errors/exit 1, and data_io
  // errors/exit 3 — all three land the error JSON on STDERR with stdout
  // completely empty; errorFromResult()'s own doc comment already
  // documents this as the established convention). So a nonzero exit with
  // non-empty, PARSEABLE stdout JSON is never a masked error — it is always
  // a genuine result the caller must see, and it flows through the exact
  // same return path (and thus the exact same downstream block-building,
  // e.g. cobraToEstimationM4's `converged`/`iters` passthrough) that a
  // clean exit-0 result would. Only when stdout is empty/unparseable do we
  // fall back to the stderr-based generic error, unchanged from before.
  if (!r.stdout_text.empty()) {
    try {
      return nlohmann::json::parse(r.stdout_text);
    } catch (const nlohmann::json::exception&) {
      // Not parseable JSON — fall through to the ok/error handling below.
    }
  }
  if (!r.ok) {
    throw errorFromResult(r, args);
  }
  std::string snippet = !r.stdout_text.empty() ? r.stdout_text : r.stderr_text;
  if (snippet.size() > 500) snippet = snippet.substr(0, 500);
  throw CobraError("engine_error", "cobra produced non-JSON output", snippet, args, r.exit_code);
}

// See cobra_runner.h's header comment for the rationale (BUG-038(a)).
std::string cobraErrorMessage(const CobraError& ce) {
  if (ce.code == "internal") {
    std::string msg = ce.what();
    static const std::regex reUnknownFn(R"(unknown function (\w+))");
    std::smatch m;
    if (std::regex_search(msg, m, reUnknownFn)) {
      return "function " + m.str(1) + "() is not supported";
    }
    // FU-852: the Parser's "expr: tq(2010q5) invalid in '<expression>'" is
    // Stata's "tq(2010q5) invalid" (r(198), eval::coarseRc), whatever expression
    // held it.
    const std::string badLiteral = eval::invalidDateLiteral(msg);
    if (!badLiteral.empty()) return badLiteral + " invalid";
    // FU-875: a syntax error in an expression whose brackets do not pair is
    // Stata's "too few ')' or ']'" / "too many ')' or ']'" (r(132), coarseRc).
    const std::string unbalanced = eval::unbalancedExpr(msg);
    if (!unbalanced.empty()) return unbalanced;
    // ... and a tin()/twithin() call it could not expand is "invalid syntax"
    // (r(198)), or "time-series operators not allowed" (r(101)) with no tsset.
    std::string spanRc;
    const std::string span = eval::timeSpanRefusal(msg, &spanRc);
    if (!span.empty()) return span;
    // BUG-1368: an unknown name in a row expression is Stata's `X not found`
    // (r(111); `count if nosuchvar > 0`, `generate g = nosuchvar + 1`, `assert
    // nosuchvar > 0` ..., w2n_followups.log:1051), not the engine's text
    // `engine error: expr: unknown variable X in '...'` (src/data/frame.cpp) or
    // `... unknown variable 'X'` (src/core/expr_eval.h). eval::coarseRc reads
    // the sentence as 111.
    static const std::regex reUnknownVar(R"(unknown variable (?:'([^']+)'|(\S+) in '))");
    if (std::regex_search(msg, m, reUnknownVar)) {
      return (m[1].matched ? m.str(1) : m.str(2)) + " not found";
    }
    return "engine error: " + msg;
  }
  return "[" + ce.code + "] " + ce.what();
}

}  // namespace piet
