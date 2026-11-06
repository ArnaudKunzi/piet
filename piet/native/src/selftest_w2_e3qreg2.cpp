// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// E3 arms for `qreg2` (handlers_e3_qreg2.cpp, fitted natively). The refusal
// layer's arms are the oracle's, tests/eval/golden/aer_w2/w2j_e3_qreg2.log: R01
// [pw=] r(101) :1996, R04 vce(robust) r(198) :2029, R06 cluster(nosuchvar) r(111)
// :2051, R07 robust r(198) :2062, R08 [fw=] with cluster() rc 0 and e() untouched
// :2076, R10 q(0) r(198) :2116. A call that passes it fits: rc 0 and qreg2's e()
// (C00 :276 `qreg2|||201|.|196|4|5|`: e(cmd) qreg2, e(vce) empty, e(N), e(df_r) =
// N - rank).
#include "selftest_w2.h"

#include <cmath>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>

#include "piet/dispatcher.h"
#include "piet/eval/errors.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

void testW2E3Qreg2() {
  std::printf("[selftest] --- W2 E3 qreg2: refusal layer + native fit ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_w2e3_qreg2").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  static const char* kLines[] = {
      "qreg2 price mpg"};
  for (const char* line : kLines) {
    piet::SessionState s(workdir);
    for (const char* c : {"clear", "set obs 20", "generate double price = 3000 + 17*_n*_n",
                          "generate double mpg = 10 + mod(_n*7,13)", "generate double weight = 2000 + 31*_n",
                          "generate byte foreign = mod(_n,2)"})
      (void)piet::dispatch(c, s);
    std::string rc = "0", reason;
    try {
      for (const auto& b : piet::dispatch(line, s)) {
        if (b.is_object() && b.value("type", std::string()) == "error") {
          rc = piet::eval::coarseRc(b);
          reason = b.value("reason", std::string());
        }
      }
    } catch (const std::exception& e) {
      rc = std::string("EXC ") + e.what();
    }
    w2check(rc == "0", std::string("qreg2: `") + line + "` fits at rc 0 (w2j C00 :276). Got rc " + rc +
                           " reason " + reason);
    const std::string* cmd = s.eval.eStrings.find("cmd");
    w2check(s.lastModel().has_value() && cmd != nullptr && *cmd == "qreg2",
            std::string("qreg2: `") + line + "` posts e(cmd) qreg2");
    const double* nObs = s.eval.eScalars.find("N");
    const double* dfr = s.eval.eScalars.find("df_r");
    w2check(nObs != nullptr && *nObs == 20 && dfr != nullptr && *dfr == 18,
            std::string("qreg2: `") + line + "` posts e(N) 20 and e(df_r) N - k = 18");
    const std::string* vce = s.eval.eStrings.find("vce");
    w2check(vce == nullptr || vce->empty(), "qreg2: e(vce) is empty (QREG:157 under version 11)");
  }
  {
    // w2j_e3_qreg2 dataset B (the do-file's closed-form generates). M00-M01: the
    // unique-optimum fit (z, q(.37)) and its `margins, dydx(clinic) post`. M07-M08:
    // the all-factor median fit, whose LP optimum is not unique. qreg2 posts PIET's
    // own optimal vertex with a note (User decision 2026-10-27), and the margins
    // record, which does not depend on the vertex, equals the unique-fit one
    // (Stata: M08 PREC byte-identical to M01).
    piet::SessionState s(workdir);
    for (const char* c :
         {"clear", "set obs 241", "generate long id = _n",
          "generate int hfac = 1 + mod(_n*13 + floor(_n/7), 30)", "generate byte tr = mod(hfac, 2)",
          "generate byte month = 1 + mod(floor(_n/12), 6)", "generate byte sid = 1 + mod(_n, 3)",
          "generate byte cash = mod(floor(_n/3), 2)", "generate byte risk = mod(floor(_n/6), 2)",
          "generate byte clinic = mod(_n*7 + hfac, 3)",
          "generate double e0 = (mod(_n*_n*7 + _n*13, 1009) - 504)/150.3 + mod(_n*_n*_n, 37)/41",
          "generate double z = mod(_n*_n*3 + _n, 61)/7",
          "generate double y = 5 + 1.2*tr + 0.3*month + 0.5*sid*cash - 0.4*risk + 0.9*(clinic==1) + "
          "2.1*(clinic==2) + 0.25*z + e0*(1 + clinic/2) + (mod(hfac*41, 13) - 6)/4"})
      (void)piet::dispatch(c, s);
    struct Fit {
      std::string rc = "0";
      bool noted = false;
      bool posted = false;
    };
    auto fit = [&](const char* command) {
      Fit f;
      for (const auto& b : piet::dispatch(command, s)) {
        if (!b.is_object()) continue;
        if (b.value("type", std::string()) == "error") f.rc = piet::eval::coarseRc(b);
        if (b.contains("warnings") && b["warnings"].is_array())
          for (const auto& w : b["warnings"])
            if (w.is_string() && w.get<std::string>().find("more than one optimal solution") != std::string::npos)
              f.noted = true;
      }
      const std::string* cmd = s.eval.eStrings.find("cmd");
      bool hasV = false;
      for (const auto& kv : s.eval.eMatrices)
        if (kv.first == "V") hasV = true;
      f.posted = cmd != nullptr && *cmd == "qreg2" && hasV;
      return f;
    };
    // the do-file's _e3_m record fields (e(b)/e(V) values are not in it)
    auto marginsRecord = [&]() {
      std::string rc = "0", text;
      for (const char* c :
           {"margins, dydx(clinic) post",
            "local __nb : colfullnames e(b)",
            "display \"`e(cmd)'|`e(est_cmd)'|\" e(N) \"|\" e(df_r) \"|\" e(N_clust) \"|`e(vce)'|`e(vcetype)'|"
            "`e(model_vce)'|`e(model_vcetype)'|`e(xvars)'|`e(derivatives)'|`__nb'|\" colsof(e(b))"}) {
        for (const auto& b : piet::dispatch(c, s)) {
          if (!b.is_object()) continue;
          const std::string ty = b.value("type", std::string());
          if (ty == "error" && rc == "0") rc = piet::eval::coarseRc(b);
          if (ty == "display" && std::string(c).rfind("display", 0) == 0) text += b.value("text", std::string());
        }
      }
      return rc + "|" + text;
    };
    const Fit u = fit("qreg2 y z i.(tr month sid#cash#risk clinic), cluster(hfac) q(.37)");
    w2check(u.rc == "0" && u.posted && !u.noted,
            "qreg2: the unique-optimum fit (w2j M00) posts without the non-unique note. Got rc " + u.rc);
    const std::string mUnique = marginsRecord();
    const Fit nu = fit("qreg2 y i.(tr month sid#cash#risk clinic), cluster(hfac)");
    w2check(nu.rc == "0" && nu.posted && nu.noted,
            "qreg2: a non-unique median optimum (w2j M07) posts e(b)/e(V) with the note. Got rc " + nu.rc);
    const std::string mNonUnique = marginsRecord();
    w2check(mUnique.rfind("0|margins|qreg2|241|", 0) == 0 &&
                mUnique.find("|0b.clinic 1.clinic 2.clinic|") != std::string::npos,
            "qreg2: margins, dydx(clinic) post after the unique fit (w2j M01). Got " + mUnique);
    w2check(mNonUnique == mUnique,
            "qreg2: the margins record after a non-unique fit equals the unique-fit one (w2j M08 = M01). Got `" +
                mNonUnique + "` vs `" + mUnique + "`");
  }
  {
    // w2p_estim_ev A_qreg_fw_q50 / A_qreg_iw_q50 (the do-file's fixture, lines 12-22).
    piet::SessionState s(workdir);
    for (const char* c : {"clear", "set obs 60", "generate double id = ceil(_n/6)",
                          "generate double x1 = mod(_n*7,11) + mod(_n,5)/10",
                          "generate double x2 = mod(_n*3,5) + mod(_n*13,7)/10",
                          "generate double w = 1 + mod(_n,4)",
                          "generate double y = 1 + 2*x1 - x2 + id/3 + mod(_n*11,17)/5"})
      (void)piet::dispatch(c, s);
    auto run = [&](const char* command) {
      std::string rc = "0";
      for (const auto& block : piet::dispatch(command, s))
        if (block.is_object() && block.value("type", std::string()) == "error")
          rc = piet::eval::coarseRc(block);
      return rc;
    };
    auto coef = [&](const char* field) {
      if (s.lastEstBlock.has_value() && s.lastEstBlock->contains("coeftable"))
        for (const auto& row : (*s.lastEstBlock)["coeftable"])
          if (row.value("name", std::string()) == "x1" && row.contains(field) && row[field].is_number())
            return row[field].get<double>();
      return std::nan("");
    };
    auto escalar = [&](const char* name) {
      const double* v = s.eval.eScalars.find(name);
      return v != nullptr ? *v : std::nan("");
    };
    auto rel = [](double a, double b) { return std::fabs(a - b) / std::fabs(b); };
    w2check(run("qreg y x1 x2 [fw=w], quantile(.5)") == "0", "qreg [fw=]: rc 0");
    w2check(rel(coef("b"), 2.017204301075269) < 1e-12 && rel(coef("se"), .050665561826678) < 1e-12,
            "qreg [fw=]: b/se of the replicated data (A_qreg_fw_q50 2.017204301075269 .050665561826678)");
    w2check(escalar("N") == 150 && escalar("df_r") == 147,
            "qreg [fw=]: e(N) is the sum of the weights, e(df_r) N - rank");
    // StataBE probe on this fixture at q .37: e(q_v) +1.5666666666666X+003 (the
    // first sorted y whose cumulative weight reaches .37*150 = 55.5).
    w2check(run("qreg y x1 x2 [fw=w], quantile(.37)") == "0" &&
                escalar("q_v") == 0x1.5666666666666p+3,
            "qreg [fw=] q(.37): e(q_v) is the weighted raw quantile");
    w2check(run("qreg y x1 x2 [iw=w], quantile(.5)") == "0", "qreg [iw=]: rc 0");
    const double ise = coef("se");
    w2check(rel(coef("b"), 2.017204301075269) < 1e-12 && rel(ise, .0293657991128867) < 1e-12,
            "qreg [iw=]: row-count bandwidth and X'W^2X (A_qreg_iw_q50 .0293657991128867)");
    w2check(escalar("N") == 60 && escalar("sum_w") == 150, "qreg [iw=]: e(N) the row count, e(sum_w) 150");
    (void)run("replace w = w/2");
    w2check(run("qreg y x1 x2 [iw=w], quantile(.5)") == "0" && rel(coef("se"), 2 * ise) < 1e-12,
            "qreg [iw=]: fractional importance weights fit; halving them doubles the iid se");
    w2check(run("qreg y x1 x2 [fw=w], quantile(.5)") == "401",
            "qreg [fw=]: noninteger frequency weights are r(401)");
  }
  {
    // A non-unique median optimum (two groups of 20, each median anywhere between
    // its 10th and 11th value): qreg posts its fit with the vertex-invariant iid
    // e(V) and e(sum_adev) (w2j M10/F05), and notes the non-uniqueness.
    piet::SessionState s(workdir);
    for (const char* c : {"clear", "set obs 40", "generate double y = _n", "generate double d = _n > 20"})
      (void)piet::dispatch(c, s);
    std::string rc = "0";
    bool noted = false;
    for (const auto& b : piet::dispatch("qreg y d", s)) {
      if (b.is_object() && b.value("type", std::string()) == "error") rc = piet::eval::coarseRc(b);
      if (b.is_object() && b.contains("warnings") && b["warnings"].is_array() && !b["warnings"].empty())
        noted = true;
    }
    const double* adev = s.eval.eScalars.find("sum_adev");
    bool hasV = false;
    for (const auto& kv : s.eval.eMatrices)
      if (kv.first == "V") hasV = true;
    w2check(rc == "0" && hasV && adev != nullptr && std::fabs(*adev - 100.0) < 1e-9 && noted,
            "qreg: a non-unique median optimum posts e(V), e(sum_adev) 100 and a note. Got rc " + rc);
  }
  struct Arm {
    const char* line;
    const char* rc;
    const char* arm;
  };
  static const Arm kArms[] = {{"qreg2 price mpg [pw=weight]", "101", "R01_pweight"},
                              {"qreg2 price mpg, vce(robust)", "198", "R04_vce_robust"},
                              {"qreg2 price mpg, cluster(nosuchvar)", "111", "R06_cluster_novar"},
                              {"qreg2 price mpg, robust", "198", "R07_robust_opt"},
                              {"qreg2 price mpg [fw=foreign], cluster(foreign)", "0", "R08_fw_cluster"},
                              {"qreg2 price mpg, q(0)", "198", "R10_q_zero"}};
  for (const Arm& a : kArms) {
    piet::SessionState s(workdir);
    for (const char* c : {"clear", "set obs 20", "generate double price = 3000 + 17*_n*_n",
                          "generate double mpg = 10 + mod(_n*7,13)", "generate double weight = 2000 + 31*_n",
                          "generate byte foreign = mod(_n,2) + 1", "quietly regress price mpg"})
      (void)piet::dispatch(c, s);
    std::string rc = "0";
    try {
      for (const auto& b : piet::dispatch(a.line, s)) {
        if (b.is_object() && b.value("type", std::string()) == "error") rc = piet::eval::coarseRc(b);
      }
    } catch (const std::exception& e) {
      rc = std::string("EXC ") + e.what();
    }
    w2check(rc == a.rc, std::string("qreg2 ") + a.arm + ": `" + a.line + "` is rc " + a.rc +
                            " as in w2j_e3_qreg2.log. Got rc " + rc);
    const std::string* cmd = s.eval.eStrings.find("cmd");
    w2check(cmd != nullptr && *cmd == "regress",
            std::string("qreg2 ") + a.arm + ": e() is untouched (e(cmd) stays regress, log :2076)");
  }
}
