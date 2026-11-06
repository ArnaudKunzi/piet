// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Implementation of piet::Shell. This is the ONLY translation unit in the
// project allowed to #include the vendored webview.h — see shell.h's header
// comment for why.
#include "piet/shell.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <stdexcept>
#include <utility>

#include "bridge_internal.h"

// WEBVIEW_HEADER is intentionally NOT defined: this .cpp is the single
// implementation unit, so we want the vendored header's inline definitions
// compiled here (header-only mode). See vendor/webview/webview.h's own
// WEBVIEW_HEADER guard (~line 189) — leaving it undefined means the .cpp
// portion of the file (the browser_engine/webview classes) is compiled in,
// not just declarations.
#include "vendor/webview/webview.h"

#ifdef _WIN32
// IDI_PIET — generated into the build tree from resources/windows/resource.h
// by the packaging layer's RC step (piet/native/CMakeLists.txt's WIN32 branch
// puts that directory on the include path).
#include "resource.h"
#endif

namespace piet {

struct Shell::Impl {
  webview::webview view;
  std::map<std::string, RouteHandler> routes;
  std::map<std::string, AsyncRouteHandler> asyncRoutes;

  explicit Impl(bool debug) : view(debug, nullptr) {}
};

namespace {

// Minimal ad-hoc JSON string escaper for building small JS snippets (window
// title, event payload wrapping). We do NOT hand-roll JSON *parsing*
// anywhere in shell.cpp — route handlers own their own JSON via nlohmann/json
// (vendor/nlohmann/json.hpp) in bridge.cpp/main.cpp. This is purely for
// embedding a C++ string as a JS string literal safely.
std::string jsStringLiteral(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 2);
  out.push_back('"');
  for (char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      default: out.push_back(c);
    }
  }
  out.push_back('"');
  return out;
}

