// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "bridge_internal.h"

#include "vendor/nlohmann/json.hpp"

namespace piet::internal {

std::pair<std::string, std::string> parseInvokeArgs(const std::string& reqJsonArray) {
  try {
    auto arr = nlohmann::json::parse(reqJsonArray);
    if (arr.is_array() && arr.size() >= 2 && arr[0].is_string() && arr[1].is_string()) {
      return {arr[0].get<std::string>(), arr[1].get<std::string>()};
    }
  } catch (const nlohmann::json::exception&) {
    // fall through to the empty-path sentinel below
  }
  return {"", "{}"};
}

std::string pietEventsShimJs() {
  return R"JS(
    (function() {
      window.pietEvents = new EventTarget();
      window.__pietDispatchEvent = function(jsonText) {
        var detail;
        try { detail = JSON.parse(jsonText); } catch (e) {
          detail = { parse_error: String(e), raw: jsonText };
        }
        window.pietEvents.dispatchEvent(new CustomEvent('piet-event', { detail: detail }));
      };
    })();
  )JS";
}

std::string pietNativeRequestShimJs() {
  return R"JS(
    (function() {
      window.pietNative = {
        request: function(path, body) {
          var bodyJson = JSON.stringify(body === undefined ? null : body);
          // window.pietNativeInvoke is the webview library's bound function.
          // The C++ side resolves it by EVAL-splicing the route's JSON result
          // straight into JS as an expression (see vendored webview.h
          // resolve(): eval("...resolve(" + result + ")")), so the promise
          // already delivers a PARSED JS value — NOT a JSON string. Re-parsing
          // it here would String()-coerce the object to "[object Object]" and
          // throw "JSON Parse error: Unexpected identifier object" on every
          // request (which is exactly what pinned the kernel-dot red and broke
          // the whole webview UI). A failed route rejects with the parsed
          // {code,message} error object, which api.js's _nativeRequest() catch
          // normalises into an ApiError — so return the promise unwrapped.
          return window.pietNativeInvoke(path, bodyJson);
        }
      };
    })();
  )JS";
}

// WebView2 hands page script only an opaque DOM File for a dropped item — the
// real path lives on the native side of postMessageWithAdditionalObjects (see
// shell.cpp's set_file_message_handler wiring). This shim is the page half of
// that carrier: it re-posts the first dropped File under the reserved tag and
// does nothing at all on a runtime that predates the API (fail closed, never a
// bogus path). The tag is spliced in as JSON so the literal can never break the
// surrounding JS.
std::string pietWindowsFileDropShimJs() {
  const std::string tagJson = nlohmann::json(kPietFileDropMessageTag).dump();
  return std::string(R"JS(
    (function() {
      window.addEventListener('drop', function(event) {
        var transfer = event && event.dataTransfer;
        var file = transfer && transfer.files && transfer.files[0];
        var webview = window.chrome && window.chrome.webview;
        if (!file || !webview ||
            typeof webview.postMessageWithAdditionalObjects !== 'function') {
          return;
        }
        webview.postMessageWithAdditionalObjects()JS") +
         tagJson + R"JS(, [file]);
      }, true);
    })();
  )JS";
}

// Both platform producers funnel through here so the Boundary-B payload is
// byte-identical on macOS and Windows: ordered_json fixes the documented
// {"kind":...,"path":...} key order, and nlohmann's serializer owns every
// escape a filesystem path can carry (backslashes, quotes, UTF-8).
std::string fileDroppedEventJson(const std::string& path) {
  nlohmann::ordered_json event = nlohmann::ordered_json::object();
  event["kind"] = "file_dropped";
  event["path"] = path;
  return event.dump();
}

}  // namespace piet::internal
