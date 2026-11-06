// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// PIET 2.0 native shell — entry point.
//
// Usage:
//   piet_native                open the real frontend (piet/frontend/) in
//                               the native webview, bridge-backed (M4b)
//   piet_native --demo          open the OLD M0 walking-skeleton demo page
//                               (piet/native/demo/index.html) — kept for the
//                               bridge self-test / historical reference, no
//                               longer the default
//   piet_native --selftest      run the headless self-test (no window) and
//                               exit 0/1 — see selftest.cpp
//   piet_native --evalcase      M1 differential-parity driver: JSONL eval
//                               cases on stdin -> JSONL results on stdout
//                               (see piet/native/tests/eval_diff/)
//   piet_native --run-block     M2 candidate JSON adapter: one piet2-corpus
//                               cell JSON object on stdin -> one block-JSON
//                               result object on stdout (see src/run_block.cpp)
//   piet_native --version       print the COMPILED piet-axis version tuple as
//                               one JSON line and exit 0 (2.0.2 WP-2 single
//                               version source: the packaging script derives
//                               Info.plist from THIS output — see
//                               piet/native/CMakeLists.txt's definition-site
//                               comment)
//   piet_native --ui-smoke      packaged-app UI journey gate (2.0.2 WP-1):
//                               opens the REAL window/WKWebView, drives a
//                               scripted boot -> bridge -> open-dataset ->
//                               varlist -> estimation -> rendered-block
//                               journey, writes a JSON verdict file, exits
//                               0 (GREEN) / 1 (RED) — see runUiSmoke() below
//
// M4b (ROADMAP §2.0.0 "M4b — frontend swap + graph rendering + run-cancel"):
// this is where the real frontend (piet/frontend/, ~4,900 LOC, UNTOUCHED
// beyond api.js's transport swap) gets loaded against the FULL bridge route
// surface (routes.cpp) for the first time — M0's toy demo page is retired
// as the default. The real do-executor's RunProgress publishing goes live
// off the main thread here for the first time (thread-lifetime discipline,
// ROADMAP "M0 shell-review carry-forward" #1, binds here): AppState owns a
// joinable background-run thread registry (NOT a detached fire-and-forget
// thread like M0's synthetic progress.start) so a window-close during an
// in-flight run joins cleanly instead of leaving a dangling Shell&.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#else
#include <signal.h>   // kill(pid, 0) — BUG-133 orphan-scratch liveness probe
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include "base64.h"
#include "piet/bundle_paths.h"
#include "piet/counters.h"
#include "piet/strmutate.h"
#include "piet/doexec.h"
#include "piet/dispatcher.h"
#include "piet/handlers.h"
#include "piet/parallel.h"
#include "piet/progress.h"
#include "piet/routes.h"
#include "piet/session_state.h"
#include "piet/settings.h"
#include "piet/shell.h"
#include "registry.h"
#include "io/win_compat.h"  // cecon::native_exists / to_native_path (BUG-050 wide open)
#include "vendor/nlohmann/json.hpp"

// Defined in selftest.cpp, external linkage (deliberately outside the
// anonymous namespace below — it's a cross-TU symbol, not file-local).
int runSelftest();

// Defined in src/eval/evalcase.cpp — the M1 differential-parity driver
// (reads JSONL eval cases on stdin, writes JSONL results on stdout; see
// piet/native/tests/eval_diff/).
namespace piet::eval {
int runEvalcaseDriver();
}

// Defined in src/run_block.cpp — the M2 candidate JSON adapter (reads one
// piet2-corpus cell JSON object on stdin, writes one block-JSON result
// object to stdout; see that file's header comment for the protocol).
namespace piet {
int runBlockAdapter();
}

namespace piet {
// R1 (selftest split): the app-support block that lived here (defaultUserCwd,
// readDoFileForBridge, the BUG-133 scratch-owner trio) moved VERBATIM to
// src/app_support.cpp so piet_selftest can link it without main.cpp. These are
// the same forward declarations selftest.cpp has always used.
std::string defaultUserCwd();
struct FileReadResult {
  bool ok = false;
  std::string errorMessage;
  std::string source;
  std::string resolvedPath;
};
FileReadResult readDoFileForBridge(const SessionState& session, const std::string& rawPath);
const char* scratchOwnerMarkerName();
void writeScratchOwnerMarker(const std::string& dir);
int reapDeadScratch(const std::string& base);
}  // namespace piet

