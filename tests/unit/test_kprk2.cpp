// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Multi-endogenous (p=2) weak-IV F parity test vs Stata ivreg2 e(widstat).
// Homoskedastic = Cragg-Donald minimum-eigenvalue F; robust/cluster = the full
// Kleibergen-Paap rk Wald F (SVD-rank construction, ranktest m_svd). Golden from
// kprk2_golden.do. All three match ivreg2 to ~1e-14 on the 73k-row fixture.
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <unordered_map>

#include "estimators/iv2sls.h"
#include "estimators/iv_weakid.h"
#include "core/model.h"
#include "io/columnar.h"
#include "validate_dir.h"

using namespace cecon;

int test_kprk2_main() {
    std::printf("test_kprk2:\n");
    const cobra_test::ValidateBase base;
    std::map<std::string, double> g;
    {
        std::ifstream f(base + "kprk2_golden.csv");
        std::string line;
        std::getline(f, line);
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            std::stringstream ss(line);
            std::string k, v;
            std::getline(ss, k, ','); std::getline(ss, v, ',');
            g[k] = std::stod(v);
        }
    }
    Dataset ds = Dataset::open(base + "kprk2.hbin");
    int64_t n = ds.n_rows();
    auto col = [&](const char* nm) { const double* p = ds.col(nm).f64(); return std::vector<double>(p, p + n); };
    auto age = col("age_z"), edu = col("edu_hi"), fem = col("female"), gv = col("gvar"), tr = col("treated"), wv = col("wv");
    std::vector<double> Y(2 * n); for (int64_t i = 0; i < n; ++i) { Y[i] = age[i]; Y[n + i] = edu[i]; }
    std::vector<double> X1(2 * n); for (int64_t i = 0; i < n; ++i) { X1[i] = wv[i]; X1[n + i] = 1.0; }
    std::vector<double> Z2(3 * n); for (int64_t i = 0; i < n; ++i) { Z2[i] = fem[i]; Z2[n + i] = gv[i]; Z2[2 * n + i] = tr[i]; }

    double cd = cragg_donald_F(Y, 2, X1, 2, Z2, 3, n);
    double rel = std::fabs(cd - g["cd_widstat"]) / (std::fabs(g["cd_widstat"]) + 1e-30);
    std::printf("  cragg_donald_F got=%.10g stata=%.10g rel=%.2e\n", cd, g["cd_widstat"], rel);
    int fail = (rel <= 1e-8) ? 0 : 1;

    // --- robust + cluster Kleibergen-Paap rk Wald F (>=2 endog) via iv2sls_fit ---
    auto build_md = [&]() {
        IvModelData md;
        md.n = n; md.has_intercept = true;
        md.kexog = 2; md.kendog = 2; md.kinst = 3;
        md.Xexog.resize(2 * n);
        for (int64_t i = 0; i < n; ++i) { md.Xexog[i] = 1.0; md.Xexog[n + i] = wv[i]; }
        md.Xendog = Y;   // age_z, edu_hi (col-major n x 2)
        md.Zinst = Z2;   // female, gvar, treated
        md.y = col("visits_w");
        md.exog_names = {"(intercept)", "wv"};
        md.endog_names = {"age_z", "edu_hi"};
        return md;
    };
    auto check = [&](const char* what, double got, double want) {
        double r = std::fabs(got - want) / (std::fabs(want) + 1e-30);
        std::printf("  %-18s got=%.10g stata=%.10g rel=%.2e\n", what, got, want, r);
        if (r > 1e-6) fail = 1;
    };
    { IvModelData md = build_md(); IvResult r = iv2sls_fit(md, Vcov::HC1);
      check("kp_widstat(rob)", r.kp_wald_F, g["kp_widstat"]); }
    {
        IvModelData md = build_md();
        // cluster on gvar
        std::unordered_map<double, int32_t> rm; Factor f; f.name = "gvar"; f.codes.resize(n);
        for (int64_t i = 0; i < n; ++i) { auto it = rm.find(gv[i]); int32_t c;
            if (it == rm.end()) { c = (int32_t)rm.size(); rm.emplace(gv[i], c); } else c = it->second;
            f.codes[i] = c; }
        f.n_levels = (int)rm.size(); f.count.assign(f.n_levels, 0.0);
        for (int32_t c : f.codes) f.count[c] += 1.0;
        md.cluster = {f};
        IvResult r = iv2sls_fit(md, Vcov::Cluster, 0);
        check("kp_widstat(clu)", r.kp_wald_F, g["kpc_widstat"]);
    }
    std::printf("%s\n", fail ? "FAILED" : "PASSED");
    return fail;
}
