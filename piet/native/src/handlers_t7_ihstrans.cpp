// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// `ihstrans` (SSC, Jan Helmdag) -- inverse hyperbolic sine transform into new
// ihs_<var> columns. Package-007 unblock (T7), lane L2. Registry, dispatcher arm,
// handlers.h declaration, effects/floors rows, NOTICE line and the pangram line
// landed with unit Z; this file is the lane's alone.
//
// METHOD. Identified by reading the public ado (method identification only; the
// package is not installed here) and against StataBE probes of the Stata
// primitives it leans on (`_xt`, `regexr`, `marksample, strok`, `generate`,
// `summarize` on an all-missing variable). The command is a loop over a variable
// list, and each step is one ordinary Stata statement:
//
//     generate double <pre><v> = log(<v>+sqrt(<v>^2+1)) [if] [in]
//     label variable <pre><v> "IHS trans. values of <v>"
//     quietly summarize <pre><v>            // drop the column when r(N) == 0
//
// NOTHING HERE COMPUTES A VALUE. Every step is re-dispatched, the way
// `runMarksample` re-dispatches its `generate`, so the transform is evaluated by
// the one expression evaluator (the formula is written out, NOT asinh(): the two
// are not bit-identical), the "(N missing values generated)" line is generate's
// own, the storage type is generate's `double`, the label lands in the label
// store, `ihs_x already defined` (r(110)) and an over-long name (r(198)) are
// generate's own errors, and r() ends up holding the last `summarize`, exactly as
// it does after the ado (a non-r-class program leaves the r() of its last r-class
// command behind; StataBE probe).
//
// WHAT THE HANDLER OWNS is the list arithmetic around the loop, and that is where
// the ado is quirky enough that the quirks are part of the contract:
//
//   * A variable is "numeric" iff its display format has no lowercase `s`
//     (regexm(format, "s") == 0). Formats are read from the one resolver
//     (SessionState::displayFormat), so a `format` command is honoured.
//   * `keepusing()` and the xtset panel/time variables are removed from the
//     numeric list by regexr(list, name, ""): ONE literal, SUBSTRING removal of
//     the FIRST match in the space-joined list. So `keepusing(a c)` is a no-op
//     unless "a c" occurs contiguously, and a time variable named `t` also bites
//     the `t` inside `tenure` ("enure" is then not a variable: r(111)). StataBE
//     probe: regexr("xx x y","x","") = "x x y"; regexr("a b c","a c","") = "a b c".
//     Variable names carry no regex metacharacter, so a plain find() is the same
//     operation. Reproduced as is: a clean exclusion here would answer
//     differently from Stata on exactly the inputs where the ado misbehaves.
//   * The panel/time variables are excluded only when `_xt` succeeds, i.e. when
//     data are xtset with a PANEL variable (StataBE: `_xt` is r(459) for a
//     tsset-only or undeclared dataset, so a tsset time variable is NOT removed).
//     tvar is removed before ivar.
//   * `casewise` marks the sample as `marksample touse, strok` does: `if`/`in`
//     and no missing value in ANY variable of the full varlist (empty strings are
//     missing under strok), and an empty sample is r(2000). Without `casewise`
//     `if`/`in` go straight onto each generate.
//   * A time-series-operator varlist (`L.x`) is r(198): the ado's `: format L.x`
//     fails (StataBE probe) before anything is generated.
#include <cstddef>
#include <exception>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "piet/datastore.h"
#include "piet/dispatcher.h"
#include "piet/eval/errors.h"
#include "piet/eval/session.h"
#include "piet/eval/tokenizer.h"
#include "piet/handlers.h"
#include "piet/loops.h"
#include "piet/session_state.h"
#include "piet/strutil.h"

// ---------------------------------------------------------------------------
// OPTION VOCABULARY -- namespace scope, so it is registered before any dispatch
// (a function-local static would construct too late, and `canonicalizeForCommand`
// would then drop an abbreviation the reject net had accepted).
//
// ihstrans.ado's own `syntax` line is
//   syntax [varlist(ts)] [if] [in], [Keepusing(varlist)] [Prefix(string)] [Casewise]
// so the capitalised stems give the minimum abbreviations k, p and c. All three
// are READ here, and the package has no others, so `unimplemented` is empty. The
// one-letter spellings are below resolveOptionAbbrev's two-character floor, which
// is why they are DECLARED as aliases rather than left to the prefix rule.
namespace {
const piet::eval::OptVocab& kVocabIhstrans = piet::eval::registerOptVocab(
    {"ihstrans"}, {{"keepusing", "prefix", "casewise"},
                   {},
                   {{"k", "keepusing"}, {"p", "prefix"}, {"c", "casewise"}}});
}  // namespace

