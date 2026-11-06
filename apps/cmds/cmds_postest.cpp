// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "common.h"
#include "commands.h"

#include <cmath>
#include <limits>
#include <set>

#include "estimators/feols.h"
#include "estimators/ols.h"
#include "estimators/posthest.h"
#include "estimators/nlcom.h"
#include "estimators/postest_names.h"
#include "estimators/logit.h"
#include "estimators/probit.h"
#include "estimators/fepois.h"
#include "estimators/xtreg.h"
#include "estimators/iv2sls.h"
#include "vcov/vcov.h"  // hc_meat/cluster_meat/Factor — P5 margins pweight+cluster sandwich (mirrors cmd_logit)

namespace cecon {
namespace cli {

// cmds_mle.cpp: Stata's perfect-prediction screen on a loaded logit/probit design
// (the one cmd_logit / cmd_probit run before ml). On return g holds the rows and
// columns left and fit_cols the original index of each column left.
bool binary_screen_glm(GlmInputs& g, bool asis, bool has_constant, const char* cmd,
                       const std::string& yname, BinaryPerfectScreen& screen, std::vector<int>& fit_cols,
                       bool notes, const std::vector<BinaryBaseLevel>* bases);

// cmds_mle.cpp: the factor base levels of the varlist the screen tests too.
std::vector<BinaryBaseLevel> binary_base_levels(const Dataset& ds, const std::vector<std::string>& xs,
                                                const GlmInputs& g);

// cmds_mle.cpp: cmd_cloglog's ml on a screened design (its logit start, then clog_lf).
BinaryMlResult cloglog_ml(const GlmInputs& g, bool has_constant, double z0, const MlMaxOptions& mo);

// cmds_mle.cpp: cmd_logit / cmd_probit's maximize options (FU-138).
bool parse_ml_max_options(const std::vector<std::string>& a, const char* cmd, MlMaxOptions& mo);

// cmds_data.cpp: the --store-type token and the store-boundary rounding (BUG-471 T1,
// frame_detail::round_to_store) predict applies to the columns it writes (BUG-924).
StoreType parse_store_type(const std::vector<std::string>& a, const char* verb);
void round_column_to_store(double* v, size_t n, StoreType st);

namespace {

// posted-matrix-wald: `--posted FILE` -- the live e(b)/e(V) of a results set the
// caller posted itself and the engine cannot re-fit (csdid/drdid/estat postings,
// suest, acreg, qreg2, `ereturn post`): test/lincom/nlcom read them as Stata's
// test/lincom do (test.ado reads e(b)/e(V); a missing e(df_r) is the chi2 / normal
// reference, a present one F/t(e(df_r))). The file is
//   {"names": [...], "b": [...], "V": [[...], ...], "df_r": number | null, "n": number}
// with V row-major (symmetric). Returns false when no --posted flag was given.
bool load_posted_model(const std::vector<std::string>& a, PostestModel& M) {
    if (!has(a, "--posted")) return false;
    const std::string path = opt(a, "--posted");
    std::ifstream in(path);
    if (!in) throw EngineError("data_io", "cannot open --posted file " + path);
    std::stringstream ss;
    ss << in.rdbuf();
    json j = json::parse(ss.str(), nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object() || !j.contains("names") || !j["names"].is_array() ||
        !j.contains("b") || !j["b"].is_array() || !j.contains("V") || !j["V"].is_array())
        throw EngineError("bad_input", "--posted: expected {names, b, V}");
    const size_t k = j["names"].size();
    if (k == 0 || j["b"].size() != k || j["V"].size() != k)
        throw EngineError("bad_input", "--posted: names, b and V do not conform");
    auto num = [](const json& x) {
        return x.is_number() ? x.get<double>() : std::numeric_limits<double>::quiet_NaN();
    };
    for (const auto& n : j["names"]) M.names.push_back(n.is_string() ? n.get<std::string>() : "");
    for (const auto& x : j["b"]) M.beta.push_back(num(x));
    M.vcov_matrix.assign(k * k, 0.0);
    for (size_t r = 0; r < k; ++r) {
        const json& row = j["V"][r];
        if (!row.is_array() || row.size() != k)
            throw EngineError("bad_input", "--posted: V is not k x k");
        // column-major storage (PostestModel); V is symmetric, the transpose is exact
        for (size_t c = 0; c < k; ++c) M.vcov_matrix[c * k + r] = num(row[c]);
    }
    M.vcov_type = "posted";
    const bool hasDf = j.contains("df_r") && j["df_r"].is_number() && j["df_r"].get<double>() > 0;
    M.asymptotic = !hasDf;
    M.dof_resid = hasDf ? static_cast<int64_t>(j["df_r"].get<double>()) : 0;
    M.n = (j.contains("n") && j["n"].is_number()) ? static_cast<int64_t>(j["n"].get<double>()) : 0;
    return true;
}

// Map a --vcov string to the Vcov enum (shared by feols-path commands in this file).
Vcov parse_vcov(const std::string& s) {
    if (s == "hc1" || s == "HC1") return Vcov::HC1;
    if (s == "cluster") return Vcov::Cluster;
    if (s == "twoway" || s == "multiway") return Vcov::Multiway;
    return Vcov::IID;
}

// True iff a design-column name is an interaction (`a#b`) or a factor level
// (BUG-101 `level.var`, e.g. "1.treated") rather than a bare continuous main
// effect. Used ONLY to route `margins` (P5/BUG-158-159): a design with none of
// these AND no weights/cluster/vce takes the exact old per-column code path
// (margins_logit/probit/poisson), byte-identical to before this fix existed;
// anything else takes the new interaction/factor/weight-aware margins_ame.
bool margins_name_is_factor_or_interaction(const std::string& nm) {
    if (nm.find('#') != std::string::npos) return true;
    size_t dot = nm.find('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 >= nm.size()) return false;
    size_t i = (nm[0] == '-') ? 1 : 0;
    if (i >= dot) return false;
    for (size_t j = i; j < dot; ++j)
        if (!std::isdigit(static_cast<unsigned char>(nm[j]))) return false;
    return true;
}

// P7 (`margins, dydx() at()`): true when design column `nm` is DERIVED from base
// variable `var` without being the plain column `var` itself — a factor level
// ("2.treated") or any interaction part ("treated#x1", "1.treated#c.x1"). Used to
// refuse an --at override the already-expanded design cannot honour, instead of
// fixing the main effect and silently leaving the derived terms as observed.
// Segment-aware so `treated` never matches `untreated` or `treated_lag`.
bool margins_name_mentions_var(const std::string& nm, const std::string& var) {
    if (nm == var) return false;
    size_t pos = 0;
    while (pos < nm.size()) {
        size_t hash = nm.find('#', pos);
        std::string seg = nm.substr(pos, hash == std::string::npos ? std::string::npos : hash - pos);
        // strip a leading factor/continuous operator: "2.", "i.", "c.", "1b."
        size_t dot = seg.find('.');
        if (dot != std::string::npos) seg = seg.substr(dot + 1);
        if (seg == var) return true;
        if (hash == std::string::npos) break;
        pos = hash + 1;
    }
    return false;
}

// FU-342: the predictive --at branches override the ONE column named `var`; a
// column derived from it (`x1#x1` after `c.x1##c.x1`, `2.t#c.x1`) would keep its
// observed values, so the margin would be taken at a point the model cannot
// reach. Refused like the dydx()+at() branch's own check below.
void margins_refuse_derived_at(const std::vector<std::string>& names, const std::string& var) {
    for (const auto& nm : names) {
        if (nm != "(intercept)" && margins_name_mentions_var(nm, var))
            throw EngineError("unsupported",
                "margins: --at cannot fix '" + var + "' because the design also carries the "
                "derived term '" + nm + "'; that column would keep its observed value and the "
                "margin would not be the one asked for");
    }
}

// contract_A §31: project a just-completed fit into the storable PostestModel — the
// exact field set lincom/nlcom/test consume. Takes the FeolsResult by rvalue (every
// call site passes the prvalue straight out of feols_fit) and MOVES the three big
// vectors out of it, so the full-specification path pays no copy for being storable.
//
// The vcov travels VERBATIM: this is the single point at which the fit's own
// covariance (whatever --vcov produced) becomes the covariance every later
// handle-addressed post-estimation call will use. Nothing here re-derives, defaults,
// or substitutes it — an `iid` fallback is not reachable from this function.
PostestModel postest_project(FeolsResult&& r) {
    PostestModel m;
    m.names = std::move(r.names);
    m.beta = std::move(r.beta);
    m.vcov_matrix = std::move(r.vcov_matrix);
    m.vcov_type = std::move(r.vcov_type);
    m.exact_fit_bread = std::move(r.exact_fit_bread);
    m.n_clusters = r.n_clusters;
    m.dof_resid = r.dof.dof_resid;
    // Stata e(N): rows unweighted and under aw/pw, the weight sum under fw/iw --
    // run_feols' n_obs rule. nlcom posts it as r(N) (w2h_postest_refit.log:750
    // R_fw_nlcom r(N) 360 on 120 rows).
    m.n = (r.n_eff > 0.0) ? static_cast<int64_t>(std::llround(r.n_eff)) : r.n;
    // BUG-1231: the classic V = s^2 (X'X)^-1 keeps its s^2 for test's negligible-
    // constraint bound (cmd_test, wald_dropped_rows on V / s^2); 0 on an exact fit.
    if (m.vcov_type == "iid") m.ls_scale = r.sigma2;
    return m;
}

namespace {

// FU-541: a refit of a logit/probit fit must fit the rows and columns the fit
// did. Stata's logit/probit drop a perfect predictor and its determined rows
// before ml unless `asis`, and every
// post-estimation verb answers from that model (e(b) with o.x at 0, e(sample)
// without the rows; w2u_binary_ml_edges.log bm_l_dz0_pred row 5
// .6736077370505257). check_binary first, then the screen (`--asis` skips it);
// g is left holding the screened design. `ds`/`xs`: what g was loaded from (the
// screen tests the varlist's factor base levels too). Returns false after
// printing a refusal.
bool binary_refit_screen(const std::string& model, GlmInputs& g, bool asis, const std::string& y,
                         const char* who, BinaryPerfectScreen& screen, std::vector<int>& fit_cols,
                         const Dataset& ds, const std::vector<std::string>& xs, bool has_constant) {
    fit_cols.clear();
    for (int c = 0; c < g.k; ++c) fit_cols.push_back(c);
    screen = BinaryPerfectScreen{};
    if (model != "logit" && model != "probit" && model != "cloglog") return true;
    if (!check_binary(g.y, who, y)) return false;
    const std::vector<BinaryBaseLevel> bases =
        asis ? std::vector<BinaryBaseLevel>() : binary_base_levels(ds, xs, g);
    return binary_screen_glm(g, asis, has_constant, who, y, screen, fit_cols, false, &bases);
}

// FU-426: the logit/probit fit's maximize options (--tolerance/--ltolerance/
// --nrtolerance/--iterate/--difficult, read as cmd_logit/cmd_probit read them;
// PIET forwards the fit's own, cmds_mle.cpp echo_ml_max_options). Every
// logit/probit refit in this file takes them, so it stops at the iterate the fit
// posted. Without them the refit stopped at the default iterate: after `logit d
// x1 x2 i.c, tolerance(1e-12) ltolerance(1e-15) nrtolerance(1e-12) iterate(300)`
// (e(ic) 10) `test 2.c` answered from the e(ic) 8 model, r(chi2)
// 1.4dfdeed2ce2a4X-008 where Stata's is 1.4dfd97e5fa7aaX-008 (w2e_probes.log:141
// lp_default, :249 lp_tight). Read unconditionally (the E6 net); a refit that is
// not logit/probit has no such options and refuses them rather than ignore them.
// Returns false after printing the refusal.
bool refit_ml_max_options(const std::vector<std::string>& a, bool binary_ml_refit, const char* who,
                          MlMaxOptions& mo) {
    bool any = false;
    for (const char* f : {"--tolerance", "--ltolerance", "--nrtolerance", "--iterate", "--difficult"})
        if (has(a, f)) any = true;
    if (any && !binary_ml_refit) {
        std::fprintf(stderr,
            "%s: --tolerance/--ltolerance/--nrtolerance/--iterate/--difficult are the maximize "
            "options of a logit/probit refit (--model logit|probit); this refit has none.\n", who);
        return false;
    }
    return parse_ml_max_options(a, who, mo);
}

// logit_fit / probit_fit under the fit's maximize options: a binary outcome takes
// the same ml call logit_fit / probit_fit make (start at the constant-only fit
// when the last column is the constant), with `mo` in place of their default
// options, so a default `mo` is the call those made. Every caller has passed
// check_binary; a non-binary y keeps their IRLS fallback.
LogitResult refit_logit(const GlmInputs& g, const MlMaxOptions& mo, const double* wptr = nullptr) {
    if (!outcome_is_binary(g.y.data(), g.n))
        return logit_fit(g.X.data(), g.n, g.k, g.y.data(), mo.maxiter, 1e-14, wptr);
    return logit_fit_ml(g.X.data(), g.n, g.k, g.y.data(), mo, wptr, nullptr,
                        last_column_is_constant(g.X.data(), g.n, g.k));
}

ProbitResult refit_probit(const GlmInputs& g, const MlMaxOptions& mo) {
    if (!outcome_is_binary(g.y.data(), g.n))
        return probit_fit(g.X.data(), g.n, g.k, g.y.data(), mo.maxiter);
    return probit_fit_ml(g.X.data(), g.n, g.k, g.y.data(), mo, nullptr,
                         last_column_is_constant(g.X.data(), g.n, g.k));
}

// ---- BUG-203: post-estimation off the model that was ACTUALLY fitted --------
//
// Until this batch `lincom`/`nlcom`/`test` had no `--model` flag at all, so the
// three producers above fed `feols_fit()` unconditionally and a caller who had
// estimated `logit y x1 x2` got the LINEAR-PROBABILITY-MODEL combination back
// with no error. Measured against StataNow 19.5 (n=120, seed 203203):
// `logit ybin x1 x2` then `lincom x1` — Stata 1.087742905079 (se .266346342116),
// this engine 0.221521751160 (se .046231897247), which is exactly what its own
// `regress ybin x1 x2` then `lincom x1` prints. `poisson ybin x1 x2` then
// `lincom x1` returned that SAME 0.221521751160, i.e. the estimator was never
// read at all.
//
// The refit mirrors cmd_margins' GLM branch (further down this file) exactly —
// load_glm + logit_fit / probit_fit / fepois_fit, the same information-matrix
// covariance — because that branch is the in-tree precedent for answering
// post-estimation off a real ML fit, and sharing its machinery is what keeps
// the two surfaces from drifting apart (BUG-188/BUG-194 were about that
// surface).
//
// Fills `out` and returns true; on any refusal it has already written a message
// to stderr and returns false. It refuses rather than approximating: this whole
// function exists to remove a silent wrong answer, so it must not introduce one.
//
// no_intercept (FU-131, Stata `logit/probit ..., noconstant`): the fit dropped
// load_glm's trailing constant (cmd_logit/cmd_probit's drop_glm_intercept), so
// the refit drops it the same way and answers for that model
// (w2p_estim_ev.log:1604 G_pnc_lincom_x1 = :1585 G_pnc_fit). poisson refuses it:
// PIET refuses `poisson ..., noconstant` at the fit, so no caller reaches it.
//
// mo (FU-426): the logit/probit fit's maximize options (refit_ml_max_options).
// The refit's V is then the posted one too: logit's bread and probit's
// probit_observed_info bread are both the inverse of the Hessian the ml
// iteration used, completely determined rows dropped (FU-435, logit.h rule 4),
// which is what cmd_logit / cmd_probit post as e(V).
bool postest_fit_glm(const std::string& model, const Dataset& ds, const std::string& y,
                     const std::vector<std::string>& xs, PostestModel& out,
                     std::vector<int64_t>* keep_out = nullptr, bool asis = false,
                     BinaryPerfectScreen* rules_out = nullptr, bool no_intercept = false,
                     const MlMaxOptions& mo = MlMaxOptions{}) {
    if (model != "logit" && model != "probit" && model != "cloglog" && model != "poisson") {
        std::fprintf(stderr,
            "post-estimation --model: '%s' has no maximum-likelihood refit in this engine "
            "(supported: logit, probit, cloglog, poisson).\n", model.c_str());
        return false;
    }
    if (no_intercept && model == "poisson") {
        std::fprintf(stderr,
            "post-estimation --model poisson: --no-intercept is not supported for the "
            "poisson refit in this engine.\n");
        return false;
    }
    GlmInputs g = load_glm(ds, "postest", y, xs);
    if (g.n == 0) {
        std::fprintf(stderr, "post-estimation --model %s: no rows after dropping missing.\n",
                     model.c_str());
        return false;
    }
    if (no_intercept) drop_glm_intercept(g, "postest");
    // cloglog's constant-only z, from the counts before its screen as cmd_cloglog takes it
    double z0 = 0.0;
    if (model == "cloglog") {
        double n0 = 0.0;
        for (double v : g.y) n0 += (v == 0.0) ? 1.0 : 0.0;
        z0 = std::log(std::log(static_cast<double>(g.n) / n0));
    }
    // FU-541: the screened design; full_names are the posted e(b) columns.
    const std::vector<std::string> full_names = g.names;
    BinaryPerfectScreen screen;
    std::vector<int> fit_cols;
    if (!binary_refit_screen(model, g, asis, y, "post-estimation", screen, fit_cols, ds, xs, !no_intercept))
        return false;
    if (model == "logit") {
        if (!check_binary(g.y, "lincom/nlcom/test(logit)", y)) return false;
        // A separated fit was posted at rc 0 with every row determined (cmd_logit;
        // BUG-1233 without asis too), so its refit answers from that fit.
        LogitResult lr = refit_logit(g, mo);
        out.beta = lr.beta;
        out.vcov_matrix = lr.bread;  // (X'WX)^{-1} — the OIM asymptotic covariance
    } else if (model == "probit") {
        if (!check_binary(g.y, "lincom/nlcom/test(probit)", y)) return false;
        ProbitResult pr = refit_probit(g, mo);  // separated: as logit above
        out.beta = pr.beta;
        // BUG-140's boundary, and the ONE place this refit cannot take the fitter's
        // own `V`. probit is a NON-canonical link, so the Fisher/expected
        // information (X'WX)^{-1} that `pr.V` carries is NOT the covariance Stata's
        // `probit` reports — Stata uses the OBSERVED Hessian, weight lambda(lambda+eta).
        // cmd_probit (cmds_mle.cpp) computes exactly this for its default iid SE;
        // taking `pr.V` here instead made `lincom x1` after `probit` report
        // se .152915189 where the probit table itself (and Stata) say .154831744 —
        // a 1.2% gap, measured, not rounding. Post-estimation MUST reproduce the
        // fit's own covariance, so this is the observed-information bread.
        //
        // This was the THIRD copy of the observed-information loop (cmds_mle.cpp's
        // cmd_probit and cmd_margins' vce(robust) branch below carried the other
        // two), and that triplication is exactly how BUG-210 happened: BUG-140
        // swept one copy and `margins`' iid branches, which had no copy at all and
        // simply read the Fisher r.V, were never revisited. BUG-210 hoisted it into
        // src/estimators/probit.h; this is now a delegation, and so are the others.
        // The SPD fallback to the Fisher bread lives inside that one definition.
        // tests/unit/test_lincom.cpp's test_lincom_ml_refit_bug203 pins this site
        // against the live Stata number, so a divergence here fails a test rather
        // than shipping quietly.
        out.vcov_matrix =
            probit_observed_info(pr, g.X.data(), g.n, g.k, g.y.data()).bread;
    } else if (model == "cloglog") {
        // cmd_cloglog's own fit and OIM V (StataBE `lincom x` after `cloglog yb x z d`:
        // .50786860699644709, se .076212489114577064, eng-ml/x21.log LCd)
        if (!check_binary(g.y, "lincom/nlcom/test(cloglog)", y)) return false;
        const BinaryMlResult cr = cloglog_ml(g, !no_intercept, z0, mo);
        out.beta = cr.beta;
        out.vcov_matrix = cr.bread;
    } else {
        for (double vv : g.y) {
            if (vv < 0) {
                std::fprintf(stderr, "post-estimation --model poisson: %s has negatives.\n",
                             y.c_str());
                return false;
            }
        }
        ModelData md;
        md.has_intercept = true;
        md.dm.n = g.n; md.dm.k = g.k;
        md.dm.X = g.X; md.dm.y = g.y; md.dm.xnames = g.names; md.keep = g.keep;
        FepoisResult pf = fepois_fit(md, Vcov::IID);
        out.beta = pf.beta;
        out.vcov_matrix = pf.vcov_matrix;
    }
    if (keep_out) *keep_out = g.keep;
    out.names = g.names;   // x names then "(intercept)" (absent under no_intercept) — resolve_coef maps `_cons` onto it
    if (fit_cols.size() != full_names.size()) {
        // Back to the posted columns: an omitted term has _b 0 and zero rows and
        // columns in V, as Stata's e(b)/e(V) carry it.
        const size_t K = full_names.size(), kf = fit_cols.size();
        std::vector<double> b(K, 0.0), V(K * K, 0.0);
        for (size_t i = 0; i < kf; ++i) {
            b[static_cast<size_t>(fit_cols[i])] = out.beta[i];
            for (size_t j = 0; j < kf; ++j)
                V[static_cast<size_t>(fit_cols[i]) + static_cast<size_t>(fit_cols[j]) * K] =
                    out.vcov_matrix[i + j * kf];
        }
        out.beta = std::move(b);
        out.vcov_matrix = std::move(V);
        out.names = full_names;
    }
    if (rules_out) *rules_out = screen;
    out.vcov_type = "oim";
    out.n_clusters = 0;
    out.dof_resid = 0;     // an ML fit has none; `asymptotic` below is what the tails read
    out.n = g.n;
    out.asymptotic = true;
    return true;
}

// The reference degrees of freedom / critical value / p-value the shared tails
// below must use for a model. ONE definition, three call sites, so lincom,
// nlcom and test can never disagree about which distribution a fit is
// referenced to. For a least-squares fit these reproduce the pre-BUG-203
// expressions literally (`n_clusters > 0 ? G-1 : dof_resid`, t(dof), t p-value);
// for an ML fit they are the normal, which is what Stata prints after
// logit/probit/poisson.
double postest_dof(const PostestModel& M) {
    if (M.asymptotic) return 0.0;
    return (M.n_clusters > 0) ? (M.n_clusters - 1.0) : static_cast<double>(M.dof_resid);
}
// lincom's reference dof: postest_dof, except after a multiway `regress` (FU-439,
// PostestModel::lincom_resid_dof), where Stata's lincom reads the residual dof N - k
// (w2s_multiway_class.log:158 r(df) 177; test keeps e(df_r) 6, :174).
double postest_lincom_dof(const PostestModel& M) {
    if (!M.asymptotic && M.lincom_resid_dof) return static_cast<double>(M.dof_resid);
    return postest_dof(M);
}
// BUG-333: `level` was hard-wired to 95 here, which is why `lincom, level(90)`
// and `nlcom, level(90)` had nothing to forward to and PIET rejected the option
// outright. The parameter defaults to 95.0 so `test` (Stata's `test` has no
// level()) and every other caller keep their exact previous behaviour; only
// lincom/nlcom pass a user value. alpha is derived the same way cmds_mle.cpp's
// logit does it, so the two surfaces cannot drift apart.
double postest_crit(const PostestModel& M, double dof, double level) {
    const double alpha = (level == 95.0) ? 0.05 : 1.0 - level / 100.0;
    if (M.asymptotic) return z_critical(alpha);
    return (dof > 0) ? t_critical(alpha, dof) : 0.0;
}
double postest_pvalue(const PostestModel& M, double t, double dof) {
    if (M.asymptotic) return z_twosided_pvalue(t);
    return (dof > 0) ? t_twosided_pvalue(t, dof) : 1.0;
}

// Fit the `--data/--y/--x[/--fe/--vcov/--cluster]` specification the way the
// caller's ESTIMATOR would have. Reads every model-spec flag unconditionally so
// each one is marked consumed regardless of which branch runs (the E6
// unconsumed-flag net would otherwise fire on the branch that does not read it).
// Returns 0 on success, or the CLI exit code to return on failure.
//
// `--model` absent (or naming the least-squares family) keeps the historical
// path byte-for-byte: same build_model, same parse_vcov default, same
// feols_fit. This is what every existing eval case, freeze fixture and PIET
// call after `regress`/`reghdfe` takes.
// The distinct values every factor variable of the varlist takes over the rows the
// fit KEPT — what lets resolve_coef tell the omitted base level `0.c` (coefficient 0,
// variance 0) apart from a level the sample never had (`9.c not found`, r(111)).
postest::FactorLevels sample_factor_levels(const Dataset& ds, const std::vector<std::string>& xs,
                                           const std::vector<int64_t>& keep) {
    postest::FactorLevels out;
    for (const auto& v : postest::factor_vars_of(xs)) {
        if (!ds.has(v) || ds.col(v).dtype != DType::F64) continue;
        const double* col = ds.col(v).f64();
        std::vector<double> lv;
        lv.reserve(keep.size());
        for (int64_t r : keep) {
            const double x = col[r];
            if (!std::isnan(x)) lv.push_back(x);
        }
        std::sort(lv.begin(), lv.end());
        lv.erase(std::unique(lv.begin(), lv.end()), lv.end());
        out[v] = std::move(lv);
    }
    return out;
}

// BUG-1268: a least-squares refit's x list. A constant-only `regress y` is refitted
// as `--x (intercept)` (PIET estimation_m4c.cpp:314-316); the token names the
// design's own intercept, as cmd_feols reads it (cmds_linear.cpp:278), so it leaves
// the list (StataBE: `lincom _cons` after `regress y` r(se) .26426452122728694).
std::vector<std::string> refit_xs(const std::string& xcsv) {
    std::vector<std::string> xs = split_csv(xcsv);
    xs.erase(std::remove(xs.begin(), xs.end(), std::string("(intercept)")), xs.end());
    return xs;
}

// `row_mask` (length ds.n_rows(), 1 = inside the fit's `if`): the restriction the
// fit applied AFTER its time-series operators read the full series (plan_keepif's
// E1 path; postest_refit below). Null = `ds` already is the fit's sample.
// `--keep-singletons`: the fit kept singleton fixed-effect groups (areg, xtreg,fe,
// reghdfe's keepsingletons), so the refit keeps them too -- dropping them answers
// for a different sample (w2_esample.log:897 D3 xtreg,fe e(N) 59 / N_g 11, :925 D4
// areg 59, :1957 H8 58: Stata keeps the singleton id 11).
int postest_fit_spec(const std::vector<std::string>& a, const Dataset& ds, const std::string& y,
                     const std::string& xcsv, PostestModel& fitted,
                     postest::FactorLevels* levels = nullptr,
                     BinaryPerfectScreen* rules_out = nullptr,
                     const std::vector<char>* row_mask = nullptr) {
    const std::string model = opt(a, "--model");
    // FU-541: the fit was `logit/probit ..., asis` (no perfect-prediction screen).
    const bool asis = has(a, "--asis");
    const bool keep_singletons = has(a, "--keep-singletons");
    const std::string fe = opt(a, "--fe");
    const std::string vcov_type = opt(a, "--vcov", "iid");
    const std::string cluster = opt(a, "--cluster");
    // BUG-571 / BUG-568 / FU-385: the fit's weight and its clustered small-sample
    // convention, under cmd_feols' own spellings, so the refit is the model that
    // was fitted. Without them `lincom x1` after `regress [pweight=w]` answered the
    // unweighted b (w2h_postest_refit.log:383 R_pw_lincomx1 2.381531911602316 vs
    // 2.4882849226167476) and after `areg, vce(cluster id)` reghdfe's nested-FE
    // exemption (:1081 A_cl se .27856854160853 vs :2005 H_cl_ctl .2613712121703248).
    const std::string weight_var = opt(a, "--weight"), wtype = opt(a, "--wtype");
    const bool keep_nested_fe_dof = has(a, "--keep-nested-fe-dof");
    const bool multiway_subset_factors = has(a, "--multiway-subset-factors");
    // The fit was Stata's builtin `regress` (cmd_feols --regress-exact-fit), so the
    // refit applies its exact-fit rule too: `test x` after an exact `regress` then
    // sees V = 0 and answers F and p missing, as StataBE does (F(1,48) = .).
    const bool regress_exact_fit = has(a, "--regress-exact-fit");
    // BUG-1067: the fit kept the earlier term of each collinear set (cmd_feols
    // --collinear-forward: reghdfe, areg, xtreg,fe), so the refit omits the same
    // columns and every verb answers for the coefficients the fit posted.
    const bool collinear_forward = has(a, "--collinear-forward");
    // FU-131: the fit's `noconstant` (cmd_feols / cmd_logit / cmd_probit's
    // --no-intercept). Without it the refit appended the constant, and every verb
    // answered for the with-constant model (w2p_estim_ev.log:1294 G_nc_lincom_x1
    // 2.356553697480766 is the no-constant fit's _b[x1], :1448 G_c_lincom_x1
    // 1.959601560760043 the with-constant one's). Absent: byte-identical argv path.
    const bool no_intercept = has(a, "--no-intercept");
    const bool ols_family = model.empty() || model == "ols" || model == "feols" ||
                            model == "regress" || model == "reg";
    // FU-426: the logit/probit fit's maximize options.
    MlMaxOptions ml_opts;
    if (!refit_ml_max_options(a, model == "logit" || model == "probit" || model == "cloglog",
                              "post-estimation", ml_opts))
        return 2;
    // FU-385/FU-387: the 2SLS refit (Stata `ivregress 2sls`): the same iv2sls_fit
    // cmd_iv runs, under the fit's --vcov/--cluster/--small and its multiway
    // convention, so lincom/test/nlcom answer from the fit's own b and V
    // (w2s_multiway_class.log:840 mw_iv_c2_lincom r(se) .0503059336995026 = :816
    // _se[x1], r(df) `.`: without --small the reference is the normal). Absorbed
    // effects, weights, the feols-only switches and noconstant have no measured
    // ivregress refit and are refused.
    if (model == "iv") {
        const std::vector<std::string> endog = split_csv(opt(a, "--endog"));
        const std::vector<std::string> insts = split_csv(opt(a, "--instruments"));
        const bool small = has(a, "--small");
        if (endog.empty() || insts.empty()) {
            std::fprintf(stderr, "post-estimation --model iv: --endog and --instruments are "
                                 "required.\n");
            return 2;
        }
        if (!fe.empty() || !weight_var.empty() || !wtype.empty() || keep_nested_fe_dof ||
            keep_singletons || no_intercept || regress_exact_fit || collinear_forward) {
            std::fprintf(stderr,
                "post-estimation --model iv: --fe, --weight/--wtype, --keep-nested-fe-dof, "
                "--keep-singletons, --no-intercept, --regress-exact-fit and --collinear-forward "
                "are not supported for the 2SLS refit in this engine.\n");
            return 2;
        }
        const Vcov vt = parse_vcov(vcov_type);
        if (multiway_subset_factors && (!small || vt != Vcov::Multiway)) {
            std::fprintf(stderr, "post-estimation --model iv: --multiway-subset-factors requires "
                                 "--small and --vcov twoway or multiway.\n");
            return 2;
        }
        std::vector<std::string> exog;
        for (const auto& x : split_csv(xcsv))
            if (x != "(intercept)") exog.push_back(x);
        const cecon::TsMeta tsm = parse_tsmeta(a);
        IvModelData md = build_iv_model(ds, y, exog, endog, insts, {}, split_csv(cluster),
                                        tsm.timevar.empty() ? nullptr : &tsm, true, row_mask);
        if (levels) {
            std::vector<std::string> terms = exog;
            terms.insert(terms.end(), endog.begin(), endog.end());
            *levels = sample_factor_levels(ds, terms, md.keep);
        }
        IvResult r = iv2sls_fit(md, vt, 0, false, small, false, multiway_subset_factors);
        fitted = PostestModel{};
        fitted.names = std::move(r.names);
        fitted.beta = std::move(r.beta);
        fitted.vcov_matrix = std::move(r.vcov_matrix);
        fitted.vcov_type = std::move(r.vcov_type);
        fitted.n_clusters = r.n_clusters;
        fitted.dof_resid = r.dof.dof_resid;
        fitted.n = r.n;
        fitted.asymptotic = !small;  // ivregress: z/chi2 unless `small` (cmd_iv's inference)
        return 0;
    }
    if (!ols_family) {
        // The ML refit (postest_fit_glm) has no weight input and no cluster dof
        // switch; accepting either would answer the unweighted / default-dof fit.
        // cmd_margins' GLM branch takes pw, but this dispatcher does not share it,
        // so it refuses (PIET refuses a weighted ML fit before calling here).
        if (!weight_var.empty() || !wtype.empty() || keep_nested_fe_dof ||
            multiway_subset_factors || keep_singletons || regress_exact_fit ||
            collinear_forward) {
            std::fprintf(stderr,
                "post-estimation --model %s: --weight/--wtype, --keep-nested-fe-dof, "
                "--multiway-subset-factors, --keep-singletons, --regress-exact-fit and "
                "--collinear-forward are not supported for a maximum-likelihood refit in this "
                "engine.\n",
                model.c_str());
            return 2;
        }
        // postest_fit_glm has no row-mask input: a time-series fit under `if` would
        // be refitted on the physically filtered rows, where its lags are missing.
        if (row_mask != nullptr) {
            std::fprintf(stderr,
                "post-estimation --model %s: a fit with time-series operators under "
                "--keepif is not supported for a maximum-likelihood refit in this engine.\n",
                model.c_str());
            return 2;
        }
        // The ML refit is the plain OIM fit with no absorbed effects and no
        // alternative VCE — exactly what cmd_margins' GLM branch supports.
        // Honouring the flag while ignoring --fe/--vcov/--cluster would answer
        // under a specification the model was not fitted under, which is the
        // very shape this fix removes, so it refuses instead.
        if (!fe.empty() || !cluster.empty() || (vcov_type != "iid" && vcov_type != "oim")) {
            std::fprintf(stderr,
                "post-estimation --model %s: fixed effects and non-default vcov are not "
                "supported for a maximum-likelihood refit in this engine (got"
                "%s%s%s%s%s%s).\n",
                model.c_str(),
                fe.empty() ? "" : " --fe ", fe.empty() ? "" : fe.c_str(),
                cluster.empty() ? "" : " --cluster ", cluster.empty() ? "" : cluster.c_str(),
                (vcov_type == "iid" || vcov_type == "oim") ? "" : " --vcov ",
                (vcov_type == "iid" || vcov_type == "oim") ? "" : vcov_type.c_str());
            return 2;
        }
        std::vector<int64_t> keep;
        if (!postest_fit_glm(model, ds, y, split_csv(xcsv), fitted, &keep, asis, rules_out, no_intercept,
                             ml_opts))
            return 2;
        if (levels) *levels = sample_factor_levels(ds, split_csv(xcsv), keep);
        return 0;
    }
    // BUG-369: the refit re-parses the ORIGINAL varlist, so an `L.x` regressor
    // needs the tsset/xtset metadata the FIT was given — .hbin does not persist
    // it. Without this, `regress y L.x` fitted fine and `test L.x` on the very
    // same model died with ts_no_tsset. Empty timevar (no flags) keeps every
    // existing caller's design byte-identical, exactly as the estimators do.
    const cecon::TsMeta tsmeta = parse_tsmeta(a);
    // The same validation cmd_feols applies to these flags (cmds_linear.cpp): an
    // unknown weight kind, a pweight without a sandwich VCE, an iweight with
    // absorbed effects, and a dof switch where it would be ignored are refused,
    // never refitted as some other model.
    cecon::WeightKind wkind = cecon::WeightKind::None;
    if (!weight_var.empty() || !wtype.empty()) {
        if (weight_var.empty())
            throw EngineError("bad_input", "post-estimation refit: --wtype requires --weight");
        if (wtype.empty() || wtype == "aw" || wtype == "aweight") wkind = cecon::WeightKind::Analytic;
        else if (wtype == "fw" || wtype == "fweight") wkind = cecon::WeightKind::Frequency;
        else if (wtype == "iw" || wtype == "iweight") wkind = cecon::WeightKind::Importance;
        else if (wtype == "pw" || wtype == "pweight") wkind = cecon::WeightKind::Probability;
        else
            throw EngineError("bad_input",
                "post-estimation refit: --wtype must be one of aw|fw|iw|pw (got '" + wtype + "')");
        if (wkind == cecon::WeightKind::Importance && !split_csv(fe).empty())
            throw EngineError("bad_input",
                "post-estimation refit: [iweight=] is not allowed with --fe (Stata's "
                "areg/reghdfe/xtreg,fe take aweight, fweight and pweight only)");
        const bool sandwich = vcov_type == "hc1" || vcov_type == "HC1" ||
                              vcov_type == "cluster" || vcov_type == "twoway" ||
                              vcov_type == "multiway";
        if (wkind == cecon::WeightKind::Probability && !sandwich)
            throw EngineError("bad_input",
                "post-estimation refit: [pweight=] requires --vcov hc1, cluster or twoway; "
                "Stata has no iid VCE under pweight");
    }
    if (keep_nested_fe_dof) {
        const bool cluster_vce = vcov_type == "cluster" || vcov_type == "twoway" ||
                                 vcov_type == "multiway";
        if (!cluster_vce || split_csv(fe).empty())
            throw EngineError("bad_input",
                "post-estimation refit: --keep-nested-fe-dof requires --fe and --vcov cluster "
                "or twoway (it selects which absorbed parameters the cluster small-sample "
                "factor charges)");
    }
    if (multiway_subset_factors && vcov_type != "twoway" && vcov_type != "multiway")
        throw EngineError("bad_input",
            "post-estimation refit: --multiway-subset-factors requires --vcov twoway or "
            "multiway (it selects the small-sample convention of a multiway cluster variance)");
    // cmd_feols' own rule for --regress-exact-fit (cmds_linear.cpp). BUG-1055: Stata's
    // areg and its [aw=]/[fw=] regress post rss 0 / V 0 on an exact fit too (reghdfe
    // does not, and sends no flag), so the refit takes the flag with absorbed effects
    // and those weights; the classic variance only, never under a pweight.
    if (regress_exact_fit && (wkind == cecon::WeightKind::Probability || vcov_type != "iid"))
        throw EngineError("bad_input",
            "post-estimation refit: --regress-exact-fit requires --vcov iid without a pweight "
            "(it selects Stata regress/areg's exact-fit rule for the classic variance)");
    // ... and with --collinear-forward only under --fe (areg; regress keeps the
    // norm-pivoted basis), cmd_feols' pairing rule.
    if (regress_exact_fit && collinear_forward && split_csv(fe).empty())
        throw EngineError("bad_input",
            "post-estimation refit: --collinear-forward is not valid with --regress-exact-fit "
            "without --fe (Stata regress keeps the norm-pivoted basis)");
    // cmd_feols' own rule (cmds_linear.cpp): absorbed effects already absorb any constant.
    if (no_intercept && !split_csv(fe).empty())
        throw EngineError("bad_input", "post-estimation refit: --no-intercept is not valid with --fe");
    ModelData md = build_model(ds, y, refit_xs(xcsv), split_csv(fe), split_csv(cluster),
                               /*offset_name=*/"", /*drop_singleton=*/!keep_singletons,
                               tsmeta.timevar.empty() ? nullptr : &tsmeta, row_mask,
                               weight_var, wkind);
    md.collinear_forward = collinear_forward;
    if (levels) *levels = sample_factor_levels(ds, refit_xs(xcsv), md.keep);
    fitted = postest_project(feols_fit(md, parse_vcov(vcov_type), /*cluster_index=*/0,
                                       no_intercept, keep_nested_fe_dof,
                                       multiway_subset_factors, regress_exact_fit));
    // FU-439: a multiway `regress` (the built-in convention with no absorbed effect).
    const Vcov vt_fit = parse_vcov(vcov_type);
    fitted.lincom_resid_dof =
        multiway_subset_factors && vt_fit == Vcov::Multiway && split_csv(fe).empty();
    return 0;
}

// The full-specification refit of test/lincom/nlcom, with the fit's `--keepif`
// routed the way cmd_feols routes it (plan_keepif): a regressor list carrying a
// time-series operator keeps the FULL data and applies the restriction as a row
// mask after the operators, so `xtreg y L.x1 x2 if t>=3, fe` then `lincom L.x1`
// refits on the fit's own 40 rows (w2_esample.log:751-775 C6 e(N) 40) instead of
// the physically filtered 30 whose t==3 lags are missing. Every other call keeps
// maybe_keepif's physical pre-filter, byte-identical.
int postest_refit(const std::vector<std::string>& a, const std::string& data,
                  const std::string& y, const std::string& xcsv, PostestModel& fitted,
                  postest::FactorLevels* levels) {
    const cecon::TsMeta tsm = parse_tsmeta(a);
    const std::vector<std::string> xs = refit_xs(xcsv);
    // The 2SLS refit's endogenous regressors and instruments may carry operators too
    // (cmd_iv's plan_keepif). Read here only for --model iv; postest_fit_spec reads
    // them again, so for any other model they stay unconsumed (the E6 net refuses).
    std::vector<std::string> ivv;
    if (opt(a, "--model") == "iv") {
        for (const auto& v : split_csv(opt(a, "--endog"))) ivv.push_back(v);
        for (const auto& v : split_csv(opt(a, "--instruments"))) ivv.push_back(v);
    }
    // BUG-1277: an operated depvar (`regress D.y x1 x2 if t>=3`) reads the full
    // series too, as cmd_feols/cmd_iv route it.
    const std::vector<std::string> ys{y};
    const KeepifPlan kp = plan_keepif(data, opt(a, "--keepif"), &tsm, {&xs, &ivv, &ys});
    const Dataset& ds = acquire_data(kp.data);
    return postest_fit_spec(a, ds, y, xcsv, fitted, levels, nullptr, kp.active ? &kp.mask : nullptr);
}

}  // namespace

// Build R (J*k row-major) and q (length J) from the restrictions in the order they
// were given (`joint` true: a --joint name, false: a --constraint equation), against
// the fitted coefficient names. Every name goes through
// postest::resolve_coef and every equation through postest::LinearParser — the
// ONE grammar lincom and nlcom use too — so `1.c`, `_b[2.c]` and the omitted base
// level `0.c` mean the same thing on all four post-estimation surfaces.
//
// A restriction that names ONLY omitted coefficients is a zero row of R, kept here
// whatever its right-hand side: cmd_test drops it with the other restrictions that
// are not independent (wald_dropped_rows), as Stata does with `test 1.c = 1` on the
// base level ("Constraint 1 dropped", rc 0).
//
// Returns 0 on success; 2 on a syntax/usage error (err set); 3 when a factor level
// could not be classified without the estimation sample (handle path only).
// Throws EngineError("coef_not_found") for a name the model does not have, so the
// caller's JSON error carries a code a client can map to Stata's r(111).
int build_restrictions(const std::vector<std::string>& names,
                       const std::vector<std::pair<bool, std::string>>& items,
                       const postest::FactorLevels* levels,
                       std::vector<double>& R, std::vector<double>& q,
                       int& J, std::string& err) {
    const int k = static_cast<int>(names.size());
    std::vector<std::vector<double>> rows;
    std::vector<double> qs;
    auto not_found = [](const std::string& nm) {
        throw EngineError("coef_not_found", nm + " not found", nm);
    };
    for (const auto& [is_joint, text] : items) {
        if (is_joint) {
            const postest::CoefRef r = postest::resolve_coef(names, text, levels);
            if (r.kind == postest::CoefKind::NeedsSample) {
                err = "test: " + r.name + " cannot be classified without the estimation sample";
                return 3;
            }
            if (r.kind == postest::CoefKind::NotFound) not_found(r.name);
            std::vector<double> row(static_cast<size_t>(k), 0.0);
            if (r.kind == postest::CoefKind::Column) row[static_cast<size_t>(r.idx)] = 1.0;
            rows.push_back(std::move(row));
            qs.push_back(0.0);
            continue;
        }
        const std::string& con = text;
        size_t eq = find_assign(con);
        if (eq == std::string::npos) { err = "test: constraint '" + con + "' has no '='"; return 2; }
        postest::LinearForm lhs, rhs;
        bool needs_sample = false;
        for (int side = 0; side < 2; ++side) {
            const std::string part = side == 0 ? con.substr(0, eq) : con.substr(eq + 1);
            postest::LinearParser lp(part, names, levels);
            std::string perr;
            if (!lp.parse(side == 0 ? lhs : rhs, perr, needs_sample)) {
                if (needs_sample) { err = "test: " + perr; return 3; }
                if (perr.size() > 10 && perr.compare(perr.size() - 10, 10, " not found") == 0)
                    not_found(perr.substr(0, perr.size() - 10));
                err = "test: " + perr + " in constraint '" + con + "'";
                return 2;
            }
        }
        std::vector<double> row(static_cast<size_t>(k), 0.0);
        for (int c = 0; c < k; ++c)
            row[static_cast<size_t>(c)] = lhs.w[static_cast<size_t>(c)] - rhs.w[static_cast<size_t>(c)];
        rows.push_back(std::move(row));
        qs.push_back(rhs.konst - lhs.konst);  // lhs - rhs = 0 → R β = (kr - kl)
    }
    if (rows.empty()) { err = "test: need --joint or --constraint"; return 2; }
    J = static_cast<int>(rows.size());
    R.assign(static_cast<size_t>(J) * k, 0.0);
    q.assign(qs.begin(), qs.end());
    for (int r = 0; r < J; ++r)
        for (int c = 0; c < k; ++c) R[static_cast<size_t>(r) * k + c] = rows[static_cast<size_t>(r)][static_cast<size_t>(c)];
    return 0;
}

// standard normal CDF + inverse (for the mediation z-test and BC bootstrap cutoffs).
double med_ncdf(double x) { return 0.5 * std::erfc(-x * 0.7071067811865476); }
double med_ninv(double p) {
    static const double a[] = {-3.969683028665376e+01, 2.209460984245205e+02, -2.759285104469687e+02,
                               1.383577518672690e+02, -3.066479806614716e+01, 2.506628277459239e+00};
    static const double b[] = {-5.447609879822406e+01, 1.615858368580409e+02, -1.556989798598866e+02,
                               6.680131188771972e+01, -1.328068155288572e+01};
    static const double c[] = {-7.784894002430293e-03, -3.223964580411365e-01, -2.400758277161838e+00,
                               -2.549732539343734e+00, 4.374664141464968e+00, 2.938163982698783e+00};
    static const double d[] = {7.784695709041462e-03, 3.224671290700398e-01, 2.445134137142996e+00, 3.754408661907416e+00};
    if (p <= 0) return -1e300; if (p >= 1) return 1e300;
    double plow = 0.02425, phigh = 1 - plow, q, r, x;
    if (p < plow) { q = std::sqrt(-2 * std::log(p)); x = (((((c[0]*q+c[1])*q+c[2])*q+c[3])*q+c[4])*q+c[5]) / ((((d[0]*q+d[1])*q+d[2])*q+d[3])*q+1); }
    else if (p <= phigh) { q = p - 0.5; r = q * q; x = (((((a[0]*r+a[1])*r+a[2])*r+a[3])*r+a[4])*r+a[5])*q / (((((b[0]*r+b[1])*r+b[2])*r+b[3])*r+b[4])*r+1); }
    else { q = std::sqrt(-2 * std::log(1 - p)); x = -(((((c[0]*q+c[1])*q+c[2])*q+c[3])*q+c[4])*q+c[5]) / ((((d[0]*q+d[1])*q+d[2])*q+d[3])*q+1); }
    double e = med_ncdf(x) - p, u = e * 2.5066282746310002 * std::exp(0.5 * x * x);
    return x - u / (1 + 0.5 * x * u);
}

}  // namespace

// mediation: product-of-coefficients indirect effect a*b on a COMMON (complete-case)
// sample. M-model: M = a*X + controls; Y-model: Y = c'*X + b*M + controls. Reports
// Sobel/Aroian/Goodman delta SEs and a Philox bootstrap (percentile + BC) CI of a*b.
int cmd_mediate(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), y = opt(a, "--y"), med = opt(a, "--mediator"), x = opt(a, "--treat");
    if (data_missing(data) || y.empty() || med.empty() || x.empty()) {
        std::fprintf(stderr, "usage: cobra mediate --data f.hbin --y Y --mediator M --treat X "
                             "[--controls c1,c2] [--reps 1000] [--seed 12345] [--level 95] [--keepif EXPR] [--out f.json]\n");
        return 2;
    }
    data = maybe_keepif(data, opt(a, "--keepif"));
    std::vector<std::string> ctrls = split_csv(opt(a, "--controls"));
    int reps = std::stoi(opt(a, "--reps", "1000"));
    if (reps < 1) { std::fprintf(stderr, "mediate: --reps must be >= 1.\n"); return 2; }
    uint64_t seed = static_cast<uint64_t>(std::stoll(opt(a, "--seed", "12345")));
    double level = std::stod(opt(a, "--level", "95"));
    double alpha = 1.0 - level / 100.0;

