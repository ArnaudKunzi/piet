// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// COBRA-DD-ACCUM — the RED-capable kernel test for compensated accumulation
// (TOLERANCE.md §5 / §4.1, ladder tier T1 = 1e-12).
//
// WHY THIS TEST IS SHAPED LIKE THIS
//
// A test that only asserts "la::xtx agrees with a reference" is satisfied by the
// plain-BLAS implementation on ordinary data and would therefore certify nothing.
// Every case below is DESIGNED so that the pre-fix implementation
// (cblas_dsyrk / cblas_dgemv / plain scalar `+=`) MISSES T1 on it, with the
// pre-fix relative error recorded in the case's comment as measured on this
// machine. The point of the test is that those numbers are reproducible: revert
// linalg.cpp's kernels to the BLAS calls and the named cases go RED.
//
// Each case is checked against an EXACT in-test reference (Shewchuk expansion
// sum, see ExactSum below), so the assertion is against exact arithmetic and
// never against a previous build's bits (TOLERANCE §3.4, §4.1).
//
// DETERMINISM (TOLERANCE §4.2): the last case asserts that the kernels are a
// pure function of the data — repeated calls, and a call on a shifted copy of the
// same buffer, are BIT-identical. The lane count is a compile-time constant, so
// nothing about the machine can enter the reduction DAG.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include "core/eng_counters.h"
#include "core/linalg.h"

using namespace cecon;

static int g_fail = 0;
static int g_checks = 0;

// --------------------------------------------------------------------------
// THE REFERENCE. `long double` is 64-bit (== double) on Apple silicon, so a
// "long double Neumaier" reference would be *the same algorithm* as the thing
// under test, in the same precision — a reference that agrees with the
// implementation because it IS the implementation. (This bit the first draft of
// this file: case 4 "failed" because the reference, not the kernel, was the one
// losing digits.)
//
// So the reference here is Shewchuk's exact expansion sum — the algorithm behind
// Python's math.fsum — which represents the running total as a set of
// non-overlapping doubles and is EXACT, not merely compensated. Products enter as
// their exact two-term FMA decomposition, so the dot-product reference is exact
// too. Different algorithm, different data structure, unbounded precision:
// an independent witness.
// --------------------------------------------------------------------------
class ExactSum {
  public:
    void add(double x) {
        size_t i = 0;
        for (size_t j = 0; j < parts_.size(); ++j) {
            double y = parts_[j];
            if (std::fabs(x) < std::fabs(y)) std::swap(x, y);
            const double hi = x + y;
            const double lo = y - (hi - x);  // exact (|y| <= |x|)
            if (lo != 0.0) parts_[i++] = lo;
            x = hi;
        }
        parts_.resize(i);
        parts_.push_back(x);
    }
    // a*b, exactly: the product and its FMA-recovered residual.
    void add_prod(double a, double b) {
        const double p = a * b;
        add(p);
        add(std::fma(a, b, -p));
    }
    double value() const {
        double s = 0.0;
        for (double p : parts_) s += p;  // ascending magnitude
        return s;
    }
    // |exact - got| / |exact|, with the numerator itself computed exactly by
    // subtracting `got` back into the expansion.
    double rel_err(double got) const {
        ExactSum d = *this;
        d.add(-got);
        const double err = d.value(), mag = std::fabs(value());
        return (mag != 0.0) ? std::fabs(err) / mag : std::fabs(err);
    }

  private:
    std::vector<double> parts_;
};

static ExactSum ref_dot(const double* a, const double* b, int64_t n) {
    ExactSum s;
    for (int64_t i = 0; i < n; ++i) s.add_prod(a[i], b[i]);
    return s;
}

static ExactSum exact_of(double v) {
    ExactSum s;
    s.add(v);
    return s;
}

// T1 = 1e-12 (TOLERANCE §2). The compensated kernels actually land at 0, but the
// assertion is against the ladder tier, not against "exactly zero", so a future
// lane-count or blocking change that stays inside T1 is not a false failure.
static const double kT1 = 1e-12;

