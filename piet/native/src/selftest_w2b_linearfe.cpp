// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// W2B lane `linearfe` -- selftest arms. Report every assertion through w2check().
//
// FU-29 (weighted `xtreg, fe`) and FU-76 (the PXT e() fields of `xtreg, fe`).
// Oracle: tests/eval/golden/aer_w2/w2_pweight.log, arms X0-X14, on the do-file's
// own fixture. Every number below is the oracle's %21x value written as a C++
// hex-float literal; values are read back through a scalar and `%18.0g`, never
// through a 9-digit `display`. Each arm says what main answered: the weighted
// arms were all r(198) (`xtreg` was registered WeightPolicy::None), and after
// `xtreg, fe` e(r2) was the LSDV R-squared, e(df_m) the slope count and
// e(r2_b)/e(r2_o) `.`.
#include "selftest_w2.h"

#include <cmath>
#include <cstdio>
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

// An e()/_b[] value at full precision: through a scalar, then `%18.0g`.
// `.` (or anything unparseable) is NaN.
double num(piet::SessionState& session, const std::string& expr) {
  (void)run(session, "scalar __w2lfe_v = " + expr);
  const std::string text = run(session, "display %18.0g __w2lfe_v").text;
  const size_t a = text.find_first_not_of(' ');
  const size_t z = text.find_last_not_of(" \n");
  if (a == std::string::npos || text.substr(a, z - a + 1) == ".") return kNaN;  // `.2203` is a number
  char* end = nullptr;
  const double v = std::strtod(text.c_str() + a, &end);
  return end == text.c_str() + a ? kNaN : v;
}

bool close(double a, double b) {  // tests/eval/TOLERANCE.md T1: rtol 1e-9
  return std::isfinite(a) && std::isfinite(b) &&
         std::fabs(a - b) <= 1e-9 * std::fmax(1.0, std::fabs(b));
}

std::string fmt(double v) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.17g", v);
  return buf;
}

// w2_pweight.do's fixture, line for line.
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

void seed(piet::SessionState& session) {
  for (const auto& s : pweightFixture()) (void)piet::dispatch(s, session);
}

const char* kPstr =
    "display e(N) \"|\" e(df_r) \"|\" e(N_clust) \"|`e(vce)'|`e(vcetype)'|`e(clustvar)'|"
    "`e(wtype)'|`e(wexp)'|\"";

// One oracle PVAL/PSTR record of a weighted arm, plus its e(df_m) from PXT.
struct Arm {
  const char* name;  // oracle arm id
  const char* cmd;   // the do-file's command, verbatim
  double bX1, seX1, bCons, seCons, F, r2, sigmaE;
  const char* pstr;  // the oracle PSTR fields, `|`-joined, without the prefix
  double dfM;
};

}  // namespace

