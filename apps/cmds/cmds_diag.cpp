// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "common.h"
#include "commands.h"

#include "estimators/diagtests.h"
#include "estimators/ts_diag.h"
#include "estimators/iv_diagnostics.h"
#include "estimators/ols.h"

namespace cecon {
namespace cli {

// estat hettest — Breusch-Pagan / Cook-Weisberg heteroskedasticity test after OLS.
// Default z = fitted values (Cook-Weisberg, df=1); --rhs uses the regressors; --z
// VARLIST uses supplied variables. Reports the Cook-Weisberg "normal" chi2 (default),
// Koenker's iid N*R^2 chi2, and Wooldridge's F form — all in one shot.
int cmd_hettest(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), y = opt(a, "--y"), xcsv = opt(a, "--x");
    std::string zcsv = opt(a, "--z");
    bool rhs = has(a, "--rhs");
    if (data_missing(data) || y.empty() || xcsv.empty()) {
        std::fprintf(stderr,
            "usage: cobra hettest --data f.hbin --y Y --x X1,X2 [--rhs | --z Z1,Z2 | --white] "
            "[--keepif EXPR] [--out f.json]\n"
            "  Breusch-Pagan / Cook-Weisberg (default, tests fitted values, df=1);\n"
            "  --white gives White's general test (n*R^2 on X + cross products).\n");
        return 2;
    }
    data = maybe_keepif(data, opt(a, "--keepif"));
    const Dataset& ds = acquire_data(data);
    GlmInputs g = load_glm(ds, "hettest", y, split_csv(xcsv));
    if (g.n == 0) { std::fprintf(stderr, "hettest: no rows after dropping missing.\n"); return 1; }
    int intercept_col = g.k - 1;  // load_glm puts the constant last

    // White's general test (estat imtest, white): n*R^2 of e^2 on X + cross products.
    if (has(a, "--white")) {
        WhiteResult w = white_test(g.X.data(), g.n, g.k, g.y.data(), intercept_col);
        if (!w.ok) { std::fprintf(stderr, "hettest --white: aux regression failed.\n"); return 1; }
        json out = {{"test", "white"}, {"n_obs", g.n}, {"df", w.df}, {"chi2", w.chi2},
                    {"p", chi2_pvalue(w.chi2, w.df)}};
        emit(out, opt(a, "--out"));
        return 0;
    }

    BpZ zmode = BpZ::Fitted;
    std::vector<double> customZ;
    int mcz = 0;
    std::vector<std::string> znames;
    if (!zcsv.empty()) {
        zmode = BpZ::Custom;
        znames = split_csv(zcsv);
        mcz = static_cast<int>(znames.size());
        customZ.assign(static_cast<size_t>(g.n) * mcz, 0.0);
        for (int c = 0; c < mcz; ++c) {
            const double* zr = require_num(ds, "hettest", znames[static_cast<size_t>(c)]);
            auto col = gather_rows(zr, g.keep);
            std::copy(col.begin(), col.end(), customZ.data() + static_cast<int64_t>(c) * g.n);
        }
    } else if (rhs) {
        zmode = BpZ::Rhs;
    }

    BpResult r = breusch_pagan(g.X.data(), g.n, g.k, g.y.data(), zmode,
                               customZ.empty() ? nullptr : customZ.data(), mcz, intercept_col);
    if (!r.ok) { std::fprintf(stderr, "hettest: aux regression failed (collinear z?).\n"); return 1; }

