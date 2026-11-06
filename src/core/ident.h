// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// ident.h -- the ONE rule for what a Stata name may contain, shared by the cobra
// engine and the PIET front end (PIET already includes src/core headers).
//
// Stata 14+ names may carry Unicode. Measured against StataBE 19.5 (probe
// do-files scratch-r150/uni-oracle/rules{,2,3}.do):
//   * at most 32 CHARACTERS (code points), not bytes: 17 Arabic letters (34
//     bytes) and `a` + 31 emoji (125 bytes) are names, 33 characters are r(198);
//   * the FIRST character is an ASCII letter, `_`, or a Unicode letter (category
//     Lu/Ll/Lt/Lm/Lo). A digit of any script, a superscript, a mark, a format
//     character, a space, punctuation or a symbol is r(198) as the first
//     character;
//   * every LATER character is an ASCII letter, ASCII digit, `_`, or ANY
//     non-ASCII character (Arabic-Indic digits, harakat, ZWNJ, NBSP, the Arabic
//     comma, emoji and the euro sign are all accepted inside a name). ASCII
//     punctuation (`a-b`) is not.
//
// Two tiers follow from that:
//   * isValidName() is the strict test, for the moment a name is CREATED;
//   * isIdentStartByte()/isIdentByte() are the permissive byte tests every
//     scanner and lexer uses: a byte >= 0x80 is part of an identifier. UTF-8
//     continuation bytes never collide with an ASCII operator, so a scanner that
//     keeps a multi-byte name whole needs no decoding, and a name that already
//     exists is looked up byte-exactly against the dataset anyway. Without this a
//     scanner reading `دخل_2020` sees an Arabic byte as a word break and binds
//     the ASCII tail `_2020` on its own.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace cecon::ident {

inline bool isAsciiAlpha(unsigned char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
inline bool isAsciiDigit(unsigned char c) { return c >= '0' && c <= '9'; }

// Permissive scanner tests (see the header comment).
inline bool isIdentStartByte(unsigned char c) { return isAsciiAlpha(c) || c == '_' || c >= 0x80; }
inline bool isIdentByte(unsigned char c) { return isIdentStartByte(c) || isAsciiDigit(c); }
inline bool isIdentStartByte(char c) { return isIdentStartByte(static_cast<unsigned char>(c)); }
inline bool isIdentByte(char c) { return isIdentByte(static_cast<unsigned char>(c)); }

// Length in bytes of the UTF-8 sequence led by `c` (1 for ASCII and for a stray
// continuation byte, so a scan always advances).
inline size_t utf8SeqLen(unsigned char c) {
  if (c < 0x80) return 1;
  if ((c & 0xE0) == 0xC0) return 2;
  if ((c & 0xF0) == 0xE0) return 3;
  if ((c & 0xF8) == 0xF0) return 4;
  return 1;
}

// Decode the code point at s[i]. Returns the bytes consumed, or 0 for a malformed
// sequence (truncated, bad continuation, overlong, surrogate, > U+10FFFF).
inline size_t decodeUtf8(std::string_view s, size_t i, char32_t* cp) {
  if (i >= s.size()) return 0;
  const unsigned char c0 = static_cast<unsigned char>(s[i]);
  if (c0 < 0x80) {
    *cp = c0;
    return 1;
  }
  size_t n;
  char32_t v;
  if ((c0 & 0xE0) == 0xC0) {
    n = 2;
    v = c0 & 0x1F;
  } else if ((c0 & 0xF0) == 0xE0) {
    n = 3;
    v = c0 & 0x0F;
  } else if ((c0 & 0xF8) == 0xF0) {
    n = 4;
    v = c0 & 0x07;
  } else {
    return 0;
  }
  if (i + n > s.size()) return 0;
  for (size_t k = 1; k < n; ++k) {
    const unsigned char ck = static_cast<unsigned char>(s[i + k]);
    if ((ck & 0xC0) != 0x80) return 0;
    v = (v << 6) | (ck & 0x3F);
  }
  static const char32_t kMin[5] = {0, 0, 0x80, 0x800, 0x10000};
  if (v < kMin[n] || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) return 0;
  *cp = v;
  return n;
}

// Unicode category L (Lu Ll Lt Lm Lo) for code points >= 0x80.
inline bool isUnicodeLetter(char32_t cp) {
  struct Range {
    char32_t lo, hi;
  };
  static constexpr Range kLetters[] = {
#include "core/unicode_letters.inc"
  };
  size_t lo = 0, hi = sizeof(kLetters) / sizeof(kLetters[0]);
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    if (cp < kLetters[mid].lo) {
      hi = mid;
    } else if (cp > kLetters[mid].hi) {
      lo = mid + 1;
    } else {
      return true;
    }
  }
  return false;
}

// Number of code points (continuation bytes are not counted).
inline size_t codePointCount(std::string_view s) {
  size_t n = 0;
  for (char c : s)
    if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++n;
  return n;
}

// The longest prefix of `s` holding at most `maxChars` code points; never cuts a
// UTF-8 sequence. A character starts at every byte that is not a continuation
// byte -- the same count codePointCount() makes, so the two agree on malformed
// input too.
inline std::string truncateCodePoints(std::string_view s, size_t maxChars) {
  size_t n = 0;
  for (size_t i = 0; i < s.size(); ++i) {
    if ((static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) continue;
    if (n == maxChars) return std::string(s.substr(0, i));
    ++n;
  }
  return std::string(s);
}

// The strict rule (see the header comment).
inline bool isValidName(std::string_view name, size_t maxChars = 32) {
  if (name.empty()) return false;
  size_t i = 0, count = 0;
  while (i < name.size()) {
    char32_t cp = 0;
    const size_t n = decodeUtf8(name, i, &cp);
    if (n == 0) return false;
    if (count == 0) {
      if (cp < 0x80 ? !(isAsciiAlpha(static_cast<unsigned char>(cp)) || cp == '_') : !isUnicodeLetter(cp))
        return false;
    } else if (cp < 0x80 && !(isAsciiAlpha(static_cast<unsigned char>(cp)) ||
                              isAsciiDigit(static_cast<unsigned char>(cp)) || cp == '_')) {
      return false;
    }
    ++count;
    i += n;
  }
  return count <= maxChars;
}

}  // namespace cecon::ident
