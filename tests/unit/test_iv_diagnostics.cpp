// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// IV diagnostics parity test vs Stata estat endogenous / overid / firststage after
// ivregress 2sls visits_w treated (age_z = female edu_hi). Golden from
// ivdiag_golden.do. Durbin/Wu-Hausman/Sargan/Basmann + first-stage F/partial R².
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "estimators/iv_diagnostics.h"
#include "io/columnar.h"
#include "validate_dir.h"

using namespace cecon;

int test_iv_diagnostics_main() {
    std::printf("test_iv_diagnostics:\n");
    const cobra_test::ValidateBase base;

    std::map<std::string, double> g;
    {
        std::ifstream f(base + "ivdiag_golden.csv");
        std::string line;
        std::getline(f, line);
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            std::stringstream ss(line);
            std::string k, v;
            std::getline(ss, k, ',');
            std::getline(ss, v, ',');
            g[k] = std::stod(v);
        }
    }

    Dataset ds = Dataset::open(base + "ivdiag.hbin");
    int64_t n = ds.n_rows();
    auto colvec = [&](const char* nm) {
        const double* p = ds.col(nm).f64();
        return std::vector<double>(p, p + n);
    };
    std::vector<double> y = colvec("visits_w");
    std::vector<double> Y = colvec("age_z");          // endogenous (p1=1)
    std::vector<double> treated = colvec("treated");
    std::vector<double> X(static_cast<size_t>(n) * 2);  // [treated, const]
    std::copy(treated.begin(), treated.end(), X.begin());
    for (int64_t i = 0; i < n; ++i) X[static_cast<int64_t>(1) * n + i] = 1.0;
    std::vector<double> Z2(static_cast<size_t>(n) * 2);  // [female, edu_hi]
    std::vector<double> female = colvec("female"), edu = colvec("edu_hi");
    std::copy(female.begin(), female.end(), Z2.begin());
    std::copy(edu.begin(), edu.end(), Z2.begin() + n);

    IvDiag r = iv_diagnostics(y, n, Y, 1, X, 2, Z2, 2);

    int fail = 0;
    auto chk = [&](const char* tag, double got, double want, double tol) {
        double rel = std::fabs(got - want) / (std::fabs(want) + 1e-30);
        std::printf("  %-12s got=%.12g  stata=%.12g  rel=%.2e\n", tag, got, want, rel);
        if (!(rel <= tol)) { std::printf("    <<FAIL %s rel %.2e > %.0e\n", tag, rel, tol); fail = 1; }
    };

    chk("durbin", r.durbin, g["durbin"], 1e-9);
    chk("durbin_p", r.durbin_p, g["durbin_p"], 1e-6);
    chk("wu", r.wu, g["wu"], 1e-9);
    chk("wu_df_r", r.wu_df_r, g["wu_dfr"], 1e-12);
    chk("sargan", r.sargan, g["sargan"], 1e-9);
    chk("basmann", r.basmann, g["basmann"], 1e-9);
    chk("overid_df", r.overid_df, g["overid_df"], 1e-12);
    chk("fs_F", r.fs_F[0], g["fs_F"], 1e-9);
    chk("fs_r2", r.fs_r2[0], g["fs_r2"], 1e-9);
    chk("fs_partial_r2", r.fs_partial_r2[0], g["fs_par_r2"], 1e-9);

    std::printf("%s\n", fail ? "FAILED" : "PASSED");
    return fail;
}
