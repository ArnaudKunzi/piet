// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// P7 wave — ivprobit / ivtobit parity vs Stata 19.5.
//
// Oracle: internal capture p7_stata.log, section P7, run on
// internal fixture wave_fixture.dta —
//     ivprobit ybin x1 (endog=z), vce(robust)
//     ivtobit  y    x1 (endog=z), ll(0) vce(robust)
//
// Tolerances follow the wave rule (coefficients rtol 1e-6, SEs rtol 1e-5, LL rtol
// 1e-6) PLUS 5e-8 — half the last digit Stata's %9.7f coef/SE format prints. The
// two terms add rather than max() because they are independent error sources: the
// oracle value we compare against is already quantized to ±5e-8 by the printing,
// and the wave rtol is the numerical-agreement budget on top of that. A rel-only
// rule is outright unsatisfiable for a small-magnitude entry like sd(e.endog)'s SE
// .0036092, whose print granularity is 1.4e-5 in relative terms; and a max() rule
// is unsatisfiable for `ivtobit corr`, whose converged value −.0069774500 sits
// EXACTLY on the .0069774/.0069775 rounding boundary (tightening our convergence
// test from 1e-9 to 1e-14 moves it by 1e-10 — we are at the optimum; the residual
// is Stata's own last-digit). The slack this buys is still ~100× smaller than any
// of the failure modes these assertions exist to catch: dropping the N/(N−1)
// factor moves se(endog) by 4e-6 against a tolerance of 4.4e-7.
//
// Iteration counts are deliberately NOT asserted — our Newton path is our own.
//
// The designs are rebuilt here directly from the .dta (listwise deletion over
// exactly the columns each command touches) so the test exercises the ESTIMATOR
// with no dependency on the CLI's loader. Note the two samples differ: ivprobit
// never touches `y`, so it keeps all 4,800 rows while ivtobit drops y's 22
// missings down to 4,778 — the oracle prints both numbers and the test pins them.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "core/distributions.h"
#include "core/linalg.h"
#include "estimators/ivml.h"
#include "estimators/posthest.h"
#include "io/dta_reader.h"
#include "validate_dir.h"

using namespace cecon;

static int fail = 0;

// tol = rtol*|want| + 5e-8 — see the header note on the print quantum.
static void chk(const char* tag, double got, double want, double rtol) {
    const double tol = rtol * std::fabs(want) + 5e-8;
    const double d = std::fabs(got - want);
    std::printf("  %-26s got=%-16.10g stata=%-14.10g |d|=%.2e tol=%.2e\n", tag, got, want, d, tol);
    if (!(d <= tol)) { std::printf("    <<FAIL %s |d| %.2e > %.2e\n", tag, d, tol); fail = 1; }
}
static void chk_abs(const char* tag, double got, double want, double atol) {
    const double d = std::fabs(got - want);
    std::printf("  %-26s got=%-16.10g stata=%-14.10g abs=%.2e\n", tag, got, want, d);
    if (!(d <= atol)) { std::printf("    <<FAIL %s abs %.2e > %.0e\n", tag, d, atol); fail = 1; }
}

// <validate>/../kitchensink/pangram/waves/_artifacts/ — the wave fixtures are
// generated Stata artifacts (gitignored), so they are resolved the same way the
// validate/ goldens are: COBRA_PANGRAM_ARTIFACTS overrides, else a path relative
// to COBRA_VALIDATE_DIR (which CMake defines to ${CMAKE_SOURCE_DIR}/tests/validate).
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

// One command's listwise sample and its two designs.
//   Xs = [y2, x1, 1]  (ks = 3)  — structural, endogenous regressor FIRST, cons LAST
//   Xw = [x1, z,  1]  (kw = 3)  — reduced form, "Exogenous: x1 z" in that order
struct Design {
    int64_t n = 0;
    int ks = 3, kw = 3;
    std::vector<double> Xs, Xw, y1, y2;
};

