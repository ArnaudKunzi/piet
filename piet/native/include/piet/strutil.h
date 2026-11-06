// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Shared ASCII string helpers (lower-case fold, " \t"-only trim) — hoisted
// out of the ~9 near-identical anonymous-namespace copies scattered across
// the native TUs (A5-04): dispatcher.cpp's lower()/trimWs(), handlers_m3.cpp's
// lower()/trim(), handlers.cpp's lower(), handlers_data_verbs.cpp's
// lower()/trim(), egen.cpp's lower()/trim(), postfile.cpp's lower()/trim(),
// loops.cpp's lower()/trim(), program.cpp's lower()/trim(), doexec.cpp's
// lower()/trim(), estimation_m4.cpp's lowerM4()/trimM4(), and
// eval/tokenizer.cpp's toLower(). Each of those TU-local copies has internal
// linkage (wrapped in its own anonymous namespace), so there was never an
// ODR violation at link time — just duplicated logic to keep in sync by
// hand. This header is the single source of truth going forward.
//
// TUs in this workstream's manifest (dispatcher.cpp, handlers_m3.cpp,
// eval/tokenizer.cpp) now forward their locally-named helper to these
// implementations, kept as thin one-line wrappers rather than removed
// outright — that way none of their many existing call sites needed
// touching (pure dedup, no behavior change). TUs outside the manifest
// (handlers.cpp, handlers_data_verbs.cpp, egen.cpp, postfile.cpp, loops.cpp,
// program.cpp, doexec.cpp, estimation_m4.cpp) still carry their own copy —
// left alone here and reported for a future sweep.
//
// NOT the same thing as eval::tokenizer.cpp's strip()/lstrip()/rstrip()
// family, which deliberately implements Python str.strip()'s wider
// whitespace set (space/tab/newline/CR/FF/VT/BOM-adjacent) for tokenizer
// parity — that trio is a different contract and is left untouched here.
#ifndef PIET_NATIVE_STRUTIL_H
#define PIET_NATIVE_STRUTIL_H

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <optional>
#include <regex>
#include <string>
#include <vector>

#include "core/ident.h"  // the shared Unicode name rule (engine + PIET)

namespace piet::strutil {

// ASCII-only case fold (std::tolower per byte) — matches every one of the
// duplicated `lower()`/`toLower()` copies this consolidates.
inline std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
  return s;
}

// Strips leading/trailing ' '/'\t' only (NOT full Python-style whitespace —
// see eval::tokenizer.cpp's strip() for that wider variant) — matches every
// one of the duplicated `trim()`/`trimWs()` copies this consolidates.
inline std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t");
  size_t b = s.find_last_not_of(" \t");
  if (a == std::string::npos) return "";
  return s.substr(a, b - a + 1);
}

// BUG C (W3 fuzzer stabilization, kitchensink-fuzz fail category
// SILENT_SWALLOW): a new-variable name (gen's LHS, rename's target, ...)
// must be a valid Stata identifier — letters/digits/underscore, NOT
// starting with a digit, at most 32 characters, non-empty. Before this, the
// rule was enforced ONLY on strmutate.cpp's string-gen fallback path (its
// own `identRe()`, which didn't even check the length cap); the far more
// common numeric `gen`/`replace` path shipped straight to `cobra eval
// --gen` with no check at all, so `gen 9zzz = 1` silently created a column
// named "9zzz". This is now the ONE shared validator — every "new variable
// name" call site (gen numeric, gen/replace string-eval, rename) calls this
// same function so the rule can never drift between them again.
//
// Unicode (2026-10-28): the rule is cecon::ident::isValidName (src/core/ident.h),
// shared with the engine -- at most 32 CHARACTERS, first an ASCII letter, `_` or a
// Unicode letter, later characters ASCII alphanumerics, `_` or any non-ASCII
// character (StataBE 19.5 probes, scratch-r150/uni-oracle). Every name class that
// calls this (variables, value labels, scalars, matrices, estimates ...) follows it.
inline bool isValidStataIdentifier(const std::string& name) {
  return cecon::ident::isValidName(name);
}

