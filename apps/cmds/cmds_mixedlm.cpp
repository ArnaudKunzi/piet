// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "common.h"
#include "commands.h"

#include "estimators/mixedlm.h"

// P7 wave — `mixed` (Stata `mixed y x1 x2 || GROUP:,`).
//
// RANDOM INTERCEPT ONLY, and by MAXIMUM LIKELIHOOD only. Both restrictions are
// deliberate honest rejects rather than guesses:
//   * REML — Stata's `mixed` default is ML, and REML is a different objective that
//     moves var(_cons) in the third digit. `--reml` is not in this command's options
//     table, so the dispatch layer's consumed-marks net rejects it outright; the
//     usage text says so in words as well.
//   * Random SLOPES (`|| id: x1`) — not oracled in this wave. --group takes a plain
//     variable name and anything else is refused below, so nobody can spell a slope
//     specification and get an intercept-only answer back.
// An honest reject is parity; a silently wrong answer is not.
//
// The group variable rides load_glm's cluster channel, so it joins the SAME single
// listwise-deletion pass as y and the x's and comes back as a dense Factor — a group
// code that disagreed with the estimation sample by even one row would silently
// mis-partition the likelihood.

namespace cecon {
namespace cli {

int cmd_mixed(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), y = opt(a, "--y"), xcsv = opt(a, "--x");
    std::string group = opt(a, "--group");
    // --vce default "iid": opt() with a default leaves an omitted flag unconsumed, so
    // a plain invocation keeps the model-based (observed-information) SEs.
    std::string vce = opt(a, "--vce", "iid");
    std::string cluster_var = opt(a, "--cluster");
    if (data_missing(data) || y.empty() || xcsv.empty() || group.empty()) {
        std::fprintf(stderr,
                     "usage: cobra mixed --data f.hbin --y Y --x X1,X2 --group ID "
                     "[--vce iid|robust|cluster] [--cluster C] [--level P] [--keepif EXPR] "
                     "[--out f.json]\n"
                     "  two-level random-INTERCEPT model fitted by ML: Stata's\n"
                     "    mixed Y X1 X2 || ID:,  [vce(robust)]\n"
                     "  --group ID names the grouping variable (Stata's `|| ID:`).\n"
                     "  --vce robust             cluster-robust by ID, exactly as Stata's\n"
                     "                           mixed vce(robust) is (all 5 parameters).\n"
                     "  --vce cluster            needs --cluster C, and C must be COARSER than\n"
                     "                           ID (every group wholly inside one cluster).\n"
                     "  NOT supported: REML (--reml is refused; Stata's mixed default is ML)\n"
                     "                 and random slopes (|| ID: X) — neither is oracled here.\n");
        return 2;
    }
    // --group is a plain variable name. A factor/interaction/TS expression, or a
    // Stata-style "id: x1" slope spec, is refused rather than silently reinterpreted.
    if (group.find_first_of(" \t:,#()") != std::string::npos ||
        group.find('.') != std::string::npos)
        throw EngineError("unsupported",
            "mixed: --group takes a plain grouping-variable name (got '" + group +
            "'). Random slopes (Stata `|| id: x1`) and factor/TS expressions in the "
            "group specification are not supported in this build.");
    if (vce != "iid" && vce != "robust" && vce != "cluster") {
        std::fprintf(stderr, "mixed: --vce must be one of iid|robust|cluster (got %s).\n", vce.c_str());
        return 2;
    }
    if (vce == "cluster" && cluster_var.empty()) {
        std::fprintf(stderr, "mixed: --vce cluster requires --cluster C.\n");
        return 2;
    }
    if (vce != "cluster" && !cluster_var.empty()) {
        std::fprintf(stderr, "mixed: --cluster is only meaningful with --vce cluster "
                             "(--vce robust already clusters by --group).\n");
        return 2;
    }
    bool level_set = has(a, "--level");
    double level = std::stod(opt(a, "--level", "95"));
    if (level_set && (level < 10.0 || level > 99.99)) {
        std::fprintf(stderr, "mixed: --level must be in [10, 99.99] (got %s).\n",
                     opt(a, "--level").c_str());
        return 2;
    }
    // Asymptotic z inference throughout — Stata's `mixed` reports z, not t.
    const double zc = z_critical(level == 95.0 ? 0.05 : 1.0 - level / 100.0);

