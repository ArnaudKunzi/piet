// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// `sctorezone` (SSC package scto, SurveyCTO) -- shift %tc time variables by a
// fixed number of hours. Package-007 unblock (T7), lane L3. Registry, dispatcher
// arm, handlers.h declaration, effects/floors rows, NOTICE line and the pangram
// line landed with unit Z; this file is the lane's alone.
//
// NOT VERIFIED AGAINST A STATA RUN OF THE PACKAGE ITSELF. The package is not
// installed on the reference machine, so `sctorezone` was never run. This file
// implements the command per the public ado's behaviour (header `*!Version 2.0
// 23APR2020 SurveyCTO support@surveycto.com`, developed by William Blackmon,
// IPA); the behaviour was identified by reading that ado, and the Stata
// PRIMITIVES it leans on were then checked one by one against StataBE 19.5
// (scratch probes, not part of the package). Which is which:
//
//   READ FROM THE ADO (not run):
//     * the grammar `sctorezone shift [if] [in], force [Only(varlist) |
//       Exclude(varlist)]`; the order of the checks; the user-visible error
//       messages (all `error 1`, i.e. rc 1); the output wording; the rule that
//       only variables whose display format starts with `%tc` (case-sensitive,
//       so `%tC`, `%-tc`, `%td` are untouched) are shifted; that the shift is
//       applied by `replace v = v + shifttime*60*60*1000` with NO if-condition
//       (the `[if] [in]` the syntax line accepts are never read), so missings
//       stay missing and every observation moves.
//   CHECKED AGAINST StataBE (primitives, not the command):
//     * `syntax anything(...) [if] [in], force [Only(varlist) | Exclude(varlist)]`:
//       a missing shift is rc 100 ("time zone shift or starttime variable
//       required") and is reported BEFORE a missing `force` (rc 198, "option
//       force required" -- NOT rc 100); `only()` with nothing inside is the same
//       as no `only()`; `only` with no parentheses is rc 198; `only(z)` together
//       with `exclude(y)` is accepted by `syntax` (the ado's own check rejects
//       it); an unknown/ambiguous varlist item is rc 111; `o` and `e` are the
//       minimum abbreviations; `force` has none.
//     * `ds <list>, has(format %tc*)` returns the matching names IN THE ORDER
//       GIVEN, DUPLICATES INCLUDED (`only(b a b)` shifts b twice and lists it
//       twice); a `ds` with an empty list means every variable (so `exclude()`
//       removing everything shifts everything -- reproduced, it is the ado's
//       behaviour).
//     * `real("...")` (blanks trimmed; `1e1`, `9.`, `.5` accepted; hex, inf,
//       `1,5`, `5e` missing) and the FLOAT storage of the tempvar the ado
//       generates (`gen shifttime = real(...)` has no `double`): 5.3 is held as
//       5.30000019073486, so `5.3*3600000` is not what is added -- the float's
//       value is. A value beyond the float range is missing; a value below the
//       smallest float is 0.
//     * the report: `qui levelsof shifttime, loc(L)` followed by `qui count if
//       shifttime == `L'` -- levelsof prints a float to 17 characters, and the
//       equality only holds when that text parses back to the float, so
//       `+0.1` reports "- 0 observations shifted forwards by .1000000014901161
//       hour." while `+5.3` ("... by 5.300000190734863 hours.") and `+5.5` report
//       the real count. Reproduced (gstr17 below).
//       A shift of exactly 0 prints "- N observations not shifted.". Under `set
//       type double` the tempvar is a double and the same text round-trips
//       (`+0.1` reports ".1 hour" with the real count).
//     * the arithmetic: `replace v = v + 9*60*60*1000` on a FLOAT %tc variable
//       re-rounds to float (04mar2020 10:11:12 -> 19:10:52, 01jan1960 ->
//       09:00:00 exactly, .a -> .), on a DOUBLE it is exact. Not computed here:
//       the shift is re-dispatched as a `replace`, so runReplace's storage-type
//       rounding (piet/storage_type.h) is the one that answers.
//   NOT IMPLEMENTED, REFUSED BY NAME (eval::refusedCommand):
//     * AUTOMATIC mode (the shift is the name of a string variable holding the
//       SurveyCTO start time, shift computed per row from `starttime`).
//     * STRING TIME VARIABLES: a string variable that is always missing or
//       matches `*:??:?? ?M` ("12:34:56 PM") and has at least one value is
//       rewritten by the ado with an hours/minutes procedure. A dataset that
//       would take that branch is refused BEFORE anything is changed; string
//       variables that do not qualify are skipped silently, as the ado does.
//
// THE REPORT COUNT IS AN INFERENCE FROM PRIMITIVES, NOT AN OBSERVATION OF THE
// PACKAGE. The ado reports `qui count if shifttime == `sh'` where `sh` is the
// text `levelsof` made of a FLOAT tempvar (a double under `set type double`,
// which is honoured). StataBE shows that text does not always parse back to the
// float, so `+0.1`, `+11.11`, `+23.99` report "- 0 observations shifted ..."
// while every row HAS moved; `+5.5`, `+5.3` and `+9` report the real count (all
// of these were run against StataBE as primitives). That is what the
// ado's code does and it is reproduced, but it is the part of this file a run of
// the package could still overturn.
//
// KNOWN DIVERGENCES (stated, not hidden): an `in` qualifier is refused by the
// tokenizer's in-range allow-list (registry allowInRange=false; Stata accepts
// and ignores it), whereas an `if` is accepted and ignored, as in Stata;
// `only`/`exclude` are matched case-insensitively here (Stata rejects `ONLY`;
// `force` IS held to Stata's rule, see badForceSpelling); a range whose end is
// a variable abbreviation is refused (the shared expander does not resolve it);
// `r()` after the command carries only r(N) (the last `count`) in the shifted
// case and is otherwise untouched; the text of a shift below 1e-4 hours is
// reproduced from the observed pattern of StataBE's macro text, not from the
// ado; `sctorezone "+9", force` (a quoted shift) is not modelled -- StataBE's
// batch run hung on that line, so what Stata does with it was not observed.
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "piet/datastore.h"
#include "piet/dispatcher.h"  // replace, per variable (storage-type rounding)
#include "piet/eval/errors.h"
#include "piet/eval/tokenizer.h"
#include "piet/handlers.h"
#include "piet/loops.h"  // expandVarlist -- the ONE varlist wildcard/range expander
#include "piet/session_state.h"
#include "piet/storage_type.h"  // resolveCreateType -- the type `gen shifttime` gets
#include "piet/strutil.h"

