// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// twoway_parse.h — Layer-aware parser for Stata `twoway` commands (2.0
// analysis.do acceptance campaign). C++ port of
// piet/backend/piet/twoway_parse.py (~261 LOC oracle), byte-for-byte
// mirroring its algorithm so parenthesised multi-layer twoway
// `(rarea ...) (connected ...) (scatter ...)` parses identically to the
// Python backend.
//
// Operates on the RAW post-keyword string — the slice of the command line
// that follows the `twoway` (or `graph twoway`) keyword — so it sees the
// parenthesised layer groups intact (the tokenizer's ParsedCommand::varlist
// already flattens them, which is why this is a SEPARATE parse over the raw
// text rather than a reuse of ParsedCommand).
//
// Public API mirrors the oracle exactly:
//   parseTwoway(rawAfterCmd) -> {layers, globalOpts, ifExpr, inRange}
//   hasToplevelParens(rawAfterCmd) -> bool   (fast pre-check for the dispatcher)
#ifndef PIET_NATIVE_TWOWAY_PARSE_H
#define PIET_NATIVE_TWOWAY_PARSE_H

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "piet/eval/tokenizer.h"

namespace piet::twoway {

// One layer parsed from a `twoway` command (mirrors twoway_parse.py's
// LayerSpec dataclass).
struct LayerSpec {
  std::string plottype;                                       // lower-cased plot-type keyword
  std::vector<std::string> varlist;                            // variable names, Stata order
  std::optional<std::string> ifExpr;                            // per-layer `if`, stripped of "if"
  std::vector<std::pair<std::string, eval::OptValue>> opts;    // per-layer options
  // serset-twoway IN: the per-layer `in` range (`(scatter y x in 3/10)`).
  std::optional<std::string> inRange;
  // serset-twoway WT: the per-layer weight clause, `[type = exp]` cut out of the
  // layer before its if/in (it may stand before or after them). weightType is
  // the full name (aweight|fweight|pweight|iweight; `[w=]` -> aweight, twoway's
  // default), "" when none; weightExp the expression text.
  std::string weightType;
  std::string weightExp;
  // serset-twoway PARB: the layer's text before its comma with if/in/weight
  // removed and the plot type stripped -- what function/scatteri read instead
  // of a varlist.
  std::string rawArgs;
};

struct ParseResult {
  std::vector<LayerSpec> layers;                                // first = bottom z-layer
  std::vector<std::pair<std::string, eval::OptValue>> globalOpts;  // post-first-comma options
  std::optional<std::string> ifExpr;                            // global `if` (applies to ALL layers)
  std::optional<std::string> inRange;                           // global `in` range
};

// Parses everything that follows the `twoway` keyword. Mirrors
// twoway_parse.py's parse_twoway() exactly (see its docstring for the
// bare-single-layer vs parenthesised-multi-layer branch semantics).
ParseResult parseTwoway(const std::string& rawAfterCmd);

// serset-twoway WT: cuts the first weight clause `[word = exp]` (a `[` at the
// start or after a blank, outside quotes and parentheses) out of `text`,
// returning the full weight type ("" when there is none; `w`/`weight` -> the
// given default) and the expression. `text` is left without the clause.
struct WeightClause {
  std::string type;
  std::string exp;
  std::string typed;  // the word as written
};
WeightClause cutWeightClause(std::string& text, const std::string& defaultType = "aweight");

// BS-053 / serset-twoway PARA: `a || b || , opts` -> `(a) (b), opts` (top-level
// `||` outside quotes and parentheses; a segment with no plot holds global
// options). Unchanged when there is no top-level `||`.
std::string expandDoublePipes(const std::string& rawArgs);

// Fast dispatcher pre-check: true if `rawAfterCmd` contains at least one
// top-level `(...)` group before the first structural comma. Mirrors
// twoway_parse.py's has_toplevel_parens().
bool hasToplevelParens(const std::string& rawAfterCmd);

}  // namespace piet::twoway

#endif  // PIET_NATIVE_TWOWAY_PARSE_H
