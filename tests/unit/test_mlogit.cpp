// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// P7 wave — multinomial logit parity vs Stata 19.5.
//
// Oracle: internal capture p7_stata.log, section P7, run on
// internal fixture wave_fixture.dta —
//     mlogit ynom x1 x2 i.region, baseoutcome(0) vce(cluster id)
// Tolerances follow the wave rule: coefficients rtol 1e-6, standard errors rtol
// 1e-5, log-likelihood rtol 1e-6.
//
// The design matrix is rebuilt here directly from the .dta (listwise deletion on
// y/x1/x2/region/id, then i.region expanded against its lowest level as base) so
// the test exercises the ESTIMATOR, with no dependency on the CLI's loader. Column
// order mirrors load_glm's exactly — [x1, x2, 2.region … 5.region, intercept], the
// intercept LAST — because the equation blocks of θ, of the scores, and of the
// vcov are all in that same order and a transposed block would give
// wrong-but-plausible cluster SEs.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "core/linalg.h"
#include "estimators/mlogit.h"
#include "io/dta_reader.h"
#include "validate_dir.h"

using namespace cecon;

static int fail = 0;

static void chk(const char* tag, double got, double want, double rtol) {
    double rel = std::fabs(got - want) / (std::fabs(want) + 1e-30);
    std::printf("  %-22s got=%-16.10g stata=%-14.10g rel=%.2e\n", tag, got, want, rel);
    if (!(rel <= rtol)) { std::printf("    <<FAIL %s rel %.2e > %.0e\n", tag, rel, rtol); fail = 1; }
}

