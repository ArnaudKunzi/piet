// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Mediation parity test: product-of-coefficients a*b + Sobel SE vs Stata regress.
// M-model: age_z ~ edu_hi (a = edu_hi); Y-model: visits_w ~ edu_hi + age_z
// (b = age_z, direct c' = edu_hi). indirect = a*b; Sobel = sqrt(a^2 se_b^2 + b^2 se_a^2).
// Golden from mediate_golden.do on the complete-case sample (tests/validate/mediate.hbin).
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "core/model.h"
#include "estimators/feols.h"
#include "io/columnar.h"
#include "validate_dir.h"

using namespace cecon;

static int idx_of(const std::vector<std::string>& nm, const std::string& s) {
    for (size_t i = 0; i < nm.size(); ++i) if (nm[i] == s) return (int)i;
    return -1;
}

int test_mediate_main() {
    std::printf("test_mediate:\n");
    const cobra_test::ValidateBase base;

    std::ifstream f(base + "mediate_golden.csv");
    std::string line; std::getline(f, line); std::getline(f, line);
    std::stringstream ss(line);
    std::vector<double> g; std::string cell;
    while (std::getline(ss, cell, ',')) g.push_back(std::stod(cell));
    double ga = g[0], gsa = g[1], gb = g[2], gsb = g[3], gcp = g[4], gind = g[5], gsob = g[6];

    Dataset ds = Dataset::open(base + "mediate.hbin");
    // M-model: age_z ~ edu_hi
    ModelData mM = build_model(ds, "age_z", {"edu_hi"}, {}, {});
    FeolsResult rM = feols_fit(mM, Vcov::IID);
    int ai = idx_of(rM.names, "edu_hi");
    double a = rM.beta[ai], se_a = rM.se[ai];
    // Y-model: visits_w ~ edu_hi + age_z
    ModelData mY = build_model(ds, "visits_w", {"edu_hi", "age_z"}, {}, {});
    FeolsResult rY = feols_fit(mY, Vcov::IID);
    int bi = idx_of(rY.names, "age_z"), ci = idx_of(rY.names, "edu_hi");
    double b = rY.beta[bi], se_b = rY.se[bi], cp = rY.beta[ci];
    double indirect = a * b;
    double sobel = std::sqrt(a * a * se_b * se_b + b * b * se_a * se_a);

    struct Chk { const char* nm; double got, want; double tol; };
    Chk chks[] = {{"a", a, ga, 1e-9}, {"se_a", se_a, gsa, 1e-9}, {"b", b, gb, 1e-9},
                  {"se_b", se_b, gsb, 1e-9}, {"direct", cp, gcp, 1e-9},
                  {"indirect", indirect, gind, 1e-9}, {"sobel", sobel, gsob, 1e-9}};
    int fail = 0;
    for (auto& c : chks) {
        double rel = std::fabs(c.got - c.want) / (std::fabs(c.want) + 1e-30);
        std::printf("  %-9s %.12g (stata %.12g  rel %.1e)\n", c.nm, c.got, c.want, rel);
        if (!(rel <= c.tol)) { std::printf("    <<FAIL rel %.2e\n", rel); fail = 1; }
    }
    std::printf("%s\n", fail ? "FAILED" : "PASSED");
    return fail;
}
