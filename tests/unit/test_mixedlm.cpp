// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// P7 wave — random-intercept multilevel ML (`mixed`) parity vs Stata 19.5.
//
// Three layers, in this order deliberately:
//
//   1. MECHANISM, on synthetic data with an INDEPENDENT reference — the analytic
//      gradient against a central-difference numerical gradient, and the Woodbury
//      closed-form log-likelihood against a brute-force dense V_j = σ_e²I + σ_u²11'
//      built and Cholesky-factored here from scratch. These need no fixture and run
//      everywhere. They are what makes a sign error in the ∂/∂lnσ terms findable in
//      minutes instead of by staring at a wrong 4th digit.
//
//   2. ORACLE, against the P7 answerbook (internal capture
//      p7_stata.log, `mixed y x1 x2 || id:, vce(robust)` on wave_fixture.dta).
//      That fixture is a Stata-produced artifact and is NOT in git, so this layer
//      SKIPS with a loud note when it is absent rather than passing quietly — a
//      skipped oracle must never read as a satisfied one.
//
//   3. FALSIFICATION of the two things that were hypotheses rather than derivations:
//      the G/(G−1) small-sample factor and the LOGNORMAL variance-component CI. Each
//      is checked BOTH ways — the adopted rule must match the oracle and the
//      plausible alternative must visibly not.
//
// Tolerances follow the wave rule: coefficients rtol 1e-6, standard errors rtol
// 1e-5, log-likelihood rtol 1e-6. Stata's iteration counts are deliberately NOT
// asserted (a different but equally converged optimizer path is not a parity break).
//
// The design matrix is rebuilt here directly from the .dta (listwise deletion on
// y/x1/x2/id) so the test exercises the ESTIMATOR with no dependency on the CLI
// loader. Column order mirrors load_glm's: [x1, x2, intercept] with the constant
// LAST — which is what cmd_mixed hands mixedlm_fit.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "core/linalg.h"
#include "estimators/mixedlm.h"
#include "io/dta_reader.h"
#include "validate_dir.h"

using namespace cecon;

static int fail = 0;

static void chk(const char* tag, double got, double want, double rtol) {
    const double rel = std::fabs(got - want) / (std::fabs(want) + 1e-30);
    std::printf("  %-26s got=%-16.10g stata=%-14.10g rel=%.2e\n", tag, got, want, rel);
    if (!(rel <= rtol)) { std::printf("    <<FAIL %s rel %.2e > %.0e\n", tag, rel, rtol); fail = 1; }
}
static void chk_abs(const char* tag, double got, double want, double atol) {
    const double d = std::fabs(got - want);
    std::printf("  %-26s got=%-16.10g want=%-14.10g abs=%.2e\n", tag, got, want, d);
    if (!(d <= atol)) { std::printf("    <<FAIL %s abs %.2e > %.0e\n", tag, d, atol); fail = 1; }
}
static void chk_true(const char* tag, bool ok) {
    std::printf("  %-26s %s\n", tag, ok ? "PASS" : "FAIL");
    if (!ok) fail = 1;
}

