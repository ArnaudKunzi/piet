// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// AER tier-1 (2026-10-28), lane ENV: macro and program-environment verbs --
// `macro drop|define|shift|list|dir`, `require`, `_dots`, `postutil`, `markout`
// and `lrtest`. Answers: tests/eval/golden/aer_w2/w2ax_t1_official.log and
// w2ax_t1_community.log (StataBE 19.5), plus the lane's own StataBE probes
// (envp1-3, recorded in the selftest's comments where an arm rests on one).
//
// EVERY VERB HERE IS WIRING OVER A STORE OR A PRIMITIVE THAT ALREADY EXISTED.
//   macro     session.eval.globals/locals (the OrderedDicts the macro layer owns);
//             `define` goes through eval::tryCaptureDefinition -- the very call
//             the dispatcher makes for a `global`/`local` line -- so a defined
//             macro is stored by the one definition path, quotes and `=` included.
//   _dots     display blocks; the mark's "no newline" is the display block's
//             `continue` flag, which doexec already joins (`display ... _continue`).
//   postutil  session.postfiles; `clear` is runPostclose per open handle.
//   markout   a dispatched `quietly replace`, the shape runMarksample uses.
//   lrtest    the stored e() of `estimates store` (SessionState::estimatesModels,
//             StoredModel::eScalars/eStrings) and the live e().
//   require   an accept-note, the `ssc install`/`which` precedent.
//
// The defect classes of handlers_aer_util.cpp's header apply verbatim: every
// return is a block object with a "type" key or an array of them (SHAPE);
// every Stata rc that is not 198/199 travels in the `reason` channel as
// "r(NNN)" (RC); every option a command reads is registered below at namespace
// scope, real-but-unread options in `unimplemented` (VOCABULARY); lrtest
// replaces r() itself (R-CLASS).
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <exception>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "piet/cclass.h"             // substituteCRefs: the dispatcher's own pre-pass for a definition line
#include "piet/datastore.h"
#include "piet/dispatcher.h"         // markout: a dispatched `quietly replace`
#include "piet/display_hardening.h"  // fmtStataDefaultG: what `display <number>` prints
#include "piet/eval/errors.h"
#include "piet/eval/macros.h"        // hasMacroRefs, tryCaptureDefinition
#include "piet/eval/pyvalue.h"       // pythonFloatRepr: a round-trippable number for $S_3/$S_6/$S_7
#include "piet/eval/session.h"       // replaceRClass
#include "piet/eval/tokenizer.h"
#include "piet/handlers.h"
#include "piet/session_state.h"
#include "piet/stata_format.h"       // renderValue/padToField: the one %w.pf renderer
#include "piet/strutil.h"

// ---------------------------------------------------------------------------
// OPTION VOCABULARIES -- namespace scope, before any dispatch (defect class 3).
namespace {

// require's own `syntax` line: INSTALL ADOPATH() FROM() DEBUG() VERBOSE STRICT.
// install/strict/verbose are READ: each answers within the accept-note (nothing
// is installed or version-checked in PIET, and the note says so). adopath()/
// from()/debug() choose WHERE a package is searched or fetched from, or feed a
// fake starbang line; none has a meaning without a package store, so each is
// refused by name rather than accepted unread. `list` is the `require, list`
// form, a different command shape.
const piet::eval::OptVocab& kVocabRequire = piet::eval::registerOptVocab(
    {"require"}, {{"install", "strict", "verbose"}, {"adopath", "from", "debug", "list"}});

// _dots' `syntax` line: title() reps() NODOTS DOTS() MAXDOTS(). All five read.
// KNOWN (review d1 D6/R10): Stata's lowercase/all-caps spellings admit no
// abbreviation, so `_dots 0, ti(T)` and `require estout, inst` are r(198)
// there, while the registry's prefix rule accepts them here.
const piet::eval::OptVocab& kVocabDots = piet::eval::registerOptVocab(
    {"_dots"}, {{"title", "reps", "nodots", "dots", "maxdots"}, {}});

// postutil takes no options.
const piet::eval::OptVocab& kVocabPostutil =
    piet::eval::registerOptVocab({"postutil"}, {{}, {}});

// markout: STRok SYSMISsok. `sysmissok` keeps a system-missing `.` and marks
// out only the extended .a-.z -- a distinction this build's frame does not
// carry (extended missings are stored as `.`; see runMvencode's note), so it
// is refused rather than answered as if every missing were `.`.
const piet::eval::OptVocab& kVocabMarkout =
    piet::eval::registerOptVocab({"markout"}, {{"strok"}, {"sysmissok"}});

// lrtest: Df() FORCE STats DIr ASIS, plus the pre-8 syntax's saving()/using()/
// model(). `stats` and `dir` print `estimates stats`/`estimates dir` tables
// after the test; the old syntax keeps models under LRTEST_ names. None is
// done here, so each is refused by name. `Df()` declares `d` as its minimum
// spelling (review l1 L15: `lrtest b3 b1, d(5)` is r(df) 5), which the prefix
// rule cannot reach -- `d` also begins `dir` -- so it is a declared alias.
const piet::eval::OptVocab& kVocabLrtest = piet::eval::registerOptVocab(
    {"lrtest"},
    {{"df", "force", "asis"}, {"stats", "dir", "saving", "using", "model"}, {{"d", "df"}}});

}  // namespace

namespace piet::handlers {

using ojson = nlohmann::ordered_json;

namespace {

// ===========================================================================
// Shared helpers
// ===========================================================================

// An error whose Stata rc is known. `reason` "r(NNN)" is the exact-code channel
// eval::coarseRc honours; 198 also carries the syntax_error code so the coarse
// map agrees with it.
ojson envError(const std::string& message, int rc) {
  return ojson(eval::errorBlock(rc == 198 ? "syntax_error" : "engine_error", message, std::nullopt,
                                std::nullopt, std::nullopt, "r(" + std::to_string(rc) + ")"));
}

ojson displayBlock(const std::string& text) {
  ojson b;
  b["type"] = "display";
  b["text"] = text;
  return b;
}

bool isErrorBlock(const ojson& b) {
  return b.is_object() && b.value("type", std::string()) == "error";
}

const ojson* firstError(const ojson& result) {
  if (isErrorBlock(result)) return &result;
  if (result.is_array()) {
    for (const auto& b : result)
      if (isErrorBlock(b)) return &b;
  }
  return nullptr;
}

std::string joinWith(const std::vector<std::string>& v, const std::string& sep) {
  std::string out;
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) out += sep;
    out += v[i];
  }
  return out;
}

std::vector<std::string> splitWs(const std::string& s) {
  std::vector<std::string> out;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    if (i >= s.size()) break;
    size_t j = i;
    while (j < s.size() && s[j] != ' ' && s[j] != '\t') ++j;
    out.push_back(s.substr(i, j - i));
    i = j;
  }
  return out;
}

std::string dequoteOpt(std::string s) {
  s = strutil::trim(s);
  if (s.size() >= 2 && s.front() == '"' && s.back() == '"') return s.substr(1, s.size() - 2);
  if (s.size() >= 4 && s.compare(0, 2, "`\"") == 0 && s.compare(s.size() - 2, 2, "\"'") == 0)
    return s.substr(2, s.size() - 4);
  return s;
}

