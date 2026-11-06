// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// --evalcase driver — the native half of the M1 differential-parity harness.
//
// Reads JSONL cases from stdin (one JSON object per line), runs each through
// the C++ evaluator port, and writes one JSON result object per line to
// stdout. The Python twin (piet/native/tests/eval_diff/py_twin.py) feeds the
// SAME cases through the real 1.16.0 backend modules and emits the same
// shape; tests/eval_diff/run_diff.py diffs the two streams. Gate: 100%.
//
// Case shapes (field `kind` selects the operation):
//   {"kind":"tokenize", "line": str}
//       -> {"ok":true, "parsed": ParsedCommand-json}
//        | {"ok":false, "error": CommandErrorResult}
//   {"kind":"preprocess_do", "source": str}
//       -> {"ok":true, "lines": [str, ...]}
//   {"kind":"scan_one_token", "s": str, "bind": bool}
//       -> {"ok":true, "token": str, "remainder": str}
//   {"kind":"find_unquoted_comma", "s": str}
//       -> {"ok":true, "index": int}
//   {"kind":"gettoken", "line": str, "locals": {name: value, ...}}
//       -> {"ok":true, "blocks": [...], "locals": {...}}   (locals AFTER)
//   {"kind":"error_helper", "helper": str, "args": {...}}
//       -> {"ok":true, "block": CommandErrorResult}
//
// Later M1 commits extend this with exprfn / eval_arith / expand_macros /
// capture_definition / macro_script / eval_macro_expr / capture_r kinds.
//
// All output goes through nlohmann::ordered_json so object key order is
// deterministic and mirrors the twin's insertion-ordered dicts.
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include "evalcase_internal.h"
#include "piet/eval/errors.h"
#include "piet/eval/gettoken.h"
#include "piet/eval/tokenizer.h"
#include "vendor/nlohmann/json.hpp"