#ifdef __APPLE__
// WS-N2 (2026-07-08): WKWebView installs no application menu bar on its own,
// so macOS never wires up the standard Cmd+C/V/X/A/Z text-editing shortcuts
// — those key equivalents are dispatched by NSApplication walking the menu
// bar for a matching item, not by the responder chain alone. Without an Edit
// menu exposing the stock selectors (cut:/copy:/paste:/selectAll:/undo:/
// redo:), Cmd+V is a silent no-op in every text input in the packaged .app
// (do-editor textarea, command input) even though nothing on the JS side
// blocks paste — this was verified separately; the fix is purely native-side.
//
// Builds the minimal standard menu bar: an unlabeled App menu (macOS
// substitutes the running process's name for the first menu regardless of
// its own title — no About/Preferences needed for this shell, just Quit) and
// an Edit menu with the six standard items, each targeting nil (first
// responder) with the exact stock AppKit selectors so whatever text control
// currently has focus handles them normally. Uses the same raw objc_msgSend
// idiom as the rest of the vendored webview.h WEBVIEW_COCOA backend (see
// webview::detail::objc::msg_send and the _cls/_sel/_str literal operators
// above it in vendor/webview/webview.h) instead of Objective-C syntax, so
// this stays plain C++ compiled-as-objective-c++ with no new language
// surface beyond what the vendored header already requires.
//
// Must run before Shell::run() calls the underlying webview::run() (which
// blocks pumping NSApp's run loop) — see the call site below.
void installStandardMainMenu() {
  using namespace webview::detail;  // brings in objc::msg_send + _cls/_sel/_str

  // Key-equivalent modifier flags this file needs (NSEvent.h's
  // NSEventModifierFlagCommand/Shift bit positions). Declared by hand rather
  // than pulling in a framework header, matching how webview.h's own
  // WEBVIEW_COCOA section declares just the AppKit enum values it needs
  // (e.g. NSWindowStyleMask, NSApplicationActivationPolicy above it) without
  // ever #including <AppKit/AppKit.h>. addItemWithTitle:action:keyEquivalent:
  // defaults new items to Command-only, so this is only needed to ADD Shift
  // for Redo (setKeyEquivalentModifierMask: replaces the mask, not merges).
  constexpr NSUInteger kCommandShiftMask = (1 << 20) | (1 << 17);

  id menubar = objc::msg_send<id>(objc::msg_send<id>("NSMenu"_cls, "alloc"_sel), "init"_sel);

  // App menu: one unlabeled top-level item (macOS shows the process name
  // here regardless) containing just Quit.
  id appMenuItem =
      objc::msg_send<id>(objc::msg_send<id>("NSMenuItem"_cls, "alloc"_sel), "init"_sel);
  objc::msg_send<void>(menubar, "addItem:"_sel, appMenuItem);
  id appMenu = objc::msg_send<id>(objc::msg_send<id>("NSMenu"_cls, "alloc"_sel), "init"_sel);
  objc::msg_send<void>(appMenuItem, "setSubmenu:"_sel, appMenu);
  objc::msg_send<id>(appMenu, "addItemWithTitle:action:keyEquivalent:"_sel, "Quit"_str,
                     "terminate:"_sel, "q"_str);

  // Edit menu: Undo, Redo, separator, Cut, Copy, Paste, Select All — the
  // standard six (seven with the separator) every Cocoa app ships.
  id editMenuItem =
      objc::msg_send<id>(objc::msg_send<id>("NSMenuItem"_cls, "alloc"_sel), "init"_sel);
  objc::msg_send<void>(menubar, "addItem:"_sel, editMenuItem);
  id editMenu = objc::msg_send<id>(objc::msg_send<id>("NSMenu"_cls, "alloc"_sel),
                                   "initWithTitle:"_sel, "Edit"_str);
  objc::msg_send<void>(editMenuItem, "setSubmenu:"_sel, editMenu);

  objc::msg_send<id>(editMenu, "addItemWithTitle:action:keyEquivalent:"_sel, "Undo"_str,
                     "undo:"_sel, "z"_str);
  id redoItem = objc::msg_send<id>(editMenu, "addItemWithTitle:action:keyEquivalent:"_sel,
                                   "Redo"_str, "redo:"_sel, "z"_str);
  objc::msg_send<void>(redoItem, "setKeyEquivalentModifierMask:"_sel, kCommandShiftMask);
  objc::msg_send<void>(editMenu, "addItem:"_sel,
                       objc::msg_send<id>("NSMenuItem"_cls, "separatorItem"_sel));
  objc::msg_send<id>(editMenu, "addItemWithTitle:action:keyEquivalent:"_sel, "Cut"_str,
                     "cut:"_sel, "x"_str);
  objc::msg_send<id>(editMenu, "addItemWithTitle:action:keyEquivalent:"_sel, "Copy"_str,
                     "copy:"_sel, "c"_str);
  objc::msg_send<id>(editMenu, "addItemWithTitle:action:keyEquivalent:"_sel, "Paste"_str,
                     "paste:"_sel, "v"_str);
  objc::msg_send<id>(editMenu, "addItemWithTitle:action:keyEquivalent:"_sel, "Select All"_str,
                     "selectAll:"_sel, "a"_str);

  id app = objc::msg_send<id>("NSApplication"_cls, "sharedApplication"_sel);
  objc::msg_send<void>(app, "setMainMenu:"_sel, menubar);
}

