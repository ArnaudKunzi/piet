// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// P9 entry contract — thread-count determinism for the ESTIMATOR-side parallel
// kernels (TOLERANCE.md §4.2, ROADMAP P9 row).
//
// §4.2 requires that the floating-point reduction DAG be a function of the DATA
// ONLY: fixed block constants (never n/nthreads), fixed-order block merge, no
// atomic scatter-add, no OpenMP reduction. Under those laws 1-core and N-core
// outputs are byte-identical, which is P9's exit evidence.
//
// test_morsel_determinism already gates the Frame-level dispatch sites
// (collapse aggregation + merge column materialization, src/data/frame.cpp).
// It does NOT reach the other three thread-dispatch surfaces in the engine, and
// the P9 entry audit found them unguarded:
//
//   src/hdfe/demean.cpp:116,122   ThreadPool::global().run over reduction LANES
//   src/estimators/xtlogit.cpp:324,438  parallel_for over panels
//   src/inference/wildboot.cpp:193      parallel_for over bootstrap replicates
//
// All three are compliant BY CONSTRUCTION today — demean re-keys its reduction
// width to reduction_lanes(n) (a pure function of n, independent of the worker
// count, demean.cpp:77-85) and the other two write disjoint per-panel/per-rep
// slots that a serial fixed-order loop then sums. This test is what keeps that
// true: it is a guard against a future patch quietly making a reduction width
// or merge order depend on the thread count.
//
// Mechanism (identical to test_morsel_determinism, deliberately): the pool's
// worker count is a process-wide singleton read ONCE from CECON_NUM_THREADS
// (threads.cpp default_threads()), so two counts require two PROCESSES. The
// parent forks (POSIX) / re-execs itself (Windows) one child per count; each
// child sets the env, runs the kernels on synthetic deterministic input, and
// dumps RAW f64 BITS. The parent byte-compares. A raw bit dump rather than a
// value tolerance is the point: any reassociated sum flips low bits and fails.
//
// Every case is sized so the parallel unit count exceeds 4 (demean: n=200000 =>
// reduction_lanes = 13 lanes; xtlogit: ng = 400 panels; wildboot: reps = 999),
// otherwise ThreadPool::run's kRunDirectFloor or parallel_for's min(nthreads, n)
// clamp would make the 4-thread arm silently serial and the comparison vacuous.
// Those are compile-time constants below, not runtime assertions — the child's
// default_threads() check is what proves the two arms really differ in workers.
//
// RED certificate (2026-08-14, P9 entry wave): each of the three comparators was
// falsified in turn by seeding a real §4.2 violation into the production kernel —
// a thread-count-keyed lane width in reduction_lanes() (demean went RED alone),
// then a default_threads()-dependent term in xtlogit's ll accumulation and in
// wildboot's tstars (those two went RED, demean stayed GREEN). Seeds reverted.
//
// AER bundle H1 arms (2026-09-06, ONESHOT-AER-SCOPE1-PLAN step 7 / §4): hpfilter,
// lpoly and pca. Today none of the three kernels (src/estimators/aer_smooth.cpp,
// src/estimators/pca.cpp) dispatches to the pool at all -- they are serial by
// construction, so 1-vs-4 identity is trivially true. The arms exist for the
// same reason the first three do: a later patch that parallelises the per-panel
// HP loop, the per-grid-point local fits, or the p x p moment accumulation must
// keep the reduction DAG a function of the data only, and this is the detector.
// RED certificate for the arms: see the landing commit message (a
// default_threads()-keyed term seeded into one kernel flipped exactly that
// arm RED; seed reverted).
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core/model.h"
#include "core/threads.h"
#include "estimators/aer_smooth.h"
#include "estimators/nnmatch.h"
#include "estimators/pca.h"
#include "estimators/oaxaca.h"
#include "estimators/xtlogit.h"
#include "hdfe/demean.h"
#include "inference/wildboot.h"
#include "io/win_compat.h"

using namespace cecon;

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

static int fail = 0;
static void check(bool cond, const std::string& msg) {
    if (!cond) { std::printf("  FAIL: %s\n", msg.c_str()); fail = 1; }
    else std::printf("  ok: %s\n", msg.c_str());
}

