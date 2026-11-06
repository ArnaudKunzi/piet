// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// E3 arms for `ritest` (handlers_e3_ritest.cpp). The oracle is
// tests/eval/golden/aer_w2/w2j_e3_ritest.log: R01_basic (:347) for the r()
// layout of a plain permutation -- r(N) the full _N, r(N_reps) = reps(),
// r(N_strata) 1 and r(N_clust) = the rows without strata()/cluster(), r(cmd)
// ritest, r(command) the rebuilt command (`reg` -> `regress`), the two-sided
// event, a 1 x K r(p) -- and X05 (e() is the original estimation afterwards);
// the Z refusals for the rcs: Z01 no colon 198, Z02 an unknown option 198, Z03
// reps(0) 198, Z04 an unknown resampvar 111, Z06 the resampvar in strata() 198,
// Z07 a treatment that varies within a cluster 9999, Z08 a statistic missing on
// the full data 322, Z15 an inner command without e(cmd) 301.
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

void ritestData(piet::SessionState& s) {
  for (const char* c : {"clear", "set obs 24", "generate double price = 3000 + 17*_n*_n",
                        "generate double mpg = 10 + mod(_n*7,13)",
                        "generate byte foreign = mod(_n,2)", "generate byte g = ceil(_n/4)",
                        "generate byte tg = mod(g,2)"})
    (void)piet::dispatch(c, s);
}

std::string rcOfLine(piet::SessionState& s, const std::string& line) {
  std::string rc = "0";
  try {
    for (const auto& b : piet::dispatch(line, s)) {
      if (b.is_object() && b.value("type", std::string()) == "error") rc = piet::eval::coarseRc(b);
    }
  } catch (const std::exception& e) {
    rc = std::string("EXC ") + e.what();
  }
  return rc;
}

}  // namespace

