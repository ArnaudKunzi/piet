// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// BUG-1067: which member of a collinear set reghdfe, areg and xtreg,fe omit.
//
// StataBE 19.5 (scratch-r150/c1067/m1.do, the r1.do fixture: 60 rows, g =
// mod(_n,5), x = mod(_n*7,13), z = 2*x, y = 1 + 0.5*x + mod(_n*3,7)): reghdfe (with
// and without absorb()), areg and xtreg,fe keep the EARLIER term of a collinear
// set and omit the later one (`x o.z _cons`, _b[x] .527368857557537, _se
// .0733801298402088), where regress keeps a norm-pivoted basis (`o.x z _cons`).
// The same rule holds for factor sets (`d c.x##i.t` with d == 2.t: `2o.t`;
// `c.x##i.t d`: `o.d`) and is what package 007's
// `reghdfe attrit ... owns_car_BL##treatment ib1.cars_num##treatment ...` needs
// (Stata omits 0.cars_num and keeps 1.treatment). The refits (lincom/test/predict)
// replay the same rule, so they answer for the coefficients e(b) posted.
#include "selftest_w2.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>

#include "piet/dispatcher.h"
#include "piet/eval/errors.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

namespace {

std::string shown(piet::SessionState& session, const std::string& cmd) {
  std::string text;
  for (const auto& b : piet::dispatch(cmd, session)) {
    if (b.is_object() && b.value("type", std::string()) == "display")
      text += b.value("text", std::string());
  }
  return text;
}

std::string rcOf(piet::SessionState& session, const std::string& cmd) {
  std::string rc = "0";
  for (const auto& b : piet::dispatch(cmd, session)) {
    if (b.is_object() && b.value("type", std::string()) == "error") rc = piet::eval::coarseRc(b);
  }
  return rc;
}

double num(piet::SessionState& session, const std::string& expr) {
  const std::string s = shown(session, "display %21.15g " + expr);
  try {
    return std::stod(s);
  } catch (...) {
    return std::nan("");
  }
}

bool near(double got, double want, double rtol = 1e-9) {
  return std::isfinite(got) && std::fabs(got - want) <= rtol * std::max(1.0, std::fabs(want));
}

std::string ebCols(piet::SessionState& session) {
  (void)piet::dispatch("matrix C1067 = e(b)", session);
  return shown(session, "display \"`: colnames C1067'\"");
}

void setup(piet::SessionState& session) {
  (void)piet::dispatch("clear", session);
  (void)piet::dispatch("set obs 60", session);
  (void)piet::dispatch("generate id = ceil(_n/6)", session);
  (void)piet::dispatch("generate g = mod(_n,5)", session);
  (void)piet::dispatch("generate x = mod(_n*7,13)", session);
  (void)piet::dispatch("generate z = 2*x", session);
  (void)piet::dispatch("generate y = 1 + 0.5*x + mod(_n*3,7)", session);
  (void)piet::dispatch("generate t = mod(_n,3)", session);
  (void)piet::dispatch("generate d = (t==2)", session);
}

}  // namespace

