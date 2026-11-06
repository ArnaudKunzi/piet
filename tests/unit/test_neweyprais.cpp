// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// newey (HAC) + prais (AR(1) FGLS) parity test vs Stata. Model price ~ weight +
// length on sysuse auto (t=_n). Golden from neweyprais_golden.do. newey lag(0)
// reproduces regress vce(robust); HAC lag(2), prais iterated/twostep/corc/tscorr.
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "core/model.h"
#include "estimators/neweyprais.h"
#include "io/columnar.h"
#include "validate_dir.h"

using namespace cecon;

int test_neweyprais_main() {
    std::printf("test_neweyprais:\n");
    const cobra_test::ValidateBase base;

    std::map<std::string, double> g;
    {
        std::ifstream f(base + "neweyprais_golden.csv");
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

    Dataset ds = Dataset::open(base + "neweyprais.hbin");
    ModelData md = build_model(ds, "price", {"weight", "length"}, {}, {});
    const double* X = md.dm.X.data();
    const double* y = md.dm.y.data();
    const int64_t n = md.dm.n;
    const int k = md.dm.k;
    const double* t = ds.col("t").f64();
    // build_model intercept is column 0; X cols = [(intercept), weight, length].
    // index helper
    int iw = -1, il = -1, ic = -1;
    for (int j = 0; j < k; ++j) {
        if (md.dm.xnames[j] == "weight") iw = j;
        else if (md.dm.xnames[j] == "length") il = j;
        else if (md.dm.xnames[j] == "(intercept)") ic = j;
    }

    int fail = 0;
    auto chk = [&](const char* tag, double got, double want, double tol) {
        double rel = std::fabs(got - want) / (std::fabs(want) + 1e-30);
        std::printf("  %-10s got=%.12g  stata=%.12g  rel=%.2e\n", tag, got, want, rel);
        if (!(rel <= tol)) { std::printf("    <<FAIL %s rel %.2e > %.0e\n", tag, rel, tol); fail = 1; }
    };

    // NEWEY lag(2)
    NeweyResult nw2 = newey_fit(X, n, k, y, t, 1.0, 2);
    chk("nw2_b_w", nw2.beta[iw], g["nw2_b_w"], 1e-10);
    chk("nw2_se_w", nw2.se[iw], g["nw2_se_w"], 1e-9);
    chk("nw2_se_l", nw2.se[il], g["nw2_se_l"], 1e-9);
    chk("nw2_se_c", nw2.se[ic], g["nw2_se_c"], 1e-9);
    // NEWEY lag(0) == robust
    NeweyResult nw0 = newey_fit(X, n, k, y, t, 1.0, 0);
    chk("nw0_se_w", nw0.se[iw], g["rob_se_w"], 1e-9);
    chk("nw0_se_l", nw0.se[il], g["rob_se_l"], 1e-9);

    // PRAIS iterated (regress rhotype, PW)
    PraisResult pr = prais_fit(X, n, k, y, t, 1.0, false, false, "regress");
    chk("pr_b_w", pr.beta[iw], g["pr_b_w"], 1e-8);
    chk("pr_b_c", pr.beta[ic], g["pr_b_c"], 1e-8);
    chk("pr_se_w", pr.se[iw], g["pr_se_w"], 1e-7);
    chk("pr_rho", pr.rho, g["pr_rho"], 1e-7);
    chk("pr_N", pr.n, g["pr_N"], 1e-12);
    chk("pr_dw", pr.dw, g["pr_dw"], 1e-7);
    chk("pr_dw0", pr.dw_0, g["pr_dw0"], 1e-9);

    // PRAIS twostep
    PraisResult pr2 = prais_fit(X, n, k, y, t, 1.0, false, true, "regress");
    chk("pr2_rho", pr2.rho, g["pr2_rho"], 1e-8);
    chk("pr2_b_w", pr2.beta[iw], g["pr2_b_w"], 1e-8);

    // PRAIS corc
    PraisResult prc = prais_fit(X, n, k, y, t, 1.0, true, false, "regress");
    chk("prc_rho", prc.rho, g["prc_rho"], 1e-7);
    chk("prc_b_w", prc.beta[iw], g["prc_b_w"], 1e-8);
    chk("prc_N", prc.n, g["prc_N"], 1e-12);

    // PRAIS tscorr rhotype
    PraisResult prt = prais_fit(X, n, k, y, t, 1.0, false, false, "tscorr");
    chk("prt_rho", prt.rho, g["prt_rho"], 1e-7);
    chk("prt_b_w", prt.beta[iw], g["prt_b_w"], 1e-8);

    std::printf("%s\n", fail ? "FAILED" : "PASSED");
    return fail;
}