namespace piet::handlers {

using ojson = nlohmann::ordered_json;

namespace {

// Namespace scope, so it constructs before the first dispatch (the winsor
// comment in handlers_aer_abbrev.cpp says why a function-local static is too
// late). `o`/`e` are the minimum abbreviations of Only()/Exclude() and a
// one-character key never prefix-resolves, so both are listed; `onl`/`excl`
// reach the canonical names through the vocabulary registry. `force` has no
// abbreviation in Stata. Every option here is read.
const eval::OptVocab& kVocabSctorezone = eval::registerOptVocab(
    {"sctorezone"}, {{"force", "only", "o", "exclude", "e"}, {}});

constexpr const char* kShiftForm =
    "Shift must either be a string variable or in the form '+3' or '-4'.";

// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------

// An error whose rc is KNOWN, stated through the `reason` channel so no message
// rule in coarseRc can turn it into another code.
ojson errorRc(const std::string& message, int rc) {
  return ojson(eval::errorBlock("engine_error", message, std::nullopt, std::nullopt, std::nullopt,
                                "r(" + std::to_string(rc) + ")"));
}

// The ado's `di as error ...` + `error 1`.
ojson adoError(const std::string& message) { return errorRc(message, 1); }

// A named refusal that also says, in the message itself, what is refused.
ojson refuse(const std::string& cause, const std::string& detail) {
  ojson b = ojson(eval::refusedCommand("sctorezone", cause));
  b["message"] = "Command `sctorezone` is not fully supported in this version of PIET: " + detail +
                 " This branch of sctorezone is not implemented; nothing was changed.";
  return b;
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

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------

const eval::OptValue* findOpt(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                              const std::string& key) {
  for (const auto& [k, v] : opts) {
    if (k == key) return &v;
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// The shift value
// ---------------------------------------------------------------------------

// Stata's real(s) for the text the ado hands it: leading/trailing blanks are
// ignored, an optional sign, digits with at most one point, an optional
// exponent. Hex, inf/nan, thousands separators, `5e` and the like are missing.
// The character filter is what keeps strtod from accepting the C spellings.
bool statRealOf(const std::string& sIn, double* out) {
  size_t b = 0, e = sIn.size();
  while (b < e && (sIn[b] == ' ' || sIn[b] == '\t')) ++b;
  while (e > b && (sIn[e - 1] == ' ' || sIn[e - 1] == '\t')) --e;
  const std::string s = sIn.substr(b, e - b);
  if (s.empty()) return false;
  bool digit = false;
  for (char c : s) {
    if (c >= '0' && c <= '9') {
      digit = true;
      continue;
    }
    if (c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') continue;
    return false;
  }
  if (!digit) return false;
  char* end = nullptr;
  const double v = std::strtod(s.c_str(), &end);
  if (end != s.c_str() + s.size()) return false;
  if (!std::isfinite(v)) return false;
  *out = v;
  return true;
}

// `gen shifttime = real(...)` declares no type, so the tempvar is a FLOAT: the
// value is rounded to float, one beyond the float range is missing, one below
// the smallest float is 0 (StataBE: real("1e39") missing; real("1e-40") 0).
// False = missing.
bool toStataFloat(double v, double* out) {
  const double av = std::fabs(v);
  if (av > 1.7014117331926443e38) return false;
  if (av < static_cast<double>(FLT_MIN)) {
    *out = 0.0;
    return true;
  }
  *out = static_cast<double>(static_cast<float>(v));
  return true;
}

// The text Stata gives a non-negative float held in a macro (`levelsof`, and
// `local x = abs(...)`, which agree for every magnitude a legal shift has):
// at most 17 characters, the leading zero of a fraction dropped, trailing zeros
// dropped; exponent form with 12 significant digits when the fixed form would
// carry fewer digits than that (StataBE: 5.300000190734863, .1000000014901161,
// .0099999997764826, 23.98999977111816, 9.99999974738e-06, 1.00000001169e-07).
std::string gstr17(double ax) {
  if (ax == 0.0) return "0";
  auto trimZeros = [](std::string s) {
    if (s.find('.') != std::string::npos) {
      while (!s.empty() && s.back() == '0') s.pop_back();
      if (!s.empty() && s.back() == '.') s.pop_back();
    }
    return s;
  };
  char buf[64];
  if (ax >= 1.0) {
    const int intDigits = ax >= 10.0 ? 2 : 1;
    std::snprintf(buf, sizeof(buf), "%.*f", 16 - intDigits, ax);
    return trimZeros(buf);
  }
  if (ax >= 1e-5) {  // at most four zeros after the point: 12+ significant digits fit
    std::snprintf(buf, sizeof(buf), "%.16f", ax);
    std::string s = trimZeros(buf);
    if (s.size() > 1 && s[0] == '0' && s[1] == '.') s.erase(0, 1);
    return s;
  }
  std::snprintf(buf, sizeof(buf), "%.11e", ax);
  std::string s = buf;
  const size_t epos = s.find('e');
  if (epos != std::string::npos) {
    std::string mant = trimZeros(s.substr(0, epos));
    s = mant + s.substr(epos);
  }
  return s;
}

// A finite double as an expression literal that parses back to the same value.
std::string exprLiteral(double v) {
  if (v == std::floor(v) && std::fabs(v) < 1e15) {
    char b[32];
    std::snprintf(b, sizeof(b), "%lld", static_cast<long long>(v));
    return b;
  }
  char b[40];
  std::snprintf(b, sizeof(b), "%.17g", v);
  return b;
}

// ---------------------------------------------------------------------------
// The variable card
// ---------------------------------------------------------------------------

struct VarEntry {
  std::string name;
  bool isString = false;
  std::string fmt;  // the display format as every other reader resolves it
};

// The dataset's variables in column order, with a name -> position index (the
// sweep form looks up every name; a linear scan per name is quadratic on a
// survey export with thousands of columns).
struct Card {
  std::vector<VarEntry> vars;
  std::unordered_map<std::string, size_t> index;
  const VarEntry* find(const std::string& name) const {
    auto it = index.find(name);
    return it == index.end() ? nullptr : &vars[it->second];
  }
};

// The resident card when it is current, else the datastore's own format card --
// the pair every card consumer in this codebase uses.
std::optional<ojson> loadCard(SessionState& session, Card* out) {
  auto fromCard = [&](const std::vector<VarInfo>& card) {
    for (const VarInfo& vi : card) {
      if (isHiddenColumn(vi.name)) continue;
      VarEntry e;
      e.name = vi.name;
      e.isString = (vi.dtype == "factor");
      e.fmt = e.isString ? std::string() : session.displayFormat(vi);
      out->index[e.name] = out->vars.size();
      out->vars.push_back(std::move(e));
    }
  };
  if (const std::vector<VarInfo>* card = session.residentReadCard()) {
    fromCard(*card);
    return std::nullopt;
  }
  try {
    Datastore* ds = session.datastore();
    if (ds == nullptr) return ojson(eval::noDataLoaded());
    fromCard(ds->formatCard(std::nullopt));
  } catch (const std::exception& e) {
    return ojson(eval::engineError(
        std::string("sctorezone: could not read the data -- ") + e.what(), "data_io"));
  }
  return std::nullopt;
}

// A bare name as a varlist reads it: exact, else the one variable it begins;
// several = "ambiguous abbreviation", none = "not found" (both rc 111).
std::optional<std::string> abbrevOf(const Card& card, const std::string& t, bool* ambiguous) {
  *ambiguous = false;
  if (card.find(t) != nullptr) return t;
  std::optional<std::string> hit;
  for (const VarEntry& v : card.vars) {
    if (v.name.size() > t.size() && v.name.compare(0, t.size(), t) == 0) {
      if (hit.has_value()) {
        *ambiguous = true;
        return std::nullopt;
      }
      hit = v.name;
    }
  }
  return hit;
}

// `Only(varlist)` / `Exclude(varlist)`: wildcards, ranges and _all through the
// shared expander, then each literal item must name a variable. Whatever is
// wrong with the list is rc 111 with Stata's "(error in option only())" tail --
// the expander's own error blocks carry no rc (a bad range end) or no tail (a
// wildcard that matches nothing), so they are restated here. Order and
// duplicates are kept -- ds does the same.
std::optional<ojson> expandVarlistOpt(SessionState& session, const Card& card,
                                      const std::string& spec, const std::string& optName,
                                      std::vector<std::string>* out) {
  const std::string tail = " (error in option " + optName + "())";
  auto vr = piet::loops::expandVarlist(spec, session);
  if (vr.isError) {
    std::string msg = vr.error.is_object() ? vr.error.value("message", std::string()) : std::string();
    if (msg.empty()) msg = "variable " + spec + " not found";
    return errorRc(msg + tail, 111);
  }
  for (const std::string& t : vr.tokens) {
    bool amb = false;
    std::optional<std::string> nm = abbrevOf(card, t, &amb);
    if (!nm.has_value())
      return errorRc(amb ? t + " ambiguous abbreviation" + tail : "variable " + t + " not found" + tail,
                     111);
    out->push_back(*nm);
  }
  return std::nullopt;
}

// The ado's test for a SurveyCTO string time variable: every value is missing or
// matches *:??:?? ?M, and at least one value is not missing. The glob runs once
// per distinct level; the row loop only reads codes.
bool isSurveyCtoStringTime(const cecon::Column& c, int64_t n) {
  if (c.dtype != cecon::DType::Factor) return false;
  const int32_t* codes = c.codes();
  std::vector<char> verdict(c.levels.size(), 0);  // 0 unknown, 1 matches, 2 does not
  bool any = false;
  for (int64_t i = 0; i < n; ++i) {
    const int32_t code = codes[i];
    if (code < 0 || static_cast<size_t>(code) >= c.levels.size()) continue;
    const size_t k = static_cast<size_t>(code);
    if (c.levels[k].empty()) continue;
    any = true;
    if (verdict[k] == 0) verdict[k] = strutil::globMatch("*:??:?? ?M", c.levels[k]) ? 1 : 2;
    if (verdict[k] == 2) return false;
  }
  return any;
}

std::string joinWords(const std::vector<std::string>& v) {
  std::string out;
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) out += ' ';
    out += v[i];
  }
  return out;
}

// `force` is declared in lower case with no capital stem, so Stata accepts it
// only spelled out in full and in lower case (StataBE: `, fo` is "option force
// required", `, FORCE` is "option FORCE not allowed", both rc 198). The option
// net below prefix-resolves and lower-cases, so the typed spelling is read off
// the raw text: the bare words after the top-level comma, parentheses skipped.
// Empty = the spelling is fine (or no force-like word at all).
std::string badForceSpelling(const std::string& raw, bool* wrongCase) {
  *wrongCase = false;
  const long long comma = eval::findUnquotedComma(raw);
  if (comma < 0) return std::string();
  const std::string tail = raw.substr(static_cast<size_t>(comma) + 1);
  int depth = 0;
  size_t i = 0;
  while (i < tail.size()) {
    const char c = tail[i];
    if (c == '(') {
      ++depth;
      ++i;
      continue;
    }
    if (c == ')') {
      if (depth > 0) --depth;
      ++i;
      continue;
    }
    const bool wordChar = std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
    if (depth > 0 || !wordChar) {
      ++i;
      continue;
    }
    size_t j = i;
    while (j < tail.size() &&
           (std::isalnum(static_cast<unsigned char>(tail[j])) != 0 || tail[j] == '_'))
      ++j;
    const std::string word = tail.substr(i, j - i);
    i = j;
    if (word == "force") continue;
    const std::string lw = strutil::lower(word);
    if (lw == "force") {
      *wrongCase = true;
      return word;
    }
    if (std::string("force").compare(0, lw.size(), lw) == 0) return word;
  }
  return std::string();
}

}  // namespace

ojson runSctorezone(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabSctorezone.known,
                                            kVocabSctorezone.unimplemented))
    return ojson(*err);

