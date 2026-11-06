// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Unit test for Frame::tssmooth_ma — exercises the REAL code path by writing a
// tiny .hbin, reopening it, and running xtset + tssmooth_ma. Convention: missing
// inputs get weight zero; panel edges use a shortened (not missing) window.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <vector>

#include "data/frame.h"
#include "io/columnar.h"
#include "io/win_compat.h"
#include "nlohmann/json.hpp"

using namespace cecon;
using nlohmann::json;

static int g_fail = 0;
static void approx(const char* what, double got, double want) {
    bool bad = std::isnan(want) ? !std::isnan(got)
                                : std::fabs(got - want) / (std::fabs(want) + 1e-30) > 1e-12;
    if (bad) { std::printf("  FAIL %s: got %.15g want %.15g\n", what, got, want); ++g_fail; }
}

// Minimal .hbin writer (mirrors Frame::write_hbin) for a set of named f64 columns.
static void write_hbin_f64(const std::string& path, int64_t n,
                           const std::vector<std::pair<std::string, std::vector<double>>>& cols) {
    constexpr int64_t A = 64;
    auto up = [](int64_t x) { return (x + A - 1) / A * A; };
    json h;
    h["magic"] = "CECONHB1"; h["version"] = 1; h["n_rows"] = n;
    json jc = json::array();
    int64_t rel = 0;
    for (auto& c : cols) {
        int64_t nb = static_cast<int64_t>(c.second.size()) * 8;
        jc.push_back({{"name", c.first}, {"dtype", "f64"}, {"offset", rel}, {"nbytes", nb}});
        rel = up(rel + nb);
    }
    h["columns"] = jc;
    std::string hj = h.dump();
    std::ofstream f(to_native_path(path), std::ios::binary);
    f.write("CECONHB1", 8);
    uint64_t hl = hj.size();
    f.write(reinterpret_cast<const char*>(&hl), 8);
    f.write(hj.data(), hj.size());
    int64_t ds = up(16 + (int64_t)hl), pos = 16 + (int64_t)hl;
    std::vector<char> pad(A, 0);
    if (pos < ds) { f.write(pad.data(), ds - pos); pos = ds; }
    for (size_t i = 0; i < cols.size(); ++i) {
        int64_t want = ds + jc[i]["offset"].get<int64_t>();
        if (pos < want) { f.write(pad.data(), want - pos); pos = want; }
        f.write(reinterpret_cast<const char*>(cols[i].second.data()),
                (int64_t)cols[i].second.size() * 8);
        pos += (int64_t)cols[i].second.size() * 8;
    }
}

