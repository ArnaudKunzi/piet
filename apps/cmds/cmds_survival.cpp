// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "common.h"
#include "commands.h"
#include "core/distributions.h"
#include "estimators/survival.h"

// P7 wave — stset, stcox, streg.
//
// `stset` is the data verb: it MATERIALIZES `_t0`, `_t`, `_d`, `_st` on the
// dataset (commit_frame, like every other mutator) and prints Stata's
// "Survival-time data settings" report as JSON on stdout. `stcox`/`streg` then
// read those four as ordinary columns — no session state, no hidden coupling.
//
// Oracle for every number: StataBE 19.5, internal capture
// p7_stata.log (the `preserve / sort id exit / stset exit,
// failure(fail) id(id)` block of p7.do). Assertions in tests/unit/test_survival.cpp.

namespace cecon {
namespace cli {

namespace {

// The four columns `stset` writes and every st command reads.
constexpr const char* kT0 = "_t0";
constexpr const char* kT = "_t";
constexpr const char* kD = "_d";
constexpr const char* kSt = "_st";

void require_st(const Dataset& ds, const char* cmd) {
    if (ds.has(kT0) && ds.has(kT) && ds.has(kD) && ds.has(kSt)) return;
    throw EngineError("bad_input",
                      std::string(cmd) + ": the data are not survival-time data — run `cobra stset` "
                                         "first (columns _t0/_t/_d/_st are missing)");
}

// The estimation sample shared by stcox and streg: load_glm does the factor
// expansion + listwise deletion + cluster column in ONE pass (exactly as every
// other MLE command does), and then the `_st == 1` restriction is applied on
// top. `_t` is passed as load_glm's y purely so the loader's listwise pass sees
// it; the analysis-time columns are re-read below at the kept row indices.
//
// Verified on the P7 oracle: `_st == 1` (1,207 records) intersected with
// listwise-complete x1 x2 leaves 1,173 records / 199 subjects / 194 failures /
// 263.5 time at risk — one subject's ENTIRE kept record set has x2 missing,
// which is why the subject count drops 200 -> 199.
struct StSample {
    int64_t n = 0;                // rows in the estimation sample
    int64_t n_st = 0;             // rows with _st == 1 before listwise deletion
    int k = 0;                    // design columns (intercept LAST, as load_glm builds it)
    std::vector<double> X;        // col-major n*k
    std::vector<double> t0, t, d;
    std::vector<std::string> names;
    std::vector<int32_t> cl_codes;  // dense 0..n_clusters-1, empty when no --cluster
    int n_clusters = 0;
    double time_at_risk = 0;
    int64_t n_fail = 0;
    bool multi_record = false;  // any row enters after t = 0 (delayed entry)
};

StSample load_st(const Dataset& ds, const char* cmd, const std::vector<std::string>& xs,
                 const std::string& cluster_var) {
    require_st(ds, cmd);
    std::vector<std::string> cl;
    if (!cluster_var.empty()) cl.push_back(cluster_var);
    GlmInputs g = load_glm(ds, cmd, kT, xs, cl);
    const double* raw_t0 = require_num(ds, cmd, kT0);
    const double* raw_t = require_num(ds, cmd, kT);
    const double* raw_d = require_num(ds, cmd, kD);
    const double* raw_st = require_num(ds, cmd, kSt);

    StSample s;
    s.k = g.k;
    s.names = g.names;
    for (int64_t i = 0; i < ds.n_rows(); ++i)
        if (raw_st[i] == 1.0) ++s.n_st;

    std::vector<int64_t> sel;
    sel.reserve(static_cast<size_t>(g.n));
    for (int64_t i = 0; i < g.n; ++i)
        if (raw_st[g.keep[static_cast<size_t>(i)]] == 1.0) sel.push_back(i);
    s.n = static_cast<int64_t>(sel.size());
    if (s.n == 0) return s;

    s.X.resize(static_cast<size_t>(s.n) * static_cast<size_t>(s.k));
    for (int c = 0; c < s.k; ++c)
        for (int64_t q = 0; q < s.n; ++q)
            s.X[static_cast<int64_t>(c) * s.n + q] = g.X[static_cast<int64_t>(c) * g.n + sel[static_cast<size_t>(q)]];
    s.t0.resize(static_cast<size_t>(s.n));
    s.t.resize(static_cast<size_t>(s.n));
    s.d.resize(static_cast<size_t>(s.n));
    for (int64_t q = 0; q < s.n; ++q) {
        const int64_t row = g.keep[static_cast<size_t>(sel[static_cast<size_t>(q)])];
        s.t0[static_cast<size_t>(q)] = raw_t0[row];
        s.t[static_cast<size_t>(q)] = raw_t[row];
        s.d[static_cast<size_t>(q)] = raw_d[row] != 0.0 ? 1.0 : 0.0;
        s.time_at_risk += raw_t[row] - raw_t0[row];
        if (s.d[static_cast<size_t>(q)] != 0.0) ++s.n_fail;
        if (raw_t0[row] > 0.0) s.multi_record = true;
    }
    if (!g.cluster.empty()) {
        // load_glm's cluster codes are dense over ITS kept rows; the `_st` filter
        // can leave a level empty, so re-densify over the rows that survived.
        const auto& F = g.cluster[0];
        std::vector<int32_t> remap(static_cast<size_t>(F.n_levels), -1);
        s.cl_codes.resize(static_cast<size_t>(s.n));
        for (int64_t q = 0; q < s.n; ++q) {
            const int32_t old = F.codes[static_cast<size_t>(sel[static_cast<size_t>(q)])];
            if (old < 0 || old >= F.n_levels)
                throw EngineError("bad_input", std::string(cmd) + ": cluster variable has a missing value");
            if (remap[static_cast<size_t>(old)] < 0) remap[static_cast<size_t>(old)] = s.n_clusters++;
            s.cl_codes[static_cast<size_t>(q)] = remap[static_cast<size_t>(old)];
        }
    }
    return s;
}

}  // namespace

// cobra stset --data f.hbin --time T [--failure F] [--id ID] [--keepif E] --out new.hbin
int cmd_stset(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), tvar = opt(a, "--time"), fvar = opt(a, "--failure"),
                idvar = opt(a, "--id"), out = opt(a, "--out");
    if (data_missing(data) || tvar.empty() || out_missing(out)) {
        std::fprintf(stderr,
                     "usage: cobra stset --data f.hbin --time T [--failure F] [--id ID] "
                     "[--keepif EXPR] --out new.hbin\n"
                     "  Declares survival-time data, Stata `stset T, failure(F) id(ID)`, by writing\n"
                     "  four real columns onto the dataset: _t0 (entry), _t (exit), _d (failure),\n"
                     "  _st (1 = in the analysis sample). With --id the records of one subject form\n"
                     "  the intervals (_t[_n-1], _t]; every record at or after the subject's FIRST\n"
                     "  failure is excluded (_st = 0) but KEPT in the data, exactly as Stata does.\n"
                     "  Without --failure the last record of each subject is the failure.\n"
                     "  The settings report is written to stdout as JSON; --out is the .hbin target.\n");
        return 2;
    }
    data = maybe_keepif(data, opt(a, "--keepif"));
    const Dataset& ds = acquire_data(data);
    const double* tp = require_num(ds, "stset", tvar);
    const double* fp = fvar.empty() ? nullptr : require_num(ds, "stset", fvar);
    const double* ip = idvar.empty() ? nullptr : require_num(ds, "stset", idvar);