  // ---- syntax: the shift (rc 100) comes before `force` (rc 198) -----------
  const std::string shiftText = strutil::trim(parsed.argText);
  if (shiftText.empty())
    return errorRc("time zone shift or starttime variable required", 100);
  {
    bool wrongCase = false;
    const std::string typed = badForceSpelling(parsed.raw, &wrongCase);
    if (!typed.empty())
      return ojson(eval::engineError(
          wrongCase ? "option " + typed + " not allowed" : std::string("option force required"),
          "syntax_error"));
  }
  if (findOpt(parsed.opts, "force") == nullptr)
    return ojson(eval::engineError("option force required", "syntax_error"));

  // only()/exclude(): empty parentheses are the same as not given; no
  // parentheses at all is "incorrectly specified" (rc 198).
  auto optSpec = [&](const char* canon, const char* shortKey, std::string* spec) -> std::optional<ojson> {
    const eval::OptValue* ov = findOpt(parsed.opts, canon);
    if (ov == nullptr) ov = findOpt(parsed.opts, shortKey);
    if (ov == nullptr) return std::nullopt;
    if (ov->isBool)
      return ojson(eval::engineError(std::string("option ") + canon + " incorrectly specified",
                                     "syntax_error"));
    *spec = strutil::trim(ov->strValue);
    return std::nullopt;
  };
  std::string onlySpec, exclSpec;
  if (auto e = optSpec("only", "o", &onlySpec)) return *e;
  if (auto e = optSpec("exclude", "e", &exclSpec)) return *e;

