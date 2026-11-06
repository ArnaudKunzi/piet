// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// cecon — time-series postestimation diagnostics on OLS residuals (the data is
// assumed already in time order, contiguous / no gaps). Replicates Stata's
// estat dwatson / durbinalt / bgodfrey / archlm.
//   dwatson  : Durbin-Watson d = Σ(e_t-e_{t-1})² / Σe_t²
//   durbinalt: Wald F-test that lag-residual coefs are 0 in the aux
//              regress e ~ L1..Lp.e + X ; chi2 = F·p (iid s²) or HC1+small
//   bgodfrey : Breusch-Godfrey LM = N·R² of the same aux regression
//   archlm   : Engle ARCH LM = (N-p)·R² of e² ~ L1..Lp.e²
// Parity notes: durbinalt/bgodfrey zero-fill the leading p lagged residuals and
// keep N = full sample (nomiss0 drops them instead); archlm never zero-fills and
// loses its first p rows. k = #params of the original regress (incl. constant).
#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

#include "core/linalg.h"
#include "estimators/diagtests.h"  // ols_fit_rss
#include "estimators/posthest.h"   // chi2_pvalue, f_pvalue

namespace cecon {

// OLS residuals of y on X (col-major n*k, intercept included).
inline bool ts_ols_residuals(const double* X, int64_t n, int k, const double* y,
                             std::vector<double>& resid) {
    std::vector<double> beta;
    double rss;
    if (!ols_fit_rss(X, n, k, y, beta, rss)) return false;
    resid.resize(n);
    for (int64_t i = 0; i < n; ++i) {
        double f = 0;
        for (int p = 0; p < k; ++p) f += X[static_cast<int64_t>(p) * n + i] * beta[p];
        resid[i] = y[i] - f;
    }
    return true;
}

struct DwResult {
    double dw = 0;
    int64_t n = 0;
    int k = 0;
    bool ok = false;
};

// Durbin-Watson d statistic (no p-value; consult d_L/d_U bounds).
inline DwResult durbin_watson(const double* X, int64_t n, int k, const double* y) {
    DwResult r;
    std::vector<double> e;
    if (!ts_ols_residuals(X, n, k, y, e)) return r;
    double num = 0, den = 0;
    for (int64_t i = 0; i < n; ++i) {
        den += e[i] * e[i];
        if (i > 0) { double d = e[i] - e[i - 1]; num += d * d; }
    }
    r.dw = (den > 0) ? num / den : 0.0;
    r.n = n;
    r.k = k;
    r.ok = true;
    return r;
}

// One per-lag result for durbinalt / bgodfrey / archlm.
struct SerialResult {
    int lag = 0;
    double chi2 = 0;   // chi2 form (durbinalt: F·p; bgodfrey/archlm: N·R²)
    double F = 0;      // small/F form (durbinalt/bgodfrey)
    int df = 0;        // = lag
    double df_r = 0;   // F-denominator df (small only)
    double p = 1;      // p-value (matching the requested form)
    bool ok = false;
};

// Internal: least-squares fit returning beta, (A'A)^-1, residuals and RSS.
struct TsAuxFit {
    std::vector<double> beta;   // length kaux
    std::vector<double> AtA_inv;  // kaux*kaux
    std::vector<double> resid;  // length n
    double rss = 0;
    bool ok = false;
};
inline TsAuxFit ts_aux_fit(const std::vector<double>& A, int64_t n, int kaux,
                           const std::vector<double>& dep) {
    TsAuxFit f;
    std::vector<double> AtA(static_cast<size_t>(kaux) * kaux), Atb(kaux);
    la::xtx(A.data(), n, kaux, AtA.data());
    la::xtv(A.data(), n, kaux, dep.data(), Atb.data());
    if (la::spd_inverse(AtA.data(), kaux) != 0) return f;
    f.AtA_inv = AtA;
    f.beta.resize(kaux);
    la::matvec(AtA.data(), kaux, Atb.data(), f.beta.data());
    f.resid.resize(n);
    f.rss = 0;
    for (int64_t i = 0; i < n; ++i) {
        double r = dep[i];
        for (int p = 0; p < kaux; ++p) r -= A[static_cast<int64_t>(p) * n + i] * f.beta[p];
        f.resid[i] = r;
        f.rss += r * r;
    }
    f.ok = true;
    return f;
}

// Build the durbinalt/bgodfrey auxiliary design [L1..Lp.e | X], with leading
// lagged-residual values zero-filled (nomiss0=false) or the first p rows dropped
// (nomiss0=true). Returns the design A, dep vector, row count m and kaux.
inline bool build_serial_aux(const std::vector<double>& e, const double* X, int64_t n, int k,
                             int lag, bool nomiss0, std::vector<double>& A,
                             std::vector<double>& dep, int64_t& m, int& kaux) {
    kaux = lag + k;
    int64_t start = nomiss0 ? lag : 0;
    m = n - start;
    if (m <= kaux) return false;
    A.assign(static_cast<size_t>(m) * kaux, 0.0);
    dep.assign(static_cast<size_t>(m), 0.0);
    for (int64_t i = start; i < n; ++i) dep[i - start] = e[i];
    for (int j = 1; j <= lag; ++j) {
        double* col = A.data() + static_cast<int64_t>(j - 1) * m;
        for (int64_t i = start; i < n; ++i)
            col[i - start] = (i >= j) ? e[i - j] : 0.0;  // zero-fill leading
    }
    for (int c = 0; c < k; ++c) {
        double* col = A.data() + static_cast<int64_t>(lag + c) * m;
        for (int64_t i = start; i < n; ++i) col[i - start] = X[static_cast<int64_t>(c) * n + i];
    }
    return true;
}

// Durbin's alternative test for one lag. robust => HC1 sandwich and small=true.
inline SerialResult durbinalt_one(const std::vector<double>& e, const double* X, int64_t n,
                                  int k, int lag, bool small, bool robust, bool nomiss0) {
    SerialResult r;
    r.lag = lag;
    r.df = lag;
    if (robust) small = true;
    std::vector<double> A, dep;
    int64_t m;
    int kaux;
    if (!build_serial_aux(e, X, n, k, lag, nomiss0, A, dep, m, kaux)) return r;
    TsAuxFit f = ts_aux_fit(A, m, kaux, dep);
    if (!f.ok) return r;
    r.df_r = static_cast<double>(m - kaux);
    // covariance of the aux coefficients
    std::vector<double> V(static_cast<size_t>(kaux) * kaux);
    if (!robust) {
        double s2 = (r.df_r > 0) ? f.rss / r.df_r : 0.0;  // regress small-sample s^2
        for (size_t i = 0; i < V.size(); ++i) V[i] = s2 * f.AtA_inv[i];
    } else {
        std::vector<double> meat(static_cast<size_t>(kaux) * kaux, 0.0);
        for (int64_t i = 0; i < m; ++i) {
            double u2 = f.resid[i] * f.resid[i];
            for (int c = 0; c < kaux; ++c) {
                double xc = A[static_cast<int64_t>(c) * m + i];
                for (int d = 0; d < kaux; ++d)
                    meat[static_cast<size_t>(c) * kaux + d] += u2 * xc * A[static_cast<int64_t>(d) * m + i];
            }
        }
        std::vector<double> tmp(static_cast<size_t>(kaux) * kaux);
        double hc1 = (m - kaux > 0) ? static_cast<double>(m) / (m - kaux) : 1.0;  // HC1
        la::sandwich(f.AtA_inv.data(), meat.data(), kaux, hc1, V.data(), tmp.data());
    }
    // Wald on the first `lag` coefficients: g'(Vgg)^-1 g
    std::vector<double> Vgg(static_cast<size_t>(lag) * lag), g(lag);
    for (int a = 0; a < lag; ++a) {
        g[a] = f.beta[a];
        for (int b = 0; b < lag; ++b) Vgg[static_cast<size_t>(a) * lag + b] = V[static_cast<size_t>(a) * kaux + b];
    }
    if (la::spd_inverse(Vgg.data(), lag) != 0) return r;
    std::vector<double> Vg(lag);
    la::matvec(Vgg.data(), lag, g.data(), Vg.data());
    double wald = 0;
    for (int a = 0; a < lag; ++a) wald += g[a] * Vg[a];
    r.F = wald / lag;
    r.chi2 = wald;  // = F * lag
    if (small) {
        r.p = (r.df_r > 0) ? f_pvalue(r.F, lag, r.df_r) : 1.0;
    } else {
        r.p = chi2_pvalue(r.chi2, lag);
    }
    r.ok = true;
    return r;
}

// Breusch-Godfrey LM test for one lag: LM = N·R² of the same aux regression.
inline SerialResult bgodfrey_one(const std::vector<double>& e, const double* X, int64_t n,
                                 int k, int lag, bool small, bool nomiss0) {
    SerialResult r;
    r.lag = lag;
    r.df = lag;
    std::vector<double> A, dep;
    int64_t m;
    int kaux;
    if (!build_serial_aux(e, X, n, k, lag, nomiss0, A, dep, m, kaux)) return r;
    TsAuxFit f = ts_aux_fit(A, m, kaux, dep);
    if (!f.ok) return r;
    double mean = 0; for (double d : dep) mean += d; mean /= static_cast<double>(m);
    double tss = 0; for (double d : dep) { double t = d - mean; tss += t * t; }
    double r2 = (tss > 0) ? 1.0 - f.rss / tss : 0.0;
    r.df_r = static_cast<double>(m - kaux);
    r.chi2 = static_cast<double>(m) * r2;     // N·R²
    r.F = r.chi2 / lag;                        // small F form = chi2/p
    if (small) {
        r.p = (r.df_r > 0) ? f_pvalue(r.F, lag, r.df_r) : 1.0;
    } else {
        r.p = chi2_pvalue(r.chi2, lag);
    }
    r.ok = true;
    return r;
}

// Engle ARCH LM for one lag: regress e² on [const | L1..Lp.e²], drop first p rows,
// statistic = (N-p)·R². chi2 only.
inline SerialResult archlm_one(const std::vector<double>& e, int64_t n, int lag) {
    SerialResult r;
    r.lag = lag;
    r.df = lag;
    int64_t m = n - lag;
    int kaux = lag + 1;  // lags + constant
    if (m <= kaux) return r;
    std::vector<double> e2(n);
    for (int64_t i = 0; i < n; ++i) e2[i] = e[i] * e[i];
    std::vector<double> A(static_cast<size_t>(m) * kaux, 0.0), dep(m);
    for (int64_t i = lag; i < n; ++i) dep[i - lag] = e2[i];
    for (int j = 1; j <= lag; ++j) {
        double* col = A.data() + static_cast<int64_t>(j - 1) * m;
        for (int64_t i = lag; i < n; ++i) col[i - lag] = e2[i - j];
    }
    double* cc = A.data() + static_cast<int64_t>(lag) * m;  // constant column
    for (int64_t i = 0; i < m; ++i) cc[i] = 1.0;
    TsAuxFit f = ts_aux_fit(A, m, kaux, dep);
    if (!f.ok) return r;
    double mean = 0; for (double d : dep) mean += d; mean /= static_cast<double>(m);
    double tss = 0; for (double d : dep) { double t = d - mean; tss += t * t; }
    double r2 = (tss > 0) ? 1.0 - f.rss / tss : 0.0;
    r.chi2 = static_cast<double>(m) * r2;
    r.p = chi2_pvalue(r.chi2, lag);
    r.ok = true;
    return r;
}

struct WnResult {
    double stat = 0;  // Ljung-Box Q
    int df = 0;       // number of lags
    double p = 1;
    bool ok = false;
    // Why the test could not be formed (valid only when !ok). TOO_FEW = bad lags or
    // n<=lags (genuine sample-size reject); ZERO_VAR = c0<=0 constant series (ρ_k
    // undefined). The call site surfaces a distinct message per cause — one string
    // for all causes misdirects the user (BUG-131).
    enum Fail { NONE = 0, TOO_FEW, ZERO_VAR } fail = TOO_FEW;
};

// Ljung-Box portmanteau (Q) white-noise test (Stata wntestq). `v` holds the series
// in TIME ORDER over `npos` positions; interior missings (NaN) are declared tsset
// gaps, NOT compacted away (Stata does not splice across a gap). ρ_k is the sample
// autocorrelation demeaned by the non-missing mean; lag-k products are formed only
// between two non-missing observations exactly k periods apart, so a gap breaks the
// pairs that straddle it. n = number of non-missing obs, and
// Q = n(n+2) Σ_{k=1}^{p} ρ_k²/(n-k) ~ χ²(p). Default lags = min(n/2-2, 40).
// With no missings this is byte-for-byte the plain dense computation (n == npos).
inline WnResult wntestq(const double* v, int64_t npos, int lags) {
    WnResult r;
    int64_t n = 0;
    for (int64_t i = 0; i < npos; ++i) if (!std::isnan(v[i])) ++n;
    if (lags <= 0) lags = static_cast<int>(std::min<int64_t>(n / 2 - 2, 40));
    if (lags < 1 || n <= lags) { r.fail = WnResult::TOO_FEW; return r; }
    double mean = 0; for (int64_t i = 0; i < npos; ++i) if (!std::isnan(v[i])) mean += v[i];
    mean /= static_cast<double>(n);
    double c0 = 0; for (int64_t i = 0; i < npos; ++i) if (!std::isnan(v[i])) { double d = v[i] - mean; c0 += d * d; }
    if (c0 <= 0) { r.fail = WnResult::ZERO_VAR; return r; }
    double q = 0;
    for (int k = 1; k <= lags; ++k) {
        double ck = 0;
        for (int64_t t = k; t < npos; ++t) {
            if (std::isnan(v[t]) || std::isnan(v[t - k])) continue;  // gap breaks the lag-k pair
            ck += (v[t] - mean) * (v[t - k] - mean);
        }
        double rho = ck / c0;
        q += rho * rho / static_cast<double>(n - k);
    }
    r.stat = static_cast<double>(n) * (n + 2) * q;
    r.df = lags;
    r.p = chi2_pvalue(r.stat, lags);
    r.ok = true;
    return r;
}

}  // namespace cecon