    const Dataset& ds = acquire_data(data);
    const double* yr = require_num(ds, "mediate", y);
    const double* mr = require_num(ds, "mediate", med);
    const double* xr = require_num(ds, "mediate", x);
    std::vector<const double*> cr;
    for (const auto& c : ctrls) cr.push_back(require_num(ds, "mediate", c));
    int64_t nraw = ds.n_rows();
    std::vector<const double*> all = {yr, mr, xr};
    for (auto* p : cr) all.push_back(p);
    auto keep = listwise_keep(all, nraw);
    const int64_t n = static_cast<int64_t>(keep.size());
    if (n == 0) { std::fprintf(stderr, "mediate: no rows after dropping missing.\n"); return 1; }

    auto Y = gather_rows(yr, keep), M = gather_rows(mr, keep), Xv = gather_rows(xr, keep);
    std::vector<std::vector<double>> C;
    for (auto* p : cr) C.push_back(gather_rows(p, keep));
    const int nc = static_cast<int>(ctrls.size());

    // M-design [X, controls.., 1] (X at col 0); Y-design [X, M, controls.., 1] (X@0, M@1)
    const int kM = 1 + nc + 1, kY = 2 + nc + 1;
    std::vector<double> DM(static_cast<size_t>(n) * kM), DY(static_cast<size_t>(n) * kY);
    for (int64_t i = 0; i < n; ++i) { DM[i] = Xv[i]; DM[static_cast<int64_t>(kM - 1) * n + i] = 1.0; }
    for (int c = 0; c < nc; ++c) for (int64_t i = 0; i < n; ++i) DM[static_cast<int64_t>(1 + c) * n + i] = C[c][i];
    for (int64_t i = 0; i < n; ++i) { DY[i] = Xv[i]; DY[n + i] = M[i]; DY[static_cast<int64_t>(kY - 1) * n + i] = 1.0; }
    for (int c = 0; c < nc; ++c) for (int64_t i = 0; i < n; ++i) DY[static_cast<int64_t>(2 + c) * n + i] = C[c][i];

