// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "common.h"
#include "commands.h"

#include <limits>
#include <stdexcept>

#include "core/collinear.h"
#include "estimators/logit.h"
#include "estimators/probit.h"
#include "estimators/tobit.h"
#include "estimators/heckman.h"
#include "estimators/qreg.h"
#include "vcov/vcov.h"  // hc_meat/cluster_meat — P5 pweight logit sandwich reuses these low-level helpers

#include "vcov/svy_vcov.h"

namespace cecon {
namespace cli {

// qreg: quantile regression (Koenker-Bassett). Frisch-Newton IP + iid (fitted-
// sparsity, Hall-Sheather) SEs; t(df_r) inference. Matches Stata `qreg`.
// --vce robust (+ optional --weight W --wtype pw): Stata's `qreg ..., vce(robust)`
// and `qreg ... [pw=w], vce(robust)` through qreg_fit_robust (the Hendricks-
// Koenker sandwich; oracle w2b_estim.log:818 / :791, formula in qreg.cpp).
int cmd_qreg(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), y = opt(a, "--y"), xcsv = opt(a, "--x");
    if (data_missing(data) || y.empty() || xcsv.empty()) {
        std::fprintf(stderr, "usage: cobra qreg --data f.hbin --y Y --x X1,X2 [--tau 0.5] [--level 95] "
                             "[--vce iid|robust] [--weight W --wtype pw] [--keepif EXPR] [--out f.json]\n"
                             "  --weight W --wtype pw    Stata pweight; requires --vce robust.\n");
        return 2;
    }
    const std::string vce = opt(a, "--vce", "iid");
    if (vce != "iid" && vce != "robust") {
        std::fprintf(stderr, "qreg: --vce must be one of iid|robust (got %s).\n", vce.c_str());
        return 2;
    }
    // pweight: Stata's qreg takes fweight/iweight/pweight ([R] qreg); only the
    // pweight + vce(robust) pair is oracle-pinned (w2b_estim.log:791). A pweight
    // without vce(robust) has no captured record of which VCE Stata computes, so
    // it is refused rather than answered with a guessed one.
    const std::string weight_var = opt(a, "--weight"), wtype = opt(a, "--wtype");
    cecon::WeightKind wkind = cecon::WeightKind::None;
    if (!weight_var.empty() || !wtype.empty()) {
        if (weight_var.empty())
            throw EngineError("bad_input", "qreg: --wtype requires --weight");
        if (wtype != "pw" && wtype != "pweight")
            throw EngineError("unsupported", "qreg: --wtype must be pw (only pweight is supported)");
        if (vce != "robust")
            throw EngineError("unsupported",
                "qreg: [pweight=] is supported only with vce(robust) in this engine");
        wkind = cecon::WeightKind::Probability;
    }
    data = maybe_keepif(data, opt(a, "--keepif"));
    double tau = std::stod(opt(a, "--tau", "0.5"));
    double level = std::stod(opt(a, "--level", "95"));
    const Dataset& ds = acquire_data(data);
    GlmInputs g = load_glm(ds, "qreg", y, split_csv(xcsv), {}, weight_var, wkind);
    if (g.n == 0) { std::fprintf(stderr, "qreg: no rows after dropping missing.\n"); return 1; }
    const double* wptr = g.weights.empty() ? nullptr : g.weights.data();
    QregResult r = (vce == "robust")
        ? qreg_fit_robust(g.X.data(), g.n, g.k, g.y.data(), wptr, tau, g.names, level)
        : qreg_fit(g.X.data(), g.n, g.k, g.y.data(), tau, g.names, level);
    if (!r.converged) std::fprintf(stderr, "qreg: warning — interior-point did not fully converge.\n");
    const double dof = r.dof_resid;
    // BUG-281: the CI multiplier must follow --level. `level` already reached
    // qreg_fit (the Hall-Sheather bandwidth, hence the SE, is level-dependent —
    // matching Stata), but this multiplier was hardcoded at the 95% alpha, so
    // `--level 90` printed a 90%-bandwidth se under a 95% t. Measured vs StataBE
    // 19.5 (auto, qreg price mpg weight, level(90)): Stata ll(mpg)
    // -2.49302234902794e+02, ours was -285.681 (t at .025 instead of .05).
    // Same alpha dance as tobit: exact 0.05 when level==95, else 1-level/100.
    const double tc = (dof > 0) ? t_critical(level == 95.0 ? 0.05 : 1.0 - level / 100.0, dof) : 0.0;
    json coefs = json::array();
    for (int j = 0; j < g.k; ++j) {
        double se = r.se[j], t = (se > 0) ? r.beta[j] / se : 0.0;
        coefs.push_back({{"name", g.names[j]}, {"b", r.beta[j]}, {"se", se}, {"t", t},
                         {"p", (se > 0 && dof > 0) ? t_twosided_pvalue(t, dof) : 1.0},
                         {"ci_lo", r.beta[j] - tc * se}, {"ci_hi", r.beta[j] + tc * se}});
    }
    json out = {{"estimator", "qreg"}, {"tau", tau}, {"n_obs", r.n}, {"coefs", coefs},
                {"sparsity", r.sparsity}, {"bwidth", r.bwidth}, {"sum_adev", r.sum_adev},
                {"df_r", dof}, {"converged", r.converged}};
    if (vce == "robust") {
        // qreg_fit_robust computes no sparsity (NaN, qreg.h), so the field is
        // absent rather than a null; the default iid output is unchanged.
        out.erase("sparsity");
        out["vce"] = "robust";
        if (r.n_crossing > 0) out["n_crossing"] = r.n_crossing;
    }
    if (wkind != cecon::WeightKind::None) { out["weight_var"] = weight_var; out["wtype"] = "pw"; }
    emit(out, opt(a, "--out"));
    return 0;
}

// heckman — sample-selection model, two-step (Heckit). --select "S=Z1,Z2".
int cmd_heckman(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), y = opt(a, "--y"), xcsv = opt(a, "--x"), selspec = opt(a, "--select");
    if (data_missing(data) || y.empty() || xcsv.empty() || selspec.empty()) {
        std::fprintf(stderr, "usage: cobra heckman --data f.hbin --y Y --x X1,X2 --select \"S=Z1,Z2\" "
                             "[--twostep] [--level P] [--keepif EXPR] [--out f.json]\n"
                             "  two-step Heckit (probit selection + inverse-Mills correction).\n"
                             "  --level P     ci_lo/ci_hi on BOTH equations (asymptotic z, matching\n"
                             "                Stata's own `heckman` table) at P%; default 95, matching\n"
                             "                Stata's default table (BUG-202, BUG-279).\n");
        return 2;
    }
    // --level P (BUG-202): heckman (even --twostep here) reports z/P>|z| in Stata,
    // not t — same asymptotic-normal convention as logit/probit/ivtobit, applied to
    // BOTH the outcome equation (coefs) and the selection equation (select_coefs).
    // Only `coefs` feeds the shared `--format tidy` coeftable (tidy_coeftable only
    // looks at the top-level "coefs"/"coef" keys) — select_coefs' ci_lo/ci_hi is
    // still emitted for a caller reading the native JSON shape directly.
    // BUG-279: default level is 95, like Stata's — the plain invocation emits the
    // 95% interval unprompted rather than a null column.
    bool level_set = has(a, "--level");
    double level = std::stod(opt(a, "--level", "95"));
    if (level_set && (level < 10.0 || level > 99.99)) {
        std::fprintf(stderr, "heckman: --level must be in [10, 99.99] (got %s).\n", opt(a, "--level").c_str());
        return 2;
    }
    const double zc = z_critical(level == 95.0 ? 0.05 : 1.0 - level / 100.0);
    auto eq = selspec.find('=');
    if (eq == std::string::npos) { std::fprintf(stderr, "heckman: --select must be \"S=Z1,Z2\".\n"); return 2; }
    std::string selvar = selspec.substr(0, eq);
    auto zvars = split_csv(selspec.substr(eq + 1));
    auto xvars = split_csv(xcsv);
    data = maybe_keepif(data, opt(a, "--keepif"));
    const Dataset& ds = acquire_data(data);
    int64_t n_raw = ds.n_rows();
    const double* Sr = require_num(ds, "heckman", selvar);
    const double* yr = require_num(ds, "heckman", y);
    int kz = static_cast<int>(zvars.size()) + 1, kx = static_cast<int>(xvars.size()) + 1;
    std::vector<const double*> zr, xr, must = {Sr};
    for (auto& s : zvars) { zr.push_back(require_num(ds, "heckman", s)); must.push_back(zr.back()); }
    for (auto& s : xvars) { xr.push_back(require_num(ds, "heckman", s)); must.push_back(xr.back()); }
    auto keep = listwise_keep(must, n_raw);  // y allowed missing (non-selected)
    int64_t n = static_cast<int64_t>(keep.size());
    require_sample(n);  // BUG-1269: r(2000) "no observations" / r(2001) for one row
    auto gv = [&](const double* p) { return gather_rows(p, keep); };
    std::vector<double> S = gv(Sr), yv = gv(yr);
    std::vector<double> Z(static_cast<size_t>(n) * kz), X(static_cast<size_t>(n) * kx);
    for (int c = 0; c + 1 < kz; ++c) { auto col = gv(zr[c]); std::copy(col.begin(), col.end(), Z.data() + static_cast<int64_t>(c) * n); }
    for (int64_t i = 0; i < n; ++i) Z[static_cast<int64_t>(kz - 1) * n + i] = 1.0;
    for (int c = 0; c + 1 < kx; ++c) { auto col = gv(xr[c]); std::copy(col.begin(), col.end(), X.data() + static_cast<int64_t>(c) * n); }
    for (int64_t i = 0; i < n; ++i) X[static_cast<int64_t>(kx - 1) * n + i] = 1.0;
    std::vector<char> sel(n);
    for (int64_t i = 0; i < n; ++i) sel[i] = (S[i] == 1.0 && !std::isnan(yv[i])) ? 1 : 0;

    HeckmanResult r = heckman_twostep(Z.data(), n, kz, S.data(), X.data(), kx, yv.data(), sel);
    if (!r.ok) { std::fprintf(stderr, "heckman: estimation failed.\n"); return 1; }
    std::vector<std::string> onames = xvars; onames.push_back("(intercept)");
    std::vector<std::string> snames = zvars; snames.push_back("(intercept)");
    json oc = json::array();
    for (int j = 0; j < kx; ++j) {
        json row = {{"name", onames[j]}, {"b", r.beta[j]}, {"se", r.se[j]},
                    {"t", r.se[j] > 0 ? r.beta[j] / r.se[j] : 0.0}};
        if (r.se[j] > 0) {
            row["ci_lo"] = r.beta[j] - zc * r.se[j];
            row["ci_hi"] = r.beta[j] + zc * r.se[j];
        }
        oc.push_back(std::move(row));
    }
    json sc = json::array();
    for (int j = 0; j < kz; ++j) {
        json row = {{"name", snames[j]}, {"b", r.sel_beta[j]}, {"se", r.sel_se[j]},
                    {"t", r.sel_se[j] > 0 ? r.sel_beta[j] / r.sel_se[j] : 0.0}};
        if (r.sel_se[j] > 0) {
            row["ci_lo"] = r.sel_beta[j] - zc * r.sel_se[j];
            row["ci_hi"] = r.sel_beta[j] + zc * r.sel_se[j];
        }
        sc.push_back(std::move(row));
    }
    json out = {{"estimator", "heckman"}, {"method", "twostep"}, {"n_obs", r.n},
                {"n_selected", r.n_selected}, {"coefs", oc}, {"select_coefs", sc},
                {"lambda", r.lambda}, {"se_lambda", r.se_lambda},
                {"rho", r.rho}, {"sigma", r.sigma}, {"converged", r.converged}};
    out["level"] = level;  // always emitted, like Stata's e(level) (BUG-279)
    emit(out, opt(a, "--out"));
    return 0;
}

