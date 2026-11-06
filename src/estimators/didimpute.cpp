// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Borusyak-Jaravel-Spiess imputation estimator — implemented
// from the published methodology paper:
//
//   [BJS24] Borusyak, K., X. Jaravel and J. Spiess (2024), "Revisiting
//           Event-Study Designs: Robust and Efficient Estimation", Review of
//           Economic Studies 91(6), 3253-3285.
//
// See didimpute.h for the contract and for the conventions this project's
// goldens pin.
//
// The Y(0) model is a two-way (unit, period) fixed-effect least squares on the
// untreated subsample. Two things are needed from it that a plain within
// transform does not give: the fitted level alpha_i + beta_t at cells OUTSIDE
// the estimation sample (the imputation itself), and the row weights
// v = X_0 (X_0'X_0)^+ xbar_h that make each tau_h a known linear combination
// v'Y (the inference). Both come from the same normal equations, so they are
// solved once, directly: one FE block is eliminated analytically and the other
// is solved through a symmetric pseudo-inverse — which is also what makes the
// solution well-defined when the untreated design splits into several
// connected components (one null direction per component, dropped).
//
// The pre-trend leads, by contrast, are ordinary regression coefficients on the
// SAME untreated sample, so they take the engine's own HDFE within transform
// (src/hdfe/demean.h) plus a cluster-robust sandwich.

#include "estimators/didimpute.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <unordered_map>
#include <vector>

#include "core/distributions.h"
#include "core/linalg.h"
#include "core/model.h"
#include "hdfe/demean.h"

namespace cecon {
namespace {

Factor make_factor_over(const double* v, const std::vector<int64_t>& rows, int64_t n,
                        const char* name, std::vector<int>* code_all) {
    Factor f;
    f.name = name;
    std::map<double, int> lev;
    for (int64_t i : rows) lev.emplace(v[i], 0);
    int c = 0;
    for (auto& kv : lev) kv.second = c++;
    f.n_levels = c;
    f.codes.resize(rows.size());
    f.count.assign(static_cast<size_t>(c), 0.0);
    for (size_t r = 0; r < rows.size(); ++r) {
        int code = lev.find(v[rows[r]])->second;
        f.codes[r] = code;
        f.count[static_cast<size_t>(code)] += 1.0;
    }
    if (code_all != nullptr) {
        code_all->assign(static_cast<size_t>(n), -1);
        for (int64_t i = 0; i < n; ++i) {
            auto it = lev.find(v[i]);
            if (it != lev.end()) (*code_all)[static_cast<size_t>(i)] = it->second;
        }
    }
    return f;
}

// Normal equations of a two-way fixed-effect least squares, X = [D_E | D_R].
// The E ("eliminated") block is folded out analytically; the R ("retained")
// block — chosen as the SMALLER of the two so the dense system stays small —
// is solved with a symmetric pseudo-inverse, which handles the one null
// direction per connected component without any pivot choice.
struct TwoWayNormal {
    int nE = 0, nR = 0;
    std::vector<double> dE, dR;
    // For each eliminated level: its (retained level, count) pairs.
    std::vector<std::vector<std::pair<int, double>>> byE;
    std::vector<double> Sp;  // nR x nR pseudo-inverse of the reduced system

    void build(const std::vector<int>& ce, const std::vector<int>& cr, int ne, int nr) {
        nE = ne; nR = nr;
        dE.assign(static_cast<size_t>(ne), 0.0);
        dR.assign(static_cast<size_t>(nr), 0.0);
        std::vector<std::map<int, double>> acc(static_cast<size_t>(ne));
        for (size_t i = 0; i < ce.size(); ++i) {
            dE[static_cast<size_t>(ce[i])] += 1.0;
            dR[static_cast<size_t>(cr[i])] += 1.0;
            acc[static_cast<size_t>(ce[i])][cr[i]] += 1.0;
        }
        byE.assign(static_cast<size_t>(ne), {});
        for (int e = 0; e < ne; ++e)
            for (const auto& kv : acc[static_cast<size_t>(e)])
                byE[static_cast<size_t>(e)].emplace_back(kv.first, kv.second);

        std::vector<double> S(static_cast<size_t>(nr) * nr, 0.0);
        for (int r = 0; r < nr; ++r) S[static_cast<size_t>(r) * nr + r] = dR[static_cast<size_t>(r)];
        for (int e = 0; e < ne; ++e) {
            const double inv = 1.0 / dE[static_cast<size_t>(e)];
            const auto& lst = byE[static_cast<size_t>(e)];
            for (const auto& p1 : lst)
                for (const auto& p2 : lst)
                    S[static_cast<size_t>(p1.first) * nr + p2.first] -= p1.second * p2.second * inv;
        }
        // Symmetric pseudo-inverse.
        std::vector<double> w(static_cast<size_t>(nr));
        std::vector<double> Q = S;
        Sp.assign(static_cast<size_t>(nr) * nr, 0.0);
        if (la::sym_eig(Q.data(), nr, w.data()) != 0) return;
        double wmax = 0;
        for (int j = 0; j < nr; ++j) wmax = std::max(wmax, std::fabs(w[static_cast<size_t>(j)]));
        const double tol = wmax * 1e-11;
        for (int j = 0; j < nr; ++j) {
            if (w[static_cast<size_t>(j)] <= tol) continue;
            const double inv = 1.0 / w[static_cast<size_t>(j)];
            const double* q = Q.data() + static_cast<size_t>(j) * nr;
            for (int r1 = 0; r1 < nr; ++r1)
                for (int r2 = 0; r2 < nr; ++r2)
                    Sp[static_cast<size_t>(r1) * nr + r2] += inv * q[r1] * q[r2];
        }
    }

