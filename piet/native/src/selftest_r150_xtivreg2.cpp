// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Remaining-150 WS-F2 (xtivreg2, fe): the within transformation with weighted group
// means, ivreg2's dofminus(N_g) large-sample convention, ffirst / first / savefirst /
// savefprefix, and the fe-or-fd and panel-in-cluster refusals. Method: matches xtivreg2
// 1.0.17 (19Feb2015) and ivreg2 4.1.12 (identified from their ado). Oracle: StataBE 19.5 with xtivreg2 1.0.17,
// tests/eval/golden/aer_w2/w2d_uncovered_xtivreg2.{do,log} (log line cited per arm);
// the other numbers are a StataBE probe of the same panel (impl_F2/stata_probe,
// printed in %21x). The panel is w2d_mkpanel verbatim (w2d_uncovered_xtivreg2.do:27-49).
#include "selftest_w2.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <string>

#include "piet/dispatcher.h"
#include "piet/doexec.h"
#include "piet/eval/errors.h"
#include "piet/session_state.h"

namespace {

const double kNaN = std::numeric_limits<double>::quiet_NaN();

void runSrc(piet::SessionState& session, const std::string& src) {
  (void)piet::doexec::executeDoSource(src, session, piet::dispatch);
}

double num(piet::SessionState& session, const std::string& expr) {
  std::string text;
  for (const auto& b : piet::dispatch("display %18.0g " + expr, session)) {
    if (b.is_object() && b.value("type", std::string()) == "display")
      text = b.value("text", std::string());
  }
  const size_t a = text.find_first_not_of(' ');
  if (a == std::string::npos || text.substr(a) == ".") return kNaN;
  char* end = nullptr;
  const double v = std::strtod(text.c_str() + a, &end);
  return end == text.c_str() + a ? kNaN : v;
}

std::string str(piet::SessionState& session, const std::string& expr) {
  std::string text;
  for (const auto& b : piet::dispatch("display \"" + expr + "\"", session)) {
    if (b.is_object() && b.value("type", std::string()) == "display")
      text = b.value("text", std::string());
  }
  return text;
}

bool near(double got, double want, double rtol) {
  return std::fabs(got - want) <= rtol * std::fabs(want);
}

std::string fmt(double v) {
  if (std::isnan(v)) return ".";
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.17g", v);
  return buf;
}

const char* kMkPanel =
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
    "    generate z1 = mod(_n*_n*8806 + _n*1919, 10007)/10007 + id/20\n"
    "    generate z2 = mod(_n*_n*5034 + _n*2323, 10007)/10007\n"
    "    generate x1 = z1 + 0.5*z2 + mod(_n*_n*142 + _n*2929, 10007)/10007\n"
    "    generate x2 = mod(_n*_n*4729 + _n*3131, 10007)/10007\n"
    "    generate y = 1 + 2*x1 - x2 + 0.3*g + id/10 + mod(_n*_n*2277 + _n*4343, 10007)/10007\n"
    "    generate wt = 1 + mod(_n,4)\n"
    "    quietly xtset id t\n"
    "end\n";

const char* kModel = "xtivreg2 y (x1 = z1 z2) x2 if c < 3";

}  // namespace

