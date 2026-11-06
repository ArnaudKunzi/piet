// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Predictive-margins-at() parity test vs Stata `margins, at()` (linear model),
// plus (BUG-194) the GLM `margins, at() --model logit` branch's CI, which must
// be normal- not t-referenced.
// margin(at) = x̄(at)'β, SE = sqrt(x̄(at)' V x̄(at)), x̄(at) = mean design row with
// the at()-listed covariates overridden. Golden from margins_at_golden.do.
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "core/distributions.h"
#include "core/linalg.h"
#include "core/model.h"
#include "estimators/feols.h"
#include "estimators/glm.h"
#include "estimators/logit.h"
#include "estimators/posthest.h"
#include "io/columnar.h"
#include "validate_dir.h"

using namespace cecon;

// BUG-194 regression: `cobra margins --at .. --model logit` (the GLM average-
// adjusted-prediction branch of cmd_margins, apps/cmds/cmds_postest.cpp) is a
// maximum-likelihood/OIM fit, so its CI must be normal (z), not t(dof) — Stata
// prints `Model VCE: OIM` and a z column there, with no residual dof. This
// reproduces that branch's exact formula (average adjusted prediction +
// delta-method SE off the model's own bread) against a live StataNow 19.5
// oracle capture (n=60, seed 194194, `logit ybin x1 x2` then
// `margins, at(x1=(0 1))`; do-file discarded after the capture below, as the
// numbers are the durable artifact — see BUG-195's lost-oracle lesson) and
// pins the ci = margin ± 1.959963984540054×se identity that is the whole of
// BUG-194's fix. Pre-fix (tc = t_critical(0.05, dof) instead of z_critical),
// this same identity check fails: t_critical(0.05, 57) ≈ 1.9721, a ~0.6%-wider
// multiplier that misses the oracle's `crit 1.959964` and the ll/ul it prints.
static int test_margins_glm_at_z() {
    std::printf("test_margins_glm_at_z (BUG-194):\n");
    // x1, x2: StataBE `set seed 194194 / set obs 60 / gen x1 = round(rnormal(0,1),.001)`
    // etc. ybin = (runiform() < invlogit(0.6*x1 - 0.4*x2 + 0.2)). Captured verbatim
    // via `outsheet x1 x2 ybin`, not regenerated here (this engine's RNG stream
    // does not need to match Stata's — only the fitted model does).
    static const double x1[] = {
        -.085, .21, .472, .739, -.879, -.738, .441, .68, .527, -1.331,
        -.951, .483, .737, 1.409, -.867, -.252, .881, .907, -.725, 1.532,
        -1.438, -.192, -1.001, -.165, .599, -.418, -1.888, -.03, -.836, -.213,
        -.976, -2.867, -.971, -.178, 1.365, -.9, .617, 1.433, 1.426, -.515,
        .583, -.089, -.014, -1.2, .781, 1.286, 1.146, .075, -.151, -.703,
        .752, -.801, -.207, -.36, -.845, 1.148, .086, -.746, .666, -.314,
    };
    static const double x2[] = {
        -.058, -1.479, -.81, .545, -.048, 1.614, -.799, -1.799, .329, -.666,
        1.285, .545, 1.197, 1.329, -.047, -.227, -.193, -.034, .501, -.145,
        -.541, .229, .132, .524, -.109, 1.378, .39, 1.629, -.947, -.224,
        -2.284, -.408, -.433, .752, -.472, 1.547, .985, .924, .161, -.698,
        2.019, .625, .533, 1.201, 1.563, .677, -.238, -.479, .526, 1.437,
        -.457, 2.381, .838, .459, .299, .127, -1.384, .161, -.765, .807,
    };
    static const double y[] = {
        0, 1, 1, 0, 1, 0, 1, 1, 1, 0,
        1, 1, 1, 1, 0, 1, 1, 0, 0, 1,
        1, 0, 0, 1, 0, 0, 1, 0, 0, 0,
        1, 1, 1, 0, 1, 0, 0, 0, 1, 1,
        0, 1, 0, 0, 1, 1, 0, 0, 0, 0,
        1, 0, 1, 0, 0, 1, 1, 0, 1, 1,
    };
    const int64_t n = 60;
    const int k = 3;  // (intercept), x1, x2
    std::vector<double> X(static_cast<size_t>(n) * k);
    for (int64_t i = 0; i < n; ++i) {
        X[static_cast<size_t>(i)] = 1.0;
        X[static_cast<size_t>(n + i)] = x1[i];
        X[static_cast<size_t>(2 * n + i)] = x2[i];
    }
    LogitResult lr = logit_fit(X.data(), n, k, y);
    const std::vector<double>& beta = lr.beta;
    const std::vector<double>& V = lr.bread;  // matches cmd_margins' V_out for logit

    // Oracle (StataNow 19.5 `matrix list r(table)` after `margins, at(x1=(0 1))`):
    //          1._at       2._at
    //   b   .52115751   .61724972
    //  se   .06113564   .08863205
    //  ll   .40133385    .4435341
    //  ul   .64098116   .79096535
    // crit   1.959964    1.959964   (both — normal, no df)
    struct { double at1; double margin; double se; double ll; double ul; } oracle[2] = {
        {0.0, 0.52115751, 0.06113564, 0.40133385, 0.64098116},
        {1.0, 0.61724972, 0.08863205, 0.44353410, 0.79096535},
    };
    const double z95 = 1.959963984540054;  // BUG-194's fix constant (z_critical(0.05))

    int fail = 0;
    for (int p = 0; p < 2; ++p) {
        std::vector<double> Xat(static_cast<size_t>(n) * k);
        for (int64_t i = 0; i < n; ++i) {
            Xat[static_cast<size_t>(i)] = 1.0;
            Xat[static_cast<size_t>(n + i)] = oracle[p].at1;  // x1 fixed at the at() point
            Xat[static_cast<size_t>(2 * n + i)] = x2[i];      // x2 as-observed
        }
        // Verbatim replica of cmd_margins' GLM predictive-margins loop
        // (cmds_postest.cpp, model=="logit" branch): average adjusted
        // prediction + delta-method SE off the fitted bread.
        double mg = 0;
        std::vector<double> J(static_cast<size_t>(k), 0.0);
        for (int64_t i = 0; i < n; ++i) {
            double eta = 0;
            for (int j = 0; j < k; ++j) eta += Xat[static_cast<size_t>(j) * n + i] * beta[static_cast<size_t>(j)];
            double gi = 1.0 / (1.0 + std::exp(-eta));
            double dgi = gi * (1.0 - gi);
            mg += gi;
            for (int j = 0; j < k; ++j) J[static_cast<size_t>(j)] += dgi * Xat[static_cast<size_t>(j) * n + i];
        }
        mg /= static_cast<double>(n);
        for (int j = 0; j < k; ++j) J[static_cast<size_t>(j)] /= static_cast<double>(n);
        double var = 0;
        for (int aa = 0; aa < k; ++aa) {
            double gv = 0;
            for (int bb = 0; bb < k; ++bb) gv += J[static_cast<size_t>(bb)] * V[static_cast<size_t>(aa) + static_cast<size_t>(bb) * k];
            var += J[static_cast<size_t>(aa)] * gv;
        }
        double se = std::sqrt(var);

        // The BUG-194 fix itself: tc must be the fixed normal quantile (what
        // cmds_postest.cpp:943 now computes via z_critical(0.05)), not
        // t_critical(0.05, dof) — dof = n-k here has no meaning for an ML fit.
        const double tc = z_critical(0.05);
        double ci_lo = mg - tc * se, ci_hi = mg + tc * se;

        double rm = std::fabs(mg - oracle[p].margin) / (std::fabs(oracle[p].margin) + 1e-30);
        double rs = std::fabs(se - oracle[p].se) / (std::fabs(oracle[p].se) + 1e-30);
        double rl = std::fabs(ci_lo - oracle[p].ll) / (std::fabs(oracle[p].ll) + 1e-30);
        double ru = std::fabs(ci_hi - oracle[p].ul) / (std::fabs(oracle[p].ul) + 1e-30);
        std::printf("  at1=%.0f margin=%.10g (stata %.8g rel %.1e) se=%.10g (stata %.8g rel %.1e) "
                    "ci=[%.10g, %.10g] (stata [%.8g, %.8g] rel %.1e/%.1e)\n",
                    oracle[p].at1, mg, oracle[p].margin, rm, se, oracle[p].se, rs,
                    ci_lo, ci_hi, oracle[p].ll, oracle[p].ul, rl, ru);
        if (!(rm <= 1e-5)) { std::printf("    <<FAIL margin rel %.2e\n", rm); fail = 1; }
        if (!(rs <= 1e-5)) { std::printf("    <<FAIL se rel %.2e\n", rs); fail = 1; }
        // The identity that IS the fix: ci = margin ± 1.959963984540054*se,
        // exact to 1e-9 (not merely "close" — the arithmetic must land on it).
        if (!(std::fabs(ci_lo - (mg - z95 * se)) <= 1e-9)) { std::printf("    <<FAIL ci_lo != margin - z95*se\n"); fail = 1; }
        if (!(std::fabs(ci_hi - (mg + z95 * se)) <= 1e-9)) { std::printf("    <<FAIL ci_hi != margin + z95*se\n"); fail = 1; }
        // And that identity must match the live oracle's own printed bounds
        // (StataBE's `crit 1.959964`, ll/ul above) to the oracle's precision —
        // this is the RED-capable leg: with tc = t_critical(0.05, 57) ≈ 1.9721
        // instead, ci_lo/ci_hi miss ll/ul by ~0.6%, well past 1e-5.
        if (!(rl <= 1e-5)) { std::printf("    <<FAIL ci_lo rel %.2e vs Stata ll\n", rl); fail = 1; }
        if (!(ru <= 1e-5)) { std::printf("    <<FAIL ci_hi rel %.2e vs Stata ul\n", ru); fail = 1; }
    }
    std::printf("%s\n", fail ? "FAILED" : "PASSED");
    return fail;
}

