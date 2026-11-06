// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace cecon {

// `teffects nnmatch` — Abadie–Imbens nearest-neighbour matching (StataCorp
// [CAUSAL]). Method identity, and every rule below, was measured against live
// StataBE 19.5 BEFORE this file existed:
//   internal note teffects-nnmatch-20260908/REPORT.md   (14 fixtures, 2,800 rows)
//   .../REFUTE-0.md + refute-0/                            (the H2 match-set rule)
//   .../REFUTE-1.md                                        (source/anchor audit)
//   tests/eval/golden/aer_estim2_oracle.log INFO|nn_*      (rmcoll, `if`, rc's)
struct NnmatchOpts {
    int nneighbor = 1;       // m; Stata default 1
    int vce_nn = 2;          // h = e(k_robust); Stata default 2
    std::string metric = "mahalanobis";  // | "ivariance" | "euclidean"
    bool atet = false;       // e(stat) = atet when true
    // e(dtolerance) = sqrt(c(epsdouble)); RELATIVE, on the distance itself.
    double dtolerance = 1.4901161193847656e-8;
};

struct NnmatchResult {
    double tau = 0.0, V = 0.0;
    int64_t n = 0, n0 = 0, n1 = 0;
    double control_level = 0.0, treated_level = 1.0;
    int k_nnmin = 0, k_nnmax = 0;
    std::vector<std::string> colnames;    // design columns actually used
    std::vector<std::string> dropped;     // _rmcoll casualties, in design order
    std::vector<double> mean;             // e(mstats) row `mean`, kept columns
    std::vector<double> sd;               // e(mstats) row `stddev` (ivariance only)
    std::vector<double> mscale;           // p*p col-major S^{-1/2}; empty unless mahalanobis
    std::vector<int64_t> rows;            // kept rows, 0-based index into the input columns
    // Per KEPT row, in `rows` order: the match set as 1-based row numbers of the
    // ORIGINAL frame (measured: with `if`, Stata's gen() ids are _n of the FULL
    // data — INFO|nn_edge|ifodd_*), and the distances in the same order.
    std::vector<std::vector<int64_t>> omega;
    std::vector<std::vector<double>> dist;
    std::vector<double> te, po0, po1;
};

// Columns are the matching varlist in TYPED order: `xcols` (continuous) first,
// then each `fvcols` entry expanded to one 0/1 column per level above its
// minimum (Stata's `0b.` base rule). Rows are listwise-complete over y, t, and
// every matching column, intersected with `keep`.
NnmatchResult nnmatch_fit(const double* y, const double* t,
                          const std::vector<std::string>& xnames,
                          const std::vector<const double*>& xcols,
                          const std::vector<std::string>& fvnames,
                          const std::vector<const double*>& fvcols,
                          const std::vector<char>& keep, const NnmatchOpts& o);

}  // namespace cecon