namespace {

std::string readFileOrDie(const std::string& path) {
  std::ifstream f(cecon::to_native_path(path), std::ios::binary);
  if (!f) {
    std::fprintf(stderr, "[piet_native] FATAL: cannot open page at %s\n", path.c_str());
    std::exit(1);
  }
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// Resolves a path relative to the development checkout this binary belongs
// to (compile-time anchor when valid, else the executable's location, else
// the cwd — see piet/bundle_paths.h).
std::string repoRelative(const std::string& suffix) {
  // BUG-1939: __FILE__ is only the first candidate; a -ffile-prefix-map build
  // makes it relative, and the executable's own location then has to anchor
  // the tree (piet::devTreePath, shared with sysuse / c(sysdir_sample) /
  // the notices route).
  return piet::devTreePath(suffix);
}

bool fileExists(const std::string& path) {
  return cecon::native_exists(path);
}

// M5 GA: resolves piet/frontend/index.html — bundled Resources/frontend/
// FIRST when running from inside a packaged .app, dev-tree __FILE__-relative
// path otherwise. Mirrors resolveCobraBin()'s bundle-first order
// (cobra_runner.cpp) so both halves of "what does a packaged .app load"
// (the UI bundle, the engine binary) resolve the same way for the same
// reason: a shipped .app must be self-contained off the machine that built
// it, and a dev checkout must keep working unmodified for every other
// invocation (dev `cmake --build` binary run in place, --run-block/
// --selftest headless harnesses, Eval's acceptance scripts run against
// piet/native/build/piet_native directly).
// Item 3 (loud-failure hardening): a packaged .app whose bundled
// Resources/frontend/index.html is missing (a packaging-script regression —
// see the Windows "resources\ runtime layer" canary bug this same
// stabilization wave already fixed once for the cobra binary) must NOT
// silently fall through to the dev-tree repoRelative() path: that path is
// derived from this SOURCE FILE's build-time __FILE__ location, which on a
// shipped .app is some OTHER machine's checkout and essentially never
// exists — navigating a file:// URL that resolves to nothing leaves a blank
// window with no diagnostic (exactly the ERR_NAME_NOT_RESOLVED-shaped
// failure mode). So: try the packaged path first; if that's not there, try
// the dev-tree path as a best-effort fallback (covers running a freshly
// packaged .app in place on the SAME machine that built it, e.g. local
// packaging QA); only when NEITHER exists do we die loud instead of
// silently navigating a dead path.
std::string resolveFrontendIndexPath() {
  std::optional<std::string> resources = piet::bundleResourcesDir();
  std::string bundled;
  if (resources.has_value()) {
    bundled = *resources + "/frontend/index.html";
    if (fileExists(bundled)) return bundled;
  }
  std::string dev = repoRelative("/piet/frontend/index.html");
  if (fileExists(dev)) return dev;

  std::fprintf(stderr,
               "[piet_native] FATAL: frontend index.html not found.\n"
               "  bundled candidate:  %s\n"
               "  dev-tree candidate: %s\n",
               resources.has_value() ? bundled.c_str() : "(not running from inside a packaged .app bundle)",
               dev.c_str());
  std::exit(1);
}

// AppState — owns the ONE persistent SessionState the live UI's bridge
// routes all close over (unlike run_block.cpp's fresh-session-per-cell
// harness), plus the background-run thread registry (thread-lifetime
// discipline, see file header). A do/do_string call spawns a worker thread
// that:
//   1. Runs doexec::executeDo/executeDoSource (which internally publishes
//      to session.progress_ — mutex-guarded, safe to read concurrently from
//      routes::runProgress on the UI thread's bind() callback).
//   2. Periodically (and at the end) calls shell.postEvent() with the SAME
//      RunProgress JSON schema (safe from any thread per shell.h's
//      threading note) so the frontend's progress strip updates live
//      instead of only on poll.
//   3. Resolves the ORIGINAL bind() promise (via BridgeResult delivered
//      through a std::promise/future pair) once done, so `await
//      pietNative.request('do_string', ...)` still returns the final
//      {blocks,...} payload the frontend awaits — do/do_string remain
//      request/response from the JS caller's point of view; the progress
//      PUSH is an independent side channel, not a replacement for the
//      response.
// The thread is OWNED (joined, never detached) — AppState::joinAll() is
// called before the window closes (Shell::terminate() is UI-thread-only per
// shell.h; a run's worker thread never calls it — it only postEvent()s,
// which is thread-safe from anywhere).
struct AppState {
  explicit AppState(std::string workdir) : session(std::move(workdir)) {}

  piet::SessionState session;

  std::mutex runThreadsMutex;
  // BUG-391 (2026-09-02): each run thread carries its own completion flag. The
  // old comment here said reaping "happens in joinAll(), called after each run
  // completes" — joinAll() was called ONLY at shutdown, so every do/do_string run
  // left a joinable thread (handle + stack) in this vector for the life of the
  // app. std::thread has no is-done query; the flag is the query. The worker sets
  // it as its very last statement, so a thread whose flag is set is guaranteed
  // to be past its last access to the Shell/AppState and joins without waiting.
  struct RunThread {
    std::thread t;
    std::shared_ptr<std::atomic<bool>> done;
  };
  std::vector<RunThread> runThreads;

  // Reap every FINISHED thread (join it, drop its slot) — called at the start
  // of each new do/do_string call and before shutdown, so runThreads is bounded
  // by the number of runs still executing (normally 0 or 1).
  void reapFinished() {
    std::lock_guard<std::mutex> lock(runThreadsMutex);
    std::vector<RunThread> stillRunning;
    for (auto& rt : runThreads) {
      if (rt.t.joinable() && rt.done && rt.done->load(std::memory_order_acquire)) {
        rt.t.join();
        continue;
      }
      if (rt.t.joinable()) stillRunning.push_back(std::move(rt));
    }
    runThreads = std::move(stillRunning);
  }

  size_t liveRunThreads() {
    std::lock_guard<std::mutex> lock(runThreadsMutex);
    return runThreads.size();
  }

  void registerThread(std::thread t, std::shared_ptr<std::atomic<bool>> done) {
    std::lock_guard<std::mutex> lock(runThreadsMutex);
    runThreads.push_back(RunThread{std::move(t), std::move(done)});
  }

  // Joins every registered thread. Safe to call multiple times. MUST be
  // called before the Shell is destroyed (window close) — this is the
  // "owned join point" the thread-lifetime discipline requires: no run
  // thread outlives the Shell it postEvent()s into.
  void joinAll() {
    std::lock_guard<std::mutex> lock(runThreadsMutex);
    for (auto& rt : runThreads) {
      if (rt.t.joinable()) rt.t.join();
    }
    runThreads.clear();
  }
};

// Background do/do_string runner: spawns an OWNED worker thread (registered
// with AppState, joined at shutdown — never detached) that executes the
// do-source, pushes RunProgress ticks over pietEvents as it goes, and
// resolves the bridge promise with the final result once done. `shell` is
// captured by pointer (not reference) into the thread lambda specifically
// so the thread-lifetime discipline is visible at the call site: the
// pointer is only ever dereferenced from a thread AppState::joinAll() joins
// before the Shell is torn down, so it never dangles — see the file header
// and AppState's doc comment for the full argument.
void runDoInBackground(piet::Shell* shell, AppState* app, const std::string& seq,
                        const std::string& source, std::vector<std::string> args,
                        std::optional<std::string> filepath) {
  app->reapFinished();
  auto runFinished = std::make_shared<std::atomic<bool>>(false);  // BUG-391
  std::thread worker([shell, app, seq, source, args = std::move(args),
                       filepath = std::move(filepath), runFinished]() {
    // BUG-087/D9 (BETA Phase F): mark this exact thread as the one
    // SessionState::requestUserInput() (a blocking wait) may safely run on
    // — see session_state.h's isCurrentThreadADoWorker() doc comment for
    // why blocking any OTHER thread (the UI thread in particular) would
    // deadlock the app. Set once, at the top of this thread's one-shot
    // body; no corresponding reset needed since the thread terminates when
    // this lambda returns.
    piet::setCurrentThreadIsDoWorker(true);

    // Progress-push side channel: poll the session's own progress snapshot
    // on a short interval WHILE the run's main work happens synchronously
    // below on this SAME worker thread would be wrong (executeDoSource
    // blocks until done) — instead, spawn a second, even shorter-lived
    // "ticker" thread that reads progressSnapshot() (mutex-guarded, safe
    // concurrent read) and postEvent()s it every 100ms until a stop flag
    // (set by this outer thread right before returning) tells it to stop
    // and push one FINAL snapshot.
    std::atomic<bool> runDone{false};
    // The snapshot carries the running statement's text, which can hold a
    // byte that is not UTF-8; a strict dump would throw on this bare thread
    // and std::terminate the app, so it goes out as U+FFFD (BUG-1131).
    std::thread ticker([shell, app, &runDone]() {
      while (!runDone.load()) {
        shell->postEvent(app->session.progressSnapshot().dump(
            -1, ' ', false, nlohmann::ordered_json::error_handler_t::replace));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    });

    // N6 (P8.1-W2, contract_B 2.4.0): live block streaming. This sink is
    // called SYNCHRONOUSLY from executeDo*/executeDoSource on THIS worker
    // thread (the only writer of streamedSeq — no lock needed on the counter),
    // once per block as it is appended, and pushes a `{kind:"block", seq,
    // block}` event over the SAME pietEvents conduit the RunProgress ticker
    // uses. postEvent marshals onto the UI thread (dispatch_async — thread-
    // safe, so it interleaves fine with the concurrent ticker postEvent()s and
    // with any request_dialog push a mid-run `_request()` pause emits). seq is
    // monotone from 0; the final `streamed` below == this count.
    int streamedSeq = 0;
    auto onBlock = [shell, &streamedSeq](const nlohmann::ordered_json& block) {
      nlohmann::ordered_json ev;
      ev["kind"] = "block";
      ev["seq"] = streamedSeq++;
      ev["block"] = block;
      shell->postEvent(ev.dump(-1, ' ', false, nlohmann::ordered_json::error_handler_t::replace));
    };

    // F6c tail-only return: move blocks[streamedCount:] — the UN-streamed
    // remainder (the terminal cancel block, or the whole array for a no-onBlock
    // caller) — into the response. The already-streamed prefix is NOT re-sent;
    // this eliminates F6's full-array copy+dump spike (the old code copied the
    // ENTIRE stored array into the JSON, dumped it, shipped it over the bridge,
    // and the frontend then discarded blocks[:streamed]). std::move out of the
    // (dead-after-this) ExecuteResult, so no deep copy.
    auto tailBlocks = [](piet::doexec::ExecuteResult& r) {
      nlohmann::ordered_json tail = nlohmann::ordered_json::array();
      int start = r.streamedCount < 0 ? 0 : r.streamedCount;
      for (int i = start; i < static_cast<int>(r.blocks.size()); ++i) {
        tail.push_back(std::move(r.blocks[static_cast<size_t>(i)]));
      }
      return tail;
    };

    nlohmann::ordered_json out;
    try {
      if (filepath.has_value()) {
        piet::doexec::ExecuteFileResult fr =
            piet::doexec::executeDo(*filepath, app->session, piet::dispatch, args, onBlock);
        out["blocks"] = tailBlocks(fr.result);
        if (fr.result.stoppedAtLine.has_value()) out["stopped_at_line"] = *fr.result.stoppedAtLine;
        if (fr.result.stoppedAtReason.has_value())
          out["stopped_at_reason"] = *fr.result.stoppedAtReason;
        out["resolved_path"] = fr.resolvedPath;
      } else {
        piet::doexec::ExecuteResult r = piet::doexec::executeDoSource(
            source, app->session, piet::dispatch, args, /*progressOwner=*/true, std::nullopt, onBlock);
        out["blocks"] = tailBlocks(r);
        if (r.stoppedAtLine.has_value()) out["stopped_at_line"] = *r.stoppedAtLine;
        if (r.stoppedAtReason.has_value()) out["stopped_at_reason"] = *r.stoppedAtReason;
      }
      // F6c dedup contract (contract_B 2.5.0): `streamed` = count of
      // {kind:"block"} pushes this run. `blocks` above is now ONLY the
      // un-streamed tail, so the frontend appends ALL of it (total = the
      // `streamed` live blocks + these blocks — no slice). The cancel block,
      // never streamed, rides in via that tail. (Pre-2.5.0 the response echoed
      // the FULL array and the frontend sliced blocks[streamed:] — the frontend
      // MUST drop that slice; see CONTRACT.md v2.5.0.)
      out["streamed"] = streamedSeq;
    } catch (const std::exception& e) {
      // A C++-level exception mid-run (not a Stata-level error block, which
      // executeDoSource already reports as a normal block) — surface it the
      // same way Shell::installBridge()'s handler_exception path does, so
      // the frontend's error-shape mapping (M0 carry-forward #2, api.js
      // seam) sees ONE consistent shape regardless of where the failure
      // originated.
      out = nlohmann::ordered_json::object();
      out["blocks"] = nlohmann::ordered_json::array();
      nlohmann::ordered_json errBlock;
      errBlock["type"] = "error";
      errBlock["code"] = "engine_error";
      errBlock["message"] = std::string("run failed: ") + e.what();
      out["blocks"].push_back(errBlock);
    }

    runDone.store(true);
    ticker.join();
    // Final snapshot push AFTER progressFinish() has landed (executeDo*
    // calls progressFinish() internally before returning), so the last
    // pietEvents tick the frontend sees has running:false + the real
    // stopped_reason — never a stale "still running" snapshot.
    shell->postEvent(app->session.progressSnapshot().dump(
        -1, ' ', false, nlohmann::ordered_json::error_handler_t::replace));

    // Resolve the ORIGINAL bind() promise with the final {blocks,...}
    // payload. webview's resolve() is documented safe from any thread (it
    // internally dispatches onto the UI thread's run loop, same as
    // postEvent — see shell.h's threading note); Shell exposes this via
    // postEvent for events, but the request/response promise itself is
    // resolved through the SAME impl_->view.resolve() shell.cpp's bind()
    // callback uses — routed here via a dedicated Shell::resolveInvoke()
    // (see shell.h/.cpp) so do/do_string can return asynchronously instead
    // of blocking the UI thread for a potentially long run.
    // BUG-1372: a string function works on bytes, as Stata's do, so a block
    // may carry a split UTF-8 sequence; it is sent as U+FFFD (what a terminal
    // shows for Stata's own log byte) instead of failing the run.
    shell->resolveInvoke(seq, true,
                         out.dump(-1, ' ', false, nlohmann::ordered_json::error_handler_t::replace));
    // BUG-391: LAST statement — nothing above may run after this store, because
    // reapFinished() joins (and thereby lets AppState/Shell teardown proceed)
    // the moment it sees the flag.
    runFinished->store(true, std::memory_order_release);
  });
  app->registerThread(std::move(worker), runFinished);
}

void bindAllRoutes(piet::Shell& shell, AppState& app) {
  shell.bindRoute("echo", piet::routes::echo);
  shell.bindRoute("cobra.version", piet::routes::cobraVersion);

  shell.bindRoute("command", [&app](const std::string& body) {
    return piet::routes::command(app.session, body);
  });
  // do / do_string: ASYNC routes — see Shell::bindAsyncRoute's doc comment.
  // The handler below returns immediately (BridgeResult is ignored by the
  // async dispatch path); the real response comes later via
  // shell->resolveInvoke() from the background worker thread.
  shell.bindAsyncRoute(
      "do", [&app](piet::Shell* shell, const std::string& seq, const std::string& body) {
        nlohmann::json req;
        try {
          req = nlohmann::json::parse(body);
        } catch (const nlohmann::json::exception&) {
          shell->resolveInvoke(seq, false, R"({"code":"bad_json","message":"malformed request body"})");
          return;
        }
        std::string filepath = req.value("filepath", std::string());
        std::vector<std::string> args;
        if (req.contains("args") && req["args"].is_array()) {
          for (const auto& a : req["args"]) args.push_back(a.get<std::string>());
        }
        runDoInBackground(shell, &app, seq, std::string(), args, filepath);
      });
  shell.bindAsyncRoute(
      "do_string", [&app](piet::Shell* shell, const std::string& seq, const std::string& body) {
        nlohmann::json req;
        try {
          req = nlohmann::json::parse(body);
        } catch (const nlohmann::json::exception&) {
          shell->resolveInvoke(seq, false, R"({"code":"bad_json","message":"malformed request body"})");
          return;
        }
        std::string source = req.value("source", std::string());
        std::vector<std::string> args;
        if (req.contains("args") && req["args"].is_array()) {
          for (const auto& a : req["args"]) args.push_back(a.get<std::string>());
        }
        runDoInBackground(shell, &app, seq, source, args, std::nullopt);
      });

  shell.bindRoute("run_progress", [&app](const std::string& body) {
    return piet::routes::runProgress(app.session, body);
  });
  shell.bindRoute("run_cancel", [&app](const std::string& body) {
    return piet::routes::runCancel(app.session, body);
  });
  // request.answer (D9, BETA Phase F — BUG-087 _request(), new contract_B
  // additive route): POST /api/request/answer -> routing key "request.answer"
  // (api.js's pathToRoutingKey: strip "/api/", "/" -> "."; matches the SAME
  // convention "dialog.open"/"label.variable" already use). Answers a
  // pending `request_dialog` push and wakes the do-executor worker thread
  // blocked in SessionState::requestUserInput(). The actual work (map write
  // + notify_all) is sub-millisecond — this uses the async-resolve pattern
  // (bindAsyncRoute + shell->resolveInvoke(), called inline before this
  // handler returns) to reuse the SAME infra do/do_string's async routes
  // already established, per this feature's own implementation brief;
  // behaviorally it is indistinguishable from a synchronous bindRoute.
  shell.bindAsyncRoute(
      "request.answer",
      [&app](piet::Shell* shell, const std::string& seq, const std::string& body) {
        nlohmann::json req;
        try {
          req = nlohmann::json::parse(body);
        } catch (const nlohmann::json::exception&) {
          shell->resolveInvoke(seq, false,
                                R"({"code":"bad_json","message":"malformed request body"})");
          return;
        }
        std::string requestId = req.value("requestId", std::string());
        std::string value = req.value("value", std::string());
        app.session.resolveUserInput(requestId, value);
        shell->resolveInvoke(seq, true, R"({"ok":true})");
      });
  shell.bindRoute("open", [&app](const std::string& body) { return piet::routes::open(app.session, body); });
  // dialog.open (2.0.1): native Finder file picker (NSOpenPanel). body:
  // {"exts":"dta,csv,hbin"} (optional filter). Returns {"path":"<abs>"} or
  // {"cancelled":true}. Backs the frontend "Open data" / "Run .do" buttons,
  // which previously fell back to a window.prompt that WKWebView doesn't
  // implement (dead no-op). Runs the panel on the UI thread (synchronous
  // route), then the frontend calls the existing `open`/`do` route with the
  // returned path.
  shell.bindRoute("dialog.open", [&shell](const std::string& body) -> piet::BridgeResult {
    std::string exts;
    try {
      auto j = nlohmann::json::parse(body);
      if (j.is_object() && j.contains("exts") && j["exts"].is_string())
        exts = j["exts"].get<std::string>();
    } catch (const nlohmann::json::exception&) {
      // no/invalid body — treat as "no extension filter"
    }
    std::string path = shell.openFileDialog(exts);
    nlohmann::ordered_json out;
    if (path.empty())
      out["cancelled"] = true;
    else
      out["path"] = path;
    return piet::BridgeResult::success(out.dump());
  });
  // file.read (WS-F1 scope addendum, native-paths stabilization wave):
  // read-only .do source fetch backing the frontend's editor-first Run.do
  // flow. body: {"path":"<user-typed path>"}. Resolves via the SAME
  // piet::resolveUserPath() every other user-path-taking handler now uses
  // (item 2 of this wave) so a relative path follows the seeded
  // session.cwd exactly like `use`/`do` would. App-shell facility, not a
  // Boundary-B kernel endpoint — same classification family as
  // dialog.open/legal.notices (it only reads session.cwd, never mutates
  // SessionState, and has no directory-listing or write affordance). The
  // resolve+validate+read logic lives in piet::readDoFileForBridge() (just
  // above bindAllRoutes) so selftest.cpp can exercise it directly; this
  // lambda is thin JSON-in/JSON-out wiring, same shape as dialog.open.
  shell.bindRoute("file.read", [&app](const std::string& body) -> piet::BridgeResult {
    auto badRequest = [](const std::string& message) {
      nlohmann::ordered_json err;
      err["code"] = "bad_request";
      err["message"] = message;
      return piet::BridgeResult::failure(err.dump());
    };

    std::string rawPath;
    try {
      auto j = nlohmann::json::parse(body);
      if (!j.is_object() || !j.contains("path") || !j["path"].is_string()) {
        return badRequest("file.read: body must be {\"path\": string}");
      }
      rawPath = j["path"].get<std::string>();
    } catch (const nlohmann::json::exception& e) {
      return badRequest(std::string("file.read: invalid JSON body: ") + e.what());
    }

    piet::FileReadResult r = piet::readDoFileForBridge(app.session, rawPath);
    if (!r.ok) return badRequest(r.errorMessage);

    nlohmann::ordered_json out;
    out["source"] = r.source;
    out["resolved_path"] = r.resolvedPath;
    // A do-file saved in Latin-1 still opens: its non-UTF-8 bytes show as
    // U+FFFD instead of failing the route (BUG-1131).
    return piet::BridgeResult::success(
        out.dump(-1, ' ', false, nlohmann::ordered_json::error_handler_t::replace));
  });
  shell.bindRoute("varlist", [&app](const std::string& body) {
    return piet::routes::varlist(app.session, body);
  });
  shell.bindRoute("browse", [&app](const std::string& body) {
    return piet::routes::browse(app.session, body);
  });
  shell.bindRoute("history", [&app](const std::string& body) {
    return piet::routes::history(app.session, body);
  });
  shell.bindRoute("version", [&app](const std::string& body) {
    return piet::routes::version(app.session, body);
  });
  shell.bindRoute("rename", [&app](const std::string& body) {
    return piet::routes::rename(app.session, body);
  });
  shell.bindRoute("label.variable", [&app](const std::string& body) {
    return piet::routes::labelVariable(app.session, body);
  });
  shell.bindRoute("label.define", [&app](const std::string& body) {
    return piet::routes::labelDefine(app.session, body);
  });
  shell.bindRoute("label.values", [&app](const std::string& body) {
    return piet::routes::labelValues(app.session, body);
  });
  shell.bindRoute("sort", [&app](const std::string& body) {
    return piet::routes::sortRoute(app.session, body);
  });
  shell.bindRoute("recode", [&app](const std::string& body) {
    return piet::routes::recode(app.session, body);
  });
  shell.bindRoute("cell", [&app](const std::string& body) { return piet::routes::cell(app.session, body); });
  shell.bindRoute("tabulate", [&app](const std::string& body) {
    return piet::routes::tabulate(app.session, body);
  });
  shell.bindRoute("describe", [&app](const std::string& body) {
    return piet::routes::describe(app.session, body);
  });
  shell.bindRoute("summarize", [&app](const std::string& body) {
    return piet::routes::summarize(app.session, body);
  });
  // var_detail (2.4.0, N7): single-column stats for the Variables-panel
  // detail popover. GET /api/var_detail?name=X -> the JS shim reshapes the
  // query param into a {name} body (same convention as browse).
  shell.bindRoute("var_detail", [&app](const std::string& body) {
    return piet::routes::varDetail(app.session, body);
  });

  // file.save (2.4.0, N1): write an export to disk. body:
  // {suggested_name, kind, text?|content_b64?, path?}. An explicit `path`
  // writes there directly with NO dialog (the Settings default-export-folder
  // flow AND headless gates); no `path` pops a native NSSavePanel (the
  // interactive "Export…" flow). Response {ok:true, path} on success,
  // {ok:false, cancelled:true} if the panel was dismissed. A FAILED write is
  // an honest {type:"error"} (routes::fileSaveToPath), NEVER {ok:true}
  // (COMMS §4.8). Captures &shell for the save panel, same as dialog.open.
  shell.bindRoute("file.save", [&shell](const std::string& body) -> piet::BridgeResult {
    nlohmann::json req;
    try {
      req = nlohmann::json::parse(body);
    } catch (const nlohmann::json::exception& e) {
      nlohmann::ordered_json err;
      err["code"] = "bad_json";
      err["message"] = e.what();
      return piet::BridgeResult::failure(err.dump());
    }
    std::string suggested = req.value("suggested_name", std::string());
    std::string kind = req.value("kind", std::string());
    std::optional<std::string> text, contentB64;
    if (req.contains("text") && req["text"].is_string()) text = req["text"].get<std::string>();
    if (req.contains("content_b64") && req["content_b64"].is_string())
      contentB64 = req["content_b64"].get<std::string>();
    std::string path;
    if (req.contains("path") && req["path"].is_string()) path = req["path"].get<std::string>();
    if (path.empty()) {
      // No explicit destination -> native save panel on the UI thread (this
      // bind() callback runs on it; runModal spins a nested loop, like
      // dialog.open's openFileDialog). `kind` doubles as the extension filter.
      path = shell.saveFileDialog(suggested, kind);
      if (path.empty()) {
        nlohmann::ordered_json out;
        out["ok"] = false;
        out["cancelled"] = true;
        return piet::BridgeResult::success(out.dump());
      }
    }
    return piet::routes::fileSaveToPath(path, kind, text, contentB64);
  });

  // window.title (2.4.0, N2): POST /api/window/title {text} -> set the native
  // window title verbatim (frontend composes "PIET — <file>"). This bind()
  // callback runs on the UI thread, so setTitle (which calls the AppKit
  // set_title: the A16 site uses) is called directly on the main thread.
  shell.bindRoute("window.title", [&shell](const std::string& body) -> piet::BridgeResult {
    std::string text;
    try {
      auto j = nlohmann::json::parse(body);
      if (j.is_object() && j.contains("text") && j["text"].is_string())
        text = j["text"].get<std::string>();
    } catch (const nlohmann::json::exception& e) {
      nlohmann::ordered_json err;
      err["code"] = "bad_json";
      err["message"] = e.what();
      return piet::BridgeResult::failure(err.dump());
    }
    shell.setTitle(text);
    return piet::BridgeResult::success(R"({"ok":true})");
  });

  // legal.notices (WI-5): app-shell service route (no session), backing the
  // frontend's "About / Legal" acknowledgements panel — same classification
  // family as dialog.open (shell facility, not a Boundary-B kernel endpoint).
  shell.bindRoute("legal.notices", piet::routes::legalNotices);

  // settings.get / settings.set (2.2.0 item D): app-shell config routes (no
  // session) — same app-shell classification as legal.notices/dialog.open,
  // contract_B unchanged. Back the Settings panel's engine/scratch group.
  shell.bindRoute("settings.get", piet::routes::settingsGet);
  shell.bindRoute("settings.set", piet::routes::settingsSet);

  // M0 demo route, kept for the bridge self-test / --demo page.
  shell.bindRoute("progress.start", [&shell](const std::string&) -> piet::BridgeResult {
    std::thread([&shell]() {
      piet::runSyntheticProgress([&shell](const std::string& tickJson) { shell.postEvent(tickJson); });
    }).detach();
    return piet::BridgeResult::success(R"({"started":true})");
  });
}

#ifdef _WIN32
// Windows has no mkdtemp. Create a uniquely-named directory under the base:
// GetTempFileNameA yields a unique name (and creates a placeholder FILE); we
// delete that file and CreateDirectory the same path.
std::string makeWorkdir() {
  // BUG-050 (scratch side): a Korean username makes %TEMP% a Hangul path (and a
  // Hangul scratch dir can be set in Settings). Reading it with ANSI getenv and
  // creating the workdir with GetTempFileNameA/CreateDirectoryA mangles it
  // through the CP949/CP1252 codepage, so every command's scratch write fails.
  // Read the base WIDE and create the dir WIDE, then return UTF-8 — the encoding
  // the rest of piet hands to cobra (cobra_runner's CreateProcessW uses CP_UTF8),
  // keeping the whole workdir Unicode-clean end to end.
  auto wenv = [](const wchar_t* k) -> std::wstring {
    DWORD n = GetEnvironmentVariableW(k, nullptr, 0);
    if (n == 0) return std::wstring();
    std::wstring v(n, L'\0');
    DWORD m = GetEnvironmentVariableW(k, &v[0], n);
    v.resize(m);
    return v;
  };
  auto w2u = [](const std::wstring& w) -> std::string {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
  };
  std::wstring wbase = wenv(L"PIET_WORKDIR");
  if (wbase.empty()) wbase = wenv(L"TEMP");
  if (wbase.empty()) wbase = wenv(L"TMP");
  if (wbase.empty()) wbase = L".";
  // BUG-134 (Windows half): a Settings scratch_path seeded into PIET_WORKDIR may
  // not exist yet, and CreateDirectoryW below creates only the LAST component.
  // Mirror the POSIX branch's create_directories step so the first launch after
  // the user picks a new scratch dir does not fall through to the PID fallback.
  {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(wbase), ec);
  }
  // Atomic, collision-free per-process scratch dir (mirrors the POSIX mkdtemp
  // branch below). The prior GetTempFileNameW + DeleteFileW + CreateDirectoryW
  // sequence was a TOCTOU race: two piet_native processes started in the same
  // tick (e.g. the student opens the app twice) draw the same time-seeded name
  // from GetTempFileNameW, the DeleteFileW discards its on-disk uniqueness
  // guarantee, and the CreateDirectoryW loser fell through to a FIXED
  // "piet_native_fallback" dir — so concurrent losers then collided on
  // identically-named scratch files, fatal under the Windows mmap file-lock
  // (macOS's mkdtemp is atomic and random-named, so POSIX never saw this).
  // CreateDirectoryW is itself atomic (fails ERROR_ALREADY_EXISTS if the name
  // is taken), so retry it directly with a process-unique candidate and let the
  // OS arbitrate — there is no delete-then-create window for another process to
  // slip into. The last-resort fallback is PID-qualified so it cannot collide.
  // BUG-133: reap our own SIGTERM/SIGKILL-orphaned scratch dirs under this base
  // before minting a new one (mirrors the POSIX branch).
  piet::reapDeadScratch(w2u(wbase));
  const DWORD pid = GetCurrentProcessId();
  for (unsigned i = 0; i < 4096; ++i) {
    std::wstring cand =
        wbase + L"\\piet_native_" + std::to_wstring(pid) + L"_" + std::to_wstring(i);
    if (CreateDirectoryW(cand.c_str(), nullptr)) {
      std::string dir = w2u(cand);
      piet::writeScratchOwnerMarker(dir);  // BUG-133: stamp for later reaping
      return dir;
    }
    if (GetLastError() != ERROR_ALREADY_EXISTS) break;  // real error — stop retrying
  }
  return w2u(wbase) + "\\piet_native_fallback_" + std::to_string(pid);
}
#else
std::string makeWorkdir() {
  // Scratch base selection (PIET-TMP / BUG-092): PIET_WORKDIR (explicit env
  // override, or the Settings scratch_path seeded into it) wins; else honor the
  // POSIX TMPDIR convention — this is what lets a user or the GA-peel harness
  // redirect the per-op hbin/csv scratch off a nearly-full boot volume with
  // `TMPDIR=/big/vol` (previously TMPDIR was ignored and scratch always landed
  // in /tmp); else /tmp. Trailing slashes are trimmed so a `TMPDIR=/dir/` value
  // doesn't yield a `//piet_native_XXXXXX` double-slash template.
  const char* envDir = std::getenv("PIET_WORKDIR");
  if (!envDir || !*envDir) envDir = std::getenv("TMPDIR");
  std::string base = envDir && *envDir ? envDir : "/tmp";
  while (base.size() > 1 && base.back() == '/') base.pop_back();
  // BUG-134: mkdtemp(3) requires its parent directory to ALREADY exist. The
  // Settings scratch_path is seeded into PIET_WORKDIR (runRealFrontend, below)
  // with no validation, so the very first launch after a user picks a not-yet-
  // existing custom scratch dir made mkdtemp ENOENT out and silently degrade to
  // the fixed "piet_native_fallback" name — a NON-unique dir two concurrent
  // piet_native processes would share, letting the first to exit remove_all() it
  // out from under the other. Create the base first (best-effort, a no-op when it
  // already exists, as on the /tmp|TMPDIR gate paths) so the normal atomic random
  // mkdtemp path succeeds and the fallback is never reached for this reason.
  {
    std::error_code ec;
    std::filesystem::create_directories(cecon::to_native_path(base), ec);
  }
  // BUG-133: before minting our own dir, reap any of OUR earlier scratch dirs
  // under this base that a SIGTERM/SIGKILL orphaned (dead owner pid + settled
  // mtime). Runs once per process (makeWorkdir is called once per entry point).
  piet::reapDeadScratch(base);
  std::string tmpl = base + "/piet_native_XXXXXX";
  std::vector<char> buf(tmpl.begin(), tmpl.end());
  buf.push_back('\0');
  char* result = mkdtemp(buf.data());
  if (result) {
    piet::writeScratchOwnerMarker(result);  // BUG-133: stamp for later reaping
    return std::string(result);
  }
  // Fallback (mkdtemp failed — e.g. the base is uncreatable/unwritable): PID-
  // qualify the name exactly like the Windows branch above so even this degraded
  // path stays collision-safe between concurrent processes, and warn LOUDLY — a
  // silent runtime degradation is otherwise only discoverable by inspecting the
  // filesystem by hand (which is how BUG-134 was found).
  const int mkdErrno = errno;
  std::string fb = base + "/piet_native_fallback_" + std::to_string(::getpid());
  std::fprintf(stderr,
               "[piet_native] WARNING: could not create a unique scratch dir under "
               "'%s' (mkdtemp: %s); falling back to '%s'. Check the scratch base "
               "exists and is writable (Settings > Engine & scratch).\n",
               base.c_str(), std::strerror(mkdErrno), fb.c_str());
  return fb;
}
#endif

// PIET-TMP (BUG-092): the per-process scratch dir makeWorkdir() mkdtemp's is
// disposable — every result/data path resolves against session.cwd, never this
// scratch dir (see runDoFileHeadless' comment) — so it must NOT outlive the
// process, or each run leaks its whole scratch tree (a ~350MB hbin/csv pair per
// string-op accumulates under the in-session budget GC, but the DIRECTORY itself
// was never reaped) onto the user's disk. This RAII guard removes the ENTIRE
// session dir at scope exit. Declare it BEFORE the AppState it pairs with so it
// destructs AFTER (AppState::joinAll() has joined every run thread by then, so
// nothing writes scratch during removal). It only ever removes our own
// mkdtemp'd `piet_native_XXXXXX` subdir, never the user's PIET_WORKDIR/TMPDIR
// base. Set PIET_KEEP_SCRATCH to retain it (debugging a run's intermediate hbins).
struct ScratchDirGuard {
  std::string path;
  ScratchDirGuard() = default;
  explicit ScratchDirGuard(std::string p) : path(std::move(p)) {}
  ScratchDirGuard(const ScratchDirGuard&) = delete;
  ScratchDirGuard& operator=(const ScratchDirGuard&) = delete;
  ~ScratchDirGuard() {
    if (path.empty()) return;
    if (const char* keep = std::getenv("PIET_KEEP_SCRATCH"); keep && *keep) {
      std::fprintf(stderr, "[piet_native] PIET_KEEP_SCRATCH set — leaving scratch: %s\n",
                   path.c_str());
      return;
    }
    std::error_code ec;
    std::filesystem::remove_all(cecon::to_native_path(path), ec);  // best-effort
  }
};

// Loads the on-disk frontend tree into the shell (WP-W4 platform branch).
// macOS/Linux navigate a file:// URL — the vendored webview.h grants file://
// documents sibling-resource access (the @a9141d1 patch) so ES-module imports
// resolve. Windows/WebView2 (Chromium) rejects ES-module imports over file://
// the same way WebKit did, so we map a virtual host to the frontend directory
// and navigate to an https:// origin instead (SetVirtualHostNameToFolderMapping).
// piet.invalid = an RFC-2606 reserved TLD that can never resolve on a real
// network — the mapping intercepts it before DNS regardless, keeping the
// offline-first guarantee even if the mapping ever fails.
void navigateFrontend(piet::Shell& shell, const std::string& indexPath) {
#ifdef _WIN32
  std::string dir = indexPath;
  std::string file = "index.html";
  auto slash = indexPath.find_last_of("/\\");
  if (slash != std::string::npos) {
    dir = indexPath.substr(0, slash);
    file = indexPath.substr(slash + 1);
  }
  shell.setVirtualHostMapping("piet.invalid", dir);
  shell.navigate("https://piet.invalid/" + file);
#else
  shell.navigate("file://" + indexPath);
#endif
}

// 2.2.0 item D — apply the persisted engine/scratch settings.
//
// BUG-346: this used to run on the GUI launch path ONLY, and the comment here
// said why: "--do/--ui-smoke/--selftest stay deterministic on $PIET_WORKDIR/tmp".
// Determinism is the right instinct for a GATE and the wrong answer for a user's
// own run. Measured 2026-08-14: with scratch_path pointed at an external volume
// (767 GiB free) precisely so scratch would not sit on the boot disk, a
// `--do` run of the acceptance corpus under `parallel, workers(2)` put every
// worker's scratch on the boot volume instead, failed 99 writes with "disk full",
// and silently finished with 208 estimations out of 640.
//
// So the settings now apply to `--do` as well, and the two properties the old
// comment was protecting are kept explicitly instead of by exclusion:
//   * an explicit $PIET_WORKDIR still wins (it always did — the seed is
//     conditional on the variable being unset), which is how every gate and
//     harness pins its own scratch;
//   * `applyGates = false` lets the deterministic entry points (--selftest,
//     --ui-smoke) opt out entirely, so a gate's environment is still decided by
//     the gate and never by whatever the user last clicked in Settings.
piet::settings::Settings applyPersistedSettings() {
  piet::settings::Settings cfg = piet::settings::load();
  if (!cfg.scratchPath.empty()) {
    const char* envWd = std::getenv("PIET_WORKDIR");
    if (!envWd || !*envWd) {
#ifndef _WIN32
      setenv("PIET_WORKDIR", cfg.scratchPath.c_str(), 1);
#else
      _wputenv_s(L"PIET_WORKDIR", cecon::to_native_path(cfg.scratchPath).c_str());
#endif
    }
  }
  piet::settings::applyEngineBudget(cfg);
  return cfg;
}

void runRealFrontend() {
  const piet::settings::Settings cfg = applyPersistedSettings();

  std::string workdir = makeWorkdir();
  ScratchDirGuard scratchGuard{workdir};  // reaped at process exit (after app dtor joins threads)
  AppState app(workdir);
  // Seed cwd BEFORE any command runs (item 1, native-paths stabilization):
  // an unseeded session.cwd falls back to getcwd(), which under a Finder/
  // dock launch is "/" — see piet::defaultUserCwd()'s comment. 2.2.0 item D
  // lets the user pin a default working directory (used only if it exists —
  // else fall back to defaultUserCwd()). Mirror into eval.cwd the same way
  // runCd does (handlers_m3.cpp) so the macro layer's own cwd view (used by
  // e.g. `local d : pwd` idioms) agrees with session.cwd from the first
  // command onward.
  std::string seedCwd = piet::defaultUserCwd();
  if (!cfg.defaultWorkingDir.empty()) {
    std::error_code seedEc;
    if (std::filesystem::is_directory(cecon::to_native_path(cfg.defaultWorkingDir), seedEc)) {
      seedCwd = cfg.defaultWorkingDir;
    }
  }
  app.session.cwd = seedCwd;
  app.session.eval.cwd = *app.session.cwd;

  piet::Shell shell(/*debug=*/true);
  shell.setTitle("PIET");
  shell.setSize(1280, 860, /*resizable=*/true);
  shell.installBridge();
  bindAllRoutes(shell, app);
  // BUG-087/D9 (BETA Phase F): wire the _request() dialog-push hook now
  // that a real Shell exists — SessionState::hasRequestPostEvent() is the
  // headless-vs-windowed signal display_hardening.cpp's _request() handling
  // consults. `shell` is captured by reference: it outlives every use of
  // this callback the same way runDoInBackground's captured Shell* does
  // (see that function's doc comment) — every do-run worker thread that
  // could invoke this is joined (app.joinAll(), below) before `shell` goes
  // out of scope at the end of this function.
  app.session.setRequestPostEvent([&shell](const std::string& json) { shell.postEvent(json); });

  std::string indexPath = resolveFrontendIndexPath();
  // file:// (NOT a data: URI): the real frontend uses ES module relative
  // imports (`import {...} from './js/api.js'` etc.) which need a real
  // filesystem base URL to resolve — a data: URI has an opaque origin and
  // cannot resolve relative module specifiers. This is the actual M4b
  // transport: the webview loads the SAME on-disk frontend tree the old
  // Python-backend era served over HTTP, just via file:// instead of
  // http://127.0.0.1:7331. M5 GA: resolveFrontendIndexPath() prefers a
  // bundled Resources/frontend/index.html when running from inside a
  // packaged .app, so the SAME file:// mechanism now also serves the
  // shipped bundle's copy — no code path divergence between dev and GA.
  // (Windows/WebView2 serves the same tree over a mapped virtual host — see
  // navigateFrontend.)
  navigateFrontend(shell, indexPath);

  std::printf("[piet_native] window open (real frontend, %s) — close it to exit.\n", indexPath.c_str());
  shell.run();

  // Owned join point (thread-lifetime discipline): every background run
  // thread is joined here, AFTER shell.run() returns (window closed) but
  // BEFORE shell's destructor tears down the webview/Shell — so no worker
  // thread's postEvent()/resolveInvoke() call can ever race a torn-down
  // Shell. If a run was genuinely still in flight when the window closed,
  // this blocks briefly until the do-executor reaches its next
  // cancel-check point (session.progressCancelRequested(), checked between
  // every top-level do-file item) — a bounded wait, not a hang, since we
  // also request cancellation first.
  app.session.progressRequestCancel();
  app.joinAll();
}

void runDemoPage() {
  piet::Shell shell(/*debug=*/true);
  shell.setTitle("PIET 2.0 — M0 walking skeleton (--demo)");
  shell.setSize(720, 640, /*resizable=*/true);
  shell.installBridge();

  shell.bindRoute("echo", piet::routes::echo);
  shell.bindRoute("cobra.version", piet::routes::cobraVersion);
  shell.bindRoute("progress.start", [&shell](const std::string&) -> piet::BridgeResult {
    std::thread([&shell]() {
      piet::runSyntheticProgress([&shell](const std::string& tickJson) { shell.postEvent(tickJson); });
    }).detach();
    return piet::BridgeResult::success(R"({"started":true})");
  });

  std::string html = readFileOrDie(repoRelative("/piet/native/demo/index.html"));
  std::string dataUri = "data:text/html;base64," + piet::base64Encode(html);
  shell.navigate(dataUri);

  std::printf("[piet_native] window open (M0 demo) — close it to exit.\n");
  shell.run();
}

// --do <file> [args...] — headless acceptance runner (Eval / GA acceptance).
// Runs a full .do file through the SAME native do-executor the live UI's Run
// button uses (doexec::executeDo -> executeDoSource, progressOwner=true), with
// NO window and NO worker thread — synchronous, deterministic. Emits one JSON
// object {blocks, stopped_at_line, stopped_at_reason, resolved_path} to stdout
// (redirect to a file; blocks can be large), plus a trailing `counters` key
// ONLY when $PIET_EMIT_COUNTERS is set non-empty (see the emit site below).
// session.cwd is seeded to the
// INVOKING SHELL's own process cwd (below) so its relative paths (`use
// "foo.dta"`, `cd`, tempfiles) resolve exactly as a real run would — a
// kitchensink run invoked as `piet_native --do debug_all.do`
// from the repo root keeps resolving `use "fatalities.csv"`-style relative
// paths against the repo root, not the .do file's own directory. Exit 0 iff
// the file ran to completion with no stopped_at_reason, OR stopped on a
// clean Stata `exit`/`exit, clear` (BUG-139 — `exit` halts via the same
// untagged-error channel a real failure uses, but it isn't one; a bare
// `exit`/`exit, clear` returns 0, `exit n` returns n). Every other
// stopped_at_reason still returns 1. This is the FAST + FAITHFUL GA-target
// path: it exercises the shipped C++ core (in-memory strmutate, no per-op
// dataset round-trip), unlike the legacy Python-backend accept_run.py.
int runDoFileHeadless(const std::string& filepath, const std::vector<std::string>& args) {
  // Scratch workdir = an internal mkdtemp (makeWorkdir honours $PIET_WORKDIR,
  // else /tmp) — the SAME choice the real .app makes (runRealFrontend), NOT
  // the invoking shell's cwd. Data/result paths resolve against session.cwd
  // (seeded just below) + the .do file's own `cd`, independent of this
  // scratch dir; the per-mutation hbin round-trips (loaded.hbin +
  // gen_*/egen_*/...) therefore land on fast local storage instead of
  // wherever the script happens to sit.
  // BUG-346 — the user's own settings apply here too. A worker parked under a
  // parent scratch dir on a cramped boot volume is how a corpus run loses two
  // thirds of its estimations to "disk full" while reporting success.
  (void)applyPersistedSettings();

  std::string workdir = makeWorkdir();
  std::fprintf(stderr, "[piet_native] --do scratch workdir: %s\n", workdir.c_str());

  ScratchDirGuard scratchGuard{workdir};  // reaped at process exit (PIET-TMP/BUG-092)
  AppState app(workdir);
  // Seed cwd = getcwd() of the invoking shell (NOT defaultUserCwd() —
  // --do is a CLI/CI entry point, not a Finder launch, so the caller's own
  // cwd is the right base; keeps kitchensink runs invoked from the repo
  // root working exactly as they did before cwd was ever seeded).
  {
    std::error_code ec;
    std::filesystem::path invokerCwd = std::filesystem::current_path(ec);
    if (!ec) {
      app.session.cwd = invokerCwd.u8string();
      app.session.eval.cwd = *app.session.cwd;
    }
  }
  nlohmann::ordered_json out = nlohmann::ordered_json::object();
  // BUG-139: set only when the run stopped on a clean `exit`/`exit N`
  // (never for a real failure) — see doexec.h's stoppedAtExitCode doc.
  std::optional<int> exitRequestedCode;
  try {
    piet::doexec::ExecuteFileResult fr =
        piet::doexec::executeDo(filepath, app.session, piet::dispatch, args);
    out["blocks"] = fr.result.blocks;
    if (fr.result.stoppedAtLine.has_value()) out["stopped_at_line"] = *fr.result.stoppedAtLine;
    if (fr.result.stoppedAtReason.has_value()) out["stopped_at_reason"] = *fr.result.stoppedAtReason;
    out["resolved_path"] = fr.resolvedPath;
    exitRequestedCode = fr.result.stoppedAtExitCode;
  } catch (const std::exception& e) {
    out["blocks"] = nlohmann::ordered_json::array();
    nlohmann::ordered_json errBlock;
    errBlock["type"] = "error";
    errBlock["code"] = "engine_error";
    errBlock["message"] = std::string("run failed: ") + e.what();
    out["blocks"].push_back(errBlock);
    out["stopped_at_reason"] = std::string("cpp_exception: ") + e.what();
  }
  // BUG-350: say plainly, on STDERR, that this run hid something. A plain
  // `capture` is supposed to suppress the block — that is Stata — but a run that
  // swallowed a disk-full and then reported 47/47 artifacts and exit 0 is the
  // count-passes-while-output-degrades family (BUG-340's sibling), and the only
  // trace was one line inside a 23-minute log.
  //
  // stderr, not the JSON: stdout is compared byte-for-byte by 163 freeze
  // fixtures, and a new key there would diff every one of them for a message
  // that concerns the OPERATOR, not the transcript. A clean run prints nothing.
  if (piet::counters::swallowedErrors() > 0) {
    std::fprintf(stderr,
                 "[piet_native] NOTE: %lld error(s) were captured and hidden by `capture` during "
                 "this run — the run's own output cannot show them. First: %s\n",
                 static_cast<long long>(piet::counters::swallowedErrors()),
                 piet::counters::firstSwallowedError().c_str());
  }
  // OVERHEAD-ZERO S5 (per-command floor gate): the mechanism counters the gate
  // reads — "did this verb re-open the dataset / rebuild its card at all" —
  // are process-wide statics whose only consumer was selftest.cpp, i.e. not
  // reachable from outside this process. Surface them, but ONLY under an
  // explicit opt-in: an unconditional key would change --do stdout for EVERY
  // run, diffing all captured freeze fixtures at once (freeze_matrix.py
  // --check). Appended last, after the try/catch, so key order is unchanged
  // and a run that threw still reports its counters.
  // T6 PIET-SIDE-SPLIT: per-verb {count, wall_ms, engine_ms} for one --do run.
  // Its OWN env gate (not PIET_EMIT_COUNTERS) so no existing consumer of the
  // counters key — floor_gate, freeze captures, the campaign's own A/B arms —
  // sees a single new byte. piet_ms = wall_ms - engine_ms is the number the
  // campaign was missing: the ~12 minutes that were not the engine.
  if (const char* et = std::getenv("PIET_EMIT_TIMING"); et && *et) {
    nlohmann::ordered_json timing = nlohmann::ordered_json::object();
    for (const auto& [verb, v] : piet::counters::verbTimings()) {
      nlohmann::ordered_json row = nlohmann::ordered_json::object();
      row["n"] = v.count;
      row["wall_ms"] = v.wallNs / 1e6;
      row["engine_ms"] = v.engineNs / 1e6;
      row["piet_ms"] = (v.wallNs - v.engineNs) / 1e6;
      timing[verb] = row;
    }
    out["timing"] = timing;
    out["engine_wait_ms_total"] = piet::counters::engineWaitNs() / 1e6;
    // T7: is the row evaluator's SLOT-COMPILED fast path actually engaging?
    // A nonzero count means rows went through the legacy per-row namespace
    // rebind instead (strmutate.cpp) — the only signal that distinguishes the
    // two, since both produce identical answers. Same report-only env gate.
    out["evalrows_map_writes"] = piet::strmutate::evalRowsMapWriteCount();
    // T7b ROWLOOP-VECTOR-SCOPING: the PIET-side half of the string gen/replace
    // path, split four ways (counters.h StrmutPhases). §T7 left exactly this
    // open — "compiled row loop vs the 108k-row CSV hand-off, which dominates
    // the ~45 ms/call" — and could not answer it because piet_ms was one lump.
    // Appended last on the same env gate, so no already-captured fixture and no
    // existing timing consumer sees a byte.
    {
      const auto& sp = piet::counters::strmutPhases();
      nlohmann::ordered_json ph = nlohmann::ordered_json::object();
      ph["evalrows_ms"] = sp.evalrowsNs / 1e6;
      ph["outbuild_ms"] = sp.outbuildNs / 1e6;
      ph["csvwrite_ms"] = sp.csvwriteNs / 1e6;
      ph["reload_ms"] = sp.reloadNs / 1e6;
      // Partition of reload_ms — three unrelated costs with three unrelated
      // fixes; the first probe put ~55% of the PIET half in reload alone.
      ph["rl_load_ms"] = sp.rlLoadNs / 1e6;
      ph["rl_nmiss_ms"] = sp.rlNmissNs / 1e6;
      ph["rl_varlist_ms"] = sp.rlVarlistNs / 1e6;
      // ROWLOOP-RLLOAD-SUBSPLIT: partition of rl_load_ms itself — the dominant
      // ~53% term, opened. Three named owners plus the previous frame's munmap;
      // rl_load - (these four) is the residual completeness check.
      ph["rll_materialize_ms"] = sp.rllMaterializeNs / 1e6;
      ph["rll_open_ms"] = sp.rllOpenNs / 1e6;
      ph["rll_reclaim_ms"] = sp.rllReclaimNs / 1e6;
      ph["rll_close_ms"] = sp.rllCloseNs / 1e6;
      ph["rll_materialize_calls"] = sp.rllMaterializeCalls;
      ph["rll_open_calls"] = sp.rllOpenCalls;
      ph["writenew_calls"] = sp.calls;
      ph["evalrows_calls"] = sp.evalrowsCalls;
      ph["rows"] = sp.rows;
      out["strmutate_phases"] = ph;
    }
    // ATTR-20260812. `timing` above mixes nesting levels (a loop header's row
    // and its body's rows are both present, and the header's wall INCLUDES the
    // body), which is why p0_landings §T6 had to write "행 합산 금지" beside it.
    // These two keys separate the levels instead of warning about them, and are
    // appended LAST on higher levels of the same gate so nothing already
    // captured moves.
    const int __lvl = piet::counters::timingLevel();
    if (__lvl >= 2) {
      // Depth-1 items, in execution order. THESE SUM: each root item appears
      // once, its wall inclusive of whatever it ran. `line` is the item index
      // within the root do-source (1-based, brace blocks count as one item).
      nlohmann::ordered_json items = nlohmann::ordered_json::array();
      for (const auto& r : piet::counters::itemRecords()) {
        nlohmann::ordered_json row = nlohmann::ordered_json::object();
        row["line"] = r.line;
        row["verb"] = r.verb;
        row["text"] = r.text;
        row["wall_ms"] = r.wallNs / 1e6;
        row["engine_ms"] = r.engineNs / 1e6;
        items.push_back(row);
      }
      out["timing_items"] = items;
    }
    if (__lvl >= 3) {
      // Depth>=2 items — inside loop bodies, program bodies and replayed do
      // files — verb-summed. Never add these to timing_items: they are the same
      // seconds, told one level down.
      nlohmann::ordered_json nested = nlohmann::ordered_json::object();
      for (const auto& [verb, v] : piet::counters::nestedTimings()) {
        nlohmann::ordered_json row = nlohmann::ordered_json::object();
        row["n"] = v.count;
        row["wall_ms"] = v.wallNs / 1e6;
        row["engine_ms"] = v.engineNs / 1e6;
        row["piet_ms"] = (v.wallNs - v.engineNs) / 1e6;
        nested[verb] = row;
      }
      out["timing_nested"] = nested;
      // Exclusive (self) time per verb: this item's wall minus its children's.
      // The one view whose rows SUM to the run, and the only one that
      // decomposes a corpus whose entire top level is a single program call.
      nlohmann::ordered_json excl = nlohmann::ordered_json::object();
      for (const auto& [verb, v] : piet::counters::exclusiveTimings()) {
        nlohmann::ordered_json row = nlohmann::ordered_json::object();
        row["n"] = v.count;
        row["self_wall_ms"] = v.wallNs / 1e6;
        row["self_engine_ms"] = v.engineNs / 1e6;
        row["self_piet_ms"] = (v.wallNs - v.engineNs) / 1e6;
        excl[verb] = row;
      }
      out["timing_exclusive"] = excl;
    }
    // INSPECT-2608, level 4: the per-FORM table that pairs wall with BYTES.
    // Every row is EXCLUSIVE (self) — the child-bucket subtraction the level-3
    // block above uses — so the rows SUM to the run. `sums_to_run` announces
    // that property rather than leaving a reader to guess which of the timing
    // views may be added up; the completeness invariant is asserted in selftest.
    if (__lvl >= 4) {
      const auto& acc = piet::counters::foldableAccumulators();
      nlohmann::ordered_json rows = nlohmann::ordered_json::object();
      for (const auto& [form, r] : piet::counters::percmdRows()) {
        nlohmann::ordered_json row = nlohmann::ordered_json::object();
        row["n"] = r.n;
        row["self_wall_ms"] = r.selfWallNs / 1e6;
        row["self_engine_ms"] = r.selfEngineNs / 1e6;
        for (size_t i = 0; i < acc.size(); ++i)
          row[acc[i].first] = i < r.self.size() ? r.self[i] : 0;
        rows[form] = row;
      }
      nlohmann::ordered_json pc = nlohmann::ordered_json::object();
      pc["rows"] = rows;
      pc["sums_to_run"] = true;
      out["percmd"] = pc;
    }
  }
  // BUG-381: the scratch census, taken HERE — the last moment the workdir still
  // exists (ScratchDirGuard reaps it on the way out). Both peak runs put their
  // PEAK sample at the very END of the run, so a census at exit is a census of
  // the peak's composition. Its own env gate, like the two above it, so no
  // captured freeze fixture and no existing counters consumer sees a byte.
  if (const char* sc = std::getenv("PIET_SCRATCH_CENSUS"); sc && *sc) {
    out["scratch_census"] = nlohmann::ordered_json::parse(app.session.scratchCensusJson());
  }
  if (const char* emit = std::getenv("PIET_EMIT_COUNTERS"); emit && *emit) {
    nlohmann::ordered_json counters = nlohmann::ordered_json::object();
    counters["datastore_constructs"] = piet::Datastore::constructCount();
    counters["card_folds"] = piet::SessionState::cardFoldCount();
    counters["postest_hits"] = piet::CobraSession::postestHitCount();
    counters["resident_live_count"] = piet::SessionState::residentLiveCount();
    // FLOOR metric ① (A1-METRICS-RATIFICATION §1.5/§7) — the byte half. Three
    // more deterministic integers on the SAME opt-in seam, added last so key
    // order is append-only and every already-captured freeze fixture stays
    // byte-identical (the emit is env-gated, so with the var absent nothing
    // here reaches stdout at all).
    //   touched_bytes  — payload bytes the resident plane moved (counters.h's
    //                    four site classes). Numerator of amp = touched/semantic.
    //   snapshot_count — direction `max`: snapshot is efficient PER OP, so its
    //                    defect is the count, not the ratio (A1 §1.3).
    //   setcol_bytes   — the `semantic_zero` class: a ratio against a zero
    //                    denominator is undefined, not infinite, so setcol-shaped
    //                    verbs leave ① and get an ABSOLUTE byte budget here.
    counters["touched_bytes"] = piet::counters::touchedBytes();
    counters["snapshot_count"] = piet::counters::snapshotCount();
    counters["setcol_bytes"] = piet::counters::setcolBytes();
    // FLOOR metric ② DENOMINATOR (A1-METRICS §2.2 · B2 NAMED-OWED ③ ·
    // PHASE-B-DECISION-MEMO §6-3) — the honest "live named artifacts" byte
    // count a2_seqbench's ws ratio needs, replacing its input-.dta-size
    // stand-in. Report-only (NO floors.json budget — Eval pen): a SNAPSHOT of
    // end-of-run session state, appended last so key order stays append-only
    // and, being inside this env-gated block, every already-captured freeze
    // fixture stays byte-identical.
    // FIX-C4COUNTER-SAMPLERSPAN/BUG-142: through the NEUTRAL wrapper, not bare
    // liveNamedBytes(). This walk may materialise a dirty head (a real
    // frameSnapshot — desirable, the value stays accurate), and several
    // counters above (datastore_constructs/touched_bytes/snapshot_count) are
    // captured BEFORE this line while snapshot_bytes is captured AFTER it —
    // a bare walk half-records its own closure into whichever keys are read
    // later (measured: snapshot_bytes +67968 with snapshot_count 0, and a
    // trace line attributed to the last dispatched verb). The neutral wrapper
    // makes capture order irrelevant and keeps trace==snapshot_count exact.
    const int64_t endNamedBytes = app.session.liveNamedBytesEndSnapshot();
    counters["live_named_bytes"] = endNamedBytes;
    // FLOOR metric ② DENOMINATOR — the RATIFIED peak-vs-peak value
    // (PHASE-B-DECISION-MEMO §6-3 ADDENDUM, 2026-07-23): the running high-water
    // max of liveNamedBytes() over the run's top-level items (doexec.cpp),
    // folded with the end snapshot so a run that never dropped its frame still
    // reports its own end state as the peak. THIS — not `live_named_bytes`
    // above — is a2_seqbench's ② denominator: a corpus that ends `exit, clear`
    // (a2_mini) drops the frame before this emit, so the end snapshot
    // under-counts while peak RSS occurred with the frame resident. Appended
    // last (append-only key order → captured freeze fixtures stay byte-identical
    // env-off). Report-only (NO floors.json budget — Eval pen).
    piet::counters::updateLiveNamedBytesPeak(endNamedBytes);
    counters["live_named_bytes_peak"] = piet::counters::liveNamedBytesPeak();
    // C4-L1 + PP-L1 (C4-RESIDENT-READ-SCOPE-brief §6 · PINGPONG-SCOPE-brief
    // §5) — four more deterministic integers, appended last on the same
    // env-gated seam (append-only key order; env-off stdout unchanged, so all
    // captured freeze fixtures stay byte-identical).
    //   snapshot_bytes   — byte half of the SNAPSHOT class: settles the §2.2
    //                      upper bound (95 × peak-frame) into a measured share
    //                      of touched_bytes.
    //   use_disk_loads / use_virtual_hits / dta_converts — runUse branch split:
    //                      closes the PINGPONG §1 disk-vs-virtual contradiction
    //                      and prices K1 (use conversion/load cache) honestly.
    //   use_stash_hits    — PP-L2B / K1b: re-`use`s that restored the worker's
    //                      resident stash (no disk open at all). Complements
    //                      use_disk_loads: the win is disk-loads falling as this
    //                      rises across a sectioning corpus's repeat `use`s.
    counters["snapshot_bytes"] = piet::counters::snapshotBytes();
    counters["use_disk_loads"] = piet::counters::useDiskLoads();
    counters["use_virtual_hits"] = piet::counters::useVirtualHits();
    counters["dta_converts"] = piet::counters::dtaConverts();
    counters["use_stash_hits"] = piet::counters::useStashHits();
    // W0 (P2PERF-WAVE-PLAN §W0) — the ENGINE ROUND-TRIP ledger, split by request
    // shape. Appended last, same env-gated seam (append-only key order → every
    // captured freeze fixture stays byte-identical with the var absent).
    //
    // The campaign's opening hypothesis was that by-group work paid a process
    // SPAWN per group; the code says otherwise (a persistent worker is the
    // default plane). These keys make that checkable instead of argued: a
    // stateless round trip re-reads --data from disk and writes --out, a
    // frame_run touches no file at all, and `one_shot_spawns` is 0 unless the
    // worker plane was unavailable. A rung that claims to remove engine work
    // must move one of these integers.
    nlohmann::ordered_json ereq = nlohmann::ordered_json::object();
    ereq["stateless"] = piet::counters::engineReqStateless();
    ereq["frame_use"] = piet::counters::engineReqFrameUse();
    ereq["frame_run"] = piet::counters::engineReqFrameRun();
    ereq["snapshot"] = piet::counters::engineReqSnapshot();
    ereq["postest"] = piet::counters::engineReqPostest();
    ereq["other"] = piet::counters::engineReqOther();
    ereq["total"] = piet::counters::engineReqTotal();
    counters["engine_requests"] = ereq;
    counters["one_shot_spawns"] = piet::counters::oneShotSpawns();
    counters["serve_spawns"] = piet::counters::serveSpawns();
    // Self-reload witness (counters.h) — a `use` that re-materialized off disk
    // a file THIS session had just written with `save`. Appended last, same
    // env-gated seam (append-only key order → every captured freeze fixture
    // stays byte-identical with the var absent).
    counters["datastore_reloads"] = piet::counters::datastoreReloads();
    // The engine's own mechanism counters, fetched from the live worker with one
    // observational request (see CobraSession::frameCounters). Absent — not
    // zero — when this run never spawned a worker or the engine predates the op:
    // an all-zero block would read as "no full rewrites happened", which is a
    // different and much more flattering claim than "not measured".
    if (piet::CobraSession* cs = app.session.cobraSessionIfLive()) {
      piet::CobraRunResult ec = cs->frameCounters();
      if (ec.ok && !ec.stdout_text.empty()) {
        try {
          counters["engine"] = nlohmann::ordered_json::parse(ec.stdout_text);
        } catch (const nlohmann::json::exception&) {
          counters["engine"] = "unparseable";
        }
      } else {
        counters["engine"] = "unavailable";
      }
    } else {
      counters["engine"] = "no-worker";
    }
    // ATTR-20260812 coverage fingerprint. The 08-12 bar-grade record had to
    // grep an append-mode log that held every run since July for these two, and
    // the graph number could not be re-derived from it afterwards. Counted per
    // run at the emission point instead. Appended last, after `engine`, so no
    // captured counters block moves.
    counters["graph_emissions"] = piet::counters::graphEmissions();
    counters["error_blocks"] = piet::counters::errorBlocks();
    out["counters"] = counters;
  }
  // BUG-1372: a split UTF-8 sequence from a byte-based string function is
  // written as U+FFFD, as the GUI path sends it (runDoInBackground above); so
  // is a byte that is not UTF-8 (`type` of a Latin-1 file, BUG-1131).
  std::printf("%s\n",
              out.dump(-1, ' ', false, nlohmann::ordered_json::error_handler_t::replace).c_str());
  // BUG-124 (silent-death mode): with stdout redirected to a file on a FULL
  // disk, the buffered transcript is lost at exit-time flush and the run ends
  // exit 1 with an EMPTY stdout and no diagnostic anywhere (observed on the
  // fair-run: 8.4s, empty transcript, only the workdir line on stderr). Flush
  // NOW, and if the transcript could not be written, say so on stderr (tiny,
  // unbuffered — lands even when the big transcript can't) and fail the run:
  // a lost transcript must never look like a clean exit to CI.
  if (std::fflush(stdout) != 0 || std::ferror(stdout)) {
    std::fprintf(stderr,
                 "[piet_native] FATAL: transcript write to stdout failed "
                 "(disk full or I/O error) — run output lost\n");
    return 1;
  }
  // BUG-139: a clean `exit`/`exit N` halts via the same untagged-error
  // channel a real failure uses — honor the requested code instead of
  // collapsing every halt to rc 1.
  if (exitRequestedCode.has_value()) return *exitRequestedCode;
  return out.contains("stopped_at_reason") ? 1 : 0;
}

// --- --ui-smoke (2.0.2, MI-138 WP-1): packaged-app UI journey gate ----------
//
// Drives a scripted user journey through the REAL window + REAL WKWebView +
// REAL frontend SPA + REAL bridge + REAL cobra subprocess — the exact stack a
// user launch exercises (Constitution Part 1 Verification law: "renders" !=
// "works"; headless PASS never declares a UI release GREEN). There are NO
// mocks anywhere in this mode: the window that opens IS the product window
// wired by the SAME bindAllRoutes()/AppState code path runRealFrontend()
// uses, so the mock-fidelity law is satisfied vacuously.
//
// Journey (driver JS injected via initJs AFTER the bridge shims, so it runs
// in the page at document-start with window.pietNative already installed):
//   1. bridge_echo         — window.pietNative.request('echo') round-trip
//                            through the real shim + real WKWebView bind
//   2. kernel_dot_green    — the SPA booted and its own ping() varlist
//                            round-trip turned #kernel-dot 'dot-ok'; the
//                            mock banner must be hidden (a mock-mode
//                            fallback is a FAIL, never a pass)
//   3. varlist_populated   — submit `use "<fixture>"` through the real
//                            #cmd-input + #btn-run click; #var-list-wrap
//                            .var-row rows must appear
//   4. estimation_rendered — submit `regress fatal beertax unemp`; a
//                            rendered .block-estimation .coeftable naming
//                            the regressor must appear in #console-body,
//                            with no .block-error present
//
// Steps 5-9 (stabilization campaign delta — verification law: gates follow
// the delta, so paste policy / Run.do / overlay mechanics that this campaign
// actually changed must be exercised, not just the M4b-era boot journey
// above):
//   5. paste_routing       — a synthetic multi-line `paste` DOM event on
//                            #cmd-input (piet-paste-autorun OFF, the
//                            default) must route to the do-editor
//                            (#doedit-overlay opens, #doedit-text gets the
//                            pasted lines verbatim, #cmd-input clears) and
//                            must NOT auto-run (no new console block within
//                            a settle window) — design decision 4.
//   6. editor_first_run    — window.pietNative.request('file.read', {path})
//                            against a tiny fixture .do this driver writes
//                            into its own scratch workdir; the returned
//                            source is loaded into #doedit-text (the
//                            externally-observable effect of
//                            onRunDo()/readDoFile(), whose own trigger is a
//                            native Finder picker unreachable headlessly),
//                            #doedit-run is clicked, and an honest
//                            .block-table (describe) result must land in
//                            #console-body with no .block-error.
//   7. overlay_exclusivity — opening the graphics overlay then the do-editor
//                            via their toolbar anchors must never leave more
//                            than one of the four console overlays visible
//                            at once (design decision 5 / A2-05's OVERLAYS
//                            registry); Escape then closes the one that's
//                            open.
//   8. transition_sanity   — opening an overlay must reach a REAL rendered
//                            end-state (getComputedStyle opacity '1' +
//                            visibility 'visible'), not just a class flip
//                            with no visible effect — polled, not a single
//                            sleep-then-check. Runs under the frontend's own
//                            reduced-motion configuration (--dur-* zeroed
//                            inline, mirroring piet.css §1's
//                            prefers-reduced-motion block) because WKWebView
//                            suspends the CSS transition clock while the
//                            window is occluded — asserting an ANIMATED
//                            computed value was the only occlusion-sensitive
//                            assertion in this journey and produced a false
//                            RED on a covered window (2026-07-09). Set
//                            PIET_UI_SMOKE_OCCLUDE_STEP8=1 to make this step
//                            wait for real occlusion (document.hidden) before
//                            running — the deterministic covered-window
//                            verification protocol.
//   9. busy_lock            — submitting a command must set the busy-lock
//                            classes (File▸Open toolbar anchor,
//                            #var-list-wrap) SYNCHRONOUSLY (checked with no
//                            await/setTimeout between the click and the
//                            assertion, since setBusy(true) runs before
//                            runCommand()'s first await) and release them
//                            once the run settles (A1-01/A1-02 regression).
//
// Verdict channel: each step reports back over the REAL bridge via a
// 'smoke.report' route registered ONLY in this mode (runRealFrontend never
// binds it, the frontend never calls it — it is a test-harness binding, NOT
// part of the Boundary-B contract surface; see CONTRACT.md which documents
// the product route set). Detection property for the a9141d1 double-parse
// class: the JS->C++ invoke leg of the bridge still delivers even when the
// C++->JS resolve leg is broken, so a broken-bridge run still lands a
// precise FAIL report; a page that never executes anything at all is caught
// by the 90s C++ watchdog (Shell::terminateFromAnyThread). Exit 0 iff the
// final report is a pass; a JSON verdict file (file-backed verdict, charter
// #2) is written to $PIET_UI_SMOKE_OUT (default
// /tmp/piet_ui_smoke_verdict.json).
//
// Run it against the PACKAGED artifact (the actual gate):
//   dist/PIET.app/Contents/MacOS/piet_native --ui-smoke
// (bundle-first resolution picks up Resources/frontend + Resources/cobra —
// same order the product launch uses). Running the dev build binary smokes
// the dev tree instead. The fixture dataset defaults to the SHIPPED sample
// (bundled Resources/sample/fatalities.csv, or the worktree's
// piet/packaging/sample/fatalities.csv for dev runs; override:
// $PIET_UI_SMOKE_DATA).
struct SmokeState {
  std::mutex m;
  std::condition_variable cv;
  bool finalReceived = false;
  bool finalPass = false;
  std::vector<nlohmann::ordered_json> reports;
};

std::string smokeDriverJs(const std::string& dataPath, const std::string& doFixturePath,
                          const std::string& saveTargetPath, bool occludeStep8) {
  // nlohmann dump() of a string produces a double-quoted, escaped literal —
  // valid as a JS string literal (JSON is a JS subset here).
  std::string dataLit = nlohmann::json(dataPath).dump();
  std::string doFixtureLit = nlohmann::json(doFixturePath).dump();
  std::string saveTargetLit = nlohmann::json(saveTargetPath).dump();
  std::string js = R"JS(
    (function () {
      var DATA_PATH = )JS";
  js += dataLit;
  js += R"JS(;
      var DO_FIXTURE_PATH = )JS";
  js += doFixtureLit;
  js += R"JS(;
      var SAVE_TARGET_PATH = )JS";
  js += saveTargetLit;
  js += R"JS(;
      var OCCLUDE_STEP8 = )JS";
  js += occludeStep8 ? "true" : "false";
  js += R"JS(;
      var STEP_TIMEOUT_MS = 15000;
      var t0 = Date.now();
      // BUG-197 hardening: the boot census below (report(true, false,
      // 'boot_prereq_dom_census', ...)) and the journey's own top-level
      // .catch() can both try to send a final report for the SAME run (the
      // census throws to unwind the chain after already reporting) — without
      // this guard the second, generic 'journey' report would overwrite the
      // C++ side's smoke.finalPass with a redundant duplicate. Only the
      // FIRST final report counts; every later one is a silent no-op.
      var finalReported = false;