std::optional<std::string> optStr(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                  const std::string& key) {
  for (const auto& [k, v] : opts)
    if (k == key && !v.isBool) return v.strValue;
  return std::nullopt;
}

bool hasOpt(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
            const std::string& key) {
  for (const auto& [k, v] : opts) {
    (void)v;
    if (k == key) return true;
  }
  return false;
}

std::optional<double> parseNumber(const std::string& sIn) {
  const std::string s = strutil::trim(sIn);
  if (s.empty()) return std::nullopt;
  try {
    size_t used = 0;
    const double v = std::stod(s, &used);
    if (used != s.size() || !std::isfinite(v)) return std::nullopt;
    return v;
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

// What `display <number>` prints: the default %9.0g, without its padding.
std::string shownNumber(double v) { return strutil::trim(fmtStataDefaultG(v)); }

// A `%w.pf` / `%w.0fc` rendering through the one renderer, padded the way
// `display` pads (padToField). NaN prints Stata's `.`.
std::string fixedFmt(double v, int width, int prec, bool group) {
  fmt::StataFmtSpec sp;
  sp.hasWidth = true;
  sp.width = width;
  sp.prec = prec;
  sp.conv = 'f';
  sp.groupThousands = group;
  if (std::isnan(v)) return fmt::padToField(".", sp);
  const std::optional<std::string> r = fmt::renderValue(v, sp);
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.*f", prec, v);
  return fmt::padToField(r.value_or(std::string(buf)), sp);
}

// ===========================================================================
// macro
// ===========================================================================
//
// Implements `macro drop|define|shift|list|dir` per macro's help file;
// identified against StataBE probes (w2ax_t1_official.log macro_* arms and the
// lane's envp1/envp2/envp3 probes).
//
// WHICH NAMES ARE NOT THE USER'S. The macro store also holds PIET's own state,
// and `macro drop _all` / `macro dir` must neither erase nor show it:
//   * a global whose name starts with `__piet` (texdoc's open path, the
//     frmttable stores -- handlers_aer_tables.cpp);
//   * the local `_rc` (the dispatcher keeps the return code there);
//   * any name that is not an identifier (program.cpp's " __piet_progclass"
//     marker, the reserved \x01 keys).
// And one class Stata itself keeps: `macro drop _all` leaves every global
// whose name starts with `S_` (envp3: `global S_mine mine`, `macro drop _all`,
// `$S_mine` is still `mine`; envp2: S_ADO/S_OS survive while F1..F8 go).
bool internalMacroName(const std::string& n, bool isLocal) {
  if (n.empty()) return true;
  const unsigned char c0 = static_cast<unsigned char>(n[0]);
  if (!(std::isalnum(c0) || c0 == '_')) return true;
  if (n.rfind("__piet", 0) == 0) return true;
  if (isLocal && n == "_rc") return true;
  return false;
}

// A global's name: Stata's identifier rule, at most 32 characters.
bool validGlobalName(const std::string& n) {
  if (n.empty() || n.size() > 32) return false;
  const unsigned char c0 = static_cast<unsigned char>(n[0]);
  if (!(std::isalpha(c0) || c0 == '_')) return false;
  for (unsigned char c : n)
    if (!(std::isalnum(c) || c == '_')) return false;
  return true;
}

// A local's name (after the `_` that marks it): identifier characters, digits
// first allowed (`_1` is the positional local 1), at most 31 characters.
bool validLocalName(const std::string& n) {
  if (n.empty() || n.size() > 31) return false;
  for (unsigned char c : n)
    if (!(std::isalnum(c) || c == '_')) return false;
  return true;
}

bool validPattern(const std::string& p) {
  if (p.empty()) return false;
  for (unsigned char c : p)
    if (!(std::isalnum(c) || c == '_' || c == '*' || c == '?' || c == '~')) return false;
  return true;
}

// `macro list` / `macro dir` line: "NAME:" padded to column 16, then the
// value; a label of 16 or more characters puts the value on the next line,
// indented 16 (envp1: `abcdefghijklmn: x14`, `abcdefghijklmno:` then
// 16 blanks + `x15`; oracle macro_list `t1LST:          hello world`).
std::string listLine(const std::string& label, const std::string& value) {
  const std::string head = label + ":";
  if (head.size() <= 15) return head + std::string(16 - head.size(), ' ') + value;
  return head + "\n" + std::string(16, ' ') + value;
}

ojson macroDrop(const std::vector<std::string>& toks, SessionState& session) {
  if (toks.empty()) return envError("invalid syntax", 198);
  // Validate every name before touching the store, so a bad name late in the
  // list does not leave the earlier ones half-dropped.
  for (const auto& t : toks) {
    if (t == "_all") continue;
    const bool isLocal = t.size() > 1 && t[0] == '_';
    const std::string bare = isLocal ? t.substr(1) : t;
    const bool wild = bare.find_first_of("*?~") != std::string::npos;
    const bool ok = wild ? validPattern(bare)
                         : (isLocal ? validLocalName(bare) : validGlobalName(bare));
    if (!ok) return envError(t + " invalid name", 198);
  }
  auto dropMatching = [&](eval::OrderedDict& store, const std::string& pattern, bool isLocal) {
    std::vector<std::string> names;
    for (const auto& kv : store.items())
      if (!internalMacroName(kv.first, isLocal) &&
          strutil::globMatch(pattern == "~" ? "*" : pattern, kv.first))
        names.push_back(kv.first);
    for (const auto& n : names) store.erase(n);
  };
  for (const auto& t : toks) {
    if (t == "_all") {
      // envp2/envp3 + oracle macro_drop_all: every user global (but `S_*`)
      // AND every local of the current scope. KNOWN (review m3): inside a
      // foreach body that includes the loop's own macro, which can end PIET's
      // loop early where Stata's runs on.
      std::vector<std::string> g;
      for (const auto& kv : session.eval.globals.items())
        if (!internalMacroName(kv.first, false) && kv.first.rfind("S_", 0) != 0)
          g.push_back(kv.first);
      for (const auto& n : g) session.eval.globals.erase(n);
      dropMatching(session.eval.locals, "*", true);
      continue;
    }
    const bool isLocal = t.size() > 1 && t[0] == '_';
    const std::string bare = isLocal ? t.substr(1) : t;
    eval::OrderedDict& store = isLocal ? session.eval.locals : session.eval.globals;
    if (bare.find_first_of("*?~") != std::string::npos) {
      dropMatching(store, bare, isLocal);
    } else if (!internalMacroName(bare, isLocal)) {
      store.erase(bare);  // a name that is not there is silent, rc 0 (macro_drop_missing)
    }
  }
  return ojson::array();
}

// `macro define NAME rest` is `global NAME rest`, and `macro define _NAME rest`
// is `local NAME rest` (envp1 mdef_local: `3 + 4` stored as text; mdef_eq:
// `= 3 + 4` evaluates to 7). The rewritten line goes through the dispatcher's
// own definition path, so quoting, compound quotes and `=` behave exactly as
// they do for `global`/`local` (macro_define: `"x y"` stores x y).
ojson macroDefine(const std::string& restRaw, SessionState& session, const std::string& line) {
  const std::string rest = strutil::trim(restRaw);
  if (rest.empty()) return envError("invalid syntax", 198);
  size_t sp = rest.find_first_of(" \t");
  const std::string name = sp == std::string::npos ? rest : rest.substr(0, sp);
  const std::string value = sp == std::string::npos ? std::string() : rest.substr(sp);
  const bool isLocal = name.size() > 1 && name[0] == '_';
  const std::string bare = isLocal ? name.substr(1) : name;
  if (!(isLocal ? validLocalName(bare) : validGlobalName(bare)))
    return envError(name + " invalid name", 198);
  const std::string defLine = (isLocal ? "local " : "global ") + bare + value;
  eval::MacroFnError mfe;
  if (!eval::tryCaptureDefinition(substituteCRefs(defLine, session), session.eval, &mfe))
    return envError("invalid syntax", 198);
  if (mfe.failed())
    return ojson(eval::errorBlock("engine_error", mfe.message, std::nullopt, std::nullopt,
                                  strutil::trim(line), "r(" + mfe.rc + ")"));
  return ojson::array();
}

// `macro shift [#]`: the numbered locals `1`, `2`, ... move down by # (default
// 1); the top # become unset and `0` is left as it was (oracle macro_shift
// `|q|r||p q r|`, macro_shift2 `|r|s||`; envp1 ms_big: shift 5 of three
// arguments clears all three). Outside a program the do-file's numbered locals
// are the ones shifted -- usually none, rc 0 (macro_shift_outside).
ojson macroShift(const std::vector<std::string>& args, SessionState& session) {
  long long k = 1;
  if (args.size() > 1) return envError("invalid syntax", 198);  // envp1 ms_two
  if (args.size() == 1) {
    const std::string& a = args[0];
    if (a.empty() || !std::all_of(a.begin(), a.end(), [](unsigned char c) { return std::isdigit(c); }))
      return envError("invalid syntax", 198);  // envp1 ms_x, ms_neg
    try {
      k = std::stoll(a);
    } catch (const std::exception&) {
      return envError("invalid syntax", 198);
    }
  }
  if (k == 0) return ojson::array();  // envp1 ms_zero
  long long top = 0;
  for (const auto& kv : session.eval.locals.items()) {
    const std::string& n = kv.first;
    if (n.empty() || n[0] == '0' || n.size() > 9) continue;
    if (!std::all_of(n.begin(), n.end(), [](unsigned char c) { return std::isdigit(c); })) continue;
    top = std::max(top, std::stoll(n));
  }
  for (long long i = 1; i <= top; ++i) {
    const std::string from = std::to_string(i + k);
    const std::string* v = (i + k <= top) ? session.eval.locals.find(from) : nullptr;
    if (v != nullptr) {
      const std::string copy = *v;
      session.eval.locals.set(std::to_string(i), copy);
    } else {
      session.eval.locals.erase(std::to_string(i));
    }
  }
  return ojson::array();
}

// `macro list [names | _all]` and `macro dir [names]` (one command: envp1
// mdir_extra, `macro dir extra` answers as `macro list extra`). Bare/_all:
// globals newest first, then locals newest first as `_name` (envp3). Stata
// keeps no empty macro, so an empty value is "not found" (envp1 ml_empty).
ojson macroList(const std::vector<std::string>& toks, SessionState& session) {
  ojson out = ojson::array();
  std::vector<std::string> lines;
  const bool all = toks.empty() || (toks.size() == 1 && toks[0] == "_all");
  if (all) {
    const auto& g = session.eval.globals.items();
    for (size_t i = g.size(); i-- > 0;)
      if (!internalMacroName(g[i].first, false) && !g[i].second.empty())
        lines.push_back(listLine(g[i].first, g[i].second));
    const auto& l = session.eval.locals.items();
    for (size_t i = l.size(); i-- > 0;)
      if (!internalMacroName(l[i].first, true) && !l[i].second.empty())
        lines.push_back(listLine("_" + l[i].first, l[i].second));
  } else {
    for (const auto& t : toks) {
      const bool isLocal = t.size() > 1 && t[0] == '_';
      const std::string bare = isLocal ? t.substr(1) : t;
      if (!(isLocal ? validLocalName(bare) : validGlobalName(bare))) {
        if (!lines.empty()) out.push_back(displayBlock(joinWith(lines, "\n")));
        out.push_back(envError(t + " invalid name", 198));
        return out;
      }
      const std::string* v = internalMacroName(bare, isLocal)
                                 ? nullptr
                                 : (isLocal ? session.eval.locals.find(bare)
                                            : session.eval.globals.find(bare));
      if (v == nullptr || v->empty()) {
        // macro_list_missing: `global macro $t1nosuch not found`; envp1
        // ml_noloc: "local macro `nosuchloc' not found". Both r(111).
        if (!lines.empty()) out.push_back(displayBlock(joinWith(lines, "\n")));
        out.push_back(envError(isLocal ? "local macro `" + bare + "' not found"
                                       : "global macro $" + bare + " not found",
                               111));
        return out;
      }
      lines.push_back(listLine(t, *v));
    }
  }
  if (!lines.empty()) out.push_back(displayBlock(joinWith(lines, "\n")));
  return out;
}

}  // namespace

// The raw-line pre-pass (dispatcher.cpp, both sites). capture/quietly/noisily
// are already stripped. `macro` is in the tokenizer's unsupported_macro net, so
// every `macro ...` line is consumed here -- the expanded site included -- and
// the net never sees one. On the raw site a line that still carries a macro
// reference is left alone (false) and answered at the expanded site, so
// `macro drop $fh` drops the macro $fh NAMES; the test is eval::hasMacroRefs,
// the very gate the dispatcher's own expansion step uses.
bool tryMacroVerbIntercept(const std::string& line, SessionState& session, const ojson& echo,
                           ojson& out, bool expanded) {
  const size_t h = line.find_first_not_of(" \t");
  if (h == std::string::npos) return false;
  // Memchr-class gate before any allocation: the command word starts "ma".
  if (line.size() < h + 2 || std::tolower(static_cast<unsigned char>(line[h])) != 'm' ||
      std::tolower(static_cast<unsigned char>(line[h + 1])) != 'a')
    return false;
  size_t e = line.find_first_of(" \t", h);
  const std::string word = strutil::lower(line.substr(h, e == std::string::npos ? std::string::npos : e - h));
  // ma:cro (macro's help file): ma, mac, macr, macro.
  if (word != "ma" && word != "mac" && word != "macr" && word != "macro") return false;
  if (!expanded && eval::hasMacroRefs(line)) return false;

  const std::string rest = e == std::string::npos ? std::string() : line.substr(e);
  const size_t sh = rest.find_first_not_of(" \t");
  std::string sub, subRest;
  if (sh != std::string::npos) {
    const size_t se = rest.find_first_of(" \t", sh);
    sub = rest.substr(sh, se == std::string::npos ? std::string::npos : se - sh);
    subRest = se == std::string::npos ? std::string() : rest.substr(se);
  }
  const std::vector<std::string> args = splitWs(subRest);

  ojson r;
  if (sub == "drop") {
    r = macroDrop(args, session);
  } else if (sub.size() >= 2 && std::string("define").compare(0, sub.size(), sub) == 0) {
    // de:fine -- `macro de`/`def`/`defi` are rc 0 in StataBE (review m1 m5/m5b,
    // m2 b4); a single `d` is r(198) (m2 b3/b5), and `dr`/`dro` are not drop.
    r = macroDefine(subRest, session, line);
  } else if (sub == "s" || sub == "sh" || sub == "shi" || sub == "shif" || sub == "shift") {
    r = macroShift(args, session);
  } else if (sub == "l" || sub == "li" || sub == "lis" || sub == "list" || sub == "di" ||
             sub == "dir") {
    r = macroList(args, session);
  } else {
    // macro_badsub (`macro bogus t1K1`) and a bare `macro`: invalid syntax.
    r = envError("invalid syntax", 198);
  }
  if (r.is_array() && r.empty()) {
    out = ojson::array();
    return true;
  }
  out = ojson::array({echo});
  if (r.is_array()) {
    for (const auto& b : r) out.push_back(b);
  } else {
    out.push_back(r);
  }
  return true;
}

// ===========================================================================
// require (Sergio Correia, Matthew P. Seay)
// ===========================================================================
//
// Implements require's requirement grammar per require's help file;
// identified against StataBE probes (w2ax_t1_community.log require_ge,
// require_eq, require_bare, require_two; envp2 req_*).
//
// The requirement is ONE package, optionally followed by an operator and a
// version: `pkg`, `pkg>=v`, `pkg==v`, `pkg=v` (= is ==). Whatever follows the
// package name is read as the operator, so a second package name is an
// operator that is not one of those -- require_two `require estout distinct`
// is r(9) `assert failed: inlist("distinct", ">=", "==", "")`, and envp2
// req_gt `estout>3` is r(9) the same way. A syntax rule, so it is mirrored.
//
// Everything past the grammar is an installed-files fact PIET cannot see:
// PIET runs no ado-files and keeps no package store, so it neither finds the
// package (Stata: r(601) when absent) nor reads its version (r(2226)/r(2227)
// when too old). It answers rc 0 with a note that says exactly that, the
// `ssc install` / `which` accept-note precedent. `require stata>=V` IS a fact
// PIET knows -- the Stata release it answers for is 19.5 (the oracles') -- so
// V above that is Stata's r(9) (envp2 req_stata21), and V at or below it rc 0.
nlohmann::ordered_json runRequire(const eval::ParsedCommand& parsed, SessionState& session) {
  (void)session;
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabRequire.known,
                                            kVocabRequire.unimplemented))
    return ojson(*err);
  const std::string req = strutil::trim(parsed.argText);
  if (req.empty()) return envError("something required", 100);  // envp2 req_none
  if (req.rfind("using", 0) == 0 && (req.size() == 5 || req[5] == ' ' || req[5] == '\t'))
    return ojson(eval::refusedCommand("require using", "no_package_store"));

  auto isOpChar = [](char c) { return c == '>' || c == '='; };
  size_t p = 0;
  while (p < req.size() && !isOpChar(req[p]) && req[p] != ' ' && req[p] != '\t') ++p;
  const std::string package = req.substr(0, p);
  while (p < req.size() && (req[p] == ' ' || req[p] == '\t')) ++p;
  std::string op;
  if (p < req.size()) {
    size_t q = p;
    if (isOpChar(req[q])) {
      while (q < req.size() && isOpChar(req[q])) ++q;
    } else {
      while (q < req.size() && !isOpChar(req[q]) && req[q] != ' ' && req[q] != '\t') ++q;
    }
    op = req.substr(p, q - p);
    p = q;
  }
  const std::string version = strutil::trim(req.substr(p));
  if (op == "=") op = "==";
  if (op != ">=" && op != "==" && !op.empty())
    return envError("assert failed: inlist(\"" + op + "\", \">=\", \"==\", \"\")", 9);
  if (package.empty()) return envError("invalid syntax", 198);

  if (package == "stata") {
    // `require stata>=V` runs `version V: qui` (review d1): no operator, an
    // empty or non-decimal V (`stata>=`, `stata>=abc`, `stata>=1e1`) is the
    // version command's r(198); V below 1 or above the release is r(9)
    // (`stata>=0` R14, `stata>=19.6` R8, `stata >= 20` R3); 16, 17.0, 19,
    // 19.5 are rc 0.
    if (op.empty()) return envError("invalid syntax", 198);  // R1 `require stata`
    bool decimal = !version.empty();
    bool sawDigit = false, sawDot = false;
    for (char c : version) {
      if (std::isdigit(static_cast<unsigned char>(c))) {
        sawDigit = true;
      } else if (c == '.' && !sawDot) {
        sawDot = true;
      } else {
        decimal = false;
      }
    }
    if (!decimal || !sawDigit) return envError("invalid syntax", 198);
    const double v = parseNumber(version).value_or(0.0);
    if (v < 1.0 || v > 19.5)
      return envError("this is version 19.5 of Stata (the release PIET answers for); it cannot "
                      "run version " + fixedFmt(v, 1, 1, false) + " programs",
                      9);
    return ojson::array();
  }
  return displayBlock("(require " + req +
                      ": PIET bundles its own implementations; installation and version not checked)");
}