// tobit — censored-normal regression MLE (Stata `tobit, ll() ul()`).
int cmd_tobit(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), y = opt(a, "--y"), xcsv = opt(a, "--x");
    std::string lls = opt(a, "--ll"), uls = opt(a, "--ul");
    // --vce (P5): default "iid" so an omitted flag reproduces the exact prior
    // behavior/output shape (opt() with a default never marks the flag consumed
    // when absent) — see below, every new branch is gated on vce=="robust".
    std::string vce = opt(a, "--vce", "iid");
    const std::string weight_var = opt(a, "--weight"), wtype = opt(a, "--wtype");
    if (data_missing(data) || y.empty() || xcsv.empty() || (lls.empty() && uls.empty())) {
        std::fprintf(stderr, "usage: cobra tobit --data f.hbin --y Y --x X1,X2 [--ll L] [--ul U] "
                             "[--vce iid|robust] [--weight W --wtype aw|fw|iw|pw] [--level P] "
                             "[--keepif EXPR] [--out f.json]\n"
                             "  censored-normal MLE; at least one of --ll/--ul required.\n"
                             "  --vce robust  Stata `tobit ..., vce(robust)` sandwich VCE (P5).\n"
                             "  --level P     coeftable ci_lo/ci_hi at P% (t(df_r), matching Stata's\n"
                             "                own tobit table); default 95, matching Stata's default\n"
                             "                table (BUG-202, BUG-279).\n");
        return 2;
    }
    if (vce != "iid" && vce != "robust") {
        std::fprintf(stderr, "tobit: --vce must be one of iid|robust (got %s).\n", vce.c_str());
        return 2;
    }
    // --level P (BUG-202): tobit had no --level at all, so ci_lo/ci_hi was
    // unconditionally absent. Stata's own `tobit` table reports t/P>|t| (not z),
    // matching this command's EXISTING p-value dispatch two lines below
    // (t_twosided_pvalue(t, dof)) — so the CI multiplier is t_critical(., dof) for
    // the SAME dof, not the z used by the pure-ML logit/probit family.
    // BUG-279: the level DEFAULT is 95, like Stata's — the plain invocation emits
    // the 95% interval unprompted, so `cobra tobit` alone matches Stata's default
    // table for every caller (not just a PIET that knows to inject level(95)).
    bool level_set = has(a, "--level");
    double level = std::stod(opt(a, "--level", "95"));
    if (level_set && (level < 10.0 || level > 99.99)) {
        std::fprintf(stderr, "tobit: --level must be in [10, 99.99] (got %s).\n", opt(a, "--level").c_str());
        return 2;
    }
    // Stata's tobit weights (BUG-1544; tobit.ado fits gsem): an fweight, iweight or
    // pweight weights the log-likelihood (e(N) = Sum w under an fweight, and a
    // pweight forces vce(robust)); an aweight is a precision weight normalized to sum
    // to N (tobit.h ll_grad_w) (StataBE 19.5, scratch-laneA/agents/file-new-2/a5.log:
    // `tobit yc x z [aw=wf], ll(1.2)` e(N) 60, _se[x] .1657775048900485; [fw=wi] e(N)
    // 150, .10898132248596666; [iw=wf] .11279087487734443; [pw=wf] .19377991916672471).
    cecon::WeightKind wkind = cecon::WeightKind::None;
    if (!weight_var.empty() || !wtype.empty()) {
        if (weight_var.empty()) throw EngineError("bad_input", "tobit: --wtype requires --weight");
        if (wtype == "aw" || wtype == "aweight") wkind = cecon::WeightKind::Analytic;
        else if (wtype == "fw" || wtype == "fweight") wkind = cecon::WeightKind::Frequency;
        else if (wtype == "iw" || wtype == "iweight") wkind = cecon::WeightKind::Importance;
        else if (wtype == "pw" || wtype == "pweight") wkind = cecon::WeightKind::Probability;
        else throw EngineError("bad_input", "tobit: --wtype must be aw, fw, iw or pw");
        if (wkind == cecon::WeightKind::Probability && vce == "iid")
            throw EngineError("bad_input",
                "tobit: [pweight=] requires vce(robust); Stata has no iid VCE under pweight");
    }
    const bool fw = wkind == cecon::WeightKind::Frequency;
    bool has_ll = !lls.empty(), has_ul = !uls.empty();
    double ll_lim = has_ll ? std::stod(lls) : 0.0, ul_lim = has_ul ? std::stod(uls) : 0.0;
    data = maybe_keepif(data, opt(a, "--keepif"));
    const Dataset& ds = acquire_data(data);
    GlmInputs g = load_glm(ds, "tobit", y, split_csv(xcsv), {}, weight_var, wkind);
    require_sample(g.n);  // BUG-1269: r(2000) "no observations" / r(2001) for one row
    // lw: the likelihood weights; sc: an aweight's scale sqrt(w*N/Sum w).
    double sum_w = 0;
    for (double w : g.weights) sum_w += w;
    std::vector<double> lw, sc;
    if (wkind == cecon::WeightKind::Analytic) {
        sc.resize(g.weights.size());
        for (size_t i = 0; i < sc.size(); ++i)
            sc[i] = std::sqrt(g.weights[i] * static_cast<double>(g.n) / sum_w);
    } else if (wkind != cecon::WeightKind::None) {
        lw = g.weights;
    }
    TobitResult r = tobit_fit(g.X.data(), g.n, g.k, g.y.data(), has_ll, ll_lim, has_ul, ul_lim,
                              lw.empty() ? nullptr : lw.data(), sc.empty() ? nullptr : sc.data());
    if (!r.ok) { std::fprintf(stderr, "tobit: MLE failed.\n"); return 1; }
    // BUG-280: Stata's tobit residual df is e(N) - e(df_m) — it subtracts ONLY the
    // slope regressors, never the intercept and never sigma. Measured against
    // StataBE 19.5 on seven specs (fatalities inc_cens with ul(): 1/2/4 slopes ->
    // df_r 335/334/332; 2 slopes noconstant -> 334, same as WITH _cons; 2 slopes
    // two-limit ll()+ul() -> 334; auto price with ll(3000): 1/2 slopes -> 73/72).
    // load_glm always appends the intercept, so slopes = g.k - 1. The old
    // `r.n - g.k - 1` subtracted intercept+sigma and landed on invttail(332,.)
    // where Stata is invttail(334,.) — wrong in the last digits of every tobit
    // CI AND p-value (this dof feeds t_twosided_pvalue below).
    double dof = static_cast<double>(r.n - (g.k - 1));
    if (fw) dof = sum_w - static_cast<double>(g.k - 1);  // e(N) is Sum w
    // vce(robust): only computed when explicitly requested — the DEFAULT (no
    // --vce) path below keeps reading r.se/r.se_sigma/r.se_variance exactly as
    // before, byte-for-byte, so existing pins (freeze corpus `tobit y x1 x2,
    // ll(0)`, tests/unit/test_tobit.cpp) are untouched.
    std::vector<double> Vrobust;
    const int np = g.k + 1;
    if (vce == "robust") {
        // ML vce(robust) small-sample multiplier: N/(N-1) — the SAME factor Stata
        // applies to logit/probit's plain vce(robust) (see cmd_logit above).
        // Verified exact vs the P5 oracle `tobit y x1 x2 i.region, ll(0)
        // vce(robust)` pin (coef/se match to <1e-6 relative).
        double factor = r.n > 1 ? static_cast<double>(r.n) / static_cast<double>(r.n - 1) : 1.0;
        // Weighted (BUG-1544): the rows' scores times w under an iweight or pweight,
        // times sqrt(w) under an fweight with N = Sum w, the aweight scale under an
        // aweight (StataBE 19.5, scratch-laneA2/agents/wml/x1.log: vce(robust) _se[x]
        // [aw=wf] .17974127515089006, [fw=wi] .11214716360456961, [iw=wf]
        // .19377991916672471).
        std::vector<double> mw;
        if (fw) {
            factor = sum_w > 1 ? sum_w / (sum_w - 1.0) : 1.0;
            mw.resize(g.weights.size());
            for (size_t i = 0; i < mw.size(); ++i) mw[i] = std::sqrt(g.weights[i]);
        } else if (!lw.empty()) {
            mw = lw;
        }
        Vrobust = tobit_robust_vcov(g.X.data(), g.n, g.k, g.y.data(), has_ll, ll_lim, has_ul, ul_lim,
                                    r, factor, mw.empty() ? nullptr : mw.data(),
                                    sc.empty() ? nullptr : sc.data());
        if (Vrobust.empty()) {
            std::fprintf(stderr, "tobit: vce(robust) failed (non-invertible sandwich).\n");
            return 1;
        }
    }
    const double tc = (dof > 0) ? t_critical(level == 95.0 ? 0.05 : 1.0 - level / 100.0, dof) : 0.0;
    json coefs = json::array();
    for (int j = 0; j < g.k; ++j) {
        double se = (vce == "robust") ? std::sqrt(std::max(0.0, Vrobust[static_cast<size_t>(j) * np + j]))
                                      : r.se[j];
        double t = (se > 0) ? r.beta[j] / se : 0.0;
        json row = {{"name", g.names[j]}, {"b", r.beta[j]}, {"se", se}, {"t", t},
                    {"p", (se > 0 && dof > 0) ? t_twosided_pvalue(t, dof) : 1.0}};
        if (se > 0 && dof > 0) {
            row["ci_lo"] = r.beta[j] - tc * se;
            row["ci_hi"] = r.beta[j] + tc * se;
        }
        coefs.push_back(std::move(row));
    }
    double se_sigma = r.se_sigma, se_variance = r.se_variance;
    if (vce == "robust") {
        double se_lnsig = std::sqrt(std::max(0.0, Vrobust[static_cast<size_t>(g.k) * np + g.k]));
        se_sigma = r.sigma * se_lnsig;         // same delta-method shape as tobit_fit's se_sigma
        se_variance = 2.0 * r.sigma * se_sigma;
    }
    // Under an fweight e(N) and the censoring counts are the observations the rows
    // stand for (a5.log `[fw=wi]`: N 150, Uncensored 107, Left-censored 43).
    int64_t n_out = r.n, n_unc = r.n_unc, n_lc = r.n_lc, n_rc = r.n_rc;
    if (fw) {
        n_out = static_cast<int64_t>(sum_w);
        n_unc = n_lc = n_rc = 0;
        for (int64_t i = 0; i < g.n; ++i) {
            const double yi = g.y[static_cast<size_t>(i)];
            const int64_t wi = static_cast<int64_t>(g.weights[static_cast<size_t>(i)]);
            if (has_ll && yi <= ll_lim) n_lc += wi;
            else if (has_ul && yi >= ul_lim) n_rc += wi;
            else n_unc += wi;
        }
    }
    json out = {{"estimator", "tobit"}, {"n_obs", n_out}, {"n_uncensored", n_unc},
                {"n_left_censored", n_lc}, {"n_right_censored", n_rc},
                {"sigma", r.sigma}, {"se_sigma", se_sigma},
                {"variance", r.variance}, {"se_variance", se_variance},
                {"ll", r.ll}, {"iters", r.iters}, {"converged", r.converged}, {"coefs", coefs}};
    if (has_ll) out["ll_limit"] = ll_lim;
    if (has_ul) out["ul_limit"] = ul_lim;
    if (has(a, "--vce")) out["vce"] = vce;  // absent-key on the default path (byte-stable)
    out["level"] = level;  // always emitted, like Stata's e(level) (BUG-279)
    // --emit-v (BUG-1196): Stata's tobit e(V) (gsem underneath) has the residual
    // VARIANCE as its last parameter, `/var(e.y)` (StataBE 19.5: colfullnames
    // e(b) `yc:x1 yc:x2 yc:_cons /var(e.yc)`). The fit's V is in (β, lnσ), so
    // the var row/column is the delta method with d var / d lnσ = 2σ²: V[j,var]
    // = 2σ² V[j,lnσ], V[var,var] = (2σ²)² V[lnσ,lnσ] (the se_variance above is
    // the square root of that cell).
    if (has(a, "--emit-v")) {
        const std::vector<double>& Vf = (vce == "robust") ? Vrobust : r.vcov;
        std::vector<double> Vs(Vf);
        const double d = 2.0 * r.variance;
        for (int i = 0; i < np; ++i) {
            Vs[static_cast<size_t>(i) * np + g.k] *= d;
            Vs[static_cast<size_t>(g.k) * np + i] *= d;
        }
        std::vector<std::string> names = g.names;
        names.push_back("/var(e." + y + ")");
        add_e_v(out, names, Vs);
    }
    emit(out, opt(a, "--out"));
    return 0;
}

// Stata's perfect-prediction screen on a loaded logit/probit design (defined
// after the anonymous namespace below). Shared with cmds_postest.cpp's refits,
// which declare it there: a refit after a fit that dropped a perfect predictor
// must fit the same rows and columns (FU-541). `bases` (binary_base_levels) are
// the factor base levels it tests beside the design columns.
bool binary_screen_glm(GlmInputs& g, bool asis, bool has_constant, const char* cmd,
                       const std::string& yname, BinaryPerfectScreen& screen, std::vector<int>& fit_cols,
                       bool notes, const std::vector<BinaryBaseLevel>* bases);

// The base level of each plain factor main effect of the varlist `xs` on the rows
// of the loaded design g (logit.h BinaryBaseLevel; defined below, shared with
// cmds_postest.cpp's refits like binary_screen_glm).
std::vector<BinaryBaseLevel> binary_base_levels(const Dataset& ds, const std::vector<std::string>& xs,
                                                const GlmInputs& g);

// FU-138 maximize options (defined after the anonymous namespace below). Shared
// with cmds_postest.cpp's refits, which declare it there (FU-426).
bool parse_ml_max_options(const std::vector<std::string>& a, const char* cmd, MlMaxOptions& mo);

// Stata `logit/probit ..., noconstant` (--no-intercept). load_glm's public
// contract puts the intercept in the LAST design column (common.cpp), so with
// X column-major the constant is dropped by truncating X to its first k-1
// columns and popping the trailing "(intercept)" name. External linkage: the
// postestimation/margins refit in cmds_postest.cpp reuses it (FU-131).
void drop_glm_intercept(GlmInputs& g, const char* cmd) {
    if (g.k < 2)
        throw EngineError("bad_input", std::string(cmd) +
                          ": noconstant with no regressors leaves an empty model");
    g.k -= 1;
    g.X.resize(static_cast<size_t>(g.n) * static_cast<size_t>(g.k));
    g.names.pop_back();
}

