// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Logit IRLS validated against statsmodels.Logit (Newton MLE) on a fixed,
// non-separable 16-obs dataset. Checks coefficients, the (X'WX)^{-1} bread
// (== statsmodels cov_params), and fitted probabilities.
#include <cmath>
#include <cstdio>
#include <vector>

#include <string>

#include "estimators/logit.h"
#include "estimators/posthest.h"

using namespace cecon;
static int g_fail = 0;
static void approx(const char* w, double got, double want, double tol = 1e-6) {
    if (std::fabs(got - want) / (std::fabs(want) + 1e-30) > tol) {
        std::printf("  FAIL %s: got %.15g want %.15g\n", w, got, want); ++g_fail;
    }
}

int test_logit_main() {
    std::printf("test_logit:\n");
    std::vector<double> x1 = {1, 2, 3, 1, 2, 4, 5, 2, 3, 1, 4, 2, 6, 3, 2, 5};
    std::vector<double> x2 = {0.5, 1.0, 0.2, 0.8, 1.5, 0.3, 0.9, 1.2, 0.4, 0.7, 0.1, 1.1, 0.6, 0.9, 1.3, 0.2};
    std::vector<double> y  = {0, 1, 0, 1, 1, 0, 1, 0, 1, 1, 0, 0, 1, 0, 1, 0};
    const int n = 16, k = 3;
    std::vector<double> X(static_cast<size_t>(n) * k);
    for (int i = 0; i < n; ++i) { X[i] = 1.0; X[n + i] = x1[i]; X[2 * n + i] = x2[i]; }

    LogitResult r = logit_fit(X.data(), n, k, y.data());
    std::printf("  iters=%d converged=%d\n", r.iters, r.converged);

    double bexp[3] = {-2.2610006219378964, 0.1589888803402532, 2.4777336754069363};
    double cexp[3] = {3.8674124734774957, 0.1496378772708132, 2.4951915613777884};
    double pexp[3] = {0.2966827041519451, 0.6305811627679008, 0.21610897021940656};
    for (int j = 0; j < 3; ++j) {
        approx("beta", r.beta[j], bexp[j]);
        approx("covdiag", r.bread[j + j * 3], cexp[j]);
    }
    for (int i = 0; i < 3; ++i) approx("p", r.p[i], pexp[i]);

    // Stata `cloglog yb x z` on BUG-1169's 200 rows (each variable stored as a float,
    // as `generate` stores it): cloglog.ado starts ml at `logit ..., asis iter(3)` and
    // iterates clog_lf (binary_ml_fit's Cloglog link). StataBE 19.5
    // (scratch-laneA/agents/eng-ml/x18.log C1): b .42755279843635863
    // -.055162435286310103 -.22425779564339185, e(ll) -113.86960425047252, e(ic) 4,
    // V[1,1] .0044216347924531932.
    {
        const int nc = 200, kc = 3;
        auto f = [](double v) { return static_cast<double>(static_cast<float>(v)); };
        std::vector<double> Xc(static_cast<size_t>(nc) * kc), yc(static_cast<size_t>(nc));
        for (int i = 0; i < nc; ++i) {
            const int m = i + 1;
            const double x = f((m * 7 % 23) / 4.0 - 2.75 + (m % 3) / 8.0);
            const double z = f((m * 11 % 17) / 8.0 + (m * 5 % 9) / 16.0);
            const double eps = f(((m * 37 % 41) / 41.0 + (m * 53 % 59) / 59.0 + (m * 29 % 31) / 31.0 - 1.5) * 2.2);
            const double z2 = f((m * 13 % 29) / 7.0 - 2.0);
            const double xe = f(0.5 * z2 + 0.3 * x + eps / 3.0);
            const bool d = (m % 9) == 0;
            yc[static_cast<size_t>(i)] = ((0.5 * x + 0.3 * xe + eps) > 0.0 || d) ? 1.0 : 0.0;
            Xc[static_cast<size_t>(i)] = x;
            Xc[static_cast<size_t>(nc + i)] = z;
            Xc[static_cast<size_t>(2 * nc + i)] = 1.0;
        }
        MlMaxOptions start_opts;
        start_opts.maxiter = 3;
        const std::vector<double> b0 =
            binary_ml_fit(BinaryLink::Logit, Xc.data(), nc, kc, yc.data(), nullptr, nullptr, true, start_opts).beta;
        const BinaryMlResult c =
            binary_ml_fit(BinaryLink::Cloglog, Xc.data(), nc, kc, yc.data(), nullptr, nullptr, true, MlMaxOptions{}, &b0);
        approx("cloglog b[x]", c.beta[0], .42755279843635863, 1e-9);
        approx("cloglog b[z]", c.beta[1], -.055162435286310103, 1e-9);
        approx("cloglog b[_cons]", c.beta[2], -.22425779564339185, 1e-9);
        approx("cloglog ll", c.ll, -113.86960425047252, 1e-12);
        approx("cloglog V[x,x]", c.bread[0], .0044216347924531932, 1e-9);
        if (c.ic != 4 || !c.converged) {
            std::printf("  FAIL cloglog ic %d converged %d (Stata 4, 1)\n", c.ic, static_cast<int>(c.converged));
            ++g_fail;
        }
        std::printf("  cloglog: b[x] %.17g ll %.17g ic %d\n", c.beta[0], c.ll, c.ic);
        // `margins, dydx(*)` after that fit: Pr = 1 - exp(-exp(xb)), its derivative
        // exp(xb - exp(xb)) (posthest.cpp GlmLink::Cloglog). StataBE 19.5
        // (eng-ml/x21.log MDA): dydx x .12647658050634944, z -.016317882172519057,
        // se(z) .04872624420247116; MD se(x) .012202946987095068.
        const std::vector<std::string> names = {"x", "z", "(intercept)"};
        const AmeResult a = margins_ame(GlmLink::Cloglog, Xc.data(), nc, kc, names, c.beta.data(),
                                        c.bread.data(), nullptr, {});
        if (!a.ok || a.rows.size() != 2) {
            std::printf("  FAIL cloglog margins_ame: %s\n", a.error.c_str());
            ++g_fail;
        } else {
            approx("cloglog AME x", a.rows[0].dydx, .12647658050634944, 1e-9);
            approx("cloglog AME se x", a.rows[0].se, .012202946987095068, 1e-9);
            approx("cloglog AME z", a.rows[1].dydx, -.016317882172519057, 1e-9);
            approx("cloglog AME se z", a.rows[1].se, .04872624420247116, 1e-9);
        }
    }

    if (g_fail) { std::printf("FAILED (%d)\n", g_fail); return 1; }
    std::printf("  ok — matches statsmodels.Logit and StataBE cloglog\nPASSED\n");
    return 0;
}