// ===========================================================================
// _dots
// ===========================================================================
//
// Implements _dots per _dots's ado (it has no help file); identified against
// StataBE probes (w2ax_t1_official.log dots_header, dots_loop, dots_x; envp2
// dots_*). `_dots 0, title(T) reps(#)` prints the title line (`T (#)` with the
// count comma-grouped, `T` alone without reps) and the 50-column ruler (not
// under `nodots`); `_dots i rc` prints one mark with no newline -- `.` for
// rc 0/text/result/input, `x` for 1/error, `e` 2, `n` 3, `s` -1, `?` anything
// else -- and every 50*dots()th mark closes its line with ` %5.0fc i`;
// `_dots i` alone ends a line that is not at a 50-mark boundary, and a bare
// `_dots` ends the line. A mark is a display block with `continue`, which
// doexec joins onto the next display (the `display ... _continue` mechanism).
nlohmann::ordered_json runDots(const eval::ParsedCommand& parsed, SessionState& session) {
  (void)session;
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabDots.known, kVocabDots.unimplemented))
    return ojson(*err);
  // reps(numlist integer max=1), dots()/maxdots() (numlist >0 integer max=1).
  auto intOpt = [&](const std::string& key, bool positive, long long* out) -> std::optional<ojson> {
    const std::optional<std::string> s = optStr(parsed.opts, key);
    if (!s.has_value()) return std::nullopt;
    const std::optional<double> v = parseNumber(*s);
    if (!v.has_value()) return envError(key + "() invalid -- invalid numlist", 121);
    if (std::floor(*v) != *v)
      return envError(key + "() invalid -- invalid number, noninteger value", 126);  // envp2 dots_badreps
    if (positive && *v <= 0)
      return envError(key + "() invalid -- invalid numlist has elements outside of allowed range",
                      125);
    *out = static_cast<long long>(*v);
    return std::nullopt;
  };
  long long reps = -1, dots = 1, maxdots = 0;
  if (auto e = intOpt("reps", false, &reps)) return *e;
  if (auto e = intOpt("dots", true, &dots)) return *e;
  if (auto e = intOpt("maxdots", true, &maxdots)) return *e;
  const bool haveMaxdots = optStr(parsed.opts, "maxdots").has_value();

  const std::vector<std::string> toks = splitWs(parsed.argText);
  const std::string i = toks.size() > 0 ? toks[0] : std::string();
  const std::string rc = toks.size() > 1 ? toks[1] : std::string();

  if (i == "0") {
    ojson out = ojson::array();
    const std::string title = dequoteOpt(optStr(parsed.opts, "title").value_or(std::string()));
    if (!title.empty()) {
      out.push_back(displayBlock(reps > 0 ? title + " (" +
                                                strutil::trim(fixedFmt(static_cast<double>(reps), 26, 0, true)) +
                                                ")"
                                          : title));
    }
    if (!hasOpt(parsed.opts, "nodots"))
      out.push_back(displayBlock("----+--- 1 ---+--- 2 ---+--- 3 ---+--- 4 ---+--- 5 "));
    return out;
  }
  if (i.empty()) return displayBlock("");  // envp2 dots_bare: a newline
  const std::optional<double> iv = parseNumber(i);
  if (!iv.has_value()) return envError(i + " not found", 111);  // envp2 dots_abc
  auto modOf = [](double x, double y) { return x - y * std::floor(x / y); };
  const double per = 50.0 * static_cast<double>(dots);
  if (rc.empty()) {
    // envp2 dots_noarg (`_dots 3`: a newline) and dots_50noarg (`_dots 50`: none).
    if (modOf(*iv, per) != 0) return displayBlock("");
    return ojson::array();
  }
  if (modOf(*iv, static_cast<double>(dots)) != 0) return ojson::array();
  std::string mark;
  if (rc == "0" || rc == "text" || rc == "result" || rc == "input") {
    mark = ".";
  } else if (rc == "-1") {
    mark = "s";
  } else if (rc == "1" || rc == "error") {
    mark = "x";
  } else if (rc == "2") {
    mark = "e";
  } else if (rc == "3") {
    mark = "n";
  } else {
    mark = "?";
  }
  if (modOf(*iv, per) == 0) {
    // envp2 dots_50: `.    50` and the line ends. maxdots() sizes the field
    // to its own comma-grouped width (_dots's CommaFormat).
    int width = 5;
    if (haveMaxdots)
      width = static_cast<int>(strutil::trim(fixedFmt(static_cast<double>(maxdots), 26, 0, true)).size());
    return displayBlock(mark + " " + fixedFmt(*iv, width, 0, true));
  }
  ojson b = displayBlock(mark);
  b["continue"] = true;
  return b;
}

