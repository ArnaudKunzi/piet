// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Ring-1 test for 2SLS: when the instrument equals the endogenous regressor, the
// projection is the identity and 2SLS must reduce exactly to OLS (coef and SE).
#include <cmath>
#include <cstdio>
#include <vector>

#include "core/matrix.h"
#include "estimators/iv2sls.h"
#include "estimators/ols.h"

using namespace cecon;

static int g_fail = 0;
static void check(const char* what, double got, double want, double tol) {
    if (std::fabs(got - want) > tol) {
        std::printf("  FAIL %-20s got=%.12g want=%.12g\n", what, got, want);
        ++g_fail;
    } else {
        std::printf("  ok   %-20s = %.12g\n", what, got);
    }
}

int test_iv_main() {
    std::printf("test_iv:\n");
    std::vector<double> x = {1, 2, 3, 4, 6};
    std::vector<double> y = {2, 3, 5, 4, 8};

    // IV with instrument == endogenous regressor (perfect first stage)
    IvModelData md;
    md.n = 5;
    md.has_intercept = true;
    md.kexog = 1;  // intercept
    md.kendog = 1;
    md.kinst = 1;
    md.Xexog = {1, 1, 1, 1, 1};
    md.Xendog = x;
    md.Zinst = x;  // instrument = regressor
    md.y = y;
    md.exog_names = {"(intercept)"};
    md.endog_names = {"x"};
    // small=true: the IV==OLS identity is asserted against ols_fit's classic
    // (N-K) SEs; the iv default is asymptotic (RSS/N) since the ivregress-parity
    // convention flip, so opt back into the small-sample factors here.
    IvResult riv = iv2sls_fit(md, Vcov::IID, 0, false, /*small=*/true);

    // OLS of y on [intercept, x]
    DesignMatrix dm;
    dm.n = 5;
    dm.k = 2;
    dm.y = y;
    dm.X.resize(10);
    for (int i = 0; i < 5; ++i) { dm.col(0)[i] = 1.0; dm.col(1)[i] = x[i]; }
    dm.xnames = {"(intercept)", "x"};
    OlsResult ro = ols_fit(dm);

    // names order: iv = [intercept, x]; ols = [intercept, x]
    check("beta intercept", riv.beta[0], ro.beta[0], 1e-10);
    check("beta x", riv.beta[1], ro.beta[1], 1e-10);
    check("se intercept (iid)", riv.se[0], ro.se_classic[0], 1e-10);
    check("se x (iid)", riv.se[1], ro.se_classic[1], 1e-10);

    if (g_fail) { std::printf("FAILED (%d)\n", g_fail); return 1; }
    std::printf("PASSED\n");
    return 0;
}
