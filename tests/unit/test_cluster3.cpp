// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// w2b_estim three-way clustering — the engine half of
// `reghdfe y x1 x2, absorb(id) cluster(id t c3)` and its two-way control.
//
// Oracle: tests/eval/golden/aer_w2/w2b_estim.log (StataBE 19.5, 2026-09-19):
//   :1410 reghdfe_cluster3      _se[x1] .0575950937310231  e(N_clustervars) 3  e(df_r) 3
//         (clusters id 10, t 6, c3 4 — the header at :1373-1375)
//   :1447 reghdfe_cluster2_ctl  _se[x1] .060463723825293   e(df_r) 5
//
// Fixture = the do-file's: set obs 60, every `generate` default-typed (FLOAT), so
// x1/x2/y are rebuilt the way Stata stores them (double expression, rounded to
// float; y reads the float x1/x2). id/t/c3 are small integers, exact either way.
//
// The small-sample variant that reproduces both arms is ONE factor from the
// smallest single-dimension G — Gmin/(Gmin-1)·(N-1)/(N-K), K = 3 with the absorbed
// id exempt because it is nested in cluster id — over the inclusion-exclusion meat
// (see compute_vcov). The per-subset G_S/(G_S-1) variant misses by 5.2% / 1.7%.
//
// Tier (tests/eval/TOLERANCE.md): se at rtol 1e-9 (T2); integers exact.
//
// Fails on the old engine: compute_vcov read only cluster[0..1], so the three-way
// fit answered the two-way se .0604637 (5.0% off) with no error, and
// multiway_meat did not exist (this TU did not compile).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <exception>
#include <map>
#include <string>
#include <vector>

#include "core/model.h"
#include "estimators/feols.h"
#include "vcov/vcov.h"

using namespace cecon;

static int g_fail = 0;

static void close_to(double got, double want, double rtol, const std::string& what) {
    const double rel = std::fabs(got - want) / std::fabs(want);
    const bool ok = rel <= rtol;
    std::printf("  %-44s got %.16g want %.16g rel %.2e %s\n", what.c_str(), got, want, rel,
                ok ? "ok" : "<<FAIL");
    if (!ok) g_fail = 1;
}

static void equal_int(long long got, long long want, const std::string& what) {
    const bool ok = got == want;
    std::printf("  %-44s got %lld want %lld %s\n", what.c_str(), got, want, ok ? "ok" : "<<FAIL");
    if (!ok) g_fail = 1;
}

static void check(bool ok, const std::string& what) {
    std::printf("  %-44s %s\n", what.c_str(), ok ? "ok" : "<<FAIL");
    if (!ok) g_fail = 1;
}

// Dense 0..L-1 codes in order of first appearance.
static Factor make_factor(const std::string& name, const std::vector<double>& v) {
    Factor f;
    f.name = name;
    std::map<double, int32_t> seen;
    f.codes.resize(v.size());
    for (size_t i = 0; i < v.size(); ++i) {
        auto it = seen.find(v[i]);
        if (it == seen.end()) it = seen.emplace(v[i], static_cast<int32_t>(seen.size())).first;
        f.codes[i] = it->second;
    }
    f.n_levels = static_cast<int>(seen.size());
    f.count.assign(f.n_levels, 0.0);
    for (int32_t c : f.codes) f.count[c] += 1.0;
    return f;
}

struct Fixture {
    std::vector<double> x1, x2, y, id, t, c3;
};

static Fixture build_fixture() {
    Fixture fx;
    for (int i = 1; i <= 60; ++i) {
        const double obs = i;
        const double id = std::ceil(obs / 6.0);
        const double x1 = static_cast<float>(std::fmod(obs * 7, 11) + std::fmod(obs, 5) / 10);
        const double x2 = static_cast<float>(std::fmod(obs * 3, 5) + std::fmod(obs * 13, 7) / 10);
        fx.x1.push_back(x1);
        fx.x2.push_back(x2);
        fx.y.push_back(static_cast<float>(1 + 2 * x1 - x2 + id / 3 + std::fmod(obs * 11, 17) / 5));
        fx.id.push_back(id);
        fx.t.push_back(std::fmod(obs - 1, 6) + 1);
        fx.c3.push_back(std::fmod(obs, 4));
    }
    return fx;
}

// absorb(id), the given cluster dimensions — the ModelData build_model would make.
static ModelData make_md(const Fixture& fx, const std::vector<Factor>& clusters) {
    ModelData md;
    const int64_t n = static_cast<int64_t>(fx.y.size());
    md.dm.n = n;
    md.dm.k = 2;
    md.dm.xnames = {"x1", "x2"};
    md.dm.X = fx.x1;
    md.dm.X.insert(md.dm.X.end(), fx.x2.begin(), fx.x2.end());
    md.dm.y = fx.y;
    md.fe = {make_factor("id", fx.id)};
    md.cluster = clusters;
    md.has_intercept = false;
    for (int64_t i = 0; i < n; ++i) md.keep.push_back(i);
    return md;
}

