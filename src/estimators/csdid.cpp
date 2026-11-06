// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Callaway–Sant'Anna staggered difference-in-differences —
// implemented from the published methodology papers:
//
//   [CS21] Callaway, B. and P.H.C. Sant'Anna (2021), "Difference-in-Differences
//          with multiple time periods", Journal of Econometrics 225(2), 200-230.
//   [SZ20] Sant'Anna, P.H.C. and J. Zhao (2020), "Doubly robust difference-in-
//          differences estimators", Journal of Econometrics 219(1), 101-122.
//
// drdid_dripw_cell implements SZ20's doubly-robust DiD estimator for panel data
// (their τ^{dr,p}, §3.1): the propensity score is fitted by maximum-likelihood
// logit, the outcome-evolution model by least squares on the comparison group,
// and the two are combined through the normalized weights
//     w1(D) = D / E[D],   w0(D,X) = [π(X)(1-D)/(1-π(X))] / E[π(1-D)/(1-π)].
// Standard errors come from the estimator's asymptotic linear representation
// (SZ20 §3.2), including the first-step estimation effects of both the
// outcome-regression coefficients and the propensity-score coefficients,
// derived here by the delta method (see drdid_dripw_cell).
//
// csdid_dripw_run implements CS21's family of group-time average treatment
// effects ATT(g,t) (§2-3): one 2x2 doubly-robust comparison per (cohort g,
// outcome period t) cell against the never-treated (default) or not-yet-treated
// comparison group, with the varying base period (the period preceding t for
// pre-treatment cells, the period preceding g for post-treatment cells), and
// the paper's aggregation schemes (§3.1): group-size-weighted "simple" overall
// ATT, event-study (dynamic) bins by e = t - g, and calendar-period effects.
// Aggregation inference follows CS21 §4: linear combinations of the per-cell
// influence functions plus the estimation effect of the group-share weights
// P(G=g), cluster-folded when a cluster variable is supplied.
//
// Sample/output conventions not fixed by the papers (cell enumeration order,
// e(N) row accounting, covariate-missingness endpoint rule, exclusion of
// exact-zero cells from aggregations, the long2 filled grid) are pinned by this
// project's committed behavioral goldens and unit tests:
//   tests/eval/golden/csdid*.csv, tests/unit/test_csdid*.cpp.

#include "estimators/csdid.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <unordered_map>
#include <vector>

#include "core/linalg.h"
#include "estimators/posthest.h"

