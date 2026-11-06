// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "common.h"
#include "commands.h"

#include "estimators/ivml.h"
#include "estimators/logit.h"
#include "estimators/probit.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>

// P7 wave — ivprobit, ivtobit (Stata's `ivprobit` / `ivtobit`, the full-information
// MLE forms, not the two-step ones).
//
// Both verbs are the same two-equation model with a different observation rule on
// the structural outcome, so they share run_ivml() below and differ only in the
// IvKind they pass, the censoring options they accept, and the extra sd(e.y) row
// ivtobit reports.
//
// VARLISTS. Stata writes `ivprobit y1 x (y2 = z)`; parsing that parenthesized form
// is PIET's job, so this CLI takes the three lists explicitly: --y, --x (structural
// exogenous), --endog (the endogenous regressor), --instruments (the excluded
// instruments). The two designs are then assembled as
//     Xs = [endog, x…, intercept]   (structural — Stata's coefficient table order)
//     Xw = [x…, instruments…, intercept]  (reduced form — the "Exogenous: x z" footer)
// from ONE load_glm pass over y + endog + instruments + x, so both equations and
// every reported statistic share a single listwise-deleted sample. That single pass
// is also why the two commands legitimately report different N on the same data —
// ivprobit never touches the tobit outcome, so its sample keeps rows the tobit run
// drops (4,800 vs 4,778 on the P7 wave fixture).
//
// A two-step (Newey / Amemiya) estimator would land close on the structural
// coefficients but has no corr(e.endog,e.y) or sd(e.·) rows at all, and no Wald
// test of exogeneity built from them — the oracle prints standard errors for those
// rows, which only full-information ML produces. estimators/ivml.h is the FIML.
// BUG-1500: ivprobit's `twostep` option asks for that two-step estimator by name;
// --twostep serves it (run_ivprobit_twostep), a different model, not a stand-in.

namespace cecon {
namespace cli {

// cmds_mle.cpp: compact_glm_rows, the rows a perfect-prediction rule leaves.
void glm_keep_rows(GlmInputs& g, const std::vector<char>& keep);

namespace {

enum class IvKind { Probit, Tobit };

// Index of `name` in `names`, or -1. Plain (non-factor) variables come back out of
// build_model under their own name, which is why --endog/--instruments are
// restricted to plain names below: the lookup is what pins each requested variable
// to its column no matter how the factor expansion of --x reshuffled the rest.
int find_name(const std::vector<std::string>& names, const std::string& name) {
    for (size_t i = 0; i < names.size(); ++i)
        if (names[i] == name) return static_cast<int>(i);
    return -1;
}

// ---- `ivprobit ..., twostep`: Newey's minimum chi-squared estimator ----
// BUG-1500. [R] ivprobit, Methods and formulas: every exogenous variable forms x, the
// structural regressors form z; alpha, the reduced-form probit's x coefficients, equals
// D delta with D = (Pi, I1) up to a known covariance Omega, and delta is the GLS solution
// of that system. Both probits are `probit` at its default maximize options with the
// covariance probit posts. The reply names coefficients without an equation, as e(b) does.

// Columns of the sample, gathered into one n x cols matrix.
std::vector<double> gather_columns(const std::vector<const double*>& columns, int64_t n) {
    std::vector<double> out;
    out.reserve(columns.size() * static_cast<size_t>(n));
    for (const double* c : columns) out.insert(out.end(), c, c + n);
    return out;
}

// Regressions on one fixed design.
class FixedDesignOls {
public:
    FixedDesignOls(std::vector<double> design, int64_t n, int k)
        : design_(std::move(design)), n_(n), k_(k), inv_xx_(static_cast<size_t>(k) * static_cast<size_t>(k)) {
        la::xtx(design_.data(), n_, k_, inv_xx_.data());
        invertible_ = la::spd_inverse(inv_xx_.data(), k_) == 0;
    }
    bool invertible() const { return invertible_; }