      function report(isFinal, pass, step, detail) {
        if (isFinal) {
          if (finalReported) return;
          finalReported = true;
        }
        var payload = { final: isFinal, pass: pass, step: step,
                        detail: detail || '', elapsed_ms: Date.now() - t0 };
        try {
          // The invoke leg delivers to C++ even if the resolve leg is broken
          // (see the C++ comment above runUiSmoke) — swallow the response.
          window.pietNative.request('smoke.report', payload).catch(function () {});
        } catch (e) { /* bridge fully absent — the C++ watchdog fires */ }
      }

      // --- BUG-197: boot-time injected-content census -----------------------
      // Runs before step 1, on the theory that a gate whose FIRST recorded
      // step already inspects its own execution environment can tell a future
      // reader which regime judged the run (see the mode line in the FINAL
      // report below). Two checks, both boot-time DOM snapshots — this is
      // deliberately NOT a live MutationObserver: the goal is "did anything
      // land in this document before we started", not "watch it forever",
      // matching the wave brief's "enumerate injected content at boot".
      //
      // 1. Script census: piet/frontend/index.html ships exactly two inline
      //    <script> elements (the pre-paint mode/theme FOUC guard, and the
      //    type="module" boot script) and ships them with NO `src` attribute
      //    — everything else the app loads (js/app.js etc.) arrives via
      //    `import`, which does not create a `<script>` DOM node. The C++
      //    side's own shims (installBridge, this very driver) are injected as
      //    WKUserScripts, which run in the page's JS context WITHOUT ever
      //    becoming document.scripts entries — so this census cannot see (or
      //    false-positive on) its own harness. Any script that IS a DOM node
      //    beyond those two, or that carries a `src`, is content this driver
      //    did not ship.
      // 2. Grammarly marker census: the internal bug ledger BUG-197 /
      //    piet/README.md's "Known issues" section confirm Grammarly Desktop
      //    as the one injector this fleet has actually caught mutating this
      //    DOM. Its confirmed markers (data-gramm*, the desktop-integration
      //    host element, its shadow-root host attribute) are checked
      //    explicitly as a named tripwire, separate from the generic script
      //    census, because they land on EXISTING elements (textarea/input)
      //    rather than as new <script> nodes — the two checks catch different
      //    injection shapes and neither subsumes the other.
      var EXPECTED_SHIPPED_SCRIPT_COUNT = 2;
      var GRAMMARLY_MARKER_SELECTORS = [
        '[data-gramm]', '[data-gramm_editor]', '[data-enable-grammarly]',
        '#grammarly-desktop-integration', '[data-grammarly-shadow-root]'
      ];
      function domCensus() {
        var findings = [];
        var scripts = document.querySelectorAll('script');
        for (var i = 0; i < scripts.length; i++) {
          if (scripts[i].src) {
            findings.push('foreign <script src="' + scripts[i].src + '">');
          }
        }
        if (scripts.length > EXPECTED_SHIPPED_SCRIPT_COUNT) {
          findings.push('script element count ' + scripts.length + ' > shipped ' +
                        EXPECTED_SHIPPED_SCRIPT_COUNT);
        }
        GRAMMARLY_MARKER_SELECTORS.forEach(function (sel) {
          try {
            if (document.querySelector(sel)) findings.push('grammarly marker present: ' + sel);
          } catch (e) { /* invalid selector on an old WebKit — ignore that one check */ }
        });
        return findings;
      }

