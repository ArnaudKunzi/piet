// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "inference/wildboot.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/linalg.h"
#include "core/rng.h"
#include "core/rng_normal.h"
#include "core/threads.h"
#include "estimators/iv2sls.h"
#include "estimators/logit.h"
#include "estimators/glm.h"
#include "core/error.h"
#include "hdfe/demean.h"
#include "hdfe/dof.h"
#include "vcov/vcov.h"

namespace cecon {

int wildboot_fanout_threads(int reps, int G) {
    const int64_t g = static_cast<int64_t>(G);
    return threads_for_work(static_cast<int64_t>(reps) * g * g, g * 8 + 4096);
}

// Stata's Rademacher weight table (StataWildDraws): G rows x (B+1) columns of
// +-1, drawn row by row; entry [rank*(B+1) + b + 1] is cluster `rank`'s weight in
// replicate b.
static std::vector<double> stata_rademacher_table(StataWildDraws& sd, int G, int B) {
    if (static_cast<int>(sd.cluster_rank.size()) != G)
        throw std::runtime_error("wildboot: Stata draw plan does not match the clusters");
    const size_t W = static_cast<size_t>(B) + 1;
    std::vector<double> t(static_cast<size_t>(G) * W);
    for (size_t i = 0; i < t.size(); ++i) t[i] = sd.stream.runiform() >= 0.5 ? 1.0 : -1.0;
    sd.taken = static_cast<uint64_t>(t.size());
    return t;
}

const char* boot_weights_name(BootWeights w) {
    switch (w) {
        case BootWeights::Webb: return "webb";
        case BootWeights::Mammen: return "mammen";
        case BootWeights::Normal: return "normal";
        case BootWeights::Gamma: return "gamma";
        case BootWeights::Rademacher: break;
    }
    return "rademacher";
}

bool boot_weights_from_name(const std::string& name, BootWeights& w) {
    for (BootWeights c : {BootWeights::Rademacher, BootWeights::Webb, BootWeights::Mammen,
                          BootWeights::Normal, BootWeights::Gamma})
        if (name == boot_weights_name(c)) {
            w = c;
            return true;
        }
    return false;
}

// Cluster g's weight in replicate b, a Philox draw of type w. Rademacher and Webb are
// core/rng.h's. The other three are boottest's (boottest.mata MakeWildWeights,
// BUG-1519), each of mean 0 and variance 1 -- the scale the WRE kernel's y* = y-bar +
// v u needs, and one the t* of the other kernels does not read:
//  - Mammen: (1 - sqrt5)/2 with probability (sqrt5 + 1)/(2 sqrt5), (1 + sqrt5)/2
//    otherwise (third moment 1);
//  - normal: the standard normal, the inverse normal CDF of one 53-bit uniform;
//  - gamma: a gamma(shape 4, scale 1/2) draw less its mean 2 (third moment 1); the
//    shape is whole, so the draw is a sum of four exponentials of mean 1/2.
// Like Webb's these are not Stata's draws (boottest takes them from rdiscrete(),
// rnormal() and rgamma(), whose algorithms are not identified here): the p agrees
// with Stata's to the bootstrap's own noise (TOLERANCE.md T5). One exception, few
// clusters under Mammen: a draw giving every cluster the low value (probability
// .7236^G) makes |t*| = |t| exactly, which the strict count settles by rounding --
// boottest's and this kernel's each their own way -- so p can differ from Stata's
// by up to that probability (StataBE 19.5, 8 clusters: by .075, either sign).
static double boot_weight(BootWeights w, uint64_t seed, uint64_t b, uint64_t g) {
    static const double kSqrt5 = std::sqrt(5.0);
    switch (w) {
        case BootWeights::Webb: return webb6(seed, b, g);
        case BootWeights::Mammen:
            return uniform01(seed, b, g) < (kSqrt5 + 1.0) / (2.0 * kSqrt5) ? (1.0 - kSqrt5) / 2.0
                                                                          : (1.0 + kSqrt5) / 2.0;
        case BootWeights::Normal: return normal_icdf(uniform01(seed, b, g));
        case BootWeights::Gamma: {
            double s = 0;
            for (uint64_t k = 0; k < 4; ++k) s -= 0.5 * std::log(uniform01(seed, b | (k << 32), g));
            return s - 2.0;
        }
        case BootWeights::Rademacher: break;
    }
    return rademacher(seed, b, g);
}

// Shared fast WCR bootstrap-t kernel. The per-replicate t* is, for both OLS and
// 2SLS, a function only of the per-cluster pieces:
//   numerator   N(v)  = sum_g v_g c_g,         c_g = a' s~_g
//   se*(v)^2    = ssf * sum_g d_g^2,           d_g = v_g c_g - sum_h v_h W[g,h]
// where s~_g are the score rows of the RESTRICTED residuals (using the score
// design Sdesign = X̃ for OLS, X̂ for 2SLS), a = row j of bread = (Sdesign'Sdesign)^-1,
// W[g,h] = a' B_g bread s~_h, and B_g = sum_{i in g} Sdesign_i Xactual_i'.
// (For OLS Sdesign==Xactual so B_g is symmetric; for 2SLS B_g mixes X̂ and X.)
// All arrays are already demeaned by `fe`; when an FE is not nested in the clusters
// the per-replicate refit re-absorbs it, which folds one more G x G term into W.
static WildBootResult wcb_core(const Factor& cl, const std::vector<Factor>& fe, int64_t n,
                               int k, const double* Sdesign,
                               const double* Xactual, const double* bread,
                               const double* eUnrestricted, const double* uR, int j,
                               double r, double ssf, double beta_j, int reps,
                               uint64_t seed, BootWeights weights,
                               const std::string& paramname,
                               StataWildDraws* stata_draws = nullptr) {
    const int G = cl.n_levels;
    if (G < 2) throw std::runtime_error("need >= 2 clusters");

    std::vector<double> a(k);
    for (int c = 0; c < k; ++c) a[c] = bread[j + static_cast<size_t>(c) * k];

    // t0 from unrestricted residuals
    std::vector<double> Se(static_cast<size_t>(G) * k);
    cluster_scores(Sdesign, n, k, eUnrestricted, cl, Se.data());
    double se2_0 = 0;
    for (int g = 0; g < G; ++g) {
        double eg = 0;
        for (int c = 0; c < k; ++c) eg += Se[g + static_cast<size_t>(c) * G] * a[c];
        se2_0 += eg * eg;
    }
    se2_0 *= ssf;
    const double se0 = std::sqrt(se2_0);
    const double t0 = (beta_j - r) / se0;

    // restricted per-cluster scores s~_g (score design), c_g = a . s~_g
    std::vector<double> St(static_cast<size_t>(G) * k);
    cluster_scores(Sdesign, n, k, uR, cl, St.data());
    std::vector<double> cg(G);
    for (int g = 0; g < G; ++g) {
        double s = 0;
        for (int c = 0; c < k; ++c) s += St[g + static_cast<size_t>(c) * G] * a[c];
        cg[g] = s;
    }

    // B_g = sum_{i in g} Sdesign_i Xactual_i'  (k x k per cluster, may be asymmetric)
    std::vector<double> B(static_cast<size_t>(G) * k * k, 0.0);
    const int32_t* gc = cl.codes.data();
    for (int64_t i = 0; i < n; ++i) {
        double* Bg = B.data() + static_cast<size_t>(gc[i]) * k * k;
        for (int c1 = 0; c1 < k; ++c1) {
            double si = Sdesign[static_cast<size_t>(c1) * n + i];
            for (int c2 = 0; c2 < k; ++c2)
                Bg[c1 + c2 * k] += si * Xactual[static_cast<size_t>(c2) * n + i];
        }
    }
    // w_g = bread * (a' B_g)';  W[g,h] = w_g . s~_h
    std::vector<double> wmat(static_cast<size_t>(G) * k);
    {
        std::vector<double> u(k), wg(k);
        for (int g = 0; g < G; ++g) {
            const double* Bg = B.data() + static_cast<size_t>(g) * k * k;
            for (int c2 = 0; c2 < k; ++c2) {
                double s = 0;
                for (int c1 = 0; c1 < k; ++c1) s += a[c1] * Bg[c1 + c2 * k];  // (a'B_g)[c2]
                u[c2] = s;
            }
            la::matvec(bread, k, u.data(), wg.data());
            for (int c = 0; c < k; ++c) wmat[g + static_cast<size_t>(c) * G] = wg[c];
        }
    }
    // High-end memory guard: W below is a dense G x G matrix of doubles, so its
    // memory is O(G^2). Cap G to keep W within a ~512MB budget: G^2 * 8B <= 512MB
    // (decimal MB = 1e6 B) => G <= sqrt(512e6 / 8) = 8000. A streaming variant that
    // never materializes the full G x G W (folding the g,h contraction into the
    // per-replicate loop instead) would lift this cap; parked as a future item,
    // not implemented here.
    constexpr int kMaxWildbootClusters = 8000;
    if (G > kMaxWildbootClusters) {
        long long mb_needed = std::llround(static_cast<double>(G) * static_cast<double>(G) *
                                           8.0 / 1e6);
        throw std::runtime_error(
            "wildboot: G=" + std::to_string(G) + " clusters would need " +
            std::to_string(mb_needed) + " MB for the GxG weight matrix (ceiling ~" +
            std::to_string(kMaxWildbootClusters) + "); reduce clusters or use a non-wildboot vce");
    }
    std::vector<double> W(static_cast<size_t>(G) * G);
    la::gemm_nt(wmat.data(), St.data(), W.data(), G, k, G);

    // The replicate's refit absorbs the FE again, so its residual is
    //   e*(v) = M_D(v o uR) - Xactual (beta* - betaR),  M_D = the FE annihilator,
    // and the score term v_g c_g above is really a' Sdesign_g' [M_D(v o uR)]_g. The two
    // agree when every FE is nested in the clusters (uR sums to zero inside each FE
    // group, so M_D leaves v o uR alone). Otherwise, with u_h = uR on cluster h and 0
    // elsewhere, a' Sdesign_g' [M_D(v o uR)]_g = sum_h v_h Wfe[g,h],
    // Wfe[g,h] = sum_{i in g} (a' Sdesign_i) [M_D u_h]_i, and folding
    // W[g,h] += delta_gh c_g - Wfe[g,h] keeps the kernel's d_g formula. Without it the
    // p is not the refit's: w2_esample_tsrefit.log:457 W5 (reghdfe absorb(id), 7
    // clusters in cl that cut across id) is 40/128 by refit and 28/128 without.
    bool fe_cuts_clusters = false;
    for (const Factor& f : fe)
        if (!fe_nested_in_cluster(f, cl)) fe_cuts_clusters = true;
    if (fe_cuts_clusters) {
        std::vector<double> ax(static_cast<size_t>(n), 0.0);
        for (int c = 0; c < k; ++c) {
            const double* col = Sdesign + static_cast<size_t>(c) * n;
            for (int64_t i = 0; i < n; ++i) ax[i] += a[c] * col[i];
        }
        std::vector<double> Wfe(static_cast<size_t>(G) * G, 0.0);
        // u_h columns demeaned in blocks of at most ~64 MB.
        const int blk = static_cast<int>(
            std::max<int64_t>(1, std::min<int64_t>(G, 8000000 / std::max<int64_t>(n, 1))));
        std::vector<double> U(static_cast<size_t>(n) * blk);
        for (int h0 = 0; h0 < G; h0 += blk) {
            const int m = std::min(blk, G - h0);
            std::fill(U.begin(), U.begin() + static_cast<size_t>(n) * m, 0.0);
            for (int64_t i = 0; i < n; ++i) {
                const int h = gc[i];
                if (h >= h0 && h < h0 + m) U[static_cast<size_t>(h - h0) * n + i] = uR[i];
            }
            demean(U.data(), n, m, fe);
            for (int hh = 0; hh < m; ++hh) {
                const double* col = U.data() + static_cast<size_t>(hh) * n;
                double* Wcol = Wfe.data() + static_cast<size_t>(h0 + hh) * G;
                for (int64_t i = 0; i < n; ++i) Wcol[gc[i]] += ax[i] * col[i];
            }
        }
        for (int h = 0; h < G; ++h)
            for (int g = 0; g < G; ++g) {
                const size_t gh = g + static_cast<size_t>(h) * G;
                W[gh] += (g == h ? cg[g] : 0.0) - Wfe[gh];
            }
    }

    // Weights and draw set follow boottest 4.5.3 (the version that wrote the
    // oracles; sha256 in internal note boottest-family/T5-GRID.md):
    // - boottest makes the weights Rademacher for any number of clusters;
    //   Webb is used only when it is asked for (there is no small-G auto-switch).
    // - boottest's enumeration rule is `enumerate = (B & auxwttype==0 & Nstar*ln(2) < ln(B)+1e-6)`:
    //   with Rademacher weights and 2^G not above reps, every one of the 2^G sign
    //   vectors is run once (count_binary) and the replications are reduced
    //   to 2^G, which boottest reports. No other weight type enumerates.
    //   (G <= 30 keeps 2^G an int: G = 31 would need reps within 2^-31 of 2^31.)
    const bool rad = (weights == BootWeights::Rademacher);
    const bool enumerate = rad && G <= 30 &&
        static_cast<double>(G) * std::log(2.0) < std::log(static_cast<double>(reps)) + 1e-6;
    const int Beff = enumerate ? (1 << G) : reps;  // boottest's B after the reduction

    // t*(v) for one weight vector; the draws and the comparison reference below
    // both go through this one expression, so they share their arithmetic.
    auto tstar_of = [&](const double* v) {
        double N = 0;
        for (int g = 0; g < G; ++g) N += v[g] * cg[g];
        double se2 = 0;
        for (int g = 0; g < G; ++g) {
            double wv = 0;
            for (int h = 0; h < G; ++h) wv += W[g + static_cast<size_t>(h) * G] * v[h];
            double d = v[g] * cg[g] - wv;
            se2 += d * d;
        }
        se2 *= ssf;
        return (se2 > 0) ? N / std::sqrt(se2) : 0.0;
    };

    std::vector<double> tstars(static_cast<size_t>(Beff));
    std::vector<double> stab;
    const bool stata_stream = stata_draws && rad && !enumerate;
    if (stata_draws) stata_draws->taken = 0;
    if (stata_stream) stab = stata_rademacher_table(*stata_draws, G, Beff);
    // Fan-out sized by the settings-derived envelope (core + memory budget), see
    // wildboot_fanout_threads, over the effective B; each replicate writes only tstars[b].
    const int wildboot_threads = wildboot_fanout_threads(Beff, G);
    parallel_for(Beff, [&](int64_t b) {
        std::vector<double> v(G);
        if (enumerate) {
            // count_binary order (as in boottest): cluster 0 is the most
            // significant digit, -1 before +1; b = 0 is all -1, b = 2^G-1 all +1.
            for (int g = 0; g < G; ++g) v[g] = ((b >> (G - 1 - g)) & 1) ? 1.0 : -1.0;
        } else if (stata_stream) {
            const size_t W = static_cast<size_t>(Beff) + 1;
            for (int g = 0; g < G; ++g)
                v[g] = stab[static_cast<size_t>(stata_draws->cluster_rank[g]) * W +
                            static_cast<size_t>(b) + 1];
        } else {
            for (int g = 0; g < G; ++g) v[g] = boot_weight(weights, seed, b, g);
        }
        tstars[b] = tstar_of(v.data());
    }, wildboot_threads);

    // p-values: boottest's getp() over the Beff draws (BFeas = Beff).
    // The reference is the full-sample statistic dist[1], which boottest computes as
    // the v = 1 column in the same pass as the draws. t_ref = t*(+1)
    // here, from the same kernel: the all +1 draw (y* = y) then equals it bit for
    // bit and the all -1 draw is its exact negation, so the strict |t*| count drops
    // both and the equal-tail counts drop the all +1 draw (the all -1 one lies strictly
    // on the far side of t_ref, as in boottest). t0 (unrestricted residuals) stays the
    // reported statistic.
    const std::vector<double> ones(G, 1.0);
    const double t_ref = tstar_of(ones.data());
    const double at_ref = std::fabs(t_ref);
    long sym = 0, below = 0, above = 0;
    for (int b = 0; b < Beff; ++b) {
        const double t = tstars[b];
        if (std::fabs(t) > at_ref) ++sym;  // rowsum(-abs(tmp) :> -abs(*pDist))
        if (t < t_ref) ++below;            // rowsum(tmp :> *pDist)
        if (t > t_ref) ++above;            // rowsum(-tmp :> -*pDist)
    }
    WildBootResult R;
    R.reps = Beff;
    R.weights = boot_weights_name(weights);
    R.param = paramname;
    R.null_beta = r;
    R.beta = beta_j;
    R.se = se0;
    R.t0 = t0;
    R.t_ref = t_ref;
    R.p_symmetric = static_cast<double>(sym) / Beff;
    R.p_equaltail = 2.0 * static_cast<double>(std::min(below, above)) / Beff;
    R.n_clusters = G;
    R.seed = seed;
    R.webb_fallback = false;  // kept for the JSON contract; there is no auto-switch
    R.tstars = std::move(tstars);
    return R;
}

// Small-sample factor matching compute_vcov one-way cluster, given a fit's dof.
static double cluster_ssf(double n, int k, const std::vector<Factor>& fe,
                          const Factor& cl, int k_fe) {
    int k_adj = k + k_fe;
    for (const Factor& f : fe)
        if (fe_nested_in_cluster(f, cl)) k_adj -= (f.n_levels - 1);
    const int G = cl.n_levels;
    return (static_cast<double>(G) / (G - 1)) *
           (static_cast<double>(n - 1) / static_cast<double>(n - k_adj));
}

WildBootResult wild_cluster_bootstrap(ModelData& md, int j, double r, int reps,
                                      uint64_t seed, int cluster_index,
                                      BootWeights weights, StataWildDraws* stata_draws) {
    DesignMatrix& dm = md.dm;
    const int64_t n = dm.n;
    const int k = dm.k;
    if (j < 0 || j >= k) throw std::runtime_error("bad param index");
    if (md.cluster.empty())
        throw std::runtime_error("wild cluster bootstrap requires a cluster variable");
    const Factor& cl = md.cluster.at(cluster_index);

    // WLS is OLS on sqrt(weight)-scaled rows. Scores on this design are
    // w*x*e, so the same restricted-fit/bootstrap algebra applies. Weighted
    // absorption needs weighted FE projections and stays explicitly refused.
    double effective_n = static_cast<double>(n);
    if (!md.weights.empty()) {
        if (!md.fe.empty()) throw std::runtime_error("wildboot: weighted fixed effects are not supported");
        double sum_w = 0.0;
        for (double w : md.weights) sum_w += w;
        if (!(sum_w > 0.0)) throw std::runtime_error("wildboot: sum of weights must be positive");
        const bool normalize = md.weight_kind == WeightKind::Analytic ||
                               md.weight_kind == WeightKind::Probability ||
                               md.weight_kind == WeightKind::Importance;
        const double scale = normalize ? static_cast<double>(n) / sum_w : 1.0;
        effective_n = normalize ? static_cast<double>(n) : sum_w;
        for (int64_t i = 0; i < n; ++i) {
            const double root = std::sqrt(md.weights[static_cast<size_t>(i)] * scale);
            dm.y[i] *= root;
            for (int c = 0; c < k; ++c) dm.X[static_cast<size_t>(c) * n + i] *= root;
        }
    }
    if (!(effective_n > k)) throw std::runtime_error("wildboot: insufficient residual degrees of freedom");
    demean(dm.y.data(), n, 1, md.fe);
    demean(dm.X.data(), n, k, md.fe);

    std::vector<double> bread(static_cast<size_t>(k) * k);
    la::xtx(dm.X.data(), n, k, bread.data());
    if (la::spd_inverse(bread.data(), k) != 0) throw std::runtime_error("X'X singular");
    std::vector<double> Xty(k), beta(k);
    la::xtv(dm.X.data(), n, k, dm.y.data(), Xty.data());
    la::matvec(bread.data(), k, Xty.data(), beta.data());
    std::vector<double> e(n);
    la::residual(dm.X.data(), n, k, beta.data(), dm.y.data(), e.data());

    DofInfo dof = compute_dof(n, k, md.fe);
    double ssf = cluster_ssf(effective_n, k, md.fe, cl, dof.k_fe);

    // restricted fit: regress (y - r*X_j) on X_{-j}
    const int km = k - 1;
    std::vector<double> betaR(k);
    betaR[j] = r;
    if (km > 0) {
        std::vector<double> Xm(static_cast<size_t>(n) * km);
        int cc = 0;
        for (int c = 0; c < k; ++c) {
            if (c == j) continue;
            std::copy(dm.X.data() + static_cast<size_t>(c) * n,
                      dm.X.data() + static_cast<size_t>(c) * n + n,
                      Xm.data() + static_cast<size_t>(cc) * n);
            ++cc;
        }
        std::vector<double> yr(n);
        const double* Xj = dm.X.data() + static_cast<size_t>(j) * n;
        for (int64_t i = 0; i < n; ++i) yr[i] = dm.y[i] - r * Xj[i];
        std::vector<double> breadR(static_cast<size_t>(km) * km), Xmty(km), betaMj(km);
        la::xtx(Xm.data(), n, km, breadR.data());
        if (la::spd_inverse(breadR.data(), km) != 0)
            throw std::runtime_error("restricted X'X singular");
        la::xtv(Xm.data(), n, km, yr.data(), Xmty.data());
        la::matvec(breadR.data(), km, Xmty.data(), betaMj.data());
        cc = 0;
        for (int c = 0; c < k; ++c)
            if (c != j) betaR[c] = betaMj[cc++];
    }
    std::vector<double> uR(n);
    la::residual(dm.X.data(), n, k, betaR.data(), dm.y.data(), uR.data());

    return wcb_core(cl, md.fe, n, k, dm.X.data(), dm.X.data(), bread.data(), e.data(), uR.data(),
                    j, r, ssf, beta[j], reps, seed, weights, dm.xnames[j], stata_draws);
}

WildBootResult wild_cluster_bootstrap_iv(IvModelData& md, double r, int reps,
                                         uint64_t seed, int cluster_index,
                                         BootWeights weights) {
    if (md.kendog != 1)
        throw std::runtime_error("IV wild bootstrap currently supports exactly one endogenous regressor");
    if (md.cluster.empty())
        throw std::runtime_error("IV wild bootstrap requires a cluster variable");
    const Factor& cl = md.cluster.at(cluster_index);
    const int64_t n = md.n;
    const int kexog = md.kexog, kx = kexog + 1, kw = kexog + md.kinst;
    const int jendog = kexog;  // the endogenous coef is last; it is the tested param

    // demean everything
    demean(md.y.data(), n, 1, md.fe);
    demean(md.Xexog.data(), n, kexog, md.fe);
    demean(md.Xendog.data(), n, md.kendog, md.fe);
    demean(md.Zinst.data(), n, md.kinst, md.fe);

    // W=[exog,inst]; first stage Xhat=[exog, P_W endog]; X=[exog,endog]
    std::vector<double> W(static_cast<size_t>(n) * kw);
    for (int j = 0; j < kexog; ++j)
        std::copy(md.exog(j), md.exog(j) + n, W.data() + static_cast<int64_t>(j) * n);
    for (int j = 0; j < md.kinst; ++j)
        std::copy(md.inst(j), md.inst(j) + n, W.data() + static_cast<int64_t>(kexog + j) * n);
    std::vector<double> breadW(static_cast<size_t>(kw) * kw);
    la::xtx(W.data(), n, kw, breadW.data());
    if (la::spd_inverse(breadW.data(), kw) != 0) throw std::runtime_error("W'W singular");
    std::vector<double> Xhat(static_cast<size_t>(n) * kx), X(static_cast<size_t>(n) * kx);
    for (int j = 0; j < kexog; ++j) {
        std::copy(md.exog(j), md.exog(j) + n, Xhat.data() + static_cast<int64_t>(j) * n);
        std::copy(md.exog(j), md.exog(j) + n, X.data() + static_cast<int64_t>(j) * n);
    }
    std::copy(md.endog(0), md.endog(0) + n, X.data() + static_cast<int64_t>(kexog) * n);
    {
        std::vector<double> Wtv(kw), gamma(kw);
        la::xtv(W.data(), n, kw, md.endog(0), Wtv.data());
        la::matvec(breadW.data(), kw, Wtv.data(), gamma.data());
        la::mat_vec(W.data(), n, kw, gamma.data(), Xhat.data() + static_cast<int64_t>(kexog) * n);
    }

    std::vector<double> bread(static_cast<size_t>(kx) * kx);
    la::xtx(Xhat.data(), n, kx, bread.data());
    if (la::spd_inverse(bread.data(), kx) != 0) throw std::runtime_error("Xhat'Xhat singular");
    std::vector<double> Xhaty(kx), beta(kx);
    la::xtv(Xhat.data(), n, kx, md.y.data(), Xhaty.data());
    la::matvec(bread.data(), kx, Xhaty.data(), beta.data());
    std::vector<double> e(n);
    la::residual(X.data(), n, kx, beta.data(), md.y.data(), e.data());

    DofInfo dof = compute_dof(n, kx, md.fe);
    double ssf = cluster_ssf(n, kx, md.fe, cl, dof.k_fe);

    // restricted fit (single endog = r): regress (y - r*endog) on exog
    std::vector<double> betaR(kx);
    betaR[jendog] = r;
    if (kexog > 0) {
        std::vector<double> yr(n);
        const double* en = md.endog(0);
        for (int64_t i = 0; i < n; ++i) yr[i] = md.y[i] - r * en[i];
        std::vector<double> breadE(static_cast<size_t>(kexog) * kexog), Ety(kexog), be(kexog);
        la::xtx(md.Xexog.data(), n, kexog, breadE.data());
        if (la::spd_inverse(breadE.data(), kexog) != 0)
            throw std::runtime_error("restricted exog X'X singular");
        la::xtv(md.Xexog.data(), n, kexog, yr.data(), Ety.data());
        la::matvec(breadE.data(), kexog, Ety.data(), be.data());
        for (int c = 0; c < kexog; ++c) betaR[c] = be[c];
    }
    std::vector<double> uR(n);
    la::residual(X.data(), n, kx, betaR.data(), md.y.data(), uR.data());

    std::string pname = md.endog_names.empty() ? "endog" : md.endog_names[0];
    // score design = Xhat (instrumented); actual = X
    return wcb_core(cl, md.fe, n, kx, Xhat.data(), X.data(), bread.data(), e.data(), uR.data(),
                    jendog, r, ssf, beta[jendog], reps, seed, weights, pname);
}

// boottest.mata _MakeInterpolables / MakeNumerAndJ: the ML score bootstrap
// evaluates the FULL information at the null-constrained optimum. The cluster
// score is recentered by cluster sample share in every draw; neither an OLS
// residual bootstrap nor the unrestricted Wald statistic gives this test.
WildBootResult wild_score_bootstrap(const std::vector<double>& X, int64_t n, int k,
                                    const std::vector<double>& y, const Factor& cl,
                                    int j, const std::string& param, const std::string& model,
                                    double null_beta, int reps, uint64_t seed, BootWeights weights,
                                    StataWildDraws* stata_draws) {
    const bool logit = model == "logit", probit = model == "probit";
    const bool poisson = model == "poisson" || model == "glm-poisson";
    const bool gaussian = model == "glm-gaussian";
    if ((!logit && !probit && !poisson && !gaussian) || null_beta != 0.0)
        throw EngineError("unsupported", "score bootstrap supports zero-null logit/probit/Poisson/Gaussian GLM only");
    if (n <= k || k < 2 || j < 0 || j >= k - 1 || reps < 1 ||
        X.size() != static_cast<size_t>(n) * k || y.size() != static_cast<size_t>(n) ||
        cl.codes.size() != static_cast<size_t>(n) || cl.n_levels < 2)
        throw EngineError("bad_input", "score bootstrap: invalid design, parameter, replications or clusters");
    // This public kernel is intentionally constant-last and single-slope-null.
    for (int64_t i = 0; i < n; ++i) {
        if (X[static_cast<size_t>(k - 1) * n + i] != 1.0 || !std::isfinite(y[i]) ||
            cl.codes[i] < 0 || cl.codes[i] >= cl.n_levels ||
            ((logit || probit) && y[i] != 0.0 && y[i] != 1.0) || (poisson && y[i] < 0.0))
            throw EngineError("bad_input", "score bootstrap: invalid outcome, cluster or intercept");
        for (int c = 0; c < k; ++c)
            if (!std::isfinite(X[static_cast<size_t>(c) * n + i]))
                throw EngineError("bad_input", "score bootstrap: nonfinite design value");
    }
    const int kr = k - 1;
    std::vector<double> reduced(static_cast<size_t>(n) * kr);
    for (int c = 0, d = 0; c < k; ++c) {
        if (c == j) continue;
        std::copy_n(X.data() + static_cast<size_t>(c) * n, n,
                    reduced.data() + static_cast<size_t>(d++) * n);
    }
    std::vector<double> br;
    if (logit || probit) {
        const auto fit = binary_ml_fit(logit ? BinaryLink::Logit : BinaryLink::Probit,
            reduced.data(), n, kr, y.data(), nullptr, nullptr, true, MlMaxOptions{});
        if (!fit.converged || fit.singular || fit.n_cdf || fit.n_cds)
            throw EngineError("unsupported", "score bootstrap: constrained binary fit did not converge on the complete original sample");
        br = fit.beta;
    } else {
        GlmOptions opts;
        opts.family = poisson ? GlmFamily::Poisson : GlmFamily::Gaussian;
        opts.link = poisson ? GlmLinkFn::Log : GlmLinkFn::Identity;
        // Poisson's native fit uses a tighter optimum; glm retains its own
        // default optimizer convention. Neither uses the unrestricted fit.
        if (model == "poisson") { opts.tol = 1e-12; opts.ltol = 1e-14; opts.nrtol = 1e-12; }
        const auto fit = glm_fit(reduced.data(), n, kr, y.data(), nullptr, nullptr, nullptr, opts);
        if (!fit.ok || !fit.converged)
            throw EngineError("unsupported", "score bootstrap: constrained GLM did not converge");
        br = fit.beta;
    }
    std::vector<double> eta(static_cast<size_t>(n));
    la::mat_vec(reduced.data(), n, kr, br.data(), eta.data());
    std::vector<double> score(static_cast<size_t>(n)), info(static_cast<size_t>(k) * k, 0.0);
    constexpr double root_two = 1.4142135623730950488016887242097;
    constexpr double inv_root_two_pi = 0.39894228040143267793994605993438;
    for (int64_t i = 0; i < n; ++i) {
        double w;
        if (logit) {
            const double p = eta[i] >= 0 ? 1.0 / (1.0 + std::exp(-eta[i]))
                                         : std::exp(eta[i]) / (1.0 + std::exp(eta[i]));
            score[i] = y[i] - p; w = p * (1.0 - p);
        } else if (probit) {
            const double sign = y[i] == 1.0 ? 1.0 : -1.0;
            const double t = sign * eta[i];
            const double probability = 0.5 * std::erfc(-t / root_two);
            if (!(probability > 0.0) || t > 6.0)
                throw EngineError("unsupported", "score bootstrap: constrained probit changes completely-determined row status");
            const double mills = inv_root_two_pi * std::exp(-0.5 * t * t) / probability;
            score[i] = sign * mills;
            w = mills * (mills + t);  // observed, not Fisher, information
        } else if (poisson) {
            const double mu = std::exp(eta[i]); score[i] = y[i] - mu; w = mu;
        } else { score[i] = y[i] - eta[i]; w = 1.0; }
        if (!std::isfinite(score[i]) || !std::isfinite(w) || !(w > 0.0))
            throw EngineError("unsupported", "score bootstrap: nonfinite or degenerate constrained information");
        for (int a = 0; a < k; ++a)
            for (int b = 0; b < k; ++b)
                info[a + static_cast<size_t>(b) * k] +=
                    X[static_cast<size_t>(a) * n + i] * X[static_cast<size_t>(b) * n + i] * w;
    }
    if (la::spd_inverse(info.data(), k) != 0)
        throw EngineError("unsupported", "score bootstrap: full constrained information is not positive definite");
    const int G = cl.n_levels;
    std::vector<double> q(G, 0.0), share(G, 0.0);
    for (int64_t i = 0; i < n; ++i) {
        double projected = 0.0;
        for (int a = 0; a < k; ++a)
            projected += X[static_cast<size_t>(a) * n + i] * info[a + static_cast<size_t>(j) * k];
        q[cl.codes[i]] += projected * score[i];
        share[cl.codes[i]] += 1.0 / static_cast<double>(n);
    }
    auto statistic = [&](const std::vector<double>& v) {
        double u = 0.0, variance = 0.0;
        for (int g = 0; g < G; ++g) u += q[g] * v[g];
        for (int g = 0; g < G; ++g) {
            const double residual = q[g] * v[g] - share[g] * u;
            variance += residual * residual;
        }
        if (!(variance > 0.0) || !std::isfinite(variance))
            return std::numeric_limits<double>::quiet_NaN();
        return u / std::sqrt(variance);
    };
    const bool rad = weights == BootWeights::Rademacher;
    const bool enumerate = rad && G <= 30 &&
        G * std::log(2.0) < std::log(static_cast<double>(reps)) + 1e-6;
    const int B = enumerate ? (1 << G) : reps;
    const double z = statistic(std::vector<double>(G, 1.0));
    if (!std::isfinite(z)) throw EngineError("unsupported", "score bootstrap: degenerate score variance");
    std::vector<double> draws(static_cast<size_t>(B));
    std::vector<double> stab;
    const bool stata_stream = stata_draws && rad && !enumerate;
    if (stata_draws) stata_draws->taken = 0;
    if (stata_stream) stab = stata_rademacher_table(*stata_draws, G, B);
    parallel_for(B, [&](int64_t b) {
        std::vector<double> v(G);
        for (int g = 0; g < G; ++g)
            v[g] = enumerate ? (((b >> (G - 1 - g)) & 1) ? 1.0 : -1.0)
                 : stata_stream ? stab[static_cast<size_t>(stata_draws->cluster_rank[g]) *
                                           (static_cast<size_t>(B) + 1) +
                                       static_cast<size_t>(b) + 1]
                 : boot_weight(weights, seed, b, g);
        draws[b] = statistic(v);
    }, wildboot_fanout_threads(B, G));
    int sym = 0, below = 0, above = 0;
    for (double t : draws) {
        if (!std::isfinite(t)) throw EngineError("unsupported", "score bootstrap: degenerate resampling draw");
        if (std::fabs(t) > std::fabs(z)) ++sym;
        if (t < z) ++below;
        if (t > z) ++above;
    }
    WildBootResult out;
    out.param = param; out.null_beta = 0.0; out.beta = std::numeric_limits<double>::quiet_NaN();
    out.t0 = z; out.t_ref = z; out.se = std::numeric_limits<double>::quiet_NaN(); out.n_clusters = G;
    out.reps = B; out.seed = seed; out.weights = boot_weights_name(weights);
    out.p_symmetric = static_cast<double>(sym) / B;
    out.p_equaltail = 2.0 * std::min(below, above) / B;
    out.tstars = std::move(draws);
    return out;
}

// ===========================================================================
// Wild restricted efficient (WRE) bootstrap and the multiway wild bootstrap-t,
// by direct refit of every replicate (boottest 4.5.3, plus/b/boottest.mata).
// ===========================================================================
namespace {

// One error clustering: the intersection of a non-empty subset of the cluster
// variables (boottest's Init over combs()). sign is + for an
// odd number of variables and - for an even number (Clust[c].even, _clustAccum); mult = N_c/(N_c-1) under `small`, else 1.
struct ErrClustering {
    std::vector<int32_t> codes;
    int n_levels = 0;
    double sign = 1.0, mult = 1.0;
};

// Dense codes of the intersection of `fs` (ascending by the tuple of codes).
std::vector<int32_t> intersect_factors(const std::vector<const Factor*>& fs, int64_t n,
                                       int& n_levels) {
    std::vector<int32_t> cur(static_cast<size_t>(n), 0);
    int lev = 1;
    for (const Factor* f : fs) {
        std::vector<int64_t> key(static_cast<size_t>(n));
        for (int64_t i = 0; i < n; ++i)
            key[i] = static_cast<int64_t>(cur[i]) * f->n_levels + f->codes[static_cast<size_t>(i)];
        std::vector<int64_t> u = key;
        std::sort(u.begin(), u.end());
        u.erase(std::unique(u.begin(), u.end()), u.end());
        for (int64_t i = 0; i < n; ++i)
            cur[i] = static_cast<int32_t>(std::lower_bound(u.begin(), u.end(), key[i]) - u.begin());
        lev = static_cast<int>(u.size());
    }
    n_levels = lev;
    return cur;
}

// k-class fit pieces that do not change across replicates: the included
// exogenous regressors Xin (each a column of the instrument matrix W) and W.
struct KclassDesign {
    int64_t n = 0;
    int kin = 0, kw = 0;
    const double* Xin = nullptr;  // n x kin
    const double* W = nullptr;    // n x kw
    std::vector<int> xin_col;     // Xin column c == W column xin_col[c]
    std::vector<double> WtWi, XinXin, XinXinInv;
};

KclassDesign make_kdesign(int64_t n, const double* Xin, int kin, const double* W, int kw,
                          std::vector<int> xin_col) {
    KclassDesign d;
    d.n = n; d.kin = kin; d.kw = kw; d.Xin = Xin; d.W = W; d.xin_col = std::move(xin_col);
    d.WtWi.assign(static_cast<size_t>(kw) * kw, 0.0);
    if (kw > 0) {
        la::xtx(W, n, kw, d.WtWi.data());
        if (la::spd_inverse(d.WtWi.data(), kw) != 0)
            throw EngineError("unsupported", "wild bootstrap: the instrument cross-product is singular");
    }
    d.XinXin.assign(static_cast<size_t>(kin) * kin, 0.0);
    if (kin > 0) la::xtx(Xin, n, kin, d.XinXin.data());
    d.XinXinInv = d.XinXin;
    if (kin > 0 && la::spd_inverse(d.XinXinInv.data(), kin) != 0)
        throw EngineError("unsupported", "wild bootstrap: the exogenous regressors are collinear");
    return d;
}

struct KclassFit {
    bool ok = false;
    double kappa = 1.0;
    std::vector<double> beta;  // [Xin..., Y2...]
    std::vector<double> Hinv;  // k x k
    std::vector<double> Pi;    // kw x (1+p): (W'W)^-1 W'[y, Y2]
};

// The k-class estimator of y on [Xin, Y2] with instruments W:
//   beta = {(1-k) X'X + k X'P_W X}^-1 {(1-k) X'y + k X'P_W y},
// with k = 1 (2SLS; OLS when p = 0) or, for LIML, the smallest root of
// |Y~'M_Xin Y~ - k Y~'M_W Y~| = 0, Y~ = [y, Y2] (ivregress liml; boottest's
// Estimate and MakeWREStats solve the same eigenproblem).
KclassFit kclass_fit(const KclassDesign& d, const double* y, const double* Y2, int p, bool liml) {
    KclassFit f;
    const int64_t n = d.n;
    const int q = 1 + p, kin = d.kin, kw = d.kw, k = kin + p;
    std::vector<double> Yt(static_cast<size_t>(n) * q);
    std::copy(y, y + n, Yt.begin());
    if (p > 0) std::copy(Y2, Y2 + static_cast<size_t>(n) * p, Yt.begin() + n);
    std::vector<double> YY(static_cast<size_t>(q) * q), WtY(static_cast<size_t>(kw) * q, 0.0),
        XinY(static_cast<size_t>(kin) * q, 0.0);
    la::xtx(Yt.data(), n, q, YY.data());
    for (int c = 0; c < q; ++c) {
        const double* col = Yt.data() + static_cast<size_t>(c) * n;
        if (kw > 0) la::xtv(d.W, n, kw, col, WtY.data() + static_cast<size_t>(c) * kw);
        if (kin > 0) la::xtv(d.Xin, n, kin, col, XinY.data() + static_cast<size_t>(c) * kin);
    }
    f.Pi.assign(static_cast<size_t>(kw) * q, 0.0);
    for (int c = 0; c < q; ++c)
        for (int r = 0; r < kw; ++r) {
            double s = 0;
            for (int m = 0; m < kw; ++m)
                s += d.WtWi[r + static_cast<size_t>(m) * kw] * WtY[m + static_cast<size_t>(c) * kw];
            f.Pi[r + static_cast<size_t>(c) * kw] = s;
        }
    std::vector<double> YPY(static_cast<size_t>(q) * q, 0.0);  // Y~'P_W Y~
    for (int a = 0; a < q; ++a)
        for (int b = 0; b < q; ++b) {
            double s = 0;
            for (int m = 0; m < kw; ++m)
                s += WtY[m + static_cast<size_t>(a) * kw] * f.Pi[m + static_cast<size_t>(b) * kw];
            YPY[a + static_cast<size_t>(b) * q] = s;
        }
    double kappa = 1.0;
    if (liml) {
        // A = Y~'M_Xin Y~, B = Y~'M_W Y~; kappa = min eig of B^-1/2 A B^-1/2.
        std::vector<double> A = YY, B(static_cast<size_t>(q) * q);
        for (int a = 0; a < q; ++a)
            for (int b = 0; b < q; ++b) {
                double s = 0;
                for (int r = 0; r < kin; ++r)
                    for (int m = 0; m < kin; ++m)
                        s += XinY[r + static_cast<size_t>(a) * kin] *
                             d.XinXinInv[r + static_cast<size_t>(m) * kin] *
                             XinY[m + static_cast<size_t>(b) * kin];
                A[a + static_cast<size_t>(b) * q] -= s;
                B[a + static_cast<size_t>(b) * q] = YY[a + static_cast<size_t>(b) * q] -
                                                     YPY[a + static_cast<size_t>(b) * q];
            }
        std::vector<double> wB(q);
        if (la::sym_eig(B.data(), q, wB.data()) != 0) return f;
        std::vector<double> Bih(static_cast<size_t>(q) * q, 0.0);
        for (int j = 0; j < q; ++j) {
            if (!(wB[j] > 0)) return f;
            const double s = 1.0 / std::sqrt(wB[j]);
            const double* vj = B.data() + static_cast<size_t>(j) * q;
            for (int c = 0; c < q; ++c)
                for (int r = 0; r < q; ++r) Bih[r + static_cast<size_t>(c) * q] += s * vj[r] * vj[c];
        }
        std::vector<double> T(static_cast<size_t>(q) * q, 0.0), S(static_cast<size_t>(q) * q, 0.0);
        for (int a = 0; a < q; ++a)
            for (int b = 0; b < q; ++b) {
                double s = 0;
                for (int c = 0; c < q; ++c) s += Bih[a + static_cast<size_t>(c) * q] * A[c + static_cast<size_t>(b) * q];
                T[a + static_cast<size_t>(b) * q] = s;
            }
        for (int a = 0; a < q; ++a)
            for (int b = 0; b < q; ++b) {
                double s = 0;
                for (int c = 0; c < q; ++c) s += T[a + static_cast<size_t>(c) * q] * Bih[c + static_cast<size_t>(b) * q];
                S[a + static_cast<size_t>(b) * q] = s;
            }
        std::vector<double> wS(q);
        if (la::sym_eig(S.data(), q, wS.data()) != 0) return f;
        kappa = wS[0];
    }
    if (k == 0) {  // a restricted model with no regressors left: its residual is y itself
        f.kappa = kappa;
        f.ok = true;
        return f;
    }
    // X'X, X'P_W X differ only in the endogenous block (P_W Xin = Xin).
    std::vector<double> H(static_cast<size_t>(k) * k), rhs(k);
    auto XX = [&](int a, int b, bool proj) -> double {
        if (a < kin && b < kin) return d.XinXin[a + static_cast<size_t>(b) * kin];
        if (a < kin) return XinY[a + static_cast<size_t>(1 + b - kin) * kin];
        if (b < kin) return XinY[b + static_cast<size_t>(1 + a - kin) * kin];
        const std::vector<double>& M = proj ? YPY : YY;
        return M[(1 + a - kin) + static_cast<size_t>(1 + b - kin) * q];
    };
    auto Xy = [&](int a, bool proj) -> double {
        if (a < kin) return XinY[a];
        const std::vector<double>& M = proj ? YPY : YY;
        return M[1 + a - kin];
    };
    for (int a = 0; a < k; ++a) {
        for (int b = 0; b < k; ++b)
            H[a + static_cast<size_t>(b) * k] =
                kappa == 1.0 ? XX(a, b, true) : (1 - kappa) * XX(a, b, false) + kappa * XX(a, b, true);
        rhs[a] = kappa == 1.0 ? Xy(a, true) : (1 - kappa) * Xy(a, false) + kappa * Xy(a, true);
    }
    if (la::spd_inverse(H.data(), k) != 0) return f;
    f.beta.assign(k, 0.0);
    la::matvec(H.data(), k, rhs.data(), f.beta.data());
    f.Hinv = std::move(H);
    f.kappa = kappa;
    f.ok = true;
    return f;
}

// Cluster-robust Wald statistic for coefficient j of a k-class fit, with
// boottest's sandwich: score rows (P_W X)_i u_i (boottest's Filling; the same sandwich V*ZPX'*ZPZi*Omega*ZPZi*ZPX*V as ivregress liml), summed over the
// error clusterings with their signs and multipliers (_clustAccum).
double kclass_wald(const KclassDesign& d, const KclassFit& f, const double* y, const double* Y2,
                   int p, int j, double r, const std::vector<ErrClustering>& cls,
                   double smallsample, double* se_out) {
    const int64_t n = d.n;
    const int kin = d.kin, kw = d.kw, k = kin + p;
    const double* a = f.Hinv.data() + static_cast<size_t>(j) * k;  // column j (symmetric)
    std::vector<double> c(kw, 0.0);
    for (int e = 0; e < kin; ++e) c[d.xin_col[e]] += a[e];
    for (int m = 0; m < p; ++m)
        for (int w = 0; w < kw; ++w) c[w] += f.Pi[w + static_cast<size_t>(1 + m) * kw] * a[kin + m];
    std::vector<double> q(static_cast<size_t>(n));
    for (int64_t i = 0; i < n; ++i) {
        double fit = 0;
        for (int e = 0; e < kin; ++e) fit += d.Xin[i + static_cast<size_t>(e) * n] * f.beta[e];
        for (int m = 0; m < p; ++m) fit += Y2[i + static_cast<size_t>(m) * n] * f.beta[kin + m];
        double xa = 0;
        for (int w = 0; w < kw; ++w) xa += d.W[i + static_cast<size_t>(w) * n] * c[w];
        q[i] = xa * (y[i] - fit);
    }
    double V = 0;
    std::vector<double> S;
    for (const ErrClustering& ec : cls) {
        S.assign(static_cast<size_t>(ec.n_levels), 0.0);
        for (int64_t i = 0; i < n; ++i) S[ec.codes[static_cast<size_t>(i)]] += q[i];
        double ss = 0;
        for (double s : S) ss += s * s;
        V += ec.sign * ec.mult * ss;
    }
    if (!(V > 0) || !std::isfinite(V)) {
        if (se_out) *se_out = std::numeric_limits<double>::quiet_NaN();
        return std::numeric_limits<double>::quiet_NaN();
    }
    // t = numer / sqrt(denom) * sqrt(smallsample) (boottest's multiplier).
    if (se_out) *se_out = std::sqrt(V / smallsample);
    return (f.beta[j] - r) / std::sqrt(V) * std::sqrt(smallsample);
}

}  // namespace

WildBootResult wild_restricted_efficient_bootstrap(
    const std::vector<double>& y, int64_t n, const std::vector<double>& Xexog, int kx,
    const std::vector<std::string>& exog_names, const std::vector<double>& Y2, int p,
    const std::vector<std::string>& endog_names, const std::vector<double>& Zexcl, int kz,
    const std::vector<Factor>& clusters, const WreSpec& spec) {
    const int k = kx + p;
    const int j = spec.param_index;
    if (j < 0 || j >= k) throw EngineError("bad_input", "wild bootstrap: bad parameter index");
    if (clusters.empty()) throw EngineError("bad_input", "wild bootstrap requires a cluster variable");
    if (p > 0 && kz < p) throw EngineError("unsupported", "wild bootstrap: the model is underidentified");
    if (spec.reps < 1) throw EngineError("bad_input", "wild bootstrap: reps must be >= 1");
    bool liml = spec.liml && p > 0;
    if (liml && kz == p) liml = false;  // exactly identified LIML is 2SLS (as in boottest)

    // Instruments W = [Xexog, Zexcl]; the exogenous regressors are its first kx columns.
    const int kw = kx + kz;
    std::vector<double> W(static_cast<size_t>(n) * kw);
    std::copy(Xexog.begin(), Xexog.end(), W.begin());
    std::copy(Zexcl.begin(), Zexcl.end(), W.begin() + static_cast<size_t>(n) * kx);
    std::vector<int> full_col(kx);
    for (int c = 0; c < kx; ++c) full_col[c] = c;
    const KclassDesign full = make_kdesign(n, Xexog.data(), kx, W.data(), kw, full_col);

    // Error clusterings and the bootstrap clusters (per boottest: the
    // bootstrap clustering defaults to all the cluster variables, i.e. their
    // intersection).
    const int ncv = static_cast<int>(clusters.size());
    if (ncv > 8) throw EngineError("unsupported", "wild bootstrap: at most 8 cluster variables");
    std::vector<ErrClustering> cls;
    int minN = std::numeric_limits<int>::max();
    for (int mask = (1 << ncv) - 1; mask >= 1; --mask) {
        std::vector<const Factor*> fs;
        for (int v = 0; v < ncv; ++v)
            if (mask & (1 << v)) fs.push_back(&clusters[static_cast<size_t>(v)]);
        ErrClustering ec;
        ec.codes = intersect_factors(fs, n, ec.n_levels);
        ec.sign = (fs.size() % 2 == 1) ? 1.0 : -1.0;
        ec.mult = spec.small ? static_cast<double>(ec.n_levels) / (ec.n_levels - 1) : 1.0;
        if (ec.n_levels < 2) throw EngineError("unsupported", "wild bootstrap: need at least 2 clusters");
        minN = std::min(minN, ec.n_levels);
        cls.push_back(std::move(ec));
    }
    const std::vector<int32_t>& boot = cls.front().codes;  // mask = all variables
    const int G = cls.front().n_levels;

    // boottest: _df = N - kZ - FEdfadj, smallsample = _df/(N - 1).
    const double dfres = static_cast<double>(n) - k - spec.fe_dof_adj;
    if (spec.small && !(dfres > 0))
        throw EngineError("unsupported", "wild bootstrap: no residual degrees of freedom");
    const double smallsample = spec.small ? dfres / (static_cast<double>(n) - 1.0) : 1.0;

    // The fit's own statistic.
    const KclassFit f0 = kclass_fit(full, y.data(), Y2.data(), p, liml);
    if (!f0.ok) throw EngineError("unsupported", "wild bootstrap: the model fit failed (singular design)");
    double se0 = 0;
    const double t0 = kclass_wald(full, f0, y.data(), Y2.data(), p, j, spec.null_beta, cls,
                                  smallsample, &se0);

    // DGP (as in boottest): the null-restricted model, always LIML
    // (DGP.LIML = 1). The tested regressor leaves the regressors; an exogenous
    // one stays among the instruments.
    const double* xj = j < kx ? Xexog.data() + static_cast<size_t>(j) * n
                              : Y2.data() + static_cast<size_t>(j - kx) * n;
    std::vector<double> y1par(static_cast<size_t>(n));
    for (int64_t i = 0; i < n; ++i) y1par[i] = y[i] - spec.null_beta * xj[i];
    const int kxr = j < kx ? kx - 1 : kx, pr = j < kx ? p : p - 1;
    std::vector<double> Xr(static_cast<size_t>(n) * kxr), Y2r(static_cast<size_t>(n) * pr);
    std::vector<int> r_col;
    for (int c = 0, d = 0; c < kx; ++c) {
        if (c == j) continue;
        std::copy_n(Xexog.data() + static_cast<size_t>(c) * n, n, Xr.data() + static_cast<size_t>(d++) * n);
        r_col.push_back(c);
    }
    for (int m = 0, d = 0; m < p; ++m) {
        if (kx + m == j) continue;
        std::copy_n(Y2.data() + static_cast<size_t>(m) * n, n, Y2r.data() + static_cast<size_t>(d++) * n);
    }
    const KclassDesign rdes = make_kdesign(n, Xr.data(), kxr, W.data(), kw, r_col);
    const KclassFit fr = kclass_fit(rdes, y1par.data(), Y2r.data(), pr, pr > 0);
    if (!fr.ok) throw EngineError("unsupported", "wild bootstrap: the null-restricted fit failed");
    std::vector<double> delta(k, 0.0);  // restricted coefficients on the full regressor list
    for (int c = 0, d = 0; c < kx; ++c) delta[c] = (c == j) ? spec.null_beta : fr.beta[d++];
    for (int m = 0, d = 0; m < p; ++m)
        delta[kx + m] = (kx + m == j) ? spec.null_beta : fr.beta[kxr + d++];
    std::vector<double> u1(static_cast<size_t>(n));  // u1ddot: restricted structural residual
    for (int64_t i = 0; i < n; ++i) {
        double fit = 0;
        for (int c = 0; c < kx; ++c) fit += Xexog[i + static_cast<size_t>(c) * n] * delta[c];
        for (int m = 0; m < p; ++m) fit += Y2[i + static_cast<size_t>(m) * n] * delta[kx + m];
        u1[i] = y[i] - fit;
    }
    // Efficient first stage (boottest's MakeResiduals): Y2 on W controlling for u1ddot.
    std::vector<double> Y2bar(static_cast<size_t>(n) * p), U2(static_cast<size_t>(n) * p);
    if (p > 0) {
        const int kwu = kw + 1;
        std::vector<double> WU(static_cast<size_t>(n) * kwu);
        std::copy(W.begin(), W.end(), WU.begin());
        std::copy(u1.begin(), u1.end(), WU.begin() + static_cast<size_t>(n) * kw);
        std::vector<double> G2(static_cast<size_t>(kwu) * kwu);
        la::xtx(WU.data(), n, kwu, G2.data());
        if (la::spd_inverse(G2.data(), kwu) != 0)
            throw EngineError("unsupported", "wild bootstrap: the efficient first stage is singular");
        std::vector<double> WUy(kwu), coef(kwu);
        for (int m = 0; m < p; ++m) {
            const double* ym = Y2.data() + static_cast<size_t>(m) * n;
            la::xtv(WU.data(), n, kwu, ym, WUy.data());
            la::matvec(G2.data(), kwu, WUy.data(), coef.data());
            for (int64_t i = 0; i < n; ++i) {
                double s = 0;
                for (int w = 0; w < kw; ++w) s += W[i + static_cast<size_t>(w) * n] * coef[w];
                Y2bar[i + static_cast<size_t>(m) * n] = s;
                U2[i + static_cast<size_t>(m) * n] = ym[i] - s;
            }
        }
    }
    // y1bar = X1 delta_X + Y2bar delta_Y; u1dddot = u1ddot + U2ddot delta_Y.
    std::vector<double> y1bar(static_cast<size_t>(n)), u1d(static_cast<size_t>(n));
    for (int64_t i = 0; i < n; ++i) {
        double fit = 0, add = 0;
        for (int c = 0; c < kx; ++c) fit += Xexog[i + static_cast<size_t>(c) * n] * delta[c];
        for (int m = 0; m < p; ++m) {
            fit += Y2bar[i + static_cast<size_t>(m) * n] * delta[kx + m];
            add += U2[i + static_cast<size_t>(m) * n] * delta[kx + m];
        }
        y1bar[i] = fit;
        u1d[i] = u1[i] + add;
    }

    // Draws: boottest enumerates the 2^G Rademacher set when it is not
    // larger than reps (as wcb_core); otherwise Rademacher +-1 (WRE convention) or the other types at variance 1 (boot_weight).
    const bool rad = (spec.weights == BootWeights::Rademacher);
    const bool enumerate = rad && G <= 30 &&
        static_cast<double>(G) * std::log(2.0) < std::log(static_cast<double>(spec.reps)) + 1e-6;
    const int Beff = enumerate ? (1 << G) : spec.reps;
    auto replicate_t = [&](const std::vector<double>& vg) {
        std::vector<double> ys(static_cast<size_t>(n)), Ys(static_cast<size_t>(n) * p);
        for (int64_t i = 0; i < n; ++i) {
            const double v = vg[static_cast<size_t>(boot[static_cast<size_t>(i)])];
            ys[i] = y1bar[i] + v * u1d[i];
            for (int m = 0; m < p; ++m) {
                const size_t o = i + static_cast<size_t>(m) * n;
                Ys[o] = Y2bar[o] + v * U2[o];
            }
        }
        const KclassFit fb = kclass_fit(full, ys.data(), Ys.data(), p, liml);
        if (!fb.ok) return std::numeric_limits<double>::quiet_NaN();
        return kclass_wald(full, fb, ys.data(), Ys.data(), p, j, spec.null_beta, cls, smallsample,
                           nullptr);
    };
    std::vector<double> tstars(static_cast<size_t>(Beff));
    parallel_for(Beff, [&](int64_t b) {
        std::vector<double> v(static_cast<size_t>(G));
        for (int g = 0; g < G; ++g) {
            if (enumerate) v[g] = ((b >> (G - 1 - g)) & 1) ? 1.0 : -1.0;
            else v[g] = boot_weight(spec.weights, spec.seed, b, g);
        }
        tstars[static_cast<size_t>(b)] = replicate_t(v);
    }, threads_for_work(static_cast<int64_t>(Beff) * n * (kw + 2),
                        n * 8 * static_cast<int64_t>(p + 4) + 4096));

    // getp (boottest's) against Dist[1], the v = 1 column of the same
    // machinery; missing draws are left out of the count and of BFeas.
    const double t_ref = replicate_t(std::vector<double>(static_cast<size_t>(G), 1.0));
    long sym = 0, below = 0, above = 0, feas = 0;
    for (double t : tstars) {
        if (!std::isfinite(t)) continue;
        ++feas;
        if (std::fabs(t) > std::fabs(t_ref)) ++sym;
        if (t < t_ref) ++below;
        if (t > t_ref) ++above;
    }
    WildBootResult R;
    R.reps = Beff;
    R.weights = boot_weights_name(spec.weights);
    R.param = j < kx ? exog_names.at(static_cast<size_t>(j)) : endog_names.at(static_cast<size_t>(j - kx));
    R.null_beta = spec.null_beta;
    R.beta = f0.beta[static_cast<size_t>(j)];
    R.se = se0;
    R.t0 = t0;
    R.t_ref = t_ref;
    const bool pok = std::isfinite(t_ref) && feas > 0;
    R.p_symmetric = pok ? static_cast<double>(sym) / feas : std::numeric_limits<double>::quiet_NaN();
    R.p_equaltail = pok ? 2.0 * static_cast<double>(std::min(below, above)) / feas
                        : std::numeric_limits<double>::quiet_NaN();
    R.n_clusters = G;
    R.seed = spec.seed;
    R.df_r = static_cast<double>(minN) - 1.0;
    R.kappa = f0.kappa;
    R.tstars = std::move(tstars);
    return R;
}

}  // namespace cecon
