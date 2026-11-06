// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "common.h"
#include "commands.h"

#include "estimators/neweyprais.h"
#include "estimators/posthest.h"  // wald_test (newey's model F)
#include "estimators/unitroot.h"
#include "estimators/ts_diag.h"

namespace cecon {
namespace cli {

namespace {

// Shared loader for the time-series regressions (newey/prais): listwise X,y over
// (y, x..., timevar[, panelvar]), returning the design plus the time column and,
// after xtset (BUG-1503 / BUG-1507), the panel column (empty for one series).
// Only used by cmd_newey/cmd_prais below (same TU) — no external linkage needed.
struct TsRegInputs {
    GlmInputs g;
    std::vector<double> time, panel;
    const double* panel_or_null() const { return panel.empty() ? nullptr : panel.data(); }
};
TsRegInputs load_tsreg(const Dataset& ds, const char* cmd, const std::string& y,
                       const std::vector<std::string>& xs, const std::string& tvar,
                       const std::string& pvar) {
    TsRegInputs r;
    const double* traw = require_num(ds, cmd, tvar);
    int64_t n_raw = ds.n_rows();
    const double* yraw = require_num(ds, cmd, y);
    int kx = static_cast<int>(xs.size());
    std::vector<const double*> all = {yraw, traw};
    const double* praw = pvar.empty() ? nullptr : require_num(ds, cmd, pvar);
    if (praw) all.push_back(praw);
    std::vector<const double*> xraw(kx);
    for (int c = 0; c < kx; ++c) { xraw[c] = require_num(ds, cmd, xs[c]); all.push_back(xraw[c]); }
    auto keep = listwise_keep(all, n_raw);
    GlmInputs& g = r.g;
    g.keep = keep;
    g.n = static_cast<int64_t>(keep.size());
    g.k = kx + 1;
    g.X.assign(static_cast<size_t>(g.n) * g.k, 0.0);
    for (int c = 0; c < kx; ++c) {
        auto col = gather_rows(xraw[c], keep);
        std::copy(col.begin(), col.end(), g.X.data() + static_cast<int64_t>(c) * g.n);
        g.names.push_back(xs[static_cast<size_t>(c)]);
    }
    for (int64_t i = 0; i < g.n; ++i) g.X[static_cast<int64_t>(kx) * g.n + i] = 1.0;
    g.names.push_back("(intercept)");
    g.y = gather_rows(yraw, keep);
    r.time = gather_rows(traw, keep);
    if (praw) r.panel = gather_rows(praw, keep);
    return r;
}

}  // namespace

// newey — Newey-West HAC OLS. lag(0) reproduces regress, vce(robust).
int cmd_newey(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), y = opt(a, "--y"), xcsv = opt(a, "--x"), tvar = opt(a, "--timevar");
    if (data_missing(data) || y.empty() || xcsv.empty() || tvar.empty() || opt(a, "--lag").empty()) {
        std::fprintf(stderr, "usage: cobra newey --data f.hbin --y Y --x X1,X2 --timevar T --lag M "
                             "[--panelvar P] [--force] [--delta D] [--keepif EXPR] "
                             "[--esample-out PATH [--esample-index COL --esample-n N]] [--out f.json]\n"
                             "  Newey-West HAC SE (Bartlett kernel, lag M); lag 0 == robust OLS.\n"
                             "  --panelvar P  xtset panel: lags pair rows of one panel only.\n"
                             "  --force  fit although the time variable is not regularly spaced.\n"
                             "  --esample-out PATH  write e(sample), one 1/0 line per --data row (see feols).\n");
        return 2;
    }
    int lag = std::stoi(opt(a, "--lag"));
    double delta = std::stod(opt(a, "--delta", "1"));
    const std::string pvar = opt(a, "--panelvar");
    const bool force = has(a, "--force");
    const EsampleRequest esreq = esample_request(a, "newey");
    const std::string src_data = data, keepif = opt(a, "--keepif");
    data = maybe_keepif(data, keepif);
    const Dataset& ds = acquire_data(data);
    TsRegInputs in = load_tsreg(ds, "newey", y, split_csv(xcsv), tvar, pvar);
    // BUG-1269: r(2000) "no observations". One row is not r(2001) here: StataBE
    // refuses it as "t is regularly spaced, but does not have intervals of 1"
    // (the Checkt block below).
    if (in.g.n == 0) require_sample(in.g.n);
    // newey.ado's Checkt (BUG-1503): the sample's time values sorted (panels pooled),
    // successive differences in units of delta. Unequal differences are "not
    // regularly spaced" (r(198) when lag > 0, unless force); equal differences
    // above 1 are refused even with force. After `xtset id t` the time variable
    // repeats across panels, so the differences are 0 and 1 (StataBE 19.5, i3.do).
    {
        std::vector<double> ts = in.time;
        std::sort(ts.begin(), ts.end());
        double dmin = 0, dmax = 0, dsum = 0;
        for (size_t i = 1; i < ts.size(); ++i) {
            const double d = (ts[i] - ts[i - 1]) / delta;
            if (i == 1 || d < dmin) dmin = d;
            if (i == 1 || d > dmax) dmax = d;
            dsum += d;
        }
        // One row leaves no difference: Stata's missing min/max/mean read as equal and > 1.
        const bool none = ts.size() < 2;
        const bool unequal = !none && dmin != dmax;
        if (none || (!unequal && dsum / static_cast<double>(ts.size() - 1) > 1.0))
            throw EngineError("bad_input", tvar + " is regularly spaced, but does not have intervals of 1",
                              "ts_not_regular");
        if (unequal && lag > 0 && !force)
            throw EngineError("bad_input", tvar + " is not regularly spaced", "ts_not_regular");
    }
    NeweyResult r = newey_fit(in.g.X.data(), in.g.n, in.g.k, in.g.y.data(), in.time.data(), delta, lag,
                              in.panel_or_null());
    if (!r.ok) { std::fprintf(stderr, "newey: regression failed (collinear?).\n"); return 1; }
    if (r.short_lag_pairs == 0) throw EngineError("no_obs", "no observations", "no_obs");
    if (r.short_lag_pairs == 1)
        throw EngineError("no_obs", "insufficient observations", "insufficient_obs");
    double dof = static_cast<double>(r.n - r.k);
    double tc = (dof > 0) ? t_critical(0.05, dof) : 0.0;
    json coefs = json::array();
    for (int j = 0; j < r.k; ++j) {
        double se = r.se[j], t = (se > 0) ? r.beta[j] / se : 0.0;
        coefs.push_back({{"name", in.g.names[j]}, {"b", r.beta[j]}, {"se", se}, {"t", t},
                         {"p", (se > 0 && dof > 0) ? t_twosided_pvalue(t, dof) : 1.0},
                         {"ci_lo", r.beta[j] - tc * se}, {"ci_hi", r.beta[j] + tc * se}});
    }
    json out = {{"estimator", "newey"}, {"lag", lag}, {"n_obs", r.n}, {"df_r", dof}, {"coefs", coefs}};
    // e(df_m)/e(F): newey.ado's `test indv, min` on the HAC V, the slopes (every
    // coefficient but the intercept, which is last) jointly zero; F = W / q over df_r.
    // StataBE 19.5 (BUG-1503 i3.do): F(2, 197) = 187.20427640633693.
    const int q = r.k - 1;
    if (q > 0) {
        std::vector<double> R(static_cast<size_t>(q) * r.k, 0.0);  // q x k row-major
        for (int j = 0; j < q; ++j) R[static_cast<size_t>(j) * r.k + j] = 1.0;
        const WaldResult w = wald_test(r.beta.data(), r.vcov.data(), r.k, R.data(), nullptr, q);
        out["df_m"] = q;
        if (w.ok && std::isfinite(w.chi2) && dof > 0) {
            out["F"] = w.chi2 / q;
            out["p_F"] = f_pvalue(w.chi2 / q, static_cast<double>(q), dof);
        }
    }
    // --emit-v (BUG-1196): the fit's full HAC covariance, in the coefficients' order.
    if (has(a, "--emit-v")) add_e_v(out, in.g.names, r.vcov);
    // e(sample) T1a: newey's sample is load_tsreg's listwise set, all of it fitted.
    if (esreq.active()) {
        EsampleRows rows;
        rows.keep = std::move(in.g.keep);
        rows.loaded = &ds;
        emit_esample(esreq, "newey", src_data, keepif, false, rows, out);
    }
    emit(out, opt(a, "--out"));
    return 0;
}

// prais — Prais-Winsten / Cochrane-Orcutt AR(1) FGLS.
int cmd_prais(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), y = opt(a, "--y"), xcsv = opt(a, "--x"), tvar = opt(a, "--timevar");
    bool corc = has(a, "--corc"), twostep = has(a, "--twostep");
    std::string rhotype = opt(a, "--rhotype", "regress");
    if (data_missing(data) || y.empty() || xcsv.empty() || tvar.empty()) {
        std::fprintf(stderr, "usage: cobra prais --data f.hbin --y Y --x X1,X2 --timevar T "
                             "[--panelvar P] [--corc] [--twostep] [--rhotype regress|tscorr] [--delta D] "
                             "[--keepif EXPR] [--esample-out PATH [--esample-index COL --esample-n N]] "
                             "[--out f.json]\n"
                             "  --panelvar P  xtset panel: rho restarts at every panel change.\n"
                             "  --esample-out PATH  write e(sample), one 1/0 line per --data row (see feols);\n"
                             "               Prais-Winsten only (refused with --corc).\n");
        return 2;
    }
    double delta = std::stod(opt(a, "--delta", "1"));
    const std::string pvar = opt(a, "--panelvar");
    const EsampleRequest esreq = esample_request(a, "prais");
    // Cochrane-Orcutt drops every row without an in-sample predecessor, and Stata's
    // e(sample) is that final transformed regression's sample (prais.ado prais_e).
    // prais_fit chooses those rows internally and does not return them, so the mask
    // is refused by name rather than rebuilt outside the fit.
    if (esreq.active() && corc)
        throw EngineError("unsupported",
            "prais: --esample-out is not supported with --corc (the Cochrane-Orcutt "
            "estimation rows are not exposed by the fit)", "--esample-out");
    const std::string src_data = data, keepif = opt(a, "--keepif");
    data = maybe_keepif(data, keepif);
    const Dataset& ds = acquire_data(data);
    TsRegInputs in = load_tsreg(ds, "prais", y, split_csv(xcsv), tvar, pvar);
    require_sample(in.g.n);  // BUG-1269: r(2000) "no observations" / r(2001) for one row
    PraisResult r = prais_fit(in.g.X.data(), in.g.n, in.g.k, in.g.y.data(), in.time.data(),
                              delta, corc, twostep, rhotype, 100, 1e-6, in.panel_or_null());
    if (!r.ok) { std::fprintf(stderr, "prais: estimation failed.\n"); return 1; }
    double dof = static_cast<double>(r.n - r.k);
    double tc = (dof > 0) ? t_critical(0.05, dof) : 0.0;
    json coefs = json::array();
    for (int j = 0; j < r.k; ++j) {
        double se = r.se[j], t = (se > 0) ? r.beta[j] / se : 0.0;
        coefs.push_back({{"name", in.g.names[j]}, {"b", r.beta[j]}, {"se", se}, {"t", t},
                         {"p", (se > 0 && dof > 0) ? t_twosided_pvalue(t, dof) : 1.0},
                         {"ci_lo", r.beta[j] - tc * se}, {"ci_hi", r.beta[j] + tc * se}});
    }
    json out = {{"estimator", "prais"}, {"tranmeth", corc ? "corc" : "prais"},
                {"method", r.method}, {"rhotype", r.rhotype}, {"rho", r.rho},
                {"n_obs", r.n}, {"df_r", dof}, {"iters", r.iters}, {"converged", r.converged},
                {"dw", r.dw}, {"dw_0", r.dw_0}, {"N_gaps", r.n_gaps}, {"coefs", coefs}};
    // Stata's prais prints this note when tsreport counts a gap (BUG-1507; a panel
    // change is one: StataBE 19.5, 40 panels of 5, e(N_gaps) 39).
    if (r.n_gaps > 0) add_warnings(out, {"computations for rho restarted at each gap."});
    // --emit-v (BUG-1196): the final transformed regression's full covariance.
    if (has(a, "--emit-v")) add_e_v(out, in.g.names, r.vcov);
    // e(sample) T1a: Prais-Winsten keeps every listwise row (run starts are scaled by
    // sqrt(1-rho^2), not dropped), so the sample is load_tsreg's keep.
    if (esreq.active()) {
        if (r.n != in.g.n)
            throw EngineError("internal",
                "prais: --esample-out: the fit used " + std::to_string(r.n) + " rows but " +
                std::to_string(in.g.n) + " were loaded; refusing to write a mask");
        EsampleRows rows;
        rows.keep = std::move(in.g.keep);
        rows.loaded = &ds;
        emit_esample(esreq, "prais", src_data, keepif, false, rows, out);
    }
    emit(out, opt(a, "--out"));
    return 0;
}

