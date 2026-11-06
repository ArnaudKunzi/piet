// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// xtreg, re validated against Stata (Swamy-Arora variance components).
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "estimators/xtreg.h"
#include "io/columnar.h"
#include "validate_dir.h"

using namespace cecon;

static double parse_stata(const std::string& s) {
    if (s.empty() || s == ".") return std::nan("");
    if (s[0] == '.') return std::stod("0" + s);
    if (s.size() > 1 && s[0] == '-' && s[1] == '.') return -std::stod("0" + s.substr(1));
    return std::stod(s);
}

int test_xtreg_main() {
    std::printf("test_xtreg:\n");
    Dataset ds = Dataset::open(cobra_test::validate_path("csdid_input.hbin"));
    int64_t n = ds.n_rows();
    const double* h_pid = ds.col("h_pid").f64();
    const double* wv    = ds.col("wv").f64();
    const double* gvar  = ds.col("gvar").f64();
    const double* y     = ds.col("visits_w").f64();
    const double* female= ds.col("female").f64();
    const double* age_z = ds.col("age_z").f64();
    const double* edu_hi= ds.col("edu_hi").f64();

    std::vector<int64_t> keep;
    for (int64_t i = 0; i < n; ++i) if (!std::isnan(y[i])) keep.push_back(i);
    int64_t N = static_cast<int64_t>(keep.size());
    const int k = 4;
    std::vector<double> X(static_cast<size_t>(N) * k);
    std::vector<double> y_use(static_cast<size_t>(N));
    std::vector<double> ivar(static_cast<size_t>(N));
    for (int64_t i = 0; i < N; ++i) {
        int64_t r = keep[static_cast<size_t>(i)];
        double tr = (gvar[r] > 0 && wv[r] >= gvar[r]) ? 1.0 : 0.0;
        X[0 * N + i] = tr;
        X[1 * N + i] = female[r];
        X[2 * N + i] = age_z[r];
        X[3 * N + i] = edu_hi[r];
        y_use[static_cast<size_t>(i)] = y[r];
        ivar[static_cast<size_t>(i)] = h_pid[r];
    }
    auto R = xtreg_re(X.data(), N, k, y_use.data(), ivar.data());
    std::printf("  N=%lld Ng=%lld  sigma_e=%.10g sigma_u=%.10g rho=%.10g T_h=%.10g\n",
                (long long)R.n_obs, (long long)R.n_groups,
                R.sigma_e, R.sigma_u, R.rho, R.T_h);

    // Golden
    std::ifstream f(cobra_test::validate_path("xtreg_re_golden.csv"));
    std::string line; std::getline(f, line);
    std::vector<std::tuple<std::string,double,double>> gold;
    while (std::getline(f, line)) {
        std::stringstream ss(line);
        std::string nm, b, s;
        std::getline(ss, nm, ','); std::getline(ss, b, ','); std::getline(ss, s, ',');
        gold.push_back({nm, parse_stata(b), parse_stata(s)});
    }
    const char* names[] = {"treated", "female", "age_z", "edu_hi", "_cons"};
    double worst_b = 0, worst_se = 0;
    for (int c = 0; c < k + 1; ++c) {
        double gb = 0, gse = 0;
        for (const auto& [nm, b, s] : gold) if (nm == names[c]) { gb = b; gse = s; }
        double rb  = std::fabs(R.beta[c] - gb)  / (std::fabs(gb)  + 1e-12);
        double rse = std::fabs(R.se[c]   - gse) / (std::fabs(gse) + 1e-12);
        std::printf("  %-8s: b=%+.10g (stata %+.10g rel %.2e)  se=%.10g (stata %.10g rel %.2e)\n",
                    names[c], R.beta[c], gb, rb, R.se[c], gse, rse);
        if (rb  > worst_b ) worst_b  = rb;
        if (rse > worst_se) worst_se = rse;
    }
    // Sigma comparisons
    double sg_e = 26.3804527611603, sg_u = 15.7037974601721, sg_rho = 0.261643959562492, sg_Th = 4.34960182425891;
    double rse_e = std::fabs(R.sigma_e - sg_e) / sg_e;
    double rse_u = std::fabs(R.sigma_u - sg_u) / sg_u;
    double rse_rho = std::fabs(R.rho - sg_rho) / sg_rho;
    double rse_Th = std::fabs(R.T_h - sg_Th) / sg_Th;
    std::printf("  sigma_e rel=%.2e  sigma_u rel=%.2e  rho rel=%.2e  T_h rel=%.2e\n",
                rse_e, rse_u, rse_rho, rse_Th);

    int fail = 0;
    if (worst_b > 1e-9) fail = 1;
    if (worst_se > 1e-3) fail = 1;     // SE formula may differ; tolerate
    if (rse_e > 1e-8 || rse_u > 1e-6 || rse_Th > 1e-12) fail = 1;
    std::printf("%s\n", fail ? "FAILED" : "PASSED");
    return fail;
}