// Deterministic value source — a plain LCG, so both children generate byte-identical
// input without depending on any library RNG.
struct Lcg {
    uint64_t s;
    explicit Lcg(uint64_t seed) : s(seed) {}
    double next() {  // uniform in [0,1)
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<double>((s >> 11) & ((1ULL << 53) - 1)) / 9007199254740992.0;
    }
};

// ---- case 1: HDFE demean (ThreadPool over reduction lanes) --------------------
//
// n is chosen well above LANE_TARGET (16384, demean.cpp:78) so reduction_lanes()
// returns many lanes and the lane loop genuinely splits across workers. Two FE
// dimensions force the alternating-projection iteration (repeated group means =
// repeated float accumulation), which is where a thread-count-keyed reduction
// width would show up.
static std::vector<double> case_demean() {
    const int64_t n = 200000;
    const int m = 3;
    Lcg r(0x5eed1234ULL);
    std::vector<double> M(static_cast<size_t>(n) * m);
    for (size_t i = 0; i < M.size(); ++i) M[i] = r.next() * 200.0 - 100.0;

    Factor f1;
    f1.name = "id";
    f1.n_levels = 977;  // prime-ish level counts: groups straddle lane boundaries
    f1.codes.resize(static_cast<size_t>(n));
    f1.count.assign(static_cast<size_t>(f1.n_levels), 0.0);
    Factor f2;
    f2.name = "t";
    f2.n_levels = 53;
    f2.codes.resize(static_cast<size_t>(n));
    f2.count.assign(static_cast<size_t>(f2.n_levels), 0.0);
    for (int64_t i = 0; i < n; ++i) {
        int32_t c1 = static_cast<int32_t>(i % f1.n_levels);
        int32_t c2 = static_cast<int32_t>((i / 7) % f2.n_levels);
        f1.codes[static_cast<size_t>(i)] = c1;
        f2.codes[static_cast<size_t>(i)] = c2;
        f1.count[static_cast<size_t>(c1)] += 1.0;
        f2.count[static_cast<size_t>(c2)] += 1.0;
    }
    std::vector<Factor> fe = {f1, f2};

    DemeanReport rep = demean(M.data(), n, m, fe);
    // The residual and iteration count are part of the observable output: an
    // order change that altered convergence would be caught here too.
    M.push_back(rep.resid);
    M.push_back(static_cast<double>(rep.iters));
    M.push_back(rep.converged ? 1.0 : 0.0);
    return M;
}

// ---- case 2: xtlogit, fe (parallel_for over panels) ---------------------------
//
// ng panels >> 4, each contributing ll_g / score_g / negH_g into its own slot,
// summed by a serial fixed-order loop over gi (xtlogit.cpp:386-396). Panels are
// built with mixed within-variation so none is dropped for K∈{0,T}.
static std::vector<double> case_xtlogit() {
    const int ng = 400, T = 6, k = 2;
    const int64_t N = static_cast<int64_t>(ng) * T;
    Lcg r(0xabcdef01ULL);
    std::vector<double> X(static_cast<size_t>(N) * k), y(static_cast<size_t>(N)),
        ivar(static_cast<size_t>(N));
    for (int g = 0; g < ng; ++g) {
        for (int t = 0; t < T; ++t) {
            const int64_t i = static_cast<int64_t>(g) * T + t;
            const double x1 = r.next() * 2.0 - 1.0;
            const double x2 = r.next() * 2.0 - 1.0;
            X[static_cast<size_t>(0 * N + i)] = x1;
            X[static_cast<size_t>(1 * N + i)] = x2;
            // Alternating base outcome guarantees 0 < K_i < T for every panel, so
            // no panel is dropped and every one contributes to the reduction.
            const double eta = 0.8 * x1 - 0.5 * x2;
            y[static_cast<size_t>(i)] = ((t % 2 == 0) != (eta > 0.0)) ? 1.0 : 0.0;
            ivar[static_cast<size_t>(i)] = static_cast<double>(g);
        }
    }
    XtlogitFeResult R = xtlogit_fe(X.data(), N, k, y.data(), ivar.data());
    std::vector<double> out;
    out.insert(out.end(), R.beta.begin(), R.beta.end());
    out.insert(out.end(), R.se_iid.begin(), R.se_iid.end());
    out.insert(out.end(), R.se_robust.begin(), R.se_robust.end());
    out.push_back(R.loglik);
    out.push_back(static_cast<double>(R.iters));
    out.push_back(R.converged ? 1.0 : 0.0);
    out.push_back(static_cast<double>(R.n_groups));
    out.push_back(static_cast<double>(R.n_dropped_const));
    return out;
}

