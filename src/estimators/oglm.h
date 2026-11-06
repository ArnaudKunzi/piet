// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// cecon — ordered logit / ordered probit MLE, matching Stata `ologit` / `oprobit`.
//
// The outcome is recoded to categories 0..J-1 by SORTED DISTINCT VALUE (Stata's
// own rule: the cutpoints are indexed by the ordering of the observed levels, not
// by their numeric magnitude). With cutpoints κ_0 < … < κ_{J-2} and the linear
// index xβ (NO intercept — it is not identified against the cutpoints), category j
// has probability
//
//     P_ij = F(κ_j − xβ) − F(κ_{j−1} − xβ),      F(κ_{−1}−·) ≡ 0, F(κ_{J−1}−·) ≡ 1
//
// so LL_i = log P_ij. F/f are the logistic CDF/pdf (f = F(1−F)) or the standard
// normal CDF/pdf. The cutpoints are carried DIRECTLY (not as an increment
// parametrization): Stata prints /cut1 /cut2 with standard errors in that metric,
// and a delta parametrization would report different cut SEs.
//
// Ordering (κ_0 < κ_1 < …) is maintained without a constraint: a candidate θ that
// crosses two cutpoints drives some P_ij to ≤ 0, whose clamped log is a very large
// negative number, so the backtracking line search rejects the step.
//
// Optimizer: Stata's own iterate (BUG-1550). ologit/oprobit report ml's iterate,
// not the exact MLE: StataBE 19.5 stops `ologit yo x1 x2` at iteration 4 with
// /cut1 -.091028014590738324, 1.2e-8 short of the MLE this file used to converge
// to. oglm_fit runs ml's path from Stata's start (β = 0, κ = F^{-1} of the
// cumulative shares, iteration 0's ll = ll_0): Newton-Raphson on the analytic
// Hessian, a full step, halved toward the old point while it does not improve ll,
// else extended by 1/8, 1/4, ... of the step while each extension improves; stop
// when (mreldif(θ) <= 1e-6 or reldif(ll) <= 1e-7) and g·inv(−H)·g' < 1e-5 at the
// new point (ml_max.ado defaults; the same emulation as logit.h's binary_ml_fit).
// Every iterate of that ologit fit, and oprobit's final one, match StataBE's
// iterate(k) captures to 5e-15. V = inv(−H) at the final θ.
#pragma once
#include "core/threads.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "core/distributions.h"
#include "core/linalg.h"

