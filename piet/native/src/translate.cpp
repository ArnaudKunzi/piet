// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/translate.h"

#include "data/ts_operators.h"  // BUG-370: cecon::is_ts_prefix — the engine's own
                               // TS-vs-factor predicate, shared not re-derived

#include <cctype>
#include <cmath>
#include <cstdio>

#include "core/ident.h"  // the shared Unicode name rule (scanner byte tests)

namespace piet::translate {

namespace {

// BS-030: format session.delta for the `--tsdelta` CLI arg. Most real-world
// deltas are small integers (delta(2), delta(7) for daily-recorded-weekly,
// ...) — print those plainly rather than via a generic float formatter (no
// new shared dependency pulled in just for this one call site, matching
// this codebase's own established per-TU convention for small local
// helpers). A non-integer delta still round-trips fine through cobra's own
// `std::stod` parse either way.
std::string formatDelta(double d) {
  if (d == std::floor(d)) {
    return std::to_string(static_cast<long long>(d));
  }
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.10g", d);
  return std::string(buf);
}

// One [LFDS][0-9]* run, e.g. "L", "L2", "S12". Returns chars consumed, or 0
// if the token doesn't start with an L/F/D/S letter at `pos`.
size_t matchLetterDigitsRun(const std::string& s, size_t pos) {
  if (pos >= s.size()) return 0;
  char c = static_cast<char>(std::toupper(static_cast<unsigned char>(s[pos])));
  if (c != 'L' && c != 'F' && c != 'D' && c != 'S') return 0;
  size_t i = pos + 1;
  while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
  return i - pos;
}

// (\([0-9/ ]+\))? — an optional parenthesised numlist, e.g. "(1/3)", "(1 2)".
// Returns chars consumed (0 if absent or malformed).
size_t matchParenNumlist(const std::string& s, size_t pos) {
  if (pos >= s.size() || s[pos] != '(') return 0;
  size_t i = pos + 1;
  size_t start = i;
  while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '/' || s[i] == ' ')) {
    ++i;
  }
  if (i == start) return 0;  // \([0-9/ ]+\) requires at least one char inside
  if (i >= s.size() || s[i] != ')') return 0;
  return (i + 1) - pos;  // include the closing ')'
}

}  // namespace

bool isTsToken(const std::string& token) {
  // Alternative 1: ^(?:[LFDS][0-9]*)+(?:\([0-9/ ]+\))?\.
  // One-or-more consecutive [LFDS][0-9]* runs, then an optional paren
  // numlist, then a literal '.'.
  {
    size_t i = 0;
    size_t runs = 0;
    while (true) {
      size_t consumed = matchLetterDigitsRun(token, i);
      if (consumed == 0) break;
      i += consumed;
      ++runs;
    }
    if (runs > 0) {
      size_t j = i;
      size_t paren = matchParenNumlist(token, j);
      j += paren;  // optional, 0 if absent
      if (j < token.size() && token[j] == '.') return true;
    }
  }
  // Alternative 2: \A[LFDS]\([0-9/ ]+\)\.  (single letter, no digit run,
  // directly followed by a paren numlist then '.')
  {
    if (!token.empty()) {
      char c0 = static_cast<char>(std::toupper(static_cast<unsigned char>(token[0])));
      if (c0 == 'L' || c0 == 'F' || c0 == 'D' || c0 == 'S') {
        size_t paren = matchParenNumlist(token, 1);
        if (paren > 0) {
          size_t j = 1 + paren;
          if (j < token.size() && token[j] == '.') return true;
        }
      }
    }
  }
  return false;
}

bool hasTsToken(const std::vector<std::string>& tokens) {
  for (const auto& t : tokens) {
    if (isTsToken(t)) return true;
  }
  return false;
}

