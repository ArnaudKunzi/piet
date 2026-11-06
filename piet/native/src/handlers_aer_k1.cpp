// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// AER scope 2, bundle K1 -- the four remaining small names in the twelve-paper
// corpus, all of which rename or reorder variables and none of which the
// registry could answer with an alias:
//
//   tolower   lowercase variable NAMES (Nicholas J. Cox, Statalist Digest 895)
//   move      put one variable in another's position (StataCorp, built in)
//   renvars   the rename transformer (Nicholas J. Cox, Jeroen Weesie, STB-60)
//   parallel  George Vega Yon's cluster runner, answered SEQUENTIALLY
//
// EVERY RULE BELOW WAS MEASURED BEFORE IT WAS WRITTEN, against StataNow 19.5 BE
// with the packages installed on this machine, and the captures live in the
// session log the commit names. That order matters here more than usual,
// because the plan for this bundle got two of the four wrong and the
// measurements are what caught it:
//
//   * The plan said `move v1 v2` is `order v1, before(v2)`. It is that in ONE
//     direction only. `move` puts v1 in v2's POSITION, so with `a b c`,
//     `move c a` gives `c a b` (before) but `move a c` gives `b c a` (after).
//     A before()-always reuse answers `b a c` for the second -- a silently
//     different column order, which is exactly the kind of wrong answer that
//     survives every gate because nothing crashes.
//   * The plan called `renvars` an alias for this build's `renvarlab`. The
//     corpus spells it `renvars v2-v19 \ id age ...` and with presub/postsub/
//     subst/postfix, and renvarlab implements none of those -- it lists them in
//     its own `unimplemented` set. An alias would have answered r(198) on the
//     real corpus lines.
//
// THE ONE COUNTER-INTUITIVE MEASUREMENT, and the reason this file does NOT
// share renvarlab's validate-everything-first shape:
//
//   `tolower` IS NOT ATOMIC. Its ado is a five-line `while` loop around
//   `rename`, so it renames until one fails and leaves the earlier renames in
//   place. Measured on `AA bb CC`: bare `tolower` answers rc 110 (`variable bb
//   already defined`, from `rename bb bb`) and the data is left as `aa bb CC`
//   -- the first variable renamed, the third never reached. An implementation
//   that validated the whole list up front would leave `AA bb CC` untouched and
//   answer the same rc, which reads as more careful and is a different answer.
//   `renvars` DOES validate up front (`confirm new var` runs before its rename
//   loop), so the two commands in this one file deliberately differ, and each
//   matches its own package.
//
// NOTHING HERE IMPLEMENTS RENAME OR ORDER A SECOND TIME. Every name change goes
// through handlers_data_verbs.cpp's `runRename` and every reposition through
// its `runOrder`, which is what makes the label plane, the sort plane and the
// resident card behave the same as they do for a hand-typed `rename`. Two of
// the measurements exist only to prove that: `renvars AAA, lower` and
// `tolower AAA` both keep the variable label ("Label for AAA") and the value
// label (`lbl`), because rename never touched either.
#include "piet/handlers.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "piet/datastore.h"
#include "piet/dispatcher.h"
#include "piet/eval/errors.h"
#include "piet/eval/session.h"
#include "piet/eval/tokenizer.h"
#include "piet/loops.h"  // expandVarlist -- the ONE varlist wildcard/range expander
#include "piet/mutation_result.h"
#include "piet/session_state.h"
#include "piet/strutil.h"