namespace {

struct MlModelTest {
    int df = 0;
    int rank = 0;
    double chi2 = std::numeric_limits<double>::quiet_NaN();
};

// Sweep through the symmetric k x k matrix V visiting indices in `order`: an
// index is kept when its original diagonal is positive and its remaining pivot
// exceeds 1e-12 of that diagonal; each kept index is swept out of the indices
// visited after it. kept[a] refers to order[a].
std::vector<char> sweep_kept(const std::vector<double>& V, int k, const std::vector<int>& order) {
    const size_t K = static_cast<size_t>(std::max(k, 0));
    std::vector<double> M(V.begin(), V.begin() + static_cast<std::ptrdiff_t>(K * K));
    std::vector<char> kept(order.size(), 0);
    for (size_t a = 0; a < order.size(); ++a) {
        const size_t p = static_cast<size_t>(order[a]);
        const double d0 = V[p * K + p];
        const double pivot = M[p * K + p];
        if (!(d0 > 0.0) || !(pivot > 1e-12 * d0)) continue;
        kept[a] = 1;
        for (size_t b = a + 1; b < order.size(); ++b) {
            const size_t i = static_cast<size_t>(order[b]);
            const double mip = M[i * K + p];
            for (size_t c = a + 1; c < order.size(); ++c) {
                const size_t j = static_cast<size_t>(order[c]);
                M[i * K + j] -= mip * M[p * K + j] / pivot;
            }
        }
    }
    return kept;
}

int count_kept(const std::vector<char>& kept) {
    int r = 0;
    for (char c : kept) r += c ? 1 : 0;
    return r;
}

// The model test after a Wald-type fit: `test` on every slope with the
// `minimum` rule (help test): the constant is visited first, and a slope that
// drops out leaves the test missing. The rank counts the columns of V left after
// those its generalized inverse sets to zero.
// Slopes are columns 0..k-2 with the constant last when has_constant, else all k.
MlModelTest ml_model_test(const std::vector<double>& V, int k, const std::vector<double>& beta, bool has_constant) {
    MlModelTest t;
    if (k <= 0) return t;
    std::vector<int> natural;
    for (int j = 0; j < k; ++j) natural.push_back(j);
    t.rank = count_kept(sweep_kept(V, k, natural));
    const int q = has_constant ? k - 1 : k;
    std::vector<int> order;
    if (has_constant) order.push_back(k - 1);
    for (int j = 0; j < q; ++j) order.push_back(j);
    const std::vector<char> kept = sweep_kept(V, k, order);
    for (size_t a = has_constant ? 1 : 0; a < order.size(); ++a) t.df += kept[a] ? 1 : 0;
    if (t.df == 0 || t.df < q) return t;
    const size_t Q = static_cast<size_t>(q), K = static_cast<size_t>(k);
    std::vector<double> Vs(Q * Q);
    for (size_t a = 0; a < Q; ++a)
        for (size_t b = 0; b < Q; ++b) Vs[a * Q + b] = V[a * K + b];
    if (la::spd_inverse(Vs.data(), q) != 0) return t;
    double chi2 = 0.0;
    for (size_t a = 0; a < Q; ++a)
        for (size_t b = 0; b < Q; ++b) chi2 += beta[a] * Vs[a * Q + b] * beta[b];
    t.chi2 = chi2;
    return t;
}

// Header statistics of a no-constant binary model. There is no constant-only
// model nested in it, so Stata prints neither the LR test nor a pseudo-R2: it
// prints a Wald chi2 on ALL k coefficients (w2b_estim.log:692 `probit d x1 x2,
// r nocons` Wald chi2(2) = 19.17; :719 `probit d x1 x2, noconstant` Wald
// chi2(2) = 18.97). `w` is the pweight (nullptr unweighted): the reported log
// (pseudo)likelihood is then the weighted one.
MlModelTest add_mle_fit_nocons(json& out, const std::vector<double>& p, const std::vector<double>& y,
                               const double* w, int64_t n, const std::vector<double>& beta,
                               const std::vector<double>& V, int k) {
    const double eps = 1e-12;
    double ll = 0.0;
    for (int64_t i = 0; i < n; ++i) {
        double pi = std::min(1.0 - eps, std::max(eps, p[static_cast<size_t>(i)]));
        double yi = y[static_cast<size_t>(i)];
        double wi = w ? w[static_cast<size_t>(i)] : 1.0;
        ll += wi * (yi * std::log(pi) + (1.0 - yi) * std::log(1.0 - pi));
    }
    out["ll"] = ll;
    out["ll_0"] = nullptr;
    out["r2_pseudo"] = nullptr;
    const MlModelTest t = ml_model_test(V, k, beta, false);
    out["df_chi2"] = t.df;
    out["chi2"] = std::isfinite(t.chi2) ? json(t.chi2) : json(nullptr);
    out["p_chi2"] = std::isfinite(t.chi2) ? json(chi2_pvalue(t.chi2, t.df)) : json(nullptr);
    return t;
}

// FU-426: the maximize options the fit was given, echoed as the flags themselves
// (`ml_max_argv`, value strings verbatim so the refit parses the same doubles).
// PIET forwards them onto every post-estimation refit of this fit
// (estimation_m4c.cpp pushFitAsisM4), which then stops at the iterate this
// reply posts instead of at the default one. Absent when no flag was given, so
// the default reply is byte-identical. --difficult is not echoed: it is a no-op
// on these concave objectives (logit.h), so the refit needs nothing from it and
// the difficult fit's reply stays the plain one's (w2e_probes.log:304 == :249,
// test_mle_parity's lp_tight_difficult arm).
void echo_ml_max_options(json& out, const std::vector<std::string>& a) {
    json flags = json::array();
    for (const char* f : {"--tolerance", "--ltolerance", "--nrtolerance", "--iterate"}) {
        if (!has(a, f)) continue;
        flags.push_back(f);
        flags.push_back(opt(a, f));
    }
    if (!flags.empty()) out["ml_max_argv"] = std::move(flags);
}

// Post the ml objective as e(ll) and keep the header statistics that derive
// from it consistent: the objective drops completely determined rows (logit.h
// rule 4), so add_mle_fit's recomputation from the clamped probabilities is not
// Stata's e(ll) (w2e_probes.log:141 lp_default e(ll) -1.77239f16196faX+001).
// `lr`: the header test is the LR chi2 (add_mle_fit), else it is a Wald test
// and only ll / r2_pseudo move. Also emits e(ic) as `ic` and the iteration log
// as `ilog` (ll at iterations 0..ic), beside the legacy `iters`.
void post_ml_fit(json& out, double ll, bool lr, int ic, const std::vector<double>& ilog) {
    out["ic"] = ic;
    out["ilog"] = ilog;
    out["ll"] = ll;
    if (!out.contains("ll_0") || out["ll_0"].is_null()) return;
    const double ll0 = out["ll_0"].get<double>();
    out["r2_pseudo"] = (ll0 != 0.0) ? 1.0 - ll / ll0 : 0.0;
    if (!lr) return;
    const double chi2 = 2.0 * (ll - ll0);
    out["chi2"] = chi2;
    const int df = out.value("df_chi2", 0);
    out["p_chi2"] = (df > 0) ? json(chi2_pvalue(chi2, static_cast<double>(df))) : json(nullptr);
}

// ---- shared by cmd_logit and cmd_probit (FU-462/FU-467/FU-463/FU-464/FU-471) ----

// The design before Stata's perfect-prediction screen, and what the screen did.
struct BinaryScreenInfo {
    std::vector<std::string> full_names;   // design names before any column was dropped
    std::vector<int> fit_cols;             // full index of each column left in g
    BinaryPerfectScreen screen;
};

// Remove the rows with keep[i] == 0 from every per-row member of g.
void compact_glm_rows(GlmInputs& g, const std::vector<char>& keep) {
    const size_t n = static_cast<size_t>(g.n);
    size_t m = 0;
    for (size_t i = 0; i < n; ++i) m += keep[i] ? 1 : 0;
    if (m == n) return;
    auto pick = [&](std::vector<double>& v) {
        if (v.size() != n) return;
        size_t o = 0;
        for (size_t i = 0; i < n; ++i)
            if (keep[i]) v[o++] = v[i];
        v.resize(m);
    };
    std::vector<double> X(m * static_cast<size_t>(g.k));
    for (int c = 0; c < g.k; ++c) {
        size_t o = 0;
        for (size_t i = 0; i < n; ++i)
            if (keep[i]) X[static_cast<size_t>(c) * m + o++] = g.X[static_cast<size_t>(c) * n + i];
    }
    g.X = std::move(X);
    pick(g.y);
    pick(g.weights);
    pick(g.offset);
    if (g.keep.size() == n) {
        size_t o = 0;
        for (size_t i = 0; i < n; ++i)
            if (keep[i]) g.keep[o++] = g.keep[i];
        g.keep.resize(m);
    }
    // A cluster factor keeps its level order; levels left with no row are
    // renumbered away so n_levels counts only the clusters still sampled.
    for (cecon::Factor& f : g.cluster) {
        std::vector<int32_t> codes;
        codes.reserve(m);
        for (size_t i = 0; i < n; ++i)
            if (keep[i]) codes.push_back(f.codes[i]);
        std::vector<int32_t> remap(static_cast<size_t>(f.n_levels), -1);
        for (int32_t c : codes) remap[static_cast<size_t>(c)] = 0;
        int32_t next = 0;
        for (int32_t& r : remap)
            if (r == 0) r = next++;
        std::vector<double> count(static_cast<size_t>(next), 0.0);
        for (int32_t& c : codes) {
            c = remap[static_cast<size_t>(c)];
            count[static_cast<size_t>(c)] += 1.0;
        }
        f.codes = std::move(codes);
        f.count = std::move(count);
        f.n_levels = next;
    }
    g.n = static_cast<int64_t>(m);
}

// screen_binary_design: binary_screen_glm (below) recording the design names
// before the screen, for the posted table; `xs` is the varlist g was loaded from
// (its factor base levels are screened too).
bool screen_binary_design(GlmInputs& g, bool asis, bool has_constant, const char* cmd,
                          const std::string& yname, BinaryScreenInfo& info, const Dataset& ds,
                          const std::vector<std::string>& xs) {
    info.full_names = g.names;
    const std::vector<BinaryBaseLevel> bases = asis ? std::vector<BinaryBaseLevel>()
                                                    : binary_base_levels(ds, xs, g);
    return binary_screen_glm(g, asis, has_constant, cmd, yname, info.screen, info.fit_cols, true, &bases);
}

// The name of the term a screen rule names: its design column, or the factor
// level `<level>.<var>` of a base-level rule (`stripe`: e(rules)'s `<level>b.<var>`).
std::string binary_rule_name(const BinaryPerfectRule& r, const std::vector<std::string>& names, bool stripe) {
    if (r.col >= 0) return names[static_cast<size_t>(r.col)];
    char lv[32];
    if (r.base_level == std::floor(r.base_level) && std::fabs(r.base_level) < 1e15)
        std::snprintf(lv, sizeof(lv), "%lld", static_cast<long long>(r.base_level));
    else
        std::snprintf(lv, sizeof(lv), "%.17g", r.base_level);
    return std::string(lv) + (stripe ? "b." : ".") + r.base_var;
}

// V = bread * meat * bread * factor for vce(robust) / vce(cluster), with
// meat = Sum_i e_i^2 x_i x_i' (robust) or Sum_g s_g s_g' (cluster) and e_i the
// per-row score scalar: (y_i - p_i) * w_i for logit, the generalized residual
// lambda_i for probit (probit_observed_info's score). Factors: N/(N-1) and
// G/(G-1), the ML multipliers (g3_logit_probit_vce.log:64/:122/:176/:234).
// n_robust: the N of N/(N-1) when it is not the row count (an fweight's Sum w).
void binary_score_sandwich(const GlmInputs& g, const std::vector<double>& e, const std::string& vce,
                           const std::vector<double>& bread, const char* cmd, std::vector<double>& V,
                           int& n_clusters, double n_robust = 0.0) {
    std::vector<double> meat(static_cast<size_t>(g.k) * g.k, 0.0);
    double factor;
    if (vce == "robust") {
        // Manual triple loop (NOT cecon::hc_meat) — preserves the EXACT
        // pre-P5 summation order/expression form so the unweighted vce(robust)
        // path stays byte-identical (hc_meat's la::xtx accumulates in a
        // different order; mathematically equal but not bit-identical under
        // -ffp-contract, which caught the drift against the P5 R2 baseline).
        // probit_observed_info's meat accumulates in the same order.
        for (int64_t i = 0; i < g.n; ++i) {
            double ei = e[static_cast<size_t>(i)];
            double e2 = ei * ei;
            for (int c = 0; c < g.k; ++c) {
                double xc = g.X[static_cast<int64_t>(c) * g.n + i];
                for (int d = 0; d < g.k; ++d)
                    meat[static_cast<size_t>(c) * g.k + d] += e2 * xc * g.X[static_cast<int64_t>(d) * g.n + i];
            }
        }
        // Stata applies the ML small-sample multiplier N/(N-1) to vce(robust)
        // (q_c, not the regress-style N/(N-k)); see g3_logit_probit_vce oracle.
        // Unchanged by pweight — pweight only changes the meat (via e above).
        if (n_robust > 0.0)
            factor = n_robust > 1.0 ? n_robust / (n_robust - 1.0) : 1.0;
        else
            factor = g.n > 1 ? static_cast<double>(g.n) / static_cast<double>(g.n - 1) : 1.0;
    } else {
        cecon::cluster_meat(g.X.data(), g.n, g.k, e.data(), g.cluster[0], meat.data(), &n_clusters);
        if (n_clusters < 2)
            throw EngineError("bad_input",
                std::string(cmd) + ": vce(cluster) requires at least 2 clusters, but the cluster "
                "variable has " + std::to_string(n_clusters));
        // ML cluster small-sample factor: G/(G-1) — NOT the regress-style CGM
        // (N-1)/(N-K) * G/(G-1) that vcov.cpp's compute_vcov uses for feols/2SLS.
        // Verified exact vs the P5 oracle `logit ... [pweight=pw], vce(cluster
        // id)` pin (200 clusters, N=4282): matches to <1e-6 relative. At G=N
        // (every obs its own cluster) this correctly reduces to the N/(N-1)
        // factor used for plain vce(robust) above.
        factor = static_cast<double>(n_clusters) / static_cast<double>(n_clusters - 1);
    }
    std::vector<double> tmp(static_cast<size_t>(g.k) * g.k, 0.0);
    la::sandwich(bread.data(), meat.data(), g.k, factor, V.data(), tmp.data());
}

// Header statistics of a constant model fitted with a sandwich VCE (robust,
// cluster or pweight): Stata prints a WALD chi2 on the slopes, not the LR test
// (g3_logit_probit_vce.log:41 logit vce(robust) Wald chi2(2) = 25.77, :95
// cluster, :153 probit vce(robust), :207 probit cluster). The LR test needs the
// true MLE Hessian, which a sandwich VCE no longer represents. ll/ll_0/r2_pseudo
// stay McFadden-style but computed from the (possibly pweight-)weighted
// log-likelihoods, which is what Stata labels "Log pseudolikelihood" / "Pseudo
// R2" in this case. g.names.back() is always the intercept here.
MlModelTest add_mle_fit_wald(json& out, const std::vector<double>& p, const std::vector<double>& y,
                             const double* w, int64_t n, const std::vector<double>& beta,
                             const std::vector<double>& V, int k) {
    double ll = 0.0, swy = 0.0, sw = 0.0;
    for (int64_t i = 0; i < n; ++i) {
        double wi = w ? w[static_cast<size_t>(i)] : 1.0;
        double pi = std::min(1.0 - 1e-12, std::max(1e-12, p[static_cast<size_t>(i)]));
        double yi = y[static_cast<size_t>(i)];
        ll += wi * (yi * std::log(pi) + (1.0 - yi) * std::log(1.0 - pi));
        swy += wi * yi;
        sw += wi;
    }
    out["ll"] = ll;
    const MlModelTest t = ml_model_test(V, k, beta, true);
    out["df_chi2"] = t.df;
    double ybar = (sw > 0.0) ? swy / sw : 0.0;
    if (ybar <= 0.0 || ybar >= 1.0) {
        out["ll_0"] = nullptr; out["r2_pseudo"] = nullptr;
        out["chi2"] = nullptr; out["p_chi2"] = nullptr;
        return t;
    }
    double ll0 = sw * (ybar * std::log(ybar) + (1.0 - ybar) * std::log(1.0 - ybar));
    out["ll_0"] = ll0;
    out["r2_pseudo"] = (ll0 != 0.0) ? 1.0 - ll / ll0 : 0.0;
    out["chi2"] = std::isfinite(t.chi2) ? json(t.chi2) : json(nullptr);
    out["p_chi2"] = std::isfinite(t.chi2) ? json(chi2_pvalue(t.chi2, t.df)) : json(nullptr);
    return t;
}

// The fitted coefficient rows put back in the full design order, with each
// column the screen dropped posted as Stata posts an omitted term: _b 0, _se 0,
// no z / p / interval (the bare name, so _b[dz0] still resolves; the names are
// listed under "omitted", add_omitted's convention). Identity when nothing was
// dropped.
json binary_full_coefs(const json& fit_coefs, const BinaryScreenInfo& info) {
    if (info.fit_cols.size() == info.full_names.size()) return fit_coefs;
    json full = json::array();
    size_t j = 0;
    for (size_t c = 0; c < info.full_names.size(); ++c) {
        if (j < info.fit_cols.size() && info.fit_cols[j] == static_cast<int>(c)) {
            full.push_back(fit_coefs[j++]);
            continue;
        }
        full.push_back({{"name", info.full_names[c]}, {"beta", 0.0}, {"se", 0.0}, {"t", nullptr},
                        {"se_iid", 0.0}, {"p", nullptr}, {"ci_lo", nullptr}, {"ci_hi", nullptr}});
    }
    return full;
}

// V (k_fit x k_fit) expanded to the full design with zero rows/columns for the
// omitted terms (Stata's e(V) keeps them, all zero).
std::vector<double> binary_full_V(const std::vector<double>& V, const BinaryScreenInfo& info) {
    const size_t K = info.full_names.size(), kf = info.fit_cols.size();
    if (kf == K) return V;
    std::vector<double> F(K * K, 0.0);
    for (size_t a = 0; a < kf; ++a)
        for (size_t b = 0; b < kf; ++b)
            F[static_cast<size_t>(info.fit_cols[a]) + static_cast<size_t>(info.fit_cols[b]) * K] = V[a + b * kf];
    return F;
}

// e(N_cdf) / e(N_cds), e(df_m), e(rank) (`k_vars`), e(chi2type), e(rules) and the
// omitted terms of a binary fit. A fit with no rule keeps its existing keys' bytes.
void post_binary_extras(json& out, const BinaryScreenInfo& info, const std::vector<double>& V, int k_fit,
                        bool has_constant, const MlModelTest* wald, int64_t n_cdf, int64_t n_cds) {
    out["N_cdf"] = n_cdf;
    out["N_cds"] = n_cds;
    if (wald) {
        out["df_m"] = wald->df;
        out["k_vars"] = wald->rank;
    } else {
        std::vector<int> natural;
        for (int j = 0; j < k_fit; ++j) natural.push_back(j);
        const int rank = count_kept(sweep_kept(V, k_fit, natural));
        const int df = has_constant ? rank - 1 : rank;
        out["df_m"] = df;
        out["k_vars"] = rank;
        // the LR test's degrees of freedom follow the rank (asis with V = 0: chi2(-1), p missing)
        if (out.contains("df_chi2") && out["df_chi2"].is_number_integer() && out["df_chi2"].get<int>() != df) {
            out["df_chi2"] = df;
            const bool have_chi2 = out.contains("chi2") && out["chi2"].is_number();
            out["p_chi2"] = (df > 0 && have_chi2)
                                ? json(chi2_pvalue(out["chi2"].get<double>(), static_cast<double>(df)))
                                : json(nullptr);
        }
    }
    out["chi2type"] = wald ? "Wald" : "LR";
    json rules = json::array(), names = json::array();
    for (const BinaryPerfectRule& r : info.screen.rules) {
        rules.push_back(json::array({r.op, r.rhs, r.interp, r.dropped}));
        names.push_back(binary_rule_name(r, info.full_names, true));
    }
    if (rules.empty()) rules.push_back(json::array({0, 0, 0, 0}));  // no rule fired: one all-zero row
    out["rules"] = std::move(rules);
    out["rules_names"] = std::move(names);
    json omitted = json::array();
    for (size_t c = 0; c < info.full_names.size(); ++c)
        if (std::find(info.fit_cols.begin(), info.fit_cols.end(), static_cast<int>(c)) == info.fit_cols.end())
            omitted.push_back(info.full_names[c]);
    if (!omitted.empty()) out["omitted"] = std::move(omitted);
}

// Stata's documented marksample rule ([P] mark): a string variable in
// the varlist marks every observation out, so logit and probit
// run _rmcoll on no rows and stop with "no observations" r(2000)
// (w2u_binary_ml_edges.log:3686-3692 `probit ystr x1`). A string column reaches
// the engine as a factor (non-f64) column. `no_obs` is the code PIET maps to
// r(2000); an empty sample after missing values is the same r(2000).
void require_binary_rows(const Dataset& ds, const char* cmd, const std::string& y,
                         const std::vector<std::string>& xs) {
    std::vector<std::string> names{y};
    names.insert(names.end(), xs.begin(), xs.end());
    for (const std::string& v : names)
        if (ds.has(v) && ds.col(v).dtype != DType::F64)
            throw EngineError("no_obs", std::string(cmd) + ": no observations (" + v +
                                            " is a string variable; Stata marks every row out)", v);
}

}  // namespace

