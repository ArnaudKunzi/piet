// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// lincom parity test vs Stata: linear combinations after regress visits_w age_z
// age2 edu_hi. Golden from lincom_golden.do. est = r'β, se = sqrt(r'Vr), 95% CI
// via t(dof_resid). Matches Stata to ~1e-10.
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "core/distributions.h"
#include "core/linalg.h"
#include "core/model.h"
#include "estimators/feols.h"
#include "estimators/fepois.h"
#include "estimators/logit.h"
#include "estimators/posthest.h"
#include "estimators/probit.h"
#include "io/columnar.h"
#include "validate_dir.h"

using namespace cecon;

static int test_lincom_ols_golden() {
    std::printf("test_lincom:\n");
    const cobra_test::ValidateBase base;
    std::map<std::string, std::vector<double>> g;  // exp -> [est, se, lb, ub]
    {
        std::ifstream f(base + "lincom_golden.csv");
        std::string line;
        std::getline(f, line);
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            std::stringstream ss(line);
            std::string e, est, se, lb, ub;
            std::getline(ss, e, ','); std::getline(ss, est, ','); std::getline(ss, se, ',');
            std::getline(ss, lb, ','); std::getline(ss, ub, ',');
            g[e] = {std::stod(est), std::stod(se), std::stod(lb), std::stod(ub)};
        }
    }

    Dataset ds = Dataset::open(base + "nlcom.hbin");
    ModelData md = build_model(ds, "visits_w", {"age_z", "age2", "edu_hi"}, {}, {});
    FeolsResult r = feols_fit(md, Vcov::IID);
    const int k = static_cast<int>(r.names.size());
    std::map<std::string, int> idx;
    for (int j = 0; j < k; ++j) idx[r.names[j]] = j;
    double dof = static_cast<double>(r.dof.dof_resid);
    double tc = t_critical(0.05, dof);

    // weight vectors + offsets matching the golden expressions
    auto eval = [&](std::vector<double> w, double offset) {
        double est = offset, var = 0;
        for (int p = 0; p < k; ++p) est += w[p] * r.beta[p];
        for (int p = 0; p < k; ++p) { double row = 0;
            for (int q = 0; q < k; ++q) row += w[q] * r.vcov_matrix[static_cast<size_t>(q) * k + p];
            var += w[p] * row; }
        double se = std::sqrt(var);
        return std::vector<double>{est, se, est - tc * se, est + tc * se};
    };
    int A = idx["age_z"], A2 = idx["age2"], E = idx["edu_hi"], C = idx["(intercept)"];
    std::map<std::string, std::vector<double>> got;
    { std::vector<double> w(k, 0); w[A] = 1; w[E] = 1; got["c1"] = eval(w, 0); }
    { std::vector<double> w(k, 0); w[A] = 2; w[A2] = -1; got["c2"] = eval(w, 0); }
    { std::vector<double> w(k, 0); w[E] = 1; got["c3"] = eval(w, 1.5); }
    { std::vector<double> w(k, 0); w[C] = 1; w[A] = -1; got["c4"] = eval(w, 0); }

    int fail = 0;
    const char* lab[4] = {"est", "se", "lb", "ub"};
    for (auto& kv : g) {
        for (int i = 0; i < 4; ++i) {
            double gv = kv.second[i], gt = got[kv.first][i];
            double rel = std::fabs(gt - gv) / (std::fabs(gv) + 1e-30);
            std::printf("  %s.%-3s got=%.10g stata=%.10g rel=%.2e\n", kv.first.c_str(), lab[i], gt, gv, rel);
            if (!(rel <= 1e-9)) { std::printf("    <<FAIL\n"); fail = 1; }
        }
    }
    std::printf("%s\n", fail ? "FAILED" : "PASSED");
    return fail;
}