    StsetResult r = stset_build(ip, tp, fp, ds.n_rows());
    if (!r.ok) throw EngineError("bad_input", "stset: " + r.err);

    Frame fr = frame_from_acquired(ds);
    fr.set_f64_column(kT0, r.t0);
    fr.set_f64_column(kT, r.t);
    fr.set_f64_column(kD, r.d);
    fr.set_f64_column(kSt, r.st);
    if (!commit_frame(std::move(fr), out))
        std::fprintf(stderr, "stset: wrote %s (%lld of %lld records in the analysis sample)\n",
                     out.c_str(), static_cast<long long>(r.n_kept),
                     static_cast<long long>(r.n_total));

    json report = {
        {"command", "stset"},
        {"time_var", tvar},
        {"id_var", idvar},
        {"failure_var", fvar},
        {"failure_event", fvar.empty() ? std::string("(last record per subject)")
                                       : fvar + "!=0 & " + fvar + "<."},
        {"time_interval", idvar.empty() ? std::string("(0, ") + tvar + "]"
                                        : std::string("(") + tvar + "[_n-1], " + tvar + "]"},
        {"exit_on_or_before", "failure"},
        {"n_total", r.n_total},
        {"n_excluded_missing", r.n_excl_missing},
        {"n_excluded_after_failure", r.n_excl_after_failure},
        {"n_excluded_negative_time", r.n_excl_negative},
        {"n_excluded_zero_length", r.n_excl_zero_length},
        {"n_remaining", r.n_kept},
        {"n_subjects", r.n_subjects},
        {"n_failures", r.n_failures},
        {"single_failure_per_subject", r.single_failure},
        {"time_at_risk", r.time_at_risk},
        {"at_risk_from", r.origin},
        {"earliest_entry", r.first_entry},
        {"last_exit", r.last_exit}};
    emit(report, "");  // stdout, regardless of --out (which is the .hbin target here)
    return 0;
}

namespace {

// Shared coefficient-table rendering for stcox/streg.
//
// `b`/`se`/`ci_lo`/`ci_hi` stay in the RAW estimation metric, exactly like every
// other cobra estimator (so the shared tidy coeftable is internally consistent);
// the HAZARD-RATIO metric Stata prints by default rides alongside as `hr` /
// `se_hr` / `hr_ci_lo` / `hr_ci_hi`.
//
// The three rules the oracle pins:
//   se_HR = |HR| * se_b        (delta method on exp)
//   z, P>|z| come from the RAW b metric, NOT from HR/se_HR
//   the HR interval is exp(b +/- z* se_b), NOT HR +/- z* se_HR
// Cross-check: stcox x1 prints HR .9390707, std. err. .0550338, z -1.07. Then
// b = ln(.9390707) = -.0628617 and se_b = .0550338/.9390707 = .0586045, so
// -.0628617/.0586045 = -1.0726 -> the printed -1.07. (HR/se_HR would give
// -17.06, and HR - 1.96*se_HR would give .8312 against the printed .8371707.)
json coef_row(const std::string& name, double b, double se, double zc) {
    const double z = tstat(b, se);
    const double pv = (se > 0) ? z_twosided_pvalue(z) : 1.0;
    const double hr = std::exp(b);
    return json{{"name", name},
                {"b", b},
                {"se", se},
                {"z", z},
                {"p", pv},
                {"ci_lo", b - zc * se},
                {"ci_hi", b + zc * se},
                {"hr", hr},
                {"se_hr", hr * se},
                {"hr_ci_lo", std::exp(b - zc * se)},
                {"hr_ci_hi", std::exp(b + zc * se)}};
}

// The Wald chi2 Stata prints in the header, over `idx` of the k coefficients.
void add_wald(json& out, const std::vector<double>& beta, const std::vector<double>& V, int np,
              const std::vector<int>& idx) {
    const int j = static_cast<int>(idx.size());
    if (j == 0) return;
    std::vector<double> R(static_cast<size_t>(j) * static_cast<size_t>(np), 0.0), q(static_cast<size_t>(j), 0.0);
    for (int i = 0; i < j; ++i) R[static_cast<size_t>(i) * static_cast<size_t>(np) + static_cast<size_t>(idx[static_cast<size_t>(i)])] = 1.0;
    std::vector<double> b(static_cast<size_t>(np), 0.0);
    for (int i = 0; i < np && i < static_cast<int>(beta.size()); ++i) b[static_cast<size_t>(i)] = beta[static_cast<size_t>(i)];
    WaldResult w = wald_test(b.data(), V.data(), np, R.data(), q.data(), j);
    if (!w.ok) return;
    out["chi2"] = w.chi2;
    out["chi2_df"] = j;
    out["p_chi2"] = chi2_pvalue(w.chi2, static_cast<double>(j));
}

std::string parse_vce(const std::vector<std::string>& a, const char* cmd,
                      const std::string& cluster_var) {
    std::string vce = opt(a, "--vce", "iid");
    if (vce != "iid" && vce != "robust" && vce != "cluster")
        throw EngineError("bad_input",
                          std::string(cmd) + ": --vce must be one of iid|robust|cluster (got " + vce + ")");
    if (vce == "cluster" && cluster_var.empty())
        throw EngineError("bad_input", std::string(cmd) + ": --vce cluster requires --cluster C");
    return vce;
}

}  // namespace

// cobra stcox --data f.hbin --x X1,X2 [--vce iid|robust|cluster] [--cluster C] [--level P] ...
int cmd_stcox(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), xcsv = opt(a, "--x"), cluster_var = opt(a, "--cluster");
    if (data_missing(data) || xcsv.empty()) {
        std::fprintf(stderr,
                     "usage: cobra stcox --data f.hbin --x X1,X2 [--vce iid|robust|cluster] "
                     "[--cluster C] [--level P] [--keepif EXPR] [--out f.json]\n"
                     "  Cox proportional hazards on stset data (needs _t0/_t/_d/_st): partial\n"
                     "  likelihood with delayed entry, risk set { i : _t0_i < t <= _t_i }, Breslow\n"
                     "  ties. Coefficients are reported as hazard ratios exp(b); z and P>|z| are\n"
                     "  computed in the raw b metric.\n"
                     "  --vce cluster --cluster ID   Lin-Wei robust: efficient score residuals\n"
                     "                               summed within cluster (Stata vce(cluster ID)).\n");
        return 2;
    }
    const std::string vce = parse_vce(a, "stcox", cluster_var);
    const double level = std::stod(opt(a, "--level", "95"));
    if (level < 10.0 || level > 99.99)
        throw EngineError("bad_input", "stcox: --level must be in [10, 99.99]");
    const double zc = z_critical(level == 95.0 ? 0.05 : 1.0 - level / 100.0);