// ---- case 3: wild cluster bootstrap (parallel_for over replicates) ------------
//
// reps >> 4; each replicate writes tstars[b] only, and the p-values are computed
// by a serial loop over b. The Philox counter RNG makes each replicate's draw a
// function of (seed, b, g) — never of a stream position — which is what lets the
// replicate loop be split at all.
static std::vector<double> case_wildboot() {
    const int G = 24, per = 8;
    const int n = G * per, k = 2;
    Lcg r(0x13579bdfULL);
    ModelData md;
    md.has_intercept = true;
    md.dm.n = n;
    md.dm.k = k;
    md.dm.y.resize(static_cast<size_t>(n));
    md.dm.X.resize(static_cast<size_t>(n) * k);
    md.dm.xnames = {"(intercept)", "x"};
    Factor cl;
    cl.name = "g";
    cl.n_levels = G;
    cl.codes.resize(static_cast<size_t>(n));
    cl.count.assign(static_cast<size_t>(G), static_cast<double>(per));
    for (int g = 0; g < G; ++g) {
        const double ge = r.next() * 2.0 - 1.0;  // cluster effect
        for (int j = 0; j < per; ++j) {
            const int i = g * per + j;
            const double x = r.next() * 4.0 - 2.0;
            md.dm.X[static_cast<size_t>(i)] = 1.0;
            md.dm.X[static_cast<size_t>(n + i)] = x;
            md.dm.y[static_cast<size_t>(i)] = 0.7 * x + ge + (r.next() - 0.5);
            cl.codes[static_cast<size_t>(i)] = static_cast<int32_t>(g);
        }
    }
    md.cluster = {cl};

    WildBootResult R = wild_cluster_bootstrap(md, /*param_index=*/1, /*null_beta=*/0.0,
                                              /*reps=*/999, /*seed=*/20260814ULL,
                                              /*cluster_index=*/0, BootWeights::Rademacher);
    std::vector<double> out;
    out.insert(out.end(), R.tstars.begin(), R.tstars.end());
    out.push_back(R.beta);
    out.push_back(R.se);
    out.push_back(R.t0);
    out.push_back(R.p_symmetric);
    out.push_back(R.p_equaltail);
    return out;
}

// ---- case 4: Hodrick-Prescott over panels (AER H1) -----------------------------
//
// The cobra `hpfilter --panel` surface runs hp_filter() once per panel on that
// panel's contiguous run of rows (apps/cmds/cmds_aer_estim.cpp), so the arm is
// the same shape: 20 panels x 5000 obs, each filtered independently, cycle and
// trend concatenated in panel order. A future per-panel parallel_for must keep
// each panel's banded LDL' and the scatter into cycle/trend slot-disjoint.
static std::vector<double> case_hpfilter() {
    const int panels = 20, T = 5000;
    Lcg r(0x48b1f11ULL);
    std::vector<double> out;
    out.reserve(static_cast<size_t>(panels) * T * 2);
    for (int g = 0; g < panels; ++g) {
        std::vector<double> y(static_cast<size_t>(T));
        const double level = 100.0 + 10.0 * g;
        for (int t = 0; t < T; ++t) {
            // trend + seasonal + noise, all deterministic from the LCG
            y[static_cast<size_t>(t)] = level + 0.05 * t + 3.0 * std::sin(t / 3.0) +
                                        (r.next() - 0.5) * 4.0;
        }
        HpResult hr = hp_filter(y, 1600.0);
        out.insert(out.end(), hr.cycle.begin(), hr.cycle.end());
        out.insert(out.end(), hr.trend.begin(), hr.trend.end());
    }
    return out;
}

