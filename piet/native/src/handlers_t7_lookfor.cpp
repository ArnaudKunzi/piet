// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// `lookfor word [word ...]` -- the variables whose name or variable label contains
// a search word, as a case-insensitive SUBSTRING. Package-007 unblock (T7), lane
// L1. Registry row, dispatcher arm, handlers.h declaration, effects/floors rows
// and the pangram line landed with unit Z; this file is the lane's alone.
//
// Implements `lookfor` per the help file; identified against StataBE probes
// (StataNow 19.5 BE, scratch-r150/t7-l1 a1..a5). What the probes pinned down:
//
//   * Every word on the line is a search word -- the line is NOT a varlist, an
//     option list or a qualifier. `lookfor price, mpg` searches for the two
//     words `price,` and `mpg` (a comma is an ordinary character of a word),
//     and `if` is a word too. So the words come from the text after the
//     command word, not from `parsed.varlist`/`parsed.opts`.
//   * Words split on blanks; `"a phrase"` is one word with the quotes removed
//     (eval::scanOneToken, bind mode, the shared scanner); an unterminated
//     quote runs to the end of the line. A quoted `""` or a quoted run of
//     blanks reaches the search as the EMPTY word, which matches every
//     variable. Leading blanks of a word are dropped, trailing and interior
//     blanks are kept (`"money "` finds nothing, `"double  space"` needs both
//     blanks).
//   * A variable matches when ANY word is a substring of its lowercased name or
//     of its lowercased label. The case fold is ASCII only (`lower()`), so an
//     accented capital in the data is NOT folded to its lowercase form.
//   * Before the label is searched, every apostrophe and every backtick is
//     deleted from it (`it's a back`tick` is searched as `its a backtick`).
//   * A matched variable is listed once, in DATA order, whichever word hit it.
//   * Matches: the `describe` table of the matched variables (the same table
//     `describe v1 v2 ...` prints) and r(varlist) = the matched names. No
//     match: nothing is printed and r() is left empty. r() is replaced on
//     EVERY call (bare `lookfor`, which is r(198), included), so an earlier
//     command's r(N) never survives a lookfor. lookfor is r-class and posts
//     only varlist: describe's own r(N)/r(k) are not visible afterwards, and
//     the declaration below keeps PIET from refusing a read of them.
//
// WHAT THIS FILE DOES NOT CONTROL (both live in unit Z's files, reported, not
// edited): (1) with NO data in memory StataBE answers rc 0 and prints nothing
// (a bare `lookfor` is still r(198)), but `lookfor` is not on the dispatcher's
// kNoDataCommands list, so the guard answers r(111) before this handler runs;
// (2) the registry row allows no `in` range and no weights, so the tokenizer
// consumes a trailing `in <token>` or a `[w=..]` before the handler sees it,
// where StataBE searches them as words.
//
// The describe table is produced by handlers::runDescribe -- the same function
// the `describe` command uses -- so the two cannot drift; nothing here formats
// a table.
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "piet/datastore.h"
#include "piet/eval/errors.h"
#include "piet/eval/session.h"
#include "piet/eval/tokenizer.h"
#include "piet/handlers.h"
#include "piet/session_state.h"
#include "piet/strutil.h"

namespace piet::handlers {

namespace {

using ojson = nlohmann::ordered_json;

// The search words of the text that follows the command word, each lowercased
// (ASCII), left-trimmed and ready for a substring test.
std::vector<std::string> searchWords(const std::string& text) {
  std::vector<std::string> words;
  std::string rest = text;
  // A remainder of nothing but blanks ends the scan; a quoted empty word (`""`)
  // is not blank text, so it is still scanned and kept.
  while (!strutil::trim(rest).empty()) {
    auto [token, next] = eval::scanOneToken(rest, /*bind=*/true);
    const size_t lead = token.find_first_not_of(" \t");
    words.push_back(strutil::lower(lead == std::string::npos ? std::string() : token.substr(lead)));
    rest = std::move(next);
  }
  return words;
}

// Every variable name in data order: the resident card when there is one, the
// datastore otherwise (the order `ds` and `describe` use). Empty with no data.
std::vector<std::string> allVariableNames(SessionState& session) {
  std::vector<std::string> names;
  if (!session.hasData()) return names;
  if (const std::vector<VarInfo>* card = session.residentReadCard()) {
    names.reserve(card->size());
    for (const auto& vi : *card) names.push_back(vi.name);
    return names;
  }
  if (Datastore* ds = session.datastore()) names = ds->names();
  return names;
}

// The variable label, lowercased, with apostrophes and backticks removed.
std::string searchableLabel(const SessionState& session, const std::string& name) {
  const auto it = session.varMeta.find(name);
  if (it == session.varMeta.end() || !it->second.label.has_value()) return std::string();
  std::string out;
  out.reserve(it->second.label->size());
  for (const char c : *it->second.label)
    if (c != '\'' && c != '`') out.push_back(c);
  return strutil::lower(std::move(out));
}

}  // namespace

ojson runLookfor(const eval::ParsedCommand& parsed, SessionState& session) {
  // The text after the command word, as typed (macros are already expanded and
  // comments stripped by the time a command reaches a handler).
  std::string rest = parsed.raw;
  {
    size_t i = rest.find_first_not_of(" \t");
    if (i == std::string::npos) i = rest.size();
    while (i < rest.size() && rest[i] != ' ' && rest[i] != '\t') ++i;
    rest.erase(0, i);
  }
  const std::vector<std::string> words = searchWords(rest);

  // r-class: r() is replaced on entry, an error included. The declaration is
  // what lets a later read of an r() name lookfor does not post (r(N), or
  // r(varlist) after a search that found nothing) answer empty as StataBE does,
  // instead of being refused as "a result set PIET does not record".
  eval::replaceRClass(session.eval);
  eval::declareRPosted(session.eval, "lookfor", {});

  // A line of nothing but blanks has no words at all; a quoted empty word
  // (`lookfor ""`) is still a word and is searched.
  if (words.empty()) return ojson(eval::engineError("nothing to look for", "syntax_error"));

  std::vector<std::string> matched;
  for (const std::string& name : allVariableNames(session)) {
    const std::string lname = strutil::lower(name);
    const std::string llabel = searchableLabel(session, name);
    for (const std::string& w : words) {
      if (lname.find(w) != std::string::npos || llabel.find(w) != std::string::npos) {
        matched.push_back(name);
        break;
      }
    }
  }
  if (matched.empty()) return ojson::array();

  std::string joined;
  for (size_t i = 0; i < matched.size(); ++i) {
    if (i) joined += ' ';
    joined += matched[i];
  }

  ojson table = ojson(runDescribe(std::optional<std::vector<std::string>>(matched), session));
  session.eval.rStrings.set("varlist", joined);
  eval::declareRPosted(session.eval, "lookfor", {"varlist"});
  return table;
}

}  // namespace piet::handlers
