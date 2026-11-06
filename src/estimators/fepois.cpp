// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "estimators/fepois.h"
#include "core/error.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

#include "core/linalg.h"
#include "hdfe/demean.h"

namespace cecon {

// IRLS for Poisson with HDFE absorption. Each step: weighted within-demean of the
// working response z and X by w=mu, then a weighted OLS. (For canonical Poisson,
// IRLS == Newton-Raphson, so this is the shared MLE step the spec calls for.)
FepoisResult fepois_fit(ModelData& md, Vcov vt, int cluster_index,
                        std::vector<double>* fe_sum_out) {
    DesignMatrix& dm = md.dm;
    const int64_t n = dm.n;
    const int k = dm.k;
    require_sample(n);
    if (n <= k) throw EngineError("no_obs", "not enough observations");
    const double* off = md.offset.empty() ? nullptr : md.offset.data();
    // P5 pweight: raw (UN-normalized) per-row weight, nullptr = unweighted (every
    // branch below that reads pw is gated so the unweighted path stays exactly
    // what it was before pweight existed). With absorbed effects (BUG-1249) the
    // working weight pw*mu drives the weighted within transform, the _cons
    // recovery and the augmented VCE alike (ppmlhdfe's own check that mu :* true_w equals
    // the HDFE weight).
    const double* pw = md.weights.empty() ? nullptr : md.weights.data();

    FepoisResult r;
    r.n = n;
    r.k = k;
    r.names = dm.xnames;
    r.n_dropped = dm.n_dropped;
    r.has_offset = (off != nullptr);

    double ybar = 0;
    for (int64_t i = 0; i < n; ++i) ybar += dm.y[i];
    ybar /= static_cast<double>(n);
    std::vector<double> mu(n), eta(n);
    for (int64_t i = 0; i < n; ++i) {
        double m = (dm.y[i] + ybar) / 2.0;
        if (m < 1e-8) m = 1e-8;
        mu[i] = m;
        eta[i] = std::log(m);
    }

    // z and X share one contiguous n×(k+1) buffer (z = column 0, X = columns 1..k) so a
    // single weighted demean() absorbs both — one wsum pass per IRLS iter instead of two.
    // Per-column demeaning is independent, so this is bit-identical to two separate calls.
    std::vector<double> zXd(static_cast<size_t>(n) * (k + 1)), z(n), w(n), sw(n);
    double* zd = zXd.data();
    double* Xd = zXd.data() + n;
    std::vector<double> bread(static_cast<size_t>(k) * k), Xs(static_cast<size_t>(n) * k);
    std::vector<double> zs(n), Xty(k), beta(k), resid(n);
    // The working weights of the LAST weighted regression (mu before its update):
    // ppmlhdfe recovers _cons with these, not with the updated mu (see below).
    std::vector<double> w_last(n);
    double dev = 0, dev_old = 1e300;
    int iter = 0, dem_iters = 0;
    bool dem_conv = true;
    const int MAXIT = 100;
    for (; iter < MAXIT; ++iter) {
        // IRLS working weight: mu (canonical Poisson) times the raw pweight when
        // present. z (below) is UNCHANGED by pw — a per-row weight only rescales
        // the WLS normal equations (Sum pw_i*mu_i*x_i*x_i' Delta = Sum pw_i*mu_i*
        // x_i*z_i), not the pseudo-response — exactly mirroring logit_fit's pw
        // treatment (see logit.cpp). pw==nullptr reproduces w[i]=mu[i] exactly.
        for (int64_t i = 0; i < n; ++i) w[i] = pw ? mu[i] * pw[i] : mu[i];
        std::copy(w.begin(), w.end(), w_last.begin());
        for (int64_t i = 0; i < n; ++i) {
            double o = off ? off[i] : 0.0;
            z[i] = (eta[i] - o) + (dm.y[i] - mu[i]) / mu[i];
        }
        std::copy(z.begin(), z.end(), zd);  // z changes every IRLS iter — always cold
        // Warm-start X: after iter 0 re-demean the PREVIOUS demeaned Xd under the new
        // weights instead of resetting to raw X. Exact by FE-subspace fixed-point
        // invariance — Xd_prev = X - p with p in the FE span, and demean_w annihilates
        // any FE-span component, so demean_w(Xd_prev) = demean_w(X) at convergence; only
        // the iteration trajectory (hence ULP-level rounding) differs. [FP-REORDER]
        if (iter == 0) std::copy(dm.X.begin(), dm.X.end(), Xd);
        DemeanReport dzx = demean(zXd.data(), n, k + 1, md.fe, w.data());
        dem_iters = dzx.iters;
        dem_conv = dzx.converged;
        for (int64_t i = 0; i < n; ++i) sw[i] = std::sqrt(w[i]);
        for (int c = 0; c < k; ++c) {
            const double* xc = Xd + static_cast<size_t>(c) * n;
            double* sc = Xs.data() + static_cast<size_t>(c) * n;
            for (int64_t i = 0; i < n; ++i) sc[i] = xc[i] * sw[i];
        }
        for (int64_t i = 0; i < n; ++i) zs[i] = zd[i] * sw[i];
        if (k > 0) {
            la::xtx(Xs.data(), n, k, bread.data());
            if (la::spd_inverse(bread.data(), k) != 0) { r.separation = true; break; }
            la::xtv(Xs.data(), n, k, zs.data(), Xty.data());
            la::matvec(bread.data(), k, Xty.data(), beta.data());
            la::residual(Xd, n, k, beta.data(), zd, resid.data());  // zd - Xd*beta
        } else {
            // No regressors (only absorbed effects, `--no-x --fe`): the fitted
            // index is the FE projection of z, i.e. z minus its demeaned part.
            std::copy(zd, zd + n, resid.begin());
        }
        dev = 0;
        for (int64_t i = 0; i < n; ++i) {
            double o = off ? off[i] : 0.0;
            double xb = z[i] - resid[i];
            eta[i] = xb + o;
            double m = std::exp(eta[i]);
            if (m < 1e-10) m = 1e-10;
            if (m > 1e15) m = 1e15;
            mu[i] = m;
            double yi = dm.y[i];
            double di = 2.0 * ((yi > 0 ? yi * std::log(yi / m) : 0.0) - (yi - m));
            dev += pw ? pw[i] * di : di;  // weighted convergence monitor under pweight
        }
        if (std::fabs(dev - dev_old) / (std::fabs(dev) + 0.1) < 1e-10) {
            dev_old = dev;
            ++iter;
            r.converged = true;
            break;
        }
        dev_old = dev;
    }
    r.irls_iters = iter;
    r.converged = r.converged && !r.separation;
    r.deviance = dev_old;
    r.demean_iters = dem_iters;
    r.demean_converged = dem_conv;
    r.beta = beta;

    // Consistent bread + score at the converged mu (for the sandwich vcov).
    // Same pw*mu weight as the IRLS loop above — bread becomes the pweight-
    // weighted-observed-information inverse (logit's canonical-link precedent:
    // observed info == Fisher info for Poisson too, no BUG-140-style split needed).
    for (int64_t i = 0; i < n; ++i) w[i] = pw ? mu[i] * pw[i] : mu[i];
    std::copy(dm.X.begin(), dm.X.end(), Xd);
    demean(Xd, n, k, md.fe, w.data());
    for (int64_t i = 0; i < n; ++i) sw[i] = std::sqrt(w[i]);
    for (int c = 0; c < k; ++c) {
        const double* xc = Xd + static_cast<size_t>(c) * n;
        double* sc = Xs.data() + static_cast<size_t>(c) * n;
        for (int64_t i = 0; i < n; ++i) sc[i] = xc[i] * sw[i];
    }
    if (k > 0) {
        la::xtx(Xs.data(), n, k, bread.data());
        la::spd_inverse(bread.data(), k);  // bread = (Xd' diag(mu) Xd)^{-1} = Fisher info inverse
    }
    std::vector<double> eP(n);
    for (int64_t i = 0; i < n; ++i) eP[i] = dm.y[i] - mu[i];  // Poisson score residual
    // meat's per-row score under pweight is pw_i*(y_i-mu_i)*x_i — pre-scale eP by
    // the raw pweight (mirrors cmd_logit's e[i]*=wptr[i]) so compute_vcov's
    // existing HC1/Cluster meat = Sum(design*e)(design*e)' comes out pweight-
    // correct with no change to vcov.cpp, the absorbed fit's augmented design
    // included. An iweight's meat is this one too. An fweight row stands for w
    // observations (BUG-1538/1539): the unclustered meat is Sum w_i e_i^2 x_i x_i'
    // (e scaled by sqrt(w)) with N = Sum w in N/(N-1), and a cluster's total is
    // the pweight one (StataBE 19.5, scratch-laneA2/agents/wml/x1.log: `poisson cnt
    // x z [fw=wi], vce(robust)` _se[x] .20397799261707955, vce(cluster cl)
    // .28187708968068442; [iw=wf], vce(robust) .41060909851198435, [pw=wf]'s).
    const bool fw = pw != nullptr && md.weight_kind == WeightKind::Frequency;
    double n_fw = 0;
    if (fw)
        for (int64_t i = 0; i < n; ++i) n_fw += pw[i];
    if (pw)
        for (int64_t i = 0; i < n; ++i) eP[i] *= (fw && vt == Vcov::HC1) ? std::sqrt(pw[i]) : pw[i];

    r.dof = compute_dof(fw ? static_cast<int64_t>(n_fw) : n, k, md.fe);
    const int k_full = k + r.dof.k_fe;
    // Unclustered robust small-sample factor for poisson/ppmlhdfe is Stata's _robust
    // q_c = N/(N-1) — NOT the HC1 regression N/(N-K). Verified exact vs `poisson,
    // vce(robust)` (d3c) and `ppmlhdfe, absorb()` (d3d) pins; N/(N-k_full) was +0.3%
    // (no FE) / +8% (FE) high. compute_vcov's HC1 branch forms n/(n-k_full), so pass
    // k_full=1 there. IID/cluster keep the FE-aware k_full (their conventions unchanged).
    const int k_full_vc = (vt == Vcov::HC1) ? 1 : k_full;
    // BUG-175 (sibling of BUG-164): poisson/ppmlhdfe is a LIKELIHOOD command, so its
    // vce(cluster) multiplier is Stata's pure ML G/(G-1) — NOT the regression CGM
    // G/(G-1)·(N-1)/(N-K) that compute_vcov applies to feols/2SLS. Carrying the
    // regression term inflated every clustered SE by sqrt((N-1)/(N-K)) and deflated
    // the model Wald by (N-1)/(N-K).
    //
    // Measured, not reasoned, against the P2 answerbook `poisson ycount x1 x2
    // i.region, vce(cluster id)` (N=4671, G=200, K=7). Candidate sweep, max relative
    // miss over all seven SEs:
    //     G/(G-1)·(N-1)/(N-K)   6.48e-04   (what this used to emit)
    //     G/(G-1)               4.64e-06   <== the oracle's pick
    //     1                     2.50e-03
    //     G/(G-1)·N/(N-1)       1.12e-04
    //     G/(G-1)·(N-1)/(N-K-1) 7.55e-04
    // The pure factor also brings e(chi2) from 251.8616 to 252.1856, which is Stata's
    // printed 252.19. This was previously scoped to the pweighted path only; that
    // scoping was the bug — the factor is a property of the LIKELIHOOD, not of the
    // weights, and every other ML estimator in this tree (logit/probit/tobit in
    // cmds_mle.cpp, mlogit, oglm, ivml, mixedlm, stcox/streg, nbreg) already used
    // G/(G-1) with its own oracle verification recorded inline. fepois was the lone
    // holdout. Passing the flag rather than hand-rolling the sandwich keeps
    // compute_vcov's nested-FE accounting (nested_fe, cluster_dof_k) intact, which
    // the old pw-only hand-rolled branch discarded.
    const bool ml_cluster = true;  // fepois is ML on every path, weighted or not
    VcovResult vc;
    if (!md.fe.empty()) {
        // ppmlhdfe recovers the constant BEFORE reghdfe's VCE.
        // Under multiway clustering the PSD projection must see that full
        // matrix in the standardized x units (reghdfe.mata reghdfe_standardize),
        // then return to the original units. Clipping the slopes alone changes
        // their variance: w2l_vce_shape_det ppmlhdfe_vce_c2_se is .0077529809719547.
        const int ka = k + 1;
        // The working weight w = pw*mu (w == mu unweighted): the means, the 1/sum
        // corner and the bread above all take it (BUG-1249).
        const double sum_mu = la::sum(w.data(), n);
        std::vector<double> means(static_cast<size_t>(k), 0.0);
        std::vector<double> side(static_cast<size_t>(k), 0.0);
        std::vector<double> scale(static_cast<size_t>(ka), 1.0);
        std::vector<double> Xa(static_cast<size_t>(n) * ka, 1.0);
        std::vector<double> ext(static_cast<size_t>(ka) * ka, 0.0);
        for (int c = 0; c < k; ++c) {
            const double* raw = dm.X.data() + static_cast<int64_t>(c) * n;
            means[static_cast<size_t>(c)] = la::dot(w.data(), raw, n) / sum_mu;
            const double sum = la::sum(raw, n);
            const double variance = (la::sumsq(raw, n) - sum * sum / n) / (n - 1.0);
            scale[static_cast<size_t>(c)] = std::max(1e-3, std::sqrt(std::max(0.0, variance)));
            for (int64_t i = 0; i < n; ++i)
                Xa[static_cast<int64_t>(c) * n + i] = Xd[static_cast<int64_t>(c) * n + i] + means[c];
            for (int row = 0; row < k; ++row)
                ext[static_cast<size_t>(row) + static_cast<size_t>(c) * ka] =
                    bread[static_cast<size_t>(row) + static_cast<size_t>(c) * k];
        }
        if (k > 0) la::matvec(bread.data(), k, means.data(), side.data());
        double corner = 1.0 / sum_mu;
        for (int c = 0; c < k; ++c) {
            corner += means[c] * side[c];
            ext[static_cast<size_t>(c) + static_cast<size_t>(k) * ka] = -side[c];
            ext[static_cast<size_t>(k) + static_cast<size_t>(c) * ka] = -side[c];
        }
        ext[static_cast<size_t>(k) + static_cast<size_t>(k) * ka] = corner;
        if (vt == Vcov::Multiway) {
            // Standardize BEFORE the shared sandwich/whole-matrix projection.
            // Its optional reghdfe clip_scale mode restores the old slope
            // block after projection, which is not ppmlhdfe's convention.
            for (int c = 0; c < ka; ++c) {
                for (int64_t i = 0; i < n; ++i) Xa[static_cast<int64_t>(c) * n + i] /= scale[c];
                for (int row = 0; row < ka; ++row)
                    ext[static_cast<size_t>(row) + static_cast<size_t>(c) * ka] *= scale[row] * scale[c];
            }
        }
        vc = compute_vcov(vt, Xa.data(), n, ka, eP.data(), ext.data(), k_full_vc,
                          1.0, md.fe, md.cluster, cluster_index, fw ? n_fw : -1.0, ml_cluster);
        if (vt == Vcov::Multiway)
            for (int c = 0; c < ka; ++c)
                for (int row = 0; row < ka; ++row)
                    vc.V[static_cast<size_t>(row) + static_cast<size_t>(c) * ka] /= scale[row] * scale[c];
        r.full_vcov_matrix = vc.V;
        r.cons_se = std::sqrt(std::max(0.0, vc.V[static_cast<size_t>(k) * ka + k]));
        r.cons_se_valid = true;
        vc.V.assign(static_cast<size_t>(k) * k, 0.0);
        for (int c = 0; c < k; ++c)
            for (int row = 0; row < k; ++row)
                vc.V[static_cast<size_t>(row) + static_cast<size_t>(c) * k] =
                    r.full_vcov_matrix[static_cast<size_t>(row) + static_cast<size_t>(c) * ka];
    } else if (k > 0) {
        vc = compute_vcov(vt, Xd, n, k, eP.data(), bread.data(), k_full_vc, 1.0, md.fe,
                          md.cluster, cluster_index, fw ? n_fw : -1.0, ml_cluster);
    } else {
        // No coefficient block: nothing to sandwich (BLAS refuses a 0-wide matrix).
        // Only the labels the reply carries are filled.
        vc.type = vt == Vcov::IID ? "iid" : "HC1";
        if (vt == Vcov::Cluster || vt == Vcov::Multiway) {
            if (md.cluster.empty()) throw EngineError("bad_input", "no cluster variable provided");
            std::vector<const Factor*> cls;
            if (vt == Vcov::Cluster) cls.push_back(&md.cluster.at(static_cast<size_t>(cluster_index)));
            else for (const Factor& c : md.cluster) cls.push_back(&c);
            int g = cls[0]->n_levels;
            for (const Factor* c : cls) g = std::min(g, c->n_levels);
            vc.n_clusters = g;
            // compute_vcov's spellings: cluster(a) / twoway(a,b) / multiway(a,b,c).
            vc.type = cls.size() == 1 ? "cluster(" : (cls.size() == 2 ? "twoway(" : "multiway(");
            for (size_t d = 0; d < cls.size(); ++d) vc.type += (d ? "," : "") + cls[d]->name;
            vc.type += ")";
        }
    }
    r.vcov_type = vc.type;
    r.n_clusters = vc.n_clusters;
    r.cluster_dof_k = vc.cluster_dof_k;
    r.nested_fe = vc.nested_fe;
    r.se.resize(k);
    for (int j = 0; j < k; ++j) r.se[j] = std::sqrt(vc.V[j + j * k]);
    r.vcov_matrix = std::move(vc.V);

    // --- Log-likelihood family: e(ll), e(ll_0), e(r2_p), e(chi2)/e(df_m) ------------
    // Computed DIRECTLY from the converged μ rather than backed out of the deviance,
    // so the deviance identity  ll == ll_saturated − deviance/2  is an independent
    // cross-check (pinned to ~1e-6 relative by tests/unit/test_fepois_ll.cpp) instead
    // of a tautology. lgamma(y+1) supplies ln(y!) for the non-integer-safe general case.
    {
        double ll = 0, lg_sum = 0, sy = 0, s_expoff = 0;
        for (int64_t i = 0; i < n; ++i) {
            const double yi = dm.y[i];
            const double wgt = pw ? pw[i] : 1.0;
            const double lgy = std::lgamma(yi + 1.0);
            ll += wgt * (yi * std::log(mu[i]) - mu[i] - lgy);
            lg_sum += wgt * lgy;
            sy += wgt * yi;
            s_expoff += wgt * (off ? std::exp(off[i]) : 1.0);
        }
        r.ll = ll;
        r.ll_valid = std::isfinite(ll);

        // Null model: μ_i = exp(α + offset_i) with α the ML solution of the single
        // score equation Σ w_i (y_i − exp(α+off_i)) = 0  ⇒  e^α = Σw·y / Σw·e^off.
        // With no offset this is just the (weighted) mean of y — Stata's `poisson`
        // null, which does NOT include any absorbed fixed effect.
        if (sy > 0 && s_expoff > 0) {
            const double a = std::log(sy / s_expoff);
            double ll0 = 0;
            for (int64_t i = 0; i < n; ++i) {
                const double wgt = pw ? pw[i] : 1.0;
                const double eta0 = a + (off ? off[i] : 0.0);
                ll0 += wgt * (dm.y[i] * eta0 - std::exp(eta0));
            }
            ll0 -= lg_sum;
            r.ll_0 = ll0;
            r.r2_p = (ll0 != 0.0) ? 1.0 - ll / ll0 : 0.0;
        } else {
            r.ll_valid = false;
        }

        // Non-intercept coefficient block. Pooled fits carry build_model's
        // "(intercept)" column; under absorb() there is no intercept column at all
        // (the FE absorb it and _cons is recovered separately below), so every
        // column is a slope there.
        std::vector<int> slope_idx;
        for (int j = 0; j < k; ++j)
            if (r.names[static_cast<size_t>(j)] != "(intercept)") slope_idx.push_back(j);
        const int ks = static_cast<int>(slope_idx.size());
        r.chi2_df = ks;
        if (ks > 0) {
            if (vt == Vcov::IID) {
                // iid ⇒ the fit IS the MLE and Stata reports the LR test.
                r.chi2 = 2.0 * (ll - r.ll_0);
                r.chi2_is_wald = false;
            } else if (static_cast<int>(r.vcov_matrix.size()) == k * k) {
                // robust/cluster ⇒ Wald quadratic form on the reported vcov.
                std::vector<double> Vs(static_cast<size_t>(ks) * ks), bs(static_cast<size_t>(ks));
                for (int a2 = 0; a2 < ks; ++a2) {
                    bs[static_cast<size_t>(a2)] = r.beta[static_cast<size_t>(slope_idx[a2])];
                    for (int b2 = 0; b2 < ks; ++b2)
                        Vs[static_cast<size_t>(a2) + static_cast<size_t>(b2) * ks] =
                            r.vcov_matrix[static_cast<size_t>(slope_idx[a2]) +
                                          static_cast<size_t>(slope_idx[b2]) * k];
                }
                if (la::spd_inverse(Vs.data(), ks) == 0) {
                    std::vector<double> tv(static_cast<size_t>(ks));
                    la::matvec(Vs.data(), ks, bs.data(), tv.data());
                    double c2 = 0;
                    for (int a2 = 0; a2 < ks; ++a2)
                        c2 += bs[static_cast<size_t>(a2)] * tv[static_cast<size_t>(a2)];
                    r.chi2 = c2;
                    r.chi2_is_wald = true;
                }
            }
        }
    }

    // ppmlhdfe-style _cons recovery under absorbed FE. ppmlhdfe reports a constant even
    // with absorb(): it is the μ-weighted mean of the total absorbed effect per obs,
    // a_i = η_i − offset_i − X_i·β  (η_i = log μ_i). General over FE dims. [d3d pin]
    if (!md.fe.empty()) {
        // The weights are the last regression's working weights (mu BEFORE the final
        // update) and the index the updated eta: ppmlhdfe's _cons. Measured on
        // w2d_uncovered.log `ppmlhdfe cnt, absorb(t)` (4 iterations): _cons
        // 2.091719879995292 = sum w_last*eta / sum w_last; the converged-mu mean
        // is 2.09171987618406, 1.8e-9 away.
        double Sw = 0, csum = 0;
        for (int64_t i = 0; i < n; ++i) Sw += w_last[i];
        for (int64_t i = 0; i < n; ++i) {
            double xb = 0;
            for (int c = 0; c < k; ++c) xb += dm.X[static_cast<size_t>(c) * n + i] * beta[c];
            double o = off ? off[i] : 0.0;
            csum += w_last[i] * (eta[i] - o - xb);
        }
        r.cons_b = csum / Sw;
        r.has_cons = true;
        if (fe_sum_out) {
            // ppmlhdfe.mata stores d = z - resid - x*b, demeaned with the working
            // weights: eta - offset - x'b less the _cons just recovered.
            fe_sum_out->assign(static_cast<size_t>(n), 0.0);
            for (int64_t i = 0; i < n; ++i) {
                double xb = 0;
                for (int c = 0; c < k; ++c) xb += dm.X[static_cast<size_t>(c) * n + i] * beta[c];
                const double o = off ? off[i] : 0.0;
                (*fe_sum_out)[static_cast<size_t>(i)] = eta[i] - o - xb - r.cons_b;
            }
        }
    }
    return r;
}

}  // namespace cecon