static Design build(const Frame& f, const char* yname) {
    const int64_t nraw = f.n_rows();
    const double* yv = f.col(yname).f64.data();
    const double* e = f.col("endog").f64.data();
    const double* x1 = f.col("x1").f64.data();
    const double* z = f.col("z").f64.data();
    std::vector<int64_t> keep;
    for (int64_t i = 0; i < nraw; ++i) {
        if (std::isnan(yv[i]) || std::isnan(e[i]) || std::isnan(x1[i]) || std::isnan(z[i])) continue;
        keep.push_back(i);
    }
    Design d;
    d.n = static_cast<int64_t>(keep.size());
    d.Xs.assign(static_cast<size_t>(d.n) * d.ks, 0.0);
    d.Xw.assign(static_cast<size_t>(d.n) * d.kw, 0.0);
    d.y1.resize(static_cast<size_t>(d.n));
    d.y2.resize(static_cast<size_t>(d.n));
    for (int64_t r = 0; r < d.n; ++r) {
        const int64_t i = keep[static_cast<size_t>(r)];
        d.y1[static_cast<size_t>(r)] = yv[i];
        d.y2[static_cast<size_t>(r)] = e[i];
        d.Xs[r] = e[i];
        d.Xs[d.n + r] = x1[i];
        d.Xs[2 * d.n + r] = 1.0;
        d.Xw[r] = x1[i];
        d.Xw[d.n + r] = z[i];
        d.Xw[2 * d.n + r] = 1.0;
    }
    return d;
}

// V = factor · bread · (Σ_i s_i s_i') · bread — vce(robust) over the FULL θ.
static std::vector<double> robust_v(const IvmlResult& r, const std::vector<double>& S, int64_t n,
                                    int np, double factor) {
    std::vector<double> meat(static_cast<size_t>(np) * np, 0.0);
    la::xtx(S.data(), n, np, meat.data());
    std::vector<double> V(static_cast<size_t>(np) * np, 0.0), tmp(static_cast<size_t>(np) * np, 0.0);
    la::sandwich(r.vcov.data(), meat.data(), np, factor, V.data(), tmp.data());
    return V;
}

// Wald χ² over the structural SLOPES only — [endog, x1], excluding the constant,
// which is exactly the chi2(2) Stata prints for a 3-column structural design.
static double wald_slopes(const IvmlResult& r, const std::vector<double>& V, int np, int nslope) {
    std::vector<double> Vs(static_cast<size_t>(nslope) * nslope);
    for (int i = 0; i < nslope; ++i)
        for (int j = 0; j < nslope; ++j)
            Vs[static_cast<size_t>(i) * nslope + j] = V[static_cast<size_t>(i) * np + j];
    if (la::spd_inverse(Vs.data(), nslope) != 0) return std::nan("");
    double w = 0;
    for (int i = 0; i < nslope; ++i)
        for (int j = 0; j < nslope; ++j)
            w += r.beta[static_cast<size_t>(i)] * Vs[static_cast<size_t>(i) * nslope + j] *
                 r.beta[static_cast<size_t>(j)];
    return w;
}

