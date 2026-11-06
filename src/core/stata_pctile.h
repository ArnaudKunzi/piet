// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// cecon — Stata's default percentile (`_pctile`, `summarize, detail`, no altdef),
// the cutoffs winsor2 reads. ONE definition so the engine's --winsor and PIET's
// winsor2 trim cannot disagree.
//
// Over the n sorted nonmissing values x[1..n], P = n*p/100: a whole P averages
// x[P] and x[P+1], any other P takes x[ceil(P)]. p <= 0 is the minimum and
// p >= 100 the maximum, which is what winsor2's one-sided cuts mean (`cuts(0 98)`
// leaves the low end alone). StataBE scratch-r150/uni-oracle/wq.do, 10 values
// 1..9,100: p10 1.5, p90 54.5, p25 3, p75 8, p3 1, p97 100.
#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace cecon {

// `sorted`: the nonmissing values, ascending. NaN when there are none.
inline double stata_pctile_sorted(const std::vector<double>& sorted, double p) {
    const std::size_t n = sorted.size();
    if (n == 0) return std::numeric_limits<double>::quiet_NaN();
    if (p <= 0) return sorted.front();
    if (p >= 100) return sorted.back();
    const double P = static_cast<double>(n) * p / 100.0;
    const double fl = std::floor(P);
    if (P == fl) {
        const std::size_t i = static_cast<std::size_t>(fl);  // 1-based x[P] is sorted[i-1]
        if (i >= n) return sorted.back();
        if (i == 0) return sorted.front();
        return (sorted[i - 1] + sorted[i]) / 2.0;
    }
    const std::size_t i = static_cast<std::size_t>(std::ceil(P));
    return sorted[std::min(i, n) - 1];
}

}  // namespace cecon
