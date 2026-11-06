// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// W2B lane `iv` (task w2b-iv-options) -- selftest arms.
//
// The IV / PPML option rows of tests/eval/golden/aer_w2/w2b_estim.log:
//   ivregress_first (:919), ivregress_exog_after_paren (:945), ivreg2_partial
//   (:1013), ivreg2_ctl (:1060), ivreghdfe_first (:1185),
//   ivreghdfe_exog_after_paren (:1243), ivreghdfe_ctl (:1301), ppmlhdfe_d
//   (:1332), ppmlhdfe_absorb_d_named (:1370).
// Every non-control arm is a DISCRIMINATOR: its message says what the frozen
// main answered (all rc 198: "option `first` not allowed", "unexpected tokens
// after the (endog = instruments) block: x2", "option `partial` not allowed",
// "option `d` not allowed").
//
// Fixture: w2b_estim.do's frame with its FLOAT storage replicated (the
// selftest_w2_estimopts.cpp BUG-568 recipe): each default-typed `generate` is
// evaluated in double, rounded to float and loaded as a literal; z1 and y read
// the float x1/x2 as Stata's do. Unrounded doubles miss the oracle's b by 3e-9.
// Numbers are read from the estimation block and r(), never through `display`
// (BUG-554: `display %18.0g _b[x]` still prints 9 significant digits).
//
// Tiers (tests/eval/TOLERANCE.md): 2SLS b/se T2 (1e-9); ppmlhdfe b T3 (1e-7,
// converged IRLS); first-stage cells at the log's display precision.
#include "selftest_w2.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <string>
#include <vector>

#include "piet/dispatcher.h"
#include "piet/eval/errors.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

namespace {

using ojson = nlohmann::ordered_json;

struct Reply {
  std::string rc = "0";
  std::string msg;
  ojson est;         // the last estimation block (null when none)
  std::string text;  // every display block's text, concatenated
};

Reply run(piet::SessionState& session, const std::string& cmd) {
  Reply r;
  try {
    for (const auto& b : piet::dispatch(cmd, session)) {
      if (!b.is_object()) continue;
      const std::string t = b.value("type", std::string());
      if (t == "error") {
        r.rc = piet::eval::coarseRc(b);
        r.msg += b.value("message", std::string());
      } else if (t == "estimation") {
        r.est = b;
      } else if (t == "display") {
        r.text += b.value("text", std::string());
      }
    }
  } catch (const std::exception& e) {
    r.rc = "EXC";
    r.msg = e.what();
  }
  return r;
}

const double kNaN = std::nan("");

double coefOf(const ojson& est, const std::string& name, const char* key) {
  try {
    if (!est.is_object() || !est.contains("coeftable") || !est["coeftable"].is_array()) return kNaN;
    for (const auto& row : est["coeftable"]) {
      if (!row.is_object() || row.value("name", std::string()) != name) continue;
      if (row.contains(key) && row[key].is_number()) return row[key].get<double>();
      return kNaN;
    }
  } catch (...) {
  }
  return kNaN;
}

size_t rowsOf(const ojson& est) {
  if (!est.is_object() || !est.contains("coeftable") || !est["coeftable"].is_array()) return 0;
  return est["coeftable"].size();
}

bool close(double a, double b, double rtol) {
  return std::isfinite(a) && std::isfinite(b) && std::fabs(a - b) <= rtol * std::fabs(b);
}

// A table cell as the log prints it: within half a unit of its last decimal.
bool displayed(double got, const char* cell) {
  const double want = std::strtod(cell, nullptr);
  const char* dot = std::strchr(cell, '.');
  const int decimals = dot ? static_cast<int>(std::strlen(dot + 1)) : 0;
  return std::isfinite(got) && std::fabs(got - want) <= 0.5 * std::pow(10.0, -decimals);
}

std::string fmt(double v) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.17g", v);
  return buf;
}

// The one first stage `first` attached to the block (structured), or null.
ojson firstStage(const ojson& est) {
  if (!est.is_object() || !est.contains("first_stage") || !est["first_stage"].is_array() ||
      est["first_stage"].size() != 1)
    return ojson();
  return est["first_stage"][0];
}

