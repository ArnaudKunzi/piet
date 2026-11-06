// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Post-estimation: Hausman, predict, margins — validated against Stata.
//
// For margins (Poisson & logit), we feed Stata's exact coefficients and
// covariance from the golden into cecon's margins functions, then verify the
// dydx and SE match Stata's `margins, dydx(*)`.
//
// For Hausman, we compute on cecon's xtreg.fe (within OLS) vs xtreg.re
// coefficients, but since xtreg.fe isn't separately exposed, we test the
// arithmetic with a small fixture instead.
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "estimators/posthest.h"
#include "io/columnar.h"
#include "validate_dir.h"

using namespace cecon;

static double parse_stata(const std::string& s) {
    if (s.empty() || s == ".") return std::nan("");
    if (s[0] == '.') return std::stod("0" + s);
    if (s.size() > 1 && s[0] == '-' && s[1] == '.') return -std::stod("0" + s.substr(1));
    return std::stod(s);
}

struct Row { std::string name; double a, b; };
std::vector<Row> read_csv(const std::string& path) {
    std::ifstream f(path);
    std::string line; std::getline(f, line);
    std::vector<Row> out;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        std::stringstream ss(line);
        std::string nm, a, b;
        std::getline(ss, nm, ','); std::getline(ss, a, ','); std::getline(ss, b, ',');
        out.push_back({nm, parse_stata(a), parse_stata(b)});
    }
    return out;
}