    auto ols = [&](const std::vector<double>& D, int k, const std::vector<double>& yv,
                   const int64_t* idx, int64_t m, std::vector<double>& beta, std::vector<double>* sd) -> bool {
        std::vector<double> XtX(static_cast<size_t>(k) * k, 0.0), Xty(k, 0.0);
        for (int64_t ii = 0; ii < m; ++ii) {
            int64_t r = idx ? idx[ii] : ii;
            for (int p = 0; p < k; ++p) {
                double xp = D[static_cast<int64_t>(p) * n + r];
                Xty[p] += xp * yv[r];
                for (int q = p; q < k; ++q) XtX[p + q * k] += xp * D[static_cast<int64_t>(q) * n + r];
            }
        }
        for (int p = 0; p < k; ++p) for (int q = 0; q < p; ++q) XtX[p + q * k] = XtX[q + p * k];
        std::vector<double> Inv = XtX;
        if (la::spd_inverse(Inv.data(), k) != 0) return false;
        beta.resize(k);
        la::matvec(Inv.data(), k, Xty.data(), beta.data());
        if (sd) {
            double ssr = 0;
            for (int64_t ii = 0; ii < m; ++ii) {
                int64_t r = idx ? idx[ii] : ii;
                double e = yv[r]; for (int p = 0; p < k; ++p) e -= D[static_cast<int64_t>(p) * n + r] * beta[p];
                ssr += e * e;
            }
            double s2 = ssr / static_cast<double>(m - k);
            sd->resize(k);
            for (int p = 0; p < k; ++p) (*sd)[p] = std::sqrt(s2 * Inv[p + p * k]);
        }
        return true;
    };

    std::vector<double> bM, sM, bY, sY;
    if (!ols(DM, kM, M, nullptr, n, bM, &sM) || !ols(DY, kY, Y, nullptr, n, bY, &sY) ||
        static_cast<int>(bM.size()) < kM || static_cast<int>(sM.size()) < kM ||
        static_cast<int>(bY.size()) < kY || static_cast<int>(sY.size()) < kY) {
        std::fprintf(stderr, "mediate: regression failed (singular design — collinear controls, constant treatment, or too few observations).\n");
        return 1;
    }
    double av = bM[0], se_a = sM[0], bv = bY[1], se_b = sY[1], cp = bY[0];
    double indirect = av * bv, total = cp + indirect;
    double va = se_a * se_a, vb = se_b * se_b;
    double sobel = std::sqrt(av * av * vb + bv * bv * va);
    double aroian = std::sqrt(av * av * vb + bv * bv * va + va * vb);
    double gd = av * av * vb + bv * bv * va - va * vb;
    double goodman = (gd > 0) ? std::sqrt(gd) : std::nan("");
    double zc = med_ninv(1 - alpha / 2);
    double zstat = (sobel > 0) ? indirect / sobel : 0.0;

    // Philox pairs bootstrap of a*b
    std::vector<double> abr(reps);
    std::vector<int64_t> idx(n);
    std::vector<double> rb1, rb2;
    for (int rep = 0; rep < reps; ++rep) {
        for (int64_t g = 0; g < n; ++g) idx[g] = Philox::draw32(seed, static_cast<uint64_t>(rep), static_cast<uint64_t>(g)) % static_cast<uint64_t>(n);
        if (ols(DM, kM, M, idx.data(), n, rb1, nullptr) && ols(DY, kY, Y, idx.data(), n, rb2, nullptr))
            abr[rep] = rb1[0] * rb2[1];
        else abr[rep] = std::nan("");
    }
    std::sort(abr.begin(), abr.end());
    auto pctl = [&](double pp) -> double {
        double rr = pp * (reps - 1);
        int lo = static_cast<int>(std::floor(rr));
        if (lo < 0) return abr.front();
        if (lo >= reps - 1) return abr.back();
        return abr[lo] + (rr - lo) * (abr[lo + 1] - abr[lo]);
    };
    int cntlt = 0; for (double v : abr) if (v < indirect) ++cntlt;
    double z0 = med_ninv(static_cast<double>(cntlt) / reps);
    double zq = med_ninv(1 - alpha / 2);