// FU-138: Stata's maximize options on logit/probit (binary_ml_fit, logit.h).
// --tolerance/--ltolerance/--nrtolerance default to [R] Maximize's 1e-6/1e-7/1e-5
// and --iterate to c(maxiter) 300; --difficult is accepted and is a no-op on
// these concave objectives (w2e_probes.log:304 == :249). Returns false after
// printing the refusal (the caller exits 2). External linkage: the
// post-estimation refits in cmds_postest.cpp parse the fit's flags with it (FU-426).
bool parse_ml_max_options(const std::vector<std::string>& a, const char* cmd, MlMaxOptions& mo) {
    struct TolFlag { const char* flag; double* dst; };
    const TolFlag tols[] = {{"--tolerance", &mo.tol}, {"--ltolerance", &mo.ltol}, {"--nrtolerance", &mo.nrtol}};
    for (const TolFlag& t : tols) {
        if (!has(a, t.flag)) continue;
        const std::string s = opt(a, t.flag);
        // The whole value must parse: std::stod stops at the first character it
        // cannot read, so `1e-8x` would otherwise run as 1e-8 (BUG-872).
        double v = std::numeric_limits<double>::quiet_NaN();
        size_t used = 0;
        try { v = std::stod(s, &used); } catch (const std::exception&) {}
        if (used != s.size() || !std::isfinite(v) || v < 0.0) {
            std::fprintf(stderr, "%s: %s must be a nonnegative number (got %s).\n", cmd, t.flag, s.c_str());
            return false;
        }
        *t.dst = v;
    }
    // BUG-333: Stata's maximize_options cap iterate() at 16000. BUG-1244: iterate(0)
    // is accepted -- ml posts the starting values unconverged (StataBE `logit ybin x
    // x2, iterate(0)`: e(converged) 0, e(ic) 0, _b[x] 0, e(ll) = e(ll_0)), which is
    // binary_ml_fit's answer with no iteration.
    if (has(a, "--iterate")) {
        const std::string s = opt(a, "--iterate");
        // A plain integer only, consumed whole. BUG-872: std::stoi stops at the
        // first non-digit, so `--iterate 1e3` ran ONE iteration (rc 0, e(ic) 1,
        // e(converged) 0) and the ml_max_argv echo repeated it in every refit.
        int v = 0;
        size_t used = 0;
        try { v = std::stoi(s, &used); } catch (const std::exception&) {}
        if (used != s.size() || v < 0 || v > 16000) {
            std::fprintf(stderr, "%s: --iterate must be an integer in [0, 16000] (got %s).\n", cmd, s.c_str());
            return false;
        }
        mo.maxiter = v;
    }
    mo.difficult = has(a, "--difficult");
    return true;
}

// Stata's perfect-prediction screen (logit.h binary_perfect_screen) on the loaded
// design: a rule's rows leave the sample and its regressor leaves the fit, to be
// posted as omitted (_b 0). `asis` skips the rules but not
// the collinearity step. On
// return g holds the rows and columns left and fit_cols the original index of
// each column left; `notes` prints Stata's notes to stderr. Returns false after
// printing the r(2000) refusal (the caller exits 2): complete separation, or no variation left
// in the outcome. `bases` (may be null) are tested as columns at their place in
// the expanded varlist and never enter the fit; a rule on one is col -1.
bool binary_screen_glm(GlmInputs& g, bool asis, bool has_constant, const char* cmd,
                       const std::string& yname, BinaryPerfectScreen& screen, std::vector<int>& fit_cols,
                       bool notes, const std::vector<BinaryBaseLevel>* bases) {
    fit_cols.clear();
    for (int c = 0; c < g.k; ++c) fit_cols.push_back(c);
    screen = BinaryPerfectScreen{};
    BinaryPerfectScreen& s = screen;
    if (asis) {
        s.keep_row.assign(static_cast<size_t>(g.n), 1);
        s.omit_col.assign(static_cast<size_t>(g.k), 0);
    } else {
        const int ncheck = has_constant ? g.k - 1 : g.k;
        const size_t nb = bases ? bases->size() : 0;
        if (nb == 0) {
            s = binary_perfect_screen(g.X.data(), g.n, g.k, ncheck, g.y.data());
        } else {
            // The expanded varlist: each base level ahead of its factor's first
            // higher level (the constant stays last). src[j] is the design column
            // of screen column j, or -1 - (its base level's index).
            std::vector<int> src;
            for (int c = 0; c <= g.k; ++c) {
                for (size_t b = 0; b < nb; ++b)
                    if ((*bases)[b].before == c) src.push_back(-1 - static_cast<int>(b));
                if (c < g.k) src.push_back(c);
            }
            const int ka = static_cast<int>(src.size());
            std::vector<double> Xa(static_cast<size_t>(ka) * static_cast<size_t>(g.n));
            for (int j = 0; j < ka; ++j) {
                const double* from = src[static_cast<size_t>(j)] >= 0
                    ? g.X.data() + static_cast<std::ptrdiff_t>(src[static_cast<size_t>(j)]) * g.n
                    : (*bases)[static_cast<size_t>(-1 - src[static_cast<size_t>(j)])].col.data();
                std::copy(from, from + g.n, Xa.begin() + static_cast<std::ptrdiff_t>(j) * g.n);
            }
            BinaryPerfectScreen sa =
                binary_perfect_screen(Xa.data(), g.n, ka, ncheck + static_cast<int>(nb), g.y.data());
            s.keep_row = std::move(sa.keep_row);
            s.inestimable = sa.inestimable;
            s.omit_col.assign(static_cast<size_t>(g.k), 0);
            for (int j = 0; j < ka; ++j)
                if (src[static_cast<size_t>(j)] >= 0 && sa.omit_col[static_cast<size_t>(j)])
                    s.omit_col[static_cast<size_t>(src[static_cast<size_t>(j)])] = 1;
            for (BinaryPerfectRule r : sa.rules) {
                const int c = src[static_cast<size_t>(r.col)];
                r.col = c >= 0 ? c : -1;
                if (c < 0) {
                    r.base_var = (*bases)[static_cast<size_t>(-1 - c)].var;
                    r.base_level = (*bases)[static_cast<size_t>(-1 - c)].level;
                }
                s.rules.push_back(r);
            }
        }
        if (s.inestimable) {
            const BinaryPerfectRule& r = s.rules.back();
            std::fprintf(stderr, "%s: outcome = %s %s %.17g predicts data perfectly; r(2000)\n", cmd,
                         binary_rule_name(r, g.names, false).c_str(), r.op == 2 ? ">" : "<", r.rhs);
            return false;
        }
        for (const BinaryPerfectRule& r : s.rules) {
            if (!notes) break;
            const std::string nm = binary_rule_name(r, g.names, false);
            if (r.op == 1)
                std::fprintf(stderr, "note: %s != %.17g predicts %s perfectly; %s omitted and %lld obs not used.\n",
                             nm.c_str(), r.rhs, r.interp == 1 ? "success" : "failure", nm.c_str(),
                             static_cast<long long>(r.dropped));
            else
                std::fprintf(stderr, "note: outcome = %s %s %.17g predicts data perfectly except for %s == %.17g "
                             "subsample; %s omitted and %lld obs not used.\n", nm.c_str(), r.op == 2 ? ">" : "<",
                             r.rhs, nm.c_str(), r.rhs, nm.c_str(), static_cast<long long>(r.dropped));
        }
        compact_glm_rows(g, s.keep_row);
    }
    // _rmcoll's collinearity step over the rows the rules left: a regressor
    // collinear on those rows alone (nested indicators, an interaction cell whose
    // other rows were dropped) is omitted like Stata's "omitted because of
    // collinearity" term (_b 0), never handed to the fit as a singular column.
    // The constant is swept first so it is the term kept (collinear.h keeps the
    // earlier column); the rest keep their design order. It runs when no rule
    // fired too (BUG-1232: `logit ybin x cst`, cst constant, is `x o.cst _cons`
    // at rc 0 in StataBE, e(rules) row `cst 4 0 0 0`; the singular column went
    // to the fit and logit posted its iteration-0 point). Each omitted column
    // is an e(rules) row (4, 0, 0, 0) after the perfect-prediction rows, in
    // design order (x2.log R1: 0b.g 1 0 0 10 / cst 4 0 0 0 / 3.g 4 0 0 0).
    // Their notes print once the outcome is known to vary, so a refusal's message
    // is stderr's first line (`logit y cst` with y constant: r(2000)).
    std::vector<std::string> collinear;
    {
        std::vector<int> order;
        if (has_constant) order.push_back(g.k - 1);
        for (int c = 0; c < (has_constant ? g.k - 1 : g.k); ++c)
            if (!s.omit_col[static_cast<size_t>(c)]) order.push_back(c);
        std::vector<double> Xo;
        Xo.reserve(order.size() * static_cast<size_t>(g.n));
        for (int c : order)
            Xo.insert(Xo.end(), g.X.begin() + static_cast<std::ptrdiff_t>(c) * g.n,
                      g.X.begin() + static_cast<std::ptrdiff_t>(c + 1) * g.n);
        const std::vector<char> keep =
            cecon::collinear_keep_mask(Xo.data(), g.n, static_cast<int>(order.size()));
        for (size_t j = 0; j < order.size(); ++j) {
            if (keep[j]) continue;
            const int c = order[j];
            s.omit_col[static_cast<size_t>(c)] = 1;
            BinaryPerfectRule coll;
            coll.col = c;
            coll.op = 4;
            s.rules.push_back(coll);
            collinear.push_back(g.names[static_cast<size_t>(c)]);
        }
    }
    if (s.rules.empty()) return true;  // no rule and no collinear column: the design as loaded
    std::vector<double> X;
    std::vector<std::string> names;
    std::vector<int> cols;
    for (int c = 0; c < g.k; ++c) {
        if (s.omit_col[static_cast<size_t>(c)]) continue;
        X.insert(X.end(), g.X.begin() + static_cast<std::ptrdiff_t>(c) * g.n,
                 g.X.begin() + static_cast<std::ptrdiff_t>(c + 1) * g.n);
        names.push_back(g.names[static_cast<size_t>(c)]);
        cols.push_back(c);
    }
    g.X = std::move(X);
    g.names = std::move(names);
    g.k = static_cast<int>(cols.size());
    fit_cols = std::move(cols);
    double n1 = 0.0;
    for (double v : g.y) n1 += (v != 0.0) ? 1.0 : 0.0;
    if (g.n == 0 || n1 == 0.0 || n1 == static_cast<double>(g.n) || g.k == 0) {
        std::fprintf(stderr, "%s: outcome %s does not vary in the rows left after the perfect "
                     "predictors were dropped; r(2000)\n", cmd, yname.c_str());
        return false;
    }
    for (const std::string& nm : collinear)
        if (notes) std::fprintf(stderr, "note: %s omitted because of collinearity.\n", nm.c_str());
    return true;
}