      function waitFor(desc, pred, timeoutMs) {
        return new Promise(function (resolve, reject) {
          var deadline = Date.now() + timeoutMs;
          (function poll() {
            var v = null;
            try { v = pred(); } catch (e) { /* keep polling */ }
            if (v) return resolve(v);
            if (Date.now() > deadline) return reject(new Error('timeout waiting for ' + desc));
            setTimeout(poll, 100);
          })();
        });
      }

      // s5's negative assertion ("no do_string executed"): poll a settle
      // window watching for a NEW console block rather than a single
      // sleep-then-check — a routing regression that auto-runs the pasted
      // block would append one almost immediately, so this fails fast
      // instead of only at the deadline.
      function waitNoNewBlock(desc, baselineCount, windowMs) {
        return new Promise(function (resolve, reject) {
          var deadline = Date.now() + windowMs;
          (function poll() {
            var n = document.querySelectorAll('#console-body .block').length;
            if (n > baselineCount) {
              return reject(new Error('unexpected new console block appeared while waiting for ' +
                                       desc + ' (count ' + baselineCount + ' -> ' + n + ')'));
            }
            if (Date.now() > deadline) return resolve();
            setTimeout(poll, 100);
          })();
        });
      }

      function submit(cmd) {
        var input = document.getElementById('cmd-input');
        var btn = document.getElementById('btn-run');
        if (!input || !btn) throw new Error('cmd-input / btn-run not in DOM');
        input.value = cmd;
        btn.click();
      }