namespace piet::handlers {

using ojson = nlohmann::ordered_json;

namespace {

// ---------------------------------------------------------------------------
// OPTION VOCABULARIES -- namespace scope, before any dispatch.
//
// A function-local static constructs too late: after the first call that needed
// it. `known` is what the handlers below actually READ; `unimplemented` names
// real options of the same package that this build does not do. A name in
// NEITHER set reads as "unknown option", which is the wrong diagnosis for a
// real option, and a name wrongly in `known` is accepted and silently ignored.

// `tolower` and `move` take no options at all -- neither ado has a comma clause
// -- so neither registers a vocabulary. An option on either is an unknown
// option, which is what Stata answers too.

// renvars' full transformation set, from renvars.ado's own `syntax` line. All
// but two are implemented, and the two that are not are refused rather than
// approximated: `map()` builds a NEW NAME BY EVALUATING A STATA EXPRESSION
// (`local newname = <expr>` after substituting the symbol), which is a name
// derivation this build has no way to run, and `symbol()` only names the
// placeholder character map() substitutes -- accepting it alone would be
// accepting half of a feature. `display` and `test` are read: they are the
// package's own dry-run surface.
const piet::eval::OptVocab& kVocabRenvars = piet::eval::registerOptVocab(
    {"renvars"}, {{"upper", "lower", "prefix", "postfix", "suffix", "presub", "postsub", "subst",
                   "predrop", "postdrop", "trim", "trimend", "display", "test"},
                  {"map", "symbol"}});

// parallel's options, split by what this build can honour while running the
// work IN THIS PROCESS. `do()` and `expression()` are append's two arguments and
// both are read. `if()`/`in()` are append's row filters, applied to each file as
// it is read, and are read too. The rest name a cluster topology that does not
// exist here: `processors()`, `statapath()`, `programs()`, `seeds()`,
// `randtype()`, `force`, `nodata`, `keep`, `by()`. Refusing them is the honest
// answer -- silently accepting `seeds(random)` would promise a draw scheme this
// build does not implement.
const piet::eval::OptVocab& kVocabParallel = piet::eval::registerOptVocab(
    {"parallel"}, {{"do", "expression", "if", "in"},
                   {"processors", "statapath", "programs", "program", "seeds", "randtype",
                    "force", "nodata", "keep", "by", "sandbox"}});

ojson k1Error(const std::string& message, const std::string& code = "engine_error") {
  return ojson(eval::engineError(message, code));
}

ojson k1Syntax(const std::string& message) {
  return ojson(eval::engineError(message, "syntax_error"));
}

// An error whose Stata rc is KNOWN but which coarseRc's message rules cannot
// recognise. `reason` is the only channel carrying an exact rc (BUG-187) and it
// outranks the coarse mapping. Every rc below is a measured one.
ojson k1ErrorRc(const std::string& message, int rc) {
  return ojson(eval::errorBlock("engine_error", message, std::nullopt, std::nullopt, std::nullopt,
                                "r(" + std::to_string(rc) + ")"));
}

bool isErrorBlock(const ojson& b) {
  return b.is_object() && b.value("type", std::string()) == "error";
}

const ojson* firstError(const ojson& result) {
  if (isErrorBlock(result)) return &result;
  if (result.is_array()) {
    for (const auto& b : result) {
      if (isErrorBlock(b)) return &b;
    }
  }
  return nullptr;
}

ojson silent() { return ojson::array(); }

ojson displayBlock(const std::string& text) {
  ojson b;
  b["type"] = "display";
  b["text"] = text;
  return b;
}

std::string lowerOf(const std::string& s) {
  std::string out = s;
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

std::string upperOf(const std::string& s) {
  std::string out = s;
  for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return out;
}

bool hasFlag(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
             const std::string& key) {
  for (const auto& [k, v] : opts) {
    if (k == key) return !v.isBool || v.boolValue;
  }
  return false;
}

std::optional<std::string> optStr(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                  const std::string& key) {
  for (const auto& [k, v] : opts) {
    if (k == key && !v.isBool) return v.strValue;
  }
  return std::nullopt;
}

// Whitespace-split, quote-aware only to the extent renvars needs: its two-word
// option arguments arrive as `subst("_cen_" "")` and `postsub(10)`, so a quoted
// EMPTY second word has to survive as a real (empty) word. A plain split on
// spaces would drop it and turn a deletion into a no-op.
std::vector<std::string> splitWords(const std::string& s) {
  std::vector<std::string> out;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    if (i >= s.size()) break;
    if (s[i] == '"') {
      const size_t start = ++i;
      while (i < s.size() && s[i] != '"') ++i;
      out.push_back(s.substr(start, i - start));
      if (i < s.size()) ++i;  // past the closing quote
    } else {
      const size_t start = i;
      while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i]))) ++i;
      out.push_back(s.substr(start, i - start));
    }
  }
  return out;
}

// Every AER handler TU carries its own copy of these two: they are four-line
// primitives and threading them through a shared header would put a compile
// dependency on every one of them for no benefit.
std::string joinWith(const std::vector<std::string>& v, const std::string& sep) {
  std::string out;
  for (size_t i = 0; i < v.size(); ++i) {
    if (i != 0) out += sep;
    out += v[i];
  }
  return out;
}

