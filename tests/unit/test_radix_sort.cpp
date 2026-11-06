// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// C2-F8 permutation-identity fuzz: gsort_radix_order (radix_sort.h) must
// reproduce, BIT-FOR-BIT, the exact permutation the former Frame::gsort
// std::stable_sort comparator produced — including NaN-last-both-directions,
// stable ties, '-' descending keys, -0.0/+0.0 collapse, and ±inf. This is the
// C++ counterpart of the eval G13b (sort_permutation_identity) gate, but fuzzed
// over random multi-key/mixed-direction data with all the pathological values.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <random>
#include <vector>

#include "data/radix_sort.h"

using cecon::radixsort::gsort_radix_order;

// The former gsort comparator, verbatim, as the reference oracle.
static std::vector<int64_t> reference_order(const std::vector<std::vector<double>>& K,
                                            const std::vector<char>& asc, int64_t n) {
    std::vector<int64_t> order(n);
    for (int64_t i = 0; i < n; ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](int64_t a, int64_t b) {
        for (size_t k = 0; k < K.size(); ++k) {
            double x = K[k][a], y = K[k][b];
            const bool nx = std::isnan(x), ny = std::isnan(y);
            if (nx || ny) {
                if (nx && ny) continue;
                return ny;
            }
            if (!asc[k]) { x = -x; y = -y; }
            if (x < y) return true;
            if (x > y) return false;
        }
        return false;
    });
    return order;
}

static int g_fail = 0;
static void check(const std::vector<std::vector<double>>& K, const std::vector<char>& asc,
                  int64_t n, const char* label) {
    std::vector<int64_t> want = reference_order(K, asc, n);
    std::vector<int64_t> got = gsort_radix_order(K, asc, n);
    if (want != got) {
        ++g_fail;
        std::printf("FAIL [%s] n=%lld\n  want:", label, (long long)n);
        for (int64_t i = 0; i < n && i < 40; ++i) std::printf(" %lld", (long long)want[i]);
        std::printf("\n  got :");
        for (int64_t i = 0; i < n && i < 40; ++i) std::printf(" %lld", (long long)got[i]);
        std::printf("\n");
    }
}

int test_radix_sort_main() {
    const double NA = std::numeric_limits<double>::quiet_NaN();
    const double INF = std::numeric_limits<double>::infinity();

    // 1. Hand fixtures over the pathological value alphabet.
    std::vector<double> alpha = {NA, -INF, INF, 0.0, -0.0, 1.0, -1.0, 2.0, -2.0,
                                 1e300, -1e300, 3.0, 3.0, NA, -0.0};
    check({alpha}, {1}, (int64_t)alpha.size(), "alpha-asc");
    check({alpha}, {0}, (int64_t)alpha.size(), "alpha-desc");

    // all-NaN column: stability must keep input order in both directions.
    std::vector<double> allnan(12, NA);
    check({allnan}, {1}, 12, "allnan-asc");
    check({allnan}, {0}, 12, "allnan-desc");

    // 2. Randomized fuzz: multi-key, mixed direction, factor-code-like small
    //    ints, heavy ties, sprinkled NaN/±inf/±0.0.
    std::mt19937_64 rng(0xC2F8ull);
    std::uniform_int_distribution<int> nrows(1, 400);
    std::uniform_int_distribution<int> ncols(1, 4);
    std::uniform_int_distribution<int> pick(0, 99);
    std::uniform_int_distribution<int> smallint(-3, 5);   // factor-code-like
    for (int t = 0; t < 4000; ++t) {
        int64_t n = nrows(rng);
        int m = ncols(rng);
        std::vector<std::vector<double>> K(m, std::vector<double>(n));
        std::vector<char> asc(m);
        for (int k = 0; k < m; ++k) {
            asc[k] = (pick(rng) & 1) ? 1 : 0;
            bool factorish = (pick(rng) < 40);  // some columns behave like factor codes
            for (int64_t i = 0; i < n; ++i) {
                int p = pick(rng);
                double v;
                if (factorish) {
                    v = (double)smallint(rng);          // ties galore, all finite
                } else if (p < 8)  v = NA;
                else if (p < 11)   v = INF;
                else if (p < 14)   v = -INF;
                else if (p < 17)   v = 0.0;
                else if (p < 20)   v = -0.0;
                else               v = (double)(smallint(rng) * 1000 + (p - 20));
                K[k][i] = v;
            }
        }
        check(K, asc, n, "fuzz");
        if (g_fail > 5) break;  // stop spamming once broken
    }

    if (g_fail) {
        std::printf("test_radix_sort: %d FAILURES\n", g_fail);
        return 1;
    }
    std::printf("test_radix_sort: radix order == stable_sort comparator on all fixtures + 4000 fuzz cases\n");
    return 0;
}
