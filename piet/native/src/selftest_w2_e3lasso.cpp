// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// E3 arms for `lasso` / `rlasso`.
//   * Built-in linear lasso implements plugin, BIC and an unselected grid.
//     CV (linear and Poisson) and adaptive CV draw their folds from the
//     session's Stata mt64 stream, Stata's partition (one runiform() per
//     sample row, ranked, consecutive blocks); oracle w2j_e3_builtin.log
//     L_cv_s1/L_cv_s2/L_cv_if/L_pois_adapt.
//   * `rlasso` left E3 (handlers_e3_lasso.cpp runRlasso). Its arms are the
//     oracle's: tests/eval/golden/aer_w2/w2j_e3_rlasso.log B1_plain (:203-212,
//     the selected set and e(b)[1,1] for x1) and the refusals R6 (`rlasso y`,
//     r(102)), R7 (a string regressor, r(109)), R8 (cluster(nosuchvar), r(111))
//     and R9 (an empty sample, r(2000)).
#include "selftest_w2.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <string>
#include <vector>

#include "piet/dispatcher.h"
#include "piet/datastore.h"
#include "piet/eval/errors.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

namespace {

std::string lastRc(piet::SessionState& s, const std::string& line, std::string* reason) {
  std::string rc = "0";
  try {
    for (const auto& b : piet::dispatch(line, s)) {
      if (b.is_object() && b.value("type", std::string()) == "error") {
        rc = piet::eval::coarseRc(b);
        if (reason != nullptr) *reason = b.value("reason", std::string());
      }
    }
  } catch (const std::exception& e) {
    rc = std::string("EXC ") + e.what();
  }
  return rc;
}

// w2j_e3_rlasso.do:31-57, the twelve regressors the value arms use.
void rlassoData(piet::SessionState& s) {
  static const int kPrimes[] = {11, 13, 17, 19, 23, 29, 31, 37, 41, 43, 47, 53};
  for (const char* c : {"clear", "set obs 240", "generate long id = ceil(_n/8)",
                        "generate byte t = mod(_n-1,8) + 1",
                        "generate str4 sid = \"g\" + string(id,\"%03.0f\")",
                        "generate double a = (mod(id*7,11)-5)/2",
                        "generate double e = (mod(_n*37,23)-11)/6"})
    (void)piet::dispatch(c, s);
  for (int j = 1; j <= 12; ++j) {
    const int P = kPrimes[j - 1];
    (void)piet::dispatch("generate double x" + std::to_string(j) + " = (mod(_n*(" +
                             std::to_string(j) + "+2)," + std::to_string(P) + ") - (" +
                             std::to_string(P) + "-1)/2)/" + std::to_string(P) + "*3.5",
                         s);
  }
  for (const char* c :
       {"replace x4 = x4 + 0.5*a", "replace x8 = x8 + 0.5*a", "replace x9 = (mod(id*3,11)-5)/3",
        "replace x10 = (mod(id*5,13)-6)/3.5", "replace x11 = (mod(id*2,7)-3)/2",
        "replace x12 = (mod(id*9,17)-8)/4.5",
        "generate double y = 1 + 2*x1 - 1.5*x2 + x3 + 1.0*a + e*(1 + (mod(id,3)==0)) + "
        "0.25*mod(t*3,5) + 0.4*x10 + 0.4*x11"})
    (void)piet::dispatch(c, s);
}

// w2j_e3_builtin.do:23-39: no random draws in the estimation fixture.
void builtinData(piet::SessionState& s) {
  for (const char* command : {"clear", "set obs 240", "generate id = ceil(_n/8)",
       "generate t = mod(_n-1,8)+1", "generate g = mod(_n,3)+1",
       "generate double x1 = mod(_n*7,11)/3 + id/10",
       "generate double x2 = mod(_n*13,17)/4 - t/3",
       "generate double x3 = mod(_n*5,7)/2", "generate double x4 = mod(_n*11,19)/5 - 1",
       "generate double x5 = mod(_n*17,23)/6", "generate double x6 = mod(_n*3,13)/4 - x1/5",
       "generate double x7 = mod(_n*19,29)/7", "generate double x8 = mod(_n*23,31)/8 - 2",
       "generate double u = (mod(_n*37,23)-11)/6",
       "generate double y = 1 + 2*x1 - x2 + 0.5*x3 + 0.3*g + u*(1+x1/4)"})
    (void)piet::dispatch(command, s);
}

bool scalarIs(const piet::SessionState& s, const std::string& name, double want) {
  const double* value = s.eval.eScalars.find(name);
  return value && (std::isnan(want) ? std::isnan(*value) :
                  std::fabs(*value-want) <= 1e-9*std::max(1.0, std::fabs(want)));
}

void matrixIs(const piet::SessionState& s, const std::string& name,
              const std::vector<std::string>& names, const std::vector<double>& want,
              const std::string& arm) {
  const piet::eval::EvalMatrix* matrix = nullptr;
  for (const auto& entry : s.eval.eMatrices) if (entry.first == name) matrix = &entry.second;
  bool same = matrix && matrix->colnames == names && matrix->data.size() == 1 &&
              matrix->data[0].size() == want.size();
  if (same) for (size_t j = 0; j < want.size(); ++j)
    same = same && std::fabs(matrix->data[0][j]-want[j]) <= 1e-9*std::max(1.0, std::fabs(want[j]));
  w2check(same, "built-in lasso " + arm + ": complete " + name + " and coefficient names match captured oracle");
}

}  // namespace