// Shared human-readable explanation of the rule above — every "invalid
// variable name" error message quotes this SAME sentence (not a
// per-call-site paraphrase) so the wording can't drift either.
inline std::string stataIdentifierRuleText() {
  return "Stata variable names must start with a letter (ASCII or Unicode) or underscore, contain "
         "only letters, digits, underscores and other non-ASCII characters, and be at most 32 "
         "characters long.";
}

// BUG-1120: the reserved names ([U] 11.3) that a well-formed identifier may still
// not be as a new variable. Every command that creates a variable answers these
// eight with "NAME invalid varname", r(198) (StataBE 19.5). The rest of [U] 11.3 is
// per command: newVarNameRefusal and generateNameRefusal below are the two rules
// most commands follow. A few (tostring, destring, xtile, pctile, predict,
// tssmooth, ...) reach the other words through their own syntax, and Stata answers
// them with that syntax's own refusal; those refuse only the eight here. svmat
// keeps its own list (handlers_aer_results.cpp).
inline bool isReservedVarName(const std::string& name) {
  static const char* const kReserved[] = {"_b",  "_coef", "_cons", "_n",
                                          "_N",  "_pi",   "_pred", "_rc"};
  for (const char* r : kReserved)
    if (name == r) return true;
  return false;
}

// A refused new-variable name: Stata's sentence and return code.
struct NameRefusal {
  int rc = 198;
  std::string message;
};

// The digits of a name spelled `str` + digits ("" for any other name): the str#
// storage type, or a name that only looks like one.
inline std::string strTypeDigits(const std::string& name) {
  if (name.size() < 4 || name.compare(0, 3, "str") != 0) return std::string();
  const std::string d = name.substr(3);
  return d.find_first_not_of("0123456789") == std::string::npos ? d : std::string();
}

// A storage type as generate reads one: byte, int, long, float, double, strL, and
// str# for # from 1 to 2045, leading zeros allowed (str01). Case matters: strl,
// Byte and str0 are names (StataBE 19.5).
inline bool isStorageTypeWord(const std::string& name) {
  static const char* const kTypes[] = {"byte", "int", "long", "float", "double", "strL"};
  for (const char* t : kTypes)
    if (name == t) return true;
  const std::string d = strTypeDigits(name);
  if (d.empty() || d.size() > 4) return false;
  const int width = std::atoi(d.c_str());
  return width >= 1 && width <= 2045;
}

// _se and the r(table) row names generate, expand, merge, encode and decode refuse
// as well ("NAME invalid varname", r(198)); _r_ci, _r_cri, _r_crlb and _r_crub pass
// there, and every one of them is an ordinary name to egen, rename, clonevar, ...
// (StataBE 19.5).
inline bool isResultRowName(const std::string& name) {
  static const char* const kRows[] = {"_se",  "_r_b",  "_r_se", "_r_z", "_r_z_abs",
                                      "_r_p", "_r_lb", "_r_ub", "_r_df"};
  for (const char* r : kRows)
    if (name == r) return true;
  return false;
}

// Stata's new-variable name rule, as egen, rename, encode, decode, cumul, expand,
// merge, ipolate, kdensity, lowess and lpoly apply it to the name they create: the
// eight above, the storage types byte, int, long, float, double and strL and the
// words if, in, using and with are "NAME invalid varname", and a str# spelling with
// no leading zero (str5, str2046) "NAME invalid variable name", all r(198) (StataBE
// 19.5). `resultRows` adds isResultRowName's names, for the built-ins.
inline std::optional<NameRefusal> newVarNameRefusal(const std::string& name,
                                                    bool resultRows = false) {
  static const char* const kWords[] = {"byte", "int", "long", "float", "double",
                                       "strL", "if",  "in",   "using", "with"};
  bool reserved = isReservedVarName(name) || (resultRows && isResultRowName(name));
  for (const char* w : kWords) reserved = reserved || name == w;
  if (reserved) return NameRefusal{198, name + " invalid varname"};
  const std::string d = strTypeDigits(name);
  if (!d.empty() && d[0] != '0') return NameRefusal{198, name + " invalid variable name"};
  return std::nullopt;
}

