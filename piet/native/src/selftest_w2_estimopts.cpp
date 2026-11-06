// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// W2 lane `estimopts` (2026-09-19) -- estimation-option defects, PIET side.
// Oracles: tests/eval/golden/aer_w2/w2_pweight.log (arms R*, X*) and
// w2_smalls.log section H (arms reghdfe_*). Every arm below is a
// DISCRIMINATOR: the comment on each says what the pre-fix main answered.
// Numbers asserted here are the oracle's own %21x values decoded to double
// (python float.fromhex), on the oracle's own synthetic fixtures; nothing is
// read through a 9-digit `display`. JSON access goes through is_object()/
// contains()/try-catch so a wrong shape is a FAIL, never an abort.
#include "selftest_w2.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "piet/dispatcher.h"
#include "piet/eval/errors.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

namespace {

using ojson = nlohmann::ordered_json;

// Everything one dispatch produced, flattened for assertion.
struct Reply {
  std::string rc = "0";
  std::string msg;
  std::string text;  // every display block's text, concatenated
  ojson est;         // the last estimation block (null when none)
  ojson stats;       // the last summarize block's `stats` (null when none)
  bool sawMutation = false;
  std::vector<std::string> newVars;
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
      } else if (t == "summarize") {
        if (b.contains("stats")) r.stats = b["stats"];
      } else if (t == "data_mutation") {
        r.sawMutation = true;
        if (b.contains("new_vars") && b["new_vars"].is_array()) {
          for (const auto& v : b["new_vars"]) {
            if (v.is_string()) r.newVars.push_back(v.get<std::string>());
          }
        }
      }
    }
  } catch (const std::exception& e) {
    r.rc = "EXC";
    r.msg = e.what();
  }
  return r;
}

const double kNaN = std::nan("");

// coeftable row `name`, field `key` ("b"/"se"); NaN when absent or mis-shaped.
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

double numOf(const ojson& obj, const char* key) {
  try {
    if (obj.is_object() && obj.contains(key) && obj[key].is_number()) return obj[key].get<double>();
  } catch (...) {
  }
  return kNaN;
}

std::string strOf(const ojson& obj, const char* key) {
  try {
    if (obj.is_object() && obj.contains(key) && obj[key].is_string()) return obj[key].get<std::string>();
  } catch (...) {
  }
  return std::string();
}

bool close(double a, double b, double rtol) {
  return std::isfinite(a) && std::isfinite(b) && std::fabs(a - b) <= rtol * std::fabs(b);
}

std::string fmt(double v) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.17g", v);
  return buf;
}

// A session value read through PIET's own evaluator (`display %21x EXPR`), so an
// identity arm compares against what the session posts, never a literal. The
// text is Stata's hex spelling ("+1.5555560000000X-002"); a missing value
// (exponent +3ff, system or extended) reads as NaN.
double sessionValue(piet::SessionState& session, const std::string& expr) {
  Reply r = run(session, "display %21x " + expr);
  const std::string h = r.text.substr(0, r.text.find_last_not_of(" \n") + 1);
  const size_t x = h.find('X');
  if (h.size() < 6 || x == std::string::npos || x + 2 > h.size() ||
      (h[0] != '+' && h[0] != '-'))
    return kNaN;
  const std::string expo = h.substr(x + 1);
  if (expo == "+3ff") return kNaN;
  char* end = nullptr;
  const long e = std::strtol(expo.c_str(), &end, 16);
  if (end == expo.c_str()) return kNaN;
  const std::string c =
      std::string(h[0] == '-' ? "-" : "") + "0x" + h.substr(1, x - 1) + "p" + std::to_string(e);
  const double v = std::strtod(c.c_str(), &end);
  return end == c.c_str() ? kNaN : v;
}

double rOf(const piet::SessionState& session, const char* key) {
  const double* v = session.eval.rValues.find(key);
  return v ? *v : kNaN;
}

// w2_pweight.do's fixture, line for line (15 panels x 8 rows, heteroskedastic
// u so robust != iid, w constant within id so weighted != unweighted).
const std::vector<std::string>& pweightFixture() {
  static const std::vector<std::string> k = {
      "clear",
      "set obs 120",
      "generate id  = ceil(_n/8)",
      "generate t   = mod(_n-1,8)+1",
      "generate cl2 = ceil(id/3)",
      "generate double x1 = mod(_n*7,11)/3 + id/5",
      "generate double x2 = mod(_n*13,17)/4 - t/3",
      "generate double u  = (mod(_n*37,23)-11)/6 * (1 + x1/3)",
      "generate double y  = 1 + 2*x1 - x2 + id/4 + u",
      "generate double w  = 1 + mod(id*3,5)",
      "generate double wv = 1 + mod(_n*3,5)",
      "xtset id",
  };
  return k;
}

// w2_smalls.do's frame as section H sees it: x[5]/x[11] were set missing and
// then to 3/8 AFTER y was generated from the original x, so y keeps the
// original values and only x carries the two edits.
const std::vector<std::string>& smallsFixture() {
  static const std::vector<std::string> k = {
      "clear",
      "set obs 12",
      "generate id = ceil(_n/3)",
      "generate t  = mod(_n-1,3)+1",
      "generate x  = mod(_n*7,11)",
      "generate x2 = mod(_n*3,5)",
      "generate y  = 2*x - x2 + mod(_n,4)",
      "replace x = 3 in 5",
      "replace x = 8 in 11",
  };
  return k;
}

void seed(piet::SessionState& session, const std::vector<std::string>& fixture) {
  for (const auto& s : fixture) (void)piet::dispatch(s, session);
}

const char* kEStrings =
    "display \"`e(vce)'|`e(vcetype)'|`e(clustvar)'|`e(wtype)'|`e(wexp)'\"";

// Oracle values, w2_pweight.log PVAL/PSTR records (%21x decoded).
constexpr double kR1SeX1 = 0.17632268398268275;  // R1 [aweight=w], iid  (= what main gave pw)
constexpr double kR2SeX1 = 0.20471596163438208;  // R2 [pweight=w] == R3 [aweight=w], vce(robust)
constexpr double kR2BX1 = 2.381531911602316;     // b_x1 is the same under aw and pw
constexpr double kR4SeX1 = 0.1871199473907192;   // R4 vce(cluster id) == R5 robust cluster(id)
constexpr double kX1SeX1 = 0.26137121217032483;  // X1 fe vce(robust) == X2 fe vce(cluster id)
constexpr double kX0SeX1 = 0.2195482082339527;   // X0 fe, conventional (control)
constexpr double kA0BX1 = 2.018755570377941;     // A0 areg unweighted, vce(cluster id) -- b_x1
// BUG-568 (areg's clustered dof), w2_pweight.log PVAL records.
constexpr double kA0SeX1 = 0x1.1d41125f9cb99p-2;    // A0 areg, vce(cluster id)
constexpr double kA0SeCons = 0x1.c749b407eba18p-1;
constexpr double kA1SeX1 = 0x1.e178af045346fp-3;    // A1 areg [pw=w] (robust)
constexpr double kA2SeX1 = 0x1.cfb6746651353p-3;    // A2 areg [pw=w], vce(cluster id)
constexpr double kA2SeCons = 0x1.61f6af6a6958dp-1;
constexpr double kA3BX1 = 0x1.e84a361e3023bp+0;     // A3 areg [pw=wv], vce(cluster id)
constexpr double kA3SeX1 = 0x1.31cf90b884108p-2;
constexpr double kA3SeCons = 0x1.ed54b6d900724p-1;
constexpr double kH1SeX1 = 0x1.b315e83b84cedp-3;    // H1 reghdfe [pw=w], vce(cluster id)
// FU-385 margins, at(x1=1) se after `regress y x1 x2, vce(cluster id id2)` on the
// w2l_vce_shape_det frame: the engine's lincom _cons + x1 + mean(x2)*x2 under the
// fit's V (test_cli_prewave.cpp test_postest_refit_cli asserts the equality).
constexpr double kMwMarginSe = 0.51048332339851565;

}  // namespace

