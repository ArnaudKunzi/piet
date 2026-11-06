// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "zicount.h"

#include "core/threads.h"

namespace cecon {

namespace {

// One Newton-Raphson maximization of zi::ll_grad from `theta`, following the
// tobit.h recipe verbatim: analytic gradient, central-difference numerical
// Hessian, symmetrize, spd_inverse(-H) for both the step and the final VCE, and a
// 40-halving backtracking line search so a full Newton step that would leave the
// concave region is walked back instead of accepted.
//
// tol is TIGHTER than tobit's 1e-9: the zinb SEs are pinned to rtol 1e-5 against
// the Stata oracle and lnα is the slowest-moving coordinate, so the loop is asked
// for a relative log-likelihood change of 1e-11 (≈1e-7 absolute at LL≈-10⁴).
// Quadratic convergence makes the extra iterations free — measured 4 vs 3 for zip
// and 6 vs 5 for zinb on the P7 fixture.
struct NewtonFit {
    std::vector<double> theta;
    std::vector<double> vcov;  // np*np, (-H)^{-1} at the converged θ
    double ll = 0;
    int iters = 0;
    bool converged = false;
    bool ok = false;
};

NewtonFit zi_newton(const double* X, int64_t n, int k, const double* y, bool nb,
                    std::vector<double> theta, int maxit = 200, double tol = 1e-11) {
    NewtonFit R;
    const int np = k + (nb ? 2 : 1);
    std::vector<double> grad(static_cast<size_t>(np));
    std::vector<double> H(static_cast<size_t>(np) * np);
    // Scratch shared by every Newton iteration: negH/step/cand are fully rewritten
    // before any read, so reusing one allocation across iterations is byte-identical
    // to a fresh vector each time.
    std::vector<double> negH(static_cast<size_t>(np) * np), step(static_cast<size_t>(np)),
        cand(static_cast<size_t>(np));
    // The numeric Hessian's columns are independent — column j reads only `th` and j
    // and writes only H[:, j], with no accumulator shared between columns — so running
    // them concurrently yields the identical matrix. These 2·np full-data gradient
    // passes are what a Newton step here actually costs.
    auto hessian_at = [&](const std::vector<double>& th) {
        parallel_for(np, [&](int64_t jj) {
            const int j = static_cast<int>(jj);
            thread_local std::vector<double> gp, gm, tp, tm;
            gp.resize(static_cast<size_t>(np));
            gm.resize(static_cast<size_t>(np));
            double h = 1e-6 * (std::fabs(th[static_cast<size_t>(j)]) + 1.0);
            tp = th;
            tm = th;
            tp[static_cast<size_t>(j)] += h;
            tm[static_cast<size_t>(j)] -= h;
            zi::ll_grad(X, n, k, y, nb, tp, &gp);
            zi::ll_grad(X, n, k, y, nb, tm, &gm);
            for (int i = 0; i < np; ++i)
                H[static_cast<size_t>(i) * np + j] =
                    (gp[static_cast<size_t>(i)] - gm[static_cast<size_t>(i)]) / (2 * h);
        }, threads_for_work(static_cast<int64_t>(np) * n));
        for (int i = 0; i < np; ++i)
            for (int j = i + 1; j < np; ++j) {
                double s = 0.5 * (H[static_cast<size_t>(i) * np + j] + H[static_cast<size_t>(j) * np + i]);
                H[static_cast<size_t>(i) * np + j] = H[static_cast<size_t>(j) * np + i] = s;
            }
    };

    double prevLL = -1e300;
    int it = 0;
    for (; it < maxit; ++it) {
        double LL = zi::ll_grad(X, n, k, y, nb, theta, &grad);
        if (!std::isfinite(LL)) return R;
        hessian_at(theta);
        for (size_t i = 0; i < negH.size(); ++i) negH[i] = -H[i];
        if (la::spd_inverse(negH.data(), np) != 0) break;  // not concave here — stop, report where we are
        la::matvec(negH.data(), np, grad.data(), step.data());  // step = (-H)^{-1} grad
        double t = 1.0;
        double candLL = LL;
        for (int bt = 0; bt < 40; ++bt) {
            for (int i = 0; i < np; ++i)
                cand[static_cast<size_t>(i)] = theta[static_cast<size_t>(i)] + t * step[static_cast<size_t>(i)];
            candLL = zi::ll_grad(X, n, k, y, nb, cand, nullptr);
            if (std::isfinite(candLL) && candLL >= LL) break;
            t *= 0.5;
        }
        theta = cand;
        if (std::fabs(candLL - prevLL) < tol * (std::fabs(prevLL) + 1e-9)) {
            prevLL = candLL;
            ++it;
            R.converged = true;
            break;
        }
        prevLL = candLL;
    }
    R.iters = it;
    R.ll = zi::ll_grad(X, n, k, y, nb, theta, &grad);
    hessian_at(theta);
    std::vector<double> V(static_cast<size_t>(np) * np);
    for (size_t i = 0; i < V.size(); ++i) V[i] = -H[i];
    if (la::spd_inverse(V.data(), np) != 0) return R;
    R.vcov = std::move(V);
    R.theta = std::move(theta);
    R.ok = true;
    return R;
}

// Index of a column that is identically 1.0 (the caller's intercept), or -1.
int const_col(const double* X, int64_t n, int k) {
    for (int c = 0; c < k; ++c) {
        bool all_one = true;
        for (int64_t i = 0; i < n; ++i)
            if (X[static_cast<int64_t>(c) * n + i] != 1.0) { all_one = false; break; }
        if (all_one) return c;
    }
    return -1;
}

// The constant-only prefit Stata prints as "Fitting constant-only model:" — a 2
// (zip) or 3 (zinb) parameter mixture on an intercept-only count equation.
//
// Moment start: the count intercept is log(mean of the POSITIVE counts) — the
// zeros are exactly what the inflation equation is there to absorb, so the
// positive part is the honest first look at μ — and γ is the logit of the excess
// zero share, i.e. the observed zero share minus the share a plain Poisson at that
// μ would already produce (exp(-ȳ⁺)). Floored at .01 so a sample with no excess
// zeros still starts at a finite γ instead of -∞.
NewtonFit zi_const_fit(int64_t n, const double* y, bool nb) {
    double sum_pos = 0;
    int64_t n_pos = 0, n_zero = 0;
    for (int64_t i = 0; i < n; ++i) {
        if (y[i] > 0) { sum_pos += y[i]; ++n_pos; }
        else ++n_zero;
    }
    double ybar_pos = (n_pos > 0) ? sum_pos / static_cast<double>(n_pos) : 1.0;
    double zero_share = (n > 0) ? static_cast<double>(n_zero) / static_cast<double>(n) : 0.0;
    double p0 = zero_share - std::exp(-ybar_pos);
    p0 = std::min(0.99, std::max(0.01, p0));
    std::vector<double> theta{std::log(std::max(ybar_pos, 1e-8)), std::log(p0 / (1.0 - p0))};
    if (nb) theta.push_back(std::log(0.1));
    std::vector<double> ones(static_cast<size_t>(n), 1.0);
    return zi_newton(ones.data(), n, 1, y, nb, theta);
}

void fill_counts(ZiResult& r, int64_t n, const double* y) {
    r.n = n;
    r.n_zero = 0;
    for (int64_t i = 0; i < n; ++i)
        if (y[i] == 0.0) ++r.n_zero;
    r.n_nonzero = n - r.n_zero;
}

}  // namespace

ZiResult zip_fit(const double* X, int64_t n, int k, const double* y) {
    ZiResult r;
    r.nb = false;
    r.k = k;
    r.np = k + 1;
    fill_counts(r, n, y);
    if (n <= 0 || k <= 0) return r;

    NewtonFit c = zi_const_fit(n, y, false);
    if (!c.ok) return r;
    r.ll_const = c.ll;

    std::vector<double> th(static_cast<size_t>(r.np), 0.0);
    int ci = const_col(X, n, k);
    if (ci >= 0) th[static_cast<size_t>(ci)] = c.theta[0];  // intercept from the prefit; slopes 0
    th[static_cast<size_t>(k)] = c.theta[1];
    NewtonFit f = zi_newton(X, n, k, y, false, th);
    if (!f.ok) return r;

    r.beta.assign(f.theta.begin(), f.theta.begin() + k);
    r.gamma0 = f.theta[static_cast<size_t>(k)];
    r.theta = f.theta;
    r.vcov = f.vcov;
    r.ll = f.ll;
    r.iters = f.iters;
    r.converged = f.converged;
    r.ok = true;
    return r;
}

ZiResult zinb_fit(const double* X, int64_t n, int k, const double* y) {
    ZiResult r;
    r.nb = true;
    r.k = k;
    r.np = k + 2;
    fill_counts(r, n, y);
    if (n <= 0 || k <= 0) return r;

    // (β, γ) warm start = the converged zip; lnα start = the constant-only zinb's
    // (a fitted dispersion for THIS sample, strictly better than a fixed guess —
    // and the prefit's own log-likelihood is the number Stata prints as the last
    // line of its "Fitting constant-only model:" chain).
    ZiResult z = zip_fit(X, n, k, y);
    if (!z.ok) return r;
    NewtonFit c = zi_const_fit(n, y, true);
    if (!c.ok) return r;
    r.ll_const = c.ll;

    std::vector<double> th(static_cast<size_t>(r.np), 0.0);
    for (int p = 0; p < k; ++p) th[static_cast<size_t>(p)] = z.beta[static_cast<size_t>(p)];
    th[static_cast<size_t>(k)] = z.gamma0;
    th[static_cast<size_t>(k) + 1] = c.theta[2];
    NewtonFit f = zi_newton(X, n, k, y, true, th);
    if (!f.ok) return r;

    r.beta.assign(f.theta.begin(), f.theta.begin() + k);
    r.gamma0 = f.theta[static_cast<size_t>(k)];
    r.lnalpha = f.theta[static_cast<size_t>(k) + 1];
    r.alpha = std::exp(r.lnalpha);
    r.theta = f.theta;
    r.vcov = f.vcov;
    r.ll = f.ll;
    r.iters = f.iters;
    r.converged = f.converged;
    r.ok = true;
    return r;
}

std::vector<double> zi_scores(const double* X, int64_t n, int k, const double* y,
                              const ZiResult& r) {
    if (!r.ok || static_cast<int>(r.theta.size()) != r.np) return {};
    std::vector<double> S;
    zi::ll_grad(X, n, k, y, r.nb, r.theta, nullptr, &S);
    return S;
}

std::vector<double> zi_robust_vcov(const double* X, int64_t n, int k, const double* y,
                                   const ZiResult& r, double factor) {
    const int np = r.np;
    if (!r.ok || static_cast<int>(r.vcov.size()) != np * np) return {};
    std::vector<double> S = zi_scores(X, n, k, y, r);
    if (S.empty()) return {};
    std::vector<double> meat(static_cast<size_t>(np) * np, 0.0);
    la::xtx(S.data(), n, np, meat.data());
    std::vector<double> V(static_cast<size_t>(np) * np, 0.0), tmp(static_cast<size_t>(np) * np, 0.0);
    la::sandwich(r.vcov.data(), meat.data(), np, factor, V.data(), tmp.data());
    return V;
}

std::vector<double> zi_cluster_vcov(const double* X, int64_t n, int k, const double* y,
                                    const ZiResult& r, const int* codes, int n_clusters,
                                    double factor) {
    const int np = r.np;
    if (!r.ok || static_cast<int>(r.vcov.size()) != np * np || n_clusters < 1 || !codes) return {};
    std::vector<double> S = zi_scores(X, n, k, y, r);
    if (S.empty()) return {};
    // Per-cluster score totals, then meat = Σ_g (Σ_{i∈g} s_i)(Σ_{i∈g} s_i)'.
    std::vector<double> G(static_cast<size_t>(n_clusters) * np, 0.0);
    for (int64_t i = 0; i < n; ++i) {
        int c = codes[i];
        if (c < 0 || c >= n_clusters) continue;
        for (int p = 0; p < np; ++p)
            G[static_cast<size_t>(c) * np + p] += S[static_cast<int64_t>(p) * n + i];
    }
    std::vector<double> meat(static_cast<size_t>(np) * np, 0.0);
    for (int g = 0; g < n_clusters; ++g) {
        const double* sg = G.data() + static_cast<size_t>(g) * np;
        for (int c = 0; c < np; ++c)
            for (int rr = 0; rr < np; ++rr)
                meat[static_cast<size_t>(c) * np + rr] += sg[c] * sg[rr];
    }
    std::vector<double> V(static_cast<size_t>(np) * np, 0.0), tmp(static_cast<size_t>(np) * np, 0.0);
    la::sandwich(r.vcov.data(), meat.data(), np, factor, V.data(), tmp.data());
    return V;
}

}  // namespace cecon