    std::string against = (zmode == BpZ::Fitted) ? "fitted"
                        : (zmode == BpZ::Rhs)    ? "rhs" : "varlist";
    json out = {{"test", "breusch_pagan_cook_weisberg"}, {"n_obs", g.n}, {"df", r.df},
                {"against", against},
                {"chi2", r.chi2_normal}, {"p", chi2_pvalue(r.chi2_normal, r.df)},
                {"chi2_iid", r.chi2_iid}, {"p_iid", chi2_pvalue(r.chi2_iid, r.df)},
                {"F", r.F}, {"df_r", r.df_r}, {"p_F", f_pvalue(r.F, r.df, r.df_r)}};
    if (zmode == BpZ::Custom) out["z"] = znames;
    emit(out, opt(a, "--out"));
    return 0;
}

// estat ovtest — Ramsey RESET after OLS. Default adds powers 2,3,4 of the (rescaled)
// fitted values and F-tests their joint significance.
int cmd_ovtest(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), y = opt(a, "--y"), xcsv = opt(a, "--x");
    if (data_missing(data) || y.empty() || xcsv.empty()) {
        std::fprintf(stderr,
            "usage: cobra ovtest --data f.hbin --y Y --x X1,X2 [--keepif EXPR] [--out f.json]\n"
            "  Ramsey RESET using powers 2,3,4 of the fitted values.\n");
        return 2;
    }
    data = maybe_keepif(data, opt(a, "--keepif"));
    const Dataset& ds = acquire_data(data);
    GlmInputs g = load_glm(ds, "ovtest", y, split_csv(xcsv));
    if (g.n == 0) { std::fprintf(stderr, "ovtest: no rows after dropping missing.\n"); return 1; }
    ResetResult r = ramsey_reset(g.X.data(), g.n, g.k, g.y.data());
    if (!r.ok) { std::fprintf(stderr, "ovtest: aux regression failed.\n"); return 1; }
    json out = {{"test", "ramsey_reset"}, {"n_obs", g.n}, {"df_m", r.df_m}, {"df_r", r.df_r},
                {"F", r.F}, {"p", f_pvalue(r.F, r.df_m, r.df_r)}};
    emit(out, opt(a, "--out"));
    return 0;
}

namespace {

// Parse a Stata-style numlist of lags ("1 2 3" or "1,2,3"); default {1}. Only used
// by durbinalt/bgodfrey/archlm below (same TU) — no external linkage needed.
std::vector<int> parse_lags(const std::string& s) {
    std::vector<int> out;
    std::string t = s;
    for (char& c : t) if (c == ',') c = ' ';
    std::stringstream ss(t);
    int v;
    while (ss >> v) if (v > 0) out.push_back(v);
    if (out.empty()) out.push_back(1);
    return out;
}

}  // namespace

// estat dwatson — Durbin-Watson d (runs OLS internally; data must be time-ordered).
int cmd_dwatson(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), y = opt(a, "--y"), xcsv = opt(a, "--x");
    if (data_missing(data) || y.empty() || xcsv.empty()) {
        std::fprintf(stderr, "usage: cobra dwatson --data f.hbin --y Y --x X1,X2 [--keepif EXPR] [--out f.json]\n"
                             "  Durbin-Watson d (rows must be in time order, no gaps).\n");
        return 2;
    }
    data = maybe_keepif(data, opt(a, "--keepif"));
    const Dataset& ds = acquire_data(data);
    GlmInputs g = load_glm(ds, "dwatson", y, split_csv(xcsv));
    if (g.n == 0) { std::fprintf(stderr, "dwatson: no rows after dropping missing.\n"); return 1; }
    DwResult r = durbin_watson(g.X.data(), g.n, g.k, g.y.data());
    if (!r.ok) { std::fprintf(stderr, "dwatson: regression failed.\n"); return 1; }
    json out = {{"test", "durbin_watson"}, {"dw", r.dw}, {"n_obs", r.n}, {"k", r.k}};
    emit(out, opt(a, "--out"));
    return 0;
}

