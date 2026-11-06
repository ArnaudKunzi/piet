// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Stata-default glm lifecycle -- see glm.h for the method and its receipts.
#include "estimators/glm.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "core/rng_normal.h"  // normal_icdf (AS241): the probit start g(mu0)

namespace cecon {

namespace {

constexpr double kPi = 3.14159265358979323846;

// Neumaier-compensated sum. The log likelihood is the quantity the forward-step
// and halving rules compare, and those comparisons are where a last-bit
// difference in accumulation turns into a different path (glm.h, "Precision").
struct CSum {
    double s = 0.0, c = 0.0;
    void add(double x) {
        const double t = s + x;
        if (std::fabs(s) >= std::fabs(x)) c += (s - t) + x;
        else c += (x - t) + s;
        s = t;
    }
    double value() const { return s + c; }
};

// Inverse link at eta: mu, dmu/deta, d2mu/deta2.
void linkParts(GlmLinkFn link, double eta, double& mu, double& d, double& d2) {
    switch (link) {
        case GlmLinkFn::Identity:
            mu = eta; d = 1.0; d2 = 0.0;
            return;
        case GlmLinkFn::Log:
            mu = std::exp(eta); d = mu; d2 = mu;
            return;
        case GlmLinkFn::Logit:
            mu = 1.0 / (1.0 + std::exp(-eta));
            d = mu * (1.0 - mu);
            d2 = d * (1.0 - 2.0 * mu);
            return;
        case GlmLinkFn::Probit: {
            mu = 0.5 * std::erfc(-eta / std::sqrt(2.0));
            d = std::exp(-0.5 * eta * eta) / std::sqrt(2.0 * kPi);
            d2 = -eta * d;
            return;
        }
    }
}

// The link itself, g(mu) -- used once, on the start mu0.
double linkOf(GlmLinkFn link, double mu) {
    switch (link) {
        case GlmLinkFn::Identity: return mu;
        case GlmLinkFn::Log: return std::log(mu);
        case GlmLinkFn::Logit: return std::log(mu / (1.0 - mu));
        case GlmLinkFn::Probit: return normal_icdf(mu);
    }
    return std::numeric_limits<double>::quiet_NaN();
}

// Variance function V(mu) and dV/dmu.
void varParts(GlmFamily fam, double mu, double alpha, double& v, double& dv) {
    switch (fam) {
        case GlmFamily::Gaussian: v = 1.0; dv = 0.0; return;
        case GlmFamily::Bernoulli: v = mu * (1.0 - mu); dv = 1.0 - 2.0 * mu; return;
        case GlmFamily::Poisson: v = mu; dv = 1.0; return;
        case GlmFamily::Gamma: v = mu * mu; dv = 2.0 * mu; return;
        case GlmFamily::NegBinomial: v = mu + mu * mu * alpha; dv = 1.0 + 2.0 * mu * alpha; return;
    }
}

// Per-observation log likelihood. `sig2` is the gaussian
// scale, phi*(N-k)/N; the scale-one families multiply by 1.
double llObs(GlmFamily fam, double y, double mu, double alpha, double sig2) {
    switch (fam) {
        case GlmFamily::Gaussian:
            return -0.5 * ((y - mu) * (y - mu) / sig2 + std::log(2.0 * kPi * sig2));
        case GlmFamily::Bernoulli:
            return (y != 0.0) ? std::log(mu) : std::log1p(-mu);
        case GlmFamily::Poisson:
            return -mu + y * std::log(mu) - std::lgamma(y + 1.0);
        case GlmFamily::Gamma:
            return -y / mu + std::log(1.0 / mu);
        case GlmFamily::NegBinomial: {
            const double m = 1.0 / alpha;
            return std::lgamma(m + y) - std::lgamma(y + 1.0) - std::lgamma(m) -
                   m * std::log1p(mu / m) + y * std::log(mu / (mu + m));
        }
    }
    return std::numeric_limits<double>::quiet_NaN();
}

// Per-observation deviance.
double devObs(GlmFamily fam, double y, double mu, double alpha) {
    switch (fam) {
        case GlmFamily::Gaussian:
            return (y - mu) * (y - mu);
        case GlmFamily::Bernoulli:
            return (y != 0.0) ? -2.0 * std::log(mu) : -2.0 * std::log1p(-mu);
        case GlmFamily::Poisson:
            return (y == 0.0) ? 2.0 * mu : 2.0 * (y * std::log(y / mu) - (y - mu));
        case GlmFamily::Gamma:
            return -2.0 * (std::log(y / mu) - (y - mu) / mu);
        case GlmFamily::NegBinomial:
            return (y == 0.0) ? 2.0 * std::log1p(mu * alpha) / alpha
                              : 2.0 * (y * std::log(y / mu) - (1.0 + y * alpha) / alpha *
                                                                  std::log((1.0 + y * alpha) /
                                                                           (1.0 + mu * alpha)));
    }
    return std::numeric_limits<double>::quiet_NaN();
}

// Solve A x = b for a k*k row-major A (copied), Gaussian elimination with
// partial pivoting. The Hessian of a non-canonical link need not be positive
// definite away from the optimum, so no Cholesky assumption is made.
bool solveInPlace(std::vector<double> A, std::vector<double>& b, int k) {
    for (int c = 0; c < k; ++c) {
        int piv = c;
        for (int r = c + 1; r < k; ++r)
            if (std::fabs(A[r * k + c]) > std::fabs(A[piv * k + c])) piv = r;
        if (!(std::fabs(A[piv * k + c]) > 0.0)) return false;
        if (piv != c) {
            for (int j = 0; j < k; ++j) std::swap(A[c * k + j], A[piv * k + j]);
            std::swap(b[c], b[piv]);
        }
        for (int r = c + 1; r < k; ++r) {
            const double f = A[r * k + c] / A[c * k + c];
            if (f == 0.0) continue;
            for (int j = c; j < k; ++j) A[r * k + j] -= f * A[c * k + j];
            b[r] -= f * b[c];
        }
    }
    for (int r = k - 1; r >= 0; --r) {
        double s = b[r];
        for (int j = r + 1; j < k; ++j) s -= A[r * k + j] * b[j];
        b[r] = s / A[r * k + r];
    }
    return true;
}

bool invert(const std::vector<double>& A, std::vector<double>& out, int k) {
    out.assign(static_cast<size_t>(k) * k, 0.0);
    for (int j = 0; j < k; ++j) {
        std::vector<double> e(static_cast<size_t>(k), 0.0);
        e[j] = 1.0;
        if (!solveInPlace(A, e, k)) return false;
        for (int i = 0; i < k; ++i) out[i * k + j] = e[i];
    }
    // Symmetrize: the exact inverse of a symmetric matrix is symmetric, and a
    // posted e(V) that is not would fail Stata's own `matrix` symmetry checks.
    for (int i = 0; i < k; ++i)
        for (int j = 0; j < i; ++j) {
            const double m = 0.5 * (out[i * k + j] + out[j * k + i]);
            out[i * k + j] = m;
            out[j * k + i] = m;
        }
    return true;
}

// Scale-invariant rank test on a symmetric positive semi-definite k*k matrix.
// An exactly collinear regressor leaves an elimination pivot at ROUNDOFF level,
// not at zero, so solveInPlace would return a finite, meaningless start. Unit-
// diagonal scaling makes each Cholesky pivot 1 - R^2 of that column on the ones
// before it (uncentred), which no legitimate regressor drives to 1e-13.
bool rankDeficient(const std::vector<double>& A, int k) {
    std::vector<double> S(static_cast<size_t>(k) * k);
    for (int i = 0; i < k; ++i) {
        if (!(A[i * k + i] > 0.0)) return true;
        for (int j = 0; j < k; ++j)
            S[i * k + j] = A[i * k + j] / std::sqrt(A[i * k + i] * A[j * k + j]);
    }
    for (int c = 0; c < k; ++c) {
        double d = S[c * k + c];
        for (int j = 0; j < c; ++j) d -= S[c * k + j] * S[c * k + j];
        if (!(d > 1e-13)) return true;
        const double r = std::sqrt(d);
        S[c * k + c] = r;
        for (int i = c + 1; i < k; ++i) {
            double s = S[i * k + c];
            for (int j = 0; j < c; ++j) s -= S[i * k + j] * S[c * k + j];
            S[i * k + c] = s / r;
        }
    }
    return false;
}

double mreldif(const std::vector<double>& a, const std::vector<double>& b) {
    double m = 0.0;
    for (size_t j = 0; j < a.size(); ++j)
        m = std::max(m, std::fabs(a[j] - b[j]) / (std::fabs(b[j]) + 1.0));
    return m;
}

// One evaluation of the ml objective at `b` under the phi snapshot `phiIn`.
struct Eval {
    double lnf = 0.0;
    std::vector<double> g;     // score / phl
    std::vector<double> H;     // k*k negative Hessian / phl
    std::vector<double> sc;    // per-observation score weight w*(y-mu)*d/v (NOT divided)
    double phl = 1.0;          // the snapshot the derivatives were divided by
    double phiNew = 0.0;       // the Pearson scale at `b`, for the NEXT evaluation
    bool finite = true;
};

}  // namespace

double glm_mean(GlmLinkFn link, double eta) {
    double mu, d, d2;
    linkParts(link, eta, mu, d, d2);
    return mu;
}
double glm_dmean(GlmLinkFn link, double eta) {
    double mu, d, d2;
    linkParts(link, eta, mu, d, d2);
    return d;
}
double glm_d2mean(GlmLinkFn link, double eta) {
    double mu, d, d2;
    linkParts(link, eta, mu, d, d2);
    return d2;
}

GlmResult glm_fit(const double* X, int64_t n, int k, const double* y, const double* aweight,
                  const double* offset, const int64_t* cluster, const GlmOptions& opt) {
    GlmResult r;
    r.n = n;
    r.k = k;
    if (n <= 0 || k <= 0) {
        r.error = "no observations";
        return r;
    }
    const size_t N = static_cast<size_t>(n);
    const double dfr = static_cast<double>(n - k);
    const GlmFamily fam = opt.family;
    const GlmLinkFn link = opt.link;
    const double alpha = opt.alpha;
    const bool scaleOne = (fam == GlmFamily::Bernoulli || fam == GlmFamily::Poisson ||
                           fam == GlmFamily::NegBinomial);

    // Analytic weights normalized to mean one (sum N), as ml does for [aw].
    std::vector<double> w(N, 1.0);
    if (aweight != nullptr) {
        CSum s;
        for (size_t i = 0; i < N; ++i) s.add(aweight[i]);
        r.sum_w = s.value();
        const double mean = r.sum_w / static_cast<double>(n);
        for (size_t i = 0; i < N; ++i) w[i] = aweight[i] / mean;
    }
    auto off = [offset](size_t i) { return offset != nullptr ? offset[i] : 0.0; };
    auto eta = [&](const std::vector<double>& b, size_t i) {
        double e = 0.0;
        for (int j = 0; j < k; ++j) e += X[static_cast<size_t>(j) * N + i] * b[j];
        return e + off(i);
    };

    // ---- 1. start: mu0, one WLS step, the seeded Pearson scale ----------------
    double ybar;
    {
        CSum sy, sw;
        for (size_t i = 0; i < N; ++i) {
            sy.add(w[i] * y[i]);
            sw.add(w[i]);
        }
        ybar = sy.value() / sw.value();
    }
    std::vector<double> v0(N);
    std::vector<double> XtWX(static_cast<size_t>(k) * k, 0.0), XtWz(static_cast<size_t>(k), 0.0);
    for (size_t i = 0; i < N; ++i) {
        const double mu0 = (fam == GlmFamily::Bernoulli) ? (y[i] + 0.5) / 2.0 : (y[i] + ybar) / 2.0;
        const double e0 = linkOf(link, mu0);
        double m, d, d2, v, dv;
        linkParts(link, e0, m, d, d2);
        varParts(fam, mu0, alpha, v, dv);
        v0[i] = v;
        const double z = e0 + (y[i] - mu0) / d - off(i);
        const double W = d * d / v * w[i];
        for (int a = 0; a < k; ++a) {
            const double xa = X[static_cast<size_t>(a) * N + i];
            XtWz[a] += W * xa * z;
            for (int c = 0; c <= a; ++c) XtWX[a * k + c] += W * xa * X[static_cast<size_t>(c) * N + i];
        }
    }
    for (int a = 0; a < k; ++a)
        for (int c = 0; c < a; ++c) XtWX[c * k + a] = XtWX[a * k + c];
    std::vector<double> b = XtWz;
    if (rankDeficient(XtWX, k) || !solveInPlace(XtWX, b, k)) {
        r.error = "the starting weighted least-squares step is singular (collinear regressors)";
        return r;
    }
    double phiSeed;
    {
        CSum s;
        for (size_t i = 0; i < N; ++i) {
            double mu, d, d2;
            linkParts(link, eta(b, i), mu, d, d2);
            s.add(w[i] * (y[i] - mu) * (y[i] - mu) / v0[i]);
        }
        phiSeed = s.value() / dfr;
    }

    // ---- the ml evaluator -------------------------------------------------------
    auto evaluate = [&](const std::vector<double>& bb, double phiIn, bool derivs) {
        Eval ev;
        ev.phl = scaleOne ? 1.0 : phiIn;
        const double sig2 = phiIn * dfr / static_cast<double>(n);
        CSum lnf, pear;
        if (derivs) {
            ev.g.assign(static_cast<size_t>(k), 0.0);
            ev.H.assign(static_cast<size_t>(k) * k, 0.0);
            ev.sc.assign(N, 0.0);
        }
        for (size_t i = 0; i < N; ++i) {
            double mu, d, d2, v, dv;
            linkParts(link, eta(bb, i), mu, d, d2);
            varParts(fam, mu, alpha, v, dv);
            const double li = llObs(fam, y[i], mu, alpha, sig2);
            if (!std::isfinite(li)) ev.finite = false;
            lnf.add(w[i] * li);
            const double res = y[i] - mu;
            pear.add(w[i] * res * res / v);
            if (!derivs) continue;
            const double sc = w[i] * res * d / v;
            const double h = w[i] * (d * d / v - res * (d2 / v - d * d * dv / (v * v)));
            ev.sc[i] = sc;
            for (int a = 0; a < k; ++a) {
                const double xa = X[static_cast<size_t>(a) * N + i];
                ev.g[a] += sc * xa;
                for (int c = 0; c <= a; ++c) ev.H[a * k + c] += h * xa * X[static_cast<size_t>(c) * N + i];
            }
        }
        ev.lnf = lnf.value();
        if (!std::isfinite(ev.lnf)) ev.finite = false;
        ev.phiNew = pear.value() / dfr;
        if (derivs) {
            for (int a = 0; a < k; ++a) {
                ev.g[a] /= ev.phl;
                for (int c = 0; c <= a; ++c) {
                    ev.H[a * k + c] /= ev.phl;
                    ev.H[c * k + a] = ev.H[a * k + c];
                }
            }
        }
        return ev;
    };

    Eval cur = evaluate(b, phiSeed, true);
    if (!cur.finite) {
        r.error = "the log likelihood is not finite at the starting values";
        return r;
    }
    double phi = cur.phiNew;
    r.ilog.push_back(cur.lnf);

    // ---- 2./3. Newton-Raphson with halving and forward steps --------------------
    const bool closedForm = (fam == GlmFamily::Gaussian && link == GlmLinkFn::Identity &&
                             offset == nullptr);
    const int maxiter = closedForm ? 0 : std::max(0, opt.maxiter);
    bool converged = closedForm;
    int ic = 0;
    while (ic < maxiter && !converged) {
        std::vector<double> dvec = cur.g;
        if (!solveInPlace(cur.H, dvec, k)) {
            r.error = "the Hessian is singular";
            return r;
        }
        std::vector<double> cand(static_cast<size_t>(k));
        for (int j = 0; j < k; ++j) cand[j] = b[j] + dvec[j];
        double f1 = evaluate(cand, phi, false).lnf;
        double step = 1.0;
        std::vector<double> bn;
        if (!(f1 >= cur.lnf)) {
            int nh = 0;
            while (nh < 60 && !(f1 >= cur.lnf)) {
                for (int j = 0; j < k; ++j) cand[j] = (cand[j] + b[j]) / 2.0;
                f1 = evaluate(cand, phi, false).lnf;
                ++nh;
                step *= 0.5;
            }
            bn = cand;
        } else {
            std::vector<double> dd(static_cast<size_t>(k));
            for (int j = 0; j < k; ++j) dd[j] = 0.125 * dvec[j];
            double f0 = f1, inc = 0.125;
            for (;;) {
                std::vector<double> tryb(static_cast<size_t>(k));
                for (int j = 0; j < k; ++j) tryb[j] = cand[j] + dd[j];
                const double fc = evaluate(tryb, phi, false).lnf;
                if (!(fc > f0)) break;
                cand = tryb;
                f0 = fc;
                step += inc;
                inc *= 2.0;
                for (int j = 0; j < k; ++j) dd[j] *= 2.0;
            }
            bn = cand;
        }
        Eval next = evaluate(bn, phi, true);
        if (!next.finite) {
            r.error = "the log likelihood became non-finite during iteration";
            return r;
        }
        ++ic;
        std::vector<double> hg = next.g;
        double nr = std::numeric_limits<double>::infinity();
        if (solveInPlace(next.H, hg, k)) {
            nr = 0.0;
            for (int j = 0; j < k; ++j) nr += next.g[j] * hg[j];
        }
        const double rl = std::fabs(next.lnf - cur.lnf) / (std::fabs(cur.lnf) + 1.0);
        converged = (mreldif(bn, b) <= opt.tol || rl <= opt.ltol) && nr < opt.nrtol;
        b = bn;
        cur = next;
        phi = next.phiNew;
        r.ilog.push_back(cur.lnf);
        r.steps.push_back(step);
    }

    // ---- 4. VCE: inverse of the final (stale-phi) Hessian, or its sandwich ------
    std::vector<double> bread;
    if (!invert(cur.H, bread, k)) {
        r.error = "the Hessian at the final estimates is singular";
        return r;
    }
    if (cluster != nullptr) {
        int64_t G = 0;
        for (size_t i = 0; i < N; ++i) G = std::max(G, cluster[i] + 1);
        std::vector<double> S(static_cast<size_t>(G) * k, 0.0);
        for (size_t i = 0; i < N; ++i)
            for (int a = 0; a < k; ++a)
                S[static_cast<size_t>(cluster[i]) * k + a] +=
                    X[static_cast<size_t>(a) * N + i] * cur.sc[i] / cur.phl;
        std::vector<double> meat(static_cast<size_t>(k) * k, 0.0);
        for (int64_t g = 0; g < G; ++g)
            for (int a = 0; a < k; ++a)
                for (int c = 0; c < k; ++c)
                    meat[a * k + c] += S[static_cast<size_t>(g) * k + a] * S[static_cast<size_t>(g) * k + c];
        std::vector<double> tmp(static_cast<size_t>(k) * k, 0.0);
        for (int a = 0; a < k; ++a)
            for (int c = 0; c < k; ++c) {
                double s = 0.0;
                for (int j = 0; j < k; ++j) s += bread[a * k + j] * meat[j * k + c];
                tmp[a * k + c] = s;
            }
        const double adj = (G > 1) ? static_cast<double>(G) / static_cast<double>(G - 1) : 1.0;
        r.V.assign(static_cast<size_t>(k) * k, 0.0);
        for (int a = 0; a < k; ++a)
            for (int c = 0; c < k; ++c) {
                double s = 0.0;
                for (int j = 0; j < k; ++j) s += tmp[a * k + j] * bread[j * k + c];
                r.V[a * k + c] = s * adj;
            }
        for (int a = 0; a < k; ++a)
            for (int c = 0; c < a; ++c) {
                const double m = 0.5 * (r.V[a * k + c] + r.V[c * k + a]);
                r.V[a * k + c] = m;
                r.V[c * k + a] = m;
            }
        r.n_clusters = G;
    } else {
        r.V = bread;
    }

    // ---- post-estimation quantities at the final point --------------------------
    CSum dev, pear;
    for (size_t i = 0; i < N; ++i) {
        double mu, d, d2, v, dv;
        linkParts(link, eta(b, i), mu, d, d2);
        varParts(fam, mu, alpha, v, dv);
        dev.add(w[i] * devObs(fam, y[i], mu, alpha));
        pear.add(w[i] * (y[i] - mu) * (y[i] - mu) / v);
    }
    r.deviance = std::fabs(dev.value());
    r.deviance_p = std::fabs(pear.value());
    r.beta = b;
    r.gradient = cur.g;
    r.ll = cur.lnf;
    r.ic = ic;
    r.converged = converged;
    r.phi = scaleOne ? 1.0 : phi;
    r.ok = true;
    return r;
}

}  // namespace cecon
