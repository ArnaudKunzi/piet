// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "common.h"
#include "commands.h"

#include "estimators/mlogit.h"
#include "estimators/oglm.h"  // ord_categories — the outcome-recoding rule (sorted
                              // distinct values -> 0..K-1) is shared with the ordered
                              // models; one definition, not two spellings of it.

// P7 wave — mlogit (Stata `mlogit`).
//
// The design KEEPS load_glm's trailing intercept: every non-base equation has its
// own constant, exactly as Stata prints one _cons per equation. θ is those
// equations stacked (equation-major, non-base outcomes ascending, load_glm's column
// order within an equation) and the score blocks are emitted in the same order, so
// the sandwich meat lines up with the bread block-for-block.
//
// The base outcome is chosen BY VALUE via --base; with the flag absent it is the
// most frequent outcome, which is Stata's own default.

namespace cecon {
namespace cli {

int cmd_mlogit(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), y = opt(a, "--y"), xcsv = opt(a, "--x");
    std::string vce = opt(a, "--vce", "iid");
    std::string cluster_var = opt(a, "--cluster");
    if (data_missing(data) || y.empty() || xcsv.empty()) {
        std::fprintf(stderr,
                     "usage: cobra mlogit --data f.hbin --y Y --x X1,X2 [--base V] "
                     "[--vce iid|robust|cluster] [--cluster C] [--level P] [--keepif EXPR] "
                     "[--out f.json]\n"
                     "  multinomial logit; one equation per non-base outcome.\n"
                     "  --base V                 base outcome BY VALUE (default: most frequent,\n"
                     "                           matching Stata's baseoutcome() default).\n"
                     "  --vce cluster            needs --cluster C.\n");
        return 2;
    }
    if (vce != "iid" && vce != "robust" && vce != "cluster") {
        std::fprintf(stderr, "mlogit: --vce must be one of iid|robust|cluster (got %s).\n", vce.c_str());
        return 2;
    }
    if (vce == "cluster" && cluster_var.empty()) {
        std::fprintf(stderr, "mlogit: --vce cluster requires --cluster C.\n");
        return 2;
    }
    bool level_set = has(a, "--level");
    double level = std::stod(opt(a, "--level", "95"));
    if (level_set && (level < 10.0 || level > 99.99)) {
        std::fprintf(stderr, "mlogit: --level must be in [10, 99.99] (got %s).\n",
                     opt(a, "--level").c_str());
        return 2;
    }
    const double zc = z_critical(level == 95.0 ? 0.05 : 1.0 - level / 100.0);
    bool base_set = has(a, "--base");
    double base_value = 0;
    if (base_set) {
        try {
            base_value = std::stod(opt(a, "--base"));
        } catch (const std::exception&) {
            throw EngineError("bad_input", "mlogit: --base must be a numeric outcome VALUE (got '" +
                                           opt(a, "--base") + "')");
        }
    }

    data = maybe_keepif(data, opt(a, "--keepif"));
    const Dataset& ds = acquire_data(data);
    std::vector<std::string> cluster_names;
    if (!cluster_var.empty()) cluster_names.push_back(cluster_var);
    GlmInputs g = load_glm(ds, "mlogit", y, split_csv(xcsv), cluster_names);
    require_sample(g.n);  // BUG-1269: r(2000) "no observations" / r(2001) for one row
    if (vce == "cluster" && (g.cluster.empty() || g.cluster[0].n_levels < 2))
        throw EngineError("bad_input",
            "mlogit: vce(cluster) requires at least 2 clusters, but the cluster variable has " +
            std::to_string(g.cluster.empty() ? 0 : g.cluster[0].n_levels));

    // Outcomes recoded 0..K-1 by sorted distinct value (ord_categories, shared with
    // the ordered models — the recoding rule is identical, only the model differs).
    std::vector<int> cat;
    std::vector<double> cat_values = ord_categories(g.y.data(), g.n, cat);
    const int K = static_cast<int>(cat_values.size());
    if (K < 2)
        throw EngineError("bad_input",
            "mlogit: outcome '" + y + "' takes fewer than 2 distinct values.");
    std::vector<int64_t> counts(static_cast<size_t>(K), 0);
    for (int64_t i = 0; i < g.n; ++i) ++counts[static_cast<size_t>(cat[static_cast<size_t>(i)])];
    int base = 0;
    if (base_set) {
        base = -1;
        for (int m = 0; m < K; ++m)
            if (cat_values[static_cast<size_t>(m)] == base_value) { base = m; break; }
        if (base < 0)
            throw EngineError("bad_input", "mlogit: --base value " + opt(a, "--base") +
                                           " does not occur in outcome '" + y + "'");
    } else {
        // Stata's default baseoutcome() is the MOST FREQUENT outcome; ties go to the
        // lower value (the scan keeps the first strict maximum).
        for (int m = 1; m < K; ++m)
            if (counts[static_cast<size_t>(m)] > counts[static_cast<size_t>(base)]) base = m;
    }
    const int neq = K - 1, np = neq * g.k;

    MlogitResult r = mlogit_fit(g.X.data(), g.n, g.k, cat.data(), K, base);
    if (!r.ok)
        throw EngineError("no_convergence",
            "mlogit: MLE failed (singular information matrix or non-convergent fit).");