// ---------------------------------------------------------------------------
// BUG-203: lincom/nlcom/test after a MAXIMUM-LIKELIHOOD fit.
//
// Until this batch cobra's lincom/nlcom/test CLIs had no `--model` flag, so
// apps/cmds/cmds_postest.cpp answered every one of them with feols_fit() — a
// caller who had estimated `logit y x1 x2` got the LINEAR-PROBABILITY-MODEL
// combination back with no error at all. The PIET side never inspected the
// estimator either, so `logit` -> `lincom x1` and `poisson` -> `lincom x1`
// returned the SAME number, which is the proof the estimator was never read.
//
// This reproduces the arithmetic cmd_lincom/cmd_test now run on the `--model`
// path — the fit's own information-matrix covariance, referenced to the NORMAL
// (Stata prints z after an ML fit, and chi2(J) for `test`, never t/F) — against
// a live StataNow 19.5 capture. Do-file discarded after the capture; the data
// and the numbers below ARE the durable artifact (BUG-195's lost-oracle
// lesson), and the design is embedded verbatim rather than regenerated because
// this engine's RNG stream does not need to match Stata's, only the fit does.
//
// Capture (StataBE -b, judged by an in-log marker, never by exit code):
//   set seed 203203 / set obs 120
//   gen double x1 = round(rnormal(0,1), .001)
//   gen double x2 = round(rnormal(0,1), .001)
//   gen byte ybin = runiform() < invlogit(0.3 + 1.1*x1 - 0.7*x2)
// then logit / poisson / probit / regress on ybin x1 x2, each followed by
// `lincom x1` (and `test`, `nlcom` on the logit fit).
//
// THE RED LEG. Every oracle row below differs from 0.221521751160 (se
// .046231897247) — Stata's own `regress ybin x1 x2` -> `lincom x1`, i.e.
// exactly what this engine served for logit, probit AND poisson before the fix.
// kOlsRefitB/kOlsRefitSe are asserted to be far from each ML answer, so a
// regression back to the OLS refit fails this test on the number, not just on
// a tolerance.

// The fixture arrays live at file scope (BUG-210) so the margins leg at the
// bottom can reuse them instead of duplicating 360 literals; the capture recipe
// and the reason they are embedded verbatim are in the comment block above.
namespace bug203 {
    static const double x1[] = {
        -1.006, -.23, -.584, .471, -2.101, -1.071, 1.091, .063, -1.497, -.716,
        -.262, -.657, -.525, .711, -2.622, 1.808, -.242, 1.435, -.97, -.916,
        -.851, 1.276, .356, .501, -.927, -.66, .718, -.123, .29, .042,
        1.039, .38, .104, -.767, .374, -1.658, -.386, 1.038, -.753, -1.042,
        -.696, .106, 1.841, 1.032, .032, .28, 1.664, -.108, .492, .781,
        -.959, -.336, -.942, -.749, -.946, 1.129, .372, -1.312, -.152, -.548,
        .985, -.478, -1.042, -1.374, -1.087, .657, .462, -.385, -.625, .67,
        1.797, .26, -.513, -.351, 1.773, -.947, .329, -.093, 1.247, .003,
        -1.185, -1.179, .136, .271, .997, .716, .228, .23, .058, 1.566,
        -.614, -.227, .76, -.787, -2.332, -1.387, .634, .456, -.872, -.061,
        .56, -.698, .349, .601, .225, -1.219, -.286, -.32, -.066, .753,
        -.384, -.057, -.417, .374, -1.588, .754, -.219, 1.027, .253, -.637,
    };
    static const double x2[] = {
        .604, -1.071, -.538, -1.414, -1.388, .75, .12, -.709, -.085, -1.3,
        -1.257, -.487, .803, -.496, .192, -.751, -.255, -.888, -2.099, .962,
        .536, -2.316, -.235, .05, .287, .668, -1.024, -.655, .594, .565,
        .24, -1.185, -.066, -.339, .24, 1.038, 1.593, .678, -.898, .469,
        .497, .011, 1.714, 3.693, -.338, -.847, .012, .51, 1.631, -.716,
        -.246, 1.75, -2.275, -.05, .547, .115, .317, -.908, .803, 1.433,
        .368, .38, -.225, .148, .973, 1.486, -.503, .284, 1.047, -1.217,
        .472, -.496, 1.298, .253, -.204, 1.624, -.194, .007, -.367, 1.869,
        -.838, -1.097, -.697, -.059, .464, -.376, -.862, -.991, -.196, .51,
        -1.156, .085, .87, 1.932, -1.731, 1.448, -.996, -.96, .777, 2.625,
        -.543, -1.405, .309, .192, .779, .879, -1.971, -.662, -.98, 1.624,
        .214, .814, -2.04, .093, -1.037, .701, -.789, -.044, -.575, -1.252,
    };
    static const double yb[] = {
        0, 1, 1, 0, 0, 1, 1, 1, 0, 1, 0, 1, 0, 1, 0, 1, 1, 1, 0, 0,
        0, 1, 0, 1, 0, 1, 1, 1, 1, 1, 0, 1, 1, 0, 1, 0, 1, 0, 0, 1,
        0, 0, 1, 0, 1, 0, 1, 0, 1, 0, 0, 0, 1, 1, 0, 1, 1, 1, 0, 0,
        1, 0, 0, 0, 1, 1, 0, 0, 0, 0, 1, 1, 0, 1, 1, 0, 1, 1, 1, 0,
        0, 0, 1, 0, 0, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 1, 1, 1, 0,
        1, 1, 0, 1, 1, 0, 0, 0, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1,
    };
}  // namespace bug203