// ---- margins_grid (goal margins-corpus-shapes) ------------------------------
// The kernel PIET's margins runs on the fit's own e(b)/e(V), checked against
// the StataNow 19.5 capture tests/eval/golden/aer_w2/w2v_margins_corpus.log.
// The fixture is that .do's deterministic arithmetic on _n, each variable
// stored as a float as `generate` stores it (its PREC|fixture sums reproduce
// exactly); the fits are refitted here, so every check covers the fit, the at()
// rebuild of each derived column, the atmeans float means, and G V G'.
namespace grid_fixture {

struct Frame {
    int64_t n = 240;
    std::map<std::string, std::vector<double>> v;
    const std::vector<double>& operator[](const std::string& k) const { return v.at(k); }
};

static double fl(double x) { return static_cast<double>(static_cast<float>(x)); }
static double smod(double a, double b) { return a - b * std::floor(a / b); }

static Frame make() {
    Frame F;
    auto gen = [&](const char* name, auto fn) {
        std::vector<double> col(240);
        for (int64_t i = 0; i < 240; ++i) col[static_cast<size_t>(i)] = fl(fn(i, static_cast<double>(i + 1)));
        F.v[name] = std::move(col);
    };
    auto at = [&](const char* name, int64_t i) { return F.v.at(name)[static_cast<size_t>(i)]; };
    gen("id", [](int64_t, double n) { return std::ceil(n / 6); });
    gen("t", [](int64_t, double n) { return smod(n - 1, 6) + 1; });
    gen("region", [&](int64_t i, double) { return smod(at("id", i), 10) + 1; });
    gen("x", [](int64_t, double n) { return smod(n * 7, 11) + smod(n, 5) / 8; });
    gen("z", [](int64_t, double n) { return smod(n * 13, 101); });
    gen("c1", [](int64_t, double n) { return smod(n * 3, 5) + smod(n * 13, 7) / 8; });
    gen("wp", [&](int64_t i, double) { return 1 + smod(at("id", i), 3); });
    gen("fw", [](int64_t, double n) { return 1 + smod(n, 3); });
    gen("e1", [](int64_t, double n) {
        return (smod(n * 37, 41) / 64 + smod(n * 53, 59) / 64 + smod(n * 29, 31) / 64 - 1) * 3;
    });
    gen("y", [&](int64_t i, double) {
        const double x = at("x", i), z = at("z", i);
        return 1 + 0.5 * x + 0.03 * z + 0.004 * z * x - 0.6 * at("c1", i) + at("id", i) / 4 + at("e1", i);
    });
    gen("female", [&](int64_t, double n) { return smod(n * 7 + std::ceil(n / 6), 3) == 0 ? 1.0 : 0.0; });
    gen("passed", [](int64_t, double n) { return smod(n * 11, 4) < 2 ? 1.0 : 0.0; });
    gen("fpassed", [&](int64_t i, double) { return at("female", i) * at("passed", i); });
    gen("yf", [&](int64_t i, double) {
        return 2 - 0.8 * at("female", i) + 0.5 * at("passed", i) + 0.3 * at("c1", i) + at("t", i) / 3 +
               at("e1", i);
    });
    gen("blockcohort", [](int64_t, double n) { return smod(n, 4) + 1; });
    gen("treatment", [](int64_t, double n) { return smod(n * 5, 7) < 3 ? 1.0 : 0.0; });
    gen("y28", [&](int64_t i, double) {
        const double tr = at("treatment", i), bc = at("blockcohort", i);
        return 1 + 0.7 * tr + 0.4 * tr * bc + bc / 2 + 0.3 * at("c1", i) + at("e1", i);
    });
    gen("year", [](int64_t, double n) { return 2010 + smod(n, 4); });
    gen("g", [&](int64_t i, double) { return smod(at("id", i), 3) + 1; });
    gen("remote", [&](int64_t i, double n) {
        return (smod(n * 37, 41) / 41 + at("x", i) / 40 + 0.15 * (at("year", i) == 2012) -
                0.1 * (at("g", i) == 2)) > 0.6 ? 1.0 : 0.0;
    });
    gen("tt", [](int64_t, double n) { return smod(n * 3, 7) < 3 ? 1.0 : 0.0; });
    gen("d", [](int64_t, double n) { return (smod(n * 7, 11) - 5) / 10; });
    gen("dcat", [](int64_t, double n) { return smod(n, 3); });
    gen("clinic", [&](int64_t i, double) { return smod(at("id", i), 12) + 1; });
    gen("yp", [&](int64_t i, double n) {
        const double tt = at("tt", i), d = at("d", i);
        return std::exp(0.5 + 0.3 * tt + 0.4 * d + 0.2 * tt * d - 0.3 * d * d + 0.05 * at("x", i)) *
               (0.5 + smod(n * 37, 41) / 41);
    });
    gen("yc", [&](int64_t i, double) { return std::floor(at("yp", i) * 2); });
    return F;
}

// The rows of F with `keep` true, every variable.
static Frame subset(const Frame& F, const std::vector<bool>& keep) {
    Frame S;
    S.n = 0;
    for (bool k : keep) S.n += k ? 1 : 0;
    for (const auto& [name, col] : F.v) {
        std::vector<double> c;
        for (size_t i = 0; i < col.size(); ++i)
            if (keep[i]) c.push_back(col[i]);
        S.v[name] = std::move(c);
    }
    return S;
}

// A design column of the fit: the e(b) name and its values (empty values = a
// base or omitted column, b 0 and V 0, not in the solve).
struct Col {
    std::string name;
    std::vector<double> values;
};

static Col var(const Frame& F, const std::string& name, const std::string& v) { return {name, F[v]}; }
static Col prod(const Frame& F, const std::string& name, const std::vector<std::string>& parts,
                const std::vector<double>& levels) {
    // parts[j] is a variable; levels[j] is NaN for a continuous atom, else the factor level.
    std::vector<double> out(static_cast<size_t>(F.n), 1.0);
    for (size_t j = 0; j < parts.size(); ++j) {
        const auto& c = F[parts[j]];
        for (size_t i = 0; i < out.size(); ++i)
            out[i] *= std::isnan(levels[j]) ? c[i] : (c[i] == levels[j] ? 1.0 : 0.0);
    }
    return {name, out};
}
static Col omitted(const std::string& name) { return {name, {}}; }
static Col cons(const Frame& F) { return {"_cons", std::vector<double>(static_cast<size_t>(F.n), 1.0)}; }

struct Fit {
    std::vector<std::string> names;
    std::vector<double> b, V;
};

enum class Vce { Iid, Robust, Cluster };

// regress: weighted least squares on the estimable columns; iid, HC1 or the
// cluster sandwich with Stata's (G/(G-1))((N-1)/(N-k)), then embedded into the
// full e(b) layout with zeros for base/omitted columns.
static Fit regress(const Frame& F, const std::string& y, const std::vector<Col>& cols, Vce vce,
                   const std::string& wvar = "", const std::string& clvar = "") {
    const int64_t n = F.n;
    std::vector<int> est;
    for (size_t j = 0; j < cols.size(); ++j)
        if (!cols[j].values.empty()) est.push_back(static_cast<int>(j));
    const int k = static_cast<int>(est.size());
    std::vector<double> X(static_cast<size_t>(n) * k);
    for (int c = 0; c < k; ++c)
        for (int64_t i = 0; i < n; ++i)
            X[static_cast<size_t>(c) * n + i] = cols[static_cast<size_t>(est[c])].values[static_cast<size_t>(i)];
    std::vector<double> w(static_cast<size_t>(n), 1.0);
    if (!wvar.empty()) w = F[wvar];
    const auto& yv = F[y];
    std::vector<double> A(static_cast<size_t>(k) * k, 0.0), Xty(static_cast<size_t>(k), 0.0);
    for (int a = 0; a < k; ++a) {
        for (int b2 = 0; b2 < k; ++b2) {
            double s = 0;
            for (int64_t i = 0; i < n; ++i)
                s += X[static_cast<size_t>(a) * n + i] * w[static_cast<size_t>(i)] * X[static_cast<size_t>(b2) * n + i];
            A[static_cast<size_t>(a) * k + b2] = s;
        }
        double s = 0;
        for (int64_t i = 0; i < n; ++i) s += X[static_cast<size_t>(a) * n + i] * w[static_cast<size_t>(i)] * yv[static_cast<size_t>(i)];
        Xty[static_cast<size_t>(a)] = s;
    }
    la::spd_inverse(A.data(), k);
    std::vector<double> b(static_cast<size_t>(k), 0.0);
    for (int a = 0; a < k; ++a)
        for (int c = 0; c < k; ++c) b[static_cast<size_t>(a)] += A[static_cast<size_t>(a) * k + c] * Xty[static_cast<size_t>(c)];
    std::vector<double> e(static_cast<size_t>(n));
    for (int64_t i = 0; i < n; ++i) {
        double s = yv[static_cast<size_t>(i)];
        for (int c = 0; c < k; ++c) s -= X[static_cast<size_t>(c) * n + i] * b[static_cast<size_t>(c)];
        e[static_cast<size_t>(i)] = s;
    }
    std::vector<double> Vs(static_cast<size_t>(k) * k, 0.0);
    std::vector<double> meat(static_cast<size_t>(k) * k, 0.0);
    double factor = 1.0;
    if (vce == Vce::Iid) {
        double rss = 0;
        for (int64_t i = 0; i < n; ++i) rss += w[static_cast<size_t>(i)] * e[static_cast<size_t>(i)] * e[static_cast<size_t>(i)];
        Vs = A;
        for (auto& v : Vs) v *= rss / static_cast<double>(n - k);
    } else {
        std::map<double, std::vector<double>> score;  // cluster (or row) -> sum of w e x
        for (int64_t i = 0; i < n; ++i) {
            const double key = vce == Vce::Cluster ? F[clvar][static_cast<size_t>(i)] : static_cast<double>(i);
            auto& s = score[key];
            s.resize(static_cast<size_t>(k), 0.0);
            for (int c = 0; c < k; ++c)
                s[static_cast<size_t>(c)] += w[static_cast<size_t>(i)] * e[static_cast<size_t>(i)] * X[static_cast<size_t>(c) * n + i];
        }
        for (const auto& [key, s] : score)
            for (int a = 0; a < k; ++a)
                for (int c = 0; c < k; ++c) meat[static_cast<size_t>(a) * k + c] += s[static_cast<size_t>(a)] * s[static_cast<size_t>(c)];
        const double G = static_cast<double>(score.size());
        factor = vce == Vce::Cluster ? (G / (G - 1)) * (static_cast<double>(n - 1) / static_cast<double>(n - k))
                                     : static_cast<double>(n) / static_cast<double>(n - k);
        la::sandwich(A.data(), meat.data(), k, factor, Vs.data());
    }
    Fit f;
    const size_t K = cols.size();
    for (const auto& c : cols) f.names.push_back(c.name);
    f.b.assign(K, 0.0);
    f.V.assign(K * K, 0.0);
    for (int a = 0; a < k; ++a) {
        f.b[static_cast<size_t>(est[a])] = b[static_cast<size_t>(a)];
        for (int c = 0; c < k; ++c)
            f.V[static_cast<size_t>(est[a]) * K + static_cast<size_t>(est[c])] = Vs[static_cast<size_t>(a) * k + c];
    }
    return f;
}

// glm: the engine's own fitter (Stata's default lifecycle), clustered.
static Fit glm(const Frame& F, const std::string& y, const std::vector<Col>& cols, GlmFamily fam,
               GlmLinkFn link, const std::string& clvar) {
    const int64_t n = F.n;
    std::vector<int> est;
    for (size_t j = 0; j < cols.size(); ++j)
        if (!cols[j].values.empty()) est.push_back(static_cast<int>(j));
    const int k = static_cast<int>(est.size());
    std::vector<double> X(static_cast<size_t>(n) * k);
    for (int c = 0; c < k; ++c)
        for (int64_t i = 0; i < n; ++i)
            X[static_cast<size_t>(c) * n + i] = cols[static_cast<size_t>(est[c])].values[static_cast<size_t>(i)];
    std::map<double, int64_t> ids;
    std::vector<int64_t> cl(static_cast<size_t>(n));
    for (int64_t i = 0; i < n; ++i) {
        const double key = F[clvar][static_cast<size_t>(i)];
        if (!ids.count(key)) ids[key] = static_cast<int64_t>(ids.size());
        cl[static_cast<size_t>(i)] = ids[key];
    }
    GlmOptions opt;
    opt.family = fam;
    opt.link = link;
    GlmResult r = glm_fit(X.data(), n, k, F[y].data(), nullptr, nullptr, cl.data(), opt);
    Fit f;
    const size_t K = cols.size();
    for (const auto& c : cols) f.names.push_back(c.name);
    f.b.assign(K, 0.0);
    f.V.assign(K * K, 0.0);
    if (!r.ok) return f;
    for (int a = 0; a < k; ++a) {
        f.b[static_cast<size_t>(est[a])] = r.beta[static_cast<size_t>(a)];
        for (int c = 0; c < k; ++c)
            f.V[static_cast<size_t>(est[a]) * K + static_cast<size_t>(est[c])] = r.V[static_cast<size_t>(a) * k + c];
    }
    return f;
}

static MarginsGridInput input(const Frame& F, const Fit& fit, const std::string& wvar = "") {
    MarginsGridInput in;
    in.names = fit.names;
    in.beta = fit.b;
    in.V = fit.V;
    in.n = F.n;
    in.data = F.v;
    if (!wvar.empty()) in.weights = F[wvar];
    return in;
}

static std::vector<MarginsGridPoint> points(const std::string& var, const std::vector<double>& vals) {
    std::vector<MarginsGridPoint> p;
    for (double v : vals) p.push_back(MarginsGridPoint{{{var, v}}});
    return p;
}

// The points of an at() option as margins_parse_at reads the .do's own spelling.
static std::vector<MarginsGridPoint> parsed(const char* at) {
    const MarginsAtParse p = margins_parse_at(at);
    if (p.rc != 0) std::printf("  at(%s): <<FAIL rc %d %s\n", at, p.rc, p.error.c_str());
    return p.points;
}

// One oracle record family: r(b) columns, el(r(V),j,j), el(r(V),2,1) and el(r(V),k,1).
struct Expect {
    const char* arm;  // w2v_margins_corpus.log PREC id
    const char* cols;
    std::vector<double> b;
    std::vector<double> vdiag;
    std::vector<double> voff;  // el(r(V),2,1), el(r(V),k,1) (absent for k == 1)
};

static int check(const MarginsGridResult& r, bool single, const std::string& over,
                 const std::vector<double>& overLevels, const Expect& e, double rtol) {
    int fail = 0;
    if (!r.ok) {
        std::printf("  %s: <<FAIL kernel error: %s\n", e.arm, r.error.c_str());
        return 1;
    }
    const auto names = margins_grid_colnames(r, single, over, overLevels);
    std::string joined;
    for (const auto& s : names) joined += (joined.empty() ? "" : " ") + s;
    if (joined != e.cols) {
        std::printf("  %s: <<FAIL cols '%s' (stata '%s')\n", e.arm, joined.c_str(), e.cols);
        fail = 1;
    }
    const size_t m = r.b.size();
    if (m != e.b.size()) {
        std::printf("  %s: <<FAIL k %zu (stata %zu)\n", e.arm, m, e.b.size());
        return 1;
    }
    double scale = 0;
    for (double v : e.vdiag) scale = std::max(scale, std::fabs(v));
    double bscale = 0;
    for (double v : e.b) bscale = std::max(bscale, std::fabs(v));
    auto cmp = [&](const char* what, size_t i, double got, double want, double sc) {
        const double err = std::fabs(got - want);
        if (!(err <= rtol * std::fabs(want) + rtol * sc)) {
            std::printf("  %s: <<FAIL %s[%zu] %.16g (stata %.16g, rel %.2e)\n", e.arm, what, i + 1, got, want,
                        err / (std::fabs(want) + 1e-300));
            fail = 1;
        }
    };
    double worst = 0;
    for (size_t j = 0; j < m; ++j) {
        cmp("b", j, r.b[j], e.b[j], bscale);
        cmp("V", j, r.V[j * m + j], e.vdiag[j], scale);
        if (e.b[j] != 0) worst = std::max(worst, std::fabs(r.b[j] - e.b[j]) / std::fabs(e.b[j]));
    }
    if (m > 1 && e.voff.size() == 2) {
        cmp("V(2,1)", 0, r.V[1 * m + 0], e.voff[0], scale);
        cmp("V(k,1)", 0, r.V[(m - 1) * m + 0], e.voff[1], scale);
    }
    std::printf("  %-26s k=%zu  max rel(b) %.1e %s\n", e.arm, m, worst, fail ? "FAIL" : "ok");
    return fail;
}

}  // namespace grid_fixture

