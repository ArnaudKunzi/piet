// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Remaining-150 WS-F (ivreg2 surface): ivreg2 ffirst/savefirst/savefprefix and
// e(first)/e(widstat), ivreg2 [aweight=], the ungrouped `statsby _b _se, clear`
// prefix and the `: sortedby` claim after `merge 1:1`. Oracle: StataBE 19.5
// tests/eval/golden/aer_w2/w2d_uncovered.{do,log}; the log line of each record is
// cited on its arm. The panel is w2d_mkpanel verbatim (w2d_uncovered.do:27-48).
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

// A number read back through `display %18.0g EXPR`; `.` (or no output) is NaN.
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
    "    generate z1 = mod(_n*_n*8806 + _n*1919, 10007)/10007 + id/20\n"
    "    generate z2 = mod(_n*_n*5034 + _n*2323, 10007)/10007\n"
    "    generate x1 = z1 + 0.5*z2 + mod(_n*_n*142 + _n*2929, 10007)/10007\n"
    "    generate x2 = mod(_n*_n*4729 + _n*3131, 10007)/10007\n"
    "    generate y = 1 + 2*x1 - x2 + 0.3*g + id/10 + mod(_n*_n*2277 + _n*4343, 10007)/10007\n"
    "    generate wt = 1 + mod(_n,4)\n"
    "end\n";

}  // namespace

void testR150Ivreg2Surface() {
  std::printf("[selftest] --- R150 WS-F: ivreg2 ffirst/savefirst/aweight, statsby, merge sortedby ---\n");
  namespace fs = std::filesystem;
  const std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_r150_ivreg2").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  piet::SessionState session(workdir);
  runSrc(session, kMkPanel);

  // ivreg2_ffirst_ctl (log:1687): no ffirst, so no e(first); e(widstat) is the KP
  // rk Wald F under cluster(g).
  runSrc(session, "w2d_mkpanel\nquietly ivreg2 y (x1 = z1 z2), cluster(g)\n");
  const double b057 = num(session, "_b[x1]");
  const double w057 = num(session, "e(widstat)");
  w2check(near(b057, 1.858786917670349, 1e-12) && near(w057, 128.5633117654573, 1e-10),
          "ivreg2_ffirst_ctl: _b[x1] 1.858786917670349, e(widstat) 128.5633117654573 (got " +
              fmt(b057) + ", " + fmt(w057) + ")");
  w2check(std::isnan(num(session, "rowsof(e(first))")), "ivreg2_ffirst_ctl: no e(first) without ffirst");

  // ivreg2_cluster_ffirst_057 (log:1623): e(first) is 21 x 1, its F the same widstat.
  runSrc(session, "w2d_mkpanel\nquietly ivreg2 y (x1 = z1 z2), cluster(g) ffirst\n");
  w2check(num(session, "rowsof(e(first))") == 21 && num(session, "colsof(e(first))") == 1,
          "ivreg2_cluster_ffirst_057: e(first) is 21 x 1");
  const double f057 = num(session, "el(e(first), 4, 1)");
  w2check(near(f057, 128.5633117654573, 1e-10),
          "ivreg2_cluster_ffirst_057: e(first)[F] = widstat with one endogenous regressor (got " +
              fmt(f057) + ")");

  // ivreg2_ffirst_savefirst_068 (log:1506): the first stage is stored as first1x1.
  runSrc(session,
         "w2d_mkpanel\n"
         "quietly ivreg2 y (x1 = z1) i.g i.t, ffirst savefirst savefprefix(first1) cluster(g)\n");
  const double b068 = num(session, "_b[x1]"), s068 = num(session, "_se[x1]");
  w2check(near(b068, 2.227224804675559, 1e-12) && near(s068, .2475625881781143, 1e-10),
          "ivreg2_ffirst_savefirst_068: the parent fit is unchanged (got " + fmt(b068) + ", " +
              fmt(s068) + ")");
  w2check(str(session, "`e(firsteqs)'") == "first1x1" && str(session, "`e(cmd)'") == "ivreg2",
          "ivreg2_ffirst_savefirst_068: e(firsteqs) first1x1 on the parent's e()");
  runSrc(session, "capture estimates restore first1x1\nlocal rr = _rc\n");
  w2check(session.eval.locals.get("rr", "?") == "0" && str(session, "`e(model)'") == "first" &&
              str(session, "`e(depvar)'") == "x1",
          "ivreg2_ffirst_savefirst_068: estimates restore first1x1 is rc 0 and the first stage");

  // ivreg2_aw_ctl (log:1916) and ivreg2_cluster2_aw_030 (log:1866).
  runSrc(session, "w2d_mkpanel\nquietly ivreg2 y (x1 = z1) [aweight = wt]\n");
  const double baw = num(session, "_b[x1]"), saw = num(session, "_se[x1]");
  w2check(near(baw, 1.990360423018126, 1e-12) && near(saw, .3304838363418316, 1e-10),
          "ivreg2_aw_ctl: weighted 2SLS b and iid se (got " + fmt(baw) + ", " + fmt(saw) + ")");
  runSrc(session, "w2d_mkpanel\nquietly ivreg2 y (x1 = z1) [aweight = wt], cluster(g t)\n");
  w2check(std::isnan(num(session, "_se[x1]")) && num(session, "e(N_clust)") == 5 &&
              num(session, "e(N_clust1)") == 5 && num(session, "e(N_clust2)") == 6,
          "ivreg2_cluster2_aw_030: unrepaired two-way variance -> _se missing; N_clust 5|5|6");

  // xtreg_statsby_fe_028 (log:3422): one float observation, _b_cons not _b__cons.
  runSrc(session,
         "w2d_mkpanel\nquietly xtset id t\ncapture statsby _b _se, clear: xtreg y x1 if c == 1, fe\n"
         "local sr = _rc\n");
  w2check(session.eval.locals.get("sr", "?") == "0" && num(session, "_N") == 1 &&
              num(session, "c(k)") == 4,
          "xtreg_statsby_fe_028: rc 0, one observation, four variables");
  w2check(num(session, "_b_x1[1]") == 1.981762409210205 && num(session, "_se_x1[1]") == .3451376259326935,
          "xtreg_statsby_fe_028: float(_b[x1]) and float(_se[x1])");
  runSrc(session, "capture confirm variable _b_cons\nlocal cn = _rc\n");
  w2check(session.eval.locals.get("cn", "?") == "0", "statsby names the constant _b_cons");

  // merge_sorted_ctl (log:3988): 1:1 with no using-only row claims the key order;
  // using-only rows leave the claim empty (matches Stata's merge, identified from its ado).
  const std::string ms = workdir + "/r150_ms.dta";
  runSrc(session,
         "clear\nset obs 5\ngenerate id = _n\ngenerate a = 7 - _n\nsave \"" + ms + "\", replace\n"
         "clear\nset obs 5\ngenerate id = _n\nmerge 1:1 id using \"" + ms + "\", nogen assert(3)\n"
         "local s1 : sortedby\n"
         "clear\nset obs 3\ngenerate id = _n\nmerge 1:1 id using \"" + ms + "\", nogen\n"
         "local s2 : sortedby\n");
  w2check(session.eval.locals.get("s1", "?") == "id" && session.eval.locals.get("s2", "?").empty(),
          "merge 1:1: `: sortedby' is the key without using-only rows, empty with them (got [" +
              session.eval.locals.get("s1", "?") + "] [" + session.eval.locals.get("s2", "?") + "])");
  fs::remove_all(workdir, ec);
}
