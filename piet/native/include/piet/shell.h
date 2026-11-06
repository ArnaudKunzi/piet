// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// piet::Shell — thin wrapper around the vendored webview/webview single-header
// library (piet/native/vendor/webview/webview.h, pinned 0.10.0).
//
// Why this exists (M0 design note, see docs/piet/ROADMAP.md "2.0.0 — C++ core"):
// the webview library exposes a raw C++ class with a large surface (navigate,
// bind, eval, dispatch, run, ...). We only need a small slice of it, and we
// never want `#include "webview.h"` to leak outside this translation unit —
// that caps blast radius if a platform quirk in the vendored lib bites, and
// gives Phase D (Windows/Linux port) exactly one seam to touch. Everything in
// piet/native/src/ talks to `piet::Shell`, never to `webview::webview`
// directly.
//
// Threading model (read this before wiring more bindings in M1+):
//   - `run()` blocks the calling thread and pumps the platform's native GUI
//     event loop (NSApp/GCD on macOS, GTK main loop on Linux, a Win32 message
//     loop on Windows). It must be called from the process's main thread —
//     WKWebView/AppKit assert on this.
//   - `bind()` callbacks execute ON THE UI THREAD's dispatch queue (the
//     underlying lib marshals `on_message` back through the platform's
//     event loop before invoking your std::function). So a bound handler
//     must not block the UI thread for long: for anything that does real
//     work (running cobra, executing a do-file), spawn a worker thread from
//     inside the handler and call `resolve()`/`postEvent()` asynchronously
//     once the work completes. M0's `echo` handler is synchronous (trivial
//     JSON round-trip, sub-millisecond) so it resolves inline; M0's progress
//     demo spawns a real background thread and calls `postEvent()` from it.
//   - `postEvent()` (our pietEvents push channel) is safe to call from ANY
//     thread: internally it calls the underlying `dispatch()`, which queues
//     the JS injection onto the UI thread's run loop. This is the pattern
//     M2+ must reuse for real run-progress ticks from the do-executor.
//   - `evalJs()` is fire-and-forget (no return value) — same threading rule
///     as postEvent: safe from any thread, marshalled onto the UI thread.
//
// None of this is exercised by --selftest's UI (there is none — selftest
// never calls run()); selftest calls the same handler functions directly and
// drives the background-thread progress path with a real std::thread + join,
// so the concurrency behavior IS covered headlessly.
#ifndef PIET_NATIVE_SHELL_H
#define PIET_NATIVE_SHELL_H

#include <functional>
#include <string>

namespace piet {

// Result of a bridge request handler: either a JSON success payload or a
// JSON error payload (webview's bind() protocol: resolve(seq, status, json),
// status==0 -> promise resolves, status!=0 -> promise rejects).
struct BridgeResult {
  bool ok = true;
  std::string json;  // must be a valid JSON value (object/array/string/etc.)

  static BridgeResult success(std::string body) { return BridgeResult{true, std::move(body)}; }
  static BridgeResult failure(std::string body) { return BridgeResult{false, std::move(body)}; }
};

// A handler for one `window.pietNative.request(path, body)` routing key.
// `body` is the raw JSON text the JS side passed; the handler parses it,
// does its work, and returns a BridgeResult. Handlers registered via
// `Shell::bindRoute` run synchronously on the UI thread (see threading note
// above) — long-running handlers must offload internally.
using RouteHandler = std::function<BridgeResult(const std::string& body)>;

// An ASYNC route handler (M4b: do/do_string) — receives the owning Shell*
// and the webview bind() call's opaque `seq` token instead of returning a
// BridgeResult directly. The handler is expected to do its (long-running)
// work on a worker thread it spawns and OWNS (joined before the Shell is
// torn down — see main.cpp's AppState for the thread-lifetime-discipline
// implementation this is designed for), then call `shell->resolveInvoke(seq,
// ok, json)` from that worker thread once done. This lets a single
// `pietNative.request()` call on the JS side stay a normal awaited Promise
// while the C++ side runs the work off the UI thread instead of blocking it
// for a potentially long do-file execution.
using AsyncRouteHandler = std::function<void(class Shell* shell, const std::string& seq,
                                              const std::string& body)>;

// piet::Shell — construct one, register routes, call run(). Non-copyable
// (owns a native window/webview handle).
class Shell {
 public:
  explicit Shell(bool debug = false);
  ~Shell();

  Shell(const Shell&) = delete;
  Shell& operator=(const Shell&) = delete;

  // Window chrome.
  void setTitle(const std::string& title);
  void setSize(int width, int height, bool resizable = true);

  // Load content. `navigate` accepts a URL (file://, http://, or a
  // `data:text/html,...` URI). For M0's demo page we load a data: URI built
  // from an on-disk file so the shell has zero HTTP dependency.
  void navigate(const std::string& url);

