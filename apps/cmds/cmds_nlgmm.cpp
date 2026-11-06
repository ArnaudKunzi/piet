// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "common.h"
#include "commands.h"

#include "core/expr_eval.h"
#include "core/ident.h"
#include "estimators/nlgmm.h"

// P7 wave — nl, gmm.
//
// Both verbs are driven by a user EXPRESSION rather than a --y/--x varlist, so
// neither uses load_glm: the columns the sample depends on are exactly the ones
// the expression names, and this file does its own listwise deletion over those.
//
//   nl   --expr "(y = {b0=2}+{b1=.5}*x1+{b2=-.2}*x2)"
//        The top-level '=' splits the dependent variable off the model; the
//        sample is the rows where the dependent variable AND every referenced
//        variable are non-missing.
//
//   gmm  --expr "(y-{b0=2}-{b1=.5}*x1-{b2=-.2}*x2)" --instruments x1,x2,z --onestep
//        The whole expression IS the residual. The sample is built the way Stata
//        builds it, in TWO stages, and the two stages are visible in the output:
//        first a markout on the instruments (4,671 rows on the P7 fixture),
//        then the rows whose residual is missing when evaluated at the initial
//        values are dropped (22 more) and reported — Stata's
//        "note: 22 missing values returned for equation 1 at initial values."
//        The count is MEASURED, never assumed; the P7 fixture happens to make
//        the two stages distinguishable because y's 22 missings and x2's 129 are
//        disjoint sets.
//
// Oracle: internal capture p7_stata.log lines 833-886.

namespace cecon {
namespace cli {

namespace {

// Drop a wrapping paren pair, so both Stata's own `(y = ...)` form and a bare
// `y = ...` are accepted. Only strips a paren that actually wraps the WHOLE
// string — "(a)+(b)" is left alone.
std::string strip_outer_parens(std::string s) {
    s = trimx(s);
    while (s.size() >= 2 && s.front() == '(' && s.back() == ')') {
        int d = 0;
        bool wraps = true;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '(') ++d;
            else if (s[i] == ')') {
                if (--d == 0 && i + 1 < s.size()) { wraps = false; break; }
            }
        }
        if (!wraps) break;
        s = trimx(s.substr(1, s.size() - 2));
    }
    return s;
}

// Index of the assignment '=' at nesting depth 0. find_assign() in common.cpp is
// paren-BLIND, which would split "{b0=2}" at the parameter's initial value; this
// one tracks () {} [] depth so only the model's own '=' is found.
size_t top_level_assign(const std::string& s) {
    int paren = 0, brace = 0, brack = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '(') ++paren;
        else if (c == ')') --paren;
        else if (c == '{') ++brace;
        else if (c == '}') --brace;
        else if (c == '[') ++brack;
        else if (c == ']') --brack;
        else if (c == '=' && paren == 0 && brace == 0 && brack == 0) {
            if (i + 1 < s.size() && s[i + 1] == '=') { ++i; continue; }   // ==
            if (i > 0 && (s[i - 1] == '=' || s[i - 1] == '<' || s[i - 1] == '>' || s[i - 1] == '!'))
                continue;                                                 // <= >= != and the tail of ==
            return i;
        }
    }
    return std::string::npos;
}

bool is_var_token(const std::string& s) {
    if (s.empty()) return false;
    if (!cecon::ident::isIdentStartByte(s[0])) return false;
    for (char c : s)
        if (!cecon::ident::isIdentByte(c)) return false;
    return true;
}

// Compile the expression body with the verb's own tag, so every rejection reads
// "nl: ..." / "gmm: ..." and surfaces as an EngineError rather than a bare
// std::runtime_error from deep inside the evaluator.
expr::Program compile_body(const std::string& body, const char* cmd) {
    expr::Options o;
    o.tag = cmd;
    o.allow_params = true;
    o.allow_vars = true;
    o.allow_coefs = false;  // _b[...] refers to a FITTED model; there is none here yet
    try {
        return expr::Program::compile(body, o);
    } catch (const std::runtime_error& ex) {
        throw EngineError("bad_input", ex.what());
    }
}

