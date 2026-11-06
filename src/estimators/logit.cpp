// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "estimators/logit.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>

#include "core/distributions.h"  // normal_icdf (AS241 = Stata's invnormal): probit b0
#include "core/linalg.h"

namespace cecon {

// The pre-FU-138 fit: deviance-convergence IRLS from beta = 0. Kept for a
// NON-binary y only (fracreg's fractional outcome, cmds_zicount.cpp), whose
// quasi-likelihood the binary ml objective of binary_ml_fit does not define.
static LogitResult logit_fit_irls(const double* X, int64_t n, int k, const double* y, int maxit, double tol,
                                  const double* weights, const double* offset) {
    LogitResult r;
    r.k = k;
    std::vector<double> beta(k, 0.0), eta(n), w(n), z(n), sw(n);
    std::vector<double> Xs(static_cast<size_t>(n) * k), zs(n), bread(static_cast<size_t>(k) * k), Xtz(k);
    r.p.assign(n, 0.0);
    auto reweight = [&](const std::vector<double>& b) {
        la::mat_vec(X, n, k, b.data(), eta.data());
        for (int64_t i = 0; i < n; ++i) {
            // BUG-331: eta carries the offset; the working response subtracts it
            // back out, because IRLS regresses (eta - offset) + (y-p)/w on X. With
            // no offset both terms are 0 and every line below is the prior code.
            double off = offset ? offset[i] : 0.0;
            double e = std::max(-30.0, std::min(30.0, eta[i] + off));
            double p = 1.0 / (1.0 + std::exp(-e));
            r.p[i] = p;
            double wi = p * (1.0 - p);
            if (wi < 1e-12) wi = 1e-12;
            // z (the IRLS working response) always divides by the UNweighted,
            // floor-clamped p(1-p) — a per-row weight only rescales the normal
            // equations (see logit.h), not the pseudo-response.
            z[i] = (e - off) + (y[i] - p) / wi;
            if (weights) wi *= weights[i];
            w[i] = wi;
            sw[i] = std::sqrt(wi);
        }
    };
    // BUG-331 starting values. beta=0 means eta_0 = offset, and a real offset is
    // far from 0 (ln(weight) ~ 8 on the oracle fixture), so the first working
    // response is O(1/p(1-p)) ~ 3000 and IRLS walks off: measured, 100 iterations,
    // beta ~ 3e10, converged=false. The standard GLM remedy is to start from the
    // DATA rather than from beta=0 — mu_i = (y_i + 1/2)/2, eta_i = link(mu_i) —
    // which is offset-free and lands the first step near the answer.
    // Applied ONLY when an offset is present: the no-offset path keeps its beta=0
    // start and is byte-identical, iteration count included (verified against a
    // zero-valued offset column, which reproduces the no-offset fit exactly).
    if (offset) {
        // link(3/4) and link(1/4) for the logit link; y is binary so these two
        // values are the whole table.
        const double kEta1 = 1.0986122886681098, kEta0 = -1.0986122886681098;
        const double kMu1 = 0.75, kMu0 = 0.25, kW = 0.1875;  // mu(1-mu) at both
        for (int64_t i = 0; i < n; ++i) {
            double e0 = y[i] == 1.0 ? kEta1 : kEta0;
            double mu = y[i] == 1.0 ? kMu1 : kMu0;
            double wi = kW;
            z[i] = e0 + (y[i] - mu) / wi - offset[i];
            if (weights) wi *= weights[i];
            w[i] = wi;
            sw[i] = std::sqrt(wi);
        }
        for (int c = 0; c < k; ++c) {
            const double* xc = X + static_cast<size_t>(c) * n;
            double* sc = Xs.data() + static_cast<size_t>(c) * n;
            for (int64_t i = 0; i < n; ++i) sc[i] = xc[i] * sw[i];
        }
        for (int64_t i = 0; i < n; ++i) zs[i] = z[i] * sw[i];
        la::xtx(Xs.data(), n, k, bread.data());
        if (la::spd_inverse(bread.data(), k) == 0) {
            la::xtv(Xs.data(), n, k, zs.data(), Xtz.data());
            la::matvec(bread.data(), k, Xtz.data(), beta.data());
        }
    }

    double dev_old = 1e300;
    for (r.iters = 0; r.iters < maxit;) {
        reweight(beta);
        for (int c = 0; c < k; ++c) {
            const double* xc = X + static_cast<size_t>(c) * n;
            double* sc = Xs.data() + static_cast<size_t>(c) * n;
            for (int64_t i = 0; i < n; ++i) sc[i] = xc[i] * sw[i];
        }
        for (int64_t i = 0; i < n; ++i) zs[i] = z[i] * sw[i];
        la::xtx(Xs.data(), n, k, bread.data());
        if (la::spd_inverse(bread.data(), k) != 0) { r.separated = true; break; }
        la::xtv(Xs.data(), n, k, zs.data(), Xtz.data());
        la::matvec(bread.data(), k, Xtz.data(), beta.data());
        ++r.iters;
        // deviance at the new beta (weighted, when weights are present, so the
        // convergence monitor tracks the actual pseudo-likelihood objective)
        la::mat_vec(X, n, k, beta.data(), eta.data());
        double dev = 0;
        for (int64_t i = 0; i < n; ++i) {
            double e = std::max(-30.0, std::min(30.0, eta[i] + (offset ? offset[i] : 0.0)));
            double p = 1.0 / (1.0 + std::exp(-e));
            double di = -2.0 * (y[i] * std::log(p > 1e-15 ? p : 1e-15) +
                                (1 - y[i]) * std::log((1 - p) > 1e-15 ? (1 - p) : 1e-15));
            dev += weights ? weights[i] * di : di;
        }
        if (std::fabs(dev - dev_old) / (std::fabs(dev) + 0.1) < tol) { r.converged = true; break; }
        dev_old = dev;
    }
    // final probabilities + bread = (X'WX)^{-1} at the converged beta
    reweight(beta);
    bool predicts_all = true;
    double final_deviance = 0.0;
    for (int64_t i = 0; i < n; ++i) {
        double p = std::min(1.0 - 1e-15, std::max(1e-15, r.p[i]));
        bool predicts_one = y[i] == 1.0 && p > 0.5;
        bool predicts_zero = y[i] == 0.0 && p < 0.5;
        if (!predicts_one && !predicts_zero) {
            predicts_all = false;
        }
        final_deviance += -2.0 * (y[i] * std::log(p) + (1.0 - y[i]) * std::log(1.0 - p));
    }
    r.separated_perfect = n > 0 && predicts_all && final_deviance / static_cast<double>(n) < 1e-6;
    for (int c = 0; c < k; ++c) {
        const double* xc = X + static_cast<size_t>(c) * n;
        double* sc = Xs.data() + static_cast<size_t>(c) * n;
        for (int64_t i = 0; i < n; ++i) sc[i] = xc[i] * sw[i];
    }
    la::xtx(Xs.data(), n, k, bread.data());
    la::spd_inverse(bread.data(), k);
    r.bread = std::move(bread);
    r.beta = std::move(beta);
    return r;
}

// The columns Stata's invsym keeps of a symmetric PSD A (k*k): sweep the largest
// absolute pivot first and drop a column once its pivot is <= 1e-9 x its original
// diagonal or below 1e-19 (the rule measured for e(rank), estimation_glm.cpp
// glmVceRank). keep[j] = 1 for a swept column.
std::vector<char> invsym_kept_columns(const std::vector<double>& A0, size_t k) {
    std::vector<double> A = A0;
    std::vector<double> d0(k);
    std::vector<char> left(k), keep(k, 0);
    for (size_t j = 0; j < k; ++j) {
        d0[j] = A[j * k + j];
        left[j] = d0[j] > 0.0;
    }
    for (;;) {
        size_t best = k;
        for (size_t j = 0; j < k; ++j) {
            if (!left[j]) continue;
            const double p = A[j * k + j];
            if (p <= 1e-9 * d0[j] || p < 1e-19) continue;
            if (best == k || p > A[best * k + best]) best = j;
        }
        if (best == k) break;
        left[best] = 0;
        keep[best] = 1;
        const double p = A[best * k + best];
        const std::vector<double> row(A.begin() + static_cast<std::ptrdiff_t>(best * k),
                                      A.begin() + static_cast<std::ptrdiff_t>(best * k + k));
        for (size_t i = 0; i < k; ++i)
            for (size_t j = 0; j < k; ++j) A[i * k + j] -= row[i] * row[j] / p;
    }
    return keep;
}

// invsym(A) as a generalized inverse: the kept block's inverse (spd_inverse, the
// arithmetic the full-rank fits already use), zero rows/columns elsewhere.
// Returns false when the kept block does not invert.
bool invsym_generalized(const std::vector<double>& A, size_t k, std::vector<double>& out) {
    const std::vector<char> keep = invsym_kept_columns(A, k);
    std::vector<size_t> live;
    for (size_t c = 0; c < k; ++c)
        if (keep[c]) live.push_back(c);
    out.assign(k * k, 0.0);
    const size_t L = live.size();
    if (L == 0) return true;
    std::vector<double> sub(L * L);
    for (size_t a = 0; a < L; ++a)
        for (size_t c = 0; c < L; ++c) sub[a * L + c] = A[live[a] * k + live[c]];
    if (la::spd_inverse(sub.data(), static_cast<int>(L)) != 0) return false;
    for (size_t a = 0; a < L; ++a)
        for (size_t c = 0; c < L; ++c) out[live[a] * k + live[c]] = sub[a * L + c];
    return true;
}

// A = -H is treated as concave at V = invsym(A) unless the generalized inverse
// zeroed a direction along which A itself is nonzero.
bool ml_check_concave(const std::vector<double>& A, const std::vector<double>& V, size_t k) {
    for (size_t j = 0; j < k; ++j)
        if (V[j * k + j] == 0.0 && A[j * k + j] != 0.0) return false;
    return true;
}

namespace {

// Neumaier's compensated summation: the step rules compare log likelihoods
// directly, so the total is kept to the last bit the data allow.
class CompensatedSum {
public:
    void add(double x) {
        const double t = total_ + x;
        if (std::fabs(total_) >= std::fabs(x))
            carry_ += (total_ - t) + x;
        else
            carry_ += (x - t) + total_;
        total_ = t;
    }
    double value() const { return total_ + carry_; }

private:
    double total_ = 0.0, carry_ = 0.0;
};

constexpr double kInvSqrt2 = 0.7071067811865476;      // 1 / sqrt(2)
constexpr double kInvSqrt2Pi = 0.3989422804014327;    // 1 / sqrt(2 pi)
constexpr double kHalfLog2Pi = 0.91893853320467274;   // ln(2 pi) / 2
// A logit row with q*eta above 18, or a probit row above 6, is completely
// determined: it adds nothing to the log likelihood or its derivatives
// (calibrated against StataBE's e(N_cdf) / e(N_cds) and iteration traces).
constexpr double kLogitDetermined = 18.0;
constexpr double kProbitDetermined = 6.0;

// One row's log likelihood l, score s = dl/deta and curvature h = -d2l/deta2.
struct RowTerm {
    double ll = 0.0, score = 0.0, curv = 0.0;
};

double determined_cutoff(BinaryLink link) {
    switch (link) {
        case BinaryLink::Logit: return kLogitDetermined;
        case BinaryLink::Probit: return kProbitDetermined;
        case BinaryLink::Cloglog: break;
    }
    return std::numeric_limits<double>::infinity();
}

// Returns false for a completely determined row (all three terms zero).
bool row_term(BinaryLink link, double eta, bool success, bool derivs, RowTerm& r) {
    const double q = success ? 1.0 : -1.0;
    const double t = q * eta;
    switch (link) {
        case BinaryLink::Logit: {
            if (t > kLogitDetermined) return false;
            r.ll = (t <= -700.0) ? t : -std::log(1.0 + std::exp(-t));
            if (derivs) {
                const double p = 1.0 / (1.0 + std::exp(-eta));
                r.score = (success ? 1.0 : 0.0) - p;
                r.curv = p * (1.0 - p);
            }
            return true;
        }
        case BinaryLink::Probit: {
            if (t > kProbitDetermined) return false;
            const double cdf = 0.5 * std::erfc(-t * kInvSqrt2);
            double ratio = 0.0;  // phi(eta) / Phi(t)
            if (cdf > 1e-300) {
                r.ll = std::log(cdf);
                if (derivs) ratio = kInvSqrt2Pi * std::exp(-0.5 * eta * eta) / cdf;
            } else {
                // Asymptotic expansion of the lower normal tail (Mills ratio series).
                const double t2 = t * t;
                r.ll = -0.5 * t2 - kHalfLog2Pi - std::log(-t) + std::log(1.0 - 1.0 / t2 + 3.0 / (t2 * t2));
                if (derivs) ratio = std::exp(-0.5 * eta * eta - kHalfLog2Pi - r.ll);
            }
            if (derivs) {
                const double lambda = q * ratio;
                r.score = lambda;
                r.curv = lambda * (lambda + eta);
            }
            return true;
        }
        case BinaryLink::Cloglog: {
            // F(eta) = 1 - exp(-exp(eta)), [R] cloglog Methods and formulas.
            const double u = std::exp(eta);
            if (!success) {
                r.ll = -u;
                r.score = -u;
                r.curv = u;
                return true;
            }
            if (eta > 100.0)
                r.ll = 0.0;
            else if (eta < -12.0)
                r.ll = eta - (u / 2.0) * (1.0 - u) / 4.0;  // Stata's measured lower-tail value
            else
                r.ll = std::log1p(-std::exp(-u));
            if (derivs) {
                const double F = -std::expm1(-u);
                const double s = std::exp(eta - u) / F;
                r.score = s;
                r.curv = (s == 0.0) ? 0.0 : (s / F) * (u - 1.0 + std::exp(-u));
            }
            return true;
        }
    }
    return true;
}

double fitted_probability(BinaryLink link, double eta) {
    switch (link) {
        case BinaryLink::Logit: return 1.0 / (1.0 + std::exp(-eta));
        case BinaryLink::Probit: return 0.5 * std::erfc(-eta * kInvSqrt2);
        case BinaryLink::Cloglog: return -std::expm1(-std::exp(eta));
    }
    return 0.0;
}

// The weighted log likelihood of one binary model over a fixed design, with
// its gradient and negative Hessian on request.
class BinaryObjective final : public MlObjective {
public:
    BinaryObjective(BinaryLink link, const double* X, int64_t n, int k, const double* y,
                    const double* w, const double* offset)
        : link_(link), X_(X), n_(n), k_(k), y_(y), w_(w), offset_(offset),
          eta_(static_cast<size_t>(n)), wscore_(static_cast<size_t>(n)), root_(static_cast<size_t>(n)),
          scaled_(static_cast<size_t>(n) * static_cast<size_t>(std::max(k, 0))) {}