// estat durbinalt — Durbin's alternative test (Wald on lagged-residual coefs).
int cmd_durbinalt(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), y = opt(a, "--y"), xcsv = opt(a, "--x");
    bool small = has(a, "--small"), robust = has(a, "--robust"), nomiss0 = has(a, "--nomiss0");
    if (data_missing(data) || y.empty() || xcsv.empty()) {
        std::fprintf(stderr, "usage: cobra durbinalt --data f.hbin --y Y --x X1,X2 [--lags \"1 2\"] "
                             "[--small] [--robust] [--nomiss0] [--keepif EXPR] [--out f.json]\n");
        return 2;
    }
    data = maybe_keepif(data, opt(a, "--keepif"));
    const Dataset& ds = acquire_data(data);
    GlmInputs g = load_glm(ds, "durbinalt", y, split_csv(xcsv));
    if (g.n == 0) { std::fprintf(stderr, "durbinalt: no rows after dropping missing.\n"); return 1; }
    std::vector<double> e;
    if (!ts_ols_residuals(g.X.data(), g.n, g.k, g.y.data(), e)) {
        std::fprintf(stderr, "durbinalt: regression failed.\n"); return 1;
    }
    bool fform = small || robust;
    json lags = json::array(), stat = json::array(), df = json::array(),
         dfr = json::array(), pv = json::array();
    for (int lag : parse_lags(opt(a, "--lags"))) {
        SerialResult r = durbinalt_one(e, g.X.data(), g.n, g.k, lag, small, robust, nomiss0);
        if (!r.ok) { std::fprintf(stderr, "durbinalt: aux regression failed at lag %d.\n", lag); return 1; }
        lags.push_back(lag); df.push_back(r.df); pv.push_back(r.p);
        stat.push_back(fform ? r.F : r.chi2);
        if (fform) dfr.push_back(r.df_r);
    }
    json out = {{"test", "durbinalt"}, {"vcov", robust ? "robust" : "iid"},
                {"form", fform ? "F" : "chi2"}, {"n_obs", g.n}, {"k", g.k},
                {"lags", lags}, {fform ? "F" : "chi2", stat}, {"df", df}, {"p", pv}};
    if (fform) out["df_r"] = dfr;
    emit(out, opt(a, "--out"));
    return 0;
}

// estat bgodfrey — Breusch-Godfrey LM test (N*R^2 of the same aux regression).
int cmd_bgodfrey(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), y = opt(a, "--y"), xcsv = opt(a, "--x");
    bool small = has(a, "--small"), nomiss0 = has(a, "--nomiss0");
    if (data_missing(data) || y.empty() || xcsv.empty()) {
        std::fprintf(stderr, "usage: cobra bgodfrey --data f.hbin --y Y --x X1,X2 [--lags \"1 2\"] "
                             "[--small] [--nomiss0] [--keepif EXPR] [--out f.json]\n");
        return 2;
    }
    data = maybe_keepif(data, opt(a, "--keepif"));
    const Dataset& ds = acquire_data(data);
    GlmInputs g = load_glm(ds, "bgodfrey", y, split_csv(xcsv));
    if (g.n == 0) { std::fprintf(stderr, "bgodfrey: no rows after dropping missing.\n"); return 1; }
    std::vector<double> e;
    if (!ts_ols_residuals(g.X.data(), g.n, g.k, g.y.data(), e)) {
        std::fprintf(stderr, "bgodfrey: regression failed.\n"); return 1;
    }
    json lags = json::array(), stat = json::array(), df = json::array(),
         dfr = json::array(), pv = json::array();
    for (int lag : parse_lags(opt(a, "--lags"))) {
        SerialResult r = bgodfrey_one(e, g.X.data(), g.n, g.k, lag, small, nomiss0);
        if (!r.ok) { std::fprintf(stderr, "bgodfrey: aux regression failed at lag %d.\n", lag); return 1; }
        lags.push_back(lag); df.push_back(r.df); pv.push_back(r.p);
        stat.push_back(small ? r.F : r.chi2);
        if (small) dfr.push_back(r.df_r);
    }
    json out = {{"test", "bgodfrey"}, {"form", small ? "F" : "chi2"}, {"n_obs", g.n}, {"k", g.k},
                {"lags", lags}, {small ? "F" : "chi2", stat}, {"df", df}, {"p", pv}};
    if (small) out["df_r"] = dfr;
    emit(out, opt(a, "--out"));
    return 0;
}

