// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// P7 wave — svy linearized (Taylor) variance for stratified, clustered,
// pweighted designs.
//
// Deliberately kept OUT of vcov.h's Vcov enum: compute_vcov's OLS-family path is
// byte-frozen, and svy is a parallel assembler in the same spirit as the ML
// sandwiches in cmds_mle.cpp. The estimator itself is unchanged (an ordinary
// pweighted fit) — only the meat and the reference distribution differ.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "core/model.h"

namespace cecon {

struct SvyMeatResult {
    std::vector<double> meat;  // p*p, column-major
    int n_psu = 0;
    int n_strata = 0;
    double pop_size = 0;  // Σw over the estimation sample
    bool ok = true;
    std::string singleton_stratum;  // non-empty ⇒ some stratum held a single PSU
};

// S: n*p column-major per-observation WEIGHTED score rows (for regress
// s_i = w_i e_i x_i; for logit w_i (y_i − p_i) x_i; for mean w_i (y_i − µ̂)/Ŵ).
//
//   meat = Σ_h [n_h/(n_h−1)] Σ_{c∈h} (z_c − z̄_h)(z_c − z̄_h)'
//
// with z_c the PSU total of the score rows, z̄_h the mean of those totals within
// stratum h, and n_h the number of PSUs in stratum h. The WITHIN-STRATUM
// CENTERING is the whole difference from vcov.cpp's uncentered cluster_meat, and
// the per-stratum n_h/(n_h−1) is the ONLY small-sample factor — no additional
// global G/(G−1) or (N−1)/(N−k) rides on top.
//
// Design degrees of freedom are #PSUs − #strata, which the caller reads off the
// returned counts. A stratum holding a single PSU leaves the linearized variance
// undefined (Stata's default reports missing); its name is returned so the caller
// can reject honestly instead of dividing by zero.
//
// `weights` may be null, in which case pop_size is the row count.
SvyMeatResult svy_linearized_meat(const double* S, int64_t n, int p, const Factor& psu,
                                  const Factor& stratum, const double* weights);

}  // namespace cecon