void testW2bLinearFe() {
  std::printf("[selftest] --- W2B linearfe: weighted xtreg, fe (FU-29) and its e() fields (FU-76) ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_w2blinearfe").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  // ── FU-29: the weighted fits (w2_pweight.log :349-559) ───────────────────
  {
    piet::SessionState s(workdir);
    seed(s);
    const Arm arms[] = {
        {"X3_aw_fe", "xtreg y x1 x2 [aweight=w], fe", 0x1.d0bd0e411b74ap+0, 0x1.c3346b30c1b3dp-3,
         0x1.bfa2a9248e6e0p+1, 0x1.7e19f804a80fbp-1, 0x1.8f731027c1e90p+5, 0x1.f814bbf95f9c2p-2,
         0x1.4013601975c1dp+1, "120|103|.|conventional|||aweight|= w|", 16},
        {"X4_pw_fe", "xtreg y x1 x2 [pweight=w], fe", 0x1.d0bd0e411b74ap+0, 0x1.b315e83b84cecp-3,
         0x1.bfa2a9248e6e0p+1, 0x1.4c1c9eb74cf76p-1, 0x1.416ef8f9a3442p+5, 0x1.f814bbf95f9c2p-2,
         0x1.4013601975c1dp+1, "120|14|15|cluster|Robust|id|pweight|= w|", 1},
        {"X5_pw_fe_clusterid", "xtreg y x1 x2 [pweight=w], fe cluster(id)", 0x1.d0bd0e411b74ap+0,
         0x1.b315e83b84cecp-3, 0x1.bfa2a9248e6e0p+1, 0x1.4c1c9eb74cf76p-1, 0x1.416ef8f9a3442p+5,
         0x1.f814bbf95f9c2p-2, 0x1.4013601975c1dp+1, "120|14|15|cluster|Robust|id|pweight|= w|", 1},
        {"X6_pw_fe_clustercl2", "xtreg y x1 x2 [pweight=w], fe cluster(cl2)", 0x1.d0bd0e411b755p+0,
         0x1.355568611c6a6p-3, 0x1.bfa2a9248e6c5p+1, 0x1.0f5e19eafe449p-1, 0x1.86017ccf3b9f1p+8,
         0x1.f814bbf95f9c6p-2, 0x1.4013601975c1cp+1, "120|4|5|cluster|Robust|cl2|pweight|= w|", 1},
        {"X7_pw_if_fe", "xtreg y x1 x2 [pweight=w] if t>2, fe", 0x1.e8fcfd2c2c9fdp+0,
         0x1.a65ddad519e59p-3, 0x1.963ebe7feb96cp+1, 0x1.49d14a52743c9p-1, 0x1.9654749e6e14dp+5,
         0x1.01f4269597e40p-1, 0x1.45aea3a2a214cp+1, "90|14|15|cluster|Robust|id|pweight|= w|", 1},
        {"X8_if_pw_fe", "xtreg y x1 x2 if t>2 [pweight=w], fe", 0x1.e8fcfd2c2c9fdp+0,
         0x1.a65ddad519e59p-3, 0x1.963ebe7feb96cp+1, 0x1.49d14a52743c9p-1, 0x1.9654749e6e14dp+5,
         0x1.01f4269597e40p-1, 0x1.45aea3a2a214cp+1, "90|14|15|cluster|Robust|id|pweight|= w|", 1},
        {"X9_pw_fe_comma_if", "xtreg y x1 x2 [pweight=w], fe, if t>2", 0x1.e8fcfd2c2c9fdp+0,
         0x1.a65ddad519e59p-3, 0x1.963ebe7feb96cp+1, 0x1.49d14a52743c9p-1, 0x1.9654749e6e14dp+5,
         0x1.01f4269597e40p-1, 0x1.45aea3a2a214cp+1, "90|14|15|cluster|Robust|id|pweight|= w|", 1},
        {"X13_xi_pw_fe", "xi: xtreg y x1 c.x1#c.x1 x2 [pweight=w], fe", 0x1.0b25bda7216b3p+1,
         0x1.f6dd75aaeba0ap-1, 0x1.9194e9ccdf9aap+1, 0x1.56f56fdf5b353p+0, 0x1.dd1d082573f68p+4,
         0x1.f8bf22c7c7e7cp-2, 0x1.416f5588fd023p+1, "120|14|15|cluster|Robust|id|pweight|= w|", 2},
    };
    for (const Arm& a : arms) {
      Reply r = run(s, a.cmd);
      const double got[] = {num(s, "_b[x1]"),  num(s, "_se[x1]"), num(s, "_b[_cons]"),
                            num(s, "_se[_cons]"), num(s, "e(F)"),  num(s, "e(r2)"),
                            num(s, "e(sigma_e)")};
      const double want[] = {a.bX1, a.seX1, a.bCons, a.seCons, a.F, a.r2, a.sigmaE};
      static const char* kNames[] = {"_b[x1]", "_se[x1]", "_b[_cons]", "_se[_cons]",
                                     "e(F)",   "e(r2)",   "e(sigma_e)"};
      std::string bad;
      for (int i = 0; i < 7; ++i) {
        if (!close(got[i], want[i])) bad += std::string(" ") + kNames[i] + "=" + fmt(got[i]) + " want " + fmt(want[i]);
      }
      const std::string pstr = run(s, kPstr).text;
      const double dfM = num(s, "e(df_m)");
      w2check(r.rc == "0" && bad.empty() && pstr == a.pstr && dfM == a.dfM,
              std::string("w2_pweight ") + a.name + ": `" + a.cmd + "` fits at rc 0 with the oracle's "
              "PVAL, PSTR `" + a.pstr + "` and e(df_m) " + fmt(a.dfM) + ". Main: r(198) (xtreg had no "
              "weight support). Got rc " + r.rc + " " + r.msg + bad + " pstr `" + pstr + "` df_m " +
              fmt(dfM));
    }
    // PXT fields the weighted engine reply DOES carry: N_g/df_a/r2_w (X3 :352).
    (void)run(s, "xtreg y x1 x2 [aweight=w], fe");
    const std::string ng = run(s, "display e(N_g) \"|\" e(df_a)").text;
    const double r2w = num(s, "e(r2_w)");
    w2check(ng == "15|14" && close(r2w, 0x1.f814bbf95f9c2p-2),
            "w2_pweight X3_aw_fe PXT: e(N_g) 15, e(df_a) 14, e(r2_w) +1.f814bbf95f9c2X-002. Got `" + ng +
                "` r2_w " + fmt(r2w));
    // PXT fields it does NOT carry (sigma_u/rho/corr/r2_b/r2_o; Stata has
    // numbers, X3 :352; engine follow-up FU-126): each one that reads `.` must
    // be named in a note on the block, and no field that has a value may be.
    // Couples note and value, so it stays green once the engine fills them.
    // Main (first round of this task): all five `.` and no note at all.
    Reply aw = run(s, "xtreg y x1 x2 [aweight=w], fe");
    std::string notes;
    if (aw.est.is_object() && aw.est.contains("warnings") && aw.est["warnings"].is_array()) {
      for (const auto& w : aw.est["warnings"]) {
        if (w.is_string()) notes += w.get<std::string>() + "\n";
      }
    }
    std::string mismatch;
    bool anyMissing = false;
    for (const char* f : {"sigma_u", "rho", "corr", "r2_b", "r2_o"}) {
      const std::string e = std::string("e(") + f + ")";
      const bool missing = std::isnan(num(s, e));
      anyMissing = anyMissing || missing;
      if (missing != (notes.find(e) != std::string::npos)) mismatch += " " + e + (missing ? " `.` unnamed" : " named but set");
    }
    w2check(aw.rc == "0" && mismatch.empty() && (!anyMissing || notes.find("weighted") != std::string::npos),
            "w2_pweight X3_aw_fe PXT: every e(sigma_u)/e(rho)/e(corr)/e(r2_b)/e(r2_o) the engine leaves "
            "`.` is named in a visible note, never a silent `.`. Got rc " + aw.rc + mismatch +
                " notes `" + notes + "`");
  }

  // ── FU-29: the three refusals and the fweight count (:566-607) ───────────
  {
    piet::SessionState s(workdir);
    seed(s);
    Reply x10 = run(s, "xtreg y x1 x2 [pweight=wv], fe");
    w2check(x10.rc == "199" && x10.msg == "weight must be constant within id",
            "w2_pweight X10_pw_varies_within_id: r(199) \"weight must be constant within id\" "
            "(Stata checks the weight within each id). Main: r(198). The engine itself would fit "
            "it. Got rc " + x10.rc + " `" + x10.msg + "`");
    // aer_b4 predict-e-xbd.log G_w_varies: the same rule under aweight.
    Reply g = run(s, "xtreg y x1 [aw=wv], fe");
    w2check(g.rc == "199",
            "aer_b4 predict-e-xbd G_w_varies: `[aw=wv], fe` with wv varying within id is r(199). "
            "Got rc " + g.rc + " `" + g.msg + "`");
    Reply x11 = run(s, "xtreg y x1 x2 [iweight=w], fe");
    w2check(x11.rc == "101" && x11.msg == "iweights not allowed",
            "w2_pweight X11_iw_fe: r(101) \"iweights not allowed\" (xtreg, fe takes aw, fw and pw). "
            "Main: r(198). Got rc " + x11.rc + " `" + x11.msg + "`");
    Reply x12 = run(s, "xtreg y x1 x2 [pweight=w], re");
    w2check(x12.rc == "101" && x12.msg == "weights not allowed",
            "w2_pweight X12_pw_re: r(101) \"weights not allowed\" (xtreg_re.ado declares no "
            "weight). Main: r(198). Got rc " + x12.rc + " `" + x12.msg + "`");
    // w2_pweight S1_summarize_pw_refused (:764-768): summarize takes aw/fw/iw, so
    // a pweight is Stata's r(101) "pweights not allowed". Main: r(198) engine gap.
    Reply s1 = run(s, "summarize y [pweight=w]");
    w2check(s1.rc == "101" && s1.msg == "pweights not allowed",
            "w2_pweight S1_summarize_pw_refused: r(101) \"pweights not allowed\". Main: r(198). Got rc " +
                s1.rc + " `" + s1.msg + "`");
    Reply s1b = run(s, "sum y [pw=w]");
    w2check(s1b.rc == "101" && s1b.msg == "pweights not allowed",
            "w2_pweight S1 via the `sum` abbreviation: r(101) \"pweights not allowed\". Got rc " +
                s1b.rc + " `" + s1b.msg + "`");
    // S2_summarize_aw_control (:769-777): the legal weight still runs.
    Reply s2 = run(s, "summarize y [aweight=w]");
    w2check(s2.rc == "0", "w2_pweight S2_summarize_aw_control: rc 0. Got rc " + s2.rc + " `" + s2.msg + "`");
    // summarize takes iweights (help summarize) and is served now (QF-SUM-IW,
    // w2aj_qualifiers.log:826 qf_sw_iw_*); mean takes aweights (QF-MEAN-AW,
    // w2aj_qualifiers.log:1983 qf_mn_aw_sp).
    Reply s3 = run(s, "summarize y [iweight=w]");
    w2check(s3.rc == "0", "summarize [iw=]: rc 0 (w2aj_qualifiers.log:826). Got rc " + s3.rc +
                              " `" + s3.msg + "`");
    Reply s4 = run(s, "mean y [aweight=w]");
    w2check(s4.rc == "0", "mean [aw=]: rc 0 (w2aj_qualifiers.log:1983). Got rc " + s4.rc + " `" +
                              s4.msg + "`");
    // w2p_estim_ev A_qreg_aw_q50 (:418-419, __rc 101): qreg allows fw/iw/pw
    // (help qreg), so an aweight is Stata's r(101) "aweights not allowed".
    Reply q1 = run(s, "qreg y x1 x2 [aw=w], quantile(0.5)");
    w2check(q1.rc == "101" && q1.msg == "aweights not allowed",
            "w2p_estim_ev A_qreg_aw_q50: r(101) \"aweights not allowed\". Main: r(198). Got rc " +
                q1.rc + " `" + q1.msg + "`");
    // qreg [fw=]/[iw=] are Stata-legal and fitted natively now (runQregNative;
    // w2p_estim_ev A_qreg_fw_q50 / A_qreg_iw_q50 `|iid||fweight|= w|`).
    for (const char* wt : {"fweight", "iweight"}) {
      Reply q2 = run(s, std::string("qreg y x1 x2 [") + wt + "=w], quantile(0.5)");
      const std::string ty = run(s, "display \"`e(wtype)'|`e(wexp)'\"").text;
      w2check(q2.rc == "0" && ty == std::string(wt) + "|= w",
              std::string("qreg [") + wt + "=]: rc 0 with e(wtype)|e(wexp) `" + wt + "|= w`. Got rc " +
                  q2.rc + " `" + ty + "` " + q2.msg);
    }
    // qreg [pw=] stays allowed by the gate (control).
    Reply q3 = run(s, "qreg y x1 x2 [pweight=w], quantile(0.5)");
    w2check(q3.rc != "101" && q3.msg.find("not allowed") == std::string::npos &&
                q3.msg.find("only supports probability weights") == std::string::npos,
            "qreg [pw=]: passes the weight gate (control). Got rc " + q3.rc + " `" + q3.msg + "`");
    Reply x14 = run(s, "xtreg y x1 x2 [fweight=w], fe");
    const std::string n14 = run(s, "display e(N)").text;
    const double b14 = num(s, "_b[x1]");
    const double se14 = num(s, "_se[x1]");
    w2check(x14.rc == "0" && n14 == "360" && close(b14, 0x1.d0bd0e411b752p+0) &&
                close(se14, 0x1.ee829a04bb9cap-4),
            "w2_pweight X14_fw_fe: rc 0, e(N) 360 (sum of fweights), _b[x1] +1.d0bd0e411b752X+000, "
            "_se[x1] +1.ee829a04bb9caX-004. Main: r(198). Got rc " + x14.rc + " N " + n14 + " b " +
                fmt(b14) + " se " + fmt(se14) + " " + x14.msg);
    // xtreg, fe checks the weight within the estimation sample only: a weight that varies
    // within id ONLY on rows the `if` drops is legal. wif = w for t>2 and wv
    // otherwise; restricted to t>2 it is X7's weight, so X7's numbers follow.
    (void)run(s, "generate double wif = cond(t>2, w, wv)");
    Reply sub = run(s, "xtreg y x1 x2 [pweight=wif] if t>2, fe");
    const double bSub = num(s, "_b[x1]");
    w2check(sub.rc == "0" && close(bSub, 0x1.e8fcfd2c2c9fdp+0),
            "xtreg, fe checks the weight on the estimation sample only: a weight varying within id only outside the "
            "`if` sample fits (X7's _b[x1] +1.e8fcfd2c2c9fdX+000). Main: r(198). Got rc " + sub.rc +
                " b " + fmt(bSub) + " " + sub.msg);
  }

  // ── FU-76: X0/X1/X2 PXT and e(r2) after the UNweighted fits (:263-324) ───
  {
    piet::SessionState s(workdir);
    seed(s);
    Reply x0 = run(s, "xtreg y x1 x2, fe");
    const double r2 = num(s, "e(r2)");
    const double dfM = num(s, "e(df_m)");
    const double r2b = num(s, "e(r2_b)");
    const double r2o = num(s, "e(r2_o)");
    const double r2w = num(s, "e(r2_w)");
    w2check(x0.rc == "0" && close(r2, 0x1.132199730c722p-1) && dfM == 16 &&
                close(r2b, 0x1.f03574901785fp-1) && close(r2o, 0x1.5afec63f08acbp-1) &&
                close(r2w, 0x1.132199730c722p-1),
            "w2_pweight X0_fe PVAL/PXT: e(r2) = r2_w +1.132199730c722X-001, e(df_m) 16 (= df_a + "
            "2), e(r2_b) +1.f03574901785fX-001, e(r2_o) +1.5afec63f08acbX-001. Main: e(r2) "
            ".723526327 (LSDV), df_m 2, r2_b/r2_o `.`. Got r2 " + fmt(r2) + " df_m " + fmt(dfM) +
                " r2_b " + fmt(r2b) + " r2_o " + fmt(r2o) + " r2_w " + fmt(r2w));
    // The emitted block keeps its freeze-pinned shape (BUG-383's block r2).
    w2check(x0.est.is_object() && x0.est.contains("r2") && x0.est["r2"].is_number() &&
                close(x0.est["r2"].get<double>(), 0x1.132199730c722p-1),
            "w2_pweight X0_fe (control): the block's r2 is still the within R-squared");
    w2check(x0.est.is_object() && !x0.est.contains("warnings"),
            "w2_pweight X0_fe (control): an unweighted fit carries every PXT field and no "
            "`warnings` key (its freeze-pinned block is unchanged)");
    (void)run(s, "xtreg y x1 x2, fe vce(robust)");
    const double dfM1 = num(s, "e(df_m)");
    const double r2b1 = num(s, "e(r2_b)");
    (void)run(s, "xtreg y x1 x2, fe vce(cluster id)");
    const double dfM2 = num(s, "e(df_m)");
    w2check(dfM1 == 1 && dfM2 == 1 && close(r2b1, 0x1.f03574901785fp-1),
            "w2_pweight X1_fe_vcerobust / X2_fe_clusterid PXT: e(df_m) 1 under the cluster VCE, "
            "r2_b as X0. Main: 2. Got " + fmt(dfM1) + " / " + fmt(dfM2) + " r2_b " + fmt(r2b1));
  }

  // ── FU-102: e(N_g)/e(g_min)/e(g_max) after the FE panels (w2p_estim_ev D) ──
  {
    piet::SessionState s(workdir);
    for (const char* c :
         {"clear", "set obs 60", "generate double id = ceil(_n/6)", "generate double t  = mod(_n-1,6)+1",
          "generate double x1 = mod(_n*7,11) + mod(_n,5)/10",
          "generate double x2 = mod(_n*3,5) + mod(_n*13,7)/10",
          "generate double y  = 1 + 2*x1 - x2 + id/3 + mod(_n*11,17)/5",
          "generate byte db = (mod(_n*7,11) + mod(_n*5,7) > 8) * (id < 10)",
          "generate double cnt = (mod(_n*7,5) + (x1 > 6)) * (id < 10)", "xtset id t"})
      (void)run(s, c);
    const char* kRec = "display e(N) \"|\" e(N_g) \"|\" e(g_min) \"|\" e(g_max) \"|`e(cmd)'|`e(cmd2)'\"";
    Reply l = run(s, "xtlogit db x1 x2, fe");
    const std::string lr = run(s, kRec).text;
    w2check(l.rc == "0" && lr == "54|9|6|6|clogit|xtlogit",
            "w2p_estim_ev D_xtlogit_fe: `54|9|6|6|clogit` and e(cmd2) xtlogit. "
            "Main: `54|.|.|.|xtlogit`. Got rc " + l.rc + " `" + lr + "`");
    Reply p = run(s, "xtpoisson cnt x1 x2, fe");
    const std::string pr = run(s, kRec).text;
    w2check(p.rc == "0" && pr == "54|9|6|6|xtpoisson|",
            "w2p_estim_ev D_xtpoisson_fe: `54|9|6|6|xtpoisson`. Main: `54|.|.|.`. "
            "Got rc " + p.rc + " `" + pr + "`");
    Reply x = run(s, "xtreg y x1 x2, fe");
    const std::string xr = run(s, kRec).text;
    w2check(x.rc == "0" && xr == "60|10|6|6|xtreg|",
            "w2p_estim_ev D_xtreg_fe_ctl: `60|10|6|6|xtreg`. Main: g_min/g_max "
            "`.`. Got rc " + x.rc + " `" + xr + "`");
    w2check(x.est.is_object() && !x.est.contains("g_min") && !x.est.contains("fe_group_sizes"),
            "xtreg, fe: the group sizes reach e() only, the freeze-pinned block is unchanged");
  }

  // ── regress exact-fit rule (w2e_float_default prop_predict_*) ──────────────
  {
    piet::SessionState s(workdir);
    for (const char* c : {"clear", "set obs 50", "generate double x = mod(_n*7,11) + _n/10",
                          "generate double yx = 3 + 2*x", "generate double yn = 3 + 2*x + mod(_n*5,7)/100"})
      (void)run(s, c);
    Reply e = run(s, "regress yx x");
    const double se = num(s, "_se[x]");
    const double rss = num(s, "e(rss)");
    const double r2 = num(s, "e(r2)");
    w2check(e.rc == "0" && se == 0.0 && rss == 0.0 && r2 == 1.0,
            "regress exact fit: e(r2) > 1 - 1e-14 zeroes e(rss) and _se (Stata: _se[p] 0 in "
            "w2e_float_default prop_predict_double). Got se " + fmt(se) + " rss " + fmt(rss) + " r2 " + fmt(r2));
    // The coefficient table: t, p and the interval are missing, not t 0 / p 1 /
    // ll = ul = b (StataBE r(table) on this fit: se, t, pvalue, ll, ul all `.`).
    (void)run(s, "regress yx x");
    (void)run(s, "matrix __w2lfe_T = r(table)");
    const double tt = num(s, "__w2lfe_T[3,1]"), tp = num(s, "__w2lfe_T[4,1]");
    const double tl = num(s, "__w2lfe_T[5,1]"), tu = num(s, "__w2lfe_T[6,1]");
    const double tse = num(s, "__w2lfe_T[2,1]"), tdf = num(s, "__w2lfe_T[7,1]");
    w2check(std::isnan(tt) && std::isnan(tp) && std::isnan(tl) && std::isnan(tu) &&
                std::isnan(tse) && tdf == 48.0,
            "regress exact fit: r(table) se/t/p/ll/ul are missing and df stays 48 (StataBE). Got se " +
                fmt(tse) + " t " + fmt(tt) + " p " + fmt(tp) + " ll " + fmt(tl) + " ul " + fmt(tu) +
                " df " + fmt(tdf));
    const ojson& ct = e.est.contains("coeftable") ? e.est["coeftable"] : ojson();
    bool ctMissing = ct.is_array() && !ct.empty();
    if (ct.is_array())
      for (const auto& row : ct)
        for (const char* k : {"t", "p", "ci_lo", "ci_hi"})
          if (!row.contains(k) || !row[k].is_null()) ctMissing = false;
    w2check(ctMissing, "regress exact fit: the estimation block's coefficient table carries t, p and "
                       "the interval as missing. Got " + (ct.is_null() ? std::string("none") : ct.dump()));
    // `test x` after the exact fit: rc 0 with r(F) and r(p) missing (StataBE:
    // F(1, 48) = ., Prob > F = .). It used to stop with "R V R' not invertible".
    Reply t = run(s, "test x");
    const double tF = num(s, "r(F)"), tP = num(s, "r(p)");
    w2check(t.rc == "0" && std::isnan(tF) && std::isnan(tP),
            "regress exact fit: `test x` is rc 0 with r(F) and r(p) missing. Got rc " + t.rc + " " + t.msg +
                " F " + fmt(tF) + " p " + fmt(tP));
    Reply tpm = run(s, "testparm x");
    w2check(tpm.rc == "0" && std::isnan(num(s, "r(F)")) && std::isnan(num(s, "r(p)")) &&
                num(s, "r(df)") == 1.0 && num(s, "r(df_r)") == 48.0,
            "regress exact fit: `testparm x` posts r(F) and r(p) missing with r(df) 1, r(df_r) 48 "
            "(StataBE). Got rc " + tpm.rc + " " + tpm.msg);
    // `lincom x` after the exact fit: StataBE r(estimate) 2, r(se) 0 and r(t), r(p),
    // r(lb), r(ub) missing (it used to answer t 0, p 1, lb = ub = 2).
    Reply lc = run(s, "lincom x");
    const double lse = num(s, "r(se)"), lt = num(s, "r(t)"), lp = num(s, "r(p)");
    const double llb = num(s, "r(lb)"), lub = num(s, "r(ub)");
    w2check(lc.rc == "0" && lse == 0.0 && std::isnan(lt) && std::isnan(lp) && std::isnan(llb) &&
                std::isnan(lub),
            "regress exact fit: `lincom x` posts r(se) 0 with r(t)/r(p)/r(lb)/r(ub) missing (StataBE). Got rc " +
                lc.rc + " se " + fmt(lse) + " t " + fmt(lt) + " p " + fmt(lp) + " lb " + fmt(llb) + " ub " +
                fmt(lub));
    // reghdfe has no such rule: on the same data StataBE keeps e(rss) 8.526e-28 and
    // _se[x] 1.72e-16 (t 1.2e16, p 0). The rule is regress's alone.
    Reply h = run(s, "reghdfe yx x, noabsorb");
    const double hrss = num(s, "e(rss)"), hse = num(s, "_se[x]");
    w2check(h.rc == "0" && hrss > 0.0 && hse > 0.0,
            "reghdfe noabsorb (control): an exact fit keeps its computed e(rss) and _se (StataBE "
            "8.526e-28 and 1.72e-16). Got rc " + h.rc + " rss " + fmt(hrss) + " se " + fmt(hse));
    (void)run(s, "regress yn x");
    const double sen = num(s, "_se[x]");
    w2check(sen > 0.0 && num(s, "e(rss)") > 0.0, "regress (control): a noisy fit keeps its RSS and se");
    Reply tn = run(s, "test x");
    w2check(tn.rc == "0" && num(s, "r(F)") > 0.0 && num(s, "r(p)") >= 0.0,
            "regress (control): `test x` after a noisy fit posts a finite r(F)");
  }

  fs::remove_all(workdir, ec);
}
