// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Post-estimation utilities: Hausman, predict, margins.
//
// All work on pre-computed (β, V) tuples plus the data — no command coupling.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "core/distributions.h"
#include "estimators/glm.h"  // GlmLinkFn, glm_mean/glm_dmean/glm_d2mean (margins_grid)

namespace cecon {

struct HausmanResult {
    int dof = 0;             // # parameters used in the test
    double chi2 = 0;
    double pvalue = 0;       // approximate (chi² right-tail)
    bool ok = true;
    std::string error;
};

// Hausman test: H = (b_FE - b_RE)' (V_FE - V_RE)^{-1} (b_FE - b_RE).
//   b_FE, b_RE: coefficient vectors (length k)
//   V_FE, V_RE: covariance matrices (k*k, column-major)
// If V_FE - V_RE is not PD (can happen in finite samples), we PSD-project
// the difference before inverting (Stata "Hausman test inconsistent" workaround).
HausmanResult hausman_test(const double* b_FE, const double* V_FE,
                           const double* b_RE, const double* V_RE,
                           int k);

// predict_xb: xb_i = X_i β + offset_i. X is column-major n*k. Returns nullptr-
// safe; writes into out (length n).
void predict_xb(const double* X, int64_t n, int k,
                const double* beta, const double* offset,
                double* out);

// Average marginal effect for Poisson (or any log-link, exp-mean model):
//   AME_c = (1/n) Σ_i β_c · exp(X_i β + off_i) = β_c · ymean
// with delta-method SE:
//   var(AME_c) = J V J'   where J_d = β_c · (1/n) Σ_i X_i,d · exp(X_i β + off_i)
//                                     + δ_{cd} · (1/n) Σ_i exp(X_i β + off_i)
// V is k*k.
struct MarginsResult {
    std::vector<double> dydx;
    std::vector<double> se;
    bool ok = true;
};

MarginsResult margins_poisson(const double* X, int64_t n, int k,
                              const double* beta, const double* V,
                              const double* offset = nullptr);

// Logit marginal effects (binary):
//   p_i  = 1/(1 + e^{-X_i β})
//   AME_c = (1/n) Σ_i β_c · p_i (1 - p_i)
MarginsResult margins_logit(const double* X, int64_t n, int k,
                            const double* beta, const double* V);

// Probit marginal effects (binary): AME_c = (1/n) Σ_i β_c · φ(X_i β),
// φ = standard normal pdf; delta-method SE. V is k*k.
MarginsResult margins_probit(const double* X, int64_t n, int k,
                             const double* beta, const double* V);

// --- P5 (BUG-158/159): weighted, interaction-aware, factor-discrete-change AME --
//
// margins_logit/probit/poisson above are correct ONLY when every design column
// is a single continuous main effect (no `#`/`##` interactions, no `i.`/`level.var`
// factor columns) and the fit is unweighted. Three independent failures otherwise:
//   1. No weights — under [pweight=] the AME must be the pweight-weighted mean over
//      the estimation sample, not a plain arithmetic mean.
//   2. No interaction chain rule — with e.g. `c.x1##i.treated`, d(xb)/d(x1) is
//      beta_x1 + beta_{x1#1.treated}*treated_i, not just beta_x1.
//   3. Factor levels need the Stata "discrete change from base" contrast
//      (mean_i[Pr(y|level)] - mean_i[Pr(y|base)]), not a pdf-scaled derivative.
//
// margins_ame implements all three, generalized over the logit/probit/poisson
// link, driven ENTIRELY off the design's column NAMES (BUG-101 convention:
// `level.var` for a factor level, `a#b` for an interaction of parts `a`,`b`,
// each itself either `var` (continuous) or `level.var`). It never re-derives the
// design — it only reads which named columns exist and what they multiply.
// Cloglog: Pr = 1 - exp(-exp(eta)), Stata's cloglog (clog_p.ado's d/d xb forms).
enum class GlmLink { Logit, Probit, Poisson, Cloglog };

// One requested/derived AME row: a continuous variable's chain-rule slope, or one
// non-base level of a requested factor variable's discrete change. `name` is the
// BUG-101 column-naming convention ("x1", "1.treated") so callers can key off it
// the same way coeftables already do.
struct AmeRow {
    std::string name;
    double dydx = 0;
    double se = 0;
    // d(dydx)/d(beta), length k: se = sqrt(J' V J). Two rows' covariance is
    // J_a' V J_b, which is how `margins, dydx() at()` builds r(V) across points.
    std::vector<double> J;
};

struct AmeResult {
    std::vector<AmeRow> rows;
    bool ok = true;
    std::string error;
};

// X: column-major n*k design (as fit — including any intercept column, named
// "(intercept)", which is silently skipped for both requesting and reporting).
// names: length k, BUG-101 column names aligned with X/beta/V.
// beta/V: the fitted coefficients and the model's OWN (possibly sandwich, possibly
// pweight-reweighted) covariance — whatever VCE the caller wants the delta-method
// SEs built from.
// weights: length n raw estimation weights, or nullptr for an unweighted (mean)
// AME.
// request_vars: base variable names to report (Stata `dydx()` — e.g. {"x1",
// "treated"}, NOT design-column names); empty means "every column with no `#`",
// i.e. auto-report every main-effect/level column, matching the shape of the
// simple per-column functions above when there are no interactions.
// offset: optional GLM offset (Poisson), length n, or nullptr.
AmeResult margins_ame(GlmLink link, const double* X, int64_t n, int k,
                      const std::vector<std::string>& names,
                      const double* beta, const double* V,
                      const double* weights,
                      const std::vector<std::string>& request_vars,
                      const double* offset = nullptr);

// --- Distribution tail probabilities -----------------------------------------
// betai / t_twosided_pvalue / t_critical / z_critical moved to
// core/distributions.h at the P6 wave so PIET's resampling driver can share the
// SAME implementation instead of growing a second copy of the same mathematics.
// Included here so every existing consumer of this header still sees them.
// Upper-tail p-value of an F(df1, df2) statistic: P(F > f).
double f_pvalue(double f, double df1, double df2);
// Upper-tail p-value of a chi-square(dof) statistic at x.
double chi2_pvalue(double x, double dof);
// Lower-tail χ² CDF P(X²_dof ≤ x), accurate in the left tail.
double chi2_cdf(double x, double dof);
// Regularized lower incomplete gamma P(a, x).
double gammp(double a, double x);

// --- General linear (Wald) test of H0: R β = q -------------------------------
//   chi2 = (Rβ−q)' (R V R')⁻¹ (Rβ−q) ~ χ²(J);  F = chi2/J.
// R is J*k row-major, q is length J, V is k*k column-major. If R V R' is not PD
// it is PSD-projected before inversion (like hausman_test); ok=false if it still
// cannot be inverted. Distribution-free — caller forms the p-values.
struct WaldResult {
    int J = 0;
    double chi2 = 0;
    double F = 0;
    bool ok = true;
    std::string error;
};
WaldResult wald_test(const double* beta, const double* V, int k,
                     const double* R, const double* q, int J);

// The restrictions Stata's `test` drops ("Constraint k dropped") before it forms
// the statistic, as 0-based row numbers of R in ascending order: the rows that
// are not linearly independent of the others under V. R V R' is reduced pivot by
// pivot, each time on the open row with the largest remaining variance (the
// earliest on a tie); a row whose remaining variance falls below 1e-9 of its own,
// or below 1e-19, is dropped. A zero row (only omitted coefficients) is always
// dropped, and with V = 0 every row is. Same R/V layout as wald_test.
std::vector<int> wald_dropped_rows(const double* V, int k, const double* R, int J);

// --- margins over a grid of at() points, from the fit's own e(b)/e(V) --------
//
// Oracle: tests/eval/golden/aer_w2/w2v_margins_corpus.log (StataNow 19.5). Header-
// inline, as core/distributions.h is, so PIET's in-process margins and the
// engine's unit tests run this one copy without linking posthest.cpp.
//
// Stata's margins fixes COVARIATES, not design columns, and rebuilds every column
// from them. Each column is the product of its `#` atoms; an atom is a
// continuous variable (`x`, `c.x`, `o.x`) or a factor-level indicator (`2.g`,
// `0b.tt`, `1o.bc`, `2010bn.year`). After `regress y c.x##c.x c1`, at(x=5) puts
// 25 in c.x#c.x (X1_at5 9.737407233828263) and atmeans puts mean(x)^2 there,
// not mean(x^2) (X1_atmeans 9.922553971152869). An atom no point fixes keeps
// its observed value, or under atmeans its weighted e(sample) mean stored at
// float precision (F2_at_atmeans 9.48656514801535, asobserved F2_at
// 9.48656523242321); a factor level's mean is its indicator's mean. Every at()
// value and every atmeans mean is evaluated at float precision whatever the
// variable's storage type: F7_dydx_at at(d=-0.4) on a float d is
// .4683828811482316 with float(-0.4), 1.1e-8 away with the double -0.4; after
// `regress y x c1 bvar ivar dvar` (byte, int, double), `margins, at(x=(1 2))
// atmeans` is 7.024710751140063 / 7.703783727515455 only with float(mean) for
// all four (w2v_f7c_jacobian.log:775-794, FU-668), and at(ivar=(2.1 3.3)) on
// the int ivar has r(V)[1,1] .0257184016424877 only at float(2.1), 2.6e-8 off
// at the double 2.1 (w2v_margins_siblings FU668_at_ivar_v; FU668_at_dvar_v and
// FU668_at_bvar_v the same for double and byte). r(at) keeps the double value.
// A factor indicator's mean is a float too (w2b_estim margins_atmeans_post:
// float(1/6)).
//
// A margin is the weighted mean over e(sample) of the response g(xb) (the
// predict default: xb with _cons after regress/areg/xtreg,fe/reghdfe, Pr(y)
// after logit, the mean after glm). dydx() of a continuous variable is
// g'(xb) d(xb)/dx, d(xb)/dx summed over every column the variable enters
// (X1_dydx_at .6731182154326146 = b_x + 2*5*b_x#x). dydx() of a factor is the
// discrete change from its base level, g(xb | level) - g(xb | base); the base
// column is 0 with a 0 variance (F5_dydx). V = G e(V) G', G the gradient of
// each margin in b.

struct MarginsAtom {
    std::string var;
    bool factor = false;
    double level = 0;   // factor atoms only
    bool base = false;  // the `b` flag of a factor level
};

// The atoms of one e(b) column name (no equation prefix): Stata's spelling
// (`c.z#c.x`, `0b.tt`, `_cons`) or the engine's (`z#x`, `1.tt`, `(intercept)`,
// base levels left out). The constant has none. *ok = false for an operator
// this does not rebuild (L., D., F., S., ...).
inline std::vector<MarginsAtom> margins_parse_column(const std::string& name, bool* ok) {
    std::vector<MarginsAtom> atoms;
    *ok = true;
    if (name == "_cons" || name == "(intercept)") return atoms;
    size_t p = 0;
    while (true) {
        const size_t q = name.find('#', p);
        const std::string seg = name.substr(p, q == std::string::npos ? std::string::npos : q - p);
        MarginsAtom a;
        const size_t dot = seg.find('.');
        const std::string op = dot == std::string::npos ? std::string() : seg.substr(0, dot);
        a.var = dot == std::string::npos ? seg : seg.substr(dot + 1);
        size_t digits = 0;
        while (digits < op.size() && op[digits] >= '0' && op[digits] <= '9') ++digits;
        if (digits > 0) {
            for (size_t c = digits; c < op.size(); ++c) {
                if (op[c] != 'b' && op[c] != 'n' && op[c] != 'o') *ok = false;
                if (op[c] == 'b') a.base = true;
            }
            a.factor = true;
            a.level = std::stod(op.substr(0, digits));
        } else if (!(op.empty() || op == "c" || op == "o" || op == "co" || op == "oc")) {
            *ok = false;
        }
        if (a.var.empty()) *ok = false;
        atoms.push_back(std::move(a));
        if (q == std::string::npos) break;
        p = q + 1;
    }
    return atoms;
}

// One at() point: the variables it fixes, each to a value (a factor to a level).
struct MarginsGridPoint {
    std::vector<std::pair<std::string, double>> fix;
};

// The points of one at() option, `varname=#` and `varname=(numlist)` settings
// ([R] margins, Syntax of at()). Every combination is a point, the rightmost
// variable varying fastest (rule 3: at(x1=(5 10) x2=(0 1)) is (5,0) (5,1)
// (10,0) (10,1)); a repeated varname adds values (rule 2: at(x1=5 x1=10)).
// `fpassed==0` is served exactly as `fpassed=0` (w2v_margins_corpus
// F3b_at3_eqeq == F3b_at3). A numlist holds numbers, `a(d)b`, `a[d]b` and
// `a/b`; anything else is rc 121 (F7_dydx_at's `d=(min(.2)max)` is
// r(121)). `(stat) varlist` and `generate()` are not parsed: rc 198, named.
// a(d)b is a + i*d (the oracle's grids, 0(25)100, 40(5)60 and -0.4(.2).4,
// land on float variables, where accumulating and multiplying agree).
struct MarginsAtParse {
    int rc = 0;
    std::string error;
    std::vector<std::string> vars;  // in order of first mention
    std::vector<MarginsGridPoint> points;
};

inline MarginsAtParse margins_parse_at(const std::string& at) {
    MarginsAtParse R;
    auto fail = [&](int rc, const std::string& msg) {
        R.rc = rc;
        R.error = msg;
        R.vars.clear();
        R.points.clear();
        return R;
    };
    auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
    auto number = [](const std::string& t, double* v) {
        if (t.empty()) return false;
        char* end = nullptr;
        *v = std::strtod(t.c_str(), &end);
        return end == t.c_str() + t.size() && std::isfinite(*v);
    };
    std::vector<std::vector<double>> values;
    auto expand = [&](const std::string& tok, std::vector<double>* out) {
        double a = 0, d = 0, b = 0;
        if (number(tok, &a)) {
            out->push_back(a);
            return true;
        }
        const size_t slash = tok.find('/');
        if (slash != std::string::npos) {
            if (!number(tok.substr(0, slash), &a) || !number(tok.substr(slash + 1), &b)) return false;
            d = b >= a ? 1 : -1;
        } else {
            const size_t open = tok.find_first_of("([");
            const char close = open == std::string::npos ? 0 : (tok[open] == '(' ? ')' : ']');
            const size_t shut = open == std::string::npos ? std::string::npos : tok.find(close, open);
            if (shut == std::string::npos || !number(tok.substr(0, open), &a) ||
                !number(tok.substr(open + 1, shut - open - 1), &d) || !number(tok.substr(shut + 1), &b) ||
                d == 0 || (b - a) / d < 0)
                return false;
        }
        const double steps = std::floor((b - a) / d * (1 + 1e-12) + 1e-9);
        if (!(steps >= 0) || steps > 1e6) return false;
        for (int64_t i = 0; i <= static_cast<int64_t>(steps); ++i) out->push_back(a + static_cast<double>(i) * d);
        return true;
    };
    const size_t n = at.size();
    size_t i = 0;
    while (true) {
        while (i < n && isSpace(at[i])) ++i;
        if (i >= n) break;
        if (at[i] == '(') return fail(198, "margins: at((stat) varlist) is not supported");
        size_t j = i;
        while (j < n && !isSpace(at[j]) && at[j] != '=' && at[j] != '(') ++j;
        const std::string var = at.substr(i, j - i);
        while (j < n && isSpace(at[j])) ++j;
        if (j >= n || at[j] != '=') return fail(198, "margins: at() " + var + " needs =# or =(numlist)");
        ++j;
        if (j < n && at[j] == '=') ++j;
        while (j < n && isSpace(at[j])) ++j;
        std::vector<double> vals;
        if (j < n && at[j] == '(') {
            int depth = 0;
            size_t k = j;
            for (; k < n; ++k) {
                if (at[k] == '(') ++depth;
                if (at[k] == ')' && --depth == 0) break;
            }
            if (k >= n) return fail(198, "margins: at() " + var + "=( has no closing parenthesis");
            const std::string list = at.substr(j + 1, k - j - 1);
            size_t p = 0;
            while (p < list.size()) {
                while (p < list.size() && (isSpace(list[p]) || list[p] == ',')) ++p;
                if (p >= list.size()) break;
                size_t q = p;
                while (q < list.size() && !isSpace(list[q]) && list[q] != ',') ++q;
                if (!expand(list.substr(p, q - p), &vals)) return fail(121, "invalid numlist");
                p = q;
            }
            if (vals.empty()) return fail(121, "invalid numlist");
            i = k + 1;
        } else {
            size_t k = j;
            while (k < n && !isSpace(at[k])) ++k;
            const std::string tok = at.substr(j, k - j);
            if (tok.compare(0, 9, "generate(") == 0 || tok.compare(0, 4, "gen(") == 0)
                return fail(198, "margins: at(" + var + "=generate()) is not supported");
            double v = 0;
            if (!number(tok, &v)) return fail(198, "margins: at() " + var + "=" + tok + " is not a number");
            vals.push_back(v);
            i = k;
        }
        if (var.empty()) return fail(198, "margins: at() needs a variable name before =");
        size_t slot = 0;
        while (slot < R.vars.size() && R.vars[slot] != var) ++slot;
        if (slot == R.vars.size()) {
            R.vars.push_back(var);
            values.emplace_back();
        }
        values[slot].insert(values[slot].end(), vals.begin(), vals.end());
    }
    if (R.vars.empty()) return fail(198, "margins: at() is empty");
    std::vector<size_t> idx(R.vars.size(), 0);
    while (true) {
        MarginsGridPoint pt;
        for (size_t v = 0; v < R.vars.size(); ++v) pt.fix.emplace_back(R.vars[v], values[v][idx[v]]);
        R.points.push_back(std::move(pt));
        size_t pos = R.vars.size();
        while (pos > 0) {
            --pos;
            if (++idx[pos] < values[pos].size()) break;
            idx[pos] = 0;
            if (pos == 0) return R;
        }
    }
}

struct MarginsGridInput {
    std::vector<std::string> names;  // e(b) column names (no equation prefix), base and omitted included
    std::vector<double> beta;        // k
    std::vector<double> V;           // k*k, symmetric
    GlmLinkFn link = GlmLinkFn::Identity;
    int64_t n = 0;                   // e(sample) rows
    std::map<std::string, std::vector<double>> data;  // every variable an atom names: n values
    std::vector<double> weights;     // n fit weights, or empty
    std::vector<double> offset;      // n, or empty
    std::vector<MarginsGridPoint> points;  // at least one; an empty point fixes nothing
    bool atmeans = false;
    std::vector<std::string> dydx;   // empty: predictive margins
};

// Column order is Stata's: dydx variable in the order the variables first enter
// e(b) (not the dydx() order: `dydx(treatment x)` after `regress y
// c.x##i.treatment c1` is `x 0b.treatment 1.treatment`, w2v_margins_siblings
// C2_dydx_tr_x_cols; `dydx(c1 x)` after `logit remote x c1 i.g` is `x c1`,
// C7_dydx_c1x_cols), then (factor) level ascending, then point.
struct MarginsGridColumn {
    std::string dydx;     // "" for a predictive margin
    bool factor = false;  // a factor dydx variable
    double level = 0;
    bool base = false;    // the base level: 0 with 0 variance
    size_t point = 0;
};

struct MarginsGridResult {
    bool ok = true;
    std::string error;
    std::string notFound;  // a dydx() or at() variable no column names
    std::vector<MarginsGridColumn> cols;
    std::vector<double> b;  // m
    std::vector<double> V;  // m*m row-major
};

inline MarginsGridResult margins_grid(const MarginsGridInput& in) {
    MarginsGridResult R;
    const size_t k = in.names.size();
    const int64_t n = in.n;
    if (in.beta.size() != k || in.V.size() != k * k || in.points.empty() || n <= 0 ||
        (!in.weights.empty() && in.weights.size() != static_cast<size_t>(n)) ||
        (!in.offset.empty() && in.offset.size() != static_cast<size_t>(n))) {
        R.ok = false;
        R.error = "margins: the estimates and the estimation sample do not match";
        return R;
    }
    std::vector<std::vector<MarginsAtom>> atoms(k);
    for (size_t j = 0; j < k; ++j) {
        bool ok = true;
        atoms[j] = margins_parse_column(in.names[j], &ok);
        if (!ok) {
            R.ok = false;
            R.error = "margins: cannot rebuild the term " + in.names[j] + " at an at() point";
            return R;
        }
        for (const auto& a : atoms[j]) {
            auto it = in.data.find(a.var);
            if (it == in.data.end() || it->second.size() != static_cast<size_t>(n)) {
                R.ok = false;
                R.error = "margins: no e(sample) values for " + a.var;
                return R;
            }
        }
    }
    auto wOf = [&](int64_t i) { return in.weights.empty() ? 1.0 : in.weights[static_cast<size_t>(i)]; };
    auto stored = [](double v) { return static_cast<double>(static_cast<float>(v)); };
    double wsum = 0;
    for (int64_t i = 0; i < n; ++i) wsum += wOf(i);

    // atmeans: each variable's weighted mean (a factor: each level indicator's), as a
    // float for every storage type (FU-668).
    std::map<std::string, double> meanC;
    std::map<std::string, std::map<double, double>> meanF;
    if (in.atmeans) {
        for (size_t j = 0; j < k; ++j) {
            for (const auto& a : atoms[j]) {
                const std::vector<double>& v = in.data.at(a.var);
                if (a.factor) {
                    if (meanF[a.var].count(a.level)) continue;
                    double s = 0;
                    for (int64_t i = 0; i < n; ++i) s += wOf(i) * (v[static_cast<size_t>(i)] == a.level ? 1.0 : 0.0);
                    meanF[a.var][a.level] = static_cast<double>(static_cast<float>(s / wsum));
                } else {
                    if (meanC.count(a.var)) continue;
                    double s = 0;
                    for (int64_t i = 0; i < n; ++i) s += wOf(i) * v[static_cast<size_t>(i)];
                    meanC[a.var] = static_cast<double>(static_cast<float>(s / wsum));
                }
            }
        }
    }
    auto mentions = [&](const std::string& var, bool* factor, bool* continuous) {
        *factor = *continuous = false;
        for (const auto& col : atoms)
            for (const auto& a : col)
                if (a.var == var) (a.factor ? *factor : *continuous) = true;
    };
    for (const auto& pt : in.points) {
        for (const auto& f : pt.fix) {
            bool fa = false, co = false;
            mentions(f.first, &fa, &co);
            if (!fa && !co) {
                R.ok = false;
                R.notFound = f.first;
                R.error = "variable " + f.first + " not found in list of covariates";
                return R;
            }
        }
    }

    // The value of atom `a` at row i, with `fix` the point's settings plus a
    // factor dydx() level (`over`, applied last).
    struct Fix {
        const std::string* var;
        double value;
    };
    auto atomValue = [&](const MarginsAtom& a, int64_t i, const std::vector<Fix>& fix) {
        for (auto f = fix.rbegin(); f != fix.rend(); ++f) {
            if (*f->var == a.var) return a.factor ? (f->value == a.level ? 1.0 : 0.0) : f->value;
        }
        if (in.atmeans) return a.factor ? meanF[a.var][a.level] : meanC[a.var];
        const double v = in.data.at(a.var)[static_cast<size_t>(i)];
        return a.factor ? (v == a.level ? 1.0 : 0.0) : v;
    };
    std::vector<double> x(k), dx(k), x2(k);
    auto buildRow = [&](int64_t i, const std::vector<Fix>& fix, std::vector<double>& row) {
        for (size_t j = 0; j < k; ++j) {
            double v = 1.0;
            for (const auto& a : atoms[j]) v *= atomValue(a, i, fix);
            row[j] = v;
        }
    };
    auto etaOf = [&](const std::vector<double>& row, int64_t i) {
        double e = in.offset.empty() ? 0.0 : in.offset[static_cast<size_t>(i)];
        for (size_t j = 0; j < k; ++j) e += row[j] * in.beta[j];
        return e;
    };

    std::vector<std::vector<double>> G;
    auto fixesOf = [&](const MarginsGridPoint& pt) {
        std::vector<Fix> fix;
        for (const auto& f : pt.fix) fix.push_back({&f.first, stored(f.second)});
        return fix;
    };
    auto predictive = [&](const MarginsGridPoint& pt, std::vector<double>& g) {
        const std::vector<Fix> fix = fixesOf(pt);
        double s = 0;
        for (int64_t i = 0; i < n; ++i) {
            buildRow(i, fix, x);
            const double e = etaOf(x, i), w = wOf(i), d = glm_dmean(in.link, e);
            s += w * glm_mean(in.link, e);
            for (size_t j = 0; j < k; ++j) g[j] += w * d * x[j];
        }
        return s;
    };
    auto slope = [&](const MarginsGridPoint& pt, const std::string& var, std::vector<double>& g) {
        const std::vector<Fix> fix = fixesOf(pt);
        double s = 0;
        for (int64_t i = 0; i < n; ++i) {
            buildRow(i, fix, x);
            double D = 0;
            for (size_t j = 0; j < k; ++j) {
                double dj = 0;
                for (size_t a = 0; a < atoms[j].size(); ++a) {
                    if (atoms[j][a].factor || atoms[j][a].var != var) continue;
                    double rest = 1.0;
                    for (size_t b = 0; b < atoms[j].size(); ++b)
                        if (b != a) rest *= atomValue(atoms[j][b], i, fix);
                    dj += rest;
                }
                dx[j] = dj;
                D += dj * in.beta[j];
            }
            const double e = etaOf(x, i), w = wOf(i);
            const double d = glm_dmean(in.link, e), d2 = glm_d2mean(in.link, e);
            s += w * d * D;
            for (size_t j = 0; j < k; ++j) g[j] += w * (d2 * x[j] * D + d * dx[j]);
        }
        return s;
    };
    auto change = [&](const MarginsGridPoint& pt, const std::string& var, double level, double base,
                      std::vector<double>& g) {
        std::vector<Fix> hi = fixesOf(pt), lo = hi;
        hi.push_back({&var, level});
        lo.push_back({&var, base});
        double s = 0;
        for (int64_t i = 0; i < n; ++i) {
            buildRow(i, hi, x);
            buildRow(i, lo, x2);
            const double eh = etaOf(x, i), el = etaOf(x2, i), w = wOf(i);
            const double dh = glm_dmean(in.link, eh), dl = glm_dmean(in.link, el);
            s += w * (glm_mean(in.link, eh) - glm_mean(in.link, el));
            for (size_t j = 0; j < k; ++j) g[j] += w * (dh * x[j] - dl * x2[j]);
        }
        return s;
    };
    auto push = [&](MarginsGridColumn c, double s, std::vector<double> g) {
        if (!c.base) {
            s /= wsum;
            for (auto& v : g) v /= wsum;
        }
        R.cols.push_back(c);
        R.b.push_back(s);
        G.push_back(std::move(g));
    };

    if (in.dydx.empty()) {
        for (size_t p = 0; p < in.points.size(); ++p) {
            std::vector<double> g(k, 0.0);
            const double s = predictive(in.points[p], g);
            MarginsGridColumn c;
            c.point = p;
            push(c, s, std::move(g));
        }
    }
    std::vector<std::string> dydxOrder = in.dydx;
    auto firstColumn = [&](const std::string& var) {
        for (size_t j = 0; j < k; ++j)
            for (const auto& a : atoms[j])
                if (a.var == var) return j;
        return k;  // not in the model: reported below
    };
    std::stable_sort(dydxOrder.begin(), dydxOrder.end(),
                     [&](const std::string& a, const std::string& b) { return firstColumn(a) < firstColumn(b); });
    for (const auto& var : dydxOrder) {
        bool fa = false, co = false;
        mentions(var, &fa, &co);
        if (!fa && !co) {
            R.ok = false;
            R.notFound = var;
            R.error = "variable " + var + " not found in list of covariates";
            return R;
        }
        if (fa && co) {
            R.ok = false;
            R.error = "margins: " + var + " enters the model both as a factor and as a continuous variable";
            return R;
        }
        if (co) {
            for (size_t p = 0; p < in.points.size(); ++p) {
                std::vector<double> g(k, 0.0);
                const double s = slope(in.points[p], var, g);
                MarginsGridColumn c;
                c.dydx = var;
                c.point = p;
                push(c, s, std::move(g));
            }
            continue;
        }
        // The levels are the e(sample) values of the variable. The base is the
        // level e(b) flags `b`, else (the engine's names leave base columns out)
        // the one level with no main-effect column.
        std::map<double, bool> levels;  // level -> base flag
        for (double v : in.data.at(var)) levels[v] = false;
        std::map<double, bool> hasMain;
        for (const auto& col : atoms)
            for (const auto& a : col)
                if (a.factor && a.var == var) {
                    levels[a.level] = levels[a.level] || a.base;
                    if (col.size() == 1) hasMain[a.level] = true;
                }
        int nBase = 0;
        for (const auto& [lv, isBase] : levels) nBase += isBase ? 1 : 0;
        if (nBase == 0) {
            for (auto& [lv, isBase] : levels)
                if (!hasMain.count(lv)) { isBase = true; ++nBase; }
        }
        double base = 0;
        for (const auto& [lv, isBase] : levels)
            if (isBase) base = lv;
        if (nBase != 1) {
            R.ok = false;
            R.error = "margins: the base level of " + var + " is not identified in e(b)";
            return R;
        }
        for (const auto& [lv, isBase] : levels) {
            for (size_t p = 0; p < in.points.size(); ++p) {
                std::vector<double> g(k, 0.0);
                MarginsGridColumn c;
                c.dydx = var;
                c.factor = true;
                c.level = lv;
                c.base = isBase;
                c.point = p;
                const double s = isBase ? 0.0 : change(in.points[p], var, lv, base, g);
                push(c, s, std::move(g));
            }
        }
    }

    const size_t m = R.b.size();
    std::vector<double> GV(m * k, 0.0);
    for (size_t a = 0; a < m; ++a)
        for (size_t j = 0; j < k; ++j) {
            double s = 0;
            for (size_t l = 0; l < k; ++l) s += G[a][l] * in.V[l * k + j];
            GV[a * k + j] = s;
        }
    R.V.assign(m * m, 0.0);
    for (size_t a = 0; a < m; ++a)
        for (size_t b = a; b < m; ++b) {
            double s = 0;
            for (size_t j = 0; j < k; ++j) s += GV[a * k + j] * G[b][j];
            R.V[a * m + b] = s;
            R.V[b * m + a] = s;
        }
    return R;
}

// Stata's colfullnames of r(b) for a margins_grid result. `over` names the
// marginlist variable whose levels `overLevels` the points are, or is empty for
// at() points (`#._at`); `single` is a form with one point and no marginlist.
// w2v_margins_corpus: `_cons` (X1_at5), `1bn._at 2._at` (F2_at), `2010bn.year`
// (F6), `x:1bn._at` (F1_dydx_at), `0b.treatment 1.treatment` (F5_dydx),
// `0b.tt:1bno._at 1.tt:1bn._at` (F7_dydx_at), `0b.tt:0bno.dcat 1.tt:0bn.dcat`
// (F7nb_dcat_dydx): the first point carries `bn`, and in the base level's
// equation every point is also `o` (omitted).
inline std::vector<std::string> margins_grid_colnames(const MarginsGridResult& r, bool single,
                                                      const std::string& over,
                                                      const std::vector<double>& overLevels) {
    auto num = [](double v) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.17g", v);
        return std::string(buf);
    };
    std::vector<std::string> out;
    for (const auto& c : r.cols) {
        std::string head;
        if (!c.dydx.empty()) head = c.factor ? num(c.level) + (c.base ? "b." : ".") + c.dydx : c.dydx;
        if (single) {
            out.push_back(head.empty() ? std::string("_cons") : head);
            continue;
        }
        std::string flags = c.point == 0 ? "bn" : "";
        if (c.factor && c.base) flags += "o";
        std::string pt;
        if (over.empty()) {
            pt = std::to_string(c.point + 1) + flags + "._at";
        } else {
            pt = (c.point < overLevels.size() ? num(overLevels[c.point]) : std::string("?")) + flags + "." + over;
        }
        out.push_back(head.empty() ? pt : head + ":" + pt);
    }
    return out;
}

}  // namespace cecon