void testW2EstimOpts() {
  std::printf("[selftest] --- W2 estimopts: pweight implies robust, cluster wins, xtreg fe robust, reghdfe residuals/noabsorb ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_w2estimopts").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  // ── BUG-536: `regress ... [pweight=w]` with no vce ──────────────────────
  {
    piet::SessionState s(workdir);
    seed(s, pweightFixture());
    Reply aw = run(s, "regress y x1 x2 [aweight=w]");
    Reply pw = run(s, "regress y x1 x2 [pweight=w]");
    Reply pwStr = run(s, kEStrings);
    Reply awR = run(s, "regress y x1 x2 [aweight=w], vce(robust)");
    w2check(aw.rc == "0" && close(coefOf(aw.est, "x1", "se"), kR1SeX1, 1e-9),
            "w2_pweight R1_aw_control: `[aweight=w]` stays iid, se_x1 +1.691bde09fd0feX-003. Got rc " +
                aw.rc + " se=" + fmt(coefOf(aw.est, "x1", "se")));
    w2check(pw.rc == "0" && close(coefOf(pw.est, "x1", "se"), kR2SeX1, 1e-9),
            "w2_pweight R2_pw: `[pweight=w]` with no vce() is ROBUST -- se_x1 +1.a3421f4182c9fX-003, "
            "the R3 (`[aweight=w], vce(robust)`) number. Main answered the R1 iid number (" +
                fmt(kR1SeX1) + ") at rc 0: a silent wrong SE. Got rc " + pw.rc +
                " se=" + fmt(coefOf(pw.est, "x1", "se")) + " " + pw.msg);
    w2check(close(coefOf(pw.est, "x1", "se"), coefOf(awR.est, "x1", "se"), 1e-12),
            "w2_pweight R3_aw_robust_equals_R2: the pw SE and the aw+robust SE are the SAME "
            "double, not merely both near the oracle. Got pw=" + fmt(coefOf(pw.est, "x1", "se")) +
                " aw_robust=" + fmt(coefOf(awR.est, "x1", "se")));
    w2check(close(coefOf(pw.est, "x1", "b"), kR2BX1, 1e-9) &&
                close(coefOf(aw.est, "x1", "b"), kR2BX1, 1e-9),
            "w2_pweight R1/R2: the coefficient is untouched by the VCE change (b_x1 "
            "+1.30d609a55b2d0X+001 under both weights). Got pw=" + fmt(coefOf(pw.est, "x1", "b")));
    w2check(strOf(pw.est, "vcov_type") == "HC1",
            "w2_pweight R2_pw: the estimation block's vcov_type says HC1 (main: iid). Got `" +
                strOf(pw.est, "vcov_type") + "`");
    w2check(pwStr.text == "robust|Robust||pweight|= w",
            "w2_pweight R2_pw PSTR: e(vce)|e(vcetype)|e(clustvar)|e(wtype)|e(wexp) read "
            "`robust|Robust||pweight|= w`. Main: `iid||||` -- e(vce) in the engine's spelling, "
            "and no producer for wtype/wexp anywhere. Got `" + pwStr.text + "`");
    Reply awStr = run(s, "regress y x1 x2 [aweight=w]");
    (void)awStr;
    // e(vce) is `ols` after a default-VCE regress, weighted or not (w2_pweight
    // R1_aw_control PSTR `120|117|.|ols|||aweight|= w|`). Main: `iid`, the engine's
    // spelling.
    Reply awStrT = run(s, "display \"`e(vcetype)'|`e(clustvar)'|`e(wtype)'|`e(wexp)'\"");
    Reply awVce = run(s, "display \"`e(vce)'\"");
    w2check(awVce.text == "ols",
            "w2_pweight R1_aw_control PSTR: e(vce) `ols` after `regress [aweight=w]` (main: "
            "`iid`). Got `" + awVce.text + "`");
    w2check(awStrT.text == "||aweight|= w",
            "w2_pweight R1 PSTR: after a plain `[aweight=w]` fit e(wtype)=aweight, e(wexp)=`= w`, "
            "and the robust-only macros are ERASED, not inherited from the previous fit. Got `" +
                awStrT.text + "`");
  }

  // ── BUG-536, the shared M4 builder: logit under a bare pweight ──────────
  // R2's rule (pweight implies robust) through buildEstimationArgvM4. No
  // captured logit arm: the assertion is an IDENTITY with this build's own
  // explicit `vce(robust)` fit. Main refused the bare form at rc 1 ("logit:
  // [pweight=] requires vce(robust)"), where Stata runs it robust at rc 0.
  {
    piet::SessionState s(workdir);
    seed(s, pweightFixture());
    (void)run(s, "generate byte yb = y > 6");
    Reply lr = run(s, "logit yb x1 x2 [pweight=w], vce(robust)");
    Reply lb = run(s, "logit yb x1 x2 [pweight=w]");
    Reply lbStr = run(s, kEStrings);
    w2check(lb.rc == "0" && lr.rc == "0" &&
                close(coefOf(lb.est, "x1", "se"), coefOf(lr.est, "x1", "se"), 1e-12),
            "w2_pweight R2 (rule, logit): bare `logit ... [pweight=w]` runs robust -- same SE as "
            "`vce(robust)`. Main: rc 1 refusal. Got rc " + lb.rc + " se=" +
                fmt(coefOf(lb.est, "x1", "se")) + " vs " + fmt(coefOf(lr.est, "x1", "se")) + " " +
                lb.msg);
    w2check(lbStr.text == "robust|Robust||pweight|= w",
            "w2_pweight R2 PSTR (rule, logit): `robust|Robust||pweight|= w`. Got `" + lbStr.text +
                "`");
  }

  // ── BUG-537: `robust cluster(id)` typed together -- the cluster wins ────
  {
    piet::SessionState s(workdir);
    seed(s, pweightFixture());
    Reply r4 = run(s, "regress y x1 x2 [pweight=w], vce(cluster id)");
    Reply r4Str = run(s, kEStrings);
    Reply r5 = run(s, "regress y x1 x2 [pweight=w], robust cluster(id)");
    Reply r5Str = run(s, kEStrings);
    w2check(r4.rc == "0" && close(coefOf(r4.est, "x1", "se"), kR4SeX1, 1e-9) &&
                numOf(r4.est, "n_clusters") == 15.0,
            "w2_pweight R4_pw_cluster (control): `vce(cluster id)` is cluster-robust, N_clust 15, "
            "se_x1 +1.7f38be33c76eeX-003. Got rc " + r4.rc + " se=" + fmt(coefOf(r4.est, "x1", "se")) +
                " n_clusters=" + fmt(numOf(r4.est, "n_clusters")));
    w2check(r5.rc == "0" && close(coefOf(r5.est, "x1", "se"), kR4SeX1, 1e-9) &&
                numOf(r5.est, "n_clusters") == 15.0,
            "w2_pweight R5_pw_robust_AND_cluster_equals_R4: `robust cluster(id)` is byte-identical "
            "to R4 -- the cluster wins. Main tested `robust` first and IGNORED the cluster: HC1, "
            "N_clust 0, se " + fmt(kR2SeX1) + ". Got rc " + r5.rc + " se=" +
                fmt(coefOf(r5.est, "x1", "se")) + " n_clusters=" + fmt(numOf(r5.est, "n_clusters")) +
                " vcov_type=" + strOf(r5.est, "vcov_type"));
    w2check(r5Str.text == "cluster|Robust|id|pweight|= w" && r4Str.text == r5Str.text,
            "w2_pweight R5 PSTR == R4 PSTR: `cluster|Robust|id|pweight|= w`. Got R5 `" + r5Str.text +
                "` R4 `" + r4Str.text + "`");
    // The unweighted twin of the same parser path, and the abbreviated
    // spellings (`r`, `cl()`) the corpus actually types. No separate oracle
    // arm: R5's rule is the whole claim, and the assertion is an IDENTITY with
    // this build's own `vce(cluster id)` fit, not a number.
    Reply cl = run(s, "regress y x1 x2, vce(cluster id)");
    Reply rcl = run(s, "regress y x1 x2, r cl(id)");
    w2check(rcl.rc == "0" && numOf(rcl.est, "n_clusters") == 15.0 &&
                close(coefOf(rcl.est, "x1", "se"), coefOf(cl.est, "x1", "se"), 1e-12),
            "w2_pweight R5 (rule, unweighted `r cl(id)`): same SE as `vce(cluster id)`, 15 "
            "clusters. Main: HC1 with the cluster dropped. Got rc " + rcl.rc + " n_clusters=" +
                fmt(numOf(rcl.est, "n_clusters")) + " se=" + fmt(coefOf(rcl.est, "x1", "se")) +
                " vs " + fmt(coefOf(cl.est, "x1", "se")));
  }

  // ── BUG-538: `xtreg, fe vce(robust)` clusters on the panel variable ──────
  {
    piet::SessionState s(workdir);
    seed(s, pweightFixture());
    Reply x0 = run(s, "xtreg y x1 x2, fe");
    Reply x0Ng = run(s, "display e(N_g) \"|\" e(df_a)");
    Reply x2 = run(s, "xtreg y x1 x2, fe vce(cluster id)");
    Reply x1 = run(s, "xtreg y x1 x2, fe vce(robust)");
    Reply x1Str = run(s, kEStrings);
    Reply x1b = run(s, "xtreg y x1 x2, fe robust");
    w2check(x0.rc == "0" && close(coefOf(x0.est, "x1", "se"), kX0SeX1, 1e-9),
            "w2_pweight X0_fe (control): conventional se_x1 +1.c1a27db214f0cX-003. Got rc " + x0.rc +
                " se=" + fmt(coefOf(x0.est, "x1", "se")));
    w2check(x0Ng.text == "15|14",
            "w2_pweight X0_fe PXT: e(N_g)=15 and e(df_a)=14 are reachable after `xtreg, fe`. "
            "Main: `.|.` (no producer). Got `" + x0Ng.text + "`");
    w2check(x2.rc == "0" && close(coefOf(x2.est, "x1", "se"), kX1SeX1, 1e-9) &&
                numOf(x2.est, "n_clusters") == 15.0,
            "w2_pweight X2_fe_clusterid (control): se_x1 +1.0ba4e5218cb87X-002, N_clust 15. Got rc " +
                x2.rc + " se=" + fmt(coefOf(x2.est, "x1", "se")));
    w2check(x1.rc == "0" && close(coefOf(x1.est, "x1", "se"), kX1SeX1, 1e-9) &&
                numOf(x1.est, "n_clusters") == 15.0 && numOf(x1.est, "df_r") == 14.0,
            "w2_pweight X1_fe_vcerobust: `fe vce(robust)` IS `fe vce(cluster id)` -- N_clust 15, "
            "df_r 14, the X2 SE. Main ran HC1 (n_clusters 0, df_r 103, se .222240881). Got rc " +
                x1.rc + " se=" + fmt(coefOf(x1.est, "x1", "se")) + " n_clusters=" +
                fmt(numOf(x1.est, "n_clusters")) + " df_r=" + fmt(numOf(x1.est, "df_r")) +
                " vcov_type=" + strOf(x1.est, "vcov_type"));
    // NOT claimed: X1/X2 PXT e(df_m). The oracle says 1 under the cluster VCE
    // (16 under X0's conventional one); this build answers 2 through e() and
    // 16 in the block for both. Pre-existing on X2; the Stata rule is not
    // identified from the ado source on disk, so no formula is guessed here.
    // e(F) IS at parity: 44.547236169625364 (oracle F(2,14) = 44.55).
    w2check(x1Str.text == "cluster|Robust|id||",
            "w2_pweight X1 PSTR: `cluster|Robust|id|||` -- e(clustvar) is the PANEL variable nobody "
            "typed. Got `" + x1Str.text + "`");
    w2check(x1b.rc == "0" && close(coefOf(x1b.est, "x1", "se"), kX1SeX1, 1e-9) &&
                numOf(x1b.est, "n_clusters") == 15.0,
            "w2_pweight X1 (bare `robust` spelling): same rewrite. Got rc " + x1b.rc +
                " se=" + fmt(coefOf(x1b.est, "x1", "se")));
    // X6's shape unweighted: an explicit cluster() beside `robust` under fe --
    // the cluster wins (BUG-537's rule through the M4 twin parser) and it is
    // NOT the panel variable. Identity with this build's own `vce(cluster cl2)`.
    Reply c2 = run(s, "xtreg y x1 x2, fe vce(cluster cl2)");
    Reply rc2 = run(s, "xtreg y x1 x2, fe robust cluster(cl2)");
    Reply rc2Str = run(s, kEStrings);
    w2check(rc2.rc == "0" && numOf(rc2.est, "n_clusters") == 5.0 &&
                close(coefOf(rc2.est, "x1", "se"), coefOf(c2.est, "x1", "se"), 1e-12) &&
                rc2Str.text == "cluster|Robust|cl2||",
            "w2_pweight X6 (shape) + R5 (rule): `fe robust cluster(cl2)` clusters on cl2 (5 "
            "clusters), not HC1 and not on id. Main: HC1, N_clust 0. Got rc " + rc2.rc +
                " n_clusters=" + fmt(numOf(rc2.est, "n_clusters")) + " strings `" + rc2Str.text + "`");
  }

  // ── e(N_clust) after UNclustered iv/poisson fits: resolver-level rule ───
  // w2d_uncovered PREC ivregress_factors_ctl (`.` unclustered, count for the
  // cluster twins ivregress_cluster_first_025). The engine's run_iv/run_fepois
  // emit n_clusters 0 unconditionally; main printed 0.
  {
    piet::SessionState s(workdir);
    seed(s, pweightFixture());
    (void)run(s, "generate z1 = mod(_n*5,13)");
    (void)run(s, "generate z2 = mod(_n*3,7)");
    (void)run(s, "generate cnt = mod(_n*7,5)");
    struct Arm { const char* cmd; const char* want; };
    const Arm arms[] = {
        {"ivregress 2sls y (x1 = z1 z2)", "."},
        {"ivregress 2sls y (x1 = z1 z2), vce(cluster id)", "15"},
        {"poisson cnt x1 x2", "."},
        {"poisson cnt x1 x2, vce(cluster id)", "15"},
    };
    for (const Arm& a : arms) {
      Reply f = run(s, a.cmd);
      Reply n = run(s, "display e(N_clust)");
      Reply m = run(s, "display missing(e(N_clust))");
      const bool unclustered = std::string(a.want) == ".";
      w2check(f.rc == "0" && n.text == a.want && m.text == (unclustered ? "1" : "0"),
              std::string("w2d_uncovered ivregress_factors_ctl / ivregress_cluster_first_025: `") +
                  a.cmd + "` e(N_clust) is `" + a.want + "`. Main: 0 when unclustered. Got fit rc " +
                  f.rc + " e(N_clust) `" + n.text + "` missing() `" + m.text + "` " + f.msg);
    }
  }

  // ── e(vcetype)/e(clustvar) belong to ONE fit, whatever handler made it ──
  // Review finding (attempt 3): setLastModel reset these two only for the
  // engine's VCE spellings, so a fit recording another spelling (nbreg's
  // engine `--vce robust`, pca "none", teffects/oaxaca "robust") inherited
  // the previous fit's macros. Before this lane published them they read
  // empty after those fits; the stale value is what these arms refuse.
  {
    piet::SessionState s(workdir);
    seed(s, pweightFixture());
    (void)run(s, "generate yc = mod(_n*7,5)");
    (void)run(s, "generate d = mod(id,2)");
    Reply cl = run(s, "regress yc x1, vce(cluster id)");
    Reply clStr = run(s, kEStrings);
    Reply nb = run(s, "nbreg yc x1, vce(robust)");
    Reply nbStr = run(s, "display \"`e(clustvar)'|\"");
    w2check(clStr.text == "cluster|Robust|id||" && nb.rc == "0" && nbStr.text == "|",
            "stale clustvar: `regress, vce(cluster id)` then `nbreg, vce(robust)` -- e(clustvar) is "
            "empty (nbreg robust clusters on nothing; the attempt-2 draft left `id`). Got regress `" +
                clStr.text + "`, nbreg rc " + nb.rc + " clustvar `" + nbStr.text + "` " + nb.msg);
    (void)run(s, "regress yc x1, vce(cluster id)");
    Reply tf = run(s, "teffects nnmatch (y x1 x2) (d)");
    Reply tfStr = run(s, "display \"`e(vcetype)'|`e(clustvar)'|\"");
    w2check(tf.rc == "0" && tfStr.text == "AI robust||",
            "teffects nnmatch after a clustered regress: e(vcetype) is the handler's own `AI robust` "
            "(aer_estim2_oracle.log INFO|nn|e(vcetype)) and e(clustvar) is empty. Got rc " + tf.rc +
                " `" + tfStr.text + "` " + tf.msg);
    (void)run(s, "regress yc x1, vce(robust)");
    Reply pc = run(s, "pca x1 x2");
    Reply pcStr = run(s, "display \"`e(vcetype)'|`e(clustvar)'|\"");
    w2check(pc.rc == "0" && pcStr.text == "||",
            "pca after `regress, vce(robust)`: e(vcetype) is erased, not the regression's `Robust` "
            "(pca has no VCE). Got rc " + pc.rc + " `" + pcStr.text + "` " + pc.msg);
  }

  // ── BUG-539: reghdfe residuals[(newvar)] in every spelling, and noabsorb ──
  {
    piet::SessionState s(workdir);
    seed(s, smallsFixture());
    (void)run(s, "generate foo = 1");  // a column made BEFORE the fit must survive
    for (const char* sp : {"resid", "residuals", "res", "residual", "resi"}) {
      (void)run(s, "capture drop _reghdfe_resid");
      Reply r = run(s, std::string("reghdfe y x x2, absorb(id) ") + sp);
      Reply made = run(s, "confirm variable _reghdfe_resid");
      w2check(r.rc == "0" && made.rc == "0" && r.est.is_object() && r.sawMutation,
              std::string("w2_smalls reghdfe_bare_") + sp + ": rc 0 and `_reghdfe_resid` exists "
              "(oracle made=1), the estimation block AND a data_mutation both emitted. Main: "
              "r(198) `option " + sp + " not allowed`. Got rc " + r.rc + " confirm rc " + made.rc +
                  " " + r.msg);
    }
    {
      (void)run(s, "capture drop _reghdfe_resid");
      Reply r = run(s, "reghdfe y x x2, absorb(id) re");
      Reply made = run(s, "confirm variable _reghdfe_resid");
      w2check(r.rc == "198" && made.rc == "111",
              "w2_smalls reghdfe_bare_re: `re` is BELOW the floor (`res`) -- r(198), nothing made. "
              "Registering only `residuals` would let the prefix rule accept it. Got rc " + r.rc +
                  " confirm rc " + made.rc);
    }
    for (const char* sp : {"resid", "residuals", "res", "residual"}) {
      (void)run(s, "capture drop myr");
      Reply r = run(s, std::string("reghdfe y x x2, absorb(id) ") + sp + "(myr)");
      Reply made = run(s, "confirm variable myr");
      w2check(r.rc == "0" && made.rc == "0" && r.newVars.size() == 1 && r.newVars[0] == "myr",
              std::string("w2_smalls reghdfe_named_") + sp + ": `" + sp +
                  "(myr)` creates myr (new_vars=[myr]). Main: r(198). Got rc " + r.rc +
                  " confirm rc " + made.rc);
    }
    {
      Reply r = run(s, "reghdfe y x x2, absorb(id) residuals(myr)");
      w2check(r.rc == "110",
              "w2_smalls reghdfe_named_exists: myr already present -> r(110) `variable myr already "
              "defined`. Got rc " + r.rc + " " + r.msg);
    }
    {
      (void)run(s, "capture drop myr");
      Reply fit = run(s, "quietly reghdfe y x x2, absorb(id) residuals(myr)");
      Reply sum = run(s, "summarize myr");
      Reply eres = run(s, "display \"`e(resid)'\"");
      Reply foo = run(s, "confirm variable foo");
      double sd = kNaN, sumv = kNaN, n = kNaN;
      try {
        if (sum.stats.is_object() && sum.stats.contains("myr") && sum.stats["myr"].is_object()) {
          sd = numOf(sum.stats["myr"], "sd");
          sumv = numOf(sum.stats["myr"], "sum");
          n = numOf(sum.stats["myr"], "N");
        }
      } catch (...) {
      }
      // reghdfe_resid_values: sum 1.05748743096e-14, sd 3.196166998496377, N 12.
      w2check(fit.rc == "0" && n == 12.0 && close(sd, 3.196166998496377, 1e-9) &&
                  std::isfinite(sumv) && std::fabs(sumv) < 1e-9,
              "w2_smalls reghdfe_resid_values: the column IS the within-residual -- N 12, sd "
              "3.196166998496377, sum ~0 (this engine's own predict path gives 3.1961669984963765). "
              "Got rc " + fit.rc + " N=" + fmt(n) + " sd=" + fmt(sd) + " sum=" + fmt(sumv));
      w2check(eres.text == "myr",
              "w2_smalls reghdfe_e_resid: e(resid) holds the residual variable's name. Got `" +
                  eres.text + "`");
      w2check(foo.rc == "0",
              "w2_smalls (frame law): a column generated BEFORE the fit survives the residual "
              "materialization -- the live head was extended, not replaced by a stale snapshot. "
              "Got confirm rc " + foo.rc);
    }
    {
      Reply fit = run(s, "reghdfe y x x2, absorb(fe1=id)");
      Reply sum = run(s, "summarize fe1");
      const double sd = sum.stats.contains("fe1") ? numOf(sum.stats["fe1"], "sd") : kNaN;
      const double n = sum.stats.contains("fe1") ? numOf(sum.stats["fe1"], "N") : kNaN;
      const double mean = sum.stats.contains("fe1") ? numOf(sum.stats["fe1"], "mean") : kNaN;
      w2check(fit.rc == "0" && fit.sawMutation && n == 12 &&
                  close(sd, 3.063714966900867, 1e-9) && std::fabs(mean) < 1e-12,
              "w2_smalls reghdfe_savefe: absorb(fe1=id) creates this fit's mean-zero double "
              "effect, N 12 and sd 3.063714966900867. Got rc " + fit.rc + " N=" + fmt(n) +
                  " sd=" + fmt(sd) + " mean=" + fmt(mean) + " " + fit.msg);
      Reply dup = run(s, "reghdfe y x x2, absorb(fe1=id)");
      w2check(dup.rc == "110", "saved FE creator refuses an existing destination before fitting");
      (void)run(s, "drop fe1");
      Reply cut = run(s, "reghdfe y x x2 if t>=2, absorb(fe1=id) residuals(res_save)");
      Reply outside = run(s, "count if t==1 & !missing(fe1)");
      w2check(cut.rc == "0" && outside.rc == "0" && rOf(s, "N") == 0,
              "saved FE plus residuals preserve the exact if-sample: effect missing off e(sample)");
      (void)run(s, "drop fe1 res_save");
    }
    {
      Reply plain = run(s, "regress y x x2");
      Reply eresAfter = run(s, "display \"`e(resid)'\"");
      w2check(eresAfter.text.empty(),
              "e(resid) belongs to ONE fit: a later plain regress erases it (the store is cleared at "
              "setLastModel). Got `" + eresAfter.text + "` rc " + plain.rc);
    }
    {
      Reply r = run(s, "reghdfe y x x2, noabsorb");
      Reply ols = run(s, "regress y x x2");
      w2check(r.rc == "0" && close(coefOf(r.est, "x", "b"), 1.90685050483118, 1e-12) &&
                  close(coefOf(r.est, "x", "b"), coefOf(ols.est, "x", "b"), 1e-14),
              "w2_smalls reghdfe_noabsorb: rc 0, _b[x] = 1.90685050483118 = the constant-only OLS "
              "fit. Main: r(198) `option noabsorb not allowed`. Got rc " + r.rc + " b=" +
                  fmt(coefOf(r.est, "x", "b")) + " " + r.msg);
    }
    {
      // residuals() under if/in (review finding, attempt 2: paper 082 types
      // `reghdfe ... if is_teacher >= 3, ... residuals`). No captured arm for
      // the restricted form; reghdfe fills the column on e(sample) ONLY and
      // leaves every other row missing, so the assertions are IDENTITIES:
      // (a) non-missing count == e(N) and nothing on the excluded rows;
      // (b) the in-sample values are the residuals of the same fit run on a
      //     frame physically cut to the sample (a second session, `keep if`).
      // Main: r(198) `option residuals not allowed`; the attempt-1 draft
      // refused by name. Both FAIL (a).
      struct Case {
        const char* restr;   // the if/in clause
        const char* keep;    // the equivalent physical cut
        const char* outside; // an if-condition selecting the excluded rows
        double nIn;
      };
      for (const Case& c : {Case{"if t > 1", "keep if t > 1", "t == 1", 8.0},
                            Case{"in 4/12", "keep in 4/12", "id == 1", 9.0},
                            Case{"if t > 1 in 4/12", "keep if t > 1 in 4/12", "t == 1 | id == 1", 6.0}}) {
        (void)run(s, "capture drop myr");
        Reply r = run(s, std::string("reghdfe y x x2 ") + c.restr + ", absorb(id) residuals(myr)");
        Reply eN = run(s, "display e(N)");
        Reply nIn = run(s, "count if !missing(myr)");
        Reply nInN = run(s, "display r(N)");
        Reply nOut = run(s, std::string("count if !missing(myr) & (") + c.outside + ")");
        Reply nOutN = run(s, "display r(N)");
        Reply sum = run(s, "summarize myr");
        Reply foo = run(s, "confirm variable foo");
        Reply mask = run(s, "confirm variable __piet_resid_y__");
        // The reference session gets its OWN directory: a second session on
        // `workdir` would run the BUG-389 dispatch-exit orphan sweep over s's
        // head and lastModel files (the mint registry is process-global and
        // `cut`'s owner table does not claim them). selftest.cpp `_fresh`.
        const std::string cutDir = workdir + "_cut";
        fs::remove_all(cutDir, ec);
        fs::create_directories(cutDir, ec);
        piet::SessionState cut(cutDir);
        seed(cut, smallsFixture());
        (void)run(cut, c.keep);
        (void)run(cut, "reghdfe y x x2, absorb(id) residuals(myr)");
        Reply sumCut = run(cut, "summarize myr");
        double sd = kNaN, sdCut = kNaN, sm = kNaN, smCut = kNaN;
        try {
          if (sum.stats.is_object() && sum.stats.contains("myr")) {
            sd = numOf(sum.stats["myr"], "sd");
            sm = numOf(sum.stats["myr"], "sum");
          }
          if (sumCut.stats.is_object() && sumCut.stats.contains("myr")) {
            sdCut = numOf(sumCut.stats["myr"], "sd");
            smCut = numOf(sumCut.stats["myr"], "sum");
          }
        } catch (...) {
        }
        const std::string want = fmt(c.nIn);
        w2check(r.rc == "0" && eN.text == want && nInN.text == want && nOutN.text == "0" &&
                    foo.rc == "0" && mask.rc == "111",
                std::string("reghdfe residuals() `") + c.restr + "`: rc 0, the column is "
                "non-missing on exactly e(N)=" + want + " rows and on NONE of the excluded rows, "
                "the pre-fit column survives, the scratch mask column is gone. Got rc " + r.rc +
                    " e(N)=" + eN.text + " nonmissing=" + nInN.text + " outside=" + nOutN.text +
                    " foo rc " + foo.rc + " mask rc " + mask.rc + " " + r.msg);
        w2check(close(sd, sdCut, 1e-9) && std::isfinite(sm) && std::isfinite(smCut) &&
                    std::fabs(sm - smCut) < 1e-9,
                std::string("reghdfe residuals() `") + c.restr + "`: the values are the fit's own "
                "residuals -- sd/sum equal those after `" + c.keep + "`. Got sd=" + fmt(sd) +
                    " vs " + fmt(sdCut) + " sum=" + fmt(sm) + " vs " + fmt(smCut));
      }
      fs::remove_all(workdir + "_cut", ec);
    }
    {
      // noabsorb's floor: reghdfe's syntax declares `noAbsorb2` -> `noa` .. `noabsorb2`.
      Reply full = run(s, "reghdfe y x x2, noabsorb");
      for (const char* sp : {"noa", "noabsorb2"}) {
        Reply r = run(s, std::string("reghdfe y x x2, ") + sp);
        w2check(r.rc == "0" && close(coefOf(r.est, "x", "b"), coefOf(full.est, "x", "b"), 1e-14),
                std::string("w2_smalls reghdfe_noabsorb (spelling `") + sp + "`, reghdfe's "
                "noAbsorb2): same fit as `noabsorb`. Got rc " + r.rc + " " + r.msg);
      }
    }
    {
      // Review finding (attempt 2): the attempt-1 draft registered the reghdfe
      // spellings with registerOptVocab, whose `reghdfe` slot IS
      // kVocabIvRegress -- the known-set runIvRegress's reject net reads for
      // ivregress/ivreg2 too -- so these passed at rc 0 with the option
      // silently ignored. Neither command has either option in Stata
      // (ivregress's syntax has no residuals/noabsorb), so the answer is the
      // unknown-option r(198).
      for (const char* cmd : {"ivregress 2sls y (x = x2), resid",
                              "ivregress 2sls y (x = x2), residuals(r)",
                              "ivregress 2sls y (x = x2), noabsorb",
                              "ivregress 2sls y (x = x2), noa",
                              "ivreg2 y (x = x2), resid"}) {
        Reply r = run(s, cmd);
        w2check(r.rc == "198",
                std::string("IV reject net: `") + cmd + "` stays r(198) -- reghdfe's residuals/"
                "noabsorb spellings must not leak into the shared IV vocabulary. Got rc " + r.rc +
                    " " + r.msg);
      }
    }
    {
      // The reject net still refuses the option where Stata has none.
      Reply r = run(s, "regress y x x2, resid");
      w2check(r.rc == "198",
              "regress has no residuals option in Stata: `regress ..., resid` stays r(198) -- the "
              "vocabulary widened for the reghdfe token only. Got rc " + r.rc + " " + r.msg);
    }
  }

  // ── BUG-568: areg's clustered dof (retires the BUG-684 stop-gap) ─────────
  // areg charges every absorbed parameter in the cluster small-sample factor,
  // including an FE nested in the cluster; reghdfe and xtreg,fe exempt it. The
  // BUG-684 guard refused weighted areg + cluster BY NAME until this landed;
  // the same commands now run at rc 0 with areg's own numbers. Main: the
  // unweighted fit answered the xtreg/reghdfe number (X2's se_x1 .2613712 where
  // A0 is .2785) and the weighted ones were refused.
  {
    piet::SessionState s(workdir);
    seed(s, pweightFixture());
    const char* kPstr =
        "display \"PSTR|\" e(N) \"|\" e(df_r) \"|\" e(N_clust) \"|`e(vce)'|`e(vcetype)'|"
        "`e(clustvar)'|`e(wtype)'|`e(wexp)'|\"";

    Reply a0 = run(s, "areg y x1 x2, absorb(id) vce(cluster id)");
    Reply a0Str = run(s, kPstr);
    w2check(a0.rc == "0" && close(coefOf(a0.est, "x1", "b"), kA0BX1, 1e-12) &&
                close(coefOf(a0.est, "x1", "se"), kA0SeX1, 1e-9) &&
                close(coefOf(a0.est, "_cons", "se"), kA0SeCons, 1e-9),
            "w2_pweight A0_areg_unweighted_clusterid: se_x1 +1.1d41125f9cb99X-002, se_cons "
            "+1.c749b407eba18X-001 -- areg charges the nested id FE (main: X2's reghdfe/xtreg "
            "number " + fmt(kX1SeX1) + "). Got rc " + a0.rc + " se=" +
                fmt(coefOf(a0.est, "x1", "se")) + " se_cons=" + fmt(coefOf(a0.est, "_cons", "se")) +
                " " + a0.msg);
    w2check(a0Str.text == "PSTR|120|14|15|cluster|Robust|id|||",
            "w2_pweight A0 PSTR: `120|14|15|cluster|Robust|id|||` (df_r = G-1). Got `" +
                a0Str.text + "`");

    Reply a1 = run(s, "areg y x1 x2 [pweight=w], absorb(id)");
    Reply a1Str = run(s, kPstr);
    w2check(a1.rc == "0" && close(coefOf(a1.est, "x1", "se"), kA1SeX1, 1e-9),
            "w2_pweight A1_areg_pw: bare pweight areg is robust (HC1), se_x1 "
            "+1.e178af045346fX-003. Got rc " + a1.rc + " se=" + fmt(coefOf(a1.est, "x1", "se")) +
                " " + a1.msg);
    w2check(a1Str.text == "PSTR|120|103|.|robust|Robust||pweight|= w|",
            "w2_pweight A1 PSTR: `120|103|.|robust|Robust||pweight|= w|` -- e(N_clust) is "
            "MISSING without a cluster (BUG-570; main: 0). Got `" + a1Str.text + "`");

    Reply a2 = run(s, "areg y x1 x2 [pweight=w], absorb(id) vce(cluster id)");
    Reply a2Str = run(s, kPstr);
    w2check(a2.rc == "0" && close(coefOf(a2.est, "x1", "se"), kA2SeX1, 1e-9) &&
                close(coefOf(a2.est, "_cons", "se"), kA2SeCons, 1e-9),
            "w2_pweight A2_areg_pw_clusterid: se_x1 +1.cfb6746651353X-003, se_cons "
            "+1.61f6af6a6958dX-001 (NOT H1's reghdfe +1.b315e83b84cedX-003; main: refused by "
            "the BUG-684 guard). Got rc " + a2.rc + " se=" + fmt(coefOf(a2.est, "x1", "se")) +
                " se_cons=" + fmt(coefOf(a2.est, "_cons", "se")) + " " + a2.msg);
    w2check(a2Str.text == "PSTR|120|14|15|cluster|Robust|id|pweight|= w|",
            "w2_pweight A2 PSTR: `120|14|15|cluster|Robust|id|pweight|= w|`. Got `" + a2Str.text +
                "`");

    Reply a3 = run(s, "areg y x1 x2 [pweight=wv], absorb(id) vce(cluster id)");
    w2check(a3.rc == "0" && close(coefOf(a3.est, "x1", "b"), kA3BX1, 1e-12) &&
                close(coefOf(a3.est, "x1", "se"), kA3SeX1, 1e-9) &&
                close(coefOf(a3.est, "_cons", "se"), kA3SeCons, 1e-9),
            "w2_pweight A3_areg_pw_VARYING_w_clusterid: b_x1 +1.e84a361e3023bX+000, se_x1 "
            "+1.31cf90b884108X-002, se_cons +1.ed54b6d900724X-001. Got rc " + a3.rc + " b=" +
                fmt(coefOf(a3.est, "x1", "b")) + " se=" + fmt(coefOf(a3.est, "x1", "se")) + " " +
                a3.msg);

    // The other two spellings the guard used to refuse: legacy cluster() and the
    // vce(cl ...) abbreviation. aweight + cluster(id) must equal pweight + vce(cluster
    // id) (the cluster meat is the same sandwich under aw and pw).
    Reply a2aw = run(s, "areg y x1 x2 [aweight=w], absorb(id) cluster(id)");
    w2check(a2aw.rc == "0" && close(coefOf(a2aw.est, "x1", "se"), kA2SeX1, 1e-9),
            "BUG-568: `areg [aweight=w], absorb(id) cluster(id)` runs with areg's dof -- same "
            "se_x1 as A2. Got rc " + a2aw.rc + " se=" + fmt(coefOf(a2aw.est, "x1", "se")) + " " +
                a2aw.msg);

    // Control: reghdfe on the SAME fit keeps the nested-FE exemption (H1).
    Reply h1 = run(s, "reghdfe y x1 x2 [pweight=w], absorb(id) vce(cluster id)");
    w2check(h1.rc == "0" && close(coefOf(h1.est, "x1", "se"), kH1SeX1, 1e-9),
            "w2_pweight H1_reghdfe_pw_clusterid_SSC (control): reghdfe keeps its convention, "
            "se_x1 +1.b315e83b84cedX-003 -- the areg switch must not leak. Got rc " + h1.rc +
                " se=" + fmt(coefOf(h1.est, "x1", "se")) + " " + h1.msg);
    // Control: xtreg,fe on the same fit is X2 (unchanged).
    Reply x2 = run(s, "xtreg y x1 x2, fe vce(cluster id)");
    w2check(x2.rc == "0" && close(coefOf(x2.est, "x1", "se"), kX1SeX1, 1e-9),
            "w2_pweight X2_fe_clusterid (control): xtreg,fe keeps the nested exemption, se_x1 "
            "+1.0ba4e5218cb87X-002. Got rc " + x2.rc + " se=" + fmt(coefOf(x2.est, "x1", "se")));

    // BUG-570 (R2_pw PSTR, field for field): e(N_clust) is `.` after an
    // unclustered fit. Main printed `120|117|0|...`.
    Reply r2 = run(s, "regress y x1 x2 [pweight=w]");
    Reply r2Str = run(s, kPstr);
    w2check(r2.rc == "0" && r2Str.text == "PSTR|120|117|.|robust|Robust||pweight|= w|",
            "w2_pweight R2_pw PSTR: `120|117|.|robust|Robust||pweight|= w|` (main: N_clust 0). "
            "Got `" + r2Str.text + "`");
    Reply r0 = run(s, "regress y x1 x2");
    Reply r0N = run(s, "display e(N_clust)");
    w2check(r0.rc == "0" && r0N.text == ".",
            "w2_pweight R0_unweighted PSTR: e(N_clust) is `.` (main: 0). Got `" + r0N.text + "`");
    Reply hu = run(s, "reghdfe y x1 x2, absorb(id)");
    Reply huN = run(s, "display e(N_clust)");
    w2check(hu.rc == "0" && huN.text == ".",
            "BUG-570: e(N_clust) is `.` after an unclustered reghdfe too (main: 0). Got `" +
                huN.text + "`");
    Reply r4 = run(s, "regress y x1 x2 [pweight=w], vce(cluster id)");
    Reply r4N = run(s, "display e(N_clust)");
    w2check(r4.rc == "0" && r4N.text == "15",
            "w2_pweight R4_pw_cluster (control): a clustered fit keeps e(N_clust) 15. Got `" +
                r4N.text + "`");
    // xtreg,fe posts through estimation_m4.cpp's cacheLastModelM4, not runRegress.
    Reply x0 = run(s, "xtreg y x1 x2, fe");
    Reply x0Str = run(s, "display e(N) \"|\" e(df_r) \"|\" e(N_clust) \"|\"");
    w2check(x0.rc == "0" && x0Str.text == "120|103|.|",
            "w2_pweight X0_fe PSTR `120|103|.|conventional|||||`: e(N_clust) is `.` after an "
            "unclustered xtreg,fe (BUG-570; main: 0). Got `" + x0Str.text + "`");
    Reply x0Pstr = run(s, kPstr);
    Reply x0Pxt = run(s, "display e(N_g) \"|\" e(df_a)");
    w2check(x0Pstr.text == "PSTR|120|103|.|conventional|||||" && x0Pxt.text == "15|14",
            "w2_pweight X0_fe PSTR `120|103|.|conventional|||||` field for field: e(vce) is "
            "`conventional` after a default-VCE xtreg,fe (main: `iid`); control: PXT e(N_g) 15, "
            "e(df_a) 14 unchanged. Got `" + x0Pstr.text + "` / `" + x0Pxt.text + "`");
    (void)run(s, "regress y x1 x2");
    Reply r0Pstr = run(s, kPstr);
    w2check(r0Pstr.text == "PSTR|120|117|.|ols|||||",
            "w2_pweight R0_unweighted PSTR `120|117|.|ols|||||` field for field (main: e(vce) "
            "`iid`). Got `" + r0Pstr.text + "`");
    (void)run(s, "xtreg y x1 x2, fe vce(cluster id)");
    Reply x2Pstr = run(s, kPstr);
    w2check(x2Pstr.text == "PSTR|120|14|15|cluster|Robust|id|||",
            "w2_pweight X2_fe_clusterid PSTR (control): a clustered xtreg,fe keeps "
            "`120|14|15|cluster|Robust|id|||`. Got `" + x2Pstr.text + "`");
    // The corrected spelling must survive `estimates store` / `restore` (the
    // restore re-publishes lm.vcov first, then the stored macro layer).
    (void)run(s, "xtreg y x1 x2, fe");
    (void)run(s, "estimates store m_xtfe");
    (void)run(s, "regress y x1 x2, vce(robust)");
    Reply rest = run(s, "estimates restore m_xtfe");
    Reply restV = run(s, "display \"`e(vce)'\"");
    w2check(rest.rc == "0" && restV.text == "conventional",
            "X0_fe e(vce) after `estimates restore` of a default xtreg,fe: `conventional`. Got rc " +
                rest.rc + " `" + restV.text + "`");
    Reply x2N = run(s, "xtreg y x1 x2, fe vce(cluster id)");
    Reply x2NStr = run(s, "display e(N_clust)");
    w2check(x2N.rc == "0" && x2NStr.text == "15",
            "w2_pweight X2_fe_clusterid PSTR (control): xtreg,fe clustered keeps e(N_clust) 15. "
            "Got `" + x2NStr.text + "`");

    // `margins, at()` was the same engine refit (cmd_margins' --at branches take
    // no weight and no areg dof). Main, this frame: after `[aweight=w]` it
    // answered 3.3492323, the unweighted fit's margin (weighted: 3.5853964);
    // after areg + cluster, the reghdfe fit's SE. The interim build refused
    // these by name (r(198)). margins-corpus-shapes serves them from the fit's
    // own e(b)/e(V) with the fit's weight (StataBE serves margins after a
    // weighted absorbed fit: w2v_margins_corpus F1_dydx_at / F2_at_post, xtreg
    // fe [pweight]), so each must be rc 0 and equal the fit's own
    // _b[_cons] + _b[x1]*1 + _b[x2]*mean(x2), the mean weighted as the fit is.
    for (const std::string& fit :
         {std::string("regress y x1 x2 [aweight=w]"), std::string("regress y x1 x2 [pweight=w]"),
          std::string("areg y x1 x2, absorb(id) vce(cluster id)")}) {
      const bool isAreg = fit.rfind("areg", 0) == 0;
      (void)run(s, fit);
      (void)run(s, isAreg ? "quietly summarize x2" : "quietly summarize x2 [aweight=w]");
      Reply want = run(s, "display %21.0g _b[_cons] + _b[x1] + _b[x2]*r(mean)");
      Reply m = run(s, "margins, at(x1=1)");
      Reply got = run(s, "display %21.0g r(b)[1,1]");
      const double wv = std::strtod(want.text.c_str(), nullptr);
      const double gv = std::strtod(got.text.c_str(), nullptr);
      w2check(m.rc == "0" && close(gv, wv, 1e-10) &&
                  (isAreg || std::fabs(gv - 3.5853964) < 1e-7),
              "BUG-571 sibling: `margins, at(x1=1)` after `" + fit + "` is served and equals "
              "the fit's own weighted value " + want.text + " (main: the unweighted refit's "
              "3.3492323; interim: r(198)). Got rc " + m.rc + " `" + got.text + "` " + m.msg);
    }
    // Controls: an unweighted fit whose refit IS the fitted model keeps margins, at().
    for (const std::string& fit :
         {std::string("reghdfe y x1 x2, absorb(id) vce(cluster id)"),
          std::string("areg y x1 x2, absorb(id) vce(robust)"), std::string("regress y x1 x2")}) {
      Reply f = run(s, fit);
      Reply m = run(s, "margins, at(x1=1)");
      w2check(f.rc == "0" && m.rc == "0",
              "margins, at() control: still runs after `" + fit + "`. Got rc " + m.rc + " " +
                  m.msg);
    }
    // Controls: where the re-fit IS the fitted model, lincom's SE equals the fit's.
    for (const std::string& fit :
         {std::string("reghdfe y x1 x2, absorb(id) vce(cluster id)"),
          std::string("areg y x1 x2, absorb(id) vce(robust)")}) {
      Reply f = run(s, fit);
      Reply l = run(s, "lincom x1");
      w2check(f.rc == "0" && l.rc == "0" &&
                  close(coefOf(l.est, "x1", "se"), coefOf(f.est, "x1", "se"), 1e-12) &&
                  close(coefOf(l.est, "x1", "b"), coefOf(f.est, "x1", "b"), 1e-12),
              "post-estimation control: `lincom x1` after `" + fit + "` still runs and matches "
                  "the fit (se " + fmt(coefOf(f.est, "x1", "se")) + "). Got rc " + l.rc + " se=" +
                  fmt(coefOf(l.est, "x1", "se")) + " " + l.msg);
    }
  }

  // ── BUG-571 / BUG-568: test/testparm/lincom/nlcom answer under the FITTED model ──
  // tests/eval/golden/aer_w2/w2h_postest_refit.{do,log}, replayed fit by fit in the
  // do-file's own order (each fit, then test x1 / test x1 x2 / test x1 = x2 /
  // testparm x1 x2 / lincom x1 - x2 / lincom x1 / nlcom _b[x1]/_b[x2]); numbers are
  // the log's %21x fields decoded. The refit (lastModelBaseArgv) now carries the
  // fit's weight and areg's clustered dof. Main: every weighted and areg-cluster
  // arm was refused r(198) (BUG-571 / BUG-568 interim); before those refusals the
  // weighted arms answered the UNWEIGHTED model (`lincom x1` 2.4882849226 against
  // _b[x1] 2.3815319116) and the areg ones reghdfe's nested-FE exemption (A_cl se
  // .2613712122 against .2785685416). R_ctl / A_r_ctl / H_cl_ctl / X_cl_ctl are the
  // controls that already passed. nlcom's own record also reads el(r(V),1,1), which
  // PIET does not post (FU-266); its estimate/se/N are pinned off the nlcom reply.
  {
    struct PostArm {
      const char* fit;
      const char* cmd;
      int pvalLine;
      double seX1;  // PVAL _se[x1]
      int t1Line;
      double t1F, dfR, t1P;
      int t2Line;
      double t2F;
      int teqLine;
      double teqF;
      int tpLine;
      double tpF;
      int ldLine;
      double ldEst, ldSe, ldDf;
      int nlLine;
      double nlB, nlSe, nlN;
    };
    static const PostArm kArms[] = {
    {"R_ctl", "regress y x1 x2", 100, 0x1.5bdef76ad8402p-3,
     120, 0x1.ad31b17e454d5p+7, 117, 0x1.8a1682de01c11p-92,  138, 0x1.f56a45f6557cfp+6,  155, 0x1.b5f5d8e851540p+7,  173, 0x1.f56a45f6557cfp+6,
     191, 0x1.bbbf207cabbcfp+1, 0x1.dfcaa48bd4c4cp-3, 117,  229, -0x1.4580ea95fe803p+1, 0x1.ccd0958e01b57p-2, 120},
    {"R_pw", "regress y x1 x2 [pweight=w]", 274, 0x1.a3421f4182c9fp-3,
     294, 0x1.0eab675f65c27p+7, 117, 0x1.c05b7b8476d0ep-69,  312, 0x1.31eda827aa806p+6,  329, 0x1.1b338a4a17c83p+7,  347, 0x1.31eda827aa806p+6,
     365, 0x1.a607b4a48f5d9p+1, 0x1.1bba210ce5e47p-2, 117,  403, -0x1.4cf19bc1abdc2p+1, 0x1.f39bca9e9a05bp-2, 120},
    {"R_aw", "regress y x1 x2 [aweight=w]", 448, 0x1.691bde09fd0fep-3,
     468, 0x1.6cdc32aaa341dp+7, 117, 0x1.361e89e54eb26p-83,  486, 0x1.a7dcc4e6593d2p+6,  503, 0x1.7263e42aaa74dp+7,  521, 0x1.a7dcc4e6593d2p+6,
     539, 0x1.a607b4a48f5d9p+1, 0x1.f030d4f7f98a8p-3, 117,  577, -0x1.4cf19bc1abdc2p+1, 0x1.01a62f3f75cb5p-1, 120},
    {"R_fw", "regress y x1 x2 [fweight=w]", 621, 0x1.9d742a6d87ee3p-4,
     641, 0x1.1652a35e103a8p+9, 357, 0x1.bbd5e2f779185p-247,  659, 0x1.4354b7050c474p+8,  676, 0x1.1a8a8edee7c56p+9,  694, 0x1.4354b7050c474p+8,
     712, 0x1.a607b4a48f5d9p+1, 0x1.1c0eecde42f0ep-3, 357,  750, -0x1.4cf19bc1abdc5p+1, 0x1.26ff390c5d725p-2, 360},
    {"R_pw_cl", "regress y x1 x2 [pweight=w], vce(cluster id)", 796, 0x1.7f38be33c76eep-3,
     816, 0x1.43f7e82bd583fp+7, 14, 0x1.2ceebff79ccdap-28,  834, 0x1.440b72d1cd924p+6,  851, 0x1.e8d236d97b3eap+6,  869, 0x1.440b72d1cd924p+6,
     887, 0x1.a607b4a48f5d9p+1, 0x1.3169e3b10f694p-2, 14,  925, -0x1.4cf19bc1abdc2p+1, 0x1.b9b2c4cc1b250p-2, 120},
    {"A_cl", "areg y x1 x2, absorb(id) vce(cluster id)", 972, 0x1.1d41125f9cb99p-2,
     992, 0x1.a423aa1505bb5p+5, 14, 0x1.1cd3d6481e0dfp-18,  1010, 0x1.39bc0068808e2p+5,  1027, 0x1.22ea1046ae997p+6,  1045, 0x1.39bc0068808e2p+5,
     1063, 0x1.7eda113da74cap+1, 0x1.6724c49dc7de2p-2, 14,  1101, -0x1.09c4f41b4f3d0p+1, 0x1.37f69c0db7682p-2, 120},
    {"A_cl_t", "areg y x1 x2, absorb(id) vce(cluster t)", 1182, 0x1.2d86c8053eee4p-2,
     1202, 0x1.78041eb416f97p+5, 7, 0x1.f8d6447f6e78ep-13,  1220, 0x1.d473a9416d753p+4,  1237, 0x1.c3c020e30a8e1p+5,  1255, 0x1.d473a9416d753p+4,
     1273, 0x1.7eda113da74cap+1, 0x1.97955321864abp-2, 7,  1311, -0x1.09c4f41b4f3cbp+1, 0x1.26125d5c0e892p-1, 120},
    {"A_r_ctl", "areg y x1 x2, absorb(id) vce(robust)", 1357, 0x1.c7263a2d3b856p-3,
     1377, 0x1.4a0cc44278e32p+6, 103, 0x1.21af3bcdfe69bp-47,  1395, 0x1.c4fc5778389e1p+5,  1412, 0x1.c4d1c699b43c7p+6,  1430, 0x1.c4fc5778389e1p+5,
     1448, 0x1.7eda113da74cap+1, 0x1.1fdd52af81558p-2, 103,  1486, -0x1.09c4f41b4f3d0p+1, 0x1.82e4761de30a1p-2, 120},
    {"A_pw_cl", "areg y x1 x2 [pweight=w], absorb(id) vce(cluster id)", 1534, 0x1.cfb6746651353p-3,
     1554, 0x1.0122444f206eap+6, 14, 0x1.6645a7c472225p-20,  1572, 0x1.1af8ab0be7405p+5,  1589, 0x1.176c88cd20e37p+6,  1607, 0x1.1af8ab0be7405p+5,
     1625, 0x1.5e164b9739c94p+1, 0x1.4f17973fb997bp-2, 14,  1663, -0x1.f954da104f929p+0, 0x1.33159b9206a66p-2, 120},
    {"A_aw_cl", "areg y x1 x2 [aweight=w], absorb(id) vce(cluster id)", 1711, 0x1.cfb6746651353p-3,
     1731, 0x1.0122444f206eap+6, 14, 0x1.6645a7c472225p-20,  1749, 0x1.1af8ab0be7405p+5,  1766, 0x1.176c88cd20e37p+6,  1784, 0x1.1af8ab0be7405p+5,
     1802, 0x1.5e164b9739c94p+1, 0x1.4f17973fb997bp-2, 14,  1840, -0x1.f954da104f929p+0, 0x1.33159b9206a66p-2, 120},
    {"H_cl_ctl", "reghdfe y x1 x2, absorb(id) vce(cluster id)", 1896, 0x1.0ba4e5218cb86p-2,
     1916, 0x1.dd3ee182c0eadp+5, 14, 0x1.140cce5b192cap-19,  1934, 0x1.6460bd5b5dd52p+5,  1951, 0x1.4a74c0780e685p+6,  1969, 0x1.6460bd5b5dd52p+5,
     1987, 0x1.7eda113da74c6p+1, 0x1.50f8d669b575cp-2, 14,  2025, -0x1.09c4f41b4f3cep+1, 0x1.24b451e506355p-2, 120},
    {"X_cl_ctl", "xtreg y x1 x2, fe vce(cluster id)", 2079, 0x1.0ba4e5218cb87p-2,
     2099, 0x1.dd3ee182c0eb1p+5, 14, 0x1.140cce5b192b7p-19,  2117, 0x1.6460bd5b5dd5ap+5,  2134, 0x1.4a74c0780e68ap+6,  2152, 0x1.6460bd5b5dd5ap+5,
     2170, 0x1.7eda113da74cap+1, 0x1.50f8d669b575dp-2, 14,  2208, -0x1.09c4f41b4f3d0p+1, 0x1.24b451e50637bp-2, 120},
    };
    piet::SessionState s(workdir);
    seed(s, pweightFixture());
    auto at = [](const PostArm& a, int line) {
      return std::string("w2h_postest_refit.log:") + std::to_string(line) + " " + a.fit;
    };
    for (const auto& a : kArms) {
      Reply f = run(s, a.cmd);
      const double b = sessionValue(s, "_b[x1]"), se = sessionValue(s, "_se[x1]");
      w2check(f.rc == "0" && close(se, a.seX1, 1e-9),
              at(a, a.pvalLine) + ": `" + a.cmd + "` rc 0, _se[x1] " + fmt(a.seX1) + " (T2). Got rc " +
                  f.rc + " se " + fmt(se) + " " + f.msg);

      Reply t1 = run(s, "test x1");
      const double F1 = rOf(s, "F"), p1 = rOf(s, "p");
      w2check(t1.rc == "0" && close(F1, a.t1F, 1e-9) && rOf(s, "df") == 1.0 &&
                  rOf(s, "df_r") == a.dfR && std::isfinite(p1) && std::fabs(p1 - a.t1P) <= 1e-8 &&
                  rOf(s, "drop") == 0.0 && close(F1, (b / se) * (b / se), 1e-9),
              at(a, a.t1Line) + "_test1: r(F) " + fmt(a.t1F) + " = (_b/_se)^2 of the session fit, "
                  "r(df) 1, r(df_r) " + fmt(a.dfR) + ", r(p) (atol 1e-8), r(drop) 0. Got rc " + t1.rc +
                  " F " + fmt(F1) + " df_r " + fmt(rOf(s, "df_r")) + " p " + fmt(p1) + " " + t1.msg);

      Reply t2 = run(s, "test x1 x2");
      w2check(t2.rc == "0" && close(rOf(s, "F"), a.t2F, 1e-9) && rOf(s, "df") == 2.0 &&
                  rOf(s, "df_r") == a.dfR,
              at(a, a.t2Line) + "_test2: r(F) " + fmt(a.t2F) + ", r(df) 2, r(df_r) " + fmt(a.dfR) +
                  ". Got rc " + t2.rc + " F " + fmt(rOf(s, "F")) + " " + t2.msg);
      Reply te = run(s, "test x1 = x2");
      w2check(te.rc == "0" && close(rOf(s, "F"), a.teqF, 1e-9) && rOf(s, "df_r") == a.dfR,
              at(a, a.teqLine) + "_testeq: r(F) " + fmt(a.teqF) + ". Got rc " + te.rc + " F " +
                  fmt(rOf(s, "F")) + " " + te.msg);
      Reply tp = run(s, "testparm x1 x2");
      w2check(tp.rc == "0" && close(rOf(s, "F"), a.tpF, 1e-9) && rOf(s, "df_r") == a.dfR,
              at(a, a.tpLine) + "_testparm: r(F) " + fmt(a.tpF) + ". Got rc " + tp.rc + " F " +
                  fmt(rOf(s, "F")) + " " + tp.msg);
      if (std::string(a.fit) == "A_cl") {
        // w2h_postest_refit.log:1141 A_cl_accum == :1010 A_cl_test2.
        Reply ac1 = run(s, "test x1");
        Reply ac2 = run(s, "test x2, accumulate");
        w2check(ac1.rc == "0" && ac2.rc == "0" && close(rOf(s, "F"), a.t2F, 1e-9) &&
                    rOf(s, "df") == 2.0 && rOf(s, "df_r") == a.dfR,
                "w2h_postest_refit.log:1141 A_cl_accum: `test x1` then `test x2, accumulate` is "
                "A_cl_test2's r(F) " + fmt(a.t2F) + ", r(df) 2, r(df_r) 14. Got rc " + ac2.rc + " F " +
                    fmt(rOf(s, "F")) + " " + ac2.msg);
      }

      Reply ld = run(s, "lincom x1 - x2");
      w2check(ld.rc == "0" && close(rOf(s, "estimate"), a.ldEst, 1e-12) &&
                  close(rOf(s, "se"), a.ldSe, 1e-9) && rOf(s, "df") == a.ldDf,
              at(a, a.ldLine) + "_lincomdiff: r(estimate) " + fmt(a.ldEst) + " (T1), r(se) " +
                  fmt(a.ldSe) + " (T2), r(df) " + fmt(a.ldDf) + ". Got rc " + ld.rc + " est " +
                  fmt(rOf(s, "estimate")) + " se " + fmt(rOf(s, "se")) + " " + ld.msg);
      Reply lx = run(s, "lincom x1");
      const double lxe = rOf(s, "estimate"), lxs = rOf(s, "se");
      w2check(lx.rc == "0" && close(lxe, b, 1e-12) && close(lxs, se, 1e-12),
              at(a, a.ldLine + 18) + "_lincomx1: r(estimate) == _b[x1] " + fmt(b) + " and r(se) == "
                  "_se[x1] " + fmt(se) + " of the session fit (1e-12). Got rc " + lx.rc + " " +
                  fmt(lxe) + " / " + fmt(lxs) + " " + lx.msg);
      // §31: a repeat in the same session answers from the handle the refit minted.
      Reply lx2 = run(s, "lincom x1");
      w2check(lx2.rc == "0" && rOf(s, "estimate") == lxe && rOf(s, "se") == lxs,
              std::string(a.fit) + ": a second `lincom x1` (the §31 handle path once minted) "
                  "repeats the first bit for bit. Handle " +
                  (s.lastModel().has_value() && !s.lastModel()->estHandle.empty() ? "minted"
                                                                                  : "not minted") +
                  ". Got rc " + lx2.rc + " " +
                  fmt(rOf(s, "estimate")) + " / " + fmt(rOf(s, "se")));

      Reply nl = run(s, "nlcom _b[x1]/_b[x2]");
      w2check(nl.rc == "0" && close(coefOf(nl.est, "_nl_1", "b"), a.nlB, 1e-12) &&
                  close(coefOf(nl.est, "_nl_1", "se"), a.nlSe, 1e-9) && rOf(s, "N") == a.nlN,
              at(a, a.nlLine) + "_nlcom: estimate " + fmt(a.nlB) + " (T1), se " + fmt(a.nlSe) +
                  " (T2), r(N) " + fmt(a.nlN) + ". Got rc " + nl.rc + " " +
                  fmt(coefOf(nl.est, "_nl_1", "b")) + " / " + fmt(coefOf(nl.est, "_nl_1", "se")) +
                  " N " + fmt(rOf(s, "N")) + " " + nl.msg);
    }
  }

  // ── FU-385 (BUG-848's refit half): the fit's multiway convention reaches the refit ──
  // w2l_vce_shape_det.do lines 11-20 (every value a small dyadic rational, exact in
  // double and float). areg's two-way post-estimation is served under
  // --keep-nested-fe-dof + --multiway-subset-factors: `lincom x1` r(se) is
  // w2l_vce_shape_det.log:619 areg_vce_c2_sex1 .0778721308909151 (main: refused
  // r(198) by the areg-cluster interim; before it, the Gmin/exempt refit). The
  // regress two-way lincom/test/testparm/nlcom arms live in
  // selftest_w2b_ivfamily.cpp (:263 regress_vce_c2_sex1); `margins, at()` after
  // that fit now runs under the fit's convention: main served reghdfe's Gmin
  // margin SE at rc 0. Reported, not counted, until a multiway-class oracle lands.
  {
    piet::SessionState s(workdir);
    seed(s, {"clear", "set obs 180", "generate id = ceil(_n/6)", "generate t = mod(_n-1,6)+1",
             "generate id2 = mod(_n,7)+1", "generate x1 = mod(_n*7,11) + mod(_n,5)/8",
             "generate z1 = mod(_n*5,13) + x1/2 + mod(_n*17,19)/16",
             "generate x2 = 0.5*z1 + mod(_n*3,5) + mod(_n*13,7)/8 + mod(_n*23,29)/32",
             "generate y = 1 + 2*x1 - x2 + id/4 + mod(_n*11,17)/4 + (mod(_n*37,41)/64 + "
             "mod(_n*53,59)/64 + mod(_n*29,31)/64 - 1)*3",
             "xtset id t"});
    Reply af = run(s, "areg y x1 x2, absorb(id) vce(cluster id id2)");
    const double b = sessionValue(s, "_b[x1]"), se = sessionValue(s, "_se[x1]");
    Reply al = run(s, "lincom x1");
    const double lse = rOf(s, "se");
    Reply at = run(s, "test x1");
    const double F = rOf(s, "F");
    Reply an = run(s, "nlcom _b[x1]");
    w2check(af.rc == "0" && close(se, .0778721308909151, 1e-9) && al.rc == "0" &&
                close(lse, se, 1e-12) && at.rc == "0" && close(F, (b / se) * (b / se), 1e-9) &&
                an.rc == "0" && close(coefOf(an.est, "_nl_1", "se"), lse, 1e-9),
            "FU-385 w2l_vce_shape_det.log:619 areg_vce_c2: after `areg y x1 x2, absorb(id) "
            "vce(cluster id id2)` lincom x1 r(se) == _se[x1] .0778721308909151, test x1 r(F) == "
            "(b/se)^2, nlcom _b[x1] se == lincom's (T2: nlcom's Jacobian is numeric) (main: "
            "r(198)). Got rc " + af.rc + "/" + al.rc + "/" + at.rc + "/" + an.rc + " se " + fmt(se) +
                " r(se) " + fmt(lse) + " F " + fmt(F) + " nlcom se " +
                fmt(coefOf(an.est, "_nl_1", "se")) +
                " " + al.msg + at.msg + an.msg);
    // regress two-way: margins, at() under the fit's V. The value is the engine's
    // `lincom _cons + x1 + mean(x2)*x2` under --multiway-subset-factors
    // (test_cli_prewave.cpp test_postest_refit_cli, which ties it to the fit's own
    // e(V)); main's Gmin refit gave .5485964528041661.
    Reply rf = run(s, "regress y x1 x2, vce(cluster id id2)");
    Reply rm = run(s, "margins, at(x1=1)");
    double mse = kNaN;
    try {
      if (rm.est.is_object() && rm.est.contains("margins") && rm.est["margins"].is_array() &&
          !rm.est["margins"].empty())
        mse = numOf(rm.est["margins"][0], "se");
    } catch (...) {
    }
    w2check(rf.rc == "0" && rm.rc == "0" && close(mse, kMwMarginSe, 1e-9),
            "FU-385: `margins, at(x1=1)` after `regress y x1 x2, vce(cluster id id2)` runs under "
            "the fit's multiway convention, se " + fmt(kMwMarginSe) + " (main: reghdfe's Gmin "
            ".5485964528041661 at rc 0). Got rc " + rm.rc + " se " + fmt(mse) + " " + rm.msg);
    // Control: a one-way cluster refit carries no multiway flag (byte-identical argv).
    Reply of = run(s, "regress y x1 x2, vce(cluster id)");
    Reply ol = run(s, "lincom x1");
    w2check(of.rc == "0" && ol.rc == "0" && close(rOf(s, "se"), sessionValue(s, "_se[x1]"), 1e-12),
            "FU-385 control: lincom x1 after one-way `regress, vce(cluster id)` still equals "
            "_se[x1]. Got rc " + ol.rc + " " + ol.msg);
    Reply a3 = run(s, "areg y x1 x2, absorb(id) vce(cluster id id2 t)");
    const double se3 = sessionValue(s, "_se[x1]");
    Reply l3 = run(s, "lincom x1");
    w2check(a3.rc == "0" && close(se3, .0720220388954095, 1e-9) && l3.rc == "0" &&
                close(rOf(s, "se"), se3, 1e-12),
            "FU-406 w2s_multiway_class.log:1165: three-way areg charges nested absorbed FE in "
            "both the fit and refit, _se[x1] .0720220388954095. Got " + fmt(se3) + " / " +
                fmt(rOf(s, "se")) + " " + a3.msg + l3.msg);
  }

  // ── w2s_multiway_class: Stata's built-in multiway cluster class ──────────
  // tests/eval/golden/aer_w2/w2s_multiway_class.do:12-35 (the w2l_vce_shape_det
  // fixture plus w, wid, cg1, cg2; every value dyadic). Each arm cites the log line
  // its value is read from; each failed on main f4624a77 (seeded RED in the report).
  {
    piet::SessionState s(workdir);
    seed(s, {"clear", "set obs 180", "generate id = ceil(_n/6)", "generate t = mod(_n-1,6)+1",
             "generate id2 = mod(_n,7)+1", "generate x1 = mod(_n*7,11) + mod(_n,5)/8",
             "generate z1 = mod(_n*5,13) + x1/2 + mod(_n*17,19)/16",
             "generate x2 = 0.5*z1 + mod(_n*3,5) + mod(_n*13,7)/8 + mod(_n*23,29)/32",
             "generate y = 1 + 2*x1 - x2 + id/4 + mod(_n*11,17)/4 + (mod(_n*37,41)/64 + "
             "mod(_n*53,59)/64 + mod(_n*29,31)/64 - 1)*3",
             "xtset id t", "generate double w = 1 + mod(_n*3,7)/4",
             "generate double wid = 1 + mod(id,4)/4", "generate cg1 = ceil(id/5)",
             "generate cg2 = mod(id,5) + 1"});
    // FU-401/FU-407: a cluster variable that varies within a panel is r(498) and
    // posts no e(): log:630-640 mw_xt_idid2_names `498||||.|.|.|.|`, :645-648 the
    // `_se[x1]` read is r(111); :774 one-way vce(cluster id2) likewise. Main
    // answered rc 0 with se(x1) .0711126484294995 / .0726672645711022.
    (void)run(s, "capture ereturn clear");
    Reply nn = run(s, "xtreg y x1 x2, fe vce(cluster id id2)");
    Reply nnSe = run(s, "display _se[x1]");
    w2check(nn.rc == "498" && nn.msg.find("panels are not nested within clusters") != std::string::npos &&
                nn.msg.find("cluster variable id2") != std::string::npos && nnSe.rc == "111",
            "w2s_multiway_class.log:640/:648 mw_xt_idid2: `xtreg y x1 x2, fe vce(cluster id id2)` "
            "is r(498) `panels are not nested within clusters` / `cluster variable id2`, and the "
            "following _se[x1] is r(111) (main: rc 0, se .0711126484294995). Got rc " + nn.rc +
                " `" + nn.msg + "`, _se rc " + nnSe.rc);
    (void)run(s, "capture ereturn clear");
    Reply n1 = run(s, "xtreg y x1 x2, fe vce(cluster id2)");
    w2check(n1.rc == "498" && n1.msg == "panels are not nested within clusters",
            "w2s_multiway_class.log:774/:777 mw_xt_c1id2: one-way `vce(cluster id2)` is r(498) with "
            "the one-line message (main: rc 0, se .0726672645711022). Got rc " + n1.rc + " `" +
                n1.msg + "`");
    // FU-386/FU-402: the nesting pair cg1 cg2 under the built-in convention
    // (log:691 `xtreg|cluster|Robust|.|4|30|180|cg1 cg2|2|`, :696 se(x1)
    // .0532737214421747; main: reghdfe's Gmin .0509313674247111, e(N_clust) 5).
    Reply xc = run(s, "xtreg y x1 x2, fe vce(cluster cg1 cg2)");
    Reply xcStr = run(s, "display \"`e(cmd)'|\" e(N_clust) \"|\" e(df_r) \"|\" e(N_g) \"|\" "
                         "e(rank) \"|`e(clustvar)'\"");
    w2check(xc.rc == "0" && close(sessionValue(s, "_se[x1]"), .0532737214421747, 1e-9) &&
                close(sessionValue(s, "_se[_cons]"), .7331067440032314, 1e-9) &&
                xcStr.text == "xtreg|.|4|30|2|cg1 cg2",
            "w2s_multiway_class.log:691/:696 mw_xt_cg: se(x1) .0532737214421747, se(_cons) "
            ".7331067440032314, `xtreg|.|4|30|2|cg1 cg2`. Got rc " + xc.rc + " se " +
                fmt(sessionValue(s, "_se[x1]")) + " `" + xcStr.text + "` " + xc.msg);
    Reply xl = run(s, "lincom x1");
    w2check(xl.rc == "0" && close(rOf(s, "se"), .0532737214421747, 1e-12) && rOf(s, "df") == 4.0,
            "FU-385 after xtreg,fe two-way: lincom x1 answers from the fit's convention, r(se) == "
            "_se[x1] (log:696), r(df) e(df_r) 4. Got rc " + xl.rc + " " + fmt(rOf(s, "se")) + " df " +
                fmt(rOf(s, "df")) + " " + xl.msg);
    Reply xa = run(s, "xtreg y x1 x2 [aw=wid], fe vce(cluster cg1 cg2)");
    w2check(xa.rc == "0" && close(sessionValue(s, "_se[x1]"), .0434670310501989, 1e-9),
            "w2s_multiway_class.log:1433 mw_xt_cg_aw_se: [aw=wid] se(x1) .0434670310501989 (main "
            ".0406848286245444). Got rc " + xa.rc + " " + fmt(sessionValue(s, "_se[x1]")) + " " + xa.msg);
    (void)run(s, "capture ereturn clear");
    Reply xw = run(s, "xtreg y x1 x2 [aw=w], fe vce(cluster cg1 cg2)");
    w2check(xw.rc == "199",
            "w2s_multiway_class.log:1381 mw_xt_cg_aw_w (control): a weight varying within id is "
            "r(199). Got rc " + xw.rc + " " + xw.msg);
    // FU-439: lincom after a two-way regress reads N - k (log:158 r(df) 177), test
    // keeps e(df_r) (log:174 r(df_r) 6). Main: r(df) 6.
    Reply rf = run(s, "regress y x1 x2, vce(cluster id id2)");
    Reply rl = run(s, "lincom x1");
    const double rlDf = rOf(s, "df"), rlSe = rOf(s, "se");
    Reply rt = run(s, "test x1");
    w2check(rf.rc == "0" && rl.rc == "0" && rlDf == 177.0 &&
                close(rlSe, .0643525900770342, 1e-9) && rt.rc == "0" && rOf(s, "df_r") == 6.0 &&
                close(rOf(s, "F"), 954.4111836269162, 1e-9),
            "w2s_multiway_class.log:158/:174 mw_reg_c2_lincom/test: r(df) 177, r(se) "
            ".0643525900770342; test F 954.4111836269162 r(df_r) 6. Got rc " + rl.rc + " df " +
                fmt(rlDf) + " se " + fmt(rlSe) + " / " + rt.rc + " F " + fmt(rOf(s, "F")) +
                " df_r " + fmt(rOf(s, "df_r")));
    // FU-404: [iw=w] two-way regress: e(N) 180 and the pweight fit's se (log:1320
    // `iweight|= w|180|.|6|.0708771701955787|`; main 316 / .0707052726893049).
    Reply iw = run(s, "regress y x1 x2 [iw=w], vce(cluster id id2)");
    w2check(iw.rc == "0" && sessionValue(s, "e(N)") == 180.0 &&
                close(sessionValue(s, "_se[x1]"), .0708771701955787, 1e-9),
            "w2s_multiway_class.log:1320 mw_reg_c2_iw: e(N) 180, se(x1) .0708771701955787. Got rc " +
                iw.rc + " N " + fmt(sessionValue(s, "e(N)")) + " se " +
                fmt(sessionValue(s, "_se[x1]")) + " " + iw.msg);
    // FU-389/FU-405: reghdfe clips the _cons row in its standardised units; the
    // slopes keep Gmin (log:960 two-way, :1036 three-way; main .4539871108580628 and
    // .3263806138476747).
    Reply h2 = run(s, "reghdfe y x1 x2, absorb(id) vce(cluster id id2)");
    w2check(h2.rc == "0" && close(sessionValue(s, "_se[x1]"), .0711126484294995, 1e-9) &&
                close(sessionValue(s, "_se[_cons]"), .4540988323750039, 1e-9),
            "w2s_multiway_class.log:960 mw_hdfe_c2_se: se(x1) .0711126484294995 (Gmin, unchanged), "
            "se(_cons) .4540988323750039. Got rc " + h2.rc + " " + fmt(sessionValue(s, "_se[_cons]")));
    Reply h3 = run(s, "reghdfe y x1 x2, absorb(id) vce(cluster id id2 t)");
    w2check(h3.rc == "0" && close(sessionValue(s, "_se[_cons]"), .3268800883326979, 1e-9),
            "w2s_multiway_class.log:1036 mw_hdfe_c3: se(_cons) .3268800883326979. Got rc " + h3.rc +
                " " + fmt(sessionValue(s, "_se[_cons]")));
    // FU-385/FU-387: lincom after a two-way ivregress is served from the fit's V,
    // normal reference (log:840 `0|fit=0|1.983838860024832|.0503059336995026|.|`;
    // main: r(198) "not supported after `iv`").
    Reply iv = run(s, "ivregress 2sls y x1 (x2 = z1), vce(cluster id id2)");
    Reply il = run(s, "lincom x1");
    Reply ilDf = run(s, "display r(df)");
    w2check(iv.rc == "0" && il.rc == "0" && close(rOf(s, "estimate"), 1.983838860024832, 1e-12) &&
                close(rOf(s, "se"), .0503059336995026, 1e-9) && ilDf.text == ".",
            "w2s_multiway_class.log:840 mw_iv_c2_lincom: rc 0, r(estimate) 1.983838860024832, "
            "r(se) .0503059336995026, r(df) `.`. Got rc " + il.rc + " " + fmt(rOf(s, "estimate")) +
                " " + fmt(rOf(s, "se")) + " df `" + ilDf.text + "` " + il.msg);
  }

  // ── BUG-568 / BUG-569 on w2b_estim.do's own fixture ─────────────────────
  // w2b_estim.do generates FLOAT columns (Stata's default storage), and PIET has
  // neither float storage nor float(), so each value is rounded to float here and
  // loaded as a literal -- the same rounding test_cli_prewave.cpp's areg_a_r
  // control uses. Unrounded doubles miss the oracle at ~4e-8 (outside T2).
  {
    piet::SessionState s(workdir);
    (void)piet::dispatch("clear", s);
    (void)piet::dispatch("set obs 60", s);
    (void)piet::dispatch("generate double id = ceil(_n/6)", s);
    (void)piet::dispatch("generate double t = mod(_n-1,6)+1", s);
    (void)piet::dispatch("generate double x1 = .", s);
    (void)piet::dispatch("generate double x2 = .", s);
    (void)piet::dispatch("generate double y = .", s);
    auto fl = [](double v) { return static_cast<double>(static_cast<float>(v)); };
    for (int n = 1; n <= 60; ++n) {
      const double id = std::ceil(n / 6.0);
      const double x1 = fl(static_cast<double>((n * 7) % 11) + static_cast<double>(n % 5) / 10.0);
      const double x2 = fl(static_cast<double>((n * 3) % 5) + static_cast<double>((n * 13) % 7) / 10.0);
      const double y =
          fl(1.0 + 2.0 * x1 - x2 + id / 3.0 + static_cast<double>((n * 11) % 17) / 5.0);
      const std::string in = " in " + std::to_string(n);
      (void)piet::dispatch("quietly replace x1 = " + fmt(x1) + in, s);
      (void)piet::dispatch("quietly replace x2 = " + fmt(x2) + in, s);
      (void)piet::dispatch("quietly replace y = " + fmt(y) + in, s);
    }

    // BUG-569: `a(id) cl(id)` is the oracle's own spelling (main: "option `a` not
    // allowed").
    Reply acl = run(s, "areg y x1 x2, a(id) cl(id)");
    Reply aclStr = run(s, "display e(N_clust) \"|\" e(df_r) \"|`e(vce)'|\"");
    w2check(acl.rc == "0" && close(coefOf(acl.est, "x1", "se"), 0.0722686736191015, 1e-9),
            "w2b_estim areg_a_cl: `areg y x1 x2, a(id) cl(id)` se_x1 .0722686736191015 (main: "
            "refused on `a`; with absorb() spelled out, the reghdfe number .0663182832). Got rc " +
                acl.rc + " se=" + fmt(coefOf(acl.est, "x1", "se")) + " " + acl.msg);
    w2check(aclStr.text == "10|9|cluster|",
            "w2b_estim areg_a_cl: e(N_clust) 10, e(df_r) 9 (= G-1), e(vce) cluster. Got `" +
                aclStr.text + "`");
    Reply avc = run(s, "areg y x1 x2, absorb(id) vce(cluster id)");
    w2check(avc.rc == "0" && close(coefOf(avc.est, "x1", "se"), 0.0722686736191015, 1e-9) &&
                close(coefOf(avc.est, "_cons", "se"), 0.4823829775414377, 1e-9),
            "w2b_estim areg_vce_cluster: se_x1 .0722686736191015, se_cons .4823829775414377 "
            "(main: .0663182832 / .442664979). Got rc " + avc.rc + " se=" +
                fmt(coefOf(avc.est, "x1", "se")) + " se_cons=" + fmt(coefOf(avc.est, "_cons", "se")));
    for (const char* sp : {"ab(id)", "abs(id)"}) {
      Reply r = run(s, std::string("areg y x1 x2, ") + sp + " cluster(id)");
      w2check(r.rc == "0" && close(coefOf(r.est, "x1", "se"), 0.0722686736191015, 1e-9),
              std::string("w2b_estim areg_") + sp + " + areg_absorb_cluster: the `" + sp +
                  "` floor (rc 0 in the oracle) with legacy cluster(id). Got rc " + r.rc + " " +
                  r.msg);
    }
    Reply ar = run(s, "areg y x1 x2, a(id) r");
    w2check(ar.rc == "0" && close(coefOf(ar.est, "x1", "se"), 0.0386296057774394, 1e-9),
            "w2b_estim areg_a_r: `a(id) r` is robust, se_x1 .0386296057774394 (main: refused on "
            "`a`). Got rc " + ar.rc + " se=" + fmt(coefOf(ar.est, "x1", "se")) + " " + ar.msg);
    Reply ann = run(s, "areg y x1 x2, absorb(id) vce(cluster t)");
    Reply annStr = run(s, "display e(df_r)");
    w2check(ann.rc == "0" && close(coefOf(ann.est, "x1", "se"), 0.0214849883739092, 1e-9) &&
                annStr.text == "5",
            "w2b_estim areg_cluster_nonnested (unchanged): se_x1 .0214849883739092, df_r 5 -- a "
            "non-nested cluster has nothing to exempt. Got rc " + ann.rc + " se=" +
                fmt(coefOf(ann.est, "x1", "se")) + " df_r=" + annStr.text);
    // BUG-604 (engine-multiway-cluster3): three-way clustering, w2b_estim.log:1410
    // PREC|reghdfe_cluster3|0| .0575950937310231|3|3| and its two-way control
    // :1447 PREC|reghdfe_cluster2_ctl|0|  .060463723825293|5|. Same float-rounded
    // fixture as above (recast is metadata-only here, so it cannot round). Main
    // refused three names; e(vce)/e(vcetype)/e(clustvar) of the fit are pinned below
    // against the display of w2b_estim.log:1385-1391 and the reghdfe class arms
    // w2l_vce_shape.log:3822/:3871 (`cluster|Robust`).
    {
      (void)piet::dispatch("generate double c3 = mod(_n,4)", s);
      Reply c3 = run(s, "reghdfe y x1 x2, absorb(id) cluster(id t c3)");
      Reply c3Str = run(s, "display e(N_clustervars) \"|\" e(df_r) \"|\" e(N_clust) \"|\"");
      Reply c3By = run(s, "display e(N_clust1) \"|\" e(N_clust2) \"|\" e(N_clust3) \"|\"");
      w2check(c3.rc == "0" && close(coefOf(c3.est, "x1", "se"), 0.0575950937310231, 1e-12),
              "w2b_estim reghdfe_cluster3: `reghdfe y x1 x2, absorb(id) cluster(id t c3)` rc 0, "
              "se_x1 .0575950937310231 (T2). Main: rc 198-class refusal. Got rc " + c3.rc +
                  " se=" + fmt(coefOf(c3.est, "x1", "se")) + " " + c3.msg);
      w2check(c3Str.text == "3|3|4|",
              "w2b_estim reghdfe_cluster3: e(N_clustervars) 3, e(df_r) 3 (= Gmin-1; c3 has 4 "
              "levels, so e(N_clust) 4). Got `" + c3Str.text + "`");
      w2check(c3By.text == "10|6|4|",
              "reghdfe e(N_clust1..3) are the per-dimension G in cluster() order (10 id, 6 t, "
              "4 c3; unpinned by a PREC arm). Got `" + c3By.text + "`");
      // e() triple, w2b_estim.log:1385-1391 (one header line per e(clustvar) token,
      // `Robust`) and reghdfe's own posting (e(clustvar) = the cluster names, in order).
      const std::string kTriple = "display \"`e(vce)'|`e(vcetype)'|`e(clustvar)'\"";
      const std::string kWant = "cluster|Robust|id t c3";
      Reply c3Tri = run(s, kTriple);
      w2check(c3Tri.text == kWant,
              "BUG-604 reghdfe_cluster3: e(vce)|e(vcetype)|e(clustvar) after the fit is `" +
                  kWant + "` (w2b_estim.log:1385-1391; main refused the fit, 952acb2d said "
                  "`multiway||`). Got `" + c3Tri.text + "`");
      Reply lc = run(s, "lincom x1");
      Reply c3Lin = run(s, kTriple);
      w2check(lc.rc == "0" && c3Lin.text == kWant,
              "BUG-604: the e() triple survives `lincom x1` (setEstHandleM4 re-publishes through "
              "setLastModel, so a post-fit override would revert). Got rc " + lc.rc + " `" +
                  c3Lin.text + "`");
      Reply st = run(s, "estimates store c3m");
      Reply other = run(s, "regress y x1 x2");
      Reply rs = run(s, "estimates restore c3m");
      Reply c3Rst = run(s, kTriple);
      w2check(st.rc == "0" && other.rc == "0" && rs.rc == "0" && c3Rst.text == kWant,
              "BUG-604: the e() triple after `estimates store` / other fit / `estimates restore` "
              "is `" + kWant + "` (invariance arm: Stata reposts the stored e(); no captured "
              "oracle for restore). Got `" + c3Rst.text + "` rc " + st.rc + "/" + other.rc + "/" +
                  rs.rc);
      Reply c3v = run(s, "reghdfe y x1 x2, absorb(id) vce(cluster id t c3)");
      w2check(c3v.rc == "0" && close(coefOf(c3v.est, "x1", "se"), 0.0575950937310231, 1e-12),
              "reghdfe_cluster3 via vce(cluster id t c3): same fit as cluster(id t c3). Got rc " +
                  c3v.rc + " se=" + fmt(coefOf(c3v.est, "x1", "se")) + " " + c3v.msg);
      Reply c2 = run(s, "reghdfe y x1 x2, absorb(id) cluster(id t)");
      Reply c2Str = run(s, "display e(df_r)");
      w2check(c2.rc == "0" && close(coefOf(c2.est, "x1", "se"), 0.060463723825293, 1e-12) &&
                  c2Str.text == "5",
              "w2b_estim reghdfe_cluster2_ctl (control): se_x1 .060463723825293, df_r 5. Got rc " +
                  c2.rc + " se=" + fmt(coefOf(c2.est, "x1", "se")) + " df_r=" + c2Str.text);
      // Control arms: commands without k>=3 support refuse with a non-1 rc and the
      // text that no longer claims three-way clustering is unimplemented.
      Reply rgL = run(s, "regress y x1 x2, cluster(id t c3)");
      w2check(rgL.rc == "198",
              "control regress legacy cluster(id t c3): r(198), class regress_opt_c2 "
              "(w2l_vce_shape.log:268-275). Got rc " + rgL.rc + " " + rgL.msg);
      Reply arL = run(s, "areg y x1 x2, absorb(id) cluster(id t c3)");
      w2check(arL.rc != "0" && arL.rc != "1",
              "control areg legacy cluster(id t c3): refused with a non-1 rc (class areg_opt_c2 is "
              "r(103), w2l_vce_shape.log:624-630; the exact rc is owned by FU-160). Got rc " +
                  arL.rc + " " + arL.msg);
      // FU-C3-B closed: Stata 19 runs vce(cluster a b c) as the built-in
      // multiway VCE at rc 0 (w2l_vce_shape.log:252/:608; w2s_multiway_class.log
      // :1115 mw_reg_c3_names, :1165 mw_areg_c3).
      for (const std::string& fit : {std::string("regress y x1 x2, vce(cluster id t c3)"),
                                     std::string("areg y x1 x2, absorb(id) vce(cluster id t c3)")}) {
        Reply r = run(s, fit);
        w2check(r.rc == "0",
                "regress/areg vce(cluster a b c) runs as multiway at rc 0 "
                "(w2l_vce_shape.log:252/:608): `" + fit + "`. Got rc " + r.rc + " " + r.msg);
      }
    }
    Reply hc = run(s, "reghdfe y x1 x2, absorb(id) vce(cluster id)");
    Reply xc = run(s, "xtset id t");
    (void)xc;
    Reply xt = run(s, "xtreg y x1 x2, fe vce(cluster id)");
    w2check(hc.rc == "0" && close(coefOf(hc.est, "x1", "se"), 0.066318283176713, 1e-9) &&
                xt.rc == "0" && close(coefOf(xt.est, "x1", "se"), 0.0663182831767131, 1e-9),
            "w2b_estim reghdfe_cluster_ctl / xtreg_fe_cluster_ctl (controls): the SAME fit keeps "
            ".0663182832 under reghdfe and xtreg,fe. Got reghdfe rc " + hc.rc + " se=" +
                fmt(coefOf(hc.est, "x1", "se")) + ", xtreg rc " + xt.rc + " se=" +
                fmt(coefOf(xt.est, "x1", "se")));
    // e(df_a) after areg / reghdfe (handlers.cpp absorbedDofA). w2b_estim.log PREC
    // areg_a(id)/ab(id)/abs(id)/absorb(id): `...|48|9|` (e(df_r) 48, e(df_a) 9 =
    // 10 categories - 1), se_x1 .043868730371399. Main: e(df_a) `.`.
    for (const char* sp : {"a(id)", "ab(id)", "abs(id)", "absorb(id)"}) {
      Reply r = run(s, std::string("areg y x1 x2, ") + sp);
      Reply d = run(s, "display e(df_r) \"|\" e(df_a)");
      w2check(r.rc == "0" && d.text == "48|9" &&
                  close(coefOf(r.est, "x1", "se"), 0.043868730371399, 1e-9),
              std::string("w2b_estim areg_") + sp + ": e(df_r) 48, e(df_a) 9, se_x1 "
              ".043868730371399 (main: e(df_a) `.`). Got rc " + r.rc + " `" + d.text + "` se=" +
                  fmt(coefOf(r.est, "x1", "se")) + " " + r.msg);
    }
    // w2b_estim.log PREC reghdfe_cluster_ctl `|.066318283176713|9|0|`: id is nested
    // in the cluster, so all 10 categories are redundant and e(df_a) is 0 (main: `.`).
    Reply hcd = run(s, "reghdfe y x1 x2, absorb(id) vce(cluster id)");
    Reply hcdS = run(s, "display e(df_r) \"|\" e(df_a)");
    w2check(hcd.rc == "0" && hcdS.text == "9|0" &&
                close(coefOf(hcd.est, "x1", "se"), 0.066318283176713, 1e-9),
            "w2b_estim reghdfe_cluster_ctl: e(df_r) 9, e(df_a) 0 -- an FE nested in the cluster "
            "is all redundant (main: e(df_a) `.`). Got rc " + hcd.rc + " `" + hcdS.text + "`");
    // Control: areg under the same nested cluster still charges the FE -- its
    // SE (areg_a_cl) is unchanged and e(df_a) is still areg's 9.
    Reply acd = run(s, "areg y x1 x2, a(id) cl(id)");
    Reply acdS = run(s, "display e(df_r) \"|\" e(df_a)");
    w2check(acd.rc == "0" && acdS.text == "9|9" &&
                close(coefOf(acd.est, "x1", "se"), 0.0722686736191015, 1e-9),
            "w2b_estim areg_a_cl (control): se_x1 .0722686736191015 and e(df_r) 9 unchanged; "
            "areg's e(df_a) stays levels - 1 = 9 under the nested cluster. Got rc " + acd.rc +
                " `" + acdS.text + "`");
    // regress posts no e(df_a) (Stata's regress has none).
    (void)run(s, "regress y x1 x2");
    Reply rgd = run(s, "display e(df_a)");
    w2check(rgd.text == ".", "regress: e(df_a) stays missing. Got `" + rgd.text + "`");
    Reply bad = run(s, "areg y x1 x2, c(id) absorb(id)");
    w2check(bad.rc == "198",
            "BUG-569 floor: `c(id)` is below the measured cluster floor (`cl`) and stays an "
            "unknown option, r(198). Got rc " + bad.rc + " " + bad.msg);

    // BUG-569 review round: the value SHAPE of cluster/robust. `cluster` takes a
    // variable and `robust` takes none; a malformed form is Stata's r(198). No
    // golden captures these spellings, so the arms pin the rc only (not a
    // message). On this branch before the fix, each one fitted iid at rc 0: the
    // abbreviation rewrite passed `cl`/`clu`/`rob(id)` on as `cluster`/`robust`,
    // and every VCE loop skipped the malformed value.
    (void)run(s, "generate double yb = y > 12");
    (void)run(s, "generate double cnt = mod(_n,4)");
    for (const std::string& cmd :
         {std::string("areg y x1 x2, a(id) cl"), std::string("areg y x1 x2, a(id) clu"),
          std::string("areg y x1 x2, a(id) rob(id)"),
          std::string("regress y x1 x2, cluster"), std::string("regress y x1 x2, robust(id)"),
          std::string("reghdfe y x1 x2, absorb(id) cluster"),
          std::string("xtreg y x1 x2, fe cluster"), std::string("xtreg y x1 x2, fe robust(id)"),
          std::string("logit yb x1 x2, cluster"), std::string("probit yb x1 x2, robust(id)"),
          std::string("poisson cnt x1 x2, cluster"),
          std::string("ivregress 2sls y (x1 = x2), cluster"),
          std::string("ivreg2 y (x1 = x2), cl")}) {
      Reply r = run(s, cmd);
      const bool names = r.msg.find("cluster") != std::string::npos ||
                         r.msg.find("robust") != std::string::npos;
      w2check(r.rc == "198" && names,
              "BUG-569 value shape: `" + cmd + "` is a malformed cluster/robust option and must "
              "refuse with r(198), not fit iid at rc 0. Got rc " + r.rc + " " + r.msg);
    }
    // Controls: the well-formed spellings still fit (w2b_estim areg_a_cl, areg_a_r),
    // and areg ignores an EMPTY cluster() (w2l_vce_shape_det.log:695
    // areg_empty_cluster `0|areg|ols||.|180`).
    for (const std::string& cmd :
         {std::string("areg y x1 x2, a(id) cl(id)"), std::string("areg y x1 x2, a(id) rob"),
          std::string("areg y x1 x2, a(id) cluster()"),
          std::string("logit yb x1 x2, cluster(id)"), std::string("probit yb x1 x2, robust")}) {
      Reply r = run(s, cmd);
      w2check(r.rc == "0", "BUG-569 value-shape control: `" + cmd + "` still fits. Got rc " +
                               r.rc + " " + r.msg);
    }
  }

  // ── e(df_a) after reghdfe: g11 and aer_b4 predict-e-xbd ──────────────────
  // reghdfe's e(df_a) = its "Num. Coefs" column summed, which depends only on the
  // FE level structure and the estimation sample, so each arm rebuilds the
  // oracle's structure with deterministic regressors.
  {
    // g11_fe_header_scalars.log: panel_synth_full.csv is a balanced 100 id x 8 t
    // panel (N 800, no missing). absorb(id): e(df_a) 1.00000000000000e+02, F(2,698);
    // absorb(id t): Num. Coefs 100 + 7 = 107, F(2,691). Main: e(df_a) `.`.
    piet::SessionState s(workdir);
    seed(s, {"clear", "set obs 800", "generate id = ceil(_n/8)", "generate t = mod(_n-1,8)+1",
             "generate double x1 = mod(_n*7,11)/3 + mod(_n,5)",
             "generate double x2 = mod(_n*13,17)/4 - t/3 + mod(_n*3,7)/5",
             "generate double y = 1 + x1 - x2 + id/50 + mod(_n*11,19)/7"});
    Reply a = run(s, "reghdfe y x1 x2, absorb(id) vce(unadjusted)");
    Reply aS = run(s, "display e(N) \"|\" e(df_r) \"|\" e(df_a)");
    w2check(a.rc == "0" && aS.text == "800|698|100",
            "g11 reghdfe absorb(id) vce(unadjusted): e(N) 800, e(df_r) 698, e(df_a) 100 (main: "
            "`.`). Got rc " + a.rc + " `" + aS.text + "` " + a.msg);
    // `vce(unadjusted)` is reghdfe's conventional VCE (g11 / reghdfe_absorb_vce.log
    // run it at rc 0): the same SE as the default fit. Main: "Unknown vce type" r(198).
    Reply dflt = run(s, "reghdfe y x1 x2, absorb(id)");
    w2check(a.rc == "0" && dflt.rc == "0" &&
                close(coefOf(a.est, "x1", "se"), coefOf(dflt.est, "x1", "se"), 1e-14),
            "g11 reghdfe vce(unadjusted): accepted, SE identical to the default iid fit. Got rc " +
                a.rc + " se=" + fmt(coefOf(a.est, "x1", "se")) + " vs " +
                fmt(coefOf(dflt.est, "x1", "se")));
    Reply b = run(s, "reghdfe y x1 x2, absorb(id t) vce(unadjusted)");
    Reply bS = run(s, "display e(N) \"|\" e(df_r) \"|\" e(df_a)");
    w2check(b.rc == "0" && bS.text == "800|691|107",
            "g11 reghdfe absorb(id t) vce(unadjusted): e(df_r) 691, e(df_a) 107 = 100 + (8 - 1 "
            "redundant) (main: `.`). Got rc " + b.rc + " `" + bS.text + "` " + b.msg);
  }
  {
    // aer_b4/predict-e-xbd.do's id/f2 structure: rows 101-125 pooled into id 900,
    // row 199 alone in id 998 (a singleton reghdfe drops), id 8 folded into 7, y
    // missing in rows 1-3. predict-e-xbd.log:348 after `reghdfe y x, absorb(id f2)
    // res`: N 236, df_a 49 (id 43 + f2 7 - 1 redundant), F(1,186). Main: `.`.
    piet::SessionState s(workdir);
    seed(s, {"clear", "set obs 240",
             "generate id = cond(inrange(_n,101,125),900,cond(_n==199,998,cond(ceil(_n/5)==8,7,"
             "ceil(_n/5))))",
             "generate f2 = mod(_n,7)", "generate double x = mod(_n*7,11)/3 + mod(_n,4)",
             "generate double y = cond(_n<=3,.,1 + .7*x + .3*id/100 + .2*f2 + mod(_n*13,17)/5)"});
    Reply r = run(s, "reghdfe y x, absorb(id f2)");
    Reply rS = run(s, "display e(N) \"|\" e(df_r) \"|\" e(df_a)");
    w2check(r.rc == "0" && rS.text == "236|186|49",
            "aer_b4 predict-e-xbd O_res: e(N) 236, e(df_r) 186, e(df_a) 49 after the singleton "
            "drop (main: e(df_a) `.`). Got rc " + r.rc + " `" + rS.text + "` " + r.msg);
  }

  // ── w2b: logit/probit VCE spellings (w2b-estim-vce-options) ──────────────
  // tests/eval/golden/aer_w2/w2b_estim.do's frame. The do-file's untyped
  // generates are FLOAT in Stata and this build stores doubles, so b/se carry a
  // ~1e-8 storage gap here; the arms below assert the e() macros, the option
  // spelling and PIET-internal identities, not the oracle's digits.
  {
    piet::SessionState s(workdir);
    seed(s, {"clear", "set obs 60", "generate id = ceil(_n/6)", "generate t = mod(_n-1,6)+1",
             "generate double x1 = mod(_n*7,11) + mod(_n,5)/10",
             "generate double x2 = mod(_n*3,5) + mod(_n*13,7)/10",
             "generate double y = 1 + 2*x1 - x2 + id/3 + mod(_n*11,17)/5",
             "generate byte d = (y > 12)", "xtset id t"});
    const char* kVceStr = "display \"`e(vce)'|`e(vcetype)'\"";

    // FU-30: the default VCE of an ML fit is `oim` (w2c_ml_getmata.log:321
    // ml_pw_vceoim, :356 ml_opts_nospace -- the same probit likelihood through
    // `ml`). Main: `iid|`, the engine's spelling.
    Reply lg = run(s, "logit d x1 x2");
    Reply lgStr = run(s, kVceStr);
    w2check(lg.rc == "0" && lgStr.text == "oim|",
            "w2b logit_ctl / FU-30: e(vce)|e(vcetype) after a default logit read `oim|` (main: "
            "`iid|`). Got rc " + lg.rc + " `" + lgStr.text + "` " + lg.msg);
    Reply pb = run(s, "probit d x1 x2");
    Reply pbStr = run(s, kVceStr);
    w2check(pb.rc == "0" && pbStr.text == "oim|",
            "w2b probit_ctl / FU-30: e(vce)|e(vcetype) after a default probit read `oim|` (main: "
            "`iid|`). Got rc " + pb.rc + " `" + pbStr.text + "` " + pb.msg);
    // Control: probit_ctl's numbers (w2b_estim.log:762 _b[x1] 1.355178493014946,
    // _se[x1] .4459396018157188 -- observed information) hold at T3 (rtol 1e-7)
    // on the double frame; measured on main 2.1e-9 and 2.7e-8.
    w2check(close(coefOf(pb.est, "x1", "b"), 1.355178493014946, 1e-7) &&
                close(coefOf(pb.est, "x1", "se"), .4459396018157188, 1e-7),
            "w2b probit_ctl control: b_x1/se_x1 within T3 of the oracle. Got b=" +
                fmt(coefOf(pb.est, "x1", "b")) + " se=" + fmt(coefOf(pb.est, "x1", "se")));

    // Controls: the robust/cluster spellings are unchanged (w2b_estim.log:619
    // logit_vce_robust `robust`; :679 logit_vce_cluster e(N_clust) 10).
    Reply lr = run(s, "logit d x1 x2, vce(robust)");
    Reply lrStr = run(s, kVceStr);
    w2check(lr.rc == "0" && lrStr.text == "robust|Robust",
            "w2b logit_vce_robust control: `robust|Robust`. Got rc " + lr.rc + " `" + lrStr.text +
                "`");
    Reply lc = run(s, "logit d x1 x2, vce(cluster id)");
    Reply lcStr = run(s, "display \"`e(vce)'|`e(vcetype)'|\" e(N_clust)");
    w2check(lc.rc == "0" && lcStr.text == "cluster|Robust|10",
            "w2b logit_vce_cluster control: `cluster|Robust|10`. Got rc " + lc.rc + " `" +
                lcStr.text + "`");

    // probit's `robust` abbreviates to `r` (w2b_estim.log:706 `probit d x1 x2, r
    // nocons` rc 0, "Robust std. err."). Main: rc 198 "option `r` not allowed".
    // The same fit as the full spelling, to the last bit.
    Reply pr = run(s, "probit d x1 x2, r");
    Reply prStr = run(s, kVceStr);
    Reply pv = run(s, "probit d x1 x2, vce(robust)");
    w2check(pr.rc == "0" && prStr.text == "robust|Robust" &&
                close(coefOf(pr.est, "x1", "se"), coefOf(pv.est, "x1", "se"), 1e-14) &&
                !close(coefOf(pr.est, "x1", "se"), coefOf(pb.est, "x1", "se"), 1e-6),
            "w2b probit_r: `probit ..., r` is vce(robust) -- rc 0, `robust|Robust`, the vce(robust) "
            "se and not the oim one (main: rc 198 option r not allowed). Got rc " + pr.rc + " `" +
                prStr.text + "` se=" + fmt(coefOf(pr.est, "x1", "se")) + " vs " +
                fmt(coefOf(pv.est, "x1", "se")) + " " + pr.msg);
  }

  // ── w2b: qreg vce(robust), probit/logit noconstant (w2b-estim-vce-options) ─
  // w2b_estim.do's frame with its FLOAT storage replicated: each value is
  // rounded to float and loaded as a literal (the BUG-568 block's recipe), so
  // the oracle's digits are reachable. w = 1 + mod(_n,4) and d = (y > 12) are
  // exact in float.
  {
    piet::SessionState s(workdir);
    (void)piet::dispatch("clear", s);
    (void)piet::dispatch("set obs 60", s);
    (void)piet::dispatch("generate double id = ceil(_n/6)", s);
    (void)piet::dispatch("generate double w = 1 + mod(_n,4)", s);
    (void)piet::dispatch("generate double x1 = .", s);
    (void)piet::dispatch("generate double x2 = .", s);
    (void)piet::dispatch("generate double y = .", s);
    auto fl = [](double v) { return static_cast<double>(static_cast<float>(v)); };
    for (int n = 1; n <= 60; ++n) {
      const double id = std::ceil(n / 6.0);
      const double x1 = fl(static_cast<double>((n * 7) % 11) + static_cast<double>(n % 5) / 10.0);
      const double x2 = fl(static_cast<double>((n * 3) % 5) + static_cast<double>((n * 13) % 7) / 10.0);
      const double y =
          fl(1.0 + 2.0 * x1 - x2 + id / 3.0 + static_cast<double>((n * 11) % 17) / 5.0);
      const std::string in = " in " + std::to_string(n);
      (void)piet::dispatch("quietly replace x1 = " + fmt(x1) + in, s);
      (void)piet::dispatch("quietly replace x2 = " + fmt(x2) + in, s);
      (void)piet::dispatch("quietly replace y = " + fmt(y) + in, s);
    }
    (void)piet::dispatch("generate byte d = (y > 12)", s);
    const char* kVceStr = "display \"`e(vce)'|`e(vcetype)'\"";

    // qreg_vce_robust, w2b_estim.log:818 `|2.010232206078236|.0805316862193467|`
    // and the table's "Robust" header. T4 (tolnote: qreg's fitted-density
    // sandwich, a bandwidth-discretized quantity). Main: rc 198 "option `vce`
    // not allowed" (kVocabQreg {quantile, level}).
    Reply qr = run(s, "qreg y x1 x2, quantile(0.5) vce(robust)");
    Reply qrStr = run(s, kVceStr);
    w2check(qr.rc == "0" && close(coefOf(qr.est, "x1", "b"), 2.010232206078236, 1e-9) &&
                close(coefOf(qr.est, "x1", "se"), .0805316862193467, 1e-6) &&
                qrStr.text == "robust|Robust",
            "w2b qreg_vce_robust: rc 0, b_x1 2.010232206078236, se_x1 .0805316862193467, "
            "`robust|Robust` (main: rc 198 option vce not allowed). Got rc " + qr.rc + " b=" +
                fmt(coefOf(qr.est, "x1", "b")) + " se=" + fmt(coefOf(qr.est, "x1", "se")) + " `" +
                qrStr.text + "` " + qr.msg);
    // Control: qreg_ctl, :844 `|.0843024787277759|iid|` -- the default VCE and
    // its e(vce) are unchanged; vce(iid) spelled out is the same fit.
    Reply qc = run(s, "qreg y x1 x2, quantile(0.5)");
    Reply qcStr = run(s, kVceStr);
    Reply qi = run(s, "qreg y x1 x2, quantile(0.5) vce(iid)");
    w2check(qc.rc == "0" && close(coefOf(qc.est, "x1", "se"), .0843024787277759, 1e-6) &&
                qcStr.text == "iid|" && qi.rc == "0" &&
                coefOf(qi.est, "x1", "se") == coefOf(qc.est, "x1", "se"),
            "w2b qreg_ctl control: se_x1 .0843024787277759, `iid|`, and vce(iid) is the same "
            "fit. Got rc " + qc.rc + " se=" + fmt(coefOf(qc.est, "x1", "se")) + " `" +
                qcStr.text + "` vce(iid) rc " + qi.rc + " se=" + fmt(coefOf(qi.est, "x1", "se")));
    // qreg_pw_vce_robust, w2b_estim.log:791 `|2.017204446266646|.0854069206829948|robust|`
    // (T4). Main: rc 198 from the tokenizer weight gate (registry qreg row was
    // WeightPolicy::None), before runQreg ran.
    Reply qpw = run(s, "qreg y x1 x2 [pw=w], quantile(0.5) vce(robust)");
    Reply qpwStr = run(s, kVceStr);
    w2check(qpw.rc == "0" && close(coefOf(qpw.est, "x1", "b"), 2.017204446266646, 1e-9) &&
                close(coefOf(qpw.est, "x1", "se"), .0854069206829948, 1e-6) &&
                qpwStr.text == "robust|Robust",
            "w2b qreg_pw_vce_robust: rc 0, b_x1 2.017204446266646, se_x1 .0854069206829948, "
            "`robust|Robust` (main: rc 198 qreg does not support weighted estimation). Got rc " +
                qpw.rc + " b=" + fmt(coefOf(qpw.est, "x1", "b")) + " se=" +
                fmt(coefOf(qpw.est, "x1", "se")) + " `" + qpwStr.text + "` " + qpw.msg);
    // FU-106, w2p_estim_ev.log A_qreg_pw_q50_novce :299 / A_qreg_pw_q25_novce :333
    // == A_qreg_pw_q50_robust :364 / A_qreg_pw_q25_robust :398 in every field: a
    // pweighted qreg WITHOUT vce() runs at rc 0 as vce(robust) (pweights imply
    // robust). Main refused it ("without vce(robust) is not supported"). Compared
    // against this build's own explicit vce(robust) fit, bit-for-bit.
    for (const char* q : {"0.5", "0.25"}) {
      const std::string qs = std::string("quantile(") + q + ")";
      Reply nv = run(s, "qreg y x1 x2 [pw=w], " + qs);
      Reply nvStr = run(s, kVceStr);
      Reply ex = run(s, "qreg y x1 x2 [pw=w], " + qs + " vce(robust)");
      w2check(nv.rc == "0" && ex.rc == "0" && nvStr.text == "robust|Robust" &&
                  coefOf(nv.est, "x1", "b") == coefOf(ex.est, "x1", "b") &&
                  coefOf(nv.est, "x1", "se") == coefOf(ex.est, "x1", "se") &&
                  coefOf(nv.est, "x1", "se") > 0,
              std::string("w2p qreg_pw_novce ") + q + ": rc 0, `robust|Robust`, b/se bit-identical to "
              "the explicit vce(robust) fit (A_qreg_pw_q*_novce == A_qreg_pw_q*_robust; main: "
              "refused). Got rc " + nv.rc + " `" + nvStr.text + "` se=" +
                  fmt(coefOf(nv.est, "x1", "se")) + " vs " + fmt(coefOf(ex.est, "x1", "se")) +
                  " " + nv.msg);
    }
    // A_qreg_pw_q50_iid :405: `qreg [pw=w] vce(iid)` is r(404), message verbatim,
    // and no model is left behind to read. Main fitted it (rc 0, iid VCE).
    Reply qpi = run(s, "qreg y x1 x2 [pw=w], quantile(0.5) vce(iid)");
    (void)run(s, "capture qreg y x1 x2 [pw=w], quantile(0.5) vce(iid)");
    Reply qpiRc2 = run(s, "display _rc");
    w2check(qpi.rc == "404" && !qpi.est.is_object() &&
                qpi.msg.find("option vce(iid) is not possible with pweighted data; pweights "
                             "imply robust") != std::string::npos &&
                qpiRc2.text == "404",
            "w2p qreg_pw_iid: r(404) `option vce(iid) is not possible with pweighted data; "
            "pweights imply robust` and `display _rc` 404 (A_qreg_pw_q50_iid :405/:411; main: "
            "rc 0). Got rc " + qpi.rc + " _rc `" + qpiRc2.text + "` " + qpi.msg);
    // FU-184, w2p_estim_ev.log :104/:170/:203: e(q) is the quantile (.25/.5/.75),
    // on the robust and the default VCE alike. Main printed `.`.
    {
      Reply q25 = run(s, "qreg y x1 x2, quantile(0.25) vce(robust)");
      Reply q25s = run(s, "display e(q)");
      Reply q50 = run(s, "qreg y x1 x2, quantile(0.5)");
      Reply q50s = run(s, "display e(q)");
      Reply q75 = run(s, "qreg y x1 x2, quantile(0.75) vce(iid)");
      Reply q75s = run(s, "display e(q)");
      Reply qp = run(s, "qreg y x1 x2 [pw=w], quantile(25)");
      Reply qps = run(s, "display e(q)");
      w2check(q25.rc == "0" && q25s.text == ".25" && q50.rc == "0" && q50s.text == ".5" &&
                  q75.rc == "0" && q75s.text == ".75" && qp.rc == "0" && qps.text == ".25" &&
                  q25.est.value("tau", 0.0) == 0.25,
              "w2p qreg_eq: e(q) is .25/.5/.75 (A_qreg_q25_robust :104, q50_iid :267, "
              "q75_iid :238; quantile(25) -> .25), and the block's `tau` key stays (main: `.`). "
              "Got `" + q25s.text + "` `" + q50s.text + "` `" + q75s.text + "` `" + qps.text + "`");
    }
    // A vce() qreg does not implement is refused at rc 198, never run as iid.
    Reply qk = run(s, "qreg y x1 x2, vce(robust, kernel)");
    w2check(qk.rc == "198" && !qk.est.is_object(),
            "w2b qreg vce(robust, kernel): refused rc 198 (only fitted/hsheather is "
            "implemented). Got rc " + qk.rc + " " + qk.msg);

    // probit_r_nocons, w2b_estim.log:706 `|.4484790120538529|.1031705727505295|2|`
    // (T3). Main: rc 1 "[bad_input] unrecognized option '--no-intercept'".
    Reply prn = run(s, "probit d x1 x2, r nocons");
    Reply prnStr = run(s, "display \"`e(vce)'|\" e(k)");
    w2check(prn.rc == "0" && close(coefOf(prn.est, "x1", "b"), .4484790120538529, 1e-7) &&
                close(coefOf(prn.est, "x1", "se"), .1031705727505295, 1e-7) &&
                prnStr.text == "robust|2" && prn.est["coeftable"].size() == 2,
            "w2b probit_r_nocons: rc 0, b_x1 .4484790120538529, se_x1 .1031705727505295, "
            "`robust|2`, no _cons row (main: rc 1 unrecognized --no-intercept). Got rc " +
                prn.rc + " b=" + fmt(coefOf(prn.est, "x1", "b")) + " se=" +
                fmt(coefOf(prn.est, "x1", "se")) + " `" + prnStr.text + "` " + prn.msg);
    // probit_noconstant, :732 `|.4484790120538529|.1036892933730802|` (T3).
    Reply pn = run(s, "probit d x1 x2, noconstant");
    Reply pnStr = run(s, "display \"`e(vce)'|\" e(k)");
    w2check(pn.rc == "0" && close(coefOf(pn.est, "x1", "b"), .4484790120538529, 1e-7) &&
                close(coefOf(pn.est, "x1", "se"), .1036892933730802, 1e-7) &&
                pnStr.text == "oim|2",
            "w2b probit_noconstant: rc 0, b_x1 .4484790120538529, se_x1 .1036892933730802, "
            "`oim|2` (main: rc 1 unrecognized --no-intercept). Got rc " + pn.rc + " b=" +
                fmt(coefOf(pn.est, "x1", "b")) + " se=" + fmt(coefOf(pn.est, "x1", "se")) + " `" +
                pnStr.text + "` " + pn.msg);
    // logit noconstant, the sibling (no captured logit arm): rc 0, two rows,
    // no _cons, and a different fit from the default logit. Main: rc 1.
    Reply ln = run(s, "logit d x1 x2, nocons");
    Reply lnStr = run(s, "display e(k)");
    Reply lcon = run(s, "logit d x1 x2");
    w2check(ln.rc == "0" && ln.est.is_object() && ln.est["coeftable"].size() == 2 &&
                std::isnan(coefOf(ln.est, "_cons", "b")) &&
                std::isnan(coefOf(ln.est, "(intercept)", "b")) && lnStr.text == "2" &&
                std::isfinite(coefOf(ln.est, "x1", "se")) &&
                !close(coefOf(ln.est, "x1", "b"), coefOf(lcon.est, "x1", "b"), 1e-3),
            "w2b logit nocons: rc 0, e(k) 2, no _cons row (main: rc 1 unrecognized "
            "--no-intercept). Got rc " + ln.rc + " `" + lnStr.text + "` b=" +
                fmt(coefOf(ln.est, "x1", "b")) + " " + ln.msg);

    // FU-131 (replaces BUG-603's interim rc-198 refusals): postestimation after a
    // no-constant fit answers from the NO-constant model, never the with-constant
    // one (probit_ctl _b[x1] 1.355178, :762). Each value is derived from the fit
    // itself on this frame: lincom x1 is the fit's _b[x1]/_se[x1] (:732
    // .4484790120538529 / .1036892933730802; w2p_estim_ev.log:1604 G_pnc_lincom_x1
    // equals :1585 G_pnc_fit on the double frame), test x1 is chi2 (b/se)^2, nlcom
    // 2*_b[x1] is 2b with se 2se, predict is normal(_b[x1]*x1 + _b[x2]*x2) with no
    // constant, and margins, dydx(x1) is mean(normalden(xb))*_b[x1]. Main: every
    // verb rc 198.
    {
      Reply pn2 = run(s, "probit d x1 x2, noconstant");
      const double pb = coefOf(pn2.est, "x1", "b"), pse = coefOf(pn2.est, "x1", "se");
      Reply pLin = run(s, "lincom x1");
      const double linEst = rOf(s, "estimate"), linSe = rOf(s, "se");
      w2check(pn2.rc == "0" && pLin.rc == "0" && close(pb, .4484790120538529, 1e-7) &&
                  close(linEst, pb, 1e-12) && close(linSe, pse, 1e-12),
              "w2b probit noconstant -> `lincom x1`: rc 0, r(estimate)/r(se) = the fit's "
              "_b[x1]/_se[x1] (w2b_estim.log:732; main: rc 198). Got rc " + pLin.rc + " " +
                  fmt(linEst) + "/" + fmt(linSe) + " vs " + fmt(pb) + "/" + fmt(pse) + " " +
                  pLin.msg);
      Reply pTest = run(s, "test x1");
      const double chi2 = rOf(s, "chi2");
      w2check(pTest.rc == "0" && close(chi2, (pb / pse) * (pb / pse), 1e-10),
              "w2b probit noconstant -> `test x1`: rc 0, r(chi2) = (_b[x1]/_se[x1])^2 (main: "
              "rc 198). Got rc " + pTest.rc + " " + fmt(chi2) + " " + pTest.msg);
      Reply pNl = run(s, "nlcom _b[x1]*2");
      double nlB = kNaN, nlSe = kNaN;
      if (pNl.est.is_object() && pNl.est.contains("coeftable") && pNl.est["coeftable"].is_array() &&
          pNl.est["coeftable"].size() == 1) {
        nlB = numOf(pNl.est["coeftable"][0], "b");
        nlSe = numOf(pNl.est["coeftable"][0], "se");
      }
      w2check(pNl.rc == "0" && close(nlB, 2 * pb, 1e-12) && close(nlSe, 2 * pse, 1e-10),
              "w2b probit noconstant -> `nlcom _b[x1]*2`: rc 0, 2b / 2se (main: rc 198). Got rc " +
                  pNl.rc + " " + fmt(nlB) + "/" + fmt(nlSe) + " " + pNl.msg);
      Reply pPred = run(s, "predict double ph");
      (void)run(s, "generate double ph_gap = abs(ph - normal(_b[x1]*x1 + _b[x2]*x2))");
      (void)run(s, "summarize ph_gap");
      const double gapMax = rOf(s, "max"), gapN = rOf(s, "N");
      w2check(pPred.rc == "0" && gapN == 60 && std::isfinite(gapMax) && gapMax < 1e-12,
              "w2b probit noconstant -> `predict ph`: rc 0, ph = normal(_b[x1]*x1 + _b[x2]*x2) "
              "on all 60 rows, no constant (main: rc 198, no column). Got rc " + pPred.rc +
                  " max gap " + fmt(gapMax) + " N " + fmt(gapN) + " " + pPred.msg);
      // generate's normalden() (BUG-1124; this arm spelled the density out before
      // it existed). pdGen.rc is checked: a refused generate would leave
      // summarize ph_gap's r(mean) in place.
      Reply pdGen = run(s, "generate double pd_nc = normalden(_b[x1]*x1 + _b[x2]*x2)");
      (void)run(s, "summarize pd_nc");
      const double ame = rOf(s, "mean") * pb;
      Reply pMarg = run(s, "margins, dydx(x1)");
      const double mB = coefOf(pMarg.est, "x1", "b");
      w2check(pdGen.rc == "0" && pMarg.rc == "0" && close(mB, ame, 1e-10),
              "w2b probit noconstant -> `margins, dydx(x1)`: rc 0, dy/dx = "
              "mean(normalden(xb))*_b[x1] with no constant (main: rc 198). Got rc " + pMarg.rc +
                  " " + fmt(mB) + " vs " + fmt(ame) + " " + pMarg.msg);
      (void)run(s, "drop ph ph_gap pd_nc");
    }
    // regress, noconstant: lincom x1 is the fit's own _b[x1]/_se[x1]
    // (w2p_estim_ev.log:1294 G_nc_lincom_x1 equals :1275 G_nc_fit). Main: rc 198.
    Reply regn = run(s, "regress y x1 x2, noconstant");
    const double rb = coefOf(regn.est, "x1", "b"), rse = coefOf(regn.est, "x1", "se");
    Reply regLin = run(s, "lincom x1");
    const double rlEst = rOf(s, "estimate"), rlSe = rOf(s, "se");
    w2check(regn.rc == "0" && regLin.rc == "0" && std::isnan(coefOf(regn.est, "_cons", "b")) &&
                close(rlEst, rb, 1e-12) && close(rlSe, rse, 1e-12),
            "w2b regress noconstant -> lincom x1: rc 0, r(estimate)/r(se) = the fit's "
            "_b[x1]/_se[x1] (main: rc 198). Got rc " + regLin.rc + " " + fmt(rlEst) + "/" +
                fmt(rlSe) + " vs " + fmt(rb) + "/" + fmt(rse) + " " + regLin.msg);
    // Controls: the with-constant fits keep serving the same verbs.
    Reply pc = run(s, "probit d x1 x2");
    Reply pcLin = run(s, "lincom x1");
    Reply pcPred = run(s, "predict ph2");
    w2check(pc.rc == "0" && pcLin.rc == "0" && pcPred.rc == "0",
            "w2b probit with-constant control: lincom/predict still serve. Got rc " + pcLin.rc +
                "/" + pcPred.rc + " " + pcLin.msg + pcPred.msg);
  }

  // ── logit/probit edge class (logit-probit-edge-class) ──────────────────────
  // (a) FU-464 / FU-463 on auto: g3_logit_probit_vce.log:197-234 `probit foreign
  // mpg weight, vce(cluster rep78)` _se[mpg] 1.57639058969213e-02 with the header
  // Wald chi2(2) = 35.04 (:207); :31-64 `logit ..., vce(robust)` Wald chi2(2) =
  // 25.77 (:41). Main: the clustered probit is rc 1 ("unrecognized option
  // '--cluster'") and the robust logit posts its LR chi2 35.7. The header
  // statistics print with 2 decimals, so they are compared at 0.005.
  {
    // `sysuse auto` is not bundled in this build. The arm reads mpg, weight,
    // foreign (Foreign = 1) and rep78 (empty = missing) from the internal oracle
    // input fixture tests/eval/golden/aer_estim2_glm_auto_input.csv, row order
    // kept. That fixture is not part of the public tree, so when it is absent
    // the arm is a declared skip, not a failure.
    struct AutoRow { double mpg, weight, foreign, rep78; };
    std::string autoCsv = std::string(__FILE__);
    for (char& c : autoCsv)
      if (c == '\\') c = '/';
    {
      const auto pos = autoCsv.rfind("/piet/native/src/selftest_w2_estimopts.cpp");
      autoCsv = (pos == std::string::npos ? std::string(".") : autoCsv.substr(0, pos)) +
                "/tests/eval/golden/aer_estim2_glm_auto_input.csv";
    }
    std::ifstream autoIn(autoCsv);
    if (!autoIn.good()) {
      w2skip("logit/probit edge class (a): g3 probit/logit vce on the auto rows",
             "fixture not found at " + autoCsv);
    } else {
      std::vector<AutoRow> autoRows;
      bool autoParsed = true;
      {
        std::string line;
        bool header = true;
        while (std::getline(autoIn, line)) {
          if (!line.empty() && line.back() == '\r') line.pop_back();
          if (header) {
            header = false;
            if (line.rfind("price,mpg,weight,foreign,rep78", 0) != 0) autoParsed = false;
            continue;
          }
          if (line.empty()) continue;
          std::vector<std::string> cell;
          std::istringstream ls(line);
          std::string c;
          while (std::getline(ls, c, ',')) cell.push_back(c);
          if (cell.size() < 5 || cell[1].empty() || cell[2].empty() ||
              (cell[3] != "Domestic" && cell[3] != "Foreign")) {
            autoParsed = false;
            break;
          }
          char* e1 = nullptr;
          char* e2 = nullptr;
          AutoRow r{};
          r.mpg = std::strtod(cell[1].c_str(), &e1);
          r.weight = std::strtod(cell[2].c_str(), &e2);
          r.foreign = cell[3] == "Foreign" ? 1.0 : 0.0;
          r.rep78 = kNaN;
          if (*e1 != '\0' || *e2 != '\0') {
            autoParsed = false;
            break;
          }
          if (!cell[4].empty()) {
            char* e3 = nullptr;
            r.rep78 = std::strtod(cell[4].c_str(), &e3);
            if (*e3 != '\0') {
              autoParsed = false;
              break;
            }
          }
          autoRows.push_back(r);
        }
      }
      w2check(autoParsed && autoRows.size() == 74,
              "logit/probit edge class (a): " + autoCsv + " parses to 74 rows (got " +
                  std::to_string(autoRows.size()) + (autoParsed ? "" : ", malformed") + ")");
      piet::SessionState s(workdir);
      (void)piet::dispatch("clear", s);
      (void)piet::dispatch("set obs 74", s);
      for (const char* v : {"mpg", "weight", "foreign", "rep78"})
        (void)piet::dispatch(std::string("generate double ") + v + " = .", s);
      int row = 0;
      for (const AutoRow& r : autoRows) {
        ++row;
        const std::string in = " in " + std::to_string(row);
        (void)piet::dispatch("replace mpg = " + fmt(r.mpg) + in, s);
        (void)piet::dispatch("replace weight = " + fmt(r.weight) + in, s);
        (void)piet::dispatch("replace foreign = " + fmt(r.foreign) + in, s);
        if (std::isfinite(r.rep78)) (void)piet::dispatch("replace rep78 = " + fmt(r.rep78) + in, s);
      }
      Reply pc = run(s, "probit foreign mpg weight, vce(cluster rep78)");
      const double se = coefOf(pc.est, "mpg", "se");
      const double chi2 = sessionValue(s, "e(chi2)");
      Reply pcs = run(s, "display \"`e(vce)'|`e(vcetype)'|`e(clustvar)'|`e(chi2type)'|\" e(N_clust)");
      w2check(pc.rc == "0" && close(se, 1.57639058969213e-02, 1e-7) && std::fabs(chi2 - 35.04) <= 0.005 &&
                  pcs.text == "cluster|Robust|rep78|Wald|5",
              "g3 probit vce(cluster rep78): rc 0, _se[mpg] .0157639058969213 (:234), Wald chi2 35.04 "
              "(:207), `cluster|Robust|rep78|Wald|5` (main: rc 1 unrecognized --cluster). Got rc " +
                  pc.rc + " se=" + fmt(se) + " chi2=" + fmt(chi2) + " `" + pcs.text + "` " + pc.msg);
      Reply lr = run(s, "logit foreign mpg weight, vce(robust)");
      const double lchi2 = sessionValue(s, "e(chi2)");
      Reply lrs = run(s, "display \"`e(chi2type)'\"");
      w2check(lr.rc == "0" && std::fabs(lchi2 - 25.77) <= 0.005 && lrs.text == "Wald",
              "g3 logit vce(robust): e(chi2) is the Wald chi2 25.77 (:41), e(chi2type) Wald (main: "
              "the LR chi2). Got rc " + lr.rc + " chi2=" + fmt(lchi2) + " `" + lrs.text + "`");
      Reply pr = run(s, "probit foreign mpg weight, vce(robust)");
      const double pchi2 = sessionValue(s, "e(chi2)");
      w2check(pr.rc == "0" && std::fabs(pchi2 - 30.26) <= 0.005,
              "g3 probit vce(robust): e(chi2) is the Wald chi2 30.26 (:153). Got rc " + pr.rc +
                  " chi2=" + fmt(pchi2));
    }
  }

  // (b) FU-471: e(N_cdf)/e(N_cds) are Stata's "Note: N failures and M successes
  // completely determined" (w2b_estim.log:757 `probit d x1 x2`: 2 failures, 10
  // successes; the logit fit at :623-648 prints no note, so 0 and 0), with
  // e(df_m) 2, e(rank) 3 and e(chi2type) LR. Main: every one is `.` / empty.
  // The w2b frame with its float storage, as the noconstant block above builds it.
  {
    piet::SessionState s(workdir);
    (void)piet::dispatch("clear", s);
    (void)piet::dispatch("set obs 60", s);
    (void)piet::dispatch("generate float x1 = mod(_n*7,11) + mod(_n,5)/10", s);
    (void)piet::dispatch("generate float x2 = mod(_n*3,5) + mod(_n*13,7)/10", s);
    (void)piet::dispatch("generate float y = 1 + 2*x1 - x2 + ceil(_n/6)/3 + mod(_n*11,17)/5", s);
    (void)piet::dispatch("generate byte d = (y > 12)", s);
    Reply pb = run(s, "probit d x1 x2");
    Reply pbs = run(s, "display e(N_cdf) \"|\" e(N_cds) \"|\" e(df_m) \"|\" e(rank) \"|`e(chi2type)'\"");
    w2check(pb.rc == "0" && pbs.text == "2|10|2|3|LR",
            "w2b probit d x1 x2: e(N_cdf)|e(N_cds)|e(df_m)|e(rank)|e(chi2type) `2|10|2|3|LR` "
            "(w2b_estim.log:757). Got rc " + pb.rc + " `" + pbs.text + "` " + pb.msg);
    Reply lg = run(s, "logit d x1 x2");
    Reply lgs = run(s, "display e(N_cdf) \"|\" e(N_cds)");
    w2check(lg.rc == "0" && lgs.text == "0|0",
            "w2b logit d x1 x2: e(N_cdf)|e(N_cds) `0|0` (no note, w2b_estim.log:623-648). Got `" +
                lgs.text + "`");
  }

  // (c) FU-462/FU-467/FU-468: a dummy that implies failure is omitted with its rows
  // (Stata's perfect-prediction check, rule 1, run for logit; measured on StataBE):
  // the fit equals the fit on dz0 == 0 without dz0, _b[dz0] is 0 and e(rules) is
  // (1, 0, 0, 10). w2u_binary_ml_edges.do's fixture (bm_l_dz0 / bm_l_dz0_ctl, the
  // equality control of METHOD.md section 6). Main: rc 0 with N 200, dz0 kept
  // (_b -29.2) and ll_0 over all 200 rows. `asis` keeps dz0 and every row (main:
  // rc 198 "option asis not allowed").
  {
    piet::SessionState s(workdir);
    seed(s, {"clear", "set obs 200", "generate double x1 = mod(_n*7,23)/4 - 2.75 + mod(_n,3)/8",
             "generate double x2 = mod(_n*11,17)/8 + mod(_n*5,9)/16", "generate byte c = mod(_n*3,4)",
             "generate double eps = (mod(_n*37,41)/41 + mod(_n*53,59)/59 + mod(_n*29,31)/31 - 1.5)*2.2",
             "generate byte y = (0.9*x1 - 0.5*x2 + 0.3*c + eps + 0.4 > 0)",
             "generate byte dz0 = (mod(_n,9)==0 & y==0)"});
    for (const std::string cmd : {std::string("logit"), std::string("probit")}) {
      Reply k = run(s, cmd + " y x1 if dz0 == 0");
      const double kb = sessionValue(s, "_b[x1]"), kll = sessionValue(s, "e(ll)"),
                   kll0 = sessionValue(s, "e(ll_0)"), kchi = sessionValue(s, "e(chi2)");
      Reply f = run(s, cmd + " y x1 dz0");
      const double fb = sessionValue(s, "_b[x1]"), fll = sessionValue(s, "e(ll)"),
                   fll0 = sessionValue(s, "e(ll_0)"), fchi = sessionValue(s, "e(chi2)"),
                   fd = sessionValue(s, "_b[dz0]"), fN = sessionValue(s, "e(N)");
      const piet::eval::EvalMatrix* rules = nullptr;
      for (const auto& kv : s.eval.eMatrices)
        if (kv.first == "rules") rules = &kv.second;
      const bool rulesOk = rules != nullptr && rules->data.size() == 1 && rules->data[0].size() == 4 &&
                           rules->data[0][0] == 1 && rules->data[0][1] == 0 && rules->data[0][2] == 0 &&
                           rules->data[0][3] == 10 && rules->rownames == std::vector<std::string>{"dz0"};
      w2check(k.rc == "0" && f.rc == "0" && fN == 190 && fb == kb && fll == kll && fll0 == kll0 &&
                  fchi == kchi && fd == 0 && rulesOk,
              "w2u " + cmd + " y x1 dz0: dz0 omitted with its 10 rows -- N 190, _b[x1]/e(ll)/e(ll_0)/"
              "e(chi2) == the dz0 == 0 control, _b[dz0] 0, e(rules) (1,0,0,10) (main: N 200, dz0 "
              "kept). Got rc " + f.rc + " N=" + fmt(fN) + " b=" + fmt(fb) + " vs " + fmt(kb) +
                  " ll0=" + fmt(fll0) + " vs " + fmt(kll0) + " _b[dz0]=" + fmt(fd) + " rules " +
                  (rulesOk ? "ok" : "wrong") + " " + f.msg);
      Reply a = run(s, cmd + " y x1 dz0, asis");
      const double aN = sessionValue(s, "e(N)");
      w2check(a.rc == "0" && aN == 200 && sessionValue(s, "_b[dz0]") < -5,
              "w2u " + cmd + " y x1 dz0, asis: rc 0, every row kept (N 200), dz0 kept with a large "
              "negative _b (main: rc 198 option asis not allowed). Got rc " + a.rc + " N=" + fmt(aN) +
                  " " + a.msg);
    }
  }

  // (d) The captured w2u_binary_ml_edges.log on the same fixture. FU-541: predict
  // after the dz0 drop is the dropped fit's (bm_l_dz0_pred row 5 .6736077370505257,
  // bm_p_dz0_pred .6682272204637553) and `.` on dz0's row 9 (predict's rule handling);
  // after `asis` it is the asis fit's (bm_l_dz0_asis_pred .6736489208297388 /
  // 1.36959495175e-07, bm_p_dz0_asis_pred .668220342134014 / 5.00441797035e-08).
  // Main: the asis values after both fits, row 9 never `.`. bm_[lp]_cg3_vce: with 3
  // clusters e(df_m) 1, e(rank) 2, e(chi2) `.` (main: 2|3|1143.6). bm_e_probit_str:
  // a string outcome is "no observations" r(2000) (main: rc 1).
  {
    piet::SessionState s(workdir);
    seed(s, {"clear", "set obs 200", "generate double x1 = mod(_n*7,23)/4 - 2.75 + mod(_n,3)/8",
             "generate double x2 = mod(_n*11,17)/8 + mod(_n*5,9)/16", "generate byte c = mod(_n*3,4)",
             "generate double eps = (mod(_n*37,41)/41 + mod(_n*53,59)/59 + mod(_n*29,31)/31 - 1.5)*2.2",
             "generate byte y = (0.9*x1 - 0.5*x2 + 0.3*c + eps + 0.4 > 0)",
             "generate byte dz0 = (mod(_n,9)==0 & y==0)", "generate g3 = mod(ceil(_n/5),3) + 1",
             "generate str1 ystr = cond(y==1, \"1\", \"0\")"});
    // w2u_binary_ml_edges.log:965-973: predict with no estimates is "last estimates
    // not found" r(301) (main: rc 1).
    Reply np = run(s, "predict double q");
    w2check(np.rc == "301", "predict before any fit: r(301) (w2u bm_l_xs_pred rcp). Got rc " + np.rc);
    struct Want { const char* cmd; double p5, ap5, ap9; };
    for (const Want& w : {Want{"logit", .6736077370505257, .6736489208297388, 1.36959495175e-07},
                          Want{"probit", .6682272204637553, .668220342134014, 5.00441797035e-08}}) {
      const std::string cmd = w.cmd;
      Reply f = run(s, cmd + " y x1 dz0");
      (void)run(s, "capture drop p");
      (void)run(s, "capture drop xb");
      Reply pp = run(s, "predict double p");
      Reply px = run(s, "predict double xb, xb");
      const double p5 = sessionValue(s, "p[5]"), p9 = sessionValue(s, "p[9]"), xb9 = sessionValue(s, "xb[9]");
      w2check(f.rc == "0" && pp.rc == "0" && px.rc == "0" && close(p5, w.p5, 1e-9) && std::isnan(p9) &&
                  std::isnan(xb9),
              "w2u " + cmd + " y x1 dz0 then predict: row 5 " + fmt(w.p5) + " (the dropped fit), row 9 "
              "p and xb `.` (_pred_rules). Got rc " + f.rc + "/" + pp.rc + "/" + px.rc + " p5=" + fmt(p5) +
                  " p9=" + fmt(p9) + " xb9=" + fmt(xb9) + " " + pp.msg);
      Reply a = run(s, cmd + " y x1 dz0, asis");
      (void)run(s, "capture drop p");
      Reply ap = run(s, "predict double p");
      const double a5 = sessionValue(s, "p[5]"), a9 = sessionValue(s, "p[9]");
      w2check(a.rc == "0" && ap.rc == "0" && close(a5, w.ap5, 1e-9) && close(a9, w.ap9, 1e-9),
              "w2u " + cmd + " y x1 dz0, asis then predict: the asis fit, row 5 " + fmt(w.ap5) +
                  ", row 9 " + fmt(w.ap9) + ". Got p5=" + fmt(a5) + " p9=" + fmt(a9) + " " + ap.msg);
      Reply cg = run(s, cmd + " y x1 x2, vce(cluster g3)");
      Reply cgs = run(s, "display e(df_m) \"|\" e(rank) \"|\" e(chi2) \"|`e(chi2type)'\"");
      w2check(cg.rc == "0" && cgs.text == "1|2|.|Wald",
              "w2u " + cmd + " y x1 x2, vce(cluster g3): `1|2|.|Wald` (bm_cg3_vce/_se, test, min). Got rc " +
                  cg.rc + " `" + cgs.text + "`");
      Reply st = run(s, cmd + " ystr x1");
      w2check(st.rc == "2000", "w2u " + cmd + " ystr x1: no observations r(2000) (bm_e_probit_str). Got rc " +
                                   st.rc + " " + st.msg);
    }
    // (e) `%18.0g r(p)` after `test x1` on the bm_[lp]_dflt fit: Stata switches to
    // scientific below 1e-5 (w2u_binary_ml_edges.log:2764 bm_l_dflt_test
    // 4.85654885718e-13, :3115 bm_p_dflt_test 2.15650386937e-15). Main printed the
    // fixed form .0000000000004857 / .0000000000000022.
    for (const auto& w : {std::pair<std::string, std::string>{"logit", " 4.85654885718e-13"},
                          std::pair<std::string, std::string>{"probit", " 2.15650386937e-15"}}) {
      Reply f = run(s, w.first + " y x1 x2 i.c");
      Reply t = run(s, "test x1");
      Reply d = run(s, "display %18.0g r(p)");
      w2check(f.rc == "0" && t.rc == "0" && d.text == w.second,
              "w2u " + w.first + " y x1 x2 i.c; test x1: %18.0g r(p) `" + w.second + "` (bm_" +
                  w.first.substr(0, 1) + "_dflt_test). Got rc " + f.rc + "/" + t.rc + " `" + d.text + "`");
    }
  }

  // (f) The %g small side and the scientific width budget, through string()'s
  // shared renderer. pins_oracle_g3.log:29-75: .00001 and .000012345 stay
  // fixed, 1e-6 is 1.00e-06 / 1.000e-06 / 1.00000e-06 at %9/%10/%12.0g, -1e-7 at
  // %9.0g is -1.00e-07, 1.23456789e-7 at %12.0g is 1.23457e-07 (main: .000001,
  // .0000001, .0000001235). w2u_binary_ml_edges.log:1650 bm_p_xd_pred: %18.0g
  // 8.154790500675727e-123 is 8.1547905007e-123, a decimal shed for the
  // three-digit exponent (main: 8.15479050068e-123, 18 characters).
  {
    piet::SessionState s(workdir);
    Reply g = run(s, "display string(1e-5, \"%9.0g\") \"|\" string(0.000012345, \"%9.0g\") \"|\" "
                     "string(1e-6, \"%9.0g\") \"|\" string(1e-6, \"%10.0g\") \"|\" string(1e-6, \"%12.0g\") "
                     "\"|\" string(-1e-7, \"%9.0g\") \"|\" string(0.000000123456789, \"%12.0g\") \"|\" "
                     "string(8.154790500675727e-123, \"%18.0g\")");
    const std::string want =
        ".00001|.0000123|1.00e-06|1.000e-06|1.00000e-06|-1.00e-07|1.23457e-07|8.1547905007e-123";
    w2check(g.rc == "0" && g.text == want,
            "%g small side (g3 cells, bm_p_xd_pred): `" + want + "`. Got rc " + g.rc + " `" + g.text + "`");
  }

  // (g) FU-425 / FU-426 / FU-435: logit and probit take Stata's maximize options
  // (tolerance/ltolerance/nrtolerance/iterate/difficult, per [R] ml), and
  // every post-estimation refit carries the fit's. w2e_probes.do:41-55's fixture.
  // Oracle (w2e_probes.log, %21x decoded): :249 lp_tight / :304 lp_tight_difficult
  // _b[x1] +1.0c0301ffafbfeX+003, e(ll) -1.77239f16196c7X+001, e(ic) 10, `test 2.c`
  // r(chi2) +1.4dfd97e5fa7aaX-008; :412 lp_probit_tight_ctl +1.2a415c2c689b2X+002,
  // -1.6df18b771fe08X+001, 9, +1.7283a7951b3fbX-009. Main: r(198) "option
  // `tolerance` not allowed" on all three, and probit's iterate() refused too.
  {
    piet::SessionState s(workdir);
    seed(s, {"clear", "set obs 60", "generate id = ceil(_n/6)", "generate t  = mod(_n-1,6)+1",
             "generate x1 = mod(_n*7,11) + mod(_n,5)/10", "generate x2 = mod(_n*3,5) + mod(_n*13,7)/10",
             "generate byte c = mod(_n,4)", "generate byte g = mod(_n,5)",
             "generate y  = 1 + 2*x1 - x2 + 0.7*c + 0.4*g + id/3 + mod(_n*11,17)/5",
             "generate byte d = (y > 12)"});
    const std::string tight = ", tolerance(1e-12) ltolerance(1e-15) nrtolerance(1e-12) iterate(300)";
    struct Arm { const char* id; std::string cmd; double b, ll, chi2; double ic; };
    for (const Arm& a : {Arm{"lp_tight", "logit d x1 x2 i.c" + tight, 0x1.0c0301ffafbfep+3,
                             -0x1.77239f16196c7p+1, 0x1.4dfd97e5fa7aap-8, 10},
                         Arm{"lp_tight_difficult", "logit d x1 x2 i.c" + tight + " difficult",
                             0x1.0c0301ffafbfep+3, -0x1.77239f16196c7p+1, 0x1.4dfd97e5fa7aap-8, 10},
                         Arm{"lp_probit_tight_ctl", "probit d x1 x2 i.c" + tight, 0x1.2a415c2c689b2p+2,
                             -0x1.6df18b771fe08p+1, 0x1.7283a7951b3fbp-9, 9}}) {
      Reply f = run(s, a.cmd);
      const double b = sessionValue(s, "_b[x1]"), ll = sessionValue(s, "e(ll)"), ic = sessionValue(s, "e(ic)");
      Reply t = run(s, "test 2.c");
      const double chi2 = rOf(s, "chi2");
      w2check(f.rc == "0" && t.rc == "0" && close(b, a.b, 1e-12) && close(ll, a.ll, 1e-12) && ic == a.ic &&
                  close(chi2, a.chi2, 1e-9),
              std::string("w2e ") + a.id + ": rc 0, _b[x1] " + fmt(a.b) + ", e(ll) " + fmt(a.ll) + ", e(ic) " +
                  fmt(a.ic) + ", test 2.c r(chi2) " + fmt(a.chi2) + ". Got rc " + f.rc + "/" + t.rc + " b=" +
                  fmt(b) + " ll=" + fmt(ll) + " ic=" + fmt(ic) + " chi2=" + fmt(chi2) + " " + f.msg);
    }
    // FU-426: after the tight fit, lincom/test/margins answer from the POSTED b and V
    // (the refit carries the fit's options; main refitted at the default iterate,
    // e(ic) 8, and lincom x1 answered 8.37536603059232 against _b[x1] 8.37536716402792).
    // FU-435: this fit has completely determined rows (e(N_cds) 27 / e(N_cdf) 9 for
    // logit), which the posted V drops; the refit's se must be the posted _se.
    for (const std::string cmd : {std::string("logit"), std::string("probit")}) {
      Reply f = run(s, cmd + " d x1 x2 i.c" + tight);
      const double b = sessionValue(s, "_b[x1]"), se = sessionValue(s, "_se[x1]");
      const double ndet = sessionValue(s, "e(N_cds)") + sessionValue(s, "e(N_cdf)");
      Reply l = run(s, "lincom x1");
      const double lb = rOf(s, "estimate"), lse = rOf(s, "se");
      Reply t = run(s, "test x1");
      const double chi2 = rOf(s, "chi2");
      w2check(f.rc == "0" && l.rc == "0" && t.rc == "0" && ndet > 0 && lb == b && close(lse, se, 1e-12) &&
                  close(chi2, (b / se) * (b / se), 1e-10),
              "w2e " + cmd + " tight then lincom x1 / test x1: r(estimate) == _b[x1] (bit), r(se) == "
              "_se[x1] (determined rows), r(chi2) == (_b/_se)^2. Got rc " + f.rc + "/" + l.rc + "/" + t.rc +
                  " est=" + fmt(lb) + " vs " + fmt(b) + " se=" + fmt(lse) + " vs " + fmt(se) + " chi2=" +
                  fmt(chi2) + " ndet=" + fmt(ndet) + " " + l.msg);
      // margins, dydx(x1) = _b[x1] * mean f(x'b) at the posted b (f the logistic /
      // normal density), the index spelled out from _b[] so nothing here refits.
      // `-(xbh^2)`, parenthesised: generate reads `-xbh^2` as (-xbh)^2 today, where
      // Stata (and `display -2^2`, -4) reads -(xbh^2).
      (void)run(s, "capture drop xbh");
      (void)run(s, "capture drop fh");
      (void)run(s, "generate double xbh = _b[x1]*x1 + _b[x2]*x2 + _b[1.c]*(c==1) + _b[2.c]*(c==2) + "
                   "_b[3.c]*(c==3) + _b[_cons]");
      (void)run(s, cmd == "logit" ? "generate double fh = exp(-xbh)/(1+exp(-xbh))^2"
                                  : "generate double fh = exp(-(xbh^2)/2)/sqrt(2*_pi)");
      (void)run(s, "summarize fh, meanonly");
      const double ame = b * rOf(s, "mean");
      Reply m = run(s, "margins, dydx(x1)");
      double dydx = kNaN;
      try {
        if (m.est.is_object() && m.est.contains("coefs") && m.est["coefs"].is_array())
          for (const auto& row : m.est["coefs"])
            if (row.is_object() && row.value("name", std::string()) == "x1" && row.contains("dydx") &&
                row["dydx"].is_number())
              dydx = row["dydx"].get<double>();
      } catch (...) {
      }
      w2check(m.rc == "0" && close(dydx, ame, 1e-12),
              "w2e " + cmd + " tight then margins, dydx(x1): the AME of the posted b " + fmt(ame) +
                  " (main: the default-iterate refit's). Got rc " + m.rc + " " + fmt(dydx) + " " + m.msg);
    }
    // BUG-333's iterate() on logit, which main already ran: the refit stopped at the
    // DEFAULT iterate, so lincom x1 after `logit ..., iterate(3)` answered another model.
    {
      Reply f = run(s, "logit d x1 x2 i.c, iterate(3)");
      const double b = sessionValue(s, "_b[x1]"), ic = sessionValue(s, "e(ic)");
      Reply l = run(s, "lincom x1");
      w2check(f.rc == "0" && l.rc == "0" && ic == 3 && rOf(s, "estimate") == b,
              "logit ..., iterate(3) then lincom x1: r(estimate) == _b[x1] (the refit stops at e(ic) 3; main "
              "refitted to convergence). Got rc " + f.rc + "/" + l.rc + " ic=" + fmt(ic) + " est=" +
                  fmt(rOf(s, "estimate")) + " vs " + fmt(b));
    }
    // Stata's minimum abbreviations (ITERate TOLerance LTOLerance
    // NRTOLerance DIFficult) reach the same fit as lp_tight (main: `iter` not allowed).
    {
      Reply f = run(s, "logit d x1 x2 i.c, iter(300) tol(1e-12) ltol(1e-15) nrtol(1e-12) dif");
      const double b = sessionValue(s, "_b[x1]"), ic = sessionValue(s, "e(ic)");
      w2check(f.rc == "0" && close(b, 0x1.0c0301ffafbfep+3, 1e-12) && ic == 10,
              "logit ..., iter() tol() ltol() nrtol() dif: lp_tight's fit. Got rc " + f.rc + " b=" + fmt(b) +
                  " ic=" + fmt(ic) + " " + f.msg);
    }
    // probit iterate(): an ml fit that hits iterate() is rc 0 with e(converged) 0
    // (w2c_ml_getmata.log:994 ml_iterate_hit `0|0|2|`); main refused probit's iterate().
    {
      Reply f = run(s, "probit d x1 x2 i.c, iterate(2)");
      const double ic = sessionValue(s, "e(ic)"), conv = sessionValue(s, "e(converged)");
      w2check(f.rc == "0" && ic == 2 && conv == 0,
              "probit ..., iterate(2): rc 0, e(ic) 2, e(converged) 0. Got rc " + f.rc + " ic=" + fmt(ic) +
                  " conv=" + fmt(conv) + " " + f.msg);
    }
    // BUG-872: iterate(1e3) is iterate(1000), never one iteration. PIET checked the
    // value with strtod but forwarded it as typed, and the engine's std::stoi read
    // `1e3` as 1: rc 0, e(ic) 1, e(converged) 0 (measured on 868d2c2b). The fit must
    // match iterate(1000)'s, which converges like lp_probit_ctl (w2e_probes.log:358).
    {
      Reply f0 = run(s, "probit d x1 x2 i.c, iterate(1000)");
      const double ic0 = sessionValue(s, "e(ic)"), b0 = sessionValue(s, "_b[x1]");
      Reply f = run(s, "probit d x1 x2 i.c, iterate(1e3)");
      const double ic = sessionValue(s, "e(ic)"), conv = sessionValue(s, "e(converged)"),
                   b = sessionValue(s, "_b[x1]");
      w2check(f0.rc == "0" && f.rc == "0" && ic == ic0 && ic > 1 && conv == 1 && b == b0,
              "probit ..., iterate(1e3) == iterate(1000)'s fit (main: e(ic) 1, e(converged) 0). Got rc " +
                  f0.rc + "/" + f.rc + " ic=" + fmt(ic) + " vs " + fmt(ic0) + " conv=" + fmt(conv) + " " + f.msg);
    }
    // Controls. The xtreg family shares this option builder and does not take them
    // (main's answer: r(198)); a value the engine would refuse is a named r(198)
    // here, never the engine's rc 1; a bare tolerance / valued difficult is r(198).
    for (const std::string cmd :
         {std::string("xtreg y x1, fe iterate(5)"), std::string("xtreg y x1, fe tolerance(1e-9)"),
          std::string("xtreg y x1, re difficult"), std::string("logit d x1 x2 i.c, tolerance(-1)"),
          std::string("logit d x1 x2 i.c, tolerance"),
          std::string("probit d x1 x2 i.c, difficult(2)"), std::string("probit d x1 x2 i.c, iterate(0x10)"),
          std::string("logit d x1 x2 i.c, tolerance(0x1p-40)"), std::string("probit d x1 x2 i.c, iterate(2.5)")}) {
      (void)run(s, "xtset id t");
      Reply r = run(s, cmd);
      w2check(r.rc == "198", "`" + cmd + "`: r(198). Got rc " + r.rc + " " + r.msg);
    }
    {
      // BUG-1244: iterate(0) is Stata's starting-values report (e(ic) 0), not a refusal.
      Reply r = run(s, "probit d x1 x2 i.c, iterate(0)");
      w2check(r.rc == "0", "`probit ..., iterate(0)` is rc 0 (BUG-1244). Got rc " + r.rc + " " + r.msg);
    }
  }

  fs::remove_all(workdir, ec);
}
