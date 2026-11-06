// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "estimators/nbreg.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

#include "core/linalg.h"

namespace cecon {

// Forward declarations (defined below).
double digamma(double x);
double trigamma(double x);

// We parameterize α via lnα. The parameter vector is θ = [β; lnα], length k+1.
//
//   μ_i = exp(η_i),  η_i = X_i β + off_i
//   r   = 1/α,                 p_i = r / (r + μ_i)
//   log L_i = lgamma(y_i + r) - lgamma(r) - lgamma(y_i + 1)
//             + r log(p_i) + y_i log(1 - p_i)
//
// Score / Hessian wrt β use the standard NB2 form:
//   ∂L/∂β   = Σ (y_i - μ_i) / (1 + α μ_i) * x_i
//   -∂²/∂β² = Σ μ_i (1 + α y_i) / (1 + α μ_i)^2 * x_i x_i'   (observed info)
// We use the OBSERVED information for the Hessian — the weight carries the
// realized y_i (not E[y_i]=μ_i), so this is Stata's nbreg (ml d2) Hessian, NOT
// the expected/Fisher info. That is why the default (iid) SE matches Stata's;
// see the per-term comments at the Hessian assembly below.
//
// For lnα we use ψ (digamma) and ψ_1 (trigamma) derivatives. NOTE: the observed
// information is NOT block-diagonal in (β, lnα) — the cross weight
// α μ (y-μ)/(1+α μ)^2 is nonzero and IS assembled below; it vanishes only in
// expectation (E[y-μ]=0), i.e. only for the Fisher info we do not use.

namespace {

inline double safe_log(double x) { return std::log(std::max(x, 1e-300)); }

// Neumaier-compensated sum for the log likelihood ml's step rules compare (BUG-1550;
// twin of logit.cpp's MlSum). On the 73,079-row nbreg golden (tests/validate) a plain
// sum's rounding, about 3e-5 of ll -313234.6, exceeds ll's last changes near the
// maximum, and a spurious "improvement" took a forward step StataBE's ml does not
// (final _b[treated] .159136555 against Stata's .15913642847630402).
struct LlSum {
    double s = 0.0, c = 0.0;
    void add(double x) {
        const double t = s + x;
        if (std::fabs(s) >= std::fabs(x)) c += (s - t) + x;
        else c += (x - t) + s;
        s = t;
    }
    double value() const { return s + c; }
};

}  // namespace

NbregResult nbreg(const double* X, int64_t n_obs, int k,
                  const double* y, const double* offset,
                  const double* cluster_var,
                  int maxit, double tol, const double* weights, bool freq) {
    NbregResult R;
    // BUG-1537: Stata's fweight/iweight/pweight weight every term of the
    // log-likelihood (nbreg.h). wt_of is exactly 1.0 unweighted, and every weighted
    // product below multiplies it in FIRST, so the unweighted arithmetic -- the
    // fused multiply-adds included -- is the one this function always did.
    // score_scale: a row's factor in the sandwich meat, sqrt(w) for an fweight's
    // unclustered meat (its w copies are separate terms, N = Sum w), else w.
    auto wt_of = [weights](int64_t i) { return weights ? weights[i] : 1.0; };
    auto score_scale = [weights, freq, cluster_var](int64_t i) {
        return !weights ? 1.0 : (freq && !cluster_var) ? std::sqrt(weights[i]) : weights[i];
    };
    double sum_w = 0;
    if (weights)
        for (int64_t i = 0; i < n_obs; ++i) sum_w += weights[i];
    const double n_eff = (weights && freq) ? sum_w : static_cast<double>(n_obs);
    R.k = k;
    R.n_obs = n_obs;
    R.beta.assign(static_cast<size_t>(k), 0.0);
    R.lnalpha = 0.0;  // start at α=1 (overdispersion ~ 1, between Poisson and NB)

    // The Poisson fit starts at zeros with intercept = log(ybar). The user is expected
    // to include an intercept as a column of X. We don't auto-add — caller's
    // responsibility. Find any constant column and set its β to log(ybar); other betas
    // start at 0. `cons` (the first such column) is the constant of the constant-only
    // fit (BUG-1550, below).
    double sy = 0;
    int64_t ny = 0;
    for (int64_t i = 0; i < n_obs; ++i) { sy += y[i]; ++ny; }
    double ybar = (ny > 0) ? (sy / static_cast<double>(ny)) : 1.0;
    double logyb = std::log(std::max(ybar, 1e-12));
    int cons = -1;
    for (int c = 0; c < k; ++c) {
        bool is_const = true;
        double v0 = X[static_cast<int64_t>(c) * n_obs + 0];
        for (int64_t i = 1; i < n_obs; ++i)
            if (X[static_cast<int64_t>(c) * n_obs + i] != v0) { is_const = false; break; }
        if (is_const && v0 == 1.0) {
            R.beta[static_cast<size_t>(c)] = logyb;
            if (cons < 0) cons = c;
        }
    }

    std::vector<double> mu(static_cast<size_t>(n_obs));
    std::vector<double> negH_inv(static_cast<size_t>(k + 1) * (k + 1), 0.0);

    // Poisson (α=0) fit: the source of the full fit's start (see below) AND the α→0
    // boundary fallback.
    // NR on β with the Poisson score Σ(y-μ)x and info Σμ x x' (concave ⇒ safe). For the
    // canonical log-link Poisson, observed and expected/Fisher info coincide (Σμ x x'
    // carries no y), so this cached info is an exact boundary SE either way. Caches it +
    // the Poisson ll at the fit.
    std::vector<double> pois_fish(static_cast<size_t>(k) * k, 0.0);
    double pois_ll = 0.0;
    auto poisson_fit = [&]() {
        std::vector<double> gb(k), Hb(static_cast<size_t>(k) * k),
            Hi(static_cast<size_t>(k) * k), db(k);
        for (int rep = 0; rep < 200; ++rep) {
            std::fill(gb.begin(), gb.end(), 0.0);
            std::fill(Hb.begin(), Hb.end(), 0.0);
            for (int64_t i = 0; i < n_obs; ++i) {
                double s = offset ? offset[i] : 0.0;
                for (int c = 0; c < k; ++c) s += X[static_cast<int64_t>(c) * n_obs + i] * R.beta[static_cast<size_t>(c)];
                double mi = std::exp(s), e = y[i] - mi;
                const double wt = wt_of(i);
                for (int c = 0; c < k; ++c) {
                    double xc = X[static_cast<int64_t>(c) * n_obs + i];
                    gb[static_cast<size_t>(c)] += wt * e * xc;
                    double* col = Hb.data() + static_cast<size_t>(c) * k;
                    for (int rr = 0; rr < k; ++rr) col[static_cast<size_t>(rr)] += wt * mi * xc * X[static_cast<int64_t>(rr) * n_obs + i];
                }
            }
            std::copy(Hb.begin(), Hb.end(), Hi.begin());
            if (la::spd_inverse(Hi.data(), k) != 0) break;
            la::matvec(Hi.data(), k, gb.data(), db.data());
            double mag = 0;
            for (int c = 0; c < k; ++c) { R.beta[static_cast<size_t>(c)] += db[static_cast<size_t>(c)]; mag += std::fabs(db[static_cast<size_t>(c)]); }
            if (mag < 1e-13) break;
        }
        std::fill(pois_fish.begin(), pois_fish.end(), 0.0);
        pois_ll = 0.0;
        for (int64_t i = 0; i < n_obs; ++i) {
            double s = offset ? offset[i] : 0.0;
            for (int c = 0; c < k; ++c) s += X[static_cast<int64_t>(c) * n_obs + i] * R.beta[static_cast<size_t>(c)];
            double mi = std::exp(s);
            const double wt = wt_of(i);
            pois_ll += wt * (-mi + y[i] * safe_log(mi) - std::lgamma(y[i] + 1.0));
            for (int c = 0; c < k; ++c) {
                double* col = pois_fish.data() + static_cast<size_t>(c) * k;
                for (int rr = 0; rr < k; ++rr)
                    col[static_cast<size_t>(rr)] += wt * mi * X[static_cast<int64_t>(c) * n_obs + i] * X[static_cast<int64_t>(rr) * n_obs + i];
            }
        }
    };
    poisson_fit();  // bp: β at the Poisson MLE

    // ---- the ml path (BUG-1550) -------------------------------------------------------
    // nbreg posts ml's iterate, not the exact MLE. The full fit starts at the point chosen
    // after nb_ml below, then runs Newton-Raphson on the observed
    // information (nb_ml): a full step, halved toward the old point while it does not
    // improve ll, else extended by 1/8, 1/4, ... of the step while each extension
    // improves; stop when (mreldif(theta) <= 1e-6 or reldif(ll) <= 1e-7) and
    // g inv(-H) g' < 1e-5 at the new point ([R] maximize defaults). `tol` is left over
    // from an older convergence test.
    (void)tol;
    const double LNA_FLOOR = -18.0;  // α ≈ 1.5e-8: at/below this treat as the α→0 boundary
    // α ≈ 2.5e-3. Above this, a non-positive (lnα,lnα) information means something
    // other than the boundary and must stay a hard failure; below it, α is already
    // negligible and the Poisson limit is the answer either way.
    const double LNA_NEAR_ZERO = -6.0;
    bool boundary = false;

    // NB2 ll over the columns `cols` of X, θ = (β_cols, lnα); with g / A non-null also
    // its score and negative Hessian (observed information, (kd+1) x (kd+1)). α is
    // clamped away from 0 so deep-below-floor trials stay finite (the boundary check
    // discards them).
    auto nb_eval = [&](const std::vector<int>& cols, const std::vector<double>& th,
                       std::vector<double>* g, std::vector<double>* A) {
        const int kd = static_cast<int>(cols.size()), nd = kd + 1;
        const double a = std::max(std::exp(th[static_cast<size_t>(kd)]), 1e-12), r = 1.0 / a;
        if (g) g->assign(static_cast<size_t>(nd), 0.0);
        if (A) A->assign(static_cast<size_t>(nd) * nd, 0.0);
        LlSum ll;
        for (int64_t i = 0; i < n_obs; ++i) {
            double s = offset ? offset[i] : 0.0;
            for (int c = 0; c < kd; ++c)
                s += X[static_cast<int64_t>(cols[static_cast<size_t>(c)]) * n_obs + i] * th[static_cast<size_t>(c)];
            const double yi = y[i], mi = std::exp(s);
            const double w = 1.0 + a * mi;          // 1 + α μ
            const double rpmu = r + mi;             // r + μ = 1/α + μ
            const double wt = wt_of(i);             // the row's weight (1 unweighted)
            ll.add(wt * (std::lgamma(yi + r) - std::lgamma(r) - std::lgamma(yi + 1.0)
                + r * (std::log(r) - safe_log(rpmu))
                + yi * (safe_log(mi) - safe_log(rpmu))));
            if (!g) continue;
            // Score: β (y - μ)/(1 + α μ) x; lnα -r ∂L/∂r, with
            //   ∂L_i/∂r = ψ(y_i+r) - ψ(r) + log(r/(r+μ_i)) + (μ_i - y_i)/(r + μ_i).
            // Negative Hessian: ββ weight μ(1 + α y)/(1 + α μ)² (the observed information,
            // as Stata's d2), β-lnα α μ (y - μ)/(1 + α μ)², lnα-lnα -(r ∂L/∂r + r² ∂²L/∂r²),
            //   ∂²L_i/∂r² = ψ_1(y_i+r) - ψ_1(r) + 1/r - 2/(r+μ_i) + (y_i+r)/(r+μ_i)².
            const double resb = (yi - mi) / w;
            const double drdL = (digamma(yi + r) - digamma(r))
                              + std::log(r) - safe_log(rpmu)
                              + (mi - yi) / rpmu;
            const double d2drr = trigamma(yi + r) - trigamma(r)
                               + 1.0 / r - 2.0 / rpmu + (yi + r) / (rpmu * rpmu);
            const double wi = wt * mi * (1.0 + a * yi) / (w * w);
            const double cross_w = wt * a * mi * (yi - mi) / (w * w);
            for (int c = 0; c < kd; ++c) {
                const double xc = X[static_cast<int64_t>(cols[static_cast<size_t>(c)]) * n_obs + i];
                (*g)[static_cast<size_t>(c)] += wt * resb * xc;
                double* col = A->data() + static_cast<size_t>(c) * nd;
                for (int rr = 0; rr < kd; ++rr)
                    col[static_cast<size_t>(rr)] += wi * xc * X[static_cast<int64_t>(cols[static_cast<size_t>(rr)]) * n_obs + i];
                (*A)[static_cast<size_t>(kd) * nd + c] += cross_w * xc;
                (*A)[static_cast<size_t>(c) * nd + kd] += cross_w * xc;
            }
            (*g)[static_cast<size_t>(kd)] += wt * -r * drdL;
            (*A)[static_cast<size_t>(kd) * nd + kd] += wt * -(r * drdL + r * r * d2drr);
        }
        return ll.value();
    };

    // Step 4 from θ over `cols`. On return θ and ll are the final point's and, unless
    // *bnd, Ainv is the inverse negative Hessian there; *steps counts the iterations.
    // false: the negative Hessian did not invert (a failure). *bnd: lnα crossed into
    // the α→0 boundary (handled after the full fit).
    auto nb_ml = [&](const std::vector<int>& cols, std::vector<double>& th, double& ll,
                     std::vector<double>& Ainv, int& steps, bool& bnd, bool& cvg) {
        const size_t nd = cols.size() + 1;
        std::vector<double> g, A, d(nd), cand(nd), tryth(nd), dd(nd), prev(nd);
        double prev_ll = 0.0;
        steps = 0;
        bnd = cvg = false;
        for (;;) {
            ll = nb_eval(cols, th, &g, &A);
            // α→0 boundary, SECOND detector (the LNA_FLOOR check below is the first).
            // As α→0 the (lnα,lnα) observed information collapses toward 0, and with
            // r = 1/α ~ 1e7 the digamma/trigamma difference that forms it loses every
            // significant digit to cancellation — it lands slightly NEGATIVE while lnα
            // is still above LNA_FLOOR, so -H is not positive definite. Measured on the
            // underdispersed `round(spirits*3)` counts from the fatalities panel: lnα =
            // -17.45 (floor -18), info -4.6e-06 against β-block entries of ~1e3. Whether
            // the cancellation lands just above or below zero is decided by the last
            // bits of digamma/trigamma, so this guards every platform. Guarded on lnα
            // being deep in boundary territory so a genuinely singular design (collinear
            // X) still reports as the failure it is.
            if (A[(nd - 1) * nd + (nd - 1)] <= 0.0 && th[nd - 1] < LNA_NEAR_ZERO) { bnd = true; return true; }
            Ainv = A;
            if (la::spd_inverse(Ainv.data(), static_cast<int>(nd)) != 0) return false;
            la::matvec(Ainv.data(), static_cast<int>(nd), g.data(), d.data());
            if (steps > 0) {
                double nr = 0.0, pc = 0.0;
                for (size_t j = 0; j < nd; ++j) {
                    nr += g[j] * d[j];
                    pc = std::max(pc, std::fabs(th[j] - prev[j]) / (std::fabs(prev[j]) + 1.0));
                }
                const double rl = std::fabs(ll - prev_ll) / (std::fabs(prev_ll) + 1.0);
                if ((pc <= 1e-6 || rl <= 1e-7) && nr < 1e-5) { cvg = true; return true; }
            }
            if (steps >= maxit) return true;
            for (size_t j = 0; j < nd; ++j) cand[j] = th[j] + d[j];
            double f1 = nb_eval(cols, cand, nullptr, nullptr);
            if (!(f1 >= ll)) {
                for (int nh = 0; nh < 60 && !(f1 >= ll); ++nh) {
                    for (size_t j = 0; j < nd; ++j) cand[j] = (cand[j] + th[j]) / 2.0;
                    f1 = nb_eval(cols, cand, nullptr, nullptr);
                }
            } else {
                for (size_t j = 0; j < nd; ++j) dd[j] = 0.125 * d[j];
                for (int nf = 0; nf < 100; ++nf) {
                    for (size_t j = 0; j < nd; ++j) tryth[j] = cand[j] + dd[j];
                    const double fc = nb_eval(cols, tryth, nullptr, nullptr);
                    if (!(fc > f1)) break;
                    cand = tryth;
                    f1 = fc;
                    for (double& v : dd) v *= 2.0;
                }
            }
            prev = th;
            prev_ll = ll;
            th = cand;
            ++steps;
            // α→0 boundary (equidispersed data): stop and hand off to the Poisson limit below.
            if (th[nd - 1] < LNA_FLOOR) { bnd = true; return true; }
        }
    };

    std::vector<int> all_cols(static_cast<size_t>(k));
    for (int c = 0; c < k; ++c) all_cols[static_cast<size_t>(c)] = c;
    std::vector<double> th;                              // the full fit's start (beta, lnalpha)
    std::vector<double> Ainv;
    double ll = 0.0;
    int steps = 0;
    bool conv = false;
    // Choosing the full fit's start. [R] nbreg shows the phases: Poisson, constant-only,
    // full. Stata reports its iterate, so the start shows in the trailing digits of b,
    // lnalpha, V and e(ic). The rule was measured on StataBE.
    const auto poisson_with_unit_alpha = [&] {
        std::vector<double> point(R.beta);
        point.push_back(0.0);
        return point;
    };
    if (cons < 0) {
        th = poisson_with_unit_alpha();
    } else {
        // Totals for the constant-only start: mean count per unit of exposure.
        double total_count = 0.0, total_exposure = 0.0, total_weight = 0.0, total_xb = 0.0;
        bool has_fweight_above_one = false;
        for (int64_t i = 0; i < n_obs; ++i) {
            const double w = wt_of(i);
            total_count += w * y[i];
            total_exposure += w * (offset ? std::exp(offset[i]) : 1.0);
            total_weight += w;
            double xb = 0.0;
            for (int c = 0; c < k; ++c)
                xb += X[static_cast<int64_t>(c) * n_obs + i] * R.beta[static_cast<size_t>(c)];
            total_xb += w * xb;
            if (freq && w > 1.0) has_fweight_above_one = true;
        }
        const double log_rate = std::log(total_count / total_exposure);
        std::vector<double> null_model = {std::isfinite(log_rate) ? log_rate : 0.0, 0.0};
        const std::vector<int> constant_only = {cons};
        const bool null_inverted =
            nb_ml(constant_only, null_model, ll, Ainv, steps, boundary, conv);
        if (!null_inverted) {
            // Not measured and not reached by any test.
            th = poisson_with_unit_alpha();
        } else if (!boundary) {
            // `ll` is now the constant-only maximum.
            const double null_cons = null_model[0], null_lnalpha = null_model[1];
            th.assign(static_cast<size_t>(k) + 1, 0.0);
            th[static_cast<size_t>(cons)] = null_cons;
            th[static_cast<size_t>(k)] = null_lnalpha;
            // Any fweight above 1 keeps the constant-only point: measured on four data
            // sets, and unexplained (expanding the same data instead gives the shifted
            // point). Otherwise take the Poisson slopes with the constant moved so the
            // weighted mean linear predictor is null_cons, when that beats the
            // constant-only likelihood.
            if (!has_fweight_above_one) {
                std::vector<double> shifted = poisson_with_unit_alpha();
                shifted[static_cast<size_t>(cons)] += null_cons - total_xb / total_weight;
                shifted[static_cast<size_t>(k)] = null_lnalpha;
                if (nb_eval(all_cols, shifted, nullptr, nullptr) > ll) th = std::move(shifted);
            }
        }
        // boundary: `th` stays empty, the Poisson-limit report below takes over.
    }
    if (!boundary) {
        const bool ok = nb_ml(all_cols, th, ll, Ainv, steps, boundary, conv);
        R.beta.assign(th.begin(), th.begin() + k);
        R.lnalpha = th[static_cast<size_t>(k)];
        R.loglik = ll;
        if (!ok) { R.converged = false; R.iters = steps; return R; }
    }
    R.iters = steps;
    if (!boundary) negH_inv = Ainv;
    R.converged = conv;
    if (boundary) {
        // α→0 boundary: NB2 collapses to Poisson (equidispersed data). Report the exact
        // Poisson MLE — coefs, ll, Fisher-based SEs — matching `poisson`. α/lnα are
        // boundary noise (Stata prints α≈0 with a MISSING lnα SE), so report α=0 and a
        // zero (missing) lnα SE. Re-solve Poisson to pin β exactly, then load the Poisson
        // Fisher into the β-block of negH_inv so the SE code below is Poisson-exact and
        // the robust/cluster sandwich (which reads α=0 ⇒ e_i=y_i-μ_i) is the Poisson one.
        poisson_fit();
        R.alpha = 0.0; R.lnalpha = LNA_FLOOR; R.loglik = pois_ll; R.converged = true;
        for (int64_t i = 0; i < n_obs; ++i) {
            double s = offset ? offset[i] : 0.0;
            for (int c = 0; c < k; ++c) s += X[static_cast<int64_t>(c) * n_obs + i] * R.beta[static_cast<size_t>(c)];
            mu[static_cast<size_t>(i)] = std::exp(s);
        }
        std::vector<double> Fi(pois_fish);
        la::spd_inverse(Fi.data(), k);
        std::fill(negH_inv.begin(), negH_inv.end(), 0.0);
        for (int c = 0; c < k; ++c)
            for (int rr = 0; rr < k; ++rr)
                negH_inv[static_cast<size_t>(c) * (k + 1) + rr] = Fi[static_cast<size_t>(c) * k + rr];
    } else {
        R.alpha = std::exp(R.lnalpha);
    }

    // Final IID SE = sqrt(diag(I^-1))
    R.se_iid.assign(static_cast<size_t>(k), 0.0);
    for (int c = 0; c < k; ++c)
        R.se_iid[static_cast<size_t>(c)] =
            std::sqrt(std::max(0.0, negH_inv[static_cast<size_t>(c) * (k + 1) + c]));
    R.se_lnalpha_iid =
        std::sqrt(std::max(0.0, negH_inv[static_cast<size_t>(k) * (k + 1) + k]));
    if (!boundary) R.vcov_iid = negH_inv;

    // Robust / cluster-robust VCE: Stata's ml sandwich is over ALL of θ = (β, lnα)
    // -- bread the full inverse observed information, meat the outer products of
    // the per-observation score rows (β scores and the lnα score of the Newton
    // loop above) or of their cluster sums -- times N/(N-1), or M/(M-1) for M
    // clusters, and nothing else (StataBE 19.5 `nbreg cnt x1 x2, vce(robust)`
    // _se[x1] .040250904916642731, `vce(cluster cl)` .018789535432332246, with a
    // full /lnalpha row in e(V); the β-block-only sandwich below gave .0400118 and,
    // with its extra (N-1)/(N-k-1), .0196015: probe BUG-1551). At
    // the α→0 boundary lnα has no score (r = 1/α is infinite) and the β-only
    // Poisson sandwich below is kept.
    if (!boundary) {
        const int np = k + 1;
        const double a = R.alpha, r = 1.0 / a;
        std::vector<double> s(static_cast<size_t>(np));
        std::vector<int> cl_of_row;
        std::unordered_map<double, int> ci_of;
        if (cluster_var) {
            cl_of_row.resize(static_cast<size_t>(n_obs));
            for (int64_t i = 0; i < n_obs; ++i) {
                const auto it = ci_of.emplace(cluster_var[i], static_cast<int>(ci_of.size())).first;
                cl_of_row[static_cast<size_t>(i)] = it->second;
            }
        }
        const int64_t rows = cluster_var ? static_cast<int64_t>(ci_of.size()) : n_obs;
        std::vector<double> Srow(static_cast<size_t>(rows) * np, 0.0);  // col-major rows x np
        for (int64_t i = 0; i < n_obs; ++i) {
            double eta_i = offset ? offset[i] : 0.0;
            for (int c = 0; c < k; ++c) eta_i += X[static_cast<int64_t>(c) * n_obs + i] * R.beta[static_cast<size_t>(c)];
            const double mi = std::exp(eta_i), yi = y[i], rpmu = r + mi;
            const double resb = (yi - mi) / (1.0 + a * mi);
            const double sc = score_scale(i);
            for (int c = 0; c < k; ++c) s[static_cast<size_t>(c)] = sc * resb * X[static_cast<int64_t>(c) * n_obs + i];
            s[static_cast<size_t>(k)] =
                sc * -r * ((digamma(yi + r) - digamma(r)) + std::log(r) - safe_log(rpmu) + (mi - yi) / rpmu);
            const int64_t row = cluster_var ? cl_of_row[static_cast<size_t>(i)] : i;
            for (int p = 0; p < np; ++p) Srow[static_cast<size_t>(p) * rows + row] += s[static_cast<size_t>(p)];
        }
        std::vector<double> meat(static_cast<size_t>(np) * np, 0.0), tmp(static_cast<size_t>(np) * np, 0.0);
        la::xtx(Srow.data(), rows, np, meat.data());
        // N/(N-1) on the rows, or under an fweight's unclustered meat on N = Sum w
        // (StataBE 19.5, scratch-laneA2/agents/wml/x1.log: `nbreg cnt x z [fw=wi],
        // vce(robust)` _se[x] .21808641854612026, vce(cluster cl) .24761054966651;
        // [pw=wf] .4424215353885973, vce(cluster cl) .31314117777098438).
        const double nq = (weights && freq && !cluster_var) ? sum_w : static_cast<double>(rows);
        const double q = nq > 1 ? nq / (nq - 1.0) : 1.0;
        R.vcov_robust.assign(static_cast<size_t>(np) * np, 0.0);
        la::sandwich(negH_inv.data(), meat.data(), np, q, R.vcov_robust.data(), tmp.data());
        R.se_robust.assign(static_cast<size_t>(k), 0.0);
        for (int c = 0; c < k; ++c)
            R.se_robust[static_cast<size_t>(c)] =
                std::sqrt(std::max(0.0, R.vcov_robust[static_cast<size_t>(c) * np + c]));
        return R;
    }

    // Cluster-robust SE: sandwich with per-cluster score sums (β block only).
    // Reached only at the α→0 boundary since BUG-1551 (above).
    if (cluster_var) {
        std::unordered_map<double, int> ci_of;
        std::vector<std::vector<double>> per_cl;
        per_cl.reserve(8192);
        // per-obs score wrt β at final β, lnα.
        double a = R.alpha;
        std::vector<double> s_i(static_cast<size_t>(n_obs) * k, 0.0);
        for (int64_t i = 0; i < n_obs; ++i) {
            double mi = mu[static_cast<size_t>(i)];
            double resb = wt_of(i) * (y[i] - mi) / (1.0 + a * mi);
            for (int c = 0; c < k; ++c)
                s_i[static_cast<size_t>(c) * n_obs + i] =
                    resb * X[static_cast<int64_t>(c) * n_obs + i];
        }
        std::vector<int> cl_of_row(static_cast<size_t>(n_obs));
        for (int64_t i = 0; i < n_obs; ++i) {
            double v = cluster_var[i];
            auto it = ci_of.find(v);
            int ci;
            if (it == ci_of.end()) {
                ci = static_cast<int>(per_cl.size());
                ci_of[v] = ci;
                per_cl.emplace_back(static_cast<size_t>(k), 0.0);
            } else ci = it->second;
            cl_of_row[static_cast<size_t>(i)] = ci;
        }
        for (int64_t i = 0; i < n_obs; ++i) {
            int ci = cl_of_row[static_cast<size_t>(i)];
            for (int c = 0; c < k; ++c)
                per_cl[static_cast<size_t>(ci)][static_cast<size_t>(c)] +=
                    s_i[static_cast<size_t>(c) * n_obs + i];
        }
        std::vector<double> G(static_cast<size_t>(k) * k, 0.0);
        for (const auto& sc : per_cl)
            for (int c = 0; c < k; ++c)
                for (int rr = 0; rr < k; ++rr)
                    G[static_cast<size_t>(c) * k + rr] += sc[static_cast<size_t>(c)] * sc[static_cast<size_t>(rr)];
        // V_rob = (negH^-1_ββ) * G * (negH^-1_ββ).
        // Extract the β-β block of negH_inv into a k*k matrix.
        std::vector<double> H_bb(static_cast<size_t>(k) * k, 0.0);
        for (int c = 0; c < k; ++c)
            for (int rr = 0; rr < k; ++rr)
                H_bb[static_cast<size_t>(c) * k + rr] =
                    negH_inv[static_cast<size_t>(c) * (k + 1) + rr];
        std::vector<double> tmp(static_cast<size_t>(k) * k, 0.0);
        for (int c = 0; c < k; ++c)
            for (int rr = 0; rr < k; ++rr) {
                double s = 0;
                for (int j = 0; j < k; ++j)
                    s += H_bb[static_cast<size_t>(j) * k + rr] * G[static_cast<size_t>(c) * k + j];
                tmp[static_cast<size_t>(c) * k + rr] = s;
            }
        // Stata's nbreg cluster: q = M/(M-1) * (N-1)/(N-k_full) where k_full=k+1 (incl. lnα).
        int M = static_cast<int>(per_cl.size());
        double q = (static_cast<double>(M) / (M - 1.0))
                 * ((n_eff - 1.0) / (n_eff - static_cast<double>(k + 1)));
        std::vector<double> Vr(static_cast<size_t>(k) * k, 0.0);
        for (int c = 0; c < k; ++c)
            for (int rr = 0; rr < k; ++rr) {
                double s = 0;
                for (int j = 0; j < k; ++j)
                    s += tmp[static_cast<size_t>(j) * k + rr] * H_bb[static_cast<size_t>(c) * k + j];
                Vr[static_cast<size_t>(c) * k + rr] = q * s;
            }
        R.se_robust.assign(static_cast<size_t>(k), 0.0);
        for (int c = 0; c < k; ++c)
            R.se_robust[static_cast<size_t>(c)] =
                std::sqrt(std::max(0.0, Vr[static_cast<size_t>(c) * k + c]));
    } else {
        // HC1 sandwich (no cluster): G = Σ_i s_i s_i'.
        double a = R.alpha;
        std::vector<double> G(static_cast<size_t>(k) * k, 0.0);
        for (int64_t i = 0; i < n_obs; ++i) {
            double mi = mu[static_cast<size_t>(i)];
            double resb = score_scale(i) * (y[i] - mi) / (1.0 + a * mi);
            for (int c = 0; c < k; ++c) {
                double sc = resb * X[static_cast<int64_t>(c) * n_obs + i];
                for (int rr = 0; rr < k; ++rr) {
                    double sr = resb * X[static_cast<int64_t>(rr) * n_obs + i];
                    G[static_cast<size_t>(c) * k + rr] += sc * sr;
                }
            }
        }
        std::vector<double> H_bb(static_cast<size_t>(k) * k, 0.0);
        for (int c = 0; c < k; ++c)
            for (int rr = 0; rr < k; ++rr)
                H_bb[static_cast<size_t>(c) * k + rr] =
                    negH_inv[static_cast<size_t>(c) * (k + 1) + rr];
        std::vector<double> tmp(static_cast<size_t>(k) * k, 0.0);
        for (int c = 0; c < k; ++c)
            for (int rr = 0; rr < k; ++rr) {
                double s = 0;
                for (int j = 0; j < k; ++j)
                    s += H_bb[static_cast<size_t>(j) * k + rr] * G[static_cast<size_t>(c) * k + j];
                tmp[static_cast<size_t>(c) * k + rr] = s;
            }
        std::vector<double> Vr(static_cast<size_t>(k) * k, 0.0);
        // BUG-164 (W:P5): NO finite-sample correction here. Stata applies the
        // regress-style N/(N-k) inflation to OLS robust VCE, but its
        // maximum-likelihood robust VCE — nbreg's included — uses the bare
        // sandwich. This code applied N/(N-k-1) anyway, so every reported
        // robust SE was too large by sqrt(N/(N-k-1)).
        //
        // Measured against the W:P5 answerbook (Stata 19.5,
        // `nbreg ycount x1 x2 i.region if t>=2, vce(robust)`, N=4,476, k+1=7):
        //   shipped  q = N/(N-k-1) -> se(x1) .0044682, ratio 1.000784 to Stata
        //            (= sqrt(4476/4469), i.e. exactly the wrong factor)
        //   q = 1    -> se(x1) .0044642, ratio 0.9998964 — now slightly SMALL
        //   q = N/(N-1) -> matches Stata to seven digits on all seven
        //            coefficients (0.9998964 x sqrt(4476/4475) = 1.0000081)
        // So Stata's ML robust VCE carries N/(N-1), not the regress-style
        // N/(N-k) this code borrowed.
        //
        // The CLUSTER branch above keeps its own q: Stata's ML cluster VCE
        // does carry M/(M-1), and that factor was verified against its own
        // oracle. Only the no-cluster path was wrong.
        const double q = n_eff / (n_eff - 1.0);
        for (int c = 0; c < k; ++c)
            for (int rr = 0; rr < k; ++rr) {
                double s = 0;
                for (int j = 0; j < k; ++j)
                    s += tmp[static_cast<size_t>(j) * k + rr] * H_bb[static_cast<size_t>(c) * k + j];
                Vr[static_cast<size_t>(c) * k + rr] = q * s;
            }
        R.se_robust.assign(static_cast<size_t>(k), 0.0);
        for (int c = 0; c < k; ++c)
            R.se_robust[static_cast<size_t>(c)] =
                std::sqrt(std::max(0.0, Vr[static_cast<size_t>(c) * k + c]));
    }
    return R;
}

// Local helpers: digamma and trigamma. C++17 std::tgamma is available; lgamma_r
// not portable. ψ and ψ_1 shift x up to >= 10 by recurrence, then sum the
// asymptotic series through its x^-10 (ψ) / x^-11 (ψ_1) term: truncation error
// below 3e-14 there. The shorter series (to x^-6 from x >= 6) this replaced erred
// by 2.5e-9 at x = 6, which moved the constant-only fit's early iterates of
// nbreg's ml path (BUG-1550) by 1e-10 relative.
double digamma(double x) {
    double sum = 0;
    while (x < 10.0) { sum -= 1.0 / x; x += 1.0; }
    // ψ(x) = log(x) - 1/(2x) - Σ B_2k / (2k x^{2k})
    const double inv = 1.0 / x;
    const double inv2 = inv * inv;
    return sum + std::log(x) - 0.5 * inv
         - inv2 * (1.0/12.0 - inv2*(1.0/120.0 - inv2*(1.0/252.0 - inv2*(1.0/240.0 - inv2*(1.0/132.0)))));
}

double trigamma(double x) {
    double sum = 0;
    while (x < 10.0) { sum += 1.0 / (x * x); x += 1.0; }
    // ψ_1(x) = 1/x + 1/(2x²) + Σ B_2k / x^{2k+1}
    const double inv = 1.0 / x;
    const double inv2 = inv * inv;
    return sum + inv + 0.5 * inv2
         + inv2 * inv * (1.0/6.0 - inv2*(1.0/30.0 - inv2*(1.0/42.0 - inv2*(1.0/30.0 - inv2*(5.0/66.0)))));
}

}  // namespace cecon