// S6 (OVERHEAD-ZERO campaign) — drag-and-drop file open. Two known traps,
// both handled here + in app.js's dragover/drop listeners (see app.js):
//
//   1. WKWebView will navigate away. If nothing preventDefault()s the DOM
//      `dragover`/`drop` events, WebKit's own default action for a dropped
//      file is to navigate the whole window to it. That fix is JS-side
//      (app.js) — this override does NOT replace WKWebView's own drag
//      handling, it chains to it (see below), so the DOM events still
//      reach the page for app.js to preventDefault().
//   2. The webview cannot see real paths. Even with the DOM `drop` event
//      firing normally, the File/DataTransfer JS receives is a sandboxed,
//      opaque reference — WKWebView never exposes a real filesystem path to
//      page script. The ONLY place the real path exists is the AppKit drag
//      pasteboard, one level below WebKit's own event synthesis — native
//      code only.
//
// Fix: add performDragOperation: to the SAME runtime-created WKWebView
// subclass ("PietFirstMouseWKWebView") the acceptsFirstMouse: patch above
// already installs (first_mouse_webview_class(), webview.h) —
// class_addMethod works on an already-registered class from any
// translation unit, so this needs no change to the vendored header. The
// override reads the real path off the drag pasteboard and pushes it to JS
// over the EXISTING pietEvents channel (Shell::postEvent — the same push
// mechanism run-progress ticks and request_dialog already use), tagged
// {"kind":"file_dropped"} so app.js's initPietEventsBridge() can route it
// into onOpenFile(path) — the SAME knownPath leg the Recent-files reopen
// already uses, i.e. the SAME /api/open -> handlers::runUse() path the
// `use` command itself runs. It then chains to WKWebView's OWN original
// implementation (captured via class_getMethodImplementation BEFORE this
// method existed on the subclass, so it's genuinely the superclass's
// behavior, not a recursive call into this same override) so nothing else
// about drag-and-drop changes.
void installFileDropHandler(Shell* shell, void* nsWindowPtr) {
  using namespace webview::detail;  // objc::msg_send + _cls/_sel/_str

  id nsWindow = (id)nsWindowPtr;
  if (!nsWindow) return;
  id webviewId = objc::msg_send<id>(nsWindow, "contentView"_sel);
  if (!webviewId) return;

  // Stash `shell` on the actual webview instance so the (captureless, C-
  // function-pointer) IMP below can reach Shell::postEvent() — same
  // objc_setAssociatedObject idiom webview.h's own
  // create_script_message_handler() uses to reach its owning
  // cocoa_wkwebview_engine*.
  objc_setAssociatedObject(webviewId, "pietFileDropShell", (id)shell,
                           OBJC_ASSOCIATION_ASSIGN);

  Class cls = object_getClass(webviewId);
  class_addMethod(
      cls, "performDragOperation:"_sel,
      (IMP)(+[](id self, SEL cmd, id sender) -> BOOL {
        id pasteboard = objc::msg_send<id>(sender, "draggingPasteboard"_sel);
        id urlClasses =
            objc::msg_send<id>("NSArray"_cls, "arrayWithObject:"_sel, "NSURL"_cls);
        id urls = objc::msg_send<id>(pasteboard, "readObjectsForClasses:options:"_sel,
                                     urlClasses, (id)nullptr);
        id url = objc::msg_send<id>(urls, "firstObject"_sel);
        // isFileURL guards against a non-file drag (e.g. a dragged hyperlink)
        // landing here with a garbage "path" (NSURL.path on a web URL is
        // just its URL path component, not a filesystem path).
        BOOL isFile = url && objc::msg_send<BOOL>(url, "isFileURL"_sel);
        id nsPath = isFile ? objc::msg_send<id>(url, "path"_sel) : nullptr;
        auto cpath =
            nsPath ? objc::msg_send<const char*>(nsPath, "UTF8String"_sel) : nullptr;
        if (cpath) {
          auto* pietShell = (Shell*)objc_getAssociatedObject(self, "pietFileDropShell");
          if (pietShell) {
            // Shared with the Windows producer (bridge_internal.cpp) so both
            // platforms emit the exact same ordered Boundary-B shape and path
            // escaping. shell.cpp still stays nlohmann-free — the serializer
            // is declared in bridge_internal.h and defined next to the other
            // shim sources.
            pietShell->postEvent(piet::internal::fileDroppedEventJson(cpath));
          }
        }
        auto orig = (BOOL(*)(id, SEL, id))class_getMethodImplementation(
            (Class) "WKWebView"_cls, "performDragOperation:"_sel);
        return orig ? orig(self, cmd, sender) : YES;
      }),
      "c@:@");
}
#endif  // __APPLE__

}  // namespace