// BUG-369: does an EXPRESSION (not a varlist token) carry a time-series operator?
// `gen ly = L.y` and `keep if L.x > 3` are ordinary Stata, and the engine resolves
// them now (frame.cpp's parser), but only if the session's tsset/xtset metadata
// travels with the call — this is what decides whether to send it.
//
// Scans for the ONE form cobra's expression parser accepts: `<LFDS-run>.<ident>`.
// The numlist variants isTsToken also matches (`L(1/3).x`) are varlist-only —
// parse_tsop has no paren grammar — so they are deliberately not looked for here.
// Quote-aware: a "L.x" inside a string literal is text, not an operator.
std::vector<TsTokenRef> extractTsTokens(const std::string& expr) {
  std::vector<TsTokenRef> out;
  bool inStr = false;
  for (size_t i = 0; i < expr.size(); ++i) {
    if (expr[i] == '"') {
      inStr = !inStr;
      continue;
    }
    if (inStr) continue;
    // core/ident.h: a byte >= 0x80 is a name byte, so `L.ملاحظات` is one run and
    // `مL.x` is the tail of a name, not an operator chain.
    if (!cecon::ident::isIdentStartByte(expr[i])) continue;
    if (i > 0) {
      char prev = expr[i - 1];
      // Mid-identifier (`totalL.x`) or right after a dot (the `y` of `L.y`) is
      // never the START of an operator chain.
      if (cecon::ident::isIdentByte(prev) || prev == '.') continue;
    }
    // Consume exactly what frame.cpp's parser consumes: the maximal [alnum_.] run.
    // That is what makes `L2.D.gdp` come back as ONE token rather than stopping at
    // the first dot, and it is why the prefix test below can be the engine's own.
    size_t j = i;
    while (j < expr.size() && (cecon::ident::isIdentByte(expr[j]) || expr[j] == '.')) ++j;
    const std::string run = expr.substr(i, j - i);
    const size_t dot = run.rfind('.');
    if (dot != std::string::npos) {
      const std::string pre = run.substr(0, dot);
      const std::string base = run.substr(dot + 1);
      // cecon::is_ts_prefix is the single source frame.cpp's Parser::primary uses.
      // Note it is only half the engine's test — the engine ALSO requires the base
      // column to exist before taking the TS reading. This scanner cannot see the
      // data, so it is deliberately the looser half: a false positive costs a
      // wasted materialise attempt whose honest error surfaces, while a false
      // negative would silently leave an operator for a parser that cannot read it.
      if (!base.empty() && cecon::is_ts_prefix(pre)) {
        TsTokenRef ref;
        ref.token = run;
        ref.begin = i;
        ref.end = j;
        out.push_back(std::move(ref));
      }
    }
    i = j - 1;  // resume after the run; the loop's ++i lands on its terminator
  }
  return out;
}

bool exprHasTsToken(const std::string& expr) { return !extractTsTokens(expr).empty(); }

// B3(c): the time variable's DISPLAY FORMAT, when the session knows one.
// tin(2010q1, 2011q4) cannot be read without it -- the same literal means a
// different instant on a %tq series and a %td one -- and the .hbin header
// carries no formats (datastore.h says so in its first paragraph), so the engine
// cannot look it up. It rides the call the way tsset itself does.
std::string tsFormatFor(const SessionState& session) {
  if (!session.timevar.has_value() || session.timevar->empty()) return "";
  auto it = session.varMeta.find(*session.timevar);
  if (it == session.varMeta.end() || !it->second.format.has_value()) return "";
  return *it->second.format;
}


// B3(c): tin()/twithin() are the second reason an expression needs the tsset
// metadata to ride the call. They are not time-series OPERATORS -- no L./F./D./S.
// token appears -- but the engine cannot expand them without knowing which
// variable is the time index and what scale its format declares, and it says so
// honestly ("tin() requires tsset") rather than guessing. Word-bounded and
// quote-aware for the same reason exprHasRunningSum is.
bool exprHasTinToken(const std::string& expr) {
  char quote = 0;
  for (size_t i = 0; i < expr.size(); ++i) {
    const char c = expr[i];
    if (quote) { if (c == quote) quote = 0; continue; }
    if (c == '"' || c == '\'') { quote = c; continue; }
    if (c != 't') continue;
    size_t len = 0;
    if (expr.compare(i, 3, "tin") == 0) len = 3;
    else if (expr.compare(i, 7, "twithin") == 0) len = 7;
    else continue;
    if (i > 0) {
      const char p = expr[i - 1];
      if (cecon::ident::isIdentByte(p) || p == '.') continue;  // core/ident.h
    }
    size_t j = i + len;
    while (j < expr.size() && (expr[j] == ' ' || expr[j] == '\t')) ++j;
    if (j < expr.size() && expr[j] == '(') return true;
  }
  return false;
}

