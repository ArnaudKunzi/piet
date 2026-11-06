// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// cecon — thin wrappers over Accelerate (CBLAS + LAPACK), column-major.
// All matrices are column-major with leading dimension = number of rows.
#pragma once
#include <cstdint>

namespace cecon::la {

// COMPENSATED ACCUMULATION (COBRA-DD-ACCUM, TOLERANCE.md §5 / §4.1).
//
// xtx / xtv / residual / dot / sumsq / sum below accumulate in double-double
// (FMA two-product + Knuth two-sum), NOT in plain BLAS double. Stata's own
// cross-products go through `quadcross` ([M-5]) for the same reason: the ladder's
// T1 tier (1e-12) is *defined* as "reachable because compensated accumulation
// makes it reachable", and plain double summation does not reach it on
// cancelling cross-products. The measurement that forced this, and the measured
// cost, are in linalg.cpp above each kernel. Every call bumps the
// `dd_accum_used` counter (core/eng_counters.h), readable via the `counters`
// frame op / PIET_EMIT_COUNTERS.
//
// Determinism (TOLERANCE §4.2): the reduction DAG is a function of the DATA
// ONLY — the lane count is a fixed compile-time constant, never derived from a
// thread count or a runtime block size, so 1-core and N-core results stay
// byte-identical when P9 parallelises above this layer.

// A := X' X, where X is n x k (col-major). Result is k x k, full symmetric.
void xtx(const double* X, int64_t n, int k, double* A);

// out := X' v   (X is n x k col-major, v is length n, out is length k).
void xtv(const double* X, int64_t n, int k, const double* v, double* out);

// e := y - X*beta   (X is n x k col-major, beta length k, y/e length n).
void residual(const double* X, int64_t n, int k, const double* beta, const double* y,
              double* e);

// Compensated inner product  sum_i a[i]*b[i]  (the RSS / moment scalar kernel).
double dot(const double* a, const double* b, int64_t n);

// Compensated sum of squares  sum_i a[i]^2  (RSS).
double sumsq(const double* a, int64_t n);

// Compensated sum of squared deviations  sum_i (a[i] - center)^2  (TSS), without
// materialising the centred copy.
double sumsq_dev(const double* a, int64_t n, double center);

// Compensated plain sum  sum_i a[i]  (means: cancellation-prone by definition).
double sum(const double* a, int64_t n);

// out := A * v   (A is k x k full symmetric/general, v length k).
void matvec(const double* A, int k, const double* v, double* out);

// out := A * x   (A is m x n col-major, x length n, out length m).
void mat_vec(const double* A, int64_t m, int n, const double* x, double* out);

// C(m x n) := A(m x kk) * B(n x kk)^T   (all column-major).
void gemm_nt(const double* A, const double* B, double* C, int m, int kk, int n);

// Invert SPD matrix A (k x k, full) in place via Cholesky. Returns LAPACK info
// (0 = success; >0 means not positive-definite at that leading minor).
int spd_inverse(double* A, int k);

// V := factor * (bread * meat * bread). All k x k full symmetric. tmp is scratch
// of size k*k supplied by caller (or pass nullptr to allocate internally).
void sandwich(const double* bread, const double* meat, int k, double factor, double* V,
              double* tmp = nullptr);

// Project symmetric A (k x k, full) onto the PSD cone: eigendecompose, clip
// negative eigenvalues to 0, rebuild. Returns the number of eigenvalues clipped
// (>0 means A was not PSD, as can happen with multiway-cluster vcov).
int psd_clip(double* A, int k);

// Symmetric eigendecomposition of A (k x k, full). On success returns 0, fills w
// with the k eigenvalues in ASCENDING order, and overwrites A with the
// eigenvectors (column j = eigenvector for w[j]). Wraps LAPACK dsyevd.
int sym_eig(double* A, int k, double* w);

// Full SVD of A (m x n, column-major; CONTENTS DESTROYED): A = U * diag(s) * Vt.
// U is m x m (col-major), Vt is n x n (= V', col-major), s holds min(m,n)
// singular values in DESCENDING order. Returns 0 on success. Wraps LAPACK dgesvd.
int svd(double* A, int m, int n, double* U, double* s, double* Vt);

// In-place inverse of a general (non-symmetric) k x k matrix A (col-major) via
// LU (dgetrf + dgetri). Returns 0 on success, nonzero if singular.
int gen_inverse(double* A, int k);

}  // namespace cecon::la
