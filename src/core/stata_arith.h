// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// stata_arith.h -- exp(), invlogit(), round() and the `^` operator with StataBE's bits.
//
// One definition for every evaluator that serves these (cobra's generate/replace
// frame.cpp, the nlcom/nl/ml expression tree core/expr_eval.h, and PIET's
// display/scalar/local/if layer and `count if` shadow evaluator), so the layers
// cannot drift apart. Values only: callers keep their own missing/overflow
// mapping (a NaN or an infinity coming out of here becomes `.` there).
//
// Identified from StataBE 19.5 %21x bits (2026-10-27, internal probes: il.do, ex2.do,
// ex3.do, pw.do, pw2.do). The rules are the same in generate, display, scalar
// and Mata (Mata checked for exp/invlogit, `:^` and scalar `^`):
//
// exp(x): libm exp, except that an argument below -709 gives exactly 0 and an
//   argument above 709 is missing (exp(709) = +1.d422d2be5dc9bX+3fe is finite, a
//   result of 8.2e307 that is still below 2^1023, while exp(709.0000000000001),
//   exp(709.5) and exp(709.78) are `.`; 662 finite and 5,338 missing points on
//   [708.9, 709.8], stata_probe2/probe.do: the cut is the argument, not the size
//   of the result).
//   exp(-1.6280000000000X+009) = exp(-709) = +0.8bfe55de02338X-3ff (a subnormal,
//   libm's value) while exp(-709.0000000000001) = 0; 4,200 points on
//   [-709.16, -708.74] and 3,000 on [-760, -685] agree with this rule bit for bit
//   (the old rule "a result below 1e-323 is 0" kept the subnormal of
//   exp(-717.5) where Stata has 0).
//
// invlogit(x): exp(x)/(1+exp(x)) for x < 0 and 1/(1+exp(-x)) for x >= 0, with
//   the exp() above. invlogit(z) == exp(z)/(1+exp(z)) on all 3,000 tail points,
//   so invlogit(-717.5) is 0 (not a subnormal) because exp(-717.5) is.
//
// x^y:
//   * y an integer with |y| <= 64: repeated multiplication, left to right:
//     x^0 = 1, x^n = ((x*x)*x)*...*x, and for n < 0 the reciprocal first:
//     b = 1/x, x^n = ((b*b)*b)*...*b. (x^4 is x*x*x*x, not (x*x)*(x*x); x^-2 is
//     (1/x)*(1/x), not 1/(x*x).) 20,000 random doubles per exponent for
//     n = 0..12, 16, 31, 32, 33, 60, 63, 64 and -1..-5, -10, -31, -60, -64.
//   * every other y (non-integer, or an integer beyond 64 in magnitude):
//     exp(y*ln(x)) with the exp() above; a negative base with an integer y takes
//     the sign of the parity, (-2)^65 = -1.0000000000002X+041 (not -2^65); a
//     negative base with a non-integer y is missing; 0^y is 0 for y > 0 and
//     missing for y < 0. 20,000 points each for y = .25, .5, 1.5, 2.5, 3.5, -.5
//     and random y in (-10, 10); 3,000 each for |y| = 65..1000, 1e10,
//     2.0000000000000004.
//   * a result of 2^1023 or more is missing for `^` (9.5e153^2 is `.` where
//     9.5e153*9.5e153 keeps its bits; a negative result is not clamped:
//     (-4.5e102)^3 = -1.038859727d162X+3ff, stata_probe2/p2.do, p3.do). With the
//     exp() rule above this covers the 34 points of the 10.04M-point check that
//     were finite here and `.` in Stata (x^100, x^-64, x^200.5 near overflow).
//   * a missing operand gives missing, including .^0 and 1^. (C's pow gives 1).
//   libm pow(x, 2) is not correctly rounded on ~0.1% of doubles, and pow(x, 3)
//   differs from Stata's x*x*x on ~26%.
//
// round(x, y): y*floor(x/y + .5), halves up (round(-2.5) is -2, round(-0.25,.5)
//   is 0), and round(x, 0) is x unmodified ([FN] round()). No shortcut past 2^52:
//   round(4503599627370497) is 4503599627370498 in Stata, because x + .5 rounds
//   to even before floor() sees it. 20 cases, display and generate alike, bit
//   for bit (lane expr-sem m8.do, 2026-10-29, BUG-1364). round(x) is y = 1.
#pragma once

#include <cmath>
#include <limits>

namespace cecon {

inline double stata_exp_value(double x) {
    if (x < -709.0) return 0.0;
    if (x > 709.0) return std::nan("");
    return std::exp(x);
}

inline double stata_invlogit_value(double x) {
    if (std::isnan(x)) return x;
    if (x >= 0.0) return 1.0 / (1.0 + stata_exp_value(-x));
    const double e = stata_exp_value(x);
    return e / (1.0 + e);
}

inline double stata_pow_unclamped(double x, double y) {
    if (std::isnan(x) || std::isnan(y)) return std::nan("");
    if (y == std::trunc(y) && std::fabs(y) <= 64.0) {
        int n = static_cast<int>(y);
        if (n == 0) return 1.0;
        const double b = n > 0 ? x : 1.0 / x;
        if (n < 0) n = -n;
        double r = b;
        for (int i = 1; i < n; ++i) r = r * b;
        return r;
    }
    if (x > 0.0) return stata_exp_value(y * std::log(x));
    if (x == 0.0) return y > 0.0 ? 0.0 : std::numeric_limits<double>::infinity();
    if (y != std::trunc(y)) return std::nan("");
    const double mag = stata_exp_value(y * std::log(-x));
    return std::fmod(y, 2.0) != 0.0 ? -mag : mag;
}

inline double stata_pow_value(double x, double y) {
    const double r = stata_pow_unclamped(x, y);
    if (r >= 0x1p1023) return std::nan("");
    return r;
}

inline double stata_round_value(double x, double y) {
    if (y == 0.0) return x;
    return std::floor(x / y + 0.5) * y;
}

}  // namespace cecon