    json out = {
        {"estimator", "mediate"}, {"n_obs", n},
        {"a", av}, {"se_a", se_a}, {"b", bv}, {"se_b", se_b},
        {"indirect", indirect}, {"direct", cp}, {"total", total},
        {"proportion_mediated", (total != 0) ? indirect / total : std::nan("")},
        {"se_sobel", sobel}, {"se_aroian", aroian}, {"se_goodman", goodman},
        {"z", zstat}, {"p", z_twosided_pvalue(zstat)},
        {"ci_lo", indirect - zc * sobel}, {"ci_hi", indirect + zc * sobel},
        {"bootstrap", {{"reps", reps}, {"seed", seed},
                       {"ci_lo_pct", pctl(alpha / 2)}, {"ci_hi_pct", pctl(1 - alpha / 2)},
                       {"ci_lo_bc", pctl(med_ncdf(2 * z0 - zq))}, {"ci_hi_bc", pctl(med_ncdf(2 * z0 + zq))}}},
    };
    emit(out, opt(a, "--out"));
    return 0;
}

// nlcom: nonlinear combinations of fitted coefficients with delta-method SE.
// Var(g) = G' V G, G = numeric central-difference Jacobian at beta-hat (matches
// Stata nlcom/testnl). t(resid-dof) [or t(G-1) under cluster] like regress.
// Both entry forms of §31 apply here too — see cmd_lincom's note. The delta-method
// Jacobian is taken at the STORED beta and contracted with the STORED V, so a
// handle-addressed nlcom reproduces the fitting one exactly.
// BUG-333: shared --level parse for lincom/nlcom. Range matches every other
// cobra surface ([10, 99.99], cmds_mle.cpp:296) so a user sees one rule.
// Returns false and prints on a bad value; the caller returns 2.
bool postest_level(const std::vector<std::string>& a, const char* who, double* level) {
    *level = 95.0;
    if (!has(a, "--level")) return true;
    *level = std::stod(opt(a, "--level", "95"));
    if (*level < 10.0 || *level > 99.99) {
        std::fprintf(stderr, "%s: --level must be in [10, 99.99] (got %s).\n", who,
                     opt(a, "--level").c_str());
        return false;
    }
    return true;
}

// A combination with exactly zero variance (an exact `regress` fit posts V = 0):
// Stata's lincom/nlcom keep r(se) 0 and print t, P>|t| and the interval as `.`
// (StataBE after `regress yx x` with yx = 3 + 2x: `lincom x` r(se) 0, r(t) r(p)
// r(lb) r(ub) missing; `nlcom _b[x]*2` the same row). The t 0 / p 1 / lb = ub = b
// this engine used to post there is a number Stata never shows.
static void postest_zero_se_missing(json& row) {
    row["t"] = nullptr;
    row["p"] = nullptr;
    row["ci_lo"] = nullptr;
    row["ci_hi"] = nullptr;
}

int cmd_nlcom(const std::vector<std::string>& a) {
    PostestModel posted;  // posted-matrix-wald: `--posted FILE` (load_posted_model)
    const PostestModel* stored = load_posted_model(a, posted) ? &posted : serve_active_model();
    PostestModel fitted;
    postest::FactorLevels levels;  // full-specification path only (resolve_coef)
    if (!stored) {
        std::string data = opt(a, "--data"), y = opt(a, "--y"), xcsv = opt(a, "--x");
        if (data_missing(data) || y.empty() || xcsv.empty()) {
            std::fprintf(stderr,
                "usage: cobra nlcom --data f.hbin --y Y --x X1,X2,... [--fe F] [--vcov ...] [--cluster C] "
                "[--model logit|probit|cloglog|poisson] [--no-intercept] "
                "[--keepif EXPR] [--level P] --expr \"name=f(_b[X1],_b[X2],..)\" [--expr ...]\n"
                "  e.g. quadratic turning point:  --expr \"turn=-_b[time]/(2*_b[time2])\"\n");
            return 2;
        }
        if (int rc = postest_refit(a, data, y, xcsv, fitted, &levels)) return rc;  // BUG-203
        serve_publish_model(fitted);
    }
    const PostestModel& M = stored ? *stored : fitted;
    const postest::FactorLevels* lv = stored ? nullptr : &levels;

    std::vector<std::string> exprs = opt_all(a, "--expr");
    if (exprs.empty()) { std::fprintf(stderr, "nlcom: need at least one --expr\n"); return 2; }

    const double dof = postest_dof(M);   // BUG-203: 0 (normal reference) on an ML fit
    double nlcom_level = 95.0;
    if (!postest_level(a, "nlcom", &nlcom_level)) return 2;
    const double tc = postest_crit(M, dof, nlcom_level);

    std::vector<NlcomTerm> terms;
    try { terms = nlcom_apply(M.names, M.beta, M.vcov_matrix, exprs, lv); }
    catch (const NlcomNeedsSample& ex) { std::fprintf(stderr, "%s\n", ex.what()); return 3; }
    catch (const std::exception& ex) { std::fprintf(stderr, "%s\n", ex.what()); return 2; }

    json arr = json::array();
    for (const auto& tm : terms) {
        double t = (tm.se > 0) ? tm.est / tm.se : 0.0;
        arr.push_back({{"name", tm.name}, {"b", tm.est}, {"se", tm.se}, {"t", t}, {"df", dof},
                       {"p", (tm.se > 0) ? postest_pvalue(M, t, dof) : 1.0},
                       {"ci_lo", tm.est - tc * tm.se}, {"ci_hi", tm.est + tc * tm.se}});
        if (tm.se == 0.0) postest_zero_se_missing(arr.back());
    }
    // FU-410: the combinations' covariance G V G' (Stata's r(V) after nlcom), m x m in
    // the order of `coefs`; its diagonal is each se squared.
    const std::vector<double> cov = nlcom_covariance(terms, M.vcov_matrix);
    json Vm = json::array();
    for (size_t p = 0; p < terms.size(); ++p) {
        json row = json::array();
        for (size_t q = 0; q < terms.size(); ++q) row.push_back(cov[p * terms.size() + q]);
        Vm.push_back(std::move(row));
    }
    json out = {{"estimator", "nlcom"}, {"coefs", arr},
                {"vcov_type", M.vcov_type}, {"n_obs", M.n}, {"df", dof}, {"V", Vm}};
    emit(out, opt(a, "--out"));
    return 0;
}

// lincom — linear combination of fitted coefficients (exact analytic SE).
//
// TWO ways in, ONE arithmetic tail (contract_A §31):
//   (a) the full model specification (--data/--y/--x/--fe/--vcov/--cluster) — reads the
//       dataset and FITS, exactly as it always has; then publishes the fit so a serve
//       session can mint a handle for it;
//   (b) a session model handle (a `postest` serve request) — the stored fit's own beta
//       and vcov answer the combination directly. NO dataset read, NO re-fit.
// Form (b) exists because form (a) was re-reading a 164 MB dataset and re-running HDFE
// to answer one r'β that had already been computed: 54 calls x ~0.25 s per dependent
// variable in the real analysis pass.
//
// The tail below is literally shared by both forms, so a handle-addressed answer is
// byte-identical to the re-fitting one by construction rather than by re-verification.
int cmd_lincom(const std::vector<std::string>& a) {
    std::vector<std::string> exprs = opt_all(a, "--exp");
    PostestModel posted;  // posted-matrix-wald: `--posted FILE` (load_posted_model)
    // non-null only under `postest` (or --posted)
    const PostestModel* stored = load_posted_model(a, posted) ? &posted : serve_active_model();
    PostestModel fitted;
    postest::FactorLevels levels;  // full-specification path only (resolve_coef)
    if (!stored) {
        std::string data = opt(a, "--data"), y = opt(a, "--y"), xcsv = opt(a, "--x");
        if (data_missing(data) || y.empty() || xcsv.empty() || exprs.empty()) {
            std::fprintf(stderr,
                "usage: cobra lincom --data f.hbin --y Y --x X1,X2,... [--fe F] [--vcov ...] [--cluster C] "
                "[--model logit|probit|cloglog|poisson] [--no-intercept] "
                "[--keepif EXPR] [--level P] --exp \"X1 + X2\" [--exp \"2*X1 - X3\"]\n"
                "  linear combination r'β with exact SE sqrt(r'Vr); bare names = coefficients, _cons = intercept.\n");
            return 2;
        }
        if (int rc = postest_refit(a, data, y, xcsv, fitted, &levels)) return rc;  // BUG-203
        serve_publish_model(fitted);  // frame-mode only; no-op one-shot / stateless §25
    } else if (exprs.empty()) {
        std::fprintf(stderr, "lincom: --exp is required.\n");
        return 2;
    }
    // The model spec flags are deliberately NOT read on the handle path, so passing
    // both a handle and (say) --vcov is caught by the E6 unconsumed-flag net as an
    // honest error instead of one of the two silently winning.
    const PostestModel& M = stored ? *stored : fitted;
    const int k = static_cast<int>(M.names.size());
    // BUG-203: 0 (normal reference) on an ML fit; FU-439: N - k after a multiway regress
    const double dof = postest_lincom_dof(M);
    double lincom_level = 95.0;
    if (!postest_level(a, "lincom", &lincom_level)) return 2;
    const double tc = postest_crit(M, dof, lincom_level);

    json arr = json::array();
    for (const std::string& e : exprs) {
        // The shared linear grammar (postest::LinearParser): the same one `test`
        // reads constraints with, so `2.c - 3.c`, `_b[2.c] + _b[x1]` and the omitted
        // base level `0.c` mean the same thing on both surfaces.
        postest::LinearForm lf;
        std::string err;
        bool needs_sample = false;
        postest::LinearParser lp(e, M.names, stored ? nullptr : &levels);
        if (!lp.parse(lf, err, needs_sample)) {
            // The structured error must be stderr's only text: a plain line
            // before it made PIET read `lincom nosuch` as rc 1, not Stata's
            // r(111) (test's not_found prints nothing first; StataBE
            // uni-oracle/hz.do Z6 `lincom weight` with two candidates is 111).
            if (err.size() > 10 && err.compare(err.size() - 10, 10, " not found") == 0)
                throw EngineError("coef_not_found", err, err.substr(0, err.size() - 10));
            std::fprintf(stderr, "lincom: %s\n", err.c_str());
            if (needs_sample) return 3;
            return 2;
        }
        const std::vector<double>& w = lf.w;
        const double offset = lf.konst;
        double est = offset, var = 0;
        for (int p = 0; p < k; ++p) est += w[p] * M.beta[p];
        // r'Vr against the model's OWN covariance — the fitted hc1/cluster/multiway V,
        // never a re-derived or defaulted one (BS-098 / BS-013).
        for (int p = 0; p < k; ++p) {
            double row = 0;
            for (int q = 0; q < k; ++q) row += w[q] * M.vcov_matrix[static_cast<size_t>(q) * k + p];
            var += w[p] * row;
        }
        double se = std::sqrt(std::max(0.0, var));
        double t = (se > 0) ? est / se : 0.0;
        arr.push_back({{"exp", e}, {"b", est}, {"se", se}, {"t", t}, {"df", dof},
                       {"p", (se > 0) ? postest_pvalue(M, t, dof) : 1.0},
                       {"ci_lo", est - tc * se}, {"ci_hi", est + tc * se}});
        if (se == 0.0) postest_zero_se_missing(arr.back());
    }
    json out = {{"estimator", "lincom"}, {"coefs", arr}, {"vcov_type", M.vcov_type},
                {"n_obs", M.n}, {"df", dof}};
    emit(out, opt(a, "--out"));
    return 0;
}

// Both entry forms of §31 apply here too — see cmd_lincom's note.
int cmd_test(const std::vector<std::string>& a) {
    PostestModel posted;  // posted-matrix-wald: `--posted FILE` (load_posted_model)
    const PostestModel* stored = load_posted_model(a, posted) ? &posted : serve_active_model();
    PostestModel fitted;
    postest::FactorLevels levels;  // full-specification path only (resolve_coef)
    if (!stored) {
        std::string data = opt(a, "--data"), y = opt(a, "--y"), xcsv = opt(a, "--x");
        if (data_missing(data) || y.empty() || xcsv.empty()) {
            std::fprintf(stderr,
                "usage: cobra test --data f.hbin --y Y --x X1,X2,... "
                "[--fe F] [--vcov iid|hc1|cluster|twoway] [--cluster C] "
                "[--model logit|probit|cloglog|poisson] [--no-intercept] [--keepif EXPR] "
                "(--joint a,b,c | --constraint \"a=b\" [--constraint \"a+b=1\" ...])\n");
            return 2;
        }
        if (int rc = postest_refit(a, data, y, xcsv, fitted, &levels)) return rc;  // BUG-203
        serve_publish_model(fitted);
    }
    const PostestModel& M = stored ? *stored : fitted;

    // The restrictions in the order they were given, --joint lists and --constraint
    // equations interleaved: Stata numbers them that way, in its "( 1)" lines and in
    // r(dropped_#), and on a tie keeps the earlier one (wald_dropped_rows).
    opt_all(a, "--joint");
    opt_all(a, "--constraint");
    std::vector<std::pair<bool, std::string>> items;
    for (size_t i = 0; i + 1 < a.size(); ++i) {
        if (a[i] == "--joint") {
            for (const auto& nm : split_csv(a[i + 1])) items.push_back({true, nm});
            ++i;
        } else if (a[i] == "--constraint") {
            items.push_back({false, a[i + 1]});
            ++i;
        }
    }
    std::vector<double> R, q; int J = 0; std::string err;
    if (int brc = build_restrictions(M.names, items, stored ? nullptr : &levels,
                                     R, q, J, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return brc;
    }
    // BUG-1276: Stata's test drops every restriction that is not independent of the
    // others under V ("Constraint k dropped", r(drop) 1, r(dropped_#)) and tests the
    // rest: collinear constraints (`test (x1=0) (2*x1=0)`), the omitted base level
    // (`test 1.c`), and every constraint when V = 0 (`logit y x1 xs, asis`, or a
    // posted V = 0), which leaves chi2(0) = . (StataBE 19.5, 2026-11-03; the rule is
    // wald_dropped_rows'). BUG-1231: after a classic least-squares fit (regress/areg/
    // xtreg,fe, V = s^2 (X'X)^-1) the rule's absolute 1e-19 floor sits at 1e-19 * s^2,
    // so it runs on V / s^2 (scratch-laneA/agents/eng-ml/x3.log F1/F2/F3, x4.log A1/X1:
    // `test 1e-8*x1 = 0` after `regress y x1 x2` drops; every other covariance, x3.log
    // F4-F7, keeps the floor on R V R' itself). Only regress's exact fit keeps its
    // constraints, F(1, 48) = .: its V is s^2 (X'X)^-1 with s^2 = 0, and the rule runs
    // under (X'X)^-1 instead (StataBE: on rank-0 fits, `test (x=0) (z=0)
    // (x+0.5*z=0)` drops 3 and `(x=0) (z=0) (x+2*z=0)` drops 2, as on the same
    // design with noise, where R R' would drop 1 and 2). Without that bread, only
    // its zero rows go.
    const int k = static_cast<int>(M.names.size());
    const bool from_posted = stored == &posted;
    const bool exact_ls = !M.asymptotic && !from_posted &&
        std::all_of(M.vcov_matrix.begin(), M.vcov_matrix.end(), [](double v) { return v == 0.0; });
    std::vector<int> drop;
    if (exact_ls && M.exact_fit_bread.size() == M.vcov_matrix.size()) {
        drop = wald_dropped_rows(M.exact_fit_bread.data(), k, R.data(), J);
    } else if (exact_ls) {
        for (int r = 0; r < J; ++r)
            if (std::all_of(R.begin() + static_cast<std::ptrdiff_t>(r) * k,
                            R.begin() + static_cast<std::ptrdiff_t>(r + 1) * k,
                            [](double v) { return v == 0.0; }))
                drop.push_back(r);
    } else if (M.ls_scale > 0.0) {
        std::vector<double> Vs(M.vcov_matrix);
        for (double& v : Vs) v /= M.ls_scale;
        drop = wald_dropped_rows(Vs.data(), k, R.data(), J);
    } else {
        drop = wald_dropped_rows(M.vcov_matrix.data(), k, R.data(), J);
    }
    std::vector<int> dropped;  // 1-based, Stata's r(dropped_#)
    if (!drop.empty()) {
        std::vector<double> Rk, qk;
        for (int r = 0, d = 0; r < J; ++r) {
            if (d < static_cast<int>(drop.size()) && drop[static_cast<size_t>(d)] == r) {
                dropped.push_back(r + 1);
                ++d;
                continue;
            }
            Rk.insert(Rk.end(), R.begin() + static_cast<std::ptrdiff_t>(r) * k,
                      R.begin() + static_cast<std::ptrdiff_t>(r + 1) * k);
            qk.push_back(q[static_cast<size_t>(r)]);
        }
        R.swap(Rk);
        q.swap(qk);
        J = static_cast<int>(q.size());
    }
    WaldResult w;
    w.chi2 = w.F = std::nan("");  // nothing left to test: chi2(0) = .
    if (J > 0) {
        w = wald_test(M.beta.data(), M.vcov_matrix.data(), k, R.data(), q.data(), J);
        if (!w.ok) { std::fprintf(stderr, "test: %s\n", w.error.c_str()); return 1; }
    }
    // Same cluster (G-1) branch as lincom/nlcom's `dof` above: when the fitted
    // model's VCE is clustered, Stata's F-reference dof is F(J, G-1), not the
    // residual dof. (No eval golden pinned the old dof_resid-unconditional
    // behavior under a clustered fit — grepped tests/ before changing; every
    // pinned `test` case is vcov_type "iid" with n_clusters 0/absent, so those
    // stay byte-identical here.)
    double df2 = postest_dof(M);
    json restr = json::array();
    for (const auto& [is_joint, text] : items) restr.push_back(is_joint ? text + " = 0" : text);
    // BUG-203: an ML fit has no residual dof, so F(J, df2) has no reference
    // distribution to be read against — Stata prints chi2(J) there and nothing
    // else. Emitting an F with df2 = 0 would be a number no consumer could use
    // honestly, so the F/df2 triple is simply absent on that path (both
    // consumers are `contains`-guarded: PIET's kPassthrough copy, and
    // eval/macros.cpp's r() map, whose "F or chi2" guard already anticipates a
    // chi2-only block). The least-squares branch keeps its historical key ORDER
    // as well as its values — `json` here is nlohmann::ordered_json, so the
    // emitted bytes are unchanged for every existing caller.
    json out = M.asymptotic
        ? json{
            {"estimator", "test"},
            {"restrictions", restr},
            {"J", J}, {"df1", J},
            {"chi2", w.chi2}, {"p_chi2", chi2_pvalue(w.chi2, J)},
            {"vcov_type", M.vcov_type}, {"n_obs", M.n},
          }
        : json{
            {"estimator", "test"},
            {"restrictions", restr},
            {"J", J}, {"df1", J}, {"df2", df2},
            {"chi2", w.chi2}, {"p_chi2", chi2_pvalue(w.chi2, J)},
            {"F", w.F}, {"p_F", J > 0 ? json(f_pvalue(w.F, J, df2)) : json(nullptr)},
            {"vcov_type", M.vcov_type}, {"n_obs", M.n},
          };
    // Only when something WAS dropped, so every existing caller's bytes are unchanged.
    if (!dropped.empty()) {
        out["dropped"] = static_cast<int>(dropped.size());
        out["dropped_rows"] = dropped;
    }
    emit(out, opt(a, "--out"));
    return 0;
}

int cmd_chow(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), y = opt(a, "--y"), xcsv = opt(a, "--x");
    std::string by = opt(a, "--by");
    if (data_missing(data) || y.empty() || xcsv.empty() || by.empty()) {
        std::fprintf(stderr,
            "usage: cobra chow --data f.hbin --y Y --x X1,X2 --by GROUP "
            "[--vcov hc1|cluster --cluster C] [--keepif EXPR] [--out f.json]\n"
            "  classic homoskedastic SSR F + heteroskedasticity-robust Wald on interactions\n");
        return 2;
    }
    data = maybe_keepif(data, opt(a, "--keepif"));
    const Dataset& ds = acquire_data(data);
    int64_t n_raw = ds.n_rows();
    const double* yraw = require_num(ds, "chow", y);
    const double* graw = require_num(ds, "chow", by);
    auto xs = split_csv(xcsv);
    int kx = static_cast<int>(xs.size());
    std::vector<const double*> xraw(kx);
    for (int c = 0; c < kx; ++c) xraw[c] = require_num(ds, "chow", xs[c]);
    // shared listwise row set over (y, group, x...)
    std::vector<const double*> all = {yraw, graw};
    for (auto* p : xraw) all.push_back(p);
    auto keep = listwise_keep(all, n_raw);
    if (keep.empty()) { std::fprintf(stderr, "chow: no rows after dropping missing.\n"); return 1; }
    int64_t n = static_cast<int64_t>(keep.size());
    auto yv = gather_rows(yraw, keep);
    auto gv = gather_rows(graw, keep);
    std::vector<std::vector<double>> xv(kx);
    for (int c = 0; c < kx; ++c) xv[c] = gather_rows(xraw[c], keep);
    // distinct groups in first-appearance order
    std::vector<double> levels;
    std::vector<int> gcode(static_cast<size_t>(n));
    for (int64_t i = 0; i < n; ++i) {
        double gval = gv[static_cast<size_t>(i)];
        int code = -1;
        for (size_t l = 0; l < levels.size(); ++l) if (levels[l] == gval) { code = static_cast<int>(l); break; }
        if (code < 0) { code = static_cast<int>(levels.size()); levels.push_back(gval); }
        gcode[static_cast<size_t>(i)] = code;
    }
    int G = static_cast<int>(levels.size());
    if (G < 2) { std::fprintf(stderr, "chow: --by '%s' has < 2 groups.\n", by.c_str()); return 2; }
    int kc = kx + 1;  // regressors incl. intercept

    // Helper: build a DesignMatrix [x..., 1] for a row subset and OLS-fit it, return SSR.
    auto fit_ssr = [&](const std::vector<int64_t>& rows, bool& ok) -> double {
        int64_t m = static_cast<int64_t>(rows.size());
        if (m <= kc) { ok = false; return 0.0; }
        DesignMatrix dm; dm.n = m; dm.k = kc; dm.X.assign(static_cast<size_t>(m) * kc, 0.0);
        dm.y.assign(static_cast<size_t>(m), 0.0);
        for (int c = 0; c < kx; ++c) { dm.xnames.push_back(xs[static_cast<size_t>(c)]);
            for (int64_t i = 0; i < m; ++i) dm.X[static_cast<size_t>(c) * m + i] = xv[static_cast<size_t>(c)][static_cast<size_t>(rows[static_cast<size_t>(i)])]; }
        dm.xnames.push_back("(intercept)");
        for (int64_t i = 0; i < m; ++i) { dm.X[static_cast<size_t>(kx) * m + i] = 1.0; dm.y[static_cast<size_t>(i)] = yv[static_cast<size_t>(rows[static_cast<size_t>(i)])]; }
        OlsResult r = ols_fit(dm);
        ok = true; return r.ssr;
    };
    std::vector<int64_t> allrows(static_cast<size_t>(n));
    for (int64_t i = 0; i < n; ++i) allrows[static_cast<size_t>(i)] = i;
    bool ok_p = true;
    double ssr_p = fit_ssr(allrows, ok_p);
    // One bucketing pass replaces one full-column rescan per group. Each bucket ends up
    // in ascending row order — exactly the order the per-group scan produced — so
    // fit_ssr sees the same rows in the same sequence and returns the same SSR bit for
    // bit. Rows whose group code is out of range are excluded here just as the old
    // `gcode[i] == g` test excluded them.
    std::vector<std::vector<int64_t>> grp_rows(static_cast<size_t>(G));
    for (int64_t i = 0; i < n; ++i) {
        int gi = gcode[static_cast<size_t>(i)];
        if (gi >= 0 && gi < G) grp_rows[static_cast<size_t>(gi)].push_back(i);
    }
    double ssr_g = 0; bool ok_g = true; std::vector<int> gsize(static_cast<size_t>(G), 0);
    for (int g = 0; g < G; ++g) {
        const std::vector<int64_t>& rows = grp_rows[static_cast<size_t>(g)];
        gsize[static_cast<size_t>(g)] = static_cast<int>(rows.size());
        bool ok; double s = fit_ssr(rows, ok);
        if (!ok) { ok_g = false; } else ssr_g += s;
    }
    json classic;
    if (ok_p && ok_g) {
        double df1 = static_cast<double>((G - 1) * kc);
        double df2 = static_cast<double>(n - static_cast<int64_t>(G) * kc);
        double F = ((ssr_p - ssr_g) / df1) / (ssr_g / df2);
        classic = {{"F", F}, {"df1", df1}, {"df2", df2}, {"p", f_pvalue(F, df1, df2)},
                   {"ssr_pooled", ssr_p}, {"ssr_groups", ssr_g}, {"ok", true}};
    } else {
        classic = {{"ok", false}, {"error", "a group has too few observations (n_g <= k)"}};
    }

    // Robust: augmented design [x..., (G-1) group dummies, (G-1)*kx interactions, 1],
    // feols (no FE) with --vcov, Wald-test all dummy+interaction terms jointly = 0.
    json robust;
    {
        std::string vcov_type = opt(a, "--vcov", "hc1");
        int n_extra = (G - 1) * (kx + 1);          // dummies + interactions
        int kk = kx + n_extra + 1;                  // + intercept
        DesignMatrix dm; dm.n = n; dm.k = kk;
        dm.X.assign(static_cast<size_t>(n) * kk, 0.0); dm.y.assign(static_cast<size_t>(n), 0.0);
        int col = 0;
        for (int c = 0; c < kx; ++c) { dm.xnames.push_back(xs[static_cast<size_t>(c)]);
            for (int64_t i = 0; i < n; ++i) dm.X[static_cast<size_t>(col) * n + i] = xv[static_cast<size_t>(c)][static_cast<size_t>(i)];
            ++col; }
        std::vector<int> test_cols;
        // Each dummy/interaction column is nonzero only on its own group's rows, and
        // dm.X was already assign()ed to 0.0, so writing just the members reproduces the
        // matrix the old row-wise ternary built — the same cells, the same values —
        // while touching O(n) cells per group instead of O(n) per group PER column.
        for (int g = 1; g < G; ++g) {  // baseline = group 0
            const std::vector<int64_t>& rows = grp_rows[static_cast<size_t>(g)];
            dm.xnames.push_back("g" + std::to_string(g));
            for (int64_t i : rows) dm.X[static_cast<size_t>(col) * n + i] = 1.0;
            test_cols.push_back(col); ++col;
            for (int c = 0; c < kx; ++c) {
                dm.xnames.push_back("g" + std::to_string(g) + "_x" + std::to_string(c));
                for (int64_t i : rows) dm.X[static_cast<size_t>(col) * n + i] =
                    xv[static_cast<size_t>(c)][static_cast<size_t>(i)];
                test_cols.push_back(col); ++col;
            }
        }
        dm.xnames.push_back("(intercept)");
        for (int64_t i = 0; i < n; ++i) { dm.X[static_cast<size_t>(col) * n + i] = 1.0; dm.y[static_cast<size_t>(i)] = yv[static_cast<size_t>(i)]; }
        ModelData md; md.dm = std::move(dm); md.has_intercept = true;
        if (vcov_type == "cluster" || vcov_type == "twoway") {
            // cluster needs the factor; fall back to hc1 unless user wired clusters via build_model — keep simple: hc1.
            vcov_type = "hc1";
        }
        FeolsResult r = feols_fit(md, parse_vcov(vcov_type));
        int J = static_cast<int>(test_cols.size());
        std::vector<double> Rm(static_cast<size_t>(J) * r.names.size(), 0.0);
        std::vector<double> qz(static_cast<size_t>(J), 0.0);
        for (int j = 0; j < J; ++j) Rm[static_cast<size_t>(j) * r.names.size() + test_cols[static_cast<size_t>(j)]] = 1.0;
        WaldResult w = wald_test(r.beta.data(), r.vcov_matrix.data(),
                                 static_cast<int>(r.names.size()), Rm.data(), qz.data(), J);
        double df2 = static_cast<double>(r.dof.dof_resid);
        if (w.ok)
            robust = {{"chi2", w.chi2}, {"p_chi2", chi2_pvalue(w.chi2, J)},
                      {"F", w.F}, {"df1", J}, {"df2", df2}, {"p_F", f_pvalue(w.F, J, df2)},
                      {"vcov_type", r.vcov_type}, {"ok", true}};
        else robust = {{"ok", false}, {"error", w.error}};
    }

    json out = {
        {"estimator", "chow"}, {"by", by}, {"G", G},
        {"group_sizes", gsize}, {"n_obs", n},
        {"classic", classic}, {"robust", robust},
    };
    emit(out, opt(a, "--out"));
    return 0;
}

// The fit and its covariance for the three GLM branches of cmd_margins below (the
// AME, dydx() at at() points, and at() predictive margins): beta, and V under
// `vce` (iid|robust|cluster) -- logit's sandwich on its bread (the score scaled by
// the raw pweight when `wptr` is set), probit's robust sandwich on the observed
// information, poisson's fepois_fit under HC1 or the cluster. One definition: the
// at() branches used to fit OIM only and drop the fit's vce, so `margins, dydx(x)
// at(c1=(1 3))` after `poisson yc x c1, vce(robust)` answered the OIM se at rc 0
// where Stata's is the robust one (w2v_margins_siblings.log:2871-2921
// F_robust_dydx_at_po, `Model VCE: Robust`).
struct MlMarginsFit {
    std::vector<double> beta, V;
    GlmLink link = GlmLink::Logit;
    int n_clusters = 0;
    double dof = 0;          // the reported "df": logit/probit N-k, poisson's residual dof
    bool separated = false;  // logit/probit: the outcome is perfectly predicted (beta/V unset)
};

// `mo`: the logit/probit fit's maximize options (FU-426, refit_ml_max_options).
// `asis`: the fit was `asis` and posted its separating fit at rc 0 (cmd_logit /
// cmd_probit), so margins answers from it too, as postest_fit_glm's refit does
// (BUG-1242: StataBE `logit y x1 xs, asis` with xs separating y completely, then
// `margins, dydx(x1)` 2.7e-14 and `margins, at(x1=1)` .535 = mean(y), rc 0).
MlMarginsFit ml_margins_fit(const std::string& model, const GlmInputs& g, const std::string& vce, bool asis,
                            const double* wptr, const MlMaxOptions& mo) {
    MlMarginsFit f;
    if (model == "logit") {
        f.link = GlmLink::Logit;
        LogitResult r = refit_logit(g, mo, wptr);
        if (r.separated_perfect && !asis) { f.separated = true; return f; }
        f.beta = r.beta;
        f.V = r.bread;
        f.dof = static_cast<double>(g.n - g.k);
        // Sandwich VCE (mirrors cmd_logit exactly, see cmds_mle.cpp cmd_logit):
        // meat's per-row score e_i = (y_i - p_i), pre-scaled by the raw pweight
        // when present (the pweight-correct meat, since bread already folded
        // the same weight into IRLS — logit is a canonical link, no BUG-140
        // observed/expected-info split needed).
        if (vce == "robust" || vce == "cluster") {
            std::vector<double> e(static_cast<size_t>(g.n));
            for (int64_t i = 0; i < g.n; ++i) {
                double ei = g.y[static_cast<size_t>(i)] - r.p[static_cast<size_t>(i)];
                if (wptr) ei *= wptr[static_cast<size_t>(i)];
                e[static_cast<size_t>(i)] = ei;
            }
            std::vector<double> meat(static_cast<size_t>(g.k) * g.k, 0.0);
            double factor;
            if (vce == "robust") {
                for (int64_t i = 0; i < g.n; ++i) {
                    double ei = e[static_cast<size_t>(i)];
                    double e2 = ei * ei;
                    for (int c = 0; c < g.k; ++c) {
                        double xc = g.X[static_cast<int64_t>(c) * g.n + i];
                        for (int d = 0; d < g.k; ++d)
                            meat[static_cast<size_t>(c) * g.k + d] += e2 * xc * g.X[static_cast<int64_t>(d) * g.n + i];
                    }
                }
                factor = g.n > 1 ? static_cast<double>(g.n) / static_cast<double>(g.n - 1) : 1.0;
            } else {
                cecon::cluster_meat(g.X.data(), g.n, g.k, e.data(), g.cluster[0], meat.data(), &f.n_clusters);
                if (f.n_clusters < 2)
                    throw EngineError("bad_input",
                        "margins: vce(cluster) requires at least 2 clusters, but the cluster "
                        "variable has " + std::to_string(f.n_clusters));
                factor = static_cast<double>(f.n_clusters) / static_cast<double>(f.n_clusters - 1);
            }
            std::vector<double> tmp(static_cast<size_t>(g.k) * g.k, 0.0);
            f.V.assign(static_cast<size_t>(g.k) * g.k, 0.0);
            la::sandwich(r.bread.data(), meat.data(), g.k, factor, f.V.data(), tmp.data());
        }
    } else if (model == "probit") {
        f.link = GlmLink::Probit;
        ProbitResult r = refit_probit(g, mo);
        if (r.separated_perfect && !asis) { f.separated = true; return f; }
        f.beta = r.beta;
        f.dof = static_cast<double>(g.n - g.k);
        // BUG-210: the iid branch used to take the Fisher bread r.V here ("matches
        // the OLD margins_probit's V convention", i.e. the PRE-BUG-140 one), while
        // only the vce(robust) branch below built the observed information. probit's
        // link is non-canonical, so that made `margins, dydx()` after probit report
        // se .037501556827 where Stata reports .038064606025 — 1.5% narrow on every
        // CI on this path, with the point estimate correct, which is the signature of
        // a covariance-source mismatch rather than a fitting difference. Both branches
        // now read the ONE observed-information definition in src/estimators/probit.h.
        ProbitObservedInfo oi = probit_observed_info(r, g.X.data(), g.n, g.k, g.y.data());
        f.V = oi.bread;
        if (vce == "robust") {
            // Robust sandwich on that same observed-info bread (mirrors cmd_probit's
            // own vce(robust), see cmds_mle.cpp) with Stata's ML multiplier N/(N-1).
            double rfac = g.n > 1 ? static_cast<double>(g.n) / static_cast<double>(g.n - 1) : 1.0;
            std::vector<double> tmp(static_cast<size_t>(g.k) * g.k, 0.0);
            f.V.assign(static_cast<size_t>(g.k) * g.k, 0.0);
            la::sandwich(oi.bread.data(), oi.meat.data(), g.k, rfac, f.V.data(), tmp.data());
        }
    } else if (model == "cloglog") {
        // cmd_cloglog's fit (cloglog_ml) and its V: the OIM bread, or the sandwich on
        // clog_lf's score scaled by the pweight (cmd_cloglog's robust/cluster rule).
        // The start's constant-only z is the screened sample's; it only starts a
        // constant-only model, whose margins have no slope.
        f.link = GlmLink::Cloglog;
        double sw = 0.0, sw0 = 0.0;
        for (int64_t i = 0; i < g.n; ++i) {
            const double wi = wptr ? wptr[static_cast<size_t>(i)] : 1.0;
            sw += wi;
            if (g.y[static_cast<size_t>(i)] == 0.0) sw0 += wi;
        }
        const bool has_constant = !g.names.empty() && g.names.back() == "(intercept)";
        // cloglog_ml reads g's own weights, which every caller's wptr points into
        const BinaryMlResult r = cloglog_ml(g, has_constant, std::log(std::log(sw / sw0)), mo);
        f.beta = r.beta;
        f.V = r.bread;
        f.dof = static_cast<double>(g.n - g.k);
        if (vce == "robust" || vce == "cluster") {
            std::vector<double> eta(static_cast<size_t>(g.n));
            la::mat_vec(g.X.data(), g.n, g.k, r.beta.data(), eta.data());
            std::vector<double> e(static_cast<size_t>(g.n));
            for (int64_t i = 0; i < g.n; ++i) {
                const double z = eta[static_cast<size_t>(i)];
                double ei = g.y[static_cast<size_t>(i)] != 0.0
                    ? std::exp(z - std::exp(z)) / (-std::expm1(-std::exp(z))) : -std::exp(z);
                if (wptr) ei *= wptr[static_cast<size_t>(i)];
                e[static_cast<size_t>(i)] = ei;
            }
            std::vector<double> meat(static_cast<size_t>(g.k) * g.k, 0.0);
            double factor;
            if (vce == "robust") {
                for (int64_t i = 0; i < g.n; ++i) {
                    const double e2 = e[static_cast<size_t>(i)] * e[static_cast<size_t>(i)];
                    for (int c = 0; c < g.k; ++c) {
                        const double xc = g.X[static_cast<int64_t>(c) * g.n + i];
                        for (int d = 0; d < g.k; ++d)
                            meat[static_cast<size_t>(c) * g.k + d] += e2 * xc * g.X[static_cast<int64_t>(d) * g.n + i];
                    }
                }
                factor = g.n > 1 ? static_cast<double>(g.n) / static_cast<double>(g.n - 1) : 1.0;
            } else {
                cecon::cluster_meat(g.X.data(), g.n, g.k, e.data(), g.cluster[0], meat.data(), &f.n_clusters);
                if (f.n_clusters < 2)
                    throw EngineError("bad_input",
                        "margins: vce(cluster) requires at least 2 clusters, but the cluster "
                        "variable has " + std::to_string(f.n_clusters));
                factor = static_cast<double>(f.n_clusters) / static_cast<double>(f.n_clusters - 1);
            }
            std::vector<double> tmp(static_cast<size_t>(g.k) * g.k, 0.0);
            f.V.assign(static_cast<size_t>(g.k) * g.k, 0.0);
            la::sandwich(r.bread.data(), meat.data(), g.k, factor, f.V.data(), tmp.data());
        }
    } else {
        f.link = GlmLink::Poisson;
        ModelData md; md.has_intercept = true; md.dm.n = g.n; md.dm.k = g.k;
        md.dm.X = g.X; md.dm.y = g.y; md.dm.xnames = g.names; md.keep = g.keep;
        md.weights = g.weights; md.weight_kind = g.weight_kind;
        Vcov vt = Vcov::IID;
        if (vce == "cluster") { md.cluster = g.cluster; vt = Vcov::Cluster; }
        else if (vce == "robust") { vt = Vcov::HC1; }
        FepoisResult r = fepois_fit(md, vt);
        f.beta = r.beta;
        f.V = r.vcov_matrix;
        f.n_clusters = r.n_clusters;
        f.dof = (r.n_clusters > 0) ? (r.n_clusters - 1.0) : static_cast<double>(r.dof.dof_resid);
    }
    return f;
}

// V[a][b] = J_a' V J_b for the rows' Jacobians (V column-major k*k, symmetric);
// the lower triangle mirrors the upper, so the result is symmetric to the bit.
json margins_cross_v(const std::vector<std::vector<double>>& jac, const std::vector<double>& V, int k) {
    const size_t m = jac.size();
    std::vector<double> cov(m * m, 0.0);
    for (size_t p = 0; p < m; ++p) {
        for (size_t q = p; q < m; ++q) {
            double v = 0;
            for (int c = 0; c < k; ++c) {
                double gv = 0;
                for (int d = 0; d < k; ++d)
                    gv += V[static_cast<size_t>(d) * k + c] * jac[q][static_cast<size_t>(d)];
                v += jac[p][static_cast<size_t>(c)] * gv;
            }
            cov[p * m + q] = v;
            cov[q * m + p] = v;
        }
    }
    json out = json::array();
    for (size_t p = 0; p < m; ++p) {
        json row = json::array();
        for (size_t q = 0; q < m; ++q) row.push_back(cov[p * m + q]);
        out.push_back(std::move(row));
    }
    return out;
}

int cmd_margins(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), y = opt(a, "--y"), xcsv = opt(a, "--x");
    std::string model = opt(a, "--model", "logit");
    // FU-541: the logit/probit fit was `asis`; else the GLM branches below fit the
    // design Stata's perfect-prediction screen leaves (binary_refit_screen).
    const bool asis = has(a, "--asis");
    // `--dydx VARS` (Stata `margins, dydx()`) SELECTS which fitted variables are
    // reported (P5/BUG-158-159): read unconditionally here (like every other
    // flag below) so it is always marked "consumed" regardless of which branch
    // below returns — the --at branches ignore it (predictive margins at a
    // point has no dydx() concept), the plain AME path at the bottom honors it.
    // Empty means "report every non-interaction main-effect/level column",
    // matching the exact old row set for a caller that never asks for one.
    std::string dydx_str = opt(a, "--dydx");
    // Weighted + cluster-robust AME (P5/BUG-158-159): --weight/--wtype mirror
    // `cobra logit`'s own pweight flags exactly (only pw is supported — Stata
    // has no other meaningful weight type for an ML AME); --vce/--cluster
    // mirror `cobra logit --vce cluster --cluster C`. Read unconditionally so
    // they are consumed even on the --at branches (which do not use them).
    std::string weight_var = opt(a, "--weight"), wtype = opt(a, "--wtype");
    std::string vce = opt(a, "--vce", "iid");
    std::string cluster_var = opt(a, "--cluster");
    if (data_missing(data) || y.empty() || xcsv.empty()) {
        std::fprintf(stderr,
            "usage: cobra margins --data f.hbin --y Y --x X1,X2 [--no-intercept] [--out f.json]\n"
            "  AME (default): --model logit|probit|cloglog|poisson  -> dydx + delta SE\n"
            "  predictive margins (linear): --at \"v1=val1,v2=val2\" [--at ...] [--fe F] [--vcov ..] [--cluster C]\n"
            "    per level of a factor: --marginlist VAR (instead of --at); [--atmeans] = float-stored means\n"
            "  predictive margins (GLM): --at ... --model logit|probit|cloglog|poisson -> average adjusted prediction\n");
        return 2;
    }
    // e(sample) T2 / FU-475: the fit's `--keepif` routed the way cmd_feols routes it
    // (plan_keepif), so a regressor list carrying a time-series operator keeps the
    // FULL data and the restriction becomes a row mask after the operators -- the
    // fit's own sample (w2_esample.log:751-775 C6 `regress y L.x1 x2 if t>=3`,
    // e(N) 40, against the 30 rows of the physically filtered copy). Only the
    // linear --at/--marginlist refit takes the mask; every other branch refuses it
    // below. Without a TS token (or tsset) this is maybe_keepif, byte-identical.
    const cecon::TsMeta tsm = parse_tsmeta(a);
    const std::vector<std::string> plan_xs = refit_xs(xcsv);
    const std::vector<std::string> plan_ys{y};  // BUG-1277: an operated depvar too
    const KeepifPlan kp = plan_keepif(data, opt(a, "--keepif"), &tsm, {&plan_xs, &plan_ys});
    data = kp.data;
    const Dataset& ds = acquire_data(data);