// ===========================================================================
// postutil
// ===========================================================================
//
// Implements `postutil clear | dir` per post's help file; identified against
// StataBE probes (w2ax_t1_official.log postutil_*; envp1 pu_*). `clear` CLOSES
// every open postfile -- envp1 pu_file/pu_desc: the file is on disk afterwards
// holding the two posted rows -- so it is runPostclose for each handle, never a
// discard. `dir` prints the open handle names, one per line. Only the full
// words: envp1 `postutil d` and `postutil cl` are both r(198), as is any other
// word or a trailing argument.
nlohmann::ordered_json runPostutil(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabPostutil.known,
                                            kVocabPostutil.unimplemented))
    return ojson(*err);
  const std::vector<std::string> toks = splitWs(parsed.argText);
  if (toks.size() == 1 && toks[0] == "clear") {
    std::vector<std::string> handles;
    for (const auto& kv : session.postfiles) handles.push_back(kv.first);
    for (const auto& h : handles) {
      ojson r = runPostclose(h, session);
      if (const ojson* e = firstError(r)) return *e;
    }
    return ojson::array();
  }
  if (toks.size() == 1 && toks[0] == "dir") {
    // Stata lists the most recently opened first (envp1 pu_dir2: hb then ha);
    // the store does not record opening order, so the names come in its own
    // (name) order. Rc and content agree; only a multi-handle order can differ.
    std::vector<std::string> names;
    for (const auto& kv : session.postfiles) names.push_back(kv.first);
    if (names.empty()) return ojson::array();
    return displayBlock(joinWith(names, "\n"));
  }
  if (toks.size() == 1 && toks[0] == "d")
    return envError("nothing found where name expected", 198);
  // KNOWN (review item 9's rc table): every other shape is the generic r(198).
  return envError("invalid syntax", 198);
}

