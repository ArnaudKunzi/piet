// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "estimators/iv2sls.h"
#include "core/error.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

#include "core/collinear.h"
#include "core/linalg.h"
#include "estimators/posthest.h"
#include "hdfe/demean.h"

namespace cecon {
namespace {

// Robust/cluster "meat" Σ (score)(score)' for the instruments W weighted by a
// per-obs w[i] (cluster: sum within cluster first). Used both for the KP rk Wald
// F (w = first-stage residuals) and Hansen J (w = 2SLS residuals).
std::vector<double> iv_robust_meat(const double* W, int64_t n, int kw, const double* w,
                                   const Factor* cl, int G, bool clust) {
    std::vector<double> M(static_cast<size_t>(kw) * kw, 0.0);
    if (clust && cl) {
        std::vector<double> S(static_cast<size_t>(G) * kw, 0.0);  // per-cluster sum of w_i W_i
        for (int64_t i = 0; i < n; ++i) {
            int32_t c = cl->codes[i];
            double wi = w[i];
            for (int a = 0; a < kw; ++a) S[static_cast<size_t>(c) * kw + a] += wi * W[static_cast<int64_t>(a) * n + i];
        }
        for (int g = 0; g < G; ++g)
            for (int a = 0; a < kw; ++a) {
                double sa = S[static_cast<size_t>(g) * kw + a];
                for (int b = 0; b < kw; ++b) M[static_cast<size_t>(a) * kw + b] += sa * S[static_cast<size_t>(g) * kw + b];
            }
    } else {
        for (int64_t i = 0; i < n; ++i) {
            double wi2 = w[i] * w[i];
            for (int a = 0; a < kw; ++a) {
                double wa = wi2 * W[static_cast<int64_t>(a) * n + i];
                for (int b = 0; b < kw; ++b) M[static_cast<size_t>(a) * kw + b] += wa * W[static_cast<int64_t>(b) * n + i];
            }
        }
    }
    return M;
}

// General (kendog>=2) Kleibergen-Paap rk Wald statistic = ranktest r(chi2) for the
// test of rank = kendog-1, computed via SVD (Kleibergen-Paap 2006 rank test).
// Returns rk_chi2 (to be fed through the same widstat scaling as the kendog==1
// case), or NaN on numerical failure. Z/Y/exog are FE-demeaned column-major; the
// included exog are partialled out here. Nminus = N - kw - sdofminus.
double kp_rk_chi2(const double* exog, int kexog, const double* Z, int L,
                  const double* Y, int K1, int64_t n, const Factor* cl, int G,
                  bool clust, double Nminus) {
    const size_t N = static_cast<size_t>(n);
    std::vector<double> Zp(Z, Z + static_cast<size_t>(L) * N);
    std::vector<double> Yp(Y, Y + static_cast<size_t>(K1) * N);
    // --- partial the included exog out of the instruments and endogenous regs ---
    if (kexog > 0) {
        std::vector<double> PtP(static_cast<size_t>(kexog) * kexog);
        la::xtx(exog, n, kexog, PtP.data());
        if (la::spd_inverse(PtP.data(), kexog) == 0) {
            auto partial_cols = [&](std::vector<double>& M, int kc) {
                std::vector<double> Pv(kexog), bb(kexog);
                for (int c = 0; c < kc; ++c) {
                    double* col = M.data() + static_cast<size_t>(c) * N;
                    la::xtv(exog, n, kexog, col, Pv.data());
                    la::matvec(PtP.data(), kexog, Pv.data(), bb.data());
                    for (int64_t i = 0; i < n; ++i) {
                        double f = 0;
                        for (int e = 0; e < kexog; ++e) f += exog[static_cast<int64_t>(e) * n + i] * bb[e];
                        col[i] -= f;
                    }
                }
            };
            partial_cols(Zp, L);
            partial_cols(Yp, K1);
        }
    }
    const double invn = 1.0 / static_cast<double>(n);
    std::vector<double> Qzz(static_cast<size_t>(L) * L), Qyy(static_cast<size_t>(K1) * K1),
        Qzy(static_cast<size_t>(L) * K1);
    la::xtx(Zp.data(), n, L, Qzz.data());
    for (auto& v : Qzz) v *= invn;
    la::xtx(Yp.data(), n, K1, Qyy.data());
    for (auto& v : Qyy) v *= invn;
    for (int j = 0; j < K1; ++j) {
        std::vector<double> t(L);
        la::xtv(Zp.data(), n, L, Yp.data() + static_cast<size_t>(j) * N, t.data());
        for (int a = 0; a < L; ++a) Qzy[static_cast<size_t>(j) * L + a] = t[a] * invn;
    }
    std::vector<double> iQzz(Qzz);
    if (la::spd_inverse(iQzz.data(), L) != 0) return std::numeric_limits<double>::quiet_NaN();
    std::vector<double> pihat(static_cast<size_t>(L) * K1);  // L x K1
    for (int j = 0; j < K1; ++j)
        la::matvec(iQzz.data(), L, Qzy.data() + static_cast<size_t>(j) * L,
                   pihat.data() + static_cast<size_t>(j) * L);
    std::vector<double> vhat(static_cast<size_t>(K1) * N);   // residuals Yp - Zp*pihat
    for (int j = 0; j < K1; ++j) {
        const double* pj = pihat.data() + static_cast<size_t>(j) * L;
        double* vj = vhat.data() + static_cast<size_t>(j) * N;
        for (int64_t i = 0; i < n; ++i) {
            double f = 0;
            for (int a = 0; a < L; ++a) f += Zp[static_cast<int64_t>(a) * n + i] * pj[a];
            vj[i] = Yp[static_cast<size_t>(j) * N + i] - f;
        }
    }
    // shat0 = raw robust/cluster meat of m_i = vhat_i (x) zp_i  (d = K1*L)
    const int d = K1 * L;
    std::vector<double> shat0(static_cast<size_t>(d) * d, 0.0), m(d);
    auto mvec = [&](int64_t i) {
        for (int j = 0; j < K1; ++j) {
            double vj = vhat[static_cast<size_t>(j) * N + i];
            for (int a = 0; a < L; ++a) m[static_cast<size_t>(j) * L + a] = vj * Zp[static_cast<int64_t>(a) * n + i];
        }
    };
    if (clust && cl) {
        std::vector<double> S(static_cast<size_t>(G) * d, 0.0);
        for (int64_t i = 0; i < n; ++i) {
            mvec(i);
            double* s = S.data() + static_cast<size_t>(cl->codes[i]) * d;
            for (int t = 0; t < d; ++t) s[t] += m[t];
        }
        for (int g = 0; g < G; ++g) {
            const double* s = S.data() + static_cast<size_t>(g) * d;
            for (int a = 0; a < d; ++a) {
                double sa = s[a];
                double* row = shat0.data() + static_cast<size_t>(a) * d;
                for (int b = 0; b < d; ++b) row[b] += sa * s[b];
            }
        }
    } else {
        for (int64_t i = 0; i < n; ++i) {
            mvec(i);
            for (int a = 0; a < d; ++a) {
                double ma = m[a];
                double* row = shat0.data() + static_cast<size_t>(a) * d;
                for (int b = 0; b < d; ++b) row[b] += ma * m[b];
            }
        }
    }
    // ranktest normalizes Qzz/Qyy/Qzy by N but takes the moment meat as a raw sum;
    // matching that mixed convention leaves shat0 one factor of N too large here.
    for (auto& v : shat0) v /= static_cast<double>(n);

    // symmetric matrix powers via eigendecomposition: Q^p = V diag(lam^p) V'
    auto sym_pow = [&](const std::vector<double>& Q, int k, double p) -> std::vector<double> {
        std::vector<double> A(Q), w(k);
        if (la::sym_eig(A.data(), k, w.data()) != 0) return {};
        std::vector<double> R(static_cast<size_t>(k) * k, 0.0);
        for (int j = 0; j < k; ++j) {
            double lam = w[j];
            if (lam <= 0) continue;
            double s = std::pow(lam, p);
            const double* vj = A.data() + static_cast<size_t>(j) * k;
            for (int c = 0; c < k; ++c)
                for (int r = 0; r < k; ++r) R[static_cast<size_t>(c) * k + r] += s * vj[r] * vj[c];
        }
        return R;
    };
    std::vector<double> Zh = sym_pow(Qzz, L, 0.5), iZh = sym_pow(Qzz, L, -0.5),
                        iYh = sym_pow(Qyy, K1, -0.5);
    if (Zh.empty() || iZh.empty() || iYh.empty()) return std::numeric_limits<double>::quiet_NaN();
    // kpthat = Zh * pihat * iYh   (L x K1)
    std::vector<double> tmp(static_cast<size_t>(L) * K1, 0.0);
    for (int j = 0; j < K1; ++j)
        for (int a = 0; a < L; ++a) {
            double s = 0;
            for (int b = 0; b < L; ++b) s += Zh[static_cast<size_t>(b) * L + a] * pihat[static_cast<size_t>(j) * L + b];
            tmp[static_cast<size_t>(j) * L + a] = s;
        }
    std::vector<double> kpthat(static_cast<size_t>(L) * K1, 0.0);
    for (int a = 0; a < L; ++a)
        for (int j = 0; j < K1; ++j) {
            double s = 0;
            for (int l = 0; l < K1; ++l) s += tmp[static_cast<size_t>(l) * L + a] * iYh[static_cast<size_t>(j) * K1 + l];
            kpthat[static_cast<size_t>(j) * L + a] = s;
        }
    // kpvar = T * shat0 * T,  T = (iYh (x) iZh)  (d x d, symmetric)
    std::vector<double> T(static_cast<size_t>(d) * d, 0.0);
    for (int j = 0; j < K1; ++j)
        for (int a = 0; a < L; ++a)
            for (int l = 0; l < K1; ++l)
                for (int b = 0; b < L; ++b)
                    T[static_cast<size_t>(l * L + b) * d + (j * L + a)] =
                        iYh[static_cast<size_t>(l) * K1 + j] * iZh[static_cast<size_t>(b) * L + a];
    std::vector<double> Ts(static_cast<size_t>(d) * d, 0.0), kpvar(static_cast<size_t>(d) * d, 0.0);
    la::gemm_nt(T.data(), shat0.data(), Ts.data(), d, d, d);  // Ts = T * shat0' = T*shat0 (sym)
    la::gemm_nt(Ts.data(), T.data(), kpvar.data(), d, d, d);  // kpvar = Ts * T' = T shat0 T
    // SVD kpthat = U S V'
    std::vector<double> A(kpthat), U(static_cast<size_t>(L) * L), sv(std::min(L, K1)),
        Vt(static_cast<size_t>(K1) * K1);
    if (la::svd(A.data(), L, K1, U.data(), sv.data(), Vt.data()) != 0)
        return std::numeric_limits<double>::quiet_NaN();
    // V = Vt'
    std::vector<double> V(static_cast<size_t>(K1) * K1);
    for (int i = 0; i < K1; ++i)
        for (int jj = 0; jj < K1; ++jj) V[static_cast<size_t>(jj) * K1 + i] = Vt[static_cast<size_t>(i) * K1 + jj];
    const int kk = K1 - 1, q = L - kk;  // df dimension of lab
    // u22 = U[kk:L, kk:L]  (q x q);  v22 = V[kk:K1, kk:K1] (1 x 1)
    std::vector<double> u22(static_cast<size_t>(q) * q);
    for (int c = 0; c < q; ++c)
        for (int r = 0; r < q; ++r) u22[static_cast<size_t>(c) * q + r] = U[static_cast<size_t>(kk + c) * L + (kk + r)];
    double v22 = V[static_cast<size_t>(kk) * K1 + kk];
    // u22h = sqrtm(u22 u22'); v22h = |v22|
    std::vector<double> u22ut(static_cast<size_t>(q) * q, 0.0);
    la::gemm_nt(u22.data(), u22.data(), u22ut.data(), q, q, q);  // u22 * u22'
    std::vector<double> u22h = sym_pow(u22ut, q, 0.5);
    if (u22h.empty()) return std::numeric_limits<double>::quiet_NaN();
    double v22h = std::fabs(v22);
    // aq = U2 * inv(u22) * u22h ,  U2 = U[:, kk:L]  (L x q)
    std::vector<double> iu22(u22);
    if (la::gen_inverse(iu22.data(), q) != 0) return std::numeric_limits<double>::quiet_NaN();
    std::vector<double> U2(static_cast<size_t>(L) * q);
    for (int c = 0; c < q; ++c)
        for (int r = 0; r < L; ++r) U2[static_cast<size_t>(c) * L + r] = U[static_cast<size_t>(kk + c) * L + r];
    std::vector<double> U2iu(static_cast<size_t>(L) * q, 0.0);  // U2 * inv(u22)  (L x q)
    for (int r = 0; r < L; ++r)
        for (int c = 0; c < q; ++c) {
            double s = 0;
            for (int t = 0; t < q; ++t) s += U2[static_cast<size_t>(t) * L + r] * iu22[static_cast<size_t>(c) * q + t];
            U2iu[static_cast<size_t>(c) * L + r] = s;
        }
    std::vector<double> aq(static_cast<size_t>(L) * q, 0.0);  // (U2 inv(u22)) * u22h  (L x q)
    for (int r = 0; r < L; ++r)
        for (int c = 0; c < q; ++c) {
            double s = 0;
            for (int t = 0; t < q; ++t) s += U2iu[static_cast<size_t>(t) * L + r] * u22h[static_cast<size_t>(c) * q + t];
            aq[static_cast<size_t>(c) * L + r] = s;
        }
    // bq = v22h * (1/v22) * V2' ,  V2 = V[:, kk:K1]  (K1 x 1) -> bq is 1 x K1
    if (v22 == 0) return std::numeric_limits<double>::quiet_NaN();
    std::vector<double> bq(K1);
    for (int j = 0; j < K1; ++j) bq[j] = v22h * (1.0 / v22) * V[static_cast<size_t>(kk) * K1 + j];
    // P = bq (x) aq'  ((q) x (K1*L));  P[r, j*L+a] = bq[j] * aq[a-> col r]
    // lab = P vec(kpthat)  (q);  vlab = P kpvar P'  (q x q)
    std::vector<double> P(static_cast<size_t>(q) * d, 0.0);
    for (int r = 0; r < q; ++r)
        for (int j = 0; j < K1; ++j)
            for (int a = 0; a < L; ++a)
                P[static_cast<size_t>(j * L + a) * q + r] = bq[j] * aq[static_cast<size_t>(r) * L + a];
    std::vector<double> lab(q, 0.0);
    for (int r = 0; r < q; ++r) {
        double s = 0;
        for (int c = 0; c < d; ++c) s += P[static_cast<size_t>(c) * q + r] * kpthat[c];
        lab[r] = s;
    }
    // vlab = P kpvar P'
    std::vector<double> PK(static_cast<size_t>(q) * d, 0.0);  // P * kpvar (q x d)
    for (int r = 0; r < q; ++r)
        for (int c = 0; c < d; ++c) {
            double s = 0;
            for (int t = 0; t < d; ++t) s += P[static_cast<size_t>(t) * q + r] * kpvar[static_cast<size_t>(c) * d + t];
            PK[static_cast<size_t>(c) * q + r] = s;
        }
    std::vector<double> vlab(static_cast<size_t>(q) * q, 0.0);
    for (int r = 0; r < q; ++r)
        for (int c = 0; c < q; ++c) {
            double s = 0;
            for (int t = 0; t < d; ++t) s += PK[static_cast<size_t>(t) * q + r] * P[static_cast<size_t>(t) * q + c];
            vlab[static_cast<size_t>(c) * q + r] = s;
        }
    std::vector<double> ivlab(vlab);
    if (la::spd_inverse(ivlab.data(), q) != 0) return std::numeric_limits<double>::quiet_NaN();
    std::vector<double> tv(q, 0.0);
    la::matvec(ivlab.data(), q, lab.data(), tv.data());
    double rk_raw = 0;
    for (int r = 0; r < q; ++r) rk_raw += lab[r] * tv[r];
    (void)Nminus;  // ranktest folds its Nminus into the widstat scaling (caller); with
                   // shat0 normalized by N above, the raw rk statistic scales by N.
    return rk_raw * static_cast<double>(n);
}

// Everything in a 2SLS fit that does NOT depend on the outcome y: the demeaned
// instrument algebra (W, (W'W)^-1, projected Xhat, first-stage gammas, the bread
// (Xhat'Xhat)^-1), the actual regressor matrix X, dof, the homoskedastic
// first-stage F, and the weak-instrument stats (KP rk Wald F, Montiel-Olea-
// Pflueger effective F, KP rk LM). Computed once and reused across outcomes in
// the multi-outcome path. Demeans md.Xexog/Xendog/Zinst in place; never touches y.
struct IvShared {
    int64_t n = 0;
    int kexog = 0, kendog = 0, kinst = 0, kx = 0, kw = 0;
    std::vector<double> W, breadW, Xhat, bread, X;
    std::vector<std::vector<double>> gammas;
    DofInfo dof;
    int k_full = 0;
    // ivreg2 dofminus (xtivreg2: the absorbed groups, IvModelData::dofminus_fe); 0 otherwise.
    int dofminus = 0;
    int dm_iters = 0;
    double dm_resid = 0;
    bool dm_conv = true;
    std::vector<std::string> names, endog_names;
    std::vector<std::string> omitted;  // excluded IVs annihilated by the FE absorption
    // BUG-1225: included regressors annihilated by the FE absorption, and the
    // pre-omission [exog, endog] names (both empty when nothing was dropped).
    std::vector<std::string> omitted_exog, full_names;
    int64_t n_dropped = 0;
    // shared first-stage / weak-id stats
    std::vector<double> first_stage_F;
    double kp_wald_F = std::numeric_limits<double>::quiet_NaN();
    double eff_F = std::numeric_limits<double>::quiet_NaN();
    double kp_lm = std::numeric_limits<double>::quiet_NaN();
    int kp_lm_df = 0;
    double kp_lm_p = std::numeric_limits<double>::quiet_NaN();
    // cluster meta for the per-outcome Hansen J meat
    bool weakid_on = false, clust = false;
    const Factor* cl = nullptr;
    int G = 0;
};

IvShared iv_build_shared(IvModelData& md, Vcov vt, int cluster_index) {
    const int64_t n = md.n;
    const int kendog = md.kendog;
    // kexog/kx and kinst/kw are NOT const: a collinear included regressor (BUG-1225)
    // and a collinear excluded instrument are dropped below (after the FE absorption
    // reveals them), which shrinks them.
    int kexog = md.kexog;
    int kinst = md.kinst;
    int kx = kexog + kendog;             // regressors
    int kw = kexog + kinst;              // instruments W = [exog, excluded IVs]
    // The sample first: with no row every instrument is collinear, and Stata's
    // `ivregress 2sls y (x = w) if 0` is "no observations" r(2000).
    require_sample(n);
    if (kinst < kendog) throw EngineError("under_identified", "under-identified: fewer instruments than endogenous regressors");
    if (n <= kx) throw EngineError("no_obs", "not enough observations");

    IvShared S;
    S.n = n; S.kendog = kendog;
    S.n_dropped = md.n_dropped;
    S.endog_names = md.endog_names;

    // --- demean exog, endog, instruments by the FE set (y demeaned by caller) ---
    // (already done, weighted, when iv_apply_weights met absorbed effects)
    const std::vector<Factor> no_fe;
    const std::vector<Factor>& dm_fe = md.fe_absorbed ? no_fe : md.fe;
    DemeanReport d1 = demean(md.Xexog.data(), n, kexog, dm_fe);
    DemeanReport d2 = demean(md.Xendog.data(), n, kendog, dm_fe);
    DemeanReport d3 = demean(md.Zinst.data(), n, kinst, dm_fe);
    S.dm_iters = std::max(d1.iters, std::max(d2.iters, d3.iters));
    S.dm_resid = std::max(d1.resid, std::max(d2.resid, d3.resid));
    S.dm_conv = d1.converged && d2.converged && d3.converged;

    // --- omit included regressors annihilated by the FE absorption (BUG-1225) -----
    // A regressor constant within an absorbed level demeans to zero (or into the span
    // of the earlier regressors). ivreghdfe's ivreg2_rmcollright2 sweeps the partialled
    // [included exog, excluded IVs, endog] in that order and drops the later member of
    // a collinear set, so such a regressor is omitted -- posted as b 0, se 0 -- and the
    // rest is fitted with it out of K (StataBE: `ivreghdfe y x2 x3 (x1 = z1), absorb(id)`
    // with x2 constant within id: x2 "(omitted)", F(2, 98), _b[x1] 2.0200764942375309,
    // _se[x1] .065431301337589967). Earlier columns win, as in the instrument pass below.
    if (kexog > 0 && !md.fe.empty() && static_cast<int>(md.exog_names.size()) == kexog) {
        const std::vector<char> keep_x = collinear_keep_mask(md.Xexog.data(), n, kexog);
        if (std::find(keep_x.begin(), keep_x.end(), 0) != keep_x.end()) {
            S.full_names = md.exog_names;
            for (const auto& s : md.endog_names) S.full_names.push_back(s);
            S.omitted_exog = compact_columns(md.Xexog, n, md.kexog, md.exog_names, keep_x);
            kexog = md.kexog;
            kx = kexog + kendog;
            kw = kexog + kinst;
        }
    }
    S.kexog = kexog; S.kx = kx;
    S.names = md.exog_names;
    for (auto& s : md.endog_names) S.names.push_back(s);

    // --- omit excluded instruments annihilated by the FE absorption (§35) --------
    // An instrument that is constant within an absorbed FE level (the textbook case
    // is a time-invariant firm attribute under absorb(firm_id)) demeans to exactly
    // zero, making W'W singular. Stata's ivreghdfe SILENTLY OMITS such an instrument
    // and fits on the survivors; cobra used to raise "collinear" and refuse the whole
    // model. Detect the deficiency on the DEMEANED instrument block and drop it.
    //
    // Scope is deliberately narrow — only the EXCLUDED-IV block may be dropped:
    //   * a rank-deficient EXOG block is a collinear-REGRESSOR problem whose remedy
    //     is to change the reported coefficient set, not the instrument set; the
    //     pass above already omitted such a regressor (BUG-1225), so the exog block
    //     reaching here is full rank;
    //   * dropping below kendog surviving instruments is genuine under-identification
    //     and also still raises.
    // The engine has no warning channel on the success path (stderr is discarded
    // there), so the omission is reported as data — S.omitted → IvResult::omitted →
    // the JSON "omitted" array — and NOT as a message.
    if (kinst > 0 && !md.fe.empty()) {
        std::vector<double> Wchk(static_cast<size_t>(n) * kw);
        for (int j = 0; j < kexog; ++j)
            std::copy(md.exog(j), md.exog(j) + n, Wchk.data() + static_cast<int64_t>(j) * n);
        for (int j = 0; j < kinst; ++j)
            std::copy(md.inst(j), md.inst(j) + n, Wchk.data() + static_cast<int64_t>(kexog + j) * n);
        // Forward rank-revealing sweep: earlier columns win, so an instrument
        // collinear with the exog block (or with an earlier instrument) is the one
        // marked. Exog columns come first and therefore are never dropped in favour
        // of an instrument.
        const std::vector<char> keep = collinear_keep_mask(Wchk.data(), n, kw);
        bool exog_deficient = false;
        int n_drop = 0;
        for (int j = 0; j < kexog; ++j)
            if (!keep[static_cast<size_t>(j)]) exog_deficient = true;
        for (int j = kexog; j < kw; ++j)
            if (!keep[static_cast<size_t>(j)]) ++n_drop;
        if (!exog_deficient && n_drop > 0) {
            std::vector<char> keep_inst(keep.begin() + kexog, keep.end());
            // inst_names may be empty when an older/other caller built md without it;
            // fall back to positional labels rather than mis-pairing names.
            std::vector<std::string> inames = md.inst_names;
            if (static_cast<int>(inames.size()) != kinst) {
                inames.clear();
                for (int j = 0; j < kinst; ++j) inames.push_back("z" + std::to_string(j + 1));
            }
            S.omitted = compact_columns(md.Zinst, n, md.kinst, inames, keep_inst);
            md.inst_names = inames;
            kinst = md.kinst;
            kw = kexog + kinst;
            if (kinst < kendog)
                throw EngineError("under_identified",
                    "under-identified: after omitting instrument(s) collinear with the absorbed "
                    "fixed effects, fewer instruments than endogenous regressors remain");
        }
    }
    S.kinst = kinst;
    S.kw = kw;

    // --- W = [exog | instruments] ---
    S.W.resize(static_cast<size_t>(n) * kw);
    double* W = S.W.data();
    for (int j = 0; j < kexog; ++j)
        std::copy(md.exog(j), md.exog(j) + n, W + static_cast<int64_t>(j) * n);
    for (int j = 0; j < kinst; ++j)
        std::copy(md.inst(j), md.inst(j) + n, W + static_cast<int64_t>(kexog + j) * n);

    S.breadW.resize(static_cast<size_t>(kw) * kw);
    double* breadW = S.breadW.data();
    la::xtx(W, n, kw, breadW);
    if (la::spd_inverse(breadW, kw) != 0)
        throw EngineError("collinear", "W'W not positive-definite (collinear instruments?)");

    // --- first stage: Xhat_endog_j = W (W'W)^-1 W' Xendog_j ; keep gamma for F ---
    S.Xhat.resize(static_cast<size_t>(n) * kx);
    double* Xhat = S.Xhat.data();
    for (int j = 0; j < kexog; ++j)  // P_W exog = exog
        std::copy(md.exog(j), md.exog(j) + n, Xhat + static_cast<int64_t>(j) * n);
    S.gammas.assign(kendog, {});
    std::vector<double> Wtv(kw);
    for (int j = 0; j < kendog; ++j) {
        la::xtv(W, n, kw, md.endog(j), Wtv.data());
        S.gammas[j].resize(kw);
        la::matvec(breadW, kw, Wtv.data(), S.gammas[j].data());
        double* xhatj = Xhat + static_cast<int64_t>(kexog + j) * n;
        la::mat_vec(W, n, kw, S.gammas[j].data(), xhatj);
    }

    // --- bread = (Xhat'Xhat)^-1 ---
    S.bread.resize(static_cast<size_t>(kx) * kx);
    double* bread = S.bread.data();
    la::xtx(Xhat, n, kx, bread);
    if (la::spd_inverse(bread, kx) != 0)
        throw EngineError("collinear", "Xhat'Xhat not positive-definite");

    // --- actual regressors X = [exog, endog] (residuals & Hansen use these) ---
    S.X.resize(static_cast<size_t>(n) * kx);
    double* X = S.X.data();
    for (int j = 0; j < kexog; ++j)
        std::copy(md.exog(j), md.exog(j) + n, X + static_cast<int64_t>(j) * n);
    for (int j = 0; j < kendog; ++j)
        std::copy(md.endog(j), md.endog(j) + n, X + static_cast<int64_t>(kexog + j) * n);

    S.dof = compute_dof(n, kx, md.fe);
    if (S.dof.dof_resid <= 0) throw EngineError("no_obs", "non-positive residual dof");
    S.k_full = kx + S.dof.k_fe;
    S.dofminus = md.dofminus_fe ? S.dof.k_fe : 0;

    // --- first-stage F per endogenous regressor (homoskedastic) ---
    // F = gamma_inst' (W'W)_inst,inst^-1 gamma_inst / (kinst * sigma_u^2)
    const int fs_dof = static_cast<int>(n) - kw - S.dof.k_fe;
    for (int j = 0; j < kendog; ++j) {
        std::vector<double> uhat(n);  // first-stage residuals u = Xendog_j - W gamma_j
        la::residual(W, n, kw, S.gammas[j].data(), md.endog(j), uhat.data());
        const double rss = la::sumsq(uhat.data(), n);  // COBRA-DD-ACCUM (T1)
        double su2 = (fs_dof > 0) ? rss / fs_dof : 0.0;
        std::vector<double> block(static_cast<size_t>(kinst) * kinst);  // instrument block of (W'W)^-1
        for (int b = 0; b < kinst; ++b)
            for (int a = 0; a < kinst; ++a)
                block[a + b * kinst] = breadW[(kexog + a) + (kexog + b) * kw];
        if (la::spd_inverse(block.data(), kinst) != 0 || su2 <= 0) {
            S.first_stage_F.push_back(0.0);
            continue;
        }
        const double* gi = S.gammas[j].data() + kexog;  // instrument coefs
        std::vector<double> tmp(kinst);
        la::matvec(block.data(), kinst, gi, tmp.data());
        double q = 0;
        for (int a = 0; a < kinst; ++a) q += gi[a] * tmp[a];
        S.first_stage_F.push_back(q / (kinst * su2));
    }

    // --- shared weak-id (KP rk Wald F, effective F, KP rk LM), robust/cluster ---
    // These depend only on endog/instruments/exog, not on the outcome y. (Hansen J
    // is the only weak-id stat that depends on y, so it is computed per outcome.)
    S.weakid_on = (vt == Vcov::HC1 || vt == Vcov::Cluster) && kinst >= kendog;
    if (S.weakid_on) {
        S.clust = (vt == Vcov::Cluster);
        S.cl = S.clust && !md.cluster.empty() ? &md.cluster[cluster_index] : nullptr;
        S.G = S.cl ? S.cl->n_levels : 0;
        const bool clust = S.clust;
        const Factor* cl = S.cl;
        const int G = S.G;
        // sdofminus = small-sample dof adjustment matching ivreg2/ivreghdfe e(widstat).
        // The constant counts ONCE: when there are no FE it is already in kw (the
        // intercept column of the exog), so the base is 0; when FE are absorbed the
        // intercept is NOT in kw, so the base is 1 and each non-cluster-nested FE
        // adds (levels-1) — giving the absorbed-FE dof (e.g. one G-level FE => G).
        // With FE that count is ivreghdfe's absorb_ct: reghdfe's df_a under this
        // vce's cluster nesting, floored at 1 (as ivreghdfe does, identified from its ado).
        int sdofminus = (md.has_intercept || md.no_constant) ? 0 : 1;
        if (!md.fe.empty()) {
            std::vector<char> nested(md.fe.size(), 0);
            if (cl)
                for (size_t g = 0; g < md.fe.size(); ++g) nested[g] = fe_nested_in_cluster(md.fe[g], *cl);
            sdofminus = std::max(absorbed_dof(md.fe, nested).df_a, 1);
        }

        // KP rk Wald F (1 endogenous: = robust first-stage F of the excluded IVs).
        if (kendog == 1) {
            const double Nd = static_cast<double>(n);
            std::vector<double> u(n);
            la::residual(W, n, kw, S.gammas[0].data(), md.endog(0), u.data());
            std::vector<double> M = iv_robust_meat(W, n, kw, u.data(), cl, G, clust);
            // V_gamma = breadW M breadW
            std::vector<double> tmpM(static_cast<size_t>(kw) * kw, 0.0), Vg(static_cast<size_t>(kw) * kw, 0.0);
            la::sandwich(breadW, M.data(), kw, 1.0, Vg.data(), tmpM.data());
            // robust variance of the excluded-instrument first-stage coefs (kinst x kinst)
            std::vector<double> Vinst(static_cast<size_t>(kinst) * kinst);
            for (int b = 0; b < kinst; ++b)
                for (int a = 0; a < kinst; ++a)
                    Vinst[a + b * kinst] = Vg[(kexog + a) + (kexog + b) * kw];
            const double* gi = S.gammas[0].data() + kexog;

            // Q = Z~'Z~ = (instrument block of (W'W)^-1)^-1  (partialled of exog)
            std::vector<double> Q(static_cast<size_t>(kinst) * kinst);
            for (int b = 0; b < kinst; ++b)
                for (int a = 0; a < kinst; ++a)
                    Q[a + b * kinst] = breadW[(kexog + a) + (kexog + b) * kw];
            bool haveQ = (la::spd_inverse(Q.data(), kinst) == 0);

            // --- Montiel-Olea-Pflueger effective F (weakivtest r(F_eff)) ---
            if (haveQ) {
                std::vector<double> Qgi(kinst);
                la::matvec(Q.data(), kinst, gi, Qgi.data());
                double q = 0;
                for (int a = 0; a < kinst; ++a) q += gi[a] * Qgi[a];  // pi' Q pi
                double trVQ = 0;
                for (int a = 0; a < kinst; ++a)
                    for (int b = 0; b < kinst; ++b) trVQ += Vinst[a + b * kinst] * Q[b + a * kinst];
                double clustdfadj = clust ? (Nd / (Nd - 1.0)) * ((G - 1.0) / G) : 1.0;
                double feff_dof = Nd - kinst - kexog - (S.dof.k_fe > 0 ? 1.0 : 0.0);
                if (trVQ > 0) S.eff_F = clustdfadj * q / trVQ * feff_dof / Nd;
            }

            // --- KP rk Wald F ---
            std::vector<double> Vi(Vinst);
            if (la::spd_inverse(Vi.data(), kinst) == 0) {
                std::vector<double> t2(kinst);
                la::matvec(Vi.data(), kinst, gi, t2.data());
                double chi2 = 0;
                for (int a = 0; a < kinst; ++a) chi2 += gi[a] * t2[a];
                if (clust)
                    S.kp_wald_F = chi2 / (Nd - 1) * (Nd - kw - sdofminus) * (G - 1.0) / G / kinst;
                else
                    S.kp_wald_F = chi2 / Nd * (Nd - kw - sdofminus) / kinst;
            }
        }

        // KP rk Wald F, >=2 endogenous regressors: the ranktest SVD rk statistic
        // (m_svd) of the rank=(kendog-1) test, fed through the same widstat scaling.
        if (kendog >= 2) {
            const double Nd = static_cast<double>(n);
            double rkchi2 = kp_rk_chi2(md.Xexog.data(), kexog, md.Zinst.data(), kinst,
                                       md.Xendog.data(), kendog, n, cl, G, clust,
                                       Nd - kw - sdofminus);
            if (!std::isnan(rkchi2)) {
                if (clust)
                    S.kp_wald_F = rkchi2 / (Nd - 1) * (Nd - kw - sdofminus) * (G - 1.0) / G / kinst;
                else
                    S.kp_wald_F = rkchi2 / Nd * (Nd - kw - sdofminus) / kinst;
            }
        }

        // Kleibergen-Paap rk LM underidentification (kendog==1) = ranktest e(idstat).
        // Partial the included exog out of the endog (y_p) and each excluded
        // instrument (z_p), then idstat = (z_p'y_p)' Sr^-1 (z_p'y_p) ~ chi2(kinst).
        if (kendog == 1) {
            std::vector<double> PtPi;
            if (kexog > 0) {
                PtPi.assign(static_cast<size_t>(kexog) * kexog, 0.0);
                la::xtx(W, n, kexog, PtPi.data());  // exog = first kexog cols of W
                if (la::spd_inverse(PtPi.data(), kexog) != 0) PtPi.clear();
            }
            auto partial = [&](const double* v, std::vector<double>& out) {
                out.assign(v, v + n);
                if (kexog > 0 && !PtPi.empty()) {
                    std::vector<double> Pv(kexog, 0.0);
                    la::xtv(W, n, kexog, v, Pv.data());
                    std::vector<double> bb(kexog, 0.0);
                    la::matvec(PtPi.data(), kexog, Pv.data(), bb.data());
                    for (int64_t i = 0; i < n; ++i) {
                        double f = 0;
                        for (int c = 0; c < kexog; ++c) f += W[static_cast<int64_t>(c) * n + i] * bb[c];
                        out[i] -= f;
                    }
                }
            };
            std::vector<double> yp;
            partial(md.endog(0), yp);
            std::vector<std::vector<double>> zp(kinst);
            for (int j = 0; j < kinst; ++j) partial(W + static_cast<int64_t>(kexog + j) * n, zp[j]);
            std::vector<double> gsum(kinst, 0.0);
            for (int j = 0; j < kinst; ++j) {
                double s = 0;
                for (int64_t i = 0; i < n; ++i) s += zp[j][i] * yp[i];
                gsum[j] = s;
            }
            std::vector<double> Sr(static_cast<size_t>(kinst) * kinst, 0.0);
            if (clust && cl) {
                std::vector<double> U(static_cast<size_t>(G) * kinst, 0.0);
                for (int64_t i = 0; i < n; ++i) {
                    int32_t c = cl->codes[i];
                    double yi = yp[i];
                    for (int j = 0; j < kinst; ++j) U[static_cast<size_t>(c) * kinst + j] += zp[j][i] * yi;
                }
                for (int gg = 0; gg < G; ++gg)
                    for (int a = 0; a < kinst; ++a) {
                        double ua = U[static_cast<size_t>(gg) * kinst + a];
                        for (int b = 0; b < kinst; ++b) Sr[a * kinst + b] += ua * U[static_cast<size_t>(gg) * kinst + b];
                    }
            } else {
                for (int64_t i = 0; i < n; ++i) {
                    double yi = yp[i];
                    for (int a = 0; a < kinst; ++a) {
                        double ga = zp[a][i] * yi;
                        for (int b = 0; b < kinst; ++b) Sr[a * kinst + b] += ga * zp[b][i] * yi;
                    }
                }
            }
            std::vector<double> Sri(Sr);
            if (la::spd_inverse(Sri.data(), kinst) == 0) {
                std::vector<double> t(kinst, 0.0);
                la::matvec(Sri.data(), kinst, gsum.data(), t.data());
                double lm = 0;
                for (int a = 0; a < kinst; ++a) lm += gsum[a] * t[a];
                S.kp_lm = lm;
                S.kp_lm_df = kinst;
                S.kp_lm_p = chi2_pvalue(lm, kinst);
            }
        }
    }
    return S;
}

// Per-outcome solve: md.y must already be demeaned by the FE set; the X-side
// algebra in S is reused. y_dm_* is the demean report for THIS outcome's y.
// `gmm` selects the efficient two-step GMM estimator (Stata `ivregress gmm`):
// step 1 = 2SLS for consistent residuals; step 2 = re-optimize beta under the
// robust efficient weight matrix Omega^-1 (Stata default wmatrix(robust)), with
// V = (X'Z Omega^-1 Z'X)^-1 (robust GMM SEs). Just-identified GMM == 2SLS.
IvResult iv_solve_outcome(const IvShared& S, IvModelData& md, Vcov vt, int cluster_index,
                          int y_dm_iters, double y_dm_resid, bool y_dm_conv, bool gmm,
                          bool small, bool multiway_subset_factors = false) {
    const int64_t n = S.n;
    const int kendog = S.kendog, kinst = S.kinst, kx = S.kx, kw = S.kw;
    const double* W = S.W.data();
    const double* Xhat = S.Xhat.data();
    const double* bread = S.bread.data();
    const double* X = S.X.data();

    IvResult r;
    r.n = n;
    r.k = kx;
    r.n_dropped = S.n_dropped;
    r.names = S.names;
    r.endog_names = S.endog_names;
    r.omitted = S.omitted;  // §35: excluded IVs the FE absorption annihilated
    r.omitted_exog = S.omitted_exog;  // BUG-1225: regressors it annihilated
    r.full_names = S.full_names;
    r.dof = S.dof;
    r.demean_iters = std::max(S.dm_iters, y_dm_iters);
    r.demean_resid = std::max(S.dm_resid, y_dm_resid);
    r.demean_converged = S.dm_conv && y_dm_conv;
    r.first_stage_F = S.first_stage_F;
    r.kp_wald_F = S.kp_wald_F;
    r.eff_F = S.eff_F;
    r.kp_lm = S.kp_lm;
    r.kp_lm_df = S.kp_lm_df;
    r.kp_lm_p = S.kp_lm_p;

    // --- 2SLS coefficients: beta = (Xhat'Xhat)^-1 Xhat'y ---
    std::vector<double> Xhaty(kx);
    la::xtv(Xhat, n, kx, md.y.data(), Xhaty.data());
    r.beta.resize(kx);
    la::matvec(bread, kx, Xhaty.data(), r.beta.data());

    // --- residuals use the ACTUAL regressors X=[exog,endog], not Xhat (key SE point) ---
    std::vector<double> e(n);
    la::residual(X, n, kx, r.beta.data(), md.y.data(), e.data());
    const double ssr = la::sumsq(e.data(), n);  // COBRA-DD-ACCUM (T1)
    r.ssr = ssr;
    // sigma2 (IID branch only): Stata `ivregress` defaults to ASYMPTOTIC (RSS/N);
    // `small` restores the N-K small-sample convention. dof.dof_resid (=N-K, a
    // structural count) is untouched either way.
    // ivreg2 dofminus (xtivreg2's absorbed groups, S.dofminus): the large-sample
    // sigma2 is rss/(N-dofminus) (livreg2 s_iegmm `sigmasq=ee/(N-dofminus)`); the small
    // one, rss/(N-K-dofminus), is dof_resid already (k_fe = dofminus).
    const int dm = S.dofminus;
    r.sigma2 = small ? ssr / static_cast<double>(r.dof.dof_resid)
                     : ssr / static_cast<double>(n - dm);

    // --- vcov: meat from instrumented Xhat, residuals from actual X ---
    // FU-387: ivregress's `small` multiway variance is the built-in convention
    // (per-combination factors, minus = rank); without `small` there is no factor at all,
    // which the undo below reproduces from the Gmin sum (the clip is scale-free).
    // w2s_multiway_class.log:880 mw_iv_c2s_se .0585934519606149, :816 .0503059336995026.
    const bool subset_multiway = multiway_subset_factors && small && !gmm && vt == Vcov::Multiway;
    VcovResult vc = compute_vcov(vt, Xhat, n, kx, e.data(), bread, S.k_full,
                                 r.sigma2, md.fe, md.cluster, cluster_index, -1.0, false,
                                 false, subset_multiway, nullptr, md.multiway_no_psd_fix);
    // compute_vcov's nested-FE K is reghdfe's, rank + df_a + 1. ivreghdfe passes
    // absorb_ct = max(df_a, 1) to ivreg2 as sdofminus and adds nothing for the
    // nesting (as ivreghdfe does, identified from its ado), so its K is one lower whenever df_a > 0.
    if (!vc.nested_fe.empty() && vc.cluster_dof_k > kx + 1) {
        const double adj = static_cast<double>(n - vc.cluster_dof_k) /
                           static_cast<double>(n - vc.cluster_dof_k + 1);
        for (auto& v : vc.V) v *= adj;
        vc.cluster_dof_k -= 1;
    }
    // compute_vcov (shared with OLS/feols) always applies Stata's SMALL-sample
    // HC1/cluster factors. ivregress's default is asymptotic (HC0 / no
    // G/(G-1)*(N-1)/(N-K) factor) — undo the factor here unless --small was
    // requested. Skipped for gmm: its own sandwich VCE below fully overwrites
    // vc.V/r.se regardless, so this would be dead work either way, and gmm/liml
    // must stay untouched.
    if (!small && !gmm) {
        if (vt == Vcov::HC1) {
            double undo = static_cast<double>(n - S.k_full) / static_cast<double>(n);
            for (auto& v : vc.V) v *= undo;
        } else if (vt == Vcov::Cluster || vt == Vcov::Multiway) {
            double undo = ((vc.n_clusters - 1.0) / vc.n_clusters) *
                          (static_cast<double>(n - vc.cluster_dof_k) / static_cast<double>(n - 1));
            for (auto& v : vc.V) v *= undo;
        }
        // dofminus: m_omega's large-sample correction of the robust (not the cluster)
        // meat, N/(N-dofminus); ivreg2's own note "No dofminus adjustment
        // needed for cluster-robust" states the same rule. StataBE xtivreg2 1.0.17 probe
        // (impl_F2/stata_probe): fe robust and fe [pw] reproduce to <=4e-16.
        if (dm > 0 && vt == Vcov::HC1) {
            const double adj = static_cast<double>(n) / static_cast<double>(n - dm);
            for (auto& v : vc.V) v *= adj;
        }
    } else if (!gmm && dm > 0 && vt == Vcov::Cluster) {
        // ivreg2's small cluster factor is (N-1)/(N-K)*M/(M-1) with K the regressors
        // alone (ivreg2's rankxx; xtivreg2 calls it nocons, dofminus never
        // enters it), where compute_vcov's K is the nested-FE-exempt cluster_dof_k.
        const double adj = static_cast<double>(n - vc.cluster_dof_k) / static_cast<double>(n - kx);
        for (auto& v : vc.V) v *= adj;
    }
    r.vcov_type = vc.type;
    r.n_clusters = vc.n_clusters;
    r.cluster_dof_k = vc.cluster_dof_k;
    r.nested_fe = vc.nested_fe;
    r.vcov_npd_fix = vc.npd_fix;
    r.se.resize(kx);
    for (int j = 0; j < kx; ++j) r.se[j] = std::sqrt(vc.V[j + j * kx]);
    r.vcov_matrix = std::move(vc.V);

    // --- Efficient two-step GMM (Stata `ivregress gmm`) ---------------------------
    // Two-step, NOT iterated (Stata default). Step 1 = 2SLS residuals e1 → robust
    // weight matrix W1 = S1^-1, S1 = Σ e1_i^2 z_i z_i' (un-normalized; cluster sums
    // when vt=Cluster). Step 2 estimate: beta = (C'W1 C)^-1 C'W1 d, C=Z'X, d=Z'y.
    // VCE = the GENERAL GMM sandwich (Stata does NOT collapse to (C'W1 C)^-1 because
    // the reported meat is rebuilt from the GMM-final residuals):
    //   V = (C'W1 C)^-1 (C'W1 S2 W1 C) (C'W1 C)^-1,  S2 = Σ e_gmm_i^2 z_i z_i'
    // Verified to match `ivregress gmm` e(b) and e(V) to all printed digits on
    // sysuse auto (mpg/_cons coefs + SEs + cross-cov). Just-identified GMM coincides
    // with 2SLS. (Stata default = wmatrix(robust) vce(robust); no finite-sample q.)
    if (gmm) {
        bool clust = (vt == Vcov::Cluster) && S.cl;
        // S1 from 2SLS residuals e; W1 = S1^-1.
        std::vector<double> W1 = iv_robust_meat(W, n, kw, e.data(), S.cl, S.G, clust);
        if (la::spd_inverse(W1.data(), kw) != 0)
            throw EngineError("collinear", "GMM step-1 weight matrix (Z'Z meat) not positive-definite");
        // C = Z'X (kw x kx, column-major), d = Z'y.
        std::vector<double> C(static_cast<size_t>(kw) * kx);
        for (int p = 0; p < kx; ++p)
            la::xtv(W, n, kw, X + static_cast<int64_t>(p) * n, C.data() + static_cast<int64_t>(p) * kw);
        std::vector<double> dvec(kw);
        la::xtv(W, n, kw, md.y.data(), dvec.data());
        // W1C (kw x kx) and W1d (kw).
        auto applyW1 = [&](const double* v, double* o) { la::matvec(W1.data(), kw, v, o); };
        std::vector<double> W1C(static_cast<size_t>(kw) * kx);
        for (int p = 0; p < kx; ++p)
            applyW1(C.data() + static_cast<int64_t>(p) * kw, W1C.data() + static_cast<int64_t>(p) * kw);
        std::vector<double> W1d(kw); applyW1(dvec.data(), W1d.data());
        // A = C'W1 C (kx x kx), rhs = C'W1 d.
        std::vector<double> A(static_cast<size_t>(kx) * kx, 0.0), rhs(kx, 0.0);
        for (int p = 0; p < kx; ++p) {
            const double* Cp = C.data() + static_cast<int64_t>(p) * kw;
            for (int q = 0; q < kx; ++q) {
                const double* W1Cq = W1C.data() + static_cast<int64_t>(q) * kw;
                double s = 0; for (int aa = 0; aa < kw; ++aa) s += Cp[aa] * W1Cq[aa];
                A[p + q * kx] = s;
            }
            double rp = 0; for (int aa = 0; aa < kw; ++aa) rp += Cp[aa] * W1d[aa];
            rhs[p] = rp;
        }
        std::vector<double> Ainv(A);
        if (la::spd_inverse(Ainv.data(), kx) != 0)
            throw EngineError("collinear", "GMM normal matrix C'W1 C not positive-definite");
        // beta_gmm = (C'W1 C)^-1 C'W1 d.
        la::matvec(Ainv.data(), kx, rhs.data(), r.beta.data());
        // GMM-final residuals → ssr/sigma2 and the VCE meat S2.
        la::residual(X, n, kx, r.beta.data(), md.y.data(), e.data());
        double ssr_g = la::sumsq(e.data(), n);  // COBRA-DD-ACCUM (T1)
        r.ssr = ssr_g;
        r.sigma2 = ssr_g / static_cast<double>(r.dof.dof_resid);
        std::vector<double> S2 = iv_robust_meat(W, n, kw, e.data(), S.cl, S.G, clust);
        // B = C'W1 (kx x kw) = (W1C)' since W1 symmetric → B[p,a] = W1C[a + p*kw].
        // meat_x = B S2 B' (kx x kx). Compute M2 = S2 B' (kw x kx) then B M2.
        std::vector<double> M2(static_cast<size_t>(kw) * kx, 0.0);  // S2 * B'  (col p = S2 @ W1C_p)
        for (int p = 0; p < kx; ++p) {
            const double* W1Cp = W1C.data() + static_cast<int64_t>(p) * kw;
            double* M2p = M2.data() + static_cast<int64_t>(p) * kw;
            la::matvec(S2.data(), kw, W1Cp, M2p);
        }
        std::vector<double> meatx(static_cast<size_t>(kx) * kx, 0.0);  // (W1C)' M2
        for (int p = 0; p < kx; ++p) {
            const double* W1Cp = W1C.data() + static_cast<int64_t>(p) * kw;
            for (int q = 0; q < kx; ++q) {
                const double* M2q = M2.data() + static_cast<int64_t>(q) * kw;
                double s = 0; for (int aa = 0; aa < kw; ++aa) s += W1Cp[aa] * M2q[aa];
                meatx[p + q * kx] = s;
            }
        }
        // V = Ainv * meatx * Ainv (Ainv symmetric).
        std::vector<double> AM(static_cast<size_t>(kx) * kx, 0.0);  // Ainv * meatx
        for (int p = 0; p < kx; ++p)
            for (int q = 0; q < kx; ++q) {
                double s = 0; for (int t = 0; t < kx; ++t) s += Ainv[p + t * kx] * meatx[t + q * kx];
                AM[p + q * kx] = s;
            }
        std::vector<double> V(static_cast<size_t>(kx) * kx, 0.0);
        for (int p = 0; p < kx; ++p)
            for (int q = 0; q < kx; ++q) {
                double s = 0; for (int t = 0; t < kx; ++t) s += AM[p + t * kx] * Ainv[t + q * kx];
                V[p + q * kx] = s;
            }
        r.vcov_matrix = V;
        for (int j = 0; j < kx; ++j) r.se[j] = std::sqrt(V[j + j * kx]);
        r.vcov_type = clust ? "gmm_cluster" : "gmm_robust";
    }

    // --- Hansen J overidentification (kinst > kendog), robust/cluster (ivreg2 e(j)) ---
    // The J is the MINIMIZED efficient-GMM objective, so beta is re-optimized under
    // the efficient weighting matrix Ω^-1 built from the consistent 2SLS residual e.
    if (S.weakid_on && kinst > kendog) {
        std::vector<double> omega = iv_robust_meat(W, n, kw, e.data(), S.cl, S.G, S.clust);
        std::vector<double> Oinv(omega);
        if (la::spd_inverse(Oinv.data(), kw) == 0) {
            std::vector<double> C(static_cast<size_t>(kw) * kx);  // C = W'X
            for (int p = 0; p < kx; ++p)
                la::xtv(W, n, kw, X + static_cast<int64_t>(p) * n, C.data() + static_cast<int64_t>(p) * kw);
            std::vector<double> dvec(kw);
            la::xtv(W, n, kw, md.y.data(), dvec.data());
            std::vector<double> OinvC(static_cast<size_t>(kw) * kx);
            for (int p = 0; p < kx; ++p)
                la::matvec(Oinv.data(), kw, C.data() + static_cast<int64_t>(p) * kw,
                           OinvC.data() + static_cast<int64_t>(p) * kw);
            std::vector<double> Oinvd(kw);
            la::matvec(Oinv.data(), kw, dvec.data(), Oinvd.data());
            std::vector<double> A(static_cast<size_t>(kx) * kx, 0.0);  // A = C' Ω^-1 C
            std::vector<double> rhs(kx, 0.0);
            for (int p = 0; p < kx; ++p) {
                const double* Cp = C.data() + static_cast<int64_t>(p) * kw;
                for (int q = 0; q < kx; ++q) {
                    const double* OCq = OinvC.data() + static_cast<int64_t>(q) * kw;
                    double s = 0;
                    for (int a = 0; a < kw; ++a) s += Cp[a] * OCq[a];
                    A[p + q * kx] = s;
                }
                double rp = 0;
                for (int a = 0; a < kw; ++a) rp += Cp[a] * Oinvd[a];
                rhs[p] = rp;
            }
            if (la::spd_inverse(A.data(), kx) == 0) {
                std::vector<double> bgmm(kx);
                la::matvec(A.data(), kx, rhs.data(), bgmm.data());
                std::vector<double> e2s(n);
                la::residual(X, n, kx, bgmm.data(), md.y.data(), e2s.data());
                std::vector<double> g2(kw);
                la::xtv(W, n, kw, e2s.data(), g2.data());
                std::vector<double> t2(kw);
                la::matvec(Oinv.data(), kw, g2.data(), t2.data());
                double J = 0;
                for (int a = 0; a < kw; ++a) J += g2[a] * t2[a];
                r.hansen_j = J;
                r.hansen_j_df = kinst - kendog;
                r.hansen_j_p = chi2_pvalue(J, r.hansen_j_df);
            }
        }
    }

    // --- Model Wald chi2 + rmse (Stata ivregress e(chi2)/e(df_m)/e(rmse)) -----------
    // chi2 = joint Wald test of all slopes (excludes "(intercept)") against the FINAL
    // beta/vcov (2SLS or, if gmm above ran, the GMM sandwich) — reduces to a plain t^2
    // when there is exactly one slope. rmse is always sqrt(RSS/N): ivregress reports
    // it on the asymptotic (N, not N-K) scale regardless of the small/vcov choice.
    {
        std::vector<int> slope_idx;
        for (int j = 0; j < kx; ++j)
            if (r.names[static_cast<size_t>(j)] != "(intercept)") slope_idx.push_back(j);
        int ks = static_cast<int>(slope_idx.size());
        if (ks > 0 && static_cast<int>(r.vcov_matrix.size()) == kx * kx) {
            std::vector<double> Vs(static_cast<size_t>(ks) * ks), bs(static_cast<size_t>(ks));
            for (int a = 0; a < ks; ++a) {
                bs[static_cast<size_t>(a)] = r.beta[static_cast<size_t>(slope_idx[a])];
                for (int b = 0; b < ks; ++b)
                    Vs[static_cast<size_t>(a) + static_cast<size_t>(b) * ks] =
                        r.vcov_matrix[static_cast<size_t>(slope_idx[a]) +
                                      static_cast<size_t>(slope_idx[b]) * kx];
            }
            if (la::spd_inverse(Vs.data(), ks) == 0) {
                std::vector<double> t(static_cast<size_t>(ks));
                la::matvec(Vs.data(), ks, bs.data(), t.data());
                double c2 = 0;
                for (int a = 0; a < ks; ++a) c2 += bs[static_cast<size_t>(a)] * t[static_cast<size_t>(a)];
                r.chi2 = c2;
                r.chi2_df = ks;
            }
        }
        r.rmse = std::sqrt(r.ssr / static_cast<double>(n));
        // ivreg2's e(rmse) = sqrt(sigmasq): rss/(N-dofminus), or rss/(N-K-dofminus)
        // under small (identified from ivreg2's ado).
        if (S.dofminus > 0 && !gmm)
            r.rmse = std::sqrt(r.ssr / static_cast<double>(small ? r.dof.dof_resid : n - S.dofminus));
    }
    return r;
}

}  // namespace

// ---- ivreg2 e(first): the first-stage summary statistics (s_ffirst) ------------
// ivreg2 4.1.12's s_ffirst (identified from its ado), for each endogenous regressor i:
//  * rmse = sqrt(rss_i/(N-L)); sheapr2 = diag(X'X)^-1 / diag(X'PzX)^-1 at i;
//  * the Wald test of the excluded instruments in the first stage, classical with
//    sigma2 = rss_i/N, giving pr2 = (W/N)/(1+W/N); under robust or cluster the
//    Wald is recomputed from the raw sandwich (Z'Z)^-1 S (Z'Z)^-1 (m_omega has no
//    small-sample factor; the F carries it);
//  * F = W/N*(N-L)/L1 with df_r = N-L, or under cluster W/(N-1)*(N-L)*(G-1)/G/L1
//    with df_r = G-1; pvalue = Ftail(L1, df_r, F);
//  * one endogenous regressor: the Sanderson-Windmeijer and Angrist-Pischke rows
//    repeat F, its Wald (chi2) and pr2; with several, each is the excluded-
//    instrument test in the regression of the partialled residual of X1_i on Z,
//    where AP partials the OTHER fitted endogenous regressors (and the exogenous
//    ones) out of X1_i and SW uses the actual ones with the same coefficients,
//    on L1-K1+1 numerator degrees of freedom.
// dofminus is 0 for the plain ivreg2 fit, and S.dofminus (the absorbed groups) for
// xtivreg2's (s_ffirst with dofminus d, sdofminus 0): the classical sigma2 is rss/(N-d)
// and the robust (not cluster) meat carries N/(N-d), so pr2 = (W/(N-d))/(1+W/(N-d));
// F = W/(N-d)*(N-d-L)/L1 with df_r = N-d-L (cluster: unchanged, df_r = G-1); rmse =
// sqrt(rss/(N-L-d)) (identified from ivreg2's ado). Other absorbed effects (ivreghdfe's
// sdofminus) are not captured, and a multiway m_omega is not implemented here, so
// both leave ffirst empty (the caller then posts no e(first)).
static void iv_fill_ffirst(const IvShared& S, IvModelData& md, Vcov vt, int cluster_index,
                           std::vector<IvFirstStage>& out) {
    if (!md.fe.empty() && S.dofminus == 0) return;
    if (vt != Vcov::IID && vt != Vcov::HC1 && vt != Vcov::Cluster) return;
    const int64_t n = S.n;
    const int kw = S.kw, kexog = S.kexog, L1 = S.kinst, K1 = S.kendog, kx = S.kx;
    if (L1 <= 0 || static_cast<int>(out.size()) != K1) return;
    const double N = static_cast<double>(n), L = static_cast<double>(kw);
    const double d = static_cast<double>(S.dofminus);
    const bool robust = vt != Vcov::IID;
    const bool clust = vt == Vcov::Cluster;
    const Factor* cl = nullptr;
    if (clust) {
        if (cluster_index < 0 || cluster_index >= static_cast<int>(md.cluster.size())) return;
        cl = &md.cluster[static_cast<size_t>(cluster_index)];
    }
    const int G = cl ? cl->n_levels : 0;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double* W = S.W.data();
    const double* breadW = S.breadW.data();

    auto exclBlock = [&](const double* V) {
        std::vector<double> B(static_cast<size_t>(L1) * L1);
        for (int b = 0; b < L1; ++b)
            for (int a = 0; a < L1; ++a)
                B[static_cast<size_t>(a) + static_cast<size_t>(b) * L1] =
                    V[static_cast<size_t>(kexog + a) + static_cast<size_t>(kexog + b) * kw];
        return B;
    };
    auto quad = [&](std::vector<double> B, const double* rb) -> double {
        if (la::spd_inverse(B.data(), L1) != 0) return nan;
        std::vector<double> t(static_cast<size_t>(L1));
        la::matvec(B.data(), L1, rb, t.data());
        double q = 0;
        for (int a = 0; a < L1; ++a) q += rb[a] * t[static_cast<size_t>(a)];
        return q;
    };
    // The Wald statistic of the excluded-instrument coefficients of a regression on
    // W with coefficients c and residual r: classical, and robust/cluster when asked.
    auto waldPair = [&](const std::vector<double>& c, const std::vector<double>& r, double& classical,
                        double& active) {
        const double* rb = c.data() + kexog;
        const double s2 = la::sumsq(r.data(), n) / (N - d);  // COBRA-DD-ACCUM (T1)
        std::vector<double> Vc = exclBlock(breadW);
        for (auto& v : Vc) v *= s2;
        classical = quad(Vc, rb);
        active = classical;
        if (robust) {
            std::vector<double> M = iv_robust_meat(W, n, kw, r.data(), cl, G, clust);
            std::vector<double> tmp(static_cast<size_t>(kw) * kw, 0.0), Vg(static_cast<size_t>(kw) * kw, 0.0);
            la::sandwich(breadW, M.data(), kw, (!clust && d > 0) ? N / (N - d) : 1.0, Vg.data(),
                         tmp.data());
            active = quad(exclBlock(Vg.data()), rb);
        }
    };
    auto fOf = [&](double wald, double df1) {
        return clust ? wald / (N - 1.0) * (N - L) * (G - 1.0) / G / df1
                     : wald / (N - d) * (N - d - L) / df1;
    };
    auto r2Of = [&](double wald) { return (wald / (N - d)) / (1.0 + wald / (N - d)); };
    const double dfr = clust ? G - 1.0 : N - d - L;
    auto ftail = [&](double x, double d1, double d2) { return std::isfinite(x) ? f_pvalue(x, d1, d2) : nan; };
    auto chi2tail = [&](double x, double d) { return std::isfinite(x) ? chi2_pvalue(x, d) : nan; };

    // sheapr2's numerator: diag (X'X)^-1, X = [exog, endog] as fitted.
    std::vector<double> XXinv(static_cast<size_t>(kx) * kx);
    la::xtx(S.X.data(), n, kx, XXinv.data());
    const bool haveXX = la::spd_inverse(XXinv.data(), kx) == 0;

    for (int i = 0; i < K1; ++i) {
        IvFirstStage& f = out[static_cast<size_t>(i)];
        const int col = kexog + i;
        std::vector<double> u(static_cast<size_t>(n));
        la::residual(W, n, kw, S.gammas[static_cast<size_t>(i)].data(), md.endog(i), u.data());
        double wc = nan, wr = nan;
        waldPair(S.gammas[static_cast<size_t>(i)], u, wc, wr);
        const double rmse = (N - L - d > 0) ? std::sqrt(la::sumsq(u.data(), n) / (N - L - d)) : nan;
        const double shea =
            haveXX ? XXinv[static_cast<size_t>(col) * (kx + 1)] / S.bread[static_cast<size_t>(col) * (kx + 1)]
                   : nan;
        const double pr2 = r2Of(wc);
        const double F = fOf(wr, L1);
        const double p = ftail(F, L1, dfr);
        double Fdf1 = L1, swF = F, swChi2 = wr, swR2 = pr2, apF = F, apChi2 = wr, apR2 = pr2;
        if (K1 > 1) {
            Fdf1 = L1 - K1 + 1;
            // Every regressor but X1_i, fitted (AP) and actual (SW).
            const int km = kx - 1;
            std::vector<double> Xh(static_cast<size_t>(n) * km), Xa(static_cast<size_t>(n) * km);
            for (int j = 0, c = 0; j < kx; ++j) {
                if (j == col) continue;
                std::copy(S.Xhat.data() + static_cast<int64_t>(j) * n,
                          S.Xhat.data() + static_cast<int64_t>(j + 1) * n, Xh.data() + static_cast<int64_t>(c) * n);
                std::copy(S.X.data() + static_cast<int64_t>(j) * n, S.X.data() + static_cast<int64_t>(j + 1) * n,
                          Xa.data() + static_cast<int64_t>(c) * n);
                ++c;
            }
            std::vector<double> XhXh(static_cast<size_t>(km) * km), b1(static_cast<size_t>(km)),
                Xhx(static_cast<size_t>(km));
            la::xtx(Xh.data(), n, km, XhXh.data());
            la::xtv(Xh.data(), n, km, md.endog(i), Xhx.data());
            if (la::spd_inverse(XhXh.data(), km) == 0) {
                la::matvec(XhXh.data(), km, Xhx.data(), b1.data());
                auto partialStat = [&](const std::vector<double>& Xs, double& Fo, double& chi2o, double& r2o) {
                    std::vector<double> e1(static_cast<size_t>(n));
                    la::residual(Xs.data(), n, km, b1.data(), md.endog(i), e1.data());
                    std::vector<double> We(static_cast<size_t>(kw)), c2(static_cast<size_t>(kw));
                    la::xtv(W, n, kw, e1.data(), We.data());
                    la::matvec(breadW, kw, We.data(), c2.data());
                    std::vector<double> e2(static_cast<size_t>(n));
                    la::residual(W, n, kw, c2.data(), e1.data(), e2.data());
                    double c = nan, a = nan;
                    waldPair(c2, e2, c, a);
                    r2o = r2Of(c);
                    chi2o = a;
                    Fo = fOf(a, Fdf1);
                };
                partialStat(Xh, apF, apChi2, apR2);
                partialStat(Xa, swF, swChi2, swR2);
            } else {
                swF = swChi2 = swR2 = apF = apChi2 = apR2 = nan;
            }
        }
        f.ffirst = {rmse,
                    shea,
                    pr2,
                    F,
                    static_cast<double>(L1),
                    dfr,
                    p,
                    swF,
                    Fdf1,
                    dfr,
                    ftail(swF, Fdf1, dfr),
                    swChi2,
                    chi2tail(swChi2, Fdf1),
                    swR2,
                    apF,
                    Fdf1,
                    dfr,
                    ftail(apF, Fdf1, dfr),
                    apChi2,
                    chi2tail(apChi2, Fdf1),
                    apR2};
    }
}

// ---- ivreg2 `savefirst`: the stored first-stage estimate, one per endogenous ----
// The design, the coefficients and the bread all already exist in IvShared: W is
// [included exog (constant first when there is one), excluded IVs], breadW = (W'W)^-1
// and gammas[j] IS the first-stage coefficient vector. Only the VCE and the posted
// scalars are new. See IvFirstStage in iv2sls.h for the receipt: this is `regress
// <endog> <exexog> <inexog>, <parent vce>` with the small-sample correction ALWAYS on,
// which is why compute_vcov's factors are used as-is and the `!small` undo that the
// main equation applies (iv_solve_outcome) is deliberately NOT applied here.
static std::vector<IvFirstStage> iv_build_first_stages(const IvShared& S, IvModelData& md,
                                                       Vcov vt, int cluster_index) {
    const int64_t n = S.n;
    const int kw = S.kw, kexog = S.kexog, kinst = S.kinst;
    // iv1_ct + dofminus: every parameter the first stage spends, absorbed FE included.
    //
    // UNPINNED, and deliberately recorded as such: every arm of the captured oracle
    // has dofminus = 0 (plain `ivreg2`, no absorbed FE), so the k_fe term below is
    // the ado's FORMULA applied to a regime no cell measures. The command that
    // occupies it is `ivreghdfe` — a readable ivreg2 fork carrying the same
    // s_ffirst/m_omega machinery (REFUTE.md §12) and the source of 14 of the corpus's
    // 38 `savefirst` lines — not `xtivreg2`, which is absent from this machine and
    // from all 106 corpus packages, so nothing here extrapolates to xtivreg2's naming
    // rule either. Reopen with an `ivreghdfe … absorb() savefirst` capture.
    const int k_full = kw + S.dof.k_fe;
    // ivreg2 posts cnZ1 = exexog1 inexog1 _cons. cobra's W is the other
    // way round (constant first, then included exog, then the excluded IVs), so build
    // the permutation once and apply it to names, beta, se and V together.
    std::vector<int> perm;
    perm.reserve(kw);
    for (int j = 0; j < kinst; ++j) perm.push_back(kexog + j);          // excluded IVs
    for (int j = (md.has_intercept ? 1 : 0); j < kexog; ++j) perm.push_back(j);  // included exog
    if (md.has_intercept) perm.push_back(0);                            // ... constant LAST
    // W's column names: cobra keeps the EXOG names on IvShared but not the
    // instrument names (S.names is exog+ENDOG), and §35 may already have compacted
    // md.inst_names, so read them from md — with the same positional fallback
    // iv_build_shared uses when a caller built md without them.
    std::vector<std::string> wnames = md.exog_names;
    if (static_cast<int>(md.inst_names.size()) == kinst) {
        for (const auto& nm : md.inst_names) wnames.push_back(nm);
    } else {
        for (int j = 0; j < kinst; ++j) wnames.push_back("z" + std::to_string(j + 1));
    }
    std::vector<IvFirstStage> out;
    out.reserve(static_cast<size_t>(S.kendog));
    std::vector<double> u(static_cast<size_t>(n));
    for (int j = 0; j < S.kendog; ++j) {
        IvFirstStage f;
        f.depvar = S.endog_names[static_cast<size_t>(j)];
        f.n = n;
        f.df_r = n - k_full;      // ivreg2 posts this unconditionally on cluster (arm E: 70, not M-1=8)
        f.df_m = kw - (md.has_intercept ? 1 : 0);
        f.k_eq = S.kendog;        // ivreg2 posts the ENDOGENOUS COUNT (arm A: 2 on both eqs)
        la::residual(S.W.data(), n, kw, S.gammas[static_cast<size_t>(j)].data(), md.endog(j),
                     u.data());
        f.rss = la::sumsq(u.data(), n);  // COBRA-DD-ACCUM (T1)
        const double dfr = static_cast<double>(f.df_r);
        const double sigma2 = (dfr > 0) ? f.rss / dfr : 0.0;
        f.rmse = std::sqrt(sigma2);      // ivreg2's rmse
        VcovResult vc = compute_vcov(vt, S.W.data(), n, kw, u.data(), S.breadW.data(), k_full,
                                     sigma2, md.fe, md.cluster, cluster_index);
        // xtivreg2 (dofminus): the stage's cluster factor is (N-1)/(N-iv1_ct)*M/(M-1),
        // iv1_ct the instruments alone (as in ivreg2); compute_vcov's K is the
        // nested-FE-exempt cluster_dof_k. iid and robust already match: their factors
        // (N-dofminus)/(N-iv1_ct-dofminus) on top of m_omega's N/(N-dofminus) are
        // k_full = iv1_ct + dofminus.
        if (S.dofminus > 0 && vt == Vcov::Cluster) {
            const double adj = static_cast<double>(n - vc.cluster_dof_k) / static_cast<double>(n - kw);
            for (auto& v : vc.V) v *= adj;
        }
        f.vcov_type = vc.type;
        f.n_clusters = vc.n_clusters;
        f.names.reserve(static_cast<size_t>(kw));
        f.beta.resize(static_cast<size_t>(kw));
        f.se.resize(static_cast<size_t>(kw));
        f.vcov_matrix.assign(static_cast<size_t>(kw) * kw, 0.0);
        for (int a = 0; a < kw; ++a) {
            const int pa = perm[static_cast<size_t>(a)];
            f.names.push_back(wnames[static_cast<size_t>(pa)]);
            f.beta[static_cast<size_t>(a)] = S.gammas[static_cast<size_t>(j)][static_cast<size_t>(pa)];
            f.se[static_cast<size_t>(a)] = std::sqrt(vc.V[static_cast<size_t>(pa) + static_cast<size_t>(pa) * kw]);
            for (int b = 0; b < kw; ++b)
                f.vcov_matrix[static_cast<size_t>(a) + static_cast<size_t>(b) * kw] =
                    vc.V[static_cast<size_t>(pa) + static_cast<size_t>(perm[static_cast<size_t>(b)]) * kw];
        }
        out.push_back(std::move(f));
    }
    iv_fill_ffirst(S, md, vt, cluster_index, out);
    return out;
}

// [aw=]/[pw=] observation weights, as ivreg2 handles them: pweight is aweight plus
// robust, and every weighted moment carries wf*w with wf = N/sum(w), so the weights
// are normalized to sum to the row count and e(N) stays the row count. Multiplying
// y and every column of X and Z (the constant included) by sqrt(wf*w) turns each
// cross-product the fit forms into that weighted one: b is weighted 2SLS, the iid
// variance's sigma2 is sum(wf*w*e^2)/N, and a robust or cluster score z*e becomes
// wf*w*z*e. With absorbed effects the within transform comes FIRST and is weighted:
// each variable minus its group mean sum(w*v)/sum(w) (as xtivreg2 does, `sum(var*
// wvar)/sum(wvar)` with the raw weight, [pw] included), and only then the sqrt(wf*w)
// scaling; md.fe_absorbed then tells the later demeaning passes it is done.
// sqrt(wf*w) per row, after checking the weights; empty when md carries none.
static std::vector<double> iv_weight_scale(const IvModelData& md) {
    if (md.weights.empty()) return {};
    if (md.weight_kind != WeightKind::Analytic && md.weight_kind != WeightKind::Probability)
        throw EngineError("bad_input", "iv: only [aweight=] and [pweight=] are implemented");
    const int64_t n = md.n;
    if (static_cast<int64_t>(md.weights.size()) != n)
        throw EngineError("internal", "iv: weight vector does not match the estimation sample");
    double sw = 0;
    for (double w : md.weights) sw += w;
    if (!(sw > 0)) throw EngineError("bad_input", "iv: the weights sum to zero");
    const double wf = static_cast<double>(n) / sw;
    std::vector<double> s(static_cast<size_t>(n));
    for (int64_t i = 0; i < n; ++i) s[static_cast<size_t>(i)] = std::sqrt(wf * md.weights[static_cast<size_t>(i)]);
    return s;
}

static void iv_scale_rows(std::vector<double>& M, int k, int64_t n, const std::vector<double>& s) {
    for (int j = 0; j < k; ++j)
        for (int64_t i = 0; i < n; ++i) M[static_cast<size_t>(j) * n + i] *= s[static_cast<size_t>(i)];
}

// The X side (exog, endog, instruments): weighted within transform, then scaling.
static void iv_apply_weights_x(IvModelData& md, const std::vector<double>& s) {
    const int64_t n = md.n;
    if (!md.fe.empty()) {
        const double* wv = md.weights.data();
        DemeanReport b = demean(md.Xexog.data(), n, md.kexog, md.fe, wv);
        DemeanReport c = demean(md.Xendog.data(), n, md.kendog, md.fe, wv);
        DemeanReport e = demean(md.Zinst.data(), n, md.kinst, md.fe, wv);
        if (!(b.converged && c.converged && e.converged))
            throw EngineError("no_convergence", "iv: the weighted within transformation did not converge");
        md.fe_absorbed = true;
    }
    iv_scale_rows(md.Xexog, md.kexog, n, s);
    iv_scale_rows(md.Xendog, md.kendog, n, s);
    iv_scale_rows(md.Zinst, md.kinst, n, s);
}

// One outcome: the same weighted within transform and scaling.
static DemeanReport iv_apply_weights_y(IvModelData& md, const std::vector<double>& s) {
    DemeanReport a;
    if (!md.fe.empty()) {
        a = demean(md.y.data(), md.n, 1, md.fe, md.weights.data());
        if (!a.converged)
            throw EngineError("no_convergence", "iv: the weighted within transformation did not converge");
    }
    iv_scale_rows(md.y, 1, md.n, s);
    return a;
}

static void iv_apply_weights(IvModelData& md) {
    const std::vector<double> s = iv_weight_scale(md);
    if (s.empty()) return;
    iv_apply_weights_y(md, s);
    iv_apply_weights_x(md, s);
}

IvResult iv2sls_fit(IvModelData& md, Vcov vt, int cluster_index, bool gmm, bool small,
                    bool savefirst, bool multiway_subset_factors) {
    iv_apply_weights(md);
    DemeanReport d0 = md.fe_absorbed ? DemeanReport{} : demean(md.y.data(), md.n, 1, md.fe);
    // GMM needs the efficient (robust) weight matrix → force the weak-id/meat plumbing
    // on so S.cl/S.G/S.weakid_on are populated even under a nominally-iid vcov request.
    IvShared S = iv_build_shared(md, gmm ? (vt == Vcov::Cluster ? vt : Vcov::HC1) : vt,
                                 cluster_index);  // demeans the X side
    if (gmm) S.weakid_on = true;
    IvResult r = iv_solve_outcome(S, md, vt, cluster_index, d0.iters, d0.resid, d0.converged,
                                  gmm, small, multiway_subset_factors);
    // The first stage does not involve y, so it is built off the shared X-side algebra
    // and is identical for every outcome in the multi batch below.
    if (savefirst) r.first_stage = iv_build_first_stages(S, md, vt, cluster_index);
    return r;
}


std::vector<IvResult> iv2sls_fit_multi(IvModelData& md, std::vector<std::vector<double>>& ys,
                                       Vcov vt, int cluster_index, bool gmm, bool small,
                                       bool savefirst) {
    // Weighted: the X side is transformed once here, each outcome in the loop.
    const std::vector<double> ws = iv_weight_scale(md);
    if (!ws.empty()) iv_apply_weights_x(md, ws);
    IvShared S = iv_build_shared(md, gmm ? (vt == Vcov::Cluster ? vt : Vcov::HC1) : vt,
                                 cluster_index);  // demean X side + algebra ONCE
    if (gmm) S.weakid_on = true;
    // Built ONCE: the first stage depends only on the X side, which is exactly what
    // this batch shares. Every outcome gets the same object.
    std::vector<IvFirstStage> fs;
    if (savefirst) fs = iv_build_first_stages(S, md, vt, cluster_index);
    std::vector<IvResult> out;
    out.reserve(ys.size());
    for (auto& yraw : ys) {
        md.y = yraw;  // copy (demean mutates in place)
        DemeanReport d0 = ws.empty() ? demean(md.y.data(), md.n, 1, md.fe) : iv_apply_weights_y(md, ws);
        out.push_back(iv_solve_outcome(S, md, vt, cluster_index, d0.iters, d0.resid, d0.converged, gmm, small));
        if (savefirst) out.back().first_stage = fs;
    }
    return out;
}

}  // namespace cecon