    std::vector<double> coefficients(const double* outcome) const {
        std::vector<double> xy(static_cast<size_t>(k_)), b(static_cast<size_t>(k_));
        la::xtv(design_.data(), n_, k_, outcome, xy.data());
        la::matvec(inv_xx_.data(), k_, xy.data(), b.data());
        return b;
    }
    std::vector<double> residuals(const double* outcome, const std::vector<double>& b) const {
        std::vector<double> e(static_cast<size_t>(n_));
        la::residual(design_.data(), n_, k_, b.data(), outcome, e.data());
        return e;
    }
    // The conventional covariance s^2 (x'x)^-1 of the regression of `outcome`.
    std::vector<double> conventional_cov(const double* outcome) const {
        const std::vector<double> e = residuals(outcome, coefficients(outcome));
        const double s2 = la::sumsq(e.data(), n_) / static_cast<double>(n_ - k_);
        std::vector<double> cov = inv_xx_;
        for (double& v : cov) v *= s2;
        return cov;
    }

private:
    std::vector<double> design_;
    int64_t n_;
    int k_;
    std::vector<double> inv_xx_;
    bool invertible_ = false;
};

struct TwostepProbit {
    std::vector<double> b, V;
};

// y on `design` (constant last).
TwostepProbit twostep_probit(const std::vector<double>& design, int64_t n, int k, const double* y) {
    const ProbitResult fit = probit_fit_ml(design.data(), n, k, y, MlMaxOptions());
    if (!fit.converged || fit.separated_perfect)
        throw EngineError("no_convergence",
            "ivprobit, twostep: a probit step did not converge or predicts the outcome perfectly");
    return {fit.beta, probit_observed_info(fit, design.data(), n, k, y).bread};
}

// b' inverse(V[which, which]) b on the listed coefficients; false when not invertible.
bool wald_on(const std::vector<double>& b, const std::vector<double>& V, int k, const std::vector<int>& which,
             double& chi2) {
    const int m = static_cast<int>(which.size());
    const size_t K = static_cast<size_t>(k), M = static_cast<size_t>(m);
    std::vector<double> block(M * M), sub_b(M), solved(M);
    for (size_t a = 0; a < M; ++a) {
        sub_b[a] = b[static_cast<size_t>(which[a])];
        for (size_t c = 0; c < M; ++c)
            block[c * M + a] = V[static_cast<size_t>(which[c]) * K + static_cast<size_t>(which[a])];
    }
    if (la::spd_inverse(block.data(), m) != 0) return false;
    la::matvec(block.data(), m, sub_b.data(), solved.data());
    chi2 = 0.0;
    for (size_t a = 0; a < M; ++a) chi2 += sub_b[a] * solved[a];
    return true;
}

// A row of the coefficient table.
json twostep_coef_row(const std::string& name, double b, double se, double zc) {
    const bool has_se = se > 0.0;
    const double z = has_se ? b / se : 0.0;
    return {{"name", name}, {"b", b}, {"se", se}, {"z", z}, {"p", has_se ? z_twosided_pvalue(z) : 1.0},
            {"ci_lo", b - zc * se}, {"ci_hi", b + zc * se}};
}

int run_ivprobit_twostep(const GlmInputs& g, const std::vector<int>& ie, const std::vector<int>& iz,
                         const std::vector<int>& ix, int icons,
                         const std::vector<std::string>& endog,
                         const std::vector<std::string>& instr, bool level_set, double level,
                         double zc, bool emit_v, const std::string& outpath) {
    const int64_t n = g.n;
    const double* y = g.y.data();
    const int n_endog = static_cast<int>(ie.size()), n_incl = static_cast<int>(ix.size());
    const int n_instr = static_cast<int>(iz.size());
    const int kx = n_instr + n_incl + 1, ks = n_endog + n_incl + 1;
    const size_t KX = static_cast<size_t>(kx), KS = static_cast<size_t>(ks);
    const auto column = [&](int j) { return g.X.data() + static_cast<std::ptrdiff_t>(j) * n; };
    const auto columns_of = [&](const std::vector<int>& ids) {
        std::vector<const double*> out;
        for (int j : ids) out.push_back(column(j));
        return out;
    };
    const std::vector<const double*> endog_cols = columns_of(ie), incl_cols = columns_of(ix),
                                     instr_cols = columns_of(iz);
    const double* constant = column(icons);
    const auto concat = [](std::initializer_list<std::vector<const double*>> parts) {
        std::vector<const double*> out;
        for (const auto& part : parts) out.insert(out.end(), part.begin(), part.end());
        return out;
    };

    const FixedDesignOls on_x(gather_columns(concat({instr_cols, incl_cols, {constant}}), n), n, kx);
    if (!on_x.invertible())
        throw EngineError("bad_input", "ivprobit: the instruments and exogenous regressors are collinear");

    // First stages: D (kx x ks) holds Pi for the endogenous columns and picks the included
    // exogenous and the constant out of x for the others; the residuals move to the probits.
    std::vector<double> D(KX * KS, 0.0);
    std::vector<std::vector<double>> first_stage_resid;
    std::vector<const double*> resid_cols;
    for (int j = 0; j < n_endog; ++j) {
        const std::vector<double> pi_j = on_x.coefficients(endog_cols[static_cast<size_t>(j)]);
        std::copy(pi_j.begin(), pi_j.end(), D.begin() + static_cast<std::ptrdiff_t>(static_cast<size_t>(j) * KX));
        first_stage_resid.push_back(on_x.residuals(endog_cols[static_cast<size_t>(j)], pi_j));
    }
    for (const auto& e : first_stage_resid) resid_cols.push_back(e.data());
    for (int t = 0; t <= n_incl; ++t)
        D[static_cast<size_t>(n_endog + t) * KX + static_cast<size_t>(n_instr + t)] = 1.0;

    // The reduced-form probit gives alpha (x's coefficients), lambda (the residuals')
    // and alpha's covariance, which starts Omega.
    const int k_rf = kx + n_endog;
    const TwostepProbit rf = twostep_probit(
        gather_columns(concat({instr_cols, incl_cols, resid_cols, {constant}}), n), n, k_rf, y);
    const auto rf_position = [&](size_t x_pos) {  // x's constant sits past the residuals
        return x_pos + 1 == KX ? static_cast<size_t>(k_rf - 1) : x_pos;
    };
    std::vector<double> alpha(KX), omega(KX * KX);
    for (size_t a = 0; a < KX; ++a) {
        alpha[a] = rf.b[rf_position(a)];
        for (size_t c = 0; c < KX; ++c)
            omega[c * KX + a] = rf.V[rf_position(c) * static_cast<size_t>(k_rf) + rf_position(a)];
    }

    // The 2SIV probit gives the endogenous coefficients and the test of exogeneity.
    const int k_siv = n_endog + n_incl + n_endog + 1;
    const TwostepProbit siv = twostep_probit(
        gather_columns(concat({endog_cols, incl_cols, resid_cols, {constant}}), n), n, k_siv, y);
    std::vector<int> resid_at;
    for (int j = 0; j < n_endog; ++j) resid_at.push_back(n_endog + n_incl + j);
    double chi2_exog = 0.0;
    if (!wald_on(siv.b, siv.V, k_siv, resid_at, chi2_exog))
        throw EngineError("no_convergence", "ivprobit, twostep: the test of exogeneity is not defined");

    // Omega's other term: the regression of sum_j y2_j (lambda_j - beta_j) on x.
    std::vector<double> mixed(static_cast<size_t>(n), 0.0);
    for (int j = 0; j < n_endog; ++j) {
        const double gap = rf.b[static_cast<size_t>(kx - 1 + j)] - siv.b[static_cast<size_t>(j)];
        const double* y2 = endog_cols[static_cast<size_t>(j)];
        for (int64_t i = 0; i < n; ++i) mixed[static_cast<size_t>(i)] += y2[i] * gap;
    }
    const std::vector<double> mixed_cov = on_x.conventional_cov(mixed.data());
    for (size_t i = 0; i < omega.size(); ++i) omega[i] += mixed_cov[i];

    // The GLS solution.
    if (la::spd_inverse(omega.data(), kx) != 0)
        throw EngineError("no_convergence", "ivprobit, twostep: Omega is not positive definite");
    std::vector<double> weighted_D(KX * KS), weighted_alpha(KX), V(KS * KS), normal_rhs(KS);
    for (size_t c = 0; c < KS; ++c) la::matvec(omega.data(), kx, D.data() + c * KX, weighted_D.data() + c * KX);
    la::matvec(omega.data(), kx, alpha.data(), weighted_alpha.data());
    for (size_t a = 0; a < KS; ++a) {
        const double* da = D.data() + a * KX;
        normal_rhs[a] = la::dot(da, weighted_alpha.data(), kx);
        for (size_t c = 0; c < KS; ++c) V[c * KS + a] = la::dot(da, weighted_D.data() + c * KX, kx);
    }
    if (la::spd_inverse(V.data(), ks) != 0)
        throw EngineError("no_convergence", "ivprobit, twostep: D' Omega^-1 D is not positive definite");
    std::vector<double> delta(KS);
    la::matvec(V.data(), ks, normal_rhs.data(), delta.data());

    // ---- the reply ----
    std::vector<std::string> names;
    for (int j : ie) names.push_back(g.names[static_cast<size_t>(j)]);
    for (int j : ix) names.push_back(g.names[static_cast<size_t>(j)]);
    names.push_back("_cons");
    json coefs = json::array();
    for (size_t c = 0; c < KS; ++c)
        coefs.push_back(twostep_coef_row(names[c], delta[c], std::sqrt(std::max(0.0, V[c * KS + c])), zc));

    const int df_model = ks - 1;
    std::vector<int> slope_at(static_cast<size_t>(df_model));
    for (int c = 0; c < df_model; ++c) slope_at[static_cast<size_t>(c)] = c;
    double chi2 = 0.0;  // stays 0 when the slopes' covariance has no inverse
    if (df_model > 0) wald_on(delta, V, ks, slope_at, chi2);

    // Rows the fitted structural index determines completely. The bound was bracketed
    // on StataBE (e(N_cds) between indices 17.92 and 18.21 on the BUG-1500 data).
    constexpr double kDeterminedIndex = 18.0;
    const std::vector<double> z_design = gather_columns(concat({endog_cols, incl_cols, {constant}}), n);
    std::vector<double> index(static_cast<size_t>(n));
    la::mat_vec(z_design.data(), n, ks, delta.data(), index.data());
    int64_t n_cds = 0, n_cdf = 0;
    for (double v : index) {
        n_cds += v > kDeterminedIndex;
        n_cdf += v < -kDeterminedIndex;
    }

    json exogenous = json::array();
    for (int j : ix) exogenous.push_back(g.names[static_cast<size_t>(j)]);
    for (int j : iz) exogenous.push_back(g.names[static_cast<size_t>(j)]);
    json out = {{"estimator", "ivprobit"}, {"method", "twostep"}, {"vce", "twostep"},
                {"n_obs", n}, {"n_dropped_missing", g.n_dropped}, {"coefs", coefs},
                {"endogenous", endog}, {"instruments", instr}, {"exogenous", exogenous},
                {"chi2", chi2}, {"df_chi2", df_model}, {"df_m", df_model},
                {"p_chi2", chi2_pvalue(chi2, df_model)},
                {"chi2_exog", chi2_exog}, {"df_exog", n_endog}, {"p_exog", chi2_pvalue(chi2_exog, n_endog)},
                {"k_vars", ks}, {"N_cds", n_cds}, {"N_cdf", n_cdf}};
    if (level_set) out["level"] = level;
    if (emit_v) {
        out["e_b"] = {{"names", names}, {"values", delta}};
        add_e_v(out, names, V);
    }
    add_warnings(out, g.warnings);
    emit(out, outpath);
    return 0;
}

int run_ivml(const std::vector<std::string>& a, IvKind kind, const char* cmd) {
    std::string data = opt(a, "--data"), y = opt(a, "--y"), xcsv = opt(a, "--x");
    std::string endog_csv = opt(a, "--endog"), instr_csv = opt(a, "--instruments");
    // --vce default "iid": opt() with a default leaves an omitted flag unconsumed,
    // so a plain invocation keeps the model-based (observed-information) SEs.
    std::string vce = opt(a, "--vce", "iid");
    std::string cluster_var = opt(a, "--cluster");
    const bool is_tobit = (kind == IvKind::Tobit);
    std::string lls = is_tobit ? opt(a, "--ll") : std::string();
    std::string uls = is_tobit ? opt(a, "--ul") : std::string();
    // BUG-1500: ivprobit's Newey two-step estimator (run_ivprobit_twostep).
    const bool twostep = !is_tobit && has(a, "--twostep");

    if (data_missing(data) || y.empty() || endog_csv.empty() || instr_csv.empty()) {
        std::fprintf(stderr,
                     "usage: cobra %s --data f.hbin --y Y --endog E --instruments Z1,Z2 [--x X1,X2]\n"
                     "       %s[--vce iid|robust|cluster] [--cluster C] [--level P] [--keepif EXPR] [--out f.json]\n"
                     "  %s with an endogenous regressor, by full-information ML:\n"
                     "    Y* = E*a + X*b + u ;  E = X*p1 + Z*p2 + v ;  (u,v) bivariate normal\n"
                     "  --endog E                the endogenous regressor (exactly one)\n"
                     "  --instruments Z1,Z2      the EXCLUDED instruments (--x stays in both equations)\n"
                     "%s"
                     "  --vce cluster            needs --cluster C.\n",
                     cmd, std::string(std::string(cmd).size() + 8, ' ').c_str(),
                     is_tobit ? "tobit" : "probit",
                     is_tobit ? "  --ll L / --ul U          censoring limits; at least one is required\n"
                              : "  --twostep                Newey's two-step estimator instead (one or more --endog)\n");
        return 2;
    }
    if (vce != "iid" && vce != "robust" && vce != "cluster") {
        std::fprintf(stderr, "%s: --vce must be one of iid|robust|cluster (got %s).\n", cmd, vce.c_str());
        return 2;
    }
    if (twostep && vce != "iid")
        throw EngineError("bad_input", std::string(cmd) + ": --twostep has its own covariance; "
                          "--vce robust|cluster is the ML estimator's");
    if (vce == "cluster" && cluster_var.empty()) {
        std::fprintf(stderr, "%s: --vce cluster requires --cluster C.\n", cmd);
        return 2;
    }
    bool level_set = has(a, "--level");
    double level = std::stod(opt(a, "--level", "95"));
    if (level_set && (level < 10.0 || level > 99.99)) {
        std::fprintf(stderr, "%s: --level must be in [10, 99.99] (got %s).\n", cmd,
                     opt(a, "--level").c_str());
        return 2;
    }
    // Asymptotic z inference throughout — Stata's IV-ML models report z, not t.
    const double zc = z_critical(level == 95.0 ? 0.05 : 1.0 - level / 100.0);

    const bool has_ll = !lls.empty(), has_ul = !uls.empty();
    const double ll_lim = has_ll ? std::stod(lls) : 0.0;
    const double ul_lim = has_ul ? std::stod(uls) : 0.0;
    if (is_tobit && !has_ll && !has_ul)
        throw EngineError("bad_input",
            std::string(cmd) + ": at least one of --ll L / --ul U is required (an uncensored "
            "outcome is `ivregress`, not ivtobit)");
    if (is_tobit && has_ll && has_ul && !(ll_lim < ul_lim))
        throw EngineError("bad_input", std::string(cmd) + ": --ll must be strictly below --ul");

    std::vector<std::string> endog = split_csv(endog_csv);
    std::vector<std::string> instr = split_csv(instr_csv);
    std::vector<std::string> exog = xcsv.empty() ? std::vector<std::string>() : split_csv(xcsv);
    // ONE endogenous regressor. With p > 1 the conditional distribution of u given
    // the reduced-form residuals is driven by a full p×p Σ_v and a p-vector of
    // correlations, which is a different likelihood — not a loop over this one. An
    // honest reject is parity; a silent single-endog answer to a two-endog question
    // is not.
    if (endog.size() != 1 && !(twostep && !endog.empty()))
        throw EngineError("unsupported",
            std::string(cmd) + ": exactly one endogenous regressor is supported (got " +
            std::to_string(endog.size()) + "); multiple endogenous regressors need the "
            "multivariate reduced form, which this build does not implement");
    if (instr.size() < endog.size())
        throw EngineError("bad_input",
            std::string(cmd) + ": the model is under-identified — " + std::to_string(instr.size()) +
            " excluded instrument(s) for " + std::to_string(endog.size()) + " endogenous regressor(s)");
    for (const auto& v : endog)
        if (v.find_first_of(".#()") != std::string::npos)
            throw EngineError("unsupported",
                std::string(cmd) + ": --endog takes plain variable names (got '" + v + "')");
    for (const auto& v : instr)
        if (v.find_first_of(".#()") != std::string::npos)
            throw EngineError("unsupported",
                std::string(cmd) + ": --instruments takes plain variable names (got '" + v + "')");
    for (const auto& v : exog)
        for (const auto& e : endog)
            if (v == e)
                throw EngineError("bad_input",
                    std::string(cmd) + ": '" + v + "' is listed in both --x and --endog");
    for (const auto& v : exog)
        for (const auto& z : instr)
            if (v == z)
                throw EngineError("bad_input",
                    std::string(cmd) + ": '" + v + "' is listed in both --x and --instruments");

    data = maybe_keepif(data, opt(a, "--keepif"));
    const Dataset& ds = acquire_data(data);
    std::vector<std::string> cluster_names;
    if (!cluster_var.empty()) cluster_names.push_back(cluster_var);
    // One load, one listwise sample, for BOTH equations.
    std::vector<std::string> allx = endog;
    allx.insert(allx.end(), instr.begin(), instr.end());
    allx.insert(allx.end(), exog.begin(), exog.end());
    GlmInputs g = load_glm(ds, cmd, y, allx, cluster_names);
    require_sample(g.n);  // BUG-1269: r(2000) "no observations" / r(2001) for one row
    if (g.k < 1 || g.names.back() != "(intercept)")
        throw EngineError("bad_input", std::string(cmd) + ": internal design has no intercept column");
    if (!is_tobit && !check_binary(g.y, cmd, y)) return 2;
    if (vce == "cluster" && (g.cluster.empty() || g.cluster[0].n_levels < 2))
        throw EngineError("bad_input",
            std::string(cmd) + ": vce(cluster) requires at least 2 clusters, but the cluster "
            "variable has " + std::to_string(g.cluster.empty() ? 0 : g.cluster[0].n_levels));

    // Column bookkeeping. Every requested endog/instrument must still be present —
    // if build_model dropped one as collinear, say so instead of silently fitting a
    // different model than the one asked for.
    const int icons = g.k - 1;
    std::vector<int> ie, iz, ix;
    std::vector<bool> claimed(static_cast<size_t>(g.k), false);
    claimed[static_cast<size_t>(icons)] = true;
    for (const auto& v : endog) {
        int j = find_name(g.names, v);
        if (j < 0 || j == icons)
            throw EngineError("bad_input",
                std::string(cmd) + ": endogenous regressor '" + v + "' is not in the fitted design "
                "(collinear or dropped)");
        ie.push_back(j);
        claimed[static_cast<size_t>(j)] = true;
    }
    for (const auto& v : instr) {
        int j = find_name(g.names, v);
        if (j < 0 || j == icons)
            throw EngineError("bad_input",
                std::string(cmd) + ": instrument '" + v + "' is not in the fitted design "
                "(collinear or dropped)");
        iz.push_back(j);
        claimed[static_cast<size_t>(j)] = true;
    }
    for (int j = 0; j < g.k; ++j)
        if (!claimed[static_cast<size_t>(j)]) ix.push_back(j);  // expanded --x, in load order

    // ivprobit's perfect-prediction screen over the structural regressors, included
    // exogenous first, as `probit` itself screens them ([R] logit, Model identification).
    std::vector<char> x_omitted(ix.size(), 0);
    if (!is_tobit) {
        std::vector<int> scol(ix);
        scol.insert(scol.end(), ie.begin(), ie.end());
        scol.push_back(icons);
        const int kscr = static_cast<int>(scol.size());
        std::vector<double> Xc(static_cast<size_t>(g.n) * kscr);
        for (int c = 0; c < kscr; ++c)
            std::copy(g.X.begin() + static_cast<std::ptrdiff_t>(scol[static_cast<size_t>(c)]) * g.n,
                      g.X.begin() + static_cast<std::ptrdiff_t>(scol[static_cast<size_t>(c)] + 1) * g.n,
                      Xc.begin() + static_cast<std::ptrdiff_t>(c) * g.n);
        const BinaryPerfectScreen s = binary_perfect_screen(Xc.data(), g.n, kscr, kscr - 1, g.y.data());
        if (s.inestimable) {
            const BinaryPerfectRule& r = s.rules.back();
            std::fprintf(stderr, "%s: outcome = %s %s %.17g predicts data perfectly; r(2000)\n", cmd,
                         g.names[static_cast<size_t>(scol[static_cast<size_t>(r.col)])].c_str(),
                         r.op == 2 ? ">" : "<", r.rhs);
            return 2;
        }
        if (twostep) {
            std::vector<int> rcol(iz);
            rcol.insert(rcol.end(), ix.begin(), ix.end());
            rcol.push_back(icons);
            const int krf = static_cast<int>(rcol.size());
            std::vector<double> Xr(static_cast<size_t>(g.n) * krf);
            for (int c = 0; c < krf; ++c)
                std::copy(g.X.begin() + static_cast<std::ptrdiff_t>(rcol[static_cast<size_t>(c)]) * g.n,
                          g.X.begin() + static_cast<std::ptrdiff_t>(rcol[static_cast<size_t>(c)] + 1) * g.n,
                          Xr.begin() + static_cast<std::ptrdiff_t>(c) * g.n);
            const BinaryPerfectScreen rs = binary_perfect_screen(Xr.data(), g.n, krf, krf - 1, g.y.data());
            if (rs.inestimable) {
                const BinaryPerfectRule& r = rs.rules.back();
                std::fprintf(stderr, "%s: outcome = %s %s %.17g predicts data perfectly; r(2000)\n", cmd,
                             g.names[static_cast<size_t>(rcol[static_cast<size_t>(r.col)])].c_str(),
                             r.op == 2 ? ">" : "<", r.rhs);
                return 2;
            }
            if (!s.rules.empty() || !rs.rules.empty())
                throw EngineError("unsupported",
                    std::string(cmd) + ", twostep: a regressor predicts the outcome perfectly; this "
                    "build does not drop it from the two-step fit");
            return run_ivprobit_twostep(g, ie, iz, ix, icons, endog, instr, level_set, level, zc,
                                        has(a, "--emit-v"), opt(a, "--out"));
        }
        for (const BinaryPerfectRule& r : s.rules) {
            const size_t j = static_cast<size_t>(r.col);
            const std::string& nm = g.names[static_cast<size_t>(scol[j])];
            if (j >= ix.size())
                throw EngineError("bad_input",
                    std::string(cmd) + ": " + nm + " predicts the outcome perfectly; "
                    "may not drop an endogenous regressor");
            x_omitted[j] = 1;
            if (r.op == 1)
                std::fprintf(stderr, "note: %s != %.17g predicts %s perfectly; %s omitted and %lld obs not used.\n",
                             nm.c_str(), r.rhs, r.interp == 1 ? "success" : "failure", nm.c_str(),
                             static_cast<long long>(r.dropped));
            else
                std::fprintf(stderr, "note: outcome = %s %s %.17g predicts data perfectly except for %s == %.17g "
                             "subsample; %s omitted and %lld obs not used.\n", nm.c_str(), r.op == 2 ? ">" : "<",
                             r.rhs, nm.c_str(), r.rhs, nm.c_str(), static_cast<long long>(r.dropped));
        }
        if (!s.rules.empty()) glm_keep_rows(g, s.keep_row);
    }
    // The exogenous regressors the fit keeps (ix_all keeps every one, for the stripe).
    const std::vector<int> ix_all = ix;
    ix.clear();
    for (size_t j = 0; j < ix_all.size(); ++j)
        if (!x_omitted[j]) ix.push_back(ix_all[j]);

    const int ne = static_cast<int>(ie.size()), nz = static_cast<int>(iz.size());
    const int nx = static_cast<int>(ix.size());
    const int ks = ne + nx + 1, kw = nx + nz + 1;
    const int64_t n = g.n;
    std::vector<double> Xs(static_cast<size_t>(n) * ks), Xw(static_cast<size_t>(n) * kw);
    std::vector<std::string> sname, wname;
    auto put = [&](std::vector<double>& D, int dst, int src) {
        const double* s = g.X.data() + static_cast<int64_t>(src) * n;
        std::copy(s, s + n, D.data() + static_cast<int64_t>(dst) * n);
    };
    { int c = 0;
      for (int j : ie) { put(Xs, c++, j); sname.push_back(g.names[static_cast<size_t>(j)]); }
      for (int j : ix) { put(Xs, c++, j); sname.push_back(g.names[static_cast<size_t>(j)]); }
      put(Xs, c, icons); sname.push_back("_cons"); }
    { int c = 0;
      for (int j : ix) { put(Xw, c++, j); wname.push_back(g.names[static_cast<size_t>(j)]); }
      for (int j : iz) { put(Xw, c++, j); wname.push_back(g.names[static_cast<size_t>(j)]); }
      put(Xw, c, icons); wname.push_back("_cons"); }
    // y2 = the endogenous regressor's own column (Xs's first column, by construction).
    const double* y2 = Xs.data();

    IvmlResult r = is_tobit
        ? ivtobit_fit(Xs.data(), Xw.data(), n, ks, kw, g.y.data(), y2, has_ll, ll_lim, has_ul, ul_lim)
        : ivprobit_fit(Xs.data(), Xw.data(), n, ks, kw, g.y.data(), y2);
    if (!r.ok)
        throw EngineError("no_convergence",
            std::string(cmd) + ": MLE failed (singular information matrix or non-convergent fit).");
    const int np = r.np;

    // V = bread (iid) or a sandwich bread·meat·bread over the per-observation score
    // rows of the FULL θ — structural δ, reduced-form Π, athrho and the ln σ's all
    // in one block, so corr and sd inherit the robust variance too (Stata prints
    // "Robust std. err." over those rows as well, not just the coefficients).
    //
    // SMALL-SAMPLE FACTOR. Stata's ML multiplier, NOT the regression-style
    // (N−1)/(N−K)·G/(G−1) that vcov.cpp applies to feols/2SLS:
    //   vce(robust)  → N/(N−1)
    //   vce(cluster) → G/(G−1)
    // Same P5 precedent as logit/probit/tobit (cmds_mle.cpp) and the P7 ordered
    // models, and VERIFIED here rather than assumed — tests/unit/test_ivml.cpp
    // asserts every robust SE of the P7 oracle `ivprobit ybin x1 (endog=z),
    // vce(robust)` (N=4,800) and `ivtobit y x1 (endog=z), ll(0) vce(robust)`
    // (N=4,778) against N/(N−1) and matches within the wave tolerance; dropping the
    // factor moves se(endog) to .0386537 against the oracle's .0386577, a 1.0e-4
    // relative miss that those assertions reject.
    std::vector<double> V = r.vcov;
    int n_clusters = 0;
    if (vce == "robust" || vce == "cluster") {
        std::vector<double> S = is_tobit
            ? ivtobit_scores(Xs.data(), Xw.data(), n, ks, kw, g.y.data(), y2, has_ll, ll_lim,
                             has_ul, ul_lim, r)
            : ivprobit_scores(Xs.data(), Xw.data(), n, ks, kw, g.y.data(), y2, r);
        int64_t rows = n;
        const Factor* fac = nullptr;
        double factor;
        if (vce == "cluster") {
            fac = &g.cluster[0];
            n_clusters = fac->n_levels;
            rows = n_clusters;
            factor = static_cast<double>(n_clusters) / (static_cast<double>(n_clusters) - 1.0);
        } else {
            factor = n > 1 ? static_cast<double>(n) / (static_cast<double>(n) - 1.0) : 1.0;
        }
        std::vector<double> M(static_cast<size_t>(rows) * np, 0.0);
        for (int64_t i = 0; i < n; ++i) {
            int64_t rr = fac ? fac->codes[static_cast<size_t>(i)] : i;
            for (int p = 0; p < np; ++p)
                M[static_cast<int64_t>(p) * rows + rr] += S[static_cast<int64_t>(p) * n + i];
        }
        std::vector<double> meat(static_cast<size_t>(np) * np, 0.0);
        la::xtx(M.data(), rows, np, meat.data());
        std::vector<double> tmp(static_cast<size_t>(np) * np, 0.0);
        V.assign(static_cast<size_t>(np) * np, 0.0);
        la::sandwich(r.vcov.data(), meat.data(), np, factor, V.data(), tmp.data());
    }
    auto sd = [&](int p) { return std::sqrt(std::max(0.0, V[static_cast<size_t>(p) * np + p])); };
    auto zp = [](double z) {
        return z_twosided_pvalue(z);
    };

    json coefs = json::array();
    for (int c = 0; c < ks; ++c) {
        double b = r.beta[static_cast<size_t>(c)], se = sd(c);
        double z = (se > 0) ? b / se : 0.0;
        coefs.push_back({{"name", sname[static_cast<size_t>(c)]}, {"b", b}, {"se", se}, {"z", z},
                         {"p", (se > 0) ? zp(z) : 1.0}, {"ci_lo", b - zc * se}, {"ci_hi", b + zc * se}});
    }
    json rf = json::array();
    for (int c = 0; c < kw; ++c) {
        double b = r.pi[static_cast<size_t>(c)], se = sd(ks + c);
        double z = (se > 0) ? b / se : 0.0;
        rf.push_back({{"name", wname[static_cast<size_t>(c)]}, {"b", b}, {"se", se}, {"z", z},
                      {"p", (se > 0) ? zp(z) : 1.0}, {"ci_lo", b - zc * se}, {"ci_hi", b + zc * se}});
    }
    // A screened-out exogenous regressor keeps its place in both equations as an
    // omitted term (_b 0, _se 0, no z / p / interval): after the endogenous one in
    // the structural table, first in the reduced form.
    for (size_t j = 0; j < ix_all.size(); ++j) {
        if (!x_omitted[j]) continue;
        const json row = {{"name", g.names[static_cast<size_t>(ix_all[j])]}, {"b", 0.0}, {"se", 0.0},
                          {"z", nullptr}, {"p", nullptr}, {"ci_lo", nullptr}, {"ci_hi", nullptr}};
        coefs.insert(coefs.begin() + static_cast<std::ptrdiff_t>(ne + j), row);
        rf.insert(rf.begin() + static_cast<std::ptrdiff_t>(j), row);
    }

    // ANCILLARY ROWS. Reported in their TRANSFORMED metrics, exactly as Stata does:
    //   corr = tanh(athrho), se = (1−corr²)·se(athrho)   [delta method]
    //        CI = tanh(athrho ± z·se(athrho))            [NOT corr ± z·se(corr)]
    //   sd   = exp(lnσ),     se = sd·se(lnσ)             [delta method]
    //        CI = exp(lnσ ± z·se(lnσ))                   [NOT sd ± z·se(sd)]
    // The CIs are built by transforming the endpoints of the SYMMETRIC interval in
    // θ's own metric, which is why they come out asymmetric around the point
    // estimate — the oracle's corr CI [−.1364806, −.0072521] around −.0721692 is
    // .0643 below and .0649 above, and a b±z·se interval would be neither.
    const double ath = r.theta[static_cast<size_t>(r.i_athrho)], se_ath = sd(r.i_athrho);
    const double rho = std::tanh(ath);
    json corr = {{"name", "corr(e." + endog[0] + ",e." + y + ")"},
                 {"b", rho}, {"se", (1.0 - rho * rho) * se_ath},
                 {"ci_lo", std::tanh(ath - zc * se_ath)}, {"ci_hi", std::tanh(ath + zc * se_ath)},
                 {"athrho", ath}, {"se_athrho", se_ath}};
    json sds = json::array();
    auto sd_row = [&](const char* nm, int idx) {
        const double l = r.theta[static_cast<size_t>(idx)], sl = sd(idx);
        return json{{"name", nm}, {"b", std::exp(l)}, {"se", std::exp(l) * sl},
                    {"ci_lo", std::exp(l - zc * sl)}, {"ci_hi", std::exp(l + zc * sl)}};
    };
    if (is_tobit) sds.push_back(sd_row(("sd(e." + y + ")").c_str(), r.i_lnse));
    sds.push_back(sd_row(("sd(e." + endog[0] + ")").c_str(), r.i_lnsv));

    // Model Wald χ² over the structural SLOPES only — [endog, x…], excluding the
    // constant. That is Stata's chi2(2) here (endog + x1), not chi2(3).
    const int nslope = ks - 1;
    double chi2 = 0;
    if (nslope > 0) {
        std::vector<double> Vs(static_cast<size_t>(nslope) * nslope);
        for (int i = 0; i < nslope; ++i)
            for (int j = 0; j < nslope; ++j)
                Vs[static_cast<size_t>(i) * nslope + j] = V[static_cast<size_t>(i) * np + j];
        if (la::spd_inverse(Vs.data(), nslope) == 0)
            for (int i = 0; i < nslope; ++i)
                for (int j = 0; j < nslope; ++j)
                    chi2 += r.beta[static_cast<size_t>(i)] * Vs[static_cast<size_t>(i) * nslope + j] *
                            r.beta[static_cast<size_t>(j)];
    }
    // Wald test of exogeneity (corr = 0): the squared t-ratio of athrho, on χ²(1).
    // Taken in ATHRHO's metric — the delta-method se(corr) cancels against the
    // delta-method corr in the ratio only to first order, and Stata's printed
    // chi2(1) = 4.75 on the probit leg matches the athrho ratio, so this is the
    // form, not an equivalent-looking rewrite.
    const double chi2_exog = (se_ath > 0) ? (ath / se_ath) * (ath / se_ath) : 0.0;

    json out = {{"estimator", cmd}, {"n_obs", n}, {"n_dropped_missing", g.n_dropped},
                {"vce", vce}, {"coefs", coefs}, {"rf_coefs", rf},
                {"corr", corr}, {"sd", sds},
                {"endogenous", endog}, {"instruments", instr},
                {"ll", r.ll}, {"ll_exogenous", r.ll_exog},
                {"chi2", chi2}, {"df_chi2", nslope}, {"p_chi2", chi2_pvalue(chi2, nslope)},
                {"chi2_exogeneity", chi2_exog}, {"p_exogeneity", chi2_pvalue(chi2_exog, 1)},
                {"iters", r.iters}, {"converged", r.converged}};
    // The footer's "Exogenous:" list — the reduced form's regressors minus the
    // constant, i.e. the included exogenous x followed by the excluded instruments.
    { json ex = json::array();
      for (int c = 0; c < kw - 1; ++c) ex.push_back(wname[static_cast<size_t>(c)]);
      out["exogenous"] = ex; }
    if (is_tobit) {
        out["n_uncensored"] = r.n_unc;
        out["n_left_censored"] = r.n_lc;
        out["n_right_censored"] = r.n_rc;
        if (has_ll) out["ll_limit"] = ll_lim;
        if (has_ul) out["ul_limit"] = ul_lim;
    }
    if (vce == "cluster") { out["cluster_var"] = cluster_var; out["n_clusters"] = n_clusters; }
    if (level_set) out["level"] = level;
    if (has(a, "--emit-v")) {
        // A screened-out exogenous regressor is `<eq>:o.<name>` at 0 with a zero
        // row and column (order entry kNone), in its place in both equations.
        const size_t kNone = static_cast<size_t>(-1);
        std::vector<std::string> names;
        std::vector<size_t> order;
        // One equation: its leading columns [0, lead), the exogenous block (kept ones
        // from column `at` on, omitted ones in their places), then the rest to `k`.
        auto put_eq = [&](const std::string& eq, const std::vector<std::string>& nm, size_t base,
                          int lead, int at, int k) {
            for (int c = 0; c < lead; ++c) {
                names.push_back(eq + ":" + nm[static_cast<size_t>(c)]);
                order.push_back(base + static_cast<size_t>(c));
            }
            int c = at;
            for (size_t j = 0; j < ix_all.size(); ++j) {
                if (x_omitted[j]) {
                    names.push_back(eq + ":o." + g.names[static_cast<size_t>(ix_all[j])]);
                    order.push_back(kNone);
                } else {
                    names.push_back(eq + ":" + nm[static_cast<size_t>(c)]);
                    order.push_back(base + static_cast<size_t>(c++));
                }
            }
            for (; c < k; ++c) {
                names.push_back(eq + ":" + nm[static_cast<size_t>(c)]);
                order.push_back(base + static_cast<size_t>(c));
            }
        };
        put_eq(y, sname, 0, ne, ne, ks);                                // [endog, x..., _cons]
        put_eq(endog[0], wname, static_cast<size_t>(ks), 0, 0, kw);     // [x..., z..., _cons]
        names.push_back("/:athrho2_1");
        order.push_back(static_cast<size_t>(r.i_athrho));
        if (is_tobit) {
            names.push_back("/:lnsigma1");
            order.push_back(static_cast<size_t>(r.i_lnse));
        }
        names.push_back("/:lnsigma2");
        order.push_back(static_cast<size_t>(r.i_lnsv));
        json values = json::array();
        for (size_t i : order) values.push_back(i == kNone ? 0.0 : r.theta[i]);
        out["e_b"] = {{"names", names}, {"values", std::move(values)}};
        // add_e_v indexes both names and V by order; names here already follow
        // the requested order, so pass the correspondingly permuted matrix.
        std::vector<double> fullV(order.size() * order.size());
        for (size_t c = 0; c < order.size(); ++c)
            for (size_t row = 0; row < order.size(); ++row)
                fullV[row + c * order.size()] =
                    (order[row] == kNone || order[c] == kNone) ? 0.0 : V[order[row] + order[c] * np];
        add_e_v(out, names, fullV);
    }
    add_warnings(out, g.warnings);
    emit(out, opt(a, "--out"));
    return 0;
}

}  // namespace

int cmd_ivprobit(const std::vector<std::string>& a) { return run_ivml(a, IvKind::Probit, "ivprobit"); }

int cmd_ivtobit(const std::vector<std::string>& a) { return run_ivml(a, IvKind::Tobit, "ivtobit"); }

}  // namespace cli
}  // namespace cecon
