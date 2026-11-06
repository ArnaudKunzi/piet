// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "common.h"
#include "commands.h"

#include "estimators/rdlocal.h"

// P-QE wave — regression-discontinuity commands: rdrobust, rddensity, rdplot.
//
// All three share one local-polynomial core (src/estimators/rdlocal.{h,cpp});
// see that header for the method sources.
// This file is only argument wiring and the JSON projection: every quantity
// Stata's rdrobust/rddensity/rdplot PRINT gets its own named field, because
// the PIET rendering layer builds its tables from this JSON and cannot invent
// a cell that the engine did not emit.
//
// Out of scope for this wave, and refused BY NAME rather than silently
// ignored (silently dropping an option the user typed is the cardinal defect
// class): fuzzy(), covs(), cluster(), bwselect() other than mserd,
// nomasspoints/masspoints(check), vce() other than nn, rddensity bwselect
// other than comb, and rdplot's custom bin-construction options.

namespace cecon {
namespace cli {

namespace {

// Reject a named option we do not implement, using the SAME bad_input channel
// (and therefore the same exit status and error envelope) as every other CLI
// argument error.  The message always names the option.
[[noreturn]] void reject_option(const char* cmd, const std::string& flag,
                                const std::string& why) {
    throw cecon::EngineError("bad_input",
                             std::string(cmd) + ": " + flag + " is not supported — " + why,
                             flag);
}

RdKernel parse_kernel(const char* cmd, const std::string& s, bool& ok) {
    ok = true;
    if (s.empty() || s == "triangular" || s == "tri") return RdKernel::Triangular;
    if (s == "uniform" || s == "uni") return RdKernel::Uniform;
    ok = false;
    throw cecon::EngineError("bad_input",
                             std::string(cmd) + ": --kernel " + s +
                                 " is not supported (triangular|uniform only; "
                                 "epanechnikov is not implemented)",
                             s);
}

// The flags every RD command refuses by name.  Reading them here also marks
// them consumed, so the E6 unknown-flag net does not fire first with a less
// informative message.
void reject_common(const std::vector<std::string>& a, const char* cmd) {
    if (has(a, "--fuzzy") || !opt(a, "--fuzzy").empty())
        reject_option(cmd, "--fuzzy", "fuzzy RD designs are not implemented (sharp RD only)");
    if (!opt(a, "--covs").empty())
        reject_option(cmd, "--covs", "covariate-adjusted RD is not implemented");
    if (!opt(a, "--cluster").empty())
        reject_option(cmd, "--cluster", "cluster-robust RD variance is not implemented");
    if (has(a, "--nomasspoints"))
        reject_option(cmd, "--nomasspoints",
                      "only the mass-point-adjusted path is implemented "
                      "(Stata's masspoints(adjust) default)");
    std::string mp = opt(a, "--masspoints");
    if (!mp.empty() && mp != "adjust")
        reject_option(cmd, "--masspoints",
                      "only masspoints(adjust) is implemented, not '" + mp + "'");
}

double req_double(const std::string& s, double def) {
    if (s.empty()) return def;
    return std::stod(s);
}

json plot_pairs(const std::vector<double>& xs, const std::vector<double>& ys) {
    json arr = json::array();
    for (size_t i = 0; i < xs.size() && i < ys.size(); ++i)
        arr.push_back({{"x", xs[i]}, {"y", ys[i]}});
    return arr;
}

}  // namespace

int cmd_rdrobust(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data");
    std::string y = opt(a, "--y");
    std::string x = opt(a, "--x");
    if (data_missing(data) || y.empty() || x.empty()) {
        std::fprintf(stderr,
            "usage: cobra rdrobust --data f.hbin --y Y --x RUNNING [--c 0] "
            "[--p 1] [--q 2] [--h H] [--b B] [--kernel triangular|uniform] "
            "[--nnmatch 3] [--level 95] [--keepif EXPR] [--out f.json]\n");
        return 2;
    }
    reject_common(a, "rdrobust");
    std::string bws = opt(a, "--bwselect");
    if (!bws.empty() && bws != "mserd")
        reject_option("rdrobust", "--bwselect",
                      "only the mserd selector is implemented, not '" + bws + "'");
    std::string vce = opt(a, "--vce", "nn");
    if (vce != "nn")
        reject_option("rdrobust", "--vce",
                      "only vce(nn) is implemented, not '" + vce + "'");

    RdLocalConfig cfg;
    bool kok = false;
    cfg.kernel = parse_kernel("rdrobust", opt(a, "--kernel"), kok);
    cfg.c = req_double(opt(a, "--c"), 0.0);
    cfg.p = static_cast<int>(req_double(opt(a, "--p"), 1));
    cfg.q = static_cast<int>(req_double(opt(a, "--q"), cfg.p + 1));
    cfg.h_left = cfg.h_right = req_double(opt(a, "--h"), 0.0);
    cfg.b_left = cfg.b_right = req_double(opt(a, "--b"), 0.0);
    cfg.nnmatch = static_cast<int>(req_double(opt(a, "--nnmatch"), 3));
    cfg.level = req_double(opt(a, "--level"), 95.0);
    cfg.vce = vce;

    data = maybe_keepif(data, opt(a, "--keepif"));
    const Dataset& ds = acquire_data(data);
    const double* yv = require_num(ds, "rdrobust", y);
    const double* xv = require_num(ds, "rdrobust", x);
    auto t0 = std::chrono::steady_clock::now();
    RdRobustResult R = rd_robust_run(yv, xv, ds.n_rows(), cfg);
    auto t1 = std::chrono::steady_clock::now();

    json rows = json::array();
    rows.push_back({{"method", "Conventional"}, {"coef", R.coef_conventional},
                    {"se", R.se_conventional}, {"z", R.z_conventional},
                    {"p", R.p_conventional},
                    {"ci_lo", R.ci_conventional_lo}, {"ci_hi", R.ci_conventional_hi}});
    rows.push_back({{"method", "Bias-corrected"}, {"coef", R.coef_bc},
                    {"se", R.se_bc}, {"z", R.z_bc}, {"p", R.p_bc},
                    {"ci_lo", R.ci_bc_lo}, {"ci_hi", R.ci_bc_hi}});
    rows.push_back({{"method", "Robust"}, {"coef", R.coef_robust},
                    {"se", R.se_robust}, {"z", R.z_robust}, {"p", R.p_robust},
                    {"ci_lo", R.ci_robust_lo}, {"ci_hi", R.ci_robust_hi}});

    json out = {
        {"estimator", "rdrobust"},
        {"design", "sharp"},
        {"y", y},
        {"x", x},
        {"cutoff", cfg.c},
        {"kernel", R.kernel},
        {"vce", R.vce},
        {"bw_type", R.bw_type},
        {"p", R.p},
        {"q", R.q},
        {"nnmatch", cfg.nnmatch},
        {"level", cfg.level},
        {"n_obs", R.n_obs},
        {"n_left", R.n_left},
        {"n_right", R.n_right},
        {"n_eff_left", R.n_eff_left},
        {"n_eff_right", R.n_eff_right},
        {"n_unique_left", R.n_unique_left},
        {"n_unique_right", R.n_unique_right},
        {"masspoints", R.masspoints_detected ? "adjust" : "none"},
        {"h", R.h_left},
        {"h_left", R.h_left},
        {"h_right", R.h_right},
        {"b", R.b_left},
        {"b_left", R.b_left},
        {"b_right", R.b_right},
        {"rho", R.rho},
        // The headline cells, spelled out so nothing downstream has to know
        // which of the three rows rdrobust's compact display draws from: the
        // effect is the conventional point estimate, the inference is robust.
        {"effect", R.coef_conventional},
        {"se", R.se_robust},
        {"z", R.z_robust},
        {"pvalue", R.p_robust},
        {"ci_lo", R.ci_robust_lo},
        {"ci_hi", R.ci_robust_hi},
        {"rows", rows},
        {"time_ms", std::chrono::duration<double, std::milli>(t1 - t0).count()},
        {"ok", R.ok},
        {"error", R.error},
    };
    emit(out, opt(a, "--out"));
    return R.ok ? 0 : 1;
}

int cmd_rddensity(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data");
    std::string x = opt(a, "--x");
    if (data_missing(data) || x.empty()) {
        std::fprintf(stderr,
            "usage: cobra rddensity --data f.hbin --x RUNNING [--c 0] [--p 2] "
            "[--q 3] [--h H] [--kernel triangular|uniform] [--keepif EXPR] "
            "[--out f.json]\n");
        return 2;
    }
    reject_common(a, "rddensity");
    std::string bws = opt(a, "--bwselect");
    if (!bws.empty() && bws != "comb")
        reject_option("rddensity", "--bwselect",
                      "only the comb selector is implemented, not '" + bws + "'");
    std::string vce = opt(a, "--vce", "jackknife");
    if (vce != "jackknife")
        reject_option("rddensity", "--vce",
                      "only vce(jackknife) is implemented, not '" + vce + "'");

    RdLocalConfig cfg;
    bool kok = false;
    cfg.kernel = parse_kernel("rddensity", opt(a, "--kernel"), kok);
    cfg.c = req_double(opt(a, "--c"), 0.0);
    // has_p/has_q record whether --p/--q were actually typed (BUG-186): the
    // numeric value alone can't tell "p=1" from "not supplied".
    cfg.has_p = !opt(a, "--p").empty();
    cfg.has_q = !opt(a, "--q").empty();
    cfg.p = static_cast<int>(req_double(opt(a, "--p"), 1));
    cfg.q = static_cast<int>(req_double(opt(a, "--q"), 2));
    cfg.h_left = cfg.h_right = req_double(opt(a, "--h"), 0.0);

    data = maybe_keepif(data, opt(a, "--keepif"));
    const Dataset& ds = acquire_data(data);
    const double* xv = require_num(ds, "rddensity", x);
    auto t0 = std::chrono::steady_clock::now();
    RdDensityResult R = rd_density_run(xv, ds.n_rows(), cfg);
    auto t1 = std::chrono::steady_clock::now();

    json bino = json::array();
    for (const auto& r : R.binomial)
        bino.push_back({{"window", r.window}, {"n_left", r.n_left},
                        {"n_right", r.n_right}, {"p", r.p_value}});

    json out = {
        {"estimator", "rddensity"},
        {"x", x},
        {"cutoff", cfg.c},
        {"model", R.model},
        {"bw_method", R.bw_method},
        {"kernel", R.kernel},
        {"vce", R.vce},
        {"p", R.p},
        {"q", R.q},
        {"n_obs", R.n_obs},
        {"n_left", R.n_left},
        {"n_right", R.n_right},
        {"n_eff_left", R.n_eff_left},
        {"n_eff_right", R.n_eff_right},
        {"masspoints", R.masspoints_detected ? "adjust" : "none"},
        {"h", R.h_left},
        {"h_left", R.h_left},
        {"h_right", R.h_right},
        {"f_left", R.f_left},
        {"f_right", R.f_right},
        {"diff", R.diff},
        {"conventional", {{"T", R.t_conventional}, {"p", R.p_conventional},
                          {"se", R.se_conventional}, {"diff", R.diff}}},
        {"robust", {{"T", R.t_robust}, {"p", R.p_robust},
                    {"se", R.se_robust}, {"diff", R.diff_q},
                    {"f_left", R.f_left_q}, {"f_right", R.f_right_q}}},
        {"T", R.t_robust},
        {"pvalue", R.p_robust},
        {"binomial", bino},
        {"time_ms", std::chrono::duration<double, std::milli>(t1 - t0).count()},
        {"ok", R.ok},
        {"error", R.error},
    };
    emit(out, opt(a, "--out"));
    return R.ok ? 0 : 1;
}

int cmd_rdplot(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data");
    std::string y = opt(a, "--y");
    std::string x = opt(a, "--x");
    if (data_missing(data) || y.empty() || x.empty()) {
        std::fprintf(stderr,
            "usage: cobra rdplot --data f.hbin --y Y --x RUNNING [--c 0] "
            "[--p 4] [--h H] [--nbins L,R] [--kernel uniform|triangular] "
            "[--keepif EXPR] [--out f.json]\n");
        return 2;
    }
    reject_common(a, "rdplot");
    std::string bsel = opt(a, "--binselect");
    if (!bsel.empty() && bsel != "esmv")
        reject_option("rdplot", "--binselect",
                      "only the esmv (evenly-spaced mimicking-variance) bin "
                      "selector is implemented, not '" + bsel + "'");
    if (!opt(a, "--scale").empty())
        reject_option("rdplot", "--scale", "bin-count rescaling is not implemented");
    if (!opt(a, "--support").empty())
        reject_option("rdplot", "--support", "custom plot support is not implemented");

    RdLocalConfig cfg;
    bool kok = false;
    std::string kern = opt(a, "--kernel");
    cfg.kernel = kern.empty() ? RdKernel::Uniform : parse_kernel("rdplot", kern, kok);
    cfg.c = req_double(opt(a, "--c"), 0.0);
    // has_p records whether --p was actually typed (BUG-186): the numeric
    // value alone can't tell "p=1" from "not supplied".
    cfg.has_p = !opt(a, "--p").empty();
    cfg.p = static_cast<int>(req_double(opt(a, "--p"), 1));  // 1 => rdplot default of 4
    cfg.h_left = cfg.h_right = req_double(opt(a, "--h"), 0.0);
    std::string nb = opt(a, "--nbins");
    if (!nb.empty()) {
        auto v = split_csv(nb);
        if (v.size() == 1) {
            cfg.nbins_left = cfg.nbins_right = std::stoi(v[0]);
        } else if (v.size() == 2) {
            cfg.nbins_left = std::stoi(v[0]);
            cfg.nbins_right = std::stoi(v[1]);
        } else {
            std::fprintf(stderr, "--nbins J   or   --nbins JLEFT,JRIGHT\n");
            return 2;
        }
    }

    data = maybe_keepif(data, opt(a, "--keepif"));
    const Dataset& ds = acquire_data(data);
    const double* yv = require_num(ds, "rdplot", y);
    const double* xv = require_num(ds, "rdplot", x);
    auto t0 = std::chrono::steady_clock::now();
    RdPlotResult R = rd_plot_run(yv, xv, ds.n_rows(), cfg);
    auto t1 = std::chrono::steady_clock::now();

    json out = {
        {"estimator", "rdplot"},
        {"y", y},
        {"x", x},
        {"cutoff", cfg.c},
        {"kernel", R.kernel},
        {"binselect", R.binselect},
        {"n_obs", R.n_obs},
        {"n_left", R.n_left},
        {"n_right", R.n_right},
        {"n_eff_left", R.n_eff_left},
        {"n_eff_right", R.n_eff_right},
        // BUG-184: spelled the same way cmd_rdrobust/cmd_rddensity spell them, so
        // PIET's already-written preamble reader needs no change (additive
        // contract-A fields, MINOR, cobra-lane pen).
        {"n_unique_left", R.n_unique_left},
        {"n_unique_right", R.n_unique_right},
        {"masspoints", R.masspoints_detected ? "adjust" : "none"},
        {"p_left", R.poly_order_left},
        {"p_right", R.poly_order_right},
        {"h_left", R.h_left},
        {"h_right", R.h_right},
        {"scale_left", R.scale_left},
        {"scale_right", R.scale_right},
        {"bins_left", R.bins_left},
        {"bins_right", R.bins_right},
        {"avg_bin_length_left", R.avg_bin_len_left},
        {"avg_bin_length_right", R.avg_bin_len_right},
        {"med_bin_length_left", R.med_bin_len_left},
        {"med_bin_length_right", R.med_bin_len_right},
        {"bins_imse_left", R.bins_imse_left},
        {"bins_imse_right", R.bins_imse_right},
        {"bins_mv_left", R.bins_mv_left},
        {"bins_mv_right", R.bins_mv_right},
        {"implied_scale_left", R.implied_scale_left},
        {"implied_scale_right", R.implied_scale_right},
        {"wimse_var_weight", R.wimse_var_weight},
        {"wimse_bias_weight", R.wimse_bias_weight},
        {"bins", {{"left", plot_pairs(R.bin_x_left, R.bin_mean_y_left)},
                  {"right", plot_pairs(R.bin_x_right, R.bin_mean_y_right)}}},
        {"fit", {{"left", plot_pairs(R.fit_x_left, R.fit_y_left)},
                 {"right", plot_pairs(R.fit_x_right, R.fit_y_right)}}},
        {"fit_coef", {{"left", R.coef_left}, {"right", R.coef_right}}},
        {"time_ms", std::chrono::duration<double, std::milli>(t1 - t0).count()},
        {"ok", R.ok},
        {"error", R.error},
    };
    emit(out, opt(a, "--out"));
    return R.ok ? 0 : 1;
}

}  // namespace cli
}  // namespace cecon