    // Linear index at b (offset included).
    void index(const std::vector<double>& b) {
        if (k_ > 0)
            la::mat_vec(X_, n_, k_, b.data(), eta_.data());
        else
            std::fill(eta_.begin(), eta_.end(), 0.0);
        if (offset_)
            for (int64_t i = 0; i < n_; ++i) eta_[static_cast<size_t>(i)] += offset_[i];
    }

    double loglik(const std::vector<double>& b) override {
        index(b);
        CompensatedSum total;
        RowTerm r;
        for (int64_t i = 0; i < n_; ++i) {
            if (!row_term(link_, eta_[static_cast<size_t>(i)], y_[i] != 0.0, false, r)) continue;
            total.add(w_ ? w_[i] * r.ll : r.ll);
        }
        return total.value();
    }

    // ll at b; g = X'(w s), A = X' diag(w h) X formed as the cross product of
    // X with rows scaled by sqrt(max(0, w h)).
    double evaluate(const std::vector<double>& b, std::vector<double>& g, std::vector<double>& A) override {
        index(b);
        CompensatedSum total;
        for (int64_t i = 0; i < n_; ++i) {
            const size_t ii = static_cast<size_t>(i);
            RowTerm r;
            if (!row_term(link_, eta_[ii], y_[i] != 0.0, true, r)) {
                wscore_[ii] = 0.0;
                root_[ii] = 0.0;
                continue;
            }
            const double wi = w_ ? w_[i] : 1.0;
            total.add(w_ ? wi * r.ll : r.ll);
            wscore_[ii] = wi * r.score;
            root_[ii] = std::sqrt(std::max(0.0, wi * r.curv));
        }
        for (int c = 0; c < k_; ++c) {
            const double* xc = X_ + static_cast<size_t>(c) * static_cast<size_t>(n_);
            double* sc = scaled_.data() + static_cast<size_t>(c) * static_cast<size_t>(n_);
            for (int64_t i = 0; i < n_; ++i) sc[i] = xc[i] * root_[static_cast<size_t>(i)];
        }
        g.assign(static_cast<size_t>(std::max(k_, 0)), 0.0);
        A.assign(static_cast<size_t>(std::max(k_, 0)) * static_cast<size_t>(std::max(k_, 0)), 0.0);
        if (k_ > 0) {
            la::xtv(X_, n_, k_, wscore_.data(), g.data());
            la::xtx(scaled_.data(), n_, k_, A.data());
        }
        return total.value();
    }

