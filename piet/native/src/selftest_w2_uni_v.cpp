// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Unicode names, lane V (validators, import names, case folding, strtoname).
// Every expected value is StataBE 19.5's, taken from the probe do-files
// scratch-r150/uni-v/p1.do (import delimited header names, case(lower|upper)),
// p2.do (per-category keep/strip and the 32-character / fold order) and p3.do
// (strtoname over every general category); the do-files and logs are kept there.
#include "selftest_w2.h"

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "core/ident.h"
#include "piet/delimited_io.h"
#include "piet/dispatcher.h"
#include "piet/session_state.h"

namespace {

namespace fs = std::filesystem;
using piet::delimited::Case;

std::string rep(const std::string& s, int n) {
  std::string out;
  for (int k = 0; k < n; ++k) out += s;
  return out;
}

std::string join(const std::vector<std::string>& v) {
  std::string out;
  for (size_t i = 0; i < v.size(); ++i) out += (i ? "|" : "") + v[i];
  return out;
}

// The text of the LAST `display` block `cmd` produced.
std::string displayOf(piet::SessionState& session, const std::string& cmd) {
  std::string text;
  for (const auto& b : piet::dispatch(cmd, session)) {
    if (b.is_object() && b.value("type", std::string()) == "display") {
      text = b.value("text", std::string());
    }
  }
  return text;
}

void checkNames(const char* tag, const std::vector<std::string>& cells, Case c,
                const std::vector<std::string>& want) {
  const auto got = piet::delimited::legalNames(cells, c).names;
  w2check(got == want, std::string("UNI/V legalNames ") + tag + " (StataBE import delimited)" +
                           (got == want ? "" : ": got " + join(got) + " want " + join(want)));
}

}  // namespace

