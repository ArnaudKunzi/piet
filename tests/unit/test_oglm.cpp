// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// P7 wave — ordered logit / ordered probit parity vs Stata 19.5.
//
// Oracle: internal capture p7_stata.log, section P7, run on
// internal fixture wave_fixture.dta —
//     ologit  yord x1 x2 i.region, vce(cluster id)
//     oprobit yord x1 x2 i.region, vce(robust)
// Tolerances follow the wave rule: coefficients rtol 1e-6, standard errors rtol
// 1e-5, log-likelihood rtol 1e-6.
//
// The design matrix is rebuilt here directly from the .dta (listwise deletion on
// y/x1/x2/region/id, then i.region expanded against its lowest level as base) so
// the test exercises the ESTIMATOR, with no dependency on the CLI's loader. The
// column order deliberately mirrors load_glm's — [x1, x2, 2.region … 5.region] —
// except that the ordered models carry NO intercept (it is not identified against
// the cutpoints), which is exactly what cmd_ologit/cmd_oprobit drop before fitting.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "core/linalg.h"
#include "estimators/oglm.h"
#include "estimators/posthest.h"
#include "io/dta_reader.h"
#include "validate_dir.h"

using namespace cecon;

static int fail = 0;

static void chk(const char* tag, double got, double want, double rtol) {
    double rel = std::fabs(got - want) / (std::fabs(want) + 1e-30);
    std::printf("  %-22s got=%-16.10g stata=%-14.10g rel=%.2e\n", tag, got, want, rel);
    if (!(rel <= rtol)) { std::printf("    <<FAIL %s rel %.2e > %.0e\n", tag, rel, rtol); fail = 1; }
}
static void chk_abs(const char* tag, double got, double want, double atol) {
    double d = std::fabs(got - want);
    std::printf("  %-22s got=%-16.10g stata=%-14.10g abs=%.2e\n", tag, got, want, d);
    if (!(d <= atol)) { std::printf("    <<FAIL %s abs %.2e > %.0e\n", tag, d, atol); fail = 1; }
}

// <validate>/../kitchensink/pangram/waves/_artifacts/ — the wave fixtures are
// generated Stata artifacts (gitignored), so they are resolved the same way the
// validate/ goldens are: COBRA_PANGRAM_ARTIFACTS overrides, else a path relative to
// COBRA_VALIDATE_DIR (which CMake defines to ${CMAKE_SOURCE_DIR}/tests/validate).
static std::string wave_fixture() {
    if (const char* env = std::getenv("COBRA_PANGRAM_ARTIFACTS")) {
        if (*env) {
            std::string d = env;
            if (d.back() != '/') d += '/';
            return d + "wave_fixture.dta";
        }
    }
    return cobra_test::require_fixture(
        cobra_test::validate_dir() + "../kitchensink/pangram/waves/_artifacts/wave_fixture.dta",
        "wave_fixture.dta");
}

struct Design {
    int64_t n = 0;
    int k = 0;                  // slope columns (NO intercept)
    std::vector<double> X;      // col-major n*k
    std::vector<int> cat;       // 0-based outcome category
    std::vector<double> levels; // distinct outcome values, ascending
    std::vector<int32_t> cid;   // 0-based cluster code (id)
    int n_clusters = 0;
    std::vector<std::string> names;
};