// estat archlm — Engle ARCH LM test on squared residuals (chi2 only).
int cmd_archlm(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), y = opt(a, "--y"), xcsv = opt(a, "--x");
    if (data_missing(data) || y.empty() || xcsv.empty()) {
        std::fprintf(stderr, "usage: cobra archlm --data f.hbin --y Y --x X1,X2 [--lags \"1 2 3\"] "
                             "[--keepif EXPR] [--out f.json]\n");
        return 2;
    }
    data = maybe_keepif(data, opt(a, "--keepif"));
    const Dataset& ds = acquire_data(data);
    GlmInputs g = load_glm(ds, "archlm", y, split_csv(xcsv));
    if (g.n == 0) { std::fprintf(stderr, "archlm: no rows after dropping missing.\n"); return 1; }
    std::vector<double> e;
    if (!ts_ols_residuals(g.X.data(), g.n, g.k, g.y.data(), e)) {
        std::fprintf(stderr, "archlm: regression failed.\n"); return 1;
    }
    json lags = json::array(), stat = json::array(), df = json::array(), pv = json::array();
    for (int lag : parse_lags(opt(a, "--lags"))) {
        SerialResult r = archlm_one(e, g.n, lag);
        if (!r.ok) { std::fprintf(stderr, "archlm: aux regression failed at lag %d.\n", lag); return 1; }
        lags.push_back(lag); df.push_back(r.df); pv.push_back(r.p); stat.push_back(r.chi2);
    }
    json out = {{"test", "archlm"}, {"n_obs", g.n}, {"lags", lags}, {"chi2", stat},
                {"df", df}, {"p", pv}};
    emit(out, opt(a, "--out"));
    return 0;
}

// iv-diag — classic i.i.d. IV postestimation: estat endogenous (Durbin/Wu-Hausman),
// estat overid (Sargan/Basmann), estat firststage (first-stage F + partial R²).
int cmd_ivdiag(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), y = opt(a, "--y"), endog = opt(a, "--endog");
    std::string inst = opt(a, "--instruments"), xcsv = opt(a, "--x");
    if (data_missing(data) || y.empty() || endog.empty() || inst.empty()) {
        std::fprintf(stderr, "usage: cobra iv-diag --data f.hbin --y Y --endog D1[,D2] "
                             "--instruments Z1,Z2 [--x EXOG] [--keepif EXPR] [--out f.json]\n"
                             "  i.i.d. Durbin/Wu-Hausman endogeneity + Sargan/Basmann overid + first stage.\n");
        return 2;
    }
    data = maybe_keepif(data, opt(a, "--keepif"));
    const Dataset& ds = acquire_data(data);
    auto endogs = split_csv(endog), insts = split_csv(inst), exogs = split_csv(xcsv);
    int p1 = static_cast<int>(endogs.size()), m = static_cast<int>(insts.size());
    int kx = static_cast<int>(exogs.size()) + 1;  // +constant
    int64_t n_raw = ds.n_rows();
    const double* yraw = require_num(ds, "iv-diag", y);
    std::vector<const double*> all = {yraw};
    std::vector<const double*> Yr, Xr, Zr;
    for (auto& s : endogs) { Yr.push_back(require_num(ds, "iv-diag", s)); all.push_back(Yr.back()); }
    for (auto& s : exogs)  { Xr.push_back(require_num(ds, "iv-diag", s)); all.push_back(Xr.back()); }
    for (auto& s : insts)  { Zr.push_back(require_num(ds, "iv-diag", s)); all.push_back(Zr.back()); }
    auto keep = listwise_keep(all, n_raw);
    int64_t n = static_cast<int64_t>(keep.size());
    if (n == 0) { std::fprintf(stderr, "iv-diag: no rows after dropping missing.\n"); return 1; }
    auto gv = [&](const double* p) { return gather_rows(p, keep); };
    std::vector<double> yv = gv(yraw);
    std::vector<double> Y(static_cast<size_t>(n) * p1), X(static_cast<size_t>(n) * kx),
        Z2(static_cast<size_t>(n) * m);
    for (int c = 0; c < p1; ++c) { auto col = gv(Yr[c]); std::copy(col.begin(), col.end(), Y.data() + static_cast<int64_t>(c) * n); }
    for (int c = 0; c + 1 < kx; ++c) { auto col = gv(Xr[c]); std::copy(col.begin(), col.end(), X.data() + static_cast<int64_t>(c) * n); }
    for (int64_t i = 0; i < n; ++i) X[static_cast<int64_t>(kx - 1) * n + i] = 1.0;  // constant last
    for (int c = 0; c < m; ++c) { auto col = gv(Zr[c]); std::copy(col.begin(), col.end(), Z2.data() + static_cast<int64_t>(c) * n); }

    IvDiag r = iv_diagnostics(yv, n, Y, p1, X, kx, Z2, m);
    if (!r.ok) { std::fprintf(stderr, "iv-diag: computation failed (collinear / underidentified?).\n"); return 1; }
    json fs = json::array();
    for (int j = 0; j < p1; ++j)
        fs.push_back({{"endog", endogs[j]}, {"F", r.fs_F[j]}, {"p", r.fs_F_p[j]},
                      {"r2", r.fs_r2[j]}, {"partial_r2", r.fs_partial_r2[j]}});
    json out = {{"estimator", "iv-diag"}, {"n_obs", n},
                {"endogeneity", {{"durbin", r.durbin}, {"durbin_df", r.durbin_df}, {"durbin_p", r.durbin_p},
                                 {"wu_hausman", r.wu}, {"wu_df", r.wu_df}, {"wu_df_r", r.wu_df_r}, {"wu_p", r.wu_p}}},
                {"firststage", fs}};
    if (r.overid_ok)
        out["overid"] = {{"sargan", r.sargan}, {"sargan_p", r.sargan_p}, {"basmann", r.basmann},
                         {"basmann_p", r.basmann_p}, {"df", r.overid_df}};
    else
        out["overid"] = {{"note", "just-identified (no overidentifying restrictions)"}};
    emit(out, opt(a, "--out"));
    return 0;
}

