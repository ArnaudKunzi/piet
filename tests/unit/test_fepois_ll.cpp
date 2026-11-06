// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// fepois log-likelihood family — e(ll) / e(ll_0) / e(r2_p) / e(chi2) / e(df_m)
// (contract A §35).
//
// These are the MECHANICAL invariants, pinned on the committed glmqp.hbin fixture
// so the gate needs no pangram artifact. The Stata-oracle values themselves
// (poisson ycount x1 x2 i.region, vce(cluster id) → e(ll) = -5961.354,
// e(chi2) = 252.19 / 6 df, e(r2_p) = 0.0108) are pinned separately, end-to-end
// through the CLI, by tests/validate/validate_wave_oracle.py.
//
// The load-bearing check here is the DEVIANCE IDENTITY:
//     ll  ==  ll_saturated - deviance/2,      ll_saturated = Σ[y·ln y - y - ln y!]
// ll is computed directly from the converged μ as Σ[y·ln μ - μ - ln y!], NOT backed
// out of the deviance — so the identity is an independent cross-check of the two
// code paths rather than a tautology. If a future change makes the deviance and the
// log-likelihood disagree about the fitted μ (a clamp, a weight, an off-by-one
// iteration), this is what catches it.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "core/model.h"
#include "estimators/fepois.h"
#include "io/columnar.h"
#include "validate_dir.h"

using namespace cecon;

static int fail = 0;

static void chk(const char* tag, double got, double want, double rtol) {
    const double rel = std::fabs(got - want) / (std::fabs(want) + 1e-30);
    std::printf("  %-34s got=%.12g want=%.12g rel=%.2e\n", tag, got, want, rel);
    if (!(rel <= rtol)) { std::printf("    <<FAIL\n"); fail = 1; }
}

static void chk_true(const char* tag, bool cond) {
    std::printf("  %-34s %s\n", tag, cond ? "ok" : "FAIL");
    if (!cond) fail = 1;
}

