// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "survival.h"

#include "core/threads.h"

#include <algorithm>
#include <cmath>
#include <numeric>

#include "core/linalg.h"

namespace cecon {

namespace {

inline bool miss(double v) { return std::isnan(v); }

// Sort key that keeps a strict weak ordering in the presence of Stata missings:
// a missing sorts LAST (Stata's own convention) and all missings compare equal,
// so std::stable_sort's comparator stays valid instead of tripping the NaN trap.
inline double sort_key(double v) { return std::isnan(v) ? HUGE_VAL : v; }

}  // namespace

// ---------------------------------------------------------------------------
// stset
// ---------------------------------------------------------------------------

StsetResult stset_build(const double* idv, const double* timev, const double* failv, int64_t n) {
    StsetResult r;
    r.n_total = n;
    const double kNaN = std::nan("");
    r.t0.assign(static_cast<size_t>(n), kNaN);
    r.t.assign(static_cast<size_t>(n), kNaN);
    r.d.assign(static_cast<size_t>(n), kNaN);
    r.st.assign(static_cast<size_t>(n), 0.0);
    if (!timev) {
        r.err = "stset requires a time variable";
        return r;
    }
    if (n == 0) {
        r.ok = true;
        r.single_failure = true;
        return r;
    }

    // (id, time, original row) ordering — the sequence each subject's records are
    // walked in. The original-row tiebreak makes the result deterministic when a
    // subject has two records at the same exit time.
    std::vector<int64_t> ord(static_cast<size_t>(n));
    std::iota(ord.begin(), ord.end(), static_cast<int64_t>(0));
    std::stable_sort(ord.begin(), ord.end(), [&](int64_t a, int64_t b) {
        const double ia = idv ? sort_key(idv[a]) : static_cast<double>(a);
        const double ib = idv ? sort_key(idv[b]) : static_cast<double>(b);
        if (ia != ib) return ia < ib;
        const double ta = sort_key(timev[a]), tb = sort_key(timev[b]);
        if (ta != tb) return ta < tb;
        return a < b;
    });

    // failure()-less default (Stata): the LAST record of each subject fails.
    std::vector<char> last_of_subject(static_cast<size_t>(n), 0);
    if (!failv) {
        for (int64_t q = 0; q < n; ++q) {
            const int64_t i = ord[static_cast<size_t>(q)];
            const bool last =
                (q + 1 == n) ||
                (idv ? !(sort_key(idv[ord[static_cast<size_t>(q + 1)]]) == sort_key(idv[i])) : true);
            last_of_subject[static_cast<size_t>(i)] = last ? 1 : 0;
        }
    }

    bool have_subject = false;
    double cur_id = 0, prev_exit = 0;
    bool seen_failure = false;
    int64_t kept_fail_this_subject = 0;
    bool any_multi_failure = false;
    bool have_kept = false;
    double kept_id = 0;

    for (int64_t q = 0; q < n; ++q) {
        const int64_t i = ord[static_cast<size_t>(q)];
        const double this_id = idv ? idv[i] : static_cast<double>(i);
        if (!have_subject || !(this_id == cur_id)) {
            // New subject run (ord groups equal ids together; a missing id is its
            // own run because `this_id == cur_id` is false for NaN).
            if (kept_fail_this_subject > 1) any_multi_failure = true;
            have_subject = true;
            cur_id = this_id;
            prev_exit = 0;  // origin(time 0): the first record enters at t = 0
            seen_failure = false;
            kept_fail_this_subject = 0;
        }

        const double ti = timev[i];
        const bool bad = miss(ti) || (idv && miss(this_id)) || (failv && miss(failv[i]));
        if (bad) {
            ++r.n_excl_missing;
            r.st[static_cast<size_t>(i)] = 0.0;
            continue;  // a dropped record does not advance the subject's clock
        }

        const double t0i = prev_exit;
        const double di =
            failv ? (failv[i] != 0.0 ? 1.0 : 0.0) : (last_of_subject[static_cast<size_t>(i)] ? 1.0 : 0.0);
        prev_exit = ti;

        r.t0[static_cast<size_t>(i)] = t0i;
        r.t[static_cast<size_t>(i)] = ti;
        r.d[static_cast<size_t>(i)] = di;

        bool keep;
        if (seen_failure) {
            ++r.n_excl_after_failure;
            keep = false;
        } else if (ti < t0i) {
            ++r.n_excl_negative;
            keep = false;
        } else if (ti == t0i) {
            ++r.n_excl_zero_length;
            keep = false;
        } else {
            keep = true;
        }
        if (di != 0.0) seen_failure = true;

        r.st[static_cast<size_t>(i)] = keep ? 1.0 : 0.0;
        if (!keep) continue;

        if (!have_kept) {
            have_kept = true;
            kept_id = this_id;
            ++r.n_subjects;
            r.first_entry = t0i;
            r.last_exit = ti;
        } else {
            if (!(this_id == kept_id)) {
                ++r.n_subjects;
                kept_id = this_id;
            }
            if (t0i < r.first_entry) r.first_entry = t0i;
            if (ti > r.last_exit) r.last_exit = ti;
        }
        ++r.n_kept;
        r.time_at_risk += ti - t0i;
        if (di != 0.0) {
            ++r.n_failures;
            ++kept_fail_this_subject;
        }
    }
    if (kept_fail_this_subject > 1) any_multi_failure = true;

    r.origin = 0.0;
    r.single_failure = !any_multi_failure;
    r.ok = true;
    return r;
}

// ---------------------------------------------------------------------------
// stcox
// ---------------------------------------------------------------------------

namespace {

// Everything about the sample that does not depend on beta: the two sweep orders
// and the failure-time grouping.
struct CoxWork {
    int64_t n = 0;
    int k = 0;
    std::vector<int64_t> ord_exit;   // rows by t  DESCENDING
    std::vector<int64_t> ord_entry;  // rows by t0 DESCENDING
    std::vector<double> ftime;       // DISTINCT failure times, DESCENDING
    std::vector<int64_t> fstart;     // ftime[m] covers frow[fstart[m] .. fstart[m+1])
    std::vector<int64_t> frow;       // failure row indices, grouped by ftime
    bool ok = false;
};

CoxWork cox_prepare(int64_t n, const double* t0, const double* t, const double* d) {
    CoxWork w;
    w.n = n;
    w.ord_exit.resize(static_cast<size_t>(n));
    w.ord_entry.resize(static_cast<size_t>(n));
    std::iota(w.ord_exit.begin(), w.ord_exit.end(), static_cast<int64_t>(0));
    std::iota(w.ord_entry.begin(), w.ord_entry.end(), static_cast<int64_t>(0));
    std::stable_sort(w.ord_exit.begin(), w.ord_exit.end(),
                     [&](int64_t a, int64_t b) { return t[a] > t[b]; });
    std::stable_sort(w.ord_entry.begin(), w.ord_entry.end(),
                     [&](int64_t a, int64_t b) { return t0[a] > t0[b]; });
    std::vector<int64_t> f;
    for (int64_t i = 0; i < n; ++i)
        if (d[i] != 0.0) f.push_back(i);
    std::stable_sort(f.begin(), f.end(), [&](int64_t a, int64_t b) { return t[a] > t[b]; });
    w.frow = std::move(f);
    for (size_t q = 0; q < w.frow.size(); ++q) {
        const double tq = t[w.frow[q]];
        if (w.ftime.empty() || tq != w.ftime.back()) {
            w.ftime.push_back(tq);
            w.fstart.push_back(static_cast<int64_t>(q));
        }
    }
    w.fstart.push_back(static_cast<int64_t>(w.frow.size()));
    w.ok = true;
    return w;
}

// One pass over the failure times, sweeping the risk-set accumulators.
//
// Risk set at t_j is R = { i : t0_i < t_j <= t_i }. Written as a SET DIFFERENCE,
// R = { t_i >= t_j } \ { t0_i >= t_j } (the second set is contained in the first
// because t0_i < t_i for every sample row), each side is a prefix of a
// descending sort — so both accumulators are built by ADDITION ONLY and the one
// subtraction happens at query time. That is strictly better conditioned than a
// single running accumulator that adds on entry and removes on exit, and it is
// still O(n log n): two monotone pointers, no risk set ever rebuilt.
//
// Returns LL. Fills grad (k) and info (k*k, = -Hessian) when non-null, and
// s0_out/xbar_out (per distinct failure time) when non-null.
double cox_eval(const CoxWork& w, const double* X, const double* t0, const double* t,
                const std::vector<double>& beta, std::vector<double>* grad,
                std::vector<double>* info, std::vector<double>* s0_out,
                std::vector<double>* xbar_out) {
    const int64_t n = w.n;
    const int k = static_cast<int>(beta.size());
    const size_t kk = static_cast<size_t>(k) * static_cast<size_t>(k);
    std::vector<double> eta(static_cast<size_t>(n), 0.0), wt(static_cast<size_t>(n), 0.0);
    for (int64_t i = 0; i < n; ++i) {
        double xb = 0;
        for (int c = 0; c < k; ++c) xb += X[static_cast<int64_t>(c) * n + i] * beta[static_cast<size_t>(c)];
        eta[static_cast<size_t>(i)] = xb;
        wt[static_cast<size_t>(i)] = std::exp(xb);
    }
    const size_t M = w.ftime.size();
    if (grad) grad->assign(static_cast<size_t>(k), 0.0);
    if (info) info->assign(kk, 0.0);
    if (s0_out) s0_out->assign(M, 0.0);
    if (xbar_out) xbar_out->assign(M * static_cast<size_t>(k), 0.0);

    double A0 = 0, B0 = 0;
    std::vector<double> A1(static_cast<size_t>(k), 0.0), B1(static_cast<size_t>(k), 0.0);
    std::vector<double> A2(info ? kk : 0, 0.0), B2(info ? kk : 0, 0.0);
    std::vector<double> S1(static_cast<size_t>(k), 0.0), xbar(static_cast<size_t>(k), 0.0);
    int64_t pa = 0, pb = 0;
    double LL = 0;

    auto accumulate = [&](int64_t i, double& s0, std::vector<double>& s1, std::vector<double>& s2) {
        const double wi = wt[static_cast<size_t>(i)];
        s0 += wi;
        for (int c = 0; c < k; ++c) {
            const double xc = X[static_cast<int64_t>(c) * n + i];
            s1[static_cast<size_t>(c)] += wi * xc;
            if (info)
                for (int e = 0; e < k; ++e)
                    s2[static_cast<size_t>(c) * static_cast<size_t>(k) + static_cast<size_t>(e)] +=
                        wi * xc * X[static_cast<int64_t>(e) * n + i];
        }
    };

    for (size_t m = 0; m < M; ++m) {
        const double tj = w.ftime[m];
        while (pa < n && t[w.ord_exit[static_cast<size_t>(pa)]] >= tj) {
            accumulate(w.ord_exit[static_cast<size_t>(pa)], A0, A1, A2);
            ++pa;
        }
        while (pb < n && t0[w.ord_entry[static_cast<size_t>(pb)]] >= tj) {
            accumulate(w.ord_entry[static_cast<size_t>(pb)], B0, B1, B2);
            ++pb;
        }
        const double S0 = A0 - B0;
        if (!(S0 > 0)) return -HUGE_VAL;
        for (int c = 0; c < k; ++c) {
            S1[static_cast<size_t>(c)] = A1[static_cast<size_t>(c)] - B1[static_cast<size_t>(c)];
            xbar[static_cast<size_t>(c)] = S1[static_cast<size_t>(c)] / S0;
        }
        const int64_t cnt = w.fstart[m + 1] - w.fstart[m];
        const double dcnt = static_cast<double>(cnt);
        for (int64_t q = w.fstart[m]; q < w.fstart[m + 1]; ++q) {
            const int64_t j = w.frow[static_cast<size_t>(q)];
            LL += eta[static_cast<size_t>(j)];
            if (grad)
                for (int c = 0; c < k; ++c)
                    (*grad)[static_cast<size_t>(c)] += X[static_cast<int64_t>(c) * n + j];
        }
        LL -= dcnt * std::log(S0);
        if (grad)
            for (int c = 0; c < k; ++c) (*grad)[static_cast<size_t>(c)] -= dcnt * xbar[static_cast<size_t>(c)];
        if (info)
            for (int c = 0; c < k; ++c)
                for (int e = 0; e < k; ++e) {
                    const size_t o = static_cast<size_t>(c) * static_cast<size_t>(k) + static_cast<size_t>(e);
                    (*info)[o] += dcnt * ((A2[o] - B2[o]) / S0 -
                                          xbar[static_cast<size_t>(c)] * xbar[static_cast<size_t>(e)]);
                }
        if (s0_out) (*s0_out)[m] = S0;
        if (xbar_out)
            for (int c = 0; c < k; ++c)
                (*xbar_out)[m * static_cast<size_t>(k) + static_cast<size_t>(c)] = xbar[static_cast<size_t>(c)];
    }
    return LL;
}

}  // namespace

CoxResult cox_fit(const double* X, int64_t n, int k, const double* t0, const double* t,
                  const double* d) {
    CoxResult r;
    r.n = n;
    if (n <= 0 || k <= 0) {
        r.err = "stcox: empty sample";
        return r;
    }
    for (int64_t i = 0; i < n; ++i) {
        if (!(t[i] > t0[i])) {
            r.err = "stcox: every analysis record must satisfy _t > _t0";
            return r;
        }
        if (d[i] != 0.0) ++r.n_fail;
    }
    if (r.n_fail == 0) {
        r.err = "stcox: no failures in the estimation sample";
        return r;
    }
    const CoxWork w = cox_prepare(n, t0, t, d);
    // Ties are DETECTED (not just tolerated) so the caller can pick Stata's
    // header honestly: Breslow coincides with exact/efron/etc. only while every
    // tie set is a singleton, which is what "Cox regression with no ties" claims.
    for (size_t m = 0; m < w.ftime.size(); ++m) {
        const int64_t cnt = w.fstart[m + 1] - w.fstart[m];
        if (cnt > 1) r.n_tied_failures += cnt;
    }

    std::vector<double> beta(static_cast<size_t>(k), 0.0), grad, info, step(static_cast<size_t>(k));
    r.ll_null = cox_eval(w, X, t0, t, beta, nullptr, nullptr, nullptr, nullptr);
    double prev = r.ll_null;
    int it = 0;
    for (; it < 100; ++it) {
        const double LL = cox_eval(w, X, t0, t, beta, &grad, &info, nullptr, nullptr);
        std::vector<double> inv = info;
        if (la::spd_inverse(inv.data(), k) != 0) {
            r.err = "stcox: information matrix is not invertible (collinear covariates?)";
            return r;
        }
        la::matvec(inv.data(), k, grad.data(), step.data());
        double s = 1.0;
        std::vector<double> cand(static_cast<size_t>(k));
        double candLL = LL;
        for (int bt = 0; bt < 40; ++bt) {
            for (int c = 0; c < k; ++c) cand[static_cast<size_t>(c)] = beta[static_cast<size_t>(c)] + s * step[static_cast<size_t>(c)];
            candLL = cox_eval(w, X, t0, t, cand, nullptr, nullptr, nullptr, nullptr);
            if (std::isfinite(candLL) && candLL >= LL) break;
            s *= 0.5;
        }
        beta = cand;
        if (std::fabs(candLL - prev) < 1e-12 * (std::fabs(prev) + 1e-12)) {
            prev = candLL;
            ++it;
            r.converged = true;
            break;
        }
        prev = candLL;
    }
    r.iters = it;
    r.ll = cox_eval(w, X, t0, t, beta, &grad, &info, nullptr, nullptr);
    std::vector<double> V = info;
    if (la::spd_inverse(V.data(), k) != 0) {
        r.err = "stcox: information matrix is not invertible (collinear covariates?)";
        return r;
    }
    r.vcov = V;
    r.beta = beta;
    r.se.resize(static_cast<size_t>(k));
    for (int c = 0; c < k; ++c)
        r.se[static_cast<size_t>(c)] =
            std::sqrt(std::max(0.0, V[static_cast<size_t>(c) * static_cast<size_t>(k) + static_cast<size_t>(c)]));
    r.ok = true;
    return r;
}

void cox_score_residuals(const double* X, int64_t n, int k, const double* t0, const double* t,
                         const double* d, const std::vector<double>& beta,
                         std::vector<double>* out) {
    out->assign(static_cast<size_t>(n) * static_cast<size_t>(k), 0.0);
    if (n <= 0 || k <= 0) return;
    const CoxWork w = cox_prepare(n, t0, t, d);
    std::vector<double> S0, XB;
    cox_eval(w, X, t0, t, beta, nullptr, nullptr, &S0, &XB);
    const size_t M = w.ftime.size();
    if (M == 0) return;

    // Cumulative-hazard prefix sums in ASCENDING failure-time order:
    //   F0(u) = SUM_{t_j <= u} m_j / S0_j          (m_j = # failures at t_j)
    //   F1(u) = SUM_{t_j <= u} m_j * xbar_j / S0_j
    // so the "SUM over j with t0_i < t_j <= t_i" in the score residual is just
    // F(t_i) - F(t0_i) — two binary searches per row, no O(n * #failures) loop.
    std::vector<double> at(M);                                 // failure times ASCENDING
    std::vector<double> F0(M + 1, 0.0), F1((M + 1) * static_cast<size_t>(k), 0.0);
    for (size_t m = 0; m < M; ++m) at[m] = w.ftime[M - 1 - m];
    for (size_t m = 0; m < M; ++m) {
        const size_t src = M - 1 - m;  // descending index of the m-th ascending time
        const double mult = static_cast<double>(w.fstart[src + 1] - w.fstart[src]) / S0[src];
        F0[m + 1] = F0[m] + mult;
        for (int c = 0; c < k; ++c)
            F1[(m + 1) * static_cast<size_t>(k) + static_cast<size_t>(c)] =
                F1[m * static_cast<size_t>(k) + static_cast<size_t>(c)] +
                mult * XB[src * static_cast<size_t>(k) + static_cast<size_t>(c)];
    }
    auto upto = [&](double u) -> size_t {
        return static_cast<size_t>(std::upper_bound(at.begin(), at.end(), u) - at.begin());
    };

    double* U = out->data();
    for (int64_t i = 0; i < n; ++i) {
        double xb = 0;
        for (int c = 0; c < k; ++c) xb += X[static_cast<int64_t>(c) * n + i] * beta[static_cast<size_t>(c)];
        const double wi = std::exp(xb);
        const size_t hi = upto(t[i]), lo = upto(t0[i]);
        const double dF0 = F0[hi] - F0[lo];
        if (d[i] != 0.0) {
            // t[i] is a failure time; its ascending index is hi-1.
            const size_t m = hi - 1;
            const size_t src = M - 1 - m;
            for (int c = 0; c < k; ++c)
                U[static_cast<int64_t>(c) * n + i] +=
                    X[static_cast<int64_t>(c) * n + i] - XB[src * static_cast<size_t>(k) + static_cast<size_t>(c)];
        }
        for (int c = 0; c < k; ++c) {
            const double dF1 = F1[hi * static_cast<size_t>(k) + static_cast<size_t>(c)] -
                               F1[lo * static_cast<size_t>(k) + static_cast<size_t>(c)];
            U[static_cast<int64_t>(c) * n + i] -= wi * (X[static_cast<int64_t>(c) * n + i] * dF0 - dF1);
        }
    }
}

// ---------------------------------------------------------------------------
// streg, distribution(weibull)
// ---------------------------------------------------------------------------

namespace weib {

double ll_grad(const double* X, int64_t n, int k, const double* t0, const double* t,
               const double* d, const std::vector<double>& theta, std::vector<double>* grad,
               std::vector<double>* scores) {
    const int np = k + 1;
    const double lnp = theta[static_cast<size_t>(k)];
    const double p = std::exp(lnp);
    double LL = 0;
    if (grad) std::fill(grad->begin(), grad->end(), 0.0);
    if (scores) scores->assign(static_cast<size_t>(n) * static_cast<size_t>(np), 0.0);
    double* S = scores ? scores->data() : nullptr;
    for (int64_t i = 0; i < n; ++i) {
        double xb = 0;
        for (int c = 0; c < k; ++c) xb += X[static_cast<int64_t>(c) * n + i] * theta[static_cast<size_t>(c)];
        const double ti = t[i], t0i = t0[i], di = d[i];
        const double lt = std::log(ti);
        const double tp = std::exp(p * lt);
        double t0p = 0.0, lt0 = 0.0;
        if (t0i > 0.0) {
            lt0 = std::log(t0i);
            t0p = std::exp(p * lt0);
        }
        const double e = std::exp(xb);
        const double Lam = (tp - t0p) * e;
        LL += di * (lnp + p * lt + xb) - Lam;
        // grad and S are computed by two INDEPENDENT branches, deliberately not
        // sharing an intermediate — same reasoning as tobit.h:64-73 (a hoisted
        // temp forces an extra rounding step that moves the last bits of every
        // downstream SE). Only one of the two is ever requested at a call site
        // here (the Newton loop asks for grad, the sandwich asks for scores).
        if (grad) {
            const double cb = di - Lam;
            for (int c = 0; c < k; ++c)
                (*grad)[static_cast<size_t>(c)] += cb * X[static_cast<int64_t>(c) * n + i];
            (*grad)[static_cast<size_t>(k)] += di * (1.0 + p * lt) - p * e * (tp * lt - t0p * lt0);
        }
        if (S) {
            const double cb = di - Lam;
            for (int c = 0; c < k; ++c)
                S[static_cast<int64_t>(c) * n + i] = cb * X[static_cast<int64_t>(c) * n + i];
            S[static_cast<int64_t>(k) * n + i] = di * (1.0 + p * lt) - p * e * (tp * lt - t0p * lt0);
        }
    }
    return LL;
}

}  // namespace weib

namespace {

// Central-difference Hessian of weib::ll_grad's ANALYTIC gradient == observed
// information (negated). Same recipe as tobit_fit.
void weib_hessian(const double* X, int64_t n, int k, const double* t0, const double* t,
                  const double* d, const std::vector<double>& theta, std::vector<double>* H) {
    const int np = k + 1;
    H->assign(static_cast<size_t>(np) * static_cast<size_t>(np), 0.0);
    // Column j reads only theta and j and writes only H[:, j] -- no accumulator is
    // shared between columns -- so evaluating them concurrently returns the identical
    // matrix, with no summation order to reorder. (PERFSWEEP H: same class as
    // ivml/zicount/tobit.)
    parallel_for(np, [&](int64_t jj) {
        const int j = static_cast<int>(jj);
        thread_local std::vector<double> gp, gm, tp, tm;
        gp.resize(static_cast<size_t>(np));
        gm.resize(static_cast<size_t>(np));
        const double h = 1e-6 * (std::fabs(theta[static_cast<size_t>(j)]) + 1.0);
        tp = theta;
        tm = theta;
        tp[static_cast<size_t>(j)] += h;
        tm[static_cast<size_t>(j)] -= h;
        weib::ll_grad(X, n, k, t0, t, d, tp, &gp);
        weib::ll_grad(X, n, k, t0, t, d, tm, &gm);
        for (int i = 0; i < np; ++i)
            (*H)[static_cast<size_t>(i) * static_cast<size_t>(np) + static_cast<size_t>(j)] =
                (gp[static_cast<size_t>(i)] - gm[static_cast<size_t>(i)]) / (2 * h);
    }, threads_for_work(static_cast<int64_t>(np) * n));
    for (int i = 0; i < np; ++i)
        for (int j = i + 1; j < np; ++j) {
            const size_t a = static_cast<size_t>(i) * static_cast<size_t>(np) + static_cast<size_t>(j);
            const size_t b = static_cast<size_t>(j) * static_cast<size_t>(np) + static_cast<size_t>(i);
            const double s = 0.5 * ((*H)[a] + (*H)[b]);
            (*H)[a] = (*H)[b] = s;
        }
}

// Newton-Raphson with backtracking line search on weib::ll_grad. Returns the
// converged theta; `iters`/`converged` report the path taken.
bool weib_newton(const double* X, int64_t n, int k, const double* t0, const double* t,
                 const double* d, std::vector<double>* theta, double* ll, int* iters,
                 bool* converged) {
    const int np = k + 1;
    std::vector<double> grad(static_cast<size_t>(np)), H, step(static_cast<size_t>(np));
    double prev = -HUGE_VAL;
    int it = 0;
    *converged = false;
    for (; it < 200; ++it) {
        const double LL = weib::ll_grad(X, n, k, t0, t, d, *theta, &grad);
        weib_hessian(X, n, k, t0, t, d, *theta, &H);
        std::vector<double> negH(H.size());
        for (size_t q = 0; q < H.size(); ++q) negH[q] = -H[q];
        if (la::spd_inverse(negH.data(), np) != 0) break;
        la::matvec(negH.data(), np, grad.data(), step.data());
        double s = 1.0;
        std::vector<double> cand(static_cast<size_t>(np));
        double candLL = LL;
        for (int bt = 0; bt < 60; ++bt) {
            for (int i = 0; i < np; ++i)
                cand[static_cast<size_t>(i)] = (*theta)[static_cast<size_t>(i)] + s * step[static_cast<size_t>(i)];
            candLL = weib::ll_grad(X, n, k, t0, t, d, cand, nullptr);
            if (std::isfinite(candLL) && candLL >= LL) break;
            s *= 0.5;
        }
        *theta = cand;
        if (std::fabs(candLL - prev) < 1e-11 * (std::fabs(prev) + 1e-11)) {
            prev = candLL;
            ++it;
            *converged = true;
            break;
        }
        prev = candLL;
    }
    *iters = it;
    *ll = weib::ll_grad(X, n, k, t0, t, d, *theta, nullptr);
    return std::isfinite(*ll);
}

}  // namespace

WeibullResult weibull_fit(const double* X, int64_t n, int k, const double* t0, const double* t,
                          const double* d, int cons_col) {
    WeibullResult r;
    r.n = n;
    if (n <= 0 || k <= 0) {
        r.err = "streg: empty sample";
        return r;
    }
    double risk = 0;
    for (int64_t i = 0; i < n; ++i) {
        if (!(t[i] > 0.0) || !(t0[i] >= 0.0) || !(t[i] > t0[i])) {
            r.err = "streg: every analysis record must satisfy 0 <= _t0 < _t";
            return r;
        }
        if (d[i] != 0.0) ++r.n_fail;
        risk += t[i] - t0[i];
    }
    if (r.n_fail == 0) {
        r.err = "streg: no failures in the estimation sample";
        return r;
    }

    const int np = k + 1;
    std::vector<double> theta(static_cast<size_t>(np), 0.0);
    // Start from the exponential (p = 1) MLE of the baseline rate: D / time-at-risk.
    const double rate0 = std::log(static_cast<double>(r.n_fail) / risk);
    if (cons_col >= 0 && cons_col < k) {
        // Stage 1 — constant-only prefit, exactly like Stata's "Fitting
        // constant-only model:" block; its (b_cons, ln p) then seeds the full fit
        // with the slopes at 0 ("Fitting full model:" starts at the prefit LL).
        std::vector<double> ones(static_cast<size_t>(n), 1.0);
        std::vector<double> th0(2, 0.0);
        th0[0] = rate0;
        double llc = 0;
        bool convc = false;
        int itc = 0;
        if (!weib_newton(ones.data(), n, 1, t0, t, d, &th0, &llc, &itc, &convc)) {
            r.err = "streg: constant-only prefit failed";
            return r;
        }
        r.ll_null = llc;
        r.iters_null = itc;
        theta[static_cast<size_t>(cons_col)] = th0[0];
        theta[static_cast<size_t>(k)] = th0[1];
    } else {
        theta[static_cast<size_t>(k)] = 0.0;
    }

    if (!weib_newton(X, n, k, t0, t, d, &theta, &r.ll, &r.iters, &r.converged)) {
        r.err = "streg: MLE failed";
        return r;
    }
    std::vector<double> H;
    weib_hessian(X, n, k, t0, t, d, theta, &H);
    std::vector<double> V(H.size());
    for (size_t q = 0; q < H.size(); ++q) V[q] = -H[q];
    if (la::spd_inverse(V.data(), np) != 0) {
        r.err = "streg: observed information is not invertible";
        return r;
    }
    r.vcov = V;
    r.beta.assign(theta.begin(), theta.begin() + k);
    r.se.resize(static_cast<size_t>(k));
    for (int c = 0; c < k; ++c)
        r.se[static_cast<size_t>(c)] =
            std::sqrt(std::max(0.0, V[static_cast<size_t>(c) * static_cast<size_t>(np) + static_cast<size_t>(c)]));
    r.ln_p = theta[static_cast<size_t>(k)];
    r.se_ln_p =
        std::sqrt(std::max(0.0, V[static_cast<size_t>(k) * static_cast<size_t>(np) + static_cast<size_t>(k)]));
    r.p = std::exp(r.ln_p);
    r.se_p = r.p * r.se_ln_p;          // delta method on exp(ln p)
    r.inv_p = 1.0 / r.p;
    r.se_inv_p = r.se_ln_p / r.p;      // delta method on exp(-ln p)
    r.ok = true;
    return r;
}

// ---------------------------------------------------------------------------
// shared sandwich
// ---------------------------------------------------------------------------

std::vector<double> survival_sandwich(const std::vector<double>& S, int64_t n, int np,
                                      const std::vector<double>& bread, const int32_t* codes,
                                      int n_levels, double factor) {
    const size_t pp = static_cast<size_t>(np) * static_cast<size_t>(np);
    if (bread.size() != pp) return {};
    if (S.size() != static_cast<size_t>(n) * static_cast<size_t>(np)) return {};
    std::vector<double> meat(pp, 0.0);
    if (codes && n_levels > 0) {
        std::vector<double> g(static_cast<size_t>(n_levels) * static_cast<size_t>(np), 0.0);
        for (int64_t i = 0; i < n; ++i) {
            const int32_t cl = codes[i];
            if (cl < 0 || cl >= n_levels) return {};
            for (int c = 0; c < np; ++c)
                g[static_cast<size_t>(cl) * static_cast<size_t>(np) + static_cast<size_t>(c)] +=
                    S[static_cast<int64_t>(c) * n + i];
        }
        for (int l = 0; l < n_levels; ++l)
            for (int c = 0; c < np; ++c)
                for (int e = 0; e < np; ++e)
                    meat[static_cast<size_t>(c) * static_cast<size_t>(np) + static_cast<size_t>(e)] +=
                        g[static_cast<size_t>(l) * static_cast<size_t>(np) + static_cast<size_t>(c)] *
                        g[static_cast<size_t>(l) * static_cast<size_t>(np) + static_cast<size_t>(e)];
    } else {
        la::xtx(S.data(), n, np, meat.data());
    }
    std::vector<double> V(pp, 0.0), tmp(pp, 0.0);
    la::sandwich(bread.data(), meat.data(), np, factor, V.data(), tmp.data());
    return V;
}

}  // namespace cecon
