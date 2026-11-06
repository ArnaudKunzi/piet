// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "common.h"
#include "commands.h"

#include "core/distributions.h"
#include "core/exact_sum.h"

// binscatter (engine half): Michael Stepner's binscatter 7.02 as StataBE 19.5
// runs it (the installed ado, ~/Library/Application Support/Stata/ado/plus/b/
// binscatter.ado). Oracle:
// tests/eval/golden/aer_w2/w2k_binscatter.log (the BS_* arms).
//
// The engine owns every NUMBER: the sample (marksample + markout),
// the residualization on controls()/absorb(), the fit-line
// regressions, the bins (fastxtile or discrete) and
// the bin points. PIET owns every piece of TEXT built from them
// (the graph command, the savedata .csv/.do files, the reportreg tables, e()),
// because that text is Stata's macro and file-write rendering of these doubles.
//
// The arithmetic follows what the oracle's %21x records show Stata doing:
//   * a mean is an exactly accumulated sum (Stata's quad precision) rounded to
//     double, then divided by the (weight) count: `summarize, meanonly` and the
//     regressions' centering both read that way (bin points of BS_ctl0 to the
//     last bit: 7.446874999999999, not 7.446875);
//   * a regression's cross-products are centered on those means and summed in
//     double in data order (= `matrix accum, deviations`);
//   * regress's slopes come from a unit-diagonal-scaled Cholesky solve of
//     that matrix (identified bit for bit from %21x probes of StataBE 19.5:
//     1,757 one-regressor and 236 two-regressor fits, every coefficient and
//     constant; see solve_regress below), and the constant is the y-mean
//     minus the dot product of the slopes with the x-means. The sweep (as
//     invsym) still gives the inverse (e(V)) and the collinearity omissions;
//   * predict's residual is stored as a FLOAT (the factory c(type)) and the
//     added mean is stored as a float again (`replace residvar = residvar +
//     r(mean)`); qfit's x^2 is `gen x_r2 = x_r^2`, a float.
//     Under `set type double` both are doubles (--double).

// No fused multiply-add: Stata's arithmetic rounds every product (the oracle's
// constants differ from an FMA-contracted `ybar - b*xbar` in the last bit).
#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(__GNUC__)
#pragma GCC optimize("fp-contract=off")
#endif

