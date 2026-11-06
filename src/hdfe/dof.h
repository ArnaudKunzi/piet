// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// cecon — degrees-of-freedom and fixed-effect redundancy accounting.
// This is the most error-prone module for standard-error correctness, so the
// result echoes the full breakdown.
#pragma once
#include <cstdint>
#include <vector>

#include "core/model.h"

namespace cecon {

struct DofInfo {
    int64_t n = 0;
    int k_vars = 0;            // estimated slope coefficients (X)
    int k_fe = 0;              // absorbed FE parameters, redundancy removed
    int fe_redundant = 0;      // sum(levels) - k_fe
    int64_t dof_resid = 0;     // n - k_vars - k_fe
    bool fe_exact = true;      // false when k_fe may still overstate the rank (3+ FE dims)
    std::vector<int> fe_levels;
};

// k_vars = number of X slope coefficients (no intercept when FE present).
// k_fe = absorbed_dof(fe, {}): 1 FE: levels. 2 FE: K1 + K2 - (#connected components),
// exact. 3+ FE: reghdfe's pairwise bound, flagged inexact.
DofInfo compute_dof(int64_t n, int k_vars, const std::vector<Factor>& fe);

// reghdfe's e(df_a) for intercept-only absorbed effects under its default
// dofadjustments (pairwise clusters), reghdfe.mata estimate_dof:
//  - an effect nested in a cluster variable (nested[g] != 0) has every level
//    redundant (M = K);
//  - among the rest, every effect but the first has at least one redundant level,
//    and so does the first when some effect was nested;
//  - each later effect j takes M_j = max(M_j, connected components of (i, j)) over
//    every earlier non-nested effect i (exact for the first pair, a bound after).
// df_a = sum(K) - sum(M). nested may be empty (no cluster nesting).
struct AbsorbedDof {
    int df_a = 0;
    int df_a_nested = 0;       // levels redundant through cluster nesting
};
AbsorbedDof absorbed_dof(const std::vector<Factor>& fe, const std::vector<char>& nested);

}  // namespace cecon