// ===========================================================================
// markout
// ===========================================================================
//
// Implements `markout markvar [varlist] [, strok]` per mark's help file;
// identified against StataBE probes (w2ax_t1_official.log markout_prog,
// markout_strok, markout_outside, markout_str_outside; envp2 mo_*). It works
// inside AND outside a program (markout_outside rc 0): unlike marksample it
// reads no `syntax` local, only the variables it is given. Rules 5-6 of the
// help: markvar is set to 0 where a numeric variable is missing; a string
// variable sets it to 0 in EVERY observation unless `strok`, under which ""
// counts as missing. The markvar must be an existing byte variable (envp2
// mo_strmark `s not byte`, mo_nonbyte `m2 not byte`: r(198), nothing changed).
// The write is a dispatched `quietly replace`, runMarksample's own shape.
nlohmann::ordered_json runMarkout(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabMarkout.known,
                                            kVocabMarkout.unimplemented))
    return ojson(*err);
  if (parsed.ifExpr.has_value() || parsed.inRange.has_value() || parsed.weightType.has_value())
    return envError("markout takes no if, in or weight qualifier", 101);
  if (parsed.varlist.empty()) return envError("varlist required", 100);
  if (!session.hasData()) return ojson(eval::noDataLoaded());
  Datastore* ds = nullptr;
  try {
    ds = session.datastore();
  } catch (const std::exception& e) {
    return ojson(eval::engineError(std::string("markout: ") + e.what(), "data_io"));
  }
  if (ds == nullptr) return ojson(eval::noDataLoaded());

  // A unique prefix stands for its variable, the markvar and the varlist alike
  // (review k1 K1 `markout t xv` for xvar, K4 `markout tma xvar` for tmark:
  // rc 0) -- resolved by the by-key resolver every varlist-taking verb shares
  // (handlers::resolveByKeyNames: wildcards and ranges expand, a unique prefix
  // binds, several matches are r(111) "ambiguous abbreviation").
  std::vector<std::string> markv{parsed.varlist[0]};
  if (markv[0].find_first_of("*?-") != std::string::npos)
    return envError(markv[0] + " invalid name", 198);
  if (auto err = resolveByKeyNames(session, markv)) return *err;
  const std::string markvar = markv[0];
  if (!ds->has(markvar)) return envError("variable " + markvar + " not found", 111);
  {
    bool isByte = ds->dtype(markvar) == "f64" && session.implicitFloatVars.count(markvar) == 0;
    auto it = session.varMeta.find(markvar);
    if (isByte && it != session.varMeta.end() && it->second.stataType.has_value() &&
        !it->second.stataType->empty())
      isByte = *it->second.stataType == "byte";
    if (!isByte) return envError(markvar + " not byte", 198);
  }
  if (parsed.varlist.size() == 1) return ojson::array();  // envp2 mo_novarlist: rc 0

  std::vector<std::string> rest(parsed.varlist.begin() + 1, parsed.varlist.end());
  for (const auto& v : rest) {
    // A time-series operator needs the lag machinery `replace` would have to
    // evaluate per variable here; refused by name rather than read as a name.
    if (v.find('.') != std::string::npos)
      return ojson(eval::refusedCommand("markout " + v, "ts_operator_varlist"));
  }
  if (auto err = resolveByKeyNames(session, rest)) return *err;  // K5 `*v`, K12 `xvar-lv`
  const bool strok = hasOpt(parsed.opts, "strok");
  bool zeroAll = false;
  std::vector<std::string> terms;
  for (const auto& v : rest) {
    if (!ds->has(v)) return envError("variable " + v + " not found", 111);
    if (ds->dtype(v) == "f64") {
      terms.push_back("missing(" + v + ")");
    } else if (strok) {
      terms.push_back(v + " == \"\"");
    } else {
      zeroAll = true;  // rule 6 (markout_prog, markout_str_outside: count 0)
    }
  }
  std::string cmd = "quietly replace " + markvar + " = 0";
  if (!zeroAll) {
    if (terms.empty()) return ojson::array();
    cmd += " if " + joinWith(terms, " | ");
  }
  ojson blocks = dispatch(cmd, session);
  if (const ojson* e = firstError(blocks)) return *e;
  return ojson::array();
}

