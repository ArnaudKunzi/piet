// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "estimators/aer_smooth.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numeric>

namespace cecon {

namespace {
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
}

// ---------------------------------------------------------------------------
// Hodrick-Prescott

HpResult hp_filter(const std::vector<double>& y, double lambda) {
    const std::size_t T = y.size();
    HpResult r;
    r.cycle.assign(T, kNaN);
    r.trend.assign(T, kNaN);
    if (T < 3) return r;

    // M = I + lambda*K'K, accumulated row by row from K itself. Each row i of K
    // is (1, -2, 1) at columns i, i+1, i+2, so it contributes a 3x3 outer
    // product; summing those IS K'K, and at T=3 the two overlapping patterns add
    // to 1+4*lambda in the middle rather than the template's 1+5*lambda.
    std::vector<double> d(T, 1.0), u1(T > 1 ? T - 1 : 0, 0.0), u2(T > 2 ? T - 2 : 0, 0.0);
    for (std::size_t i = 0; i + 2 < T; ++i) {
        d[i] += lambda * 1.0;
        d[i + 1] += lambda * 4.0;
        d[i + 2] += lambda * 1.0;
        u1[i] += lambda * -2.0;
        u1[i + 1] += lambda * -2.0;
        u2[i] += lambda * 1.0;
    }

    // Banded LDL' with bandwidth 2. l1[i] = L[i+1][i], l2[i] = L[i+2][i].
    std::vector<double> D(T, 0.0), l1(T > 1 ? T - 1 : 0, 0.0), l2(T > 2 ? T - 2 : 0, 0.0);
    for (std::size_t i = 0; i < T; ++i) {
        double s = d[i];
        if (i >= 1) s -= l1[i - 1] * l1[i - 1] * D[i - 1];
        if (i >= 2) s -= l2[i - 2] * l2[i - 2] * D[i - 2];
        D[i] = s;
        if (i + 1 < T) {
            double t = u1[i];
            if (i >= 1) t -= l2[i - 1] * l1[i - 1] * D[i - 1];
            l1[i] = t / D[i];
        }
        if (i + 2 < T) {
            l2[i] = u2[i] / D[i];
        }
    }

    // L z = y, D w = z, L' tau = w.
    std::vector<double> z(T, 0.0);
    for (std::size_t i = 0; i < T; ++i) {
        double s = y[i];
        if (i >= 1) s -= l1[i - 1] * z[i - 1];
        if (i >= 2) s -= l2[i - 2] * z[i - 2];
        z[i] = s;
    }
    for (std::size_t i = 0; i < T; ++i) z[i] /= D[i];
    std::vector<double> tau(T, 0.0);
    for (std::size_t k = T; k-- > 0;) {
        double s = z[k];
        if (k + 1 < T) s -= l1[k] * tau[k + 1];
        if (k + 2 < T) s -= l2[k] * tau[k + 2];
        tau[k] = s;
    }
    for (std::size_t i = 0; i < T; ++i) {
        r.trend[i] = tau[i];
        r.cycle[i] = y[i] - tau[i];
    }
    return r;
}

double hp_lambda_for_format(const std::string& fmt) {
    // Match the LEADING format letters; a real format carries width/detail after
    // them (`%tq`, `%tqCCYY!qQ`, ...), so an exact compare would miss most of them.
    auto starts = [&fmt](const char* p) { return fmt.rfind(p, 0) == 0; };
    if (starts("%ty")) return 6.25;
    if (starts("%th")) return 100.0;
    if (starts("%tq")) return 1600.0;
    if (starts("%tm")) return 129600.0;
    if (starts("%tw")) return 1600.0 * 12.0 * 12.0 * 12.0 * 12.0;  // 33,177,600
    if (starts("%td")) {
        const double q = 365.0 / 4.0;  // 91.25, exactly representable
        return 1600.0 * q * q * q * q;  // 110,930,628,906.25
    }
    return -1.0;
}

// ---------------------------------------------------------------------------
// LOWESS

double lowess_effective_bwidth(double requested) {
    return (requested <= 0.0 || requested >= 1.0) ? 0.8 : requested;
}

std::vector<double> lowess_fit(const std::vector<double>& x, const std::vector<double>& y,
                               const LowessOpts& o) {
    const std::size_t n = x.size();
    std::vector<double> out(n, kNaN);
    if (n < 3) return out;  // Stata's lowess guard: all-missing, not an error
    if (x.back() - x.front() < 1e-30) return out;

    const double bw = lowess_effective_bwidth(o.bwidth);
    const long long k =
        static_cast<long long>(std::floor((static_cast<double>(n) * bw - 0.5) / 2.0));
    const long long N = static_cast<long long>(n);

    for (long long i = 0; i < N; ++i) {
        const long long lo = std::max(0LL, i - k);
        const long long hi = std::min(N - 1, i + k);
        const double xi = x[static_cast<std::size_t>(i)];
        // Delta is 1.0001x the larger half-width of the window, so the point at
        // the far edge gets a weight just above zero rather than exactly zero.
        const double dl = xi - x[static_cast<std::size_t>(lo)];
        const double dr = x[static_cast<std::size_t>(hi)] - xi;
        const double delta = 1.0001 * std::max(dl, dr);

        double sw = 0, swx = 0, swy = 0, swxx = 0, swxy = 0;
        for (long long j = lo; j <= hi; ++j) {
            const double xj = x[static_cast<std::size_t>(j)];
            const double yj = y[static_cast<std::size_t>(j)];
            double w = 1.0;
            if (!o.noweight) {
                if (delta <= 0.0) {
                    w = 1.0;
                } else {
                    const double u = std::fabs(xj - xi) / delta;
                    if (u >= 1.0) continue;
                    const double t = 1.0 - u * u * u;
                    w = t * t * t;
                }
            }
            sw += w;
            swx += w * xj;
            swy += w * yj;
            swxx += w * xj * xj;
            swxy += w * xj * yj;
        }
        if (sw <= 0.0) continue;
        if (o.mean) {
            out[static_cast<std::size_t>(i)] = swy / sw;
            continue;
        }
        const double mx = swx / sw, my = swy / sw;
        const double sxx = swxx - sw * mx * mx;
        const double sxy = swxy - sw * mx * my;
        const double slope = (std::fabs(sxx) < 1e-300) ? 0.0 : sxy / sxx;
        out[static_cast<std::size_t>(i)] = my + slope * (xi - mx);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Local polynomial

namespace {

double kernel_value(LpolyKernel k, double z) {
    const double az = std::fabs(z);
    switch (k) {
        case LpolyKernel::Epanechnikov: {
            const double s = std::sqrt(5.0);
            if (az >= s) return 0.0;
            return 0.75 * (1.0 - z * z / 5.0) / s;
        }
        case LpolyKernel::Epan2:
            return az >= 1.0 ? 0.0 : 0.75 * (1.0 - z * z);
        case LpolyKernel::Biweight: {
            if (az >= 1.0) return 0.0;
            const double t = 1.0 - z * z;
            return (15.0 / 16.0) * t * t;
        }
        case LpolyKernel::Cosine:
            return az >= 0.5 ? 0.0 : 1.0 + std::cos(2.0 * M_PI * z);
        case LpolyKernel::Gaussian:
            return std::exp(-0.5 * z * z) / std::sqrt(2.0 * M_PI);
        case LpolyKernel::Parzen: {
            if (az <= 0.5) return 4.0 / 3.0 - 8.0 * z * z + 8.0 * az * az * az;
            if (az <= 1.0) {
                const double t = 1.0 - az;
                return 8.0 * t * t * t / 3.0;
            }
            return 0.0;
        }
        case LpolyKernel::Rectangle:
            return az >= 1.0 ? 0.0 : 0.5;
        case LpolyKernel::Triangle:
            return az >= 1.0 ? 0.0 : 1.0 - az;
    }
    return 0.0;
}

// C_{0,p}(K) = ((p+1)!^2 R(K*) / (2(p+1) mu_{p+1}(K*)^2))^(1/(2p+3)), the
// Fan-Gijbels constant of the equivalent kernel K*(u) = e1' S^-1 (1,u,..,u^p) K(u),
// with EVERY kernel moment taken by Stata's trapezoid rule rather than
// analytically. That is what makes the constants "Stata-effective": the
// analytic rectangle value 1.350960 is 1.27% away from Stata's. The grid is
// MEASURED (probe_rot_constants capture, 2026-10-15: every kernel x degree 0..3
// on two fixtures, each back-out agreeing with the other to 2.7e-12 at p=1 and
// with this rule to 4.3e-12 at p=1 and 7.5e-11 at p=3):
//   epanechnikov  101 points on [-sqrt(5), sqrt(5)]
//   gaussian      101 points on [-5, 5]
//   every kernel supported on [-1, 1] or inside it (epan2, biweight, cosine,
//                 parzen, rectangle, triangle): 81 points on [-1, 1]
// The 6-digit constants this replaces put the default epanechnikov bandwidth
// 7.1e-8 above Stata (FU-902), outside T2, and the smooth followed.
// Only p=1 and p=3 are pinned that way; rectangle at p=5 keeps its earlier
// live back-out (the trapezoid value there is 2.7e-4 away and unmeasured), and
// anything else returns -1 so the caller refuses by name.
bool spd_solve(std::vector<double>& A, std::vector<double>& b, int m);

double trapezoid_constant(LpolyKernel k, int p) {
    double a = 1.0;
    int npts = 81;
    if (k == LpolyKernel::Epanechnikov) {
        a = std::sqrt(5.0);
        npts = 101;
    } else if (k == LpolyKernel::Gaussian) {
        a = 5.0;
        npts = 101;
    }
    const double step = 2.0 * a / static_cast<double>(npts - 1);
    std::vector<double> u(static_cast<std::size_t>(npts)), wq(static_cast<std::size_t>(npts), step),
        kv(static_cast<std::size_t>(npts));
    wq.front() = wq.back() = step / 2.0;
    for (int i = 0; i < npts; ++i) {
        u[static_cast<std::size_t>(i)] = -a + static_cast<double>(i) * step;
        kv[static_cast<std::size_t>(i)] = kernel_value(k, u[static_cast<std::size_t>(i)]);
    }
    auto upow = [](double v, int j) {
        double r = 1.0;
        for (int t = 0; t < j; ++t) r *= v;
        return r;
    };
    auto moment = [&](int j) {
        double s = 0.0;
        for (int i = 0; i < npts; ++i)
            s += wq[static_cast<std::size_t>(i)] * upow(u[static_cast<std::size_t>(i)], j) *
                 kv[static_cast<std::size_t>(i)];
        return s;
    };
    const int m = p + 1;
    std::vector<double> S(static_cast<std::size_t>(m * m)), e(static_cast<std::size_t>(m), 0.0);
    for (int r = 0; r < m; ++r)
        for (int c = 0; c < m; ++c) S[static_cast<std::size_t>(r * m + c)] = moment(r + c);
    e[0] = 1.0;
    if (!spd_solve(S, e, m)) return -1;
    double R = 0.0, mu = 0.0;
    for (int i = 0; i < npts; ++i) {
        const double ui = u[static_cast<std::size_t>(i)];
        double poly = 0.0;
        for (int j = 0; j < m; ++j) poly += e[static_cast<std::size_t>(j)] * upow(ui, j);
        const double ks = poly * kv[static_cast<std::size_t>(i)];
        R += wq[static_cast<std::size_t>(i)] * ks * ks;
        mu += wq[static_cast<std::size_t>(i)] * upow(ui, p + 1) * ks;
    }
    double fact = 1.0;
    for (int t = 2; t <= p + 1; ++t) fact *= static_cast<double>(t);
    return std::pow(fact * fact * R / (2.0 * (p + 1) * mu * mu), 1.0 / (2.0 * p + 3.0));
}

double rot_constant(LpolyKernel k, int p) {
    if (p == 1 || p == 3) {
        // Computed once per (kernel, p); function-local statics are thread-safe.
        static const std::vector<double> table = [] {
            std::vector<double> t;
            for (int kk = 0; kk <= static_cast<int>(LpolyKernel::Triangle); ++kk)
                for (int pp : {1, 3}) t.push_back(trapezoid_constant(static_cast<LpolyKernel>(kk), pp));
            return t;
        }();
        return table[static_cast<std::size_t>(static_cast<int>(k) * 2 + (p == 3 ? 1 : 0))];
    }
    if (p == 5) return k == LpolyKernel::Rectangle ? 4.337764 : -1;
    return -1;
}

// Solve the (m x m) SPD normal system in place by Cholesky. false = singular.
bool spd_solve(std::vector<double>& A, std::vector<double>& b, int m) {
    for (int i = 0; i < m; ++i) {
        for (int j = 0; j <= i; ++j) {
            double s = A[static_cast<std::size_t>(i * m + j)];
            for (int t = 0; t < j; ++t)
                s -= A[static_cast<std::size_t>(i * m + t)] * A[static_cast<std::size_t>(j * m + t)];
            if (i == j) {
                if (s <= 0.0) return false;
                A[static_cast<std::size_t>(i * m + j)] = std::sqrt(s);
            } else {
                A[static_cast<std::size_t>(i * m + j)] = s / A[static_cast<std::size_t>(j * m + j)];
            }
        }
    }
    for (int i = 0; i < m; ++i) {
        double s = b[static_cast<std::size_t>(i)];
        for (int t = 0; t < i; ++t) s -= A[static_cast<std::size_t>(i * m + t)] * b[static_cast<std::size_t>(t)];
        b[static_cast<std::size_t>(i)] = s / A[static_cast<std::size_t>(i * m + i)];
    }
    for (int i = m - 1; i >= 0; --i) {
        double s = b[static_cast<std::size_t>(i)];
        for (int t = i + 1; t < m; ++t) s -= A[static_cast<std::size_t>(t * m + i)] * b[static_cast<std::size_t>(t)];
        b[static_cast<std::size_t>(i)] = s / A[static_cast<std::size_t>(i * m + i)];
    }
    return true;
}

// The rule of thumb's global polynomial fitted the way Stata fits it: y on the
// RAW powers 1, x, ..., x^deg through cross products accumulated in double in
// row order (rows sorted by x, as lpoly sorts before fitting) and a
// generalized inverse that sets a column to zero once its sweep pivot is
// <= 1e-9 x its original diagonal or below 1e-19 -- invsym's rule as measured
// for e(rank) in estimation_glm.cpp glmVceRank, sweeping the largest absolute
// pivot first. MEASURED on probe_rot_conditioning (internal
// oracle notes, 2026-10-15): Stata's r(bwidth) is NOT translation
// invariant -- x+10 already moves it 1.1e-8 (p=1) and 3e-6 (p=3), x+100 moves
// it 3% (p=1) and 600% (p=3) -- and the dropped columns read off the values
// exactly: x*0.001 at p=3 is the exact fit without x^5 and x^6 (3e-12), x+100
// at p=1 the exact fit without the constant (1e-7). This routine drops the same
// columns in all eight breakdown arms of that probe and lands within 2e-6 of
// Stata there, where a centred QR fit (what this replaced) is 3% to 800% off.
// Where nothing is dropped the answer carries the raw-power conditioning
// noise Stata's own carries (1e-9 at p=3 on the lpl_ci_se_if subsample, which
// is 1.1e-9 from exact arithmetic). The pivot step divides the pivot row once
// and subtracts M[i][k] * row[j] (the variant closest to Stata over that
// probe's nine no-drop arms; Stata's bits are not reproduced there).
// FMA contraction is off here and in lpoly_rot_bandwidth's finish: on the
// lpl_ci_se_if subsample a fused multiply-add build lands 1.42e-9 from Stata,
// outside T2, and an unfused one -1.86e-10 (the conditioning is ~1e12, so one
// rounding per product shows). GCC gets the same through the attribute.
// `w` may be null (unweighted). Returns false if every column is dropped.
// `rss` is sum w e^2.
#if defined(__GNUC__) && !defined(__clang__)
#define CECON_NO_FP_CONTRACT __attribute__((optimize("fp-contract=off")))
#else
#define CECON_NO_FP_CONTRACT
#endif
CECON_NO_FP_CONTRACT
bool rot_poly_fit(const std::vector<double>& x, const std::vector<double>& y,
                  const std::vector<double>* w, int deg, std::vector<double>* beta, double* rss) {
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
    const int m = deg + 1;
    const std::size_t mz = static_cast<std::size_t>(m);
    const std::size_t n = x.size();
    if (n < mz) return false;
    std::vector<double> M(mz * mz, 0.0), b(mz, 0.0), r(mz);
    for (std::size_t i = 0; i < n; ++i) {
        const double wi = w ? (*w)[i] : 1.0;
        for (int j = 0; j < m; ++j) r[static_cast<std::size_t>(j)] = j == 0 ? 1.0 : std::pow(x[i], j);
        for (std::size_t a = 0; a < mz; ++a) {
            const double wa = w ? wi * r[a] : r[a];
            for (std::size_t c = 0; c < mz; ++c) M[a * mz + c] += wa * r[c];
            b[a] += wa * y[i];
        }
    }
    std::vector<double> d0(mz), row(mz);
    std::vector<char> left(mz), swept(mz, 0);
    for (std::size_t j = 0; j < mz; ++j) {
        d0[j] = M[j * mz + j];
        left[j] = d0[j] > 0.0;
    }
    for (;;) {
        std::size_t k = mz;
        for (std::size_t j = 0; j < mz; ++j) {
            if (!left[j]) continue;
            const double p = M[j * mz + j];
            if (p <= 1e-9 * d0[j] || p < 1e-19) continue;
            if (k == mz || p > M[k * mz + k]) k = j;
        }
        if (k == mz) break;
        left[k] = 0;
        swept[k] = 1;
        const double d = M[k * mz + k];
        for (std::size_t j = 0; j < mz; ++j) row[j] = M[k * mz + j] / d;
        for (std::size_t i = 0; i < mz; ++i) {
            if (i == k) continue;
            for (std::size_t j = 0; j < mz; ++j)
                if (j != k) M[i * mz + j] -= M[i * mz + k] * row[j];
        }
        for (std::size_t i = 0; i < mz; ++i) {
            if (i == k) continue;
            M[i * mz + k] /= d;
            M[k * mz + i] /= d;
        }
        M[k * mz + k] = -1.0 / d;
    }
    bool any = false;
    std::vector<double> bt(mz, 0.0);
    for (std::size_t i = 0; i < mz; ++i) {
        if (!swept[i]) continue;
        any = true;
        double v = 0.0;
        for (std::size_t j = 0; j < mz; ++j)
            if (swept[j]) v += -M[i * mz + j] * b[j];
        bt[i] = v;
    }
    if (!any) return false;
    double sum = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        double fit = 0.0;
        for (int j = 0; j < m; ++j)
            fit += bt[static_cast<std::size_t>(j)] * (j == 0 ? 1.0 : std::pow(x[i], j));
        const double e = y[i] - fit;
        sum += (w ? (*w)[i] : 1.0) * e * e;
    }
    *beta = bt;
    *rss = sum;
    return true;
}

// One local polynomial regression at x0 with bandwidth h, in the scaled basis
// u = (x - x0)/h (the intercept, the variance factor and the hat-matrix trace
// are all invariant to that scaling, and it keeps degree p+2 well conditioned).
struct LocalFit {
    bool ok = false;
    double b0 = 0;
    double var_factor = 0;  // e1'(X'WX)^-1 X'W^2X (X'WX)^-1 e1  (want_var)
    double sigma2 = 0;      // sum W r^2 / (tr W - tr((X'WX)^-1 X'W^2X))  (want_sigma2)
};

LocalFit local_fit(const std::vector<double>& x, const std::vector<double>& y,
                   const std::vector<double>* w, LpolyKernel kern, double x0, double h, int deg,
                   bool want_var, bool want_sigma2) {
    LocalFit f;
    const int m = deg + 1;
    const std::size_t n = x.size();
    const std::size_t mm = static_cast<std::size_t>(m) * static_cast<std::size_t>(m);
    std::vector<double> A(mm, 0.0), B(mm, 0.0), b(static_cast<std::size_t>(m), 0.0),
        pw(static_cast<std::size_t>(m), 0.0);
    double wsum = 0;
    int nw = 0;  // observations INSIDE the window (positive kernel weight)
    for (std::size_t i = 0; i < n; ++i) {
        const double u = (x[i] - x0) / h;
        double k = kernel_value(kern, u) / h;
        if (w) k *= (*w)[i];
        if (k <= 0) continue;
        wsum += k;
        ++nw;
        pw[0] = 1.0;
        for (int j = 1; j < m; ++j) pw[static_cast<std::size_t>(j)] = pw[static_cast<std::size_t>(j - 1)] * u;
        for (int rr = 0; rr < m; ++rr) {
            b[static_cast<std::size_t>(rr)] += k * pw[static_cast<std::size_t>(rr)] * y[i];
            for (int cc = 0; cc < m; ++cc) {
                const double pp = pw[static_cast<std::size_t>(rr)] * pw[static_cast<std::size_t>(cc)];
                A[static_cast<std::size_t>(rr * m + cc)] += k * pp;
                if (want_var || want_sigma2) B[static_cast<std::size_t>(rr * m + cc)] += k * k * pp;
            }
        }
    }
    // BUG-416: a window holding ONE observation is left MISSING, not fitted.
    // At degree 0 that window gives a "solvable" 1x1 system whose answer is
    // that single y, so the old wsum-only gate published a phantom smooth
    // value (a flat 12-mpg shelf across a 400-lb gap Stata leaves blank) and
    // counted it in r(ngrid) -- 49 vs Stata's 46 on sysuse auto. The rule is
    // MEASURED, not derived: on StataBE 19.5 every zero- or one-observation
    // window is missing and every two-observation window is fitted, even two
    // at the SAME x (so the test is a COUNT, not distinct x), at degree 0
    // and degree 1 alike. Beyond degree 1 the threshold is unmeasured --
    // Stata's lpoly internals are not documented -- and the
    // singular-system gate below already covers the under-determined cases.
    if (wsum <= 0 || nw < 2) return f;
    // r(ngrid) counts SUCCESSFUL local regressions, so a singular window is a
    // missing smooth value AND is not counted -- not an error.
    std::vector<double> Ac = A, bc = b;
    if (!spd_solve(Ac, bc, m)) return f;
    f.b0 = bc[0];
    f.ok = true;
    if (!want_var && !want_sigma2) return f;
    // Columns of A^-1, one solve each (m <= degree+3, tiny).
    std::vector<double> Ainv(mm);
    for (int c = 0; c < m; ++c) {
        std::vector<double> Aw = A, e(static_cast<std::size_t>(m), 0.0);
        e[static_cast<std::size_t>(c)] = 1.0;
        if (!spd_solve(Aw, e, m)) {
            f.ok = false;
            return f;
        }
        for (int r = 0; r < m; ++r) Ainv[static_cast<std::size_t>(r * m + c)] = e[static_cast<std::size_t>(r)];
    }
    if (want_var) {
        double v = 0;
        for (int r = 0; r < m; ++r)
            for (int c = 0; c < m; ++c)
                v += Ainv[static_cast<std::size_t>(r)] * B[static_cast<std::size_t>(r * m + c)] *
                     Ainv[static_cast<std::size_t>(c)];
        f.var_factor = v;
    }
    if (want_sigma2) {
        double num = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const double u = (x[i] - x0) / h;
            double k = kernel_value(kern, u) / h;
            if (w) k *= (*w)[i];
            if (k <= 0) continue;
            double fit = 0, up = 1.0;
            for (int j = 0; j < m; ++j) {
                fit += bc[static_cast<std::size_t>(j)] * up;
                up *= u;
            }
            const double e = y[i] - fit;
            num += k * e * e;
        }
        double trS = 0;
        for (int r = 0; r < m; ++r)
            for (int c = 0; c < m; ++c)
                trS += Ainv[static_cast<std::size_t>(r * m + c)] * B[static_cast<std::size_t>(c * m + r)];
        const double den = wsum - trS;
        if (!(den > 0)) {
            f.ok = false;
            return f;
        }
        f.sigma2 = num / den;
    }
    return f;
}

}  // namespace

bool lpoly_kernel_from_name(const std::string& name, LpolyKernel* out) {
    if (name == "epanechnikov" || name.empty()) { *out = LpolyKernel::Epanechnikov; return true; }
    if (name == "epan2") { *out = LpolyKernel::Epan2; return true; }
    if (name == "biweight") { *out = LpolyKernel::Biweight; return true; }
    if (name == "cosine") { *out = LpolyKernel::Cosine; return true; }
    if (name == "gaussian") { *out = LpolyKernel::Gaussian; return true; }
    if (name == "parzen") { *out = LpolyKernel::Parzen; return true; }
    if (name == "rectangle") { *out = LpolyKernel::Rectangle; return true; }
    if (name == "triangle") { *out = LpolyKernel::Triangle; return true; }
    return false;
}

const char* lpoly_kernel_name(LpolyKernel k) {
    switch (k) {
        case LpolyKernel::Epanechnikov: return "epanechnikov";
        case LpolyKernel::Epan2: return "epan2";
        case LpolyKernel::Biweight: return "biweight";
        case LpolyKernel::Cosine: return "cosine";
        case LpolyKernel::Gaussian: return "gaussian";
        case LpolyKernel::Parzen: return "parzen";
        case LpolyKernel::Rectangle: return "rectangle";
        case LpolyKernel::Triangle: return "triangle";
    }
    return "epanechnikov";
}

double lpoly_rot_constant(LpolyKernel k, int p) { return rot_constant(k, p); }

CECON_NO_FP_CONTRACT
double lpoly_rot_bandwidth(const std::vector<double>& x, const std::vector<double>& y,
                           LpolyKernel k, int degree, const std::vector<double>* w) {
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
    int p = degree;
    if (p % 2 == 0) ++p;  // formula (3) is stated for odd p
    const double C = rot_constant(k, p);
    if (C < 0) return -1;

    const std::size_t n = x.size();
    if (n < static_cast<std::size_t>(p + 4) + 1) return -1;
    const double xmin = *std::min_element(x.begin(), x.end());
    const double xmax = *std::max_element(x.begin(), x.end());
    const double range = xmax - xmin;
    if (range <= 0) return -1;

    // Global polynomial of degree p+3 in raw x (rot_poly_fit: Stata's own
    // conditioning), then its (p+1)th derivative -- a QUADRATIC, because the
    // surviving terms are j = p+1..p+3.
    // The cross products are summed in row order, so the rows go in x order
    // (stable, as lpoly sorts) whatever order the caller has.
    std::vector<std::size_t> ord(n);
    for (std::size_t i = 0; i < n; ++i) ord[i] = i;
    std::stable_sort(ord.begin(), ord.end(), [&](std::size_t a, std::size_t b) { return x[a] < x[b]; });
    std::vector<double> xs(n), ys(n), ws;
    for (std::size_t i = 0; i < n; ++i) {
        xs[i] = x[ord[i]];
        ys[i] = y[ord[i]];
    }
    if (w) {
        ws.resize(n);
        for (std::size_t i = 0; i < n; ++i) ws[i] = (*w)[ord[i]];
    }
    std::vector<double> beta;
    double rss = 0;
    if (!rot_poly_fit(xs, ys, w ? &ws : nullptr, p + 3, &beta, &rss)) return -1;
    const double sigma2 = rss / static_cast<double>(n);  // RSS/n, not RSS/(n-p-4)

    // d^(p+1)m/dx^(p+1) = d0 + d1 x + d2 x^2, d_e = b_(p+1+e) (p+1+e)!/e!
    double d[3] = {0, 0, 0};
    for (int e = 0; e < 3; ++e) {
        const int j = p + 1 + e;
        double fact = 1.0;
        for (int t = e + 1; t <= j; ++t) fact *= static_cast<double>(t);
        d[e] = beta[static_cast<std::size_t>(j)] * fact;
    }

    // n * integral over the w0 interval of the squared derivative -- exact,
    // because the integrand is a quartic. NOT a sum over observations, and no
    // design-density factor.
    const double lo = xmin + 0.05 * range, hi = xmax - 0.05 * range;
    const double q[5] = {d[0] * d[0], 2 * d[0] * d[1], d[1] * d[1] + 2 * d[0] * d[2],
                         2 * d[1] * d[2], d[2] * d[2]};
    auto anti = [&](double t) {
        double v = 0.0;
        for (int e = 0; e < 5; ++e) v += q[e] * std::pow(t, e + 1) / static_cast<double>(e + 1);
        return v;
    };
    const double integral = anti(hi) - anti(lo);
    const double denom = static_cast<double>(n) * integral;
    if (!(denom > 0)) return -1;

    const double num = sigma2 * 0.9 * range;
    return C * std::pow(num / denom, 1.0 / (2.0 * p + 3.0));
}

LpolyResult lpoly_fit(const std::vector<double>& x, const std::vector<double>& y,
                      const LpolyOpts& o) {
    LpolyResult r;
    const std::size_t n = x.size();
    if (n == 0) return r;
    const std::vector<double>* w = o.weights;

    double h = o.bwidth;
    const double rot = lpoly_rot_bandwidth(x, y, o.kernel, o.degree, w);
    if (h <= 0) h = rot;
    if (!(h > 0)) return r;  // caller turns this into a named refusal
    r.bwidth = h;
    // BUG-417: the PILOT bandwidth is 1.5 x the RULE-OF-THUMB bandwidth of the
    // same degree and kernel -- NOT 1.5 x the bandwidth actually used. The two
    // coincide only when the ROT is what was used, which is why an explicit
    // bwidth() was the arm that showed the difference (Stata 236.97023 vs the
    // 1.5 x 500 = 750 this used to report). Stata's lpoly computes the pilot from
    // the same rule-of-thumb call whether or not bwidth() was given.
    if (o.pwidth > 0) {
        r.pwidth = o.pwidth;
    } else {
        // A sample too small for the ROT chain has no pilot to report; falling
        // back to 1.5 x h keeps the field populated and is NAMED here rather
        // than silently passed off as Stata's number.
        r.pwidth = (rot > 0) ? 1.5 * rot : 1.5 * h;
    }

    if (o.at != nullptr) {
        // at(): the evaluation points are the at variable's values, row for row.
        r.grid = *o.at;
    } else {
        const double xmin = *std::min_element(x.begin(), x.end());
        const double xmax = *std::max_element(x.begin(), x.end());
        // Stata's lpoly: n() <= 1 means 50, and n() above _N is set to _N.
        int ng = (o.n > 1) ? o.n : 50;
        if (o.n_cap > 0 && ng > o.n_cap) {
            ng = o.n_cap;
            r.n_capped = true;
        }
        if (ng < 1) ng = 1;
        r.grid.resize(static_cast<std::size_t>(ng));
        const double delta = (ng == 1) ? 0.0 : (xmax - xmin) / static_cast<double>(ng - 1);
        for (int g = 0; g < ng; ++g)
            r.grid[static_cast<std::size_t>(g)] = xmin + static_cast<double>(g) * delta;
    }
    const std::size_t ng = r.grid.size();
    r.smooth.assign(ng, kNaN);
    if (o.want_se) r.se.assign(ng, kNaN);

    for (std::size_t g = 0; g < ng; ++g) {
        const double x0 = r.grid[g];
        if (std::isnan(x0)) continue;
        const LocalFit f = local_fit(x, y, w, o.kernel, x0, h, o.degree, o.want_se, false);
        if (!f.ok) continue;
        r.smooth[g] = f.b0;
        ++r.ngrid;
        if (o.want_se) {
            const LocalFit pf =
                local_fit(x, y, w, o.kernel, x0, r.pwidth, o.degree + 2, false, true);
            if (pf.ok && f.var_factor >= 0) r.se[g] = std::sqrt(f.var_factor * pf.sigma2);
        }
    }
    return r;
}

}  // namespace cecon