namespace piet::eval {

using ojson = nlohmann::ordered_json;

namespace {

// Re-encode a plain json error block as ordered_json preserving the
// errorBlock() field order (type, code, message, token?, line_number?,
// context?, reason?) — the twin emits dicts in the same insertion order.
ojson orderedError(const nlohmann::json& b) {
  ojson o;
  for (const char* k : {"type", "code", "message", "token", "line_number", "context", "reason"}) {
    if (b.contains(k)) o[k] = b.at(k);
  }
  return o;
}

ojson handleTokenize(const ojson& c) {
  ojson out;
  try {
    ParsedCommand pc = tokenize(c.at("line").get<std::string>());
    out["ok"] = true;
    ojson parsed;
    parsed["cmd"] = pc.cmd;
    parsed["varlist"] = pc.varlist;
    parsed["if_expr"] = pc.ifExpr ? ojson(*pc.ifExpr) : ojson(nullptr);
    parsed["in_range"] = pc.inRange ? ojson(*pc.inRange) : ojson(nullptr);
    parsed["weight_type"] = pc.weightType ? ojson(*pc.weightType) : ojson(nullptr);
    parsed["weight_var"] = pc.weightVar ? ojson(*pc.weightVar) : ojson(nullptr);
    ojson opts = ojson::object();
    for (const auto& [k, v] : pc.opts) {
      opts[k] = v.isBool ? ojson(v.boolValue) : ojson(v.strValue);
    }
    parsed["opts"] = opts;
    parsed["raw"] = pc.raw;
    out["parsed"] = parsed;
  } catch (const TokenizerError& e) {
    out["ok"] = false;
    out["error"] = orderedError(e.block);
  }
  return out;
}

ojson handlePreprocessDo(const ojson& c) {
  ojson out;
  out["ok"] = true;
  out["lines"] = preprocessDo(c.at("source").get<std::string>());
  return out;
}

ojson handleScanOneToken(const ojson& c) {
  auto [tok, rem] = scanOneToken(c.at("s").get<std::string>(), c.value("bind", false));
  ojson out;
  out["ok"] = true;
  out["token"] = tok;
  out["remainder"] = rem;
  return out;
}

ojson handleFindUnquotedComma(const ojson& c) {
  ojson out;
  out["ok"] = true;
  out["index"] = findUnquotedComma(c.at("s").get<std::string>());
  return out;
}

ojson handleGettoken(const ojson& c) {
  // Ordered locals store mirroring Python dict insertion-order semantics:
  // updating an existing key keeps its position; new keys append.
  std::vector<std::pair<std::string, std::string>> locals;
  if (c.contains("locals")) {
    for (const auto& [k, v] : c.at("locals").items()) {
      locals.emplace_back(k, v.get<std::string>());
    }
  }
  LocalsStore store;
  store.get = [&locals](const std::string& name) -> std::string {
    for (const auto& [k, v] : locals) {
      if (k == name) return v;
    }
    return "";
  };
  store.set = [&locals](const std::string& name, const std::string& value) {
    for (auto& [k, v] : locals) {
      if (k == name) {
        v = value;
        return;
      }
    }
    locals.emplace_back(name, value);
  };

  auto blocks = runGettoken(c.at("line").get<std::string>(), store);

  ojson out;
  out["ok"] = true;
  ojson blocksJ = ojson::array();
  for (const auto& b : blocks) blocksJ.push_back(orderedError(b));
  out["blocks"] = blocksJ;
  ojson localsJ = ojson::object();
  for (const auto& [k, v] : locals) localsJ[k] = v;
  out["locals"] = localsJ;
  return out;
}

ojson handleErrorHelper(const ojson& c) {
  const std::string helper = c.at("helper").get<std::string>();
  const ojson args = c.value("args", ojson::object());

  auto optStr = [&args](const char* k) -> std::optional<std::string> {
    if (args.contains(k) && !args.at(k).is_null()) return args.at(k).get<std::string>();
    return std::nullopt;
  };
  auto optInt = [&args](const char* k) -> std::optional<int> {
    if (args.contains(k) && !args.at(k).is_null()) return args.at(k).get<int>();
    return std::nullopt;
  };

  nlohmann::json block;
  if (helper == "error_block") {
    block = errorBlock(args.at("code").get<std::string>(), args.at("message").get<std::string>(),
                        optStr("token"), optInt("line_number"), optStr("context"), optStr("reason"));
  } else if (helper == "unsupported_loop") {
    block = unsupportedLoop(args.at("token").get<std::string>(), optInt("line_number"), optStr("context"));
  } else if (helper == "unsupported_macro") {
    block = unsupportedMacro(args.at("token").get<std::string>(), optInt("line_number"), optStr("context"));
  } else if (helper == "unsupported_program") {
    block = unsupportedProgram(args.at("token").get<std::string>(), optInt("line_number"), optStr("context"));
  } else if (helper == "unsupported_delimiter") {
    block = unsupportedDelimiter(args.at("token").get<std::string>(), optInt("line_number"), optStr("context"));
  } else if (helper == "unsupported_command") {
    block = unsupportedCommand(args.at("cmd").get<std::string>(), optInt("line_number"), optStr("context"),
                                args.value("reason", std::string("piet_gap")));
  } else if (helper == "unsupported_option") {
    block = unsupportedOption(args.at("opt").get<std::string>(), args.value("hint", std::string()),
                               optInt("line_number"), optStr("context"), optStr("reason"));
  } else if (helper == "no_data_loaded") {
    block = noDataLoaded(optInt("line_number"));
  } else if (helper == "engine_error") {
    block = engineError(args.at("message").get<std::string>(),
                         args.value("code", std::string("engine_error")), optInt("line_number"),
                         optStr("context"));
  } else {
    ojson out;
    out["ok"] = false;
    out["driver_error"] = "unknown error helper: " + helper;
    return out;
  }
  ojson out;
  out["ok"] = true;
  out["block"] = orderedError(block);
  return out;
}

ojson handleCase(const ojson& c) {
  const std::string kind = c.at("kind").get<std::string>();
  if (kind == "tokenize") return handleTokenize(c);
  if (kind == "preprocess_do") return handlePreprocessDo(c);
  if (kind == "scan_one_token") return handleScanOneToken(c);
  if (kind == "find_unquoted_comma") return handleFindUnquotedComma(c);
  if (kind == "gettoken") return handleGettoken(c);
  if (kind == "error_helper") return handleErrorHelper(c);
  // exprfn / eval_arith / eval_macro_expr / expand_macros /
  // capture_definition / macro_script / has_macro_refs / capture_r kinds are
  // added by the exprfns and macros M1 commits (see extendEvalcaseKinds).
  if (ojson extended = handleExtendedCase(kind, c); !extended.is_null()) return extended;
  ojson out;
  out["ok"] = false;
  out["driver_error"] = "unknown case kind: " + kind;
  return out;
}

}  // namespace

int runEvalcaseDriver() {
  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.empty()) continue;
    ojson result;
    try {
      ojson c = ojson::parse(line);
      result = handleCase(c);
    } catch (const std::exception& e) {
      result = ojson::object();
      result["ok"] = false;
      result["driver_error"] = std::string("exception: ") + e.what();
    } catch (...) {
      // Safety net: a PyExc escaping a handler is a driver bug, but it must
      // show up as a diffable line, not a process abort.
      result = ojson::object();
      result["ok"] = false;
      result["driver_error"] = "exception: non-std exception escaped a handler";
    }
    std::fputs(result.dump().c_str(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
  }
  return 0;
}

}  // namespace piet::eval
