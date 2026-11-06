// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// cecon — Stata-matching detection and omission of collinear regressors.
//
// Rank-deficient designs arise mainly from factor-variable interactions with
// structurally empty cells (e.g. `i.foreign##i.rep78` on sysuse auto, where
// foreign cars have rep78 in {3,4,5} only, so 1.foreign == 1.foreign#3.rep78 +
// 1.foreign#4.rep78 + 1.foreign#5.rep78 exactly). The model fit is unique, but
// the coefficient parameterization is not, so we must drop the SAME column Stata
// drops to match `regress` coefficient-for-coefficient.
//
// The forward pass retains the original term order for callers whose contract
// requires it (instrument and differenced-design selection). OLS uses the
// pivoted pass below: scaled proportional columns need not retain the earlier
// term (w2ak_estout_family m7 and w2an_coefplot_family om).
#pragma once
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace cecon {

// Forward rank-revealing pass over a column-major design X (n x k). Returns a
// length-k keep mask: keep[j]==1 to retain column j, ==0 to omit it because it
// lies in the span of the earlier KEPT columns (to relative tolerance `tol`) or
// is all-zero. Earlier columns always win, so the LATER member of a collinear
// set is the one dropped. A full-rank design
// returns all ones (the pass is then a no-op for the caller).
//
// Robustness: modified Gram-Schmidt with one reorthogonalization pass ("twice is
// enough"). The acceptance test is scale-invariant per column (residual norm
// relative to the column's own norm == sin of its angle to the accepted span),
// so exact collinearity from 0/1 indicator arithmetic yields a residual ratio at
// machine-epsilon level, far below `tol`, while merely-correlated full-rank
// columns stay well above it.
// `late` (optional, size k): columns flagged there are scanned after every other
// column, each group in design order (ModelData::late_cells).
inline std::vector<char> collinear_keep_mask(const double* X, int64_t n, int k,
                                             double tol = 1e-7,
                                             const std::vector<char>* late = nullptr) {
    std::vector<char> keep(static_cast<size_t>(k), 1);
    std::vector<std::vector<double>> Q;  // orthonormal basis of the accepted columns
    Q.reserve(static_cast<size_t>(k));
    std::vector<double> v(static_cast<size_t>(n));
    std::vector<int> order;
    order.reserve(static_cast<size_t>(k));
    const bool useLate = late != nullptr && late->size() == static_cast<size_t>(k);
    for (int pass = 0; pass < (useLate ? 2 : 1); ++pass)
        for (int j = 0; j < k; ++j)
            if (!useLate || ((*late)[static_cast<size_t>(j)] != 0) == (pass == 1)) order.push_back(j);
    for (int j : order) {
        const double* xj = X + static_cast<int64_t>(j) * n;
        double orig = 0.0;
        for (int64_t t = 0; t < n; ++t) { v[static_cast<size_t>(t)] = xj[t]; orig += xj[t] * xj[t]; }
        orig = std::sqrt(orig);
        if (orig <= 0.0) { keep[static_cast<size_t>(j)] = 0; continue; }  // all-zero column
        for (int pass = 0; pass < 2; ++pass)
            for (const auto& q : Q) {
                double d = 0.0;
                for (int64_t t = 0; t < n; ++t) d += q[static_cast<size_t>(t)] * v[static_cast<size_t>(t)];
                for (int64_t t = 0; t < n; ++t) v[static_cast<size_t>(t)] -= d * q[static_cast<size_t>(t)];
            }
        double nrm = 0.0;
        for (int64_t t = 0; t < n; ++t) nrm += v[static_cast<size_t>(t)] * v[static_cast<size_t>(t)];
        nrm = std::sqrt(nrm);
        if (nrm / orig < tol) { keep[static_cast<size_t>(j)] = 0; continue; }  // collinear
        for (int64_t t = 0; t < n; ++t) v[static_cast<size_t>(t)] /= nrm;
        Q.push_back(v);
    }
    return keep;
}

// Rank-revealing column pivoting for least squares. Retain the column with the
// largest unexplained norm at each step (the diagonal Schur-complement pivot),
// with original order breaking ties. Protect the model's intercept before the
// other pivots. Masks stay in ORIGINAL order, so the solve and coefficient order
// do not change on a full-rank design. The ordinary forward pass is the fast
// path: only a deficient design allocates the pivoted residual matrix.
inline std::vector<char> collinear_pivot_keep_mask(const double* X, int64_t n, int k,
                                                   int intercept = -1, double tol = 1e-7) {
    std::vector<char> forward = collinear_keep_mask(X, n, k, tol);
    bool full_rank = true;
    for (char c : forward) full_rank = full_rank && c;
    if (full_rank) return forward;

    std::vector<double> residual(X, X + static_cast<int64_t>(k) * n);
    std::vector<double> orig(static_cast<size_t>(k), 0.0), norm(static_cast<size_t>(k), 0.0);
    std::vector<char> keep(static_cast<size_t>(k), 0), done(static_cast<size_t>(k), 0);
    for (int j = 0; j < k; ++j) {
        const double* x = X + static_cast<int64_t>(j) * n;
        for (int64_t i = 0; i < n; ++i) orig[j] += x[i] * x[i];
        norm[j] = orig[j];
    }
    for (int step = 0; step < k; ++step) {
        int pivot = -1;
        if (step == 0 && intercept >= 0 && intercept < k) pivot = intercept;
        else {
            for (int j = 0; j < k; ++j)
                if (!done[j] && (pivot < 0 || norm[j] > norm[pivot])) pivot = j;
        }
        if (pivot < 0) break;
        done[pivot] = 1;
        if (!(orig[pivot] > 0.0) || norm[pivot] / orig[pivot] < tol * tol) continue;
        keep[pivot] = 1;
        double* q = residual.data() + static_cast<int64_t>(pivot) * n;
        const double invnorm = 1.0 / std::sqrt(norm[pivot]);
        for (int64_t i = 0; i < n; ++i) q[i] *= invnorm;
        for (int j = 0; j < k; ++j) {
            if (done[j]) continue;
            double* v = residual.data() + static_cast<int64_t>(j) * n;
            for (int pass = 0; pass < 2; ++pass) {
                double dot = 0.0;
                for (int64_t i = 0; i < n; ++i) dot += q[i] * v[i];
                for (int64_t i = 0; i < n; ++i) v[i] -= dot * q[i];
            }
            norm[j] = 0.0;
            for (int64_t i = 0; i < n; ++i) norm[j] += v[i] * v[i];
        }
    }
    return keep;
}

// Compact a column-major design `X` (length n*k) in place, retaining only the
// columns with keep[j]!=0 (moved left, preserving order) and shrinking `names`
// to match. Updates `k` to the surviving count and resizes `X`. Returns the
// names of the dropped columns in their original order (for the `omitted[]`
// report). No-op returning {} if every column is kept.
inline std::vector<std::string> compact_columns(std::vector<double>& X, int64_t n, int& k,
                                                std::vector<std::string>& names,
                                                const std::vector<char>& keep) {
    std::vector<std::string> omitted;
    std::vector<std::string> surviving;
    surviving.reserve(static_cast<size_t>(k));
    int dst = 0;
    for (int j = 0; j < k; ++j) {
        if (keep[static_cast<size_t>(j)]) {
            if (dst != j)
                std::memmove(X.data() + static_cast<int64_t>(dst) * n,
                             X.data() + static_cast<int64_t>(j) * n,
                             static_cast<size_t>(n) * sizeof(double));
            surviving.push_back(std::move(names[static_cast<size_t>(j)]));
            ++dst;
        } else {
            omitted.push_back(std::move(names[static_cast<size_t>(j)]));
        }
    }
    k = dst;
    X.resize(static_cast<size_t>(n) * static_cast<size_t>(dst));
    names = std::move(surviving);
    return omitted;
}

}  // namespace cecon