Shell::Shell(bool debug) : impl_(new Impl(debug)) {
#ifdef _WIN32
  // The PE resource supplies Explorer/shortcut branding; set the already-
  // created top-level WebView2 HWND as well so the same icon appears in the
  // title bar, Alt-Tab switcher, and taskbar.
  // window() is null when the WebView2 runtime is missing (the engine's
  // constructor bails early); there is no window to brand in that case.
  HWND hwnd = static_cast<HWND>(impl_->view.window());
  if (!hwnd) return;
  HINSTANCE module = GetModuleHandleW(nullptr);
  // NOT named `small`/`large`: <rpcndr.h> (reached via objbase.h) #defines
  // `small` to `char`, which would silently mangle a variable of that name.
  HICON iconBig = static_cast<HICON>(
      LoadImageW(module, MAKEINTRESOURCEW(IDI_PIET), IMAGE_ICON,
                 GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_SHARED));
  HICON iconSmall = static_cast<HICON>(
      LoadImageW(module, MAKEINTRESOURCEW(IDI_PIET), IMAGE_ICON,
                 GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));
  if (iconBig) SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(iconBig));
  if (iconSmall) SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(iconSmall));
#endif
}

Shell::~Shell() { delete impl_; }

void Shell::setTitle(const std::string& title) { impl_->view.set_title(title); }

void Shell::setSize(int width, int height, bool resizable) {
  // WEBVIEW_HINT_NONE = 0 (fixed-but-user-resizable per platform default),
  // WEBVIEW_HINT_FIXED = 3 disables resizing entirely.
  impl_->view.set_size(width, height, resizable ? WEBVIEW_HINT_NONE : WEBVIEW_HINT_FIXED);
}

void Shell::navigate(const std::string& url) { impl_->view.navigate(url); }

void Shell::setVirtualHostMapping(const std::string& host, const std::string& folder) {
#ifdef _WIN32
  // WEBVIEW_EDGE backend only — see the WP-W4 patch in vendor/webview/webview.h.
  // WS-N2 (2026-07-08): a failed mapping means the packaged frontend has no
  // folder to serve over https://<host>/ — main.cpp's navigateFrontend()
  // would then navigate a URL that resolves to nothing (the first Windows
  // canary's silent dead app: ERR_NAME_NOT_RESOLVED with no diagnostic).
  // set_virtual_host_mapping() now returns false and has already printed
  // which step failed (see its patch comment in webview.h); fail loud and
  // immediately here instead of limping into a blank/DNS-error window.
  if (!impl_->view.set_virtual_host_mapping(host, folder)) {
    std::fprintf(stderr,
                 "FATAL: failed to map virtual host '%s' to folder '%s' "
                 "(see the webview: diagnostic above for which step failed) "
                 "- the packaged frontend cannot be served; refusing to "
                 "start with a dead window.\n",
                 host.c_str(), folder.c_str());
    std::exit(1);
  }
#else
  // macOS/Linux load the frontend over file:// directly (navigateFrontend in
  // main.cpp); no host mapping. Referenced only inside main.cpp's _WIN32
  // branch, so this body never runs off-Windows — kept as a no-op so the
  // Shell interface stays platform-uniform.
  (void)host;
  (void)folder;
#endif
}

void Shell::initJs(const std::string& js) { impl_->view.init(js); }

// BUG-1469: the vendored GTK backend (Linux) has no native file panels, so the
// two forwarders below compiled only on macOS and Windows and piet/native did
// not build on Linux. There they answer "" -- the routes' cancelled dialog --
// until the GTK backend grows the panels.
std::string Shell::openFileDialog(const std::string& exts) {
#if defined(__APPLE__) || defined(_WIN32)
  return impl_->view.show_open_dialog(exts);
#else
  (void)exts;
  return std::string();
#endif
}