int test_tsma_main() {
    std::printf("test_tsma:\n");
    const double NA = std::nan("");
    // Panel 1: consecutive times (no gaps). Panel 2: out-of-order (tests sorting).
    // Panel 3: a TIME GAP (t=1,2,10) — distinguishes time-aware from observation-based.
    std::vector<double> id = {1, 1, 1, 1, 1, 2, 2, 3, 3, 3};
    std::vector<double> tm = {1, 2, 3, 4, 5, 2, 1, 1, 2, 10};
    std::vector<double> x  = {10, 20, NA, 40, 50, 8, 7, 5, 7, 9};
    const std::string path = native_temp_path("cecon_tsma_test.hbin");
    write_hbin_f64(path, 10, {{"id", id}, {"t", tm}, {"x", x}});

    Dataset ds = Dataset::open(path);
    Frame fr = Frame::from_dataset(ds);
    fr.xtset("id", "t");
    fr.tssmooth_ma("ma", "x", /*lags=*/1, /*leads=*/1);
    const auto& ma = fr.col("ma").f64;

    // window(1,1,1) in TIME units, NaN weight 0. id=2 sorted by t → [7@t1, 8@t2].
    // id1: [10,20]/2=15; [10,20]/2=15 (NA skip); [20,40]/2=30; [40,50]/2=45; [40,50]/2=45
    // id2 (orig order t2 then t1): both = mean(7,8)=7.5
    // id3 (gap): t1 win[0,2]days→{5,7}=6; t2 win[1,3]days→{5,7}=6; t10 win[9,11]→{9}=9
    //   (observation-based would give 6,7,8 — the gap row at t10 is excluded by time)
    std::vector<double> want = {15, 15, 30, 45, 45, 7.5, 7.5, 6, 6, 9};
    for (size_t i = 0; i < want.size(); ++i) {
        char b[16]; std::snprintf(b, sizeof b, "ma[%zu]", i);
        approx(b, ma[i], want[i]);
    }

    // ---- BUG-370: a PURE time series (Stata `tsset t`, no panel) -------------
    // This used to be unreachable: the guard demanded xtset outright, so the
    // plainest use of a moving average — one series over time — threw. The panel
    // case above must be bit-identical afterwards, which is why it runs first and
    // is left untouched; a null panel pointer is the ONLY difference.
    {
        std::vector<double> t2 = {1, 2, 3, 4, 5};
        std::vector<double> y2 = {1, 2, 3, 4, 5};
        const std::string p2 = native_temp_path("cecon_tsma_tsset.hbin");
        write_hbin_f64(p2, 5, {{"t", t2}, {"y", y2}});
        Dataset d2 = Dataset::open(p2);
        Frame f2 = Frame::from_dataset(d2);
        f2.tsset("t");
        f2.tssmooth_ma("m", "y", /*lags=*/1, /*leads=*/1);
        const auto& m = f2.col("m").f64;
        // Edges shorten rather than go missing: {1,2}/2, {1,2,3}/3, … {4,5}/2.
        std::vector<double> want2 = {1.5, 2, 3, 4, 4.5};
        for (size_t i = 0; i < want2.size(); ++i) {
            char b[24]; std::snprintf(b, sizeof b, "tsset ma[%zu]", i);
            approx(b, m[i], want2[i]);
        }
        native_unlink(p2);
    }
    {
        // Same, with a TIME GAP. t=4's window [3,5] contains no t=3 and no t=5, so
        // it averages only itself — the discriminator between a time-aware window
        // and an observation-based one, which would have given (2+4)/2=3 here.
        std::vector<double> t3 = {1, 2, 4};
        std::vector<double> y3 = {1, 2, 4};
        const std::string p3 = native_temp_path("cecon_tsma_tsgap.hbin");
        write_hbin_f64(p3, 3, {{"t", t3}, {"y", y3}});
        Dataset d3 = Dataset::open(p3);
        Frame f3 = Frame::from_dataset(d3);
        f3.tsset("t");
        f3.tssmooth_ma("m", "y", /*lags=*/1, /*leads=*/1);
        const auto& m = f3.col("m").f64;
        std::vector<double> want3 = {1.5, 1.5, 4};
        for (size_t i = 0; i < want3.size(); ++i) {
            char b[24]; std::snprintf(b, sizeof b, "tsgap ma[%zu]", i);
            approx(b, m[i], want3[i]);
        }
        native_unlink(p3);
    }
    {
        // No time variable at all is still an honest refusal — the relaxation is
        // "a panel is optional", not "anything goes".
        std::vector<double> y4 = {1, 2, 3};
        const std::string p4 = native_temp_path("cecon_tsma_nots.hbin");
        write_hbin_f64(p4, 3, {{"y", y4}});
        Dataset d4 = Dataset::open(p4);
        Frame f4 = Frame::from_dataset(d4);
        bool threw = false;
        try { f4.tssmooth_ma("m", "y", 1, 1); } catch (const std::exception&) { threw = true; }
        if (!threw) { std::printf("  FAIL no-tsset tssmooth_ma did not throw\n"); ++g_fail; }
        native_unlink(p4);
    }

    if (g_fail) { std::printf("FAILED (%d)\n", g_fail); return 1; }
    std::printf("  ok — real Frame path, sort + NaN-weight-0 + edge shortening;\n"
                "       tsset-only (no panel) + time gap + no-tsset refusal\nPASSED\n");
    return 0;
}
