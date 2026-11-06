// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Synthetic difference-in-differences, implemented from the published
// methodology paper:
//
//   [AAHIW21] Arkhangelsky, D., S. Athey, D.A. Hirshberg, G.W. Imbens and
//             S. Wager (2021), "Synthetic Difference-in-Differences",
//             American Economic Review 111(12), 4088-4118.
//
// Method source: the paper cited above. Conventions the paper leaves open are
// pinned by this project's own committed goldens and unit tests.
//
// sdid_run implements AAHIW21's estimator (Algorithm 1): unit weights omega and
// time weights lambda are each chosen by a separate regularized minimum-distance
// problem over the simplex so that the pre-treatment trajectory of the synthetic
// control matches the treated average, and the ATT is the resulting weighted
// diff-in-diff (eq. 2.2-2.3). Staggered adoption is handled as AAHIW21 §A
// prescribes: the panel is split into one block per adoption date (that cohort's
// treated units against the NEVER-treated units, pre = periods before adoption),
// each block is estimated separately, and the block ATTs are averaged with
// weights proportional to each block's count of treated (unit, period) cells,
// N_tr^a * T_post^a. Inference is by vce(): bootstrap (Algorithm 2, resampling
// PANEL UNITS with replacement) is the implemented estimator; reps() and seed()
// parametrize it. The bootstrap draws come from this engine's own counter-based
// RNG (core/rng.h, Philox4x32-10) — fixed-seed bit-reproducibility is ours, and
// is deliberately NOT an attempt to reproduce any other package's draw stream.
//
// Conventions the paper leaves open, pinned here by measurement against the
// project's Stata 19.5 oracle (see tests/unit/test_sdid.cpp for the target and
// sdid.cpp's SWEEP note for the variants tried and rejected):
//   - the noise level sigma feeding zeta is the SAMPLE standard deviation
//     (denominator n-1) of the first differences of the CONTROL units over the
//     block's PRE periods;
//   - zeta_omega = (N_tr * T_post)^(1/4) * sigma, entering the omega program as
//     a ridge zeta_omega^2 * T_pre * ||omega||^2;
//   - the lambda program carries only a numerically negligible ridge
//     (1e-6 * sigma)^2 * N_co;
//   - both programs carry a free intercept (they match trajectory SHAPE, not
//     level), which the solver absorbs by centering;
//   - the block ATTs aggregate with weights proportional to N_tr^a * T_post^a.
//
// AAHIW21's construction requires a COMPLETE panel rectangle (every unit
// observed at every period, with a non-missing outcome) and, for the placebo
// variance estimator, more control than treated units; sdid_run reports these as
// diagnostics instead of silently degrading (see SdidDiagnostic below). The
// incomplete-rectangle guard is LOAD-BEARING, not cosmetic: without it the
// estimator propagates the gaps and returns a missing ATT, which is the exact
// silent-wrong-answer shape this codebase treats as a cardinal defect. That is
// what SdidConfig::unguard_incomplete_panel exists to demonstrate — see its
// comment there.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace cecon {

enum class SdidDiagnostic {
    None,
    IncompletePanel,               // the (unit, period) rectangle has gaps
    PlaceboInsufficientControls,   // vce(placebo) needs more controls than treated units
    UnsupportedVce,                // vce() named a variance estimator this wave does not implement
    BadPanel,                      // no treated units / no controls / non-absorbing treatment / no pre-periods
};

struct SdidConfig {
    std::string vce = "bootstrap";  // bootstrap | jackknife | placebo | noinference
    int reps = 50;
    uint64_t seed = 0;              // seeds OUR Philox stream (core/rng.h)
    double level = 95.0;            // confidence level for ci_lo/ci_hi

    // Frank-Wolfe stopping rule for the two simplex programs. The defaults reach
    // the exact minimizer on the oracle fixture (measured: the duality gap is
    // exhausted in ~250-350 pairwise steps per block, and tightening the tolerance
    // by three further decades moves the ATT by < 1e-12).
    int max_iter = 20000;
    double tol = 1e-13;

    // FALSIFICATION HOOK, test-only. When true the incomplete-rectangle guard is
    // SKIPPED and the estimator runs on the gappy rectangle exactly as it would if
    // the guard had never been written. It exists so tests/unit/test_sdid.cpp can
    // prove the guard is load-bearing: the unguarded arm must produce a NON-FINITE
    // ATT (the silent wrong answer), the guarded arm must produce the named loud
    // error. A test that only exercised the guarded arm would still pass if someone
    // deleted the guard, and would therefore certify nothing. No CLI path sets this.
    bool unguard_incomplete_panel = false;
};

// One adoption-date block of the staggered decomposition.
struct SdidCohort {
    double adopt = 0;      // first period at which this cohort is treated
    int n_treated = 0;     // treated units in this block
    int n_control = 0;     // never-treated units used as its donor pool
    int t_pre = 0;         // periods before adoption
    int t_post = 0;        // periods from adoption onward
    double zeta = 0;       // regularization constant actually used for omega
    double tau = 0;        // this block's ATT
    double weight = 0;     // its normalized weight in the aggregate ATT
};

struct SdidResult {
    double att = 0;
    double se = 0;
    double t = 0;
    double p = 1;
    double ci_lo = 0, ci_hi = 0;

    // N accounting.
    int64_t n_obs = 0;
    int n_units = 0;
    int n_treated_units = 0;
    int n_control_units = 0;
    int n_periods = 0;

    std::vector<SdidCohort> cohorts;

    // Replicate log — the bootstrap's own draws, kept so the reported se can be
    // recomputed from them (the self-identity leg in test_sdid.cpp).
    std::vector<double> rep_att;
    int reps_done = 0;
    int reps_redrawn = 0;  // degenerate resamples (no treated or no control) rejected

    bool ok = true;
    SdidDiagnostic diagnostic = SdidDiagnostic::None;
    std::string error;
};

// y[n_obs]: outcome. ivar[n_obs]: unit id. time[n_obs]: period.
// treated[n_obs]: 1 if this (unit, period) cell is treated, 0 otherwise
// (standard block/staggered treatment indicator; a unit is "a treated unit"
// if treated==1 for any of its periods).
SdidResult sdid_run(const double* y, const double* ivar, const double* time,
                     const double* treated, int64_t n_obs,
                     const SdidConfig& cfg = {});

// Human-readable tag for the diagnostic, carried verbatim in the CLI's JSON so a
// frontend can branch on WHY a run refused without scraping the message.
const char* sdid_diagnostic_name(SdidDiagnostic d);

}  // namespace cecon
