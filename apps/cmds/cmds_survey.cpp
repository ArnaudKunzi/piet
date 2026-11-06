// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "common.h"
#include "commands.h"

#include <map>

#include "vcov/svy_vcov.h"

// P3 answerbook wave: `mean` and `proportion` (Stata survey estimation commands).
// Both share one estimator shape — a weighted (or unweighted, w≡1) mean of some
// 0/1 or continuous quantity, with the linearized-variance formula Stata uses for
// `mean`/`proportion` under pweights (the same formula collapses exactly onto the
// textbook unweighted sample-mean SE when every weight is 1, so one code path
// covers both). `proportion` layers a per-level indicator + Stata's default
// logit-transformed CI on top of that shared core.
//
// --wtype is intentionally narrow: only `pw`/`pweight` is accepted. aw/fw/iw carry
// DIFFERENT variance semantics for these two estimators (not `w_i` on both sides of
// the ratio the way pw is) and none of those have been checked against a Stata
// oracle here — guessing would risk a silently wrong SE, so anything else is an
// honest bad_input reject rather than a guess.

namespace cecon {
namespace cli {

namespace {

// Validate --weight/--wtype for mean/proportion. Absent --weight ⇒ unweighted
// (w≡1); present --weight with no --wtype defaults to pw; any --wtype without
// --weight is meaningless (bad_input); any --wtype other than pw/pweight is the
// deliberate honest reject documented above, except the ones `mean` serves
// (allow_aw: aw, and fw/iw, each measured against StataBE) and `proportion`
// serves (allow_fw_iw: fw/iw, BUG-1541).
void check_wtype(const std::vector<std::string>& a, const char* cmd, const std::string& weight,
                 bool allow_aw = false, bool allow_fw_iw = false) {
    bool wtype_set = has(a, "--wtype");
    std::string wtype = opt(a, "--wtype", "pw");
    if (weight.empty()) {
        if (wtype_set)
            throw EngineError("bad_input", std::string(cmd) + ": --wtype requires --weight");
        return;
    }
    if (allow_aw && (wtype == "aw" || wtype == "aweight" || wtype == "fw" || wtype == "fweight" ||
                     wtype == "iw" || wtype == "iweight"))
        return;
    if (allow_fw_iw && (wtype == "fw" || wtype == "fweight" || wtype == "iw" || wtype == "iweight"))
        return;
    if (wtype != "pw" && wtype != "pweight")
        throw EngineError("bad_input",
            std::string(cmd) + ": only probability weights (pw) are supported for " + cmd +
            " in this engine — aweight/fweight/iweight variance semantics for this estimator "
            "are not verified against a Stata oracle here");
}

// Casewise-common estimation sample: row i is kept iff every listed analysis
// column is non-missing AND (if weighted) the weight is non-missing — the same
// "listwise across the whole varlist" rule Stata's estimation commands use (mean/
// proportion behave like regress here, not like summarize's per-variable sample).
std::vector<int64_t> casewise_keep(const std::vector<const double*>& cols, const double* w, int64_t N) {
    std::vector<int64_t> keep;
    for (int64_t i = 0; i < N; ++i) {
        bool ok = true;
        for (const double* c : cols) if (std::isnan(c[i])) { ok = false; break; }
        if (ok && w && std::isnan(w[i])) ok = false;
        if (ok) keep.push_back(i);
    }
    return keep;
}

// NaN → JSON null (house convention — see cmds_summary.cpp's mat()/row["p"] guards);
// never let a raw NaN reach nlohmann's dump().
json nz(double v) { return std::isnan(v) ? json(nullptr) : json(v); }

// The full covariance of K estimates whose variances are scale * Sum_i a_i z_ij^2, with
// the cross terms that formula implies: V_jk = scale * Sum_i a_i z_ij z_ik. `z` holds the
// estimation sample's K centered columns (row r of column j at z[j][r]), `a` one factor
// per row. Emitted as `e_V` (add_e_v) in column order when every entry is finite.
void add_full_e_v(json& out, const std::vector<std::string>& names,
                  const std::vector<std::vector<double>>& z, const std::vector<double>& a,
                  double scale) {
    const size_t k = names.size();
    std::vector<double> V(k * k, 0.0);
    for (size_t j = 0; j < k; ++j) {
        for (size_t l = 0; l <= j; ++l) {
            double s = 0.0;
            for (size_t r = 0; r < a.size(); ++r) s += a[r] * z[j][r] * z[l][r];
            V[j + l * k] = V[l + j * k] = scale * s;
        }
    }
    for (double v : V)
        if (!std::isfinite(v)) return;
    add_e_v(out, names, V);
}

// Render a level value the way categorize() (cmds_summary.cpp) renders numeric
// labels: an integral value prints without a decimal point ("0", not "0.000000").
std::string level_name(double v) {
    char bb[32];
    if (v == std::floor(v) && std::fabs(v) < 1e15)
        std::snprintf(bb, sizeof(bb), "%lld", static_cast<long long>(v));
    else
        std::snprintf(bb, sizeof(bb), "%g", v);
    return bb;
}

}  // namespace

// ── mean ─────────────────────────────────────────────────────────────────────
int cmd_mean(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data");
    std::vector<std::string> vars = split_csv(opt(a, "--vars"));
    if (data_missing(data) || vars.empty()) {
        std::fprintf(stderr, "usage: cobra mean --data f.hbin --vars Y[,Y2,...] "
                             "[--weight W --wtype pw|aw|fw|iw] [--level P] [--keepif EXPR] [--out f.json]\n"
                             "  --weight W   Stata [pw=W] (default), [aw=W], [fw=W] or [iw=W]\n"
                             "  --level P    confidence level for ci_lo/ci_hi, 10 < P <= 99.99 (default 95).\n");
        return 2;
    }
    std::string weight = opt(a, "--weight");
    check_wtype(a, "mean", weight, /*allow_aw=*/true);
    // Stata `mean [aw=]` (w2aj_qualifiers.log:1983 qf_mn_aw_sp): b = Sum wx/Sum w and
    // se = sqrt(Sum v(x-b)^2/(n-1)/n) with v = w*n/Sum w, e(N) the row count.
    // `mean [fw=]` / `[iw=]`: the replicated sample, W = Sum w observations:
    // se = sqrt(Sum w(x-b)^2/(W-1)/W), e(N) = W, e(df_r) = W - 1, an iweight's
    // fractional W included (StataBE 19.5, scratch-laneA/agents/eng-ml/x1.log E1-E3:
    // [fw=wi] e(N) 30, se .79196757741599555, e(df_r) 29; [iw=wf] e(N) 28.5,
    // b 6.1842105263157894, se .80066889479364045, e(df_r) 27.5).
    const std::string wtype_in = opt(a, "--wtype", "pw");
    const bool analytic_w = !weight.empty() && (wtype_in == "aw" || wtype_in == "aweight");
    const bool freq_fw = !weight.empty() && (wtype_in == "fw" || wtype_in == "fweight");
    const bool freq_w = freq_fw || (!weight.empty() && (wtype_in == "iw" || wtype_in == "iweight"));
    bool level_set = has(a, "--level");
    double level = std::stod(opt(a, "--level", "95"));
    if (level_set && (level < 10.0 || level > 99.99)) {
        std::fprintf(stderr, "mean: --level must be in [10, 99.99] (got %s).\n",
                     opt(a, "--level").c_str());
        return 2;
    }
    data = maybe_keepif(data, opt(a, "--keepif"));
    const Dataset& ds = acquire_data(data);
    const int64_t N = ds.n_rows();
    std::vector<const double*> col(vars.size());
    for (size_t j = 0; j < vars.size(); ++j) col[j] = require_num(ds, "mean", vars[j]);
    const double* w = weight.empty() ? nullptr : require_num(ds, "mean", weight);

    // P7 `svy: mean`. The single-stage formula below is exactly the degenerate case
    // of the survey one (one stratum whose PSUs are the observations), so svy does
    // not replace it — it generalizes the variance to PSU totals centered within
    // stratum, and the reference dof from n−1 to #PSU − #strata.
    const bool svy = has(a, "--svy");
    std::string psu_var = opt(a, "--psu"), strata_var = opt(a, "--strata");
    const double *psu_col = nullptr, *strata_col = nullptr;
    if (svy && (analytic_w || freq_w))
        throw EngineError("bad_input", "mean: --svy takes probability weights only");
    if (svy) {
        // --strata is optional: a design without strata() is one stratum, as
        // run_feols_svy reads it (BUG-1424: StataBE `svyset id [pweight=w]`, `svy:
        // mean y` e(N_strata) 1, e(df_r) = #PSU - 1 = 19, _se[y] .023028889911831222).
        if (psu_var.empty())
            throw EngineError("bad_input", "mean: --svy requires --psu");
        if (weight.empty())
            throw EngineError("bad_input",
                "mean: --svy requires --weight W --wtype pw (a survey design is a "
                "probability-weighted sample)");
        psu_col = require_num(ds, "mean", psu_var);
        if (!strata_var.empty()) strata_col = require_num(ds, "mean", strata_var);
    } else if (!psu_var.empty() || !strata_var.empty()) {
        throw EngineError("bad_input", "mean: --psu/--strata are only meaningful with --svy");
    }

    std::vector<const double*> keep_cols = col;
    if (svy) {
        keep_cols.push_back(psu_col);
        if (strata_col) keep_cols.push_back(strata_col);
    }
    std::vector<int64_t> keep = casewise_keep(keep_cols, w, N);
    const int64_t n = static_cast<int64_t>(keep.size());
    double W = 0; for (int64_t i : keep) W += w ? w[i] : 1.0;
    if (freq_fw)
        for (int64_t i : keep)
            if (w[i] != std::floor(w[i]))
                throw EngineError("bad_input", "mean: fweight '" + weight + "' has a non-integer value");

    cecon::Factor f_psu, f_strata;
    int design_df = 0;
    if (svy) {
        auto densify = [&](const double* src, const std::string& nm) {
            cecon::Factor f;
            f.name = nm;
            f.codes.resize(static_cast<size_t>(n));
            std::map<double, int> lev;
            for (int64_t r = 0; r < n; ++r) {
                double v = src[keep[static_cast<size_t>(r)]];
                auto it = lev.find(v);
                if (it == lev.end()) it = lev.emplace(v, static_cast<int>(lev.size())).first;
                f.codes[static_cast<size_t>(r)] = it->second;
            }
            f.n_levels = static_cast<int>(lev.size());
            f.count.assign(static_cast<size_t>(f.n_levels), 0.0);
            for (int c : f.codes) f.count[static_cast<size_t>(c)] += 1.0;
            return f;
        };
        f_psu = densify(psu_col, psu_var);
        if (strata_col) {
            f_strata = densify(strata_col, strata_var);
        } else {
            f_strata.n_levels = 1;
            f_strata.codes.assign(static_cast<size_t>(n), 0);
            f_strata.count.assign(1, static_cast<double>(n));
        }
        design_df = f_psu.n_levels - f_strata.n_levels;
        if (design_df <= 0)
            throw EngineError("bad_input",
                "mean: svy design degrees of freedom (#PSU − #strata) is not positive");
    }

    const double dfree = svy ? static_cast<double>(design_df)
                             : freq_w ? W - 1.0 : static_cast<double>(n - 1);
    const double alpha = 1.0 - level / 100.0;
    const double tc = t_critical(alpha, dfree);

    // coef/se are not enough. `--tidy`'s coeftable builder (tidy_coeftable) takes the
    // coef{} branch whenever coef{} exists, and reads t/p/CI from the SIBLING maps
    // tstat/pvalue/ci_lo/ci_hi — never from coefs[]. Emitting coef/se alone therefore
    // renders a table with the estimate and SE filled in and t, p and the confidence
    // interval blank, which for `mean`/`proportion` is exactly the column Stata puts
    // the CI in. Every map below is filled from the SAME per-row values as coefs[], so
    // the two representations cannot drift apart.
    json coef = json::object(), se_map = json::object(), coefs = json::array();
    json t_map = json::object(), p_map = json::object(),
         lo_map = json::object(), hi_map = json::object();
    for (size_t j = 0; j < vars.size(); ++j) {
        double sw_y = 0; for (int64_t i : keep) sw_y += (w ? w[i] : 1.0) * col[j][i];
        double mu = W > 0 ? sw_y / W : std::numeric_limits<double>::quiet_NaN();
        double V;
        if (svy) {
            // Linearized variance of the ratio estimator: u_i = w_i (y_i − µ̂)/Ŵ,
            // then the same stratified PSU-total meat every svy estimator uses
            // (p = 1 here, and there is no bread because dµ/dµ = 1).
            std::vector<double> u(static_cast<size_t>(n));
            for (int64_t r = 0; r < n; ++r) {
                int64_t i = keep[static_cast<size_t>(r)];
                u[static_cast<size_t>(r)] = (w ? w[i] : 1.0) * (col[j][i] - mu) / W;
            }
            cecon::SvyMeatResult sm =
                cecon::svy_linearized_meat(u.data(), n, 1, f_psu, f_strata, nullptr);
            if (!sm.singleton_stratum.empty())
                throw EngineError("bad_input",
                    "mean: svy stratum variable '" + sm.singleton_stratum +
                        "' has a stratum with a single sampling unit; the linearized "
                        "variance is undefined there (Stata reports missing)");
            if (!sm.ok)
                throw EngineError("bad_input",
                    "mean: svy sampling units are not nested within strata");
            V = sm.meat[0];
        } else if (analytic_w) {
            double ss = 0;
            for (int64_t i : keep) {
                const double v = w[i] * static_cast<double>(n) / W;
                ss += v * (col[j][i] - mu) * (col[j][i] - mu);
            }
            V = (n > 1 && W > 0) ? ss / static_cast<double>(n - 1) / static_cast<double>(n)
                                 : std::numeric_limits<double>::quiet_NaN();
        } else if (freq_w) {
            double ss = 0;
            for (int64_t i : keep) ss += w[i] * (col[j][i] - mu) * (col[j][i] - mu);
            V = (W > 1) ? ss / (W - 1.0) / W : std::numeric_limits<double>::quiet_NaN();
        } else {
            double ss = 0;
            for (int64_t i : keep) { double t = (w ? w[i] : 1.0) * (col[j][i] - mu); ss += t * t; }
            V = (n > 1 && W > 0) ? (static_cast<double>(n) / (n - 1)) * ss / (W * W)
                                 : std::numeric_limits<double>::quiet_NaN();
        }
        double se = std::sqrt(V);
        double t = se > 0 ? mu / se : std::numeric_limits<double>::quiet_NaN();
        double p = std::isnan(t) ? std::numeric_limits<double>::quiet_NaN() : t_twosided_pvalue(t, dfree);
        double ci_lo = mu - tc * se, ci_hi = mu + tc * se;
        coefs.push_back({{"name", vars[j]}, {"b", nz(mu)}, {"se", nz(se)}, {"t", nz(t)}, {"p", nz(p)},
                         {"ci_lo", nz(ci_lo)}, {"ci_hi", nz(ci_hi)}});
        coef[vars[j]] = nz(mu); se_map[vars[j]] = nz(se);
        t_map[vars[j]] = nz(t); p_map[vars[j]] = nz(p);
        lo_map[vars[j]] = nz(ci_lo); hi_map[vars[j]] = nz(ci_hi);
    }
    // e(N): the row count, or the weight sum under fw/iw.
    json out = {{"estimator", "mean"}, {"n_obs", freq_w ? json(W) : json(n)}, {"sum_w", W}, {"level", level},
                {"coefs", coefs}, {"df", dfree}, {"coef", coef}, {"se", se_map},
                {"tstat", t_map}, {"pvalue", p_map}, {"ci_lo", lo_map}, {"ci_hi", hi_map}};
    if (w)  // normalize the long spellings ("pweight" ...) to the two-letter ones
        out["weight"] = {{"var", weight},
                         {"type", analytic_w ? "aw" : freq_fw ? "fw" : freq_w ? "iw" : "pw"}};
    // e(V): each variance above with its cross-variable terms, Sum a_i z_ij z_ik with z
    // the centered values (StataBE 19.5, scratch-laneA2/agents/epost/s11.log, `mean x x2`
    // V[2,1] .013257575757575754; [aw=] -.087530428943171312, [fw=] -.015517241379310339,
    // [iw=] -.035012171577268526, [pw=] -.11170982699362622). The svy design's is not
    // built here.
    if (!svy && n > 1 && W > 0 && (!freq_w || W > 1)) {
        std::vector<std::vector<double>> z(vars.size(),
                                           std::vector<double>(static_cast<size_t>(n)));
        std::vector<double> a(static_cast<size_t>(n));
        for (size_t j = 0; j < vars.size(); ++j) {
            double sw_y = 0;
            for (int64_t i : keep) sw_y += (w ? w[i] : 1.0) * col[j][i];
            const double mu = sw_y / W;
            for (int64_t r = 0; r < n; ++r)
                z[j][static_cast<size_t>(r)] = col[j][keep[static_cast<size_t>(r)]] - mu;
        }
        for (int64_t r = 0; r < n; ++r) {
            const double wi = w ? w[keep[static_cast<size_t>(r)]] : 1.0;
            a[static_cast<size_t>(r)] = analytic_w ? wi * static_cast<double>(n) / W
                                        : freq_w   ? wi
                                                   : wi * wi;
        }
        const double nn = static_cast<double>(n);
        const double scale = analytic_w ? 1.0 / (nn - 1.0) / nn
                             : freq_w   ? 1.0 / (W - 1.0) / W
                                        : nn / (nn - 1.0) / (W * W);
        add_full_e_v(out, vars, z, a, scale);
    }
    if (svy)
        out["svy"] = {{"strata_var", strata_var}, {"psu_var", psu_var},
                      {"n_strata", f_strata.n_levels}, {"n_psu", f_psu.n_levels},
                      {"design_df", design_df}, {"pop_size", W}};
    emit(out, opt(a, "--out"));
    return 0;
}

// ── proportion ───────────────────────────────────────────────────────────────
int cmd_proportion(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data");
    std::vector<std::string> vars = split_csv(opt(a, "--vars"));
    if (data_missing(data) || vars.empty()) {
        std::fprintf(stderr, "usage: cobra proportion --data f.hbin --vars V[,V2,...] "
                             "[--weight W --wtype pw|fw|iw] [--level P] [--keepif EXPR] "
                             "[--out f.json]\n"
                             "  --weight W   Stata [pw=W] (default), [fw=W] or [iw=W]\n"
                             "  --level P    confidence level for ci_lo/ci_hi, 10 < P <= 99.99 (default 95);\n"
                             "               CI uses Stata's default logit transform.\n");
        return 2;
    }
    std::string weight = opt(a, "--weight");
    check_wtype(a, "proportion", weight, /*allow_aw=*/false, /*allow_fw_iw=*/true);
    // `proportion [fw=]` / `[iw=]` (BUG-1541): the replicated sample of W = Sum w
    // observations, as `mean`'s (e(N) = W, e(df_r) = W - 1), with Stata's analytic
    // variance of a proportion, Sum w(ind - p)^2/W^2 = p(1 - p)/W -- no small-sample
    // factor (_svy_summarize.ado's AdjV4prop scales the SRS variance by (n-1)/W)
    // (StataBE 19.5, scratch-laneA/agents/file-new-2/m1.log: [fw=wi] e(N) 30, e(df_r)
    // 29, V[1,1] .0074074074074074051, V[1,2] -.0074074074074074051; [iw=wf] e(N)
    // 28.5, e(df_r) 27.5, V[1,1] .0077972709551656898).
    const std::string wtype_in = opt(a, "--wtype", "pw");
    const bool freq_fw = !weight.empty() && (wtype_in == "fw" || wtype_in == "fweight");
    const bool freq_w = freq_fw || (!weight.empty() && (wtype_in == "iw" || wtype_in == "iweight"));
    // An unweighted proportion takes that same analytic variance with w = 1, p(1 - p)/n
    // (AdjV4prop applies to it too): StataBE 19.5 (scratch-laneA2/agents/wml/x2.log)
    // `proportion d` on 12 rows, p = 2/3: V[1,1] .018518518518518514 = (2/9)/12,
    // _se[1.d] .13608276348795431, where the linearized n/(n-1) form gave (2/9)/11.
    // Only a pweight keeps the linearized variance.
    const bool analytic = freq_w || weight.empty();
    bool level_set = has(a, "--level");
    double level = std::stod(opt(a, "--level", "95"));
    if (level_set && (level < 10.0 || level > 99.99)) {
        std::fprintf(stderr, "proportion: --level must be in [10, 99.99] (got %s).\n",
                     opt(a, "--level").c_str());
        return 2;
    }
    data = maybe_keepif(data, opt(a, "--keepif"));
    const Dataset& ds = acquire_data(data);
    const int64_t N = ds.n_rows();
    std::vector<const double*> col(vars.size());
    for (size_t j = 0; j < vars.size(); ++j) col[j] = require_num(ds, "proportion", vars[j]);
    const double* w = weight.empty() ? nullptr : require_num(ds, "proportion", weight);

    std::vector<int64_t> keep = casewise_keep(col, w, N);
    const int64_t n = static_cast<int64_t>(keep.size());
    double W = 0; for (int64_t i : keep) W += w ? w[i] : 1.0;
    if (freq_fw)
        for (int64_t i : keep)
            if (w[i] != std::floor(w[i]))
                throw EngineError("bad_input",
                                  "proportion: fweight '" + weight + "' has a non-integer value");
    const double dfree = freq_w ? W - 1.0 : static_cast<double>(n - 1);
    const double alpha = 1.0 - level / 100.0;
    const double tc = t_critical(alpha, dfree);

    // coef/se are not enough. `--tidy`'s coeftable builder (tidy_coeftable) takes the
    // coef{} branch whenever coef{} exists, and reads t/p/CI from the SIBLING maps
    // tstat/pvalue/ci_lo/ci_hi — never from coefs[]. Emitting coef/se alone therefore
    // renders a table with the estimate and SE filled in and t, p and the confidence
    // interval blank, which for `mean`/`proportion` is exactly the column Stata puts
    // the CI in. Every map below is filled from the SAME per-row values as coefs[], so
    // the two representations cannot drift apart.
    json coef = json::object(), se_map = json::object(), coefs = json::array();
    json t_map = json::object(), p_map = json::object(),
         lo_map = json::object(), hi_map = json::object();
    // every level's centered indicator over the sample, for e(V) below
    std::vector<std::string> v_names;
    std::vector<std::vector<double>> v_z;
    for (size_t j = 0; j < vars.size(); ++j) {
        std::vector<double> levels; for (int64_t i : keep) levels.push_back(col[j][i]);
        std::sort(levels.begin(), levels.end());
        levels.erase(std::unique(levels.begin(), levels.end()), levels.end());
        for (double L : levels) {
            double sw_ind = 0;
            for (int64_t i : keep) if (col[j][i] == L) sw_ind += w ? w[i] : 1.0;
            double p = W > 0 ? sw_ind / W : std::numeric_limits<double>::quiet_NaN();
            v_names.push_back(vars[j] + ":" + level_name(L));
            v_z.emplace_back(static_cast<size_t>(n));
            for (int64_t r = 0; r < n; ++r) {
                const int64_t i = keep[static_cast<size_t>(r)];
                v_z.back()[static_cast<size_t>(r)] = (col[j][i] == L ? 1.0 : 0.0) - p;
            }
            double ss = 0;
            for (int64_t i : keep) {
                double ind = (col[j][i] == L) ? 1.0 : 0.0;
                if (analytic) {
                    ss += (w ? w[i] : 1.0) * (ind - p) * (ind - p);
                    continue;
                }
                double t = (w ? w[i] : 1.0) * (ind - p);
                ss += t * t;
            }
            double V = analytic ? ((W > 0 && (w || n > 1)) ? ss / (W * W)
                                                           : std::numeric_limits<double>::quiet_NaN())
                     : (n > 1 && W > 0) ? (static_cast<double>(n) / (n - 1)) * ss / (W * W)
                                        : std::numeric_limits<double>::quiet_NaN();
            double se = std::sqrt(V);
            // Stata's default CI for proportion is the logit transform; guard the
            // degenerate p==0/p==1 boundary (ln(p/(1-p)) blows up) by collapsing the
            // CI onto the point estimate instead of emitting NaN/inf. se is already
            // exactly 0 in the degenerate case (every indicator deviation is 0).
            double ci_lo, ci_hi;
            if (p <= 0.0 || p >= 1.0 || se == 0.0) {
                ci_lo = p; ci_hi = p;
            } else {
                double g = std::log(p / (1.0 - p));
                double h = tc * se / (p * (1.0 - p));
                ci_lo = 1.0 / (1.0 + std::exp(-(g - h)));
                ci_hi = 1.0 / (1.0 + std::exp(-(g + h)));
            }
            double t = se > 0 ? p / se : std::numeric_limits<double>::quiet_NaN();
            double pval = std::isnan(t) ? std::numeric_limits<double>::quiet_NaN() : t_twosided_pvalue(t, dfree);
            std::string name = vars[j] + ":" + level_name(L);
            coefs.push_back({{"name", name}, {"b", nz(p)}, {"se", nz(se)}, {"t", nz(t)}, {"p", nz(pval)},
                             {"ci_lo", nz(ci_lo)}, {"ci_hi", nz(ci_hi)}});
            coef[name] = nz(p); se_map[name] = nz(se);
            t_map[name] = nz(t); p_map[name] = nz(pval);
            lo_map[name] = nz(ci_lo); hi_map[name] = nz(ci_hi);
        }
    }
    // e(N): the row count, or the weight sum under fw/iw.
    json out = {{"estimator", "proportion"}, {"n_obs", freq_w ? json(W) : json(n)}, {"sum_w", W},
                {"level", level}, {"coefs", coefs}, {"df", dfree}, {"coef", coef}, {"se", se_map},
                {"tstat", t_map}, {"pvalue", p_map}, {"ci_lo", lo_map}, {"ci_hi", hi_map}};
    if (w)  // normalize the long spellings ("pweight" ...) to the two-letter ones
        out["weight"] = {{"var", weight}, {"type", freq_fw ? "fw" : freq_w ? "iw" : "pw"}};
    // e(V): each level's variance above with the cross terms between levels and between
    // variables, Sum a_i z_ia z_ib over the centered indicators (StataBE 19.5,
    // scratch-laneA2/agents/epost/s11.log, `proportion d d3` V[3,1] .009259259259259257,
    // V[5,1] -.018518518518518514; [fw=] .0037037037037037025, [iw=] .0026674874320303673,
    // [pw=] .0065356878303371975).
    if (W > 0 && (analytic ? (w != nullptr || n > 1) : n > 1)) {
        std::vector<double> av(static_cast<size_t>(n));
        for (int64_t r = 0; r < n; ++r) {
            const double wi = w ? w[keep[static_cast<size_t>(r)]] : 1.0;
            av[static_cast<size_t>(r)] = analytic ? wi : wi * wi;
        }
        const double nn = static_cast<double>(n);
        const double scale = analytic ? 1.0 / (W * W) : nn / (nn - 1.0) / (W * W);
        add_full_e_v(out, v_names, v_z, av, scale);
    }
    emit(out, opt(a, "--out"));
    return 0;
}

}  // namespace cli
}  // namespace cecon
