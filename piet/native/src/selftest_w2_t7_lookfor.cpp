// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// T7 lane L1 selftest (lookfor) -- package-007 unblock.
//
// Every expected value below is the answer StataBE 19.5 gave to the same
// command on the same fixture (probe do-files a1..a5 under
// scratch-r150/t7-l1); none is derived from the handler. The arms:
//
//   name / label / mixed-case matching, multi-word and phrase search, the
//   once-only data-order r(varlist), the apostrophe/backtick strip applied to
//   labels, the ASCII-only case fold, the blank rules of a quoted word, "a
//   comma and a star are ordinary characters", the unterminated quote, r()
//   replacement (match, no match, bare), the no-data case, and that the table
//   printed is exactly `describe` of the matched variables.
#include "selftest_w2.h"

#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>
#include <vector>

#include "piet/dispatcher.h"
#include "piet/eval/errors.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

namespace {

using ojson = nlohmann::ordered_json;

int gLookforChecks = 0;
void lfCheck(bool cond, const std::string& label) {
  ++gLookforChecks;
  w2check(cond, label);
}

// Every block one dispatched line produced (an exception becomes an error block).
std::vector<ojson> blocksOf(piet::SessionState& session, const std::string& cmd) {
  std::vector<ojson> out;
  try {
    const ojson res = piet::dispatch(cmd, session);
    if (res.is_array()) {
      for (const auto& b : res) out.push_back(b);
    } else if (!res.is_null()) {
      out.push_back(res);
    }
  } catch (const std::exception& e) {
    out.push_back(ojson{{"type", "error"}, {"message", std::string("EXC:") + e.what()}});
  }
  return out;
}

// rc of one dispatched line (the last error block's coarse rc; "0" if none).
std::string rcOf(piet::SessionState& session, const std::string& cmd) {
  std::string rc = "0";
  for (const auto& b : blocksOf(session, cmd)) {
    if (b.is_object() && b.value("type", std::string()) == "error") rc = piet::eval::coarseRc(b);
  }
  return rc;
}

std::string errOf(piet::SessionState& session, const std::string& cmd) {
  std::string msg;
  for (const auto& b : blocksOf(session, cmd)) {
    if (b.is_object() && b.value("type", std::string()) == "error")
      msg = b.value("message", std::string());
  }
  return msg;
}

// Text of one `display ...` line.
std::string shown(piet::SessionState& session, const std::string& cmd) {
  std::string text;
  for (const auto& b : blocksOf(session, cmd)) {
    if (b.is_object() && b.value("type", std::string()) == "display")
      text += b.value("text", std::string());
  }
  return text;
}

// r(varlist) as the corpus reads it: ``local x `r(varlist)'`` and then the local.
// A refused read (an error block) must not look like an empty list, so it comes
// back as "ERROR:<message>".
std::string rVarlist(piet::SessionState& session) {
  std::string text;
  for (const auto& b : blocksOf(session, "display \"`r(varlist)'\"")) {
    if (!b.is_object()) continue;
    const std::string type = b.value("type", std::string());
    if (type == "error") return "ERROR:" + b.value("message", std::string());
    if (type == "display") text += b.value("text", std::string());
  }
  return text;
}

// The names a describe block lists, space-joined.
std::string describedNames(const ojson& block) {
  std::string out;
  if (!block.is_object() || !block.contains("variables")) return out;
  for (const auto& v : block.at("variables")) {
    if (!out.empty()) out += ' ';
    out += v.value("name", std::string());
  }
  return out;
}

// The describe blocks among a line's blocks.
std::vector<ojson> describeBlocks(const std::vector<ojson>& blocks) {
  std::vector<ojson> out;
  for (const auto& b : blocks)
    if (b.is_object() && b.value("type", std::string()) == "describe") out.push_back(b);
  return out;
}

bool anyError(const std::vector<ojson>& blocks) {
  for (const auto& b : blocks)
    if (b.is_object() && b.value("type", std::string()) == "error") return true;
  return false;
}

// One `lookfor` line: true when it printed exactly one describe table listing
// `names`, left r(varlist) == `names` and raised no error.
bool findsExactly(piet::SessionState& session, const std::string& cmd, const std::string& names) {
  const std::vector<ojson> blocks = blocksOf(session, cmd);
  const std::vector<ojson> tables = describeBlocks(blocks);
  return !anyError(blocks) && tables.size() == 1 && describedNames(tables[0]) == names &&
         rVarlist(session) == names;
}

// One `lookfor` line that finds nothing: nothing printed beyond the command echo
// (no table, no error), and r(varlist) empty.
bool findsNothing(piet::SessionState& session, const std::string& cmd) {
  const std::vector<ojson> blocks = blocksOf(session, cmd);
  for (const auto& b : blocks) {
    const std::string type = b.is_object() ? b.value("type", std::string()) : std::string();
    if (type != "echo") return false;
  }
  return rVarlist(session).empty();
}

// The a1/a2/a3 fixture, minus the labels that a command line cannot carry.
void buildFixture(piet::SessionState& session) {
  (void)piet::dispatch("clear", session);
  (void)piet::dispatch("set obs 3", session);
  (void)piet::dispatch("generate price_W2 = 1", session);
  (void)piet::dispatch("generate mpg_w2 = 2", session);
  (void)piet::dispatch("generate Other = 3", session);
  (void)piet::dispatch("label variable Other \"The WAVE2 thing\"", session);
  (void)piet::dispatch("generate x_BL = 4", session);
  (void)piet::dispatch("generate y = 5", session);
  (void)piet::dispatch("label variable y \"Price of Money\"", session);
  (void)piet::dispatch("generate zz = 6", session);
  (void)piet::dispatch("label variable zz \"foo bar\"", session);
  (void)piet::dispatch("generate u = 1", session);
  (void)piet::dispatch("generate v = 2", session);
  // Labels a command line cannot carry faithfully: an apostrophe and a backtick
  // (the tokenizer would read them as macro syntax), a non-ASCII capital, and a
  // run of two blanks. Stored the way `label variable` stores them in StataBE.
  session.varMeta["x_BL"].label = "it's a back`tick label";
  session.varMeta["u"].label = "\xC3\x84pfel und \xC3\x96l";  // "Äpfel und Öl"
  session.varMeta["v"].label = "a  double  space";
}

void testMatching(const std::string& workdir) {
  piet::SessionState session(workdir);
  buildFixture(session);

  // a1 T1: the corpus form -- a name suffix, two hits in data order.
  lfCheck(findsExactly(session, "lookfor _w2", "price_W2 mpg_w2"),
          "lookfor: `lookfor _w2` lists price_W2 mpg_w2 and sets r(varlist) (StataBE a1 T1)");
  // The 007 shape: the result is read into a local and used as a varlist.
  (void)piet::dispatch("lookfor _w2", session);
  (void)piet::dispatch("local suffix_vars `r(varlist)'", session);
  lfCheck(shown(session, "display \"[`suffix_vars']\"") == "[price_W2 mpg_w2]",
          "lookfor: ``local suffix_vars `r(varlist)'`` holds the matched names");

  // a1 T2/T3: a label hit, and the case fold in both directions.
  lfCheck(findsExactly(session, "lookfor wave2", "Other"),
          "lookfor: a word found in a variable LABEL (The WAVE2 thing) (a1 T2)");
  lfCheck(findsExactly(session, "lookfor _BL", "x_BL"),
          "lookfor: an upper-case word matches the lower-cased name x_BL (a1 T3)");
  lfCheck(findsExactly(session, "lookfor WAVE", "Other"),
          "lookfor: an upper-case word matches the lower-cased label (a1 T3)");
  lfCheck(findsExactly(session, "lookfor OTHER", "Other"),
          "lookfor: a name with a capital is found by any case of the word");

  // a1 T4: several words; a variable matching two words appears once, in DATA
  // order, whichever order the words are typed.
  lfCheck(findsExactly(session, "lookfor price mpg", "price_W2 mpg_w2 y"),
          "lookfor: `price mpg` -> price_W2 mpg_w2 y, each once (a1 T4)");
  lfCheck(findsExactly(session, "lookfor mpg price", "price_W2 mpg_w2 y"),
          "lookfor: `mpg price` lists the same data order (a1 T4)");

  // a1 T5: a quoted phrase is one word.
  lfCheck(findsExactly(session, "lookfor \"foo bar\"", "zz"),
          "lookfor: a quoted phrase is one word (a1 T5)");
  lfCheck(findsExactly(session, "lookfor foo zzzznomatch", "zz"),
          "lookfor: unquoted words are separate words; one hit is enough");
  lfCheck(findsExactly(session, "lookfor \"money\" \"foo bar\"", "y zz"),
          "lookfor: two quoted words (a1 T5)");
  lfCheck(findsExactly(session, "lookfor `\"price\"' `\"money\"'", "price_W2 y"),
          "lookfor: compound-quoted words (a2 W7)");
  lfCheck(findsExactly(session, "lookfor `\"of money\"'", "y"),
          "lookfor: a compound-quoted phrase is one word (a3 C4)");

  // a1 T7 / a5: apostrophes and backticks are deleted from the label first.
  lfCheck(findsExactly(session, "lookfor \"its a\"", "x_BL"),
          "lookfor: the apostrophe of the label is deleted before the search (a5 A1)");
  lfCheck(findsExactly(session, "lookfor backtick", "x_BL"),
          "lookfor: the backtick of the label is deleted before the search (a5 A2)");
  lfCheck(findsNothing(session, "lookfor \"it's\""),
          "lookfor: the word it's finds nothing, the label's apostrophe having been deleted "
          "(a5 A3)");
  lfCheck(findsNothing(session, "lookfor itsa"), "lookfor: `itsa` is not `its a` (a1 T7)");

  // a2: odd words.
  lfCheck(findsNothing(session, "lookfor price*"),
          "lookfor: `*` is a character, not a wildcard (a2 W2)");
  lfCheck(findsExactly(session, "lookfor price, mpg", "mpg_w2"),
          "lookfor: a comma belongs to its word: `price,` finds nothing, `mpg` finds mpg_w2 "
          "(a2 W3)");
  lfCheck(findsNothing(session, "lookfor price,") && findsNothing(session, "lookfor ,price"),
          "lookfor: `price,` and `,price` are literal words (a3 C1)");
  lfCheck(findsNothing(session, "lookfor 5"), "lookfor: a number is a word (a2 W16)");
  lfCheck(findsExactly(session, "lookfor    mpg     price", "price_W2 mpg_w2 y"),
          "lookfor: runs of blanks between words (a2 W15)");
  lfCheck(findsExactly(session, "lookfor \"price\" \"money", "price_W2 y"),
          "lookfor: an unterminated quote runs to the end of the line (a3 C3)");
  lfCheck(findsExactly(session, "lookfor \"price", "price_W2 y"),
          "lookfor: an unterminated quote on a single word (a2 W11)");
}

void testBlanksAndCase(const std::string& workdir) {
  piet::SessionState session(workdir);
  buildFixture(session);

  // a3 Q1: leading blanks of a word are dropped, trailing and interior kept.
  lfCheck(findsExactly(session, "lookfor \" money\"", "y"),
          "lookfor: a leading blank of a quoted word is dropped (a3 Q1)");
  lfCheck(findsExactly(session, "lookfor \"  money\"", "y"),
          "lookfor: two leading blanks are both dropped (a4 S3)");
  lfCheck(findsNothing(session, "lookfor \"money \""),
          "lookfor: a trailing blank is kept, so `money ` finds nothing (a3 Q1)");
  lfCheck(findsExactly(session, "lookfor \"of money\"", "y"),
          "lookfor: an interior blank is kept (a3 Q1)");
  lfCheck(findsExactly(session, "lookfor \"double  space\"", "v"),
          "lookfor: two interior blanks need two blanks in the label (a3 Q1)");
  lfCheck(findsNothing(session, "lookfor \"double space\""),
          "lookfor: one blank does not match two (a3 Q1)");

  // a2 W1 / a4 S1-S4: an empty or all-blank quoted word is the empty word, and
  // the empty word is a substring of every name.
  const std::string all = "price_W2 mpg_w2 Other x_BL y zz u v";
  lfCheck(findsExactly(session, "lookfor \"\"", all),
          "lookfor: the empty word matches every variable (a2 W1)");
  lfCheck(findsExactly(session, "lookfor \" \"", all),
          "lookfor: a quoted blank is the empty word (a4 S1)");
  lfCheck(findsExactly(session, "lookfor \"   \"", all),
          "lookfor: a quoted run of blanks is the empty word (a4 S2)");
  lfCheck(findsExactly(session, "lookfor \" \" zzzz", all),
          "lookfor: the empty word among other words (a4 S4)");

  // a3 U1: lower() folds A-Z only. "\xC3\x84" is the capital A-umlaut, which
  // stays itself, so the lower-case spelling does not match.
  lfCheck(findsExactly(session, "lookfor \"\xC3\x84PFEL\"", "u"),
          "lookfor: ASCII-only fold: the capital umlaut word matches the label (a3 U1)");
  lfCheck(findsNothing(session, "lookfor \"\xC3\xA4pfel\""),
          "lookfor: ASCII-only fold: the lower-case umlaut word does not (a3 U1)");
}

void testResultsAndErrors(const std::string& workdir) {
  piet::SessionState session(workdir);
  buildFixture(session);

  // The table is describe's own.
  {
    const std::vector<ojson> lf = describeBlocks(blocksOf(session, "lookfor _w2"));
    const std::vector<ojson> ds = describeBlocks(blocksOf(session, "describe price_W2 mpg_w2"));
    lfCheck(lf.size() == 1 && ds.size() == 1 && lf[0] == ds[0],
            "lookfor: the printed table is exactly `describe price_W2 mpg_w2` (a1 T1)");
  }

  // a1 T1: lookfor posts r(varlist) and nothing else -- describe's r(N)/r(k)
  // are not visible afterwards.
  (void)piet::dispatch("lookfor _w2", session);
  lfCheck(shown(session, "display \"[`r(N)'][`r(k)']\"") == "[][]",
          "lookfor: r(N)/r(k) of the inner describe are not left behind (a1 T1)");

  // a1 T6: no match prints nothing and CLEARS r() (an earlier summarize's r(N)).
  (void)piet::dispatch("quietly summarize zz", session);
  lfCheck(shown(session, "display \"[`r(N)']\"") == "[3]",
          "lookfor fixture: summarize posted r(N) = 3");
  lfCheck(findsNothing(session, "lookfor nonexistentzzz") &&
              shown(session, "display \"[`r(N)'][`r(varlist)']\"") == "[][]",
          "lookfor: a failed search clears r(); r(N) and r(varlist) read empty, not refused "
          "(a1 T6)");

  // a1 T8/T9: bare lookfor is r(198), "nothing to look for", and it too clears r().
  (void)piet::dispatch("quietly summarize zz", session);
  lfCheck(rcOf(session, "lookfor") == "198" &&
              errOf(session, "lookfor") == "nothing to look for",
          "lookfor: bare `lookfor` is r(198) `nothing to look for` (a1 T8)");
  (void)piet::dispatch("quietly summarize zz", session);
  (void)piet::dispatch("capture lookfor", session);
  lfCheck(shown(session, "display \"[`r(N)']\"") == "[]",
          "lookfor: even the failing bare form replaces r() (a1 T9)");
  lfCheck(rcOf(session, "lookfor   ") == "198", "lookfor: a blank-only line is r(198) too");

  // a2 W6: quietly keeps r(varlist), prints nothing.
  {
    const std::vector<ojson> q = blocksOf(session, "quietly lookfor price");
    lfCheck(describeBlocks(q).empty() && rVarlist(session) == "price_W2 y",
            "lookfor: `quietly lookfor` prints nothing and still sets r(varlist) (a2 W6)");
  }
  // a5 A4: capture keeps rc 0 and r(varlist).
  lfCheck(rcOf(session, "capture lookfor y") == "0" && rVarlist(session) == "y",
          "lookfor: `capture lookfor` keeps r(varlist) (a5 A4)");

  // a2 W17: a later describe replaces r(varlist) (describe has none).
  (void)piet::dispatch("lookfor mpg", session);
  (void)piet::dispatch("quietly describe y", session);
  lfCheck(rVarlist(session).empty(), "lookfor: a following describe replaces r()");
}

void testNoData(const std::string& workdir) {
  piet::SessionState session(workdir);
  (void)piet::dispatch("clear", session);
  // a2 N0 / a4 S6: StataBE treats no data in memory as "nothing to find" (rc 0,
  // no output), and checks the bare form first (rc 198, a4 S5). PIET's
  // dispatcher answers r(111) before the handler runs unless `lookfor` is on
  // its kNoDataCommands list (dispatcher.cpp, unit Z's file). Until it is, this
  // is a listed SKIP rather than a pinned divergence; the arms switch to real
  // checks by themselves the moment the dispatcher stops answering r(111).
  if (rcOf(session, "lookfor x") == "0") {
    lfCheck(findsNothing(session, "lookfor x"),
            "lookfor: with no data a word prints nothing and leaves r(varlist) empty (a2 N0, a4 S6)");
    lfCheck(rcOf(session, "lookfor") == "198",
            "lookfor: bare `lookfor` with no data is r(198) (a4 S5)");
  } else {
    w2skip("lookfor: with no data a word is rc 0 and bare is r(198) (a2 N0, a4 S5, a4 S6)",
           "dispatcher.cpp kNoDataCommands lacks `lookfor`, so the guard answers r(111) first "
           "(unit Z's file)");
  }
  // a3 C5: zero observations is just a dataset.
  (void)piet::dispatch("set obs 2", session);
  (void)piet::dispatch("generate price = 1", session);
  (void)piet::dispatch("label variable price \"Price of Money\"", session);
  (void)piet::dispatch("keep if 0", session);
  lfCheck(findsExactly(session, "lookfor money", "price"),
          "lookfor: a dataset with zero observations is searched (a3 C5)");
}

}  // namespace

void testW2T7Lookfor() {
  std::printf("[selftest] --- T7 L1 lookfor: name/label substring search, r(varlist) ---\n");
  namespace fs = std::filesystem;
  const std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_t7lookfor").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  testMatching(workdir);
  testBlanksAndCase(workdir);
  testResultsAndErrors(workdir);
  testNoData(workdir);

  fs::remove_all(workdir, ec);
  std::printf("[selftest] T7 L1 lookfor: %d checks\n", gLookforChecks);
}
