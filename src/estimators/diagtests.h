// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// cecon — post-OLS specification diagnostics: Breusch-Pagan / Cook-Weisberg
// heteroskedasticity test (estat hettest) and Ramsey RESET (estat ovtest).
// Parity notes: sigma^2 = RSS/n (OLS divisor n, NOT n-k); default hettest is the
// Cook-Weisberg "normal" form S = (model SS of aux on e^2/sigma^2)/2 ~ chi2(m);
// RESET normalizes the fitted values to [0,1] BEFORE taking powers 2,3,4.
#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

#include "core/linalg.h"

namespace cecon {

// OLS via normal equations on a col-major n*k design (intercept included). Returns
// RSS and fills beta (length k). Returns false if X'X is singular.
inline bool ols_fit_rss(const double* X, int64_t n, int k, const double* y,
                        std::vector<double>& beta, double& rss) {
    std::vector<double> XtX(static_cast<size_t>(k) * k), Xty(k);
    la::xtx(X, n, k, XtX.data());
    la::xtv(X, n, k, y, Xty.data());
    if (la::spd_inverse(XtX.data(), k) != 0) return false;
    beta.resize(k);
    la::matvec(XtX.data(), k, Xty.data(), beta.data());
    rss = 0;
    for (int64_t i = 0; i < n; ++i) {
        double e = y[i];
        for (int p = 0; p < k; ++p) e -= X[static_cast<int64_t>(p) * n + i] * beta[p];
        rss += e * e;
    }
    return true;
}

struct BpResult {
    double chi2_normal = 0, p_normal = 1;  // Cook-Weisberg (default)
    double chi2_iid = 0, p_iid = 1;        // Koenker N*R^2
    double F = 0, p_F = 1;                  // Wooldridge F-form
    int df = 0;                            // = m (# of z columns)
    double df_r = 0;
    bool ok = false;
};

enum class BpZ { Fitted, Rhs, Custom };

// Breusch-Pagan / Cook-Weisberg. Xmain (n*kmain incl intercept), y. The aux test
// regresses e^2/sigma2 on [1, Z]. Z = fitted values (Fitted), the non-intercept
// regressors (Rhs), or a supplied matrix customZ (n*mcz, Custom). intercept_col is
// the index of the constant column in Xmain (so Rhs can skip it).
inline BpResult breusch_pagan(const double* Xmain, int64_t n, int kmain, const double* y,
                              BpZ zmode, const double* customZ, int mcz, int intercept_col) {
    BpResult r;
    std::vector<double> beta;
    double rss_main;
    if (!ols_fit_rss(Xmain, n, kmain, y, beta, rss_main)) return r;
    double sigma2 = rss_main / static_cast<double>(n);
    if (sigma2 <= 0) return r;

    // dependent variable of the aux regression: e^2 / sigma2
    std::vector<double> dep(n), yhat(n, 0.0);
    for (int64_t i = 0; i < n; ++i) {
        double fit = 0;
        for (int p = 0; p < kmain; ++p) fit += Xmain[static_cast<int64_t>(p) * n + i] * beta[p];
        yhat[i] = fit;
        double e = y[i] - fit;
        dep[i] = e * e / sigma2;
    }

    // build z columns
    int m;
    std::vector<double> Z;  // n*m
    if (zmode == BpZ::Fitted) {
        m = 1; Z = yhat;
    } else if (zmode == BpZ::Rhs) {
        m = kmain - 1;
        Z.assign(static_cast<size_t>(n) * m, 0.0);
        int c = 0;
        for (int p = 0; p < kmain; ++p) {
            if (p == intercept_col) continue;
            std::copy(Xmain + static_cast<int64_t>(p) * n, Xmain + static_cast<int64_t>(p + 1) * n,
                      Z.data() + static_cast<int64_t>(c) * n);
            ++c;
        }
    } else {
        m = mcz; Z.assign(customZ, customZ + static_cast<size_t>(n) * mcz);
    }
    if (m < 1) return r;

    // aux design [Z | 1]
    int ka = m + 1;
    std::vector<double> A(static_cast<size_t>(n) * ka);
    std::copy(Z.begin(), Z.end(), A.begin());
    for (int64_t i = 0; i < n; ++i) A[static_cast<int64_t>(m) * n + i] = 1.0;

    std::vector<double> bA;
    double rss_aux;
    if (!ols_fit_rss(A.data(), n, ka, dep.data(), bA, rss_aux)) return r;
    double dmean = 0; for (int64_t i = 0; i < n; ++i) dmean += dep[i]; dmean /= n;
    double tss = 0; for (int64_t i = 0; i < n; ++i) { double t = dep[i] - dmean; tss += t * t; }
    double model_ss = tss - rss_aux;
    double r2 = (tss > 0) ? model_ss / tss : 0.0;

    r.df = m;
    r.df_r = static_cast<double>(n - m - 1);
    r.chi2_normal = model_ss / 2.0;
    r.chi2_iid = n * r2;
    r.F = (r.df_r > 0) ? (model_ss / m) / (rss_aux / r.df_r) : 0.0;
    r.ok = true;
    return r;
}

struct WhiteResult {
    double chi2 = 0, p = 1;
    int df = 0;
    bool ok = false;
};

// White's general test for heteroskedasticity (estat imtest, white): n*R^2 from
// regressing the squared OLS residuals on the regressors, their squares, and all
// pairwise cross-products. Aux columns that are collinear (e.g. a 0/1 dummy
// squared = itself) are detected and dropped by a rank-revealing modified
// Gram-Schmidt; df = number of surviving non-constant aux columns. R^2 is basis-
// independent, so it matches Stata regardless of which collinear column is omitted.
inline WhiteResult white_test(const double* Xmain, int64_t n, int kmain, const double* y,
                              int intercept_col) {
    WhiteResult r;
    std::vector<double> beta;
    double rss_main;
    if (!ols_fit_rss(Xmain, n, kmain, y, beta, rss_main)) return r;
    std::vector<double> dep(n);  // squared residuals
    for (int64_t i = 0; i < n; ++i) {
        double e = y[i];
        for (int p = 0; p < kmain; ++p) e -= Xmain[static_cast<int64_t>(p) * n + i] * beta[p];
        dep[i] = e * e;
    }
    // non-intercept regressors (the "X" of White's test)
    std::vector<const double*> xs;
    for (int p = 0; p < kmain; ++p)
        if (p != intercept_col) xs.push_back(Xmain + static_cast<int64_t>(p) * n);
    int pnum = static_cast<int>(xs.size());
    // candidate aux columns: levels, then cross products incl. squares (i<=j)
    std::vector<std::vector<double>> cand;
    for (int j = 0; j < pnum; ++j) cand.emplace_back(xs[j], xs[j] + n);
    for (int i = 0; i < pnum; ++i)
        for (int j = i; j < pnum; ++j) {
            std::vector<double> col(n);
            for (int64_t t = 0; t < n; ++t) col[t] = xs[i][t] * xs[j][t];
            cand.push_back(std::move(col));
        }
    // modified Gram-Schmidt with one reorthogonalization pass; drops collinear cols
    std::vector<std::vector<double>> Q;
    auto add_dir = [&](std::vector<double> v) -> bool {
        double orig = 0; for (double x : v) orig += x * x; orig = std::sqrt(orig);
        if (orig <= 0) return false;
        for (int pass = 0; pass < 2; ++pass)
            for (auto& q : Q) {
                double d = 0; for (int64_t t = 0; t < n; ++t) d += q[t] * v[t];
                for (int64_t t = 0; t < n; ++t) v[t] -= d * q[t];
            }
        double nrm = 0; for (double x : v) nrm += x * x; nrm = std::sqrt(nrm);
        if (nrm / orig < 1e-7) return false;  // collinear with the accepted set
        for (auto& x : v) x /= nrm;
        Q.push_back(std::move(v));
        return true;
    };
    add_dir(std::vector<double>(n, 1.0));  // constant direction (index 0)
    int dfnc = 0;
    for (auto& c : cand) if (add_dir(std::move(c))) ++dfnc;
    // centered R^2: model SS = sum over non-constant directions of (q . dep)^2
    double mean = 0; for (double d : dep) mean += d; mean /= n;
    double tss = 0; for (double d : dep) { double t = d - mean; tss += t * t; }
    double model_ss = 0;
    for (size_t qi = 1; qi < Q.size(); ++qi) {
        double pr = 0; for (int64_t t = 0; t < n; ++t) pr += Q[qi][t] * dep[t];
        model_ss += pr * pr;
    }
    double r2 = (tss > 0) ? model_ss / tss : 0.0;
    r.df = dfnc;
    r.chi2 = static_cast<double>(n) * r2;
    r.ok = true;
    return r;
}

struct ResetResult {
    double F = 0, p_F = 1;
    int df_m = 0;
    double df_r = 0;
    bool ok = false;
};

// Ramsey RESET (estat ovtest, default): add powers 2,3,4 of the [0,1]-normalized
// fitted values to the model and F-test their joint significance.
inline ResetResult ramsey_reset(const double* Xmain, int64_t n, int kmain, const double* y) {
    ResetResult r;
    std::vector<double> beta;
    double rss_main;
    if (!ols_fit_rss(Xmain, n, kmain, y, beta, rss_main)) return r;
    std::vector<double> yhat(n);
    double mn = 1e300, mx = -1e300;
    for (int64_t i = 0; i < n; ++i) {
        double fit = 0;
        for (int p = 0; p < kmain; ++p) fit += Xmain[static_cast<int64_t>(p) * n + i] * beta[p];
        yhat[i] = fit; mn = std::min(mn, fit); mx = std::max(mx, fit);
    }
    if (mx <= mn) return r;  // degenerate

    const int npow = 3;  // powers 2,3,4
    int ka = kmain + npow;
    std::vector<double> A(static_cast<size_t>(n) * ka);
    std::copy(Xmain, Xmain + static_cast<size_t>(n) * kmain, A.begin());
    // estat ovtest normalizes yhat to [0,1] then stores the powers as `gen float`
    // (single precision) before the augmented regression. Match that truncation
    // bit-for-bit: compute the product in double, then round to float.
    for (int64_t i = 0; i < n; ++i) {
        double yn = (yhat[i] - mn) / (mx - mn);
        A[static_cast<int64_t>(kmain) * n + i] = static_cast<float>(yn * yn);
        A[static_cast<int64_t>(kmain + 1) * n + i] = static_cast<float>(yn * yn * yn);
        A[static_cast<int64_t>(kmain + 2) * n + i] = static_cast<float>(yn * yn * yn * yn);
    }
    std::vector<double> bA;
    double rss_aug;
    if (!ols_fit_rss(A.data(), n, ka, y, bA, rss_aug)) return r;
    r.df_m = npow;
    r.df_r = static_cast<double>(n - ka);
    r.F = (r.df_r > 0 && rss_aug > 0) ? ((rss_main - rss_aug) / npow) / (rss_aug / r.df_r) : 0.0;
    r.ok = true;
    return r;
}

}  // namespace cecon