// N1 (P8.1-W2): thin forwarder to the vendored backend's native save panel,
// beside openFileDialog and with the same UI-thread contract (see shell.h).
std::string Shell::saveFileDialog(const std::string& suggestedName, const std::string& ext) {
#if defined(__APPLE__) || defined(_WIN32)
  return impl_->view.show_save_dialog(suggestedName, ext);
#else
  (void)suggestedName;
  (void)ext;
  return std::string();
#endif
}

// A6-01 (stabilization W3): shell.h documents evalJs/postEvent as "safe from
// any thread, marshalled onto the UI thread" — but this body used to call
// view.eval() directly, so the do-run worker thread's ~100ms RunProgress
// postEvent()s drove WKWebView/WebView2 JS evaluation off the UI thread.
// dispatch() queues the closure onto the webview's own main loop, making the
// implementation match the documented contract. (webview::dispatch is safe
// both before run() — queued until the loop starts — and from the UI thread
// itself.)
void Shell::evalJs(const std::string& js) {
  impl_->view.dispatch([impl = impl_, js] { impl->view.eval(js); });
}

void Shell::postEvent(const std::string& jsonPayload) {
  // Dispatch a CustomEvent carrying the (already-serialized) JSON payload as
  // `detail`. We parse-then-rewrap in JS (JSON.parse) rather than trying to
  // splice raw JSON into the JS source, so the payload's own quoting/escaping
  // is handled by the JS engine's own JSON parser instead of our C++ escaper.
  std::string js = "window.__pietDispatchEvent(" + jsStringLiteral(jsonPayload) + ")";
  evalJs(js);
}

void Shell::installBridge() {
  // 1) Install the pietEvents push-channel shim ahead of any page load. This
  //    defines a tiny EventTarget-backed object at window.pietEvents with a
  //    single method JS code uses to subscribe: pietEvents.addEventListener
  //    ("piet-event", handler). window.__pietDispatchEvent is the internal
  //    entry point postEvent() calls via eval(). Source text lives in
  //    bridge_internal.cpp so selftest.cpp can run the SAME string through a
  //    real (headless, Node) JS engine rather than a paraphrased copy.
  initJs(piet::internal::pietEventsShimJs());

#ifdef _WIN32
  // WebView2 exposes a dropped DOM File's real path only through
  // postMessageWithAdditionalObjects -> ICoreWebView2File. Keep that
  // platform transport behind Shell and convert it immediately into the
  // same file_dropped pietEvents payload macOS emits above.
  impl_->view.set_file_message_handler(
      [this](const std::string& tag, const std::string& path) -> bool {
        if (tag != piet::internal::kPietFileDropMessageTag || path.empty()) {
          return false;
        }
        postEvent(piet::internal::fileDroppedEventJson(path));
        return true;
      });
  initJs(piet::internal::pietWindowsFileDropShimJs());
#endif

  // 2) Install window.pietNative.request(path, body) as a thin wrapper over
  //    ONE underlying webview bind(), `pietNativeInvoke(path, bodyJson)` ->
  //    Promise<string>. Fan-out to per-path handlers happens on the C++ side
  //    in the bind callback below, so JS always sees a single stable entry
  //    point regardless of how many routes M1+ registers.
  initJs(piet::internal::pietNativeRequestShimJs());

  impl_->view.bind("pietNativeInvoke",
                    [this](const std::string& seq, const std::string& req, void*) {
                      // req is a JSON array-of-strings: ["path", "bodyJson"]
                      // (webview's bind() protocol packs JS call arguments as
                      // a JSON array string). We do a tiny hand-rolled split
                      // here rather than pulling nlohmann/json into shell.cpp
                      // (kept dependency-free per the header comment); the
                      // route handlers themselves use nlohmann/json for the
                      // real payload.
                      // req looks like: ["echo","{\"msg\":\"hi\"}"] (webview's
                      // bind() protocol packs JS call arguments as a JSON
                      // array string). Parsing lives in bridge.cpp (uses
                      // nlohmann/json) to keep shell.cpp's only vendored
                      // dependency the webview header itself.
                      std::string path, bodyJson;
                      std::tie(path, bodyJson) = piet::internal::parseInvokeArgs(req);

                      // M4b: async routes (do/do_string) — the handler owns
                      // resolving this seq itself, from whatever worker
                      // thread it spawns; we do NOT call resolve() here (that
                      // would race the async handler's own eventual resolve
                      // call, and webview only expects resolve() ONCE per
                      // seq).
                      auto asyncIt = impl_->asyncRoutes.find(path);
                      if (asyncIt != impl_->asyncRoutes.end()) {
                        try {
                          asyncIt->second(this, seq, bodyJson);
                        } catch (const std::exception& e) {
                          std::string msg = e.what();
                          impl_->view.resolve(seq, 1,
                                               "{\"code\":\"handler_exception\",\"message\":" +
                                                   jsStringLiteral(msg) + "}");
                        }
                        return;
                      }

                      auto it = impl_->routes.find(path);
                      BridgeResult result;
                      if (it == impl_->routes.end()) {
                        result = BridgeResult::failure(
                            R"({"code":"unknown_route","message":"no handler for path"})");
                      } else {
                        try {
                          result = it->second(bodyJson);
                        } catch (const std::exception& e) {
                          std::string msg = e.what();
                          result = BridgeResult::failure(
                              "{\"code\":\"handler_exception\",\"message\":" +
                              jsStringLiteral(msg) + "}");
                        }
                      }
                      impl_->view.resolve(seq, result.ok ? 0 : 1, result.json);
                    },
                    nullptr);
}

