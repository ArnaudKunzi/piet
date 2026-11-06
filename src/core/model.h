// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// cecon — assemble a model's data (design matrix + fixed-effect/cluster factors)
// from a Dataset, with a single listwise deletion across every involved column.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "core/matrix.h"
#include "io/columnar.h"

namespace cecon {

// A categorical variable encoded as dense codes 0..n_levels-1 over the kept rows.
struct Factor {
    std::string name;
    int n_levels = 0;
    std::vector<int32_t> codes;   // length n
    std::vector<double> count;    // per-level row count (== weight sum for unit weights)
};

// Stata estimation weights ([aw=]/[fw=]/[iw=]/[pw=]). The kind decides two things
// and nothing else: whether the weights are normalized to sum to the row count
// (analytic/probability) or used raw (frequency/importance), and therefore what
// Stata reports as e(N) — the row count for aw/pw, Σw for fw/iw. None leaves every
// unweighted code path byte-identical to before weights existed.
enum class WeightKind { None, Analytic, Frequency, Importance, Probability };

struct ModelData {
    DesignMatrix dm;               // y and X (no intercept when FE are present)
    std::vector<Factor> fe;        // fixed effects to absorb
    std::vector<Factor> cluster;   // clustering dimensions
    std::vector<double> offset;    // optional GLM offset (empty if none), length n
    std::vector<double> weights;   // raw (UN-normalized) weights, length n; empty = unweighted
    WeightKind weight_kind = WeightKind::None;
    bool has_intercept = false;
    std::vector<int64_t> keep;     // original-dataset row indices kept (length n)
    int64_t n_singletons_dropped = 0;  // rows removed by singleton-FE dropping
    // BUG-1067: which member of a collinear set feols_fit omits. false (the
    // default) keeps a norm-pivoted basis, Stata `regress`'s choice (z = 2*x:
    // `o.x z`). true keeps the EARLIER term in design order and omits the later
    // one (collinear_keep_mask), which implements reghdfe, areg and xtreg,fe per
    // StataBE probes (`x o.z` for all three). Set by the caller after build_model
    // (cmd_feols and the post-estimation refits' `--collinear-forward`).
    bool collinear_forward = false;
    // Under collinear_forward, the design columns scanned LAST: the cells of a
    // standalone `a#b` that hold a factor's base level. StataBE (AER 007
    // TA17: `treatment##i.husb ... treatment#i.edu_nohs_BL`) keeps
    // 0b.treatment#1.edu_nohs_BL and 1.treatment#1.edu_nohs_BL and omits
    // 1o.treatment#0b.edu_nohs_BL, which duplicates the 1.treatment main effect;
    // a plain forward scan kept that cell and omitted 1.treatment#1.edu_nohs_BL,
    // reporting 1.treatment .0942 for Stata's .1851. Filled by build_model.
    std::vector<std::string> late_cells;
    // BUG-112(8): non-fatal model-spec notes raised while resolving the design
    // (today: `ib#.` naming a base level absent from the data). Empty on every
    // clean run — callers surface it as the response's optional "warnings" array,
    // so a clean response stays byte-identical to before.
    std::vector<std::string> warnings;
};

// Build y ~ X with fixed effects `fe` and clustering `cluster`. Rows with a
// missing value in ANY involved column (y, X, fe, cluster) are dropped once.
// An intercept is added only when there are no fixed effects (FE absorb it).
// FE/cluster columns may be .hbin factor columns or numeric (f64) columns; in
// both cases codes are made dense over the kept rows.
// drop_singleton: iteratively drop singleton FE groups (count==1), like reghdfe's
// default, before returning. No effect when there are no fixed effects.
struct TsMeta;  // fwd (defined below)
// row_mask (E1): when non-null, an ADDITIONAL per-row admission mask, indexed like
// `ds` (length ds.n_rows()), applied alongside the usual missing-value checks — a
// row with row_mask[i]==0 is excluded exactly as if it were missing. This lets a
// caller apply a Stata `if` sample restriction AFTER `ts` operators have already
// been materialized against the FULL (unrestricted) `ds` — required so L./F./D./S.
// see the whole tsset series (Stata semantics: `if` selects estimation rows; it does
// not shrink the series the TS operators are computed over). nullptr (the default)
// reproduces the exact prior behavior — every non-TS / non-`if` caller is unaffected.
// weight_name (P3): a Stata weight column. It joins the SAME single listwise-deletion
// pass as y/X/fe/cluster (a missing weight drops the row, exactly like a missing
// regressor), and rows whose weight is zero are excluded from the estimation sample
// the way Stata excludes them. A negative weight — or, for `fw`, a non-integer one —
// is a hard error, never a silently coerced value. Empty (the default) leaves
// md.weights empty and every existing caller's design byte-identical.
ModelData build_model(const Dataset& ds, const std::string& yname,
                      const std::vector<std::string>& xnames,
                      const std::vector<std::string>& fenames,
                      const std::vector<std::string>& clusternames,
                      const std::string& offset_name = "",
                      bool drop_singleton = true,
                      const TsMeta* ts = nullptr,
                      const std::vector<char>* row_mask = nullptr,
                      const std::string& weight_name = "",
                      WeightKind weight_kind = WeightKind::None);

// Drop singleton FE groups (FE levels with exactly one obs), iterated to a fixed
// point. Rebuilds md in place; returns the number of rows dropped.
int64_t drop_singletons(ModelData& md);

// Intersection of two factors: a new factor whose levels are the distinct
// (a,b) code pairs. Used for the multiway-cluster intersection term.
Factor intersect_factors(const Factor& a, const Factor& b);

// ppmlhdfe "fe" separation: iteratively drop rows whose FE level has sum(y)==0
// (all-zero cells), to a fixed point (dropping rows can zero another dim's cell).
// Rebuilds md.dm.X/y/offset, md.fe/md.cluster codes+counts, and md.keep in place.
// Returns the number of rows dropped. No-op if there are no fixed effects.
int64_t drop_zero_outcome_fe_cells(ModelData& md);

// Data for an IV/2SLS model. Regressors are [exog, endog]; the instrument set is
// W = [exog, excluded instruments]. All column-major, listwise-deleted jointly.
struct IvModelData {
    int64_t n = 0;
    int64_t n_dropped = 0;
    int64_t n_singletons_dropped = 0;  // rows removed by singleton-FE dropping
    int kexog = 0, kendog = 0, kinst = 0;  // kinst = excluded instruments
    std::vector<double> Xexog, Xendog, Zinst, y;
    // inst_names parallels the Zinst columns (post factor expansion / TS rewrite),
    // exactly as exog_names parallels Xexog. Needed so an instrument the FE
    // absorption annihilates can be REPORTED by name when it is omitted (§35).
    std::vector<std::string> exog_names, endog_names, inst_names;
    std::vector<Factor> fe, cluster;
    bool has_intercept = false;
    // `ivregress ..., noconstant`: no intercept column AND no absorbed effect
    // standing in for it (the small-sample dof base is then 0, not 1).
    bool no_constant = false;
    std::vector<int64_t> keep;  // original-dataset row indices kept (length n)
    std::vector<std::string> warnings;  // BUG-112(8), see ModelData::warnings
    // [aw=]/[pw=] observation weights (raw, length n; empty = unweighted). Set by the
    // caller after build_iv_model, aligned to `keep`. iv2sls_fit applies them (see
    // there); only Analytic and Probability are accepted.
    std::vector<double> weights;
    WeightKind weight_kind = WeightKind::None;
    // ivreg2's two-way cluster variance: left unrepaired (compute_vcov no_psd_fix).
    bool multiway_no_psd_fix = false;
    // xtivreg2's ivreg2 call (identified from its ado): within-transformed data fitted
    // with dofminus(N_g) — the absorbed parameters are a LARGE-sample adjustment
    // (sigma2 = rss/(N-dofminus), robust meat * N/(N-dofminus), cluster untouched)
    // and are never counted in the cluster small-sample K. See iv2sls_fit.
    bool dofminus_fe = false;
    // Set by iv2sls_fit when weights meet absorbed effects: the (weighted) within
    // transform has already been applied, so the later demeaning passes are skipped.
    bool fe_absorbed = false;

