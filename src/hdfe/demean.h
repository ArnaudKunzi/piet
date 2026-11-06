// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// cecon — high-dimensional fixed-effect demeaning via the Method of Alternating
// Projections (within transformation), à la reghdfe / fixest / FixedEffectModels.
#pragma once
#include <cstdint>
#include <vector>

#include "core/model.h"

namespace cecon {

struct DemeanReport {
    int iters = 0;
    double resid = 0.0;  // final max relative group-mean adjustment
    bool converged = true;
};

// Demean the m columns of M (column-major, n x m, leading dim n) in place by the
// fixed-effect set `fe`. If `w` is non-null, subtract WEIGHTED group means
// (sum w_i v_i / sum w_i) — used by FE-Poisson IRLS. One FE converges in a single
// sweep; two FE iterate plain alternating projections; three+ FE use Anderson
// acceleration (cuts the iteration count for slow many-FE problems). Each O(n)
// pass is parallelized across observations via the global ThreadPool. Set
// `serial=true` when the caller is already parallel over an outer loop (e.g. the
// bootstrap reps) so the inner passes don't oversubscribe.
DemeanReport demean(double* M, int64_t n, int m, const std::vector<Factor>& fe,
                    const double* w = nullptr, double tol = 1e-12, int maxiter = 10000,
                    bool serial = false);

}  // namespace cecon
