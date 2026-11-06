// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "miimpute.h"

#include <algorithm>
#include <cmath>

#include "core/linalg.h"
#include "core/rng_normal.h"
#include "estimators/posthest.h"

namespace cecon {

namespace {

inline bool observed(double v) { return !std::isnan(v); }

// Lower Cholesky L (column-major, L[i + j*k], i >= j) of a symmetric PD A.
// Returns false when A is not positive definite — (X'X)^-1 always is when the
// design has full rank, so a false here is the collinearity report.
bool cholesky_lower(const double* A, int k, std::vector<double>* L) {
    L->assign(static_cast<size_t>(k) * static_cast<size_t>(k), 0.0);
    double* l = L->data();
    for (int j = 0; j < k; ++j) {
        double d = A[static_cast<size_t>(j) + static_cast<size_t>(j) * k];
        for (int p = 0; p < j; ++p) {
            const double v = l[static_cast<size_t>(j) + static_cast<size_t>(p) * k];
            d -= v * v;
        }
        if (!(d > 0.0)) return false;
        const double djj = std::sqrt(d);
        l[static_cast<size_t>(j) + static_cast<size_t>(j) * k] = djj;
        for (int i = j + 1; i < k; ++i) {
            double s = A[static_cast<size_t>(i) + static_cast<size_t>(j) * k];
            for (int p = 0; p < j; ++p)
                s -= l[static_cast<size_t>(i) + static_cast<size_t>(p) * k] *
                     l[static_cast<size_t>(j) + static_cast<size_t>(p) * k];
            l[static_cast<size_t>(i) + static_cast<size_t>(j) * k] = s / djj;
        }
    }
    return true;
}

}  // namespace

MiSample mi_classify(const double* y, const std::vector<const double*>& xs, int64_t n) {
    MiSample s;
    s.n_total = n;
    for (int64_t i = 0; i < n; ++i) {
        bool x_ok = true;
        for (const double* c : xs)
            if (!observed(c[i])) { x_ok = false; break; }
        if (observed(y[i])) {
            ++s.n_complete;
            if (x_ok) s.fit_rows.push_back(i);
        } else {
            ++s.n_incomplete;
            if (x_ok) { ++s.n_imputable; s.impute_rows.push_back(i); }
        }
    }
    return s;
}

// ---------------------------------------------------------------------------
// One imputation's draws — the whole reason the counter layout exists
// ---------------------------------------------------------------------------
bool mi_impute_draw(const MiImputeResult& fit, const double* Ximp, int64_t n_imp, int m,
                    uint64_t seed, std::vector<double>* out_values, double* out_sigma_star,
                    std::vector<double>* out_beta_star) {
    if (!fit.ok || fit.k <= 0 || fit.df_resid <= 0 || m < 1) return false;
    const int k = fit.k;
    const uint64_t stream = static_cast<uint64_t>(m);

    // counter 0 — sigma*^2 = RSS / chi2(nu). The chi-square, not the normal, is
    // what makes this a PROPER imputation: sigma is redrawn per imputation from
    // its own posterior instead of being frozen at the fitted Root MSE.
    const double nu = static_cast<double>(fit.df_resid);
    const double chi = chi2_draw(seed, stream, 0, nu);
    if (!(chi > 0.0) || !std::isfinite(chi)) return false;
    const double sigma_star = std::sqrt(fit.rss / chi);

    // counters 1..k — beta* = b_hat + sigma* L u, L L' = (X'X)^-1.
    std::vector<double> L;
    if (!cholesky_lower(fit.xtx_inv.data(), k, &L)) return false;
    std::vector<double> u(static_cast<size_t>(k));
    for (int j = 0; j < k; ++j)
        u[static_cast<size_t>(j)] = normal_draw(seed, stream, static_cast<uint64_t>(j) + 1);
    std::vector<double> bstar(static_cast<size_t>(k));
    for (int i = 0; i < k; ++i) {
        double s = 0.0;
        for (int j = 0; j <= i; ++j)
            s += L[static_cast<size_t>(i) + static_cast<size_t>(j) * k] * u[static_cast<size_t>(j)];
        bstar[static_cast<size_t>(i)] = fit.beta[static_cast<size_t>(i)] + sigma_star * s;
    }

    // counters k+1+j — y*_j = x_j' beta* + sigma* eps_j.
    std::vector<double> vals(static_cast<size_t>(n_imp));
    for (int64_t r = 0; r < n_imp; ++r) {
        double xb = 0.0;
        for (int c = 0; c < k; ++c)
            xb += Ximp[static_cast<int64_t>(c) * n_imp + r] * bstar[static_cast<size_t>(c)];
        const double eps =
            normal_draw(seed, stream, static_cast<uint64_t>(k) + 1 + static_cast<uint64_t>(r));
        vals[static_cast<size_t>(r)] = xb + sigma_star * eps;
    }

    if (out_values) *out_values = std::move(vals);
    if (out_sigma_star) *out_sigma_star = sigma_star;
    if (out_beta_star) *out_beta_star = std::move(bstar);
    return true;
}

// ---------------------------------------------------------------------------
// Fit the complete cases, then draw `add` imputations
// ---------------------------------------------------------------------------
MiImputeResult mi_impute_regress(const double* Xfit, const double* yfit, int64_t n_fit, int k,
                                 const double* Ximp, int64_t n_imp, int add, uint64_t seed) {
    MiImputeResult r;
    r.k = k;
    r.n_fit = n_fit;
    r.add = add;
    if (k <= 0 || n_fit <= k) {
        r.err = "too few complete cases to fit the imputation model";
        return r;
    }
    if (add < 1) {
        r.err = "add() must be at least 1";
        return r;
    }

    // OLS by normal equations: (X'X) b = X'y, and (X'X)^-1 is kept because it is
    // both the SE source and the SHAPE of the beta* draw.
    std::vector<double> A(static_cast<size_t>(k) * static_cast<size_t>(k));
    la::xtx(Xfit, n_fit, k, A.data());
    std::vector<double> Ainv = A;
    if (la::spd_inverse(Ainv.data(), k) != 0) {
        r.err = "the imputation model's X'X is singular (collinear predictors)";
        return r;
    }
    std::vector<double> Xty(static_cast<size_t>(k));
    la::xtv(Xfit, n_fit, k, yfit, Xty.data());
    r.beta.assign(static_cast<size_t>(k), 0.0);
    for (int i = 0; i < k; ++i) {
        double s = 0.0;
        for (int j = 0; j < k; ++j)
            s += Ainv[static_cast<size_t>(i) + static_cast<size_t>(j) * k] * Xty[static_cast<size_t>(j)];
        r.beta[static_cast<size_t>(i)] = s;
    }
    r.xtx_inv = Ainv;

    double ybar = 0.0;
    for (int64_t i = 0; i < n_fit; ++i) ybar += yfit[i];
    ybar /= static_cast<double>(n_fit);
    r.rss = 0.0;
    r.tss = 0.0;
    for (int64_t i = 0; i < n_fit; ++i) {
        double xb = 0.0;
        for (int c = 0; c < k; ++c) xb += Xfit[static_cast<int64_t>(c) * n_fit + i] * r.beta[static_cast<size_t>(c)];
        const double e = yfit[i] - xb;
        r.rss += e * e;
        const double dy = yfit[i] - ybar;
        r.tss += dy * dy;
    }
    r.mss = r.tss - r.rss;
    r.df_model = k - 1;   // the intercept is not a model degree of freedom
    r.df_resid = n_fit - k;
    const double s2 = r.rss / static_cast<double>(r.df_resid);
    r.rmse = std::sqrt(s2);
    r.r2 = (r.tss > 0.0) ? r.mss / r.tss : 0.0;
    r.r2_adj = 1.0 - (1.0 - r.r2) * static_cast<double>(n_fit - 1) / static_cast<double>(r.df_resid);
    r.f_stat = (r.df_model > 0) ? (r.mss / static_cast<double>(r.df_model)) / s2 : 0.0;
    r.p_f = (r.df_model > 0) ? f_pvalue(r.f_stat, static_cast<double>(r.df_model),
                                        static_cast<double>(r.df_resid))
                             : 1.0;
    r.se.assign(static_cast<size_t>(k), 0.0);
    for (int j = 0; j < k; ++j)
        r.se[static_cast<size_t>(j)] =
            std::sqrt(std::max(0.0, s2 * Ainv[static_cast<size_t>(j) + static_cast<size_t>(j) * k]));
    r.ok = true;

    // The draws. Each imputation is independent of the others by construction —
    // it reads only its own (stream = m) counters — so this loop is a convenience,
    // not a dependency chain.
    r.n_imputed = n_imp;
    r.values.assign(static_cast<size_t>(add) * static_cast<size_t>(n_imp), 0.0);
    r.sigma_star.assign(static_cast<size_t>(add), 0.0);
    r.beta_star.assign(static_cast<size_t>(add) * static_cast<size_t>(k), 0.0);
    for (int m = 1; m <= add; ++m) {
        std::vector<double> vals, bstar;
        double sig = 0.0;
        if (!mi_impute_draw(r, Ximp, n_imp, m, seed, &vals, &sig, &bstar)) {
            r.ok = false;
            r.err = "the imputation draw failed (non-PD (X'X)^-1 or a degenerate chi-square draw)";
            return r;
        }
        r.sigma_star[static_cast<size_t>(m - 1)] = sig;
        for (int c = 0; c < k; ++c)
            r.beta_star[static_cast<size_t>(m - 1) * static_cast<size_t>(k) + static_cast<size_t>(c)] =
                bstar[static_cast<size_t>(c)];
        for (int64_t j = 0; j < n_imp; ++j)
            r.values[static_cast<size_t>(m - 1) * static_cast<size_t>(n_imp) + static_cast<size_t>(j)] =
                vals[static_cast<size_t>(j)];
    }
    return r;
}

}  // namespace cecon
