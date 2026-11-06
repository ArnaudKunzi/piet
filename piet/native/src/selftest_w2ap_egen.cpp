// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// W2AP lane `egen` (goal data-egen-stat-family, 2026-10-21) -- egen count, sum, min,
// max, median, pctile, iqr, sd, std, rank, pc, seq and mode and the row functions
// rowmax, rowmin, rowmiss, rowsd and rsum, against
// tests/eval/golden/aer_w2/w2ap_egen_family.log (StataNow 19.5 BE).
// GENERATED from the oracle by internal script gen_tu.py:
// the do-file's own fixture and, for every arm that was RED on main 6192e751 (plus
// five refusal controls), the arm's own stanza; each PREC record must equal the
// oracle's line, cited by log line. The arms rest on BUG-1000 (an existing target is
// r(110), never overwritten), BUG-1001 (each function's own option vocabulary, pc()
// and rsum(), row varlist wildcards, the refusals with Stata's rc), BUG-1002 (count()
// and mode() of a string) and BUG-1003 (seq() from() > to() counts down). On main
// each RED arm prints a different record (a refusal rc, or rc 0 for a statement Stata
// refuses, or the upward seq).
#include "selftest_w2.h"

#include <cstdio>
#include <exception>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "piet/dispatcher.h"
#include "piet/doexec.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