// tsops — generate Stata time-series operator variables (L./F./D./S.) into a new
// .hbin. Requires --timevar; --panelvar makes operators panel-aware (reset per id).
int cmd_tsops(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), tvar = opt(a, "--timevar"), pvar = opt(a, "--panelvar");
    std::vector<std::string> gens = opt_all(a, "--gen");
    if (data_missing(data) || tvar.empty() || gens.empty()) {
        std::fprintf(stderr,
            "usage: cobra tsops --data f.hbin --timevar T [--panelvar ID] [--delta D] "
            "--gen \"out=L.x\" [--gen ...] [--keepif EXPR] [--out f.json|.hbin]\n"
            "  operators: L F D S with optional multiplicity (L2.x, D2.x, S12.x, LD.x).\n");
        return 2;
    }
    double delta = std::stod(opt(a, "--delta", "1"));
    // E1: compute every --gen against the FULL (unfiltered) series first, THEN apply
    // --keepif — Stata's L./F./D./S. operators are resolved against the whole
    // dataset in memory; `if`/keepif only selects which rows survive afterward. The
    // OLD order (maybe_keepif's physical pre-filter, then ts_apply) let a boundary
    // row's lag/lead lookup miss a neighbor that keepif had already deleted, so it
    // spuriously came back missing. No pre-filter scratch file is needed here at
    // all: we filter the already-open Frame in place, right before writing --out.
    const Dataset& ds = acquire_data(data);
    Frame fr = frame_from_acquired(ds);
    // Time/panel/value columns must be numeric (f64). A FACTOR column has an empty
    // .f64 vector, so .data() is nullptr and ts_apply would deref it -> SIGSEGV.
    // Guard like require_num (feols/iv/csdid) does: fail loudly with an actionable
    // message instead of crashing. Returns nullptr (caller returns 2) on a factor.
    auto numeric_col = [&](const std::string& nm, const char* role) -> const double* {
        if (fr.col(nm).is_factor) {
            std::fprintf(stderr,
                "tsops: %s '%s' is a FACTOR but must be numeric (f64); time/panel/value "
                "columns must stay numeric — re-run to_hbin WITHOUT --factor on '%s'.\n",
                role, nm.c_str(), nm.c_str());
            return nullptr;
        }
        return fr.col(nm).f64.data();
    };
    if (!fr.has(tvar)) { std::fprintf(stderr, "tsops: timevar '%s' not found.\n", tvar.c_str()); return 2; }
    const double* time = numeric_col(tvar, "timevar");
    if (!time) return 2;
    const double* panel = nullptr;
    if (!pvar.empty()) {
        if (!fr.has(pvar)) { std::fprintf(stderr, "tsops: panelvar '%s' not found.\n", pvar.c_str()); return 2; }
        panel = numeric_col(pvar, "panelvar");
        if (!panel) return 2;
    }
    int64_t n = fr.n_rows();
    for (const std::string& g : gens) {
        auto eq = g.find('=');
        if (eq == std::string::npos) { std::fprintf(stderr, "tsops: bad --gen '%s' (need out=OP.var)\n", g.c_str()); return 2; }
        std::string outname = g.substr(0, eq), rhs = g.substr(eq + 1);
        TsOpSpec spec = parse_tsop(rhs);
        if (!spec.ok) { std::fprintf(stderr, "tsops: %s in '%s'\n", spec.err.c_str(), rhs.c_str()); return 2; }
        if (!fr.has(spec.var)) { std::fprintf(stderr, "tsops: variable '%s' not found.\n", spec.var.c_str()); return 2; }
        const double* x = numeric_col(spec.var, "variable");
        if (!x) return 2;
        std::vector<double> outcol;
        std::string err;
        if (!ts_apply(x, n, time, panel, delta, spec, outcol, err)) {
            std::fprintf(stderr, "tsops: %s\n", err.c_str()); return 1;
        }
        fr.set_f64_column(outname, std::move(outcol));
    }
    std::string keepif = opt(a, "--keepif");
    if (!keepif.empty()) fr.keep_if(keepif);  // AFTER materialization (E1) — see comment above
    std::string out = opt(a, "--out");
    if (out_missing(out)) { std::fprintf(stderr, "tsops: --out f.hbin is required.\n"); return 2; }
    int64_t nrows = fr.n_rows();  // capture before the move into commit_frame
    if (!commit_frame(std::move(fr), out))  // §26: stage into resident session, or write+notice one-shot
        std::fprintf(stderr, "wrote %s (%lld rows)\n", out.c_str(), static_cast<long long>(nrows));
    return 0;
}