// ---- case 5: local polynomial smoother (AER H1) --------------------------------
//
// n = 200000 sorted x (the cobra surface sorts before calling lpoly_fit), a
// 50-point grid, degree 1 with the rule-of-thumb bandwidth so BOTH the global
// quartic ROT fit and the 50 local WLS fits are exercised, plus standard
// errors. Output = grid, smooth, se, bwidth, pwidth, ngrid.
static std::vector<double> case_lpoly() {
    const size_t n = 200000;
    Lcg r(0x1b01a1ULL);
    std::vector<double> x(n), y(n);
    for (size_t i = 0; i < n; ++i) {
        // ascending x with irregular spacing; y a smooth curve plus noise
        x[i] = (i == 0 ? 0.0 : x[i - 1]) + r.next() * 0.01;
    }
    for (size_t i = 0; i < n; ++i)
        y[i] = 2.0 + 0.3 * x[i] + std::sin(x[i] / 50.0) + (r.next() - 0.5);
    LpolyOpts o;
    o.kernel = LpolyKernel::Epanechnikov;
    o.degree = 1;
    o.n = 50;
    o.want_se = true;
    LpolyResult R = lpoly_fit(x, y, o);
    std::vector<double> out;
    out.insert(out.end(), R.grid.begin(), R.grid.end());
    out.insert(out.end(), R.smooth.begin(), R.smooth.end());
    out.insert(out.end(), R.se.begin(), R.se.end());
    out.push_back(R.bwidth);
    out.push_back(R.pwidth);
    out.push_back(static_cast<double>(R.ngrid));
    return out;
}

// ---- case 6: principal components (AER H1) -------------------------------------
//
// N = 200000 rows x p = 8 correlated columns (each is a mix of two latent
// factors plus noise so the spectrum is spread, not degenerate). The p x p
// moment accumulation over N rows is the only O(N) reduction in pca_fit; every
// downstream quantity (eigen-decomposition, loadings, psi, trace/lndet/cond,
// components table) is a deterministic function of those p*p sums, so it is all
// dumped: a reassociated moment sum would change the low bits everywhere.
static std::vector<double> case_pca() {
    const size_t N = 200000;
    const int p = 8;
    Lcg r(0x9ca0c1ULL);
    std::vector<std::vector<double>> col(static_cast<size_t>(p), std::vector<double>(N));
    for (size_t i = 0; i < N; ++i) {
        const double f1 = r.next() * 2.0 - 1.0, f2 = r.next() * 2.0 - 1.0;
        for (int j = 0; j < p; ++j) {
            const double w1 = 1.0 - 0.1 * j, w2 = 0.15 * j;
            col[static_cast<size_t>(j)][i] =
                10.0 * j + w1 * f1 + w2 * f2 + (r.next() - 0.5) * 0.5;
        }
    }
    std::vector<std::string> names;
    std::vector<const double*> cols;
    for (int j = 0; j < p; ++j) {
        names.push_back("v" + std::to_string(j));
        cols.push_back(col[static_cast<size_t>(j)].data());
    }
    std::vector<char> keep(N, 1);
    PcaOpts o;  // correlation PCA, all p components, default mineigen
    PcaResult R = pca_fit(names, cols, keep, o);
    std::vector<double> out;
    out.insert(out.end(), R.ev.begin(), R.ev.end());
    out.insert(out.end(), R.loadings.begin(), R.loadings.end());
    out.insert(out.end(), R.psi.begin(), R.psi.end());
    out.insert(out.end(), R.c.begin(), R.c.end());
    out.insert(out.end(), R.means.begin(), R.means.end());
    out.insert(out.end(), R.sds.begin(), R.sds.end());
    out.insert(out.end(), R.vars.begin(), R.vars.end());
    out.insert(out.end(), R.components_tbl.begin(), R.components_tbl.end());
    out.push_back(R.trace);
    out.push_back(R.rho);
    out.push_back(R.lndet);
    out.push_back(R.cond);
    out.push_back(static_cast<double>(R.f));
    out.push_back(static_cast<double>(R.n_obs));
    return out;
}