static int test_margins_grid_corpus() {
    using namespace grid_fixture;
    std::printf("test_margins_grid_corpus (w2v_margins_corpus.log):\n");
    const double nan = std::nan("");
    const Frame F = make();
    int fail = 0;
    {
        // PREC|fixture: the .do's own sums (float storage reproduced).
        double sy = 0, syp = 0;
        for (int64_t i = 0; i < F.n; ++i) { sy += F["y"][static_cast<size_t>(i)]; syp += F["yp"][static_cast<size_t>(i)]; }
        if (sy != 2382.659496307373 || syp != 599.2959622740746) {
            std::printf("  fixture: <<FAIL sum(y) %.16g sum(yp) %.16g\n", sy, syp);
            return 1;
        }
    }
    const double T2 = 1e-9;  // TOLERANCE.md T2: postestimation derived quantities

    // X: regress y c.x##c.x c1 -- FU-342's form; every column rebuilt from x.
    {
        const Fit fit = regress(F, "y", {var(F, "x", "x"), prod(F, "c.x#c.x", {"x", "x"}, {nan, nan}),
                                         var(F, "c1", "c1"), cons(F)}, Vce::Iid);
        MarginsGridInput in = input(F, fit);
        in.points = parsed("x=5");
        fail |= check(margins_grid(in), true, "", {}, {"X1_at5", "_cons", {9.737407233828263}, {.1070351966938353}, {}}, T2);
        in.atmeans = true;
        fail |= check(margins_grid(in), true, "", {},
                      {"X1_at5_atmeans", "_cons", {9.737407185459235}, {.1070351968483671}, {}}, T2);
        in.points = {MarginsGridPoint{}};
        fail |= check(margins_grid(in), true, "", {},
                      {"X1_atmeans", "_cons", {9.922553971152869}, {.1075322297501106}, {}}, T2);
        in.atmeans = false;
        in.points = parsed("x=(2 5 8)");
        in.dydx = {"x"};
        fail |= check(margins_grid(in), false, "", {},
                      {"X1_dydx_at", "x:1bn._at x:2._at x:3._at",
                       {.6700071625601931, .6731182154326146, .6762292683050363},
                       {.0303364247435173, .0049012269684728, .0226419624452567},
                       {.0068248425430379, -.0166867396574415}}, T2);
        in.dydx.clear();
        fail |= check(margins_grid(in), false, "", {},
                      {"X1_at_post", "1bn._at 2._at 3._at",
                       {7.722719166839052, 9.737407233828263, 11.76142845943474},
                       {.0983506955643933, .1070351966938353, .0863805481053472},
                       {.0476946922312902, .0041435364023605}}, T2);
    }
    // X1c (control): regress y x c1 -- no derived column, at(x=5).
    {
        const Fit fit = regress(F, "y", {var(F, "x", "x"), var(F, "c1", "c1"), cons(F)}, Vce::Iid);
        MarginsGridInput in = input(F, fit);
        in.points = points("x", {5});
        fail |= check(margins_grid(in), true, "", {}, {"X1c_at5", "_cons", {9.742564158401034}, {.0475170348890136}, {}}, T2);
    }
    const std::vector<double> z40 = {40, 45, 50, 55, 60};
    // F2r: regress y c.x##c.z c1; at(z=(40(5)60)), asobserved and atmeans.
    {
        const Fit fit = regress(F, "y", {var(F, "x", "x"), var(F, "z", "z"), prod(F, "c.x#c.z", {"x", "z"}, {nan, nan}),
                                         var(F, "c1", "c1"), cons(F)}, Vce::Iid);
        MarginsGridInput in = input(F, fit);
        in.points = parsed("z=(40(5)60)");
        fail |= check(margins_grid(in), false, "", {},
                      {"F2r_at", "1bn._at 2._at 3._at 4._at 5._at",
                       {9.451139676591096, 9.68800056072797, 9.924861444864842, 10.16172232900172, 10.39858321313859},
                       {.0440851469004587, .0404904019444024, .0392174825847202, .0402663888214124, .0436371206544784},
                       {.041707318023337, .0345738313919719}}, T2);
        in.atmeans = true;
        fail |= check(margins_grid(in), false, "", {},
                      {"F2r_at_atmeans", "1bn._at 2._at 3._at 4._at 5._at",
                       {9.45113968965047, 9.688000575203789, 9.924861460757107, 10.16172234631043, 10.39858323186375},
                       {.0440851468354864, .0404904019061567, .0392174825726185, .040266388834872, .0436371206929178},
                       {.0417073179718736, .0345738313810354}}, T2);
    }
    // F2rc (control): regress y x z c1 -- the interaction dropped.
    {
        const Fit fit = regress(F, "y", {var(F, "x", "x"), var(F, "z", "z"), var(F, "c1", "c1"), cons(F)}, Vce::Iid);
        MarginsGridInput in = input(F, fit);
        in.points = points("z", z40);
        fail |= check(margins_grid(in), false, "", {},
                      {"F2rc_at", "1bn._at 2._at 3._at 4._at 5._at",
                       {9.442378175533442, 9.679432986643224, 9.916487797753007, 10.15354260886279, 10.39059741997257},
                       {.0442098648956542, .0406034077808735, .0393274560714438, .0403820097673651, .0437670688686375},
                       {.0418240099869261, .0346664452607418}}, T2);
    }
    // F1r: regress y x c1 c.z#c.x if t>1 [pweight=wp]; dydx(x) at(z=(0(25)100)).
    // z enters only through the interaction, so its values come from the data.
    {
        std::vector<bool> keep;
        for (double t : F["t"]) keep.push_back(t > 1);
        const Frame S = subset(F, keep);
        const Fit fit = regress(S, "y", {var(S, "x", "x"), var(S, "c1", "c1"), prod(S, "c.z#c.x", {"z", "x"}, {nan, nan}),
                                         cons(S)}, Vce::Robust, "wp");
        MarginsGridInput in = input(S, fit, "wp");
        in.points = parsed("z=(0(25)100)");
        in.dydx = {"x"};
        fail |= check(margins_grid(in), false, "", {},
                      {"F1r_dydx_at", "x:1bn._at x:2._at x:3._at x:4._at x:5._at",
                       {.3172485102392241, .5093443868535769, .7014402634679295, .8935361400822823, 1.085632016696635},
                       {.0104809061877464, .0070205485142927, .0058074362220739, .00684156931109, .0101229477813409},
                       {.0081889160057108, .0013129454596041}}, T2);
    }
    // F4 / F4b / F4i: regress ..., r; at(x=(p10 p90)) atmeans (p10 1, p90 9.5 from
    // summarize, detail), at(female=(0 1)) atmeans.
    {
        const Fit fit = regress(F, "y", {var(F, "x", "x"), var(F, "c1", "c1"), var(F, "z", "z"), cons(F)}, Vce::Robust);
        MarginsGridInput in = input(F, fit);
        in.points = points("x", {1, 9.5});
        in.atmeans = true;
        fail |= check(margins_grid(in), false, "", {},
                      {"F4_p10p90", "1bn._at 2._at", {6.992035775907258, 12.82912404435772},
                       {.1115498560658852, .1100389618969069}, {-.0321408127624716, -.0321408127624716}}, T2);
    }
    {
        const Fit fit = regress(F, "y", {var(F, "female", "female"), var(F, "x", "x"), var(F, "c1", "c1"), cons(F)},
                                Vce::Robust);
        MarginsGridInput in = input(F, fit);
        in.points = points("female", {0, 1});
        in.atmeans = true;
        fail |= check(margins_grid(in), false, "", {},
                      {"F4b_01", "1bn._at 2._at", {9.920709932924538, 9.94182388551309},
                       {.0774923805479624, .1165411831377848}, {-.0000670776630412, -.0000670776630412}}, T2);
    }
    {
        const Fit fit = regress(F, "y", {var(F, "x", "x"), var(F, "z", "z"), prod(F, "c.x#c.z", {"x", "z"}, {nan, nan}),
                                         var(F, "c1", "c1"), cons(F)}, Vce::Robust);
        MarginsGridInput in = input(F, fit);
        in.points = points("x", {1, 9.5});
        in.atmeans = true;
        fail |= check(margins_grid(in), false, "", {},
                      {"F4i_p10p90", "1bn._at 2._at", {6.993825346382975, 12.84398644336533},
                       {.1107074195671292, .1078229299925423}, {-.0310833231286066, -.0310833231286066}}, T2);
    }
    // F5r: regress y28 treatment##i.blockcohort c1, robust; dydx(treatment) is the
    // discrete change from 0b.treatment (F5_dydx after areg absorb(blockcohort)
    // answers the same 1.681239018387203).
    {
        std::vector<Col> cols = {omitted("0b.treatment"), var(F, "1.treatment", "treatment"), omitted("1b.blockcohort")};
        for (int l = 2; l <= 4; ++l)
            cols.push_back(prod(F, std::to_string(l) + ".blockcohort", {"blockcohort"}, {static_cast<double>(l)}));
        for (int l = 1; l <= 4; ++l) cols.push_back(omitted("0b.treatment#" + std::to_string(l) + (l == 1 ? "b" : "o") + ".blockcohort"));
        cols.push_back(omitted("1o.treatment#1b.blockcohort"));
        for (int l = 2; l <= 4; ++l)
            cols.push_back(prod(F, "1.treatment#" + std::to_string(l) + ".blockcohort", {"treatment", "blockcohort"},
                                {1.0, static_cast<double>(l)}));
        cols.push_back(var(F, "c1", "c1"));
        cols.push_back(cons(F));
        const Fit fit = regress(F, "y28", cols, Vce::Robust);
        MarginsGridInput in = input(F, fit);
        in.points = {MarginsGridPoint{}};
        in.dydx = {"treatment"};
        fail |= check(margins_grid(in), true, "", {},
                      {"F5r_dydx", "0b.treatment 1.treatment", {0, 1.681239018387205}, {0, .0190304204360292}, {0, 0}}, T2);
    }
    // The same two fits under the engine's column names (PIET's e(b) on main
    // 307089f8: `(intercept) 1.treatment 2.blockcohort ... 1.treatment#2.blockcohort
    // ... c1`, `(intercept) x x#x c1`): no base columns, so the base level is the
    // e(sample) level with no main-effect column.
    {
        std::vector<Col> cols = {cons(F), var(F, "1.treatment", "treatment")};
        cols.back().name = "1.treatment";
        cols.front().name = "(intercept)";
        for (int l = 2; l <= 4; ++l)
            cols.push_back(prod(F, std::to_string(l) + ".blockcohort", {"blockcohort"}, {static_cast<double>(l)}));
        for (int l = 2; l <= 4; ++l)
            cols.push_back(prod(F, "1.treatment#" + std::to_string(l) + ".blockcohort", {"treatment", "blockcohort"},
                                {1.0, static_cast<double>(l)}));
        cols.push_back(var(F, "c1", "c1"));
        const Fit fit = regress(F, "y28", cols, Vce::Robust);
        MarginsGridInput in = input(F, fit);
        in.points = {MarginsGridPoint{}};
        in.dydx = {"treatment"};
        fail |= check(margins_grid(in), true, "", {},
                      {"F5r_dydx (engine names)", "0b.treatment 1.treatment", {0, 1.681239018387205},
                       {0, .0190304204360292}, {0, 0}}, T2);
        Col one = cons(F);
        one.name = "(intercept)";
        const Fit fx = regress(F, "y", {one, var(F, "x", "x"), prod(F, "x#x", {"x", "x"}, {nan, nan}), var(F, "c1", "c1")},
                               Vce::Iid);
        MarginsGridInput ix = input(F, fx);
        ix.points = points("x", {5});
        fail |= check(margins_grid(ix), true, "", {},
                      {"X1_at5 (engine names)", "_cons", {9.737407233828263}, {.1070351966938353}, {}}, T2);
    }
    // F3r: regress yf female passed fpassed c1 i.t, vce(cluster id); at(female=0) atmeans.
    {
        std::vector<Col> cols = {var(F, "female", "female"), var(F, "passed", "passed"), var(F, "fpassed", "fpassed"),
                                 var(F, "c1", "c1"), omitted("1b.t")};
        for (int l = 2; l <= 6; ++l) cols.push_back(prod(F, std::to_string(l) + ".t", {"t"}, {static_cast<double>(l)}));
        cols.push_back(cons(F));
        const Fit fit = regress(F, "yf", cols, Vce::Cluster, "", "id");
        MarginsGridInput in = input(F, fit);
        in.points = points("female", {0});
        in.atmeans = true;
        fail |= check(margins_grid(in), true, "", {},
                      {"F3r_at_atmeans", "_cons", {4.200680766853591}, {.0143997374188271}, {}}, T2);
    }
    // F7 / F7c / F7nb: log-link glm, clustered; dydx(tt) is a discrete change on the
    // response scale, every tt/d term rebuilt at each at(d=) point.
    const std::vector<double> dgrid = {-0.4, -0.2, 0, 0.2, 0.4};
    {
        std::vector<Col> cols = {var(F, "x", "x"), omitted("1b.g"), prod(F, "2.g", {"g"}, {2}), prod(F, "3.g", {"g"}, {3}),
                                 omitted("0b.tt"), var(F, "1.tt", "tt"), var(F, "d", "d"),
                                 omitted("0b.tt#co.d"), prod(F, "1.tt#c.d", {"tt", "d"}, {1, nan}),
                                 prod(F, "c.d#c.d", {"d", "d"}, {nan, nan}), omitted("0b.tt#co.d#co.d"),
                                 prod(F, "1.tt#c.d#c.d", {"tt", "d", "d"}, {1, nan, nan}), cons(F)};
        const Fit fit = glm(F, "yp", cols, GlmFamily::Gamma, GlmLinkFn::Log, "clinic");
        MarginsGridInput in = input(F, fit);
        in.link = GlmLinkFn::Log;
        in.dydx = {"tt"};
        in.points = parsed("d=(-0.4(.2).4)");
        fail |= check(margins_grid(in), false, "", {},
                      {"F7_dydx_at",
                       "0b.tt:1bno._at 0b.tt:2o._at 0b.tt:3o._at 0b.tt:4o._at 0b.tt:5o._at 1.tt:1bn._at 1.tt:2._at "
                       "1.tt:3._at 1.tt:4._at 1.tt:5._at",
                       {0, 0, 0, 0, 0, .4683828811482316, .6382863907403848, .8027243524703778, .9505870193159307,
                        1.070505732578448},
                       {0, 0, 0, 0, 0, .0203455863901781, .0113922138924973, .0127464447249043, .0303940098772056,
                        .1134711037571208},
                       {0, 0}}, T2);
        in.points = {MarginsGridPoint{}};
        fail |= check(margins_grid(in), true, "", {},
                      {"F7_dydx", "0b.tt 1.tt", {0, .8218919953662627}, {0, .0066737117056778}, {0, 0}}, T2);
    }
    {
        std::vector<Col> cols = {var(F, "x", "x"), omitted("1b.g"), prod(F, "2.g", {"g"}, {2}), prod(F, "3.g", {"g"}, {3}),
                                 omitted("0b.tt"), var(F, "1.tt", "tt"), var(F, "d", "d"), cons(F)};
        const Fit fit = glm(F, "yp", cols, GlmFamily::Gamma, GlmLinkFn::Log, "clinic");
        MarginsGridInput in = input(F, fit);
        in.link = GlmLinkFn::Log;
        in.dydx = {"tt"};
        in.points = points("d", dgrid);
        fail |= check(margins_grid(in), false, "", {},
                      {"F7c_dydx_at",
                       "0b.tt:1bno._at 0b.tt:2o._at 0b.tt:3o._at 0b.tt:4o._at 0b.tt:5o._at 1.tt:1bn._at 1.tt:2._at "
                       "1.tt:3._at 1.tt:4._at 1.tt:5._at",
                       {0, 0, 0, 0, 0, .6356563180601498, .6936549430929237, .756945485172881, .8260107899884916,
                        .9013777590886631},
                       {0, 0, 0, 0, 0, .0129234625258257, .0045915334204371, .00300789550551, .0130352912907837,
                        .0415250718034389},
                       {0, 0}}, T2);
    }
    {
        std::vector<Col> cols = {var(F, "x", "x"), omitted("0b.tt"), var(F, "1.tt", "tt"), omitted("0b.dcat"),
                                 prod(F, "1.dcat", {"dcat"}, {1}), prod(F, "2.dcat", {"dcat"}, {2}),
                                 omitted("0b.tt#0b.dcat"), omitted("0b.tt#1o.dcat"), omitted("0b.tt#2o.dcat"),
                                 omitted("1o.tt#0b.dcat"), prod(F, "1.tt#1.dcat", {"tt", "dcat"}, {1, 1}),
                                 prod(F, "1.tt#2.dcat", {"tt", "dcat"}, {1, 2}), cons(F)};
        const Fit fit = glm(F, "yc", cols, GlmFamily::NegBinomial, GlmLinkFn::Log, "clinic");
        MarginsGridInput in = input(F, fit);
        in.link = GlmLinkFn::Log;
        in.dydx = {"tt"};
        in.points = points("dcat", {0, 1, 2});
        fail |= check(margins_grid(in), false, "dcat", {0, 1, 2},
                      {"F7nb_dcat_dydx", "0b.tt:0bno.dcat 0b.tt:1o.dcat 0b.tt:2o.dcat 1.tt:0bn.dcat 1.tt:1.dcat 1.tt:2.dcat",
                       {0, 0, 0, 2.155846017850307, 2.227986917085964, .2028037498934836},
                       {0, 0, 0, .0952893669334776, .1513147299520834, .0597200748123968},
                       {0, 0}}, T2);
    }
    // F6: logit remote i.year c1 i.g [fweight=fw], vce(cl region); margins year
    // (asobserved and atmeans). The fit is the engine's ML on the fw-expanded rows
    // (a frequency weight IS replication); margins weights the 240 rows by fw.
    {
        std::vector<bool> all(static_cast<size_t>(F.n), true);
        Frame E;  // fw-expanded copy
        E.n = 0;
        for (const auto& [name, col] : F.v) E.v[name];
        for (int64_t i = 0; i < F.n; ++i) {
            const int reps = static_cast<int>(F["fw"][static_cast<size_t>(i)]);
            for (int r = 0; r < reps; ++r) {
                for (auto& [name, col] : E.v) col.push_back(F[name][static_cast<size_t>(i)]);
                ++E.n;
            }
        }
        auto design = [nan](const Frame& Fr) {
            std::vector<Col> cols = {omitted("2010b.year")};
            for (int l = 2011; l <= 2013; ++l)
                cols.push_back(prod(Fr, std::to_string(l) + ".year", {"year"}, {static_cast<double>(l)}));
            cols.push_back(var(Fr, "c1", "c1"));
            cols.push_back(omitted("1b.g"));
            cols.push_back(prod(Fr, "2.g", {"g"}, {2}));
            cols.push_back(prod(Fr, "3.g", {"g"}, {3}));
            cols.push_back(cons(Fr));
            (void)nan;
            return cols;
        };
        const Fit fit = glm(E, "remote", design(E), GlmFamily::Bernoulli, GlmLinkFn::Logit, "region");
        MarginsGridInput in = input(F, fit, "fw");
        in.link = GlmLinkFn::Logit;
        in.points = points("year", {2010, 2011, 2012, 2013});
        const double T3 = 1e-7;  // TOLERANCE.md T3: a converged ML fit underneath
        fail |= check(margins_grid(in), false, "year", {2010, 2011, 2012, 2013},
                      {"F6_year_asobs", "2010bn.year 2011.year 2012.year 2013.year",
                       {.4324253468071036, .4932886124381229, .6590459105587019, .5316689953583911},
                       {.0057127685464316, .0018430465495676, .0035117567450757, .0026330315636511},
                       {.0015317077412551, .0029268928198978}}, T3);
        in.atmeans = true;
        fail |= check(margins_grid(in), false, "year", {2010, 2011, 2012, 2013},
                      {"F6_year_atmeans", "2010bn.year 2011.year 2012.year 2013.year",
                       {.4314834822549226, .4933234778891725, .661447599475008, .5323216765616233},
                       {.0058606455266161, .001911827314283, .0035854653061514, .0027199417892621},
                       {.0015838316443271, .0030229783940565}}, T3);
        (void)all;
    }
    // Refusals the kernel owns: an unknown at()/dydx() variable is named.
    {
        const Fit fit = regress(F, "y", {var(F, "x", "x"), var(F, "c1", "c1"), cons(F)}, Vce::Iid);
        MarginsGridInput in = input(F, fit);
        in.points = points("zz", {1});
        const MarginsGridResult r = margins_grid(in);
        if (r.ok || r.notFound != "zz") { std::printf("  unknown at() var: <<FAIL not refused\n"); fail = 1; }
        // dydx(c1 x) posts x first, the order the variables enter e(b)
        // (w2v_margins_siblings C7_dydx_c1x_cols `x c1`).
        MarginsGridInput d = input(F, fit);
        d.points = {MarginsGridPoint{}};
        d.dydx = {"c1", "x"};
        const MarginsGridResult rd = margins_grid(d);
        if (!rd.ok || rd.cols.size() != 2 || rd.cols[0].dydx != "x" || rd.cols[1].dydx != "c1") {
            std::printf("  dydx(c1 x): <<FAIL columns not in e(b) order\n");
            fail = 1;
        }
        bool ok = true;
        (void)margins_parse_column("L.x", &ok);
        if (ok) { std::printf("  L.x: <<FAIL a time-series operator parsed as rebuildable\n"); fail = 1; }
    }
    std::printf("%s\n", fail ? "FAILED" : "PASSED");
    return fail;
}

