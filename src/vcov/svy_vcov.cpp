// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "svy_vcov.h"

#include <cstddef>
#include <vector>

namespace cecon {

SvyMeatResult svy_linearized_meat(const double* S, int64_t n, int p, const Factor& psu,
                                  const Factor& stratum, const double* weights) {
    SvyMeatResult r;
    r.n_psu = psu.n_levels;
    r.n_strata = stratum.n_levels;
    r.meat.assign(static_cast<size_t>(p) * p, 0.0);

    double pop = 0.0;
    for (int64_t i = 0; i < n; ++i) pop += weights ? weights[i] : 1.0;
    r.pop_size = pop;

    if (psu.n_levels <= 0 || stratum.n_levels <= 0) {
        r.ok = false;
        return r;
    }

    // PSU totals z_c (n_psu x p, column-major) and the stratum each PSU sits in.
    // PSUs are assumed nested in strata (the design svyset describes). A PSU seen
    // under two strata is a malformed design, and silently keeping the first
    // stratum would answer a different question — so it clears ok instead.
    std::vector<double> z(static_cast<size_t>(psu.n_levels) * p, 0.0);
    std::vector<int> psu_stratum(psu.n_levels, -1);
    for (int64_t i = 0; i < n; ++i) {
        int c = psu.codes[static_cast<size_t>(i)];
        int h = stratum.codes[static_cast<size_t>(i)];
        if (c < 0 || h < 0) continue;
        if (psu_stratum[c] == -1)
            psu_stratum[c] = h;
        else if (psu_stratum[c] != h)
            r.ok = false;
        for (int j = 0; j < p; ++j)
            z[static_cast<size_t>(j) * psu.n_levels + c] += S[static_cast<size_t>(j) * n + i];
    }
    if (!r.ok) return r;

    std::vector<int> n_h(stratum.n_levels, 0);
    for (int c = 0; c < psu.n_levels; ++c)
        if (psu_stratum[c] >= 0) ++n_h[psu_stratum[c]];
    for (int h = 0; h < stratum.n_levels; ++h) {
        if (n_h[h] == 1) {
            r.singleton_stratum = stratum.name;
            return r;  // caller rejects honestly; meat deliberately left zero
        }
    }

    // Stratum means of the PSU totals.
    std::vector<double> zbar(static_cast<size_t>(stratum.n_levels) * p, 0.0);
    for (int c = 0; c < psu.n_levels; ++c) {
        int h = psu_stratum[c];
        if (h < 0) continue;
        for (int j = 0; j < p; ++j)
            zbar[static_cast<size_t>(j) * stratum.n_levels + h] +=
                z[static_cast<size_t>(j) * psu.n_levels + c];
    }
    for (int h = 0; h < stratum.n_levels; ++h) {
        if (n_h[h] <= 0) continue;
        for (int j = 0; j < p; ++j) zbar[static_cast<size_t>(j) * stratum.n_levels + h] /= n_h[h];
    }

    // Σ_h [n_h/(n_h−1)] Σ_{c∈h} (z_c − z̄_h)(z_c − z̄_h)'
    std::vector<double> d(static_cast<size_t>(p));
    for (int c = 0; c < psu.n_levels; ++c) {
        int h = psu_stratum[c];
        if (h < 0 || n_h[h] < 2) continue;
        const double f = static_cast<double>(n_h[h]) / static_cast<double>(n_h[h] - 1);
        for (int j = 0; j < p; ++j)
            d[static_cast<size_t>(j)] = z[static_cast<size_t>(j) * psu.n_levels + c] -
                                        zbar[static_cast<size_t>(j) * stratum.n_levels + h];
        for (int a = 0; a < p; ++a)
            for (int b = 0; b < p; ++b)
                r.meat[static_cast<size_t>(b) * p + a] +=
                    f * d[static_cast<size_t>(a)] * d[static_cast<size_t>(b)];
    }
    return r;
}

}  // namespace cecon