namespace {

// Parse the case for dfuller/pperron from flags (default = constant). Only used
// by cmd_dfuller/cmd_pperron below (same TU) — no external linkage needed.
int unitroot_case(const std::vector<std::string>& a, bool allow_drift) {
    if (has(a, "--noconstant")) return 1;
    if (has(a, "--trend")) return 4;
    if (allow_drift && has(a, "--drift")) return 3;
    return 2;
}

}  // namespace

// dfuller — Augmented Dickey-Fuller unit-root test.
int cmd_dfuller(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), var = opt(a, "--var"), tvar = opt(a, "--timevar");
    if (data_missing(data) || var.empty() || tvar.empty()) {
        std::fprintf(stderr, "usage: cobra dfuller --data f.hbin --var Y --timevar T [--lags k] "
                             "[--noconstant|--drift|--trend] [--keepif EXPR] [--out f.json]\n");
        return 2;
    }
    int lags = std::stoi(opt(a, "--lags", "0"));
    int kase = unitroot_case(a, true);
    double delta = std::stod(opt(a, "--delta", "1"));
    data = maybe_keepif(data, opt(a, "--keepif"));
    const Dataset& ds = acquire_data(data);
    const double* yr = require_num(ds, "dfuller", var);
    const double* tr = require_num(ds, "dfuller", tvar);
    auto keep = listwise_keep({yr, tr}, ds.n_rows());
    auto y = gather_rows(yr, keep), t = gather_rows(tr, keep);
    DfullerResult r = dfuller_fit(y, t, static_cast<int64_t>(y.size()), lags, kase, delta);
    if (!r.ok) {
        if (r.fail == DfullerResult::SINGULAR)
            std::fprintf(stderr, "dfuller: regression is singular (collinear or a degenerate/constant series).\n");
        else
            std::fprintf(stderr, "dfuller: too few observations.\n");
        return 1;
    }
    const char* cn[] = {"", "noconstant", "constant", "drift", "trend"};
    json out = {{"test", "dfuller"}, {"case", cn[kase]}, {"Zt", r.Zt}, {"lags", r.lags},
                {"n_obs", r.n}, {"cv_1", r.cv1}, {"cv_5", r.cv5}, {"cv_10", r.cv10}};
    if (r.has_p) out["p"] = r.p;
    emit(out, opt(a, "--out"));
    return 0;
}

