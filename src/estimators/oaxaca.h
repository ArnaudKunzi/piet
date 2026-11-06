// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#pragma once
#include "core/matrix.h"
#include <array>

namespace cecon {
struct OaxacaResult {
    std::vector<std::string> names;
    std::vector<double> beta, V; // full covariance, column-major
    std::array<int64_t,2> n{};
    bool omitted = false;
};
// Linear pooled twofold decomposition. dm includes an intercept; group is 0/1
// on the same complete sample. Jann (2008), robust suest and stochastic means.
OaxacaResult oaxaca_fit(const DesignMatrix& dm, const std::vector<int>& group, bool relax);
}
