// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// LIML / k-class parity test vs Stata ivregress liml (small). Overidentified IV:
// visits_w treated (age_z = female edu_hi). kappa, beta, SE match ~1e-9; the
// kappa_override=1 path reproduces 2SLS. Golden from liml_golden.do.
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "estimators/liml.h"
#include "io/columnar.h"
#include "validate_dir.h"

using namespace cecon;

int test_liml_main() {
    std::printf("test_liml:\n");
    const cobra_test::ValidateBase base;
    std::map<std::string, double> g;
    {
        std::ifstream f(base + "liml_golden.csv");
        std::string line;
        std::getline(f, line);
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            std::stringstream ss(line);
            std::string k, v;
            std::getline(ss, k, ','); std::getline(ss, v, ',');
            g[k] = std::stod(v);
        }
    }
    Dataset ds = Dataset::open(base + "ivdiag.hbin");
    int64_t n = ds.n_rows();
    auto col = [&](const char* nm) { const double* p = ds.col(nm).f64(); return std::vector<double>(p, p + n); };
    std::vector<double> y = col("visits_w"), age = col("age_z"), tr = col("treated"),
                        fem = col("female"), edu = col("edu_hi");
    std::vector<double> Y = age;                              // endogenous (p=1)
    std::vector<double> X1(static_cast<size_t>(n) * 2);       // [treated, const]
    std::copy(tr.begin(), tr.end(), X1.begin());
    for (int64_t i = 0; i < n; ++i) X1[n + i] = 1.0;
    std::vector<double> Z2(static_cast<size_t>(n) * 2);       // [female, edu_hi]
    std::copy(fem.begin(), fem.end(), Z2.begin());
    std::copy(edu.begin(), edu.end(), Z2.begin() + n);

    int fail = 0;
    auto chk = [&](const char* tag, double got, double want, double tol) {
        double rel = std::fabs(got - want) / (std::fabs(want) + 1e-30);
        std::printf("  %-14s got=%.10g stata=%.10g rel=%.2e\n", tag, got, want, rel);
        if (!(rel <= tol)) { std::printf("    <<FAIL\n"); fail = 1; }
    };

    LimlResult r = liml_fit(y, n, Y, 1, X1, 2, Z2, 2, /*small=*/true);
    chk("liml_kappa", r.kappa, g["liml_kappa"], 1e-9);
    chk("liml_b_age", r.beta[0], g["liml_b_age"], 1e-8);      // X=[age_z, treated, const]
    chk("liml_se_age", r.se[0], g["liml_se_age"], 1e-7);
    chk("liml_b_treated", r.beta[1], g["liml_b_treated"], 1e-8);
    chk("liml_se_treated", r.se[1], g["liml_se_treated"], 1e-7);
    chk("liml_b_cons", r.beta[2], g["liml_b_cons"], 1e-8);
    chk("liml_se_cons", r.se[2], g["liml_se_cons"], 1e-7);

    // kappa_override = 1 -> 2SLS
    LimlResult r2 = liml_fit(y, n, Y, 1, X1, 2, Z2, 2, true, 0.0, /*kappa_override=*/1.0);
    chk("tsls_kappa", r2.kappa, g["tsls_kappa"], 1e-12);
    chk("tsls_b_age", r2.beta[0], g["tsls_b_age"], 1e-8);

    std::printf("%s\n", fail ? "FAILED" : "PASSED");
    return fail;
}