// --- independent reference: dense V_j, no Woodbury anywhere -------------------
// Builds each group's n_j × n_j covariance explicitly, factors it by a Cholesky
// written here (not the shared linalg), and accumulates
//   LL = −½[N log2π + Σ_j log|V_j| + Σ_j e_j'V_j⁻¹e_j].
// Nothing in this function shares a line of code with mxd::ll_grad, so agreement
// between the two is real evidence that the closed forms are right — independent of
// any oracle.
static double brute_ll(const std::vector<double>& X, int64_t n, int k, const std::vector<double>& y,
                       const std::vector<int32_t>& gcode, int G, const std::vector<double>& theta) {
    const double su2 = std::exp(2.0 * theta[static_cast<size_t>(k)]);
    const double se2 = std::exp(2.0 * theta[static_cast<size_t>(k) + 1]);
    const double log2pi = 1.8378770664093454836;
    std::vector<std::vector<double>> ge(static_cast<size_t>(G));
    for (int64_t i = 0; i < n; ++i) {
        double xb = 0;
        for (int p = 0; p < k; ++p) xb += X[static_cast<size_t>(p) * n + i] * theta[static_cast<size_t>(p)];
        ge[static_cast<size_t>(gcode[static_cast<size_t>(i)])].push_back(y[static_cast<size_t>(i)] - xb);
    }
    double LL = -0.5 * static_cast<double>(n) * log2pi;
    for (int j = 0; j < G; ++j) {
        const int m = static_cast<int>(ge[static_cast<size_t>(j)].size());
        if (m == 0) continue;
        std::vector<double> V(static_cast<size_t>(m) * m);
        for (int a = 0; a < m; ++a)
            for (int b = 0; b < m; ++b)
                V[static_cast<size_t>(a) * m + b] = su2 + ((a == b) ? se2 : 0.0);
        std::vector<double> L(static_cast<size_t>(m) * m, 0.0);
        for (int a = 0; a < m; ++a)
            for (int b = 0; b <= a; ++b) {
                double s = V[static_cast<size_t>(a) * m + b];
                for (int t = 0; t < b; ++t)
                    s -= L[static_cast<size_t>(a) * m + t] * L[static_cast<size_t>(b) * m + t];
                L[static_cast<size_t>(a) * m + b] =
                    (a == b) ? std::sqrt(s) : s / L[static_cast<size_t>(b) * m + b];
            }
        double logdet = 0;
        for (int a = 0; a < m; ++a) logdet += 2.0 * std::log(L[static_cast<size_t>(a) * m + a]);
        std::vector<double> z(static_cast<size_t>(m));
        for (int a = 0; a < m; ++a) {
            double s = ge[static_cast<size_t>(j)][static_cast<size_t>(a)];
            for (int t = 0; t < a; ++t) s -= L[static_cast<size_t>(a) * m + t] * z[static_cast<size_t>(t)];
            z[static_cast<size_t>(a)] = s / L[static_cast<size_t>(a) * m + a];
        }
        double q = 0;
        for (int a = 0; a < m; ++a) q += z[static_cast<size_t>(a)] * z[static_cast<size_t>(a)];
        LL += -0.5 * (logdet + q);
    }
    return LL;
}