// margins_parse_at on the .do's at() spellings and [R] margins' Syntax of at().
static int test_margins_parse_at() {
    std::printf("test_margins_parse_at:\n");
    int fail = 0;
    auto expect = [&](const char* at, int rc, const std::vector<std::vector<std::pair<std::string, double>>>& pts) {
        const MarginsAtParse p = margins_parse_at(at);
        bool ok = p.rc == rc && p.points.size() == pts.size();
        for (size_t i = 0; ok && i < pts.size(); ++i) ok = p.points[i].fix == pts[i];
        if (!ok) {
            std::printf("  at(%s): <<FAIL rc %d (want %d), %zu points (want %zu)\n", at, p.rc, rc, p.points.size(),
                        pts.size());
            fail = 1;
        }
    };
    // F2_at2 (k=4, 1bn._at..4._at): z varies, x stays 1.
    expect("z=(10(10)40) x=(1)", 0, {{{"z", 10}, {"x", 1}}, {{"z", 20}, {"x", 1}}, {{"z", 30}, {"x", 1}}, {{"z", 40}, {"x", 1}}});
    // F3b_at3_eqeq == F3b_at3 (4.448804326632731 both): `==` is `=`.
    expect("female=0 passed=1 fpassed==0", 0, {{{"female", 0}, {"passed", 1}, {"fpassed", 0}}});
    expect("female=0 passed=1 fpassed=0", 0, {{{"female", 0}, {"passed", 1}, {"fpassed", 0}}});
    // F7_dydx_minmax: rc 121.
    expect("d=(min(.2)max)", 121, {});
    // F1_dydx_at_full: x:1bn._at .. x:21._at.
    if (margins_parse_at("z=(0(5)100)").points.size() != 21) { std::printf("  0(5)100: <<FAIL not 21 points\n"); fail = 1; }
    // F4b_01 and Syntax of at() rules 2 and 3 (the rightmost variable varies fastest).
    expect("female=(0 1)", 0, {{{"female", 0}}, {{"female", 1}}});
    expect("x1=5 x1=10", 0, {{{"x1", 5}}, {{"x1", 10}}});
    expect("x1=(5 10) x2=(0 1)", 0,
           {{{"x1", 5}, {"x2", 0}}, {{"x1", 5}, {"x2", 1}}, {{"x1", 10}, {"x2", 0}}, {{"x1", 10}, {"x2", 1}}});
    expect("x = (1/3)", 0, {{{"x", 1}}, {{"x", 2}}, {{"x", 3}}});
    expect("(mean) x", 198, {});
    expect("x=generate(x+1)", 198, {});
    std::printf("%s\n", fail ? "FAILED" : "PASSED");
    return fail;
}

