// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// --run-block — the M2 candidate JSON adapter: reads a piet2-corpus cell
// (JSON object) from stdin, executes it through the SAME dispatch() the
// interactive UI would call, and emits the canonical block-JSON stream to
// stdout. Matches the seam tests/eval/piet2/runner.py documents ("Future
// C++ core (M2+): wire a subprocess adapter in runner.py — the diff/
// tolerance machinery is already candidate-agnostic"): runner.py is
// Eval-owned (Art 10, read-only for this milestone), so this binary is the
// native-side half of that seam — Eval (or this milestone's own harness
// script, kept OUTSIDE tests/eval/**) drives it and diffs the output
// against the frozen corpus using the runner's existing canon.py/
// tolerances.py machinery.
//
// Protocol (one JSON object on stdin -> one JSON object on stdout, NOT
// JSONL streaming like --evalcase, since each invocation gets its own fresh
// SessionState). Two shapes, both under `kind: "dispatch"`:
//
//   Single-command:
//   {
//     "kind": "dispatch",
//     "cmd": "regress y x",            // input.cmd from the corpus cell
//     "pre_hbin": "/abs/path.hbin",    // optional: preload this dataset
//     "workdir": "/abs/scratch/dir"    // optional: SessionState workdir
//                                       // (defaults to a mkdtemp'd dir)
//   } -> {"ok": true, "blocks": [...]}
//
//   Sequence (replays an ordered list of commands against ONE shared
//   session, so a gate script can cold-start a full source_test's call
//   sequence and score any subset of the resulting block lists — the same
//   "sequence replay" contract tests/eval/piet2/runner.py's
//   run_sequence_replay() uses, reproduced here because runner.py itself is
//   Eval-owned/read-only for this milestone):
//   {
//     "kind": "dispatch", "cmds": ["use f.hbin, clear", "describe", ...],
//     "pre_hbin": "...", "workdir": "..."
//   } -> {"ok": true, "block_lists": [[...blocks for cmds[0]...], [...for cmds[1]...], ...]}
//
//   Harness-level failure (either shape): {"ok": false, "driver_error": "..."}
//
// M3a: `kind: "execute_do_source"` cells (the piet2 corpus's `input.source`
// leg — do-file/loop-body replay through doexec::executeDoSource, NOT a
// single dispatch() call) are now implemented — this was the "not yet"
// gap noted above at M2:
//   {
//     "kind": "execute_do_source",
//     "source": "foreach v in mpg price {\n  summarize `v'\n}\n",
//     "args": ["a", "b"],              // optional positional do-file args
//     "pre_hbin": "...", "workdir": "..."
//   } -> {"ok": true, "blocks": [...]}   (flat — matches doexec.py's
//                                          execute_do_source() result["blocks"]
//                                          shape exactly; stopped_at_line/
//                                          reason are NOT part of the piet2
//                                          corpus's frozen `output` field, so
//                                          they are omitted here too — the
//                                          gate script only diffs `blocks`)
//
// M3a: mixed-kind sequence replay — a source_test in the piet2 corpus can
// interleave `dispatch` and `execute_do_source` top-level calls sharing ONE
// session (session_seq), e.g. a `local k 3` dispatch cell followed by an
// `execute_do_source` cell whose `forvalues j = 1/`k' {...}` body depends on
// that local. `cmds`/`kind:"dispatch"` sequence mode (above) cannot express
// this (every step assumed to be a single dispatch() command). "steps" mode:
//   {
//     "kind": "steps",
//     "steps": [
//       {"kind": "dispatch", "cmd": "local k 3"},
//       {"kind": "execute_do_source", "source": "forvalues j=1/`k' {\n...\n}\n", "args": [...]}
//     ],
//     "pre_hbin": "...", "workdir": "..."
//   } -> {"ok": true, "block_lists": [[...blocks for steps[0]...], [...for steps[1]...], ...]}
// A dispatch()/executeDoSource() exception on any step aborts the whole
// sequence (matches a crash halting real replay) — reported as a harness
// error, not swallowed.
//
// One subprocess per corpus cell OR per source_test sequence — mirrors the
// Python runner's `fresh_session(preload)` per top-level test sequence.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>

#include "piet/dispatcher.h"
#include "piet/doexec.h"
#include "piet/session_state.h"
#include "io/win_compat.h"  // cecon::to_native_path (BUG-050 wide mkdir)
#include "vendor/nlohmann/json.hpp"