// ---- raw bit dump ------------------------------------------------------------

static void dump(const std::vector<double>& v, const std::string& path) {
    std::FILE* fp = native_fopen(path, "wb");
    if (!fp) { std::perror(("fopen " + path).c_str()); _exit(3); }
    const uint64_t nn = v.size();
    if (std::fwrite(&nn, 1, sizeof nn, fp) != sizeof nn) { std::perror("fwrite"); _exit(4); }
    for (double d : v)
        if (std::fwrite(&d, 1, sizeof d, fp) != sizeof d) { std::perror("fwrite"); _exit(4); }
    if (std::fclose(fp) != 0) { std::perror("fclose"); _exit(5); }
}

// AER scope-2 B2 arm (2026-09-08): oaxaca_fit is three OLS fits, three influence
// stacks and a delta product -- serial by construction today, like pca. The
// arm exists so that a later patch that parallelises the per-group fits or the
// N x N score products must keep the reduction DAG a function of the data only.
// n = 40000 rows, two unequal groups (3:1), five regressors.
static std::vector<double> case_oaxaca() {
    const size_t N = 40000;
    const int p = 5;
    Lcg r(0x0a7aca5eULL);
    DesignMatrix dm;
    dm.n = static_cast<int64_t>(N);
    dm.k = p + 1;
    dm.xnames = {"x1", "x2", "x3", "x4", "x5", "_cons"};
    dm.X.assign(N * static_cast<size_t>(p + 1), 0.0);
    dm.y.resize(N);
    std::vector<int> g(N);
    for (size_t i = 0; i < N; ++i) {
        g[i] = (r.next() < 0.75) ? 0 : 1;
        double y = 0.5 + 0.3 * g[i];
        for (int j = 0; j < p; ++j) {
            const double x = r.next() * 2.0 - 1.0 + 0.4 * g[i] * j;
            dm.X[i + static_cast<size_t>(j) * N] = x;
            y += (0.2 * (j + 1) - 0.15 * g[i] * j) * x;
        }
        dm.X[i + static_cast<size_t>(p) * N] = 1.0;
        dm.y[i] = y + (r.next() - 0.5) * (1.0 + 0.5 * g[i]);
    }
    OaxacaResult R = oaxaca_fit(dm, g, true);
    std::vector<double> out(R.beta.begin(), R.beta.end());
    out.insert(out.end(), R.V.begin(), R.V.end());
    return out;
}

// AER scope-2 B2-4 arm (2026-09-09): nnmatch_fit is an O(n^2) distance sweep, a
// tolerance-class walk and two reductions -- serial by construction today, like
// pca and oaxaca. This arm exists so that a later patch which parallelises the
// per-i sweep must keep the reduction DAG a function of the DATA only: the
// match sets are decided by a RELATIVE tolerance of 1.5e-8 on the distance, so
// a reassociated moment sum does not merely move the low bits of e(V) -- it can
// change WHICH ids come back, which is what paper 076 consumes.
// n = 4000 rows, 4 continuous columns, 1:3 treated, nneighbor(2).
static std::vector<double> case_nnmatch() {
    const size_t N = 4000;
    const int p = 4;
    Lcg r(0x11a7c4ULL);
    std::vector<std::vector<double>> col(static_cast<size_t>(p), std::vector<double>(N));
    std::vector<double> y(N), t(N);
    for (size_t i = 0; i < N; ++i) {
        t[i] = (r.next() < 0.25) ? 1.0 : 0.0;
        double yy = 0.4 + 0.2 * t[i];
        for (int j = 0; j < p; ++j) {
            const double x = r.next() * 2.0 - 1.0 + 0.3 * t[i] * j;
            col[static_cast<size_t>(j)][i] = x;
            yy += (0.25 * (j + 1)) * x;
        }
        y[i] = yy + (r.next() - 0.5);
    }
    std::vector<std::string> names;
    std::vector<const double*> cols;
    for (int j = 0; j < p; ++j) {
        names.push_back("v" + std::to_string(j));
        cols.push_back(col[static_cast<size_t>(j)].data());
    }
    std::vector<char> keep(N, 1);
    NnmatchOpts o;
    o.nneighbor = 2;
    NnmatchResult R = nnmatch_fit(y.data(), t.data(), names, cols, {}, {}, keep, o);
    std::vector<double> out{R.tau, R.V, static_cast<double>(R.k_nnmin),
                            static_cast<double>(R.k_nnmax)};
    out.insert(out.end(), R.te.begin(), R.te.end());
    out.insert(out.end(), R.po0.begin(), R.po0.end());
    out.insert(out.end(), R.po1.begin(), R.po1.end());
    for (const auto& ids : R.omega)
        for (int64_t id : ids) out.push_back(static_cast<double>(id));
    return out;
}