    // ---- Predictive margins at fixed covariate values (LINEAR model), `margins, at()` ----
    // A linear predictive margin at a point = x̄(at)'β where x̄(at) is the average design
    // row with the at()-listed covariates overridden (AsObserved means for the rest);
    // delta-method SE = sqrt(x̄(at)' V x̄(at)). Each --at "v1=val1,v2=val2,..." is one point.
    // An --at whose value is empty ("") overrides nothing: the one point is the mean
    // prediction over the estimation sample (Stata's bare `margins` after a linear fit).
    std::vector<std::string> atspecs = opt_all(a, "--at");
    // `--marginlist VAR` (Stata `margins VAR` / `margins i.VAR`): one point per level of
    // the factor VAR in the estimation sample, the level's indicator columns set to that
    // level and every other design column at its mean. A linear prediction averaged over
    // the sample IS the prediction at the sample means, so this is the predictive margin
    // (w2b_estim.log:529 margins_noatmeans_ctl). Read unconditionally (like every flag
    // here) so the non-linear branches below can refuse it instead of ignoring it.
    const std::string marginlist = opt(a, "--marginlist");
    // `--atmeans` (Stata `margins ..., atmeans`): the design columns no point overrides are
    // held at their means STORED AT FLOAT PRECISION. Stata's adjusted predictions do this:
    // w2b_estim.log:499 (_b[1.t] 12.7622444005381), :558 (12.67333339694155) are each
    // reproduced to every printed digit by x̄'β with float(x̄) -- including the factor
    // indicator means float(1/6) -- and are 3.9e-9 / 3.7e-9 (relative) away from the
    // double-mean value, which is the :529 / bare-margins number instead.
    const bool atmeans = has(a, "--atmeans");
    // `--emit-v`: the poisson fit's e(b)/e(V) for PIET's margins grid (see below).
    const bool emit_v = has(a, "--emit-v");
    // "flag present" vs "default" — opt(a,"--model","logit") always returns "logit" when
    // --model is omitted, so that default must not leak into the --at path (BUG: --at used
    // to silently compute the LINEAR margin even when --model logit|probit|poisson was passed).
    bool model_given = has(a, "--model");
    if (emit_v && (!model_given || model != "poisson"))
        throw EngineError("unsupported", "margins: --emit-v is implemented for --model poisson only");
    // FU-426: the logit/probit fit's maximize options, for every GLM branch below.
    MlMaxOptions ml_opts;
    // A missing --model is the logit AME below, except on the linear --at/--marginlist path.
    const bool binary_ml_refit = (model == "logit" || model == "probit" || model == "cloglog") &&
                                 (model_given || (atspecs.empty() && marginlist.empty()));
    if (!refit_ml_max_options(a, binary_ml_refit, "margins", ml_opts)) return 2;
    // FU-385: Stata regress's multiway cluster convention (BUG-848), so the linear
    // at() refit after `regress ..., vce(cluster a b)` carries the fit's e(V) and
    // not reghdfe's Gmin one. Only that branch fits feols (the --at points and the
    // --marginlist levels); anywhere else the flag would be ignored, so it is
    // refused (read unconditionally for the E6 net).
    const bool multiway_subset_factors = has(a, "--multiway-subset-factors");
    // The fit kept its singleton fixed-effect groups (areg, xtreg,fe, reghdfe's
    // keepsingletons; see postest_fit_spec). Only the linear --at/--marginlist
    // refit absorbs effects; anywhere else the flag would be ignored, so it is
    // refused (read unconditionally for the E6 net).
    const bool keep_singletons = has(a, "--keep-singletons");
    if (keep_singletons && ((atspecs.empty() && marginlist.empty()) || model_given))
        throw EngineError("bad_input",
            "margins: --keep-singletons requires the linear --at/--marginlist path (no --model)");
    // BUG-1067: the fit kept the earlier term of each collinear set (reghdfe, areg,
    // xtreg,fe). Same reach as --keep-singletons: only the linear refit reads it.
    const bool collinear_forward = has(a, "--collinear-forward");
    if (collinear_forward && ((atspecs.empty() && marginlist.empty()) || model_given))
        throw EngineError("bad_input",
            "margins: --collinear-forward requires the linear --at/--marginlist path (no --model)");
    if (multiway_subset_factors) {
        const std::string vt = opt(a, "--vcov", "iid");
        if ((atspecs.empty() && marginlist.empty()) || model_given ||
            (vt != "twoway" && vt != "multiway"))
            throw EngineError("bad_input",
                "margins: --multiway-subset-factors requires the linear --at path (no --model) "
                "and --vcov twoway or multiway");
    }
    // FU-131: the fit's `noconstant`. Every branch below refits (feols_fit on the
    // linear --at path, load_glm elsewhere), and without it each appended the
    // constant the fit never had (w2p_estim_ev.log:1372 G_nc_margins_at, :1628
    // G_pnc_margins_dydx). The GLM branches drop load_glm's trailing constant the
    // way cmd_logit/cmd_probit do; poisson has no such fit to answer for.
    const bool no_intercept = has(a, "--no-intercept");
    if (no_intercept && model_given && model == "poisson")
        throw EngineError("bad_input", "margins: --no-intercept is not supported with --model poisson");
    if ((!atspecs.empty() || !marginlist.empty()) && !model_given) {
        if (!marginlist.empty() && !atspecs.empty())
            throw EngineError("unsupported",
                "margins: --marginlist together with --at is not implemented");
        const std::string fe_csv = opt(a, "--fe");
        if (no_intercept && !split_csv(fe_csv).empty())
            throw EngineError("bad_input", "margins: --no-intercept is not valid with --fe");
        // Under --fe the slope columns below are read after feols_fit has demeaned them,
        // so their "means" are ~0, not the covariates' means. The two new forms refuse
        // there rather than extend that to a new surface.
        if ((atmeans || !marginlist.empty()) && !fe_csv.empty())
            throw EngineError("unsupported",
                "margins: --atmeans/--marginlist with --fe (absorbed effects) is not implemented");
        ModelData md = build_model(ds, y, refit_xs(xcsv), split_csv(fe_csv),
                                   split_csv(opt(a, "--cluster")), /*offset_name=*/"",
                                   /*drop_singleton=*/!keep_singletons,
                                   tsm.timevar.empty() ? nullptr : &tsm,
                                   kp.active ? &kp.mask : nullptr);
        md.collinear_forward = collinear_forward;
        FeolsResult r = feols_fit(md, parse_vcov(opt(a, "--vcov", "iid")), /*cluster_index=*/0,
                                  no_intercept, /*keep_nested_fe_dof=*/false,
                                  multiway_subset_factors);
        const int k = static_cast<int>(r.names.size());
        // md.dm.X holds only the md.dm.k design columns actually solved. With NO FE that
        // is k columns incl. a "(intercept)" ones column (mean = 1). Under FE, feols_fit
        // appends a reconstructed "_cons" row to r.names (GAP B / 3217393) so r.names.size()
        // == md.dm.k + 1, and md.dm.X has NO _cons column (and its slope cols are demeaned).
        // The intercept/_cons design value is the constant 1, so pin its mean to 1.0 rather
        // than reading md.dm.X out of bounds. Byte-identical for no-FE (ones-column mean==1).
        std::vector<double> mean(k, 0.0);  // observed column means of the design (intercept = 1)
        for (int j = 0; j < k; ++j) {
            if (r.names[static_cast<size_t>(j)] == "(intercept)" ||
                r.names[static_cast<size_t>(j)] == "_cons") { mean[j] = 1.0; continue; }
            if (j >= md.dm.k) { mean[j] = 0.0; continue; }  // defensive: no backing column
            const double* col = md.dm.X.data() + static_cast<int64_t>(j) * md.dm.n;
            double s = 0; for (int64_t i = 0; i < md.dm.n; ++i) s += col[i];
            mean[j] = s / static_cast<double>(md.dm.n);
            if (atmeans) mean[j] = static_cast<double>(static_cast<float>(mean[j]));
        }
        std::unordered_map<std::string, int> idx;
        for (int j = 0; j < k; ++j) idx[r.names[j]] = j;
        // Every point as (label, overrides), parsed before any is computed so the
        // covariance across points below can reuse the same design rows.
        struct AtPoint {
            std::string label;
            std::vector<std::pair<int, double>> set;
        };
        std::vector<AtPoint> points;
        for (const auto& spec : atspecs) {
            AtPoint pt;
            pt.label = spec;
            for (const auto& tok : split_csv(spec)) {
                size_t eq = tok.find('=');
                if (eq == std::string::npos) { std::fprintf(stderr, "margins --at: bad token '%s'\n", tok.c_str()); return 2; }
                std::string nm = trimx(tok.substr(0, eq));
                auto it = idx.find(nm);
                if (it == idx.end()) { std::fprintf(stderr, "margins --at: unknown covariate '%s'\n", nm.c_str()); return 2; }
                margins_refuse_derived_at(r.names, nm);
                try {
                    pt.set.emplace_back(it->second, std::stod(trimx(tok.substr(eq + 1))));
                } catch (const std::exception&) {
                    std::fprintf(stderr, "margins --at: '%s' has a non-numeric value.\n", tok.c_str());
                    return 2;
                }
            }
            points.push_back(std::move(pt));
        }
        if (!marginlist.empty()) {
            // The factor's level columns are `<level>.VAR` (BUG-101 naming; the base level
            // has no column). Any other column derived from VAR (an interaction) cannot be
            // set per level from the already-expanded design, and VAR itself as a column
            // means it entered the model as a continuous variable.
            std::map<long long, int> level_col;
            for (int j = 0; j < k; ++j) {
                const std::string& nm = r.names[static_cast<size_t>(j)];
                if (nm == marginlist)
                    throw EngineError("bad_input",
                        "factor " + marginlist + " not found in list of covariates");
                if (!margins_name_mentions_var(nm, marginlist)) continue;
                const size_t dot = nm.find('.');
                const std::string lv = nm.substr(0, dot);
                bool integer_level = !lv.empty() && nm.substr(dot + 1) == marginlist;
                for (size_t c = (integer_level && lv[0] == '-') ? 1 : 0; integer_level && c < lv.size(); ++c)
                    if (!std::isdigit(static_cast<unsigned char>(lv[c]))) integer_level = false;
                if (!integer_level || lv == "-")
                    throw EngineError("unsupported",
                        "margins: the design carries the term '" + nm + "' derived from " +
                        marginlist + "; per-level margins of " + marginlist +
                        " with that term are not implemented");
                level_col[std::stoll(lv)] = j;
            }
            if (level_col.empty())
                throw EngineError("bad_input",
                    "factor " + marginlist + " not found in list of covariates");
            const double* fv = require_num(ds, "margins", marginlist);
            std::set<long long> levels;
            for (int64_t row : md.keep) {
                const double v = fv[row];
                if (v != std::floor(v))
                    throw EngineError("bad_input",
                        marginlist + ":  factor variables may not contain noninteger values");
                levels.insert(static_cast<long long>(v));
            }
            int no_column = 0;
            for (long long lv : levels) no_column += level_col.count(lv) ? 0 : 1;
            if (no_column != 1)
                throw EngineError("unsupported",
                    "margins: the levels of " + marginlist + " in the estimation sample do not "
                    "map to its design columns (one base level expected)");
            for (long long lv : levels) {
                AtPoint pt;
                pt.label = std::to_string(lv) + "." + marginlist;
                for (const auto& [lc, col] : level_col) pt.set.emplace_back(col, lc == lv ? 1.0 : 0.0);
                points.push_back(std::move(pt));
            }
        }
        const double dof = (r.n_clusters > 0) ? (r.n_clusters - 1.0) : static_cast<double>(r.dof.dof_resid);
        const double tc = (dof > 0) ? t_critical(0.05, dof) : 0.0;
        json arr = json::array();
        std::vector<std::vector<double>> rows;
        for (const auto& pt : points) {
            std::vector<double> xb = mean;  // AsObserved means, then apply overrides
            for (const auto& [col, val] : pt.set) xb[col] = val;
            double mg = 0;
            for (int j = 0; j < k; ++j) mg += xb[j] * r.beta[j];
            double var = 0;
            for (int aa = 0; aa < k; ++aa) {
                double gv = 0;
                for (int bb = 0; bb < k; ++bb) gv += xb[bb] * r.vcov_matrix[aa + bb * k];
                var += xb[aa] * gv;
            }
            double se = (var > 0) ? std::sqrt(var) : 0.0;
            double t = (se > 0) ? mg / se : 0.0;
            arr.push_back({{"at", pt.label}, {"margin", mg}, {"se", se}, {"t", t}, {"df", dof},
                           {"p", (se > 0 && dof > 0) ? t_twosided_pvalue(t, dof) : 1.0},
                           {"ci_lo", mg - tc * se}, {"ci_hi", mg + tc * se}});
            rows.push_back(std::move(xb));
        }
        // The full delta-method covariance of the margins, V[p][q] = x̄_p' V x̄_q (Stata's
        // r(V): `margins t` has non-zero off-diagonals, live StataBE 19.5 2026-10-12,
        // el(r(V),1,2) .00007652). Its diagonal is each point's var above; the
        // lower triangle mirrors the upper, so it is symmetric to the bit.
        const size_t np = rows.size();
        std::vector<double> cov(np * np, 0.0);
        for (size_t p = 0; p < np; ++p) {
            for (size_t q = p; q < np; ++q) {
                double v = 0;
                for (int aa = 0; aa < k; ++aa) {
                    double gv = 0;
                    for (int bb = 0; bb < k; ++bb) gv += rows[q][bb] * r.vcov_matrix[aa + bb * k];
                    v += rows[p][aa] * gv;
                }
                cov[p * np + q] = v;
                cov[q * np + p] = v;
            }
        }
        json Vm = json::array();
        for (size_t p = 0; p < np; ++p) {
            json vrow = json::array();
            for (size_t q = 0; q < np; ++q) vrow.push_back(cov[p * np + q]);
            Vm.push_back(std::move(vrow));
        }
        json out = {{"estimator", "margins"}, {"type", "predictive"}, {"vcov_type", r.vcov_type},
                    {"n_obs", r.n}, {"df", dof}, {"margins", arr}, {"V", Vm}};
        emit(out, opt(a, "--out"));
        return 0;
    }
    // The GLM/AME branches below read every row of --data: under a TS --keepif that
    // is the unrestricted data, a different sample from the fit's.
    if (kp.active)
        throw EngineError("unsupported",
            "margins: a fit with time-series operators under --keepif is supported for the "
            "linear predictive margins (--at/--marginlist, no --model) only");
    // The two flags above belong to the linear predictive branch only; every branch
    // below would read past them, so they refuse here instead.
    if (atmeans || !marginlist.empty())
        throw EngineError("unsupported",
            "margins: --atmeans/--marginlist are implemented for the linear predictive "
            "margins (no --model) only");
    // The GLM at() branches below fit under the fit's own --vce/--cluster
    // (ml_margins_fit), validated as the AME branch validates them. They average
    // unweighted, so a weight refuses instead of being read and dropped.
    std::vector<std::string> at_cluster_names;
    if (!atspecs.empty() && model_given) {
        if (vce != "iid" && vce != "robust" && vce != "cluster") {
            std::fprintf(stderr, "margins: --vce must be one of iid|robust|cluster (got %s).\n", vce.c_str());
            return 2;
        }
        if (vce == "cluster" && cluster_var.empty()) {
            std::fprintf(stderr, "margins: --vce cluster requires --cluster C.\n");
            return 2;
        }
        if (model == "probit" && vce == "cluster") {
            std::fprintf(stderr, "margins(probit): --vce cluster is not supported; use --model logit.\n");
            return 2;
        }
        if (!weight_var.empty() || !wtype.empty())
            throw EngineError("unsupported",
                "margins: --weight/--wtype with --at --model is not implemented (the at() "
                "branches average unweighted)");
        if (!cluster_var.empty()) at_cluster_names.push_back(cluster_var);
    }
    // vce(cluster) needs two groups in the rows the fit used.
    auto at_clusters_ok = [&](const GlmInputs& g) {
        if (vce == "cluster" && (g.cluster.empty() || g.cluster[0].n_levels < 2))
            throw EngineError("bad_input",
                "margins: vce(cluster) requires at least 2 clusters, but the cluster variable has " +
                std::to_string(g.cluster.empty() ? 0 : g.cluster[0].n_levels));
    };
    const std::string vce_out = vce == "iid" ? std::string("oim") : vce;
    // `--emit-v --model poisson` (no --at/--dydx/--marginlist/--atmeans): the fit
    // itself, e(b) and the full e(V) under the fit's --vce/--cluster
    // (ml_margins_fit, the fit the GLM at() branches below evaluate), as
    // out["e_V"] = {names, matrix} plus a coeftable. PIET's margins grid reads it
    // for the poisson forms it computes from e(b)/e(V): `margins, atmeans` and
    // `margins g` after `poisson yc x c1 i.g, vce(robust)` (w2v_margins_vce.log
    // :1854-1958 V5_po_atmeans/V5_po_g; cluster :2205-2309 V6). cmd_fepois has no
    // --emit-v; logit/probit have their own (cmd_logit/cmd_probit), so only
    // poisson is served, unweighted and with a constant.
    if (emit_v) {
        if (!model_given || model != "poisson" || !atspecs.empty() || !dydx_str.empty() ||
            atmeans || !marginlist.empty())
            throw EngineError("unsupported",
                "margins: --emit-v is implemented for --model poisson without "
                "--at/--dydx/--marginlist/--atmeans only");
        if (!weight_var.empty() || !wtype.empty())
            throw EngineError("unsupported", "margins: --emit-v with --weight/--wtype is not implemented");
        if (vce != "iid" && vce != "robust" && vce != "cluster") {
            std::fprintf(stderr, "margins: --vce must be one of iid|robust|cluster (got %s).\n", vce.c_str());
            return 2;
        }
        if (vce == "cluster" && cluster_var.empty()) {
            std::fprintf(stderr, "margins: --vce cluster requires --cluster C.\n");
            return 2;
        }
        std::vector<std::string> ev_cluster;
        if (!cluster_var.empty()) ev_cluster.push_back(cluster_var);
        GlmInputs g = load_glm(ds, "margins", y, split_csv(xcsv), ev_cluster);
        if (g.n == 0) { std::fprintf(stderr, "margins: no rows after dropping missing.\n"); return 1; }
        for (double vv : g.y) if (vv < 0) { std::fprintf(stderr, "margins(poisson): y has negatives.\n"); return 2; }
        at_clusters_ok(g);
        const MlMarginsFit fit = ml_margins_fit(model, g, vce, asis, nullptr, ml_opts);
        const int k = g.k;
        if (fit.beta.size() != static_cast<size_t>(k) || fit.V.size() != static_cast<size_t>(k) * k)
            throw EngineError("internal", "margins --emit-v: the fit returned no covariance matrix");
        json ct = json::array();
        for (int j = 0; j < k; ++j) {
            const double v = fit.V[static_cast<size_t>(j) * k + j];
            ct.push_back({{"name", g.names[static_cast<size_t>(j)]}, {"b", fit.beta[static_cast<size_t>(j)]},
                          {"se", v > 0 ? std::sqrt(v) : 0.0}});
        }
        json out = {{"estimator", "margins"}, {"type", "fit"}, {"model", model}, {"n_obs", g.n},
                    {"vce", vce_out}, {"coeftable", ct}};
        if (fit.n_clusters > 0) out["n_clusters"] = fit.n_clusters;
        add_e_v(out, g.names, fit.V);
        emit(out, opt(a, "--out"));
        return 0;
    }
    // ---- P7: marginal EFFECTS at fixed covariate values, `margins, dydx() at()` ----
    // Stata allows dydx() and at() together; until P7 this engine treated them as
    // mutually exclusive (the --at branches read --dydx only to mark it consumed,
    // then ignored it), so `margins, dydx(x1 x2) at(treated=(0 1))` silently
    // answered a different question — an average adjusted PREDICTION instead of an
    // average marginal EFFECT.
    //
    // Semantics: the model is fitted ONCE on the real data; for each at-point the
    // design is copied with the at-listed columns overridden counterfactually, and
    // the AME is taken over that copy with the original beta and V. That is exactly
    // margins_ame's existing job, called once per at-point.
    if (!atspecs.empty() && model_given && !dydx_str.empty()) {
        if (model != "logit" && model != "probit" && model != "cloglog" && model != "poisson") {
            std::fprintf(stderr, "margins: --model must be logit|probit|cloglog|poisson\n");
            return 2;
        }
        GlmInputs g = load_glm(ds, "margins", y, split_csv(xcsv), at_cluster_names);
        if (g.n == 0) { std::fprintf(stderr, "margins: no rows after dropping missing.\n"); return 1; }
        if (no_intercept) drop_glm_intercept(g, "margins");
        BinaryPerfectScreen screen;
        std::vector<int> fit_cols;
        if (!binary_refit_screen(model, g, asis, y, "margins", screen, fit_cols, ds, split_csv(xcsv),
                                 !no_intercept))
            return 2;
        at_clusters_ok(g);

        if (model == "logit" || model == "probit" || model == "cloglog") {
            if (!check_binary(g.y, ("margins(" + model + ")").c_str(), y)) return 2;
        } else {
            for (double vv : g.y) if (vv < 0) { std::fprintf(stderr, "margins(poisson): y has negatives.\n"); return 2; }
        }
        // The fit's own VCE (ml_margins_fit): Stata's header on this form after
        // `logit ..., vce(robust)` is `Model VCE: Robust` and its se is the robust
        // one (w2v_margins_siblings.log:2787-2837 F_robust_dydx_at_lg).
        const MlMarginsFit fit = ml_margins_fit(model, g, vce, asis, nullptr, ml_opts);
        if (fit.separated) {
            std::fprintf(stderr, "margins(%s): outcome %s predicts data perfectly; r(2000)\n", model.c_str(), y.c_str());
            return 2;
        }
        const std::vector<double>& beta = fit.beta;
        const std::vector<double>& V = fit.V;
        const GlmLink link = fit.link;

        const std::vector<std::string> request_vars = split_vars(dydx_str);
        // One BLOCK per at-point, not one flat row list. A flat table repeats the
        // same variable names across at-points, and every name-keyed consumer
        // downstream (the tidy coeftable builder, PIET's AME remap) then collapses
        // them to whichever came last — measured: both at-points reported the
        // treated=1 effect. The equations[] shape already exists for exactly this
        // (reg3, mlogit) and renders as Stata's own grouped output, so it is
        // reused rather than inventing an `x1@_at1` spelling.
        json at_groups = json::array();
        int at_index = 0;
        // Every effect's d(dydx)/d(beta), at-point by at-point in emission order:
        // r(V) across points is J_a' V J_b (w2v_margins_siblings.log:2894-2915
        // F_robust_dydx_at_po_v, el(r(V),2,1) .0016499830905486).
        std::vector<std::vector<double>> jac;
        for (const auto& spec : atspecs) {
            ++at_index;
            std::vector<double> Xat = g.X;  // counterfactual copy, per at-point
            json at_desc = json::array();
            for (const auto& kv : split_csv(spec)) {
                auto eq = kv.find('=');
                if (eq == std::string::npos)
                    throw EngineError("bad_input",
                        "margins: --at entry '" + kv + "' must be VAR=VALUE");
                const std::string var = trimx(kv.substr(0, eq));
                const double val = std::stod(kv.substr(eq + 1));
                // Override the design column that IS this variable. A column that
                // merely MENTIONS it (a factor level or an interaction term) cannot
                // be recomputed from the already-expanded design, so it is an honest
                // reject — computing the effect while leaving such a column at its
                // observed value would answer a different question silently.
                bool hit = false;
                for (int c = 0; c < g.k; ++c) {
                    const std::string& nm = g.names[static_cast<size_t>(c)];
                    if (nm == var) {
                        for (int64_t i = 0; i < g.n; ++i)
                            Xat[static_cast<int64_t>(c) * g.n + i] = val;
                        hit = true;
                    } else if (nm != "(intercept)" && margins_name_mentions_var(nm, var)) {
                        throw EngineError("unsupported",
                            "margins: --at cannot fix '" + var + "' because the design also "
                            "carries the derived term '" + nm + "'; that column would keep its "
                            "observed value and the reported effect would not be the one asked for");
                    }
                }
                if (!hit)
                    throw EngineError("bad_input",
                        "margins: --at names '" + var + "', which is not a design column of this model");
                at_desc.push_back({{"var", var}, {"value", val}});
            }
            AmeResult m = margins_ame(link, Xat.data(), g.n, g.k, g.names, beta.data(), V.data(),
                                      nullptr, request_vars);
            if (!m.ok) { std::fprintf(stderr, "%s\n", m.error.c_str()); return 2; }
            json rows = json::array();
            for (const auto& row : m.rows) {
                jac.push_back(row.J);
                const double t = tstat(row.dydx, row.se);
                const double zc = 1.959963984540054;  // 95% normal critical value
                rows.push_back({{"name", row.name},
                                {"b", row.dydx},
                                {"dydx", row.dydx},
                                {"se", row.se},
                                {"t", t},
                                {"p", row.se > 0 ? z_twosided_pvalue(t) : 1.0},
                                {"ci_lo", row.dydx - zc * row.se},
                                {"ci_hi", row.dydx + zc * row.se}});
            }
            at_groups.push_back({{"outcome", "_at" + std::to_string(at_index)},
                                 {"at_index", at_index},
                                 {"at", at_desc},
                                 {"coefs", rows},
                                 {"coeftable", rows}});
        }
        json out = {{"estimator", "margins"}, {"model", model}, {"n_obs", g.n},
                    {"vce", vce_out}, {"n_at", at_index}, {"equations", at_groups},
                    {"V", margins_cross_v(jac, V, g.k)}};
        if (fit.n_clusters > 0) out["n_clusters"] = fit.n_clusters;
        emit(out, opt(a, "--out"));
        return 0;
    }

