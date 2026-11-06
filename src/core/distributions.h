// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Distribution tail probabilities and critical values — ONE implementation,
// shared by the engine's estimators and by PIET.
//
// These were file-local to src/estimators/posthest.cpp until the P6 wave, when
// PIET's resampling driver (bootstrap/jackknife/permute) needed the same t and
// normal quantiles to render its own tables. Re-implementing them on the PIET
// side would have put the same mathematics in two places, which is exactly how
// two layers start disagreeing about a number — so they moved here VERBATIM and
// both sides include this header. Nothing about the values changed.
//
// Header-only and dependency-free (<cmath>/<limits>, and core/stata_arith.h, which is
// header-only on the same two), so sharing them costs no new link edge on either side.
#pragma once
#include <cmath>
#include <limits>

#include "core/stata_arith.h"  // stata_normalden: Stata's exp() underflow edge

namespace cecon {

// Continued fraction for the incomplete beta (Numerical Recipes betacf).
// `maxit` defaults to the 300 every existing caller has always had; only the
// Student-t tail below raises it (its a = df/2 needs O(sqrt(a)) terms, see there).
inline double betacf(double a, double b, double x, int maxit = 300) {
    const int MAXIT = maxit;
    const double EPS = 1e-15, FPMIN = 1e-300;
    double qab = a + b, qap = a + 1.0, qam = a - 1.0;
    double c = 1.0, d = 1.0 - qab * x / qap;
    if (std::fabs(d) < FPMIN) d = FPMIN;
    d = 1.0 / d;
    double h = d;
    for (int m = 1; m <= MAXIT; ++m) {
        double mf = static_cast<double>(m);
        double aa = mf * (b - mf) * x / ((qam + 2 * mf) * (a + 2 * mf));
        d = 1.0 + aa * d; if (std::fabs(d) < FPMIN) d = FPMIN;
        c = 1.0 + aa / c; if (std::fabs(c) < FPMIN) c = FPMIN;
        d = 1.0 / d; h *= d * c;
        aa = -(a + mf) * (qab + mf) * x / ((a + 2 * mf) * (qap + 2 * mf));
        d = 1.0 + aa * d; if (std::fabs(d) < FPMIN) d = FPMIN;
        c = 1.0 + aa / c; if (std::fabs(c) < FPMIN) c = FPMIN;
        d = 1.0 / d;
        double del = d * c; h *= del;
        if (std::fabs(del - 1.0) < EPS) break;
    }
    return h;
}

// Regularized incomplete beta I_x(a,b).
inline double betai(double a, double b, double x) {
    if (x <= 0.0) return 0.0;
    if (x >= 1.0) return 1.0;
    double bt = std::exp(std::lgamma(a + b) - std::lgamma(a) - std::lgamma(b)
                         + a * std::log(x) + b * std::log(1.0 - x));
    if (x < (a + 1.0) / (a + b + 2.0)) return bt * betacf(a, b, x) / a;
    return 1.0 - bt * betacf(b, a, 1.0 - x) / b;
}

// Upper tail 1 - I_x(a,b), computed directly rather than as 1 - betai(a,b,x).
// Stata calls these two out as SEPARATE functions (ibeta / ibetatail) and the
// difference is not cosmetic: for a strongly positive Spearman rho the
// subtraction annihilates the answer. Measured at n=74 with tie-free ranks and
// rho=0.9999, `1 - betai(...)` returns exactly 0 where the upper tail is
// 6.8136e-32 -- both display as 0.0000, so a check value cannot see it, but a
// golden at rtol 1e-9 diverges once the tail drops below about 1e-7.
//
// Every one of the four returns is the mirror of betai's, GUARDS INCLUDED. The
// guards are not decoration: spearman's s = (T_obs - l)/(u - l) is not confined
// to [0,1] -- on tied data it leaves the interval in both directions (measured
// over n=5..7 ternary inputs: 5,303,556 valid fits, 4,872 with s <= 0 and 5,412
// with s >= 1, range -0.0447..+1.0447). Get the x>=1 guard backwards and p_u
// returns 1 instead of 0, which makes the reported p 1.0 where it should be 0 --
// the largest error a p-value can carry, on ordinary ordinal data.
inline double betaitail(double a, double b, double x) {
    if (x <= 0.0) return 1.0;
    if (x >= 1.0) return 0.0;
    double bt = std::exp(std::lgamma(a + b) - std::lgamma(a) - std::lgamma(b)
                         + a * std::log(x) + b * std::log(1.0 - x));
    if (x < (a + 1.0) / (a + b + 2.0)) return 1.0 - bt * betacf(a, b, x) / a;
    return bt * betacf(b, a, 1.0 - x) / b;
}

// P(|T_dof| > |t|) = I_{dof/(dof+t^2)}(dof/2, 1/2).
inline double t_twosided_pvalue(double t, double dof) {
    if (dof <= 0) return std::nan("");
    double x = dof / (dof + t * t);
    return betai(dof / 2.0, 0.5, x);
}

// Critical value t* with P(|T_dof| > t*) = alpha, via bisection on the
// monotone two-sided tail (alpha in (0,1); e.g. 0.05 → 95% CI).
inline double t_critical(double alpha, double dof) {
    if (dof <= 0 || alpha <= 0 || alpha >= 1) return std::nan("");
    double lo = 0.0, hi = 1e4;
    for (int it = 0; it < 200; ++it) {
        double mid = 0.5 * (lo + hi);
        double p = t_twosided_pvalue(mid, dof);  // decreasing in mid
        if (p > alpha) lo = mid; else hi = mid;
    }
    return 0.5 * (lo + hi);
}

// Standard-normal two-sided critical value z* with P(|Z| > z*) = alpha, i.e. the
// upper (1 − alpha/2) quantile. Acklam's rational inverse-normal + one Halley step
// (~1e-15). alpha == 0.05 short-circuits to Stata's exact invnormal(0.975) literal so
// a 95% asymptotic CI is bit-identical to add_inference's iv default (the 1.0−95/100
// vs 0.05 one-ulp gap the 0.25.0→0.25.1 freeze caught applies here too — callers pass
// a literal 0.05 for the 95% case, never 1.0−level/100).
inline double z_critical(double alpha) {
    if (alpha <= 0 || alpha >= 1) return std::nan("");
    if (alpha == 0.05) return 1.959963984540054;
    const double p = 1.0 - alpha / 2.0;  // upper quantile
    static const double a[] = {-3.969683028665376e+01, 2.209460984245205e+02, -2.759285104469687e+02,
                               1.383577518672690e+02, -3.066479806614716e+01, 2.506628277459239e+00};
    static const double b[] = {-5.447609879822406e+01, 1.615858368580409e+02, -1.556989798598866e+02,
                               6.680131188771972e+01, -1.328068155288572e+01};
    static const double c[] = {-7.784894002430293e-03, -3.223964580411365e-01, -2.400758277161838e+00,
                               -2.549732539343734e+00, 4.374664141464968e+00, 2.938163982698783e+00};
    static const double d[] = {7.784695709041462e-03, 3.224671290700398e-01, 2.445134137142996e+00,
                               3.754408661907416e+00};
    const double plow = 0.02425, phigh = 1 - plow;
    double q, r, x;
    if (p < plow) {
        q = std::sqrt(-2 * std::log(p));
        x = (((((c[0]*q+c[1])*q+c[2])*q+c[3])*q+c[4])*q+c[5]) / ((((d[0]*q+d[1])*q+d[2])*q+d[3])*q+1);
    } else if (p <= phigh) {
        q = p - 0.5; r = q * q;
        x = (((((a[0]*r+a[1])*r+a[2])*r+a[3])*r+a[4])*r+a[5])*q / (((((b[0]*r+b[1])*r+b[2])*r+b[3])*r+b[4])*r+1);
    } else {
        q = std::sqrt(-2 * std::log(1 - p));
        x = -(((((c[0]*q+c[1])*q+c[2])*q+c[3])*q+c[4])*q+c[5]) / ((((d[0]*q+d[1])*q+d[2])*q+d[3])*q+1);
    }
    double e = 0.5 * std::erfc(-x * 0.7071067811865476) - p;
    double u = e * 2.5066282746310002 * std::exp(0.5 * x * x);
    return x - u / (1 + 0.5 * x * u);
}


// ---------------------------------------------------------------------------
// Standard-normal quantile — Wichura's AS241 (PPND16), |rel. error| < 1e-15
// ---------------------------------------------------------------------------
//
// Three rational branches: the central body in q = p - 1/2, then two tail
// branches in r = sqrt(-log(min(p, 1-p))) split at r = 5 (|z| ~ 4.7 / p ~ 1.4e-6).
// core/distributions.h's z_critical uses the SHORTER Acklam form plus a Halley
// step; that is fine for a 95% critical value but this is the workhorse of an
// imputation stream, so it gets the full-precision polynomial with no refinement
// pass — a refinement pass is one more place a draw could stop being a pure
// function of its counter if the refinement ever changed.
inline double normal_icdf(double p) {
    if (!(p > 0.0) || !(p < 1.0)) {
        if (p == 0.0) return -std::numeric_limits<double>::infinity();
        if (p == 1.0) return std::numeric_limits<double>::infinity();
        return std::numeric_limits<double>::quiet_NaN();
    }
    const double q = p - 0.5;
    double r;
    if (std::fabs(q) <= 0.425) {
        r = 0.180625 - q * q;
        return q *
               (((((((2.5090809287301226727e+3 * r + 3.3430575583588128105e+4) * r +
                     6.7265770927008700853e+4) * r + 4.5921953931549871457e+4) * r +
                   1.3731693765509461125e+4) * r + 1.9715909503065514427e+3) * r +
                 1.3314166789178437745e+2) * r + 3.3871328727963666080e+0) /
               (((((((5.2264952788528545610e+3 * r + 2.8729085735721942674e+4) * r +
                     3.9307895800092710610e+4) * r + 2.1213794301586595867e+4) * r +
                   5.3941960214247511077e+3) * r + 6.8718700749205790830e+2) * r +
                 4.2313330701600911252e+1) * r + 1.0);
    }
    r = (q < 0.0) ? p : (1.0 - p);
    r = std::sqrt(-std::log(r));
    double z;
    if (r <= 5.0) {
        r -= 1.6;
        z = (((((((7.74545014278341407640e-4 * r + 2.27238449892691845833e-2) * r +
                  2.41780725177450611770e-1) * r + 1.27045825245236838258e+0) * r +
                3.64784832476320460504e+0) * r + 5.76949722146069140550e+0) * r +
              4.63033784615654529590e+0) * r + 1.42343711074968357734e+0) /
            (((((((1.05075007164441684324e-9 * r + 5.47593808499534494600e-4) * r +
                  1.51986665636164571966e-2) * r + 1.48103976427480074590e-1) * r +
                6.89767334985100004550e-1) * r + 1.67638483018380384940e+0) * r +
              2.05319162663775882187e+0) * r + 1.0);
    } else {
        r -= 5.0;
        z = (((((((2.01033439929228813265e-7 * r + 2.71155556874348757815e-5) * r +
                  1.24266094738807843860e-3) * r + 2.65321895265761230930e-2) * r +
                2.96560571828504891230e-1) * r + 1.78482653991729133580e+0) * r +
              5.46378491116411436990e+0) * r + 6.65790464350110377720e+0) /
            (((((((2.04426310338993978564e-15 * r + 1.42151175831644588870e-7) * r +
                  1.84631831751005468180e-5) * r + 7.86869131145613259100e-4) * r +
                1.48753612908506148525e-2) * r + 1.36929880922735805310e-1) * r +
              5.99832206555887937690e-1) * r + 1.0);
    }
    return (q < 0.0) ? -z : z;
}


// ---------------------------------------------------------------------------
// Stata's expression-language distribution functions
// ---------------------------------------------------------------------------
//
// normal(), invnormal(), ttail(), invttail() and invt() as `generate`/`replace`
// evaluate them. Every return here is a double or NaN; NaN means Stata's `.`, and
// the caller maps it. Domains, and the places where Stata answers `.` or a hard
// 0/1 instead of a number, were MEASURED on StataBE 19.5 (2026-09-29; the grid of
// 1,450 values is the scratch artefact of task engine-expression-functions), not
// read off the manual, which is looser than the program:
//   * df must lie in (0, 2e17]; df = 3e17 is `.` for all three t functions even
//     though [FN] prints the domain as "2e-10 to 2e+17".
//   * ttail(df,t) is 0 (t > 0) or 1 (t < 0) once t*t overflows a double
//     (|t| > 1.34e154): 1.3e154 still answers 2.4e-155, 1.4e154 answers 0.
//   * invttail answers `.` when the root is larger than about 9.35e153 in
//     magnitude: 9.30e153 is returned, 9.40e153 is not (bisected on df = 1; the
//     band was later narrowed to 9.34996e153 returned, 9.37e153 `.`). The edge
//     inside that band is not pinned; see the cut in stata_invttail.
//   * normal(z) flushes a subnormal result to 0 (normal(-37.5) = 4.6e-308 is
//     returned, normal(-38) = 2.9e-316 is 0).
//   * invnormal(p) is `.` outside the open interval (0,1), invttail/invt too.

// log Gamma(a + 1/2) - log Gamma(a). Taking the difference of two lgamma calls
// loses |lgamma(a)| * 2^-53 absolutely -- about 1e-9 at a = 5e7, which is a
// df = 1e8 t statistic -- so above a = 20 the difference is formed from the
// Stirling series directly: (a) ln(a+1/2) - (a-1/2) ln a - 1/2, written with
// log1p, plus the difference of the two Bernoulli tails. The first omitted
// Bernoulli term (B12) is below 1e-17 at z = 20.
inline double lgamma_half_ratio(double a) {
    if (a < 20.0) return std::lgamma(a + 0.5) - std::lgamma(a);
    auto bern = [](double z) {
        const double z2 = 1.0 / (z * z);
        return (1.0 / 12.0 -
                z2 * (1.0 / 360.0 - z2 * (1.0 / 1260.0 - z2 * (1.0 / 1680.0 - z2 / 1188.0)))) /
               z;
    };
    return a * std::log1p(0.5 / a) + 0.5 * std::log(a) - 0.5 + bern(a + 0.5) - bern(a);
}

// Upper normal tail P(Z > z) = 0.5 * erfc(z / sqrt 2), with the ARGUMENT carried
// to double-double. The plain form rounds z / sqrt 2 once (and 1/sqrt 2 once
// more), and erfc's relative sensitivity to its argument is about 2x^2, so that
// one rounding costs up to ~1,600 ulp in the deep tail: measured against a
// 400-digit reference over 1,221 z in [-38, 9], 0.5 * erfc(-z * 0.7071...) was
// off by up to 1,570 ulp below z = -30 and 1,032 ulp on [-30, -10], where
// StataBE 19.5 is within 4 ulp (grid: task engine-expression-functions scratch,
// stata/ngrid.*). With x = xh + xl exact to ~2^-106, erfc(xh + xl) =
// erfc(xh) - xl (2/sqrt pi) exp(-xh^2) to first order (xl^2 is below 1e-32 of the
// value), which brings the worst case to 4 ulp over the same grid.
inline double normal_upper_tail(double z) {
    if (!std::isfinite(z)) return std::isnan(z) ? z : (z > 0 ? 0.0 : 1.0);
    const double c_hi = 0.7071067811865476, c_lo = -4.833646656726457e-17;  // 1/sqrt 2
    const double xh = z * c_hi;
    const double xl = std::fma(z, c_hi, -xh) + z * c_lo;
    return 0.5 * (std::erfc(xh) - xl * 1.1283791670955126 * std::exp(-xh * xh));
}

// Portions derived from R-nmath 2024 (GPL-2.0-or-later).
// See THIRD_PARTY_NOTICES.
//
// Standard normal CDF, Stata's normal(z): W. J. Cody's rational Chebyshev
// approximation (ACM TOMS Algorithm 715, ANORM; the same code as R's
// nmath/pnorm.c pnorm_both), lower tail. Identified from StataBE 19.5 %21x
// bits (2026-10-27): 3,409 distinct z -- the 2,001-point grid z = -8(1/125)8,
// 1,599 values from codex-recovery/normal-values.txt and the 800 probit
// arguments of probit-spy.log -- match this form bit for bit, 3409/3409. The
// erfc forms it replaces missed 1,385 of them (up to 81 ulp in the lower tail).
// Every product and sum is rounded separately (no FMA), as in the reference.
// Values below the smallest normal double are 0, as measured on StataBE.
inline double stata_normal(double x) {
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
    if (std::isnan(x)) return std::nan("");
    if (!(std::fabs(x) < 40.0)) return x > 0 ? 1.0 : 0.0;  // incl. +-inf; |x| >= 40 is 0 or 1
    static const double a[5] = {2.2352520354606839287, 161.02823106855587881, 1067.6894854603709582,
                                18154.981253343561249, 0.065682337918207449113};
    static const double b[4] = {47.20258190468824187, 976.09855173777669322, 10260.932208618978205,
                                45507.789335026729956};
    static const double c[9] = {0.39894151208813466764, 8.8831497943883759412, 93.506656132177855979,
                                597.27027639480026226, 2494.5375852903726711, 6848.1904505362823326,
                                11602.651437647350124, 9842.7148383839780218, 1.0765576773720192317e-8};
    static const double d[8] = {22.266688044328115691, 235.38790178262499861, 1519.377599407554805,
                                6485.558298266760755, 18615.571640885098091, 34900.952721145977266,
                                38912.003286093271411, 19685.429676859990727};
    static const double p[6] = {0.21589853405795699, 0.1274011611602473639, 0.022235277870649807,
                                0.001421619193227893466, 2.9112874951168792e-5, 0.02307344176494017303};
    static const double q[5] = {1.28426009614491121, 0.468238212480865118, 0.0659881378689285515,
                                0.00378239633202758244, 7.29751555083966205e-5};
    const double y = std::fabs(x);
    double cum;
    if (y <= 0.67448975) {
        double xnum = 0, xden = 0;
        if (y > 2.220446049250313e-16 * 0.5) {
            const double xsq = x * x;
            xnum = a[4] * xsq;
            xden = xsq;
            for (int i = 0; i < 3; ++i) {
                xnum = (xnum + a[i]) * xsq;
                xden = (xden + b[i]) * xsq;
            }
        }
        const double temp = x * (xnum + a[3]) / (xden + b[3]);
        return 0.5 + temp;
    } else if (y <= 5.656854249492380195206754896838) {  // sqrt(32)
        double xnum = c[8] * y, xden = y;
        for (int i = 0; i < 7; ++i) {
            xnum = (xnum + c[i]) * y;
            xden = (xden + d[i]) * y;
        }
        const double temp = (xnum + c[7]) / (xden + d[7]);
        const double xsq = std::trunc(y * 16) / 16, del = (y - xsq) * (y + xsq);
        cum = std::exp(-xsq * xsq * 0.5) * std::exp(-del * 0.5) * temp;
    } else {
        const double xsq0 = 1.0 / (x * x);
        double xnum = p[5] * xsq0, xden = xsq0;
        for (int i = 0; i < 4; ++i) {
            xnum = (xnum + p[i]) * xsq0;
            xden = (xden + q[i]) * xsq0;
        }
        double temp = xsq0 * (xnum + p[4]) / (xden + q[4]);
        temp = (0.398942280401432677939946059934 - temp) / y;  // 1/sqrt(2 pi)
        const double xsq = std::trunc(x * 16) / 16, del = (x - xsq) * (x + xsq);
        cum = std::exp(-xsq * xsq * 0.5) * std::exp(-del * 0.5) * temp;
    }
    if (x > 0) return 1.0 - cum;
    return (cum < 2.2250738585072014e-308) ? 0.0 : cum;  // subnormal -> 0, as measured
}

// Stata's invnormal(p): Wichura's AS241 (normal_icdf above), which is already at
// |relative error| < 1e-15 over the whole open interval; no refinement pass.
inline double stata_invnormal(double p) {
    if (std::isnan(p) || !(p > 0.0) || !(p < 1.0)) return std::nan("");
    return normal_icdf(p);
}

// Stata's normalden(x[, m], s): the normal density ([FN] normalden(x,m,s) =
// normalden((x-m)/s)/s; normalden(x,s) is m = 0). Read off StataBE 19.5 %21x bits
// (BUG-1124, 2026-10-29): exp(-z*z/2) with Stata's exp() (an argument below -709
// is exactly 0), DIVIDED by the double 0x1.40d931ff62706p+1 -- one ulp above
// sqrt(2*pi) rounded to nearest -- and then by s. The 400-point grid
// z = (n-200)/37.3, n = 1..400, matches bit for bit in all three forms; the
// 0.3989422804014327 multiply misses 338 of them by an ulp. The density
// underflows to a subnormal (normalden(37.65)), overflows to missing
// (normalden(0, 1e-310)), and is missing for a missing argument and for s <= 0.
// NaN = missing; the caller maps it to its own `.`.
inline double stata_normalden(double x, double m = 0.0, double s = 1.0) {
    if (std::isnan(x) || std::isnan(m) || std::isnan(s) || !(s > 0.0)) return std::nan("");
    const double z = (x - m) / s;
    const double d = stata_exp_value(-0.5 * z * z) / 0x1.40d931ff62706p+1 / s;
    return std::isfinite(d) ? d : std::nan("");
}

// Student's t with df degrees of freedom: T > |t| has probability
// 0.5 * I_x(df/2, 1/2) with x = df/(df+t^2) -- the regularized incomplete beta.
// Returns ln P(T > |t|) in *lupper, P(T > |t|) itself in *upper (when asked: going
// through the log and back costs |ln P| ulp, 158 ulp at ttail(1e7,30), so a value
// the expansion forms directly is handed back as is), and P(T < |t|) - 1/2 in
// *center, each formed
// without subtracting a number near 1 from 1: x and 1-x = t^2/(df+t^2) are built
// separately from r = t^2/df, and the continued fraction (betacf, Lentz's form as
// in Numerical Recipes 6.4) is run on whichever of I_x(a,b) or I_{1-x}(b,a)
// converges, the other side taken as its complement.
//
// Large df. Near its switch point the continued fraction below is ill-conditioned
// once a = df/2 is large: it settles within ~100 terms, but on a value that is off
// by ~6e-12 at df = 1e6 and ~2e-11 at df = 1e7 (ttail(df,10), against a 60-digit
// incomplete-beta reference; more terms only add rounding). There the tail is
// taken instead from Fisher's expansion in 1/df,
//   P(T > t) = Q(t) + phi(t) [g1/df + g2/df^2 + ... + g8/df^8],
// whose g_k come from expanding the density (1 + t^2/df)^(-(df+1)/2) and the
// constant G((df+1)/2) / (sqrt(df pi) G(df/2)) in 1/df and integrating
// phi(x) x^n from t to infinity term by term. The first four are
//   g1 = (t^3 + t) / 4
//   g2 = (3t^7 - 7t^5 - 5t^3 - 3t) / 96
//   g3 = (t^11 - 11t^9 + 14t^7 + 6t^5 - 3t^3 - 15t) / 384
//   g4 = (15t^15 - 375t^13 + 2225t^11 - 2141t^9 - 939t^7 - 213t^5 + 915t^3 + 945t) / 92160
// and all eight (kT_TAIL_G below) were generated in exact rational arithmetic by a
// script that re-derives g1..g4 above independently, asserts at every order that
// the coefficient of Q(t) cancels (as it must for a density that integrates to 1),
// and prints the table (task engine-expression-functions scratch,
// harness/derive_g.py). The expansion is used only where its last term is below
// 1e-16 of the tail; everywhere else the continued fraction is used.
struct TTailG { double den; double c[16]; };  // c: coefficients of t^(4k-1), t^(4k-3), ..., t
inline const TTailG kT_TAIL_G[8] = {
            {4.0, {1.0, 1.0}},
            {96.0, {3.0, -7.0, -5.0, -3.0}},
            {384.0, {1.0, -11.0, 14.0, 6.0, -3.0, -15.0}},
            {92160.0, {15.0, -375.0, 2225.0, -2141.0, -939.0, -213.0, 915.0, 945.0}},
            {368640.0, {3.0, -133.0, 1764.0, -7516.0, 5994.0, 2490.0, 1140.0, 180.0, 5355.0, 17955.0}},
            {185794560.0, {63.0, -4347.0, 101115.0, -946043.0, 3237542.0, -2253390.0, -876282.0, -384150.0, 263115.0, 129465.0, -2329425.0, -2463615.0}},
            {743178240.0, {9.0, -891.0, 32010.0, -519710.0, 3845611.0, -11213649.0, 7034364.0, 2607948.0, 1208655.0, -783405.0, -2545830.0, -4881870.0, -35519715.0, -111486375.0}},
            {356725555200.0, {135.0, -18135.0, 929565.0, -23142945.0, 294349195.0, -1831354675.0, 4717343001.0, -2723449941.0, -971108667.0, -476844165.0, 175806855.0, 608864445.0, -621196695.0, -298409265.0, 13660121475.0, 14223634425.0}},
};
inline void t_tail_parts(double df, double t, double* lupper, double* center,
                         double* upper = nullptr) {
    const double at = std::fabs(t);
    if (df >= 1000.0) {
        const double t2 = at * at;
        const double phi = std::exp(-0.5 * t2) * 0.39894228040143267794;
        const double e = 1.0 / df;
        // sum_k g_k e^k by Horner in e, each g_k by Horner in t^2. env8 is g8's
        // envelope, sum |c_i| t^(power): unlike g8 it has no roots, so a small value
        // means the last term is small, not that t sits on a zero of g8.
        double sum = 0.0, env8 = 0.0;
        for (int k = 8; k >= 1; --k) {
            const TTailG& g = kT_TAIL_G[k - 1];
            double v = 0.0;
            for (int i = 0; i < 2 * k; ++i) {
                v = v * t2 + g.c[i];
                if (k == 8) env8 = env8 * t2 + std::fabs(g.c[i]);
            }
            v = v * at / g.den;
            sum = (sum + v) * e;
        }
        env8 = env8 * at / kT_TAIL_G[7].den;
        const double corr = phi * sum;
        const double q = normal_upper_tail(at) + corr;
        // Accept when the last term's envelope is below 1e-16 of the tail. That also
        // bounds every earlier term's size in the divergent regime (there the terms
        // grow, so term 1 would be smaller still, which needs t^4 < 4e-16 df), so a
        // diverging series cannot pass. The envelope is formed as (env8 phi) e^8 so an
        // underflowing e^8 cannot zero it early, and phi must be a normal number
        // (t < ~37.5): measured, a subnormal phi let invttail(1000,1e-300) answer 38.1
        // instead of 54.29.
        const double last = env8 * phi * std::pow(e, 8);
        if (phi >= 2.2250738585072014e-308 && q > 0.0 && last <= 1e-16 * q) {
            *lupper = std::log(q);
            *center = 0.5 * std::erf(at * 0.70710678118654752440) - corr;
            if (upper) *upper = q;
            return;
        }
    }
    const double a = 0.5 * df, b = 0.5;
    // r = t^2/df; lx = ln x, ly = ln(1-x); x = 1/(1+r), 1-x = r/(1+r).
    // Formed by division where that is finite; the log form only covers t*t
    // overflowing (invttail's search reaches t = e^360).
    double lx, ly, x, y;
    const double r = (at * at) / df;
    if (!std::isfinite(r) || r > 1.0) {
        double q = df / (at * at);  // 1/r, formed directly
        const double lq = (q > 0.0 && std::isfinite(q)) ? std::log(q)
                                                         : std::log(df) - 2.0 * std::log(at);
        q = std::exp(lq);
        lx = lq - std::log1p(q);
        ly = -std::log1p(q);
        x = q / (1.0 + q);
        y = 1.0 / (1.0 + q);
    } else {
        lx = -std::log1p(r);
        ly = std::log(r) - std::log1p(r);
        x = 1.0 / (1.0 + r);
        y = r / (1.0 + r);
    }
    // ln[x^a (1-x)^b / B(a,b)], with ln B(a, 1/2) = ln sqrt(pi) - ln[G(a+1/2)/G(a)].
    const double lbt = a * lx + b * ly - (0.57236494292470008707 - lgamma_half_ratio(a));
    // The fraction needs O(sqrt(a)) terms near its switch point; betacf's usual cap
    // of 300 stops it short from df ~ 1e4. Capped so a huge df cannot overflow int.
    const int maxit = 300 + static_cast<int>(20.0 * std::sqrt(std::fmin(a, 1e10)));
    if (x < (a + 1.0) / (a + b + 2.0)) {
        // I = I_x(a, 1/2) is the small side: P(T > |t|) = I/2.
        const double lI = lbt + std::log(betacf(a, b, x, maxit)) - std::log(a);
        *lupper = lI - 0.69314718055994530942;
        *center = 0.5 * (1.0 - std::exp(lI));
        if (upper) *upper = 0.5 * std::exp(lI);
    } else {
        // J = I_{1-x}(1/2, a) = 1 - I is the small side: P(0 < T < |t|) = J/2.
        const double J = std::exp(lbt) * betacf(b, a, y, maxit) / b;
        *lupper = std::log1p(-J) - 0.69314718055994530942;
        *center = 0.5 * J;
        if (upper) *upper = 0.5 * (1.0 - J);  // J is the small side here: no cancellation
    }
}

inline bool stata_t_df_ok(double df) { return df > 0.0 && df <= 2e17; }

// Stata's ttail(df,t) = P(T > t), the UPPER tail.
inline double stata_ttail(double df, double t) {
    if (std::isnan(df) || std::isnan(t) || !stata_t_df_ok(df)) return std::nan("");
    if (t == 0.0) return 0.5;
    if (!std::isfinite(t * t)) return t > 0.0 ? 0.0 : 1.0;  // measured, see header
    double lu, c, up;
    t_tail_parts(df, t, &lu, &c, &up);
    return t > 0.0 ? up : 0.5 + c;
}

// Stata's invttail(df,p): the t with ttail(df,t) = p.
//
// Newton's method on u = ln t against ln ttail(df, e^u), for p < 1/2 (t > 0);
// p > 1/2 is -invttail(df, 1-p), and 1-p is exact there. Working in logs makes
// the step well scaled from p = 0.49 down to p = 1e-300, where the root runs from
// 0.03 to 1e150 depending on df. d ln ttail / du = -t f(t) / ttail(t), with the
// density f = exp(lc - (df+1)/2 ln(1 + t^2/df)). A [lo, hi] bracket is kept on u
// and any step that leaves it bisects instead, so the loop always terminates.
// Start: the larger of the Cornish-Fisher normal start z + g1(z)/df and the tail
// power law t ~ (c df^((df-1)/2) / p)^(1/df), the second being the right shape
// for small df where the normal start is off by orders of magnitude.
inline double stata_invttail(double df, double p) {
    if (std::isnan(df) || std::isnan(p) || !stata_t_df_ok(df)) return std::nan("");
    if (!(p > 0.0) || !(p < 1.0)) return std::nan("");
    if (p == 0.5) return 0.0;
    if (p > 0.5) {
        const double t = stata_invttail(df, 1.0 - p);
        return std::isnan(t) ? t : -t;
    }
    const double lp = std::log(p);
    const double lc = lgamma_half_ratio(0.5 * df) - 0.5 * std::log(df * 3.14159265358979323846);
    const double z = -normal_icdf(p);
    double u = std::log(std::fmax(z + (z * z * z + z) / (4.0 * df), 1e-300));
    const double ut = (lc + 0.5 * (df - 1.0) * std::log(df) - lp) / df;
    if (ut > u) u = ut;
    // Near the median the upper tail is 1/2 - eps, and one ulp of 1/2 is a relative
    // 5.5e-13 of eps = 1e-4: iterating on ln ttail lost that much at p = .4999
    // (df = 1e9: 4.7e-13 against a 50-digit Cornish-Fisher reference, where
    // StataBE is at 6e-16). For p > 1/4 the target is therefore the centre mass
    // P(0 < T < t) = 1/2 - p instead, which is exact (Sterbenz) and which
    // t_tail_parts forms without cancellation near t = 0.
    const bool central = p > 0.25;
    const double lcT = central ? std::log(0.5 - p) : 0.0;
    // g(u) is decreasing in u either way: ln ttail - ln p, or ln(1/2 - p) - ln centre.
    auto resid = [&](double lu, double cen) { return central ? lcT - std::log(cen) : lu - lp; };
    double lo = -700.0, hi = 360.0;  // u = ln t; e^360 is past the 9.35e153 cut
    u = std::fmin(std::fmax(u, lo + 1.0), hi - 1.0);
    for (int it = 0; it < 200; ++it) {
        const double t = std::exp(u);
        double lu, c;
        t_tail_parts(df, t, &lu, &c);
        const double g = resid(lu, c);
        if (g > 0.0) lo = u; else hi = u;
        const double lf = lc - 0.5 * (df + 1.0) * std::log1p(t * t / df);
        // d g / d u = -t f(t) / (the tail or centre mass the residual is taken on)
        const double dg = -std::exp(u + lf - (central ? std::log(c) : lu));
        double un = u - g / dg;
        if (!(un > lo) || !(un < hi) || !std::isfinite(un)) un = 0.5 * (lo + hi);
        const bool done = std::fabs(un - u) <= 1e-15 * std::fmax(1.0, std::fabs(u));
        u = un;
        if (done) break;
    }
    // Polish in t itself: u = ln t carries only |u| * 2^-53 absolute, which is a
    // relative 4e-14 on t = 1e150. Newton on ttail(t) - p = 0 (centre(t) - (1/2 - p)
    // near the median) with the residual formed as m expm1(ln target - ln m), so two
    // nearly equal masses are never subtracted.
    double t = std::exp(u);
    for (int it = 0; it < 2 && std::isfinite(t); ++it) {
        double lu, c;
        t_tail_parts(df, t, &lu, &c);
        const double lf = lc - 0.5 * (df + 1.0) * std::log1p(t * t / df);
        const double step = central
            ? std::expm1(lcT - std::log(c)) * std::exp(std::log(c) - lf - std::log(t))
            : -std::expm1(lp - lu) * std::exp(lu - lf - std::log(t));
        if (!(std::fabs(step) < 1e-6)) break;
        t *= 1.0 + step;
    }
    // Stata answers `.` once the root is too large. Measured on StataBE 19.5
    // (2026-09-29, df = 1): a root of 9.30e153 is returned as a value, 9.40e153 is
    // `.`; narrowed, invttail(1,3.4044e-155) = 9.3499555335386770e+153 is returned
    // and invttail(1,3.3971e-155) (root ~9.37e153) is `.`. The exact edge inside
    // that bracket is NOT pinned: a listed unpinned edge (only p below ~3.4e-155 at
    // small df reaches it).
    if (!(t < 9.35e153)) return std::nan("");
    return t;
}

// Stata's invt(df,p): the LOWER-tail inverse, t with P(T <= t) = p. By symmetry
// that is -invttail(df,p) with no subtraction: invt(10,.975) = 2.228138851986273.
inline double stata_invt(double df, double p) {
    const double t = stata_invttail(df, p);
    return std::isnan(t) ? t : -t;
}

}  // namespace cecon