    data = maybe_keepif(data, opt(a, "--keepif"));
    const Dataset& ds = acquire_data(data);
    std::vector<std::string> cluster_names;
    cluster_names.push_back(group);  // ALWAYS first — g.cluster[0] is the group Factor
    const bool sep_cluster = (vce == "cluster" && cluster_var != group);
    if (sep_cluster) cluster_names.push_back(cluster_var);
    GlmInputs g = load_glm(ds, "mixed", y, split_csv(xcsv), cluster_names);
    require_sample(g.n);  // BUG-1269: r(2000) "no observations" / r(2001) for one row
    if (g.cluster.empty())
        throw EngineError("bad_input", "mixed: could not build the group factor for '" + group + "'.");
    const Factor& gf = g.cluster[0];
    const int G = gf.n_levels;
    if (G < 2)
        throw EngineError("bad_input",
            "mixed: --group '" + group + "' has " + std::to_string(G) +
            " level(s); a two-level model needs at least 2 groups.");
    if (G >= static_cast<int>(g.n))
        throw EngineError("bad_input",
            "mixed: --group '" + group + "' has one observation per group (" +
            std::to_string(G) + " groups, " + std::to_string(g.n) +
            " observations); σ_u and σ_e are not separately identified.");

    MixedLmResult r = mixedlm_fit(g.X.data(), g.n, g.k, g.y.data(), gf.codes.data(), G);
    if (!r.ok)
        throw EngineError("no_convergence",
            "mixed: ML failed (singular information matrix or non-convergent fit).");

    const int np = g.k + 2;
    // V = bread (iid) or a sandwich bread·meat·bread over the PER-GROUP score rows.
    //
    // ROBUST COVERS ALL FIVE PARAMETERS, not just the fixed effects — Stata prints a
    // `Robust` std. err. column over the random-effects table too under vce(robust).
    //
    // The bread is r.vcov, which is NOT a plain (−H)⁻¹: it is the block-diagonal
    // matrix Stata itself reports as e(V), with the β block inverted marginally and
    // the variance-component block taken from the profiled (full) inverse. See the
    // long comment in src/estimators/mixedlm.cpp for the e(V) measurement that
    // establishes it — a plain (−H)⁻¹ bread misses se(x1) by 0.9% and se(_cons) by
    // 13%, i.e. it is not a rounding-level difference.
    //
    // SMALL-SAMPLE FACTOR G/(G−1), with G the number of GROUPS — the same ML cluster
    // multiplier the P5/P7 precedents use (cmds_mle.cpp, cmds_oglm.cpp), NOT the
    // regression-style (N−1)/(N−K)·G/(G−1). VERIFIED, not assumed: tests/unit/
    // test_mixedlm.cpp asserts se(x1)=.0733897, se(x2)=.0719695, se(_cons)=.0496301
    // and both variance-component SEs of the P7 oracle (200 groups) against exactly
    // this factor and matches to <1e-5 relative; the regression-style factor would
    // multiply every SE by an extra √((N−1)/(N−K)) ≈ 1.0002, and dropping the factor
    // altogether would shrink them by √(199/200) ≈ 0.9975 — both far outside 1e-5.
    std::vector<double> V = r.vcov;
    int n_clusters = 0;
    if (vce == "robust" || vce == "cluster") {
        std::vector<double> S =
            mixedlm_group_scores(g.X.data(), g.n, g.k, g.y.data(), gf.codes.data(), G, r);
        if (S.size() != static_cast<size_t>(G) * np)
            throw EngineError("no_convergence", "mixed: could not form the score matrix.");
        int64_t rows = G;
        std::vector<double> M;
        if (vce == "cluster" && sep_cluster) {
            // Clustering at a level COARSER than the group: the group scores are the
            // independent units, so a coarser cluster meat is the sum of its groups'
            // score rows. That is only meaningful if each group lies wholly inside one
            // cluster; a cluster finer than (or crossed with) the group would break the
            // independence the meat rests on, so it is refused rather than approximated.
            const Factor& cf = g.cluster[1];
            std::vector<int> gmap(static_cast<size_t>(G), -1);
            for (int64_t i = 0; i < g.n; ++i) {
                const int gj = gf.codes[static_cast<size_t>(i)];
                const int cj = cf.codes[static_cast<size_t>(i)];
                if (gmap[static_cast<size_t>(gj)] < 0) gmap[static_cast<size_t>(gj)] = cj;
                else if (gmap[static_cast<size_t>(gj)] != cj)
                    throw EngineError("bad_input",
                        "mixed: --cluster '" + cluster_var + "' is not coarser than --group '" +
                        group + "' (a group spans more than one cluster); vce(cluster) needs the "
                        "groups nested inside the clusters.");
            }
            n_clusters = cf.n_levels;
            rows = n_clusters;
            M.assign(static_cast<size_t>(rows) * np, 0.0);
            for (int j = 0; j < G; ++j)
                for (int p = 0; p < np; ++p)
                    M[static_cast<int64_t>(p) * rows + gmap[static_cast<size_t>(j)]] +=
                        S[static_cast<int64_t>(p) * G + j];
        } else {
            n_clusters = G;
            M = S;
        }
        if (n_clusters < 2)
            throw EngineError("bad_input",
                "mixed: the robust variance needs at least 2 clusters, got " +
                std::to_string(n_clusters));
        const double factor =
            static_cast<double>(n_clusters) / (static_cast<double>(n_clusters) - 1.0);
        std::vector<double> meat(static_cast<size_t>(np) * np, 0.0);
        la::xtx(M.data(), rows, np, meat.data());
        std::vector<double> tmp(static_cast<size_t>(np) * np, 0.0);
        V.assign(static_cast<size_t>(np) * np, 0.0);
        la::sandwich(r.vcov.data(), meat.data(), np, factor, V.data(), tmp.data());
    }
    auto sd = [&](int p) { return std::sqrt(std::max(0.0, V[static_cast<size_t>(p) * np + p])); };

