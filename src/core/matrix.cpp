// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "core/matrix.h"
#include "core/error.h"

#include <cmath>
#include <stdexcept>

namespace cecon {

static const double* require_f64(const Dataset& ds, const std::string& name) {
    const Column& c = ds.col(name);
    if (c.dtype != DType::F64)
        throw EngineError("factor_misuse", "column '" + name + "' is not numeric (f64)", name);
    return c.f64();
}

DesignMatrix build_design(const Dataset& ds, const std::string& yname,
                          const std::vector<std::string>& xnames, bool intercept) {
    const int64_t N = ds.n_rows();
    const double* yp = require_f64(ds, yname);
    std::vector<const double*> xp;
    xp.reserve(xnames.size());
    for (const auto& nm : xnames) xp.push_back(require_f64(ds, nm));

    // Pass 1: which rows are complete?
    std::vector<int64_t> keep;
    keep.reserve(N);
    for (int64_t i = 0; i < N; ++i) {
        if (std::isnan(yp[i])) continue;
        bool ok = true;
        for (const double* p : xp)
            if (std::isnan(p[i])) {
                ok = false;
                break;
            }
        if (ok) keep.push_back(i);
    }

    DesignMatrix dm;
    dm.n = static_cast<int64_t>(keep.size());
    dm.n_dropped = N - dm.n;
    const int nx = static_cast<int>(xnames.size());
    dm.k = nx + (intercept ? 1 : 0);
    dm.X.resize(static_cast<size_t>(dm.n) * dm.k);
    dm.y.resize(dm.n);
    dm.xnames.reserve(dm.k);

    // Pass 2: fill (column-major). Intercept first if present.
    int col = 0;
    if (intercept) {
        double* c0 = dm.col(col++);
        for (int64_t r = 0; r < dm.n; ++r) c0[r] = 1.0;
        dm.xnames.push_back("(intercept)");
    }
    for (int j = 0; j < nx; ++j) {
        double* cj = dm.col(col++);
        const double* src = xp[j];
        for (int64_t r = 0; r < dm.n; ++r) cj[r] = src[keep[r]];
        dm.xnames.push_back(xnames[j]);
    }
    for (int64_t r = 0; r < dm.n; ++r) dm.y[r] = yp[keep[r]];
    return dm;
}

}  // namespace cecon