// The same rule where the name is read out of an option as a newvarlist
// (splitsample's and twoway__histogram_gen's generate()): there if, in and using
// open a qualifier the option does not allow ("if not allowed", "in range not
// allowed", "using not allowed", r(101)) and with is "invalid 'with'", r(198).
inline std::optional<NameRefusal> newVarOptionRefusal(const std::string& name) {
  if (name == "if") return NameRefusal{101, "if not allowed"};
  if (name == "in") return NameRefusal{101, "in range not allowed"};
  if (name == "using") return NameRefusal{101, "using not allowed"};
  if (name == "with") return NameRefusal{198, "invalid 'with'"};
  return newVarNameRefusal(name);
}

// generate's own reading of `generate [type] NAME = exp`, which clonevar and the
// generate() of duplicates tag go through too: a storage type where NAME stands is
// taken for the type and leaves no name, "too few variables specified" r(102); `in`
// opens an in-range, "'=' invalid observation number" r(198); using, _all and _skip
// are "NAME not allowed" r(101), with "=exp required" r(100) and alias "type alias
// not allowed" r(109); the eight are "NAME invalid varname" and a str# past str2045
// "NAME invalid variable name", r(198) (StataBE 19.5). `if` opens an if-condition
// that starts with `=`, r(198): Stata names the expression ("=1 invalid name"; for
// duplicates tag the temporary variable it generates from), so the sentence here
// names the word instead. `resultRows` adds isResultRowName's names, for generate.
inline std::optional<NameRefusal> generateNameRefusal(const std::string& name,
                                                      bool resultRows = false) {
  if (isStorageTypeWord(name)) return NameRefusal{102, "too few variables specified"};
  if (name == "in") return NameRefusal{198, "'=' invalid observation number"};
  if (name == "if") return NameRefusal{198, "if invalid name"};
  if (name == "using" || name == "_all" || name == "_skip")
    return NameRefusal{101, name + " not allowed"};
  if (name == "with") return NameRefusal{100, "=exp required"};
  if (name == "alias") return NameRefusal{109, "type alias not allowed"};
  if (isReservedVarName(name) || (resultRows && isResultRowName(name)))
    return NameRefusal{198, name + " invalid varname"};
  const std::string d = strTypeDigits(name);
  if (!d.empty() && d[0] != '0') return NameRefusal{198, name + " invalid variable name"};
  return std::nullopt;
}

// A quoted argument the blank-splitting tokenizer cut into pieces (`"a b.tex"` ->
// `"a`, `b.tex"`), put back together from words[i] on; `i` is left on the last
// piece used. A word that is not an open quote comes back as is. Compound quotes
// (`"..."') are handled the same way. AER 007: `esttab ... using
// "$out/Working and ability ..._`c(current_date)'.tex"` wrote a file named `"../...Working.txt`.
inline std::string rejoinQuotedWord(const std::vector<std::string>& words, size_t& i) {
  if (i >= words.size()) return std::string();
  const std::string& w = words[i];
  const bool compound = w.size() >= 2 && w[0] == '`' && w[1] == '"';
  if (!(compound || (!w.empty() && w[0] == '"'))) return w;
  auto closed = [&](const std::string& t, bool first) {
    if (compound) return t.size() >= (first ? 4u : 2u) && t.compare(t.size() - 2, 2, "\"'") == 0;
    return t.size() >= (first ? 2u : 1u) && t.back() == '"';
  };
  if (closed(w, true)) return w;
  std::string out = w;
  for (size_t j = i + 1; j < words.size(); ++j) {
    out += ' ';
    out += words[j];
    if (closed(words[j], false)) {
      i = j;
      return out;
    }
  }
  return w;  // never closed: leave the first piece, as before
}

