// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// cecon — Philox4x32-10 counter-based RNG. Stateless: a draw is a pure function
// of (seed, counter), so resampling is bit-identical regardless of how replicates
// are scheduled across threads. Used for wild-bootstrap / permutation weights.
#pragma once
#include <cstdint>

namespace cecon {

struct Philox {
    static inline void mulhilo(uint32_t a, uint32_t b, uint32_t& hi, uint32_t& lo) {
        uint64_t p = static_cast<uint64_t>(a) * b;
        hi = static_cast<uint32_t>(p >> 32);
        lo = static_cast<uint32_t>(p);
    }
    // 4x32 bijection of counter c under key k (10 rounds).
    static inline void bijection(uint32_t c[4], uint32_t k[2]) {
        const uint32_t M0 = 0xD2511F53u, M1 = 0xCD9E8D57u, W0 = 0x9E3779B9u, W1 = 0xBB67AE85u;
        for (int r = 0; r < 10; ++r) {
            uint32_t hi0, lo0, hi1, lo1;
            mulhilo(M0, c[0], hi0, lo0);
            mulhilo(M1, c[2], hi1, lo1);
            uint32_t n0 = hi1 ^ c[1] ^ k[0];
            uint32_t n1 = lo1;
            uint32_t n2 = hi0 ^ c[3] ^ k[1];
            uint32_t n3 = lo0;
            c[0] = n0; c[1] = n1; c[2] = n2; c[3] = n3;
            k[0] += W0; k[1] += W1;
        }
    }
    // One 32-bit draw indexed by (seed, b, g).
    static inline uint32_t draw32(uint64_t seed, uint64_t b, uint64_t g) {
        uint32_t c[4] = {static_cast<uint32_t>(b), static_cast<uint32_t>(b >> 32),
                         static_cast<uint32_t>(g), static_cast<uint32_t>(g >> 32)};
        uint32_t k[2] = {static_cast<uint32_t>(seed), static_cast<uint32_t>(seed >> 32)};
        bijection(c, k);
        return c[0];
    }
};

// Rademacher: +1/-1 with prob 1/2 each.
inline double rademacher(uint64_t seed, uint64_t b, uint64_t g) {
    return (Philox::draw32(seed, b, g) >> 31) ? 1.0 : -1.0;
}

// Webb 6-point: {±sqrt(1/2), ±1, ±sqrt(3/2)} each prob 1/6 (better for few clusters).
inline double webb6(uint64_t seed, uint64_t b, uint64_t g) {
    static const double v[6] = {-1.2247448713915890, -1.0, -0.7071067811865476,
                                 0.7071067811865476, 1.0, 1.2247448713915890};
    double u = (Philox::draw32(seed, b, g) + 0.5) / 4294967296.0;  // (0,1)
    int idx = static_cast<int>(u * 6.0);
    if (idx > 5) idx = 5;
    return v[idx];
}

}  // namespace cecon