    data = maybe_keepif(data, opt(a, "--keepif"));
    const Dataset& ds = acquire_data(data);
    StSample s = load_st(ds, "stcox", split_csv(xcsv), cluster_var);
    if (s.n == 0) { std::fprintf(stderr, "stcox: no rows in the analysis sample.\n"); return 1; }
    // The partial likelihood conditions the intercept away, so the Cox design is
    // load_glm's WITHOUT its trailing intercept column.
    const int k = s.k - 1;
    if (k <= 0) throw EngineError("bad_input", "stcox: at least one covariate is required");

    CoxResult r = cox_fit(s.X.data(), s.n, k, s.t0.data(), s.t.data(), s.d.data());
    if (!r.ok) throw EngineError("bad_input", r.err.empty() ? "stcox: fit failed" : r.err);

    std::vector<double> V = r.vcov;
    int n_clusters = 0;
    if (vce == "robust" || vce == "cluster") {
        std::vector<double> S;
        cox_score_residuals(s.X.data(), s.n, k, s.t0.data(), s.t.data(), s.d.data(), r.beta, &S);
        const bool clustered = (vce == "cluster");
        if (clustered) {
            n_clusters = s.n_clusters;
            if (n_clusters < 2)
                throw EngineError("bad_input",
                                  "stcox: vce(cluster) requires at least 2 clusters, but the cluster "
                                  "variable has " + std::to_string(n_clusters));
        }
        // Small-sample multiplier. cluster: q_c = G/(G-1) — the same ML cluster
        // factor logit/tobit use, NOT the regress-style CGM product. VERIFIED,
        // not assumed: with G = 199 the two candidates differ by 0.25% in the SE,
        // and G/(G-1) reproduces the oracle's Haz. ratio std. err. .0550338 /
        // .1019729 exactly (q_c = 1 would give .0548954 / .1017164). robust:
        // N/(N-1), the G=N limit of the same rule — no P7 oracle pins that arm,
        // so it is the house convention rather than a measured fact.
        const double factor =
            clustered ? static_cast<double>(n_clusters) / static_cast<double>(n_clusters - 1)
                      : (s.n > 1 ? static_cast<double>(s.n) / static_cast<double>(s.n - 1) : 1.0);
        V = survival_sandwich(S, s.n, k, r.vcov, clustered ? s.cl_codes.data() : nullptr,
                              clustered ? n_clusters : 0, factor);
        if (V.empty()) throw EngineError("bad_input", "stcox: robust VCE failed (non-invertible sandwich)");
    }