namespace cecon {
namespace cli {

namespace {

const double kNaN = std::numeric_limits<double>::quiet_NaN();

// An error the kernel raises after it has printed something: the ado prints
// its warnings and reportreg tables as it goes, so they stand before the error
// (`discrete controls(z)`: the warning first, then r(134)).
// cmd_binscatter replies {error, rc, messages, byvals} for it; a caller that
// catches EngineError (the unit test) sees the plain error.
struct BinscatterFailure : EngineError {
    json messages;
    json byvals;
    BinscatterFailure(const std::string& code_, const std::string& msg, const std::string& rc, json messages_,
                      json byvals_)
        : EngineError(code_, msg, rc), messages(std::move(messages_)), byvals(std::move(byvals_)) {}
};

// Exact-enough summation: core/exact_sum.h's ExactSum (a double-double
// accumulator rounded once), the rule every summary statistic of the engine
// uses. Stata's quad-precision sums read this way (see the header).

double to_float(double v) { return std::isnan(v) ? v : static_cast<double>(static_cast<float>(v)); }

// The rows of one regression, with their (possibly normalized) weights.
struct LsInput {
    std::vector<int64_t> rows;                  // dataset rows, data order
    std::vector<const double*> x;               // regressor columns (full length)
    const double* y = nullptr;                  // dependent column (full length)
    const double* w = nullptr;                  // raw weights (full length) or nullptr
    bool normalize_w = false;                   // aweights: rescaled to sum to N
    const std::vector<int32_t>* fe = nullptr;   // absorb() group code per row (full length) or nullptr
    int n_fe = 0;
};

struct LsResult {
    int64_t n = 0;
    std::vector<double> b;       // per regressor (0 for an omitted one)
    std::vector<char> omitted;
    double cons = 0.0;           // only without fe
    double ybar = 0.0;
    double tss = 0.0, rss = 0.0;
    std::vector<double> inv;     // k*k inverse of the centered cross-product matrix
    int df_m = 0;
    double wsum = 0.0;
    // residual per row of `rows` (y - xb - d)
    std::vector<double> resid;
};

// regress's (builtin _regress) solve of the centered normal equations A b = c,
// over the kept regressors `idx` (A is k*k row-major). Stata's code is closed;
// the operation order below is the one that reproduces its e(b) in %21x:
//   D_j  = sqrt(A_jj)                         (scale to unit diagonal)
//   A'_ij = A_ij / (D_i * D_j),  c'_j = c_j / D_j
//   Cholesky A' = L L':  L_jj = sqrt(A'_jj - sum_p L_jp^2),
//                        L_ij = (A'_ij - sum_p L_ip L_jp) / L_jj
//   forward z_i = (c'_i - sum_p L_ip z_p) / L_ii,
//   back    b'_i = (z_i - sum_{p>i} L_pi b'_p) / L_ii,   b_j = b'_j / D_j,
// each sum accumulated first (ascending p) and then subtracted. The scaling
// is visible in the last bit: with one regressor b = ((c/D)/L')/L'/D where
// L' = sqrt(A/(D*D)) is 1 - 2^-53 exactly when fl(D*D) > A, and plain
// (c/D)/D otherwise (1,500 two-observation fits split on that test with no
// exception). Matches all 236 two-regressor probe fits; three or more
// regressors match about 85% of probes (op order not fully identified).
// Returns false when a pivot is not positive (the caller keeps the sweep).
bool solve_regress(const std::vector<double>& A, const std::vector<double>& c, int k, const std::vector<int>& idx,
                   std::vector<double>& b) {
    const int m = static_cast<int>(idx.size());
    auto at = [&](int i, int j) { return A[static_cast<size_t>(idx[static_cast<size_t>(i)]) * k + idx[static_cast<size_t>(j)]]; };
    std::vector<double> D(static_cast<size_t>(m)), Ap(static_cast<size_t>(m) * m), cp(static_cast<size_t>(m));
    for (int i = 0; i < m; ++i) {
        const double d = at(i, i);
        if (!(d > 0.0)) return false;
        D[static_cast<size_t>(i)] = std::sqrt(d);
    }
    for (int i = 0; i < m; ++i) {
        for (int j = 0; j < m; ++j)
            Ap[static_cast<size_t>(i) * m + j] = at(i, j) / (D[static_cast<size_t>(i)] * D[static_cast<size_t>(j)]);
        cp[static_cast<size_t>(i)] = c[static_cast<size_t>(idx[static_cast<size_t>(i)])] / D[static_cast<size_t>(i)];
    }
    std::vector<double> L(static_cast<size_t>(m) * m, 0.0);
    auto Lx = [&](int i, int j) -> double& { return L[static_cast<size_t>(i) * m + j]; };
    for (int j = 0; j < m; ++j) {
        double s = 0.0;
        for (int p = 0; p < j; ++p) s += Lx(j, p) * Lx(j, p);
        const double piv = Ap[static_cast<size_t>(j) * m + j] - s;
        if (!(piv > 0.0)) return false;
        Lx(j, j) = std::sqrt(piv);
        for (int i = j + 1; i < m; ++i) {
            double t = 0.0;
            for (int p = 0; p < j; ++p) t += Lx(i, p) * Lx(j, p);
            Lx(i, j) = (Ap[static_cast<size_t>(i) * m + j] - t) / Lx(j, j);
        }
    }
    std::vector<double> z(static_cast<size_t>(m)), bp(static_cast<size_t>(m));
    for (int i = 0; i < m; ++i) {
        double s = 0.0;
        for (int p = 0; p < i; ++p) s += Lx(i, p) * z[static_cast<size_t>(p)];
        z[static_cast<size_t>(i)] = (cp[static_cast<size_t>(i)] - s) / Lx(i, i);
    }
    for (int i = m - 1; i >= 0; --i) {
        double s = 0.0;
        for (int p = i + 1; p < m; ++p) s += Lx(p, i) * bp[static_cast<size_t>(p)];
        bp[static_cast<size_t>(i)] = (z[static_cast<size_t>(i)] - s) / Lx(i, i);
    }
    b.assign(static_cast<size_t>(k), 0.0);
    for (int i = 0; i < m; ++i) b[static_cast<size_t>(idx[static_cast<size_t>(i)])] = bp[static_cast<size_t>(i)] / D[static_cast<size_t>(i)];
    return true;
}

// Weighted least squares on centered (or within-group demeaned) data. The
// arithmetic model is the one in the file header; collinear regressors are
// omitted in order, as invsym does (pivot below 1e-13 of its diagonal).
LsResult least_squares(const LsInput& in, bool want_resid) {
    LsResult r;
    const int64_t n = static_cast<int64_t>(in.rows.size());
    const int k = static_cast<int>(in.x.size());
    r.n = n;
    r.b.assign(static_cast<size_t>(k), 0.0);
    r.omitted.assign(static_cast<size_t>(k), 0);
    if (n == 0) return r;

    std::vector<double> w(static_cast<size_t>(n), 1.0);
    if (in.w) {
        ExactSum ws;
        for (int64_t i = 0; i < n; ++i) {
            w[static_cast<size_t>(i)] = in.w[in.rows[static_cast<size_t>(i)]];
            ws.add(w[static_cast<size_t>(i)]);
        }
        if (in.normalize_w) {
            const double scale = static_cast<double>(n) / ws.value();
            for (auto& v : w) v *= scale;
        }
    }
    ExactSum wsum;
    for (double v : w) wsum.add(v);
    r.wsum = wsum.value();

    // Centered columns: on the weighted mean (no fe) or on the group means (fe).
    auto center = [&](const double* col, std::vector<double>& out, double* mean_out) {
        out.resize(static_cast<size_t>(n));
        if (!in.fe) {
            ExactSum s;
            for (int64_t i = 0; i < n; ++i) {
                const double v = col[in.rows[static_cast<size_t>(i)]];
                s.add(in.w ? w[static_cast<size_t>(i)] * v : v);
            }
            const double m = s.value() / r.wsum;
            if (mean_out) *mean_out = m;
            for (int64_t i = 0; i < n; ++i) out[static_cast<size_t>(i)] = col[in.rows[static_cast<size_t>(i)]] - m;
        } else {
            std::vector<ExactSum> gs(static_cast<size_t>(in.n_fe)), gw(static_cast<size_t>(in.n_fe));
            for (int64_t i = 0; i < n; ++i) {
                const int64_t row = in.rows[static_cast<size_t>(i)];
                const int32_t g = (*in.fe)[static_cast<size_t>(row)];
                const double v = col[row];
                gs[static_cast<size_t>(g)].add(in.w ? w[static_cast<size_t>(i)] * v : v);
                gw[static_cast<size_t>(g)].add(w[static_cast<size_t>(i)]);
            }
            std::vector<double> gm(static_cast<size_t>(in.n_fe), 0.0);
            for (int g = 0; g < in.n_fe; ++g) {
                const double ww = gw[static_cast<size_t>(g)].value();
                gm[static_cast<size_t>(g)] = ww > 0 ? gs[static_cast<size_t>(g)].value() / ww : 0.0;
            }
            if (mean_out) {
                ExactSum s;
                for (int64_t i = 0; i < n; ++i) {
                    const double v = col[in.rows[static_cast<size_t>(i)]];
                    s.add(in.w ? w[static_cast<size_t>(i)] * v : v);
                }
                *mean_out = s.value() / r.wsum;
            }
            for (int64_t i = 0; i < n; ++i) {
                const int64_t row = in.rows[static_cast<size_t>(i)];
                out[static_cast<size_t>(i)] = col[row] - gm[static_cast<size_t>((*in.fe)[static_cast<size_t>(row)])];
            }
        }
    };

    std::vector<std::vector<double>> dx(static_cast<size_t>(k));
    std::vector<double> xbar(static_cast<size_t>(k), 0.0);
    for (int j = 0; j < k; ++j) center(in.x[static_cast<size_t>(j)], dx[static_cast<size_t>(j)], &xbar[static_cast<size_t>(j)]);
    std::vector<double> dy;
    center(in.y, dy, &r.ybar);

    // Centered cross-products, summed in double in data order.
    std::vector<double> A(static_cast<size_t>(k) * k, 0.0), c(static_cast<size_t>(k), 0.0);
    double syy = 0.0;
    for (int64_t i = 0; i < n; ++i) {
        const double wi = w[static_cast<size_t>(i)];
        const double yi = dy[static_cast<size_t>(i)];
        for (int j = 0; j < k; ++j) {
            const double xj = dx[static_cast<size_t>(j)][static_cast<size_t>(i)];
            const double wxj = in.w ? wi * xj : xj;
            for (int l = 0; l <= j; ++l) A[static_cast<size_t>(j) * k + l] += wxj * dx[static_cast<size_t>(l)][static_cast<size_t>(i)];
            c[static_cast<size_t>(j)] += wxj * yi;
        }
        syy += (in.w ? wi * yi : yi) * yi;
    }
    for (int j = 0; j < k; ++j)
        for (int l = j + 1; l < k; ++l) A[static_cast<size_t>(j) * k + l] = A[static_cast<size_t>(l) * k + j];

    // Sweep the augmented matrix [A c; c' syy] (invsym's operator) on each
    // regressor pivot in column order: the y column then holds the slopes and
    // the corner the residual sum of squares. A pivot that has lost all but
    // 1e-13 of its diagonal is collinear with the earlier ones and is omitted.
    const int K = k + 1;
    std::vector<double> M(static_cast<size_t>(K) * K, 0.0);
    for (int i = 0; i < k; ++i) {
        for (int j = 0; j < k; ++j) M[static_cast<size_t>(i) * K + j] = A[static_cast<size_t>(i) * k + j];
        M[static_cast<size_t>(i) * K + k] = c[static_cast<size_t>(i)];
        M[static_cast<size_t>(k) * K + i] = c[static_cast<size_t>(i)];
    }
    M[static_cast<size_t>(k) * K + k] = syy;
    std::vector<char> swept(static_cast<size_t>(k), 0);
    for (int p = 0; p < k; ++p) {
        const double d0 = A[static_cast<size_t>(p) * k + p];
        const double d = M[static_cast<size_t>(p) * K + p];
        if (!(d0 > 0.0) || !(d > 1e-13 * d0)) {
            r.omitted[static_cast<size_t>(p)] = 1;
            continue;
        }
        std::vector<double> N = M;
        for (int i = 0; i < K; ++i) {
            if (i == p) continue;
            for (int j = 0; j < K; ++j) {
                if (j == p) continue;
                N[static_cast<size_t>(i) * K + j] =
                    M[static_cast<size_t>(i) * K + j] - M[static_cast<size_t>(i) * K + p] * M[static_cast<size_t>(p) * K + j] / d;
            }
        }
        for (int i = 0; i < K; ++i) {
            if (i == p) continue;
            N[static_cast<size_t>(i) * K + p] = M[static_cast<size_t>(i) * K + p] / d;
            N[static_cast<size_t>(p) * K + i] = M[static_cast<size_t>(p) * K + i] / d;
        }
        N[static_cast<size_t>(p) * K + p] = -1.0 / d;
        M.swap(N);
        swept[static_cast<size_t>(p)] = 1;
    }
    r.inv.assign(static_cast<size_t>(k) * k, 0.0);
    for (int i = 0; i < k; ++i) {
        if (!swept[static_cast<size_t>(i)]) continue;
        for (int j = 0; j < k; ++j)
            if (swept[static_cast<size_t>(j)]) r.inv[static_cast<size_t>(i) * k + j] = -M[static_cast<size_t>(i) * K + j];
        r.b[static_cast<size_t>(i)] = M[static_cast<size_t>(i) * K + k];
        ++r.df_m;
    }
    if (!in.fe) {
        // regress: the slopes of the kept regressors from _regress's own solve
        // (solve_regress), the constant as ybar - (sum_j b_j xbar_j).
        std::vector<int> kept;
        for (int j = 0; j < k; ++j)
            if (swept[static_cast<size_t>(j)]) kept.push_back(j);
        std::vector<double> bs;
        if (!kept.empty() && solve_regress(A, c, k, kept, bs)) r.b = bs;
        double xb = 0.0;
        for (int j = 0; j < k; ++j) xb += r.b[static_cast<size_t>(j)] * xbar[static_cast<size_t>(j)];
        r.cons = r.ybar - xb;
    } else {
        double cons = r.ybar;
        for (int j = 0; j < k; ++j) cons -= r.b[static_cast<size_t>(j)] * xbar[static_cast<size_t>(j)];
        r.cons = cons;
    }

    r.tss = syy;
    double rss = 0.0;
    if (want_resid) r.resid.resize(static_cast<size_t>(n));
    for (int64_t i = 0; i < n; ++i) {
        double e = dy[static_cast<size_t>(i)];
        for (int j = 0; j < k; ++j) e -= r.b[static_cast<size_t>(j)] * dx[static_cast<size_t>(j)][static_cast<size_t>(i)];
        if (want_resid) r.resid[static_cast<size_t>(i)] = e;
        rss += (in.w ? w[static_cast<size_t>(i)] * e : e) * e;
    }
    r.rss = rss;
    return r;
}

// Stata's percentile at fraction num/den of the (weighted) sample, `_pctile`'s
// rule: walk the cumulative weight; landing exactly on the target averages
// with the next observation, otherwise the observation that crosses it.
// `vals` ascending, `w` parallel (nullptr = unit weights). The exact-landing
// test is cum*den == W*num, exact for integer weights.
double pctile_at(const std::vector<double>& vals, const std::vector<double>* w, double W, int64_t num,
                 int64_t den) {
    const int64_t N = static_cast<int64_t>(vals.size());
    if (N == 0) return kNaN;
    const double target = W * static_cast<double>(num);
    double cum = 0.0;
    for (int64_t i = 0; i < N; ++i) {
        cum += w ? (*w)[static_cast<size_t>(i)] : 1.0;
        const double lhs = cum * static_cast<double>(den);
        if (lhs == target) return i + 1 < N ? (vals[static_cast<size_t>(i)] + vals[static_cast<size_t>(i + 1)]) / 2.0 : vals[static_cast<size_t>(i)];
        if (lhs > target) return vals[static_cast<size_t>(i)];
    }
    return vals[static_cast<size_t>(N - 1)];
}

// A bin's point: mean (summarize, meanonly: exact weighted sum over exact
// weight sum) or median (_pctile, p(50)).
double bin_stat(const std::vector<int64_t>& rows, const double* v, const double* w, bool medians) {
    if (rows.empty()) return kNaN;
    if (!medians) {
        ExactSum s, sw;
        for (int64_t r : rows) {
            const double wi = w ? w[r] : 1.0;
            s.add(w ? wi * v[r] : v[r]);
            sw.add(wi);
        }
        return s.value() / sw.value();
    }
    std::vector<std::pair<double, double>> vw;
    vw.reserve(rows.size());
    for (int64_t r : rows) vw.emplace_back(v[r], w ? w[r] : 1.0);
    std::stable_sort(vw.begin(), vw.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<double> vals, ws;
    ExactSum sw;
    for (const auto& p : vw) {
        vals.push_back(p.first);
        ws.push_back(p.second);
        sw.add(p.second);
    }
    return pctile_at(vals, &ws, sw.value(), 50, 100);
}

json reg_json(const LsResult& r, const std::vector<std::string>& names, bool fw) {
    const int k = static_cast<int>(r.b.size());
    const double N = fw ? r.wsum : static_cast<double>(r.n);
    const double df_r = N - r.df_m - 1;
    const double mss = r.tss - r.rss;
    const double s2 = df_r > 0 ? r.rss / df_r : kNaN;
    json coefs = json::array();
    const double tcrit = df_r > 0 ? stata_invttail(df_r, 0.025) : kNaN;
    for (int j = 0; j < k; ++j) {
        const bool om = r.omitted[static_cast<size_t>(j)] != 0;
        const double b = r.b[static_cast<size_t>(j)];
        const double se = om ? kNaN : std::sqrt(s2 * r.inv[static_cast<size_t>(j) * k + j]);
        const double t = om ? kNaN : b / se;
        json row = {{"name", names[static_cast<size_t>(j)]}, {"omitted", om}, {"b", b}};
        row["se"] = std::isnan(se) ? json(nullptr) : json(se);
        row["t"] = std::isnan(t) ? json(nullptr) : json(t);
        const double p = std::isnan(t) ? kNaN : 2.0 * stata_ttail(df_r, std::fabs(t));
        row["p"] = std::isnan(p) ? json(nullptr) : json(p);
        row["lo"] = std::isnan(se) ? json(nullptr) : json(b - tcrit * se);
        row["hi"] = std::isnan(se) ? json(nullptr) : json(b + tcrit * se);
        coefs.push_back(row);
    }
    // _cons: its variance is s2 * (1/W + xbar' inv xbar); taken from the
    // centered inverse the same way regress's full inverse holds it.
    json out = {{"N", N}, {"df_m", r.df_m}, {"df_r", df_r}, {"mss", mss}, {"rss", r.rss}, {"tss", r.tss},
                {"coefs", coefs}};
    const double F = (r.df_m > 0 && df_r > 0) ? (mss / r.df_m) / s2 : kNaN;
    out["F"] = std::isnan(F) ? json(nullptr) : json(F);
    const double pF = std::isnan(F) ? kNaN : f_pvalue(F, r.df_m, df_r);
    out["p_F"] = std::isnan(pF) ? json(nullptr) : json(pF);
    out["r2"] = r.tss > 0 ? json(mss / r.tss) : json(nullptr);
    out["r2_a"] = (r.tss > 0 && df_r > 0) ? json(1.0 - (r.rss / df_r) / (r.tss / (N - 1))) : json(nullptr);
    out["rmse"] = std::isnan(s2) ? json(nullptr) : json(std::sqrt(s2));
    return out;
}

}  // namespace

// What binscatter's options say, once parsed (the CLI flags one for one).
struct BinscatterSpec {
    std::vector<std::string> ys;        // several y (varlist minus its last word)
    std::string x;                      // the last word
    std::string by;                     // by(varname), numeric
    std::vector<std::string> controls;  // controls(varlist numeric ts fv)
    std::string absorb;                 // absorb(varname)
    std::string weight;                 // [aw=]/[fw=] variable
    bool fweight = false;
    bool noaddmean = false, discrete = false, medians = false, reportreg = false;
    bool genxq = false;                 // emit the fastxtile bin of every row (genxq())
    // c(type) is double (`set type double`): the residuals (`predict, residuals`)
    // and qfit's x^2 (`gen`) are new variables of the default
    // storage type, so they stay double instead of the factory float.
    bool double_storage = false;
    std::string linetype = "lfit";
    int nquantiles = 20;                // Nquantiles(integer 20)
};

// The kernel: everything after the if/in restriction. `ifin` is the if/in
// mask (1 = admitted), length ds.n_rows(). Callable without the CLI (the unit
// test tests/unit/test_binscatter.cpp drives it on the oracle's fixture).
json binscatter_kernel(const Dataset& ds, std::vector<char> touse, const BinscatterSpec& spec) {
    const std::vector<std::string>& ynames = spec.ys;
    const std::string& xname = spec.x;
    const std::string& byname = spec.by;
    const std::vector<std::string>& controls = spec.controls;
    const std::string& absorb = spec.absorb;
    const std::string& wname = spec.weight;
    const bool fw = spec.fweight;
    const bool noaddmean = spec.noaddmean;
    const bool discrete_opt = spec.discrete;
    const bool medians = spec.medians;
    const bool reportreg = spec.reportreg;
    const auto stored = [&spec](double v) { return spec.double_storage ? v : to_float(v); };
    const std::string& linetype = spec.linetype;
    if (linetype != "lfit" && linetype != "qfit" && linetype != "connect" && linetype != "none")
        throw EngineError("bad_input", "linetype() must either be connect, lfit, qfit, or none", "r(198)");
    const bool fit = linetype == "lfit" || linetype == "qfit";
    const bool qfit = linetype == "qfit";
    int nquantiles = spec.nquantiles;
    const int64_t N = ds.n_rows();
    if (static_cast<int64_t>(touse.size()) != N) throw EngineError("internal", "binscatter: sample mask length");
    auto num_col = [&](const std::string& name) -> const double* {
        if (!ds.has(name))
            throw EngineError("missing_column", "variable " + name + " not found", "r(111)");
        const Column& c = ds.col(name);
        if (c.dtype != DType::F64) throw EngineError("factor_misuse", "type mismatch: " + name + " is a string variable", "r(109)");
        return c.f64();
    };
    std::vector<const double*> ycols;
    for (const auto& y : ynames) ycols.push_back(num_col(y));
    const double* xcol = num_col(xname);
    const double* bycol = byname.empty() ? nullptr : num_col(byname);
    const double* wcol = wname.empty() ? nullptr : num_col(wname);

    // ---- the sample: if/in, every y and x, by, absorb, controls, weight
    std::vector<int32_t> fe_code;
    int n_fe = 0;
    const Column* abscol = absorb.empty() ? nullptr : &ds.col(absorb);
    for (int64_t i = 0; i < N; ++i) {
        if (!touse[static_cast<size_t>(i)]) continue;
        bool ok = !std::isnan(xcol[i]);
        for (const double* y : ycols) ok = ok && !std::isnan(y[i]);
        if (bycol) ok = ok && !std::isnan(bycol[i]);
        if (wcol) {
            if (ok && !std::isnan(wcol[i]) && wcol[i] < 0)
                throw EngineError("bad_input", "negative weights encountered", "r(402)");
            ok = ok && !std::isnan(wcol[i]) && wcol[i] != 0.0;
            if (ok && fw && wcol[i] != std::floor(wcol[i]))
                throw EngineError("bad_input", "may not use noninteger frequency weights", "r(401)");
        }
        touse[static_cast<size_t>(i)] = ok ? 1 : 0;
    }
    // absorb() and the controls (factor terms included) mark out through the
    // model builder's own listwise rule over the rows that survived so far.
    if (abscol || !controls.empty()) {
        const ModelData md = build_model(ds, xname, controls, absorb.empty() ? std::vector<std::string>{}
                                                                               : std::vector<std::string>{absorb},
                                         {}, "", /*drop_singleton=*/false, nullptr, &touse, wname,
                                         wcol ? (fw ? WeightKind::Frequency : WeightKind::Analytic)
                                              : WeightKind::None);
        std::vector<char> kept(static_cast<size_t>(N), 0);
        for (int64_t r : md.keep) kept[static_cast<size_t>(r)] = 1;
        touse = kept;
        if (abscol) {
            fe_code.assign(static_cast<size_t>(N), -1);
            const Factor& f = md.fe.at(0);
            for (size_t j = 0; j < md.keep.size(); ++j) fe_code[static_cast<size_t>(md.keep[j])] = f.codes[j];
            n_fe = f.n_levels;
        }
    }
    std::vector<int64_t> rows;
    for (int64_t i = 0; i < N; ++i)
        if (touse[static_cast<size_t>(i)]) rows.push_back(i);
    const int64_t samplesize = static_cast<int64_t>(rows.size());
    if (samplesize == 0) throw EngineError("no_obs", "no observations", "r(2000)");

    // ---- by values: ascending distinct values over the sample
    std::vector<double> byvals;
    if (bycol) {
        for (int64_t r : rows) byvals.push_back(bycol[r]);
        std::sort(byvals.begin(), byvals.end());
        byvals.erase(std::unique(byvals.begin(), byvals.end()), byvals.end());
    }
    const int bynum = bycol ? static_cast<int>(byvals.size()) : 1;

    // ---- residualization
    std::vector<std::vector<double>> resid_store;
    const double* x_r = xcol;
    std::vector<const double*> y_r = ycols;
    if (abscol || !controls.empty()) {
        std::vector<std::string> vars = {xname};
        vars.insert(vars.end(), ynames.begin(), ynames.end());
        resid_store.resize(vars.size());
        for (size_t v = 0; v < vars.size(); ++v) {
            const ModelData md = build_model(ds, vars[v], controls, {}, {}, "", false, nullptr, &touse, "",
                                             WeightKind::None);
            if (static_cast<int64_t>(md.keep.size()) != samplesize)
                throw EngineError("internal", "binscatter: residualization sample drifted for " + vars[v]);
            // The design's columns, full length (indexed by dataset row).
            const int k0 = md.dm.k - (md.has_intercept ? 1 : 0);
            std::vector<std::vector<double>> cols(static_cast<size_t>(k0), std::vector<double>(static_cast<size_t>(N), kNaN));
            for (int j = 0; j < k0; ++j) {
                const double* src = md.dm.col(j + (md.has_intercept ? 1 : 0));
                for (size_t r = 0; r < md.keep.size(); ++r) cols[static_cast<size_t>(j)][static_cast<size_t>(md.keep[r])] = src[r];
            }
            LsInput in;
            in.rows = rows;
            for (const auto& cvec : cols) in.x.push_back(cvec.data());
            const double* vcol = v == 0 ? xcol : ycols[v - 1];
            in.y = vcol;
            in.w = wcol;
            in.normalize_w = wcol && !fw;
            if (abscol) {
                in.fe = &fe_code;
                in.n_fe = n_fe;
            }
            const LsResult ls = least_squares(in, /*want_resid=*/true);
            double mean = 0.0;
            {
                ExactSum s, sw;
                for (int64_t r : rows) {
                    const double wi = wcol ? wcol[r] : 1.0;
                    s.add(wcol ? wi * vcol[r] : vcol[r]);
                    sw.add(wi);
                }
                mean = s.value() / sw.value();
            }
            std::vector<double>& out = resid_store[v];
            out.assign(static_cast<size_t>(N), kNaN);
            for (size_t i = 0; i < rows.size(); ++i) {
                const double rf = stored(ls.resid[i]);
                out[static_cast<size_t>(rows[i])] = noaddmean ? rf : stored(rf + mean);
            }
        }
        x_r = resid_store[0].data();
        for (size_t v = 0; v < ycols.size(); ++v) y_r[v] = resid_store[v + 1].data();
    }

    // ---- fit-line regressions, in Stata's print order
    json messages = json::array();
    auto fail = [&](const std::string& code, const std::string& msg, const std::string& rc) {
        throw BinscatterFailure(code, msg, rc, messages, byvals);
    };
    std::vector<double> x_r2_store;
    if (qfit) {
        x_r2_store.assign(static_cast<size_t>(N), kNaN);
        for (int64_t r : rows) x_r2_store[static_cast<size_t>(r)] = stored(x_r[r] * x_r[r]);
    }
    // coefs[y][by] = row of e(y#_coefs) (x^2 x _cons or x _cons), null = missing
    std::vector<json> coefs(ynames.size(), json::array());
    if (fit) {
        for (int b = 0; b < bynum; ++b) {
            std::vector<int64_t> grows;
            for (int64_t r : rows)
                if (!bycol || bycol[r] == byvals[static_cast<size_t>(b)]) grows.push_back(r);
            if (reportreg) {
                json m = {{"kind", "group"}};
                if (bycol) m["byval"] = byvals[static_cast<size_t>(b)];
                messages.push_back(m);
            }
            for (size_t yi = 0; yi < ynames.size(); ++yi) {
                if (reportreg && ynames.size() > 1) messages.push_back({{"kind", "yvar"}, {"name", ynames[yi]}});
                if (grows.empty()) {
                    // regress: r(2000), caught by the ado
                    messages.push_back({{"kind", "no_obs"}});
                    json row = json::array();
                    for (int j = 0; j < (qfit ? 3 : 2); ++j) row.push_back(nullptr);
                    coefs[yi].push_back(row);
                    continue;
                }
                LsInput in;
                in.rows = grows;
                if (qfit) in.x.push_back(x_r2_store.data());
                in.x.push_back(x_r);
                in.y = y_r[yi];
                in.w = wcol;
                in.normalize_w = wcol && !fw;
                const LsResult ls = least_squares(in, false);
                json row = json::array();
                for (double v : ls.b) row.push_back(v);
                row.push_back(ls.cons);
                coefs[yi].push_back(row);
                if (reportreg) {
                    json reg = reg_json(ls, qfit ? std::vector<std::string>{"x2", "x"} : std::vector<std::string>{"x"},
                                        fw);
                    // _cons: Var = s2 * (1/W + xbar' inv xbar) is not kept by
                    // least_squares; derive it from the regression's own inputs.
                    {
                        const int k = static_cast<int>(ls.b.size());
                        std::vector<double> xbar(static_cast<size_t>(k));
                        for (int j = 0; j < k; ++j) {
                            ExactSum s, sw;
                            const double* col = in.x[static_cast<size_t>(j)];
                            for (int64_t r : grows) {
                                const double wi = wcol ? wcol[r] : 1.0;
                                s.add(wcol ? wi * col[r] : col[r]);
                                sw.add(wi);
                            }
                            xbar[static_cast<size_t>(j)] = s.value() / sw.value();
                        }
                        double q = 0.0;
                        for (int i = 0; i < k; ++i)
                            for (int j = 0; j < k; ++j) q += xbar[static_cast<size_t>(i)] * ls.inv[static_cast<size_t>(i) * k + j] * xbar[static_cast<size_t>(j)];
                        const double df_r = reg["df_r"].get<double>();
                        const double s2 = df_r > 0 ? ls.rss / df_r : kNaN;
                        const double se = std::sqrt(s2 * (1.0 / ls.wsum + q));
                        const double t = ls.cons / se;
                        const double tcrit = df_r > 0 ? stata_invttail(df_r, 0.025) : kNaN;
                        json crow = {{"name", "_cons"}, {"omitted", false}, {"b", ls.cons}};
                        crow["se"] = std::isnan(se) ? json(nullptr) : json(se);
                        crow["t"] = std::isnan(t) ? json(nullptr) : json(t);
                        const double p = std::isnan(t) ? kNaN : 2.0 * stata_ttail(df_r, std::fabs(t));
                        crow["p"] = std::isnan(p) ? json(nullptr) : json(p);
                        crow["lo"] = std::isnan(se) ? json(nullptr) : json(ls.cons - tcrit * se);
                        crow["hi"] = std::isnan(se) ? json(nullptr) : json(ls.cons + tcrit * se);
                        reg["coefs"].push_back(crow);
                    }
                    messages.push_back({{"kind", "regress"}, {"reg", reg}});
                }
            }
        }
    }

    // ---- the bins. Values over the sample in ascending x_r order.
    std::vector<int64_t> sorted = rows;
    std::stable_sort(sorted.begin(), sorted.end(), [&](int64_t p, int64_t q) { return x_r[p] < x_r[q]; });
    auto count_unique = [&](const std::vector<int64_t>& srt, const double* col) {
        int64_t u = 0;
        double prev = kNaN;
        for (size_t i = 0; i < srt.size(); ++i) {
            const double v = col[srt[i]];
            if (i == 0 || v != prev) ++u;
            prev = v;
        }
        return u;
    };
    bool discrete = discrete_opt;
    bool binned = false;                                     // fastxtile ran (genxq's variable)
    std::vector<double> xq(static_cast<size_t>(N), kNaN);   // the bin key per row
    const int64_t nunique_x = count_unique(sorted, x_r);
    if (!discrete) {
        if (nunique_x <= nquantiles) {
            // fewer unique values than bins: discrete
            discrete = true;
            nquantiles = static_cast<int>(nunique_x);
            messages.push_back({{"kind", "auto_discrete"}, {"nquantiles", nquantiles}});
            for (int64_t r : rows) xq[static_cast<size_t>(r)] = x_r[r];
        } else {
            // fastxtile: nq checks, _pctile cut points, 1+irecode
            if (nquantiles > samplesize + 1)
                fail("bad_input",
                     "nquantiles() must be less than or equal to the number of observations [" +
                         std::to_string(samplesize) + "] plus one",
                     "r(198)");
            if (nquantiles < 2) fail("bad_input", "nquantiles() must be greater than or equal to 2", "r(198)");
            std::vector<double> vals, ws;
            ExactSum W;
            for (int64_t r : sorted) {
                vals.push_back(x_r[r]);
                const double wi = wcol ? wcol[r] : 1.0;
                ws.push_back(wi);
                W.add(wi);
            }
            std::vector<double> cuts;
            for (int i = 1; i < nquantiles; ++i)
                cuts.push_back(pctile_at(vals, &ws, W.value(), i, nquantiles));
            for (int64_t r : rows) {
                int bin = 1;
                for (double cut : cuts)
                    if (x_r[r] > cut) ++bin;
                xq[static_cast<size_t>(r)] = bin;
            }
            binned = true;
            if (!bycol) {
                std::vector<double> bins;
                for (int64_t r : rows) bins.push_back(xq[static_cast<size_t>(r)]);
                std::sort(bins.begin(), bins.end());
                const int got = static_cast<int>(std::unique(bins.begin(), bins.end()) - bins.begin());
                if (got != nquantiles) {
                    messages.push_back({{"kind", "nq_warning"}, {"requested", nquantiles}, {"generated", got}});
                    nquantiles = got;
                }
            }
        }
    } else {
        if (abscol || !controls.empty()) messages.push_back({{"kind", "discrete_controls_warning"}});
        // characterize_unique_vals_sorted(x_r, ..., N/2): more unique values -> 134
        if (static_cast<double>(nunique_x) > static_cast<double>(samplesize) / 2.0)
            fail("bad_input", "discrete specified, but number of unique values is > (sample size/2)", "r(134)");
        nquantiles = static_cast<int>(nunique_x);
        for (int64_t r : rows) xq[static_cast<size_t>(r)] = x_r[r];
    }

    // ---- bin points: per by-group, the bins present, ascending
    json points = json::array();
    for (size_t yi = 0; yi < ynames.size(); ++yi) points.push_back(json::array());
    json bin_counts = json::array();
    for (int b = 0; b < bynum; ++b) {
        std::map<double, std::vector<int64_t>> bins;
        for (int64_t r : sorted)
            if (!bycol || bycol[r] == byvals[static_cast<size_t>(b)]) bins[xq[static_cast<size_t>(r)]].push_back(r);
        json xs = json::array();
        json counts = json::array();
        std::vector<json> ys(ynames.size(), json::array());
        for (const auto& [key, brows] : bins) {
            const double xv = discrete ? key : bin_stat(brows, x_r, wcol, medians);
            xs.push_back(xv);
            counts.push_back(static_cast<int64_t>(brows.size()));
            for (size_t yi = 0; yi < ynames.size(); ++yi) ys[yi].push_back(bin_stat(brows, y_r[yi], wcol, medians));
        }
        for (size_t yi = 0; yi < ynames.size(); ++yi)
            points[yi].push_back({{"x", xs}, {"y", ys[yi]}});
        bin_counts.push_back(counts);
    }

    json out = {
        {"estimator", "binscatter"},
        {"N", samplesize},
        {"x", xname},
        {"ys", ynames},
        {"linetype", linetype},
        {"nquantiles", nquantiles},
        {"discrete", discrete},
        {"by", byname.empty() ? json(nullptr) : json(byname)},
        {"byvals", byvals},
        {"points", points},
        {"bin_counts", bin_counts},
        {"messages", messages},
    };
    if (fit) out["coefs"] = coefs;
    if (spec.genxq && binned) {
        // genxq(): the fastxtile variable, missing outside the sample
        json col = json::array();
        for (int64_t i = 0; i < N; ++i) {
            const double v = xq[static_cast<size_t>(i)];
            col.push_back(std::isnan(v) ? json(nullptr) : json(v));
        }
        out["xq"] = col;
    }
    return out;
}

#ifndef COBRA_BINSCATTER_KERNEL_ONLY
int cmd_binscatter(const std::vector<std::string>& a) {
    const std::string data = opt(a, "--data"), ylist = opt(a, "--y"), xname = opt(a, "--x");
    if (data_missing(data) || ylist.empty() || xname.empty()) {
        std::fprintf(stderr,
            "usage: cobra binscatter --data f.hbin --y \"Y1 [Y2 ...]\" --x X [--by B]\n"
            "         [--controls \"C1 [i.C2 ...]\"] [--absorb A] [--noaddmean]\n"
            "         [--weight W --wtype aweight|fweight] [--keepif EXPR] [--inrange A/B]\n"
            "         [--nquantiles N] [--discrete] [--medians] [--linetype lfit|qfit|connect|none]\n"
            "         [--reportreg] [--genxq] [--double] [--out f.json]\n"
            "  Stepner's binscatter 7.02: the sample, the residualization on controls()/absorb()\n"
            "  (mean added back unless --noaddmean), ONE pooled fastxtile binning (or the\n"
            "  discrete rule), per-group bin means (or medians) and fit-line coefficients.\n");
        return 2;
    }
    BinscatterSpec spec;
    spec.ys = split_vars(ylist);
    spec.x = xname;
    spec.by = opt(a, "--by");
    spec.controls = split_vars(opt(a, "--controls"));
    spec.absorb = opt(a, "--absorb");
    spec.noaddmean = has(a, "--noaddmean");
    spec.discrete = has(a, "--discrete");
    spec.medians = has(a, "--medians");
    spec.reportreg = has(a, "--reportreg");
    spec.genxq = has(a, "--genxq");
    spec.double_storage = has(a, "--double");
    spec.linetype = opt(a, "--linetype", "lfit");
    const std::string nqs = opt(a, "--nquantiles");
    if (!nqs.empty()) {
        try {
            spec.nquantiles = std::stoi(nqs);
        } catch (const std::exception&) {
            throw EngineError("bad_input", "binscatter: --nquantiles must be an integer", "r(198)");
        }
    }
    spec.weight = opt(a, "--weight");
    const std::string wtype = opt(a, "--wtype");
    if (!spec.weight.empty()) {
        if (wtype == "fweight") spec.fweight = true;
        else if (wtype != "aweight")
            throw EngineError("bad_input", "binscatter: --wtype must be aweight or fweight (got `" + wtype + "')",
                              "r(101)");
    }

    const Dataset& ds = acquire_data(data);
    const int64_t N = ds.n_rows();
    std::vector<char> touse(static_cast<size_t>(N), 1);
    const std::string keepif = opt(a, "--keepif");
    if (!keepif.empty()) {
        const std::vector<char> m = eval_keepif_mask(data, keepif);
        for (int64_t i = 0; i < N && i < static_cast<int64_t>(m.size()); ++i)
            if (!m[static_cast<size_t>(i)]) touse[static_cast<size_t>(i)] = 0;
    }
    const std::string inr = opt(a, "--inrange");
    if (!inr.empty()) {
        const size_t slash = inr.find('/');
        long long lo = 0, hi = 0;
        try {
            lo = std::stoll(inr.substr(0, slash));
            hi = slash == std::string::npos ? lo : std::stoll(inr.substr(slash + 1));
        } catch (const std::exception&) {
            throw EngineError("bad_input", "binscatter: --inrange must be A/B");
        }
        for (int64_t i = 0; i < N; ++i)
            if (i + 1 < lo || i + 1 > hi) touse[static_cast<size_t>(i)] = 0;
    }
    json reply;
    try {
        reply = binscatter_kernel(ds, std::move(touse), spec);
    } catch (const BinscatterFailure& e) {
        if (e.messages.empty()) throw;  // nothing printed yet: the CLI's stderr envelope
        // The error rides on stdout with what was printed before it (PIET's
        // runCobraJson returns a parseable stdout whatever the exit code).
        json err = {{"code", e.code}, {"message", e.what()}};
        if (!e.detail.empty()) err["detail"] = e.detail;
        int rc = 0;
        if (e.detail.size() > 3 && e.detail.compare(0, 2, "r(") == 0) rc = std::atoi(e.detail.c_str() + 2);
        emit({{"error", err}, {"rc", rc}, {"messages", e.messages}, {"byvals", e.byvals}}, opt(a, "--out"));
        return exit_code_for(e.code);
    }
    emit(reply, opt(a, "--out"));
    return 0;
}
#endif  // COBRA_BINSCATTER_KERNEL_ONLY

}  // namespace cli
}  // namespace cecon
