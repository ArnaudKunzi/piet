// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Time-series postestimation diagnostics parity test vs Stata estat
// dwatson/durbinalt/bgodfrey/archlm on a synthetic AR(1)-error series.
// Golden from ts_diag_golden.do. All statistics match Stata to ~1e-11.
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "core/model.h"
#include "estimators/ts_diag.h"
#include "io/columnar.h"
#include "validate_dir.h"

using namespace cecon;

int test_ts_diag_main() {
    std::printf("test_ts_diag:\n");
    const cobra_test::ValidateBase base;

    std::map<std::string, double> g;
    {
        std::ifstream f(base + "ts_diag_golden.csv");
        std::string line;
        std::getline(f, line);  // header
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            std::stringstream ss(line);
            std::string k, v;
            std::getline(ss, k, ',');
            std::getline(ss, v, ',');
            g[k] = std::stod(v);
        }
    }

    Dataset ds = Dataset::open(base + "ts_diag.hbin");
    ModelData md = build_model(ds, "y", {"x1", "x2"}, {}, {});
    const double* X = md.dm.X.data();
    const double* y = md.dm.y.data();
    const int64_t n = md.dm.n;
    const int k = md.dm.k;

    std::vector<double> e;
    ts_ols_residuals(X, n, k, y, e);

    int fail = 0;
    auto chk = [&](const char* tag, double got, double want, double tol) {
        double rel = std::fabs(got - want) / (std::fabs(want) + 1e-30);
        std::printf("  %-12s got=%.12g  stata=%.12g  rel=%.2e\n", tag, got, want, rel);
        if (!(rel <= tol)) { std::printf("    <<FAIL %s rel %.2e > %.0e\n", tag, rel, tol); fail = 1; }
    };
    auto chkb = [&](const char* tag, bool cond) {
        std::printf("  %-24s %s\n", tag, cond ? "ok" : "<<FAIL");
        if (!cond) fail = 1;
    };

    // Durbin-Watson
    DwResult dw = durbin_watson(X, n, k, y);
    chk("dw", dw.dw, g["dw"], 1e-11);
    chk("dw_k", dw.k, g["dw_k"], 1e-12);

    // Durbin's alternative (chi2, lags 1 & 2)
    SerialResult da1 = durbinalt_one(e, X, n, k, 1, false, false, false);
    SerialResult da2 = durbinalt_one(e, X, n, k, 2, false, false, false);
    chk("da_l1_chi2", da1.chi2, g["da_l1_chi2"], 1e-10);
    chk("da_l1_p", da1.p, g["da_l1_p"], 1e-8);
    chk("da_l2_chi2", da2.chi2, g["da_l2_chi2"], 1e-10);
    // small (F) form, lag 1
    SerialResult das = durbinalt_one(e, X, n, k, 1, true, false, false);
    chk("da_s_F", das.F, g["da_s_F"], 1e-10);
    chk("da_s_dfr", das.df_r, g["da_s_dfr"], 1e-12);
    // robust (HC1; forces F form), lag 1
    SerialResult dar = durbinalt_one(e, X, n, k, 1, false, true, false);
    chk("da_r_F", dar.F, g["da_r_F"], 1e-9);
    chk("da_r_p", dar.p, g["da_r_p"], 1e-7);

    // Breusch-Godfrey (chi2, lags 1 & 2)
    SerialResult bg1 = bgodfrey_one(e, X, n, k, 1, false, false);
    SerialResult bg2 = bgodfrey_one(e, X, n, k, 2, false, false);
    chk("bg_l1_chi2", bg1.chi2, g["bg_l1_chi2"], 1e-10);
    chk("bg_l2_chi2", bg2.chi2, g["bg_l2_chi2"], 1e-10);
    SerialResult bgs = bgodfrey_one(e, X, n, k, 1, true, false);
    chk("bg_s_F", bgs.F, g["bg_s_F"], 1e-10);
    chk("bg_s_dfr", bgs.df_r, g["bg_s_dfr"], 1e-12);

    // ARCH LM (lags 1 2 3)
    SerialResult ar1 = archlm_one(e, n, 1);
    SerialResult ar2 = archlm_one(e, n, 2);
    SerialResult ar3 = archlm_one(e, n, 3);
    chk("ar_l1_chi2", ar1.chi2, g["ar_l1_chi2"], 1e-10);
    chk("ar_l2_chi2", ar2.chi2, g["ar_l2_chi2"], 1e-10);
    chk("ar_l3_chi2", ar3.chi2, g["ar_l3_chi2"], 1e-10);
    chk("ar_l1_p", ar1.p, g["ar_l1_p"], 1e-8);

    // Ljung-Box portmanteau (wntestq) on y (autocorrelated) and x1 (white noise)
    auto colptr = [&](const char* nm) {
        for (int j = 0; j < k; ++j) if (md.dm.xnames[j] == nm) return X + static_cast<int64_t>(j) * n;
        return static_cast<const double*>(nullptr);
    };
    const double* yv = y;
    const double* x1v = colptr("x1");
    WnResult wqy5 = wntestq(yv, n, 5);
    WnResult wqy10 = wntestq(yv, n, 10);
    WnResult wqx5 = wntestq(x1v, n, 5);
    chk("wq_y5_stat", wqy5.stat, g["wq_y5_stat"], 1e-10);
    chk("wq_y5_p", wqy5.p, g["wq_y5_p"], 1e-7);
    chk("wq_y10_stat", wqy10.stat, g["wq_y10_stat"], 1e-10);
    chk("wq_x5_stat", wqx5.stat, g["wq_x5_stat"], 1e-10);
    chk("wq_x5_p", wqx5.p, g["wq_x5_p"], 1e-9);

    // BUG-131: wntestq reject-cause discriminator. A zero-variance (constant) series
    // has c0=0 → ρ_k undefined → reject as ZERO_VAR, NOT the misleading "too few
    // observations" (TOO_FEW). A genuinely short series (n<=lags) still rejects TOO_FEW.
    // Both keep ok=false — the reject is correct; only the surfaced cause differs.
    std::vector<double> constser(50, 7.0);  // 50 obs, all equal → c0 = 0
    WnResult wqconst = wntestq(constser.data(), static_cast<int64_t>(constser.size()), 5);
    chkb("wq_const_reject", !wqconst.ok);
    chkb("wq_const_cause_zerovar", wqconst.fail == WnResult::ZERO_VAR);
    std::vector<double> shortser = {1.0, 2.0, 3.0};  // n=3, lags=10 → genuine too-few
    WnResult wqshort = wntestq(shortser.data(), static_cast<int64_t>(shortser.size()), 10);
    chkb("wq_short_reject", !wqshort.ok);
    chkb("wq_short_cause_toofew", wqshort.fail == WnResult::TOO_FEW);

    std::printf("%s\n", fail ? "FAILED" : "PASSED");
    return fail;
}