    // V = bread (iid) or a sandwich bread·meat·bread over the per-observation score
    // rows, whose block order matches r.vcov's exactly.
    //
    // SMALL-SAMPLE FACTOR. Stata's ML multiplier, NOT the regression-style
    // (N-1)/(N-K)·G/(G-1):
    //   vce(robust)  → N/(N-1)
    //   vce(cluster) → G/(G-1)
    // Same P5 precedent as logit/probit/tobit (cmds_mle.cpp:284-297), VERIFIED not
    // assumed — tests/unit/test_mlogit.cpp asserts all 14 standard errors of the P7
    // oracle `mlogit ynom x1 x2 i.region, baseoutcome(0) vce(cluster id)` (200
    // clusters, N=4,671) against G/(G-1) and reproduces every printed digit; the
    // regression-style factor would add (N-1)/(N-K) ≈ 1.0014 to each.
    std::vector<double> V = r.vcov;
    int n_clusters = 0;
    if (vce == "robust" || vce == "cluster") {
        std::vector<double> S = mlogit_scores(g.X.data(), g.n, g.k, cat.data(), r);
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

    // equations[]: one entry per outcome IN OUTCOME ORDER. The base outcome carries
    // {outcome, base:true} and no coefs — rendering "(base outcome)" is PIET's job;
    // the engine emits the stub so the equation list is complete and ordered.
    json equations = json::array();
    for (int m = 0; m < K; ++m) {
        json eq = {{"outcome", cat_values[static_cast<size_t>(m)]},
                   {"n_obs_outcome", counts[static_cast<size_t>(m)]}};
        if (m == base) {
            eq["base"] = true;
            equations.push_back(eq);
            continue;
        }
        const int e = (m < base) ? m : m - 1;  // equation index (base excluded)
        json coefs = json::array();
        for (int c = 0; c < g.k; ++c) {
            const int p = e * g.k + c;
            double b = r.beta[static_cast<size_t>(p)], se = sd(p);
            double z = (se > 0) ? b / se : 0.0;
            coefs.push_back({{"name", g.names[static_cast<size_t>(c)]}, {"b", b}, {"se", se}, {"z", z},
                             {"p", (se > 0) ? z_twosided_pvalue(z) : 1.0},
                             {"ci_lo", b - zc * se}, {"ci_hi", b + zc * se}});
        }
        eq["base"] = false;
        eq["coefs"] = coefs;
        equations.push_back(eq);
    }

    // Wald χ² over ALL slopes of ALL equations — every equation's constant excluded
    // (load_glm puts it last in each block), so df = (K-1)·(k-1): Stata's chi2(12)
    // for 2 equations × 6 slopes here.
    double chi2 = 0;
    int df = 0;
    {
        std::vector<int> idx;
        for (int e = 0; e < neq; ++e)
            for (int c = 0; c < g.k; ++c)
                if (g.names[static_cast<size_t>(c)] != "(intercept)") idx.push_back(e * g.k + c);
        df = static_cast<int>(idx.size());
        if (df > 0) {
            std::vector<double> Vs(static_cast<size_t>(df) * df);
            for (int i = 0; i < df; ++i)
                for (int j = 0; j < df; ++j)
                    Vs[static_cast<size_t>(i) * df + j] =
                        V[static_cast<size_t>(idx[static_cast<size_t>(i)]) * np + idx[static_cast<size_t>(j)]];
            if (la::spd_inverse(Vs.data(), df) == 0)
                for (int i = 0; i < df; ++i)
                    for (int j = 0; j < df; ++j)
                        chi2 += r.beta[static_cast<size_t>(idx[static_cast<size_t>(i)])] *
                                Vs[static_cast<size_t>(i) * df + j] *
                                r.beta[static_cast<size_t>(idx[static_cast<size_t>(j)])];
        }
    }

    json out = {{"estimator", "mlogit"}, {"n_obs", g.n}, {"n_dropped_missing", g.n_dropped},
                {"n_cats", K}, {"cat_values", cat_values},
                {"base_outcome", cat_values[static_cast<size_t>(base)]},
                {"vce", vce}, {"equations", equations},
                {"ll", r.ll}, {"ll_0", r.ll_0},
                {"r2_pseudo", (r.ll_0 != 0) ? 1.0 - r.ll / r.ll_0 : 0.0},
                {"chi2", chi2}, {"df_chi2", df}, {"p_chi2", chi2_pvalue(chi2, df)},
                {"iters", r.iters}, {"converged", r.converged}};
    if (vce == "cluster") { out["cluster_var"] = cluster_var; out["n_clusters"] = n_clusters; }
    if (level_set) out["level"] = level;
    add_warnings(out, g.warnings);
    // --emit-v (BUG-1196): the fit's V over the non-base equations in θ's order,
    // each name `<outcome value>:<coefficient>`. Stata's e(V) also carries the
    // base outcome's equation as zero rows/columns (StataBE 19.5 colfullnames
    // e(b) `1:o.x1 1:o.x2 1:o._cons 2:x1 ...`); that stripe is PIET's to build.
    if (has(a, "--emit-v")) {
        std::vector<std::string> names;
        for (int m = 0; m < K; ++m) {
            if (m == base) continue;
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.17g", cat_values[static_cast<size_t>(m)]);
            for (int c = 0; c < g.k; ++c) names.push_back(std::string(buf) + ":" + g.names[static_cast<size_t>(c)]);
        }
        add_e_v(out, names, V);
    }
    emit(out, opt(a, "--out"));
    return 0;
}

}  // namespace cli
}  // namespace cecon
