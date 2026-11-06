// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Contract: same input ⇒ same output, to the LAST BIT, regardless of worker
// count. The HDFE demean reduces group sums over a fixed lane decomposition
// (function of n only), so processing those lanes with 1 worker (serial) or N
// workers (the pool) must give byte-identical results. n is chosen > LANE_TARGET
// so there is more than one lane (the case where thread-count used to matter).
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include "core/model.h"
#include "hdfe/demean.h"

using namespace cecon;

static Factor mkfac(const std::vector<int32_t>& codes) {
    Factor f;
    f.codes = codes;
    int mx = 0;
    for (int32_t c : codes) mx = std::max(mx, (int)c);
    f.n_levels = mx + 1;
    f.count.assign(f.n_levels, 0.0);
    for (int32_t c : codes) f.count[c] += 1.0;
    return f;
}

static int bit_equal(const std::vector<double>& a, const std::vector<double>& b) {
    if (a.size() != b.size()) return 0;
    return std::memcmp(a.data(), b.data(), a.size() * sizeof(double)) == 0;
}

int test_determinism_main() {
    std::printf("test_determinism:\n");
    const int64_t n = 100000;  // > LANE_TARGET (16384) ⇒ multiple lanes
    const int m = 3;
    std::mt19937 rng(99);
    std::normal_distribution<double> N(0, 1);

    std::vector<int32_t> g1(n), g2(n);
    for (int64_t i = 0; i < n; ++i) {
        g1[i] = (int32_t)(i % 137);     // FE 1
        g2[i] = (int32_t)((i / 7) % 53); // FE 2
    }
    std::vector<Factor> fe = {mkfac(g1), mkfac(g2)};

    std::vector<double> base(static_cast<size_t>(n) * m);
    for (auto& v : base) v = N(rng);

    int fail = 0;
    // single FE and two FE
    for (int nfe = 1; nfe <= 2; ++nfe) {
        std::vector<Factor> sub(fe.begin(), fe.begin() + nfe);
        std::vector<double> par = base, ser = base;
        demean(par.data(), n, m, sub, nullptr, 1e-12, 10000, /*serial=*/false);
        demean(ser.data(), n, m, sub, nullptr, 1e-12, 10000, /*serial=*/true);
        bool ok = bit_equal(par, ser);
        std::printf("  %d-FE serial==parallel (bitwise): %s\n", nfe, ok ? "ok" : "FAIL");
        if (!ok) ++fail;
    }

    // weighted demean determinism
    {
        std::vector<double> w(n);
        for (int64_t i = 0; i < n; ++i) w[i] = 0.5 + std::fabs(N(rng));
        std::vector<double> par = base, ser = base;
        demean(par.data(), n, m, fe, w.data(), 1e-12, 10000, false);
        demean(ser.data(), n, m, fe, w.data(), 1e-12, 10000, true);
        bool ok = bit_equal(par, ser);
        std::printf("  weighted serial==parallel (bitwise): %s\n", ok ? "ok" : "FAIL");
        if (!ok) ++fail;
    }

    if (fail) { std::printf("FAILED (%d)\n", fail); return 1; }
    std::printf("PASSED\n");
    return 0;
}
