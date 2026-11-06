// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// cecon — multi-endogenous weak-instrument statistics (generalizes the 1-endog
// first-stage / KP rk F to p>=2 endogenous regressors). Matches ivreg2 e(widstat):
//   homoskedastic  -> Cragg-Donald minimum-eigenvalue F
//   robust/cluster -> Kleibergen-Paap rk Wald F
// Notation: Y = endogenous (p), X1 = included exogenous incl const (kx1), Z2 =
// excluded instruments (m). Partial X1 out (FWL): Ỹ=M_X1 Y, Z̃2=M_X1 Z2. Uses sym_eig.
#pragma once
#include <cmath>
#include <vector>

#include "core/linalg.h"

namespace cecon {

namespace wkid {
// residualize each column of A (n*k) on X1 (n*kx1): returns M_X1 A.
inline std::vector<double> partial_out(const std::vector<double>& A, int64_t n, int k,
                                       const std::vector<double>& X1, int kx1,
                                       const std::vector<double>& X1tX1inv) {
    std::vector<double> out = A;
    for (int c = 0; c < k; ++c) {
        std::vector<double> X1ta(kx1, 0.0);
        const double* ac = A.data() + static_cast<int64_t>(c) * n;
        for (int j = 0; j < kx1; ++j) { double s = 0; const double* xj = X1.data() + static_cast<int64_t>(j) * n;
            for (int64_t i = 0; i < n; ++i) s += xj[i] * ac[i]; X1ta[j] = s; }
        std::vector<double> coef(kx1, 0.0);
        la::matvec(X1tX1inv.data(), kx1, X1ta.data(), coef.data());
        double* oc = out.data() + static_cast<int64_t>(c) * n;
        for (int64_t i = 0; i < n; ++i) { double f = 0;
            for (int j = 0; j < kx1; ++j) f += X1[static_cast<int64_t>(j) * n + i] * coef[j];
            oc[i] -= f; }
    }
    return out;
}
inline std::vector<double> cross(const std::vector<double>& A, int ka, const std::vector<double>& B, int kb, int64_t n) {
    std::vector<double> C(static_cast<size_t>(ka) * kb, 0.0);
    for (int a = 0; a < ka; ++a)
        for (int b = 0; b < kb; ++b) { double s = 0;
            for (int64_t i = 0; i < n; ++i) s += A[static_cast<int64_t>(a) * n + i] * B[static_cast<int64_t>(b) * n + i];
            C[static_cast<size_t>(a) * kb + b] = s; }
    return C;
}
// min generalized eigenvalue of (A, B), A symmetric, B SPD (both k*k): eig(B^-1/2 A B^-1/2).
inline double min_geneig(std::vector<double> A, std::vector<double> B, int k) {
    std::vector<double> wB(k);
    if (la::sym_eig(B.data(), k, wB.data()) != 0) return -1;
    std::vector<double> Bih(static_cast<size_t>(k) * k, 0.0);
    for (int j = 0; j < k; ++j) { if (wB[j] <= 0) return -1; double iv = 1.0 / std::sqrt(wB[j]);
        const double* vj = B.data() + static_cast<int64_t>(j) * k;
        for (int c = 0; c < k; ++c) for (int r = 0; r < k; ++r) Bih[static_cast<size_t>(c) * k + r] += iv * vj[r] * vj[c]; }
    std::vector<double> tmp(static_cast<size_t>(k) * k, 0.0), S(static_cast<size_t>(k) * k, 0.0);
    for (int a = 0; a < k; ++a) for (int b = 0; b < k; ++b) { double s = 0;
        for (int c = 0; c < k; ++c) s += Bih[static_cast<size_t>(a) * k + c] * A[static_cast<size_t>(c) * k + b]; tmp[static_cast<size_t>(a) * k + b] = s; }
    for (int a = 0; a < k; ++a) for (int b = 0; b < k; ++b) { double s = 0;
        for (int c = 0; c < k; ++c) s += tmp[static_cast<size_t>(a) * k + c] * Bih[static_cast<size_t>(c) * k + b]; S[static_cast<size_t>(a) * k + b] = s; }
    std::vector<double> wS(k);
    if (la::sym_eig(S.data(), k, wS.data()) != 0) return -1;
    return wS[0];  // ascending -> minimum
}
}  // namespace wkid

// Cragg-Donald minimum-eigenvalue F (homoskedastic weak-IV F, ivreg2 e(widstat)).
inline double cragg_donald_F(const std::vector<double>& Y, int p,
                             const std::vector<double>& X1, int kx1,
                             const std::vector<double>& Z2, int m, int64_t n) {
    int kZ = kx1 + m;
    std::vector<double> X1tX1(static_cast<size_t>(kx1) * kx1);
    la::xtx(X1.data(), n, kx1, X1tX1.data());
    if (la::spd_inverse(X1tX1.data(), kx1) != 0) return -1;
    std::vector<double> Yt = wkid::partial_out(Y, n, p, X1, kx1, X1tX1);
    std::vector<double> Z2t = wkid::partial_out(Z2, n, m, X1, kx1, X1tX1);
    std::vector<double> Z2tZ2 = wkid::cross(Z2t, m, Z2t, m, n);
    std::vector<double> Z2tZ2inv = Z2tZ2;
    if (la::spd_inverse(Z2tZ2inv.data(), m) != 0) return -1;
    std::vector<double> Z2tY = wkid::cross(Z2t, m, Yt, p, n);  // m x p
    // M_exp = (Z2'Y)' (Z2'Z2)^-1 (Z2'Y)  (p x p) = Ỹ'P_Z̃2 Ỹ
    std::vector<double> tmp(static_cast<size_t>(m) * p, 0.0);  // Z2tZ2inv * Z2tY
    for (int a = 0; a < m; ++a) for (int b = 0; b < p; ++b) { double s = 0;
        for (int c = 0; c < m; ++c) s += Z2tZ2inv[static_cast<size_t>(a) * m + c] * Z2tY[static_cast<size_t>(c) * p + b]; tmp[static_cast<size_t>(a) * p + b] = s; }
    std::vector<double> Mexp(static_cast<size_t>(p) * p, 0.0);
    for (int a = 0; a < p; ++a) for (int b = 0; b < p; ++b) { double s = 0;
        for (int c = 0; c < m; ++c) s += Z2tY[static_cast<size_t>(c) * p + a] * tmp[static_cast<size_t>(c) * p + b]; Mexp[static_cast<size_t>(a) * p + b] = s; }
    // Sigma_VV = (Ỹ'Ỹ - M_exp) / (n - kZ)
    std::vector<double> YtY = wkid::cross(Yt, p, Yt, p, n);
    std::vector<double> Sig(static_cast<size_t>(p) * p);
    double df = static_cast<double>(n - kZ);
    for (int i = 0; i < p * p; ++i) Sig[i] = (YtY[i] - Mexp[i]) / df;
    double lam = wkid::min_geneig(Mexp, Sig, p);
    return (lam < 0) ? -1 : lam / static_cast<double>(m);
}

}  // namespace cecon