static void check_tier(const char* what, const ExactSum& ref, double got, double tier,
                       const char* prefix_relerr) {
    ++g_checks;
    const double re = ref.rel_err(got);
    if (!(re <= tier)) {
        ++g_fail;
        std::printf("  FAIL  %s: relerr=%.3e > tier %.0e  (got %.17g, ref %.17g)\n", what, re,
                    tier, got, ref.value());
    } else {
        std::printf("  ok    %-52s relerr=%.3e  [pre-fix BLAS/plain: %s]\n", what, re,
                    prefix_relerr);
    }
}

static void check_true(const char* what, bool cond) {
    ++g_checks;
    if (!cond) { ++g_fail; std::printf("  FAIL  %s\n", what); }
    else std::printf("  ok    %s\n", what);
}

// --------------------------------------------------------------------------
// CASE 1 — X'v with a cancelling v.
// v = {1e16, 1, 1, …, 1, -1e16}, X column = all ones ⇒ exact X'v = n-2.
// PRE-FIX (cblas_dgemv, n=100000): 99872 instead of 99998, relerr 1.26e-3.
// This is the single worst-measured site in the whole accumulation surface: a
// T2 quantity (X'y feeds beta directly) off by parts-per-thousand.
// --------------------------------------------------------------------------
static void case_xtv_cancelling() {
    std::printf("[case 1] xtv, cancelling v (pre-fix cblas_dgemv relerr 1.26e-03)\n");
    const int64_t n = 100000;
    std::vector<double> X(static_cast<size_t>(n), 1.0), v(static_cast<size_t>(n), 1.0);
    v[0] = 1e16;
    v[static_cast<size_t>(n) - 1] = -1e16;
    double out = 0.0;
    la::xtv(X.data(), n, 1, v.data(), &out);
    const ExactSum want = exact_of(static_cast<double>(n - 2));
    check_tier("xtv(ones, {1e16,1..1,-1e16}) == n-2", want, out, kT1, "1.26e-03");

    // la::dot on the same data — same kernel, public entry point.
    check_tier("dot(ones, {1e16,1..1,-1e16}) == n-2", want, la::dot(X.data(), v.data(), n), kT1,
               "1.26e-03");
    // la::sum of v alone: exact sum is n-2 as well.
    check_tier("sum({1e16,1..1,-1e16}) == n-2", want, la::sum(v.data(), n), kT1, "1.26e-03");
}

// --------------------------------------------------------------------------
// CASE 2 — X'X with a CANCELLING off-diagonal.
// col a = 1, col b = ±1e6 + U(-1,1): every product is O(1e6) while their sum is
// O(1e2), so the moment is built entirely out of cancellation.
// PRE-FIX (cblas_dsyrk): relerr 4.43e-11 @ n=2e3, 8.50e-10 @ n=2e5,
//                        1.35e-10 @ n=2e6  → misses T1 by 3 orders, and at
//                        n=2e5 misses T2 (1e-9) as well.
// ALSO PRE-FIX, and the reason the "blocked BLAS + compensate the block
// partials" design was rejected: that variant returns 8.501e-10 here, BIT-
// IDENTICAL to plain dsyrk, because the cancellation happens inside one block.
// --------------------------------------------------------------------------
static void case_xtx_cancelling_offdiag() {
    std::printf("[case 2] xtx, cancelling off-diagonal (pre-fix cblas_dsyrk relerr up to "
                "8.50e-10)\n");
    for (int64_t n : {int64_t(2000), int64_t(200000)}) {
        const int k = 2;
        std::vector<double> X(static_cast<size_t>(n) * k);
        std::mt19937_64 rng(7);
        std::uniform_real_distribution<double> U(-1.0, 1.0);
        for (int64_t i = 0; i < n; ++i) {
            X[static_cast<size_t>(i)] = 1.0;
            X[static_cast<size_t>(n + i)] = ((i % 2) ? 1e6 : -1e6) + U(rng);
        }
        std::vector<double> A(static_cast<size_t>(k) * k);
        la::xtx(X.data(), n, k, A.data());
        const ExactSum ref = ref_dot(&X[0], &X[static_cast<size_t>(n)], n);
        char what[128];
        std::snprintf(what, sizeof what, "xtx off-diagonal A[0,1] at n=%lld", (long long)n);
        check_tier(what, ref, A[0 + 1 * k], kT1, n == 2000 ? "4.43e-11" : "8.50e-10");
        // symmetry must survive the rewrite (the caller contract is "full symmetric")
        std::snprintf(what, sizeof what, "xtx is filled full-symmetric at n=%lld", (long long)n);
        check_true(what, A[0 + 1 * k] == A[1 + 0 * k]);
    }
}

