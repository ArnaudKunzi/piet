// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Principal components, AER bundle H1.
//
// The arithmetic is ordinary; the POST-PROCESSING is where Stata's answer lives,
// and much of it is not in [MV] pca. What the manual does not state was measured
// on StataBE.
#pragma once
#include <string>
#include <vector>

namespace cecon {

struct PcaOpts {
    bool covariance = false;   // default is the correlation matrix
    int components = -1;       // <0 = all p
    double mineigen = 1e-5;    // [MV] pca: the default is 1e-5
};

struct PcaResult {
    int n_obs = 0;
    int p = 0;
    int f = 0;
    std::string ctype;            // "correlation" | "covariance"
    std::vector<double> ev;       // p eigenvalues, DESCENDING, after zeroing
    std::vector<double> loadings; // p*f, row-major: L[i*f + j]
    std::vector<double> psi;      // p uniquenesses, chopped
    std::vector<double> c;        // p*p, the analysed matrix itself
    std::vector<double> means;    // p
    std::vector<double> sds;      // p, EMPTY for covariance PCA
    std::vector<double> vars;     // p, raw variances
    std::vector<double> components_tbl;  // p*4: eigenvalue, difference, proportion, cumulative
    double trace = 0;
    double rho = 0;
    double lndet = 0;
    double cond = 0;
    bool lndet_missing = false;
    bool cond_missing = false;
    bool trace_missing = false;
    std::vector<std::string> dropped;  // zero-variance columns, dropped with a note
};

// Throws EngineError for every screening failure Stata's pca reports before it
// decomposes anything: all variables dropped (498), fewer than two surviving
// (102), zero rows (2000), one row (2001), and a negative eigenvalue where none
// is expected (198).
//
// `names` and `cols` are parallel; each column is the FULL column and `keep`
// marks the listwise-complete rows.
PcaResult pca_fit(const std::vector<std::string>& names,
                  const std::vector<const double*>& cols, const std::vector<char>& keep,
                  const PcaOpts& o);

}  // namespace cecon
