// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// W2B lane `glm` (X2-GLM) -- `glm` fitted in-process, and `margins` after it.
// Oracle: tests/eval/golden/aer_w2/w2_glm_lifecycle.{do,log}. Every number
// asserted below is that log's own %21x record (B|, V|, SCALAR|, MARGINS|)
// rewritten mechanically as a C++ hex literal, on the do-file's own 96-row
// synthetic stanza. Nothing is read through a 9-digit `display`: coefficients
// and variances come from the POSTED e(b)/e(V), scalars through `scalar s =
// e(name)`, which substitutes at full precision (BUG-382).
//
// Every arm FAILS on the pre-X2 main: `glm` was refused there
// (refused:e3_deferred, rc 199), so no fit, no e(), and no margins existed.
#include "selftest_w2.h"

#include <cmath>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>
#include <vector>

#include "piet/dispatcher.h"
#include "piet/eval/errors.h"
#include "piet/handlers.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

namespace {

using ojson = nlohmann::ordered_json;

struct Reply {
  std::string rc = "0";
  std::string reason;
  std::string msg;
  std::string text;  // every display block's text
  ojson est;         // the last estimation block
};

Reply run(piet::SessionState& session, const std::string& cmd) {
  Reply r;
  try {
    for (const auto& b : piet::dispatch(cmd, session)) {
      if (!b.is_object()) continue;
      const std::string t = b.value("type", std::string());
      if (t == "error") {
        r.rc = piet::eval::coarseRc(b);
        r.reason = b.value("reason", std::string());
        r.msg += b.value("message", std::string());
      } else if (t == "estimation") {
        r.est = b;
      } else if (t == "display") {
        r.text += b.value("text", std::string()) + "\n";
      }
    }
  } catch (const std::exception& e) {
    r.rc = "EXC";
    r.msg = e.what();
  }
  return r;
}

std::string fmt(double v) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.17g", v);
  return buf;
}

bool close(double a, double b, double rtol, double atol = 0.0) {
  return std::isfinite(a) && std::isfinite(b) && std::fabs(a - b) <= rtol * std::fabs(b) + atol;
}

// e(name) through the scalar resolver, at full precision.
double eScalar(piet::SessionState& s, const std::string& name) {
  (void)piet::dispatch("scalar w2bglm_s = e(" + name + ")", s);
  const std::string v = s.eval.scalars.get("w2bglm_s", "");
  try {
    return v.empty() || v == "." ? std::nan("") : std::stod(v);
  } catch (...) {
    return std::nan("");
  }
}

std::string eString(piet::SessionState& s, const std::string& name) {
  const std::string* v = s.eval.eStrings.find(name);
  return v == nullptr ? std::string() : *v;
}

// The posted e(b) and e(V) (lower triangle, row by row) against the oracle.
// T2 (1e-9) with a floor of 1e-9 x the largest |entry|: the base-level zeros
// and the near-zero covariances have no relative scale of their own.
bool matchesBV(piet::SessionState& s, const std::vector<double>& b, const std::vector<double>& vlow,
               std::string& why) {
  const piet::eval::EvalMatrix* eb = s.eval.findClassMatrix('e', "b");
  const piet::eval::EvalMatrix* ev = s.eval.findClassMatrix('e', "V");
  if (eb == nullptr || ev == nullptr || eb->data.size() != 1 || eb->data[0].size() != b.size() ||
      ev->data.size() != b.size()) {
    why = "e(b)/e(V) not posted with " + std::to_string(b.size()) + " columns";
    return false;
  }
  double bs = 0, vs = 0;
  for (double x : b) bs = std::max(bs, std::fabs(x));
  for (double x : vlow) vs = std::max(vs, std::fabs(x));
  for (size_t j = 0; j < b.size(); ++j) {
    if (!close(eb->data[0][j], b[j], 1e-9, 1e-9 * bs)) {
      why = "b[" + std::to_string(j + 1) + "] got " + fmt(eb->data[0][j]) + " want " + fmt(b[j]);
      return false;
    }
  }
  size_t q = 0;
  for (size_t i = 0; i < b.size(); ++i)
    for (size_t j = 0; j <= i; ++j, ++q) {
      if (ev->data[i].size() != b.size() || !close(ev->data[i][j], vlow[q], 1e-9, 1e-9 * vs)) {
        why = "V[" + std::to_string(i + 1) + "," + std::to_string(j + 1) + "] got " +
              (ev->data[i].size() > j ? fmt(ev->data[i][j]) : std::string("?")) + " want " + fmt(vlow[q]);
        return false;
      }
    }
  return true;
}