// Whole-string integer parse: `predrop(2x)` must be REJECTED, not silently
// truncated to 2, which would drop a different number of characters with no
// diagnostic. This is `confirm integer number`'s job in the ado.
bool parseStrictInt(const std::string& s, long long* out) {
  if (s.empty()) return false;
  size_t i = 0;
  if (s[0] == '+' || s[0] == '-') i = 1;
  if (i >= s.size()) return false;
  for (size_t k = i; k < s.size(); ++k) {
    if (std::isdigit(static_cast<unsigned char>(s[k])) == 0) return false;
  }
  try {
    *out = std::stoll(s);
  } catch (const std::exception&) {
    return false;
  }
  return true;
}

// The columns, from the session, without forcing a Datastore open when the
// resident shadow card can answer. Both commands here only ever need NAMES.
bool columnNames(SessionState& session, std::vector<std::string>* out, ojson* err) {
  Datastore* dsp = nullptr;
  try {
    dsp = session.datastore();
  } catch (const std::exception& e) {
    *err = k1Error(std::string("K1: ") + e.what(), "data_io");
    return false;
  }
  if (dsp == nullptr) {
    *err = ojson(eval::noDataLoaded());
    return false;
  }
  *out = dsp->names();
  return true;
}

}  // namespace

// ===========================================================================
// tolower
// ===========================================================================
//
//     tolower [varlist]
//
// Nicholas J. Cox's, from Statalist Digest 895 -- the attribution is in the
// ado's own first line, which is why this name gets a registry row where
// `dropmiss` still does not.
//
// NOT ATOMIC, deliberately: see the file header. The ado is
//
//     while "`1'" != "" { local newname = lower("`1'") ; rename `1' `newname' ; mac shift }
//
// so it walks the list renaming, and the first `rename x x` (a name that was
// already lowercase) errors r(110) with everything before it already renamed.
// That is measured, not inferred: on `AA bb CC` a bare `tolower` leaves
// `aa bb CC` behind and answers rc 110.
ojson runTolower(const eval::ParsedCommand& parsed, SessionState& session) {
  // No comma clause in the ado at all, so any option is an unknown option.
  if (auto err = eval::rejectUnknownOptions(parsed.opts, {}, {})) return ojson(*err);
  if (!session.hasData()) return ojson(eval::noDataLoaded());

  std::vector<std::string> vars;
  if (parsed.varlist.empty()) {
    // `local varlist "opt ex"` with nothing typed is _all.
    ojson err;
    if (!columnNames(session, &vars, &err)) return err;
  } else {
    // expandVarlist is the ONE place the wildcard/hyphen varlist grammar lives.
    // The corpus spells the range with a space after the hyphen
    // (`tolower AL1- KONEV_L`), which that expander already handles.
    loops::VarlistExpandResult ex =
        loops::expandVarlist(joinWith(parsed.varlist, " "), session);
    if (ex.isError) return ex.error;
    vars = ex.tokens;
  }

  // The live name set, refreshed as renames land, because a rename earlier in
  // the list changes what the next one collides with. `from`/`to` accumulate
  // what actually landed, for the single mutation block at the end.
  std::vector<std::string> from, to;
  std::vector<std::string> live;
  {
    ojson err;
    if (!columnNames(session, &live, &err)) return err;
  }

  for (const auto& nm : vars) {
    const std::string out = lowerOf(nm);
    // Stata does NOT skip a name that is already lowercase: it issues the
    // no-op `rename abc abc` and gets r(110) back, which is why the collision
    // test here is simply "does that name exist" -- the self case and the
    // clash case are the same case, and both are measured at 110.
    //
    // The rc is answered HERE rather than left to runRename because runRename
    // reports its own collision as a plain engine_error, which coarseRc maps to
    // rc 1. That gap belongs to `rename` itself and is filed rather than
    // patched from this file (BUG-449); what this arm owes is tolower's own
    // measured rc, and it pays it.
    if (std::find(live.begin(), live.end(), out) != live.end())
      return k1ErrorRc("variable " + out + " already defined", 110);
    ojson r = runRename(nm, out, session);
    if (const ojson* e = firstError(r)) return *e;
    for (auto& l : live) {
      if (l == nm) l = out;
    }
    from.push_back(nm);
    to.push_back(out);
  }
  // ONE mutation block for the whole run, not none and not one per rename.
  //
  // Not NONE, which is what the landed sibling `renvarlab` does and what this
  // handler did until the freeze capture caught it: runRename's own
  // mutationResult was being swallowed, so `tolower` changed the variable plane
  // and published nothing -- and a freeze fixture for it pinned the PREAMBLE's
  // varlist instead of its own ([[gates-watch-production-not-absence]]: the
  // capture gates cannot see what a command declines to emit). Filed for
  // renvarlab as BUG-452.
  //
  // Not one PER rename either: the intermediate varlists are states the user
  // never asked about, and `tolower` over a wide dataset would publish a block
  // per column. mutationResult renders the FINAL varlist with the complete
  // new/dropped pair, which is the whole truth in one block.
  if (from.empty()) return silent();
  return mutationResult(session, "rename", to, from);
}

