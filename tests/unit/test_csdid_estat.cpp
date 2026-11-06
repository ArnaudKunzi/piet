// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// csdid estat pretrend + estat calendar + estat group parity test vs Stata.
// Pretrend = joint Wald that all pre-treatment att(g,t) cells == 0; calendar =
// group-weighted ATT per outcome period; group = ATT per treatment cohort over
// its post cells. Golden from csdid_estat_golden.do. Also pins group !=
// calendar (the estat-group-returns-calendar-table regression).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "estimators/csdid.h"
#include "estimators/posthest.h"
#include "io/columnar.h"
#include "validate_dir.h"

using namespace cecon;

int test_csdid_estat_main() {
    std::printf("test_csdid_estat:\n");
    const cobra_test::ValidateBase base;
    std::map<std::string, double> g;
    {
        std::ifstream f(base + "csdid_estat_golden.csv");
        std::string line;
        std::getline(f, line);
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            std::stringstream ss(line);
            std::string k, v;
            std::getline(ss, k, ','); std::getline(ss, v, ',');
            if (v == "." || v.empty()) continue;
            g[k] = std::stod(v);
        }
    }
    Dataset ds = Dataset::open(base + "csdid_input.hbin");
    int64_t n = ds.n_rows();
    const double* h_pid = ds.col("h_pid").f64();
    const double* wv = ds.col("wv").f64();
    const double* gvar = ds.col("gvar").f64();
    const double* y = ds.col("visits_w").f64();
    std::vector<double> X(static_cast<size_t>(n) * 3);
    const double* fem = ds.col("female").f64();
    const double* age = ds.col("age_z").f64();
    const double* edu = ds.col("edu_hi").f64();
    for (int64_t i = 0; i < n; ++i) { X[i] = fem[i]; X[n + i] = age[i]; X[2 * n + i] = edu[i]; }
    CsdidConfig cfg;
    CsdidRunResult R = csdid_dripw_run(h_pid, wv, gvar, y, X.data(), 3, n, cfg, 0, 0);

    int fail = 0;
    auto chk = [&](const char* tag, double got, double want, double tol) {
        double rel = std::fabs(got - want) / (std::fabs(want) + 1e-30);
        std::printf("  %-16s got=%.10g stata=%.10g rel=%.2e\n", tag, got, want, rel);
        if (!(rel <= tol)) { std::printf("    <<FAIL\n"); fail = 1; }
    };
    // pretrend
    chk("pretrend_chi2", R.pretrend_chi2, g["pretrend_chi2"], 1e-6);
    chk("pretrend_df", R.pretrend_df, g["pretrend_df"], 1e-12);
    // calendar per period: golden key cal_T<wv>_att / _se
    int matched = 0;
    for (const auto& cb : R.calendar) {
        int per = static_cast<int>(std::lround(cb.t));
        std::string ka = "cal_T" + std::to_string(per) + "_att";
        std::string ks = "cal_T" + std::to_string(per) + "_se";
        if (g.count(ka)) { chk(("calT" + std::to_string(per) + "_att").c_str(), cb.att, g[ka], 1e-6); ++matched; }
        if (g.count(ks)) chk(("calT" + std::to_string(per) + "_se").c_str(), cb.se, g[ks], 1e-4);
    }
    std::printf("  matched %d calendar periods; pretrend p (cecon) = %.3e\n",
                matched, R.pretrend_p);
    if (matched < 5) { std::printf("    <<FAIL too few calendar periods matched\n"); fail = 1; }
    // group per cohort: golden key grp_G<g>_att / _se
    int gmatched = 0;
    for (const auto& gb : R.groups) {
        int coh = static_cast<int>(std::lround(gb.g));
        std::string ka = "grp_G" + std::to_string(coh) + "_att";
        std::string ks = "grp_G" + std::to_string(coh) + "_se";
        if (g.count(ka)) { chk(("grpG" + std::to_string(coh) + "_att").c_str(), gb.att, g[ka], 1e-6); ++gmatched; }
        if (g.count(ks)) chk(("grpG" + std::to_string(coh) + "_se").c_str(), gb.se, g[ks], 1e-4);
    }
    std::printf("  matched %d group cohorts\n", gmatched);
    if (gmatched < 2) { std::printf("    <<FAIL too few group cohorts matched\n"); fail = 1; }
    // group and calendar are distinct aggregations — identical tables = the
    // silent-wrong-value bug this test exists to keep dead.
    bool distinct = R.groups.size() != R.calendar.size();
    if (!distinct) {
        for (size_t j = 0; j < R.groups.size(); ++j)
            if (std::fabs(R.groups[j].att - R.calendar[j].att) > 1e-12) { distinct = true; break; }
    }
    if (!distinct) { std::printf("    <<FAIL group aggregation identical to calendar\n"); fail = 1; }

    // ---- The agg() postings and the estat event V (csdid-drdid-attgt-ematrix;
    // LAYOUT.md 1.6, 1.8). GAverage/CAverage are the golden's estat group /
    // estat calendar first columns: both weight the bins by FIXED weights
    // (the cohort's mean wgt; equal), so agg(group)/agg(calendar) post the same
    // number. The other columns repeat the bins above; every SE is the posted
    // V's diagonal.
    {
        chk("GAverage_att", R.agg_group.b.empty() ? 0.0 : R.agg_group.b[0],
            g["grp_GAverage_att"], 1e-12);
        chk("GAverage_se", R.agg_group.V.empty() ? 0.0 : std::sqrt(R.agg_group.V[0]),
            g["grp_GAverage_se"], 1e-8);
        chk("CAverage_att", R.agg_calendar.b.empty() ? 0.0 : R.agg_calendar.b[0],
            g["cal_CAverage_att"], 1e-12);
        chk("CAverage_se", R.agg_calendar.V.empty() ? 0.0 : std::sqrt(R.agg_calendar.V[0]),
            g["cal_CAverage_se"], 1e-8);
        int bad = 0;
        const auto diag_is = [&](const CsdidPosting& P, size_t j, double se) {
            const size_t m = P.b.size();
            return std::fabs(std::sqrt(P.V[j * m + j]) - se) <= 1e-13 * se;
        };
        if (R.agg_group.names.size() != 1 + R.groups.size() || R.agg_group.names[0] != "GAverage") ++bad;
        for (size_t j = 0; bad == 0 && j < R.groups.size(); ++j)
            if (R.agg_group.names[1 + j] != "G" + std::to_string(std::lround(R.groups[j].g)) ||
                R.agg_group.b[1 + j] != R.groups[j].att || !diag_is(R.agg_group, 1 + j, R.groups[j].se))
                ++bad;
        if (R.agg_calendar.names.size() != 1 + R.calendar.size() || R.agg_calendar.names[0] != "CAverage") ++bad;
        for (size_t j = 0; bad == 0 && j < R.calendar.size(); ++j)
            if (R.agg_calendar.names[1 + j] != "T" + std::to_string(std::lround(R.calendar[j].t)) ||
                R.agg_calendar.b[1 + j] != R.calendar[j].att ||
                !diag_is(R.agg_calendar, 1 + j, R.calendar[j].se))
                ++bad;
        // agg(simple): `ATT`, V = simple_se^2.
        if (R.agg_simple.names != std::vector<std::string>{"ATT"} || R.agg_simple.b.size() != 1 ||
            R.agg_simple.b[0] != R.simple_att || !diag_is(R.agg_simple, 0, R.simple_se))
            ++bad;
        // agg(event): Pre_avg Post_avg Tm.. Tp.. with the FULL covariance; estat
        // event's e(V) is the same matrix with the averages-by-bins blocks zero.
        const size_t m = R.agg_event.b.size();
        if (R.agg_event.names != R.event_names || R.agg_event.b != R.event_b ||
            m != 2 + R.events.size() || R.event_V.size() != m * m || R.agg_event.V.size() != m * m)
            ++bad;
        double cross = 0;
        for (size_t r = 0; bad == 0 && r < m; ++r)
            for (size_t c = 0; c < m; ++c) {
                const bool same_block = (r < 2) == (c < 2);
                if (same_block ? R.event_V[r * m + c] != R.agg_event.V[r * m + c]
                               : R.event_V[r * m + c] != 0.0)
                    ++bad;
                if (!same_block) cross = std::max(cross, std::fabs(R.agg_event.V[r * m + c]));
            }
        for (size_t j = 0; bad == 0 && j < R.events.size(); ++j)
            if (!diag_is(R.agg_event, 2 + j, R.events[j].se)) ++bad;
        if (bad == 0 && (!diag_is(R.agg_event, 0, R.pre_avg_se) || !diag_is(R.agg_event, 1, R.post_avg_se)))
            ++bad;
        // agg(event)'s averages-by-bins covariance is genuinely nonzero (the
        // averages are means of the bins), unlike estat event's.
        std::printf("  agg postings: structure mismatches %d, agg(event) max |cross| %.3g\n", bad, cross);
        if (bad || !(cross > 0)) { std::printf("    <<FAIL\n"); fail = 1; }
    }
    std::printf("%s\n", fail ? "FAILED" : "PASSED");
    return fail;
}
