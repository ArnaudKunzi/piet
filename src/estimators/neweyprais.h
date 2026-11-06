// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// cecon — Newey-West HAC OLS (newey) and Prais-Winsten / Cochrane-Orcutt AR(1)
// FGLS (prais). Lags index by the TIME value (gaps break pairing), matching Stata's
// tsset operators; with a panel column (xtset) a lag pairs rows of the same panel
// only, so every panel change is a gap (BUG-1503 / BUG-1507).
//   newey: β = OLS; Var = (X'X)^-1 · Ω · (X'X)^-1 · n/(n-k), where
//          Ω = Γ0 + Σ_{l=1}^m (1-l/(m+1))(Γl + Γl'),  Γl = Σ_t e_t e_{t-l} x_t x_{t-l}'.
//          lag(0) == regress, vce(robust) (HC1).
//   prais: iterate { ρ̂ from current residuals; GLS-transform y,X by ρ; refit } until
//          |Δρ|<tol. PW scales the first obs of each run by sqrt(1-ρ²); CO drops it.
#pragma once
#include <cmath>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "core/linalg.h"

namespace cecon {

struct NeweyResult {
    int64_t n = 0;
    int k = 0;
    int lag = 0;
    std::vector<double> beta, se, vcov;
    bool ok = false;
    // Rows paired at the first lag 1..m that pairs fewer than two (-1: none does):
    // newey.ado stops there, 0 -> r(2000), 1 -> r(2001).
    int64_t short_lag_pairs = -1;
};

namespace detail {
// (panel, time) -> row: the row a lag operator reaches. `panel` null = one series.
using TsKey = std::pair<double, double>;
inline std::map<TsKey, int64_t> ts_index(const double* time, const double* panel, int64_t n) {
    std::map<TsKey, int64_t> m;
    for (int64_t i = 0; i < n; ++i) m[TsKey(panel ? panel[i] : 0.0, time[i])] = i;
    return m;
}
inline int64_t ts_lag(const std::map<TsKey, int64_t>& m, const double* time, const double* panel,
                      int64_t t, double back) {
    auto it = m.find(TsKey(panel ? panel[t] : 0.0, time[t] - back));
    return (it == m.end()) ? -1 : it->second;
}
}  // namespace detail

// X is col-major n*k (intercept included). time is the f64 time variable, delta the
// step. lag = Newey-West lag truncation m (lag 0 = HC1 robust). panel (optional) is
// the xtset panel variable: Γl then sums only pairs inside one panel.
inline NeweyResult newey_fit(const double* X, int64_t n, int k, const double* y,
                             const double* time, double delta, int lag,
                             const double* panel = nullptr) {
    NeweyResult r;
    r.n = n; r.k = k; r.lag = lag;
    std::vector<double> XtX(static_cast<size_t>(k) * k), Xty(k);
    la::xtx(X, n, k, XtX.data());
    la::xtv(X, n, k, y, Xty.data());
    if (la::spd_inverse(XtX.data(), k) != 0) return r;
    std::vector<double> bread = XtX;  // (X'X)^-1
    r.beta.resize(k);
    la::matvec(bread.data(), k, Xty.data(), r.beta.data());
    std::vector<double> e(n);
    for (int64_t i = 0; i < n; ++i) {
        double f = 0;
        for (int p = 0; p < k; ++p) f += X[static_cast<int64_t>(p) * n + i] * r.beta[p];
        e[i] = y[i] - f;
    }
    const auto tmap = detail::ts_index(time, panel, n);

    std::vector<double> meat(static_cast<size_t>(k) * k, 0.0);
    for (int64_t t = 0; t < n; ++t) {  // Γ0 = Σ e_t² x_t x_t'
        double e2 = e[t] * e[t];
        for (int c = 0; c < k; ++c) {
            double xc = X[static_cast<int64_t>(c) * n + t];
            for (int d = 0; d < k; ++d)
                meat[static_cast<size_t>(c) * k + d] += e2 * xc * X[static_cast<int64_t>(d) * n + t];
        }
    }
    for (int l = 1; l <= lag; ++l) {
        double w = 1.0 - static_cast<double>(l) / (lag + 1);
        std::vector<double> Gl(static_cast<size_t>(k) * k, 0.0);
        int64_t pairs = 0;
        for (int64_t t = 0; t < n; ++t) {
            const int64_t s = detail::ts_lag(tmap, time, panel, t, l * delta);
            if (s < 0) continue;
            ++pairs;
            double prod = e[t] * e[s];
            for (int c = 0; c < k; ++c) {
                double xtc = X[static_cast<int64_t>(c) * n + t];
                for (int d = 0; d < k; ++d)
                    Gl[static_cast<size_t>(c) * k + d] += prod * xtc * X[static_cast<int64_t>(d) * n + s];
            }
        }
        // newey.ado accumulates each lag with `mat vecaccum`, which stops with "no
        // observations" when no row has its lag-l partner and "insufficient
        // observations" when one does (StataBE 19.5: lag(7) on 8 rows, r(2001)).
        if (pairs < 2 && r.short_lag_pairs < 0) r.short_lag_pairs = pairs;
        for (int c = 0; c < k; ++c)
            for (int d = 0; d < k; ++d)
                meat[static_cast<size_t>(c) * k + d] += w * (Gl[static_cast<size_t>(c) * k + d] +
                                                             Gl[static_cast<size_t>(d) * k + c]);
    }
    double dofm = (n - k > 0) ? static_cast<double>(n) / (n - k) : 1.0;
    r.vcov.assign(static_cast<size_t>(k) * k, 0.0);
    std::vector<double> tmp(static_cast<size_t>(k) * k);
    la::sandwich(bread.data(), meat.data(), k, dofm, r.vcov.data(), tmp.data());
    r.se.resize(k);
    for (int c = 0; c < k; ++c) r.se[c] = std::sqrt(std::max(0.0, r.vcov[static_cast<size_t>(c) * k + c]));
    r.ok = true;
    return r;
}

struct PraisResult {
    int64_t n = 0;  // obs used in the transformed regression (PW: n; CO: n-runs)
    int k = 0;
    std::vector<double> beta, se, vcov;
    double rho = 0, dw = 0, dw_0 = 0;
    int iters = 0;
    bool converged = false;
    bool corc = false;
    int64_t n_gaps = 0;  // rows without an in-sample lag partner, less the first (e(N_gaps))
    std::string method, rhotype;
    bool ok = false;
};

namespace detail {
// Durbin-Watson of residuals e (row-indexed; `used` marks the rows that have one):
// Σ (e_t - L.e_t)² over the rows whose lag partner `pred` also has one, over Σ e_t²
// (prais.ado DW: `(e - l.e)^2` is missing across a gap or a panel change).
// Each square is rounded before it is summed, as prais.ado's `gen double` then
// `summarize` do (and as the single-series loop this replaced compiled): separate
// statements, so the products are not fused into the sums.
template <class Pred>
inline double prais_dw(const std::vector<double>& e, const std::vector<char>& used, Pred pred) {
    double num = 0, den = 0;
    for (size_t i = 0; i < e.size(); ++i) {
        if (!used[i]) continue;
        const double sq = e[i] * e[i];
        den += sq;
        const int64_t s = pred(static_cast<int64_t>(i));
        if (s >= 0 && used[static_cast<size_t>(s)]) {
            const double d = e[i] - e[static_cast<size_t>(s)];
            const double d2 = d * d;
            num += d2;
        }
    }
    return (den > 0) ? num / den : 0.0;
}
}  // namespace detail

// rhotype: "regress" (default) or "tscorr". panel (optional) is the xtset panel
// variable: rho, the transform and the DW statistics restart at every panel change,
// as at any gap (Stata's prais notes "computations for rho restarted at each gap").
inline PraisResult prais_fit(const double* X, int64_t n, int k, const double* y,
                             const double* time, double delta, bool corc, bool twostep,
                             const std::string& rhotype, int max_iter = 100,
                             double tol = 1e-6, const double* panel = nullptr) {
    PraisResult r;
    r.k = k; r.corc = corc; r.rhotype = rhotype.empty() ? "regress" : rhotype;
    r.method = twostep ? "twostep" : "iterated";
    const auto tmap = detail::ts_index(time, panel, n);
    auto pred = [&](int64_t t) -> int64_t { return detail::ts_lag(tmap, time, panel, t, delta); };
    // tsreport's definition: a gap is an in-sample row whose lag is not, except
    // the first row of the sample. StataBE 19.5, 40 panels of 5: 39.
    for (int64_t t = 0; t < n; ++t)
        if (pred(t) < 0) ++r.n_gaps;
    if (r.n_gaps > 0) --r.n_gaps;

    auto ols_resid = [&](const std::vector<double>& beta, std::vector<double>& e) {
        e.resize(n);
        for (int64_t i = 0; i < n; ++i) {
            double f = 0;
            for (int p = 0; p < k; ++p) f += X[static_cast<int64_t>(p) * n + i] * beta[p];
            e[i] = y[i] - f;
        }
    };
    auto est_rho = [&](const std::vector<double>& e) -> double {
        double num = 0, den = 0;
        for (int64_t t = 0; t < n; ++t) {
            int64_t s = pred(t);
            if (s >= 0) num += e[t] * e[s];
        }
        if (r.rhotype == "tscorr") {
            for (int64_t t = 0; t < n; ++t) den += e[t] * e[t];  // full SSE
        } else {                                                  // regress: Σ e_{t-1}²
            for (int64_t t = 0; t < n; ++t) { int64_t s = pred(t); if (s >= 0) den += e[s] * e[s]; }
        }
        return (den > 0) ? num / den : 0.0;
    };
    // GLS-transform and refit (noconstant: the intercept is a transformed X column).
    struct Fit { std::vector<double> beta, vcov; double rss = 0; int64_t nt = 0; bool ok = false; };
    auto prais_refit = [&](double rho) -> Fit {
        Fit f;
        double s1 = std::sqrt(std::max(0.0, 1.0 - rho * rho));
        std::vector<int64_t> rows;
        for (int64_t t = 0; t < n; ++t) {
            if (pred(t) >= 0) rows.push_back(t);
            else if (!corc) rows.push_back(t);  // PW keeps run-starts (sqrt-scaled); CO drops
        }
        int64_t m = static_cast<int64_t>(rows.size());
        if (m <= k) return f;
        std::vector<double> T(static_cast<size_t>(m) * k), ty(m);
        for (int64_t i = 0; i < m; ++i) {
            int64_t t = rows[i];
            int64_t s = pred(t);
            if (s >= 0) {
                ty[i] = y[t] - rho * y[s];
                for (int c = 0; c < k; ++c)
                    T[static_cast<int64_t>(c) * m + i] = X[static_cast<int64_t>(c) * n + t] -
                                                         rho * X[static_cast<int64_t>(c) * n + s];
            } else {  // PW run-start
                ty[i] = s1 * y[t];
                for (int c = 0; c < k; ++c)
                    T[static_cast<int64_t>(c) * m + i] = s1 * X[static_cast<int64_t>(c) * n + t];
            }
        }
        std::vector<double> TtT(static_cast<size_t>(k) * k), Tty(k);
        la::xtx(T.data(), m, k, TtT.data());
        la::xtv(T.data(), m, k, ty.data(), Tty.data());
        if (la::spd_inverse(TtT.data(), k) != 0) return f;
        std::vector<double> inv = TtT;
        f.beta.resize(k);
        la::matvec(inv.data(), k, Tty.data(), f.beta.data());
        f.rss = 0;
        for (int64_t i = 0; i < m; ++i) {
            double res = ty[i];
            for (int c = 0; c < k; ++c) res -= T[static_cast<int64_t>(c) * m + i] * f.beta[c];
            f.rss += res * res;
        }
        double s2 = (m - k > 0) ? f.rss / (m - k) : 0.0;  // OLS s² of transformed eqn
        f.vcov.assign(static_cast<size_t>(k) * k, 0.0);
        for (size_t i = 0; i < f.vcov.size(); ++i) f.vcov[i] = s2 * inv[i];
        f.nt = m;
        f.ok = true;
        return f;
    };

    // initial OLS (model 0)
    std::vector<double> XtX(static_cast<size_t>(k) * k), Xty(k), beta(k);
    la::xtx(X, n, k, XtX.data());
    la::xtv(X, n, k, y, Xty.data());
    if (la::spd_inverse(XtX.data(), k) != 0) return r;
    la::matvec(XtX.data(), k, Xty.data(), beta.data());
    std::vector<double> e;
    ols_resid(beta, e);
    r.dw_0 = detail::prais_dw(e, std::vector<char>(static_cast<size_t>(n), 1), pred);

    int maxit = twostep ? 1 : max_iter;
    double rho = 0, lastrho = 2;
    int it = 0;
    Fit f;
    while (it < maxit && std::fabs(rho - lastrho) > tol) {
        lastrho = rho;
        rho = est_rho(e);
        f = prais_refit(rho);
        if (!f.ok) return r;
        ols_resid(f.beta, e);  // original-scale residuals for the next rho
        ++it;
    }
    r.rho = rho;
    r.iters = it;
    r.converged = twostep ? false : (it < maxit);
    r.beta = f.beta;
    r.vcov = f.vcov;
    r.n = f.nt;
    r.se.resize(k);
    for (int c = 0; c < k; ++c) r.se[c] = std::sqrt(std::max(0.0, r.vcov[static_cast<size_t>(c) * k + c]));

    // transformed-residual DW (DWdiff): transform with final rho, resid = ty - T·β
    {
        double s1 = std::sqrt(std::max(0.0, 1.0 - rho * rho));
        std::vector<double> u(static_cast<size_t>(n), 0.0);
        std::vector<char> used(static_cast<size_t>(n), 0);
        for (int64_t t = 0; t < n; ++t) {
            int64_t s = pred(t);
            double tyv, res;
            if (s >= 0) {
                tyv = y[t] - rho * y[s];
                res = tyv;
                for (int c = 0; c < k; ++c)
                    res -= (X[static_cast<int64_t>(c) * n + t] - rho * X[static_cast<int64_t>(c) * n + s]) * r.beta[c];
            } else if (!corc) {
                tyv = s1 * y[t];
                res = tyv;
                for (int c = 0; c < k; ++c) res -= (s1 * X[static_cast<int64_t>(c) * n + t]) * r.beta[c];
            } else {
                continue;
            }
            u[static_cast<size_t>(t)] = res;
            used[static_cast<size_t>(t)] = 1;
        }
        r.dw = detail::prais_dw(u, used, pred);
    }
    r.ok = true;
    return r;
}

}  // namespace cecon
