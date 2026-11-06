// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// W2B lane `margins` -- the marginlist, atmeans and bare `margins` (FU-94, FU-95)
// and the r(b)/r(V) margins posts (FU-179, margins half).
//
// Oracles:
//  - tests/eval/golden/aer_w2/w2b_estim.log:499/:529/:558/:588 (do-file lines
//    109-127), on the do-file's own fixture. w2b_estim.do generates FLOAT columns,
//    so each value is rounded to float and loaded as a literal (the
//    selftest_w2b_predict.cpp recipe); held to T2 (rtol 1e-9).
//  - tests/eval/golden/aer_w2/w2p_estim_ev.log:1526 (G_c_margins_at) and :1704
//    (G_pc_margins_dydx), on that do-file's DOUBLE fixture. The probit arm is a
//    converged-MLE quantity, T3 (TOLERANCE.md §2).
//  - Refusal rcs 111/322/452 and the bare-margins value are live StataBE 19.5
//    measurements (2026-10-12, not goldens): `margins nosuchvar` r(111),
//    `margins id` with id no factor of the model r(322), `margins x1` with x1
//    non-integer r(452), bare `margins` after `regress y x1 x2 i.t` 12.67333335032065733.
//    The 198s are this build's own refusals of forms Stata serves (unpinned).
//
// Every arm FAILS on main before this change: `atmeans` was an unknown option
// (r(198)), the marginlist was dropped (`margins t` = bare `margins`, which after
// regress refused at rc 1 and after logit answered every AME at rc 0), and no
// margins form set r(b) ('r(b) is not set').
#include "selftest_w2.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "piet/dispatcher.h"
#include "piet/eval/errors.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

namespace {

struct MReply {
  std::string rc = "0";
  std::string msg;
  std::string text;  // the display blocks' text, one line each
  int estimationBlocks = 0;
  nlohmann::json est;  // the last estimation block
};

MReply mrun(piet::SessionState& session, const std::string& cmd) {
  MReply r;
  try {
    for (const auto& b : piet::dispatch(cmd, session)) {
      if (!b.is_object()) continue;
      const std::string t = b.value("type", std::string());
      if (t == "error") {
        r.rc = piet::eval::coarseRc(b);
        r.msg += b.value("message", std::string());
      } else if (t == "estimation") {
        ++r.estimationBlocks;
        r.est = b;
      } else if (t == "display") {
        if (!r.text.empty()) r.text += "\n";
        r.text += b.value("text", std::string());
      }
    }
  } catch (const std::exception& e) {
    r.rc = "EXC";
    r.msg = e.what();
  }
  return r;
}

std::string mfmt(double v) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.17g", v);
  return buf;
}

bool near(double got, double want, double rtol) {
  return std::isfinite(got) && std::fabs(got - want) <= rtol * std::fabs(want);
}

const piet::eval::EvalMatrix* rmat(piet::SessionState& s, const char* name) {
  return s.eval.findClassMatrix('r', name);
}

double rel(const piet::eval::EvalMatrix* m, size_t i, size_t j) {
  if (m == nullptr || i >= m->data.size() || j >= m->data[i].size()) return std::nan("");
  return m->data[i][j];
}

// The coefficient-table row `name` of the posted e() (after `post`), b or se.
double ecoef(piet::SessionState& s, const std::string& name, const char* key) {
  const auto& lm = s.lastModel();
  if (!lm.has_value() || !lm->result.has_value()) return std::nan("");
  const auto& r = *lm->result;
  if (!r.contains("coeftable") || !r["coeftable"].is_array()) return std::nan("");
  for (const auto& row : r["coeftable"]) {
    if (row.value("name", std::string()) == name && row.contains(key) && row[key].is_number()) {
      return row[key].get<double>();
    }
  }
  return std::nan("");
}

// w2b_estim.do lines 5-16, with every float column rounded to float.
void seedFloatFixture(piet::SessionState& s) {
  (void)mrun(s, "clear");
  (void)mrun(s, "set obs 60");
  (void)mrun(s, "generate double id = ceil(_n/6)");
  (void)mrun(s, "generate double t = mod(_n-1,6)+1");
  (void)mrun(s, "generate double x1 = .");
  (void)mrun(s, "generate double x2 = .");
  (void)mrun(s, "generate double y = .");
  auto fl = [](double v) { return static_cast<double>(static_cast<float>(v)); };
  for (int n = 1; n <= 60; ++n) {
    const double id = std::ceil(n / 6.0);
    const double x1 = fl(static_cast<double>((n * 7) % 11) + static_cast<double>(n % 5) / 10.0);
    const double x2 = fl(static_cast<double>((n * 3) % 5) + static_cast<double>((n * 13) % 7) / 10.0);
    const double y = fl(1.0 + 2.0 * x1 - x2 + id / 3.0 + static_cast<double>((n * 11) % 17) / 5.0);
    const std::string in = " in " + std::to_string(n);
    (void)mrun(s, "quietly replace x1 = " + mfmt(x1) + in);
    (void)mrun(s, "quietly replace x2 = " + mfmt(x2) + in);
    (void)mrun(s, "quietly replace y = " + mfmt(y) + in);
  }
  (void)mrun(s, "generate byte d = (y > 12)");
  (void)mrun(s, "xtset id t");
}

// w2p_estim_ev.do lines 14-23: the same formulas, every variable double.
void seedDoubleFixture(piet::SessionState& s) {
  (void)mrun(s, "clear");
  (void)mrun(s, "set obs 60");
  (void)mrun(s, "generate double id = ceil(_n/6)");
  (void)mrun(s, "generate double t  = mod(_n-1,6)+1");
  (void)mrun(s, "generate double x1 = mod(_n*7,11) + mod(_n,5)/10");
  (void)mrun(s, "generate double x2 = mod(_n*3,5) + mod(_n*13,7)/10");
  (void)mrun(s, "generate double y  = 1 + 2*x1 - x2 + id/3 + mod(_n*11,17)/5");
  (void)mrun(s, "generate byte d = (y > 12)");
}

// w2v_margins_corpus.do:33-60 (the w2v fixture), line by line; no RNG.
void seedW2vFixture(piet::SessionState& s) {
  static const char* kLines[] = {
      "clear",
      "set obs 240",
      "generate id = ceil(_n/6)",
      "generate t = mod(_n-1,6)+1",
      "generate region = mod(id,10)+1",
      "generate x = mod(_n*7,11) + mod(_n,5)/8",
      "generate z = mod(_n*13,101)",
      "generate c1 = mod(_n*3,5) + mod(_n*13,7)/8",
      "generate wp = 1 + mod(id,3)",
      "generate fw = 1 + mod(_n,3)",
      "generate e1 = (mod(_n*37,41)/64 + mod(_n*53,59)/64 + mod(_n*29,31)/64 - 1)*3",
      "generate y = 1 + 0.5*x + 0.03*z + 0.004*z*x - 0.6*c1 + id/4 + e1",
      "generate female = (mod(_n*7 + id,3)==0)",
      "generate passed = (mod(_n*11,4)<2)",
      "generate fpassed = female*passed",
      "generate yf = 2 - 0.8*female + 0.5*passed + 0.3*c1 + t/3 + e1",
      "generate blockcohort = mod(_n,4)+1",
      "generate treatment = (mod(_n*5,7)<3)",
      "generate y28 = 1 + 0.7*treatment + 0.4*treatment*blockcohort + blockcohort/2 + 0.3*c1 + e1",
      "generate year = 2010 + mod(_n,4)",
      "generate g = mod(id,3)+1",
      "generate remote = (mod(_n*37,41)/41 + x/40 + 0.15*(year==2012) - 0.1*(g==2) > 0.6)",
      "generate tt = (mod(_n*3,7)<3)",
      "generate d = (mod(_n*7,11)-5)/10",
      "generate dcat = mod(_n,3)",
      "generate clinic = mod(id,12)+1",
      "generate yp = exp(0.5 + 0.3*tt + 0.4*d + 0.2*tt*d - 0.3*d*d + 0.05*x)*(0.5 + mod(_n*37,41)/41)",
      "generate yc = int(yp*2)",
      "xtset id t",
  };
  for (const char* line : kLines) (void)mrun(s, line);
}

// The coefficient-table row `name` of a reply's last estimation block, b or se.
double bcoef(const MReply& r, const std::string& name, const char* key) {
  if (!r.est.is_object() || !r.est.contains("coeftable") || !r.est["coeftable"].is_array())
    return std::nan("");
  for (const auto& row : r.est["coeftable"])
    if (row.value("name", std::string()) == name && row.contains(key) && row[key].is_number())
      return row[key].get<double>();
  return std::nan("");
}

double bobs(const MReply& r) {
  return r.est.is_object() && r.est.contains("n_obs") && r.est["n_obs"].is_number()
             ? r.est["n_obs"].get<double>()
             : std::nan("");
}

// A named refusal: the rc asked for, a message, and no estimation block.
void checkRefusal(piet::SessionState& s, const std::string& label, const std::string& cmd,
                  const std::string& wantRc, const std::string& wantInMsg) {
  const MReply r = mrun(s, cmd);
  w2check(r.rc == wantRc && r.estimationBlocks == 0 && r.msg.find(wantInMsg) != std::string::npos,
          label + ": `" + cmd + "` is a named refusal, rc " + wantRc + " (never rc 0 with " +
              "results, never rc 1), message names `" + wantInMsg + "`. Got rc " + r.rc + " (" +
              std::to_string(r.estimationBlocks) + " estimation blocks) `" + r.msg + "`");
}

}  // namespace