namespace {

struct EgenArm {
  const char* arm;    // oracle arm id
  int doLine;         // its stanza in w2ap_egen_family.do
  const char* stanza;
  std::vector<std::pair<int, const char*>> prec;  // (log line, PREC record)
};

const char* kEgenFixture =
    "clear all\n"
    "set linesize 255\n"
    "quietly set obs 20\n"
    "generate byte id = _n\n"
    "generate g = .\n"
    "generate str1 s = \"\"\n"
    "generate x = .\n"
    "generate double xd = .\n"
    "generate int k = .\n"
    "generate byte ex = 0\n"
    "generate w1 = .\n"
    "generate w2 = .\n"
    "generate w3 = .\n"
    "quietly replace g = 1 in 1\n"
    "quietly replace s = \"a\" in 1\n"
    "quietly replace x = 3 in 1\n"
    "quietly replace xd = 1/3 in 1\n"
    "quietly replace k = 1 in 1\n"
    "quietly replace w1 = 1 in 1\n"
    "quietly replace w2 = 2 in 1\n"
    "quietly replace w3 = 3 in 1\n"
    "quietly replace g = 1 in 2\n"
    "quietly replace s = \"a\" in 2\n"
    "quietly replace x = 1 in 2\n"
    "quietly replace xd = 2/3 in 2\n"
    "quietly replace k = 2 in 2\n"
    "quietly replace w2 = 2 in 2\n"
    "quietly replace w3 = 3 in 2\n"
    "quietly replace ex = 1 in 2\n"
    "quietly replace g = 1 in 3\n"
    "quietly replace s = \"a\" in 3\n"
    "quietly replace x = 3 in 3\n"
    "quietly replace xd = 1/3 in 3\n"
    "quietly replace k = 2 in 3\n"
    "quietly replace w3 = 3 in 3\n"
    "quietly replace g = 1 in 4\n"
    "quietly replace k = 1 in 4\n"
    "quietly replace g = 1 in 5\n"
    "quietly replace s = \"b\" in 5\n"
    "quietly replace x = 0.7 in 5\n"
    "quietly replace xd = 5/3 in 5\n"
    "quietly replace k = 2 in 5\n"
    "quietly replace w1 = 2 in 5\n"
    "quietly replace w2 = 2 in 5\n"
    "quietly replace w3 = 2 in 5\n"
    "quietly replace g = 1 in 6\n"
    "quietly replace s = \"b\" in 6\n"
    "quietly replace x = 5 in 6\n"
    "quietly replace xd = 2 in 6\n"
    "quietly replace w1 = 3 in 6\n"
    "quietly replace w2 = 1 in 6\n"
    "quietly replace g = 2 in 7\n"
    "quietly replace x = 4 in 7\n"
    "quietly replace xd = 7/3 in 7\n"
    "quietly replace k = 5 in 7\n"
    "quietly replace w1 = -1 in 7\n"
    "quietly replace w2 = -1 in 7\n"
    "quietly replace w3 = -2 in 7\n"
    "quietly replace g = 2 in 8\n"
    "quietly replace x = 4 in 8\n"
    "quietly replace xd = 7/3 in 8\n"
    "quietly replace k = 5 in 8\n"
    "quietly replace w1 = 4 in 8\n"
    "quietly replace w3 = 4 in 8\n"
    "quietly replace g = 2 in 9\n"
    "quietly replace s = \"b\" in 9\n"
    "quietly replace k = 6 in 9\n"
    "quietly replace w1 = 5 in 9\n"
    "quietly replace w2 = 5 in 9\n"
    "quietly replace g = 2 in 10\n"
    "quietly replace s = \"b\" in 10\n"
    "quietly replace x = -2 in 10\n"
    "quietly replace xd = -1/3 in 10\n"
    "quietly replace k = 6 in 10\n"
    "quietly replace w1 = 6 in 10\n"
    "quietly replace w2 = 7 in 10\n"
    "quietly replace w3 = 8 in 10\n"
    "quietly replace g = 3 in 11\n"
    "quietly replace s = \"c\" in 11\n"
    "quietly replace x = 7 in 11\n"
    "quietly replace xd = 11/3 in 11\n"
    "quietly replace k = 9 in 11\n"
    "quietly replace w1 = 7 in 11\n"
    "quietly replace g = 4 in 12\n"
    "quietly replace s = \"c\" in 12\n"
    "quietly replace w2 = 8 in 12\n"
    "quietly replace g = 4 in 13\n"
    "quietly replace w1 = 1.5 in 13\n"
    "quietly replace w2 = 2.5 in 13\n"
    "quietly replace w3 = 3.5 in 13\n"
    "quietly replace g = 4 in 14\n"
    "quietly replace x = .a in 14\n"
    "quietly replace xd = .b in 14\n"
    "quietly replace w1 = 0 in 14\n"
    "quietly replace w2 = 0 in 14\n"
    "quietly replace g = 5 in 15\n"
    "quietly replace s = \"d\" in 15\n"
    "quietly replace x = 1.5 in 15\n"
    "quietly replace xd = 5 in 15\n"
    "quietly replace k = 2 in 15\n"
    "quietly replace w1 = 2 in 15\n"
    "quietly replace w2 = 3 in 15\n"
    "quietly replace w3 = 1 in 15\n"
    "quietly replace ex = 1 in 15\n"
    "quietly replace g = 5 in 16\n"
    "quietly replace s = \"d\" in 16\n"
    "quietly replace x = 2.1 in 16\n"
    "quietly replace xd = 16/3 in 16\n"
    "quietly replace k = 2 in 16\n"
    "quietly replace w1 = 2 in 16\n"
    "quietly replace w3 = 2 in 16\n"
    "quietly replace ex = 1 in 16\n"
    "quietly replace g = 5 in 17\n"
    "quietly replace s = \"d\" in 17\n"
    "quietly replace x = 1.5 in 17\n"
    "quietly replace xd = 5 in 17\n"
    "quietly replace k = 3 in 17\n"
    "quietly replace w3 = 9 in 17\n"
    "quietly replace ex = 1 in 17\n"
    "quietly replace g = 5 in 18\n"
    "quietly replace s = \"d\" in 18\n"
    "quietly replace x = 10 in 18\n"
    "quietly replace xd = 6 in 18\n"
    "quietly replace k = 3 in 18\n"
    "quietly replace w1 = 3 in 18\n"
    "quietly replace w2 = 3 in 18\n"
    "quietly replace w3 = 3 in 18\n"
    "quietly replace ex = 1 in 18\n"
    "quietly replace s = \"e\" in 19\n"
    "quietly replace x = 6 in 19\n"
    "quietly replace xd = 19/3 in 19\n"
    "quietly replace k = 4 in 19\n"
    "quietly replace w1 = 1 in 19\n"
    "quietly replace w2 = 2 in 19\n"
    "quietly replace w3 = 3 in 19\n"
    "quietly replace s = \" \" in 20\n"
    "quietly replace x = 8 in 20\n"
    "quietly replace xd = 20/3 in 20\n"
    "quietly replace k = 4 in 20\n"
    "quietly replace w1 = 0.1 in 20\n"
    "quietly replace w2 = 0.2 in 20\n"
    "quietly replace w3 = 0.3 in 20\n"
    "sort id\n";

const std::vector<EgenArm>& egenArms() {
  static const std::vector<EgenArm> t = {
      {"eg_count_byopt_str", 191,
       "capture noisily egen c3 = count(s), by(g)\n"
       "local __rc = _rc\n"
       "sort id\n"
       "local __t \"absent\"\n"
       "capture local __t : type c3\n"
       "local __f \"absent\"\n"
       "capture local __f : format c3\n"
       "capture noisily display \"PREC|eg_count_byopt_str|\" `__rc' \"|\" \"`__t'|\" \"`__f'|\" %21x c3[1] \"|\" %21x c3[2] \"|\" %21x c3[3] \"|\" %21x c3[4] \"|\" %21x c3[5] \"|\" %21x c3[6] \"|\" %21x c3[7] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_count_byopt_str|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture noisily display \"PREC|eg_count_byopt_str_p2|\" `__rc' \"|\" %21x c3[8] \"|\" %21x c3[9] \"|\" %21x c3[10] \"|\" %21x c3[11] \"|\" %21x c3[12] \"|\" %21x c3[13] \"|\" %21x c3[14] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_count_byopt_str_p2|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture noisily display \"PREC|eg_count_byopt_str_p3|\" `__rc' \"|\" %21x c3[15] \"|\" %21x c3[16] \"|\" %21x c3[17] \"|\" %21x c3[18] \"|\" %21x c3[19] \"|\" %21x c3[20] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_count_byopt_str_p3|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture drop c3\n",
       {{418, "PREC|eg_count_byopt_str|0|float|%9.0g|+1.4000000000000X+002|+1.4000000000000X+002|+1.4000000000000X+002|+1.4000000000000X+002|+1.4000000000000X+002|+1.4000000000000X+002|+1.0000000000000X+001|"}, {423, "PREC|eg_count_byopt_str_p2|0|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|"}, {428, "PREC|eg_count_byopt_str_p3|0|+1.0000000000000X+002|+1.0000000000000X+002|+1.0000000000000X+002|+1.0000000000000X+002|+1.0000000000000X+001|+1.0000000000000X+001|"}}},
      {"eg_mode_byopt_min", 595,
       "capture noisily egen mo1 = mode(k), by(g) minmode\n"
       "local __rc = _rc\n"
       "sort id\n"
       "local __t \"absent\"\n"
       "capture local __t : type mo1\n"
       "local __f \"absent\"\n"
       "capture local __f : format mo1\n"
       "capture noisily display \"PREC|eg_mode_byopt_min|\" `__rc' \"|\" \"`__t'|\" \"`__f'|\" %21x mo1[1] \"|\" %21x mo1[2] \"|\" %21x mo1[3] \"|\" %21x mo1[4] \"|\" %21x mo1[5] \"|\" %21x mo1[6] \"|\" %21x mo1[7] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_mode_byopt_min|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture noisily display \"PREC|eg_mode_byopt_min_p2|\" `__rc' \"|\" %21x mo1[8] \"|\" %21x mo1[9] \"|\" %21x mo1[10] \"|\" %21x mo1[11] \"|\" %21x mo1[12] \"|\" %21x mo1[13] \"|\" %21x mo1[14] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_mode_byopt_min_p2|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture noisily display \"PREC|eg_mode_byopt_min_p3|\" `__rc' \"|\" %21x mo1[15] \"|\" %21x mo1[16] \"|\" %21x mo1[17] \"|\" %21x mo1[18] \"|\" %21x mo1[19] \"|\" %21x mo1[20] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_mode_byopt_min_p3|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture drop mo1\n",
       {{1270, "PREC|eg_mode_byopt_min|0|int|%8.0g|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+001|+1.4000000000000X+002|"}, {1275, "PREC|eg_mode_byopt_min_p2|0|+1.4000000000000X+002|+1.4000000000000X+002|+1.4000000000000X+002|+1.2000000000000X+003|+1.0000000000000X+3ff|+1.0000000000000X+3ff|+1.0000000000000X+3ff|"}, {1280, "PREC|eg_mode_byopt_min_p3|0|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+002|+1.0000000000000X+002|"}}},
      {"eg_mode_byopt_max", 611,
       "capture noisily egen mo2 = mode(k), maxmode by(g)\n"
       "local __rc = _rc\n"
       "sort id\n"
       "local __t \"absent\"\n"
       "capture local __t : type mo2\n"
       "local __f \"absent\"\n"
       "capture local __f : format mo2\n"
       "capture noisily display \"PREC|eg_mode_byopt_max|\" `__rc' \"|\" \"`__t'|\" \"`__f'|\" %21x mo2[1] \"|\" %21x mo2[2] \"|\" %21x mo2[3] \"|\" %21x mo2[4] \"|\" %21x mo2[5] \"|\" %21x mo2[6] \"|\" %21x mo2[7] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_mode_byopt_max|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture noisily display \"PREC|eg_mode_byopt_max_p2|\" `__rc' \"|\" %21x mo2[8] \"|\" %21x mo2[9] \"|\" %21x mo2[10] \"|\" %21x mo2[11] \"|\" %21x mo2[12] \"|\" %21x mo2[13] \"|\" %21x mo2[14] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_mode_byopt_max_p2|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture noisily display \"PREC|eg_mode_byopt_max_p3|\" `__rc' \"|\" %21x mo2[15] \"|\" %21x mo2[16] \"|\" %21x mo2[17] \"|\" %21x mo2[18] \"|\" %21x mo2[19] \"|\" %21x mo2[20] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_mode_byopt_max_p3|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture drop mo2\n",
       {{1305, "PREC|eg_mode_byopt_max|0|int|%8.0g|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+001|+1.8000000000000X+002|"}, {1310, "PREC|eg_mode_byopt_max_p2|0|+1.8000000000000X+002|+1.8000000000000X+002|+1.8000000000000X+002|+1.2000000000000X+003|+1.0000000000000X+3ff|+1.0000000000000X+3ff|+1.0000000000000X+3ff|"}, {1315, "PREC|eg_mode_byopt_max_p3|0|+1.8000000000000X+001|+1.8000000000000X+001|+1.8000000000000X+001|+1.8000000000000X+001|+1.0000000000000X+002|+1.0000000000000X+002|"}}},
      {"eg_mode_byopt_miss", 643,
       "capture noisily egen mo4 = mode(k), by(g) missing\n"
       "local __rc = _rc\n"
       "sort id\n"
       "local __t \"absent\"\n"
       "capture local __t : type mo4\n"
       "local __f \"absent\"\n"
       "capture local __f : format mo4\n"
       "capture noisily display \"PREC|eg_mode_byopt_miss|\" `__rc' \"|\" \"`__t'|\" \"`__f'|\" %21x mo4[1] \"|\" %21x mo4[2] \"|\" %21x mo4[3] \"|\" %21x mo4[4] \"|\" %21x mo4[5] \"|\" %21x mo4[6] \"|\" %21x mo4[7] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_mode_byopt_miss|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture noisily display \"PREC|eg_mode_byopt_miss_p2|\" `__rc' \"|\" %21x mo4[8] \"|\" %21x mo4[9] \"|\" %21x mo4[10] \"|\" %21x mo4[11] \"|\" %21x mo4[12] \"|\" %21x mo4[13] \"|\" %21x mo4[14] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_mode_byopt_miss_p2|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture noisily display \"PREC|eg_mode_byopt_miss_p3|\" `__rc' \"|\" %21x mo4[15] \"|\" %21x mo4[16] \"|\" %21x mo4[17] \"|\" %21x mo4[18] \"|\" %21x mo4[19] \"|\" %21x mo4[20] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_mode_byopt_miss_p3|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture drop mo4\n",
       {{1376, "PREC|eg_mode_byopt_miss|0|int|%8.0g|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+3ff|"}, {1381, "PREC|eg_mode_byopt_miss_p2|0|+1.0000000000000X+3ff|+1.0000000000000X+3ff|+1.0000000000000X+3ff|+1.2000000000000X+003|+1.0000000000000X+3ff|+1.0000000000000X+3ff|+1.0000000000000X+3ff|"}, {1386, "PREC|eg_mode_byopt_miss_p3|0|+1.0000000000000X+3ff|+1.0000000000000X+3ff|+1.0000000000000X+3ff|+1.0000000000000X+3ff|+1.0000000000000X+002|+1.0000000000000X+002|"}}},
      {"eg_mode_byp_max", 659,
       "capture noisily bysort s: egen mo5 = mode(k), maxmode\n"
       "local __rc = _rc\n"
       "sort id\n"
       "local __t \"absent\"\n"
       "capture local __t : type mo5\n"
       "local __f \"absent\"\n"
       "capture local __f : format mo5\n"
       "capture noisily display \"PREC|eg_mode_byp_max|\" `__rc' \"|\" \"`__t'|\" \"`__f'|\" %21x mo5[1] \"|\" %21x mo5[2] \"|\" %21x mo5[3] \"|\" %21x mo5[4] \"|\" %21x mo5[5] \"|\" %21x mo5[6] \"|\" %21x mo5[7] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_mode_byp_max|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture noisily display \"PREC|eg_mode_byp_max_p2|\" `__rc' \"|\" %21x mo5[8] \"|\" %21x mo5[9] \"|\" %21x mo5[10] \"|\" %21x mo5[11] \"|\" %21x mo5[12] \"|\" %21x mo5[13] \"|\" %21x mo5[14] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_mode_byp_max_p2|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture noisily display \"PREC|eg_mode_byp_max_p3|\" `__rc' \"|\" %21x mo5[15] \"|\" %21x mo5[16] \"|\" %21x mo5[17] \"|\" %21x mo5[18] \"|\" %21x mo5[19] \"|\" %21x mo5[20] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_mode_byp_max_p3|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture drop mo5\n",
       {{1409, "PREC|eg_mode_byp_max|0|int|%8.0g|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+001|+1.4000000000000X+002|+1.8000000000000X+002|+1.8000000000000X+002|+1.4000000000000X+002|"}, {1414, "PREC|eg_mode_byp_max_p2|0|+1.4000000000000X+002|+1.8000000000000X+002|+1.8000000000000X+002|+1.2000000000000X+003|+1.2000000000000X+003|+1.4000000000000X+002|+1.4000000000000X+002|"}, {1419, "PREC|eg_mode_byp_max_p3|0|+1.8000000000000X+001|+1.8000000000000X+001|+1.8000000000000X+001|+1.8000000000000X+001|+1.0000000000000X+002|+1.0000000000000X+002|"}}},
      {"eg_mode_byp_if", 675,
       "capture noisily bys g: egen mo6 = mode(k) if k>1, maxmode\n"
       "local __rc = _rc\n"
       "sort id\n"
       "local __t \"absent\"\n"
       "capture local __t : type mo6\n"
       "local __f \"absent\"\n"
       "capture local __f : format mo6\n"
       "capture noisily display \"PREC|eg_mode_byp_if|\" `__rc' \"|\" \"`__t'|\" \"`__f'|\" %21x mo6[1] \"|\" %21x mo6[2] \"|\" %21x mo6[3] \"|\" %21x mo6[4] \"|\" %21x mo6[5] \"|\" %21x mo6[6] \"|\" %21x mo6[7] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_mode_byp_if|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture noisily display \"PREC|eg_mode_byp_if_p2|\" `__rc' \"|\" %21x mo6[8] \"|\" %21x mo6[9] \"|\" %21x mo6[10] \"|\" %21x mo6[11] \"|\" %21x mo6[12] \"|\" %21x mo6[13] \"|\" %21x mo6[14] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_mode_byp_if_p2|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture noisily display \"PREC|eg_mode_byp_if_p3|\" `__rc' \"|\" %21x mo6[15] \"|\" %21x mo6[16] \"|\" %21x mo6[17] \"|\" %21x mo6[18] \"|\" %21x mo6[19] \"|\" %21x mo6[20] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_mode_byp_if_p3|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture drop mo6\n",
       {{1444, "PREC|eg_mode_byp_if|0|int|%8.0g|+1.0000000000000X+3ff|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+3ff|+1.0000000000000X+001|+1.0000000000000X+001|+1.8000000000000X+002|"}, {1449, "PREC|eg_mode_byp_if_p2|0|+1.8000000000000X+002|+1.8000000000000X+002|+1.8000000000000X+002|+1.2000000000000X+003|+1.0000000000000X+3ff|+1.0000000000000X+3ff|+1.0000000000000X+3ff|"}, {1454, "PREC|eg_mode_byp_if_p3|0|+1.8000000000000X+001|+1.8000000000000X+001|+1.8000000000000X+001|+1.8000000000000X+001|+1.0000000000000X+002|+1.0000000000000X+002|"}}},
      {"eg_mode_byp_minab", 691,
       "capture noisily bys g: egen mo7 = mode(k), min\n"
       "local __rc = _rc\n"
       "sort id\n"
       "local __t \"absent\"\n"
       "capture local __t : type mo7\n"
       "local __f \"absent\"\n"
       "capture local __f : format mo7\n"
       "capture noisily display \"PREC|eg_mode_byp_minab|\" `__rc' \"|\" \"`__t'|\" \"`__f'|\" %21x mo7[1] \"|\" %21x mo7[2] \"|\" %21x mo7[3] \"|\" %21x mo7[4] \"|\" %21x mo7[5] \"|\" %21x mo7[6] \"|\" %21x mo7[7] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_mode_byp_minab|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture noisily display \"PREC|eg_mode_byp_minab_p2|\" `__rc' \"|\" %21x mo7[8] \"|\" %21x mo7[9] \"|\" %21x mo7[10] \"|\" %21x mo7[11] \"|\" %21x mo7[12] \"|\" %21x mo7[13] \"|\" %21x mo7[14] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_mode_byp_minab_p2|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture noisily display \"PREC|eg_mode_byp_minab_p3|\" `__rc' \"|\" %21x mo7[15] \"|\" %21x mo7[16] \"|\" %21x mo7[17] \"|\" %21x mo7[18] \"|\" %21x mo7[19] \"|\" %21x mo7[20] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_mode_byp_minab_p3|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture drop mo7\n",
       {{1479, "PREC|eg_mode_byp_minab|0|int|%8.0g|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+001|+1.4000000000000X+002|"}, {1484, "PREC|eg_mode_byp_minab_p2|0|+1.4000000000000X+002|+1.4000000000000X+002|+1.4000000000000X+002|+1.2000000000000X+003|+1.0000000000000X+3ff|+1.0000000000000X+3ff|+1.0000000000000X+3ff|"}, {1489, "PREC|eg_mode_byp_minab_p3|0|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+002|+1.0000000000000X+002|"}}},
      {"eg_mode_byp_maxab", 707,
       "sort g id\n"
       "capture noisily by g: egen mo8 = mode(k),max\n"
       "local __rc = _rc\n"
       "sort id\n"
       "local __t \"absent\"\n"
       "capture local __t : type mo8\n"
       "local __f \"absent\"\n"
       "capture local __f : format mo8\n"
       "capture noisily display \"PREC|eg_mode_byp_maxab|\" `__rc' \"|\" \"`__t'|\" \"`__f'|\" %21x mo8[1] \"|\" %21x mo8[2] \"|\" %21x mo8[3] \"|\" %21x mo8[4] \"|\" %21x mo8[5] \"|\" %21x mo8[6] \"|\" %21x mo8[7] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_mode_byp_maxab|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture noisily display \"PREC|eg_mode_byp_maxab_p2|\" `__rc' \"|\" %21x mo8[8] \"|\" %21x mo8[9] \"|\" %21x mo8[10] \"|\" %21x mo8[11] \"|\" %21x mo8[12] \"|\" %21x mo8[13] \"|\" %21x mo8[14] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_mode_byp_maxab_p2|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture noisily display \"PREC|eg_mode_byp_maxab_p3|\" `__rc' \"|\" %21x mo8[15] \"|\" %21x mo8[16] \"|\" %21x mo8[17] \"|\" %21x mo8[18] \"|\" %21x mo8[19] \"|\" %21x mo8[20] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_mode_byp_maxab_p3|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture drop mo8\n",
       {{1516, "PREC|eg_mode_byp_maxab|0|int|%8.0g|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+001|+1.8000000000000X+002|"}, {1521, "PREC|eg_mode_byp_maxab_p2|0|+1.8000000000000X+002|+1.8000000000000X+002|+1.8000000000000X+002|+1.2000000000000X+003|+1.0000000000000X+3ff|+1.0000000000000X+3ff|+1.0000000000000X+3ff|"}, {1526, "PREC|eg_mode_byp_maxab_p3|0|+1.8000000000000X+001|+1.8000000000000X+001|+1.8000000000000X+001|+1.8000000000000X+001|+1.0000000000000X+002|+1.0000000000000X+002|"}}},
      {"eg_mode_byp_num", 740,
       "capture noisily bys g: egen mo10 = mode(k), nummode(2)\n"
       "local __rc = _rc\n"
       "sort id\n"
       "local __t \"absent\"\n"
       "capture local __t : type mo10\n"
       "local __f \"absent\"\n"
       "capture local __f : format mo10\n"
       "capture noisily display \"PREC|eg_mode_byp_num|\" `__rc' \"|\" \"`__t'|\" \"`__f'|\" %21x mo10[1] \"|\" %21x mo10[2] \"|\" %21x mo10[3] \"|\" %21x mo10[4] \"|\" %21x mo10[5] \"|\" %21x mo10[6] \"|\" %21x mo10[7] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_mode_byp_num|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture noisily display \"PREC|eg_mode_byp_num_p2|\" `__rc' \"|\" %21x mo10[8] \"|\" %21x mo10[9] \"|\" %21x mo10[10] \"|\" %21x mo10[11] \"|\" %21x mo10[12] \"|\" %21x mo10[13] \"|\" %21x mo10[14] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_mode_byp_num_p2|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture noisily display \"PREC|eg_mode_byp_num_p3|\" `__rc' \"|\" %21x mo10[15] \"|\" %21x mo10[16] \"|\" %21x mo10[17] \"|\" %21x mo10[18] \"|\" %21x mo10[19] \"|\" %21x mo10[20] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_mode_byp_num_p3|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture drop mo10\n",
       {{1588, "PREC|eg_mode_byp_num|0|int|%8.0g|+1.0000000000000X+3ff|+1.0000000000000X+3ff|+1.0000000000000X+3ff|+1.0000000000000X+3ff|+1.0000000000000X+3ff|+1.0000000000000X+3ff|+1.8000000000000X+002|"}, {1593, "PREC|eg_mode_byp_num_p2|0|+1.8000000000000X+002|+1.8000000000000X+002|+1.8000000000000X+002|+1.0000000000000X+3ff|+1.0000000000000X+3ff|+1.0000000000000X+3ff|+1.0000000000000X+3ff|"}, {1598, "PREC|eg_mode_byp_num_p3|0|+1.8000000000000X+001|+1.8000000000000X+001|+1.8000000000000X+001|+1.8000000000000X+001|+1.0000000000000X+3ff|+1.0000000000000X+3ff|"}}},
      {"eg_mode_byp_str", 756,
       "capture noisily bys g: egen mo11 = mode(s), maxmode\n"
       "local __rc = _rc\n"
       "sort id\n"
       "local __t \"absent\"\n"
       "capture local __t : type mo11\n"
       "local __f \"absent\"\n"
       "capture local __f : format mo11\n"
       "capture noisily display \"PREC|eg_mode_byp_str|\" `__rc' \"|\" \"`__t'|\" \"`__f'|\" \"[\" mo11[1] \"]|\" \"[\" mo11[2] \"]|\" \"[\" mo11[3] \"]|\" \"[\" mo11[4] \"]|\" \"[\" mo11[5] \"]|\" \"[\" mo11[6] \"]|\" \"[\" mo11[7] \"]|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_mode_byp_str|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture noisily display \"PREC|eg_mode_byp_str_p2|\" `__rc' \"|\" \"[\" mo11[8] \"]|\" \"[\" mo11[9] \"]|\" \"[\" mo11[10] \"]|\" \"[\" mo11[11] \"]|\" \"[\" mo11[12] \"]|\" \"[\" mo11[13] \"]|\" \"[\" mo11[14] \"]|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_mode_byp_str_p2|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture noisily display \"PREC|eg_mode_byp_str_p3|\" `__rc' \"|\" \"[\" mo11[15] \"]|\" \"[\" mo11[16] \"]|\" \"[\" mo11[17] \"]|\" \"[\" mo11[18] \"]|\" \"[\" mo11[19] \"]|\" \"[\" mo11[20] \"]|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_mode_byp_str_p3|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture drop mo11\n",
       {{1621, "PREC|eg_mode_byp_str|0|str1|%9s|[a]|[a]|[a]|[a]|[a]|[a]|[b]|"}, {1626, "PREC|eg_mode_byp_str_p2|0|[b]|[b]|[b]|[c]|[c]|[c]|[c]|"}, {1631, "PREC|eg_mode_byp_str_p3|0|[d]|[d]|[d]|[d]|[e]|[e]|"}}},
      {"eg_pc_plain", 772,
       "capture noisily egen pc1 = pc(x)\n"
       "local __rc = _rc\n"
       "sort id\n"
       "local __t \"absent\"\n"
       "capture local __t : type pc1\n"
       "local __f \"absent\"\n"
       "capture local __f : format pc1\n"
       "capture noisily display \"PREC|eg_pc_plain|\" `__rc' \"|\" \"`__t'|\" \"`__f'|\" %21x pc1[1] \"|\" %21x pc1[2] \"|\" %21x pc1[3] \"|\" %21x pc1[4] \"|\" %21x pc1[5] \"|\" %21x pc1[6] \"|\" %21x pc1[7] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_pc_plain|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture noisily display \"PREC|eg_pc_plain_p2|\" `__rc' \"|\" %21x pc1[8] \"|\" %21x pc1[9] \"|\" %21x pc1[10] \"|\" %21x pc1[11] \"|\" %21x pc1[12] \"|\" %21x pc1[13] \"|\" %21x pc1[14] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_pc_plain_p2|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture noisily display \"PREC|eg_pc_plain_p3|\" `__rc' \"|\" %21x pc1[15] \"|\" %21x pc1[16] \"|\" %21x pc1[17] \"|\" %21x pc1[18] \"|\" %21x pc1[19] \"|\" %21x pc1[20] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_pc_plain_p3|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture drop pc1\n",
       {{1655, "PREC|eg_pc_plain|0|float|%9.0g|+1.5e5d6e0000000X+002|+1.d3273e0000000X+000|+1.5e5d6e0000000X+002|+1.0000000000000X+3ff|+1.4701de0000000X+000|+1.23f8860000000X+003|+1.d3273e0000000X+002|"}, {1660, "PREC|eg_pc_plain_p2|0|+1.d3273e0000000X+002|+1.0000000000000X+3ff|-1.d3273e0000000X+001|+1.98c2560000000X+003|+1.0000000000000X+3ff|+1.0000000000000X+3ff|+1.0000000000000X+3ff|"}, {1665, "PREC|eg_pc_plain_p3|0|+1.5e5d6e0000000X+001|+1.ea82cc0000000X+001|+1.5e5d6e0000000X+001|+1.23f8860000000X+004|+1.5e5d6e0000000X+003|+1.d3273e0000000X+003|"}}},
      {"eg_pc_plain_prop", 788,
       "capture noisily egen pc2 = pc(xd), prop\n"
       "local __rc = _rc\n"
       "sort id\n"
       "local __t \"absent\"\n"
       "capture local __t : type pc2\n"
       "local __f \"absent\"\n"
       "capture local __f : format pc2\n"
       "capture noisily display \"PREC|eg_pc_plain_prop|\" `__rc' \"|\" \"`__t'|\" \"`__f'|\" %21x pc2[1] \"|\" %21x pc2[2] \"|\" %21x pc2[3] \"|\" %21x pc2[4] \"|\" %21x pc2[5] \"|\" %21x pc2[6] \"|\" %21x pc2[7] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_pc_plain_prop|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture noisily display \"PREC|eg_pc_plain_prop_p2|\" `__rc' \"|\" %21x pc2[8] \"|\" %21x pc2[9] \"|\" %21x pc2[10] \"|\" %21x pc2[11] \"|\" %21x pc2[12] \"|\" %21x pc2[13] \"|\" %21x pc2[14] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_pc_plain_prop_p2|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture noisily display \"PREC|eg_pc_plain_prop_p3|\" `__rc' \"|\" %21x pc2[15] \"|\" %21x pc2[16] \"|\" %21x pc2[17] \"|\" %21x pc2[18] \"|\" %21x pc2[19] \"|\" %21x pc2[20] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_pc_plain_prop_p3|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture drop pc2\n",
       {{1689, "PREC|eg_pc_plain_prop|0|float|%9.0g|+1.cd85680000000X-008|+1.cd85680000000X-007|+1.cd85680000000X-008|+1.0000000000000X+3ff|+1.2073620000000X-005|+1.5a240e0000000X-005|+1.93d4bc0000000X-005|"}, {1694, "PREC|eg_pc_plain_prop_p2|0|+1.93d4bc0000000X-005|+1.0000000000000X+3ff|-1.cd85680000000X-008|+1.3d4bb80000000X-004|+1.0000000000000X+3ff|+1.0000000000000X+3ff|+1.0000000000000X+3ff|"}, {1699, "PREC|eg_pc_plain_prop_p3|0|+1.b0ad120000000X-004|+1.cd85680000000X-004|+1.b0ad120000000X-004|+1.039b0a0000000X-003|+1.1207360000000X-003|+1.2073620000000X-003|"}}},
      {"eg_pc_plain_if", 804,
       "capture noisily egen pc3 = pc(x) if ex==0\n"
       "local __rc = _rc\n"
       "sort id\n"
       "local __t \"absent\"\n"
       "capture local __t : type pc3\n"
       "local __f \"absent\"\n"
       "capture local __f : format pc3\n"
       "capture noisily display \"PREC|eg_pc_plain_if|\" `__rc' \"|\" \"`__t'|\" \"`__f'|\" %21x pc3[1] \"|\" %21x pc3[2] \"|\" %21x pc3[3] \"|\" %21x pc3[4] \"|\" %21x pc3[5] \"|\" %21x pc3[6] \"|\" %21x pc3[7] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_pc_plain_if|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture noisily display \"PREC|eg_pc_plain_if_p2|\" `__rc' \"|\" %21x pc3[8] \"|\" %21x pc3[9] \"|\" %21x pc3[10] \"|\" %21x pc3[11] \"|\" %21x pc3[12] \"|\" %21x pc3[13] \"|\" %21x pc3[14] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_pc_plain_if_p2|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture noisily display \"PREC|eg_pc_plain_if_p3|\" `__rc' \"|\" %21x pc3[15] \"|\" %21x pc3[16] \"|\" %21x pc3[17] \"|\" %21x pc3[18] \"|\" %21x pc3[19] \"|\" %21x pc3[20] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_pc_plain_if_p3|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture drop pc3\n",
       {{1723, "PREC|eg_pc_plain_if|0|float|%9.0g|+1.f01fc00000000X+002|+1.0000000000000X+3ff|+1.f01fc00000000X+002|+1.0000000000000X+3ff|+1.cf0c920000000X+000|+1.9d6fcc0000000X+003|+1.4abfd60000000X+003|"}, {1728, "PREC|eg_pc_plain_if_p2|0|+1.4abfd60000000X+003|+1.0000000000000X+3ff|-1.4abfd60000000X+002|+1.2167da0000000X+004|+1.0000000000000X+3ff|+1.0000000000000X+3ff|+1.0000000000000X+3ff|"}, {1733, "PREC|eg_pc_plain_if_p3|0|+1.0000000000000X+3ff|+1.0000000000000X+3ff|+1.0000000000000X+3ff|+1.0000000000000X+3ff|+1.f01fc00000000X+003|+1.4abfd60000000X+004|"}}},
      {"eg_pc_byp_prop", 820,
       "capture noisily bysort g: egen pc4 = pc(x), prop\n"
       "local __rc = _rc\n"
       "sort id\n"
       "local __t \"absent\"\n"
       "capture local __t : type pc4\n"
       "local __f \"absent\"\n"
       "capture local __f : format pc4\n"
       "capture noisily display \"PREC|eg_pc_byp_prop|\" `__rc' \"|\" \"`__t'|\" \"`__f'|\" %21x pc4[1] \"|\" %21x pc4[2] \"|\" %21x pc4[3] \"|\" %21x pc4[4] \"|\" %21x pc4[5] \"|\" %21x pc4[6] \"|\" %21x pc4[7] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_pc_byp_prop|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture noisily display \"PREC|eg_pc_byp_prop_p2|\" `__rc' \"|\" %21x pc4[8] \"|\" %21x pc4[9] \"|\" %21x pc4[10] \"|\" %21x pc4[11] \"|\" %21x pc4[12] \"|\" %21x pc4[13] \"|\" %21x pc4[14] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_pc_byp_prop_p2|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture noisily display \"PREC|eg_pc_byp_prop_p3|\" `__rc' \"|\" %21x pc4[15] \"|\" %21x pc4[16] \"|\" %21x pc4[17] \"|\" %21x pc4[18] \"|\" %21x pc4[19] \"|\" %21x pc4[20] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_pc_byp_prop_p3|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture drop pc4\n",
       {{1757, "PREC|eg_pc_byp_prop|0|float|%9.0g|+1.e3c7900000000X-003|+1.42850a0000000X-004|+1.e3c7900000000X-003|+1.0000000000000X+3ff|+1.c3870e0000000X-005|+1.93264c0000000X-002|+1.5555560000000X-001|"}, {1762, "PREC|eg_pc_byp_prop_p2|0|+1.5555560000000X-001|+1.0000000000000X+3ff|-1.5555560000000X-002|+1.0000000000000X+000|+1.0000000000000X+3ff|+1.0000000000000X+3ff|+1.0000000000000X+3ff|"}, {1767, "PREC|eg_pc_byp_prop_p3|0|+1.96e32e0000000X-004|+1.1cd2380000000X-003|+1.96e32e0000000X-004|+1.5312a60000000X-001|+1.b6db6e0000000X-002|+1.24924a0000000X-001|"}}},
      {"eg_rank_byp_field", 1045,
       "capture noisily bysort g: egen rk1 = rank(x), field\n"
       "local __rc = _rc\n"
       "sort id\n"
       "local __t \"absent\"\n"
       "capture local __t : type rk1\n"
       "local __f \"absent\"\n"
       "capture local __f : format rk1\n"
       "capture noisily display \"PREC|eg_rank_byp_field|\" `__rc' \"|\" \"`__t'|\" \"`__f'|\" %21x rk1[1] \"|\" %21x rk1[2] \"|\" %21x rk1[3] \"|\" %21x rk1[4] \"|\" %21x rk1[5] \"|\" %21x rk1[6] \"|\" %21x rk1[7] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_rank_byp_field|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture noisily display \"PREC|eg_rank_byp_field_p2|\" `__rc' \"|\" %21x rk1[8] \"|\" %21x rk1[9] \"|\" %21x rk1[10] \"|\" %21x rk1[11] \"|\" %21x rk1[12] \"|\" %21x rk1[13] \"|\" %21x rk1[14] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_rank_byp_field_p2|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture noisily display \"PREC|eg_rank_byp_field_p3|\" `__rc' \"|\" %21x rk1[15] \"|\" %21x rk1[16] \"|\" %21x rk1[17] \"|\" %21x rk1[18] \"|\" %21x rk1[19] \"|\" %21x rk1[20] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_rank_byp_field_p3|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture drop rk1\n",
       {{2230, "PREC|eg_rank_byp_field|0|float|%9.0g|+1.0000000000000X+001|+1.0000000000000X+002|+1.0000000000000X+001|+1.0000000000000X+3ff|+1.4000000000000X+002|+1.0000000000000X+000|+1.0000000000000X+000|"}, {2235, "PREC|eg_rank_byp_field_p2|0|+1.0000000000000X+000|+1.0000000000000X+3ff|+1.8000000000000X+001|+1.0000000000000X+000|+1.0000000000000X+3ff|+1.0000000000000X+3ff|+1.0000000000000X+3ff|"}, {2240, "PREC|eg_rank_byp_field_p3|0|+1.8000000000000X+001|+1.0000000000000X+001|+1.8000000000000X+001|+1.0000000000000X+000|+1.0000000000000X+001|+1.0000000000000X+000|"}}},
      {"eg_rank_byp_unique", 1061,
       "capture noisily bys g: egen rk2 = rank(x), unique\n"
       "local __rc = _rc\n"
       "sort id\n"
       "local __t \"absent\"\n"
       "capture local __t : type rk2\n"
       "local __f \"absent\"\n"
       "capture local __f : format rk2\n"
       "capture noisily display \"PREC|eg_rank_byp_unique|\" `__rc' \"|\" \"`__t'|\" \"`__f'|\" %21x (rk2[1]+rk2[3]) \"|\" %21x rk2[2] \"|\" %21x (rk2[1]*rk2[3]) \"|\" %21x rk2[4] \"|\" %21x rk2[5] \"|\" %21x rk2[6] \"|\" %21x (rk2[7]+rk2[8]) \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_rank_byp_unique|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture noisily display \"PREC|eg_rank_byp_unique_p2|\" `__rc' \"|\" %21x (rk2[7]*rk2[8]) \"|\" %21x rk2[9] \"|\" %21x rk2[10] \"|\" %21x rk2[11] \"|\" %21x rk2[12] \"|\" %21x rk2[13] \"|\" %21x rk2[14] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_rank_byp_unique_p2|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture noisily display \"PREC|eg_rank_byp_unique_p3|\" `__rc' \"|\" %21x (rk2[15]+rk2[17]) \"|\" %21x rk2[16] \"|\" %21x (rk2[15]*rk2[17]) \"|\" %21x rk2[18] \"|\" %21x rk2[19] \"|\" %21x rk2[20] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_rank_byp_unique_p3|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture drop rk2\n",
       {{2264, "PREC|eg_rank_byp_unique|0|float|%9.0g|+1.c000000000000X+002|+1.0000000000000X+001|+1.8000000000000X+003|+1.0000000000000X+3ff|+1.0000000000000X+000|+1.4000000000000X+002|+1.4000000000000X+002|"}, {2269, "PREC|eg_rank_byp_unique_p2|0|+1.8000000000000X+002|+1.0000000000000X+3ff|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+3ff|+1.0000000000000X+3ff|+1.0000000000000X+3ff|"}, {2274, "PREC|eg_rank_byp_unique_p3|0|+1.8000000000000X+001|+1.8000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+002|+1.0000000000000X+000|+1.0000000000000X+001|"}}},
      {"eg_rank_byp_track", 1077,
       "capture noisily bysort g: egen rk3 = rank(x), track\n"
       "local __rc = _rc\n"
       "sort id\n"
       "local __t \"absent\"\n"
       "capture local __t : type rk3\n"
       "local __f \"absent\"\n"
       "capture local __f : format rk3\n"
       "capture noisily display \"PREC|eg_rank_byp_track|\" `__rc' \"|\" \"`__t'|\" \"`__f'|\" %21x rk3[1] \"|\" %21x rk3[2] \"|\" %21x rk3[3] \"|\" %21x rk3[4] \"|\" %21x rk3[5] \"|\" %21x rk3[6] \"|\" %21x rk3[7] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_rank_byp_track|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture noisily display \"PREC|eg_rank_byp_track_p2|\" `__rc' \"|\" %21x rk3[8] \"|\" %21x rk3[9] \"|\" %21x rk3[10] \"|\" %21x rk3[11] \"|\" %21x rk3[12] \"|\" %21x rk3[13] \"|\" %21x rk3[14] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_rank_byp_track_p2|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture noisily display \"PREC|eg_rank_byp_track_p3|\" `__rc' \"|\" %21x rk3[15] \"|\" %21x rk3[16] \"|\" %21x rk3[17] \"|\" %21x rk3[18] \"|\" %21x rk3[19] \"|\" %21x rk3[20] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_rank_byp_track_p3|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture drop rk3\n",
       {{2298, "PREC|eg_rank_byp_track|0|float|%9.0g|+1.8000000000000X+001|+1.0000000000000X+001|+1.8000000000000X+001|+1.0000000000000X+3ff|+1.0000000000000X+000|+1.4000000000000X+002|+1.0000000000000X+001|"}, {2303, "PREC|eg_rank_byp_track_p2|0|+1.0000000000000X+001|+1.0000000000000X+3ff|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+3ff|+1.0000000000000X+3ff|+1.0000000000000X+3ff|"}, {2308, "PREC|eg_rank_byp_track_p3|0|+1.0000000000000X+000|+1.8000000000000X+001|+1.0000000000000X+000|+1.0000000000000X+002|+1.0000000000000X+000|+1.0000000000000X+001|"}}},
      {"eg_rank_byp_str", 1109,
       "capture noisily bys s: egen rk5 = rank(xd), field\n"
       "local __rc = _rc\n"
       "sort id\n"
       "local __t \"absent\"\n"
       "capture local __t : type rk5\n"
       "local __f \"absent\"\n"
       "capture local __f : format rk5\n"
       "capture noisily display \"PREC|eg_rank_byp_str|\" `__rc' \"|\" \"`__t'|\" \"`__f'|\" %21x rk5[1] \"|\" %21x rk5[2] \"|\" %21x rk5[3] \"|\" %21x rk5[4] \"|\" %21x rk5[5] \"|\" %21x rk5[6] \"|\" %21x rk5[7] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_rank_byp_str|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture noisily display \"PREC|eg_rank_byp_str_p2|\" `__rc' \"|\" %21x rk5[8] \"|\" %21x rk5[9] \"|\" %21x rk5[10] \"|\" %21x rk5[11] \"|\" %21x rk5[12] \"|\" %21x rk5[13] \"|\" %21x rk5[14] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_rank_byp_str_p2|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture noisily display \"PREC|eg_rank_byp_str_p3|\" `__rc' \"|\" %21x rk5[15] \"|\" %21x rk5[16] \"|\" %21x rk5[17] \"|\" %21x rk5[18] \"|\" %21x rk5[19] \"|\" %21x rk5[20] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_rank_byp_str_p3|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture drop rk5\n",
       {{2366, "PREC|eg_rank_byp_str|0|float|%9.0g|+1.0000000000000X+001|+1.0000000000000X+000|+1.0000000000000X+001|+1.0000000000000X+3ff|+1.0000000000000X+001|+1.0000000000000X+000|+1.0000000000000X+000|"}, {2371, "PREC|eg_rank_byp_str_p2|0|+1.0000000000000X+000|+1.0000000000000X+3ff|+1.8000000000000X+001|+1.0000000000000X+000|+1.0000000000000X+3ff|+1.0000000000000X+3ff|+1.0000000000000X+3ff|"}, {2376, "PREC|eg_rank_byp_str_p3|0|+1.8000000000000X+001|+1.0000000000000X+001|+1.8000000000000X+001|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|"}}},
      {"eg_rank_byp_if", 1125,
       "capture noisily bys g: egen rk6 = rank(x) if ex==0, track\n"
       "local __rc = _rc\n"
       "sort id\n"
       "local __t \"absent\"\n"
       "capture local __t : type rk6\n"
       "local __f \"absent\"\n"
       "capture local __f : format rk6\n"
       "capture noisily display \"PREC|eg_rank_byp_if|\" `__rc' \"|\" \"`__t'|\" \"`__f'|\" %21x rk6[1] \"|\" %21x rk6[2] \"|\" %21x rk6[3] \"|\" %21x rk6[4] \"|\" %21x rk6[5] \"|\" %21x rk6[6] \"|\" %21x rk6[7] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_rank_byp_if|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture noisily display \"PREC|eg_rank_byp_if_p2|\" `__rc' \"|\" %21x rk6[8] \"|\" %21x rk6[9] \"|\" %21x rk6[10] \"|\" %21x rk6[11] \"|\" %21x rk6[12] \"|\" %21x rk6[13] \"|\" %21x rk6[14] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_rank_byp_if_p2|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture noisily display \"PREC|eg_rank_byp_if_p3|\" `__rc' \"|\" %21x rk6[15] \"|\" %21x rk6[16] \"|\" %21x rk6[17] \"|\" %21x rk6[18] \"|\" %21x rk6[19] \"|\" %21x rk6[20] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_rank_byp_if_p3|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture drop rk6\n",
       {{2400, "PREC|eg_rank_byp_if|0|float|%9.0g|+1.0000000000000X+001|+1.0000000000000X+3ff|+1.0000000000000X+001|+1.0000000000000X+3ff|+1.0000000000000X+000|+1.0000000000000X+002|+1.0000000000000X+001|"}, {2405, "PREC|eg_rank_byp_if_p2|0|+1.0000000000000X+001|+1.0000000000000X+3ff|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+3ff|+1.0000000000000X+3ff|+1.0000000000000X+3ff|"}, {2410, "PREC|eg_rank_byp_if_p3|0|+1.0000000000000X+3ff|+1.0000000000000X+3ff|+1.0000000000000X+3ff|+1.0000000000000X+3ff|+1.0000000000000X+000|+1.0000000000000X+001|"}}},
      {"eg_rank_byopt_track", 1141,
       "capture noisily egen rk7 = rank(x), by(g) track\n"
       "local __rc = _rc\n"
       "sort id\n"
       "local __t \"absent\"\n"
       "capture local __t : type rk7\n"
       "local __f \"absent\"\n"
       "capture local __f : format rk7\n"
       "capture noisily display \"PREC|eg_rank_byopt_track|\" `__rc' \"|\" \"`__t'|\" \"`__f'|\" %21x rk7[1] \"|\" %21x rk7[2] \"|\" %21x rk7[3] \"|\" %21x rk7[4] \"|\" %21x rk7[5] \"|\" %21x rk7[6] \"|\" %21x rk7[7] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_rank_byopt_track|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture noisily display \"PREC|eg_rank_byopt_track_p2|\" `__rc' \"|\" %21x rk7[8] \"|\" %21x rk7[9] \"|\" %21x rk7[10] \"|\" %21x rk7[11] \"|\" %21x rk7[12] \"|\" %21x rk7[13] \"|\" %21x rk7[14] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_rank_byopt_track_p2|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture noisily display \"PREC|eg_rank_byopt_track_p3|\" `__rc' \"|\" %21x rk7[15] \"|\" %21x rk7[16] \"|\" %21x rk7[17] \"|\" %21x rk7[18] \"|\" %21x rk7[19] \"|\" %21x rk7[20] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_rank_byopt_track_p3|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture drop rk7\n",
       {{2434, "PREC|eg_rank_byopt_track|0|float|%9.0g|+1.8000000000000X+001|+1.0000000000000X+001|+1.8000000000000X+001|+1.0000000000000X+3ff|+1.0000000000000X+000|+1.4000000000000X+002|+1.0000000000000X+001|"}, {2439, "PREC|eg_rank_byopt_track_p2|0|+1.0000000000000X+001|+1.0000000000000X+3ff|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+3ff|+1.0000000000000X+3ff|+1.0000000000000X+3ff|"}, {2444, "PREC|eg_rank_byopt_track_p3|0|+1.0000000000000X+000|+1.8000000000000X+001|+1.0000000000000X+000|+1.0000000000000X+002|+1.0000000000000X+000|+1.0000000000000X+001|"}}},
      {"eg_rowmax_wild", 1173,
       "capture noisily egen rx2 = rowmax(w*)\n"
       "local __rc = _rc\n"
       "sort id\n"
       "local __t \"absent\"\n"
       "capture local __t : type rx2\n"
       "local __f \"absent\"\n"
       "capture local __f : format rx2\n"
       "capture noisily display \"PREC|eg_rowmax_wild|\" `__rc' \"|\" \"`__t'|\" \"`__f'|\" %21x rx2[1] \"|\" %21x rx2[2] \"|\" %21x rx2[3] \"|\" %21x rx2[4] \"|\" %21x rx2[5] \"|\" %21x rx2[6] \"|\" %21x rx2[7] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_rowmax_wild|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture noisily display \"PREC|eg_rowmax_wild_p2|\" `__rc' \"|\" %21x rx2[8] \"|\" %21x rx2[9] \"|\" %21x rx2[10] \"|\" %21x rx2[11] \"|\" %21x rx2[12] \"|\" %21x rx2[13] \"|\" %21x rx2[14] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_rowmax_wild_p2|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture noisily display \"PREC|eg_rowmax_wild_p3|\" `__rc' \"|\" %21x rx2[15] \"|\" %21x rx2[16] \"|\" %21x rx2[17] \"|\" %21x rx2[18] \"|\" %21x rx2[19] \"|\" %21x rx2[20] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_rowmax_wild_p3|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture drop rx2\n",
       {{2502, "PREC|eg_rowmax_wild|0|float|%9.0g|+1.8000000000000X+001|+1.8000000000000X+001|+1.8000000000000X+001|+1.0000000000000X+3ff|+1.0000000000000X+001|+1.8000000000000X+001|-1.0000000000000X+000|"}, {2507, "PREC|eg_rowmax_wild_p2|0|+1.0000000000000X+002|+1.4000000000000X+002|+1.0000000000000X+003|+1.c000000000000X+002|+1.0000000000000X+003|+1.c000000000000X+001|+0.0000000000000X-3ff|"}, {2512, "PREC|eg_rowmax_wild_p3|0|+1.8000000000000X+001|+1.0000000000000X+001|+1.2000000000000X+003|+1.8000000000000X+001|+1.8000000000000X+001|+1.3333340000000X-002|"}}},
      {"eg_rowmin_wild", 1221,
       "capture noisily egen rn2 = rowmin(w*) if ex==0\n"
       "local __rc = _rc\n"
       "sort id\n"
       "local __t \"absent\"\n"
       "capture local __t : type rn2\n"
       "local __f \"absent\"\n"
       "capture local __f : format rn2\n"
       "capture noisily display \"PREC|eg_rowmin_wild|\" `__rc' \"|\" \"`__t'|\" \"`__f'|\" %21x rn2[1] \"|\" %21x rn2[2] \"|\" %21x rn2[3] \"|\" %21x rn2[4] \"|\" %21x rn2[5] \"|\" %21x rn2[6] \"|\" %21x rn2[7] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_rowmin_wild|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture noisily display \"PREC|eg_rowmin_wild_p2|\" `__rc' \"|\" %21x rn2[8] \"|\" %21x rn2[9] \"|\" %21x rn2[10] \"|\" %21x rn2[11] \"|\" %21x rn2[12] \"|\" %21x rn2[13] \"|\" %21x rn2[14] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_rowmin_wild_p2|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture noisily display \"PREC|eg_rowmin_wild_p3|\" `__rc' \"|\" %21x rn2[15] \"|\" %21x rn2[16] \"|\" %21x rn2[17] \"|\" %21x rn2[18] \"|\" %21x rn2[19] \"|\" %21x rn2[20] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_rowmin_wild_p3|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture drop rn2\n",
       {{2604, "PREC|eg_rowmin_wild|0|float|%9.0g|+1.0000000000000X+000|+1.0000000000000X+3ff|+1.8000000000000X+001|+1.0000000000000X+3ff|+1.0000000000000X+001|+1.0000000000000X+000|-1.0000000000000X+001|"}, {2609, "PREC|eg_rowmin_wild_p2|0|+1.0000000000000X+002|+1.4000000000000X+002|+1.8000000000000X+002|+1.c000000000000X+002|+1.0000000000000X+003|+1.8000000000000X+000|+0.0000000000000X-3ff|"}, {2614, "PREC|eg_rowmin_wild_p3|0|+1.0000000000000X+3ff|+1.0000000000000X+3ff|+1.0000000000000X+3ff|+1.0000000000000X+3ff|+1.0000000000000X+000|+1.99999a0000000X-004|"}}},
      {"eg_rowmiss_if", 1269,
       "capture noisily egen rm3 = rowmiss(w*) if ex==0\n"
       "local __rc = _rc\n"
       "sort id\n"
       "local __t \"absent\"\n"
       "capture local __t : type rm3\n"
       "local __f \"absent\"\n"
       "capture local __f : format rm3\n"
       "capture noisily display \"PREC|eg_rowmiss_if|\" `__rc' \"|\" \"`__t'|\" \"`__f'|\" %21x rm3[1] \"|\" %21x rm3[2] \"|\" %21x rm3[3] \"|\" %21x rm3[4] \"|\" %21x rm3[5] \"|\" %21x rm3[6] \"|\" %21x rm3[7] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_rowmiss_if|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture noisily display \"PREC|eg_rowmiss_if_p2|\" `__rc' \"|\" %21x rm3[8] \"|\" %21x rm3[9] \"|\" %21x rm3[10] \"|\" %21x rm3[11] \"|\" %21x rm3[12] \"|\" %21x rm3[13] \"|\" %21x rm3[14] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_rowmiss_if_p2|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture noisily display \"PREC|eg_rowmiss_if_p3|\" `__rc' \"|\" %21x rm3[15] \"|\" %21x rm3[16] \"|\" %21x rm3[17] \"|\" %21x rm3[18] \"|\" %21x rm3[19] \"|\" %21x rm3[20] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_rowmiss_if_p3|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture drop rm3\n",
       {{2704, "PREC|eg_rowmiss_if|0|float|%9.0g|+0.0000000000000X-3ff|+1.0000000000000X+3ff|+1.0000000000000X+001|+1.8000000000000X+001|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|"}, {2709, "PREC|eg_rowmiss_if_p2|0|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+001|+1.0000000000000X+001|+0.0000000000000X-3ff|+1.0000000000000X+000|"}, {2714, "PREC|eg_rowmiss_if_p3|0|+1.0000000000000X+3ff|+1.0000000000000X+3ff|+1.0000000000000X+3ff|+1.0000000000000X+3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|"}}},
      {"eg_rowsd_if", 1301,
       "capture noisily egen rs2 = rowsd(w*) if ex==0\n"
       "local __rc = _rc\n"
       "sort id\n"
       "local __t \"absent\"\n"
       "capture local __t : type rs2\n"
       "local __f \"absent\"\n"
       "capture local __f : format rs2\n"
       "capture noisily display \"PREC|eg_rowsd_if|\" `__rc' \"|\" \"`__t'|\" \"`__f'|\" %21x rs2[1] \"|\" %21x rs2[2] \"|\" %21x rs2[3] \"|\" %21x rs2[4] \"|\" %21x rs2[5] \"|\" %21x rs2[6] \"|\" %21x rs2[7] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_rowsd_if|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture noisily display \"PREC|eg_rowsd_if_p2|\" `__rc' \"|\" %21x rs2[8] \"|\" %21x rs2[9] \"|\" %21x rs2[10] \"|\" %21x rs2[11] \"|\" %21x rs2[12] \"|\" %21x rs2[13] \"|\" %21x rs2[14] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_rowsd_if_p2|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture noisily display \"PREC|eg_rowsd_if_p3|\" `__rc' \"|\" %21x rs2[15] \"|\" %21x rs2[16] \"|\" %21x rs2[17] \"|\" %21x rs2[18] \"|\" %21x rs2[19] \"|\" %21x rs2[20] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_rowsd_if_p3|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture drop rs2\n",
       {{2772, "PREC|eg_rowsd_if|0|float|%9.0g|+1.0000000000000X+000|+1.0000000000000X+3ff|+1.0000000000000X+3ff|+1.0000000000000X+3ff|+0.0000000000000X-3ff|+1.6a09e60000000X+000|+1.279a740000000X-001|"}, {2777, "PREC|eg_rowsd_if_p2|0|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+3ff|+1.0000000000000X+3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|"}, {2782, "PREC|eg_rowsd_if_p3|0|+1.0000000000000X+3ff|+1.0000000000000X+3ff|+1.0000000000000X+3ff|+1.0000000000000X+3ff|+1.0000000000000X+000|+1.99999c0000000X-004|"}}},
      {"eg_rsum", 1317,
       "capture noisily egen rt1 = rsum(w1 w2 w3)\n"
       "local __rc = _rc\n"
       "sort id\n"
       "local __t \"absent\"\n"
       "capture local __t : type rt1\n"
       "local __f \"absent\"\n"
       "capture local __f : format rt1\n"
       "capture noisily display \"PREC|eg_rsum|\" `__rc' \"|\" \"`__t'|\" \"`__f'|\" %21x rt1[1] \"|\" %21x rt1[2] \"|\" %21x rt1[3] \"|\" %21x rt1[4] \"|\" %21x rt1[5] \"|\" %21x rt1[6] \"|\" %21x rt1[7] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_rsum|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture noisily display \"PREC|eg_rsum_p2|\" `__rc' \"|\" %21x rt1[8] \"|\" %21x rt1[9] \"|\" %21x rt1[10] \"|\" %21x rt1[11] \"|\" %21x rt1[12] \"|\" %21x rt1[13] \"|\" %21x rt1[14] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_rsum_p2|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture noisily display \"PREC|eg_rsum_p3|\" `__rc' \"|\" %21x rt1[15] \"|\" %21x rt1[16] \"|\" %21x rt1[17] \"|\" %21x rt1[18] \"|\" %21x rt1[19] \"|\" %21x rt1[20] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_rsum_p3|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture drop rt1\n",
       {{2805, "PREC|eg_rsum|0|float|%9.0g|+1.8000000000000X+002|+1.4000000000000X+002|+1.8000000000000X+001|+0.0000000000000X-3ff|+1.8000000000000X+002|+1.0000000000000X+002|-1.0000000000000X+002|"}, {2810, "PREC|eg_rsum_p2|0|+1.0000000000000X+003|+1.4000000000000X+003|+1.5000000000000X+004|+1.c000000000000X+002|+1.0000000000000X+003|+1.e000000000000X+002|+0.0000000000000X-3ff|"}, {2815, "PREC|eg_rsum_p3|0|+1.8000000000000X+002|+1.0000000000000X+002|+1.2000000000000X+003|+1.2000000000000X+003|+1.8000000000000X+002|+1.3333340000000X-001|"}}},
      {"eg_rsum_m", 1333,
       "capture noisily egen rt2 = rsum(w1 w2), m\n"
       "local __rc = _rc\n"
       "sort id\n"
       "local __t \"absent\"\n"
       "capture local __t : type rt2\n"
       "local __f \"absent\"\n"
       "capture local __f : format rt2\n"
       "capture noisily display \"PREC|eg_rsum_m|\" `__rc' \"|\" \"`__t'|\" \"`__f'|\" %21x rt2[1] \"|\" %21x rt2[2] \"|\" %21x rt2[3] \"|\" %21x rt2[4] \"|\" %21x rt2[5] \"|\" %21x rt2[6] \"|\" %21x rt2[7] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_rsum_m|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture noisily display \"PREC|eg_rsum_m_p2|\" `__rc' \"|\" %21x rt2[8] \"|\" %21x rt2[9] \"|\" %21x rt2[10] \"|\" %21x rt2[11] \"|\" %21x rt2[12] \"|\" %21x rt2[13] \"|\" %21x rt2[14] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_rsum_m_p2|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture noisily display \"PREC|eg_rsum_m_p3|\" `__rc' \"|\" %21x rt2[15] \"|\" %21x rt2[16] \"|\" %21x rt2[17] \"|\" %21x rt2[18] \"|\" %21x rt2[19] \"|\" %21x rt2[20] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_rsum_m_p3|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture drop rt2\n",
       {{2839, "PREC|eg_rsum_m|0|float|%9.0g|+1.8000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+3ff|+1.0000000000000X+3ff|+1.0000000000000X+002|+1.0000000000000X+002|-1.0000000000000X+001|"}, {2844, "PREC|eg_rsum_m_p2|0|+1.0000000000000X+002|+1.4000000000000X+003|+1.a000000000000X+003|+1.c000000000000X+002|+1.0000000000000X+003|+1.0000000000000X+002|+0.0000000000000X-3ff|"}, {2849, "PREC|eg_rsum_m_p3|0|+1.4000000000000X+002|+1.0000000000000X+001|+1.0000000000000X+3ff|+1.8000000000000X+002|+1.8000000000000X+001|+1.3333340000000X-002|"}}},
      {"eg_rsum_wild", 1349,
       "capture noisily egen rt3 = rsum(w*)\n"
       "local __rc = _rc\n"
       "sort id\n"
       "local __t \"absent\"\n"
       "capture local __t : type rt3\n"
       "local __f \"absent\"\n"
       "capture local __f : format rt3\n"
       "capture noisily display \"PREC|eg_rsum_wild|\" `__rc' \"|\" \"`__t'|\" \"`__f'|\" %21x rt3[1] \"|\" %21x rt3[2] \"|\" %21x rt3[3] \"|\" %21x rt3[4] \"|\" %21x rt3[5] \"|\" %21x rt3[6] \"|\" %21x rt3[7] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_rsum_wild|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture noisily display \"PREC|eg_rsum_wild_p2|\" `__rc' \"|\" %21x rt3[8] \"|\" %21x rt3[9] \"|\" %21x rt3[10] \"|\" %21x rt3[11] \"|\" %21x rt3[12] \"|\" %21x rt3[13] \"|\" %21x rt3[14] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_rsum_wild_p2|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture noisily display \"PREC|eg_rsum_wild_p3|\" `__rc' \"|\" %21x rt3[15] \"|\" %21x rt3[16] \"|\" %21x rt3[17] \"|\" %21x rt3[18] \"|\" %21x rt3[19] \"|\" %21x rt3[20] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_rsum_wild_p3|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture drop rt3\n",
       {{2872, "PREC|eg_rsum_wild|0|float|%9.0g|+1.8000000000000X+002|+1.4000000000000X+002|+1.8000000000000X+001|+0.0000000000000X-3ff|+1.8000000000000X+002|+1.0000000000000X+002|-1.0000000000000X+002|"}, {2877, "PREC|eg_rsum_wild_p2|0|+1.0000000000000X+003|+1.4000000000000X+003|+1.5000000000000X+004|+1.c000000000000X+002|+1.0000000000000X+003|+1.e000000000000X+002|+0.0000000000000X-3ff|"}, {2882, "PREC|eg_rsum_wild_p3|0|+1.8000000000000X+002|+1.0000000000000X+002|+1.2000000000000X+003|+1.2000000000000X+003|+1.8000000000000X+002|+1.3333340000000X-001|"}}},
      {"eg_rsum_if", 1365,
       "capture noisily egen double rt4 = rsum(w1 w3) if ex==0\n"
       "local __rc = _rc\n"
       "sort id\n"
       "local __t \"absent\"\n"
       "capture local __t : type rt4\n"
       "local __f \"absent\"\n"
       "capture local __f : format rt4\n"
       "capture noisily display \"PREC|eg_rsum_if|\" `__rc' \"|\" \"`__t'|\" \"`__f'|\" %21x rt4[1] \"|\" %21x rt4[2] \"|\" %21x rt4[3] \"|\" %21x rt4[4] \"|\" %21x rt4[5] \"|\" %21x rt4[6] \"|\" %21x rt4[7] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_rsum_if|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture noisily display \"PREC|eg_rsum_if_p2|\" `__rc' \"|\" %21x rt4[8] \"|\" %21x rt4[9] \"|\" %21x rt4[10] \"|\" %21x rt4[11] \"|\" %21x rt4[12] \"|\" %21x rt4[13] \"|\" %21x rt4[14] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_rsum_if_p2|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture noisily display \"PREC|eg_rsum_if_p3|\" `__rc' \"|\" %21x rt4[15] \"|\" %21x rt4[16] \"|\" %21x rt4[17] \"|\" %21x rt4[18] \"|\" %21x rt4[19] \"|\" %21x rt4[20] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_rsum_if_p3|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture drop rt4\n",
       {{2906, "PREC|eg_rsum_if|0|double|%10.0g|+1.0000000000000X+002|+1.0000000000000X+3ff|+1.8000000000000X+001|+0.0000000000000X-3ff|+1.0000000000000X+002|+1.8000000000000X+001|-1.8000000000000X+001|"}, {2911, "PREC|eg_rsum_if_p2|0|+1.0000000000000X+003|+1.4000000000000X+002|+1.c000000000000X+003|+1.c000000000000X+002|+0.0000000000000X-3ff|+1.4000000000000X+002|+0.0000000000000X-3ff|"}, {2916, "PREC|eg_rsum_if_p3|0|+1.0000000000000X+3ff|+1.0000000000000X+3ff|+1.0000000000000X+3ff|+1.0000000000000X+3ff|+1.0000000000000X+002|+1.99999a8000000X-002|"}}},
      {"eg_seq_plain_rev", 1542,
       "capture noisily egen sq6 = seq(), from(5) to(1) block(2)\n"
       "local __rc = _rc\n"
       "sort id\n"
       "local __t \"absent\"\n"
       "capture local __t : type sq6\n"
       "local __f \"absent\"\n"
       "capture local __f : format sq6\n"
       "capture noisily display \"PREC|eg_seq_plain_rev|\" `__rc' \"|\" \"`__t'|\" \"`__f'|\" %21x sq6[1] \"|\" %21x sq6[2] \"|\" %21x sq6[3] \"|\" %21x sq6[4] \"|\" %21x sq6[5] \"|\" %21x sq6[6] \"|\" %21x sq6[7] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_seq_plain_rev|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture noisily display \"PREC|eg_seq_plain_rev_p2|\" `__rc' \"|\" %21x sq6[8] \"|\" %21x sq6[9] \"|\" %21x sq6[10] \"|\" %21x sq6[11] \"|\" %21x sq6[12] \"|\" %21x sq6[13] \"|\" %21x sq6[14] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_seq_plain_rev_p2|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture noisily display \"PREC|eg_seq_plain_rev_p3|\" `__rc' \"|\" %21x sq6[15] \"|\" %21x sq6[16] \"|\" %21x sq6[17] \"|\" %21x sq6[18] \"|\" %21x sq6[19] \"|\" %21x sq6[20] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_seq_plain_rev_p3|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture drop sq6\n",
       {{3277, "PREC|eg_seq_plain_rev|0|byte|%8.0g|+1.4000000000000X+002|+1.4000000000000X+002|+1.0000000000000X+002|+1.0000000000000X+002|+1.8000000000000X+001|+1.8000000000000X+001|+1.0000000000000X+001|"}, {3282, "PREC|eg_seq_plain_rev_p2|0|+1.0000000000000X+001|+1.0000000000000X+000|+1.0000000000000X+000|+1.4000000000000X+002|+1.4000000000000X+002|+1.0000000000000X+002|+1.0000000000000X+002|"}, {3287, "PREC|eg_seq_plain_rev_p3|0|+1.8000000000000X+001|+1.8000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+000|+1.0000000000000X+000|"}}},
      {"eg_std_plain_ms", 1654,
       "capture noisily egen float st5 = std(x), mean(0) std(1)\n"
       "local __rc = _rc\n"
       "sort id\n"
       "local __t \"absent\"\n"
       "capture local __t : type st5\n"
       "local __f \"absent\"\n"
       "capture local __f : format st5\n"
       "capture noisily display \"PREC|eg_std_plain_ms|\" `__rc' \"|\" \"`__t'|\" \"`__f'|\" %21x st5[1] \"|\" %21x st5[2] \"|\" %21x st5[3] \"|\" %21x st5[4] \"|\" %21x st5[5] \"|\" %21x st5[6] \"|\" %21x st5[7] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_std_plain_ms|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture noisily display \"PREC|eg_std_plain_ms_p2|\" `__rc' \"|\" %21x st5[8] \"|\" %21x st5[9] \"|\" %21x st5[10] \"|\" %21x st5[11] \"|\" %21x st5[12] \"|\" %21x st5[13] \"|\" %21x st5[14] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_std_plain_ms_p2|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture noisily display \"PREC|eg_std_plain_ms_p3|\" `__rc' \"|\" %21x st5[15] \"|\" %21x st5[16] \"|\" %21x st5[17] \"|\" %21x st5[18] \"|\" %21x st5[19] \"|\" %21x st5[20] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_std_plain_ms_p3|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture drop st5\n",
       {{3513, "PREC|eg_std_plain_ms|0|float|%9.0g|-1.a9df320000000X-003|-1.b063ec0000000X-001|-1.a9df320000000X-003|+1.0000000000000X+3ff|-1.e147560000000X-001|+1.b6e8a40000000X-002|+1.c3f2180000000X-004|"}, {3518, "PREC|eg_std_plain_ms_p2|0|+1.c3f2180000000X-004|+1.0000000000000X+3ff|-1.cca30e0000000X+000|+1.10b0380000000X+000|+1.0000000000000X+3ff|+1.0000000000000X+3ff|+1.0000000000000X+3ff|"}, {3523, "PREC|eg_std_plain_ms_p3|0|-1.5ee8e40000000X-001|-1.fa441e0000000X-002|-1.5ee8e40000000X-001|+1.0290a80000000X+001|+1.7e6a620000000X-001|+1.622b400000000X+000|"}}},
      {"eg_std_plain_ms2", 1670,
       "capture noisily egen st6 = std(xd), mean(10) std(2)\n"
       "local __rc = _rc\n"
       "sort id\n"
       "local __t \"absent\"\n"
       "capture local __t : type st6\n"
       "local __f \"absent\"\n"
       "capture local __f : format st6\n"
       "capture noisily display \"PREC|eg_std_plain_ms2|\" `__rc' \"|\" \"`__t'|\" \"`__f'|\" %21x st6[1] \"|\" %21x st6[2] \"|\" %21x st6[3] \"|\" %21x st6[4] \"|\" %21x st6[5] \"|\" %21x st6[6] \"|\" %21x st6[7] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_std_plain_ms2|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture noisily display \"PREC|eg_std_plain_ms2_p2|\" `__rc' \"|\" %21x st6[8] \"|\" %21x st6[9] \"|\" %21x st6[10] \"|\" %21x st6[11] \"|\" %21x st6[12] \"|\" %21x st6[13] \"|\" %21x st6[14] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_std_plain_ms2_p2|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture noisily display \"PREC|eg_std_plain_ms2_p3|\" `__rc' \"|\" %21x st6[15] \"|\" %21x st6[16] \"|\" %21x st6[17] \"|\" %21x st6[18] \"|\" %21x st6[19] \"|\" %21x st6[20] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_std_plain_ms2_p3|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture drop st6\n",
       {{3547, "PREC|eg_std_plain_ms2|0|float|%9.0g|+1.ea4d320000000X+002|+1.fbfb820000000X+002|+1.ea4d320000000X+002|+1.0000000000000X+3ff|+1.18833a0000000X+003|+1.215a620000000X+003|+1.2a318c0000000X+003|"}, {3552, "PREC|eg_std_plain_ms2_p2|0|+1.2a318c0000000X+003|+1.0000000000000X+3ff|+1.c6f0900000000X+002|+1.4d8e2c0000000X+003|+1.0000000000000X+3ff|+1.0000000000000X+3ff|+1.0000000000000X+3ff|"}, {3557, "PREC|eg_std_plain_ms2_p3|0|+1.70eace0000000X+003|+1.79c1f60000000X+003|+1.70eace0000000X+003|+1.8b70480000000X+003|+1.9447700000000X+003|+1.9d1e980000000X+003|"}}},
      {"eg_sum_byp_miss", 1798,
       "sort g id\n"
       "capture noisily by g: egen sm6 = sum(x), missing\n"
       "local __rc = _rc\n"
       "sort id\n"
       "local __t \"absent\"\n"
       "capture local __t : type sm6\n"
       "local __f \"absent\"\n"
       "capture local __f : format sm6\n"
       "capture noisily display \"PREC|eg_sum_byp_miss|\" `__rc' \"|\" \"`__t'|\" \"`__f'|\" %21x sm6[1] \"|\" %21x sm6[2] \"|\" %21x sm6[3] \"|\" %21x sm6[4] \"|\" %21x sm6[5] \"|\" %21x sm6[6] \"|\" %21x sm6[7] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_sum_byp_miss|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture noisily display \"PREC|eg_sum_byp_miss_p2|\" `__rc' \"|\" %21x sm6[8] \"|\" %21x sm6[9] \"|\" %21x sm6[10] \"|\" %21x sm6[11] \"|\" %21x sm6[12] \"|\" %21x sm6[13] \"|\" %21x sm6[14] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_sum_byp_miss_p2|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture noisily display \"PREC|eg_sum_byp_miss_p3|\" `__rc' \"|\" %21x sm6[15] \"|\" %21x sm6[16] \"|\" %21x sm6[17] \"|\" %21x sm6[18] \"|\" %21x sm6[19] \"|\" %21x sm6[20] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_sum_byp_miss_p3|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture drop sm6\n",
       {{3817, "PREC|eg_sum_byp_miss|0|float|%9.0g|+1.9666660000000X+003|+1.9666660000000X+003|+1.9666660000000X+003|+1.9666660000000X+003|+1.9666660000000X+003|+1.9666660000000X+003|+1.8000000000000X+002|"}, {3822, "PREC|eg_sum_byp_miss_p2|0|+1.8000000000000X+002|+1.8000000000000X+002|+1.8000000000000X+002|+1.c000000000000X+002|+1.0000000000000X+3ff|+1.0000000000000X+3ff|+1.0000000000000X+3ff|"}, {3827, "PREC|eg_sum_byp_miss_p3|0|+1.e333340000000X+003|+1.e333340000000X+003|+1.e333340000000X+003|+1.e333340000000X+003|+1.c000000000000X+003|+1.c000000000000X+003|"}}},
      {"eg_total_byp_miss", 1911,
       "capture noisily bysort g: egen tt1 = total(x), missing\n"
       "local __rc = _rc\n"
       "sort id\n"
       "local __t \"absent\"\n"
       "capture local __t : type tt1\n"
       "local __f \"absent\"\n"
       "capture local __f : format tt1\n"
       "capture noisily display \"PREC|eg_total_byp_miss|\" `__rc' \"|\" \"`__t'|\" \"`__f'|\" %21x tt1[1] \"|\" %21x tt1[2] \"|\" %21x tt1[3] \"|\" %21x tt1[4] \"|\" %21x tt1[5] \"|\" %21x tt1[6] \"|\" %21x tt1[7] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_total_byp_miss|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture noisily display \"PREC|eg_total_byp_miss_p2|\" `__rc' \"|\" %21x tt1[8] \"|\" %21x tt1[9] \"|\" %21x tt1[10] \"|\" %21x tt1[11] \"|\" %21x tt1[12] \"|\" %21x tt1[13] \"|\" %21x tt1[14] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_total_byp_miss_p2|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture noisily display \"PREC|eg_total_byp_miss_p3|\" `__rc' \"|\" %21x tt1[15] \"|\" %21x tt1[16] \"|\" %21x tt1[17] \"|\" %21x tt1[18] \"|\" %21x tt1[19] \"|\" %21x tt1[20] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_total_byp_miss_p3|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture drop tt1\n",
       {{4052, "PREC|eg_total_byp_miss|0|float|%9.0g|+1.9666660000000X+003|+1.9666660000000X+003|+1.9666660000000X+003|+1.9666660000000X+003|+1.9666660000000X+003|+1.9666660000000X+003|+1.8000000000000X+002|"}, {4057, "PREC|eg_total_byp_miss_p2|0|+1.8000000000000X+002|+1.8000000000000X+002|+1.8000000000000X+002|+1.c000000000000X+002|+1.0000000000000X+3ff|+1.0000000000000X+3ff|+1.0000000000000X+3ff|"}, {4062, "PREC|eg_total_byp_miss_p3|0|+1.e333340000000X+003|+1.e333340000000X+003|+1.e333340000000X+003|+1.e333340000000X+003|+1.c000000000000X+003|+1.c000000000000X+003|"}}},
      {"eg_total_plain_miss", 1927,
       "capture noisily egen tt2 = total(xd) if g==4, missing\n"
       "local __rc = _rc\n"
       "sort id\n"
       "local __t \"absent\"\n"
       "capture local __t : type tt2\n"
       "local __f \"absent\"\n"
       "capture local __f : format tt2\n"
       "capture noisily display \"PREC|eg_total_plain_miss|\" `__rc' \"|\" \"`__t'|\" \"`__f'|\" %21x tt2[1] \"|\" %21x tt2[2] \"|\" %21x tt2[3] \"|\" %21x tt2[4] \"|\" %21x tt2[5] \"|\" %21x tt2[6] \"|\" %21x tt2[7] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_total_plain_miss|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture noisily display \"PREC|eg_total_plain_miss_p2|\" `__rc' \"|\" %21x tt2[8] \"|\" %21x tt2[9] \"|\" %21x tt2[10] \"|\" %21x tt2[11] \"|\" %21x tt2[12] \"|\" %21x tt2[13] \"|\" %21x tt2[14] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_total_plain_miss_p2|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture noisily display \"PREC|eg_total_plain_miss_p3|\" `__rc' \"|\" %21x tt2[15] \"|\" %21x tt2[16] \"|\" %21x tt2[17] \"|\" %21x tt2[18] \"|\" %21x tt2[19] \"|\" %21x tt2[20] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_total_plain_miss_p3|rc=`__rc'|display_rc=\" _rc \"|\"\n"
       "capture drop tt2\n",
       {{4086, "PREC|eg_total_plain_miss|0|float|%9.0g|+1.0000000000000X+3ff|+1.0000000000000X+3ff|+1.0000000000000X+3ff|+1.0000000000000X+3ff|+1.0000000000000X+3ff|+1.0000000000000X+3ff|+1.0000000000000X+3ff|"}, {4091, "PREC|eg_total_plain_miss_p2|0|+1.0000000000000X+3ff|+1.0000000000000X+3ff|+1.0000000000000X+3ff|+1.0000000000000X+3ff|+1.0000000000000X+3ff|+1.0000000000000X+3ff|+1.0000000000000X+3ff|"}, {4096, "PREC|eg_total_plain_miss_p3|0|+1.0000000000000X+3ff|+1.0000000000000X+3ff|+1.0000000000000X+3ff|+1.0000000000000X+3ff|+1.0000000000000X+3ff|+1.0000000000000X+3ff|"}}},
      {"eg_ref_unknown", 1943,
       "capture noisily egen z1 = nosuchfn(x)\n"
       "local __rc = _rc\n"
       "sort id\n"
       "local __e1 = 0\n"
       "capture confirm variable z1, exact\n"
       "if _rc == 0 local __e1 = 1\n"
       "capture noisily display \"PREC|eg_ref_unknown|\" `__rc' \"|\" \"`__e1'|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_ref_unknown|rc=`__rc'|display_rc=\" _rc \"|\"\n",
       {{4118, "PREC|eg_ref_unknown|133|0|"}}},
      {"eg_ref_strarg", 1953,
       "capture noisily egen z2 = sum(s)\n"
       "local __rc = _rc\n"
       "sort id\n"
       "local __e1 = 0\n"
       "capture confirm variable z2, exact\n"
       "if _rc == 0 local __e1 = 1\n"
       "capture noisily display \"PREC|eg_ref_strarg|\" `__rc' \"|\" \"`__e1'|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_ref_strarg|rc=`__rc'|display_rc=\" _rc \"|\"\n",
       {{4138, "PREC|eg_ref_strarg|109|0|"}}},
      {"eg_ref_strarg_by", 1963,
       "capture noisily bys g: egen z3 = sd(s)\n"
       "local __rc = _rc\n"
       "sort id\n"
       "local __e1 = 0\n"
       "capture confirm variable z3, exact\n"
       "if _rc == 0 local __e1 = 1\n"
       "capture noisily display \"PREC|eg_ref_strarg_by|\" `__rc' \"|\" \"`__e1'|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_ref_strarg_by|rc=`__rc'|display_rc=\" _rc \"|\"\n",
       {{4158, "PREC|eg_ref_strarg_by|109|0|"}}},
      {"eg_ref_badopt", 1973,
       "capture noisily egen z4 = sd(x), p(50)\n"
       "local __rc = _rc\n"
       "sort id\n"
       "local __e1 = 0\n"
       "capture confirm variable z4, exact\n"
       "if _rc == 0 local __e1 = 1\n"
       "capture noisily display \"PREC|eg_ref_badopt|\" `__rc' \"|\" \"`__e1'|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_ref_badopt|rc=`__rc'|display_rc=\" _rc \"|\"\n",
       {{4178, "PREC|eg_ref_badopt|198|0|"}}},
      {"eg_ref_rank_two", 1983,
       "capture noisily bys g: egen z5 = rank(x), field track\n"
       "local __rc = _rc\n"
       "sort id\n"
       "local __e1 = 0\n"
       "capture confirm variable z5, exact\n"
       "if _rc == 0 local __e1 = 1\n"
       "capture noisily display \"PREC|eg_ref_rank_two|\" `__rc' \"|\" \"`__e1'|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_ref_rank_two|rc=`__rc'|display_rc=\" _rc \"|\"\n",
       {{4198, "PREC|eg_ref_rank_two|198|0|"}}},
      {"eg_ref_mode_two", 1993,
       "capture noisily egen z6 = mode(k), by(g) minmode maxmode\n"
       "local __rc = _rc\n"
       "sort id\n"
       "local __e1 = 0\n"
       "capture confirm variable z6, exact\n"
       "if _rc == 0 local __e1 = 1\n"
       "capture noisily display \"PREC|eg_ref_mode_two|\" `__rc' \"|\" \"`__e1'|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_ref_mode_two|rc=`__rc'|display_rc=\" _rc \"|\"\n",
       {{4218, "PREC|eg_ref_mode_two|198|0|"}}},
      {"eg_ref_row_by", 2003,
       "capture noisily egen z7 = rowmax(w1 w2), by(g)\n"
       "local __rc = _rc\n"
       "sort id\n"
       "local __e1 = 0\n"
       "capture confirm variable z7, exact\n"
       "if _rc == 0 local __e1 = 1\n"
       "capture noisily display \"PREC|eg_ref_row_by|\" `__rc' \"|\" \"`__e1'|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_ref_row_by|rc=`__rc'|display_rc=\" _rc \"|\"\n",
       {{4238, "PREC|eg_ref_row_by|190|0|"}}},
      {"eg_ref_rowsd_one", 2013,
       "capture noisily egen z8 = rowsd(w1)\n"
       "local __rc = _rc\n"
       "sort id\n"
       "local __e1 = 0\n"
       "capture confirm variable z8, exact\n"
       "if _rc == 0 local __e1 = 1\n"
       "capture noisily display \"PREC|eg_ref_rowsd_one|\" `__rc' \"|\" \"`__e1'|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_ref_rowsd_one|rc=`__rc'|display_rc=\" _rc \"|\"\n",
       {{4258, "PREC|eg_ref_rowsd_one|102|0|"}}},
      {"eg_ref_seq_block", 2023,
       "capture noisily egen z9 = seq(), block(0)\n"
       "local __rc = _rc\n"
       "sort id\n"
       "local __e1 = 0\n"
       "capture confirm variable z9, exact\n"
       "if _rc == 0 local __e1 = 1\n"
       "capture noisily display \"PREC|eg_ref_seq_block|\" `__rc' \"|\" \"`__e1'|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_ref_seq_block|rc=`__rc'|display_rc=\" _rc \"|\"\n",
       {{4278, "PREC|eg_ref_seq_block|498|0|"}}},
      {"eg_ref_pc_badopt", 2033,
       "capture noisily egen z10 = pc(x), missing\n"
       "local __rc = _rc\n"
       "sort id\n"
       "local __e1 = 0\n"
       "capture confirm variable z10, exact\n"
       "if _rc == 0 local __e1 = 1\n"
       "capture noisily display \"PREC|eg_ref_pc_badopt|\" `__rc' \"|\" \"`__e1'|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_ref_pc_badopt|rc=`__rc'|display_rc=\" _rc \"|\"\n",
       {{4298, "PREC|eg_ref_pc_badopt|198|0|"}}},
      {"eg_ref_mode_num", 2043,
       "capture noisily egen z11 = mode(k), by(g) nummode(5)\n"
       "local __rc = _rc\n"
       "sort id\n"
       "local __e1 = 0\n"
       "capture confirm variable z11, exact\n"
       "if _rc == 0 local __e1 = 1\n"
       "capture noisily display \"PREC|eg_ref_mode_num|\" `__rc' \"|\" \"`__e1'|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_ref_mode_num|rc=`__rc'|display_rc=\" _rc \"|\"\n",
       {{4320, "PREC|eg_ref_mode_num|198|0|"}}},
      {"eg_ref_exist", 2053,
       "capture noisily egen x = sum(xd)\n"
       "local __rc = _rc\n"
       "sort id\n"
       "local __e1 \"absent\"\n"
       "capture local __e1 : type x\n"
       "capture noisily display \"PREC|eg_ref_exist|\" `__rc' \"|\" \"`__e1'|\" %21x x[1] \"|\" %21x x[14] \"|\"\n"
       "if _rc != 0 display \"PRECFAIL|eg_ref_exist|rc=`__rc'|display_rc=\" _rc \"|\"\n",
       {{4338, "PREC|eg_ref_exist|110|float|+1.8000000000000X+001|+1.0010000000000X+3ff|"}}},
  };
  return t;
}

}  // namespace