// ===========================================================================
// move
// ===========================================================================
//
//     move varname1 varname2
//
// Puts varname1 in varname2's POSITION. Direction matters and the plan's
// `order v1, before(v2)` is only half of it -- see the file header for the
// measured table. Implemented as the two order() forms rather than as a private
// permutation so the Frame-level column move, the resident card invalidation
// and the mutation result all stay in one place.
ojson runMove(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, {}, {})) return ojson(*err);
  if (!session.hasData()) return ojson(eval::noDataLoaded());

  // Measured rcs: one name is r(102) `too few variables specified`, three is
  // r(103) `too many variables specified`.
  if (parsed.varlist.size() < 2) return k1ErrorRc("too few variables specified", 102);
  if (parsed.varlist.size() > 2) return k1ErrorRc("too many variables specified", 103);

  const std::string& from = parsed.varlist[0];
  const std::string& to = parsed.varlist[1];

  std::vector<std::string> names;
  ojson err;
  if (!columnNames(session, &names, &err)) return err;

  const auto itFrom = std::find(names.begin(), names.end(), from);
  const auto itTo = std::find(names.begin(), names.end(), to);
  // r(111) `variable zz not found`, measured.
  if (itFrom == names.end()) return k1ErrorRc("variable " + from + " not found", 111);
  if (itTo == names.end()) return k1ErrorRc("variable " + to + " not found", 111);
  // `move a a` is rc 0 and changes nothing, measured.
  if (itFrom == itTo) return silent();

  // v1 is BEFORE v2 -> v1 lands where v2 is, so it ends up AFTER it once v2 has
  // shifted left. v1 is AFTER v2 -> v1 lands where v2 is and v2 shifts right.
  const bool fromIsEarlier = itFrom < itTo;
  std::vector<std::pair<std::string, eval::OptValue>> opts;
  opts.emplace_back(fromIsEarlier ? "after" : "before", eval::OptValue::ofString(to));
  return runOrder({from}, opts, session);
}

// ===========================================================================
// renvars
// ===========================================================================
//
//     renvars [varlist] \ newnamelist
//     renvars [varlist] , <exactly one transformation option> [display] [test]
//
// Nicholas J. Cox and Jeroen Weesie, STB-60 dm88 (the attribution is in
// renvars.ado's own header; it is on the Stata Journal as dm88_1, not on SSC,
// which is why an earlier pass concluded there was no package).
//
// THE TRAP THIS FILE EXISTS TO AVOID. `postsub()` and `presub()` substitute at
// the END and at the START only -- they are not `subst()` with a different
// name. The corpus contains `renvars share_sigvar*, postsub("_" "")`, and
// against `share_sigvar_a` that is measured to be a NO-OP at rc 0, because `_`
// is not the last character. Implementing postsub as "replace everywhere" would
// have renamed those variables to `sharesigvara` and answered rc 0 -- a silent
// wrong answer with no diagnostic anywhere.
//
// ATOMIC, unlike tolower above: renvars.ado runs `confirm new var newvars` over
// the WHOLE target list before its rename loop starts, so a collision anywhere
// leaves the data untouched.
struct RenvarsPlan {
  std::vector<std::pair<std::string, std::string>> pairs;  // old -> new
};