// For each plain factor main effect of the varlist (`i.g`, `ib#.g`, `ib(first).g`
// ...; not `ibn.`, `i(numlist).` or an interaction) whose levels in the sample are
// all `<level>.g` design columns but one: that one, the base level, as a column
// over g's rows, placed before its factor's first higher level (logit.h).
std::vector<BinaryBaseLevel> binary_base_levels(const Dataset& ds, const std::vector<std::string>& xs,
                                                const GlmInputs& g) {
    std::vector<BinaryBaseLevel> out;
    if (g.keep.size() != static_cast<size_t>(g.n)) return out;
    for (const std::string& t : xs) {
        const size_t dot = t.find('.');
        if (dot == std::string::npos || t.find('#') != std::string::npos) continue;
        const std::string op = t.substr(0, dot), var = t.substr(dot + 1);
        if (op != "i" && !(op.size() > 2 && op.compare(0, 2, "ib") == 0 && op != "ibn")) continue;
        if (var.empty() || var.find_first_of(".() ") != std::string::npos || !ds.has(var) ||
            ds.col(var).dtype != DType::F64)
            continue;
        std::vector<int> cols;
        std::vector<double> levels;
        const std::string suffix = "." + var;
        for (int c = 0; c < g.k; ++c) {
            const std::string& nm = g.names[static_cast<size_t>(c)];
            if (nm.size() <= suffix.size() ||
                nm.compare(nm.size() - suffix.size(), suffix.size(), suffix) != 0)
                continue;
            const std::string head = nm.substr(0, nm.size() - suffix.size());
            if (head.find_first_not_of("0123456789") != std::string::npos) continue;
            cols.push_back(c);
            levels.push_back(std::stod(head));
        }
        if (cols.empty()) continue;
        std::vector<double> sorted(levels);
        std::sort(sorted.begin(), sorted.end());
        const double* v = ds.col(var).f64();
        BinaryBaseLevel b;
        b.var = var;
        b.col.assign(static_cast<size_t>(g.n), 0.0);
        bool found = false, single = true;
        for (int64_t i = 0; i < g.n && single; ++i) {
            const double x = v[g.keep[static_cast<size_t>(i)]];
            if (std::binary_search(sorted.begin(), sorted.end(), x)) continue;
            if (!found) {
                b.level = x;
                found = true;
            }
            single = (x == b.level);
            b.col[static_cast<size_t>(i)] = 1.0;
        }
        if (!found || !single) continue;
        b.before = cols.back() + 1;
        for (size_t j = 0; j < cols.size(); ++j)
            if (levels[j] > b.level) {
                b.before = cols[j];
                break;
            }
        out.push_back(std::move(b));
    }
    return out;
}

// compact_glm_rows for cmds_ivml.cpp's ivprobit screen (BUG-1170).
void glm_keep_rows(GlmInputs& g, const std::vector<char>& keep) { compact_glm_rows(g, keep); }

