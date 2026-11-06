// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "nlgmm.h"

#include "core/threads.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "core/linalg.h"
#include "vcov/vcov.h"

namespace cecon {

namespace {

constexpr int kMaxIter = 200;
constexpr int kMaxHalve = 30;

// Relative change in the objective below which the fit is declared converged.
// Both objectives are sums of squares, so a relative criterion is the scale-free
// one; 1e-13 is a few ulp above the point where the objective stops moving at
// all in double precision.
constexpr double kTolObj = 1e-13;

constexpr double kPi = 3.14159265358979323846;

// Solve A x = rhs for symmetric positive-definite A via la::spd_inverse (which
// overwrites its argument with the inverse). `A` is taken BY VALUE because it is
// destroyed; `Ainv`, when non-null, receives the inverse so a caller can reuse
// the factorisation for a covariance.
bool spd_solve(std::vector<double> A, int k, const std::vector<double>& rhs,
               std::vector<double>* x, std::vector<double>* Ainv) {
    if (la::spd_inverse(A.data(), k) != 0) return false;
    x->assign(static_cast<size_t>(k), 0.0);
    la::matvec(A.data(), k, rhs.data(), x->data());
    if (Ainv) *Ainv = A;
    return true;
}

// Levenberg damping: A += lambda * |diag(A)|, applied ONLY when plain
// Gauss-Newton's normal matrix will not factor.
void damp(std::vector<double>* A, int k, double lambda) {
    for (int i = 0; i < k; ++i) {
        double& d = (*A)[static_cast<size_t>(i) * k + i];
        d += lambda * (std::fabs(d) > 0 ? std::fabs(d) : 1.0);
    }
}

bool all_finite(const std::vector<double>& v) {
    for (double x : v)
        if (!std::isfinite(x)) return false;
    return true;
}

// Gauss-Newton step with the Levenberg escalation. Returns false when even the
// heavily damped system is unsolvable.
bool gn_step(const std::vector<double>& A, int k, const std::vector<double>& rhs,
             std::vector<double>* delta, std::vector<double>* Ainv) {
    if (spd_solve(A, k, rhs, delta, Ainv) && all_finite(*delta)) return true;
    for (double lambda = 1e-8; lambda <= 1e8; lambda *= 100.0) {
        std::vector<double> Ad = A;
        damp(&Ad, k, lambda);
        if (spd_solve(Ad, k, rhs, delta, Ainv) && all_finite(*delta)) return true;
    }
    return false;
}

}  // namespace

void nl_jacobian(const NlResidFn& resid, const std::vector<double>& b, int64_t n, int k,
                 std::vector<double>* J) {
    static const double kCbrtEps = std::cbrt(std::numeric_limits<double>::epsilon());
    J->assign(static_cast<size_t>(n) * static_cast<size_t>(k), 0.0);
    // Column j perturbs only b_j and writes only J[:, j] — no accumulator is shared
    // between columns — so evaluating them concurrently returns the identical Jacobian.
    // The perturbed parameter vectors have to become per-worker (the serial version
    // reused two, resetting the touched entry each time); everything each worker reads
    // besides them is const. (PERFSWEEP H: same class as the numeric Hessians.)
    parallel_for(k, [&](int64_t jj_) {
        const size_t jj = static_cast<size_t>(jj_);
        thread_local std::vector<double> bp, bm;
        bp = b;
        bm = b;
        const double h = kCbrtEps * std::max(std::fabs(b[jj]), 1.0);
        bp[jj] = b[jj] + h;
        bm[jj] = b[jj] - h;
        const double step = bp[jj] - bm[jj];  // the REALIZED step, not 2h — see the header
        double* col = J->data() + jj * static_cast<size_t>(n);
        for (int64_t i = 0; i < n; ++i) col[i] = (resid(bp, i) - resid(bm, i)) / step;
    }, threads_for_work(static_cast<int64_t>(k) * n));
}

// ---------------------------------------------------------------------------
// nl
// ---------------------------------------------------------------------------

NlResult nl_fit(const NlResidFn& resid_in, const double* y, int64_t n,
                const std::vector<double>& b0, const std::vector<std::string>& names,
                const double* w, bool robust) {
    NlResult r;
    r.names = names;
    r.n = n;
    r.k = static_cast<int>(b0.size());
    const int k = r.k;
    if (k == 0) { r.err = "nl: the model has no parameters"; return r; }
    if (n <= k) { r.err = "nl: need more observations than parameters"; return r; }

    // Weighted (BUG-1283): sqrt(w) * e_i(b) turns every sum below into its weighted
    // form -- the SS, J'J = J'WJ, and the robust meat's w e J.
    std::vector<double> sw;
    if (w) {
        sw.resize(static_cast<size_t>(n));
        for (int64_t i = 0; i < n; ++i) sw[static_cast<size_t>(i)] = std::sqrt(w[i]);
    }
    const NlResidFn resid = w ? NlResidFn([&](const std::vector<double>& bb, int64_t i) {
                                    return sw[static_cast<size_t>(i)] * resid_in(bb, i);
                                })
                              : resid_in;

    std::vector<double> b = b0, e(static_cast<size_t>(n));
    auto eval_rss = [&](const std::vector<double>& bb, std::vector<double>* out) {
        double s = 0;
        for (int64_t i = 0; i < n; ++i) {
            const double ei = resid(bb, i);
            if (out) (*out)[static_cast<size_t>(i)] = ei;
            s += ei * ei;
        }
        return s;
    };

    double rss = eval_rss(b, &e);
    if (!std::isfinite(rss)) {
        r.err = "nl: the residual is not finite at the initial values";
        return r;
    }
    r.iters.push_back({rss, false});

    std::vector<double> J, A, g, rhs, delta, Ainv, btry(static_cast<size_t>(k));
    for (int it = 0; it < kMaxIter; ++it) {
        // (J'J) d = -J'e, J = de/db.
        nl_jacobian(resid, b, n, k, &J);
        A.assign(static_cast<size_t>(k) * k, 0.0);
        la::xtx(J.data(), n, k, A.data());
        g.assign(static_cast<size_t>(k), 0.0);
        la::xtv(J.data(), n, k, e.data(), g.data());
        rhs.assign(static_cast<size_t>(k), 0.0);
        for (int j = 0; j < k; ++j) rhs[static_cast<size_t>(j)] = -g[static_cast<size_t>(j)];
        if (!gn_step(A, k, rhs, &delta, &Ainv)) {
            r.err = "nl: singular Gauss-Newton normal matrix (the derivative columns are collinear)";
            return r;
        }

        // Line search: full step, then halve until the objective improves. A step
        // that had to be shortened is Stata's "(backed up)".
        double scale = 1.0, rss_new = rss;
        bool improved = false, backed_up = false;
        for (int h = 0; h <= kMaxHalve; ++h) {
            for (int j = 0; j < k; ++j)
                btry[static_cast<size_t>(j)] = b[static_cast<size_t>(j)] + scale * delta[static_cast<size_t>(j)];
            rss_new = eval_rss(btry, nullptr);
            if (std::isfinite(rss_new) && rss_new < rss) { improved = true; break; }
            scale *= 0.5;
            backed_up = true;
        }
        if (!improved) {
            // The full step and every shortened one failed to improve: this IS the
            // optimum to the precision the objective can express. Stata's log ends
            // the same way — a final "(backed up)" line at an unchanged SS.
            r.iters.push_back({rss, true});
            r.converged = true;
            break;
        }
        b = btry;
        const double prev = rss;
        rss = eval_rss(b, &e);
        r.iters.push_back({rss, backed_up});
        if (std::fabs(prev - rss) <= kTolObj * (std::fabs(prev) + 1.0)) {
            r.converged = true;
            break;
        }
    }

    // Jacobian AT THE OPTIMUM for the covariance and the constant-term test: the
    // loop's last J belongs to the previous b.
    nl_jacobian(resid, b, n, k, &J);
    A.assign(static_cast<size_t>(k) * k, 0.0);
    la::xtx(J.data(), n, k, A.data());
    if (la::spd_inverse(A.data(), k) != 0) {
        r.err = "nl: (J'J) is singular at the converged parameters";
        return r;
    }

    r.beta = b;
    r.rss = rss;
    r.df_r = static_cast<double>(n - k);
    r.sigma2 = r.rss / r.df_r;

    // CONSTANT-TERM DETECTION. de/db_j is identically -1 exactly when b_j enters
    // additively, i.e. when the FITTED function's derivative column is the
    // constant 1 that Stata's note describes.
    //
    // TOLERANCE — measurement overruled the design note here. The note proposed
    // ~1e-12 relative; that is BELOW the noise floor of a central difference.
    // Differencing subtracts two O(1) residuals whose gap is O(h) ~ 1e-5, so the
    // quotient carries a cancellation error of order eps/h ~ 1e-11. On the P7
    // fixture the b0 column deviates from -1 by up to ~1e-11, which a 1e-12 test
    // would reject outright. 1e-8 sits three orders above that noise and orders
    // below any real covariate's variation, so it discriminates without being
    // fooled. tests/unit/test_nlgmm.cpp prints the measured worst deviation.
    constexpr double kConsTol = 1e-8;
    for (int j = 0; j < k && r.cons_param < 0; ++j) {
        const double* col = J.data() + static_cast<size_t>(j) * static_cast<size_t>(n);
        bool constant = true;
        for (int64_t i = 0; i < n && constant; ++i)
            constant = std::fabs((w ? col[i] / sw[static_cast<size_t>(i)] : col[i]) + 1.0) <= kConsTol;
        if (constant) r.cons_param = j;
    }

    double ybar = 0, sumw = 0;
    for (int64_t i = 0; i < n; ++i) {
        ybar += w ? w[i] * y[i] : y[i];
        sumw += w ? w[i] : 1.0;
    }
    ybar /= sumw;
    r.tss = 0;
    for (int64_t i = 0; i < n; ++i) {
        const double d = (r.cons_param >= 0) ? (y[i] - ybar) : y[i];
        r.tss += w ? w[i] * d * d : d * d;
    }
    r.df_t = (r.cons_param >= 0) ? static_cast<double>(n - 1) : static_cast<double>(n);
    r.df_m = (r.cons_param >= 0) ? static_cast<double>(k - 1) : static_cast<double>(k);
    r.mss = r.tss - r.rss;
    r.r2 = (r.tss > 0) ? 1.0 - r.rss / r.tss : 0.0;
    r.r2_adj = (r.tss > 0 && r.df_t > 0 && r.df_r > 0) ? 1.0 - (r.rss / r.df_r) / (r.tss / r.df_t) : 0.0;
    r.rmse = std::sqrt(r.sigma2);
    // "Res. dev." = -2 lnL of the Gaussian NLS model at its ML variance rss/n.
    r.res_dev = static_cast<double>(n) *
                (1.0 + std::log(2.0 * kPi) + std::log(r.rss / static_cast<double>(n)));
    if (w) {
        // Stata's nl: e(dev) = n*(1 + ln(2*pi*rss/n) - mean(ln w)), w normalized.
        double sum_lnw = 0;  // n * mean(ln w)
        for (int64_t i = 0; i < n; ++i) sum_lnw += std::log(w[i]);
        r.res_dev -= sum_lnw;
    }

    r.vcov.assign(static_cast<size_t>(k) * k, 0.0);
    if (robust) {
        // e is sqrt(w)*e and J sqrt(w)*J here, so e_i J_i is the pweighted score.
        std::vector<double> meat(static_cast<size_t>(k) * k);
        hc_meat(J.data(), n, k, e.data(), meat.data());
        la::sandwich(A.data(), meat.data(), k,
                     static_cast<double>(n) / static_cast<double>(n - k), r.vcov.data());
    } else {
        for (size_t idx = 0; idx < r.vcov.size(); ++idx) r.vcov[idx] = r.sigma2 * A[idx];
    }
    r.se.assign(static_cast<size_t>(k), 0.0);
    for (int j = 0; j < k; ++j)
        r.se[static_cast<size_t>(j)] = std::sqrt(std::max(0.0, r.vcov[static_cast<size_t>(j) * k + j]));
    r.ok = true;
    return r;
}

// ---------------------------------------------------------------------------
// gmm
// ---------------------------------------------------------------------------

GmmResult gmm_fit(const NlResidFn& resid, const double* Z, int64_t n, int L,
                  const std::vector<double>& b0, const std::vector<std::string>& names) {
    GmmResult r;
    r.names = names;
    r.n = n;
    r.k = static_cast<int>(b0.size());
    r.n_moments = L;
    const int k = r.k;
    if (k == 0) { r.err = "gmm: the model has no parameters"; return r; }
    if (L < k) { r.err = "gmm: the model needs at least as many moments as parameters"; return r; }
    if (n <= k) { r.err = "gmm: need more observations than parameters"; return r; }
    const double N = static_cast<double>(n);
    const size_t Lz = static_cast<size_t>(L);

    // W = (Z'Z/N)^-1 — Stata's winitial(unadjusted), which `onestep` leaves in place.
    std::vector<double> W(Lz * Lz, 0.0);
    la::xtx(Z, n, L, W.data());
    for (double& w : W) w /= N;
    if (la::spd_inverse(W.data(), L) != 0) {
        r.err = "gmm: the instrument cross-product Z'Z is singular";
        return r;
    }

    std::vector<double> e(static_cast<size_t>(n)), gm(Lz);
    auto moments = [&](const std::vector<double>& bb) {
        for (int64_t i = 0; i < n; ++i) e[static_cast<size_t>(i)] = resid(bb, i);
        gm.assign(Lz, 0.0);
        la::xtv(Z, n, L, e.data(), gm.data());
        for (double& v : gm) v /= N;
    };
    auto quad = [&](const std::vector<double>& v) {
        double q = 0;
        for (int a = 0; a < L; ++a) {
            double s = 0;
            for (int c = 0; c < L; ++c) s += W[static_cast<size_t>(a) * Lz + c] * v[static_cast<size_t>(c)];
            q += v[static_cast<size_t>(a)] * s;
        }
        return q;
    };

    // G = (1/N) Z' de/db'   — L x k, column-major (G[j*L + m] is row m, col j).
    std::vector<double> J;
    auto build_G = [&](const std::vector<double>& bb, std::vector<double>* G) {
        nl_jacobian(resid, bb, n, k, &J);
        G->assign(Lz * static_cast<size_t>(k), 0.0);
        for (int j = 0; j < k; ++j) {
            const double* dcol = J.data() + static_cast<size_t>(j) * static_cast<size_t>(n);
            double* gcol = G->data() + static_cast<size_t>(j) * Lz;
            la::xtv(Z, n, L, dcol, gcol);
            for (int m = 0; m < L; ++m) gcol[m] /= N;
        }
    };
    // GW = G'W  (k x L, GW[j*L + c] is row j, col c) and A = (G'W) G  (k x k).
    auto build_GW_A = [&](const std::vector<double>& G, std::vector<double>* GW,
                          std::vector<double>* A) {
        GW->assign(static_cast<size_t>(k) * Lz, 0.0);
        for (int j = 0; j < k; ++j)
            for (int c = 0; c < L; ++c) {
                double s = 0;
                for (int m = 0; m < L; ++m)
                    s += G[static_cast<size_t>(j) * Lz + m] * W[static_cast<size_t>(m) * Lz + c];
                (*GW)[static_cast<size_t>(j) * Lz + c] = s;
            }
        A->assign(static_cast<size_t>(k) * k, 0.0);
        for (int j = 0; j < k; ++j)
            for (int c = 0; c < k; ++c) {
                double s = 0;
                for (int m = 0; m < L; ++m)
                    s += (*GW)[static_cast<size_t>(j) * Lz + m] * G[static_cast<size_t>(c) * Lz + m];
                (*A)[static_cast<size_t>(j) * k + c] = s;
            }
    };

    std::vector<double> b = b0;
    moments(b);
    double q = quad(gm);
    if (!std::isfinite(q)) {
        r.err = "gmm: the moment conditions are not finite at the initial values";
        return r;
    }
    r.iters.push_back({q, false});

    std::vector<double> G, GW, A, rhs, delta, Ainv, btry(static_cast<size_t>(k));
    for (int it = 0; it < kMaxIter; ++it) {
        build_G(b, &G);
        build_GW_A(G, &GW, &A);
        rhs.assign(static_cast<size_t>(k), 0.0);
        for (int j = 0; j < k; ++j) {
            double s = 0;
            for (int m = 0; m < L; ++m) s += GW[static_cast<size_t>(j) * Lz + m] * gm[static_cast<size_t>(m)];
            rhs[static_cast<size_t>(j)] = -s;
        }
        if (!gn_step(A, k, rhs, &delta, &Ainv)) {
            r.err = "gmm: singular G'WG (the moment conditions do not identify the parameters)";
            return r;
        }

        double scale = 1.0, q_new = q;
        bool improved = false, backed_up = false;
        for (int h = 0; h <= kMaxHalve; ++h) {
            for (int j = 0; j < k; ++j)
                btry[static_cast<size_t>(j)] = b[static_cast<size_t>(j)] + scale * delta[static_cast<size_t>(j)];
            moments(btry);
            q_new = quad(gm);
            if (std::isfinite(q_new) && q_new < q) { improved = true; break; }
            scale *= 0.5;
            backed_up = true;
        }
        if (!improved) {
            r.iters.push_back({q, true});
            r.converged = true;
            break;
        }
        b = btry;
        const double prev = q;
        q = q_new;
        r.iters.push_back({q, backed_up});
        if (std::fabs(prev - q) <= kTolObj * (std::fabs(prev) + 1.0)) {
            r.converged = true;
            break;
        }
    }

    // Covariance at the optimum — recomputed rather than reused from the loop,
    // whose last G belongs to the previous b.
    moments(b);
    q = quad(gm);
    build_G(b, &G);
    build_GW_A(G, &GW, &A);
    std::vector<double> bread = A;  // G'WG
    if (la::spd_inverse(bread.data(), k) != 0) {
        r.err = "gmm: G'WG is singular at the converged parameters";
        return r;
    }

    // S = (1/N) sum_i e_i^2 z_i z_i'   (L x L)
    std::vector<double> S(Lz * Lz, 0.0);
    for (int64_t i = 0; i < n; ++i) {
        const double w = e[static_cast<size_t>(i)] * e[static_cast<size_t>(i)];
        for (int aa = 0; aa < L; ++aa) {
            const double za = w * Z[static_cast<size_t>(aa) * static_cast<size_t>(n) + i];
            for (int bb = 0; bb < L; ++bb)
                S[static_cast<size_t>(aa) * Lz + bb] += za * Z[static_cast<size_t>(bb) * static_cast<size_t>(n) + i];
        }
    }
    for (double& v : S) v /= N;

    // meat = (G'W) S (G'W)'   (k x k)
    std::vector<double> SW(static_cast<size_t>(k) * Lz, 0.0), meat(static_cast<size_t>(k) * k, 0.0);
    for (int j = 0; j < k; ++j)
        for (int c = 0; c < L; ++c) {
            double s = 0;
            for (int m = 0; m < L; ++m)
                s += GW[static_cast<size_t>(j) * Lz + m] * S[static_cast<size_t>(m) * Lz + c];
            SW[static_cast<size_t>(j) * Lz + c] = s;
        }
    for (int j = 0; j < k; ++j)
        for (int c = 0; c < k; ++c) {
            double s = 0;
            for (int m = 0; m < L; ++m)
                s += SW[static_cast<size_t>(j) * Lz + m] * GW[static_cast<size_t>(c) * Lz + m];
            meat[static_cast<size_t>(j) * k + c] = s;
        }

    // V = (1/N) bread * meat * bread. NO small-sample factor (see the header).
    r.vcov.assign(static_cast<size_t>(k) * k, 0.0);
    std::vector<double> tmp(static_cast<size_t>(k) * k, 0.0);
    la::sandwich(bread.data(), meat.data(), k, 1.0 / N, r.vcov.data(), tmp.data());

    r.beta = b;
    r.q = q;
    r.se.assign(static_cast<size_t>(k), 0.0);
    for (int j = 0; j < k; ++j)
        r.se[static_cast<size_t>(j)] = std::sqrt(std::max(0.0, r.vcov[static_cast<size_t>(j) * k + j]));
    r.ok = true;
    return r;
}

}  // namespace cecon
