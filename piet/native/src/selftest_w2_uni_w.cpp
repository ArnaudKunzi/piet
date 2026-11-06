// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Unicode names, lane W (fixed-width output). StataBE pads and abbreviates a
// Unicode variable name by CHARACTERS, not bytes: the expected stubs below are
// the ones scratch-r150/uni-v/p4.do and p5.do printed (sktest, centile, spearman
// via pwcorr's layout, xtsum, summarize), taken from a 40-observation panel with
// the variables قيمة, قيمةطويلة and قيمةطويلةجداجداجدا. The numbers differ here (a
// small fixed panel), so only the NAME columns and the layout lines are pinned.
#include "selftest_w2.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "core/ident.h"
#include "piet/dispatcher.h"
#include "piet/session_state.h"

namespace {

namespace fs = std::filesystem;

std::string displayOf(piet::SessionState& session, const std::string& cmd) {
  std::string text;
  for (const auto& b : piet::dispatch(cmd, session)) {
    if (b.is_object() && b.value("type", std::string()) == "display") {
      text = b.value("text", std::string());
    }
  }
  return text;
}

std::vector<std::string> linesOf(const std::string& text) {
  std::vector<std::string> out;
  std::istringstream in(text);
  std::string l;
  while (std::getline(in, l)) out.push_back(l);
  return out;
}

bool hasLine(const std::vector<std::string>& lines, const std::string& want) {
  for (const auto& l : lines)
    if (l == want) return true;
  return false;
}

bool hasPrefixLine(const std::vector<std::string>& lines, const std::string& prefix) {
  for (const auto& l : lines)
    if (l.compare(0, prefix.size(), prefix) == 0) return true;
  return false;
}

std::string sp(size_t n) { return std::string(n, ' '); }

}  // namespace

