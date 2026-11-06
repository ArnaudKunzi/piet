// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// collapse + tsfill parity test vs Stata. collapse aggregates one row per by-group;
// tsfill inserts rows for per-panel time gaps. Goldens from coll_tsf_golden.do.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "data/frame.h"
#include "io/columnar.h"
#include "validate_dir.h"

using namespace cecon;

static double cell(const std::string& s) { return (s == "." || s.empty()) ? std::nan("") : std::stod(s); }
static std::vector<std::vector<double>> load(const std::string& p) {
    std::vector<std::vector<double>> g;
    std::ifstream f(p);
    std::string line;
    std::getline(f, line);
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        std::stringstream ss(line);
        std::string c;
        std::vector<double> row;
        while (std::getline(ss, c, ',')) row.push_back(cell(c));
        g.push_back(row);
    }
    return g;
}

int test_coll_tsf_main() {
    std::printf("test_coll_tsf:\n");
    const cobra_test::ValidateBase base;
    int fail = 0;
    auto eq = [](double a, double b) {
        if (std::isnan(a) && std::isnan(b)) return true;
        if (std::isnan(a) != std::isnan(b)) return false;
        return std::fabs(a - b) <= 1e-9 * (std::fabs(b) + 1.0);
    };

    Dataset ds = Dataset::open(base + "coll_tsf.hbin");

    // ---- collapse ----
    {
        Frame fr = Frame::from_dataset(ds);
        std::vector<Frame::CollapseSpec> sp = {
            {"mean", "y", "my"}, {"sum", "y", "sy"}, {"sd", "y", "sdy"}, {"count", "y", "ny"},
            {"max", "w", "mxw"}, {"min", "w", "mnw"}, {"median", "y", "mdy"}};
        Frame r = fr.collapse(sp, {"id"});
        auto g = load(base + "collapse_golden.csv");  // id,my,sy,sdy,ny,mxw,mnw,mdy
        const char* cols[8] = {"id", "my", "sy", "sdy", "ny", "mxw", "mnw", "mdy"};
        int bad = 0;
        for (int64_t i = 0; i < r.n_rows(); ++i)
            for (int c = 0; c < 8; ++c)
                if (!eq(r.col(cols[c]).f64[i], g[i][c])) { bad++; fail = 1;
                    std::printf("  collapse[%lld].%s got=%g stata=%g\n", (long long)i, cols[c], r.col(cols[c]).f64[i], g[i][c]); }
        std::printf("  collapse: %d groups, %d mismatches\n", (int)r.n_rows(), bad);
    }
    // ---- tsfill ----
    {
        Frame fr = Frame::from_dataset(ds);
        Frame r = fr.tsfill("t", "id", 1.0);
        auto g = load(base + "tsfill_golden.csv");  // id,t,y,w
        const char* cols[4] = {"id", "t", "y", "w"};
        int bad = 0;
        if (r.n_rows() != (int64_t)g.size()) { std::printf("  tsfill ROW COUNT got=%lld stata=%zu\n", (long long)r.n_rows(), g.size()); fail = 1; }
        for (int64_t i = 0; i < r.n_rows() && i < (int64_t)g.size(); ++i)
            for (int c = 0; c < 4; ++c)
                if (!eq(r.col(cols[c]).f64[i], g[i][c])) { bad++; fail = 1;
                    std::printf("  tsfill[%lld].%s got=%g stata=%g\n", (long long)i, cols[c], r.col(cols[c]).f64[i], g[i][c]); }
        std::printf("  tsfill: %d rows, %d mismatches\n", (int)r.n_rows(), bad);
    }
    // ---- tsfill --full and missing times (BUG-897) ----
    // Fixtures and expected values are the StataBE 19.5 oracle
    // tests/eval/golden/aer_w2/w2w_tsset_tsfill.log (w2w_mk / w2w_mk2, (D) and (E)):
    // N, missing y, rows of id 2 (or 3), min/max t, sum _n*(100*id+t) and
    // sum y*(100*id+t) over nonmissing y, each %21x field read as a hex literal.
    {
        struct Want { const char* arm; int64_t n, nmy, nk; double tmn, tmx, sofp, skfp; };
        auto mk = [](const std::vector<double>& id, const std::vector<double>& t) {
            Frame fr;
            fr.set_n_rows_hint(static_cast<int64_t>(id.size()));
            std::vector<double> y(id.size());
            for (size_t i = 0; i < id.size(); ++i) {
                const double n = static_cast<double>(i + 1);
                y[i] = std::fmod(n * n * 1854 + n * 5353, 10007) / 10007;
            }
            fr.set_f64_column("id", id);
            fr.set_f64_column("t", t);
            fr.set_f64_column("y", y);
            return fr;
        };
        auto check = [&](const Want& w, const Frame& r, double kId) {
            const auto& id = r.col("id").f64;
            const auto& t = r.col("t").f64;
            const auto& y = r.col("y").f64;
            int64_t nmy = 0, nk = 0;
            double tmn = std::nan(""), tmx = std::nan(""), sofp = 0, skfp = 0;
            for (int64_t i = 0; i < r.n_rows(); ++i) {
                if (std::isnan(y[i])) ++nmy;
                else if (!std::isnan(y[i] * (100 * id[i] + t[i]))) skfp += y[i] * (100 * id[i] + t[i]);
                if (id[i] == kId) ++nk;
                if (!std::isnan(t[i])) {
                    tmn = std::isnan(tmn) ? t[i] : std::min(tmn, t[i]);
                    tmx = std::isnan(tmx) ? t[i] : std::max(tmx, t[i]);
                }
                // `summarize, meanonly` r(sum) skips a missing product.
                const double o = static_cast<double>(i + 1) * (100 * id[i] + t[i]);
                if (!std::isnan(o)) sofp += o;
            }
            const bool ok = r.n_rows() == w.n && nmy == w.nmy && nk == w.nk && tmn == w.tmn &&
                            tmx == w.tmx && sofp == w.sofp && eq(skfp, w.skfp);
            std::printf("  %s %-22s N=%lld nmy=%lld n%g=%lld t=%g..%g sofp=%.17g skfp=%.17g\n",
                        ok ? "ok  " : "FAIL", w.arm, (long long)r.n_rows(), (long long)nmy, kId,
                        (long long)nk, tmn, tmx, sofp, skfp);
            if (!ok) fail = 1;
        };
        const std::vector<double> idA = {1, 1, 1, 1, 1, 2, 2, 2, 3, 3, 3, 3, 3, 4};
        const std::vector<double> tA = {1, 2, 3, 4, 5, 3, 4, 6, 1, 2, 5, 7, 8, 2};
        const std::vector<double> idB = {1, 1, 1, 1, 2, 2, 2, 3, 3};
        const std::vector<double> tB = {2, 4, 6, 10, 4, 6, 8, 2, 8};
        // log:852 tsfill_panel (control: the non-full path)
        check({"tsfill_panel", 18, 4, 4, 1, 8, 0x1.75e6p+15, 0x1.99b88fdfaa0b5p+10},
              mk(idA, tA).tsfill("t", "id", 1.0), 2);
        // log:917 tsfill_full_panel
        check({"tsfill_full_panel", 32, 18, 8, 1, 8, 0x1.4548p+17, 0x1.99b88fdfaa0b5p+10},
              mk(idA, tA).tsfill("t", "id", 1.0, true), 2);
        // log:1286 tsfill_d2 / :1339 tsfill_full_d2 (delta 2; the id 3 row count)
        check({"tsfill_d2", 12, 3, 4, 2, 10, 0x1.252p+14, 0x1.c589fd64001a3p+9},
              mk(idB, tB).tsfill("t", "id", 2.0), 3);
        check({"tsfill_full_d2", 15, 6, 5, 2, 10, 0x1.d15p+14, 0x1.c589fd64001a3p+9},
              mk(idB, tB).tsfill("t", "id", 2.0, true), 3);
        // log:1165 tsfill_tsonly / :1233 tsfill_full_tsonly (`keep if id == 3`, `tsset t`)
        {
            const std::vector<double> id3 = {3, 3, 3, 3, 3}, t3 = {1, 2, 5, 7, 8};
            Frame f3 = mk(idA, tA);
            Frame g3;
            g3.set_n_rows_hint(5);
            std::vector<double> y3;
            for (int i = 8; i < 13; ++i) y3.push_back(f3.col("y").f64[static_cast<size_t>(i)]);
            g3.set_f64_column("id", id3);
            g3.set_f64_column("t", t3);
            g3.set_f64_column("y", y3);
            // id is not the panel here, so filled rows carry no id (the record's n2 = 0).
            check({"tsfill_tsonly", 8, 3, 0, 1, 8, 0x1.b83p+12, 0x1.65c3fc025a827p+9},
                  g3.tsfill("t", "", 1.0), 2);
            check({"tsfill_full_tsonly", 8, 3, 0, 1, 8, 0x1.b83p+12, 0x1.65c3fc025a827p+9},
                  g3.tsfill("t", "", 1.0, true), 2);
        }
        // log:1729 rf_tsfill_tmiss_ctl: a missing time stays, last in its panel, and
        // the rest of the panel fills (the old comparator ordered NaN arbitrarily).
        std::vector<double> tMiss = tA;
        tMiss[4] = std::nan("");
        check({"rf_tsfill_tmiss_ctl", 18, 4, 4, 1, 8, 0x1.71ccp+15, 0x1.91ada6b7ef6c4p+10},
              mk(idA, tMiss).tsfill("t", "id", 1.0), 2);
        // log:1662 rf_tsfill_full_tmiss: full with a missing time refuses (r(451) in PIET).
        bool threw = false;
        try {
            (void)mk(idA, tMiss).tsfill("t", "id", 1.0, true);
        } catch (const std::exception& e) {
            threw = std::string(e.what()).find("may not contain missing values") != std::string::npos;
        }
        std::printf("  %s %-22s full with a missing time refuses\n", threw ? "ok  " : "FAIL",
                    "rf_tsfill_full_tmiss");
        if (!threw) fail = 1;
    }
    std::printf("%s\n", fail ? "FAILED" : "PASSED");
    return fail;
}
