// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Unit tests for sdid (synthetic difference-in-differences, AAHIW21).
//
// The acceptance authority for the ESTIMATE is real Stata 19.5, captured in the
// P-QE answer key and reproduced here for the record. On the pangram wave
// fixture (internal fixture wave_fixture.dta) reduced
// to the 178 units with no missing outcome:
//
//     sdid y id t treated, vce(bootstrap) reps(20) seed(20260802)
//     ATT 1.33439   Std. Err. 0.10497   t 12.71   95% CI [1.12866, 1.54013]
//
// That leg is a CLI/data-file test, not a unit test — this file cannot open a
// .dta. What it CAN do, and does, is pin the properties that make that number
// trustworthy:
//   A. an analytic identity the estimator must satisfy exactly, on data whose
//      right answer is known without running the estimator;
//   B. a numeric regression pin on a fixed pseudo-random staggered panel;
//   C. the RED CERTIFICATE for the incomplete-rectangle guard — the arm that
//      fails if the guard is deleted;
//   D. the two oracle-pinned refusals (placebo needs more controls than
//      treated; unimplemented vce named rather than silently substituted);
//   E. fixed-seed determinism and the bootstrap's self-identity.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "estimators/sdid.h"

using namespace cecon;

static int g_fail = 0;

static void ok(bool cond, const char* what) {
    if (cond) std::printf("  ok   %s\n", what);
    else { std::printf("  FAIL %s\n", what); ++g_fail; }
}

static void near(double got, double want, double tol, const char* what) {
    if (std::isfinite(got) && std::fabs(got - want) <= tol) {
        std::printf("  ok   %s (%.10f)\n", what, got);
    } else {
        std::printf("  FAIL %s: got %.10f want %.10f (tol %g)\n", what, got, want, tol);
        ++g_fail;
    }
}

// ---------------------------------------------------------------------------
// Panel builder. `adopt[u]` is the first treated period (0 = never treated).
struct Panel {
    std::vector<double> y, id, t, w;
    void push(double unit, double per, double yy, double treat) {
        id.push_back(unit); t.push_back(per); y.push_back(yy); w.push_back(treat);
    }
    int64_t n() const { return static_cast<int64_t>(y.size()); }
};

// Exactly two-way additive: y = alpha_i + beta_t + tau * treated. For such data
// the sdid estimand is tau for ANY unit weights on the simplex and ANY time
// weights on the simplex — the weighted double difference cancels alpha and beta
// identically. So this leg checks the estimator's algebra against an answer
// derived from the DGP, not from a previous run of the estimator.
static Panel additive_panel(int n_units, int n_periods, const std::vector<int>& adopt,
                            double tau) {
    Panel P;
    for (int u = 0; u < n_units; ++u) {
        double alpha = 0.37 * u - 1.5;
        for (int p = 1; p <= n_periods; ++p) {
            double beta = 0.11 * p * p - 0.4 * p;
            double treat = (adopt[u] > 0 && p >= adopt[u]) ? 1.0 : 0.0;
            P.push(u + 1, p, alpha + beta + tau * treat, treat);
        }
    }
    return P;
}

// A deterministic pseudo-random staggered panel: unit and period effects, a
// two-factor structure that gives the synthetic control something real to fit,
// and reproducible noise from a fixed 64-bit LCG.
static Panel factor_panel(int n_units, int n_periods, const std::vector<int>& adopt,
                          double tau) {
    Panel P;
    uint64_t s = 88172645463325252ull;
    auto rnd = [&s]() {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<double>((s >> 11) & ((1ull << 53) - 1)) / 9007199254740992.0 - 0.5;
    };
    std::vector<double> l1(n_units), l2(n_units);
    for (int u = 0; u < n_units; ++u) { l1[u] = rnd(); l2[u] = rnd(); }
    for (int u = 0; u < n_units; ++u) {
        double alpha = 0.5 * rnd();
        for (int p = 1; p <= n_periods; ++p) {
            double f1 = std::sin(0.7 * p), f2 = std::cos(0.3 * p);
            double treat = (adopt[u] > 0 && p >= adopt[u]) ? 1.0 : 0.0;
            P.push(u + 1, p,
                   2.0 + alpha + 0.05 * p + 1.3 * l1[u] * f1 + 0.9 * l2[u] * f2 +
                       0.25 * rnd() + tau * treat,
                   treat);
        }
    }
    return P;
}