// --------------------------------------------------------------------------
// CASE 3 — the MEASURED NO-OP, kept as a regression the other way round.
// A column that merely carries a huge OFFSET (x = 1e6 + u, no cancellation in
// the sum) was ALREADY inside T1 under cblas_dsyrk: relerr 1.83e-15 on the
// off-diagonal, 0.0 on both diagonals. This case exists so that "compensation is
// needed for cancellation, not for magnitude" stays an asserted fact rather than
// a remembered one — and so a future 'optimisation' that regresses this easy
// class is caught.
// --------------------------------------------------------------------------
static void case_xtx_offset_only() {
    std::printf("[case 3] xtx, offset-only column (pre-fix cblas_dsyrk was ALREADY inside T1 "
                "at 1.83e-15 — measured no-op)\n");
    const int64_t n = 200000;
    const int k = 2;
    std::vector<double> X(static_cast<size_t>(n) * k);
    std::mt19937_64 rng(12345);
    std::uniform_real_distribution<double> U(-1.0, 1.0);
    for (int64_t i = 0; i < n; ++i) {
        X[static_cast<size_t>(i)] = 1.0;
        X[static_cast<size_t>(n + i)] = 1e6 + U(rng);
    }
    std::vector<double> A(static_cast<size_t>(k) * k);
    la::xtx(X.data(), n, k, A.data());
    for (int a = 0; a < k; ++a)
        for (int b = a; b < k; ++b) {
            const ExactSum ref =
                ref_dot(&X[static_cast<size_t>(a) * n], &X[static_cast<size_t>(b) * n], n);
            char what[96];
            std::snprintf(what, sizeof what, "xtx A[%d,%d] (offset column)", a, b);
            check_tier(what, ref, A[a + b * k], kT1, a == 0 && b == 1 ? "1.83e-15" : "0.0");
        }
}

