// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// cecon — runiform() / runiformint(), on top of the Philox counter RNG.
//
// AER scope 2, B3(c). Doctrine gate 1 has a named carve-out for this: a
// stochastic estimator satisfies it as 1-S, meaning fixed-seed bit
// reproducibility HERE plus distributional agreement -- never draw-level
// identity with Stata. Stata's stream is Stata's; this one is ours, and the
// corpus lines that use it (`gen tiebreak = ... + runiform(-0.1,0.1)`,
// `gen anchor = runiformint(212,239)`) are tie-breakers and placebo draws whose
// papers depend on the distribution, not on the individual numbers.
//
// This reuses core/rng.h's Philox rather than introducing a second generator:
// its header already states the property this needs -- "a draw is a pure
// function of (seed, counter)" -- and the wild bootstrap already depends on it.
//
// Why a pure function rather than a running generator. This engine has TWO
// evaluators that walk rows in different orders (the scalar ev() and the
// column-at-a-time ev_fn_vec()), plus a bysort path that iterates in sort order.
// A stateful generator would hand back different numbers depending on which one
// ran, silently, because both answers look like noise. Keying every draw on its
// coordinates removes the question: the same cell gets the same number whoever
// asks. The coordinates are
//   seed    `set seed`, carried from the PIET session (this process is a
//           subprocess and remembers nothing between calls)
//   stream  a per-call counter, so two successive `gen`s do not repeat
//   node    which runiform() in the expression, left to right
//   row     the observation
#pragma once
#include <cstdint>

#include "core/rng.h"

namespace cecon {

// Philox::draw32 gives 32 bits per call; a double needs its full 53-bit
// mantissa, so two calls are combined. The `half` flag rides in the top bit of
// the counter word so the two halves cannot collide with a neighbouring
// (stream, node) pair.
inline uint64_t uniform_bits(int64_t seed, int64_t stream, int64_t node, int64_t row) {
    const uint64_t base = (static_cast<uint64_t>(static_cast<uint32_t>(stream)) << 32) |
                          static_cast<uint32_t>(node);
    const uint64_t hi = Philox::draw32(static_cast<uint64_t>(seed), base,
                                       static_cast<uint64_t>(row));
    const uint64_t lo = Philox::draw32(static_cast<uint64_t>(seed), base ^ (1ull << 63),
                                       static_cast<uint64_t>(row));
    return (hi << 32) | lo;
}

// [0,1). Taking the top 53 bits makes every representable double in the interval
// reachable and none favoured.
inline double rng_uniform(int64_t seed, int64_t stream, int64_t node, int64_t row) {
    return static_cast<double>(uniform_bits(seed, stream, node, row) >> 11) *
           (1.0 / 9007199254740992.0);
}

// runiformint(a,b): a uniform INTEGER in [a,b]. Lemire's multiply-shift over the
// closed range, so the mapping is unbiased rather than the modulo it resembles.
// Stata answers the endpoint itself for a degenerate range (runiformint(7,7) is
// 7, measured), which falls out of range == 1.
inline double rng_uniform_int(int64_t seed, int64_t stream, int64_t node, int64_t row,
                              double a, double b) {
    const uint64_t range = static_cast<uint64_t>(b - a) + 1ull;
    const __uint128_t wide = static_cast<__uint128_t>(uniform_bits(seed, stream, node, row)) *
                             static_cast<__uint128_t>(range);
    return a + static_cast<double>(static_cast<uint64_t>(wide >> 64));
}

}  // namespace cecon