// B3(c): draw coordinates. Appended only when the expression actually asks for a
// draw, so no other call's argv changes -- and the counter only advances when a
// draw really happens, which is what keeps a `set seed` run reproducible.
bool exprHasDrawToken(const std::string& expr) {
  char quote = 0;
  for (size_t i = 0; i < expr.size(); ++i) {
    const char c = expr[i];
    if (quote) { if (c == quote) quote = 0; continue; }
    if (c == '"' || c == '\'') { quote = c; continue; }
    size_t len = 0;
    if (expr.compare(i, 11, "runiformint") == 0) len = 11;
    else if (expr.compare(i, 8, "runiform") == 0) len = 8;
    else if (expr.compare(i, 7, "rnormal") == 0) len = 7;
    else if (expr.compare(i, 7, "uniform") == 0) len = 7;
    else continue;
    if (i > 0) {
      const char p = expr[i - 1];
      if (cecon::ident::isIdentByte(p) || p == '.') continue;  // core/ident.h
    }
    size_t j = i + len;
    while (j < expr.size() && (expr[j] == ' ' || expr[j] == '\t')) ++j;
    if (j < expr.size() && expr[j] == '(') return true;
  }
  return false;
}

std::vector<std::string> injectRngContextForExpr(const std::vector<std::string>& argv,
                                                 const std::string& expr,
                                                 SessionState& session) {
  if (!exprHasDrawToken(expr)) return argv;
  std::vector<std::string> out = argv;
  out.push_back("--rng-mt64");
  out.push_back(session.seed.stream().state_string());
  ++session.rngStream;
  return out;
}

void absorbRngDraws(const nlohmann::json& engineResponse, SessionState& session) {
  if (!engineResponse.is_object()) return;
  auto it = engineResponse.find("rng_draws");
  if (it == engineResponse.end() || !it->is_number_integer()) return;
  const long long n = it->get<long long>();
  if (n > 0) session.seed.stream().discard(static_cast<uint64_t>(n));
}

void carryStoreOverflow(const nlohmann::json& engineResponse, nlohmann::ordered_json& block) {
  if (!engineResponse.is_object() || !block.is_object() ||
      block.value("type", std::string()) != "data_mutation")
    return;
  auto it = engineResponse.find("store_overflow");
  if (it != engineResponse.end() && it->is_number_integer() && it->get<long long>() > 0)
    block["store_overflow"] = it->get<long long>();
}

std::vector<std::string> injectTsContextForExpr(const std::vector<std::string>& argv,
                                                const std::string& expr,
                                                const SessionState& session) {
  const bool tin = exprHasTinToken(expr);
  if (!exprHasTsToken(expr) && !tin) return argv;
  std::vector<std::string> out = injectTsContext(argv, session);
  // The format goes ONLY on the tin()/twithin() path. injectTsContext is SHARED
  // with the estimator calls, and `--tsfmt` is declared on `eval` alone, so
  // appending it there made every `regress y x1 L.x2` fail on an undeclared
  // option. The freeze harness then hid that: when a primary-corpus fixture
  // stops early it silently falls through to the aux corpus, so the RED it
  // reported was a fixture-identity diff on `encode` and said nothing about the
  // regress line that had actually broken.
  if (tin) {
    const std::string f = tsFormatFor(session);
    if (!f.empty()) { out.push_back("--tsfmt"); out.push_back(f); }
  }
  return out;
}

std::vector<std::string> injectTsContext(const std::vector<std::string>& argv,
                                          const SessionState& session) {
  if (session.panelvar.has_value() && !session.panelvar->empty()) {
    std::vector<std::string> out = argv;
    std::string val = *session.panelvar;
    if (session.timevar.has_value() && !session.timevar->empty()) {
      val += "," + *session.timevar;
    }
    out.push_back("--xtset");
    out.push_back(val);
    // BS-030: forward the user's delta() so cobra's L./F./D./S. operator
    // resolution steps by that many index units (apps/cmds/common.cpp's
    // parse_tsmeta reads this same --tsdelta flag) instead of silently
    // defaulting to 1 — previously delta() was accepted by the tsset/xtset
    // dispatch arms and then dropped entirely, never reaching the engine.
    if (session.delta.has_value()) {
      out.push_back("--tsdelta");
      out.push_back(formatDelta(*session.delta));
    }
    return out;
  }
  if (session.timevar.has_value() && !session.timevar->empty()) {
    std::vector<std::string> out = argv;
    out.push_back("--tsset");
    out.push_back(*session.timevar);
    if (session.delta.has_value()) {
      out.push_back("--tsdelta");
      out.push_back(formatDelta(*session.delta));
    }
    return out;
  }
  return argv;
}

}  // namespace piet::translate