// pperron — Phillips-Perron unit-root test.
int cmd_pperron(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), var = opt(a, "--var"), tvar = opt(a, "--timevar");
    if (data_missing(data) || var.empty() || tvar.empty()) {
        std::fprintf(stderr, "usage: cobra pperron --data f.hbin --var Y --timevar T [--lags q] "
                             "[--noconstant|--trend] [--keepif EXPR] [--out f.json]\n");
        return 2;
    }
    int lags = std::stoi(opt(a, "--lags", "-1"));  // -1 => Newey-West default bandwidth
    int kase = unitroot_case(a, false);
    double delta = std::stod(opt(a, "--delta", "1"));
    data = maybe_keepif(data, opt(a, "--keepif"));
    const Dataset& ds = acquire_data(data);
    const double* yr = require_num(ds, "pperron", var);
    const double* tr = require_num(ds, "pperron", tvar);
    auto keep = listwise_keep({yr, tr}, ds.n_rows());
    auto y = gather_rows(yr, keep), t = gather_rows(tr, keep);
    PperronResult r = pperron_fit(y, t, static_cast<int64_t>(y.size()), lags, kase, delta);
    if (!r.ok) {
        if (r.fail == PperronResult::SINGULAR)
            std::fprintf(stderr, "pperron: regression is singular (collinear or a degenerate/constant series).\n");
        else
            std::fprintf(stderr, "pperron: too few observations.\n");
        return 1;
    }
    const char* cn[] = {"", "noconstant", "constant", "", "trend"};
    json out = {{"test", "pperron"}, {"case", cn[kase]}, {"Zt", r.Ztau}, {"Zrho", r.Zrho},
                {"newey_west_lags", r.lags}, {"n_obs", r.n},
                {"cv_1", r.cv1}, {"cv_5", r.cv5}, {"cv_10", r.cv10}, {"p", r.p}};
    emit(out, opt(a, "--out"));
    return 0;
}