// ── vif (after OLS) ──────────────────────────────────────────────────────────
int cmd_vif(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), y = opt(a, "--y"), xcsv = opt(a, "--x");
    if (data_missing(data) || y.empty() || xcsv.empty()) {
        std::fprintf(stderr, "usage: cobra vif --data f.hbin --y Y --x X1,X2,..  (VIF_j = 1/(1-R2_j))\n");
        return 2;
    }
    data = maybe_keepif(data, opt(a, "--keepif"));
    const Dataset& ds = acquire_data(data);
    std::vector<std::string> xs = split_csv(xcsv);
    DesignMatrix dm = build_design(ds, y, xs, true);
    OlsResult mr = ols_fit(dm);
    json coefs = json::array(), tol = json::object(), r2aux = json::object();
    double sumv = 0;
    for (size_t j = 0; j < xs.size(); ++j) {
        double r2j = 0.0;
        if (xs.size() > 1) {
            std::vector<std::string> others;
            for (size_t l = 0; l < xs.size(); ++l) if (l != j) others.push_back(xs[l]);
            DesignMatrix d = build_design(ds, xs[j], others, true);
            r2j = ols_fit(d).r2;
        }
        double v = 1.0 / (1.0 - r2j);
        coefs.push_back({{"name", xs[j]}, {"b", v}});
        tol[xs[j]] = 1.0 - r2j;
        r2aux[xs[j]] = r2j;
        sumv += v;
    }
    json out = {{"estimator", "vif"}, {"depvar", y}, {"n_obs", mr.n}, {"coefs", coefs},
                {"tolerance", tol}, {"r2_aux", r2aux},
                {"mean_vif", xs.empty() ? 0.0 : sumv / xs.size()}, {"r2", mr.r2}};
    emit(out, opt(a, "--out"));
    return 0;
}

}  // namespace cli
}  // namespace cecon
