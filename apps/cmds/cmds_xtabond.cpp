// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "common.h"
#include "commands.h"

#include "estimators/xtabond.h"

// P7 wave — xtabond (Arellano–Bond one-step difference GMM).
//
// The command layer does three things and no arithmetic: resolve the panel
// context from `--xtset ID,T` (same shape xtreg/xtpois use), expand `--x`
// through the shared varlist expander so factor and time-series operators work
// here exactly as everywhere else, and hand the LEVEL columns to the estimator,
// which does its own lagging/differencing by time value.
//
// Oracle for every number: StataBE 19.5,
// internal capture p7_stata.log — `xtset id t` then
// `xtabond y L.y x1 x2, lags(1) vce(robust)`. Assertions in
// tests/unit/test_xtabond.cpp.

namespace cecon {
namespace cli {

// cobra xtabond --data f.hbin --y Y --x L.y,X1,X2 --xtset ID,T [--lags P]
//               [--vce robust] [--cluster ID] [--level P] [--keepif E] [--out f.json]
int cmd_xtabond(const std::vector<std::string>& a) {
    const std::string y = opt(a, "--y"), xcsv = opt(a, "--x");
    std::string data = opt(a, "--data");
    const std::string xtset = opt(a, "--xtset"), cluster_var = opt(a, "--cluster");
    if (data_missing(data) || y.empty() || xtset.empty()) {
        std::fprintf(stderr,
                     "usage: cobra xtabond --data f.hbin --y Y --xtset ID,T [--x X1,X2] "
                     "[--lags P] [--vce robust] [--cluster ID] [--level P] [--keepif EXPR] "
                     "[--out f.json]\n"
                     "  Arellano-Bond ONE-STEP difference GMM for dynamic panels (Stata\n"
                     "  `xtabond y x1 x2, lags(P) vce(robust)`). The P autoregressive terms are\n"
                     "  created by --lags; --x carries the strictly exogenous covariates, which\n"
                     "  enter the model in differences and instrument themselves in differences.\n"
                     "  Deep lags of y, L(P+1/.).y, are the GMM-type instruments; `_cons` is\n"
                     "  estimated off a level equation instrumented by itself.\n"
                     "  --vce conventional (default, Stata's bare xtabond) or robust/cluster\n"
                     "  cluster-on-panel sandwich Stata prints as \"Std. err. adjusted for\n"
                     "  clustering on ID\". Two-step estimation (and hence the Windmeijer\n"
                     "  correction and the conventional one-step VCE) is NOT implemented and is\n"
                     "  rejected rather than silently answered with one-step numbers.\n");
        return 2;
    }
    TsMeta tsm = parse_tsmeta(a);
    if (tsm.panelvar.empty() || tsm.timevar.empty())
        throw EngineError("bad_input",
                          "xtabond: --xtset must name BOTH the panel and the time variable, as "
                          "--xtset ID,T (a dynamic panel model needs the time dimension)");

    // BUG-383 #5: Stata's default is the one-step CONVENTIONAL VCE; robust is
    // opt-in (`vce(robust)`). The old default here was robust — the only arm
    // implemented then — which made every bare `xtabond` silently disagree with
    // Stata's header. "cluster" remains an alias of the robust sandwich (it
    // clusters on the panel, which is what the sandwich always did).
    const std::string vce = opt(a, "--vce", "conventional");
    if (vce != "conventional" && vce != "robust" && vce != "cluster")
        throw EngineError("unsupported",
                          "xtabond: --vce must be conventional (default), robust, or cluster; "
                          "got '" + vce + "'.");
    if (!cluster_var.empty() && cluster_var != tsm.panelvar)
        throw EngineError("bad_input",
                          "xtabond: the one-step robust VCE always clusters on the PANEL variable "
                          "('" + tsm.panelvar + "'); --cluster " + cluster_var + " is not supported.");

    int lags = 1;
    {
        const std::string s = opt(a, "--lags", "1");
        try {
            lags = std::stoi(s);
        } catch (const std::exception&) {
            throw EngineError("bad_input", "xtabond: --lags must be a positive integer (got '" + s + "')");
        }
        if (lags < 1) throw EngineError("bad_input", "xtabond: --lags must be at least 1");
    }
    const double level = std::stod(opt(a, "--level", "95"));
    if (level < 10.0 || level > 99.99)
        throw EngineError("bad_input", "xtabond: --level must be in [10, 99.99]");

    // --keepif is Stata's `if`, and in a dynamic panel EVERY variable is
    // time-series: physically dropping the excluded rows would punch holes in
    // the panels and move every lag. So the dataset stays whole and the
    // qualifier becomes a row mask over the estimation sample (the same E1
    // routing plan_keepif applies to any ts-bearing varlist — unconditional
    // here, because y is lagged whether or not the varlist says so).
    const std::string keepif = opt(a, "--keepif");
    const Dataset& ds = acquire_data(data);
    std::vector<char> mask;
    if (!keepif.empty()) mask = eval_keepif_mask(data, keepif);
    const int64_t n = ds.n_rows();
    const double* yv = require_num(ds, "xtabond", y);
    const double* iv = require_num(ds, "xtabond", tsm.panelvar);
    const double* tv = require_num(ds, "xtabond", tsm.timevar);

    // Covariates through the shared expander: `L.y`, `i.region`, `D.x` and
    // friends all resolve here, panel-aware, exactly as they do in feols/csdid.
    std::vector<std::string> xs = split_csv(xcsv);
    auto xcols = expand_varlist(ds, xs, tsm, /*bare_factor=*/false);
    const int kx = static_cast<int>(xcols.size());
    std::vector<std::string> xnames(static_cast<size_t>(kx));
    std::vector<double> X(static_cast<size_t>(n) * static_cast<size_t>(kx));
    for (int c = 0; c < kx; ++c) {
        xnames[static_cast<size_t>(c)] = xcols[static_cast<size_t>(c)].first;
        std::copy(xcols[static_cast<size_t>(c)].second.begin(),
                  xcols[static_cast<size_t>(c)].second.end(),
                  X.data() + static_cast<int64_t>(c) * n);
    }

    XtabondSpec spec;
    spec.lags = lags;
    spec.delta = (tsm.delta > 0.0) ? tsm.delta : 1.0;
    spec.constant = true;

    const auto t0 = std::chrono::steady_clock::now();
    XtabondResult R = xtabond_fit(iv, tv, yv, kx > 0 ? X.data() : nullptr, kx, xnames, n, spec,
                                  mask.empty() ? nullptr : mask.data(), /*conventional_vce=*/vce == "conventional");
    const auto t1 = std::chrono::steady_clock::now();
    if (!R.ok) throw EngineError("bad_input", R.err.empty() ? "xtabond: estimation failed" : R.err);

    const double zc = z_critical(level == 95.0 ? 0.05 : 1.0 - level / 100.0);
    json coefs = json::array();
    for (int c = 0; c < R.k; ++c) {
        const double b = R.beta[static_cast<size_t>(c)], se = R.se[static_cast<size_t>(c)];
        const double z = tstat(b, se);
        const double pv = (se > 0) ? z_twosided_pvalue(z) : 1.0;
        coefs.push_back({{"name", R.names[static_cast<size_t>(c)]},
                         {"beta", b},
                         {"se", se},
                         {"z", z},
                         {"p", pv},
                         {"ci_lo", b - zc * se},
                         {"ci_hi", b + zc * se}});
    }
    json out = {
        {"estimator", "xtabond"},
        {"vce", vce},
        {"rss", R.rss},      // e(rss): differenced residual SS
        {"sig2", R.sig2},    // e(sig2): rss / (2(N-k))
        {"cluster", tsm.panelvar},
        {"steps", 1},
        {"lags", lags},
        {"n_obs", R.n_obs},
        {"n_groups", R.n_groups},
        {"obs_per_group", {{"min", R.min_obs}, {"avg", R.avg_obs}, {"max", R.max_obs}}},
        {"n_level_obs", R.n_level_obs},
        {"n_instruments", R.n_instruments},
        {"instruments",
         {{"differenced_gmm", R.gmm_label},
          {"differenced_standard", R.std_labels},
          {"level_standard", json::array({"_cons"})},
          {"n_gmm", R.n_gmm_instruments},
          {"n_standard", R.n_std_instruments},
          {"n_level", R.n_level_instruments}}},
        {"coefs", coefs},
        {"level", level},
        {"time_ms", std::chrono::duration<double, std::milli>(t1 - t0).count()},
    };
    if (std::isfinite(R.chi2)) {
        out["chi2"] = R.chi2;
        out["df_chi2"] = R.chi2_df;
        out["p_chi2"] = R.p_chi2;
    } else {
        out["chi2"] = nullptr;
        out["df_chi2"] = R.chi2_df;
        out["p_chi2"] = nullptr;
    }
    if (!R.omitted.empty()) {
        out["omitted"] = R.omitted;
        std::vector<std::string> notes;
        notes.reserve(R.omitted.size());
        for (const auto& nm : R.omitted) notes.push_back(nm + " omitted because of collinearity");
        add_warnings(out, notes);
    }
    emit(out, opt(a, "--out"));
    return 0;
}

}  // namespace cli
}  // namespace cecon
