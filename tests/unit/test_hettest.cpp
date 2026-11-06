// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Heteroskedasticity (Breusch-Pagan / Cook-Weisberg) + Ramsey RESET parity test.
// Model: price ~ mpg + weight + foreign on sysuse auto. Goldens from
// hettest_golden.do (estat hettest {,iid,fstat,rhs} and estat ovtest). All stats
// match Stata to ~1e-12; ovtest matches after the `gen float` power truncation.
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "core/model.h"
#include "estimators/diagtests.h"
#include "io/columnar.h"
#include "validate_dir.h"

using namespace cecon;

int test_hettest_main() {
    std::printf("test_hettest:\n");
    const cobra_test::ValidateBase base;

    // golden: stat,value
    std::map<std::string, double> g;
    {
        std::ifstream f(base + "hettest_golden.csv");
        std::string line;
        std::getline(f, line);  // header
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            std::stringstream ss(line);
            std::string k, v;
            std::getline(ss, k, ',');
            std::getline(ss, v, ',');
            g[k] = std::stod(v);
        }
    }

    Dataset ds = Dataset::open(base + "hettest.hbin");
    ModelData md = build_model(ds, "price", {"mpg", "weight", "foreign"}, {}, {});
    const double* X = md.dm.X.data();
    const double* y = md.dm.y.data();
    const int64_t n = md.dm.n;
    const int k = md.dm.k;          // [(intercept), mpg, weight, foreign]
    const int intercept_col = 0;    // build_model puts the constant first

    int fail = 0;
    auto chk = [&](const char* tag, double got, double want, double tol) {
        double rel = std::fabs(got - want) / (std::fabs(want) + 1e-30);
        std::printf("  %-10s got=%.12g  stata=%.12g  rel=%.2e\n", tag, got, want, rel);
        if (!(rel <= tol)) { std::printf("    <<FAIL %s rel %.2e > %.0e\n", tag, rel, tol); fail = 1; }
    };

    // --- BP/Cook-Weisberg against fitted values (default) ---
    BpResult bf = breusch_pagan(X, n, k, y, BpZ::Fitted, nullptr, 0, intercept_col);
    chk("ht_chi2", bf.chi2_normal, g["ht_chi2"], 1e-10);
    chk("ht_df", bf.df, g["ht_df"], 1e-12);
    chk("hi_chi2", bf.chi2_iid, g["hi_chi2"], 1e-10);
    chk("hf_F", bf.F, g["hf_F"], 1e-10);
    chk("hf_dfr", bf.df_r, g["hf_dfr"], 1e-12);

    // --- BP against the rhs regressors ---
    BpResult br = breusch_pagan(X, n, k, y, BpZ::Rhs, nullptr, 0, intercept_col);
    chk("hr_chi2", br.chi2_normal, g["hr_chi2"], 1e-10);
    chk("hr_df", br.df, g["hr_df"], 1e-12);
    chk("hri_chi2", br.chi2_iid, g["hri_chi2"], 1e-10);

    // --- White's general test (estat imtest, white) ---
    WhiteResult w = white_test(X, n, k, y, intercept_col);
    chk("w_chi2", w.chi2, g["w_chi2"], 1e-6);
    chk("w_df", w.df, g["w_df"], 1e-12);

    // --- Ramsey RESET (estat ovtest) ---
    ResetResult ov = ramsey_reset(X, n, k, y);
    chk("ov_F", ov.F, g["ov_F"], 1e-9);
    chk("ov_dfm", ov.df_m, g["ov_dfm"], 1e-12);
    chk("ov_dfr", ov.df_r, g["ov_dfr"], 1e-12);

    std::printf("%s\n", fail ? "FAILED" : "PASSED");
    return fail;
}
