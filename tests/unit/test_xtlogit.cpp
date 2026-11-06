// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// xtlogit, fe (Chamberlain conditional logit) validated against Stata.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "estimators/xtlogit.h"
#include "io/columnar.h"
#include "validate_dir.h"

using namespace cecon;

static double parse_stata(const std::string& s) {
    if (s.empty() || s == ".") return std::nan("");
    if (s[0] == '.') return std::stod("0" + s);
    if (s.size() > 1 && s[0] == '-' && s[1] == '.') return -std::stod("0" + s.substr(1));
    return std::stod(s);
}

int test_xtlogit_main() {
    std::printf("test_xtlogit:\n");
    Dataset ds = Dataset::open(cobra_test::validate_path("csdid_input.hbin"));
    int64_t n = ds.n_rows();
    const double* h_pid = ds.col("h_pid").f64();
    const double* wv    = ds.col("wv").f64();
    const double* gvar  = ds.col("gvar").f64();
    const double* y     = ds.col("visits_w").f64();
    const double* age_z = ds.col("age_z").f64();
    const double* edu_hi= ds.col("edu_hi").f64();

    std::vector<int64_t> keep;
    for (int64_t i = 0; i < n; ++i) if (!std::isnan(y[i])) keep.push_back(i);
    int64_t N = static_cast<int64_t>(keep.size());

    // Match the do-file: visits_any = (visits_w > 0).
    const int k = 3;  // treated, age_z, edu_hi
    std::vector<double> X(static_cast<size_t>(N) * k);
    std::vector<double> y_use(static_cast<size_t>(N));
    std::vector<double> ivar(static_cast<size_t>(N));
    for (int64_t i = 0; i < N; ++i) {
        int64_t r = keep[static_cast<size_t>(i)];
        double tr = (gvar[r] > 0 && wv[r] >= gvar[r]) ? 1.0 : 0.0;
        X[0 * N + i] = tr;
        X[1 * N + i] = age_z[r];
        X[2 * N + i] = edu_hi[r];
        y_use[static_cast<size_t>(i)] = (y[r] > 0) ? 1.0 : 0.0;
        ivar[static_cast<size_t>(i)] = h_pid[r];
    }

    auto R = xtlogit_fe(X.data(), N, k, y_use.data(), ivar.data());
    std::printf("  conv=%d iters=%d  N=%lld Ng=%lld  drop_const=%lld drop_size=%lld  ll=%.6f\n",
                R.converged?1:0, R.iters,
                (long long)R.n_obs, (long long)R.n_groups,
                (long long)R.n_dropped_const, (long long)R.n_dropped_size,
                R.loglik);

    // Golden
    std::ifstream f(cobra_test::validate_path("xtlogit_fe_golden.csv"));
    std::string line; std::getline(f, line);
    std::vector<std::tuple<std::string,double,double>> gold;
    while (std::getline(f, line)) {
        std::stringstream ss(line);
        std::string nm, b, s;
        std::getline(ss, nm, ','); std::getline(ss, b, ','); std::getline(ss, s, ',');
        gold.push_back({nm, parse_stata(b), parse_stata(s)});
    }
    const char* names[] = {"treated", "age_z", "edu_hi"};
    double worst_b = 0, worst_se = 0;
    for (int c = 0; c < k; ++c) {
        double gb = 0, gse = 0;
        for (const auto& [nm, b, s] : gold) if (nm == names[c]) { gb = b; gse = s; }
        double rb  = std::fabs(R.beta[c]   - gb)  / (std::fabs(gb)  + 1e-12);
        double rs  = std::fabs(R.se_iid[c] - gse) / (std::fabs(gse) + 1e-12);
        std::printf("  %-8s: b=%+.10g (stata %+.10g rel %.2e)  se_iid=%.10g (stata %.10g rel %.2e)\n",
                    names[c], R.beta[c], gb, rb, R.se_iid[c], gse, rs);
        if (rb > worst_b)  worst_b = rb;
        if (rs > worst_se) worst_se = rs;
    }
    double exp_ll = -3509.21457018448;
    double rll = std::fabs(R.loglik - exp_ll) / std::fabs(exp_ll);
    std::printf("  ll rel=%.2e (cecon=%.6f stata=%.6f)\n", rll, R.loglik, exp_ll);

    int fail = 0;
    if (!R.converged) fail = 1;
    if (worst_b > 1e-6) fail = 1;
    if (worst_se > 1e-5) fail = 1;
    if (rll > 1e-8) fail = 1;

    // ---------------------------------------------------------------------------
    // BUG-135 regression leg — WIDE panels + an OFFSET covariate.
    //
    // The O(T·K·k²) moment recursion that replaced the O(T²K²) pairwise-q block has
    // two numerical failure modes that small panels cannot see, and both were live in
    // the first build of the rewrite (measured on the acceptance corpus, T up to 1117):
    //   1. an UNCENTERED covariate makes Cov(S) = E[SS'] − E[S]E[S'] a difference of
    //      huge nearly-equal numbers, so the Hessian loses every significant digit;
    //   2. weights shifted by max(xb) are all ≤ 1, so B[K] — a product of K of them —
    //      underflows to exactly 0 and E[S] = A[K]/B[K] becomes 0/0 = NaN.
    // Both surface only when T is large AND the covariate carries a big offset
    // relative to its within-panel spread. `elo ≈ 1688 ± 60` is exactly that shape.
    //
    // The oracle here needs no Stata: the conditional likelihood conditions on
    // K = Σ_t y_t, so adding a constant c to a covariate shifts Σ_t d_t x_t by the
    // constant K·c and cancels EXACTLY. β and SE are therefore invariant under an
    // offset — a property the estimator must satisfy identically, not approximately.
    // Run the same data centred and offset by +1700 and require agreement.
    {
        const int Gw = 6, Tw = 1200, kw = 2;
        const int64_t Nw = static_cast<int64_t>(Gw) * Tw;
        std::vector<double> Xc(static_cast<size_t>(Nw) * kw), Xo(static_cast<size_t>(Nw) * kw);
        std::vector<double> yw(static_cast<size_t>(Nw)), iw(static_cast<size_t>(Nw));
        uint64_t rng = 0x9E3779B97F4A7C15ull;   // fixed seed: deterministic across platforms
        auto next = [&rng]() {                  // splitmix64 → uniform [0,1)
            uint64_t z = (rng += 0x9E3779B97F4A7C15ull);
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            z = z ^ (z >> 31);
            return static_cast<double>(z >> 11) / 9007199254740992.0;
        };
        const double OFFSET = 1700.0;
        for (int g = 0; g < Gw; ++g) {
            for (int t = 0; t < Tw; ++t) {
                int64_t i = static_cast<int64_t>(g) * Tw + t;
                double z1 = 60.0 * (2.0 * next() - 1.0);   // spread ±60, like `elo`
                double z2 = 2.0 * next();
                Xc[0 * Nw + i] = z1;              Xc[1 * Nw + i] = z2;
                Xo[0 * Nw + i] = z1 + OFFSET;     Xo[1 * Nw + i] = z2;
                double eta = 0.025 * z1 + 0.30 * z2 + 0.5 * (2.0 * next() - 1.0);
                yw[static_cast<size_t>(i)] = (eta > 0.0) ? 1.0 : 0.0;
                iw[static_cast<size_t>(i)] = g;
            }
        }
        auto Rc = xtlogit_fe(Xc.data(), Nw, kw, yw.data(), iw.data());
        auto Ro = xtlogit_fe(Xo.data(), Nw, kw, yw.data(), iw.data());
        std::printf("  [BUG-135] wide panels G=%d T=%d: centred conv=%d iters=%d ll=%.10f | "
                    "offset(+%.0f) conv=%d iters=%d ll=%.10f\n",
                    Gw, Tw, Rc.converged ? 1 : 0, Rc.iters, Rc.loglik,
                    OFFSET, Ro.converged ? 1 : 0, Ro.iters, Ro.loglik);
        int wfail = 0;
        if (!Rc.converged || !Ro.converged) { std::printf("  [BUG-135] FAIL: did not converge\n"); wfail = 1; }
        if (static_cast<int>(Rc.beta.size()) < kw || static_cast<int>(Ro.beta.size()) < kw ||
            static_cast<int>(Rc.se_iid.size()) < kw || static_cast<int>(Ro.se_iid.size()) < kw) {
            std::printf("  [BUG-135] FAIL: no coefficients produced\n"); wfail = 1;
        } else {
            for (int c = 0; c < kw; ++c) {
                if (!std::isfinite(Ro.beta[c]) || !std::isfinite(Ro.se_iid[c])) {
                    std::printf("  [BUG-135] FAIL: non-finite estimate at c=%d\n", c); wfail = 1; continue;
                }
                double rb = std::fabs(Rc.beta[c]   - Ro.beta[c])   / (std::fabs(Rc.beta[c])   + 1e-12);
                double rs = std::fabs(Rc.se_iid[c] - Ro.se_iid[c]) / (std::fabs(Rc.se_iid[c]) + 1e-12);
                std::printf("  [BUG-135] x%d: b %+.12g vs %+.12g (rel %.2e)  se %.12g vs %.12g (rel %.2e)\n",
                            c, Rc.beta[c], Ro.beta[c], rb, Rc.se_iid[c], Ro.se_iid[c], rs);
                if (rb > 1e-8) wfail = 1;
                if (rs > 1e-7) wfail = 1;
            }
            double rll2 = std::fabs(Rc.loglik - Ro.loglik) / (std::fabs(Rc.loglik) + 1e-12);
            if (rll2 > 1e-12) { std::printf("  [BUG-135] FAIL: loglik not offset-invariant (rel %.2e)\n", rll2); wfail = 1; }
        }
        if (wfail) fail = 1;
    }

    std::printf("%s\n", fail ? "FAILED" : "PASSED");
    return fail;
}