struct Case { const char* tag; std::vector<double> (*run)(); };

static const std::vector<Case>& all_cases() {
    static const std::vector<Case> cases = {
        {"demean", case_demean},
        {"xtlogit", case_xtlogit},
        {"wildboot", case_wildboot},
        {"hpfilter", case_hpfilter},
        {"lpoly", case_lpoly},
        {"pca", case_pca},
        {"oaxaca", case_oaxaca},
        {"nnmatch", case_nnmatch},
    };
    return cases;
}

static std::string path_for(const std::string& base, const char* tag, int nthr) {
    return base + "_" + tag + "_" + std::to_string(nthr) + ".bin";
}

static bool files_equal(const std::string& a, const std::string& b) {
    std::FILE* fa = native_fopen(a, "rb");
    std::FILE* fb = native_fopen(b, "rb");
    if (!fa || !fb) { if (fa) std::fclose(fa); if (fb) std::fclose(fb); return false; }
    bool eq = true;
    for (;;) {
        char ba[65536], bb[65536];
        size_t na = std::fread(ba, 1, sizeof ba, fa);
        size_t nb = std::fread(bb, 1, sizeof bb, fb);
        if (na != nb) { eq = false; break; }
        if (na == 0) break;
        if (std::memcmp(ba, bb, na) != 0) { eq = false; break; }
    }
    std::fclose(fa);
    std::fclose(fb);
    return eq;
}

static bool file_nonempty(const std::string& p) {
    std::FILE* f = native_fopen(p, "rb");
    if (!f) return false;
    char b[16];
    const size_t n = std::fread(b, 1, sizeof b, f);
    std::fclose(f);
    return n > 0;
}

static int run_child(int nthr, const std::string& base) {
#if defined(_WIN32)
    ::_putenv_s("CECON_NUM_THREADS", std::to_string(nthr).c_str());
#else
    setenv("CECON_NUM_THREADS", std::to_string(nthr).c_str(), 1);
#endif
    // Anti-vacuity: prove the child really got the worker count it was asked for.
    // Without this a clamp (affinity, cgroup quota) could silently give BOTH arms
    // one worker, and every byte comparison below would pass while proving nothing.
    if (default_threads() != nthr) {
        std::fprintf(stderr, "child(%d): default_threads()=%d — env not honored\n", nthr,
                     default_threads());
        return 6;
    }
    // Anti-vacuity (2026-09-02, settings-derived fan-out): the two parallel_for
    // estimator sites now size their fan-out through threads_for_work (work
    // threshold + memory budget), so a case below the threshold would run BOTH
    // arms serially and this test would compare nothing. Ask the estimators'
    // own resolvers, with this file's case sizes, whether the multi-thread arm
    // really fans out. (The demean case is ThreadPool-sized and unaffected.)
    if (nthr > 1 && (xtlogit_fanout_threads(400, 6, 2) < 2 || wildboot_fanout_threads(999, 24) < 2)) {
        std::fprintf(stderr, "child(%d): an estimator case is below its fan-out threshold — vacuous\n",
                     nthr);
        return 7;
    }
    try {
        for (const Case& c : all_cases()) dump(c.run(), path_for(base, c.tag, nthr));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "child(%d) threw: %s\n", nthr, e.what());
        return 2;
    }
    return 0;
}

