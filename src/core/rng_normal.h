// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// P7 wave — Gaussian and chi-square draws on the existing counter-based Philox
// stream (src/core/rng.h), which supplies uniforms only (rademacher/webb6).
//
// Inverse-CDF, not Box-Muller: one uniform per draw makes a draw addressable by
// (seed, stream, counter), so the m-th imputation's j-th value does not depend on
// how many draws were taken before it. That is the property the P6 RNG ruling
// actually needs — OUR stream reproducible, order-independent — and rejection or
// pairing methods do not have it.
//
// WHY THIS INCLUDES estimators/posthest.h. chi2_icdf is the inverse of the
// regularized incomplete gamma, and the engine already has exactly one
// implementation of that function (cecon::gammp). Growing a second copy here so
// that this header could stay "core-only" is the failure mode the shared
// core/distributions.h was created to end, so the single include edge is
// deliberate: this header inverts gammp, it does not re-derive it. Nothing under
// src/core/ includes this header, so no core translation unit gains the edge.
#pragma once
#include <cmath>
#include <cstdint>
#include <limits>

#include "core/rng.h"
#include "estimators/posthest.h"

namespace cecon {

// ---------------------------------------------------------------------------
// The uniform underneath every draw
// ---------------------------------------------------------------------------
//
// One LOGICAL uniform per (seed, stream, counter), built from two Philox words.
// Philox::draw32 returns only c[0] of the 4x32 block, so a single call yields 32
// bits — a 2^-32 lattice caps |z| at 6.33 and makes the far tail of a normal draw
// visibly discrete. Two calls at the SAME logical counter, distinguished by the
// high half of Philox's g index (which the callers below never use for anything
// else), give a full 53-bit mantissa without costing the addressability: the pair
// is still a pure function of (seed, stream, counter).
//
// Returns a value strictly inside (0, 1), so normal_icdf never sees 0 or 1.
// `counter` must be < 2^32 (the high half is this function's sub-word selector);
// every caller here indexes rows and coefficients, so that ceiling is 4 billion
// draws per imputation.
inline double uniform01(uint64_t seed, uint64_t stream, uint64_t counter) {
    const uint64_t hi = Philox::draw32(seed, stream, counter);
    const uint64_t lo = Philox::draw32(seed, stream, counter | (1ull << 32));
    // 27 high bits + 26 high bits = 53, the exact mantissa width of a double.
    const uint64_t bits = ((hi >> 5) << 26) | (lo >> 6);
    return (static_cast<double>(bits) + 0.5) / 9007199254740992.0;  // 2^53
}

// normal_icdf (Wichura's AS241) lives in core/distributions.h, which this header
// reaches through estimators/posthest.h: Stata's invnormal() in the expression
// language needed the same quantile, and one polynomial is the whole point.

// ---------------------------------------------------------------------------
// Chi-square quantile — Newton on gammp, bracketed, bounded iteration count
// ---------------------------------------------------------------------------
//
// chi2(nu) is gamma(shape = nu/2, scale = 2), so P(X <= x) = gammp(nu/2, x/2).
// The starting point is Wilson-Hilferty, which is already within a fraction of a
// percent for the nu the imputer uses (nu = n - k, in the thousands); Newton then
// converges quadratically. A [lo, hi] bracket is carried alongside and every step
// that would leave it falls back to a bisection — so the loop is DETERMINISTIC and
// terminates in a fixed number of iterations regardless of the input, which a
// rejection sampler would not.
inline double chi2_icdf(double p, double nu) {
    if (!(nu > 0.0)) return std::numeric_limits<double>::quiet_NaN();
    if (!(p > 0.0)) return 0.0;
    if (!(p < 1.0)) return std::numeric_limits<double>::infinity();
    const double a = 0.5 * nu;

    // Wilson-Hilferty: X/nu ~ (1 - 2/(9nu) + Z sqrt(2/(9nu)))^3.
    const double z = normal_icdf(p);
    const double h = 2.0 / (9.0 * nu);
    double t = 1.0 - h + z * std::sqrt(h);
    double x = (t > 0.0) ? nu * t * t * t : nu * 1e-3;  // gamma-scale below
    x *= 0.5;                                           // work in the gamma variable x/2
    if (!(x > 0.0)) x = 1e-12;

    double lo = 0.0, hi = std::numeric_limits<double>::infinity();
    const double lg = std::lgamma(a);
    for (int it = 0; it < 100; ++it) {
        const double f = gammp(a, x) - p;
        if (f > 0.0) { if (x < hi) hi = x; } else { if (x > lo) lo = x; }
        // d/dx gammp(a, x) = x^(a-1) e^-x / Gamma(a), formed in logs (a is in the
        // thousands here, so the unlogged form overflows long before it is useful).
        const double dens = std::exp((a - 1.0) * std::log(x) - x - lg);
        double step = (dens > 0.0) ? f / dens : 0.0;
        double xn = x - step;
        if (!(xn > lo) || !(xn < hi) || !std::isfinite(xn)) {
            // Newton left the bracket (or the density underflowed): bisect. When
            // the upper end is still unbounded, expand geometrically instead.
            xn = std::isfinite(hi) ? 0.5 * (lo + hi) : (x > 0.0 ? 2.0 * x : 1.0);
        }
        if (std::fabs(xn - x) <= 1e-15 * std::fabs(xn)) { x = xn; break; }
        x = xn;
    }
    return 2.0 * x;  // gamma(a, scale 1) -> chi-square(nu)
}

// ---------------------------------------------------------------------------
// Counter-addressed draws
// ---------------------------------------------------------------------------
// N(0,1) at the address (seed, stream, counter).
inline double normal_draw(uint64_t seed, uint64_t stream, uint64_t counter) {
    return normal_icdf(uniform01(seed, stream, counter));
}

// chi-square(nu) at the address (seed, stream, counter).
inline double chi2_draw(uint64_t seed, uint64_t stream, uint64_t counter, double nu) {
    return chi2_icdf(uniform01(seed, stream, counter), nu);
}

}  // namespace cecon
