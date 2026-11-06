// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "common.h"
#include "commands.h"

#include "core/distributions.h"
#include "estimators/varmodel.h"

// P7 wave — var.
//
// ONE verb serves three Stata commands. `cobra var` returns, in a single
// payload: the per-equation results (`var`), the Granger-causality Wald block
// (`vargranger`), and the full IRF array set (`irf create`). PIET renders
// `vargranger` from the stored fit and materializes the `.irf` file — which IS a
// release-118 `.dta` — from the `irf.cells[]` array, whose keys are EXACTLY the
// `.irf` column names so no translation table is needed on either side.
//
// Oracle for every number: Stata 19.5, internal capture
// p7_stata.log (the `preserve / keep if id==1 / tsset t` block of p7.do) plus the
// `_runs/p7_irf.irf` artifact it writes. Assertions — and an explicit statement
// of what that fixture CANNOT pin — in tests/unit/test_varmodel.cpp.

namespace cecon {
namespace cli {

namespace {

// Stata writes `lags(1/2)`. We accept the plain count `2` and the equivalent
// `1/2`, and REJECT anything else (`2/4`, `1 3`, ...): a non-contiguous or
// non-unit-based lag list is a different model, and fitting 1..p while the user
// asked for 2..4 would be a silent wrong answer.
int parse_lags(const std::string& s) {
    const auto slash = s.find('/');
    if (slash == std::string::npos) return std::stoi(s);
    const int lo = std::stoi(s.substr(0, slash)), hi = std::stoi(s.substr(slash + 1));
    if (lo != 1)
        throw EngineError("bad_input",
                          "var: --lags " + s +
                              " — cobra fits contiguous lags 1..p only; a lag list that does not "
                              "start at 1 is a different model and is not silently substituted");
    return hi;
}

json num_or_null(double v) { return std::isfinite(v) ? json(v) : json(nullptr); }

}  // namespace

// cobra var --data f.hbin --y Y,X1,X2 --lags 2 --tsset T [--irf-steps N] ...
int cmd_var(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), ycsv = opt(a, "--y"), lagstr = opt(a, "--lags");
    // parse_tsmeta ALSO reads --xtset, which `var` does not declare and cannot
    // honour: a VAR is a single time series, and a panel spec would silently
    // degrade to "use the time variable, ignore the panel". Rejected here, BEFORE
    // parse_tsmeta can consume the token, so the flag never looks accepted.
    for (const std::string& tok : a)
        if (tok == "--xtset")
            throw EngineError("bad_input",
                              "var: --xtset declares a panel, but `var` fits a single time series — "
                              "use --tsset T (and restrict to one panel with --keepif)");
    TsMeta ts = parse_tsmeta(a);
    if (data_missing(data) || ycsv.empty() || lagstr.empty() || ts.timevar.empty()) {
        std::fprintf(stderr,
                     "usage: cobra var --data f.hbin --y Y,X1,X2 --lags P --tsset T "
                     "[--tsdelta D] [--irf-steps N] [--level P] [--keepif EXPR] [--out f.json]\n"
                     "  Vector autoregression: equation-by-equation OLS on lags 1..P of every\n"
                     "  endogenous variable plus a constant. ONE call returns the per-equation\n"
                     "  results (Stata `var`), the Granger-causality Wald block (`vargranger`)\n"
                     "  and the impulse-response / FEVD arrays (`irf create`, --irf-steps N,\n"
                     "  default 8; pass -1 to skip). The IRF keys are the .irf file's own\n"
                     "  column names (irf/cirf/oirf/coirf/std*/fevd/stdfevd/mse).\n");
        return 2;
    }
    const int lags = parse_lags(lagstr);
    if (lags < 1) throw EngineError("bad_input", "var: --lags must be at least 1");
    const int steps = std::stoi(opt(a, "--irf-steps", "8"));
    const double level = std::stod(opt(a, "--level", "95"));
    if (level < 10.0 || level > 99.99)
        throw EngineError("bad_input", "var: --level must be in [10, 99.99]");
    const double zc = z_critical(level == 95.0 ? 0.05 : 1.0 - level / 100.0);

    std::vector<std::string> vars = split_csv(ycsv);
    if (vars.size() < 2)
        throw EngineError("bad_input", "var: --y needs at least two endogenous variables");
    const int K = static_cast<int>(vars.size());

    data = maybe_keepif(data, opt(a, "--keepif"));
    const Dataset& ds = acquire_data(data);
    const double* traw = require_num(ds, "var", ts.timevar);
    const int64_t n = ds.n_rows();
    // NO listwise compaction here: the lag lookups below are by TIME value, so the
    // series must stay in its original row layout. var_fit drops a row when its own
    // values or any of its p lagged values are missing — which is also what makes a
    // tsset gap behave like a gap instead of silently splicing across it.
    std::vector<double> Y(static_cast<size_t>(n) * static_cast<size_t>(K));
    for (int c = 0; c < K; ++c) {
        const double* col = require_num(ds, "var", vars[static_cast<size_t>(c)]);
        std::copy(col, col + n, Y.data() + static_cast<int64_t>(c) * n);
    }