void testW2E3Lasso() {
  std::printf("[selftest] --- W2 E3 lasso (refused:e3_deferred) / rlasso (native) ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_w2e3_lasso").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  {
    piet::SessionState s(workdir);
    for (const char* c : {"clear", "set obs 20", "generate double price = 3000 + 17*_n*_n",
                          "generate double mpg = 10 + mod(_n*7,13)"})
      (void)piet::dispatch(c, s);
    std::string reason;
    const std::string rc = lastRc(s, "lasso logit price mpg", &reason);
    w2check(reason == "refused:e3_deferred" && rc == "199",
            "E3 scaffold: `lasso logit price mpg` refuses with refused:e3_deferred, rc 199. Got rc " +
                rc + " reason " + reason);
    w2check(!s.lastModel().has_value(), "E3 scaffold: `lasso` sets no model state (e())");
    // lasso's own input checks run before the refusal (w2j_e3_builtin.log
    // :3401-3461): L_r_nomodel, L_r_rseed0, L_r_folds1.
    for (const char* line : {"lasso price mpg", "lasso linear price mpg, rseed(0)",
                             "lasso linear price mpg, selection(cv, folds(1)) rseed(1)"}) {
      const std::string rc2 = lastRc(s, line, nullptr);
      w2check(rc2 == "198", std::string("lasso refusal: `") + line +
                                "` is r(198) as in w2j_e3_builtin.log. Got rc " + rc2);
    }
  }
  {
    piet::SessionState s(workdir);
    rlassoData(s);
    const std::string rc = lastRc(s, "rlasso y x1-x12", nullptr);
    w2check(rc == "0", "rlasso B1_plain: `rlasso y x1-x12` runs (oracle rc 0). Got rc " + rc);
    const std::string* sel = s.eval.eStrings.find("selected");
    w2check(sel != nullptr && *sel == "x1 x2 x3 x4 x5 x8 x9 x10",
            "rlasso B1_plain: e(selected) = x1 x2 x3 x4 x5 x8 x9 x10 (w2j_e3_rlasso.log:203). Got " +
                (sel ? *sel : std::string("<unset>")));
    const double* sc = s.eval.eScalars.find("s");
    w2check(sc != nullptr && *sc == 8.0, "rlasso B1_plain: e(s) = 8 (w2j_e3_rlasso.log:203)");
    double b1 = std::nan("");
    for (const auto& kv : s.eval.eMatrices)
      if (kv.first == "b" && !kv.second.data.empty() && !kv.second.data[0].empty())
        b1 = kv.second.data[0][0];
    const double want = std::strtod("0x1.9f6ed661fca46p+0", nullptr);  // log :205
    w2check(std::fabs(b1 - want) <= 1e-9 * std::fabs(want),
            "rlasso B1_plain: e(b)[1,1] (x1) matches the oracle to rtol 1e-9 (log :205)");
  }

  // Independent built-in method identity: these are Stata's lasso results,
  // not the rlasso fixture or its data-dependent penalty convention.
  {
    piet::SessionState s(workdir);
    builtinData(s);
    const std::string plugin = lastRc(s, "lasso linear y x1-x8, selection(plugin)", nullptr);
    w2check(plugin == "0" && scalarIs(s, "N", 240) && scalarIs(s, "k_nonzero_sel", 3) &&
                scalarIs(s, "lambda_sel", 0x1.bbab8b5b6f348p-3) && scalarIs(s, "n_lambda", 1),
            "built-in lasso L_plugin: 240 observations, 3 selected, captured lambda and one fit");
    matrixIs(s, "b", {"x1", "x2", "x3", "_cons"},
             {0x1.9a4119280e59ap+0, -0x1.6b9af5d45f649p-1, 0x1.5a35430169258p-5, 0x1.b393cc2d007ccp+1}, "L_plugin");
    matrixIs(s, "b_postselection", {"x1", "x2", "x3", "_cons"},
             {0x1.f1b6d92791538p+0, -0x1.0014083f879a9p+0, 0x1.fd569fa269763p-2, 0x1.c3707f7624aa0p+0}, "L_plugin_post");
    const std::string bic = lastRc(s, "lasso linear y x1-x8, selection(bic)", nullptr);
    w2check(bic == "0" && scalarIs(s, "ID_sel", 42) && scalarIs(s, "n_lambda", 64) &&
                scalarIs(s, "k_nonzero_sel", 5) && scalarIs(s, "lambda_sel", 0x1.de1cb4c802dd8p-5) &&
                scalarIs(s, "lambda_gmax", 0x1.52c737b13a86bp+1) &&
                scalarIs(s, "ll_sel", -0x1.fd3a7689ef54ep+8),
            "built-in lasso L_bic: selected point42, path64, 5 variables, exact grid and log likelihood");
    matrixIs(s, "b", {"x1", "x2", "x3", "x5", "x8", "_cons"},
             {0x1.e6c91e1d8f7efp+0, -0x1.e9010891a840cp-1, 0x1.c171928313a96p-2,
              0x1.a1b142bac03f1p-3, 0x1.8b768d6cfbc5ap-4, 0x1.99bb849866510p+0}, "L_bic");
    matrixIs(s, "b_standardized", {"x1", "x2", "x3", "x5", "x8", "_cons"},
             {0x1.4b0b363d8bc7dp+1, -0x1.60e3e78e4b95bp+0, 0x1.c0809e333a38fp-2,
              0x1.cc99f9a097253p-3, 0x1.b985d7f784777p-4, 0}, "L_bic_std");
    matrixIs(s, "b_postselection", {"x1", "x2", "x3", "x5", "x8", "_cons"},
             {0x1.f1f44476407e7p+0, -0x1.fc9d331a3bb76p-1, 0x1.fe91ec5e15da8p-2,
              0x1.0ce45b447de9ap-2, 0x1.393d6945655bbp-3, 0x1.499dd6e78b784p+0}, "L_bic_post");
    const std::string ps = lastRc(s, "lasso linear y x1-x8, selection(bic, postselection)", nullptr);
    w2check(ps == "0" && scalarIs(s, "ID_sel", 20) && scalarIs(s, "n_lambda", 48) &&
                scalarIs(s, "k_nonzero_sel", 3) && scalarIs(s, "ll_sel", -0x1.ffb4de3683d19p+8),
            "built-in lasso L_bic_ps: postselection criterion changes selected point and path length");
    const std::string grid = lastRc(s, "lasso linear y x1-x8, selection(bic) grid(20, ratio(1e-3))", nullptr);
    w2check(grid == "0" && scalarIs(s, "ID_sel", 11) && scalarIs(s, "n_lambda", 20) &&
                scalarIs(s, "lambda_sel", 0x1.1dd634ed8a575p-4),
            "built-in lasso L_grid: custom logarithmic grid selects point11 of20");
    const std::string none = lastRc(s, "lasso linear y x1-x8, selection(none)", nullptr);
    const std::string* properties = s.eval.eStrings.find("properties");
    bool hasB = false;
    for (const auto& entry : s.eval.eMatrices) hasB = hasB || entry.first == "b";
    w2check(none == "0" && scalarIs(s, "n_lambda", 64) && scalarIs(s, "lambda_sel", std::nan("")) &&
                (!properties || properties->empty()) && !hasB,
            "built-in lasso L_none: grid without selection leaves no e(b) or stale previous coefficients");
    // Stata's fold partition on the session mt64 stream (oracle L_cv_s1,
    // L_cv_s1_rng, L_cv_setseed, L_cv_s2: w2j_e3_builtin.log:2454-2736).
    (void)piet::dispatch("set seed 1", s);
    const std::string before = s.seed.stream().state_string();
    const std::string cv1 = lastRc(s, "lasso linear y x1-x8, rseed(1)", nullptr);
    const std::string* rngstate = s.eval.eStrings.find("rngstate");
    cecon::Mt64 expected(1); expected.discard(240);
    w2check(cv1 == "0" && scalarIs(s, "ID_sel", 41) && scalarIs(s, "n_lambda", 47) && scalarIs(s, "k_nonzero_sel", 5) &&
                scalarIs(s, "lambda_sel", 0x1.065d1df167ad0p-4) && scalarIs(s, "lambda_serule", 0x1.fbd8c6eaecfd4p-2) &&
                rngstate && *rngstate == before && rngstate->size() == 5011 &&
                s.seed.stream().state_string() == expected.state_string(),
            "built-in lasso L_cv_s1: Stata folds from rseed(1), e(rngstate) = c(rngstate) before the draws, one draw per sample row");
    (void)piet::dispatch("set seed 1", s);
    const std::string cvSession = lastRc(s, "lasso linear y x1-x8", nullptr);
    w2check(cvSession == "0" && scalarIs(s, "ID_sel", 41) && scalarIs(s, "n_lambda", 47),
            "built-in lasso L_cv_setseed: without rseed() the folds come from the session stream");
    const std::string cv2 = lastRc(s, "lasso linear y x1-x8, rseed(2)", nullptr);
    w2check(cv2 == "0" && scalarIs(s, "ID_sel", 43) && scalarIs(s, "n_lambda", 52) && scalarIs(s, "k_nonzero_sel", 6),
            "built-in lasso L_cv_s2: rseed(2) partition");
    // N=57, K=10: the first mod(N,K)=7 folds hold 6 observations, the rest 5
    // (StataBE savecvtouse, internal probe g03/5).
    const std::string odd = lastRc(s, "lasso linear y x1-x8 in 1/57, selection(cv, gridminok) rseed(3)", nullptr);
    std::vector<size_t> counts(10, 0);
    bool blocks = odd == "0";
    for (const auto& entry : s.eval.eMatrices) if (entry.first == "cv_foldid")
      for (const auto& row : entry.second.data) if (!std::isnan(row[0])) ++counts[static_cast<size_t>(row[0])-1];
    w2check(blocks && counts == std::vector<size_t>{6,6,6,6,6,6,6,5,5,5},
            "built-in lasso CV: unbalanced folds are consecutive rank blocks, larger folds first");
  }

  {
    piet::SessionState s(workdir);
    builtinData(s);
    (void)piet::dispatch("generate d = mod(id,2)", s);
    (void)piet::dispatch("generate long cnt = floor(exp(0.3+0.15*x1-0.1*x2+0.05*x3))+mod(_n,3)", s);
    const std::string rc = lastRc(s, "lasso poisson cnt i.(g d) x1-x8, selection(bic)", nullptr);
    w2check(rc == "0" && scalarIs(s, "N", 240) && scalarIs(s, "k_allvars", 13) &&
                scalarIs(s, "ID_sel", 25) && scalarIs(s, "n_lambda", 35) && scalarIs(s, "k_nonzero_sel", 4) &&
                scalarIs(s, "lambda_sel", 0x1.33f9476dc61cdp-4) && scalarIs(s, "ll_sel", -0x1.609ccdc500563p+8),
            "built-in lasso L_pois_bic: full dummy basis, selected25/path35, 4 variables and captured likelihood");
    const piet::eval::EvalMatrix* matrix = nullptr;
    for (const auto& entry : s.eval.eMatrices) if (entry.first == "b") matrix = &entry.second;
    const bool shape = matrix && matrix->colnames == std::vector<std::string>{"1bn.g", "3.g", "x1", "x2", "_cons"} &&
                       matrix->data.size() == 1 && matrix->data[0].size() == 5;
    w2check(shape, "built-in Poisson BIC: complete captured coefficient names and matrix shape");
    // Check every candidate's KKT score, including the five dummy columns.
    // This independently checks the numerical solution and intercept rather
    // than copying one coefficient rounded by Stata's nonlinear stopping rule.
    bool kkt = shape;
    if (shape) {
      piet::Datastore* ds = s.datastore();
      const auto& b = matrix->data[0];
      const double lambda = *s.eval.eScalars.find("lambda_sel");
      std::vector<double> residual(240);
      double interceptScore = 0;
      for (size_t i = 0; i < 240; ++i) {
        const double group = ds->col("g").f64()[i];
        const double eta = b[4]+b[0]*(group==1)+b[1]*(group==3)+b[2]*ds->col("x1").f64()[i]+b[3]*ds->col("x2").f64()[i];
        residual[i] = ds->col("cnt").f64()[i]-std::exp(eta);
        interceptScore += residual[i]/240;
      }
      kkt = std::fabs(interceptScore) < 5e-7;
      for (int j = 0; j < 13; ++j) {
        std::vector<double> values(240);
        double mean = 0, variance = 0;
        for (size_t i = 0; i < 240; ++i) {
          values[i] = j < 3 ? ds->col("g").f64()[i] == j+1 :
                      j < 5 ? ds->col("d").f64()[i] == j-3 : ds->col("x"+std::to_string(j-4)).f64()[i];
          mean += values[i]/240;
        }
        for (double value : values) variance += (value-mean)*(value-mean)/240;
        double score = 0;
        for (size_t i = 0; i < 240; ++i) score += (values[i]-mean)/std::sqrt(variance)*residual[i]/240;
        const int selected = j == 0 ? 0 : j == 2 ? 1 : j == 5 ? 2 : j == 6 ? 3 : -1;
        kkt = kkt && (selected >= 0 ? std::fabs(score-std::copysign(lambda,b[selected])) < 5e-7 : std::fabs(score) <= lambda+5e-7);
      }
    }
    w2check(kkt, "built-in Poisson BIC: intercept and all 13 candidate KKT equations satisfy the documented solver tolerance");
    const std::string plugin = lastRc(s, "lasso poisson cnt i.(g d) x1-x8, selection(plugin)", nullptr);
    const std::string* selected = s.eval.eStrings.find("allvars_sel");
    w2check(plugin == "0" && scalarIs(s, "k_allvars", 13) && scalarIs(s, "k_nonzero_sel", 5) &&
                scalarIs(s, "lambda_sel", 0x1.d073d4be16eb3p-3) && scalarIs(s, "n_lambda", 1) &&
                scalarIs(s, "ll_sel", -0x1.60743683ea26cp+8) && selected && *selected == "1bn.g 3bn.g x1 x2 x3",
            "built-in lasso L_pois_plugin: actual iterative score loadings, captured lambda, selected set and likelihood");
    // Adaptive Poisson CV (oracle L_pois_adapt, w2j_e3_builtin.log:3130):
    // step-2 grid, accumulated n_lambda/ID, same folds both steps, and no net
    // draws from the session stream.
    (void)piet::dispatch("set seed 7", s);
    const std::string untouched = s.seed.stream().state_string();
    const std::string adaptive = lastRc(s, "lasso poisson cnt i.(g d) x1-x8, nolog selection(adaptive)", nullptr);
    w2check(adaptive == "0" && s.seed.stream().state_string() == untouched,
            "built-in adaptive lasso: samegroup restores the RNG state, so the command takes no net draws");
    const std::string adapt1 = lastRc(s, "lasso poisson cnt i.(g d) x1-x8, nolog selection(adaptive) rseed(1)", nullptr);
    const std::string* adaptSelected = s.eval.eStrings.find("allvars_sel");
    // The lambdas are functions of the step-1 converged Poisson coefficients,
    // so they agree with Stata at its own convergence tolerance (T3, 1e-7);
    // measured 1.7e-8.
    auto near = [&](const std::string& name, double want) {
      const double* value = s.eval.eScalars.find(name);
      return value && std::fabs(*value-want) <= 1e-7*std::fabs(want);
    };
    w2check(adapt1 == "0" && scalarIs(s, "ID_sel", 130) && scalarIs(s, "n_lambda", 148) && scalarIs(s, "k_nonzero_sel", 6) &&
                near("lambda_gmax", 0x1.756de03fef351p+3) && near("lambda_sel", 0x1.eb9bad0d1e296p-8) &&
                near("lambda_gmin", 0x1.31e9c7ae06b93p-10) && near("cvm_min", 0x1.fd0c9ac5b09ccp-6) &&
                adaptSelected && *adaptSelected == "1bn.g 3bn.g x1 x2 x3 x6",
            "built-in lasso L_pois_adapt: two CV steps, mean-1 standardized-coefficient weights, captured selection");
  }

  // The Poisson lasso answers its own converged optimum (task r150-lasso-converge).
  // Stata stops its default iterate at a coefficient change of 1e-7, which is
  // 4e-9 to 3.5e-8 from its own optimum (the manifest's T3 rows account for
  // that); PIET must not add its own early stop on top of it. These checks are
  // solver-independent: they recompute the score equations of the posted
  // coefficient vectors from the data.
  //   (1) the penalized fit satisfies the unpenalized intercept equation,
  //       sum(y-mu) = 0 (measured before the fix: 1.1e-9 relative);
  //   (2) e(b_postselection) is the plain Poisson MLE of the selected set,
  //       sum_i x_ij (y_i-mu_i) = 0 for every column including _cons, and
  //       does not depend on the lasso tolerance (measured before: 5e-9);
  //   (3) tolerance() can never loosen the internal floor.
  {
    piet::SessionState s(workdir);
    builtinData(s);
    (void)piet::dispatch("generate d = mod(id,2)", s);
    (void)piet::dispatch("generate long cnt = floor(exp(0.3+0.15*x1-0.1*x2+0.05*x3))+mod(_n,3)", s);
    auto matrixOf = [&](const std::string& name) {
      piet::eval::EvalMatrix result;
      for (const auto& entry : s.eval.eMatrices) if (entry.first == name) result = entry.second;
      return result;
    };
    // The datastore is read only after the fit (datastore() hands the resident
    // plane over, so it must not be held across a dispatch).
    auto term = [&](piet::Datastore* ds, const std::string& name, size_t i) {
      if (name == "_cons") return 1.0;
      // Factor level indicator "<level>[bn].<variable>" (e.g. 1bn.g, 0bn.d).
      const size_t dot = name.find('.');
      if (dot != std::string::npos)
        return ds->col(name.substr(dot+1)).f64()[i] == std::atof(name.c_str()) ? 1.0 : 0.0;
      return ds->col(name).f64()[i];
    };
    // Largest score of a posted vector relative to the size of its terms.
    auto scoreResidual = [&](const std::string& matrixName, bool interceptOnly) {
      const auto m = matrixOf(matrixName);
      if (m.data.size() != 1) return 1.0;
      piet::Datastore* ds = s.datastore();
      double worst = 0;
      for (size_t column = 0; column < m.colnames.size(); ++column) {
        if (interceptOnly && m.colnames[column] != "_cons") continue;
        long double score = 0, scale = 0;
        for (size_t i = 0; i < 240; ++i) {
          double eta = 0;
          for (size_t j = 0; j < m.colnames.size(); ++j) eta += m.data[0][j]*term(ds, m.colnames[j], i);
          const double mu = std::exp(eta), y = ds->col("cnt").f64()[i], x = term(ds, m.colnames[column], i);
          score += x*(y-mu); scale += std::fabs(x)*(y+mu);
        }
        worst = std::max(worst, static_cast<double>(std::fabs(score)/scale));
      }
      return worst;
    };
    const std::string fitted = lastRc(s, "lasso poisson cnt i.(g d) x1-x8, nolog selection(adaptive) rseed(1)", nullptr);
    const double penalizedScore = scoreResidual("b", true), postScore = scoreResidual("b_postselection", false);
    const double cvm = s.eval.eScalars.find("cvm_min") ? *s.eval.eScalars.find("cvm_min") : std::nan("");
    const double lambda = s.eval.eScalars.find("lambda_sel") ? *s.eval.eScalars.find("lambda_sel") : std::nan("");
    std::printf("[selftest] lasso poisson converge: rc %s, penalized intercept score %.3e, postselection score %.3e\n",
                fitted.c_str(), penalizedScore, postScore);
    w2check(fitted == "0" && penalizedScore < 1e-11,
            "built-in Poisson lasso: the penalized fit satisfies the intercept score equation sum(y-mu)=0 to 1e-11");
    w2check(fitted == "0" && postScore < 1e-12,
            "built-in Poisson lasso: e(b_postselection) is the plain Poisson MLE of the selected set (every score 1e-12)");
    const std::string looser = lastRc(s, "lasso poisson cnt i.(g d) x1-x8, nolog selection(adaptive) rseed(1) tolerance(1e-5)", nullptr);
    const double* cvmLoose = s.eval.eScalars.find("cvm_min");
    const double* lambdaLoose = s.eval.eScalars.find("lambda_sel");
    w2check(looser == "0" && cvmLoose && lambdaLoose && *cvmLoose == cvm && *lambdaLoose == lambda &&
                scoreResidual("b_postselection", false) < 1e-12,
            "built-in Poisson lasso: tolerance(1e-5) cannot loosen the internal convergence floor or the postselection refit");
    const std::string tighter = lastRc(s, "lasso poisson cnt i.(g d) x1-x8, nolog selection(adaptive) rseed(1) tolerance(1e-13)", nullptr);
    const double* cvmTight = s.eval.eScalars.find("cvm_min");
    w2check(tighter == "0" && cvmTight && std::fabs(*cvmTight-cvm) <= 1e-9*std::fabs(cvm),
            "built-in Poisson lasso: a tolerance() tighter than the floor still converges to the same optimum");
  }

  // CV contract on a small design: one regressor makes each training fit independently reconstructible by
  // soft-thresholding its standardized covariance with y.
  {
    piet::SessionState s(workdir);
    for (const char* command : {"clear", "set obs 24", "generate double x = _n-12",
        "generate double y = 2+3*x+mod(_n*5,7)/3"}) (void)piet::dispatch(command,s);
    auto matrix = [&](const std::string& name) {
      piet::eval::EvalMatrix result;
      for (const auto& entry : s.eval.eMatrices) if (entry.first == name) result = entry.second;
      return result;
    };
    const std::string options = "selection(cv, folds(3) alllambdas gridminok) grid(12, ratio(.001)) stop(0) rseed(41)";
    const std::string fitted = lastRc(s,"lasso linear y x, "+options,nullptr);
    const auto curve = matrix("cv_curve"), ids = matrix("cv_foldid");
    bool valid = fitted == "0" && scalarIs(s,"N",24) && scalarIs(s,"n_fold",3) && curve.data.size()==12 && ids.data.size()==24;
    std::vector<size_t> counts(3,0);
    if (valid) for (const auto& row : ids.data) {
      if (row.size()!=1 || row[0]<1 || row[0]>3) { valid=false; break; }
      ++counts[static_cast<size_t>(row[0])-1];
    }
    w2check(valid && counts==std::vector<size_t>{8,8,8},"linear CV: every observation belongs to one balanced held-out fold");
    if (valid) {
      piet::Datastore* ds=s.datastore();
      for (const auto& point : curve.data) {
        double total=0; std::vector<double> foldLoss;
        for (size_t fold=1;fold<=3;++fold) {
          double mx=0,my=0,variance=0,cov=0; size_t train=0;
          for (size_t i=0;i<24;++i) if (ids.data[i][0]!=fold) {mx+=ds->col("x").f64()[i];my+=ds->col("y").f64()[i];++train;}
          mx/=train;my/=train;
          for (size_t i=0;i<24;++i) if(ids.data[i][0]!=fold) variance+=(ds->col("x").f64()[i]-mx)*(ds->col("x").f64()[i]-mx)/train;
          const double scale=std::sqrt(variance);
          for (size_t i=0;i<24;++i) if(ids.data[i][0]!=fold) cov+=(ds->col("x").f64()[i]-mx)/scale*(ds->col("y").f64()[i]-my)/train;
          const double beta=std::copysign(std::max(std::fabs(cov)-point[0],0.0),cov);
          double loss=0;
          for(size_t i=0;i<24;++i) if(ids.data[i][0]==fold) {
            const double residual=ds->col("y").f64()[i]-(my+beta*(ds->col("x").f64()[i]-mx)/scale);loss+=residual*residual;
          }
          total+=loss;foldLoss.push_back(loss/8);
        }
        const double mean=(foldLoss[0]+foldLoss[1]+foldLoss[2])/3;
        double variance=0;for(double value:foldLoss) variance+=(value-mean)*(value-mean)/2;  // sample sd of the K=3 fold means
        valid=valid && std::fabs(total/24-point[1])<1e-9*std::max(1.0,point[1]) && std::fabs(std::sqrt(variance)-point[2])<1e-9*std::max(1.0,point[2]);
      }
    }
    w2check(valid,"linear CV: every grid loss and SE agrees with independent training-only soft-threshold fits");
    const auto firstB=matrix("b");
    const std::string repeated=lastRc(s,"lasso linear y x, "+options,nullptr);
    w2check(repeated=="0" && matrix("cv_foldid").data==ids.data && matrix("cv_curve").data==curve.data && matrix("b").data==firstB.data,
            "linear CV: explicit rseed reproduces folds, loss curve and full-sample selected coefficients");
    const std::string changed=lastRc(s,"lasso linear y x, selection(cv, folds(3) alllambdas gridminok) grid(12, ratio(.001)) stop(0) rseed(42)",nullptr);
    w2check(changed=="0" && matrix("cv_foldid").data!=ids.data,"linear CV: a different explicit seed changes fold assignment");
    const std::string oneSE=lastRc(s,"lasso linear y x, selection(cv, folds(3) alllambdas gridminok serule) grid(12, ratio(.001)) stop(0) rseed(41)",nullptr);
    size_t minimum=0,one=0;
    if(!curve.data.empty()) {
      for(size_t i=1;i<curve.data.size();++i) if(curve.data[i][1]<curve.data[minimum][1]) minimum=i;
      for(size_t i=0;i<=minimum;++i) if(curve.data[i][1]<=curve.data[minimum][1]+curve.data[minimum][2]) {one=i;break;}
    }
    w2check(oneSE=="0" && scalarIs(s,"ID_cv",minimum+1) && scalarIs(s,"ID_serule",one+1) && scalarIs(s,"ID_sel",one+1),
            "linear CV: one-SE chooses the largest lambda inside the minimum's standard-error band");
    const std::string loo="selection(cv, folds(24) alllambdas gridminok) grid(6, ratio(.001))";
    const std::string loo1=lastRc(s,"lasso linear y x, "+loo+" rseed(1)",nullptr);const auto looCurve=matrix("cv_curve");
    const std::string loo2=lastRc(s,"lasso linear y x, "+loo+" rseed(2)",nullptr);
    // Same partition; only the fold order (hence summation order) moves.
    bool sameLoo = loo1=="0" && loo2=="0" && matrix("cv_curve").data.size()==looCurve.data.size();
    if (sameLoo) for (size_t i=0;i<looCurve.data.size();++i) for (size_t j=0;j<looCurve.data[i].size();++j)
      sameLoo = sameLoo && std::fabs(matrix("cv_curve").data[i][j]-looCurve.data[i][j]) <= 1e-12*std::max(1.0,std::fabs(looCurve.data[i][j]));
    w2check(sameLoo,"linear leave-one-out CV has no seed-dependent partition");
    const std::string qualified=lastRc(s,"lasso linear y x if _n!=2 in 1/18, selection(cv, folds(3) alllambdas gridminok) grid(8, ratio(.001)) rseed(41)",nullptr);
    const auto selectedB=matrix("b");
    w2check(qualified=="0" && scalarIs(s,"N",17) && std::isnan(matrix("cv_foldid").data[1][0]) && std::isnan(matrix("cv_foldid").data[23][0]),
            "linear CV if/in qualifiers define both the sample and fold membership");
    const auto cached=s.lastModel()->result->at("lasso_fit");
    (void)piet::dispatch("replace y = 999",s);  // an OLS refit would now predict999
    const std::string predicted=lastRc(s,"predict double fitted",nullptr);
    bool correct=predicted=="0";
    if(correct) {
      piet::Datastore* ds=s.datastore();
      for(size_t i=0;i<24;++i) {
        double expected=cached.at("intercept").get<double>();
        for(const auto& term:cached.at("terms")) expected+=term.at("b").get<double>()*ds->col("x").f64()[i];
        correct=correct && std::fabs(ds->col("fitted").f64()[i]-expected)<1e-11;
      }
    }
    w2check(correct && matrix("b").data==selectedB.data,"lasso predict evaluates cached penalized coefficients on all live rows, without a refit");
    const std::string mask=lastRc(s,"predict float masked if mod(_n,2)==0 in 2/10",nullptr);
    bool masked=mask=="0";
    if(masked) {
      piet::Datastore* ds=s.datastore();
      for(size_t i=0;i<24;++i) {
        const bool selected=i>=1 && i<10 && (i+1)%2==0;
        const double value=ds->col("masked").f64()[i];
        masked=masked && (selected ? value==static_cast<double>(static_cast<float>(ds->col("fitted").f64()[i])) : std::isnan(value));
      }
    }
    w2check(masked && s.varMeta["masked"].stataType==std::optional<std::string>("float"),"lasso predict preserves explicit float storage and combined if/in mask");
    const std::string post=lastRc(s,"predict double postfit, postselection",nullptr);
    w2check(post=="0" && lastRc(s,"predict bad, pr",nullptr)=="198" && lastRc(s,"lassocoef, display(coef, penalized) sort(names)",nullptr)=="0" &&
                lastRc(s,"lassocoef, unsupported",nullptr)=="198",
            "lasso postselection prediction and current-fit lassocoef are real supported paths; unknown options refuse");
  }
  // FV products (oracle L_cv_if: i.g#i.d without main effects) equal the
  // explicit all-cell dummy basis, including predictions outside the sample.
  {
    piet::SessionState s(workdir);
    for (const char* command : {"clear", "set obs 72", "generate g = mod(_n,3)+1",
        "generate d = mod(_n,2)", "generate double x = (mod(_n*7,17)-8)/3",
        "generate double y = 1+x*(1+d)+2*(g==2 & d==1)+mod(_n*11,13)/10"}) (void)piet::dispatch(command,s);
    for (int g=1;g<=3;++g) for (int d=0;d<=1;++d)
      (void)piet::dispatch("generate double cell"+std::to_string(g)+std::to_string(d)+" = (g=="+std::to_string(g)+" & d=="+std::to_string(d)+")",s);
    (void)piet::dispatch("generate double xd0 = x*(d==0)",s);
    (void)piet::dispatch("generate double xd1 = x*(d==1)",s);
    const std::string options=" in 1/60, selection(cv, folds(3) alllambdas gridminok) grid(12, ratio(.001)) stop(0) rseed(17)";
    const std::string factored=lastRc(s,"lasso linear y i.g#i.d c.x#i.d"+options,nullptr);
    const bool shape=factored=="0" && scalarIs(s,"k_allvars",8) && scalarIs(s,"N",60);
    const std::string predicted=lastRc(s,"predict double factor_fit",nullptr);
    const std::string explicitFit=lastRc(s,"lasso linear y cell10 cell11 cell20 cell21 cell30 cell31 xd0 xd1"+options,nullptr);
    const std::string explicitPrediction=lastRc(s,"predict double explicit_fit",nullptr);
    bool equal=shape && predicted=="0" && explicitFit=="0" && explicitPrediction=="0";
    if(equal) for(size_t i=0;i<72;++i)
      equal=equal && std::fabs(s.datastore()->col("factor_fit").f64()[i]-s.datastore()->col("explicit_fit").f64()[i])<1e-11;
    w2check(equal,"lasso FV products equal the explicit full dummy/product basis, including predictions outside the fit sample");
  }
  struct Refusal {
    const char* line;
    const char* rc;
    const char* arm;
  };
  static const Refusal kRefusals[] = {
      {"rlasso y", "102", "R6_single_variable"},
      {"rlasso y x1 sid", "109", "R7_string_regressor"},
      {"rlasso y x1-x8, cluster(nosuchvar)", "111", "R8_cluster_unknown_variable"},
      {"rlasso y x1-x8 if id>999", "2000", "R9_empty_sample"}};
  for (const Refusal& r : kRefusals) {
    piet::SessionState s(workdir);
    rlassoData(s);
    const std::string rc = lastRc(s, r.line, nullptr);
    w2check(rc == r.rc, std::string("rlasso ") + r.arm + ": `" + r.line + "` is r(" + r.rc +
                            ") as in w2j_e3_rlasso.log. Got rc " + rc);
  }
}