// Pull the expression's variables (and, for nl, the dependent variable) out of
// the dataset as a row-major n_kept x nv block, so one pointer bump per row
// feeds expr::VarSource.
struct ExprData {
    std::vector<int64_t> keep;
    std::vector<double> vals;  // ROW-major: row i's variables at vals[i*nv]
    int nv = 0;
    int64_t n = 0;
};

// `listwise_vars` decides whether the EXPRESSION's own variables take part in
// the listwise deletion. nl says yes; gmm says no, because Stata's gmm marks out
// on the instruments only and then reports the expression's remaining missings
// separately (see the file header). Either way every named column is looked up,
// so a typo is an honest missing_column error rather than a quietly smaller N.
ExprData gather_expr_vars(const Dataset& ds, const char* cmd,
                          const std::vector<std::string>& vars,
                          const std::vector<std::string>& extra_listwise, bool listwise_vars) {
    ExprData d;
    d.nv = static_cast<int>(vars.size());
    std::vector<const double*> vptr;
    vptr.reserve(vars.size());
    for (const std::string& v : vars) vptr.push_back(require_num(ds, cmd, v));
    std::vector<const double*> lw;
    if (listwise_vars) lw = vptr;
    for (const std::string& v : extra_listwise) lw.push_back(require_num(ds, cmd, v));
    d.keep = listwise_keep(lw, ds.n_rows());
    d.n = static_cast<int64_t>(d.keep.size());
    d.vals.assign(static_cast<size_t>(d.n) * static_cast<size_t>(d.nv > 0 ? d.nv : 1), 0.0);
    for (int j = 0; j < d.nv; ++j) {
        const double* src = vptr[static_cast<size_t>(j)];
        for (int64_t i = 0; i < d.n; ++i)
            d.vals[static_cast<size_t>(i) * static_cast<size_t>(d.nv) + static_cast<size_t>(j)] =
                src[d.keep[static_cast<size_t>(i)]];
    }
    return d;
}

std::vector<double> initial_values(const expr::Program& prog) {
    std::vector<double> b0;
    b0.reserve(prog.params().size());
    for (const expr::Param& p : prog.params()) b0.push_back(p.init);
    return b0;
}

json iteration_log(const std::vector<NlIter>& its, const char* key) {
    json a = json::array();
    for (const NlIter& it : its) a.push_back({{key, it.obj}, {"backed_up", it.backed_up}});
    return a;
}

}  // namespace