    json coefs = json::array();
    std::vector<double> se(static_cast<size_t>(k));
    for (int c = 0; c < k; ++c) {
        se[static_cast<size_t>(c)] =
            std::sqrt(std::max(0.0, V[static_cast<size_t>(c) * static_cast<size_t>(k) + static_cast<size_t>(c)]));
        coefs.push_back(coef_row(s.names[static_cast<size_t>(c)], r.beta[static_cast<size_t>(c)],
                                 se[static_cast<size_t>(c)], zc));
    }
    json out = {{"estimator", "stcox"},
                {"vce", vce},
                // Breslow == every exact method while each tie set is a singleton,
                // which is what Stata's "Cox regression with no ties" header asserts.
                // Reported from a real tie SCAN so the header can never over-claim.
                {"ties", r.n_tied_failures == 0 ? "none" : "breslow"},
                {"n_obs", s.n},
                {"n_dropped_missing", s.n_st - s.n},
                {"n_failures", s.n_fail},
                {"time_at_risk", s.time_at_risk},
                {"ll", r.ll},
                {"ll_null", r.ll_null},
                {"converged", r.converged},
                {"iters", r.iters},
                {"level", level}};
    if (!cluster_var.empty()) out["n_subjects"] = s.n_clusters;
    std::vector<int> idx(static_cast<size_t>(k));
    for (int c = 0; c < k; ++c) idx[static_cast<size_t>(c)] = c;
    add_wald(out, r.beta, V, k, idx);
    // BUG-383 #6 (off-pin sweep vs live Stata, 2026-09-01): with the default
    // (non-robust) VCE, Stata's stcox header and e(chi2) are the LIKELIHOOD-
    // RATIO statistic, not the Wald — verified against the capture to 1e-10:
    // e(chi2) = 10.4561647774 = 2*(e(ll) - e(ll_0)) exactly, where our Wald
    // said 12.4253. Under vce(robust)/vce(cluster) Stata itself switches the
    // header to Wald (a robust LR is not defined), which is what add_wald
    // already produced — so the override applies to the iid arm only. Both ll
    // terms were already computed and published above; this is a publication
    // fix, not an estimator change.
    if (vce == "iid" && r.converged && k > 0) {
        const double lr = 2.0 * (r.ll - r.ll_null);
        out["chi2"] = lr;
        out["chi2_df"] = k;
        out["p_chi2"] = chi2_pvalue(lr, static_cast<double>(k));
        out["chi2type"] = "LR";
    }
    if (vce == "cluster") { out["cluster_var"] = cluster_var; out["n_clusters"] = n_clusters; }
    out["coefs"] = coefs;
    emit(out, opt(a, "--out"));
    return 0;
}

