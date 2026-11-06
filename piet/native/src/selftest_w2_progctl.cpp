// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// W2 lane `progctl` -- selftest arms for the COMMON_GE2 control-flow and
// temporary-object rows (goal ge2-prog-controlflow-temp, BUG-920..923).
//
// Oracle: the StataBE 19.5 capture of w2_prog_controlflow.do
// (internal capture, log sha256
// 58a41aa6...; lands as tests/eval/golden/aer_w2/w2_prog_controlflow.log).
// Every arm names the oracle arm (pc_*) it rests on and what main answered
// before the fix, so each arm is a discriminator: it fails on main and passes
// with the fix. The fixture is pc_fixture of that .do, built in memory.
//
// Round 3 (ST1-ST13 of internal capture
// ROUND3-SPEC.md section 5) rests on the second
// capture, w2ad_prog_preserve_nest (tests/eval/golden/aer_w2/, log sha256
// d37d24c0...): which level owns a preserved copy, the loop-macro tails, and
// `continue` and temporaries across program and do-file levels. Its fixture
// is pn_fixture (5 obs, x = 2^(_n-1)), so the sum of x is a bitmask of the
// rows left, and every step is recorded as rc:N:sum.
#include "selftest_w2.h"

#include <cstdio>
#include <filesystem>
#include <string>

#include "piet/dispatcher.h"
#include "piet/doexec.h"
#include "piet/eval/tokenizer.h"
#include "piet/session_state.h"

namespace {

// pc_fixture of w2_prog_controlflow.do, verbatim (the program body).
const char* kFixture =
    "clear\n"
    "quietly set obs 6\n"
    "generate double d = 12345678901 + _n*_n\n"
    "generate float f = _n/10\n"
    "generate long l = 2000000000 + _n\n"
    "generate byte b = mod(_n, 3) - 1\n"
    "generate int i = 100*_n\n"
    "generate str5 s = \"s \" + string(_n)\n"
    "generate double di = _n\n"
    "generate str12 s12 = \"x\" + string(_n)\n"
    "label variable d \"Big double\"\n"
    "label define bl -1 \"neg\" 0 \"zero\" 1 \"pos\"\n"
    "label values b bl\n"
    "format d %15.0f\n";

}  // namespace