      function domReady() {
        return new Promise(function (resolve) {
          if (document.readyState !== 'loading') return resolve();
          document.addEventListener('DOMContentLoaded', function () { resolve(); });
        });
      }

      // Console overlays: #browser-overlay is the sole exception that uses
      // the `hidden` attribute instead of `.open` (grid.js owns it — see
      // index.html's comment on #browser-overlay); the other three
      // (doedit/graphics/about) use `.open` via app.js's OVERLAYS registry.
      var OVERLAY_IDS = ['browser-overlay', 'doedit-overlay', 'graphics-overlay', 'about-overlay'];
      function overlayVisible(id) {
        var el = document.getElementById(id);
        if (!el) return false;
        return id === 'browser-overlay' ? !el.hidden : el.classList.contains('open');
      }
      function countVisibleOverlays() {
        return OVERLAY_IDS.filter(overlayVisible).length;
      }

      domReady().then(function () {
        // 0. boot-time injected-content census (BUG-197). Runs BEFORE step 1
        // on purpose — a page that already has foreign content on it should
        // never get to claim a bridge_echo pass, since nothing downstream can
        // be trusted once this fails. Reports its own final verdict directly
        // (report()'s finalReported guard makes the generic top-level .catch()
        // below a no-op for this case) so the FAIL is attributed to
        // 'boot_prereq_dom_census', not the generic 'journey' bucket.
        var findings = domCensus();
        if (findings.length) {
          var detail = 'PANGRAM_PREREQ: injected content detected at boot — ' +
                       findings.join('; ');
          report(true, false, 'boot_prereq_dom_census', detail);
          throw new Error(detail);
        }
        report(false, true, 'boot_prereq_dom_census');
      }).then(function () {
        // 1. bridge alive: direct echo round-trip through the real shim.
        return window.pietNative.request('echo', { probe: 'ui-smoke', n: 42 }).then(function (r) {
          if (!r || !r.echoed || r.echoed.n !== 42) {
            throw new Error('echo payload mismatch: ' + JSON.stringify(r));
          }
          report(false, true, 'bridge_echo');
        });
      }).then(function () {
        // 2. kernel dot green — the SPA's own boot + ping succeeded.
        return waitFor('kernel dot green (#kernel-dot .dot-ok)', function () {
          var dot = document.getElementById('kernel-dot');
          return dot && dot.classList.contains('dot-ok');
        }, STEP_TIMEOUT_MS).then(function () {
          var banner = document.getElementById('mock-banner');
          if (banner && !banner.hidden) {
            throw new Error('mock banner visible — SPA fell back to mock mode');
          }
          report(false, true, 'kernel_dot_green');
        });
      }).then(function () {
        // 3. open dataset via the real console `use` path; varlist populates.
        submit('use "' + DATA_PATH + '"');
        return waitFor('varlist rows (#var-list-wrap .var-row)', function () {
          return document.querySelectorAll('#var-list-wrap .var-row').length > 0;
        }, STEP_TIMEOUT_MS).then(function () {
          var n = document.querySelectorAll('#var-list-wrap .var-row').length;
          report(false, true, 'varlist_populated', n + ' variable rows');
        }).then(function () {
          // 3b. STATUSBAR HONESTY (P8 "Honest Cockpit", U1 gate-blindness
          // close): a painted-but-empty statusbar that never actually named
          // the loaded file was invisible to this gate before (varlist's
          // source_file / open's filepath were wired nowhere). Only the
          // FILENAME (DATA_PATH's basename) need appear as a substring — the
          // bar may render the full resolved path.
          var fname = DATA_PATH.split('/').pop();
          return waitFor('#statusbar text contains loaded filename (' + fname + ')', function () {
            var bar = document.getElementById('statusbar');
            return !!(bar && bar.textContent && bar.textContent.indexOf(fname) !== -1);
          }, STEP_TIMEOUT_MS).then(function () {
            var bar = document.getElementById('statusbar');
            report(false, true, 'statusbar_honest', 'statusbar: ' + JSON.stringify(bar.textContent.slice(0, 160)));
          });
        });
      }).then(function () {
        // 4. run one estimation; a rendered results block must appear.
        submit('regress fatal beertax unemp');
        return waitFor('rendered estimation block (.block-estimation .coeftable)', function () {
          var el = document.querySelector('#console-body .block-estimation .coeftable');
          return (el && el.textContent && el.textContent.length > 0) ? el : null;
        }, STEP_TIMEOUT_MS).then(function (el) {
          var txt = el.textContent;
          if (txt.indexOf('beertax') === -1) {
            throw new Error('coeftable does not mention regressor beertax: ' + txt.slice(0, 120));
          }
          var errs = document.querySelectorAll('#console-body .block-error');
          if (errs.length > 0) {
            throw new Error('console has ' + errs.length + ' error block(s): ' +
                            (errs[errs.length - 1].textContent || '').slice(0, 200));
          }
          report(false, true, 'estimation_rendered', txt.slice(0, 120));
        });
      }).then(function () {
        // 5. PASTE-ROUTING (design decision 4): a multi-line paste on
        // #cmd-input, with piet-paste-autorun OFF (the default), must load
        // into the do-editor and must NOT auto-run.
        localStorage.setItem('piet-paste-autorun', '0');
        // Deterministic UI state (gate self-isolation): WKWebView persists
        // localStorage by bundle id ACROSS runs, so a prior run that engaged
        // split-view — or died before step 12's Reset cleared it — leaves THIS
        // run in split-view, where openDoEditor() docks the editor as a column
        // instead of adding `.open`, breaking the editor-first step below.
        // Force single-view + drop the persisted key so the journey is
        // independent of any leaked prefs.
        (function () {
          var st = document.getElementById('split-toggle');
          if (st && st.getAttribute('aria-pressed') === 'true') st.click();
          try { localStorage.removeItem('piet-split'); } catch (e) {}
        })();
        var input = document.getElementById('cmd-input');
        if (!input) throw new Error('#cmd-input not in DOM');
        var pasteLines = 'summarize fatal\nsummarize beertax';
        var baselineBlocks = document.querySelectorAll('#console-body .block').length;

        // Feasibility probe: WKWebView's ClipboardEvent/DataTransfer
        // constructors are exercised first; if they don't round-trip (some
        // WebKit builds restrict DataTransfer construction/setData outside
        // a real user gesture), fall back to a duck-typed plain Event
        // carrying a `clipboardData.getData()` shim — onInputPaste() only
        // calls e.clipboardData?.getData('text') and e.preventDefault(),
        // both satisfied structurally by the fallback, and addEventListener
        // dispatches on event TYPE regardless of the concrete constructor.
        var pasteEvent;
        var usedRealClipboardEvent = true;
        var ceFeasibilityDetail;
        try {
          var dt = new DataTransfer();
          dt.setData('text/plain', pasteLines);
          pasteEvent = new ClipboardEvent('paste', { clipboardData: dt, cancelable: true, bubbles: true });
          if (!pasteEvent.clipboardData || pasteEvent.clipboardData.getData('text') !== pasteLines) {
            throw new Error('constructed ClipboardEvent.clipboardData did not round-trip setData/getData');
          }
          ceFeasibilityDetail = 'real ClipboardEvent + DataTransfer constructible and round-tripped in this WKWebView';
        } catch (ceErr) {
          usedRealClipboardEvent = false;
          ceFeasibilityDetail = 'ClipboardEvent/DataTransfer path failed (' + String(ceErr.message || ceErr) +
                                ') — used a duck-typed plain Event with a clipboardData.getData() shim instead';
          pasteEvent = new Event('paste', { cancelable: true, bubbles: true });
          pasteEvent.clipboardData = { getData: function () { return pasteLines; } };
        }
        report(false, true, 'paste_routing_ce_feasibility', ceFeasibilityDetail);

        input.value = 'text-that-must-be-cleared-by-paste-handler';
        input.dispatchEvent(pasteEvent);

        return waitFor('do-editor opens after multi-line paste (#doedit-overlay.open)', function () {
          return document.getElementById('doedit-overlay')?.classList.contains('open');
        }, STEP_TIMEOUT_MS).then(function () {
          var ta = document.getElementById('doedit-text');
          if (!ta || ta.value.indexOf('summarize fatal') === -1 || ta.value.indexOf('summarize beertax') === -1) {
            throw new Error('#doedit-text does not contain the pasted lines verbatim: ' +
                            JSON.stringify(ta && ta.value));
          }
          if (input.value !== '') {
            throw new Error('#cmd-input was not cleared by the paste handler: ' + JSON.stringify(input.value));
          }
          return waitNoNewBlock('paste-routing settle window', baselineBlocks, 1200);
        }).then(function () {
          document.getElementById('doedit-close')?.click();
          report(false, true, 'paste_routing',
                 (usedRealClipboardEvent ? '(real ClipboardEvent) ' : '(fallback event) ') +
                 'routed to do-editor, no auto-run');
        });
      }).then(function () {
        // 6. EDITOR-FIRST RUN: window.pietNative.request('file.read', ...)
        // backs the Run.do editor-first hop (design decision 3); onRunDo()
        // itself is unreachable headlessly (native Finder picker), so this
        // drives file.read directly, then reproduces onRunDo()'s
        // externally-observable effect (fill #doedit-text, foreground the
        // editor via the SAME [data-action="doedit"] entry point
        // openDoEditor() is bound to), then runs it.
        return window.pietNative.request('file.read', { path: DO_FIXTURE_PATH }).then(function (r) {
          if (!r || typeof r.source !== 'string' || r.source.indexOf('describe') === -1) {
            throw new Error('file.read source missing/unexpected: ' + JSON.stringify(r));
          }
          if (!r.resolved_path || r.resolved_path.indexOf('ui_smoke_fixture.do') === -1) {
            throw new Error('file.read resolved_path unexpected: ' + JSON.stringify(r));
          }
          report(false, true, 'file_read_bridge', r.resolved_path);

          var ta = document.getElementById('doedit-text');
          if (!ta) throw new Error('#doedit-text not in DOM');
          ta.value = r.source;
          var doeditAnchor = document.querySelector('[data-action="doedit"]');
          if (!doeditAnchor) throw new Error('[data-action="doedit"] not in DOM');
          doeditAnchor.click();

          return waitFor('doedit overlay open after editor-first load', function () {
            return document.getElementById('doedit-overlay')?.classList.contains('open');
          }, STEP_TIMEOUT_MS);
        }).then(function () {
          var runBtn = document.getElementById('doedit-run');
          if (!runBtn) throw new Error('#doedit-run not in DOM');
          runBtn.click();
          // Wait for the TERMINAL state directly (not just "any new block") —
          // runDoEditor() synchronously appends an immediate 'do (editor)'
          // echo block BEFORE the async client.runDoString(source) call
          // resolves, so a naive "block count grew" predicate would resolve
          // on that echo and race ahead of the actual describe result.
          return waitFor('describe result or error block renders after doedit-run', function () {
            return document.querySelector('#console-body .block-table, #console-body .block-error');
          }, STEP_TIMEOUT_MS);
        }).then(function () {
          var errs = document.querySelectorAll('#console-body .block-error');
          if (errs.length > 0) {
            throw new Error('console has error block(s) after editor-first run: ' +
                            (errs[errs.length - 1].textContent || '').slice(0, 200));
          }
          var tableEl = document.querySelector('#console-body .block-table');
          if (!tableEl) {
            throw new Error('no .block-table (describe) rendered after the do-editor run — ' +
                            'honest run semantics violated');
          }
          report(false, true, 'editor_first_run', 'file.read -> editor -> doedit-run -> describe block rendered');
        });
      }).then(function () {
        // 7. OVERLAY EXCLUSIVITY (design decision 5 / A2-05): opening one
        // console-overlay via its toolbar anchor must close any other.
        var graphicsAnchor = document.querySelector('[data-action="graphics"]');
        var doeditAnchor = document.querySelector('[data-action="doedit"]');
        if (!graphicsAnchor || !doeditAnchor) throw new Error('toolbar overlay anchors not in DOM');

        graphicsAnchor.click();
        return waitFor('graphics overlay open', function () {
          return overlayVisible('graphics-overlay');
        }, STEP_TIMEOUT_MS).then(function () {
          var n = countVisibleOverlays();
          if (n !== 1) throw new Error('expected exactly 1 overlay open after graphics click, got ' + n);
          doeditAnchor.click();
          return waitFor('doedit overlay open (graphics must close)', function () {
            return overlayVisible('doedit-overlay');
          }, STEP_TIMEOUT_MS);
        }).then(function () {
          if (overlayVisible('graphics-overlay')) {
            throw new Error('graphics-overlay still visible after opening doedit-overlay — exclusivity violated');
          }
          var n = countVisibleOverlays();
          if (n !== 1) throw new Error('expected exactly 1 overlay open after doedit click, got ' + n);
          document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true, cancelable: true }));
          return waitFor('all overlays closed after Escape', function () {
            return countVisibleOverlays() === 0;
          }, STEP_TIMEOUT_MS);
        }).then(function () {
          report(false, true, 'overlay_exclusivity', 'graphics -> doedit -> Escape; at most 1 open at each point');
        });
      }).then(function () {
        // 8. TRANSITION SANITY: opening an overlay must reach a REAL
        // rendered end-state, not just a class flip with no visible effect.
        // Polled — no fixed sleep-then-check.
        //
        // Occlusion hardening (2026-07-09): WKWebView suspends the CSS
        // transition/animation clock while the window is occluded (covered by
        // another app, locked screen) — JS timers keep firing but a 200ms
        // opacity transition never advances, so asserting the animated
        // computed style produced a false RED on a covered window with the
        // timeout message below (observed 2026-07-09; immediate re-run
        // GREEN). Forcing the window frontmost can't close that hole
        // (unattended/locked-screen runs stay occluded), so the step instead
        // runs under the frontend's OWN reduced-motion configuration:
        // piet.css §1 zeroes --dur-fast/--dur-med for
        // prefers-reduced-motion, and the same inline override makes the
        // .open class flip update computed style DISCRETELY — independent of
        // the animation clock. The assertion is unchanged (getComputedStyle
        // opacity '1' + visibility 'visible'), so a class flip whose CSS
        // stops producing a visible end-state still FAILS; only the
        // animation-clock dependency is gone. Vars are restored afterwards.
        var motionVars = document.documentElement.style;
        motionVars.setProperty('--dur-fast', '0ms');
        motionVars.setProperty('--dur-med', '0ms');
        var occludePre = OCCLUDE_STEP8
            ? waitFor('window occluded before transition_sanity (document.hidden; cover the ' +
                      'window — PIET_UI_SMOKE_OCCLUDE_STEP8 is set)', function () {
                return document.hidden === true;
              }, 30000)
            : Promise.resolve();
        return occludePre.then(function () {
          var graphicsAnchor = document.querySelector('[data-action="graphics"]');
          if (!graphicsAnchor) throw new Error('[data-action="graphics"] not in DOM');
          graphicsAnchor.click();
          return waitFor('graphics-overlay opacity settles to 1 + visibility visible', function () {
            var el = document.getElementById('graphics-overlay');
            if (!el) return null;
            var cs = getComputedStyle(el);
            return (cs.opacity === '1' && cs.visibility === 'visible') ? el : null;
          }, STEP_TIMEOUT_MS);
        }).then(function () {
          report(false, true, 'transition_sanity',
                 'graphics-overlay reached opacity:1 + visibility:visible ' +
                 '(reduced-motion vars, visibilityState=' + document.visibilityState + ')');
          // UX-047 removed #graphics-close; Escape goes through the top-level
          // overlay-registry handler, which exists on both frontend versions
          // (same idiom as the overlay-exclusivity step above).
          document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true, cancelable: true }));
          return waitFor('graphics-overlay closed before the next step', function () {
            return !document.getElementById('graphics-overlay')?.classList.contains('open');
          }, STEP_TIMEOUT_MS);
        }).then(function () {
          motionVars.removeProperty('--dur-fast');
          motionVars.removeProperty('--dur-med');
        });
      }).then(function () {
        // 9. BUSY-LOCK (A1-01/A1-02 regression): setBusy(true) runs
        // synchronously inside runCommand() before its first await, so the
        // lock must already be visible the instant btn.click() returns —
        // asserted here with NO await/setTimeout in between.
        var input = document.getElementById('cmd-input');
        var btn = document.getElementById('btn-run');
        var openLink = document.querySelector('[data-action="open"]');
        var varWrap = document.getElementById('var-list-wrap');
        if (!input || !btn || !openLink || !varWrap) throw new Error('busy-lock targets not in DOM');

        input.value = 'describe';
        btn.click();

        if (!openLink.classList.contains('busy') || openLink.getAttribute('aria-disabled') !== 'true') {
          throw new Error('[data-action="open"] did not carry the busy lock synchronously after btn-run click');
        }
        if (!varWrap.classList.contains('busy')) {
          throw new Error('#var-list-wrap did not carry the busy lock synchronously after btn-run click');
        }

        return waitFor('busy lock releases after the run settles', function () {
          return !openLink.classList.contains('busy') && !varWrap.classList.contains('busy');
        }, STEP_TIMEOUT_MS).then(function () {
          if (openLink.getAttribute('aria-disabled') !== 'false') {
            throw new Error('[data-action="open"] aria-disabled not restored to false after busy release');
          }
          report(false, true, 'busy_lock', 'busy classes set synchronously on click, released after settle');
        });
      }).then(function () {
        // 10. TOOLBAR GROUP ORDER (2.2.0 item A, USER-corrected swap): the view
        // tabs sit LEFT of the structural divider, the actions group RIGHT.
        var views = document.querySelector('.toolbar-views');
        var divider = document.querySelector('.toolbar-group-divider');
        var actions = document.querySelector('.toolbar-actions');
        if (!views || !divider || !actions) throw new Error('toolbar groups (.toolbar-views/.toolbar-group-divider/.toolbar-actions) not all in DOM');
        var FOLLOWING = Node.DOCUMENT_POSITION_FOLLOWING;
        var viewsBeforeDivider = (views.compareDocumentPosition(divider) & FOLLOWING) !== 0;
        var dividerBeforeActions = (divider.compareDocumentPosition(actions) & FOLLOWING) !== 0;
        if (!viewsBeforeDivider || !dividerBeforeActions) {
          throw new Error('toolbar order wrong: expected views | divider | actions (item A swap)');
        }
        var tabs = Array.prototype.map.call(document.querySelectorAll('.toolbar-views .view-tab'),
          function (t) { return t.getAttribute('data-view'); }).join(',');
        if (tabs !== 'results,doedit,graphics') {
          throw new Error('view-tab internal order changed: ' + tabs);
        }
        report(false, true, 'toolbar_group_order', 'views | divider | actions; tabs ' + tabs);
      }).then(function () {
        // 11. RUN-SELECTION (2.2.0 item B): a drag-selected range in the
        // do-editor, run via the Stata Do-button binding (⇧⌘D), executes ONLY
        // that range (same do_string path Run .do uses).
        document.querySelector('[data-action="doedit"]').click();
        return waitFor('doedit open for run-selection', function () {
          return document.getElementById('doedit-overlay')?.classList.contains('open');
        }, STEP_TIMEOUT_MS).then(function () {
          var ta = document.getElementById('doedit-text');
          ta.value = 'display "RUNSEL_A"\ndisplay "RUNSEL_B"\ndisplay "RUNSEL_C"';
          var start = ta.value.indexOf('display "RUNSEL_B"');
          var end = start + 'display "RUNSEL_B"'.length;
          ta.focus();
          ta.setSelectionRange(start, end);
          var baseline = document.querySelectorAll('#console-body .block').length;
          ta.dispatchEvent(new KeyboardEvent('keydown',
            { key: 'd', metaKey: true, ctrlKey: true, shiftKey: true, bubbles: true, cancelable: true }));
          return waitFor('run-selection produced a new console block', function () {
            return document.querySelectorAll('#console-body .block').length > baseline;
          }, STEP_TIMEOUT_MS);
        }).then(function () {
          // Let any trailing display block land, then assert isolation.
          return waitFor('selected line RUNSEL_B rendered', function () {
            return (document.getElementById('console-body').textContent || '').indexOf('RUNSEL_B') !== -1;
          }, STEP_TIMEOUT_MS);
        }).then(function () {
          var consoleTxt = document.getElementById('console-body').textContent || '';
          if (consoleTxt.indexOf('RUNSEL_A') !== -1 || consoleTxt.indexOf('RUNSEL_C') !== -1) {
            throw new Error('run-selection ran unselected lines (RUNSEL_A/RUNSEL_C leaked into console)');
          }
          report(false, true, 'run_selection', 'selected middle line ran alone (RUNSEL_B only)');
        });
      }).then(function () {
        // 12. SPLIT + RESET (2.2.0 item C): split -> dock a surface column ->
        // one-click Reset restores the default single-view layout.
        var splitToggle = document.getElementById('split-toggle');
        var resetBtn = document.getElementById('reset-layout');
        var pane = document.getElementById('pane-console');
        if (!splitToggle || !resetBtn || !pane) throw new Error('split controls not in DOM');
        splitToggle.click();
        return waitFor('split mode engaged', function () {
          return pane.classList.contains('split') &&
                 splitToggle.getAttribute('aria-pressed') === 'true' && !resetBtn.hidden;
        }, STEP_TIMEOUT_MS).then(function () {
          document.querySelector('[data-action="doedit"]').click();
          return waitFor('do-editor docked as a side-by-side column', function () {
            var ov = document.getElementById('doedit-overlay');
            return ov && ov.classList.contains('docked') && ov.classList.contains('open');
          }, STEP_TIMEOUT_MS);
        }).then(function () {
          resetBtn.click();
          return waitFor('Reset layout restored single-view', function () {
            var ov = document.getElementById('doedit-overlay');
            return !pane.classList.contains('split') && resetBtn.hidden &&
                   ov && !ov.classList.contains('docked') && !ov.classList.contains('open');
          }, STEP_TIMEOUT_MS);
        }).then(function () {
          if (localStorage.getItem('piet-split') !== null) {
            throw new Error('Reset layout did not clear the persisted piet-split key');
          }
          report(false, true, 'split_reset', 'split -> dock editor column -> Reset -> single-view + persistence cleared');
        });
      }).then(function () {
        // 13. SETTINGS open -> change -> apply (2.2.0 item D): the panel loads
        // detected system info; a display change (font size) applies live; an
        // engine change persists via settings.set (isolated store).
        document.querySelector('[data-action="settings"]').click();
        return waitFor('settings overlay open + populated', function () {
          var ov = document.getElementById('settings-overlay');
          var sysinfo = document.getElementById('settings-sysinfo');
          var cores = document.getElementById('set-cores');
          return ov && ov.classList.contains('open') &&
                 sysinfo && sysinfo.textContent.indexOf('cores') !== -1 &&
                 cores && cores.options.length > 1;
        }, STEP_TIMEOUT_MS).then(function () {
          var fsSel = document.getElementById('set-fontsize');
          fsSel.value = 'large';
          fsSel.dispatchEvent(new Event('change', { bubbles: true }));
          if (document.documentElement.dataset.fontsize !== 'large') {
            throw new Error('font-size change did not apply live');
          }
          var mem = document.getElementById('set-mem-budget');
          mem.value = String(Number(mem.min) + 128);
          document.getElementById('settings-apply').click();
          return waitFor('settings apply confirmed (settings.set round-trip)', function () {
            var st = document.getElementById('settings-status');
            return st && st.textContent.indexOf('Saved') !== -1;
          }, STEP_TIMEOUT_MS);
        }).then(function () {
          // cleanup: restore medium font, close the panel.
          var fsSel = document.getElementById('set-fontsize');
          fsSel.value = 'medium';
          fsSel.dispatchEvent(new Event('change', { bubbles: true }));
          document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true, cancelable: true }));
          report(false, true, 'settings_apply', 'open -> font-size live -> memory-budget Apply -> Saved');
        });
      }).then(function () {
        // 14. PNG-EXPORT PAINT CHECK (P8 "Honest Cockpit", U2 regression gate;
        // BUG-084 was "PNG download is all-black — flatten CSS before
        // rasterizing"). This harness does not know the frontend's private
        // "⬇ PNG"-button DOM wiring or exact svgToPng() call shape (out of
        // this file's owned scope — piet/frontend/** belongs to a separate
        // agent), so a real canvas/Image decode of the exported download was
        // impractical to drive reliably here. Using the SANCTIONED fallback
        // instead: render a graph and assert its SVG marks carry INLINE
        // paint (a fill/stroke ATTRIBUTE or inline `style`), not paint that
        // lives ONLY in an external CSS class — exactly the condition the
        // BUG-084 fix restored. A canvas rasterizer given a serialized SVG
        // never resolves an external stylesheet class (no cascade context
        // outside the live DOM), so a mark painted only via a CSS class
        // rasterizes as black/transparent; inline paint survives
        // serialization and rasterizes correctly. This is a proxy for "the
        // exported PNG is not all-black", not a pixel decode.
        var svgBaseline = document.querySelectorAll('#console-body svg').length;
        submit('scatter fatal beertax');
        return waitFor('a new graph SVG renders in #console-body', function () {
          var svgs = document.querySelectorAll('#console-body svg');
          return svgs.length > svgBaseline ? svgs[svgs.length - 1] : null;
        }, STEP_TIMEOUT_MS).then(function (svg) {
          var marks = svg.querySelectorAll('circle, rect, path, line, polyline, polygon');
          if (marks.length === 0) {
            throw new Error('graph SVG rendered with no mark elements ' +
                            '(circle/rect/path/line/polyline/polygon)');
          }
          var NEUTRAL = { '': 1, 'none': 1, 'transparent': 1, 'black': 1,
                          '#000': 1, '#000000': 1, 'rgb(0,0,0)': 1, 'rgba(0,0,0,0)': 1 };
          function paints(v) {
            if (!v) return false;
            return !NEUTRAL[v.replace(/\s+/g, '').toLowerCase()];
          }
          // svgToPng() (app.js) flattens paint by reading getComputedStyle on
          // the LIVE marks and inlining it onto the export clone. The live SVG
          // paints via CSS classes (.g-point/.g-line/…) by design, so checking
          // the live node for INLINE paint is the WRONG artifact (absent on
          // purpose). The correct proxy for "the exported PNG is not all-black"
          // is that the marks RESOLVE (via the cascade) to real, non-black
          // paint — exactly the value svgToPng captures and inlines.
          var hasResolvedPaint = Array.prototype.some.call(marks, function (el) {
            var cs = getComputedStyle(el);
            return paints(cs.fill) || paints(cs.stroke);
          });
          if (!hasResolvedPaint) {
            throw new Error('graph SVG marks resolve to no visible fill/stroke paint (getComputedStyle) — ' +
                            'svgToPng would flatten black and the exported PNG would be all-black (BUG-084)');
          }
          report(false, true, 'png_export_paint_check',
                 marks.length + ' mark element(s) resolve to real paint via getComputedStyle (the value ' +
                 'svgToPng inlines onto its export clone — proxy for a non-black PNG rasterization; a real ' +
                 'canvas getImageData decode was impractical from this headless harness)');
        });
      }).then(function () {
        // 16. REQUEST-DIALOG JOURNEY (D9, BETA Phase F — BUG-087 native
        // _request()): a do-string containing `_request(macname)` must pause
        // the run, push a REAL request_dialog modal (#request-overlay), take
        // a typed answer through the REAL bridge (POST /api/request/answer),
        // and resume — $macname must resolve to the typed value afterward.
        // Driven through the do-editor's Run .do (client.runDoString ->
        // the ASYNC "do_string" bridge route, the background do-executor
        // thread) — NOT the single-line #cmd-input/#btn-run path, which
        // goes through the SYNCHRONOUS "command" route and is refused
        // outright by the native side on purpose (blocking the UI thread
        // there would deadlock the whole app; see session_state.h's
        // isCurrentThreadADoWorker() doc comment).
        document.querySelector('[data-action="doedit"]').click();
        return waitFor('doedit open for _request() journey', function () {
          return document.getElementById('doedit-overlay')?.classList.contains('open');
        }, STEP_TIMEOUT_MS).then(function () {
          var ta = document.getElementById('doedit-text');
          if (!ta) throw new Error('#doedit-text not in DOM');
          ta.value = 'display "before request"\n' +
                     'display as text "Type a value: " _request(uismokereq)\n' +
                     'display "after: $uismokereq"';
          var runBtn = document.getElementById('doedit-run');
          if (!runBtn) throw new Error('#doedit-run not in DOM');
          runBtn.click();
          return waitFor('request-dialog overlay opens (#request-overlay.open)', function () {
            return document.getElementById('request-overlay')?.classList.contains('open');
          }, STEP_TIMEOUT_MS);
        }).then(function () {
          var macroEl = document.getElementById('request-macro');
          var promptEl = document.getElementById('request-prompt');
          if (!macroEl || macroEl.textContent.indexOf('uismokereq') === -1) {
            throw new Error('#request-macro does not name the pending macro: ' +
                            JSON.stringify(macroEl && macroEl.textContent));
          }
          if (!promptEl || promptEl.textContent.indexOf('Type a value:') === -1) {
            throw new Error('#request-prompt does not carry the preceding display text: ' +
                            JSON.stringify(promptEl && promptEl.textContent));
          }
          var input = document.getElementById('request-input');
          if (!input) throw new Error('#request-input not in DOM');
          input.value = 'ui-smoke-answer';
          var form = document.getElementById('request-form');
          if (!form) throw new Error('#request-form not in DOM');
          if (form.requestSubmit) { form.requestSubmit(); }
          else { document.getElementById('request-submit').click(); }
          return waitFor('request-dialog overlay closes after submit', function () {
            return !document.getElementById('request-overlay')?.classList.contains('open');
          }, STEP_TIMEOUT_MS);
        }).then(function () {
          return waitFor('do-run resumed and rendered the post-_request() display line', function () {
            var txt = document.getElementById('console-body').textContent || '';
            return txt.indexOf('after: ui-smoke-answer') !== -1;
          }, STEP_TIMEOUT_MS);
        }).then(function () {
          report(false, true, 'request_dialog_journey',
                 '_request() paused the run, real modal answered via /api/request/answer, ' +
                 '$uismokereq resolved to the typed value, execution resumed');
        });
      }).then(function () {
        // 17. RESIZER GEOMETRY (P8.1 T6, BUG-094): the Variables/History
        // pane-resizer grab zones (panes.js's #pane-resizer-left/-right) must
        // span ONLY the content row — never the toolbar or statusbar rows.
        // This regressed once as CSS Grid row '1 / 6' (the full grid height),
        // which put the invisible col-resize strip on TOP of toolbar controls
        // near the divider and silently swallowed their clicks; the fix
        // scoped it to gridRow '3' (the content row alongside Results/
        // Variables/History). Real CSS Grid layout is required to observe
        // this at all (jsdom has none — the DOM test runner can only assert
        // the element exists, not where it sits), so it can ONLY be gated
        // here, against the packaged app's real WKWebView layout engine.
        var toolbarEl = document.getElementById('toolbar');
        var statusbarEl = document.getElementById('statusbar');
        if (!toolbarEl || !statusbarEl) throw new Error('#toolbar/#statusbar not in DOM');
        var toolbarRect = toolbarEl.getBoundingClientRect();
        var statusbarRect = statusbarEl.getBoundingClientRect();
        var GEOM_TOL = 1;   // px tolerance for sub-pixel layout rounding
        ['pane-resizer-left', 'pane-resizer-right'].forEach(function (id) {
          var el = document.getElementById(id);
          if (!el) throw new Error('#' + id + ' not in DOM');
          var r = el.getBoundingClientRect();
          if (r.top < toolbarRect.bottom - GEOM_TOL) {
            throw new Error('#' + id + ' (top=' + r.top + ') vertically intersects #toolbar (bottom=' +
                            toolbarRect.bottom + ')');
          }
          if (r.bottom > statusbarRect.top + GEOM_TOL) {
            throw new Error('#' + id + ' (bottom=' + r.bottom + ') vertically intersects #statusbar (top=' +
                            statusbarRect.top + ')');
          }
        });
        report(false, true, 'resizer_geometry',
               'pane-resizer-left/right grab zones stay within the content row — no vertical ' +
               'overlap with #toolbar or #statusbar (BUG-094)');
      }).then(function () {
        // 18. SETTINGS TOGGLE (P8.1 T6, BUG-095): the [data-action="settings"]
        // toolbar link opens #settings-overlay; #settings-close (the
        // reintroduced visible ✕ — see step 13's BUG-095 comment) closes it;
        // and a SECOND click on the SAME link toggles it closed again
        // (app.js's settingsLink handler branches on _isOverlayOpen rather
        // than unconditionally opening — the exact BUG-095 fix under test).
        // Ends with the overlay closed so it doesn't linger open for
        // whatever runs after this journey.
        var settingsLink = document.querySelector('[data-action="settings"]');
        var settingsOv = document.getElementById('settings-overlay');
        var settingsCloseBtn = document.getElementById('settings-close');
        if (!settingsLink || !settingsOv || !settingsCloseBtn) {
          throw new Error('settings toggle elements ([data-action="settings"]/#settings-overlay/#settings-close) not in DOM');
        }
        settingsLink.click();
        return waitFor('settings overlay opens on first link click', function () {
          return settingsOv.classList.contains('open');
        }, STEP_TIMEOUT_MS).then(function () {
          settingsCloseBtn.click();
          return waitFor('#settings-close closes the overlay', function () {
            return !settingsOv.classList.contains('open');
          }, STEP_TIMEOUT_MS);
        }).then(function () {
          settingsLink.click();
          return waitFor('settings overlay re-opens on a second link click', function () {
            return settingsOv.classList.contains('open');
          }, STEP_TIMEOUT_MS);
        }).then(function () {
          settingsLink.click();   // same link, clicked again — toggle closed
          return waitFor('the SAME link click toggles the overlay closed (BUG-095 toggle semantics)', function () {
            return !settingsOv.classList.contains('open');
          }, STEP_TIMEOUT_MS);
        }).then(function () {
          report(false, true, 'settings_toggle',
                 'link opens -> #settings-close closes -> link re-opens -> same link toggles closed; ' +
                 'restored to closed');
        });
      }).then(function () {
        // 19. FILE SAVE ROUNDTRIP (P8.1 W2, N1): write a file through the REAL
        // file.save route with an EXPLICIT path (the headless / default-export
        // branch — no NSSavePanel), then read it back via file.read and assert
        // the bytes roundtrip. Proves the bridge genuinely wrote to disk, not a
        // painted {ok:true}. The target is a .do path so file.read (which only
        // serves .do) can verify it; kind:"log" is advisory on the explicit-
        // path branch and does not reshape the written bytes.
        var SAVE_MARKER = 'ui-smoke file.save roundtrip OK\n';
        return window.pietNative.request('file.save', {
          suggested_name: 'ui_smoke_save_roundtrip.do',
          kind: 'log',
          text: SAVE_MARKER,
          path: SAVE_TARGET_PATH
        }).then(function (r) {
          if (!r || r.ok !== true || r.path !== SAVE_TARGET_PATH) {
            throw new Error('file.save did not return {ok:true, path:<target>}: ' + JSON.stringify(r));
          }
          return window.pietNative.request('file.read', { path: SAVE_TARGET_PATH });
        }).then(function (rd) {
          if (!rd || rd.source !== SAVE_MARKER) {
            throw new Error('file.read did not return the exact bytes file.save wrote: ' + JSON.stringify(rd));
          }
          report(false, true, 'file_save_roundtrip',
                 'file.save(explicit path, text) -> file.read: bytes roundtrip, real disk write (N1)');
        });
      }).then(function () {
        // 20. WINDOW TITLE ROUTE (P8.1 W2, N2): POST /api/window/title sets the
        // native NSWindow title verbatim. The title is an AppKit property, not
        // in the DOM, so it is not observable from JS — per the contract this
        // asserts ONLY the route's {ok:true} response shape.
        return window.pietNative.request('window.title', { text: 'PIET — ui-smoke roundtrip' })
          .then(function (r) {
            if (!r || r.ok !== true) {
              throw new Error('window.title did not return {ok:true}: ' + JSON.stringify(r));
            }
            report(false, true, 'window_title_route',
                   'POST /api/window/title -> {ok:true} (native title set verbatim; not JS-observable) (N2)');
          });
      }).then(function () {
        report(true, true, 'journey',
               'boot -> bridge -> kernel dot green -> use -> varlist -> statusbar honest -> regress -> ' +
               'rendered block -> paste routing -> editor-first run -> overlay exclusivity -> ' +
               'transition sanity -> busy lock -> toolbar order -> run-selection -> split+reset -> ' +
               'settings apply -> png export paint check -> request dialog journey -> resizer geometry -> ' +
               'settings toggle -> file save roundtrip -> window title route');
      }).catch(function (e) {
        var detail = String((e && e.message) || e);
        try {
          var errs = document.querySelectorAll('#console-body .block-error');
          if (errs.length) {
            detail += ' | last console error block: ' +
                      (errs[errs.length - 1].textContent || '').slice(0, 300);
          }
        } catch (ignored) {}
        report(true, false, 'journey', detail);
      });
    })();
  )JS";
  return js;
}