    const double* exog(int j) const { return Xexog.data() + static_cast<int64_t>(j) * n; }
    const double* endog(int j) const { return Xendog.data() + static_cast<int64_t>(j) * n; }
    const double* inst(int j) const { return Zinst.data() + static_cast<int64_t>(j) * n; }
};

// drop_singleton: iteratively drop singleton FE groups (count==1), like reghdfe/
// ivreghdfe's default (Correia 2015), before returning. No effect without FE.
// row_mask: see build_model's row_mask doc (E1) — same contract, applied in the
// IV listwise-deletion pass.
IvModelData build_iv_model(const Dataset& ds, const std::string& yname,
                           const std::vector<std::string>& exog,
                           const std::vector<std::string>& endog,
                           const std::vector<std::string>& instruments,
                           const std::vector<std::string>& fenames,
                           const std::vector<std::string>& clusternames,
                           const TsMeta* ts = nullptr,
                           bool drop_singleton = true,
                           const std::vector<char>* row_mask = nullptr,
                           bool no_intercept = false);

// Drop singleton FE groups from an IV model (FE levels with exactly one obs),
// iterated to a fixed point. Rebuilds md in place; returns rows dropped.
int64_t drop_singletons_iv(IvModelData& md);

// Time-series metadata for the L./F./D./S. operators in a varlist. The .hbin format
// does NOT persist xtset/tsset, so callers pass the panel/time vars per invocation
// (Stata `tsset time` or `xtset panel time`). Empty timevar disables TS operators
// (a token with an L./F./D./S. prefix then errors rather than silently passing).
struct TsMeta {
    std::string timevar;    // time variable name (required for TS ops)
    std::string panelvar;   // panel id (empty = single time series)
    double delta = 1.0;     // time step (Stata tsset delta; default 1)
};

// Split a token into an optional leading factor prefix and a trailing operand
// carrying a time-series operator chain (L./F./D./S., composed/numbered), e.g.
// "i.L.gdp" -> {fac_prefix="i.", ts_token="L.gdp", base_var="gdp", has_ts=true};
// a token with no TS operator returns has_ts=false. Pure string parsing (no
// Dataset needed) — the SAME detector ts_prepass uses internally, exposed so a
// keepif-aware caller can tell whether a varlist needs TS-before-`if` handling
// (E1: TS operators must materialize against the FULL tsset series before an
// `if` sample filter runs — see maybe_keepif/plan_keepif in cecon_cli.cpp)
// BEFORE opening any data.
struct TsSplit { std::string fac_prefix; std::string ts_token; std::string base_var; bool has_ts = false; };
TsSplit split_ts(const std::string& tok);

// Stata varlist expander — the single pre-estimator preprocessing layer every
// estimator funnels its varlist through. Resolves, over ALL rows of `ds`:
//   - factor-variable operators  i. / c. / b#. / ib#. / ibn. / i(numlist). / o.
//     and interactions  #  /  ##   (delegates to the engine's factor expander)
//   - time-series operators  L. L2. F. D. S.  (and composed/numbered forms),
//     materialized via ts_operators using `ts` (time/panel/delta) metadata
// Returns materialized (column-name, full-length f64 column) pairs in varlist order.
// Plain numeric columns pass through unchanged. A bare name with no operator and no
// `bare_factor` context stays continuous (a single pass-through column).
// `bare_factor` mirrors parse: false = plain main-effect varlist (bare col = continuous);
// callers that want bare names treated as factors set it true (rare).
std::vector<std::pair<std::string, std::vector<double>>>
expand_varlist(const Dataset& ds, const std::vector<std::string>& names,
               const TsMeta& ts = {}, bool bare_factor = false);

}  // namespace cecon
