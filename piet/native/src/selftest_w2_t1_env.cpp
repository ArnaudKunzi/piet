// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// AER tier-1 (2026-10-28), lane ENV (macro require _dots postutil markout lrtest):
// checks against the StataBE 19.5 captures tests/eval/golden/aer_w2/
// w2ax_t1_official.log / w2ax_t1_community.log. Every check names the oracle
// arm (PREC|<arm>|) or log line it mirrors; the few that rest on the lane's own
// StataBE probes (envp1-3, run 2026-10-28 on the same StataBE) say so and quote
// the Stata answer. The display checks pin text the PREC records do not carry
// (the log's printed lines). The fixtures are the oracle do-file's own lines.
#include "selftest_w2.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "piet/dispatcher.h"
#include "piet/doexec.h"
#include "piet/session_state.h"

namespace {

using ojson = nlohmann::ordered_json;

std::vector<std::string> displayTexts(const ojson& blocks) {
  std::vector<std::string> out;
  if (!blocks.is_array()) return out;
  for (const auto& b : blocks)
    if (b.is_object() && b.value("type", std::string()) == "display")
      out.push_back(b.value("text", std::string()));
  return out;
}

std::string errorMessages(const ojson& blocks) {
  std::string out;
  if (!blocks.is_array()) return out;
  for (const auto& b : blocks)
    if (b.is_object() && b.value("type", std::string()) == "error")
      out += b.value("message", std::string()) + "|";
  return out;
}

bool hasText(const std::vector<std::string>& v, const std::string& want) {
  for (const auto& t : v)
    if (t == want || t.find(want) != std::string::npos) return true;
  return false;
}

bool near(double a, double b, double rtol = 1e-9) {
  if (std::isnan(a) || std::isnan(b)) return false;
  return std::fabs(a - b) <= rtol * std::max(std::fabs(a), std::fabs(b));
}

}  // namespace

