// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// AER tier-1 (2026-10-28), lane GEN (mvdecode integ sencode fillmissing astile): checks against
// tests/eval/golden/aer_w2/w2ax_t1_official.log / w2ax_t1_community.log (StataBE 19.5) and
// the lane's own StataBE probes p1-p5 (scratch lane-gen/st/p*.log, 2026-10-28). Every check
// names the arm whose record it mirrors; the fixtures repeat the oracle do-files' set-up lines,
// with each `input` block rebuilt from `set obs` + `generate`/`replace`.
//
// Values are read back through `display`, so a check sees what a do-file would see. JSON is
// read with is_object()/value() only, so a wrong shape is a FAIL, never an abort.
#include "selftest_w2.h"

#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "piet/dispatcher.h"
#include "piet/eval/errors.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

namespace {

int gT1GenChecks = 0;
void genCheck(bool cond, const std::string& label) {
  ++gT1GenChecks;
  w2check(cond, "T1 Gen " + label);
}

// rc of one dispatched line: the last error block's coarse rc, "0" when none.
std::string rcOf(piet::SessionState& session, const std::string& cmd) {
  std::string rc = "0";
  try {
    for (const auto& b : piet::dispatch(cmd, session)) {
      if (b.is_object() && b.value("type", std::string()) == "error")
        rc = piet::eval::coarseRc(b);
    }
  } catch (const std::exception& e) {
    rc = std::string("EXC:") + e.what();
  }
  return rc;
}

// Every display block's text a dispatched line produced, concatenated.
std::string textOf(piet::SessionState& session, const std::string& cmd) {
  std::string text;
  try {
    for (const auto& b : piet::dispatch(cmd, session))
      if (b.is_object() && b.value("type", std::string()) == "display")
        text += b.value("text", std::string());
  } catch (const std::exception&) {
  }
  return text;
}

// rc and display text of ONE dispatch of a line.
struct Out {
  std::string rc = "0";
  std::string text;
};
Out outOf(piet::SessionState& session, const std::string& cmd) {
  Out o;
  try {
    for (const auto& b : piet::dispatch(cmd, session)) {
      if (!b.is_object()) continue;
      const std::string type = b.value("type", std::string());
      if (type == "error") o.rc = piet::eval::coarseRc(b);
      if (type == "display") o.text += b.value("text", std::string());
    }
  } catch (const std::exception& e) {
    o.rc = std::string("EXC:") + e.what();
  }
  return o;
}

// `display EXP`, without the trailing newline.
std::string shown(piet::SessionState& session, const std::string& exp) {
  std::string t = textOf(session, "display " + exp);
  while (!t.empty() && (t.back() == '\n' || t.back() == '\r')) t.pop_back();
  return t;
}

void run(piet::SessionState& session, std::initializer_list<const char*> lines) {
  for (const char* l : lines) (void)rcOf(session, l);
}

// ---------------------------------------------------------------------------
// mvdecode -- official do-file section (4); probe p2 mv*.
void mvdecodeArms(const std::string& workdir) {
  piet::SessionState s(workdir);
  run(s, {"clear", "set obs 12", "generate a = mod(_n,4)*3", "replace a = 99 in 12",
          "generate b = 9", "replace b = 99 in 1", "replace b = . in 2",
          "generate str3 s = \"9\""});
  const std::string t1 = textOf(s, "mvdecode a, mv(9)");
  (void)rcOf(s, "quietly count if missing(a)");
  genCheck(shown(s, "r(N)") == "3" && shown(s, "a[3]") == "." && shown(s, "a[12]") == "99",
           "mvdecode_single: 3 missing, a[3] . a[12] 99 (official log)");
  genCheck(t1 == "           a: 3 missing values generated\n",
           "mvdecode_single display `           a: 3 missing values generated' (official log)");
  const std::string t2 = textOf(s, "mvdecode a b, mv(9=. \\ 99=.)");
  (void)rcOf(s, "quietly count if missing(a)");
  const std::string na = shown(s, "r(N)");
  (void)rcOf(s, "quietly count if missing(b)");
  genCheck(na == "4" && shown(s, "r(N)") == "12" && shown(s, "a[12]") == "." &&
               shown(s, "b[1]") == ".",
           "mvdecode_rules: 4|12|.|. (official log)");
  genCheck(t2 == "           a: 1 missing value generated\n           b: 11 missing values "
                 "generated\n",
           "mvdecode_rules display, singular `value' for 1 (official log)");
  run(s, {"generate c = mod(_n,3)"});
  genCheck(rcOf(s, "mvdecode c, mv(0 2)") == "0", "mvdecode_numlist rc 0 (official log)");
  (void)rcOf(s, "quietly count if missing(c)");
  genCheck(shown(s, "r(N)") == "8", "mvdecode_numlist: 8 missing (official log)");
  run(s, {"generate d = mod(_n,3)"});
  const std::string t3 = textOf(s, "mvdecode d s, mv(1)");
  (void)rcOf(s, "quietly count if missing(d)");
  genCheck(shown(s, "r(N)") == "4" && shown(s, "s[1]") == "9" &&
               t3.find("           s: string variable ignored") != std::string::npos,
           "mvdecode_withstr: 4 missing, string skipped with its notice (official log)");
  run(s, {"generate e = mod(_n,5)"});
  genCheck(rcOf(s, "mvdecode _all, mv(4)") == "0", "mvdecode_all rc 0 (official log)");
  (void)rcOf(s, "quietly count if missing(e)");
  genCheck(shown(s, "r(N)") == "2", "mvdecode_all: e 2 missing (official log)");
  run(s, {"generate f = 1"});
  genCheck(rcOf(s, "mvdecode f, mv(7)") == "0" && shown(s, "f[1]") == "1",
           "mvdecode_none: rc 0, f[1] 1 (official log)");
  genCheck(rcOf(s, "mvdecode s, mv(9)") == "0", "mvdecode_stronly rc 0 (official log)");
  genCheck(rcOf(s, "mvdecode f") == "198", "mvdecode_nomv r(198) (official log)");
  genCheck(rcOf(s, "mvdecode f, mv(abc)") == "121", "mvdecode mv(abc) r(121) (probe p2 mvbad)");
  genCheck(rcOf(s, "mvdecode f, mv(1=2)") == "198",
           "mvdecode mv(1=2) r(198) `2 is not a missing value code' (probe p2 mvbadcode)");
  genCheck(rcOf(s, "mvdecode f, mv(1=.a)") == "198" && shown(s, "f[1]") == "1",
           "mvdecode mv(1=.a) refused by name, data untouched (probe p2 mvext: no .a in PIET)");

  // Float compare (probe p2 mvfloat): a float variable meets float(#).
  piet::SessionState s2(workdir);
  run(s2, {"clear", "set obs 6", "generate float f = 0.1*_n", "generate double d = 0.1*_n",
           "mvdecode f d, mv(0.1 0.3)", "quietly count if missing(f)"});
  const std::string nf = shown(s2, "r(N)");
  (void)rcOf(s2, "quietly count if missing(d)");
  genCheck(nf == "2" && shown(s2, "r(N)") == "1",
           "mvdecode float vs double compare: 2|1 (probe p2 mvfloat)");

  // Numlist spellings and their decimal elements (review gg G1-G7, gi I1-I5).
  piet::SessionState s3(workdir);
  run(s3, {"clear", "set obs 11", "generate double x = (_n-1)/10", "generate double x3 = x",
           "generate double x4 = x", "generate y = _n", "generate y3 = _n"});
  (void)rcOf(s3, "mvdecode x, mv(0(0.1)1)");
  (void)rcOf(s3, "quietly count if missing(x)");
  genCheck(shown(s3, "r(N)") == "11", "mvdecode mv(0(0.1)1): all 11 decimals hit (review gg G1)");
  (void)rcOf(s3, "mvdecode x3, mv(0 0.1 to 1)");
  (void)rcOf(s3, "quietly count if missing(x3)");
  genCheck(shown(s3, "r(N)") == "11", "mvdecode mv(0 0.1 to 1): 11 (review gg G3)");
  genCheck(rcOf(s3, "mvdecode x4 if 0, mv(bad)") == "0",
           "mvdecode empty sample exits before mv() is read, rc 0 (review gg G4)");
  (void)rcOf(s3, "mvdecode y, mv(1[2]9)");
  (void)rcOf(s3, "quietly count if missing(y)");
  genCheck(shown(s3, "r(N)") == "5", "mvdecode mv(1[2]9): 5 (review gg G5)");
  (void)rcOf(s3, "mvdecode y3, mv(1 3: 9)");
  (void)rcOf(s3, "quietly count if missing(y3)");
  genCheck(shown(s3, "r(N)") == "5", "mvdecode mv(1 3: 9): 5 (review gg G7)");
  genCheck(rcOf(s3, "mvdecode x-zz, mv(1)") == "111",
           "mvdecode bad varlist range r(111) `variable zz not found' (review gk K2)");
}

// ---------------------------------------------------------------------------
// integ -- official do-file section (7); probe p2 i*.
void integArms(const std::string& workdir) {
  piet::SessionState s(workdir);
  run(s, {"clear", "set obs 9", "generate x = _n/2", "generate y = x^2 + mod(_n,3)"});
  const std::string t = textOf(s, "integ y x");
  genCheck(shown(s, "%18.0g r(integral)") == " 34.83333333333333" && shown(s, "r(N_points)") == "9",
           "integ_basic: cubic spline 34.83333333333333, N_points 9 (official log)");
  genCheck(t == "\nnumber of points = 9\n\nintegral         = 34.833333\n",
           "integ_basic display `integral         = 34.833333' (official log)");
  genCheck(rcOf(s, "integ y x, generate(t1cum)") == "0" &&
               shown(s, "%18.0g t1cum[1]") == "                 0" &&
               shown(s, "%18.0g t1cum[5]") == " 7.562929630279541" &&
               shown(s, "%18.0g t1cum[9]") == " 34.83333206176758",
           "integ_gen: float running integral 0|7.562929630279541|34.83333206176758 (official log)");
  genCheck(shown(s, "\"`: variable label t1cum'\"") == "Integral of y",
           "integ generate() label `Integral of y' (probe p2 itiesg)");
  (void)rcOf(s, "integ y x if x > 1");
  genCheck(shown(s, "%18.0g r(integral)") == "           32.4375", "integ_if: 32.4375 (official log)");
  (void)rcOf(s, "integ y x, trapezoid");
  genCheck(shown(s, "%18.0g r(integral)") == "             34.75", "integ_trap: 34.75 (official log)");
  run(s, {"gsort -x", "integ y x, generate(t1cum2)"});
  genCheck(shown(s, "%18.0g t1cum2[1]") == " 34.83333206176758" &&
               shown(s, "%18.0g t1cum2[9]") == "                 0",
           "integ_unsorted: values land on their own rows (official log)");
  run(s, {"replace y = . in 4", "integ y x"});
  genCheck(shown(s, "%18.0g r(integral)") == " 35.49459639418152",
           "integ_missing: 35.49459639418152 over 8 points (official log)");

  piet::SessionState s2(workdir);
  run(s2, {"clear", "set obs 8", "generate x = cond(_n<=3, _n, _n-3)",
           "generate y = cond(_n<=3, _n^2, cond(_n==4, 2, cond(_n==5, 3, cond(_n==6, 6, 8))))",
           "generate g = cond(_n<=3, 1, 2)", "drop if _n == 8"});
  // Rows now: g1 (1,1) (2,4) (3,9); g2 (1,2) (2,3) (3,6) (4,8) -- no tied x within a group.
  (void)rcOf(s2, "integ y x if g == 1");
  genCheck(shown(s2, "%21x r(integral)") == "+1.1555555555555X+003",
           "integ 3 points: +1.1555555555555X+003 (probe p2 i3)");
  (void)rcOf(s2, "integ y x if g == 1 & x < 3");
  genCheck(shown(s2, "r(integral)") == "2.5", "integ 2 points is the trapezoid, 2.5 (probe p2 i2)");
  genCheck(rcOf(s2, "integ y x, by(g) gen(ib) double") == "0" &&
               shown(s2, "%21x ib[3]") == "+1.1555555555555X+003" &&
               shown(s2, "\"`: type ib'\"") == "double" && shown(s2, "r(integral)") == ".",
           "integ by(): per-group integral, double, r() emptied (probe p2 iby)");
  genCheck(rcOf(s2, "integ y x, gen(ib)") == "110", "integ existing generate() r(110) (probe p2 iexist)");
  genCheck(rcOf(s2, "integ y x if g == 9") == "2000", "integ no observations r(2000) (probe p2 inone)");
  genCheck(rcOf(s2, "integ y") == "102", "integ one variable r(102) (probe p2 ione)");
  genCheck(rcOf(s2, "integ y x, by(g)") == "198", "integ by() without generate() r(198) (probe p2 ibynogen)");
  // A tie in x: the integral is served; generate() is refused by name, because the row
  // StataBE keeps the value on is its own sort's choice (probe p2 irepl).
  run(s2, {"replace x = 2 in 6"});
  genCheck(rcOf(s2, "integ y x if g == 2") == "0" && shown(s2, "r(N_points)") == "3",
           "integ tied x: mean y per tie, 3 points (probe p2 ities)");
  genCheck(rcOf(s2, "integ y x if g == 2, gen(it)") == "199" && rcOf(s2, "confirm variable it") == "111",
           "integ tied x + generate(): refused by name, nothing written (probe p2 irepl)");

  // `float`: the running integral is a float variable, r(integral) included (review gd
  // D5/D10; the input block's float data rebuilt with generate).
  piet::SessionState s3(workdir);
  run(s3, {"clear", "set obs 6",
           "generate x = cond(_n==1,0,cond(_n==2,0.3,cond(_n==3,1.1,cond(_n==4,1.2,cond(_n==5,3.7,4)))))",
           "generate y = cond(_n==1,1,cond(_n==2,2.5,cond(_n==3,0.7,cond(_n==4,4,cond(_n==5,-2,1.5)))))"});
  (void)rcOf(s3, "integ y x, float");
  genCheck(shown(s3, "%21x r(integral)") == "+1.0500c00000000X+004",
           "integ, float: r(integral) +1.0500c00000000X+004 (review gd D5)");
  (void)rcOf(s3, "integ y x, trapezoid float");
  genCheck(shown(s3, "%21x r(integral)") == "+1.1dc2900000000X+002",
           "integ, trapezoid float: +1.1dc2900000000X+002 (review gd D10)");
}

// ---------------------------------------------------------------------------
// sencode -- community do-file section (8); probe p3/p4 s*.
void sencodeFixture(piet::SessionState& s) {
  run(s, {"clear", "set obs 6", "generate str5 A = \"\"", "replace A = \"b\" in 1",
          "replace A = \"a\" in 2", "replace A = \"c\" in 3", "replace A = \"a\" in 4",
          "replace A = \"b\" in 5"});
}

void sencodeArms(const std::string& workdir) {
  piet::SessionState s(workdir);
  sencodeFixture(s);
  genCheck(rcOf(s, "sencode A, g(yaxis)") == "0" && shown(s, "yaxis[1]") == "1" &&
               shown(s, "yaxis[2]") == "2" && shown(s, "yaxis[3]") == "3" &&
               shown(s, "yaxis[4]") == "2" && shown(s, "yaxis[6]") == ".",
           "sencode_basic: order of appearance 1 2 3 2 . (community log)");
  genCheck(shown(s, "\"`: value label yaxis'|`: label (yaxis) 1'|`: label (yaxis) 2'|`: label (yaxis) 3'\"") ==
               "yaxis|b|a|c",
           "sencode_basic: label yaxis b a c (community log)");
  genCheck(rcOf(s, "sencode A, g(y2) gsort(A)") == "0" && shown(s, "y2[1]") == "2" &&
               shown(s, "y2[2]") == "1" && shown(s, "\"`: label (y2) 1'\"") == "a",
           "sencode_gsort: alphabetical 2 1, label 1 a (community log)");
  genCheck(rcOf(s, "sencode A, g(y3) gsort(-A)") == "0" && shown(s, "y3[1]") == "2" &&
               shown(s, "y3[2]") == "3" && shown(s, "y3[3]") == "1",
           "sencode_gsortdesc: 2 3 1 (community log)");
  genCheck(rcOf(s, "sencode A, g(yaxis)") == "110", "sencode_exists r(110) (community log)");
  genCheck(rcOf(s, "sencode A, g(y4) label(t1lab)") == "0" &&
               shown(s, "\"`: value label y4'|`: label (y4) 2'\"") == "t1lab|a",
           "sencode_label: label t1lab, 2 a (community log)");
  run(s, {"generate str5 B = A"});
  genCheck(rcOf(s, "sencode B, replace") == "0" && shown(s, "B[1]") == "1" &&
               shown(s, "\"`: value label B'|`: label (B) 1'\"") == "B|b",
           "sencode_replace: B coded in place, label B (community log)");
  genCheck(rcOf(s, "sencode A") == "198", "sencode_noopt r(198) (community log)");
  genCheck(rcOf(s, "sencode A in 1/3, g(y5)") == "0" && shown(s, "y5[1]") == "1" &&
               shown(s, "y5[3]") == "3" && shown(s, "y5[4]") == ".",
           "sencode_in: 1 3 . (community log)");

  // An existing set keeps its codes; new strings take its largest code + 1 (probe p3 slab).
  piet::SessionState s2(workdir);
  sencodeFixture(s2);
  run(s2, {"label variable A \"the A\"", "label define pre 7 \"c\" 2 \"q\""});
  genCheck(rcOf(s2, "sencode A, g(e1) label(pre)") == "0" && shown(s2, "e1[1]") == "8" &&
               shown(s2, "e1[2]") == "9" && shown(s2, "e1[3]") == "7" &&
               shown(s2, "\"`: type e1'|`: variable label e1'|`: format e1'\"") == "byte|the A|%8.0g",
           "sencode label(pre) extends pre: 8 9 7, byte, label copied (probe p3 slab)");
  genCheck(rcOf(s2, "sencode A, g(e2) label(pre, replace)") == "0" && shown(s2, "e2[1]") == "1" &&
               shown(s2, "\"`: label (e2) 7'\"") == "7",
           "sencode label(, replace) starts the set afresh (probe p3 slabrep)");
  genCheck(rcOf(s2, "sencode A, g(e3) replace") == "198", "sencode generate()+replace r(198) (probe p3 sboth)");
  run(s2, {"generate n = _n"});
  genCheck(rcOf(s2, "sencode n, g(e4)") == "109", "sencode numeric variable r(109) (probe p3 snum)");
  genCheck(rcOf(s2, "sencode A if n > 1, g(e6)") == "0" && shown(s2, "e6[1]") == "." &&
               shown(s2, "e6[2]") == "1" && shown(s2, "e6[5]") == "3",
           "sencode if: codes over the sample only (probe p3 sif)");
  genCheck(rcOf(s2, "sencode A, g(e8) manyto1") == "198" && rcOf(s2, "confirm variable e8") == "111",
           "sencode manyto1 refused by name, nothing written (probe p3 smany)");
}

// ---------------------------------------------------------------------------
// fillmissing -- community do-file section (6); probe p1/p2/p5.
// The section's input block: id t x y s, nine rows.
void fillFixture(piet::SessionState& s) {
  run(s, {"clear", "set obs 9", "generate id = cond(_n<=4, 1, cond(_n<=7, 2, 3))",
          "generate t = cond(_n<=4, _n, cond(_n<=7, _n-4, _n-7))", "generate x = .",
          "replace x = 3 in 2", "replace x = 5 in 4", "replace x = 7 in 7", "generate y = .",
          "replace y = 4 in 1", "replace y = 6 in 3", "replace y = 8 in 6",
          "generate str2 s = \"\"", "replace s = \"a\" in 2", "replace s = \"b\" in 5",
          "replace s = \"c\" in 7", "generate long pos = _n", "generate x2 = x",
          "generate x3 = x", "generate x4 = x", "generate x5 = x", "generate x6 = x",
          "generate x7 = x", "generate str2 s2 = s", "generate str2 s3 = s", "sort id pos"});
}

void fillArms(const std::string& workdir) {
  piet::SessionState s(workdir);
  fillFixture(s);
  const Out anyOut = outOf(s, "by id: fillmissing x, with(any)");
  genCheck(anyOut.rc == "0" &&
               shown(s, "x[1] \"|\" x[3] \"|\" x[4] \"|\" x[5] \"|\" x[6] \"|\" x[8]") == "3|3|5|7|7|.",
           "fill_any_by: 3 3 5 7 7 . (community log)");
  genCheck(anyOut.text == "(4 real changes made)\n",
           "fill_any_by prints `(4 real changes made)' (community log)");
  const Out firstOut = outOf(s, "by id: fillmissing x2, with(first)");
  genCheck(firstOut.rc == "0" &&
               shown(s, "x2[1] \"|\" x2[3] \"|\" x2[5] \"|\" x2[6]") == ".|.|.|.",
           "fill_first_by: the first row's own missing (community log)");
  genCheck(firstOut.text == "(0 real changes made)\n",
           "fill_first_by prints `(0 real changes made)' (community log)");
  genCheck(rcOf(s, "by id: fillmissing x3, with(last)") == "0" &&
               shown(s, "x3[1] \"|\" x3[3] \"|\" x3[5]") == "5|5|7",
           "fill_last_by: 5 5 7 (community log)");
  const Out meanOut = outOf(s, "by id: fillmissing x4, with(mean)");
  genCheck(meanOut.rc == "0" &&
               shown(s, "x4[1] \"|\" x4[3] \"|\" x4[5] \"|\" x4[8]") == "4|4|7|.",
           "fill_mean_by: 4 4 7 . (community log)");
  genCheck(meanOut.text == "(2 missing values generated)\n(4 real changes made)\n",
           "fill_mean_by prints egen's and replace's notes (community log)");
  genCheck(rcOf(s, "by id: fillmissing x5, with(max)") == "0" && shown(s, "x5[1] \"|\" x5[5]") == "5|7",
           "fill_max_by: 5 7 (community log)");
  genCheck(rcOf(s, "by id: fillmissing x6, with(min)") == "0" && shown(s, "x6[1] \"|\" x6[5]") == "3|7",
           "fill_min_by: 3 7 (community log)");
  genCheck(rcOf(s, "fillmissing x7, with(any)") == "0" && shown(s, "x7[1] \"|\" x7[8]") == "3|3",
           "fill_any_nobs: the whole data is one group (community log)");
  genCheck(rcOf(s, "by id: fillmissing s, with(any)") == "0" &&
               shown(s, "s[1] \"|\" s[3] \"|\" s[5] \"|\" s[6] \"|\" s[8]") == "a|a|b|b|",
           "fill_str_any: a a b b \"\" (community log)");
  const std::string note = textOf(s, "by id: fillmissing s2, with(mean)");
  genCheck(rcOf(s, "by id: fillmissing s2, with(mean)") == "0" &&
               note == "Variable s2 is a string variable, option with(mean) cannot be used with "
                       "string variables.\n",
           "fill_str_mean: rc 0 with the package's notice (community log)");
  genCheck(rcOf(s, "by id: fillmissing y") == "0" && shown(s, "y[2] \"|\" y[4] \"|\" y[5]") == "4|4|8",
           "fill_default: with(any) 4 4 8 (community log)");
  genCheck(rcOf(s, "by id: fillmissing s3, with(first)") == "0" &&
               shown(s, "s3[1] \"|\" s3[3] \"|\" s3[6]") == "||b",
           "fill_str_first: \"\" \"\" b (community log)");
  run(s, {"generate x8 = x", "replace x8 = . in 1/4"});
  genCheck(rcOf(s, "fillmissing x8 if id == 2, with(any)") == "0" && shown(s, "x8[1] \"|\" x8[5]") == ".|7",
           "fill_if: outside the sample untouched (community log)");
  genCheck(rcOf(s, "confirm variable __piet_t1g_touse") == "111" &&
               rcOf(s, "confirm variable __piet_t1g_stat") == "111",
           "fillmissing leaves no scratch column (probe ORPHAN 0)");

  // any = the group's SMALLEST value, not its first (probe p1 any: ., 5, ., 3 -> 3).
  piet::SessionState s2(workdir);
  run(s2, {"clear", "set obs 6", "generate id = cond(_n<=4, 1, 2)", "generate x = .",
           "replace x = 5 in 2", "replace x = 3 in 4", "replace x = 9 in 6",
           "generate x2 = x", "generate x3 = x", "generate x4 = x", "sort id"});
  genCheck(rcOf(s2, "by id: fillmissing x, with(any)") == "0" &&
               shown(s2, "x[1] \"|\" x[3] \"|\" x[5]") == "3|3|9",
           "fillmissing any is the group minimum 3, not the first 5 (probe p1 any)");
  genCheck(rcOf(s2, "by id: fillmissing x2, with(previous)") == "0" &&
               shown(s2, "x2[1] \"|\" x2[3] \"|\" x2[5]") == ".|5|.",
           "fillmissing previous (probe p1 prev)");
  const Out nextOut = outOf(s2, "by id: fillmissing x3, with(next)");
  genCheck(nextOut.rc == "0" && shown(s2, "x3[1] \"|\" x3[3] \"|\" x3[5]") == "5|3|9" &&
               nextOut.text == "(3 real changes made)\n(0 real changes made)\n",
           "fillmissing next: values and its two replace notes (probe p1 next)");
  genCheck(rcOf(s2, "fillmissing x4 in 1/2, with(last)") == "199" && shown(s2, "x4[1]") == ".",
           "fillmissing order fill off the sorted order refused by name (probe p1 in: StataBE's "
           "unstable tie order)");
  genCheck(rcOf(s2, "fillmissing x4, with(bogus)") == "0" && shown(s2, "x4[1]") == ".",
           "fillmissing with(bogus): the package's usage text at rc 0 (probe p1 bogus)");

  // A statistic is stored as egen stores it, float (probe p2 meand: 5/3 -> +1.aaaaaa0000000X+000),
  // and a byte variable widens to float (probe p3 meanb).
  piet::SessionState s3(workdir);
  run(s3, {"clear", "set obs 4", "generate id = 1", "generate double x = cond(_n==1, 1, 2)",
           "replace x = . in 4", "generate byte b = x", "sort id"});
  (void)rcOf(s3, "by id: fillmissing x, with(mean)");
  const Out byteOut = outOf(s3, "by id: fillmissing b, with(mean)");
  genCheck(byteOut.text == "variable b was byte now float\n(1 real change made)\n",
           "fillmissing mean into a byte: the widening note, then `(1 real change made)' (probe p3 "
           "meanb)");
  genCheck(shown(s3, "%21x x[4]") == "+1.aaaaaa0000000X+000" &&
               shown(s3, "%21x b[4] \"|`: type b'\"") == "+1.aaaaaa0000000X+000|float",
           "fillmissing mean is float(5/3); byte widens to float (probe p2 meand / p3 meanb)");
}

// ---------------------------------------------------------------------------
// astile -- community do-file section (3); probe p1/p5 a*.
void astileArms(const std::string& workdir) {
  piet::SessionState s(workdir);
  run(s, {"clear", "set obs 41", "generate x = mod(_n*7,13) + _n/100", "replace x = 5 in 3/6",
          "replace x = . in 9", "generate g = mod(_n,3)", "generate long pos = _n"});
  auto checksum = [&](const std::string& v) {
    run(s, {("generate double t1c = " + v + "*pos").c_str(), "quietly summarize t1c"});
    const std::string r = shown(s, "r(sum)");
    (void)rcOf(s, "drop t1c");
    return r;
  };
  genCheck(rcOf(s, "astile d10 = x, nq(10)") == "0" && checksum("d10") == "4806" &&
               shown(s, "d10[1] \"|\" d10[3] \"|\" d10[9] \"|\" d10[41]") == "7|4|.|2",
           "astile_nq10: 4806, 7 4 . 2 (community log)");
  run(s, {"xtile x10 = x, nq(10)", "quietly count if x10 != d10"});
  genCheck(shown(s, "r(N)") == "0", "astile_vs_xtile10: 0 disagreements with xtile (community log)");
  genCheck(shown(s, "\"`: type d10'|`: format d10'|`: variable label d10'\"") ==
               "float|%9.0g|10 quantiles of x",
           "astile without by(): c(type) float %9.0g, `10 quantiles of x' (review gc C1)");
  genCheck(rcOf(s, "astile h2 = x, nq(2) by(g)") == "0" && checksum("h2") == "1326" &&
               shown(s, "h2[1] \"|\" h2[2] \"|\" h2[3]") == "2|1|1",
           "astile_nq2_by: 1326, 2 1 1 (community log)");
  genCheck(rcOf(s, "astile q4 = x, nq(4) by(g)") == "0" && checksum("q4") == "2121",
           "astile_nq4_by: 2121 (community log)");
  genCheck(shown(s, "\"`: type q4'\"") == "double", "astile with by(): double (review gc C1)");
  genCheck(rcOf(s, "astile m2 = x") == "0" && checksum("m2") == "1328",
           "astile_default: nq 2, 1328 (community log)");
  genCheck(rcOf(s, "astile e5 = x*2 + 1, nq(5)") == "3598", "astile_expr r(3598) (community log)");
  genCheck(rcOf(s, "astile i4 = x if g == 1, nq(4)") == "0" && checksum("i4") == "700" &&
               shown(s, "i4[1] \"|\" i4[2]") == "3|.",
           "astile_if: 700, 3 . (community log)");
  genCheck(rcOf(s, "astile d10 = x, nq(10)") == "3598", "astile_exists r(3598) (community log)");
  genCheck(rcOf(s, "bysort g: astile b3 = x, nq(3)") == "0" && rcOf(s, "sort pos") == "0" &&
               checksum("b3") == "1677",
           "astile_byprefix: 1677 (community log)");
  genCheck(rcOf(s, "astile n1 = x, nq(1)") == "3598", "astile nq(1) r(3598) (probe p1 anq1)");
  genCheck(rcOf(s, "astile n2 = x, nq(2.5)") == "3598",
           "astile nq(2.5) r(3598) `option nq() incorrectly specified' (review gk K10)");
  genCheck(rcOf(s, "astile n3 = x if pos > 3, nquantiles(3)") == "0" &&
               shown(s, "\"`: variable label n3'|\"") == "3 quantiles of x |",
           "astile label keeps the blank before `if' (review gb B6)");
  run(s, {"set type double"});
  genCheck(rcOf(s, "astile n4 = x, nq(4)") == "0" && shown(s, "\"`: type n4'\"") == "double",
           "astile under set type double: double (review gc C2)");
  run(s, {"set type float"});
  genCheck(rcOf(s, "confirm variable __piet_t1g_ax") == "111" &&
               rcOf(s, "confirm variable __piet_t1g_c1") == "111",
           "astile leaves no scratch column (probe ORPHAN 0)");

  // nq() above the group size is served, unlike xtile's limit (probe p1 asmall).
  piet::SessionState s2(workdir);
  run(s2, {"clear", "set obs 10", "generate g = cond(_n<=2, 1, cond(_n<=7, 2, cond(_n<=9, 3, 4)))",
           "generate x = cond(_n<=2, _n, cond(_n<=7, _n+2, cond(_n<=9, ., 3)))"});
  genCheck(rcOf(s2, "astile q = x, nq(4) by(g)") == "0" &&
               shown(s2, "q[1] \"|\" q[2] \"|\" q[3] \"|\" q[7] \"|\" q[8] \"|\" q[10]") == "1|3|1|4|.|1",
           "astile small groups: 1 3 1 4 . 1 (probe p1 asmall)");
  run(s2, {"generate str2 sx = \"a\""});
  genCheck(rcOf(s2, "astile q2 = sx, nq(2)") == "109", "astile string variable r(109) (probe p1 astr)");

  // qc(): breakpoints from the criterion rows, every sample row placed (probe p5 aqc1).
  piet::SessionState s3(workdir);
  run(s3, {"clear", "set obs 30", "generate x = mod(_n*7, 11) + _n/100", "generate g = mod(_n, 3)",
           "generate f = mod(_n, 2)", "replace f = 0 if g == 2", "generate long pos = _n",
           "astile a1 = x, nq(4) qc(f == 1)", "generate double t = a1*pos", "quietly summarize t"});
  genCheck(shown(s3, "r(sum) \"|\" a1[1] \"|\" a1[2]") == "1093|3|1", "astile qc(): 1093 (probe p5 aqc1)");
}

}  // namespace

void testW2T1Gen() {
  std::printf("[selftest] --- T1 Gen: mvdecode integ sencode fillmissing astile (w2ax_t1_*.log) ---\n");
  namespace fs = std::filesystem;
  const std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_t1gen").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  mvdecodeArms(workdir);
  integArms(workdir);
  sencodeArms(workdir);
  fillArms(workdir);
  astileArms(workdir);
  std::printf("[selftest] T1 Gen: %d checks\n", gT1GenChecks);
}
