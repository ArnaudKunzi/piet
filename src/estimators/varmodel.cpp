// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "varmodel.h"

#include <algorithm>
#include <cmath>
#include <map>

#include "core/distributions.h"
#include "core/linalg.h"
#include "estimators/posthest.h"  // chi2_pvalue

namespace cecon {

namespace {

// ---- small dense helpers ---------------------------------------------------
// Everything in the IRF block is a handful of K^2-by-K^2*lags matrices (9 x 18
// doubles at K = 3, lags = 2), so plain ROW-MAJOR loops are used here rather than
// the column-major BLAS wrappers: the Kronecker index arithmetic is the part that
// has to stay readable and auditable, and at these sizes the wrappers buy
// nothing. `la::` is still used where numerics matter -- X'X and its SPD inverse.

// C(r x c) := A(r x k) * B(k x c), all row-major.
void mm(const double* A, const double* B, double* C, int r, int k, int c) {
    for (int i = 0; i < r; ++i)
        for (int j = 0; j < c; ++j) {
            double s = 0;
            for (int t = 0; t < k; ++t)
                s += A[static_cast<size_t>(i) * k + t] * B[static_cast<size_t>(t) * c + j];
            C[static_cast<size_t>(i) * c + j] = s;
        }
}

// out[i] := (M S M')[i][i], with M `rows` x `cols` and S `cols` x `cols`, row-major.
void quad_diag(const std::vector<double>& M, const std::vector<double>& S, int rows, int cols,
               std::vector<double>& out) {
    std::vector<double> MS(static_cast<size_t>(rows) * cols, 0.0);
    mm(M.data(), S.data(), MS.data(), rows, cols, cols);
    out.assign(static_cast<size_t>(rows), 0.0);
    for (int i = 0; i < rows; ++i) {
        double s = 0;
        for (int j = 0; j < cols; ++j)
            s += MS[static_cast<size_t>(i) * cols + j] * M[static_cast<size_t>(i) * cols + j];
        out[static_cast<size_t>(i)] = s;
    }
}

// v' S v for a vector v of length k (S is k x k, row-major).
double quad_form(const std::vector<double>& v, const std::vector<double>& S, int k) {
    double s = 0;
    for (int i = 0; i < k; ++i) {
        double row = 0;
        for (int j = 0; j < k; ++j)
            row += S[static_cast<size_t>(i) * k + j] * v[static_cast<size_t>(j)];
        s += v[static_cast<size_t>(i)] * row;
    }
    return s;
}

// Lower Cholesky of a K x K symmetric matrix (row-major). Returns false on a
// non-positive pivot -- the honest answer for a numerically singular Sigma. The
// pivot is never patched to keep going, because every orthogonalized quantity
// downstream would then be a manufactured number.
bool chol_lower(const std::vector<double>& S, int K, std::vector<double>& P) {
    P.assign(static_cast<size_t>(K) * K, 0.0);
    for (int i = 0; i < K; ++i)
        for (int j = 0; j <= i; ++j) {
            double s = S[static_cast<size_t>(i) * K + j];
            for (int t = 0; t < j; ++t)
                s -= P[static_cast<size_t>(i) * K + t] * P[static_cast<size_t>(j) * K + t];
            if (i == j) {
                if (!(s > 0)) return false;
                P[static_cast<size_t>(i) * K + j] = std::sqrt(s);
            } else {
                P[static_cast<size_t>(i) * K + j] = s / P[static_cast<size_t>(j) * K + j];
            }
        }
    return true;
}

// Inverse of a lower-triangular K x K matrix (row-major), by forward substitution.
void tril_inverse(const std::vector<double>& L, int K, std::vector<double>& Inv) {
    Inv.assign(static_cast<size_t>(K) * K, 0.0);
    for (int j = 0; j < K; ++j) {
        Inv[static_cast<size_t>(j) * K + j] = 1.0 / L[static_cast<size_t>(j) * K + j];
        for (int i = j + 1; i < K; ++i) {
            double s = 0;
            for (int t = j; t < i; ++t)
                s += L[static_cast<size_t>(i) * K + t] * Inv[static_cast<size_t>(t) * K + j];
            Inv[static_cast<size_t>(i) * K + j] = -s / L[static_cast<size_t>(i) * K + i];
        }
    }
}

// Wald statistic b_sub' (Sigma_aa * (X'X)^-1_sub)^-1 b_sub over the coefficient
// positions `idx` of one equation. Returns false when the block will not invert.
bool wald_block(const std::vector<double>& XtXi, int m, double sig_aa, const std::vector<double>& b,
                const std::vector<int>& idx, double* stat) {
    const int q = static_cast<int>(idx.size());
    if (q == 0 || !(sig_aa > 0)) return false;
    std::vector<double> V(static_cast<size_t>(q) * q);
    for (int i = 0; i < q; ++i)
        for (int j = 0; j < q; ++j)
            V[static_cast<size_t>(i) * q + j] =
                sig_aa * XtXi[static_cast<size_t>(idx[static_cast<size_t>(i)]) * m +
                              static_cast<size_t>(idx[static_cast<size_t>(j)])];
    if (la::spd_inverse(V.data(), q) != 0) return false;
    std::vector<double> bs(static_cast<size_t>(q));
    for (int i = 0; i < q; ++i)
        bs[static_cast<size_t>(i)] = b[static_cast<size_t>(idx[static_cast<size_t>(i)])];
    const double w = quad_form(bs, V, q);
    if (!std::isfinite(w)) return false;
    *stat = w;
    return true;
}

// An equation whose residual variance has collapsed to numerical zero: the fit is
// exact to double precision, so every Wald statistic in that equation is 0/0.
//
// The threshold is RELATIVE -- residual variance against the dependent variable's
// own variance, i.e. "R^2 equals 1 to within 1e-16". Measured on the P7 oracle
// fixture the three equations sit at 5.6e-02 (y), 3.5e-10 (x2) and 3.7e-20 (x1):
// a live equation and a collapsed one are ten orders of magnitude apart, and
// 1e-16 leaves ~6 orders of headroom on the live side and ~3 on the collapsed
// side. Stata prints `.` with df 0 for exactly the x1 equation.
constexpr double kDegenerateRelVar = 1e-16;

// Spelled out rather than <cmath>'s M_PI, which is a POSIX extension MSVC only
// defines under _USE_MATH_DEFINES — and this file has to build on the Windows
// cross-compile too.
constexpr double kPi = 3.14159265358979323846;

}  // namespace

VarResult var_fit(const double* Y, const double* time, int64_t n,
                  const std::vector<std::string>& names, int lags, int irf_steps, double delta) {
    VarResult r;
    const int K = static_cast<int>(names.size());
    r.K = K;
    r.lags = lags;
    if (K < 1 || lags < 1 || !(delta > 0) || n <= 0) {
        r.fail = VarResult::BAD_SPEC;
        return r;
    }
    const int p = lags, Kp = K * p, m = Kp + 1;
    r.m = m;

    // ---- estimation sample -------------------------------------------------
    // tsset semantics: a row enters when its own values AND the values at
    // time - 1*delta ... time - p*delta are all present and non-missing. Gaps are
    // respected (the rows around a gap simply fail the lookup), so the sample is
    // not merely "drop the first p rows".
    std::map<long long, int64_t> at;
    for (int64_t i = 0; i < n; ++i) {
        const double t = time[i];
        if (std::isnan(t)) continue;
        const long long key = std::llround(t / delta);
        if (!at.emplace(key, i).second) {  // repeated time value -- not a time series
            r.fail = VarResult::BAD_SPEC;
            return r;
        }
    }
    auto complete = [&](int64_t row) {
        for (int c = 0; c < K; ++c)
            if (std::isnan(Y[static_cast<int64_t>(c) * n + row])) return false;
        return true;
    };
    std::vector<int64_t> rows;    // sample rows, in time order
    std::vector<int64_t> lagrow;  // p lag rows per sample row, flattened
    std::vector<int64_t> lr(static_cast<size_t>(p));
    rows.reserve(at.size());
    for (const auto& kv : at) {
        if (!complete(kv.second)) continue;
        bool ok = true;
        for (int j = 1; j <= p; ++j) {
            auto it = at.find(kv.first - j);
            if (it == at.end() || !complete(it->second)) { ok = false; break; }
            lr[static_cast<size_t>(j - 1)] = it->second;
        }
        if (!ok) continue;
        if (rows.empty()) r.t_first = time[kv.second];
        r.t_last = time[kv.second];
        rows.push_back(kv.second);
        for (int j = 0; j < p; ++j) lagrow.push_back(lr[static_cast<size_t>(j)]);
    }
    const int64_t T = static_cast<int64_t>(rows.size());
    r.n_obs = T;
    if (T <= m) {
        r.fail = VarResult::TOO_FEW;
        return r;
    }

    // ---- design ------------------------------------------------------------
    // Regressor order: L1.v0 ... L1.v(K-1), L2.v0 ... Lp.v(K-1), then _cons.
    r.names = names;
    r.rnames.reserve(static_cast<size_t>(m));
    for (int j = 1; j <= p; ++j)
        for (int c = 0; c < K; ++c)
            r.rnames.push_back("L" + std::to_string(j) + "." + names[static_cast<size_t>(c)]);
    r.rnames.push_back("_cons");

    std::vector<double> X(static_cast<size_t>(T) * m);  // column-major for la::
    for (int j = 1; j <= p; ++j)
        for (int c = 0; c < K; ++c) {
            const int rr = (j - 1) * K + c;
            double* col = X.data() + static_cast<int64_t>(rr) * T;
            for (int64_t q = 0; q < T; ++q)
                col[q] = Y[static_cast<int64_t>(c) * n +
                           lagrow[static_cast<size_t>(q) * static_cast<size_t>(p) + static_cast<size_t>(j - 1)]];
        }
    for (int64_t q = 0; q < T; ++q) X[static_cast<int64_t>(Kp) * T + q] = 1.0;

    std::vector<std::vector<double>> yd(static_cast<size_t>(K));
    for (int c = 0; c < K; ++c) {
        yd[static_cast<size_t>(c)].resize(static_cast<size_t>(T));
        for (int64_t q = 0; q < T; ++q)
            yd[static_cast<size_t>(c)][static_cast<size_t>(q)] =
                Y[static_cast<int64_t>(c) * n + rows[static_cast<size_t>(q)]];
    }

    std::vector<double> XtXi(static_cast<size_t>(m) * m);
    la::xtx(X.data(), T, m, XtXi.data());
    if (la::spd_inverse(XtXi.data(), m) != 0) {
        r.fail = VarResult::SINGULAR;
        return r;
    }

    // ---- equation-by-equation OLS -----------------------------------------
    // Every equation has the identical regressor list, so the system GLS/SUR
    // refinement collapses algebraically onto these OLS coefficients.
    std::vector<std::vector<double>> resid(static_cast<size_t>(K));
    r.eqs.assign(static_cast<size_t>(K), VarEquation{});
    for (int a = 0; a < K; ++a) {
        VarEquation& eq = r.eqs[static_cast<size_t>(a)];
        eq.name = r.names[static_cast<size_t>(a)];
        std::vector<double> Xty(static_cast<size_t>(m));
        la::xtv(X.data(), T, m, yd[static_cast<size_t>(a)].data(), Xty.data());
        eq.b.assign(static_cast<size_t>(m), 0.0);
        la::matvec(XtXi.data(), m, Xty.data(), eq.b.data());
        resid[static_cast<size_t>(a)].resize(static_cast<size_t>(T));
        la::residual(X.data(), T, m, eq.b.data(), yd[static_cast<size_t>(a)].data(),
                     resid[static_cast<size_t>(a)].data());
    }

    // Sigma_ml = E'E / T. Stata's `var` uses THIS matrix (divisor T, not T - m)
    // for the coefficient covariance -- measured against the P7 oracle, where it
    // reproduces every printed Std. err. to 7 significant digits while the T - m
    // divisor is off by the factor sqrt(22/15) = 1.21.
    r.sigma.assign(static_cast<size_t>(K) * K, 0.0);
    for (int a = 0; a < K; ++a)
        for (int b = 0; b < K; ++b) {
            double s = 0;
            for (int64_t q = 0; q < T; ++q)
                s += resid[static_cast<size_t>(a)][static_cast<size_t>(q)] *
                     resid[static_cast<size_t>(b)][static_cast<size_t>(q)];
            r.sigma[static_cast<size_t>(a) * K + b] = s / static_cast<double>(T);
        }

    const int KM = K * m;
    r.vcov.assign(static_cast<size_t>(KM) * KM, 0.0);
    for (int a = 0; a < K; ++a)
        for (int b = 0; b < K; ++b) {
            const double sab = r.sigma[static_cast<size_t>(a) * K + b];
            for (int ra = 0; ra < m; ++ra)
                for (int rb = 0; rb < m; ++rb)
                    r.vcov[static_cast<size_t>(a * m + ra) * KM + (b * m + rb)] =
                        sab * XtXi[static_cast<size_t>(ra) * m + rb];
        }

    std::vector<int> lagidx(static_cast<size_t>(Kp));
    for (int i = 0; i < Kp; ++i) lagidx[static_cast<size_t>(i)] = i;

    for (int a = 0; a < K; ++a) {
        VarEquation& eq = r.eqs[static_cast<size_t>(a)];
        const double sig_aa = r.sigma[static_cast<size_t>(a) * K + a];
        double mean = 0;
        for (int64_t q = 0; q < T; ++q) mean += yd[static_cast<size_t>(a)][static_cast<size_t>(q)];
        mean /= static_cast<double>(T);
        double tss = 0, rss = 0;
        for (int64_t q = 0; q < T; ++q) {
            const double dv = yd[static_cast<size_t>(a)][static_cast<size_t>(q)] - mean;
            const double e = resid[static_cast<size_t>(a)][static_cast<size_t>(q)];
            tss += dv * dv;
            rss += e * e;
        }
        eq.rmse = std::sqrt(rss / static_cast<double>(T - m));
        eq.r2 = (tss > 0) ? 1.0 - rss / tss : 1.0;
        const double scale = tss / static_cast<double>(T);
        eq.degenerate = (scale > 0) ? (sig_aa <= kDegenerateRelVar * scale) : !(sig_aa > 0);
        eq.chi2_df = Kp;
        eq.chi2_ok = !eq.degenerate && wald_block(XtXi, m, sig_aa, eq.b, lagidx, &eq.chi2);
        if (eq.chi2_ok) {
            eq.p = chi2_pvalue(eq.chi2, static_cast<double>(Kp));
        } else {
            eq.chi2 = std::nan("");
            eq.p = std::nan("");
            eq.chi2_df = 0;  // Stata prints `.` with df 0
        }
        eq.se.reserve(static_cast<size_t>(m));
        for (int j = 0; j < m; ++j)
            eq.se.push_back(std::sqrt(std::max(0.0, sig_aa * XtXi[static_cast<size_t>(j) * m + j])));
    }

    // ---- header scalars ----------------------------------------------------
    // WARNING (spelled out in tests/unit/test_varmodel.cpp): every scalar below is
    // a function of det(Sigma_ml). When any equation is degenerate that
    // determinant is numerically zero, so LL / FPE / AIC / HQIC / SBIC are all
    // dominated by the floating-point noise in the collapsed residual. They are
    // structurally right but NOT reproducible across implementations on such a
    // fixture; the formulas are pinned on a synthetic non-degenerate VAR instead.
    std::vector<double> P;
    r.sigma_pd = chol_lower(r.sigma, K, P);
    if (r.sigma_pd) {
        double lndet = 0;
        for (int i = 0; i < K; ++i) lndet += 2.0 * std::log(P[static_cast<size_t>(i) * K + i]);
        const double Td = static_cast<double>(T);
        const double tp = static_cast<double>(K) * static_cast<double>(m);  // estimated coefficients
        r.det_sigma = std::exp(lndet);
        r.ll = -(Td / 2.0) *
               (static_cast<double>(K) * std::log(2.0 * kPi) + lndet + static_cast<double>(K));
        r.fpe = std::pow((Td + m) / (Td - m), K) * r.det_sigma;
        r.aic = -2.0 * r.ll / Td + 2.0 * tp / Td;
        r.hqic = -2.0 * r.ll / Td + 2.0 * std::log(std::log(Td)) * tp / Td;
        r.sbic = -2.0 * r.ll / Td + std::log(Td) * tp / Td;
    } else {
        r.det_sigma = r.ll = r.fpe = r.aic = r.hqic = r.sbic = std::nan("");
    }

    // ---- vargranger --------------------------------------------------------
    // Within each equation, are variable v's lags jointly zero? df = lags; the
    // ALL row excludes every OTHER variable's lags jointly, df = lags*(K-1). The
    // test uses the VAR's own covariance Sigma (x) (X'X)^-1, which is exact here
    // because every equation shares the same regressors. A degenerate equation
    // contributes rows with `.` and df 0, exactly as Stata prints them -- never a
    // number squeezed out of a numerically singular block.
    for (int a = 0; a < K; ++a) {
        const VarEquation& eq = r.eqs[static_cast<size_t>(a)];
        const double sig_aa = r.sigma[static_cast<size_t>(a) * K + a];
        std::vector<int> all;
        for (int b = 0; b < K; ++b) {
            if (b == a) continue;
            std::vector<int> idx;
            for (int j = 0; j < p; ++j) idx.push_back(j * K + b);
            all.insert(all.end(), idx.begin(), idx.end());
            VarGrangerRow row;
            row.equation = r.names[static_cast<size_t>(a)];
            row.excluded = r.names[static_cast<size_t>(b)];
            row.ok = !eq.degenerate && wald_block(XtXi, m, sig_aa, eq.b, idx, &row.chi2);
            if (row.ok) {
                row.df = p;
                row.p = chi2_pvalue(row.chi2, static_cast<double>(p));
            } else {
                row.chi2 = std::nan("");
                row.p = std::nan("");
                row.df = 0;
            }
            r.granger.push_back(row);
        }
        if (K > 1) {
            VarGrangerRow row;
            row.equation = r.names[static_cast<size_t>(a)];
            row.excluded = "ALL";
            row.ok = !eq.degenerate && wald_block(XtXi, m, sig_aa, eq.b, all, &row.chi2);
            if (row.ok) {
                row.df = static_cast<int>(all.size());
                row.p = chi2_pvalue(row.chi2, static_cast<double>(row.df));
            } else {
                row.chi2 = std::nan("");
                row.p = std::nan("");
                row.df = 0;
            }
            r.granger.push_back(row);
        }
    }

    r.ok = true;
    if (irf_steps < 0) return r;
    const int S = irf_steps;
    r.irf_steps = S;

    // ---- IRF ---------------------------------------------------------------
    // Companion F = [[A1 ... Ap],[I 0 ... 0],[0 I ... 0], ...]; W_s = F^s J' is
    // built by repeated multiplication (no eigendecomposition anywhere -- and
    // none is needed), and Phi_s = J W_s is its top K x K block.
    std::vector<double> F(static_cast<size_t>(Kp) * Kp, 0.0);
    for (int a = 0; a < K; ++a)
        for (int rr = 0; rr < Kp; ++rr)
            F[static_cast<size_t>(a) * Kp + rr] = r.eqs[static_cast<size_t>(a)].b[static_cast<size_t>(rr)];
    for (int i = 0; i < Kp - K; ++i) F[static_cast<size_t>(K + i) * Kp + i] = 1.0;

    std::vector<std::vector<double>> W(static_cast<size_t>(S) + 1), Phi(static_cast<size_t>(S) + 1);
    W[0].assign(static_cast<size_t>(Kp) * K, 0.0);
    for (int i = 0; i < K; ++i) W[0][static_cast<size_t>(i) * K + i] = 1.0;
    for (int s = 1; s <= S; ++s) {
        W[static_cast<size_t>(s)].assign(static_cast<size_t>(Kp) * K, 0.0);
        mm(F.data(), W[static_cast<size_t>(s) - 1].data(), W[static_cast<size_t>(s)].data(), Kp, Kp, K);
    }
    for (int s = 0; s <= S; ++s) {
        Phi[static_cast<size_t>(s)].assign(static_cast<size_t>(K) * K, 0.0);
        for (int a = 0; a < K; ++a)
            for (int b = 0; b < K; ++b)
                Phi[static_cast<size_t>(s)][static_cast<size_t>(a) * K + b] =
                    W[static_cast<size_t>(s)][static_cast<size_t>(a) * K + b];
    }

    // Theta_s = Phi_s * P, with P the lower Cholesky of Sigma_ml.
    std::vector<std::vector<double>> Theta(static_cast<size_t>(S) + 1);
    for (int s = 0; s <= S; ++s) {
        Theta[static_cast<size_t>(s)].assign(static_cast<size_t>(K) * K, 0.0);
        if (r.sigma_pd)
            mm(Phi[static_cast<size_t>(s)].data(), P.data(), Theta[static_cast<size_t>(s)].data(), K, K, K);
    }

    // Sigma_alpha = (X'X)^-1_lag (x) Sigma, in vec(A) order: index rr*K + c for
    // regressor rr, equation c. Same covariance as r.vcov, permuted to the
    // ordering the Kronecker Jacobians below need.
    const int K2 = K * K, Ka = Kp * K;
    std::vector<double> Salpha(static_cast<size_t>(Ka) * Ka, 0.0);
    for (int rr = 0; rr < Kp; ++rr)
        for (int c = 0; c < K; ++c)
            for (int rr2 = 0; rr2 < Kp; ++rr2)
                for (int c2 = 0; c2 < K; ++c2)
                    Salpha[static_cast<size_t>(rr * K + c) * Ka + (rr2 * K + c2)] =
                        XtXi[static_cast<size_t>(rr) * m + rr2] * r.sigma[static_cast<size_t>(c) * K + c2];

    // Cov(vec(P_hat)) = Jp Cov(vec(Sigma_hat)) Jp', with
    //   Cov(vec(Sigma_hat))[(b,a),(b',a')] = (S[b][b']S[a][a'] + S[a][b']S[b][a']) / T
    // (the Gaussian ML result (I + K_KK)(Sigma (x) Sigma)/T, i.e. the vec form of
    // 2 D+ (Sigma (x) Sigma) D+' / T), and Jp the Cholesky Jacobian obtained from
    // dSigma = dP P' + P dP'  =>  dP = P * tril_half(P^-1 dSigma P^-T).
    //
    // THE DIVISOR IS T, NOT T - m. Calibrated against the oracle's
    // stdoirf(0)_y->y = .0268042, which depends on this term alone: at step 0 the
    // coefficient term vanishes (Phi_0 = I is not estimated), leaving
    // SE(P_11) = P_11 / sqrt(2 * divisor). The oracle gives
    // .1777987 / .0268042 = 6.63325 = sqrt(44) = sqrt(2 * 22), and T = 22.
    std::vector<double> CovP(static_cast<size_t>(K2) * K2, 0.0);
    if (r.sigma_pd) {
        std::vector<double> CovSig(static_cast<size_t>(K2) * K2, 0.0);
        const double Td = static_cast<double>(T);
        for (int b = 0; b < K; ++b)
            for (int a = 0; a < K; ++a)
                for (int b2 = 0; b2 < K; ++b2)
                    for (int a2 = 0; a2 < K; ++a2)
                        CovSig[static_cast<size_t>(b * K + a) * K2 + (b2 * K + a2)] =
                            (r.sigma[static_cast<size_t>(b) * K + b2] * r.sigma[static_cast<size_t>(a) * K + a2] +
                             r.sigma[static_cast<size_t>(a) * K + b2] * r.sigma[static_cast<size_t>(b) * K + a2]) /
                            Td;
        std::vector<double> Pinv;
        tril_inverse(P, K, Pinv);
        std::vector<double> Jp(static_cast<size_t>(K2) * K2, 0.0), L(static_cast<size_t>(K) * K),
            dP(static_cast<size_t>(K) * K);
        for (int aa = 0; aa < K; ++aa)
            for (int bb = 0; bb < K; ++bb) {
                // dSigma = E_{aa,bb}  =>  (P^-1 dSigma P^-T)[i][j] = Pinv[i][aa] * Pinv[j][bb]
                for (int i = 0; i < K; ++i)
                    for (int j = 0; j < K; ++j) {
                        const double v =
                            Pinv[static_cast<size_t>(i) * K + aa] * Pinv[static_cast<size_t>(j) * K + bb];
                        L[static_cast<size_t>(i) * K + j] = (i > j) ? v : (i == j ? 0.5 * v : 0.0);
                    }
                mm(P.data(), L.data(), dP.data(), K, K, K);
                const int col = bb * K + aa;
                for (int i = 0; i < K; ++i)
                    for (int j = 0; j < K; ++j)
                        Jp[static_cast<size_t>(j * K + i) * K2 + col] = dP[static_cast<size_t>(i) * K + j];
            }
        std::vector<double> tmp(static_cast<size_t>(K2) * K2, 0.0);
        mm(Jp.data(), CovSig.data(), tmp.data(), K2, K2, K2);
        for (int i = 0; i < K2; ++i)
            for (int j = 0; j < K2; ++j) {
                double s = 0;
                for (int t = 0; t < K2; ++t)
                    s += tmp[static_cast<size_t>(i) * K2 + t] * Jp[static_cast<size_t>(j) * K2 + t];
                CovP[static_cast<size_t>(i) * K2 + j] = s;
            }
    }

    // G_s = d vec(Phi_s) / d vec(A)' = sum_{u=0}^{s-1} J (F')^{s-1-u} (x) Phi_u.
    // Elementwise, with vec(Phi)[b*K+a] = Phi[a][b] and vec(A)[rr*K+c] = A[c][rr]:
    //   G_s[(b*K+a)][(rr*K+c)] = sum_u W_{s-1-u}[rr][b] * Phi_u[a][c].
    std::vector<std::vector<double>> Gs(static_cast<size_t>(S) + 1), Cs(static_cast<size_t>(S) + 1),
        Ds(static_cast<size_t>(S) + 1);
    for (int s = 0; s <= S; ++s) {
        std::vector<double>& G = Gs[static_cast<size_t>(s)];
        G.assign(static_cast<size_t>(K2) * Ka, 0.0);
        for (int u = 0; u < s; ++u) {
            const std::vector<double>& Wu = W[static_cast<size_t>(s - 1 - u)];
            const std::vector<double>& Pu = Phi[static_cast<size_t>(u)];
            for (int b = 0; b < K; ++b)
                for (int a = 0; a < K; ++a)
                    for (int rr = 0; rr < Kp; ++rr) {
                        const double w = Wu[static_cast<size_t>(rr) * K + b];
                        if (w == 0.0) continue;
                        for (int c = 0; c < K; ++c)
                            G[static_cast<size_t>(b * K + a) * Ka + (rr * K + c)] +=
                                w * Pu[static_cast<size_t>(a) * K + c];
                    }
        }
        // C_s = (P' (x) I_K) G_s -- the coefficient-uncertainty channel of the OIRF.
        std::vector<double>& C = Cs[static_cast<size_t>(s)];
        C.assign(static_cast<size_t>(K2) * Ka, 0.0);
        if (r.sigma_pd)
            for (int b = 0; b < K; ++b)
                for (int a = 0; a < K; ++a)
                    for (int b2 = 0; b2 < K; ++b2) {
                        const double pv = P[static_cast<size_t>(b2) * K + b];
                        if (pv == 0.0) continue;
                        for (int col = 0; col < Ka; ++col)
                            C[static_cast<size_t>(b * K + a) * Ka + col] +=
                                pv * G[static_cast<size_t>(b2 * K + a) * Ka + col];
                    }
        // D_s = I_K (x) Phi_s -- the Sigma-uncertainty channel of the OIRF.
        std::vector<double>& D = Ds[static_cast<size_t>(s)];
        D.assign(static_cast<size_t>(K2) * K2, 0.0);
        for (int b = 0; b < K; ++b)
            for (int a = 0; a < K; ++a)
                for (int a2 = 0; a2 < K; ++a2)
                    D[static_cast<size_t>(b * K + a) * K2 + (b * K + a2)] =
                        Phi[static_cast<size_t>(s)][static_cast<size_t>(a) * K + a2];
    }

    // ---- assemble the cells ------------------------------------------------
    std::vector<double> Gc(static_cast<size_t>(K2) * Ka, 0.0), Cc(static_cast<size_t>(K2) * Ka, 0.0),
        Dc(static_cast<size_t>(K2) * K2, 0.0);
    std::vector<double> Psi(static_cast<size_t>(K) * K, 0.0), Cum(static_cast<size_t>(K) * K, 0.0),
        CumO(static_cast<size_t>(K) * K, 0.0);
    std::vector<double> vIrf, vCirf, vOirfA, vOirfS, vCoirfA, vCoirfS;
    r.irf.reserve(static_cast<size_t>(S + 1) * static_cast<size_t>(K2));
    for (int s = 0; s <= S; ++s) {
        for (size_t i = 0; i < Gc.size(); ++i) Gc[i] += Gs[static_cast<size_t>(s)][i];
        for (size_t i = 0; i < Cc.size(); ++i) Cc[i] += Cs[static_cast<size_t>(s)][i];
        for (int i = 0; i < K * K; ++i) {
            Psi[static_cast<size_t>(i)] += Phi[static_cast<size_t>(s)][static_cast<size_t>(i)];
            Cum[static_cast<size_t>(i)] += Phi[static_cast<size_t>(s)][static_cast<size_t>(i)];
            CumO[static_cast<size_t>(i)] += Theta[static_cast<size_t>(s)][static_cast<size_t>(i)];
        }
        for (int b = 0; b < K; ++b)
            for (int a = 0; a < K; ++a)
                for (int a2 = 0; a2 < K; ++a2)
                    Dc[static_cast<size_t>(b * K + a) * K2 + (b * K + a2)] =
                        Psi[static_cast<size_t>(a) * K + a2];

        quad_diag(Gs[static_cast<size_t>(s)], Salpha, K2, Ka, vIrf);
        quad_diag(Gc, Salpha, K2, Ka, vCirf);
        if (r.sigma_pd) {
            quad_diag(Cs[static_cast<size_t>(s)], Salpha, K2, Ka, vOirfA);
            quad_diag(Ds[static_cast<size_t>(s)], CovP, K2, K2, vOirfS);
            quad_diag(Cc, Salpha, K2, Ka, vCoirfA);
            quad_diag(Dc, CovP, K2, K2, vCoirfS);
        }

        // Forecast-error variance decomposition over horizon s (steps 0..s-1):
        //   fevd_{ab}(s) = sum_{u<s} Theta_u[a][b]^2 / sum_n sum_{u<s} Theta_u[a][n]^2
        //   mse_a(s)     = sqrt(denominator) -- the forecast STANDARD error, which
        //                  is what the .irf file's `mse` column actually stores.
        std::vector<double> num(static_cast<size_t>(K) * K, 0.0), den(static_cast<size_t>(K), 0.0);
        for (int u = 0; u < s; ++u)
            for (int a = 0; a < K; ++a)
                for (int b = 0; b < K; ++b) {
                    const double th = Theta[static_cast<size_t>(u)][static_cast<size_t>(a) * K + b];
                    num[static_cast<size_t>(a) * K + b] += th * th;
                    den[static_cast<size_t>(a)] += th * th;
                }

        for (int a = 0; a < K; ++a)
            for (int b = 0; b < K; ++b) {
                VarIrfCell cell;
                cell.step = s;
                cell.response = a;
                cell.impulse = b;
                cell.orth_ok = r.sigma_pd;
                const size_t vi = static_cast<size_t>(b * K + a);
                cell.irf = Phi[static_cast<size_t>(s)][static_cast<size_t>(a) * K + b];
                cell.cirf = Cum[static_cast<size_t>(a) * K + b];
                cell.stdirf = std::sqrt(std::max(0.0, vIrf[vi]));
                cell.stdcirf = std::sqrt(std::max(0.0, vCirf[vi]));
                if (r.sigma_pd) {
                    cell.oirf = Theta[static_cast<size_t>(s)][static_cast<size_t>(a) * K + b];
                    cell.coirf = CumO[static_cast<size_t>(a) * K + b];
                    cell.stdoirf = std::sqrt(std::max(0.0, vOirfA[vi] + vOirfS[vi]));
                    cell.stdcoirf = std::sqrt(std::max(0.0, vCoirfA[vi] + vCoirfS[vi]));
                    cell.mse = std::sqrt(std::max(0.0, den[static_cast<size_t>(a)]));
                    cell.fevd = (den[static_cast<size_t>(a)] > 0)
                                    ? num[static_cast<size_t>(a) * K + b] / den[static_cast<size_t>(a)]
                                    : 0.0;
                    // Delta-method SE of the FEVD share: the gradient of
                    // omega_{ab}(s) in the Theta_u, chained through C_u (the
                    // coefficient channel) and D_u (the Cholesky channel), then
                    // sandwiched with Salpha / CovP.
                    double sd = 0;
                    if (den[static_cast<size_t>(a)] > 0 && s > 0) {
                        std::vector<double> ga(static_cast<size_t>(Ka), 0.0),
                            gsg(static_cast<size_t>(K2), 0.0);
                        const double w = cell.fevd, D0 = den[static_cast<size_t>(a)];
                        for (int u = 0; u < s; ++u)
                            for (int nn = 0; nn < K; ++nn) {
                                const double th =
                                    Theta[static_cast<size_t>(u)][static_cast<size_t>(a) * K + nn];
                                const double g = (2.0 * th / D0) * ((nn == b) ? (1.0 - w) : -w);
                                if (g == 0.0) continue;
                                const size_t rowi = static_cast<size_t>(nn * K + a);
                                for (int col = 0; col < Ka; ++col)
                                    ga[static_cast<size_t>(col)] +=
                                        g * Cs[static_cast<size_t>(u)][rowi * static_cast<size_t>(Ka) +
                                                                       static_cast<size_t>(col)];
                                for (int col = 0; col < K2; ++col)
                                    gsg[static_cast<size_t>(col)] +=
                                        g * Ds[static_cast<size_t>(u)][rowi * static_cast<size_t>(K2) +
                                                                       static_cast<size_t>(col)];
                            }
                        sd = std::sqrt(
                            std::max(0.0, quad_form(ga, Salpha, Ka) + quad_form(gsg, CovP, K2)));
                    }
                    cell.stdfevd = sd;
                }
                r.irf.push_back(cell);
            }
    }
    return r;
}

}  // namespace cecon
