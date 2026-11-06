// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// W2B lane `ivfamily` -- selftest arms.
//
// FU-23 (BUG-756..758): the VCE options' SHAPE on the estimation_m4b.cpp
// handlers -- xtpoisson/xtlogit ,fe, ivprobit/ivtobit, csdid, boottest -- and
// FU-28 (BUG-759): e(vce) after a default `ivregress 2sls`.
// Oracles: tests/eval/golden/aer_w2/w2l_vce_shape.log (arm names below) and
// w2d_uncovered.log:2187/2323/2403/2536. Every arm is a DISCRIMINATOR: its
// comment says what the pre-fix main answered. rc and e() strings are the
// oracle's; the fixture is w2l_vce_shape.do's with its rnormal()/runiform()/
// rpoisson() draws replaced by fixed arithmetic, so numbers are compared
// within this build (same-fit identities), never against the log's values.
#include "selftest_w2.h"

#include <cmath>
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

using ojson = nlohmann::ordered_json;

struct Reply {
  std::string rc = "0";
  std::string msg;
  std::string text;  // every display block's text, concatenated
  ojson est;         // the last estimation block (null when none)
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

// The first coeftable row's `se` (csdid's rows are aggregates, not x1).
double firstSe(const ojson& est) {
  try {
    if (!est.is_object() || !est.contains("coeftable") || !est["coeftable"].is_array() ||
        est["coeftable"].empty())
      return kNaN;
    const auto& row = est["coeftable"][0];
    if (row.is_object() && row.contains("se") && row["se"].is_number()) return row["se"].get<double>();
  } catch (...) {
  }
  return kNaN;
}

double seOf(const ojson& est, const std::string& name) {
  try {
    if (!est.is_object() || !est.contains("coeftable") || !est["coeftable"].is_array()) return kNaN;
    for (const auto& row : est["coeftable"]) {
      if (!row.is_object() || row.value("name", std::string()) != name) continue;
      if (row.contains("se") && row["se"].is_number()) return row["se"].get<double>();
      return kNaN;
    }
  } catch (...) {
  }
  return kNaN;
}

// The coeftable row `name`'s field `key` (b, se, ...).
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

bool same(double a, double b) { return std::isfinite(a) && std::isfinite(b) && a == b; }

// Score results publish a test statistic and p, never a synthetic coefficient.
// Check identities against the posted r() store without freezing this fixture's z.
bool scorePostingMatches(const Reply& reply, const piet::SessionState& session) {
  const auto& block = reply.est;
  if (reply.rc != "0" || !block.is_object() ||
      block.value("estimator", std::string()) != "wild_score_bootstrap" ||
      !block.contains("z") || !block["z"].is_number() ||
      !block.contains("p_symmetric") || !block["p_symmetric"].is_number()) return false;
  const double z = block["z"].get<double>(), p = block["p_symmetric"].get<double>();
  auto num = [&](const char* key) {
    const double* value = session.eval.rValues.find(key);
    return value ? *value : kNaN;
  };
  return std::isfinite(z) && std::isfinite(p) && p >= 0 && p <= 1 &&
      num("z") == z && num("chi2") == z*z && num("p") == p &&
      num("df") == 1 && num("reps") == 99 &&
      session.eval.rStrings.get("boottype") == "score" &&
      session.eval.rStrings.get("statistic") == "z" &&
      session.eval.rStrings.get("clustvars") == "id" &&
      !block.contains("beta") && !block.contains("se") && !block.contains("t0") &&
      !block.contains("coeftable") && !session.eval.rValues.contains("mean") &&
      !session.eval.rValues.contains("t") && !session.eval.rValues.contains("F") &&
      !session.eval.rValues.contains("df_r") && !session.eval.rValues.contains("N_clust");
}

std::string fmt(double v) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.17g", v);
  return buf;
}

// w2l_vce_shape.do's frame: 30 panels (id) x 6 periods (t), id2 crossing id,
// g in {0,3,5}; the random draws are replaced by fixed arithmetic.
void seed(piet::SessionState& s) {
  static const char* kFixture[] = {
      "clear",
      "set obs 180",
      "generate id = ceil(_n/6)",
      "generate t = mod(_n-1,6)+1",
      "generate id2 = mod(_n,7)+1",
      "generate x1 = mod(_n*7,11) + mod(_n,5)/10",
      "generate z1 = mod(_n*5,13) + x1/2 + mod(_n*19,23)/23",
      "generate x2 = 0.5*z1 + mod(_n*3,5) + mod(_n*13,7)/10 + mod(_n*29,31)/31",
      "generate y = 1 + 2*x1 - x2 + id/3 + mod(_n*11,17)/5 + (mod(_n*37,41)-20)/10",
      "generate byte yb = (y + (mod(_n*43,47)-23)/10 > 8)",
      "generate cnt = mod(_n*3,5) + mod(id,4) + (x1>5)",
      "generate g = cond(mod(id,3)==0,0,cond(mod(id,3)==1,3,5))",
      "xtset id t",
  };
  for (const char* c : kFixture) (void)piet::dispatch(c, s);
}

const char* kECmdVce = "display \"`e(cmd)'|`e(vce)'|`e(vcetype)'\"";

}  // namespace