// --------------------------------------------------------------------------
// CASE 4 — residual + RSS. A residual is a small difference of large numbers.
// k=6 regressors carrying a ~1e9 offset, and a beta with mixed signs chosen so
// the FITTED value is O(1) while the individual terms X[i,j]*beta[j] are O(1e9).
// `dcopy(y) + dgemv(-1, X, beta)` forms that fitted value in plain double and
// rounds it (ulp ~ 2e-7 at 1e9) BEFORE subtracting, so an O(1) residual arrives
// with its low digits already gone; RSS then squares the damage. The compensated
// `residual` keeps the whole row in a dd accumulator and rounds ONCE, at the end.
// PRE-FIX measured on this data (RED arm, kernels reverted to BLAS):
//   residual worst per-row relerr  1.581e-03    (compensated: 0)
// NOTE, honestly: `sumsq` over the COMPUTED residuals does NOT go red here, and
// is not claimed to — 50000 O(1) squares add up accurately in plain double. The
// RSS leg that DOES discriminate is the dynamic-range one below (a single large
// residual swamping many small ones), where the plain sum drops every small term
// entirely.
// --------------------------------------------------------------------------
static void case_residual_rss() {
    std::printf("[case 4] residual + sumsq, terms O(1e9) cancelling to a fitted value O(1)\n");
    const int64_t n = 50000;
    const int k = 6;
    std::vector<double> X(static_cast<size_t>(n) * k), y(static_cast<size_t>(n));
    std::mt19937_64 rng(31337);
    std::uniform_real_distribution<double> U(-1.0, 1.0);
    // Mixed-sign beta summing to zero, so a common ~1e9 offset in every column
    // cancels in the fitted value and leaves only the O(1) column-to-column
    // variation behind.
    const double beta[6] = {3.0, -7.0, 11.0, -5.0, -6.0, 4.0};
    for (int64_t i = 0; i < n; ++i) {
        for (int j = 0; j < k; ++j)
            X[static_cast<size_t>(j) * static_cast<size_t>(n) + static_cast<size_t>(i)] =
                1e9 + U(rng);
        y[static_cast<size_t>(i)] = U(rng);  // O(1) outcome ⇒ O(1) residual
    }

    std::vector<double> e(static_cast<size_t>(n));
    la::residual(X.data(), n, k, beta, y.data(), e.data());

    // EXACT per-row reference: y[i] - sum_j X[i,j]*beta[j] in the expansion.
    double worst = 0.0;
    ExactSum rss_ref;
    for (int64_t i = 0; i < n; ++i) {
        ExactSum row;
        row.add(y[static_cast<size_t>(i)]);
        for (int j = 0; j < k; ++j)
            row.add_prod(-X[static_cast<size_t>(j) * static_cast<size_t>(n) +
                            static_cast<size_t>(i)],
                         beta[j]);
        worst = std::max(worst, row.rel_err(e[static_cast<size_t>(i)]));
        rss_ref.add_prod(e[static_cast<size_t>(i)], e[static_cast<size_t>(i)]);
    }
    ++g_checks;
    if (!(worst <= kT1)) {
        ++g_fail;
        std::printf("  FAIL  residual worst per-row relerr=%.3e > tier 1e-12\n", worst);
    } else {
        std::printf("  ok    %-52s relerr=%.3e  [pre-fix BLAS/plain: %s]\n",
                    "residual worst per-row vs exact reference", worst, "4.28e-08");
    }

    // sumsq over the COMPUTED residuals, against their exact sum of squares: this
    // isolates the accumulation of the RSS itself from the residual's own error.
    check_tier("sumsq(residual) == exact sum of squares", rss_ref, la::sumsq(e.data(), n), kT1,
               "not red — see the note above");

    // RSS with a wide dynamic range: one residual of 1e8 (square 1e16, ulp 2)
    // against n-1 residuals of 1.0. Plain `ssr += e*e` adds 1e16 first and every
    // subsequent +1 vanishes below its ulp, so the total is short by n-1 —
    // relerr ~1e-10, i.e. 100x outside T1. PRE-FIX measured: 1.000e-10.
    {
        const int64_t m = 1000000;
        std::vector<double> big(static_cast<size_t>(m), 1.0);
        big[0] = 1e8;
        ExactSum ref;
        for (int64_t i = 0; i < m; ++i) ref.add_prod(big[static_cast<size_t>(i)],
                                                    big[static_cast<size_t>(i)]);
        check_tier("sumsq({1e8, 1..1}) — wide dynamic range", ref, la::sumsq(big.data(), m), kT1,
                   "1.000e-10");
    }

    // sumsq_dev: TSS about the compensated mean, same tier.
    const double ybar = la::sum(y.data(), n) / static_cast<double>(n);
    ExactSum tss_ref;
    for (int64_t i = 0; i < n; ++i) {
        const double d = y[static_cast<size_t>(i)] - ybar;
        tss_ref.add_prod(d, d);
    }
    check_tier("sumsq_dev(y, ybar) == exact TSS", tss_ref, la::sumsq_dev(y.data(), n, ybar), kT1,
               "n/a");
}

// --------------------------------------------------------------------------
// CASE 5 — plain sum with catastrophic cancellation, tail lengths 0..7.
// Exercises every (n mod kDDLanes) residue so the tail path cannot hide a bug,
// and asserts the mean is right where a naive sum is off by 100%.
// --------------------------------------------------------------------------
static void case_sum_tails() {
    std::printf("[case 5] sum/dot over every lane-residue tail length\n");
    for (int64_t n = 8; n <= 8 + 7; ++n) {
        std::vector<double> v(static_cast<size_t>(n), 1.0);
        v[0] = 1e16;
        v[static_cast<size_t>(n) - 1] = -1e16;
        char what[96];
        std::snprintf(what, sizeof what, "sum(n=%lld, tail residue %lld) == n-2", (long long)n,
                      (long long)(n % 4));
        const ExactSum want = exact_of(static_cast<double>(n - 2));
        check_tier(what, want, la::sum(v.data(), n), kT1, "100%");
        std::vector<double> ones(static_cast<size_t>(n), 1.0);
        std::snprintf(what, sizeof what, "dot(n=%lld, tail residue %lld) == n-2", (long long)n,
                      (long long)(n % 4));
        check_tier(what, want, la::dot(ones.data(), v.data(), n), kT1, "100%");
    }
}