int cmd_nl(const std::vector<std::string>& a) {
    const std::string spec = opt(a, "--expr");
    std::string data = opt(a, "--data");
    const std::string vce = opt(a, "--vce", "iid");
    if (data_missing(data) || spec.empty()) {
        std::fprintf(stderr,
                     "usage: cobra nl --data f.hbin --expr \"(Y = <expr in {params} and vars>)\" "
                     "[--vce iid] [--weight W --wtype aw|pw] [--level P] [--keepif EXPR] "
                     "[--out f.json]\n"
                     "  nonlinear least squares (Stata `nl`). Parameters are written {name=init}\n"
                     "  and are solved for; every other identifier is a variable in the data.\n"
                     "  e.g. --expr \"(y = {b0=2}+{b1=.5}*x1+{b2=-.2}*x2)\"\n"
                     "  --weight W --wtype aw|pw  Stata [aweight=]/[pweight=]: normalized to sum to\n"
                     "               the row count; pw takes the robust sandwich (vce robust).\n");
        return 2;
    }
    // --vce: only the model-based (iid) covariance is pinned against Stata. A
    // robust NLS sandwich differs from Stata's only in the small-sample factor,
    // which is exactly the part no oracle here fixes — so it is rejected rather
    // than emitted as a number nothing verified.
    // BUG-1283: Stata's [aweight=] / [pweight=]. A pweight implies the robust sandwich,
    // whose n/(n-k) factor the pweight oracle pins (StataBE `nl (y = {b0} + {b1}*x1 +
    // {b2}*x2) [pw=w]`: _se[/b1] .071775268345285762); fw/iw would make N the sum of
    // the weights and are not served here.
    const std::string weight_var = opt(a, "--weight"), wtype = opt(a, "--wtype");
    bool pweight = false;
    if (!weight_var.empty() || !wtype.empty()) {
        if (weight_var.empty()) throw EngineError("bad_input", "nl: --wtype requires --weight");
        if (wtype.empty() || wtype == "aw" || wtype == "aweight") pweight = false;
        else if (wtype == "pw" || wtype == "pweight") pweight = true;
        else
            throw EngineError("unsupported",
                "nl: --wtype must be aw or pw (got '" + wtype + "'); fweight and iweight are "
                "not implemented for nl");
    }
    if (vce != "iid" && !(pweight && vce == "robust"))
        throw EngineError("unsupported",
            "nl: --vce " + vce + " is not implemented; only the model-based (iid) covariance "
            "sigma2*(J'J)^-1, and a pweight's robust sandwich, are pinned against Stata in this "
            "build");
    const bool level_set = has(a, "--level");
    const double level = std::stod(opt(a, "--level", "95"));
    if (level_set && (level < 10.0 || level > 99.99)) {
        std::fprintf(stderr, "nl: --level must be in [10, 99.99] (got %s).\n", opt(a, "--level").c_str());
        return 2;
    }

    const std::string body = strip_outer_parens(spec);
    const size_t eq = top_level_assign(body);
    if (eq == std::string::npos)
        throw EngineError("bad_input",
            "nl: --expr must have the form \"(depvar = <expression>)\" — no top-level '=' found "
            "in \"" + spec + "\"");
    const std::string dep = trimx(body.substr(0, eq));
    const std::string rhs = body.substr(eq + 1);
    if (!is_var_token(dep))
        throw EngineError("bad_input",
            "nl: the left side of '=' must be a single variable name (got \"" + dep + "\")");

    const expr::Program prog = compile_body(rhs, "nl");
    if (prog.params().empty())
        throw EngineError("bad_input",
            "nl: the model has no {parameters} to estimate — write them as {name} or {name=init}");

    data = maybe_keepif(data, opt(a, "--keepif"));
    const Dataset& ds = acquire_data(data);
    const double* depcol = require_num(ds, "nl", dep);
    std::vector<std::string> listwise_extra{dep};
    if (!weight_var.empty()) listwise_extra.push_back(weight_var);
    ExprData d = gather_expr_vars(ds, "nl", prog.variables(), listwise_extra, /*listwise_vars=*/true);
    // A zero weight marks the row out (Stata's mark), a negative one is an error; the
    // rest are normalized to sum to the row count (nl.ado).
    std::vector<double> wn;
    double sum_w = 0;
    if (!weight_var.empty()) {
        const double* wcol = require_num(ds, "nl", weight_var);
        std::vector<int64_t> keep2;
        std::vector<double> vals2;
        for (int64_t i = 0; i < d.n; ++i) {
            const double wi = wcol[d.keep[static_cast<size_t>(i)]];
            if (wi < 0)
                throw EngineError("bad_input", "nl: weight '" + weight_var + "' has a negative value");
            if (wi == 0) continue;
            keep2.push_back(d.keep[static_cast<size_t>(i)]);
            for (int j = 0; j < d.nv; ++j)
                vals2.push_back(d.vals[static_cast<size_t>(i) * static_cast<size_t>(d.nv) +
                                       static_cast<size_t>(j)]);
            wn.push_back(wi);
            sum_w += wi;
        }
        d.keep = std::move(keep2);
        if (d.nv > 0) d.vals = std::move(vals2);
        d.n = static_cast<int64_t>(d.keep.size());
        for (double& v : wn) v *= static_cast<double>(d.n) / sum_w;
    }
    // BUG-1269: nl words its empty and too-small samples itself (StataBE 19.5,
    // `nl (y = {b0} + {b1}*x) if x > 20` / `if x == 1`), r(2000) / r(2001).
    const int64_t n_params = static_cast<int64_t>(prog.params().size());
    if (d.n == 0)
        throw EngineError("no_obs", "estimation sample indicator variable eliminates all observations",
                          "r(2000)");
    if (d.n < n_params)
        throw EngineError("no_obs", "cannot have fewer observations (" + std::to_string(d.n) +
                                        ") than parameters (" + std::to_string(n_params) + ")",
                          "r(2001)");
    std::vector<double> Y = gather_rows(depcol, d.keep);

    const int nv = d.nv;
    const double* vals = d.vals.data();
    const NlResidFn resid = [&](const std::vector<double>& b, int64_t i) {
        expr::VarSource vs;
        vs.values = vals + static_cast<size_t>(i) * static_cast<size_t>(nv);
        return Y[static_cast<size_t>(i)] - prog.eval(b.data(), vs, nullptr);
    };

    std::vector<std::string> pnames;
    for (const expr::Param& p : prog.params()) pnames.push_back(p.name);
    const NlResult r = nl_fit(resid, Y.data(), d.n, initial_values(prog), pnames,
                              wn.empty() ? nullptr : wn.data(), pweight);
    if (!r.ok) throw EngineError("no_convergence", r.err.empty() ? "nl: the fit failed" : r.err);

    const double tc = t_critical(level == 95.0 ? 0.05 : 1.0 - level / 100.0, r.df_r);
    json coefs = json::array();
    for (int j = 0; j < r.k; ++j) {
        const double b = r.beta[static_cast<size_t>(j)], se = r.se[static_cast<size_t>(j)];
        const double t = (se > 0) ? b / se : 0.0;
        coefs.push_back({{"name", "/" + r.names[static_cast<size_t>(j)]},
                         {"b", b}, {"se", se}, {"t", t},
                         {"p", (se > 0 && r.df_r > 0) ? t_twosided_pvalue(t, r.df_r) : 1.0},
                         {"ci_lo", b - tc * se}, {"ci_hi", b + tc * se}});
    }

    json out = {{"estimator", "nl"}, {"depvar", dep}, {"expr", spec},
                {"n_obs", d.n}, {"n_dropped_missing", ds.n_rows() - d.n},
                {"n_params", r.k}, {"vce", pweight ? std::string("robust") : vce},
                {"coefs", coefs},
                {"rss", r.rss}, {"mss", r.mss}, {"tss", r.tss},
                {"df_r", r.df_r}, {"df_m", r.df_m}, {"df_t", r.df_t},
                {"ms_r", r.rss / r.df_r}, {"ms_m", r.df_m > 0 ? r.mss / r.df_m : 0.0},
                {"ms_t", r.df_t > 0 ? r.tss / r.df_t : 0.0},
                {"r2", r.r2}, {"r2_adj", r.r2_adj}, {"rmse", r.rmse}, {"res_dev", r.res_dev},
                {"has_constant", r.cons_param >= 0},
                {"iterations", iteration_log(r.iters, "rss")},
                {"converged", r.converged}};
    if (level_set) out["level"] = level;
    // Weighted only, so an unweighted reply is unchanged: e(wtype), e(sum_w).
    if (!weight_var.empty())
        out["weight"] = {{"var", weight_var}, {"type", pweight ? "pw" : "aw"}, {"sum_w", sum_w}};
    std::vector<std::string> notes;
    if (r.cons_param >= 0)
        notes.push_back("Parameter " + r.names[static_cast<size_t>(r.cons_param)] +
                        " is used as a constant term during estimation.");
    else
        // No parameter enters additively, so there is no centring to justify: the
        // ANOVA block above is built on the UNCENTRED total sum of squares. Stata
        // prints its own wording here; this build says what it actually did rather
        // than guessing at a string no oracle in this wave pinned.
        notes.push_back("No parameter is used as a constant term during estimation; R-squared is "
                        "computed against the uncentred total sum of squares.");
    add_warnings(out, notes);
    emit(out, opt(a, "--out"));
    return 0;
}