// Analytic gradient vs central differences of the SAME log-likelihood, at a θ that
// is deliberately NOT the optimum (at the optimum every gradient is ~0 and the check
// is vacuous). Returns the worst relative discrepancy.
static double gradient_selfcheck(const std::vector<double>& X, int64_t n, int k,
                                 const std::vector<double>& y, const std::vector<int32_t>& gcode,
                                 int G, const std::vector<double>& theta) {
    const int np = k + 2;
    // θ carries k slopes plus BOTH log-scale parameters. A short θ here would run the
    // perturbation loop off the end of the vector — caught the hard way once, so the
    // shape is asserted rather than trusted.
    if (static_cast<int>(theta.size()) != np) {
        std::printf("    <<FAIL gradient self-check called with |theta|=%d, expected %d\n",
                    static_cast<int>(theta.size()), np);
        fail = 1;
        return 1e30;
    }
    std::vector<double> g(static_cast<size_t>(np));
    mxd::ll_grad(X.data(), n, k, y.data(), gcode.data(), G, theta, &g);
    double worst = 0;
    for (int p = 0; p < np; ++p) {
        const double h = 1e-6 * (std::fabs(theta[static_cast<size_t>(p)]) + 1.0);
        std::vector<double> tp = theta, tm = theta;
        tp[static_cast<size_t>(p)] += h;
        tm[static_cast<size_t>(p)] -= h;
        const double lp = mxd::ll_grad(X.data(), n, k, y.data(), gcode.data(), G, tp, nullptr);
        const double lm = mxd::ll_grad(X.data(), n, k, y.data(), gcode.data(), G, tm, nullptr);
        const double num = (lp - lm) / (2 * h);
        const double rel = std::fabs(num - g[static_cast<size_t>(p)]) / (std::fabs(num) + 1e-8);
        worst = std::max(worst, rel);
    }
    return worst;
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

// --- 1. mechanism ------------------------------------------------------------

static void test_mechanism() {
    std::printf("  -- mechanism (synthetic balanced panel, no fixture needed)\n");
    // 6 groups × 4 observations, k = 2 ([x, intercept] — constant LAST, as load_glm
    // hands it over). Deterministic values; the point is an exactly reproducible
    // design, not a realistic one.
    const int G = 6, m = 4, k = 2;
    const int64_t n = static_cast<int64_t>(G) * m;
    std::vector<double> X(static_cast<size_t>(n) * k, 1.0), y(static_cast<size_t>(n));
    std::vector<int32_t> gcode(static_cast<size_t>(n));
    for (int j = 0; j < G; ++j)
        for (int t = 0; t < m; ++t) {
            const int64_t i = static_cast<int64_t>(j) * m + t;
            gcode[static_cast<size_t>(i)] = j;
            const double x = 0.3 * j - 0.45 * t + 0.17 * ((i * 7) % 5);
            X[static_cast<size_t>(i)] = x;                       // column 0: x
            X[static_cast<size_t>(n) + static_cast<size_t>(i)] = 1.0;  // column 1: intercept
            y[static_cast<size_t>(i)] = 1.4 + 0.8 * x + 0.5 * (j - 2.5) + 0.31 * ((i * 3) % 7 - 3);
        }

    // Woodbury LL == dense-V_j LL, at three quite different (σ_u, σ_e) and β.
    const double thetas[3][4] = {{0.8, 1.4, std::log(0.7), std::log(0.9)},
                                 {0.0, 0.0, std::log(0.05), std::log(2.0)},
                                 {-1.2, 3.0, std::log(3.0), std::log(0.25)}};
    for (int c = 0; c < 3; ++c) {
        std::vector<double> th(thetas[c], thetas[c] + 4);
        const double fast = mxd::ll_grad(X.data(), n, k, y.data(), gcode.data(), G, th, nullptr);
        const double slow = brute_ll(X, n, k, y, gcode, G, th);
        char tag[64];
        std::snprintf(tag, sizeof tag, "woodbury LL == dense #%d", c + 1);
        chk(tag, fast, slow, 1e-12);
    }

    // Gradient self-check at a NON-optimal θ.
    {
        std::vector<double> th = {0.55, 1.1, std::log(0.6), std::log(1.1)};
        const double worst = gradient_selfcheck(X, n, k, y, gcode, G, th);
        std::printf("  %-26s worst rel = %.3e\n", "gradient self-check", worst);
        chk_true("gradient analytic==numeric", worst <= 1e-6);
    }

    // Unbalanced groups exercise the n_j-dependent c_j and log|V_j| — a formula that
    // happened to be right only for equal group sizes would pass everything above.
    {
        std::vector<double> Xu, yu;
        std::vector<int32_t> gu;
        int64_t nu = 0;
        for (int64_t i = 0; i < n; ++i) {
            if (gcode[static_cast<size_t>(i)] == 2 && (i % 2) == 0) continue;  // thin group 2
            if (gcode[static_cast<size_t>(i)] == 5 && (i % 3) != 0) continue;  // thin group 5 harder
            ++nu;
        }
        Xu.assign(static_cast<size_t>(nu) * k, 0.0);
        yu.resize(static_cast<size_t>(nu));
        gu.resize(static_cast<size_t>(nu));
        int64_t w = 0;
        for (int64_t i = 0; i < n; ++i) {
            if (gcode[static_cast<size_t>(i)] == 2 && (i % 2) == 0) continue;
            if (gcode[static_cast<size_t>(i)] == 5 && (i % 3) != 0) continue;
            Xu[static_cast<size_t>(w)] = X[static_cast<size_t>(i)];
            Xu[static_cast<size_t>(nu) + static_cast<size_t>(w)] = 1.0;
            yu[static_cast<size_t>(w)] = y[static_cast<size_t>(i)];
            gu[static_cast<size_t>(w)] = gcode[static_cast<size_t>(i)];
            ++w;
        }
        std::vector<double> th = {0.5, 1.2, std::log(0.9), std::log(0.6)};
        const double fast = mxd::ll_grad(Xu.data(), nu, k, yu.data(), gu.data(), G, th, nullptr);
        const double slow = brute_ll(Xu, nu, k, yu, gu, G, th);
        chk("woodbury LL == dense (unbal)", fast, slow, 1e-12);
        const double worst = gradient_selfcheck(Xu, nu, k, yu, gu, G, th);
        chk_true("gradient (unbalanced)", worst <= 1e-6);
        // And the fit itself must land on a stationary point of that same LL.
        MixedLmResult r = mixedlm_fit(Xu.data(), nu, k, yu.data(), gu.data(), G);
        chk_true("unbalanced fit ok", r.ok);
        if (r.ok) {
            chk("fit LL == dense LL", r.ll, brute_ll(Xu, nu, k, yu, gu, G, r.theta), 1e-12);
            std::vector<double> gg(static_cast<size_t>(k) + 2);
            mxd::ll_grad(Xu.data(), nu, k, yu.data(), gu.data(), G, r.theta, &gg);
            double gmax = 0;
            for (double v : gg) gmax = std::max(gmax, std::fabs(v));
            std::printf("  %-26s max|grad| = %.3e\n", "fit stationarity", gmax);
            chk_true("fit gradient ~ 0", gmax <= 1e-6);
        }
    }

    // Group ORDER must not matter: the likelihood is a product over groups, so
    // shuffling rows (carrying their group code along) is the identity. A code path
    // that assumed contiguous groups would fail here and nowhere else.
    {
        std::vector<double> Xs(static_cast<size_t>(n) * k), ys(static_cast<size_t>(n));
        std::vector<int32_t> gs(static_cast<size_t>(n));
        for (int64_t i = 0; i < n; ++i) {
            const int64_t src = (i * 7 + 3) % n;  // a fixed permutation (gcd(7,24)=1)
            Xs[static_cast<size_t>(i)] = X[static_cast<size_t>(src)];
            Xs[static_cast<size_t>(n) + static_cast<size_t>(i)] = 1.0;
            ys[static_cast<size_t>(i)] = y[static_cast<size_t>(src)];
            gs[static_cast<size_t>(i)] = gcode[static_cast<size_t>(src)];
        }
        std::vector<double> th = {0.55, 1.1, std::log(0.6), std::log(1.1)};
        const double a = mxd::ll_grad(X.data(), n, k, y.data(), gcode.data(), G, th, nullptr);
        const double b = mxd::ll_grad(Xs.data(), n, k, ys.data(), gs.data(), G, th, nullptr);
        chk("LL invariant to row order", b, a, 1e-12);
    }
}

// --- 2/3. oracle + falsification ---------------------------------------------

static void test_oracle() {
    const std::string path = wave_fixture();
    DtaResult dr;
    try {
        dr = read_dta(path);
    } catch (const std::exception& e) {
        std::printf("  ** SKIP oracle legs: cannot read %s (%s)\n", path.c_str(), e.what());
        std::printf("  **   the P7 wave fixture is a Stata-produced artifact and is NOT in git.\n");
        std::printf("  **   Set COBRA_PANGRAM_ARTIFACTS to the directory holding wave_fixture.dta\n");
        std::printf("  **   (or COBRA_VALIDATE_DIR to a checkout that has it) to enable them.\n");
        std::printf("  **   NOTHING BELOW WAS CHECKED AGAINST STATA.\n");
        return;
    }
    std::printf("  -- oracle: mixed y x1 x2 || id:, vce(robust)\n");
    const Frame& f = dr.frame;
    const int64_t nraw = f.n_rows();
    const double* yraw = f.col("y").f64.data();
    const double* x1 = f.col("x1").f64.data();
    const double* x2 = f.col("x2").f64.data();
    const double* idc = f.col("id").f64.data();

    std::vector<int64_t> keep;
    for (int64_t i = 0; i < nraw; ++i)
        if (!std::isnan(yraw[i]) && !std::isnan(x1[i]) && !std::isnan(x2[i]) && !std::isnan(idc[i]))
            keep.push_back(i);
    const int64_t n = static_cast<int64_t>(keep.size());
    const int k = 3;  // [x1, x2, intercept] — intercept LAST, as load_glm delivers it
    chk_abs("N", static_cast<double>(n), 4649, 0);

    std::vector<double> X(static_cast<size_t>(n) * k, 0.0), y(static_cast<size_t>(n));
    std::vector<int32_t> gcode(static_cast<size_t>(n));
    std::vector<double> seen;
    for (int64_t r = 0; r < n; ++r) {
        const int64_t i = keep[static_cast<size_t>(r)];
        X[static_cast<size_t>(r)] = x1[i];
        X[static_cast<size_t>(n) + static_cast<size_t>(r)] = x2[i];
        X[static_cast<size_t>(2) * n + static_cast<size_t>(r)] = 1.0;
        y[static_cast<size_t>(r)] = yraw[i];
        int code = -1;
        for (size_t s = 0; s < seen.size(); ++s)
            if (seen[s] == idc[i]) { code = static_cast<int>(s); break; }
        if (code < 0) { code = static_cast<int>(seen.size()); seen.push_back(idc[i]); }
        gcode[static_cast<size_t>(r)] = code;
    }
    const int G = static_cast<int>(seen.size());
    chk_abs("n_groups", G, 200, 0);

    // Gradient self-check on the REAL design too, at a non-optimal θ — the synthetic
    // case cannot exercise a 3-column design with this conditioning.
    {
        std::vector<double> th = {0.5, 0.1, 2.4, std::log(0.4), std::log(1.2)};
        const double worst = gradient_selfcheck(X, n, k, y, gcode, G, th);
        std::printf("  %-26s worst rel = %.3e\n", "gradient (oracle design)", worst);
        chk_true("gradient analytic==numeric", worst <= 1e-6);
    }

    MixedLmResult r = mixedlm_fit(X.data(), n, k, y.data(), gcode.data(), G);
    chk_true("fit ok", r.ok);
    chk_true("fit converged", r.converged);
    if (!r.ok) return;

    // Obs per group: min 22 / avg 23.2 / max 24.
    chk_abs("obs/group min", static_cast<double>(r.g_min), 22, 0);
    chk_abs("obs/group max", static_cast<double>(r.g_max), 24, 0);
    chk_abs("obs/group avg", r.g_avg, 23.2, 0.05);

    // Log pseudolikelihood = -6244.5896. Also re-derived by the dense-V_j reference
    // at the SAME θ, so the headline number is not resting on the closed forms alone.
    chk("ll", r.ll, -6244.5896, 1e-6);
    chk("ll (dense reference)", brute_ll(X, n, k, y, gcode, G, r.theta), -6244.5896, 1e-6);

    const int np = k + 2;  // (β, ln σ_u, ln σ_e)
    chk("b x1", r.beta[0], .7786023, 1e-6);
    chk("b _cons", r.beta[2], 2.77562, 1e-6);
    chk("var(_cons)", r.var_u, .1031043, 1e-6);
    chk("var(Residual)", r.var_e, .8100676, 1e-6);
    // x2 is the ONE number where a relative tolerance measures nothing about the
    // estimator: Stata prints -.0389073 (six significant digits) for a coefficient
    // whose own robust standard error is .0720, and Stata's optimizer stops with
    // max|grad| = 7.3e-5 where ours reaches 1.2e-10 at a STRICTLY HIGHER likelihood
    // (asserted just below). Against Stata's own full-precision e(b) the gap is
    // 5.9e-8 — 8e-7 of one standard error. Pinned in absolute terms, at both the
    // printed and the full-precision value, rather than papered over with a looser
    // rtol that would hide a real drift of the same size.
    chk_abs("b x2 (printed)", r.beta[1], -.0389073, 1e-6);
    chk_abs("b x2 (vs e(b))", r.beta[1], -0.038907306423, 2e-7);

    // Stata 19.5's own θ, captured locally by re-running the oracle command and
    // dumping the parameter vector — the full-precision counterpart of the log's
    // printed table (see the STATA PROBE note at the bottom of this file):
    //     mixed y x1 x2 || id:
    //     matrix list e(b), format(%20.12f)
    // Our θ must be at least as good a maximizer as Stata's; if a future change
    // makes us land on a WORSE point that still rounds to the printed digits, this
    // is the leg that notices.
    {
        const std::vector<double> st = {0.778602293389, -0.038907306423, 2.775619652738,
                                        -1.136006997362, -0.105318813151};
        std::vector<double> gs(st.size());
        const double ll_st = mxd::ll_grad(X.data(), n, k, y.data(), gcode.data(), G, st, &gs);
        double gmax_st = 0, gmax_ours = 0;
        for (double v : gs) gmax_st = std::max(gmax_st, std::fabs(v));
        std::vector<double> go(st.size());
        mxd::ll_grad(X.data(), n, k, y.data(), gcode.data(), G, r.theta, &go);
        for (double v : go) gmax_ours = std::max(gmax_ours, std::fabs(v));
        std::printf("  %-26s ours %.4e vs stata-θ %.4e ; ΔLL = %+.3e\n", "max|grad| / LL gap",
                    gmax_ours, gmax_st, r.ll - ll_st);
        chk_true("LL(ours) >= LL(stata θ)", r.ll >= ll_st);
        chk_true("our gradient < 1e-8", gmax_ours < 1e-8);
    }

    // --- model-based e(V): the BREAD STRUCTURE, pinned ------------------------
    // These are Stata's e(V) after the SAME command with NO vce() option (STATA
    // PROBE note at the bottom of this file), and they are the direct evidence for
    // how mixedlm_fit builds r.vcov. Two things are pinned here and nowhere else:
    //   * the β/lnσ cross block is EXACTLY zero — Stata prints structural zeros;
    //   * the β block is the MARGINAL (I_ββ)⁻¹ while the variance block is the
    //     PROFILED (full-inverse) block. A plain (−H)⁻¹ — the recipe every other
    //     house MLE uses — gives .00044741 for e(V)[x1,x1] and .00446... for the
    //     lnσ_u entry, so it fails these on sight. Without this leg the wrong bread
    //     could still slip through if the robust legs were ever relaxed.
    {
        const int npp = np;
        auto v = [&](int a, int b) { return r.vcov[static_cast<size_t>(a) * npp + b]; };
        chk("e(V) x1,x1", v(0, 0), .00044169596363, 1e-5);
        chk("e(V) x2,x2", v(1, 1), .000571514731791, 1e-5);
        chk("e(V) _cons,_cons", v(2, 2), .000900767123787, 1e-5);
        chk("e(V) x1,x2", v(0, 1), -1.99522146920e-06, 1e-4);
        chk("e(V) x1,_cons", v(0, 2), -.000221305695088, 1e-5);
        chk("e(V) x2,_cons", v(1, 2), -.000238106233621, 1e-5);
        chk("e(V) lnsu,lnsu", v(3, 3), .00544844550056, 1e-5);
        chk("e(V) lnse,lnse", v(4, 4), .000113442847389, 1e-5);
        chk("e(V) lnsu,lnse", v(3, 4), -.000069856354414, 1e-4);
        bool zero = true;
        for (int a = 0; a < k; ++a)
            for (int b = k; b < npp; ++b)
                if (v(a, b) != 0.0 || v(b, a) != 0.0) zero = false;
        chk_true("e(V) beta/lnsigma == 0", zero);
        // The model-based variance-component SEs Stata prints (.015221 / .017256)
        // come out of the same two rules the robust ones do — checked here so the
        // delta-method rule is pinned independently of the sandwich.
        chk("model se var(_cons)", mixed_var_se(r.var_u, std::sqrt(v(3, 3))), .015221, 1e-4);
        chk("model se var(Resid)", mixed_var_se(r.var_e, std::sqrt(v(4, 4))), .017256, 1e-4);
    }

    // --- robust sandwich over the PER-GROUP scores ---------------------------
    // The bread is r.vcov — the SAME block-diagonal matrix Stata reports as e(V)
    // without vce(), pinned above. Under vce(robust) Stata prints a Robust column
    // over the random-effects table too, so the sandwich covers all five parameters;
    // with a zero cross block that separates cleanly into V_ββ = B_ββ M_ββ B_ββ and
    // V_φφ = B_φφ M_φφ B_φφ, which is why BOTH blocks of the bread have to be right.
    std::vector<double> S = mixedlm_group_scores(X.data(), n, k, y.data(), gcode.data(), G, r);
    chk_abs("score rows", static_cast<double>(S.size()), static_cast<double>(G) * np, 0);
    // The per-group scores must SUM to the full gradient (≈0 at the optimum) — the
    // cheapest possible check that the meat's raw material is the same object the
    // optimizer converged on.
    {
        double worst = 0;
        for (int p = 0; p < np; ++p) {
            double s = 0;
            for (int j = 0; j < G; ++j) s += S[static_cast<size_t>(p) * G + j];
            worst = std::max(worst, std::fabs(s));
        }
        std::printf("  %-26s max|Σ_j g_j| = %.3e\n", "group scores sum to grad", worst);
        chk_true("Σ_j g_j == 0 at optimum", worst <= 1e-6);
    }

    auto sandwich_V = [&](double factor) {
        std::vector<double> meat(static_cast<size_t>(np) * np, 0.0);
        la::xtx(S.data(), G, np, meat.data());
        std::vector<double> V(static_cast<size_t>(np) * np, 0.0), tmp(static_cast<size_t>(np) * np, 0.0);
        la::sandwich(r.vcov.data(), meat.data(), np, factor, V.data(), tmp.data());
        return V;
    };
    // HYPOTHESIS UNDER TEST: the ML cluster small-sample factor G/(G−1) with G the
    // number of GROUPS (200) — the same multiplier the P5/P7 ML precedents use, not
    // the regression-style (N−1)/(N−K)·G/(G−1). The assertions below are what verify
    // it. The falsification leg further down shows the no-factor alternative missing.
    const double factor = static_cast<double>(G) / (static_cast<double>(G) - 1.0);
    std::vector<double> V = sandwich_V(factor);
    auto sd = [&](int p) { return std::sqrt(std::max(0.0, V[static_cast<size_t>(p) * np + p])); };

    chk("se x1 (robust)", sd(0), .0733897, 1e-5);
    chk("se x2 (robust)", sd(1), .0719695, 1e-5);
    chk("se _cons (robust)", sd(2), .0496301, 1e-5);
    chk("z x1", r.beta[0] / sd(0), 10.61, 1e-3);
    chk("z _cons", r.beta[2] / sd(2), 55.93, 1e-3);
    const double z95 = 1.959963984540054;
    chk("ci_lo x1", r.beta[0] - z95 * sd(0), .6347611, 1e-5);
    chk("ci_hi x1", r.beta[0] + z95 * sd(0), .9224435, 1e-5);
    chk("ci_lo x2", r.beta[1] - z95 * sd(1), -.1799649, 1e-5);
    chk("ci_hi x2", r.beta[1] + z95 * sd(1), .1021503, 1e-5);
    chk("ci_lo _cons", r.beta[2] - z95 * sd(2), 2.678346, 1e-5);
    chk("ci_hi _cons", r.beta[2] + z95 * sd(2), 2.872893, 1e-5);

    // Wald chi2(2) = 112.63 — SLOPES only, the constant excluded.
    {
        const int ks = 2;
        std::vector<double> Vs(static_cast<size_t>(ks) * ks);
        for (int i = 0; i < ks; ++i)
            for (int j = 0; j < ks; ++j) Vs[static_cast<size_t>(i) * ks + j] = V[static_cast<size_t>(i) * np + j];
        double w = 0;
        if (la::spd_inverse(Vs.data(), ks) == 0)
            for (int i = 0; i < ks; ++i)
                for (int j = 0; j < ks; ++j)
                    w += r.beta[static_cast<size_t>(i)] * Vs[static_cast<size_t>(i) * ks + j] *
                         r.beta[static_cast<size_t>(j)];
        chk("Wald chi2(2)", w, 112.63, 1e-4);
    }

    // --- variance components, Stata's σ² metric ------------------------------
    // se(var) = 2·var·se(lnσ) (delta method) and a LOGNORMAL CI. Both rules come from
    // mixed_var_se/mixed_var_ci in the estimator header — the SAME functions the
    // command layer reports with, so this pins what a user actually sees.
    {
        const double se_ln_u = sd(k), se_ln_e = sd(k + 1);
        const double se_vu = mixed_var_se(r.var_u, se_ln_u);
        const double se_ve = mixed_var_se(r.var_e, se_ln_e);
        chk("se var(_cons)", se_vu, .0181469, 1e-5);
        chk("se var(Residual)", se_ve, .0462533, 1e-5);
        double lo = 0, hi = 0;
        mixed_var_ci(r.var_u, se_vu, z95, &lo, &hi);
        chk("ci_lo var(_cons)", lo, .0730233, 1e-5);
        chk("ci_hi var(_cons)", hi, .1455769, 1e-5);
        mixed_var_ci(r.var_e, se_ve, z95, &lo, &hi);
        chk("ci_lo var(Residual)", lo, .7243014, 1e-5);
        chk("ci_hi var(Residual)", hi, .9059895, 1e-5);

        // FALSIFICATION 1 — the CI is lognormal, not symmetric. A `b ± z·se` interval
        // on var(_cons) gives [.06754, .13867]; if THAT matched the oracle to 1e-5 the
        // assertions above would be meaningless.
        const double sym_lo = r.var_u - z95 * se_vu, sym_hi = r.var_u + z95 * se_vu;
        chk_true("CI is NOT symmetric b±z·se",
                 std::fabs(sym_lo - .0730233) / .0730233 > 1e-3 &&
                 std::fabs(sym_hi - .1455769) / .1455769 > 1e-3);
    }

    // FALSIFICATION 2 — the G/(G−1) factor is doing real work. Without it every SE
    // shrinks by √(199/200) ≈ 0.99749, i.e. ~2.5e-3 relative — 250× the tolerance the
    // adopted factor passes at, so the two are cleanly distinguishable on this data.
    {
        std::vector<double> V1 = sandwich_V(1.0);
        const double se1 = std::sqrt(std::max(0.0, V1[0]));
        std::printf("  %-26s no-factor se(x1) = %.10g (oracle .0733897)\n", "falsify G/(G-1)", se1);
        chk_true("no-factor se(x1) MISSES oracle", std::fabs(se1 - .0733897) / .0733897 > 1e-4);
        chk("with-factor/no-factor ratio", sd(0) / se1,
            std::sqrt(static_cast<double>(G) / (static_cast<double>(G) - 1.0)), 1e-12);
    }
}

// --- STATA PROBE (provenance of the e(b)/e(V) numbers above) -----------------
//
// Everything printed in the frozen answerbook (internal capture
// p7_stata.log) is asserted above at the precision the log prints. Three
// groups of numbers here are NOT in that log — Stata's full-precision e(b), its
// model-based e(V), and the model-based variance-component SEs — and they exist
// because the log's printed table alone cannot distinguish the several breads that
// all round to the same robust digits. They were captured by re-running the SAME
// command on the SAME fixture with the SAME Stata (19.5), which reproduced every
// printed digit of the answerbook exactly. Reproduce with:
//
//     use wave_fixture.dta, clear
//     mixed y x1 x2 || id:
//     matrix list e(b), format(%20.12f)
//     matrix list e(V), format(%20.12g)
//     mixed y x1 x2 || id:, vce(robust)
//     matrix list e(V), format(%20.12g)
//
// The robust e(V) from that probe agrees with every SE asserted from the frozen log,
// so the two sources are one oracle, not two.
int test_mixedlm_main() {
    std::printf("test_mixedlm:\n");
    test_mechanism();
    test_oracle();
    std::printf(fail ? "test_mixedlm: FAIL\n" : "test_mixedlm: OK\n");
    return fail;
}