  if (!session.hasData()) return ojson(eval::noDataLoaded());

  Card card;
  if (auto e = loadCard(session, &card)) return *e;
  if (card.vars.empty()) return errorRc("no variables defined", 111);

  // Only()/Exclude() are varlists: their errors (rc 111) surface in `syntax`,
  // ahead of every check the ado itself makes.
  std::vector<std::string> onlyVars, exclVars;
  if (!onlySpec.empty())
    if (auto e = expandVarlistOpt(session, card, onlySpec, "only", &onlyVars)) return *e;
  if (!exclSpec.empty())
    if (auto e = expandVarlistOpt(session, card, exclSpec, "exclude", &exclVars)) return *e;

  // ---- automatic or manual? `cap confirm string variable shift` --------------
  // rc 0 only when every word is a (possibly abbreviated) string variable; any
  // failure -- including an ambiguous abbreviation, whose rc 111 the ado's `cap`
  // swallows -- means manual, and a manual shift must begin with + or -.
  {
    std::vector<std::string> words;
    size_t i = 0;
    while (i < shiftText.size()) {
      while (i < shiftText.size() && (shiftText[i] == ' ' || shiftText[i] == '\t')) ++i;
      size_t j = i;
      while (j < shiftText.size() && shiftText[j] != ' ' && shiftText[j] != '\t') ++j;
      if (j > i) words.push_back(shiftText.substr(i, j - i));
      i = j;
    }
    bool allString = !words.empty();
    for (const std::string& w : words) {
      bool amb = false;
      std::optional<std::string> nm = abbrevOf(card, w, &amb);
      bool isStr = false;
      if (nm.has_value()) {
        if (const VarEntry* ve = card.find(*nm)) isStr = ve->isString;
      }
      if (!isStr) {
        allString = false;
        break;
      }
    }
    if (allString)
      return refuse("automatic_shift_string_variable",
                    "the shift `" + shiftText +
                        "' names a string variable (automatic mode: the shift is computed per "
                        "observation from the SurveyCTO starttime). Give a manual shift such as "
                        "+9 or -4.");
  }