// Applies renvars' one transformation to one name. `ok` goes false only for the
// integer-argument options with a non-integer argument.
static std::string renvarsTransform(const std::string& name,
                             const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                             const std::string& which, bool* ok, std::string* why) {
  *ok = true;
  auto words = [&](const std::string& key) {
    return splitWords(optStr(opts, key).value_or(std::string()));
  };
  // ONE word, with its quotes off. The corpus writes both `postfix("10")` and
  // `prefix(bl_)`, and the tokenizer hands the option value through with the
  // quotes still on it -- `syntax ... POSTFix(str)` in the ado is what strips
  // them there. Taking word 1 off splitWords does the same job and, unlike a
  // strip-if-both-ends-are-quotes, keeps a legitimately spaced argument whole.
  auto word1 = [&](const std::string& key) {
    const std::vector<std::string> w = words(key);
    return w.empty() ? std::string() : w[0];
  };
  auto intArg = [&](const std::string& key, long long* out) {
    const std::string raw = word1(key);
    // `confirm integer number` in the ado: a non-integer is that command's own
    // error, not a silent truncation.
    if (raw.empty() || !parseStrictInt(raw, out)) {
      *ok = false;
      *why = "'" + raw + "' found where integer expected";
      return false;
    }
    return true;
  };

  if (which == "upper") return upperOf(name);
  if (which == "lower") return lowerOf(name);
  if (which == "prefix") return word1("prefix") + name;
  if (which == "postfix" || which == "suffix") return name + word1(which);
  if (which == "subst") {
    const std::vector<std::string> w = words("subst");
    if (w.empty()) return name;
    const std::string& srch = w[0];
    const std::string repl = w.size() > 1 ? w[1] : std::string();
    if (srch.empty()) return name;
    std::string out;
    size_t i = 0;
    while (i < name.size()) {
      if (name.compare(i, srch.size(), srch) == 0) {
        out += repl;
        i += srch.size();
      } else {
        out.push_back(name[i]);
        ++i;
      }
    }
    return out;
  }
  if (which == "presub") {
    const std::vector<std::string> w = words("presub");
    if (w.empty()) return name;
    const std::string& srch = w[0];
    const std::string repl = w.size() > 1 ? w[1] : std::string();
    if (srch.empty() || name.compare(0, srch.size(), srch) != 0) return name;
    return repl + name.substr(srch.size());
  }
  if (which == "postsub") {
    const std::vector<std::string> w = words("postsub");
    if (w.empty()) return name;
    const std::string& srch = w[0];
    const std::string repl = w.size() > 1 ? w[1] : std::string();
    if (srch.empty() || srch.size() > name.size()) return name;
    if (name.compare(name.size() - srch.size(), srch.size(), srch) != 0) return name;
    return name.substr(0, name.size() - srch.size()) + repl;
  }
  if (which == "predrop") {
    long long n = 0;
    if (!intArg("predrop", &n)) return name;
    if (n <= 0) return name;
    return static_cast<size_t>(n) >= name.size() ? std::string() : name.substr(static_cast<size_t>(n));
  }
  if (which == "postdrop") {
    long long n = 0;
    if (!intArg("postdrop", &n)) return name;
    if (n <= 0) return name;
    return static_cast<size_t>(n) >= name.size() ? std::string()
                                                 : name.substr(0, name.size() - static_cast<size_t>(n));
  }
  if (which == "trim") {
    long long n = 0;
    if (!intArg("trim", &n)) return name;
    if (n <= 0) return std::string();
    return static_cast<size_t>(n) >= name.size() ? name : name.substr(0, static_cast<size_t>(n));
  }
  if (which == "trimend") {
    long long n = 0;
    if (!intArg("trimend", &n)) return name;
    // renvars.ado guards this one: a trimend wider than the name leaves it be.
    if (n <= 0 || static_cast<size_t>(n) > name.size()) return name;
    return name.substr(name.size() - static_cast<size_t>(n));
  }
  return name;
}