void testW2bIvFamily() {
  std::printf("[selftest] --- W2B ivfamily: VCE option shapes on xtpoisson/xtlogit fe, ivprobit/ivtobit, csdid, boottest; ivregress e(vce) ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_w2bivfamily").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  // ── BUG-756: xtpoisson / xtlogit ,fe ────────────────────────────────────
  {
    piet::SessionState s(workdir);
    seed(s);
    // Malformed spellings Stata refuses r(198). Main: a bare `cluster`, a
    // valued `robust(x1)` and `vce(robust x1)` all fitted at rc 0 (iid / iid /
    // robust), and xtlogit's vce(robust) fitted a robust SE Stata refuses.
    struct Arm {
      const char* oracle;
      const char* cmd;
    };
    const Arm refused[] = {
        {"xtpoisson_fe_bare_cluster", "xtpoisson cnt x1 x2, fe cluster"},
        {"xtpoisson_fe_robust_val", "xtpoisson cnt x1 x2, fe robust(x1)"},
        {"xtpoisson_fe_vce_robust_x", "xtpoisson cnt x1 x2, fe vce(robust x1)"},
        {"xtpoisson_fe_vce_c2", "xtpoisson cnt x1 x2, fe vce(cluster id id2)"},
        {"xtpoisson_fe_ctl_cluster", "xtpoisson cnt x1 x2, fe cluster(id)"},
        {"xtlogit_fe_bare_cluster", "xtlogit yb x1 x2, fe cluster"},
        {"xtlogit_fe_robust_val", "xtlogit yb x1 x2, fe robust(x1)"},
        {"xtlogit_fe_vce_robust_x", "xtlogit yb x1 x2, fe vce(robust x1)"},
        {"xtlogit_fe_ctl_vcerobust", "xtlogit yb x1 x2, fe vce(robust)"},
        {"xtlogit_fe_vce_cluster_novar", "xtlogit yb x1 x2, fe vce(cluster)"},
    };
    for (const auto& a : refused) {
      Reply r = run(s, a.cmd);
      w2check(r.rc == "198" && r.est.is_null(),
              std::string("w2l_vce_shape ") + a.oracle + ": `" + a.cmd +
                  "` is r(198) with no fit. Got rc " + r.rc + " " + r.msg);
    }

    // xtpoisson_fe_empty_cluster: rc 0, cluster() IGNORED -- the default fit's
    // SE. Main refused it r(198) as a cluster request.
    Reply base = run(s, "xtpoisson cnt x1 x2, fe");
    Reply empty = run(s, "xtpoisson cnt x1 x2, fe cluster()");
    w2check(empty.rc == "0" && same(seOf(empty.est, "x1"), seOf(base.est, "x1")),
            "w2l_vce_shape xtpoisson_fe_empty_cluster: `cluster()` is rc 0 and fits the default "
            "model (se_x1 equal to the plain ,fe fit). Got rc " + empty.rc + " se=" +
                fmt(seOf(empty.est, "x1")) + " base=" + fmt(seOf(base.est, "x1")) + " " + empty.msg);

    // xtpoisson_fe_ctl_vcerobust: rc 0, e(cmd) `xtpoisson`. Main: `xtpois`.
    Reply rob = run(s, "xtpoisson cnt x1 x2, fe vce(robust)");
    Reply robStr = run(s, kECmdVce);
    w2check(rob.rc == "0" && robStr.text == "xtpoisson|robust|Robust",
            "w2l_vce_shape xtpoisson_fe_ctl_vcerobust: rc 0, e(cmd)|e(vce)|e(vcetype) "
            "`xtpoisson|robust|Robust` (main: e(cmd) `xtpois`). Got rc " + rob.rc + " `" +
                robStr.text + "`");

    // xtlogit_fe_empty_cluster: rc 0, the default fit, e(cmd) `clogit`. Main: r(198).
    Reply lbase = run(s, "xtlogit yb x1 x2, fe");
    Reply lempty = run(s, "xtlogit yb x1 x2, fe cluster()");
    Reply lemptyStr = run(s, "display \"`e(cmd)'\"");
    w2check(lempty.rc == "0" && same(seOf(lempty.est, "x1"), seOf(lbase.est, "x1")) &&
                lemptyStr.text == "clogit",
            "w2l_vce_shape xtlogit_fe_empty_cluster: rc 0, the default fit, e(cmd) `clogit`. Got rc " +
                lempty.rc + " se=" + fmt(seOf(lempty.est, "x1")) + " base=" +
                fmt(seOf(lbase.est, "x1")) + " e(cmd)=`" + lemptyStr.text + "` " + lempty.msg);

    // FU-34: a refused command leaves e() as it was. The last fit above was
    // xtlogit (clogit); a refused bare-cluster xtpoisson must not replace it.
    (void)run(s, "xtpoisson cnt x1 x2, fe cluster");
    Reply after = run(s, "display \"`e(cmd)'\"");
    w2check(after.text == "clogit",
            "FU-34: a refused `xtpoisson ..., fe cluster` leaves e(cmd) of the previous fit "
            "(`clogit`). Got `" + after.text + "`");
  }

  // ── BUG-756: ivprobit / ivtobit ─────────────────────────────────────────
  {
    piet::SessionState s(workdir);
    seed(s);
    struct Arm {
      const char* oracle;
      const char* cmd;
      const char* main;  // what the frozen main answered
    };
    const Arm refused[] = {
        {"ivprobit_ctl_cluster", "ivprobit yb x1 (x2 = z1), cluster(id)", "rc 0, clustered on id"},
        {"ivprobit_bare_cluster", "ivprobit yb x1 (x2 = z1), cluster", "rc 0, iid"},
        {"ivprobit_empty_cluster", "ivprobit yb x1 (x2 = z1), cluster()", "rc 0, iid"},
        {"ivprobit_vce_c2", "ivprobit yb x1 (x2 = z1), vce(cluster id id2)",
         "rc 1, `column not found: id id2`"},
        {"ivprobit_robust_val", "ivprobit yb x1 (x2 = z1), robust(x1)", "rc 0, iid"},
        {"ivprobit_vce_robust_x", "ivprobit yb x1 (x2 = z1), vce(robust x1)", "rc 0, robust"},
        {"ivtobit_bare_cluster", "ivtobit y x1 (x2 = z1), ll(0) cluster", "rc 0, iid"},
        {"ivtobit_opt_c2", "ivtobit y x1 (x2 = z1), ll(0) cluster(id id2)", "rc 1"},
        {"ivtobit_robust_val", "ivtobit y x1 (x2 = z1), ll(0) robust(x1)", "rc 0, iid"},
    };
    for (const auto& a : refused) {
      Reply r = run(s, a.cmd);
      w2check(r.rc == "198" && r.est.is_null(),
              std::string("w2l_vce_shape ") + a.oracle + ": `" + a.cmd + "` is r(198) (main: " +
                  a.main + "). Got rc " + r.rc + " " + r.msg);
    }
    // Controls: vce(cluster id) and vce(robust) still fit.
    Reply cl = run(s, "ivprobit yb x1 (x2 = z1), vce(cluster id)");
    Reply clStr = run(s, "display \"`e(vce)'|\" e(N_clust)");
    w2check(cl.rc == "0" && clStr.text == "cluster|30",
            "w2l_vce_shape ivprobit_ctl_vcecluster (control): rc 0, e(vce) cluster, e(N_clust) 30. "
            "Got rc " + cl.rc + " `" + clStr.text + "` " + cl.msg);
    Reply tr = run(s, "ivtobit y x1 (x2 = z1), ll(0) vce(robust)");
    w2check(tr.rc == "0" && !tr.est.is_null(),
            "w2l_vce_shape ivtobit_ctl_vcerobust (control): rc 0. Got rc " + tr.rc + " " + tr.msg);
  }

  // ── BUG-757: csdid ──────────────────────────────────────────────────────
  {
    piet::SessionState s(workdir);
    seed(s);
    Reply base = run(s, "csdid y x1, ivar(id) time(t) gvar(g)");
    // vce()/robust are ignored by csdid (rc 0, the default se). Main: r(198)
    // "option `vce` not allowed".
    const char* ignored[][2] = {
        {"csdid_ctl_vcecluster", "csdid y x1, ivar(id) time(t) gvar(g) vce(cluster id)"},
        {"csdid_vce_c2", "csdid y x1, ivar(id) time(t) gvar(g) vce(cluster id id2)"},
        {"csdid_robust_val", "csdid y x1, ivar(id) time(t) gvar(g) robust(x1)"},
        {"csdid_bare_cluster", "csdid y x1, ivar(id) time(t) gvar(g) cluster"},
    };
    for (const auto& a : ignored) {
      Reply r = run(s, a[1]);
      w2check(base.rc == "0" && r.rc == "0" && same(firstSe(r.est), firstSe(base.est)),
              std::string("w2l_vce_shape ") + a[0] + ": `" + a[1] +
                  "` is rc 0 with the default fit's se. Got rc " + r.rc + " se=" +
                  fmt(firstSe(r.est)) + " base=" + fmt(firstSe(base.est)) + " " + r.msg);
    }
    // The cluster() refusals with Stata's own rc. Main: rc 1 for all three
    // (engine bad_input; cluster(id id2) was cut to `id`).
    const char* refused[][3] = {
        {"csdid_opt_c2", "csdid y x1, ivar(id) time(t) gvar(g) cluster(id id2)", "103"},
        {"csdid_ctl_cluster", "csdid y x1, ivar(id) time(t) gvar(g) cluster(id)", "198"},
        {"csdid_ctl_cluster_id2", "csdid y x1, ivar(id) time(t) gvar(g) cluster(id2)", "498"},
    };
    (void)run(s, "ivprobit yb x1 (x2 = z1), vce(robust)");
    // FU-34 / csdid_ctl_cluster (w2l_vce_shape_det.log:2037 `198|drdid|||.|40`):
    // the syntax refusal (103) and the nesting check (498, csdid's own check)
    // stop before any cell and leave e() as it was; cluster(id) == ivar fits
    // every cell with `capture drdid` and fails only at the collapse
    // (:756-768), so the last cell's drdid e() is left behind.
    const char* eCmdAfter[] = {"ivprobit", "drdid", "drdid"};
    for (size_t k = 0; k < sizeof(refused) / sizeof(refused[0]); ++k) {
      const auto& a = refused[k];
      Reply r = run(s, a[1]);
      w2check(r.rc == a[2] && r.est.is_null(),
              std::string("w2l_vce_shape ") + a[0] + ": `" + a[1] + "` is r(" + a[2] +
                  "). Got rc " + r.rc + " " + r.msg);
      Reply after = run(s, "display \"`e(cmd)'\"");
      w2check(after.text == eCmdAfter[k],
              std::string("FU-34: after the refused `") + a[1] + "`, e(cmd) is `" +
                  eCmdAfter[k] + "`. Got `" + after.text + "`");
    }
  }

  // ── BUG-758: boottest ───────────────────────────────────────────────────
  {
    piet::SessionState s(workdir);
    seed(s);
    const char* kFit = "regress y x1 x2, vce(cluster id)";
    // boottest_ctl_cluster: `nograph` accepted. Main: r(198) "option `nograph` not allowed".
    (void)run(s, kFit);
    Reply ctl = run(s, "boottest x1, cluster(id) reps(99) seed(1) nograph");
    Reply ctlR = run(s, "display \"`r(clustvars)'|\" r(p)");
    w2check(ctl.rc == "0" && !ctl.est.is_null() && ctlR.text.rfind("id|", 0) == 0,
            "w2l_vce_shape boottest_ctl_cluster: rc 0, r(clustvars) `id`, r(p) set. Got rc " +
                ctl.rc + " `" + ctlR.text + "` " + ctl.msg);
    // boottest_ctl_cluster_id2: cluster(id2) overrides the fit's id. Main: refused r(198).
    (void)run(s, kFit);
    Reply id2 = run(s, "boottest x1, cluster(id2) reps(99) seed(1) nograph");
    Reply id2R = run(s, "display \"`r(clustvars)'\"");
    w2check(id2.rc == "0" && !id2.est.is_null() && id2R.text == "id2",
            "w2l_vce_shape boottest_ctl_cluster_id2: rc 0, bootstrap over id2, r(clustvars) `id2`. "
            "Got rc " + id2.rc + " `" + id2R.text + "` " + id2.msg);
    // boottest_empty_cluster: cluster() is the fit's clustering. Main: refused r(198).
    (void)run(s, kFit);
    Reply emp = run(s, "boottest x1, cluster() reps(99) seed(1) nograph");
    Reply empR = run(s, "display \"`r(clustvars)'\"");
    w2check(emp.rc == "0" && !emp.est.is_null() && empR.text == "id",
            "w2l_vce_shape boottest_empty_cluster: rc 0, r(clustvars) `id`. Got rc " + emp.rc + " `" +
                empR.text + "` " + emp.msg);
    // boottest_bare_cluster / boottest_robust_val: boottest's own message, rc 0,
    // nothing returned. Main: bare `cluster` bootstrapped anyway; robust(x1) r(198).
    // r() is read from the store: this build's `display` refuses an UNSET r()
    // where Stata prints `.`/empty (a macro-layer gap of its own).
    (void)run(s, "boottest x1, cluster(id) reps(99) seed(1) nograph");  // r() non-empty first
    (void)run(s, kFit);
    Reply bare = run(s, "boottest x1, cluster reps(99) seed(1) nograph");
    w2check(bare.rc == "0" && bare.est.is_null() && bare.text == "Option cluster not allowed." &&
                !s.eval.rValues.contains("p") && !s.eval.rStrings.contains("clustvars"),
            "w2l_vce_shape boottest_bare_cluster: rc 0, `Option cluster not allowed.`, no "
            "bootstrap, r(p)/r(clustvars) unset. Got rc " + bare.rc + " text `" + bare.text +
                "` r(p) set=" + (s.eval.rValues.contains("p") ? "yes" : "no") + " " + bare.msg);
    (void)run(s, kFit);
    Reply rv = run(s, "boottest x1, robust(x1) reps(99) seed(1) nograph");
    w2check(rv.rc == "0" && rv.est.is_null() && rv.text == "Option robust(x1) not allowed.",
            "w2l_vce_shape boottest_robust_val: rc 0, `Option robust(x1) not allowed.`, no "
            "bootstrap. Got rc " + rv.rc + " text `" + rv.text + "` " + rv.msg);
    auto rnum = [&](const char* key) {
      const double* v = s.eval.rValues.find(key);
      return v ? *v : kNaN;
    };
    // boottest_opt_c2 (w2l_vce_shape_det.log:4142-4159) is a two-way bootstrap:
    // rc 0, r(clustvars) `id id2`, r(N_clust) `.`, df_r = min(30, 7) - 1 = 6
    // (boottest's df rule, identified from its source). Its oracle t is pinned below on the w2t fixture.
    // bin/main: refused at 198 (the engine's bootstrap was one-way).
    (void)run(s, kFit);
    Reply c2 = run(s, "boottest x1, cluster(id id2) reps(99) seed(1) nograph");
    const double c2t = rnum("t"), c2p = rnum("p");
    w2check(c2.rc == "0" && !c2.est.is_null() && s.eval.rStrings.get("clustvars") == "id id2" &&
                rnum("df_r") == 6 && std::isfinite(c2t) && c2p >= 0 && c2p <= 1 &&
                !s.eval.rValues.contains("N_clust"),
            "w2l_vce_shape boottest_opt_c2: the two-way bootstrap is served (clustvars `id id2`, "
            "df_r 6, N_clust unset). Got rc " + c2.rc + " clustvars `" +
                s.eval.rStrings.get("clustvars") + "` df_r " + fmt(rnum("df_r")) + " " + c2.msg);
    // Sibling: a two-way FIT with no cluster() bootstraps on both variables
    // (it used to be cut to its first variable, `--cluster id,id2` read at index 0).
    (void)run(s, "regress y x1 x2, vce(cluster id id2)");
    Reply tw = run(s, "boottest x1, reps(99) seed(1)");
    w2check(tw.rc == "0" && s.eval.rStrings.get("clustvars") == "id id2" && rnum("t") == c2t &&
                rnum("p") == c2p,
            "boottest after a two-way clustered fit bootstraps both clusterings (the same t and p "
            "as cluster(id id2)). Got rc " + tw.rc + " t " + fmt(rnum("t")) + " " + tw.msg);
  }

  // ── FU-133: boottest preserves the fitted least-squares specification ─────
  // Oracle: tests/eval/golden/aer_w2/w2t_boottest_family.log. IV (D1 :1047)
  // is the WRE (its oracle values are pinned in the W2T block below); a weighted
  // IV fit remains a named PIET refusal. ML F1/F3 now use
  // the null-imposed score test; weighted/no-constant OLS C2/C3 also have actual
  // bootstrap paths. Each arm distinguishes the fitted model from a silent
  // least-squares substitution. Previously main
  // answered every one at rc 0 from the wrong model. M1-M4 pin boottest.ado's own
  // refusal of a two-absorbed-set reghdfe/ivreghdfe fit: its message at rc 0, no
  // bootstrap, r() emptied (D4 :1276/:1282/:1293, D5 :1361/:1367, E5 :1715/:1721/
  // :1732, E8 :1893/:1899; p5_stata.log:843/:848, p9_stata.log:223/:228).
  {
    piet::SessionState s(workdir);
    seed(s);
    (void)run(s, "generate w = 1 + mod(_n,4)");
    struct Case {
      const char* fit;
      const char* needle;
      const char* label;
    };
    const Case kRefused[] = {
        // ivregress itself is now the WRE (pinned below); ivreg2 is not served.
        {"ivreg2 y x2 (x1 = z1), cluster(id)", "ivreg2", "ivreg2"},
    };
    for (const Case& c : kRefused) {
      Reply f = run(s, c.fit);
      Reply b = run(s, "boottest x1, reps(99) seed(1) nograph");
      w2check(f.rc == "0" && b.rc == "198" && b.est.is_null() &&
                  b.msg.find(c.needle) != std::string::npos,
              std::string("FU-133 boottest after ") + c.label +
                  ": refused by name at rc 198, no bootstrap of a different model. Fit rc " + f.rc +
                  "; got rc " + b.rc + " " + b.msg);
    }
    const char* kScoreFits[] = {"logit yb x1 x2, vce(cluster id)",
                                "poisson cnt x1 x2, vce(cluster id)"};
    for (const char* fit : kScoreFits) {
      Reply f = run(s, fit);
      (void)run(s, "summarize x1");
      Reply b = run(s, "boottest x1, reps(99) seed(1) nograph");
      w2check(f.rc == "0" && scorePostingMatches(b, s),
              std::string("boottest after `") + fit +
                  "` publishes actual score z/chi2/p and score metadata, replaces r(), and "
                  "omits coefficient/OLS-only fields. Fit rc " + f.rc + "; got rc " + b.rc + " " + b.msg);
      Reply nonzero = run(s, "boottest x1 = 1, reps(99) seed(1) nograph");
      w2check(nonzero.rc != "0" && nonzero.est.is_null() &&
                  nonzero.msg.find("zero-null") != std::string::npos,
              "ML score bootstrap refuses a nonzero null explicitly. Got rc " + nonzero.rc + " " + nonzero.msg);
      Reply changedCluster = run(s, "boottest x1, cluster(id2) reps(99) seed(1) nograph");
      w2check(changedCluster.rc == "198" && changedCluster.est.is_null() &&
                  changedCluster.msg.find("same single error and bootstrap cluster") != std::string::npos,
              "ML score bootstrap refuses a changed error/bootstrap clustering specification. Got rc " +
                  changedCluster.rc + " " + changedCluster.msg);
    }
    // Controls: absorbed-FE least squares stays served (wildboot --fe shares the
    // reghdfe/xtreg,fe dof via cluster_ssf).
    const char* kServed[] = {"regress y x1 x2, vce(cluster id)",
                             "regress y x1 x2, vce(cluster id) noconstant",
                             "regress y x1 x2 [pweight=w], vce(cluster id)",
                             "reghdfe y x1 x2, absorb(t) vce(cluster id)",
                             "xtreg y x1 x2, fe vce(cluster id)"};
    for (const char* fit : kServed) {
      Reply f = run(s, fit);
      Reply b = run(s, "boottest x1, reps(99) seed(1) nograph");
      w2check(f.rc == "0" && b.rc == "0" && !b.est.is_null(),
              std::string("FU-133 control: boottest after `") + fit + "` is still served. Fit rc " +
                  f.rc + "; got rc " + b.rc + " " + b.msg);
    }

    const std::string kReghdfeA2 =
        "Doesn't work after reghdfe with more than one set of absorbed fixed effects or with "
        "absorbed interaction terms";
    const std::string kIvreghdfeA2 =
        "Doesn't work after ivreghdfe with more than one set of absorbed fixed effects or with "
        "absorbed interaction terms";
    // M1 (E5_reghdfe_a2, :1715/:1721/:1732): the message at rc 0, no bootstrap,
    // r(p) unset and summarize's r(mean) gone. Main: bootstrapped, r(p) posted.
    {
      Reply f = run(s, "reghdfe y x1 x2, absorb(id t) vce(cluster id)");
      (void)run(s, "summarize x1");
      Reply b = run(s, "boottest x1, reps(99) seed(1) nograph");
      w2check(f.rc == "0" && b.rc == "0" && b.est.is_null() && b.text == kReghdfeA2 &&
                  !s.eval.rValues.contains("p") && !s.eval.rValues.contains("mean"),
              "FU-133 M1 boottest after a two-absorb reghdfe: boottest.ado's message at rc 0, no "
              "bootstrap, r() emptied (w2t E5). Fit rc " + f.rc + "; got rc " + b.rc + " text `" +
                  b.text + "` r(p) set=" + (s.eval.rValues.contains("p") ? "yes" : "no") +
                  " r(mean) set=" + (s.eval.rValues.contains("mean") ? "yes" : "no") + " " + b.msg);
    }
    // M2 (D5_p5_verbatim, :1361/:1367; p5.do:115/:119): the ivreghdfe message at
    // rc 0. Main: bootstrapped.
    {
      Reply f = run(s, "ivreghdfe y x2 (x1 = z1), absorb(id t) cluster(id)");
      Reply b = run(s, "boottest x2, cluster(id) reps(99) seed(20260802)");
      w2check(f.rc == "0" && b.rc == "0" && b.est.is_null() && b.text == kIvreghdfeA2,
              "FU-133 M2 boottest after a two-absorb ivreghdfe: boottest.ado's message at rc 0, "
              "no bootstrap (w2t D5, P5). Fit rc " + f.rc + "; got rc " + b.rc + " text `" +
                  b.text + "` " + b.msg);
    }
    // M3 (E8_reghdfe_a2_bare, :1893/:1899): the estimator message comes before
    // the option message. Main: `Option cluster not allowed.`.
    {
      Reply f = run(s, "reghdfe y x1 x2, absorb(id t) vce(cluster id)");
      Reply b = run(s, "boottest x1, cluster reps(99) seed(1) nograph");
      w2check(f.rc == "0" && b.rc == "0" && b.est.is_null() && b.text == kReghdfeA2,
              "FU-133 M3 boottest, cluster after a two-absorb reghdfe: the estimator message, not "
              "`Option cluster not allowed.` (w2t E8). Fit rc " + f.rc + "; got rc " + b.rc +
                  " text `" + b.text + "` " + b.msg);
    }
    // M4 (D3_ivreghdfe_a1, :1197 served by Stata): one absorbed set is not
    // boottest.ado's refusal; it is the WRE (oracle t pinned below).
    {
      Reply f = run(s, "ivreghdfe y x2 (x1 = z1), absorb(t) cluster(id)");
      Reply b = run(s, "boottest x2, reps(99) seed(1) nograph");
      w2check(f.rc == "0" && b.rc == "0" && !b.est.is_null() &&
                  s.eval.rValues.contains("t") && s.eval.rValues.contains("df_r"),
              "FU-133 M4 boottest after a one-absorb ivreghdfe is served (WRE, t and df_r). "
              "Fit rc " + f.rc + "; got rc " + b.rc + " " + b.msg);
    }
  }

  // ── boottest-h0-noest-served-returns: the null, no estimation, r(), boottype ──
  // Oracle: tests/eval/golden/aer_w2/w2t_boottest_family.log (StataBE 19.5,
  // boottest 4.5.3). Numbers are compared within this build (this fixture is not
  // the capture's); each identity is the one the capture shows. Every arm fails
  // on bin/main (787a9fa2 and e0d69349), whose answer is in its comment.
  {
    piet::SessionState s(workdir);
    seed(s);
    (void)run(s, "generate w = 1 + mod(_n,4)");
    const char* kFit = "regress y x1 x2, vce(cluster id)";
    auto num = [&](const char* key) {
      const double* v = s.eval.rValues.find(key);
      return v != nullptr ? *v : kNaN;
    };
    auto estNum = [](const Reply& r, const char* key) {
      return r.est.is_object() && r.est.contains(key) && r.est[key].is_number()
                 ? r.est[key].get<double>()
                 : kNaN;
    };
    // H0 (C10 :875/:881, C11 :931/:937): `x1 = 0.0625` and `x1=0.0625` test
    // x1 = 0.0625, so t moves to (b - .0625)/se of C1's fit, t*(b - .0625)/b
    // (C10_st t -.2359095793768099 from C1_st 1.001001389919719, b .0505797).
    // bin/main: the spaced form tested x1 = 0 (null_beta 0, C1's t), the compact
    // one rc 1 (`param 'x1=0.0625' not among regressors`).
    (void)run(s, kFit);
    Reply h0 = run(s, "boottest x1, reps(99) seed(1) nograph");
    const double t1 = estNum(h0, "t0"), b1 = estNum(h0, "beta");
    const double want = t1 * (b1 - 0.0625) / b1;
    (void)run(s, kFit);
    Reply sp = run(s, "boottest x1 = 0.0625, reps(99) seed(1) nograph");
    const double tsp = estNum(sp, "t0");
    const double rtsp = num("t");
    w2check(sp.rc == "0" && std::fabs(tsp - want) <= 1e-9 * std::fabs(want) && rtsp == tsp &&
                estNum(sp, "null_beta") == 0.0625,
            "boottest x1 = 0.0625 tests x1 = 0.0625 (w2t C10 :881): t " + fmt(tsp) + " r(t) " +
                fmt(rtsp) + " want " + fmt(want) + " null_beta " + fmt(estNum(sp, "null_beta")) +
                ". Got rc " + sp.rc + " " + sp.msg);
    (void)run(s, kFit);
    Reply cp = run(s, "boottest x1=0.0625, reps(99) seed(1) nograph");
    w2check(cp.rc == "0" && estNum(cp, "t0") == tsp,
            "boottest x1=0.0625 is the same hypothesis (w2t C11 :937 = C10 :881). Got rc " + cp.rc +
                " t " + fmt(estNum(cp, "t0")) + " " + cp.msg);
    // Shapes PIET cannot represent refuse by name at 198, never as a test of x1
    // alone (bin/main served `x1 x2` as x1, rc 0, C1's t) and never at rc 1
    // (bin/main: `{x1=0.0625}` and `x1+x2=1` reached the engine as a name).
    const char* kShapes[] = {"boottest x1 x2, reps(99) seed(1) nograph",
                             "boottest {x1=0.0625}, reps(99) seed(1) nograph",
                             "boottest (x1=0.0625) (x2=0), reps(99) seed(1) nograph",
                             "boottest x1+x2=1, reps(99) seed(1) nograph",
                             "boottest x1 = x2, reps(99) seed(1) nograph"};
    for (const char* cmd : kShapes) {
      (void)run(s, kFit);
      Reply r = run(s, cmd);
      w2check(r.rc == "198" && r.est.is_null() && r.msg.find("hypothesis") != std::string::npos,
              std::string("`") + cmd + "`: refused by name at 198, not served as one coefficient. "
              "Got rc " + r.rc + " " + r.msg);
    }

    // Served r() (C1_st :346, C1_r :351, return list :356-:381): r(t) is the
    // displayed t, F = t^2, df_r = G - 1 (30 clusters), df 1, reps 99, the
    // macros; summarize's r(mean) is gone; r(N_clust) is outside Stata's set and
    // reads `.`; r(CIstr) is the computed confidence set, `[lo, hi]` (BUG-1174, whose
    // probe pins Stata's numbers; it refused at r(111) before PIET computed the set).
    // bin/main: r(t), r(df_r), r(N_clust) all r(111), r(mean) still 5.x.
    (void)run(s, kFit);
    (void)run(s, "summarize x1");
    Reply sv = run(s, "boottest x1, cluster(id) reps(99) seed(1) nograph");
    const double tsv = estNum(sv, "t0");
    Reply nclust = run(s, "display r(N_clust)");
    Reply mean = run(s, "display r(mean)");
    Reply cistr = run(s, "display \"`r(CIstr)'\"");
    w2check(sv.rc == "0" && num("t") == tsv && num("F") == tsv * tsv && num("df_r") == 29 &&
                num("df") == 1 && num("reps") == 99 && num("NH0s") == 1 && num("null") == 1 &&
                s.eval.rStrings.get("boottype") == "wild" &&
                s.eval.rStrings.get("weighttype") == "rademacher" &&
                s.eval.rStrings.get("statistic") == "t" && s.eval.rStrings.get("seed") == "1" &&
                s.eval.rStrings.get("clustvars") == "id" && nclust.rc == "0" &&
                nclust.text == "." && mean.rc == "0" && mean.text == "." && cistr.rc == "0" &&
                cistr.text.size() > 4 && cistr.text.front() == '[' && cistr.text.back() == ']',
            "served boottest posts r(t) r(F) r(df_r) r(df) r(reps) and the macros (w2t C1 :346, "
            ":356-:381), r(N_clust)/r(mean) read `.`, r(CIstr) `[lo, hi]` (BUG-1174). Got r(t) " +
                fmt(num("t")) + " t0 " + fmt(tsv) + " df_r " + fmt(num("df_r")) + " N_clust `" +
                nclust.text + "` rc " + nclust.rc + " mean `" + mean.text + "` CIstr rc " +
                cistr.rc + " `" + cistr.text + "` " + sv.msg);
    // boottype(wild) after a least-squares fit is the default (C12 :987/:993 =
    // C1 :340/:346). bin/main: r(198) `option boottype not allowed`.
    (void)run(s, kFit);
    Reply bw = run(s, "boottest x1, cluster(id) reps(99) seed(1) nograph boottype(wild)");
    w2check(bw.rc == "0" && estNum(bw, "t0") == tsv && num("t") == tsv &&
                s.eval.rStrings.get("boottype") == "wild",
            "boottype(wild) after regress equals the default (w2t C12 :993). Got rc " + bw.rc +
                " t " + fmt(estNum(bw, "t0")) + " " + bw.msg);
    (void)run(s, kFit);
    Reply bs = run(s, "boottest x1, cluster(id) reps(99) seed(1) nograph boottype(score)");
    w2check(bs.rc == "198" && bs.est.is_null() && bs.msg.find("boottype") != std::string::npos,
            "boottype(score) is refused by name at 198 (no score bootstrap). Got rc " + bs.rc +
                " " + bs.msg);

    // xtreg, fe: boottest's t uses N - kZ (FEdfadj 0, identified from boottest's ado) where reghdfe's
    // uses N - kZ - 1: E6_st :1792 1.268217344494529 = E4_st :1668
    // 1.264649918737085 * sqrt(178/177). bin/main: r(t) r(111), t0 E4's.
    (void)run(s, "reghdfe y x1 x2, absorb(id) vce(cluster id)");
    Reply rh = run(s, "boottest x1, reps(99) seed(1) nograph");
    const double trh = num("t");
    (void)run(s, "xtreg y x1 x2, fe vce(cluster id)");
    Reply xt = run(s, "boottest x1, reps(99) seed(1) nograph");
    const double wantXt = trh * std::sqrt(178.0 / 177.0);
    w2check(rh.rc == "0" && xt.rc == "0" && trh == estNum(rh, "t0") &&
                std::fabs(num("t") - wantXt) <= 1e-12 * std::fabs(wantXt) &&
                estNum(xt, "t0") == num("t"),
            "boottest after xtreg, fe: r(t) = reghdfe's * sqrt(178/177) (w2t E6_st :1792 vs E4_st "
            ":1668). Got reghdfe " + fmt(trh) + " xtreg " + fmt(num("t")) + " want " +
                fmt(wantXt) + " " + xt.msg);

    // r(b) and r(V) (return list :380-:381; boottest's posting, identified from its source): b = _b - null,
    // V = se^2 * (N-1)/_df with t = b/se boottest's t and _df = N - kZ - FEdfadj
    // (boottest's _df, identified from its source): w2_esample_tsrefit W1/W3/W5_st :342/:399/:463.
    // bin/main: both r(111) (declared, never posted).
    auto rcell = [&](const char* name) {
      for (const auto& [k, m] : s.eval.rMatrices) {
        if (k == name && m.data.size() == 1 && m.data[0].size() == 1) return m.data[0][0];
      }
      return kNaN;
    };
    auto near12 = [](double a, double want) {
      return std::isfinite(a) && std::isfinite(want) &&
             std::fabs(a - want) <= 1e-12 * std::fabs(want);
    };
    {
      Reply f = run(s, kFit);
      Reply b = run(s, "boottest x1 = 0.0625, reps(99) seed(1) nograph");
      const double bx = coefOf(f.est, "x1", "b"), sx = seOf(f.est, "x1");
      const double wantV = sx * sx * 179.0 / 177.0;  // regress: _df = 180 - 3
      w2check(b.rc == "0" && near12(rcell("b"), bx - 0.0625) && near12(rcell("V"), wantV) &&
                  near12(num("t"), (bx - 0.0625) / sx),
              "boottest x1 = 0.0625 after regress: r(b) = _b[x1] - .0625, r(V) = _se^2 * 179/177. "
              "Got rc " + b.rc + " b " + fmt(rcell("b")) + " want " + fmt(bx - 0.0625) + " V " +
                  fmt(rcell("V")) + " want " + fmt(wantV) + " " + b.msg);
      (void)run(s, "xtreg y x1 x2, fe vce(cluster id)");
      Reply x = run(s, "boottest x1, reps(99) seed(1) nograph");
      // xtreg, fe: _df = 180 - 2 (FEdfadj 0), se = b/t (W3_st :399).
      const double tx = num("t"), bxx = rcell("b");
      w2check(x.rc == "0" && near12(rcell("V"), (bxx / tx) * (bxx / tx) * 179.0 / 178.0),
              "boottest after xtreg, fe: r(V) = (r(b)/r(t))^2 * 179/178 (W3_st :399). Got V " +
                  fmt(rcell("V")) + " b " + fmt(bxx) + " t " + fmt(tx) + " " + x.msg);
    }

    // FU-732 areg + cluster (w2_esample_tsrefit W1 :336/:342, w2t E1 :1471/:1477,
    // E2 :1529/:1535): served. Its p is the engine's (cluster_ssf scales t0 and
    // every bootstrap t alike); r(t) is the areg table's _b/_se, which charges an
    // absorbed effect nested in the cluster (absorb(id) cluster(id): E2_st t
    // 1.156417206472481, not reghdfe's 1.264649918737085 at :1668), and r(V) is
    // _se^2 * (N-1)/(N - kZ - #groups). bin/main: r(198) `after areg ...,
    // vce(cluster)`.
    {
      struct AregCase {
        const char* fit;
        double df;  // boottest's _df: 180 - 2 slopes - absorbed groups
        const char* label;
      };
      const AregCase kAreg[] = {
          {"areg y x1 x2, absorb(t) vce(cluster id)", 172.0, "E1-shape absorb(t)"},
          {"areg y x1 x2, absorb(id) vce(cluster id)", 148.0, "E2-shape absorb(id), nested"},
      };
      for (const AregCase& c : kAreg) {
        Reply f = run(s, c.fit);
        Reply b = run(s, "boottest x1, reps(99) seed(1) nograph");
        const double bx = coefOf(f.est, "x1", "b"), sx = seOf(f.est, "x1");
        const double p = num("p");
        w2check(f.rc == "0" && b.rc == "0" && !b.est.is_null() && near12(num("t"), bx / sx) &&
                    near12(estNum(b, "t0"), bx / sx) && near12(rcell("b"), bx) &&
                    near12(rcell("V"), sx * sx * 179.0 / c.df) && p >= 0.0 && p <= 1.0 &&
                    num("df_r") == 29,
                std::string("FU-732 boottest after areg ") + c.label +
                    ": served, r(t) = _b/_se of the areg table, r(V) = _se^2*179/_df. Fit rc " +
                    f.rc + "; got rc " + b.rc + " t " + fmt(num("t")) + " want " +
                    fmt(bx / sx) + " V " + fmt(rcell("V")) + " " + b.msg);
      }
      // The nested case is where the engine's t0 parts from areg's: the same fit
      // under reghdfe reports a different t (E4_st :1668 vs E2_st :1535).
      Reply fr = run(s, "reghdfe y x1 x2, absorb(id) vce(cluster id)");
      (void)run(s, "boottest x1, reps(99) seed(1) nograph");
      const double tReghdfe = num("t");
      Reply fa = run(s, "areg y x1 x2, absorb(id) vce(cluster id)");
      (void)run(s, "boottest x1, reps(99) seed(1) nograph");
      w2check(fr.rc == "0" && fa.rc == "0" && std::isfinite(tReghdfe) &&
                  std::fabs(num("t") - tReghdfe) > 1e-6 * std::fabs(tReghdfe),
              "areg absorb(id) cluster(id): boottest's t is areg's, not reghdfe's (E2_st :1535 vs "
              "E4_st :1668). Got areg " + fmt(num("t")) + " reghdfe " + fmt(tReghdfe));
    }
  }
  // No estimation (B1 :216/:221/:227, B2 :237/:243/:248): `No estimates detected`
  // at rc 0, r() emptied. bin/main: rc 1 `No previous estimation result.`.
  {
    piet::SessionState s(workdir);
    seed(s);
    (void)run(s, "summarize x1");
    Reply b = run(s, "boottest x1, cluster(id) reps(99) seed(1) nograph");
    Reply rp = run(s, "display r(p)");
    w2check(b.rc == "0" && b.est.is_null() && b.text == "No estimates detected" &&
                !s.eval.rValues.contains("mean") && rp.rc == "0" && rp.text == ".",
            "boottest with no estimation: `No estimates detected`, rc 0, r() emptied (w2t B2). "
            "Got rc " + b.rc + " text `" + b.text + "` r(p) `" + rp.text + "` " + b.msg);
  }
  // boottest.ado's estimator and boottype messages (E7 :1834/:1840, D6 :1405/:1411,
  // F6 :2377-2378/:2384, H1 :2491/:2497): one display block at rc 0, no bootstrap,
  // r(p) unset and summarize's r(mean) gone. bin/main: 198 for each (`requires a
  // preceding regression with vce(cluster ...)` after xtreg, re and svy; the IV
  // and logit refit guard; `option boottype not allowed`).
  {
    piet::SessionState s(workdir);
    seed(s);
    (void)run(s, "generate w = 1 + mod(_n,4)");
    struct Mirror {
      std::vector<const char*> setup;
      const char* boottest;
      const char* text;
      const char* label;
    };
    const Mirror kMirror[] = {
        {{"xtreg y x1 x2, re"}, "boottest x1, cluster(id) reps(99) seed(1) nograph",
         "Doesn't work after {xtreg, re}", "E7 xtreg, re"},
        {{"ivregress gmm y x2 (x1 = z1), vce(cluster id)"},
         "boottest x2, cluster(id) reps(99) seed(1) nograph", "GMM no longer supported",
         "D6 ivregress gmm"},
        {{"logit yb x1 x2, vce(cluster id)"},
         "boottest x1, cluster(id) reps(99) seed(1) nograph boottype(wild)",
         "Overriding estimator's cluster/robust settings with cluster(id)\n"
         "boottype(wild) not accepted after Maximum Likelihood-based estimation",
         "F6 logit boottype(wild)"},
        {{"svyset id [pweight=w], strata(g)", "svy: regress y x1 x2"},
         "boottest x1, cluster(id) reps(99) seed(1) nograph", "Doesn't work after svy",
         "H1 svy"},
    };
    for (const Mirror& m : kMirror) {
      std::string fitRc;
      for (const char* c : m.setup) fitRc += run(s, c).rc;
      (void)run(s, "summarize x1");
      Reply b = run(s, m.boottest);
      w2check(b.rc == "0" && b.est.is_null() && b.text == m.text &&
                  !s.eval.rValues.contains("p") && !s.eval.rValues.contains("mean"),
              std::string(m.label) + ": boottest.ado's message at rc 0, no bootstrap, r() "
              "emptied. Fit rc " + fitRc + "; got rc " + b.rc + " text `" + b.text + "` " + b.msg);
    }
    (void)run(s, "svyset, clear");
    // F1 defaults to the score bootstrap. Explicit score uses the identical
    // kernel and seed; neither publishes an OLS t or a coefficient estimate.
    Reply f1 = run(s, "logit yb x1 x2, vce(cluster id)");
    (void)run(s, "summarize x1");
    Reply b1 = run(s, "boottest x1, cluster(id) reps(99) seed(1) nograph");
    const bool defaultScore = scorePostingMatches(b1, s);
    Reply explicitScore = run(s, "boottest x1, cluster(id) reps(99) seed(1) nograph boottype(score)");
    w2check(f1.rc == "0" && defaultScore && scorePostingMatches(explicitScore, s) &&
                b1.est["z"] == explicitScore.est["z"] &&
                b1.est["p_symmetric"] == explicitScore.est["p_symmetric"],
            "boottest after logit defaults to the actual score test; explicit score with the same "
            "seed preserves z/p and omits OLS-only fields. Got default rc " + b1.rc +
                ", explicit rc " + explicitScore.rc + " " + b1.msg + " " + explicitScore.msg);
  }
  // FU-787/FU-822/FU-823: H1's own design has no strata() (w2t :2455 `svyset id
  // [pweight=w]`, "Strata 1: <one>"; :2497 fit_rc=0 ecmd=regress). One stratum:
  // the fit runs, its se equals the fit under a constant strata() variable,
  // e(cmd) is regress, and boottest prints the svy message. `svy: logit` without
  // strata() stays a named 198 (the engine's logit requires --strata).
  // bin/main: svyset refused at `requires strata()` and e(cmd) read `feols`.
  {
    piet::SessionState s(workdir);
    seed(s);
    (void)run(s, "generate w = 1 + mod(_n,4)");
    (void)run(s, "generate one = 1");
    Reply sv = run(s, "svyset id [pweight=w]");
    Reply f = run(s, "svy: regress y x1 x2");
    Reply ec = run(s, "display \"E|`e(cmd)'|\"");
    Reply se1 = run(s, "display \"S|\" %21.0g _se[x1]");
    (void)run(s, "summarize x1");
    Reply b = run(s, "boottest x1, cluster(id) reps(99) seed(1) nograph");
    Reply lg = run(s, "svy: logit yb x1 x2");
    (void)run(s, "svyset id [pweight=w], strata(one)");
    (void)run(s, "svy: regress y x1 x2");
    Reply se2 = run(s, "display \"S|\" %21.0g _se[x1]");
    (void)run(s, "svyset, clear");
    w2check(sv.rc == "0" && f.rc == "0" && ec.text == "E|regress|" && se1.text == se2.text &&
                se1.text != "S|" && b.rc == "0" && b.text == "Doesn't work after svy" &&
                !s.eval.rValues.contains("p") && lg.rc == "0",
            "H1 svy without strata(): one stratum, e(cmd) regress, boottest's svy message; "
            "`svy: logit` takes the one-stratum design too (BUG-1424). Got "
            "svyset rc " + sv.rc + " " + sv.msg + "; fit rc " + f.rc + " " + f.msg + "; `" +
                ec.text + "`; se `" + se1.text + "` vs `" + se2.text + "`; boottest rc " + b.rc +
                " `" + b.text + "`; svy: logit rc " + lg.rc);
  }
  // FU-238 (FU-168): boottest's option message empties r() and leaves s() (G1_r
  // :2444 `mean=. N=.`; s() is not r(), eval::replaceRClass's convention).
  // bin/main: the triple clear erased rStrings whole, s(foo) included.
  {
    piet::SessionState s(workdir);
    seed(s);
    try {
      (void)piet::doexec::executeDoSource("program define w2b_bts, sclass\n"
                                          "  sreturn local foo \"bar\"\n"
                                          "end\n",
                                          s, piet::dispatch);
    } catch (const std::exception&) {
    }
    (void)run(s, "regress y x1 x2, vce(cluster id)");
    (void)run(s, "w2b_bts");
    (void)run(s, "summarize x1");
    Reply g = run(s, "boottest x1, cluster reps(99) seed(1) nograph");
    Reply sf = run(s, "display \"S|`s(foo)'|\"");
    w2check(g.rc == "0" && g.text == "Option cluster not allowed." &&
                !s.eval.rValues.contains("mean") && sf.text == "S|bar|",
            "boottest's option message empties r() and keeps s(foo) (w2t G1_r :2444). Got rc " +
                g.rc + " text `" + g.text + "` s `" + sf.text + "`");
  }
  // FU-748: e(cmd) is the command typed after ivreghdfe/ivregress (w2t D3 :1197,
  // D4 :1282 `ecmd=ivreghdfe`; D1 :1047, D6 :1411 `ecmd=ivregress`). bin/main: `iv`.
  {
    piet::SessionState s(workdir);
    seed(s);
    (void)run(s, "ivreghdfe y x2 (x1 = z1), absorb(id t) cluster(id)");
    Reply c1 = run(s, "display \"`e(cmd)'\"");
    (void)run(s, "ivregress 2sls y x2 (x1 = z1), vce(cluster id)");
    Reply c2 = run(s, "display \"`e(cmd)'\"");
    w2check(c1.text == "ivreghdfe" && c2.text == "ivregress",
            "e(cmd) after ivreghdfe / ivregress is the typed command (w2t D4 :1282, D1 :1047). Got `" +
                c1.text + "` / `" + c2.text + "`");
  }

  // ── BUG-759 (FU-28): e(vce) after ivregress 2sls ────────────────────────
  {
    piet::SessionState s(workdir);
    seed(s);
    Reply d = run(s, "ivregress 2sls y x1 (x2 = z1)");
    Reply dStr = run(s, kECmdVce);
    w2check(d.rc == "0" && dStr.text.find("|unadjusted|") != std::string::npos &&
                dStr.text.size() >= 1 && dStr.text.back() == '|',
            "w2d_uncovered ivregress_*_ctl: default `ivregress 2sls` e(vce) `unadjusted`, e(vcetype) "
            "empty (main: `iid`). Got rc " + d.rc + " `" + dStr.text + "`");
    Reply r = run(s, "ivregress 2sls y x1 (x2 = z1), vce(robust)");
    Reply rStr = run(s, "display \"`e(vce)'|`e(vcetype)'\"");
    w2check(r.rc == "0" && rStr.text == "robust|Robust",
            "ivregress 2sls vce(robust) keeps e(vce) `robust`, e(vcetype) `Robust`. Got `" +
                rStr.text + "`");
    Reply c = run(s, "ivregress 2sls y x1 (x2 = z1), vce(cluster id)");
    Reply cStr = run(s, "display \"`e(vce)'|`e(vcetype)'\"");
    w2check(c.rc == "0" && cStr.text == "cluster|Robust",
            "ivregress 2sls vce(cluster id) keeps e(vce) `cluster`, e(vcetype) `Robust`. Got `" +
                cStr.text + "`");
  }

  // ── BUG-788..791 (FU-117 / FU-147 / FU-160): vce()/cluster() list shape ──
  {
    piet::SessionState s(workdir);
    seed(s);
    // FU-117. Main: every one of these fitted HC1 at rc 0 (the token after the
    // vce type was dropped). regress_vce_robust_x w2l_vce_shape.log:407 and
    // areg_vce_robust_x :764 are r(198) with empty e(); ivreghdfe :3580 is 198.
    // logit/probit/xtreg/ivregress and vce(iid x1)/vce(hc1 x1) are UNPINNED
    // predictions (Stata's _vce_parse), cited to FU-117.
    struct Arm {
      const char* oracle;
      const char* cmd;
    };
    const Arm trailing[] = {
        {"regress_vce_robust_x", "regress y x1 x2, vce(robust x1)"},
        {"areg_vce_robust_x", "areg y x1 x2, absorb(id) vce(robust x1)"},
        {"ivreghdfe_vce_robust_x", "ivreghdfe y x1 (x2 = z1), absorb(id) vce(robust x1)"},
        {"unpinned regress vce(iid x1)", "regress y x1 x2, vce(iid x1)"},
        {"unpinned regress vce(hc1 x1)", "regress y x1 x2, vce(hc1 x1)"},
        {"unpinned logit vce(robust x1)", "logit yb x1 x2, vce(robust x1)"},
        {"unpinned probit vce(robust x1)", "probit yb x1 x2, vce(robust x1)"},
        {"unpinned xtreg,fe vce(robust x1)", "xtreg y x1 x2, fe vce(robust x1)"},
        {"unpinned ivregress vce(robust x1)", "ivregress 2sls y x1 (x2 = z1), vce(robust x1)"},
    };
    for (const auto& a : trailing) {
      piet::SessionState f(workdir);
      seed(f);
      Reply r = run(f, a.cmd);
      Reply e = run(f, "display \"`e(cmd)'|\"");
      w2check(r.rc == "198" && r.est.is_null() && e.text == "|" &&
                  r.msg.find("x1") != std::string::npos,
              std::string("FU-117 ") + a.oracle + ": `" + a.cmd +
                  "` is r(198) naming the extra token, no e(). Got rc " + r.rc + " e(cmd) `" +
                  e.text + "` " + r.msg);
    }
    // Controls: the plain spellings and the community commands keep rc 0.
    for (const char* c : {"regress y x1 x2, vce(robust)", "regress y x1 x2, robust",
                          "areg y x1 x2, absorb(id) vce(robust)"}) {
      Reply r = run(s, c);
      Reply v = run(s, kECmdVce);
      w2check(r.rc == "0" && v.text.size() > 6 && v.text.find("|Robust") != std::string::npos,
              std::string("FU-117 control (regress_ctl_robust): `") + c + "` still fits HC1. Got rc " +
                  r.rc + " `" + v.text + "` " + r.msg);
    }
    {
      // reghdfe_vce_robust_x :4034 `reghdfe|robust|Robust`, ppmlhdfe_vce_robust_x :2972
      // `ppmlhdfe|robust|Robust`: the community commands accept the extra token.
      Reply h = run(s, "reghdfe y x1 x2, absorb(id) vce(robust x1)");
      Reply hv = run(s, kECmdVce);
      w2check(h.rc == "0" && hv.text == "reghdfe|robust|Robust",
              "w2l_vce_shape reghdfe_vce_robust_x (control): rc 0, reghdfe|robust|Robust. Got rc " +
                  h.rc + " `" + hv.text + "` " + h.msg);
      // e(cmd) reads `fepois` here on main as well (the oracle says `ppmlhdfe`): not this
      // task's, so only the accepted rc and the vce fields are pinned.
      Reply pp = run(s, "ppmlhdfe cnt x1, absorb(id) vce(robust x1)");
      Reply pv = run(s, "display \"`e(vce)'|`e(vcetype)'\"");
      w2check(pp.rc == "0" && pv.text == "robust|Robust",
              "w2l_vce_shape ppmlhdfe_vce_robust_x (control): rc 0, e(vce) robust|Robust. Got rc " +
                  pp.rc + " `" + pv.text + "` " + pp.msg);
    }

    // FU-147. Main: rc 1 `column not found`. No captured arm; class oracle
    // bug483_484_control_oracle.log:121-125 (`variable nosuchcol not found`,
    // rc 111). Unpinned across regress/areg/xtreg/reghdfe and the legacy form.
    const Arm missing[] = {
        {"regress vce(cluster nosuchvar)", "regress y x1 x2, vce(cluster nosuchvar)"},
        {"areg vce(cluster nosuchvar)", "areg y x1 x2, absorb(id) vce(cluster nosuchvar)"},
        {"xtreg vce(cluster nosuchvar)", "xtreg y x1 x2, fe vce(cluster nosuchvar)"},
        {"reghdfe vce(cluster id nosuchvar)", "reghdfe y x1, absorb(id) vce(cluster id nosuchvar)"},
        {"reghdfe vce(cluster id id2 nosuchvar)",
         "reghdfe y x1, absorb(id) vce(cluster id id2 nosuchvar)"},
        {"reghdfe legacy cluster(nosuchvar)", "reghdfe y x1, absorb(id) cluster(nosuchvar)"},
        {"logit vce(cluster nosuchvar)", "logit yb x1 x2, vce(cluster nosuchvar)"},
    };
    for (const auto& a : missing) {
      piet::SessionState f(workdir);
      seed(f);
      Reply r = run(f, a.cmd);
      Reply e = run(f, "display \"`e(cmd)'|\"");
      w2check(r.rc == "111" && r.est.is_null() && e.text == "|" &&
                  r.msg == "variable nosuchvar not found",
              std::string("FU-147 ") + a.oracle + ": `" + a.cmd +
                  "` is r(111) `variable nosuchvar not found`, no e(). Got rc " + r.rc + " e(cmd) `" +
                  e.text + "` " + r.msg);
    }

    // FU-160. Main: the legacy two-name list fitted two-way at rc 0.
    // regress_opt_c2 :268-275 r(198), areg_opt_c2 :624-630 r(103); xtreg is an
    // unpinned prediction (areg's). Controls: the vce(cluster a b) forms stay
    // multiway (regress_vce_c2_se :257, areg_vce_c2_se :613), reghdfe's legacy
    // list is accepted (:3838), ppmlhdfe's (:2757). A missing name in regress's
    // and areg's legacy cluster() carries that option's tail, and is looked up
    // before the count (StataBE 19.5, 2026-11-03: `regress y x, cluster(nosuch)`
    // and `cluster(kgr nosuch)` r(198) "variable nosuch not found / option
    // cluster() incorrectly specified"; areg's r(111) "... / (error in option
    // cluster())").
    struct Opt {
      const char* oracle;
      const char* cmd;
      const char* rc;
      const char* msg;
    };
    const Opt legacy[] = {
        {"regress_opt_c2", "regress y x1 x2, cluster(id id2)", "198",
         "too many variables specified\noption cluster() incorrectly specified"},
        {"areg_opt_c2", "areg y x1 x2, absorb(id) cluster(id id2)", "103",
         "cluster():  too many variables specified"},
        {"unpinned xtreg legacy c2", "xtreg y x1 x2, fe cluster(id id2)", "103",
         "cluster():  too many variables specified"},
        {"order: regress legacy c2 + missing", "regress y x1 x2, cluster(id nosuchvar)", "198",
         "variable nosuchvar not found\noption cluster() incorrectly specified"},
        {"regress legacy cluster(nosuchvar)", "regress y x1 x2, cluster(nosuchvar)", "198",
         "variable nosuchvar not found\noption cluster() incorrectly specified"},
        {"areg legacy cluster(nosuchvar)", "areg y x1 x2, absorb(id) cluster(nosuchvar)", "111",
         "variable nosuchvar not found\n(error in option cluster())"},
    };
    for (const auto& a : legacy) {
      piet::SessionState f(workdir);
      seed(f);
      Reply r = run(f, a.cmd);
      Reply e = run(f, "display \"`e(cmd)'|\"");
      w2check(r.rc == a.rc && r.est.is_null() && e.text == "|" && r.msg == a.msg,
              std::string("w2l_vce_shape ") + a.oracle + ": `" + a.cmd + "` is r(" + a.rc +
                  ") `" + a.msg + "`, no e(). Got rc " + r.rc + " e(cmd) `" + e.text + "` " + r.msg);
    }
    // The oracle's se (regress :257 .031594230522256, areg :613 .0500623565759049)
    // rests on runiform()/rnormal() draws that seed() replaces by fixed
    // arithmetic, so it cannot be reproduced on this frame. The se below is a
    // CONVENTION pin on THIS frame, not an oracle value: Stata's built-in
    // multiway convention (per-combination small-sample
    // factors, negative eigenvalues set to 0; BUG-848), whose oracle is
    // w2l_vce_shape_det.log:258 regress_vce_c2_se .0643525900770342 and :614
    // areg_vce_c2_se .0778721308909151. Cross-check: a numpy run of that
    // convention on this frame with Stata float storage gives .049200867086478695
    // and .065481593469633545 (~7e-15). Frozen main's reghdfe-style Gmin fit
    // (.042592773724794787 / .065010518657989375) was pinned here before and
    // certified the defect. The oracle also contributes rc 0, e(vce) cluster,
    // e(vcetype) Robust and e(N_clust) `.` (:252, :608; BUG-849).
    struct Ctl {
      const char* oracle;
      const char* cmd;
      double se;
    };
    const Ctl ctl[] = {
        {"regress_vce_c2 :252", "regress y x1 x2, vce(cluster id id2)", .049200867086479028},
        {"areg_vce_c2 :608", "areg y x1 x2, absorb(id) vce(cluster id id2)", .065481593469633476},
    };
    for (const auto& c : ctl) {
      piet::SessionState f(workdir);
      seed(f);
      Reply r = run(f, c.cmd);
      Reply v = run(f, "display \"`e(vce)'|`e(vcetype)'|\" e(N_clust)");
      const double se = seOf(r.est, "x1");
      w2check(r.rc == "0" && v.text == "cluster|Robust|." && std::isfinite(se) &&
                  std::fabs(se - c.se) <= 1e-9 * std::fabs(c.se),
              std::string("w2l_vce_shape ") + c.oracle + " (control): `" + c.cmd +
                  "` is multiway at rc 0, e(vce) cluster, e(vcetype) Robust, e(N_clust) `.`, se(x1) " +
                  fmt(c.se) + " (Stata built-in multiway convention, rtol 1e-9). Got rc " + r.rc +
                  " `" + v.text + "` se " + fmt(se) + " " + r.msg);
    }
    // FU-385 (BUG-850): the post-estimation refit carries the fit's
    // --multiway-subset-factors (fitConventionFlagsM4), so after a two-way
    // regress lincom/test/testparm/nlcom answer under the table's V. Oracle:
    // w2l_vce_shape_det.log:263 regress_vce_c2_sex1 lincom .0643525900770342 ==
    // :258 _se[x1]; on this frame the identity r(se) == _se[x1] (the table's
    // .049200867086479028 above) is the pin. bin/main refused all four at rc 198
    // (BUG-850 interim); before that it answered on reghdfe's Gmin factor
    // (.0425927737).
    {
      piet::SessionState f(workdir);
      seed(f);
      Reply fit = run(f, "regress y x1 x2, vce(cluster id id2)");
      const double b = coefOf(fit.est, "x1", "b"), se = seOf(fit.est, "x1");
      const double F0 = (b / se) * (b / se);
      auto rv = [&](const char* k) {
        const double* v = f.eval.rValues.find(k);
        return v ? *v : kNaN;
      };
      auto near = [](double a, double e, double tol) {
        return std::isfinite(a) && std::isfinite(e) && std::fabs(a - e) <= tol * std::fabs(e);
      };
      Reply l = run(f, "capture noisily lincom x1");
      Reply lrc = run(f, "display _rc");
      const double lse = rv("se"), lb = rv("estimate");
      w2check(fit.rc == "0" && lrc.text == "0" && near(lse, se, 1e-12) && near(lb, b, 1e-12),
              "FU-385 w2l_vce_shape_det.log:263: lincom x1 after `regress y x1 x2, vce(cluster id "
              "id2)` is rc 0 with r(estimate) == _b[x1] " + fmt(b) + " and r(se) == _se[x1] " +
                  fmt(se) + " (rtol 1e-12; main: r(198)). Got _rc `" + lrc.text + "` r(estimate) " +
                  fmt(lb) + " r(se) " + fmt(lse) + " " + l.msg + l.text);
      for (const char* verb : {"test x1", "testparm x1"}) {
        Reply t = run(f, std::string("capture noisily ") + verb);
        Reply trc = run(f, "display _rc");
        const double F = rv("F");
        w2check(trc.text == "0" && near(F, F0, 1e-9),
                std::string("FU-385 w2l_vce_shape_det.log:263: `") + verb +
                    "` after `regress y x1 x2, vce(cluster id id2)` is rc 0 with r(F) == "
                    "(_b[x1]/_se[x1])^2 " + fmt(F0) + " (rtol 1e-9; main: r(198)). Got _rc `" +
                    trc.text + "` r(F) " + fmt(F) + " " + t.msg + t.text);
      }
      Reply n = run(f, "nlcom _b[x1]");
      const double nse = coefOf(n.est, "_nl_1", "se");
      w2check(n.rc == "0" && near(nse, lse, 1e-9),
              "FU-385 w2l_vce_shape_det.log:263: nlcom _b[x1] after `regress y x1 x2, vce(cluster "
              "id id2)` is rc 0 with se == lincom's r(se) " + fmt(lse) +
                  " (T2: nlcom's Jacobian is numeric; main: r(198)). Got rc " + n.rc +
                  " se " + fmt(nse) + " " + n.msg + n.text);
    }
    {  // Controls: one-way regress and two-way reghdfe answer lincom as on bin/main.
      piet::SessionState f(workdir);
      seed(f);
      Reply fit = run(f, "regress y x1 x2, vce(cluster id)");
      Reply l = run(f, "capture noisily lincom x1");
      Reply rc = run(f, "display _rc");
      const double* rse = f.eval.rValues.find("se");
      const double se = seOf(fit.est, "x1");
      w2check(fit.rc == "0" && rc.text == "0" && rse != nullptr && std::isfinite(se) &&
                  std::fabs(*rse - se) <= 1e-12 * std::fabs(se),
              "FU-385 interim (control): lincom x1 after one-way `regress ..., vce(cluster id)` "
              "is rc 0 with r(se) = _se[x1] " + fmt(se) + ". Got _rc `" + rc.text + "` r(se) " +
                  (rse ? fmt(*rse) : std::string("unset")) + " " + l.msg);
      Reply h = run(f, "reghdfe y x1 x2, absorb(id) cluster(id id2)");
      Reply hl = run(f, "capture noisily lincom x1");
      Reply hrc = run(f, "display _rc");
      const double* hse = f.eval.rValues.find("se");
      const double kMainSe = .05944671427558015;  // bin/main 4c4ba2fc on this frame
      w2check(h.rc == "0" && hrc.text == "0" && hse != nullptr &&
                  std::fabs(*hse - kMainSe) <= 1e-12 * kMainSe,
              "FU-385 interim (control): lincom x1 after two-way reghdfe is rc 0 with r(se) " +
                  fmt(kMainSe) + " as on bin/main. Got _rc `" + hrc.text + "` r(se) " +
                  (hse ? fmt(*hse) : std::string("unset")) + " " + hl.msg);
    }
    {
      Reply h = run(s, "reghdfe y x1 x2, absorb(id) cluster(id id2)");
      Reply hv = run(s, "display \"`e(cmd)'|`e(vce)'|\" e(N_clust)");
      w2check(h.rc == "0" && hv.text == "reghdfe|cluster|7",
              "w2l_vce_shape reghdfe_opt_c2 (control, :3838): rc 0, two-way. Got rc " + h.rc + " `" +
                  hv.text + "` " + h.msg);
      Reply pp = run(s, "ppmlhdfe cnt x1, absorb(id) cluster(id id2)");
      Reply pv = run(s, "display \"`e(vce)'|\" e(N_clust)");
      w2check(pp.rc == "0" && pv.text == "cluster|7",
              "w2l_vce_shape ppmlhdfe_opt_c2 (control, :2757): rc 0, two-way. Got rc " + pp.rc +
                  " `" + pv.text + "` " + pp.msg);
    }
  }

  // ── W2T WRE / LIML VCE / multiway: the oracle's own fixture ─────────────────
  // tests/eval/golden/aer_w2/w2t_boottest_family.do:61-79 (w2l_vce_shape_det.do's
  // frame plus yw/z/endog). Stata values: the capture's log (D1_st :1053, D2_st
  // :1111, D3_st :1203, boottest_opt_c2's t(6) 32.3278 at
  // w2l_vce_shape_det.log:4152) and the method probe of this change
  // (StataBE 19.5, boottest 4.5.3, same fixture), %18.0g. Deterministic statistics
  // at T2 (rtol 1e-9); p is a draw (T5) and is checked only for its range and
  // its reproducibility. bin/main: every boottest here r(198), and `ivregress
  // liml ..., vce()` r(198).
  {
    piet::SessionState s(workdir);
    static const char* kW2t[] = {
        "clear",
        "set obs 180",
        "generate id = ceil(_n/6)",
        "generate t = mod(_n-1,6)+1",
        "generate id2 = mod(_n,7)+1",
        "generate x1 = mod(_n*7,11) + mod(_n,5)/8",
        "generate z1 = mod(_n*5,13) + x1/2 + mod(_n*17,19)/16",
        "generate x2 = 0.5*z1 + mod(_n*3,5) + mod(_n*13,7)/8 + mod(_n*23,29)/32",
        "generate y = 1 + 2*x1 - x2 + id/4 + mod(_n*11,17)/4 + (mod(_n*37,41)/64 + mod(_n*53,59)/64 + mod(_n*29,31)/64 - 1)*3",
        "xtset id t",
        "generate yw = y - 31*x1/16",
        "generate z = mod(_n*5,13)/2 + mod(_n*31,37)/16",
        "generate endog = z/2 + x1/8 + mod(_n*37,41)/32 + mod(_n*7,13)/16",
    };
    for (const char* c : kW2t) (void)piet::dispatch(c, s);
    auto rnum = [&](const char* key) {
      const double* v = s.eval.rValues.find(key);
      return v ? *v : kNaN;
    };
    auto close9 = [](double got, double want) {
      return std::isfinite(got) && std::fabs(got - want) <= 1e-9 * std::fabs(want);
    };
    struct Arm {
      const char* fit;
      const char* test;
      const char* stat;   // "z" or "t"
      double want;
      double dfr;         // r(df_r) when small, else NaN
      const char* label;
    };
    const Arm kArms[] = {
        {"ivregress 2sls yw x1 (endog = z z1), vce(cluster id)",
         "boottest x1, cluster(id) reps(99) seed(1) nograph", "z", .1434202311956093, kNaN,
         "D1_ivregress_2sls_st :1053"},
        {"ivregress liml yw x1 (endog = z z1), vce(cluster id)",
         "boottest x1, cluster(id) reps(99) seed(1) nograph", "z", .1683750187999031, kNaN,
         "D2_ivregress_liml_st :1111"},
        {"ivreghdfe yw x1 (endog = z), absorb(t) cluster(id)",
         "boottest x1, cluster(id) reps(99) seed(1) nograph", "t", 2.286824317666255, 29,
         "D3_ivreghdfe_a1_st :1203 (no FE, no constant, FEdfadj = e(df_a) 6)"},
        {"ivregress 2sls yw x1 (endog = z z1), vce(cluster id)",
         "boottest endog = -1.5, cluster(id) reps(99) seed(1) nograph", "z", -1.162546447947244,
         kNaN, "probe D1e (endogenous coefficient, nonzero null)"},
        {"ivregress liml yw x1 (endog = z z1), vce(cluster id)",
         "boottest endog = -1.5, cluster(id) reps(99) seed(1) nograph", "z", -1.213777178798285,
         kNaN, "probe D2e (LIML, endogenous coefficient)"},
        {"ivregress 2sls yw x1 (endog = z z1), vce(cluster id) small",
         "boottest x1, cluster(id) reps(99) seed(1) nograph", "t", .1402196592967912, 29,
         "probe D1s (ivregress small)"},
        {"regress y x1 x2, vce(cluster id)",
         "boottest x1, cluster(id id2) reps(99) seed(1) nograph", "t", 32.32783204036483, 6,
         "boottest_opt_c2 (multiway)"},
        {"regress yw x1 x2, vce(cluster id)",
         "boottest x1, cluster(id id2) reps(99) seed(1) nograph", "t", .8224680704119507, 6,
         "probe C2yw (multiway)"},
    };
    for (const Arm& a : kArms) {
      Reply f = run(s, a.fit);
      (void)run(s, "summarize x1");
      Reply b = run(s, a.test);
      const double got = rnum(a.stat), p1 = rnum("p");
      const bool small = std::isfinite(a.dfr);
      const bool other = s.eval.rValues.contains(small ? "z" : "t");
      const bool dfOk = small ? rnum("df_r") == a.dfr : !s.eval.rValues.contains("df_r");
      (void)run(s, a.fit);
      Reply b2 = run(s, a.test);
      const double p2 = rnum("p");
      w2check(f.rc == "0" && b.rc == "0" && close9(got, a.want) && !other && dfOk &&
                  std::isfinite(p1) && p1 >= 0 && p1 <= 1 && p1 == p2 &&
                  !s.eval.rValues.contains("mean") && !s.eval.rValues.contains("N_clust"),
              std::string("W2T ") + a.label + ": `" + a.test + "` after `" + a.fit + "` r(" +
                  a.stat + ") " + fmt(a.want) + ", r(df_r) " + fmt(a.dfr) +
                  ", r(p) reproducible, r(mean)/r(N_clust) unset. Fit rc " + f.rc + "; got rc " +
                  b.rc + " stat " + fmt(got) + " df_r " + fmt(rnum("df_r")) + " p " + fmt(p1) +
                  "/" + fmt(p2) + " " + b.msg + b2.msg);
    }
    // LIML robust/cluster VCE (matches Stata's ivregress liml), probe values.
    struct Vce {
      const char* fit;
      double want;
    };
    const Vce kVce[] = {
        {"ivregress liml yw x1 (endog = z z1), vce(robust)", .0809669411165636},
        {"ivregress liml yw x1 (endog = z z1), vce(cluster id)", .0647214949059624},
        {"ivregress liml yw x1 (endog = z z1), vce(robust) small", .0816502185562152},
        {"ivregress liml yw x1 (endog = z z1), vce(cluster id) small", .0661987898793233},
    };
    for (const Vce& v : kVce) {
      Reply f = run(s, v.fit);
      const double se = seOf(f.est, "x1");
      w2check(f.rc == "0" && close9(se, v.want),
              std::string("W2T LIML VCE: `") + v.fit + "` _se[x1] " + fmt(v.want) + ". Got rc " +
                  f.rc + " se " + fmt(se) + " " + f.msg);
    }
  }

  fs::remove_all(workdir, ec);
}