  // Windows/WebView2 frontend-loading support (WP-W4). Maps a virtual host
  // name to a local folder so the frontend can be served over an https://
  // origin instead of file:// (Chromium rejects ES-module imports over
  // file://, the same class the macOS webview.h file:// prefs patch fixes on
  // WebKit). No-op on macOS/Linux, where navigate("file://...") is used
  // directly — see main.cpp's navigateFrontend(). Must be called before the
  // matching navigate("https://<host>/...").
  void setVirtualHostMapping(const std::string& host, const std::string& folder);

  // Install the `window.pietNative.request(path, body)` bridge. Must be
  // called before `run()`. Internally this registers ONE webview bind()
  // under the name `pietNativeInvoke`; the JS-side shim (injected via
  // `initJs`, see shell.cpp) fans out by `path` to route handlers registered
  // with `bindRoute`.
  void installBridge();

  // Register a handler for one routing key (e.g. "echo", "cobra.version").
  // Route names are the same strings the Python backend used as REST paths
  // today (minus the leading "/api/"), per the Master Plan Boundary-B design
  // — endpoint identity is preserved across the transport swap.
  void bindRoute(const std::string& path, RouteHandler handler);

  // Register an ASYNC handler for one routing key (M4b: "do"/"do_string").
  // Mutually exclusive with bindRoute for the same path (async is checked
  // first — see shell.cpp's pietNativeInvoke dispatch). See AsyncRouteHandler
  // above for the calling contract.
  void bindAsyncRoute(const std::string& path, AsyncRouteHandler handler);

  // Resolve a PENDING async-route invocation (identified by `seq`, the
  // opaque token webview's bind() protocol handed the C++ side at call
  // time) with a final result. Safe from any thread (internally routes
  // through the same webview resolve() the synchronous bind() callback
  // uses, which itself dispatches onto the UI thread — see shell.h's
  // threading note on postEvent/evalJs for the identical guarantee).
  void resolveInvoke(const std::string& seq, bool ok, const std::string& json);

  // Push an event to `window.pietEvents` listeners in JS (CustomEvent
  // "piet-event" with `detail` = the parsed JSON). Safe from any thread.
  void postEvent(const std::string& jsonPayload);

  // Fire-and-forget JS evaluation. Safe from any thread (marshalled onto the
  // UI thread internally).
  void evalJs(const std::string& js);

  // Inject JS that runs before every page load (used to install the
  // `pietEvents` EventTarget shim ahead of the demo page's own script).
  void initJs(const std::string& js);

  // Show a native file-open panel (Finder) and return the chosen file's
  // absolute path, or "" if cancelled. `exts` is a comma-separated
  // allowed-extension filter (e.g. "dta,csv,hbin"; empty = any). MUST be
  // called on the UI thread — intended to be invoked from a synchronous
  // bind() route handler (runModal spins a nested modal loop, same as the
  // webview's own <input type=file> open-panel). Backs the `dialog.open`
  // route so the frontend's "Open data" / "Run .do" buttons show a real
  // picker instead of a path prompt.
  std::string openFileDialog(const std::string& exts);

  // N1 (P8.1-W2): show a native file-SAVE panel (NSSavePanel / Win32
  // IFileSaveDialog) and return the chosen destination path, or "" if
  // cancelled. `suggestedName` seeds the editable filename; `ext` (single
  // extension, may be empty) constrains the file type. Same UI-thread
  // contract as openFileDialog — invoked from the synchronous `file.save`
  // bind() route handler (runModal spins a nested modal loop). Backs the
  // `file.save` route's no-`path` (interactive "Export…") branch.
  std::string saveFileDialog(const std::string& suggestedName, const std::string& ext);

  // Enter the native run loop. Blocks until the window is closed or
  // terminate() is called. MUST be called from the main thread.
  void run();

  // Ask the run loop to exit. Safe to call from a bind() handler; NOT
  // documented as safe from an arbitrary background thread by the vendored
  // lib, so callers should route through dispatch-safe calls (postEvent's
  // mechanism) if triggering close from a worker thread becomes needed
  // post-M0.
  void terminate();

  // Ask the run loop to exit from ANY thread: marshalled onto the UI thread
  // via the underlying dispatch() (the exact mechanism postEvent/evalJs
  // already rely on), unlike terminate() above which is only documented safe
  // on the UI thread / from a bind() handler. Used by --ui-smoke's
  // watchdog/terminator thread (2.0.2, MI-138 WP-1).
  void terminateFromAnyThread();

 private:
  struct Impl;
  Impl* impl_;
};

}  // namespace piet

#endif  // PIET_NATIVE_SHELL_H
