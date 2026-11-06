// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Unicode names, unit A: the shared rule in src/core/ident.h, the validator that
// wraps it (strutil::isValidStataIdentifier) and globMatch's `?`. Every expected
// verdict is StataBE 19.5's (`capture generate <name> = 1`, rc 0 vs r(198);
// probe do-files scratch-r150/uni-oracle/rules.do, rules2.do, rules3.do).
#include "selftest_w2.h"

#include <cstdio>
#include <string>
#include <vector>

#include "core/ident.h"
#include "piet/strutil.h"

namespace {

struct Case {
  const char* tag;
  std::string name;
  bool valid;
};

std::string rep(const std::string& s, int n) {
  std::string out;
  for (int k = 0; k < n; ++k) out += s;
  return out;
}

}  // namespace

void testW2UniCore() {
  std::printf("[selftest] --- Unicode names (A): shared rule, validator, globMatch ---\n");
  const std::vector<Case> cases = {
      {"arabic", "قيمة", true},
      {"cjk", "價格", true},
      {"latin_accent", "café", true},
      {"greek", "αβ", true},
      {"arabic_indic_digit_inside", "ق١", true},
      {"arabic_indic_digit_start", "١ق", false},
      {"harakat_inside", "قَم", true},
      {"harakat_start", "َx", false},
      {"zwnj_inside", "a‌b", true},
      {"zwnj_start", "‌x", false},
      {"arabic_comma_inside", "ا،ب", true},
      {"arabic_comma_start", "،x", false},
      {"emoji_inside", "x\U0001F600", true},
      {"emoji_start", "\U0001F600x", false},
      {"mixed_ascii_tail", "دخل_2020", true},
      {"superscript_start", "²x", false},
      {"superscript_inside", "x²", true},
      {"nbsp_inside", "a b", true},
      {"nbsp_start", " x", false},
      {"euro_inside", "a€b", true},
      {"euro_start", "€x", false},
      {"roman_numeral_start", "Ⅻx", false},
      {"fullwidth_digit_start", "１x", false},
      {"start_Lm", "ʰx", true},
      {"start_Lt", "ǅx", true},
      {"start_math_bold_A", "\U0001D400x", true},
      {"underscore_arabic", "_ق", true},
      {"ascii_dash_inside", "a-b", false},
      {"digit_start", "9zzz", false},
      {"len32_arabic", rep("ب", 32), true},
      {"len33_arabic", rep("ب", 33), false},
      {"a_31emoji_125bytes", "a" + rep("\U0001F600", 31), true},
      {"a_32emoji", "a" + rep("\U0001F600", 32), false},
      {"len32_ascii", std::string(32, 'a'), true},
      {"len33_ascii", std::string(33, 'a'), false},
      {"malformed_utf8", std::string("a\xd8"), false},
  };
  for (const auto& c : cases) {
    w2check(piet::strutil::isValidStataIdentifier(c.name) == c.valid,
            std::string("UNI/A rule: `") + c.tag + "` is " + (c.valid ? "a valid" : "NOT a valid") +
                " name (StataBE uni-oracle)");
  }
  // Truncation never splits a code point; 32 characters, not bytes.
  w2check(cecon::ident::truncateCodePoints(rep("ب", 40), 32) == rep("ب", 32),
          "UNI/A truncateCodePoints: 40 Arabic letters keep 32 whole letters (64 bytes)");
  w2check(cecon::ident::codePointCount("aب\U0001F600") == 3, "UNI/A codePointCount counts code points");
  // The permissive byte test keeps a mixed-script name whole.
  bool whole = true;
  for (char ch : std::string("دخل_2020")) whole = whole && cecon::ident::isIdentByte(ch);
  w2check(whole, "UNI/A isIdentByte: every byte of `دخل_2020` is an identifier byte");
  // globMatch: `?` is one character (StataBE `ds ق?` -> ق١ قا قب).
  w2check(piet::strutil::globMatch("ق?", "قا"), "UNI/A globMatch: `ق?` matches a 2-letter Arabic name");
  w2check(!piet::strutil::globMatch("ق?", "قيم"),
          "UNI/A globMatch: `ق?` does not match a 3-letter Arabic name");
  w2check(piet::strutil::globMatch("*ة", "قيمة"), "UNI/A globMatch: `*` before an Arabic suffix");
  w2check(piet::strutil::globMatch("a?c", "abc") && !piet::strutil::globMatch("a?c", "abbc"),
          "UNI/A globMatch: ASCII `?` unchanged");
}
