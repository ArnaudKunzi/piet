// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Ring-1 synthetic exact-answer test for pooled OLS.
// Data: (x,y) = (1,1),(2,2),(3,2); regress y on intercept + x.
// Hand-computed: beta = [2/3, 1/2], ssr = 1/6, sigma2 = 1/6,
//   se(slope) = sqrt(sigma2/Sxx) = sqrt((1/6)/2), se(intercept) similarly.
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "core/matrix.h"
#include "core/collinear.h"
#include "estimators/ols.h"
#include "estimators/feols.h"

using namespace cecon;

static int g_fail = 0;
static void check(const char* what, double got, double want, double tol) {
    double err = std::fabs(got - want);
    if (err > tol) {
        std::printf("  FAIL %-18s got=%.12g want=%.12g (err=%.3g > %.3g)\n", what, got,
                    want, err, tol);
        ++g_fail;
    } else {
        std::printf("  ok   %-18s = %.12g\n", what, got);
    }
}

int test_ols_synth_main() {
    DesignMatrix dm;
    dm.n = 3;
    dm.k = 2;
    dm.y = {1, 2, 2};
    dm.X.resize(6);
    // col 0 = intercept (ones), col 1 = x
    double* c0 = dm.col(0);
    double* c1 = dm.col(1);
    c0[0] = c0[1] = c0[2] = 1.0;
    c1[0] = 1;
    c1[1] = 2;
    c1[2] = 3;
    dm.xnames = {"(intercept)", "x"};

    OlsResult r = ols_fit(dm);

    std::printf("test_ols_synth:\n");
    check("beta[intercept]", r.beta[0], 2.0 / 3.0, 1e-10);
    check("beta[x]", r.beta[1], 0.5, 1e-10);
    check("ssr", r.ssr, 1.0 / 6.0, 1e-10);
    check("sigma2", r.sigma2, 1.0 / 6.0, 1e-10);
    double Sxx = 2.0;  // sum (x-xbar)^2
    check("se_classic[x]", r.se_classic[1], std::sqrt((1.0 / 6.0) / Sxx), 1e-9);
    // se(intercept) = sqrt(sigma2 * (1/n + xbar^2/Sxx)), xbar=2
    check("se_classic[icpt]", r.se_classic[0],
          std::sqrt((1.0 / 6.0) * (1.0 / 3.0 + 4.0 / Sxx)), 1e-9);

    // Frozen estout/coefplot captures retain the larger of x and 2*x, rather
    // than always the first continuous column. The mask remains in source order.
    DesignMatrix scaled;
    scaled.n = 6;
    scaled.k = 3;
    scaled.xnames = {"(intercept)", "x", "twox"};
    scaled.X = {1, 1, 1, 1, 1, 1, -3, -2, -1, 1, 2, 3, -6, -4, -2, 2, 4, 6};
    scaled.y = {-5, -2, 1, 7, 10, 13};
    const auto scaled_mask = collinear_pivot_keep_mask(scaled.X.data(), 6, 3, 0);
    check("scaled mask", scaled_mask == std::vector<char>({1, 0, 1}), 1, 0);
    const OlsResult sr = ols_fit(scaled);
    check("scaled omit x", sr.omitted == std::vector<std::string>({"x"}), 1, 0);
    check("scaled coefficient", sr.beta[1], 1.5, 1e-12);
    check("scaled intercept", sr.beta[0], 4.0, 1e-12);
    std::swap_ranges(scaled.X.begin() + 6, scaled.X.begin() + 12, scaled.X.begin() + 12);
    const auto reversed = collinear_pivot_keep_mask(scaled.X.data(), 6, 3, 0);
    check("reversed mask", reversed == std::vector<char>({1, 1, 0}), 1, 0);

    const double constant_duplicate[] = {2, 2, 2, 2, 1, 1, 1, 1};
    const auto protected_mask = collinear_pivot_keep_mask(constant_duplicate, 4, 2, 1);
    check("protected constant", protected_mask == std::vector<char>({0, 1}), 1, 0);
    const auto full_rank = collinear_pivot_keep_mask(dm.X.data(), dm.n, dm.k, 0);
    check("full-rank mask", full_rank == std::vector<char>({1, 1}), 1, 0);

    // w2af_macro_text_file mt_num_e_r2: exact centered R² is 3/6019.
    // The former 1-RSS/TSS formula prints ...7285 instead of ...7284.
    ModelData small_r2;
    small_r2.has_intercept = true;
    small_r2.dm.n = 12;
    small_r2.dm.k = 2;
    small_r2.dm.xnames = {"(intercept)", "x"};
    small_r2.dm.X.resize(24);
    for (int i = 0; i < 12; ++i) {
        small_r2.dm.X[i] = 1;
        small_r2.dm.X[i + 12] = i + 1;
        small_r2.dm.y.push_back((7 * (i + 1)) % 11 + (i + 1) / 4.0);
    }
    const OlsResult small_ols = ols_fit(small_r2.dm);
    const FeolsResult small_feols = feols_fit(small_r2, Vcov::IID);
    check("small R2 ols", small_ols.r2, 3.0 / 6019.0, 5e-17);
    check("small R2 feols", small_feols.r2, 3.0 / 6019.0, 5e-17);
    char r2text[64];
    std::snprintf(r2text, sizeof(r2text), "%.16f", small_feols.r2);
    check("small R2 text", std::string(r2text) == "0.0004984216647284", 1, 0);

    if (g_fail) {
        std::printf("FAILED (%d checks)\n", g_fail);
        return 1;
    }
    std::printf("PASSED\n");
    return 0;
}
