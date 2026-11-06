// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// P7 wave — miimpute oracle assertions (Stata 19.5,
// internal capture p7_stata.log, the
// `mi set mlong / mi register imputed x2 / mi impute regress x2 = x1 y,
// add(2) rseed(20260802) noisily` block of p7.do).
//
// WHAT IS ASSERTED AGAINST STATA AND WHAT IS NOT. Only the DETERMINISTIC half of
// `mi impute regress` has an oracle: the inner regression over the complete cases
// and the accounting box. The imputed VALUES are cobra's own draws (P6 RNG ruling
// — a fixed seed makes OUR stream reproducible; correctness is judged on the
// mathematics), so they are asserted against the mathematics instead:
// order-independence, bit-identical reproducibility, seed sensitivity, and a
// distributional check against what the fitted model implies. Nothing here
// compares a drawn number to a Stata number.
//
// FIXTURE. The oracle leg needs internal capture
// wave_fixture.dta, which is a generated wave artifact and is GITIGNORED. It is
// resolved from COBRA_PANGRAM_ARTIFACTS when that is set, else relative to the
// validate directory; when it is absent the oracle leg SKIPS LOUDLY and says so.
// Every RNG and algebra leg below runs on a self-contained synthetic design and
// is therefore never skipped.
//
// Tolerances follow the wave rule — coefficients rtol 1e-6, standard errors
// rtol 1e-5 — widened, per assertion, to half of the last digit Stata actually
// PRINTED, because that rounding (not our arithmetic) is the binding uncertainty.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "core/rng_normal.h"
#include "estimators/miimpute.h"
#include "estimators/posthest.h"
#include "io/dta_reader.h"
#include "validate_dir.h"

using namespace cecon;

static int fail_count = 0;
static void check(bool cond, const std::string& msg) {
    if (!cond) {
        std::printf("  FAIL: %s\n", msg.c_str());
        fail_count = 1;
    }
}

// `o` is the value Stata PRINTED; `unit` is that print's last-digit unit.
static void near(double c, double o, double rtol, double unit, const std::string& what) {
    const double tol = std::max(rtol * std::fabs(o), 0.5 * unit);
    if (!(std::fabs(c - o) <= tol)) {
        std::printf("  FAIL: %s — got %.10g, oracle %.10g (|d| %.3g > tol %.3g)\n", what.c_str(), c,
                    o, std::fabs(c - o), tol);
        fail_count = 1;
    }
}

namespace {

// Standard-normal CDF, split so BOTH tails are computed in their accurate
// direction — 0.5*erfc(-z/sqrt2) loses everything to cancellation for large
// positive z, which would make the round-trip test measure erfc, not AS241.
double phi_lower(double z) { return 0.5 * std::erfc(-z * 0.7071067811865476); }
double phi_upper(double z) { return 0.5 * std::erfc(z * 0.7071067811865476); }

// A deterministic synthetic problem, built with the engine's OWN draws used as a
// data generator (seed 7, a stream no assertion below reads), so the legs that
// need a design but not the oracle never depend on a gitignored artifact.
struct Synth {
    int64_t nf = 0, ni = 0;
    int k = 3;
    std::vector<double> Xf, yf, Xi;
};

Synth make_synth(int64_t nf, int64_t ni) {
    Synth s;
    s.nf = nf;
    s.ni = ni;
    s.Xf.assign(static_cast<size_t>(nf) * 3, 1.0);
    s.Xi.assign(static_cast<size_t>(ni) * 3, 1.0);
    s.yf.assign(static_cast<size_t>(nf), 0.0);
    for (int64_t i = 0; i < nf; ++i) {
        const double a = normal_draw(7, 0, static_cast<uint64_t>(i));
        const double b = normal_draw(7, 1, static_cast<uint64_t>(i));
        s.Xf[i] = a;
        s.Xf[nf + i] = 0.4 * a + b;
        s.yf[static_cast<size_t>(i)] =
            1.5 + 0.8 * a - 0.3 * s.Xf[nf + i] + 2.0 * normal_draw(7, 2, static_cast<uint64_t>(i));
    }
    for (int64_t i = 0; i < ni; ++i) {
        const double a = normal_draw(7, 3, static_cast<uint64_t>(i));
        s.Xi[i] = a;
        s.Xi[ni + i] = 0.4 * a + normal_draw(7, 4, static_cast<uint64_t>(i));
    }
    return s;
}

}  // namespace

