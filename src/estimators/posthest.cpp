// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "estimators/posthest.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <vector>

#include "core/distributions.h"  // betai/t_critical/z_critical: shared with PIET (P6)
#include "core/linalg.h"

namespace cecon {

// Lanczos approximation for log Γ (already in std::lgamma). We need the
// regularized upper incomplete gamma for the χ² p-value:
//   P(X² > x | dof) = Γ(dof/2, x/2) / Γ(dof/2)
// Use a continued-fraction expansion for Q(a, x) (upper-tail regularized).
static double gammq(double a, double x) {
    if (x < 0 || a <= 0) return std::nan("");
    if (x == 0) return 1.0;
    // For x < a + 1, use series expansion of lower-tail and complement.
    if (x < a + 1.0) {
        double ap = a, sum = 1.0 / a, term = sum;
        for (int n = 1; n < 200; ++n) {
            ap += 1.0;
            term *= x / ap;
            sum += term;
            if (std::fabs(term) < std::fabs(sum) * 1e-15) break;
        }
        double gln = std::lgamma(a);
        double lower = sum * std::exp(-x + a * std::log(x) - gln);
        return 1.0 - lower;
    }
    // Continued fraction for upper-tail.
    double b = x + 1.0 - a;
    double c = 1.0 / 1e-300;
    double d = 1.0 / b;
    double h = d;
    for (int i = 1; i < 200; ++i) {
        double an = -static_cast<double>(i) * (static_cast<double>(i) - a);
        b += 2.0;
        d = an * d + b;
        if (std::fabs(d) < 1e-300) d = 1e-300;
        c = b + an / c;
        if (std::fabs(c) < 1e-300) c = 1e-300;
        d = 1.0 / d;
        double del = d * c;
        h *= del;
        if (std::fabs(del - 1.0) < 1e-15) break;
    }
    double gln = std::lgamma(a);
    return std::exp(-x + a * std::log(x) - gln) * h;
}



// P(F_{df1,df2} > f) = I_{df2/(df2+df1 f)}(df2/2, df1/2).
double f_pvalue(double f, double df1, double df2) {
    if (std::isnan(f)) return std::nan("");  // a missing F has a missing p (wald_test's V = 0)
    if (f <= 0 || df1 <= 0 || df2 <= 0) return 1.0;
    double x = df2 / (df2 + df1 * f);
    return betai(df2 / 2.0, df1 / 2.0, x);
}

double chi2_pvalue(double x, double dof) {
    if (std::isnan(x)) return std::nan("");  // a missing chi2 has a missing p (wald_test's V = 0)
    return gammq(dof / 2.0, x / 2.0);
}

// Regularized LOWER incomplete gamma P(a,x); accurate in the left tail (used for
// the lower-tail χ² probability in sdtest, where 1−gammq would cancel).
double gammp(double a, double x) {
    if (x < 0 || a <= 0) return std::nan("");
    if (x == 0) return 0.0;
    if (x < a + 1.0) {
        double ap = a, sum = 1.0 / a, term = sum;
        for (int n = 1; n < 200; ++n) {
            ap += 1.0;
            term *= x / ap;
            sum += term;
            if (std::fabs(term) < std::fabs(sum) * 1e-15) break;
        }
        return sum * std::exp(-x + a * std::log(x) - std::lgamma(a));
    }
    return 1.0 - gammq(a, x);
}

double chi2_cdf(double x, double dof) { return gammp(dof / 2.0, x / 2.0); }




namespace {
// M = R V R'   (J*J, column-major). VR' first: (k*J), then R*(VR').
// Compute VRt[c, r] = sum_j V[c,j] R[r,j]   (V column-major: V[c*k+j... ] —
// V is symmetric so indexing order is fine).
std::vector<double> restriction_variance(const double* V, int k, const double* R, int J) {
    std::vector<double> VRt(static_cast<size_t>(k) * J, 0.0);  // k rows, J cols (col-major)
    for (int r = 0; r < J; ++r)
        for (int c = 0; c < k; ++c) {
            double s = 0;
            for (int j = 0; j < k; ++j) s += V[static_cast<size_t>(j) * k + c] * R[static_cast<size_t>(r) * k + j];
            VRt[static_cast<size_t>(r) * k + c] = s;
        }
    std::vector<double> M(static_cast<size_t>(J) * J, 0.0);  // col-major J*J
    for (int a = 0; a < J; ++a)
        for (int b = 0; b < J; ++b) {
            double s = 0;
            for (int c = 0; c < k; ++c) s += R[static_cast<size_t>(a) * k + c] * VRt[static_cast<size_t>(b) * k + c];
            M[static_cast<size_t>(b) * J + a] = s;
        }
    return M;
}
}  // namespace

WaldResult wald_test(const double* beta, const double* V, int k,
                     const double* R, const double* q, int J) {
    WaldResult w;
    w.J = J;
    if (J <= 0 || J > k) { w.ok = false; w.error = "invalid restriction count"; return w; }
    // d = R beta - q   (length J)
    std::vector<double> d(static_cast<size_t>(J), 0.0);
    for (int r = 0; r < J; ++r) {
        double s = 0;
        for (int c = 0; c < k; ++c) s += R[static_cast<size_t>(r) * k + c] * beta[c];
        d[static_cast<size_t>(r)] = s - (q ? q[r] : 0.0);
    }
    const std::vector<double> M = restriction_variance(V, k, R, J);
    // R V R' exactly zero: the tested combinations have no sampling variance (an
    // exact `regress` fit posts V = 0). Stata's `test` answers F, chi2 and p
    // missing at rc 0 there (StataBE: `regress yx x` with yx = 3 + 2x, then
    // `test x`: F(1, 48) = ., Prob > F = ., r(F) = r(p) = .), not an error.
    bool m_zero = true;
    for (double v : M)
        if (v != 0.0) { m_zero = false; break; }
    if (m_zero) {
        w.chi2 = std::nan("");
        w.F = std::nan("");
        return w;
    }
    // Invert M with a PSD-projection fallback (mirrors hausman_test).
    std::vector<double> Minv(M);
    if (la::spd_inverse(Minv.data(), J) != 0) {
        std::vector<double> Mp(M);
        la::psd_clip(Mp.data(), J);
        Minv = Mp;
        if (la::spd_inverse(Minv.data(), J) != 0) {
            w.ok = false; w.error = "R V R' not invertible (collinear restrictions?)";
            return w;
        }
    }
    std::vector<double> tmp(static_cast<size_t>(J), 0.0);
    la::matvec(Minv.data(), J, d.data(), tmp.data());
    double chi2 = 0;
    for (int r = 0; r < J; ++r) chi2 += d[static_cast<size_t>(r)] * tmp[static_cast<size_t>(r)];
    w.chi2 = chi2;
    w.F = chi2 / J;
    return w;
}

// StataBE 19.5 (2026-11-03), `test` after regress and after `ereturn post b V`:
//   (x1=0) (2*x1=0) drops 1, (2*x1=0) (x1=0) drops 2, (x1=0) (x1=0) drops 2: the
//     row with the larger variance stays, the earlier on a tie;
//   V = diag(1,4,9), (a) (bb) (a+2*bb) drops 2 (a+2*bb first, then a: 16/17 of its
//     variance remains against bb's 4/17); V = I(3), (a) (bb) (2*a+bb) drops 1;
//   V = I(3), (a) (a+s*bb) drops 1 while s^2 < 1e-9 and keeps both above it, and
//     (4*a) (a+s*bb) keeps both at s^2 = 1e-9: the floor is relative to the row's
//     own variance, not to the pivot's or the largest;
//   V_aa = 9.9e-20 drops `test a` and 1e-19 keeps it, whatever the other entries,
//     while (1e10*a) with V_aa = 6e-20 is kept: an absolute floor on R V R';
//   regress y x i.c: (1.c = 1) (x = 0) drops 1 (the base level, a zero row).
// Where two open rows' remaining variances agree mathematically -- (x1=0) (x2=0)
// (x1+x2=0), once x1+x2 is kept -- Stata's choice follows its own rounding (its
// invsym(R*V*R') zeroes x2 there, its test drops x1); this keeps the earlier one
// on an exact tie and otherwise goes by this arithmetic's last digits. Which
// constraint goes can differ there; the statistic and its df cannot.
std::vector<int> wald_dropped_rows(const double* V, int k, const double* R, int J) {
    std::vector<double> M = restriction_variance(V, k, R, J);
    std::vector<double> own(static_cast<size_t>(J));
    for (int i = 0; i < J; ++i) own[static_cast<size_t>(i)] = M[static_cast<size_t>(i) * J + i];
    enum : char { kOpen, kKept, kDropped };
    std::vector<char> state(static_cast<size_t>(J), kOpen);
    for (;;) {
        int p = -1;
        for (int i = 0; i < J; ++i) {
            if (state[static_cast<size_t>(i)] != kOpen) continue;
            const double v = M[static_cast<size_t>(i) * J + i];
            if (v < 1e-19 || v < 1e-9 * own[static_cast<size_t>(i)]) {
                state[static_cast<size_t>(i)] = kDropped;
                continue;
            }
            if (p < 0 || v > M[static_cast<size_t>(p) * J + p]) p = i;
        }
        if (p < 0) break;
        state[static_cast<size_t>(p)] = kKept;
        // Take row p's part out of every open row (one elimination step on M).
        const double mpp = M[static_cast<size_t>(p) * J + p];
        for (int i = 0; i < J; ++i) {
            if (state[static_cast<size_t>(i)] != kOpen) continue;
            const double mip = M[static_cast<size_t>(p) * J + i];
            for (int j = 0; j < J; ++j) {
                if (state[static_cast<size_t>(j)] != kOpen) continue;
                M[static_cast<size_t>(j) * J + i] -= mip * M[static_cast<size_t>(j) * J + p] / mpp;
            }
        }
    }
    std::vector<int> dropped;
    for (int i = 0; i < J; ++i)
        if (state[static_cast<size_t>(i)] == kDropped) dropped.push_back(i);
    return dropped;
}

HausmanResult hausman_test(const double* b_FE, const double* V_FE,
                           const double* b_RE, const double* V_RE,
                           int k) {
    HausmanResult R;
    R.dof = k;
    std::vector<double> db(static_cast<size_t>(k));
    for (int c = 0; c < k; ++c) db[static_cast<size_t>(c)] = b_FE[c] - b_RE[c];

    // ΔV = V_FE - V_RE  (k*k column-major)
    std::vector<double> dV(static_cast<size_t>(k) * k);
    for (int c = 0; c < k; ++c)
        for (int r = 0; r < k; ++r)
            dV[static_cast<size_t>(c) * k + r] = V_FE[c * k + r] - V_RE[c * k + r];

    R.se_diff.assign(static_cast<size_t>(k), std::numeric_limits<double>::quiet_NaN());
    for (int c = 0; c < k; ++c) {
        double d = dV[static_cast<size_t>(c) * k + c];
        if (d >= 0) R.se_diff[static_cast<size_t>(c)] = std::sqrt(d);
    }

    // Stata's rule (identified from its documentation and output): the
    // difference matrix is inverted with a generalized inverse whether or
    // not it is positive definite; the statistic may then be negative, the
    // output carries the note "(V_b-V_B is not positive definite)", and
    // r(df) is the rank of the difference. BUG-1940: the previous fallback
    // (project onto the PSD cone, then Cholesky) answered a near-singular
    // clipped matrix with chi2 ~ 1e16, or refused outright, where Stata
    // reports 1.15 with the note.
    std::vector<double> dV_inv(dV);
    int info = la::spd_inverse(dV_inv.data(), k);   // positive definite: plain inverse, as before
    int rank = k;
    if (info != 0) {
        std::vector<double> Q(dV);
        std::vector<double> w(static_cast<size_t>(k));
        if (la::sym_eig(Q.data(), k, w.data()) != 0) {
            R.ok = false;
            R.error = "V_FE - V_RE: symmetric eigendecomposition failed";
            return R;
        }
        double wmax = 0;
        for (int c = 0; c < k; ++c) wmax = std::max(wmax, std::fabs(w[static_cast<size_t>(c)]));
        const double tol = wmax * 1e-13;
        rank = 0;
        R.not_pd = true;
        // dV^- = Q diag(1/w_j for |w_j| > tol, else 0) Q'
        std::fill(dV_inv.begin(), dV_inv.end(), 0.0);
        for (int j = 0; j < k; ++j) {
            double wj = w[static_cast<size_t>(j)];
            if (std::fabs(wj) <= tol) continue;
            ++rank;
            const double* q = Q.data() + static_cast<size_t>(j) * k;
            for (int c = 0; c < k; ++c)
                for (int r = 0; r < k; ++r)
                    dV_inv[static_cast<size_t>(c) * k + r] += q[r] * q[c] / wj;
        }
        if (rank == 0) {
            R.ok = false;
            R.error = "V_FE - V_RE is the zero matrix; no test is defined";
            return R;
        }
    }

    // chi2 = db' · dV^- · db
    std::vector<double> tmp(static_cast<size_t>(k));
    la::matvec(dV_inv.data(), k, db.data(), tmp.data());
    double chi2 = 0;
    for (int c = 0; c < k; ++c) chi2 += db[static_cast<size_t>(c)] * tmp[static_cast<size_t>(c)];
    R.chi2 = chi2;
    R.dof = rank;
    R.pvalue = chi2 >= 0 ? gammq(static_cast<double>(rank) / 2.0, chi2 / 2.0)
                         : std::numeric_limits<double>::quiet_NaN();
    return R;
}

void predict_xb(const double* X, int64_t n, int k,
                const double* beta, const double* offset,
                double* out) {
    for (int64_t i = 0; i < n; ++i) {
        double s = offset ? offset[i] : 0.0;
        for (int c = 0; c < k; ++c) s += X[static_cast<int64_t>(c) * n + i] * beta[c];
        out[i] = s;
    }
}

MarginsResult margins_poisson(const double* X, int64_t n, int k,
                              const double* beta, const double* V,
                              const double* offset) {
    MarginsResult R;
    R.dydx.assign(static_cast<size_t>(k), 0.0);
    R.se.assign(static_cast<size_t>(k), 0.0);

    // Compute μ_i = exp(X_i β + off_i), accumulate mean and Σ X_i,d μ_i.
    double mu_mean = 0;
    std::vector<double> sum_Xmu(static_cast<size_t>(k), 0.0);  // (1/n) Σ X_i,d μ_i  (per-d)
    for (int64_t i = 0; i < n; ++i) {
        double xb = offset ? offset[i] : 0.0;
        for (int c = 0; c < k; ++c) xb += X[static_cast<int64_t>(c) * n + i] * beta[c];
        double mu = std::exp(xb);
        mu_mean += mu;
        for (int d = 0; d < k; ++d)
            sum_Xmu[static_cast<size_t>(d)] += mu * X[static_cast<int64_t>(d) * n + i];
    }
    mu_mean /= static_cast<double>(n);
    for (auto& v : sum_Xmu) v /= static_cast<double>(n);

    // AME_c = β_c · mu_mean.
    for (int c = 0; c < k; ++c) R.dydx[static_cast<size_t>(c)] = beta[c] * mu_mean;

    // Delta-method: J[c, d] = β_c · sum_Xmu[d] + δ_{cd} · mu_mean.
    // Var(AME_c) = Σ_{d, e} J[c,d] V[d,e] J[c,e].
    std::vector<double> Jrow(static_cast<size_t>(k));
    for (int c = 0; c < k; ++c) {
        for (int d = 0; d < k; ++d)
            Jrow[static_cast<size_t>(d)] = beta[c] * sum_Xmu[static_cast<size_t>(d)]
                                          + (c == d ? mu_mean : 0.0);
        double s = 0;
        for (int d = 0; d < k; ++d)
            for (int e = 0; e < k; ++e)
                s += Jrow[static_cast<size_t>(d)] * V[e * k + d] * Jrow[static_cast<size_t>(e)];
        R.se[static_cast<size_t>(c)] = std::sqrt(std::max(0.0, s));
    }
    return R;
}

MarginsResult margins_logit(const double* X, int64_t n, int k,
                            const double* beta, const double* V) {
    MarginsResult R;
    R.dydx.assign(static_cast<size_t>(k), 0.0);
    R.se.assign(static_cast<size_t>(k), 0.0);

    // p_i = 1/(1+e^{-xb_i}); w_i = p_i(1-p_i).
    // AME_c = β_c · (1/n) Σ w_i.
    // ∂AME_c / ∂β_d = (1/n) Σ [ β_c · w_i · (1 - 2 p_i) · X_i,d + δ_{cd} · w_i ]
    double w_mean = 0;
    std::vector<double> sum_Xw_p(static_cast<size_t>(k), 0.0);  // (1/n) Σ X_i,d · w_i · (1-2 p_i)
    for (int64_t i = 0; i < n; ++i) {
        double xb = 0;
        for (int c = 0; c < k; ++c) xb += X[static_cast<int64_t>(c) * n + i] * beta[c];
        double p = 1.0 / (1.0 + std::exp(-xb));
        double w = p * (1.0 - p);
        w_mean += w;
        double factor = w * (1.0 - 2.0 * p);
        for (int d = 0; d < k; ++d)
            sum_Xw_p[static_cast<size_t>(d)] += factor * X[static_cast<int64_t>(d) * n + i];
    }
    w_mean /= static_cast<double>(n);
    for (auto& v : sum_Xw_p) v /= static_cast<double>(n);

    for (int c = 0; c < k; ++c) R.dydx[static_cast<size_t>(c)] = beta[c] * w_mean;

    std::vector<double> Jrow(static_cast<size_t>(k));
    for (int c = 0; c < k; ++c) {
        for (int d = 0; d < k; ++d)
            Jrow[static_cast<size_t>(d)] = beta[c] * sum_Xw_p[static_cast<size_t>(d)]
                                          + (c == d ? w_mean : 0.0);
        double s = 0;
        for (int d = 0; d < k; ++d)
            for (int e = 0; e < k; ++e)
                s += Jrow[static_cast<size_t>(d)] * V[e * k + d] * Jrow[static_cast<size_t>(e)];
        R.se[static_cast<size_t>(c)] = std::sqrt(std::max(0.0, s));
    }
    return R;
}

MarginsResult margins_probit(const double* X, int64_t n, int k,
                             const double* beta, const double* V) {
    MarginsResult R;
    R.dydx.assign(static_cast<size_t>(k), 0.0);
    R.se.assign(static_cast<size_t>(k), 0.0);
    const double INV_SQRT_2PI = 0.3989422804014327;
    // φ_i = φ(xb_i); AME_c = β_c · mean(φ).
    // ∂φ/∂η = -η φ, so ∂AME_c/∂β_d = (1/n) Σ [ δ_{cd} φ_i - β_c φ_i η_i X_i,d ].
    double phi_mean = 0;
    std::vector<double> sum_Xphi_eta(static_cast<size_t>(k), 0.0);
    for (int64_t i = 0; i < n; ++i) {
        double xb = 0;
        for (int c = 0; c < k; ++c) xb += X[static_cast<int64_t>(c) * n + i] * beta[c];
        double phi = INV_SQRT_2PI * std::exp(-0.5 * xb * xb);
        phi_mean += phi;
        double f = phi * xb;
        for (int d = 0; d < k; ++d)
            sum_Xphi_eta[static_cast<size_t>(d)] += f * X[static_cast<int64_t>(d) * n + i];
    }
    phi_mean /= static_cast<double>(n);
    for (auto& v : sum_Xphi_eta) v /= static_cast<double>(n);

    for (int c = 0; c < k; ++c) R.dydx[static_cast<size_t>(c)] = beta[c] * phi_mean;

    std::vector<double> Jrow(static_cast<size_t>(k));
    for (int c = 0; c < k; ++c) {
        for (int d = 0; d < k; ++d)
            Jrow[static_cast<size_t>(d)] = (c == d ? phi_mean : 0.0)
                                          - beta[c] * sum_Xphi_eta[static_cast<size_t>(d)];
        double s = 0;
        for (int d = 0; d < k; ++d)
            for (int e = 0; e < k; ++e)
                s += Jrow[static_cast<size_t>(d)] * V[e * k + d] * Jrow[static_cast<size_t>(e)];
        R.se[static_cast<size_t>(c)] = std::sqrt(std::max(0.0, s));
    }
    return R;
}

namespace {

// One `#`-separated atom of a design-column name: either a factor level
// ("level.var", BUG-101) or a continuous main effect ("var").
struct ColComp {
    bool factor = false;
    std::string level;   // only meaningful when factor
    std::string var;
    std::string raw;     // original piece text (== level+"."+var for factor)
};

// Split "a#b#c" into its `#`-separated pieces and classify each as a factor
// level ("N.var", N a signed-integer head — fmt_level's common case) or a plain
// continuous name. Reads the BUG-101 naming convention directly off the string
// (src/core/model.cpp fvterm_columns) rather than re-deriving the design.
std::vector<ColComp> parse_col_components(const std::string& colname) {
    std::vector<ColComp> out;
    size_t p = 0;
    while (true) {
        size_t q = colname.find('#', p);
        std::string piece = (q == std::string::npos) ? colname.substr(p) : colname.substr(p, q - p);
        ColComp c;
        c.raw = piece;
        size_t dot = piece.find('.');
        bool is_level = false;
        if (dot != std::string::npos && dot > 0 && dot + 1 < piece.size()) {
            size_t i = (piece[0] == '-') ? 1 : 0;
            bool all_digit = i < dot;
            for (size_t j = i; j < dot && all_digit; ++j)
                if (!std::isdigit(static_cast<unsigned char>(piece[j]))) all_digit = false;
            is_level = all_digit;
        }
        if (is_level) { c.factor = true; c.level = piece.substr(0, dot); c.var = piece.substr(dot + 1); }
        else { c.factor = false; c.var = piece; }
        out.push_back(std::move(c));
        if (q == std::string::npos) break;
        p = q + 1;
    }
    return out;
}

// Product of every OTHER component of column `col`'s value — the chain-rule
// "everything but this one piece" multiplier, evaluated per row. Tries an exact
// design-column name match first (present whenever the model was built with
// `##`, which always includes every lower-order term); falls back to
// multiplying each remaining atom's OWN standalone column when found. Leaves
// *ok=false (with *err set) rather than a silently-wrong guess if neither
// resolves.
std::vector<double> remaining_column(const double* X, int64_t n,
                                     const std::vector<std::vector<ColComp>>& comps_by_col,
                                     const std::unordered_map<std::string, int>& name_idx,
                                     int col, size_t skip_piece, bool* ok, std::string* err) {
    const auto& comps = comps_by_col[static_cast<size_t>(col)];
    std::vector<std::string> remaining_raw;
    for (size_t p = 0; p < comps.size(); ++p) if (p != skip_piece) remaining_raw.push_back(comps[p].raw);
    if (remaining_raw.empty()) return std::vector<double>(static_cast<size_t>(n), 1.0);
    std::string joined;
    for (size_t i = 0; i < remaining_raw.size(); ++i) { if (i) joined += "#"; joined += remaining_raw[i]; }
    auto it = name_idx.find(joined);
    if (it != name_idx.end()) {
        const double* p = X + static_cast<int64_t>(it->second) * n;
        return std::vector<double>(p, p + n);
    }
    std::vector<double> result(static_cast<size_t>(n), 1.0);
    for (const auto& raw : remaining_raw) {
        auto it2 = name_idx.find(raw);
        if (it2 == name_idx.end()) {
            *ok = false;
            *err = "margins: cannot resolve interaction component '" + raw + "' while "
                   "differentiating — the model must include every lower-order term (fit "
                   "with `##`, not a bare `#`, so the AME's chain rule has a column to read)";
            return {};
        }
        const double* p = X + static_cast<int64_t>(it2->second) * n;
        for (int64_t i = 0; i < n; ++i) result[static_cast<size_t>(i)] *= p[i];
    }
    return result;
}

inline double glm_g(GlmLink link, double eta) {
    switch (link) {
        case GlmLink::Logit: return 1.0 / (1.0 + std::exp(-eta));
        case GlmLink::Probit: return 0.5 * std::erfc(-eta * 0.7071067811865476);
        case GlmLink::Poisson: return std::exp(eta);
        case GlmLink::Cloglog: return -std::expm1(-std::exp(eta));
    }
    return 0.0;
}
inline double glm_gprime(GlmLink link, double eta, double g) {
    switch (link) {
        case GlmLink::Logit: return g * (1.0 - g);
        case GlmLink::Probit: return 0.3989422804014327 * std::exp(-0.5 * eta * eta);
        case GlmLink::Poisson: return g;  // d/deta exp(eta) == exp(eta)
        case GlmLink::Cloglog: return std::exp(eta - std::exp(eta));
    }
    return 0.0;
}
inline double glm_gsecond(GlmLink link, double eta, double g, double gp) {
    switch (link) {
        case GlmLink::Logit: return gp * (1.0 - 2.0 * g);
        case GlmLink::Probit: return -eta * gp;
        case GlmLink::Poisson: return gp;
        case GlmLink::Cloglog: return gp * (1.0 - std::exp(eta));
    }
    return 0.0;
}

}  // namespace

AmeResult margins_ame(GlmLink link, const double* X, int64_t n, int k,
                      const std::vector<std::string>& names,
                      const double* beta, const double* V,
                      const double* weights,
                      const std::vector<std::string>& request_vars,
                      const double* offset) {
    AmeResult R;
    std::unordered_map<std::string, int> name_idx;
    for (int j = 0; j < k; ++j) name_idx[names[static_cast<size_t>(j)]] = j;
    std::vector<std::vector<ColComp>> comps_by_col(static_cast<size_t>(k));
    for (int j = 0; j < k; ++j) {
        if (names[static_cast<size_t>(j)] == "(intercept)") continue;
        comps_by_col[static_cast<size_t>(j)] = parse_col_components(names[static_cast<size_t>(j)]);
    }

    std::vector<double> eta(static_cast<size_t>(n));
    for (int64_t i = 0; i < n; ++i) {
        double s = offset ? offset[i] : 0.0;
        for (int c = 0; c < k; ++c) s += X[static_cast<int64_t>(c) * n + i] * beta[c];
        eta[static_cast<size_t>(i)] = s;
    }
    double wsum_total = 0;
    for (int64_t i = 0; i < n; ++i) wsum_total += weights ? weights[i] : 1.0;

    auto weighted_mean = [&](const std::vector<double>& v) {
        double vsum = 0;
        for (int64_t i = 0; i < n; ++i) vsum += (weights ? weights[i] : 1.0) * v[static_cast<size_t>(i)];
        return (wsum_total > 0) ? vsum / wsum_total : 0.0;
    };
    auto se_from_jacobian = [&](const std::vector<double>& J) {
        double s = 0;
        for (int c = 0; c < k; ++c)
            for (int d = 0; d < k; ++d)
                s += J[static_cast<size_t>(c)] * V[d * k + c] * J[static_cast<size_t>(d)];
        return std::sqrt(std::max(0.0, s));
    };

    // Which base variables to report, in first-appearance order. Empty
    // request_vars = "auto": every non-interaction, non-intercept column's own
    // var name — the same SET the old per-column functions covered, so a caller
    // that never asks for interactions/factors sees the identical row set.
    std::vector<std::string> vars = request_vars;
    if (vars.empty()) {
        for (int j = 0; j < k; ++j) {
            if (names[static_cast<size_t>(j)] == "(intercept)") continue;
            if (comps_by_col[static_cast<size_t>(j)].size() != 1) continue;  // skip raw interaction terms
            const std::string& v = comps_by_col[static_cast<size_t>(j)][0].var;
            if (std::find(vars.begin(), vars.end(), v) == vars.end()) vars.push_back(v);
        }
    }

    for (const auto& reqvar : vars) {
        // Classify: does `reqvar` appear as a FACTOR atom in one of its OWN
        // single-piece main-effect columns ("L.reqvar" — the levels Stata
        // actually estimated) -> discrete change (one row per non-base level);
        // else does it appear as a continuous atom anywhere -> chain-rule slope.
        std::vector<std::pair<std::string, int>> level_cols;  // (level, col) for single-piece "L.reqvar"
        bool any_continuous = false;
        for (int j = 0; j < k; ++j) {
            const auto& comps = comps_by_col[static_cast<size_t>(j)];
            if (comps.size() == 1 && comps[0].factor && comps[0].var == reqvar)
                level_cols.emplace_back(comps[0].level, j);
            if (comps.size() == 1 && !comps[0].factor && comps[0].var == reqvar)
                any_continuous = true;
        }
        if (level_cols.empty() && !any_continuous) {
            R.ok = false;
            R.error = "margins: unknown dydx variable '" + reqvar + "' (not a fitted covariate)";
            return R;
        }

        if (!level_cols.empty()) {
            // ---- Discrete change (factor variable) ------------------------------
            // all_R_cols: every column (main effect OR interaction) mentioning
            // `reqvar` at ANY level — subtracted out of eta to get the base-level
            // (all-indicators-zero) linear predictor eta_lo.
            std::vector<int> all_R_cols;
            for (int j = 0; j < k; ++j)
                for (const auto& p : comps_by_col[static_cast<size_t>(j)])
                    if (p.factor && p.var == reqvar) { all_R_cols.push_back(j); break; }

            std::vector<double> excl(eta);
            for (int64_t i = 0; i < n; ++i)
                for (int j : all_R_cols)
                    excl[static_cast<size_t>(i)] -= beta[j] * X[static_cast<int64_t>(j) * n + i];

            std::vector<char> is_all_R(static_cast<size_t>(k), 0);
            for (int j : all_R_cols) is_all_R[static_cast<size_t>(j)] = 1;

            for (const auto& lvl_col : level_cols) {
                const std::string& L = lvl_col.first;
                std::vector<std::pair<int, size_t>> hit;  // (col, piece idx of the "L.reqvar" atom)
                for (int j : all_R_cols) {
                    const auto& comps = comps_by_col[static_cast<size_t>(j)];
                    for (size_t p = 0; p < comps.size(); ++p)
                        if (comps[p].factor && comps[p].var == reqvar && comps[p].level == L) { hit.emplace_back(j, p); break; }
                }
                std::vector<std::vector<double>> remaining(hit.size());
                for (size_t h = 0; h < hit.size(); ++h) {
                    bool ok = true; std::string err;
                    remaining[h] = remaining_column(X, n, comps_by_col, name_idx, hit[h].first, hit[h].second, &ok, &err);
                    if (!ok) { R.ok = false; R.error = err; return R; }
                }
                std::vector<double> eta_hi(excl);
                for (size_t h = 0; h < hit.size(); ++h) {
                    int j = hit[h].first;
                    for (int64_t i = 0; i < n; ++i) eta_hi[static_cast<size_t>(i)] += beta[j] * remaining[h][static_cast<size_t>(i)];
                }
                const std::vector<double>& eta_lo = excl;
                std::vector<double> diff(static_cast<size_t>(n)), gp_hi(static_cast<size_t>(n)), gp_lo(static_cast<size_t>(n));
                for (int64_t i = 0; i < n; ++i) {
                    double g_hi = glm_g(link, eta_hi[static_cast<size_t>(i)]);
                    double g_lo = glm_g(link, eta_lo[static_cast<size_t>(i)]);
                    diff[static_cast<size_t>(i)] = g_hi - g_lo;
                    gp_hi[static_cast<size_t>(i)] = glm_gprime(link, eta_hi[static_cast<size_t>(i)], g_hi);
                    gp_lo[static_cast<size_t>(i)] = glm_gprime(link, eta_lo[static_cast<size_t>(i)], g_lo);
                }
                double ame = weighted_mean(diff);

                std::vector<char> is_hit(static_cast<size_t>(k), 0);
                for (auto& h : hit) is_hit[static_cast<size_t>(h.first)] = 1;
                std::vector<double> J(static_cast<size_t>(k), 0.0);
                for (int d = 0; d < k; ++d) {
                    if (!is_all_R[static_cast<size_t>(d)]) {
                        std::vector<double> term(static_cast<size_t>(n));
                        for (int64_t i = 0; i < n; ++i)
                            term[static_cast<size_t>(i)] = (gp_hi[static_cast<size_t>(i)] - gp_lo[static_cast<size_t>(i)]) *
                                                          X[static_cast<int64_t>(d) * n + i];
                        J[static_cast<size_t>(d)] = weighted_mean(term);
                    } else if (is_hit[static_cast<size_t>(d)]) {
                        size_t h = 0; while (hit[h].first != d) ++h;
                        std::vector<double> term(static_cast<size_t>(n));
                        for (int64_t i = 0; i < n; ++i)
                            term[static_cast<size_t>(i)] = gp_hi[static_cast<size_t>(i)] * remaining[h][static_cast<size_t>(i)];
                        J[static_cast<size_t>(d)] = weighted_mean(term);
                    }
                    // else: d mentions reqvar at a DIFFERENT level -> X_hi[d]=X_lo[d]=0, J[d]=0.
                }
                R.rows.push_back({L + "." + reqvar, ame, se_from_jacobian(J), J});
            }
        } else {
            // ---- Chain-rule slope (continuous variable) --------------------------
            std::vector<std::pair<int, size_t>> hit;  // (col, piece idx of the reqvar atom)
            for (int j = 0; j < k; ++j) {
                const auto& comps = comps_by_col[static_cast<size_t>(j)];
                for (size_t p = 0; p < comps.size(); ++p)
                    if (!comps[p].factor && comps[p].var == reqvar) { hit.emplace_back(j, p); break; }
            }
            std::vector<std::vector<double>> remaining(hit.size());
            for (size_t h = 0; h < hit.size(); ++h) {
                bool ok = true; std::string err;
                remaining[h] = remaining_column(X, n, comps_by_col, name_idx, hit[h].first, hit[h].second, &ok, &err);
                if (!ok) { R.ok = false; R.error = err; return R; }
            }
            std::vector<double> D(static_cast<size_t>(n), 0.0);  // d(eta_i)/d(reqvar)
            for (size_t h = 0; h < hit.size(); ++h) {
                int j = hit[h].first;
                for (int64_t i = 0; i < n; ++i) D[static_cast<size_t>(i)] += beta[j] * remaining[h][static_cast<size_t>(i)];
            }
            std::vector<double> slope(static_cast<size_t>(n)), gprime_all(static_cast<size_t>(n)), common(static_cast<size_t>(n));
            for (int64_t i = 0; i < n; ++i) {
                double g = glm_g(link, eta[static_cast<size_t>(i)]);
                double gp = glm_gprime(link, eta[static_cast<size_t>(i)], g);
                double gs = glm_gsecond(link, eta[static_cast<size_t>(i)], g, gp);
                gprime_all[static_cast<size_t>(i)] = gp;
                slope[static_cast<size_t>(i)] = gp * D[static_cast<size_t>(i)];
                common[static_cast<size_t>(i)] = gs * D[static_cast<size_t>(i)];
            }
            double ame = weighted_mean(slope);

            std::vector<char> is_hit(static_cast<size_t>(k), 0);
            for (auto& h : hit) is_hit[static_cast<size_t>(h.first)] = 1;
            std::vector<double> J(static_cast<size_t>(k), 0.0);
            for (int d = 0; d < k; ++d) {
                std::vector<double> term(static_cast<size_t>(n));
                for (int64_t i = 0; i < n; ++i)
                    term[static_cast<size_t>(i)] = common[static_cast<size_t>(i)] * X[static_cast<int64_t>(d) * n + i];
                J[static_cast<size_t>(d)] = weighted_mean(term);
                if (is_hit[static_cast<size_t>(d)]) {
                    size_t h = 0; while (hit[h].first != d) ++h;
                    std::vector<double> term2(static_cast<size_t>(n));
                    for (int64_t i = 0; i < n; ++i)
                        term2[static_cast<size_t>(i)] = gprime_all[static_cast<size_t>(i)] * remaining[h][static_cast<size_t>(i)];
                    J[static_cast<size_t>(d)] += weighted_mean(term2);
                }
            }
            R.rows.push_back({reqvar, ame, se_from_jacobian(J), J});
        }
    }
    return R;
}

}  // namespace cecon