// Stata prints coefficients and standard errors in a NINE-CHARACTER %9.0g field.
// On this fit every mlogit coefficient is tiny (the covariates buy essentially
// nothing here — Pseudo R² prints 0.0000), so a printed value like ".0002711"
// carries only four significant digits and the LOG ITSELF cannot resolve the
// wave's 1e-6 relative rule: two numbers agreeing to 1e-4 relative print
// identically. `printed` is therefore the oracle literal exactly as it appears in
// p7_stata.log, and the assertion passes when EITHER the wave rtol holds OR our
// value rounds to those exact digits (|Δ| ≤ half a unit in the last printed
// decimal) — i.e. re-running Stata on our numbers would reproduce the log
// character for character. Every line below records which rule it met.
static void chkp(const char* tag, double got, const char* printed, double rtol) {
    const double want = std::atof(printed);
    const std::string s(printed);
    const size_t dot = s.find('.');
    const int dec = (dot == std::string::npos) ? 0 : static_cast<int>(s.size() - dot - 1);
    const double half_ulp = 0.5 * std::pow(10.0, -dec);
    const double rel = std::fabs(got - want) / (std::fabs(want) + 1e-30);
    const bool by_rtol = (rel <= rtol);
    const bool by_print = (std::fabs(got - want) <= half_ulp);
    std::printf("  %-22s got=%-16.10g stata=%-11s rel=%.2e  [%s]\n", tag, got, printed, rel,
                by_rtol ? "rtol" : (by_print ? "print-exact" : "MISS"));
    if (!(by_rtol || by_print)) {
        std::printf("    <<FAIL %s rel %.2e > %.0e and |Δ|=%.3e > half-ulp %.1e\n",
                    tag, rel, rtol, std::fabs(got - want), half_ulp);
        fail = 1;
    }
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

int test_mlogit_main() {
    std::printf("test_mlogit:\n");
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
    const double* yraw = f.col("ynom").f64.data();
    const double* x1 = f.col("x1").f64.data();
    const double* x2 = f.col("x2").f64.data();
    const double* reg = f.col("region").f64.data();
    const double* idc = f.col("id").f64.data();

    std::vector<int64_t> keep;
    for (int64_t i = 0; i < nraw; ++i) {
        if (std::isnan(yraw[i]) || std::isnan(x1[i]) || std::isnan(x2[i]) ||
            std::isnan(reg[i]) || std::isnan(idc[i])) continue;
        keep.push_back(i);
    }
    const int64_t n = static_cast<int64_t>(keep.size());
    std::printf("  N = %lld (raw %lld)\n", (long long)n, (long long)nraw);
    chk_abs("N", static_cast<double>(n), 4671, 0);

    std::vector<double> rlv;
    for (int64_t i : keep) {
        bool seen = false;
        for (double v : rlv) if (v == reg[i]) { seen = true; break; }
        if (!seen) rlv.push_back(reg[i]);
    }
    std::sort(rlv.begin(), rlv.end());
    const int nreg = static_cast<int>(rlv.size()) - 1;
    const int k = 2 + nreg + 1;  // x1, x2, region dummies, intercept LAST
    std::vector<double> X(static_cast<size_t>(n) * k, 0.0);
    for (int64_t r = 0; r < n; ++r) {
        const int64_t i = keep[static_cast<size_t>(r)];
        X[r] = x1[i];
        X[n + r] = x2[i];
        for (int j = 0; j < nreg; ++j)
            X[static_cast<int64_t>(2 + j) * n + r] = (reg[i] == rlv[static_cast<size_t>(j) + 1]) ? 1.0 : 0.0;
        X[static_cast<int64_t>(k - 1) * n + r] = 1.0;
    }
    // Outcomes recoded by sorted distinct value; baseoutcome(0) is the lowest, index 0.
    std::vector<double> ovals;
    std::vector<int> cat(static_cast<size_t>(n));
    {
        std::vector<double> yk(static_cast<size_t>(n));
        for (int64_t r = 0; r < n; ++r) yk[static_cast<size_t>(r)] = yraw[keep[static_cast<size_t>(r)]];
        ovals = yk;
        std::sort(ovals.begin(), ovals.end());
        ovals.erase(std::unique(ovals.begin(), ovals.end()), ovals.end());
        for (int64_t r = 0; r < n; ++r)
            cat[static_cast<size_t>(r)] = static_cast<int>(
                std::lower_bound(ovals.begin(), ovals.end(), yk[static_cast<size_t>(r)]) - ovals.begin());
    }
    const int K = static_cast<int>(ovals.size());
    std::printf("  K = %d outcomes (%g, %g, %g)\n", K, ovals[0], ovals[1], ovals[2]);
    chk_abs("n_cats", K, 3, 0);
    chk_abs("base outcome value", ovals[0], 0, 0);

    std::vector<int32_t> cid(static_cast<size_t>(n));
    int G = 0;
    {
        std::vector<double> seen;
        for (int64_t r = 0; r < n; ++r) {
            double v = idc[keep[static_cast<size_t>(r)]];
            int code = -1;
            for (size_t s = 0; s < seen.size(); ++s) if (seen[s] == v) { code = static_cast<int>(s); break; }
            if (code < 0) { code = static_cast<int>(seen.size()); seen.push_back(v); }
            cid[static_cast<size_t>(r)] = code;
        }
        G = static_cast<int>(seen.size());
    }
    std::printf("  G = %d clusters\n", G);
    chk_abs("n_clusters", G, 200, 0);

    const int base = 0;
    const int neq = K - 1, np = neq * k;

    // ---- 1. Gradient self-check --------------------------------------------
    // At a NON-optimal θ, the analytic gradient must match a central-difference
    // numerical gradient of the SAME log-likelihood — the check that makes a
    // block-ordering or sign error findable in minutes.
    {
        std::vector<int> eq_of_cat(static_cast<size_t>(K), -1);
        for (int m = 0, e = 0; m < K; ++m) if (m != base) eq_of_cat[static_cast<size_t>(m)] = e++;
        std::vector<double> theta(static_cast<size_t>(np), 0.0);
        for (int p = 0; p < np; ++p) theta[static_cast<size_t>(p)] = 0.17 - 0.031 * (p % 5) + 0.011 * (p / 5);
        std::vector<double> g(static_cast<size_t>(np));
        mlg::ll_grad(X.data(), n, k, cat.data(), K, eq_of_cat, theta, &g);
        double worst = 0;
        for (int p = 0; p < np; ++p) {
            double h = 1e-6 * (std::fabs(theta[static_cast<size_t>(p)]) + 1.0);
            std::vector<double> tp = theta, tm = theta;
            tp[static_cast<size_t>(p)] += h; tm[static_cast<size_t>(p)] -= h;
            double lp = mlg::ll_grad(X.data(), n, k, cat.data(), K, eq_of_cat, tp, nullptr);
            double lm = mlg::ll_grad(X.data(), n, k, cat.data(), K, eq_of_cat, tm, nullptr);
            double num = (lp - lm) / (2 * h);
            double rel = std::fabs(num - g[static_cast<size_t>(p)]) / (std::fabs(num) + 1e-8);
            if (rel > worst) worst = rel;
        }
        std::printf("  gradient self-check: worst rel = %.3e\n", worst);
        if (!(worst <= 1e-6)) { std::printf("    <<FAIL analytic gradient mismatch\n"); fail = 1; }
        // Iteration 0 is θ = 0, where every p_im = 1/K, so LL = −N·ln(K). Stata
        // prints exactly this as its own iteration 0 (-5131.6174).
        std::vector<double> z(static_cast<size_t>(np), 0.0);
        double ll0 = mlg::ll_grad(X.data(), n, k, cat.data(), K, eq_of_cat, z, nullptr);
        chk("iteration-0 LL", ll0, -5131.6174, 1e-6);
        chk("iteration-0 LL closed", ll0, -static_cast<double>(n) * std::log(3.0), 1e-12);
    }

    // ---- 2. mlogit ynom x1 x2 i.region, baseoutcome(0) vce(cluster id) ------
    MlogitResult r = mlogit_fit(X.data(), n, k, cat.data(), K, base);
    if (!r.ok) { std::printf("    <<FAIL mlogit did not fit\n"); return 1; }
    chk("mlogit ll", r.ll, -5131.4623, 1e-6);
    // Pseudo R2 = 1 − LL/LL_0 against the constants-only closed form; Stata prints
    // 0.0000 here (the covariates buy essentially nothing), so this is an absolute
    // check on a value that rounds to zero at four decimals.
    double r2 = 1.0 - r.ll / r.ll_0;
    std::printf("  pseudo R2 = %.8f (Stata prints 0.0000)\n", r2);
    if (!(std::fabs(r2) < 0.00005)) { std::printf("    <<FAIL pseudo R2 does not print as 0.0000\n"); fail = 1; }

    // ML cluster small-sample factor G/(G-1) — the P5 precedent for ML models,
    // NOT the regression-style (N-1)/(N-K)·G/(G-1). The SE assertions below are
    // what verify it: with G/(G-1) every cluster SE lands within 1e-5 relative of
    // the oracle; the regression factor would add an extra (N-1)/(N-K) = 4670/4657
    // (~0.14% on every SE), far outside the tolerance.
    std::vector<double> S = mlogit_scores(X.data(), n, k, cat.data(), r);
    std::vector<double> M(static_cast<size_t>(G) * np, 0.0);
    for (int64_t i = 0; i < n; ++i)
        for (int p = 0; p < np; ++p)
            M[static_cast<int64_t>(p) * G + cid[static_cast<size_t>(i)]] += S[static_cast<int64_t>(p) * n + i];
    std::vector<double> meat(static_cast<size_t>(np) * np, 0.0);
    la::xtx(M.data(), G, np, meat.data());
    std::vector<double> V(static_cast<size_t>(np) * np, 0.0), tmp(static_cast<size_t>(np) * np, 0.0);
    la::sandwich(r.vcov.data(), meat.data(), np, static_cast<double>(G) / (G - 1.0), V.data(), tmp.data());
    auto se = [&](int p) { return std::sqrt(V[static_cast<size_t>(p) * np + p]); };
    auto b = [&](int p) { return r.beta[static_cast<size_t>(p)]; };

    // Equation for outcome 1 (equation index 0): [x1 x2 2.region … 5.region _cons]
    std::printf("  -- outcome 1\n");
    chkp("eq1 b x1", b(0), ".0126966", 1e-6);
    chkp("eq1 b x2", b(1), ".0080817", 1e-6);
    chkp("eq1 b 2.region", b(2), ".0228758", 1e-6);
    chkp("eq1 b 3.region", b(3), ".0160928", 1e-6);
    chkp("eq1 b 4.region", b(4), ".0095639", 1e-6);
    chkp("eq1 b 5.region", b(5), "-.0030537", 1e-6);
    chkp("eq1 b _cons", b(6), "-.0195198", 1e-6);
    chkp("eq1 se x1", se(0), ".0133887", 1e-5);
    chkp("eq1 se x2", se(1), ".0131663", 1e-5);
    chkp("eq1 se 2.region", se(2), ".0179585", 1e-5);
    chkp("eq1 se 3.region", se(3), ".0183632", 1e-5);
    chkp("eq1 se 4.region", se(4), ".0192935", 1e-5);
    chkp("eq1 se 5.region", se(5), ".018062", 1e-5);
    chkp("eq1 se _cons", se(6), ".0169034", 1e-5);
    chkp("eq1 z x1", b(0) / se(0), "0.95", 1e-2);
    chkp("eq1 ci_lo x1", b(0) - 1.959963984540054 * se(0), "-.0135448", 1e-4);
    chkp("eq1 ci_hi x1", b(0) + 1.959963984540054 * se(0), ".038938", 1e-5);

    // Equation for outcome 2 (equation index 1) — block offset k.
    std::printf("  -- outcome 2\n");
    chkp("eq2 b x1", b(k + 0), "-.0044822", 1e-6);
    chkp("eq2 b x2", b(k + 1), ".0045807", 1e-6);
    chkp("eq2 b 2.region", b(k + 2), ".0130003", 1e-6);
    chkp("eq2 b 3.region", b(k + 3), ".0194065", 1e-6);
    chkp("eq2 b 4.region", b(k + 4), ".0256937", 1e-6);
    chkp("eq2 b 5.region", b(k + 5), ".0002711", 1e-5);
    chkp("eq2 b _cons", b(k + 6), "-.012615", 1e-6);
    chkp("eq2 se x1", se(k + 0), ".0130391", 1e-5);
    chkp("eq2 se x2", se(k + 1), ".0138393", 1e-5);
    chkp("eq2 se 2.region", se(k + 2), ".0194864", 1e-5);
    chkp("eq2 se 3.region", se(k + 3), ".0186616", 1e-5);
    chkp("eq2 se 4.region", se(k + 4), ".0178723", 1e-5);
    chkp("eq2 se 5.region", se(k + 5), ".0179793", 1e-5);
    chkp("eq2 se _cons", se(k + 6), ".0159286", 1e-5);

    // Wald χ² over ALL slopes of ALL equations, both constants excluded → chi2(12).
    {
        std::vector<int> idx;
        for (int e = 0; e < neq; ++e)
            for (int c = 0; c < k - 1; ++c) idx.push_back(e * k + c);
        const int q = static_cast<int>(idx.size());
        std::vector<double> Vs(static_cast<size_t>(q) * q);
        for (int i = 0; i < q; ++i)
            for (int j = 0; j < q; ++j)
                Vs[static_cast<size_t>(i) * q + j] =
                    V[static_cast<size_t>(idx[static_cast<size_t>(i)]) * np + idx[static_cast<size_t>(j)]];
        if (la::spd_inverse(Vs.data(), q) != 0) { std::printf("    <<FAIL Wald vcov not invertible\n"); fail = 1; }
        double w = 0;
        for (int i = 0; i < q; ++i)
            for (int j = 0; j < q; ++j)
                w += b(idx[static_cast<size_t>(i)]) * Vs[static_cast<size_t>(i) * q + j] * b(idx[static_cast<size_t>(j)]);
        chk_abs("df_chi2", q, 12, 0);
        chkp("mlogit chi2(12)", w, "7.50", 1e-3);
    }

    std::printf(fail ? "test_mlogit: FAIL\n" : "test_mlogit: OK\n");
    return fail;
}