#if defined(_WIN32)
int main(int argc, char** argv) {
    if (argc == 4 && std::strcmp(argv[1], "--child") == 0)
        return run_child(std::atoi(argv[2]), argv[3]);

    std::printf("test_thread_determinism:\n");
    const std::string base =
        "./thread_det_" + std::to_string(static_cast<long>(native_process_id()));
    const std::wstring wbase = to_native_path(base).wstring();

    wchar_t exe[32768];
    constexpr DWORD exe_cap = static_cast<DWORD>(sizeof(exe) / sizeof(exe[0]));
    const DWORD exe_len = ::GetModuleFileNameW(nullptr, exe, exe_cap);
    auto spawn = [&](int nthr) -> int {
        if (exe_len == 0 || exe_len >= exe_cap) return -1;
        const std::wstring wn = std::to_wstring(nthr);
        const intptr_t rc = ::_wspawnl(_P_WAIT, exe, exe, L"--child", wn.c_str(), wbase.c_str(),
                                       static_cast<wchar_t*>(nullptr));
        return rc < 0 ? -1 : static_cast<int>(rc);
    };
    const int st1 = spawn(1);
    const int st4 = spawn(4);
    check(st1 == 0, "child CECON_NUM_THREADS=1 exited cleanly");
    check(st4 == 0, "child CECON_NUM_THREADS=4 exited cleanly");
    if (st1 == 0 && st4 == 0) {
        for (const Case& c : all_cases()) {
            const std::string a = path_for(base, c.tag, 1), b = path_for(base, c.tag, 4);
            check(file_nonempty(a), std::string(c.tag) + ": 1-worker arm produced output");
            check(files_equal(a, b),
                  std::string(c.tag) + ": output byte-identical across 1 vs 4 workers");
        }
    }
    for (const Case& c : all_cases()) {
        native_unlink(path_for(base, c.tag, 1));
        native_unlink(path_for(base, c.tag, 4));
    }
    std::printf("%s\n", fail ? "FAILED" : "PASSED");
    return fail;
}
#else
int main() {
    std::printf("test_thread_determinism:\n");
    const std::string base =
        "./thread_det_" + std::to_string(static_cast<long>(native_process_id()));

    // Nothing thread-related runs before the fork, so each child initializes the pool
    // fresh from its own CECON_NUM_THREADS (first-call-cached singleton).
    auto spawn = [&](int nthr) -> pid_t {
        pid_t pid = fork();
        if (pid < 0) { std::perror("fork"); return -1; }
        if (pid == 0) _exit(run_child(nthr, base));
        return pid;
    };
    pid_t p1 = spawn(1);
    pid_t p4 = spawn(4);
    check(p1 > 0 && p4 > 0, "forked both worker-count children");

    int st1 = 0, st4 = 0;
    if (p1 > 0) waitpid(p1, &st1, 0);
    if (p4 > 0) waitpid(p4, &st4, 0);
    const bool ok1 = p1 > 0 && WIFEXITED(st1) && WEXITSTATUS(st1) == 0;
    const bool ok4 = p4 > 0 && WIFEXITED(st4) && WEXITSTATUS(st4) == 0;
    check(ok1, "child CECON_NUM_THREADS=1 exited cleanly");
    check(ok4, "child CECON_NUM_THREADS=4 exited cleanly");

    if (ok1 && ok4) {
        for (const Case& c : all_cases()) {
            const std::string a = path_for(base, c.tag, 1), b = path_for(base, c.tag, 4);
            check(file_nonempty(a), std::string(c.tag) + ": 1-worker arm produced output");
            check(files_equal(a, b),
                  std::string(c.tag) + ": output byte-identical across 1 vs 4 workers");
        }
    }

    for (const Case& c : all_cases()) {
        native_unlink(path_for(base, c.tag, 1));
        native_unlink(path_for(base, c.tag, 4));
    }
    std::printf("%s\n", fail ? "FAILED" : "PASSED");
    return fail;
}
#endif
