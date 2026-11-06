// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Package-007 lane LB selftest: `leebounds` (SSC, Harald Tauchmann, version 1.5).
//
// Every number pinned here was read off StataBE 19.5 running the public ado on the
// same synthetic fixtures (scratch-r150/lee/oracle/lo.golden and
// scratch-r150/lee/lb/{p1,p4}.log, `%21.15g`), and is compared at the parity
// ladder's 1e-12 relative tolerance (tests/eval/TOLERANCE.md). The arms cover the
// branches the bounds take: the treatment group trimmed and the control group
// trimmed, a quantile between two data values and one ON a repeated value (the
// tie-share correction), a balanced design (the trimming target is an integer
// rank, where `_pctile`'s pweight tolerance decides) and an unbalanced one,
// if/in/select(), a two-valued non-0/1 treatment, level(), replay, the ado's own
// error codes, and the refusals by name.
#include "selftest_w2.h"

#include <cmath>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "piet/dispatcher.h"
#include "piet/eval/errors.h"
#include "piet/eval/session.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

namespace {

using ojson = nlohmann::ordered_json;

void lbCheck(bool cond, const std::string& label) { w2check(cond, "LB/leebounds: " + label); }

ojson run(piet::SessionState& s, const std::string& cmd) {
  try {
    return piet::dispatch(cmd, s);
  } catch (const std::exception& e) {
    return ojson::array({ojson{{"type", "error"}, {"message", std::string("EXC:") + e.what()}}});
  }
}

std::string rcOf(piet::SessionState& s, const std::string& cmd) {
  std::string rc = "0";
  for (const auto& b : run(s, cmd))
    if (b.is_object() && b.value("type", std::string()) == "error") rc = piet::eval::coarseRc(b);
  return rc;
}

// Concatenated display text of one line.
std::string textOf(piet::SessionState& s, const std::string& cmd) {
  std::string t;
  for (const auto& b : run(s, cmd))
    if (b.is_object() && b.value("type", std::string()) == "display")
      t += b.value("text", std::string()) + "\n";
  return t;
}

bool close12(double a, double want) {
  if (want == 0) return a == 0;
  return std::fabs(a - want) <= 1e-12 * std::fabs(want);
}

const piet::eval::EvalMatrix* eMat(piet::SessionState& s, const std::string& name) {
  for (const auto& [k, m] : s.eval.eMatrices)
    if (k == name) return &m;
  return nullptr;
}
double eSc(piet::SessionState& s, const std::string& k) {
  const double* d = s.eval.eScalars.find(k);
  return d != nullptr ? *d : std::nan("");
}
std::string eStr(piet::SessionState& s, const std::string& k) {
  const std::string* v = s.eval.eStrings.find(k);
  return v != nullptr ? *v : std::string("<unset>");
}

// e(b) / diag e(V) / e(N) e(Nsel) e(trim) against StataBE, plus V's zero
// off-diagonal and the treatvar:lower treatvar:upper stripe.
void pinFit(piet::SessionState& s, const std::string& tag, const std::string& tr, double bl,
            double bu, double vl, double vu, double n, double nsel, double trim,
            const std::string& trimmed) {
  const auto* b = eMat(s, "b");
  const auto* V = eMat(s, "V");
  const bool shape = b != nullptr && V != nullptr && b->data.size() == 1 && b->data[0].size() == 2 &&
                     V->data.size() == 2 && V->data[0].size() == 2 && V->data[1].size() == 2;
  lbCheck(shape, tag + ": e(b) is 1x2 and e(V) 2x2");
  if (!shape) return;
  lbCheck(close12(b->data[0][0], bl) && close12(b->data[0][1], bu),
          tag + ": e(b) matches StataBE to 1e-12");
  lbCheck(close12(V->data[0][0], vl) && close12(V->data[1][1], vu) && V->data[0][1] == 0 &&
              V->data[1][0] == 0,
          tag + ": e(V) matches StataBE to 1e-12 (diagonal, zero covariance)");
  const std::vector<std::string> names = {tr + ":lower", tr + ":upper"};
  lbCheck(b->colnames == names && V->colnames == names && V->rownames == names,
          tag + ": e(b)/e(V) are striped " + tr + ":lower " + tr + ":upper");
  lbCheck(eSc(s, "N") == n && eSc(s, "Nsel") == nsel && close12(eSc(s, "trim"), trim),
          tag + ": e(N), e(Nsel), e(trim) match StataBE");
  lbCheck(eStr(s, "trimmed") == trimmed && eStr(s, "treatment") == tr && eStr(s, "cmd") == "leebounds" &&
              eStr(s, "vce") == "analytic" && eStr(s, "properties") == "b V",
          tag + ": e(trimmed)/e(treatment)/e(cmd)/e(vce)/e(properties)");
}

void fixture(piet::SessionState& s, bool controlTrimmed, bool unbalanced) {
  (void)run(s, "clear");
  (void)run(s, "set obs 200");
  (void)run(s, "generate id = _n");
  (void)run(s, unbalanced ? "generate treat = mod(_n,3) == 0" : "generate treat = mod(_n,2)");
  (void)run(s, "generate double y = mod(_n*37,101)/10 + treat");
  if (controlTrimmed) {
    (void)run(s, "replace y = . if mod(_n*13,17) < 3 & treat==1");
    (void)run(s, "replace y = . if mod(_n*13,17) < 1 & treat==0");
  } else {
    (void)run(s, "replace y = . if mod(_n*13,17) < 3 & treat==0");
    (void)run(s, "replace y = . if mod(_n*13,17) < 1 & treat==1");
  }
  (void)run(s, "generate yd = round(y/2)");
}

void testBounds(piet::SessionState& s) {
  fixture(s, false, false);
  lbCheck(rcOf(s, "leebounds y treat") == "0", "plain fit is rc 0");
  pinFit(s, "plain", "treat", .597590361445781, 1.78313253012048, .268144706147398,
         .268489646244698, 200, 177, .117021276595745, "treatment");
  lbCheck(eStr(s, "depvar") == "y" && eStr(s, "title") == "Lee (2009) treatment effect bounds" &&
              eStr(s, "cmdline") == "leebounds y treat" && eSc(s, "level") == 95,
          "plain: e(depvar)/e(title)/e(cmdline)/e(level)");
  lbCheck(textOf(s, "display \"`: colfullnames e(b)'\"") == "treat:lower treat:upper\n",
          "`: colfullnames e(b)' reads treat:lower treat:upper");
  (void)run(s, "scalar __lb1 = _b[treat:lower]");
  (void)run(s, "scalar __lb2 = _se[treat:upper]");
  lbCheck(textOf(s, "display %21.15g __lb1") == "     .597590361445781\n" &&
              textOf(s, "display %21.15g __lb2") == "     .518159865528678\n",
          "_b[treat:lower] / _se[treat:upper] read the posted bounds (StataBE %21.15g text)");
  (void)run(s, "quietly count if e(sample)");
  const double* rn = s.eval.rValues.find("N");
  lbCheck(rn != nullptr && *rn == 200, "e(sample) marks the 200 sample rows");

  lbCheck(rcOf(s, "leebounds yd treat") == "0", "tied outcome is rc 0");
  pinFit(s, "ties", "treat", .313253012048193, .855421686746988, .0684765327289921,
         .0705115912385419, 200, 177, .117021276595745, "treatment");

  fixture(s, true, false);
  (void)run(s, "leebounds y treat");
  pinFit(s, "control-trimmed", "treat", .4, 1.77317073170732, .265325281387654, .2637019470315, 200,
         177, .136842105263158, "control");
  (void)run(s, "leebounds yd treat");
  pinFit(s, "control-trimmed ties", "treat", .182926829268293, .890243902439024,
         .0614429098551017, .0602882545250861, 200, 177, .136842105263158, "control");

  fixture(s, false, true);
  (void)run(s, "leebounds y treat");
  pinFit(s, "unbalanced", "treat", .499586776859504, 1.88407713498623, .283247972302693,
         .280701398697327, 200, 173, .14001421464108, "treatment");
  (void)run(s, "leebounds yd treat");
  pinFit(s, "unbalanced ties", "treat", .174380165289256, .844077134986226, .0760944346644589,
         .0779949094852943, 200, 173, .14001421464108, "treatment");
}

void testSampleOptions(piet::SessionState& s) {
  fixture(s, false, false);
  (void)run(s, "leebounds y treat if id > 20");
  pinFit(s, "if", "treat", .616000000000001, 1.8, .296929179472806, .294292116810143, 180, 160,
         .117647058823529, "treatment");
  lbCheck(eStr(s, "cmdline") == "leebounds y treat if id > 20", "if: e(cmdline)");
  (void)run(s, "leebounds y treat in 11/190");
  pinFit(s, "in", "treat", .573333333333333, 1.66133333333333, .298888032192192, .302081393393393,
         180, 159, .107142857142857, "treatment");
  (void)run(s, "leebounds y treat if id > 5 in 1/150");
  pinFit(s, "if+in", "treat", .517453703703703, 1.8362962962963, .375375154186694,
         .373783157324512, 145, 128, .129734085414988, "treatment");
  // a missing treatment value leaves the row out; a 2/7 treatment is a group()
  (void)run(s, "generate tm = treat");
  (void)run(s, "replace tm = . if mod(_n,11) == 0");
  (void)run(s, "leebounds y tm");
  pinFit(s, "missing treatment", "tm", .618666666666667, 1.91333333333333, .293890558239998,
         .29686288018832, 182, 161, .127906976744186, "treatment");
  (void)run(s, "generate t27 = treat*5 + 2");
  (void)run(s, "leebounds y t27");
  pinFit(s, "treatment 2/7", "t27", .597590361445781, 1.78313253012048, .268144706147398,
         .268489646244698, 200, 177, .117021276595745, "treatment");
  // select(): rows with y missing but selected leave the sample
  (void)run(s, "generate sel = y < .");
  (void)run(s, "replace sel = 0 if mod(_n,23) == 0");
  (void)run(s, "replace sel = . if mod(_n,29) == 0");
  (void)run(s, "leebounds y treat, select(sel)");
  pinFit(s, "select", "treat", .672727272727273, 1.83636363636364, .310099646903591,
         .315498771753755, 194, 164, .114942528735632, "treatment");
  lbCheck(eStr(s, "select") == "sel", "select: e(select)");
  // level(): [50,100) rounded to .01, otherwise c(level)
  (void)run(s, "leebounds y treat, level(90)");
  lbCheck(eSc(s, "level") == 90, "level(90): e(level) 90");
  (void)run(s, "leebounds y treat, level(99.555)");
  lbCheck(eSc(s, "level") == 99.56, "level(99.555): e(level) 99.56");
  (void)run(s, "leebounds y treat, level(30)");
  lbCheck(eSc(s, "level") == 95, "level(30) falls back to c(level)");
  (void)run(s, "leebounds y treat, vce(ana)");
  lbCheck(eStr(s, "vce") == "analytic" && eStr(s, "cmdline") == "leebounds y treat, vce(ana)",
          "vce(ana) is the analytic default");
  // the table
  const std::string t = textOf(s, "leebounds y treat, level(90)");
  lbCheck(t.find("Trimming porportion                =   0.1170") != std::string::npos &&
              t.find("[90% conf. interval]") != std::string::npos &&
              t.find("treat        |") != std::string::npos &&
              t.find("       lower |   .5975904   .5178269     1.15   0.248    -.2541591     1.44934") !=
                  std::string::npos,
          "the console table is the ado's (StataBE p1 H90 text)");
  // replay
  const std::string r = textOf(s, "leebounds, level(80)");
  lbCheck(r.find("[80% conf. interval]") != std::string::npos && eSc(s, "level") == 90,
          "replay redisplays at level(80) and leaves e(level)");
  lbCheck(rcOf(s, "leebounds, select(sel)") == "198", "replay takes level() only (r(198))");
}

void testErrors(piet::SessionState& s) {
  fixture(s, false, false);
  (void)run(s, "generate t3 = mod(_n,3)");
  lbCheck(rcOf(s, "leebounds y t3") == "149", "three treatment values: r(149)");
  (void)run(s, "generate sel2 = (y < .)*2");
  lbCheck(rcOf(s, "leebounds y treat, select(sel2)") == "109", "select() not 0/1: r(109)");
  (void)run(s, "generate sel1 = 1");
  lbCheck(rcOf(s, "leebounds y treat, select(sel1)") == "109", "select() constant: r(109)");
  lbCheck(rcOf(s, "leebounds y") == "102" && rcOf(s, "leebounds y treat id") == "103" &&
              rcOf(s, "leebounds if id > 3") == "100",
          "varlist(min=2 max=2): r(102)/r(103)/r(100)");
  lbCheck(rcOf(s, "leebounds y treat [aw=id]") == "101", "aweights: r(101)");
  lbCheck(rcOf(s, "leebounds y treat, level(99.995)") == "198", "level rounding to 100: r(198)");
  for (const char* cmd : {"leebounds y treat, tight(id)", "leebounds y treat, cieffect",
                          "leebounds y treat, vce(bootstrap)", "leebounds y treat [pw=id]",
                          "leebounds y treat [fw=id]"})
    lbCheck(rcOf(s, cmd) == "199", std::string("refused by name (r(199)): ") + cmd);
  (void)run(s, "generate str3 sy = \"a\"");
  lbCheck(rcOf(s, "leebounds sy treat") == "109", "string outcome: r(109)");
  // the selection-share checks
  (void)run(s, "clear");
  (void)run(s, "set obs 200");
  (void)run(s, "generate treat = mod(_n,2)");
  (void)run(s, "generate double y = _n");
  lbCheck(rcOf(s, "leebounds y treat") == "109", "no attrition: r(109) all observations selected");
  (void)run(s, "replace y = . if mod(_n,10) < 2");
  lbCheck(rcOf(s, "leebounds y treat") == "498", "equal selection shares: r(498)");
  (void)run(s, "replace y = _n");
  (void)run(s, "replace y = . if treat == 1");
  lbCheck(rcOf(s, "leebounds y treat") == "2000", "no selected treated: r(2000)");
  (void)run(s, "replace y = .");
  lbCheck(rcOf(s, "leebounds y treat") == "2000", "nothing selected: r(2000)");
  (void)run(s, "ereturn clear");
  lbCheck(rcOf(s, "leebounds") == "301", "replay without estimates: r(301)");
}

void testStore(piet::SessionState& s) {
  fixture(s, false, false);
  (void)run(s, "leebounds y treat");
  lbCheck(rcOf(s, "estimates store lb1") == "0", "estimates store takes the fit");
  (void)run(s, "regress y treat");
  lbCheck(rcOf(s, "estimates restore lb1") == "0" && eStr(s, "cmd") == "leebounds",
          "estimates restore brings it back");
  pinFit(s, "restored", "treat", .597590361445781, 1.78313253012048, .268144706147398,
         .268489646244698, 200, 177, .117021276595745, "treatment");
  // esttab reads the stripe as an equation: the package-007 table's options,
  // byte for byte StataBE's file (scratch-r150/lee/lb/p1_tab.tex, esttab 2.1.4).
  (void)run(s, "label variable treat \"Treatment\"");
  (void)run(s, "eststo clear");
  (void)run(s, "eststo m1: leebounds y treat");
  lbCheck(eStr(s, "cmdline") == "leebounds y treat", "eststo prefix: e(cmdline) is the inner command");
  const std::string path = s.workdir() + "/lb_tab.tex";
  lbCheck(rcOf(s, "esttab m1 using \"" + path + "\", replace nomtitles nodepvars nolines "
                  "star(* .1 ** .05 *** .01) se t(4) b(4) label nonotes nonum fragment nogaps "
                  "nobaselevels scalar(\"N Observations\")") == "0",
          "esttab of the stored model is rc 0");
  std::string got;
  {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream o;
    o << f.rdbuf();
    got = o.str();
  }
  const std::string want =
      "Treatment           &                     \\\\\n"
      "lower               &      0.5976         \\\\\n"
      "                    &    (0.5178)         \\\\\n"
      "upper               &      1.7831\\sym{***}\\\\\n"
      "                    &    (0.5182)         \\\\\n"
      "Observations        &         200         \\\\\n";
  lbCheck(got == want, "esttab: the equation header row and lower/upper rows are StataBE's bytes");
}

}  // namespace

void testW2LbLeebounds() {
  std::printf("[selftest] --- LB leebounds: Lee (2009) bounds against StataBE ---\n");
  namespace fs = std::filesystem;
  const std::string workdir =
      (fs::temp_directory_path() / "piet_native_selftest_lb_leebounds").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  {
    piet::SessionState s(workdir);
    testBounds(s);
    testSampleOptions(s);
    testErrors(s);
    testStore(s);
  }
  fs::remove_all(workdir, ec);
}