ojson runRenvars(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabRenvars.known,
                                            kVocabRenvars.unimplemented))
    return ojson(*err);
  if (!session.hasData()) return ojson(eval::noDataLoaded());

  std::vector<std::string> allNames;
  ojson nerr;
  if (!columnNames(session, &allNames, &nerr)) return nerr;

  // ---- the two syntaxes -------------------------------------------------
  // The tokenizer hands the map form's `\` (or the undocumented `/`) through in
  // the varlist, because neither is an option and neither ends the command. So
  // the split is done here, on the varlist, exactly as the ado does it on `0`.
  std::vector<std::string> oldSpec, newSpec;
  bool isMap = false;
  for (const auto& tok : parsed.varlist) {
    if (!isMap && (tok == "\\" || tok == "/")) {
      isMap = true;
      continue;
    }
    (isMap ? newSpec : oldSpec).push_back(tok);
  }

  // An empty old list is `_all` in both forms (the ado pushes the punctuation
  // back and sets oldvars to _all).
  std::vector<std::string> oldVars;
  if (oldSpec.empty()) {
    oldVars = allNames;
  } else {
    loops::VarlistExpandResult ex =
        loops::expandVarlist(joinWith(oldSpec, " "), session);
    if (ex.isError) return ex.error;
    oldVars = ex.tokens;
  }

  RenvarsPlan plan;
  bool wantDisplay = hasFlag(parsed.opts, "display");
  const bool wantTest = hasFlag(parsed.opts, "test");

  if (isMap) {
    // One-to-one. The new names are typed literally and are NOT varlist-
    // expanded: they do not exist yet.
    if (oldVars.size() != newSpec.size())
      return k1Syntax("lists of old and new varnames unequal in length");
    for (size_t i = 0; i < oldVars.size(); ++i) {
      if (oldVars[i] != newSpec[i]) plan.pairs.emplace_back(oldVars[i], newSpec[i]);
    }
  } else {
    // Transformation. EXACTLY ONE, counted the way renvars.ado counts it --
    // `suffix` and `postfix` are the same transformation under two spellings,
    // and giving both is legal so long as they agree.
    static const char* kTransforms[] = {"upper",   "lower",    "prefix", "postfix", "suffix",
                                        "presub",  "postsub",  "subst",  "predrop", "postdrop",
                                        "trim",    "trimend"};
    std::vector<std::string> given;
    for (const char* t : kTransforms) {
      const bool present = (std::string(t) == "upper" || std::string(t) == "lower")
                               ? hasFlag(parsed.opts, t)
                               : optStr(parsed.opts, t).has_value();
      if (present) given.push_back(t);
    }
    const std::optional<std::string> sfx = optStr(parsed.opts, "suffix");
    const std::optional<std::string> pfx = optStr(parsed.opts, "postfix");
    if (sfx.has_value() && pfx.has_value()) {
      // Compared unquoted, so `postfix(10) suffix("10")` agrees the way the ado
      // sees it agree.
      const auto unq = [](const std::string& v) {
        const std::vector<std::string> w = splitWords(v);
        return w.empty() ? std::string() : w[0];
      };
      if (unq(*sfx) != unq(*pfx)) return k1Syntax("postfix() and suffix() differ");
      // They agree, so they count once.
      given.erase(std::remove(given.begin(), given.end(), std::string("suffix")), given.end());
    }
    if (given.size() != 1)
      return k1Syntax("exactly one transformation option should be specified");

    const std::string which = given[0];
    for (const auto& nm : oldVars) {
      bool ok = true;
      std::string why;
      const std::string out = renvarsTransform(nm, parsed.opts, which, &ok, &why);
      if (!ok) return k1ErrorRc(why, 7);
      if (out != nm) plan.pairs.emplace_back(nm, out);
    }
  }

  if (plan.pairs.empty()) {
    // The ado says this and exits 0.
    return displayBlock("no renames necessary");
  }

  // ---- `confirm new var` over the WHOLE target list, before any rename ----
  // This is what makes renvars atomic where tolower is not.
  std::set<std::string> renaming;
  for (const auto& [from, to] : plan.pairs) renaming.insert(from);
  std::set<std::string> taken;
  for (const auto& nm : allNames) {
    if (renaming.count(nm) == 0) taken.insert(nm);
  }
  std::set<std::string> targets;
  for (const auto& [from, to] : plan.pairs) {
    if (!strutil::isValidStataIdentifier(to))
      return k1Syntax("renvars: `" + from + "' would become `" + to +
                      "', which is not a legal variable name -- " +
                      strutil::stataIdentifierRuleText());
    if (taken.count(to) != 0) return k1ErrorRc("variable " + to + " already defined", 110);
    if (!targets.insert(to).second)
      return k1ErrorRc("variable " + to + " already defined", 110);
  }

  if (wantTest) {
    // `test` prints the plan and renames nothing. `display` is implied by it in
    // the ado's own output, which prints the same lines.
    ojson blocks = ojson::array();
    blocks.push_back(displayBlock("specification would result in the following renames:"));
    for (const auto& [from, to] : plan.pairs)
      blocks.push_back(displayBlock("  " + from + " -> " + to));
    return blocks;
  }

  ojson blocks = ojson::array();
  std::vector<std::string> fromNames, toNames;
  for (const auto& [from, to] : plan.pairs) {
    if (wantDisplay) blocks.push_back(displayBlock("  " + from + " -> " + to));
    ojson r = runRename(from, to, session);
    if (const ojson* e = firstError(r)) return *e;
    fromNames.push_back(from);
    toNames.push_back(to);
  }
  // See runTolower: one mutation block for the whole plan, so the variable
  // plane this command just rewrote is actually published.
  blocks.push_back(mutationResult(session, "rename", toNames, fromNames));
  return blocks;
}

}  // namespace piet::handlers