int cmd_logit(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), y = opt(a, "--y"), xcsv = opt(a, "--x");
    std::string vce = opt(a, "--vce", "iid");
    std::string weight_var = opt(a, "--weight"), wtype = opt(a, "--wtype");
    std::string cluster_var = opt(a, "--cluster");
    if (data_missing(data) || y.empty() || xcsv.empty()) {
        std::fprintf(stderr, "usage: cobra logit --data f.hbin --y Y --x X1,X2 [--vce iid|robust|cluster] "
                             "[--cluster C] [--weight W --wtype pw|fw] [--no-intercept] [--asis] [--level P] [--out f.json]\n"
                             "  [--tolerance T] [--ltolerance T] [--nrtolerance T] [--iterate N] [--difficult]\n"
                             "  --asis                   keep perfect predictors and their rows (Stata's asis).\n"
                             "                           Stata's maximize options (defaults 1e-6/1e-7/1e-5/300).\n"
                             "  --vce cluster            needs --cluster C.\n"
                             "  --weight W --wtype pw    Stata pweight (P5): forces vce(robust) or\n"
                             "                           vce(cluster ...) — Stata has no iid VCE under\n"
                             "                           pweight — and reweights the MLE by Sum w_i*loglik_i.\n");
        return 2;
    }
    if (vce != "iid" && vce != "robust" && vce != "cluster") {
        std::fprintf(stderr, "logit: --vce must be one of iid|robust|cluster (got %s).\n", vce.c_str());
        return 2;
    }
    if (vce == "cluster" && cluster_var.empty()) {
        std::fprintf(stderr, "logit: --vce cluster requires --cluster C.\n");
        return 2;
    }
    // Stata pweight (fweight: FU-585 below; Stata's logit takes no aweight, and its
    // iweight is not implemented here), and pweight always forces a sandwich VCE —
    // there is no iid pweight estimator in Stata, so reject that combination rather
    // than silently reporting a model-based SE that doesn't match anything Stata emits.
    cecon::WeightKind wkind = cecon::WeightKind::None;
    if (!weight_var.empty() && (wtype == "fw" || wtype == "fweight")) {
        // FU-585: Stata fweight (099 tables_share_remote.do:58, `logit ... [fweight=w],
        // vce(cl region)`). A row stands for w identical observations: the
        // likelihood is Sum w_i*loglik_i (the pweight fit), e(N) is Sum w, and a
        // cluster's score total is Sum w_i*s_i (the pweight meat; every copy of a
        // row is in its cluster), with the ML factor G/(G-1). The default VCE is
        // the OIM one (the weighted Hessian) under an LR header on the weighted
        // likelihoods, and vce(robust)'s meat Sum w_i s_i^2 x_i x_i' takes
        // N/(N-1) on N = Sum w (StataBE 19.5, BUG-1286 fixture, eng-ml/x11.log:
        // `logit d x [fw=wi]` e(N) 30, _se[x] .091571881918893761, LR chi2
        // .20682699177548614; vce(robust) _se[x] .090468517465118692).
        if (has(a, "--svy"))
            throw EngineError("unsupported",
                "logit: [fweight=] is not supported with svy in this version; nothing was fitted");
        wkind = cecon::WeightKind::Frequency;
    } else if (!weight_var.empty() && (wtype == "iw" || wtype == "iweight")) {
        // Stata iweight: the same weighted likelihood and OIM VCE, e(N) the row
        // count; vce(robust)'s meat is Sum (w_i s_i)^2 x_i x_i' with N/(N-1) on the
        // rows (x11.log: `logit d x [iw=wi]` e(N) 12, fw's b and _se;
        // vce(robust) _se[x] .15224312172809468). vce(cluster) is not measured.
        if (vce == "cluster" || has(a, "--svy"))
            throw EngineError("unsupported",
                "logit: [iweight=] is supported with the default VCE or vce(robust) only in "
                "this version; nothing was fitted");
        wkind = cecon::WeightKind::Importance;
    } else if (!weight_var.empty() || !wtype.empty()) {
        if (weight_var.empty())
            throw EngineError("bad_input", "logit: --wtype requires --weight");
        if (wtype != "pw" && wtype != "pweight")
            throw EngineError("bad_input", "logit: --wtype must be pw, fw or iw");
        wkind = cecon::WeightKind::Probability;
        // --svy is its own (linearized) sandwich, so it satisfies this requirement
        // without naming a --vce; without the exemption `svy: logit` would be
        // rejected for the very thing it supplies.
        if (vce == "iid" && !has(a, "--svy"))
            throw EngineError("bad_input",
                "logit: [pweight=] requires vce(robust) or vce(cluster ...); Stata has no "
                "iid VCE under pweight");
    }
    // --level P (B.0.3): asymptotic z Wald p + CI per coef, emitted ONLY when passed so
    // the default output stays byte-identical (no golden re-pin); `logit y x, level(90)`
    // now yields a real 90% CI instead of the silently-ignored option that was BUG-110's
    // concrete gate-① instance. add_inference's asymptotic branch is 95%-fixed, so the CI
    // is built inline (like qreg) from the level-driven z critical.
    bool level_set = has(a, "--level");
    double level = std::stod(opt(a, "--level", "95"));
    if (level_set && (level < 10.0 || level > 99.99)) {
        std::fprintf(stderr, "logit: --level must be in [10, 99.99] (got %s).\n", opt(a, "--level").c_str());
        return 2;
    }
    // BUG-282: `zc` was gated on level_set, so a DEFAULT `cobra logit` produced no
    // critical value and the emission below produced no p/ci_lo/ci_hi at all —
    // while Stata prints a 95% interval unprompted. `level` already defaults to
    // 95 above, and the alpha dance keeps 95 at the literal 0.05 so existing
    // --level 95 bytes are unchanged; only the DEFAULT path gains output.
    const double zc = z_critical(level == 95.0 ? 0.05 : 1.0 - level / 100.0);
    // BUG-333 gave `iterate(#)` a CLI surface; FU-138 adds the rest of Stata's
    // maximize options and moves the fit onto Stata's ml path (binary_ml_fit).
    MlMaxOptions ml_opts;
    if (!parse_ml_max_options(a, "logit", ml_opts)) return 2;
    // P7 `svy: logit`. The point estimates are the ordinary pweighted logit; svy
    // replaces the meat (stratified, over PSU totals), the reference distribution
    // (t with design dof, not z), and the model test (adjusted Wald F, not chi2).
    const bool svy = has(a, "--svy");
    std::string psu_var = opt(a, "--psu"), strata_var = opt(a, "--strata");
    if (svy) {
        // --strata is optional: one stratum, as run_feols_svy reads a design without
        // strata() (BUG-1424: StataBE `svyset id [pweight=w]`, `svy: logit d x` e(df_r)
        // 19 = #PSU - 1, _se[x] .46578656013189068).
        if (psu_var.empty())
            throw EngineError("bad_input", "logit: --svy requires --psu");
        if (wkind != cecon::WeightKind::Probability)
            throw EngineError("bad_input",
                "logit: --svy requires --weight W --wtype pw (a survey design is a "
                "probability-weighted sample)");
        if (has(a, "--vce") || !cluster_var.empty())
            throw EngineError("bad_input",
                "logit: --svy supplies its own linearized variance; --vce/--cluster are not "
                "accepted with it");
    } else if (!psu_var.empty() || !strata_var.empty()) {
        throw EngineError("bad_input", "logit: --psu/--strata are only meaningful with --svy");
    }
    // Stata `noconstant`. Not combined with --svy: its adjusted Wald F below
    // tests the slopes as "every column but the trailing intercept", which a
    // no-constant design does not have, and no svy+noconstant record is captured.
    const bool no_intercept = has(a, "--no-intercept");
    if (no_intercept && svy)
        throw EngineError("unsupported", "logit: --svy is not supported with --no-intercept");
    // e(sample) (ME07): the rows the fit used -- listwise, weight and cluster
    // losses AND the perfect-prediction screen's drops (g.keep after
    // compact_glm_rows), so `count if e(sample)` is e(N) (w2u_binary_ml_edges.log
    // :199 bm_l_dz0_fit `esample=190`).
    const EsampleRequest esreq = esample_request(a, "logit");
    const std::string es_data = data, es_keepif = opt(a, "--keepif");
    // BUG-1484: under --xtset/--tsset an L./F./D./S. regressor (`logit yb L.x1 x2`)
    // reads the FULL series and `if` applies after it (plan_keepif, as cmd_feols).
    const cecon::TsMeta tsm = parse_tsmeta(a);
    const std::vector<std::string> ts_xs = split_csv(xcsv), ts_ys{y};
    const KeepifPlan kp = plan_keepif(data, opt(a, "--keepif"), &tsm, {&ts_xs, &ts_ys});
    data = kp.data;
    const Dataset& ds = acquire_data(data);
    std::vector<std::string> cluster_names;
    if (!cluster_var.empty()) cluster_names.push_back(cluster_var);
    if (svy) {
        cluster_names.clear();
        cluster_names.push_back(psu_var);
        if (!strata_var.empty()) cluster_names.push_back(strata_var);
    }
    // BUG-331: Stata's `offset()` — a fixed-coefficient term in the linear
    // predictor. Stata implements it on logit/probit (StataBE 19.5 oracle,
    // 2026-08-17: `logit foreign mpg, offset(lnw)` is rc 0); we did not, so a
    // model Stata answers had no answer here.
    const std::string offset_var = opt(a, "--offset");
    require_binary_rows(ds, "logit", y, split_csv(xcsv));
    GlmInputs g = load_glm(ds, "logit", y, split_csv(xcsv), cluster_names, weight_var, wkind,
                           offset_var, tsm.timevar.empty() ? nullptr : &tsm,
                           kp.active ? &kp.mask : nullptr);
    if (g.n == 0) throw EngineError("no_obs", "logit: no observations");
    require_sample(g.n);  // BUG-1269: one row is r(2001) "insufficient observations"
    if (svy && strata_var.empty() && g.cluster.size() == 1) {
        // the one stratum, added after the listwise pass so it drops no row
        cecon::Factor st;
        st.n_levels = 1;
        st.codes.assign(static_cast<size_t>(g.n), 0);
        st.count.assign(1, static_cast<double>(g.n));
        g.cluster.push_back(std::move(st));
    }
    if (!check_binary(g.y, "logit", y)) return 2;
    if (no_intercept) drop_glm_intercept(g, "logit");
    if (vce == "cluster" && (g.cluster.empty() || g.cluster[0].n_levels < 2))
        throw EngineError("bad_input",
            "logit: vce(cluster) requires at least 2 clusters, but the cluster variable has " +
            std::to_string(g.cluster.empty() ? 0 : g.cluster[0].n_levels));
    // FU-462/FU-467: Stata's perfect-prediction screen before ml ([R] logit).
    BinaryScreenInfo screen;
    if (!screen_binary_design(g, has(a, "--asis"), !no_intercept, "logit", y, screen, ds, split_csv(xcsv)))
        return 2;
    const double* wptr = g.weights.empty() ? nullptr : g.weights.data();
    const double* offptr = g.offset.empty() ? nullptr : g.offset.data();
    // FU-138: Stata's iterate, not the fully converged MLE (logit.h; the
    // default stops at w2b_estim.log:630's iteration 5). The fully converged
    // answer the retired IRLS fit reported is reachable with tight flags.
    LogitResult r = logit_fit_ml(g.X.data(), g.n, g.k, g.y.data(), ml_opts, wptr, offptr, !no_intercept);
    // A separation the screen above leaves in the fit is Stata's ml's to answer, at rc 0
    // with every row completely determined: asis keeping a separating regressor
    // (w2u_binary_ml_edges.log `logit y x1 xs, asis`: ll 0, e(N_cds) 107, e(N_cdf) 93,
    // e(rank) 0, e(df_m) -1), or, without asis, a linear combination no single
    // regressor shows (BUG-1233, StataBE 19.5: `logit y x1 x2` with y = x1 + x2 > 4.5
    // posts _b[x1] 671.235060194856, ll 0, e(rank) 0 at rc 0). The IRLS-era
    // separated_perfect refusal, r(2000), is not Stata's.
    // V = bread (iid) or a sandwich bread*meat*bread. meat's per-row score is
    // e_i = (y_i - p_i), scaled by the raw pweight when one is present (wptr): under
    // pweight the score contribution to the SUM-of-weighted-loglik gradient is
    // w_i*(y_i-p_i)*x_i, so pre-scaling e by w_i and reusing the SAME hc_meat/
    // cluster_meat helpers as the unweighted case gives the pweight-correct meat
    // with no separate code path (mirrors feols's √w̃ row-scaling precedent, but
    // here only e — not X — needs the scale, since the meat is linear in e).
    // bread stays r.bread — logit_fit already folded the SAME raw weight into its
    // IRLS working weight, so r.bread is the weighted-observed-information inverse
    // (logit's link is canonical, so observed info == Fisher info; no BUG-140
    // probit-style expected/observed split needed here).
    std::vector<double> V = r.bread;
    int n_clusters = 0;
    int svy_df = 0, svy_n_psu = 0, svy_n_strata = 0;
    double svy_pop = 0.0;
    if (svy) {
        // Scores w_i (y_i − p_i) x_i. X is NOT weight-scaled on this path (logit_fit
        // folds the weight into its IRLS working weight instead), so the raw design
        // is the right one to multiply here — unlike feols, whose design arrives
        // pre-scaled by sqrt(w̃).
        std::vector<double> S(static_cast<size_t>(g.n) * g.k, 0.0);
        for (int64_t i = 0; i < g.n; ++i) {
            double ei = g.y[static_cast<size_t>(i)] - r.p[static_cast<size_t>(i)];
            if (wptr) ei *= wptr[static_cast<size_t>(i)];
            for (int c = 0; c < g.k; ++c)
                S[static_cast<size_t>(c) * g.n + i] = ei * g.X[static_cast<int64_t>(c) * g.n + i];
        }
        cecon::SvyMeatResult sm =
            cecon::svy_linearized_meat(S.data(), g.n, g.k, g.cluster[0], g.cluster[1], wptr);
        if (!sm.singleton_stratum.empty())
            throw EngineError("bad_input",
                "logit: svy stratum variable '" + sm.singleton_stratum +
                    "' has a stratum with a single sampling unit; the linearized variance "
                    "is undefined there (Stata reports missing)");
        if (!sm.ok)
            throw EngineError("bad_input",
                "logit: svy sampling units are not nested within strata");
        std::vector<double> tmp(static_cast<size_t>(g.k) * g.k, 0.0);
        la::sandwich(r.bread.data(), sm.meat.data(), g.k, 1.0, V.data(), tmp.data());
        svy_n_psu = sm.n_psu;
        svy_n_strata = sm.n_strata;
        svy_pop = sm.pop_size;
        svy_df = sm.n_psu - sm.n_strata;
        if (svy_df <= 0)
            throw EngineError("bad_input",
                "logit: svy design degrees of freedom (#PSU − #strata) is not positive");
    } else if (vce == "robust" || vce == "cluster") {
        // fweight under vce(robust): each of a row's w copies is its own term,
        // e_i^2 = w_i s_i^2, and N = Sum w (see the weight parsing above).
        const bool fw_robust = wkind == cecon::WeightKind::Frequency && vce == "robust";
        double n_robust = 0.0;
        std::vector<double> e(static_cast<size_t>(g.n));
        for (int64_t i = 0; i < g.n; ++i) {
            double ei = g.y[static_cast<size_t>(i)] - r.p[static_cast<size_t>(i)];
            if (wptr) {
                const double wi = wptr[static_cast<size_t>(i)];
                ei *= fw_robust ? std::sqrt(wi) : wi;
                n_robust += wi;
            }
            e[static_cast<size_t>(i)] = ei;
        }
        binary_score_sandwich(g, e, vce, r.bread, "logit", V, n_clusters, fw_robust ? n_robust : 0.0);
    }
    json coefs = json::array();
    for (int c = 0; c < g.k; ++c) {
        double se = std::sqrt(std::max(0.0, V[static_cast<size_t>(c) * g.k + c]));
        json row = {{"name", g.names[static_cast<size_t>(c)]}, {"beta", r.beta[c]},
                    {"se", se}, {"t", tstat(r.beta[c], se)},
                    {"se_iid", std::sqrt(std::max(0.0, r.bread[static_cast<size_t>(c) * g.k + c]))}};
        if (svy) {
            // Survey inference is t(design dof), not z — and it is emitted
            // unconditionally here, because under svy the p-value and CI are part
            // of the table Stata always prints, not a --level extra.
            const double t = tstat(r.beta[c], se);
            const double dd = static_cast<double>(svy_df);
            const double tcv = t_critical(1.0 - level / 100.0, dd);
            row["p"] = (se > 0) ? t_twosided_pvalue(t, dd) : 1.0;
            row["ci_lo"] = r.beta[c] - tcv * se;
            row["ci_hi"] = r.beta[c] + tcv * se;
        } else {
            // BUG-282: unconditional. Stata's logit table always carries P>|z| and
            // the 95% interval; gating them on --level made the default output a
            // strictly smaller table than the oracle's.
            double t = tstat(r.beta[c], se);
            row["p"] = (se > 0) ? z_twosided_pvalue(t) : 1.0;
            row["ci_lo"] = r.beta[c] - zc * se;
            row["ci_hi"] = r.beta[c] + zc * se;
        }
        coefs.push_back(row);
    }
    // e(N) under fweight is the number of observations the rows stand for.
    double n_obs = static_cast<double>(g.n);
    if (wkind == cecon::WeightKind::Frequency) {
        n_obs = 0.0;
        for (double w : g.weights) n_obs += w;
    }
    json out = {{"estimator", "logit"}, {"vce", vce}, {"has_offset", !offset_var.empty()},
                {"n_obs", wkind == cecon::WeightKind::Frequency ? json(n_obs) : json(g.n)},
                {"n_dropped_missing", g.n_dropped}, {"converged", r.converged},
                {"iters", r.iters}, {"coefs", binary_full_coefs(coefs, screen)}};
    if (vce == "cluster") { out["cluster_var"] = cluster_var; out["n_clusters"] = n_clusters; }
    if (wkind != cecon::WeightKind::None) {
        out["weight_var"] = weight_var;
        out["wtype"] = wkind == cecon::WeightKind::Frequency    ? "fw"
                       : wkind == cecon::WeightKind::Importance ? "iw"
                                                                : "pw";
    }
    if (has(a, "--asis")) out["asis"] = true;  // a refit must skip the screen too (FU-541)
    echo_ml_max_options(out, a);  // ... and stop where this fit stopped (FU-426)
    // V = the matrix the se above came from, zero rows/columns for omitted terms
    if (has(a, "--emit-v")) add_e_v(out, screen.full_names, binary_full_V(V, screen));
    if (svy) {
        out["vce"] = "svy_linearized";
        out["svy"] = {{"strata_var", strata_var}, {"psu_var", psu_var},
                      {"n_strata", svy_n_strata},  {"n_psu", svy_n_psu},
                      {"design_df", svy_df},       {"pop_size", svy_pop}};
        // Adjusted Wald F over the slopes, the same convention as `svy: regress`:
        // W·(d − q + 1)/(q·d) reported as F(q, d − q + 1). The intercept is the
        // LAST design column on this path (g.names.back()), not the first as in
        // feols — a detail worth stating, since testing for the wrong end silently
        // changes q and therefore both F and its degrees of freedom.
        const int q = g.k - 1;
        if (q > 0 && svy_df - q + 1 > 0) {
            std::vector<double> R(static_cast<size_t>(q) * g.k, 0.0), qv(static_cast<size_t>(q), 0.0);
            for (int j = 0; j < q; ++j) R[static_cast<size_t>(j) * g.k + j] = 1.0;
            cecon::WaldResult wr = cecon::wald_test(r.beta.data(), V.data(), g.k, R.data(), qv.data(), q);
            if (wr.ok) {
                const double f = wr.chi2 * static_cast<double>(svy_df - q + 1) /
                                 (static_cast<double>(q) * static_cast<double>(svy_df));
                out["F"] = f;
                out["p_F"] = f_pvalue(f, static_cast<double>(q), static_cast<double>(svy_df - q + 1));
                out["svy"]["f_df1"] = q;
                out["svy"]["f_df2"] = svy_df - q + 1;
            }
        }
    }
    // use_wald: Stata's header statistic after vce(robust), vce(cluster) or a
    // pweight is a WALD chi2 on the slopes, computed from the sandwich V
    // (g3_logit_probit_vce.log:41 `logit foreign mpg weight, vce(robust)` Wald
    // chi2(2) = 25.77; :95 vce(cluster rep78)). FU-463: plain unweighted
    // vce(robust) used to keep the LR chi2 of the OIM fit, a silent wrong e(chi2).
    // An fweight or iweight under the default VCE keeps the LR test, on the weighted
    // likelihoods (add_mle_fit_wald's weighted ll_0; post_ml_fit puts the LR chi2
    // in place of its Wald one).
    bool use_wald = (vce != "iid") || (wkind == cecon::WeightKind::Probability);
    MlModelTest wald;
    if (no_intercept) {
        wald = add_mle_fit_nocons(out, r.p, g.y, wptr, g.n, r.beta, V, g.k);
    } else if (!use_wald && wptr) {
        add_mle_fit_wald(out, r.p, g.y, wptr, g.n, r.beta, V, g.k);
    } else if (!use_wald) {
        add_mle_fit(out, r.p, g.y, g.n, g.k - 1);  // ll/ll_0/r2_pseudo/chi2/p_chi2 (since contract A 1.5.0)
    } else {
        wald = add_mle_fit_wald(out, r.p, g.y, wptr, g.n, r.beta, V, g.k);
    }
    post_ml_fit(out, r.ll, !no_intercept && !use_wald, r.ic, r.ilog);
    post_binary_extras(out, screen, V, g.k, !no_intercept, (no_intercept || use_wald) ? &wald : nullptr,
                       r.n_cdf, r.n_cds);
    add_warnings(out, g.warnings);
    if (esreq.active()) {
        EsampleRows esrows;
        esrows.keep = g.keep;
        esrows.loaded = &ds;
        emit_esample(esreq, "logit", es_data, es_keepif, kp.active, esrows, out);
    }
    emit(out, opt(a, "--out"));
    // BUG-1199: an iteration limit reached is a posted fit, not a failure: Stata's
    // logit answers rc 0 with e(converged) 0 and "Warning: Convergence not
    // achieved." (StataBE 19.5 `logit y x, iterate(1)`), and the JSON says
    // converged false. Exit 1 made a complete reply read as an engine error.
    return 0;
}

