// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Bridge route handlers — one function per `window.pietNative.request(path,
// body)` routing key. M0 shipped 2 demo routes (echo, cobra.version); M4b
// (ROADMAP §2.0.0 "M4b — frontend swap + graph rendering + run-cancel")
// wires the FULL CONTRACT.md Boundary-B surface onto the real dispatcher/
// doexec/SessionState machinery M2/M3/M4a already built — endpoint NAMES
// are preserved as routing keys (Master Plan Boundary-B design: "endpoint
// identity is preserved across the transport swap"), just stripped of the
// leading "/api/" and using "." instead of "/" for path segments (e.g.
// POST /api/label/variable -> routing key "label.variable") so the JS-side
// api.js swap is a mechanical string-literal change, not a redesign.
//
// Every route here binds to ONE shared, persistent SessionState (owned by
// main.cpp's AppState, constructed once when the window opens) rather than
// run_block.cpp's per-invocation fresh session — this is the live
// interactive kernel, not the headless gate harness.
#ifndef PIET_NATIVE_ROUTES_H
#define PIET_NATIVE_ROUTES_H

#include <optional>
#include <string>

#include "piet/session_state.h"
#include "piet/shell.h"

namespace piet::routes {

// Routing key "echo" — M0 demo, kept for the bridge self-test.
BridgeResult echo(const std::string& body);

// Routing key "cobra.version" — M0 demo (raw cobra binary probe). Superseded
// for user-facing version display by "version" (below), which returns the
// full CONTRACT.md 4-tuple; kept as-is for the M0 selftest's bridge-echo
// proof and for anything that wants the raw cobra JSON.
BridgeResult cobraVersion(const std::string& body);

// -- M4b: the full Boundary-B surface, one handler per CONTRACT.md endpoint,
// each closing over the shared SessionState& (bound via a lambda in
// main.cpp, mirroring how M0's progress.start closed over Shell&). ---------

// "command" — POST /api/command: {cmd} -> {blocks, varlist_after?}
BridgeResult command(SessionState& session, const std::string& body);

// "do_string" — POST /api/do_string: {source, args?} -> same shape as "do".
BridgeResult doString(SessionState& session, const std::string& body);

// "run_progress" — GET /api/run_progress (no body): -> RunProgress dict.
BridgeResult runProgress(SessionState& session, const std::string& body);

// "run_cancel" — POST /api/run_cancel ({}): -> {cancelled: bool}.
BridgeResult runCancel(SessionState& session, const std::string& body);

// "open" — POST /api/open: {filepath, format?} -> DataMutationResult
// (action:"open") or CommandErrorResult.
BridgeResult open(SessionState& session, const std::string& body);

// "varlist" — GET /api/varlist (no body): -> {varlist, n_rows, n_cols}.
BridgeResult varlist(SessionState& session, const std::string& body);

// "browse" — GET /api/browse: {offset?, limit?, vars?} (query params
// reshaped into a JSON body by the JS shim, since window.pietNative.request
// only carries one body argument — see api.js's adaptation note) -> BrowseResult.
BridgeResult browse(SessionState& session, const std::string& body);

// "history" — GET /api/history (no body): -> {history: [...]}.
BridgeResult history(SessionState& session, const std::string& body);

// "version" — GET /api/version (no body): -> {piet_version, cobra_version,
// contract_a, contract_b}. The real 4-tuple aggregator (M0's cobraVersion
// only probed the engine).
BridgeResult version(SessionState& session, const std::string& body);

// "rename" — POST /api/rename: {varname, newname} -> DataMutationResult.
BridgeResult rename(SessionState& session, const std::string& body);

// "label.variable" — POST /api/label/variable: {varname, label}.
BridgeResult labelVariable(SessionState& session, const std::string& body);

// "label.define" — POST /api/label/define: {lblname, mapping}.
BridgeResult labelDefine(SessionState& session, const std::string& body);

// "label.values" — POST /api/label/values: {varname, lblname?}.
BridgeResult labelValues(SessionState& session, const std::string& body);

// "sort" — POST /api/sort: {varlist, ascending?}.
BridgeResult sortRoute(SessionState& session, const std::string& body);

// "recode" — POST /api/recode: {varname, rules, gen?}.
BridgeResult recode(SessionState& session, const std::string& body);

// "cell" — POST /api/cell: {varname, row, value} -> DataMutationResult
// (action:"cell_write") — genuinely new engine logic (no prior C++ port);
// implemented directly against Datastore/cobra eval (see routes.cpp).
BridgeResult cell(SessionState& session, const std::string& body);

// "tabulate" — POST /api/tabulate: {varlist, chi2?}.
BridgeResult tabulate(SessionState& session, const std::string& body);

// "describe" — POST /api/describe: {varlist?}.
BridgeResult describe(SessionState& session, const std::string& body);

// "summarize" — POST /api/summarize: {varlist?, detail?}.
BridgeResult summarize(SessionState& session, const std::string& body);

// "legal.notices" (WI-5, legal remediation 2026-07-08) — app-shell service
// route (no SessionState; same classification as "dialog.open": a shell
// facility, not a Boundary-B kernel endpoint — the maintainer classifies at merge).
// Returns { text: string, source: "bundle"|"dev" }: the
// THIRD_PARTY_NOTICES.txt generated into the packaged bundle's Resources/
// by package_native_app.sh, or (dev runs) the same content concatenated
// live from the checked-in piet/packaging/licenses/ manifest. Backs the
// frontend's "About / Legal" acknowledgements panel.
BridgeResult legalNotices(const std::string& body);

// "settings.get" / "settings.set" (2.2.0 item D) — app-shell config routes
// (no SessionState; same classification family as dialog.open/legal.notices —
// NOT a Boundary-B kernel endpoint, contract_B unchanged). settings.get
// returns the persisted engine/scratch settings + detected system info (RAM/
// cores/recommended budget); settings.set merges a partial, re-persists, and
// applies the compute-cores cap live. Implemented in settings.cpp (kept as
// free functions so selftest.cpp can exercise them without a window).
BridgeResult settingsGet(const std::string& body);
BridgeResult settingsSet(const std::string& body);

// "var_detail" — GET /api/var_detail?name=X (v2.4.0, N7): single-column
// stats {name, dtype, format, label, N, n_missing, mean?, sd?, min?, max?}
// (numeric moments omitted for string/factor cols). Unknown name → engine
// error. O(rows) single-column, never a full-frame scan.
BridgeResult varDetail(SessionState& session, const std::string& body);

// N1 (P8.1-W2): core writer for POST /api/file/save once a destination PATH
// is resolved (the body's explicit `path`, or the NSSavePanel result). Kept
// Shell-free so selftest exercises the decode+write contract headlessly; the
// main.cpp `file.save` bind() lambda handles the no-`path` save-panel branch
// and then calls this. Exactly one of `text`/`contentB64`; a decode/IO
// failure is an honest {type:"error"}, never a false {ok:true}.
BridgeResult fileSaveToPath(const std::string& path, const std::string& kind,
                            const std::optional<std::string>& text,
                            const std::optional<std::string>& contentB64);

}  // namespace piet::routes

#endif  // PIET_NATIVE_ROUTES_H
