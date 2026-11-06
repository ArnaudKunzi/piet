// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// cecon — pooled OLS (no fixed effects yet) with classic and HC1-robust SEs.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "core/matrix.h"

namespace cecon {

struct OlsResult {
    int64_t n = 0;
    int k = 0;
    std::vector<std::string> names;
    std::vector<double> beta;
    std::vector<double> se_classic;
    std::vector<double> se_hc1;
    double ssr = 0;     // residual sum of squares
    double tss = 0;     // total sum of squares (about the mean if intercept)
    double r2 = 0;      // 1 - ssr/tss
    double sigma2 = 0;  // ssr / (n - k)
    int64_t n_dropped = 0;
    std::vector<std::string> omitted;  // collinear columns dropped (Stata omits, coef 0)
    std::vector<std::string> full_names;  // pre-drop design-matrix column order (BUG-112(6))
};

// Fit y ~ X. Computes both classic and HC1 SEs. Throws if X'X is singular.
OlsResult ols_fit(const DesignMatrix& dm);

}  // namespace cecon
