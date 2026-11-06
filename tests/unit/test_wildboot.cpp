// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Ring-1: the fast WCR bootstrap t* must equal a brute-force refit t* for every
// replicate (validates the O(G^2) fast-algorithm derivation), and be deterministic.
// The draw set and the p-value follow boottest 4.5.3 (arms R1-R4 below; source
// lines cited at src/inference/wildboot.cpp wcb_core).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/linalg.h"
#include "core/model.h"
#include "core/rng.h"
#include "inference/wildboot.h"

using namespace cecon;

static int g_fail = 0;

// Brute-force WCR bootstrap t for one weight vector vg (one weight per cluster;
// no FE; pooled OLS): build y*, refit, cluster SE.
static double brute_tstar(const std::vector<double>& y, const std::vector<double>& x,
                          const std::vector<int>& g, int G, int j, double r,
                          const std::vector<double>& vg, double ssf) {
    const int n = static_cast<int>(y.size());
    const int k = 2;  // intercept + x
    // design X = [1, x]
    std::vector<double> X(static_cast<size_t>(n) * k);
    for (int i = 0; i < n; ++i) { X[i] = 1.0; X[n + i] = x[i]; }
    std::vector<double> bread(k * k);
    la::xtx(X.data(), n, k, bread.data());
    la::spd_inverse(bread.data(), k);
    std::vector<double> a(k);
    for (int c = 0; c < k; ++c) a[c] = bread[j + c * k];

    // restricted fit: regress (y - r*x) on intercept -> betaR0 = mean(y - r*x)
    double betaR0 = 0;
    for (int i = 0; i < n; ++i) betaR0 += y[i] - r * x[i];
    betaR0 /= n;
    std::vector<double> yhatR(n), uR(n);
    for (int i = 0; i < n; ++i) {
        yhatR[i] = betaR0 + r * x[i];
        uR[i] = y[i] - yhatR[i];
    }
    // y* and refit
    std::vector<double> ystar(n);
    for (int i = 0; i < n; ++i) ystar[i] = yhatR[i] + vg[g[i]] * uR[i];
    std::vector<double> Xty(k), beta(k);
    la::xtv(X.data(), n, k, ystar.data(), Xty.data());
    la::matvec(bread.data(), k, Xty.data(), beta.data());
    std::vector<double> e(n);
    la::residual(X.data(), n, k, beta.data(), ystar.data(), e.data());
    // cluster se of beta_j
    std::vector<double> sg(G, 0.0), sgx(G, 0.0);
    for (int i = 0; i < n; ++i) { sg[g[i]] += e[i]; sgx[g[i]] += x[i] * e[i]; }
    double se2 = 0;
    for (int gg = 0; gg < G; ++gg) {
        double agsg = a[0] * sg[gg] + a[1] * sgx[gg];
        se2 += agsg * agsg;
    }
    se2 *= ssf;
    return (beta[j] - r) / std::sqrt(se2);
}

// Brute-force WCR bootstrap t for one weight vector vg of a 2SLS model (intercept
// + 1 endogenous d instrumented by z).
static double brute_tstar_iv(const std::vector<double>& y, const std::vector<double>& d,
                             const std::vector<double>& z, const std::vector<int>& g, int G,
                             double r, const std::vector<double>& vg, double ssf) {
    const int n = static_cast<int>(y.size()), kx = 2, kw = 2;
    std::vector<double> W(n * kw);
    for (int i = 0; i < n; ++i) { W[i] = 1.0; W[n + i] = z[i]; }
    std::vector<double> breadW(kw * kw);
    la::xtx(W.data(), n, kw, breadW.data());
    la::spd_inverse(breadW.data(), kw);
    std::vector<double> Wtd(kw), gam(kw), dhat(n);
    la::xtv(W.data(), n, kw, d.data(), Wtd.data());
    la::matvec(breadW.data(), kw, Wtd.data(), gam.data());
    la::mat_vec(W.data(), n, kw, gam.data(), dhat.data());
    std::vector<double> Xhat(n * kx), X(n * kx);
    for (int i = 0; i < n; ++i) { Xhat[i] = 1; Xhat[n + i] = dhat[i]; X[i] = 1; X[n + i] = d[i]; }
    std::vector<double> bread(kx * kx);
    la::xtx(Xhat.data(), n, kx, bread.data());
    la::spd_inverse(bread.data(), kx);
    double a0 = bread[1 + 0 * kx], a1 = bread[1 + 1 * kx];  // row j=1
    double b0R = 0;
    for (int i = 0; i < n; ++i) b0R += y[i] - r * d[i];
    b0R /= n;
    std::vector<double> ys(n);
    for (int i = 0; i < n; ++i)
        ys[i] = b0R + r * d[i] + vg[g[i]] * (y[i] - b0R - r * d[i]);
    std::vector<double> Xty(kx), beta(kx), e(n);
    la::xtv(Xhat.data(), n, kx, ys.data(), Xty.data());
    la::matvec(bread.data(), kx, Xty.data(), beta.data());
    la::residual(X.data(), n, kx, beta.data(), ys.data(), e.data());
    std::vector<double> s0(G, 0), s1(G, 0);
    for (int i = 0; i < n; ++i) { s0[g[i]] += Xhat[i] * e[i]; s1[g[i]] += Xhat[n + i] * e[i]; }
    double se2 = 0;
    for (int gg = 0; gg < G; ++gg) { double as = a0 * s0[gg] + a1 * s1[gg]; se2 += as * as; }
    se2 *= ssf;
    return (beta[1] - r) / std::sqrt(se2);
}

