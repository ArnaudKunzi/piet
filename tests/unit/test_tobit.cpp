// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// tobit (censored-normal MLE) parity test vs Stata `tobit visits_w age_z edu_hi
// treated, ll(0)`. Golden from tobit_golden.do. Coefs + sigma match to ~1e-5;
// log-likelihood + censoring counts exact.
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "estimators/tobit.h"
#include "io/columnar.h"
#include "validate_dir.h"

using namespace cecon;

int test_tobit_main() {
    std::printf("test_tobit:\n");
    const cobra_test::ValidateBase base;

    std::map<std::string, std::pair<double, double>> g;  // name -> (b, se)
    {
        std::ifstream f(base + "tobit_golden.csv");
        std::string line;
        std::getline(f, line);
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            std::stringstream ss(line);
            std::string nm, b, se;
            std::getline(ss, nm, ',');
            std::getline(ss, b, ',');
            std::getline(ss, se, ',');
            g[nm] = {std::stod(b), std::stod(se)};
        }
    }

    Dataset ds = Dataset::open(base + "tobit.hbin");
    int64_t n = ds.n_rows();
    auto col = [&](const char* nm) { const double* p = ds.col(nm).f64(); return std::vector<double>(p, p + n); };
    std::vector<double> y = col("visits_w");
    std::vector<double> age = col("age_z"), edu = col("edu_hi"), tr = col("treated");
    int k = 4;  // age_z, edu_hi, treated, const
    std::vector<double> X(static_cast<size_t>(n) * k);
    std::copy(age.begin(), age.end(), X.begin());
    std::copy(edu.begin(), edu.end(), X.begin() + n);
    std::copy(tr.begin(), tr.end(), X.begin() + 2 * n);
    for (int64_t i = 0; i < n; ++i) X[3 * n + i] = 1.0;

    TobitResult r = tobit_fit(X.data(), n, k, y.data(), true, 0.0, false, 0.0);

    int fail = 0;
    auto chk = [&](const char* tag, double got, double want, double tol) {
        double rel = std::fabs(got - want) / (std::fabs(want) + 1e-30);
        std::printf("  %-10s got=%.10g  stata=%.10g  rel=%.2e\n", tag, got, want, rel);
        if (!(rel <= tol)) { std::printf("    <<FAIL %s rel %.2e > %.0e\n", tag, rel, tol); fail = 1; }
    };
    const char* names[4] = {"age_z", "edu_hi", "treated", "_cons"};
    for (int j = 0; j < 4; ++j) {
        chk((std::string(names[j]) + "_b").c_str(), r.beta[j], g[names[j]].first, 1e-5);
        chk((std::string(names[j]) + "_se").c_str(), r.se[j], g[names[j]].second, 1e-4);
    }
    // Stata reports the residual variance var(e.y) = σ² (with SE = 2σ·SE(σ))
    chk("variance", r.variance, g["sigma"].first, 1e-5);
    chk("variance_se", r.se_variance, g["sigma"].second, 1e-4);
    chk("ll", r.ll, g["ll"].first, 1e-8);
    chk("N_lc", static_cast<double>(r.n_lc), g["N_lc"].first, 1e-12);

    std::printf("converged=%d iters=%d  N_unc=%lld N_lc=%lld\n", r.converged, r.iters,
                (long long)r.n_unc, (long long)r.n_lc);
    std::printf("%s\n", fail ? "FAILED" : "PASSED");
    return fail;
}