    // Solve X'X [aE; bR] = [rE; rR].
    void solve(const std::vector<double>& rE, const std::vector<double>& rR,
               std::vector<double>& aE, std::vector<double>& bR) const {
        std::vector<double> rhs = rR;
        for (int e = 0; e < nE; ++e) {
            const double s = rE[static_cast<size_t>(e)] / dE[static_cast<size_t>(e)];
            for (const auto& p : byE[static_cast<size_t>(e)]) rhs[static_cast<size_t>(p.first)] -= p.second * s;
        }
        bR.assign(static_cast<size_t>(nR), 0.0);
        for (int r1 = 0; r1 < nR; ++r1) {
            double s = 0;
            for (int r2 = 0; r2 < nR; ++r2)
                s += Sp[static_cast<size_t>(r1) * nR + r2] * rhs[static_cast<size_t>(r2)];
            bR[static_cast<size_t>(r1)] = s;
        }
        aE.assign(static_cast<size_t>(nE), 0.0);
        for (int e = 0; e < nE; ++e) {
            double s = rE[static_cast<size_t>(e)];
            for (const auto& p : byE[static_cast<size_t>(e)]) s -= p.second * bR[static_cast<size_t>(p.first)];
            aE[static_cast<size_t>(e)] = s / dE[static_cast<size_t>(e)];
        }
    }
};

std::string fmt_h(double h) {
    long long v = static_cast<long long>(std::llround(h));
    if (std::fabs(h - static_cast<double>(v)) < 1e-9) return std::to_string(v);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", h);
    return buf;
}

}  // namespace

DidImputeResult didimpute_run(const double* y, const double* ivar, const double* time,
                              const double* treat_time, int64_t n_obs,
                              const DidImputeConfig& cfg) {
    DidImputeResult R;
    R.level = cfg.level;
    if (n_obs <= 0 || y == nullptr || ivar == nullptr || time == nullptr || treat_time == nullptr) {
        R.ok = false;
        R.error = "did_imputation: empty or incomplete estimation sample";
        return R;
    }
    // Treated iff the unit has a treatment date and the period has reached it.
    std::vector<char> treated(static_cast<size_t>(n_obs), 0);
    std::vector<int64_t> untr;
    untr.reserve(static_cast<size_t>(n_obs));
    for (int64_t i = 0; i < n_obs; ++i) {
        const bool ever = !std::isnan(treat_time[i]);
        treated[static_cast<size_t>(i)] = (ever && time[i] >= treat_time[i]) ? 1 : 0;
        if (!treated[static_cast<size_t>(i)]) untr.push_back(i);
    }
    if (untr.empty()) {
        R.ok = false;
        R.error = "did_imputation: no untreated observation — Y(0) cannot be estimated";
        return R;
    }

    std::vector<int> unit_all, time_all;
    Factor fi = make_factor_over(ivar, untr, n_obs, "ivar", &unit_all);
    Factor ft = make_factor_over(time, untr, n_obs, "time", &time_all);
    const int nI = fi.n_levels, nT = ft.n_levels;
    const int64_t n0 = static_cast<int64_t>(untr.size());
    R.n_model = n0;
    R.n_units = nI;

    // Connected components of the untreated design: a cell is imputable only if
    // its unit and its period sit in the SAME component (otherwise their two
    // fixed effects are on unrelated scales).
    std::vector<int> parent(static_cast<size_t>(nI + nT));
    for (size_t i = 0; i < parent.size(); ++i) parent[i] = static_cast<int>(i);
    std::function<int(int)> find = [&](int x) {
        while (parent[static_cast<size_t>(x)] != x) {
            parent[static_cast<size_t>(x)] =
                parent[static_cast<size_t>(parent[static_cast<size_t>(x)])];
            x = parent[static_cast<size_t>(x)];
        }
        return x;
    };
    for (size_t r = 0; r < untr.size(); ++r) {
        int a = find(fi.codes[r]), b = find(nI + ft.codes[r]);
        if (a != b) parent[static_cast<size_t>(a)] = b;
    }
    int ncomp = 0;
    {
        std::vector<char> seen(parent.size(), 0);
        for (int u = 0; u < nI; ++u) {
            int r = find(u);
            if (!seen[static_cast<size_t>(r)]) { seen[static_cast<size_t>(r)] = 1; ++ncomp; }
        }
        if (ncomp == 0) ncomp = 1;
    }

    // --- Y(0) fit --------------------------------------------------------------
    const bool unit_is_retained = nI <= nT;
    std::vector<int> ce(untr.size()), cr(untr.size());
    for (size_t r = 0; r < untr.size(); ++r) {
        ce[r] = unit_is_retained ? ft.codes[r] : fi.codes[r];
        cr[r] = unit_is_retained ? fi.codes[r] : ft.codes[r];
    }
    TwoWayNormal sys;
    sys.build(ce, cr, unit_is_retained ? nT : nI, unit_is_retained ? nI : nT);

    std::vector<double> rE(static_cast<size_t>(sys.nE), 0.0), rR(static_cast<size_t>(sys.nR), 0.0);
    for (size_t r = 0; r < untr.size(); ++r) {
        const double yv = y[untr[r]];
        rE[static_cast<size_t>(ce[r])] += yv;
        rR[static_cast<size_t>(cr[r])] += yv;
    }
    std::vector<double> aE, bR;
    sys.solve(rE, rR, aE, bR);
    auto fitted = [&](int64_t i) {
        const int u = unit_all[static_cast<size_t>(i)], t = time_all[static_cast<size_t>(i)];
        const int e = unit_is_retained ? t : u, rr = unit_is_retained ? u : t;
        return aE[static_cast<size_t>(e)] + bR[static_cast<size_t>(rr)];
    };
    auto imputable = [&](int64_t i) {
        const int u = unit_all[static_cast<size_t>(i)], t = time_all[static_cast<size_t>(i)];
        return u >= 0 && t >= 0 && find(u) == find(nI + t);
    };

    std::vector<double> resid0(untr.size());
    for (size_t r = 0; r < untr.size(); ++r) resid0[r] = y[untr[r]] - fitted(untr[r]);

    // Clusters: the unit variable (did_imputation's default), over the units that
    // actually enter the Y(0) model. A unit with no untreated observation
    // contributes nothing to any estimate — its cells are unimputable — so it is
    // not a cluster either, and must not inflate the G/(G-1) factor.
    std::unordered_map<double, int> cl_index;
    std::vector<int> cl_code(static_cast<size_t>(n_obs), -1);
    for (int64_t i : untr) {
        auto it = cl_index.find(ivar[i]);
        if (it == cl_index.end()) it = cl_index.emplace(ivar[i], static_cast<int>(cl_index.size())).first;
    }
    for (int64_t i = 0; i < n_obs; ++i) {
        auto it = cl_index.find(ivar[i]);
        if (it != cl_index.end()) cl_code[static_cast<size_t>(i)] = it->second;
    }
    const int NC = static_cast<int>(cl_index.size());
    R.n_clusters = NC;

    const double alpha = cfg.level == 95 ? 0.05 : 1.0 - cfg.level / 100.0;
    const double zcrit = z_critical(alpha);

    // --- autosample admission ---------------------------------------------------
    // Decided BEFORE anything is estimated: without autosample the unimputable
    // cells are an error, and an error must not ship a coefficient payload
    // computed off the very sample the caller has not agreed to drop.
    std::vector<std::string> lost;
    for (double h : cfg.horizons) {
        int64_t all_cells = 0, ok_cells = 0;
        for (int64_t i = 0; i < n_obs; ++i) {
            if (!treated[static_cast<size_t>(i)]) continue;
            if (time[i] - treat_time[i] != h) continue;
            ++all_cells;
            if (imputable(i)) ++ok_cells;
        }
        if (all_cells > ok_cells) {
            R.n_dropped += all_cells - ok_cells;
            lost.push_back("tau" + fmt_h(h));
        }
    }
    if (!lost.empty() && !cfg.autosample) {
        R.ok = false;
        R.n_model = 0;
        R.n_units = 0;
        R.n_dropped = 0;
        R.error = "did_imputation: the fixed effects of some treated cells cannot be "
                  "imputed from the untreated sample; specify autosample to drop them";
        return R;
    }
    if (!lost.empty()) {
        R.autosample_dropped = true;
        R.autosample_coefs = lost;
        std::string msg = "Warning: part of the sample was dropped for the following "
                          "coefficients because FE could not be imputed:";
        for (const auto& s : lost) msg += " " + s;
        msg += ".";
        R.autosample_message = msg;
    }

    // --- tau_h ------------------------------------------------------------------
    int64_t n_used_cells = 0;
    std::vector<double> vrow(static_cast<size_t>(n_obs));
    std::vector<double> eps(static_cast<size_t>(n_obs));
    std::vector<double> cs(static_cast<size_t>(NC));
    for (double h : cfg.horizons) {
        DidImputeCoef row;
        row.name = "tau" + fmt_h(h);
        row.horizon = h;
        std::vector<int64_t> cells;
        int64_t all_cells = 0;
        for (int64_t i = 0; i < n_obs; ++i) {
            if (!treated[static_cast<size_t>(i)]) continue;
            if (time[i] - treat_time[i] != h) continue;
            ++all_cells;
            if (imputable(i)) cells.push_back(i);
        }
        row.n_cells_all = all_cells;
        row.n_cells = static_cast<int64_t>(cells.size());
        if (cells.empty()) {
            row.estimate = std::nan(""); row.se = std::nan("");
            row.z = std::nan(""); row.p = std::nan("");
            row.ci_lo = std::nan(""); row.ci_hi = std::nan("");
            R.coefs.push_back(row);
            continue;
        }
        n_used_cells += static_cast<int64_t>(cells.size());
        const double nh = static_cast<double>(cells.size());
        double tau = 0;
        for (int64_t i : cells) tau += y[i] - fitted(i);
        tau /= nh;
        row.estimate = tau;

        // v = X0 (X0'X0)^+ xbar_h on the estimation sample, 1/n_h on the cells.
        std::fill(rE.begin(), rE.end(), 0.0);
        std::fill(rR.begin(), rR.end(), 0.0);
        for (int64_t i : cells) {
            const int u = unit_all[static_cast<size_t>(i)], t = time_all[static_cast<size_t>(i)];
            const int e = unit_is_retained ? t : u, rr = unit_is_retained ? u : t;
            rE[static_cast<size_t>(e)] += 1.0 / nh;
            rR[static_cast<size_t>(rr)] += 1.0 / nh;
        }
        std::vector<double> vaE, vbR;
        sys.solve(rE, rR, vaE, vbR);
        std::fill(vrow.begin(), vrow.end(), 0.0);
        std::fill(eps.begin(), eps.end(), 0.0);
        for (size_t r = 0; r < untr.size(); ++r) {
            const int64_t i = untr[r];
            const int u = unit_all[static_cast<size_t>(i)], t = time_all[static_cast<size_t>(i)];
            const int e = unit_is_retained ? t : u, rr = unit_is_retained ? u : t;
            vrow[static_cast<size_t>(i)] = -(vaE[static_cast<size_t>(e)] + vbR[static_cast<size_t>(rr)]);
            eps[static_cast<size_t>(i)] = resid0[r];
        }
        // Treated-cell residuals: deviation from the cell's own (cohort, horizon)
        // mean, so cross-cohort effect heterogeneity is not read as noise.
        std::map<double, std::pair<double, double>> coh;  // cohort -> (sum, count)
        for (int64_t i : cells) {
            auto& s = coh[treat_time[i]];
            s.first += y[i] - fitted(i);
            s.second += 1.0;
        }
        for (int64_t i : cells) {
            const auto& s = coh[treat_time[i]];
            vrow[static_cast<size_t>(i)] = 1.0 / nh;
            eps[static_cast<size_t>(i)] = (y[i] - fitted(i)) - s.first / s.second;
        }
        std::fill(cs.begin(), cs.end(), 0.0);
        for (int64_t i = 0; i < n_obs; ++i) {
            const int c = cl_code[static_cast<size_t>(i)];
            if (c >= 0)
                cs[static_cast<size_t>(c)] +=
                    vrow[static_cast<size_t>(i)] * eps[static_cast<size_t>(i)];
        }
        double var = 0;
        for (int c = 0; c < NC; ++c) var += cs[static_cast<size_t>(c)] * cs[static_cast<size_t>(c)];
        row.se = var > 0 ? std::sqrt(var) : 0.0;
        row.z = row.se > 0 ? tau / row.se : std::nan("");
        row.p = row.se > 0 ? std::erfc(std::fabs(row.z) * 0.7071067811865476) : std::nan("");
        row.ci_lo = tau - zcrit * row.se;
        row.ci_hi = tau + zcrit * row.se;
        R.coefs.push_back(row);
    }
    R.n_obs = n0 + n_used_cells;

    // --- pre-trend leads --------------------------------------------------------
    if (cfg.pretrends > 0) {
        const int P = cfg.pretrends;
        std::vector<double> lead(static_cast<size_t>(n0) * P, 0.0);
        std::vector<int64_t> ncell(static_cast<size_t>(P), 0);
        for (size_t r = 0; r < untr.size(); ++r) {
            const int64_t i = untr[r];
            if (std::isnan(treat_time[i])) continue;
            const double rel = time[i] - treat_time[i];
            for (int k = 1; k <= P; ++k)
                if (rel == -static_cast<double>(k)) {
                    lead[static_cast<size_t>(k - 1) * n0 + static_cast<int64_t>(r)] = 1.0;
                    ++ncell[static_cast<size_t>(k - 1)];
                }
        }
        std::vector<double> yl(untr.size());
        for (size_t r = 0; r < untr.size(); ++r) yl[r] = y[untr[r]];
        Factor fi2 = fi, ft2 = ft;
        std::vector<Factor> fes{fi2, ft2};
        demean(yl.data(), n0, 1, fes);
        demean(lead.data(), n0, P, fes);

        std::vector<double> A(static_cast<size_t>(P) * P);
        la::xtx(lead.data(), n0, P, A.data());
        std::vector<double> rhs(static_cast<size_t>(P));
        la::xtv(lead.data(), n0, P, yl.data(), rhs.data());
        if (la::spd_inverse(A.data(), P) != 0) {
            R.ok = false;
            R.coefs.clear();  // an error ships no numbers
            R.error = "did_imputation: the pretrends() leads are collinear with the "
                      "unit/period fixed effects on the untreated sample";
            return R;
        }
        std::vector<double> pi(static_cast<size_t>(P));
        la::matvec(A.data(), P, rhs.data(), pi.data());
        std::vector<double> el(untr.size());
        la::residual(lead.data(), n0, P, pi.data(), yl.data(), el.data());

        std::vector<double> S(static_cast<size_t>(NC) * P, 0.0);
        for (size_t r = 0; r < untr.size(); ++r) {
            const int c = cl_code[static_cast<size_t>(untr[r])];
            for (int j = 0; j < P; ++j)
                S[static_cast<size_t>(j) * NC + c] += lead[static_cast<size_t>(j) * n0 + static_cast<int64_t>(r)] * el[r];
        }
        std::vector<double> meat(static_cast<size_t>(P) * P);
        la::xtx(S.data(), NC, P, meat.data());
        // K = rank(design) - #unit FE = (nT - ncomp) + P: the unit block is nested
        // in the cluster variable and is exempt from the small-sample factor.
        const int64_t Kpre = static_cast<int64_t>(nT - ncomp) + P;
        const double Gc = static_cast<double>(NC);
        const double corr = (Gc > 1 && n0 > Kpre)
                                ? (Gc / (Gc - 1.0)) * (static_cast<double>(n0 - 1) /
                                                       static_cast<double>(n0 - Kpre))
                                : 1.0;
        std::vector<double> V(static_cast<size_t>(P) * P);
        la::sandwich(A.data(), meat.data(), P, corr, V.data());
        for (int k = 1; k <= P; ++k) {
            DidImputeCoef row;
            row.name = "pre" + std::to_string(k);
            row.horizon = k;
            row.is_pretrend = true;
            row.n_cells = ncell[static_cast<size_t>(k - 1)];
            row.n_cells_all = row.n_cells;
            row.estimate = pi[static_cast<size_t>(k - 1)];
            const double vv = V[static_cast<size_t>(k - 1) * P + (k - 1)];
            row.se = vv > 0 ? std::sqrt(vv) : 0.0;
            row.z = row.se > 0 ? row.estimate / row.se : std::nan("");
            row.p = row.se > 0 ? std::erfc(std::fabs(row.z) * 0.7071067811865476) : std::nan("");
            row.ci_lo = row.estimate - zcrit * row.se;
            row.ci_hi = row.estimate + zcrit * row.se;
            R.coefs.push_back(row);
        }
    }
    return R;
}

}  // namespace cecon