  // ---- manual: +N / -N ------------------------------------------------------
  const char direction = shiftText[0];
  if (direction != '+' && direction != '-') return adoError(kShiftForm);

  const int64_t nObs = static_cast<int64_t>(session.macroObsCount());
  // `gen shifttime = ...` takes the session's default type: float, or double
  // after `set type double` (StataBE: the levelsof text of 5.3 is then `5.3`).
  const bool tempIsDouble = (storage::resolveCreateType(session, "") == "double");
  double shift = 0.0;  // the value the tempvar holds, sign applied
  // With no observations the generated tempvar is empty and the ado's
  // assert/count checks are vacuously true, so an unparsable or oversized shift
  // goes through unremarked (the report below then prints only its first line).
  if (nObs > 0) {
    double v = 0.0, f = 0.0;
    if (!statRealOf(shiftText.substr(1), &v)) return adoError(kShiftForm);
    if (tempIsDouble) {
      f = v;
    } else if (!toStataFloat(v, &f)) {
      return adoError(kShiftForm);
    }
    shift = (direction == '-') ? -f : f;
  }

  if (!onlySpec.empty() && !exclSpec.empty())
    return adoError("Use the \"only\" or \"exclude\" options (or neither), not both.");
  if (nObs > 0 && (shift <= -24.0 || shift >= 24.0))
    return adoError("Time zone shift must be in hours, between +/-24 (exclusive).");

