// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "xtabond.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>

#include "core/collinear.h"
#include "core/linalg.h"
#include "estimators/posthest.h"  // chi2_pvalue

namespace cecon {
namespace {

inline bool fin(double v) { return std::isfinite(v); }

// One (column, value) pair of a Z row. Z is very sparse — a differenced row at
// period t carries (t - 2) GMM entries plus one per typed covariate, a level
// row carries exactly one — so every accumulation below walks nonzeros, and the
// n_i x n_i H block is never materialized (only its 3 nonzero diagonals are
// ever touched).
struct Ent {
    int col;
    double val;
};

// Symmetric inverse: Cholesky when the matrix is positive definite (the normal
// case — H is PD and Z has full column rank), otherwise an eigenvalue
// pseudo-inverse so a structurally empty instrument column degrades to "that
// moment contributes nothing" instead of aborting the fit.
bool sym_inverse(std::vector<double>& A, int m) {
    std::vector<double> keep = A;
    if (la::spd_inverse(A.data(), m) == 0) return true;
    A = keep;
    std::vector<double> w(static_cast<size_t>(m));
    if (la::sym_eig(A.data(), m, w.data()) != 0) return false;
    double wmax = 0.0;
    for (int i = 0; i < m; ++i) wmax = std::max(wmax, std::fabs(w[static_cast<size_t>(i)]));
    const double tol = 1e-12 * wmax;
    // A now holds the eigenvectors (column j <-> w[j]); rebuild V diag(1/w) V'.
    std::vector<double> out(static_cast<size_t>(m) * static_cast<size_t>(m), 0.0);
    for (int j = 0; j < m; ++j) {
        const double wj = w[static_cast<size_t>(j)];
        if (!(std::fabs(wj) > tol)) continue;
        const double inv = 1.0 / wj;
        const double* v = A.data() + static_cast<int64_t>(j) * m;
        for (int r = 0; r < m; ++r) {
            const double vr = v[r] * inv;
            for (int c = 0; c < m; ++c) out[static_cast<size_t>(r) * m + c] += vr * v[c];
        }
    }
    A.swap(out);
    return true;
}

}  // namespace

XtabondResult xtabond_fit(const double* id, const double* time, const double* y, const double* X,
                          int kx, const std::vector<std::string>& xnames, int64_t n,
                          const XtabondSpec& spec, const char* rowmask, bool conventional_vce) {
    XtabondResult R;
    const int p = spec.lags;
    const double delta = spec.delta;
    if (p < 1) { R.err = "xtabond: lags() must be at least 1"; return R; }
    if (!(delta > 0.0)) { R.err = "xtabond: the panel time delta must be positive"; return R; }
    if (kx > 0 && static_cast<int>(xnames.size()) != kx) {
        R.err = "xtabond: covariate name count does not match the design";
        return R;
    }

    // ---- 1. index the panel: id -> (time -> row). Lags are resolved by TIME
    // VALUE (like every other cobra ts operator), so a hole in a panel yields a
    // missing lag rather than silently borrowing the previous present row.
    std::map<double, std::map<double, int64_t>> panel;
    std::map<double, int> period_seen;  // every time value present in the data
    for (int64_t r = 0; r < n; ++r) {
        if (!fin(id[r]) || !fin(time[r])) continue;
        auto& g = panel[id[r]];
        if (!g.emplace(time[r], r).second) {
            R.err = "xtabond: repeated time values within a panel (the data are not uniquely xtset)";
            return R;
        }
        ++period_seen[time[r]];
    }
    if (panel.empty()) { R.err = "xtabond: no observations with a non-missing panel id and time"; return R; }
    std::vector<double> all_periods;
    all_periods.reserve(period_seen.size());
    for (const auto& kv : period_seen) all_periods.push_back(kv.first);

    // ---- 2. the two samples.
    //
    // DIFFERENCED (the estimation sample): D.y_t, the p differenced lags and
    // every D.x must exist -> y at t, t-1, ..., t-(p+1) and each covariate at
    // t and t-1.
    //
    // LEVEL (the `_cons` moment): a strict SUPERSET — the level equation needs
    // one fewer lag of y and no lagged covariate. On the P7 fixture that is
    // 4,433 rows against the differenced 4,103, and it is exactly this
    // difference that puts `_cons` at .8199228 rather than .8284683.
    struct Grp {
        std::vector<double> dt, lt;  // the periods each equation contributes, ascending
        const std::map<double, int64_t>* rows = nullptr;  // this panel's time -> row index
    };
    std::vector<Grp> groups;
    groups.reserve(panel.size());

    auto row_at = [&](const std::map<double, int64_t>& g, double tau, int j) -> int64_t {
        auto it = g.find(tau - static_cast<double>(j) * delta);
        return it == g.end() ? -1 : it->second;
    };

    for (const auto& kv : panel) {
        const std::map<double, int64_t>& g = kv.second;
        Grp G;
        G.rows = &g;
        for (const auto& tr : g) {
            const double tau = tr.first;
            const int64_t r = tr.second;
            // `if` excludes a row from the SAMPLES only — it stays a legal lag
            // source and instrument value below (Stata's ts-operator semantics).
            bool lvl = (rowmask == nullptr || rowmask[r] != 0) && fin(y[r]);
            for (int j = 1; j <= p && lvl; ++j) {
                const int64_t rj = row_at(g, tau, j);
                if (rj < 0 || !fin(y[rj])) lvl = false;
            }
            for (int c = 0; c < kx && lvl; ++c)
                if (!fin(X[static_cast<int64_t>(c) * n + r])) lvl = false;
            if (!lvl) continue;
            G.lt.push_back(tau);

            bool dif = true;
            for (int j = p; j <= p + 1 && dif; ++j) {  // j<p already checked by `lvl`
                const int64_t rj = row_at(g, tau, j);
                if (rj < 0 || !fin(y[rj])) dif = false;
            }
            const int64_t rm = row_at(g, tau, 1);
            if (dif && rm >= 0) {
                for (int c = 0; c < kx && dif; ++c)
                    if (!fin(X[static_cast<int64_t>(c) * n + rm])) dif = false;
            } else {
                dif = false;
            }
            if (!dif) continue;
            G.dt.push_back(tau);
        }
        if (!G.dt.empty()) groups.push_back(std::move(G));
    }
    if (groups.empty()) { R.err = "xtabond: no observations survive the lag/differencing requirements"; return R; }

    int64_t nd_total = 0, nl_total = 0, gaps = 0;
    int mn = 0, mx = 0;
    for (const auto& G : groups) {
        const int ni = static_cast<int>(G.dt.size());
        nd_total += ni;
        nl_total += static_cast<int64_t>(G.lt.size());
        if (mn == 0 || ni < mn) mn = ni;
        mx = std::max(mx, ni);
        for (size_t q = 1; q < G.dt.size(); ++q)
            if (std::fabs((G.dt[q] - G.dt[q - 1]) - delta) > 1e-9 * std::max(1.0, std::fabs(delta)))
                ++gaps;
    }
    R.n_obs = nd_total;
    R.n_groups = static_cast<int>(groups.size());
    R.n_level_obs = nl_total;
    R.min_obs = mn;
    R.max_obs = mx;
    R.avg_obs = static_cast<double>(nd_total) / static_cast<double>(groups.size());
    R.n_period_gaps = gaps;

    // ---- 3. the differenced design, and Stata's collinearity sweep over it.
    // The auto-generated lags come FIRST, so an explicitly typed `L.y` is the
    // LATER member of the collinear pair and is the one omitted — matching the
    // oracle's "note: L.y omitted because of collinearity."
    const int kreg_full = p + kx;
    if (kreg_full == 0) { R.err = "xtabond: the model has no regressors"; return R; }
    std::vector<std::string> full_names;
    full_names.reserve(static_cast<size_t>(kreg_full));
    for (int j = 1; j <= p; ++j) full_names.push_back("L" + std::to_string(j) + ".y");
    for (int c = 0; c < kx; ++c) full_names.push_back(xnames[static_cast<size_t>(c)]);

    // Differenced regressor value for design column `j` at (group, period tau).
    auto dcol = [&](const std::map<double, int64_t>& g, double tau, int j) -> double {
        if (j < p) {
            const int64_t a = row_at(g, tau, j + 1), b = row_at(g, tau, j + 2);
            return y[a] - y[b];
        }
        const int c = j - p;
        const int64_t a = row_at(g, tau, 0), b = row_at(g, tau, 1);
        return X[static_cast<int64_t>(c) * n + a] - X[static_cast<int64_t>(c) * n + b];
    };
    // Level regressor value for the same design column.
    auto lcol = [&](const std::map<double, int64_t>& g, double tau, int j) -> double {
        if (j < p) return y[row_at(g, tau, j + 1)];
        return X[static_cast<int64_t>(j - p) * n + row_at(g, tau, 0)];
    };

    std::vector<double> Dsn(static_cast<size_t>(nd_total) * static_cast<size_t>(kreg_full));
    {
        int64_t r0 = 0;
        for (const auto& G : groups) {
            for (size_t q = 0; q < G.dt.size(); ++q) {
                for (int j = 0; j < kreg_full; ++j)
                    Dsn[static_cast<int64_t>(j) * nd_total + r0] = dcol(*G.rows, G.dt[q], j);
                ++r0;
            }
        }
    }
    std::vector<char> keep = collinear_keep_mask(Dsn.data(), nd_total, kreg_full);
    std::vector<int> kept;
    for (int j = 0; j < kreg_full; ++j) {
        if (keep[static_cast<size_t>(j)]) kept.push_back(j);
        else R.omitted.push_back(full_names[static_cast<size_t>(j)]);
    }
    if (kept.empty()) { R.err = "xtabond: every regressor was collinear in the differenced design"; return R; }
    const int kslope = static_cast<int>(kept.size());
    const int k = kslope + (spec.constant ? 1 : 0);
    R.k = k;
    for (int j : kept) R.names.push_back(full_names[static_cast<size_t>(j)]);
    if (spec.constant) R.names.push_back("_cons");

    // ---- 4. instrument columns.
    //
    // GMM-type L(2/.).y: one column per (differenced period tau, level period
    // sigma <= tau - 2*delta) pair that CAN occur anywhere in the data. The
    // column exists whether or not a given panel has that observation — a panel
    // missing it contributes a zero, the column is not dropped. That rule is
    // what makes the count 253 = SUM_{t=3..24} (t-2) on the P7 fixture, and
    // getting it wrong is directly visible in the instrument total.
    //
    // THE DEPTH IS 2, NOT p+1 — MEASURED, and it contradicts the obvious guess.
    // D.eps_t is MA(1) in eps whatever p is, so y_{t-2} is orthogonal to it for
    // every lag order; only the SAMPLE moves with p (a differenced row needs y
    // back to t-(p+1)). Stata prints "GMM-type: L(2/.).y" under lags(2) as well
    // as lags(1), and its lags(2) instrument total on this fixture is 255 =
    // SUM_{t=4..24}(t-2) + 2 + 1 = 252 + 3. The p+1 reading gives 234.
    std::vector<double> dperiods;
    {
        std::map<double, int> seen;
        for (const auto& G : groups)
            for (double t : G.dt) ++seen[t];
        for (const auto& kv : seen) dperiods.push_back(kv.first);
    }
    std::map<std::pair<double, double>, int> gmm_idx;
    for (double tau : dperiods) {
        const double cutoff = tau - 2.0 * delta;  // AB depth: lag 2, for every p
        const double eps = 1e-9 * std::max(1.0, std::fabs(cutoff));
        for (double sig : all_periods) {
            if (sig > cutoff + eps) break;  // all_periods is ascending
            gmm_idx.emplace(std::make_pair(tau, sig), static_cast<int>(gmm_idx.size()));
        }
    }
    const int m_gmm = static_cast<int>(gmm_idx.size());
    const int m_std = kx;
    const int m_lvl = spec.constant ? 1 : 0;
    const int m = m_gmm + m_std + m_lvl;
    R.n_gmm_instruments = m_gmm;
    R.n_std_instruments = m_std;
    R.n_level_instruments = m_lvl;
    R.n_instruments = m;
    R.gmm_label = "L(2/.).y";
    for (int c = 0; c < kx; ++c) {
        // Stata composes the difference INTO the operator prefix: the standard
        // instrument for a typed `L.y` prints as "LD.y", not "D.L.y".
        const std::string& nm = xnames[static_cast<size_t>(c)];
        const size_t dot = nm.rfind('.');
        R.std_labels.push_back(dot == std::string::npos ? "D." + nm
                                                        : nm.substr(0, dot) + "D." + nm.substr(dot + 1));
    }
    if (m < k) { R.err = "xtabond: fewer instruments than parameters (the model is not identified)"; return R; }

    // Per-group builder, shared by the two accumulation passes. `Z` gets one
    // sparse row per stacked observation (differenced rows first, then level
    // rows), `Xr` the k design values, `yv` the left-hand side.
    std::vector<std::vector<Ent>> Z;
    std::vector<double> Xr, yv;
    auto build = [&](const Grp& G, int& nd, int& nl) {
        const std::map<double, int64_t>& g = *G.rows;
        nd = static_cast<int>(G.dt.size());
        nl = m_lvl ? static_cast<int>(G.lt.size()) : 0;
        Z.assign(static_cast<size_t>(nd + nl), {});
        Xr.assign(static_cast<size_t>(nd + nl) * static_cast<size_t>(k), 0.0);
        yv.assign(static_cast<size_t>(nd + nl), 0.0);
        for (int q = 0; q < nd; ++q) {
            const double tau = G.dt[static_cast<size_t>(q)];
            auto& row = Z[static_cast<size_t>(q)];
            const double cutoff = tau - 2.0 * delta;  // AB depth: lag 2, for every p
            const double eps = 1e-9 * std::max(1.0, std::fabs(cutoff));
            for (double sig : all_periods) {
                if (sig > cutoff + eps) break;
                auto it = g.find(sig);
                if (it == g.end() || !fin(y[it->second])) continue;  // zero fill
                row.push_back({gmm_idx.at(std::make_pair(tau, sig)), y[it->second]});
            }
            for (int c = 0; c < kx; ++c) {
                const int64_t a = row_at(g, tau, 0), b = row_at(g, tau, 1);
                row.push_back({m_gmm + c, X[static_cast<int64_t>(c) * n + a] -
                                              X[static_cast<int64_t>(c) * n + b]});
            }
            for (int j = 0; j < kslope; ++j)
                Xr[static_cast<size_t>(q) * k + j] = dcol(g, tau, kept[static_cast<size_t>(j)]);
            // the constant differences away: its differenced-row entry is 0
            yv[static_cast<size_t>(q)] = y[row_at(g, tau, 0)] - y[row_at(g, tau, 1)];
        }
        for (int q = 0; q < nl; ++q) {
            const double tau = G.lt[static_cast<size_t>(q)];
            const size_t rr = static_cast<size_t>(nd + q);
            Z[rr].push_back({m_gmm + m_std, 1.0});
            for (int j = 0; j < kslope; ++j)
                Xr[rr * k + j] = lcol(g, tau, kept[static_cast<size_t>(j)]);
            Xr[rr * k + kslope] = 1.0;
            yv[rr] = y[row_at(g, tau, 0)];
        }
    };

    // ---- 5. pass 1 — Q_xz, Q_zy and SUM_i Z_i' H_i Z_i.
    std::vector<double> Qxz(static_cast<size_t>(k) * m, 0.0), Qzy(static_cast<size_t>(m), 0.0);
    std::vector<double> ZHZ(static_cast<size_t>(m) * m, 0.0);
    for (const auto& G : groups) {
        int nd = 0, nl = 0;
        build(G, nd, nl);
        for (int q = 0; q < nd + nl; ++q) {
            for (const Ent& e : Z[static_cast<size_t>(q)]) {
                Qzy[static_cast<size_t>(e.col)] += e.val * yv[static_cast<size_t>(q)];
                for (int j = 0; j < k; ++j)
                    Qxz[static_cast<size_t>(j) * m + e.col] += Xr[static_cast<size_t>(q) * k + j] * e.val;
            }
        }
        auto outer = [&](int ra, int rb, double h) {
            for (const Ent& a : Z[static_cast<size_t>(ra)])
                for (const Ent& b : Z[static_cast<size_t>(rb)])
                    ZHZ[static_cast<size_t>(a.col) * m + b.col] += h * a.val * b.val;
        };
        for (int q = 0; q < nd; ++q) outer(q, q, 1.0);
        for (int q = 0; q + 1 < nd; ++q) {
            const double dt = G.dt[static_cast<size_t>(q + 1)] - G.dt[static_cast<size_t>(q)];
            if (std::fabs(dt - delta) > 1e-9 * std::max(1.0, std::fabs(delta))) continue;
            outer(q, q + 1, -0.5);
            outer(q + 1, q, -0.5);
        }
        for (int q = 0; q < nl; ++q) outer(nd + q, nd + q, 0.5);
    }

    // ---- 6. one-step coefficients.
    std::vector<double> A1 = ZHZ;
    if (!sym_inverse(A1, m)) { R.err = "xtabond: the instrument weight matrix could not be inverted"; return R; }
    std::vector<double> QA(static_cast<size_t>(k) * m, 0.0);  // Q_xz A1, k x m
    for (int j = 0; j < k; ++j)
        for (int b = 0; b < m; ++b) {
            double s = 0.0;
            for (int a = 0; a < m; ++a) s += Qxz[static_cast<size_t>(j) * m + a] * A1[static_cast<size_t>(a) * m + b];
            QA[static_cast<size_t>(j) * m + b] = s;
        }
    std::vector<double> W1(static_cast<size_t>(k) * k, 0.0), rhs(static_cast<size_t>(k), 0.0);
    for (int j = 0; j < k; ++j) {
        for (int b = 0; b < m; ++b) {
            rhs[static_cast<size_t>(j)] += QA[static_cast<size_t>(j) * m + b] * Qzy[static_cast<size_t>(b)];
            for (int i = 0; i < k; ++i)
                W1[static_cast<size_t>(j) * k + i] += QA[static_cast<size_t>(j) * m + b] * Qxz[static_cast<size_t>(i) * m + b];
        }
    }
    std::vector<double> W1i = W1;
    if (!sym_inverse(W1i, k)) { R.err = "xtabond: X'ZWZ'X is singular (the model is not identified)"; return R; }
    R.beta.assign(static_cast<size_t>(k), 0.0);
    for (int j = 0; j < k; ++j)
        for (int i = 0; i < k; ++i)
            R.beta[static_cast<size_t>(j)] += W1i[static_cast<size_t>(j) * k + i] * rhs[static_cast<size_t>(i)];

    // ---- 7. pass 2 — the cluster-on-panel robust sandwich. M = W1^-1 Q_xz A1,
    // and V = SUM_i (M Z_i'u_i)(M Z_i'u_i)': the m x m meat is never formed.
    std::vector<double> M(static_cast<size_t>(k) * m, 0.0);
    for (int j = 0; j < k; ++j)
        for (int b = 0; b < m; ++b) {
            double s = 0.0;
            for (int i = 0; i < k; ++i) s += W1i[static_cast<size_t>(j) * k + i] * QA[static_cast<size_t>(i) * m + b];
            M[static_cast<size_t>(j) * m + b] = s;
        }
    R.vcov.assign(static_cast<size_t>(k) * k, 0.0);
    R.zu.assign(static_cast<size_t>(m), 0.0);
    std::vector<double> zui(static_cast<size_t>(m)), ti(static_cast<size_t>(k));
    for (const auto& G : groups) {
        int nd = 0, nl = 0;
        build(G, nd, nl);
        std::fill(zui.begin(), zui.end(), 0.0);
        for (int q = 0; q < nd + nl; ++q) {
            double u = yv[static_cast<size_t>(q)];
            for (int j = 0; j < k; ++j) u -= Xr[static_cast<size_t>(q) * k + j] * R.beta[static_cast<size_t>(j)];
            if (q < nd) R.rss += u * u;  // BUG-383 #5: e(rss) = differenced rows only
            for (const Ent& e : Z[static_cast<size_t>(q)]) zui[static_cast<size_t>(e.col)] += e.val * u;
        }
        for (int b = 0; b < m; ++b) R.zu[static_cast<size_t>(b)] += zui[static_cast<size_t>(b)];
        for (int j = 0; j < k; ++j) {
            double s = 0.0;
            for (int b = 0; b < m; ++b) s += M[static_cast<size_t>(j) * m + b] * zui[static_cast<size_t>(b)];
            ti[static_cast<size_t>(j)] = s;
        }
        for (int j = 0; j < k; ++j)
            for (int i = 0; i < k; ++i)
                R.vcov[static_cast<size_t>(j) * k + i] += ti[static_cast<size_t>(j)] * ti[static_cast<size_t>(i)];
    }
    // BUG-383 #5 (off-pin sweep vs live Stata, 2026-09-01): Stata's bare
    // `xtabond` reports the one-step CONVENTIONAL VCE — vce(robust) is opt-in —
    // and ours only had the robust sandwich, silently, so every default-form
    // header chi2 disagreed (ours 118.82 vs Stata 89.64 on the fixture).
    // Conventional: V = sigma2_v * (Q A1_stata Q')^{-1}. Our H is Stata's H/2
    // (1 / -0.5 diagonal convention), so A1_ours = 2 A1_stata and
    // W1i_ours = W1i_stata / 2, giving V = 2 * sig2 * W1i_ours =
    // (rss/(N-k)) * W1i_ours. The scale was backed out of the capture exactly:
    // e(rss)/e(sig2) = 474 = 2(N-k) on the fixture (N=240, k=3).
    R.sig2 = (R.n_obs > k) ? R.rss / (2.0 * static_cast<double>(R.n_obs - k)) : 0.0;
    if (conventional_vce) {
        const double s2d = 2.0 * R.sig2;  // rss/(N-k)
        for (int j = 0; j < k; ++j)
            for (int i = 0; i < k; ++i)
                R.vcov[static_cast<size_t>(j) * k + i] = s2d * W1i[static_cast<size_t>(j) * k + i];
    }
    R.se.assign(static_cast<size_t>(k), 0.0);
    for (int j = 0; j < k; ++j)
        R.se[static_cast<size_t>(j)] = std::sqrt(std::max(0.0, R.vcov[static_cast<size_t>(j) * k + j]));

    // GMM first-order conditions at the solution — zero for any correct solve,
    // whatever the identification status. (zu itself is zero only when m == k.)
    R.foc.assign(static_cast<size_t>(k), 0.0);
    for (int j = 0; j < k; ++j) {
        double s = 0.0;
        for (int b = 0; b < m; ++b) s += QA[static_cast<size_t>(j) * m + b] * R.zu[static_cast<size_t>(b)];
        R.foc[static_cast<size_t>(j)] = s;
    }

    // ---- 8. Wald chi2 over the slopes (Stata's header excludes `_cons`).
    if (kslope > 0) {
        std::vector<double> S(static_cast<size_t>(kslope) * kslope);
        for (int r = 0; r < kslope; ++r)
            for (int c = 0; c < kslope; ++c)
                S[static_cast<size_t>(r) * kslope + c] = R.vcov[static_cast<size_t>(r) * k + c];
        if (la::spd_inverse(S.data(), kslope) == 0) {
            double q = 0.0;
            for (int r = 0; r < kslope; ++r)
                for (int c = 0; c < kslope; ++c)
                    q += R.beta[static_cast<size_t>(r)] * S[static_cast<size_t>(r) * kslope + c] *
                         R.beta[static_cast<size_t>(c)];
            R.chi2 = q;
            R.chi2_df = kslope;
            R.p_chi2 = chi2_pvalue(q, static_cast<double>(kslope));
        }
    }
    R.ok = true;
    return R;
}

}  // namespace cecon
