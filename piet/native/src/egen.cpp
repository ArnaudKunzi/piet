// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// `egen NEWVAR = fn(...) [, by(g) missing p(#) ...]` — C++ port of
// piet/backend/piet/egen.py (option-suffix rebuilding) + mutate.py's
// run_egen (the cobra --egen / --rowegen dispatch). M3b (ROADMAP "M3b
// scope" item 4 — bysort'd egen via bysort.py's by()-injection is the
// M3a-RED-cell fix; this file is the underlying `egen` command itself,
// which the bysort injection path also calls through dispatch()).
//
// Row functions (rowmean/rowsd/rowtotal/rowmax/rowmin/rowmiss/rownonmiss)
// route to `cobra eval --rowegen NEWVAR=fn(a,b,c)` (comma-separated args,
// converted from the space-separated Stata varlist form). Every other
// function (the original 8 stat fns mean/sum/total/count/min/max/median/sd
// plus the cobra 0.13.0 extended set tag/group/seq/rank/pctile/std/iqr)
// routes to `cobra eval --egen NEWVAR=fn(expr)[,opts][, by(g1,g2)]`.
#include "piet/handlers.h"
#include "piet/eval/exprfns.h"  // B3(c): exprHasRunningSum
#include "piet/translate.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <limits>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_set>

#include "core/ident.h"  // isIdentByte: cut()'s argument
#include "data/missing.h"  // missing_kind: string() of a .a cell
#include "io/win_compat.h"  // to_native_path, native_exists, native_unlink
#include "piet/cobra_runner.h"
#include "piet/datastore.h"
#include "piet/dispatcher.h"  // cut's value label and the any* note are ordinary commands
#include "piet/display_hardening.h"  // exprUnboundNameRefusal: cut()'s =/exp
#include "piet/eval/errors.h"
#include "piet/inrange_filter.h"
#include "piet/loops.h"  // BUG-1001: expandVarlist, the one varlist wildcard/range expander
#include "piet/mutation_result.h"
#include "piet/strutil.h"
#include "piet/scratch_seq.h"
#include "piet/storage_type.h"  // BUG-471 T2: --store-type

namespace {
// BUG-233 / FIX:OPTNET-ABBREV-SWEEP — egen's option vocabulary. Union of the
// base set and every per-function conditional key (see runEgen); registered so
// dispatcher.cpp's single resolution point can canonicalize an abbreviation
// BEFORE runEgen's own `k == "by"` / `k == "missing"` comparisons run.
const piet::eval::OptVocab& kVocabEgen = piet::eval::registerOptVocab(
    // The UNION across egen functions (BUG-233's rule): the dispatcher
    // canonicalizes an abbreviation against this set, and the per-function
    // check inside runEgen is what actually refuses a wrong pairing.
    // BUG-1001: the per-function sets of egenFnVocab() below join the union. egen
    // is in kOptMinComplete (generated/opt_minimums_bug264.inc), so the dispatcher
    // resolves no prefix against this union; runEgen resolves every key against the
    // function's OWN declared minimums.
    // values/punct/format/at/group/icodes/label: the options of the functions
    // runEgenAdoFunction computes.
    {"egen"}, {{"by", "missing", "p", "from", "f", "to", "t", "block", "b", "minmode",
                "maxmode", "nummode", "field", "track", "unique", "prop", "mean", "sd",
                "std", "strok", "values", "punct", "format", "at", "group", "icodes", "label"},
               {"autotype", "suffix", "decode", "maxlength"}});
}  // namespace