  // ---- which variables ------------------------------------------------------
  std::vector<std::string> cand;
  if (!onlySpec.empty()) {
    cand = onlyVars;
  } else {
    for (const VarEntry& v : card.vars) cand.push_back(v.name);
    if (!exclSpec.empty()) {
      // subinstr(" all ", " v ", " ", 1) per excluded name: each name leaves once.
      for (const std::string& x : exclVars) {
        auto it = std::find(cand.begin(), cand.end(), x);
        if (it != cand.end()) cand.erase(it);
      }
      // `ds , has(...)` with an empty list means EVERY variable.
      if (cand.empty())
        for (const VarEntry& v : card.vars) cand.push_back(v.name);
    }
  }

  std::vector<std::string> dtvars;       // %tc variables, in the ado's order
  std::vector<std::string> stringTimes;  // would take the string-time branch
  Datastore* dsp = nullptr;
  for (const std::string& name : cand) {
    const VarEntry* ent = card.find(name);
    if (ent == nullptr) continue;
    if (ent->isString) {
      if (nObs == 0) continue;  // no value => never in tvars
      try {
        if (dsp == nullptr) dsp = session.datastore();
        if (dsp != nullptr && isSurveyCtoStringTime(dsp->col(name), dsp->nRows()) &&
            std::find(stringTimes.begin(), stringTimes.end(), name) == stringTimes.end())
          stringTimes.push_back(name);
      } catch (const std::exception& e) {
        return ojson(eval::engineError(
            std::string("sctorezone: could not read string variable ") + name + " -- " + e.what(),
            "data_io"));
      }
      continue;
    }
    // ds, has(format %tc*): case-sensitive prefix; %tC, %-tc, %td do not match.
    if (ent->fmt.compare(0, 3, "%tc") == 0) dtvars.push_back(name);
  }
  if (!stringTimes.empty())
    return refuse("string_time_variables",
                  "the variable(s) " + joinWords(stringTimes) +
                      " hold SurveyCTO string times (12:34:56 PM) that the package would rewrite "
                      "by hours and minutes. Restrict the command to the %tc variables with "
                      "only(varlist).");

