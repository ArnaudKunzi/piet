// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "estimators/xtlogit.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <unordered_map>
#include <vector>

#include "core/linalg.h"
#include "core/threads.h"
#include "estimators/logit.h"  // binary_ml_fit, binary_perfect_screen

namespace cecon {

namespace {

struct Group { int64_t start, end; };

// For one panel, given xb[T] = x_t β and K = Σ y_t (K in [1, T-1]):
//   - forward recursion: f[t][k] = sum exp{Σ_{s<=t} d_s xb_s} over (d_1..d_t)
//                                  with Σ_s d_s = k
//   - backward: g[t][k] = sum over (d_{t+1}..d_T) with Σ d_s = k
//   - log B = log f[T][K]
//   - E[y_t | K] = f[t-1][·] · exp(xb_t) · g[t][K-·] / B  (sum over · = K-1)
// We work in LOG space to avoid overflow with large xb. f_log[t][k] = log f[t][k].
// We use log-sum-exp for the additions.

inline double lse2(double a, double b) {
    if (!std::isfinite(a)) return b;
    if (!std::isfinite(b)) return a;
    double m = std::max(a, b);
    return m + std::log(std::exp(a - m) + std::exp(b - m));
}

const double NEG_INF = -std::numeric_limits<double>::infinity();

// Compute log B and per-row p_t = E[d_t | K] for one panel.
// xb: length T; K: panel sum (1..T-1).
void panel_logB_p(const double* xb, int T, int K,
                  double& log_B,
                  std::vector<double>& p,
                  std::vector<double>& fmat,
                  std::vector<double>& gmat) {
    int W = T + 1;
    int64_t WW = static_cast<int64_t>(W) * W;
    std::fill(fmat.begin(), fmat.begin() + WW, NEG_INF);
    std::fill(gmat.begin(), gmat.begin() + WW, NEG_INF);

    // Forward: f[t][k] = sum over (d_1..d_t) with Σ = k of exp(Σ d_u xb_u), in log.
    fmat[0 * W + 0] = 0.0;
    for (int t = 1; t <= T; ++t) {
        int kmax = std::min(t, K);
        for (int kk = 0; kk <= kmax; ++kk) {
            double a = fmat[(t - 1) * W + kk];
            double b = (kk >= 1) ? (fmat[(t - 1) * W + kk - 1] + xb[t - 1]) : NEG_INF;
            fmat[t * W + kk] = lse2(a, b);
        }
    }
    log_B = fmat[T * W + K];

    // Backward: g[t][k] = sum over (d_{t+1}..d_T) with Σ = k.
    gmat[T * W + 0] = 0.0;
    for (int t = T - 1; t >= 0; --t) {
        int kmax = std::min(T - t, K);
        for (int kk = 0; kk <= kmax; ++kk) {
            double a = gmat[(t + 1) * W + kk];
            double b = (kk >= 1) ? (gmat[(t + 1) * W + kk - 1] + xb[t]) : NEG_INF;
            gmat[t * W + kk] = lse2(a, b);
        }
    }

    // p[t-1] = E[d_t | K] = (e^{xb_t} · Σ_j f[t-1][j] · g[t][K-1-j]) / B
    for (int t = 1; t <= T; ++t) {
        double accum = NEG_INF;
        for (int j = 0; j <= K - 1; ++j) {
            int kk = K - 1 - j;
            if (j > t - 1 || kk > T - t) continue;
            double term = fmat[(t - 1) * W + j] + xb[t - 1] + gmat[t * W + kk];
            accum = lse2(accum, term);
        }
        p[static_cast<size_t>(t - 1)] = std::exp(accum - log_B);
    }

}

// Exact conditional moments of the covariate sum, in O(T·K·k²).
//
// The Hessian term needs Σ_{t,u} q_{tu} x_t x_u' where q_{tu} = E[d_t d_u | K].
// Because d_t ∈ {0,1} (so d_t² = d_t, which is exactly why the old q had p_t on
// its diagonal), that double sum IS the second moment of the covariate sum
//   S_c = Σ_t d_t x_tc,   i.e.  Σ_{t,u} q_{tu} x_tc x_ur = E[S_c S_r | K].
// So the T×T pairwise matrix never has to be built at all: propagate the first
// and second moments through the SAME forward sweep that builds B, carrying a
// k-vector and a k×k matrix per cell instead of a scalar.
//
// With unnormalized weights u_t = e^{xb_t}, over subsets S of the first j items:
//   B_j(m) = Σ_{|S|=m} Π_{t∈S} u_t
//   A_j(m) = Σ_{|S|=m} (Σ_{t∈S} x_t) Π u        (k-vector)
//   C_j(m) = Σ_{|S|=m} (Σ_{t∈S} x_t)(Σ_{t∈S} x_t)' Π u   (k×k)
// Admitting item j+1 (weight u, covariates x) extends every subset by either
// skipping it or taking it, and taking it shifts the running sum by x:
//   B(m) += u·B(m-1)
//   A(m) += u·(A(m-1) + x·B(m-1))
//   C(m) += u·(C(m-1) + x A(m-1)' + A(m-1) x' + x x'·B(m-1))
// Descending m keeps the (m-1) cells at their previous-item values. Then
//   E[S] = A_T(K)/B_T(K),  E[S S'] = C_T(K)/B_T(K).
// This is EXACT (a re-association of the same finite sum), not an approximation.
//
// CONDITIONING — the three choices below are all forced by measurement, not taste.
//
// (0) WITHIN-PANEL CENTERING of the covariates. The consumer wants the conditional
// COVARIANCE Cov(S) = E[S S'] − E[S] E[S']. Because K is fixed by the conditioning,
// replacing x_t with (x_t − x̄_p) for a per-panel constant shifts S by the constant
// K·x̄_p, and a covariance is invariant under a constant shift — so centering changes
// the returned covariance NOT AT ALL in exact arithmetic, while in floating point it
// decides whether the Hessian survives. On the acceptance corpus `elo ≈ 1688` with
// T ≈ 875, K ≈ 444: uncentered E[S_elo] ≈ 7.5e5 and E[S_elo²] ≈ 5.6e11 while the
// true variance is ~1e6, so the subtraction cancels the leading digits away. Panels
// whose covariate is near-constant within the unit — the normal case for a slowly
// drifting team rating — are hit hardest, which is why synthetic panels that shuffle
// rows ACROSS units did not reproduce it. `mean` and `m2` are therefore returned in
// centred coordinates; the caller consumes only their difference, so this is invisible
// to it.
//
// (a) The weights are e^{xb_t - shift}. Any uniform per-item scale λ multiplies
// A_T(K), C_T(K) and B_T(K) all by λ^K, so it cancels from both ratios and the
// shift is free to choose. **shift = MEAN(xb), not max(xb).** With the max, every
// weight is ≤ 1, so B[K] is a product of K factors each ≤ 1 and underflows to
// exactly 0 on a wide panel — and then mean = A[K]/B[K] = 0/0 = NaN, which poisons
// the whole Hessian. Measured on the acceptance corpus at the β reached after
// Newton step 1: panels (T=1111, K=598) and (T=1013, K=587) both gave B[K] == 0
// under the max-shift and the run died as "non-convergent". With the mean-shift,
// B[K] over all 118 panels lands in [1.7e-19, 4.2e+98] and the worst intermediate
// cell across the whole corpus still holds 188 orders of headroom above double's
// underflow floor. The reason is structural, not luck: with the mean subtracted the
// largest K-subset has Σ(xb_t − mean) ≥ 0, so B[K] is bounded below by ~1 at the
// end, and the only live risk becomes OVERFLOW — which the rescale below handles.
//
// (b) Range is then held by rescaling all three arrays by a POWER OF TWO
// (frexp/ldexp) whenever B drifts — exact, perturbing no digit of the result. Note
// the guard watches max(B), which is the right sentinel for the overflow direction
// ONLY. It is deliberately not asked to catch underflow: B[0] is pinned at 1 (then
// scaled), so a max-watching guard can never see the leading cell collapse — that
// is exactly why the max-shift bug above was silent rather than caught here.
//
// Returns false if a panel still ends with a non-finite or non-positive B[K]; the
// caller must treat that as a numerical failure rather than divide and propagate
// NaN (BUG-131 honesty class — no silent wrong numbers).
//
// mean[k] and m2[k*k] are outputs (row-major); B/A/C/xt are caller scratch.
bool panel_moments(const double* xb, int T, int K, int k,
                   const double* Xcol, int64_t xstride, int64_t xoff,
                   std::vector<double>& B, std::vector<double>& A,
                   std::vector<double>& C, std::vector<double>& xt,
                   double* mean, double* m2) {
    const size_t kk = static_cast<size_t>(k);
    const size_t Kp = static_cast<size_t>(K) + 1;
    B.assign(Kp, 0.0);
    A.assign(Kp * kk, 0.0);
    C.assign(Kp * kk * kk, 0.0);
    xt.assign(2 * kk, 0.0);   // [0,k) = current row (centred), [k,2k) = the panel centre
    B[0] = 1.0;

    double shift = 0.0;
    for (int t = 0; t < T; ++t) shift += xb[t];
    shift /= static_cast<double>(T);

    for (int c = 0; c < k; ++c) {
        const double* xc = Xcol + static_cast<int64_t>(c) * xstride + xoff;
        double s = 0.0;
        for (int t = 0; t < T; ++t) s += xc[t];
        xt[kk + static_cast<size_t>(c)] = s / static_cast<double>(T);
    }

    for (int t = 0; t < T; ++t) {
        const double u = std::exp(xb[t] - shift);
        for (int c = 0; c < k; ++c)
            xt[static_cast<size_t>(c)] =
                Xcol[static_cast<int64_t>(c) * xstride + xoff + t] - xt[kk + static_cast<size_t>(c)];

        const int mhi = std::min(t + 1, K);
        for (int m = mhi; m >= 1; --m) {
            const size_t mm = static_cast<size_t>(m);
            const double bp = B[mm - 1];
            const double* ap = &A[(mm - 1) * kk];
            const double* cp = &C[(mm - 1) * kk * kk];
            double* cm = &C[mm * kk * kk];
            for (int c = 0; c < k; ++c) {
                const double xc = xt[static_cast<size_t>(c)];
                const double ac = ap[static_cast<size_t>(c)];
                for (int r = 0; r < k; ++r) {
                    const double xr = xt[static_cast<size_t>(r)];
                    cm[static_cast<size_t>(c) * kk + static_cast<size_t>(r)] +=
                        u * (cp[static_cast<size_t>(c) * kk + static_cast<size_t>(r)] +
                             xc * ap[static_cast<size_t>(r)] + ac * xr + xc * xr * bp);
                }
            }
            double* am = &A[mm * kk];
            for (int c = 0; c < k; ++c)
                am[static_cast<size_t>(c)] +=
                    u * (ap[static_cast<size_t>(c)] + xt[static_cast<size_t>(c)] * bp);
            B[mm] += u * bp;
        }

        // Keep B in range. Power-of-two scaling ⇒ bit-exact, ratio untouched.
        double mx = 0.0;
        for (int m = 0; m <= mhi; ++m) mx = std::max(mx, B[static_cast<size_t>(m)]);
        if (mx > 1e100 || (mx > 0.0 && mx < 1e-100)) {
            int e = 0;
            std::frexp(mx, &e);
            const double s = std::ldexp(1.0, -e);
            for (auto& v : B) v *= s;
            for (auto& v : A) v *= s;
            for (auto& v : C) v *= s;
        }
    }

    const double bK = B[static_cast<size_t>(K)];
    const double* aK = &A[static_cast<size_t>(K) * kk];
    const double* cK = &C[static_cast<size_t>(K) * kk * kk];
    if (!(bK > 0.0) || !std::isfinite(bK)) return false;
    for (int c = 0; c < k; ++c) mean[c] = aK[static_cast<size_t>(c)] / bK;
    for (size_t i = 0; i < kk * kk; ++i) m2[i] = cK[i] / bK;
    for (int c = 0; c < k; ++c) if (!std::isfinite(mean[c])) return false;
    for (size_t i = 0; i < kk * kk; ++i) if (!std::isfinite(m2[i])) return false;
    return true;
}

// The slopes of an ordinary logit of y on X plus a constant over the usable rows, the
// point Stata's clogit starts from, identified from its ado. That logit stops early, on
// logit's undocumented doopt option (tolerance 1e-4 on the coefficients, no
// log-likelihood or nrtolerance check, 20 iterations, halving only), so its reported
// vector is not the logit MLE, and the distance reaches e(ic) and the last digits of the
// conditional fit. Empty when the logit cannot give every regressor a coefficient.
std::vector<double> pooled_logit_slopes(const double* X, int64_t N, int k, const double* y) {
    const size_t rows = static_cast<size_t>(N);
    const size_t slopes = static_cast<size_t>(k);
    std::vector<double> design(rows * (slopes + 1), 1.0);  // the trailing column stays 1
    std::copy_n(X, rows * slopes, design.begin());

    const BinaryPerfectScreen screen = binary_perfect_screen(design.data(), N, k + 1, k, y);
    const auto dropped_col = screen.omit_col.begin();
    if (screen.inestimable ||
        std::any_of(dropped_col, dropped_col + k, [](char omitted) { return omitted != 0; }))
        return {};

    MlMaxOptions early;
    early.tol = 1e-4;
    early.ltol = 0.0;
    early.maxiter = 20;
    early.doopt = true;
    early.step_forward = false;
    early.ignore_nrtol = true;
    BinaryMlResult fit = binary_ml_fit(BinaryLink::Logit, design.data(), N, k + 1, y, nullptr, nullptr,
                                       /*has_constant=*/true, early);
    if (fit.singular || fit.beta.size() != slopes + 1) return {};
    fit.beta.pop_back();  // the constant is not a parameter of the conditional model
    return std::move(fit.beta);
}

// Where xtlogit_fe's Newton iteration starts. X: N x k column-major over the usable panels
// (packed group by group), y: their 0/1 outcomes, ll_at: the conditional log likelihood
// at a coefficient vector. The zero vector stands in whenever the pooled logit has no
// answer or its answer gives a log likelihood the iteration could not work from.
std::vector<double> xtlogit_fe_start(const double* X, int64_t N, int k, const double* y,
                                     const std::function<double(const std::vector<double>&)>& ll_at) {
    const std::vector<double> zeros(static_cast<size_t>(k), 0.0);
    if (N <= 0) return zeros;
    const std::vector<double> slopes = pooled_logit_slopes(X, N, k, y);
    if (slopes.empty() || !std::all_of(slopes.begin(), slopes.end(), [](double v) { return std::isfinite(v); }))
        return zeros;
    constexpr double kLargestWorkableLl = 1e50;
    return std::fabs(ll_at(slopes)) <= kLargestWorkableLl ? slopes : zeros;
}

}  // namespace

int xtlogit_fanout_threads(int ng, int Tmax, int k) {
    // Work: ng panels × an O((T+1)^2) recursion pair, ~8 elementary ops per table
    // cell across the forward/backward tables and the moment accumulation (an
    // estimate keyed to the Hessian sites' calibrated 100k-op break-even, not a
    // measured constant of its own). Scratch per worker thread: the thread_local
    // vectors below — 2 (Tmax+1)^2 tables + 2 Tmax + k + k^2 + the k×T moment
    // buffers, over-approximated as 4·k·Tmax, plus slack — in doubles.
    const int64_t W = static_cast<int64_t>(Tmax + 1) * static_cast<int64_t>(Tmax + 1);
    const int64_t units = static_cast<int64_t>(ng) * W * 8;
    const int64_t kk = static_cast<int64_t>(k);
    const int64_t scratch_bytes =
        8 * (2 * W + 2 * static_cast<int64_t>(Tmax) + kk * kk + kk + 4 * kk * Tmax + 64);
    return threads_for_work(units, scratch_bytes);
}

XtlogitFeResult xtlogit_fe(const double* X_in, int64_t n_obs, int k,
                           const double* y_in, const double* ivar_in,
                           int maxit) {
    XtlogitFeResult R;
    R.k = k;

    // ---- 1. Group rows by ivar (first-appearance order).
    std::unordered_map<double, int> idx_of;
    std::vector<std::vector<int64_t>> rows_per_g;
    for (int64_t r = 0; r < n_obs; ++r) {
        double v = ivar_in[r];
        auto it = idx_of.find(v);
        int gi;
        if (it == idx_of.end()) {
            gi = static_cast<int>(rows_per_g.size());
            idx_of[v] = gi;
            rows_per_g.emplace_back();
        } else gi = it->second;
        rows_per_g[static_cast<size_t>(gi)].push_back(r);
    }

    // ---- 2. Drop K=0 or K=T panels, and size<2 panels.
    std::vector<std::vector<int64_t>> usable;
    for (const auto& rs : rows_per_g) {
        if (rs.size() < 2) { ++R.n_dropped_size; continue; }
        double sy = 0;
        for (int64_t r : rs) sy += y_in[r];
        if (sy <= 0 || sy >= static_cast<double>(rs.size())) {
            ++R.n_dropped_const;
            continue;
        }
        usable.push_back(rs);
    }
    R.n_groups = static_cast<int64_t>(usable.size());
    if (!usable.empty()) {
        int64_t tot = 0;
        R.g_min = -1;
        for (const auto& rs : usable) {
            const int64_t T = static_cast<int64_t>(rs.size());
            R.g_min = R.g_min < 0 ? T : std::min(R.g_min, T);
            R.g_max = std::max(R.g_max, T);
            tot += T;
        }
        R.g_avg = static_cast<double>(tot) / static_cast<double>(usable.size());
    }

    // Pack working arrays group-contiguous.
    int64_t N = 0;
    for (const auto& rs : usable) N += static_cast<int64_t>(rs.size());
    R.n_obs = N;
    std::vector<Group> groups; groups.reserve(usable.size());
    std::vector<double> X(static_cast<size_t>(N) * k);
    std::vector<double> y(static_cast<size_t>(N));
    int64_t cur = 0;
    int Tmax = 0;
    for (const auto& rs : usable) {
        Group g{cur, cur + static_cast<int64_t>(rs.size())};
        if (static_cast<int>(rs.size()) > Tmax) Tmax = static_cast<int>(rs.size());
        groups.push_back(g);
        for (int64_t r : rs) {
            y[static_cast<size_t>(cur)] = y_in[r];
            for (int c = 0; c < k; ++c)
                X[static_cast<size_t>(c) * N + cur] = X_in[static_cast<int64_t>(c) * n_obs + r];
            ++cur;
        }
    }

    R.beta.assign(static_cast<size_t>(k), 0.0);
    std::vector<double> xb(static_cast<size_t>(N), 0.0);
    std::vector<double> p(static_cast<size_t>(N), 0.0);
    std::vector<double> score(static_cast<size_t>(k), 0.0);
    std::vector<double> negH(static_cast<size_t>(k) * k, 0.0);
    std::vector<double> negH_inv(static_cast<size_t>(k) * k, 0.0);
    std::vector<double> step(static_cast<size_t>(k), 0.0);
    // Both panel passes below allocate their own per-thread scratch, so the shared serial
    // buffers this function used to carry (fmat/gmat/xb_g/p_g/xbar_g — and the Tmax×Tmax
    // pairwise `q_g`, the O(T²) allocation this rewrite exists to remove) are all gone.
    std::vector<double> per_grp_score;

    // ---- 3. The optimizer ------------------------------------------------------------
    // Newton-Raphson on the conditional log likelihood with the analytic Hessian, from the
    // start xtlogit_fe_start chooses: a full step, halved while it does not improve ll,
    // else extended while each extension improves; stop when (max relative change in b
    // <= 1e-6 or relative change in ll <= 1e-7) and g inv(-H) g' < 1e-5 ([R] maximize
    // defaults). V = inv(-H) at the final b. `maxit` is iterate(), 300 by default.
    // Moment pass at R.beta: fills ll, score and negH. Returns false when a panel fails.
    auto full_eval = [&](double& ll_out) -> bool {
        // Recompute xb under current β.
        for (int64_t i = 0; i < N; ++i) {
            double s = 0;
            for (int c = 0; c < k; ++c) s += X[static_cast<size_t>(c) * N + i] * R.beta[static_cast<size_t>(c)];
            xb[static_cast<size_t>(i)] = s;
        }

        std::fill(score.begin(), score.end(), 0.0);
        std::fill(negH.begin(), negH.end(), 0.0);

        // Panels are independent: compute each panel's contribution in PARALLEL into
        // per-group buffers, then reduce SEQUENTIALLY in group order. The reduction does
        // the same additions in the same order as the old serial loop, so the result is
        // BIT-IDENTICAL (and identical across thread counts) — only faster. The per-panel
        // conditional-logit work is the per-panel recursion pair, so this is the hot loop.
        const int ng = static_cast<int>(groups.size());
        std::vector<double> ll_g(static_cast<size_t>(ng), 0.0);
        std::vector<double> score_g(static_cast<size_t>(ng) * static_cast<size_t>(k), 0.0);
        std::vector<double> negH_g(static_cast<size_t>(ng) * static_cast<size_t>(k) * static_cast<size_t>(k), 0.0);
        // 1 = this panel's moments came back finite. A panel that fails is a numerical
        // failure of the WHOLE fit — never a silently dropped observation, never a NaN
        // allowed downstream (BUG-131 honesty class).
        std::vector<char> ok_g(static_cast<size_t>(ng), 1);
        // Fan-out sized by the settings-derived envelope (core budget via
        // default_threads(), memory budget via memory_budget_bytes()) — see
        // xtlogit_fanout_threads. Bit-neutral by construction: each panel writes
        // only its own slot and the reduction below is serial, in gi order.
        const int xtlogit_threads = xtlogit_fanout_threads(ng, Tmax, k);
        parallel_for(ng, [&](int64_t gi) {
            const Group& g = groups[static_cast<size_t>(gi)];
            int T = static_cast<int>(g.end - g.start);
            // Per-worker scratch, not per-panel. `parallel_for` spawns its threads for
            // the duration of one call, so a thread_local here is built once per worker
            // per call instead of once per panel — 10 vectors × ng allocations collapse
            // to 10 × nthreads, and the two biggest (the (T+1)² forward/backward tables)
            // stop churning ~13 KB blocks thousands of times per Newton step.
            // Sizing to Tmax is safe because every consumer indexes by the explicit
            // T/W/k it was handed, never by the buffer's size: panel_logB_p fills
            // [begin, begin+WW) before reading inside it, and panel_moments .assign()s
            // mB/mA/mC/mx outright. A longer tail is therefore never read.
            thread_local std::vector<double> xb_l, p_l, f_l, g_l, xbar, m2, mB, mA, mC, mx;
            if (xb_l.size() < static_cast<size_t>(Tmax)) {
                xb_l.resize(static_cast<size_t>(Tmax));
                p_l.resize(static_cast<size_t>(Tmax));
            }
            const size_t wneed = static_cast<size_t>(Tmax + 1) * static_cast<size_t>(Tmax + 1);
            if (f_l.size() < wneed) { f_l.resize(wneed); g_l.resize(wneed); }
            if (xbar.size() != static_cast<size_t>(k)) {
                xbar.assign(static_cast<size_t>(k), 0.0);
                m2.assign(static_cast<size_t>(k) * static_cast<size_t>(k), 0.0);
            }
            int K = 0;
            double yb_sum_xb = 0;  // Σ y_t xb_t
            for (int t = 0; t < T; ++t) {
                xb_l[static_cast<size_t>(t)] = xb[static_cast<size_t>(g.start + t)];
                K += static_cast<int>(y[static_cast<size_t>(g.start + t)]);
                yb_sum_xb += y[static_cast<size_t>(g.start + t)] * xb_l[static_cast<size_t>(t)];
            }
            double log_B;
            panel_logB_p(xb_l.data(), T, K, log_B, p_l, f_l, g_l);
            if (!panel_moments(xb_l.data(), T, K, k, X.data(), N, g.start, mB, mA, mC, mx,
                               xbar.data(), m2.data())) {
                ok_g[static_cast<size_t>(gi)] = 0;
                return;
            }
            ll_g[static_cast<size_t>(gi)] = yb_sum_xb - log_B;
            double* sc = &score_g[static_cast<size_t>(gi) * static_cast<size_t>(k)];
            double* hh = &negH_g[static_cast<size_t>(gi) * static_cast<size_t>(k) * static_cast<size_t>(k)];
            // Score Σ_t (y_t − p_t)·x_tc, computed on the covariate CENTRED within the
            // panel — the same exact-invariance argument as the Hessian: Σ_t (y_t − p_t)
            // is exactly 0 (both sides sum to K), so subtracting any per-panel constant
            // changes the score by x̄_p·0 = 0 in exact arithmetic. In floating point it
            // removes x̄_p × (the rounding residual of Σ(y−p)), which for `elo`-shaped
            // data (x̄ ≈ 1700, T = 1200) is ~1e-10 of pure noise — enough to keep the
            // Newton step above tol forever. MEASURED: without this the offset arm of
            // the BUG-135 leg burns all 100 iterations and reports converged=false while
            // its β already agrees with the centred arm to 5.6e-9.
            for (int c = 0; c < k; ++c) {
                const double* xc = &X[static_cast<size_t>(c) * N + g.start];
                double ctr = 0;
                for (int t = 0; t < T; ++t) ctr += xc[t];
                ctr /= static_cast<double>(T);
                double s = 0;
                for (int t = 0; t < T; ++t) {
                    double r = y[static_cast<size_t>(g.start + t)] - p_l[static_cast<size_t>(t)];
                    s += r * (xc[t] - ctr);
                }
                sc[static_cast<size_t>(c)] = s;
            }
            // EXACT conditional Hessian term Cov(S) = E[S S'] − E[S]E[S']
            // (== the old Σ_{t,u} q_{tu} x_t x_u' − x̄ x̄', without the T×T matrix).
            for (int c = 0; c < k; ++c) {
                double xbar_c = xbar[static_cast<size_t>(c)];
                for (int rr = 0; rr < k; ++rr)
                    hh[static_cast<size_t>(c) * static_cast<size_t>(k) + rr] =
                        m2[static_cast<size_t>(c) * static_cast<size_t>(k) + rr] -
                        xbar_c * xbar[static_cast<size_t>(rr)];
            }
        }, xtlogit_threads);
        double ll = 0;
        for (int gi = 0; gi < ng; ++gi)
            if (!ok_g[static_cast<size_t>(gi)]) return false;
        for (int gi = 0; gi < ng; ++gi) {
            ll += ll_g[static_cast<size_t>(gi)];
            const double* sc = &score_g[static_cast<size_t>(gi) * static_cast<size_t>(k)];
            const double* hh = &negH_g[static_cast<size_t>(gi) * static_cast<size_t>(k) * static_cast<size_t>(k)];
            for (int c = 0; c < k; ++c) {
                score[static_cast<size_t>(c)] += sc[static_cast<size_t>(c)];
                for (int rr = 0; rr < k; ++rr)
                    negH[static_cast<size_t>(c) * static_cast<size_t>(k) + rr] +=
                        hh[static_cast<size_t>(c) * static_cast<size_t>(k) + rr];
            }
        }
        ll_out = ll;
        return true;
    };
    // ll only (the line-search evaluations): the same forward recursion and the same
    // per-panel sum order as full_eval, so a point's ll has the same bits either way.
    auto ll_only = [&](const std::vector<double>& b) -> double {
        const int ng = static_cast<int>(groups.size());
        std::vector<double> ll_g(static_cast<size_t>(ng), 0.0);
        const int xtlogit_threads = xtlogit_fanout_threads(ng, Tmax, k);
        parallel_for(ng, [&](int64_t gi) {
            const Group& g = groups[static_cast<size_t>(gi)];
            const int T = static_cast<int>(g.end - g.start);
            thread_local std::vector<double> xb_l, p_l, f_l, g_l;
            if (xb_l.size() < static_cast<size_t>(Tmax)) {
                xb_l.resize(static_cast<size_t>(Tmax));
                p_l.resize(static_cast<size_t>(Tmax));
            }
            const size_t wneed = static_cast<size_t>(Tmax + 1) * static_cast<size_t>(Tmax + 1);
            if (f_l.size() < wneed) { f_l.resize(wneed); g_l.resize(wneed); }
            int K = 0;
            double yb_sum_xb = 0;
            for (int t = 0; t < T; ++t) {
                double s = 0;
                for (int c = 0; c < k; ++c)
                    s += X[static_cast<size_t>(c) * N + g.start + t] * b[static_cast<size_t>(c)];
                xb_l[static_cast<size_t>(t)] = s;
                K += static_cast<int>(y[static_cast<size_t>(g.start + t)]);
                yb_sum_xb += y[static_cast<size_t>(g.start + t)] * s;
            }
            double log_B;
            panel_logB_p(xb_l.data(), T, K, log_B, p_l, f_l, g_l);
            ll_g[static_cast<size_t>(gi)] = yb_sum_xb - log_B;
        }, xtlogit_threads);
        double ll = 0;
        for (int gi = 0; gi < ng; ++gi) ll += ll_g[static_cast<size_t>(gi)];
        return ll;
    };

    R.beta = xtlogit_fe_start(X.data(), N, k, y.data(), ll_only);

    double ll = 0.0;
    if (!full_eval(ll)) { R.converged = false; R.iters = 0; return R; }
    bool conv = false;
    int ic = 0;
    while (ic < maxit && !conv) {
        std::copy(negH.begin(), negH.end(), negH_inv.begin());
        if (la::spd_inverse(negH_inv.data(), k) != 0) {
            R.converged = false; R.iters = ic; R.loglik = ll; return R;
        }
        la::matvec(negH_inv.data(), k, score.data(), step.data());
        const std::vector<double> b0 = R.beta;
        std::vector<double> cand(static_cast<size_t>(k));
        for (int c = 0; c < k; ++c) cand[static_cast<size_t>(c)] = b0[static_cast<size_t>(c)] + step[static_cast<size_t>(c)];
        double f1 = ll_only(cand);
        if (!(f1 >= ll)) {
            for (int nh = 0; nh < 60 && !(f1 >= ll); ++nh) {
                for (int c = 0; c < k; ++c)
                    cand[static_cast<size_t>(c)] = (cand[static_cast<size_t>(c)] + b0[static_cast<size_t>(c)]) / 2.0;
                f1 = ll_only(cand);
            }
        } else {
            std::vector<double> dd(static_cast<size_t>(k)), tryb(static_cast<size_t>(k));
            for (int c = 0; c < k; ++c) dd[static_cast<size_t>(c)] = 0.125 * step[static_cast<size_t>(c)];
            double f0 = f1;
            for (int nf = 0; nf < 100; ++nf) {
                for (int c = 0; c < k; ++c) tryb[static_cast<size_t>(c)] = cand[static_cast<size_t>(c)] + dd[static_cast<size_t>(c)];
                const double fc = ll_only(tryb);
                if (!(fc > f0)) break;
                cand = tryb;
                f0 = fc;
                for (int c = 0; c < k; ++c) dd[static_cast<size_t>(c)] *= 2.0;
            }
        }
        R.beta = cand;
        const double ll_old = ll;
        if (!full_eval(ll)) { R.converged = false; R.iters = ic + 1; return R; }
        ++ic;
        std::copy(negH.begin(), negH.end(), negH_inv.begin());
        double nr = std::numeric_limits<double>::infinity();
        if (la::spd_inverse(negH_inv.data(), k) == 0) {
            la::matvec(negH_inv.data(), k, score.data(), step.data());
            nr = 0.0;
            for (int c = 0; c < k; ++c) nr += score[static_cast<size_t>(c)] * step[static_cast<size_t>(c)];
        }
        double pconv = 0.0;
        for (int c = 0; c < k; ++c)
            pconv = std::max(pconv, std::fabs(cand[static_cast<size_t>(c)] - b0[static_cast<size_t>(c)]) /
                                        (std::fabs(b0[static_cast<size_t>(c)]) + 1.0));
        const double rl = std::fabs(ll - ll_old) / (std::fabs(ll_old) + 1.0);
        conv = (pconv <= 1e-6 || rl <= 1e-7) && nr < 1e-5;
    }
    R.iters = ic;
    R.loglik = ll;
    R.converged = conv;

    // ---- Final pass: per-group score for cluster-robust SE.
    for (int64_t i = 0; i < N; ++i) {
        double s = 0;
        for (int c = 0; c < k; ++c) s += X[static_cast<size_t>(c) * N + i] * R.beta[static_cast<size_t>(c)];
        xb[static_cast<size_t>(i)] = s;
    }
    std::fill(negH.begin(), negH.end(), 0.0);
    per_grp_score.assign(static_cast<size_t>(R.n_groups) * k, 0.0);
    // Same shape as the iteration loop above — this pass used to be the ONLY serial
    // full-cost sweep left (two more panel passes at single-thread), so it dominated the
    // tail. Panels in PARALLEL into per-group buffers, reduced SEQUENTIALLY in group
    // order: the additions happen in the same order as the old serial loop, so the result
    // is independent of thread count. per_grp_score is already indexed by group, so each
    // panel writes its own slice.
    {
        const int ng = static_cast<int>(groups.size());
        std::vector<double> negH_g(static_cast<size_t>(ng) * static_cast<size_t>(k) * static_cast<size_t>(k), 0.0);
        // 1 = this panel's moments came back finite. A panel that fails is a numerical
        // failure of the WHOLE fit — never a silently dropped observation, never a NaN
        // allowed downstream (BUG-131 honesty class).
        std::vector<char> ok_g(static_cast<size_t>(ng), 1);
        // Fan-out sized by the settings-derived envelope (core budget via
        // default_threads(), memory budget via memory_budget_bytes()) — see
        // xtlogit_fanout_threads. Bit-neutral by construction: each panel writes
        // only its own slot and the reduction below is serial, in gi order.
        const int xtlogit_threads = xtlogit_fanout_threads(ng, Tmax, k);
        parallel_for(ng, [&](int64_t gi) {
            const Group& g = groups[static_cast<size_t>(gi)];
            int T = static_cast<int>(g.end - g.start);
            // Same per-worker scratch as the Newton pass above; see the note there.
            thread_local std::vector<double> xb_l, p_l, f_l, g_l, xbar, m2, mB, mA, mC, mx;
            if (xb_l.size() < static_cast<size_t>(Tmax)) {
                xb_l.resize(static_cast<size_t>(Tmax));
                p_l.resize(static_cast<size_t>(Tmax));
            }
            const size_t wneed = static_cast<size_t>(Tmax + 1) * static_cast<size_t>(Tmax + 1);
            if (f_l.size() < wneed) { f_l.resize(wneed); g_l.resize(wneed); }
            if (xbar.size() != static_cast<size_t>(k)) {
                xbar.assign(static_cast<size_t>(k), 0.0);
                m2.assign(static_cast<size_t>(k) * static_cast<size_t>(k), 0.0);
            }
            int K = 0;
            for (int t = 0; t < T; ++t) {
                xb_l[static_cast<size_t>(t)] = xb[static_cast<size_t>(g.start + t)];
                K += static_cast<int>(y[static_cast<size_t>(g.start + t)]);
            }
            double log_B;
            panel_logB_p(xb_l.data(), T, K, log_B, p_l, f_l, g_l);
            if (!panel_moments(xb_l.data(), T, K, k, X.data(), N, g.start, mB, mA, mC, mx,
                               xbar.data(), m2.data())) {
                ok_g[static_cast<size_t>(gi)] = 0;
                return;
            }
            double* pgs = &per_grp_score[static_cast<size_t>(gi) * static_cast<size_t>(k)];
            // Per-group score for the cluster-robust sandwich — centred for the same
            // reason as the Newton score above (exactly invariant, numerically decisive).
            for (int c = 0; c < k; ++c) {
                const double* xc = &X[static_cast<size_t>(c) * N + g.start];
                double ctr = 0;
                for (int t = 0; t < T; ++t) ctr += xc[t];
                ctr /= static_cast<double>(T);
                double s = 0;
                for (int t = 0; t < T; ++t) {
                    double r = y[static_cast<size_t>(g.start + t)] - p_l[static_cast<size_t>(t)];
                    s += r * (xc[t] - ctr);
                }
                pgs[static_cast<size_t>(c)] = s;
            }
            double* hh = &negH_g[static_cast<size_t>(gi) * static_cast<size_t>(k) * static_cast<size_t>(k)];
            for (int c = 0; c < k; ++c) {
                double xbar_c = xbar[static_cast<size_t>(c)];
                for (int rr = 0; rr < k; ++rr)
                    hh[static_cast<size_t>(c) * static_cast<size_t>(k) + rr] =
                        m2[static_cast<size_t>(c) * static_cast<size_t>(k) + rr] -
                        xbar_c * xbar[static_cast<size_t>(rr)];
            }
        }, xtlogit_threads);
        for (int gi = 0; gi < ng; ++gi)
            if (!ok_g[static_cast<size_t>(gi)]) { R.converged = false; return R; }
        for (int gi = 0; gi < ng; ++gi) {
            const double* hh = &negH_g[static_cast<size_t>(gi) * static_cast<size_t>(k) * static_cast<size_t>(k)];
            for (int c = 0; c < k; ++c)
                for (int rr = 0; rr < k; ++rr)
                    negH[static_cast<size_t>(c) * static_cast<size_t>(k) + rr] +=
                        hh[static_cast<size_t>(c) * static_cast<size_t>(k) + rr];
        }
    }
    std::copy(negH.begin(), negH.end(), negH_inv.begin());
    if (la::spd_inverse(negH_inv.data(), k) != 0) {
        R.converged = false; return R;
    }
    R.se_iid.assign(static_cast<size_t>(k), 0.0);
    R.V_iid = negH_inv;
    for (int c = 0; c < k; ++c)
        R.se_iid[static_cast<size_t>(c)] =
            std::sqrt(std::max(0.0, negH_inv[static_cast<size_t>(c) * k + c]));

    // Cluster-robust by panel (same machinery as xtpoisson).
    std::vector<double> G(static_cast<size_t>(k) * k, 0.0);
    for (int64_t gi = 0; gi < R.n_groups; ++gi)
        for (int c = 0; c < k; ++c) {
            double sc = per_grp_score[static_cast<size_t>(gi) * k + c];
            for (int rr = 0; rr < k; ++rr)
                G[static_cast<size_t>(c) * k + rr] += sc * per_grp_score[static_cast<size_t>(gi) * k + rr];
        }
    std::vector<double> tmp(static_cast<size_t>(k) * k, 0.0);
    for (int c = 0; c < k; ++c)
        for (int rr = 0; rr < k; ++rr) {
            double s = 0;
            for (int j = 0; j < k; ++j)
                s += negH_inv[static_cast<size_t>(j) * k + rr] * G[static_cast<size_t>(c) * k + j];
            tmp[static_cast<size_t>(c) * k + rr] = s;
        }
    std::vector<double> Vr(static_cast<size_t>(k) * k, 0.0);
    for (int c = 0; c < k; ++c)
        for (int rr = 0; rr < k; ++rr) {
            double s = 0;
            for (int j = 0; j < k; ++j)
                s += tmp[static_cast<size_t>(j) * k + rr] * negH_inv[static_cast<size_t>(c) * k + j];
            Vr[static_cast<size_t>(c) * k + rr] = s;     // q=1 (Stata's xtlogit, fe)
        }
    R.se_robust.assign(static_cast<size_t>(k), 0.0);
    R.V_robust = Vr;
    for (int c = 0; c < k; ++c)
        R.se_robust[static_cast<size_t>(c)] =
            std::sqrt(std::max(0.0, Vr[static_cast<size_t>(c) * k + c]));
    return R;
}

}  // namespace cecon