static double se_of(const FeolsResult& r, const std::string& name) {
    for (int j = 0; j < r.k; ++j)
        if (r.names[j] == name) return r.se[j];
    return std::nan("");
}

int main() {
    std::printf("test_cluster3:\n");
    const Fixture fx = build_fixture();
    const Factor fid = make_factor("id", fx.id), ft = make_factor("t", fx.t),
                 fc3 = make_factor("c3", fx.c3);

    // reghdfe_cluster3 (w2b_estim.log:1410).
    {
        ModelData md = make_md(fx, {fid, ft, fc3});
        FeolsResult r = feols_fit(md, Vcov::Multiway);
        close_to(se_of(r, "x1"), 0.0575950937310231, 1e-9, "cluster3 se[x1]");
        equal_int(r.df_r, 3, "cluster3 e(df_r) = Gmin-1");
        equal_int(r.n_clusters, 4, "cluster3 n_clusters = Gmin (c3)");
        equal_int(r.cluster_dof_k, 3, "cluster3 K (id nested in cluster id)");
        check(r.vcov_type == "multiway(id,t,c3)", "cluster3 vcov_type " + r.vcov_type);
        // The per-dimension G the reply's n_clusters_by carries (e(N_clust1..3)).
        equal_int(md.cluster.size(), 3, "cluster3 dimensions kept");
        equal_int(md.cluster[0].n_levels, 10, "cluster3 N_clust1 (id)");
        equal_int(md.cluster[1].n_levels, 6, "cluster3 N_clust2 (t)");
        equal_int(md.cluster[2].n_levels, 4, "cluster3 N_clust3 (c3)");
    }

    // reghdfe_cluster2_ctl (w2b_estim.log:1447): the two-way answer must not move.
    {
        ModelData md = make_md(fx, {fid, ft});
        FeolsResult r = feols_fit(md, Vcov::Multiway);
        close_to(se_of(r, "x1"), 0.060463723825293, 1e-9, "cluster2_ctl se[x1]");
        equal_int(r.df_r, 5, "cluster2_ctl e(df_r)");
        check(r.vcov_type == "twoway(id,t)", "cluster2_ctl vcov_type " + r.vcov_type);
    }

    // multiway_meat at d = 2 is bit-identical to the m1 + m2 - m12 form it replaced,
    // and at d = 3 equals the seven-term inclusion-exclusion written out by hand.
    {
        const int64_t n = 60;
        const int k = 2;
        std::vector<double> design = fx.x1, e(n);
        design.insert(design.end(), fx.x2.begin(), fx.x2.end());
        for (int64_t i = 0; i < n; ++i) e[i] = std::fmod(static_cast<double>(i) * 0.37, 1.3) - 0.6;
        auto meat_of = [&](const Factor& f) {
            std::vector<double> m(k * k);
            int G = 0;
            cluster_meat(design.data(), n, k, e.data(), f, m.data(), &G);
            return m;
        };
        const Factor ab = intersect_factors(fid, ft), ac = intersect_factors(fid, fc3),
                     bc = intersect_factors(ft, fc3), abc = intersect_factors(ab, fc3);
        const auto ma = meat_of(fid), mb = meat_of(ft), mc = meat_of(fc3), mab = meat_of(ab),
                   mac = meat_of(ac), mbc = meat_of(bc), mabc = meat_of(abc);

        std::vector<double> got2(k * k), got3(k * k);
        multiway_meat(design.data(), n, k, e.data(), {&fid, &ft}, got2.data());
        multiway_meat(design.data(), n, k, e.data(), {&fid, &ft, &fc3}, got3.data());
        bool same2 = true;
        double worst3 = 0;
        for (int i = 0; i < k * k; ++i) {
            if (got2[i] != ma[i] + mb[i] - mab[i]) same2 = false;
            const double want = ma[i] + mb[i] + mc[i] - mab[i] - mac[i] - mbc[i] + mabc[i];
            worst3 = std::max(worst3, std::fabs(got3[i] - want) / std::max(1.0, std::fabs(want)));
        }
        check(same2, "multiway_meat d=2 bit-identical to m1+m2-m12");
        check(worst3 <= 1e-12, "multiway_meat d=3 = 7-term inclusion-exclusion");
    }

    // Past the dimension bound the engine refuses by name instead of truncating.
    {
        ModelData md = make_md(fx, std::vector<Factor>(kMaxMultiwayClusters + 1, ft));
        bool threw = false;
        try {
            (void)feols_fit(md, Vcov::Multiway);
        } catch (const std::exception&) {
            threw = true;
        }
        check(threw, "more than kMaxMultiwayClusters dimensions refused");
    }

    std::printf(g_fail ? "test_cluster3: FAIL\n" : "test_cluster3: PASS\n");
    return g_fail;
}