namespace piet {

namespace {

// BUG-1372: a string function works on bytes, as Stata's do, so a block may
// carry a split UTF-8 sequence; it is written as U+FFFD, as the do paths write
// it (main.cpp), instead of aborting the adapter.
std::string dumpBlocks(const nlohmann::ordered_json& out) {
  return out.dump(-1, ' ', false, nlohmann::ordered_json::error_handler_t::replace);
}

}  // namespace

int runBlockAdapter() {
  std::ostringstream ss;
  ss << std::cin.rdbuf();
  std::string input = ss.str();

  nlohmann::ordered_json out;
  nlohmann::json req;
  try {
    req = nlohmann::json::parse(input);
  } catch (const std::exception& e) {
    out["ok"] = false;
    out["driver_error"] = std::string("bad JSON on stdin: ") + e.what();
    std::fputs(dumpBlocks(out).c_str(), stdout);
    std::fputc('\n', stdout);
    return 1;
  }

  std::string kind = req.value("kind", std::string("dispatch"));
  if (kind != "dispatch" && kind != "execute_do_source" && kind != "steps") {
    out["ok"] = false;
    out["driver_error"] =
        "kind '" + kind + "' not implemented (M3a supports 'dispatch'/'execute_do_source'/'steps')";
    std::fputs(dumpBlocks(out).c_str(), stdout);
    std::fputc('\n', stdout);
    return 1;
  }

  // The gate script always passes an existing tmp dir; the default and the
  // mkdir below are the harmless fallback. Windows has no /tmp and no POSIX
  // `mkdir -p`, so the default moves to %TEMP% there only — leaving the macOS
  // default byte-identical.
#ifdef _WIN32
  std::string defaultWorkdir =
      (std::filesystem::temp_directory_path() / "piet_native_run_block").u8string();
#else
  std::string defaultWorkdir = "/tmp/piet_native_run_block";
#endif
  std::string workdir = req.value("workdir", defaultWorkdir);
  // create_directories replaces the old `std::system("mkdir -p '...'")`
  // shell-out: same semantics, no shell (cmd.exe has no `mkdir -p`), and no
  // quoting hazard on a path containing a quote.
  std::error_code workdirEc;
  std::filesystem::create_directories(cecon::to_native_path(workdir), workdirEc);

  SessionState session(workdir);

  if (req.contains("pre_hbin") && !req["pre_hbin"].is_null()) {
    std::string pre = req["pre_hbin"].get<std::string>();
    try {
      session.load(pre);
      // Force the datastore to open now so a bad pre_hbin fails loudly here,
      // not silently on first use inside dispatch().
      session.datastore();
    } catch (const std::exception& e) {
      out["ok"] = false;
      out["driver_error"] = std::string("pre_hbin load failed: ") + e.what();
      std::fputs(dumpBlocks(out).c_str(), stdout);
      std::fputc('\n', stdout);
      return 1;
    }
  }
  if (req.contains("cwd") && !req["cwd"].is_null()) {
    session.cwd = req["cwd"].get<std::string>();
    session.eval.cwd = *session.cwd;
  }

  if (kind == "steps") {
    nlohmann::ordered_json blockLists = nlohmann::ordered_json::array();
    try {
      for (const auto& step : req.value("steps", nlohmann::json::array())) {
        std::string stepKind = step.value("kind", std::string("dispatch"));
        if (stepKind == "execute_do_source") {
          std::string source = step.value("source", std::string());
          std::vector<std::string> args;
          if (step.contains("args") && step["args"].is_array()) {
            for (const auto& a : step["args"]) args.push_back(a.get<std::string>());
          }
          doexec::ExecuteResult result =
              doexec::executeDoSource(source, session, dispatch, args, /*progressOwner=*/false, std::nullopt);
          blockLists.push_back(result.blocks);
        } else {
          std::string cmd = step.value("cmd", std::string());
          blockLists.push_back(dispatch(cmd, session));
        }
      }
    } catch (const std::exception& e) {
      out["ok"] = false;
      out["driver_error"] = std::string("step execution threw mid-sequence: ") + e.what();
      std::fputs(dumpBlocks(out).c_str(), stdout);
      std::fputc('\n', stdout);
      return 1;
    }
    out["ok"] = true;
    out["block_lists"] = blockLists;
    std::fputs(dumpBlocks(out).c_str(), stdout);
    std::fputc('\n', stdout);
    return 0;
  }

  if (kind == "execute_do_source") {
    std::string source = req.value("source", std::string());
    std::vector<std::string> args;
    if (req.contains("args") && req["args"].is_array()) {
      for (const auto& a : req["args"]) args.push_back(a.get<std::string>());
    }
    try {
      doexec::ExecuteResult result = doexec::executeDoSource(source, session, dispatch, args,
                                                              /*progressOwner=*/false, std::nullopt);
      out["ok"] = true;
      out["blocks"] = result.blocks;
    } catch (const std::exception& e) {
      out["ok"] = false;
      out["driver_error"] = std::string("executeDoSource() threw: ") + e.what();
      std::fputs(dumpBlocks(out).c_str(), stdout);
      std::fputc('\n', stdout);
      return 1;
    }
    std::fputs(dumpBlocks(out).c_str(), stdout);
    std::fputc('\n', stdout);
    return 0;
  }

  if (req.contains("cmds")) {
    // Sequence mode: replay every command against the SAME session, in
    // order, collecting one block list per command. A dispatch()-level
    // exception aborts the whole sequence (matches how a crash would halt
    // real replay) — reported as a harness error, not swallowed.
    nlohmann::ordered_json blockLists = nlohmann::ordered_json::array();
    try {
      for (const auto& cmdJ : req["cmds"]) {
        std::string cmd = cmdJ.get<std::string>();
        blockLists.push_back(dispatch(cmd, session));
      }
    } catch (const std::exception& e) {
      out["ok"] = false;
      out["driver_error"] = std::string("dispatch() threw mid-sequence: ") + e.what();
      std::fputs(dumpBlocks(out).c_str(), stdout);
      std::fputc('\n', stdout);
      return 1;
    }
    out["ok"] = true;
    out["block_lists"] = blockLists;
    std::fputs(dumpBlocks(out).c_str(), stdout);
    std::fputc('\n', stdout);
    return 0;
  }

  std::string cmd = req.value("cmd", std::string());
  try {
    nlohmann::ordered_json blocks = dispatch(cmd, session);
    out["ok"] = true;
    out["blocks"] = blocks;
  } catch (const std::exception& e) {
    out["ok"] = false;
    out["driver_error"] = std::string("dispatch() threw: ") + e.what();
    std::fputs(dumpBlocks(out).c_str(), stdout);
    std::fputc('\n', stdout);
    return 1;
  }

  std::fputs(dumpBlocks(out).c_str(), stdout);
  std::fputc('\n', stdout);
  return 0;
}

}  // namespace piet
