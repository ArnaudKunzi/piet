// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// C2-F8 — value-identical radix sort for gsort keys (Step 3a data-layer perf).
//
// The former Frame::gsort used std::stable_sort with a comparator whose total
// order is: genuine NaN (f64 missing) sorts LAST in BOTH ascending and
// descending directions (a deliberate divergence from Stata, ledgered in
// contract §22 and pinned by the G13b permutation-identity gate); finite keys
// compare by value, negated for a '-' (descending) key; ties are stable.
//
// gsort_radix_key encodes that comparator LOSSLESSLY into an unsigned 64-bit
// key, so an LSD radix sort reproduces the comparator's permutation
// BIT-FOR-BIT. Kept in a header (not frame.cpp's anonymous namespace) purely so
// the fuzz unit test (tests/unit/test_radix_sort.cpp) can drive the exact same
// code path the engine uses.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <numeric>
#include <vector>

namespace cecon {
namespace radixsort {

// Monotone map of a materialized gsort key double -> uint64 whose UNSIGNED
// order matches gsort's comparator. Factor "missing" arrives as a finite -1
// (cast from the int32 code) and is therefore treated as an ordinary value; only
// a genuine f64 NaN is the sort-last sentinel.
inline uint64_t gsort_radix_key(double x, bool asc) {
    if (std::isnan(x)) return ~0ull;            // NaN last, both directions
    if (x == 0.0) x = 0.0;                       // collapse -0.0 -> +0.0
    uint64_t u;
    std::memcpy(&u, &x, sizeof(u));
    u ^= (u & 0x8000000000000000ull) ? ~0ull : 0x8000000000000000ull;  // ascending order
    return asc ? u : ~u;                         // '-' key reverses non-NaN space only
}

// Stable LSD radix argsort over key columns `K` (major-to-minor), per-column
// direction `asc`. Passes run minor-key-first, byte 0..7 within each key; every
// counting-sort pass is stable, so the result is ordered by (K[0],K[1],..) with
// the original row index as the ultimate tiebreak — identical to a
// std::stable_sort under the gsort comparator.
inline std::vector<int64_t> gsort_radix_order(const std::vector<std::vector<double>>& K,
                                              const std::vector<char>& asc, int64_t n) {
    std::vector<int64_t> order(n);
    std::iota(order.begin(), order.end(), 0);
    const size_t m = K.size();
    if (m == 0 || n < 2) return order;

    std::vector<std::vector<uint64_t>> keys(m, std::vector<uint64_t>(n));
    for (size_t k = 0; k < m; ++k) {
        const bool a = asc[k] != 0;
        const std::vector<double>& col = K[k];
        std::vector<uint64_t>& dst = keys[k];
        for (int64_t i = 0; i < n; ++i) dst[i] = gsort_radix_key(col[i], a);
    }

    std::vector<int64_t> tmp(n);
    for (size_t kk = m; kk-- > 0;) {            // minor key last column ... major key first
        const std::vector<uint64_t>& key = keys[kk];
        for (int shift = 0; shift < 64; shift += 8) {
            size_t count[257];
            std::memset(count, 0, sizeof(count));
            for (int64_t i = 0; i < n; ++i)
                ++count[((key[order[i]] >> shift) & 0xFFu) + 1];
            for (int b = 0; b < 256; ++b) count[b + 1] += count[b];
            for (int64_t i = 0; i < n; ++i) {
                uint64_t bkt = (key[order[i]] >> shift) & 0xFFu;
                tmp[count[bkt]++] = order[i];
            }
            order.swap(tmp);
        }
    }
    return order;
}

}  // namespace radixsort
}  // namespace cecon