  // ---- the shift itself: one dispatched `replace` per variable ---------------
  ojson mutation;  // the last data_mutation block a replace produced
  if (!dtvars.empty() && nObs > 0) {
    const std::string mult = "(" + exprLiteral(shift) + ")*60*60*1000";
    for (const std::string& v : dtvars) {
      // Not prefixed `quietly`: that drops the data_mutation block the front end
      // refreshes its cards from. The ado's `qui` is honoured by forwarding that
      // one block and discarding the "(N real changes made)" text.
      ojson blocks = dispatch("replace " + v + " = " + v + " + " + mult, session);
      if (const ojson* e = firstError(blocks)) {
        // Variables before this one are already shifted in place: hand the front
        // end the last card refresh it owes for them, then the error.
        ojson failed = ojson::array();
        if (!mutation.is_null()) failed.push_back(mutation);
        failed.push_back(*e);
        return failed;
      }
      if (blocks.is_array()) {
        for (const auto& b : blocks)
          if (b.is_object() && b.value("type", std::string()) == "data_mutation") mutation = b;
      } else if (blocks.is_object() && blocks.value("type", std::string()) == "data_mutation") {
        mutation = blocks;
      }
    }
  }

  // ---- the report -------------------------------------------------------------
  std::string text;
  if (dtvars.empty()) {
    text = "No variables have been shifted.\n";
  } else {
    // `di "...shifted: `tvars' `dtvars'"` with tvars empty: two blanks after the colon.
    text = "The following variable(s) have been shifted:  " + joinWords(dtvars) + "\n";
    if (nObs > 0) {
      const double ash = std::fabs(shift);
      const std::string ashText = gstr17(ash);
      // qui count if shifttime == `L' holds only when L's text parses back to the float.
      const bool roundTrips = std::strtod(ashText.c_str(), nullptr) == ash;
      const int64_t cnt = roundTrips ? nObs : 0;
      if (shift != 0.0) {
        const bool plural = std::strtod(ashText.c_str(), nullptr) > 1.0;
        text += "- " + std::to_string(cnt) + " observations shifted " +
                (shift > 0.0 ? "forwards" : "backwards") + " by " + ashText + " hour" +
                (plural ? "s" : "") + ".\n";
      } else {
        text += "- " + std::to_string(cnt) + " observations not shifted.\n";
      }
      session.eval.rValues.set("N", static_cast<double>(cnt));
    }
  }

  ojson out = ojson::array();
  ojson d;
  d["type"] = "display";
  d["text"] = text;
  out.push_back(std::move(d));
  if (!mutation.is_null()) out.push_back(std::move(mutation));
  return out;
}

}  // namespace piet::handlers
