// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// xtlogit, fe — Chamberlain (1980) conditional logit for binary panels.
//
// Conditional on the panel sum K_i = Σ_t y_it, the unit fixed effect drops out.
// The conditional likelihood is
//   L_i = exp(Σ_t y_it · x_it β) / B_i(K_i, β)
// where B_i is the partition function summing exp(Σ_t d_t x_t β) over all
// binary sequences (d_1..d_{T_i}) with Σ d_t = K_i.
//
// Howard's O(T·K) recursion to compute B_i:
//   f_t(k) = f_{t-1}(k) + f_{t-1}(k-1) · e^{x_t β}
// with f_0(0)=1, f_0(k>0)=0. Then B_i = f_{T_i}(K_i).
//
// To get score / Hessian we also need expected y_it under the conditional:
//   p_it = E[y_it | K_i, x_i, β]
// Forward-backward recursion provides this in O(T·K).
//
// Panels with K_i == 0 or K_i == T_i contribute nothing (the only possible
// sequence is constant); they're dropped.
#pragma once
#include <cstdint>
#include <vector>

namespace cecon {

struct XtlogitFeResult {
    int k = 0;
    int64_t n_obs = 0, n_groups = 0;
    int64_t n_dropped_const = 0;   // K_i ∈ {0, T_i}
    int64_t n_dropped_size = 0;    // T_i < 2
    // Row counts of the panels used (after the drops): Stata's e(g_min)/e(g_avg)/
    // e(g_max) over e(sample). 0 when no panel is kept.
    int64_t g_min = 0, g_max = 0;
    double g_avg = 0;
    std::vector<double> beta;
    std::vector<double> se_iid;
    std::vector<double> se_robust;
    std::vector<double> V_iid, V_robust;  // complete fitted covariance, column-major
    double loglik = 0;
    int iters = 0;
    bool converged = false;
};

// Optimizer: Newton-Raphson with halving / forward steps and ml's default stop
// rule from the start xtlogit_fe_start chooses; see xtlogit.cpp section 3.
// `iters` is e(ic); `loglik` is e(ll) at beta.
XtlogitFeResult xtlogit_fe(const double* X, int64_t n_obs, int k,
                           const double* y, const double* ivar,
                           int maxit = 300);

// Thread count xtlogit_fe's per-panel fan-out uses for a fit of `ng` panels
// whose longest panel is `Tmax` with `k` regressors: src/core/threads.h
// threads_for_work with this estimator's work model (each panel's recursion
// pair is O((T+1)^2)) and its per-thread scratch model (the two (Tmax+1)^2
// forward/backward tables dominate). One definition for both dispatch sites,
// exposed so test_thread_determinism can prove its multi-thread arm actually
// fans out instead of comparing two serial runs.
int xtlogit_fanout_threads(int ng, int Tmax, int k);

}  // namespace cecon
