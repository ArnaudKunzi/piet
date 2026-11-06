// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// glm quasi-Poisson scale(x2) parity test vs Stata. Pooled Poisson coefs == fepois;
// SEs scaled by sqrt(Pearson dispersion phi). Golden from glmqp_golden.do (~1e-6).
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "core/model.h"
#include "estimators/fepois.h"
#include "io/columnar.h"
#include "validate_dir.h"

using namespace cecon;

int test_glmqp_main() {
    std::printf("test_glmqp:\n");
    const cobra_test::ValidateBase base;
    std::map<std::string, std::pair<double, double>> g;
    {
        std::ifstream f(base + "glmqp_golden.csv");
        std::string line;
        std::getline(f, line);
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            std::stringstream ss(line);
            std::string nm, b, se;
            std::getline(ss, nm, ','); std::getline(ss, b, ','); std::getline(ss, se, ',');
            if (b == "." || se == ".") continue;  // skip missing rows (phi display)
            g[nm] = {std::stod(b), std::stod(se)};
        }
    }
    Dataset ds = Dataset::open(base + "glmqp.hbin");
    ModelData md = build_model(ds, "visits_w", {"age_z", "edu_hi", "treated"}, {}, {});
    FepoisResult r = fepois_fit(md, Vcov::IID);
    const int64_t n = md.dm.n;
    const int k = r.k;
    // Pearson dispersion phi = Σ(y-μ)²/μ / (n-k)
    double pearson = 0;
    for (int64_t i = 0; i < n; ++i) {
        double eta = 0;
        for (int j = 0; j < k; ++j) eta += md.dm.X[static_cast<int64_t>(j) * n + i] * r.beta[j];
        double mu = std::exp(eta), e = md.dm.y[i] - mu;
        if (mu > 0) pearson += e * e / mu;
    }
    double phi = pearson / static_cast<double>(n - k);
    double sq = std::sqrt(phi);

    std::map<std::string, int> idx;
    for (int j = 0; j < k; ++j) idx[r.names[j]] = j;
    auto nm = [&](const char* g_name) { return std::string(g_name) == "_cons" ? "(intercept)" : g_name; };

    int fail = 0;
    auto chk = [&](const char* tag, double got, double want, double tol) {
        double rel = std::fabs(got - want) / (std::fabs(want) + 1e-30);
        std::printf("  %-12s got=%.10g stata=%.10g rel=%.2e\n", tag, got, want, rel);
        if (!(rel <= tol)) { std::printf("    <<FAIL\n"); fail = 1; }
    };
    for (const char* c : {"age_z", "edu_hi", "treated", "_cons"}) {
        int j = idx[nm(c)];
        chk((std::string(c) + "_b").c_str(), r.beta[j], g[c].first, 1e-6);
        chk((std::string(c) + "_se").c_str(), r.se[j] * sq, g[c].second, 1e-6);
    }
    std::printf("phi=%.6f\n%s\n", phi, fail ? "FAILED" : "PASSED");
    return fail;
}