// cobra streg --data f.hbin --x X1,X2 --dist weibull [--vce iid|robust|cluster] [--cluster C] ...
int cmd_streg(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), xcsv = opt(a, "--x"), cluster_var = opt(a, "--cluster"),
                dist = opt(a, "--dist", "weibull");
    if (data_missing(data) || xcsv.empty()) {
        std::fprintf(stderr,
                     "usage: cobra streg --data f.hbin --x X1,X2 --dist weibull "
                     "[--vce iid|robust|cluster] [--cluster C] [--level P] [--keepif EXPR] [--out f.json]\n"
                     "  Weibull PH regression on stset data (needs _t0/_t/_d/_st), fitted in\n"
                     "  (beta, ln p) with the delayed-entry correction -(t^p - t0^p)exp(xb).\n"
                     "  Reports hazard ratios exp(b) (_cons estimates the baseline hazard), plus\n"
                     "  /ln_p, p and 1/p with delta-method standard errors.\n"
                     "  --vce robust on MULTI-RECORD st data requires --cluster ID: Stata clusters\n"
                     "  vce(robust) on the stset id() SUBJECT, not on the record.\n");
        return 2;
    }
    if (dist != "weibull")
        throw EngineError("bad_input",
                          "streg: --dist " + dist + " is not implemented in this build (only weibull)");
    const std::string vce = parse_vce(a, "streg", cluster_var);
    const double level = std::stod(opt(a, "--level", "95"));
    if (level < 10.0 || level > 99.99)
        throw EngineError("bad_input", "streg: --level must be in [10, 99.99]");
    const double zc = z_critical(level == 95.0 ? 0.05 : 1.0 - level / 100.0);

    data = maybe_keepif(data, opt(a, "--keepif"));
    const Dataset& ds = acquire_data(data);
    StSample s = load_st(ds, "streg", split_csv(xcsv), cluster_var);
    if (s.n == 0) { std::fprintf(stderr, "streg: no rows in the analysis sample.\n"); return 1; }
    const int k = s.k;              // intercept INCLUDED (it is the baseline hazard)
    const int cons_col = s.k - 1;   // load_glm puts it last
    const int np = k + 1;           // + ln p

    // Stata's vce(robust) on st data with id() is SUBJECT-clustered, not
    // record-clustered — 199 independent subjects here, not 1,173 independent
    // records. VERIFIED against the oracle: subject-clustered with q_c = G/(G-1)
    // gives /ln_p se .0423804 (oracle .0423804) and Haz. ratio std. errs
    // .0596222 / .1013885 / .0050684, all matching; the record-level sandwich
    // gives .0407665 for /ln_p, which misses on the second digit. Rather than
    // silently produce that wrong number, refuse: on multi-record data the caller
    // must name the subject id.
    const bool cluster_on_id = (vce == "cluster") || (vce == "robust" && !cluster_var.empty());
    if (vce == "robust" && cluster_var.empty() && s.multi_record)
        throw EngineError("bad_input",
                          "streg: --vce robust on multi-record survival data requires --cluster ID "
                          "(Stata clusters vce(robust) on the stset id() subject); pass the id() "
                          "variable used by stset");

    WeibullResult r = weibull_fit(s.X.data(), s.n, k, s.t0.data(), s.t.data(), s.d.data(), cons_col);
    if (!r.ok) throw EngineError("bad_input", r.err.empty() ? "streg: MLE failed" : r.err);

    std::vector<double> V = r.vcov;
    int n_clusters = 0;
    if (vce == "robust" || vce == "cluster") {
        std::vector<double> theta = r.beta;
        theta.push_back(r.ln_p);
        std::vector<double> S;
        weib::ll_grad(s.X.data(), s.n, k, s.t0.data(), s.t.data(), s.d.data(), theta, nullptr, &S);
        if (cluster_on_id) {
            n_clusters = s.n_clusters;
            if (n_clusters < 2)
                throw EngineError("bad_input",
                                  "streg: clustering requires at least 2 clusters, but the cluster "
                                  "variable has " + std::to_string(n_clusters));
        }
        // Same verified factor as stcox: q_c = G/(G-1) when clustered (matches the
        // oracle to <1e-6 relative), N/(N-1) for the unclustered single-record arm.
        const double factor =
            cluster_on_id ? static_cast<double>(n_clusters) / static_cast<double>(n_clusters - 1)
                          : (s.n > 1 ? static_cast<double>(s.n) / static_cast<double>(s.n - 1) : 1.0);
        V = survival_sandwich(S, s.n, np, r.vcov, cluster_on_id ? s.cl_codes.data() : nullptr,
                              cluster_on_id ? n_clusters : 0, factor);
        if (V.empty()) throw EngineError("bad_input", "streg: robust VCE failed (non-invertible sandwich)");
    }

    json coefs = json::array();
    for (int c = 0; c < k; ++c) {
        const double se =
            std::sqrt(std::max(0.0, V[static_cast<size_t>(c) * static_cast<size_t>(np) + static_cast<size_t>(c)]));
        coefs.push_back(coef_row(s.names[static_cast<size_t>(c)], r.beta[static_cast<size_t>(c)], se, zc));
    }
    const double se_lnp =
        std::sqrt(std::max(0.0, V[static_cast<size_t>(k) * static_cast<size_t>(np) + static_cast<size_t>(k)]));
    const double p = std::exp(r.ln_p);

    json out = {{"estimator", "streg"},
                {"distribution", "weibull"},
                {"metric", "ph"},
                {"vce", vce},
                {"n_obs", s.n},
                {"n_dropped_missing", s.n_st - s.n},
                {"n_failures", s.n_fail},
                {"time_at_risk", s.time_at_risk},
                {"ll", r.ll},
                {"ll_null", r.ll_null},
                {"converged", r.converged},
                {"iters", r.iters},
                {"level", level},
                {"cons_note", "_cons estimates baseline hazard"}};
    if (!cluster_var.empty()) out["n_subjects"] = s.n_clusters;
    // Stata's header Wald tests the SLOPES only — _cons and /ln_p are excluded.
    // Verified: 1.02 on the oracle (p 0.5992); including _cons would not give it.
    std::vector<int> idx;
    for (int c = 0; c < k; ++c)
        if (c != cons_col) idx.push_back(c);
    {
        std::vector<double> theta = r.beta;
        theta.push_back(r.ln_p);
        add_wald(out, theta, V, np, idx);
    }
    if (cluster_on_id) { out["cluster_var"] = cluster_var; out["n_clusters"] = n_clusters; }
    out["coefs"] = coefs;
    // The ancillary parameter, in all three metrics Stata prints. p and 1/p are
    // delta-method transforms of ln_p, and their CIs are the ln_p CI mapped
    // through the same transform (so 1/p's bounds come out reversed), matching
    // Stata's [9.676766, 11.42559] for p and [.0875228, .1033403] for 1/p.
    out["ln_p"] = {{"b", r.ln_p},
                   {"se", se_lnp},
                   {"z", tstat(r.ln_p, se_lnp)},
                   {"ci_lo", r.ln_p - zc * se_lnp},
                   {"ci_hi", r.ln_p + zc * se_lnp}};
    out["p"] = {{"b", p},
                {"se", p * se_lnp},
                {"ci_lo", std::exp(r.ln_p - zc * se_lnp)},
                {"ci_hi", std::exp(r.ln_p + zc * se_lnp)}};
    out["inv_p"] = {{"b", 1.0 / p},
                    {"se", se_lnp / p},
                    {"ci_lo", std::exp(-(r.ln_p + zc * se_lnp))},
                    {"ci_hi", std::exp(-(r.ln_p - zc * se_lnp))}};
    emit(out, opt(a, "--out"));
    return 0;
}

}  // namespace cli
}  // namespace cecon
