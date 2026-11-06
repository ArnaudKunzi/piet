// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// reg3 systems parity test vs Stata: SUR (reg3 ..., sure) and 3SLS (reg3 default
// with an endogenous RHS). Golden from reg3_golden.do. Coefs, SEs, and Σ̂ match
// Stata to ~1e-9.
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "estimators/reg3.h"
#include "io/columnar.h"
#include "validate_dir.h"

using namespace cecon;

int test_reg3_main() {
    std::printf("test_reg3:\n");
    const cobra_test::ValidateBase base;
    std::map<std::string, double> g;
    {
        std::ifstream f(base + "reg3_golden.csv");
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
    Dataset ds = Dataset::open(base + "reg3.hbin");
    int64_t n = ds.n_rows();
    auto col = [&](const char* nm) { const double* p = ds.col(nm).f64(); return std::vector<double>(p, p + n); };
    auto x1 = col("x1"), x2 = col("x2"), x3 = col("x3"), y1 = col("y1"), y2 = col("y2"), y1s = col("y1s");
    std::vector<double> one(n, 1.0);
    auto mat = [&](std::vector<std::vector<double>> cols) {
        int k = static_cast<int>(cols.size());
        std::vector<double> M(static_cast<size_t>(n) * k);
        for (int c = 0; c < k; ++c) std::copy(cols[c].begin(), cols[c].end(), M.begin() + static_cast<int64_t>(c) * n);
        return M;
    };

    int fail = 0;
    auto chk = [&](const char* tag, double got, double want, double tol) {
        double rel = std::fabs(got - want) / (std::fabs(want) + 1e-30);
        std::printf("  %-12s got=%.10g  stata=%.10g  rel=%.2e\n", tag, got, want, rel);
        if (!(rel <= tol)) { std::printf("    <<FAIL %s rel %.2e > %.0e\n", tag, rel, tol); fail = 1; }
    };

    // ===== SUR: (y1 x1 x2) (y2 x2 x3) =====
    {
        std::vector<Reg3Eq> eqs(2);
        eqs[0].Z = mat({x1, x2, one}); eqs[0].k = 3; eqs[0].y = y1; eqs[0].endog = {0, 0, 0};
        eqs[1].Z = mat({x2, x3, one}); eqs[1].k = 3; eqs[1].y = y2; eqs[1].endog = {0, 0, 0};
        std::vector<double> W = mat({x1, x2, x3, one});
        Reg3Result r = reg3_fit(eqs, n, W, 4);
        chk("sur_b1", r.beta[0], g["sur_b1"], 1e-9);
        chk("sur_se1", r.se[0], g["sur_se1"], 1e-8);
        chk("sur_b2", r.beta[1], g["sur_b2"], 1e-9);
        chk("sur_bc", r.beta[2], g["sur_bc"], 1e-9);
        chk("sur_bcse", r.se[2], g["sur_bcse"], 1e-8);
        chk("sur_b4", r.beta[3], g["sur_b4"], 1e-9);
        chk("sur_se4", r.se[3], g["sur_se4"], 1e-8);
        chk("sur_b5", r.beta[4], g["sur_b5"], 1e-9);
        chk("sur_sig11", r.Sigma[0], g["sur_sig11"], 1e-9);
        chk("sur_sig12", r.Sigma[1], g["sur_sig12"], 1e-9);
        chk("sur_sig22", r.Sigma[3], g["sur_sig22"], 1e-9);
    }
    // ===== 3SLS: (y1s y2 x1) (y2 x2 x3) ; y2 endogenous in eq1 =====
    {
        std::vector<Reg3Eq> eqs(2);
        eqs[0].Z = mat({y2, x1, one}); eqs[0].k = 3; eqs[0].y = y1s; eqs[0].endog = {1, 0, 0};
        eqs[1].Z = mat({x2, x3, one}); eqs[1].k = 3; eqs[1].y = y2; eqs[1].endog = {0, 0, 0};
        std::vector<double> W = mat({x1, x2, x3, one});
        Reg3Result r = reg3_fit(eqs, n, W, 4);
        chk("tsls_by2", r.beta[0], g["tsls_by2"], 1e-8);
        chk("tsls_by2se", r.se[0], g["tsls_by2se"], 1e-7);
        chk("tsls_bx1", r.beta[1], g["tsls_bx1"], 1e-8);
        chk("tsls_bc", r.beta[2], g["tsls_bc"], 1e-8);
        chk("tsls_bcse", r.se[2], g["tsls_bcse"], 1e-7);
        chk("tsls_e2x2", r.beta[3], g["tsls_e2x2"], 1e-8);
        chk("tsls_e2x2se", r.se[3], g["tsls_e2x2se"], 1e-7);
    }

    std::printf("%s\n", fail ? "FAILED" : "PASSED");
    return fail;
}