int test_ivml_main() {
    std::printf("test_ivml:\n");
    const std::string path = wave_fixture();
    DtaResult dr;
    try {
        dr = read_dta(path);
    } catch (const std::exception& ex) {
        std::printf("  FAIL: cannot read %s (%s)\n", path.c_str(), ex.what());
        std::printf("  set COBRA_PANGRAM_ARTIFACTS to the directory holding wave_fixture.dta\n");
        return 1;
    }
    const Frame& f = dr.frame;
    const double zc = z_critical(0.05);  // 1.959963985 — Stata's 95% normal critical

    Design dp = build(f, "ybin");  // ivprobit sample
    Design dt = build(f, "y");     // ivtobit sample
    std::printf("  N(ivprobit) = %lld, N(ivtobit) = %lld\n", (long long)dp.n, (long long)dt.n);
    chk_abs("N ivprobit", static_cast<double>(dp.n), 4800, 0);
    chk_abs("N ivtobit", static_cast<double>(dt.n), 4778, 0);

    // ---- 1. Gradient self-checks (written BEFORE any oracle chase) -----------
    // At a deliberately NON-optimal θ — in particular athrho ≠ 0, where atanh's
    // double appearance in m (numerator AND the 1/√(1−ρ²) scale) actually bites —
    // the analytic gradient must equal a central difference of the same LL.
    {
        const int np = dp.ks + dp.kw + 2;
        std::vector<double> th(static_cast<size_t>(np), 0.0);
        for (int p = 0; p < dp.ks; ++p) th[static_cast<size_t>(p)] = 0.21 - 0.08 * p;
        for (int p = 0; p < dp.kw; ++p) th[static_cast<size_t>(dp.ks + p)] = 0.35 - 0.11 * p;
        th[static_cast<size_t>(dp.ks + dp.kw)] = 0.42;       // athrho (ρ ≈ 0.397)
        th[static_cast<size_t>(dp.ks + dp.kw + 1)] = -0.25;  // lnσ_v
        std::vector<double> g(static_cast<size_t>(np));
        ivm::ivprobit_ll_grad(dp.Xs.data(), dp.Xw.data(), dp.n, dp.ks, dp.kw, dp.y1.data(),
                              dp.y2.data(), th, &g);
        double worst = 0;
        for (int p = 0; p < np; ++p) {
            const double h = 1e-6 * (std::fabs(th[static_cast<size_t>(p)]) + 1.0);
            std::vector<double> tp = th, tm = th;
            tp[static_cast<size_t>(p)] += h;
            tm[static_cast<size_t>(p)] -= h;
            const double lp = ivm::ivprobit_ll_grad(dp.Xs.data(), dp.Xw.data(), dp.n, dp.ks, dp.kw,
                                                    dp.y1.data(), dp.y2.data(), tp, nullptr);
            const double lm = ivm::ivprobit_ll_grad(dp.Xs.data(), dp.Xw.data(), dp.n, dp.ks, dp.kw,
                                                    dp.y1.data(), dp.y2.data(), tm, nullptr);
            const double num = (lp - lm) / (2 * h);
            const double rel = std::fabs(num - g[static_cast<size_t>(p)]) / (std::fabs(num) + 1e-8);
            if (rel > worst) worst = rel;
        }
        std::printf("  gradient self-check (ivprobit): worst rel = %.3e\n", worst);
        if (!(worst <= 1e-6)) { std::printf("    <<FAIL analytic gradient mismatch\n"); fail = 1; }
    }
    {
        const int np = dt.ks + dt.kw + 3;
        std::vector<int8_t> cls(static_cast<size_t>(dt.n), 0);
        for (int64_t i = 0; i < dt.n; ++i)
            if (dt.y1[static_cast<size_t>(i)] <= 0.0) cls[static_cast<size_t>(i)] = 1;
        std::vector<double> th(static_cast<size_t>(np), 0.0);
        for (int p = 0; p < dt.ks; ++p) th[static_cast<size_t>(p)] = 0.31 - 0.09 * p;
        for (int p = 0; p < dt.kw; ++p) th[static_cast<size_t>(dt.ks + p)] = 0.28 - 0.07 * p;
        th[static_cast<size_t>(dt.ks + dt.kw)] = -0.37;      // athrho (ρ ≈ −0.354)
        th[static_cast<size_t>(dt.ks + dt.kw + 1)] = -0.31;  // lnσ_v
        th[static_cast<size_t>(dt.ks + dt.kw + 2)] = 0.14;   // lnσ_e
        auto LL = [&](const std::vector<double>& t, std::vector<double>* g) {
            return ivm::ivtobit_ll_grad(dt.Xs.data(), dt.Xw.data(), dt.n, dt.ks, dt.kw, dt.y1.data(),
                                        dt.y2.data(), cls.data(), 0.0, 0.0, t, g);
        };
        std::vector<double> g(static_cast<size_t>(np));
        LL(th, &g);
        double worst = 0;
        for (int p = 0; p < np; ++p) {
            const double h = 1e-6 * (std::fabs(th[static_cast<size_t>(p)]) + 1.0);
            std::vector<double> tp = th, tm = th;
            tp[static_cast<size_t>(p)] += h;
            tm[static_cast<size_t>(p)] -= h;
            const double num = (LL(tp, nullptr) - LL(tm, nullptr)) / (2 * h);
            const double rel = std::fabs(num - g[static_cast<size_t>(p)]) / (std::fabs(num) + 1e-8);
            if (rel > worst) worst = rel;
        }
        std::printf("  gradient self-check (ivtobit):  worst rel = %.3e\n", worst);
        if (!(worst <= 1e-6)) { std::printf("    <<FAIL analytic gradient mismatch\n"); fail = 1; }
    }

    // ---- 2. ivprobit ybin x1 (endog=z), vce(robust) --------------------------
    {
        std::printf("  -- ivprobit ybin x1 (endog=z), vce(robust)\n");
        IvmlResult r = ivprobit_fit(dp.Xs.data(), dp.Xw.data(), dp.n, dp.ks, dp.kw, dp.y1.data(),
                                    dp.y2.data());
        if (!r.ok) { std::printf("    <<FAIL ivprobit did not fit\n"); return 1; }
        const int np = r.np;
        chk("ivprobit ll", r.ll, -8085.4702, 1e-6);
        chk("ivprobit b endog", r.beta[0], .1204481, 1e-6);
        chk("ivprobit b x1", r.beta[1], .6496528, 1e-6);
        chk("ivprobit b _cons", r.beta[2], -.4503476, 1e-6);

        // SMALL-SAMPLE FACTOR. Stata's ML vce(robust) multiplier N/(N−1) — the P5
        // precedent shared by logit/probit/tobit (cmds_mle.cpp) and by the P7
        // ordered models — NOT the regression-style (N−1)/(N−K). Verified, not
        // assumed: with N/(N−1) every SE below matches to <1e-5 relative, and the
        // unfactored sandwich (factor 1) is off by √(N/(N−1)) ≈ 1.000104, i.e.
        // se(endog) would read .0386537 against the oracle's .0386577 — a 1.0e-4
        // relative miss that this assertion set rejects.
        const double factor = static_cast<double>(dp.n) / (static_cast<double>(dp.n) - 1.0);
        std::vector<double> S = ivprobit_scores(dp.Xs.data(), dp.Xw.data(), dp.n, dp.ks, dp.kw,
                                                dp.y1.data(), dp.y2.data(), r);
        std::vector<double> V = robust_v(r, S, dp.n, np, factor);
        auto sd = [&](int p) { return std::sqrt(std::max(0.0, V[static_cast<size_t>(p) * np + p])); };
        chk("ivprobit se endog", sd(0), .0386577, 1e-5);
        chk("ivprobit se x1", sd(1), .0300313, 1e-5);
        chk("ivprobit se _cons", sd(2), .0280869, 1e-5);
        chk("ivprobit z endog", r.beta[0] / sd(0), 3.12, 2e-3);
        chk("ivprobit z x1", r.beta[1] / sd(1), 21.63, 2e-3);
        chk("ivprobit z _cons", r.beta[2] / sd(2), -16.03, 2e-3);

        // corr(e.endog,e.ybin): ρ = tanh(athrho), se(ρ) = (1−ρ²)·se(athrho), and a
        // CI that is the TANH-TRANSFORMED interval tanh(athrho ± z·se_athrho) — NOT
        // ρ ± z·se(ρ). The oracle's asymmetry around −.0721692 (−.1364806 is .0643
        // below, −.0072521 is .0649 above) is the pin that separates the two.
        const double a = r.athrho, sa = sd(r.i_athrho);
        const double rho = std::tanh(a), se_rho = (1.0 - rho * rho) * sa;
        chk("ivprobit corr", rho, -.0721692, 1e-6);
        chk("ivprobit se corr", se_rho, .0330128, 1e-5);
        chk("ivprobit corr ci_lo", std::tanh(a - zc * sa), -.1364806, 1e-5);
        chk("ivprobit corr ci_hi", std::tanh(a + zc * sa), -.0072521, 1e-5);

        // sd(e.endog) = exp(lnσ_v), se = σ_v·se(lnσ_v), CI = exp(lnσ_v ± z·se(lnσ_v)).
        const double lv = r.theta[static_cast<size_t>(r.i_lnsv)], slv = sd(r.i_lnsv);
        chk("ivprobit sd(e.endog)", std::exp(lv), .7072413, 1e-6);
        chk("ivprobit se sd(e.endog)", std::exp(lv) * slv, .0036092, 1e-5);
        chk("ivprobit sd ci_lo", std::exp(lv - zc * slv), .7002027, 1e-5);
        chk("ivprobit sd ci_hi", std::exp(lv + zc * slv), .7143506, 1e-5);

        const double w = wald_slopes(r, V, np, 2);
        chk_abs("ivprobit Wald chi2(2)", w, 693.95, 5e-3);
        chk_abs("ivprobit Wald p", chi2_pvalue(w, 2), 0.0000, 5e-5);
        const double wex = (a / sa) * (a / sa);
        chk_abs("ivprobit exog chi2(1)", wex, 4.75, 5e-3);
        chk_abs("ivprobit exog p", chi2_pvalue(wex, 1), 0.0294, 5e-5);
    }

    // ---- 3. ivtobit y x1 (endog=z), ll(0) vce(robust) ------------------------
    {
        std::printf("  -- ivtobit y x1 (endog=z), ll(0) vce(robust)\n");
        IvmlResult r = ivtobit_fit(dt.Xs.data(), dt.Xw.data(), dt.n, dt.ks, dt.kw, dt.y1.data(),
                                   dt.y2.data(), true, 0.0, false, 0.0);
        if (!r.ok) { std::printf("    <<FAIL ivtobit did not fit\n"); return 1; }
        const int np = r.np;
        chk_abs("ivtobit uncensored", static_cast<double>(r.n_unc), 4777, 0);
        chk_abs("ivtobit left-censored", static_cast<double>(r.n_lc), 1, 0);
        chk_abs("ivtobit right-censored", static_cast<double>(r.n_rc), 0, 0);
        chk("ivtobit ll", r.ll, -11714.072, 1e-6);
        chk("ivtobit b endog", r.beta[0], .0127551, 1e-6);
        chk("ivtobit b x1", r.beta[1], .7303191, 1e-6);
        chk("ivtobit b _cons", r.beta[2], 2.777327, 1e-6);

        const double factor = static_cast<double>(dt.n) / (static_cast<double>(dt.n) - 1.0);
        std::vector<double> S = ivtobit_scores(dt.Xs.data(), dt.Xw.data(), dt.n, dt.ks, dt.kw,
                                               dt.y1.data(), dt.y2.data(), true, 0.0, false, 0.0, r);
        std::vector<double> V = robust_v(r, S, dt.n, np, factor);
        auto sd = [&](int p) { return std::sqrt(std::max(0.0, V[static_cast<size_t>(p) * np + p])); };
        chk("ivtobit se endog", sd(0), .0282842, 1e-5);
        chk("ivtobit se x1", sd(1), .0218938, 1e-5);
        chk("ivtobit se _cons", sd(2), .0194398, 1e-5);
        chk("ivtobit z endog", r.beta[0] / sd(0), 0.45, 1e-2);
        chk("ivtobit z x1", r.beta[1] / sd(1), 33.36, 2e-3);
        chk("ivtobit z _cons", r.beta[2] / sd(2), 142.87, 2e-3);

        const double a = r.athrho, sa = sd(r.i_athrho);
        const double rho = std::tanh(a), se_rho = (1.0 - rho * rho) * sa;
        chk("ivtobit corr", rho, -.0069774, 1e-6);
        chk("ivtobit se corr", se_rho, .0250603, 1e-5);
        chk("ivtobit corr ci_lo", std::tanh(a - zc * sa), -.0560384, 1e-5);
        chk("ivtobit corr ci_hi", std::tanh(a + zc * sa), .0421171, 1e-5);

        const double le = r.theta[static_cast<size_t>(r.i_lnse)], sle = sd(r.i_lnse);
        chk("ivtobit sd(e.y)", std::exp(le), .9611126, 1e-6);
        chk("ivtobit se sd(e.y)", std::exp(le) * sle, .0080498, 1e-5);
        chk("ivtobit sd(e.y) ci_lo", std::exp(le - zc * sle), .945464, 1e-5);
        chk("ivtobit sd(e.y) ci_hi", std::exp(le + zc * sle), .9770202, 1e-5);

        const double lv = r.theta[static_cast<size_t>(r.i_lnsv)], slv = sd(r.i_lnsv);
        chk("ivtobit sd(e.endog)", std::exp(lv), .7071298, 1e-6);
        chk("ivtobit se sd(e.endog)", std::exp(lv) * slv, .0036182, 1e-5);
        chk("ivtobit sd(e.endog) lo", std::exp(lv - zc * slv), .7000737, 1e-5);
        chk("ivtobit sd(e.endog) hi", std::exp(lv + zc * slv), .714257, 1e-5);

        const double w = wald_slopes(r, V, np, 2);
        chk_abs("ivtobit Wald chi2(2)", w, 1624.42, 5e-3);
        chk_abs("ivtobit Wald p", chi2_pvalue(w, 2), 0.0000, 5e-5);
        const double wex = (a / sa) * (a / sa);
        chk_abs("ivtobit exog chi2(1)", wex, 0.08, 5e-3);
        chk_abs("ivtobit exog p", chi2_pvalue(wex, 1), 0.7807, 5e-5);
    }

    std::printf("test_ivml: %s\n", fail ? "FAIL" : "OK");
    return fail;
}
