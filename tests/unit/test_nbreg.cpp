// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// nbreg (NB2) validated against Stata nbreg.
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "estimators/nbreg.h"
#include "io/columnar.h"
#include "validate_dir.h"

using namespace cecon;

static double parse_stata(const std::string& s) {
    if (s.empty() || s == ".") return std::nan("");
    if (s[0] == '.') return std::stod("0" + s);
    if (s.size() > 1 && s[0] == '-' && s[1] == '.') return -std::stod("0" + s.substr(1));
    return std::stod(s);
}

int test_nbreg_main() {
    std::printf("test_nbreg:\n");
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

    // Match the do-file: round(visits_w) and add a constant.
    const int k = 5;  // treated, female, age_z, edu_hi, intercept
    std::vector<double> X(static_cast<size_t>(N) * k);
    std::vector<double> y_use(static_cast<size_t>(N));
    std::vector<double> cl(static_cast<size_t>(N));
    for (int64_t i = 0; i < N; ++i) {
        int64_t r = keep[static_cast<size_t>(i)];
        double tr = (gvar[r] > 0 && wv[r] >= gvar[r]) ? 1.0 : 0.0;
        X[0 * N + i] = tr;
        X[1 * N + i] = female[r];
        X[2 * N + i] = age_z[r];
        X[3 * N + i] = edu_hi[r];
        X[4 * N + i] = 1.0;       // intercept
        y_use[static_cast<size_t>(i)] = std::round(y[r]);
        cl[static_cast<size_t>(i)] = h_pid[r];
    }

    // iid SE first
    auto Rii = nbreg(X.data(), N, k, y_use.data(), nullptr, nullptr);
    std::printf("  iid: conv=%d iters=%d ll=%.6f alpha=%.10g lnalpha=%.10g\n",
                Rii.converged?1:0, Rii.iters, Rii.loglik, Rii.alpha, Rii.lnalpha);

    // Read golden
    std::ifstream f(cobra_test::validate_path("nbreg_golden.csv"));
    std::string line; std::getline(f, line);
    std::vector<std::tuple<std::string,double,double,double,double>> gold;
    while (std::getline(f, line)) {
        std::stringstream ss(line);
        std::string nm, a, b, c, d;
        std::getline(ss, nm, ','); std::getline(ss, a, ','); std::getline(ss, b, ',');
        std::getline(ss, c, ','); std::getline(ss, d, ',');
        gold.push_back({nm, parse_stata(a), parse_stata(b), parse_stata(c), parse_stata(d)});
    }
    const char* names[] = {"treated", "female", "age_z", "edu_hi", "_cons"};
    double worst_b = 0, worst_seii = 0;
    for (int c = 0; c < k; ++c) {
        double gb = 0, gsei = 0, gbr = 0, gser = 0;
        for (const auto& [nm, b, se, br, ser] : gold)
            if (nm == names[c]) { gb = b; gsei = se; gbr = br; gser = ser; }
        double rb  = std::fabs(Rii.beta[c]   - gb)   / (std::fabs(gb)   + 1e-12);
        double rs  = std::fabs(Rii.se_iid[c] - gsei) / (std::fabs(gsei) + 1e-12);
        std::printf("  %-8s: b=%+.10g (stata %+.10g rel %.2e)  se_iid=%.10g (stata %.10g rel %.2e)\n",
                    names[c], Rii.beta[c], gb, rb, Rii.se_iid[c], gsei, rs);
        if (rb > worst_b)   worst_b = rb;
        if (rs > worst_seii)worst_seii = rs;
    }
    // lnalpha
    double exp_lna = -0.130303695017804;
    double rln = std::fabs(Rii.lnalpha - exp_lna) / std::fabs(exp_lna);
    std::printf("  lnalpha rel=%.2e (cecon=%.12g stata=%.12g)\n",
                rln, Rii.lnalpha, exp_lna);

    // LL
    double exp_ll = -313234.613453349;
    double rll = std::fabs(Rii.loglik - exp_ll) / std::fabs(exp_ll);
    std::printf("  ll rel=%.2e (cecon=%.6f stata=%.6f)\n", rll, Rii.loglik, exp_ll);

    // Robust SE: Stata's vce(robust) is HC (heteroscedasticity-robust), no cluster.
    auto Rrob = nbreg(X.data(), N, k, y_use.data(), nullptr, nullptr);
    (void)cl;  // not used in this golden's vce(robust) check
    double worst_serob = 0;
    for (int c = 0; c < k; ++c) {
        double gser = 0;
        for (const auto& [nm, b, se, br, ser] : gold) if (nm == names[c]) gser = ser;
        double rs = std::fabs(Rrob.se_robust[c] - gser) / (std::fabs(gser) + 1e-12);
        std::printf("  %-8s: se_rob=%.10g (stata %.10g rel %.2e)\n",
                    names[c], Rrob.se_robust[c], gser, rs);
        if (rs > worst_serob) worst_serob = rs;
    }

    int fail = 0;
    if (!Rii.converged) fail = 1;
    if (worst_b > 1e-6) fail = 1;
    if (worst_seii > 1e-5) fail = 1;
    if (rln > 1e-6) fail = 1;
    if (rll > 1e-10) fail = 1;
    if (worst_serob > 5e-3) fail = 1;
    std::printf("%s\n", fail ? "FAILED" : "PASSED");
    return fail;
}