int test_miimpute_main() {
    std::printf("test_miimpute:\n");

    // ---- 1. MANDATORY: AS241 accuracy ---------------------------------------
    // The normal quantile is round-tripped through erfc — an INDEPENDENT inverse,
    // not a rearrangement of the same polynomial — at a spread of probabilities
    // that crosses both of AS241's branch seams (|q| = 0.425, i.e. p = .075/.925,
    // and r = 5, i.e. p ~ 1.4e-6) and reaches further into the tail than any
    // uniform this RNG can produce (uniform01's floor is 2^-54 ~ 5.5e-17).
    {
        static const double ps[] = {1e-17, 1e-15, 1e-12, 1.4e-6,  1e-6,   1e-4,   0.001,
                                    0.01,  0.02,  0.05,  0.074,   0.075,  0.076,  0.1,
                                    0.25,  0.4,   0.49,  0.5,     0.51,   0.6,    0.75,
                                    0.9,   0.924, 0.925, 0.926,   0.99,   0.999,  1 - 1e-6,
                                    1 - 1e-12, 1 - 1e-15};
        double worst = 0;
        for (double p : ps) {
            const double z = normal_icdf(p);
            // Round-trip in whichever tail is computed without cancellation.
            const double back = (p <= 0.5) ? phi_lower(z) : phi_upper(z);
            const double target = (p <= 0.5) ? p : (1.0 - p);
            const double rel = std::fabs(back - target) / target;
            if (rel > worst) worst = rel;
            check(rel < 1e-13, "AS241: erfc round-trip at p = " + std::to_string(p) +
                                   " (rel " + std::to_string(rel) + ")");
        }
        // Odd symmetry: the two tail branches must agree with each other. Only
        // checkable where the COMPLEMENT is well conditioned — for p = 1e-10 the
        // double 1-p has already thrown away six digits of p, so 1-(1-p) is not p
        // and the disagreement that produces is floating-point representation,
        // not AS241. (Measured: 2.5e-8 relative at p = 1e-10, entirely explained
        // by dz = dp/phi(z) with dp/p ~ 1e-6.) uniform01 never forms a probability
        // as 1-u, so nothing in the draw path depends on that regime.
        for (double p : {1e-3, 0.01, 0.05, 0.2, 0.45}) {
            const double a = normal_icdf(p), b = normal_icdf(1.0 - p);
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%g", p);
            check(std::fabs(a + b) <= 1e-12 * std::fabs(a),
                  std::string("AS241: normal_icdf(p) == -normal_icdf(1-p) at p = ") + buf);
        }
        check(normal_icdf(0.5) == 0.0, "AS241: normal_icdf(0.5) is exactly 0");
        std::printf("  AS241: worst erfc round-trip relative error %.3g over %zu probabilities\n",
                    worst, sizeof(ps) / sizeof(ps[0]));
        // seeded-RED: the assertion is sharp enough to see a WRONG quantile. A
        // 1e-9 relative nudge of z (a thousand times looser than AS241's claim)
        // must break the 1e-13 round-trip — otherwise the leg above proves nothing.
        {
            const double p = 0.01, z = normal_icdf(p) * (1.0 + 1e-9);
            check(std::fabs(phi_lower(z) - p) / p > 1e-13,
                  "AS241: the round-trip tolerance DISTINGUISHES a 1e-9 perturbation of z "
                  "(so the leg above is a measurement, not a formality)");
        }
    }

    // ---- 2. MANDATORY: chi-square inverse round-trip through gammp ----------
    {
        double worst = 0;
        for (double nu : {1.0, 2.0, 3.0, 10.0, 100.0, 1000.0, 4646.0}) {
            for (double p : {1e-6, 1e-3, 0.01, 0.1, 0.25, 0.5, 0.75, 0.9, 0.99, 0.999, 1 - 1e-6}) {
                const double x = chi2_icdf(p, nu);
                const double back = gammp(nu / 2.0, x / 2.0);  // == chi2_cdf(x, nu)
                const double rel = std::fabs(back - p) / p;
                if (rel > worst) worst = rel;
                check(rel < 1e-10, "chi2_icdf: gammp round-trip at nu = " + std::to_string(nu) +
                                       ", p = " + std::to_string(p) + " (rel " + std::to_string(rel) + ")");
            }
        }
        std::printf("  chi2_icdf: worst gammp round-trip relative error %.3g\n", worst);
        // The nu the P7 imputer actually uses, against its own moments: E = nu,
        // Var = 2nu, so the median must sit just below nu and the quantile
        // function must be monotone across the whole grid.
        double prev = -1;
        for (double p : {1e-6, 1e-3, 0.01, 0.1, 0.5, 0.9, 0.99, 1 - 1e-6}) {
            const double x = chi2_icdf(p, 4646.0);
            check(x > prev, "chi2_icdf(nu = 4646) is monotone in p at p = " + std::to_string(p));
            prev = x;
        }
        const double med = chi2_icdf(0.5, 4646.0);
        check(med < 4646.0 && med > 4646.0 - 3.0,
              "chi2_icdf: median of chi2(4646) is just below the mean (got " + std::to_string(med) + ")");
    }

    // ---- 3-6. the draw's properties, on a self-contained synthetic design ---
    const Synth sy = make_synth(600, 200);
    MiImputeResult base = mi_impute_regress(sy.Xf.data(), sy.yf.data(), sy.nf, sy.k, sy.Xi.data(),
                                            sy.ni, 3, 20260802);
    check(base.ok, "mi_impute_regress: the synthetic fit succeeds");
    if (!base.ok) { std::printf("  (synthetic fit failed: %s)\n", base.err.c_str()); return 1; }

    // 3. MANDATORY: ORDER INDEPENDENCE. Drawing imputation m = 2 alone must be
    // BIT-identical to drawing 1..3 and keeping the second. This is the whole
    // reason the counter layout exists, so it is asserted with `==`, not a
    // tolerance — a sequential stream (Box-Muller pairing, rejection sampling)
    // could not pass it.
    {
        std::vector<double> vals, bstar;
        double sig = 0;
        const bool ok = mi_impute_draw(base, sy.Xi.data(), sy.ni, 2, 20260802, &vals, &sig, &bstar);
        check(ok, "mi_impute_draw: the standalone m = 2 draw succeeds");
        int64_t diff = 0;
        for (int64_t j = 0; j < sy.ni; ++j)
            if (vals[static_cast<size_t>(j)] != base.values[static_cast<size_t>(sy.ni) + static_cast<size_t>(j)])
                ++diff;
        check(diff == 0, "order independence: m = 2 drawn ALONE is bit-identical to m = 2 drawn "
                         "after m = 1 (" + std::to_string((long long)diff) + " differing values)");
        check(sig == base.sigma_star[1], "order independence: sigma* for m = 2 is bit-identical");
        for (int c = 0; c < sy.k; ++c)
            check(bstar[static_cast<size_t>(c)] ==
                      base.beta_star[static_cast<size_t>(sy.k) + static_cast<size_t>(c)],
                  "order independence: beta*[" + std::to_string(c) + "] for m = 2 is bit-identical");
        // ... and an add(1) RUN gives the same m = 1 as an add(3) run, i.e. the
        // property holds through the full entry point, not just the inner helper.
        MiImputeResult one = mi_impute_regress(sy.Xf.data(), sy.yf.data(), sy.nf, sy.k,
                                               sy.Xi.data(), sy.ni, 1, 20260802);
        check(one.ok && one.values.size() == static_cast<size_t>(sy.ni), "add(1) run succeeds");
        int64_t d1 = 0;
        for (int64_t j = 0; j < sy.ni; ++j)
            if (one.values[static_cast<size_t>(j)] != base.values[static_cast<size_t>(j)]) ++d1;
        check(d1 == 0, "order independence: add(1) reproduces add(3)'s m = 1 exactly");
        // seeded-RED: m = 1 and m = 2 must NOT be the same numbers — otherwise
        // "bit-identical" above would be trivially satisfiable by a constant.
        int64_t same = 0;
        for (int64_t j = 0; j < sy.ni; ++j)
            if (base.values[static_cast<size_t>(j)] ==
                base.values[static_cast<size_t>(sy.ni) + static_cast<size_t>(j)])
                ++same;
        check(same == 0, "the imputations are genuinely different draws (m = 1 vs m = 2 share " +
                             std::to_string((long long)same) + " values)");
    }

    // 4. MANDATORY: bit-identical reproducibility across runs.
    {
        MiImputeResult again = mi_impute_regress(sy.Xf.data(), sy.yf.data(), sy.nf, sy.k,
                                                 sy.Xi.data(), sy.ni, 3, 20260802);
        check(again.ok && again.values.size() == base.values.size(), "the repeat run succeeds");
        int64_t diff = 0;
        for (size_t i = 0; i < base.values.size(); ++i)
            if (again.values[i] != base.values[i]) ++diff;
        check(diff == 0, "reproducibility: two runs with seed 20260802 are bit-identical (" +
                             std::to_string((long long)diff) + " differing values)");
    }

    // 5. MANDATORY: seed sensitivity — the seed is actually wired to the draw.
    {
        MiImputeResult other = mi_impute_regress(sy.Xf.data(), sy.yf.data(), sy.nf, sy.k,
                                                 sy.Xi.data(), sy.ni, 3, 20260803);
        check(other.ok, "the different-seed run succeeds");
        int64_t same = 0;
        for (size_t i = 0; i < base.values.size(); ++i)
            if (other.values[i] == base.values[i]) ++same;
        check(same == 0, "seed sensitivity: seed 20260803 shares NO value with seed 20260802 (" +
                             std::to_string((long long)same) + " shared)");
        // The fitted model is seed-INDEPENDENT (only the draws move) — a seed that
        // leaked into the fit would be a much worse bug than one that did not reach
        // the draw at all.
        for (int c = 0; c < sy.k; ++c)
            check(other.beta[static_cast<size_t>(c)] == base.beta[static_cast<size_t>(c)],
                  "seed sensitivity: the FITTED beta is untouched by the seed");
        check(other.rss == base.rss, "seed sensitivity: the fitted RSS is untouched by the seed");
    }

    // 6. MANDATORY: distributional sanity. Conditional on the drawn (beta*,
    // sigma*) that the result reports, y*_j - x_j'beta* is exactly sigma* eps_j
    // with eps ~ N(0,1) — so the standardized residuals must have mean 0 with
    // s.e. 1/sqrt(N) and s.d. 1 with s.e. 1/sqrt(2N). Run over MANY imputations
    // so the check is sharp (N = 200 * 400 = 80,000 draws, s.e. 0.0035), then
    // repeated on a SINGLE imputation at the 129-value scale the oracle uses.
    {
        const int M = 400;
        MiImputeResult big = mi_impute_regress(sy.Xf.data(), sy.yf.data(), sy.nf, sy.k,
                                               sy.Xi.data(), sy.ni, M, 20260802);
        check(big.ok, "the 400-imputation run succeeds");
        double sum = 0, sumsq = 0;
        int64_t N = 0;
        for (int m = 0; m < M; ++m) {
            const double sig = big.sigma_star[static_cast<size_t>(m)];
            for (int64_t j = 0; j < sy.ni; ++j) {
                double xb = 0;
                for (int c = 0; c < sy.k; ++c)
                    xb += sy.Xi[static_cast<int64_t>(c) * sy.ni + j] *
                          big.beta_star[static_cast<size_t>(m) * static_cast<size_t>(sy.k) + static_cast<size_t>(c)];
                const double e =
                    (big.values[static_cast<size_t>(m) * static_cast<size_t>(sy.ni) + static_cast<size_t>(j)] - xb) / sig;
                sum += e;
                sumsq += e * e;
                ++N;
            }
        }
        const double mean = sum / static_cast<double>(N);
        const double sd = std::sqrt(sumsq / static_cast<double>(N) - mean * mean);
        const double se_mean = 1.0 / std::sqrt(static_cast<double>(N));
        const double se_sd = 1.0 / std::sqrt(2.0 * static_cast<double>(N));
        std::printf("  draws: %lld standardized residuals, mean %+.5f (4 s.e. = %.5f), "
                    "sd %.5f (4 s.e. = %.5f)\n",
                    (long long)N, mean, 4 * se_mean, sd, 4 * se_sd);
        check(std::fabs(mean) < 4 * se_mean,
              "distribution: the standardized residuals have mean 0 to 4 s.e.");
        check(std::fabs(sd - 1.0) < 4 * se_sd,
              "distribution: the standardized residuals have s.d. 1 to 4 s.e.");
        // sigma*^2 = RSS/chi2(nu) has mean RSS/(nu-2); with nu = 597 the ratio
        // E[sigma*^2] / s^2 = nu/(nu-2) = 1.0034, and 400 draws give the mean a
        // relative s.e. of sqrt(2/nu)/sqrt(400) = 0.0029.
        double s2sum = 0;
        for (int m = 0; m < M; ++m)
            s2sum += big.sigma_star[static_cast<size_t>(m)] * big.sigma_star[static_cast<size_t>(m)];
        const double nu = static_cast<double>(base.df_resid);
        const double s2hat = s2sum / M;
        const double s2 = base.rss / nu;
        const double expect = s2 * nu / (nu - 2.0);
        const double se = expect * std::sqrt(2.0 / nu) / std::sqrt(static_cast<double>(M));
        std::printf("  draws: mean sigma*^2 %.6f, model implies %.6f (4 s.e. = %.6f)\n", s2hat,
                    expect, 4 * se);
        check(std::fabs(s2hat - expect) < 4 * se,
              "distribution: E[sigma*^2] = RSS/(nu-2) to 4 s.e. (the chi-square draw is the "
              "POSTERIOR of sigma^2, not the point estimate)");
    }

    // ---- 7. MANDATORY: the oracle ------------------------------------------
    // Every deterministic number Stata printed. Needs the (gitignored) wave
    // artifact; skips LOUDLY when it is absent.
    {
        std::string dta;
        if (const char* env = std::getenv("COBRA_PANGRAM_ARTIFACTS")) {
            dta = env;
            if (!dta.empty() && dta.back() != '/') dta += '/';
            dta += "wave_fixture.dta";
        } else {
            const cobra_test::ValidateBase vb;
            dta = vb.str() + "../kitchensink/pangram/waves/_artifacts/wave_fixture.dta";
        }
        std::ifstream probe(dta.c_str(), std::ios::binary);
        if (!probe.good()) {
            std::printf(
                "\n"
                "  *********************************************************************\n"
                "  ** SKIPPED: the P7 mi-impute ORACLE leg did not run.               **\n"
                "  ** wave_fixture.dta was not found at                               **\n"
                "  **   %s\n"
                "  ** It is a GENERATED wave artifact and is gitignored. Set          **\n"
                "  ** COBRA_PANGRAM_ARTIFACTS to the directory that holds it (or      **\n"
                "  ** COBRA_VALIDATE_DIR so that ../kitchensink/... resolves) to      **\n"
                "  ** assert the 4,649-observation inner regression and the           **\n"
                "  ** 4,671 / 129 / 129 / 4,800 accounting box against Stata 19.5.    **\n"
                "  ** Everything above ran; NOTHING below was checked.                **\n"
                "  *********************************************************************\n",
                dta.c_str());
        } else {
            probe.close();
            DtaResult dr = read_dta(dta);
            const Frame& fr = dr.frame;
            const int64_t n = fr.n_rows();
            check(n == 4800, "fixture: 4,800 rows — got " + std::to_string((long long)n));
            const auto& c_x2 = fr.col("x2").f64;
            const auto& c_x1 = fr.col("x1").f64;
            const auto& c_y = fr.col("y").f64;
            const double* x2 = c_x2.data();
            const double* x1 = c_x1.data();
            const double* yv = c_y.data();

            // --- the accounting box -----------------------------------------
            // `mi register imputed x2` prints "(129 m=0 obs now marked as
            // incomplete)"; the per-imputation box prints
            //   x2 | 4671 complete, 129 incomplete, 129 imputed | 4800 total.
            MiSample s = mi_classify(x2, {x1, yv}, n);
            check(s.n_total == 4800, "mi box: 4,800 total");
            check(s.n_complete == 4671, "mi box: 4,671 complete — got " + std::to_string((long long)s.n_complete));
            check(s.n_incomplete == 129,
                  "mi register: 129 m=0 obs marked incomplete — got " + std::to_string((long long)s.n_incomplete));
            check(s.n_imputable == 129, "mi box: 129 imputed — got " + std::to_string((long long)s.n_imputable));
            check(s.n_complete + s.n_incomplete == s.n_total,
                  "mi box: Complete + Incomplete = Total");
            check(static_cast<int64_t>(s.fit_rows.size()) == 4649,
                  "inner regression: N = 4,649 complete cases — got " +
                      std::to_string((long long)s.fit_rows.size()));
            // Stata's footnote: the model sample is SMALLER than the complete
            // count because 22 rows have a missing right-hand-side y. Asserted so
            // the "listwise deletion" note can never be a decorative string.
            check(s.n_complete - static_cast<int64_t>(s.fit_rows.size()) == 22,
                  "inner regression: 4,671 - 4,649 = 22 complete rows lost to a missing predictor");
            // The two missingness sets are DISJOINT, which is why all 129
            // incomplete rows are imputable (Imputed == Incomplete).
            int64_t both = 0;
            for (int64_t i = 0; i < n; ++i)
                if (std::isnan(x2[static_cast<size_t>(i)]) && std::isnan(yv[static_cast<size_t>(i)])) ++both;
            check(both == 0, "fixture: the x2- and y-missing sets are disjoint (" +
                                 std::to_string((long long)both) + " rows miss both)");

            // --- the inner regression Stata prints under `noisily` ------------
            const int64_t nf = static_cast<int64_t>(s.fit_rows.size());
            const int64_t ni = static_cast<int64_t>(s.impute_rows.size());
            const int k = 3;
            std::vector<double> Xf(static_cast<size_t>(nf) * 3, 1.0), yf(static_cast<size_t>(nf));
            std::vector<double> Xi(static_cast<size_t>(ni) * 3, 1.0);
            for (int64_t i = 0; i < nf; ++i) {
                const int64_t row = s.fit_rows[static_cast<size_t>(i)];
                Xf[i] = x1[static_cast<size_t>(row)];
                Xf[nf + i] = yv[static_cast<size_t>(row)];
                yf[static_cast<size_t>(i)] = x2[static_cast<size_t>(row)];
            }
            for (int64_t i = 0; i < ni; ++i) {
                const int64_t row = s.impute_rows[static_cast<size_t>(i)];
                Xi[i] = x1[static_cast<size_t>(row)];
                Xi[ni + i] = yv[static_cast<size_t>(row)];
            }
            MiImputeResult r = mi_impute_regress(Xf.data(), yf.data(), nf, k, Xi.data(), ni, 2, 20260802);
            check(r.ok, "mi impute regress: the fit succeeds");
            if (r.ok) {
                check(r.n_fit == 4649, "regress header: Number of obs = 4,649");
                check(r.df_model == 2 && r.df_resid == 4646, "regress header: F(2, 4646) degrees of freedom");
                near(r.mss, 59.3067149, 1e-6, 1e-7, "regress ANOVA: Model SS");
                near(r.rss, 2508.67153, 1e-6, 1e-5, "regress ANOVA: Residual SS");
                near(r.tss, 2567.97825, 1e-6, 1e-5, "regress ANOVA: Total SS");
                near(r.mss / 2.0, 29.6533574, 1e-6, 1e-7, "regress ANOVA: Model MS");
                near(r.rss / 4646.0, 0.539963739, 1e-6, 1e-9, "regress ANOVA: Residual MS");
                near(r.tss / 4648.0, 0.552491017, 1e-6, 1e-9, "regress ANOVA: Total MS");
                near(r.f_stat, 54.92, 1e-4, 1e-2, "regress header: F(2, 4646)");
                check(r.p_f < 5e-5, "regress header: Prob > F = 0.0000 (got " + std::to_string(r.p_f) + ")");
                near(r.r2, 0.0231, 1e-4, 1e-4, "regress header: R-squared");
                near(r.r2_adj, 0.0227, 1e-4, 1e-4, "regress header: Adj R-squared");
                near(r.rmse, 0.73482, 1e-6, 1e-5, "regress header: Root MSE");
                near(r.beta[0], 0.0945355, 1e-6, 1e-7, "regress: x1 coefficient");
                near(r.se[0], 0.0163812, 1e-5, 1e-7, "regress: x1 std. err.");
                near(r.beta[1], -0.1176004, 1e-6, 1e-7, "regress: y coefficient");
                near(r.se[1], 0.0112377, 1e-5, 1e-7, "regress: y std. err.");
                near(r.beta[2], 0.7398663, 1e-6, 1e-7, "regress: _cons");
                near(r.se[2], 0.0338241, 1e-5, 1e-7, "regress: _cons std. err.");
                // t = b/se, and the 95% CI uses the t(4646) critical value.
                near(r.beta[0] / r.se[0], 5.77, 1e-3, 1e-2, "regress: x1 t");
                near(r.beta[1] / r.se[1], -10.46, 1e-3, 1e-2, "regress: y t");
                near(r.beta[2] / r.se[2], 21.87, 1e-3, 1e-2, "regress: _cons t");
                const double tc = t_critical(0.05, 4646.0);
                near(r.beta[0] - tc * r.se[0], 0.0624205, 1e-5, 1e-7, "regress: x1 95% CI lower");
                near(r.beta[0] + tc * r.se[0], 0.1266504, 1e-5, 1e-7, "regress: x1 95% CI upper");
                near(r.beta[1] - tc * r.se[1], -0.1396317, 1e-5, 1e-7, "regress: y 95% CI lower");
                near(r.beta[1] + tc * r.se[1], -0.0955691, 1e-5, 1e-7, "regress: y 95% CI upper");
                near(r.beta[2] - tc * r.se[2], 0.6735549, 1e-5, 1e-7, "regress: _cons 95% CI lower");
                near(r.beta[2] + tc * r.se[2], 0.8061776, 1e-5, 1e-7, "regress: _cons 95% CI upper");
                check(r.n_imputed == 129, "mi box: 129 values filled in per imputation");
                check(r.add == 2 && r.updated == 0, "mi header: added = 2, updated = 0");
                // seeded-RED: the SE tolerance is sharp enough to see the WRONG
                // residual-variance divisor. s^2 = RSS/(n-k) with n-k = 4,646 is
                // what reproduces .0163812; RSS/n would give .0163759 — a 3.2e-4
                // relative gap, 30x the SE tolerance. Without this the leg above
                // could pass on an off-by-k degrees-of-freedom choice.
                {
                    const double se_wrong =
                        r.se[0] * std::sqrt(static_cast<double>(nf - k) / static_cast<double>(nf));
                    check(std::fabs(se_wrong - 0.0163812) > 1e-5 * 0.0163812,
                          "regress: RSS/n is DISTINGUISHABLE from RSS/(n-k) at the SE tolerance "
                          "(so the residual d.f. is pinned, not free)");
                }

                // The DRAWS on the real fixture — same three properties, at the
                // 129-value scale the oracle box describes. Values are ours; only
                // their mathematics is asserted.
                MiImputeResult again = mi_impute_regress(Xf.data(), yf.data(), nf, k, Xi.data(), ni, 2, 20260802);
                int64_t diff = 0;
                for (size_t i = 0; i < r.values.size(); ++i)
                    if (again.values[i] != r.values[i]) ++diff;
                check(diff == 0, "fixture draws: bit-identical across runs at seed 20260802");
                std::vector<double> v2;
                double sig2 = 0;
                std::vector<double> b2;
                mi_impute_draw(r, Xi.data(), ni, 2, 20260802, &v2, &sig2, &b2);
                int64_t od = 0;
                for (int64_t j = 0; j < ni; ++j)
                    if (v2[static_cast<size_t>(j)] != r.values[static_cast<size_t>(ni) + static_cast<size_t>(j)]) ++od;
                check(od == 0, "fixture draws: m = 2 alone == m = 2 after m = 1");
                // Distributional sanity over the 129 imputed values of m = 1.
                for (int m = 0; m < 2; ++m) {
                    const double sig = r.sigma_star[static_cast<size_t>(m)];
                    double sum = 0, sumsq = 0;
                    for (int64_t j = 0; j < ni; ++j) {
                        double xb = 0;
                        for (int c = 0; c < k; ++c)
                            xb += Xi[static_cast<int64_t>(c) * ni + j] *
                                  r.beta_star[static_cast<size_t>(m) * 3 + static_cast<size_t>(c)];
                        const double e =
                            (r.values[static_cast<size_t>(m) * static_cast<size_t>(ni) + static_cast<size_t>(j)] - xb) / sig;
                        sum += e;
                        sumsq += e * e;
                    }
                    const double mean = sum / static_cast<double>(ni);
                    const double sd = std::sqrt(sumsq / static_cast<double>(ni) - mean * mean);
                    const double se_mean = 1.0 / std::sqrt(static_cast<double>(ni));
                    const double se_sd = 1.0 / std::sqrt(2.0 * static_cast<double>(ni));
                    std::printf("  fixture m=%d: 129 standardized residuals, mean %+.4f (4 s.e. %.4f), "
                                "sd %.4f (4 s.e. %.4f); sigma* %.6f vs Root MSE %.6f\n",
                                m + 1, mean, 4 * se_mean, sd, 4 * se_sd, sig, r.rmse);
                    check(std::fabs(mean) < 4 * se_mean,
                          "fixture m=" + std::to_string(m + 1) + ": imputed values' mean matches the "
                          "fitted model to 4 s.e.");
                    check(std::fabs(sd - 1.0) < 4 * se_sd,
                          "fixture m=" + std::to_string(m + 1) + ": imputed values' s.d. matches "
                          "sigma* to 4 s.e.");
                    // sigma* is a DRAW, so it must be close to — and generally not
                    // equal to — the fitted Root MSE. nu = 4646 makes the posterior
                    // s.d. of sigma*/s equal 1/sqrt(2 nu) = 0.0104.
                    check(sig != r.rmse && std::fabs(sig / r.rmse - 1.0) < 6.0 / std::sqrt(2.0 * 4646.0),
                          "fixture m=" + std::to_string(m + 1) + ": sigma* is a draw around Root MSE "
                          "(ratio " + std::to_string(sig / r.rmse) + ")");
                }
            }
            std::printf("  oracle leg RAN against %s\n", dta.c_str());
        }
    }

    if (fail_count == 0) std::printf("  all miimpute assertions passed\n");
    return fail_count;
}
