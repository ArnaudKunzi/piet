// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "estimators/probit.h"

#include <cmath>
#include <vector>

#include "core/linalg.h"

namespace cecon {

static inline double norm_cdf(double x) { return 0.5 * std::erfc(-x * 0.7071067811865476); }
static inline double norm_pdf(double x) { return 0.3989422804014327 * std::exp(-0.5 * x * x); }

// The pre-FU-138 fit: Fisher-scoring IRLS from beta = 0 with the working weight
// phi^2 / (Phi (1-Phi)) and response eta + (y-Phi)/phi. Kept for a NON-binary
// y only (the logit_fit_irls twin; logit.cpp).
static ProbitResult probit_fit_irls(const double* X, int64_t n, int k, const double* y, int maxit, double tol,
                                    const double* offset) {
    ProbitResult r;
    r.k = k;
    std::vector<double> beta(k, 0.0), beta_prev(k, 0.0), eta(n), w(n), z(n), sw(n);
    std::vector<double> Xs(static_cast<size_t>(n) * k), zs(n), V(static_cast<size_t>(k) * k), Xtz(k);
    r.p.assign(n, 0.0);
    auto reweight = [&](const std::vector<double>& b) {
        la::mat_vec(X, n, k, b.data(), eta.data());
        for (int64_t i = 0; i < n; ++i) {
            // BUG-331: eta carries the offset; z subtracts it back out because
            // IRLS regresses (eta - offset) + (y-Phi)/phi on X. No offset -> both
            // terms are 0 and this is the prior code.
            double off = offset ? offset[i] : 0.0;
            double e = std::max(-30.0, std::min(30.0, eta[i] + off));
            double Phi = norm_cdf(e);
            double phi = norm_pdf(e);
            Phi = std::min(1.0 - 1e-12, std::max(1e-12, Phi));
            if (phi < 1e-12) phi = 1e-12;
            r.p[i] = Phi;
            double wi = (phi * phi) / (Phi * (1.0 - Phi));   // Fisher weight
            if (wi < 1e-12) wi = 1e-12;
            w[i] = wi;
            sw[i] = std::sqrt(wi);
            z[i] = (e - off) + (y[i] - Phi) / phi;           // working response
        }
    };
    // BUG-331 starting values — same reasoning as logit.cpp's block: beta=0 makes
    // eta_0 = offset, which for a real offset is far from 0 and walks IRLS off.
    // Start from the data instead: mu_i = (y_i + 1/2)/2, eta_i = Phi^{-1}(mu_i).
    // y is binary, so mu takes exactly two values and the quantiles are constants.
    // ONLY when an offset is present; the no-offset path is untouched.
    if (offset) {
        const double kEta1 = 0.6744897501960817, kEta0 = -0.6744897501960817;  // Phi^-1(3/4), (1/4)
        const double kMu1 = 0.75, kMu0 = 0.25;
        const double kPhi = 0.3177766154556257;  // phi(0.6744897501960817)
        const double kW = (kPhi * kPhi) / (kMu1 * (1.0 - kMu1));  // same at both mus
        for (int64_t i = 0; i < n; ++i) {
            double e0 = y[i] == 1.0 ? kEta1 : kEta0;
            double mu = y[i] == 1.0 ? kMu1 : kMu0;
            z[i] = e0 + (y[i] - mu) / kPhi - offset[i];
            w[i] = kW;
            sw[i] = std::sqrt(kW);
        }
        for (int c = 0; c < k; ++c) {
            const double* xc = X + static_cast<size_t>(c) * n;
            double* sc = Xs.data() + static_cast<size_t>(c) * n;
            for (int64_t i = 0; i < n; ++i) sc[i] = xc[i] * sw[i];
        }
        for (int64_t i = 0; i < n; ++i) zs[i] = z[i] * sw[i];
        la::xtx(Xs.data(), n, k, V.data());
        if (la::spd_inverse(V.data(), k) == 0) {
            la::xtv(Xs.data(), n, k, zs.data(), Xtz.data());
            la::matvec(V.data(), k, Xtz.data(), beta.data());
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
        la::xtx(Xs.data(), n, k, V.data());
        if (la::spd_inverse(V.data(), k) != 0) { r.separated = true; break; }
        la::xtv(Xs.data(), n, k, zs.data(), Xtz.data());
        beta_prev = beta;
        la::matvec(V.data(), k, Xtz.data(), beta.data());  // IRLS: full replacement
        ++r.iters;
        la::mat_vec(X, n, k, beta.data(), eta.data());
        double dev = 0;
        for (int64_t i = 0; i < n; ++i) {
            double e = std::max(-30.0, std::min(30.0, eta[i] + (offset ? offset[i] : 0.0)));
            double Phi = std::min(1.0 - 1e-15, std::max(1e-15, norm_cdf(e)));
            dev += -2.0 * (y[i] * std::log(Phi) + (1 - y[i]) * std::log(1.0 - Phi));
        }
        // Converge on the PARAMETER step, not the deviance: near the optimum the
        // deviance is quadratically flat, so |Δdev|<tol is reached while β is still
        // ~sqrt(tol) off the MLE — not tight enough for the .b rtol=1e-9 goldens.
        double bstep = 0.0;
        for (int c = 0; c < k; ++c)
            bstep = std::max(bstep, std::fabs(beta[c] - beta_prev[c]) / (std::fabs(beta[c]) + 1e-12));
        if (bstep < 1e-12 && std::fabs(dev - dev_old) / (std::fabs(dev) + 0.1) < tol) {
            r.converged = true; break;
        }
        dev_old = dev;
    }
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
    la::xtx(Xs.data(), n, k, V.data());
    la::spd_inverse(V.data(), k);
    r.V = std::move(V);
    r.beta = std::move(beta);
    return r;
}

// The ONE observed-information definition (BUG-210). See probit.h for why probit
// cannot use r.V here. The body is the loop that used to live, verbatim and
// three times over, in cmds_mle.cpp's cmd_probit, cmds_postest.cpp's
// postest_fit_glm, and cmds_postest.cpp's cmd_margins vce(robust) branch —
// including the [1e-12, 1-1e-12] clamp on Phi and the r.V fallback when the
// observed Hessian is not SPD (which the concave probit log-likelihood should
// never produce, but which those copies all guarded and so does this).
ProbitObservedInfo probit_observed_info(const ProbitResult& r, const double* X,
                                        int64_t n, int k, const double* y,
                                        const double* offset) {
    ProbitObservedInfo oi;
    oi.meat.assign(static_cast<size_t>(k) * k, 0.0);
    oi.score.assign(static_cast<size_t>(n), 0.0);
    std::vector<double> obsI(static_cast<size_t>(k) * k, 0.0);
    const double INV_SQRT_2PI = 0.3989422804014327;
    for (int64_t i = 0; i < n; ++i) {
        double Phi = std::min(1.0 - 1e-12, std::max(1e-12, r.p[static_cast<size_t>(i)]));
        double eta = offset ? offset[static_cast<size_t>(i)] : 0.0;  // BUG-331
        for (int c = 0; c < k; ++c)
            eta += X[static_cast<int64_t>(c) * n + i] * r.beta[static_cast<size_t>(c)];
        double phi = INV_SQRT_2PI * std::exp(-0.5 * eta * eta);
        double sc = (y[static_cast<size_t>(i)] - Phi) * phi / (Phi * (1.0 - Phi));
        oi.score[static_cast<size_t>(i)] = sc;
        double sc2 = sc * sc;
        double hw = sc * (sc + eta);   // observed-info weight lambda(lambda+eta)
        for (int c = 0; c < k; ++c) {
            double xc = X[static_cast<int64_t>(c) * n + i];
            for (int d = 0; d < k; ++d) {
                double xd = X[static_cast<int64_t>(d) * n + i];
                oi.meat[static_cast<size_t>(c) * k + d] += sc2 * xc * xd;
                obsI[static_cast<size_t>(c) * k + d] += hw * xc * xd;
            }
        }
    }
    // FU-210: a fit from the ml path carries the inverse of the Hessian its
    // iteration used, which drops completely determined rows as Stata's does;
    // that is the bread Stata reports (w2b_estim.log:762). The all-rows
    // observed information above stays the fallback for a fit without it.
    if (!r.bread_oim.empty()) {
        oi.bread = r.bread_oim;
        return oi;
    }
    oi.bread = std::move(obsI);
    if (la::spd_inverse(oi.bread.data(), k) != 0) {
        oi.bread = r.V;
        oi.observed_spd = false;
    }
    return oi;
}

ProbitResult probit_fit_ml(const double* X, int64_t n, int k, const double* y,
                           const MlMaxOptions& opt, const double* offset, bool has_constant,
                           const double* weights) {
    BinaryMlResult m = binary_ml_fit(BinaryLink::Probit, X, n, k, y, weights, offset, has_constant, opt);
    ProbitResult r;
    r.k = k;
    r.separated_perfect = binary_predicts_perfectly(m.p, y, n);
    // V: the Fisher bread at the ml beta (the retired IRLS fit's clamps).
    std::vector<double> sw(static_cast<size_t>(n)), Xs(static_cast<size_t>(n) * k),
        V(static_cast<size_t>(k) * k), eta(static_cast<size_t>(n));
    la::mat_vec(X, n, k, m.beta.data(), eta.data());
    for (int64_t i = 0; i < n; ++i) {
        double e = std::max(-30.0, std::min(30.0, eta[i] + (offset ? offset[i] : 0.0)));
        double Phi = std::min(1.0 - 1e-12, std::max(1e-12, norm_cdf(e)));
        double phi = norm_pdf(e);
        if (phi < 1e-12) phi = 1e-12;
        double wi = (phi * phi) / (Phi * (1.0 - Phi));
        if (wi < 1e-12) wi = 1e-12;
        sw[i] = std::sqrt(weights ? weights[i] * wi : wi);
    }
    for (int c = 0; c < k; ++c) {
        const double* xc = X + static_cast<size_t>(c) * n;
        double* sc = Xs.data() + static_cast<size_t>(c) * n;
        for (int64_t i = 0; i < n; ++i) sc[i] = xc[i] * sw[i];
    }
    la::xtx(Xs.data(), n, k, V.data());
    la::spd_inverse(V.data(), k);
    r.V = std::move(V);
    r.beta = std::move(m.beta);
    r.p = std::move(m.p);
    r.bread_oim = std::move(m.bread);
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

ProbitResult probit_fit(const double* X, int64_t n, int k, const double* y, int maxit, double tol,
                        const double* offset) {
    if (!outcome_is_binary(y, n)) return probit_fit_irls(X, n, k, y, maxit, tol, offset);
    MlMaxOptions opt;
    opt.maxiter = maxit;
    return probit_fit_ml(X, n, k, y, opt, offset, last_column_is_constant(X, n, k));
}

}  // namespace cecon