namespace piet::handlers {

namespace {

using ojson = nlohmann::ordered_json;

// A USER SYNTAX error: "syntax_error" is what coarseRc maps to rc 198; the
// default "engine_error" would become rc 1.
ojson syntaxError(const std::string& message) {
  return ojson(eval::engineError(message, "syntax_error"));
}

// An error whose Stata rc is known but which coarseRc's message rules cannot
// recognise. `reason` is the channel that carries an exact rc and outranks the
// coarse mapping.
ojson errorWithRc(const std::string& message, int rc) {
  return ojson(eval::errorBlock("engine_error", message, std::nullopt, std::nullopt, std::nullopt,
                                "r(" + std::to_string(rc) + ")"));
}

bool isErrorBlock(const ojson& b) {
  return b.is_object() && b.value("type", std::string()) == "error";
}

// An error block inside a dispatched command's result, which is an array of
// blocks (or, defensively, one bare block).
const ojson* firstError(const ojson& result) {
  if (isErrorBlock(result)) return &result;
  if (result.is_array()) {
    for (const auto& b : result)
      if (isErrorBlock(b)) return &b;
  }
  return nullptr;
}

// Append a dispatched command's blocks to `out`, flattening one level. Every
// dispatch() leads its result with an "echo" block of the line it ran; the ado's
// own statements are not echoed in Stata's log (only the line the user typed is,
// and that one the outer dispatch already echoed), so the nested echoes go. ERROR
// blocks stay behind too: a failure is reported once, last, by the caller's
// fail(), after the blocks that preceded it.
void appendBlocks(ojson& out, const ojson& result) {
  auto keep = [](const ojson& b) {
    if (!b.is_object()) return false;
    const std::string type = b.value("type", std::string());
    return type != "echo" && type != "error";
  };
  if (result.is_array()) {
    for (const auto& b : result)
      if (keep(b)) out.push_back(b);
  } else if (keep(result)) {
    out.push_back(result);
  }
}

std::vector<std::string> splitWs(const std::string& s) {
  std::vector<std::string> out;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    const size_t start = i;
    while (i < s.size() && s[i] != ' ' && s[i] != '\t') ++i;
    if (i > start) out.push_back(s.substr(start, i - start));
  }
  return out;
}

std::string joinWs(const std::vector<std::string>& v) {
  std::string out;
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) out += ' ';
    out += v[i];
  }
  return out;
}

// regexr(hay, needle, "") for a needle with no regex metacharacter: remove the
// FIRST literal occurrence, anywhere in the string (substring, not token).
std::string removeFirstOccurrence(std::string hay, const std::string& needle) {
  if (needle.empty()) return hay;
  const size_t pos = hay.find(needle);
  if (pos != std::string::npos) hay.erase(pos, needle.size());
  return hay;
}

// An option that takes a value: nullopt when absent, the dequoted text when
// given; `*bad` is set when it was given without one (`prefix` bare).
std::optional<std::string> valuedOption(const eval::ParsedCommand& parsed, const std::string& key,
                                        bool* bad) {
  for (const auto& [k, v] : parsed.opts) {
    if (k != key) continue;
    if (v.isBool) {
      *bad = true;
      return std::nullopt;
    }
    std::string s = strutil::trim(v.strValue);
    if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') ||
                          (s.front() == '\'' && s.back() == '\'')))
      s = strutil::trim(s.substr(1, s.size() - 2));
    return s;
  }
  return std::nullopt;
}

// The dataset's variables in column order, each with the format the one
// resolver reports for it.
struct VarCard {
  std::vector<std::string> names;
  std::vector<std::string> formats;                 // parallel to `names`
  std::unordered_map<std::string, size_t> indexOf;  // name -> position
};

bool loadCard(SessionState& session, VarCard* card) {
  Datastore* ds = session.datastore();
  if (ds == nullptr) return false;
  for (const VarInfo& vi : ds->formatCard(std::nullopt)) {
    card->indexOf.emplace(vi.name, card->names.size());
    card->names.push_back(vi.name);
    card->formats.push_back(session.displayFormat(vi));
  }
  return true;
}

