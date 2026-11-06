// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// cecon — build a dense column-major design matrix from a Dataset, with
// listwise deletion of rows that have a missing value in y or any regressor.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "io/columnar.h"

namespace cecon {

struct DesignMatrix {
    int64_t n = 0;  // rows used (after listwise deletion)
    int k = 0;      // number of regressors (including intercept if requested)
    std::vector<double> X;  // column-major, n*k
    std::vector<double> y;  // length n
    std::vector<std::string> xnames;
    int64_t n_dropped = 0;  // rows removed by listwise deletion

    const double* col(int j) const { return X.data() + static_cast<int64_t>(j) * n; }
    double* col(int j) { return X.data() + static_cast<int64_t>(j) * n; }
};

// Build  y ~ (intercept?) + xnames  from f64 columns of `ds`, dropping rows with
// any NaN among the involved columns. Throws if a column is missing or not f64.
DesignMatrix build_design(const Dataset& ds, const std::string& yname,
                          const std::vector<std::string>& xnames, bool intercept);

}  // namespace cecon