void testW2UniV() {
  std::printf("[selftest] --- Unicode names (V): import names, case folding, strtoname ---\n");

  // p1.do: 34 headers of h1.csv, default case(lower).
  checkNames(
      "h1 default (lower)",
      {"1abc", "123", "٣abc", "€x", "\U0001F600x", "a b", "a-b", "قيمة x",
       "a٣b", "a‌b", "ab́c", "_x", "__", "x.y", "(a)", "ΑΣ", "İstanbul",
       "ǅx", "ǆx", "ﬁx", "ŉx", "Ωx", "ΐx", "ǰx", "ᾳx",
       "ხx", "Ꭰx", "ꭰx", "x ", "-1", "$a", "1", "é1", "_1é"},
      Case::kLower,
      {"abc", "v2", "v3", "x", "v5", "ab", "v7", "قيمةx", "a٣b", "v10", "v11",
       "_x", "__", "xy", "a", "ας", "i̇stanbul", "ǆx", "v19", "ﬁx", "ŉx",
       "ωx", "ΐx", "ǰx", "ᾳx", "ხx", "ꭰx", "v28", "v29", "v30", "v31",
       "v32", "é1", "_1é"});

  // p1.do: the h2.csv headers, case(lower) and case(upper).
  const std::vector<std::string> h2 = {
      "ÉTÉ", "Straße", "قيمة", "ΑΣ", "İstanbul",
      "ǅx", "ﬁx", "ŉx", "ΐx", "ǰx", "ᾳx", "ხx", "Ꭰx",
      "ꭰx", "Ωx", "ǆx", "ΣΑΣ", "ǱA"};
  checkNames("h2 case(lower)", h2, Case::kLower,
             {"été", "straße", "قيمة", "ας",
              "i̇stanbul", "ǆx", "ﬁx", "ŉx", "ΐx", "ǰx", "ᾳx",
              "ხx", "ꭰx", "v14", "ωx", "v16", "σας", "ǳa"});
  checkNames("h2 case(upper)", h2, Case::kUpper,
             {"ÉTÉ", "STRASSE", "قيمة", "ΑΣ",
              "İSTANBUL", "ǄX", "FIX", "ʼNX", "Ϊ́X", "J̌X",
              "ΑΙX", "ᲮX", "ᎠX", "V14", "ΩX", "V16",
              "ΣΑΣ", "ǱA"});

  // p2.do (c3.csv): leading digits of any script go, `_` stops the strip.
  checkNames("leading digits", {"1_x", "٣٣y", "1٣z", "_9w", "9٣9v", "٣", "٣٣"},
             Case::kLower, {"_x", "y", "z", "_9w", "v", "v6", "v7"});
  // 40 Arabic letters -> 32 LETTERS (the oracle's import_long line); the fold of
  // 40 e-acute letters is cut at 32 first.
  checkNames("long Arabic header", {rep("ا", 40)}, Case::kLower, {rep("ا", 32)});
  checkNames("long e-acute header", {"t" + rep("é", 40)}, Case::kUpper,
             {"T" + rep("É", 31)});
  // p2.do: filter first (90 raw characters -> 60 letters), then cut to 32.
  {
    std::string h;
    for (int k = 0; k < 30; ++k) h += "ab ";
    checkNames("filter before cut", {h}, Case::kLower, {rep("ab", 16)});
  }
  // p2.do: the cut comes BEFORE the fold; a fold past 32 characters is v<column>,
  // not a re-cut (u + 20 sharp-s -> 41 characters upper-cased).
  checkNames("fold past 32 is v<col>", {"u" + rep("ß", 20), "s" + rep("ß", 17)},
             Case::kUpper, {"V1", "V2"});
  checkNames("fold within 32 (lower)", {"u" + rep("ß", 20)}, Case::kLower,
             {"u" + rep("ß", 20)});
  // p2.do: final sigma, also after a cased + case-ignorable letter.
  checkNames("final sigma", {"r" + rep("ΑΣ", 3), "ΑΣ1", "Σx", "ʰΣ",
                             "ΙΣΧΥΡΌΣ"},
             Case::kLower,
             {"rασασας", "ας1", "σx", "ʰς",
              "ισχυρός"});
  // p2.do: the keep set. A Unicode letter or Nd digit inside survives; a mark,
  // format character or symbol is dropped (so the later duplicates are v<col>).
  checkNames("kept inside", {"a٣b", "aقb", "a²b", "áb", "a€b"}, Case::kPreserve,
             {"a٣b", "aقb", "ab", "v4", "v5"});
  // case(preserve) leaves case alone.
  checkNames("preserve", {"ÉTÉ"}, Case::kPreserve, {"ÉTÉ"});
  // A malformed byte is dropped, never carried into a name.
  checkNames("malformed byte", {std::string("a\xd8") + "b"}, Case::kLower, {"ab"});

  // caseMapUtf8 directly (the table is generated from Unicode 16 by
  // tools/gen_unicode_case.py).
  w2check(piet::delimited::caseMapUtf8("Straße", Case::kUpper) == "STRASSE",
          "UNI/V caseMapUtf8: sharp s upper-cases to SS");
  w2check(piet::delimited::caseMapUtf8("Σ", Case::kLower) == "σ",
          "UNI/V caseMapUtf8: a lone capital sigma lowers to the medial form");

  // p3.do: strtoname works in BYTES, keeps every non-ASCII byte whole, prefixes an
  // ASCII leading digit, leaves "" empty, and strtoname(s, 0) does not prefix.
  const std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_w2_uni_v").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  {
    piet::SessionState session(workdir);
    auto st = [&](const std::string& expr) { return displayOf(session, "display " + expr); };
    w2check(st("strtoname(\"قيمة x\")") == "قيمة_x",
            "UNI/V strtoname: `قيمة x` -> `قيمة_x` (StataBE ops.golden)");
    w2check(st("strtoname(\"٣a\")") == "٣a",
            "UNI/V strtoname: a non-ASCII digit first is NOT prefixed (p3.do EX|0)");
    w2check(st("strtoname(\"1a\")") == "_1a", "UNI/V strtoname: an ASCII digit first gets `_` (p3.do EX|1)");
    w2check(displayOf(session, "display \"[\" strtoname(\"\") \"]\"") == "[]",
            "UNI/V strtoname: an empty string stays empty (p6.do EMPTY; the old `_` was a guess)");
    w2check(st("strtoname(\"1 b\", 0)") == "1_b", "UNI/V strtoname(s, 0): no prefix (p3.do EXN3)");
    w2check(st("strtoname(\"1 b\", 1)") == "_1_b", "UNI/V strtoname(s, 1): prefix (p3.do EXN2)");
    w2check(st("strtoname(\"a" + std::string("€") + "b\")") == "a€b",
            "UNI/V strtoname: a currency symbol survives (p3.do, every non-ASCII category kept)");
    w2check(st("strtoname(\"" + rep("a", 40) + "\")") == rep("a", 32),
            "UNI/V strtoname: ASCII cut at 32 (p3.do EX|4)");
    const std::string cut = st("strtoname(\"" + rep("é", 40) + "\")");
    w2check(cut == rep("é", 16),
            "UNI/V strtoname: 40 e-acute letters -> 16 letters, the 32-BYTE cut (p3.do EX|5)");
    w2check(st("strtoname(\"x" + rep("\U0001F600", 35) + "\")") == "x" + rep("\U0001F600", 7),
            "UNI/V strtoname: the byte cut never leaves half a character (StataBE leaves 3 stray bytes)");
  }
  fs::remove_all(workdir, ec);
}