namespace cecon {

enum class OrdLink { Logit, Probit };

struct OglmResult {
    std::vector<double> beta;    // k slopes
    std::vector<double> se;      // k slope SEs (model-based / observed information)
    std::vector<double> cuts;    // J-1 cutpoints, ascending
    std::vector<double> se_cut;  // J-1 cutpoint SEs (same metric as `cuts`)
    std::vector<double> vcov;    // np*np, np = k + (J-1); slopes first, then cutpoints
    std::vector<int64_t> cat_n;  // per-category row counts
    double ll = 0;               // log-likelihood at the optimum
    double ll_0 = 0;             // cutpoints-only (β=0) log-likelihood = Σ_j n_j·ln(n_j/n)
    int64_t n = 0;
    int n_cats = 0;
    int iters = 0;
    bool converged = false;
    bool ok = false;
};

namespace ogl {

inline double ncdf(double x) { return 0.5 * std::erfc(-x * 0.70710678118654752440); }
inline double npdf(double x) { return 0.39894228040143267794 * std::exp(-0.5 * x * x); }
// Logistic CDF, branch-split so neither exp() overflows.
inline double lcdf(double x) {
    if (x >= 0) return 1.0 / (1.0 + std::exp(-x));
    double e = std::exp(x);
    return e / (1.0 + e);
}

// Φ^{-1}(p) built on the shared z_critical (core/distributions.h) rather than a
// second copy of Acklam's coefficients — z_critical(α) is the upper 1−α/2 normal
// quantile, so the two-sided α naming quantile p is 2(1−p) above the median and
// mirrors below it. Used only for ordered-probit starting cutpoints.
inline double norm_ppf(double p) {
    if (p <= 0.0 || p >= 1.0) return 0.0;
    if (p == 0.5) return 0.0;
    if (p > 0.5) return z_critical(2.0 * (1.0 - p));
    return -z_critical(2.0 * p);
}

// Log-likelihood + analytic gradient at θ = (β[0..k-1], κ[0..J-2]). X is col-major
// n*k WITHOUT an intercept column. `cat` holds the 0-based category of each row.
//
// scores: optional n*np COLUMN-major output (same layout as X — column p, row i at
// scores[p*n+i]); when non-null it is filled with the SAME per-observation gradient
// contribution that would otherwise only be summed into *grad, so la::xtx(scores,
// n, np, ...) gives the robust meat Σ_i s_i s_i' directly (and summing score ROWS
// within a cluster first gives the cluster meat).
//
// As in tobit.h, the grad and scores accumulations are written as two INDEPENDENT
// branches that deliberately do not share an intermediate: `(*grad)[p] += c *
// X[...]` is one fused expression eligible for -ffp-contract FMA fusion, and
// hoisting the product into a shared temp forces an extra rounding step that moves
// the last bits of every downstream SE. No call site here ever asks for both at
// once (the Newton loop passes grad only, the sandwich passes scores only), so the
// duplicated recompute costs nothing in practice.
inline double ll_grad(const double* X, int64_t n, int k, const int* cat, int J, OrdLink link,
                      const std::vector<double>& theta, std::vector<double>* grad,
                      std::vector<double>* scores = nullptr) {
    const int nc = J - 1;  // number of cutpoints
    const int np = k + nc;
    // LL is a Neumaier-compensated sum: oglm_fit's ml step rules compare it, and a
    // plain sum's rounding on a large sample exceeds its last changes near the maximum
    // (the nbreg.cpp LlSum note, BUG-1550).
    double LL = 0, LLc = 0;
    if (grad) std::fill(grad->begin(), grad->end(), 0.0);
    if (scores) scores->assign(static_cast<size_t>(n) * np, 0.0);
    double* S = scores ? scores->data() : nullptr;
    for (int64_t i = 0; i < n; ++i) {
        double xb = 0;
        for (int p = 0; p < k; ++p) xb += X[static_cast<int64_t>(p) * n + i] * theta[p];
        const int j = cat[i];
        // f1/f2 stay 0 at the ±∞ boundaries (j == 0 has no lower cut, j == J-1 no
        // upper cut), which is exactly f(±∞) = 0 — no special-casing downstream.
        double F1 = 0.0, f1 = 0.0, F2 = 1.0, f2 = 0.0;
        if (link == OrdLink::Logit) {
            if (j > 0) { double a1 = theta[k + j - 1] - xb; F1 = lcdf(a1); f1 = F1 * (1.0 - F1); }
            if (j < nc) { double a2 = theta[k + j] - xb; F2 = lcdf(a2); f2 = F2 * (1.0 - F2); }
        } else {
            if (j > 0) { double a1 = theta[k + j - 1] - xb; F1 = ncdf(a1); f1 = npdf(a1); }
            if (j < nc) { double a2 = theta[k + j] - xb; F2 = ncdf(a2); f2 = npdf(a2); }
        }
        double P = F2 - F1;
        // A cut-ordering violation lands here; log() of the clamp is ≈ −690 per
        // row, so the line search rejects the step instead of carrying a NaN into
        // the Hessian.
        if (P < 1e-300) P = 1e-300;
        const double lp = std::log(P), tsum = LL + lp;
        LLc += (std::fabs(LL) >= std::fabs(lp)) ? (LL - tsum) + lp : (lp - tsum) + LL;
        LL = tsum;
        if (grad) {
            double c = (f1 - f2) / P;
            for (int p = 0; p < k; ++p) (*grad)[p] += c * X[static_cast<int64_t>(p) * n + i];
            if (j > 0) (*grad)[k + j - 1] += -f1 / P;
            if (j < nc) (*grad)[k + j] += f2 / P;
        }
        if (S) {
            double c = (f1 - f2) / P;
            for (int p = 0; p < k; ++p)
                S[static_cast<int64_t>(p) * n + i] = c * X[static_cast<int64_t>(p) * n + i];
            if (j > 0) S[static_cast<int64_t>(k + j - 1) * n + i] = -f1 / P;
            if (j < nc) S[static_cast<int64_t>(k + j) * n + i] = f2 / P;
        }
    }
    return LL + LLc;
}

// Analytic negative Hessian −∂²LL/∂θ∂θ' (np*np, symmetric). Per row, with
// P = F(a2) − F(a1), a = κ − xβ: −∂²ln P = ∇P∇P'/P² − ∇²P/P, where ∇P is
// ((f1 − f2)x, −f1 at κ_{j−1}, f2 at κ_j) and ∇²P has (f2' − f1')xx',
// f1'x / −f2'x across (β, κ_{j−1} / κ_j), −f1' and f2' on the cut diagonals;
// f' = f(1 − 2F) (logistic) or −aφ(a) (normal).
inline void neg_hessian(const double* X, int64_t n, int k, const int* cat, int J, OrdLink link,
                        const std::vector<double>& theta, std::vector<double>& A) {
    const int nc = J - 1, np = k + nc;
    A.assign(static_cast<size_t>(np) * np, 0.0);
    std::vector<double> gP(static_cast<size_t>(np));
    for (int64_t i = 0; i < n; ++i) {
        double xb = 0;
        for (int p = 0; p < k; ++p) xb += X[static_cast<int64_t>(p) * n + i] * theta[p];
        const int j = cat[i];
        double F1 = 0.0, f1 = 0.0, d1 = 0.0, F2 = 1.0, f2 = 0.0, d2 = 0.0;
        if (link == OrdLink::Logit) {
            if (j > 0) { double a1 = theta[k + j - 1] - xb; F1 = lcdf(a1); f1 = F1 * (1.0 - F1); d1 = f1 * (1.0 - 2.0 * F1); }
            if (j < nc) { double a2 = theta[k + j] - xb; F2 = lcdf(a2); f2 = F2 * (1.0 - F2); d2 = f2 * (1.0 - 2.0 * F2); }
        } else {
            if (j > 0) { double a1 = theta[k + j - 1] - xb; F1 = ncdf(a1); f1 = npdf(a1); d1 = -a1 * f1; }
            if (j < nc) { double a2 = theta[k + j] - xb; F2 = ncdf(a2); f2 = npdf(a2); d2 = -a2 * f2; }
        }
        double P = F2 - F1;
        if (P < 1e-300) P = 1e-300;
        std::fill(gP.begin(), gP.end(), 0.0);
        for (int p = 0; p < k; ++p) gP[static_cast<size_t>(p)] = (f1 - f2) * X[static_cast<int64_t>(p) * n + i];
        if (j > 0) gP[static_cast<size_t>(k + j - 1)] = -f1;
        if (j < nc) gP[static_cast<size_t>(k + j)] = f2;
        for (int a = 0; a < np; ++a)
            for (int b = 0; b < np; ++b)
                A[static_cast<size_t>(a) * np + b] += gP[static_cast<size_t>(a)] * gP[static_cast<size_t>(b)] / (P * P);
        for (int a = 0; a < k; ++a) {
            const double xa = X[static_cast<int64_t>(a) * n + i];
            for (int b = 0; b < k; ++b)
                A[static_cast<size_t>(a) * np + b] -= (d2 - d1) * xa * X[static_cast<int64_t>(b) * n + i] / P;
            if (j > 0) {
                A[static_cast<size_t>(a) * np + (k + j - 1)] -= d1 * xa / P;
                A[static_cast<size_t>(k + j - 1) * np + a] -= d1 * xa / P;
            }
            if (j < nc) {
                A[static_cast<size_t>(a) * np + (k + j)] += d2 * xa / P;
                A[static_cast<size_t>(k + j) * np + a] += d2 * xa / P;
            }
        }
        if (j > 0) A[static_cast<size_t>(k + j - 1) * np + (k + j - 1)] += d1 / P;
        if (j < nc) A[static_cast<size_t>(k + j) * np + (k + j)] -= d2 / P;
    }
}

}  // namespace ogl

// Recode y to 0-based categories by sorted distinct value. Returns the distinct
// values (ascending — the labels Stata prints); `cat` is filled per row.
//
// ONE definition, shared: `mlogit` recodes its outcome by exactly the same rule
// (see mlogit.h's header comment), and cmds_mlogit.cpp calls this function rather
// than growing a second copy — two spellings of one coding rule is how the coding
// and the reported cat_values start disagreeing silently.
inline std::vector<double> ord_categories(const double* y, int64_t n, std::vector<int>& cat) {
    std::vector<double> vals(y, y + n);
    std::sort(vals.begin(), vals.end());
    vals.erase(std::unique(vals.begin(), vals.end()), vals.end());
    cat.resize(static_cast<size_t>(n));
    for (int64_t i = 0; i < n; ++i)
        cat[static_cast<size_t>(i)] =
            static_cast<int>(std::lower_bound(vals.begin(), vals.end(), y[i]) - vals.begin());
    return vals;
}

// X is col-major n*k with NO intercept column (the caller must drop it — with an
// intercept in the design the information matrix is singular against the cutpoints).
inline OglmResult oglm_fit(const double* X, int64_t n, int k, const int* cat, int J, OrdLink link) {
    OglmResult r;
    r.n = n;
    r.n_cats = J;
    if (J < 2 || n <= 0) return r;
    const int nc = J - 1, np = k + nc;

    r.cat_n.assign(static_cast<size_t>(J), 0);
    for (int64_t i = 0; i < n; ++i) ++r.cat_n[static_cast<size_t>(cat[i])];
    // Constant-only (β=0) log-likelihood. LINK-FREE: at β=0 with the starting
    // cutpoints below, P_ij is exactly the sample share of category j, so this is
    // both the ll_0 behind Pseudo R² AND the iteration-0 value Stata prints.
    for (int j = 0; j < J; ++j) {
        double nj = static_cast<double>(r.cat_n[static_cast<size_t>(j)]);
        if (nj > 0) r.ll_0 += nj * std::log(nj / static_cast<double>(n));
    }

    std::vector<double> theta(static_cast<size_t>(np), 0.0);
    {   // start: β = 0, κ_j = F^{-1}(cumulative share through j) — the closed-form
        // cutpoints-only fit, so the Newton loop starts AT the null model.
        double cum = 0;
        for (int j = 0; j < nc; ++j) {
            cum += static_cast<double>(r.cat_n[static_cast<size_t>(j)]) / static_cast<double>(n);
            theta[static_cast<size_t>(k + j)] =
                (link == OrdLink::Logit) ? std::log(cum / (1.0 - cum)) : ogl::norm_ppf(cum);
        }
    }

    // ml's Newton-Raphson (header comment). `V` holds inv(−H) at `theta`.
    std::vector<double> grad(static_cast<size_t>(np)), V, step(static_cast<size_t>(np));
    auto evaluate = [&](const std::vector<double>& th, std::vector<double>& g, std::vector<double>& Vinv) {
        const double LL = ogl::ll_grad(X, n, k, cat, J, link, th, &g);
        ogl::neg_hessian(X, n, k, cat, J, link, th, Vinv);
        return la::spd_inverse(Vinv.data(), np) == 0 ? LL : std::nan("");
    };
    auto llat = [&](const std::vector<double>& th) { return ogl::ll_grad(X, n, k, cat, J, link, th, nullptr); };
    double LL = evaluate(theta, grad, V);
    int it = 0;
    bool conv = false;
    std::vector<double> cand(static_cast<size_t>(np)), tryth(static_cast<size_t>(np)), dd(static_cast<size_t>(np));
    std::vector<double> g2(static_cast<size_t>(np)), V2;
    while (it < 300 && !conv && std::isfinite(LL)) {
        la::matvec(V.data(), np, grad.data(), step.data());  // step = inv(−H) g
        for (int i = 0; i < np; ++i) cand[static_cast<size_t>(i)] = theta[static_cast<size_t>(i)] + step[static_cast<size_t>(i)];
        double f1 = llat(cand);
        if (!(f1 >= LL)) {
            for (int nh = 0; nh < 60 && !(f1 >= LL); ++nh) {
                for (int i = 0; i < np; ++i)
                    cand[static_cast<size_t>(i)] = (cand[static_cast<size_t>(i)] + theta[static_cast<size_t>(i)]) / 2.0;
                f1 = llat(cand);
            }
        } else {
            for (int i = 0; i < np; ++i) dd[static_cast<size_t>(i)] = 0.125 * step[static_cast<size_t>(i)];
            for (int nf = 0; nf < 100; ++nf) {
                for (int i = 0; i < np; ++i) tryth[static_cast<size_t>(i)] = cand[static_cast<size_t>(i)] + dd[static_cast<size_t>(i)];
                const double fc = llat(tryth);
                if (!(fc > f1)) break;
                cand = tryth;
                f1 = fc;
                for (double& v : dd) v *= 2.0;
            }
        }
        const double LL2 = evaluate(cand, g2, V2);
        ++it;
        if (!std::isfinite(LL2)) { theta = cand; LL = LL2; break; }
        la::matvec(V2.data(), np, g2.data(), step.data());
        double nr = 0.0, pc = 0.0;
        for (int i = 0; i < np; ++i) {
            nr += g2[static_cast<size_t>(i)] * step[static_cast<size_t>(i)];
            pc = std::max(pc, std::fabs(cand[static_cast<size_t>(i)] - theta[static_cast<size_t>(i)]) /
                                  (std::fabs(theta[static_cast<size_t>(i)]) + 1.0));
        }
        const double rl = std::fabs(LL2 - LL) / (std::fabs(LL) + 1.0);
        conv = (pc <= 1e-6 || rl <= 1e-7) && nr < 1e-5;
        theta = cand;
        LL = LL2;
        grad.swap(g2);
        V.swap(V2);
    }
    r.converged = conv;
    r.iters = it;
    if (!std::isfinite(LL)) return r;
    r.ll = LL;
    r.vcov = V;
    r.beta.assign(theta.begin(), theta.begin() + k);
    r.cuts.assign(theta.begin() + k, theta.end());
    r.se.resize(static_cast<size_t>(k));
    for (int c = 0; c < k; ++c) r.se[c] = std::sqrt(std::max(0.0, V[static_cast<size_t>(c) * np + c]));
    r.se_cut.resize(static_cast<size_t>(nc));
    for (int c = 0; c < nc; ++c)
        r.se_cut[static_cast<size_t>(c)] =
            std::sqrt(std::max(0.0, V[static_cast<size_t>(k + c) * np + (k + c)]));
    r.ok = true;
    return r;
}

// Per-observation score rows at the converged θ (n*np, col-major) — the sandwich
// meat's raw material for vce(robust) / vce(cluster). Parameter order matches
// r.vcov exactly: slopes, then cutpoints.
inline std::vector<double> oglm_scores(const double* X, int64_t n, int k, const int* cat, int J,
                                       OrdLink link, const OglmResult& r) {
    std::vector<double> theta = r.beta;
    theta.insert(theta.end(), r.cuts.begin(), r.cuts.end());
    std::vector<double> S;
    ogl::ll_grad(X, n, k, cat, J, link, theta, nullptr, &S);
    return S;
}

}  // namespace cecon