int cmd_gmm(const std::vector<std::string>& a) {
    const std::string spec = opt(a, "--expr");
    std::string data = opt(a, "--data");
    const std::string instr = opt(a, "--instruments");
    const std::string vce = opt(a, "--vce", "robust");
    const bool onestep = has(a, "--onestep");
    if (data_missing(data) || spec.empty() || instr.empty()) {
        std::fprintf(stderr,
                     "usage: cobra gmm --data f.hbin --expr \"(<residual in {params} and vars>)\" "
                     "--instruments Z1,Z2 --onestep [--vce robust] [--level P] [--keepif EXPR] "
                     "[--out f.json]\n"
                     "  single-equation GMM (Stata `gmm`). The expression IS the residual.\n"
                     "  A constant is appended to the instrument list (Stata's `_cons`).\n"
                     "  e.g. --expr \"(y-{b0=2}-{b1=.5}*x1-{b2=-.2}*x2)\" --instruments x1,x2,z --onestep\n");
        return 2;
    }
    // Two-step GMM would re-weight by S(b1)^-1 and change the covariance with it;
    // nothing in this wave's oracle pins either. Reject rather than emit numbers
    // no measurement stands behind.
    if (!onestep)
        throw EngineError("unsupported",
            "gmm: only --onestep is implemented (the one-step estimator with the unadjusted "
            "initial weight matrix); two-step GMM is not in this build");
    if (vce != "robust")
        throw EngineError("unsupported",
            "gmm: --vce " + vce + " is not implemented; the robust sandwich that Stata `gmm` "
            "reports by default is the only covariance pinned in this build");
    const bool level_set = has(a, "--level");
    const double level = std::stod(opt(a, "--level", "95"));
    if (level_set && (level < 10.0 || level > 99.99)) {
        std::fprintf(stderr, "gmm: --level must be in [10, 99.99] (got %s).\n", opt(a, "--level").c_str());
        return 2;
    }

    const std::string body = strip_outer_parens(spec);
    if (top_level_assign(body) != std::string::npos)
        throw EngineError("bad_input",
            "gmm: --expr is the RESIDUAL, not an equation — it must not contain a top-level '=' "
            "(got \"" + spec + "\")");
    const expr::Program prog = compile_body(body, "gmm");
    if (prog.params().empty())
        throw EngineError("bad_input",
            "gmm: the model has no {parameters} to estimate — write them as {name} or {name=init}");

    std::vector<std::string> zs = split_vars(instr);
    if (zs.empty()) throw EngineError("bad_input", "gmm: --instruments named no variables");

    data = maybe_keepif(data, opt(a, "--keepif"));
    const Dataset& ds = acquire_data(data);

    // STAGE 1 — markout on the INSTRUMENTS only. This is what makes Stata's
    // "missing values returned ... at initial values" note possible at all: a row
    // whose expression variable is missing but whose instruments are complete
    // survives this stage and is caught by stage 2.
    const ExprData d = gather_expr_vars(ds, "gmm", prog.variables(), zs, /*listwise_vars=*/false);
    require_sample(d.n);  // BUG-1269: r(2000) "no observations" / r(2001) for one row
    const int64_t n_markout = d.n;

    const int nv = d.nv;
    const double* vals0 = d.vals.data();
    const std::vector<double> b0 = initial_values(prog);

    // STAGE 2 — evaluate the residual at the INITIAL values and drop the rows
    // where it comes back missing, counting them for the note.
    std::vector<int64_t> sel;
    sel.reserve(static_cast<size_t>(d.n));
    for (int64_t i = 0; i < d.n; ++i) {
        expr::VarSource vs;
        vs.values = vals0 + static_cast<size_t>(i) * static_cast<size_t>(nv);
        if (std::isfinite(prog.eval(b0.data(), vs, nullptr))) sel.push_back(i);
    }
    const int64_t n_init_missing = d.n - static_cast<int64_t>(sel.size());
    const int64_t n = static_cast<int64_t>(sel.size());
    if (n == 0) {
        std::fprintf(stderr, "gmm: the residual is missing on every row at the initial values.\n");
        return 1;
    }

    // Compact the expression variables onto the surviving rows, and build
    // Z = [instruments..., 1] in the user's order with the constant LAST, which
    // is the order Stata's "Instruments for equation 1: x1 x2 z _cons" reports.
    std::vector<double> vals(static_cast<size_t>(n) * static_cast<size_t>(nv > 0 ? nv : 1), 0.0);
    for (int64_t i = 0; i < n; ++i)
        for (int j = 0; j < nv; ++j)
            vals[static_cast<size_t>(i) * static_cast<size_t>(nv) + static_cast<size_t>(j)] =
                d.vals[static_cast<size_t>(sel[static_cast<size_t>(i)]) * static_cast<size_t>(nv) +
                       static_cast<size_t>(j)];

    const int L = static_cast<int>(zs.size()) + 1;
    std::vector<double> Z(static_cast<size_t>(n) * static_cast<size_t>(L), 1.0);
    for (size_t c = 0; c < zs.size(); ++c) {
        const double* src = require_num(ds, "gmm", zs[c]);
        double* dst = Z.data() + c * static_cast<size_t>(n);
        for (int64_t i = 0; i < n; ++i)
            dst[i] = src[d.keep[static_cast<size_t>(sel[static_cast<size_t>(i)])]];
    }

    const double* valsp = vals.data();
    const NlResidFn resid = [&](const std::vector<double>& b, int64_t i) {
        expr::VarSource vs;
        vs.values = valsp + static_cast<size_t>(i) * static_cast<size_t>(nv);
        return prog.eval(b.data(), vs, nullptr);
    };

    std::vector<std::string> pnames;
    for (const expr::Param& p : prog.params()) pnames.push_back(p.name);
    const GmmResult r = gmm_fit(resid, Z.data(), n, L, b0, pnames);
    if (!r.ok) throw EngineError("no_convergence", r.err.empty() ? "gmm: the fit failed" : r.err);

    // Asymptotic z inference — Stata's `gmm` header reports z, not t.
    const double zc = z_critical(level == 95.0 ? 0.05 : 1.0 - level / 100.0);
    json coefs = json::array();
    for (int j = 0; j < r.k; ++j) {
        const double b = r.beta[static_cast<size_t>(j)], se = r.se[static_cast<size_t>(j)];
        const double z = (se > 0) ? b / se : 0.0;
        coefs.push_back({{"name", "/" + r.names[static_cast<size_t>(j)]},
                         {"b", b}, {"se", se}, {"z", z},
                         // erfc(|z|/sqrt2) IS the two-sided normal tail. Written
                         // that way rather than as 2*(1 - Phi(|z|)): the latter
                         // cancels to a hard 0 past |z| ~ 8, which would report
                         // p = 0 for this fixture's z = -10.79 (true 3.8e-27).
                         {"p", (se > 0) ? std::erfc(std::fabs(z) * 0.7071067811865476) : 1.0},
                         {"ci_lo", b - zc * se}, {"ci_hi", b + zc * se}});
    }
    std::vector<std::string> znames = zs;
    znames.push_back("_cons");

    json out = {{"estimator", "gmm"}, {"expr", spec},
                {"n_obs", n}, {"n_dropped_missing", ds.n_rows() - n},
                {"n_instrument_markout", n_markout},  // rows surviving stage 1 (see the header)
                {"n_params", r.k}, {"n_moments", r.n_moments},
                {"instruments", znames}, {"onestep", true},
                {"winitial", "unadjusted"}, {"vce", vce},
                {"coefs", coefs}, {"criterion", r.q},
                {"iterations", iteration_log(r.iters, "criterion")},
                {"converged", r.converged}};
    if (level_set) out["level"] = level;
    std::vector<std::string> notes;
    if (n_init_missing > 0)
        notes.push_back(std::to_string(static_cast<long long>(n_init_missing)) +
                        " missing values returned for equation 1 at initial values.");
    add_warnings(out, notes);
    emit(out, opt(a, "--out"));
    return 0;
}

}  // namespace cli
}  // namespace cecon
