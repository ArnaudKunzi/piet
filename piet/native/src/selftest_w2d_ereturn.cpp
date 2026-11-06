// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// W2D-T01 (2026-10-11) -- `ereturn clear`, `ereturn post`, `ereturn local`
// (handlers_ereturn.cpp). Before this, only `ereturn list` existed: `ereturn
// clear` answered no_data_loaded with nothing in memory and
// unsupported_command (rc 199) with data, so the golden do-file stopped at its
// first helper, and with the clear wrapped in capture the PREVIOUS model stayed
// in e() (BUG-732).
// Oracle: tests/eval/golden/aer_w2/w2d_uncovered.log, cited per arm; the
// stanzas and the helper programs are copied verbatim from w2d_uncovered.do.
// Second oracle: tests/eval/golden/aer_w2/w2_esample.log G5 (1487) and G11
// (1634), the top-level forms.
// Seeded-RED on the frozen main binary: (1)-(3), (5), (6), (7) and (8) (the
// helper or the post line fails there, so the record never prints or prints
// the old model).
// Round 2 (2026-10-12), arms (11)-(17): BUG-733, a posted e() (`ereturn post`,
// estpost) is superseded by the next real fit, and top-level `ereturn local`
// is r(152) (w2p_estim_ev.log:917). Each fails on d1e599ea or on frozen main.
// Round 3 (2026-10-12), arms (19)-(28): REDIAGNOSIS.md (internal capture
// w2d-t01-ereturn) S4 and the FU-375 leg. An `ereturn local` written
// with no posting live sits on top of the fit (or of an empty e()) and ends at
// the next model install (help return: e() results persist
// until a new model is fitted); test/lincom/nlcom/erepost of the same
// model leave e() alone (help test, help lincom,
// help nlcom, help ereturn); and the e() a pca/oaxaca fit
// wrote before its install ends at the next install too (FU-375, BUG-734).
// Every arm but the controls -- (21) PKEEP/PTITLE, (22), the (24) esttab
// identity and (28) OR2 -- fails on b3dcdae9.
// The e(sample) field of the ereturn_post records: arm (4b), the posted mask
// (handlers_ereturn.cpp installPostedMask, SessionState::ePostedEsampleSerial).
#include "selftest_w2.h"

#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>
#include <vector>

#include "piet/dispatcher.h"
#include "piet/doexec.h"
#include "piet/eval/errors.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

namespace {

int gEreturnChecks = 0;
void erCheck(bool cond, const std::string& label) {
  ++gEreturnChecks;
  w2check(cond, label);
}

// w2d_uncovered.do's helpers, verbatim (do lines 22-49).
const char* kHelpers =
    "capture program drop w2d_rclear\n"
    "program define w2d_rclear, rclass\n"
    "    return clear\n"
    "end\n"
    "capture program drop w2d_mkpanel\n"
    "program define w2d_mkpanel, eclass\n"
    "    ereturn clear\n"
    "    estimates clear\n"
    "    clear\n"
    "    set obs 60\n"
    "    generate id = ceil(_n/6)\n"
    "    generate t = mod(_n-1,6) + 1\n"
    "    generate g = mod(id,5) + 1\n"
    "    generate c = mod(_n,3) + 1\n"
    "    generate w1 = mod(_n*_n*7919 + _n*1313, 10007)/10007\n"
    "    generate w2 = mod(_n*_n*4659 + _n*1717, 10007)/10007\n"
    "    generate z1 = mod(_n*_n*8806 + _n*1919, 10007)/10007 + id/20\n"
    "    generate z2 = mod(_n*_n*5034 + _n*2323, 10007)/10007\n"
    "    generate x1 = z1 + 0.5*z2 + mod(_n*_n*142 + _n*2929, 10007)/10007\n"
    "    generate x2 = mod(_n*_n*4729 + _n*3131, 10007)/10007\n"
    "    generate x3 = mod(_n*_n*493 + _n*3737, 10007)/10007\n"
    "    generate x4 = mod(_n*_n*7949 + _n*4141, 10007)/10007\n"
    "    generate y = 1 + 2*x1 - x2 + 0.3*g + id/10 + mod(_n*_n*2277 + _n*4343, 10007)/10007\n"
    "    generate cnt = floor(exp(0.2 + 0.8*x1 + 0.05*g + mod(_n*_n*3821 + _n*4747, "
    "10007)/10007))\n"
    "    generate wt = 1 + mod(_n,4)\n"
    "    w2d_rclear\n"
    "end\n";

// Every `display` text one executeDoSource run produced.
std::vector<std::string> displays(piet::SessionState& session, const std::string& src) {
  std::vector<std::string> out;
  try {
    auto res = piet::doexec::executeDoSource(src, session, piet::dispatch);
    for (const auto& b : res.blocks) {
      if (b.is_object() && b.value("type", std::string()) == "display")
        out.push_back(b.value("text", std::string()));
    }
  } catch (const std::exception& e) {
    out.push_back(std::string("EXC:") + e.what());
  }
  return out;
}

// The display line that starts with `prefix` ("" when none was printed).
std::string lineStarting(const std::vector<std::string>& lines, const std::string& prefix) {
  for (const auto& l : lines) {
    if (l.compare(0, prefix.size(), prefix) == 0) return l;
  }
  return std::string();
}

// rc of one dispatched line (the last error block's coarse rc; "0" if none),
// and whether every block it produced is a single JSON object (defect class
// ARRAY-vs-BLOCK: an array pushed as one block aborts the selftest binary).
std::string rcOf(piet::SessionState& session, const std::string& cmd, bool* allObjects = nullptr) {
  std::string rc = "0";
  if (allObjects != nullptr) *allObjects = true;
  try {
    for (const auto& b : piet::dispatch(cmd, session)) {
      if (!b.is_object()) {
        if (allObjects != nullptr) *allObjects = false;
        continue;
      }
      if (b.value("type", std::string()) == "error") rc = piet::eval::coarseRc(b);
    }
  } catch (const std::exception& e) {
    rc = std::string("EXC:") + e.what();
  }
  return rc;
}

// Text of `display EXP` (empty when the line errored or displayed nothing).
std::string shown(piet::SessionState& session, const std::string& exp) {
  std::string text;
  try {
    for (const auto& b : piet::dispatch("display " + exp, session)) {
      if (b.is_object() && b.value("type", std::string()) == "display")
        text += b.value("text", std::string());
    }
  } catch (const std::exception&) {
  }
  return text;
}

// A fitted model in e() to clear or replace: the previous arm's model in the
// golden run was a poisson; any fitted model makes the same point.
const char* kPriorModel = "w2d_mkpanel\nquietly regress y x1\n";

}  // namespace

