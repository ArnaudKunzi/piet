// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Unit-root parity test vs Stata dfuller (cases 1-4 + lags) and pperron.
// Persistent AR(1) series. Golden from unitroot_golden.do. Z(t)/Z(rho), Fuller
// critical values, MacKinnon p-values all match Stata to ~1e-9.
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "estimators/unitroot.h"
#include "io/columnar.h"
#include "validate_dir.h"

using namespace cecon;

int test_unitroot_main() {
    std::printf("test_unitroot:\n");
    const cobra_test::ValidateBase base;

    std::map<std::string, double> g;
    {
        std::ifstream f(base + "unitroot_golden.csv");
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

    Dataset ds = Dataset::open(base + "unitroot.hbin");
    int64_t n = ds.n_rows();
    const double* yp = ds.col("y").f64();
    const double* tp = ds.col("t").f64();
    std::vector<double> y(yp, yp + n), t(tp, tp + n);

    int fail = 0;
    auto chk = [&](const char* tag, double got, double want, double tol) {
        double rel = std::fabs(got - want) / (std::fabs(want) + 1e-30);
        std::printf("  %-10s got=%.10g  stata=%.10g  rel=%.2e\n", tag, got, want, rel);
        if (!(rel <= tol)) { std::printf("    <<FAIL %s rel %.2e > %.0e\n", tag, rel, tol); fail = 1; }
    };

    // dfuller default (case 2)
    DfullerResult d2 = dfuller_fit(y, t, n, 0, 2);
    chk("d2_Zt", d2.Zt, g["d2_Zt"], 1e-9);
    chk("d2_p", d2.p, g["d2_p"], 1e-7);
    chk("d2_cv1", d2.cv1, g["d2_cv1"], 1e-9);
    chk("d2_cv5", d2.cv5, g["d2_cv5"], 1e-9);
    chk("d2_cv10", d2.cv10, g["d2_cv10"], 1e-9);
    chk("d2_N", d2.n, g["d2_N"], 1e-12);
    // dfuller noconstant (case 1)
    DfullerResult d1 = dfuller_fit(y, t, n, 0, 1);
    chk("d1_Zt", d1.Zt, g["d1_Zt"], 1e-9);
    chk("d1_cv5", d1.cv5, g["d1_cv5"], 1e-9);
    // dfuller trend (case 4)
    DfullerResult d4 = dfuller_fit(y, t, n, 0, 4);
    chk("d4_Zt", d4.Zt, g["d4_Zt"], 1e-9);
    chk("d4_p", d4.p, g["d4_p"], 1e-7);
    chk("d4_cv5", d4.cv5, g["d4_cv5"], 1e-9);
    // dfuller drift (case 3)
    DfullerResult d3 = dfuller_fit(y, t, n, 0, 3);
    chk("d3_Zt", d3.Zt, g["d3_Zt"], 1e-9);
    chk("d3_p", d3.p, g["d3_p"], 1e-6);
    chk("d3_cv5", d3.cv5, g["d3_cv5"], 1e-7);
    // dfuller lags(2)
    DfullerResult dl = dfuller_fit(y, t, n, 2, 2);
    chk("dl_Zt", dl.Zt, g["dl_Zt"], 1e-9);
    chk("dl_p", dl.p, g["dl_p"], 1e-7);
    chk("dl_N", dl.n, g["dl_N"], 1e-12);

    // pperron default (case 2)
    PperronResult pp = pperron_fit(y, t, n, -1, 2);
    chk("pp_Zt", pp.Ztau, g["pp_Zt"], 1e-9);
    chk("pp_Zrho", pp.Zrho, g["pp_Zrho"], 1e-9);
    chk("pp_p", pp.p, g["pp_p"], 1e-7);
    chk("pp_lags", pp.lags, g["pp_lags"], 1e-12);
    // pperron trend (case 4)
    PperronResult ppt = pperron_fit(y, t, n, -1, 4);
    chk("ppt_Zt", ppt.Ztau, g["ppt_Zt"], 1e-9);
    chk("ppt_Zrho", ppt.Zrho, g["ppt_Zrho"], 1e-9);
    chk("ppt_p", ppt.p, g["ppt_p"], 1e-7);

    std::printf("%s\n", fail ? "FAILED" : "PASSED");
    return fail;
}