int test_oglm_main() {
    std::printf("test_oglm:\n");
    const std::string path = wave_fixture();
    DtaResult dr;
    try {
        dr = read_dta(path);
    } catch (const std::exception& e) {
        std::printf("  FAIL: cannot read %s (%s)\n", path.c_str(), e.what());
        std::printf("  set COBRA_PANGRAM_ARTIFACTS to the directory holding wave_fixture.dta\n");
        return 1;
    }
    const Frame& f = dr.frame;
    const int64_t nraw = f.n_rows();
    const double* yraw = f.col("yord").f64.data();
    const double* x1 = f.col("x1").f64.data();
    const double* x2 = f.col("x2").f64.data();
    const double* reg = f.col("region").f64.data();
    const double* idc = f.col("id").f64.data();

    // Listwise deletion over every column the model touches.
    std::vector<int64_t> keep;
    for (int64_t i = 0; i < nraw; ++i) {
        if (std::isnan(yraw[i]) || std::isnan(x1[i]) || std::isnan(x2[i]) ||
            std::isnan(reg[i]) || std::isnan(idc[i])) continue;
        keep.push_back(i);
    }
    Design d;
    d.n = static_cast<int64_t>(keep.size());
    std::printf("  N = %lld (raw %lld)\n", (long long)d.n, (long long)nraw);
    chk_abs("N", static_cast<double>(d.n), 4671, 0);

    // i.region: dummies for every level but the lowest (Stata's default base).
    std::vector<double> rlv;
    for (int64_t i : keep) {
        bool seen = false;
        for (double v : rlv) if (v == reg[i]) { seen = true; break; }
        if (!seen) rlv.push_back(reg[i]);
    }
    std::sort(rlv.begin(), rlv.end());
    const int nreg = static_cast<int>(rlv.size()) - 1;  // dummies (base dropped)
    d.k = 2 + nreg;
    d.X.assign(static_cast<size_t>(d.n) * d.k, 0.0);
    d.names.push_back("x1");
    d.names.push_back("x2");
    for (int j = 0; j < nreg; ++j)
        d.names.push_back(std::to_string(static_cast<long long>(rlv[static_cast<size_t>(j) + 1])) + ".region");
    for (int64_t r = 0; r < d.n; ++r) {
        const int64_t i = keep[static_cast<size_t>(r)];
        d.X[r] = x1[i];
        d.X[d.n + r] = x2[i];
        for (int j = 0; j < nreg; ++j)
            d.X[static_cast<int64_t>(2 + j) * d.n + r] = (reg[i] == rlv[static_cast<size_t>(j) + 1]) ? 1.0 : 0.0;
    }
    std::vector<double> yk(static_cast<size_t>(d.n));
    for (int64_t r = 0; r < d.n; ++r) yk[static_cast<size_t>(r)] = yraw[keep[static_cast<size_t>(r)]];
    d.levels = ord_categories(yk.data(), d.n, d.cat);
    std::printf("  J = %d categories\n", (int)d.levels.size());
    chk_abs("n_cats", static_cast<double>(d.levels.size()), 3, 0);

    // Cluster codes for id (dense, first-seen order — only the partition matters).
    {
        std::vector<double> seen;
        d.cid.resize(static_cast<size_t>(d.n));
        for (int64_t r = 0; r < d.n; ++r) {
            double v = idc[keep[static_cast<size_t>(r)]];
            int code = -1;
            for (size_t s = 0; s < seen.size(); ++s) if (seen[s] == v) { code = static_cast<int>(s); break; }
            if (code < 0) { code = static_cast<int>(seen.size()); seen.push_back(v); }
            d.cid[static_cast<size_t>(r)] = code;
        }
        d.n_clusters = static_cast<int>(seen.size());
    }
    std::printf("  G = %d clusters\n", d.n_clusters);
    chk_abs("n_clusters", d.n_clusters, 200, 0);

    const int J = static_cast<int>(d.levels.size());
    const int np = d.k + J - 1;

    // ---- 1. Gradient self-check --------------------------------------------
    // At a NON-optimal θ, the analytic gradient must equal a central-difference
    // numerical gradient of the SAME log-likelihood. This is what makes a sign
    // error in the κ terms findable in minutes; run it for both links.
    for (int lk = 0; lk < 2; ++lk) {
        const OrdLink link = (lk == 0) ? OrdLink::Logit : OrdLink::Probit;
        std::vector<double> theta(static_cast<size_t>(np), 0.0);
        for (int p = 0; p < d.k; ++p) theta[static_cast<size_t>(p)] = 0.13 - 0.05 * p;
        for (int c = 0; c < J - 1; ++c) theta[static_cast<size_t>(d.k + c)] = -0.4 + 0.9 * c;
        std::vector<double> g(static_cast<size_t>(np));
        ogl::ll_grad(d.X.data(), d.n, d.k, d.cat.data(), J, link, theta, &g);
        double worst = 0;
        for (int p = 0; p < np; ++p) {
            double h = 1e-6 * (std::fabs(theta[static_cast<size_t>(p)]) + 1.0);
            std::vector<double> tp = theta, tm = theta;
            tp[static_cast<size_t>(p)] += h; tm[static_cast<size_t>(p)] -= h;
            double lp = ogl::ll_grad(d.X.data(), d.n, d.k, d.cat.data(), J, link, tp, nullptr);
            double lm = ogl::ll_grad(d.X.data(), d.n, d.k, d.cat.data(), J, link, tm, nullptr);
            double num = (lp - lm) / (2 * h);
            double rel = std::fabs(num - g[static_cast<size_t>(p)]) / (std::fabs(num) + 1e-8);
            if (rel > worst) worst = rel;
        }
        std::printf("  gradient self-check (%s): worst rel = %.3e\n",
                    lk == 0 ? "logit" : "probit", worst);
        if (!(worst <= 1e-6)) { std::printf("    <<FAIL analytic gradient mismatch\n"); fail = 1; }
    }

    // Shared sandwich builder: V = factor * bread * (Σ s s') * bread over the
    // per-observation score rows. `group` < 0 means one row per observation
    // (robust); otherwise score rows are summed within a group first (cluster).
    auto sandwich_se = [&](const OglmResult& r, const std::vector<double>& S,
                           const std::vector<int32_t>* group, int ngroups, double factor) {
        const int64_t rows = group ? ngroups : d.n;
        std::vector<double> M(static_cast<size_t>(rows) * np, 0.0);
        for (int64_t i = 0; i < d.n; ++i) {
            int64_t rr = group ? (*group)[static_cast<size_t>(i)] : i;
            for (int p = 0; p < np; ++p) M[static_cast<int64_t>(p) * rows + rr] += S[static_cast<int64_t>(p) * d.n + i];
        }
        std::vector<double> meat(static_cast<size_t>(np) * np, 0.0);
        la::xtx(M.data(), rows, np, meat.data());
        std::vector<double> V(static_cast<size_t>(np) * np, 0.0), tmp(static_cast<size_t>(np) * np, 0.0);
        la::sandwich(r.vcov.data(), meat.data(), np, factor, V.data(), tmp.data());
        return V;
    };
    // Wald χ² over the SLOPES only (cutpoints excluded) — Stata's chi2(6) here.
    auto wald_slopes = [&](const OglmResult& r, const std::vector<double>& V) {
        std::vector<double> Vs(static_cast<size_t>(d.k) * d.k);
        for (int i = 0; i < d.k; ++i)
            for (int j = 0; j < d.k; ++j) Vs[static_cast<size_t>(i) * d.k + j] = V[static_cast<size_t>(i) * np + j];
        if (la::spd_inverse(Vs.data(), d.k) != 0) return std::nan("");
        double w = 0;
        for (int i = 0; i < d.k; ++i)
            for (int j = 0; j < d.k; ++j) w += r.beta[static_cast<size_t>(i)] * Vs[static_cast<size_t>(i) * d.k + j] * r.beta[static_cast<size_t>(j)];
        return w;
    };

    // ---- 2. ologit yord x1 x2 i.region, vce(cluster id) ---------------------
    {
        std::printf("  -- ologit, vce(cluster id)\n");
        OglmResult r = oglm_fit(d.X.data(), d.n, d.k, d.cat.data(), J, OrdLink::Logit);
        if (!r.ok) { std::printf("    <<FAIL ologit did not fit\n"); return 1; }
        chk("ologit ll", r.ll, -4567.8113, 1e-6);
        chk("ologit ll_0 (iter 0)", r.ll_0, -5098.6457, 1e-6);
        chk("ologit pseudo R2", 1.0 - r.ll / r.ll_0, 0.1041, 1e-3);
        chk("ologit b x1", r.beta[0], 1.210794, 1e-6);
        chk("ologit b x2", r.beta[1], -.3371372, 1e-6);
        chk("ologit b 2.region", r.beta[2], .1144187, 1e-6);
        chk("ologit b 3.region", r.beta[3], .0721595, 1e-6);
        chk("ologit b 4.region", r.beta[4], .0796058, 1e-6);
        chk("ologit b 5.region", r.beta[5], .0717107, 1e-6);
        chk("ologit /cut1", r.cuts[0], -.5329718, 1e-6);
        chk("ologit /cut2", r.cuts[1], 1.125396, 1e-6);

        // ML cluster small-sample factor G/(G-1) — the P5 precedent for ML models
        // (logit/probit/tobit), NOT the regression-style (N-1)/(N-K)·G/(G-1). The
        // assertions below are what verify it: with G/(G-1) every cluster SE lands
        // within 1e-5 relative of the oracle; the regression factor would be off by
        // ~0.06% (it multiplies by an extra (N-1)/(N-K) = 4670/4663).
        std::vector<double> S = oglm_scores(d.X.data(), d.n, d.k, d.cat.data(), J, OrdLink::Logit, r);
        double factor = static_cast<double>(d.n_clusters) / (d.n_clusters - 1.0);
        std::vector<double> V = sandwich_se(r, S, &d.cid, d.n_clusters, factor);
        auto se = [&](int p) { return std::sqrt(V[static_cast<size_t>(p) * np + p]); };
        chk("ologit se x1", se(0), .1164798, 1e-5);
        chk("ologit se x2", se(1), .0959604, 1e-5);
        chk("ologit se 2.region", se(2), .143534, 1e-5);
        chk("ologit se 3.region", se(3), .1409787, 1e-5);
        chk("ologit se 4.region", se(4), .1573098, 1e-5);
        chk("ologit se 5.region", se(5), .151756, 1e-5);
        chk("ologit se /cut1", se(6), .1222087, 1e-5);
        chk("ologit se /cut2", se(7), .1438165, 1e-5);
        chk("ologit z x1", r.beta[0] / se(0), 10.39, 1e-3);
        chk("ologit chi2(6)", wald_slopes(r, V), 111.98, 1e-4);
        // 95% CI on x1 as Stata prints it (asymptotic z).
        chk("ologit ci_lo x1", r.beta[0] - 1.959963984540054 * se(0), .9824974, 1e-5);
        chk("ologit ci_hi x1", r.beta[0] + 1.959963984540054 * se(0), 1.43909, 1e-5);
        chk("ologit ci_lo /cut1", r.cuts[0] - 1.959963984540054 * se(6), -.7724964, 1e-5);
        chk("ologit ci_hi /cut2", r.cuts[1] + 1.959963984540054 * se(7), 1.407271, 1e-5);
    }

    // ---- 3. oprobit yord x1 x2 i.region, vce(robust) ------------------------
    {
        std::printf("  -- oprobit, vce(robust)\n");
        OglmResult r = oglm_fit(d.X.data(), d.n, d.k, d.cat.data(), J, OrdLink::Probit);
        if (!r.ok) { std::printf("    <<FAIL oprobit did not fit\n"); return 1; }
        chk("oprobit ll", r.ll, -4562.8861, 1e-6);
        // Same value as ologit's: the cutpoints-only log-likelihood is LINK-FREE
        // (Σ_j n_j ln(n_j/n)), which is why Stata prints -5098.6457 as iteration 0
        // for BOTH models — a strong check that the category coding matches.
        chk("oprobit ll_0 (iter 0)", r.ll_0, -5098.6457, 1e-6);
        chk("oprobit pseudo R2", 1.0 - r.ll / r.ll_0, 0.1051, 1e-3);
        chk("oprobit b x1", r.beta[0], .7300159, 1e-6);
        chk("oprobit b x2", r.beta[1], -.2074426, 1e-6);
        chk("oprobit b 2.region", r.beta[2], .0586603, 1e-6);
        chk("oprobit b 3.region", r.beta[3], .0269848, 1e-6);
        chk("oprobit b 4.region", r.beta[4], .0341804, 1e-6);
        chk("oprobit b 5.region", r.beta[5], .0388351, 1e-6);
        chk("oprobit /cut1", r.cuts[0], -.337268, 1e-6);
        chk("oprobit /cut2", r.cuts[1], .6582166, 1e-6);

        // ML vce(robust) small-sample factor N/(N-1) — same P5 precedent. Verified
        // by the SE assertions below (the regression-style N/(N-K) would move every
        // SE by ~0.09%, far outside the 1e-5 tolerance).
        std::vector<double> S = oglm_scores(d.X.data(), d.n, d.k, d.cat.data(), J, OrdLink::Probit, r);
        double factor = static_cast<double>(d.n) / (static_cast<double>(d.n) - 1.0);
        std::vector<double> V = sandwich_se(r, S, nullptr, 0, factor);
        auto se = [&](int p) { return std::sqrt(V[static_cast<size_t>(p) * np + p]); };
        chk("oprobit se x1", se(0), .0234468, 1e-5);
        chk("oprobit se x2", se(1), .0225871, 1e-5);
        chk("oprobit se 2.region", se(2), .053538, 1e-5);
        chk("oprobit se 3.region", se(3), .0548626, 1e-5);
        chk("oprobit se 4.region", se(4), .0542664, 1e-5);
        chk("oprobit se 5.region", se(5), .0532149, 1e-5);
        chk("oprobit se /cut1", se(6), .0425214, 1e-5);
        chk("oprobit se /cut2", se(7), .0441141, 1e-5);
        chk("oprobit z x1", r.beta[0] / se(0), 31.14, 1e-3);
        chk("oprobit chi2(6)", wald_slopes(r, V), 1036.11, 1e-4);
        chk("oprobit ci_lo x1", r.beta[0] - 1.959963984540054 * se(0), .6840611, 1e-5);
        chk("oprobit ci_hi x1", r.beta[0] + 1.959963984540054 * se(0), .7759707, 1e-5);
    }

    std::printf(fail ? "test_oglm: FAIL\n" : "test_oglm: OK\n");
    return fail;
}
