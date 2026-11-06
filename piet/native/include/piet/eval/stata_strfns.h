// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Stata string functions that have no engine counterpart.
//
// AER scope 2, B3 stage (a). cobra's expression grammar is numeric-only --
// src/data/frame.cpp throws on a string literal and its only result sink takes
// vector<double> -- so the whole string family lives in PIET, the way substr,
// strpos and subinstr already do. That is architecture, not an omission, and it
// is why these are NOT in src/data/stata_dates.h beside the shared calendar.
//
// Every rule below is MEASURED against StataNow 19.5 BE, not recalled;
// tests/validate/b3_funcs_oracle.json carries the captures. Two of them are
// counter-intuitive enough to be worth stating up front:
//
//   * "blank" means SPACE, and nothing else. stritrim/strltrim/strrtrim do not
//     touch a tab or a newline. Measured: stritrim("a  \t  b") comes back at
//     length 5 with the tab still at position 3, and strltrim("\t x") is
//     unchanged. The two space runs on either side of that tab are separate
//     internal runs, each collapsed on its own.
//   * soundex() and soundex_nara() are NOT the same function, and 21 of 24 name
//     pairs cannot tell them apart. The difference is whether H and W separate
//     two consonants that share a code: for soundex they do, for NARA they do
//     not. Ashcraft A226/A261, Burroughs B622/B620, Sachs S220/S200 are the
//     three discriminators, and they are in the selftest for that reason.
#ifndef PIET_NATIVE_EVAL_STATA_STRFNS_H
#define PIET_NATIVE_EVAL_STATA_STRFNS_H

#include <cctype>
#include <string>

namespace piet::eval::strfns {

// ---- blank handling ------------------------------------------------
// Stata's "blank" is ' '. Using std::isspace here would silently eat tabs,
// which is the one thing the capture says not to do.
inline bool is_blank(char c) { return c == ' '; }

// stritrim: collapse INTERNAL runs of blanks to one. Leading and trailing runs
// are copied verbatim -- stritrim("  a   b  ") is "  a b  ", not "a b".
inline std::string stritrim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && is_blank(s[a])) ++a;
  while (b > a && is_blank(s[b - 1])) --b;
  if (a >= b) return s;  // all blanks (or empty): nothing is internal
  std::string out;
  out.reserve(s.size());
  out.append(s, 0, a);
  bool prev_blank = false;
  for (size_t i = a; i < b; ++i) {
    const bool blank = is_blank(s[i]);
    if (blank && prev_blank) continue;
    out.push_back(s[i]);
    prev_blank = blank;
  }
  out.append(s, b, s.size() - b);
  return out;
}

inline std::string strltrim(const std::string& s) {
  size_t a = 0;
  while (a < s.size() && is_blank(s[a])) ++a;
  return s.substr(a);
}

inline std::string strrtrim(const std::string& s) {
  size_t b = s.size();
  while (b > 0 && is_blank(s[b - 1])) --b;
  return s.substr(0, b);
}

// ---- soundex -------------------------------------------------------
// 0 marks a letter that carries no code (the vowels plus H, W, Y).
inline char soundex_code(char c) {
  switch (std::toupper(static_cast<unsigned char>(c))) {
    case 'B': case 'F': case 'P': case 'V': return '1';
    case 'C': case 'G': case 'J': case 'K':
    case 'Q': case 'S': case 'X': case 'Z': return '2';
    case 'D': case 'T':                     return '3';
    case 'L':                               return '4';
    case 'M': case 'N':                     return '5';
    case 'R':                               return '6';
    default:                                return '0';
  }
}

// `nara` selects the U.S. Census variant. The ONLY difference is what happens
// to the "previous code" when a zero-code letter goes by: in plain soundex any
// of them resets it, so a following same-coded consonant is emitted again; in
// NARA, H and W are transparent and only the vowels reset. Everything that is
// not a letter (digits, spaces, punctuation) is skipped without resetting in
// either variant -- soundex_nara("Mc Donald") is M235 and ("O123Brien") is O165.
inline std::string soundex_impl(const std::string& s, bool nara) {
  size_t i = 0;
  while (i < s.size() && !std::isalpha(static_cast<unsigned char>(s[i]))) ++i;
  if (i >= s.size()) return "";  // no letters at all: soundex("123") is ""
  std::string out(1, static_cast<char>(std::toupper(static_cast<unsigned char>(s[i]))));
  char prev = soundex_code(s[i]);
  for (++i; i < s.size() && out.size() < 4; ++i) {
    const unsigned char ch = static_cast<unsigned char>(s[i]);
    if (!std::isalpha(ch)) continue;
    const char code = soundex_code(s[i]);
    if (code == '0') {
      const char u = static_cast<char>(std::toupper(ch));
      const bool transparent = nara && (u == 'H' || u == 'W');
      if (!transparent) prev = '0';
      continue;
    }
    if (code != prev) out.push_back(code);
    prev = code;
  }
  while (out.size() < 4) out.push_back('0');
  return out;
}

inline std::string soundex(const std::string& s) { return soundex_impl(s, false); }
inline std::string soundex_nara(const std::string& s) { return soundex_impl(s, true); }

// ---- ustrfix -------------------------------------------------------
// Replace each byte that is not part of a valid UTF-8 sequence. Stata takes at
// most ONE character of the replacement -- ustrfix(s, "xyz") uses "x" -- and an
// EMPTY replacement deletes the byte rather than inserting anything. With no
// replacement argument the substitute is U+FFFD.
inline bool utf8_seq_len(const std::string& s, size_t i, size_t& len) {
  const unsigned char c = static_cast<unsigned char>(s[i]);
  if (c < 0x80) { len = 1; return true; }
  size_t n = 0;
  unsigned int cp = 0;
  if ((c & 0xE0) == 0xC0) { n = 2; cp = c & 0x1Fu; }
  else if ((c & 0xF0) == 0xE0) { n = 3; cp = c & 0x0Fu; }
  else if ((c & 0xF8) == 0xF0) { n = 4; cp = c & 0x07u; }
  else return false;                                  // continuation or 5+ byte lead
  if (i + n > s.size()) return false;
  for (size_t k = 1; k < n; ++k) {
    const unsigned char cc = static_cast<unsigned char>(s[i + k]);
    if ((cc & 0xC0) != 0x80) return false;
    cp = (cp << 6) | (cc & 0x3Fu);
  }
  if (n == 2 && cp < 0x80) return false;              // overlong
  if (n == 3 && cp < 0x800) return false;
  if (n == 4 && cp < 0x10000) return false;
  if (cp > 0x10FFFF) return false;
  if (cp >= 0xD800 && cp <= 0xDFFF) return false;     // surrogate half
  len = n;
  return true;
}

inline std::string ustrfix(const std::string& s, const std::string& repl,
                           bool have_repl) {
  // One character of the replacement, measured: ustrfix(s,"xyz") -> "x".
  std::string sub;
  if (!have_repl) {
    sub = "\xEF\xBF\xBD";  // U+FFFD
  } else if (!repl.empty()) {
    size_t n = 1;
    if (!utf8_seq_len(repl, 0, n)) n = 1;
    sub = repl.substr(0, n);
  }  // empty replacement stays empty: the bad byte is deleted
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size();) {
    size_t n = 1;
    if (utf8_seq_len(s, i, n)) {
      out.append(s, i, n);
      i += n;
    } else {
      out += sub;
      ++i;  // one substitution per BAD BYTE, not per run
    }
  }
  return out;
}

}  // namespace piet::eval::strfns

#endif  // PIET_NATIVE_EVAL_STATA_STRFNS_H