double stageCell(const ojson& fs, const char* key, const std::string& name) {
  if (!fs.is_object() || !fs.contains(key) || !fs[key].is_object() || !fs[key].contains(name) ||
      !fs[key][name].is_number())
    return kNaN;
  return fs[key][name].get<double>();
}

bool warningsMention(const ojson& est, const std::string& needle) {
  if (!est.is_object() || !est.contains("warnings") || !est["warnings"].is_array()) return false;
  for (const auto& w : est["warnings"])
    if (w.is_string() && w.get<std::string>().find(needle) != std::string::npos) return true;
  return false;
}

double rScalar(piet::SessionState& s, const char* name) {
  const double* v = s.eval.rValues.find(name);
  return v != nullptr ? *v : kNaN;
}

void seed(piet::SessionState& s) {
  (void)piet::dispatch("clear", s);
  (void)piet::dispatch("set obs 60", s);
  (void)piet::dispatch("generate double id = ceil(_n/6)", s);
  (void)piet::dispatch("generate double t = mod(_n-1,6)+1", s);
  (void)piet::dispatch("generate double x1 = .", s);
  (void)piet::dispatch("generate double x2 = .", s);
  (void)piet::dispatch("generate double z1 = .", s);
  (void)piet::dispatch("generate double y = .", s);
  auto fl = [](double v) { return static_cast<double>(static_cast<float>(v)); };
  for (int n = 1; n <= 60; ++n) {
    const double id = std::ceil(n / 6.0);
    const double x1 = fl(static_cast<double>((n * 7) % 11) + static_cast<double>(n % 5) / 10.0);
    const double x2 = fl(static_cast<double>((n * 3) % 5) + static_cast<double>((n * 13) % 7) / 10.0);
    const double z1 = fl(static_cast<double>((n * 5) % 13) + x1 / 2.0);
    const double y = fl(1.0 + 2.0 * x1 - x2 + id / 3.0 + static_cast<double>((n * 11) % 17) / 5.0);
    const std::string in = " in " + std::to_string(n);
    (void)piet::dispatch("quietly replace x1 = " + fmt(x1) + in, s);
    (void)piet::dispatch("quietly replace x2 = " + fmt(x2) + in, s);
    (void)piet::dispatch("quietly replace z1 = " + fmt(z1) + in, s);
    (void)piet::dispatch("quietly replace y = " + fmt(y) + in, s);
  }
  (void)piet::dispatch("generate byte d = (y > 12)", s);
  (void)piet::dispatch("xtset id t", s);
}

}  // namespace

