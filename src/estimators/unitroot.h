// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// cecon — unit-root tests: Augmented Dickey-Fuller (dfuller) and Phillips-Perron
// (pperron). Single time series, time-ordered; lags/diffs built via ts_apply so
// gaps/boundaries drop the right rows. Fuller (1996) critical values (interpolated
// by sample size) and MacKinnon (1994) approximate p-values, matching Stata's
// dfuller/pperron critical-value and p-value tables.
//   case 1 = noconstant, 2 = constant (default), 3 = drift (standard-t), 4 = trend.
#pragma once
#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

#include "core/linalg.h"
#include "data/ts_operators.h"
#include "estimators/posthest.h"  // t_critical, t_twosided_pvalue

namespace cecon {

namespace uroot {
inline double norm_cdf(double x) { return 0.5 * std::erfc(-x * 0.70710678118654752440); }

// Fuller (1996) DF t critical values: rows 1%/5%/10%, anchors N=25,50,100,250,500,inf.
inline void fuller_cv(int kase, double N, double& c1, double& c5, double& c10) {
    static const double T1[3][6] = {{-2.66, -2.62, -2.60, -2.58, -2.58, -2.58},
                                    {-1.95, -1.95, -1.95, -1.95, -1.95, -1.95},
                                    {-1.60, -1.61, -1.61, -1.62, -1.62, -1.62}};
    static const double T2[3][6] = {{-3.75, -3.58, -3.51, -3.46, -3.44, -3.43},
                                    {-3.00, -2.93, -2.89, -2.88, -2.87, -2.86},
                                    {-2.63, -2.60, -2.58, -2.57, -2.57, -2.57}};
    static const double T4[3][6] = {{-4.38, -4.15, -4.04, -3.99, -3.98, -3.96},
                                    {-3.60, -3.50, -3.45, -3.43, -3.42, -3.41},
                                    {-3.24, -3.18, -3.15, -3.13, -3.13, -3.12}};
    const double(*zt)[6] = (kase == 1) ? T1 : (kase == 4) ? T4 : T2;
    auto interp = [&](int r) -> double {
        if (N <= 25) return zt[r][0];
        if (N <= 50) return zt[r][0] + (N - 25) / 25 * (zt[r][1] - zt[r][0]);
        if (N <= 100) return zt[r][1] + (N - 50) / 50 * (zt[r][2] - zt[r][1]);
        if (N <= 250) return zt[r][2] + (N - 100) / 150 * (zt[r][3] - zt[r][2]);
        if (N <= 500) return zt[r][3] + (N - 250) / 250 * (zt[r][4] - zt[r][3]);
        return zt[r][5];
    };
    c1 = interp(0); c5 = interp(1); c10 = interp(2);
}

// MacKinnon (1994) approximate p-value. type: 'c' (constant) or 't' (trend).
inline double mackinnon_p(char type, double tau) {
    double g0, g1, g2, g3 = 0, mn = -1e300, mx = 1e300;
    if (type == 'c') {
        if (tau > -1.61) { mn = -9999; mx = 2.74; g0 = 1.7339; g1 = 0.93202; g2 = -0.12745; g3 = -0.010368; }
        else { mn = -18.83; mx = 1e300; g0 = 2.1659; g1 = 1.4412; g2 = 0.038269; }
    } else {  // 't'
        if (tau > -2.89) { mn = -9999; mx = 0.70; g0 = 2.5261; g1 = 0.61654; g2 = -0.37956; g3 = -0.060285; }
        else { mn = -16.18; mx = 1e300; g0 = 3.2512; g1 = 1.6047; g2 = 0.049588; }
    }
    double h = g0 + g1 * tau + g2 * tau * tau + g3 * tau * tau * tau;
    if (tau < mn) return 0.0;
    if (tau > mx) return 1.0;
    return norm_cdf(h);
}

// OLS with SEs on a col-major n*k design; returns beta, se (length k), rss, dofr.
inline bool ols_se(const std::vector<double>& X, int64_t n, int k, const std::vector<double>& y,
                   std::vector<double>& beta, std::vector<double>& se, double& rss, double& dofr) {
    std::vector<double> XtX(static_cast<size_t>(k) * k), Xty(k);
    la::xtx(X.data(), n, k, XtX.data());
    la::xtv(X.data(), n, k, y.data(), Xty.data());
    if (la::spd_inverse(XtX.data(), k) != 0) return false;
    beta.assign(k, 0.0);
    la::matvec(XtX.data(), k, Xty.data(), beta.data());
    rss = 0;
    for (int64_t i = 0; i < n; ++i) {
        double r = y[i];
        for (int p = 0; p < k; ++p) r -= X[static_cast<int64_t>(p) * n + i] * beta[p];
        rss += r * r;
    }
    dofr = static_cast<double>(n - k);
    double s2 = (dofr > 0) ? rss / dofr : 0.0;
    se.assign(k, 0.0);
    for (int c = 0; c < k; ++c) se[c] = std::sqrt(std::max(0.0, s2 * XtX[static_cast<size_t>(c) * k + c]));
    return true;
}
}  // namespace uroot

struct DfullerResult {
    double Zt = 0, p = 1, cv1 = 0, cv5 = 0, cv10 = 0;
    int64_t n = 0;
    int lags = 0, kase = 2;
    bool has_p = false;
    bool ok = false;
    // Cause of a reject (valid only when !ok). TOO_FEW = empty time / m<=k genuine
    // sample-size reject; SINGULAR = the ADF design is rank-deficient (collinear or a
    // degenerate/constant series). One "too few observations" string for both misdirects
    // the user on a constant series (BUG-131 class, A11 sweep).
    enum Fail { NONE = 0, TOO_FEW, SINGULAR } fail = TOO_FEW;
};

// kase: 1 noconstant, 2 constant (default), 3 drift, 4 trend. lags = # of ΔY lags.
inline DfullerResult dfuller_fit(const std::vector<double>& y, const std::vector<double>& time,
                                 int64_t n, int lags, int kase, double delta = 1.0) {
    DfullerResult r;
    r.lags = lags; r.kase = kase;
    if (lags < 0) lags = 0;
    auto apply = [&](const TsOpSpec& s, const std::vector<double>& in) {
        std::vector<double> out; std::string err;
        ts_apply(in.data(), n, time.data(), nullptr, delta, s, out, err);
        return out;
    };
    TsOpSpec sD; sD.ok = true; sD.d_count = 1;          // D.y
    TsOpSpec sL; sL.ok = true; sL.lag_net = 1;          // L.y
    std::vector<double> Dy = apply(sD, y);
    std::vector<double> Ly = apply(sL, y);
    std::vector<std::vector<double>> DL;                // DL(1..lags).y = L^j (D.y)
    for (int j = 1; j <= lags; ++j) { TsOpSpec s; s.ok = true; s.lag_net = j; DL.push_back(apply(s, Dy)); }
    bool wantC = (kase != 1);   // constant for cases 2,3,4
    bool wantT = (kase == 4);   // trend
    if (time.empty()) return r;
    double tmin = *std::min_element(time.begin(), time.end());

    int k = 1 + lags + (wantC ? 1 : 0) + (wantT ? 1 : 0);  // L.y + lags + const + trend
    std::vector<double> Xcols;  // collect rows that survive listwise
    std::vector<double> dep;
    std::vector<int64_t> rows;
    int64_t navail = std::min<int64_t>(n, std::min<int64_t>(static_cast<int64_t>(Dy.size()), static_cast<int64_t>(Ly.size())));
    for (int j = 0; j < lags; ++j) navail = std::min<int64_t>(navail, static_cast<int64_t>(DL[static_cast<size_t>(j)].size()));
    for (int64_t i = 0; i < navail; ++i) {
        if (std::isnan(Dy[i]) || std::isnan(Ly[i])) continue;
        bool ok = true;
        for (int j = 0; j < lags; ++j) if (std::isnan(DL[j][i])) { ok = false; break; }
        if (ok) rows.push_back(i);
    }
    int64_t m = static_cast<int64_t>(rows.size());
    if (m <= k) return r;
    std::vector<double> X(static_cast<size_t>(m) * k);
    dep.resize(m);
    for (int64_t i = 0; i < m; ++i) {
        int64_t t = rows[i];
        dep[i] = Dy[t];
        int c = 0;
        X[static_cast<int64_t>(c++) * m + i] = Ly[t];
        for (int j = 0; j < lags; ++j) X[static_cast<int64_t>(c++) * m + i] = DL[j][t];
        if (wantT) X[static_cast<int64_t>(c++) * m + i] = time[t] - tmin;
        if (wantC) X[static_cast<int64_t>(c++) * m + i] = 1.0;
    }
    std::vector<double> beta, se;
    double rss, dofr;
    if (!uroot::ols_se(X, m, k, dep, beta, se, rss, dofr)) { r.fail = DfullerResult::SINGULAR; return r; }
    r.Zt = (se[0] > 0) ? beta[0] / se[0] : 0.0;  // coefficient on L.y
    r.n = m;
    uroot::fuller_cv(kase, static_cast<double>(m), r.cv1, r.cv5, r.cv10);
    if (kase == 3) {  // drift: ordinary t-distribution, one-sided H_a: beta < 0
        r.cv1 = -t_critical(0.02, dofr);   // invttail(df,0.99)
        r.cv5 = -t_critical(0.10, dofr);   // invttail(df,0.95)
        r.cv10 = -t_critical(0.20, dofr);  // invttail(df,0.90)
        double half = t_twosided_pvalue(r.Zt, dofr) / 2.0;  // = ttail(df,|Zt|)
        r.p = (r.Zt < 0) ? half : 1.0 - half;               // lower-tail CDF at Z(t)
        r.has_p = true;
    } else if (kase == 2 || kase == 4) {
        r.p = uroot::mackinnon_p(kase == 4 ? 't' : 'c', r.Zt);
        r.has_p = true;
    }
    r.ok = true;
    return r;
}

struct PperronResult {
    double Zrho = 0, Ztau = 0, p = 1, cv1 = 0, cv5 = 0, cv10 = 0;
    int64_t n = 0;
    int lags = 0, kase = 2;
    bool ok = false;
    // Cause of a reject (valid only when !ok). TOO_FEW = empty time / T<=k+2 genuine
    // sample-size reject; SINGULAR = rank-deficient design (collinear / degenerate
    // constant series). Distinct message per cause (BUG-131 class, A11 sweep).
    enum Fail { NONE = 0, TOO_FEW, SINGULAR } fail = TOO_FEW;
};

// kase: 1 noconstant, 2 constant (default), 4 trend. lags = Newey-West bandwidth q
// (default int(4*(T/100)^(2/9))).
inline PperronResult pperron_fit(const std::vector<double>& y, const std::vector<double>& time,
                                 int64_t n, int lags, int kase, double delta = 1.0) {
    PperronResult r;
    r.kase = kase;
    TsOpSpec sL; sL.ok = true; sL.lag_net = 1;
    std::vector<double> out; std::string err;
    ts_apply(y.data(), n, time.data(), nullptr, delta, sL, out, err);
    const std::vector<double>& Ly = out;
    bool wantC = (kase != 1), wantT = (kase == 4);
    if (time.empty()) return r;
    double tmin = *std::min_element(time.begin(), time.end());
    int k = 1 + (wantC ? 1 : 0) + (wantT ? 1 : 0);  // L.y + const + trend
    std::vector<int64_t> rows;
    int64_t navail = std::min<int64_t>(n, std::min<int64_t>(static_cast<int64_t>(Ly.size()), static_cast<int64_t>(y.size())));
    for (int64_t i = 0; i < navail; ++i) if (!std::isnan(Ly[i]) && !std::isnan(y[i])) rows.push_back(i);
    int64_t T = static_cast<int64_t>(rows.size());
    if (T <= k + 2) return r;
    std::vector<double> X(static_cast<size_t>(T) * k), dep(T);
    for (int64_t i = 0; i < T; ++i) {
        int64_t t = rows[i];
        dep[i] = y[t];
        int c = 0;
        X[static_cast<int64_t>(c++) * T + i] = Ly[t];
        if (wantT) X[static_cast<int64_t>(c++) * T + i] = time[t] - tmin;
        if (wantC) X[static_cast<int64_t>(c++) * T + i] = 1.0;
    }
    std::vector<double> beta, se;
    double rss, dofr;
    if (!uroot::ols_se(X, T, k, dep, beta, se, rss, dofr)) { r.fail = PperronResult::SINGULAR; return r; }
    double rho = beta[0], sig = se[0];
    double s = std::sqrt(rss / dofr);  // e(rmse)
    // residuals u
    std::vector<double> u(T);
    for (int64_t i = 0; i < T; ++i) {
        double f = 0;
        for (int c = 0; c < k; ++c) f += X[static_cast<int64_t>(c) * T + i] * beta[c];
        u[i] = dep[i] - f;
    }
    int q = lags;
    if (q < 0) q = static_cast<int>(4 * std::pow(static_cast<double>(T) / 100.0, 2.0 / 9.0));
    double Td = static_cast<double>(T);
    double gam0 = 0; for (int64_t i = 0; i < T; ++i) gam0 += u[i] * u[i]; gam0 /= Td;
    double lam = gam0;
    for (int j = 1; j <= q; ++j) {
        double g = 0;
        for (int64_t i = j; i < T; ++i) g += u[i] * u[i - j];
        g /= Td;
        lam += 2.0 * (1.0 - static_cast<double>(j) / (q + 1)) * g;
    }
    double Zp0 = Td * (rho - 1);
    double Zt0 = (rho - 1) / sig;
    double Tss = Td * sig / s;
    r.Zrho = Zp0 - (Tss * Tss) * (lam - gam0) / 2.0;
    r.Ztau = Zt0 * std::sqrt(gam0 / lam) - 0.5 * (lam - gam0) * Tss / std::sqrt(lam);
    r.n = T; r.lags = q;
    uroot::fuller_cv(kase, Td, r.cv1, r.cv5, r.cv10);
    if (kase == 2 || kase == 4) r.p = uroot::mackinnon_p(kase == 4 ? 't' : 'c', r.Ztau);
    r.ok = true;
    return r;
}

}  // namespace cecon
