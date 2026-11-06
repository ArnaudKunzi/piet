// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#pragma once
#include <cmath>  // std::fma (ExactSqDev)
// ExactSum: the summation rule of Stata's summary statistics.
//
// StataBE 19.5 accumulates a sum in more than double precision and rounds it
// once. Measured (tests/eval/golden/aer_w2/w2as_summary_stats_family.log,
// sx_sum_acc3/acc4/su_acc5 and their meanonly twins): 1e16, twenty-two 1s and
// -1e16 give r(sum) 22 (and 1e20 / 1e100 in place of 1e16 likewise), where a
// data-order double sum gives 0, an 80-bit long double sum gives 0 at 1e20 and a
// Kahan sum gives 0 at 1e100; acc1/acc2 (0.1/0.2/0.3 and 0.1*_n) give the exactly
// rounded sum. A double-double accumulator (Knuth TwoSum, the error terms kept
// in a second double) rounded once at the end reproduces every one of them, and
// the binscatter kernel reads Stata's means the same way (w2k_binscatter).
//
// One accumulator for the summary-statistic sums of the engine:
// Frame::summarize (summarize, meanonly, by-prefixed, and tabstat, which is
// summarize per group) and the binscatter kernel. A second rule would let r(sum)
// of `summarize` and tabstat's sum disagree on the same column. egen without
// by() reads summarize in Stata (total/mean/sd/std/pc), so
// Frame::egen_by uses it there too (BUG-1150). Not callers, by measurement:
// egen with by() (a running sum() in data order), gtools' gegen total/sum/mean
// (data order with or without by()) and collapse, whose (sum) and (mean) of
// 1e16, twenty-two 1s and -1e16 are 0 in StataBE 19.5.
namespace cecon {

struct ExactSum {
    double hi = 0.0, lo = 0.0;
    void add(double a) {
        const double s = hi + a;
        const double bb = s - hi;
        const double err = (hi - (s - bb)) + (a - bb);
        hi = s;
        lo += err;
    }
    double value() const { return hi + lo; }
};

// The sum of squared deviations from `m` that Stata's r(sd)/r(Var) rest on,
// accumulated exactly and rounded once: StataBE's r(sd) is the correctly
// rounded sqrt(sum (x - r(mean))^2 / (N-1)) (uni-oracle/sm.do:
// 0x1.943b553de0432p5 where a data-order double sum gave ...438). Each
// deviation is split exactly (TwoSum) and squared exactly (fma) into one
// ExactSum. Frame::summarize and PIET's swindex share it.
struct ExactSqDev {
    explicit ExactSqDev(double mean) : m(mean) {}
    void add(double x) {
        const double d = x - m;
        const double bb = d - x;
        const double de = (x - (d - bb)) + (-m - bb);
        const double p2 = d * d;
        acc.add(p2);
        acc.add(std::fma(d, d, -p2));
        acc.add(2.0 * d * de);
        acc.add(de * de);
    }
    double value() const { return acc.value(); }
    double m;
    ExactSum acc;
};

}  // namespace cecon