void testR150Xtivreg2Fe() {
  std::printf("[selftest] --- R150 WS-F2: xtivreg2, fe (dofminus, weights, ffirst/savefirst) ---\n");
  namespace fs = std::filesystem;
  const std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_r150_xtivreg2").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  piet::SessionState session(workdir);
  runSrc(session, kMkPanel);

  // xtivreg2_fe_cluster_ffirst_044 (log:2687): cluster meat with no dofminus factor and
  // no small-sample factor; e(first) 21 x 1 with the cluster F 20.06 (log:2587).
  runSrc(session, "w2d_mkpanel\ncapture " + std::string(kModel) +
                      ", fe cluster(id) ffirst\nlocal r1 = _rc\n");
  const double b044 = num(session, "_b[x1]"), s044 = num(session, "_se[x1]");
  w2check(session.eval.locals.get("r1", "?") == "0" && near(b044, 1.90729299403388, 1e-12) &&
              near(s044, .2009336090446331, 1e-10),
          "xtivreg2_fe_cluster_ffirst_044: rc 0, b 1.90729299403388, se .2009336090446331 (got " +
              fmt(b044) + ", " + fmt(s044) + ")");
  w2check(str(session, "`e(cmd)'") == "xtivreg2" && num(session, "e(N_g)") == 10 &&
              num(session, "e(df_m)") == 2 && num(session, "e(df_b)") == 2 &&
              num(session, "e(N_clust)") == 10,
          "044: e(cmd) xtivreg2, N_g 10, df_m = df_b = 2 under cluster, N_clust 10");
  w2check(num(session, "rowsof(e(first))") == 21 &&
              near(num(session, "el(e(first), 4, 1)"), 20.064619628667053, 1e-10) &&
              num(session, "el(e(first), 6, 1)") == 9 &&
              near(num(session, "el(e(first), 12, 1)"), 46.998208139220125, 1e-10) &&
              near(num(session, "el(e(first), 1, 1)"), .27151362504875171, 1e-10),
          "044: e(first) F 20.0646 df_r 9 SWchi2 46.998 rmse .271514 (probe fe_cl_ffirst)");
  w2check(near(num(session, "e(rmse)"), .26599301158375871, 1e-12),
          "044: e(rmse) = sqrt(rss/(N-N_g)) .265993");

  // xtivreg2_fe_pw_savefirst_034 (log:2853): weighted within transform, robust meat
  // * N/(N-N_g); the first stage stored as st1x1 with e(cmd) xtivreg2 (log:2714).
  runSrc(session,
         "w2d_mkpanel\ncapture xtivreg2 y x2 w1 (x1 = z1) [pweight = wt] if c < 3, fe first "
         "savefirst savefprefix(st1)\nlocal r2 = _rc\n");
  const double b034 = num(session, "_b[x1]"), s034 = num(session, "_se[x1]");
  w2check(session.eval.locals.get("r2", "?") == "0" && near(b034, 1.873391704724933, 1e-12) &&
              near(s034, .1495929119342674, 1e-10),
          "xtivreg2_fe_pw_savefirst_034: rc 0, b 1.873391704724933, se .1495929119342674 (got " +
              fmt(b034) + ", " + fmt(s034) + ")");
  w2check(str(session, "`e(firsteqs)'") == "st1x1" && num(session, "e(df_m)") == 13 &&
              near(num(session, "el(e(first), 4, 1)"), 22.760213092434295, 1e-10) &&
              num(session, "el(e(first), 6, 1)") == 27,
          "034: e(firsteqs) st1x1, df_m 13, e(first) F 22.7602 on df_r 27 (log:2742-2743)");
  runSrc(session, "capture estimates restore st1x1\nlocal r3 = _rc\n");
  w2check(session.eval.locals.get("r3", "?") == "0" && str(session, "`e(cmd)'") == "xtivreg2" &&
              num(session, "e(df_r)") == 27 && num(session, "e(df_m)") == 13 &&
              near(num(session, "_b[z1]"), 1.1756425227031322, 1e-10) &&
              near(num(session, "_se[z1]"), .2464263, 1e-6) &&
              near(num(session, "e(rmse)"), .30340405917671581, 1e-10),
          "034: st1x1 restores as the stage (cmd xtivreg2, df_r 27, z1 1.175643 / .2464263)");

  // The dofminus convention on every vce, with and without small (probe, %21x).
  struct Arm {
    const char* opts;
    double se;
  };
  const Arm arms[] = {{", fe", 0.15580589449820823},
                      {", fe robust small", 0.1883727223331845},
                      {", fe cluster(id) small", 0.2145714002476122},
                      {", fe cluster(g)", 0.21947091936835975}};
  for (const auto& a : arms) {
    runSrc(session, "w2d_mkpanel\nquietly " + std::string(kModel) + a.opts + "\n");
    const double se = num(session, "_se[x1]");
    w2check(near(se, a.se, 1e-10),
            std::string("xtivreg2") + a.opts + ": se " + fmt(a.se) + " (got " + fmt(se) + ")");
  }
  runSrc(session, "w2d_mkpanel\nquietly xtivreg2 y (x1 = z1 z2) x2 [aw=wt] if c < 3, fe\n");
  w2check(near(num(session, "_b[x1]"), 2.0058616257909008, 1e-12) &&
              near(num(session, "_se[x1]"), 0.15058838315211964, 1e-10) &&
              num(session, "e(df_m)") == 12,
          "xtivreg2 [aw], fe: weighted within b 2.005862, iid se .150588, df_m 12");

  // xtivreg2_nofe_ctl (log:2866-2878) and a panel spanning clusters (probe SPAN).
  runSrc(session, "w2d_mkpanel\ncapture " + std::string(kModel) +
                      ", cluster(id) ffirst\nlocal r4 = _rc\n"
                      "capture " + std::string(kModel) + ", fe cluster(t)\nlocal r5 = _rc\n");
  w2check(session.eval.locals.get("r4", "?") == "198" && session.eval.locals.get("r5", "?") == "198",
          "xtivreg2 without fe/fd is r(198); fe with a cluster a panel spans is r(198) (got " +
              session.eval.locals.get("r4", "?") + ", " + session.eval.locals.get("r5", "?") + ")");
  fs::remove_all(workdir, ec);
}