// tsfill — insert rows for missing time periods within each panel (Stata tsfill).
// --full (Stata `tsfill, full`, BUG-897): every panel over the overall time range.
int cmd_tsfill(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), tvar = opt(a, "--timevar"), pvar = opt(a, "--panelvar"),
                out = opt(a, "--out");
    if (data_missing(data) || tvar.empty() || out_missing(out)) {
        std::fprintf(stderr, "usage: cobra tsfill --data f.hbin --timevar T [--panelvar ID] "
                             "[--delta D] [--full] [--keepif EXPR] --out new.hbin\n");
        return 2;
    }
    data = maybe_keepif(data, opt(a, "--keepif"));
    double delta = std::stod(opt(a, "--delta", "1"));
    const bool full = has(a, "--full");
    const Dataset& ds = acquire_data(data);
    Frame fr = frame_from_acquired(ds);
    Frame res = fr.tsfill(tvar, pvar, delta, full);
    int64_t nrows = res.n_rows();  // capture before the move into commit_frame (fr is NOT moved)
    if (!commit_frame(std::move(res), out))  // §26: stage into resident session, or write+notice one-shot
        std::fprintf(stderr, "wrote %s (%lld rows, was %lld)\n", out.c_str(),
                     static_cast<long long>(nrows), static_cast<long long>(fr.n_rows()));
    return 0;
}