void testW2ProgCtl() {
  std::printf("[selftest] --- W2 progctl: loops, if/else, preserve/restore, tempfile/tempvar/"
              "tempname, program/args (BUG-920..923) ---\n");
  namespace fs = std::filesystem;
  const std::string workdir =
      (fs::temp_directory_path() / "piet_native_selftest_w2progctl").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  // ── BUG-920: loops ──────────────────────────────────────────────────────
  {
    piet::SessionState session(workdir);
    auto run = [&](const std::string& src) {
      return piet::doexec::executeDoSource(src, session, piet::dispatch);
    };
    auto L = [&](const std::string& k) { return session.eval.locals.get(k, "<unset>"); };

    run("local m \"before\"\nforeach m in p1 p2 p3 {\n    local seen \"`seen'`m'\"\n}\n"
        "local i \"before\"\nforvalues i = 1/5 {\n    local sum = `sum' + `i'\n}\n");
    w2check(L("seen") == "p1p2p3" && L("m") == "<unset>" && L("sum") == "15" && L("i") == "<unset>",
            "W2 progctl/BUG-920 (pc_fe_after, pc_fv_ab): the loop macro is empty after foreach "
            "and forvalues. Main: m=p3, i=5. Got m=" + L("m") + " i=" + L("i"));

    // wt_x01_L4 (w2at_table_writers.do:834, log `[x1 & .125 \\]`): a loop or
    // program body is stored with each unquoted blank run collapsed to one
    // blank before macro expansion (StataBE 19.5 probes, diag g12 table: T1,
    // F1-F3, I1, program echo0): quoted and compound-quoted text and an
    // expanded macro's own blanks are kept; a top-level line or top-level
    // `if {}` body keeps its blanks. Before: every body kept its blanks.
    {
      namespace ev = piet::eval;
      w2check(ev::collapseUnquotedBlanks("    tex `lab' &  ``lab'_m' \\\\ ") ==
                      "    tex `lab' & ``lab'_m' \\\\ " &&
                  ev::collapseUnquotedBlanks("a  \"x  y\"\t `\"u  \"v\"  w\"'  b") ==
                      "a \"x  y\" `\"u  \"v\"  w\"' b",
              "W2 progctl (wt_x01): collapseUnquotedBlanks keeps indentation, double- and "
              "nested compound-quoted text, and collapses every other blank/tab run");
      auto G = [&]() { return session.eval.globals.get("PC_E0", "<unset>"); };
      run("capture program drop pc_e0\nprogram pc_e0\n    global PC_E0 `\"`0'\"'\nend\n");
      run("pc_e0 T1 a &  b\n");
      const std::string t1 = G();
      run("if 1 {\n    pc_e0 I1 p  q\n}\n");
      const std::string i1 = G();
      run("foreach k in 1 {\n    pc_e0 F1 a  \"x  y\"  b\n}\n");
      const std::string f1 = G();
      run("foreach k in 1 {\n    pc_e0 F2 a\t&\tb\n}\n");
      const std::string f2 = G();
      run("local q a  &  b\nforvalues k = 1/1 {\n    if 1 {\n        pc_e0 F3 `q'  z\n    }\n}\n");
      const std::string f3 = G();
      run("capture program drop pc_e1\nprogram pc_e1\n    pc_e0 P1 a   b \"q  q\"\nend\npc_e1\n");
      const std::string p1 = G();
      w2check(t1 == "T1 a &  b" && i1 == "I1 p  q" && f1 == "F1 a \"x  y\" b" && f2 == "F2 a & b" &&
                  f3 == "F3 a  &  b z" && p1 == "P1 a b \"q  q\"",
              "W2 progctl (wt_x01): loop/program bodies collapse unquoted blank runs, top level "
              "keeps them. Got [" + t1 + "] [" + i1 + "] [" + f1 + "] [" + f2 + "] [" + f3 + "] [" +
                  p1 + "]");
    }

    run("local acc \"\"\nforvalues e = 0(0.1)1 {\n    local acc \"`acc' `e'\"\n}\n"
        "local n 0\nforvalues sub = 0(0.01)0.31 {\n    local n = `n' + 1\n    local last `sub'\n}\n");
    w2check(L("acc") == " 0 .1 .2 .3 .4 .5 .6 .7 .7999999999999999 .8999999999999999 "
                        ".9999999999999999",
            "W2 progctl/BUG-920 (pc_fv_frac): forvalues 0(0.1)1 assigns Stata's macro text "
            "(.3, no leading 0). Main: 0.1 0.2 0.30000000000000004 ... Got [" + L("acc") + "]");
    w2check(L("n") == "31" && L("last") == ".3000000000000001",
            "W2 progctl/BUG-920 (pc_fv_frac_small): 0(0.01)0.31 runs 31 times, last "
            ".3000000000000001 (no end tolerance). Main: 32, 0.3100000000000001. Got n=" + L("n") +
                " last=" + L("last"));

    run("local n1 0\nlocal n3 0\ncapture noisily {\n    forvalues i = 3/1 {\n"
        "        local n1 = `n1' + 1\n    }\n    foreach v in {\n        local n3 = `n3' + 1\n    }\n}\n"
        "local rcE = _rc\n");
    w2check(L("n1") == "0" && L("n3") == "0" && L("rcE") == "0",
            "W2 progctl/BUG-920 (pc_fv_empty, pc_fe_empty_in): forvalues 3/1 and `foreach v in {` "
            "run zero times at rc 0. Main: 3 iterations; the empty in-list rc 1. Got n1=" + L("n1") +
                " n3=" + L("n3") + " rc=" + L("rcE"));

    run("local acc \"\"\ncapture noisily {\n    foreach k of numlist 1/3 {\n        if `k' == 1 {\n"
        "            local acc \"`acc' A\"\n        } else if `k' == 2 {\n"
        "            local acc \"`acc' B\"\n        }\n    }\n}\nlocal rcB = _rc\n");
    w2check(L("rcB") == "198" && L("acc") == " A",
            "W2 progctl/BUG-920 (pc_if_brace_else): `} else if` on the close-brace line is r(198) "
            "after the block ran once. Main: r(199). Got rc=" + L("rcB") + " acc=[" + L("acc") + "]");

    // ST11: the loop macro is empty after `continue, break` and after an error
    // exit too (w2ad pn_lm_fe_break [p1p2]|[], pn_lm_fv_break, pn_lm_fe_err,
    // pn_lm_fv_err, pn_lm_nested_break [AxAy<>BxBy<>]|[]|[]). Main kept the
    // last value: [p2], [3], and <y> for the inner macro read in the outer body.
    run("local m \"before\"\nlocal s1 \"\"\ncapture noisily {\n    foreach m in p1 p2 p3 {\n"
        "        local s1 \"`s1'`m'\"\n        if \"`m'\" == \"p2\" continue, break\n    }\n}\n"
        "local m1 \"[`m']\"\nlocal i \"before\"\ncapture noisily {\n    forvalues i = 1/5 {\n"
        "        if `i' == 3 continue, break\n    }\n}\nlocal i1 \"[`i']\"\n"
        "local m \"before\"\ncapture noisily {\n    foreach m in p1 p2 p3 {\n"
        "        if \"`m'\" == \"p2\" error 459\n    }\n}\nlocal rcE1 = _rc\nlocal m2 \"[`m']\"\n"
        "local i \"before\"\ncapture noisily {\n    forvalues i = 1/5 {\n"
        "        if `i' == 3 error 459\n    }\n}\nlocal rcE2 = _rc\nlocal i2 \"[`i']\"\n"
        "local a \"before\"\nlocal b \"before\"\nlocal acc \"\"\ncapture noisily {\n"
        "    foreach a in A B {\n        foreach b in x y z {\n            local acc \"`acc'`a'`b'\"\n"
        "            if \"`b'\" == \"y\" continue, break\n        }\n"
        "        local acc \"`acc'<`b'>\"\n    }\n}\nlocal nb \"`acc'|`a'|`b'\"\n");
    w2check(L("s1") == "p1p2" && L("m1") == "[]" && L("i1") == "[]" && L("rcE1") == "459" &&
                L("m2") == "[]" && L("rcE2") == "459" && L("i2") == "[]" &&
                L("nb") == "AxAy<>BxBy<>||",
            "W2 progctl/BUG-920 ST11 (w2ad pn_lm_fe_break, pn_lm_fv_break, pn_lm_fe_err, "
            "pn_lm_fv_err, pn_lm_nested_break): the loop macro is empty after continue, break and "
            "after an error exit. Main: [p2]/[3]/[p2]/[3], nested <y>. Got " + L("m1") + L("i1") +
                L("m2") + L("i2") + " [" + L("nb") + "]");

    // ST12: normal completion -- nested loops, a zero-iteration range and an
    // empty list -- leaves the macro empty as well (w2ad pn_lm_nested_done
    // [AxAy<>BxBy<>]|[]|[], pn_lm_fv_zero []|[], pn_lm_fe_zero []|[]). Main:
    // <y> and [B], 3/1 iterated 3 2 1, the empty list kept [before].
    run("local a \"before\"\nlocal b \"before\"\nlocal acc \"\"\ncapture noisily {\n"
        "    foreach a in A B {\n        foreach b in x y {\n            local acc \"`acc'`a'`b'\"\n"
        "        }\n        local acc \"`acc'<`b'>\"\n    }\n}\nlocal nd \"`acc'|`a'|`b'\"\n"
        "local i \"before\"\nlocal s2 \"\"\ncapture noisily {\n    forvalues i = 3/1 {\n"
        "        local s2 \"`s2' `i'\"\n    }\n}\nlocal z1 \"`s2'|`i'\"\n"
        "local m \"before\"\nlocal s3 \"\"\nlocal pn_empty \"\"\ncapture noisily {\n"
        "    foreach m of local pn_empty {\n        local s3 \"`s3'`m'\"\n    }\n}\n"
        "local z2 \"`s3'|`m'\"\n");
    w2check(L("nd") == "AxAy<>BxBy<>||" && L("z1") == "|" && L("z2") == "|",
            "W2 progctl/BUG-920 ST12 (w2ad pn_lm_nested_done, pn_lm_fv_zero, pn_lm_fe_zero): "
            "normal completion leaves the loop macro empty. Main: AxAy<y>BxBy<y>|B|y, ' 3 2 1|1', "
            "'|before'. Got [" + L("nd") + "] [" + L("z1") + "] [" + L("z2") + "]");

    // S2 sibling: a program or nested do-file run from a loop body starts
    // outside any loop, so its `continue, break` is r(119) at every iteration
    // and the caller's loop runs on (w2ad pn_lm_cb_prog, pn_lm_cb_do:
    // [ 1:119 2:119 3:119]). Main: rc 0 -- the program's `continue` broke out
    // of nothing and was swallowed.
    {
      const std::string cbDo = (fs::path(workdir) / "pcst_cb.do").u8string();
      if (FILE* fp = std::fopen(cbDo.c_str(), "w")) {
        std::fputs("continue, break\n", fp);
        std::fclose(fp);
      }
      run("capture program drop pcst_cb\nprogram define pcst_cb\n    continue, break\nend\n"
          "local cp \"\"\nlocal cd \"\"\ncapture noisily {\n    forvalues i = 1/3 {\n"
          "        capture noisily pcst_cb\n        local r = _rc\n        local cp \"`cp' `i':`r'\"\n"
          "        capture noisily do \"" + cbDo + "\"\n        local r = _rc\n"
          "        local cd \"`cd' `i':`r'\"\n    }\n}\n");
      w2check(L("cp") == " 1:119 2:119 3:119" && L("cd") == " 1:119 2:119 3:119",
              "W2 progctl/BUG-920 S2 (w2ad pn_lm_cb_prog, pn_lm_cb_do): `continue, break` in a "
              "program or do-file run from a loop body is r(119) each iteration. Main: 1:0 2:0 "
              "3:0. Got [" + L("cp") + "] [" + L("cd") + "]");
    }
  }

  // ── BUG-921: the if condition reads variables (obs 1, explicit subscripts) ──
  {
    piet::SessionState session(workdir);
    auto run = [&](const std::string& src) {
      return piet::doexec::executeDoSource(src, session, piet::dispatch);
    };
    auto L = [&](const std::string& k) { return session.eval.locals.get(k, "<unset>"); };
    run(kFixture);
    run("scalar i = 999\ncapture noisily {\n    if i > 250 {\n        local r3 \"T\"\n    }\n"
        "    else local r3 \"F\"\n    if s == \"s 1\" local r4 \"T\"\n    if i[3] > 250 local r5 \"T\"\n"
        "    if i == 999 local r6 \"T\"\n}\nlocal rcV = _rc\n"
        "capture noisily {\n    if d[99] == . local m2 \"T\"\n    if missing(f[7]) local m4 \"T\"\n}\n"
        "local rcM = _rc\n");
    w2check(L("rcV") == "0" && L("r3") == "F" && L("r4") == "T" && L("r5") == "T" &&
                L("r6") == "<unset>",
            "W2 progctl/BUG-921 (pc_if_varref): a variable outranks a same-named scalar at obs 1, a "
            "string variable compares, i[3] reads obs 3. Main: r3=T (scalar won), then rc 1 on the "
            "string variable. Got rc=" + L("rcV") + " r3=" + L("r3") + " r4=" + L("r4") + " r5=" +
                L("r5") + " r6=" + L("r6"));
    w2check(L("rcM") == "0" && L("m2") == "T" && L("m4") == "T",
            "W2 progctl/BUG-921 (pc_if_missing): d[99] and f[7] past _N are missing. Main: rc 1. "
            "Got rc=" + L("rcM") + " m2=" + L("m2") + " m4=" + L("m4"));
  }

  // ── BUG-922: preserve/restore ───────────────────────────────────────────
  {
    piet::SessionState session(workdir);
    auto run = [&](const std::string& src) {
      return piet::doexec::executeDoSource(src, session, piet::dispatch);
    };
    auto L = [&](const std::string& k) { return session.eval.locals.get(k, "<unset>"); };
    run(kFixture);
    run("preserve\nquietly keep in 1/3\ncapture noisily restore, not\nlocal rcN = _rc\n"
        "local nN = _N\ncapture noisily restore\nlocal rcN2 = _rc\nlocal nN2 = _N\n");
    w2check(L("rcN") == "0" && L("nN") == "3" && L("rcN2") == "622" && L("nN2") == "3",
            "W2 progctl/BUG-922 (pc_pr_not, pc_pr_not_then_restore): `restore, not` keeps the "
            "data and disarms; the next restore is r(622). Main: r(198), then the restore "
            "succeeded. Got " + L("rcN") + "/" + L("nN") + "/" + L("rcN2") + "/" + L("nN2"));

    run(kFixture);
    run("preserve\nquietly replace i = 7 in 1\ncapture noisily preserve\nlocal rcT = _rc\n"
        "capture noisily restore\nlocal rcT2 = _rc\nquietly summarize i in 1\nlocal iT = r(mean)\n"
        "capture noisily restore\n"
        "local rcT3 = _rc\n");
    w2check(L("rcT") == "621" && L("rcT2") == "0" && L("iT") == "100" && L("rcT3") == "622",
            "W2 progctl/BUG-922 (pc_pr_twice*): a second preserve is r(621), restore brings back "
            "the FIRST copy, a further restore is r(622). Main: 0, i[1]=7, 0. Got " + L("rcT") +
                "/" + L("iT") + "/" + L("rcT3"));

    run("capture program drop pcst_pp\nprogram define pcst_pp\n    preserve\n"
        "    quietly keep in 1/2\nend\ncapture program drop pcst_perr\nprogram define pcst_perr\n"
        "    preserve\n    quietly keep in 1\n    error 459\nend\n");
    run(kFixture);
    run("capture noisily pcst_pp\nlocal nP = _N\ncapture noisily pcst_perr\nlocal rcE = _rc\n"
        "local nE = _N\ncapture noisily restore\nlocal rcP = _rc\n");
    w2check(L("nP") == "6" && L("rcE") == "459" && L("nE") == "6" && L("rcP") == "622",
            "W2 progctl/BUG-922 (pc_pr_in_program, pc_pr_in_program_err): a program's own "
            "preserve is restored when it returns, also on error, and leaves nothing armed. "
            "Main: N 2 and N 1. Got " + L("nP") + "/" + L("rcE") + "/" + L("nE") + "/" + L("rcP"));

    // A nested do-file has its own preserve level ([P] preserve: the data are
    // restored automatically when the program or do-file ends). Not in the w2_prog_controlflow oracle; the review probe measured
    // main: a sub-file that preserves while its caller holds a copy runs (rc 0,
    // its own restore gives back N 4). The first cut of BUG-922 keyed the level
    // on program calls only and refused it r(621); main never restored a copy a
    // sub-file left behind (N stayed 1).
    {
      const std::string subDo = (fs::path(workdir) / "pcst_sub.do").u8string();
      const std::string leaveDo = (fs::path(workdir) / "pcst_leave.do").u8string();
      if (FILE* fp = std::fopen(subDo.c_str(), "w")) {
        std::fputs("preserve\nquietly keep in 1/2\nglobal PCST_SUBN = _N\nrestore\n"
                   "global PCST_SUBN2 = _N\n", fp);
        std::fclose(fp);
      }
      if (FILE* fp = std::fopen(leaveDo.c_str(), "w")) {
        std::fputs("preserve\nquietly keep in 1\nglobal PCST_LEAVEN = _N\n", fp);
        std::fclose(fp);
      }
      run(kFixture);
      run("preserve\nquietly drop in 1/2\ncapture noisily do \"" + subDo +
          "\"\nlocal rcS = _rc\nlocal nS = _N\ncapture noisily do \"" + leaveDo +
          "\"\nlocal rcL = _rc\nlocal nL = _N\ncapture noisily restore\nlocal rcR = _rc\n"
          "local nR = _N\n");
      const std::string g1 = session.eval.globals.get("PCST_SUBN", "<unset>");
      const std::string g2 = session.eval.globals.get("PCST_SUBN2", "<unset>");
      const std::string g3 = session.eval.globals.get("PCST_LEAVEN", "<unset>");
      // ST7: both halves in one check (w2ad pn_d_nested_pres, pn_d_leave_held).
      w2check(L("rcS") == "0" && g1 == "2" && g2 == "4" && L("nS") == "4" && L("rcL") == "0" &&
                  g3 == "1" && L("nL") == "4" && L("rcR") == "0" && L("nR") == "6",
              "W2 progctl/BUG-922 ST7 (help preserve; w2ad pn_d_nested_pres, "
              "pn_d_leave_held): a nested do-file preserves while its caller holds a copy and "
              "its restore gives back its own copy; the copy a nested do-file leaves preserved "
              "is restored when it concludes, and the caller's own restore then brings back the "
              "caller's copy. First cut: r(621); main: N 1, then the caller's restore gave N 4. "
              "Got " + L("rcS") + "/" + g1 + "/" + g2 + "/" + L("nS") + " " + L("rcL") + "/" + g3 +
                  "/" + L("nL") + "/" + L("rcR") + "/" + L("nR"));
    }
  }

  // ── BUG-922 round 3: a preserved copy belongs to the level that preserved ──
  // Oracle w2ad_prog_preserve_nest. One reach rule decides every form: only
  // the level that ran `preserve` may restore, cancel or re-arm its copy; a
  // deeper level's `restore`, `restore, not` and `restore, preserve` are
  // r(622) and leave the caller's copy alone; whatever a program or nested
  // do/run file leaves preserved is restored when it concludes; an included
  // file shares its includer's level.
  {
    piet::SessionState session(workdir);
    session.eval.cwd = workdir;
    session.cwd = workdir;
    auto run = [&](const std::string& src) {
      return piet::doexec::executeDoSource(src, session, piet::dispatch);
    };
    auto G = [&](const std::string& k) { return session.eval.globals.get(k, "<unset>"); };
    // R(tag) records rc:N:sum of the statement just run (globals, so inner
    // levels can record too); V(tag) reads it back.
    auto R = [](const std::string& tag) {
      return "global ST_" + tag + "_r = _rc\nglobal ST_" + tag + "_n = _N\nquietly summarize x\n"
             "global ST_" + tag + "_s = r(sum)\n";
    };
    auto V = [&](const std::string& tag) {
      return tag + "=" + G("ST_" + tag + "_r") + ":" + G("ST_" + tag + "_n") + ":" +
             G("ST_" + tag + "_s");
    };
    auto file = [&](const std::string& name, const std::string& body) {
      const std::string path = (fs::path(workdir) / name).u8string();
      if (FILE* fp = std::fopen(path.c_str(), "w")) {
        std::fputs(body.c_str(), fp);
        std::fclose(fp);
      }
      return path;
    };
    auto prog = [&](const std::string& name, const std::string& body) {
      run("capture program drop " + name + "\nprogram define " + name + "\n" + body + "end\n");
    };
    const std::string kPn = "clear\nquietly set obs 5\ngenerate double x = 2^(_n-1)\n";
    // The w2ad caller shape: preserve, drop in 1, the call, restore, restore.
    auto held = [&](const std::string& call) {
      run(kPn + "capture noisily preserve\n" + R("pres") + "capture noisily drop in 1\n" +
          R("drop") + call + "\n" + R("ret") + "capture noisily restore\n" + R("rest") +
          "capture noisily restore\n" + R("rest2"));
      return V("pres") + "|" + V("drop") + "|" + V("ret") + "|" + V("rest") + "|" + V("rest2");
    };
    // The bare caller that preserved nothing: the call, then a restore.
    auto bare = [&](const std::string& call) {
      run(kPn + call + "\n" + R("ret") + "capture noisily restore\n" + R("rest"));
      return V("ret") + "|" + V("rest");
    };

    // ST1, ST2, ST3: `restore, preserve`, `restore, not` and `restore` one
    // level below the copy's owner (a program, a do-file, a run-file) are
    // r(622) and touch nothing; the inner drop then stays, and the caller's
    // restore brings its copy back (w2ad pn_p_rp_caller, pn_d_rp_caller,
    // pn_r_rp_caller, pn_p_not_caller, pn_d_not_caller, pn_p_restore_caller,
    // pn_d_restore_caller: in1 622:4:30, in2 0:2:24, ret 0:2:24, rest 0:5:31,
    // rest2 622). c279eb79 re-armed the copy at the inner level and restored
    // it again when that level ended (rp: ret 0:5:31, rest 622), cancelled the
    // caller's copy (not: rest 622) and popped it (restore: rest 622); main
    // let the inner level take the copy (in1 0:5:31) and answered 1 or 198.
    {
      const std::string want =
          "pres=0:5:31|drop=0:4:30|ret=0:2:24|rest=0:5:31|rest2=622:5:31|in1=622:4:30|"
          "in2=0:2:24";
      std::string got;
      bool ok = true;
      for (const char* form : {"restore, preserve", "restore, not", "restore"}) {
        const std::string body = std::string("capture noisily ") + form + "\n" + R("in1") +
                                 "capture noisily drop in 1/2\n" + R("in2");
        prog("pcst_rc", body);
        const std::string f = file("pcst_rc.do", body);
        for (const std::string& call :
             {std::string("capture noisily pcst_rc"), "capture noisily do \"" + f + "\"",
              "capture noisily run \"" + f + "\""}) {
          const std::string rec = held(call) + "|" + V("in1") + "|" + V("in2");
          if (rec != want) {
            ok = false;
            got += std::string(" [") + form + " / " + call.substr(16, 6) + ": " + rec + "]";
          }
        }
      }
      w2check(ok,
              "W2 progctl/BUG-922 ST1-ST3 (w2ad pn_*_rp_caller, pn_*_not_caller, "
              "pn_*_restore_caller): a level that preserved nothing cannot restore, cancel or "
              "re-arm its caller's copy: r(622), and the caller's restore gets its copy. Want " +
                  want + ". Got" + got);
    }

    // ST4: `restore, not` with nothing preserved anywhere is r(622) at the top,
    // in a do-file and in a program (w2ad pn_top_none_not 622, pn_d_none_not,
    // pn_p_none_not: in1 622:5:31). Main: r(198).
    {
      const std::string body = "capture noisily restore, not\n" + R("in1");
      prog("pcst_nn", body);
      const std::string f = file("pcst_nn.do", body);
      run(kPn + "capture noisily restore, not\n" + R("top"));
      const std::string top = V("top");
      bare("capture noisily pcst_nn");
      const std::string inP = V("in1");
      bare("capture noisily do \"" + f + "\"");
      const std::string inD = V("in1");
      w2check(top == "top=622:5:31" && inP == "in1=622:5:31" && inD == "in1=622:5:31",
              "W2 progctl/BUG-922 ST4 (w2ad pn_top_none_not, pn_p_none_not, pn_d_none_not): "
              "`restore, not` with nothing preserved is r(622). Main: 198. Got " + top + " " +
                  inP + " " + inD);
    }

    // ST5: a second preserve in a program or do-file that holds its copy is
    // r(621); the restore brings back the first copy and a further one is
    // r(622); the caller holds nothing afterwards (w2ad pn_p_twice, pn_d_twice:
    // in1 0:5:31, in2 621:5:531, in3 0:5:31, in4 622:5:31, ret 0:5:31, rest
    // 622). Main stacked a second copy (in2 0, in3 0:5:531).
    {
      const std::string body = "capture noisily preserve\n" + R("in1") +
                               "quietly replace x = x + 100\ncapture noisily preserve\n" +
                               R("in2") + "capture noisily restore\n" + R("in3") +
                               "capture noisily restore\n" + R("in4");
      prog("pcst_tw", body);
      const std::string f = file("pcst_tw.do", body);
      const std::string want =
          "ret=0:5:31|rest=622:5:31|in1=0:5:31|in2=621:5:531|in3=0:5:31|in4=622:5:31";
      const std::string gp = bare("capture noisily pcst_tw") + "|" + V("in1") + "|" + V("in2") +
                             "|" + V("in3") + "|" + V("in4");
      const std::string gd = bare("capture noisily do \"" + f + "\"") + "|" + V("in1") + "|" +
                             V("in2") + "|" + V("in3") + "|" + V("in4");
      w2check(gp == want && gd == want,
              "W2 progctl/BUG-922 ST5 (w2ad pn_p_twice, pn_d_twice): r(621) at the inner level "
              "keeps the first copy. Want " + want + ". Got " + gp + " / " + gd);
    }

    // ST6: the copy a nested do/run file leaves preserved is restored when it
    // concludes -- on success, on error, after `exit`, under quietly -- and
    // nothing stays armed for the caller (w2ad pn_d_leave, pn_r_leave,
    // pn_d_leave_err (459), pn_d_leave_exit, pn_q_qdo_leave: ret 0:5:31, in2
    // 0:1:1, rest 622). Main kept the inner data (ret 0:1:1) and the caller's
    // restore then reloaded it.
    {
      const std::string body =
          "capture noisily preserve\n" + R("in1") + "capture noisily keep in 1\n" + R("in2");
      const std::string f = file("pcst_lv.do", body);
      const std::string fe = file("pcst_lve.do", body + "error 459\n");
      const std::string fx = file("pcst_lvx.do", body + "exit\n");
      const std::string want = "ret=0:5:31|rest=622:5:31|in2=0:1:1";
      std::string got;
      bool ok = true;
      for (const std::string& call :
           {"capture noisily do \"" + f + "\"", "capture noisily run \"" + f + "\"",
            "capture noisily do \"" + fx + "\"", "capture quietly do \"" + f + "\""}) {
        const std::string rec = bare(call) + "|" + V("in2");
        if (rec != want) {
          ok = false;
          got += " [" + call + ": " + rec + "]";
        }
      }
      const std::string recE = bare("capture noisily do \"" + fe + "\"") + "|" + V("in2");
      w2check(ok && recE == "ret=459:5:31|rest=622:5:31|in2=0:1:1",
              "W2 progctl/BUG-922 ST6 (w2ad pn_d_leave, pn_r_leave, pn_d_leave_exit, "
              "pn_q_qdo_leave, pn_d_leave_err): a nested do-file's leftover copy is restored "
              "when it concludes. Want " + want + ". Got" + got + " err: " + recE);
    }

    // ST8: a program or do-file preserves its own copy while the caller holds
    // one, and leaves it (w2ad pn_p_leave_held, pn_d_leave_held: in1 0:4:30,
    // in2 0:1:2, ret 0:4:30, rest 0:5:31, rest2 622); and the mixed nesting
    // of a program that runs such a do-file and then preserves itself (w2ad
    // pn_pd_leave: f1 0:4:30, f2 0:1:2, in1 0:4:30, in2 0:4:30, in3 0:3:28,
    // ret 0:4:30). Main: ret 0:1:2, and 0:0:0 for the mixed shape.
    {
      const std::string body =
          "capture noisily preserve\n" + R("in1") + "capture noisily keep in 1\n" + R("in2");
      prog("pcst_lh", body);
      const std::string f = file("pcst_lh.do", body);
      const std::string want = "pres=0:5:31|drop=0:4:30|ret=0:4:30|rest=0:5:31|rest2=622:5:31|"
                               "in1=0:4:30|in2=0:1:2";
      const std::string gp = held("capture noisily pcst_lh") + "|" + V("in1") + "|" + V("in2");
      const std::string gd =
          held("capture noisily do \"" + f + "\"") + "|" + V("in1") + "|" + V("in2");
      const std::string ff = file("pcst_lf.do", "capture noisily preserve\n" + R("f1") +
                                                    "capture noisily keep in 1\n" + R("f2"));
      prog("pcst_pd", "capture noisily do \"" + ff + "\"\n" + R("in1") +
                          "capture noisily preserve\n" + R("in2") + "capture noisily drop in 1\n" +
                          R("in3"));
      const std::string gm = held("capture noisily pcst_pd") + "|" + V("f1") + "|" + V("f2") +
                             "|" + V("in1") + "|" + V("in2") + "|" + V("in3");
      w2check(gp == want && gd == want &&
                  gm == "pres=0:5:31|drop=0:4:30|ret=0:4:30|rest=0:5:31|rest2=622:5:31|"
                        "f1=0:4:30|f2=0:1:2|in1=0:4:30|in2=0:4:30|in3=0:3:28",
              "W2 progctl/BUG-922 ST8 (w2ad pn_p_leave_held, pn_d_leave_held, pn_pd_leave): "
              "each level restores exactly its own copy. Got " + gp + " / " + gd + " / " + gm);
    }

    // ST9: an included file shares its includer's level: its `preserve` is
    // r(621) while the includer holds a copy, and its `restore, not` cancels
    // the includer's copy (w2ad pn_i_pres_held: in1 621:4:30, in2 0:1:2, ret
    // 0:1:2, rest 0:5:31; pn_i_not: in1 0:4:30, ret 0:2:24, rest 622). Main:
    // in1 0 (a second copy), and 198 for `restore, not`.
    {
      const std::string fp = file("pcst_ip.inc", "capture noisily preserve\n" + R("in1") +
                                                     "capture noisily keep in 1\n" + R("in2"));
      const std::string fn = file("pcst_in.inc", "capture noisily restore, not\n" + R("in1") +
                                                     "capture noisily drop in 1/2\n" + R("in2"));
      const std::string gp =
          held("capture noisily include \"" + fp + "\"") + "|" + V("in1") + "|" + V("in2");
      const std::string gn =
          held("capture noisily include \"" + fn + "\"") + "|" + V("in1") + "|" + V("in2");
      w2check(gp == "pres=0:5:31|drop=0:4:30|ret=0:1:2|rest=0:5:31|rest2=622:5:31|"
                    "in1=621:4:30|in2=0:1:2" &&
                  gn == "pres=0:5:31|drop=0:4:30|ret=0:2:24|rest=622:2:24|rest2=622:2:24|"
                        "in1=0:4:30|in2=0:2:24",
              "W2 progctl/BUG-922 ST9 (w2ad pn_i_pres_held, pn_i_not): an included file shares "
              "its includer's preserve level. Got " + gp + " / " + gn);
    }

    // ST13 (guard): 073's own shape, `restore, preserve` at the level that owns
    // the copy, re-arms it at that level (2_Descriptives.do:417,
    // 4_Simulations.do:253; w2ad pn_p_same_rp: in1..in4 0:5:31, ret 0:5:31,
    // rest 622). Main passed the inner steps and answered rc 1 for the
    // caller's restore.
    {
      prog("pcst_own", "capture noisily preserve\n" + R("in1") +
                           "forvalues k = 1/2 {\n    capture noisily drop in 1/`k'\n"
                           "    capture noisily restore, preserve\n    local j = `k' + 1\n" +
                           "    global ST_in`j'_r = _rc\n    global ST_in`j'_n = _N\n"
                           "    quietly summarize x\n    global ST_in`j'_s = r(sum)\n}\n" +
                           "capture noisily restore\n" + R("in4"));
      const std::string g = bare("capture noisily pcst_own") + "|" + V("in1") + "|" + V("in2") +
                            "|" + V("in3") + "|" + V("in4");
      w2check(g == "ret=0:5:31|rest=622:5:31|in1=0:5:31|in2=0:5:31|in3=0:5:31|in4=0:5:31",
              "W2 progctl/BUG-922 ST13 (w2ad pn_p_same_rp, guard): `restore, preserve` at the "
              "owning level re-arms the copy there. Got " + g);
    }

    // S3 sibling: a nested do-file's tempvar and tempname die when it
    // concludes (w2ad pn_tv_do_gone: ret 0:5:31, var=111, scalar=111). Main
    // and c279eb79 keyed them on program calls only: 0 and 0.
    {
      const std::string f = file("pcst_tv.do",
                                 "tempvar tv\ngenerate double `tv' = 1\nglobal PCST_DTV `tv'\n"
                                 "tempname ts\nscalar `ts' = 5\nglobal PCST_DTS `ts'\n");
      run(kPn + "capture noisily do \"" + f + "\"\n" + R("ret") +
          "capture confirm variable $PCST_DTV\nglobal ST_v = _rc\n"
          "capture confirm scalar $PCST_DTS\nglobal ST_s = _rc\n");
      w2check(V("ret") == "ret=0:5:31" && G("ST_v") == "111" && G("ST_s") == "111",
              "W2 progctl/BUG-922 S3 (w2ad pn_tv_do_gone): a nested do-file's tempvar and "
              "tempname are gone after it concludes. Main: 0|0. Got " + V("ret") + " var=" +
                  G("ST_v") + " scalar=" + G("ST_s"));
    }
  }

  // ── BUG-922 round 3, ST10: a `parallel` child reaches no frame below it ──
  // Design decision, no oracle: the package cannot run here (selftest K1c),
  // and its child is a separate Stata session, which has none of its
  // parent's preserved copies. The child's `restore` in every form is r(622);
  // the parent's data come back from the nodata snapshot and the parent's own
  // copy is untouched (TRANSITIONS P3, P4, P7, P8). Main and c279eb79 let the
  // child pop the snapshot or the parent's copy: data or a copy was lost.
  {
    auto runP = [&](const std::string& child, const std::string& tail) {
      piet::SessionState s(workdir);
      s.eval.cwd = workdir;
      s.cwd = workdir;
      const std::string f = (fs::path(workdir) / "pcst_pll.do").u8string();
      if (FILE* fp = std::fopen(f.c_str(), "w")) {
        std::fputs((child + "global PCST_CRC = _rc\n").c_str(), fp);
        std::fclose(fp);
      }
      piet::doexec::executeDoSource(
          "clear\nquietly set obs 5\ngenerate double x = 2^(_n-1)\nparallel initialize 1\n" +
              tail.substr(0, tail.find('@')) + "capture noisily parallel do \"" + f +
              "\", nodata processors(1)\nglobal PCST_A = string(_rc) + \":\" + string(_N)\n"
              "quietly summarize x\nglobal PCST_A = \"$PCST_A:\" + string(r(sum))\n"
              "capture noisily restore\nglobal PCST_R = string(_rc) + \":\" + string(_N)\n",
          s, piet::dispatch);
      return s.eval.globals.get("PCST_CRC", "<unset>") + "|" +
             s.eval.globals.get("PCST_A", "<unset>") + "|" +
             s.eval.globals.get("PCST_R", "<unset>");
    };
    const std::string kHeld = "preserve\nquietly drop in 1\n@";
    const std::string p3 = runP("capture noisily restore\n", kHeld);
    const std::string p4 = runP("capture noisily restore, not\n", "@");
    const std::string p7 = runP("capture noisily restore, preserve\n", kHeld);
    const std::string p8 = runP(
        "clear\nquietly set obs 3\ngenerate double x = 1\npreserve\nquietly drop in 1\n", "@");
    w2check(p3 == "622|0:4:30|0:5" && p4 == "622|0:5:31|622:5" && p7 == "622|0:4:30|0:5" &&
                p8 == "0|0:5:31|622:5",
            "W2 progctl/BUG-922 ST10 (TRANSITIONS P3/P4/P7/P8, design): a parallel child cannot "
            "restore, cancel or re-arm its parent's copies or the nodata snapshot, and the copy "
            "it leaves is restored when it ends. Got P3 " + p3 + " P4 " + p4 + " P7 " + p7 +
                " P8 " + p8);
  }

  // ── BUG-922: tempfile round trip keeps types and value labels (FU-752) ──
  {
    piet::SessionState session(workdir);
    auto run = [&](const std::string& src) {
      return piet::doexec::executeDoSource(src, session, piet::dispatch);
    };
    auto L = [&](const std::string& k) { return session.eval.locals.get(k, "<unset>"); };
    run(kFixture);
    run("tempfile ta tb\nquietly save `ta'\nquietly drop in 1/2\nquietly use `ta', clear\n"
        "local u1 : type d\nlocal u3 : type l\nlocal u4 : type b\nlocal u5 : type i\n"
        "capture local u6 : type s\nlocal ul3 : value label b\nlocal ul1 : variable label d\n"
        "local uf1 : format d\ncapture decode b, generate(bd)\nquietly count if bd == \"zero\"\n"
        "local bd1 = r(N)\n"
        "capture drop bd\nquietly save `tb'\nquietly use `tb', clear\nlocal v1 : type d\n"
        "capture local v8 : type s12\n");
    w2check(L("u1") == "double" && L("u3") == "long" && L("u4") == "byte" && L("u5") == "int" &&
                L("u6") == "str5",
            "W2 progctl/BUG-922 (pc_tf_use_types, FU-752): the tempfile round trip keeps "
            "double/long/byte/int/str5. Main: float everywhere, str# refused. Got " + L("u1") + "/" +
                L("u3") + "/" + L("u4") + "/" + L("u5") + "/" + L("u6"));
    w2check(L("ul3") == "bl" && L("bd1") == "2" && L("ul1") == "Big double" &&
                L("uf1") == "%15.0f",
            "W2 progctl/BUG-922 (pc_tf_use_labels, pc_tf_use_formats): the value-label attachment "
            "and definitions come back with the variable label and format. Main: no value label. "
            "Got [" + L("ul3") + "] [" + L("bd1") + "] [" + L("ul1") + "] [" + L("uf1") + "]");
    w2check(L("v1") == "double" && L("v8") == "str12",
            "W2 progctl/BUG-922 (pc_tf_rehop): a second hop keeps the types. Main: float, str12 "
            "refused. Got " + L("v1") + "/" + L("v8"));
  }

  // ── BUG-922: merge/append using a tempfile read its storage types ───────
  {
    piet::SessionState session(workdir);
    auto run = [&](const std::string& src) {
      return piet::doexec::executeDoSource(src, session, piet::dispatch);
    };
    auto L = [&](const std::string& k) { return session.eval.locals.get(k, "<unset>"); };
    run(kFixture);
    run("tempfile tm ta\nquietly keep i\nquietly generate double m = i/3\nquietly save `tm'\n"
        "quietly keep in 1/2\nquietly replace m = m + 0.1\nquietly rename m g\n"
        "quietly save `ta'\n");
    run(kFixture);
    run("quietly merge 1:1 i using `tm'\nlocal mt : type m\n"
        "quietly generate float g = 0.5\nquietly append using `ta'\nlocal at : type g\nlocal an = r(N)\n");
    w2check(L("mt") == "double",
            "W2 progctl/BUG-922 (pc_tf_merge): a variable merged in from a tempfile keeps its "
            "double type. Main: float. Got " + L("mt"));
    w2check(L("an") == "2",
            "append describes the last using tempfile and publishes its actual row count");
    w2check(L("at") == "double",
            "W2 progctl/BUG-922 (BUG-666 append promotion, StataBE 2026-09-27: float+double -> "
            "double): append using a tempfile reads the using side's double. Main: float. Got " +
                L("at"));
  }

  // ── BUG-922: tempvar/tempname die with their program ────────────────────
  {
    piet::SessionState session(workdir);
    auto run = [&](const std::string& src) {
      return piet::doexec::executeDoSource(src, session, piet::dispatch);
    };
    auto L = [&](const std::string& k) { return session.eval.locals.get(k, "<unset>"); };
    run(kFixture);
    run("capture program drop pcst_tmp\nprogram define pcst_tmp\n    tempvar t\n"
        "    tempname sc mt\n    quietly generate double `t' = i/4\n    scalar `sc' = 2\n"
        "    matrix `mt' = (5, 6)\n    scalar pcst_m = el(`mt', 1, 2) + `sc'\n"
        "    global PCST_TV \"`t'\"\n    global PCST_TN \"`sc'\"\n    global PCST_TM \"`mt'\"\nend\n"
        "pcst_tmp\nlocal tm = pcst_m\ncapture confirm variable $PCST_TV\nlocal g1 = _rc\n"
        "capture confirm scalar $PCST_TN\nlocal g2 = _rc\ncapture confirm matrix $PCST_TM\n"
        "local g3 = _rc\n");
    w2check(L("tm") == "8" && L("g1") == "111" && L("g2") == "111" && L("g3") == "111",
            "W2 progctl/BUG-922 (pc_tv_gone): the program's tempvar, tempname scalar and matrix "
            "are gone after it returns. Main: 0|0|0. Got " + L("tm") + "/" + L("g1") + "/" +
                L("g2") + "/" + L("g3"));
  }

  // ── BUG-923: args at do-file level; program drop/redefine ───────────
  {
    piet::SessionState session(workdir);
    auto run = [&](const std::string& src) {
      return piet::doexec::executeDoSource(src, session, piet::dispatch);
    };
    auto L = [&](const std::string& k) { return session.eval.locals.get(k, "<unset>"); };
    run("capture local 1 \"alpha\"\ncapture local 2 \"b c\"\ncapture noisily args pa pb pc\n");
    w2check(L("pa") == "alpha" && L("pb") == "b c" && L("pc") == "",
            "W2 progctl/BUG-923 (pc_pg_args_top): `local 1 ...` defines a positional local, so a "
            "do-file-level args reads it. Main: refused, pa empty. Got [" + L("pa") + "] [" +
                L("pb") + "]");

    run(kFixture);  // data in memory: an unknown command is then r(199), not no-data r(111)
    run("capture noisily program drop pcst_never\nlocal rcD = _rc\n"
        "capture program drop pcst_add\nprogram define pcst_add\n    scalar pcst_r = 1\nend\n"
        "capture noisily program define pcst_add\nlocal rcR = _rc\ncapture noisily pcst_add\n"
        "local rcC = _rc\ncapture noisily prog drop _all\nlocal rcA = _rc\ncapture pcst_add\n"
        "local rcG = _rc\n");
    w2check(L("rcD") == "111" && L("rcR") == "110" && L("rcC") == "0" && L("rcA") == "0" &&
                L("rcG") == "199",
            "W2 progctl/BUG-923 (pc_pg_capdrop_absent, pc_pg_redefine, pc_pg_prog_drop_all): drop "
            "of an absent program r(111); a define of an existing one r(110) keeping the old; "
            "`prog drop _all` drops it. Main: 0/199/0/0/0. Got " + L("rcD") + "/" + L("rcR") + "/" +
                L("rcC") + "/" + L("rcA") + "/" + L("rcG"));
  }

  fs::remove_all(workdir, ec);
}
