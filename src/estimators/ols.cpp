// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "estimators/ols.h"
#include "core/error.h"

#include <cmath>
#include <stdexcept>

#include "core/collinear.h"
#include "core/linalg.h"

namespace cecon {

OlsResult ols_fit(const DesignMatrix& dm) {
    const int64_t n = dm.n;

    OlsResult r;
    r.n = n;
    r.n_dropped = dm.n_dropped;

    // Omit collinear regressors using a norm-pivoted basis. Operate on the original design
    // when full rank (the common case, zero copy); only materialize a reduced
    // copy when something is actually dropped.
    int k = dm.k;
    const double* X = dm.X.data();
    std::vector<std::string> names = dm.xnames;
    std::vector<double> Xreduced;
    {
        int intercept = -1;
        for (int j = 0; j < k; ++j)
            if (names[j] == "(intercept)" || names[j] == "_cons") intercept = j;
        std::vector<char> keep = collinear_pivot_keep_mask(X, n, k, intercept);
        int nkeep = 0;
        for (char c : keep) nkeep += c;
        if (nkeep < k) {
            Xreduced.assign(dm.X.begin(), dm.X.end());
            r.omitted = compact_columns(Xreduced, n, k, names, keep);
            X = Xreduced.data();
        }
    }
    require_sample(n);
    if (n <= k) throw EngineError("no_obs", "not enough observations: n <= k");
    r.k = k;
    r.names = names;
    // dm is const here (compact_columns above operated on the local `names` copy,
    // never dm.xnames), so it still holds the pre-drop column order verbatim.
    r.full_names = dm.xnames;

    // bread = (X'X)^{-1}
    std::vector<double> XtX(static_cast<size_t>(k) * k);
    la::xtx(X, n, k, XtX.data());
    int info = la::spd_inverse(XtX.data(), k);  // XtX now holds the inverse
    if (info != 0) throw EngineError("collinear", "X'X not positive-definite (collinear regressors?)");
    const std::vector<double>& bread = XtX;

    // beta = bread * X'y
    std::vector<double> Xty(k);
    la::xtv(X, n, k, dm.y.data(), Xty.data());
    r.beta.resize(k);
    la::matvec(bread.data(), k, Xty.data(), r.beta.data());

    // residuals, ssr, tss, r2, sigma2
    std::vector<double> e(n);
    la::residual(X, n, k, r.beta.data(), dm.y.data(), e.data());
    // COBRA-DD-ACCUM: RSS/TSS/mean are the T1 "closed-form single-pass
    // arithmetic" tier, whose whole justification is that compensated
    // accumulation makes 1e-12 reachable. Plain `+=` measured 3.87e-14 relative
    // on a wide-dynamic-range residual vector at n=1e6 — inside T1 by only ~26x,
    // with a bound that grows in n. la::sumsq/sum accumulate in double-double.
    double ssr = la::sumsq(e.data(), n);
    double ybar = la::sum(dm.y.data(), n) / static_cast<double>(n);
    double tss = la::sumsq_dev(dm.y.data(), n, ybar);
    r.ssr = ssr;
    r.tss = tss;
    r.r2 = (tss > 0) ? 1.0 - ssr / tss : 0.0;
    for (int intercept = 0; intercept < k; ++intercept) {
        if (names[intercept] != "(intercept)" && names[intercept] != "_cons") continue;
        std::vector<double> centered_fit(static_cast<size_t>(n), 0.0);
        for (int j = 0; j < k; ++j) {
            if (j == intercept) continue;
            const double* x = X + static_cast<int64_t>(j) * n;
            const double mean = la::sum(x, n) / static_cast<double>(n);
            for (int64_t i = 0; i < n; ++i) centered_fit[i] += (x[i] - mean) * r.beta[j];
        }
        std::vector<double> centered_y(static_cast<size_t>(n));
        for (int64_t i = 0; i < n; ++i) centered_y[i] = dm.y[i] - ybar;
        double mss = la::dot(centered_fit.data(), centered_y.data(), n);
        if (k == 2) {
            const double* x = X + static_cast<int64_t>(1 - intercept) * n;
            const double xbar = la::sum(x, n) / static_cast<double>(n);
            std::vector<double> centered_x(static_cast<size_t>(n));
            for (int64_t i = 0; i < n; ++i) centered_x[i] = x[i] - xbar;
            const double sxx = la::sumsq(centered_x.data(), n);
            const double sxy = la::dot(centered_x.data(), centered_y.data(), n);
            mss = sxx > 0.0 ? sxy * (sxy / sxx) : 0.0;
        }
        r.r2 = mss + ssr > 0.0 ? mss / (mss + ssr) : 0.0;
        break;
    }
    r.sigma2 = ssr / static_cast<double>(n - k);

    // classic SE: sigma2 * diag(bread)
    r.se_classic.resize(k);
    for (int j = 0; j < k; ++j) r.se_classic[j] = std::sqrt(r.sigma2 * bread[j + j * k]);

    // HC1: meat = sum_i e_i^2 x_i x_i' = W'W with W = X scaled rowwise by e_i.
    std::vector<double> W(static_cast<size_t>(n) * k);
    for (int j = 0; j < k; ++j) {
        const double* xj = X + static_cast<int64_t>(j) * n;
        double* wj = W.data() + static_cast<int64_t>(j) * n;
        for (int64_t i = 0; i < n; ++i) wj[i] = xj[i] * e[i];
    }
    std::vector<double> meat(static_cast<size_t>(k) * k);
    la::xtx(W.data(), n, k, meat.data());
    std::vector<double> V(static_cast<size_t>(k) * k);
    double hc1 = static_cast<double>(n) / static_cast<double>(n - k);
    la::sandwich(bread.data(), meat.data(), k, hc1, V.data());
    r.se_hc1.resize(k);
    for (int j = 0; j < k; ++j) r.se_hc1[j] = std::sqrt(V[j + j * k]);

    return r;
}

}  // namespace cecon