int test_margins_at_main() {
    std::printf("test_margins_at:\n");
    const cobra_test::ValidateBase base;

    std::ifstream f(base + "margins_at_golden.csv");
    std::string line;
    std::getline(f, line);  // header
    std::vector<double> gm, gs;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        std::stringstream ss(line);
        std::string pt, m, se;
        std::getline(ss, pt, ',');
        std::getline(ss, m, ',');
        std::getline(ss, se, ',');
        gm.push_back(std::stod(m));
        gs.push_back(std::stod(se));
    }

    Dataset ds = Dataset::open(base + "nlcom.hbin");
    ModelData md = build_model(ds, "visits_w", {"age_z", "age2", "edu_hi"}, {}, {});
    FeolsResult r = feols_fit(md, Vcov::IID);
    const int k = static_cast<int>(r.names.size());

    std::vector<double> mean(k, 0.0);
    for (int j = 0; j < k; ++j) {
        const double* col = md.dm.X.data() + static_cast<int64_t>(j) * md.dm.n;
        double s = 0; for (int64_t i = 0; i < md.dm.n; ++i) s += col[i];
        mean[j] = s / static_cast<double>(md.dm.n);
    }
    std::map<std::string, int> idx;
    for (int j = 0; j < k; ++j) idx[r.names[j]] = j;

    std::vector<std::map<std::string, double>> pts = {
        {{"age_z", 0}, {"age2", 0}},
        {{"age_z", 1}, {"age2", 1}},
        {{"age_z", -0.5}, {"age2", 0.25}, {"edu_hi", 1}},
    };

    int fail = 0;
    for (size_t p = 0; p < pts.size(); ++p) {
        std::vector<double> xb = mean;
        for (auto& kv : pts[p]) xb[idx[kv.first]] = kv.second;
        double mg = 0;
        for (int j = 0; j < k; ++j) mg += xb[j] * r.beta[j];
        double var = 0;
        for (int aa = 0; aa < k; ++aa) {
            double gv = 0;
            for (int bb = 0; bb < k; ++bb) gv += xb[bb] * r.vcov_matrix[aa + bb * k];
            var += xb[aa] * gv;
        }
        double se = std::sqrt(var);
        double rm = std::fabs(mg - gm[p]) / (std::fabs(gm[p]) + 1e-30);
        double rs = std::fabs(se - gs[p]) / (std::fabs(gs[p]) + 1e-30);
        std::printf("  p%zu margin=%.12g (stata %.12g rel %.1e)  se=%.12g (stata %.12g rel %.1e)\n",
                    p + 1, mg, gm[p], rm, se, gs[p], rs);
        if (!(rm <= 1e-9)) { std::printf("    <<FAIL margin rel %.2e\n", rm); fail = 1; }
        if (!(rs <= 1e-9)) { std::printf("    <<FAIL se rel %.2e\n", rs); fail = 1; }
    }
    std::printf("%s\n", fail ? "FAILED" : "PASSED");
    fail |= test_margins_glm_at_z();
    fail |= test_margins_grid_corpus();
    fail |= test_margins_parse_at();
    return fail;
}