void testW2apEgen() {
  std::printf(
      "[selftest] --- W2AP egen: the statistics family against w2ap_egen_family.log ---\n");
  namespace fs = std::filesystem;
  const std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_w2apegen").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  auto s = std::make_unique<piet::SessionState>(workdir);
  s->cwd = workdir;
  // One session, the do-file's own order: the fixture, then each arm's stanza (each
  // drops what it made; eg_ref_exist, the one that could overwrite x, runs last).
  std::string src = kEgenFixture;
  for (const auto& a : egenArms()) src += a.stanza;
  std::vector<std::string> displays;
  bool arrayBlock = false;
  try {
    auto res = piet::doexec::executeDoSource(src, *s, piet::dispatch);
    for (const auto& b : res.blocks) {
      if (!b.is_object()) {
        arrayBlock = true;
        continue;
      }
      if (b.value("type", std::string()) == "display") displays.push_back(b.value("text", std::string()));
    }
  } catch (const std::exception& e) {
    displays.push_back(std::string("EXC:") + e.what());
  }
  w2check(!arrayBlock, "W2AP egen: every block is an object (no array pushed as a block)");
  for (const auto& a : egenArms()) {
    for (const auto& [line, want] : a.prec) {
      const std::string w(want);
      const std::string key = w.substr(0, w.find('|', 5) + 1);  // "PREC|<arm>|"
      std::string got = "<no " + key + " line>";
      for (const auto& d : displays) {
        std::string t = d;
        while (!t.empty() && (t.back() == '\n' || t.back() == ' ')) t.pop_back();
        if (t.compare(0, key.size(), key) == 0) {
          got = t;
          break;
        }
      }
      w2check(got == w, std::string("W2AP egen ") + a.arm + " (do:" + std::to_string(a.doLine) +
                            ", log:" + std::to_string(line) + ") -- want " + w + " got " + got);
    }
  }
  // rownonmiss(), strok: not an oracle arm (no golden prints it), so the receipt is
  // the egen function's documented varlist/strok/by() syntax and its strok
  // branch counting !missing(x) (007 Wave 3 cleaning.do:1463 writes the form). Over
  // numbers strok is inert; a string counts as s != "". Without strok the ado's
  // (x<.) is r(109). The previous candidate refused strok (r(198)) and bin/main
  // could not read the string column.
  {
    const std::string strokSrc =
        "capture noisily egen nm1 = rownonmiss(w1 w2), strok\n"
        "local __r1 = _rc\n"
        "capture noisily egen nm2 = rownonmiss(w1 s), strok\n"
        "local __r2 = _rc\n"
        "capture noisily egen nm3 = rownonmiss(w1 s)\n"
        "local __r3 = _rc\n"
        "sort id\n"
        "local __v1 \"\"\n"
        "local __v2 \"\"\n"
        "forvalues i = 1/20 {\n"
        "local __v1 \"`__v1'`=nm1[`i']'\"\n"
        "local __v2 \"`__v2'`=nm2[`i']'\"\n"
        "}\n"
        "display \"STROK|`__r1'|`__r2'|`__r3'|`__v1'|`__v2'|\"\n";
    std::string got = "<no STROK line>";
    try {
      auto res = piet::doexec::executeDoSource(strokSrc, *s, piet::dispatch);
      for (const auto& b : res.blocks) {
        if (!b.is_object() || b.value("type", std::string()) != "display") continue;
        std::string t = b.value("text", std::string());
        while (!t.empty() && (t.back() == '\n' || t.back() == ' ')) t.pop_back();
        if (t.compare(0, 6, "STROK|") == 0) got = t;
      }
    } catch (const std::exception& e) {
      got = std::string("EXC:") + e.what();
    }
    const std::string want =
        "STROK|0|0|109|21002221221122210222|21102211222111221222|";
    w2check(got == want, "W2AP egen rownonmiss(), strok -- want " + want +
                             " got " + got);
  }
  fs::remove_all(workdir, ec);
}