void testW2T1Env() {
  std::printf("[selftest] --- AER tier-1 ENV: macro require _dots postutil markout lrtest ---\n");
  namespace fs = std::filesystem;
  const std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_t1env").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  piet::SessionState session(workdir);
  auto run = [&](const std::string& src) {
    return piet::doexec::executeDoSource(src, session, piet::dispatch).blocks;
  };
  auto G = [&](const std::string& k) { return session.eval.globals.get(k, "<unset>"); };
  auto L = [&](const std::string& k) { return session.eval.locals.get(k, "<unset>"); };
  auto R = [&](const std::string& k) {
    const double* v = session.eval.rValues.find(k);
    return v ? *v : std::nan("");
  };

  // ── macro ─────────────────────────────────────────────────────────────────
  run("global t1G 1\nlocal t1L 2\ncapture noisily macro drop t1G\nlocal rc = _rc\n");
  w2check(L("rc") == "0" && G("t1G") == "<unset>" && L("t1L") == "2",
          "T1 Env macro (macro_drop_global `|0||2|`): `macro drop t1G` drops the global, the "
          "local survives. Got rc=" + L("rc") + " G=" + G("t1G") + " L=" + L("t1L"));

  run("global t1A a\nglobal t1B b\nglobal file_r21 a\nglobal file_r22 b\nglobal file_x c\n"
      "macro drop t1A t1B\nmacro drop file_r2*\ncapture noisily macro drop t1nosuchmacro\n"
      "local rcm = _rc\n");
  w2check(G("t1A") == "<unset>" && G("t1B") == "<unset>" && G("file_r21") == "<unset>" &&
              G("file_r22") == "<unset>" && G("file_x") == "c" && L("rcm") == "0",
          "T1 Env macro (macro_drop_two `|0|||`, macro_drop_wild `|0|||c|`, macro_drop_missing "
          "`|0|`): two names, a `prefix*` pattern, and a missing name silently. Got file_x=" +
              G("file_x") + " rc=" + L("rcm"));

  run("local t1LL 5\nmacro drop _t1LL\nlocal q1 a\nmacro drop _q*\n");
  w2check(L("t1LL") == "<unset>" && L("q1") == "<unset>",
          "T1 Env macro (macro_drop_local `|0||`; envp1 md_locstar `|0||`): `_name` and `_q*` "
          "drop locals");

  run("macro define t1M \"x y\"\nmacro define t1N 3 + 4\nmacro define _ld 3 + 4\n"
      "macro define gd = 3 + 4\n");
  w2check(G("t1M") == "x y" && G("t1N") == "3 + 4" && L("ld") == "3 + 4" && G("gd") == "7",
          "T1 Env macro (macro_define `|0|x y|`, macro_define_expr `|0|3 + 4|`; envp1 "
          "mdef_local `3 + 4`, mdef_eq `7`): define stores as `global`/`local` would. Got [" +
              G("t1M") + "] [" + G("t1N") + "] [" + L("ld") + "] [" + G("gd") + "]");

  run("global todrop \"g1 g2\"\nglobal g1 one\nglobal g2 two\nmacro drop $todrop\n");
  w2check(G("g1") == "<unset>" && G("g2") == "<unset>" && G("todrop") == "g1 g2",
          "T1 Env macro (corpus `macro drop $name` shape): a line holding a macro reference is "
          "answered after expansion -- it drops the names $todrop holds");

  run("capture program drop t1shiftp\nprogram define t1shiftp\n    capture noisily macro shift\n"
      "    local src = _rc\n    global T1S \"`src'|`1'|`2'|`3'|`0'\"\nend\nt1shiftp p q r\n"
      "capture program drop t1shiftp2\nprogram define t1shiftp2\n    capture noisily macro shift 2\n"
      "    local src = _rc\n    global T1S2 \"`src'|`1'|`2'|`3'\"\nend\nt1shiftp2 p q r s\n"
      "capture noisily macro shift\nlocal rco = _rc\n");
  w2check(G("T1S") == "0|q|r||p q r" && G("T1S2") == "0|r|s|" && L("rco") == "0",
          "T1 Env macro (macro_shift `|0|q|r||p q r|`, macro_shift2 `|0|r|s||`, "
          "macro_shift_outside `|0|`): numbered locals shift, `0` is kept. Got [" + G("T1S") +
              "] [" + G("T1S2") + "] rc=" + L("rco"));

  {
    ojson b = run("global t1LST hello world\nlocal t1L 2\ncapture noisily macro list t1LST\n"
                  "capture noisily macro list _t1L\n");
    const auto t = displayTexts(b);
    w2check(hasText(t, "t1LST:          hello world") && hasText(t, "_t1L:           2"),
            "T1 Env macro (macro_list log `t1LST:          hello world`, macro_list_local "
            "`_t1L:           2`): the name and colon padded to column 16");
  }
  {
    ojson b = run("global abcdefghijklmno x15\nmacro list abcdefghijklmno\n");
    w2check(hasText(displayTexts(b), "abcdefghijklmno:\n                x15"),
            "T1 Env macro (envp1 log: a 16-character label puts the value on the next line, "
            "indented 16)");
  }
  {
    ojson b = run("capture noisily macro list t1nosuch\nlocal rcl = _rc\n"
                  "capture noisily macro bogus t1K1\nlocal rcb = _rc\n");
    w2check(L("rcl") == "111" && errorMessages(b).find("global macro $t1nosuch not found") !=
                                      std::string::npos &&
                L("rcb") == "198",
            "T1 Env macro (macro_list_missing `|111|` `global macro $t1nosuch not found`; "
            "macro_badsub `|198|`). Got rc=" + L("rcl") + "/" + L("rcb"));
  }
  run("global t1K1 k1\nglobal t1K2 k2\nglobal S_mine mine\nlocal t1KL kl\n");
  session.eval.globals.set("__piet_texdoc_path", "/tmp/kept.tex");
  run("capture noisily macro drop _all\nlocal rca = _rc\n");
  w2check(G("t1K1") == "<unset>" && G("t1K2") == "<unset>" && L("t1KL") == "<unset>" &&
              G("S_mine") == "mine" && G("__piet_texdoc_path") == "/tmp/kept.tex" &&
              L("rca") == "0",
          "T1 Env macro (macro_drop_all `|0||||`; envp3 sdrop `|mine|`): _all drops user "
          "globals and locals, keeps `S_*` and PIET's own __piet state. Got S_mine=" +
              G("S_mine") + " rc=" + L("rca"));
  session.eval.globals.erase("__piet_texdoc_path");

  // ── require ───────────────────────────────────────────────────────────────
  {
    ojson b = run("capture noisily require estout>=3.23, install\nlocal r1 = _rc\n"
                  "capture noisily require distinct==1.2.1\nlocal r2 = _rc\n"
                  "capture noisily require estout\nlocal r3 = _rc\n"
                  "capture noisily require estout distinct\nlocal r4 = _rc\n");
    w2check(L("r1") == "0" && L("r2") == "0" && L("r3") == "0" &&
                hasText(displayTexts(b), "(require estout>=3.23: PIET bundles its own implementations"),
            "T1 Env require (require_ge, require_eq, require_bare `|0|`): rc 0 with the "
            "accept-note. Got " + L("r1") + L("r2") + L("r3"));
    w2check(L("r4") == "9" && errorMessages(b).find("assert failed: inlist(\"distinct\", \">=\", "
                                                    "\"==\", \"\")") != std::string::npos,
            "T1 Env require (require_two `|9|`, log `assert failed: inlist(\"distinct\", ...)`): "
            "a second package name is read as the operator. Got rc=" + L("r4"));
  }

  // ── _dots ─────────────────────────────────────────────────────────────────
  {
    ojson b = run("capture noisily _dots 0, title(Loop running) reps(3)\nlocal rd = _rc\n");
    const auto t = displayTexts(b);
    w2check(L("rd") == "0" && t.size() == 2 && t[0] == "Loop running (3)" &&
                t[1] == "----+--- 1 ---+--- 2 ---+--- 3 ---+--- 4 ---+--- 5 ",
            "T1 Env _dots (dots_header `|0|`, log `Loop running (3)` + the ruler)");
    ojson m = piet::dispatch("_dots 1 0", session);
    ojson x = piet::dispatch("_dots 4 1", session);
    auto first = [](const ojson& arr) -> ojson {
      for (const auto& e : arr)
        if (e.is_object() && e.value("type", std::string()) == "display") return e;
      return ojson();
    };
    const ojson dm = first(m), dx = first(x);
    w2check(dm.value("text", std::string()) == "." && dm.value("continue", false) &&
                dx.value("text", std::string()) == "x" && dx.value("continue", false),
            "T1 Env _dots (dots_loop log `...`, dots_x log `x`): one mark per call, no newline "
            "(the display block's continue flag)");
    ojson loop = run("forvalues i = 1/3 {\n    _dots `i' 0\n}\ndisplay \"\"\n");
    w2check(hasText(displayTexts(loop), "..."),
            "T1 Env _dots (dots_loop log line `...`): three marks share one line");
    ojson f = piet::dispatch("_dots 50 0", session);
    const ojson df = first(f);
    w2check(df.value("text", std::string()) == ".    50" && !df.value("continue", false),
            "T1 Env _dots (envp2 dots_50 log `.    50`): the 50th mark closes the line with "
            "%5.0fc");
  }

  // ── postutil ──────────────────────────────────────────────────────────────
  {
    const std::string pf1 = (fs::path(workdir) / "t1pf1").u8string();
    const std::string pf2 = (fs::path(workdir) / "t1pf2").u8string();
    run("postfile t1h1 a using \"" + pf1 + "\", replace\npost t1h1 (1)\npost t1h1 (2)\n"
        "capture noisily postutil clear\nlocal pc = _rc\ncapture noisily postutil clear\n"
        "local pe = _rc\n");
    w2check(L("pc") == "0" && L("pe") == "0" && session.postfiles.empty() &&
                fs::exists(pf1 + ".dta"),
            "T1 Env postutil (postutil_clear `|0|`, postutil_clear_empty `|0|`; envp1 pu_file "
            "`|0|`): clear CLOSES every handle -- the file is written, not discarded");
    run("use \"" + pf1 + "\", clear\nlocal pn = _N\n");
    w2check(L("pn") == "2",
            "T1 Env postutil (envp1 pu_desc `|0|2|`): the closed file holds the two posted rows. "
            "Got N=" + L("pn"));
    ojson d = run("postfile t1h2 a using \"" + pf2 + "\", replace\ncapture noisily postutil dir\n"
                  "local pd = _rc\npostclose t1h2\ncapture noisily postutil bogus\nlocal pb = _rc\n"
                  "capture noisily postutil d\nlocal pdd = _rc\n");
    w2check(L("pd") == "0" && hasText(displayTexts(d), "t1h2") && L("pb") == "198" &&
                L("pdd") == "198",
            "T1 Env postutil (postutil_dir `|0|` log `t1h2`, postutil_bad `|198|`; envp1 pu_d "
            "`|198|`)");
  }

  // ── markout ───────────────────────────────────────────────────────────────
  run("clear\nset obs 8\ngenerate y = _n\nreplace y = . in 2\ngenerate x = _n\n"
      "replace x = . in 5\ngenerate str2 s = \"q\"\nreplace s = \"\" in 7\n"
      "capture program drop t1mk\nprogram define t1mk\n    syntax varlist\n    marksample touse\n"
      "    capture noisily markout `touse' x s\n    local __rc = _rc\n    quietly count if `touse'\n"
      "    local n = r(N)\n    global T1MK \"`__rc'|`n'\"\n    marksample touse2\n"
      "    capture noisily markout `touse2' x s, strok\n    local __rc = _rc\n"
      "    quietly count if `touse2'\n    local n = r(N)\n    global T1MK2 \"`__rc'|`n'\"\nend\nt1mk y\n"
      "generate byte t1u = 1\ncapture noisily markout t1u x\nlocal mo1 = _rc\n"
      "quietly count if t1u\nlocal mo1n = r(N)\ncapture noisily markout t1u s\nlocal mo2 = _rc\n"
      "quietly count if t1u\nlocal mo2n = r(N)\n");
  w2check(G("T1MK") == "0|0" && G("T1MK2") == "0|5",
          "T1 Env markout (markout_prog `|0|0|`, markout_strok `|0|5|`): inside a program; a "
          "string variable zeroes every row unless strok. Got [" + G("T1MK") + "] [" +
              G("T1MK2") + "]");
  w2check(L("mo1") == "0" && L("mo1n") == "7" && L("mo2") == "0" && L("mo2n") == "0",
          "T1 Env markout (markout_outside `|0|7|`, markout_str_outside `|0|0|`): outside a "
          "program too. Got " + L("mo1") + "/" + L("mo1n") + " " + L("mo2") + "/" + L("mo2n"));
  run("generate double m2 = 5\ncapture noisily markout m2 x\nlocal mb = _rc\nquietly summarize m2\n"
      "local mbmin = r(min)\n");
  w2check(L("mb") == "198" && L("mbmin") == "5",
          "T1 Env markout (envp2 mo_nonbyte `|198|5|5|4|` `m2 not byte`): a non-byte markvar is "
          "refused and left as it was. Got rc=" + L("mb") + " min=" + L("mbmin"));

  // ── lrtest ────────────────────────────────────────────────────────────────
  run("clear\nset obs 80\ngenerate x1 = mod(_n*7,11) + mod(_n,5)/10\n"
      "generate x2 = mod(_n*3,5) + mod(_n*13,7)/10\ngenerate x3 = mod(_n*11,9)/3\n"
      "generate y  = 1 + 0.5*x1 - 0.3*x2 + mod(_n*17,23)/4\ngenerate byte d = (y > 6)\n"
      "quietly regress y x1\nestimates store t1r1\nquietly regress y x1 x2 x3\n"
      "estimates store t1r3\n");
  {
    ojson b = run("capture noisily lrtest t1r3 t1r1\nlocal lr = _rc\n");
    w2check(L("lr") == "0" && near(R("chi2"), 5.465765009417908) && R("df") == 2 &&
                near(R("p"), .0650315654465017),
            "T1 Env lrtest (lrtest_regress `|0| 5.465765009417908|2| .0650315654465017|`)");
    const auto t = displayTexts(b);
    w2check(hasText(t, "\nLikelihood-ratio test\nAssumption: t1r1 nested within t1r3\n\n"
                       " LR chi2(2) =   5.47\nProb > chi2 = 0.0650"),
            "T1 Env lrtest (lrtest_regress log lines): the test block as Stata prints it");
  }
  run("capture noisily lrtest t1r1 t1r3\n");
  w2check(near(R("chi2"), 5.465765009417908) && R("df") == 2 && near(R("p"), .0650315654465017),
          "T1 Env lrtest (lrtest_reverse): the order of the names does not matter");
  run("capture noisily lrtest t1r3 t1r1, df(5)\n");
  w2check(R("df") == 5 && near(R("p"), .3617144774636523),
          "T1 Env lrtest (lrtest_df `|5| .3617144774636523|`): df() overrides the rank difference");
  run("quietly logit d x1\nestimates store t1l1\nquietly logit d x1 x2\nestimates store t1l2\n"
      "capture noisily lrtest t1l2 t1l1\n");
  w2check(near(R("chi2"), 1.997545696783433) && R("df") == 1 && near(R("p"), .1575541415593047),
          "T1 Env lrtest (lrtest_logit `|0| 1.997545696783433|1| .1575541415593047|`)");
  {
    ojson b = run("capture noisily lrtest t1l1\nlocal lc = _rc\n");
    w2check(L("lc") == "0" && near(R("chi2"), 1.997545696783433) &&
                hasText(displayTexts(b), "Assumption: t1l1 nested within t1l2"),
            "T1 Env lrtest (lrtest_current, log `Assumption: t1l1 nested within t1l2`): the "
            "current results go by their stored name");
  }
  {
    ojson b = run("capture noisily lrtest t1r3 t1l1\nlocal lm = _rc\n");
    w2check(L("lm") == "498" &&
                errorMessages(b).find("test involves different estimators: regress vs. logit") !=
                    std::string::npos,
            "T1 Env lrtest (lrtest_mixed `|498|`, log `test involves different estimators: "
            "regress vs. logit`). Got rc=" + L("lm"));
  }
  run("capture noisily lrtest t1r3 t1l1, force\nlocal lf = _rc\n");
  w2check(L("lf") == "0" && near(R("chi2"), -222.6225071939842) && R("df") == 2 && R("p") == 1,
          "T1 Env lrtest (lrtest_mixed_force `|0|-222.6225071939842|2|`, log `Prob > chi2 =  "
          "1.0000`): the larger rank is unrestricted, chi2 = 2*(ll_u - ll_r)");
  {
    ojson b = run("capture noisily lrtest t1r3 t1nosuch\nlocal ln = _rc\n"
                  "quietly regress y x1 x2 if x3 > 1\nestimates store t1rs\n"
                  "capture noisily lrtest t1rs t1r1\nlocal lN = _rc\n");
    w2check(L("ln") == "111" && L("lN") == "498" &&
                errorMessages(b).find("observations differ: 45 vs. 80") != std::string::npos,
            "T1 Env lrtest (lrtest_missing `|111|`, lrtest_diffN `|498|` log `observations "
            "differ: 45 vs. 80`). Got " + L("ln") + "/" + L("lN"));
  }
  run("capture noisily lrtest t1r1 t1r1\nlocal ls = _rc\n");
  w2check(L("ls") == "198" && std::isnan(R("chi2")),
          "T1 Env lrtest (envp3 lr_self `|198|` `specified more than once`): and a failed lrtest "
          "leaves r() empty, as the rclass program does");

  // ── review fixes (coordinator's review of 2026-10-28, StataBE logs in the
  // review's l1/l3/l4/k1/d1/m1/m2 do-files) ──────────────────────────────────
  {
    // l4 S1/S2: after lrtest, an r() it did not post reads as `.` through the
    // expansion layer -- the do-file goes on (before the fix: r(111), stopped).
    piet::doexec::ExecuteResult er = piet::doexec::executeDoSource(
        "capture lrtest t1r3 t1nosuch\nlocal pv = r(p)\nlrtest t1r3 t1r1\nlocal rn = r(N)\n"
        "local rp = r(p)\n",
        session, piet::dispatch);
    w2check(!er.stoppedAtLine.has_value() && L("pv") == "." && L("rn") == "." &&
                L("rp") != "." && L("rp") != "<unset>",
            "T1 Env lrtest (review l4 S2 `|.|`, S1 `|.|`): r() lrtest did not post is `.` read "
            "through macro expansion, after a failure and after a success. Got pv=" + L("pv") +
                " rn=" + L("rn") + " rp=" + L("rp"));
    // l1 L2: lrtest.ado's double save.
    auto num = [](const std::string& t) {
      try {
        return std::stod(t);
      } catch (...) {
        return std::nan("");
      }
    };
    w2check(G("S_3") == "2" && near(num(G("S_6")), 5.465765009417908) &&
                near(num(G("S_7")), .0650315654465017) && G("S_7").rfind(".0650315654", 0) == 0,
            "T1 Env lrtest (review l1 L2 `|2|5.465765009417908|.0650315654465017|`): $S_3 $S_6 "
            "$S_7 hold df, chi2, p. Got " + G("S_3") + "|" + G("S_6") + "|" + G("S_7"));
    // l1 L15: d() is df().
    run("capture noisily lrtest t1r3 t1r1, d(5)\nlocal ld5 = _rc\n");
    w2check(L("ld5") == "0" && R("df") == 5,
            "T1 Env lrtest (review l1 L15 `|0|5|`): `d(5)` is df(5). Got rc=" + L("ld5"));
    // l3 H1: df(1e300) answers at once, with the lexical-integer note.
    ojson h = run("capture noisily lrtest t1r3 t1r1, df(1e300)\nlocal lh = _rc\n");
    w2check(L("lh") == "0" && R("df") == 1e300 && R("p") == 1 &&
                hasText(displayTexts(h), "note: specified degrees of freedom (1e300) is not an "
                                         "integer."),
            "T1 Env lrtest (review l3 H1 `|0|1.00e+300|1|`): a huge df() terminates, p = 1, with "
            "Stata's note. Got rc=" + L("lh"));
  }
  {
    // k1 K1/K4: unique-prefix abbreviations of the markvar and of the varlist.
    run("clear\nset obs 6\ngenerate xvar = _n\nreplace xvar = . in 2\ngenerate byte t = 1\n"
        "generate byte tmark = 1\ncapture noisily markout t xv\nlocal k1 = _rc\n"
        "quietly count if t\nlocal k1n = r(N)\ncapture noisily markout tma xvar\nlocal k4 = _rc\n"
        "quietly count if tmark\nlocal k4n = r(N)\n");
    w2check(L("k1") == "0" && L("k1n") == "5" && L("k4") == "0" && L("k4n") == "5",
            "T1 Env markout (review k1 K1 `|0|5|` `markout t xv`, K4 `|0|5|` `markout tma "
            "xvar`): abbreviations resolve. Got " + L("k1") + "/" + L("k1n") + " " + L("k4") + "/" +
                L("k4n"));
  }
  {
    // m1 m5/m5b, m2 b4/b3: de:fine is the minimum; a lone `d` is r(198).
    run("capture noisily macro def abv 5\ncapture noisily macro de abv2 6\n"
        "capture noisily macro defi q1 1\ncapture noisily macro d keep1\nlocal md = _rc\n");
    w2check(G("abv") == "5" && G("abv2") == "6" && G("q1") == "1" && L("md") == "198",
            "T1 Env macro (review m1 m5 `|0|5|`, m5b `|0|6|`, m2 b4 `|0|1|`, b3 `|198|`): "
            "`macro de`/`def`/`defi` define. Got " + G("abv") + G("abv2") + G("q1") + " d=" +
                L("md"));
  }
  {
    // d1 R1/R14/R9/R6: `require stata` needs an operator and a decimal version.
    run("capture noisily require stata\nlocal q1 = _rc\ncapture noisily require stata>=0\n"
        "local q2 = _rc\ncapture noisily require stata>=1e1\nlocal q3 = _rc\n"
        "capture noisily require stata==17.0\nlocal q4 = _rc\n");
    w2check(L("q1") == "198" && L("q2") == "9" && L("q3") == "198" && L("q4") == "0",
            "T1 Env require (review d1 R1 `|198|`, R14 `|9|`, R9 `|198|`, R6 `|0|`). Got " +
                L("q1") + "/" + L("q2") + "/" + L("q3") + "/" + L("q4"));
  }

  fs::remove_all(workdir, ec);
}