void testW2C1067() {
  std::printf("[selftest] --- BUG-1067 reghdfe/areg/xtreg,fe omit the later collinear term ---\n");
  namespace fs = std::filesystem;
  const std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_c1067").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  {
    piet::SessionState session(workdir);
    setup(session);
    // StataBE m1.do A_reghdfe_fe / A_areg / A_xtreg: `x o.z _cons`, identical b/se.
    (void)piet::dispatch("xtset g", session);
    for (const char* fit : {"reghdfe y x z, absorb(g)", "areg y x z, absorb(g)", "xtreg y x z, fe"}) {
      const std::string tag = std::string("BUG-1067 [") + fit + "]: ";
      const std::string rc = rcOf(session, std::string("quietly ") + fit);
      w2check(rc == "0", tag + "the fit is rc 0, got " + rc);
      const std::string cols = ebCols(session);
      w2check(cols == "x o.z _cons",
              tag + "e(b) is `x o.z _cons` (StataBE m1.do: the later member is omitted). Got `" +
                  cols + "`");
      w2check(near(num(session, "_b[x]"), .527368857557537) &&
                  near(num(session, "_se[x]"), .0733801298402088),
              tag + "_b[x] .527368857557537 _se[x] .0733801298402088 (StataBE). Got " +
                  shown(session, "display %21.15g _b[x] \" \" %21.15g _se[x]"));
      w2check(num(session, "_b[z]") == 0.0, tag + "_b[z] is 0 (omitted)");
      // The refits replay the fit's omission: lincom x is _b[x], test x is t^2.
      (void)piet::dispatch("quietly lincom x", session);
      w2check(near(num(session, "r(estimate)"), .527368857557537) &&
                  near(num(session, "r(se)"), .0733801298402088),
              tag + "lincom x answers _b[x] and _se[x] (the refit omits z too). Got " +
                  shown(session, "display %21.15g r(estimate) \" \" %21.15g r(se)"));
      (void)piet::dispatch("quietly test x", session);
      const double t = .527368857557537 / .0733801298402088;
      w2check(near(num(session, "r(F)"), t * t, 1e-8),
              tag + "test x is F = t^2 on the kept x. Got " + shown(session, "display %21.15g r(F)"));
    }
    // reghdfe without absorb() keeps the same rule (StataBE A_reghdfe_noabs `x o.z _cons`).
    (void)piet::dispatch("quietly reghdfe y x z", session);
    w2check(ebCols(session) == "x o.z _cons",
            "BUG-1067: reghdfe without absorb() is `x o.z _cons` (StataBE m1.do). Got `" +
                ebCols(session) + "`");
    // regress keeps its norm-pivoted basis (StataBE A_regress `o.x z _cons`).
    (void)piet::dispatch("quietly regress y x z", session);
    w2check(ebCols(session) == "o.x z _cons",
            "BUG-1067 control: regress stays `o.x z _cons` (StataBE m1.do). Got `" +
                ebCols(session) + "`");
    // Factor sets: d == 2.t. The earlier of the two is kept, whichever it is
    // (StataBE E_reghdfe_d_first / E_reghdfe_d_last). Read through _b[]: the e(b)
    // stripe of this collinear `c.x##i.t` form has no StataBE capture.
    (void)piet::dispatch("quietly reghdfe y d c.x##i.t, absorb(g)", session);
    w2check(near(num(session, "_b[d]"), -.606974414215032, 1e-8) && num(session, "_b[2.t]") == 0.0,
            "BUG-1067: `reghdfe y d c.x##i.t` keeps d (_b[d] -.606974414215032, StataBE) and "
            "omits 2.t. Got _b[d] _b[2.t] " +
                shown(session, "display %21.15g _b[d] \" \" %21.15g _b[2.t]"));
    (void)piet::dispatch("quietly reghdfe y c.x##i.t d, absorb(g)", session);
    w2check(near(num(session, "_b[2.t]"), -.606974414215035, 1e-8) && num(session, "_b[d]") == 0.0,
            "BUG-1067: `reghdfe y c.x##i.t d` keeps 2.t (_b[2.t] -.606974414215035, StataBE) and "
            "omits d. Got _b[2.t] _b[d] " +
                shown(session, "display %21.15g _b[2.t] \" \" %21.15g _b[d]"));
    // The residuals() refit and predict keep the fit's rule (rc 0, same within resid).
    (void)piet::dispatch("quietly reghdfe y x z, absorb(g) residuals(r1067)", session);
    (void)piet::dispatch("quietly predict double xb1067, xb", session);
    w2check(rcOf(session, "summarize r1067 xb1067") == "0",
            "BUG-1067: reghdfe residuals() and predict after a collinear fit are rc 0");
  }
  fs::remove_all(workdir, ec);
}