    const std::vector<double>& eta() const { return eta_; }

private:
    BinaryLink link_;
    const double* X_;
    int64_t n_;
    int k_;
    const double* y_;
    const double* w_;
    const double* offset_;
    std::vector<double> eta_, wscore_, root_, scaled_;
};

// The inverse of A = -H used for the Newton direction and the bread, with the
// point's status.
struct Curvature {
    std::vector<double> inv;
    bool singular = false;
    bool concave = true;
};

Curvature invert_curvature(const std::vector<double>& A, size_t k, const MlMaxOptions& opt) {
    Curvature c;
    if (opt.spd_concavity) {
        c.inv = A;
        if (la::spd_inverse(c.inv.data(), static_cast<int>(k)) == 0) return c;
        c.concave = false;
        if (!invsym_generalized(A, k, c.inv)) {
            c.singular = true;
            c.inv.assign(k * k, 0.0);
        }
        return c;
    }
    if (opt.stata_invsym) {
        if (!invsym_generalized(A, k, c.inv)) {
            c.singular = true;
            c.inv.assign(k * k, 0.0);
            return c;
        }
        c.concave = ml_check_concave(A, c.inv, k);
        return c;
    }
    const bool all_zero = std::all_of(A.begin(), A.end(), [](double v) { return v == 0.0; });
    if (all_zero) {  // every row determined: a zero step, not a singular point
        c.inv.assign(k * k, 0.0);
        return c;
    }
    c.inv = A;
    if (la::spd_inverse(c.inv.data(), static_cast<int>(k)) != 0) {
        c.singular = true;
        if (!invsym_generalized(A, k, c.inv)) c.inv.assign(k * k, 0.0);
    }
    return c;
}

double quad_form(const std::vector<double>& M, const std::vector<double>& v, size_t k) {
    std::vector<double> Mv(k);
    if (k > 0) la::matvec(M.data(), static_cast<int>(k), v.data(), Mv.data());
    double s = 0.0;
    for (size_t j = 0; j < k; ++j) s += v[j] * Mv[j];
    return s;
}

double dot_k(const std::vector<double>& a, const std::vector<double>& b) {
    double s = 0.0;
    for (size_t j = 0; j < a.size(); ++j) s += a[j] * b[j];
    return s;
}

// The numbers of Stata's non-concave step ([M-5] moptimize, singularHmethod
// "m-marquardt"), gathered here so they read in one place.
struct NonConcaveStepConstants {
    double first_loading = 1.1;      // diagonal loading factor of the first attempt
    double loading_growth = 2.0;     // growth of that factor between attempts
    int max_loadings = 1100;         // attempts before the point counts as singular
    double gain_early = 1e-2;        // gain target while the log holds at most two values
    double gain_slope = 0.1;         // later: this share of the last relative ll change,
    double gain_floor = 1e-8;        //   kept at or above this floor
    double gain_ceiling = 1e-2;      //   and at or below this ceiling
    double unscalable_step = 2.0;    // step multiplier when the gain target cannot be solved
};
constexpr NonConcaveStepConstants kNonConcave{};

// Line-search budget: how far the search backs up, and how it runs on.
constexpr int kMaxHalvings = 60;
constexpr int kMaxExtensions = 100;
constexpr double kFirstExtensionShare = 0.125;  // of the direction, at a concave point

// A trial coefficient vector with everything the search asks of it.
struct MlPoint {
    std::vector<double> beta, grad, neg_hess;
    double ll = 0.0;
    Curvature curv;
};

MlPoint evaluate_point(MlObjective& objective, std::vector<double> beta, size_t k, const MlMaxOptions& opt) {
    MlPoint point;
    point.beta = std::move(beta);
    point.ll = objective.evaluate(point.beta, point.grad, point.neg_hess);
    point.curv = invert_curvature(point.neg_hess, k, opt);
    return point;
}

// What the search carries from one iteration to the next.
struct MlSearch {
    MlPoint here;
    std::vector<double> ll_trace;
    int iterations = 0;
    bool converged = false;
    bool failed = false;
};

double matrix_trace(const std::vector<double>& m, size_t k) {
    double total = 0.0;
    for (size_t j = 0; j < k; ++j) total += m[j * k + j];
    return total;
}

// Loads the diagonal of `loaded` by a growing share of itself until its generalized
// inverse passes the concavity test. False when the attempts run out.
bool load_diagonal_until_concave(std::vector<double> loaded, size_t k, std::vector<double>& inverse) {
    double factor = kNonConcave.first_loading;
    for (int attempt = 0; attempt < kNonConcave.max_loadings; ++attempt) {
        for (size_t j = 0; j < k; ++j) loaded[j * k + j] += factor * std::fabs(loaded[j * k + j]);
        if (invsym_generalized(loaded, k, inverse) && ml_check_concave(loaded, inverse, k)) return true;
        factor *= kNonConcave.loading_growth;
    }
    return false;
}

// The share of |ll| a non-concave step should gain to first order.
double nonconcave_gain_target(const std::vector<double>& ll_trace) {
    if (ll_trace.size() <= 2) return kNonConcave.gain_early;
    const double latest = ll_trace.back(), earlier = ll_trace[ll_trace.size() - 2];
    const double share = kNonConcave.gain_slope * (latest - earlier) / std::fabs(earlier);
    if (!std::isfinite(share)) return kNonConcave.gain_floor;
    return std::min(kNonConcave.gain_ceiling, std::max(kNonConcave.gain_floor, share));
}

// Direction at a non-concave point: a loaded inverse brought to the size of the
// current one (its trace), then a step scaled to the gain target.
bool nonconcave_direction(const MlSearch& search, size_t k, std::vector<double>& direction) {
    const MlPoint& here = search.here;
    std::vector<double> inverse;
    if (!load_diagonal_until_concave(here.neg_hess, k, inverse)) return false;
    const double size_ratio = matrix_trace(here.curv.inv, k) / matrix_trace(inverse, k);
    if (size_ratio > 0.0)
        for (double& entry : inverse) entry *= size_ratio;
    direction.assign(k, 0.0);
    if (k > 0) la::matvec(inverse.data(), static_cast<int>(k), here.grad.data(), direction.data());
    double multiplier = nonconcave_gain_target(search.ll_trace) * std::fabs(here.ll) / dot_k(here.grad, direction);
    if (!std::isfinite(multiplier)) multiplier = kNonConcave.unscalable_step;
    for (double& entry : direction) entry *= multiplier;
    return true;
}

// The largest |change_j| relative to |base_j| + pad.
double largest_relative(const std::vector<double>& change, const std::vector<double>& base, double pad) {
    double largest = 0.0;
    for (size_t j = 0; j < change.size(); ++j)
        largest = std::max(largest, std::fabs(change[j]) / (std::fabs(base[j]) + pad));
    return largest;
}

// The documented stop rule, (ptol or vtol) and nrtol, between consecutive iterates.
bool stop_rule_holds(const MlPoint& before, const MlPoint& after, size_t k, const MlMaxOptions& opt) {
    if (!opt.ignore_nrtol) {
        if (after.curv.singular || !after.curv.concave) return false;  // nr is +infinity here
        if (!(quad_form(after.curv.inv, after.grad, k) < opt.nrtol)) return false;
    }
    if (std::fabs(after.ll - before.ll) / (std::fabs(before.ll) + 1.0) <= opt.ltol) return true;
    std::vector<double> moved(k);
    for (size_t j = 0; j < k; ++j) moved[j] = after.beta[j] - before.beta[j];
    double param_change = largest_relative(moved, before.beta, 1.0);
    if (opt.doopt) param_change = std::max(param_change, largest_relative(moved, after.beta, 0.001));
    return param_change <= opt.tol;
}

// Walks a losing point back toward `here`. Equal ll is good enough to stop.
std::vector<double> back_up(MlObjective& objective, const MlPoint& here, std::vector<double> point,
                            bool return_home_on_failure) {
    for (int halving = 0; halving < kMaxHalvings; ++halving) {
        for (size_t j = 0; j < point.size(); ++j) point[j] = (point[j] + here.beta[j]) / 2.0;
        if (objective.loglik(point) >= here.ll) return point;
    }
    return return_home_on_failure ? here.beta : point;
}

// Runs a winning point further along `stride`, doubling it, while ll strictly rises.
std::vector<double> run_on(MlObjective& objective, std::vector<double> point, double point_ll,
                           std::vector<double> stride) {
    std::vector<double> further(point.size());
    for (int extension = 0; extension < kMaxExtensions; ++extension) {
        for (size_t j = 0; j < point.size(); ++j) further[j] = point[j] + stride[j];
        const double further_ll = objective.loglik(further);
        if (!(further_ll > point_ll)) break;
        point.swap(further);
        point_ll = further_ll;
        for (double& entry : stride) entry *= 2.0;
    }
    return point;
}

// One iteration: choose a direction, settle how far to go, and move there.
void search_step(MlObjective& objective, const MlMaxOptions& opt, size_t k, MlSearch& search) {
    MlPoint& here = search.here;
    if (here.curv.singular || !std::isfinite(here.ll)) {
        search.failed = true;
        return;
    }
    const bool concave = here.curv.concave;
    std::vector<double> direction(k);
    if (!concave) {
        if (!nonconcave_direction(search, k, direction)) {
            search.failed = true;
            return;
        }
    } else if (k > 0) {
        la::matvec(here.curv.inv.data(), static_cast<int>(k), here.grad.data(), direction.data());
    }
    std::vector<double> landing(k);
    for (size_t j = 0; j < k; ++j) landing[j] = here.beta[j] + direction[j];

    // doopt: a Newton step this small is taken without evaluating where it lands, so
    // ll, the inverse and the iteration count stay those of the current point.
    if (opt.doopt && concave && largest_relative(direction, landing, 0.001) <= opt.tol &&
        (opt.ignore_nrtol || std::fabs(dot_k(here.grad, direction)) <= opt.nrtol)) {
        here.beta = std::move(landing);
        search.converged = true;
        return;
    }

    const double landing_ll = objective.loglik(landing);
    if (!(landing_ll >= here.ll)) {
        landing = back_up(objective, here, std::move(landing), opt.stata_invsym || opt.spd_concavity);
    } else if (opt.step_forward) {
        std::vector<double> stride(direction);
        const double share = concave ? kFirstExtensionShare : 1.0;
        for (double& entry : stride) entry = share * entry;
        landing = run_on(objective, std::move(landing), landing_ll, std::move(stride));
    }

    MlPoint next = evaluate_point(objective, std::move(landing), k, opt);
    ++search.iterations;
    search.converged = stop_rule_holds(here, next, k, opt);
    search.ll_trace.push_back(next.ll);
    here = std::move(next);
}

}  // namespace

// Stata's modified Newton-Raphson ([R] Maximize; [M-5] moptimize, convergence
// criteria): Newton direction, step halving on a decrease, forward extension on an
// increase, and the stop rule (ptol or vtol) and nrtol. The result is the iterate
// where that rule fires, not the fully converged MLE.
MlPath ml_newton_path(MlObjective& obj, std::vector<double> start, const MlMaxOptions& opt) {
    const size_t k = start.size();
    MlSearch search;
    search.here = evaluate_point(obj, std::move(start), k, opt);
    search.ll_trace.push_back(search.here.ll);
    while (search.iterations < opt.maxiter && !search.converged && !search.failed)
        search_step(obj, opt, k, search);

    MlPoint& final_point = search.here;
    MlPath path;
    path.failed = search.failed || final_point.curv.singular;
    path.converged = search.converged && !path.failed;
    path.concave = final_point.curv.concave && !final_point.curv.singular;
    path.ic = search.iterations;
    path.ll = final_point.ll;
    path.ilog = std::move(search.ll_trace);
    path.inv_curvature = std::move(final_point.curv.inv);
    path.b = std::move(final_point.beta);
    return path;
}

// Stata's ml on a binary-outcome likelihood, with the analytic Hessian.
BinaryMlResult binary_ml_fit(BinaryLink link, const double* X, int64_t n, int k, const double* y,
                             const double* weights, const double* offset, bool has_constant,
                             const MlMaxOptions& opt, const std::vector<double>* start) {
    const size_t K = static_cast<size_t>(std::max(k, 0));
    BinaryObjective obj(link, X, n, k, y, weights, offset);

    // ---- starting values ----
    std::vector<double> b(K, 0.0);
    if (start && start->size() == K) {
        b = *start;
    } else if (has_constant && k > 0) {
        double n1 = 0.0, nw = 0.0;
        for (int64_t i = 0; i < n; ++i) {
            const double wi = weights ? weights[i] : 1.0;
            nw += wi;
            if (y[i] != 0.0) n1 += wi;
        }
        double b0 = 0.0;
        if (n1 > 0.0 && n1 < nw) {
            switch (link) {
                case BinaryLink::Logit: b0 = std::log(n1 / (nw - n1)); break;
                case BinaryLink::Probit: b0 = normal_icdf(n1 / nw); break;
                case BinaryLink::Cloglog: b0 = std::log(std::log(nw / (nw - n1))); break;
            }
        }
        b[K - 1] = b0;
    }

    MlPath path = ml_newton_path(obj, std::move(b), opt);

    BinaryMlResult r;
    r.singular = path.failed;
    r.converged = path.converged;
    r.ic = path.ic;
    r.ll = path.ll;
    r.ilog = std::move(path.ilog);
    r.bread.assign(K * K, 0.0);
    for (size_t a = 0; a < K; ++a)
        for (size_t c = 0; c < K; ++c)
            r.bread[a * K + c] = (path.inv_curvature[a * K + c] + path.inv_curvature[c * K + a]) / 2.0;

    obj.index(path.b);
    const std::vector<double>& eta = obj.eta();
    const double cutoff = determined_cutoff(link);
    r.p.resize(static_cast<size_t>(n));
    for (int64_t i = 0; i < n; ++i) {
        const double e = eta[static_cast<size_t>(i)];
        r.p[static_cast<size_t>(i)] = fitted_probability(link, e);
        if (link == BinaryLink::Cloglog) continue;
        const bool success = y[i] != 0.0;
        if ((success ? e : -e) > cutoff) ++(success ? r.n_cds : r.n_cdf);
    }
    r.beta = std::move(path.b);
    return r;
}

// The IRLS fits' separated_perfect screen (logit_fit_irls / probit_fit_irls), on the ml
// path's probabilities (cmd_logit / cmd_probit refuse it with r(2000)).
bool binary_predicts_perfectly(const std::vector<double>& pr, const double* y, int64_t n) {
    bool predicts_all = true;
    double dev = 0.0;
    for (int64_t i = 0; i < n; ++i) {
        const double p = std::min(1.0 - 1e-15, std::max(1e-15, pr[static_cast<size_t>(i)]));
        if (!((y[i] == 1.0 && p > 0.5) || (y[i] == 0.0 && p < 0.5))) predicts_all = false;
        dev += -2.0 * (y[i] * std::log(p) + (1.0 - y[i]) * std::log(1.0 - p));
    }
    return n > 0 && predicts_all && dev / static_cast<double>(n) < 1e-6;
}

LogitResult logit_fit_ml(const double* X, int64_t n, int k, const double* y, const MlMaxOptions& opt,
                         const double* weights, const double* offset, bool has_constant) {
    BinaryMlResult m = binary_ml_fit(BinaryLink::Logit, X, n, k, y, weights, offset, has_constant, opt);
    LogitResult r;
    r.k = k;
    r.separated_perfect = binary_predicts_perfectly(m.p, y, n);
    r.beta = std::move(m.beta);
    r.p = std::move(m.p);
    r.bread = std::move(m.bread);
    r.iters = m.ic;
    r.ic = m.ic;
    r.converged = m.converged;
    r.separated = m.singular;
    r.ll = m.ll;
    r.ilog = std::move(m.ilog);
    r.n_cdf = m.n_cdf;
    r.n_cds = m.n_cds;
    return r;
}

// Perfect-prediction screen, as [R] logit Remarks "Model identification"
// documents it: a regressor that predicts one outcome perfectly on part of the
// sample leaves with those rows; one that separates the outcome completely makes
// the model inestimable. After each rule the scan restarts at the first remaining
// candidate, since dropped rows can expose a new rule.
BinaryPerfectScreen binary_perfect_screen(const double* X, int64_t n, int k, int ncheck,
                                          const double* y) {
    BinaryPerfectScreen s;
    s.keep_row.assign(static_cast<size_t>(std::max<int64_t>(n, 0)), 1);
    s.omit_col.assign(static_cast<size_t>(std::max(k, 0)), 0);
    std::vector<int> candidates;
    for (int c = 0; c < std::min(ncheck, k); ++c) candidates.push_back(c);
    const double inf = std::numeric_limits<double>::infinity();

    bool fired = true;
    while (fired) {
        fired = false;
        for (size_t pos = 0; pos < candidates.size() && !fired; ++pos) {
            const int col = candidates[pos];
            const double* x = X + static_cast<size_t>(col) * static_cast<size_t>(n);
            // Range of x over the kept failures (lo0..hi0) and successes (lo1..hi1).
            double lo0 = inf, hi0 = -inf, lo1 = inf, hi1 = -inf, xmin = inf, xmax = -inf;
            bool any0 = false, any1 = false;
            for (int64_t i = 0; i < n; ++i) {
                if (!s.keep_row[static_cast<size_t>(i)]) continue;
                const double v = x[i];
                xmin = std::min(xmin, v);
                xmax = std::max(xmax, v);
                if (y[i] != 0.0) {
                    any1 = true;
                    lo1 = std::min(lo1, v);
                    hi1 = std::max(hi1, v);
                } else {
                    any0 = true;
                    lo0 = std::min(lo0, v);
                    hi0 = std::max(hi0, v);
                }
            }
            if (!(xmin < xmax)) continue;  // no variation: left to the collinearity step
            // An empty group's range is missing, which sorts above every number.
            if (!any0) lo0 = hi0 = inf;
            if (!any1) lo1 = hi1 = inf;

            BinaryPerfectRule rule;
            rule.col = col;
            if (lo0 == hi0 && (lo0 == lo1 || lo0 == hi1)) {
                rule.op = 1; rule.rhs = lo0; rule.interp = 1;    // x != v predicts success
            } else if (lo1 == hi1 && (lo1 == lo0 || lo1 == hi0)) {
                rule.op = 1; rule.rhs = lo1; rule.interp = 0;    // x != v predicts failure
            } else if (hi0 == lo1) {
                rule.op = 2; rule.rhs = hi0; rule.interp = -1;   // x > v predicts data except x == v
            } else if (hi1 == lo0) {
                rule.op = 3; rule.rhs = hi1; rule.interp = -1;   // x < v predicts data except x == v
            } else if (hi0 < lo1 || hi1 < lo0) {                 // complete separation
                rule.op = hi0 < lo1 ? 2 : 3;
                rule.rhs = hi0 < lo1 ? hi0 : hi1;
                rule.interp = -2;
                s.rules.push_back(rule);
                s.inestimable = true;
                std::fill(s.keep_row.begin(), s.keep_row.end(), 0);
                return s;
            } else {
                continue;
            }
            // Every non-separating rule keeps exactly the rows with x == rhs.
            for (int64_t i = 0; i < n; ++i) {
                if (!s.keep_row[static_cast<size_t>(i)] || x[i] == rule.rhs) continue;
                s.keep_row[static_cast<size_t>(i)] = 0;
                ++rule.dropped;
            }
            s.rules.push_back(rule);
            s.omit_col[static_cast<size_t>(col)] = 1;
            candidates.erase(candidates.begin() + static_cast<std::ptrdiff_t>(pos));
            fired = true;
        }
    }
    return s;
}

bool last_column_is_constant(const double* X, int64_t n, int k) {
    if (k < 1 || n < 1) return false;
    const double* c = X + static_cast<size_t>(k - 1) * static_cast<size_t>(n);
    for (int64_t i = 0; i < n; ++i)
        if (c[i] != 1.0) return false;
    return true;
}

bool outcome_is_binary(const double* y, int64_t n) {
    for (int64_t i = 0; i < n; ++i)
        if (y[i] != 0.0 && y[i] != 1.0) return false;
    return true;
}

LogitResult logit_fit(const double* X, int64_t n, int k, const double* y, int maxit, double tol,
                      const double* weights, const double* offset) {
    if (!outcome_is_binary(y, n)) return logit_fit_irls(X, n, k, y, maxit, tol, weights, offset);
    MlMaxOptions opt;
    opt.maxiter = maxit;
    return logit_fit_ml(X, n, k, y, opt, weights, offset, last_column_is_constant(X, n, k));
}

}  // namespace cecon