int test_fepois_ll_main() {
    std::printf("test_fepois_ll:\n");
    const cobra_test::ValidateBase base;
    Dataset ds = Dataset::open(base + "glmqp.hbin");

    // --- pooled fit (design carries an "(intercept)" column) ---------------------
    {
        ModelData md = build_model(ds, "visits_w", {"age_z", "edu_hi", "treated"}, {}, {});
        FepoisResult r = fepois_fit(md, Vcov::IID);
        const int64_t n = md.dm.n;

        // 1. deviance identity — the independent cross-check.
        double ll_sat = 0;
        for (int64_t i = 0; i < n; ++i) {
            const double y = md.dm.y[i];
            ll_sat += (y > 0 ? y * std::log(y) : 0.0) - y - std::lgamma(y + 1.0);
        }
        chk("ll == ll_sat - deviance/2", r.ll, ll_sat - r.deviance / 2.0, 1e-9);

        // 2. ll_0: with no offset the intercept-only MLE is μ ≡ ȳ, so the null
        //    log-likelihood has the closed form Σ[y·ln ȳ - ȳ - ln y!].
        double ybar = 0;
        for (int64_t i = 0; i < n; ++i) ybar += md.dm.y[i];
        ybar /= static_cast<double>(n);
        double ll0_closed = 0;
        for (int64_t i = 0; i < n; ++i) {
            const double y = md.dm.y[i];
            ll0_closed += y * std::log(ybar) - ybar - std::lgamma(y + 1.0);
        }
        chk("ll_0 == closed-form mu==ybar", r.ll_0, ll0_closed, 1e-9);

        // 3. the fitted model must not be worse than the null, and r2_p is McFadden.
        chk_true("ll >= ll_0 (fit beats the null)", r.ll >= r.ll_0);
        chk("r2_p == 1 - ll/ll_0", r.r2_p, 1.0 - r.ll / r.ll_0, 1e-12);
        chk_true("0 <= r2_p < 1", r.r2_p >= 0.0 && r.r2_p < 1.0);

        // 4. iid VCE ⇒ Stata reports the LIKELIHOOD-RATIO statistic.
        chk("iid chi2 == 2(ll - ll_0)", r.chi2, 2.0 * (r.ll - r.ll_0), 1e-12);
        // df_m counts the NON-intercept coefficients: 3 slopes here, of 4 columns.
        chk_true("pooled design carries (intercept)",
                 std::find(r.names.begin(), r.names.end(), std::string("(intercept)")) != r.names.end());
        chk_true("chi2_df == 3 (k=4 minus the intercept)", r.chi2_df == 3);
        chk_true("chi2 > 0", r.chi2 > 0.0);
    }

    // --- same design, robust VCE ⇒ Stata reports a WALD statistic ----------------
    {
        ModelData md = build_model(ds, "visits_w", {"age_z", "edu_hi", "treated"}, {}, {});
        FepoisResult r = fepois_fit(md, Vcov::HC1);
        // ll / ll_0 / r2_p depend only on the fit, not on the VCE, so they must be
        // unchanged; chi2 must NOT be the LR any more.
        chk_true("robust chi2_df still 3", r.chi2_df == 3);
        chk_true("robust chi2 > 0", r.chi2 > 0.0);
        const double lr = 2.0 * (r.ll - r.ll_0);
        chk_true("robust chi2 != LR (Wald was used)",
                 std::fabs(r.chi2 - lr) / (std::fabs(lr) + 1e-30) > 1e-9);

        // Hand-rolled Wald over the non-intercept block must reproduce it exactly.
        std::vector<int> slope;
        for (int j = 0; j < r.k; ++j)
            if (r.names[static_cast<size_t>(j)] != "(intercept)") slope.push_back(j);
        const int q = static_cast<int>(slope.size());
        std::vector<double> V(static_cast<size_t>(q) * q), b(static_cast<size_t>(q));
        for (int a = 0; a < q; ++a) {
            b[static_cast<size_t>(a)] = r.beta[static_cast<size_t>(slope[a])];
            for (int c = 0; c < q; ++c)
                V[static_cast<size_t>(a) + static_cast<size_t>(c) * q] =
                    r.vcov_matrix[static_cast<size_t>(slope[a]) + static_cast<size_t>(slope[c]) * r.k];
        }
        // 3x3 solve by Gauss-Jordan (independent of la::spd_inverse, on purpose).
        std::vector<double> M = V, I(static_cast<size_t>(q) * q, 0.0);
        for (int i = 0; i < q; ++i) I[static_cast<size_t>(i) + static_cast<size_t>(i) * q] = 1.0;
        for (int c = 0; c < q; ++c) {
            const double p = M[static_cast<size_t>(c) + static_cast<size_t>(c) * q];
            for (int j = 0; j < q; ++j) {
                M[static_cast<size_t>(c) + static_cast<size_t>(j) * q] /= p;
                I[static_cast<size_t>(c) + static_cast<size_t>(j) * q] /= p;
            }
            for (int rr = 0; rr < q; ++rr) {
                if (rr == c) continue;
                const double f = M[static_cast<size_t>(rr) + static_cast<size_t>(c) * q];
                for (int j = 0; j < q; ++j) {
                    M[static_cast<size_t>(rr) + static_cast<size_t>(j) * q] -=
                        f * M[static_cast<size_t>(c) + static_cast<size_t>(j) * q];
                    I[static_cast<size_t>(rr) + static_cast<size_t>(j) * q] -=
                        f * I[static_cast<size_t>(c) + static_cast<size_t>(j) * q];
                }
            }
        }
        double w = 0;
        for (int a = 0; a < q; ++a)
            for (int c = 0; c < q; ++c)
                w += b[static_cast<size_t>(a)] *
                     I[static_cast<size_t>(a) + static_cast<size_t>(c) * q] * b[static_cast<size_t>(c)];
        chk("robust chi2 == b'V^-1 b", r.chi2, w, 1e-8);
    }

    // --- absorbed FE: the null stays the PLAIN intercept-only model ---------------
    // Under absorb() the design has no intercept column at all (the FE absorb it),
    // so every column is a slope and chi2_df counts only those. ll_0 must NOT pick
    // up the fixed effects — Stata's `poisson` null is the bare constant.
    {
        ModelData mp = build_model(ds, "visits_w", {"age_z", "edu_hi", "treated"}, {}, {});
        FepoisResult rp = fepois_fit(mp, Vcov::IID);

        ModelData mf = build_model(ds, "visits_w", {"age_z", "edu_hi"}, {"treated"}, {});
        FepoisResult rf = fepois_fit(mf, Vcov::IID);
        chk_true("absorb design has no (intercept) column",
                 std::find(rf.names.begin(), rf.names.end(), std::string("(intercept)")) == rf.names.end());
        chk_true("absorb chi2_df == 2 (slopes only)", rf.chi2_df == 2);
        // `treated` is binary, so absorbing it is the same model as carrying it as a
        // dummy: the log-likelihood (and hence ll_0 and r2_p) must agree.
        chk("absorb ll == dummy-variable ll", rf.ll, rp.ll, 1e-9);
        chk("absorb ll_0 == dummy-variable ll_0", rf.ll_0, rp.ll_0, 1e-12);
        chk("absorb r2_p == dummy-variable r2_p", rf.r2_p, rp.r2_p, 1e-9);
    }

    std::printf("%s\n", fail ? "FAILED" : "PASSED");
    return fail;
}