void testW2UniW() {
  std::printf("[selftest] --- Unicode names (W): fixed-width output by characters ---\n");
  std::error_code ec;
  const std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_w2_uni_w").u8string();
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  const std::string A = "قيمة";                  // قيمة (4)
  const std::string B = "قيمةطويلة";  // قيمةطويلة (9)
  // قيمةطويلةجداجداجدا (18)
  const std::string C = B + "جداجداجدا";
  {
    // 12 observations on 2 panels of 6; values are arbitrary but non-degenerate.
    std::ofstream f(workdir + "/p.csv", std::ios::binary);
    f << "id,t," << A << ",yyyy," << C << "," << B << ",displacement_long,weightxyzabc\n";
    const double v[12][4] = {{1.2, -0.3, 2.2, 0.9},  {0.4, 1.1, -1.0, 0.1},  {-0.7, 0.5, 0.3, -0.8},
                             {2.1, -1.4, 0.8, 1.7},  {0.0, 0.6, -0.2, 0.4},  {-1.1, 0.9, 1.5, -0.6},
                             {0.8, -0.2, -0.9, 1.2}, {1.9, 1.3, 0.1, -0.1},  {-0.4, -0.8, 1.1, 0.7},
                             {0.3, 0.2, -1.3, 1.4},  {-1.6, 1.0, 0.6, -1.0}, {1.0, -0.5, 0.2, 0.5}};
    for (int i = 0; i < 12; ++i)
      f << (i / 6 + 1) << "," << (i % 6) << "," << v[i][0] << "," << v[i][1] << "," << v[i][2] << ","
        << v[i][3] << "," << v[i][1] * 2 + i * 0.1 << "," << v[i][2] - v[i][0] * 0.3 << "\n";
  }
  piet::SessionState session(workdir);
  (void)displayOf(session, "import delimited using \"" + workdir + "/p.csv\", clear");

  // sktest (p5.do): the name column is as wide as the longest name (18 here), so
  // the header, the rule and every row stay aligned by characters.
  {
    const auto ls = linesOf(displayOf(session, "sktest " + A + " " + B + " " + C + " yyyy"));
    w2check(hasLine(ls, sp(63) + "----- Joint test -----"),
            "UNI/W sktest: the `Joint test` banner shifts with the 18-character name column (p5.do)");
    w2check(hasLine(ls, sp(10) + "Variable |       Obs   Pr(skewness)   Pr(kurtosis)   Adj chi2(2)  Prob>chi2"),
            "UNI/W sktest: `Variable` right-aligned in the 18-character stub (p5.do)");
    w2check(hasLine(ls, std::string(19, '-') +
                            "+-----------------------------------------------------------------"),
            "UNI/W sktest: rule is name width + 1 dashes before the `+` (p5.do)");
    w2check(hasPrefixLine(ls, sp(14) + A + " |"),
            "UNI/W sktest: a 4-character name is right-aligned by CHARACTERS (14 blanks, not 10; p5.do)");
    w2check(hasPrefixLine(ls, sp(9) + B + " |"),
            "UNI/W sktest: a 9-character name sits behind 9 blanks (p5.do)");
    w2check(hasPrefixLine(ls, C + " |"), "UNI/W sktest: an 18-character name fills the stub (p5.do)");
  }
  // centile (p4.do): a name past 12 characters is abbreviated `first 10 ~ last`.
  {
    const auto ls = linesOf(displayOf(session, "centile " + A + " " + C + " yyyy, centile(25 50)"));
    w2check(hasPrefixLine(ls, sp(8) + A + " |        12         25"),
            "UNI/W centile: `قيمة` is right-aligned in 12 CHARACTERS (8 blanks; p4.do)");
    const std::string abbr = "قيمةطويلةج~ا";
    w2check(hasPrefixLine(ls, abbr + " |        12         25"),
            "UNI/W centile: an 18-character name prints as `قيمةطويلةج~ا` (p4.do)");
  }
  // spearman matrix form (pwcorr's layout, p4.do): the 9-character name is
  // abbreviated to 8 characters in the column head.
  {
    const auto ls = linesOf(displayOf(session, "spearman " + A + " " + B + " yyyy"));
    const std::string head8 = "قيمةطو~ة";  // قيمةطو~ة
    bool headOk = false;
    for (const auto& l : ls)
      if (l.find(head8) != std::string::npos) headOk = true;
    w2check(headOk, "UNI/W spearman: column head `قيمةطويلة` -> `قيمةطو~ة` (8 characters; p4.do pwcorr)");
    w2check(hasPrefixLine(ls, sp(3) + B + " |"),
            "UNI/W spearman: a 9-character row name is right-aligned in 12 characters (3 blanks; p4.do)");
  }
  // The same abbreviation for an ASCII name (p6.do: StataBE spearman `displa~g
  // weight~c`, centile `displaceme~g`) -- the old code cut it hard at 8 / 12.
  {
    const auto sl = linesOf(displayOf(session, "spearman displacement_long weightxyzabc yyyy"));
    bool headOk = false;
    for (const auto& l : sl)
      if (l.find("displa~g weight~c") != std::string::npos) headOk = true;
    w2check(headOk && hasPrefixLine(sl, "displaceme~g |") && hasPrefixLine(sl, "weightxyzabc |"),
            "UNI/W spearman: ASCII long names abbreviate `displa~g` / `displaceme~g` (p6.do)");
    const auto cl = linesOf(displayOf(session, "centile displacement_long weightxyzabc, centile(50)"));
    w2check(hasPrefixLine(cl, "displaceme~g |        12         50") &&
                hasPrefixLine(cl, "weightxyzabc |        12         50"),
            "UNI/W centile: ASCII long names abbreviate `displaceme~g` (p6.do)");
  }
  // xtsum (p4.do): the 16-character stub is padded by characters.
  {
    (void)displayOf(session, "xtset id t");
    const auto ls = linesOf(displayOf(session, "xtsum " + A + " yyyy"));
    w2check(hasPrefixLine(ls, A + "  overall" + sp(3) + " |"),
            "UNI/W xtsum: the name+tag stub is 16 CHARACTERS wide (3 blanks after `overall`)");
    w2check(hasPrefixLine(ls, "yyyy  overall" + sp(3) + " |"),
            "UNI/W xtsum: an ASCII stub is the same 16 characters (unchanged)");
  }
  // esttab: every line of the fixed-width table is the same number of characters
  // wide, however many bytes the names take.
  {
    (void)displayOf(session, "quietly regress yyyy " + A + " " + B);
    (void)displayOf(session, "eststo clear");
    (void)displayOf(session, "eststo m1");
    const std::string txt = workdir + "/t.txt";
    (void)displayOf(session, "esttab m1 using \"" + txt + "\", replace");
    std::ifstream in(txt, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    const auto ls = linesOf(ss.str());
    // The table body runs from the first rule to the third; every line in it is
    // as many CHARACTERS wide as the rule, whatever the bytes of its labels.
    size_t want = 0, rules = 0, body = 0;
    bool same = !ls.empty();
    bool nameRow = false;
    for (const auto& l : ls) {
      const size_t n = cecon::ident::codePointCount(l);
      if (!l.empty() && l.find_first_not_of('-') == std::string::npos) {
        ++rules;
        if (want == 0) want = n;
        if (rules >= 3) break;
      }
      if (rules >= 1 && !l.empty()) {
        ++body;
        if (n != want) same = false;
      }
      if (l.compare(0, A.size(), A) == 0) nameRow = true;
    }
    w2check(body >= 6 && same && nameRow,
            "UNI/W esttab: the fixed-width table is one width in characters and lists the Arabic names");
  }
  fs::remove_all(workdir, ec);
}