    json coefs = json::array();
    for (int c = 0; c < g.k; ++c) {
        const double b = r.beta[static_cast<size_t>(c)], se = sd(c);
        const double z = (se > 0) ? b / se : 0.0;
        coefs.push_back({{"name", g.names[static_cast<size_t>(c)]}, {"b", b}, {"se", se}, {"z", z},
                         {"p", (se > 0) ? z_twosided_pvalue(z) : 1.0},
                         {"ci_lo", b - zc * se}, {"ci_hi", b + zc * se}});
    }

    // Random-effects table in Stata's σ² metric. Both the SE (delta method) and the
    // LOGNORMAL confidence interval come from mixed_var_se/mixed_var_ci in
    // estimators/mixedlm.h — one definition, shared with the unit test that pins them.
    json re = json::array();
    {
        struct Comp { const char* name; double var; int idx; };
        const Comp comps[2] = {{"var(_cons)", r.var_u, g.k}, {"var(Residual)", r.var_e, g.k + 1}};
        for (const Comp& cp : comps) {
            const double se_ln = sd(cp.idx);
            const double se_v = mixed_var_se(cp.var, se_ln);
            double lo = 0, hi = 0;
            mixed_var_ci(cp.var, se_v, zc, &lo, &hi);
            re.push_back({{"name", cp.name}, {"b", cp.var}, {"se", se_v},
                          {"se_ln_sigma", se_ln}, {"ci_lo", lo}, {"ci_hi", hi}});
        }
    }

    // Wald χ² over the SLOPES ONLY — Stata reports chi2(2) for `mixed y x1 x2 || id:`,
    // i.e. the constant is not part of the "all coefficients are zero" null. load_glm
    // puts the intercept LAST, so the slopes are exactly the leading g.k−1 columns.
    const int ks = (g.k > 0 && !g.names.empty() && g.names.back() == "(intercept)") ? g.k - 1 : g.k;
    double chi2 = 0;
    if (ks > 0) {
        std::vector<double> Vs(static_cast<size_t>(ks) * ks);
        for (int i = 0; i < ks; ++i)
            for (int j = 0; j < ks; ++j)
                Vs[static_cast<size_t>(i) * ks + j] = V[static_cast<size_t>(i) * np + j];
        if (la::spd_inverse(Vs.data(), ks) == 0)
            for (int i = 0; i < ks; ++i)
                for (int j = 0; j < ks; ++j)
                    chi2 += r.beta[static_cast<size_t>(i)] * Vs[static_cast<size_t>(i) * ks + j] *
                            r.beta[static_cast<size_t>(j)];
    }

    json out = {{"estimator", "mixed"}, {"method", "ml"}, {"n_obs", g.n},
                {"n_dropped_missing", g.n_dropped}, {"group_var", group}, {"n_groups", G},
                {"obs_per_group_min", r.g_min}, {"obs_per_group_avg", r.g_avg},
                {"obs_per_group_max", r.g_max}, {"vce", vce}, {"coefs", coefs},
                {"random_effects", re}, {"var_u", r.var_u}, {"var_e", r.var_e},
                {"ll", r.ll}, {"chi2", chi2}, {"df_chi2", ks}, {"p_chi2", chi2_pvalue(chi2, ks)},
                {"em_iters", r.em_iters}, {"iters", r.iters}, {"converged", r.converged}};
    if (vce == "robust" || vce == "cluster") {
        out["n_clusters"] = n_clusters;
        out["cluster_var"] = (vce == "cluster" && sep_cluster) ? cluster_var : group;
    }
    if (level_set) out["level"] = level;
    add_warnings(out, g.warnings);
    emit(out, opt(a, "--out"));
    return 0;
}

}  // namespace cli
}  // namespace cecon