// Build the column-major design (x1, x2, intercept LAST — matching load_glm) and
// the outcome from the fixture above.
static void bug203_fixture(std::vector<double>& X, std::vector<double>& y,
                           int64_t& n, int& k) {
    n = 120;
    k = 3;  // x1, x2, (intercept) — intercept LAST, matching load_glm
    X.assign(static_cast<size_t>(n) * k, 0.0);
    y.assign(static_cast<size_t>(n), 0.0);
    for (int64_t i = 0; i < n; ++i) {
        X[static_cast<size_t>(i)] = bug203::x1[i];
        X[static_cast<size_t>(n + i)] = bug203::x2[i];
        X[static_cast<size_t>(2 * n + i)] = 1.0;
        y[static_cast<size_t>(i)] = bug203::yb[i];
    }
}

static int test_lincom_ml_refit_bug203() {
    std::printf("test_lincom_ml_refit (BUG-203):\n");
    std::vector<double> X, y;
    int k = 0; int64_t n = 0;
    bug203_fixture(X, y, n, k);

    int fail = 0;
    // The engine's own asymptotic critical value (cmds_postest.cpp's
    // postest_crit on an `asymptotic` model) — NOT t(dof_resid).
    const double zc = z_critical(0.05);

    auto cmp = [&](const char* what, double got, double stata, double rtol) {
        double rel = std::fabs(got - stata) / (std::fabs(stata) + 1e-30);
        std::printf("  %-28s got=%.12g stata=%.12g rel=%.2e\n", what, got, stata, rel);
        if (!(rel <= rtol)) { std::printf("    <<FAIL (rtol %.0e)\n", rtol); fail = 1; }
    };
    // est = w'b, se = sqrt(w'Vw) — cmd_lincom's tail verbatim (V is k*k,
    // column-major and symmetric, so the index form matches either way).
    auto lincom = [&](const std::vector<double>& b, const std::vector<double>& V,
                      const std::vector<double>& w, double& est, double& se) {
        est = 0; double var = 0;
        for (int p = 0; p < k; ++p) est += w[static_cast<size_t>(p)] * b[static_cast<size_t>(p)];
        for (int p = 0; p < k; ++p) {
            double row = 0;
            for (int q = 0; q < k; ++q)
                row += w[static_cast<size_t>(q)] * V[static_cast<size_t>(q) * k + p];
            var += w[static_cast<size_t>(p)] * row;
        }
        se = std::sqrt(std::max(0.0, var));
    };

    // What the pre-fix engine served for ALL of logit/probit/poisson: Stata's
    // own `regress ybin x1 x2` -> `lincom x1`.
    const double kOlsRefitB = 0.221521751160, kOlsRefitSe = 0.046231897247;

    // ---- logit -----------------------------------------------------------
    {
        LogitResult lr = logit_fit(X.data(), n, k, y.data());
        if (!lr.converged) { std::printf("  logit_fit did not converge <<FAIL\n"); fail = 1; }
        double est, se;
        std::vector<double> w(static_cast<size_t>(k), 0.0);
        w[0] = 1.0;  // lincom x1
        lincom(lr.beta, lr.bread, w, est, se);
        cmp("logit lincom x1 .b", est, 1.087742905079, 1e-9);
        cmp("logit lincom x1 .se", se, 0.266346342116, 1e-9);
        cmp("logit lincom x1 .lb", est - zc * se, 0.565713667118, 1e-9);
        cmp("logit lincom x1 .ub", est + zc * se, 1.609772143040, 1e-9);
        if (std::fabs(est - kOlsRefitB) < 1e-3) {
            std::printf("    <<FAIL: logit lincom returned the OLS-refit answer (BUG-203)\n");
            fail = 1;
        }
        std::vector<double> w2(static_cast<size_t>(k), 0.0);
        w2[0] = 1.0; w2[1] = 1.0;  // lincom x1 + x2
        lincom(lr.beta, lr.bread, w2, est, se);
        cmp("logit lincom x1+x2 .b", est, 0.566604685137, 1e-9);
        cmp("logit lincom x1+x2 .se", se, 0.303122262498, 1e-9);

        // `nlcom exp(_b[x1])`: delta method, d/db exp(b) = exp(b), so the SE is
        // exp(b)*se(b). Stata: 2.967568424150 (se .790400994758).
        lincom(lr.beta, lr.bread, w, est, se);
        cmp("logit nlcom exp(_b[x1]) .b", std::exp(est), 2.967568424150, 1e-9);
        cmp("logit nlcom exp(_b[x1]) .se", std::exp(est) * se, 0.790400994758, 1e-9);

        // `test x1` and `test x1 x2`: Stata reports chi2(J) after an ML fit, not
        // F(J, dof_resid) — there is no residual dof to reference.
        std::vector<double> R(static_cast<size_t>(k), 0.0), q(1, 0.0);
        R[0] = 1.0;
        WaldResult w1 = wald_test(lr.beta.data(), lr.bread.data(), k, R.data(), q.data(), 1);
        cmp("logit test x1 .chi2", w1.chi2, 16.678578946346, 1e-9);
        std::vector<double> R2(static_cast<size_t>(2 * k), 0.0), q2(2, 0.0);
        R2[0] = 1.0; R2[static_cast<size_t>(k) + 1] = 1.0;
        WaldResult w2r = wald_test(lr.beta.data(), lr.bread.data(), k, R2.data(), q2.data(), 2);
        cmp("logit test x1 x2 .chi2", w2r.chi2, 19.951516286487, 1e-9);
    }

    // ---- poisson (the row that proves the estimator is READ) --------------
    // Same y and same x as the logit fit above, so a plain-OLS refit answers
    // them identically. Stata does not: 0.408561052511 vs logit's 1.087742905079.
    {
        ModelData md;
        md.has_intercept = true;
        md.dm.n = n; md.dm.k = k;
        md.dm.X = X; md.dm.y = y;
        md.dm.xnames = {"x1", "x2", "(intercept)"};
        FepoisResult pf = fepois_fit(md, Vcov::IID);
        double est, se;
        std::vector<double> w(static_cast<size_t>(k), 0.0);
        w[0] = 1.0;
        lincom(pf.beta, pf.vcov_matrix, w, est, se);
        cmp("poisson lincom x1 .b", est, 0.408561052511, 1e-8);
        cmp("poisson lincom x1 .se", se, 0.140587815397, 1e-8);
        cmp("poisson lincom x1 .lb", est - zc * se, 0.133013997668, 1e-7);
        cmp("poisson lincom x1 .ub", est + zc * se, 0.684108107354, 1e-8);
        if (std::fabs(est - kOlsRefitB) < 1e-3 || std::fabs(se - kOlsRefitSe) < 1e-4) {
            std::printf("    <<FAIL: poisson lincom returned the OLS-refit answer (BUG-203)\n");
            fail = 1;
        }
        std::vector<double> R(static_cast<size_t>(k), 0.0), q(1, 0.0);
        R[0] = 1.0;
        WaldResult wr = wald_test(pf.beta.data(), pf.vcov_matrix.data(), k, R.data(), q.data(), 1);
        cmp("poisson test x1 .chi2", wr.chi2, 8.445367689880, 1e-8);
    }

    // ---- probit ----------------------------------------------------------
    // Looser tolerance by design: probit is the ONE swept non-canonical
    // OIM-vs-EIM case (BUG-140's audit table), ~1e-7 relative against Stata.
    //
    // THE COVARIANCE HERE IS NOT `pr.V`, AND THAT IS THE POINT. probit's link is
    // non-canonical, so the Fisher/expected information (X'WX)^{-1} that
    // ProbitResult::V carries is NOT what Stata's `probit` reports — Stata uses
    // the OBSERVED Hessian, weight lambda(lambda+eta). Written against `pr.V`,
    // this leg fails at rel 1.24e-2 (se .152915189 vs Stata's .154831744) —
    // measured, and it is how the first cut of cmds_postest.cpp's
    // postest_fit_glm was caught before it shipped.
    //
    // BUG-210: this block used to carry a FOURTH, test-side copy of the loop, so
    // it pinned its own reimplementation rather than the engine's — a break in
    // production would not have failed it. The loop is now ONE definition
    // (cecon::probit_observed_info) and this calls it, which is what makes this a
    // pin on the shipped code path instead of on a parallel transcription.
    {
        ProbitResult pr = probit_fit(X.data(), n, k, y.data());
        ProbitObservedInfo oi = probit_observed_info(pr, X.data(), n, k, y.data());
        if (!oi.observed_spd) {
            std::printf("  probit observed information not SPD <<FAIL\n");
            fail = 1;
        }
        const std::vector<double>& V = oi.bread;
        double est, se;
        std::vector<double> w(static_cast<size_t>(k), 0.0);
        w[0] = 1.0;
        lincom(pr.beta, V, w, est, se);
        cmp("probit lincom x1 .b", est, 0.662891968649, 1e-6);
        cmp("probit lincom x1 .se", se, 0.154831743849, 1e-6);
        if (std::fabs(est - kOlsRefitB) < 1e-3) {
            std::printf("    <<FAIL: probit lincom returned the OLS-refit answer (BUG-203)\n");
            fail = 1;
        }
    }

    std::printf("%s\n", fail ? "FAILED" : "PASSED");
    return fail;
}

