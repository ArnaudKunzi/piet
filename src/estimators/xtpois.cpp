// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "estimators/xtpois.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "core/linalg.h"

namespace cecon {

namespace {

struct Group {
    int64_t start, end;  // [start, end) into the working arrays (sorted by group)
};

}  // namespace

XtpoisFeResult xtpoisson_fe(const double* X_in, int64_t n_obs, int k,
                            const double* y_in, const double* ivar_in,
                            const double* offset_in,
                            int maxit, double tol) {
    XtpoisFeResult R;
    R.k = k;

    // ---- 1. Sort rows by ivar (stable ordering on first appearance) ----
    // Build perm so that perm[r] is the original row index, ordered such that
    // all rows with the same ivar are contiguous (in first-appearance order
    // of distinct ivar values).
    std::unordered_map<double, int> idx_of_ivar;
    idx_of_ivar.reserve(static_cast<size_t>(n_obs / 4 + 1));
    std::vector<int> grp_of_row(static_cast<size_t>(n_obs));
    std::vector<std::vector<int64_t>> rows_per_grp;
    rows_per_grp.reserve(8192);
    for (int64_t r = 0; r < n_obs; ++r) {
        double v = ivar_in[r];
        auto it = idx_of_ivar.find(v);
        int gi;
        if (it == idx_of_ivar.end()) {
            gi = static_cast<int>(rows_per_grp.size());
            idx_of_ivar[v] = gi;
            rows_per_grp.emplace_back();
        } else {
            gi = it->second;
        }
        grp_of_row[static_cast<size_t>(r)] = gi;
        rows_per_grp[static_cast<size_t>(gi)].push_back(r);
    }

    // ---- 2. Drop groups with sum(y) == 0 or size == 1 ----
    std::vector<int> keep_grp;
    keep_grp.reserve(rows_per_grp.size());
    int64_t n_dropped_zero = 0, n_dropped_size = 0;
    for (size_t gi = 0; gi < rows_per_grp.size(); ++gi) {
        const auto& rows = rows_per_grp[gi];
        if (rows.size() < 2) { ++n_dropped_size; continue; }
        double sy = 0;
        for (int64_t r : rows) sy += y_in[r];
        if (sy <= 0) { ++n_dropped_zero; continue; }
        keep_grp.push_back(static_cast<int>(gi));
    }
    R.n_dropped_zero = n_dropped_zero;
    R.n_dropped_size = n_dropped_size;
    R.n_groups = static_cast<int64_t>(keep_grp.size());
    if (!keep_grp.empty()) {
        int64_t tot = 0;
        R.g_min = -1;
        for (int gi : keep_grp) {
            const int64_t T = static_cast<int64_t>(rows_per_grp[static_cast<size_t>(gi)].size());
            R.g_min = R.g_min < 0 ? T : std::min(R.g_min, T);
            R.g_max = std::max(R.g_max, T);
            tot += T;
        }
        R.g_avg = static_cast<double>(tot) / static_cast<double>(keep_grp.size());
    }

    // Pack working arrays in group-contiguous order.
    std::vector<Group> groups;
    groups.reserve(keep_grp.size());
    int64_t n_w = 0;
    for (int gi : keep_grp) n_w += static_cast<int64_t>(rows_per_grp[static_cast<size_t>(gi)].size());
    R.n_obs = n_w;

    std::vector<double> X(static_cast<size_t>(n_w) * k);  // column-major n_w * k
    std::vector<double> y(static_cast<size_t>(n_w));
    std::vector<double> off(static_cast<size_t>(n_w), 0.0);

    int64_t cursor = 0;
    for (int gi : keep_grp) {
        const auto& rows = rows_per_grp[static_cast<size_t>(gi)];
        Group g{cursor, cursor + static_cast<int64_t>(rows.size())};
        groups.push_back(g);
        for (int64_t r : rows) {
            y[static_cast<size_t>(cursor)] = y_in[r];
            if (offset_in) off[static_cast<size_t>(cursor)] = offset_in[r];
            for (int c = 0; c < k; ++c) {
                X[static_cast<size_t>(c) * n_w + cursor] = X_in[static_cast<int64_t>(c) * n_obs + r];
            }
            ++cursor;
        }
    }

    // ---- 3. Newton-Raphson on the conditional log-likelihood ----
    R.beta.assign(static_cast<size_t>(k), 0.0);
    std::vector<double> xb(static_cast<size_t>(n_w), 0.0);
    std::vector<double> p(static_cast<size_t>(n_w), 0.0);
    std::vector<double> score(static_cast<size_t>(k), 0.0);
    std::vector<double> negH(static_cast<size_t>(k) * k, 0.0);
    std::vector<double> negH_inv(static_cast<size_t>(k) * k, 0.0);
    std::vector<double> step(static_cast<size_t>(k), 0.0);
    std::vector<double> xbar(static_cast<size_t>(k), 0.0);
    std::vector<double> dx(static_cast<size_t>(k), 0.0);
    std::vector<double> per_group_score;  // for robust SE; n_groups * k

    double prev_ll = -1e300;
    bool converged = false;
    for (int iter = 0; iter < maxit; ++iter) {
        // 3a. xb_it = X_it β + offset_it
        for (int64_t i = 0; i < n_w; ++i) {
            double s = off[static_cast<size_t>(i)];
            for (int c = 0; c < k; ++c) s += X[static_cast<size_t>(c) * n_w + i] * R.beta[static_cast<size_t>(c)];
            xb[static_cast<size_t>(i)] = s;
        }

        // 3b. For each group: p_it (softmax over t), score, Hessian, log-lik
        std::fill(score.begin(), score.end(), 0.0);
        std::fill(negH.begin(), negH.end(), 0.0);
        double ll = 0.0;
        for (const auto& g : groups) {
            // softmax over rows in this group (numerically stable)
            double m = -1e300;
            for (int64_t i = g.start; i < g.end; ++i)
                if (xb[static_cast<size_t>(i)] > m) m = xb[static_cast<size_t>(i)];
            double denom = 0.0;
            for (int64_t i = g.start; i < g.end; ++i) {
                double e = std::exp(xb[static_cast<size_t>(i)] - m);
                p[static_cast<size_t>(i)] = e;
                denom += e;
            }
            double log_denom = std::log(denom) + m;
            double n_i = 0.0;
            for (int64_t i = g.start; i < g.end; ++i) {
                p[static_cast<size_t>(i)] /= denom;
                n_i += y[static_cast<size_t>(i)];
                // Add y_it * log p_it to log-lik (drop constants).
                ll += y[static_cast<size_t>(i)] * (xb[static_cast<size_t>(i)] - log_denom);
            }
            // x̄_i = Σ_t p_it x_t  (k-vector)
            for (int c = 0; c < k; ++c) {
                double s = 0;
                for (int64_t i = g.start; i < g.end; ++i)
                    s += p[static_cast<size_t>(i)] * X[static_cast<size_t>(c) * n_w + i];
                xbar[static_cast<size_t>(c)] = s;
            }
            // score_i_c = Σ_t (y_it - n_i p_it)(x_itc - x̄_ic)
            // A11 sibling of the BUG-135 xtlogit fix: the residuals sum to EXACTLY zero
            // within a panel (Σ_t y_it = n_i and Σ_t p_it = 1), so subtracting the
            // conditional mean x̄ leaves the score unchanged in exact arithmetic while
            // removing x̄ × (the rounding residual of Σ_t r_t) in floating point. That
            // product is pure noise and, on an offset covariate, large enough to hold the
            // Newton step above tol. The Hessian below was ALREADY written in centred
            // form ((x−x̄)(x−x̄)'), so this makes the two halves consistent.
            for (int c = 0; c < k; ++c) {
                double s = 0;
                for (int64_t i = g.start; i < g.end; ++i) {
                    double r = y[static_cast<size_t>(i)]
                             - n_i * p[static_cast<size_t>(i)];
                    s += r * (X[static_cast<size_t>(c) * n_w + i] - xbar[static_cast<size_t>(c)]);
                }
                score[static_cast<size_t>(c)] += s;
            }
            // -H_g = n_i * Σ_t p_t (x_t - x̄)(x_t - x̄)'
            // Accumulate by symmetric outer product into negH (k*k column-major).
            for (int64_t i = g.start; i < g.end; ++i) {
                double w = n_i * p[static_cast<size_t>(i)];
                if (w == 0) continue;
                for (int c = 0; c < k; ++c)
                    dx[static_cast<size_t>(c)] = X[static_cast<size_t>(c) * n_w + i]
                                                - xbar[static_cast<size_t>(c)];
                for (int c = 0; c < k; ++c) {
                    double wd = w * dx[static_cast<size_t>(c)];
                    double* col = negH.data() + static_cast<size_t>(c) * k;
                    for (int r = 0; r < k; ++r)
                        col[static_cast<size_t>(r)] += wd * dx[static_cast<size_t>(r)];
                }
            }
        }

        // 3c. Solve (-H) step = score, β += step.
        std::copy(negH.begin(), negH.end(), negH_inv.begin());
        int info = la::spd_inverse(negH_inv.data(), k);
        if (info != 0) {
            R.converged = false;
            R.iters = iter;
            return R;
        }
        la::matvec(negH_inv.data(), k, score.data(), step.data());
        double smag = 0;
        for (int c = 0; c < k; ++c) {
            R.beta[static_cast<size_t>(c)] += step[static_cast<size_t>(c)];
            smag += std::fabs(step[static_cast<size_t>(c)]);
        }
        R.iters = iter + 1;
        if (std::fabs(ll - prev_ll) < tol * (std::fabs(ll) + 1.0) && smag < tol) {
            converged = true;
            // Final pass to compute log-lik constants too.
            // L_const = Σ_i [logΓ(n_i + 1) - Σ_t logΓ(y_it + 1)]
            double cst = 0;
            for (const auto& g : groups) {
                double n_i = 0;
                for (int64_t i = g.start; i < g.end; ++i) n_i += y[static_cast<size_t>(i)];
                cst += std::lgamma(n_i + 1.0);
                for (int64_t i = g.start; i < g.end; ++i)
                    cst -= std::lgamma(y[static_cast<size_t>(i)] + 1.0);
            }
            R.loglik = ll + cst;
            break;
        }
        prev_ll = ll;
    }
    R.converged = converged;
    if (!converged) {
        // Use last achieved ll for return; not strictly correct but informative.
        R.loglik = prev_ll;
    }

    // ---- 4. IID and robust VCE ----
    // Recompute -H, per-group scores at the final β.
    for (int64_t i = 0; i < n_w; ++i) {
        double s = off[static_cast<size_t>(i)];
        for (int c = 0; c < k; ++c) s += X[static_cast<size_t>(c) * n_w + i] * R.beta[static_cast<size_t>(c)];
        xb[static_cast<size_t>(i)] = s;
    }
    std::fill(negH.begin(), negH.end(), 0.0);
    per_group_score.assign(static_cast<size_t>(R.n_groups) * k, 0.0);
    size_t gi_idx = 0;
    for (const auto& g : groups) {
        double m = -1e300;
        for (int64_t i = g.start; i < g.end; ++i)
            if (xb[static_cast<size_t>(i)] > m) m = xb[static_cast<size_t>(i)];
        double denom = 0;
        for (int64_t i = g.start; i < g.end; ++i) {
            double e = std::exp(xb[static_cast<size_t>(i)] - m);
            p[static_cast<size_t>(i)] = e;
            denom += e;
        }
        double n_i = 0;
        for (int64_t i = g.start; i < g.end; ++i) {
            p[static_cast<size_t>(i)] /= denom;
            n_i += y[static_cast<size_t>(i)];
        }
        for (int c = 0; c < k; ++c) {
            double s = 0;
            for (int64_t i = g.start; i < g.end; ++i)
                s += p[static_cast<size_t>(i)] * X[static_cast<size_t>(c) * n_w + i];
            xbar[static_cast<size_t>(c)] = s;
        }
        // per-group score — centred, same argument as the Newton score above.
        for (int c = 0; c < k; ++c) {
            double s = 0;
            for (int64_t i = g.start; i < g.end; ++i) {
                double r = y[static_cast<size_t>(i)] - n_i * p[static_cast<size_t>(i)];
                s += r * (X[static_cast<size_t>(c) * n_w + i] - xbar[static_cast<size_t>(c)]);
            }
            per_group_score[gi_idx * k + static_cast<size_t>(c)] = s;
        }
        ++gi_idx;
        for (int64_t i = g.start; i < g.end; ++i) {
            double w = n_i * p[static_cast<size_t>(i)];
            if (w == 0) continue;
            for (int c = 0; c < k; ++c)
                dx[static_cast<size_t>(c)] = X[static_cast<size_t>(c) * n_w + i]
                                            - xbar[static_cast<size_t>(c)];
            for (int c = 0; c < k; ++c) {
                double wd = w * dx[static_cast<size_t>(c)];
                double* col = negH.data() + static_cast<size_t>(c) * k;
                for (int rr = 0; rr < k; ++rr)
                    col[static_cast<size_t>(rr)] += wd * dx[static_cast<size_t>(rr)];
            }
        }
    }
    std::copy(negH.begin(), negH.end(), negH_inv.begin());
    if (la::spd_inverse(negH_inv.data(), k) != 0) {
        R.converged = false;
        return R;
    }
    // IID SE = sqrt(diag((-H)^{-1}))
    R.V_iid = negH_inv;
    R.se_iid.assign(static_cast<size_t>(k), 0.0);
    for (int c = 0; c < k; ++c)
        R.se_iid[static_cast<size_t>(c)] =
            std::sqrt(std::max(0.0, negH_inv[static_cast<size_t>(c) * k + c]));

    // G = Σ_i s_i s_i'    (k*k symmetric, column-major)
    std::vector<double> G(static_cast<size_t>(k) * k, 0.0);
    for (int64_t gi = 0; gi < R.n_groups; ++gi) {
        for (int c = 0; c < k; ++c) {
            double sc = per_group_score[static_cast<size_t>(gi) * k + c];
            for (int r = 0; r < k; ++r)
                G[static_cast<size_t>(c) * k + r] += sc * per_group_score[static_cast<size_t>(gi) * k + r];
        }
    }
    // Stata's xtpoisson, fe with vce(robust) applies NO cluster small-sample
    // correction (it implements the Wooldridge 1999 formula as-is). This is
    // different from cluster-robust in regress / reghdfe (which use M/(M-1) *
    // (N-1)/(N-k)). We follow Stata here so SEs match the golden.
    double q = 1.0;

    // V_robust = (-H)^{-1} * G * (-H)^{-1} * q
    std::vector<double> tmp(static_cast<size_t>(k) * k, 0.0);
    // tmp = negH_inv * G
    for (int c = 0; c < k; ++c)
        for (int r = 0; r < k; ++r) {
            double s = 0;
            for (int j = 0; j < k; ++j)
                s += negH_inv[static_cast<size_t>(j) * k + r] * G[static_cast<size_t>(c) * k + j];
            tmp[static_cast<size_t>(c) * k + r] = s;
        }
    std::vector<double> Vr(static_cast<size_t>(k) * k, 0.0);
    for (int c = 0; c < k; ++c)
        for (int r = 0; r < k; ++r) {
            double s = 0;
            for (int j = 0; j < k; ++j)
                s += tmp[static_cast<size_t>(j) * k + r] * negH_inv[static_cast<size_t>(c) * k + j];
            Vr[static_cast<size_t>(c) * k + r] = q * s;
        }
    R.V_robust = Vr;
    R.se_robust.assign(static_cast<size_t>(k), 0.0);
    for (int c = 0; c < k; ++c)
        R.se_robust[static_cast<size_t>(c)] =
            std::sqrt(std::max(0.0, Vr[static_cast<size_t>(c) * k + c]));

    return R;
}

}  // namespace cecon