void testW2bIv() {
  std::printf("[selftest] --- W2B iv: ivregress/ivreghdfe first + exog after the paren block, "
              "ivreg2 partial(), ppmlhdfe d ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_w2biv").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  const double kT2 = 1e-9, kT3 = 1e-7;
  piet::SessionState s(workdir);
  seed(s);

  // ── ivregress ──────────────────────────────────────────────────────────
  {
    // ivregress_first (:919): the fit of the option-free command, plus the
    // first stage the engine estimated -- x1 on x2 z1 _cons, :894-896.
    Reply r = run(s, "ivregress 2sls y x2 (x1 = z1), first");
    const ojson st = firstStage(r.est);
    w2check(r.rc == "0" && close(coefOf(r.est, "x1", "b"), 1.940390224456596, kT2) &&
                close(coefOf(r.est, "x1", "se"), .1611032742759025, kT2),
            "w2b_estim ivregress_first: rc 0, b_x1 1.940390224456596, se_x1 .1611032742759025 "
            "(main: rc 198 option `first` not allowed). Got rc " + r.rc + " b=" +
                fmt(coefOf(r.est, "x1", "b")) + " se=" + fmt(coefOf(r.est, "x1", "se")) + " " + r.msg);
    w2check(displayed(stageCell(st, "coef", "z1"), ".2724199") &&
                displayed(stageCell(st, "se", "z1"), ".0983224") &&
                displayed(stageCell(st, "coef", "x2"), ".1668943") &&
                displayed(stageCell(st, "coef", "(intercept)"), "2.505772") &&
                st.value("names", ojson::array()) == ojson({"x2", "z1", "(intercept)"}) &&
                r.text.find("First-stage regressions") != std::string::npos &&
                r.text.find("F(2, 57)      =   4.11") != std::string::npos &&
                r.text.find("Prob > F      = 0.0215") != std::string::npos &&
                r.text.find("R-squared     = 0.1261") != std::string::npos &&
                r.text.find("Adj R-squared = 0.0954") != std::string::npos &&
                !warningsMention(r.est, "first-stage regression of x1"),
            "w2b_estim ivregress_first (:883-896): the first stage x1 on x2 z1 _cons is "
            "displayed in ivregress's row order, z1 .2724199 (.0983224), x2 .1668943, _cons "
            "2.505772, under its header F(2, 57) 4.11, Prob > F 0.0215, R-squared .1261, Adj "
            "R-squared .0954, in a display block ahead of the IV table (BUG-1227; it rode in "
            "the block's warnings with no F or R-squared). Got " +
                (st.is_null() ? std::string("no first_stage") : st.dump()) + " text `" + r.text +
                "`");
    w2check(!s.eval.eStrings.contains("firsteqs"),
            "w2b_estim ivregress_first: `first` stores no e(firsteqs) (that is ivreg2's "
            "savefirst, not `first`).");

    // ivregress_exog_after_paren (:945): `y (x1 = z1) x2` is the `y x2 (x1 = z1)`
    // model, here under vce(robust).
    Reply a = run(s, "ivregress 2sls y (x1 = z1) x2, vce(robust)");
    w2check(a.rc == "0" && close(coefOf(a.est, "x1", "b"), 1.940390224456596, kT2) &&
                close(coefOf(a.est, "x1", "se"), .1770926044974198, kT2) &&
                std::isfinite(coefOf(a.est, "x2", "b")) && rowsOf(a.est) == 3,
            "w2b_estim ivregress_exog_after_paren: rc 0, b_x1 1.940390224456596, se_x1 "
            ".1770926044974198, rows x1 x2 _cons (main: rc 198 unexpected tokens after the "
            "block: x2). Got rc " + a.rc + " b=" + fmt(coefOf(a.est, "x1", "b")) + " se=" +
                fmt(coefOf(a.est, "x1", "se")) + " " + a.msg);

    // `first` with gmm/liml has no measured table: refused by name, e() untouched.
    (void)run(s, "ivregress 2sls y x2 (x1 = z1)");
    Reply g = run(s, "ivregress gmm y x2 (x1 = z1), first");
    w2check(g.rc == "198" && g.est.is_null(),
            "ivregress gmm ..., first: refused rc 198 with no fit (no measured first-stage "
            "table for gmm). Got rc " + g.rc + " " + g.msg);
  }

  // ── ivreg2 ─────────────────────────────────────────────────────────────
  {
    // ivreg2_ctl (:1060) -- control, unchanged: x1 x2 _cons, se .1611032742759016.
    Reply c = run(s, "ivreg2 y x2 (x1 = z1)");
    w2check(c.rc == "0" && close(coefOf(c.est, "x1", "se"), .1611032742759016, kT2) &&
                rowsOf(c.est) == 3,
            "w2b_estim ivreg2_ctl (control): rc 0, se_x1 .1611032742759016, 3 rows. Got rc " +
                c.rc + " se=" + fmt(coefOf(c.est, "x1", "se")) + " " + c.msg);

    // ivreg2_partial (:1013): x2 and _cons partialled out -- only x1 is reported,
    // with the full fit's b and se (FWL; ivreg2 counts x2 in K), and the model
    // statistic is x1's own z^2 (:978 F(1,57)).
    Reply p = run(s, "ivreg2 y x2 (x1 = z1), partial(x2)");
    const double z = coefOf(p.est, "x1", "t");
    const double chi2 = (p.est.is_object() && p.est.contains("chi2") && p.est["chi2"].is_number())
                            ? p.est["chi2"].get<double>()
                            : kNaN;
    w2check(p.rc == "0" && close(coefOf(p.est, "x1", "b"), 1.940390224456596, kT2) &&
                close(coefOf(p.est, "x1", "se"), .1611032742759026, kT2) && rowsOf(p.est) == 1 &&
                close(chi2, z * z, 1e-12) && warningsMention(p.est, "Partialled-out: x2 _cons"),
            "w2b_estim ivreg2_partial: rc 0, b_x1 1.940390224456596, se_x1 .1611032742759026, "
            "x1 the only row, chi2 = z^2 (main: rc 198 option `partial` not allowed). Got rc " +
                p.rc + " b=" + fmt(coefOf(p.est, "x1", "b")) + " se=" +
                fmt(coefOf(p.est, "x1", "se")) + " rows=" + std::to_string(rowsOf(p.est)) +
                " chi2=" + fmt(chi2) + " " + p.msg);

    // partial() names an included exogenous regressor or nothing at all.
    Reply bad = run(s, "ivreg2 y x2 (x1 = z1), partial(z1)");
    w2check(bad.rc == "198" && bad.est.is_null(),
            "ivreg2 ..., partial(z1): z1 is an excluded instrument, refused rc 198. Got rc " +
                bad.rc + " " + bad.msg);
    // ivregress has no partial(): still refused.
    Reply ivp = run(s, "ivregress 2sls y x2 (x1 = z1), partial(x2)");
    w2check(ivp.rc == "198" && ivp.est.is_null(),
            "ivregress ..., partial(x2): not an ivregress option, rc 198. Got rc " + ivp.rc);
  }

  // ── ivreghdfe ──────────────────────────────────────────────────────────
  {
    Reply c = run(s, "ivreghdfe y x2 (x1 = z1), absorb(id)");
    w2check(c.rc == "0" && close(coefOf(c.est, "x1", "b"), 2.00795312049193, kT2) &&
                close(coefOf(c.est, "x1", "se"), .1308860665203314, kT2),
            "w2b_estim ivreghdfe_ctl (control): rc 0, b_x1 2.00795312049193, se_x1 "
            ".1308860665203314. Got rc " + c.rc + " se=" + fmt(coefOf(c.est, "x1", "se")) + " " +
                c.msg);

    // ivreghdfe_first (:1185): same fit; the absorbed first stage z1 x2, no _cons
    // (:1078-1079).
    Reply f = run(s, "ivreghdfe y x2 (x1 = z1), absorb(id) first");
    const ojson st = firstStage(f.est);
    w2check(f.rc == "0" && close(coefOf(f.est, "x1", "b"), 2.00795312049193, kT2) &&
                close(coefOf(f.est, "x1", "se"), .1308860665203314, kT2) &&
                displayed(stageCell(st, "coef", "z1"), ".2657743") &&
                displayed(stageCell(st, "se", "z1"), ".1072108") &&
                displayed(stageCell(st, "coef", "x2"), ".159917") &&
                displayed(stageCell(st, "se", "x2"), ".2957897") &&
                st.value("names", ojson::array()) == ojson({"z1", "x2"}),
            "w2b_estim ivreghdfe_first: rc 0, the ctl fit, and the absorbed first stage z1 "
            ".2657743 (.1072108), x2 .159917 (.2957897) in ivreg2 order with no _cons (main: "
            "rc 198 option `first` not allowed). Got rc " + f.rc + " stage " +
                (st.is_null() ? std::string("none") : st.dump()) + " " + f.msg);

    // ivreghdfe_exog_after_paren (:1243).
    Reply a = run(s, "ivreghdfe y (x1 = z1) x2, absorb(id)");
    w2check(a.rc == "0" && close(coefOf(a.est, "x1", "b"), 2.00795312049193, kT2) &&
                close(coefOf(a.est, "x1", "se"), .1308860665203314, kT2),
            "w2b_estim ivreghdfe_exog_after_paren: rc 0, b_x1 2.00795312049193, se_x1 "
            ".1308860665203314 (main: rc 198 unexpected tokens after the block: x2). Got rc " +
                a.rc + " se=" + fmt(coefOf(a.est, "x1", "se")) + " " + a.msg);
  }

  // ── ppmlhdfe d ─────────────────────────────────────────────────────────
  {
    // ppmlhdfe_d (:1332): rc 0 and the option-free fit; bare `d` saves
    // _ppmlhdfe_d (as ppmlhdfe does, identified from its ado), which without absorb() is 0 on the
    // estimation sample (the only absorbed term is the constant).
    Reply r = run(s, "ppmlhdfe d x1 x2, d");
    Reply sum = run(s, "quietly summarize _ppmlhdfe_d");
    const double n0 = rScalar(s, "N"), s0 = rScalar(s, "sum");
    w2check(r.rc == "0" && close(coefOf(r.est, "x1", "b"), .2632300535840953, kT3) &&
                sum.rc == "0" && n0 == 60.0 && std::isfinite(s0) &&
                std::fabs(s0) < 1e-9,
            "w2b_estim ppmlhdfe_d: rc 0, b_x1 .2632300535840953, _ppmlhdfe_d 0 on all 60 rows "
            "(main: rc 198 option `d` not allowed). Got rc " + r.rc + " b=" +
                fmt(coefOf(r.est, "x1", "b")) + " N=" + fmt(n0) + " sum=" +
                fmt(s0) + " " + r.msg + sum.msg);

    // ppmlhdfe_absorb_d_named (:1370): d(dd) under absorb(id). d is the sum of the
    // FEs with xbd = _cons + xb + d = ln(mu) (ppmlhdfe's definition, identified from its source), so the PPML
    // score Σ(y - exp(xbd)) = 0 must hold over the sample: Σ exp(xbd) = Σ d = 34.
    Reply a = run(s, "ppmlhdfe d x1 x2, absorb(id) d(dd)");
    (void)run(s, "generate double __mu = exp(_b[_cons] + _b[x1]*x1 + _b[x2]*x2 + dd)");
    (void)run(s, "quietly summarize __mu");
    const double muSum = rScalar(s, "sum");
    (void)run(s, "quietly summarize dd");
    const double nDd = rScalar(s, "N");
    w2check(a.rc == "0" && close(coefOf(a.est, "x1", "b"), .2694167988041918, kT3) &&
                nDd == 60.0 && close(muSum, 34.0, kT3),
            "w2b_estim ppmlhdfe_absorb_d_named: rc 0, b_x1 .2694167988041918, dd on 60 rows, "
            "sum exp(_cons + xb + dd) = sum d = 34 (main: rc 198 option `d` not allowed). "
            "Got rc " + a.rc + " b=" + fmt(coefOf(a.est, "x1", "b")) + " N=" +
                fmt(nDd) + " sum_mu=" + fmt(muSum) + " " + a.msg);
    // The FE score holds inside EVERY group (sum_g exp(_cons + xb + dd) = sum_g d), which
    // given the oracle's b and _cons pins every row of dd: a closed form that only
    // balances the grand total fails here. The d column has no oracle record yet.
    {
      std::string bad;
      for (int g = 1; g <= 10; ++g) {
        const std::string gs = std::to_string(g);
        (void)run(s, "quietly summarize __mu if id == " + gs);
        const double mg = rScalar(s, "sum");
        (void)run(s, "quietly summarize d if id == " + gs);
        const double yg = rScalar(s, "sum");
        if (!(std::fabs(mg - yg) <= 1e-7 * std::max(1.0, yg))) {
          bad += " g" + gs + ":" + fmt(mg) + "vs" + fmt(yg);
        }
      }
      w2check(bad.empty(),
              "ppmlhdfe d(dd): per-group score sum_g exp(_cons + xb + dd) = sum_g d for g=1..10. "
              "Off in:" + bad);
    }
    // e(d) names the column and dd is a double labelled "Sum of fixed effects"
    // (StataBE 19.5, BUG-1250/BUG-1252). Before, the block's warnings said e(d) is not
    // posted (FU-153), and dd was unlabelled.
    w2check(s.eval.eStrings.get("d", std::string()) == "dd" && !warningsMention(a.est, "e(d)") &&
                s.varMeta["dd"].stataType.value_or("") == "double" &&
                s.varMeta["dd"].label.value_or("") == "Sum of fixed effects",
            "ppmlhdfe d(dd): e(d) is `dd` and dd is double, labelled \"Sum of fixed effects\" "
            "(BUG-1250/BUG-1252). Got e(d) `" + s.eval.eStrings.get("d", std::string()) + "`");
    (void)run(s, "drop __mu");

    // d(newvar) is `confirm new variable`: r(110) before any fit, data untouched.
    Reply again = run(s, "ppmlhdfe d x1 x2, absorb(id) d(dd)");
    w2check(again.rc == "110" && again.est.is_null(),
            "ppmlhdfe ..., d(dd) with dd existing: r(110), no fit. Got rc " + again.rc + " " +
                again.msg);
    // The multi-FE fit now returns the absorbed predictor from its final IRLS
    // state (w2d_uncovered.log ppmlhdfe_xi_absorb_cluster2_d_001). Its saved
    // sum of effects must reconstruct the fitted means in both FE dimensions.
    Reply two = run(s, "ppmlhdfe d x1 x2, absorb(id t) d(dd2)");
    Reply dd2 = run(s, "confirm variable dd2");
    w2check(two.rc == "0" && !two.est.is_null() && dd2.rc == "0",
            "ppmlhdfe ..., absorb(id t) d(dd2): fitted means are available. Got rc " + two.rc +
                " confirm rc " + dd2.rc + " " + two.msg);
    (void)run(s, "generate double __mu2 = exp(_b[_cons] + _b[x1]*x1 + _b[x2]*x2 + dd2)");
    std::string badTwo;
    for (const auto& dim : {std::make_pair(std::string("id"), 10),
                            std::make_pair(std::string("t"), 6)}) {
      for (int g = 1; g <= dim.second; ++g) {
        const std::string cond = " if " + dim.first + " == " + std::to_string(g);
        (void)run(s, "quietly summarize __mu2" + cond);
        const double mu = rScalar(s, "sum");
        (void)run(s, "quietly summarize d" + cond);
        const double y = rScalar(s, "sum");
        if (!(std::fabs(mu - y) <= 1e-7 * std::max(1.0, y)))
          badTwo += " " + dim.first + std::to_string(g) + ":" + fmt(mu) + "vs" + fmt(y);
      }
    }
    w2check(badTwo.empty(), "ppmlhdfe multi-FE d(dd2): each id and t score balances. Off in:" + badTwo);
    (void)run(s, "drop __mu2");
    // plain poisson has no d.
    Reply pd = run(s, "poisson d x1 x2, d");
    w2check(pd.rc == "198" && pd.est.is_null(),
            "poisson ..., d: not a poisson option, rc 198. Got rc " + pd.rc);

    // d/d() with a weight: refused by name before the fit (no corpus use, the weighted
    // closed form is uncaptured, the engine refuses weighted --fe). Main answered rc 198
    // "option `d` not allowed" (no mention of weights) or, for absorb, rc 1; the unweighted
    // closed form would otherwise be written under pweights.
    (void)run(s, "generate double w = 1 + mod(_n,4)");
    (void)run(s, "drop _ppmlhdfe_d");
    Reply wa = run(s, "ppmlhdfe d x1 x2 [pw=w], absorb(id) d(dw)");
    Reply dw = run(s, "confirm variable dw");
    w2check(wa.rc == "198" && wa.est.is_null() && wa.msg.find("weights") != std::string::npos &&
                dw.rc != "0",
            "ppmlhdfe ... [pw=w], absorb(id) d(dw): refused rc 198 naming weights, no dw column. "
            "Got rc " + wa.rc + " confirm rc " + dw.rc + " " + wa.msg);
    Reply wb = run(s, "ppmlhdfe d x1 x2 [pw=w], d");
    Reply wbc = run(s, "confirm variable _ppmlhdfe_d");
    w2check(wb.rc == "198" && wb.est.is_null() && wb.msg.find("weights") != std::string::npos &&
                wbc.rc != "0",
            "ppmlhdfe ... [pw=w], d: refused rc 198 naming weights, no _ppmlhdfe_d column. Got rc " +
                wb.rc + " confirm rc " + wbc.rc + " " + wb.msg);
    Reply wc = run(s, "ppmlhdfe d x1 x2 [pw=w]");
    w2check(wc.rc == "0" && std::isfinite(coefOf(wc.est, "x1", "b")),
            "ppmlhdfe d x1 x2 [pw=w] without d: still runs (control). Got rc " + wc.rc + " " +
                wc.msg);
  }

  fs::remove_all(workdir, ec);
}