// ---- BUG-297: `predict --model` — the ML prediction itself -------------------
//
// Same n=120 seed-203203 fixture. BUG-204's entry recorded what Stata's
// `predict double p` after `logit ybin x1 x2` returns for the first two rows —
// and what the pre-fix engine wrote instead (its own OLS fitted values):
//
//   row | Stata (pr)      | pre-fix engine (OLS xb)
//     1 | 0.254170566797  | 0.278809077142
//     2 | 0.654867291451  | 0.633506355461
//
// cmd_predict's --model branch (apps/cmds/cmds_postest.cpp) is CLI plumbing
// with no external linkage, so — exactly like the BUG-203 legs above — this
// pins the shipped FITTERS + the inverse-link convention (verbatim
// cmd_margins' GLM branch: logistic / med_ncdf / exp) that branch applies; the
// end-to-end wiring is pinned live in piet_native's testBUG297PredictGlm
// against a fresh 336-row Stata capture. Per BUG-204's blindness note the
// range is asserted over ALL rows, never the mean alone: the OLS column's
// whole failure mode (mean equal to 15 digits, values outside [0,1]) is
// invisible to central tendency.
static int test_predict_ml_bug297() {
    std::printf("test_predict_ml (BUG-297):\n");
    std::vector<double> X, y;
    int k = 0; int64_t n = 0;
    bug203_fixture(X, y, n, k);

    int fail = 0;
    auto cmp = [&](const char* what, double got, double stata, double rtol) {
        double rel = std::fabs(got - stata) / (std::fabs(stata) + 1e-30);
        std::printf("  %-28s got=%.12g stata=%.12g rel=%.2e\n", what, got, stata, rel);
        if (!(rel <= rtol)) { std::printf("    <<FAIL (rtol %.0e)\n", rtol); fail = 1; }
    };
    auto eta_row = [&](const std::vector<double>& b, int64_t i) {
        double e = 0;
        for (int c = 0; c < k; ++c) e += X[static_cast<size_t>(c) * n + i] * b[static_cast<size_t>(c)];
        return e;
    };

    LogitResult lr = logit_fit(X.data(), n, k, y.data());
    if (!lr.converged) { std::printf("  logit_fit did not converge <<FAIL\n"); fail = 1; }
    // Stata's default statistic after logit: pr = invlogit(xb).
    // rtol 1e-7: measured rel 1.3e-8/2.1e-8 — the logit convergence floor
    // amplified through eta (the BUG-210 legs beside this run at 2e-8..9e-8
    // for the same reason); the defect pinned against differs at ~2e-2.
    const double p1 = 1.0 / (1.0 + std::exp(-eta_row(lr.beta, 0)));
    const double p2 = 1.0 / (1.0 + std::exp(-eta_row(lr.beta, 1)));
    cmp("logit predict pr[1]", p1, 0.254170566797, 1e-7);
    cmp("logit predict pr[2]", p2, 0.654867291451, 1e-7);
    // The pre-fix values were the OLS fitted ones — assert we are ORDERS away
    // from them, not merely near Stata (the two differ by ~2e-2 absolute).
    if (std::fabs(p1 - 0.278809077142) < 1e-3 || std::fabs(p2 - 0.633506355461) < 1e-3) {
        std::printf("    <<FAIL: predict returned the OLS fitted value (BUG-204's defect)\n");
        fail = 1;
    }
    // THE RANGE PIN, all 120 rows: a probability outside [0,1] is categorical
    // failure regardless of any summary statistic.
    for (int64_t i = 0; i < n; ++i) {
        double pi = 1.0 / (1.0 + std::exp(-eta_row(lr.beta, i)));
        if (!(pi >= 0.0 && pi <= 1.0)) {
            std::printf("  <<FAIL: pr[%lld] = %.6g outside [0,1]\n",
                        static_cast<long long>(i + 1), pi);
            fail = 1;
        }
    }

    std::printf("%s\n", fail ? "FAILED" : "PASSED");
    return fail;
}

