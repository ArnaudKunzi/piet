// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// cecon — multinomial (polytomous) logit MLE, matching Stata `mlogit`.
//
// Outcomes are recoded to 0..K-1 by SORTED DISTINCT VALUE. One outcome is the
// BASE: its equation is normalized to η ≡ 0 (all its coefficients are fixed at
// zero and are not parameters), so with the remaining K−1 equations
//
//     p_im = exp(η_im) / (1 + Σ_l exp(η_il)),     η_im = x_i·β_m,  l over non-base
//     p_i,base = 1 / (1 + Σ_l exp(η_il))
//
// and LL_i = log p_{i,y_i}. The per-observation score for equation m is
// (1[y_i = m] − p_im)·x_i.
//
// θ is the (K−1) equation blocks STACKED, equation-major, non-base outcomes in
// ascending outcome order, and within a block the design's own column order (which
// for load_glm is [expanded x…, intercept]). The score-block order is the same, so
// the sandwich meat's blocks line up with the bread's — a transposed block here
// yields wrong-but-plausible cluster SEs, which is why the two orders are stated
// once and shared.
//
// Optimizer follows the house new-MLE recipe (see estimators/tobit.h): analytic
// gradient, central-difference numerical Hessian of that gradient, symmetrize,
// spd_inverse(−H) for the model-based VCE, 40-halving backtracking line search,
// maxit 200, convergence |ΔLL| < 1e-9·(|LL|+1e-9).
#pragma once
#include "core/threads.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "core/linalg.h"