static SdidResult run(const Panel& P, const SdidConfig& cfg) {
    return sdid_run(P.y.data(), P.id.data(), P.t.data(), P.w.data(), P.n(), cfg);
}

int test_sdid_main() {
    std::printf("test_sdid:\n");

    // ---- A. analytic identity on an exactly additive staggered panel ---------
    {
        std::vector<int> adopt(24, 0);
        for (int u = 0; u < 8; ++u) adopt[u] = 5;
        for (int u = 8; u < 16; ++u) adopt[u] = 9;   // two staggered cohorts
        Panel P = additive_panel(24, 12, adopt, 1.2);
        SdidConfig cfg; cfg.vce = "noinference";
        SdidResult R = run(P, cfg);
        ok(R.ok, "A: additive staggered panel estimates");
        near(R.att, 1.2, 1e-9, "A: ATT == tau exactly on two-way-additive data");
        ok(R.cohorts.size() == 2, "A: two adoption cohorts decomposed");
        if (R.cohorts.size() == 2) {
            near(R.cohorts[0].tau, 1.2, 1e-9, "A: cohort-1 block ATT");
            near(R.cohorts[1].tau, 1.2, 1e-9, "A: cohort-2 block ATT");
            // Aggregation weights are proportional to treated (unit, period) cells:
            // cohort 1 = 8 units x 8 post periods, cohort 2 = 8 x 4.
            near(R.cohorts[0].weight, 64.0 / 96.0, 1e-12, "A: weight = N_tr*T_post share");
            near(R.cohorts[1].weight, 32.0 / 96.0, 1e-12, "A: weight = N_tr*T_post share");
        }
        ok(R.n_units == 24 && R.n_treated_units == 16 && R.n_control_units == 8,
           "A: N accounting (24 units = 16 treated + 8 control)");
        ok(R.n_periods == 12 && R.n_obs == 24 * 12, "A: period / observation accounting");
    }

    // ---- B. regression pin on a fixed pseudo-random staggered panel ----------
    // Not an oracle: a golden that turns red if the solver, the regularization or
    // the aggregation drifts. Values recorded from this implementation.
    {
        std::vector<int> adopt(30, 0);
        for (int u = 0; u < 10; ++u) adopt[u] = 7;
        for (int u = 10; u < 18; ++u) adopt[u] = 11;
        Panel P = factor_panel(30, 16, adopt, 1.5);
        SdidConfig cfg; cfg.vce = "bootstrap"; cfg.reps = 40; cfg.seed = 20260806ull;
        SdidResult R = run(P, cfg);
        ok(R.ok, "B: factor-structure staggered panel estimates");
        near(R.att, 1.4591054059, 1e-8, "B: ATT regression pin");
        // The SE has one pin per floating-point regime, not a wider tolerance. A few
        // resampled panels here are near-degenerate, so whether `a*b + c` rounds once
        // (FMA) or twice moves those replicates by up to 3e-4 and the SE in its fourth
        // significant digit; the point estimate above does not move. Targets without
        // FMA (the x86-64-v2 Windows floor) never fuse, and GCC and clang agree on them
        // bit for bit, so that value is compiler-independent.
#if defined(__x86_64__) && !defined(__FMA__)
        near(R.se, 0.0599451776, 1e-8, "B: bootstrap SE regression pin (no FMA)");
#else
        near(R.se, 0.0599426696, 1e-8, "B: bootstrap SE regression pin");
#endif
        near(R.t, R.att / R.se, 1e-12, "B: t == att/se");
        near(R.ci_hi - R.ci_lo, 2.0 * 1.959963984540054 * R.se, 1e-12,
             "B: 95% CI width == 2 z se (large-sample, as Stata's sdid prints)");
        ok(R.reps_done == 40 && static_cast<int>(R.rep_att.size()) == 40,
           "B: replicate log has one entry per rep");
    }

    // ---- C. RED CERTIFICATE for the incomplete-rectangle guard ---------------
    // The guard must be LOAD-BEARING. Each arm below runs the SAME gappy panel
    // twice: once normally (must refuse, loudly, by name) and once with the guard
    // explicitly bypassed (must produce a NON-FINITE ATT — the silent wrong answer
    // the guard exists to prevent). Deleting the guard turns the first assertion
    // red; making the gap harmless turns the second red. A test that only checked
    // the refusal would still pass with the guard removed, and would certify
    // nothing.
    {
        std::vector<int> adopt(20, 0);
        for (int u = 0; u < 12; ++u) adopt[u] = 6;

        struct Arm { const char* name; int drop_row; bool blank; };
        const Arm arms[] = {
            {"C1 missing outcome in a control unit's pre period", 3 * 10 + 2, true},
            {"C2 missing outcome in a treated unit's post period", 0 * 10 + 8, true},
            {"C3 an absent (unit, period) row — a true rectangle gap", 17 * 10 + 4, false},
        };
        for (const Arm& arm : arms) {
            Panel full = factor_panel(20, 10, adopt, 1.0);
            Panel P;
            for (int64_t k = 0; k < full.n(); ++k) {
                if (k == arm.drop_row) {
                    if (!arm.blank) continue;                    // row absent entirely
                    P.push(full.id[k], full.t[k], std::nan(""), full.w[k]);
                    continue;
                }
                P.push(full.id[k], full.t[k], full.y[k], full.w[k]);
            }
            SdidConfig guarded; guarded.vce = "noinference";
            SdidResult G = run(P, guarded);
            std::string msg = std::string("C: guarded refuses — ") + arm.name;
            ok(!G.ok && G.diagnostic == SdidDiagnostic::IncompletePanel, msg.c_str());
            ok(G.error.find("rectangle is incomplete") != std::string::npos &&
                   G.error.find("r(504)") != std::string::npos,
               "C: refusal names the incomplete rectangle and Stata's r(504)");
            ok(std::isnan(G.att), "C: refusal carries no number at all");

            SdidConfig unguarded = guarded;
            unguarded.unguard_incomplete_panel = true;
            SdidResult U = run(P, unguarded);
            // THIS is the certificate: without the guard the estimator does not
            // error, it ANSWERS — with a missing ATT. That is the defect.
            ok(U.ok, "C: guard-removed build still reports success (the trap)");
            ok(!std::isfinite(U.att),
               "C: guard-removed build yields a NON-FINITE ATT — guard is load-bearing");

            // Control: the same panel WITHOUT the gap estimates fine, so the arms
            // above isolate the gap and nothing else.
            SdidResult ctrl = run(full, guarded);
            ok(ctrl.ok && std::isfinite(ctrl.att),
               "C: the same panel with the cell restored estimates normally");
        }
    }

    // ---- D. oracle-pinned refusals ------------------------------------------
    {
        // 12 treated vs 8 controls — placebo inference is impossible.
        std::vector<int> adopt(20, 0);
        for (int u = 0; u < 12; ++u) adopt[u] = 6;
        Panel P = factor_panel(20, 10, adopt, 1.0);
        SdidConfig cfg; cfg.vce = "placebo";
        SdidResult R = run(P, cfg);
        ok(!R.ok && R.diagnostic == SdidDiagnostic::PlaceboInsufficientControls,
           "D: vce(placebo) refused when controls do not outnumber treated");
        ok(R.error.find("r(451)") != std::string::npos &&
               R.error.find("control") != std::string::npos,
           "D: placebo refusal names r(451) and the control/treated counts");

        // 8 treated vs 12 controls — placebo is now well posed, and is refused BY
        // NAME as unimplemented rather than silently downgraded to bootstrap.
        std::vector<int> adopt2(20, 0);
        for (int u = 0; u < 8; ++u) adopt2[u] = 6;
        Panel P2 = factor_panel(20, 10, adopt2, 1.0);
        SdidResult R2 = run(P2, cfg);
        ok(!R2.ok && R2.diagnostic == SdidDiagnostic::UnsupportedVce,
           "D: the passing path of vce(placebo) is refused by name, not substituted");

        SdidConfig jk; jk.vce = "jackknife";
        SdidResult R3 = run(P2, jk);
        ok(!R3.ok && R3.diagnostic == SdidDiagnostic::UnsupportedVce &&
               R3.error.find("jackknife") != std::string::npos,
           "D: vce(jackknife) refused by name");

        SdidConfig junk; junk.vce = "wildboot";
        SdidResult R4 = run(P2, junk);
        ok(!R4.ok && R4.diagnostic == SdidDiagnostic::UnsupportedVce,
           "D: an unknown vce() is refused, not ignored");
    }

    // ---- E. determinism and the bootstrap's self-identity --------------------
    {
        std::vector<int> adopt(26, 0);
        for (int u = 0; u < 9; ++u) adopt[u] = 5;
        for (int u = 9; u < 16; ++u) adopt[u] = 9;
        Panel P = factor_panel(26, 14, adopt, 0.8);

        SdidConfig c1; c1.vce = "bootstrap"; c1.reps = 60; c1.seed = 4242ull;
        SdidResult A = run(P, c1), B = run(P, c1);
        ok(A.ok && B.ok, "E: two runs at the same seed both estimate");
        bool same = A.att == B.att && A.se == B.se && A.rep_att.size() == B.rep_att.size();
        for (size_t i = 0; same && i < A.rep_att.size(); ++i)
            same = A.rep_att[i] == B.rep_att[i];
        ok(same, "E: same seed twice -> bit-identical ATT, SE and replicate log");

        SdidConfig c2 = c1; c2.seed = 4243ull;
        SdidResult C = run(P, c2);
        ok(C.ok && C.se != A.se, "E: a different seed moves the SE");
        ok(C.att == A.att, "E: the point estimate does NOT depend on the seed");

        // Self-identity: the reported se^2 must be exactly the variance of the
        // replicate log it claims to summarize (AAHIW21 Algorithm 2 uses 1/B).
        double m = 0;
        for (double v : A.rep_att) m += v;
        m /= A.rep_att.size();
        double v2 = 0;
        for (double v : A.rep_att) v2 += (v - m) * (v - m);
        v2 /= A.rep_att.size();
        near(A.se * A.se, v2, 1e-14, "E: se^2 recomputed from the replicate log");
        ok(A.reps_done == 60, "E: reps_done matches reps()");
    }

    // ---- F. panels sdid cannot estimate are named, not guessed ---------------
    {
        std::vector<int> none(10, 0);
        Panel P = factor_panel(10, 8, none, 1.0);
        SdidResult R = run(P, SdidConfig{});
        ok(!R.ok && R.diagnostic == SdidDiagnostic::BadPanel &&
               R.error.find("no treated units") != std::string::npos,
           "F: a panel with no treated units is refused by name");

        std::vector<int> all(10, 4);
        Panel P2 = factor_panel(10, 8, all, 1.0);
        SdidResult R2 = run(P2, SdidConfig{});
        ok(!R2.ok && R2.diagnostic == SdidDiagnostic::BadPanel &&
               R2.error.find("never-treated control units") != std::string::npos,
           "F: a panel with no never-treated controls is refused by name");

        // Treatment that switches back off is not staggered adoption.
        std::vector<int> adopt(12, 0);
        for (int u = 0; u < 6; ++u) adopt[u] = 5;
        Panel P3 = factor_panel(12, 10, adopt, 1.0);
        for (int64_t k = 0; k < P3.n(); ++k)
            if (P3.id[k] == 1 && P3.t[k] == 8) P3.w[k] = 0.0;
        SdidResult R3 = run(P3, SdidConfig{});
        ok(!R3.ok && R3.diagnostic == SdidDiagnostic::BadPanel &&
               R3.error.find("not absorbing") != std::string::npos,
           "F: non-absorbing treatment is refused by name");

        // Adoption at the very start leaves no pre-period to fit weights on.
        std::vector<int> adopt4(12, 0);
        for (int u = 0; u < 6; ++u) adopt4[u] = 1;
        Panel P4 = factor_panel(12, 10, adopt4, 1.0);
        SdidResult R4 = run(P4, SdidConfig{});
        ok(!R4.ok && R4.diagnostic == SdidDiagnostic::BadPanel &&
               R4.error.find("pre-treatment periods") != std::string::npos,
           "F: adoption with no pre-treatment periods is refused by name");

        // Two rows for the same (unit, period) cell.
        std::vector<int> adopt5(12, 0);
        for (int u = 0; u < 6; ++u) adopt5[u] = 5;
        Panel P5 = factor_panel(12, 10, adopt5, 1.0);
        P5.push(1, 3, 0.0, 0.0);
        SdidResult R5 = run(P5, SdidConfig{});
        ok(!R5.ok && R5.diagnostic == SdidDiagnostic::BadPanel &&
               R5.error.find("duplicate") != std::string::npos,
           "F: duplicate (unit, period) rows are refused by name");
    }

    if (g_fail) {
        std::printf("FAILED (%d checks)\n", g_fail);
        return 1;
    }
    std::printf("PASSED\n");
    return 0;
}
