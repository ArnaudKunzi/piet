// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "common.h"
#include "commands.h"

#include "estimators/oglm.h"

// P7 wave — ologit, oprobit (Stata `ologit` / `oprobit`).
//
// Both verbs are the SAME model with a different link, so they share run_oglm()
// below and differ only in the OrdLink they pass and the name they report.
//
// NO INTERCEPT. load_glm returns [expanded x…, intercept] with the constant LAST,
// so dropping it is a pure truncation (g.k -= 1, resize X, pop the name). It has to
// go: with a constant column in the design the information matrix is singular
// against the cutpoints (the model is identified only up to a common shift of xβ
// and every κ), and Stata reports the cutpoints instead of a constant for exactly
// that reason.

namespace cecon {
namespace cli {

namespace {

int run_oglm(const std::vector<std::string>& a, OrdLink link, const char* cmd) {
    std::string data = opt(a, "--data"), y = opt(a, "--y"), xcsv = opt(a, "--x");
    // --vce default "iid": opt() with a default leaves an omitted flag unconsumed,
    // so a plain invocation keeps the model-based (observed-information) SEs.
    std::string vce = opt(a, "--vce", "iid");
    std::string cluster_var = opt(a, "--cluster");
    if (data_missing(data) || y.empty() || xcsv.empty()) {
        std::fprintf(stderr,
                     "usage: cobra %s --data f.hbin --y Y --x X1,X2 [--vce iid|robust|cluster] "
                     "[--cluster C] [--level P] [--keepif EXPR] [--out f.json]\n"
                     "  ordered %s: outcome recoded 0..J-1 by sorted distinct value;\n"
                     "  reports J-1 cutpoints (/cut1 …) instead of a constant.\n"
                     "  --vce cluster            needs --cluster C.\n",
                     cmd, link == OrdLink::Logit ? "logit" : "probit");
        return 2;
    }
    if (vce != "iid" && vce != "robust" && vce != "cluster") {
        std::fprintf(stderr, "%s: --vce must be one of iid|robust|cluster (got %s).\n", cmd, vce.c_str());
        return 2;
    }
    if (vce == "cluster" && cluster_var.empty()) {
        std::fprintf(stderr, "%s: --vce cluster requires --cluster C.\n", cmd);
        return 2;
    }
    bool level_set = has(a, "--level");
    double level = std::stod(opt(a, "--level", "95"));
    if (level_set && (level < 10.0 || level > 99.99)) {
        std::fprintf(stderr, "%s: --level must be in [10, 99.99] (got %s).\n", cmd,
                     opt(a, "--level").c_str());
        return 2;
    }
    // Asymptotic z inference throughout — Stata's ordered models report z, not t.
    const double zc = z_critical(level == 95.0 ? 0.05 : 1.0 - level / 100.0);

    data = maybe_keepif(data, opt(a, "--keepif"));
    const Dataset& ds = acquire_data(data);
    std::vector<std::string> cluster_names;
    if (!cluster_var.empty()) cluster_names.push_back(cluster_var);
    GlmInputs g = load_glm(ds, cmd, y, split_csv(xcsv), cluster_names);
    require_sample(g.n);  // BUG-1269: r(2000) "no observations" / r(2001) for one row
    // Drop the trailing intercept (see the file header). Pure truncation: it is the
    // LAST column of the col-major design, so the surviving bytes do not move.
    if (g.k > 0 && !g.names.empty() && g.names.back() == "(intercept)") {
        g.k -= 1;
        g.X.resize(static_cast<size_t>(g.n) * g.k);
        g.names.pop_back();
    }
    if (g.k == 0)
        throw EngineError("bad_input", std::string(cmd) + ": no covariates left after dropping the "
                                       "intercept; ordered models need at least one regressor.");
    if (vce == "cluster" && (g.cluster.empty() || g.cluster[0].n_levels < 2))
        throw EngineError("bad_input",
            std::string(cmd) + ": vce(cluster) requires at least 2 clusters, but the cluster "
            "variable has " + std::to_string(g.cluster.empty() ? 0 : g.cluster[0].n_levels));

    std::vector<int> cat;
    std::vector<double> cat_values = ord_categories(g.y.data(), g.n, cat);
    const int J = static_cast<int>(cat_values.size());
    if (J < 2)
        throw EngineError("bad_input",
            std::string(cmd) + ": outcome '" + y + "' takes fewer than 2 distinct values.");
    const int np = g.k + J - 1;

    OglmResult r = oglm_fit(g.X.data(), g.n, g.k, cat.data(), J, link);
    if (!r.ok)
        throw EngineError("no_convergence",
            std::string(cmd) + ": MLE failed (singular information matrix or non-convergent fit).");

    // V = bread (iid) or a sandwich bread·meat·bread over the per-observation score
    // rows. bread is oglm_fit's converged (−H)^{-1}; the score rows come back in the
    // SAME parameter order (slopes, then cutpoints), so no re-blocking is needed.
    //
    // SMALL-SAMPLE FACTOR. Stata's ML multiplier, NOT the regression-style
    // (N-1)/(N-K)·G/(G-1) that vcov.cpp applies to feols/2SLS:
    //   vce(robust)  → N/(N-1)
    //   vce(cluster) → G/(G-1)
    // Same P5 precedent as logit/probit/tobit (cmds_mle.cpp:284-297), and VERIFIED
    // here rather than assumed — tests/unit/test_oglm.cpp asserts every SE of the
    // P7 oracle `ologit yord x1 x2 i.region, vce(cluster id)` (200 clusters) and
    // `oprobit yord x1 x2 i.region, vce(robust)` (N=4,671) against these factors and
    // matches to <1e-5 relative; the regression-style factor would shift every
    // cluster SE by (N-1)/(N-K) ≈ 1.0008, an order of magnitude outside that.
    std::vector<double> V = r.vcov;
    int n_clusters = 0;
    if (vce == "robust" || vce == "cluster") {
        std::vector<double> S = oglm_scores(g.X.data(), g.n, g.k, cat.data(), J, link, r);
        int64_t rows = g.n;
        const Factor* fac = nullptr;
        double factor;
        if (vce == "cluster") {
            fac = &g.cluster[0];
            n_clusters = fac->n_levels;
            rows = n_clusters;
            factor = static_cast<double>(n_clusters) / (static_cast<double>(n_clusters) - 1.0);
        } else {
            factor = g.n > 1 ? static_cast<double>(g.n) / (static_cast<double>(g.n) - 1.0) : 1.0;
        }
        std::vector<double> M(static_cast<size_t>(rows) * np, 0.0);
        for (int64_t i = 0; i < g.n; ++i) {
            int64_t rr = fac ? fac->codes[static_cast<size_t>(i)] : i;
            for (int p = 0; p < np; ++p)
                M[static_cast<int64_t>(p) * rows + rr] += S[static_cast<int64_t>(p) * g.n + i];
        }
        std::vector<double> meat(static_cast<size_t>(np) * np, 0.0);
        la::xtx(M.data(), rows, np, meat.data());
        std::vector<double> tmp(static_cast<size_t>(np) * np, 0.0);
        V.assign(static_cast<size_t>(np) * np, 0.0);
        la::sandwich(r.vcov.data(), meat.data(), np, factor, V.data(), tmp.data());
    }
    auto sd = [&](int p) { return std::sqrt(std::max(0.0, V[static_cast<size_t>(p) * np + p])); };

    json coefs = json::array();
    for (int c = 0; c < g.k; ++c) {
        double b = r.beta[static_cast<size_t>(c)], se = sd(c);
        double z = (se > 0) ? b / se : 0.0;
        coefs.push_back({{"name", g.names[static_cast<size_t>(c)]}, {"b", b}, {"se", se}, {"z", z},
                         {"p", (se > 0) ? z_twosided_pvalue(z) : 1.0},
                         {"ci_lo", b - zc * se}, {"ci_hi", b + zc * se}});
    }
    // Cutpoints carry b/se/CI but NO z/p — Stata leaves those cells blank for
    // /cut1 /cut2 (a cutpoint's "= 0" null is not a hypothesis anyone poses).
    json cuts = json::array();
    for (int c = 0; c < J - 1; ++c) {
        double b = r.cuts[static_cast<size_t>(c)], se = sd(g.k + c);
        cuts.push_back({{"name", "/cut" + std::to_string(c + 1)}, {"b", b}, {"se", se},
                        {"ci_lo", b - zc * se}, {"ci_hi", b + zc * se}});
    }
    // Wald χ² over the SLOPES ONLY — the cutpoints are not part of the "all
    // coefficients are zero" null Stata reports (chi2(6) for 6 slopes + 2 cuts).
    double chi2 = 0;
    {
        std::vector<double> Vs(static_cast<size_t>(g.k) * g.k);
        for (int i = 0; i < g.k; ++i)
            for (int j = 0; j < g.k; ++j)
                Vs[static_cast<size_t>(i) * g.k + j] = V[static_cast<size_t>(i) * np + j];
        if (la::spd_inverse(Vs.data(), g.k) == 0)
            for (int i = 0; i < g.k; ++i)
                for (int j = 0; j < g.k; ++j)
                    chi2 += r.beta[static_cast<size_t>(i)] * Vs[static_cast<size_t>(i) * g.k + j] *
                            r.beta[static_cast<size_t>(j)];
    }

    json out = {{"estimator", cmd}, {"n_obs", g.n}, {"n_dropped_missing", g.n_dropped},
                {"n_cats", J}, {"cat_values", cat_values}, {"vce", vce},
                {"coefs", coefs}, {"cuts", cuts},
                {"ll", r.ll}, {"ll_0", r.ll_0},
                {"r2_pseudo", (r.ll_0 != 0) ? 1.0 - r.ll / r.ll_0 : 0.0},
                {"chi2", chi2}, {"df_chi2", g.k}, {"p_chi2", chi2_pvalue(chi2, g.k)},
                {"iters", r.iters}, {"converged", r.converged}};
    if (vce == "cluster") { out["cluster_var"] = cluster_var; out["n_clusters"] = n_clusters; }
    if (level_set) out["level"] = level;
    add_warnings(out, g.warnings);
    // --emit-v (BUG-1196): the fit's V, slopes then cutpoints -- Stata's e(V)
    // order and metric (StataBE 19.5 colfullnames e(b) `yo:x1 yo:x2 /cut1 /cut2`).
    if (has(a, "--emit-v")) {
        std::vector<std::string> names = g.names;
        for (int c = 0; c < J - 1; ++c) names.push_back("/cut" + std::to_string(c + 1));
        add_e_v(out, names, V);
    }
    emit(out, opt(a, "--out"));
    return 0;
}

}  // namespace

int cmd_ologit(const std::vector<std::string>& a) { return run_oglm(a, OrdLink::Logit, "ologit"); }

int cmd_oprobit(const std::vector<std::string>& a) { return run_oglm(a, OrdLink::Probit, "oprobit"); }

}  // namespace cli
}  // namespace cecon
