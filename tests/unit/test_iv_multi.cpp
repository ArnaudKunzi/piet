// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Guards the iv2sls refactor: a multi-outcome batch (iv2sls_fit_multi) must
// produce bit-for-bit the same coefficients, SEs and instrument diagnostics as
// fitting each outcome separately with iv2sls_fit. Exercises the shared FE
// absorption + first-stage reuse across outcomes, under iid / HC1 / cluster vcov,
// with an over-identified design (so Hansen J is also compared).
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

#include "estimators/iv2sls.h"

using namespace cecon;

static int g_fail = 0;
static void eq(const char* what, double got, double want, double tol = 1e-9) {
    double d = std::fabs(got - want);
    if (std::isnan(got) != std::isnan(want) || (!std::isnan(d) && d > tol)) {
        std::printf("  FAIL %-22s got=%.12g want=%.12g (|d|=%.3g)\n", what, got, want, d);
        ++g_fail;
    }
}

static Factor mkfac(const std::string& name, const std::vector<int32_t>& codes) {
    Factor f;
    f.name = name;
    f.codes = codes;
    int mx = 0;
    for (int32_t c : codes) mx = std::max(mx, static_cast<int>(c));
    f.n_levels = mx + 1;
    f.count.assign(f.n_levels, 0.0);
    for (int32_t c : codes) f.count[c] += 1.0;
    return f;
}

int test_iv_multi_main() {
    std::printf("test_iv_multi:\n");
    const int64_t n = 360;
    const int G = 12, NC = 8;
    std::mt19937 rng(20260617);
    std::normal_distribution<double> N(0.0, 1.0);

    std::vector<int32_t> g(n), cl(n);
    std::vector<double> z1(n), z2(n), x(n), d(n);
    std::vector<double> fe_d(G), fe_y(G);
    for (int j = 0; j < G; ++j) { fe_d[j] = N(rng); fe_y[j] = N(rng); }
    for (int64_t i = 0; i < n; ++i) {
        g[i] = static_cast<int32_t>(i % G);
        cl[i] = static_cast<int32_t>(i % NC);
        z1[i] = N(rng); z2[i] = N(rng); x[i] = N(rng);
        d[i] = 0.9 * z1[i] + 0.6 * z2[i] + 0.4 * x[i] + fe_d[g[i]] + 0.5 * N(rng);  // real first stage
    }
    // three outcomes, different structural coefs
    const int O = 3;
    double bd[O] = {1.0, -0.5, 0.25}, bx[O] = {0.3, 0.8, -0.2};
    std::vector<std::vector<double>> ys(O, std::vector<double>(n));
    for (int o = 0; o < O; ++o)
        for (int64_t i = 0; i < n; ++i)
            ys[o][i] = bd[o] * d[i] + bx[o] * x[i] + fe_y[g[i]] + 0.7 * N(rng);

    auto make_md = [&](const std::vector<double>& yinit) {
        IvModelData md;
        md.n = n;
        md.has_intercept = false;  // FE present
        md.kexog = 1; md.kendog = 1; md.kinst = 2;
        md.Xexog = x;          // n x 1
        md.Xendog = d;         // n x 1
        md.Zinst.resize(static_cast<size_t>(n) * 2);
        for (int64_t i = 0; i < n; ++i) { md.Zinst[i] = z1[i]; md.Zinst[n + i] = z2[i]; }
        md.y = yinit;
        md.exog_names = {"x"};
        md.endog_names = {"d"};
        md.fe = {mkfac("g", g)};
        md.cluster = {mkfac("cl", cl)};
        md.keep.resize(static_cast<size_t>(n));
        for (int64_t i = 0; i < n; ++i) md.keep[i] = i;
        return md;
    };

    struct Case { const char* name; Vcov vt; };
    Case cases[] = {{"iid", Vcov::IID}, {"hc1", Vcov::HC1}, {"cluster", Vcov::Cluster}};
    for (const Case& c : cases) {
        // singles
        std::vector<IvResult> singles;
        for (int o = 0; o < O; ++o) {
            IvModelData md = make_md(ys[o]);
            singles.push_back(iv2sls_fit(md, c.vt));
        }
        // multi (fresh md; its y is set per-outcome internally)
        IvModelData md = make_md(ys[0]);
        std::vector<std::vector<double>> yv = ys;
        std::vector<IvResult> multi = iv2sls_fit_multi(md, yv, c.vt);

        if (multi.size() != static_cast<size_t>(O)) { std::printf("  FAIL multi size\n"); ++g_fail; continue; }
        for (int o = 0; o < O; ++o) {
            const IvResult& s = singles[o];
            const IvResult& m = multi[o];
            char tag[64];
            for (int j = 0; j < s.k; ++j) {
                std::snprintf(tag, sizeof(tag), "%s o%d beta[%d]", c.name, o, j);
                eq(tag, m.beta[j], s.beta[j]);
                std::snprintf(tag, sizeof(tag), "%s o%d se[%d]", c.name, o, j);
                eq(tag, m.se[j], s.se[j]);
            }
            std::snprintf(tag, sizeof(tag), "%s o%d first_stage_F", c.name, o);
            eq(tag, m.first_stage_F[0], s.first_stage_F[0]);
            std::snprintf(tag, sizeof(tag), "%s o%d kp_wald_F", c.name, o);
            eq(tag, m.kp_wald_F, s.kp_wald_F);
            std::snprintf(tag, sizeof(tag), "%s o%d eff_F", c.name, o);
            eq(tag, m.eff_F, s.eff_F);
            std::snprintf(tag, sizeof(tag), "%s o%d kp_lm", c.name, o);
            eq(tag, m.kp_lm, s.kp_lm);
            std::snprintf(tag, sizeof(tag), "%s o%d hansen_j", c.name, o);
            eq(tag, m.hansen_j, s.hansen_j);
        }
    }

    if (g_fail) { std::printf("FAILED (%d)\n", g_fail); return 1; }
    std::printf("PASSED (single==multi for iid/hc1/cluster, over-identified + FE)\n");
    return 0;
}