// wntestq — Ljung-Box portmanteau white-noise test on a single series.
int cmd_wntestq(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), var = opt(a, "--var");
    if (data_missing(data) || var.empty()) {
        std::fprintf(stderr, "usage: cobra wntestq --data f.hbin --var V [--lags P] [--keepif EXPR] [--out f.json]\n"
                             "  Ljung-Box Q white-noise test (rows in time order; interior missings are gaps).\n");
        return 2;
    }
    data = maybe_keepif(data, opt(a, "--keepif"));
    const Dataset& ds = acquire_data(data);
    const double* raw = require_num(ds, "wntestq", var);
    const int64_t nrows = ds.n_rows();
    // Keep the series in time order (do NOT compact) so interior missings act as
    // declared tsset gaps — wntestq() forms lag pairs by time distance and skips
    // any pair touching a gap, matching Stata (a spliced series over-counts lags).
    int64_t nnm = 0;
    for (int64_t i = 0; i < nrows; ++i) if (!std::isnan(raw[i])) ++nnm;
    if (nnm == 0) { std::fprintf(stderr, "wntestq: no rows after dropping missing.\n"); return 1; }
    int lags = std::stoi(opt(a, "--lags", "0"));  // 0 => default min(n/2-2, 40)
    WnResult r = wntestq(raw, nrows, lags);
    if (!r.ok) {
        if (r.fail == WnResult::ZERO_VAR)
            std::fprintf(stderr, "wntestq: series has zero variance (constant) — white-noise test undefined.\n");
        else
            std::fprintf(stderr, "wntestq: too few observations for the requested lags.\n");
        return 1;
    }
    json out = {{"test", "wntestq"}, {"var", var}, {"n_obs", nnm},
                {"lags", r.df}, {"Q", r.stat}, {"df", r.df}, {"p", r.p}};
    emit(out, opt(a, "--out"));
    return 0;
}

}  // namespace cli
}  // namespace cecon