void testW2bMargins() {
  std::printf("[selftest] --- W2B margins: marginlist, atmeans, bare margins, r(b)/r(V) ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_w2bmargins").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  {
    piet::SessionState s(workdir);
    seedFloatFixture(s);

    // ── margins_atmeans_post (w2b_estim.log:499) ─────────────────────────
    // main: r(198), `atmeans` was not in the margins vocabulary.
    (void)mrun(s, "quietly regress y x1 x2 i.t");
    {
      const MReply r = mrun(s, "margins t, atmeans post");
      const double b1 = ecoef(s, "1.t", "b"), b6 = ecoef(s, "6.t", "b"), se1 = ecoef(s, "1.t", "se");
      const std::string ecmd = s.eval.eStrings.get("cmd", "");
      w2check(r.rc == "0" && near(b1, 12.7622444005381, 1e-9) && near(b6, 12.92006156482251, 1e-9) &&
                  near(se1, .4552220808934213, 1e-9) && ecmd == "margins",
              "w2b_estim margins_atmeans_post: `margins t, atmeans post` rc 0, _b[1.t] "
              "12.7622444005381, _b[6.t] 12.92006156482251, _se[1.t] .4552220808934213, e(cmd) "
              "margins. Got rc " + r.rc + " " + mfmt(b1) + " " + mfmt(b6) + " " + mfmt(se1) +
                  " `" + ecmd + "` " + r.msg);
    }

    // ── margins_noatmeans_ctl (w2b_estim.log:529) ────────────────────────
    // main: rc 1, the marginlist was dropped and bare `margins` after regress
    // refused under the dydx wording.
    (void)mrun(s, "quietly regress y x1 x2 i.t");
    {
      const MReply r = mrun(s, "margins t");
      const auto* b = rmat(s, "b");
      const auto* V = rmat(s, "V");
      // BUG-1240: the first level's column is `1bn.t` (StataBE 19.5 `margins c` after
      // `regress y x1 x2 i.c`: `0bn.c 1.c 2.c 3.c`, scratch-laneA/agents/E3/rx2.do).
      const bool shape = b != nullptr && b->data.size() == 1 && b->data[0].size() == 6 &&
                         b->colnames.size() == 6 && b->colnames[0] == "1bn.t" &&
                         b->colnames[5] == "6.t" && V != nullptr && V->data.size() == 6;
      w2check(r.rc == "0" && shape && near(rel(b, 0, 0), 12.76224435126746, 1e-9) &&
                  near(rel(b, 0, 5), 12.92006151555187, 1e-9) &&
                  near(std::sqrt(rel(V, 0, 0)), .4552220809308993, 1e-9),
              "w2b_estim margins_noatmeans_ctl: `margins t` rc 0, r(b) 1x6 (1bn.t..6.t), "
              "el(r(b),1,1) 12.76224435126746, el(r(b),1,6) 12.92006151555187, "
              "sqrt(el(r(V),1,1)) .4552220809308993. Got rc " + r.rc + " " +
                  mfmt(rel(b, 0, 0)) + " " + mfmt(rel(b, 0, 5)) + " " +
                  mfmt(std::sqrt(rel(V, 0, 0))) + " " + r.msg);
      // The covariance across levels is the engine's x̄_p' V x̄_q, not a zero:
      // live StataBE el(r(V),1,2) .00007652 (4 printed digits).
      w2check(near(rel(V, 0, 1), .00007652, 5e-4) && rel(V, 0, 1) == rel(V, 1, 0),
              "margins t: r(V) carries the off-diagonal covariance (StataBE .00007652), "
              "symmetric. Got " + mfmt(rel(V, 0, 1)) + " / " + mfmt(rel(V, 1, 0)));
      w2check(s.eval.rValues.contains("N") && *s.eval.rValues.find("N") == 60.0,
              "margins t: r(N) 60");
    }

    // ── margins_overall_atmeans (w2b_estim.log:558) ──────────────────────
    // main: r(198) (`atmeans`). The means are held at float precision; the
    // double means give 12.6733333503 (3.7e-9 off).
    {
      const MReply r = mrun(s, "margins, atmeans");
      const auto* b = rmat(s, "b");
      const auto* V = rmat(s, "V");
      w2check(r.rc == "0" && near(rel(b, 0, 0), 12.67333339694155, 1e-9) &&
                  near(std::sqrt(rel(V, 0, 0)), .1855244278812313, 1e-9) && b != nullptr &&
                  b->colnames.size() == 1 && b->colnames[0] == "_cons",
              "w2b_estim margins_overall_atmeans: `margins, atmeans` rc 0, el(r(b),1,1) "
              "12.67333339694155, sqrt(el(r(V),1,1)) .1855244278812313, column _cons. Got rc " +
                  r.rc + " " + mfmt(rel(b, 0, 0)) + " " + mfmt(std::sqrt(rel(V, 0, 0))) + " " +
                  r.msg);
    }

    // ── margins_dydx_atmeans (w2b_estim.log:588) ─────────────────────────
    // main: r(198) (`atmeans`). A linear dy/dx is the coefficient at any point.
    {
      const MReply r = mrun(s, "margins, dydx(x1) atmeans");
      w2check(r.rc == "0" && near(rel(rmat(s, "b"), 0, 0), 1.95728887906858, 1e-9),
              "w2b_estim margins_dydx_atmeans: `margins, dydx(x1) atmeans` rc 0, el(r(b),1,1) "
              "1.95728887906858. Got rc " + r.rc + " " + mfmt(rel(rmat(s, "b"), 0, 0)) + " " +
                  r.msg);
    }

    // ── bare `margins` after a linear fit (FU-95) ────────────────────────
    // main: rc 1 ("marginal effects (dydx) after a linear model"). One row, the
    // mean prediction over e(sample) = mean(y) with a constant (live StataBE
    // 12.67333335032065733). It equals :558 up to the float-held means (3.7e-9).
    {
      const MReply r = mrun(s, "margins");
      const double bare = rel(rmat(s, "b"), 0, 0);
      (void)mrun(s, "quietly summarize y");
      const double meanY = s.eval.rValues.contains("mean") ? *s.eval.rValues.find("mean") : std::nan("");
      w2check(r.rc == "0" && near(bare, 12.67333335032066, 1e-9) && near(bare, meanY, 1e-12) &&
                  near(bare, 12.67333339694155, 1e-8),
              "bare margins after regress: rc 0, one row 12.67333335032066 (StataBE live) = "
              "mean(y), within 1e-8 of margins_overall_atmeans :558. Got rc " + r.rc + " " +
                  mfmt(bare) + " mean(y) " + mfmt(meanY) + " " + r.msg);
    }

    // ── named refusals of the marginlist (FU-94) ─────────────────────────
    // main: each was dropped and answered as bare `margins` (rc 1 here).
    (void)mrun(s, "quietly regress y x1 x2 i.t");
    checkRefusal(s, "margins nonfactor", "margins x1", "452",
                 "factor variables may not contain noninteger values");
    checkRefusal(s, "margins two terms (Stata stops at x1)", "margins t x1", "452",
                 "noninteger");
    checkRefusal(s, "margins not in model", "margins id", "322",
                 "factor id not found in list of covariates");
    checkRefusal(s, "margins unknown variable", "margins nosuchvar", "111",
                 "variable nosuchvar not found");
    // main: rc 0, the marginlist dropped and dydx(x1) served alone.
    checkRefusal(s, "margins marginlist+dydx", "margins t, dydx(x1)", "198", "not implemented");
    (void)mrun(s, "quietly regress y x1 x2 i.t i.id");
    checkRefusal(s, "margins several factors", "margins t id", "198", "several terms");
    // Control (passes on main too): over() is outside the vocabulary.
    checkRefusal(s, "margins over() control", "margins, over(t)", "198", "over");

    // ── after logit: no AMEs in place of a predictive margin (FU-95) ─────
    // main: rc 0, bare `margins` / `margins t` answered every covariate's AME.
    (void)mrun(s, "quietly logit d x1 x2");
    checkRefusal(s, "bare margins after logit", "margins", "198", "not implemented");
    (void)mrun(s, "quietly logit d x1 x2 i.t");
    {
      // margins-corpus-shapes: a factor marginlist after logit is the average
      // predicted probability at each level (w2v_margins_corpus F6_year_asobs,
      // r(b) columns `2010bn.year 2011.year ...`). main: r(198) "not implemented".
      const MReply r = mrun(s, "margins t");
      const auto* b = rmat(s, "b");
      bool probs = b != nullptr && b->data.size() == 1 && b->data[0].size() == 6;
      for (size_t j = 0; probs && j < 6; ++j) probs = b->data[0][j] > 0 && b->data[0][j] < 1;
      w2check(r.rc == "0" && probs && b->colnames[0] == "1bn.t" && b->colnames[5] == "6.t",
              "margins t after logit: rc 0, six probabilities named 1bn.t .. 6.t (F6_year_asobs "
              "shape). Got rc " + r.rc + " " + (b ? "k " + std::to_string(b->colnames.size()) + " " +
                  (b->colnames.empty() ? std::string() : b->colnames[0]) : std::string("r(b) not set")) +
                  " " + r.msg);
    }
    // main: r(198) too, but as an unknown option ("option `atmeans` not allowed").
    checkRefusal(s, "margins, atmeans after logit", "margins, dydx(x1) atmeans", "198",
                 "margins, atmeans after `logit`");

    // ── `if` chooses the averaging rows (review round 1) ─────────────────
    // Stata averages over the qualified rows of the full-sample fit; no path
    // here does, so each refuses by name. 88915bae: `margins if t==1` rc 0 with
    // the all-rows 12.673333 (the t==1 rows average 11.913333). main: rc 1
    // (the dydx-after-linear wording) and `margins if t==1, dydx(x1)` rc 0.
    (void)mrun(s, "quietly regress y x1 x2 i.t");
    checkRefusal(s, "bare margins if", "margins if t==1", "198", "if qualifier");
    checkRefusal(s, "margins t if", "margins t if t>2", "198", "if qualifier");
    checkRefusal(s, "margins dydx() if", "margins if t==1, dydx(x1)", "198", "if qualifier");
    // Controls (refused on main too): `in` and weights meet the registry's
    // allowInRange/WeightPolicy gates before the margins arm.
    checkRefusal(s, "margins in control", "margins in 1/10", "198", "row-range qualifier");
    checkRefusal(s, "margins weights control", "margins [aweight=x2]", "198",
                 "weighted estimation");

    // ── a factor dy/dx carries the base level (BUG-1237) ─────────────────
    // Stata's matrix carries the base level as a column (b 0, V 0;
    // w2_glm_lifecycle.log:1663; StataBE 19.5 `margins, dydx(c)` after `regress y
    // x1 x2 i.c`: `0b.c 1.c 2.c 3.c`, scratch-laneA/agents/E3/rx2.do); 88915bae
    // posted el(r(b),1,1) = the 2.t effect -.1604234767395158 where Stata reads 0.
    // r() is still replaced: a stale r(b) from `margins t` must not survive and
    // r(N) is this command's (main: r() untouched, r(N) the summarize's 10).
    (void)mrun(s, "quietly regress y x1 x2 i.t");
    (void)mrun(s, "quietly summarize x1 in 1/10");
    (void)mrun(s, "margins t");
    {
      const MReply r = mrun(s, "margins, dydx(t)");
      const bool nN = s.eval.rValues.contains("N") && *s.eval.rValues.find("N") == 60.0;
      const auto* b = rmat(s, "b");
      const bool shape = b != nullptr && b->colnames.size() == 6 && b->colnames[0] == "1b.t" &&
                         b->colnames[1] == "2.t" && rel(b, 0, 0) == 0.0 && rmat(s, "V") != nullptr &&
                         rel(rmat(s, "V"), 0, 0) == 0.0;
      w2check(r.rc == "0" && r.estimationBlocks == 1 && shape && nN,
              "margins, dydx(t): rc 0 with the effects table, r(b) `1b.t 2.t ...` with the base "
              "level 0 and its V row 0 (BUG-1237), r(N) 60. Got rc " + r.rc + " r(b) " +
                  (b ? "set el11 " + mfmt(rel(b, 0, 0)) + " " + b->colnames[0] : "not set") +
                  (nN ? "" : " r(N) not 60") + " " + r.msg);
    }
    {
      const MReply r = mrun(s, "margins, dydx(x1 t)");
      const auto* b = rmat(s, "b");
      w2check(r.rc == "0" && b != nullptr && b->colnames.size() == 7 && b->colnames[0] == "x1" &&
                  b->colnames[1] == "1b.t",
              "margins, dydx(x1 t): r(b) `x1 1b.t 2.t ...` in e(b) order (BUG-1237; StataBE "
              "dydx(x1 c): `x1 0b.c 1.c 2.c 3.c`). Got rc " + r.rc + " " +
                  (b ? "r(b) set " + b->colnames[0] : "not set"));
    }

    // ── atmeans after an interaction/polynomial (review round 1) ─────────
    // The engine holds each DESIGN column at its mean; Stata holds x1 at its
    // mean and rebuilds x1#x1 = mean(x1)^2. 88915bae: `margins, atmeans` rc 0
    // 12.673333 where the fit at the means is 12.950175. main: r(198) as an
    // unknown option (message without the interaction wording).
    // margins-corpus-shapes serves the bare and at() forms: every covariate at its
    // mean and x1#x1 rebuilt as mean(x1)^2 (w2v_margins_corpus X1_atmeans
    // 9.922553971152869 after `regress y c.x##c.x c1`; a factor level at its float
    // share, w2b_estim margins_atmeans_post). The expected point prediction is
    // built here from the fit's own coefficients; 88915bae's 12.673333 (the
    // design-column means) is what this must not equal.
    (void)mrun(s, "quietly regress y c.x1##c.x1 x2 i.t");
    {
      double b0 = std::nan(""), b1 = b0, b11 = b0, b2 = b0, bt = 0;
      const auto& lm = s.lastModel();
      if (lm.has_value() && lm->result.has_value() && lm->result->contains("coeftable")) {
        for (const auto& row : (*lm->result)["coeftable"]) {
          const std::string nm = row.value("name", std::string());
          const double v = row.contains("b") && row["b"].is_number() ? row["b"].get<double>() : 0.0;
          if (nm == "(intercept)" || nm == "_cons") b0 = v;
          else if (nm == "x1") b1 = v;
          else if (nm == "x1#x1") b11 = v;
          else if (nm == "x2") b2 = v;
          else if (nm.size() == 3 && nm[1] == '.' && nm[2] == 't') bt += v;
        }
      }
      double m1 = 0, m2 = 0;
      for (int n = 1; n <= 60; ++n) {
        auto fl = [](double v) { return static_cast<double>(static_cast<float>(v)); };
        m1 += fl(static_cast<double>((n * 7) % 11) + static_cast<double>(n % 5) / 10.0);
        m2 += fl(static_cast<double>((n * 3) % 5) + static_cast<double>((n * 13) % 7) / 10.0);
      }
      // Each mean is held at float precision whatever the storage type (FU-668,
      // w2v_f7c_jacobian.log:775-794); w2b_estim.do's own columns are float.
      m1 = static_cast<double>(static_cast<float>(m1 / 60));
      m2 = static_cast<double>(static_cast<float>(m2 / 60));
      const double share = static_cast<double>(static_cast<float>(1.0 / 6.0));
      const double want = b0 + b1 * m1 + b11 * m1 * m1 + b2 * m2 + bt * share;
      const MReply r = mrun(s, "margins, atmeans");
      const double got = rel(rmat(s, "b"), 0, 0);
      w2check(r.rc == "0" && near(got, want, 1e-12),
              "margins, atmeans after c.x1##c.x1 x2 i.t: rc 0, the fit at the covariate means "
              "with x1#x1 = mean(x1)^2 (X1_atmeans shape). main: r(198). Got rc " + r.rc + " " +
                  mfmt(got) + " want " + mfmt(want) + " " + r.msg);
      const MReply a = mrun(s, "margins, at(x2=1) atmeans");
      const double gotA = rel(rmat(s, "b"), 0, 0);
      const double wantA = want + b2 * (1.0 - m2);
      w2check(a.rc == "0" && near(gotA, wantA, 1e-12),
              "margins, at(x2=1) atmeans after c.x1##c.x1 x2 i.t: rc 0, the same point with x2 "
              "set to 1. main: r(198). Got rc " + a.rc + " " + mfmt(gotA) + " want " + mfmt(wantA) +
                  " " + a.msg);
    }
    // The per-level path keeps its refusal (no oracle for a marginlist with
    // atmeans after an interaction yet).
    checkRefusal(s, "atmeans polynomial marginlist", "margins t, atmeans", "198",
                 "interaction or polynomial");
    // Controls: without atmeans the average prediction is the column-mean
    // prediction (linear in the columns) = mean(y) with a constant; dydx(x2) is
    // the coefficient at any point.
    {
      const MReply r = mrun(s, "margins");
      const double bare = rel(rmat(s, "b"), 0, 0);
      w2check(r.rc == "0" && near(bare, 12.67333335032066, 1e-9),
              "bare margins after the polynomial fit (control): rc 0, mean(y) 12.67333335032066. "
              "Got rc " + r.rc + " " + mfmt(bare) + " " + r.msg);
      const MReply d = mrun(s, "margins, dydx(x2) atmeans");
      const double bx2 = rel(rmat(s, "b"), 0, 0);
      (void)mrun(s, "quietly regress y c.x1##c.x1 x2 i.t");
      const auto& lm = s.lastModel();
      double coefX2 = std::nan("");
      if (lm.has_value() && lm->result.has_value() && lm->result->contains("coeftable")) {
        for (const auto& row : (*lm->result)["coeftable"]) {
          if (row.value("name", std::string()) == "x2" && row.contains("b") && row["b"].is_number())
            coefX2 = row["b"].get<double>();
        }
      }
      w2check(d.rc == "0" && near(bx2, coefX2, 1e-12),
              "margins, dydx(x2) atmeans after the polynomial fit (control): rc 0, _b[x2]. Got rc " +
                  d.rc + " " + mfmt(bx2) + " vs " + mfmt(coefX2) + " " + d.msg);
    }

    // ── FU-342: an at() variable inside a derived column ─────────────────
    // The engine overrode the column x1 and kept x1#x1 at its observed values
    // (39fd4ccb: rc 0 with x1#x1 at its sample mean). main refused it by name
    // (r(198)); margins-corpus-shapes serves it with x1#x1 rebuilt as 25
    // (w2v_margins_corpus X1_at5 9.737407233828263 after `regress y c.x##c.x c1`),
    // which must equal the hand-built `x1 x1sq` design at (x1=5, x1sq=25).
    {
      (void)mrun(s, "capture drop x1sq");
      (void)mrun(s, "generate double x1sq = x1*x1");
      (void)mrun(s, "quietly regress y x1 x1sq x2");
      (void)mrun(s, "margins, at(x1=5 x1sq=25)");
      const double handBuilt = rel(rmat(s, "b"), 0, 0);
      (void)mrun(s, "quietly regress y c.x1##c.x1 x2");
      const MReply r = mrun(s, "margins, at(x1=5)");
      const double grid = rel(rmat(s, "b"), 0, 0);
      const auto* b = rmat(s, "b");
      w2check(r.rc == "0" && near(grid, handBuilt, 1e-10) && b != nullptr &&
                  b->colnames.size() == 1 && b->colnames[0] == "_cons",
              "at() polynomial: `margins, at(x1=5)` after c.x1##c.x1 x2 is rc 0, column _cons, "
              "equal to the x1 x1sq design at (5, 25). main: r(198). Got rc " + r.rc + " " +
                  mfmt(grid) + " vs " + mfmt(handBuilt) + " " + r.msg);
    }
    // i.t#c.x1 spans the same columns as the six hand-built slopes tx_k =
    // (t==k)*x1, so the at(x1=5) margin is the mean over the sample of
    // _cons + _b[x2]*x2 + _b[tx_k]*5 on each row's own level k (review
    // revi-20261014T140858-1af3eb: the arm used to accept any finite value).
    {
      (void)mrun(s, "capture drop tx1 tx2 tx3 tx4 tx5 tx6 hb");
      for (int k = 1; k <= 6; ++k)
        (void)mrun(s, "generate double tx" + std::to_string(k) + " = (t==" + std::to_string(k) + ")*x1");
      (void)mrun(s, "quietly regress y tx1 tx2 tx3 tx4 tx5 tx6 x2");
      (void)mrun(s, "generate double hb = _b[_cons] + _b[x2]*x2 + 5*(_b[tx1]*(t==1) + _b[tx2]*(t==2) + "
                    "_b[tx3]*(t==3) + _b[tx4]*(t==4) + _b[tx5]*(t==5) + _b[tx6]*(t==6))");
      (void)mrun(s, "quietly summarize hb, meanonly");
      const double handBuilt = s.eval.rValues.contains("mean") ? *s.eval.rValues.find("mean") : std::nan("");
      (void)mrun(s, "quietly regress y i.t#c.x1 x2");
      const MReply r = mrun(s, "margins, at(x1=5)");
      const double got = rel(rmat(s, "b"), 0, 0);
      w2check(r.rc == "0" && near(got, handBuilt, 1e-10),
              "at() factor-by-continuous: `margins, at(x1=5)` after i.t#c.x1 x2 is rc 0 and equals "
              "the hand-built tx1..tx6 design's mean prediction at x1=5 (every t#x1 column rebuilt). "
              "main: r(198). Got rc " + r.rc + " " + mfmt(got) + " vs " + mfmt(handBuilt) + " " +
                  r.msg);
      (void)mrun(s, "capture drop tx1 tx2 tx3 tx4 tx5 tx6 hb");
    }
    // Control: at() of a covariate outside every interaction is the same
    // answer as the hand-built polynomial `x1 x1sq` design.
    {
      (void)mrun(s, "capture drop x1sq");
      (void)mrun(s, "generate double x1sq = x1*x1");
      (void)mrun(s, "quietly regress y x1 x1sq x2");
      (void)mrun(s, "margins, at(x2=1)");
      const double handBuilt = rel(rmat(s, "b"), 0, 0);
      (void)mrun(s, "quietly regress y c.x1##c.x1 x2");
      const MReply r = mrun(s, "margins, at(x2=1)");
      const double factorSyntax = rel(rmat(s, "b"), 0, 0);
      w2check(r.rc == "0" && near(factorSyntax, handBuilt, 1e-10),
              "margins, at(x2=1) after c.x1##c.x1 x2 (control): rc 0, equal to the x1 x1sq "
              "design's margin. Got rc " + r.rc + " " + mfmt(factorSyntax) + " vs " +
                  mfmt(handBuilt) + " " + r.msg);
    }

    // ── FU-341: `if` with the pre-existing at() form ─────────────────────
    // main: rc 0 with n_obs 60, the full-sample answer (FU-341 repro).
    (void)mrun(s, "quietly regress y x1 x2 i.t");
    checkRefusal(s, "margins at() if", "margins if t==1, at(x1=1)", "198", "if qualifier");
  }

  // ── FU-179: r(b)/r(V) after the existing at() and dydx() forms ─────────
  {
    piet::SessionState s(workdir);
    seedDoubleFixture(s);
    // w2p_estim_ev.log:1526 G_c_margins_at. main: PRECFAIL 'r(b) is not set'.
    (void)mrun(s, "quietly regress y x1 x2");
    {
      const MReply r = mrun(s, "margins, at(x1=5)");
      const auto* b = rmat(s, "b");
      w2check(r.rc == "0" && near(rel(b, 0, 0), 12.18343294314332, 1e-9) &&
                  near(rel(rmat(s, "V"), 0, 0), .0320938394163614, 1e-9) && b != nullptr &&
                  b->colnames.size() == 1 && b->colnames[0] == "_cons",
              "w2p_estim_ev G_c_margins_at: `margins, at(x1=5)` posts el(r(b),1,1) "
              "12.18343294314332, el(r(V),1,1) .0320938394163614, column _cons. Got rc " +
                  r.rc + " " + mfmt(rel(b, 0, 0)) + " " + mfmt(rel(rmat(s, "V"), 0, 0)) + " " +
                  r.msg);
    }
    // w2p_estim_ev.log:1704 G_pc_margins_dydx (T3: converged probit).
    (void)mrun(s, "quietly probit d x1 x2");
    {
      const MReply r = mrun(s, "margins, dydx(x1)");
      const auto* b = rmat(s, "b");
      w2check(r.rc == "0" && near(rel(b, 0, 0), .089805000869499, 1e-8) &&
                  near(rel(rmat(s, "V"), 0, 0), .0000147564915036, 1e-7) && b != nullptr &&
                  b->colnames.size() == 1 && b->colnames[0] == "x1",
              "w2p_estim_ev G_pc_margins_dydx: `margins, dydx(x1)` after probit posts "
              "el(r(b),1,1) .089805000869499, el(r(V),1,1) .0000147564915036 (T3). Got rc " +
                  r.rc + " " + mfmt(rel(b, 0, 0)) + " " + mfmt(rel(rmat(s, "V"), 0, 0)) + " " +
                  r.msg);
    }
    // Two effects: the cross covariance is e(V)'s (BUG-1241; StataBE 19.5 on this
    // fixture, scratch-laneA/agents/p-margins/s5.do: el(r(V),1,2)
    // -.0006508024675375354 after `regress y x1 x2`, -.0007091206124858477 after
    // `regress y x1 x2 i.t`), never a silent 0.
    (void)mrun(s, "quietly regress y x1 x2");
    {
      const MReply r = mrun(s, "margins, dydx(x1 x2)");
      const auto* V = rmat(s, "V");
      w2check(r.rc == "0" && V != nullptr && V->data.size() == 2 &&
                  near(rel(V, 0, 1), -.0006508024675375354, 1e-9) && rel(V, 0, 1) == rel(V, 1, 0) &&
                  std::isfinite(rel(V, 0, 0)),
              "margins, dydx(x1 x2): r(V) 2x2 with the e(V) off-diagonal -.0006508024675375354 "
              "(BUG-1241). Got rc " + r.rc + " V12 " + mfmt(rel(V, 0, 1)));
    }
  }

  // ── FU-276: margins' r() is margins' own, declared ─────────────────────
  // Live StataBE 19.5 (2026-10-13) `return list` after bare `margins` following
  // `regress y x1 x2 i.t`: r(level) 95, r(cmd) margins, r(df_r) 52, r(table)
  // 9 x 1 (df 52, crit 2.006646805061686578); after `margins, dydx(x1)`:
  // r(derivatives) dy/dx, r(xvars) x1; after `margins t, post` r(b)/r(V)/
  // r(table) are still posted. Golden shape: w2o_rnames.log:436 (N26) / :448
  // (N27). main: margins was class U -- every r() read refused r(111).
  {
    piet::SessionState s(workdir);
    seedDoubleFixture(s);
    (void)mrun(s, "quietly summarize x1");
    (void)mrun(s, "quietly regress y x1 x2 i.t");
    {
      const MReply r = mrun(s, "margins");
      const auto* tb = rmat(s, "table");
      const double* lv = s.eval.rValues.find("level");
      const double* dfr = s.eval.rValues.find("df_r");
      const std::string cmd = s.eval.rStrings.get("cmd", "");
      const bool tableOk = tb != nullptr && tb->data.size() == 9 && tb->colnames.size() == 1 &&
                           tb->colnames[0] == "_cons" && rel(tb, 6, 0) == 52.0 &&
                           near(rel(tb, 7, 0), 2.006646805061686578, 1e-12) &&
                           near(rel(tb, 0, 0), rel(rmat(s, "b"), 0, 0), 1e-15);
      w2check(r.rc == "0" && lv != nullptr && *lv == 95.0 && dfr != nullptr && *dfr == 52.0 &&
                  cmd == "margins" && tableOk,
              "FU-276 bare margins: r(level) 95, r(df_r) 52, r(cmd) margins, r(table) 9x1 _cons "
              "(df 52, crit 2.006646805061686578, b = r(b)). Got rc " + r.rc + " level " +
                  (lv ? mfmt(*lv) : "unset") + " df_r " + (dfr ? mfmt(*dfr) : "unset") + " cmd `" +
                  cmd + "` table " + (tb ? "set" : "unset") + " " + r.msg);
      const MReply mean = mrun(s, "display r(mean)");
      const MReply kat = mrun(s, "display r(k_at)");
      const double* katV = s.eval.rValues.find("k_at");
      w2check(mean.rc == "0" && kat.rc == "0" && katV != nullptr && *katV == 0.0,
              "FU-276 bare margins: r(mean) (not Stata's) reads `.` at rc 0 (w2o_rnames.log:436); "
              "r(k_at) is Stata's 0 (BUG-1449, scratch-laneA/agents/E3/rx2.do). Got " + mean.rc +
                  " / " + kat.rc + " " + kat.msg);
    }
    (void)mrun(s, "quietly regress y x1 x2 i.t");
    {
      const MReply r = mrun(s, "margins, dydx(x1)");
      const std::string der = s.eval.rStrings.get("derivatives", "");
      const std::string xv = s.eval.rStrings.get("xvars", "");
      w2check(r.rc == "0" && der == "dy/dx" && xv == "x1" && rmat(s, "table") != nullptr,
              "FU-276 margins, dydx(x1): r(derivatives) dy/dx, r(xvars) x1 (w2o_rnames.log:448), "
              "r(table) posted. Got rc " + r.rc + " `" + der + "` `" + xv + "` " + r.msg);
    }
    (void)mrun(s, "quietly regress y x1 x2 i.t");
    {
      const MReply r = mrun(s, "margins t, post");
      const auto* b = rmat(s, "b");
      w2check(r.rc == "0" && b != nullptr && b->colnames.size() == 6 && rmat(s, "V") != nullptr &&
                  rmat(s, "table") != nullptr && s.eval.eStrings.get("cmd", "") == "margins",
              "FU-276 margins t, post: e(cmd) margins and r(b) 1x6 / r(V) / r(table) still "
              "posted (StataBE return list after post). Got rc " + r.rc + " r(b) " +
                  (b ? "set" : "unset") + " " + r.msg);
    }
  }

  // ── FU-410: nlcom posts r(V) = G V G' ──────────────────────────────────
  // w2p_estim_ev.log:1502 G_c_nlcom: after `regress y x1 x2`, `nlcom 2*_b[x1]`
  // el(r(V),1,1) .0129450049474129. main: 'r(V) after `nlcom`: Stata posts this
  // result and this version of PIET does not', r(111).
  {
    piet::SessionState s(workdir);
    seedDoubleFixture(s);
    (void)mrun(s, "quietly regress y x1 x2");
    {
      const MReply r = mrun(s, "nlcom 2*_b[x1]");
      const auto* V = rmat(s, "V");
      const double se = rel(rmat(s, "table"), 1, 0);
      w2check(r.rc == "0" && near(rel(V, 0, 0), .0129450049474129, 1e-9) &&
                  near(std::sqrt(rel(V, 0, 0)), se, 1e-12),
              "w2p_estim_ev G_c_nlcom: el(r(V),1,1) .0129450049474129, sqrt = the table se to "
              "1e-12. Got rc " + r.rc + " " + mfmt(rel(V, 0, 0)) + " se " + mfmt(se) + " " + r.msg);
    }
    (void)mrun(s, "quietly regress y x1 x2");
    (void)mrun(s, "matrix EB = e(b)");
    (void)mrun(s, "matrix EV = e(V)");
    {
      const MReply r = mrun(s, "nlcom (r1: _b[x1]/_b[x2]) (r2: _b[x1]*_b[x2])");
      const auto* V = rmat(s, "V");
      const auto* eb = s.eval.findMatrix("EB");
      const auto* eV = s.eval.findMatrix("EV");
      // Hand Jacobians over (x1, x2): r1 = b1/b2 -> (1/b2, -b1/b2^2); r2 = b1*b2
      // -> (b2, b1). Cross covariance G1 V G2' from e(V)'s x1/x2 block.
      double hand = std::nan("");
      if (eb != nullptr && eV != nullptr && eb->colnames.size() >= 2 && eV->data.size() >= 2) {
        size_t i1 = 0, i2 = 1;
        for (size_t j = 0; j < eb->colnames.size(); ++j) {
          if (eb->colnames[j] == "x1") i1 = j;
          if (eb->colnames[j] == "x2") i2 = j;
        }
        const double b1 = eb->data[0][i1], b2 = eb->data[0][i2];
        const double g1[2] = {1.0 / b2, -b1 / (b2 * b2)};
        const double g2[2] = {b2, b1};
        const size_t ix[2] = {i1, i2};
        hand = 0;
        for (int a = 0; a < 2; ++a)
          for (int c = 0; c < 2; ++c) hand += g1[a] * eV->data[ix[a]][ix[c]] * g2[c];
      }
      const bool shape = V != nullptr && V->data.size() == 2 && V->colnames.size() == 2 &&
                         V->colnames[0] == "r1" && V->colnames[1] == "r2";
      const double se1 = rel(rmat(s, "table"), 1, 0), se2 = rel(rmat(s, "table"), 1, 1);
      w2check(r.rc == "0" && shape && rel(V, 0, 1) == rel(V, 1, 0) && near(rel(V, 0, 1), hand, 1e-6) &&
                  near(std::sqrt(rel(V, 0, 0)), se1, 1e-12) && near(std::sqrt(rel(V, 1, 1)), se2, 1e-12),
              "nlcom two terms: r(V) 2x2 (r1 r2), symmetric, diagonal = table se^2, el(r(V),1,2) "
              "= G1 e(V) G2' by hand " + mfmt(hand) + ". Got rc " + r.rc + " V12 " +
                  mfmt(rel(V, 0, 1)) + " V21 " + mfmt(rel(V, 1, 0)) + " " + r.msg);
    }
  }

  // ── Reads after `margins ..., post` and a display that fails part-way ──
  // w2v_margins_corpus (StataBE 19.5): F1_dydx_at_post_bname1/2 read `_b[1._at]`
  // after `margins, dydx(x) at(z=(...)) post`, whose e(b) is `x:1bn._at ...`
  // (an equation-less name is the FIRST equation's); F7nb_dcat_dydx_post_bname1
  // prints `...|1.tt#0.dcat|[1.tt#0.dcat] not found` (the pieces before the
  // failing one, then the message); F7_dydx_minmax_b prints `...|121|type
  // mismatch` and is r(109) for el(r(b),1,1) with r(b) absent; F3_at_atmeans_enopost
  // reads e(N_full) after reghdfe. main: `_b[1._at]` r(111), no display line,
  // el() rc 1, e(N_full) `.`.
  {
    piet::SessionState s(workdir);
    seedDoubleFixture(s);
    (void)mrun(s, "quietly regress y c.x1##c.x2");
    {
      const MReply r = mrun(s, "margins, dydx(x1) at(x2=(0 3)) post");
      const MReply bare = mrun(s, "display %21.0g _b[1._at] \"|\" %21.0g _se[2._at]");
      const MReply full = mrun(s, "display %21.0g _b[x1:1._at] \"|\" %21.0g _se[x1:2._at]");
      const MReply loc = mrun(s, "local q = _b[2._at]");
      const MReply locFull = mrun(s, "display %21.0g `q' \"|\" %21.0g _b[x1:2._at]");
      // The local holds the macro layer's own rendering of the value, so it is
      // compared as a number.
      const auto bar = locFull.text.find('|');
      const double viaLocal = bar == std::string::npos ? std::nan("")
                                                       : std::strtod(locFull.text.c_str(), nullptr);
      const double direct = bar == std::string::npos
                                ? std::nan("")
                                : std::strtod(locFull.text.c_str() + bar + 1, nullptr);
      w2check(r.rc == "0" && bare.rc == "0" && bare.text == full.text && !full.text.empty() &&
                  loc.rc == "0" && near(viaLocal, direct, 1e-14),
              "_b[1._at]/_se[2._at] after `margins, dydx(x1) at() post` read e(b)'s first "
              "equation (x1:), on display and on the macro layer (w2v_margins_corpus "
              "F1_dydx_at_post_bname1/2; main r(111)). Got `" + bare.text + "` vs `" + full.text +
                  "`, local `" + locFull.text + "` rc " + bare.rc + "/" + loc.rc + " " + bare.msg);
      const MReply miss = mrun(s, "capture noisily display \"A|\" %18.0g _b[1.x1#0.x2] \"|\"");
      w2check(miss.text == "A|[1.x1#0.x2] not found" && s.eval.locals.find("_rc") != nullptr &&
                  *s.eval.locals.find("_rc") == "111",
              "a display whose _b[] piece is unknown prints the pieces before it and Stata's "
              "message (w2v_margins_corpus F7nb_dcat_dydx_post_bname1), _rc 111; main printed "
              "nothing. Got `" + miss.text + "`");
      const MReply first = mrun(s, "capture noisily display _b[nosuch] \"|\"");
      w2check(first.text.empty(),
              "display failing at its FIRST piece shows no display line (the error block "
              "carries the message). Got `" + first.text + "`");
    }
    (void)mrun(s, "quietly summarize x1");
    {
      const MReply r = mrun(s, "capture noisily display \"A|\" %18.0g el(r(b),1,1) \"|\"");
      const std::string* rc = s.eval.locals.find("_rc");
      w2check(r.text == "A|type mismatch" && rc != nullptr && *rc == "109",
              "el(r(b),1,1) with r(b) absent is r(109) type mismatch, printed after the "
              "pieces before it (w2v_margins_corpus F7_dydx_minmax_b; main rc 1, no line). "
              "Got `" + r.text + "` _rc " + (rc ? *rc : std::string("?")));
    }
    {
      (void)mrun(s, "generate double g = cond(_n==1, 99, t)");
      const MReply f = mrun(s, "reghdfe y x1 x2, absorb(g)");
      const MReply n = mrun(s, "display e(N) \"|\" e(N_full)");
      w2check(f.rc == "0" && n.text == "59|60",
              "e(N_full) after reghdfe is e(N) plus the dropped singleton (reghdfe.mata "
              "e(N_full) = e(N) + num_singletons; w2v_margins_corpus F3_at_atmeans_enopost). "
              "main `.`. Got rc " + f.rc + " `" + n.text + "`");
    }
  }

  // ── Review round (attempt 6) ──
  // (a) glm builds `#` cells with every base-level cell omitted, which is
  // Stata's design only when the lower-order terms are in the model (`##`,
  // w2v_margins_corpus F7 `tt##c.d##c.d`, F7nb `tt##dcat`). `i.tt#c.x1` alone
  // estimates 0.tt#c.x1 AND 1.tt#c.x1; serving it with 0.tt#c.x1 omitted was a
  // constrained model at rc 0. Refused by name (r(199), as main refused any `#`)
  // until an oracle arm pins its cells; the spelled-out `##` is served and equal.
  {
    piet::SessionState s(workdir);
    seedDoubleFixture(s);
    (void)mrun(s, "generate byte tt = mod(_n,2)");
    const MReply lone = mrun(s, "glm y i.tt#c.x1");
    const MReply noMain = mrun(s, "glm y i.tt i.tt#c.x1");
    w2check(lone.rc == "199" && lone.estimationBlocks == 0 && noMain.rc == "199" &&
                noMain.estimationBlocks == 0,
            "glm with a `#` term whose lower-order terms are absent is refused r(199), never "
            "fitted with base cells dropped. Got rc " + lone.rc + "/" + noMain.rc + " " + lone.msg);
    const MReply spelled = mrun(s, "glm y i.tt c.x1 i.tt#c.x1");
    const MReply a = mrun(s, "display %21.0g _b[1.tt#c.x1] \"|\" %21.0g _se[1.tt#c.x1]");
    const MReply full = mrun(s, "glm y i.tt##c.x1");
    const MReply b = mrun(s, "display %21.0g _b[1.tt#c.x1] \"|\" %21.0g _se[1.tt#c.x1]");
    w2check(spelled.rc == "0" && full.rc == "0" && !a.text.empty() && a.text == b.text,
            "glm y i.tt c.x1 i.tt#c.x1 is i.tt##c.x1 written out ([U] 11.4.3) and gives the "
            "same 1.tt#c.x1. Got rc " + spelled.rc + "/" + full.rc + " `" + a.text + "` vs `" +
                b.text + "`");
  }
  // (b) A time-series operator column is not one the grid rebuilds, and the
  // grid's replay has no tsset context: `margins, at(d=(0 1))` after
  // `regress y L.x1 d` keeps the landed path (served on main; the grid answered
  // r(198) ts_no_tsset).
  {
    piet::SessionState s(workdir);
    seedDoubleFixture(s);
    (void)mrun(s, "generate double tm = _n");
    (void)mrun(s, "tsset tm");
    const MReply f = mrun(s, "quietly regress y L.x1 d");
    const double bd = ecoef(s, "d", "b");
    const MReply r = mrun(s, "margins, at(d=(0 1))");
    const auto* b = rmat(s, "b");
    w2check(f.rc == "0" && r.rc == "0" && near(rel(b, 0, 1) - rel(b, 0, 0), bd, 1e-9),
            "margins, at(d=(0 1)) after regress y L.x1 d is served, and the two margins differ "
            "by _b[d]. Got rc " + f.rc + "/" + r.rc + " " + mfmt(rel(b, 0, 1) - rel(b, 0, 0)) +
                " vs " + mfmt(bd) + " " + r.msg);

    // (c) A display that fails part-way is evaluated once: the line printed
    // before the failing piece carries the line's own runiform() draw, and the
    // next draw is the one an uncaptured sequence gives (Stata evaluates the
    // line once). The re-evaluating version printed draw 3 and shifted every
    // later draw.
    (void)mrun(s, "set seed 7");
    const MReply d1 = mrun(s, "display %18.0g runiform()");
    const MReply d2 = mrun(s, "display %18.0g runiform()");
    (void)mrun(s, "set seed 7");
    const MReply c = mrun(s, "capture noisily display \"C|\" %18.0g runiform() \"|\" _b[nosuch]");
    const MReply n = mrun(s, "display %18.0g runiform()");
    w2check(!d1.text.empty() && c.text == "C|" + d1.text + "|[nosuch] not found" && n.text == d2.text,
            "a failing display prints its first draw and leaves the stream at draw 2. Got `" +
                c.text + "` then `" + n.text + "` (draws `" + d1.text + "`, `" + d2.text + "`)");
    (void)mrun(s, "set seed 7");
    const MReply e = mrun(s, "capture noisily display \"E|\" %18.0g runiform() \"|\" el(r(nosuch),1,1)");
    const MReply n2 = mrun(s, "display %18.0g runiform()");
    w2check(e.text == "E|" + d1.text + "|type mismatch" && n2.text == d2.text,
            "the same for a piece whose r() matrix is absent (F7_dydx_minmax_b's shape). Got `" +
                e.text + "` then `" + n2.text + "`");
  }

  // ── Round 3: a form leaves main's landed margins path only for a path that
  // serves it (margins-corpus-rediagnosis SPEC.md §1-§2) ──
  // W:P5 pins `margins, dydx(x1 treated)` after `logit ybin c.x1##i.treated x2
  // [pweight=pw] if t>=3, vce(cluster id)` at rc 0 (p5_target_stata.log:165-187,
  // rows :177/:180). The grid router claimed every binary dydx() of an
  // interacted variable and its sample reader refused restricted and
  // perfect-prediction logit/probit fits, r(198). The landed engine AME carries
  // the fit's `if` (BUG-163) and its drops, so those forms stay there; each is
  // checked against its physically restricted twin. The values pinned are
  // bin/main's (main 3deef0ea, the same frame).
  {
    piet::SessionState s(workdir);
    seedW2vFixture(s);
    auto twinEq = [](double a, double b) { return near(a, b, 1e-10); };

    // R3-1 (P5's shape on the w2v frame): restricted pweighted cluster logit.
    (void)mrun(s, "quietly logit remote c.x##i.treatment c1 [pweight=wp] if t>2, vce(cluster id)");
    const MReply r1 = mrun(s, "margins, dydx(x treatment)");
    (void)mrun(s, "preserve");
    (void)mrun(s, "quietly keep if t>2");
    (void)mrun(s, "quietly logit remote c.x##i.treatment c1 [pweight=wp], vce(cluster id)");
    const MReply t1 = mrun(s, "margins, dydx(x treatment)");
    (void)mrun(s, "restore");
    w2check(r1.rc == "0" && t1.rc == "0" && bobs(r1) == 160 &&
                near(bcoef(r1, "x", "b"), .028081584636718844, 1e-10) &&
                near(bcoef(r1, "x", "se"), .010364229391346671, 1e-10) &&
                twinEq(bcoef(r1, "x", "b"), bcoef(t1, "x", "b")) &&
                twinEq(bcoef(r1, "x", "se"), bcoef(t1, "x", "se")) &&
                twinEq(bcoef(r1, "1.treatment", "b"), bcoef(t1, "1.treatment", "b")) &&
                twinEq(bcoef(r1, "1.treatment", "se"), bcoef(t1, "1.treatment", "se")) &&
                near(bcoef(r1, "1.treatment", "b"), .036195657229255634, 1e-10),
            "R3-1 (W:P5 shape): `margins, dydx(x treatment)` after a restricted pweighted "
            "cluster logit is rc 0, N 160, x .028081584636718844 (.010364229391346671), "
            "1.treatment .036195657229255634, equal to its keep-if twin (candidate r(198)). Got rc " +
                r1.rc + " x " + mfmt(bcoef(r1, "x", "b")) + " (" + mfmt(bcoef(r1, "x", "se")) +
                ") twin " + mfmt(bcoef(t1, "x", "b")) + " " + r1.msg);

    // R3-2: restricted probit, robust.
    (void)mrun(s, "quietly probit remote c.x##i.treatment c1 if t>2, vce(robust)");
    const MReply r2 = mrun(s, "margins, dydx(x)");
    (void)mrun(s, "preserve");
    (void)mrun(s, "quietly keep if t>2");
    (void)mrun(s, "quietly probit remote c.x##i.treatment c1, vce(robust)");
    const MReply t2 = mrun(s, "margins, dydx(x)");
    (void)mrun(s, "restore");
    w2check(r2.rc == "0" && near(bcoef(r2, "x", "b"), .031209739887169456, 1e-10) &&
                twinEq(bcoef(r2, "x", "b"), bcoef(t2, "x", "b")) &&
                twinEq(bcoef(r2, "x", "se"), bcoef(t2, "x", "se")),
            "R3-2: `margins, dydx(x)` after a restricted robust probit is rc 0, "
            ".031209739887169456, equal to its keep-if twin (candidate r(198)). Got rc " + r2.rc +
                " " + mfmt(bcoef(r2, "x", "b")) + " twin " + mfmt(bcoef(t2, "x", "b")) + " " + r2.msg);

    // R3-3: post after a restricted logit.
    (void)mrun(s, "quietly logit remote c.x##i.treatment c1 if t>2");
    const MReply r3 = mrun(s, "margins, dydx(x) post");
    const std::string ecmd3 = s.eval.eStrings.get("cmd", "");
    w2check(r3.rc == "0" && ecmd3 == "margins" && near(ecoef(s, "x", "b"), .031182184330598262, 1e-10),
            "R3-3: `margins, dydx(x) post` after a restricted logit is rc 0, e(cmd) margins, "
            "_b[x] .031182184330598262 (candidate r(198), e(cmd) logit). Got rc " + r3.rc + " `" +
                ecmd3 + "` " + mfmt(ecoef(s, "x", "b")) + " " + r3.msg);

    // K-1 (control): the unrestricted fit, served on both paths.
    (void)mrun(s, "quietly logit remote c.x##i.treatment c1");
    const MReply k1 = mrun(s, "margins, dydx(x)");
    w2check(k1.rc == "0" && bobs(k1) == 240 && near(bcoef(k1, "x", "b"), .020331815990033662, 1e-10) &&
                near(bcoef(k1, "x", "se"), .00979121695301716, 1e-9),
            "K-1: `margins, dydx(x)` after the unrestricted interacted logit, .020331815990033662 "
            "(.00979121695301716), N 240. Got rc " + k1.rc + " " + mfmt(bcoef(k1, "x", "b")) + " (" +
                mfmt(bcoef(k1, "x", "se")) + ") " + k1.msg);
    // K-2 (control): dydx() with at() stays on the grid (r(b) one column per point).
    const MReply k2 = mrun(s, "margins, dydx(x) at(c1=(1 3))");
    const auto* bk2 = rmat(s, "b");
    w2check(k2.rc == "0" && bk2 != nullptr && bk2->colnames.size() == 2 &&
                bk2->colnames[1].find("2._at") != std::string::npos,
            "K-2: `margins, dydx(x) at(c1=(1 3))` after the interacted logit stays on the grid: "
            "rc 0, r(b) 1x2 (.._at). Got rc " + k2.rc + " " + k2.msg);

    // R3-5 / R3-6: an fweighted logit (BUG-893). The landed engine path takes
    // --wtype pw only and answered rc 1; the grid serves dydx() and equals the
    // expanded-rows fit; dydx(*) is a named refusal.
    (void)mrun(s, "quietly logit remote x c1 i.g [fweight=fw], vce(cluster region)");
    const MReply r5 = mrun(s, "margins, dydx(x)");
    const double r5b = rel(rmat(s, "b"), 0, 0), r5se = std::sqrt(rel(rmat(s, "V"), 0, 0));
    const double r5n = s.eval.rValues.contains("N") ? *s.eval.rValues.find("N") : std::nan("");
    // R3-6: dydx(*) is every covariate in e(b) order (w2v_margins_siblings.log
    // :1907 F_fw_all_cols `x c1 1b.g 2.g 3.g`, F_fw_all_b first column
    // .0198856760759958).
    {
      const MReply r6 = mrun(s, "margins, dydx(*)");
      const auto* b6 = rmat(s, "b");
      w2check(r6.rc == "0" && b6 != nullptr && b6->colnames.size() == 5 &&
                  b6->colnames[0] == "x" && near(rel(b6, 0, 0), .0198856760759958, 1e-9),
              "R3-6 dydx(*) after an fweighted logit: rc 0, 5 columns from x "
              "(.0198856760759958, F_fw_all). Got rc " + r6.rc + " " + r6.msg);
    }
    (void)mrun(s, "preserve");
    (void)mrun(s, "generate long oid = _n");
    (void)mrun(s, "quietly expand 3");
    (void)mrun(s, "sort oid");
    (void)mrun(s, "by oid: generate k = _n");
    (void)mrun(s, "quietly keep if k <= fw");
    (void)mrun(s, "quietly logit remote x c1 i.g, vce(cluster region)");
    const MReply t5 = mrun(s, "margins, dydx(x)");
    (void)mrun(s, "restore");
    w2check(r5.rc == "0" && r5n == 480 && twinEq(r5b, bcoef(t5, "x", "b")) &&
                twinEq(r5se, bcoef(t5, "x", "se")) && bobs(t5) == 480,
            "R3-5: `margins, dydx(x)` after `logit ... [fweight=fw], vce(cluster region)` is rc 0 "
            "(never rc 1), r(N) 480, equal to the expanded-rows fit. Got rc " + r5.rc + " " +
                mfmt(r5b) + " (" + mfmt(r5se) + ") N " + mfmt(r5n) + " twin " +
                mfmt(bcoef(t5, "x", "b")) + " (" + mfmt(bcoef(t5, "x", "se")) + ") " + r5.msg);

    // K-3 (control): the F6 marginlist after the fweighted logit stays served
    // (w2v_margins_corpus.log:2693 F6_year_asobs_b).
    (void)mrun(s, "quietly logit remote i.year c1 i.g [fweight=fw], vce(cl region)");
    const MReply k3 = mrun(s, "margins year");
    const auto* bk3 = rmat(s, "b");
    w2check(k3.rc == "0" && near(rel(bk3, 0, 0), .4324253468071036, 1e-9) &&
                near(rel(bk3, 0, 1), .4932886124381229, 1e-9) &&
                near(rel(bk3, 0, 2), .6590459105587019, 1e-9) &&
                near(rel(bk3, 0, 3), .5316689953583911, 1e-9),
            "K-3: `margins year` after the F6 fit equals F6_year_asobs_b (log:2693). Got rc " +
                k3.rc + " " + mfmt(rel(bk3, 0, 0)) + " " + k3.msg);

    // FU-633: the fit now recovers _cons and its full covariance after
    // absorbed factor columns are omitted. Predictive margins use that fit,
    // including the intercept/slope cross-covariances (siblings F_absorb_int).
    const char* kAbsorbedFits[] = {
        "quietly areg y28 treatment##i.blockcohort c1, absorb(blockcohort) robust",
        "quietly reghdfe y28 treatment##i.blockcohort c1, absorb(blockcohort) vce(robust)",
        "quietly xtreg y28 treatment##i.blockcohort c1, fe",
    };
    for (const char* f : kAbsorbedFits) {
      const bool xt = std::string(f).find("xtreg") != std::string::npos;
      if (xt) (void)mrun(s, "xtset blockcohort");
      const MReply fr = mrun(s, f);
      w2check(fr.rc == "0", std::string("R3-7 fit `") + f + "` rc 0. Got rc " + fr.rc + " " + fr.msg);
      const MReply at = mrun(s, "margins, at(c1=1)");
      const double atb = rel(rmat(s, "b"), 0, 0), atv = rel(rmat(s, "V"), 0, 0);
      const bool areg = std::string(f).find("areg ") != std::string::npos;
      w2check(at.rc == "0" && near(atb, 3.325886028302597, 1e-9) && atv > 0 &&
                  (!areg || near(atv, .0071912456611016, 1e-9)),
              std::string("F_absorb_int_at: recovered intercept/covariance after `") + f +
                  "`, b 3.325886028302597, areg V .0071912456611016. Got rc " + at.rc +
                  " b=" + mfmt(atb) + " V=" + mfmt(atv) + " " + at.msg);
      const MReply means = mrun(s, "margins, atmeans");
      const double mb = rel(rmat(s, "b"), 0, 0), mv = rel(rmat(s, "V"), 0, 0);
      w2check(means.rc == "0" && near(mb, 3.720998147756231, 1e-9) && mv > 0 &&
                  (!areg || near(mv, .0046530178789928, 1e-9)),
              std::string("F_absorb_int_atmeans: recovered intercept/covariance after `") + f +
                  "`, b 3.720998147756231, areg V .0046530178789928. Got rc " + means.rc +
                  " b=" + mfmt(mb) + " V=" + mfmt(mv) + " " + means.msg);
      if (xt) (void)mrun(s, "xtset id t");
    }
    // K-5 (control): dydx(treatment) after the F5 areg fit is served.
    (void)mrun(s, "quietly areg y28 treatment##i.blockcohort c1, absorb(blockcohort) robust");
    const MReply k5 = mrun(s, "margins, dydx(treatment)");
    w2check(k5.rc == "0" && near(rel(rmat(s, "b"), 0, 1), 1.681239018387203, 1e-9),
            "K-5: `margins, dydx(treatment)` after the F5 areg fit is served, 1.treatment "
            "1.681239018387203 (F5_dydx_b, log:2413). Got rc " + k5.rc + " " +
                mfmt(rel(rmat(s, "b"), 0, 1)) + " " + k5.msg);
    // K-4 (control): an absorbed fit WITH _cons keeps its grid answer
    // (F3a_at_atmeans_b, log:1951).
    (void)mrun(s, "quietly areg yf female passed fpassed c1, absorb(t) vce(cluster id)");
    const MReply k4 = mrun(s, "margins, at(female=0) atmeans");
    w2check(k4.rc == "0" && near(rel(rmat(s, "b"), 0, 0), 4.200680741753562, 1e-9),
            "K-4: `margins, at(female=0) atmeans` after areg with _cons is served, "
            "4.200680741753562 (F3a_at_atmeans_b, log:1951). Got rc " + k4.rc + " " +
                mfmt(rel(rmat(s, "b"), 0, 0)) + " " + k4.msg);
  }
  // FU-668: an atmeans mean is evaluated at float precision for byte, int and
  // double variables too (w2v_f7c_jacobian.log:775-794: after `regress y x c1
  // bvar ivar dvar`, `margins, at(x=(1 2)) atmeans` r(b) = 7.024710751140063 /
  // 7.703783727515455, hex +1.c194dc6700650X+002 / +1.ed0acae748b1dX+002, only
  // with float(mean) for all four). The grid rounded only float variables and
  // missed by 1.85e-8.
  {
    piet::SessionState s(workdir);
    seedW2vFixture(s);
    (void)mrun(s, "generate byte bvar = (mod(_n,3)==0)");
    (void)mrun(s, "generate int ivar = mod(_n*7,13)");
    (void)mrun(s, "generate double dvar = _n/7 + 0.1");
    (void)mrun(s, "quietly regress y x c1 bvar ivar dvar");
    const MReply r = mrun(s, "margins, at(x=(1 2)) atmeans");
    const auto* b = rmat(s, "b");
    w2check(r.rc == "0" && near(rel(b, 0, 0), 7.024710751140063, 1e-11) &&
                near(rel(b, 0, 1), 7.703783727515455, 1e-11),
            "FU-668: `margins, at(x=(1 2)) atmeans` after regress with byte/int/double covariates "
            "holds every mean at float precision: 7.024710751140063 / 7.703783727515455 "
            "(w2v_f7c_jacobian FU642_b_r1). Got rc " + r.rc + " " + mfmt(rel(b, 0, 0)) + " / " +
                mfmt(rel(b, 0, 1)) + " " + r.msg);
    // FU-668, explicit at(): a byte, int or double variable is evaluated at
    // float(value) too. at(ivar=(2.1 3.3)) on the int ivar: r(V)[1,1]
    // .0257184016424877 at float(2.1); the double 2.1 gives .0257184009844379
    // (w2v_margins_siblings.log:3850/3855 FU668_at_ivar_b/_v; :3802 and :3908
    // the same for the double dvar and the byte bvar).
    const MReply ri = mrun(s, "margins, at(ivar=(2.1 3.3))");
    const auto* vi = rmat(s, "V");
    w2check(ri.rc == "0" && near(rel(rmat(s, "b"), 0, 0), 9.841313341847826, 1e-11) &&
                near(rel(vi, 0, 0), .0257184016424877, 1e-11) &&
                near(rel(vi, 1, 1), .0187203130279343, 1e-11) &&
                near(rel(vi, 1, 0), .0215783110384359, 1e-11),
            "FU-668: `margins, at(ivar=(2.1 3.3))` on an int variable is evaluated at float(2.1) / "
            "float(3.3): r(V) .0257184016424877 .0187203130279343 .0215783110384359 "
            "(FU668_at_ivar_v). Got rc " + ri.rc + " " + mfmt(rel(vi, 0, 0)) + " " +
                mfmt(rel(vi, 1, 1)) + " " + mfmt(rel(vi, 1, 0)) + " " + ri.msg);
    (void)mrun(s, "margins, at(dvar=(0.1 0.3))");
    const double vd = rel(rmat(s, "V"), 1, 1);
    (void)mrun(s, "margins, at(bvar=(0.1 0.7))");
    const double vb = rel(rmat(s, "V"), 1, 1);
    w2check(near(vd, .0488565895344635, 1e-11) && near(vb, .0198274744955, 1e-11),
            "FU-668: at(dvar=(0.1 0.3)) on a double and at(bvar=(0.1 0.7)) on a byte variable: "
            "r(V)[2,2] .0488565895344635 / .0198274744955 (FU668_at_dvar_v, FU668_at_bvar_v). Got " +
                mfmt(vd) + " / " + mfmt(vb));
    // The landed single-point at() path (the engine's refit, no grid) takes the
    // same rule: `margins, at(dvar=0.1) atmeans` is 4.921007387529768 at
    // float(0.1), 4.921007387096345 at the double 0.1 (FU668_at_dvar_atmeans_b).
    const MReply ra = mrun(s, "margins, at(dvar=0.1) atmeans");
    w2check(ra.rc == "0" && near(rel(rmat(s, "b"), 0, 0), 4.921007387529768, 1e-11),
            "FU-668: `margins, at(dvar=0.1) atmeans` on the landed path is evaluated at float(0.1): "
            "4.921007387529768 (FU668_at_dvar_atmeans_b). Got rc " + ra.rc + " " +
                mfmt(rel(rmat(s, "b"), 0, 0)) + " " + ra.msg);
  }
  // w2v_margins_siblings C2/C7: dydx() columns follow the order the variables
  // enter e(b), not the dydx() list. `dydx(treatment x)` after `regress y
  // c.x##i.treatment c1` is `x 0b.treatment 1.treatment`, x .6736641119513347
  // (log:561/566); `dydx(c1 x)` after the fweighted logit is `x c1`,
  // .0198856760759958 .0114139703600975 (log:1798/1803).
  {
    piet::SessionState s(workdir);
    seedW2vFixture(s);
    (void)mrun(s, "quietly regress y c.x##i.treatment c1");
    const MReply r2 = mrun(s, "margins, dydx(treatment x)");
    const auto* b2 = rmat(s, "b");
    const std::string c2 = b2 == nullptr ? std::string("<none>")
                                         : (b2->colnames.size() == 3 ? b2->colnames[0] + " " + b2->colnames[1] +
                                                                           " " + b2->colnames[2]
                                                                     : std::string("<k!=3>"));
    w2check(r2.rc == "0" && c2 == "x 0b.treatment 1.treatment" && near(rel(b2, 0, 0), .6736641119513347, 1e-9),
            "C2: `margins, dydx(treatment x)` posts `x 0b.treatment 1.treatment`, x first "
            ".6736641119513347 (w2v_margins_siblings C2_dydx_tr_x_cols/_b). Got rc " + r2.rc + " `" + c2 +
                "` " + mfmt(rel(b2, 0, 0)) + " " + r2.msg);
    (void)mrun(s, "quietly logit remote x c1 i.g [fweight=fw], vce(cluster region)");
    const MReply r7 = mrun(s, "margins, dydx(c1 x)");
    const auto* b7 = rmat(s, "b");
    w2check(r7.rc == "0" && b7 != nullptr && b7->colnames.size() == 2 && b7->colnames[0] == "x" &&
                near(rel(b7, 0, 0), .0198856760759958, 1e-9) && near(rel(b7, 0, 1), .0114139703600975, 1e-9),
            "C7: `margins, dydx(c1 x)` after the fweighted logit posts `x c1` .0198856760759958 "
            ".0114139703600975 (C7_dydx_c1x_cols/_b). Got rc " + r7.rc + " " + mfmt(rel(b7, 0, 0)) + " " +
                r7.msg);
    // The same rule on the landed engine AME (an unweighted logit takes it, not
    // the grid): `dydx(c1 x)` is `x c1`, its first column x's own effect.
    (void)mrun(s, "quietly logit remote x c1 i.g");
    (void)mrun(s, "margins, dydx(x)");
    const double xOnly = rel(rmat(s, "b"), 0, 0);
    const MReply rl = mrun(s, "margins, dydx(c1 x)");
    const auto* bl = rmat(s, "b");
    w2check(rl.rc == "0" && bl != nullptr && bl->colnames.size() == 2 && bl->colnames[0] == "x" &&
                bl->colnames[1] == "c1" && near(rel(bl, 0, 0), xOnly, 1e-12),
            "C7 rule on the landed logit AME: `margins, dydx(c1 x)` posts `x c1`, r(b)[1,1] = dydx(x)'s " +
                mfmt(xOnly) + ". Got rc " + rl.rc + " " + mfmt(rel(bl, 0, 0)) + " " + rl.msg);
  }
  // w2v_margins_siblings C8: after a glm without an interaction, at(),
  // atmeans and a marginlist are served by the margins grid (ME05; the grid's
  // analytic Jacobian agrees with Stata's numerical one to ~1e-9 relative), and
  // `margins, dydx()` posts r() from its own path (C8_gamif_dydx_x N=160,
  // .1303959895000301, V .1455738400200312, se .3815414001390036;
  // log:2434-2460), where r(b) was unset.
  {
    piet::SessionState s(workdir);
    seedW2vFixture(s);
    (void)mrun(s, "quietly glm yp x i.g c.d, link(log) family(gamma)");
    const MReply ra = mrun(s, "margins, at(d=(-0.2 0.2))");
    w2check(ra.rc == "0" && near(rel(rmat(s, "b"), 0, 0), 2.218103725697229, 1e-9) &&
                near(rel(rmat(s, "b"), 0, 1), 2.632896568647277, 1e-9),
            "C8 at() after a glm without an interaction: r(b) 2.218103725697229 2.632896568647277 "
            "(C8_gam_at_d_b). Got rc " + ra.rc + " " + mfmt(rel(rmat(s, "b"), 0, 0)) + " " + ra.msg);
    const MReply rm = mrun(s, "margins, atmeans");
    w2check(rm.rc == "0" && near(rel(rmat(s, "b"), 0, 0), 2.382939267883001, 1e-9),
            "C8 atmeans after a glm without an interaction: r(b) 2.382939267883001 "
            "(C8_gam_atmeans_b). Got rc " + rm.rc + " " + mfmt(rel(rmat(s, "b"), 0, 0)) + " " + rm.msg);
    const MReply rg = mrun(s, "margins g");
    w2check(rg.rc == "0" && near(rel(rmat(s, "b"), 0, 0), 2.525391298626589, 1e-9) &&
                near(rel(rmat(s, "b"), 0, 2), 2.433619571829875, 1e-9),
            "C8 marginlist after a glm without an interaction: r(b) 2.525391298626589 ... "
            "2.433619571829875 (C8_gam_g_b, log:2153). Got rc " + rg.rc + " " +
                mfmt(rel(rmat(s, "b"), 0, 0)) + " " + rg.msg);
    // Control: dydx() at at() points on a glm without an interaction stays on the
    // grid (w2v_margins_corpus F7c_dydx_at_b, 1.tt:1bn._at .6356563180601498).
    (void)mrun(s, "quietly glm yp x i.g i.tt c.d, link(log) family(gamma) vce(cl clinic)");
    const MReply f7c = mrun(s, "margins, dydx(tt) at(d=(-0.4(.2).4))");
    w2check(f7c.rc == "0" && near(rel(rmat(s, "b"), 0, 5), .6356563180601498, 1e-9),
            "C8 control: `margins, dydx(tt) at(d=(-0.4(.2).4))` after the glm without an interaction "
            "is served, 1.tt:1bn._at .6356563180601498 (F7c_dydx_at_b). Got rc " + f7c.rc + " " +
                mfmt(rel(rmat(s, "b"), 0, 5)) + " " + f7c.msg);
    (void)mrun(s, "quietly glm yp x i.g c.d if t>2, link(log) family(gamma)");
    const MReply r = mrun(s, "margins, dydx(x)");
    const auto* b = rmat(s, "b");
    const auto* t = rmat(s, "table");
    w2check(r.rc == "0" && near(s.eval.rValues.find("N") ? *s.eval.rValues.find("N") : -1, 160, 0) &&
                b != nullptr && b->colnames.size() == 1 && b->colnames[0] == "x" &&
                near(rel(b, 0, 0), .1303959895000301, 1e-9) &&
                near(rel(t, 1, 0), .3815414001390036, 1e-9) &&
                // r(V) is the square of that se; it misses Stata's (numerical
                // Jacobian) by 1.03e-9, the F7c class (FU-643/FU-678).
                near(rel(rmat(s, "V"), 0, 0), .1455738400200312, 2e-9),
            "C8: `margins, dydx(x)` after `glm ... if t>2` posts r(N) 160, r(b) x .1303959895000301, "
            "r(V) .1455738400200312, r(table) se .3815414001390036 (C8_gamif_dydx_x). Got rc " + r.rc +
                " " + mfmt(rel(b, 0, 0)) + " " + r.msg);
  }
  // R3-4: a perfect predictor the fit drops (e(N) 234 of 240); the landed AME
  // equals the fit on the remaining rows (candidate r(198), complete cases 240
  // != e(N)).
  {
    piet::SessionState s(workdir);
    seedW2vFixture(s);
    (void)mrun(s, "generate byte pp = (mod(_n,40)==0)");
    (void)mrun(s, "quietly replace remote = 1 if pp");
    (void)mrun(s, "quietly logit remote c.x##i.treatment c1 i.pp");
    const MReply nr = mrun(s, "display e(N)");
    const MReply r4 = mrun(s, "margins, dydx(x)");
    (void)mrun(s, "quietly logit remote c.x##i.treatment c1 if pp==0");
    const MReply t4 = mrun(s, "margins, dydx(x)");
    w2check(nr.text == "234" && r4.rc == "0" && t4.rc == "0" &&
                near(bcoef(r4, "x", "b"), bcoef(t4, "x", "b"), 1e-10) &&
                near(bcoef(r4, "x", "se"), bcoef(t4, "x", "se"), 1e-10),
            "R3-4: `margins, dydx(x)` after a logit that drops a perfect predictor is rc 0 "
            "(e(N) 234) and equals its `if pp==0` twin. Got e(N) `" + nr.text + "` rc " + r4.rc +
                " " + mfmt(bcoef(r4, "x", "b")) + " twin " + mfmt(bcoef(t4, "x", "b")) + " " + r4.msg);
  }

  // ── ROUND 4 (R4-a, R4-b): the landed GLM at() paths ──────────────────
  // Oracle: w2v_margins_siblings (job-20261014T122544-c01b4c). Each arm reads
  // r(b)/r(V) and the column names Stata posts.
  {
    piet::SessionState s(workdir);
    seedW2vFixture(s);
    auto atArm = [&](const std::string& label, const std::string& cmd, const std::vector<std::string>& cols,
                     const std::vector<double>& wantB, const std::vector<double>& wantVdiag, double wantV21,
                     double tol) {
      const MReply r = mrun(s, cmd);
      const auto* b = rmat(s, "b");
      const auto* V = rmat(s, "V");
      bool ok = r.rc == "0" && b != nullptr && V != nullptr && b->colnames == cols &&
                b->data.size() == 1 && b->data[0].size() == wantB.size();
      std::string got;
      for (size_t j = 0; ok && j < wantB.size(); ++j) {
        ok = near(rel(b, 0, j), wantB[j], tol) && near(rel(V, j, j), wantVdiag[j], tol);
        got += " " + mfmt(rel(b, 0, j)) + " (" + mfmt(rel(V, j, j)) + ")";
      }
      ok = ok && near(rel(V, 1, 0), wantV21, tol);
      std::string names;
      if (b != nullptr)
        for (const auto& c : b->colnames) names += c + " ";
      w2check(ok, label + ": `" + cmd + "` is rc 0 with r(b) " + std::to_string(wantB.size()) +
                      " columns and r(V) across points as Stata. Got rc " + r.rc + " [" + names + "]" +
                      got + " " + r.msg);
    };
    // R4-1 (siblings log:2961-3012 F_range_numlist_lg): main answered `c1=1(1` as
    // ONE point, rc 0 (parseAtNumlistM4 closed the list at its first `)`).
    (void)mrun(s, "quietly logit remote x c1 i.g");
    atArm("R4-1 range numlist after logit", "margins, at(c1=(1(1)3))", {"1bn._at", "2._at", "3._at"},
          {.5150054576080881, .5252824426477876, .5355385574607817},
          {.0019612422125101, .0010876205976622, .001204043471281}, .0012759229767275, 1e-11);
    // R4-2 (log:3018-3069 F_range_slash_lg): `(1/3)` is the same three points.
    atArm("R4-2 a/b numlist after logit", "margins, at(c1=(1/3))", {"1bn._at", "2._at", "3._at"},
          {.5150054576080881, .5252824426477876, .5355385574607817},
          {.0019612422125101, .0010876205976622, .001204043471281}, .0012759229767275, 1e-11);
    // R4-3 (log:3102-3153 F_range_numlist_po). T2: Stata's poisson stops one
    // Newton step short of the MLE PIET's fit reaches (b 5e-10 apart).
    (void)mrun(s, "quietly poisson yc x c1");
    atArm("R4-3 range numlist after poisson", "margins, at(c1=(1(1)3))", {"1bn._at", "2._at", "3._at"},
          {4.555787169183967, 4.524426249076032, 4.493281209840819},
          {.0370741431949145, .0202160922851704, .0222396277346574}, .023884627841906, 1e-9);
    // R4-4 (log:2787-2837 F_robust_dydx_at_lg): after vce(robust) the se is the
    // robust one; main answered the OIM se at rc 0 and posted no r(b).
    (void)mrun(s, "quietly logit remote x c1 i.g, vce(robust)");
    atArm("R4-4 robust dydx at() after logit", "margins, dydx(x) at(c1=(1 3))", {"x:1bn._at", "x:2._at"},
          {.0205335288288549, .0204502148122379}, {.0000971919895369, .0000964935260369},
          .0000968250808032, 1e-10);
    // R4-5 (log:2871-2921 F_robust_dydx_at_po): the same after poisson, through
    // the engine's at() branch under the fit's --vce (main: OIM se, no r(b)).
    (void)mrun(s, "quietly poisson yc x c1, vce(robust)");
    atArm("R4-5 robust dydx at() after poisson", "margins, dydx(x) at(c1=(1 3))", {"x:1bn._at", "x:2._at"},
          {.4892991753213422, .4825859305573755}, {.0019353441036849, .0016832476527241},
          .0016499830905486, 1e-9);
    // R4-6: the OIM poisson dydx() at() has the same effects (the VCE changes the
    // se only) and now posts them in r(b) (main: no r(b) for this shape).
    (void)mrun(s, "quietly poisson yc x c1");
    const MReply oim = mrun(s, "margins, dydx(x) at(c1=(1 3))");
    const auto* bo = rmat(s, "b");
    w2check(oim.rc == "0" && bo != nullptr && near(rel(bo, 0, 0), .4892991753213422, 1e-9) &&
                near(rel(bo, 0, 1), .4825859305573755, 1e-9),
            "R4-6: OIM poisson `margins, dydx(x) at(c1=(1 3))` posts the same effects in r(b). Got rc " +
                oim.rc + " " + oim.msg);
    // R4-7/R4-8 (w2v_margins_vce.log:2691-2827 V9): a restricted robust logit.
    // The grid cannot read its sample; the landed path refits on the fit's `if`
    // under its --vce (main: the OIM V at rc 0, .0028792394334065 for V[1,1]).
    (void)mrun(s, "quietly logit remote x c1 i.g if t>2, vce(robust)");
    atArm("R4-7 restricted robust at() after logit", "margins, at(c1=(1 3))", {"1bn._at", "2._at"},
          {.5207238958036464, .5359998338660664}, {.0027952970657046, .0017850712410974},
          .0008797393910197, 1e-10);
    atArm("R4-8 restricted robust dydx at() after logit", "margins, dydx(x) at(c1=(1 3))",
          {"x:1bn._at", "x:2._at"}, {.0311766149457442, .0310734429373849},
          {.0001316479367471, .0001308696864462}, .0001311957548245, 1e-10);
    // R4-9 (w2v_margins_vce.log:2343-2380 V7): after `xtpoisson, fe` margins
    // answers from the fit's own e(b)/e(V) -- the linear index without constant or
    // fixed effect (ME06): dydx(x) is _b[x] .1088590169532439 (never the pooled
    // refit's .4846500614524408).
    (void)mrun(s, "quietly xtpoisson yc x c1, fe");
    const MReply xp = mrun(s, "margins, dydx(x)");
    w2check(xp.rc == "0" && near(rel(rmat(s, "b"), 0, 0), .1088590169532439, 1e-8),
            "R4-9: `margins, dydx(x)` after `xtpoisson, fe` is _b[x] .1088590169532439 "
            "(V7_xtp_dydx_x_b). Got rc " + xp.rc + " " + mfmt(rel(rmat(s, "b"), 0, 0)) + " " + xp.msg);
  }

  // ── ROUND 5 (R5-a): atmeans and a factor marginlist after poisson ─────
  // Oracle: tests/eval/golden/aer_w2/w2v_margins_vce.log (aer_w2 v13). Both forms were
  // r(198) on main; the grid answers them on the response scale exp(xb) from the
  // fit's own robust or cluster e(V). Tolerance 1e-9 relative: Stata's poisson
  // e(b) stops one Newton step short of the MLE PIET reaches (POISSON-CONV-RULING
  // v2), which moves these records by up to 8e-10. The V6 marginlist's
  // off-diagonal moves 1.48e-9 by that offset alone (formula at Stata's own
  // e(b)/e(V) 2.8e-15 from Stata's record, at the exact MLE 2.0e-16 from PIET's;
  // internal note margins-corpus/R5-V6G-OFFSET.md), so that
  // arm reads it at 1e-8.
  {
    piet::SessionState s(workdir);
    seedW2vFixture(s);
    auto gridArm = [&](const std::string& label, const std::string& cmd, const std::vector<std::string>& cols,
                       const std::vector<double>& wantB, const std::vector<double>& wantVdiag,
                       std::optional<double> wantV21, double v21tol) {
      const MReply r = mrun(s, cmd);
      const auto* b = rmat(s, "b");
      const auto* V = rmat(s, "V");
      bool ok = r.rc == "0" && b != nullptr && V != nullptr && b->colnames == cols &&
                b->data.size() == 1 && b->data[0].size() == wantB.size();
      std::string got;
      for (size_t j = 0; ok && j < wantB.size(); ++j) {
        ok = near(rel(b, 0, j), wantB[j], 1e-9) && near(rel(V, j, j), wantVdiag[j], 1e-9);
        got += " " + mfmt(rel(b, 0, j)) + " (" + mfmt(rel(V, j, j)) + ")";
      }
      if (ok && wantV21.has_value()) ok = near(rel(V, 1, 0), *wantV21, v21tol);
      const MReply n = mrun(s, "display r(N)");
      ok = ok && n.text == "240";
      std::string names;
      if (b != nullptr)
        for (const auto& c : b->colnames) names += c + " ";
      w2check(ok, label + ": `" + cmd + "` is rc 0 with r(N) 240 and r(b)/r(V) as Stata. Got rc " + r.rc +
                      " r(N) `" + n.text + "` [" + names + "]" + got + " " + r.msg);
    };
    // R5-1/R5-2 (w2v_margins_vce.log:1614 fit; :1854-1905 V5_po_atmeans, :1908-1958 V5_po_g).
    (void)mrun(s, "quietly poisson yc x c1 i.g, vce(robust)");
    gridArm("R5-1 atmeans after robust poisson", "margins, atmeans", {"_cons"}, {4.25955644964916},
            {.0112179737469909}, std::nullopt, 0);
    gridArm("R5-2 marginlist after robust poisson", "margins g", {"1bn.g", "2.g", "3.g"},
            {4.627788621086942, 4.618701257993219, 4.280836611553611},
            {.0455040708022075, .0432428969807631, .0333774267797176}, -.000169393152804, 1e-9);
    // R5-3/R5-4 (log:1964 fit; :2205-2256 V6_po_atmeans, :2259-2309 V6_po_g).
    (void)mrun(s, "quietly poisson yc x c1 i.g, vce(cluster region)");
    gridArm("R5-3 atmeans after cluster poisson", "margins, atmeans", {"_cons"}, {4.25955644964916},
            {.019432300259038}, std::nullopt, 0);
    gridArm("R5-4 marginlist after cluster poisson", "margins g", {"1bn.g", "2.g", "3.g"},
            {4.627788621086942, 4.618701257993219, 4.280836611553611},
            {.0367032568559744, .0243007977358412, .0313636240228916}, .0173319292605411, 1e-8);
    // R5-5 (class): after an OIM poisson the same forms read the OIM e(V). The
    // margins are the robust fit's (the VCE moves the se only), and the atmeans
    // se is the delta method at the means on the OIM e(V): it differs from the
    // robust one, so a served OIM se after vce(robust) cannot pass R5-1.
    (void)mrun(s, "quietly poisson yc x c1 i.g");
    const MReply om = mrun(s, "margins, atmeans");
    const auto* bm = rmat(s, "b");
    const auto* vm = rmat(s, "V");
    w2check(om.rc == "0" && bm != nullptr && vm != nullptr && near(rel(bm, 0, 0), 4.25955644964916, 1e-9) &&
                std::isfinite(rel(vm, 0, 0)) && !near(rel(vm, 0, 0), .0112179737469909, 1e-3),
            "R5-5: `margins, atmeans` after an OIM poisson is rc 0 at 4.25955644964916 with the OIM se. "
            "Got rc " + om.rc + " " + (bm ? mfmt(rel(bm, 0, 0)) : std::string("no r(b)")) + " " + om.msg);
    // R5-6: the combinations no record pins keep their named r(198).
    const MReply oc = mrun(s, "margins g, atmeans");
    w2check(oc.rc == "198", "R5-6: `margins g, atmeans` after poisson stays refused r(198). Got rc " + oc.rc +
                                " " + oc.msg);
  }

  fs::remove_all(workdir, ec);
}