// ===========================================================================
// lrtest
// ===========================================================================
//
// Implements the likelihood-ratio test per lrtest's help file and its ado;
// identified against StataBE probes (w2ax_t1_official.log lrtest_regress,
// lrtest_reverse, lrtest_df, lrtest_logit, lrtest_current, lrtest_mixed,
// lrtest_mixed_force, lrtest_missing, lrtest_diffN; envp3 lr_*).
//
// THE RULE. Each model contributes e(N), e(ll), e(ll_0), e(cmd), e(depvar) and
// its parameter count e(rank). The first name is taken as unrestricted; when
// its rank is the smaller one the two swap (not under `asis`), so the order of
// the names does not matter (lrtest_reverse). df = rank_u - rank_r, or df(#);
// chi2 = 2*(ll_u - ll_r), p = upper chi2(df) tail. Under `force` neither the
// df checks nor the sample/estimator checks run: lrtest_mixed_force is regress
// (rank 4) against logit (rank 2) -> df 2, chi2 = 2*(ll_regress - ll_logit) =
// -222.62..., p 1. Without it: df < 0 or = 0, N different (lrtest_diffN
// `observations differ: 45 vs. 80`), e(cmd) different (lrtest_mixed `test
// involves different estimators: regress vs. logit`), e(depvar) different, or
// e(ll_0) different beyond 1e-6 are r(498); a robust/cluster/pweight/pseudo-
// likelihood fit is r(498) too. One name means "against the current results",
// which are named by their stored name when they are a stored model
// (lrtest_current `t1l1 nested within t1l2`; envp3 lr_self: naming that model
// as well is r(198) "specified more than once") and `.` otherwise.
//
// THE p-VALUE. No upper chi2 tail is linked into PIET (no chi2tail() in the
// expression layer; the engine's gammq lives in src/estimators/posthest.cpp,
// which piet_core does not compile, and qreg2's copy is file-local). For an
// integer df the tail has an exact finite form in std::exp/std::erfc --
// Q(m, z) = e^-z sum_{j<m} z^j/j! and Q(m+1/2, z) = erfc(sqrt z) +
// sum_{j<m} z^(j+1/2) e^-z / Gamma(j+3/2), z = chi2/2 -- and that is what is
// used here; a fractional df() (2.5) is refused by name (the general tail is
// the shared helper this file does not have), while an integer-valued one in
// any spelling (2.0, 1e300) is computed. df 0 has a missing p (envp3
// lr_eqdf_force), a negative chi2 p 1.
namespace {

// `df` is a positive integer-valued double (df(#) may be as large as 1e300,
// review l3 H1). The terms are those of a Poisson(z) pmf (shifted by 1/2 when
// df is odd): they rise to the mode at j ~ z and then fall geometrically, so
// the series stops once a term past the mode no longer moves the sum -- the
// remaining terms are below 1e-17 of it -- and a df far above chi2 costs
// O(z) terms, not O(df) (Stata answers df(1e300) at once: p = 1.0000).
double chi2UpperTailIntegerDf(double df, double x) {
  if (!(df > 0) || !std::isfinite(df)) return std::numeric_limits<double>::quiet_NaN();
  if (!(x > 0)) return 1.0;
  const double z = x / 2.0;
  const bool odd = std::fmod(df, 2.0) != 0.0;
  const double m = std::floor(df / 2.0);
  const double shift = odd ? 0.5 : 0.0;
  double sum = odd ? std::erfc(std::sqrt(z)) : 0.0;
  auto settled = [&](double j, double term) { return j > z && term <= sum * 1e-17; };
  if (z < 700.0) {
    // Forward recurrence in the term ratio z/(j+a): every term positive, no
    // cancellation.
    const double kPi = 3.14159265358979323846;
    double term = odd ? 2.0 * std::sqrt(z / kPi) * std::exp(-z) : std::exp(-z);
    for (double j = 0; j < m; j += 1) {
      if (j > 0) term *= z / (j + shift);
      sum += term;
      if (settled(j, term)) break;
    }
  } else {
    // Far tail: each term in log space so e^-z cannot underflow on its own.
    const double lz = std::log(z);
    for (double j = 0; j < m; j += 1) {
      const double a = j + shift;
      const double term = std::exp(a * lz - z - std::lgamma(a + 1.0));
      sum += term;
      if (settled(j, term)) break;
    }
  }
  return std::min(1.0, sum);
}

// Stata's abbrev(s, n) for a name: first n-2 characters, `~`, last character.
std::string abbrevName(const std::string& s, size_t n) {
  if (s.size() <= n || n < 3) return s;
  return s.substr(0, n - 2) + "~" + s.back();
}

struct LrModel {
  std::string name;  // as displayed: a stored name, or "."
  double N = std::numeric_limits<double>::quiet_NaN();
  double rank = 0;
  double ll = std::numeric_limits<double>::quiet_NaN();
  double ll0 = std::numeric_limits<double>::quiet_NaN();
  std::string cmd, depvar;
};

// e(NAME) as a number, through the macro layer's own e() resolver -- the one
// `display e(rank)` and `local k = e(ll)` read (it maps Stata's names onto the
// estimation result's keys: rank -> k_vars, N -> n_obs, ...; a posted e-scalar
// otherwise). NaN for an absent or missing value. Never a second key table.
double eScalarOf(const eval::EvalSession& ev, const std::string& name) {
  std::string t = strutil::trim(eval::resolveEstScalarRefs("e(" + name + ")", ev, nullptr));
  // A negative value comes back parenthesised, "(-153.53...)", so it stays one
  // operand inside the expression it was substituted into.
  if (t.size() >= 2 && t.front() == '(' && t.back() == ')') t = strutil::trim(t.substr(1, t.size() - 2));
  if (t.empty() || t[0] == '"' || t[0] == '.') {
    // "." / ".a" are missing; a leading-dot NUMBER (".5") is not what the
    // resolver writes (it substitutes pythonFloatRepr text, "0.5").
    if (t.size() > 1 && t[0] == '.' && std::isdigit(static_cast<unsigned char>(t[1]))) {
      if (auto v = parseNumber(t)) return *v;
    }
    return std::numeric_limits<double>::quiet_NaN();
  }
  return parseNumber(t).value_or(std::numeric_limits<double>::quiet_NaN());
}

// lrtest's GetInfo over one model's e(). `ev` is the live e() (the current
// results) or a resolver session holding a stored model's e() stores.
// Returns an error block, or nullopt with `m` filled; `note` collects the
// rank-0 note.
std::optional<ojson> lrModelInfo(const std::string& name, const eval::EvalSession& ev, bool force,
                                 LrModel& m, std::string& note) {
  m.name = name;
  const eval::OrderedDict& estr = ev.eStrings;
  const std::string cmd = estr.get("cmd");
  if (cmd == "vec" || cmd == "mswitch" || !estr.get("mecmd").empty())
    return ojson(eval::refusedCommand("lrtest after " + (cmd.empty() ? estr.get("mecmd") : cmd),
                                      "model_family_not_wired"));
  if (!force) {
    const std::string vcetype = estr.get("vcetype");
    const bool isRegFamily = cmd == "regress" || cmd == "areg" || cmd == "xtreg";
    if (vcetype == "Robust") return envError("LR test likely invalid for models with robust VCE", 498);
    if (!estr.get("clustvar").empty())
      return envError("LR test likely invalid with cluster(" + estr.get("clustvar") + ")", 498);
    if (vcetype == "HAC" && isRegFamily)
      return envError("LR test likely invalid for models with HAC VCEs", 498);
    if (vcetype == "DK robust" && isRegFamily)
      return envError("LR test likely invalid for models with Driscoll-Kraay VCEs", 498);
    if (estr.get("wtype") == "pweight")
      return envError("LR test invalid in case of pweighted estimators", 498);
    if (estr.get("crittype") == "log pseudolikelihood")
      return envError("LR test likely invalid with log pseudolikelihoods", 498);
  }
  const double rank = eScalarOf(ev, "rank");
  if (std::isfinite(rank)) {
    m.rank = rank;
  } else {
    const eval::EvalMatrix* V = nullptr;
    for (const auto& [k, mat] : ev.eMatrices)
      if (k == "V") V = &mat;
    if (V == nullptr) {
      note += "note: model " + name + " does not contain matrix e(V); rank = 0 assumed.\n";
      m.rank = 0;
    } else {
      // rowsof(V) - diag0cnt(syminv(V)): syminv zeroes the rows of a singular
      // (omitted) parameter, whose variance is the zero on V's diagonal.
      double k = 0;
      for (size_t i = 0; i < V->data.size(); ++i)
        if (i < V->data[i].size() && V->data[i][i] != 0 && std::isfinite(V->data[i][i])) k += 1;
      m.rank = k;
    }
  }
  const double ll = eScalarOf(ev, "ll");
  if (!std::isfinite(ll)) {
    const bool present = ev.eScalars.find("ll") != nullptr ||
                         (ev.lastModelResult.has_value() && ev.lastModelResult->is_object() &&
                          ev.lastModelResult->contains("ll"));
    return envError(present ? "model " + name + " has missing e(ll)"
                            : "model " + name + " does not contain scalar e(ll)",
                    498);
  }
  m.ll = ll;
  m.N = eScalarOf(ev, "N");
  m.ll0 = eScalarOf(ev, "ll_0");
  m.cmd = cmd;
  m.depvar = estr.get("depvar");
  return std::nullopt;
}

// One specification token of `lrtest`: a name, or a parenthesised name.
// Returns false (with `composite` set) for a parenthesised LIST, a wildcard or
// `_all` -- the namelist's composite forms, which this file does not take.
bool lrSpecTokens(const std::string& text, std::vector<std::string>& toks, bool& composite) {
  size_t i = 0;
  while (i < text.size()) {
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
    if (i >= text.size()) break;
    if (text[i] == '(') {
      const size_t close = text.find(')', i);
      if (close == std::string::npos) return false;
      const std::vector<std::string> inner = splitWs(text.substr(i + 1, close - i - 1));
      if (inner.size() != 1) {
        composite = true;
        return false;
      }
      toks.push_back(inner[0]);
      i = close + 1;
      continue;
    }
    size_t j = i;
    while (j < text.size() && text[j] != ' ' && text[j] != '\t') ++j;
    toks.push_back(text.substr(i, j - i));
    i = j;
  }
  for (const auto& t : toks)
    if (t == "_all" || t.find_first_of("*?~") != std::string::npos) composite = true;
  return !composite;
}

}  // namespace