    if (!atspecs.empty() && model_given) {
        // ---- Predictive margins at fixed covariate values (GLM), `margins --at .. --model ..` ----
        // Average adjusted prediction: margin = (1/n) Σ_i g(x_i(at)'β) over the estimation
        // sample, where x_i(at) is row i's design with the --at covariates overridden (the
        // rest as-observed) — NOT the "at means" convention the linear path above uses.
        // Delta-method SE = sqrt(J' V J), J = (1/n) Σ_i g'(x_i(at)'β) · x_i(at). Reuses the
        // same IID-only fit machinery as the dydx/AME path below (no --vcov/--cluster here,
        // matching that path).
        if (model != "logit" && model != "probit" && model != "cloglog" && model != "poisson") {
            std::fprintf(stderr, "margins: --model must be logit|probit|cloglog|poisson\n");
            return 2;
        }
        GlmInputs g = load_glm(ds, "margins", y, split_csv(xcsv), at_cluster_names);
        if (g.n == 0) { std::fprintf(stderr, "margins: no rows after dropping missing.\n"); return 1; }
        if (no_intercept) drop_glm_intercept(g, "margins");
        BinaryPerfectScreen screen;
        std::vector<int> fit_cols;
        if (!binary_refit_screen(model, g, asis, y, "margins", screen, fit_cols, ds, split_csv(xcsv),
                                 !no_intercept))
            return 2;
        at_clusters_ok(g);
        if (model == "logit" || model == "probit" || model == "cloglog") {
            if (!check_binary(g.y, ("margins(" + model + ")").c_str(), y)) return 2;
        } else {
            for (double vv : g.y) if (vv < 0) { std::fprintf(stderr, "margins(poisson): y has negatives.\n"); return 2; }
        }
        const MlMarginsFit fit = ml_margins_fit(model, g, vce, asis, nullptr, ml_opts);
        if (fit.separated) {
            std::fprintf(stderr, "margins(%s): outcome %s predicts data perfectly; r(2000)\n", model.c_str(), y.c_str());
            return 2;
        }
        const std::vector<double>& beta = fit.beta;
        const std::vector<double>& V = fit.V;
        const double dof = fit.dof;
        const int k = g.k;
        std::unordered_map<std::string, int> idx;
        for (int j = 0; j < k; ++j) idx[g.names[static_cast<size_t>(j)]] = j;
        // BUG-194: this is a predictive margin off a maximum-likelihood (OIM) fit —
        // Stata prints `Model VCE: OIM` and a normal `z` column here, not t(dof); there
        // is no residual dof for an ML model. `dof` above is a Wald N-k approximation
        // kept only for the reported "df" field, and must not multiply the CI. Use the
        // same fixed 95% normal quantile the asymptotic branch of add_inference
        // (common.cpp) uses.
        const double tc = z_critical(0.05);
        const double INV_SQRT_2PI = 0.3989422804014327;
        json arr = json::array();
        std::vector<std::vector<double>> jac;  // each point's Jacobian, for r(V) across points
        for (const auto& spec : atspecs) {
            std::vector<double> Xat = g.X;  // per-row design, overridden below (col-major n*k)
            for (const auto& tok : split_csv(spec)) {
                size_t eq = tok.find('=');
                if (eq == std::string::npos) { std::fprintf(stderr, "margins --at: bad token '%s'\n", tok.c_str()); return 2; }
                std::string nm = trimx(tok.substr(0, eq));
                auto it = idx.find(nm);
                if (it == idx.end()) { std::fprintf(stderr, "margins --at: unknown covariate '%s'\n", nm.c_str()); return 2; }
                margins_refuse_derived_at(g.names, nm);
                double val;
                try { val = std::stod(trimx(tok.substr(eq + 1))); }
                catch (const std::exception&) {
                    std::fprintf(stderr, "margins --at: '%s' has a non-numeric value.\n", tok.c_str());
                    return 2;
                }
                double* col = Xat.data() + static_cast<int64_t>(it->second) * g.n;
                for (int64_t i = 0; i < g.n; ++i) col[i] = val;
            }
            double mg = 0;
            std::vector<double> J(static_cast<size_t>(k), 0.0);
            for (int64_t i = 0; i < g.n; ++i) {
                double eta = 0;
                for (int j = 0; j < k; ++j) eta += Xat[static_cast<int64_t>(j) * g.n + i] * beta[static_cast<size_t>(j)];
                double gi, dgi;
                if (model == "logit") { gi = 1.0 / (1.0 + std::exp(-eta)); dgi = gi * (1.0 - gi); }
                else if (model == "probit") { gi = med_ncdf(eta); dgi = INV_SQRT_2PI * std::exp(-0.5 * eta * eta); }
                else if (model == "cloglog") { gi = -std::expm1(-std::exp(eta)); dgi = std::exp(eta - std::exp(eta)); }
                else { gi = std::exp(eta); dgi = gi; }
                mg += gi;
                for (int j = 0; j < k; ++j) J[static_cast<size_t>(j)] += dgi * Xat[static_cast<int64_t>(j) * g.n + i];
            }
            mg /= static_cast<double>(g.n);
            for (int j = 0; j < k; ++j) J[static_cast<size_t>(j)] /= static_cast<double>(g.n);
            jac.push_back(J);
            double var = 0;
            for (int aa = 0; aa < k; ++aa) {
                double gv = 0;
                for (int bb = 0; bb < k; ++bb) gv += J[static_cast<size_t>(bb)] * V[static_cast<size_t>(aa) + static_cast<size_t>(bb) * k];
                var += J[static_cast<size_t>(aa)] * gv;
            }
            double se = (var > 0) ? std::sqrt(var) : 0.0;
            double t = (se > 0) ? mg / se : 0.0;
            // p-value shares tc's reference distribution (BUG-194): normal, not t(dof).
            arr.push_back({{"at", spec}, {"margin", mg}, {"se", se}, {"t", t}, {"df", dof},
                           {"p", se > 0 ? z_twosided_pvalue(t) : 1.0},
                           {"ci_lo", mg - tc * se}, {"ci_hi", mg + tc * se}, {"model", model}});
        }
        // The full delta-method covariance across points (Stata's r(V): `margins,
        // at(c1=(1(1)3))` after logit, el(r(V),2,1) .0012759229767275 at
        // w2v_margins_siblings.log:2984-3012 F_range_numlist_lg_v).
        json out = {{"estimator", "margins"}, {"type", "predictive"}, {"model", model},
                    {"n_obs", g.n}, {"df", dof}, {"margins", arr}, {"vce", vce_out},
                    {"V", margins_cross_v(jac, V, k)}};
        if (fit.n_clusters > 0) out["n_clusters"] = fit.n_clusters;
        emit(out, opt(a, "--out"));
        return 0;
    }