    VarResult r = var_fit(Y.data(), traw, n, vars, lags, steps, ts.delta);
    if (!r.ok) {
        switch (r.fail) {
            case VarResult::TOO_FEW:
                std::fprintf(stderr,
                             "var: only %lld usable observations for %d parameters per equation — "
                             "too few after forming the lag sample.\n",
                             static_cast<long long>(r.n_obs), r.m);
                return 1;
            case VarResult::SINGULAR:
                std::fprintf(stderr, "var: the lag design is singular (collinear regressors).\n");
                return 1;
            case VarResult::BAD_SPEC:
                std::fprintf(stderr,
                             "var: bad specification — check --lags, --tsdelta, and that the time "
                             "variable has no repeated values.\n");
                return 1;
            default:
                std::fprintf(stderr, "var: estimation failed.\n");
                return 1;
        }
    }

    json equations = json::array();
    for (const VarEquation& eq : r.eqs) {
        json coefs = json::array();
        for (int j = 0; j < r.m; ++j) {
            const double b = eq.b[static_cast<size_t>(j)], se = eq.se[static_cast<size_t>(j)];
            const double z = (se > 0) ? b / se : 0.0;
            coefs.push_back({{"name", r.rnames[static_cast<size_t>(j)]},
                             {"b", b},
                             {"se", se},
                             {"z", z},
                             {"p", (se > 0) ? z_twosided_pvalue(z)
                                            : 1.0},
                             {"ci_lo", b - zc * se},
                             {"ci_hi", b + zc * se}});
        }
        equations.push_back({{"name", eq.name},
                             {"parms", r.m},
                             {"rmse", eq.rmse},
                             {"r2", eq.r2},
                             // A degenerate equation reports chi2 = null with df 0 —
                             // Stata's `.` — instead of a number pseudo-inverted out
                             // of a numerically singular block.
                             {"chi2", num_or_null(eq.chi2)},
                             {"chi2_df", eq.chi2_df},
                             {"p_chi2", num_or_null(eq.p)},
                             {"degenerate", eq.degenerate},
                             {"coefs", coefs}});
    }

    json granger = json::array();
    for (const VarGrangerRow& g : r.granger)
        granger.push_back({{"equation", g.equation},
                           {"excluded", g.excluded},
                           {"chi2", num_or_null(g.chi2)},
                           {"df", g.df},
                           {"p", num_or_null(g.p)}});

    json sigma = json::array();
    for (int i = 0; i < K; ++i) {
        json row = json::array();
        for (int j = 0; j < K; ++j) row.push_back(r.sigma[static_cast<size_t>(i) * K + j]);
        sigma.push_back(row);
    }

    json out = {{"estimator", "var"},
                {"vars", vars},
                {"regressors", r.rnames},
                {"lags", r.lags},
                {"n_obs", r.n_obs},
                {"parms_per_eq", r.m},
                {"tvar", ts.timevar},
                {"t_first", r.t_first},
                {"t_last", r.t_last},
                {"level", level},
                {"ll", num_or_null(r.ll)},
                {"det_sigma", num_or_null(r.det_sigma)},
                {"fpe", num_or_null(r.fpe)},
                {"aic", num_or_null(r.aic)},
                {"hqic", num_or_null(r.hqic)},
                {"sbic", num_or_null(r.sbic)},
                {"sigma", sigma},
                {"equations", equations},
                {"granger", granger}};

    if (steps >= 0) {
        json cells = json::array();
        for (const VarIrfCell& c : r.irf) {
            // Keys below are the .irf file's own column names. The structural-VAR
            // columns (sirf/dm/cdm/sfevd and their std* partners) are ALL missing
            // for a plain `var` and are emitted as null so the frontend can write
            // the full 23-variable schema without inventing values.
            cells.push_back({{"step", c.step},
                             {"response", vars[static_cast<size_t>(c.response)]},
                             {"impulse", vars[static_cast<size_t>(c.impulse)]},
                             {"irf", c.irf},
                             {"cirf", c.cirf},
                             {"oirf", c.orth_ok ? json(c.oirf) : json(nullptr)},
                             {"coirf", c.orth_ok ? json(c.coirf) : json(nullptr)},
                             {"stdirf", c.stdirf},
                             {"stdcirf", c.stdcirf},
                             {"stdoirf", c.orth_ok ? json(c.stdoirf) : json(nullptr)},
                             {"stdcoirf", c.orth_ok ? json(c.stdcoirf) : json(nullptr)},
                             {"fevd", c.orth_ok ? json(c.fevd) : json(nullptr)},
                             {"stdfevd", c.orth_ok ? json(c.stdfevd) : json(nullptr)},
                             {"mse", c.orth_ok ? json(c.mse) : json(nullptr)},
                             {"sirf", nullptr},
                             {"dm", nullptr},
                             {"cdm", nullptr},
                             {"sfevd", nullptr},
                             {"stdsirf", nullptr},
                             {"stddm", nullptr},
                             {"stdcdm", nullptr},
                             {"stdsfevd", nullptr}});
        }
        out["irf"] = {{"steps", r.irf_steps},
                      {"order", vars},  // Cholesky ordering = the --y varlist order
                      {"orthogonalized", r.sigma_pd},
                      {"cells", cells}};
        if (!r.sigma_pd)
            std::fprintf(stderr,
                         "var: Sigma is not numerically positive definite — the orthogonalized "
                         "IRF/FEVD columns are undefined and are reported as missing.\n");
    }

    emit(out, opt(a, "--out"));
    return 0;
}

}  // namespace cli
}  // namespace cecon