int cmd_probit(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), y = opt(a, "--y"), xcsv = opt(a, "--x");
    std::string vce = opt(a, "--vce", "iid");
    std::string cluster_var = opt(a, "--cluster");
    const std::string weight_var = opt(a, "--weight"), wtype = opt(a, "--wtype");
    if (data_missing(data) || y.empty() || xcsv.empty()) {
        std::fprintf(stderr, "usage: cobra probit --data f.hbin --y Y --x X1,X2 [--vce iid|robust|cluster] [--cluster C] "
                             "[--weight W --wtype fw|iw|pw] [--no-intercept] [--asis] [--level P] "
                             "[--tolerance T] [--ltolerance T] [--nrtolerance T] [--iterate N] [--difficult] [--out f.json]\n"
                             "  --vce cluster            needs --cluster C.\n"
                             "  --asis                   keep perfect predictors and their rows (Stata's asis).\n");
        return 2;
    }
    // Stata fweight / iweight, as cmd_logit serves them under the default VCE and
    // vce(robust) (StataBE 19.5, BUG-1286 fixture, eng-ml/x11.log: `probit d x
    // [fw=wi]` e(N) 30, _b[x] -.025652964357401924, _se[x] .056114942772508274, LR
    // chi2 .20960521381575603; vce(robust) _se[x] .055788689162290329; [iw=wi] e(N)
    // 12, the same b and _se, vce(robust) _se[x] .09388524011828428). pweight
    // (BUG-1540): the iweight fit under the sandwich Stata forces, as cmd_logit --
    // the rows' w*lambda under vce(robust), their cluster sums with G/(G-1) under
    // vce(cluster) (StataBE 19.5, scratch-laneA2/agents/wml/x1.log: `probit d x z
    // [pw=wf]` e(N) 60, _se[x] .63851717091582183; vce(cluster cl) .64646364662232192).
    // fweight and iweight under vce(cluster) are not served.
    cecon::WeightKind wkind = cecon::WeightKind::None;
    if (!weight_var.empty() || !wtype.empty()) {
        if (weight_var.empty()) throw EngineError("bad_input", "probit: --wtype requires --weight");
        if (wtype == "fw" || wtype == "fweight") wkind = cecon::WeightKind::Frequency;
        else if (wtype == "iw" || wtype == "iweight") wkind = cecon::WeightKind::Importance;
        else if (wtype == "pw" || wtype == "pweight") wkind = cecon::WeightKind::Probability;
        else throw EngineError("bad_input", "probit: --wtype must be fw, iw or pw");
        if (wkind == cecon::WeightKind::Probability && vce == "iid")
            throw EngineError("bad_input",
                "probit: [pweight=] requires vce(robust) or vce(cluster ...); Stata has no "
                "iid VCE under pweight");
        if (vce == "cluster" && wkind != cecon::WeightKind::Probability)
            throw EngineError("unsupported",
                "probit: weights are supported with the default VCE or vce(robust) only in this "
                "version; nothing was fitted");
    }
    // FU-205: validate --vce as cmd_logit does. FU-464: vce(cluster C) is served
    // through logit's clustered score sandwich (binary_score_sandwich), as Stata
    // serves it (g3_logit_probit_vce.log:197-234 probit vce(cluster rep78)).
    if (vce != "iid" && vce != "robust" && vce != "cluster") {
        std::fprintf(stderr, "probit: --vce must be one of iid|robust|cluster (got %s).\n", vce.c_str());
        return 2;
    }
    if (vce == "cluster" && cluster_var.empty()) {
        std::fprintf(stderr, "probit: --vce cluster requires --cluster C.\n");
        return 2;
    }
    MlMaxOptions ml_opts;
    if (!parse_ml_max_options(a, "probit", ml_opts)) return 2;
    // --level P (B.0.3): see cmd_logit — asymptotic z Wald p + CI per coef, flag-gated
    // (default output byte-identical), inline z critical from the level.
    bool level_set = has(a, "--level");
    double level = std::stod(opt(a, "--level", "95"));
    if (level_set && (level < 10.0 || level > 99.99)) {
        std::fprintf(stderr, "probit: --level must be in [10, 99.99] (got %s).\n", opt(a, "--level").c_str());
        return 2;
    }
    // BUG-282: unconditional, same reasoning as logit above.
    const double zc = z_critical(level == 95.0 ? 0.05 : 1.0 - level / 100.0);
    const EsampleRequest esreq = esample_request(a, "probit");  // e(sample), as cmd_logit
    const std::string es_data = data, es_keepif = opt(a, "--keepif");
    const cecon::TsMeta tsm = parse_tsmeta(a);  // BUG-1484, as cmd_logit
    const std::vector<std::string> ts_xs = split_csv(xcsv), ts_ys{y};
    const KeepifPlan kp = plan_keepif(data, opt(a, "--keepif"), &tsm, {&ts_xs, &ts_ys});
    data = kp.data;
    const Dataset& ds = acquire_data(data);
    const std::string offset_var = opt(a, "--offset");  // BUG-331, see cmd_logit
    std::vector<std::string> cluster_names;
    if (vce == "cluster") cluster_names.push_back(cluster_var);
    require_binary_rows(ds, "probit", y, split_csv(xcsv));
    GlmInputs g = load_glm(ds, "probit", y, split_csv(xcsv), cluster_names, weight_var, wkind,
                           offset_var, tsm.timevar.empty() ? nullptr : &tsm,
                           kp.active ? &kp.mask : nullptr);
    if (g.n == 0) throw EngineError("no_obs", "probit: no observations");
    require_sample(g.n);  // BUG-1269: one row is r(2001) "insufficient observations"
    if (!check_binary(g.y, "probit", y)) return 2;
    // Stata `noconstant` (w2b_estim.log:706/:732, e(k) 2): see drop_glm_intercept.
    const bool no_intercept = has(a, "--no-intercept");
    if (no_intercept) drop_glm_intercept(g, "probit");
    if (vce == "cluster" && (g.cluster.empty() || g.cluster[0].n_levels < 2))
        throw EngineError("bad_input",
            "probit: vce(cluster) requires at least 2 clusters, but the cluster variable has " +
            std::to_string(g.cluster.empty() ? 0 : g.cluster[0].n_levels));
    // FU-462/FU-467: Stata's perfect-prediction screen before ml ([R] probit).
    BinaryScreenInfo screen;
    if (!screen_binary_design(g, has(a, "--asis"), !no_intercept, "probit", y, screen, ds, split_csv(xcsv)))
        return 2;
    const double* offptr = g.offset.empty() ? nullptr : g.offset.data();
    const double* wptr = g.weights.empty() ? nullptr : g.weights.data();
    // FU-138 / FU-210: Stata's ml iterate (logit.h; w2b_estim.log:738-743 stops
    // at iteration 5), not the fully converged IRLS answer reported before.
    ProbitResult r = probit_fit_ml(g.X.data(), g.n, g.k, g.y.data(), ml_opts, offptr, !no_intercept, wptr);
    // A separation the screen leaves in the fit is answered at rc 0 with every row
    // completely determined, as cmd_logit answers it (asis: w2u_binary_ml_edges.log
    // `probit y x1 xs, asis`, ll 0, e(N_cds) 107, e(N_cdf) 93, e(rank) 0, e(df_m) -1;
    // BUG-1233 without asis: `probit y x1 x2`, _b[x1] 160.026937533279, ll 0).
    // probit score:  s_i = (y-Φ) φ / (Φ(1-Φ)) · x   (λ_i = generalized residual).
    // meat = Σ s_i s_i'. Bread = OBSERVED-information inverse: Stata's probit uses
    // the observed Hessian, NOT the Fisher/expected info (X'WX)^{-1} that r.V
    // carries (probit is a non-canonical link, so the two differ in-sample; logit
    // is canonical, hence its bread stays r.bread). The observed information
    // weight is λ_i(λ_i+η_i); see g3_logit_probit_vce oracle. BUG-140: the DEFAULT
    // (iid) SE needs this same observed-information bread, not just vce(robust)'s
    // sandwich meat — Stata's plain `probit` already uses observed info, cobra's
    // iid branch was the only one still reading the Fisher-info r.V.
    //
    // BUG-210: this loop used to live here verbatim, and in two more places, and
    // `margins`' iid branches were left reading r.V because of that. It is now
    // ONE definition in src/estimators/probit.h; this is a delegation.
    // The SAME offset must reach the observed information, or the SEs would be
    // evaluated at a different eta than the fit converged to (probit.h).
    ProbitObservedInfo oi = probit_observed_info(r, g.X.data(), g.n, g.k, g.y.data(), offptr);
    // FU-210: oi.bread is the ml fit's own bread_oim (probit.h), which drops
    // completely determined rows as Stata's does (w2b_estim.log:762).
    const std::vector<double>& bread = oi.bread;
    std::vector<double> V = bread;   // default/iid SE (BUG-140: was r.V, the Fisher bread)
    int n_clusters = 0;
    // vce(robust) / vce(cluster): logit's sandwich on the per-row generalized
    // residual (oi.score); its robust meat is oi.meat term for term. Weighted as
    // cmd_logit weights it: sqrt(w) lambda with N = Sum w under fw, w lambda under iw
    // and pw.
    if (vce == "robust" || vce == "cluster") {
        if (!wptr) {
            binary_score_sandwich(g, oi.score, vce, bread, "probit", V, n_clusters);
        } else {
            const bool fw = wkind == cecon::WeightKind::Frequency;
            std::vector<double> e(oi.score);
            double sw = 0.0;
            for (int64_t i = 0; i < g.n; ++i) {
                const double wi = wptr[static_cast<size_t>(i)];
                e[static_cast<size_t>(i)] *= fw ? std::sqrt(wi) : wi;
                sw += wi;
            }
            binary_score_sandwich(g, e, vce, bread, "probit", V, n_clusters, fw ? sw : 0.0);
        }
    }
    json coefs = json::array();
    for (int c = 0; c < g.k; ++c) {
        double se = std::sqrt(std::max(0.0, V[static_cast<size_t>(c) * g.k + c]));
        json row = {{"name", g.names[static_cast<size_t>(c)]}, {"beta", r.beta[c]},
                    {"se", se}, {"t", tstat(r.beta[c], se)},
                    {"se_iid", std::sqrt(std::max(0.0, r.V[static_cast<size_t>(c) * g.k + c]))}};
        {
            // BUG-282: unconditional (Stata's probit table always carries them).
            double t = tstat(r.beta[c], se);
            row["p"] = (se > 0) ? z_twosided_pvalue(t) : 1.0;
            row["ci_lo"] = r.beta[c] - zc * se;
            row["ci_hi"] = r.beta[c] + zc * se;
        }
        coefs.push_back(row);
    }
    // e(N): the rows, or under an fweight the observations they stand for.
    double sum_fw = 0.0;
    if (wkind == cecon::WeightKind::Frequency)
        for (double w : g.weights) sum_fw += w;
    json out = {{"estimator", "probit"}, {"vce", vce}, {"has_offset", !offset_var.empty()},
                {"n_obs", wkind == cecon::WeightKind::Frequency ? json(sum_fw) : json(g.n)},
                {"n_dropped_missing", g.n_dropped}, {"converged", r.converged},
                {"iters", r.iters}, {"coefs", binary_full_coefs(coefs, screen)}};
    if (vce == "cluster") { out["cluster_var"] = cluster_var; out["n_clusters"] = n_clusters; }
    if (wkind != cecon::WeightKind::None) {
        out["weight_var"] = weight_var;
        out["wtype"] = wkind == cecon::WeightKind::Frequency    ? "fw"
                       : wkind == cecon::WeightKind::Importance ? "iw"
                                                                : "pw";
    }
    if (has(a, "--asis")) out["asis"] = true;  // a refit must skip the screen too (FU-541)
    echo_ml_max_options(out, a);  // ... and stop where this fit stopped (FU-426)
    // V = the matrix the se above came from, zero rows/columns for omitted terms
    if (has(a, "--emit-v")) add_e_v(out, screen.full_names, binary_full_V(V, screen));
    // FU-463: the header statistic after vce(robust)/vce(cluster) is the Wald
    // chi2 on the slopes (g3_logit_probit_vce.log:153, :207), as in cmd_logit.
    const bool use_wald = (vce != "iid");
    MlModelTest wald;
    if (no_intercept)
        wald = add_mle_fit_nocons(out, r.p, g.y, wptr, g.n, r.beta, V, g.k);
    else if (!use_wald && wptr)
        add_mle_fit_wald(out, r.p, g.y, wptr, g.n, r.beta, V, g.k);  // weighted ll_0, LR below (cmd_logit)
    else if (!use_wald)
        add_mle_fit(out, r.p, g.y, g.n, g.k - 1);  // ll/ll_0/r2_pseudo/chi2/p_chi2 (since contract A 1.5.0)
    else
        wald = add_mle_fit_wald(out, r.p, g.y, wptr, g.n, r.beta, V, g.k);
    post_ml_fit(out, r.ll, !no_intercept && !use_wald, r.ic, r.ilog);
    post_binary_extras(out, screen, V, g.k, !no_intercept, (no_intercept || use_wald) ? &wald : nullptr,
                       r.n_cdf, r.n_cds);
    add_warnings(out, g.warnings);
    if (esreq.active()) {
        EsampleRows esrows;
        esrows.keep = g.keep;
        esrows.loaded = &ds;
        emit_esample(esreq, "probit", es_data, es_keepif, kp.active, esrows, out);
    }
    emit(out, opt(a, "--out"));
    return 0;  // BUG-1199: not converged is still a posted fit (cmd_logit)
}