    // ---- Average marginal effects (AME / `margins, dydx()`) -----------------------
    if (model != "logit" && model != "probit" && model != "cloglog" && model != "poisson") {
        std::fprintf(stderr, "margins: --model must be logit|probit|cloglog|poisson\n");
        return 2;
    }
    if (vce != "iid" && vce != "robust" && vce != "cluster") {
        std::fprintf(stderr, "margins: --vce must be one of iid|robust|cluster (got %s).\n", vce.c_str());
        return 2;
    }
    if (vce == "cluster" && cluster_var.empty()) {
        std::fprintf(stderr, "margins: --vce cluster requires --cluster C.\n");
        return 2;
    }
    // Stata pweight (P5/BUG-158-159): mirrors `cobra logit`'s own validation
    // exactly — only pw is meaningful for an AME, and pweight always forces a
    // sandwich VCE (no iid pweight estimator exists in Stata).
    cecon::WeightKind wkind = cecon::WeightKind::None;
    if (!weight_var.empty() || !wtype.empty()) {
        if (weight_var.empty())
            throw EngineError("bad_input", "margins: --wtype requires --weight");
        if (wtype != "pw" && wtype != "pweight")
            throw EngineError("bad_input", "margins: --wtype must be pw (only pweight is supported)");
        wkind = cecon::WeightKind::Probability;
        if (vce == "iid")
            throw EngineError("bad_input",
                "margins: [pweight=] requires vce(robust) or vce(cluster ...); Stata has no "
                "iid VCE under pweight");
    }
    // probit/poisson's underlying estimators don't (yet) carry every VCE shape a
    // pweighted/clustered logit does — reject honestly rather than silently
    // dropping the request (BUG-110 failure mode).
    if (model == "probit" && wkind != cecon::WeightKind::None) {
        std::fprintf(stderr, "margins(probit): [pweight=] is not supported (probit_fit has no "
                             "weighted estimator); use --model logit.\n");
        return 2;
    }
    if (model == "probit" && vce == "cluster") {
        std::fprintf(stderr, "margins(probit): --vce cluster is not supported; use --model logit.\n");
        return 2;
    }

    std::vector<std::string> cluster_names;
    if (!cluster_var.empty()) cluster_names.push_back(cluster_var);
    GlmInputs g = load_glm(ds, "margins", y, split_csv(xcsv), cluster_names, weight_var, wkind);
    if (g.n == 0) { std::fprintf(stderr, "margins: no rows after dropping missing.\n"); return 1; }
    if (no_intercept) drop_glm_intercept(g, "margins");
    BinaryPerfectScreen screen;
    std::vector<int> fit_cols;
    if (!binary_refit_screen(model, g, asis, y, "margins", screen, fit_cols, ds, split_csv(xcsv),
                             !no_intercept))
        return 2;
    if (vce == "cluster" && (g.cluster.empty() || g.cluster[0].n_levels < 2))
        throw EngineError("bad_input",
            "margins: vce(cluster) requires at least 2 clusters, but the cluster variable has " +
            std::to_string(g.cluster.empty() ? 0 : g.cluster[0].n_levels));

    // Route: a plain-continuous, unweighted, iid design (no `#`/`level.var`
    // columns, no --weight/--vce/--cluster/--dydx) takes the EXACT old
    // per-column path below — byte-identical output to before this fix existed.
    // Anything else (an interaction, a factor level, a weight, a non-iid VCE, or
    // an explicit --dydx selection) takes the new margins_ame (P5/BUG-158-159).
    bool interaction_or_factor = false;
    for (const auto& nm : g.names)
        if (nm != "(intercept)" && margins_name_is_factor_or_interaction(nm)) { interaction_or_factor = true; break; }
    // cloglog has no per-column path of its own: margins_ame takes it.
    bool use_general = interaction_or_factor || wkind != cecon::WeightKind::None ||
                       vce != "iid" || !cluster_var.empty() || !dydx_str.empty() ||
                       model == "cloglog";

    json coefs = json::array();
    int n_clusters_out = 0;
    if (!use_general) {
        MarginsResult m;
        if (model == "logit") {
            if (!check_binary(g.y, "margins(logit)", y)) return 2;
            LogitResult r = refit_logit(g, ml_opts);
            m = margins_logit(g.X.data(), g.n, g.k, r.beta.data(), r.bread.data());
        } else if (model == "probit") {
            if (!check_binary(g.y, "margins(probit)", y)) return 2;
            ProbitResult r = refit_probit(g, ml_opts);
            // BUG-210, second iid site: this "exact old per-column path" passed the
            // FISHER bread r.V too, so plain `margins --model probit` (no --dydx) was
            // wrong by the same 1.5% as the --dydx branch — measured identical SEs
            // pre-fix. probit is non-canonical; the delta method must be taken against
            // the OBSERVED-information covariance Stata and `cobra probit` both report.
            m = margins_probit(g.X.data(), g.n, g.k, r.beta.data(),
                               probit_observed_info(r, g.X.data(), g.n, g.k, g.y.data()).bread.data());
        } else {
            for (double v : g.y) if (v < 0) { std::fprintf(stderr, "margins(poisson): y has negatives.\n"); return 2; }
            // pooled poisson = fepois with no FE; build a ModelData with intercept.
            ModelData md; md.has_intercept = true; md.dm.n = g.n; md.dm.k = g.k;
            md.dm.X = g.X; md.dm.y = g.y; md.dm.xnames = g.names; md.keep = g.keep;
            FepoisResult r = fepois_fit(md, Vcov::IID);
            m = margins_poisson(g.X.data(), g.n, g.k, r.beta.data(), r.vcov_matrix.data(), nullptr);
        }
        for (int c = 0; c < g.k; ++c) {
            if (g.names[static_cast<size_t>(c)] == "(intercept)") continue;  // no AME for the constant
            double se = m.se[static_cast<size_t>(c)];
            coefs.push_back({{"name", g.names[static_cast<size_t>(c)]}, {"dydx", m.dydx[static_cast<size_t>(c)]},
                             {"se", se}, {"t", tstat(m.dydx[static_cast<size_t>(c)], se)}});
        }
    } else {
        const double* wptr = g.weights.empty() ? nullptr : g.weights.data();
        if (model == "logit" || model == "probit" || model == "cloglog") {
            if (!check_binary(g.y, ("margins(" + model + ")").c_str(), y)) return 2;
        } else {
            for (double v : g.y) if (v < 0) { std::fprintf(stderr, "margins(poisson): y has negatives.\n"); return 2; }
        }
        // The fit's maximize options; this branch used to cap logit at iterate(100)
        // where the fit's default is c(maxiter) 300 (FU-426).
        MlMarginsFit fit = ml_margins_fit(model, g, vce, asis, wptr, ml_opts);
        if (fit.separated) {
            std::fprintf(stderr, "margins(%s): outcome %s predicts data perfectly; r(2000)\n", model.c_str(), y.c_str());
            return 2;
        }
        const GlmLink link = fit.link;
        const std::vector<double>& beta_out = fit.beta;
        const std::vector<double>& V_out = fit.V;
        n_clusters_out = fit.n_clusters;
        std::vector<std::string> request_vars = dydx_str.empty() ? std::vector<std::string>{} : split_vars(dydx_str);
        AmeResult m = margins_ame(link, g.X.data(), g.n, g.k, g.names, beta_out.data(), V_out.data(),
                                  wptr, request_vars);
        if (!m.ok) { std::fprintf(stderr, "%s\n", m.error.c_str()); return 2; }
        for (const auto& row : m.rows)
            coefs.push_back({{"name", row.name}, {"dydx", row.dydx}, {"se", row.se}, {"t", tstat(row.dydx, row.se)}});
    }
    json out = {{"estimator", "margins"}, {"model", model}, {"n_obs", g.n}, {"coefs", coefs}};
    if (use_general) {
        out["vce"] = vce;
        if (wkind != cecon::WeightKind::None) { out["weight_var"] = weight_var; out["wtype"] = "pw"; }
        if (vce == "cluster") { out["cluster_var"] = cluster_var; out["n_clusters"] = n_clusters_out; }
    }
    emit(out, opt(a, "--out"));
    return 0;
}

int cmd_predict(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), y = opt(a, "--y"), xcsv = opt(a, "--x");
    std::string outpath = opt(a, "--out");
    // Create ONLY the columns the caller asked for. Creating both unconditionally
    // appended a second, unrequested variable and — when the dataset already had one
    // named `resid` — silently overwrote the user's own data (Stata's `predict yhat, xb`
    // touches nothing but `yhat`). With neither flag given the standalone CLI keeps its
    // documented both-columns behaviour, which is the only shape any caller relied on.
    bool wantXb = has(a, "--xb"), wantResid = has(a, "--resid");
    if (!wantXb && !wantResid) wantXb = wantResid = true;
    std::string xbname = opt(a, "--xb", "xb"), rname = opt(a, "--resid", "resid");
    // Restricted-sample fit (Stata `regress ... if/in ...` then `predict` over the
    // FULL dataset). --fit-data names an already-materialized restricted file — the
    // shape PIET needs for `in` ranges, which are `_n`-positional and have no cobra
    // keepif predicate (PIET's applyIfAndInFilter, piet/native/src/inrange_filter.cpp,
    // materializes those into a scratch .hbin and passes ITS path here). --fit-keepif
    // applies a keepif EXPR to --data directly for callers that already have one.
    // Neither given -> fit sample == predict data, byte-identical to the prior
    // behavior (the only shape this function had before this option existed).
    std::string fit_data_opt = opt(a, "--fit-data");
    std::string fit_keepif = opt(a, "--fit-keepif");
    // FU-131: the fit's `noconstant` (w2p_estim_ev.log:1387 G_nc_predict_xb,
    // :1402 G_nc_predict_res). Read here so both paths consume it: the ML path
    // forwards it through postest_fit_spec, the least-squares path to feols_fit.
    const bool no_intercept = has(a, "--no-intercept");
    // BUG-1067: the fit's collinearity rule (cmd_feols --collinear-forward). The ML
    // path's postest_fit_spec reads it too and refuses it there.
    const bool collinear_forward = has(a, "--collinear-forward");
    if (data_missing(data) || y.empty() || xcsv.empty() || out_missing(outpath)) {
        std::fprintf(stderr, "usage: cobra predict --data f.hbin --y Y --x X1,X2 [--fe F] "
                             "--out new.hbin [--xb NAME] [--resid NAME] "
                             "[--model logit|probit|cloglog|poisson [--pred NAME]] "
                             "[--fit-data FIT.hbin] [--fit-keepif EXPR] [--no-intercept] "
                             "[--store-type byte|int|long|float|double]\n"
                             "  appends fitted values and/or residuals (within-residuals under --fe);\n"
                             "  with neither flag both are appended under their default names.\n"
                             "  --model re-fits with the named ML estimator and appends its Stata-default\n"
                             "  statistic (--pred: pr after logit/probit/cloglog, n after poisson) and/or --xb;\n"
                             "  --resid/--fe are not offered on that path.\n"
                             "  --fit-data/--fit-keepif fit on a restricted sample while xb/resid are\n"
                             "  still evaluated over every row of --data with non-missing covariates;\n"
                             "  both together restrict FIT.hbin by EXPR. With --fe only --fit-keepif\n"
                             "  over a time-series regressor is served: xb is x b + _cons (no absorbed\n"
                             "  part) and resid the within residual on the fit's rows.\n"
                             "  --store-type stores every written column as that Stata type holds it\n"
                             "  (Stata's predict creates its variable as c(type), float by default).\n");
        return 2;
    }
    // BUG-924: the new variable's Stata storage type. Absent = double, so every
    // caller that does not ask keeps its bytes.
    const StoreType store_type = parse_store_type(a, "predict");
    const Dataset& ds = acquire_data(data);
    auto fes = split_csv(opt(a, "--fe"));
    auto xs = refit_xs(xcsv);
    // The fit's tsset context (BUG-369's shape: .hbin does not persist it), so an
    // `L.x` regressor is the lag over the full series in the refit and in the
    // prediction. Empty when no flag is given: every existing call is unchanged.
    const cecon::TsMeta tsm = parse_tsmeta(a);
    const cecon::TsMeta* tsp = tsm.timevar.empty() ? nullptr : &tsm;
    const std::vector<std::string> ys_plan{y};  // BUG-1277: an operated depvar plans the `if` too
    bool restricted_fit = !fit_data_opt.empty() || !fit_keepif.empty();
    auto refuse_restricted_fe = [] {
        // Out-of-sample prediction under absorbed FE would need the FE's fitted
        // per-level intercepts reconstructed and applied to rows the FE model never
        // saw (undefined for FE levels absent from the fit sample) — not attempted.
        std::fprintf(stderr,
            "predict: --fit-data/--fit-keepif restricted-sample fitting is not "
            "supported together with --fe (fixed-effects out-of-sample prediction "
            "is undefined).\n");
        return 2;
    };
    // The one restricted shape served with --fe is --fit-keepif alone routed as a
    // row mask over --data itself: the fit's rows are rows of --data, so the within
    // residual has a row to land on (a time-series regressor, and since FU-768 any
    // other regressor too, below).
    if (restricted_fit && !fes.empty() && !fit_data_opt.empty()) return refuse_restricted_fe();
    // Dataset::open (not acquire_data, which is a single-slot cache — see its own
    // doc comment) so the fit dataset and the full predict dataset (`ds`, already
    // acquired above) can be alive at the same time.
    Dataset fit_owned;
    const Dataset* fit_ds = &ds;
    KeepifPlan fit_plan;
    if (!fit_data_opt.empty() && !fit_keepif.empty()) {
        // Both: the fit's own frame (a file) and its restriction, routed as the
        // fit routed it -- with a time-series regressor a row mask on the FULL
        // frame after the operators, otherwise the physical pre-filter. PIET's
        // predict on a moved head sends this pair (the live frame is --data).
        fit_plan = plan_keepif(fit_data_opt, fit_keepif, &tsm, {&xs, &ys_plan});
        fit_owned = Dataset::open(fit_plan.data);
        fit_ds = &fit_owned;
    } else if (!fit_data_opt.empty()) {
        fit_owned = Dataset::open(fit_data_opt);
        fit_ds = &fit_owned;
    } else if (!fit_keepif.empty() && !fes.empty()) {
        // FU-768 (w2aa_predict_type_restricted.log b_if_* / c_xt_if_* / c_hdfe_if_*:
        // `areg|xtreg|reghdfe ... if t>=3` then predict, rc 0): the restriction is a
        // row mask over --data whatever the regressors, so the within residual lands
        // on the fit's own rows of --data and is missing off them. Without a
        // time-series operator the masked fit and the physically filtered one admit
        // the same rows (eval_keepif_mask applies Frame::keep_if's own rule); with
        // one this is plan_keepif's own masked plan.
        fit_plan.data = data;
        fit_plan.mask = eval_keepif_mask(data, fit_keepif);
        fit_plan.active = true;
    } else if (!fit_keepif.empty()) {
        // e(sample) T2 / FU-475: routed as the fit routed its --keepif (plan_keepif):
        // with a time-series regressor the restriction is a row mask on the FULL
        // data after the operators (w2_esample.log:751-775 C6 e(N) 40), not the
        // physically filtered copy whose boundary lags are missing.
        fit_plan = plan_keepif(data, fit_keepif, &tsm, {&xs, &ys_plan});
        if (!fit_plan.active && fit_plan.data != data) {
            fit_owned = Dataset::open(fit_plan.data);
            fit_ds = &fit_owned;
        }
    }
    const std::vector<char>* fit_mask = fit_plan.active ? &fit_plan.mask : nullptr;
    if (restricted_fit && !fes.empty() && (fit_mask == nullptr || fit_ds != &ds))
        return refuse_restricted_fe();
    // ---- BUG-204's open half (BUG-297): `--model` = the maximum-likelihood
    // prediction path. Everything ABOVE this branch (usage, restricted-fit
    // plumbing) is shared; everything BELOW it is the historical least-squares
    // path, byte-for-byte — `--model` absent (or naming the least-squares
    // family) never enters here, which is what keeps every existing predict
    // caller, eval case and freeze fixture unchanged.
    //
    // The refit is postest_fit_spec — the ONE dispatcher BUG-203 built for
    // lincom/nlcom/test — so `predict --model logit` answers off the identical
    // logit_fit/probit_fit/fepois_fit coefficients those verbs use (and
    // inherits its refusals: --fe / --cluster / non-default --vcov, and any
    // model outside logit|probit|poisson). The statistic emitted is Stata's
    // DEFAULT for the model — `pr` after logit/probit, `n` after poisson —
    // under `--pred NAME`; `--xb NAME` is the linear index. `--resid` is an
    // honest refusal: Stata's `predict, residuals` after an ML fit is a
    // DIFFERENT statistic (Pearson residuals), not y - xb, and serving y - xb
    // under that name would be exactly the silent-wrong shape BUG-204 removed.
    //
    // Inverse links are verbatim cmd_margins' GLM branch (gi above): logistic,
    // med_ncdf, exp — one convention, two surfaces, no drift. Stata evaluates
    // the prediction for EVERY row with non-missing covariates (y is not
    // needed), so the loop mirrors the restricted-fit OLS loop below:
    // name-keyed lookup over the full dataset's expanded design, NaN rows
    // skipped, never renumbered.
    const std::string ml_model = opt(a, "--model");
    const bool ols_family = ml_model.empty() || ml_model == "ols" || ml_model == "feols" ||
                            ml_model == "regress" || ml_model == "reg";
    if (!ols_family) {
        if (has(a, "--resid")) {
            std::fprintf(stderr,
                "predict --model %s: residuals are not supported after a maximum-likelihood "
                "fit in this engine (Stata's `predict, residuals` there is a Pearson residual, "
                "not y - xb). Supported: the model's default statistic (--pred NAME: pr after "
                "logit/probit/cloglog, n after poisson) and --xb NAME.\n", ml_model.c_str());
            return 2;
        }
        PostestModel M;
        BinaryPerfectScreen rules;
        if (int rc = postest_fit_spec(a, *fit_ds, y, xcsv, M, nullptr, &rules, fit_mask))
            return rc;  // BUG-203's dispatcher
        const bool wantXbMl = has(a, "--xb");
        const bool wantPred = has(a, "--pred") || !wantXbMl;
        const std::string predname = opt(a, "--pred", ml_model == "poisson" ? "n" : "pr");
        int64_t Nml = ds.n_rows();
        TsMeta tsm_ml;
        auto xcols_ml = expand_varlist(ds, xs, tsm_ml, /*bare_factor=*/false);
        std::unordered_map<std::string, const std::vector<double>*> colmap_ml;
        colmap_ml.reserve(xcols_ml.size() * 2);
        for (const auto& nc : xcols_ml) colmap_ml.emplace(nc.first, &nc.second);
        const int kk = static_cast<int>(M.names.size());
        std::vector<double> pred_full(static_cast<size_t>(Nml), std::nan(""));
        std::vector<double> xbml_full(static_cast<size_t>(Nml), std::nan(""));
        for (int64_t i = 0; i < Nml; ++i) {
            double eta = 0.0;
            bool ok = true;
            for (int c = 0; c < kk; ++c) {
                const std::string& nm = M.names[static_cast<size_t>(c)];
                double v;
                if (nm == "(intercept)" || nm == "_cons") {
                    v = 1.0;
                } else {
                    auto it = colmap_ml.find(nm);
                    if (it == colmap_ml.end()) { ok = false; break; }
                    v = (*it->second)[static_cast<size_t>(i)];
                    if (std::isnan(v)) { ok = false; break; }
                }
                eta += v * M.beta[static_cast<size_t>(c)];
            }
            if (!ok) continue;
            xbml_full[static_cast<size_t>(i)] = eta;
            if (ml_model == "logit")       pred_full[static_cast<size_t>(i)] = 1.0 / (1.0 + std::exp(-eta));
            else if (ml_model == "probit") pred_full[static_cast<size_t>(i)] = med_ncdf(eta);
            else if (ml_model == "cloglog") pred_full[static_cast<size_t>(i)] = -std::expm1(-std::exp(eta));
            else                           pred_full[static_cast<size_t>(i)] = std::exp(eta);
        }
        // FU-541: Stata's predict rules for logit/probit: for each e(rules)
        // row, xb and pr are missing where float(x) != float(rhs) (op 1), and for op
        // 2/3 wherever float(x) differs from float(rhs) on either side
        // (w2u_binary_ml_edges.log bm_l_dz0_pred: row 9, dz0 == 1, p and xb missing).
        // A rule on a factor's base level (`0b.g`, col -1) reads the level's
        // indicator off the factor itself (StataBE `logit d0 x i.g` with 0.g
        // dropped: pr and xb missing on the 10 g == 0 rows, eng-ml/x5.log P1/P2).
        // cloglog's predict applies no rules (StataBE `cloglog yb x z d` with d's
        // rows dropped: pr on all 200 rows, none missing, eng-ml/x21.log PDm).
        for (const BinaryPerfectRule& r : rules.rules) {
            if (ml_model == "cloglog") break;
            if (r.op < 1 || r.op > 3) continue;
            std::vector<double> base_ind;
            const std::vector<double>* xv = nullptr;
            if (r.col < 0) {
                if (r.base_var.empty() || !ds.has(r.base_var) || ds.col(r.base_var).dtype != DType::F64)
                    continue;
                const double* f = ds.col(r.base_var).f64();
                base_ind.resize(static_cast<size_t>(Nml));
                for (int64_t i = 0; i < Nml; ++i)
                    base_ind[static_cast<size_t>(i)] =
                        std::isnan(f[i]) ? f[i] : (f[i] == r.base_level ? 1.0 : 0.0);
                xv = &base_ind;
            } else {
                if (static_cast<size_t>(r.col) >= M.names.size()) continue;
                auto it = colmap_ml.find(M.names[static_cast<size_t>(r.col)]);
                if (it == colmap_ml.end()) continue;
                xv = it->second;
            }
            const float rhs = static_cast<float>(r.rhs);
            for (int64_t i = 0; i < Nml; ++i) {
                const double v = (*xv)[static_cast<size_t>(i)];
                if (std::isnan(v) || static_cast<float>(v) == rhs) continue;
                pred_full[static_cast<size_t>(i)] = std::nan("");
                xbml_full[static_cast<size_t>(i)] = std::nan("");
            }
        }
        round_column_to_store(pred_full.data(), pred_full.size(), store_type);
        round_column_to_store(xbml_full.data(), xbml_full.size(), store_type);
        Frame frml = frame_from_acquired(ds);
        if (wantPred) frml.set_f64_column(predname, std::move(pred_full));
        if (wantXbMl) frml.set_f64_column(xbname, std::move(xbml_full));
        commit_frame(std::move(frml), outpath);
        json outml = {{"estimator", "predict"}, {"model", ml_model}, {"n_obs", M.n},
                      {"n_rows", Nml}, {"pred", wantPred ? predname : std::string()},
                      {"xb", wantXbMl ? xbname : std::string()},
                      {"resid", std::string()}, {"out", outpath}, {"within", false}};
        emit(outml, "");
        return 0;
    }

