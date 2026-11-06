// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Validates the Wald/F machinery (betai, f_pvalue, t_*, wald_test) against
// textbook constants and the Stata `regress; test` golden (test_chow_golden.do).
#include <cmath>
#include <cstdio>
#include <vector>

#include "estimators/feols.h"
#include "estimators/posthest.h"
#include "core/model.h"
#include "io/columnar.h"
#include "validate_dir.h"

using namespace cecon;

static int near(double a, double b, double rtol, const char* what) {
    double r = std::fabs(a - b) / (std::fabs(b) + 1e-300);
    std::printf("  %-22s cecon=%.10g  exp=%.10g  rel=%.2e\n", what, a, b, r);
    return r <= rtol ? 0 : 1;
}

int test_wald_main() {
    std::printf("test_wald:\n");
    int fail = 0;
    // Distribution sanity vs known constants.
    fail |= near(betai(0.5, 0.5, 0.5), 0.5, 1e-12, "betai(.5,.5,.5)");
    fail |= near(chi2_pvalue(3.8414588, 1), 0.05, 1e-6, "chi2_p(3.841,1)");
    fail |= near(t_critical(0.05, 10), 2.2281389, 1e-6, "t_crit(.05,10)");
    fail |= near(f_pvalue(2.69, 3, 100), 0.0503472, 1e-4, "f_p(2.69,3,100)");

    // Wald F vs Stata `regress visits_w treated age_z edu_hi; test ...`.
    Dataset ds = Dataset::open(cobra_test::validate_path("csdid_input.hbin"));
    int64_t N = ds.n_rows();
    const double* wv = ds.col("wv").f64();
    const double* gvar = ds.col("gvar").f64();
    const double* y = ds.col("visits_w").f64();
    const double* age_z = ds.col("age_z").f64();
    const double* edu_hi = ds.col("edu_hi").f64();
    // listwise drop NaN y; build [treated, age_z, edu_hi, intercept].
    std::vector<int64_t> keep;
    for (int64_t i = 0; i < N; ++i) if (!std::isnan(y[i])) keep.push_back(i);
    int64_t n = (int64_t)keep.size();
    int k = 4;
    ModelData md; md.has_intercept = true;
    md.dm.n = n; md.dm.k = k; md.dm.X.assign((size_t)n * k, 0.0); md.dm.y.assign((size_t)n, 0.0);
    md.dm.xnames = {"treated", "age_z", "edu_hi", "(intercept)"};
    for (int64_t i = 0; i < n; ++i) {
        int64_t r = keep[(size_t)i];
        double tr = (gvar[r] > 0 && wv[r] >= gvar[r]) ? 1.0 : 0.0;
        md.dm.X[0 * n + i] = tr;
        md.dm.X[1 * n + i] = age_z[r];
        md.dm.X[2 * n + i] = edu_hi[r];
        md.dm.X[3 * n + i] = 1.0;
        md.dm.y[(size_t)i] = y[r];
    }
    FeolsResult r = feols_fit(md, Vcov::IID);
    int kk = (int)r.names.size();
    double df2 = (double)r.dof.dof_resid;

    // joint: treated=0 & age_z=0  → Stata F=187.312401874111
    {
        std::vector<double> R((size_t)2 * kk, 0.0), q(2, 0.0);
        R[0 * kk + 0] = 1.0; R[1 * kk + 1] = 1.0;
        WaldResult w = wald_test(r.beta.data(), r.vcov_matrix.data(), kk, R.data(), q.data(), 2);
        fail |= near(w.F, 187.312401874111, 1e-6, "joint F");
        fail |= near(f_pvalue(w.F, 2, df2), 7.2288288145058e-82, 1e-3, "joint p_F");
    }
    // linear: treated - age_z = 0  → Stata F=360.66508489019
    {
        std::vector<double> R((size_t)kk, 0.0), q(1, 0.0);
        R[0] = 1.0; R[1] = -1.0;
        WaldResult w = wald_test(r.beta.data(), r.vcov_matrix.data(), kk, R.data(), q.data(), 1);
        fail |= near(w.F, 360.66508489019, 1e-6, "linear F");
    }
    // affine: treated + age_z = 1  → Stata F=211.922088115436
    {
        std::vector<double> R((size_t)kk, 0.0), q(1, 1.0);
        R[0] = 1.0; R[1] = 1.0;
        WaldResult w = wald_test(r.beta.data(), r.vcov_matrix.data(), kk, R.data(), q.data(), 1);
        fail |= near(w.F, 211.922088115436, 1e-6, "affine F");
    }
    // Stata `regress` exact fit: yx = 3 + 2x on x = mod(_n*7,11) + _n/10, N 50.
    // With regress_exact_fit the RSS and V are 0, and `test x` has R V R' = 0:
    // StataBE answers F(1, 48) = . and Prob > F = . at rc 0 (missing, not an
    // error). Without the switch (reghdfe's route) the computed RSS stays:
    // StataBE `reghdfe yx x, noabsorb` e(rss) 8.526e-28, _se[x] 1.72e-16.
    {
        const int64_t n50 = 50;
        auto exact_md = [&]() {
            ModelData m; m.has_intercept = true;
            m.dm.n = n50; m.dm.k = 2; m.dm.X.assign((size_t)n50 * 2, 0.0); m.dm.y.assign((size_t)n50, 0.0);
            m.dm.xnames = {"(intercept)", "x"};  // build_model's order: the constant is column 0
            for (int64_t i = 0; i < n50; ++i) {
                const double x = std::fmod((double)(i + 1) * 7.0, 11.0) + (double)(i + 1) / 10.0;
                m.dm.X[(size_t)i] = 1.0;
                m.dm.X[(size_t)n50 + (size_t)i] = x;
                m.dm.y[(size_t)i] = 3.0 + 2.0 * x;
            }
            return m;
        };
        ModelData m1 = exact_md();
        FeolsResult ex = feols_fit(m1, Vcov::IID, 0, false, false, false, /*regress_exact_fit=*/true);
        int bad = (ex.exact_fit && ex.rss == 0.0 && ex.se[0] == 0.0 && ex.se[1] == 0.0 && ex.r2 == 1.0) ? 0 : 1;
        std::printf("  %-22s exact_fit=%d rss=%.3g se=%.3g r2=%.17g\n", "regress exact fit", (int)ex.exact_fit,
                    ex.rss, ex.se[1], ex.r2);
        fail |= bad;
        std::vector<double> R(2, 0.0), q(1, 0.0);
        R[1] = 1.0;  // test x
        WaldResult w = wald_test(ex.beta.data(), ex.vcov_matrix.data(), 2, R.data(), q.data(), 1);
        bad = (w.ok && std::isnan(w.F) && std::isnan(w.chi2) && std::isnan(f_pvalue(w.F, 1, 48)) &&
               std::isnan(chi2_pvalue(w.chi2, 1))) ? 0 : 1;
        std::printf("  %-22s ok=%d F=%g p=%g\n", "test on V = 0", (int)w.ok, w.F, f_pvalue(w.F, 1, 48));
        fail |= bad;
        ModelData m2 = exact_md();
        FeolsResult hd = feols_fit(m2, Vcov::IID);
        bad = (!hd.exact_fit && hd.rss > 0.0 && hd.se[1] > 0.0) ? 0 : 1;
        std::printf("  %-22s exact_fit=%d rss=%.3g se=%.3g\n", "default keeps RSS", (int)hd.exact_fit, hd.rss,
                    hd.se[1]);
        fail |= bad;
    }
    std::printf("%s\n", fail ? "FAILED" : "PASSED");
    return fail;
}