// A varlist as `syntax varlist` returns it: wildcards and ranges expanded, a
// unique prefix completed to the full name, every name required to exist.
// nullopt on success, else the error block (r(111), or r(198) for a token that
// is not a plain name -- a time-series operator).
std::optional<ojson> resolveNames(const std::string& spec, const VarCard& card,
                                  SessionState& session, std::vector<std::string>* out) {
  if (spec.find_first_not_of(" \t") == std::string::npos) return std::nullopt;
  loops::VarlistExpandResult vr = loops::expandVarlist(spec, session);
  if (vr.isError) return ojson(vr.error);
  for (const std::string& tok : vr.tokens) {
    if (tok.find('.') != std::string::npos)
      return syntaxError("ihstrans: `" + tok +
                         "' -- time-series operators are not allowed (the new variable would be "
                         "named after the operator)");
    if (card.indexOf.count(tok) != 0) {
      out->push_back(tok);
      continue;
    }
    std::vector<std::string> hits;
    for (const std::string& n : card.names)
      if (n.size() > tok.size() && n.compare(0, tok.size(), tok) == 0) hits.push_back(n);
    if (hits.size() == 1) {
      out->push_back(hits.front());
      continue;
    }
    if (hits.size() > 1) return errorWithRc(tok + " ambiguous abbreviation", 111);
    return errorWithRc("variable " + tok + " not found", 111);
  }
  return std::nullopt;
}

}  // namespace

