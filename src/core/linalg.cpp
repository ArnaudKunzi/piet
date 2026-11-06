// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "core/linalg.h"

// Portable BLAS/LAPACK shim:
//   macOS  -> Apple Accelerate (Fortran LAPACK API, __LAPACK_int)
//   Linux  -> OpenBLAS for CBLAS + LAPACKE for LAPACK (clean C API)
#ifdef __APPLE__
  #ifndef ACCELERATE_NEW_LAPACK
  #define ACCELERATE_NEW_LAPACK 1
  #endif
  #include <Accelerate/Accelerate.h>
  using la_int = __LAPACK_int;
#else
  #include <cblas.h>
  #include <lapacke.h>
  using la_int = lapack_int;
#endif

#include <algorithm>
#include <cmath>
#include <vector>

#include "core/eng_counters.h"  // COBRA-DD-ACCUM: dd_accum_used mechanism counter

namespace cecon::la {

static void symmetrize_upper_to_full(double* A, int k) {
    for (int j = 0; j < k; ++j)
        for (int i = 0; i < j; ++i) A[j + i * k] = A[i + j * k];
}

// ===========================================================================
// COMPENSATED ACCUMULATION KERNEL (COBRA-DD-ACCUM)
// ===========================================================================
//
// WHAT WAS MEASURED (2026-08-07, this machine, -O2 probe; the accuracy figures
// are exact-arithmetic comparisons and do not depend on -O level):
//
//   probe A — X'X off-diagonal that CANCELS: col a = 1, col b = ±1e6 + U(-1,1),
//   so sum_i a_i b_i is O(1e2) while each product is O(1e6).
//       n=2e3    cblas_dsyrk relerr 4.43e-11   dd 0
//       n=2e5    cblas_dsyrk relerr 8.50e-10   dd 0
//       n=2e6    cblas_dsyrk relerr 1.35e-10   dd 0
//   → dsyrk misses T1 (1e-12) by ~3 orders and misses even T2 (1e-9) at n=2e5.
//
//   probe 1 — X'v with a cancelling v ({1e16, 1…1, -1e16}, exact sum n-2):
//       cblas_dgemv relerr 1.26e-3    dd 0.
//   → the *worst* site is xtv, not xtx.
//
//   probe 2 — X'X where the column merely carries a huge OFFSET (x = 1e6 + u,
//   no cancellation in the sum): dsyrk relerr 1.83e-15 on the off-diagonal,
//   0 on both diagonals. That class ALREADY MEETS T1 — a measured no-op.
//   Compensation is not needed for offset alone; it is needed for cancellation.
//
//   probe 3 — RSS-shaped plain scalar `for(i) ssr += e[i]*e[i]` over a
//   lognormal(0,8) e, n=1e6: relerr 3.87e-14 (cblas_ddot 6.19e-15). Inside T1
//   but only by ~26x, and the bound grows with n; dd gives 0.
//
// WHAT THIS FALSIFIES: the pre-adjudicated design said a *blocked* BLAS sum with
// compensation over the BLOCK PARTIALS would meet T1 and would be the smallest
// change. Measured: blocked dsyrk (B=512) + two-sum over the k×k block partials
// returns relerr 8.501e-10 on probe A — BIT-IDENTICAL to plain dsyrk, i.e. it
// buys exactly nothing. The reason is structural: the cancellation happens
// *inside* one block (512 products of 1e6 summing to O(1)), so the block partial
// is already rounded before the compensated cross-block sum ever sees it.
// Compensation has to be per-element or it is theatre. Hence the kernel below.
//
// WHAT IT COSTS (probe3, X'X, median of 5, -O2):
//     n=960     k=6   dsyrk  0.026 ms → dd 0.036 ms (1.4x)
//     n=1e5     k=6   dsyrk  2.70 ms  → dd 2.12 ms  (0.8x — FASTER; dsyrk is
//                                       poorly served by very skinny matrices)
//     n=1e6     k=10  dsyrk  9.89 ms  → dd 46.8 ms  (4.7x)
//     n=1e6     k=30  dsyrk 56.7 ms   → dd 398 ms   (7.0x)
//     n=2e5     k=60  dsyrk  7.68 ms  → dd 308 ms   (40x)
//   X'v: 2.5–5x on an operation an order of magnitude cheaper than X'X.
//   Row-blocking for locality was measured and rejected: 2% at k=30, i.e. the
//   kernel is dependency-chain bound, not memory bound. The 4 lanes are what
//   buys the speed (1.4x vs 1.9x single-lane at k=6; 4.7x vs 6.6x at k=10).
//   The k≳60 wall is real and is NOT hit by any current gate or bench section
//   (every regression in the composite bench and the freeze corpus is k≤~8 at
//   n≤~5e4, where the delta is tens of microseconds); see BUGLOG BUG-226.
//
// WHY NOT A CHEAPER SCHEME: the error is in the SUMMATION, not the products, so
// Dekker splitting into 3 exact dsyrk calls does not help (each still rounds its
// own accumulation), and the centred reformulation
// sum x_a x_b = n·m_a·m_b + sum(x_a-m_a)(x_b-m_b) just relocates the identical
// cancellation into the final add whenever the true moment is small next to
// n·m_a·m_b. Stata pays the same price in quadcross.

namespace {

// A double-double accumulator: value == hi + lo, with |lo| <= ulp(hi)/2.
struct DDAcc {
    double hi = 0.0;
    double lo = 0.0;
};

// s += a*b, exactly: FMA recovers the product's rounding residual, Knuth's
// two-sum recovers the addition's. Only `lo` is ever rounded, so the accumulated
// error is O(n·eps^2) instead of O(n·eps).
inline void dd_fma(DDAcc& s, double a, double b) {
    const double p = a * b;
    const double pe = std::fma(a, b, -p);  // exact: a*b - p
    const double t = s.hi + p;
    const double bv = t - s.hi;
    s.lo += (s.hi - (t - bv)) + (p - bv) + pe;
    s.hi = t;
}

// s += x, exactly (the plain-sum entry point).
inline void dd_add(DDAcc& s, double x) {
    const double t = s.hi + x;
    const double bv = t - s.hi;
    s.lo += (s.hi - (t - bv)) + (x - bv);
    s.hi = t;
}

inline void dd_merge(DDAcc& d, const DDAcc& s) {
    const double t = d.hi + s.hi;
    const double bv = t - d.hi;
    d.lo += (d.hi - (t - bv)) + (s.hi - bv) + s.lo;
    d.hi = t;
}

inline double dd_val(const DDAcc& s) { return s.hi + s.lo; }

// FIXED lane count — TOLERANCE §4.2 forbids a reduction width derived from a
// thread count or a runtime partition, because that makes the DAG a function of
// the machine instead of the data. Four independent chains hide the ~15-cycle
// two-sum latency; the tail and the merge order are fixed, so the result is a
// pure function of (a, b, n).
constexpr int kDDLanes = 4;

double dd_dot(const double* a, const double* b, int64_t n) {
    DDAcc s[kDDLanes];
    int64_t i = 0;
    for (; i + kDDLanes <= n; i += kDDLanes) {
        dd_fma(s[0], a[i + 0], b[i + 0]);
        dd_fma(s[1], a[i + 1], b[i + 1]);
        dd_fma(s[2], a[i + 2], b[i + 2]);
        dd_fma(s[3], a[i + 3], b[i + 3]);
    }
    for (; i < n; ++i) dd_fma(s[0], a[i], b[i]);
    dd_merge(s[0], s[1]);
    dd_merge(s[2], s[3]);
    dd_merge(s[0], s[2]);
    return dd_val(s[0]);
}

double dd_sum(const double* a, int64_t n) {
    DDAcc s[kDDLanes];
    int64_t i = 0;
    for (; i + kDDLanes <= n; i += kDDLanes) {
        dd_add(s[0], a[i + 0]);
        dd_add(s[1], a[i + 1]);
        dd_add(s[2], a[i + 2]);
        dd_add(s[3], a[i + 3]);
    }
    for (; i < n; ++i) dd_add(s[0], a[i]);
    dd_merge(s[0], s[1]);
    dd_merge(s[2], s[3]);
    dd_merge(s[0], s[2]);
    return dd_val(s[0]);
}

}  // namespace

void xtx(const double* X, int64_t n, int k, double* A) {
    engctr::addDdAccum();
    for (int j = 0; j < k; ++j) {
        const double* xj = X + static_cast<int64_t>(j) * n;
        for (int i = 0; i <= j; ++i) {
            const double v = dd_dot(X + static_cast<int64_t>(i) * n, xj, n);
            A[i + j * k] = v;
        }
    }
    symmetrize_upper_to_full(A, k);
}

void xtv(const double* X, int64_t n, int k, const double* v, double* out) {
    engctr::addDdAccum();
    for (int j = 0; j < k; ++j)
        out[j] = dd_dot(X + static_cast<int64_t>(j) * n, v, n);
}

void residual(const double* X, int64_t n, int k, const double* beta, const double* y,
              double* e) {
    engctr::addDdAccum();
    // Row-blocked so the column-major walk stays sequential in BOTH loops while
    // each row keeps its own dd accumulator. y[i] is seeded exactly (hi=y, lo=0)
    // and only the final dd_val rounds — which is the whole point: a residual is
    // a small difference of large numbers, and rounding the fitted value before
    // subtracting is exactly the lost-digit path RSS then squares.
    constexpr int64_t kBlock = 512;
    DDAcc acc[kBlock];
    for (int64_t s = 0; s < n; s += kBlock) {
        const int64_t m = std::min(kBlock, n - s);
        for (int64_t q = 0; q < m; ++q) {
            acc[q].hi = y[s + q];
            acc[q].lo = 0.0;
        }
        for (int j = 0; j < k; ++j) {
            const double* xj = X + static_cast<int64_t>(j) * n + s;
            const double nbj = -beta[j];
            for (int64_t q = 0; q < m; ++q) dd_fma(acc[q], xj[q], nbj);
        }
        for (int64_t q = 0; q < m; ++q) e[s + q] = dd_val(acc[q]);
    }
}

double dot(const double* a, const double* b, int64_t n) {
    engctr::addDdAccum();
    return dd_dot(a, b, n);
}

double sumsq(const double* a, int64_t n) {
    engctr::addDdAccum();
    return dd_dot(a, a, n);
}

double sumsq_dev(const double* a, int64_t n, double center) {
    engctr::addDdAccum();
    DDAcc s[kDDLanes];
    int64_t i = 0;
    for (; i + kDDLanes <= n; i += kDDLanes) {
        const double d0 = a[i + 0] - center, d1 = a[i + 1] - center;
        const double d2 = a[i + 2] - center, d3 = a[i + 3] - center;
        dd_fma(s[0], d0, d0);
        dd_fma(s[1], d1, d1);
        dd_fma(s[2], d2, d2);
        dd_fma(s[3], d3, d3);
    }
    for (; i < n; ++i) {
        const double d = a[i] - center;
        dd_fma(s[0], d, d);
    }
    dd_merge(s[0], s[1]);
    dd_merge(s[2], s[3]);
    dd_merge(s[0], s[2]);
    return dd_val(s[0]);
}

double sum(const double* a, int64_t n) {
    engctr::addDdAccum();
    return dd_sum(a, n);
}

void matvec(const double* A, int k, const double* v, double* out) {
    cblas_dgemv(CblasColMajor, CblasNoTrans, k, k, 1.0, A, k, v, 1, 0.0, out, 1);
}

void mat_vec(const double* A, int64_t m, int n, const double* x, double* out) {
    cblas_dgemv(CblasColMajor, CblasNoTrans, static_cast<int>(m), n, 1.0, A,
                static_cast<int>(m), x, 1, 0.0, out, 1);
}

void gemm_nt(const double* A, const double* B, double* C, int m, int kk, int n) {
    cblas_dgemm(CblasColMajor, CblasNoTrans, CblasTrans, m, n, kk, 1.0, A, m, B, n, 0.0,
                C, m);
}

int spd_inverse(double* A, int k) {
#ifdef __APPLE__
    la_int n = k, lda = k, info = 0;
    char uplo = 'U';
    dpotrf_(&uplo, &n, A, &lda, &info);
    if (info != 0) return static_cast<int>(info);
    dpotri_(&uplo, &n, A, &lda, &info);
    if (info != 0) return static_cast<int>(info);
#else
    la_int info = LAPACKE_dpotrf(LAPACK_COL_MAJOR, 'U', k, A, k);
    if (info != 0) return static_cast<int>(info);
    info = LAPACKE_dpotri(LAPACK_COL_MAJOR, 'U', k, A, k);
    if (info != 0) return static_cast<int>(info);
#endif
    symmetrize_upper_to_full(A, k);
    return 0;
}

void sandwich(const double* bread, const double* meat, int k, double factor, double* V,
              double* tmp) {
    std::vector<double> owned;
    if (!tmp) {
        owned.resize(static_cast<size_t>(k) * k);
        tmp = owned.data();
    }
    cblas_dgemm(CblasColMajor, CblasNoTrans, CblasNoTrans, k, k, k, 1.0, bread, k, meat,
                k, 0.0, tmp, k);
    cblas_dgemm(CblasColMajor, CblasNoTrans, CblasNoTrans, k, k, k, factor, tmp, k, bread,
                k, 0.0, V, k);
}

int psd_clip(double* A, int k) {
    std::vector<double> w(k);
#ifdef __APPLE__
    char jobz = 'V', uplo = 'U';
    la_int n = k, lda = k, info = 0, lwork = -1, liwork = -1;
    double wkopt = 0;
    la_int iwkopt = 0;
    dsyevd_(&jobz, &uplo, &n, A, &lda, w.data(), &wkopt, &lwork, &iwkopt, &liwork, &info);
    lwork = static_cast<la_int>(wkopt);
    liwork = iwkopt;
    std::vector<double> work(std::max<la_int>(lwork, 1));
    std::vector<la_int>  iwork(std::max<la_int>(liwork, 1));
    dsyevd_(&jobz, &uplo, &n, A, &lda, w.data(), work.data(), &lwork, iwork.data(),
            &liwork, &info);
    if (info != 0) return -1;
#else
    la_int info = LAPACKE_dsyevd(LAPACK_COL_MAJOR, 'V', 'U', k, A, k, w.data());
    if (info != 0) return -1;
#endif
    std::vector<double> R(static_cast<size_t>(k) * k, 0.0);
    int clipped = 0;
    for (int j = 0; j < k; ++j) {
        double lam = w[j];
        if (lam < 0) { ++clipped; continue; }
        const double* vj = A + static_cast<int64_t>(j) * k;
        for (int c = 0; c < k; ++c)
            for (int r = 0; r < k; ++r) R[r + c * k] += lam * vj[r] * vj[c];
    }
    for (int i = 0; i < k * k; ++i) A[i] = R[i];
    return clipped;
}

int sym_eig(double* A, int k, double* w) {
#ifdef __APPLE__
    char jobz = 'V', uplo = 'U';
    la_int n = k, lda = k, info = 0, lwork = -1, liwork = -1;
    double wkopt = 0;
    la_int iwkopt = 0;
    dsyevd_(&jobz, &uplo, &n, A, &lda, w, &wkopt, &lwork, &iwkopt, &liwork, &info);
    lwork = static_cast<la_int>(wkopt);
    liwork = iwkopt;
    std::vector<double> work(std::max<la_int>(lwork, 1));
    std::vector<la_int> iwork(std::max<la_int>(liwork, 1));
    dsyevd_(&jobz, &uplo, &n, A, &lda, w, work.data(), &lwork, iwork.data(), &liwork, &info);
    return info == 0 ? 0 : -1;
#else
    return LAPACKE_dsyevd(LAPACK_COL_MAJOR, 'V', 'U', k, A, k, w) == 0 ? 0 : -1;
#endif
}

int svd(double* A, int m, int n, double* U, double* s, double* Vt) {
#ifdef __APPLE__
    char jobu = 'A', jobvt = 'A';
    la_int M = m, N = n, lda = m, ldu = m, ldvt = n, info = 0, lwork = -1;
    double wkopt = 0;
    dgesvd_(&jobu, &jobvt, &M, &N, A, &lda, s, U, &ldu, Vt, &ldvt, &wkopt, &lwork, &info);
    lwork = static_cast<la_int>(wkopt);
    std::vector<double> work(std::max<la_int>(lwork, 1));
    dgesvd_(&jobu, &jobvt, &M, &N, A, &lda, s, U, &ldu, Vt, &ldvt, work.data(), &lwork, &info);
    return info == 0 ? 0 : -1;
#else
    std::vector<double> superb(std::max(1, std::min(m, n) - 1));
    return LAPACKE_dgesvd(LAPACK_COL_MAJOR, 'A', 'A', m, n, A, m, s, U, m, Vt, n,
                          superb.data()) == 0 ? 0 : -1;
#endif
}

int gen_inverse(double* A, int k) {
#ifdef __APPLE__
    std::vector<la_int> ipiv(k);
    la_int n = k, lda = k, info = 0;
    dgetrf_(&n, &n, A, &lda, ipiv.data(), &info);
    if (info != 0) return -1;
    la_int lwork = -1;
    double wkopt = 0;
    dgetri_(&n, A, &lda, ipiv.data(), &wkopt, &lwork, &info);
    lwork = static_cast<la_int>(wkopt);
    std::vector<double> work(std::max<la_int>(lwork, 1));
    dgetri_(&n, A, &lda, ipiv.data(), work.data(), &lwork, &info);
    return info == 0 ? 0 : -1;
#else
    std::vector<lapack_int> ipiv(k);
    if (LAPACKE_dgetrf(LAPACK_COL_MAJOR, k, k, A, k, ipiv.data()) != 0) return -1;
    return LAPACKE_dgetri(LAPACK_COL_MAJOR, k, A, k, ipiv.data()) == 0 ? 0 : -1;
#endif
}

}  // namespace cecon::la