void Shell::bindRoute(const std::string& path, RouteHandler handler) {
  impl_->routes[path] = std::move(handler);
}

void Shell::bindAsyncRoute(const std::string& path, AsyncRouteHandler handler) {
  impl_->asyncRoutes[path] = std::move(handler);
}

void Shell::resolveInvoke(const std::string& seq, bool ok, const std::string& json) {
  // webview's resolve() is documented safe from any thread (dispatches onto
  // the UI thread's run loop internally — see shell.h's threading note);
  // this is the SAME call the synchronous bind() callback above uses, just
  // exposed for an async handler's worker thread to call once, later.
  impl_->view.resolve(seq, ok ? 0 : 1, json);
}

void Shell::run() {
#ifdef __APPLE__
  // Must happen before the underlying webview::run() below starts pumping
  // NSApp's run loop — see installStandardMainMenu()'s doc comment.
  installStandardMainMenu();
  // S6 (OVERHEAD-ZERO, drag-and-drop file open): not "must" for the same
  // reason (a drop can't physically happen before the window is on screen)
  // but IS safe this early — impl_->view's NSWindow/WKWebView are already
  // fully constructed by the time Shell's own constructor returns
  // (cocoa_wkwebview_engine sets both up synchronously via a nested
  // run-loop trick — see on_application_did_finish_launching in
  // webview.h). See installFileDropHandler()'s doc comment above for the
  // full design.
  installFileDropHandler(this, impl_->view.window());
#endif
  impl_->view.run();
}

void Shell::terminate() { impl_->view.terminate(); }

void Shell::terminateFromAnyThread() {
  // dispatch() queues the closure onto the platform UI thread's run loop
  // (dispatch_async on the main GCD queue on macOS — see the vendored
  // webview.h cocoa_wkwebview_engine::dispatch), so the actual terminate()
  // executes on the UI thread regardless of the calling thread. `this` is
  // safe to capture: the Shell owner joins/awaits the calling thread before
  // destroying the Shell (see main.cpp runUiSmoke's terminator join).
  impl_->view.dispatch([this]() { impl_->view.terminate(); });
}

}  // namespace piet