// ---- factor-level value-label substitution (BUG-102) -----------------------
// A factor-variable coefficient row's engine name is `<level>.<var>` (BUG-101,
// model.cpp:526), or `<level>.<var>#<level>.<var>` for interactions; real Stata
// substitutes the attached value label for the bare level when displaying such a
// row (verified live, StataBE 19.5). PIET's flat one-row-per-coefficient table
// keeps the ".varname" suffix — there is no shared group header the way Stata's
// ASCII table has one — so only the level segment is substituted.
//
// WHY IT LIVES HERE. This was TWO byte-identical hand-written copies —
// handlers.cpp's `substituteFactorLevelLabel` (the M2/regress slice) and
// estimation_m4.cpp's `substituteFactorLevelLabelM4` — differing in ONE comment
// line and nothing else. The copy's own note said no shared header linked the
// two anonymous namespaces; the answer was to make one. Two copies of a display
// RULE that must agree is the twin class this codebase has already been bitten
// by (BUG-195's formatter twins, and BUG-112(8), where landing a fix in only the
// M4a copy left `regress` — the most common estimator — still broken).
//
// It takes the label TABLE rather than a SessionState precisely so it can live
// in this dependency-light header: the rule needs `varname -> {code: text}` and
// nothing else about a session.
inline std::string substituteFactorLevelLabel(
    const std::string& name,
    const std::map<std::string, std::map<int, std::string>>& valueLabels) {
  static const std::regex segRe(R"(^(-?\d+)\.(\w+)$)");
  if (valueLabels.empty()) return name;
  std::vector<std::string> segs;
  size_t p = 0;
  while (true) {
    size_t q = name.find('#', p);
    segs.push_back(name.substr(p, q == std::string::npos ? std::string::npos : q - p));
    if (q == std::string::npos) break;
    p = q + 1;
  }
  bool changed = false;
  for (auto& seg : segs) {
    std::smatch m;
    if (!std::regex_match(seg, m, segRe)) continue;
    auto vlIt = valueLabels.find(m[2].str());
    if (vlIt == valueLabels.end()) continue;
    // `m[1]` is regex-guaranteed `-?\d+`, so the conversion cannot fail on a
    // shape this branch accepts (clang-tidy's unchecked-conversion warning here
    // is answered by the guard, not by the call).
    auto codeIt = vlIt->second.find(std::atoi(m[1].str().c_str()));
    if (codeIt == vlIt->second.end()) continue;
    seg = codeIt->second + "." + m[2].str();
    changed = true;
  }
  if (!changed) return name;
  std::string out;
  for (size_t i = 0; i < segs.size(); ++i) {
    if (i) out += "#";
    out += segs[i];
  }
  return out;
}

// A minimal glob: `*` (any run, including empty) and `?` (one character).
// Deliberately NOT <fnmatch.h>: that header does not exist on the Windows target
// this codebase cross-compiles for, and a pattern matcher is small enough that
// adding a platform dependency for it would cost more than it saves.
//
// This is Stata's `match()` / `strmatch()` for the surfaces that need it as a
// utility. It moved here from handlers_aer_util.cpp's anonymous namespace,
// byte-identical, when handlers_aer_tables.cpp needed the SAME matcher for
// esttab/estout `keep()`/`drop()`/`order()` and `transform()`'s coefficient
// patterns -- a second copy in a sibling translation unit is how two surfaces
// end up disagreeing about what `*` means. `loops.cpp` and `estimation_m4c.cpp`
// still carry their own (BUG-496 records the count); they are not callers here.
//
// `?` is ONE CHARACTER, not one byte (StataBE: `ds ق?` lists the two-letter
// Arabic names), and a `*` backtrack resumes on a character boundary.
inline bool globMatch(const std::string& pat, const std::string& s) {
  size_t p = 0, i = 0, star = std::string::npos, mark = 0;
  while (i < s.size()) {
    if (p < pat.size() && pat[p] == '?') {
      ++p;
      i += cecon::ident::utf8SeqLen(static_cast<unsigned char>(s[i]));
      if (i > s.size()) i = s.size();
    } else if (p < pat.size() && pat[p] == s[i]) {
      ++p;
      ++i;
    } else if (p < pat.size() && pat[p] == '*') {
      star = p++;
      mark = i;
    } else if (star != std::string::npos) {
      p = star + 1;
      mark += cecon::ident::utf8SeqLen(static_cast<unsigned char>(s[mark]));
      if (mark > s.size()) mark = s.size();
      i = mark;
    } else {
      return false;
    }
  }
  while (p < pat.size() && pat[p] == '*') ++p;
  return p == pat.size();
}

}  // namespace piet::strutil

#endif  // PIET_NATIVE_STRUTIL_H