nlohmann::ordered_json runIhstrans(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabIhstrans.known,
                                            kVocabIhstrans.unimplemented))
    return ojson(*err);

  // ---- options (parsed before the data is looked at, so a malformed option is
  // the same syntax error whatever the dataset; at the console the dispatcher's own
  // no-data gate answers r(111) before this arm is reached) ---------------------
  bool bad = false;
  const std::optional<std::string> keepusingText = valuedOption(parsed, "keepusing", &bad);
  if (bad) return syntaxError("ihstrans: option keepusing() requires a varlist");
  const std::optional<std::string> prefixText = valuedOption(parsed, "prefix", &bad);
  if (bad) return syntaxError("ihstrans: option prefix() requires a string");
  bool casewise = false;
  for (const auto& [k, v] : parsed.opts) {
    if (k != "casewise") continue;
    if (!v.isBool) return syntaxError("ihstrans: option casewise not allowed with an argument");
    casewise = v.boolValue;
  }
  const std::string pre = (prefixText.has_value() && !prefixText->empty()) ? *prefixText : "ihs_";

  if (!session.hasData()) return ojson(eval::noDataLoaded());
  VarCard card;
  try {
    if (!loadCard(session, &card)) return ojson(eval::noDataLoaded());
  } catch (const std::exception& e) {
    return ojson(eval::engineError(std::string("ihstrans: could not read the data -- ") + e.what(),
                                   "data_io"));
  }
  if (card.names.empty()) return errorWithRc("no variables defined", 111);

  // ---- the full varlist: what was typed, or every variable ---------------------
  std::vector<std::string> varlist;
  {
    std::string spec;
    for (const std::string& t : parsed.varlist) spec += t + " ";
    if (spec.find_first_not_of(" \t") == std::string::npos) {
      varlist = card.names;
    } else if (auto err = resolveNames(spec, card, session, &varlist)) {
      return *err;
    }
  }
  std::vector<std::string> keepusing;
  if (keepusingText.has_value()) {
    if (auto err = resolveNames(*keepusingText, card, session, &keepusing)) return *err;
  }

  // ---- the numeric list, then the ado's regexr exclusions -------------------
  std::vector<std::string> numeric;
  for (const std::string& v : varlist) {
    const std::string& fmt = card.formats[card.indexOf.at(v)];
    if (fmt.find('s') == std::string::npos) numeric.push_back(v);
  }
  std::string list = joinWs(numeric);
  if (!keepusing.empty()) list = removeFirstOccurrence(list, joinWs(keepusing));
  // `capture _xt` succeeds only for data xtset with a panel variable; then tvar
  // (when there is one) goes first and ivar second.
  if (session.panelvar.has_value() && !session.panelvar->empty()) {
    if (session.timevar.has_value() && !session.timevar->empty())
      list = removeFirstOccurrence(list, *session.timevar);
    list = removeFirstOccurrence(list, *session.panelvar);
  }
  const std::vector<std::string> targets = splitWs(list);

  // ---- the qualifier text every generate carries ------------------------------
  std::string qual;
  if (parsed.ifExpr.has_value() && !parsed.ifExpr->empty()) qual += " if " + *parsed.ifExpr;
  if (parsed.inRange.has_value() && !parsed.inRange->empty()) qual += " in " + *parsed.inRange;

  ojson out = ojson::array();
  std::string touse;  // the casewise marker's column; dropped on every exit path

  // The casewise marker goes with a plain `drop`, not a quiet one: `quietly`
  // swallows the data_mutation block, and the last such block is what the
  // variables list shows, so a quiet drop would leave the marker on display.
  // It runs on the failure path too (a dispatch that threw may throw again), so a
  // second exception is swallowed here: the error being reported is the first one.
  auto dropMarker = [&]() {
    if (touse.empty()) return;
    const std::string marker = touse;
    touse.clear();
    try {
      SessionState::TempDropScope td(session);
      appendBlocks(out, dispatch("drop " + marker, session));
    } catch (const std::exception&) {
    }
  };

  // Everything generated before a failure stays (the ado does not unwind), so the
  // error is appended to the blocks so far rather than replacing them -- and it
  // stays LAST, after the marker's cleanup (appendBlocks never copies an error).
  auto fail = [&](const ojson& err) -> ojson {
    dropMarker();
    out.push_back(err);
    return out;
  };

  try {
    if (casewise) {
      // marksample touse, strok -- `if`/`in` plus no missing value (an empty
      // string is missing) in any variable of the FULL varlist. The same
      // conjunction runMarksample builds, one evaluator for the lot; it cannot be
      // called here because it reads the `syntax` locals of an enclosing program.
      Datastore* ds = session.datastore();
      std::string name = "__ihs_touse";
      for (int n = 0; ds != nullptr && ds->has(name); ++n) name = "__ihs_touse" + std::to_string(n);
      std::string cond;
      for (const std::string& v : varlist) {
        if (!cond.empty()) cond += " & ";
        cond += "!missing(" + v + ")";
      }
      ojson blocks = dispatch("quietly generate byte " + name + " = (" + cond + ")" + qual, session);
      if (const ojson* e = firstError(blocks)) return fail(*e);
      touse = name;
      if (!qual.empty()) {
        blocks = dispatch("quietly replace " + touse + " = 0 if missing(" + touse + ")", session);
        if (const ojson* e = firstError(blocks)) return fail(*e);
      }
      blocks = dispatch("quietly count if " + touse + " == 1", session);
      if (const ojson* e = firstError(blocks)) return fail(*e);
      const double* n = session.eval.rValues.find("N");
      if (n != nullptr && *n == 0.0) return fail(errorWithRc("no observations", 2000));
    }

    for (const std::string& v : targets) {
      const std::string nv = pre + v;
      const std::string cond = casewise ? " if " + touse + " == 1" : qual;
      ojson blocks =
          dispatch("generate double " + nv + " = log(" + v + "+sqrt(" + v + "^2+1))" + cond, session);
      appendBlocks(out, blocks);
      if (const ojson* e = firstError(blocks)) return fail(*e);

      blocks = dispatch("label variable " + nv + " \"IHS trans. values of " + v + "\"", session);
      appendBlocks(out, blocks);
      if (const ojson* e = firstError(blocks)) return fail(*e);

      blocks = dispatch("quietly summarize " + nv, session);
      if (const ojson* e = firstError(blocks)) return fail(*e);
      const double* n = session.eval.rValues.find("N");
      if (n != nullptr && *n == 0.0) {
        // The ado's own `drop`; plain, so its data_mutation block reaches the
        // variables list (see dropMarker).
        blocks = dispatch("drop " + nv, session);
        appendBlocks(out, blocks);
        if (const ojson* e = firstError(blocks)) return fail(*e);
      }
    }
    dropMarker();
  } catch (const std::exception& e) {
    return fail(ojson(eval::engineError(std::string("ihstrans: ") + e.what())));
  }
  return out;
}

}  // namespace piet::handlers
