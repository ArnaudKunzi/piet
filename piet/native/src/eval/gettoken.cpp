// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/eval/gettoken.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <regex>

#include "piet/eval/errors.h"
#include "piet/eval/macros.h"
#include "piet/eval/tokenizer.h"

namespace piet::eval {

namespace {

std::string toLowerCopy(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
  return s;
}

// `gettoken TOK REST : SRC [, opts]`. Each name is an emname of [P] gettoken:
// `name`, `(local) name` or `(global) name` (StataBE 19.5, x2.do D34); a local's
// name may start with a digit -- the manual's own `gettoken subcmd 0 : 0`
// (NEW-gettoken-digit-name.do: `gettoken t 1 : 1` gives p and ` q`).
const std::string& emnamePattern() {
  static const std::string p = R"((?:\(\s*(local|global)\s*\)\s*)?([A-Za-z0-9_]+))";
  return p;
}
const std::regex& reGettoken() {
  static const std::regex r(std::string(R"(^gettoken\s+)") + emnamePattern() + R"(\s+)" +
                                emnamePattern() + R"(\s*:\s*)" + emnamePattern() + "(.*)$",
                            std::regex::icase);
  return r;
}

// BS-033 (BETA campaign C3): the 2-argument destructure form — `gettoken
// TOKNAME : SRCNAME [, opts]` (no RESTNAME). This is a real, documented
// Stata idiom ("peek" the first token without consuming it) that the
// 3-arg-only regex above simply couldn't match at all (syntax_error on
// every use). Real Stata semantics (live-verified this stint, StataBE):
// unlike the 3-arg form, SRCNAME's own macro is left COMPLETELY UNCHANGED
// — this is a peek, not a pop (oracle: `local line "alpha beta gamma"` +
// `gettoken tok : line` -> tok=[alpha], line=[alpha beta gamma], still the
// full original string).
const std::regex& reGettoken2Arg() {
  static const std::regex r(std::string(R"(^gettoken\s+)") + emnamePattern() + R"(\s*:\s*)" +
                                emnamePattern() + "(.*)$",
                            std::regex::icase);
  return r;
}

std::string strip(const std::string& s) {
  size_t a = 0, b = s.size();
  auto isSp = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; };
  while (a < b && isSp(s[a])) ++a;
  while (b > a && isSp(s[b - 1])) --b;
  return s.substr(a, b - a);
}

// One past the quoted span opening at `k` ("..." or a nesting `"..."'), or
// npos when it never closes.
size_t quotedSpanEnd(const std::string& s, size_t k) {
  if (s[k] == '"') {
    const size_t e = s.find('"', k + 1);
    return e == std::string::npos ? std::string::npos : e + 1;
  }
  return compoundSpanEnd(s, k);
}

// BUG-1499: gettoken's options, [P] gettoken: p:arse("pchars") q:uotes
// qed(lmacname) m:atch(lmacname) bind. Measured in StataBE 19.5
// (scratch-laneA2/agents/prs/x2.do, x5.do).
struct GettokenOpts {
  std::string pchars = " ";  // parse(): the parsing characters, a blank by default
  bool quotes = false;       // keep the token's outer quotes, as `"..."'
  std::string qed;           // local set to 1/0: was the token quoted
  std::string match;         // local set to "(" when a (...) token was matched
  bool bind = false;         // (...) and [...] inside a token do not split it
};

std::optional<nlohmann::json> syntaxError(const std::string& msg) {
  return errorBlock("syntax_error", msg, std::nullopt, std::nullopt, std::nullopt, "r(198)");
}

// The option list after the source name. Anything but a `, options` tail is
// r(198); so is an unknown or repeated option (`option nosuchopt not allowed`,
// `option qe() not allowed`, `option parse() not allowed`) and an option
// without its argument (`option parse incorrectly specified`).
std::optional<nlohmann::json> parseGettokenOpts(const std::string& raw, GettokenOpts& o) {
  std::string s = strip(raw);
  if (s.empty()) return std::nullopt;
  if (s[0] != ',') return syntaxError("invalid syntax");
  size_t i = 1;
  const size_t n = s.size();
  std::vector<std::string> seen;
  while (i < n) {
    while (i < n && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    if (i >= n) break;
    const size_t w0 = i;
    while (i < n && std::isalnum(static_cast<unsigned char>(s[i]))) ++i;
    const std::string word = toLowerCopy(s.substr(w0, i - w0));
    if (word.empty()) return syntaxError("invalid syntax");
    std::optional<std::string> arg;
    if (i < n && s[i] == '(') {
      // The argument runs to the matching `)`; quoted spans are text.
      int depth = 0;
      size_t j = i;
      while (j < n) {
        if (s[j] == '"' || startsCompound(s, j)) {
          const size_t e = quotedSpanEnd(s, j);
          j = e == std::string::npos ? n : e;
          continue;
        }
        if (s[j] == '(') ++depth;
        if (s[j] == ')' && --depth == 0) break;
        ++j;
      }
      if (j >= n) return syntaxError("option " + word + " incorrectly specified");
      arg = s.substr(i + 1, j - i - 1);
      i = j + 1;
    }
    auto abbrevOf = [&](const char* full, size_t minLen) {
      const std::string f = full;
      return word.size() >= minLen && word.size() <= f.size() && f.compare(0, word.size(), word) == 0;
    };
    // qed and bind are not abbreviated (`qe()`, `b`, `bi` are not allowed).
    const std::string canon = word == "qed"                   ? "qed"
                              : abbrevOf("parse", 1)          ? "parse"
                              : abbrevOf("quotes", 1) && !arg ? "quotes"
                              : abbrevOf("match", 1)          ? "match"
                              : word == "bind" && !arg        ? "bind"
                                                              : "";
    if (canon.empty() || std::find(seen.begin(), seen.end(), canon) != seen.end())
      return syntaxError("option " + word + (arg ? "()" : "") + " not allowed");
    seen.push_back(canon);
    if (canon == "quotes") {
      o.quotes = true;
    } else if (canon == "bind") {
      o.bind = true;
    } else if (!arg || (canon != "parse" && strip(*arg).empty())) {
      return syntaxError("option " + word + " incorrectly specified");
    } else if (canon == "qed") {
      o.qed = strip(*arg);
    } else if (canon == "match") {
      o.match = strip(*arg);
    } else {
      std::string p = strip(*arg);
      if (startsCompound(p, 0) && compoundSpanEnd(p, 0) == p.size()) {
        p = p.substr(2, p.size() - 4);
      } else if (p.size() >= 2 && p.front() == '"' && p.back() == '"') {
        p = p.substr(1, p.size() - 2);
      }
      // parse("") is the default, a blank (x2.do D39).
      o.pchars = p.empty() ? std::string(" ") : p;
    }
  }
  return std::nullopt;
}

struct GettokenScan {
  std::string token;
  std::string rest;
  bool quoted = false;  // qed()
  bool paren = false;   // match(): a (...) token was matched
  bool unbalanced = false;
};

// [P] gettoken, as StataBE 19.5 parses (x2.do D1-D71): leading blanks are
// skipped whatever parse() holds ("  a,b" with parse(",") gives `a`); a token
// that opens with a quote is the quoted text, quotes stripped (kept as `"..."'
// with `quotes`), and the rest starts right after the closing quote; with
// match() a token that opens with `(` is the text inside its matching `)`; a
// token that opens with a parse character is that character (`==` when `=` is
// one); any other token runs to the next parse character -- a quoted span
// inside it is part of it (`a"b c"d`), and with bind so is a (...) or [...]
// group. The rest is everything after the token, its leading blanks kept
// (`alpha beta` -> ` beta`).
GettokenScan scanGettoken(const std::string& s, const GettokenOpts& o) {
  GettokenScan r;
  const size_t n = s.size();
  size_t i = 0;
  while (i < n && s[i] == ' ') ++i;
  if (i >= n) return r;
  auto isParse = [&](char c) { return o.pchars.find(c) != std::string::npos; };
  // A quoted token. An unterminated one runs to the end.
  if (s[i] == '"' || startsCompound(s, i)) {
    const size_t open = s[i] == '"' ? 1 : 2;
    const size_t e = quotedSpanEnd(s, i);
    const size_t end = e == std::string::npos ? n : e;
    const size_t close = e == std::string::npos ? 0 : open;
    const std::string body = s.substr(i + open, end - i - open - close);
    r.token = o.quotes ? "`\"" + body + "\"'" : body;
    r.rest = s.substr(end);
    r.quoted = true;
    return r;
  }
  // The index of the bracket closing the group opened at `k`, quote-aware and
  // counting both kinds; npos when it never closes.
  auto groupClose = [&](size_t k) -> size_t {
    int depth = 0;
    while (k < n) {
      if (s[k] == '"' || startsCompound(s, k)) {
        const size_t e = quotedSpanEnd(s, k);
        if (e == std::string::npos) return std::string::npos;
        k = e;
        continue;
      }
      if (s[k] == '(' || s[k] == '[') ++depth;
      if ((s[k] == ')' || s[k] == ']') && --depth == 0) return k;
      ++k;
    }
    return std::string::npos;
  };
  if (!o.match.empty() && s[i] == '(') {
    const size_t c = groupClose(i);
    if (c == std::string::npos) {  // `(a b c`: r(132) "parentheses unbalanced" (D13)
      r.unbalanced = true;
      return r;
    }
    r.token = s.substr(i + 1, c - i - 1);
    r.rest = s.substr(c + 1);
    r.paren = true;
    return r;
  }
  if (isParse(s[i])) {
    const size_t len = (s[i] == '=' && i + 1 < n && s[i + 1] == '=') ? 2 : 1;
    r.token = s.substr(i, len);
    r.rest = s.substr(i + len);
    return r;
  }
  size_t j = i;
  while (j < n && !isParse(s[j])) {
    if (s[j] == '"' || startsCompound(s, j)) {
      const size_t e = quotedSpanEnd(s, j);
      j = e == std::string::npos ? n : e;
      continue;
    }
    if (o.bind && (s[j] == '(' || s[j] == '[')) {
      const size_t c = groupClose(j);
      j = c == std::string::npos ? n : c + 1;
      continue;
    }
    ++j;
  }
  r.token = s.substr(i, j - i);
  r.rest = s.substr(j);
  return r;
}

}  // namespace

std::vector<nlohmann::json> runGettoken(const std::string& rawLine, LocalsStore& locals) {
  std::string stripped = strip(rawLine);
  // {kind, name} of TOK, REST (kind "-" when the 2-argument form has none), SRC.
  std::string kinds[3], names[3], optsRaw;
  std::smatch m;
  if (std::regex_match(stripped, m, reGettoken())) {
    for (int k = 0; k < 3; ++k) {
      kinds[k] = toLowerCopy(m[1 + 2 * k].str());
      names[k] = m[2 + 2 * k].str();
    }
    optsRaw = m[7].str();
  } else if (std::regex_match(stripped, m, reGettoken2Arg())) {
    // BS-033: 2-arg destructure form — see reGettoken2Arg()'s own comment.
    kinds[0] = toLowerCopy(m[1].str());
    names[0] = m[2].str();
    kinds[1] = "-";
    kinds[2] = toLowerCopy(m[3].str());
    names[2] = m[4].str();
    optsRaw = m[5].str();
  } else {
    return {engineError(
        "gettoken: syntax error — expected "
        "``gettoken TOKNAME RESTNAME : SRCNAME [, bind]``",
        "syntax_error")};
  }
  for (int k = 0; k < 3; ++k) {
    if (kinds[k] != "global") continue;
    // A global's name starts with a letter or `_`.
    if (!locals.getGlobal || !locals.setGlobal ||
        std::isdigit(static_cast<unsigned char>(names[k][0])))
      return {engineError("gettoken: (global) " + names[k] + " is not available here",
                          "syntax_error")};
  }
  auto getM = [&](int k) {
    return kinds[k] == "global" ? locals.getGlobal(names[k]) : locals.get(names[k]);
  };
  auto setM = [&](int k, const std::string& v) {
    if (kinds[k] == "global") locals.setGlobal(names[k], v);
    else locals.set(names[k], v);
  };
  GettokenOpts opts;
  if (auto e = parseGettokenOpts(optsRaw, opts)) return {*e};

  // Read SRC fully before writing any binding (handles aliasing: TOK == SRC).
  const std::string srcVal = getM(2);
  const GettokenScan r = scanGettoken(srcVal, opts);
  if (r.unbalanced) {
    // StataBE 19.5 (x2.do D13): the token's macro is left empty, r(132).
    setM(0, "");
    return {errorBlock("syntax_error", "parentheses unbalanced", std::nullopt, std::nullopt,
                       std::nullopt, "r(132)")};
  }
  setM(0, r.token);
  if (kinds[1] != "-") setM(1, r.rest);
  if (!opts.qed.empty()) locals.set(opts.qed, r.quoted ? "1" : "0");
  if (!opts.match.empty()) locals.set(opts.match, r.paren ? "(" : "");
  return {};
}

}  // namespace piet::eval
