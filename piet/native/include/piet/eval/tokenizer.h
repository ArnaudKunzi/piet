// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Stata-subset tokenizer — C++ port of piet/backend/piet/router/tokenizer.py.
//
// Order of operations (per BACKEND.md Tokenizer, mirrored verbatim):
//   1. Macro / control-flow detection -> TokenizerError FIRST, before any
//      other parsing, so an unresolved macro can't silently alter a cmd.
//   2. Strip comments (* lines, inline //, /* */), join continuations (///).
//   3. Split on first unparenthesised/unquoted comma to isolate
//      `[cmd varlist [if] [in] [wt]]` | `opts`.
//   4. Extract `if expr`, `in range`, `[weight]` from the pre-comma portion.
//   5. Return ParsedCommand.
//
// The tokenizer is purely structural — it does NOT look up command names or
// validate whether variables exist (that is the translate/commands layer's
// job, ported at M2/M3).
#ifndef PIET_NATIVE_EVAL_TOKENIZER_H
#define PIET_NATIVE_EVAL_TOKENIZER_H

#include <map>
#include <optional>
#include <regex>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "vendor/nlohmann/json.hpp"

namespace piet::eval {

// One parsed option value: either a bool flag (true) or a string value
// (for `key(val)` forms). Mirrors Python's `dict[str, str | bool]`.
struct OptValue {
  bool isBool = false;
  bool boolValue = false;
  std::string strValue;
  // BUG-264/BUG-335: set by the dispatcher for commands whose ENTIRE option
  // vocabulary has been oracle-adjudicated (kOptMinComplete). It tells
  // rejectUnknownOptions not to prefix-resolve this key, so the accepted
  // spelling set is exactly {prefix p : |p| >= measured minimum} — i.e. Stata's.
  // Carried on the value rather than passed as an argument because the ~79
  // rejectUnknownOptions call sites live inside their own command arms and do
  // not thread the command name; the dispatcher is the one place that knows it.
  bool exactOnly = false;

  static OptValue ofBool(bool v) {
    OptValue o;
    o.isBool = true;
    o.boolValue = v;
    return o;
  }
  static OptValue ofString(std::string v) {
    OptValue o;
    o.isBool = false;
    o.strValue = std::move(v);
    return o;
  }
};

struct ParsedCommand {
  std::string cmd;
  std::vector<std::string> varlist;
  std::optional<std::string> ifExpr;
  std::optional<std::string> inRange;
  std::optional<std::string> weightType;
  std::optional<std::string> weightVar;
  // The weight expression as Stata keeps it for e(wexp): the text after `=` with
  // its leading blanks removed and its trailing ones kept (`[aw=  wf ]` -> "wf ",
  // w2aj_qualifiers qf_rw_aw_wide). weightVar is the same text stripped. Not part
  // of toJson().
  std::optional<std::string> weightExp;
  // Preserves insertion order like Python dict does (tests may rely on it
  // for display, though equality checks are order-independent by key).
  std::vector<std::pair<std::string, OptValue>> opts;
  std::string raw;
  // The pre-comma text after the command word, qualifiers removed, as typed:
  // `varlist` is this text split on blanks, which collapses a run of blanks
  // inside a string literal (`generate b = "x  y"`). generate/replace read
  // their expression from here (FU-639/FU-652). Not part of toJson().
  std::string argText;

  nlohmann::json toJson() const;
};

// Thrown by tokenize() on rejection. Carries the pre-built error block,
// exactly like Python's TokenizerError(block).
struct TokenizerError : std::runtime_error {
  nlohmann::json block;
  explicit TokenizerError(nlohmann::json b)
      : std::runtime_error(b.value("message", std::string())), block(std::move(b)) {}
};

// Shared quote-aware token scanner (tokenizer.py scan_one_token). Used by
// gettoken and (eventually) program arg-splitting.
//
// Returns {token, remainder}. `bind` strips one level of compound quoting
// (`"..."' -> ...) instead of preserving the outer delimiters.
std::pair<std::string, std::string> scanOneToken(const std::string& s, bool bind = false);

// Index of the first comma not inside parens/brackets/quotes (including
// Stata compound double quotes `"..."'). -1 if none.
long long findUnquotedComma(const std::string& s);

// Per-character mask: true where the character at that index lies inside a
// quoted span (plain `"..."` or Stata compound `` `"..."' ``) — same quote-
// nesting rules as findUnquotedComma. Exposed so any if/in/weight-style
// qualifier scan can skip matches that only appear inside a string literal
// (BUG-115: `display "in failprog"` / `display "if you see this"` were
// mis-split because the qualifier regexes ran quote-blind).
std::vector<bool> quotedSpanMask(const std::string& s);

// Stata's storage rule for the lines of a loop body (foreach/forvalues/while)
// and of a program body: every run of blanks (spaces and tabs) outside double
// quotes and compound quotes becomes ONE blank, before any macro is expanded
// (StataBE 19.5 probes, w2at_table_writers wt_x01: `tex `lab' &  ...` inside a
// foreach writes `x1 & .125`; top-level lines and a top-level `if {}` body keep
// their blanks). Leading indentation is left as it is. Idempotent.
std::string collapseUnquotedBlanks(const std::string& s);

// std::regex_search, but skips any match whose span overlaps a quoted
// string (per quotedSpanMask) — a drop-in replacement at if/in/weight
// qualifier call sites so a qualifier-shaped substring inside a quoted
// literal is never mistaken for a real qualifier. Returns false (leaving
// `out` untouched, same as regex_search) if no unquoted match exists.
bool searchUnquoted(const std::string& s, const std::regex& re, std::smatch& out);

// Parses the post-comma option-list portion of a command (e.g. `title("x")
// replace width(2400)`) into an ordered {name -> OptValue} list. Exposed
// (moved out of tokenizer.cpp's anonymous namespace) so other modules that
// parse a comma-delimited option tail without a full tokenize() call — e.g.
// twoway.cpp's per-layer `(plottype var, layer_opts)` interior — can reuse
// the SAME quote/paren-aware option grammar rather than re-deriving it
// (mirrors twoway_parse.py's own "reuse, don't reinvent" docstring note).
std::vector<std::pair<std::string, OptValue>> parseOpts(const std::string& optStr);

// `text` with its first unquoted weight clause removed -- the one weight grammar
// tokenize() reads (`[aw = x]`, `[ w= x ]`, `[cell=x]`, ...), for a handler that
// re-reads its own raw command text (collapse's stat list). Unchanged when there
// is no clause.
std::string removeWeightClause(const std::string& text);

// Parse one Stata command line into a ParsedCommand. Throws TokenizerError
// on rejection (matches Python's raise semantics exactly).
ParsedCommand tokenize(const std::string& line);

// Strip comments and join /// continuations in a .do source string. Returns
// one (possibly empty) output line per input line.
std::vector<std::string> preprocessDo(const std::string& source);

}  // namespace piet::eval

#endif  // PIET_NATIVE_EVAL_TOKENIZER_H