// --- BUG-197 falsification hook: PIET_UI_SMOKE_SEED_INJECTION ---------------
// A hermetic stand-in for "a real extension/injector got to this page
// first", so the DONE-WHEN #2 seeded-RED certificate needs no actual
// Grammarly install. Registered via initJs BEFORE smokeDriverJs (user
// scripts run in registration order — same fact main.cpp's installBridge
// comment relies on), so this listener attaches to DOMContentLoaded first
// and its synchronous appendChild()s land before smokeDriverJs's domReady()
// .then() microtask runs (that .then() cannot fire until the DOMContentLoaded
// dispatch — during which both listeners run synchronously in registration
// order — has fully returned). No network I/O: the fake script node carries
// no `src`, only a marker attribute, so this cannot hang on a DNS lookup.
std::string seedInjectionProbeJs() {
  return R"JS(
    (function () {
      function seed() {
        var s = document.createElement('script');
        s.setAttribute('data-fake-injected-probe', 'PIET_UI_SMOKE_SEED_INJECTION');
        s.textContent = '/* seeded by PIET_UI_SMOKE_SEED_INJECTION — not a real threat */';
        document.documentElement.appendChild(s);
        var marker = document.createElement('div');
        marker.id = 'grammarly-desktop-integration';
        marker.setAttribute('data-gramm', 'true');
        marker.style.display = 'none';
        (document.body || document.documentElement).appendChild(marker);
      }
      if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', seed);
      } else {
        seed();
      }
    })();
  )JS";
}