nlohmann::ordered_json runLrtest(const eval::ParsedCommand& parsed, SessionState& session) {
  // lrtest is an rclass program: whatever it ends with, the previous r() is
  // gone -- a failed lrtest leaves r() empty (envp3: `return list` after the
  // r(198) of `lrtest r*` lists nothing). Cleared first, on every path.
  eval::replaceRClass(session.eval);
  // ...and it posts nothing until the test is computed, so an absent r() read
  // afterwards is Stata's `.`, never the r-class layer's named r(111) refusal
  // (review l4: `capture lrtest m2 nosuch` then `local pv = r(p)` is `.`).
  eval::declareRPosted(session.eval, "lrtest", {});
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabLrtest.known,
                                            kVocabLrtest.unimplemented))
    return ojson(*err);
  const std::string curCmd = session.eval.eStrings.get("cmd");
  if (session.eval.eStrings.get("prefix") == "svy")
    return envError("lrtest is not appropriate with survey estimation results", 322);
  if (curCmd == "cfprobit") return envError("lrtest is not supported after cfprobit", 322);

  std::vector<std::string> toks;
  bool composite = false;
  if (!lrSpecTokens(parsed.argText, toks, composite)) {
    if (composite)
      return ojson(eval::refusedCommand("lrtest (composite model list)", "composite_models"));
    return envError("invalid syntax", 198);
  }
  if (toks.empty())
    // envp3 lr_none: the pre-8 syntax, whose unrestricted model is the one an
    // old `lrtest, saving(0)` stored -- which this build never stores.
    return envError("In the old syntax, the unrestricted model defaulted to a model saved under "
                    "the name 0. This model was not found.",
                    198);
  std::string name1 = toks[0];
  std::string name2;
  if (toks.size() >= 2) {
    name2 = toks[1];
  } else {
    if (curCmd.empty()) return envError("last estimates not found\nsecond specification expected", 301);
    name2 = ".";
  }
  if (toks.size() > 2) {
    std::vector<std::string> extra(toks.begin() + 2, toks.end());
    return envError("invalid input following specifications: " + joinWith(extra, " "), 198);
  }
  // `.` names the current results -- by their stored name when the
  // live e() IS a stored model (lrtest_current); a stored name must exist
  // (lrtest_missing: `estimation result t1nosuch not found`, r(111)).
  const std::string activeName = session.activeStoredName();
  auto expand = [&](std::string& n) -> std::optional<ojson> {
    if (n == ".") {
      if (curCmd.empty()) return envError("last estimates not found", 301);
      if (!activeName.empty()) n = activeName;
      return std::nullopt;
    }
    const auto it = session.estimatesModels.find(n);
    if (it == session.estimatesModels.end() || !it->second) {
      // A name the eststo snapshot list knows but with no stored e() behind it
      // is a store this build cannot read back, not a missing name.
      if (session.estimatesGet(n) != nullptr)
        return ojson(eval::refusedCommand("lrtest " + n, "stored_results_without_e"));
      return envError("estimation result " + n + " not found", 111);
    }
    return std::nullopt;
  };
  if (auto e = expand(name1)) return *e;
  if (auto e = expand(name2)) return *e;
  if (name1 == name2) return envError("models " + name1 + " specified more than once", 198);

  const bool force = hasOpt(parsed.opts, "force");
  const bool asis = hasOpt(parsed.opts, "asis");
  std::optional<double> dfOpt;
  std::string dfNote;
  std::string out;
  if (auto s = optStr(parsed.opts, "df")) {
    const std::string typed = strutil::trim(*s);
    const std::optional<double> v = parseNumber(typed);
    if (!v.has_value()) return envError("'" + typed + "' found where number expected", 7);
    if (!(*v > 0)) return envError("option df() should be positive", 198);  // envp3 lr_df0
    if (std::floor(*v) != *v)
      return ojson(eval::refusedCommand("lrtest, df(" + typed + ")", "noninteger_df"));
    // lrtest's `confirm integer number` is LEXICAL: only [+-]digits passes
    // (envp5: 2, +3, 05 and 2147483648 pass; 2.0, 1e3, 1e300 do not). A df()
    // spelled otherwise is computed with the note (review l3 H1 `df(1e300)`).
    size_t k = (typed[0] == '+' || typed[0] == '-') ? 1 : 0;
    bool lexInt = k < typed.size();
    for (; k < typed.size(); ++k)
      if (!std::isdigit(static_cast<unsigned char>(typed[k]))) lexInt = false;
    if (!lexInt) dfNote = "note: specified degrees of freedom (" + typed + ") is not an integer.\n";
    dfOpt = *v;
  }

  auto info = [&](const std::string& n, LrModel& m) -> std::optional<ojson> {
    if (n == ".") return lrModelInfo(n, session.eval, force, m, out);
    // A stored model's e(), read through a resolver session that holds only
    // that model's e() stores (handlers_ml.cpp's local EvalSession precedent):
    // nothing about the live session is touched, and no `estimates restore`
    // side effect (e(sample) install, the active name) happens.
    const auto& sm = *session.estimatesModels.at(n);
    eval::EvalSession probe;
    probe.lastModelResult = sm.lastModelResult;
    probe.eScalars = sm.eScalars;
    probe.eStrings = sm.eStrings;
    probe.eMatrices = sm.eMatrices;
    return lrModelInfo(n, probe, force, m, out);
  };
  LrModel u, r;
  if (auto e = info(name1, u)) return *e;
  if (auto e = info(name2, r)) return *e;
  if (u.rank < r.rank && !asis) std::swap(u, r);

  double df = u.rank - r.rank;
  if (dfOpt.has_value()) df = *dfOpt;
  if (!force) {
    if (df < 0)
      return envError("df(unrestricted) < df(restricted): " + shownNumber(u.rank) + " < " +
                          shownNumber(r.rank),
                      498);  // envp3 lr_asis_rev
    if (df == 0)
      return envError("df(unrestricted) = df(restricted) = " + shownNumber(u.rank), 498);  // lr_eqdf
    if (u.N != r.N)
      return envError("observations differ: " + shownNumber(u.N) + " vs. " + shownNumber(r.N), 498);
    if (u.cmd != r.cmd)
      return envError("test involves different estimators: " + u.cmd + " vs. " + r.cmd, 498);
    if (!u.depvar.empty() && !r.depvar.empty() && u.depvar != r.depvar)
      return envError("dependent variables differ: " + u.depvar + " vs. " + r.depvar, 498);
    if (std::isfinite(u.ll0) && std::isfinite(r.ll0)) {
      const double reldif = std::fabs(u.ll0 - r.ll0) / (std::fabs(r.ll0) + 1.0);
      // KNOWN, cosmetic: Stata's %9.0g here; fmtStataDefaultG is the %9.0g
      // renderer, but its spacing is not pinned against a captured message.
      if (reldif > 1e-6)
        return envError("log likelihood of null models differ:" + fmtStataDefaultG(u.ll0) + " vs. " +
                            fmtStataDefaultG(r.ll0),
                        498);
    }
  }

  const double chi2 = -2.0 * (r.ll - u.ll);
  // The series costs about chi2/2 + 40*sqrt(chi2/2) terms (or df/2, if less);
  // a statistic so large that this would not finish at once is refused by
  // name rather than left to spin.
  {
    const double z = chi2 / 2.0;
    const double work = std::min(df / 2.0, z > 0 ? z + 40.0 * std::sqrt(z) + 50.0 : 0.0);
    if (work > 1e8)
      return ojson(eval::refusedCommand("lrtest (chi2 = " + shownNumber(chi2) + ")",
                                        "chi2_tail_out_of_range"));
  }
  const double p = chi2UpperTailIntegerDf(df, chi2);
  eval::replaceRClass(session.eval);
  session.eval.rValues.set("df", df);
  session.eval.rValues.set("chi2", chi2);
  session.eval.rValues.set("p", p);
  eval::declareRPosted(session.eval, "lrtest", {"df", "chi2", "p"});
  // lrtest.ado's "double save for backward compatibility": $S_3 = df, $S_6 =
  // chi2, $S_7 = p (review l1 L2 `|2|5.465765009417908|.0650315654465017|`),
  // each stored by the `global NAME = exp` path so the number is written as
  // Stata writes a macro's value.
  for (const auto& [g, v] : {std::pair<const char*, double>{"S_3", df}, {"S_6", chi2}, {"S_7", p}}) {
    const std::string num = std::isnan(v) ? std::string(".") : eval::pythonFloatRepr(v);
    eval::MacroFnError mfe;
    eval::tryCaptureDefinition(std::string("global ") + g + " = " + num, session.eval, &mfe);
  }

  // The display: lrtest's own layout (oracle lrtest_* log lines, envp3 lr_df25).
  out = dfNote + out;
  const std::string dichi2 = strutil::trim(fixedFmt(chi2, 10, 2, false));
  const std::string dip = std::isnan(p) ? "." : strutil::trim(fixedFmt(p, 9, 4, false));
  const std::string dilr = "LR chi2(" + shownNumber(df) + ")";
  const std::string diprob = "Prob > chi2";
  const size_t w1 = std::max(dilr.size(), diprob.size());
  const size_t w2 = std::max(dichi2.size(), dip.size());
  auto ralign = [](const std::string& s, size_t w) {
    return s.size() >= w ? s : std::string(w - s.size(), ' ') + s;
  };
  const std::string rshow = r.name == "." ? "." : abbrevName(r.name, 14);
  const std::string ushow = u.name == "." ? "." : abbrevName(u.name, 14);
  out += "\nLikelihood-ratio test\nAssumption: " + rshow + " nested within " + ushow + "\n\n" +
         ralign(dilr, w1) + " = " + ralign(dichi2, w2) + "\n" + ralign(diprob, w1) + " = " +
         ralign(dip, w2);
  return displayBlock(out);
}

}  // namespace piet::handlers