namespace cecon {

struct MlogitResult {
    int n_cats = 0;              // K
    int base = 0;                // 0-based index of the base outcome among the K
    std::vector<double> beta;    // (K-1)*k, equation-major (non-base, ascending)
    std::vector<double> se;      // same layout as beta (model-based)
    std::vector<double> vcov;    // np*np, np = (K-1)*k, same layout
    std::vector<int64_t> cat_n;  // per-outcome row counts
    double ll = 0;               // log-likelihood at the optimum
    double ll_0 = 0;             // constants-only closed form Σ_m n_m·ln(n_m/n)
    int64_t n = 0;
    int iters = 0;
    bool converged = false;
    bool ok = false;
};

namespace mlg {

// Log-likelihood + analytic gradient at θ (np = (K-1)*k, equation-major). X is
// col-major n*k INCLUDING its intercept column. `cat` holds the 0-based outcome of
// each row; `eq_of_cat[c]` is the equation index of outcome c, or -1 for the base.
//
// scores: optional n*np COLUMN-major output (column p, row i at scores[p*n+i]) —
// filled with the SAME per-observation gradient contribution that is otherwise only
// summed into *grad, so la::xtx(scores, n, np, ...) gives the robust meat directly
// and summing score ROWS within a cluster first gives the cluster meat.
//
// As in tobit.h/oglm.h, the grad and scores accumulations are two INDEPENDENT
// branches that deliberately do not share an intermediate: `(*grad)[o] += c *
// X[...]` is one fused expression eligible for -ffp-contract FMA fusion, and
// hoisting the product into a shared temp forces an extra rounding step that moves
// the last bits of every downstream SE. No call site asks for both at once (the
// Newton loop passes grad only, the sandwich passes scores only).
inline double ll_grad(const double* X, int64_t n, int k, const int* cat, int K,
                      const std::vector<int>& eq_of_cat, const std::vector<double>& theta,
                      std::vector<double>* grad, std::vector<double>* scores = nullptr) {
    const int neq = K - 1;
    const int np = neq * k;
    double LL = 0;
    if (grad) std::fill(grad->begin(), grad->end(), 0.0);
    if (scores) scores->assign(static_cast<size_t>(n) * np, 0.0);
    double* S = scores ? scores->data() : nullptr;
    std::vector<double> eta(static_cast<size_t>(neq)), p(static_cast<size_t>(neq));
    for (int64_t i = 0; i < n; ++i) {
        double emax = 0.0;  // the base equation's η ≡ 0 is always in the pool
        for (int e = 0; e < neq; ++e) {
            double v = 0;
            const double* b = theta.data() + static_cast<size_t>(e) * k;
            for (int c = 0; c < k; ++c) v += X[static_cast<int64_t>(c) * n + i] * b[c];
            eta[static_cast<size_t>(e)] = v;
            if (v > emax) emax = v;
        }
        double denom = std::exp(-emax);  // = exp(0 − emax), the base equation
        for (int e = 0; e < neq; ++e) {
            double ex = std::exp(eta[static_cast<size_t>(e)] - emax);
            p[static_cast<size_t>(e)] = ex;
            denom += ex;
        }
        for (int e = 0; e < neq; ++e) p[static_cast<size_t>(e)] /= denom;
        const int ei = eq_of_cat[static_cast<size_t>(cat[i])];  // -1 for the base outcome
        double lin = (ei >= 0) ? eta[static_cast<size_t>(ei)] : 0.0;
        LL += lin - emax - std::log(denom);
        if (grad) {
            for (int e = 0; e < neq; ++e) {
                double c = ((e == ei) ? 1.0 : 0.0) - p[static_cast<size_t>(e)];
                double* g = grad->data() + static_cast<size_t>(e) * k;
                for (int q = 0; q < k; ++q) g[q] += c * X[static_cast<int64_t>(q) * n + i];
            }
        }
        if (S) {
            for (int e = 0; e < neq; ++e) {
                double c = ((e == ei) ? 1.0 : 0.0) - p[static_cast<size_t>(e)];
                for (int q = 0; q < k; ++q)
                    S[(static_cast<int64_t>(e) * k + q) * n + i] = c * X[static_cast<int64_t>(q) * n + i];
            }
        }
    }
    return LL;
}

// Numerical (central-difference) Hessian of ll_grad's analytic gradient, symmetrized.
inline void hessian(const double* X, int64_t n, int k, const int* cat, int K,
                    const std::vector<int>& eq_of_cat, const std::vector<double>& theta,
                    std::vector<double>& H) {
    const int np = static_cast<int>(theta.size());
    H.assign(static_cast<size_t>(np) * np, 0.0);
    // Column j reads only theta and j and writes only H[:, j] -- no accumulator is
    // shared between columns -- so evaluating them concurrently returns the identical
    // matrix, with no summation order to reorder. These 2*np full-data gradient passes
    // are what a Newton step costs here. (PERFSWEEP H: same class as ivml/zicount.)
    parallel_for(np, [&](int64_t jj) {
        const int j = static_cast<int>(jj);
        thread_local std::vector<double> gp, gm, tp, tm;
        gp.resize(static_cast<size_t>(np));
        gm.resize(static_cast<size_t>(np));
        double h = 1e-6 * (std::fabs(theta[static_cast<size_t>(j)]) + 1.0);
        tp = theta; tm = theta;
        tp[static_cast<size_t>(j)] += h; tm[static_cast<size_t>(j)] -= h;
        ll_grad(X, n, k, cat, K, eq_of_cat, tp, &gp);
        ll_grad(X, n, k, cat, K, eq_of_cat, tm, &gm);
        for (int i = 0; i < np; ++i)
            H[static_cast<size_t>(i) * np + j] =
                (gp[static_cast<size_t>(i)] - gm[static_cast<size_t>(i)]) / (2 * h);
    }, threads_for_work(static_cast<int64_t>(np) * n));
    for (int i = 0; i < np; ++i)
        for (int j = i + 1; j < np; ++j) {
            double s = 0.5 * (H[static_cast<size_t>(i) * np + j] + H[static_cast<size_t>(j) * np + i]);
            H[static_cast<size_t>(i) * np + j] = H[static_cast<size_t>(j) * np + i] = s;
        }
}

}  // namespace mlg

// X is col-major n*k INCLUDING the intercept column. `base` is the 0-based index of
// the base outcome among the K sorted distinct outcome values.
inline MlogitResult mlogit_fit(const double* X, int64_t n, int k, const int* cat, int K, int base) {
    MlogitResult r;
    r.n = n;
    r.n_cats = K;
    r.base = base;
    if (K < 2 || n <= 0 || base < 0 || base >= K) return r;
    const int neq = K - 1, np = neq * k;

    r.cat_n.assign(static_cast<size_t>(K), 0);
    for (int64_t i = 0; i < n; ++i) ++r.cat_n[static_cast<size_t>(cat[i])];
    for (int m = 0; m < K; ++m) {
        double nm = static_cast<double>(r.cat_n[static_cast<size_t>(m)]);
        if (nm > 0) r.ll_0 += nm * std::log(nm / static_cast<double>(n));
    }
    // Outcome -> equation index (ascending outcome order, base excluded).
    std::vector<int> eq_of_cat(static_cast<size_t>(K), -1);
    for (int m = 0, e = 0; m < K; ++m)
        if (m != base) eq_of_cat[static_cast<size_t>(m)] = e++;

    // Start: ALL ZEROS. Every p_im is then 1/K, so LL = −n·ln(K) — the value Stata
    // prints as its iteration 0.
    std::vector<double> theta(static_cast<size_t>(np), 0.0);
    std::vector<double> grad(static_cast<size_t>(np)), H, negH, step(static_cast<size_t>(np));
    double prevLL = -1e300;
    int it = 0;
    for (; it < 200; ++it) {
        double LL = mlg::ll_grad(X, n, k, cat, K, eq_of_cat, theta, &grad);
        mlg::hessian(X, n, k, cat, K, eq_of_cat, theta, H);
        negH.assign(H.size(), 0.0);
        for (size_t i = 0; i < H.size(); ++i) negH[i] = -H[i];
        if (la::spd_inverse(negH.data(), np) != 0) break;
        la::matvec(negH.data(), np, grad.data(), step.data());  // step = (−H)^{-1} grad
        double t = 1.0;
        std::vector<double> cand(static_cast<size_t>(np));
        double candLL = LL;
        for (int bt = 0; bt < 40; ++bt) {
            for (int i = 0; i < np; ++i)
                cand[static_cast<size_t>(i)] = theta[static_cast<size_t>(i)] + t * step[static_cast<size_t>(i)];
            candLL = mlg::ll_grad(X, n, k, cat, K, eq_of_cat, cand, nullptr);
            if (std::isfinite(candLL) && candLL >= LL) break;
            t *= 0.5;
        }
        theta = cand;
        if (std::fabs(candLL - prevLL) < 1e-9 * (std::fabs(prevLL) + 1e-9)) {
            prevLL = candLL; ++it; r.converged = true; break;
        }
        prevLL = candLL;
    }
    r.iters = it;
    r.ll = mlg::ll_grad(X, n, k, cat, K, eq_of_cat, theta, &grad);
    mlg::hessian(X, n, k, cat, K, eq_of_cat, theta, H);
    std::vector<double> V(H.size());
    for (size_t i = 0; i < V.size(); ++i) V[i] = -H[i];
    if (la::spd_inverse(V.data(), np) != 0) return r;
    r.vcov = V;
    r.beta = theta;
    r.se.resize(static_cast<size_t>(np));
    for (int c = 0; c < np; ++c)
        r.se[static_cast<size_t>(c)] = std::sqrt(std::max(0.0, V[static_cast<size_t>(c) * np + c]));
    r.ok = true;
    return r;
}

// Outcome -> equation index for a fitted model (-1 for the base outcome).
inline std::vector<int> mlogit_eq_of_cat(const MlogitResult& r) {
    std::vector<int> eq_of_cat(static_cast<size_t>(r.n_cats), -1);
    for (int m = 0, e = 0; m < r.n_cats; ++m)
        if (m != r.base) eq_of_cat[static_cast<size_t>(m)] = e++;
    return eq_of_cat;
}

// Per-observation score rows at the converged θ (n*np, col-major) — the sandwich
// meat's raw material for vce(robust) / vce(cluster). Parameter order matches
// r.vcov exactly (equation-major, design column order within an equation).
inline std::vector<double> mlogit_scores(const double* X, int64_t n, int k, const int* cat,
                                         const MlogitResult& r) {
    std::vector<double> S;
    std::vector<int> eq_of_cat = mlogit_eq_of_cat(r);
    mlg::ll_grad(X, n, k, cat, r.n_cats, eq_of_cat, r.beta, nullptr, &S);
    return S;
}

}  // namespace cecon