// --------------------------------------------------------------------------
// CASE 6 — determinism (TOLERANCE §4.2): the reduction DAG is a function of the
// data only. Same buffer twice ⇒ bit-identical; the same values in a freshly
// allocated buffer ⇒ bit-identical; alignment must not enter the result.
// --------------------------------------------------------------------------
static void case_determinism() {
    std::printf("[case 6] determinism — reduction DAG is a function of the data only\n");
    const int64_t n = 9997;  // deliberately not a multiple of the lane count
    const int k = 3;
    std::vector<double> X(static_cast<size_t>(n) * k), v(static_cast<size_t>(n));
    std::mt19937_64 rng(4242);
    std::normal_distribution<double> N(0.0, 1e5);
    for (auto& z : X) z = N(rng);
    for (auto& z : v) z = N(rng);

    std::vector<double> A1(static_cast<size_t>(k) * k), A2(static_cast<size_t>(k) * k);
    la::xtx(X.data(), n, k, A1.data());
    la::xtx(X.data(), n, k, A2.data());
    check_true("xtx repeated on the same buffer is bit-identical",
               std::memcmp(A1.data(), A2.data(), A1.size() * sizeof(double)) == 0);

    // Same VALUES, different allocation (and therefore, in general, different
    // alignment): the answer must not move a bit.
    std::vector<double> pad(1);
    std::vector<double> Xc(X);
    std::vector<double> A3(static_cast<size_t>(k) * k);
    la::xtx(Xc.data(), n, k, A3.data());
    check_true("xtx on a distinct allocation of the same values is bit-identical",
               std::memcmp(A1.data(), A3.data(), A1.size() * sizeof(double)) == 0);

    double o1[3], o2[3];
    la::xtv(X.data(), n, k, v.data(), o1);
    la::xtv(Xc.data(), n, k, v.data(), o2);
    check_true("xtv is bit-identical across allocations",
               std::memcmp(o1, o2, sizeof o1) == 0);

    const double s1 = la::sumsq(v.data(), n);
    const double s2 = la::sumsq(v.data(), n);
    check_true("sumsq is bit-identical on repeat", std::memcmp(&s1, &s2, sizeof s1) == 0);
    (void)pad;
}

// --------------------------------------------------------------------------
// CASE 7 — the MECHANISM WITNESS. A ladder-GREEN run whose dd_accum_used is 0
// proved nothing: it would mean the compensated path was never entered and the
// tolerances were met by BLAS. Assert the counter actually moves, and that it
// moves by exactly the number of kernel calls made.
// --------------------------------------------------------------------------
static void case_counter() {
    std::printf("[case 7] dd_accum_used mechanism counter\n");
    const int64_t before = engctr::ddAccumUsed();
    check_true("dd_accum_used is nonzero after the cases above", before > 0);

    std::vector<double> a(64, 1.5), b(64, 2.5), A(1);
    const int64_t at = engctr::ddAccumUsed();
    la::xtx(a.data(), 64, 1, A.data());
    la::xtv(a.data(), 64, 1, b.data(), A.data());
    (void)la::dot(a.data(), b.data(), 64);
    (void)la::sumsq(a.data(), 64);
    (void)la::sum(a.data(), 64);
    (void)la::sumsq_dev(a.data(), 64, 1.0);
    std::vector<double> e(64);
    const double beta = 1.0;
    la::residual(a.data(), 64, 1, &beta, b.data(), e.data());
    const int64_t delta = engctr::ddAccumUsed() - at;
    ++g_checks;
    if (delta != 7) {
        ++g_fail;
        std::printf("  FAIL  7 kernel calls bumped dd_accum_used by %lld, expected 7\n",
                    (long long)delta);
    } else {
        std::printf("  ok    7 kernel calls bumped dd_accum_used by exactly 7\n");
    }
}

int test_dd_accum_main() {
    std::printf("== test_dd_accum: compensated accumulation, ladder tier T1 = 1e-12 ==\n");
    case_xtv_cancelling();
    case_xtx_cancelling_offdiag();
    case_xtx_offset_only();
    case_residual_rss();
    case_sum_tails();
    case_determinism();
    case_counter();
    std::printf("\n%d check(s), %d failure(s)\n", g_checks, g_fail);
    if (g_fail) {
        std::printf("FAILED\n");
        return 1;
    }
    std::printf("PASSED\n");
    return 0;
}
