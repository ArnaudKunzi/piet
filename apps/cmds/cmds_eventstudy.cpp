// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "common.h"
#include "commands.h"

#include "estimators/didimpute.h"
#include "estimators/sunab.h"

// P-QE wave — heterogeneity-robust event-study commands:
//
//   eventstudyinteract — Sun & Abraham (2021) interaction-weighted event study.
//   did_imputation     — Borusyak, Jaravel & Spiess (2024) imputation estimator.
//
// Both handlers do their own single listwise deletion over the columns they
// actually read, then hand raw arrays to the estimator (the csdid pattern —
// these commands have no design matrix of covariates for build_model to shape).
//
// Every quantity either command PRINTS is a named field of its JSON: the PIET
// layer renders from this payload and cannot invent a header scalar that the
// engine omitted.

namespace cecon {
namespace cli {
namespace {

// Stata numlist subset used by horizons(): a comma list whose items may be a
// single number or an inclusive `a/b` integer range.
bool parse_numlist(const std::string& s, std::vector<double>& out) {
    for (const auto& tok : split_csv(s)) {
        if (tok.empty()) continue;
        const auto slash = tok.find('/');
        try {
            if (slash == std::string::npos) {
                out.push_back(std::stod(tok));
            } else {
                const long long lo = std::stoll(tok.substr(0, slash));
                const long long hi = std::stoll(tok.substr(slash + 1));
                if (hi < lo) return false;
                for (long long v = lo; v <= hi; ++v) out.push_back(static_cast<double>(v));
            }
        } catch (const std::exception&) {
            return false;
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return !out.empty();
}

}  // namespace

int cmd_eventstudyinteract(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data");
    const std::string y = opt(a, "--y");
    const std::string relcsv = opt(a, "--rel");
    const std::string abscsv = opt(a, "--absorb");
    const std::string cohortv = opt(a, "--cohort");
    const std::string controlv = opt(a, "--control_cohort");
    const std::string vce = opt(a, "--vce", "cluster");
    const std::string clvar = opt(a, "--cluster");
    const double level = std::stod(opt(a, "--level", "95"));
    if (data_missing(data) || y.empty() || relcsv.empty() || abscsv.empty() ||
        cohortv.empty() || controlv.empty()) {
        std::fprintf(stderr,
            "usage: cobra eventstudyinteract --data f.hbin --y Y --rel F2,F3,L2,L3 "
            "--absorb ID,T --cohort G --control_cohort NEVER --cluster ID "
            "[--vce cluster] [--weight W --wtype aw|pw] [--level 95] [--keepif EXPR] "
            "[--out f.json]\n");
        return 2;
    }
    // Honest reject BY NAME: the IW variance combines the saturated stage's
    // cluster-robust VCE with the cohort-share estimation error, and only the
    // clustered shape is implemented (which is also all eventstudyinteract
    // itself accepts). Silently downgrading to another VCE would be the
    // cardinal defect — a plausible-looking wrong standard error.
    if (vce != "cluster")
        throw cecon::EngineError(
            "bad_input",
            "eventstudyinteract: --vce '" + vce +
                "' is not supported — this command implements vce(cluster) only; "
                "pass --vce cluster together with --cluster VAR",
            "--vce");
    if (clvar.empty())
        throw cecon::EngineError(
            "bad_input",
            "eventstudyinteract: vce(cluster) requires --cluster VAR", "--cluster");
    const auto absvars = split_csv(abscsv);
    if (absvars.size() != 2)
        throw cecon::EngineError(
            "bad_input",
            "eventstudyinteract: --absorb takes exactly two fixed effects (unit,period); "
            "got " + std::to_string(absvars.size()),
            "--absorb");
    const auto relvars = split_csv(relcsv);
    if (relvars.empty())
        throw cecon::EngineError("bad_input",
                                 "eventstudyinteract: --rel names no variable", "--rel");
    // BUG-1283: [aweight=] / [pweight=] (identical answers under vce(cluster); see
    // sunab.h for the weighted estimator). fw/iw are refused by name.
    const std::string wvar = opt(a, "--weight"), wtype = opt(a, "--wtype");
    if (!wvar.empty() || !wtype.empty()) {
        if (wvar.empty())
            throw cecon::EngineError("bad_input", "eventstudyinteract: --wtype requires --weight",
                                     "--wtype");
        if (!(wtype.empty() || wtype == "aw" || wtype == "aweight" || wtype == "pw" ||
              wtype == "pweight"))
            throw cecon::EngineError(
                "unsupported",
                "eventstudyinteract: --wtype must be aw or pw (got '" + wtype +
                    "'); fweight and iweight are not implemented",
                "--wtype");
    }

    data = maybe_keepif(data, opt(a, "--keepif"));
    const Dataset& ds = acquire_data(data);
    const int64_t n_raw = ds.n_rows();
    const double* yv = require_num(ds, "eventstudyinteract", y);
    const double* gv = require_num(ds, "eventstudyinteract", cohortv);
    const double* cv = require_num(ds, "eventstudyinteract", controlv);
    const double* fu = require_num(ds, "eventstudyinteract", absvars[0]);
    const double* ft = require_num(ds, "eventstudyinteract", absvars[1]);
    const double* cl = require_num(ds, "eventstudyinteract", clvar);
    std::vector<const double*> rv;
    for (const auto& nm : relvars) rv.push_back(require_num(ds, "eventstudyinteract", nm));
    const double* wv = wvar.empty() ? nullptr : require_num(ds, "eventstudyinteract", wvar);

    // One listwise deletion over everything the estimator reads but the cohort: a
    // missing cohort() value is data (BUG-1525, StataBE 19.5: `gen first = g if g > 0`
    // leaves the never-treated missing and cohort(first) keeps them, e(N) 180;
    // sunab_run says what a missing cohort means on a treated row).
    std::vector<int64_t> keep;
    keep.reserve(static_cast<size_t>(n_raw));
    for (int64_t i = 0; i < n_raw; ++i) {
        if (std::isnan(yv[i]) || std::isnan(cv[i]) ||
            std::isnan(fu[i]) || std::isnan(ft[i]) || std::isnan(cl[i])) continue;
        bool bad = false;
        for (const double* c : rv) if (std::isnan(c[i])) { bad = true; break; }
        if (wv) {
            // A missing or zero weight marks the row out; a negative one is an error.
            if (std::isnan(wv[i]) || wv[i] == 0.0) bad = true;
            else if (wv[i] < 0.0)
                throw cecon::EngineError("bad_input", "eventstudyinteract: weight '" + wvar +
                                                          "' has a negative value", "--weight");
        }
        if (!bad) keep.push_back(i);
    }
    const int64_t n = static_cast<int64_t>(keep.size());
    const int L = static_cast<int>(relvars.size());
    std::vector<double> yy(static_cast<size_t>(n)), gg(static_cast<size_t>(n)),
        cc(static_cast<size_t>(n)), uu(static_cast<size_t>(n)), tt(static_cast<size_t>(n)),
        clc(static_cast<size_t>(n)), rel(static_cast<size_t>(n) * L), ww;
    if (wv) ww.resize(static_cast<size_t>(n));
    for (int64_t r = 0; r < n; ++r) {
        const int64_t i = keep[static_cast<size_t>(r)];
        yy[static_cast<size_t>(r)] = yv[i];
        gg[static_cast<size_t>(r)] = gv[i];
        cc[static_cast<size_t>(r)] = cv[i];
        uu[static_cast<size_t>(r)] = fu[i];
        tt[static_cast<size_t>(r)] = ft[i];
        clc[static_cast<size_t>(r)] = cl[i];
        if (wv) ww[static_cast<size_t>(r)] = wv[i];
        for (int l = 0; l < L; ++l)
            rel[static_cast<size_t>(l) * n + r] = rv[static_cast<size_t>(l)][i];
    }

    SunabConfig cfg;
    cfg.vce = vce;
    cfg.level = level;
    const auto t0 = std::chrono::steady_clock::now();
    SunabResult R = sunab_run(yy.data(), rel.data(), L, gg.data(), cc.data(), uu.data(),
                              tt.data(), clc.data(), n, cfg, wv ? ww.data() : nullptr);
    const auto t1 = std::chrono::steady_clock::now();

    json coef = json::object(), se = json::object(), tst = json::object(),
         pv = json::object(), lo = json::object(), hi = json::object(),
         wts = json::object(), cco = json::object();
    for (const auto& c : R.coefs) {
        const std::string& nm = relvars[static_cast<size_t>(c.rel_index)];
        coef[nm] = c.coef; se[nm] = c.se; tst[nm] = c.t; pv[nm] = c.p;
        lo[nm] = c.ci_lo; hi[nm] = c.ci_hi;
        wts[nm] = c.cohort_weight; cco[nm] = c.cohort_coef;
    }
    // e(V_iw) (BUG-1524): rows and columns in `rel` order.
    json viw = json::array();
    if (R.V_iw.size() == relvars.size() * relvars.size())
        for (size_t l = 0; l < relvars.size(); ++l) {
            json row = json::array();
            for (size_t m = 0; m < relvars.size(); ++m) row.push_back(R.V_iw[l * relvars.size() + m]);
            viw.push_back(row);
        }
    json out = {
        {"estimator", "eventstudyinteract"},
        {"y", y},
        {"rel", relvars},
        {"absorb", absvars},
        {"cohort", cohortv},
        {"control_cohort", controlv},
        {"vce", vce},
        {"cluster", clvar},
        {"n_obs", R.n_obs},
        {"n_obs_weights", R.n_obs_weights},
        {"n_clusters", R.n_clusters},
        {"n_interactions", R.n_interactions},
        {"df_absorb", R.df_absorb},
        {"dof_resid", R.dof_resid},
        {"F", R.F},
        {"df1", R.df1},
        {"df2", R.df2},
        {"prob_F", (R.df1 > 0 && R.df2 > 0 && !std::isnan(R.F))
                       ? f_pvalue(R.F, R.df1, R.df2) : std::nan("")},
        {"r2", R.r2},
        {"r2_a", R.r2_a},
        {"rmse", R.rmse},
        {"level", R.level},
        {"cohorts", R.cohorts},
        {"coef", coef},
        {"se", se},
        {"tstat", tst},
        {"pvalue", pv},
        {"ci_lo", lo},
        {"ci_hi", hi},
        {"iw_weight", wts},
        {"cohort_coef", cco},
        {"V_iw", viw},
        {"time_ms", std::chrono::duration<double, std::milli>(t1 - t0).count()},
        {"ok", R.ok},
        {"error", R.error},
    };
    // Weighted only, so an unweighted reply is unchanged: e(wtype), e(sumweights)
    // and the "(sum of wgt is ...)" line's sum over the share rows.
    if (wv)
        out["weight"] = {{"var", wvar},
                         {"type", (wtype == "pw" || wtype == "pweight") ? "pw" : "aw"},
                         {"sum_w", R.sum_w},
                         {"sum_w_weights", R.sum_w_weights}};
    emit(out, opt(a, "--out"));
    return R.ok ? 0 : 1;
}

int cmd_did_imputation(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data");
    const std::string y = opt(a, "--y");
    const std::string ivar = opt(a, "--ivar");
    const std::string tvar = opt(a, "--time");
    const std::string gvar = opt(a, "--gvar");
    const std::string horiz = opt(a, "--horizons", "0");
    const std::string pres = opt(a, "--pretrends", "0");
    const bool autosample = has(a, "--autosample");
    const double level = std::stod(opt(a, "--level", "95"));
    // Consumed only so the refusal can NAME the option: silently ignoring a
    // weight or a re-clustering request would change the answer without saying so.
    const std::string wvar = opt(a, "--weight");
    const std::string clvar = opt(a, "--cluster");
    if (data_missing(data) || y.empty() || ivar.empty() || tvar.empty() || gvar.empty()) {
        std::fprintf(stderr,
            "usage: cobra did_imputation --data f.hbin --y Y --ivar ID --time T --gvar E "
            "[--horizons 0/3] [--pretrends 2] [--autosample] [--level 95] "
            "[--keepif EXPR] [--out f.json]\n"
            "  --gvar E is each unit's first treated period; a MISSING value marks a "
            "never-treated unit.\n");
        return 2;
    }
    if (!wvar.empty())
        throw cecon::EngineError(
            "bad_input",
            "did_imputation: --weight is not supported — this command implements the "
            "unweighted BJS imputation estimator only",
            "--weight");
    if (!clvar.empty())
        throw cecon::EngineError(
            "bad_input",
            "did_imputation: --cluster is not supported — standard errors are always "
            "clustered on --ivar ('" + ivar + "'), the command's default",
            "--cluster");
    std::vector<double> horizons;
    if (!parse_numlist(horiz, horizons))
        throw cecon::EngineError("bad_input",
                                 "did_imputation: --horizons '" + horiz +
                                     "' is not a numlist (e.g. 0/3 or 0,1,2,3)",
                                 "--horizons");
    int npre = 0;
    try {
        npre = std::stoi(pres);
    } catch (const std::exception&) {
        throw cecon::EngineError("bad_input",
                                 "did_imputation: --pretrends '" + pres +
                                     "' is not a non-negative integer",
                                 "--pretrends");
    }
    if (npre < 0)
        throw cecon::EngineError("bad_input",
                                 "did_imputation: --pretrends must be non-negative",
                                 "--pretrends");

    data = maybe_keepif(data, opt(a, "--keepif"));
    const Dataset& ds = acquire_data(data);
    const int64_t n_raw = ds.n_rows();
    const double* yv = require_num(ds, "did_imputation", y);
    const double* iv = require_num(ds, "did_imputation", ivar);
    const double* tv = require_num(ds, "did_imputation", tvar);
    const double* gv = require_num(ds, "did_imputation", gvar);

    // Listwise over y/ivar/time only: a MISSING --gvar is data, not absence —
    // it is how a never-treated unit is spelled.
    std::vector<int64_t> keep;
    keep.reserve(static_cast<size_t>(n_raw));
    for (int64_t i = 0; i < n_raw; ++i)
        if (!std::isnan(yv[i]) && !std::isnan(iv[i]) && !std::isnan(tv[i])) keep.push_back(i);
    const int64_t n = static_cast<int64_t>(keep.size());
    std::vector<double> yy(static_cast<size_t>(n)), ii(static_cast<size_t>(n)),
        ttv(static_cast<size_t>(n)), gg(static_cast<size_t>(n));
    for (int64_t r = 0; r < n; ++r) {
        const int64_t i = keep[static_cast<size_t>(r)];
        yy[static_cast<size_t>(r)] = yv[i];
        ii[static_cast<size_t>(r)] = iv[i];
        ttv[static_cast<size_t>(r)] = tv[i];
        gg[static_cast<size_t>(r)] = gv[i];
    }

    DidImputeConfig cfg;
    cfg.horizons = horizons;
    cfg.pretrends = npre;
    cfg.autosample = autosample;
    cfg.level = level;
    const auto t0 = std::chrono::steady_clock::now();
    DidImputeResult R = didimpute_run(yy.data(), ii.data(), ttv.data(), gg.data(), n, cfg);
    const auto t1 = std::chrono::steady_clock::now();

    json coef = json::object(), se = json::object(), zs = json::object(),
         pv = json::object(), lo = json::object(), hi = json::object(),
         nc = json::object(), nca = json::object();
    for (const auto& c : R.coefs) {
        coef[c.name] = c.estimate; se[c.name] = c.se; zs[c.name] = c.z; pv[c.name] = c.p;
        lo[c.name] = c.ci_lo; hi[c.name] = c.ci_hi;
        nc[c.name] = c.n_cells; nca[c.name] = c.n_cells_all;
    }
    json out = {
        {"estimator", "did_imputation"},
        {"y", y},
        {"ivar", ivar},
        {"time", tvar},
        {"gvar", gvar},
        {"horizons", horizons},
        {"pretrends", npre},
        {"autosample", autosample},
        {"n_obs", R.n_obs},
        {"n_model", R.n_model},
        {"n_units", R.n_units},
        {"n_clusters", R.n_clusters},
        {"n_dropped", R.n_dropped},
        {"level", R.level},
        {"coef", coef},
        {"se", se},
        {"zstat", zs},
        {"pvalue", pv},
        {"ci_lo", lo},
        {"ci_hi", hi},
        {"n_cells", nc},
        {"n_cells_all", nca},
        {"autosample_dropped", R.autosample_dropped},
        {"autosample_message", R.autosample_message},
        {"autosample_coefs", R.autosample_coefs},
        {"time_ms", std::chrono::duration<double, std::milli>(t1 - t0).count()},
        {"ok", R.ok},
        {"error", R.error},
    };
    emit(out, opt(a, "--out"));
    return R.ok ? 0 : 1;
}

}  // namespace cli
}  // namespace cecon