int runUiSmoke() {
  // Fixture default (WI-4, 2.0.3): the SHIPPED sample dataset — bundled
  // Resources/sample/fatalities.csv when running from a packaged .app (the
  // gate is then fully self-contained on any machine), else the same file in
  // the worktree (piet/packaging/sample/). $PIET_UI_SMOKE_DATA overrides.
  // env_utf8: a Korean %USERPROFILE%-rooted override arrives from narrow
  // getenv as CP949, which no wide open would ever find (BUG-051).
  std::string dataEnv = cecon::env_utf8("PIET_UI_SMOKE_DATA");
  std::string dataDefault;
  if (std::optional<std::string> resources = piet::bundleResourcesDir()) {
    dataDefault = *resources + "/sample/fatalities.csv";
  } else {
    dataDefault = repoRelative("/piet/packaging/sample/fatalities.csv");
  }
  std::string dataPath = !dataEnv.empty() ? dataEnv : dataDefault;
  std::string outEnv = cecon::env_utf8("PIET_UI_SMOKE_OUT");
  // /tmp does not exist on Windows. Only the Windows arm moves to %TEMP% —
  // temp_directory_path() is $TMPDIR on macOS, so making this unconditional
  // would silently relocate the Mac verdict file.
#ifdef _WIN32
  std::string outDefault =
      (std::filesystem::temp_directory_path() / "piet_ui_smoke_verdict.json").u8string();
#else
  std::string outDefault = "/tmp/piet_ui_smoke_verdict.json";
#endif
  std::string outPath = !outEnv.empty() ? outEnv : outDefault;

  if (!fileExists(dataPath)) {
    std::fprintf(stderr,
                 "[ui-smoke] FATAL: fixture dataset not found at %s (set PIET_UI_SMOKE_DATA)\n",
                 dataPath.c_str());
    return 1;
  }

  std::string workdir = makeWorkdir();
  ScratchDirGuard scratchGuard{workdir};  // reaped at process exit (PIET-TMP/BUG-092)
  AppState app(workdir);
  SmokeState smoke;

  // 2.2.0 item D: isolate the settings store to this run's scratch workdir so
  // the step-13 settings.set journey never clobbers the User's real settings.
  {
    std::string smokeSettings = workdir + "/ui_smoke_settings.json";
#ifndef _WIN32
    setenv("PIET_SETTINGS_FILE", smokeSettings.c_str(), 1);
#else
    // Wide put: a Hangul workdir would be stored mangled by the narrow form,
    // and settings.cpp reads this back through env_utf8/wide open.
    _wputenv_s(L"PIET_SETTINGS_FILE", cecon::to_native_path(smokeSettings).c_str());
#endif
  }

  // s6 EDITOR-FIRST RUN fixture (stabilization campaign delta): a tiny
  // one-line .do file written into this smoke run's OWN scratch workdir (an
  // absolute path, so resolveUserPath() passes it through unchanged
  // regardless of session.cwd — see piet::resolveUserPath's selftest
  // invariants) so the driver's file.read step exercises the real
  // resolve+validate+read path (piet::readDoFileForBridge) against a file
  // that genuinely exists on disk, independent of repo-tree layout.
  std::string doFixturePath = workdir + "/ui_smoke_fixture.do";
  {
    std::ofstream fixtureFile(cecon::to_native_path(doFixturePath));
    fixtureFile << "describe\n";
  }

  // P8.1 W2 (N1): file_save_roundtrip target — an ABSOLUTE .do path in this
  // run's own scratch workdir. The driver writes it via file.save (explicit
  // path) then reads it back via file.read to prove a real disk write; a .do
  // name so file.read (which only serves .do) can verify it.
  std::string saveTargetPath = workdir + "/ui_smoke_save_roundtrip.do";

  // --- BUG-197: hermetic-verdict regime -------------------------------------
  // Two independent layers, and the report below names both so a future
  // reader of one log line knows which regime judged that run:
  //   1. Ephemeral WKWebsiteDataStore (vendor/webview/webview.h Cocoa patch,
  //      "PIET_WEBVIEW_EPHEMERAL_DATASTORE") — real isolation for this run's
  //      cookies/cache/storage from the host's shared store. Default ON for
  //      this gate; `setenv(..., /*overwrite=*/0)` lets a caller force it off
  //      (used by the seeded-RED probe below to show the census fires
  //      identically either way — see the BUG-197 close note for why: it is
  //      not the layer the confirmed injector goes through).
  //   2. Boot-time injected-content census, always on (main.cpp's
  //      domCensus() inside smokeDriverJs) — THIS is the layer that actually
  //      judges the run: it is what a seeded fake extension trips.
  // Measured non-fix: WKWebsiteDataStore governs persisted browsing state
  // (cookies/cache/IndexedDB/service workers). Grammarly Desktop — the one
  // injector this fleet has confirmed hitting this DOM (piet/README.md
  // "Known issues"; the internal bug ledger BUG-197) — attaches through
  // the macOS Accessibility API / a global input-event tap, neither of which
  // has any relationship to a WKWebView's data-store identity. Item 1 alone
  // is therefore not a fix for BUG-197's finding; it ships anyway as
  // legitimate defense-in-depth for the storage-borne class of interference
  // (a stale cookie/cache left by a PREVIOUS run, a service worker, etc.),
  // and item 2 is the layer whose regime line below is the honest answer to
  // "what judged this run".
#ifndef _WIN32
  setenv("PIET_WEBVIEW_EPHEMERAL_DATASTORE", "1", /*overwrite=*/0);
#endif
  const char* ephemeralEnv = std::getenv("PIET_WEBVIEW_EPHEMERAL_DATASTORE");
  bool ephemeralDatastore = ephemeralEnv && *ephemeralEnv && std::strcmp(ephemeralEnv, "0") != 0;

  // Falsification hook (BUG-197 DONE-WHEN #2): PIET_UI_SMOKE_SEED_INJECTION=1
  // makes THIS run seed a fake foreign <script> node + the confirmed
  // Grammarly DOM markers into the page before the driver's own boot census
  // runs — a hermetic stand-in for "an extension/injector got to this page
  // first" that needs no actual Grammarly install to exercise. A GREEN run
  // under this flag is the gate failing to do its job.
  const char* seedEnv = std::getenv("PIET_UI_SMOKE_SEED_INJECTION");
  bool seedInjection = seedEnv && *seedEnv && std::strcmp(seedEnv, "0") != 0;

  piet::Shell shell(/*debug=*/true);
  shell.setTitle("PIET 2.0 — UI smoke");
  shell.setSize(1280, 860, /*resizable=*/true);
  shell.installBridge();
  // The EXACT product route surface — same function runRealFrontend() uses.
  bindAllRoutes(shell, app);
  // BUG-087/D9 (BETA Phase F): same _request() dialog-push wiring
  // runRealFrontend() does — see that call site's doc comment. Required for
  // the request_dialog_journey step below to exercise a REAL modal, not a
  // headless fallback (this mode always constructs a real Shell/webview).
  app.session.setRequestPostEvent([&shell](const std::string& json) { shell.postEvent(json); });

  // Test-mode-only verdict channel (see the mode doc comment above — NOT a
  // Boundary-B route; never bound in the product run mode).
  shell.bindRoute("smoke.report", [&smoke](const std::string& body) -> piet::BridgeResult {
    nlohmann::ordered_json rep;
    try {
      rep = nlohmann::ordered_json::parse(body);
    } catch (const nlohmann::json::exception&) {
      rep = nlohmann::ordered_json{
          {"final", true}, {"pass", false}, {"step", "report_parse"}, {"detail", body}};
    }
    bool isFinal = rep.value("final", false);
    bool pass = rep.value("pass", false);
    std::printf("[ui-smoke] %s %s %s%s%s\n", isFinal ? "FINAL" : "step ",
                pass ? "PASS" : "FAIL", rep.value("step", std::string("?")).c_str(),
                rep.value("detail", std::string()).empty() ? "" : " — ",
                rep.value("detail", std::string()).c_str());
    {
      std::lock_guard<std::mutex> lock(smoke.m);
      smoke.reports.push_back(rep);
      if (isFinal) {
        smoke.finalReceived = true;
        smoke.finalPass = pass;
      }
    }
    if (isFinal) smoke.cv.notify_all();
    return piet::BridgeResult::success(R"({"ok":true})");
  });

  // Occlusion-verification knob (2026-07-09 transition_sanity hardening): with
  // PIET_UI_SMOKE_OCCLUDE_STEP8=1 the driver WAITS (up to 30s) for the window
  // to actually be occluded (document.hidden, which WebKit ties to
  // NSWindow occlusion on macOS) before running step 8 — so "verify with the
  // window covered" is a deterministic protocol (cover the window after
  // launch), not a race against the ~3s journey.
  const char* occludeEnv = std::getenv("PIET_UI_SMOKE_OCCLUDE_STEP8");
  bool occludeStep8 = occludeEnv && *occludeEnv && std::strcmp(occludeEnv, "0") != 0;

  std::string indexPath = resolveFrontendIndexPath();
  // AFTER installBridge(): user scripts run in registration order, so the
  // driver sees window.pietNative already installed at document start.
  // BEFORE smokeDriverJs when armed: the seeded probe's DOMContentLoaded
  // listener must be registered first so its synchronous DOM mutation lands
  // ahead of the driver's own boot census (see seedInjectionProbeJs()'s
  // comment for why registration order is sufficient here).
  if (seedInjection) {
    shell.initJs(seedInjectionProbeJs());
    std::printf(
        "[ui-smoke] PIET_UI_SMOKE_SEED_INJECTION active — this run WILL seed a fake "
        "injected script + Grammarly DOM markers and expects the boot census to refuse it\n");
  }
  shell.initJs(smokeDriverJs(dataPath, doFixturePath, saveTargetPath, occludeStep8));
  navigateFrontend(shell, indexPath);

  // BUG-197 self-check line: names the regime that judges THIS run's
  // verdict, printed before the journey runs so it is present even on a
  // 90s-timeout RED with zero step reports. The operative regime is always
  // "prereq-checked" (the boot census in smokeDriverJs) — ephemeral_datastore
  // is reported alongside as a second, independently-toggleable fact, not as
  // an alternative regime, since it is measured to not affect this gate's
  // verdict (see the close note above the setenv call).
  std::printf("[ui-smoke] mode: prereq-checked (ephemeral_datastore=%s%s)\n",
              ephemeralDatastore ? "on" : "off", seedInjection ? ", seed_injection=on" : "");

  // Watchdog/terminator: waits for the final verdict (or 90s — covers a page
  // that never executes anything), then exits the UI run loop from this
  // background thread via the dispatch-marshalled terminate. A normal
  // completion signals the cv immediately, so GREEN runs exit fast.
  std::thread terminator([&shell, &smoke]() {
    {
      std::unique_lock<std::mutex> lock(smoke.m);
      smoke.cv.wait_for(lock, std::chrono::seconds(90),
                        [&smoke] { return smoke.finalReceived; });
    }
    shell.terminateFromAnyThread();
  });

  std::printf("[ui-smoke] window open (%s) — journey driver running...\n", indexPath.c_str());
  shell.run();

  terminator.join();
  // Same owned-join discipline as runRealFrontend().
  app.session.progressRequestCancel();
  app.joinAll();

  bool green;
  nlohmann::ordered_json verdict;
  {
    std::lock_guard<std::mutex> lock(smoke.m);
    green = smoke.finalReceived && smoke.finalPass;
    verdict["gate"] = "piet_native --ui-smoke";
    verdict["verdict"] = green ? "GREEN" : "RED";
    verdict["final_received"] = smoke.finalReceived;
    if (!smoke.finalReceived) {
      verdict["timeout"] =
          "no final report within the 90s watchdog — the page/bridge never ran the journey driver";
    }
    verdict["piet_version"] = PIET_VERSION_STRING;
    verdict["contract_a"] = PIET_CONTRACT_A_VERSION;
    verdict["contract_b"] = PIET_CONTRACT_B_VERSION;
    verdict["frontend_index"] = indexPath;
    verdict["data_fixture"] = dataPath;
    // BUG-197: which regime judged this run (see the mode line above for the
    // rationale — prereq-checked is the layer that can actually flip the
    // verdict; ephemeral_datastore is reported as a second independent fact).
    verdict["ui_smoke_regime"] = "prereq-checked";
    verdict["ephemeral_datastore"] = ephemeralDatastore;
    verdict["seed_injection_probe"] = seedInjection;
    verdict["steps"] = smoke.reports;
  }
  {
    std::ofstream vf(cecon::to_native_path(outPath));
    if (vf) {
      vf << verdict.dump(2) << "\n";
      std::printf("[ui-smoke] verdict written: %s\n", outPath.c_str());
    } else {
      std::fprintf(stderr, "[ui-smoke] WARNING: could not write verdict file %s\n",
                   outPath.c_str());
    }
  }
  std::printf("[ui-smoke] %s\n",
              green ? "GREEN — full journey passed in the real platform webview"
                    : "RED — journey failed (see verdict file)");
  return green ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
  // GUI double-click must NOT pop a console window (this is a -mwindows /
  // WINDOWS-subsystem build, like the macOS .app has no terminal). But the CLI
  // modes (--do/--selftest/--version/--ui-smoke) still need stdout: when we were
  // launched from an interactive terminal, reattach to that parent console so
  // their output is visible. On a double-click there is no parent console,
  // AttachConsole fails harmlessly, and nothing appears. Redirected/piped stdout
  // (e.g. `--do x > out`) already works via the inherited handle regardless of
  // subsystem.
  if (AttachConsole(ATTACH_PARENT_PROCESS)) {
    // Reattach a std stream to the parent console ONLY where it is not already
    // redirected to a pipe or file — otherwise we'd clobber `--do x > out` or a
    // shell pipe (`--do x | ...`), swallowing the output.
    auto redirected = [](DWORD which) {
      DWORD t = GetFileType(GetStdHandle(which));
      return t == FILE_TYPE_DISK || t == FILE_TYPE_PIPE;
    };
    if (!redirected(STD_OUTPUT_HANDLE)) (void)std::freopen("CONOUT$", "w", stdout);
    if (!redirected(STD_ERROR_HANDLE)) (void)std::freopen("CONOUT$", "w", stderr);
    if (!redirected(STD_INPUT_HANDLE)) (void)std::freopen("CONIN$", "r", stdin);
  }
  // AFTER the freopen()s above (freopen resets the fd's translation mode): put
  // the std streams in binary so the CRT does not rewrite every '\n' the
  // --do/--run-block/--dump-registry writers emit into CRLF. The port is gated
  // on BYTE-IDENTICAL fixture comparison, so a text-mode stdout would fail
  // every freeze check for a reason that has nothing to do with the engine.
  cecon::native_set_standard_streams_binary();
  // BUG-050: recover real Unicode argv from the wide command line. A narrow
  // main() receives argv re-decoded through the ANSI codepage, which on a
  // CP949 (Korean) system turns a Hangul path arg (e.g. --do
  // "...\분석.do") into CP949 bytes that are NOT valid UTF-8 — so the first
  // to_native_path()/u8path() on it throws filesystem "Illegal byte sequence".
  // Re-expose args as UTF-8 (same treatment cobra's main already gets) so every
  // downstream path arg is UTF-8. u8store/u8argv live for all of main().
  std::vector<std::string> u8store;
  std::vector<char*> u8argv;
  if (int wargc = 0; LPWSTR* wv = CommandLineToArgvW(GetCommandLineW(), &wargc)) {
    u8store.reserve(static_cast<size_t>(wargc));
    for (int i = 0; i < wargc; ++i) {
      int n = WideCharToMultiByte(CP_UTF8, 0, wv[i], -1, nullptr, 0, nullptr, nullptr);
      std::string s;
      if (n > 0) {
        s.resize(static_cast<size_t>(n));
        WideCharToMultiByte(CP_UTF8, 0, wv[i], -1, s.data(), n, nullptr, nullptr);
        s.resize(static_cast<size_t>(n - 1));  // drop the trailing NUL
      }
      u8store.push_back(std::move(s));
    }
    LocalFree(wv);
    for (auto& s : u8store) u8argv.push_back(s.data());
    u8argv.push_back(nullptr);
    argc = wargc;
    argv = u8argv.data();
  }
#endif
  bool demo = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--selftest") == 0) {
      // R1 (selftest split): the suite lives in the sibling `piet_selftest`
      // binary now — exec it so every script that says `piet_native --selftest`
      // keeps working, dev tree and CI alike. The product binary itself no
      // longer carries the 24k-line suite; a packaged .app that does not bundle
      // piet_selftest gets an honest pointer instead of a silent no-op.
#ifndef _WIN32
      std::error_code selfEc;
      std::filesystem::path self =
          std::filesystem::weakly_canonical(std::filesystem::path(argv[0]), selfEc);
      std::filesystem::path sib =
          (selfEc || !self.has_parent_path()) ? std::filesystem::path("piet_selftest")
                                              : self.parent_path() / "piet_selftest";
      ::execl(sib.string().c_str(), "piet_selftest", static_cast<char*>(nullptr));
      std::fprintf(stderr,
                   "[piet_native] --selftest: could not exec %s (%s). The suite builds as the "
                   "separate `piet_selftest` binary since the R1 split — build it with "
                   "`cmake --build <builddir> --target piet_selftest` and run it directly.\n",
                   sib.string().c_str(), std::strerror(errno));
      return 2;
#else
      std::fprintf(stderr,
                   "[piet_native] --selftest: run the sibling piet_selftest.exe directly on "
                   "Windows (R1 split).\n");
      return 2;
#endif
    }
    if (std::strcmp(argv[i], "--version") == 0) {
      // WP-2 single-version-source derivation point: prints the COMPILED
      // piet-axis tuple. package_native_app.sh reads this back out of the
      // binary it bundles to stamp Info.plist — never a hand-synced copy.
      std::printf("{\"piet_version\":\"%s\",\"contract_a\":\"%s\",\"contract_b\":\"%s\"}\n",
                  PIET_VERSION_STRING, PIET_CONTRACT_A_VERSION, PIET_CONTRACT_B_VERSION);
      return 0;
    }
    if (std::strcmp(argv[i], "--dump-registry") == 0) {
      // BETA campaign Phase A0: prints registry::commandRegistry() as JSON —
      // MUST be a returning branch here (not a bool-then-fallthrough like
      // --demo below), since the post-loop fallthrough calls
      // runRealFrontend(), which hangs headless (see registry.h's doc
      // comment + the A0 anchor map's LANDMINE note).
      return piet::registry::runDumpRegistry();
    }
    if (std::strcmp(argv[i], "--dump-prefix-registry") == 0) {
      return piet::registry::runDumpPrefixRegistry();
    }
    // AER scope-2 A1: the option-vocabulary census, and the one-option probe of
    // dispatcher.cpp's resolution point. RETURNING arms for the same reason
    // --dump-registry above spells out. Both stand alone and must precede any
    // --do, which swallows every remaining argv below.
    if (std::strcmp(argv[i], "--dump-optvocab") == 0) {
      if (i + 1 < argc && std::strcmp(argv[i + 1], "--resolve") == 0) {
        // Arity error follows the --parallel-worker/--do precedent: stderr and
        // rc 2, never a fallthrough into runRealFrontend()'s headless hang.
        if (i + 3 >= argc) {
          std::fprintf(stderr,
                       "[piet_native] --dump-optvocab --resolve requires <command> <option>\n");
          return 2;
        }
        return piet::registry::runDumpOptVocabResolve(argv[i + 2], argv[i + 3]);
      }
      return piet::registry::runDumpOptVocab();
    }
    if (std::strcmp(argv[i], "--dump-functions") == 0) {
      return piet::registry::runDumpFunctions();
    }
    if (std::strcmp(argv[i], "--ui-smoke") == 0) {
      return runUiSmoke();
    }
    if (std::strcmp(argv[i], "--evalcase") == 0) {
      return piet::eval::runEvalcaseDriver();
    }
    if (std::strcmp(argv[i], "--run-block") == 0) {
      return piet::runBlockAdapter();
    }
    // PLEVEL-IMPL-WAVE arc ①: the parallel WORKER entry point (design §3.2).
    // A returning branch alongside --run-block/--evalcase, for the same reason
    // --dump-registry documents: falling through would reach runRealFrontend(),
    // which hangs headless. Arc ② is what spawns this; landing the entry point
    // (and its spec schema) first means that arc only has to add the fork/exec
    // half, against a child that already exists and can be driven by hand.
    if (std::strcmp(argv[i], "--parallel-worker") == 0) {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "[piet_native] --parallel-worker requires a task-spec path\n");
        return 2;
      }
      return piet::parallel::runParallelWorker(argv[i + 1]);
    }
    if (std::strcmp(argv[i], "--do") == 0) {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "[piet_native] --do requires a .do file path\n");
        return 2;
      }
      std::string filepath = argv[i + 1];
      // Everything after the file path is a do-file arg (e.g. `no`/`yes`).
      std::vector<std::string> doArgs;
      for (int j = i + 2; j < argc; ++j) doArgs.push_back(argv[j]);
      return runDoFileHeadless(filepath, doArgs);
    }
    if (std::strcmp(argv[i], "--demo") == 0) {
      demo = true;
    }
  }
  if (demo) {
    runDemoPage();
  } else {
    runRealFrontend();
  }
  return 0;
}