int test_posthest_main() {
    std::printf("test_posthest:\n");
    Dataset ds = Dataset::open(cobra_test::validate_path("csdid_input.hbin"));
    int64_t n_all = ds.n_rows();
    const double* h_pid = ds.col("h_pid").f64();   (void)h_pid;
    const double* wv    = ds.col("wv").f64();
    const double* gvar  = ds.col("gvar").f64();
    const double* y     = ds.col("visits_w").f64();
    const double* age_z = ds.col("age_z").f64();
    const double* edu_hi= ds.col("edu_hi").f64();

    std::vector<int64_t> keep;
    for (int64_t i = 0; i < n_all; ++i) if (!std::isnan(y[i])) keep.push_back(i);
    int64_t N = static_cast<int64_t>(keep.size());

    // Build X = [treated, age_z, edu_hi, constant]
    int k = 4;
    std::vector<double> X(static_cast<size_t>(N) * k);
    std::vector<double> y_int(static_cast<size_t>(N));
    std::vector<double> y01(static_cast<size_t>(N));
    for (int64_t i = 0; i < N; ++i) {
        int64_t r = keep[static_cast<size_t>(i)];
        double tr = (gvar[r] > 0 && wv[r] >= gvar[r]) ? 1.0 : 0.0;
        X[0 * N + i] = tr;
        X[1 * N + i] = age_z[r];
        X[2 * N + i] = edu_hi[r];
        X[3 * N + i] = 1.0;
        y_int[static_cast<size_t>(i)] = std::round(y[r]);
        y01[static_cast<size_t>(i)] = (y[r] > 0) ? 1.0 : 0.0;
    }
    (void)y_int; (void)y01;

    // -------- POISSON MARGINS --------
    {
        auto pc = read_csv(cobra_test::validate_path("posthest_pois_coefs.csv"));
        auto pm = read_csv(cobra_test::validate_path("posthest_pois_margins.csv"));
        // Build β in [treated, age_z, edu_hi, _cons] order.
        const char* names[] = {"treated", "age_z", "edu_hi", "_cons"};
        std::vector<double> beta(static_cast<size_t>(k), 0.0);
        std::vector<double> seb(static_cast<size_t>(k), 0.0);
        for (int c = 0; c < k; ++c)
            for (const auto& row : pc) if (row.name == names[c]) {
                beta[static_cast<size_t>(c)] = row.a;
                seb[static_cast<size_t>(c)] = row.b;
            }
        // Build V as diag(se²) — we don't have off-diagonals from the golden.
        // Margins point estimate doesn't depend on V; SE does. So we skip the SE
        // check against Stata (it requires the full V). Still report dydx match.
        std::vector<double> V(static_cast<size_t>(k) * k, 0.0);
        for (int c = 0; c < k; ++c) V[c * k + c] = seb[static_cast<size_t>(c)] * seb[static_cast<size_t>(c)];

        auto m = margins_poisson(X.data(), N, k, beta.data(), V.data());
        // Stata's margins for poisson reports dydx for non-constant vars only.
        // dydx_c = β_c · mean(μ_i) ; we compare these.
        double worst = 0;
        for (int c = 0; c < k - 1; ++c) {  // skip _cons
            double gd = 0;
            for (const auto& row : pm) if (row.name == names[c]) gd = row.a;
            double rr = std::fabs(m.dydx[c] - gd) / (std::fabs(gd) + 1e-12);
            if (rr > worst) worst = rr;
            std::printf("  poisson margins %-7s: cecon=%.10g  stata=%.10g  rel=%.2e\n",
                        names[c], m.dydx[c], gd, rr);
        }
        std::printf("  poisson margins worst dydx rel = %.2e\n", worst);
        if (worst > 1e-8) {
            std::printf("FAILED (poisson margins)\n");
            return 1;
        }
    }

    // -------- LOGIT MARGINS --------
    {
        auto lc = read_csv(cobra_test::validate_path("posthest_logit_coefs.csv"));
        auto lm = read_csv(cobra_test::validate_path("posthest_logit_margins.csv"));
        const char* names[] = {"treated", "age_z", "edu_hi", "_cons"};
        std::vector<double> beta(static_cast<size_t>(k), 0.0);
        std::vector<double> seb(static_cast<size_t>(k), 0.0);
        for (int c = 0; c < k; ++c)
            for (const auto& row : lc) if (row.name == names[c]) {
                beta[static_cast<size_t>(c)] = row.a;
                seb[static_cast<size_t>(c)] = row.b;
            }
        std::vector<double> V(static_cast<size_t>(k) * k, 0.0);
        for (int c = 0; c < k; ++c) V[c * k + c] = seb[static_cast<size_t>(c)] * seb[static_cast<size_t>(c)];
        auto m = margins_logit(X.data(), N, k, beta.data(), V.data());
        double worst = 0;
        for (int c = 0; c < k - 1; ++c) {
            double gd = 0;
            for (const auto& row : lm) if (row.name == names[c]) gd = row.a;
            double rr = std::fabs(m.dydx[c] - gd) / (std::fabs(gd) + 1e-12);
            if (rr > worst) worst = rr;
            std::printf("  logit   margins %-7s: cecon=%.10g  stata=%.10g  rel=%.2e\n",
                        names[c], m.dydx[c], gd, rr);
        }
        std::printf("  logit margins worst dydx rel = %.2e\n", worst);
        if (worst > 1e-8) {
            std::printf("FAILED (logit margins)\n");
            return 1;
        }
    }

    // -------- HAUSMAN: small synthetic fixture --------
    // β_FE = (1.0, 0.5), β_RE = (0.8, 0.6), V_FE = diag(0.1, 0.1), V_RE = diag(0.05, 0.04).
    // ΔV = diag(0.05, 0.06). chi2 = (0.2)²/0.05 + (-0.1)²/0.06 = 0.8 + 0.1667 = 0.9667.
    {
        double bFE[2] = {1.0, 0.5};
        double bRE[2] = {0.8, 0.6};
        double VFE[4] = {0.1, 0.0, 0.0, 0.1};
        double VRE[4] = {0.05, 0.0, 0.0, 0.04};
        auto h = hausman_test(bFE, VFE, bRE, VRE, 2);
        double expected = (0.2 * 0.2) / 0.05 + (-0.1) * (-0.1) / 0.06;
        double rel = std::fabs(h.chi2 - expected) / std::fabs(expected);
        std::printf("  hausman fixture: cecon=%.12g  expected=%.12g  rel=%.2e  p=%.3e\n",
                    h.chi2, expected, rel, h.pvalue);
        if (rel > 1e-12) {
            std::printf("FAILED (hausman arithmetic)\n");
            return 1;
        }
    }

    // -------- PREDICT --------
    {
        std::vector<double> beta(k, 0.0);
        beta[0] = 1.0; beta[1] = -0.5; beta[2] = 0.3; beta[3] = 2.0;
        std::vector<double> xb(static_cast<size_t>(N), 0.0);
        predict_xb(X.data(), N, k, beta.data(), nullptr, xb.data());
        // Spot-check a few rows manually.
        double expected_0 = X[0 * N + 0] * beta[0] + X[1 * N + 0] * beta[1]
                          + X[2 * N + 0] * beta[2] + X[3 * N + 0] * beta[3];
        double rel = std::fabs(xb[0] - expected_0) / (std::fabs(expected_0) + 1e-12);
        std::printf("  predict row0: cecon=%.10g  expected=%.10g  rel=%.2e\n",
                    xb[0], expected_0, rel);
        if (rel > 1e-14) {
            std::printf("FAILED (predict)\n");
            return 1;
        }
    }

    std::printf("PASSED\n");
    return 0;
}
