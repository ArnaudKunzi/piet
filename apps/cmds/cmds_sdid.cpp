// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "common.h"
#include "commands.h"

#include "estimators/sdid.h"

// P-QE wave — sdid (synthetic difference-in-differences), AAHIW21. See
// src/estimators/sdid.h for the method sources and
// the conventions the paper leaves open that this implementation pins.
//
// The JSON below carries EVERY printed quantity as a named field (att/se/t/p/
// ci_lo/ci_hi), the full N accounting, the per-cohort decomposition and the
// bootstrap replicate log, because the PIET layer renders from this JSON and
// cannot invent what the engine omits.
//
// Out of scope this wave, and REFUSED BY NAME rather than silently ignored
// (silent ignore is the cardinal defect here): covariates (--x, Stata's
// covariates()), method() variants, and the passing path of vce(placebo) — only
// placebo's refusal, when the controls do not outnumber the treated units, is
// implemented. vce(jackknife) is likewise named and refused.

namespace cecon {
namespace cli {

int cmd_sdid(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data");
    std::string y = opt(a, "--y");
    std::string ivar = opt(a, "--ivar");
    std::string tvar = opt(a, "--time");
    std::string wvar = opt(a, "--treated");
    std::string vce = opt(a, "--vce", "bootstrap");
    std::string xcsv = opt(a, "--x");
    std::string method = opt(a, "--method");
    if (data_missing(data) || y.empty() || ivar.empty() || tvar.empty() || wvar.empty()) {
        std::fprintf(stderr,
            "usage: cobra sdid --data f.hbin --y Y --ivar ID --time T --treated W "
            "[--vce bootstrap|noinference] [--reps 50] [--seed 0] [--level 95] "
            "[--keepif EXPR] [--out f.json]\n");
        return 2;
    }
    // Named refusals. sdid's published estimator (AAHIW21) is the no-covariate,
    // no-method-variant one; accepting and ignoring either option would silently
    // answer a different question than the one asked.
    if (!xcsv.empty())
        throw cecon::EngineError(
            "bad_input",
            // BUG-182: name the FLAG, like every other refuse-by-name site — the
            // detail field carried only the value, so the message never said which
            // option was refused.
            "sdid: --x (covariates) is not supported — this release implements AAHIW21's "
            "estimator without the covariate-adjustment extension, and running without "
            "the covariates you asked for would answer a different question. Residualize "
            "the outcome first, or use csdid",
            xcsv);
    if (!method.empty())
        throw cecon::EngineError(
            "bad_input",
            "sdid: --method is not supported — this release implements only AAHIW21's "
            "synthetic diff-in-diff estimator itself (Stata sdid's method(sdid) default); "
            "method(did) and method(sc) are not implemented",
            method);

    int reps = std::stoi(opt(a, "--reps", "50"));
    if (reps < 1) { std::fprintf(stderr, "sdid: --reps must be >= 1.\n"); return 2; }
    uint64_t seed = std::stoull(opt(a, "--seed", "0"));
    double level = std::stod(opt(a, "--level", "95"));
    if (!(level > 0 && level < 100)) {
        std::fprintf(stderr, "sdid: --level must be in (0,100).\n");
        return 2;
    }

    data = maybe_keepif(data, opt(a, "--keepif"));
    const Dataset& ds = acquire_data(data);
    const double* yv = require_num(ds, "sdid", y);
    const double* iv = require_num(ds, "sdid", ivar);
    const double* tv = require_num(ds, "sdid", tvar);
    const double* wv = require_num(ds, "sdid", wvar);
    const int64_t n_obs = ds.n_rows();

    SdidConfig cfg;
    cfg.vce = vce;
    cfg.reps = reps;
    cfg.seed = seed;
    cfg.level = level;

    auto t0 = std::chrono::steady_clock::now();
    SdidResult R = sdid_run(yv, iv, tv, wv, n_obs, cfg);
    auto t1 = std::chrono::steady_clock::now();

    // A refusal is LOUD: the error envelope on stderr plus a nonzero exit, never a
    // result object carrying a missing or plausible-looking ATT. `detail` carries
    // the machine-readable diagnostic tag so a frontend can branch on WHY.
    if (!R.ok)
        throw cecon::EngineError("bad_input", R.error, sdid_diagnostic_name(R.diagnostic));

    json cohorts_j = json::array();
    for (const auto& c : R.cohorts) {
        cohorts_j.push_back({
            {"adopt", c.adopt},
            {"n_treated", c.n_treated},
            {"n_control", c.n_control},
            {"t_pre", c.t_pre},
            {"t_post", c.t_post},
            {"zeta", c.zeta},
            {"tau", c.tau},
            {"weight", c.weight},
        });
    }
    json out = {
        {"estimator", "sdid"},
        {"y", y},
        {"ivar", ivar},
        {"time", tvar},
        {"treated", wvar},
        {"vce", vce},
        {"reps", reps},
        {"seed", seed},
        {"level", level},
        {"att", R.att},
        {"se", R.se},
        {"t", R.t},
        {"p", R.p},
        {"ci_lo", R.ci_lo},
        {"ci_hi", R.ci_hi},
        {"n_obs", R.n_obs},
        {"n_units", R.n_units},
        {"n_treated_units", R.n_treated_units},
        {"n_control_units", R.n_control_units},
        {"n_periods", R.n_periods},
        {"cohorts", cohorts_j},
        {"reps_done", R.reps_done},
        {"reps_redrawn", R.reps_redrawn},
        {"rep_att", R.rep_att},
        {"diagnostic", sdid_diagnostic_name(R.diagnostic)},
        {"time_ms", std::chrono::duration<double, std::milli>(t1 - t0).count()},
        {"ok", R.ok},
        {"error", R.error},
    };
    emit(out, opt(a, "--out"));
    return 0;
}

}  // namespace cli
}  // namespace cecon