int test_wildboot_main() {
    std::printf("test_wildboot:\n");
    // tiny pooled data, 4 clusters x 3 obs
    std::vector<double> x = {1, 2, 3, 2, 4, 1, 5, 2, 3, 1, 4, 2};
    std::vector<double> y = {2, 3, 5, 3, 6, 2, 8, 3, 5, 2, 7, 3};
    std::vector<int> gcodes = {0, 0, 0, 1, 1, 1, 2, 2, 2, 3, 3, 3};
    const int n = 12, G = 4;

    ModelData md;
    md.has_intercept = true;
    md.dm.n = n;
    md.dm.k = 2;
    md.dm.y = y;
    md.dm.X.resize(static_cast<size_t>(n) * 2);
    for (int i = 0; i < n; ++i) { md.dm.X[i] = 1.0; md.dm.X[n + i] = x[i]; }
    md.dm.xnames = {"(intercept)", "x"};
    Factor cl;
    cl.name = "g";
    cl.n_levels = G;
    cl.codes.assign(gcodes.begin(), gcodes.end());
    cl.count = {3, 3, 3, 3};
    md.cluster = {cl};

    // Random draws: G=4 enumerates at reps >= 16 (4 ln2 < ln16 + 1e-6), so reps 15
    // is the largest random run; it checks the Philox draws b = 0..14.
    const int reps = 200;
    const int reps_rand = 15;
    const uint64_t seed = 123;
    auto rad_draw = [&](int64_t b) {
        std::vector<double> vg(G);
        for (int gg = 0; gg < G; ++gg) vg[gg] = rademacher(seed, b, gg);
        return vg;
    };
    // count_binary order (matches boottest's behaviour, identified from its source): cluster 0 is the most significant
    // digit and -1 comes first, so b = 0 is all -1 and b = 2^G - 1 is all +1.
    auto enum_draw = [](int64_t b, int GG) {
        std::vector<double> vg(GG);
        for (int gg = 0; gg < GG; ++gg) vg[gg] = ((b >> (GG - 1 - gg)) & 1) ? 1.0 : -1.0;
        return vg;
    };
    ModelData mdr = md;
    WildBootResult Rr = wild_cluster_bootstrap(mdr, /*j=*/1, /*r=*/0.0, reps_rand, seed, 0,
                                               BootWeights::Rademacher);

    // ssf the fast path used: G/(G-1)*(N-1)/(N-k_adj), no FE so k_adj=k=2
    double ssf = (static_cast<double>(G) / (G - 1)) * (static_cast<double>(n - 1) / (n - 2));
    double maxerr = 0;
    bool reps_rand_ok = (Rr.reps == reps_rand && static_cast<int>(Rr.tstars.size()) == reps_rand);
    for (int b = 0; reps_rand_ok && b < reps_rand; ++b) {
        double bt = brute_tstar(y, x, gcodes, G, 1, 0.0, rad_draw(b), ssf);
        maxerr = std::max(maxerr, std::fabs(bt - Rr.tstars[b]));
    }
    std::printf("  fast-vs-brute max |dt*| over %d random reps = %.3e\n", reps_rand, maxerr);
    if (!reps_rand_ok || maxerr > 1e-9) {
        std::printf("  FAIL equivalence (random, reps %d)\n", Rr.reps);
        ++g_fail;
    } else std::printf("  ok   fast == brute-force (random draws)\n");

    // Enumerated: reps 200 over G=4 runs the 16 sign vectors once each.
    WildBootResult R = wild_cluster_bootstrap(md, /*j=*/1, /*r=*/0.0, reps, seed, 0,
                                              BootWeights::Rademacher);
    maxerr = 0;
    bool reps_enum_ok = (R.reps == 16 && R.tstars.size() == 16);
    for (int b = 0; reps_enum_ok && b < 16; ++b) {
        double bt = brute_tstar(y, x, gcodes, G, 1, 0.0, enum_draw(b, G), ssf);
        maxerr = std::max(maxerr, std::fabs(bt - R.tstars[b]));
    }
    std::printf("  fast-vs-brute max |dt*| over the 16 sign vectors = %.3e\n", maxerr);
    if (!reps_enum_ok || maxerr > 1e-9) {
        std::printf("  FAIL equivalence (enumerated, reps %d)\n", R.reps);
        ++g_fail;
    } else std::printf("  ok   fast == brute-force (full 2^4 Rademacher set)\n");

    // determinism: rerun, identical tstars
    ModelData md2 = md;  // fresh copy
    WildBootResult R2 = wild_cluster_bootstrap(md2, 1, 0.0, reps, seed, 0,
                                               BootWeights::Rademacher);
    bool same = R.tstars.size() == R2.tstars.size();
    for (size_t b = 0; same && b < R.tstars.size(); ++b)
        if (R.tstars[b] != R2.tstars[b]) same = false;
    if (!same) { std::printf("  FAIL determinism\n"); ++g_fail; }
    else std::printf("  ok   deterministic across runs\n");

    // --- IV: fast vs brute-force equivalence (enumerated: 16 sign vectors) ---
    std::vector<double> d = x;  // endogenous
    std::vector<double> z = {2, 1, 4, 3, 5, 2, 6, 1, 4, 2, 5, 3};  // instrument
    IvModelData ivmd;
    ivmd.n = n;
    ivmd.has_intercept = true;
    ivmd.kexog = 1;
    ivmd.kendog = 1;
    ivmd.kinst = 1;
    ivmd.Xexog.assign(n, 1.0);
    ivmd.Xendog = d;
    ivmd.Zinst = z;
    ivmd.y = y;
    ivmd.exog_names = {"(intercept)"};
    ivmd.endog_names = {"d"};
    ivmd.cluster = {cl};
    WildBootResult Riv = wild_cluster_bootstrap_iv(ivmd, 0.0, reps, seed, 0,
                                                   BootWeights::Rademacher);
    double ssf_iv = (static_cast<double>(G) / (G - 1)) * (static_cast<double>(n - 1) / (n - 2));
    double maxe_iv = 0;
    bool reps_iv_ok = (Riv.reps == 16 && Riv.tstars.size() == 16);
    for (int b = 0; reps_iv_ok && b < 16; ++b)
        maxe_iv = std::max(maxe_iv, std::fabs(brute_tstar_iv(y, d, z, gcodes, G, 0.0,
                                                             enum_draw(b, G), ssf_iv) -
                                              Riv.tstars[b]));
    std::printf("  IV fast-vs-brute max |dt*| = %.3e\n", maxe_iv);
    if (!reps_iv_ok || maxe_iv > 1e-9) { std::printf("  FAIL IV equivalence\n"); ++g_fail; }
    else std::printf("  ok   IV fast == brute-force\n");

    // --- E9: high-end memory guard. G just above the 8000-cluster ceiling must
    // throw a clean input error *before* the O(G^2) W matrix is materialized, so
    // this stays cheap — no real 8000-cluster bootstrap is run. Reuses the tiny
    // 12-row/4-cluster dataset above and only bumps the cluster factor's n_levels
    // metadata (codes/count untouched, still index 0..3 << 8001).
    {
        ModelData mdBig = md;  // fresh copy; md.fe is empty so demean() no-ops anyway
        mdBig.cluster[0].n_levels = 8001;  // one above the ceiling
        bool threw = false;
        std::string what;
        try {
            wild_cluster_bootstrap(mdBig, 1, 0.0, reps, seed, 0, BootWeights::Rademacher);
        } catch (const std::runtime_error& ex) {
            threw = true;
            what = ex.what();
        }
        bool msg_ok = threw && what.find("8001") != std::string::npos &&
                     what.find("8000") != std::string::npos;
        if (!msg_ok) {
            std::printf("  FAIL high-G guard (threw=%d what=\"%s\")\n", threw, what.c_str());
            ++g_fail;
        } else {
            std::printf("  ok   G=8001 > ceiling throws: %s\n", what.c_str());
        }
    }

    // --- Small-G path unaffected by the new guard: same dataset, real G=4. Reuses
    // the R already computed by the fast-vs-brute check above — reaching that
    // comparison at all (and getting n_clusters back unchanged) is the "no
    // in-range behavior change" assertion for E9.
    if (R.n_clusters != G) {
        std::printf("  FAIL small-G path perturbed by guard (n_clusters=%d, want %d)\n",
                   R.n_clusters, G);
        ++g_fail;
    } else {
        std::printf("  ok   small-G (G=%d) path unaffected by high-end guard\n", G);
    }

    // --- boottest 4.5.3 rules on a 7-cluster design (the G of the oracle arms
    // w2_esample_tsrefit.log:336 W1 and :457 W5: 7 clusters, reps(999) -> "Sampling
    // each once", 128 replications, Rademacher weights, p = 40/128).
    {
        const int G7 = 7, per = 5, n7 = G7 * per;
        std::vector<double> x7(n7), y7(n7);
        std::vector<int> g7(n7);
        for (int i = 0; i < n7; ++i) {
            g7[i] = i % G7;
            x7[i] = std::fmod(i * 7.0, 11.0) + (i % 5) / 10.0;
            y7[i] = 1.0 + 0.05 * x7[i] + ((g7[i] * 5) % 7) / 3.0 + std::fmod(i * 11.0, 17.0) / 5.0;
        }
        ModelData m7;
        m7.has_intercept = true;
        m7.dm.n = n7;
        m7.dm.k = 2;
        m7.dm.y = y7;
        m7.dm.X.resize(static_cast<size_t>(n7) * 2);
        for (int i = 0; i < n7; ++i) { m7.dm.X[i] = 1.0; m7.dm.X[n7 + i] = x7[i]; }
        m7.dm.xnames = {"(intercept)", "x"};
        Factor c7;
        c7.name = "g";
        c7.n_levels = G7;
        c7.codes.assign(g7.begin(), g7.end());
        c7.count.assign(G7, static_cast<double>(per));
        m7.cluster = {c7};
        const double ssf7 = (static_cast<double>(G7) / (G7 - 1)) *
                            (static_cast<double>(n7 - 1) / (n7 - 2));
        auto run7 = [&](int rp, uint64_t sd, BootWeights wt) {
            ModelData mm = m7;
            return wild_cluster_bootstrap(mm, 1, 0.0, rp, sd, 0, wt);
        };

        // R1 (matches boottest's behaviour): the default is Rademacher for any G -- no Webb
        // switch below 12 clusters; Webb only when asked for, and it never enumerates.
        WildBootResult Rd = run7(999, 1, BootWeights::Rademacher);
        if (Rd.weights != "rademacher" || Rd.webb_fallback) {
            std::printf("  FAIL R1 7-cluster default weights=%s webb_fallback=%d\n",
                        Rd.weights.c_str(), Rd.webb_fallback);
            ++g_fail;
        } else std::printf("  ok   R1 7-cluster default: rademacher, webb_fallback false\n");
        WildBootResult Rw = run7(999, 1, BootWeights::Webb);
        const double kw = Rw.p_symmetric * Rw.reps;
        if (Rw.weights != "webb" || Rw.reps != 999 || Rw.tstars.size() != 999 ||
            Rw.webb_fallback || kw != std::floor(kw)) {
            std::printf("  FAIL R1 webb: weights=%s reps=%d p=%.17g\n", Rw.weights.c_str(),
                        Rw.reps, Rw.p_symmetric);
            ++g_fail;
        } else std::printf("  ok   R1 Webb honoured, not enumerated (reps 999, p = %.0f/999)\n", kw);

        // R2 (matches boottest's behaviour): G ln2 < ln(reps) + 1e-6 runs all 2^G
        // sign vectors once and reports 2^G; the answer no longer depends on the seed.
        WildBootResult Rs2 = run7(999, 2, BootWeights::Rademacher);
        bool seedfree = Rd.reps == 128 && Rs2.reps == 128 && Rd.tstars == Rs2.tstars &&
                        Rd.p_symmetric == Rs2.p_symmetric && Rd.p_equaltail == Rs2.p_equaltail;
        if (!seedfree) {
            std::printf("  FAIL R2 G=7 reps 999: reps=%d/%d p=%.17g/%.17g\n", Rd.reps, Rs2.reps,
                        Rd.p_symmetric, Rs2.p_symmetric);
            ++g_fail;
        } else std::printf("  ok   R2 G=7 reps 999 -> 128 replications, seed 1 == seed 2\n");
        WildBootResult R128 = run7(128, 5, BootWeights::Rademacher);
        WildBootResult R127 = run7(127, 5, BootWeights::Rademacher);
        bool bound = R128.reps == 128 && R128.tstars == Rd.tstars && R127.reps == 127 &&
                     R127.tstars.size() == 127;
        // reps 127 is random: draw 0 is Philox (seed 5, b 0), not the all -1 vector.
        if (bound) {
            std::vector<double> v0(G7);
            for (int gg = 0; gg < G7; ++gg) v0[gg] = rademacher(5, 0, gg);
            bound = std::fabs(brute_tstar(y7, x7, g7, G7, 1, 0.0, v0, ssf7) - R127.tstars[0]) < 1e-9;
        }
        if (!bound) {
            std::printf("  FAIL R2 boundary: reps 128 -> %d, reps 127 -> %d\n", R128.reps,
                        R127.reps);
            ++g_fail;
        } else std::printf("  ok   R2 boundary: reps 128 enumerates, reps 127 draws at random\n");

        // R3 (matches boottest's getp step, BFeas = B): recount by brute-force refits over
        // all 128 sign vectors. A tie is |difference| <= 1e-9 relative; the only ties
        // allowed are the all +1 vector (t* = t_ref) and, for |t*|, the all -1 one.
        bool eq_ok = Rd.tstars.size() == 128;
        double maxe7 = 0;
        std::vector<double> bt(128);
        for (int b = 0; eq_ok && b < 128; ++b) {
            bt[b] = brute_tstar(y7, x7, g7, G7, 1, 0.0, enum_draw(b, G7), ssf7);
            maxe7 = std::max(maxe7, std::fabs(bt[b] - Rd.tstars[b]));
        }
        long ksym = 0, kbelow = 0, kabove = 0;
        bool ties_ok = eq_ok;
        if (eq_ok) {
            const double tr = bt[127], tol = 1e-9 * std::max(1.0, std::fabs(tr));
            for (int b = 0; b < 128; ++b) {
                const double ds = std::fabs(bt[b]) - std::fabs(tr), dt = bt[b] - tr;
                if (std::fabs(ds) <= tol) { if (b != 0 && b != 127) ties_ok = false; }
                else if (ds > 0) ++ksym;
                if (std::fabs(dt) <= tol) { if (b != 127) ties_ok = false; }
                else if (dt < 0) ++kbelow;
                else ++kabove;
            }
        }
        const double psym = static_cast<double>(ksym) / 128.0;
        const double peq = 2.0 * static_cast<double>(std::min(kbelow, kabove)) / 128.0;
        std::printf("  R3 7-cluster: brute k_sym=%ld below=%ld above=%ld; engine p_sym=%.17g "
                    "p_eq=%.17g (max |dt*| %.3e)\n", ksym, kbelow, kabove, Rd.p_symmetric,
                    Rd.p_equaltail, maxe7);
        if (!eq_ok || !ties_ok || maxe7 > 1e-9 || Rd.p_symmetric != psym ||
            Rd.p_equaltail != peq || ksym == 0 || ksym >= 126) {
            std::printf("  FAIL R3 p = k/2^G with strict comparisons (want %.17g / %.17g)\n",
                        psym, peq);
            ++g_fail;
        } else std::printf("  ok   R3 p_symmetric = %ld/128, p_equaltail = 2*%ld/128\n", ksym,
                           std::min(kbelow, kabove));

        // R4 (design: the comparison reference is t*(+1) from the draws' own kernel,
        // as boottest's dist[1] is the v = 1 column of the same pass, :1465/:1485).
        // The all +1 draw (b = 127) equals t_ref bit for bit and the all -1 draw (b = 0)
        // is its exact negation, so the strict |t*| count drops both: a non-strict
        // count over the engine's own tstars is exactly 2 larger.
        long ge = 0;
        for (double t : Rd.tstars) if (std::fabs(t) >= std::fabs(Rd.t_ref)) ++ge;
        uint64_t ub0 = 0, ub1 = 0;
        std::memcpy(&ub0, &Rd.t0, 8);
        std::memcpy(&ub1, &Rd.t_ref, 8);
        const long long ulp = static_cast<long long>(ub0 > ub1 ? ub0 - ub1 : ub1 - ub0);
        std::printf("  R4 t0=%.17g t*(+1)=%.17g ulp gap %lld\n", Rd.t0, Rd.t_ref, ulp);
        if (!eq_ok || Rd.tstars[127] != Rd.t_ref || Rd.tstars[0] != -Rd.t_ref ||
            static_cast<double>(ge - 2) / 128.0 != Rd.p_symmetric ||
            std::fabs(Rd.t0 - Rd.t_ref) > 1e-9 * std::max(1.0, std::fabs(Rd.t0))) {
            std::printf("  FAIL R4 tie vectors (t*[127]=%.17g t*[0]=%.17g ge=%ld)\n",
                        eq_ok ? Rd.tstars[127] : 0.0, eq_ok ? Rd.tstars[0] : 0.0, ge);
            ++g_fail;
        } else std::printf("  ok   R4 the all +1 / all -1 vectors are never counted\n");
    }

    // --- R5: an FE that is not nested in the clusters. The replicate refit
    // re-absorbs the FE, so y* is demeaned again (the oracle arm: w2_esample_tsrefit
    // .log:457 W5, reghdfe absorb(id) with 7 clusters in cl cutting across id, 40/128).
    // Brute force: demean y*, refit on the demeaned X, cluster SE, over all 2^5 sign
    // vectors. t* is compared as t*(v)/t*(+1), so the dof factor drops out.
    {
        const int G5 = 5, n5 = 40, nf = 4;
        std::vector<double> xa(n5), xb(n5), y5(n5);
        std::vector<int> g5(n5), f5(n5);
        for (int i = 0; i < n5; ++i) {
            g5[i] = i % G5;
            f5[i] = i / 10;  // each FE group spans all five clusters
            xa[i] = std::fmod(i * 7.0, 11.0) + (i % 5) / 10.0;
            xb[i] = std::fmod(i * 3.0, 5.0) + std::fmod(i * 13.0, 7.0) / 10.0;
            y5[i] = 0.05 * xa[i] - xb[i] + f5[i] / 3.0 + std::fmod(i * 11.0, 17.0) / 5.0 +
                    ((g5[i] * 3) % 5) / 4.0;
        }
        auto demean_f = [&](std::vector<double> v) {
            std::vector<double> s(nf, 0.0), c(nf, 0.0);
            for (int i = 0; i < n5; ++i) { s[f5[i]] += v[i]; c[f5[i]] += 1; }
            for (int i = 0; i < n5; ++i) v[i] -= s[f5[i]] / c[f5[i]];
            return v;
        };
        const std::vector<double> yt = demean_f(y5), at = demean_f(xa), bt5 = demean_f(xb);
        double saa = 0, sab = 0, sbb = 0;
        for (int i = 0; i < n5; ++i) {
            saa += at[i] * at[i];
            sab += at[i] * bt5[i];
            sbb += bt5[i] * bt5[i];
        }
        const double det = saa * sbb - sab * sab;
        const double ia0 = sbb / det, ia1 = -sab / det;  // row 0 of (X'X)^-1
        double sby = 0;
        for (int i = 0; i < n5; ++i) sby += bt5[i] * yt[i];
        const double bR = sby / sbb;  // restricted: the xa coefficient is 0
        auto brute_fe = [&](const std::vector<double>& vg) {
            std::vector<double> ys(n5);
            for (int i = 0; i < n5; ++i) ys[i] = bt5[i] * bR + vg[g5[i]] * (yt[i] - bt5[i] * bR);
            ys = demean_f(ys);
            double say = 0, sby2 = 0;
            for (int i = 0; i < n5; ++i) { say += at[i] * ys[i]; sby2 += bt5[i] * ys[i]; }
            const double b0 = (sbb * say - sab * sby2) / det, b1 = (saa * sby2 - sab * say) / det;
            std::vector<double> sc(G5, 0.0);
            for (int i = 0; i < n5; ++i)
                sc[g5[i]] += (ia0 * at[i] + ia1 * bt5[i]) * (ys[i] - at[i] * b0 - bt5[i] * b1);
            double se2 = 0;
            for (double q : sc) se2 += q * q;
            return b0 / std::sqrt(se2);
        };
        ModelData m5;
        m5.has_intercept = false;
        m5.dm.n = n5;
        m5.dm.k = 2;
        m5.dm.y = y5;
        m5.dm.X.resize(static_cast<size_t>(n5) * 2);
        for (int i = 0; i < n5; ++i) { m5.dm.X[i] = xa[i]; m5.dm.X[n5 + i] = xb[i]; }
        m5.dm.xnames = {"xa", "xb"};
        Factor fF, cF;
        fF.name = "f";
        fF.n_levels = nf;
        fF.codes.assign(f5.begin(), f5.end());
        fF.count.assign(nf, 10.0);
        cF.name = "g";
        cF.n_levels = G5;
        cF.codes.assign(g5.begin(), g5.end());
        cF.count.assign(G5, 8.0);
        m5.fe = {fF};
        m5.cluster = {cF};
        WildBootResult R5 = wild_cluster_bootstrap(m5, 0, 0.0, 999, 1, 0, BootWeights::Rademacher);
        bool ok5 = R5.reps == 32 && R5.tstars.size() == 32;
        double maxr = 0;
        long k5 = 0;
        std::vector<double> b5(32);
        for (int b = 0; ok5 && b < 32; ++b) b5[b] = brute_fe(enum_draw(b, G5));
        if (ok5) {
            const double tr = b5[31], tol = 1e-9 * std::max(1.0, std::fabs(tr));
            for (int b = 0; b < 32; ++b) {
                maxr = std::max(maxr, std::fabs(b5[b] / tr - R5.tstars[b] / R5.tstars[31]));
                const double ds = std::fabs(b5[b]) - std::fabs(tr);
                if (std::fabs(ds) <= tol) { if (b != 0 && b != 31) ok5 = false; }
                else if (ds > 0) ++k5;
            }
        }
        std::printf("  R5 FE across clusters: brute k_sym=%ld/32, engine p_sym=%.17g, max "
                    "|d(t*/t_ref)| %.3e\n", k5, R5.p_symmetric, maxr);
        if (!ok5 || maxr > 1e-9 || R5.p_symmetric != static_cast<double>(k5) / 32.0 ||
            k5 == 0 || k5 >= 30) {
            std::printf("  FAIL R5 refit with the FE re-absorbed\n");
            ++g_fail;
        } else std::printf("  ok   R5 FE not nested in clusters: t* = the FE refit's, p = %ld/32\n", k5);
    }

    // --- W1-W4: wild_restricted_efficient_bootstrap on the w2t_boottest_family
    // fixture (tests/eval/golden/aer_w2/w2t_boottest_family.do:61-79). Oracle
    // statistics from StataBE 19.5 / boottest 4.5.3: D1_st z .1434202311956093
    // (w2t_boottest_family.log:1053), D2_st z .1683750187999031 (:1111), D3_st t
    // 2.286824317666255 df_r 29 (:1203), boottest_opt_c2 t 32.32783204036483 df_r 6
    // (w2l_vce_shape_det.log:4152, %18.0g from the method probe). The p is a draw:
    // checked for its range and for reproducibility only.
    {
        const int n = 180;
        std::vector<double> idv(n), id2v(n), x1(n), z1(n), x2(n), y(n), yw(n), z(n), en(n);
        auto md = [](double a, double b) { return std::fmod(a, b); };
        for (int i = 0; i < n; ++i) {
            const double k = i + 1;
            idv[i] = std::ceil(k / 6);
            id2v[i] = md(k, 7) + 1;
            x1[i] = md(k * 7, 11) + md(k, 5) / 8;
            z1[i] = md(k * 5, 13) + x1[i] / 2 + md(k * 17, 19) / 16;
            x2[i] = 0.5 * z1[i] + md(k * 3, 5) + md(k * 13, 7) / 8 + md(k * 23, 29) / 32;
            y[i] = 1 + 2 * x1[i] - x2[i] + idv[i] / 4 + md(k * 11, 17) / 4 +
                   (md(k * 37, 41) / 64 + md(k * 53, 59) / 64 + md(k * 29, 31) / 64 - 1) * 3;
            yw[i] = y[i] - 31 * x1[i] / 16;
            z[i] = md(k * 5, 13) / 2 + md(k * 31, 37) / 16;
            en[i] = z[i] / 2 + x1[i] / 8 + md(k * 37, 41) / 32 + md(k * 7, 13) / 16;
        }
        auto factor = [&](const std::vector<double>& v) {
            Factor f;
            std::vector<double> u = v;
            std::sort(u.begin(), u.end());
            u.erase(std::unique(u.begin(), u.end()), u.end());
            f.n_levels = static_cast<int>(u.size());
            f.codes.resize(v.size());
            for (size_t i = 0; i < v.size(); ++i)
                f.codes[i] = static_cast<int32_t>(std::lower_bound(u.begin(), u.end(), v[i]) - u.begin());
            return f;
        };
        const std::vector<Factor> cl1 = {factor(idv)}, cl2 = {factor(idv), factor(id2v)};
        std::vector<double> X1c(2 * n), X1(x1), Z2(2 * n), X12c(3 * n);
        for (int i = 0; i < n; ++i) {
            X1c[i] = x1[i]; X1c[n + i] = 1.0;
            Z2[i] = z[i]; Z2[n + i] = z1[i];
            X12c[i] = x1[i]; X12c[n + i] = x2[i]; X12c[2 * n + i] = 1.0;
        }
        struct W { const char* label; double got, want, dfr, wantdfr, p1, p2; };
        std::vector<W> ws;
        auto run = [&](const char* label, const std::vector<double>& yy, const std::vector<double>& Xe,
                       int kx, const std::vector<double>& Y2, int p, const std::vector<double>& Zx,
                       int kz, const std::vector<Factor>& cl, WreSpec spec, double want, double wantdfr) {
            std::vector<std::string> en_names(kx, "x"), endn(p, "endog");
            en_names[0] = "x1";
            WildBootResult a = wild_restricted_efficient_bootstrap(yy, n, Xe, kx, en_names, Y2, p, endn, Zx, kz, cl, spec);
            WildBootResult b = wild_restricted_efficient_bootstrap(yy, n, Xe, kx, en_names, Y2, p, endn, Zx, kz, cl, spec);
            ws.push_back({label, a.t0, want, a.df_r, wantdfr, a.p_symmetric, b.p_symmetric});
        };
        WreSpec s;
        s.reps = 99; s.seed = 1; s.param_index = 0;
        run("W1 D1 2SLS z", yw, X1c, 2, en, 1, Z2, 2, cl1, s, .1434202311956093, 29);
        WreSpec sl = s; sl.liml = true;
        run("W2 D2 LIML z", yw, X1c, 2, en, 1, Z2, 2, cl1, sl, .1683750187999031, 29);
        WreSpec s3 = s; s3.small = true; s3.fe_dof_adj = 6;
        run("W3 D3 ivreghdfe t", yw, X1, 1, en, 1, z, 1, cl1, s3, 2.286824317666255, 29);
        WreSpec s4 = s; s4.small = true;
        run("W4 opt_c2 multiway t", y, X12c, 3, {}, 0, {}, 0, cl2, s4, 32.32783204036483, 6);
        for (const W& w : ws) {
            const bool ok = std::fabs(w.got - w.want) <= 1e-9 * std::fabs(w.want) && w.dfr == w.wantdfr &&
                            w.p1 >= 0 && w.p1 <= 1 && w.p1 == w.p2;
            std::printf("  %s %s: stat %.17g (Stata %.17g) df_r %g p %.17g/%.17g\n", ok ? "ok  " : "FAIL",
                        w.label, w.got, w.want, w.dfr, w.p1, w.p2);
            if (!ok) ++g_fail;
        }
    }

    if (g_fail) { std::printf("FAILED (%d)\n", g_fail); return 1; }
    std::printf("PASSED\n");
    return 0;
}
