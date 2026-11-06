// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Internal helper shared between shell.cpp and bridge.cpp. Not part of the
// public piet/shell.h surface — deliberately kept out of include/piet/ so it
// never leaks to demo/main consumers.
#ifndef PIET_NATIVE_BRIDGE_INTERNAL_H
#define PIET_NATIVE_BRIDGE_INTERNAL_H

#include <string>
#include <utility>

namespace piet::internal {

// Reserved WebView2 message tag for the Windows file-drop carrier. This is
// transport-private: the public Boundary-B payload remains the
// {"kind":"file_dropped","path":...} pietEvents push built below.
inline constexpr char kPietFileDropMessageTag[] = "piet:file-drop:v1";

// Parses the webview bind() request string, a JSON array of the JS call's
// arguments serialized as text — for `pietNativeInvoke(path, bodyJson)` this
// is always a 2-element array of strings: ["path","bodyJson"]. Returns
// {path, bodyJson}; on any parse failure returns {"", "{}"} rather than
// throwing (a malformed bridge call should surface as unknown_route, not
// crash the shell).
std::pair<std::string, std::string> parseInvokeArgs(const std::string& reqJsonArray);

// The two JS shim snippets Shell::installBridge() injects via init(). Pulled
// out as free functions (rather than inlined string literals in shell.cpp)
// so selftest.cpp can feed the EXACT same source text to a real JS engine
// (Node, headless) instead of duplicating/paraphrasing it — see
// selftest.cpp's testBridgeEcho() for how this is used.
std::string pietEventsShimJs();
std::string pietNativeRequestShimJs();

// Windows/WebView2 document-start shim: forwards the first dropped DOM File as
// an AdditionalObject under kPietFileDropMessageTag. The native WebView2
// handler is the only layer that can turn that File into an absolute path.
// Kept here so headless tests can execute the exact injected source.
std::string pietWindowsFileDropShimJs();

// Exact Boundary-B serializer shared by the macOS pasteboard producer and the
// Windows WebView2 AdditionalObjects producer. ordered_json preserves the
// documented key order while handling every path escape correctly.
std::string fileDroppedEventJson(const std::string& path);

}  // namespace piet::internal

#endif  // PIET_NATIVE_BRIDGE_INTERNAL_H