    if (no_intercept && !fes.empty())
        throw EngineError("bad_input", "predict: --no-intercept is not valid with --fe");
    // The fit kept its singleton fixed-effect groups (areg, xtreg,fe, reghdfe's
    // keepsingletons): so does the refit, or a singleton row's xb/resid is missing
    // where Stata's is not (w2_esample.log:925 D4 areg keeps id 11).
    ModelData md = build_model(*fit_ds, y, xs, fes, {}, /*offset_name=*/"",
                               /*drop_singleton=*/!has(a, "--keep-singletons"), tsp, fit_mask);
    // BUG-1067: the fit's collinearity rule (reghdfe/areg/xtreg,fe keep the earlier
    // term), so the refit's coefficients are the ones the fit posted.
    md.collinear_forward = collinear_forward;
    // feols_fit drops build_model's leading constant column from md.dm under
    // no_intercept, so the in-sample loop below reads the fitted design as-is.
    FeolsResult r = feols_fit(md, Vcov::IID, /*cluster_index=*/0, no_intercept);  // demeans md.dm in place (no-op if no FE)
    int64_t n = md.dm.n; int k = md.dm.k;
    int64_t N = ds.n_rows();
    std::vector<double> xb_full(static_cast<size_t>(N), std::nan(""));
    std::vector<double> rs_full(static_cast<size_t>(N), std::nan(""));
    // Without absorbed effects xb is a pure linear-predictor evaluation over every
    // row whose covariates are present, fit sample or not (w2n_followups.log:265
    // B01: y missing in rows 3 and 11, `predict yhat` leaves 2 missing, yhat[3]
    // 6.685) -- the general loop below, whichever data the fit read. The
    // in-sample loop serves the --fe fits, whose xb/resid are e(sample)-only.
    if (fit_ds == &ds && fit_mask == nullptr && !fes.empty()) {
        // Fit sample == predict data: exactly the prior (pre-restricted-fit) code path.
        for (int64_t i = 0; i < n; ++i) {
            double fit = 0;
            for (int c = 0; c < k; ++c) fit += md.dm.X[static_cast<size_t>(c) * n + i] * r.beta[c];
            double resid = md.dm.y[static_cast<size_t>(i)] - fit;
            int64_t orig = md.keep[static_cast<size_t>(i)];
            xb_full[static_cast<size_t>(orig)] = fit;
            rs_full[static_cast<size_t>(orig)] = resid;
        }
    } else {
        // Restricted fit: evaluate xb/resid over EVERY row of the full predict
        // dataset whose covariates are non-missing — xb is a pure linear-predictor
        // evaluation and Stata computes it wherever the covariates are present,
        // regardless of y (resid additionally needs y, since resid = y - xb).
        // Build the same factor-expanded design over the FULL dataset (the engine's
        // `level.varname` naming convention, BUG-101, makes terms name-comparable
        // across the two datasets) and look each of the fit's coefficients up by
        // name: a level the full data has but the fit sample didn't never entered
        // r.names, so it is simply never referenced here; a level the fit sample
        // needed that the full data is missing on a given row makes that row's
        // lookup miss (or read NaN), and the row is left NaN — the fit-sample's own
        // coefficient/level set is never renumbered or silently dropped.
        auto xcols_full = expand_varlist(ds, xs, tsm, /*bare_factor=*/false);
        std::unordered_map<std::string, const std::vector<double>*> colmap;
        colmap.reserve(xcols_full.size() * 2);
        for (const auto& nc : xcols_full) colmap.emplace(nc.first, &nc.second);
        // BUG-1277: an operated depvar (D.y) is the operator over the full series,
        // materialized as the regressors are (the residual reads it).
        std::vector<std::pair<std::string, std::vector<double>>> ycol_ts;
        if (tsp && varlist_has_ts(ys_plan)) ycol_ts = expand_varlist(ds, ys_plan, tsm, /*bare_factor=*/false);
        const double* yfull = ycol_ts.size() == 1 ? ycol_ts[0].second.data() : require_num(ds, "predict", y);
        const int kk = static_cast<int>(r.names.size());
        for (int64_t i = 0; i < N; ++i) {
            double fit = 0.0;
            bool ok = true;
            for (int c = 0; c < kk; ++c) {
                const std::string& nm = r.names[static_cast<size_t>(c)];
                double v;
                if (nm == "(intercept)" || nm == "_cons") {
                    v = 1.0;
                } else {
                    auto it = colmap.find(nm);
                    if (it == colmap.end()) { ok = false; break; }
                    v = (*it->second)[static_cast<size_t>(i)];
                    if (std::isnan(v)) { ok = false; break; }
                }
                fit += v * r.beta[static_cast<size_t>(c)];
            }
            if (!ok) continue;
            xb_full[static_cast<size_t>(i)] = fit;
            double yi = yfull[i];
            if (!std::isnan(yi)) rs_full[static_cast<size_t>(i)] = yi - fit;
        }
        if (!fes.empty()) {
            // --fe under the --fit-keepif row mask (w2_esample_tsrefit.log:251-263
            // T7, `xtreg y L.x1 x2 if t>=3, fe` then predict xb): xb above is
            // x b + _cons (the fit's reconstructed constant, feols GAP B) over
            // every row with non-missing covariates, without the absorbed
            // component, and resid is the WITHIN residual on the estimation
            // sample only, as the unrestricted --fe path writes it.
            std::fill(rs_full.begin(), rs_full.end(), std::nan(""));
            for (int64_t i = 0; i < n; ++i) {
                double fit = 0;
                for (int c = 0; c < k; ++c) fit += md.dm.X[static_cast<size_t>(c) * n + i] * r.beta[c];
                rs_full[static_cast<size_t>(md.keep[static_cast<size_t>(i)])] =
                    md.dm.y[static_cast<size_t>(i)] - fit;
            }
        }
    }
    round_column_to_store(xb_full.data(), xb_full.size(), store_type);
    round_column_to_store(rs_full.data(), rs_full.size(), store_type);
    Frame fr = frame_from_acquired(ds);
    if (wantXb) fr.set_f64_column(xbname, std::move(xb_full));
    if (wantResid) fr.set_f64_column(rname, std::move(rs_full));
    commit_frame(std::move(fr), outpath);  // §26: stage into resident session, or write one-shot (status JSON below)
    json out = {{"estimator", "predict"}, {"n_obs", n}, {"n_rows", N},
                {"xb", wantXb ? xbname : std::string()},
                {"resid", wantResid ? rname : std::string()},
                {"fe", fes}, {"out", outpath},
                {"within", !fes.empty()}};
    emit(out, "");  // status to stdout
    return 0;
}

int cmd_hausman(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), y = opt(a, "--y"), xcsv = opt(a, "--x");
    std::string ivar = opt(a, "--ivar");
    if (data_missing(data) || y.empty() || xcsv.empty() || ivar.empty()) {
        std::fprintf(stderr, "usage: cobra hausman --data f.hbin --y Y --x X1,X2 --ivar ID [--out f.json]\n"
                             "  FE (feols, fe=ivar) vs RE (xtreg) on the common time-varying slopes.\n");
        return 2;
    }
    const Dataset& ds = acquire_data(data);
    auto xs = split_csv(xcsv);
    // FE via feols absorbing ivar.
    ModelData md = build_model(ds, y, xs, {ivar}, {});
    FeolsResult fe = feols_fit(md, Vcov::IID);  // names = xs (no intercept under FE)
    // RE via xtreg (CLI-style listwise + collinear drop already inside xtreg path).
    int64_t n_raw = ds.n_rows();
    const double* yraw = require_num(ds, "hausman", y);
    const double* iraw = require_num(ds, "hausman", ivar);
    int kx = static_cast<int>(xs.size());
    std::vector<const double*> xraw(kx);
    for (int c = 0; c < kx; ++c) xraw[c] = require_num(ds, "hausman", xs[c]);
    std::vector<const double*> all = {yraw, iraw};
    for (auto* p : xraw) all.push_back(p);
    auto keep = listwise_keep(all, n_raw);
    int64_t n = static_cast<int64_t>(keep.size());
    auto yv = gather_rows(yraw, keep); auto iv = gather_rows(iraw, keep);
    std::vector<double> Xre(static_cast<size_t>(n) * kx);
    for (int c = 0; c < kx; ++c) { auto col = gather_rows(xraw[c], keep);
        std::copy(col.begin(), col.end(), Xre.data() + static_cast<int64_t>(c) * n); }
    XtregReResult re = xtreg_re(Xre.data(), n, kx, yv.data(), iv.data());
    // RE names: xs[0..kx-1] then _cons; RE V is (kx+1)*(kx+1) with _cons last.
    // Common set = the time-varying slopes that FE identifies (fe.names ⊆ xs).
    std::vector<int> fe_idx, re_idx;
    std::vector<std::string> common;
    for (size_t f = 0; f < fe.names.size(); ++f) {
        // find this name in xs (RE order)
        for (int c = 0; c < kx; ++c)
            if (xs[static_cast<size_t>(c)] == fe.names[f]) {
                fe_idx.push_back(static_cast<int>(f)); re_idx.push_back(c);
                common.push_back(fe.names[f]); break;
            }
    }
    int kc = static_cast<int>(common.size());
    if (kc == 0) { std::fprintf(stderr, "hausman: no common coefficients.\n"); return 1; }
    int kfe = static_cast<int>(fe.names.size());
    int kre = kx + 1;
    if (static_cast<int>(fe.beta.size()) < kfe || static_cast<int>(re.beta.size()) < kre ||
        static_cast<int>(fe.vcov_matrix.size()) < kfe * kfe ||
        static_cast<int>(re.vcov_matrix.size()) < kre * kre) {
        std::fprintf(stderr, "hausman: degenerate FE or RE fit (single panel unit, collinear regressors, or too few observations); cannot form the comparison.\n");
        return 1;
    }
    std::vector<double> bFE(kc), bRE(kc), VFE(static_cast<size_t>(kc) * kc, 0.0), VRE(static_cast<size_t>(kc) * kc, 0.0);
    for (int i = 0; i < kc; ++i) {
        bFE[static_cast<size_t>(i)] = fe.beta[static_cast<size_t>(fe_idx[static_cast<size_t>(i)])];
        bRE[static_cast<size_t>(i)] = re.beta[static_cast<size_t>(re_idx[static_cast<size_t>(i)])];
        for (int j = 0; j < kc; ++j) {
            VFE[static_cast<size_t>(j) * kc + i] = fe.vcov_matrix[static_cast<size_t>(fe_idx[static_cast<size_t>(j)]) * kfe + fe_idx[static_cast<size_t>(i)]];
            VRE[static_cast<size_t>(j) * kc + i] = re.vcov_matrix[static_cast<size_t>(re_idx[static_cast<size_t>(j)]) * kre + re_idx[static_cast<size_t>(i)]];
        }
    }
    HausmanResult h = hausman_test(bFE.data(), VFE.data(), bRE.data(), VRE.data(), kc);
    if (!h.ok) { std::fprintf(stderr, "hausman: %s\n", h.error.c_str()); return 1; }
    json diffs = json::array();
    for (int i = 0; i < kc; ++i) {
        json row = {{"name", common[static_cast<size_t>(i)]}, {"b_fe", bFE[static_cast<size_t>(i)]},
                    {"b_re", bRE[static_cast<size_t>(i)]}, {"diff", bFE[static_cast<size_t>(i)] - bRE[static_cast<size_t>(i)]}};
        double sd = i < static_cast<int>(h.se_diff.size()) ? h.se_diff[static_cast<size_t>(i)]
                                                           : std::numeric_limits<double>::quiet_NaN();
        if (std::isnan(sd)) row["se_diff"] = nullptr; else row["se_diff"] = sd;  // Stata prints "." here
        diffs.push_back(row);
    }
    // BUG-1940: dof = rank of the difference, p missing when chi2 < 0, and
    // the not-positive-definite flag Stata reports as a note.
    json out = {{"estimator", "hausman"}, {"chi2", h.chi2}, {"dof", h.dof},
                {"not_pd", h.not_pd}, {"coefs", diffs}};
    if (std::isnan(h.pvalue)) out["p"] = nullptr; else out["p"] = h.pvalue;
    emit(out, opt(a, "--out"));
    return 0;
}

}  // namespace cli
}  // namespace cecon