// ---- BUG-210: `margins` after probit must use the OBSERVED information -------
//
// Same n=120 seed-203203 fixture as test_lincom_ml_refit_bug203 above (the arrays
// are that function's; this leg re-derives the design from them rather than
// duplicating 360 literals). Oracle: StataNow 19.5, StataBE -b, judged by an
// in-log WSM_ marker, never by exit code:
//
//   probit ybin x1 x2
//   margins, dydx(x1 x2)          -> dy/dx .218046920741  se .038064606025   (x1)
//                                                -.103320184241  se .036952040176 (x2)
//
// THE RED. cmd_margins had FIVE probit sites reading the Fisher bread pr.V — the
// plain per-column path, the general/--dydx path, the at()+dydx path, the at()
// predictive path (and the vce(robust) branch, which alone was already correct).
// Against pr.V this leg reports se .037501556827 for x1 — rel 1.5e-2, measured on
// the shipped 0.38.0 binary, point estimate unchanged. That "estimate right, SE
// wrong" signature is the covariance-source fingerprint of this whole bug class.
//
// The kFisherSe guard below asserts the answer is FAR from the Fisher one, so a
// regression back to pr.V fails on the number rather than on a tolerance.
//
// Tolerance T3 (1e-7): a converged iterative MLE quantity — Stata's own
// convergence tolerance means the oracle is not defined tighter (TOLERANCE.md).
static int test_margins_probit_observed_info_bug210() {
    std::printf("test_margins_probit_observed_info (BUG-210):\n");
    std::vector<double> X, y;
    int k = 0; int64_t n = 0;
    bug203_fixture(X, y, n, k);

    int fail = 0;
    ProbitResult pr = probit_fit(X.data(), n, k, y.data());
    if (!pr.converged) { std::printf("  probit_fit did not converge <<FAIL\n"); fail = 1; }

    // The engine's ONE observed-information definition — the same call every
    // cmd_margins probit site now makes.
    ProbitObservedInfo oi = probit_observed_info(pr, X.data(), n, k, y.data());
    if (!oi.observed_spd) { std::printf("  observed information not SPD <<FAIL\n"); fail = 1; }

    auto cmp = [&](const char* what, double got, double stata, double rtol) {
        double rel = std::fabs(got - stata) / (std::fabs(stata) + 1e-30);
        std::printf("  %-34s got=%.12g stata=%.12g rel=%.2e\n", what, got, stata, rel);
        if (!(rel <= rtol)) { std::printf("    <<FAIL (rtol %.0e)\n", rtol); fail = 1; }
    };

    // What the pre-fix engine served on all four iid probit margins sites.
    const double kFisherSeX1 = 0.037501556827;

    MarginsResult m = margins_probit(X.data(), n, k, pr.beta.data(), oi.bread.data());
    if (!m.ok) { std::printf("  margins_probit failed <<FAIL\n"); return 1; }
    cmp("margins probit dydx x1", m.dydx[0], 0.218046920741, 1e-7);
    cmp("margins probit se   x1", m.se[0], 0.038064606025, 1e-7);
    cmp("margins probit dydx x2", m.dydx[1], -0.103320184241, 1e-7);
    cmp("margins probit se   x2", m.se[1], 0.036952040176, 1e-7);
    if (std::fabs(m.se[0] - kFisherSeX1) < 1e-6) {
        std::printf("    <<FAIL: margins after probit is back on the FISHER bread (BUG-210)\n");
        fail = 1;
    }

    // The general margins_ame path (--dydx / interactions / weights) must agree —
    // it is a different code path to the same number, and it was equally RED.
    const std::vector<std::string> names = {"x1", "x2", "(intercept)"};
    AmeResult a = margins_ame(GlmLink::Probit, X.data(), n, k, names,
                              pr.beta.data(), oi.bread.data(), nullptr, {"x1", "x2"});
    if (!a.ok) { std::printf("  margins_ame failed: %s <<FAIL\n", a.error.c_str()); return 1; }
    if (a.rows.size() != 2) {
        std::printf("  margins_ame returned %zu rows, expected 2 <<FAIL\n", a.rows.size());
        return 1;
    }
    cmp("margins_ame probit se x1", a.rows[0].se, 0.038064606025, 1e-7);
    cmp("margins_ame probit se x2", a.rows[1].se, 0.036952040176, 1e-7);

    // Guard the hoist itself: probit's Fisher bread pr.V and the observed bread
    // must actually DIFFER here. If they ever coincide this fixture has stopped
    // exercising the non-canonical-link case and every assertion above goes
    // vacuously green — the failure mode BUG-140's audit walked into.
    double maxrel = 0;
    for (size_t i = 0; i < oi.bread.size(); ++i)
        maxrel = std::max(maxrel, std::fabs(oi.bread[i] - pr.V[i]) / (std::fabs(pr.V[i]) + 1e-30));
    std::printf("  %-34s max rel |obs - Fisher| = %.2e\n", "observed != Fisher (discriminator)", maxrel);
    if (!(maxrel > 1e-3)) {
        std::printf("    <<FAIL: the two information matrices coincide — this leg proves nothing\n");
        fail = 1;
    }

    std::printf("%s\n", fail ? "FAILED" : "PASSED");
    return fail;
}

int test_lincom_main() {
    int fail = 0;
    fail |= test_lincom_ols_golden();
    fail |= test_lincom_ml_refit_bug203();
    fail |= test_predict_ml_bug297();
    fail |= test_margins_probit_observed_info_bug210();
    return fail;
}
