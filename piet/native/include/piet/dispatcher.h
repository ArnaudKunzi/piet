// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// dispatcher — the central dispatch function, C++ port of
// piet/backend/piet/dispatcher.py's dispatch(): raw Stata command string ->
// list[ResultBlock] (see CONTRACT.md's ResultBlock union).
//
// M2 scope (ROADMAP "M2 — glue + vertical slice"): the block-executor
// pipeline skeleton mirroring the oracle's dispatch() entry — step-0a/0b
// (tryCaptureDefinition -> hasMacroRefs -> expandMacros, the M1 contract)
// then command routing to a SMALL vertical slice of handlers:
//   use, gen, replace, describe, summarize, regress (+ reg/feols aliases).
// Every other command falls through to unsupported_command("piet_gap") —
// deliberately, so the corpus subset this milestone does NOT cover reports
// as an explicit RED/out-of-scope count, never a silent wrong answer.
// TODO(M3): add handlers to the SAME routing table (see dispatcher.cpp's
// "M3 extension point" comment) without needing to touch this pipeline.
#ifndef PIET_NATIVE_DISPATCHER_H
#define PIET_NATIVE_DISPATCHER_H

#include <string>

#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

namespace piet {

// Returns [EchoResult, ...ResultBlocks] as an ordered_json array — the same
// shape the Python oracle's dispatch() returns (list[dict], JSON-encoded).
nlohmann::ordered_json dispatch(const std::string& rawCmd, SessionState& session);

// AER scope-2 A1 — what dispatcher.cpp's option-name resolution point did to
// ONE (command, option) pair. `stage` names the LAST pass that rewrote the key;
// see resolveOptionKeyForCommand() for the three passes and what each is.
struct OptKeyResolution {
  std::string resolved;             // the spelling the handler will actually see
  bool exactOnly = false;           // OptValue::exactOnly: no unique-prefix fallback
  // "floor" is the NARROWING pass (AER scope-2 B4): the key is a proper prefix
  // of an option whose minimum abbreviation a live capture measured as longer,
  // so it was deliberately left unresolved and `exactOnly` set.
  const char* stage = "unchanged";  // "minimums" | "floor" | "vocab" | "unchanged"
};

// THE option-name resolution rule, exported so there is exactly one copy of it.
// `cmd` must already be the CANONICAL command name (registry::resolveCanonical
// of the lowercased token) — that is what dispatchImpl holds when it calls this,
// and resolving against the typed spelling would consult a different table.
// dispatchImpl runs this over every parsed option key; `piet_native
// --dump-optvocab --resolve` runs the same function so it reports the
// dispatcher's real answer rather than a reimplementation of it.
OptKeyResolution resolveOptionKeyForCommand(const std::string& cmd, const std::string& key);

}  // namespace piet

#endif  // PIET_NATIVE_DISPATCHER_H