namespace piet::handlers {

using ojson = nlohmann::ordered_json;

namespace {

// A5-04: forwards to the shared strutil copy (piet/strutil.h) — kept as a
// thin wrapper (not removed) so this file's many `trim(...)`/`lower(...)`
// call sites didn't need touching. Pure dedup, no behavior change.
std::string trim(const std::string& s) { return strutil::trim(s); }

ojson engineErrorBlock(const std::string& message, const std::string& code = "engine_error") {
  return eval::engineError(message, code);
}

// BUG-038(a): forwards to the shared mapping point (piet::cobraErrorMessage,
// cobra_runner.h) — kept as a thin wrapper (not removed) so this file's
// call sites didn't need touching. Pure dedup, no behavior change here.
std::string cobraErrorStr(const CobraError& ce) { return cobraErrorMessage(ce); }

// BUG-042 (egen if-wiring): `cobra merge`'s success path prints a plain-text
// status line to stderr (not JSON), so `runCobraJson`'s "parse stdout as
// JSON" contract doesn't fit — this needs the raw `runCobra()` + a manual
// CobraError build from stderr, same as handlers_data_verbs.cpp's own
// `errorFromRaw` (file-local copy per this codebase's established
// one-copy-per-TU convention for exactly this helper).
CobraError errorFromRaw(const CobraRunResult& r) {
  std::string stderrText = r.stderr_text;
  while (!stderrText.empty() &&
         (stderrText.back() == '\n' || stderrText.back() == '\r' || stderrText.back() == ' ')) {
    stderrText.pop_back();
  }
  try {
    nlohmann::json payload = nlohmann::json::parse(stderrText);
    nlohmann::json err = payload.contains("error") ? payload.at("error") : payload;
    if (err.is_object() && err.contains("message")) {
      return CobraError(err.value("code", std::string("engine_error")),
                        err.at("message").get<std::string>(), err.value("detail", std::string()));
    }
  } catch (const nlohmann::json::exception&) {
  }
  std::string firstLine = stderrText;
  auto nl = firstLine.find('\n');
  if (nl != std::string::npos) firstLine = firstLine.substr(0, nl);
  if (firstLine.empty()) firstLine = "cobra exited " + std::to_string(r.exit_code);
  return CobraError("engine_error", firstLine, stderrText);
}

std::string nextPath(const std::string& workdir, const std::string& prefix) {
  return mintScratchPath(workdir, prefix);  // BUG-389: one body, registered at mint
}

// egen.py's _ROW_FUNS (mutate.py:187) — functions that route to --rowegen
// instead of --eigen. The Stata 8 names (rsum, rmax, ...) arrive renamed
// (rowAliasOf).
const std::set<std::string>& rowFuns() {
  static const std::set<std::string> s = {"rowmean", "rowsd",   "rowtotal",  "rowmax",
                                          "rowmin",  "rowmiss", "rownonmiss"};
  return s;
}

// The Stata 8 row-function names are aliases: each behaves as its Stata 9
// function on the same arguments, so the values, options and refusals are that
// function's, reported under the new name (`egen ... rowtotal() may not be
// combined with by` for rsum; BUG-1001, BUG-1112, StataBE 19.5). "" for any other
// name.
std::string rowAliasOf(const std::string& fn) {
  static const std::map<std::string, std::string> t = {
      {"rsum", "rowtotal"}, {"rmax", "rowmax"},   {"rmin", "rowmin"},    {"rmean", "rowmean"},
      {"rmiss", "rowmiss"}, {"robs", "rownonmiss"}, {"rsd", "rowsd"}};
  auto it = t.find(fn);
  return it == t.end() ? std::string() : it->second;
}

// BUG-1001: each egen function's OWN option vocabulary: the option names (help
// egen) and the shortest abbreviation StataBE 19.5 accepts for each, measured
// (StataBE oracle arms E_*): `mode(v), min`
// runs and `mi` is r(198), so minmode is {"minmode", 3}; `total(v), m` runs, so
// total's missing is {"missing", 1}, while `mode(v), mis` is r(198) and `miss` runs,
// so mode's is {"missing", 4}. Where the help file's underlining differs from what
// Stata accepts (num:mode, lab:el; `n()` and `l` run), the measurement wins.
// `valued` options take a (...) argument. `by()` is not listed: every function
// takes it (the row functions refuse it, r(190)). `unimplemented` holds options
// Stata accepts and this build does not compute; they are refused by name, never
// accepted and dropped. A function absent from this table keeps the pre-BUG-1001
// net in runEgen.
struct EgenOptSpec {
  const char* full;
  size_t minLen;
  bool valued;
};
struct EgenFnVocab {
  std::vector<EgenOptSpec> known;
  std::vector<EgenOptSpec> unimplemented;
};
const std::map<std::string, EgenFnVocab>& egenFnVocab() {
  static const std::map<std::string, EgenFnVocab> t = {
      {"count", {{}, {}}},
      {"sum", {{{"missing", 1, false}}, {}}},                        // sum is total
      {"total", {{{"missing", 1, false}}, {}}},
      {"min", {{}, {{"missing", 4, false}}}},
      {"max", {{}, {{"missing", 4, false}}}},
      {"median", {{}, {{"autotype", 8, false}}}},
      {"iqr", {{}, {{"autotype", 8, false}}}},
      {"pctile", {{{"p", 1, true}}, {{"autotype", 8, false}}}},
      {"sd", {{}, {}}},
      {"std", {{{"mean", 1, true}, {"sd", 1, true}, {"std", 2, true}}, {}}},
      {"rank",
       {{{"field", 1, false}, {"track", 1, false}, {"unique", 1, false}},
        {{"suffix", 1, true}}}},
      {"pc", {{{"prop", 4, false}}, {}}},
      {"seq", {{{"block", 1, true}, {"from", 1, true}, {"to", 1, true}}, {}}},
      {"mode",
       {{{"missing", 4, false}, {"minmode", 3, false}, {"maxmode", 3, false},
         {"nummode", 1, true}},
        {}}},
      {"rowmax", {{}, {}}},        {"rowmin", {{}, {}}},
      {"rowmiss", {{}, {}}},       {"rowsd", {{}, {}}},
      {"rowmean", {{}, {}}},
      {"rownonmiss", {{{"strok", 1, false}}, {}}},
      {"rowtotal", {{{"missing", 1, false}}, {}}},
      // cut: at(numlist, ascending) | group(#), icodes, label (help egen; `a()` and
      // `i` are r(198), `ic`, `g()` and `l` run). runEgenAdoFunction computes it.
      {"cut",
       {{{"at", 2, true}, {"group", 1, true}, {"icodes", 2, false}, {"label", 1, false}}, {}}},
      // concat: punct(), decode, maxlength(), format() (help egen; `max()` is
      // r(198), `maxl()` runs); punct() and format() run in runEgenAdoFunction.
      {"concat",
       {{{"punct", 1, true}, {"format", 1, true}},
        {{"decode", 1, false}, {"maxlength", 4, true}}}},
      // values(): anyvalue keeps its text, anycount/anymatch read an integer numlist
      // (help egen); all three run in runEgenAdoFunction.
      {"anyvalue", {{{"values", 1, true}}, {}}},
      {"anycount", {{{"values", 1, true}}, {}}},
      {"anymatch", {{{"values", 1, true}}, {}}},
  };
  return t;
}

// The spec `key` names under Stata's rule: the full name, or a prefix of it at
// least minLen long. nullptr when none does.
const EgenOptSpec* matchEgenOpt(const std::string& key, const std::vector<EgenOptSpec>& specs) {
  for (const auto& sp : specs) {
    const std::string full(sp.full);
    if (key.size() >= sp.minLen && key.size() <= full.size() &&
        full.compare(0, key.size(), key) == 0)
      return &sp;
  }
  return nullptr;
}

// Stata's egen functions that this build does not compute (they ship with Stata):
// named as unimplemented rather than "unknown", which would be false. Each row
// carries the options Stata accepts besides by(), which are read before the
// computation would start (one outside them is r(198) "option ... not allowed",
// StataBE 19.5 `egen q = mdev(k), bar`), and, for a function that refuses a
// by-list, the name that refusal reports (an old name reports its row*/any*
// successor: `egen q = robs(x k), by(k)` is "egen ... rownonmiss() may not be
// combined with by"; measured, StataBE oracle arms E_*_by).
struct UnimplementedEgenFn {
  std::vector<EgenOptSpec> opts;
  const char* noByName;  // nullptr: the function takes by()
};
const std::map<std::string, UnimplementedEgenFn>& unimplementedEgenFns() {
  static const std::map<std::string, UnimplementedEgenFn> t = {
      {"any", {{{"values", 1, true}}, "anyvalue"}},     // old name of anyvalue
      {"diff", {{}, "diff"}},
      {"ends",
       {{{"punct", 1, true}, {"head", 1, false}, {"last", 1, false}, {"tail", 1, false},
         {"trim", 2, false}},
        "ends"}},
      {"eqany", {{{"values", 1, true}}, "anymatch"}},   // old name of anymatch
      {"fill", {{}, "fill"}},
      {"kurt", {{}, nullptr}},
      {"ma", {{{"t", 1, true}, {"nomiss", 3, false}}, "ma"}},
      {"mad", {{}, nullptr}},
      {"mdev", {{}, nullptr}},
      {"mtr", {{}, "mtr"}},
      {"neqany", {{{"values", 1, true}}, "anycount"}},  // old name of anycount
      {"rfirst", {{}, "rowfirst"}},                     // old name of rowfirst
      {"rlast", {{}, "rowlast"}},                       // old name of rowlast
      {"rowfirst", {{}, "rowfirst"}},
      {"rowlast", {{}, "rowlast"}},
      {"rowmedian", {{}, "rowmedian"}},
      {"rowpctile", {{{"p", 1, true}}, "rowpctile"}},
      {"skew", {{}, nullptr}},
  };
  return t;
}

bool stataEgenFunctionUnimplemented(const std::string& fn) {
  return unimplementedEgenFns().count(fn) != 0;
}

// What a function's ado `syntax` binds first, before any option: a varlist or an
// =/exp. _gcount's `anything`, seq()'s empty argument, fill()'s numlist and mtr()'s
// own parser bind nothing there.
enum class EgenArgKind { kNone, kVarlist, kExp };
EgenArgKind egenArgKind(const std::string& fn) {
  static const std::set<std::string> varlist = {
      "group", "tag",   "concat", "anyvalue", "anycount", "anymatch", "mode",   "any",
      "diff",  "ends",  "eqany",  "neqany",   "rfirst",   "rlast",    "rmax",   "rmean",
      "rmin",  "rmiss", "robs",   "rowfirst", "rowlast",  "rowmedian", "rowpctile", "rsd"};
  static const std::set<std::string> exp = {"mean",   "total", "sum",  "sd",   "max",  "min",
                                            "median", "pctile", "iqr", "rank", "std",  "pc",
                                            "cut",    "mdev",  "kurt", "skew", "mad",  "ma"};
  if (rowFuns().count(fn) != 0 || varlist.count(fn) != 0) return EgenArgKind::kVarlist;
  if (exp.count(fn) != 0) return EgenArgKind::kExp;
  return EgenArgKind::kNone;
}

// An error block carrying Stata's exact return code (coarseRc reads `reason`).
ojson egenRcError(const std::string& message, int rc, const std::string& code = "engine_error") {
  return ojson(eval::errorBlock(code, message, std::nullopt, std::nullopt, std::nullopt,
                                "r(" + std::to_string(rc) + ")"));
}

// egen's ado: a function with no _g<fn>.ado is r(133) (eg_ref_unknown); one
// whose ado ships but this build does not compute is named as unimplemented.
ojson unknownEgenFunctionError(const std::string& fn) {
  if (stataEgenFunctionUnimplemented(fn))
    return ojson(eval::errorBlock(
        "unsupported_command",
        "egen function " + fn + "() is a valid Stata function that this version "
        "of PIET does not yet implement.", fn));
  return egenRcError("unknown egen function " + fn + "()", 133);
}

// Option `full` of the function's ado, which this build does not compute: refused
// by name, never accepted and dropped.
ojson egenUnimplementedOption(const char* full) {
  return ojson(eval::engineError("Option `" + std::string(full) + "` is a valid Stata option that "
                                 "this version of PIET does not yet implement.",
                                 "unsupported_option"));
}

// One function's option net (egenFnVocab): every key but by() is resolved under
// the ado's own minimums, and `canonical` (when given) receives it under its full
// name. An option the ado does not declare, or a valued one written bare (or the
// reverse), is Stata's r(198); one it declares and this build does not compute is
// held in `unimplemented` for the caller to refuse after the by-list.
std::optional<ojson> egenOptionNet(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                   const EgenFnVocab& vocab,
                                   std::vector<std::pair<std::string, eval::OptValue>>* canonical,
                                   std::optional<ojson>* unimplemented,
                                   bool missingInFull = false) {
  for (const auto& [k, v] : opts) {
    if (k == "by") continue;
    const EgenOptSpec* sp = matchEgenOpt(k, vocab.known);
    // `missingInFull`: gegen's own total()/sum() (BUG-1111, at the call).
    if (missingInFull && sp != nullptr && std::string(sp->full) == "missing" && k != "missing")
      sp = nullptr;
    if (sp == nullptr) {
      if (const EgenOptSpec* un = matchEgenOpt(k, vocab.unimplemented)) {
        if (!unimplemented->has_value()) *unimplemented = egenUnimplementedOption(un->full);
        continue;
      }
      return ojson(eval::engineError("option `" + k + "` not allowed", "unsupported_option"));
    }
    if (sp->valued == v.isBool)
      return ojson(eval::engineError("option `" + k + "` incorrectly specified",
                                     "unsupported_option"));
    if (canonical != nullptr) canonical->emplace_back(sp->full, v);
  }
  return std::nullopt;
}

// egen.ado looks the function up (r(133) when no ado ships) and hands the argument
// to the function's ado, whose `syntax` binds it before any option, a by-list or
// the computation: a varlist's names ("variable V not found" r(111); no varlist
// there takes an operator, r(101)) or an =/exp's ("V not found" r(111)). StataBE
// 19.5: `egen q = group(x nosuch), by(k) bogus`, `egen q = mean(nosuch), bogus`
// -> r(111); `egen q = rowmean(L.x x)`, `egen q = group(i.k)` -> r(101). An =/exp
// with a time-series operator or a value-label literal is left to the evaluator.
std::optional<ojson> egenArgumentRefusal(const std::string& fn, const std::string& args,
                                         SessionState& session) {
  if (args.empty()) return std::nullopt;
  const EgenArgKind kind = egenArgKind(fn);
  if (kind == EgenArgKind::kVarlist) {
    // `egen q = rowmean(x, t)` -> "invalid syntax" r(198).
    if (args.find(',') != std::string::npos) return egenRcError("invalid syntax", 198);
    std::istringstream in(args);
    std::string tok;
    while (in >> tok)
      if (tok.find('.') != std::string::npos)
        return egenRcError("factor-variable and time-series operators not allowed", 101);
    loops::VarlistExpandResult vx = loops::expandVarlist(args, session);
    if (vx.isError) return vx.error;
    return loops::checkKnownVarlist(vx.tokens, session);
  }
  if (kind == EgenArgKind::kExp && !translate::exprHasTsToken(args) &&
      args.find("\":") == std::string::npos) {
    if (auto unbound = exprUnboundNameRefusal(args, session))  // "r(111): ..."
      return egenRcError(unbound->substr(unbound->find(": ") + 2), 111);
  }
  return std::nullopt;
}

// egen's ado `confirm new variable`: a name already in the dataset is r(110).
bool egenTargetExists(SessionState& session, const std::string& name) {
  if (name.empty() || !session.hasData()) return false;
  if (const std::vector<VarInfo>* card = session.residentReadCard()) {
    for (const auto& vi : *card)
      if (vi.name == name) return true;
    return false;
  }
  Datastore* ds = session.datastore();
  return ds != nullptr && ds->has(name);
}

// A5-04: forwards to the shared strutil copy (piet/strutil.h) — see the
// trim() wrapper above for why this stays a thin pass-through.
std::string lower(std::string s) { return strutil::lower(std::move(s)); }

// egen.py's build_opts_suffix: opts dict (excluding `by`) -> cobra-syntax
// suffix, e.g. {"p":"90"} -> ",p(90)"; {"missing":true} -> ",missing".
// Preserves ParsedCommand::opts insertion order (parity with Python dict
// iteration order — both reflect tokenizer left-to-right option order).
std::string buildOptsSuffix(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                            const std::set<std::string>& exclude) {
  std::string suffix;
  bool any = false;
  for (const auto& [key, val] : opts) {
    if (exclude.count(key)) continue;
    if (val.isBool) {
      if (!val.boolValue) continue;  // Python: `val is True` only
      suffix += (any ? "," : ",") + key;
      any = true;
    } else if (!val.strValue.empty()) {
      suffix += "," + key + "(" + val.strValue + ")";
      any = true;
    }
  }
  return suffix;
}

// bysort.py's _make_bygen_expr: "newvar=rhs_expr, by(v1,v2), sort(s1,s2)".
// by_vars=[] (global) omits both by()/sort(); sort_vars defaults to by_vars.
std::string makeBygenExpr(const std::string& expr, const std::vector<std::string>& byVars,
                          const std::optional<std::vector<std::string>>& sortVars) {
  std::string s = expr;
  if (!byVars.empty()) {
    std::string joined;
    for (size_t i = 0; i < byVars.size(); ++i) {
      if (i) joined += ",";
      joined += byVars[i];
    }
    s += ", by(" + joined + ")";
    const std::vector<std::string>& sv = sortVars.has_value() ? *sortVars : byVars;
    if (!sv.empty()) {
      std::string sjoined;
      for (size_t i = 0; i < sv.size(); ++i) {
        if (i) sjoined += ",";
        sjoined += sv[i];
      }
      s += ", sort(" + sjoined + ")";
    }
  }
  return s;
}

// Does `e` call function `fn`? exprHasRunningSum's scan (exprfns.h) for any
// name: quote-aware and word-bounded, so `rowgroup(` or a "group(" literal do
// not match.
bool exprCallsFn(const std::string& e, const std::string& fn) {
  char quote = 0;
  for (size_t i = 0; i < e.size(); ++i) {
    const char c = e[i];
    if (quote) {
      if (c == quote) quote = 0;
      continue;
    }
    if (c == '"' || c == '\'') { quote = c; continue; }
    if (e.compare(i, fn.size(), fn) != 0) continue;
    if (i > 0) {
      const char p = e[i - 1];
      if (cecon::ident::isIdentByte(p) || p == '.') continue;  // core/ident.h
    }
    size_t j = i + fn.size();
    while (j < e.size() && (e[j] == ' ' || e[j] == '\t')) ++j;
    if (j < e.size() && e[j] == '(') return true;
  }
  return false;
}

// BUG-841/842: the replace store-type decision (BUG-471 T2) when the folded
// probe of storage::decideReplaceStoreType would not read the values the store
// writes: a running sum under a qualifier (the fold accumulates the excluded
// rows) or group() under `in` (the fold numbers over the dataset). Measured:
// `generate byte b = 0` / `replace b = sum(x) if _n > 1` with x = -100, 150
// read 50 from the fold and kept byte, so the store wrote `.` for 150. This
// probe is the store's own --bygen with the target swapped for the scratch name
// and the same qualifier flags (no --gen-replace: untouched rows stay missing),
// read-only, so it summarizes exactly the stored values. The second --bygen
// marks the fractional ones under the same groups and qualifiers.
struct MaskedTypeDecision {
  std::string storeType;
  std::optional<ojson> error;
};
MaskedTypeDecision maskedReplaceStoreType(SessionState& session, const std::string& resolved,
                                          bool noPromote, const std::string& rhs,
                                          const std::vector<std::string>& byVars,
                                          const std::optional<std::vector<std::string>>& bygenSort,
                                          const std::vector<std::string>& qualifierArgs,
                                          const std::string& tsExpr) {
  MaskedTypeDecision d;
  d.storeType = resolved;
  if (!storage::isIntegerType(resolved) || noPromote) return d;  // as decideReplaceStoreType
  Datastore* ds = session.datastore();
  if (ds == nullptr) {
    d.error = ojson(eval::noDataLoaded());
    return d;
  }
  const std::string pv = storage::kReplaceProbeVar;
  const std::string pf = pv + "frac";
  const std::string probeArg = makeBygenExpr(pv + "=" + rhs, byVars, bygenSort);
  std::vector<std::string> args = {
      "eval", "--data", ds->path(), "--bygen", probeArg, "--bygen",
      makeBygenExpr(pf + "=(" + pv + "!=int(" + pv + "))", byVars, bygenSort)};
  args.insert(args.end(), qualifierArgs.begin(), qualifierArgs.end());
  args.push_back("--summarize");
  args.push_back(pv + "," + pf);
  args = translate::injectTsContextForExpr(args, tsExpr, session);
  // The store draws from the stream this probe would consume: hand the probe
  // that stream and give it back, as decideReplaceStoreType does.
  const long long savedStream = session.rngStream;
  args = translate::injectRngContextForExpr(args, probeArg + " " + tsExpr, session);
  session.rngStream = savedStream;
  ojson out;
  {
    SessionState::ResidentSuspend suspend(session);
    SessionState::StatelessRead statelessRead(session);
    try {
      out = runCobraJson(args);
    } catch (const CobraError& ce) {
      d.error = engineErrorBlock(cobraErrorStr(ce), ce.code);
      return d;
    }
  }
  const ojson summ = out.value("summarize", ojson::object());
  const ojson sv = summ.value(pv, ojson::object());
  const ojson sf = summ.value(pf, ojson::object());
  const auto num = [](const ojson& o, const char* k) {
    return o.contains(k) && o[k].is_number() ? o[k].get<double>() : 0.0;
  };
  if (!(num(sv, "N") > 0)) return d;  // nothing non-missing is stored: no widening
  d.storeType = storage::promotedType(resolved, num(sf, "max") > 0.0, num(sv, "min"),
                                      num(sv, "max"));
  return d;
}

}  // namespace

// BUG-532: Stata's `bysort KEYS (SORT): generate|replace` SORTS THE DATA by
// KEYS SORT and leaves them sorted (marker KEYS SORT) before it computes, so
// every later row-order read (`_n`, `in`, `list`, `x[_n-1]`, `save`, a plain
// `by`) sees Stata's order (w2_bykeep.log by_p_after_bysgen: a plain `by g
// (b):` runs after `bysort g (b): generate`). The rows are sorted in place, in
// Stata's order (a string key by its bytes; the engine moves no row when the
// marker already begins with KEYS SORT, and the marker becomes KEYS SORT
// either way).
std::optional<ojson> sortForBysort(SessionState& session, const std::vector<std::string>& byVars,
                                   const std::optional<std::vector<std::string>>& sortVars) {
  std::vector<std::string> keys = byVars;
  if (sortVars.has_value()) keys.insert(keys.end(), sortVars->begin(), sortVars->end());
  // bysort skips a redundant sort when its keys already lead the marker;
  // unlike an explicit `sort g`, it then keeps a more-specific `g b` claim
  // (w2c_functions sortedby_bysort_then_by_gb).
  if (Datastore* ds = session.datastore()) {
    std::istringstream marker(ds->sortedBy());
    bool alreadySorted = true;
    for (const auto& key : keys) {
      std::string marked;
      if (!(marker >> marked) || marked != key) { alreadySorted = false; break; }
    }
    if (alreadySorted) return std::nullopt;
  }
  return sortInStataOrder(session, keys);
}

// bysort.py's run_bysort_gen — `[bysort VARLIST:] gen/replace newvar = expr`
// via cobra eval --bygen (which natively supports _n/_N in group context;
// used unconditionally here even for exprs WITHOUT _n/_N, matching the
// oracle's own "keep separate for clarity" design note). `inRange` (BUG-077)
// is only ever non-nullopt from the DIRECT gen/replace dispatch path — the
// bysort/by-prefix call site (dispatcher.cpp) never passes it, since a
// by-group `_n` can't express Stata's GLOBAL `in` range (intentionally left
// rejected by the tokenizer's inRangeAllowed gate for bysort/by).
ojson runBysortGen(SessionState& session, const std::string& expr,
                   const std::vector<std::string>& byVars, const std::optional<std::string>& ifExpr,
                   bool sortFirst, bool isReplace, const std::optional<std::string>& inRange,
                   const std::optional<std::vector<std::string>>& sortVars,
                   const std::string& storeType, bool noPromote) {
  if (!session.currentHbin().has_value()) {
    return eval::noDataLoaded();
  }
  auto eqPos = expr.find('=');
  if (eqPos == std::string::npos) {
    return engineErrorBlock("bysort gen: expression must be `newvar = expr`");
  }
  std::string newvar = trim(expr.substr(0, eqPos));
  std::string rhs = trim(expr.substr(eqPos + 1));
  // replace writes an existing variable only. This route never names the
  // target in the expression when the `if` rides --gen-mask, so the engine
  // cannot refuse it there: `replace y = runiform() if _n > 1` and
  // `bysort g: replace y = 1` on an absent y are r(111) in StataBE 19.5.
  if (isReplace) {
    Datastore* ds = session.datastore();
    if (ds != nullptr && !ds->has(newvar)) {
      return ojson(eval::errorBlock("syntax_error", "variable " + newvar + " not found",
                                    std::nullopt, std::nullopt, std::nullopt, "r(111)"));
    }
  }

  // BUG-077 / BUG-842: `in a/b` rides the engine's own range channel (eval
  // --gen-in), never the expression: group() numbers within an `in` range
  // (w2m_bymask G05 :514) but not within an `if` that names the same rows
  // (G02 :482), so the two must reach the engine apart. Rows outside the range
  // are missing for gen and keep their value for replace (--gen-replace below).
  bool haveRange = inRange.has_value() && !inRange->empty();
  std::string rangeArg;
  std::string rangePred;  // the replace type probe's folded form (see below)
  if (haveRange) {
    // BUG-672/673: the shared [U] 11.1.4 resolver (inrange_filter.cpp), not a
    // private grammar. The range becomes literal global row numbers (this path
    // never runs with a by-list; see above). `_N` is the frame's row count as
    // the macro layer sees it — card first, so a dirty virtual head is not
    // snapshotted.
    InRangeBounds rr = parseInRangeBounds(*inRange, session.macroObsCount());
    if (!rr.ok) {
      InRangeFilterResult fr;
      fr.ok = false;
      fr.refusal = rr.refusal;
      return inRangeRejectError(fr, *inRange, "gen/replace");
    }
    rangeArg = std::to_string(rr.a) + "/" + std::to_string(rr.b);
    rangePred = "_n>=" + std::to_string(rr.a) + " & _n<=" + std::to_string(rr.b);
  }

  bool haveIf = ifExpr.has_value() && !ifExpr->empty();
  bool haveCombined = haveIf || haveRange;

  // AER scope 2 / B3(c). Folding `if` into cond() is exactly equivalent for every
  // function whose answer depends only on its own row -- but sum() is a RUNNING
  // total, and a folded qualifier makes it accumulate across the rows the
  // qualifier excluded (measured: Stata gives . 2 5 9 . 6 13 21 for paper 015's
  // shape, folding gives . 3 6 10 . 11 18 26). When the right-hand side carries a
  // running sum the condition rides separately as --gen-mask and the engine
  // applies it to the accumulation itself -- under replace too (BUG-841:
  // `replace c09 = sum(x) if _n > 1` summed the unselected row into every later
  // total, -1 60 120 130 160 210 at rc 0, oracle w2m_bymask C09 :361
  // -1 40 100 110 140 190). --gen-replace makes the rows a separate qualifier
  // excludes keep their value, as replace does.
  const bool runningSum = piet::eval::exprHasRunningSum(rhs);
  // A draw rides the mask for the same reason: the engine evaluates both branches
  // of cond(), as Stata does (BUG-1050), so a folded `if` would draw on the rows
  // it excludes, which Stata's qualifier never evaluates.
  const bool rhsDraws = translate::exprHasDrawToken(rhs);
  const bool maskOut = haveIf && (runningSum || rhsDraws);
  const bool keepUnselected = isReplace && (maskOut || haveRange);
  std::string bygenRhs = rhs;
  if (haveIf && !maskOut) {
    bygenRhs = "cond((" + *ifExpr + "),(" + rhs + ")," + (isReplace ? newvar : ".") + ")";
  }
  std::string bygenFullExpr = newvar + "=" + bygenRhs;
  // BS-002: an explicit sortVars (the `(sortvar)` parenthetical) always
  // wins over the sortFirst-driven default — it is a strictly more specific
  // instruction from the same `bysort`/`by` prefix line.
  std::optional<std::vector<std::string>> bygenSort =
      sortVars.has_value() ? sortVars : (sortFirst ? std::optional(byVars) : std::nullopt);
  // BUG-532: --bygen alone only computes through a sort order and leaves the
  // rows where they were (see sortForBysort); after the in-place sort it groups
  // on by() alone, whose stable sort keeps each group's rows in that physical
  // order — also for a string sort key, whose engine code order is not
  // Stata's. A plain `by` (sortFirst false) never sorts.
  if (sortFirst && !byVars.empty()) {
    if (std::optional<ojson> sortErr = sortForBysort(session, byVars, sortVars)) return *sortErr;
    bygenSort = std::vector<std::string>{};
  }
  std::string bygenArg = makeBygenExpr(bygenFullExpr, byVars, bygenSort);

  // BUG-471 T2: a replace widens an integer type by Stata's lattice before the
  // one store (see runReplace, handlers.cpp). The probe is the same --bygen with
  // the target swapped for the scratch name and the untouched rows missing, run
  // after the in-place sort above so it groups exactly as the store will.
  std::string finalStoreType = storeType;
  // The qualifiers as the store sends them (--gen-replace aside).
  std::vector<std::string> qualifierArgs;
  if (maskOut) qualifierArgs.insert(qualifierArgs.end(), {"--gen-mask", *ifExpr});
  if (haveRange) qualifierArgs.insert(qualifierArgs.end(), {"--gen-in", rangeArg});
  const bool foldedProbeExact =
      !((runningSum || rhsDraws) && haveCombined) &&
      !(haveRange && (exprCallsFn(rhs, "group") || (haveIf && exprCallsFn(*ifExpr, "group"))));
  if (isReplace && !foldedProbeExact) {
    MaskedTypeDecision dec =
        maskedReplaceStoreType(session, storeType, noPromote, rhs, byVars, bygenSort,
                               qualifierArgs, rhs + " " + ifExpr.value_or(std::string()));
    if (dec.error.has_value()) return *dec.error;
    finalStoreType = dec.storeType;
  } else if (isReplace) {
    // decideReplaceStoreType's probe is one --bygen value, so it reads the
    // qualifiers folded (the `if` half `!=0`-normalised, BUG-622); for every
    // shape that reaches here the fold computes the stored values exactly.
    std::string combinedIf;
    if (haveIf && haveRange) {
      combinedIf = "((" + *ifExpr + ")!=0) & (" + rangePred + ")";
    } else if (haveIf) {
      combinedIf = *ifExpr;
    } else if (haveRange) {
      combinedIf = rangePred;
    }
    const std::string probeRhs =
        haveCombined ? ("cond((" + combinedIf + "),(" + rhs + "),.)") : rhs;
    const std::string probeArg = makeBygenExpr(
        std::string(storage::kReplaceProbeVar) + "=" + probeRhs, byVars, bygenSort);
    std::string keyNames;
    for (const auto& b : byVars) keyNames += b + " ";
    if (bygenSort.has_value())
      for (const auto& b : *bygenSort) keyNames += b + " ";
    storage::ReplaceTypeDecision dec = storage::decideReplaceStoreType(
        session, storeType, noPromote, newvar, rhs, haveCombined, "--bygen", probeArg,
        {rhs, combinedIf, keyNames}, /*drawContext=*/true);
    if (dec.error.has_value()) return *dec.error;
    finalStoreType = dec.storeType;
  }

  std::string outPath = nextPath(session.workdir(), "bysort_gen");
  // BUG-369 follow-up: `bysort g: gen lb = L.y` — the operator resolves over
  // the FULL tsset series (E1) and the by-prefix only scopes _n/_N, so the
  // whole-frame bygen evaluation is exactly Stata's semantics; it just needs
  // the tsset metadata to ride the call.
  std::vector<std::string> baseArgs = {"eval", "--data", *session.currentHbin(),
                                      "--bygen", bygenArg, "--out", outPath};
  baseArgs.insert(baseArgs.end(), qualifierArgs.begin(), qualifierArgs.end());
  if (keepUnselected) baseArgs.push_back("--gen-replace");
  storage::appendStoreTypeArg(baseArgs, finalStoreType);  // BUG-471 T2
  // The `if` sent as --gen-mask is not inside bygenArg, so the context scan
  // reads it too: a mask with `L.z` or a draw needs the tsset metadata or the
  // draw coordinates exactly as a folded one does (the engine parses the mask
  // against the same frame).
  const std::string ctxExpr =
      maskOut ? bygenArg + " " + *ifExpr : bygenArg;
  std::vector<std::string> args =
      translate::injectTsContextForExpr(baseArgs, ctxExpr, session);
  args = translate::injectRngContextForExpr(args, ctxExpr, session);
  // ARC-B1 (P2PERF): opt this verb into the resident plane, the same way its
  // siblings runGen/runReplace/runEgen already are. Without the opt-in the call
  // routes STATELESS — the engine re-reads the whole dataset from disk, writes a
  // whole new file, and the resident token is cleared, so the NEXT resident verb
  // pays a full re-adoption on top. The W0 counters put the benchmark at 3449
  // stateless trips and 2701 re-adoptions, and a `bysort: gen` in a loop is one
  // of the verbs feeding both.
  //
  // Sound because `--bygen` is frame-shaped like every other eval op: cmd_eval
  // builds its output column against the in-RAM frame and commits it, and
  // Frame::bygen computes through a sort ORDER vector without ever permuting the
  // frame's rows — so residency changes no row order, no column identity, and no
  // value. (The `bysort` half's in-place sort above is its own mutation,
  // done before this call.) The sort parenthetical is already folded into the single --bygen
  // expression above, so there is no second call to keep in step.
  SessionState::ResidentEligible bygenResident(session);
  nlohmann::json response;
  try {
    response = runCobraJson(args);
    translate::absorbRngDraws(response, session);
  } catch (const CobraError& ce) {
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  session.load(outPath);
  if (isReplace) {
    storage::recordReplaceType(session, newvar, storeType, finalStoreType);
    // A replace rewrites the target in place: it goes in the rewrittenVars slot,
    // as runReplace's does, never in newVars, which the console reads as
    // "(variable created)".
    return mutationResult(session, "gen", {}, {}, {newvar});
  }
  ojson block = mutationResult(session, "gen", newvar.empty() ? std::vector<std::string>{}
                                                               : std::vector<std::string>{newvar});
  translate::carryStoreOverflow(response, block);  // BUG-1123
  return block;
}

namespace {
// anyvalue/anycount/anymatch, and cut/concat in the forms the engine path does not
// serve, are computed here from the columns directly (help egen; the behaviours and
// messages are measured against StataBE 19.5, StataBE oracle arms F_* to O_*).

// numlist (the forms these options are written in): numbers, `a/b`, `a(s)b`.
bool expandEgenNumlist(const std::string& spec, std::vector<double>* out) {
  // Commas may separate the numbers as blanks do: `egen ... = cut(x), at(0,10,20,30)`
  // is the corpus spelling (probe_b4).
  std::string flat = spec;
  for (char& ch : flat)
    if (ch == ',') ch = ' ';
  std::istringstream iss(flat);
  std::string tok;
  while (iss >> tok) {
    const size_t lp = tok.find('(');
    const size_t slash = tok.find('/');
    char* end = nullptr;
    if (lp != std::string::npos && tok.back() != ')') {
      const size_t rp = tok.find(')', lp);
      if (rp == std::string::npos) return false;
      const double a = std::strtod(tok.substr(0, lp).c_str(), &end);
      const double st = std::strtod(tok.substr(lp + 1, rp - lp - 1).c_str(), &end);
      const double b = std::strtod(tok.substr(rp + 1).c_str(), &end);
      if (!(st > 0)) return false;
      for (double v = a; v <= b + 1e-9 * std::max(1.0, std::fabs(b)); v += st) out->push_back(v);
      continue;
    }
    if (slash != std::string::npos) {
      const double a = std::strtod(tok.substr(0, slash).c_str(), &end);
      const double b = std::strtod(tok.substr(slash + 1).c_str(), &end);
      for (double v = a; v <= b; v += 1.0) out->push_back(v);
      continue;
    }
    const double v = std::strtod(tok.c_str(), &end);
    if (end == nullptr || *end != '\0') return false;
    out->push_back(v);
  }
  return !out->empty();
}

// str# of a string column's longest value (str1 when every value is "", strL
// past str2045), the type `compress` and a widening replace leave.
std::string strTypeOfLongest(const Datastore& ds, const std::string& var) {
  const cecon::Column& c = ds.col(var);
  size_t longest = 0;
  for (int64_t i = 0; i < ds.nRows(); ++i) {
    const int32_t k = c.codes()[i];
    if (k >= 0) longest = std::max(longest, c.levels[static_cast<size_t>(k)].size());
  }
  if (longest > 2045) return "strL";
  return "str" + std::to_string(std::max<size_t>(1, longest));
}

using EgenOptions = std::vector<std::pair<std::string, eval::OptValue>>;

// What every computed function works from once its argument and options have passed.
struct ComputedEgen {
  const std::string& newvar;
  const std::vector<std::string>& vars;
  const std::optional<std::string>& ifExpr;
  const std::optional<std::string>& inRange;
  const std::string& storeType;
  SessionState& session;
};

// Option `full` as typed, under whichever abbreviation spelled it; nullptr when absent.
const eval::OptValue* optionNamed(const EgenOptions& opts, const EgenFnVocab& vocab,
                                  const char* full) {
  const eval::OptValue* found = nullptr;
  for (const auto& [key, value] : opts) {
    if (key == "by") continue;
    const EgenOptSpec* spec = matchEgenOpt(key, vocab.known);
    if (spec != nullptr && std::string(spec->full) == full) found = &value;
  }
  return found;
}

// The value a variable of storage type `type` holds for v. Only float changes a number
// these functions store: their integer targets widen instead (storage::promotedType).
double heldIn(const std::string& type, double v) {
  if (type != "float" || std::isnan(v)) return v;
  if (std::fabs(v) > static_cast<double>(std::numeric_limits<float>::max()))
    return std::numeric_limits<double>::quiet_NaN();
  return static_cast<double>(static_cast<float>(v));
}

// Stata's string(v) or string(v, format), by the expression layer's own function.
std::string numberAsText(double v, const std::string* format) {
  double operand = v;
  if (std::isnan(v)) {
    const int kind = cecon::missing_kind(v);
    operand = kind > 0 ? eval::stataMissingWithKind(kind) : eval::kStataMissingSentinel;
  }
  std::vector<eval::PyValue> args = {eval::PyValue::floating(operand)};
  if (format != nullptr) args.push_back(eval::PyValue::str(*format));
  return eval::fnString(args).strVal();
}

// One flag per row: inside the command's if and in. Returns the refusal, if any.
std::optional<ojson> flagQualifiedRows(const ComputedEgen& call, std::vector<char>* inside) {
  SessionState& session = call.session;
  const int64_t rows = session.datastore()->nRows();
  inside->assign(static_cast<size_t>(rows), 1);
  if (call.inRange.has_value() && !call.inRange->empty()) {
    const InRangeBounds bounds = parseInRangeBounds(*call.inRange, rows);
    if (!bounds.ok) {
      InRangeFilterResult refused;
      refused.ok = false;
      refused.refusal = bounds.refusal;
      return inRangeRejectError(refused, *call.inRange, "egen");
    }
    for (int64_t i = 0; i < rows; ++i)
      (*inside)[static_cast<size_t>(i)] = i + 1 >= bounds.a && i + 1 <= bounds.b;
  }
  if (!call.ifExpr.has_value() || call.ifExpr->empty()) return std::nullopt;
  RowIndexCache maskFrame;
  maskFrame.path = *session.currentHbin();
  maskFrame.nRows = rows;
  if (auto refusal = addIfMaskColumn(session, maskFrame, *call.ifExpr, {})) return refusal;
  {
    Datastore maskData(maskFrame.path);
    const double* truth = maskData.col(maskFrame.maskVar).f64();
    for (int64_t i = 0; i < rows; ++i)
      if (truth[i] == 0.0) (*inside)[static_cast<size_t>(i)] = 0;
  }
  cecon::native_unlink(maskFrame.path);
  return std::nullopt;
}

// Writes `payload` (one line per row) as new column `var` of the head (cobra setcol) and
// makes the result the head.
std::optional<ojson> installColumn(SessionState& session, const std::string& var, bool isText,
                                   const std::string& payload) {
  const std::string minted = mintScratchPath(session.workdir(), "egenval");
  forgetScratchMint(minted);
  const std::string valuesPath = minted.substr(0, minted.size() - 5) + ".csv";  // ".hbin"
  registerScratchMint(valuesPath);
  {
    std::ofstream file(cecon::to_native_path(valuesPath), std::ios::binary);
    file << payload;
    file.flush();
    if (!file) {
      cecon::native_unlink(valuesPath);
      return egenRcError("could not write the values of " + var + " (disk full or I/O error)", 693);
    }
  }
  const std::string outPath = nextPath(session.workdir(), "egenval");
  const CobraRunResult run =
      runCobra({"setcol", "--data", *session.currentHbin(), "--var", var, "--values", valuesPath,
                "--dtype", isText ? "factor" : "f64", "--new", "--out", outPath});
  cecon::native_unlink(valuesPath);
  if (!run.ok) {
    cecon::native_unlink(outPath);
    return engineErrorBlock(cobraErrorStr(errorFromRaw(run)));
  }
  // The engine creates --out before it can notice a full disk (BUG-124).
  if (!cecon::native_exists(outPath))
    return egenRcError("could not create " + var + " (disk full or I/O error)", 693);
  session.load(outPath);
  return std::nullopt;
}

std::optional<ojson> installNumbers(SessionState& session, const std::string& var,
                                    const std::vector<double>& values) {
  std::string payload;
  payload.reserve(values.size() * 4);
  char digits[40];
  for (double v : values) {
    if (!std::isnan(v)) {
      std::snprintf(digits, sizeof digits, "%.17g", v);
      payload += digits;
    }
    payload += '\n';
  }
  return installColumn(session, var, false, payload);
}

// The type a numeric result ends with. `created` is the one it was declared with;
// a widening keeps that type's display format.
void noteResultType(SessionState& session, const std::string& var, const std::string& created,
                    const std::string& ended) {
  session.implicitFloatVars.erase(var);
  session.varMeta[var].stataType = created;
  storage::recordReplaceType(session, var, created, ended);
}

// values() as an integer numlist. The two any* families word the refusal differently.
std::optional<ojson> readIntegerList(const std::string& text, const std::string& lead,
                                     std::vector<double>* out) {
  if (!expandEgenNumlist(text, out)) return egenRcError(lead + "invalid numlist", 121);
  for (double v : *out)
    if (v != std::floor(v)) return egenRcError(lead + "invalid numlist has noninteger elements", 126);
  return std::nullopt;
}

// A result label too long for a variable label goes to a note instead (Stata's 80-byte
// limit, measured: oracle part 5, N_*). `shortened` "" keeps `full` as the label.
void labelAnyResult(SessionState& session, const std::string& var, const std::string& full,
                    const std::string& shortened) {
  if (shortened.empty()) {
    session.setVarLabel(var, full);
    return;
  }
  session.setVarLabel(var, shortened);
  (void)dispatch("quietly notes " + var + ": " + full, session);
}

// anyvalue: the variable's value where it equals a listed integer, else missing.
ojson computeAnyvalue(const ComputedEgen& call, const std::vector<double>& wanted,
                      const std::string& typedValues) {
  SessionState& session = call.session;
  std::vector<char> inside;
  if (auto refusal = flagQualifiedRows(call, &inside)) return *refusal;
  const std::string& var = call.vars.front();
  const double* source = session.datastore()->col(var).f64();

  std::vector<double> kept(inside.size(), std::numeric_limits<double>::quiet_NaN());
  double lowest = std::numeric_limits<double>::infinity();
  double highest = -lowest;
  for (size_t i = 0; i < inside.size(); ++i) {
    if (!inside[i] || std::find(wanted.begin(), wanted.end(), source[i]) == wanted.end()) continue;
    kept[i] = source[i];
    lowest = std::min(lowest, source[i]);
    highest = std::max(highest, source[i]);
  }
  const bool anyKept = lowest <= highest;
  const std::string ended =
      anyKept ? storage::promotedType(call.storeType, false, lowest, highest) : call.storeType;
  for (double& v : kept) v = heldIn(ended, v);

  if (auto failed = installNumbers(session, call.newvar, kept)) return *failed;
  noteResultType(session, call.newvar, call.storeType, ended);
  const std::string label = var + " if " + typedValues;
  labelAnyResult(session, call.newvar, label, label.size() > 80 ? var + ": see notes" : "");
  return mutationResult(session, "egen", {call.newvar});
}

// anycount: (variable, listed value) pairs that match, a value counted as often as it is
// listed; anymatch: whether any does. Byte whatever was declared; 0 outside the sample.
ojson computeAnycount(const ComputedEgen& call, const std::vector<double>& wanted, bool asFlag) {
  SessionState& session = call.session;
  std::vector<char> inside;
  if (auto refusal = flagQualifiedRows(call, &inside)) return *refusal;
  std::vector<double> tally(inside.size(), 0.0);
  Datastore* ds = session.datastore();
  for (const std::string& var : call.vars) {
    const double* source = ds->col(var).f64();
    for (size_t i = 0; i < inside.size(); ++i) {
      if (!inside[i]) continue;
      tally[i] += static_cast<double>(std::count(wanted.begin(), wanted.end(), source[i]));
    }
  }
  double most = 0.0;
  for (double& n : tally) {
    if (asFlag) n = n > 0.0 ? 1.0 : 0.0;
    most = std::max(most, n);
  }

  std::string varNames;
  for (const std::string& var : call.vars) varNames += (varNames.empty() ? "" : " ") + var;
  std::string listed;
  char digits[40];
  for (double v : wanted) {
    std::snprintf(digits, sizeof digits, "%.0f", v);
    listed += (listed.empty() ? "" : " ") + std::string(digits);
  }

  if (auto failed = installNumbers(session, call.newvar, tally)) return *failed;
  noteResultType(session, call.newvar, "byte", storage::promotedType("byte", false, 0.0, most));
  const std::string label = varNames + " == " + listed;
  std::string shortened;
  if (varNames.size() >= 69) shortened = "see notes";
  else if (label.size() > 80) shortened = varNames + ": see notes";
  labelAnyResult(session, call.newvar, label, shortened);
  return mutationResult(session, "egen", {call.newvar});
}

// cut() takes a variable. Blanks and enclosing parentheses do not matter to Stata, which
// reads the argument as an expression but first requires summarize to accept it (measured:
// `cut(x y)` looks for xy, `cut((x))` runs, `cut(x*)` is "x* invalid name").
std::string cutArgumentName(const std::string& argText) {
  std::string name;
  for (char c : argText)
    if (!std::isspace(static_cast<unsigned char>(c))) name += c;
  while (name.size() >= 2 && name.front() == '(' && name.back() == ')')
    name = name.substr(1, name.size() - 2);
  return name;
}

bool isPlainVariableName(const std::string& name) {
  if (name.empty() || std::isdigit(static_cast<unsigned char>(name[0]))) return false;
  return std::all_of(name.begin(), name.end(), [](char c) {
    return cecon::ident::isIdentByte(static_cast<unsigned char>(c));
  });
}

// L.x and its kin: the engine computes them, into a scratch copy of the frame.
std::optional<ojson> engineColumnOf(SessionState& session, const std::string& tsExpr,
                                    std::vector<double>* out) {
  static const char* const kCopy = "__piet_egen_arg__";
  const std::string scratch = nextPath(session.workdir(), "egen_arg");
  try {
    runCobraJson(translate::injectTsContextForExpr(
        {"eval", "--data", *session.currentHbin(), "--gen", std::string(kCopy) + "=" + tsExpr,
         "--out", scratch},
        tsExpr, session));
  } catch (const CobraError& ce) {
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  {
    Datastore copy(scratch);
    const double* values = copy.col(kCopy).f64();
    out->assign(values, values + copy.nRows());
  }
  cecon::native_unlink(scratch);
  return std::nullopt;
}

// `var` with its last character (a whole UTF-8 sequence) swapped for each ending Stata
// tries, in the order Stata tries them: lowercase, digits, uppercase.
std::vector<std::string> respelledLabelNames(const std::string& var) {
  std::string kept = var;
  while (kept.size() > 1 && (static_cast<unsigned char>(kept.back()) & 0xC0) == 0x80)
    kept.pop_back();
  kept.pop_back();
  std::vector<std::string> names;
  names.reserve(62);
  for (char last : std::string("abcdefghijklmnopqrstuvwxyz0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"))
    names.push_back(kept + last);
  return names;
}

// The value-label set a labelled cut() uses: the variable's own name while no set holds it,
// else the first free respelling (measured: price -> prica, zz with za taken -> zb). With
// every respelling taken the last one is answered, and the caller adds to that set.
std::string freeValueLabelName(const SessionState& session, const std::string& var) {
  const auto isFree = [&](const std::string& name) { return session.labelDefs.count(name) == 0; };
  if (isFree(var)) return var;
  const std::vector<std::string> names = respelledLabelNames(var);
  const auto found = std::find_if(names.begin(), names.end(), isFree);
  return found != names.end() ? *found : names.back();
}

std::optional<ojson> errorIn(const ojson& blocks) {
  for (const auto& block : blocks)
    if (block.is_object() && block.value("type", std::string()) == "error") return block;
  return std::nullopt;
}

// Stata's default quantile rule for ascending values ([D] pctile, Methods and formulas):
// with P = N*p/100, the average of ranks P and P+1 when P is whole, else rank ceil(P).
double defaultQuantile(const std::vector<double>& ascending, double p) {
  const int64_t n = static_cast<int64_t>(ascending.size());
  const double position = static_cast<double>(n) * p / 100.0;
  const double whole = std::floor(position);
  int64_t rank = static_cast<int64_t>(whole);
  if (rank == whole && rank >= 1 && rank < n)
    return (ascending[static_cast<size_t>(rank - 1)] + ascending[static_cast<size_t>(rank)]) / 2.0;
  rank = std::min(std::max<int64_t>(rank + 1, 1), n);
  return ascending[static_cast<size_t>(rank - 1)];
}

// cut() over the session's float or double copy of the variable (the default storage
// type decides the binned value, whatever the result's type: BUG-1107). `edges` holds
// at()'s numbers, or nothing under group(k), which reads them off the sample: the
// minimum of the WHOLE variable, the k-1 inner quantiles held in the default type, and
// the sample's maximum plus one (StataBE: `cut(x) if x > 2, group(2) label` on 1..6
// names 1-, 4.5-, 7-).
ojson computeCut(const ComputedEgen& call, std::vector<double> edges, int quantiles,
                 bool wantIndex, bool wantLabel, const std::vector<double>* computedArg) {
  SessionState& session = call.session;
  std::vector<char> inside;
  if (auto refusal = flagQualifiedRows(call, &inside)) return *refusal;

  const std::string& defaultType = session.defaultStorageType;
  std::vector<double> binned(inside.size());
  {
    const double* source =
        computedArg != nullptr ? computedArg->data() : session.datastore()->col(call.vars.front()).f64();
    for (size_t i = 0; i < inside.size(); ++i) binned[i] = heldIn(defaultType, source[i]);
  }

  // at(): a value at or past the top edge has no interval and leaves the sample.
  std::vector<double> sample;
  sample.reserve(binned.size());
  for (size_t i = 0; i < binned.size(); ++i) {
    if (std::isnan(binned[i]) || (quantiles == 0 && binned[i] >= edges.back())) inside[i] = 0;
    if (inside[i]) sample.push_back(binned[i]);
  }
  std::sort(sample.begin(), sample.end());

  if (quantiles != 0) {
    if (sample.empty()) return egenRcError("no observations", 2000);
    double lowest = sample.front();
    for (double v : binned)
      if (!std::isnan(v)) lowest = std::min(lowest, v);
    edges.push_back(lowest);
    for (int i = 1; i < quantiles; ++i)
      edges.push_back(heldIn(defaultType, defaultQuantile(sample, 100.0 * i / quantiles)));
    edges.push_back(sample.back() + 1.0);
  }
  // A declared string type fails only here, on the first number it would store.
  if (call.storeType.empty()) return egenRcError("type mismatch", 109);

  // Filling the variable edge by edge in ascending order, every edge some row reaches is
  // stored at least once and widens an integer variable if it must, even when all those
  // rows go on to a higher edge.
  const bool asIndex = wantIndex || wantLabel || quantiles != 0;
  std::string held = call.storeType;
  std::vector<double> valueOfBin(edges.size(), std::numeric_limits<double>::quiet_NaN());
  for (size_t i = 0; i < edges.size() && !sample.empty() && sample.back() >= edges[i]; ++i) {
    const double v = asIndex ? static_cast<double>(i) : edges[i];
    held = storage::promotedType(held, v != std::floor(v), v, v);
    valueOfBin[i] = heldIn(held, v);
  }

  std::vector<double> result(binned.size(), std::numeric_limits<double>::quiet_NaN());
  for (size_t i = 0; i < binned.size(); ++i) {
    if (!inside[i]) continue;
    const auto above = std::upper_bound(edges.begin(), edges.end(), binned[i]);
    if (above != edges.begin()) result[i] = valueOfBin[static_cast<size_t>(above - edges.begin()) - 1];
  }

  // The set goes first: a refused definition must leave no variable behind.
  std::string setName;
  if (wantLabel) {
    setName = freeValueLabelName(session, call.newvar);
    std::string define = "quietly label define " + setName;
    for (size_t i = 0; i < edges.size(); ++i)
      define += " " + std::to_string(i) + " \"" + numberAsText(edges[i], nullptr) + "-\"";
    if (auto bad = errorIn(dispatch(define + ", add", session))) return *bad;
  }
  if (auto failed = installNumbers(session, call.newvar, result)) return *failed;
  noteResultType(session, call.newvar, call.storeType, held);
  if (wantLabel) {
    if (auto bad = errorIn(dispatch("quietly label values " + call.newvar + " " + setName, session)))
      return *bad;
  }
  return mutationResult(session, "egen", {call.newvar});
}

// format() as `format` would take it (Stata checks it that way, r(120)): %9g and %9.2 are
// not formats, a string format does not suit a number, and text with no % reaches
// string(), which answers "".
bool concatFormatUsable(const std::string& format) {
  if (format.empty() || format[0] != '%') return true;
  static const std::regex reWidthOnly(R"(^%-?0?\d*[fge]c?$)");
  return isValidDisplayFormat(format) && format.back() != 's' &&
         !std::regex_match(format, reWidthOnly);
}

// concat() with punct()/format(): as the engine's (a separator after every field, the last
// one dropped, blanks trimmed), and rows outside the sample are "". The str# is sized by
// the longest text before that drop and trim, the width the variable reached being built.
ojson computeConcat(const ComputedEgen& call, const std::string& punct, const std::string* format) {
  SessionState& session = call.session;
  std::vector<char> inside;
  if (auto refusal = flagQualifiedRows(call, &inside)) return *refusal;
  Datastore* ds = session.datastore();
  std::vector<const cecon::Column*> sources;
  std::vector<char> isText;
  for (const std::string& var : call.vars) {
    sources.push_back(&ds->col(var));
    isText.push_back(ds->dtype(var) != "f64");
  }

  std::string payload;
  size_t longest = 0;
  for (size_t row = 0; row < inside.size(); ++row) {
    std::string text;
    if (inside[row]) {
      for (size_t j = 0; j < sources.size(); ++j) {
        const cecon::Column& source = *sources[j];
        if (isText[j]) {
          const int32_t code = source.codes()[row];
          if (code >= 0) text += source.levels[static_cast<size_t>(code)];
        } else {
          try {
            text += numberAsText(source.f64()[row], format);
          } catch (const eval::PyExc&) {
            return egenRcError("Invalid format", 120);
          }
        }
        text += punct;
      }
      longest = std::max(longest, text.size());
      text.resize(text.size() - punct.size());
      const size_t first = text.find_first_not_of(' ');
      text = first == std::string::npos
                 ? std::string()
                 : text.substr(first, text.find_last_not_of(' ') - first + 1);
    }
    payload += '"';
    for (char c : text) {
      if (c == '"') payload += '"';
      payload += c;
    }
    payload += "\"\n";
  }

  if (auto failed = installColumn(session, call.newvar, true, payload)) return *failed;
  session.implicitFloatVars.erase(call.newvar);
  session.varMeta[call.newvar].stataType =
      longest > 2045 ? std::string("strL") : "str" + std::to_string(std::max<size_t>(1, longest));
  return mutationResult(session, "egen", {call.newvar});
}

std::optional<ojson> runEgenAdoFunction(const std::string& fn, const std::string& newvar,
                                        const std::string& argText,
                                        const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                        const std::optional<std::string>& ifExpr,
                                        const std::optional<std::string>& inRange,
                                        const std::string& storeType, SessionState& session) {
  const bool isCut = fn == "cut";
  const bool isConcat = fn == "concat";
  const bool isAnyvalue = fn == "anyvalue";
  const bool isAnyCounter = fn == "anycount" || fn == "anymatch";
  if (!isCut && !isConcat && !isAnyvalue && !isAnyCounter) return std::nullopt;

  const EgenFnVocab& vocab = egenFnVocab().at(fn);
  const eval::OptValue* atOpt = optionNamed(opts, vocab, "at");
  const eval::OptValue* groupOpt = optionNamed(opts, vocab, "group");
  const bool wantIndex = optionNamed(opts, vocab, "icodes") != nullptr;
  const bool wantLabel = optionNamed(opts, vocab, "label") != nullptr;
  const eval::OptValue* punctOpt = optionNamed(opts, vocab, "punct");
  const eval::OptValue* formatOpt = optionNamed(opts, vocab, "format");
  const eval::OptValue* valuesOpt = optionNamed(opts, vocab, "values");
  // A bare cut(x) or concat(a b) is the engine path's.
  if (isCut && atOpt == nullptr && groupOpt == nullptr && !wantIndex && !wantLabel)
    return std::nullopt;
  if (isConcat && punctOpt == nullptr && formatOpt == nullptr) return std::nullopt;

  // Stata reads the argument first, then the options, then the by-list.
  const std::string arg = trim(argText);
  std::vector<std::string> vars;
  if (!isCut) {
    loops::VarlistExpandResult expanded = loops::expandVarlist(arg, session);
    if (expanded.isError) return expanded.error;
    if (auto unknown = loops::checkKnownVarlist(expanded.tokens, session)) return unknown;
    vars = std::move(expanded.tokens);
    if (vars.empty()) return egenRcError("too few variables specified", 102);
    if (isAnyvalue && vars.size() > 1) return egenRcError("too many variables specified", 103);
  } else if (arg.empty()) {
    return egenRcError("invalid syntax", 198);
  }
  Datastore* ds = session.datastore();
  if (isAnyCounter) {
    for (const std::string& var : vars) {
      if (ds->dtype(var) != "f64") {
        return egenRcError("string variables not allowed in varlist;\n" + var +
                               " is a string variable", 109);
      }
    }
  }

  std::optional<ojson> unimplementedOpt;
  if (auto bad = egenOptionNet(opts, vocab, nullptr, &unimplementedOpt)) return bad;

  std::vector<double> numbers;
  if ((isAnyvalue || isAnyCounter) && valuesOpt == nullptr)
    return egenRcError("option values() required", 198);
  if (isAnyCounter) {
    if (auto bad = readIntegerList(valuesOpt->strValue, "values() invalid -- ", &numbers)) return bad;
  }
  double groupWanted = 0.0;
  if (isCut) {
    if (atOpt != nullptr) {
      if (!expandEgenNumlist(atOpt->strValue, &numbers))
        return egenRcError("at() invalid -- invalid numlist", 121);
      for (size_t j = 1; j < numbers.size(); ++j) {
        if (!(numbers[j - 1] < numbers[j]))
          return egenRcError("at() invalid -- invalid numlist has elements out of order", 124);
      }
    }
    if (groupOpt != nullptr) {
      const std::string text = trim(groupOpt->strValue);
      char* parsedTo = nullptr;
      groupWanted = std::strtod(text.c_str(), &parsedTo);
      if (text.empty() || *parsedTo != '\0')
        return egenRcError("option group() incorrectly specified", 198);
    }
  }

  for (const auto& entry : opts)
    if (entry.first == "by") return egenRcError("egen ... " + fn + "() may not be combined with by", 190);
  if (unimplementedOpt.has_value()) return unimplementedOpt;

  // group(0) is how Stata reads "no group()".
  const ComputedEgen call{newvar, vars, ifExpr, inRange, storeType, session};
  if (isAnyvalue) {
    if (auto bad = readIntegerList(valuesOpt->strValue, "", &numbers)) return bad;
    if (ds->dtype(vars.front()) != "f64" || storeType.empty()) return egenRcError("type mismatch", 109);
    return computeAnyvalue(call, numbers, trim(valuesOpt->strValue));
  }
  if (isAnyCounter) return computeAnycount(call, numbers, fn == "anymatch");

  if (isConcat) {
    std::string punct = punctOpt != nullptr ? punctOpt->strValue : std::string();
    if (punct.size() >= 2 && punct.front() == '"' && punct.back() == '"')
      punct = punct.substr(1, punct.size() - 2);
    const std::string format = formatOpt != nullptr ? trim(formatOpt->strValue) : std::string();
    if (!concatFormatUsable(format)) return egenRcError("Invalid format", 120);
    return computeConcat(call, punct, format.empty() ? nullptr : &format);
  }

  std::vector<double> computedArg;
  const std::string name = cutArgumentName(arg);
  if (translate::exprHasTsToken(name)) {
    if (auto bad = engineColumnOf(session, name, &computedArg)) return bad;
    ds = session.datastore();
  } else if (name.find_first_of("*?") != std::string::npos) {
    return egenRcError(name + " invalid name", 198);
  } else if (!isPlainVariableName(name)) {
    return egenRcError("cut(" + arg + ")   invalid, variable name expected", 198);
  } else {
    vars.push_back(name);
    if (auto unknown = loops::checkKnownVarlist(vars, session)) return unknown;
  }
  if (numbers.empty() && groupWanted == 0.0)
    return egenRcError("one of at() or group() must be specified", 198);
  if (!numbers.empty() && groupWanted != 0.0)
    return egenRcError("cannot specify both at() and group() option", 198);
  if (!vars.empty() && ds->dtype(vars.front()) != "f64") return egenRcError("type mismatch", 109);
  if (groupWanted != 0.0) {
    if (groupWanted != std::floor(groupWanted))
      return egenRcError("option nq() incorrectly specified", 198);
    if (groupWanted < 2.0)
      return egenRcError("nquantiles() must be greater than or equal to 2", 198);
    if (groupWanted > static_cast<double>(ds->nRows()) + 1.0)
      return egenRcError("nquantiles() must be less than or equal to number of observations plus one", 198);
  }
  return computeCut(call, std::move(numbers), static_cast<int>(groupWanted), wantIndex, wantLabel,
                    vars.empty() ? &computedArg : nullptr);
}

}  // namespace

ojson runEgen(const std::string& exprIn, const std::string& by,
              const std::optional<std::string>& ifExpr,
              const std::vector<std::pair<std::string, eval::OptValue>>& opts,
              SessionState& session, const std::optional<std::string>& inRange,
              const std::string& storeTypeIn, std::string* fixedTypeOut, bool gtools) {
  if (!session.currentHbin().has_value()) {
    return eval::noDataLoaded();
  }
  std::string expr = trim(exprIn);
  std::string outPath = nextPath(session.workdir(), "egen");

  auto eqPos = expr.find('=');
  std::string newvar = eqPos == std::string::npos ? "" : trim(expr.substr(0, eqPos));
  std::string rhs = eqPos == std::string::npos ? expr : trim(expr.substr(eqPos + 1));
  {
    static const std::regex reHead(R"(^(\w+)(\s*\(.*)$)");
    std::smatch hm;
    if (std::regex_match(rhs, hm, reHead)) {
      // egen function names are case-sensitive: `Rmax` looks for _gRmax.ado and
      // is r(199) in Stata, so the name is matched as typed.
      const std::string alias = rowAliasOf(hm.str(1));
      if (!alias.empty()) {
        rhs = alias + hm.str(2);
        expr = newvar.empty() ? rhs : newvar + " = " + rhs;
      }
    }
  }

  // BUG-1120: `egen _cons = mean(x)` is "_cons invalid varname" r(198), and so are
  // the storage types and if/in/using/with (`egen long = mean(x)`); a str# is
  // "str5 invalid variable name" (StataBE 19.5).
  if (auto r = strutil::newVarNameRefusal(newvar)) return egenRcError(r->message, r->rc);
  // BUG-1000: egen's ado runs `confirm new variable` before anything else, so an
  // existing target is r(110) and is never overwritten (the engine's column store
  // is create-or-replace: `egen x = sum(xd)` replaced x at rc 0). Oracle
  // w2ap_egen_family eg_ref_exist (rc 110, x untouched).
  if (egenTargetExists(session, newvar))
    return egenRcError("variable " + newvar + " already defined", 110);

  {
    static const std::regex reFnArgs(R"(^(\w+)\s*\((.*)\)\s*$)");
    std::smatch fa;
    if (std::regex_match(rhs, fa, reFnArgs)) {
      const std::string fn = lower(fa.str(1));
      if (auto bad = egenArgumentRefusal(fn, trim(fa.str(2)), session)) return *bad;
      // anyvalue/anycount/anymatch, cut(), at()/group() and concat(), punct()/
      // format(): computed in runEgenAdoFunction (aer_w2 w2b_dataio
      // egen_anyvalue .. egen_cut_icodes_label, w2b_dataio.log:1030-1112).
      if (auto r = runEgenAdoFunction(fn, newvar, fa.str(2), opts, ifExpr, inRange, storeTypeIn,
                                      session)) {
        // The function recorded the result's type; that, not the declaration, is
        // what the dispatcher records (BUG-1108).
        const bool failed = r->is_object() && r->value("type", std::string()) == "error";
        auto vm = session.varMeta.find(newvar);
        if (!failed && fixedTypeOut != nullptr && vm != session.varMeta.end() &&
            vm->second.stataType.has_value())
          *fixedTypeOut = *vm->second.stataType;
        return *r;
      }
    }
  }

  // BUG-1001: the function and its own option vocabulary (egenFnVocab). Every key
  // is resolved against the function's declared minimums, and the forwarded suffix
  // carries the CANONICAL names, which are the words the engine reads. egen is in
  // the dispatcher's kOptMinComplete, so the keys arrive unresolved except for
  // the measured `missing` row; the resolution below is the only one.
  std::string fnName;
  {
    static const std::regex reFnName(R"(^(\w+)\s*\()");
    std::smatch fm;
    if (std::regex_search(rhs, fm, reFnName)) fnName = lower(fm.str(1));
  }
  const bool haveBy = !trim(by).empty();
  const bool isRowFn = rowFuns().count(fnName) != 0;
  std::vector<std::pair<std::string, eval::OptValue>> fnOpts;  // canonical, by() excluded
  // An option the ado takes and this build does not compute: refused after the
  // by-list below, which the ado's `syntax` lets it reach (BUG-1106: `egen v =
  // concat(x k), by(k) decode` is r(190) in StataBE 19.5).
  std::optional<ojson> unimplementedOpt;
  const auto vocabIt = egenFnVocab().find(fnName);
  const auto unimplementedIt = unimplementedEgenFns().find(fnName);
  if (vocabIt != egenFnVocab().end()) {
    // gtools' gegen computes total() and sum() itself and takes `missing` only
    // in full: `gegen q = total(a), m` / `, mi` is `option m not allowed`
    // r(198) with gtools installed, where egen's _gtotal reads m (BUG-1111).
    if (auto err = egenOptionNet(opts, vocabIt->second, &fnOpts, &unimplementedOpt,
                                 gtools && (fnName == "total" || fnName == "sum")))
      return *err;
  } else if (fnName == "mean" || fnName == "var" || fnName == "group" || fnName == "tag" ||
             fnName == "cut" || fnName == "nvals") {
    // Engine functions outside BUG-1001's rows keep the net they had: by() for every
    // one, plus `missing` for group (BUG-233).
    std::unordered_set<std::string> known = {"by"};
    std::vector<std::pair<std::string, eval::OptValue>> netOpts = opts;
    if (fnName == "group") {
      // egen group() `Missing`: m, mi, ... are missing (BUG-1111). The key
      // arrives as typed (the dispatcher leaves egen's keys to this function).
      // gtools' gegen takes `missing` only in full (`gegen q = group(a), m` is
      // `option m not allowed` r(198) with gtools installed).
      static const std::vector<EgenOptSpec> kMissing = {{"missing", 1, false}};
      for (auto& kv : netOpts) {
        if (kv.first == "missing" || matchEgenOpt(kv.first, kMissing) == nullptr) continue;
        if (gtools)
          if (auto err = eval::rejectUnknownOptions({kv}, {})) return *err;
        kv.first = "missing";
      }
      known.insert("missing");
      // `label` (egen group()): the groups' value label, built below from the
      // grouping variables' values (aer_w2 w2b_dataio egen_group_label `1
      // zero`, w2b_dataio.log:1076). It is PIET-side, never forwarded.
      known.insert("label");
    }
    // What egen group() and egen tag() also declare, and this build does not
    // compute, is refused after the by-list (BUG-1106: `egen g = tag(x), by(k)
    // missing`, `egen g = group(x), by(k) lname(foo)` are r(190) in StataBE 19.5).
    static const std::vector<EgenOptSpec> kGroupUnimplemented = {
        {"lname", 2, true}, {"truncate", 1, true}, {"autotype", 8, false},
        {"qlabel", 6, false}, {"nsort", 5, true}, {"debug", 5, false}};
    static const std::vector<EgenOptSpec> kTagUnimplemented = {{"missing", 1, false}};
    std::vector<std::pair<std::string, eval::OptValue>> netted;
    for (const auto& kv : netOpts) {
      const EgenOptSpec* un = nullptr;
      if (known.count(kv.first) == 0 && fnName == "group")
        un = matchEgenOpt(kv.first, kGroupUnimplemented);
      else if (known.count(kv.first) == 0 && fnName == "tag")
        un = matchEgenOpt(kv.first, kTagUnimplemented);
      if (un == nullptr) {
        netted.push_back(kv);
      } else if (!unimplementedOpt.has_value()) {
        unimplementedOpt = egenUnimplementedOption(un->full);
      }
    }
    if (auto err = eval::rejectUnknownOptions(netted, known)) return *err;
    for (const auto& kv : netted)
      if (kv.first != "by" && kv.first != "label") fnOpts.push_back(kv);
  } else if (unimplementedIt != unimplementedEgenFns().end()) {
    // Its ado ships, and its own `syntax` reads the options: one it does not
    // declare is r(198) (BUG-1110 follow-up; StataBE 19.5 `egen q = mdev(k), bar`,
    // `egen q = rowmedian(k x), by(x) bogus`).
    for (const auto& kv : opts)
      if (kv.first != "by" && matchEgenOpt(kv.first, unimplementedIt->second.opts) == nullptr)
        return ojson(eval::engineError("option `" + kv.first + "` not allowed",
                                       "unsupported_option"));
  } else {
    // A name no table above lists is not an egen function this build computes,
    // and an option on it must not ride along unread if the engine ever learns
    // the name. egen.ado looks the function up before its ado reads any option,
    // so the answer is the lookup's (BUG-1110: `egen q = nosuchfn(k), bar` is
    // r(133) "unknown egen function nosuchfn()" in StataBE 19.5, never the
    // option's r(198)).
    for (const auto& kv : opts)
      if (kv.first != "by") return unknownEgenFunctionError(fnName);
  }
  // _egennoby (every row function's ado, _ggroup, _gtag, _gconcat, _gcut and the
  // ados this build does not compute but noByName): a by-list, as an option or as
  // a by prefix the dispatcher turned into one, is r(190). Each ado reads its
  // `syntax` first, so the argument (egenArgumentRefusal) and an option it does
  // not take (the net above) answer before it. Oracle eg_ref_row_by; BUG-1106,
  // StataBE 19.5: `bysort k: egen g = tag(x)`, `egen g = group(x), by(k)`, `egen v
  // = concat(x k), by(k)`, `egen q = cut(x), by(k)` -> "egen ... tag() may not be
  // combined with by" r(190); `egen g = group(x), by(k) bogus` -> r(198); `egen g
  // = group(x nosuch), by(k) bogus` -> "variable nosuch not found" r(111).
  if (haveBy) {
    std::string noBy;
    if (isRowFn || fnName == "group" || fnName == "tag" || fnName == "concat" || fnName == "cut")
      noBy = fnName == "rsum" ? std::string("rowtotal") : fnName;  // _grsum -> _growtotal
    else if (unimplementedIt != unimplementedEgenFns().end() &&
             unimplementedIt->second.noByName != nullptr)
      noBy = unimplementedIt->second.noByName;
    if (!noBy.empty())
      return egenRcError("egen ... " + noBy + "() may not be combined with by", 190);
  }
  if (unimplementedOpt.has_value()) return *unimplementedOpt;
  if (unimplementedIt != unimplementedEgenFns().end()) return unknownEgenFunctionError(fnName);
  auto fnOpt = [&](const char* key) -> const eval::OptValue* {
    for (const auto& kv : fnOpts)
      if (kv.first == key) return &kv.second;
    return nullptr;
  };
  // opts_exclusive in egen rank() and egen mode(): r(198), naming the first two.
  auto exclusive = [&](std::initializer_list<const char*> keys) -> std::optional<ojson> {
    std::vector<std::string> given;
    for (const char* k : keys)
      if (fnOpt(k) != nullptr)
        given.push_back(std::string(k) + (std::string(k) == "nummode" ? "()" : ""));
    if (given.size() < 2) return std::nullopt;
    return egenRcError("only one of options " + given[0] + " or " + given[1] + " is allowed", 198);
  };
  if (fnName == "rank") {
    if (auto e = exclusive({"field", "track", "unique"})) return *e;  // oracle eg_ref_rank_two
  } else if (fnName == "mode") {
    if (auto e = exclusive({"minmode", "maxmode", "nummode"})) return *e;  // eg_ref_mode_two
    if (const eval::OptValue* nm = fnOpt("nummode")) {
      // Nummode(numlist max=1 int >= 1): anything else is r(125)/r(126).
      const std::string t = trim(nm->strValue);
      if (t.empty() || t.find_first_not_of("0123456789") != std::string::npos ||
          std::strtol(t.c_str(), nullptr, 10) < 1)
        return egenRcError("nummode(): invalid numlist has elements outside of allowed range",
                           125);
    }
  } else if (fnName == "std") {
    // egen std(): sd() and its old spelling std() together are r(198); the
    // engine reads the one value as sd().
    if (fnOpt("sd") != nullptr && fnOpt("std") != nullptr)
      return egenRcError("only one of sd() or std() is allowed", 198);
    for (auto& kv : fnOpts)
      if (kv.first == "std") kv.first = "sd";
    if (const eval::OptValue* sv = fnOpt("sd")) {
      // Sd(numlist max=1 >0).
      char* end = nullptr;
      const std::string t = trim(sv->strValue);
      const double d = std::strtod(t.c_str(), &end);
      if (t.empty() || end == nullptr || *end != '\0' || !(d > 0.0))
        return egenRcError("sd(): invalid numlist has elements outside of allowed range", 125);
    }
    if (const eval::OptValue* mv = fnOpt("mean")) {
      char* end = nullptr;
      const std::string t = trim(mv->strValue);
      (void)std::strtod(t.c_str(), &end);
      if (t.empty() || end == nullptr || *end != '\0')
        // Named as typed, as syntax does (StataBE 19.5: `std(x), m(abc)` is
        // `option m() incorrectly specified`).
        for (const auto& [k, v] : opts) {
          const EgenOptSpec* sp = k == "by" ? nullptr : matchEgenOpt(k, vocabIt->second.known);
          if (sp != nullptr && std::string(sp->full) == "mean")
            return egenRcError("option " + k + "() incorrectly specified", 198);
        }
    }
  } else if (fnName == "pctile") {
    // egen pctile(): P(real 50), and outside (0, 100) is r(198).
    if (const eval::OptValue* pv = fnOpt("p")) {
      char* end = nullptr;
      const std::string t = trim(pv->strValue);
      const double d = std::strtod(t.c_str(), &end);
      if (t.empty() || end == nullptr || *end != '\0')
        return egenRcError("option p() incorrectly specified", 198);
      if (!(d > 0.0 && d < 100.0))
        return egenRcError("p(" + t + ") must be between 0 and 100", 198);
    }
  } else if (fnName == "cut") {
    // at() is a numlist: Stata reads `0 10 20`, `0,10,20` and `0(10)70` alike
    // (w2f_residuals.log cut_space/cut_comma/cut_rangelist/cut_range all
    // `0|10|30`). The engine reads a blank-separated list, so commas become
    // blanks and an INTEGER range a(step)b or a/b is expanded here; a
    // fractional range stays as typed for the engine's own refusal (Stata
    // builds those edges in float, which a double expansion would not match).
    for (auto& kv : fnOpts) {
      if (kv.first != "at" || kv.second.isBool) continue;
      std::string t = kv.second.strValue;
      std::replace(t.begin(), t.end(), ',', ' ');
      static const std::regex reStep(R"(^(-?\d+)\((-?\d+)\)(-?\d+)$)");
      static const std::regex reSlash(R"(^(-?\d+)/(-?\d+)$)");
      std::string out;
      std::istringstream in(t);
      std::string tok;
      while (in >> tok) {
        std::smatch m;
        long long a = 0, st = 0, b = 0;
        bool range = false;
        if (std::regex_match(tok, m, reStep)) {
          a = std::stoll(m.str(1));
          st = std::stoll(m.str(2));
          b = std::stoll(m.str(3));
          range = st != 0 && ((st > 0 && a <= b) || (st < 0 && a >= b));
        } else if (std::regex_match(tok, m, reSlash)) {
          a = std::stoll(m.str(1));
          b = std::stoll(m.str(2));
          st = a <= b ? 1 : -1;
          range = true;
        }
        if (range && (b - a) / st < 100000) {
          for (long long v = a; st > 0 ? v <= b : v >= b; v += st)
            out += (out.empty() ? "" : " ") + std::to_string(v);
        } else {
          out += (out.empty() ? "" : " ") + tok;
        }
      }
      kv.second.strValue = out;
    }
  } else if (fnName == "seq") {
    // from() and block() take a number that is a whole number within int's
    // range, and refuse anything else -- the first such option as typed --
    // before anything is computed (BUG-1114, StataBE 19.5:
    // 2.0, +2, 1e1 and -2 run; 1.5, abc, an empty from(), 1e10 and 0x1p1 are
    // `option from() incorrectly specified` r(198)).
    // The message names the option as typed (`seq(), f(1.5)` is `option f()
    // incorrectly specified`), so the typed keys are walked, not fnOpts' canonical
    // ones; both keep the typed order.
    static const std::regex reNum(R"(^[+-]?(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?$)");
    for (const auto& [k, v] : opts) {
      const EgenOptSpec* sp = k == "by" ? nullptr : matchEgenOpt(k, vocabIt->second.known);
      if (sp == nullptr || (std::string(sp->full) != "from" && std::string(sp->full) != "block"))
        continue;
      const std::string t = trim(v.strValue);
      const double d = std::regex_match(t, reNum) ? std::strtod(t.c_str(), nullptr) : std::nan("");
      if (!(d == std::trunc(d) && d >= -2147483648.0 && d <= 2147483647.0))
        return egenRcError("option " + k + "() incorrectly specified", 198);
    }
    // `block(0)` must be at least 1, r(498) (eg_ref_seq_block).
    if (const eval::OptValue* bv = fnOpt("block")) {
      const std::string t = trim(bv->strValue);
      if (std::strtod(t.c_str(), nullptr) < 1.0)
        return egenRcError("block(" + t + ") must be at least 1", 498);
    }
    // A given to() must be written as an integer, which reads the text: a
    // sign and digits (+3, 03 and -1 run), else `'2.5' found where integer
    // expected` r(7) (3.0, 1e1 and . alike), and a second word after one is
    // `'4' found where nothing expected` r(7).
    if (const eval::OptValue* tv = fnOpt("to")) {
      const std::string t = trim(tv->strValue);
      const size_t cut = t.find_first_of(" \t,");
      const std::string first = t.substr(0, cut);
      static const std::regex reInt(R"(^[+-]?\d+$)");
      if (!t.empty() && !std::regex_match(first, reInt))
        return egenRcError("'" + first + "' found where integer expected", 7);
      if (cut != std::string::npos) {
        const std::string rest = trim(t.substr(cut));
        const std::string next = rest[0] == ',' ? std::string(",")
                                                : rest.substr(0, rest.find_first_of(" \t,"));
        return egenRcError("'" + next + "' found where nothing expected", 7);
      }
    }
  }

  // BUG-471 T2: the result's storage type rides the ONE call that computes it
  // (never the if/in plumbing calls). tag()/seq()/mode() ignore the requested
  // type in Stata (DIAGNOSIS section B15-B17), so they get no flag at all. The
  // replace-built results (rowtotal/rowmax/rowmin, default rank, cut with a
  // fractional edge) widen an integer type in Stata; the engine refuses that
  // with bad_input, and the refusal is surfaced as an r(198) option refusal —
  // never retried without the flag, which would store a double under an
  // integer label.
  const std::string egenFn = storage::egenFunctionName(rhs);
  const std::string storeType = storage::egenIgnoresStoreType(egenFn) ? std::string() : storeTypeIn;
  auto egenStoreError = [&](const CobraError& ce) -> ojson {
    if (storage::isIntegerType(storeType) && ce.code == "bad_input" &&
        std::string(ce.what()).find("--store-type") != std::string::npos) {
      return ojson(eval::unsupportedOption(
          storeType, "egen " + egenFn + "() builds its result with replace, which widens a " +
                         storeType + " in Stata; PIET cannot widen it yet. Declare the result " +
                         "float or double instead."));
    }
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  };

  // BUG-042: `egen NEWVAR = fn(...) if COND` — Stata restricts BOTH the
  // computation (fn() sees only the matching rows: e.g. mean() is the
  // subset's mean, group()/tag() only consider the subset's distinct
  // values) AND the assignment (non-matching rows come back missing) to
  // the if-subset. Mechanical fix (row-filter infra reuse, mirroring
  // summarize/count's keepif prefilter): tag every row with a stable
  // index, filter a COPY to the if-subset, run the *exact same* egen op
  // there, then merge the freshly-computed column back onto the FULL
  // (untouched) dataset by that index — unmatched rows come back missing
  // for NEWVAR (cobra merge's standard unmatched-master semantics), while
  // every OTHER variable is untouched (unlike a whole-dataset keepif,
  // which would also drop rows from every other column). This is uniform
  // across every egen function family (row/stat/tag/group/seq) since it
  // never special-cases which fn was requested — it just re-runs whatever
  // cobra call was going to happen anyway, against the filtered subset.
  // No ifExpr/inRange: dataPath == the live hbin, zero extra cobra calls,
  // byte-identical to the pre-fix behavior (freeze-compatible).
  //
  // BS-090 (BETA Phase G): `in a/b` now composes with `if` via AND here
  // too — reusing the SAME kIdxVar this block already materializes for the
  // if-merge-back key (it is already the GLOBAL row index, `_n` over the
  // whole dataset), so unlike piet::handlers::applyIfAndInFilter's general
  // case, no SEPARATE idx materialization is needed.
  bool haveIf = ifExpr.has_value() && !ifExpr->empty();
  // BUG-369 follow-up, E1 guard: with if/in, this handler computes fn() on a
  // row-FILTERED copy (the whole point of the BUG-042 fix above). But a TS
  // operator inside fn() must be resolved against the FULL tsset series before
  // any sample restriction (E1 — the same law cmds_ts.cpp/tsops enforces), and
  // on the filtered copy a boundary row's lag would silently come back missing
  // or skip across removed rows. A silently-wrong number is the one class this
  // codebase refuses to ship — so the combination is an honest reject with the
  // working recipe, until someone builds a pre-materialize step. `egen m =
  // mean(L.y)` WITHOUT if/in stays fully supported (whole-frame evaluation,
  // E1 holds by construction).
  if ((haveIf || (inRange.has_value() && !inRange->empty())) &&
      translate::exprHasTsToken(rhs)) {
    return engineErrorBlock(
        "egen with if/in cannot take a time-series operator inside the function yet: "
        "the operator must see the full tsset series BEFORE the sample restriction "
        "(materialize it first — `gen tmp = L.x`, then egen over tmp with your if/in)",
        "unimplemented");
  }
  bool haveIn = inRange.has_value() && !inRange->empty();
  // Stage 1b: the common (no if/in) egen is a clean single mutation whose --out
  // becomes the head and is never read back as a file — opt it into the
  // resident frame plane. The if/in path instead builds intermediate scratch
  // hbins and reads them back AS FILES out of resident order (the `Datastore
  // srcDs` below; the merge-back references the idx temp created several steps
  // earlier), so it stays on the default stateless path (real temp files, and
  // its `--data currentHbin` idx call already materializes any virtual head).
  std::optional<SessionState::ResidentEligible> egenResident;
  if (!(haveIf || haveIn)) egenResident.emplace(session);
  std::string egenDataPath = *session.currentHbin();
  std::string idxFullPath;
  static const char* kIdxVar = "__egen_ifidx__";
  // BUG-840: under by() -- the option, or a by-prefix, which dispatcher.cpp
  // turns into the option -- Stata evaluates the `if` inside each by-group: its
  // _n, _N and subscripts count within the group (w2m_bymask E01 :373 and E02
  // :383 `if _n > 1` give . 100 100 | . 80 80; E05 :411, D09 :712). A --keepif
  // over the whole frame counted them over the dataset (E01 gave . 100 100 | 90
  // 90 90 at rc 0). The selection is built by a --bygen over the by-groups in
  // their current row order (bysort has already sorted the rows in place), in
  // the same call that tags the merge-back index, and the filter keeps it. The
  // `!=0` keeps a missing condition selecting (BUG-622). Without by() nothing
  // changes.
  static const char* kSelVar = "__egen_ifsel__";
  std::vector<std::string> byList;
  {
    std::string tok;
    for (char ch : by + " ") {
      if (ch == ' ' || ch == ',' || ch == '\t') {
        if (!tok.empty()) byList.push_back(tok);
        tok.clear();
      } else {
        tok += ch;
      }
    }
  }
  const bool groupIf = haveIf && !byList.empty();
  if (haveIf || haveIn) {
    idxFullPath = nextPath(session.workdir(), "egen_ifidx");
    std::vector<std::string> idxArgs = {"eval", "--data", *session.currentHbin(), "--bygen",
                                        std::string(kIdxVar) + "=_n"};
    if (groupIf) {
      const std::string selArg = makeBygenExpr(
          std::string(kSelVar) + "=((" + *ifExpr + ")!=0)", byList, std::nullopt);
      idxArgs.push_back("--bygen");
      idxArgs.push_back(selArg);
      idxArgs = translate::injectTsContextForExpr(idxArgs, selArg, session);
    }
    idxArgs.push_back("--out");
    idxArgs.push_back(idxFullPath);
    try {
      runCobraJson(idxArgs);
    } catch (const CobraError& ce) {
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    std::string combinedIf = groupIf ? std::string(kSelVar) : ifExpr.value_or("");
    if (haveIn) {
      Datastore srcDs(*session.currentHbin());
      InRangeBounds rr = parseInRangeBounds(*inRange, srcDs.nRows());
      if (!rr.ok) {
        InRangeFilterResult fr;
        fr.ok = false;
        fr.refusal = rr.refusal;
        return inRangeRejectError(fr, *inRange, "egen");
      }
      std::string rangePred = std::string(kIdxVar) + ">=" + std::to_string(rr.a) + " & " +
                              std::string(kIdxVar) + "<=" + std::to_string(rr.b);
      // BUG-622 (its egen.cpp sites): `!=0` on the `if` half, as
      // applyIfAndInFilter does (BUG-534). The group-relative selection is
      // already 0/1.
      if (groupIf) {
        combinedIf = std::string(kSelVar) + " & (" + rangePred + ")";
      } else {
        combinedIf = haveIf ? ("((" + *ifExpr + ")!=0) & (" + rangePred + ")") : rangePred;
      }
    }
    std::string filteredPath = nextPath(session.workdir(), "egen_iffilter");
    std::vector<std::string> filterArgs = translate::injectTsContextForExpr(
        {"eval", "--data", idxFullPath, "--keepif", combinedIf, "--out", filteredPath},
        combinedIf, session);  // BUG-369 follow-up: the if itself may carry L.
    try {
      runCobraJson(filterArgs);
    } catch (const CobraError& ce) {
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    egenDataPath = filteredPath;
  }

  // Row-function detection: `fn(a b c)` (space-separated Stata varlist) with
  // fn in _ROW_FUNS -> --rowegen NEWVAR=fn(a,b,c) (comma-separated).
  static const std::regex reRowFn(R"(^(\w+)\s*\(([^)]+)\)\s*$)");
  std::smatch m;
  std::string computedPath;
  if (std::regex_match(rhs, m, reRowFn) && rowFuns().count(lower(m.str(1)))) {
    std::string rowFnName = m.str(1);
    std::string argsStr = trim(m.str(2));
    // BUG-1001: the varlist expands its wildcards and ranges through the one
    // expander (`rowmax(w*)` reached the engine as a column named `w*`); a
    // pattern that matches nothing is r(111). Space-separated -> comma-separated
    // (mutate.py: ",".join(args_str.split())).
    loops::VarlistExpandResult rowVars = loops::expandVarlist(argsStr, session);
    if (rowVars.isError) return rowVars.error;
    // rowsd() takes at least two variables: one is r(102). Oracle eg_ref_rowsd_one.
    if (fnName == "rowsd" && rowVars.tokens.size() < 2)
      return egenRcError("too few variables specified", 102);
    std::string argsCsv;
    for (size_t t = 0; t < rowVars.tokens.size(); ++t) {
      if (t) argsCsv += ",";
      argsCsv += rowVars.tokens[t];
    }
    std::string rowegenExpr = newvar + "=" + rowFnName + "(" + argsCsv + ")";
    if (fnOpt("missing") != nullptr) rowegenExpr += ",missing";  // rowtotal only
    if (fnOpt("strok") != nullptr) rowegenExpr += ",strok";      // rownonmiss only
    std::vector<std::string> args = {"eval", "--data", egenDataPath, "--rowegen",
                                      rowegenExpr, "--out", outPath};
    storage::appendStoreTypeArg(args, storeType);  // BUG-471 T2
    try {
      runCobraJson(args);
    } catch (const CobraError& ce) {
      // A string variable in a row function (rownonmiss without strok included)
      // is Stata's r(109) type mismatch (StataBE oracle arms D_str_*).
      if (std::string(ce.what()).find("variable used where a number is required") !=
          std::string::npos)
        return egenRcError("type mismatch", 109);
      return egenStoreError(ce);
    }
    computedPath = outPath;
  } else {
    // Non-row function: --eigen path (cobra's --egen flag). BS-097: cobra's
    // own `--egen` CLI parsing (apps/cmds/cmds_data.cpp) never validates the
    // comma-suffix tail at all — it only ever substring-scans for the
    // specific keys it reads (by()/missing/p()) and drops everything else on
    // the floor. The per-function net at the top of runEgen (BUG-1001) is
    // therefore the ONLY place an unsupported egen option is caught, and it has
    // already run: `fnOpts` holds the canonical names it accepted.
    const std::string egenFnName = fnName;
    std::vector<std::pair<std::string, eval::OptValue>> fwdOpts = fnOpts;
    // Stata's seq(): an absent to() is _N of the WHOLE dataset. Under if/in this
    // call runs on the filtered copy, whose row count is smaller, so the full
    // count travels as an explicit to().
    if (egenFnName == "seq" && fnOpt("to") == nullptr && (haveIf || haveIn)) {
      eval::OptValue tv;
      tv.isBool = false;
      tv.strValue = std::to_string(Datastore(*session.currentHbin()).nRows());
      fwdOpts.emplace_back("to", tv);
    }
    // gtools' gegen computes total(), sum() and mean() in its own plugin, which
    // sums in data order with or without by() (StataBE 19.5 with gtools: gegen
    // total of 1e16, twenty-two 1s and -1e16 is 0 and of ten 0.1s
    // +1.fffffffffffffX-001, where egen gives 22 and 1). Its pc() and std() are
    // official egen's, and keep summarize's exact sum.
    if (gtools && (egenFnName == "total" || egenFnName == "sum" || egenFnName == "mean"))
      fwdOpts.emplace_back("dataorder", eval::OptValue::ofBool(true));
    // The suffix carries every option EXCEPT by() (appended separately below).
    std::string optsSuffix = buildOptsSuffix(fwdOpts, {"by"});
    std::string cobraExpr = expr + optsSuffix;

    if (!by.empty()) {
      // cobra eval --egen already parses by() out of the expr; append it.
      cobraExpr += ", by(" + by + ")";
    }

    std::vector<std::string> args = translate::injectTsContextForExpr(
        {"eval", "--data", egenDataPath, "--egen", cobraExpr, "--out", outPath},
        cobraExpr, session);  // BUG-369 follow-up: `egen m = mean(L.y)`
    storage::appendStoreTypeArg(args, storeType);  // BUG-471 T2
    // BUG-471 T2 fix-up (F3): Stata's seq() result starts as a byte and widens as a
    // replace would (egen_seq_type), so its type follows the values it stored. The
    // computing call summarizes the new column after the egen (eval's
    // --summarize reads the final frame), so the min/max cost no extra engine
    // call and no dispatch(): r() and e() are untouched. Under if/in this frame
    // is the filtered copy, i.e. exactly the rows the replace touched.
    const bool seqSummary = fixedTypeOut != nullptr && egenFnName == "seq" && !newvar.empty();
    if (seqSummary) {
      args.push_back("--summarize");
      args.push_back(newvar);
    }
    ojson egenOut;
    try {
      egenOut = runCobraJson(args);
    } catch (const CobraError& ce) {
      const std::string what = ce.what();
      if (what.find("--egen unknown fn") != std::string::npos)
        return unknownEgenFunctionError(egenFnName);
      // A string where the function's `gen double x = (exp)` needs a number is
      // Stata's r(109) type mismatch (eg_ref_strarg, eg_ref_strarg_by).
      if (what.find("variable used where a number is required") != std::string::npos ||
          what.find("string literal used where a number is required") != std::string::npos)
        return egenRcError("type mismatch", 109);
      // egen mode() with nummode() checks exit 198 (eg_ref_mode_num).
      if (egenFnName == "mode" && what.find("nummode(") != std::string::npos)
        return egenRcError(what, 198);
      return egenStoreError(ce);
    }
    if (seqSummary) {
      const ojson summ = egenOut.value("summarize", ojson::object()).value(newvar, ojson::object());
      const auto num = [&](const char* k) {
        return summ.contains(k) && summ[k].is_number() ? summ[k].get<double>() : 0.0;
      };
      // No stored value (every row excluded): the `gen byte` stands.
      *fixedTypeOut = num("N") > 0 ? storage::promotedType("byte", false, num("min"), num("max"))
                                 : std::string("byte");
    }
    computedPath = outPath;
  }

  std::string finalPath = computedPath;
  if (haveIf || haveIn) {
    // Merge NEWVAR back onto the untouched full dataset by the stable idx
    // (--keepusing restricts the using-side to idx+NEWVAR only — merging
    // the full column set back in would immediately fail cobra's
    // both-sides-have-this-column conflict check). --nogenerate skips the
    // _merge indicator (this isn't a user-visible merge, just plumbing).
    std::string mergedPath = nextPath(session.workdir(), "egen_ifmerge");
    std::vector<std::string> mergeArgs = {
        "merge",       "--data", idxFullPath, "--using",     computedPath, "--type", "1:1",
        "--key",       kIdxVar,  "--keepusing", newvar,       "--nogenerate", "--out", mergedPath};
    CobraRunResult mcr = runCobra(mergeArgs);
    if (!mcr.ok) {
      CobraError ce = errorFromRaw(mcr);
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    std::string dropIdxPath = nextPath(session.workdir(), "egen_ifdropidx");
    std::vector<std::string> dropArgs = {"eval", "--data", mergedPath, "--drop", kIdxVar};
    if (groupIf) {
      dropArgs.push_back("--drop");
      dropArgs.push_back(kSelVar);
    }
    dropArgs.push_back("--out");
    dropArgs.push_back(dropIdxPath);
    try {
      runCobraJson(dropArgs);
    } catch (const CobraError& ce) {
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    finalPath = dropIdxPath;
  }

  session.load(finalPath);
  // BUG-1109: egen mode() ends with `qui compress g` for a string source
  // (:124-126), so the result is str# of its longest value, str1 when every value
  // is "", whatever the source's width or a declared type (strL too; StataBE
  // 19.5). The ado leaves before that only without `missing', when the sample
  // has no observation or the source is "" on every one of them
  // (CheckMissingVarlist): the result keeps the source's type, which the
  // dispatcher carries. The sample is the filtered copy under if/in.
  if (fnName == "mode" && fixedTypeOut != nullptr && !newvar.empty()) {
    static const std::regex reSrc(R"(^\w+\s*\(\s*([A-Za-z_]\w*)\s*\)$)");
    std::smatch sm;
    Datastore* ds = session.datastore();
    if (std::regex_match(rhs, sm, reSrc) && ds != nullptr && ds->has(newvar) &&
        ds->has(sm.str(1)) && ds->dtype(sm.str(1)) == "factor") {
      bool leftEarly = false;
      if (fnOpt("missing") == nullptr) {
        std::optional<Datastore> filtered;
        if (haveIf || haveIn) filtered.emplace(egenDataPath);
        const Datastore& sample = filtered.has_value() ? *filtered : *ds;
        const cecon::Column& sc = sample.col(sm.str(1));
        leftEarly = true;
        for (int64_t i = 0; i < sample.nRows() && leftEarly; ++i) {
          const int32_t k = sc.codes()[i];
          if (k >= 0 && !sc.levels[static_cast<size_t>(k)].empty()) leftEarly = false;
        }
      }
      if (!leftEarly) *fixedTypeOut = strTypeOfLongest(*ds, newvar);
    }
  }
  // concat() ignores egen's type too: the result is a string variable as wide as
  // the longest text it holds (StataBE 19.5: `egen
  // double c = concat(x s)` is str4; the dispatcher recorded double, or an
  // untyped float that made `: type` r(101)). This is the engine path, with no
  // punct() to trim off; runEgenAdoFunction records its own result's type.
  if (fnName == "concat" && fixedTypeOut != nullptr && !newvar.empty()) {
    Datastore* ds = session.datastore();
    if (ds != nullptr && ds->has(newvar) && ds->dtype(newvar) == "factor")
      *fixedTypeOut = strTypeOfLongest(*ds, newvar);
  }
  bool groupLabel = false;
  if (fnName == "group")
    for (const auto& kv : opts)
      if (kv.first == "label") groupLabel = true;
  if (groupLabel && !newvar.empty()) {
    // egen group() `label`: each group's label is its grouping variables'
    // values, value labels decoded, joined by a blank; the set is named
    // after the new variable.
    static const std::regex reArgs(R"(^\w+\s*\((.*)\)\s*$)");
    std::smatch am;
    std::vector<std::string> gvars;
    if (std::regex_match(rhs, am, reArgs)) {
      loops::VarlistExpandResult vx = loops::expandVarlist(trim(am.str(1)), session);
      if (!vx.isError) gvars = vx.tokens;
    }
    Datastore* gds = session.datastore();
    if (gds != nullptr && gds->has(newvar) && !gvars.empty()) {
      const cecon::Column& gc = gds->col(newvar);
      std::map<long long, std::string> labelOf;
      for (int64_t r = 0; r < gds->nRows(); ++r) {
        const double code = gc.f64()[r];
        if (std::isnan(code)) continue;
        const long long c = static_cast<long long>(code);
        if (labelOf.count(c)) continue;
        std::string text;
        for (const auto& gv : gvars) {
          if (!gds->has(gv)) continue;
          const cecon::Column& col = gds->col(gv);
          std::string piece;
          if (col.dtype == cecon::DType::Factor) {
            const int32_t k = col.codes()[r];
            piece = k >= 0 ? col.levels[static_cast<size_t>(k)] : std::string();
          } else {
            const double v = col.f64()[r];
            bool done = false;
            auto lv = session.varLblname.find(gv);
            if (lv != session.varLblname.end() && !std::isnan(v) && v == std::floor(v)) {
              auto ld = session.labelDefs.find(lv->second);
              if (ld != session.labelDefs.end()) {
                auto it = ld->second.find(std::to_string(static_cast<long long>(v)));
                if (it != ld->second.end()) { piece = it->second; done = true; }
              }
            }
            if (!done) {
              char buf[64];
              std::snprintf(buf, sizeof(buf), "%.9g", v);
              piece = std::isnan(v) ? std::string(".") : std::string(buf);
            }
          }
          text += (text.empty() ? "" : " ") + piece;
        }
        labelOf[c] = text;
      }
      for (const auto& [c, text] : labelOf) {
        std::string esc;
        for (char ch : text) esc += (ch == '"') ? '\'' : ch;
        (void)piet::dispatch("label define " + newvar + " " + std::to_string(c) + " \"" + esc +
                                 "\", add",
                             session);
      }
      (void)piet::dispatch("label values " + newvar + " " + newvar, session);
    }
  }
  return mutationResult(session, "egen",
                        newvar.empty() ? std::vector<std::string>{} : std::vector<std::string>{newvar});
}

}  // namespace piet::handlers
