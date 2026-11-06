// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// cecon — classic (i.i.d.) IV postestimation diagnostics after 2SLS:
//   estat endogenous : Durbin χ² and Wu-Hausman F (Durbin-Wu-Hausman test)
//   estat overid     : Sargan χ² and Basmann χ² overidentification tests
//   estat firststage : first-stage F and partial R² per endogenous regressor
// Formulas per Stata's ivregress postestimation spec (endogenous/overid/firststage).
// Notation: y; Y = endogenous (p1); X = included exogenous incl. constant (kx);
// Z2 = excluded instruments (m). Z=[X,Z2] (kZ=kx+m), W=[Y,X] (kW=p1+kx).
// Durbin = (ue'P_{[Z,Y]}ue − uc'P_Z uc)/(ue'ue/N); Wu-Hausman is its F analog;
// Sargan = N·uc'P_Z uc/(uc'uc); Basmann = Sargan·(N−kZ)/(N−Sargan). Robust/HAC
// (Wooldridge), Shea's R² and Cragg-Donald min-eigenvalue are not implemented.
#pragma once
#include <cmath>
#include <vector>

#include "core/linalg.h"
#include "estimators/posthest.h"

namespace cecon {

struct IvDiag {
    double durbin = 0, durbin_p = 1;
    int durbin_df = 0;
    double wu = 0, wu_p = 1, wu_df_r = 0;
    int wu_df = 0;
    double sargan = 0, sargan_p = 1, basmann = 0, basmann_p = 1;
    int overid_df = 0;
    bool overid_ok = false;  // only when m > p1
    std::vector<double> fs_F, fs_F_p, fs_partial_r2, fs_r2;  // per endogenous regressor
    bool ok = false;
};

namespace ivd {
// v' P_A v with P_A = A (A'A)^-1 A'. A is n*kA col-major.
inline double quad_proj(const std::vector<double>& A, int64_t n, int kA, const std::vector<double>& v) {
    std::vector<double> AtA(static_cast<size_t>(kA) * kA), Atv(kA);
    la::xtx(A.data(), n, kA, AtA.data());
    la::xtv(A.data(), n, kA, v.data(), Atv.data());
    if (la::spd_inverse(AtA.data(), kA) != 0) return 0.0;
    std::vector<double> tmp(kA);
    la::matvec(AtA.data(), kA, Atv.data(), tmp.data());
    double q = 0;
    for (int i = 0; i < kA; ++i) q += Atv[i] * tmp[i];
    return q;
}
// OLS RSS of v on A (col-major n*kA).
inline double ols_rss(const std::vector<double>& A, int64_t n, int kA, const std::vector<double>& v) {
    std::vector<double> AtA(static_cast<size_t>(kA) * kA), Atv(kA), beta(kA);
    la::xtx(A.data(), n, kA, AtA.data());
    la::xtv(A.data(), n, kA, v.data(), Atv.data());
    if (la::spd_inverse(AtA.data(), kA) != 0) return 0.0;
    la::matvec(AtA.data(), kA, Atv.data(), beta.data());
    double rss = 0;
    for (int64_t i = 0; i < n; ++i) {
        double r = v[i];
        for (int p = 0; p < kA; ++p) r -= A[static_cast<int64_t>(p) * n + i] * beta[p];
        rss += r * r;
    }
    return rss;
}
}  // namespace ivd

// Y/X/Z2 are col-major n*(p1/kx/m). X includes the constant column.
inline IvDiag iv_diagnostics(const std::vector<double>& y, int64_t n,
                             const std::vector<double>& Y, int p1,
                             const std::vector<double>& X, int kx,
                             const std::vector<double>& Z2, int m) {
    IvDiag r;
    int kZ = kx + m, kW = p1 + kx;
    std::vector<double> Z(static_cast<size_t>(n) * kZ), W(static_cast<size_t>(n) * kW);
    std::copy(X.begin(), X.end(), Z.begin());
    std::copy(Z2.begin(), Z2.end(), Z.begin() + static_cast<int64_t>(kx) * n);
    std::copy(Y.begin(), Y.end(), W.begin());
    std::copy(X.begin(), X.end(), W.begin() + static_cast<int64_t>(p1) * n);

    std::vector<double> ZtZ(static_cast<size_t>(kZ) * kZ);
    la::xtx(Z.data(), n, kZ, ZtZ.data());
    if (la::spd_inverse(ZtZ.data(), kZ) != 0) return r;  // ZtZ now = (Z'Z)^-1
    // What = P_Z W (project every W column onto Z)
    std::vector<double> What(static_cast<size_t>(n) * kW);
    for (int c = 0; c < kW; ++c) {
        std::vector<double> Ztw(kZ, 0.0);
        const double* wc = W.data() + static_cast<int64_t>(c) * n;
        for (int j = 0; j < kZ; ++j) {
            double s = 0;
            const double* zj = Z.data() + static_cast<int64_t>(j) * n;
            for (int64_t i = 0; i < n; ++i) s += zj[i] * wc[i];
            Ztw[j] = s;
        }
        std::vector<double> coef(kZ);
        la::matvec(ZtZ.data(), kZ, Ztw.data(), coef.data());
        double* hc = What.data() + static_cast<int64_t>(c) * n;
        for (int64_t i = 0; i < n; ++i) {
            double s = 0;
            for (int j = 0; j < kZ; ++j) s += Z[static_cast<int64_t>(j) * n + i] * coef[j];
            hc[i] = s;
        }
    }
    // 2SLS β = (What'W)^-1 (What'y); residuals uc = y - W β
    std::vector<double> M1(static_cast<size_t>(kW) * kW, 0.0), rhs(kW, 0.0);
    for (int aa = 0; aa < kW; ++aa) {
        const double* ha = What.data() + static_cast<int64_t>(aa) * n;
        for (int bb = 0; bb < kW; ++bb) {
            double s = 0;
            const double* wb = W.data() + static_cast<int64_t>(bb) * n;
            for (int64_t i = 0; i < n; ++i) s += ha[i] * wb[i];
            M1[static_cast<size_t>(aa) * kW + bb] = s;
        }
        double sy = 0;
        for (int64_t i = 0; i < n; ++i) sy += ha[i] * y[i];
        rhs[aa] = sy;
    }
    if (la::spd_inverse(M1.data(), kW) != 0) return r;
    std::vector<double> b2sls(kW);
    la::matvec(M1.data(), kW, rhs.data(), b2sls.data());
    std::vector<double> uc(n);
    double eqnrss = 0;
    for (int64_t i = 0; i < n; ++i) {
        double f = 0;
        for (int c = 0; c < kW; ++c) f += W[static_cast<int64_t>(c) * n + i] * b2sls[c];
        uc[i] = y[i] - f;
        eqnrss += uc[i] * uc[i];
    }
    double ucPuc = ivd::quad_proj(Z, n, kZ, uc);

    // efficient OLS: y on W; residuals ue
    std::vector<double> WtW(static_cast<size_t>(kW) * kW), Wty(kW), bols(kW);
    la::xtx(W.data(), n, kW, WtW.data());
    la::xtv(W.data(), n, kW, y.data(), Wty.data());
    if (la::spd_inverse(WtW.data(), kW) != 0) return r;
    la::matvec(WtW.data(), kW, Wty.data(), bols.data());
    std::vector<double> ue(n);
    double USSR = 0;
    for (int64_t i = 0; i < n; ++i) {
        double f = 0;
        for (int c = 0; c < kW; ++c) f += W[static_cast<int64_t>(c) * n + i] * bols[c];
        ue[i] = y[i] - f;
        USSR += ue[i] * ue[i];
    }
    // M = [Z, Y]; uePue = ue' P_M ue
    std::vector<double> Mmat(static_cast<size_t>(n) * (kZ + p1));
    std::copy(Z.begin(), Z.end(), Mmat.begin());
    std::copy(Y.begin(), Y.end(), Mmat.begin() + static_cast<int64_t>(kZ) * n);
    double uePue = ivd::quad_proj(Mmat, n, kZ + p1, ue);

    double Nd = static_cast<double>(n);
    double num = uePue - ucPuc;
    r.durbin_df = p1;
    r.durbin = num / (USSR / Nd);
    r.durbin_p = chi2_pvalue(r.durbin, p1);
    int kcons = p1 + kx;  // total params of the consistent model (incl. constant)
    r.wu_df = p1;
    r.wu_df_r = static_cast<double>(n - kcons - p1);
    r.wu = (num / p1) / ((USSR - num) / r.wu_df_r);
    r.wu_p = (r.wu_df_r > 0) ? f_pvalue(r.wu, p1, r.wu_df_r) : 1.0;

    if (m > p1) {
        r.overid_ok = true;
        r.overid_df = m - p1;
        r.sargan = Nd * ucPuc / eqnrss;
        r.sargan_p = chi2_pvalue(r.sargan, r.overid_df);
        r.basmann = r.sargan * (Nd - kZ) / (Nd - r.sargan);
        r.basmann_p = chi2_pvalue(r.basmann, r.overid_df);
    }

    // first stage per endogenous regressor: Y_j on X (restricted) vs Z (full)
    for (int j = 0; j < p1; ++j) {
        std::vector<double> yj(Y.begin() + static_cast<int64_t>(j) * n,
                               Y.begin() + static_cast<int64_t>(j + 1) * n);
        double rss_r = ivd::ols_rss(X, n, kx, yj);
        double rss_u = ivd::ols_rss(Z, n, kZ, yj);
        double dfd = static_cast<double>(n - kZ);
        double F = (dfd > 0 && rss_u > 0) ? ((rss_r - rss_u) / m) / (rss_u / dfd) : 0.0;
        r.fs_F.push_back(F);
        r.fs_F_p.push_back((dfd > 0) ? f_pvalue(F, m, dfd) : 1.0);
        r.fs_partial_r2.push_back((rss_r > 0) ? (rss_r - rss_u) / rss_r : 0.0);
        // full first-stage R² (centered): yj on Z; mean(yj) removed
        double mean = 0; for (double v : yj) mean += v; mean /= Nd;
        double tss = 0; for (double v : yj) { double t = v - mean; tss += t * t; }
        r.fs_r2.push_back((tss > 0) ? 1.0 - rss_u / tss : 0.0);
    }
    r.ok = true;
    return r;
}

}  // namespace cecon