void testW2E3Ritest() {
  std::printf("[selftest] --- W2 E3 ritest: permutation inference (w2j_e3_ritest.log) ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_w2e3_ritest").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  {
    piet::SessionState s(workdir);
    ritestData(s);
    const std::string rc =
        rcOfLine(s, "ritest foreign _b[foreign], reps(20) seed(7) nodots: reg price foreign");
    w2check(rc == "0", "ritest R01 shape: the call runs (oracle rc 0). Got rc " + rc);
    const double* N = s.eval.rValues.find("N");
    const double* nr = s.eval.rValues.find("N_reps");
    const double* ns = s.eval.rValues.find("N_strata");
    const double* nc = s.eval.rValues.find("N_clust");
    w2check(N && *N == 24 && nr && *nr == 20 && ns && *ns == 1 && nc && *nc == 24,
            "ritest R01 shape: r(N) 24, r(N_reps) 20, r(N_strata) 1, r(N_clust) 24 "
            "(w2j_e3_ritest.log:347 `72|40|1|72`)");
    const std::string* cmd = s.eval.rStrings.find("cmd");
    const std::string* command = s.eval.rStrings.find("command");
    const std::string* event = s.eval.rStrings.find("event");
    w2check(cmd && *cmd == "ritest" && command && *command == "regress price foreign" && event &&
                *event == "|T| >= |T(obs)|",
            "ritest R01 shape: r(cmd) ritest, r(command) `regress price foreign`, r(event) "
            "`|T| >= |T(obs)|` (log :347)");
    const piet::eval::EvalMatrix* p = s.eval.findClassMatrix('r', "p");
    w2check(p != nullptr && p->data.size() == 1 && p->data[0].size() == 1 &&
                p->colnames.size() == 1 && p->colnames[0] == "_pm_1",
            "ritest R01 shape: r(p) is 1 x 1 with column _pm_1 (log :347 `1x1|..|_pm_1`)");
    const std::string* ecmd = s.eval.eStrings.find("cmd");
    w2check(ecmd != nullptr && *ecmd == "regress",
            "ritest X05: e() is the original estimation afterwards (e(cmd) regress)");
  }
  struct Arm {
    const char* line;
    const char* rc;
    const char* arm;
  };
  static const Arm kArms[] = {
      {"ritest foreign _b[foreign], reps(5) reg price foreign", "198", "Z01_no_colon"},
      {"ritest foreign _b[foreign], reps(5) bogusopt(1) nodots: reg price foreign", "198",
       "Z02_unknown_option"},
      {"ritest foreign _b[foreign], reps(0) nodots: reg price foreign", "198", "Z03_reps0"},
      {"ritest nosuchvar _b[foreign], reps(5) nodots: reg price foreign", "111",
       "Z04_no_resampvar"},
      {"ritest foreign _b[foreign], reps(5) nodots strata(foreign): reg price foreign", "198",
       "Z06_resampvar_in_strata"},
      {"ritest foreign _b[foreign], reps(5) nodots cluster(g): reg price foreign", "9999",
       "Z07_cluster_not_constant"},
      {"ritest foreign e(nosuch), reps(5) nodots: reg price foreign", "322",
       "Z08_stat_missing_full"},
      {"ritest foreign r(mean), reps(5) nodots: summarize price", "301",
       "Z15_rclass_inner_no_ecmd"}};
  for (const Arm& a : kArms) {
    piet::SessionState s(workdir);
    ritestData(s);
    const std::string rc = rcOfLine(s, a.line);
    w2check(rc == a.rc, std::string("ritest ") + a.arm + ": `" + a.line + "` is r(" + a.rc +
                            ") as in w2j_e3_ritest.log. Got rc " + rc);
  }
  {
    // The inner-command shape the oracle's arms run (w2j_e3_ritest.do:48-114
    // `_e3_inv`): an eclass program with `syntax [if] [in], T(varname numeric)
    // [Y(varname numeric)]` (program.cpp bindSyntax), `marksample touse,
    // novarlist` folding the bound if/in, and `ereturn post b, esample(touse)
    // obs(n)`: e(N)/e(n) and `count if e(sample)` agree with the if-condition.
    piet::SessionState s(workdir);
    ritestData(s);
    std::vector<std::string> shownLines;
    try {
      auto res = piet::doexec::executeDoSource(
          "capture program drop _st_inv\n"
          "program define _st_inv, eclass\n"
          "    syntax [if] [in], T(varname numeric) [Y(varname numeric)]\n"
          "    marksample touse, novarlist\n"
          "    quietly count if `touse' & `t'==1\n"
          "    local n1 = r(N)\n"
          "    quietly count if `touse'\n"
          "    local n = r(N)\n"
          "    tempname b\n"
          "    matrix `b' = (0)\n"
          "    matrix colnames `b' = c\n"
          "    ereturn post `b', esample(`touse') obs(`n')\n"
          "    ereturn local cmd \"_st_inv\"\n"
          "    ereturn scalar n1 = `n1'\n"
          "end\n"
          "_st_inv if g <= 3, t(foreign)\n"
          "quietly count if e(sample)\n"
          "display \"SX|\" e(N) \"|\" e(n1) \"|\" r(N) \"|`e(cmd)'|\"\n"
          "capture noisily _st_inv, t(nosuchvar)\n"
          "display \"SY|\" _rc \"|\"\n"
          "capture noisily _st_inv, y(price)\n"
          "display \"SZ|\" _rc \"|\"\n",
          s, piet::dispatch);
      for (const auto& b : res.blocks)
        if (b.is_object() && b.value("type", std::string()) == "display")
          shownLines.push_back(b.value("text", std::string()));
    } catch (const std::exception& e) {
      shownLines.push_back(std::string("EXC:") + e.what());
    }
    auto line = [&](const std::string& pfx) {
      for (const auto& l : shownLines)
        if (l.compare(0, pfx.size(), pfx) == 0) return l;
      return std::string();
    };
    w2check(line("SX|") == "SX|12|6|12|_st_inv|",
            "syntax/marksample/esample: `_st_inv if g <= 3, t(foreign)` posts e(N) 12, e(n1) 6 "
            "and e(sample) = the 12 if-rows -- got `" + line("SX|") + "`");
    w2check(line("SY|") == "SY|111|",
            "syntax: T(varname numeric) of an unknown variable is r(111) -- got `" + line("SY|") + "`");
    w2check(line("SZ|") == "SZ|198|",
            "syntax: a required option left out is r(198) -- got `" + line("SZ|") + "`");
  }
  {
    // a cluster-constant treatment passes the Z07 check and counts cells
    piet::SessionState s(workdir);
    ritestData(s);
    const std::string rc =
        rcOfLine(s, "ritest tg _b[tg], reps(5) seed(3) nodots cluster(g): reg price tg");
    const double* nc = s.eval.rValues.find("N_clust");
    w2check(rc == "0" && nc && *nc == 6,
            "ritest ST02 shape: cluster(g) permutes 6 cells, r(N_clust) 6. Got rc " + rc);
  }
}