void testW2dEreturn() {
  std::printf("[selftest] --- W2D ereturn: clear / post / local (w2d_uncovered.log) ---\n");
  namespace fs = std::filesystem;
  const std::string workdir =
      (fs::temp_directory_path() / "piet_native_selftest_w2dereturn").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  // Enumerate actual explicit postings in publication order. A new posting
  // replaces the namespace; clear must not leave stale reflected names.
  {
    piet::SessionState s(workdir);
    const auto lines = displays(s,
        "clear\nset obs 4\ngenerate double x = _n\n"
        "quietly estpost summarize x\n"
        "local sm : e(scalars)\nlocal mm : e(matrices)\n"
        "display \"EN1|`sm'|`mm'|\"\n"
        "display \"EN2|`: e(scalars)'|\" e(k) \"|\"\n"
        );
    erCheck(lineStarting(lines, "EN1|") == "EN1|N k|count sum_w mean Var sd min max sum|",
            "e() reflection lists real estpost results in fixed captured order "
            "(w2v_estpost_ematrix.log:129/145)");
    erCheck(lineStarting(lines, "EN2|") == "EN2|N k|1|",
            "inline e() reflection and genuine estpost k / variance values");
    const auto variance = s.eval.resolveEMatrix("Var");
    erCheck(variance && variance->data.size() == 1 && variance->data[0].size() == 1 &&
                std::fabs(variance->data[0][0] - 5.0/3.0) < 1e-14,
            "estpost publishes the actual sample variance");
    const auto cleared = displays(s, "ereturn clear\nlocal empty : e(matrices)\n"
                                     "display \"EN3|`empty'|\"\n");
    erCheck(lineStarting(cleared, "EN3|") == "EN3||",
            "ereturn clear removes names from reflection");
  }

  // A posted eclass store has no LastModel. Its sample must survive that
  // store/restore path just as the ordinary model's sample does.
  {
    piet::SessionState s(workdir);
    const auto lines = displays(
        s, "clear\nset obs 4\ngenerate double x = _n\n"
           "generate byte marker = _n <= 2\n"
           "program define posted_sample_store, eclass\n"
           "matrix bpost = (1)\nmatrix colnames bpost = x\n"
           "matrix vpost = (1)\nmatrix colnames vpost = x\n"
           "matrix rownames vpost = x\n"
           "ereturn post bpost vpost, esample(marker) obs(2)\n"
           "ereturn local cmd posted_sample_store\nend\n"
           "posted_sample_store\ncount if e(sample)\n"
           "display \"POSTED_BEFORE|\" r(N) \"|\"\n"
           "estimates store posted_sample_A\nereturn clear\n"
           "estimates restore posted_sample_A\ncount if e(sample)\n"
           "display \"POSTED_AFTER|\" r(N) \"|\"\n");
    erCheck(lineStarting(lines, "POSTED_BEFORE|") == "POSTED_BEFORE|2|" &&
                lineStarting(lines, "POSTED_AFTER|") == "POSTED_AFTER|2|",
            "posted eclass estimates store/restore preserves its e(sample) generation");
  }

  // (1) regress_absorb_cluster_052, w2d_uncovered.log:2934. Main: the helper
  // stops at `ereturn clear` (rc 199), so the record never prints; with the
  // clear captured it printed the previous model's b/se/cmd.
  {
    piet::SessionState s(workdir);
    const auto lines = displays(
        s, std::string(kHelpers) + kPriorModel +
               "w2d_mkpanel\n"
               "capture noisily reg y x1, absorb(g) cluster(g)\n"
               "local __rc = _rc\n"
               "local b1 \"\"\n"
               "local s1 \"\"\n"
               "capture local b1 : display %18.0g _b[x1]\n"
               "capture local s1 : display %18.0g _se[x1]\n"
               "capture noisily display \"PREC|regress_absorb_cluster_052|\" `__rc' "
               "\"|`b1'|`s1'|`e(cmd)'|\"\n");
    const std::string got = lineStarting(lines, "PREC|regress_absorb_cluster_052|");
    erCheck(got == "PREC|regress_absorb_cluster_052|198||||",
            "W2D ereturn (1) regress_absorb_cluster_052 (log 2934): e() is empty after "
            "`ereturn clear` -- want `PREC|regress_absorb_cluster_052|198||||`, got `" + got + "`");
  }

  // (2) regress_absorb_vce_106, w2d_uncovered.log:2956.
  {
    piet::SessionState s(workdir);
    const auto lines = displays(
        s, std::string(kHelpers) + kPriorModel +
               "w2d_mkpanel\n"
               "capture noisily reg y x1 if c == 1, absorb(id) vce(cluster id)\n"
               "local __rc = _rc\n"
               "local b1 \"\"\n"
               "local s1 \"\"\n"
               "capture local b1 : display %18.0g _b[x1]\n"
               "capture local s1 : display %18.0g _se[x1]\n"
               "capture noisily display \"PREC|regress_absorb_vce_106|\" `__rc' "
               "\"|`b1'|`s1'|`e(cmd)'|\"\n");
    const std::string got = lineStarting(lines, "PREC|regress_absorb_vce_106|");
    erCheck(got == "PREC|regress_absorb_vce_106|198||||",
            "W2D ereturn (2) regress_absorb_vce_106 (log 2956): want "
            "`PREC|regress_absorb_vce_106|198||||`, got `" + got + "`");
  }

  // (3) ereturn_post_noesample_ctl, w2d_uncovered.log:4637 `0|.|0|y`: the
  // program verbatim (do 1482-1487). Fields rc, e(N) and e(depvar) are
  // asserted; the e(sample) count (`0`) is owed to the mask chain. Main: the
  // post line is unsupported_command, rc 199.
  {
    piet::SessionState s(workdir);
    const auto lines = displays(
        s, std::string(kHelpers) +
               "capture program drop w2d_post0\n"
               "program define w2d_post0, eclass\n"
               "    quietly regress y x1 x2\n"
               "    matrix b = e(b)\n"
               "    ereturn post b, depname(y)\n"
               "end\n"
               "w2d_mkpanel\n"
               "replace y = . in 1/5\n"
               "capture noisily w2d_post0\n"
               "local __rc = _rc\n"
               "capture noisily display \"REC|\" `__rc' \"|\" e(N) \"|`e(depvar)'|\"\n");
    const std::string got = lineStarting(lines, "REC|");
    erCheck(got == "REC|0|.|y|",
            "W2D ereturn (3) ereturn_post_noesample_ctl (log 4637 fields rc|e(N)|e(depvar) = "
            "0|.|y): `ereturn post b, depname(y)` replaces e() -- e(N) is missing although the "
            "program's regress had N 55 -- got `" + got + "`");
  }

  // (4) ereturn_post_esample_068's non-e(sample) fields, w2d_uncovered.log:4597
  // `0|55|55|111|y|b`: e(N) = obs(), e(depvar) = depname(), and `ereturn local
  // properties "b"` read back. The golden program also runs `generate e =
  // e(sample)`, which the macro layer refuses until the mask chain lands, so
  // that line is left out here and the esample() option is pinned in (4b).
  {
    piet::SessionState s(workdir);
    const auto lines = displays(
        s, std::string(kHelpers) +
               "capture program drop w2d_post068x\n"
               "program define w2d_post068x, eclass\n"
               "    quietly regress y x1 x2\n"
               "    matrix b = e(b)\n"
               "    quietly count if !missing(y)\n"
               "    ereturn post b, depname(y) obs(`r(N)')\n"
               "    ereturn local properties \"b\"\n"
               "end\n"
               "w2d_mkpanel\n"
               "replace y = . in 1/5\n"
               "capture noisily w2d_post068x\n"
               "local __rc = _rc\n"
               "capture noisily display \"REC|\" `__rc' \"|\" e(N) \"|`e(depvar)'|`e(properties)'|\"\n");
    const std::string got = lineStarting(lines, "REC|");
    erCheck(got == "REC|0|55|y|b|",
            "W2D ereturn (4) ereturn_post_esample_068 (log 4597 fields rc|e(N)|e(depvar)|"
            "e(properties) = 0|55|y|b) without the esample() part -- got `" + got + "`");
  }

  // (4b) esample(VAR) moves VAR into e(sample) (w2d_uncovered.log:4597
  // ereturn_post_esample_068 `0|55|55|111|`): rc 0, e(N) = obs(), VAR leaves the
  // data (confirm variable r(111)) and `count if e(sample)` reads VAR's nonzero
  // rows. A later fit ends the posting and its mask (e(sample) is the fit's).
  {
    piet::SessionState s(workdir);
    (void)displays(s, std::string(kHelpers) + kPriorModel +
                          "matrix b = e(b)\n"
                          "generate e = (_n > 5)\n"
                          "quietly count if e\n"
                          "local want = r(N)\n");
    bool objs = false;
    const std::string rc = rcOf(s, "ereturn post b, depname(y) esample(e) obs(55)", &objs);
    erCheck(rc == "0", "W2D ereturn (4b) `ereturn post ..., esample(e)` runs (oracle rc 0) -- got rc " + rc);
    erCheck(shown(s, "e(N)") == "55" && rcOf(s, "confirm variable e") == "111",
            "W2D ereturn (4b) e(N) is obs() and `e` left the data (log :4597 `55|..|111`) -- e(N) " +
                shown(s, "e(N)"));
    const std::vector<std::string> ls =
        displays(s, "quietly count if e(sample)\ndisplay \"NS|\" r(N) \"|\" `want' \"|\"\n");
    const std::string ns = lineStarting(ls, "NS|");
    const size_t b1 = ns.find('|', 3);
    erCheck(!ns.empty() && b1 != std::string::npos &&
                ns.substr(3, b1 - 3) == ns.substr(b1 + 1, ns.size() - b1 - 2) && ns != "NS|0|0|",
            "W2D ereturn (4b) `count if e(sample)` reads the posted mask (the rows where e != 0) "
            "-- got `" + ns + "`");
    (void)displays(s, "quietly regress y x1\n");
    const std::vector<std::string> ls2 =
        displays(s, "quietly count if e(sample)\ndisplay \"NF|\" r(N) \"|\" e(N) \"|\"\n");
    const std::string nf = lineStarting(ls2, "NF|");
    const size_t c1 = nf.find('|', 3);
    erCheck(!nf.empty() && c1 != std::string::npos &&
                nf.substr(3, c1 - 3) == nf.substr(c1 + 1, nf.size() - c1 - 2),
            "W2D ereturn (4b) after a new fit e(sample) is the fit's (count = e(N)) -- got `" + nf +
                "`");
  }

  // (5) ereturn_post_esample_tempvar_012's non-e(sample) fields,
  // w2d_uncovered.log:4617 `0|.|55|bootstrap|2`: a tempname matrix, renamed,
  // posted without obs(); `ereturn local cmd = "bootstrap"`. The golden builds
  // the matrix as `(_b[x1], _b[_cons])`, a literal the `matrix` handler refuses
  // today ("malformed literal", a follow-up outside this task); `= e(b)` gives
  // the same 1 x 2 values here.
  {
    piet::SessionState s(workdir);
    const auto lines = displays(
        s, std::string(kHelpers) +
               "capture program drop w2d_post012x\n"
               "program define w2d_post012x, eclass\n"
               "    quietly regress y x1\n"
               "    tempname bb\n"
               "    matrix `bb' = e(b)\n"
               "    matrix colnames `bb' = b1 b0\n"
               "    ereturn post `bb'\n"
               "    ereturn local cmd = \"bootstrap\"\n"
               "end\n"
               "w2d_mkpanel\n"
               "replace y = . in 1/5\n"
               "capture noisily w2d_post012x\n"
               "local __rc = _rc\n"
               "capture noisily display \"REC|\" `__rc' \"|\" e(N) \"|`e(cmd)'|\" colsof(e(b)) \"|\"\n"
               "scalar w2dsb = _b[b1]\n"
               "matrix w2dE = e(b)\n"
               "scalar w2dc = w2dE[1,1]\n"
               "capture noisily display \"BREC|\" missing(w2dsb) \"|\" (w2dsb == w2dc) \"|\"\n");
    const std::string got = lineStarting(lines, "REC|");
    erCheck(got == "REC|0|.|bootstrap|2|",
            "W2D ereturn (5) ereturn_post_esample_tempvar_012 (log 4617 fields rc|e(N)|e(cmd)|"
            "colsof(e(b)) = 0|.|bootstrap|2) -- got `" + got + "`");
    // _b[] after a post: the macro layer serves the POSTED coefficient (it
    // would be a silent `.` at rc 0 otherwise). No oracle arm prints it; the
    // check is against e(b) itself.
    const std::string bgot = lineStarting(lines, "BREC|");
    erCheck(bgot == "BREC|0|1|",
            "W2D ereturn (5) `scalar s = _b[b1]` after `ereturn post` reads the posted e(b)[1,1] "
            "-- got `" + bgot + "`");
  }

  // (6) w2_esample.log:1487 (G5): a TOP-LEVEL `ereturn post` is rc 0 and e(N)
  // is missing afterwards.
  {
    piet::SessionState s(workdir);
    (void)displays(s, std::string(kHelpers) + kPriorModel +
                          "matrix bG5 = (1, 2)\n"
                          "matrix colnames bG5 = x1 _cons\n");
    bool objs = false;
    const std::string rc = rcOf(s, "ereturn post bG5", &objs);
    const std::string n = shown(s, "e(N)");
    erCheck(rc == "0" && objs && n == ".",
            "W2D ereturn (6) G5 (w2_esample.log 1487 `0|...|.`): top-level `ereturn post` rc 0, "
            "e(N) . -- got rc " + rc + ", e(N) " + n);
  }

  // (7) w2_esample.log:1634 (G11): top-level `ereturn clear` after a fit is rc 0
  // and BOTH reader layers see an empty e(): the display twin (`display
  // e(N)`, `display _b[x1]`) and the macro/arith layer (`local n = e(N)`,
  // `scalar s = _b[x1]`, `e(cmd)'). The coefficient readers answer exactly
  // what they answer after `clear results` -- the existing no-estimates path,
  // which is r(111) "_b not allowed when e(b) is not present" in both layers
  // (w2v_estpost_ematrix.log ep15f_local_bx1 / ep15f_disp_bx1 after `ereturn
  // clear`; eb-order T-P3 retired the macro layer's `.` here).
  {
    piet::SessionState s(workdir);
    (void)displays(s, std::string(kHelpers) + kPriorModel);
    bool objs = false;
    const std::string rc = rcOf(s, "ereturn clear", &objs);
    const std::string dispN = shown(s, "e(N)");
    const auto mac = displays(s,
                              "local n = e(N)\n"
                              "local c \"`e(cmd)'\"\n"
                              "capture scalar w2ds = _b[x1]\n"
                              "display \"MAC|`n'|`c'|\" _rc \"|\"\n");
    const std::string macGot = lineStarting(mac, "MAC|");
    const std::string dispB = rcOf(s, "display _b[x1]");
    erCheck(rc == "0" && objs,
            "W2D ereturn (7) G11 (w2_esample.log 1634): top-level `ereturn clear` after regress "
            "is rc 0 -- got " + rc);
    erCheck(dispN == ".", "W2D ereturn (7) display twin: `display e(N)` is . after "
                          "`ereturn clear` -- got `" + dispN + "`");
    erCheck(macGot == "MAC|.||111|",
            "W2D ereturn (7) macro layer: `local n = e(N)` is ., e(cmd) is empty and `scalar s = "
            "_b[x1]` is r(111) after `ereturn clear` (w2v_estpost_ematrix.log ep15f_local_bx1) "
            "-- got `" + macGot + "`");
    // The display twin's _b[] takes its no-estimates path, the same answer as
    // after `clear results` (pinned side by side so the two cannot drift).
    piet::SessionState r(workdir);
    (void)displays(r, std::string(kHelpers) + kPriorModel + "clear results\n");
    const std::string dispBRef = rcOf(r, "display _b[x1]");
    erCheck(dispB != "0" && dispB == dispBRef,
            "W2D ereturn (7) display twin: `display _b[x1]` after `ereturn clear` takes the same "
            "no-estimates path as after `clear results` -- rc " + dispB + " vs " + dispBRef);
  }

  // (8) No data in memory: `ereturn clear` is rc 0 (w2d_uncovered.log:211-212,
  // `w2d_mkts` runs its `ereturn clear` after `clear all` and goes on).
  // Main: no_data_loaded (r(111)).
  {
    piet::SessionState s(workdir);
    bool objs = false;
    const std::string rc = rcOf(s, "ereturn clear", &objs);
    erCheck(rc == "0" && objs,
            "W2D ereturn (8) `ereturn clear` with no data in memory is rc 0 (log 211-212) -- got " +
                rc);
    // `ereturn list` keeps the answer it had while the command sat behind the
    // no-data guard.
    const std::string rcList = rcOf(s, "ereturn list");
    erCheck(rcList == "111",
            "W2D ereturn (8) `ereturn list` with no data is still no_data_loaded -- got " + rcList);
  }

  // (9) A staged `ereturn scalar` of the running eclass program does not come
  // back after `ereturn clear` (runProgram flushes staged e-scalars at return).
  {
    piet::SessionState s(workdir);
    const auto lines = displays(s,
                                "capture program drop w2dstage\n"
                                "program define w2dstage, eclass\n"
                                "    ereturn scalar w2dk = 3\n"
                                "    ereturn clear\n"
                                "end\n"
                                "w2dstage\n"
                                "display \"STG|\" e(w2dk) \"|\"\n");
    const std::string got = lineStarting(lines, "STG|");
    erCheck(got == "STG|.|",
            "W2D ereturn (9) `ereturn clear` also drops an `ereturn scalar` staged earlier in the "
            "same eclass program -- got `" + got + "`");
  }

  // (10) Refusals: every one named, every one with a Stata rc (never 1), every
  // one a single block. Malformed posts have no oracle arm; their rc are the
  // explicitly chosen, UNPINNED codes of handlers_ereturn.cpp.
  {
    struct Refusal {
      const char* cmd;
      const char* rc;
    };
    static const Refusal kRefusals[] = {
        {"ereturn", "198"},
        // Top level: r(152) "non e-class program may not set e()" (StataBE 19.5,
        // BUG-1495, scratch-laneA2/agents/rprog/x1495.do TOP_REPOST_B, TOP_MATRIX).
        {"ereturn repost b = b", "152"},
        {"ereturn scalar", "199"},
        {"ereturn matrix m = b", "152"},
        {"ereturn display", "199"},
        {"ereturn hidden local x y", "199"},
        {"ereturn nosuchsub", "199"},
        {"ereturn clear, nosuch", "198"},
        {"ereturn post b, nosuch(1)", "198"},
        {"ereturn post b, buildfvinfo", "198"},
        {"ereturn post w2dnosuch", "111"},
        {"ereturn post b V2", "503"},
        {"ereturn post b V3", "507"},
        {"ereturn post b, obs(abc)", "198"},
        {"ereturn post b b b", "198"},
        // Top level: r(152), w2p_estim_ev.log:917 (C_ereturn_local).
        {"ereturn local x = 1+1", "152"},
        {"ereturn local bar \"x\"", "152"},
    };
    piet::SessionState s(workdir);
    (void)displays(s,
                   "matrix b = (1, 2)\n"
                   "matrix colnames b = x1 _cons\n"
                   "matrix V2 = (1, 0, 0 \\ 0, 1, 0 \\ 0, 0, 1)\n"
                   "matrix V3 = (1, 0 \\ 0, 1)\n");
    for (const auto& r : kRefusals) {
      bool objs = false;
      const std::string rc = rcOf(s, r.cmd, &objs);
      erCheck(rc == r.rc && objs, std::string("W2D ereturn (10) `") + r.cmd + "` is a named r(" +
                                      r.rc + ") in one block -- got rc " + rc);
    }
  }

  // (11) R2: inside an eclass program `ereturn local NAME` takes the raw rest
  // of the line as text, comma included (as `local` does; UNPINNED, no oracle
  // arm), and a numeric `= exp` stays a named refusal (199, unpinned). d1e599ea
  // refused the comma form r(198).
  {
    piet::SessionState s(workdir);
    const auto lines = displays(s,
                                "capture program drop w2dloc\n"
                                "program define w2dloc, eclass\n"
                                "    ereturn local note a, b\n"
                                "    capture ereturn local num = 1+1\n"
                                "    local r = _rc\n"
                                "    ereturn local numrc `r'\n"
                                "end\n"
                                "w2dloc\n"
                                "display \"LOC|`e(note)'|`e(num)'|`e(numrc)'|\"\n");
    const std::string got = lineStarting(lines, "LOC|");
    erCheck(got == "LOC|a, b||199|",
            "W2D ereturn (11) in an eclass program `ereturn local note a, b` stores `a, b`; "
            "`ereturn local num = 1+1` is a named refusal 199 -- got `" + got + "`");
  }

  // (12) w2p_estim_ev.log:917 C_ereturn_local `152||60|regress|`: at top level
  // `ereturn local` is r(152) and e() is unchanged. Main: rc 199
  // (unsupported_command); d1e599ea: rc 0 and e(bar) stored.
  {
    piet::SessionState s(workdir);
    const auto lines = displays(
        s, std::string(kHelpers) +
               "w2d_mkpanel\n"
               "capture ereturn clear\n"
               "quietly regress y x1 x2\n"
               "capture noisily ereturn local bar \"x\"\n"
               "local __rc = _rc\n"
               "capture noisily display \"PREC|C_ereturn_local|\" `__rc' \"|`e(bar)'|\" e(N) "
               "\"|`e(cmd)'|\"\n");
    const std::string got = lineStarting(lines, "PREC|C_ereturn_local|");
    erCheck(got == "PREC|C_ereturn_local|152||60|regress|",
            "W2D ereturn (12) C_ereturn_local (w2p_estim_ev.log:917): top-level `ereturn local` "
            "is r(152) and stores nothing -- got `" + got + "`");
  }

  // (13) BUG-733, `ereturn post` then a real fit: the posted e() is GONE. Both
  // reader layers: the display twin (`display e(N)`, `display e(df_r)`) and
  // the macro/arith layer (`local n = e(N)`, `scalar sn = e(N)`, `local d =
  // e(df_r)`), plus `matrix c = e(b)`. d1e599ea printed e(N) 999, e(df_r) 7 and a 1 x 2 e(b)
  // (review rev/stale.do, rev/stale2.do); main refuses the post, so the line never prints.
  // Oracle: Stata replaces all of e() on every estimation (w2d_uncovered.log
  // 4637: even the program's own regress e(N) is gone after a post).
  {
    piet::SessionState s(workdir);
    const auto lines = displays(
        s, std::string(kHelpers) +
               "w2d_mkpanel\n"
               "matrix p = (100, 200)\n"
               "matrix colnames p = a b\n"
               "capture program drop w2dpost\n"
               "program define w2dpost, eclass\n"
               "    ereturn post p, obs(999) dof(7) depname(zz)\n"
               "    ereturn local w2dtag \"posted\"\n"
               "end\n"
               "w2dpost\n"
               "display \"PRE|\" e(N) \"|\" e(df_r) \"|`e(depvar)'|`e(w2dtag)'|\"\n"
               "quietly regress y x1 x2\n"
               "local n = e(N)\n"
               "scalar w2dsn = e(N)\n"
               "local d = e(df_r)\n"
               "matrix w2dc = e(b)\n"
               "display \"AREG|\" e(N) \"|`n'|\" w2dsn \"|`d'|\" e(df_r) \"|\" colsof(w2dc) "
               "\"|`e(depvar)'|\"\n");
    const std::string pre = lineStarting(lines, "PRE|");
    erCheck(pre == "PRE|999|7|zz|posted|",
            "W2D ereturn (13) BUG-733 precondition: the posting is e() until the fit -- got `" +
                pre + "`");
    const std::string got = lineStarting(lines, "AREG|");
    erCheck(got == "AREG|60|60|60|57|57|3|y|",
            "W2D ereturn (13) BUG-733: after `ereturn post p, obs(999) dof(7)` a 60-obs regress "
            "owns e(): e(N) 60 and e(df_r) 57 in both layers, a 1 x 3 e(b), e(depvar) y -- got `" +
                got + "`");
    // `matrix c = e(b)` is the REGRESS coefficient vector, name by name (the
    // order is the engine's, a separate eb-order item, so it is not asserted).
    const piet::eval::EvalMatrix* c = nullptr;
    for (const auto& [k, m] : s.eval.matrices) {
      if (k == "w2dc") c = &m;
    }
    bool same = c != nullptr && c->data.size() == 1 && c->colnames.size() == 3 &&
                s.eval.lastModelResult.has_value() &&
                s.eval.lastModelResult->contains("coeftable");
    for (size_t j = 0; same && j < c->colnames.size(); ++j) {
      bool found = false;
      for (const auto& row : (*s.eval.lastModelResult)["coeftable"]) {
        const std::string nm = row.value("name", std::string());
        const bool match = nm == c->colnames[j] ||
                           (c->colnames[j] == "_cons" && nm == "(intercept)");
        if (match && row.contains("b") && row["b"].is_number()) {
          found = row["b"].get<double>() == c->data[0][j];
        }
      }
      same = found;
    }
    erCheck(same, "W2D ereturn (13) BUG-733: `matrix c = e(b)` after the regress equals the "
                  "regress coefficients name by name (not the posted a/b = 100/200)");
    const auto tag = displays(s, "display \"TAG|`e(w2dtag)'|\"\n");
    erCheck(lineStarting(tag, "TAG|") == "TAG||",
            "W2D ereturn (13) BUG-733: an `ereturn local` added to the posting goes with it -- "
            "got `" + lineStarting(tag, "TAG|") + "`");
  }

  // (14) BUG-733, the estpost half (BUG-467 class), measured by the w2d replay:
  // after an estpost on 60 rows, fits on a NEW 30-row dataset reported e(N) 60.
  // probe5/e.do verbatim (w2d-uncovered-replay-breakdown); the expected lines
  // are the fits' own values (30 rows, 5 clusters of g).
  {
    piet::SessionState s(workdir);
    const auto lines = displays(
        s,
        "clear\n"
        "set obs 60\n"
        "gen x1 = mod(_n,7)+ _n/13\n"
        "gen c = mod(_n,4)\n"
        "gen y = round(exp(0.1*x1)*3)\n"
        "gen g = mod(_n,10)\n"
        "estpost tabstat x1, by(c) statistics(mean sd) columns(statistics)\n"
        "clear\n"
        "set obs 30\n"
        "gen x1 = mod(_n,7)+ _n/10\n"
        "gen y = round(exp(0.1*x1)*3) + mod(_n,3)\n"
        "gen g = mod(_n,5)\n"
        "quietly poisson y x1\n"
        "display \"P|\" e(N) \"|\" e(cmd) \"|\"\n"
        "quietly regress y x1, vce(cluster g)\n"
        "display \"R|\" e(N) \"|\" e(df_r) \"|\" e(N_clust) \"|\"\n"
        "quietly xtset g\n"
        "quietly xtreg y x1, fe\n"
        "display \"X|\" e(N) \"|\" e(N_g) \"|\"\n");
    const std::string p = lineStarting(lines, "P|");
    const std::string r = lineStarting(lines, "R|");
    const std::string x = lineStarting(lines, "X|");
    erCheck(p == "P|30|poisson|" && r == "R|30|4|5|" && x == "X|30|5|",
            "W2D ereturn (14) BUG-733: after an estpost on 60 rows, poisson / regress, "
            "vce(cluster) / xtreg, fe on 30 rows own e(N) -- got `" + p + "` `" + r + "` `" + x +
                "`");
  }

  // (15) probe2/s.do: `estpost tabstat` then `regress y x1 if c < 3` -> e(N) 40,
  // e(df_r) 38 (the fit's own; main printed 60).
  {
    piet::SessionState s(workdir);
    const auto lines = displays(
        s,
        "clear\n"
        "set obs 60\n"
        "generate c = mod(_n,3) + 1\n"
        "generate x1 = mod(_n*_n*142 + _n*2929, 10007)/10007\n"
        "generate y = 1 + 2*x1 + mod(_n*_n*2277 + _n*4343, 10007)/10007\n"
        "quietly estpost tabstat x1, by(c) statistics(mean) columns(statistics) nototal "
        "listwise\n"
        "display \"P|\" e(N) \"|\"\n"
        "quietly regress y x1 if c < 3\n"
        "display \"A|\" e(N) \"|\" e(cmd) \"|\" e(df_r) \"|\"\n");
    const std::string a = lineStarting(lines, "A|");
    erCheck(a == "A|40|regress|38|",
            "W2D ereturn (15) BUG-733 probe2/s.do: estpost tabstat, then regress if c < 3 -> "
            "e(N) 40, e(df_r) 38 -- got `" + a + "`");
  }

  // (16) BUG-733 against the oracle: the e(N) field of five w2d_uncovered.log
  // records whose fit runs `if c < 3` on the 60-row panel is 40. The replay
  // printed 60 for all five (an earlier estpost on 60 rows was still in e()),
  // so the estpost is put in front of each fit here. Stanzas verbatim from
  // w2d_uncovered.do; b/se (a float-default item) are NOT asserted.
  {
    struct EnArm {
      const char* name;
      const char* line;
      const char* fit;
    };
    static const EnArm kArms[] = {
        {"ivregress_r_exog_before_098", "2501",
         "capture noisily ivregress 2sls y w1 i.t (x1 = z1) if c < 3, r\n"},
        {"ivregress_exog_before_ctl", "2536",
         "capture noisily ivregress 2sls y w1 i.t (x1 = z1) if c < 3\n"},
        {"xtreg_fe_cluster_049", "3234",
         "quietly xtset id t\ncapture noisily xtreg y i.g x1 if c < 3, fe cluster(id)\n"},
        {"xtreg_robust_fe_cluster_019", "3274",
         "quietly xtset id t\ncapture noisily xtreg y x1 x2 if c < 3 , robust fe cluster(id)\n"},
        {"xtreg_fe_vce_cluster_ctl", "3314",
         "quietly xtset id t\ncapture noisily xtreg y x1 x2 if c < 3, fe vce(cluster id)\n"},
    };
    for (const auto& a : kArms) {
      piet::SessionState s(workdir);
      const auto lines = displays(s, std::string(kHelpers) + "w2d_mkpanel\n" +
                                         "quietly estpost summarize y x1 x2\n" + a.fit +
                                         "local __rc = _rc\n"
                                         "display \"EN|\" `__rc' \"|\" e(N) \"|\"\n");
      const std::string got = lineStarting(lines, "EN|");
      erCheck(got == "EN|0|40|", std::string("W2D ereturn (16) BUG-733 ") + a.name +
                                     " (w2d_uncovered.log:" + a.line +
                                     ") rc|e(N) = 0|40 after an estpost on 60 rows -- got `" +
                                     got + "`");
    }
  }

  // (17) BUG-733 guard for the fits that post e() BEFORE setLastModel (pca /
  // oaxaca / teffects, dispatcher.cpp runFitWritingEFirst). After `ereturn
  // post p, obs(999) dof(7)`, pca owns e(): its own e(N), e(f) and e(cmd)
  // survive, and the posted e(df_r) is gone (d1e599ea: 7). A refused fit leaves
  // the posting in place, as in Stata.
  {
    piet::SessionState s(workdir);
    const auto lines = displays(
        s, std::string(kHelpers) +
               "w2d_mkpanel\n"
               "matrix p = (100, 200)\n"
               "matrix colnames p = a b\n"
               "ereturn post p, obs(999) dof(7)\n"
               "capture pca w2dnosuch\n"
               "display \"KEEP|\" (_rc != 0) \"|\" e(N) \"|\" e(df_r) \"|\"\n"
               "capture regress y w2dnosuch\n"
               "display \"KEEP2|\" (_rc != 0) \"|\" e(N) \"|\"\n"
               "quietly pca x1 x2 x3\n"
               "display \"PCA|\" e(N) \"|\" e(df_r) \"|\" (e(f) >= 1 & !missing(e(f))) \"|`e(cmd)'|\"\n");
    const std::string keep = lineStarting(lines, "KEEP|");
    const std::string keep2 = lineStarting(lines, "KEEP2|");
    const std::string pca = lineStarting(lines, "PCA|");
    erCheck(keep == "KEEP|1|999|7|" && keep2 == "KEEP2|1|999|",
            "W2D ereturn (17) BUG-733: a refused fit leaves the posted e() in place -- got `" +
                keep + "` `" + keep2 + "`");
    erCheck(pca == "PCA|60|.|1|pca|",
            "W2D ereturn (17) BUG-733: pca after a posting owns e() -- its own e(N)/e(f)/e(cmd), "
            "no posted e(df_r) -- got `" + pca + "`");
    // Same data, estpost first: pca's e(N) EQUALS the posted 60, the case a
    // value comparison could not tell apart. It must still read 60.
    piet::SessionState s2(workdir);
    const auto l2 = displays(s2, std::string(kHelpers) +
                                     "w2d_mkpanel\n"
                                     "quietly estpost summarize x1 x2 x3\n"
                                     "quietly pca x1 x2 x3\n"
                                     "display \"PCA2|\" e(N) \"|\" (!missing(e(rho))) \"|\"\n");
    const std::string pca2 = lineStarting(l2, "PCA2|");
    erCheck(pca2 == "PCA2|60|1|",
            "W2D ereturn (17) BUG-733: estpost then pca on the same 60 rows keeps pca's e(N) 60 "
            "and e(rho) -- got `" + pca2 + "`");
  }

  // (18) Control (item (d)): estpost itself is unchanged, and so is esttab after
  // it. The live table right after `estpost summarize` and the STORED table
  // (`eststo S1`) rendered after a later regress -- which now supersedes the
  // posting -- carry the same cells (mean 15.5 of 1..30). Existing pins of the
  // live path: selftest.cpp AER-TABLES (v), freeze estpost.json, wave P4.
  {
    auto tableCsv = [&](piet::SessionState& s, const std::string& src) {
      std::string csv;
      try {
        auto res = piet::doexec::executeDoSource(src, s, piet::dispatch);
        for (const auto& b : res.blocks) {
          if (b.is_object() && b.value("type", std::string()) == "estimates_table")
            csv = b.value("csv", std::string());
        }
      } catch (const std::exception&) {
      }
      return csv;
    };
    const std::string data =
        "clear\nset obs 30\ngenerate x1 = _n\ngenerate y = 2*_n + mod(_n,7)\n";
    piet::SessionState a(workdir);
    const std::string live = tableCsv(a, data + "estpost summarize x1 y\nesttab\n");
    piet::SessionState b(workdir);
    const std::string stored = tableCsv(
        b, data + "estpost summarize x1 y\neststo S1\nquietly regress y x1\nesttab S1\n");
    // The column head names the model (`est1` live, `S1` stored); the body --
    // cells, N 30 -- must be identical.
    const auto body = [](const std::string& csv) {
      const size_t nl = csv.find('\n');
      return nl == std::string::npos ? std::string() : csv.substr(nl + 1);
    };
    erCheck(!live.empty() && live.find("15.5") != std::string::npos &&
                body(live).find("N,30") != std::string::npos && body(stored) == body(live),
            "W2D ereturn (18) control: esttab of an estpost is unchanged, live and stored after a "
            "later fit -- live `" + live.substr(0, 120) + "` stored `" + stored.substr(0, 120) +
                "`");
  }

  // Round 3 (REDIAGNOSIS.md S4). The eclass programs the arms share.
  const std::string kOnFit =
      "capture program drop w2dfit\n"
      "program define w2dfit, eclass\n"
      "    regress y x1\n"
      "    ereturn local fe \"Yes\"\n"
      "end\n"
      "capture program drop w2dcmd\n"
      "program define w2dcmd, eclass\n"
      "    regress y x1\n"
      "    ereturn local cmd \"w2dcmd\"\n"
      "end\n"
      "capture program drop w2dtitle\n"
      "program define w2dtitle, eclass\n"
      "    regress y x1\n"
      "    ereturn local title \"mine\"\n"
      "end\n"
      "capture program drop w2dnote\n"
      "program define w2dnote, eclass\n"
      "    ereturn local note \"hi\"\n"
      "end\n";
  const std::string kPanel = std::string(kHelpers) + kOnFit + "w2d_mkpanel\n";

  // (19) Table row E4, the round-2 review's MAJOR (rev2/a.do): `ereturn local`
  // on top of a REAL fit is part of that fit's e() (help program,
  // "or modifies already existing results in e()") and is gone after the next
  // fit (help return). b3dcdae9: `NEXT|Yes|40|regress|`.
  {
    piet::SessionState s(workdir);
    const auto lines = displays(s, kPanel +
                                       "quietly w2dfit\n"
                                       "display \"ONFIT|`e(fe)'|\" e(N) \"|`e(cmd)'|\"\n"
                                       "quietly regress y x2 if c < 3\n"
                                       "display \"NEXT|`e(fe)'|\" e(N) \"|`e(cmd)'|\"\n");
    const std::string onfit = lineStarting(lines, "ONFIT|");
    const std::string next = lineStarting(lines, "NEXT|");
    erCheck(onfit == "ONFIT|Yes|60|regress|" && next == "NEXT||40|regress|" &&
                !s.eval.eStrings.contains("fe"),
            "W2D ereturn (19) E4: an `ereturn local` on top of a fit ends at the next fit -- want "
            "`ONFIT|Yes|60|regress|` `NEXT||40|regress|`, got `" + onfit + "` `" + next + "`");
  }

  // (20) Row E5: on top of an EMPTY e() (the shape binsreg's ado leaves) the key
  // is gone after the next fit as well. b3dcdae9: `EMPTY2|hi|60|`.
  {
    piet::SessionState s(workdir);
    const auto lines = displays(s, kPanel +
                                       "ereturn clear\n"
                                       "w2dnote\n"
                                       "display \"EMPTY|`e(note)'|\" e(N) \"|\"\n"
                                       "quietly regress y x1\n"
                                       "display \"EMPTY2|`e(note)'|\" e(N) \"|\"\n");
    const std::string a = lineStarting(lines, "EMPTY|");
    const std::string b = lineStarting(lines, "EMPTY2|");
    erCheck(a == "EMPTY|hi|.|" && b == "EMPTY2||60|",
            "W2D ereturn (20) E5: an `ereturn local` on an empty e() ends at the next fit -- want "
            "`EMPTY|hi|.|` `EMPTY2||60|`, got `" + a + "` `" + b + "`");
  }

  // (21) Rows E23/E24, the pre-install writers (dispatcher.cpp
  // runFitWritingEFirst). A refused pca keeps the on-top key (PKEEP, control);
  // a pca fit drops it (PFIT; b3dcdae9 `PFIT|Yes|Principal components|pca|`);
  // and the drop happens BEFORE pca writes, so an on-top e(title) does not
  // erase pca's own (PTITLE, control).
  {
    piet::SessionState s(workdir);
    const auto lines = displays(s, kPanel +
                                       "quietly w2dfit\n"
                                       "capture pca w2dnosuch\n"
                                       "display \"PKEEP|\" (_rc != 0) \"|`e(fe)'|\" e(N) \"|\"\n"
                                       "quietly pca x1 x2 x3\n"
                                       "display \"PFIT|`e(fe)'|`e(title)'|`e(cmd)'|\"\n"
                                       "quietly w2dtitle\n"
                                       "quietly pca x1 x2 x3\n"
                                       "display \"PTITLE|`e(title)'|`e(cmd)'|\"\n");
    const std::string keep = lineStarting(lines, "PKEEP|");
    const std::string fit = lineStarting(lines, "PFIT|");
    const std::string title = lineStarting(lines, "PTITLE|");
    erCheck(keep == "PKEEP|1|Yes|60|",
            "W2D ereturn (21) E24 control: a refused pca leaves the on-top key -- got `" + keep +
                "`");
    erCheck(fit == "PFIT||Principal components|pca|",
            "W2D ereturn (21) E23: pca drops the key written on top of the previous fit -- want "
            "`PFIT||Principal components|pca|`, got `" + fit + "`");
    erCheck(title == "PTITLE|Principal components|pca|",
            "W2D ereturn (21) E23 control: an on-top e(title) does not erase pca's own -- got `" +
                title + "`");
  }

  // (22) Control, rows E26/E20: test, lincom, nlcom and erepost of the same
  // model leave the on-top key alone (they store r(), or change b only). RE is
  // the deterministic guard: erepost always re-enters setLastModel, test/lincom
  // only when the engine mints a handle.
  {
    piet::SessionState s(workdir);
    const auto lines = displays(s, kPanel +
                                       "quietly w2dfit\n"
                                       "capture quietly test x1\n"
                                       "display \"RT|\" _rc \"|`e(fe)'|\" e(N) \"|\"\n"
                                       "capture quietly lincom x1\n"
                                       "display \"RL|\" _rc \"|`e(fe)'|\" e(N) \"|\"\n"
                                       "capture quietly nlcom _b[x1]\n"
                                       "display \"RN|\" _rc \"|`e(fe)'|\" e(N) \"|\"\n"
                                       "matrix w2drb = (0.5, 0.25)\n"
                                       // e(b)'s names: unnamed is r(507) (BUG-1494).
                                       "matrix colnames w2drb = x1 _cons\n"
                                       "capture quietly erepost b = w2drb\n"
                                       "display \"RE|\" _rc \"|`e(fe)'|\"\n");
    const std::string rt = lineStarting(lines, "RT|");
    const std::string rl = lineStarting(lines, "RL|");
    const std::string rn = lineStarting(lines, "RN|");
    const std::string re = lineStarting(lines, "RE|");
    erCheck(rt == "RT|0|Yes|60|" && rl == "RL|0|Yes|60|" && rn == "RN|0|Yes|60|",
            "W2D ereturn (22) E26 control: test/lincom/nlcom keep the on-top key -- got `" + rt +
                "` `" + rl + "` `" + rn + "`");
    erCheck(re == "RE|0|Yes|",
            "W2D ereturn (22) E20: erepost keeps the on-top key -- got `" + re + "`");
  }

  // (23) Rows E26(b)/E20: an on-top e(cmd) survives test and erepost of the
  // same model (b3dcdae9: the re-entry republished `regress`, CMD2 always and
  // CMD1 when a handle is minted).
  {
    piet::SessionState s(workdir);
    const auto lines = displays(s, kPanel +
                                       "quietly w2dcmd\n"
                                       "display \"CMD0|`e(cmd)'|\"\n"
                                       "capture quietly test x1\n"
                                       "display \"CMD1|\" _rc \"|`e(cmd)'|\"\n"
                                       "matrix w2drb = (0.5, 0.25)\n"
                                       // e(b)'s names: unnamed is r(507) (BUG-1494).
                                       "matrix colnames w2drb = x1 _cons\n"
                                       "capture quietly erepost b = w2drb\n"
                                       "display \"CMD2|\" _rc \"|`e(cmd)'|\"\n");
    const std::string c0 = lineStarting(lines, "CMD0|");
    const std::string c1 = lineStarting(lines, "CMD1|");
    const std::string c2 = lineStarting(lines, "CMD2|");
    erCheck(c0 == "CMD0|w2dcmd|" && c1 == "CMD1|0|w2dcmd|",
            "W2D ereturn (23) E26(b): an on-top e(cmd) survives test -- got `" + c0 + "` `" + c1 +
                "`");
    erCheck(c2 == "CMD2|0|w2dcmd|",
            "W2D ereturn (23) E20: an on-top e(cmd) survives erepost -- want `CMD2|0|w2dcmd|`, "
            "got `" + c2 + "`");
  }

  // (24) Row E26(a), the round-2 review's MINOR: test and lincom after an
  // estpost leave the posting alone (frozen main 60; b3dcdae9 `EP1|0|40|`, the
  // stale 40-row fit's e(N) after setEstHandleM4 re-entered). eb-order T-P3:
  // estpost posts no e(b), so both are Stata's r(321) "requested action not
  // valid after most recent estimation command" (w2v_estpost_ematrix.log
  // ep14_test, ep14_lincom), and the posting's e(N) stays. Plus the control:
  // esttab of an estpost is byte-identical with a `test` in between.
  {
    piet::SessionState s(workdir);
    const auto lines = displays(s, kPanel +
                                       "quietly regress y x1 if c < 3\n"
                                       "quietly estpost summarize y x1 x2\n"
                                       "display \"EP0|\" e(N) \"|\"\n"
                                       "capture quietly test x1\n"
                                       "display \"EP1|\" _rc \"|\" e(N) \"|\"\n"
                                       "capture quietly lincom x1\n"
                                       "display \"EP2|\" _rc \"|\" e(N) \"|\"\n");
    const std::string e0 = lineStarting(lines, "EP0|");
    const std::string e1 = lineStarting(lines, "EP1|");
    const std::string e2 = lineStarting(lines, "EP2|");
    erCheck(e0 == "EP0|60|" && e1 == "EP1|321|60|" && e2 == "EP2|321|60|",
            "W2D ereturn (24) E26(a): test/lincom after an estpost are r(321) and keep the "
            "posting -- want `EP0|60|` `EP1|321|60|` `EP2|321|60|`, got `" + e0 + "` `" + e1 +
                "` `" + e2 + "`");
    auto tableCsv = [&](piet::SessionState& t, const std::string& src) {
      std::string csv;
      try {
        auto res = piet::doexec::executeDoSource(src, t, piet::dispatch);
        for (const auto& b : res.blocks) {
          if (b.is_object() && b.value("type", std::string()) == "estimates_table")
            csv = b.value("csv", std::string());
        }
      } catch (const std::exception&) {
      }
      return csv;
    };
    const std::string data =
        "clear\nset obs 30\ngenerate x1 = _n\ngenerate y = 2*_n + mod(_n,7)\n"
        "quietly regress y x1\n";
    piet::SessionState a(workdir);
    const std::string plain = tableCsv(a, data + "estpost summarize x1 y\nesttab\n");
    piet::SessionState b(workdir);
    const std::string tested =
        tableCsv(b, data + "estpost summarize x1 y\ncapture quietly test x1\nesttab\n");
    erCheck(!plain.empty() && plain == tested,
            "W2D ereturn (24) control: esttab of an estpost is byte-identical with a `test` in "
            "between -- plain `" + plain.substr(0, 120) + "` tested `" + tested.substr(0, 120) +
                "`");
  }

  // (25) Rows E27/E28: `estimates store` carries the on-top key with its
  // lifetime; the next fit drops it (SR1), restore brings it back (SR2), and
  // the fit after the restore drops it again (SR3). b3dcdae9: `SR1|Yes|60|`,
  // `SR3|Yes|40|`.
  {
    piet::SessionState s(workdir);
    const auto lines = displays(s, kPanel +
                                       "quietly w2dfit\n"
                                       "estimates store w2dm1\n"
                                       "quietly regress y x2\n"
                                       "display \"SR1|`e(fe)'|\" e(N) \"|\"\n"
                                       "estimates restore w2dm1\n"
                                       "display \"SR2|`e(fe)'|\" e(N) \"|\"\n"
                                       "quietly regress y x2 if c < 3\n"
                                       "display \"SR3|`e(fe)'|\" e(N) \"|\"\n");
    const std::string r1 = lineStarting(lines, "SR1|");
    const std::string r2 = lineStarting(lines, "SR2|");
    const std::string r3 = lineStarting(lines, "SR3|");
    erCheck(r1 == "SR1||60|" && r2 == "SR2|Yes|60|" && r3 == "SR3||40|",
            "W2D ereturn (25) E27/E28: store/restore carry the on-top key's lifetime -- want "
            "`SR1||60|` `SR2|Yes|60|` `SR3||40|`, got `" + r1 + "` `" + r2 + "` `" + r3 + "`");
  }

  // FU-375 (the BUG-467 pre-install half). The b2_oracle data stanza of
  // w2g_ebmat.do:770-777 (aer_estim2_oracle.log:800-813).
  const std::string kOxData =
      "clear\n"
      "set obs 200\n"
      "gen double id = _n\n"
      "gen double x1 = mod(_n * 7, 23) / 23\n"
      "gen double x2 = mod(_n * 13, 17) / 17\n"
      "gen byte   g  = mod(_n, 2)\n"
      "gen byte   t  = mod(floor((_n - 1) / 3), 2)\n"
      "gen double y  = 1 + 2*x1 - 0.5*x2 + 0.3*g + 0.25*t + mod(_n * 11, 31)/31 - 0.5\n";

  // (26) w2g_ebmat.do:832 then :1147-1157 (log :2610-2622): oaxaca, then a
  // regress, then `matrix b = e(b)`. The regress's e(b) is a 1 x 3 vector
  // (`C|x1 x2 _cons|`), never the oaxaca 1 x 10 stripe that b3dcdae9 and
  // frozen main still served from the posted eMatrices, nor oaxaca's e(N) 200
  // for a 99-row fit. The column NAMES are compared with a fresh-session
  // regress, not the log text: the coeftable fallback still spells the
  // intercept `(intercept)` and puts it first (eb-order T-P1, outside this
  // task), and that must not make this arm fail when it lands.
  {
    const std::string tail =
        "quietly regress y x1 x2 if t == 1\n"
        "matrix b = e(b)\n"
        "local cn : colnames b\n"
        "display \"C|`cn'|\" colsof(b) \"|\" e(N) \"|`e(title)'|\"\n";
    piet::SessionState s(workdir);
    const auto lines =
        displays(s, kOxData + "quietly oaxaca y x1 x2, by(g) pooled relax\n" + tail);
    piet::SessionState f(workdir);
    const auto fresh = displays(f, kOxData + tail);
    const std::string got = lineStarting(lines, "C|");
    const std::string want = lineStarting(fresh, "C|");
    // e(title) is regress's own "Linear regression" (r150 WS-G posts regress's
    // e(title); StataBE 19.5 `ereturn list` after regress, and regress's
    // e(macros) list `cmdline title marginsprop ...`), never oaxaca's.
    erCheck(!want.empty() && want.find("|3|99|Linear regression|") != std::string::npos &&
                got == want,
            "W2D ereturn (26) FU-375: after oaxaca, regress's `matrix b = e(b)` is its own 1 x 3 "
            "and e(N) 99, e(title) regress's -- want `" + want + "`, got `" + got + "`");
  }

  // (27) Probe K (REDIAGNOSIS 7): pca's pre-install e(N) 60 / e(title) no
  // longer answer for a 40-row regress. b3dcdae9 and frozen main: `60`.
  {
    piet::SessionState s(workdir);
    const auto lines = displays(s, kPanel +
                                       "quietly pca x1 x2\n"
                                       "quietly regress y x2 if c < 3\n"
                                       "display \"PK|\" e(N) \"|`e(cmd)'|`e(title)'|\"\n");
    const std::string got = lineStarting(lines, "PK|");
    // e(title) is regress's own "Linear regression" (r150 WS-G; StataBE 19.5),
    // not pca's "Principal components".
    erCheck(got == "PK|40|regress|Linear regression|",
            "W2D ereturn (27) FU-375: pca then regress if c < 3 -> e(N) 40, regress's own "
            "e(title), no pca e(title) -- got `" + got + "`");
  }

  // (28) Rows E27/E28 for a pre-install writer: oaxaca stored, a regress (its
  // own e(b)/e(N)), restore (oaxaca's 1 x 10 e(b) and e(N) 200 come back), a
  // regress again (its own again). b3dcdae9: R1 and R3 read 10 / 200.
  {
    piet::SessionState s(workdir);
    const auto lines = displays(s, kOxData +
                                       "quietly oaxaca y x1 x2, by(g) pooled relax\n"
                                       "estimates store w2dox\n"
                                       "quietly regress y x1 x2 if t == 1\n"
                                       "matrix b = e(b)\n"
                                       "display \"OR1|\" colsof(b) \"|`e(cmd)'|\" e(N) \"|\"\n"
                                       "estimates restore w2dox\n"
                                       "matrix b = e(b)\n"
                                       "display \"OR2|\" colsof(b) \"|`e(cmd)'|\" e(N) \"|\"\n"
                                       "quietly regress y x1 x2 if t == 1\n"
                                       "matrix b = e(b)\n"
                                       "display \"OR3|\" colsof(b) \"|`e(cmd)'|\" e(N) \"|\"\n");
    const std::string r1 = lineStarting(lines, "OR1|");
    const std::string r2 = lineStarting(lines, "OR2|");
    const std::string r3 = lineStarting(lines, "OR3|");
    erCheck(r1 == "OR1|3|regress|99|" && r2 == "OR2|10|oaxaca|200|" && r3 == "OR3|3|regress|99|",
            "W2D ereturn (28) FU-375: oaxaca store/regress/restore/regress -- want "
            "`OR1|3|regress|99|` `OR2|10|oaxaca|200|` `OR3|3|regress|99|`, got `" + r1 + "` `" +
                r2 + "` `" + r3 + "`");
  }

  // (29) r150 WS-G round 2: regress's e(beta) is read from the live block at
  // the moment of need, never by swapping session.lastEstBlock at the fit (the
  // round-1 swap could hand the restored block the previous fit's freed
  // address, so liveEstimationIdentity saw "e() unchanged", annotateEbStripe
  // never ran, and w2c_postest grouped_ib2's `matrix ib2_b = e(b)` lost its
  // `2b.` columns about one replay in three). The factor fit's e(beta) carries
  // e(b)'s base column at 0 (StataBE 19.5 `regress y x1 i.g`: e(beta) `x1 1b.g
  // 2.g 3.g`, base +0.0X-3ff), `_cons` dropped, and each slope is
  // b * sd(x) / sd(y) ([R] regress Stored results).
  {
    const std::string kFv =
        "clear\n"
        "set obs 60\n"
        "gen double x1 = mod(_n*7,23)\n"
        "gen double x2 = mod(_n*13,17)\n"
        "gen double y = 1 + 2*x1 - .5*x2 + mod(_n*11,31)/31\n"
        "gen g = mod(_n,5)\n"
        "gen c = mod(_n,4)\n";
    piet::SessionState s(workdir);
    const auto lines = displays(
        s, kFv +
               "regress y i.(c g) x1\n"
               "regress y ib2.(c g)\n"
               "matrix B2 = e(b)\n"
               "display \"IB|`: colfullnames B2'|\"\n"
               "quietly summarize y\n"
               "scalar sdy = r(sd)\n"
               "quietly summarize x1\n"
               "scalar sdx = r(sd)\n"
               "regress y x1 i.c\n"
               "matrix BT = e(beta)\n"
               "local m : e(matrices)\n"
               "display \"BT|`: colfullnames BT'|\" el(BT,1,2) \"|`m'|\" "
               "(abs(el(BT,1,1) - _b[x1]*sdx/sdy) < 1e-12*abs(el(BT,1,1))) \"|\"\n");
    const std::string ib = lineStarting(lines, "IB|");
    const std::string bt = lineStarting(lines, "BT|");
    erCheck(ib == "IB|0.c 1.c 2b.c 3.c 0.g 1.g 2b.g 3.g 4.g _cons|",
            "W2D ereturn (29) WS-G: after `regress y i.(c g) x1`, `regress y ib2.(c g)`'s e(b) "
            "keeps its base columns -- got `" + ib + "`");
    erCheck(bt == "BT|x1 0b.c 1.c 2.c 3.c|0|b V beta|1|",
            "W2D ereturn (29) WS-G: e(beta) after `regress y x1 i.c` is e(b)'s stripe less "
            "_cons with a 0 base column, b*sd(x)/sd(y), listed `b V beta` -- got `" + bt + "`");
  }

  fs::remove_all(workdir, ec);
  std::printf("[selftest] W2D ereturn: %d checks executed\n", gEreturnChecks);
}