namespace cecon {
namespace {

// ---------------------------------------------------------------------------
// Small dense helpers (column-major storage, k is tiny).

// x := A⁻¹ b for symmetric positive-definite A (k×k, full storage; taken by
// value so the caller's copy is preserved). Returns false if A is not PD.
bool spd_solve(std::vector<double> A, const double* b, int k, std::vector<double>& x) {
    if (la::spd_inverse(A.data(), k) != 0) return false;
    x.assign(static_cast<size_t>(k), 0.0);
    la::matvec(A.data(), k, b, x.data());
    return true;
}

// Numerically stable logistic CDF.
double logistic(double e) {
    if (e >= 0) return 1.0 / (1.0 + std::exp(-e));
    const double z = std::exp(e);
    return z / (1.0 + z);
}

// Maximum-likelihood logit of d on X (n×k col-major, constant column already
// included): Newton–Raphson from β = 0 on the globally concave log-likelihood.
// Fills beta (k) and ps (n, fitted probabilities). False on failure.
//
// Quasi-/complete separation. When the treatment is perfectly predicted by X
// (routine for a cohort of one treated unit — the tiny-cohort case in CS21
// panels) the logit MLE does not exist: β diverges to ±∞ along a separating
// direction, so the Newton step never contracts and the information matrix H
// collapses to singular as the fitted probabilities reach the 0/1 boundary.
// The score (gradient) does, however, tend to zero — the objective is being
// maximised, just at a point at infinity. Rather than discard the whole (g,t)
// cell, we return the highest-likelihood iterate once the score has effectively
// vanished: a finite point on the separation plateau. Along a fixed separating
// direction the odds weights ŵ0 = π̂/(1−π̂) are scale-invariant after
// normalisation, so the DR estimand is insensitive to how far out on the
// plateau this iterate sits. This mirrors the observed behaviour of Stata's ml
// maximiser, which declares convergence at that plateau and returns finite
// coefficients (the exact separating direction, hence the cell value, is
// maximiser-specific and not pinned by the papers, which assume an interior
// MLE). Non-separated cells are unaffected: they converge earlier by the step
// test and never reach this path.
bool logit_mle(const double* X, const double* d, int64_t n, int k,
               std::vector<double>& beta, std::vector<double>& ps) {
    beta.assign(static_cast<size_t>(k), 0.0);
    ps.assign(static_cast<size_t>(n), 0.5);
    std::vector<double> grad(static_cast<size_t>(k));
    std::vector<double> H(static_cast<size_t>(k) * k);
    std::vector<double> step;
    std::vector<double> best_beta, best_ps;  // highest-likelihood iterate seen
    double best_ll = -std::numeric_limits<double>::infinity();
    double best_gnorm = std::numeric_limits<double>::infinity();
    double g0 = 0.0;  // ‖score‖∞ at β = 0, for a scale-free convergence gate
    for (int it = 0; it < 200; ++it) {
        std::fill(grad.begin(), grad.end(), 0.0);
        std::fill(H.begin(), H.end(), 0.0);
        double ll = 0.0;
        for (int64_t i = 0; i < n; ++i) {
            double eta = 0;
            for (int j = 0; j < k; ++j) eta += X[static_cast<int64_t>(j) * n + i] * beta[j];
            const double pi = logistic(eta);
            ps[i] = pi;
            // Log-likelihood, floored at the perfectly-predicting 0/1 boundary.
            if (d[i] != 0.0) ll += (pi > 0.0) ? std::log(pi) : -745.0;
            else             ll += (pi < 1.0) ? std::log(1.0 - pi) : -745.0;
            const double r = d[i] - pi;        // score residual
            const double w = pi * (1.0 - pi);  // information weight
            for (int a = 0; a < k; ++a) {
                const double xa = X[static_cast<int64_t>(a) * n + i];
                grad[a] += xa * r;
                for (int b = 0; b <= a; ++b)
                    H[a * k + b] += w * xa * X[static_cast<int64_t>(b) * n + i];
            }
        }
        for (int a = 0; a < k; ++a)
            for (int b = a + 1; b < k; ++b) H[a * k + b] = H[b * k + a];
        double gnorm = 0.0;
        for (int j = 0; j < k; ++j) gnorm = std::max(gnorm, std::fabs(grad[j]));
        if (it == 0) g0 = gnorm;
        if (ll > best_ll) { best_ll = ll; best_gnorm = gnorm; best_beta = beta; best_ps = ps; }
        // A vanished score identifies a (quasi-)separated maximiser at infinity.
        const bool separated = best_gnorm <= 1e-9 * (g0 + 1.0);
        if (!spd_solve(H, grad.data(), k, step)) {
            // H went singular. If we are on the separation plateau, hand back
            // the finite best iterate; otherwise it is a genuine breakdown.
            if (separated) { beta = best_beta; ps = best_ps; return true; }
            return false;
        }
        double mx = 0, mb = 0;
        for (int j = 0; j < k; ++j) {
            beta[j] += step[j];
            mx = std::max(mx, std::fabs(step[j]));
            mb = std::max(mb, std::fabs(beta[j]));
        }
        if (mx <= 1e-11 * (1.0 + mb)) return true;
    }
    // Iteration budget exhausted: accept a (quasi-)separated maximiser, else fail.
    if (best_gnorm <= 1e-9 * (g0 + 1.0)) { beta = best_beta; ps = best_ps; return true; }
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// One 2x2 doubly-robust cell — SZ20 §3.1 (panel), "DR-IPW" flavor: ML-logit
// propensity + unweighted least squares outcome regression on the comparison
// group, combined with normalized odds weights.
//
// Estimator:  τ = Ê[ŵ1 (ΔY − X'β̂)] − Ê[ŵ0 (ΔY − X'β̂)]
//   with ŵ1 = D/Ê[D], ŵ0 = w0/Ê[w0], w0 = π̂(X)(1−D)/(1−π̂(X)).
//
// Influence function (SZ20 §3.2; delta method over (β̂, γ̂, the two weight
// normalizations)). Writing ê = ΔY − X'β̂, m1 = Ê[D], m0 = Ê[w0],
// τ1 = Ê[D ê]/m1, τ0 = Ê[w0 ê]/m0:
//   ψ_i = D_i(ê_i − τ1)/m1 − w0_i(ê_i − τ0)/m0
//         − (M1 − M0)' Â⁻¹ X_i (1−D_i) ê_i        (outcome-regression effect)
//         − M2' Ĥ⁻¹ X_i (D_i − π̂_i)               (propensity-score effect)
// where M1 = Ê[D X]/m1, M0 = Ê[w0 X]/m0, M2 = Ê[w0 X (ê − τ0)]/m0,
// Â = Ê[(1−D) X X'] (OLS bread), Ĥ = Ê[π̂(1−π̂) X X'] (logit information).
// The M2 form uses dw0/dη = w0 for the logistic odds w0 = (1−D)π/(1−π).
// Each ψ term is exactly mean-zero by the first-order conditions, so the
// recentered per-unit influence function inf_i = τ + ψ_i has mean τ.
// SE = sqrt(Σψ²)/n (unit-level IID).
DrdidCell drdid_dripw_cell(const double* dy, const double* X, int64_t n, int p,
                           const double* trt, double pscoretrim) {
    DrdidCell out;
    out.n = n;
    const int k = p + 1;
    if (n < k + 1) { out.ok = false; return out; }

    // Design [X, 1]: constant appended as the last column.
    std::vector<double> Xc(static_cast<size_t>(n) * k);
    if (p > 0) std::memcpy(Xc.data(), X, sizeof(double) * static_cast<size_t>(n) * p);
    std::fill(Xc.begin() + static_cast<size_t>(n) * p, Xc.end(), 1.0);

    // Treatment model: π̂(X) by ML logit [SZ20 §3.1].
    std::vector<double> gamma, ps;
    if (!logit_mle(Xc.data(), trt, n, k, gamma, ps)) { out.ok = false; return out; }

    // Outcome model: μ̂_{0,Δ}(X) = X'β̂ by least squares on the comparison group.
    std::vector<double> A(static_cast<size_t>(k) * k, 0.0), xy(static_cast<size_t>(k), 0.0);
    std::vector<double> beta;
    int64_t n1 = 0, n0 = 0;
    for (int64_t i = 0; i < n; ++i) {
        if (trt[i] != 0.0) { ++n1; continue; }
        ++n0;
        for (int a = 0; a < k; ++a) {
            const double xa = Xc[static_cast<int64_t>(a) * n + i];
            xy[a] += xa * dy[i];
            for (int b = 0; b <= a; ++b)
                A[a * k + b] += xa * Xc[static_cast<int64_t>(b) * n + i];
        }
    }
    for (int a = 0; a < k; ++a)
        for (int b = a + 1; b < k; ++b) A[a * k + b] = A[b * k + a];
    if (n1 < 1 || n0 < 1 || !spd_solve(A, xy.data(), k, beta)) { out.ok = false; return out; }

    // Residualized long differences and the SZ20 weights; comparison units with
    // π̂ above the trimming threshold get zero weight.
    std::vector<double> ehat(static_cast<size_t>(n)), w0v(static_cast<size_t>(n));
    double sw1 = 0, sw0 = 0, sw1e = 0, sw0e = 0;
    for (int64_t i = 0; i < n; ++i) {
        double xb = 0;
        for (int j = 0; j < k; ++j) xb += Xc[static_cast<int64_t>(j) * n + i] * beta[j];
        ehat[i] = dy[i] - xb;
        double w0 = 0;
        if (trt[i] == 0.0 && ps[i] <= pscoretrim) w0 = ps[i] / (1.0 - ps[i]);
        w0v[i] = w0;
        sw1 += trt[i];
        sw0 += w0;
        sw1e += trt[i] * ehat[i];
        sw0e += w0 * ehat[i];
    }
    if (!(sw1 > 0) || !(sw0 > 0)) { out.ok = false; return out; }
    const double att_t = sw1e / sw1;  // treated component  τ1
    const double att_c = sw0e / sw0;  // comparison component τ0
    out.att = att_t - att_c;

    // First-step estimation effects: gradient vectors M1, M0, M2 and the logit
    // information at the optimum.
    std::vector<double> M1(static_cast<size_t>(k), 0.0), M0(static_cast<size_t>(k), 0.0);
    std::vector<double> M2(static_cast<size_t>(k), 0.0);
    std::vector<double> Hs(static_cast<size_t>(k) * k, 0.0);
    for (int64_t i = 0; i < n; ++i) {
        const double w = ps[i] * (1.0 - ps[i]);
        for (int a = 0; a < k; ++a) {
            const double xa = Xc[static_cast<int64_t>(a) * n + i];
            M1[a] += trt[i] * xa;
            M0[a] += w0v[i] * xa;
            M2[a] += w0v[i] * xa * (ehat[i] - att_c);
            for (int b = 0; b <= a; ++b)
                Hs[a * k + b] += w * xa * Xc[static_cast<int64_t>(b) * n + i];
        }
    }
    for (int a = 0; a < k; ++a)
        for (int b = a + 1; b < k; ++b) Hs[a * k + b] = Hs[b * k + a];
    std::vector<double> dM(static_cast<size_t>(k));
    for (int j = 0; j < k; ++j) {
        M1[j] /= sw1;
        M0[j] /= sw0;
        M2[j] /= sw0;
        dM[j] = M1[j] - M0[j];
    }
    // c_or' = (M1−M0)'Â⁻¹, c_ps' = M2'Ĥ⁻¹ with Â, Ĥ as per-observation means:
    // A and Hs above are sums, so scale the solutions by n.
    std::vector<double> c_or, c_ps;
    if (!spd_solve(A, dM.data(), k, c_or)) { out.ok = false; return out; }
    if (!spd_solve(Hs, M2.data(), k, c_ps)) {
        // Ĥ (the logit information) is singular under a (quasi-)separated
        // propensity fit: the fitted probabilities have reached the 0/1
        // boundary, so π̂(1−π̂) → 0. The propensity-estimation correction it
        // scales, ψ_ps = c_ps'X (D − π̂), vanishes anyway because (D_i − π̂_i)
        // → 0 under perfect prediction. Drop the term (c_ps = 0) and keep the
        // finite direct + outcome-regression influence, rather than the cell.
        c_ps.assign(static_cast<size_t>(k), 0.0);
    }
    for (int j = 0; j < k; ++j) {
        c_or[j] *= static_cast<double>(n);
        c_ps[j] *= static_cast<double>(n);
    }

    // Assemble ψ and the recentered influence function. The normalizations use
    // the per-observation means m1 = sw1/n, m0 = sw0/n.
    out.inf.resize(static_cast<size_t>(n));
    const double m1 = sw1 / static_cast<double>(n);
    const double m0 = sw0 / static_cast<double>(n);
    double ssq = 0;
    for (int64_t i = 0; i < n; ++i) {
        double xor_ = 0, xps = 0;
        for (int j = 0; j < k; ++j) {
            const double xa = Xc[static_cast<int64_t>(j) * n + i];
            xor_ += xa * c_or[j];
            xps += xa * c_ps[j];
        }
        const double psi_direct = trt[i] * (ehat[i] - att_t) / m1
                                - w0v[i] * (ehat[i] - att_c) / m0;
        const double psi_or = xor_ * (1.0 - trt[i]) * ehat[i];  // β̂ effect
        const double psi_ps = xps * (trt[i] - ps[i]);           // γ̂ effect
        const double psi_i = psi_direct - psi_or - psi_ps;
        out.inf[i] = out.att + psi_i;
        ssq += psi_i * psi_i;
    }
    out.se = std::sqrt(ssq) / static_cast<double>(n);

    // BUG-329 instrument (ONESHOT-ZERO WS-V). `CSDID_DIAG=1` emits, per cell, the
    // propensity-overlap statistics that ATTRIBUTE this estimator's SE-vs-Stata
    // residual. Kept in the tree rather than re-derived next time: the finding is
    // that the residual separates CLEANLY on max(pi-hat) — on the 21-cell
    // kitchensink corpus every cell with maxps <= 0.698 agrees with StataBE to
    // <= 2.7e-13 and every cell with maxps >= 0.750 sits at 4.1e-11 .. 7.2e-9,
    // with no overlap. It is a conditioning amplification, not an accumulation
    // order (three candidates falsified — see the BUGLOG entry), so re-running
    // the measurement must stay cheap. Zero cost when the variable is unset.
    if (std::getenv("CSDID_DIAG") != nullptr) {
        double maxps = 0, maxw0 = 0, sdir = 0, sps = 0;
        for (int64_t i = 0; i < n; ++i) {
            maxps = std::max(maxps, ps[i]);
            maxw0 = std::max(maxw0, w0v[i]);
        }
        for (int64_t i = 0; i < n; ++i) {
            double xps = 0;
            for (int j = 0; j < k; ++j) xps += Xc[static_cast<int64_t>(j) * n + i] * c_ps[j];
            const double pd = trt[i] * (ehat[i] - att_t) / m1 - w0v[i] * (ehat[i] - att_c) / m0;
            sdir += pd * pd;
            sps += (xps * (trt[i] - ps[i])) * (xps * (trt[i] - ps[i]));
        }
        std::fprintf(stderr,
                     "CSDID_DIAG n=%lld att=%.17g se=%.17g maxps=%.6f maxw0=%.6g "
                     "psi_ps_share=%.6g\n",
                     static_cast<long long>(n), out.att, out.se, maxps, maxw0,
                     sps / (sdir + 1e-300));
    }
    return out;
}

// ---------------------------------------------------------------------------
// One 2x2 outcome-regression cell — SZ20 §2 (panel): the regression-adjustment
// DiD estimator τ = Ê[D (ΔY − X'β̂)] / Ê[D], β̂ least squares of ΔY on X over
// the comparison units (constant appended as the last column of X).
//
// Influence function (delta method over β̂). With ê = ΔY − X'β̂, m1 = Ê[D],
// M1 = Ê[D X]/m1 and Â = Ê[(1−D) X X'] (the OLS bread):
//   ψ_i = D_i (ê_i − τ)/m1 − M1' Â⁻¹ X_i (1−D_i) ê_i.
// The first term is mean-zero by τ's definition, the second by the OLS normal
// equations, so inf_i = τ + ψ_i has mean τ; SE = sqrt(Σψ²)/n.
// With no covariate (X = 1) τ is the difference of mean ΔY between treated and
// comparison units, the same estimand and influence function as
// drdid_dripw_cell with a constant-only propensity and outcome model.
DrdidCell drdid_reg_cell(const double* dy, const double* X, int64_t n, int p,
                         const double* trt) {
    DrdidCell out;
    out.n = n;
    const int k = p + 1;
    if (n < k + 1) { out.ok = false; return out; }

    std::vector<double> Xc(static_cast<size_t>(n) * k);
    if (p > 0) std::memcpy(Xc.data(), X, sizeof(double) * static_cast<size_t>(n) * p);
    std::fill(Xc.begin() + static_cast<size_t>(n) * p, Xc.end(), 1.0);

    // Outcome model on the comparison group.
    std::vector<double> A(static_cast<size_t>(k) * k, 0.0), xy(static_cast<size_t>(k), 0.0);
    std::vector<double> beta;
    int64_t n1 = 0, n0 = 0;
    for (int64_t i = 0; i < n; ++i) {
        if (trt[i] != 0.0) { ++n1; continue; }
        ++n0;
        for (int a = 0; a < k; ++a) {
            const double xa = Xc[static_cast<int64_t>(a) * n + i];
            xy[a] += xa * dy[i];
            for (int b = 0; b <= a; ++b)
                A[a * k + b] += xa * Xc[static_cast<int64_t>(b) * n + i];
        }
    }
    for (int a = 0; a < k; ++a)
        for (int b = a + 1; b < k; ++b) A[a * k + b] = A[b * k + a];
    if (n1 < 1 || n0 < 1 || !spd_solve(A, xy.data(), k, beta)) { out.ok = false; return out; }

    std::vector<double> ehat(static_cast<size_t>(n));
    std::vector<double> M1(static_cast<size_t>(k), 0.0);
    double sw1 = 0, sw1e = 0;
    for (int64_t i = 0; i < n; ++i) {
        double xb = 0;
        for (int j = 0; j < k; ++j) xb += Xc[static_cast<int64_t>(j) * n + i] * beta[j];
        ehat[i] = dy[i] - xb;
        sw1 += trt[i];
        sw1e += trt[i] * ehat[i];
        for (int j = 0; j < k; ++j) M1[j] += trt[i] * Xc[static_cast<int64_t>(j) * n + i];
    }
    if (!(sw1 > 0)) { out.ok = false; return out; }
    out.att = sw1e / sw1;
    for (int j = 0; j < k; ++j) M1[j] /= sw1;

    // c_or' = M1'Â⁻¹ with Â a per-observation mean (A above is a sum).
    std::vector<double> c_or;
    if (!spd_solve(A, M1.data(), k, c_or)) { out.ok = false; return out; }
    for (int j = 0; j < k; ++j) c_or[j] *= static_cast<double>(n);

    out.inf.resize(static_cast<size_t>(n));
    const double m1 = sw1 / static_cast<double>(n);
    double ssq = 0;
    for (int64_t i = 0; i < n; ++i) {
        double xor_ = 0;
        for (int j = 0; j < k; ++j) xor_ += Xc[static_cast<int64_t>(j) * n + i] * c_or[j];
        const double psi_i = trt[i] * (ehat[i] - out.att) / m1 - xor_ * (1.0 - trt[i]) * ehat[i];
        out.inf[i] = out.att + psi_i;
        ssq += psi_i * psi_i;
    }
    out.se = std::sqrt(ssq) / static_cast<double>(n);
    return out;
}

// ---------------------------------------------------------------------------
// CS21 driver.

namespace {

// Included-in-aggregation gate: a cell contributes iff its DR estimation ran
// and its ATT is not exactly 0.0 (degenerate cells are excluded from every
// aggregation, and event times with no contributing cell are not enumerated).
inline bool cell_contributes(const CsdidCellEstimate& ce) {
    return ce.ok && ce.att != 0.0;
}

// A period or cohort value as it is spelled inside csdid's column names
// (`g2004:t_2003_2004`, `wgt:w2004_2005`): an integer value in plain digits.
std::string name_num(double v) {
    if (std::isfinite(v) && v == std::floor(v) && std::fabs(v) < 1e15)
        return std::to_string(static_cast<long long>(v));
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.9g", v);
    return buf;
}

}  // namespace

CsdidRunResult csdid_dripw_run(const double* ivar, const double* time,
                               const double* gvar, const double* y,
                               const double* X, int p, int64_t n_obs,
                               const CsdidConfig& cfg,
                               int event_lo, int event_hi) {
    CsdidRunResult R;
    if (n_obs <= 0) {
        R.ok = false;
        R.error = "csdid: empty data";
        return R;
    }

    // ---- Row screens.
    // "Panel" rows carry valid ids (unit, time, cohort): they define the unit
    // universe — the empirical distribution whose group shares P(G=g) weight
    // the CS21 aggregations. "Usable" rows additionally require the outcome
    // and every covariate to be non-missing: cell estimation samples require a
    // usable row at BOTH window endpoints (base and t), so a unit missing a
    // covariate at either endpoint is excluded from that whole cell.
    std::vector<uint8_t> panelrow(static_cast<size_t>(n_obs), 0);
    std::vector<uint8_t> usable(static_cast<size_t>(n_obs), 0);
    for (int64_t r = 0; r < n_obs; ++r) {
        const bool idok = std::isfinite(ivar[r]) && std::isfinite(time[r]) &&
                          std::isfinite(gvar[r]);
        panelrow[r] = idok ? 1 : 0;
        bool ok = idok && std::isfinite(y[r]);
        for (int c = 0; ok && c < p; ++c) ok = std::isfinite(X[static_cast<int64_t>(c) * n_obs + r]);
        usable[r] = ok ? 1 : 0;
    }

    // ---- Observed time levels.
    std::vector<double> levels;
    levels.reserve(64);
    {
        std::vector<double> tv;
        tv.reserve(static_cast<size_t>(n_obs));
        for (int64_t r = 0; r < n_obs; ++r)
            if (usable[r]) tv.push_back(time[r]);
        std::sort(tv.begin(), tv.end());
        tv.erase(std::unique(tv.begin(), tv.end()), tv.end());
        levels = std::move(tv);
    }
    const int L = static_cast<int>(levels.size());
    if (L < 2) {
        R.ok = false;
        R.error = "csdid: need at least two time periods";
        return R;
    }
    R.time0 = levels.front();
    double gdelta = levels[1] - levels[0];
    for (int i = 2; i < L; ++i) gdelta = std::min(gdelta, levels[i] - levels[i - 1]);
    const double ttol = 1e-8 * std::max(1.0, std::fabs(levels.back()));

    // ---- Units (first-appearance order over panel rows).
    std::unordered_map<double, int64_t> uix;
    uix.reserve(static_cast<size_t>(n_obs) / 4 + 8);
    std::vector<double> gvar_u;
    std::vector<int64_t> first_row;
    for (int64_t r = 0; r < n_obs; ++r) {
        if (!panelrow[r]) continue;
        auto it = uix.find(ivar[r]);
        if (it == uix.end()) {
            uix.emplace(ivar[r], static_cast<int64_t>(gvar_u.size()));
            gvar_u.push_back(gvar[r]);
            first_row.push_back(r);
        }
    }
    const int64_t N = static_cast<int64_t>(gvar_u.size());
    if (N == 0) {
        R.ok = false;
        R.error = "csdid: no usable observations";
        return R;
    }
    R.n_units = N;

    // Cluster index per unit (default: each unit its own cluster).
    std::vector<int> clu_u(static_cast<size_t>(N));
    int n_clusters = 0;
    if (cfg.cluster != nullptr) {
        std::unordered_map<double, int> cix;
        cix.reserve(static_cast<size_t>(N));
        for (int64_t u = 0; u < N; ++u) {
            const double cv = cfg.cluster[first_row[static_cast<size_t>(u)]];
            auto it = cix.find(cv);
            if (it == cix.end()) {
                it = cix.emplace(cv, n_clusters++).first;
            }
            clu_u[static_cast<size_t>(u)] = it->second;
        }
    } else {
        for (int64_t u = 0; u < N; ++u) clu_u[static_cast<size_t>(u)] = static_cast<int>(u);
        n_clusters = static_cast<int>(N);
    }
    const double Nd = static_cast<double>(N);
    R.n_clusters = n_clusters;

    // ---- (unit, level) -> row lookup.
    std::unordered_map<double, int> lev_ix;
    lev_ix.reserve(static_cast<size_t>(L) * 2);
    for (int i = 0; i < L; ++i) lev_ix.emplace(levels[static_cast<size_t>(i)], i);
    std::vector<int64_t> row_at(static_cast<size_t>(N) * L, -1);
    for (int64_t r = 0; r < n_obs; ++r) {
        if (!usable[r]) continue;
        const int64_t u = uix[ivar[r]];
        row_at[static_cast<size_t>(u) * L + lev_ix[time[r]]] = r;
    }

    // ---- Cohorts and group shares P(G=g) [CS21 §3].
    std::map<double, int64_t> gcount;
    for (int64_t u = 0; u < N; ++u)
        if (gvar_u[static_cast<size_t>(u)] > 0) ++gcount[gvar_u[static_cast<size_t>(u)]];
    for (const auto& kv : gcount) {
        R.glev.push_back(kv.first);
        R.pg.push_back(static_cast<double>(kv.second) / Nd);
    }
    const size_t G = R.glev.size();
    if (G == 0) {
        R.ok = false;
        R.error = "csdid: no treated cohorts (gvar > 0)";
        return R;
    }

    // Observed-level lookup with tolerance (grid values are arithmetic).
    auto find_level = [&](double v) -> int {
        auto it = std::lower_bound(levels.begin(), levels.end(), v - ttol);
        if (it != levels.end() && std::fabs(*it - v) <= ttol)
            return static_cast<int>(it - levels.begin());
        return -1;
    };
    // A grid period as the observed level it meets, if any (so a panel with no
    // gap keeps its observed period values bit for bit).
    auto on_level = [&](double v) -> double {
        const int ix = find_level(v);
        return ix >= 0 ? levels[static_cast<size_t>(ix)] : v;
    };

    // ---- Target periods: the regular gdelta-spaced grid from min to max
    // observed time, gap periods included as empty targets (csdid 1.81's
    // data check: range(min, max, min gap)). Default: every grid period after
    // time0 (BUG-1187: with no t == 4, StataBE posts g3's t_2_4 and g5's t_3_4
    // as empty cells). long2: time0 itself is an eligible long-difference target.
    std::vector<double> targets;
    {
        const int64_t J = llround((levels.back() - levels.front()) / gdelta);
        targets.reserve(static_cast<size_t>(J) + 1);
        for (int64_t j = cfg.long2 ? 0 : 1; j <= J; ++j) {
            const double v = levels.front() + static_cast<double>(j) * gdelta;
            targets.push_back(cfg.long2 ? v : on_level(v));
        }
    }
    R.tlev = targets;

    // ---- Per-cell estimation.
    const size_t K = G * targets.size();
    R.cells.reserve(K);
    std::vector<std::vector<double>> cell_psi(K);      // full-sample IFs (mean 0)
    std::vector<std::vector<int64_t>> cell_trt(K);     // treated units in the cell sample
    std::vector<int> cell_ev(K, 0);                    // event time e = (t-g)/gdelta
    std::vector<size_t> cell_gi(K, 0);                 // cohort index
    std::vector<uint8_t> used_row(static_cast<size_t>(n_obs), 0);

    std::vector<int64_t> memb;      // unit ids in the cell sample
    std::vector<int64_t> memb_rb, memb_rt;
    std::vector<double> dyv, Xcell, trtv;

    for (size_t gi = 0; gi < G; ++gi) {
        const double g = R.glev[gi];
        for (size_t tj = 0; tj < targets.size(); ++tj) {
            const double t = targets[tj];
            CsdidCellEstimate ce;
            ce.g = g;
            ce.t = t;
            ce.att = 0;
            ce.se = 0;
            ce.n_cell = 0;
            ce.ok = false;

            // Base period [CS21 §2-3]: long2 uses the fixed pre-treatment
            // period g - gdelta; otherwise the varying base: csdid.ado's loop
            // starts it at time0 and moves it to each grid period j < g once
            // j's cell is done, so it is the grid period preceding t for a
            // pre-treatment cell and the last grid period preceding g for a
            // post one, observed or not (BUG-1187: g = 5 with no t == 4 posts
            // t_4_5 and t_4_6 as empty cells, not t_3_5 / t_3_6 off period 3).
            double base = 0;
            bool base_defined = false;
            if (cfg.long2) {
                base = g - gdelta;
                base_defined = true;
            } else {
                const double ref = (t < g) ? t : g;
                const double jb = std::ceil((ref - levels.front()) / gdelta - 1e-9) - 1.0;
                if (jb >= 0.0) {
                    base = on_level(levels.front() + jb * gdelta);
                    base_defined = true;
                }
            }
            ce.base = base_defined ? base : 0.0;

            const size_t kcell = gi * targets.size() + tj;
            cell_gi[kcell] = gi;
            cell_ev[kcell] = static_cast<int>(llround((t - g) / gdelta));

            const int tix = find_level(t);
            const int bix = base_defined ? find_level(base) : -1;
            if (tix < 0 || bix < 0 || std::fabs(t - base) <= ttol) {
                R.cells.push_back(ce);  // blank: gap target / missing base / t == base
                continue;
            }

            // Cell sample: cohort-g units plus the comparison group, requiring
            // usable rows at BOTH window endpoints (base and t).
            memb.clear();
            memb_rb.clear();
            memb_rt.clear();
            for (int64_t u = 0; u < N; ++u) {
                const double gu = gvar_u[static_cast<size_t>(u)];
                const bool treated = (gu == g);
                bool control = (gu == 0.0);
                // Not-yet-treated as csdid 1.81 selects it (long2 likewise):
                // gvar > g AND gvar > t, so a cohort treated before g is never a
                // comparison, also in g's pre-period cells (BUG-956).
                if (!control && cfg.comparison == CsdidComparison::Notyet)
                    control = gu > std::max(g, t) + ttol;
                if (!treated && !control) continue;
                const int64_t rb = row_at[static_cast<size_t>(u) * L + bix];
                const int64_t rt = row_at[static_cast<size_t>(u) * L + tix];
                if (rb < 0 || rt < 0) continue;
                memb.push_back(u);
                memb_rb.push_back(rb);
                memb_rt.push_back(rt);
            }
            const int64_t n = static_cast<int64_t>(memb.size());
            if (n < p + 2) {
                R.cells.push_back(ce);
                continue;
            }
            dyv.resize(static_cast<size_t>(n));
            trtv.resize(static_cast<size_t>(n));
            Xcell.resize(static_cast<size_t>(n) * static_cast<size_t>(p));
            for (int64_t m = 0; m < n; ++m) {
                dyv[static_cast<size_t>(m)] = y[memb_rt[static_cast<size_t>(m)]] - y[memb_rb[static_cast<size_t>(m)]];
                trtv[static_cast<size_t>(m)] =
                    (gvar_u[static_cast<size_t>(memb[static_cast<size_t>(m)])] == g) ? 1.0 : 0.0;
                // Covariates enter at their base-period values.
                for (int c = 0; c < p; ++c)
                    Xcell[static_cast<size_t>(c) * n + m] =
                        X[static_cast<int64_t>(c) * n_obs + memb_rb[static_cast<size_t>(m)]];
            }

            DrdidCell cell =
                cfg.method == CsdidMethod::Reg
                    ? drdid_reg_cell(dyv.data(), p > 0 ? Xcell.data() : nullptr, n, p, trtv.data())
                    : drdid_dripw_cell(dyv.data(), p > 0 ? Xcell.data() : nullptr, n, p,
                                       trtv.data(), cfg.pscoretrim);
            if (!cell.ok) {
                R.cells.push_back(ce);
                continue;
            }
            ce.ok = true;
            ce.att = cell.att;
            ce.n_cell = cell.n;

            // Full-sample influence function: the cell subsample enters the
            // N-unit empirical distribution with selection weight N/n. Also
            // record the treated members: their count is the cell's sample
            // analog of P(G=g), which weights the CS21 aggregations.
            auto& psi = cell_psi[kcell];
            psi.assign(static_cast<size_t>(N), 0.0);
            const double scale = Nd / static_cast<double>(n);
            for (int64_t m = 0; m < n; ++m) {
                psi[static_cast<size_t>(memb[static_cast<size_t>(m)])] =
                    scale * (cell.inf[static_cast<size_t>(m)] - cell.att);
                if (trtv[static_cast<size_t>(m)] != 0.0)
                    cell_trt[kcell].push_back(memb[static_cast<size_t>(m)]);
            }
            // e(N) row accounting: both endpoint rows of every sampled unit.
            for (int64_t m = 0; m < n; ++m) {
                used_row[static_cast<size_t>(memb_rb[static_cast<size_t>(m)])] = 1;
                used_row[static_cast<size_t>(memb_rt[static_cast<size_t>(m)])] = 1;
            }
            R.cells.push_back(ce);
        }
    }

    // FIX:PLEVEL-WORKER-RSS-SCOPING owed instrumentation (2026-07-29 §7, landed
    // ONESHOT-P9N4): the one-line memory census without which every scratch-
    // reduction candidate is a guess. Report-only, env-gated. The env value is a
    // FILE PATH to append to, not a flag: the engine's stderr is discarded on
    // every success path PIET owns ([[cobra-piet-warning-channel]] — measured,
    // not a choice this instrument gets to revisit), so stderr-only would be
    // unreachable behind the product. Appending is deliberate: one bench run =
    // one line per csdid call, across however many serve processes.
    if (const char* memlog = std::getenv("COBRA_CSDID_MEMLOG"); memlog && *memlog) {
        int64_t nnz = 0, ok_cells = 0, max_n = 0;
        for (const auto& c : R.cells)
            if (c.ok) { ++ok_cells; nnz += c.n_cell; max_n = std::max(max_n, c.n_cell); }
        if (std::FILE* mf = std::fopen(memlog, "a")) {
            std::fprintf(mf,
                         "[csdid-memlog] N=%lld L=%d K=%zu ok_cells=%lld p=%d n_clusters=%d "
                         "max_n_cell=%lld cell_psi_dense=%.1fMB (nnz=%lld -> sparse=%.1fMB) "
                         "row_at=%.1fMB\n",
                         static_cast<long long>(N), L, K, static_cast<long long>(ok_cells), p,
                         n_clusters, static_cast<long long>(max_n),
                         static_cast<double>(K) * Nd * 8.0 / 1048576.0, static_cast<long long>(nnz),
                         static_cast<double>(nnz) * 16.0 / 1048576.0,
                         static_cast<double>(N) * L * 8.0 / 1048576.0);
            std::fclose(mf);
        }
    }

    // ---- Cluster folding helpers: V = Σ_c S_c S_c' / N².
    auto folded_se = [&](const std::vector<double>& psi) -> double {
        std::vector<double> S(static_cast<size_t>(n_clusters), 0.0);
        for (int64_t u = 0; u < N; ++u) S[static_cast<size_t>(clu_u[static_cast<size_t>(u)])] += psi[static_cast<size_t>(u)];
        double v = 0;
        for (double s : S) v += s * s;
        return std::sqrt(v) / Nd;
    };
    // THE covariance builder: every V this driver reports (the per-cell and
    // aggregate SEs, the pre-trends Wald V, the attgt/agg postings, estat
    // event's V) is the joint covariance of m influence-function columns,
    //   V = Σ_c S_c S_c' / N²,  S_c = per-cluster sums of the columns
    // (returns m×m, row-major == col-major by symmetry). A column may carry a
    // constant shift h (the attgt wgt columns are an indicator minus its mean):
    // its value at unit u is ψ(u) + h, its cluster sum Σ_{u∈c} ψ(u) + h·n_c.
    // When every folded column is dense and none is shifted, the gram is one
    // la::xtx; otherwise each entry is the compensated dot over the sparser
    // column's nonzero clusters (exact zeros contribute nothing), plus the
    // shift terms h_j (S_i·n) + h_i (S_j·n) + h_i h_j (n·n).
    std::vector<double> clu_n(static_cast<size_t>(n_clusters), 0.0);
    for (int64_t u = 0; u < N; ++u) clu_n[static_cast<size_t>(clu_u[static_cast<size_t>(u)])] += 1.0;
    auto folded_cov_shifted = [&](const std::vector<const std::vector<double>*>& cols,
                                  const std::vector<double>& shift) -> std::vector<double> {
        const int m = static_cast<int>(cols.size());
        std::vector<double> C(static_cast<size_t>(n_clusters) * m, 0.0);
        for (int j = 0; j < m; ++j) {
            const std::vector<double>& pj = *cols[static_cast<size_t>(j)];
            double* cj = C.data() + static_cast<size_t>(j) * n_clusters;
            for (int64_t u = 0; u < N; ++u) cj[clu_u[static_cast<size_t>(u)]] += pj[static_cast<size_t>(u)];
        }
        std::vector<double> V(static_cast<size_t>(m) * m, 0.0);
        const double inv = 1.0 / (Nd * Nd);
        std::vector<std::vector<int>> nz(static_cast<size_t>(m));
        bool all_dense = true;
        for (int j = 0; j < m; ++j) {
            const double* cj = C.data() + static_cast<size_t>(j) * n_clusters;
            for (int c = 0; c < n_clusters; ++c)
                if (cj[c] != 0.0) nz[static_cast<size_t>(j)].push_back(c);
            if (nz[static_cast<size_t>(j)].size() * 2 < static_cast<size_t>(n_clusters)) all_dense = false;
        }
        const bool shifted = !shift.empty();
        if (all_dense && !shifted) {
            la::xtx(C.data(), n_clusters, m, V.data());
            for (double& v : V) v *= inv;
            return V;
        }
        std::vector<double> cn(static_cast<size_t>(m), 0.0);  // S_j · n
        double nn = 0;
        if (shifted) {
            for (int j = 0; j < m; ++j)
                cn[static_cast<size_t>(j)] =
                    la::dot(C.data() + static_cast<size_t>(j) * n_clusters, clu_n.data(), n_clusters);
            nn = la::dot(clu_n.data(), clu_n.data(), n_clusters);
        }
        std::vector<double> ga, gb;
        for (int j = 0; j < m; ++j) {
            const double* cj = C.data() + static_cast<size_t>(j) * n_clusters;
            for (int i = 0; i <= j; ++i) {
                const double* ci = C.data() + static_cast<size_t>(i) * n_clusters;
                const std::vector<int>& sp =
                    nz[static_cast<size_t>(i)].size() <= nz[static_cast<size_t>(j)].size()
                        ? nz[static_cast<size_t>(i)] : nz[static_cast<size_t>(j)];
                double v;
                if (sp.size() * 2 < static_cast<size_t>(n_clusters)) {
                    ga.resize(sp.size());
                    gb.resize(sp.size());
                    for (size_t q = 0; q < sp.size(); ++q) {
                        ga[q] = ci[sp[q]];
                        gb[q] = cj[sp[q]];
                    }
                    v = sp.empty() ? 0.0 : la::dot(ga.data(), gb.data(), static_cast<int64_t>(sp.size()));
                } else {
                    v = la::dot(ci, cj, n_clusters);
                }
                if (shifted) {
                    const double hi = shift[static_cast<size_t>(i)], hj = shift[static_cast<size_t>(j)];
                    v += hj * cn[static_cast<size_t>(i)] + hi * cn[static_cast<size_t>(j)] + hi * hj * nn;
                }
                V[static_cast<size_t>(i) + static_cast<size_t>(j) * m] = v * inv;
                V[static_cast<size_t>(j) + static_cast<size_t>(i) * m] = v * inv;
            }
        }
        return V;
    };
    auto folded_cov = [&](const std::vector<const std::vector<double>*>& cols) {
        return folded_cov_shifted(cols, {});
    };

    // Per-cell SEs (cluster-folded; reduces to the IID formula without cluster).
    for (size_t kc = 0; kc < K; ++kc)
        if (R.cells[kc].ok) R.cells[kc].se = folded_se(cell_psi[kc]);

    // ---- csdid's e(b)/e(V), agg(attgt) layout (csdid-drdid LAYOUT.md 0, 1.1).
    // One column per cell, cohort-major, named `g<g>:t_<a>_<b>` (a < b the two
    // window endpoints), then one `wgt:w<g>_<t>` column per cell, t running over
    // the target periods without time0 (positionally: the k-th wgt column
    // belongs to the k-th cell, also under long2 where the cells' own periods
    // include time0). The wgt column of cell k is its share of treated units,
    // n1_k/N, normalised by the sum S of those shares over all posted cells;
    // its influence function is (1{u ∈ T_k} − n1_k/N)/S. A cell that could not
    // be estimated posts zero columns. long2 posts no column for the cohort's
    // t == base cell. V is the joint covariance of all columns' influence
    // functions (the same folded_cov as every aggregate below).
    const std::vector<double> zero_psi(static_cast<size_t>(N), 0.0);
    {
        std::vector<size_t> post_ix;
        post_ix.reserve(K);
        for (size_t kc = 0; kc < K; ++kc) {
            const CsdidCellEstimate& ce = R.cells[kc];
            if (cfg.long2 && std::fabs(ce.t - ce.base) <= ttol) continue;
            post_ix.push_back(kc);
        }
        const size_t P = post_ix.size();
        const std::vector<double> wnames(targets.begin() + (cfg.long2 ? 1 : 0), targets.end());
        double S = 0;
        for (size_t kc : post_ix) S += static_cast<double>(cell_trt[kc].size()) / Nd;
        CsdidPosting& A = R.attgt;
        A.names.reserve(2 * P);
        A.b.reserve(2 * P);
        std::vector<const std::vector<double>*> cols;
        cols.reserve(2 * P);
        for (size_t kc : post_ix) {
            const CsdidCellEstimate& ce = R.cells[kc];
            A.names.push_back("g" + name_num(ce.g) + ":t_" + name_num(std::min(ce.base, ce.t)) +
                              "_" + name_num(std::max(ce.base, ce.t)));
            A.b.push_back(ce.ok ? ce.att : 0.0);
            cols.push_back(cell_psi[kc].empty() ? &zero_psi : &cell_psi[kc]);
        }
        std::vector<std::vector<double>> wpsi(P);
        std::vector<double> shift(P, 0.0);  // the cells' columns are not shifted
        size_t jg = 0;
        for (size_t j = 0; j < P; ++j) {
            const size_t kc = post_ix[j];
            const CsdidCellEstimate& ce = R.cells[kc];
            if (j == 0 || ce.g != R.cells[post_ix[j - 1]].g) jg = 0;
            const double tw = jg < wnames.size() ? wnames[jg] : ce.t;
            ++jg;
            A.names.push_back("wgt:w" + name_num(ce.g) + "_" + name_num(tw));
            const double share = static_cast<double>(cell_trt[kc].size()) / Nd;
            std::vector<double>& w = wpsi[j];
            w.assign(static_cast<size_t>(N), 0.0);
            if (S > 0) {
                A.b.push_back(share / S);
                for (int64_t u : cell_trt[kc]) w[static_cast<size_t>(u)] = 1.0 / S;
                shift.push_back(-share / S);
            } else {
                A.b.push_back(0.0);
                shift.push_back(0.0);
            }
            cols.push_back(&w);
        }
        A.V = folded_cov_shifted(cols, shift);
    }

    // ---- CS21 §3-4 aggregation: θ = Σ_k w_k a_k with w_k ∝ P̂_k, where
    // P̂_k = n1_k / N is the sample share of cohort-g_k units usable in cell
    // k's estimation window (the cell-level sample analog of P(G=g); with
    // complete data it reduces to the global group share). The influence
    // function is Σ_k w_k ψ_k plus the share-estimation effect
    //   (1/P) Σ_k (a_k − θ)(1{u ∈ T_k} − P̂_k),  P = Σ_k P̂_k,
    // with T_k the treated members of cell k (delta method on the ratio of
    // estimated shares, CS21 §4.1).
    struct Agg {
        double att = 0;
        std::vector<double> psi;
        bool any = false;
    };
    auto combine = [&](const std::vector<size_t>& cells_ix) -> Agg {
        Agg out;
        if (cells_ix.empty()) return out;
        double sn1 = 0, num = 0;  // Σ n1_k and Σ n1_k a_k
        for (size_t kc : cells_ix) {
            const double n1 = static_cast<double>(cell_trt[kc].size());
            sn1 += n1;
            num += n1 * R.cells[kc].att;
        }
        const double theta = num / sn1;
        out.att = theta;
        out.psi.assign(static_cast<size_t>(N), 0.0);
        // direct part: Σ_k (n1_k/Σn1) ψ_k
        for (size_t kc : cells_ix) {
            const double w = static_cast<double>(cell_trt[kc].size()) / sn1;
            const std::vector<double>& pk = cell_psi[kc];
            for (int64_t u = 0; u < N; ++u) out.psi[static_cast<size_t>(u)] += w * pk[static_cast<size_t>(u)];
        }
        // share-estimation part: (N/Σn1)[Σ_k (a_k − θ) 1{u ∈ T_k} − c0],
        // c0 = Σ_k (a_k − θ) n1_k / N.
        double c0 = 0;
        std::vector<double> acc(static_cast<size_t>(N), 0.0);
        for (size_t kc : cells_ix) {
            const double d = R.cells[kc].att - theta;
            c0 += d * static_cast<double>(cell_trt[kc].size()) / Nd;
            for (int64_t u : cell_trt[kc]) acc[static_cast<size_t>(u)] += d;
        }
        const double wnorm = Nd / sn1;
        for (int64_t u = 0; u < N; ++u)
            out.psi[static_cast<size_t>(u)] += (acc[static_cast<size_t>(u)] - c0) * wnorm;
        out.any = true;
        return out;
    };

    // ---- Simple overall ATT [CS21 §3.1]: all contributing post cells (e ≥ 0),
    // weighted by group share (not restricted by the event window).
    {
        std::vector<size_t> post_ix;
        for (size_t kc = 0; kc < K; ++kc)
            if (cell_contributes(R.cells[kc]) && cell_ev[kc] >= 0) post_ix.push_back(kc);
        Agg s = combine(post_ix);
        R.agg_simple.names = {"ATT"};
        if (s.any) {
            R.simple_att = s.att;
            R.simple_se = folded_se(s.psi);
            R.agg_simple.b = {s.att};
            R.agg_simple.V = folded_cov({&s.psi});
        } else {
            R.agg_simple.b = {0.0};
            R.agg_simple.V = {0.0};
        }
    }

    // ---- Event-study (dynamic) bins [CS21 §3.1], within the event window.
    const bool win_all = (event_lo == 0 && event_hi == 0);
    std::map<int, std::vector<size_t>> bins;
    for (size_t kc = 0; kc < K; ++kc) {
        if (!cell_contributes(R.cells[kc])) continue;
        const int e = cell_ev[kc];
        // The window is read on the event list estat builds from (cohort,
        // target) POSITIONS over the target grid without time0 (LAYOUT.md 1.4):
        // under long2 a pre-period cell sits one step later there (its base,
        // e == -1, takes no position), so its window coordinate is e + 1.
        const int ew = (cfg.long2 && e < 0) ? e + 1 : e;
        if (!win_all && (ew < event_lo || ew > event_hi)) continue;
        bins[e].push_back(kc);
    }
    std::vector<std::vector<double>> bin_psi;
    std::vector<int> bin_e;
    for (const auto& kv : bins) {
        Agg b = combine(kv.second);
        if (!b.any) continue;
        CsdidEventBin eb;
        eb.e = kv.first;
        eb.att = b.att;
        eb.se = folded_se(b.psi);
        R.events.push_back(eb);
        bin_e.push_back(kv.first);
        bin_psi.push_back(std::move(b.psi));
    }

    // ---- Pre_avg / Post_avg: unweighted averages of the (windowed) event bins.
    std::vector<double> psi_pre(static_cast<size_t>(N), 0.0), psi_post(static_cast<size_t>(N), 0.0);
    {
        int npre = 0, npost = 0;
        double apre = 0, apost = 0;
        for (size_t j = 0; j < bin_e.size(); ++j) {
            if (bin_e[j] < 0) {
                ++npre;
                apre += R.events[j].att;
            } else {
                ++npost;
                apost += R.events[j].att;
            }
        }
        if (npre > 0) {
            R.pre_avg = apre / npre;
            for (size_t j = 0; j < bin_e.size(); ++j)
                if (bin_e[j] < 0)
                    for (int64_t u = 0; u < N; ++u)
                        psi_pre[static_cast<size_t>(u)] += bin_psi[j][static_cast<size_t>(u)] / npre;
            R.pre_avg_se = folded_se(psi_pre);
        }
        if (npost > 0) {
            R.post_avg = apost / npost;
            for (size_t j = 0; j < bin_e.size(); ++j)
                if (bin_e[j] >= 0)
                    for (int64_t u = 0; u < N; ++u)
                        psi_post[static_cast<size_t>(u)] += bin_psi[j][static_cast<size_t>(u)] / npost;
            R.post_avg_se = folded_se(psi_post);
        }
    }

    // ---- Calendar-period effects [CS21 §3.1]: contributing post cells grouped
    // by outcome period (not restricted by the event window).
    {
        std::map<double, std::vector<size_t>> byt;
        for (size_t kc = 0; kc < K; ++kc)
            if (cell_contributes(R.cells[kc]) && cell_ev[kc] >= 0) byt[R.cells[kc].t].push_back(kc);
        std::vector<std::vector<double>> cal_psi;
        for (const auto& kv : byt) {
            Agg c = combine(kv.second);
            if (!c.any) continue;
            CsdidRunResult::CalendarBin cb;
            cb.t = kv.first;
            cb.att = c.att;
            cb.se = folded_se(c.psi);
            R.calendar.push_back(cb);
            cal_psi.push_back(std::move(c.psi));
        }
        // agg(calendar): CAverage (the unweighted mean of the periods) then T#.
        CsdidPosting& P = R.agg_calendar;
        const size_t C = R.calendar.size();
        std::vector<double> avg(static_cast<size_t>(N), 0.0);
        double ab = 0;
        for (size_t j = 0; j < C; ++j) {
            ab += R.calendar[j].att / static_cast<double>(C);
            for (int64_t u = 0; u < N; ++u)
                avg[static_cast<size_t>(u)] += cal_psi[j][static_cast<size_t>(u)] / static_cast<double>(C);
        }
        P.names.push_back("CAverage");
        P.b.push_back(ab);
        std::vector<const std::vector<double>*> cols{&avg};
        for (size_t j = 0; j < C; ++j) {
            P.names.push_back("T" + name_num(R.calendar[j].t));
            P.b.push_back(R.calendar[j].att);
            cols.push_back(&cal_psi[j]);
        }
        P.V = folded_cov(cols);
    }

    // ---- Group (cohort) effects [CS21 §3.1]: contributing post cells grouped
    // by cohort (not restricted by the event window). Within a cohort the
    // combine() weights n1_k/Σn1 reduce to the paper's equal per-period
    // weighting whenever the usable treated count is constant across the
    // cohort's post cells (e.g. balanced panels).
    {
        std::map<double, std::vector<size_t>> byg;
        for (size_t kc = 0; kc < K; ++kc)
            if (cell_contributes(R.cells[kc]) && cell_ev[kc] >= 0) byg[R.cells[kc].g].push_back(kc);
        std::vector<std::vector<double>> grp_psi;
        std::vector<double> grp_w;  // mean treated count over the cohort's post cells
        for (const auto& kv : byg) {
            Agg a = combine(kv.second);
            if (!a.any) continue;
            CsdidRunResult::GroupBin gb;
            gb.g = kv.first;
            gb.att = a.att;
            gb.se = folded_se(a.psi);
            R.groups.push_back(gb);
            grp_psi.push_back(std::move(a.psi));
            double sn1 = 0;
            for (size_t kc : kv.second) sn1 += static_cast<double>(cell_trt[kc].size());
            grp_w.push_back(sn1 / static_cast<double>(kv.second.size()));
        }
        // agg(group): GAverage then G#. GAverage weights each cohort by the
        // mean treated count of its post cells, held FIXED (no share-estimation
        // term), as `estat group` does from the posted wgt columns.
        CsdidPosting& P = R.agg_group;
        const size_t Gn = R.groups.size();
        double sw = 0;
        for (double v : grp_w) sw += v;
        std::vector<double> avg(static_cast<size_t>(N), 0.0);
        double ab = 0;
        for (size_t j = 0; j < Gn; ++j) {
            const double wj = sw > 0 ? grp_w[j] / sw : 0.0;
            ab += wj * R.groups[j].att;
            for (int64_t u = 0; u < N; ++u)
                avg[static_cast<size_t>(u)] += wj * grp_psi[j][static_cast<size_t>(u)];
        }
        P.names.push_back("GAverage");
        P.b.push_back(ab);
        std::vector<const std::vector<double>*> cols{&avg};
        for (size_t j = 0; j < Gn; ++j) {
            P.names.push_back("G" + name_num(R.groups[j].g));
            P.b.push_back(R.groups[j].att);
            cols.push_back(&grp_psi[j]);
        }
        P.V = folded_cov(cols);
    }

    // ---- Pre-trends joint Wald test: H0: att(g,t) = 0 for all contributing
    // pre-treatment cells (e < 0), covariance from the cell influence functions.
    {
        std::vector<size_t> pre_ix;
        for (size_t kc = 0; kc < K; ++kc)
            if (cell_contributes(R.cells[kc]) && cell_ev[kc] < 0) pre_ix.push_back(kc);
        const int df = static_cast<int>(pre_ix.size());
        R.pretrend_df = df;
        if (df > 0) {
            std::vector<double> a(static_cast<size_t>(df));
            std::vector<const std::vector<double>*> cols(static_cast<size_t>(df));
            for (int j = 0; j < df; ++j) {
                a[static_cast<size_t>(j)] = R.cells[pre_ix[static_cast<size_t>(j)]].att;
                cols[static_cast<size_t>(j)] = &cell_psi[pre_ix[static_cast<size_t>(j)]];
            }
            std::vector<double> V = folded_cov(cols);
            std::vector<double> Rmat(static_cast<size_t>(df) * df, 0.0);
            std::vector<double> q(static_cast<size_t>(df), 0.0);
            for (int j = 0; j < df; ++j) Rmat[static_cast<size_t>(j) * df + j] = 1.0;
            WaldResult w = wald_test(a.data(), V.data(), df, Rmat.data(), q.data(), df);
            if (w.ok) {
                R.pretrend_chi2 = w.chi2;
                R.pretrend_p = chi2_pvalue(w.chi2, df);
            }
        }
    }

    // ---- [Pre_avg, Post_avg, Tm#…Tp#]: agg(event)'s e(b)/e(V) with the full
    // covariance of the 2+E influence functions, and the `estat event, post`
    // e(V), which is the same matrix with the averages-by-bins blocks zeroed
    // (estat event posts the two blocks block-diagonally; LAYOUT.md 1.6, 1.8).
    {
        const size_t E = R.events.size();
        R.event_names.reserve(2 + E);
        R.event_b.reserve(2 + E);
        R.event_names.push_back("Pre_avg");
        R.event_names.push_back("Post_avg");
        R.event_b.push_back(R.pre_avg);
        R.event_b.push_back(R.post_avg);
        for (size_t j = 0; j < E; ++j) {
            const int e = R.events[j].e;
            R.event_names.push_back(e < 0 ? "Tm" + std::to_string(-e)
                                          : "Tp" + std::to_string(e));
            R.event_b.push_back(R.events[j].att);
        }
        const size_t m = 2 + E;
        std::vector<const std::vector<double>*> cols{&psi_pre, &psi_post};
        for (size_t j = 0; j < E; ++j) cols.push_back(&bin_psi[j]);
        R.agg_event.names = R.event_names;
        R.agg_event.b = R.event_b;
        R.agg_event.V = folded_cov(cols);
        R.event_V = R.agg_event.V;
        for (size_t r = 0; r < m; ++r)
            for (size_t c = 0; c < m; ++c)
                if ((r < 2) != (c < 2)) R.event_V[r * m + c] = 0.0;
    }

    // ---- e(N): union of endpoint rows over all estimated cell samples.
    int64_t nused = 0;
    for (int64_t r = 0; r < n_obs; ++r) nused += used_row[static_cast<size_t>(r)];
    R.n_obs_est = nused;

    return R;
}

}  // namespace cecon
