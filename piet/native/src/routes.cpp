// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Bridge route handlers — M0 shipped echo/cobra.version; M4b (ROADMAP
// §2.0.0 "M4b") wires the full CONTRACT.md Boundary-B surface onto the
// dispatcher/doexec/SessionState/Datastore machinery M2/M3/M4a already
// built. Route-adapter layer only: every handler here parses the JS-side
// JSON body, calls an EXISTING verified handler/primitive, and reshapes the
// result to the exact CONTRACT.md response shape — no new Stata-semantics
// logic beyond what's noted inline (cell-write's f64 path is the one
// genuinely new piece; see below).
#include "piet/routes.h"

#include <dirent.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "base64.h"  // N1 (P8.1-W2): base64Decode for file.save content_b64
#include "piet/bundle_paths.h"
#include "piet/cobra_runner.h"
#include "piet/datastore.h"
#include "piet/dispatcher.h"
#include "piet/doexec.h"
#include "piet/eval/errors.h"
#include "piet/handlers.h"
#include "io/win_compat.h"  // cecon::to_native_path (BUG-050 wide open)
#include "vendor/nlohmann/json.hpp"

namespace piet::routes {

using ojson = nlohmann::ordered_json;

BridgeResult echo(const std::string& body) {
  try {
    auto parsed = nlohmann::json::parse(body);
    nlohmann::json out;
    out["echoed"] = parsed;
    return BridgeResult::success(out.dump());
  } catch (const nlohmann::json::exception& e) {
    nlohmann::json err;
    err["code"] = "bad_json";
    err["message"] = e.what();
    return BridgeResult::failure(err.dump());
  }
}

BridgeResult cobraVersion(const std::string& /*body*/) {
  CobraRunResult run = runCobra({"version", "--json"});

  if (!run.ok) {
    nlohmann::json err;
    err["code"] = "cobra_launch_failed";
    err["message"] = run.error.empty()
                          ? ("cobra exited " + std::to_string(run.exit_code) + ": " + run.stderr_text)
                          : run.error;
    err["resolved_bin"] = resolveCobraBin();
    return BridgeResult::failure(err.dump());
  }

  try {
    auto parsed = nlohmann::json::parse(run.stdout_text);
    return BridgeResult::success(parsed.dump());
  } catch (const nlohmann::json::exception& e) {
    nlohmann::json err;
    err["code"] = "bad_cobra_output";
    err["message"] = e.what();
    err["raw_stdout"] = run.stdout_text;
    return BridgeResult::failure(err.dump());
  }
}

namespace {

// M0 bridge-error-shape unification (ROADMAP carry-forward #2): every
// handler below either returns BridgeResult::success(<200-shaped-JSON>) —
// including CommandErrorResult bodies, since CONTRACT.md's uniform rule is
// "rejections are 200 + error block, never a bare 4xx/reject" — or, for a
// genuine HARNESS-level failure (bad JSON on the wire, no data loaded where
// the route contract has no error-block affordance), BridgeResult::failure
// with a {code,message} shape. api.js's route adapter (M4b) maps a
// **rejected** promise to CommandErrorResult at the JS seam; a **resolved**
// promise carrying a `{type:"error",...}` block is already the shape the
// console renderer expects, unchanged from the fetch-transport era.
//
// BUG-1372: a string function works on bytes, as Stata's do, so a block may
// carry a split UTF-8 sequence; it is sent as U+FFFD, as the do paths send it
// (main.cpp), instead of throwing the whole response away. So is a byte that is
// not UTF-8 (BUG-1131).
BridgeResult ok(const ojson& j) {
  return BridgeResult::success(j.dump(-1, ' ', false, ojson::error_handler_t::replace));
}

BridgeResult harnessError(const std::string& code, const std::string& message) {
  ojson err;
  err["code"] = code;
  err["message"] = message;
  return BridgeResult::failure(err.dump());
}

ojson noDataLoadedRest() {
  ojson j;
  j["type"] = "error";
  j["code"] = "no_data_loaded";
  j["message"] = "No dataset loaded";
  return j;
}

ojson engineErrorRest(const std::string& message) {
  ojson j;
  j["type"] = "error";
  j["code"] = "engine_error";
  j["message"] = message;
  return j;
}

int routeTmpCounter = 0;
std::string nextPathR(const std::string& workdir, const std::string& prefix) {
  return workdir + "/" + prefix + "_" + std::to_string(++routeTmpCounter) + ".hbin";
}

}  // namespace

// ---------------------------------------------------------------------------
// command / do / do_string

BridgeResult command(SessionState& session, const std::string& body) {
  nlohmann::json req;
  try {
    req = nlohmann::json::parse(body);
  } catch (const nlohmann::json::exception& e) {
    return harnessError("bad_json", e.what());
  }
  std::string cmd = req.value("cmd", std::string());
  ojson blocks = dispatch(cmd, session);

  ojson varlistAfter = nullptr;
  for (const auto& b : blocks) {
    if (b.value("type", std::string()) == "data_mutation" && b.contains("varlist_after")) {
      varlistAfter = b["varlist_after"];
    }
  }
  ojson out;
  out["blocks"] = blocks;
  out["varlist_after"] = varlistAfter;
  return ok(out);
}

namespace {

ojson executeResultToJson(const doexec::ExecuteResult& r, std::optional<std::string> resolvedPath) {
  ojson out;
  out["blocks"] = r.blocks;
  if (r.stoppedAtLine.has_value()) out["stopped_at_line"] = *r.stoppedAtLine;
  if (r.stoppedAtReason.has_value()) out["stopped_at_reason"] = *r.stoppedAtReason;
  if (resolvedPath.has_value()) out["resolved_path"] = *resolvedPath;
  return out;
}

}  // namespace

BridgeResult doString(SessionState& session, const std::string& body) {
  nlohmann::json req;
  try {
    req = nlohmann::json::parse(body);
  } catch (const nlohmann::json::exception& e) {
    return harnessError("bad_json", e.what());
  }
  std::string source = req.value("source", std::string());
  std::vector<std::string> args;
  if (req.contains("args") && req["args"].is_array()) {
    for (const auto& a : req["args"]) args.push_back(a.get<std::string>());
  }
  doexec::ExecuteResult r =
      doexec::executeDoSource(source, session, dispatch, args, /*progressOwner=*/true, std::nullopt);
  return ok(executeResultToJson(r, std::nullopt));
}

// ---------------------------------------------------------------------------
// run_progress / run_cancel (B1; see main.cpp for the background-thread
// wiring that makes these observe a run IN PROGRESS from a concurrent
// bridge call — these two handlers themselves are trivial passthroughs to
// SessionState's already-mutex-guarded snapshot/cancel-flag).

BridgeResult runProgress(SessionState& session, const std::string& /*body*/) {
  return ok(session.progressSnapshot());
}

BridgeResult runCancel(SessionState& session, const std::string& /*body*/) {
  bool wasRunning = session.progressRequestCancel();
  ojson out;
  out["cancelled"] = wasRunning;
  return ok(out);
}

// ---------------------------------------------------------------------------
// open / varlist / browse / history / version

BridgeResult open(SessionState& session, const std::string& body) {
  nlohmann::json req;
  try {
    req = nlohmann::json::parse(body);
  } catch (const nlohmann::json::exception& e) {
    return harnessError("bad_json", e.what());
  }
  std::string filepath = req.value("filepath", std::string());
  try {
    ojson result = handlers::runUse(filepath, session);
    return ok(result);
  } catch (const std::exception& e) {
    return ok(engineErrorRest(e.what()));
  }
}

BridgeResult varlist(SessionState& session, const std::string& /*body*/) {
  if (!session.hasData()) {
    ojson out;
    out["varlist"] = ojson::array();
    out["n_rows"] = 0;
    out["n_cols"] = 0;
    // P8 "Honest Cockpit" (U1, contract_B v2.1.0, additive): "" when no
    // dataset has ever loaded this session (currentFile_ default).
    out["source_file"] = session.currentFile();
    return ok(out);
  }
  Datastore* ds = session.datastore();
  ojson vl = ojson::array();
  for (const auto& v : session.varlistWithMeta(ds)) vl.push_back(v.toJson());
  ojson out;
  out["varlist"] = vl;
  out["n_rows"] = ds ? ds->nRows() : 0;
  out["n_cols"] = ds ? ds->nCols() : 0;
  // P8 "Honest Cockpit" (U1, contract_B v2.1.0, additive): resolved path of
  // the last successfully-loaded dataset — lets the frontend show what's
  // actually loaded instead of a painted-but-empty status bar.
  out["source_file"] = session.currentFile();
  return ok(out);
}

BridgeResult browse(SessionState& session, const std::string& body) {
  if (!session.hasData()) {
    return ok(noDataLoadedRest());
  }
  nlohmann::json req;
  try {
    req = body.empty() ? nlohmann::json::object() : nlohmann::json::parse(body);
  } catch (const nlohmann::json::exception&) {
    req = nlohmann::json::object();
  }
  int64_t offset = req.value("offset", 0);
  int64_t limit = req.value("limit", 200);
  std::optional<std::vector<std::string>> vars;
  if (req.contains("vars") && !req["vars"].is_null()) {
    std::vector<std::string> v;
    if (req["vars"].is_array()) {
      for (const auto& x : req["vars"]) v.push_back(x.get<std::string>());
    } else if (req["vars"].is_string()) {
      std::string s = req["vars"].get<std::string>();
      size_t start = 0;
      while (start <= s.size()) {
        size_t comma = s.find(',', start);
        std::string tok = s.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        if (!tok.empty()) v.push_back(tok);
        if (comma == std::string::npos) break;
        start = comma + 1;
      }
    }
    vars = v;
  }
  Datastore* ds = session.datastore();
  // format-display-default-lifecycle: the Data Editor grid renders each column
  // in the format `list` and describe report (SessionState::displayFormat); this
  // route used to pass no format map, so a `format x %td` never reached it.
  ojson result = ds->browse(offset, limit, vars, session.valueLabelMapForBrowse(),
                            session.displayFormatMap(ds->formatCard(vars)));
  return ok(result);
}

BridgeResult history(SessionState& session, const std::string& /*body*/) {
  ojson out;
  out["history"] = session.historyJson();
  return ok(out);
}

BridgeResult version(SessionState& session, const std::string& /*body*/) {
  (void)session;
  ojson out;
  out["piet_version"] = PIET_VERSION_STRING;
  out["contract_a"] = PIET_CONTRACT_A_VERSION;
  out["contract_b"] = PIET_CONTRACT_B_VERSION;

  CobraRunResult run = runCobra({"version", "--json"});
  std::string cobraVer = "unknown";
  if (run.ok) {
    try {
      auto parsed = nlohmann::json::parse(run.stdout_text);
      cobraVer = parsed.value("cobra_version", parsed.value("version", std::string("unknown")));
    } catch (const nlohmann::json::exception&) {
      // leave "unknown" — a malformed cobra probe should never fail the
      // whole /api/version route (mirrors the oracle's best-effort
      // _get_cobra_version_info(), which also swallows probe failures).
    }
  }
  out["cobra_version"] = cobraVer;
  return ok(out);
}

// ---------------------------------------------------------------------------
// Variable-Editor endpoints (rename / label.* / sort / recode / cell)

BridgeResult rename(SessionState& session, const std::string& body) {
  nlohmann::json req;
  try {
    req = nlohmann::json::parse(body);
  } catch (const nlohmann::json::exception& e) {
    return harnessError("bad_json", e.what());
  }
  try {
    ojson result = handlers::runRename(req.value("varname", std::string()),
                                        req.value("newname", std::string()), session);
    return ok(result);
  } catch (const std::exception& e) {
    return ok(engineErrorRest(e.what()));
  }
}

BridgeResult labelVariable(SessionState& session, const std::string& body) {
  nlohmann::json req;
  try {
    req = nlohmann::json::parse(body);
  } catch (const nlohmann::json::exception& e) {
    return harnessError("bad_json", e.what());
  }
  try {
    ojson result = handlers::runLabelVariable(req.value("varname", std::string()),
                                               req.value("label", std::string()), session);
    return ok(result);
  } catch (const std::exception& e) {
    return ok(engineErrorRest(e.what()));
  }
}

// label.define: CONTRACT.md's body is {lblname, mapping:{code:text}} — a
// STRUCTURED map, not the raw Stata `label define ...` syntax
// handlers::runLabelDefine expects (that's the console-command path, which
// also re-attaches to already-labeled vars — a stronger behavior than the
// REST oracle's editor.py::run_label_define, a PURE metadata store with no
// re-attach). This adapter mirrors editor.py exactly: write straight into
// session.labelDefs, do NOT touch varLblname/valueLabels.
BridgeResult labelDefine(SessionState& session, const std::string& body) {
  nlohmann::json req;
  try {
    req = nlohmann::json::parse(body);
  } catch (const nlohmann::json::exception& e) {
    return harnessError("bad_json", e.what());
  }
  std::string lblname = req.value("lblname", std::string());
  if (lblname.empty()) {
    return ok(engineErrorRest("lblname must not be empty."));
  }
  std::map<std::string, std::string> mapping;
  if (req.contains("mapping") && req["mapping"].is_object()) {
    for (auto it = req["mapping"].begin(); it != req["mapping"].end(); ++it) {
      mapping[it.key()] = it.value().is_string() ? it.value().get<std::string>() : it.value().dump();
    }
  }
  session.labelDefs[lblname] = mapping;

  session.invalidateDatastore();
  Datastore* ds = session.datastore();
  ojson vl = ojson::array();
  for (const auto& v : session.varlistWithMeta(ds)) vl.push_back(v.toJson());
  ojson out;
  out["type"] = "data_mutation";
  out["action"] = "label_define";
  out["varlist_after"] = vl;
  out["n_rows_after"] = ds ? ds->nRows() : 0;
  return ok(out);
}

BridgeResult labelValues(SessionState& session, const std::string& body) {
  nlohmann::json req;
  try {
    req = nlohmann::json::parse(body);
  } catch (const nlohmann::json::exception& e) {
    return harnessError("bad_json", e.what());
  }
  std::string varname = req.value("varname", std::string());
  std::optional<std::string> lblname;
  if (req.contains("lblname") && !req["lblname"].is_null()) lblname = req["lblname"].get<std::string>();
  try {
    ojson result = handlers::runLabelValues(varname, lblname, session);
    return ok(result);
  } catch (const std::exception& e) {
    return ok(engineErrorRest(e.what()));
  }
}

// sort: CONTRACT.md's optional per-column `ascending: bool[]` has no
// counterpart in handlers::runSort (the console `sort` command is always
// all-ascending; per-column direction is a `gsort`-only feature natively).
// When every requested direction is ascending (the common case, and the
// ONLY case the console `sort` command itself can express), delegate to the
// byte-verified handlers::runSort. When any column requests descending,
// build the equivalent gsort token list ([+-]varname) and delegate to
// handlers::runGsort (M3b, cobra's native --vars "-a,b" verb — pre-validated
// to the SAME "Variable 'X' not found." error text runSort uses), then
// normalise the response's `action` field back to "sort" to match
// CONTRACT.md's DataMutationResult(action:"sort") shape exactly.
BridgeResult sortRoute(SessionState& session, const std::string& body) {
  nlohmann::json req;
  try {
    req = nlohmann::json::parse(body);
  } catch (const nlohmann::json::exception& e) {
    return harnessError("bad_json", e.what());
  }
  std::vector<std::string> varlist;
  if (req.contains("varlist") && req["varlist"].is_array()) {
    for (const auto& v : req["varlist"]) varlist.push_back(v.get<std::string>());
  }
  std::vector<bool> ascending;
  bool hasDescending = false;
  if (req.contains("ascending") && req["ascending"].is_array()) {
    for (const auto& a : req["ascending"]) {
      bool v = a.get<bool>();
      ascending.push_back(v);
      if (!v) hasDescending = true;
    }
  }
  try {
    if (!hasDescending) {
      return ok(handlers::runSort(varlist, session));
    }
    std::vector<std::string> tokens;
    for (size_t i = 0; i < varlist.size(); ++i) {
      bool asc = i < ascending.size() ? ascending[i] : true;
      tokens.push_back((asc ? "" : "-") + varlist[i]);
    }
    ojson result = handlers::runGsort(tokens, session);
    if (result.is_object() && result.value("type", std::string()) == "data_mutation") {
      result["action"] = "sort";
    }
    return ok(result);
  } catch (const std::exception& e) {
    return ok(engineErrorRest(e.what()));
  }
}

// recode: CONTRACT.md's body ({varname, rules, gen?}) carries `rules` in
// editor.py's BARE space-separated grammar ("1/2=1 3/5=2 else=99" — see
// editor.py's _parse_recode_rules), which is a DIFFERENT surface grammar
// from handlers::runRecode's parenthesised Stata/cobra-native syntax
// ("(1/2=1)(3/5=2)(else=99)" — M3b routed the console `recode` command to
// cobra 0.18.0's native verb, which speaks the parenthesised form). Both
// express the identical rule LIST, just delimited differently, so this
// adapter does a mechanical bare->parenthesised token wrap (each
// whitespace-separated "LHS=RHS" token, honouring the `.`/`else` special
// LHS forms verbatim) and delegates to the byte-verified handlers::
// runRecode — not new recode semantics, just a grammar-surface bridge.
std::string wrapRecodeRulesParens(const std::string& bareRules) {
  std::string out;
  std::istringstream iss(bareRules);
  std::string token;
  while (iss >> token) {
    out += "(" + token + ")";
  }
  return out;
}

BridgeResult recode(SessionState& session, const std::string& body) {
  nlohmann::json req;
  try {
    req = nlohmann::json::parse(body);
  } catch (const nlohmann::json::exception& e) {
    return harnessError("bad_json", e.what());
  }
  std::string varname = req.value("varname", std::string());
  std::string bareRules = req.value("rules", std::string());
  std::string rules = wrapRecodeRulesParens(bareRules);
  std::string line = "recode " + varname + " " + rules;
  if (req.contains("gen") && !req["gen"].is_null()) {
    line += ", gen(" + req["gen"].get<std::string>() + ")";
  }
  try {
    return ok(handlers::runRecode(line, session));
  } catch (const std::exception& e) {
    return ok(engineErrorRest(e.what()));
  }
}

// cell: POST /api/cell {varname, row, value} — genuinely new engine logic;
// no prior C++ port existed (native hbin WRITE has no direct API in the
// linked columnar.h — read-only, per postfile.cpp's own note). f64 columns:
// implemented via a `cobra eval --egen "rowidx=seq()" --gen "VAR=cond(
// rowidx==(ROW+1),VALUE,VAR)" --drop rowidx --out FILE` round-trip (the
// SAME cobra-subprocess-then-session.load() pattern every other mutation
// handler in this codebase uses — see handlers.cpp's runReplace). factor
// columns (resolve-or-append a level) need hbin level-table mutation with
// no cobra verb or linked write-API to do it through — left as an explicit
// piet_gap (documented remaining item), NOT a silent wrong write.
BridgeResult cell(SessionState& session, const std::string& body) {
  nlohmann::json req;
  try {
    req = nlohmann::json::parse(body);
  } catch (const nlohmann::json::exception& e) {
    return harnessError("bad_json", e.what());
  }
  std::string varname = req.value("varname", std::string());
  int64_t row = req.value("row", 0);

  if (!session.hasData()) return ok(noDataLoadedRest());
  Datastore* ds = session.datastore();
  if (ds == nullptr || !ds->has(varname)) {
    return ok(engineErrorRest("Variable '" + varname + "' not found."));
  }
  int64_t nRows = ds->nRows();
  if (row < 0 || row >= nRows) {
    return ok(engineErrorRest("Row " + std::to_string(row) + " out of range (dataset has " +
                               std::to_string(nRows) + " rows; valid 0.." +
                               std::to_string(nRows - 1) + ")."));
  }

  std::string dtype = ds->dtype(varname);
  if (dtype != "f64") {
    return ok(eval::errorBlock(
        "engine_error",
        "cell write: factor-column edits are not yet supported by the native core "
        "(level resolve/append needs hbin level-table mutation — M4b-remaining).",
        std::nullopt, std::nullopt, std::nullopt, std::string("piet_gap")));
  }

  std::string valueExpr;
  bool isNull = req.contains("value") ? req["value"].is_null() : true;
  if (isNull) {
    valueExpr = "(0/0)";  // NaN sentinel matching cobra's own missing-value expr idiom
  } else {
    double v;
    try {
      v = req["value"].get<double>();
    } catch (const nlohmann::json::exception&) {
      return ok(engineErrorRest("Cell value must be a number for an f64 column."));
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.17g", v);
    valueExpr = buf;
  }

  std::string outPath = nextPathR(session.workdir(), "cell");
  std::string genExpr =
      varname + "=cond(rowidx==(" + std::to_string(row + 1) + ")," + valueExpr + "," + varname + ")";
  std::vector<std::string> args = {"eval",         "--data",  *session.currentHbin(),
                                    "--egen",       "rowidx=seq()",
                                    "--gen",        genExpr,
                                    "--drop",       "rowidx",
                                    "--out",        outPath};
  try {
    runCobraJson(args);
  } catch (const CobraError& ce) {
    return ok(engineErrorRest(ce.what()));
  }
  session.load(outPath);

  Datastore* newDs = session.datastore();
  ojson vl = ojson::array();
  for (const auto& v : session.varlistWithMeta(newDs)) vl.push_back(v.toJson());
  ojson out;
  out["type"] = "data_mutation";
  out["action"] = "cell_write";
  out["varname"] = varname;
  out["row"] = row;
  out["value"] = isNull ? ojson(nullptr) : ojson(req["value"]);
  out["varlist_after"] = vl;
  out["n_rows_after"] = newDs ? newDs->nRows() : 0;
  return ok(out);
}

// ---------------------------------------------------------------------------
// tabulate / describe / summarize

BridgeResult tabulate(SessionState& session, const std::string& body) {
  if (!session.hasData()) return ok(noDataLoadedRest());
  nlohmann::json req;
  try {
    req = nlohmann::json::parse(body);
  } catch (const nlohmann::json::exception& e) {
    return harnessError("bad_json", e.what());
  }
  std::vector<std::string> varlist;
  if (req.contains("varlist") && req["varlist"].is_array()) {
    for (const auto& v : req["varlist"]) varlist.push_back(v.get<std::string>());
  }
  bool chi2 = req.value("chi2", false);
  Datastore* ds = session.datastore();
  return ok(handlers::runTabulate(*ds, varlist, chi2, "tabulate", std::nullopt));
}

BridgeResult describe(SessionState& session, const std::string& body) {
  if (!session.hasData()) return ok(noDataLoadedRest());
  nlohmann::json req;
  try {
    req = body.empty() ? nlohmann::json::object() : nlohmann::json::parse(body);
  } catch (const nlohmann::json::exception&) {
    req = nlohmann::json::object();
  }
  std::optional<std::vector<std::string>> varlist;
  if (req.contains("varlist") && req["varlist"].is_array()) {
    std::vector<std::string> v;
    for (const auto& x : req["varlist"]) v.push_back(x.get<std::string>());
    varlist = v;
  }
  return ok(handlers::runDescribe(varlist, session));
}

BridgeResult summarize(SessionState& session, const std::string& body) {
  if (!session.hasData()) return ok(noDataLoadedRest());
  nlohmann::json req;
  try {
    req = body.empty() ? nlohmann::json::object() : nlohmann::json::parse(body);
  } catch (const nlohmann::json::exception&) {
    req = nlohmann::json::object();
  }
  std::optional<std::vector<std::string>> varlist;
  if (req.contains("varlist") && req["varlist"].is_array()) {
    std::vector<std::string> v;
    for (const auto& x : req["varlist"]) v.push_back(x.get<std::string>());
    varlist = v;
  }
  bool detail = req.value("detail", false);
  return ok(handlers::runSummarize(varlist, detail, session));
}

// ---------------------------------------------------------------------------
// file.save (v2.4.0, N1) / var_detail (v2.4.0, N7)

// Core writer for POST /api/file/save once a destination PATH is known (the
// body's explicit `path`, or the NSSavePanel result the main.cpp lambda
// resolves before calling this). Shell-free on purpose so selftest.cpp
// exercises the whole decode+write contract headlessly. Exactly one of
// `text` / `contentB64` must be present: content_b64 is base64-decoded and
// written as BINARY (PNG/SVG exports); text is written verbatim. `kind` is
// advisory only (it drove the save-panel's type filter upstream) — the bytes
// written are exactly what text/content_b64 carry, never reshaped by kind. A
// decode failure, a missing payload, or an IO failure is an HONEST bridge
// error ({type:"error"} idiom), NEVER a false {ok:true} on an unwritten file
// (COMMS §4.8 — the false-"Saved" class this route exists to kill).
BridgeResult fileSaveToPath(const std::string& path, const std::string& kind,
                            const std::optional<std::string>& text,
                            const std::optional<std::string>& contentB64) {
  (void)kind;  // advisory (see doc comment) — deliberately not used here.
  auto saveError = [&](const std::string& why) {
    return ok(engineErrorRest("file.save: " + why + " (" + path + ")"));
  };
  std::string bytes;
  if (contentB64.has_value() && text.has_value()) {
    return saveError("provide exactly one of `text` or `content_b64`, not both");
  }
  if (contentB64.has_value()) {
    if (!piet::base64Decode(*contentB64, bytes)) {
      return saveError("`content_b64` is not valid base64");
    }
  } else if (text.has_value()) {
    bytes = *text;
  } else {
    return saveError("requires exactly one of `text` or `content_b64`");
  }
  std::ofstream f(cecon::to_native_path(path), std::ios::binary | std::ios::trunc);
  if (!f) {
    return saveError("could not open destination for writing");
  }
  f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  f.flush();
  if (!f) {
    return saveError("write failed (path not writable, or the disk is full)");
  }
  ojson out;
  out["ok"] = true;
  out["path"] = path;
  return ok(out);
}

// GET /api/var_detail?name=X — single-column stats (N7). The JS shim reshapes
// the query param into a {name} body (same convention `browse` uses). O(rows)
// on ONE column — never materializes the whole frame. dtype/format/label come
// from the SAME metadata-enriched varlist the Variables panel reads, so
// var_detail never disagrees with the table; N/n_missing/moments come from a
// single pass over the column (Welford for a numerically-stable mean/sd,
// matching `summarize`'s divisor-(n-1) sd). String/factor columns omit the
// numeric moments per contract. Unknown name → engine error, never a
// fabricated row.
BridgeResult varDetail(SessionState& session, const std::string& body) {
  if (!session.hasData()) return ok(noDataLoadedRest());
  nlohmann::json req;
  try {
    req = body.empty() ? nlohmann::json::object() : nlohmann::json::parse(body);
  } catch (const nlohmann::json::exception& e) {
    return harnessError("bad_json", e.what());
  }
  std::string name = req.value("name", std::string());
  Datastore* ds = session.datastore();
  if (ds == nullptr || !ds->has(name)) {
    return ok(engineErrorRest("Variable '" + name + "' not found."));
  }

  // Metadata (dtype/label/n_missing/format) from the enriched varlist, whose
  // format is the one resolver's answer (SessionState::displayFormat) -- the
  // same one list, describe and `: format` report.
  VarInfo meta;
  for (const auto& v : session.varlistWithMeta(ds)) {
    if (v.name == name) {
      meta = v;
      break;
    }
  }
  const std::string fmt = meta.format;

  ojson out;
  out["name"] = name;
  out["dtype"] = meta.dtype;
  out["format"] = fmt;
  out["label"] = meta.label;
  out["N"] = ds->nRows();
  // F5 lazy-levels companion (contract_B 2.5.0): varlist cards no longer
  // inline factor levels — this endpoint is the on-demand source. Same
  // label-remapped vector the cards' n_levels counts (varlistWithMeta).
  if (meta.dtype == "factor" && !meta.levels.empty()) out["levels"] = meta.levels;

  const cecon::Column& c = ds->col(name);
  if (c.dtype == cecon::DType::F64) {
    const double* d = c.f64();
    int64_t count = c.nbytes / static_cast<int64_t>(sizeof(double));
    int64_t nMiss = 0;
    int64_t nGood = 0;
    double mean = 0.0, m2 = 0.0, mn = 0.0, mx = 0.0;
    bool first = true;
    for (int64_t i = 0; i < count; ++i) {
      double v = d[i];
      if (std::isnan(v)) {
        ++nMiss;
        continue;
      }
      ++nGood;
      double delta = v - mean;
      mean += delta / static_cast<double>(nGood);
      m2 += delta * (v - mean);
      if (first) {
        mn = mx = v;
        first = false;
      } else {
        if (v < mn) mn = v;
        if (v > mx) mx = v;
      }
    }
    out["n_missing"] = nMiss;
    if (nGood > 0) {
      out["mean"] = mean;
      out["min"] = mn;
      out["max"] = mx;
      if (nGood > 1) {
        double var = m2 / static_cast<double>(nGood - 1);
        if (var < 0) var = 0;  // guard against fp round-off dipping below 0
        out["sd"] = std::sqrt(var);
      }
    }
  } else {
    // factor / string: missing = negative code; numeric moments omitted.
    out["n_missing"] = ds->nMissing(name);
  }
  return ok(out);
}

// ── legal.notices (WI-5) ─────────────────────────────────────────────────
// App-shell service route (no session): the third-party notices text for the
// frontend's "About / Legal" panel. Packaged runs read the exact
// Resources/THIRD_PARTY_NOTICES.txt file package_native_app.sh generated and
// preflight-asserted into the bundle; dev runs concatenate the same
// checked-in manifest (piet/packaging/licenses/NN-*.txt, lexical order —
// mirrors the packager's generator) so both modes serve identical content
// from ONE source. staged/ placeholders are excluded in both modes.
namespace {

bool readWholeFile(const std::string& path, std::string& out) {
  std::ifstream f(cecon::to_native_path(path), std::ios::binary);
  if (!f) return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  out = ss.str();
  return true;
}

}  // namespace

BridgeResult legalNotices(const std::string& /*body*/) {
  ojson resp;

  // Packaged bundle: the generated notices file.
  if (std::optional<std::string> resources = bundleResourcesDir()) {
    std::string text;
    if (readWholeFile(*resources + "/THIRD_PARTY_NOTICES.txt", text) && !text.empty()) {
      resp["text"] = text;
      resp["source"] = "bundle";
      return BridgeResult::success(resp.dump());
    }
    return harnessError("notices_missing",
                        "THIRD_PARTY_NOTICES.txt absent from the bundle Resources — "
                        "packaging preflight should have failed this build");
  }

  // Dev run: concatenate the checked-in manifest (same NN-*.txt glob +
  // lexical order the packager uses).
  std::string root = std::string(__FILE__);
  auto pos = root.rfind("/piet/native/src/routes.cpp");
  if (pos != std::string::npos) {
    std::string licensesDir = root.substr(0, pos) + "/piet/packaging/licenses";
    std::vector<std::string> parts;
    if (DIR* d = opendir(licensesDir.c_str())) {
      while (dirent* e = readdir(d)) {
        std::string name = e->d_name;
        // NN-*.txt only (digits prefix): skips README.md and staged/.
        if (name.size() > 4 && std::isdigit(static_cast<unsigned char>(name[0])) &&
            std::isdigit(static_cast<unsigned char>(name[1])) &&
            name.compare(name.size() - 4, 4, ".txt") == 0) {
          parts.push_back(name);
        }
      }
      closedir(d);
    }
    std::sort(parts.begin(), parts.end());
    if (!parts.empty()) {
      std::ostringstream joined;
      joined << "PIET " << PIET_VERSION_STRING
             << " — THIRD-PARTY NOTICES (dev build; assembled live from piet/packaging/licenses/)\n";
      for (const auto& p : parts) {
        std::string text;
        if (!readWholeFile(licensesDir + "/" + p, text)) continue;
        joined << "\n────────────────────────────────────────────────────────────────────\n\n" << text;
      }
      resp["text"] = joined.str();
      resp["source"] = "dev";
      return BridgeResult::success(resp.dump());
    }
  }

  return harnessError("notices_unavailable",
                      "third-party notices not found (no bundle Resources and no dev manifest)");
}

}  // namespace piet::routes
