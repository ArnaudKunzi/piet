// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Quantile regression parity test: qreg_fit vs Stata `qreg` (default vce iid,
// fitted sparsity, Hall-Sheather bandwidth) at tau = 0.25/0.5/0.75. Validates the
// Frisch-Newton coefficients, the sparsity, and the i.i.d. SEs.
// Golden from qreg_golden.do. Coef name order: age_z, edu_hi, _cons(=intercept).
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "core/model.h"
#include "estimators/qreg.h"
#include "io/columnar.h"
#include "validate_dir.h"

using namespace cecon;

int test_qreg_main() {
    std::printf("test_qreg:\n");
    const cobra_test::ValidateBase base;

    // golden[(tau,name)] = {b, se}; plus sparsity per tau
    std::map<std::pair<std::string, std::string>, std::pair<double, double>> gold;
    std::map<std::string, double> gsp;
    std::ifstream f(base + "qreg_golden.csv");
    std::string line;
    std::getline(f, line);  // header
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        std::stringstream ss(line);
        std::string tau, name, b, se, sp;
        std::getline(ss, tau, ','); std::getline(ss, name, ',');
        std::getline(ss, b, ','); std::getline(ss, se, ','); std::getline(ss, sp, ',');
        // Stata _cons -> cecon (intercept)
        if (name == "_cons") name = "(intercept)";
        gold[{tau, name}] = {std::stod(b), std::stod(se)};
        gsp[tau] = std::stod(sp);
    }

    Dataset ds = Dataset::open(base + "csdid_input.hbin");
    ModelData md = build_model(ds, "visits_w", {"age_z", "edu_hi"}, {}, {});

    int fail = 0;
    for (const std::string& tau : {std::string("0.25"), std::string("0.5"), std::string("0.75")}) {
        double q = std::stod(tau);
        QregResult r = qreg_fit(md.dm.X.data(), md.dm.n, md.dm.k, md.dm.y.data(), q, md.dm.xnames, 95.0);
        if (!r.converged) { std::printf("  tau=%s <<FAIL not converged\n", tau.c_str()); fail = 1; }
        double rsp = std::fabs(r.sparsity - gsp[tau]) / (std::fabs(gsp[tau]) + 1e-30);
        std::printf("  tau=%s sparsity=%.8g (stata %.8g rel %.1e)\n", tau.c_str(), r.sparsity, gsp[tau], rsp);
        if (!(rsp <= 1e-7)) { std::printf("    <<FAIL sparsity rel %.2e\n", rsp); fail = 1; }
        for (int j = 0; j < md.dm.k; ++j) {
            const std::string& nm = r.names[j];
            auto g = gold[{tau, nm}];
            // coefficients can be ~0 (use absolute tol there); SEs use relative tol
            double db = std::fabs(r.beta[j] - g.first);
            double rb = db / (std::fabs(g.first) + 1e-6);
            double rs = std::fabs(r.se[j] - g.second) / (std::fabs(g.second) + 1e-30);
            std::printf("    %-11s b=%.9g (stata %.9g)  se=%.9g (stata %.9g rel %.1e)\n",
                        nm.c_str(), r.beta[j], g.first, r.se[j], g.second, rs);
            if (!(rb <= 1e-6 || db <= 1e-6)) { std::printf("      <<FAIL b\n"); fail = 1; }
            if (!(rs <= 1e-6)) { std::printf("      <<FAIL se rel %.2e\n", rs); fail = 1; }
        }
    }
    std::printf("%s\n", fail ? "FAILED" : "PASSED");
    return fail;
}