// cloglog on an already screened design (shared with cmds_postest.cpp's refits).
// Start: a constant-only model starts at its closed form z0; otherwise it matches
// Stata's cloglog (starts from `logit ..., asis iterate(3)`), identified from its ado.
BinaryMlResult cloglog_ml(const GlmInputs& g, bool has_constant, double z0, const MlMaxOptions& mo) {
    const double* wptr = g.weights.empty() ? nullptr : g.weights.data();
    std::vector<double> start;
    if (has_constant && g.k == 1) {
        start.assign(1, z0);
    } else {
        MlMaxOptions lo;
        lo.maxiter = 3;
        start = binary_ml_fit(BinaryLink::Logit, g.X.data(), g.n, g.k, g.y.data(), wptr, nullptr, has_constant, lo)
                    .beta;
    }
    return binary_ml_fit(BinaryLink::Cloglog, g.X.data(), g.n, g.k, g.y.data(), wptr, nullptr, has_constant, mo,
                         &start);
}

// A success row's log likelihood at index z in the constant-only model (e(ll_0)),
// with the lower-tail value measured on StataBE (clog_tail P2/P3).
static double cloglog_success_ll0(double z) {
    const double u = std::exp(z);
    if (z > 100.0) return 0.0;
    if (z < -12.0) return z - (u / 2.0) * (1.0 - u) / 12.0;
    return std::log(-std::expm1(-u));
}

// d ll_i / d eta of a cloglog row ([R] cloglog Methods and formulas).
static double cloglog_score(double eta, bool success) {
    const double u = std::exp(eta);
    if (!success) return -u;
    return std::exp(eta - u) / (-std::expm1(-u));
}

int cmd_cloglog(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), y = opt(a, "--y"), xcsv = opt(a, "--x");
    std::string vce = opt(a, "--vce", "iid");
    const std::string cluster_var = opt(a, "--cluster");
    const std::string weight_var = opt(a, "--weight"), wtype = opt(a, "--wtype");
    if (data_missing(data) || y.empty() || xcsv.empty()) {
        std::fprintf(stderr,
                     "usage: cobra cloglog --data f.hbin --y Y --x X1,X2 [--vce iid|robust|cluster] [--cluster C] "
                     "[--weight W --wtype fw|iw|pw] [--no-intercept] [--asis] [--level P] [--tolerance T] "
                     "[--ltolerance T] [--nrtolerance T] [--iterate N] [--difficult] [--emit-v] [--out f.json]\n"
                     "  --vce cluster            needs --cluster C.\n"
                     "  --wtype pw               forces vce(robust) or vce(cluster ...).\n"
                     "  --asis                   keep perfect predictors and their rows (Stata's asis).\n");
        return 2;
    }
    if (vce != "iid" && vce != "robust" && vce != "cluster") {
        std::fprintf(stderr, "cloglog: --vce must be one of iid|robust|cluster (got %s).\n", vce.c_str());
        return 2;
    }
    if (vce == "cluster" && cluster_var.empty()) {
        std::fprintf(stderr, "cloglog: --vce cluster requires --cluster C.\n");
        return 2;
    }
    cecon::WeightKind wkind = cecon::WeightKind::None;
    if (!weight_var.empty() || !wtype.empty()) {
        if (weight_var.empty()) throw EngineError("bad_input", "cloglog: --wtype requires --weight");
        if (wtype == "fw" || wtype == "fweight") wkind = cecon::WeightKind::Frequency;
        else if (wtype == "iw" || wtype == "iweight") wkind = cecon::WeightKind::Importance;
        else if (wtype == "pw" || wtype == "pweight") wkind = cecon::WeightKind::Probability;
        else throw EngineError("bad_input", "cloglog: --wtype must be fw, iw or pw");
    }
    // A pweight implies the sandwich VCE (Stata has no OIM VCE under pweight).
    if (wkind == cecon::WeightKind::Probability && vce == "iid") vce = "robust";
    const bool level_set = has(a, "--level");
    const double level = std::stod(opt(a, "--level", "95"));
    if (level_set && (level < 10.0 || level > 99.99)) {
        std::fprintf(stderr, "cloglog: --level must be in [10, 99.99] (got %s).\n", opt(a, "--level").c_str());
        return 2;
    }
    const double zc = z_critical(level == 95.0 ? 0.05 : 1.0 - level / 100.0);
    MlMaxOptions mo;
    if (!parse_ml_max_options(a, "cloglog", mo)) return 2;

    const EsampleRequest esreq = esample_request(a, "cloglog");
    const std::string es_data = data, es_keepif = opt(a, "--keepif");
    const cecon::TsMeta tsm = parse_tsmeta(a);
    const std::vector<std::string> xs = split_csv(xcsv), ts_ys{y};
    const KeepifPlan kp = plan_keepif(data, opt(a, "--keepif"), &tsm, {&xs, &ts_ys});
    data = kp.data;
    const Dataset& ds = acquire_data(data);
    std::vector<std::string> cluster_names;
    if (vce == "cluster") cluster_names.push_back(cluster_var);
    require_binary_rows(ds, "cloglog", y, xs);
    GlmInputs g = load_glm(ds, "cloglog", y, xs, cluster_names, weight_var, wkind, "",
                           tsm.timevar.empty() ? nullptr : &tsm, kp.active ? &kp.mask : nullptr);
    if (g.n == 0) throw EngineError("no_obs", "cloglog: no observations");
    if (!check_binary(g.y, "cloglog", y)) return 2;
    const bool no_intercept = has(a, "--no-intercept");
    if (no_intercept) drop_glm_intercept(g, "cloglog");
    if (vce == "cluster" && (g.cluster.empty() || g.cluster[0].n_levels < 2))
        throw EngineError("bad_input",
            "cloglog: vce(cluster) requires at least 2 clusters, but the cluster variable has " +
            std::to_string(g.cluster.empty() ? 0 : g.cluster[0].n_levels));

    // Counts over the loaded rows, before the screen (the unweighted / iweight z0).
    const int64_t n_pre = g.n;
    double n0_pre = 0.0, w_pre = 0.0;
    for (int64_t i = 0; i < g.n; ++i) {
        w_pre += g.weights.empty() ? 1.0 : g.weights[static_cast<size_t>(i)];
        if (g.y[static_cast<size_t>(i)] == 0.0) n0_pre += 1.0;
    }
    if (n0_pre == 0.0 || n0_pre == static_cast<double>(n_pre)) {
        std::fprintf(stderr, "cloglog: outcome %s does not vary; r(2000)\n", y.c_str());
        return 2;
    }

    BinaryScreenInfo info;
    const bool asis = has(a, "--asis");
    if (!screen_binary_design(g, asis, !no_intercept, "cloglog", y, info, ds, xs)) return 2;
    const double* wptr = g.weights.empty() ? nullptr : g.weights.data();
    double w0 = 0.0, w1 = 0.0;  // failure / success weight sums after the screen (row counts unweighted)
    int64_t rows0 = 0;
    for (int64_t i = 0; i < g.n; ++i) {
        const double wi = wptr ? wptr[i] : 1.0;
        if (g.y[static_cast<size_t>(i)] == 0.0) {
            w0 += wi;
            ++rows0;
        } else {
            w1 += wi;
        }
    }
    if (!(w0 > 0.0) || !(w1 > 0.0)) {
        std::fprintf(stderr, "cloglog: outcome %s does not vary; r(2000)\n", y.c_str());
        return 2;
    }
    // Constant-only index z0 = ln(-ln(1 - p)) = ln(ln(A/B)), with A and B as
    // measured on StataBE (oracle clog_w / clog_w2 / x18 C2).
    double z0;
    if (wkind == cecon::WeightKind::None)
        z0 = std::log(std::log(static_cast<double>(n_pre) / n0_pre));
    else if (wkind == cecon::WeightKind::Importance)
        z0 = std::log(std::log(w_pre / w0));
    else
        z0 = std::log(std::log((w0 + w1) / w0));
    // e(ll_0), summed by outcome group.
    const double lf0 = w0 * (-std::exp(z0)) + w1 * cloglog_success_ll0(z0);

    const BinaryMlResult r = cloglog_ml(g, !no_intercept, z0, mo);
    std::vector<double> V = r.bread;
    int n_clusters = 0;
    if (vce == "robust" || vce == "cluster") {
        const bool fw_robust = wkind == cecon::WeightKind::Frequency && vce == "robust";
        std::vector<double> eta(static_cast<size_t>(g.n));
        la::mat_vec(g.X.data(), g.n, g.k, r.beta.data(), eta.data());
        std::vector<double> e(static_cast<size_t>(g.n));
        for (int64_t i = 0; i < g.n; ++i) {
            double ei = cloglog_score(eta[static_cast<size_t>(i)], g.y[static_cast<size_t>(i)] != 0.0);
            if (wptr) ei *= fw_robust ? std::sqrt(wptr[i]) : wptr[i];
            e[static_cast<size_t>(i)] = ei;
        }
        binary_score_sandwich(g, e, vce, r.bread, "cloglog", V, n_clusters, fw_robust ? (w0 + w1) : 0.0);
    }

    json coefs = json::array();
    for (int c = 0; c < g.k; ++c) {
        const size_t cc = static_cast<size_t>(c) * static_cast<size_t>(g.k) + static_cast<size_t>(c);
        const double bc = r.beta[static_cast<size_t>(c)];
        const double se = std::sqrt(std::max(0.0, V[cc]));
        const double t = tstat(bc, se);
        coefs.push_back({{"name", g.names[static_cast<size_t>(c)]}, {"beta", bc}, {"se", se}, {"t", t},
                         {"se_iid", std::sqrt(std::max(0.0, r.bread[cc]))},
                         {"p", se > 0 ? z_twosided_pvalue(t) : 1.0},
                         {"ci_lo", bc - zc * se}, {"ci_hi", bc + zc * se}});
    }
    const bool fw = wkind == cecon::WeightKind::Frequency;
    json out = {{"estimator", "cloglog"}, {"vce", vce},
                {"n_obs", fw ? json(w0 + w1) : json(g.n)},
                {"n_dropped_missing", g.n_dropped}, {"converged", r.converged},
                {"iters", r.ic}, {"coefs", binary_full_coefs(coefs, info)}};
    if (vce == "cluster") {
        out["cluster_var"] = cluster_var;
        out["n_clusters"] = n_clusters;
    }
    if (wkind != cecon::WeightKind::None) {
        out["weight_var"] = weight_var;
        out["wtype"] = fw ? "fw" : wkind == cecon::WeightKind::Importance ? "iw" : "pw";
    }
    if (asis) out["asis"] = true;
    echo_ml_max_options(out, a);
    if (has(a, "--emit-v")) add_e_v(out, info.full_names, binary_full_V(V, info));

    // Header test: Wald after a sandwich VCE or noconstant, else LR against lf0.
    const bool use_wald = vce != "iid" || no_intercept;
    MlModelTest wald;
    if (no_intercept) {
        wald = add_mle_fit_nocons(out, r.p, g.y, wptr, g.n, r.beta, V, g.k);
    } else {
        wald = add_mle_fit_wald(out, r.p, g.y, wptr, g.n, r.beta, V, g.k);
        out["ll_0"] = lf0;
    }
    post_ml_fit(out, r.ll, !use_wald, r.ic, r.ilog);
    post_binary_extras(out, info, V, g.k, !no_intercept, use_wald ? &wald : nullptr, 0, 0);
    for (const char* key : {"N_cdf", "N_cds", "rules", "rules_names", "r2_pseudo"}) out.erase(key);

    if (fw) {
        out["N_f"] = w0;
        out["N_s"] = w1;
    } else {
        out["N_f"] = rows0;
        out["N_s"] = g.n - rows0;
    }
    if (level_set) out["level"] = level;
    add_warnings(out, g.warnings);
    if (esreq.active()) {
        EsampleRows esrows;
        esrows.keep = g.keep;
        esrows.loaded = &ds;
        emit_esample(esreq, "cloglog", es_data, es_keepif, kp.active, esrows, out);
    }
    emit(out, opt(a, "--out"));
    return r.converged ? 0 : 1;  // current contract: not converged exits 1
}

}  // namespace cli
}  // namespace cecon