// w2_glm_lifecycle.do's data stanza, line for line.
void seed(piet::SessionState& s) {
  static const char* kFixture[] = {
      "clear",
      "set obs 96",
      "generate cl = mod(_n-1,8)+1",
      "generate g = mod(_n-1,4)+1",
      "generate double x1 = 12 + mod(_n*7,29)",
      "generate double x2 = 1800 + 25*mod(_n*13,97)",
      "generate double w = 1 + mod(_n,5)/3",
      "generate double y = (40 + mod(_n*37,101))*(1 + x1/25)*(1 + x2/1500)/10",
      "generate double cnt = floor(y/3)",
      "generate byte bin = (x1 + mod(_n*19,37)) > 28",
  };
  for (const char* c : kFixture) (void)piet::dispatch(c, s);
}

}  // namespace

void testW2bGlm() {
  std::printf("[selftest] --- W2B glm: default ML lifecycle, stale-phi e(V), fam(nb), [aw] cluster, i.(), margins ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_w2bglm").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  std::string why;

  // ── gam_def: gamma/log, default tolerances ─────────────────────────────────
  {
    piet::SessionState s(workdir);
    seed(s);
    Reply r = run(s, "glm y x1 x2, family(gamma) link(log)");
    w2check(r.rc == "0" && r.est.is_object(),
            "w2_glm_lifecycle PREC|gam_def|0: `glm y x1 x2, family(gamma) link(log)` fits (main: "
            "refused:e3_deferred, rc 199). Got rc " + r.rc + " " + r.msg);
    w2check(matchesBV(s, {0x1.287270399c3b8p-6, 0x1.df962e2a0f875p-14, 0x1.95b36ffd48cbep+1},
                      {0x1.01b308ddac1bdp-16, 0x1.570c9b7271090p-27, 0x1.3317ae9a2b143p-29,
                       -0x1.c4d0e700a7675p-12, -0x1.d53df800d1de9p-18, 0x1.11d78e4512636p-5},
                      why),
            "w2_glm_lifecycle B|gam_def / V|gam_def: e(b) and the FULL e(V) (off-diagonals included: "
            "the V uses the STALE phi of iteration 1, STALEPHI|prev). " + why);
    w2check(close(eScalar(s, "ll"), -0x1.dec6dbd62c5e2p+8, 1e-12) &&
                close(eScalar(s, "phi"), 0x1.a391d262efe5ep-4, 1e-9) &&
                close(eScalar(s, "dispers_p"), 0x1.a391d262efe5cp-4, 1e-9) &&
                close(eScalar(s, "deviance"), 0x1.4c86e9211f399p+3, 1e-9) &&
                eScalar(s, "ic") == 2 && eScalar(s, "converged") == 1 && eScalar(s, "rank") == 3 &&
                eScalar(s, "k") == 3 && eScalar(s, "df") == 93 && eScalar(s, "N") == 96,
            "w2_glm_lifecycle SCALAR|gam_def: e(ll) e(phi) e(dispers_p) e(deviance) e(ic)=2 "
            "e(converged)=1 e(rank)=e(k)=3 e(df)=93 e(N)=96. Got ll=" + fmt(eScalar(s, "ll")) +
                " phi=" + fmt(eScalar(s, "phi")) + " ic=" + fmt(eScalar(s, "ic")));
    w2check(eString(s, "cmd") == "glm" && eString(s, "varfunct") == "Gamma" &&
                eString(s, "linkt") == "Log" && eString(s, "vce") == "oim" &&
                eString(s, "vcetype") == "OIM" && eString(s, "opt") == "moptimize" &&
                eString(s, "technique") == "nr" && eString(s, "a") == "1",
            "w2_glm_lifecycle STRING|gam_def: Gamma|Log|oim|OIM|moptimize|nr|a=1, e(cmd) glm. Got " +
                eString(s, "varfunct") + "|" + eString(s, "linkt") + "|" + eString(s, "vce") + "|" +
                eString(s, "vcetype"));
    w2check(r.text.find("Iteration 2:  Log likelihood = -478.77679") != std::string::npos &&
                r.text.find("Scale parameter =    .102434") != std::string::npos,
            "w2_glm_lifecycle gam_def log text: the iteration log and the header print Stata's "
            "lines. Got: " + r.text.substr(0, 400));
    // test/lincom/predict must not refit an OLS behind the glm.
    Reply lc = run(s, "lincom x1");
    w2check(lc.rc != "0" && lc.msg.find("not supported after `glm`") != std::string::npos,
            "post-estimation after glm is an honest refusal naming glm, never an OLS refit "
            "(no oracle arm: the lincom engine has no glm model). Got rc " + lc.rc + " " + lc.msg);
    // glm POSTS e(V) and its macros; a later model must not read them. Measured
    // on the first build of this lane: `glm` then `regress` then `matrix V =
    // e(V)` returned the glm's V (V[2,1] 9.984e-09) and e(varfunct) was still Gamma.
    Reply rg = run(s, "regress y x1 x2");
    const piet::eval::EvalMatrix* staleV = s.eval.findClassMatrix('e', "V");
    w2check(r.rc == "0" && rg.rc == "0" && eString(s, "cmd") == "regress" &&
                (staleV == nullptr || staleV->data.size() != 3 ||
                 !close(staleV->data[1][0], 0x1.570c9b7271090p-27, 1e-6)) &&
                eString(s, "varfunct").empty() && eString(s, "linkt").empty() &&
                s.eval.findClassMatrix('e', "ilog") == nullptr,
            "after `glm` then `regress`, e(V)/e(ilog)/e(varfunct)/e(linkt) are no longer the glm's "
            "(the glm-posted entries are dropped once the model in memory is not a glm). Got "
            "e(cmd) " + eString(s, "cmd") + " varfunct `" + eString(s, "varfunct") + "'");
  }

  // ── gam_it0: iterate(0) is the WLS start; e(V) uses the SEEDED phi ────────
  {
    piet::SessionState s(workdir);
    seed(s);
    Reply r = run(s, "glm y x1 x2, family(gamma) link(log) iterate(0)");
    w2check(r.rc == "0" &&
                matchesBV(s, {0x1.1b65205a9abcep-6, 0x1.a738ebe6b0adep-14, 0x1.991d70eef5027p+1},
                          {0x1.0aa2bb1c9aca7p-16, 0x1.6408dd2adce10p-27, 0x1.3e093b2f44c73p-29,
                           -0x1.d58ad20a3cae0p-12, -0x1.e7001b6d85e96p-18, 0x1.1ca6b5fb084f7p-5},
                          why),
            "w2_glm_lifecycle B|gam_it0 / V|gam_it0: b_0 is Stata's one-step WLS start and e(V) "
            "scales by sum (y-mu(b_0))^2/V(mu0)/(N-k) -- the START variance (1.5e-2 off without "
            "it). Got rc " + r.rc + " " + why + " " + r.msg);
    w2check(eScalar(s, "ic") == 0 && eScalar(s, "converged") == 0 &&
                close(eScalar(s, "phi"), 0x1.c8f7646870294p-4, 1e-9) &&
                r.text.find("convergence not achieved") != std::string::npos &&
                r.text.find("Warning: Convergence not achieved.") != std::string::npos,
            "w2_glm_lifecycle SCALAR|gam_it0: e(ic)=0 e(converged)=0 at rc 0 with Stata's two "
            "convergence notes. Got ic=" + fmt(eScalar(s, "ic")) + " conv=" +
                fmt(eScalar(s, "converged")));
  }

  // ── bin_logit_it1: the first NR step is EXTENDED to 1.375 (forward steps) ──
  {
    piet::SessionState s(workdir);
    seed(s);
    Reply r = run(s, "glm bin x1 x2, family(binomial) link(logit) iterate(1) nolog");
    w2check(r.rc == "0" &&
                matchesBV(s, {0x1.64ed13d8f2538p-4, -0x1.58bcf274332c7p-13, 0x1.667cf83d1a49ep-2},
                          {0x1.d030ed0b8f5ccp-10, 0x1.60d5d1fd2c8c0p-23, 0x1.c2cacc6f05985p-23,
                           -0x1.424aa7da9aee9p-5, -0x1.54348a0897e03p-11, 0x1.7a911e2eba651p+1},
                          why) &&
                close(eScalar(s, "ll"), -0x1.0f52205f8885cp+5, 1e-12),
            "w2_glm_lifecycle B|bin_logit_it1: after ONE iteration b is b_0 + 1.375 x the Newton "
            "step (ml's forward steps 1/8, 1/4 kept, 1/2 ignored). Got rc " + r.rc + " " + why);
    w2check(r.text.find("Iteration") == std::string::npos,
            "w2_glm_lifecycle bin_logit_it1: `nolog` prints no iteration lines");
  }

  // ── bin_probit_def: converged probit, e(ic)=3 ─────────────────────────────
  {
    piet::SessionState s(workdir);
    seed(s);
    Reply r = run(s, "glm bin x1 x2, family(binomial) link(probit) showstep");
    w2check(r.rc == "0" &&
                matchesBV(s, {0x1.962ecdb4bb3c3p-5, -0x1.3d5c17fca42d1p-14, 0x1.794012c095a51p-3},
                          {0x1.2447d1935799ep-11, 0x1.0b768074fa630p-23, 0x1.0aac5c5907c68p-24,
                           -0x1.acebc24c58e42p-7, -0x1.92e21441f796dp-13, 0x1.d39c1bbd3dd90p-1},
                          why) &&
                eScalar(s, "ic") == 3 && eString(s, "varfunct") == "Bernoulli" &&
                eString(s, "linkt") == "Probit",
            "w2_glm_lifecycle bin_probit_def: Bernoulli|Probit, e(ic)=3, b and V. Got rc " + r.rc +
                " " + why);
  }

  // ── nb_default: fam(nb) = alpha fixed at 1, vce(cl cl) ─────────────────────
  {
    piet::SessionState s(workdir);
    seed(s);
    Reply r = run(s, "glm cnt x1 x2, link(log) fam(nb) vce(cl cl)");
    const bool bv = matchesBV(s, {0x1.33aace78a7ef2p-6, 0x1.f8452d654c17ep-14, 0x1.00ce486e580d5p+1},
                              {0x1.1d50422c7a2d4p-15, 0x1.6fae22fa85997p-23, 0x1.4dd6447f1b7cap-26,
                               -0x1.6a2ae426bc5b7p-10, -0x1.08a709680ff42p-14, 0x1.cf8d8c4300ca6p-3},
                              why);
    w2check(r.rc == "0" && bv && eScalar(s, "N_clust") == 8 && eScalar(s, "phi") == 1 &&
                close(eScalar(s, "ll"), -0x1.7543a5648be93p+8, 1e-12),
            "w2_glm_lifecycle nb_default (069's corpus form, 10 of 16 statements): the fixed-alpha "
            "NB2 with the cluster sandwich, e(N_clust)=8, e(phi)=1. Got rc " + r.rc + " " + why +
                " " + r.msg);
    w2check(eString(s, "varfunct") == "Neg. Binomial" && eString(s, "vce") == "cluster" &&
                eString(s, "vcetype") == "Robust" && eString(s, "a") == "1" &&
                r.text.find("Log pseudolikelihood = -373.2642424") != std::string::npos,
            "w2_glm_lifecycle STRING|nb_default: Neg. Binomial|Log|cluster|Robust|...|a=1 and the "
            "pseudolikelihood header. Got " + eString(s, "varfunct") + "|" + eString(s, "vce"));
    Reply c = run(s, "glm cnt x1 x2, link(log) family(nbinomial 1) vce(cluster cl)");
    const piet::eval::EvalMatrix* eb = s.eval.findClassMatrix('e', "b");
    w2check(c.rc == "0" && eb != nullptr && eb->data.size() == 1 && eb->data[0].size() == 3 &&
                close(eb->data[0][0], 0x1.33aace78a7ef2p-6, 1e-9) && eString(s, "a") == "1",
            "w2_glm_lifecycle nb_alpha1_control: `family(nbinomial 1)` is the same fit as `fam(nb)`");
  }

  // ── pois_aw_cl_def: [aw=w] + cluster + i.g, base level posted as a zero ────
  {
    piet::SessionState s(workdir);
    seed(s);
    Reply r = run(s, "glm cnt x1 x2 i.g [aw=w], family(poisson) link(log) vce(cluster cl)");
    const bool bv = matchesBV(
        s,
        {0x1.1eec749b40809p-6, 0x1.d6b65e1ac8bd7p-14, 0.0, -0x1.2ff67a655a812p-4,
         0x1.33041a85a9811p-6, -0x1.f0b666b760820p-8, 0x1.0a59543c429b9p+1},
        {0x1.a8bb6ce7b6958p-16, 0x1.a9d03fd49b072p-24, 0x1.4d2381a0ac03bp-26, 0.0, 0.0, 0.0,
         -0x1.9da9bd1619fa5p-15, -0x1.0802489c93247p-19, 0.0, 0x1.8102005c816bfp-8,
         -0x1.30c2f0e41bea6p-13, 0x1.2a78954fc28d8p-20, 0.0, 0x1.25899276c2e93p-9,
         0x1.75d1f7682e3e0p-8, -0x1.c8c7bf2034ce0p-16, 0x1.a289f866df9bap-20, 0.0,
         0x1.6a2aa46cc12ffp-9, 0x1.9cf77ba948ee1p-9, 0x1.701b84c812697p-8,
         -0x1.bfedcbc72b04ep-11, -0x1.063065ee741cfp-14, 0.0, 0x1.3c75823b96d75p-8,
         -0x1.99b6cd31f8d90p-10, -0x1.d2ddc873278dcp-8, 0x1.b7ef2c47982dap-3},
        why);
    w2check(r.rc == "0" && bv,
            "w2_glm_lifecycle pois_aw_cl_def (027's shape): analytic weights normalized to mean 1, "
            "the cluster sandwich, and e(b)'s 1b.g column posted as 0 (main refused [aw=] on glm "
            "outright). Got rc " + r.rc + " " + why + " " + r.msg);
    w2check(eScalar(s, "k") == 7 && eScalar(s, "rank") == 6 && eScalar(s, "df_m") == 5 &&
                eScalar(s, "df") == 90 && close(eScalar(s, "ll"), -0x1.3bff4b91c5678p+8, 1e-12) &&
                close(eScalar(s, "dispers_p"), 0x1.fd0a5adc6647dp+0, 1e-9) &&
                r.text.find("(sum of wgt is 159.6666666666667)") != std::string::npos,
            "w2_glm_lifecycle SCALAR|pois_aw_cl_def: e(k)=7 (base included) e(rank)=6 e(df_m)=5 "
            "e(df)=90, e(ll), e(dispers_p), and the `(sum of wgt is ...)` note. Got k=" +
                fmt(eScalar(s, "k")) + " rank=" + fmt(eScalar(s, "rank")));
  }

  // ── gam_cl_factor + gam_cl_margins: i.() first, then margins, dydx(g) post ─
  {
    piet::SessionState s(workdir);
    seed(s);
    Reply r = run(s, "glm y i.(g) x1 x2, link(log) fam(gamma) vce(cl cl)");
    const bool bv = matchesBV(
        s,
        {0.0, -0x1.41ae958b8f0b4p-10, 0x1.0d182cdb7cd64p-6, 0x1.1603fdeec6fc8p-5,
         0x1.259a2da4d4c61p-6, 0x1.e7a2e1da8b1f2p-14, 0x1.93f4fe13fa817p+1},
        {0.0, 0.0, 0x1.2237d920eba66p-8, 0.0, 0x1.c703e201b4f30p-11, 0x1.17c13db765518p-8, 0.0,
         0x1.a79f2ce8248fbp-9, 0x1.83a9c15cf7002p-10, 0x1.1272698cd1dcap-8, 0.0,
         -0x1.958983f867100p-21, -0x1.b605bd0ecafb8p-14, -0x1.74288495d07dep-15,
         0x1.f20d1074a8138p-16, 0.0, 0x1.bd2cdb0398eb6p-18, 0x1.c47363fe2a795p-20,
         0x1.eef6819f38424p-18, 0x1.0595ca4f9bc86p-23, 0x1.536b248a87c77p-26, 0.0,
         -0x1.64c371ec03baep-6, -0x1.8c58346afc4f2p-9, -0x1.7e8e776c7e445p-6,
         -0x1.1ae71c72aeb75p-10, -0x1.16e531cf6b2aap-14, 0x1.eaba958b43b12p-3},
        why);
    w2check(r.rc == "0" && bv && close(eScalar(s, "phi"), 0x1.b22f505c20ce7p-4, 1e-9),
            "w2_glm_lifecycle gam_cl_factor (069's shape): `i.(g)` expands to 1b.g 2.g 3.g 4.g "
            "ahead of x1 x2, clustered gamma/log. Got rc " + r.rc + " " + why + " " + r.msg);
    Reply m = run(s, "margins, dydx(g) post");
    const piet::eval::EvalMatrix* mb = s.eval.findClassMatrix('e', "b");
    const piet::eval::EvalMatrix* mv = s.eval.findClassMatrix('e', "V");
    bool ok = m.rc == "0" && mb != nullptr && mv != nullptr && mb->data.size() == 1 &&
              mb->data[0].size() == 4 && mv->data.size() == 4;
    const double wantB[4] = {0.0, -0x1.0f4508cddc800p-4, 0x1.c9e005fc1acc0p-1, 0x1.dd3b9ebee6e20p+0};
    const double wantV[4] = {0.0, 0x1.9c97e9773405dp+3, 0x1.9973e9076749fp+3, 0x1.955e430a9c0cdp+3};
    std::string got;
    for (int j = 0; ok && j < 4; ++j) {
      got += fmt(mb->data[0][j]) + "/" + fmt(mv->data[j][j]) + " ";
      ok = close(mb->data[0][j], wantB[j], 1e-9, 1e-12) && close(mv->data[j][j], wantV[j], 1e-9, 1e-12);
    }
    w2check(ok && eString(s, "cmd") == "margins",
            "w2_glm_lifecycle MARGINS|gam_cl_margins: discrete-change AMEs of g on the predicted "
            "mean with the delta-method V through the CLUSTERED e(V); `post` makes e(b) 1x4 with "
            "1b.g = 0. (Main: no glm; after any other model margins answered that model.) Got rc " +
                m.rc + " " + got + m.msg);
  }

  // ── honest refusals: families/links/vce the oracle does not cover ──────────
  {
    piet::SessionState s(workdir);
    seed(s);
    struct Case {
      const char* cmd;
      const char* reason;
    };
    const Case cases[] = {
        // gamma's DEFAULT link is the reciprocal ([R] glm), not covered.
        {"glm y x1 x2, family(gamma)", "refused:glm_link_reciprocal"},
        {"glm y x1 x2, family(igaussian) link(log)", "refused:glm_family_igaussian"},
        {"glm cnt x1 x2, family(nbinomial ml) link(log)", "refused:glm_family_nbinomial_ml"},
        {"glm y x1 x2, family(gamma) link(log) vce(robust)", "refused:glm_vce_robust"},
        {"glm bin x1 x2, family(binomial) link(cloglog)", "refused:glm_link_cloglog"},
    };
    for (const auto& c : cases) {
      Reply r = run(s, c.cmd);
      w2check(r.rc == "199" && r.reason == c.reason,
              std::string("glm refuses by NAME what no oracle arm covers: `") + c.cmd + "` -> " +
                  c.reason + " (main: refused:e3_deferred for every glm). Got rc " + r.rc + " " +
                  r.reason);
    }
    // The family()/link() grammar, measured on StataBE (glmgram_probe*.log).
    struct GrammarCase {
      const char* cmd;
      const char* rc;
      const char* reason;  // "" = not asserted
      const char* varfunct;
      const char* linkt;
    };
    const GrammarCase grammar[] = {
        {"glm y x1 x2, family(n)", "0", "", "Gaussian", "Identity"},
        {"glm y x1 x2, family(gau)", "0", "", "Gaussian", "Identity"},
        {"glm y x1 x2, family(gaussian 1)", "0", "", "Gaussian", "Identity"},
        {"glm y x1 x2, family(gamma 2) link(log)", "0", "", "Gamma", "Log"},
        {"glm cnt x1 x2, family(  POISSON  ) link(log 2)", "0", "", "Poisson", "Log"},
        {"glm cnt x1 x2, family(poisson) link(power 0)", "0", "", "Poisson", "Log"},
        {"glm bin x1 x2, family(b) link(opower 0)", "0", "", "Bernoulli", "Logit"},
        {"glm bin x1 x2, family(binomial 1) link(l)", "0", "", "Bernoulli", "Logit"},
        {"glm y x1 x2, family(g)", "198", "", "", ""},
        {"glm y x1 x2, family(foo)", "198", "", "", ""},
        {"glm bin x1 x2, family(binomial 0)", "198", "", "", ""},
        {"glm bin x1 x2, family(binomial 1 2)", "198", "", "", ""},
        {"glm cnt x1 x2, family(nbinomial abc)", "7", "", "", ""},
        {"glm cnt x1 x2, family(nbinomial ML)", "7", "", "", ""},
        {"glm cnt x1 x2, family(nbinomial 0) link(foo)", "198", "", "", ""},
        {"glm y x1 x2, family(igaussian) link(power abc)", "198", "", "", ""},
        {"glm y x1 x2, family(foo) link(pow)", "198", "", "", ""},
        {"glm y x1 x2, family(gaussian) link(power 1 2)", "198", "", "", ""},
        {"glm y x1 x2, family(foo) link(log)", "199", "refused:glm_family_unrecognized", "", ""},
        {"glm y x1 x2, family(gamma) link(foo)", "199", "refused:glm_link_unrecognized", "", ""},
        {"glm y x1 x2, family(gamma) link(power -1)", "199", "refused:glm_link_reciprocal", "", ""},
        {"glm y x1 x2, family(gamma) link(power 1)", "199", "refused:glm_link_power", "", ""},
        {"glm bin x1 x2, family(b) link(c)", "199", "refused:glm_link_cloglog", "", ""},
        {"glm bin x1 x2, family(binomial 3)", "199", "refused:glm_family_binomial_denominator", "", ""},
        {"glm cnt x1 x2, family(poisson) link(identity)", "199",
         "refused:glm_family_link_poisson_identity", "", ""},
        {"glm y x1 x2, link(log)", "199", "refused:glm_family_link_gaussian_log", "", ""},
    };
    for (const auto& g : grammar) {
      Reply r = run(s, g.cmd);
      bool ok = r.rc == g.rc && (*g.reason == '\0' || r.reason == g.reason);
      if (*g.varfunct != '\0')
        ok = ok && s.eval.eStrings.get("varfunct", "") == g.varfunct &&
             s.eval.eStrings.get("linkt", "") == g.linkt;
      w2check(ok, std::string("glm grammar: `") + g.cmd + "` is rc " + g.rc + " " + g.reason + " " +
                      g.varfunct + "/" + g.linkt + ". Got rc " + r.rc + " " + r.reason + " " + r.msg);
    }
    // e(a) keeps the typed text; the variance header rounds it (Stata's u+(.5)u^2).
    Reply na = run(s, "glm cnt x1 x2, family(nbinomial 0.50)");
    w2check(na.rc == "0" && s.eval.eStrings.get("a", "") == "0.50" &&
                na.text.find("u+(.5)u^2") != std::string::npos,
            "glm: family(nbinomial 0.50) keeps e(a) 0.50 and prints u+(.5)u^2. Got rc " + na.rc +
                " a=" + s.eval.eStrings.get("a", "") + "\n" + na.text);

    // An exactly collinear regressor: Stata omits it (o.x3); this build refuses
    // by name rather than returning the finite garbage a roundoff pivot gives.
    (void)run(s, "generate double x3 = 2*x1");
    Reply cl = run(s, "glm y x1 x2 x3, family(gamma) link(log)");
    w2check(cl.rc == "199" && cl.reason == "refused:glm_collinear_regressors",
            "glm with an exactly collinear regressor refuses by name (refused:glm_collinear_regressors), "
            "never a finite fit from a roundoff-level pivot. Got rc " + cl.rc + " " + cl.reason);
    Reply o = run(s, "glm y x1 x2, family(gamma) link(log) eform");
    w2check(o.rc == "198" && o.msg.find("eform") != std::string::npos,
            "glm option vocabulary: a real glm option this build does not implement (`eform`) is "
            "an r(198) option refusal naming it, not a silent no-op. Got rc " + o.rc + " " + o.msg);
  }

  // ── vce(cluster) with more parameters than clusters: rank/df from e(V) ─────
  // Oracle: tests/eval/golden/aer_estim2_glm_oracle.log arms syn_B, syn_C and
  // the control syn_C_oim, on that do-file's 240-row synthetic stanza (paper
  // 027's shape: i.id FE nested in 8 clusters). Stata posts e(rank) =
  // rank(e(V)), so the clustered V of 42 columns / 8
  // clusters gives rank 1, e(df) 239, e(df_m) 0, and the (1/df) dispersions,
  // AIC and BIC follow; e(phi) stays the N-k Pearson scale. Before this the
  // build reported rank 41 / df 199 for every fit (a silent wrong answer).
  {
    piet::SessionState s(workdir);
    static const char* kSyn[] = {
        "clear",
        "set obs 240",
        "generate id = mod(_n-1,40)+1",
        "generate cl = mod(id-1,8)+1",
        "generate double x = sin(_n*1.3)",
        "generate double w = 1 + mod(_n,7)/4",
        "generate double y = exp(.5+.3*x+id/100)*(0.2+mod(_n*37,101)/35)",
        "generate count = floor(y*5)",
    };
    for (const char* c : kSyn) (void)piet::dispatch(c, s);
    struct Arm {
      const char* name;
      const char* cmd;
      double rank, df, dfm, phi, dispers, dispersP, dispersS, aic, bic;
      const char* dfLine;
    };
    const Arm arms[] = {
        {"syn_B", "glm count c.x i.id [aw=w], family(poisson) link(log) vce(cluster cl) nolog", 1, 239, 0,
         1.0, 0x1.226fa42b5f92fp+2, 0x1.09564f8b9af35p+2, 0x1.226fa42b5f92fp+2, 0x1.1e9f13cfaf54fp+3,
         -0x1.c28d00bebec98p+7, "Residual df     =        239"},
        {"syn_C", "glm y c.x i.id, family(gamma) link(log) vce(cluster cl) nolog", 1, 239, 0,
         0x1.39356370deeb0p-2, 0x1.61e32dfb71b06p-2, 0x1.04c9e8c19867ap-2, 0x1.213fadc147c52p+0,
         0x1.18a9216fd1881p+2, -0x1.32d1a8dd7a212p+10, "Residual df     =        239"},
        {"syn_C_oim (control)", "glm y c.x i.id, family(gamma) link(log) nolog", 41, 199, 40,
         0x1.39356370deeb0p-2, 0x1.a9054416d470dp-2, 0x1.39356370deeb1p-2, 0x1.5b63a25e7b80cp+0,
         0x1.2dfe76c526dd6p+2, -0x1.f80672ae417d1p+9, "Residual df     =        199"},
    };
    for (const auto& a : arms) {
      Reply r = run(s, a.cmd);
      const double rank = eScalar(s, "rank"), df = eScalar(s, "df"), dfm = eScalar(s, "df_m");
      const bool ok = r.rc == "0" && rank == a.rank && df == a.df && dfm == a.dfm &&
                      close(eScalar(s, "phi"), a.phi, 1e-9) &&
                      close(eScalar(s, "dispers"), a.dispers, 1e-9) &&
                      close(eScalar(s, "dispers_p"), a.dispersP, 1e-9) &&
                      close(eScalar(s, "dispers_s"), a.dispersS, 1e-9) &&
                      close(eScalar(s, "aic"), a.aic, 1e-9) && close(eScalar(s, "bic"), a.bic, 1e-9) &&
                      r.text.find(a.dfLine) != std::string::npos;
      w2check(ok, std::string("aer_estim2_glm ") + a.name + ": e(rank) " + fmt(a.rank) + ", e(df) " +
                      fmt(a.df) + ", e(df_m) " + fmt(a.dfm) +
                      ", dispersions/AIC/BIC on that df at T2, header `" + a.dfLine + "`. Got rc " +
                      r.rc + " rank " + fmt(rank) + " df " + fmt(df) + " df_m " + fmt(dfm) + " dispers_p " +
                      fmt(eScalar(s, "dispers_p")) + " aic " + fmt(eScalar(s, "aic")) + " bic " +
                      fmt(eScalar(s, "bic")) + " " + r.msg);
    }
  }

  // ── e(rank)'s invsym drop rule and sweep order, on the helper itself ───────
  // StataBE capture x2-glm-native/rank/st/cal{,2,3}.log (`display
  // rowsof(Ai)-diag0cnt(Ai)` after `matrix Ai = invsym(A)`). COLR|1e-8 pins the
  // ORDER: largest-relative-pivot first ties at 1.0, sweeps the 1e-12 column
  // first and keeps the other at relative 1e-8 (rank 2); Stata posts 1. The
  // other rows pin the 1e-9 relative cut and the 1e-19 absolute floor.
  {
    struct Cal {
      const char* row;
      std::vector<double> V;
      int64_t rank;
    };
    const Cal cals[] = {
        {"COLR|1e-8", {1e-12 * (1 + 1e-8), 1e-6, 1e-6, 1}, 1},
        {"COL|1e-8", {1, 1e-6, 1e-6, 1e-12 * (1 + 1e-8)}, 1},
        {"CAL|1|1e-9", {1, 1, 1, 1 + 1e-9}, 2},
        {"CAL|1|1e-10", {1, 1, 1, 1 + 1e-10}, 1},
        {"R2|1|1e-19", {1, 0, 0, 1e-19}, 2},
        {"R2|1|5e-20", {1, 0, 0, 5e-20}, 1},
        {"T3|1e-9", {1e6, 0, 0, 0, 1, 1, 0, 1, 1 + 1e-9}, 3},
        {"T3|1e-10", {1e6, 0, 0, 0, 1, 1, 0, 1, 1 + 1e-10}, 2},
    };
    for (const auto& c : cals) {
      const size_t k = c.V.size() == 9 ? 3 : 2;
      const int64_t got = piet::handlers::glmVceRank(c.V, k);
      w2check(got == c.rank, std::string("invsym cal ") + c.row + ": glm e(rank) helper gives " +
                                 std::to_string(c.rank) + " as StataBE does. Got " + std::to_string(got));
    }
  }

  // ── BUG-702: margins after an estimator runMargins does not map ────────────
  // Measured on the frozen main: `oprobit` on a 0/1 outcome then `margins,
  // dydx(x1)` returned rc 0 with dy/dx -0.0012381763 -- cobra's default LOGIT
  // AME, not the probit-family answer (-0.0012386098 after `probit` on the same
  // data); after `nbreg` the same fall-through said rc 1 "margins(logit):
  // outcome 'cnt' is not binary". No oracle arm covers either estimator's
  // margins, so the honest answer is a named r(198) refusal.
  {
    piet::SessionState s(workdir);
    seed(s);
    (void)run(s, "generate byte d = (mod(_n*37,101) > 50)");
    struct Case {
      const char* fit;
      const char* name;
    };
    const Case cases[] = {{"oprobit d x1 x2", "oprobit"}, {"nbreg cnt x1 x2", "nbreg"}};
    for (const auto& c : cases) {
      Reply f = run(s, c.fit);
      Reply m = run(s, "margins, dydx(x1)");
      w2check(f.rc == "0" && m.rc == "198" &&
                  m.msg.find(std::string("not supported after `") + c.name + "`") != std::string::npos &&
                  !m.est.is_object(),
              std::string("BUG-702: `") + c.fit + "` then `margins, dydx(x1)` is an r(198) refusal "
                  "naming the estimator, never cobra's logit AME (main: oprobit rc 0 with the logit "
                  "dy/dx; nbreg rc 1 'margins(logit)'). Got fit rc " + f.rc + ", margins rc " + m.rc +
                  " " + m.msg);
    }
    // Control: the mapped family is untouched.
    Reply p = run(s, "probit d x1 x2");
    Reply pm = run(s, "margins, dydx(x1)");
    w2check(p.rc == "0" && pm.rc == "0" && pm.est.is_object(),
            "BUG-702 control: `probit` then `margins, dydx(x1)` still answers through the engine "
            "(kModelMap). Got rc " + pm.rc + " " + pm.msg);
  }
  fs::remove_all(workdir, ec);
}
