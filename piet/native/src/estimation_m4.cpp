// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// M4a estimation + diagnostic surface — C++ port of
// piet/backend/piet/router/translate.py's build_estimation_argv/
// build_csdid_argv/build_corr_argv/build_ttest_argv routing plus
// dispatcher.py's _cobra_to_estimation / _csdid_to_estimation / _run_predict
// / the test_cmd/lincom_cmd/nlcom_cmd/margins_cmd post-estimation handlers
// and the hettest/ovtest/vif "estat" diagnostic rewrite. See
// piet/native/include/piet/handlers.h for the per-handler oracle-source
// mapping (ROADMAP M4a item 2).
//
// Scope: xtreg (fe/re), logit, probit, csdid (+ estat simple/event/
// calendar/group/pretrend), tabstat, correlate/pwcorr/ttest, predict,
// test, lincom, nlcom, margins, estat hettest/ovtest/vif, ivregress/
// ivreghdfe/reg3/boottest (BETA D1), tobit/heckman/qreg/mediate (BETA D2,
// BS-069/070/071/072).
#include "piet/handlers.h"

#include <cmath>
#include <cstdio>
#include <ctime>
#include <limits>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_set>

#include "piet/cobra_runner.h"
#include "piet/cobra_session.h"
#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"  // kStataMissingSentinel
#include "piet/inrange_filter.h"
#include "piet/settings.h"
#include "piet/strutil.h"
#include "io/win_compat.h"  // cecon::native_unlink (BUG-050 wide unlink)
#include "piet/translate.h"
#include "piet/scratch_seq.h"
#include "lapack_dsyevr.h"  // BUG-1482: the clustered V's rank (clusteredVRankM4)

// ---------------------------------------------------------------------------
// BUG-233 / FIX:OPTNET-ABBREV-SWEEP — this file's option vocabularies.
//
// Every set below used to be a function-local `static const` sitting next to its
// reject net, which meant nothing upstream of the handler could see it — and the
// handler's own `k == "fullname"` comparisons therefore ran against RAW keys
// while eval::rejectUnknownOptions had already ACCEPTED an abbreviation of them.
// That is BUG-231's mechanism, and it was live in nets across six files.
//
// Registering here (static init, before any dispatch) lets dispatcher.cpp's single
// resolution point canonicalize the keys BEFORE routing. The sets did not get
// copied: each net now binds a reference back to its registered entry, so there is
// still exactly one place each spelling is written.
namespace {
const piet::eval::OptVocab& kVocabTtest = piet::eval::registerOptVocab(
    {"ttest"}, {{"level", "by", "unequal", "welch", "paired", "reverse", "unpaired"},
     {}});
const piet::eval::OptVocab& kVocabCsdid = piet::eval::registerOptVocab(
    {"csdid"}, {{"ivar", "time", "gvar", "method", "comparison", "pscoretrim", "window", "cluster", "long2"},
     {}});
const piet::eval::OptVocab& kVocabReg3 = piet::eval::registerOptVocab(
    {"reg3"}, {{"sure", "3sls"},
     {}});
const piet::eval::OptVocab& kVocabBoottest = piet::eval::registerOptVocab(
    {"boottest"}, {{"reps", "seed", "weighttype", "null",
                                                          "cluster"},
     {}});
const piet::eval::OptVocab& kVocabTobit = piet::eval::registerOptVocab(
    {"tobit"}, {{"ll", "ul", "level", "vce", "robust"},
     {}});
const piet::eval::OptVocab& kVocabHeckman = piet::eval::registerOptVocab(
    {"heckman"}, {{"select", "twostep", "level"},
     {}});
const piet::eval::OptVocab& kVocabQreg = piet::eval::registerOptVocab(
    {"qreg"}, {{"quantile", "level", "vce"},
     {}});
const piet::eval::OptVocab& kVocabMediate = piet::eval::registerOptVocab(
    {"mediate"}, {{"reps", "seed", "level"},
     {}});
// BUG-263: mediate's treatment sub-block `(tvar, ...)` is a SECOND
// eval::parseOpts vector built inside runMediate itself (BUG-262's class), so
// the dispatcher's resolution point can never see its keys — the handler
// canonicalizes and rejects locally against this entry. Registered under its
// own token, NOT under "mediate": registerOptVocab UNIONs per token and
// kVocabMediate above binds the "mediate" slot by reference, so adding
// `continuous` there would make the TOP-LEVEL net accept-and-silently-ignore
// a trailing `, continuous(...)` — BUG-231's exact shape. The sub-block
// grammar is closed at exactly one option ([CAUSAL] mediate, Syntax:
// `(tvar [, continuous(numlist)])`), so the unimplemented set is empty by
// measurement, not omission; the "real Stata shape PIET doesn't implement"
// honesty lives in the handler's own numlist-arity message (a >2-value
// continuous() numlist is real Stata, and gets the two-level-only reject).
const piet::eval::OptVocab& kVocabMediateTreat = piet::eval::registerOptVocab(
    {"mediate:tvar"}, {{"continuous"},
     {}});
const piet::eval::OptVocab& kVocabDrdid = piet::eval::registerOptVocab(
    {"drdid"}, {{"ivar", "time",     "treatment", "dripw",
                                                          "reg",  "ipw",      "std_ipw",   "trad"},
     {}});
// Item 4 (User ruling 2026-09-09): `post` moves to `known` -- see
// estimation_m4c.cpp's copy (the one actually READ by runMargins) for the
// full rationale. Three byte-identical copies must move together.
const piet::eval::OptVocab& kVocabMargins = piet::eval::registerOptVocab(
    {"margins"}, {{"at", "dydx", "post", "atmeans"},
     {}});
const piet::eval::OptVocab& kVocabEstatDurbinalt = piet::eval::registerOptVocab(
    {"estat"}, {{"small", "lags"},
     {}});
const piet::eval::OptVocab& kVocabEstatEvent = piet::eval::registerOptVocab(
    {"estat"}, {{"window", "post"},
     {}});
const piet::eval::OptVocab& kVocabNewey = piet::eval::registerOptVocab(
    {"newey"}, {{"lag", "force"},
     {}});
const piet::eval::OptVocab& kVocabPrais = piet::eval::registerOptVocab(
    {"prais"}, {{"corc", "twostep", "rhotype"},
     {}});
const piet::eval::OptVocab& kVocabWntestq = piet::eval::registerOptVocab(
    {"wntestq"}, {{"lags"},
     {}});
const piet::eval::OptVocab& kVocabSdtest = piet::eval::registerOptVocab(
    {"sdtest"}, {{"by", "level"},
     {}});
const piet::eval::OptVocab& kVocabHausman = piet::eval::registerOptVocab(
    {"hausman"}, {{},
     {"constant", "sigmamore", "sigmaless", "force", "alleqs",
      "skipeqs",  "equations", "df",        "tconsistent", "tefficient"}});
const piet::eval::OptVocab& kVocabChow = piet::eval::registerOptVocab(
    {"chow"}, {{"by"},
     {}});
const piet::eval::OptVocab& kVocabMkspline = piet::eval::registerOptVocab(
    {"mkspline"}, {{"cubic", "nknots", "knots",
                                                          "displayknots"},
     {}});
const piet::eval::OptVocab& kVocabIvRegress = piet::eval::registerOptVocab(
    {"ivregress", "ivreg", "ivreghdfe", "reghdfe"}, {{"vce", "vcov", "robust", "cluster",
      "gmm",
      "absorb", "keepsingletons", "noconstant", "nocons", "level"},
     {}});
const piet::eval::OptVocab& kVocabPoisson = piet::eval::registerOptVocab(
    {"poisson", "ppmlhdfe", "fepois"}, {{"vce", "vcov", "robust", "cluster", "absorb",
      "offset", "exposure", "noconstant", "nocons", "level"},
     {}});
const piet::eval::OptVocab& kVocabNbreg = piet::eval::registerOptVocab(
    {"nbreg"}, {{"vce", "vcov", "robust", "cluster", "noconstant",
      "nocons", "offset", "exposure", "dispersion", "level"},
     {}});
// The two conditionally-built nets: the base literal lives here (one copy), and
// the registered vocabulary is base + the command-conditional key, so the
// dispatcher can canonicalize `offs`/`dri` while the handler's own net keeps
// deciding whether the canonical spelling is allowed for the command at hand.
const std::unordered_set<std::string> kXtPoissonLogitBase = {
    "fe", "re", "pa", "vce", "vcov", "robust", "cluster", "noconstant", "nocons", "level"};
const piet::eval::OptVocab& kVocabXtPoissonLogit = piet::eval::registerOptVocab(
    {"xtpoisson", "xtlogit"},
    {[] { auto s = kXtPoissonLogitBase; s.insert("offset"); return s; }(), {}});
const std::unordered_set<std::string> kDfullerPperronBase = {
    "lags", "noconstant", "nocons", "trend"};
const piet::eval::OptVocab& kVocabDfullerPperron = piet::eval::registerOptVocab(
    {"dfuller", "pperron"},
    {[] { auto s = kDfullerPperronBase; s.insert("drift"); return s; }(), {}});
// w2b-estim-vce-options: probit's `robust` abbreviates to ONE character --
// tests/eval/golden/aer_w2/w2b_estim.log:706 `probit d x1 x2, r nocons` is rc 0
// with the Robust std. err. column. logit gets `r` from the measured-minimums
// table (opt_minimums_bug264.inc `{"robust", 1}`); probit has no robust row
// there, and the 1-char spelling is below canonicalizeForCommand's prefix
// floor, so it is DECLARED here as an alias (BUG-430's mechanism). `known`
// holds `robust` alone: it is the only key this registration speaks for, and
// every longer prefix of it is accepted by the same 1-char minimum. The
// handler's own net (buildEstimationArgvM4 kEstimationKnownOpts) is unchanged.
const piet::eval::OptVocab& kVocabProbitRobust = piet::eval::registerOptVocab(
    {"probit"}, {{"robust"}, {}, {{"r", "robust"}}});
}  // namespace

#include "estimation_m4_internal.h"

namespace piet::handlers {

using ojson = nlohmann::ordered_json;

// R2: the shared helper block that lived here as an anonymous namespace is now
// `namespace est_detail` (definitions HERE, declarations in
// estimation_m4_internal.h) so the two continuation TUs can link against it.
namespace est_detail {

// A5-04: forward to the shared strutil copy (piet/strutil.h) — kept as thin
// wrappers (not removed) so this file's many lowerM4(...)/trimM4(...) call
// sites didn't need touching. Pure dedup, no behavior change.
std::string lowerM4(std::string s) { return strutil::lower(std::move(s)); }

std::string trimM4(const std::string& s) { return strutil::trim(s); }

ojson engineErrorBlockM4(const std::string& message, const std::string& code) {
  return eval::engineError(message, code);
}

// BUG-569 (review round): the VCE options' value SHAPE, twin of handlers.cpp
// vceOptionShapeError (the reasons are written there). Each VCE loop in this
// file reads `cluster` only when valued and `robust` only as a flag, so a bare
// `cluster`, an empty `cluster()` or a valued `robust(x)` was skipped and the
// model ran iid at rc 0. Measured on the branch binary 2026-09-29:
// `xtreg y x1, fe cluster`, `logit yb x1, cluster`, `probit yb x1, robust(id)`
// and `poisson cnt x1, cluster` each returned rc 0 with e(vce) iid.
//
// ppmlhdfe and ivreghdfe ignore an EMPTY `cluster()` and fit their default VCE
// at rc 0 (w2l_vce_shape_det.log ppmlhdfe_empty_cluster `0|ppmlhdfe|robust|
// Robust|.|180`, :3508 ivreghdfe_empty_cluster `0|ivreghdfe|||.|180`); the
// official ML commands (logit, probit, ivprobit, ivtobit, ...) keep r(198).
static bool emptyClusterIgnoredM4(const std::string& cmd) {
  return cmd == "ppmlhdfe" || cmd == "ivreghdfe";
}

static std::optional<std::string> vceOptionShapeErrorM4(const eval::ParsedCommand& parsed) {
  for (const auto& [k, v] : parsed.opts) {
    if (k == "cluster" && !v.isBool && trimM4(v.strValue).empty() &&
        emptyClusterIgnoredM4(parsed.cmd))
      continue;
    if (k == "cluster" && (v.isBool || trimM4(v.strValue).empty())) {
      return std::string("option `cluster` requires a variable: cluster(varname)");
    }
    if (k == "robust" && !v.isBool) {
      return std::string("option `robust` takes no argument");
    }
  }
  return std::nullopt;
}

// BUG-038(a): forwards to the shared mapping point (piet::cobraErrorMessage,
// cobra_runner.h) — kept as a thin wrapper (not removed) so this file's
// many call sites didn't need touching. Pure dedup, no behavior change here.
std::string cobraErrorStrM4(const CobraError& ce) { return cobraErrorMessage(ce); }

std::string joinCommaM4(const std::vector<std::string>& v) {
  std::string s;
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) s += ",";
    s += v[i];
  }
  return s;
}

// kZ975: moved to estimation_m4_internal.h as inline constexpr (R2 — internal-linkage const cannot cross the TU cut)

double normalPvalue(double z) { return std::erfc(std::abs(z) / std::sqrt(2.0)); }

void fillCoeftableStatsM4(ojson& block);

// BUG-102: a factor-variable coefficient row's engine name is `<level>.<var>`
// (BUG-101, model.cpp:526), or `<level>.<var>#<level>.<var>` for interactions.
// Real Stata substitutes the attached value label for the bare level when
// DISPLAYING such a row (verified live against StataBE 19.5: `regress y
// i.state_num` with `label values state_num statelbl` prints "Arizona"/
// "Idaho" rows, not "2.state_num"/"3.state_num") — but that live table also
// relies on a shared "state_num" group header PIET's flat one-row-per-
// coefficient table has no equivalent for (formatOneTable, console.js), so
// the ".varname" suffix is kept here rather than dropped bare: the level
// portion alone is substituted, segment-by-segment across any "#" interaction
// join, matching the scoping note's "rewrite factor-level row names" framing.
std::string substituteFactorLevelLabelM4(const std::string& name, const SessionState& session) {
  return strutil::substituteFactorLevelLabel(name, session.valueLabels);
}

// Applies substituteFactorLevelLabelM4 to every row["name"] in a coeftable
// array in place. A no-op (session.valueLabels empty, or a coeftable that
// isn't an array) costs one map .empty() check — freeze/pangram-safe for the
// overwhelming majority of runs that never attach a value label.
void applyFactorValueLabelsM4(ojson& coeftable, const SessionState& session) {
  // BUG-397 (2026-09-02, the g12 oracle-first leg's FIX:BUG102-COEFTABLE-NAME-PARITY): the row's
  // `name` is Stata's e(b) colname and is INVARIANT under value labels (`1983.yearpl` stays
  // `1983.yearpl`; `_b[1983.yearpl]` must keep resolving). The label is DISPLAY: it rides on two
  // additive fields — `stub` = the BUG-102 display shape with the level segment substituted
  // (`Odd83.yearpl`), which the frontend prints in place of the name; `label` = the bare label
  // text of the (last) substituted level (`Odd83`), Stata's own stub. Rows without a label get
  // neither field, so every unlabeled fixture is byte-identical.
  if (session.valueLabels.empty() || !coeftable.is_array()) return;
  for (auto& row : coeftable) {
    if (!row.is_object() || !row.contains("name") || !row["name"].is_string()) continue;
    const std::string name = row["name"].get<std::string>();
    std::string sub = substituteFactorLevelLabelM4(name, session);
    if (sub == name) continue;
    row["stub"] = sub;
    // bare label = the text before the first '.' of the LAST '#'-segment that changed
    std::string lastSeg = sub.substr(sub.find_last_of('#') == std::string::npos ? 0 : sub.find_last_of('#') + 1);
    row["label"] = lastSeg.substr(0, lastSeg.find('.'));
  }
}

// -- _cobra_to_estimation (dispatcher.py) — full port including the BUG-006
// margins AME b=null remap that handlers.cpp's cobraToEstimation explicitly
// does NOT include (that copy is the M2/regress-only vertical slice). This
// is the M4a-complete version used by every command in this file.
// `session` is optional (default nullptr): BUG-102 value-label substitution
// only runs when a caller has one to offer (regression-family estimators);
// callers with no natural SessionState in scope (corr/tabstat) are unchanged.
ojson cobraToEstimationM4(const ojson& raw, const std::string& cobraCmd,
                           const SessionState* session) {
  ojson block;
  block["type"] = "estimation";
  std::string estimator = raw.value("estimator", cobraCmd);
  block["estimator"] = estimator;
  block["depvar"] = raw.value("depvar", "");
  // BUG-165 (W:P5): a two-sample ttest has no single `n_obs` on the engine's
  // wire — it reports the two group counts — so the plain default published
  // 0 observations for a test that used thousands. Sum the groups when they
  // are what the engine gave us; every other estimator is unchanged.
  if (!raw.contains("n_obs") && raw.contains("n_1") && raw.contains("n_2") &&
      raw["n_1"].is_number() && raw["n_2"].is_number()) {
    block["n_obs"] = raw["n_1"].get<long long>() + raw["n_2"].get<long long>();
  } else {
    block["n_obs"] = raw.value("n_obs", 0);
  }

  // BUG-112(8), consumption half. cobra emits non-fatal model-spec notes as a
  // top-level `warnings` array (contract A; `add_warnings`, absent when empty)
  // — Stata's `note:` line, e.g. "99b.rep78 identifies no observations in the
  // sample" when an `ib#.` base level is absent. Until now NOTHING on this side
  // read the key: the success path discards cobra's stderr and no consumer
  // looked for a new JSON field, so the engine half shipped INERT. Carrying it
  // onto the block is the whole fix — a warning produced and never shown is a
  // warning that does not exist, and doctrine I says PIET mirrors what cobra
  // says, nothing less. Optional and absent-when-empty on this side too, so a
  // clean estimation stays byte-identical (freeze: 0 fixtures carry the key).
  if (raw.contains("warnings") && raw["warnings"].is_array() && !raw["warnings"].empty()) {
    block["warnings"] = raw["warnings"];
  }

  ojson coeftable;
  bool hasCoeftable = raw.contains("coeftable");
  if (hasCoeftable) coeftable = raw["coeftable"];

  // BUG-006: margins AME — coeftable has b=null; remap from coefs[].dydx.
  bool allBNull = hasCoeftable && coeftable.is_array() && !coeftable.empty();
  if (allBNull) {
    for (const auto& row : coeftable) {
      if (!row.contains("b") || !row["b"].is_null()) {
        allBNull = false;
        break;
      }
    }
  } else {
    allBNull = false;
  }
  if (hasCoeftable && estimator == "margins" && allBNull) {
    ojson coefsRaw = raw.value("coefs", ojson::array());
    std::map<std::string, ojson> coefsByName;
    for (const auto& c : coefsRaw) {
      if (c.contains("name")) coefsByName[c["name"].get<std::string>()] = c;
    }
    ojson remapped = ojson::array();
    for (const auto& row : coeftable) {
      std::string name = row.value("name", std::string());
      auto it = coefsByName.find(name);
      ojson c = (it != coefsByName.end()) ? it->second : ojson::object();
      ojson dydx = c.contains("dydx") ? c["dydx"] : ojson(nullptr);
      ojson se = (it != coefsByName.end() && c.contains("se")) ? c["se"] : row.value("se", ojson(nullptr));
      ojson z = (it != coefsByName.end() && c.contains("t")) ? c["t"] : row.value("t", ojson(nullptr));
      if (dydx.is_null()) dydx = row.value("b", ojson(nullptr));

      ojson r;
      r["name"] = name;
      std::optional<double> bF, seF, zF, pF, ciLo, ciHi;
      try {
        if (!dydx.is_null()) bF = dydx.get<double>();
        if (!se.is_null()) seF = se.get<double>();
        if (!z.is_null()) zF = z.get<double>();
        if (bF.has_value() && seF.has_value()) {
          if (!zF.has_value() && *seF != 0.0) zF = *bF / *seF;
          if (zF.has_value()) pF = normalPvalue(*zF);
          ciLo = *bF - kZ975 * *seF;
          ciHi = *bF + kZ975 * *seF;
        }
      } catch (...) {
        bF.reset();
      }
      r["b"] = bF.has_value() ? ojson(*bF) : dydx;
      r["se"] = seF.has_value() ? ojson(*seF) : se;
      r["t"] = zF.has_value() ? ojson(*zF) : z;
      r["p"] = pF.has_value() ? ojson(*pF) : ojson(nullptr);
      r["ci_lo"] = ciLo.has_value() ? ojson(*ciLo) : ojson(nullptr);
      r["ci_hi"] = ciHi.has_value() ? ojson(*ciHi) : ojson(nullptr);
      remapped.push_back(r);
    }
    coeftable = remapped;
  }

  if (hasCoeftable) block["coeftable"] = coeftable;
  if (hasCoeftable && session) applyFactorValueLabelsM4(block["coeftable"], *session);
  // BS-065: cobra's `reg3 --tidy` payload carries its per-equation results
  // under "equations" (an array of {depvar, coefs, coeftable} — NOT the
  // {..., r2_within, dof_resid} shape --y-batch commands like feols/iv use
  // for "outcomes") rather than under the key "outcomes" itself. Rather than
  // inventing a second, parallel multi-model block shape for the frontend
  // to learn, this reuses the EXACT SAME block["outcomes"] output shape
  // outcomes already produces (falling back to null for the two per-eq
  // stats reg3 doesn't emit, r2_within/dof_resid — an honest engine-data
  // gap, not a frontend bug: verified live, cobra's reg3 --tidy simply
  // never computes them per-equation) so any existing "iterate
  // block.outcomes[]" frontend renderer picks up reg3 for free.
  if (raw.contains("outcomes") || raw.contains("equations")) {
    const ojson& srcArr = raw.contains("outcomes") ? raw["outcomes"] : raw["equations"];
    ojson outcomes = ojson::array();
    for (const auto& o : srcArr) {
      ojson oo;
      // P7: mlogit's equations are keyed by OUTCOME, not by a dependent variable
      // (there is only one depvar, split across K−1 equations). Falling through
      // to the empty default rendered three unlabeled blocks with no way to tell
      // which outcome each belonged to — the numbers were right and the table was
      // unreadable. The base outcome carries no coefficients by construction, so
      // it is labelled as such rather than shown as an empty equation.
      if (o.contains("outcome") && !o.contains("depvar")) {
        oo["depvar"] = o["outcome"].is_string() ? o["outcome"].get<std::string>()
                                                : o["outcome"].dump();
        oo["outcome"] = o["outcome"];
        if (o.value("base", false)) oo["base"] = true;
      } else {
        oo["depvar"] = o.value("depvar", "");
      }
      oo["coeftable"] = o.value("coeftable", ojson::array());
      if (session) applyFactorValueLabelsM4(oo["coeftable"], *session);
      oo["r2"] = o.contains("r2_within") ? o["r2_within"] : o.value("r2", ojson(nullptr));
      oo["r2_within"] = o.contains("r2_within") ? o["r2_within"] : ojson(nullptr);
      oo["dof_resid"] = o.contains("dof_resid") ? o["dof_resid"] : ojson(nullptr);
      outcomes.push_back(oo);
    }
    block["outcomes"] = outcomes;
  }

  static const char* kPassthrough[] = {
      "r2", "r2_within", "sigma2", "deviance", "converged", "iters", "n_clusters",
      "cluster_dof_k", "dof_resid", "first_stage_F", "kp_wald_F", "hansen_j", "hansen_j_p",
      "F", "J", "chi2", "df1", "df2", "p_F", "p_chi2", "restrictions",
      "margin", "at", "margins", "df", "mean_1", "mean_2", "by", "coefs",
      // BUG-165 (W:P5): ttest's per-group counts and dispersions. The engine
      // has always emitted n_1/n_2/sd_1/sd_2 — Stata prints all four in the
      // group rows (`Obs`, `Std. dev.`) — but they were never forwarded, so
      // the block reached the frontend with only the pooled diff. Worse, the
      // engine emits no `n_obs` key for ttest at all, so cobraToEstimationM4's
      // `raw.value("n_obs", 0)` default published **n_obs: 0** on the wire: a
      // test over 3,822 observations reporting zero. Carrying these lets the
      // n_obs fallback below compute the real total.
      "n_1", "n_2",
      "r", "variables", "p", "nobs", "method", "pairwise", "sig",
      // BS-026 (BETA Phase C batch C1): cobra's `tabstat --tidy` payload
      // carries the actual per-var per-stat table in "stats" (the
      // requested stat-name list, e.g. ["mean","sd","n"]) and "total" (a
      // {varname: {statname: value}} object) plus "vars" (the ordered var
      // list) — none of the three were forwarded, so every tabstat block
      // reached the frontend as an empty shell ({estimator:"tabstat",
      // n_obs, by:null}, "by" already being the only tabstat-relevant key
      // in the original list above). Verified against a live cobra
      // `tabstat --tidy` run: these are cobra's OWN field names (not a
      // frontend field-name mismatch to translate) — the frontend has no
      // tabstat table renderer at all yet regardless (out of scope,
      // piet/frontend/** forbidden this batch), so this fixes the DATA
      // layer; the block now genuinely carries every stat that was asked
      // for.
      "stats", "total", "vars",
  };
  for (const char* k : kPassthrough) {
    if (raw.contains(k)) block[k] = raw[k];
  }
  static const char* kOls[] = {"adj_r2", "rmse", "mss", "rss", "df_m", "df_r"};
  for (const char* k : kOls) {
    if (raw.contains(k)) block[k] = raw[k];
  }
  // BUG-156 (wiring half, P2-PREP): poisson/fepois fit statistics. `chi2` is
  // already carried by kPassthrough above; `chi2_df` and `r2_p` (Stata's
  // e(r2_p), the pseudo-R2 spelling poisson uses — distinct from the
  // `r2_pseudo` key logit/probit emit) were dropped on the floor, so even
  // once the engine emits them they could not reach a consumer.
  //
  // The engine does NOT emit these keys yet — that is the parallel half of
  // BUG-156 and is engine COMPUTE work. Until it does, `display e(r2_p)`
  // still answers missing; this is the wiring so it stops being TWO fixes.
  static const char* kGlm[] = {"ll", "ll_0", "r2_pseudo", "df_chi2", "chi2_df", "r2_p"};
  for (const char* k : kGlm) {
    if (raw.contains(k)) block[k] = raw[k];
  }
  // BUG-156 (wiring half, P2-PREP): feols single-FE variance decomposition —
  // Stata's e(sigma_u)/e(sigma_e)/e(rho)/e(corr). Same story as kGlm above:
  // allowlisted now, emitted by the engine in the parallel workstream.
  static const char* kXtreg[] = {"r2_between", "r2_overall", "sigma_u", "sigma_e", "rho",
                                 "corr_u_xb"};
  for (const char* k : kXtreg) {
    if (raw.contains(k)) block[k] = raw[k];
  }
  // BS-066: cobra's `wildboot --tidy` payload has NO "coeftable" at all (a
  // single wild-cluster-bootstrap test result, not a coefficient table) —
  // its own field names (verified live) carry the actual answer.
  static const char* kWildboot[] = {
      "beta", "p_equaltail", "p_symmetric", "null_beta", "seed", "reps", "weights", "t0",
      "webb_fallback",
  };
  for (const char* k : kWildboot) {
    if (raw.contains(k)) block[k] = raw[k];
  }

  if (raw.contains("absorbed") && !raw["absorbed"].empty()) block["absorbed"] = raw["absorbed"];
  if (raw.contains("vcov_type") && raw["vcov_type"].is_string() &&
      !raw["vcov_type"].get<std::string>().empty()) {
    block["vcov_type"] = raw["vcov_type"];
  }
  return block;
}

// -- shared vcov-option parsing (translate.py's _vcov_from_opts) -----------
// Returns the --vcov[/--cluster] argv tail; throws a std::string error
// message (caller wraps into unsupported_option) on a malformed vce().
// struct VcovResult: defined in estimation_m4_internal.h (R2 split)

// BS-067 (BETA D1): `vce(cluster A B)` / legacy `cluster(A B)` naming TWO
// space-separated variables -> `--vcov twoway --cluster A,B` (cobra's
// feols/iv/fepois --vcov surface accepts "twoway" as a 4th mode alongside
// iid/hc1/cluster — live-verified against the D1i reghdfe 2-way oracle:
// `reghdfe income beertax, absorb(state) vce(cluster state year)` ->
// beertax se=1867.524 matches byte-for-byte through this exact
// --vcov twoway --cluster state,year shape). One var -> the pre-existing
// single-cluster path (unchanged). Three+ vars -> `--vcov multiway --cluster
// A,B,C` where `allowMultiway` (the callers whose k-way fit is measured, see
// vcovFromOpts), else a named refusal.
struct ClusterSplitM4 {
  std::vector<std::string> argv;  // {"--vcov", TYPE, "--cluster", VARS}
  bool errored = false;
  std::string errorMsg;
};

ClusterSplitM4 splitClusterVarsM4(const std::string& raw, bool allowMultiway = false) {
  ClusterSplitM4 r;
  std::vector<std::string> toks;
  std::istringstream iss(raw);
  std::string t;
  while (iss >> t) toks.push_back(t);
  if (toks.empty()) {
    r.errored = true;
    r.errorMsg = "cluster variable required";
    return r;
  }
  if (toks.size() == 1) {
    r.argv = {"--vcov", "cluster", "--cluster", toks[0]};
  } else if (toks.size() == 2) {
    r.argv = {"--vcov", "twoway", "--cluster", toks[0] + "," + toks[1]};
  } else if (allowMultiway) {
    std::string joined = toks[0];
    for (size_t i = 1; i < toks.size(); ++i) joined += "," + toks[i];
    r.argv = {"--vcov", "multiway", "--cluster", joined};
  } else {
    r.errored = true;
    r.errorMsg =
        "three or more cluster variables are not supported for this command in this "
        "version of PIET; multiway clustering with three or more cluster variables is "
        "implemented for reghdfe, xtreg, fe and ppmlhdfe only";
  }
  return r;
}

// The M4 commands whose k-way (k >= 3) cluster VCE is measured against StataBE 19.5:
// xtreg, fe under the built-in multiway convention (BUG-1290: vce(cluster cg1 cg2
// cg3) _se[x1] .019191881847891872, e(df_r) 2, e(N_clust) .) and ppmlhdfe under
// reghdfe's (BUG-1189: vce(cluster id id2 id3) e(N_clust) 5, _se[x1]
// .018284790750361792). The engine's two-way and multiway modes are one
// (apps/cmds/common.cpp Vcov::Multiway), over every listed variable.
static bool multiwayClusterMeasuredM4(const std::string& cmd) {
  return cmd == "xtreg" || cmd == "ppmlhdfe";
}

static bool vceTrailingTokenAcceptedM4(const std::string& cmd) {
  return cmd == "reghdfe" || cmd == "ppmlhdfe" || cmd == "fepois" || cmd == "feols" ||
         cmd == "csdid" || cmd == "ivreg2" || cmd == "xtivreg2";
}

// Stata's own refusals, carried with Stata's rc (coarseRc reads the `r(NNN)`
// reason first, so the rc is never the generic 1).
static ojson stataRefusalBlockM4(const std::string& message, int rc) {
  return ojson(eval::errorBlock("syntax_error", message, std::nullopt, std::nullopt, std::nullopt,
                                "r(" + std::to_string(rc) + ")"));
}

// FU-147 / FU-160, twin of handlers.cpp clusterSpecRefusal for the callers of
// buildEstimationArgvM4 (xtreg, logit, probit, ...) and, BUG-1278, of the m4b
// fits that read vce() themselves (ivregress, poisson/ppmlhdfe, nbreg): what
// the fit's own option parser refuses about its cluster variables, before any
// engine call, in that parser's order. Both options are read, whichever one
// vcovFromOpts would pick (BUG-1278: `poisson y x, vce(cluster k) cluster(nosuch)`
// fitted at rc 0). StataBE 19.5, 2026-11-03, over logit/probit/xtreg/poisson/
// nbreg/ivregress/ppmlhdfe with vce(cluster), cluster() and both:
//  - kVceParse (_vce_parse): with `multiwayVceRefused`, vce(cluster a b) is
//    "multiway cluster VCE not allowed" r(198) before anything is looked up;
//    then cluster()'s names, "variable V not found" + "(error in option
//    cluster())" r(111); with `legacyOneVar`, a second cluster() name "cluster():
//    too many variables specified" r(103); then vce(cluster)'s names, r(111)
//    without the tail; then the pairing: vce(robust) with cluster() "only one of
//    options cluster() or vce(robust) is allowed" r(198), vce(cluster a) with
//    cluster(b) "options cluster(clustvar) and vce(cluster clustvar) are in
//    conflict" r(100) (the same variable in both is accepted).
//  - kIvregress: cluster()'s names and r(103) as above, then any vce() with
//    cluster() "cannot specify options robust or cluster() with option vce()"
//    r(198), then vce(cluster)'s names, which its vce() parser turns into
//    "option vce() incorrectly specified" r(198).
//  - kPpmlhdfe: vce() with cluster() "only one of cluster() and vce() can be
//    specified" r(198) first; a name is r(111) with no cluster() tail.
std::optional<ojson> clusterSpecRefusalM4(const eval::ParsedCommand& parsed, SessionState& session,
                                          ClusterOptParserM4 parser, bool multiwayVceRefused,
                                          bool legacyOneVar) {
  std::optional<std::string> vceVal;
  std::optional<std::string> clusterOpt;
  for (const auto& [k, v] : parsed.opts) {
    if ((k == "vce" || k == "vcov") && !vceVal.has_value()) {
      vceVal = v.isBool ? std::string() : v.strValue;
    } else if (k == "cluster" && !v.isBool) {
      clusterOpt = v.strValue;
    }
  }
  auto tokens = [](const std::string& raw) {
    std::vector<std::string> toks;
    std::istringstream iss(raw);
    std::string t;
    while (iss >> t) toks.push_back(t);
    return toks;
  };
  std::string vceType;
  std::string vceRest;
  if (vceVal.has_value() && !vceVal->empty()) vceType = canonicalVceType(*vceVal, &vceRest);
  const bool both = !vceType.empty() && clusterOpt.has_value();
  const std::vector<std::string> legacyToks = tokens(clusterOpt.value_or(""));
  // vce(cluster) with no variable of its own takes cluster()'s (vcovFromOpts).
  const std::vector<std::string> vceToks =
      vceType == "cluster" && !vceRest.empty() ? tokens(vceRest) : std::vector<std::string>();
  if (parser == ClusterOptParserM4::kPpmlhdfe && both)
    return stataRefusalBlockM4("only one of cluster() and vce() can be specified", 198);
  if (multiwayVceRefused && vceToks.size() >= 2)
    return stataRefusalBlockM4("multiway cluster VCE not allowed", 198);
  Datastore* ds = session.datastore();
  if (ds == nullptr) return std::nullopt;
  // BUG-1437: the dispatcher has already put a unique abbreviation's full name
  // in; a name that begins several variables is "NAME ambiguous abbreviation"
  // where one that begins none is "variable NAME not found", with the same rc and
  // tail (StataBE 19.5, 2026-11-03, over the seven fits above).
  auto notFound = [ds](const std::string& n) {
    int hits = 0;
    for (const auto& v : ds->names())
      if (v.size() > n.size() && v.compare(0, n.size(), n) == 0) ++hits;
    return hits > 1 ? n + " ambiguous abbreviation" : "variable " + n + " not found";
  };
  for (const auto& n : legacyToks) {
    if (ds->has(n)) continue;
    return stataRefusalBlockM4(
        notFound(n) +
            (parser == ClusterOptParserM4::kPpmlhdfe ? "" : "\n(error in option cluster())"),
        111);
  }
  if (legacyOneVar && legacyToks.size() >= 2)
    return stataRefusalBlockM4("cluster():  too many variables specified", 103);
  if (parser == ClusterOptParserM4::kIvregress && both)
    return stataRefusalBlockM4("cannot specify options robust or cluster() with option vce()", 198);
  for (const auto& n : vceToks) {
    if (ds->has(n)) continue;
    if (parser == ClusterOptParserM4::kIvregress)
      return stataRefusalBlockM4(notFound(n) + "\noption vce() incorrectly specified", 198);
    return stataRefusalBlockM4(notFound(n), 111);
  }
  if (parser == ClusterOptParserM4::kVceParse && both) {
    if (vceType == "robust")
      return stataRefusalBlockM4("only one of options cluster() or vce(robust) is allowed", 198);
    if (vceType == "cluster" && !vceToks.empty() && vceToks != legacyToks)
      return stataRefusalBlockM4(
          "options cluster(clustvar) and vce(cluster clustvar) are in conflict", 100);
  }
  return std::nullopt;
}

ojson vcovErrorBlockM4(const VcovResult& vr) {
  if (vr.rc != 0) return stataRefusalBlockM4(vr.errorMsg, vr.rc);
  return engineErrorBlockM4(vr.errorMsg, "unsupported_option");
}

VcovResult vcovFromOpts(const eval::ParsedCommand& parsed, bool* sawRobustFlag) {
  VcovResult out;
  // ppmlhdfe's own option parser (ppmlhdfe.ado's parse_vce through
  // _assert_abort) refuses a bare `cluster` with r(3498) "Invalid options:
  // cluster" (w2l_vce_shape_det.log:2819 ppmlhdfe_bare_cluster).
  if (parsed.cmd == "ppmlhdfe") {
    for (const auto& [k, v] : parsed.opts) {
      if (k == "cluster" && v.isBool) {
        out.errored = true;
        out.errorMsg = "Invalid options: cluster";
        out.rc = 3498;
        return out;
      }
    }
  }
  if (auto shapeErr = vceOptionShapeErrorM4(parsed)) {
    out.errored = true;
    out.errorMsg = *shapeErr;
    return out;
  }
  std::optional<std::string> vceVal;
  std::optional<std::string> clusterOpt;
  bool robustFlag = false;
  for (const auto& [k, v] : parsed.opts) {
    if ((k == "vce" || k == "vcov") && !vceVal.has_value()) {
      vceVal = v.isBool ? std::string() : v.strValue;
    } else if (k == "robust" && v.isBool && v.boolValue) {
      robustFlag = true;
    } else if (k == "cluster" && !v.isBool) {
      clusterOpt = trimM4(v.strValue);
    }
  }
  if (sawRobustFlag) *sawRobustFlag = robustFlag;
  if (vceVal.has_value() && !vceVal->empty()) {
    // W1b: `vce()`'s argument abbreviates -- ONE canonicaliser for all three
    // sites (piet/handlers.h `canonicalVceType`), measured floors.
    std::string vceRest;
    const std::string lv = canonicalVceType(*vceVal, &vceRest);
    // FU-117: the token after a non-cluster vce type was dropped (HC1 at rc 0).
    // Official commands refuse it as Stata's _vce_parse does
    // (w2l_vce_shape.log:407 regress, :764 areg, :3580 ivreghdfe: r(198)); the
    // community commands that reach this helper accept it (:2972 ppmlhdfe,
    // :4034 reghdfe, csdid :2471), so they are exempt by command word.
    if (!vceRest.empty() && (lv == "robust" || lv == "hc1" || lv == "iid") &&
        !vceTrailingTokenAcceptedM4(parsed.cmd)) {
      out.errored = true;
      out.errorMsg = "option vce() incorrectly specified: `" + vceRest + "` cannot follow vce(" +
                     lv + ")";
      return out;
    }
    if (lv == "robust" || lv == "hc1") {
      out.argv = {"--vcov", "hc1"};
    } else if (lv == "cluster") {
      std::string cvar = clusterOpt.value_or("");
      if (!vceRest.empty()) cvar = vceRest;
      if (cvar.empty() && parsed.cmd == "ppmlhdfe") {
        // reghdfe's parse_vce, which ppmlhdfe shares: r(9)
        // (w2l_vce_shape_det.log:2893 ppmlhdfe_vce_cluster_novar).
        out.errored = true;
        out.errorMsg = "Can't specify cluster without clustervars (and viceversa)";
        out.rc = 9;
        return out;
      }
      if (cvar.empty()) {
        out.errored = true;
        out.errorMsg = "Option `vce(cluster)` is not supported in this version of PIET. "
                        "Cluster variable required: vce(cluster varname)";
        return out;
      }
      ClusterSplitM4 cs = splitClusterVarsM4(cvar, multiwayClusterMeasuredM4(parsed.cmd));
      if (cs.errored) {
        out.errored = true;
        out.errorMsg = cs.errorMsg;
        return out;
      }
      out.argv = cs.argv;
    } else if (lv == "iid") {
      out.argv = {"--vcov", "iid"};
    } else if (!unimplementedVceType(*vceVal).empty()) {
      out.errored = true;
      out.errorMsg = "Option `vce(" + unimplementedVceType(*vceVal) +
                     ")` is a valid Stata option that this version of PIET does not implement";
      return out;
    } else {
      out.errored = true;
      out.errorMsg = "Unknown vce type: " + *vceVal;
      return out;
    }
  } else if (clusterOpt.has_value() && !clusterOpt->empty()) {
    // BUG-055 / P2: Stata's legacy `cluster(varname)` — with neither vce()
    // nor robust given — is a synonym for `vce(cluster varname)`: it
    // implies robust-cluster VCE, not iid. Before this fix, `clusterOpt`
    // was parsed (above) but only ever consulted from inside the
    // `vce(cluster ...)` branch, so a bare `cluster(v)` fell through both
    // `if`s and silently ran iid (v wholly ignored) for every caller of
    // this shared helper (xtreg fe/re, logit, probit). Oracle
    // P2_legacycluster (mirrors handlers.cpp::runRegress's identical fix).
    // BUG-537 (W2 estimopts): tested BEFORE the bare-`robust` branch, which
    // used to win and drop the cluster (HC1, N_clust 0) whenever both were
    // typed -- `xtreg ..., robust fe cluster(firmID)` x12 in one corpus
    // file. Stata's cluster wins: w2_pweight.log R5_pw_robust_AND_cluster_
    // equals_R4 is byte-identical to R4. Twin ordering in runRegress.
    ClusterSplitM4 cs = splitClusterVarsM4(*clusterOpt, multiwayClusterMeasuredM4(parsed.cmd));
    if (cs.errored) {
      out.errored = true;
      out.errorMsg = cs.errorMsg;
      return out;
    }
    out.argv = cs.argv;
  } else if (robustFlag) {
    out.argv = {"--vcov", "hc1"};
  }
  return out;
}

// build_estimation_argv (translate.py) — shared skeleton for xtreg/logit/
// probit's argv construction (fe/absorb/vcov/noconstant/level/TS-injection).
// `extraFe` mirrors the xtreg,fe router's `extra={"fe": panelvar}` override.
// struct EstimationArgvResult: defined in estimation_m4_internal.h (R2 split)

// W:P5 fit-sample fidelity (BUG-163). The fit argv is ground truth for what
// restriction/weights the engine actually estimated under — scan it once at
// LastModel-caching time so every post-estimation refit can replay the SAME
// sample instead of silently answering from the full dataset. Called at every
// `lm.dataPath = ...` site in this file (handlers.cpp's runRegress sets
// lm.fitDataPath directly instead — it materializes its if/in filter into a
// scratch file rather than passing --keepif).
void captureFitFilterM4(LastModel& lm, const std::vector<std::string>& argv) {
  for (size_t i = 0; i + 1 < argv.size(); ++i) {
    if (argv[i] == "--keepif") lm.fitKeepif = argv[i + 1];
    if (argv[i] == "--weight") lm.fitWeight = argv[i + 1];
    if (argv[i] == "--wtype") lm.fitWtype = argv[i + 1];
  }
}

EstimationArgvResult buildEstimationArgvM4(const std::string& cobraCmd,
                                            const eval::ParsedCommand& parsed, SessionState& session,
                                            const std::optional<std::string>& extraFe) {
  EstimationArgvResult res;
  if (parsed.varlist.empty()) {
    res.errored = true;
    res.errorBlock = engineErrorBlockM4("syntax_error: no variables specified", "syntax_error");
    return res;
  }
  std::string y = parsed.varlist[0];
  std::vector<std::string> xs(parsed.varlist.begin() + 1, parsed.varlist.end());

  std::vector<std::string> argv = {cobraCmd, "--y", y};
  if (!xs.empty()) {
    argv.push_back("--x");
    argv.push_back(joinCommaM4(xs));
  } else {
    argv.push_back("--x");
    argv.push_back("(intercept)");
  }
  if (parsed.ifExpr.has_value()) {
    argv.push_back("--keepif");
    argv.push_back(*parsed.ifExpr);
  }

  // --fe: extraFe (xtreg,fe router) OR absorb() option.
  std::string fe = extraFe.value_or("");
  if (fe.empty()) {
    for (const auto& [k, v] : parsed.opts) {
      if (k == "absorb" && !v.isBool) fe = v.strValue;
    }
  }
  if (!fe.empty()) {
    std::string joined;
    std::string cur;
    for (char c : fe) {
      if (c == ' ' || c == ',') {
        if (!cur.empty()) {
          if (!joined.empty()) joined += ",";
          joined += cur;
          cur.clear();
        }
      } else {
        cur += c;
      }
    }
    if (!cur.empty()) {
      if (!joined.empty()) joined += ",";
      joined += cur;
    }
    if (!joined.empty()) {
      argv.push_back("--fe");
      argv.push_back(joined);
    }
  }

  // xtreg's _vce_parse takes vce(cluster a b) on to its own panel checks.
  if (auto bad = clusterSpecRefusalM4(parsed, session, ClusterOptParserM4::kVceParse,
                                      /*multiwayVceRefused=*/parsed.cmd != "xtreg")) {
    res.errored = true;
    res.errorBlock = *bad;
    return res;
  }
  VcovResult vr = vcovFromOpts(parsed);
  if (vr.errored) {
    res.errored = true;
    res.errorBlock = vcovErrorBlockM4(vr);
    return res;
  }
  // P3 / BUG-056: cobra's logit/probit estimator does not implement
  // cluster-robust VCE. Reject HONESTLY here — whether the user asked via
  // `vce(cluster ...)` or the legacy `cluster()` synonym (BUG-055, above,
  // now maps to the exact same "--vcov cluster" shape in vr.argv for every
  // caller of this shared builder) — instead of forwarding a flag cobra
  // would otherwise have to silently drop or bounce as an unknown-flag
  // error (engine E6 net). vr.argv is always exactly {"--vcov", TYPE, ...}
  // (see every branch of vcovFromOpts above), so this check is precise.
  // BS-067: also catches "twoway" (a 2-var cluster now that
  // splitClusterVarsM4 exists) — logit/probit never gained cluster-robust
  // VCE of any shape, so a `vce(cluster A B)` must reject the SAME way a
  // single-var `vce(cluster A)` always has, not slip past this guard just
  // because vr.argv[1] reads "twoway" instead of "cluster".
  // W:P5: single-variable vce(cluster) is now IMPLEMENTED for logit/probit
  // (weighted/unweighted score sandwich in the engine, src/estimators/logit),
  // so the blanket reject above narrows to the shape that is still missing:
  // two-way clustering, which no MLE path in this engine computes. Keeping
  // the reject for "twoway" is the honest half — a silent fallback to
  // one-way would answer a different question.
  // cloglog (BUG-1169/1170) is the third binary ML verb and shares their CLI.
  const bool binaryMl = cobraCmd == "logit" || cobraCmd == "probit" || cobraCmd == "cloglog";
  if (binaryMl && vr.argv.size() >= 2 &&
      vr.argv[0] == "--vcov" && vr.argv[1] == "twoway") {
    res.errored = true;
    res.errorBlock = engineErrorBlockM4(
        "vce(cluster A B) — two-way clustering is not supported for logit/probit in this engine; "
        "one-way vce(cluster A) is",
        "unsupported_option");
    return res;
  }
  // BUG-536 (W2 estimopts): a probability weight IMPLIES a robust VCE in
  // Stata -- w2_pweight.log R2_pw is byte-identical to R3 (`vce(robust)`),
  // and X4_pw_fe to X5 (the xtreg,fe rewrite below turns this hc1 into the
  // panel cluster). Twin of runRegress's rule: only when no vce()/robust/
  // cluster() chose one. logit's engine refused a bare pweight outright
  // ("requires vce(robust)", rc 1) where Stata runs it robust at rc 0.
  if (vr.argv.empty() && parsed.weightType.has_value() && parsed.weightVar.has_value() &&
      !parsed.weightVar->empty()) {
    const std::string wt = lowerM4(*parsed.weightType);
    if (wt == "pw" || wt == "pweight") vr.argv = {"--vcov", "hc1"};
  }
  for (const auto& a : vr.argv) argv.push_back(a);

  // W:P5: forward the weight, normalized to cobra's 2-letter --wtype
  // spelling — the same shape handlers.cpp::runRegress has used since P3.
  // The tokenizer's registry gate (registry.cpp WeightPolicy) already
  // decided WHICH types are legal for this command, so any weight reaching
  // here is one the command declares support for; an engine that cannot
  // honour it surfaces its own error through the ordinary CobraError catch.
  if (parsed.weightType.has_value() && parsed.weightVar.has_value() &&
      !parsed.weightVar->empty()) {
    const std::string wt = lowerM4(*parsed.weightType);
    std::string wtype;
    if (wt == "aw" || wt == "aweight") wtype = "aw";
    else if (wt == "fw" || wt == "fweight") wtype = "fw";
    else if (wt == "iw" || wt == "iweight") wtype = "iw";
    else if (wt == "pw" || wt == "pweight") wtype = "pw";
    if (!wtype.empty()) {
      argv.push_back("--weight");
      argv.push_back(*parsed.weightVar);
      argv.push_back("--wtype");
      argv.push_back(wtype);
    }
  }

  for (const auto& [k, v] : parsed.opts) {
    if ((k == "noconstant" || k == "nocons") && v.isBool && v.boolValue) {
      argv.push_back("--no-intercept");
    }
  }
  for (const auto& [k, v] : parsed.opts) {
    if (k == "level" && !v.isBool) {
      try {
        int lv = std::stoi(v.strValue);
        if (lv < 1 || lv > 99) {
          res.errored = true;
          res.errorBlock = engineErrorBlockM4("level(#) must be an integer between 1 and 99.",
                                              "unsupported_option");
          return res;
        }
        argv.push_back("--level");
        argv.push_back(std::to_string(lv));
      } catch (...) {
        res.errored = true;
        res.errorBlock = engineErrorBlockM4("level(#) requires an integer argument, e.g. level(90).",
                                            "unsupported_option");
        return res;
      }
    }
  }
  // BS-048 (BETA A4) — NOT extended here: verified that cobra's own
  // xtreg/logit/probit CLIs never read --level at all (grepped every
  // apps/cmds/*.cpp `opt(a, "--level", ...)` site — only qreg/sdtest/ttest/
  // mediate do, and only ttest is wired into PIET's dispatch surface today;
  // runTtest carries the session.level fallback instead). Sending --level
  // to a command that never consumes it trips cobra's E6 unconsumed-flag
  // net (apps/cmds/common.cpp::check_unconsumed_flags) and turns an
  // otherwise-successful estimation into a hard error — so a session.level
  // fallback HERE would make an ordinary `set level 90` + subsequent
  // xtreg/logit/probit call newly error where it worked before (a
  // regression this fix must not introduce). The pre-existing explicit
  // `level(#)` handling above has the SAME latent gap (untested by any
  // freeze fixture — no `level(...)` usage anywhere in debug_all.do) but is
  // OUT of this phase's 4-mechanism scope to fix; flagged for the operator.

  // P4 / BUG-060: unknown-option rejection net for xtreg(fe/re)/logit/
  // probit. Known-option set is the UNION of what THIS shared builder
  // consumes (absorb/vce/vcov/robust/cluster/noconstant/nocons/level,
  // above) and what runXtreg's caller-side checks already consume BEFORE
  // invoking this builder (fe/re model-select; be/gls/mle/ec1/pa reject) —
  // one shared, deliberately-permissive net rather than a per-caller list,
  // so an option one caller legitimately handles is never misflagged as
  // leftover for another caller of this same builder (conservative by
  // design: false-accept is safe here, false-reject is not — see the
  // BUG-060 report for the full over-rejection sweep list). No curated
  // "valid-Stata-but-gap" bucket here (unlike regress's `beta`): no
  // evidenced case yet, so every leftover reads as the r(198)-parity
  // "not allowed" message.
  static const std::set<std::string> kEstimationKnownOpts = {
      "absorb", "vce", "vcov", "robust", "cluster", "noconstant", "nocons",
      "level",  "fe",  "re",   "be",     "gls",     "mle",        "ec1",  "pa",
  };
  // BUG-333: `or` (odds-ratio display) is logit-ONLY. It is gated on cobraCmd
  // rather than added to the shared set above, because this builder also serves
  // probit/xtreg/etc. — widening the shared set would manufacture an
  // over-acceptance on the other commands. `or` is consumed in the dispatcher
  // (display metric, never reaches the engine).
  const bool logitOnlyOk = (cobraCmd == "logit");
  // FU-425: Stata's maximize options, on logit AND probit: both pass them
  // to the shared ML-options parser, whose syntax line
  // is ITERate(int) TOLerance(real) LTOLerance(real) NRTOLerance(real)
  // DIFficult; the capitalised stem is the minimum abbreviation. BUG-333 had
  // gated `iterate` on logit alone, so `probit ..., iterate(300)` was r(198)
  // where Stata runs it (w2e_probes.log:412 lp_probit_tight_ctl, rc 0). Gated on
  // the two commands, not the shared set, for the reason `or` is: xtreg and the
  // other callers of this builder must not start accepting them. Forwarded below
  // under the engine's spellings (cmds_mle.cpp parse_ml_max_options).
  const bool mlMaxOk = binaryMl;
  struct MlMaxOpt { const char* name; size_t minLen; bool valued; };
  static const MlMaxOpt kMlMaxOpts[] = {{"iterate", 4, true},     {"tolerance", 3, true},
                                        {"ltolerance", 4, true},  {"nrtolerance", 5, true},
                                        {"difficult", 3, false}};
  // The canonical maximize option `k` spells (nullptr when none), and only in
  // its own shape: a valued one with a value, `difficult` bare.
  auto mlMaxOptOf = [&](const std::string& k, const eval::OptValue& v) -> const MlMaxOpt* {
    if (!mlMaxOk) return nullptr;
    for (const MlMaxOpt& o : kMlMaxOpts) {
      const std::string name = o.name;
      if (k.size() >= o.minLen && k.size() <= name.size() && name.compare(0, k.size(), k) == 0)
        return (o.valued ? !v.isBool : (v.isBool && v.boolValue)) ? &o : nullptr;
    }
    return nullptr;
  };
  // BUG-331: `offset()` is a logit/probit option in Stata (StataBE 19.5 oracle,
  // 2026-08-17) and the engine implements it as of cobra 0.42.0. Gated on the two
  // commands that have it rather than widened into the shared set, for the same
  // reason `or`/`iterate` are: this builder also serves xtreg and friends.
  const bool offsetOk = (cobraCmd == "logit" || cobraCmd == "probit");
  // FU-468: `asis` keeps perfect predictors and their rows (logit's help,
  // probit's help); a flag, forwarded as the engine's --asis below.
  const bool asisOk = offsetOk || cobraCmd == "cloglog";  // cobra cloglog has no --offset
  for (const auto& [k, v] : parsed.opts) {
    (void)v;
    if (kEstimationKnownOpts.count(k)) continue;
    if (logitOnlyOk && k == "or") continue;
    if (mlMaxOptOf(k, v) != nullptr) continue;
    if (offsetOk && k == "offset") continue;
    if (asisOk && k == "asis" && v.isBool) continue;
    res.errored = true;
    res.errorBlock = engineErrorBlockM4("option `" + k + "` not allowed", "unsupported_option");
    return res;
  }

  // GLM-style: cobra's logit/probit CLI spells its VCE selector `--vce`, not
  // the `--vcov` every linear estimator uses, and names the robust shape
  // "robust" rather than "hc1". W:P5 widens this remap from robust-only to
  // cluster as well — the engine now implements clustered MLE VCE, and
  // leaving `--vcov cluster` unmapped made a supported combination fail as an
  // unrecognized flag. `--cluster VAR` rides along unchanged (same spelling
  // on both CLIs).
  if (binaryMl) {
    for (size_t i = 0; i < argv.size(); ++i) {
      if (argv[i] != "--vcov" || i + 1 >= argv.size()) continue;
      if (argv[i + 1] == "hc1") {
        argv[i] = "--vce";
        argv[i + 1] = "robust";
      } else if (argv[i + 1] == "cluster") {
        argv[i] = "--vce";
      }
    }
  }

  // TS-context injection (MI-083 §16) — same helper regress uses.
  std::vector<std::string> tsTokens = {y};
  for (const auto& x : xs) tsTokens.push_back(x);
  if (translate::hasTsToken(tsTokens)) {
    argv = translate::injectTsContext(argv, session);
  }

  // BUG-333 / FU-425: forward the maximize options (logit and probit) as
  // --iterate/--tolerance/--ltolerance/--nrtolerance/--difficult. This MUST push
  // onto the local `argv`, not onto res.argv — the assignment below overwrites
  // res.argv wholesale, which silently swallowed the first version of this
  // forward (measured: `logit ..., iterate(2)` returned the un-truncated default
  // fit). The first spelling of each option wins, as the --iterate forward did.
  // The engine echoes them (`ml_max_argv`) and every post-estimation refit
  // carries them (estimation_m4c.cpp pushFitAsisM4), so lincom/test/margins
  // answer from the posted iterate (FU-426).
  // A value the engine's parse_ml_max_options would refuse (exit 2, which this
  // layer reports as rc 1) is refused here by name at r(198) instead: a
  // tolerance that is not a nonnegative number, an iterate() outside the
  // engine's [1, 16000]. No captured record covers these values (Stata's
  // mlopts takes iterate(0) and any real tolerance), so this is a refusal of a
  // shape this build does not run, not a claim about Stata's answer.
  // BUG-872: the value is a decimal literal parsed whole (no hex, inf or nan),
  // and iterate() is forwarded as the integer it parsed to, never as typed: the
  // engine reads --iterate as a plain integer, so `iterate(1e3)` forwarded
  // verbatim used to run ONE iteration at rc 0 (e(ic) 1, e(converged) 0).
  if (mlMaxOk) {
    std::set<std::string> sent;
    for (const auto& [k, v] : parsed.opts) {
      const MlMaxOpt* o = mlMaxOptOf(k, v);
      if (o == nullptr || !sent.insert(o->name).second) continue;
      if (o->valued) {
        const std::string val = strutil::trim(v.strValue);
        char* end = nullptr;
        const double d = std::strtod(val.c_str(), &end);
        const bool decimal = val.find_first_not_of("0123456789.eE+-") == std::string::npos;
        const bool parsed_all =
            !val.empty() && decimal && end != nullptr && *end == '\0' && std::isfinite(d);
        const bool isIterate = std::string(o->name) == "iterate";
        // iterate(0) reports the starting values (BUG-1244: StataBE e(converged) 0,
        // e(ic) 0, the constant-only start; the engine takes --iterate 0).
        const bool ok = parsed_all && (isIterate ? (d == std::floor(d) && d >= 0 && d <= 16000) : d >= 0);
        if (!ok) {
          res.errored = true;
          res.errorBlock = engineErrorBlockM4(
              std::string("option ") + o->name + "() " +
                  (isIterate ? "must be an integer from 0 to 16000 in this build"
                             : "must be a nonnegative number in this build") +
                  " (got `" + v.strValue + "`)",
              "unsupported_option");
          return res;
        }
      }
      argv.push_back(std::string("--") + o->name);
      if (o->valued) {
        const std::string val = strutil::trim(v.strValue);
        argv.push_back(std::string(o->name) == "iterate"
                           ? std::to_string(static_cast<int>(std::strtod(val.c_str(), nullptr)))
                           : val);
      }
    }
  }

  // BUG-331: forward `offset(VAR)`. Same local-`argv` rule as --iterate above —
  // res.argv is overwritten wholesale below, so pushing onto res.argv here would
  // silently swallow the flag (the defect BUG-333's comment records).
  if (offsetOk) {
    for (const auto& [k, v] : parsed.opts)
      if (k == "offset" && !v.isBool) {
        argv.push_back("--offset");
        argv.push_back(v.strValue);
        break;
      }
  }
  if (asisOk) {
    for (const auto& [k, v] : parsed.opts)
      if (k == "asis" && v.isBool && v.boolValue) {
        argv.push_back("--asis");
        break;
      }
  }

  res.argv = argv;
  return res;
}

// P6: the ONE choke point every cobra invocation built in this file passes
// through, so the persisted `provenance_hash` setting (settings.h/.cpp)
// applies uniformly across every M4a estimator and post-estimation command
// without a per-call-site decision. Interactive default is OFF
// (Settings::provenanceHash = false) -> append `--no-provenance` (VERIFIED
// E6-net-safe: cobra consumes it, never rejects it, and simply omits its
// `provenance{}` block from the result JSON). The user opting IN
// (provenanceHash = true) appends nothing, so cobra's own CLI default
// (provenance ON) is unchanged — this is a PIET-native default flip only,
// never the engine's.
nlohmann::json runCobraJsonM4(std::vector<std::string> args) {
  if (!piet::settings::load().provenanceHash) {
    args.push_back("--no-provenance");
  }
  return runCobraJson(args);
}

std::string nextPathM4(const std::string& workdir, const std::string& prefix) {
  return mintScratchPath(workdir, prefix);  // BUG-389: one body, registered at mint
}

// BUG-042: shared if-prefilter for tabstat/correlate/ttest — none of the
// three has a --keepif of its own on cobra's CLI (checked against each
// subcommand's own usage banner), so this mirrors handlers.cpp's
// runSummarize if-prefilter (the SAME row-filter infra: `cobra eval
// --keepif COND --out tmp.hbin`, then point the real call at the filtered
// copy instead of the live hbin). No ifExpr/inRange -> returns hbinPath
// unchanged, zero behavior change on the unqualified path (freeze-compatible).
// On a cobra error, returns nullopt and leaves the CommandErrorResult block
// in `errBlockOut` for the caller to return directly.
// `inRange` (BS-090, BETA Phase G, default nullopt): an optional `in a/b`
// row-range, composed with `ifExpr` via AND through
// piet::handlers::applyIfAndInFilter — only ttest/correlate/pwcorr pass a
// real value here (registry.cpp's allowInRange); tabstat/tobit's existing
// call sites are untouched (still pass no 5th argument), so their behavior
// is byte-identical to before this phase.
std::optional<std::string> applyIfFilterM4(SessionState& session, const std::string& hbinPath,
                                           const std::optional<std::string>& ifExpr,
                                           ojson& errBlockOut,
                                           const std::optional<std::string>& inRange) {
  bool haveIf = ifExpr.has_value() && !ifExpr->empty();
  bool haveIn = inRange.has_value() && !inRange->empty();
  if (!haveIf && !haveIn) return hbinPath;
  InRangeFilterResult fr;
  try {
    fr = applyIfAndInFilter(session, hbinPath, ifExpr, inRange);
  } catch (const CobraError& ce) {
    errBlockOut = engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
    return std::nullopt;
  }
  if (!fr.ok) {
    errBlockOut = inRangeRejectError(fr, *inRange, "this command");
    return std::nullopt;
  }
  return fr.dataPath;
}

// BS-069 (BETA D2): Stata's tobit accepts BARE `ll`/`ul` (no numeric value)
// meaning "censor at the depvar's own observed min/max" — cobra's tobit
// CLI has no equivalent (live-verified: `--ll` with no following value fails
// inside cobra's own arg parser with "stod: no conversion", exit 1). This
// computes the min/max of `yName` over the SAME sample cobra will fit on
// (the `if`-filtered copy, reusing applyIfFilterM4 above) so PIET can pass
// an explicit numeric value either way. Returns false + fills errBlockOut on
// any failure (missing data, non-numeric column, filter error, no rows).
bool computeYMinMaxM4(SessionState& session, const std::string& yName,
                       const std::optional<std::string>& ifExpr, double& outMin, double& outMax,
                       ojson& errBlockOut) {
  std::string basePath = *session.currentHbin();
  ojson filterErr;
  auto filteredPath = applyIfFilterM4(session, basePath, ifExpr, filterErr);
  if (!filteredPath.has_value()) {
    errBlockOut = filterErr;
    return false;
  }
  bool madeTemp = (*filteredPath != basePath);
  std::optional<Datastore> tempDs;
  Datastore* ds = nullptr;
  if (madeTemp) {
    tempDs.emplace(*filteredPath);
    ds = &*tempDs;
  } else {
    ds = session.datastore();
  }
  bool ok = false;
  if (ds != nullptr && ds->has(yName) && ds->dtype(yName) == "f64") {
    const cecon::Column& c = ds->col(yName);
    const double* d = c.f64();
    int64_t count = c.nbytes / static_cast<int64_t>(sizeof(double));
    double mn = std::numeric_limits<double>::infinity();
    double mx = -std::numeric_limits<double>::infinity();
    bool any = false;
    for (int64_t i = 0; i < count; ++i) {
      double v = d[i];
      if (std::isnan(v)) continue;
      any = true;
      if (v < mn) mn = v;
      if (v > mx) mx = v;
    }
    if (any) {
      outMin = mn;
      outMax = mx;
      ok = true;
    }
  }
  if (madeTemp) cecon::native_unlink(*filteredPath);
  if (!ok) {
    errBlockOut = engineErrorBlockM4(
        "tobit: could not compute the observed min/max of `" + yName + "` for a bare ll/ul",
        "engine_error");
    return false;
  }
  return true;
}

std::optional<std::vector<int64_t>> esampleRowsM4(SessionState& session, const LastModel& lm) {
  if (lm.esample != LastModel::EsampleState::kColumn || lm.esampleSerial == 0) return std::nullopt;
  try {
    Datastore* ds = session.datastore();
    if (ds == nullptr || !ds->has(kEsampleColumn) || ds->dtype(kEsampleColumn) != "f64")
      return std::nullopt;
    const double* mk = ds->col(kEsampleColumn).f64();
    const double serial = static_cast<double>(lm.esampleSerial);
    std::vector<int64_t> rows;
    for (int64_t i = 0; i < ds->nRows(); ++i)
      if (mk[i] == serial) rows.push_back(i);
    return rows;
  } catch (const std::exception&) {
    return std::nullopt;  // an unreadable frame: the caller posts nothing that needs the rows
  }
}

const double* f64ColumnM4(SessionState& session, const std::string& name) {
  try {
    Datastore* ds = session.datastore();
    if (ds == nullptr || name.empty() || !ds->has(name) || ds->dtype(name) != "f64") return nullptr;
    return ds->col(name).f64();
  } catch (const std::exception&) {
    return nullptr;
  }
}

}  // namespace est_detail
using namespace est_detail;

// ===========================================================================
// Stata's built-in multiway cluster e() (FU-388 / FU-402 / FU-406). regress
// (Stata's multiway-cluster step), areg and xtreg,fe (Stata's absorb step)
// post, after a VCE with two or more cluster variables: no e(N_clust),
// e(rank) = the rank of the clipped variance (r(rankc)), the e(kcluster) matrix
// (1 x every cluster combination, row "#Groups", columns cluster1..) and the macros
// e(cluster1..) naming each combination (`id#id2`). The engine's feols reply under
// --multiway-subset-factors carries `vcov_rank` and `kcluster` (apps/cmds/common.cpp
// run_feols). w2s_multiway_class.log:116 regress `.|6|2|180`, :133
// `30|7|180|id|id2|id#id2`, :691 xtreg `.|4|30|180|cg1 cg2|2`, :1115/:1126 three-way.
// Shared with handlers.cpp (runRegress), which declares both itself.
//
// The cached reply's half: run on the reply that becomes LastModel::result, BEFORE
// setLastModel (which mirrors it into the resolver), like BUG-570's n_clusters erase.
// The emitted block keeps the engine's fields.
void applyBuiltinMultiwayReplyM4(ojson& result) {
  if (!result.is_object()) return;
  result.erase("n_clusters");
  if (result.contains("vcov_rank") && result["vcov_rank"].is_number())
    result["k_vars"] = result["vcov_rank"];
}

// The e() stores' half, AFTER setLastModel: e(kcluster) and e(cluster#). Recorded
// with noteEOnFit so the next install drops them (SessionState::dropEOnFit), as it
// does any name written on top of a fit.
void postBuiltinMultiwayEM4(SessionState& session, const ojson& raw) {
  if (!raw.is_object() || !raw.contains("kcluster") || !raw["kcluster"].is_array() ||
      raw["kcluster"].empty())
    return;
  eval::EvalMatrix m;
  m.rownames = {"#Groups"};
  m.data.emplace_back();
  size_t i = 0;
  for (const auto& c : raw["kcluster"]) {
    if (!c.is_object() || !c.contains("levels") || !c["levels"].is_number()) return;
    ++i;
    m.colnames.push_back("cluster" + std::to_string(i));
    m.data[0].push_back(c["levels"].get<double>());
  }
  auto& store = session.eval.eMatrices;
  store.erase(std::remove_if(store.begin(), store.end(),
                             [](const auto& p) { return p.first == "kcluster"; }),
              store.end());
  store.emplace_back("kcluster", std::move(m));
  session.noteEOnFit("kcluster");
  i = 0;
  for (const auto& c : raw["kcluster"]) {
    const std::string name = "cluster" + std::to_string(++i);
    session.eval.setEString(name, c.value("name", std::string()));
    session.noteEOnFit(name);
  }
}

// ===========================================================================
// Stata's ordered e() name lists (`: e(scalars)`, `: e(macros)`, `: e(matrices)`)
// for the fits whose whole Stata namespace PIET posts (w2s_multiway_class mw_*_scalars
// /_macros/_lists, w2v_estpost_ematrix ep15d_ematrices). Each path below posts every
// name Stata lists that PIET's lazily resolved e() did not carry, with Stata's value,
// then installs the list (SessionState::ENameContract; eNameOrderFor re-checks it
// against the live stores on every read). A kind whose names cannot all be posted
// with Stata's value is left unclaimed, so `: e(kind)' keeps its r(111) refusal.
//
// The orders are Stata's e() store order (StataBE 19.5 on the oracle machine):
// e(macros) is newest-first and a re-set name moves to the front, e(matrices) is
// `b V` then newest-first (both reproduce from the ado posting sequences, e.g.
// regress's own posts then Stata's multiway-cluster step for the multiway regress
// lists); e(scalars) follows the store's own order, which the ado sequence alone does
// not determine (areg's list, w2s_multiway_class.log mw_areg_c2_scalars, has
// `ar2 p F r2_a` after the hidden/historical posts), so each scalar list is the one
// measured for that path. Names that depend on the specification are generated:
// e(cluster1..C) for the C cluster combinations.
namespace {

void putEScalarNL(SessionState& s, const std::string& n, double v) {
  s.eval.setEScalar(n, v);
  s.noteEOnFit(n);
}
void putEStringNL(SessionState& s, const std::string& n, const std::string& v) {
  s.eval.setEString(n, v);
  s.noteEOnFit(n);
}
void putEMatrixNL(SessionState& s, const std::string& n, eval::EvalMatrix m) {
  auto& store = s.eval.eMatrices;
  store.erase(std::remove_if(store.begin(), store.end(),
                             [&n](const auto& p) { return p.first == n; }),
              store.end());
  store.emplace_back(n, std::move(m));
  s.noteEOnFit(n);
}
std::optional<double> rawNumNL(const ojson& raw, const char* key) {
  if (!raw.is_object() || !raw.contains(key) || !raw[key].is_number()) return std::nullopt;
  return raw[key].get<double>();
}
// The regress-family log likelihood of a Gaussian fit with residual (or, for ll_0,
// total) sum of squares `ss` over N observations: regress's e(ll)/e(ll_0)
// (-N/2 (1 + ln 2pi + ln(ss/N)); StataBE regress c2 e(ll) -429.7924086070367 with
// rss 1249.565, e(ll_0) -590.9762832476949 with tss 7491.19; areg/xtreg,fe post
// r(ll0) over the within sum of squares, e(ll_0) -577.19 with tss_w 6427.50).
double gaussLogLikNL(double N, double ss) {
  return -0.5 * N * (1.0 + std::log(2.0 * 3.14159265358979323846) + std::log(ss / N));
}
// e(cmdline): the ado's `"<cmd> `0'"' -- the command word as Stata names it, then
// the arguments as typed (StataBE `reg y x1 x2 , vce(cluster id   id2)` posts
// "regress y x1 x2 , vce(cluster id   id2)").
std::string stataCmdlineNL(const std::string& word, const eval::ParsedCommand& parsed,
                           const std::string& expanded = std::string()) {
  // the dispatched line before tokenize, which keeps the trailing blanks parsed.raw drops
  const std::string& s = expanded.empty() ? parsed.raw : expanded;
  size_t i = s.find_first_not_of(" \t");
  if (i == std::string::npos) return word;
  size_t j = s.find_first_of(" \t,", i);
  if (j == std::string::npos) return word;
  size_t k = s.find_first_not_of(" \t", j);
  if (k == std::string::npos) return word;
  // Trailing blanks stay: Stata's `0` keeps those a macro or xi's substitution
  // leaves (`regress y x1 ` after `xi: regress y x1 i.one`, BUG-1048); a typed
  // line arrives here already trimmed, as Stata trims it.
  return word + " " + s.substr(k);
}
// Record the engine's e(beta) coefficients (`beta_std`) or e(V_modelbased)
// (`e_Vmb`) on the live session block, IN PLACE: the session block is mutated,
// never reassigned or swapped, so its identity (liveEstimationIdentity, the
// address r(table) and the e(b) stripe annotation key on) is the fit's own. The
// matrices are built at the moment of need by EvalSession::resolveEMatrixFor
// from a copy of this block through the e(b)/e(V) reader, after
// annotateEbStripe has recorded the base-level columns. Session block only,
// like e_V and `noconstant`: the console block (freeze-pinned) is unchanged.
bool recordOnBlockNL(SessionState& session, const ojson& raw, const char* key) {
  if (!session.lastEstBlock.has_value() || !session.lastEstBlock->is_object() ||
      !raw.is_object() || !raw.contains(key) || !raw[key].is_object() || raw[key].empty())
    return false;
  (*session.lastEstBlock)[key] = raw[key];
  return true;
}
std::vector<std::string> clusterMacrosNL(size_t combos) {
  std::vector<std::string> v;
  for (size_t i = 1; i <= combos; ++i) v.push_back("cluster" + std::to_string(i));
  return v;
}
void installENameContractNL(SessionState& session, const std::string& cmd,
                            std::optional<std::vector<std::string>> scalars,
                            std::optional<std::vector<std::string>> macros,
                            std::optional<std::vector<std::string>> matrices,
                            std::vector<std::string> missingScalars = {}) {
  if (!session.lastModel().has_value()) return;
  SessionState::ENameContract c;
  c.fitSerial = session.lastModel()->fitSerial;
  c.cmd = cmd;
  c.scalars = std::move(scalars);
  c.macros = std::move(macros);
  c.matrices = std::move(matrices);
  c.missingScalars = std::move(missingScalars);
  session.eNameContract = std::move(c);
}
// The options whose presence leaves the e() namespace of a regress/areg/xtreg fit as
// measured (vce/robust/cluster pick the measured VCE path; level is display-only).
// Anything else (beta, hascons, tsscons, notable, noheader, absorb options...) is
// not measured, and the fit keeps the refusal.
bool onlyOptionsNL(const eval::ParsedCommand& parsed, std::initializer_list<const char*> allowed) {
  for (const auto& [k, v] : parsed.opts) {
    (void)v;
    bool ok = false;
    for (const char* a : allowed) ok = ok || k == a;
    if (!ok) return false;
  }
  return true;
}

}  // namespace

// regress (one-way: OLS, vce(robust), vce(cluster v); built-in multiway) and areg
// (built-in multiway), unweighted, with the constant; and a weighted or noconstant
// one-way regress (BUG-1542), whose values are posted without the ordered name lists.
// Called by runRegress right after the multiway e() hook. `raw` is the engine reply as
// the fit received it.
void postENameListsRegressM4(SessionState& session, const ojson& raw,
                             const eval::ParsedCommand& parsed, const std::string& stataCmdIn,
                             bool builtinMultiway, bool noConstant) {
  session.eNameContract.reset();
  const std::string stataCmd = stataCmdIn == "reg" ? std::string("regress") : stataCmdIn;
  const auto& lmo = session.lastModel();
  if (!lmo.has_value() || !raw.is_object()) return;
  const bool weighted = !lmo->fitWeight.empty();
  if ((weighted || noConstant) && stataCmd != "regress") return;
  const LastModel& lm = *lmo;
  const bool multiway = builtinMultiway && (lm.vcov == "twoway" || lm.vcov == "multiway") &&
                        raw.contains("kcluster") && raw["kcluster"].is_array() &&
                        !raw["kcluster"].empty();
  const auto N = rawNumNL(raw, "n_obs");
  const auto rss = rawNumNL(raw, "rss");
  const auto mss = rawNumNL(raw, "mss");
  if (!N || !rss || !mss || *N <= 0 || *rss <= 0) return;
  const std::string cmdline = stataCmdlineNL(stataCmd, parsed, session.leafCmdExpanded);

  if (stataCmd == "regress") {
    if (!lm.fe.empty() ||
        !onlyOptionsNL(parsed, {"vce", "robust", "cluster", "level", "noconstant", "nocons"}))
      return;
    enum class Path { kOls, kRobust, kCluster1, kMultiway } path;
    if (multiway) path = Path::kMultiway;
    else if (lm.vcov == "iid") path = Path::kOls;
    else if (lm.vcov == "hc1") path = Path::kRobust;
    else if (lm.vcov == "cluster" && lm.cluster.size() == 1 &&
             lm.cluster[0].find(',') == std::string::npos)
      path = Path::kCluster1;
    else return;
    // [R] regress Stored results and regress's posting: e(sum_w) is N unweighted,
    // e(ll)/e(ll_0) the Gaussian log likelihoods of the fit and of the constant-only fit.
    // Weighted (BUG-1542, StataBE 19.5, 12 rows, sum(wf) 28.5): e(sum_w) is the weight
    // total and e(ll)/e(ll_0) the same Gaussian over e(N) and e(rss)/e(mss) -- [fw=] N 30,
    // ll -17.670289377592514; [iw=] N 28, -21.142846032770578, or N 12 on the normalized
    // weights under vce(robust)/vce(cluster) (-8.9550222657338434, the [pw=] value) --
    // plus, for [aw=] only, 0.5 * sum(ln(w_i * N / sum w)) over e(sample), each weight
    // read as a precision (-10.210580476229087 under every VCE). noconstant posts no
    // e(ll_0) and the same e(ll) over its own e(rss) (StataBE `regress y x, noconstant`
    // -25.243130074332818 under every VCE, [aw=] -27.799597115109073, [fw=]
    // -65.078391062216852, [iw=] -62.18388485682388). Not measured weighted or
    // noconstant: the built-in multiway path.
    if ((weighted || noConstant) && path == Path::kMultiway) return;
    double sumW = *N, llShift = 0.0;
    if (weighted) {
      if (!raw.contains("weight") || !raw["weight"].is_object()) return;
      const auto w = rawNumNL(raw["weight"], "sum_w");
      if (!w || !(*w > 0)) return;
      sumW = *w;
      if (lm.fitWtype == "aw") {
        llShift = std::numeric_limits<double>::quiet_NaN();
        const auto rows = esampleRowsM4(session, lm);
        const double* wv = f64ColumnM4(session, lm.fitWeight);
        if (rows.has_value() && wv != nullptr && static_cast<double>(rows->size()) == *N) {
          double s = 0.0;
          for (const int64_t i : *rows) s += std::log(wv[i] * *N / sumW);
          if (std::isfinite(s)) llShift = 0.5 * s;
        }
      }
    }
    putEScalarNL(session, "sum_w", sumW);
    if (!std::isnan(llShift)) {
      putEScalarNL(session, "ll", gaussLogLikNL(*N, *rss) + llShift);
      if (!noConstant) putEScalarNL(session, "ll_0", gaussLogLikNL(*N, *mss + *rss) + llShift);
    }
    // regress's own macros; the coefficient-table step posts the hidden
    // e(_r_z__CL)/e(_r_z_abs__CL) when the table is shown (StataBE `ereturn list, all`).
    putEStringNL(session, "cmdline", cmdline);
    putEStringNL(session, "title", "Linear regression");
    putEStringNL(session, "marginsprop", "minus");
    putEStringNL(session, "marginsok", "XB default");
    putEStringNL(session, "properties", "b V");
    putEStringNL(session, "predict", "regres_p");
    putEStringNL(session, "model", "ols");
    putEStringNL(session, "estat_cmd", "regress_estat");
    putEStringNL(session, "_r_z__CL", "t");
    putEStringNL(session, "_r_z_abs__CL", "|t|");
    // A weighted or noconstant fit's e(beta), e(V_modelbased) and ordered name lists
    // are not measured.
    if (weighted || noConstant) return;
    // e(beta) is posted by regress for every fit except a one-way cluster VCE
    // (StataBE `: e(matrices)`: OLS/robust/hc3/aw/fv `b V beta...`, vce(cluster id)
    // `b V V_modelbased`); the multiway path's inner regress runs without the cluster.
    // Both are recorded on the block and resolved when read (recordOnBlockNL);
    // the matrix list below names them only when recorded, and eNameOrderFor
    // re-checks on every read that each one resolves.
    const bool beta = path != Path::kCluster1 && recordOnBlockNL(session, raw, "beta_std");
    // e(V_modelbased) is (X'X)^-1 = V/rmse^2 under every VCE the engine sends it for
    // (BUG-1043: one-way robust/cluster too, unweighted or fweighted).
    const bool vmb = path != Path::kOls && recordOnBlockNL(session, raw, "e_Vmb");
    const std::vector<std::string> base = {"cmdline", "title", "marginsprop", "marginsok",
                                           "vce", "_r_z_abs__CL", "_r_z__CL", "depvar", "cmd",
                                           "properties", "predict", "model", "estat_cmd"};
    const std::vector<std::string> olsScalars = {"N",   "sum_w", "df_m", "df_r", "F",
                                                 "r2",  "rmse",  "mss",  "rss",  "r2_a",
                                                 "ll",  "ll_0",  "rank"};
    std::optional<std::vector<std::string>> S, M, X;
    switch (path) {
      case Path::kOls:
        S = olsScalars;
        M = base;
        if (beta) X = std::vector<std::string>{"b", "V", "beta"};
        break;
      case Path::kRobust:
        // StataBE `: e(matrices)` after vce(robust): `b V beta V_modelbased`.
        S = olsScalars;
        M = base;
        M->push_back("vcetype");
        if (beta && vmb) X = std::vector<std::string>{"b", "V", "beta", "V_modelbased"};
        break;
      case Path::kCluster1: {
        S = std::vector<std::string>{"N_clust"};
        S->insert(S->end(), olsScalars.begin(), olsScalars.end());
        M = base;
        M->push_back("vcetype");
        M->push_back("clustvar");
        if (vmb) X = std::vector<std::string>{"b", "V", "V_modelbased"};  // StataBE `b V V_modelbased`
        break;
      }
      case Path::kMultiway: {
        // Stata's multiway-cluster step: df_r and rank re-set, then
        // the model test's df_m, F, p.
        S = std::vector<std::string>{"N",   "sum_w", "r2",   "rmse", "mss",  "rss", "r2_a",
                                     "ll",  "ll_0",  "df_r", "rank", "df_m", "F",   "p"};
        M = std::vector<std::string>{"_r_z_abs__CL", "_r_z__CL", "clustvar", "vce", "vcetype"};
        for (const auto& c : clusterMacrosNL(raw["kcluster"].size())) M->push_back(c);
        for (const char* n : {"cmdline", "title", "marginsprop", "marginsok", "depvar", "cmd",
                              "properties", "predict", "model", "estat_cmd"})
          M->push_back(n);
        if (beta && vmb)
          X = std::vector<std::string>{"b", "V", "kcluster", "V_modelbased", "beta"};
        break;
      }
    }
    installENameContractNL(session, session.eval.eStrings.get("cmd"), std::move(S), std::move(M), std::move(X));
    return;
  }

  if (stataCmd == "areg") {
    // The MAP path with one absorbed variable and the built-in multiway VCE
    // (Stata's absorb step, areg's ado).
    if (!multiway || lm.fe.size() != 1 || !onlyOptionsNL(parsed, {"absorb", "vce", "cluster", "level"}))
      return;
    const auto tss = rawNumNL(raw, "tss_total");
    const auto tssW = rawNumNL(raw, "tss_within");
    const auto r2a = rawNumNL(raw, "adj_r2");
    if (!tss || !tssW || !r2a || !raw.contains("e_V") || !raw["e_V"].is_object() ||
        !raw["e_V"].contains("names") || !raw["e_V"]["names"].is_array() ||
        !raw.contains("absorbed") || !raw["absorbed"].is_object() ||
        !raw["absorbed"].contains(lm.fe[0]) || !raw["absorbed"][lm.fe[0]].is_number())
      return;
    const double levels = raw["absorbed"][lm.fe[0]].get<double>();
    const double rankMb = static_cast<double>(raw["e_V"]["names"].size());
    putEScalarNL(session, "rc", 0.0);
    // r(dfe), the model-based residual dof (areg charges the absorbed levels).
    putEScalarNL(session, "df_r_mb", *N - (levels - 1.0) - rankMb);
    putEScalarNL(session, "rank_mb", rankMb);
    putEScalarNL(session, "ll", gaussLogLikNL(*N, *rss));
    putEScalarNL(session, "ll_0", gaussLogLikNL(*N, *tssW));
    putEScalarNL(session, "tss", *tss);
    putEScalarNL(session, "adjdf", 0.0);
    putEScalarNL(session, "mss_w", *tssW - *rss);
    putEScalarNL(session, "tss_w", *tssW);
    putEScalarNL(session, "k_absorb", levels);
    putEScalarNL(session, "ar2", *r2a);  // areg's ado historical(9.2) ar2 = e(r2_a)
    std::optional<std::vector<std::string>> M, X;
    // Macros (Stata's absorb step, areg, the estimation-sample signature)
    // and matrices. Only a plain varlist (no factor or
    // time-series operators, which unopvarlist would rewrite) is claimed.
    const auto plain = [](const std::string& v) {
      return !v.empty() && v.find_first_of(".#()") == std::string::npos;
    };
    bool plainVars = plain(lm.y);
    for (const auto& x : lm.x) plainVars = plainVars && plain(x);
    const std::string absvar = lm.fe[0];
    const std::string clustvar = session.eval.eStrings.get("clustvar");
    std::vector<std::string> sigVars;
    if (plainVars && !clustvar.empty()) {
      sigVars.push_back(lm.y);
      for (const auto& x : lm.x) sigVars.push_back(x);
      sigVars.push_back(absvar);
      std::istringstream cs(clustvar);
      for (std::string c; cs >> c;) sigVars.push_back(c);
    }
    std::string signature;
    if (!sigVars.empty() && lm.esample == LastModel::EsampleState::kColumn &&
        session.currentHbin().has_value()) {
      // e(datasignature): `_datasignature <vars>, esample nonames` over e(sample).
      // Stata's checksum algorithm is unpublished, so this is the engine's own
      // signature in Stata's nonames shape (src/data/datasignature.h Q9 ruling,
      // the same divergence `datasignature` itself carries).
      std::string csv;
      for (const auto& v : sigVars) csv += (csv.empty() ? "" : ",") + v;
      CobraRunResult cr = runCobra({"datasignature", "--data", *session.currentHbin(), "--vars",
                                    csv, "--keepif",
                                    std::string(kEsampleColumn) + "==" +
                                        std::to_string(lm.esampleSerial)});
      if (cr.ok) {
        signature = cr.stdout_text;
        while (!signature.empty() && (signature.back() == '\n' || signature.back() == '\r' ||
                                      signature.back() == ' '))
          signature.pop_back();
      }
    }
    if (!signature.empty()) {
      std::string sv;
      for (const auto& v : sigVars) sv += (sv.empty() ? "" : " ") + v;
      putEStringNL(session, "cmdline", cmdline);
      putEStringNL(session, "_r_z__CL", "t");
      putEStringNL(session, "_r_z_abs__CL", "|t|");
      putEStringNL(session, "estmethod", "MAP");
      putEStringNL(session, "footnote", "areg_footnote");
      putEStringNL(session, "title2", "Absorbed variable: " + absvar);
      putEStringNL(session, "title", "Linear regression, absorbing indicators");
      putEStringNL(session, "predict", "areg_p");
      putEStringNL(session, "marginsprop", "nolinearize");
      putEStringNL(session, "marginsnotok", "Residuals SCore D XBD");
      putEStringNL(session, "datasignaturevars", sv);
      putEStringNL(session, "datasignature", signature);
      putEStringNL(session, "absvar", absvar);
      putEStringNL(session, "apm", "projection");
      putEStringNL(session, "map_method", "projection");
      putEStringNL(session, "properties", "b V");
      M = std::vector<std::string>{"cmdline", "_r_z_abs__CL", "_r_z__CL", "estmethod", "cmd",
                                   "footnote", "title2", "title", "predict", "marginsprop",
                                   "marginsnotok", "datasignaturevars", "datasignature", "vce"};
      for (const auto& c : clusterMacrosNL(raw["kcluster"].size())) M->push_back(c);
      for (const char* n : {"absvar", "depvar", "vcetype", "clustvar", "apm", "map_method",
                            "properties"})
        M->push_back(n);
    }
    // e(ksingle) counts the absorbed singletons; claimed only when there are none.
    const bool noSingletons = raw.contains("fe_group_sizes") && raw["fe_group_sizes"].is_object() &&
                              raw["fe_group_sizes"].contains("min") &&
                              raw["fe_group_sizes"]["min"].is_number() &&
                              raw["fe_group_sizes"]["min"].get<double>() > 1.0;
    const bool vmb = recordOnBlockNL(session, raw, "e_Vmb");  // resolved when read
    if (noSingletons && vmb) {
      const auto one = [&absvar](const char* row, double v) {
        eval::EvalMatrix m;
        m.rownames = {row};
        m.colnames = {absvar};
        m.data = {{v}};
        return m;
      };
      putEMatrixNL(session, "ksingle", one("singletons", 0.0));
      putEMatrixNL(session, "dfabsorb", one("df", levels - 1.0));
      putEMatrixNL(session, "kabsorb", one("absvar", levels));
      X = std::vector<std::string>{"b",       "V",       "kcluster",    "ksingle",
                                   "dfabsorb", "kabsorb", "V_modelbased"};
    }
    installENameContractNL(
        session, session.eval.eStrings.get("cmd"),
        std::vector<std::string>{"rc",   "N",      "df_a",     "df_r", "df_r_mb", "rank",
                                 "rank_mb", "ll",  "ll_0",     "rss",  "tss",     "adjdf",
                                 "rmse", "mss",    "r2_w",     "r2",   "mss_w",   "tss_w",
                                 "k_absorb", "df_m", "ar2",    "p",    "F",       "r2_a"},
        std::move(M), std::move(X));
  }
}

// xtreg, fe under the built-in multiway VCE, unweighted (Stata's absorb step with
// cmd xtreg,fe). Called after postBuiltinMultiwayEM4.
void postENameListsXtregFeM4(SessionState& session, const ojson& raw,
                             const eval::ParsedCommand& parsed) {
  session.eNameContract.reset();
  const auto& lmo = session.lastModel();
  if (!lmo.has_value() || !lmo->fitWeight.empty() || !raw.is_object() ||
      !onlyOptionsNL(parsed, {"fe", "vce", "cluster", "level"}))
    return;
  const auto N = rawNumNL(raw, "n_obs");
  const auto rss = rawNumNL(raw, "rss");
  const auto tss = rawNumNL(raw, "tss_total");
  const auto tssW = rawNumNL(raw, "tss_within");
  const auto su = rawNumNL(raw, "sigma_u");
  const auto se = rawNumNL(raw, "sigma_e");
  const auto ng = rawNumNL(raw, "N_g");
  if (!N || !rss || !tss || !tssW || !su || !se || !ng || !raw.contains("fe_group_sizes") ||
      !raw["fe_group_sizes"].is_object() || !raw.contains("e_V") || !raw["e_V"].is_object() ||
      !raw["e_V"].contains("names") || !raw["e_V"]["names"].is_array() ||
      !raw.contains("kcluster") || !raw["kcluster"].is_array() || raw["kcluster"].empty())
    return;
  const auto& gs = raw["fe_group_sizes"];
  if (!gs.contains("min") || !gs.contains("max") || !gs.contains("avg")) return;
  const double gmin = gs["min"].get<double>(), gmax = gs["max"].get<double>(),
               gavg = gs["avg"].get<double>();
  const double rankMb = static_cast<double>(raw["e_V"]["names"].size());
  putEScalarNL(session, "rc", 0.0);
  putEScalarNL(session, "df_ax", 0.0);  // Stata's absorb step (xtreg,fe)
  // r(dfe): the panels nest in every cluster (xtreg requires it, _xtreg_chk_cl2), so
  // the absorbed panel means are not charged (StataBE e(df_r_mb) 177 = 180 - 3).
  putEScalarNL(session, "df_r_mb", *N - rankMb);
  putEScalarNL(session, "rank_mb", rankMb);
  putEScalarNL(session, "ll", gaussLogLikNL(*N, *rss));
  putEScalarNL(session, "ll_0", gaussLogLikNL(*N, *tssW));
  putEScalarNL(session, "tss", *tss);
  putEScalarNL(session, "adjdf", 0.0);
  putEScalarNL(session, "k_absorb", *ng);
  // e(df_b): the dof of the model test of the slopes
  // (Stata's absorb step), the reply's df_b (runXtreg: the slope count,
  // bounded by the clipped V's rank).
  putEScalarNL(session, "df_b", rawNumNL(raw, "df_b").value_or(rankMb - 1.0));
  putEScalarNL(session, "sigma", std::sqrt(*su * *su + *se * *se));  // xtreg,fe
  putEScalarNL(session, "ui", *su);
  putEScalarNL(session, "g_max", gmax);
  putEScalarNL(session, "g_avg", gavg);
  putEScalarNL(session, "g_min", gmin);
  putEScalarNL(session, "Tcon", gmin == gmax ? 1.0 : 0.0);
  putEScalarNL(session, "Tbar", gavg);
  putEScalarNL(session, "tss_w", *tssW);
  putEScalarNL(session, "mss_w", *tssW - *rss);
  putEScalarNL(session, "tss_xt", *tssW);
  installENameContractNL(
      session, "xtreg",
      std::vector<std::string>{"rc",      "N",     "df_a",  "df_ax",  "df_r",  "df_r_mb", "rank",
                               "rank_mb", "ll",    "ll_0",  "rss",    "tss",   "adjdf",   "rmse",
                               "mss",     "r2",    "k_absorb", "df_b", "df_m", "r2_w",    "p",
                               "rho",     "sigma", "sigma_e", "r2_b", "r2_o",  "corr",    "sigma_u",
                               "ui",      "N_g",   "g_max", "g_avg",  "g_min", "Tcon",    "Tbar",
                               "F",       "r2_a",  "tss_w", "mss_w",  "tss_xt"},
      std::nullopt, std::nullopt);
}

// ivregress 2sls under the built-in multiway VCE, asymptotic (no `small`),
// unweighted, with the constant (ivregress.ado / ivregress 2sls). Called after
// runIvRegress has posted e(mss)/e(tss)/e(rss)/e(r2)/e(r2_a)/e(df_m).
void postENameListsIvregressM4(SessionState& session, const ojson& raw, size_t kEndog) {
  session.eNameContract.reset();
  if (!session.lastModel().has_value() || !raw.is_object()) return;
  const auto p = rawNumNL(raw, "chi2_p");
  if (!p || kEndog == 0) return;
  for (const char* n : {"mss", "tss", "rss", "r2", "r2_a", "df_m"})
    if (session.eval.eScalars.find(n) == nullptr) return;
  putEScalarNL(session, "kappa", 1.0);
  putEScalarNL(session, "p", *p);
  putEScalarNL(session, "k_endog", static_cast<double>(kEndog));
  putEScalarNL(session, "iterations", 0.0);
  installENameContractNL(
      session, "ivregress",
      std::vector<std::string>{"N",    "kappa",   "mss",  "tss",  "rss",  "rmse",
                               "r2",   "r2_a",    "df_m", "p",    "rank_mb", "rank",
                               "chi2", "df_r",    "k_endog", "iterations"},
      std::nullopt, std::nullopt, {"rank_mb", "df_r"});
}

// ===========================================================================
// tabstat / correlate / pwcorr / ttest — all route through cobraToEstimationM4
// (dispatcher.py's "4b. Cobra estimator" argv-Dispatch path; no native
// handler in the oracle, just an argv builder + the standard estimation
// wrapper).

// W:P5: tabstat's body, split at the row-filter boundary. runTabstat (below)
// does the filtering and calls this; the `by G: tabstat` driver in
// dispatcher.cpp has ALREADY materialized its group slice — with the
// sub-command's own `if` folded in — and calls runTabstatOnData, which lands
// here without repeating the filter. This split is what lets the by-prefix
// driver REUSE tabstat rather than grow a second copy of it (the P4
// `by: regress` precedent).
namespace {
ojson runTabstatCore(const std::vector<std::string>& varlist,
                      const std::optional<std::string>& byVal,
                      const std::optional<std::string>& statsVal, const std::string& dataPath,
                      bool wantGroups, bool noTotal, const std::string& weightFlag = "",
                      const std::string& weightVar = "") {
  const std::string* dataPathOpt = &dataPath;
  std::vector<std::string> args = {"tabstat", "--vars", joinCommaM4(varlist)};
  if (byVal.has_value() && !byVal->empty()) {
    args.push_back("--by");
    args.push_back(*byVal);
  }
  // BUG-1014: [aw=]/[fw=] reach the engine, whose tabstat is summarize per group
  // with the same weight conventions (w2as sx_tabstat_aw, sx_tabstat_fw).
  if (!weightFlag.empty() && !weightVar.empty()) {
    args.push_back(weightFlag);
    args.push_back(weightVar);
  }
  if (statsVal.has_value() && !statsVal->empty()) {
    // space/comma -> comma-joined.
    std::string joined;
    std::string cur;
    for (char c : *statsVal) {
      if (c == ' ' || c == ',') {
        if (!cur.empty()) {
          if (!joined.empty()) joined += ",";
          joined += cur;
          cur.clear();
        }
      } else {
        cur += c;
      }
    }
    if (!cur.empty()) {
      if (!joined.empty()) joined += ",";
      joined += cur;
    }
    args.push_back("--stats");
    args.push_back(joined);
  } else {
    args.push_back("--stats");
    args.push_back("mean,sd,min,max,n");
  }
  args.push_back("--data");
  args.push_back(*dataPathOpt);
  args.push_back("--tidy");

  ojson raw;
  try {
    raw = runCobraJsonM4(args);
  } catch (const CobraError& ce) {
    return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
  }
  ojson block = cobraToEstimationM4(raw, "tabstat");
  // ------------------------------------------------------------------
  // B4-W3b. The engine has ALWAYS emitted a `groups` array under `--by`
  // (verified on this build: `cobra tabstat --by year` returns seven group
  // objects), and cobraToEstimationM4 has never carried it — so `tabstat y x1,
  // by(region)` rendered the TOTAL row only. That gap is NOT closed here, and
  // deliberately: four freeze fixtures (tabstat/corr/pwcorr/levelsof.json)
  // byte-pin the group-less block for exactly that command, and re-pinning
  // them is a main-checkout edit outside this branch's manifest.
  //
  // What IS closed is the half `save`/`nototal` reach: both spellings answered
  // r(198) until this delta, so no captured fixture can contain either, and
  // carrying `groups` only when one of them is present cannot move a pin.
  if (wantGroups && raw.contains("groups") && raw["groups"].is_array()) {
    block["groups"] = raw["groups"];
  }
  if (noTotal) block.erase("total");
  return block;
}
}  // namespace

ojson runTabstat(const std::vector<std::string>& varlist, const std::optional<std::string>& byVal,
                  const std::optional<std::string>& statsVal, SessionState& session,
                  const std::optional<std::string>& ifExpr, bool wantGroups, bool noTotal) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  if (varlist.empty()) return engineErrorBlockM4("tabstat requires at least one variable");

  ojson ifErr;
  auto dataPathOpt = applyIfFilterM4(session, *session.currentHbin(), ifExpr, ifErr);
  if (!dataPathOpt.has_value()) return ifErr;
  return runTabstatCore(varlist, byVal, statsVal, *dataPathOpt, wantGroups, noTotal);
}

// BUG-1014: the weighted form of runTabstat. Not in piet/handlers.h (outside
// this lane's manifest); forward-declared in dispatcher.cpp like
// runSummarizeAweight. `weightFlag` is "--aweight" or "--fweight".
ojson runTabstatWeighted(const std::vector<std::string>& varlist,
                         const std::optional<std::string>& byVal,
                         const std::optional<std::string>& statsVal, SessionState& session,
                         const std::optional<std::string>& ifExpr, bool wantGroups, bool noTotal,
                         const std::string& weightFlag, const std::string& weightVar) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  if (varlist.empty()) return engineErrorBlockM4("tabstat requires at least one variable");
  ojson ifErr;
  auto dataPathOpt = applyIfFilterM4(session, *session.currentHbin(), ifExpr, ifErr);
  if (!dataPathOpt.has_value()) return ifErr;
  return runTabstatCore(varlist, byVal, statsVal, *dataPathOpt, wantGroups, noTotal, weightFlag,
                        weightVar);
}

ojson runTabstatOnData(const std::vector<std::string>& varlist,
                        const std::optional<std::string>& statsVal, SessionState& session,
                        const std::string& dataPath) {
  (void)session;  // slice already materialized by the by-prefix driver
  if (varlist.empty()) return engineErrorBlockM4("tabstat requires at least one variable");
  return runTabstatCore(varlist, std::nullopt, statsVal, dataPath, /*wantGroups=*/false,
                        /*noTotal=*/false);
}

// BUG-1520: `by g: tabstat x [aw=w]` / `[fw=w]` -- one group's slice with the
// weight. Forward-declared in dispatcher.cpp like runTabstatWeighted.
ojson runTabstatOnDataWeighted(const std::vector<std::string>& varlist,
                               const std::optional<std::string>& statsVal,
                               const std::string& dataPath, const std::string& weightFlag,
                               const std::string& weightVar) {
  if (varlist.empty()) return engineErrorBlockM4("tabstat requires at least one variable");
  return runTabstatCore(varlist, std::nullopt, statsVal, dataPath, /*wantGroups=*/false,
                        /*noTotal=*/false, weightFlag, weightVar);
}

// BUG-1014: correlate / pwcorr as StataBE 19.5 posts them (w2as_summary_stats_family
// sx_corr_*, sx_correlate_*, sx_pwcorr_*; w2o_rnames N18 N19). Not in
// piet/handlers.h (outside this lane's manifest); forward-declared in
// dispatcher.cpp. `weightFlag` is "--aweight", "--fweight" or "".
//  correlate: r(N) (Σw under fw), r(rho) = r(C)[1,2] (0 for one variable; not
//    posted with covariance), r(C) the correlation -- or covariance -- matrix,
//    r(cov_12) r(Var_1) r(Var_2) with covariance, r(sum_w) with aweights;
//  pwcorr (pwcorr.ado): r(N) = r(Nobs)[1,2], r(rho) = r(C)[1,2], r(C), r(Nobs),
//    and r(sig) with sig.
// A string variable is dropped from the varlist first ("(s ignored because
// string variable)", sx_corr_str); fewer than two left is r(102) for pwcorr.
ojson runCorrStata(const std::vector<std::string>& varlistIn, bool pairwise, bool sig, bool covar,
                   const std::string& weightFlag, const std::string& weightVar,
                   SessionState& session, const std::optional<std::string>& ifExpr,
                   const std::optional<std::string>& inRange) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  std::vector<std::string> varlist;
  if (Datastore* ds = session.datastore()) {
    for (const auto& v : varlistIn)
      if (!(ds->has(v) && ds->dtype(v) == "factor")) varlist.push_back(v);
  } else {
    varlist = varlistIn;
  }
  if (varlist.empty() || (pairwise && varlist.size() < 2)) {
    return ojson(eval::errorBlock("syntax_error", "too few variables specified", std::nullopt,
                                  std::nullopt, std::nullopt, std::string("r(102)")));
  }
  ojson ifErr;
  auto dataPathOpt = applyIfFilterM4(session, *session.currentHbin(), ifExpr, ifErr, inRange);
  if (!dataPathOpt.has_value()) return ifErr;

  std::vector<std::string> args = {"corr", "--vars", joinCommaM4(varlist)};
  if (pairwise) args.push_back("--pairwise");
  if (sig) args.push_back("--sig");
  if (covar) args.push_back("--cov");
  if (!weightFlag.empty() && !weightVar.empty()) {
    args.push_back(weightFlag);
    args.push_back(weightVar);
  }
  args.push_back("--data");
  args.push_back(*dataPathOpt);
  args.push_back("--tidy");

  ojson raw;
  try {
    raw = runCobraJsonM4(args);
  } catch (const CobraError& ce) {
    return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
  }
  // correlate reads only the rows with every variable present: none is r(2000)
  // "no observations", one r(2001) "insufficient observations", weighted or not
  // and with covariance too (StataBE 19.5, 2026-11-03: `correlate x1 x2 if x1 >
  // 100`, `... in 1`, `... [fw=w] in 1` with w = 5). r() then holds r(N) = 0 and,
  // after an earlier correlate, that one's r(C) -- even past `return clear`.
  // pwcorr is left as it was (`pwcorr x1 x2 in 1` is rc 0 in Stata).
  // The count is of rows: under [fw=] the engine's n_obs is the weight sum, so
  // the usable rows are counted here (a present, positive weight, as the engine
  // reads them).
  std::optional<double> usableRows;
  if (!pairwise && raw.contains("n_obs") && raw["n_obs"].is_number())
    usableRows = raw["n_obs"].get<double>();
  if (usableRows.has_value() && weightFlag == "--fweight" && !weightVar.empty()) {
    try {
      Datastore wds(*dataPathOpt);
      std::vector<const double*> cols;
      for (const auto& v : varlist) cols.push_back(wds.col(v).f64());
      const double* w = wds.col(weightVar).f64();
      int64_t rows = 0;
      for (int64_t r = 0; r < wds.nRows() && rows < 2; ++r) {
        bool ok = !std::isnan(w[r]) && w[r] > 0;
        for (const double* c : cols) ok = ok && !std::isnan(c[r]);
        if (ok) ++rows;
      }
      usableRows = static_cast<double>(rows);
    } catch (const std::exception&) {
      usableRows.reset();
    }
  }
  if (usableRows.has_value() && *usableRows <= 1.0) {
    const bool none = *usableRows <= 0.0;
    eval::replaceRClass(session.eval);
    std::vector<std::string> posted = {"N"};
    session.eval.rValues.set("N", 0.0);
    if (session.lastCorrelateC.has_value()) {
      eval::EvalMatrix c = *session.lastCorrelateC;
      c.gen = eval::nextStoreGeneration();  // a write of this command's
      session.eval.rMatrices.emplace_back("C", std::move(c));
      posted.push_back("C");
    }
    eval::declareRPosted(session.eval, "correlate", std::move(posted));
    return ojson(eval::errorBlock("engine_error",
                                  none ? "no observations" : "insufficient observations",
                                  std::nullopt, std::nullopt, std::nullopt,
                                  std::string(none ? "r(2000)" : "r(2001)")));
  }
  ojson corrBlock = cobraToEstimationM4(raw, "corr");
  auto cell = [](const ojson& m, size_t i, size_t j) {
    if (m.is_array() && i < m.size() && m[i].is_array() && j < m[i].size() && m[i][j].is_number())
      return m[i][j].get<double>();
    return eval::kStataMissingSentinel;
  };
  auto toMatrix = [&](const ojson& m) {
    eval::EvalMatrix out;
    out.rownames = varlist;
    out.colnames = varlist;
    for (size_t i = 0; i < varlist.size(); ++i) {
      std::vector<double> row;
      for (size_t j = 0; j < varlist.size(); ++j) {
        const double v = cell(m, i, j);
        row.push_back(v >= eval::kStataMissingSentinel ? std::numeric_limits<double>::quiet_NaN() : v);
      }
      out.data.push_back(std::move(row));
    }
    return out;
  };
  // r() is this command's from here on (an r-class command replaces all of it).
  eval::replaceRClass(session.eval);
  std::vector<std::string> posted;
  auto post = [&](const char* name, double v) {
    session.eval.rValues.set(name, v);
    posted.push_back(name);
  };
  auto postMatrix = [&](const char* name, const ojson& m) {
    session.eval.rMatrices.emplace_back(name, toMatrix(m));
    posted.push_back(name);
  };
  const ojson& R = raw.contains("r") ? raw["r"] : ojson();
  if (pairwise) {
    const ojson& NB = raw.contains("nobs") ? raw["nobs"] : ojson();
    post("N", cell(NB, 0, 1));
    post("rho", cell(R, 0, 1));
    postMatrix("C", R);
    postMatrix("Nobs", NB);
    if (sig && raw.contains("p")) postMatrix("sig", raw["p"]);
  } else {
    if (raw.contains("n_obs") && raw["n_obs"].is_number()) post("N", raw["n_obs"].get<double>());
    if (covar) {
      const ojson& CV = raw.contains("cov") ? raw["cov"] : ojson();
      postMatrix("C", CV);
      post("Var_1", cell(CV, 0, 0));
      if (varlist.size() >= 2) {
        post("cov_12", cell(CV, 0, 1));
        post("Var_2", cell(CV, 1, 1));
      }
    } else {
      postMatrix("C", R);
      post("rho", varlist.size() >= 2 ? cell(R, 0, 1) : 0.0);
    }
    session.lastCorrelateC = session.eval.rMatrices.back().second;
    if (weightFlag == "--aweight" && raw.contains("sum_w") && raw["sum_w"].is_number())
      post("sum_w", raw["sum_w"].get<double>());
  }
  eval::declareRPosted(session.eval, pairwise ? "pwcorr" : "correlate", std::move(posted));
  return corrBlock;
}

ojson runCorr(const std::vector<std::string>& varlist, bool pairwise, bool sig,
              SessionState& session, const std::optional<std::string>& ifExpr,
              const std::optional<std::string>& inRange) {
  return runCorrStata(varlist, pairwise, sig, /*covar=*/false, "", "", session, ifExpr, inRange);
}

ojson runTtest(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  const std::vector<std::string>& varlist = parsed.varlist;
  const std::string& raw = parsed.raw;
  if (varlist.empty()) {
    return engineErrorBlockM4("syntax_error: ttest requires a variable", "syntax_error");
  }

  std::vector<std::string> levelArgs;
  bool levelExplicit = false;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "level" && !v.isBool) {
      levelExplicit = true;
      try {
        int lv = std::stoi(v.strValue);
        levelArgs = {"--level", std::to_string(lv)};
      } catch (...) {
      }
    }
  }
  // BS-048 (BETA A4): no explicit level() — fall back to the session's `set
  // level`/`set clevel` default. ttest is the ONE PIET-wired command whose
  // cobra CLI genuinely reads --level (cmd_ttest, apps/cmds/cmds_summary.cpp
  // — applies it to alpha/t_critical throughout); session.level==95 default
  // keeps this a no-op until `set level` actually runs. (buildEstimationArgvM4
  // / handlers.cpp::runRegress deliberately do NOT carry this same fallback —
  // see their own comments: cobra's feols/xtreg/logit/probit CLIs never
  // consume --level at all, so forwarding it there would trip cobra's E6
  // unconsumed-flag net instead of doing anything useful.)
  if (!levelExplicit && session.level != 95) {
    levelArgs = {"--level", std::to_string(session.level)};
  }

  std::optional<std::string> byVal;
  bool unequal = false;
  bool paired = false;
  bool reverse = false;
  bool unpaired = false;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "by" && !v.isBool) byVal = v.strValue;
    if ((k == "unequal" || k == "welch") && v.isBool && v.boolValue) unequal = true;
    if (k == "paired" && v.isBool && v.boolValue) paired = true;
    if (k == "reverse" && v.isBool && v.boolValue) reverse = true;
    if (k == "unpaired" && v.isBool && v.boolValue) unpaired = true;
  }

  // BS-059/BS-089: level/by/unequal/welch/paired/reverse/unpaired are the
  // only ttest options this handler reads — a leftover key reads as an
  // honest reject.
  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kTtestKnownOpts = kVocabTtest.known;
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kTtestKnownOpts)) {
    return *err;
  }

  // A string variable has no observations: _ttest's summarize finds none,
  // r(2000) (w2as sx_ttest_ref_str; it was a factor_misuse engine error, rc 1).
  if (Datastore* tds = session.datastore()) {
    if (tds->has(varlist[0]) && tds->dtype(varlist[0]) == "factor")
      return ojson(eval::errorBlock("engine_error", "no observations", std::nullopt, std::nullopt,
                                    std::nullopt, std::string("r(2000)")));
  }
  // `ttest x = y` is `ttest x == y` (ttest.ado turns "==" into "=" before
  // syntax). The right-hand side is read from the varlist tokens, never from the
  // raw text: a regex over the raw line found the `==` of an `if` clause, so
  // `ttest pre = post if d == 1` ran a one-sample test of pre against 1 and
  // posted its names (sx_ttest_paired_eq).
  std::optional<std::string> eqRhs;
  {
    std::string joined;
    for (const auto& t : varlist) joined += t + " ";
    const size_t eq = joined.find('=');
    if (eq != std::string::npos) {
      std::string r = joined.substr(eq + 1);
      while (!r.empty() && (r.front() == '=' || std::isspace(static_cast<unsigned char>(r.front()))))
        r.erase(r.begin());
      eqRhs = trimM4(r);
    }
  }
  std::vector<std::string> args;
  if (byVal.has_value() && !byVal->empty()) {
    args = {"ttest", "--var", varlist[0], "--by", *byVal};
    if (unequal) args.push_back("--unequal");
    if (reverse) args.push_back("--reverse");
    for (const auto& a : levelArgs) args.push_back(a);
  } else {
    if (eqRhs.has_value() && !eqRhs->empty()) {
      std::string rhs = *eqRhs;
      try {
        size_t pos;
        std::stod(rhs, &pos);  // Python's float(rhs) — value unused, only validity matters
        if (pos == rhs.size()) {
          args = {"ttest", "--var", varlist[0], "--against", rhs};
          if (paired) args.push_back("--paired");
          for (const auto& a : levelArgs) args.push_back(a);
        } else {
          throw std::invalid_argument("not fully numeric");
        }
      } catch (...) {
        // rhs is a variable name -> paired/two-sample.
        args = {"ttest", "--vars", varlist[0] + "," + rhs};
        if (paired) args.push_back("--paired");
        if (unpaired) args.push_back("--unpaired");
        if (unpaired && unequal) args.push_back("--unequal");
        if (reverse) args.push_back("--reverse");
        for (const auto& a : levelArgs) args.push_back(a);
      }
    } else if (varlist.size() >= 2) {
      args = {"ttest", "--vars", varlist[0] + "," + varlist[1]};
      if (paired) args.push_back("--paired");
      if (unpaired) args.push_back("--unpaired");
      if (unpaired && unequal) args.push_back("--unequal");
      if (reverse) args.push_back("--reverse");
      for (const auto& a : levelArgs) args.push_back(a);
    } else {
      args = {"ttest", "--var", varlist[0], "--against", "0"};
      for (const auto& a : levelArgs) args.push_back(a);
    }
  }

  // BUG-042: `ttest ... if COND` — ttest's own cobra CLI has no --keepif;
  // pre-filter via the shared row-filter infra (same helper tabstat/corr use
  // above). `parsed.ifExpr`/`parsed.inRange` are already available on this
  // handler's own argument (no signature change needed, unlike tabstat/corr).
  // BS-090 (BETA Phase G): `in` now composes with `if` via AND here too.
  ojson ifErr;
  auto dataPathOpt =
      applyIfFilterM4(session, *session.currentHbin(), parsed.ifExpr, ifErr, parsed.inRange);
  if (!dataPathOpt.has_value()) return ifErr;

  args.push_back("--data");
  args.push_back(*dataPathOpt);
  args.push_back("--tidy");

  ojson raw2;
  try {
    raw2 = runCobraJsonM4(args);
  } catch (const CobraError& ce) {
    // by() must name exactly two groups: Stata's r(420) (sx_ttest_ref_by3). The
    // engine's two_groups() says how many it found.
    const std::string msg = cobraErrorStrM4(ce);
    static const std::regex kGroups(R"(must define exactly 2 groups \(found (\d+)\))");
    std::smatch gm;
    if (std::regex_search(msg, gm, kGroups)) {
      const int found = std::stoi(gm[1].str());
      return ojson(eval::errorBlock(
          "engine_error",
          found > 2 ? "more than 2 groups found, only 2 allowed"
                    : std::to_string(found) + " group" + (found == 1 ? "" : "s") + " found, 2 required",
          std::nullopt, std::nullopt, std::nullopt, std::string("r(420)")));
    }
    if (msg.find("no observations") != std::string::npos)
      return ojson(eval::errorBlock("engine_error", "no observations", std::nullopt, std::nullopt,
                                    std::nullopt, std::string("r(2000)")));
    return engineErrorBlockM4(msg, ce.code);
  }
  ojson ttBlock = cobraToEstimationM4(raw2, "ttest");
  // BUG-165 (W:P5): the per-group dispersions Stata prints in the group rows
  // ("Std. dev."). Forwarded HERE rather than from the shared passthrough
  // list, because `sdtest` emits the same two key names and already forwards
  // them from its own field list — adding them to the shared list moved them
  // earlier in sdtest's key order, a byte change to an output this fix has no
  // business touching.
  for (const char* k : {"sd_1", "sd_2"}) {
    if (raw2.contains(k)) ttBlock[k] = raw2[k];
  }
  // BUG-1015: r() is the engine's `r` object -- every name Stata posts for the
  // form that ran (one-sample 12, two-sample 28, paired 23; ttest.sthlp Stored
  // results) and no other: r(sd) after a one-sample test reads `.` (FU-269,
  // w2o_rnames N07), the two-sample and paired names are posted (FU-270, N09 N10).
  eval::replaceRClass(session.eval);
  std::vector<std::string> posted;
  if (raw2.contains("r") && raw2["r"].is_object()) {
    for (auto it = raw2["r"].begin(); it != raw2["r"].end(); ++it) {
      if (!it.value().is_number()) continue;
      session.eval.rValues.set(it.key(), it.value().get<double>());
      posted.push_back(it.key());
    }
  }
  eval::declareRPosted(session.eval, "ttest", std::move(posted));
  ttBlock.erase("r");  // r() carriage, not part of the displayed block
  return ttBlock;
}

namespace {

// dispatcher.py's "Cache last_model for test/margins/predict/lincom/
// coefplot/eststo" block — extracts --y/--x/--fe/--vcov/--cluster from the
// argv actually sent to cobra (post TS-injection) so post-estimation
// commands reconstruct the SAME call. Mirrors handlers.cpp::runRegress's
// identical inline logic (M2), duplicated here per this file's
// established one-copy-per-milestone-file convention.
//
// e(sample) T2: `esampleValues` is the fit's `--esample-out` file (esampleFitArgs,
// NOT in `argv`) for a route wired for the mask; empty (every other caller)
// leaves the model's e(sample) a named r(198) and the call byte-identical.
//
// `stataCmd`: the Stata command word e(cmd) publishes (LastModel::stataCmd), or
// empty to publish cobraCmd as before.
void cacheLastModelM4(SessionState& session, const std::string& cobraCmd,
                       const std::vector<std::string>& argv, const ojson& block, const ojson& raw,
                       const std::string& esampleValues = std::string(),
                       const std::string& stataCmd = std::string()) {
  LastModel lm;
  lm.cobraCmd = cobraCmd;
  lm.stataCmd = stataCmd;
  for (size_t i = 0; i < argv.size(); ++i) {
    if (argv[i] == "--y" && i + 1 < argv.size()) lm.y = argv[i + 1];
    if (argv[i] == "--x" && i + 1 < argv.size()) {
      std::string joined = argv[i + 1];
      std::string cur;
      for (char c : joined) {
        if (c == ',') {
          if (!cur.empty()) lm.x.push_back(cur);
          cur.clear();
        } else {
          cur += c;
        }
      }
      if (!cur.empty()) lm.x.push_back(cur);
    }
    if (argv[i] == "--fe" && i + 1 < argv.size()) {
      std::string joined = argv[i + 1];
      std::string cur;
      for (char c : joined) {
        if (c == ',') {
          if (!cur.empty()) lm.fe.push_back(cur);
          cur.clear();
        } else {
          cur += c;
        }
      }
      if (!cur.empty()) lm.fe.push_back(cur);
    }
    if (argv[i] == "--vcov" && i + 1 < argv.size()) lm.vcov = argv[i + 1];
    if (argv[i] == "--keep-singletons") lm.keepSingletons = true;  // e(sample) T2
    if (argv[i] == "--collinear-forward") lm.collinearForward = true;  // BUG-1067
    // W:P5: logit/probit spell the same selector `--vce` (buildEstimationArgvM4
    // remaps `--vcov hc1|cluster` onto it), so scanning only `--vcov` left
    // lm.vcov at its "iid" default for exactly the models that DID have a
    // robust/cluster VCE — and every post-estimation command then replayed the
    // fit as iid. Normalize back to the linear spelling so this one field
    // means one thing no matter which CLI produced it.
    if (argv[i] == "--vce" && i + 1 < argv.size()) {
      lm.vcov = (argv[i + 1] == "robust") ? std::string("hc1") : argv[i + 1];
    }
    if (argv[i] == "--cluster" && i + 1 < argv.size()) lm.cluster.push_back(argv[i + 1]);
    // runSvyPrefix: a `svy:` fit's e(vce) is linearized (StataBE 19.5, BUG-1513);
    // lm.vcov keeps the engine spelling the replays key on.
    if (argv[i] == "--svy") lm.eVce = "linearized";
  }
  lm.depvar = block.value("depvar", "");
  captureFitFilterM4(lm, argv);
  lm.dataPath = session.currentHbin().value_or("");
  lm.result = raw;
  // BUG-570, the twin of runRegress's rule: e(N_clust) exists only after a
  // clustered fit, but the engine's feols reports n_clusters 0 for every other
  // VCE, so `xtreg ..., fe` (routed here as "feols") printed e(N_clust) 0 where
  // Stata prints `.` (w2_pweight.log X0_fe PSTR `120|103|.|conventional|||||`,
  // X3_aw_fe alike). logit/probit and xtreg,re were already `.` -- their engines
  // omit the key when unclustered (measured on main 2026-09-28) -- so this is a
  // no-op for them. Dropped from the cached reply only; the emitted block is
  // unchanged.
  if (lm.result.has_value() && lm.result->is_object() && lm.result->contains("n_clusters") &&
      (*lm.result)["n_clusters"].is_number() &&
      (*lm.result)["n_clusters"].get<double>() == 0.0) {
    lm.result->erase("n_clusters");
  }
  // BUG-1041: xtreg, fe posts e(tss), the total sum of squares about the overall
  // mean (StataBE 9544.62 where its within e(mss) is 8389.45). Cached reply only.
  if (lm.stataCmd == "xtreg" && lm.result.has_value() && lm.result->is_object() &&
      lm.result->contains("tss_total") && (*lm.result)["tss_total"].is_number())
    (*lm.result)["tss"] = (*lm.result)["tss_total"];
  // BUG-518 order: the mask column first, then setLastModel.
  if (!esampleValues.empty()) (void)esampleInstallOnHead(session, esampleValues, *lm.result, lm);
  session.setLastModel(lm);
  session.lastEstBlock = block;
  session.attachFitCovariance(raw);  // eb-order T-P1: e(V) when the fit sent one
  // A noconstant fit (`--no-intercept`) records it, as runRegress does, so the
  // e(b) readers know the missing `_cons` is Stata's too (w2p_estim_ev.log:1585
  // G_pnc_fit `probit d x1 x2, noconstant`: colsof(e(b)) = 2).
  if (std::find(argv.begin(), argv.end(), "--no-intercept") != argv.end() &&
      session.lastEstBlock.has_value() && session.lastEstBlock->is_object())
    (*session.lastEstBlock)["noconstant"] = true;
}

// FU-29: xtreg,fe's weight rule. After `mark touse [weight] if in` and
// `markout touse ivar tvar varlist` it runs `by ivar touse: assert w == w[_N]
// if touse` and stops with "weight must be constant within <ivar>" r(199)
// (w2_pweight.log X10_pw_varies_within_id; aer_b4 predict-e-xbd.log
// G_w_varies, aweight). The engine fits a varying weight without complaint, so
// the check has to happen here, before the fit.
//
// The rows checked are a SUPERSET of Stata's sample: the `if` filter, a
// non-missing panel id, a non-missing and nonzero weight (`mark` drops zero
// weights), and a non-missing depvar and plain/c./i. regressor. Terms behind
// any other operator (L./D./ib2./...) do not narrow the rows. A superset can
// only refuse where Stata would fit (an honest r(199) on a weight that varies
// only on rows Stata drops); it can never fit where Stata refuses.
// Returns nullopt when the weight passes.
std::optional<ojson> xtregWeightWithinPanelM4(const eval::ParsedCommand& parsed,
                                              SessionState& session, const std::string& ivar) {
  const std::string& wvar = *parsed.weightVar;
  const std::string basePath = *session.currentHbin();
  ojson filterErr;
  auto filteredPath = applyIfFilterM4(session, basePath, parsed.ifExpr, filterErr);
  if (!filteredPath.has_value()) return filterErr;
  const bool madeTemp = (*filteredPath != basePath);
  std::optional<ojson> verdict;
  try {
    std::optional<Datastore> tempDs;
    Datastore* ds = nullptr;
    if (madeTemp) {
      tempDs.emplace(*filteredPath);
      ds = &*tempDs;
    } else {
      ds = session.datastore();
    }
    // A weight that is not a column of the data (an expression such as
    // `[aw=1/x]`) is refused by the engine itself ("column not found"), and a
    // panel variable is always a column, so only the type needs a verdict here.
    if (ds != nullptr && ds->has(wvar) && ds->has(ivar)) {
      if (ds->dtype(wvar) != "f64" || ds->dtype(ivar) != "f64") {
        verdict = engineErrorBlockM4("xtreg: could not verify that the weight is constant within " +
                                     ivar + " (weight and panel variable must be numeric)");
      } else {
        std::vector<const double*> need;
        int64_t n = ds->nRows();
        auto add = [&](const std::string& name) {
          if (!ds->has(name) || ds->dtype(name) != "f64") return;
          const cecon::Column& c = ds->col(name);
          const int64_t rows = c.nbytes / static_cast<int64_t>(sizeof(double));
          if (rows < n) n = rows;
          need.push_back(c.f64());
        };
        add(wvar);
        add(ivar);
        const double* w = need[0];  // both are f64 columns (checked just above)
        const double* g = need[1];
        for (const auto& term : parsed.varlist) {
          size_t start = 0;
          while (start <= term.size()) {
            const size_t hash = term.find('#', start);
            const std::string part =
                term.substr(start, hash == std::string::npos ? std::string::npos : hash - start);
            start = (hash == std::string::npos) ? term.size() + 1 : hash + 1;
            if (part.empty()) continue;
            const size_t dot = part.rfind('.');
            if (dot == std::string::npos) {
              add(part);
            } else {
              const std::string op = lowerM4(part.substr(0, dot));
              if (op == "c" || op == "i") add(part.substr(dot + 1));
            }
          }
        }
        std::map<double, double> first;  // panel id -> its first weight in the sample
        for (int64_t i = 0; i < n && !verdict.has_value(); ++i) {
          bool in = !(w[i] == 0.0);
          for (const double* col : need) {
            if (std::isnan(col[i])) in = false;
          }
          if (!in) continue;
          auto it = first.emplace(g[i], w[i]).first;
          if (it->second != w[i]) {
            verdict = ojson(eval::errorBlock("engine_error", "weight must be constant within " + ivar,
                                             std::nullopt, std::nullopt, std::nullopt, "r(199)"));
          }
        }
      }
    }
  } catch (const std::exception& e) {
    verdict = engineErrorBlockM4(std::string("xtreg: could not read the data to check the weight: ") +
                                 e.what());
  }
  if (madeTemp) cecon::native_unlink(*filteredPath);
  return verdict;
}

// FU-76: the e() spellings of the panel R-squared pair. The engine reports them
// as r2_between/r2_overall (feols single-FE, apps/cmds/common.cpp
// add_panel_scalars; xtreg,re, cmds_panel.cpp), and the e() resolver only knew
// `r2_w`, so e(r2_b)/e(r2_o) read `.` beside a number the reply already held
// (w2_pweight.log X0_fe PXT +1.f03574901785fX-001 / +1.5afec63f08acbX-001; the
// engine's 0.9691578317540571 / 0.6777250244088803). Copied into the cached
// reply only, as N_g/df_a are: the emitted block's shape is freeze-pinned. A
// reply without the source key (weighted fits, see runXtreg) stays `.`.
void aliasPanelR2M4(ojson& raw) {
  if (!raw.is_object()) return;
  if (raw.contains("r2_between") && raw["r2_between"].is_number()) raw["r2_b"] = raw["r2_between"];
  if (raw.contains("r2_overall") && raw["r2_overall"].is_number()) raw["r2_o"] = raw["r2_overall"];
}

// BUG-1482: the rank of a one-way clustered V (e_V, slopes and _cons), Stata's
// r(rankc) (Stata's robust-VCE step: the pivoted QR of the clustered V, tolerance 1e-9):
// the eigenvalues of the symmetric V above 1e-9 of the largest. Under xtreg, fe
// the clusters nest the panels, so the _cons scores sum to zero in every cluster
// and the rank is min(slopes, G - 1) on ordinary data (StataBE 19.5, 2 clusters
// and 2 slopes: e(rank) 1). nullopt when e_V is absent or not square.
std::optional<double> clusteredVRankM4(const ojson& raw) {
  if (!raw.contains("e_V") || !raw["e_V"].is_object() || !raw["e_V"].contains("matrix") ||
      !raw["e_V"]["matrix"].is_array())
    return std::nullopt;
  const ojson& m = raw["e_V"]["matrix"];
  const size_t k = m.size();
  if (k == 0) return std::nullopt;
  std::vector<double> a(k * k);
  for (size_t r = 0; r < k; ++r) {
    if (!m[r].is_array() || m[r].size() != k) return std::nullopt;
    for (size_t c = 0; c < k; ++c) {
      if (!m[r][c].is_number()) return std::nullopt;
      a[r + c * k] = m[r][c].get<double>();
    }
  }
  std::vector<double> w, z;
  if (piet::lapack::dsyevrVAL(static_cast<int>(k), std::move(a), 0.0, w, z) != 0 || w.empty())
    return std::nullopt;
  const double top = *std::max_element(w.begin(), w.end());
  double rank = 0.0;
  for (double v : w)
    if (v > 1e-9 * top) rank += 1.0;
  return rank;
}

}  // namespace

// ===========================================================================
// xtreg — fe routes to feols with --fe=panelvar; re routes to cobra xtreg
// with --ivar=panelvar (no vcov support).

ojson runXtreg(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();

  bool isBe = false;
  for (const auto& [k, v] : parsed.opts) {
    static const std::set<std::string> kBad = {"gls", "mle", "ec1", "pa"};
    if (kBad.count(k) && v.isBool && v.boolValue) {
      return engineErrorBlockM4(
          "This version of PIET supports only `xtreg ,fe` (-> feols within), "
          "`xtreg ,re` (Swamy-Arora GLS) and `xtreg ,be` (between). `xtreg ," + k +
              "` is not available.",
          "unsupported_option");
    }
    if (k == "be" && v.isBool && v.boolValue) isBe = true;
  }
  // BUG-1515: between effects, xtreg,be: the OLS of the panel means of y on the
  // panel means of the regressors and a constant, one row per panel (cobra xtreg
  // --be). StataBE 19.5 (i6.do, 20 panels of 6): _b[x] .7062577229568211, _se[x]
  // .12404345599768729, e(df_r) 18, e(r2_b) .64298067688888827, e(model) "be".
  // Its syntax has no weight and no vce other than conventional (m2.do, StataBE
  // 19.5): [aw=] r(101) "weights not allowed", vce(robust) / vce(cluster id) r(198)
  // "vcetype robust|cluster not allowed", and robust, cluster() and noconstant are
  // r(198) "option ... not allowed". `wls` (weights by panel size) is not served.
  if (isBe) {
    for (const auto& [k, v] : parsed.opts) {
      if ((k == "fe" || k == "re") && v.isBool && v.boolValue)  // xtreg.ado opts_exclusive
        return ojson(eval::errorBlock("engine_error",
                                      "only one of re, fe, be, pa, or cre is allowed",
                                      std::nullopt, std::nullopt, std::nullopt, "r(198)"));
    }
    if (!session.panelvar.has_value() || session.panelvar->empty()) {
      return ojson(eval::errorBlock("engine_error", "must specify panelvar; use xtset",
                                    std::nullopt, std::nullopt, std::nullopt, "r(459)"));
    }
    if (parsed.weightType.has_value() && parsed.weightVar.has_value() &&
        !parsed.weightVar->empty()) {
      return ojson(eval::errorBlock("engine_error", "weights not allowed", std::nullopt,
                                    std::nullopt, std::nullopt, "r(101)"));
    }
    for (const auto& [k, v] : parsed.opts) {
      if (v.isBool && !v.boolValue) continue;
      std::string refusal;
      if (k == "vce" || k == "vcov") {
        // The first word as typed: vce(r) is "vcetype r not allowed", and even
        // vce(conv) is refused (StataBE 19.5 m3b.do); vce(bootstrap) and
        // vce(jackknife) are prefix runs this build does not have.
        const std::string vt = trimM4(v.isBool ? std::string() : v.strValue);
        const std::string word = vt.substr(0, vt.find_first_of(" \t,"));
        const std::string lw = lowerM4(word);
        if (lw.size() >= 4 && (std::string("bootstrap").rfind(lw, 0) == 0 ||
                               std::string("jackknife").rfind(lw, 0) == 0))
          return engineErrorBlockM4("`xtreg ,be vce(" + word + ")` is not available in this version "
                                    "of PIET.",
                                    "unsupported_option");
        if (!word.empty() && word != "conventional") refusal = "vcetype " + word + " not allowed";
      } else if (k == "robust" || k == "noconstant" || k == "nocons") {
        refusal = "option " + k + " not allowed";
      } else if (k == "cluster") {
        refusal = "option cluster() not allowed";
      } else if (k == "absorb") {
        refusal = "option absorb() not allowed";
      } else if (k == "wls") {
        return engineErrorBlockM4("`xtreg ,be wls` (weights by panel size) is not available in this "
                                  "version of PIET.",
                                  "unsupported_option");
      }
      if (!refusal.empty())
        return ojson(eval::errorBlock("engine_error", refusal, std::nullopt, std::nullopt,
                                      std::nullopt, "r(198)"));
    }
    if (auto refused = tsFitRestrictionRefusal(parsed, "xtreg")) return *refused;
    // What is left of vce() is vce(conventional), the default (rc 0 in Stata),
    // which the shared builder does not spell.
    eval::ParsedCommand beParsed = parsed;
    beParsed.opts.erase(std::remove_if(beParsed.opts.begin(), beParsed.opts.end(),
                                       [](const auto& o) { return o.first == "vce" || o.first == "vcov"; }),
                        beParsed.opts.end());
    EstimationArgvResult built = buildEstimationArgvM4("xtreg", beParsed, session);
    if (built.errored) return built.errorBlock;
    std::vector<std::string> cobraArgs = built.argv;
    cobraArgs.push_back("--ivar");
    cobraArgs.push_back(*session.panelvar);
    cobraArgs.push_back("--be");
    cobraArgs.push_back("--emit-v");
    cobraArgs.push_back("--data");
    cobraArgs.push_back(*session.currentHbin());
    cobraArgs.push_back("--tidy");
    ojson raw;
    try {
      raw = runCobraJsonM4(cobraArgs);
    } catch (const CobraError& ce) {
      return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
    }
    ojson block = cobraToEstimationM4(raw, "xtreg", &session);
    aliasPanelR2M4(raw);
    // e(N_g), e(Tbar) (the mean panel size), e(Tcon), e(rank) (cached reply only).
    if (raw.is_object()) {
      if (raw.contains("n_groups") && raw["n_groups"].is_number()) raw["N_g"] = raw["n_groups"];
      if (raw.contains("g_avg") && raw["g_avg"].is_number()) raw["Tbar"] = raw["g_avg"];
      if (raw.contains("g_min") && raw["g_min"].is_number() && raw.contains("g_max") &&
          raw["g_max"].is_number())
        raw["Tcon"] = raw["g_min"].get<double>() == raw["g_max"].get<double>() ? 1.0 : 0.0;
      if (raw.contains("e_V") && raw["e_V"].is_object() && raw["e_V"].contains("names") &&
          raw["e_V"]["names"].is_array())
        raw["k_vars"] = raw["e_V"]["names"].size();
    }
    cacheLastModelM4(session, "xtreg_be", cobraArgs, block, raw, std::string(), "xtreg");
    if (session.lastModel().has_value()) {
      putEStringNL(session, "model", "be");
      session.eval.setEString("vce", "conventional");
    }
    return block;
  }
  // model = str(opts.get("fe", opts.get("re", ""))).lower() — reads fe's
  // value if the key is present (bare flag -> "true"), else falls back to
  // re's value, else "". isFe = (model=="fe") or (opts.get("fe") is True).
  bool fePresent = false, rePresent = false;
  eval::OptValue feVal, reVal;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "fe") {
      fePresent = true;
      feVal = v;
    }
    if (k == "re") {
      rePresent = true;
      reVal = v;
    }
  }
  bool isFe = false;
  if (fePresent) {
    isFe = feVal.isBool && feVal.boolValue;  // opts.get("fe") is True (bare flag)
  } else if (rePresent) {
    isFe = false;
  } else {
    isFe = false;  // model == "" -> not "fe" -> RE branch (oracle errors there on missing panelvar)
  }

  if (isFe) {
    // BUG-657: Stata's own refusal (xtreg.ado -> `_xt, i()`): "must specify
    // panelvar; use xtset" r(459) -- w2_eststo_prefix.log:437-441 and
    // w2d_uncovered.log:3468-3473. It was unsupported_option = r(198), so a
    // do-file branching on `_rc == 459` took the wrong arm.
    if (!session.panelvar.has_value() || session.panelvar->empty()) {
      return ojson(eval::errorBlock("engine_error", "must specify panelvar; use xtset",
                                    std::nullopt, std::nullopt, std::nullopt, "r(459)"));
    }
    // FU-29: xtreg,fe's `syntax ... [aw fw pw]` -- an iweight is Stata's
    // r(101) "iweights not allowed" (w2_pweight.log X11_iw_fe). The registry
    // row is AllWeights because the RE arm below has a different rule.
    const bool weighted =
        parsed.weightType.has_value() && parsed.weightVar.has_value() && !parsed.weightVar->empty();
    if (weighted) {
      const std::string wt = lowerM4(*parsed.weightType);
      if (wt == "iw" || wt == "iweight") {
        return ojson(eval::errorBlock("engine_error", "iweights not allowed", std::nullopt,
                                      std::nullopt, std::nullopt, "r(101)"));
      }
    }
    // FU-475: the engine applies --keepif after its time-series operators only
    // for a restriction it can evaluate on the full frame.
    if (auto refused = tsFitRestrictionRefusal(parsed, "xtreg")) return *refused;
    EstimationArgvResult built =
        buildEstimationArgvM4("feols", parsed, session, *session.panelvar);
    if (built.errored) return built.errorBlock;
    if (weighted) {
      if (auto refused = xtregWeightWithinPanelM4(parsed, session, *session.panelvar)) {
        return *refused;
      }
    }
    std::vector<std::string> cobraArgs = built.argv;
    // FU-386: xtreg,fe's multiway cluster VCE is Stata's built-in convention
    // (xtreg,fe -> Stata's absorb step -> Stata's robust-VCE step), minus = rank + dfax:
    // w2s_multiway_class.log:696 vce(cluster cg1 cg2) se(x1) .0532737214421747 where
    // reghdfe's Gmin factor (the engine default) gave .0509313674247111.
    // FU-401/FU-407: every cluster variable must nest the panels, else r(498)
    // (Stata's absorb step -> Stata's xtreg cluster check; log:640
    // vce(cluster id id2), vce(cluster id2)): the engine checks it on the
    // estimation sample (--fe-nested-in-clusters) and the refusal is mapped below.
    bool clusterVce = false;
    bool multiwayVce = false;
    for (size_t i = 0; i + 1 < cobraArgs.size(); ++i) {
      if (cobraArgs[i] != "--vcov") continue;
      clusterVce = cobraArgs[i + 1] == "cluster" || cobraArgs[i + 1] == "twoway" ||
                   cobraArgs[i + 1] == "multiway";
      multiwayVce = cobraArgs[i + 1] == "twoway" || cobraArgs[i + 1] == "multiway";
    }
    if (multiwayVce) cobraArgs.push_back("--multiway-subset-factors");
    // BUG-538 (W2 estimopts): under `xtreg, fe` Stata's vce(robust) / bare
    // `robust` IS vce(cluster panelvar) -- w2_pweight.log X1_fe_vcerobust
    // is byte-identical to X2_fe_clusterid (e(vce)=cluster, e(clustvar)=id,
    // N_clust 15, df_r 14, se_x1 +1.0ba4e5218cb87X-002), where this route
    // forwarded HC1 (n_clusters 0, df_r 103) at rc 0. The shared builder
    // cannot know the panel variable, so the rewrite lives here, after it.
    for (size_t i = 0; i + 1 < cobraArgs.size(); ++i) {
      if (cobraArgs[i] == "--vcov" && cobraArgs[i + 1] == "hc1") {
        cobraArgs[i + 1] = "cluster";
        cobraArgs.insert(cobraArgs.begin() + static_cast<std::ptrdiff_t>(i + 2),
                         {"--cluster", *session.panelvar});
        break;
      }
    }
    // e(sample) T2: xtreg, fe keeps a one-observation panel in the sample and in
    // N_g (w2_esample.log:897 D3 `0|59|59|11|`; the engine's reghdfe default
    // dropped id 11: 58 and 10). Read back into LastModel::keepSingletons by
    // cacheLastModelM4, so the refits keep it too.
    cobraArgs.push_back("--keep-singletons");
    // BUG-1067: xtreg, fe keeps the earlier term of a collinear set, as reghdfe and
    // areg do (StataBE scratch-r150/c1067/m1.do A_xtreg `x o.z`), not the engine's
    // default pivoted basis (regress's `o.x z`). Read back into
    // LastModel::collinearForward by cacheLastModelM4, so the refits keep it too.
    cobraArgs.push_back("--collinear-forward");
    cobraArgs.push_back("--data");
    cobraArgs.push_back(*session.currentHbin());
    cobraArgs.push_back("--tidy");
    // e(sample) T2: the engine writes the fit's admitted rows over the --data
    // rows (its own --keepif included); cacheLastModelM4 installs them. Kept
    // out of cobraArgs, which cacheLastModelM4 reads as the fit's flags.
    std::vector<std::string> fitArgv = cobraArgs;
    const std::string esampleValues = esampleFitArgs(session, fitArgv, std::string(), 0);
    fitArgv.push_back("--emit-v");  // eb-order T-P1: the fit's full e(V) (feols row)
    // FU-401: the nesting check is the fit's alone (not in cobraArgs, which the
    // refits replay: a model that was fitted already passed it). vce(robust) was
    // rewritten to cluster(panelvar) above, which nests trivially.
    if (clusterVce || std::find(cobraArgs.begin(), cobraArgs.end(), "--cluster") != cobraArgs.end())
      fitArgv.push_back("--fe-nested-in-clusters");
    ojson raw;
    try {
      raw = runCobraJsonM4(fitArgv);
    } catch (const CobraError& ce) {
      cecon::native_unlink(esampleValues);
      // Stata's r(498), with its message (one-way: the first line only); no e() is
      // posted, so a following `_se[x1]` is r(111) (log:645-648).
      if (ce.detail == "panels_not_nested")
        return ojson(eval::errorBlock("engine_error", ce.what(), std::nullopt, std::nullopt,
                                      std::nullopt, std::string("r(498)")));
      // BUG-1269: `xtreg, fe` on an empty sample is r(2001) "insufficient
      // observations", not regress's r(2000) (StataBE 19.5, `xtreg y x if x > 20, fe`).
      if (ce.code == "no_obs" && std::string(ce.what()) == "no observations")
        return ojson(eval::errorBlock("engine_error", "insufficient observations", std::nullopt,
                                      std::nullopt, std::nullopt, std::string("r(2001)")));
      return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
    }
    // FU-29: e(sigma_e) of a weighted fit. The engine's panel-scalar producer
    // (apps/cmds/common.cpp capture_panel_means) stands down under weights, so
    // the weighted reply has no sigma_e -- but it has rmse, and for the within
    // estimator both are sqrt(rss / (N - N_g - k)) (the unweighted reply
    // carries the two keys bit-identical: 2.4986263477117516). Filled only when
    // the reply's own residual dof IS N - N_g - k. Oracle: w2_pweight.log X3-X9
    // PVAL sigma_e +1.4013601975c1dX+001 = the engine's weighted rmse
    // 2.500591289931903; X14 (fweight) header sigma_e 2.3734216 = rmse with dof
    // 343. sigma_u/rho/corr/r2_b/r2_o have no such identity and stay `.`,
    // named in a note on the block (see below).
    if (raw.is_object() && !raw.contains("sigma_e") && raw.contains("rmse") &&
        raw["rmse"].is_number() && raw.contains("dof_resid") && raw["dof_resid"].is_number() &&
        raw.contains("n_obs") && raw["n_obs"].is_number() && raw.contains("k_vars") &&
        raw["k_vars"].is_number() && raw.contains("absorbed") && raw["absorbed"].is_object() &&
        raw["absorbed"].size() == 1 && raw["absorbed"].contains(*session.panelvar) &&
        raw["absorbed"][*session.panelvar].is_number()) {
      const double dofWithin = raw["n_obs"].get<double>() -
                               raw["absorbed"][*session.panelvar].get<double>() -
                               raw["k_vars"].get<double>();
      if (raw["dof_resid"].get<double>() == dofWithin) raw["sigma_e"] = raw["rmse"];
    }
    ojson block = cobraToEstimationM4(raw, "feols", &session);
    // BUG-538, e() half: [XT] xtreg stores e(N_g) (panels in the fit) and
    // e(df_a) = N_g - 1 (w2_pweight.log X0_fe PXT: 15 and 14; X7 under
    // `if t>2`: still 15 and 14). The engine reports the panel count as the
    // absorbed-levels entry for the panel variable; it is copied into the
    // cached reply under the xtreg names so e(N_g)/e(df_a) resolve (the
    // resolver falls through key==name), and NOT into the emitted block,
    // whose shape the freeze fixtures pin.
    if (raw.is_object() && raw.contains("absorbed") && raw["absorbed"].is_object() &&
        raw["absorbed"].contains(*session.panelvar) &&
        raw["absorbed"][*session.panelvar].is_number()) {
      const double ng = raw["absorbed"][*session.panelvar].get<double>();
      raw["N_g"] = ng;
      raw["df_a"] = ng - 1.0;
    }
    // FU-102: e(g_min)/e(g_avg)/e(g_max), the panel row counts over the
    // estimation sample (xtreg,fe; w2p_estim_ev D_xtreg_fe_ctl
    // `0|60|10|6|6|xtreg|`). Copied from the engine's fe_group_sizes into the
    // cached reply only, like N_g above.
    if (raw.is_object() && raw.contains("fe_group_sizes") && raw["fe_group_sizes"].is_object()) {
      const ojson& gs = raw["fe_group_sizes"];
      if (gs.contains("min") && gs.contains("avg") && gs.contains("max")) {
        raw["g_min"] = gs["min"];
        raw["g_avg"] = gs["avg"];
        raw["g_max"] = gs["max"];
        // BUG-1516: e(Tbar) is the mean group size and e(Tcon) whether every panel
        // has the same size (xtreg,fe); e(k_absorb) is e(N_g), the
        // absorbed panel count (also under a multiway VCE).
        // StataBE 19.5 (30 panels of 6): e(Tbar) 6, e(Tcon) 1, e(k_absorb) 30.
        raw["Tbar"] = gs["avg"];
        if (gs["min"].is_number() && gs["max"].is_number())
          raw["Tcon"] = gs["min"].get<double>() == gs["max"].get<double>() ? 1.0 : 0.0;
        if (raw.contains("N_g")) raw["k_absorb"] = raw["N_g"];
      }
    }
    // BUG-383 #1 (off-pin sweep vs live Stata, 2026-09-01): the xtreg,fe route
    // published the underlying feols block untouched, and two of its members
    // are feols conventions, not xtreg ones. [XT] xtreg stores e(r2) as the
    // WITHIN R-squared (captured: 0.07589076642404147 — our raw r2_within
    // matches it to 1e-15), where feols's r2 is the FULL-model LSDV fit
    // (0.9905). And e(df_m) counts the absorbed group dummies alongside the
    // slopes (captured: 48 = 1 slope + 47 dummies), where feols reports the
    // slope count alone. Both raw ingredients are already in the engine reply
    // (r2_within, k_vars, k_absorbed); this route just has to pick the xtreg
    // spellings. The r2_w/r2_b/r2_o trio and the rendered header are untouched.
    // The block's df_m now mirrors e(df_m), with the header's own numerator beside
    // it (BUG-1291, below the model-based scalars).
    if (raw.contains("r2_within")) block["r2"] = raw["r2_within"];
    // FU-76: the same two members, e() side. BUG-383 wrote them into the block
    // only, while e() resolves from the cached reply, so e(r2) read the LSDV fit
    // (0.7235263265398173) and e(df_m) the slope count 2 (w2_pweight.log X0_fe
    // PVAL e(r2) +1.132199730c722X-001 = r2_within 0.5373657181682785).
    // e(df_m) follows xtreg,fe's two arms: conventional VCE posts
    // `df_m = e(df_a) + dfb` (X0_fe and X3_aw_fe: 2 + 14 = 16), a cluster VCE
    // posts regress's own model df, which the oracle shows as one less than
    // the slope count: X1, X2, X4, X5, X7, X8, X9 (cluster id, 15 clusters)
    // and X6 (cluster cl2, 5 clusters) all 1 with k = 2, X13 2 with k = 3.
    // Other VCE shapes are not witnessed and keep the engine's value. The
    // block mirrors it (BUG-1291, below).
    // BUG-1291: e(df_b), the slope count the model F tests: r(dfr) under the
    // conventional VCE, the model test's dof under a cluster VCE
    // (Stata's absorb step); StataBE 2 for `xtreg y x1 x2, fe`
    // and for vce(cluster id) (with e(df_m) 31 and 1). The test drops the
    // constraints a clipped multiway V cannot carry, so its rank (the reply's
    // vcov_rank) bounds it: StataBE [aw=w] vce(cluster cg1 cg2 cg3) e(rank) 1,
    // e(df_b) 1.
    if (raw.contains("r2_within")) raw["r2"] = raw["r2_within"];
    // BUG-1482: the clustered V's rank r(rankc) on the absorb route
    // (xtreg,fe: a cluster variable other than the panel variable, or several):
    // the multiway reply's vcov_rank, else the rank of the one-way e_V. Below the
    // slope count it is e(rank), it bounds e(df_b) and e(df_m), and the model test
    // drops a constraint, so e(F) is missing. StataBE 19.5,
    // vce(cluster c2) with 2 clusters and 2 slopes: e(rank) 1, e(df_b) 1, e(df_m) 0,
    // e(F) . (vce(cluster c2 c3), min G 2, alike).
    std::optional<double> rankc;
    if (multiwayVce) {
      if (raw.contains("vcov_rank") && raw["vcov_rank"].is_number())
        rankc = raw["vcov_rank"].get<double>();
    } else if (clusterVce) {
      for (size_t i = 0; i + 1 < cobraArgs.size(); ++i)
        if (cobraArgs[i] == "--cluster" && cobraArgs[i + 1] != *session.panelvar)
          rankc = clusteredVRankM4(raw);
    }
    if (raw.contains("k_vars") && raw["k_vars"].is_number() && raw.contains("df_a") &&
        raw["df_a"].is_number()) {
      const double k = raw["k_vars"].get<double>();
      const std::string vt = raw.value("vcov_type", std::string());
      raw["df_b"] = rankc.has_value() ? std::min(k, *rankc) : k;
      if (rankc.has_value() && *rankc < k) {
        raw["F"] = nullptr;
        raw["p_F"] = nullptr;
        block["F"] = nullptr;
        block["p_F"] = nullptr;
      }
      if (vt == "iid") {
        raw["df_m"] = k + raw["df_a"].get<double>();
      } else if (vt.rfind("cluster(", 0) == 0 && k >= 1.0) {
        raw["df_m"] = k - 1.0;
      }
    }
    aliasPanelR2M4(raw);
    // FU-29 review: a weighted fit's reply has no sigma_u/rho/corr_u_xb/
    // r2_between/r2_overall (the engine's capture_panel_means stands down under
    // weights; engine follow-up FU-126), so those e() fields read `.` where
    // Stata posts numbers (w2_pweight.log X3_aw_fe PXT :352 sigma_u
    // +1.4e5921bc56b86X+000, rho +1.b6f09048fd04cX-003, corr
    // +1.0d40c44e4227aX-001; X4-X9 and X13 alike). The fit itself matches, so
    // it is kept, and the gap is named on the block's `warnings` (rendered as
    // `note:` lines, BUG-112(8)) rather than left as a silent `.`. Only the
    // fields actually absent are named; an unweighted fit carries all five and
    // gets no note (its block stays freeze-identical).
    if (weighted && raw.is_object()) {
      static const std::vector<std::pair<const char*, const char*>> kPanelScalars = {
          {"sigma_u", "sigma_u"}, {"rho", "rho"}, {"corr", "corr_u_xb"},
          {"r2_b", "r2_b"},       {"r2_o", "r2_o"}};
      std::vector<std::string> absent;
      for (const auto& [eName, key] : kPanelScalars) {
        if (!raw.contains(key) || !raw[key].is_number()) absent.push_back(std::string("e(") + eName + ")");
      }
      if (!absent.empty()) {
        std::string list;
        for (size_t i = 0; i < absent.size(); ++i) {
          if (i > 0) list += (i + 1 == absent.size()) ? " and " : ", ";
          list += absent[i];
        }
        if (!block.contains("warnings") || !block["warnings"].is_array()) block["warnings"] = ojson::array();
        block["warnings"].push_back(list + (absent.size() == 1 ? " is" : " are") +
                                    " not computed for weighted xtreg, fe in this engine (left missing)");
      }
    }
    // FU-402: the built-in multiway e() (no e(N_clust), e(rank) of the clipped V).
    const ojson rawFit = raw;
    if (multiwayVce) applyBuiltinMultiwayReplyM4(raw);
    // The model-based scalars (Stata's absorb step with cmd xtreg_fe):
    // e(rmse) = sqrt(r(mse)) and e(r2_a) = 1 - mse/(sst_xt/(N-1)), mse = rss / r(dfe). Under any cluster VCE (one-way, vce(robust) =
    // cluster(panelvar) here, multiway; Stata's kclust > 0) the panels nest in
    // every cluster (checked above), so r(dfe) = N - rank_mb charges no absorbed
    // panel mean: StataBE (fixture w2s_multiway_class) vce(cluster cg1 cg2),
    // vce(cluster id) and vce(cluster cg1) e(rmse) 1.4571896494673038 =
    // sqrt(375.84/177) and e(r2_a) .9408651668053871, where the engine's within dof
    // (148) gave 1.5936 and .93932 -- that dof is e(sigma_e)'s (xtreg,fe).
    // Under the conventional VCE r(dfe) is that within dof, whose rmse the reply
    // carries, and e(r2_a) .929277935976713 (BUG-1042; the reply's adj_r2 is the
    // LSDV fit's .93932); e(rank) there is r(rank) = rank_mb, 3 (a cluster VCE's
    // e(rank) is the clustered V's, the slope count k_vars or the multiway
    // vcov_rank). Weighted fits follow the same rules on the reply's weighted sums
    // (StataBE [aw=w] conventional e(r2_a) .93017655599106241, vce(cluster id)
    // e(rmse) 1.4200006643786318 and .94161655529196175, [pw=w] alike; [fw=w] with
    // N = 450 .93798739005530563, and 1.4128349298420197 / .94201057951480482;
    // [aw=w] vce(cluster cg1 cg2) 1.4200006643786312 / .94161655529196187).
    // e(df_m) under the robust/cluster VCE is r(dfr) + r(dfax)
    // (Stata's absorb step, put back after the model test),
    // with r(dfr) = min(rank, rankc) - cons (Stata's absorb solver): rank the
    // model-based rank e(rank_mb) (the fitted columns, _cons included), rankc the
    // clustered V's rank e(rank) (the engine's vcov_rank), cons 1 when _cons has a
    // positive variance, and r(dfax) 0 because xtreg, fe absorbs only
    // the panel. StataBE (fixture above): rank_mb 3, rank 2 ->
    // e(df_m) 1, weighted ([aw=w] vce(cluster cg1 cg2)) too. e(df_b) is the
    // model test's dof (postENameListsXtregFeM4).
    const std::string vcovTypeFit = rawFit.value("vcov_type", std::string());
    const bool clusterFit = multiwayVce || vcovTypeFit.rfind("cluster(", 0) == 0;
    if ((clusterFit || vcovTypeFit == "iid") && raw.is_object() && raw.contains("tss_within") &&
        raw["tss_within"].is_number() && raw.contains("rss") && raw["rss"].is_number() &&
        raw.contains("n_obs") && raw["n_obs"].is_number() && raw.contains("dof_resid") &&
        raw["dof_resid"].is_number() && raw.contains("e_V") && raw["e_V"].is_object() &&
        raw["e_V"].contains("names") && raw["e_V"]["names"].is_array()) {
      const double n = raw["n_obs"].get<double>();
      const double rankMb = static_cast<double>(raw["e_V"]["names"].size());
      const double dfe = clusterFit ? n - rankMb : raw["dof_resid"].get<double>();
      const double rssV = raw["rss"].get<double>(), sstXt = raw["tss_within"].get<double>();
      if (dfe > 0 && n > 1 && sstXt > 0) {
        const double mse = rssV / dfe;
        if (clusterFit) raw["rmse"] = std::sqrt(mse);
        raw["adj_r2"] = 1.0 - mse / (sstXt / (n - 1.0));
        if (!clusterFit) raw["k_vars"] = rankMb;  // e(rank) = r(rank)
        // BUG-1482: a one-way cluster VCE's e(rank) is r(rankc) too (the multiway
        // reply's k_vars already is, applyBuiltinMultiwayReplyM4).
        if (rankc.has_value() && !multiwayVce) raw["k_vars"] = *rankc;
        if (rankc.has_value()) {
          // cons: the _cons column's variance is positive in the clustered V.
          double cons = 0.0;
          const auto& ev = raw["e_V"];
          if (ev.contains("matrix") && ev["matrix"].is_array()) {
            const auto& names = ev["names"];
            for (size_t j = 0; j < names.size(); ++j) {
              if (!names[j].is_string()) continue;
              const std::string nm = names[j].get<std::string>();
              if (nm != "_cons" && nm != "(intercept)") continue;
              const auto& mat = ev["matrix"];
              if (j < mat.size() && mat[j].is_array() && j < mat[j].size() &&
                  mat[j][j].is_number() && mat[j][j].get<double>() > 0.0)
                cons = 1.0;
            }
          }
          const double dfax = 0.0;  // xtreg, fe: only the panel is absorbed
          const double dfr = std::min(rankMb, *rankc) - cons;
          if (dfr >= 0.0) raw["df_m"] = dfr + dfax;
        }
      }
    }
    // BUG-1291 / BUG-1042: the block carries the e() scalars eststo/esttab read off
    // it (eststo_esttab.cpp escalarsFrom: e(df_m), e(r2_a), e(rmse)) as the cached
    // reply resolves them, where it had k + N_g - 1, the LSDV adj_r2 and the
    // within-dof rmse under every VCE; and e(df_b), the numerator of xtreg's header
    // F(e(df_b), e(df_r)) (xtreg,fe's display), which the console prints:
    // StataBE "F(2, 148) = 1191.52", under vce(cluster id) "F(2, 29) = 1142.39",
    // where the block's df_m printed F(31, ...).
    for (const char* key : {"df_m", "df_b", "adj_r2", "rmse"})
      if (raw.contains(key) && raw[key].is_number()) block[key] = raw[key];
    // e(cmd) is `xtreg` (w2v_margins_corpus F1_fit `cmd=xtreg|N=200`); the replay
    // machinery keeps keying on cobraCmd "feols".
    cacheLastModelM4(session, "feols", cobraArgs, block, raw, esampleValues, "xtreg");
    if (multiwayVce) postBuiltinMultiwayEM4(session, rawFit);
    // Stata's ordered `: e(scalars)' for this fit (w2s_multiway_class mw_xt_cg_scalars).
    if (multiwayVce && !weighted) postENameListsXtregFeM4(session, raw, parsed);
    else session.eNameContract.reset();
    // e(vce) after a default-VCE `xtreg, fe` is `conventional` (w2_pweight.log
    // X0_fe PSTR `120|103|.|conventional|||||`; X3_aw_fe alike). setLastModel()
    // published lm.vcov's engine spelling `iid`, which the replay machinery keys
    // on, so only the macro store is corrected. vce(robust)/cluster keep
    // `cluster` (X1/X2 PSTR). Same KNOWN GAP as runRegress's `ols`: a later
    // setLastModel() on this model (setEstHandleM4) republishes `iid`.
    if (session.lastModel().has_value() && session.lastModel()->vcov == "iid") {
      session.eval.setEString("vce", "conventional");
    }
    // BUG-1516: e(model) "fe" (xtreg,fe; StataBE 19.5 i17.do).
    if (session.lastModel().has_value()) putEStringNL(session, "model", "fe");
    return block;
  }

  // Random effects.
  // BUG-657: same `_xt, i()` refusal as the fixed-effects arm above (xtreg.ado
  // calls it before choosing the model), r(459).
  if (!session.panelvar.has_value() || session.panelvar->empty()) {
    return ojson(eval::errorBlock("engine_error", "must specify panelvar; use xtset", std::nullopt,
                                  std::nullopt, std::nullopt, "r(459)"));
  }
  // FU-29: xtreg,re's `syntax` declares no weight, so any weight type is
  // Stata's r(101) "weights not allowed" (w2_pweight.log X12_pw_re). Until the
  // registry row opened to AllWeights the tokenizer refused it as r(198).
  if (parsed.weightType.has_value() && parsed.weightVar.has_value() &&
      !parsed.weightVar->empty()) {
    return ojson(eval::errorBlock("engine_error", "weights not allowed", std::nullopt, std::nullopt,
                                  std::nullopt, "r(101)"));
  }
  for (const auto& [k, v] : parsed.opts) {
    // BUG-055/P2 side effect: legacy `cluster(v)` (no vce()/robust) now
    // maps to `vce(cluster v)` inside vcovFromOpts (shared by this RE
    // path), so it must be caught HERE too — otherwise it would sail past
    // this guard and reach cobra's RE call as an unsupported --vcov/
    // --cluster flag pair instead of getting this same clean message.
    if ((k == "vce" || k == "vcov" || k == "robust" || k == "cluster") &&
        !(v.isBool && !v.boolValue)) {
      return engineErrorBlockM4("cobra xtreg (random-effects) does not support vcov options.",
                                "unsupported_option");
    }
  }
  EstimationArgvResult built = buildEstimationArgvM4("xtreg", parsed, session);
  if (built.errored) return built.errorBlock;
  std::vector<std::string> cobraArgs = built.argv;
  cobraArgs.push_back("--ivar");
  cobraArgs.push_back(*session.panelvar);
  cobraArgs.push_back("--emit-v");  // BUG-1196: e(V), joined by cacheLastModelM4
  cobraArgs.push_back("--data");
  cobraArgs.push_back(*session.currentHbin());
  cobraArgs.push_back("--tidy");
  ojson raw;
  try {
    raw = runCobraJsonM4(cobraArgs);
  } catch (const CobraError& ce) {
    return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
  }
  ojson block = cobraToEstimationM4(raw, "xtreg", &session);
  aliasPanelR2M4(raw);
  // BUG-1516: xtreg,re posts e(Tbar), the harmonic mean of the panel sizes
  // (the reply's T_h), e(Tcon) whether they are all equal, e(N_g), and e(rank)
  // the rank of the full-rank V (every coefficient, _cons included). StataBE 19.5
  // (30 panels of 6): e(Tbar) 6, e(Tcon) 1, e(rank) 3. Cached reply only.
  if (raw.is_object()) {
    if (raw.contains("T_h") && raw["T_h"].is_number()) raw["Tbar"] = raw["T_h"];
    if (raw.contains("g_min") && raw["g_min"].is_number() && raw.contains("g_max") &&
        raw["g_max"].is_number())
      raw["Tcon"] = raw["g_min"].get<double>() == raw["g_max"].get<double>() ? 1.0 : 0.0;
    if (raw.contains("n_groups") && raw["n_groups"].is_number()) raw["N_g"] = raw["n_groups"];
    if (raw.contains("e_V") && raw["e_V"].is_object() && raw["e_V"].contains("names") &&
        raw["e_V"]["names"].is_array())
      raw["k_vars"] = raw["e_V"]["names"].size();
  }
  cacheLastModelM4(session, "xtreg", cobraArgs, block, raw);
  if (session.lastModel().has_value()) putEStringNL(session, "model", "re");
  return block;
}

// ===========================================================================
// P7 — ordered / multinomial / zero-inflated / fractional MLE
//
// These engine verbs take `--vce iid|robust|cluster --cluster C` (the ML family
// spelling), NOT buildEstimationArgvM4's `--vcov`, so they share a small builder
// of their own rather than bending the shared one and risking the frozen output
// of every existing caller.

namespace est_detail {  // R2: reopened — these cross the TU cut (runP7Mle & friends)

struct P7MleArgv {
  bool errored = false;
  ojson errorBlock;
  std::vector<std::string> argv;
};

// Builds `CMD --y Y --x X1,X2 [--keepif E] [--vce ... [--cluster C]]`.
// `vceAllowed` names the vce spellings this verb actually implements; anything
// else is an honest reject rather than a silent downgrade to iid.
// BUG-333: forward Stata's `level(#)` to the engine. All six commands wired
// through this (nl/gmm, mixed, fracreg, ivprobit/ivtobit) register AND consume
// `--level` in cobra today (options_table.cpp:110-118); PIET was simply not
// passing it, so the option was an honest-but-unnecessary reject. Kept as a
// free function rather than a branch inside buildP7MleArgv because that builder
// is shared with commands whose engine half has NO --level (xtlogit), and a
// blanket push there would turn a working call into an E6 unconsumed-flag error.
// Same idiom as runTobit/runQreg/runNbreg: forward the string, let the engine
// own the [10, 99.99] range check and surface it through CobraError.
void pushLevelIfGivenM4(const eval::ParsedCommand& parsed, std::vector<std::string>& argv) {
  for (const auto& [k, v] : parsed.opts) {
    if (k == "level" && !v.isBool) {
      argv.push_back("--level");
      argv.push_back(v.strValue);
      return;
    }
  }
}

P7MleArgv buildP7MleArgv(const std::string& cobraCmd, const eval::ParsedCommand& parsed,
                         const std::vector<std::string>& vceAllowed) {
  P7MleArgv r;
  if (parsed.varlist.size() < 2) {
    r.errored = true;
    r.errorBlock = engineErrorBlockM4(
        "syntax_error: " + cobraCmd + " requires a dependent variable and at least one regressor",
        "syntax_error");
    return r;
  }
  const std::string y = parsed.varlist[0];
  const std::vector<std::string> xs(parsed.varlist.begin() + 1, parsed.varlist.end());
  r.argv = {cobraCmd, "--y", y, "--x", joinCommaM4(xs)};
  if (parsed.ifExpr.has_value()) {
    r.argv.push_back("--keepif");
    r.argv.push_back(*parsed.ifExpr);
  }

  if (auto shapeErr = vceOptionShapeErrorM4(parsed)) {
    r.errored = true;
    r.errorBlock = engineErrorBlockM4(*shapeErr, "unsupported_option");
    return r;
  }
  std::optional<std::string> vceVal, clusterOpt;
  bool robustFlag = false;
  for (const auto& [k, v] : parsed.opts) {
    if ((k == "vce" || k == "vcov") && !vceVal.has_value())
      vceVal = v.isBool ? std::string() : v.strValue;
    else if (k == "robust" && v.isBool && v.boolValue)
      robustFlag = true;
    else if (k == "cluster" && !v.isBool)
      clusterOpt = v.strValue;
  }
  std::string want;
  std::string cvar = clusterOpt.value_or("");
  if (vceVal.has_value() && !vceVal->empty()) {
    std::string lv = lowerM4(*vceVal);
    if (lv.rfind("cluster", 0) == 0) {
      want = "cluster";
      if (lv.size() > 7) {
        std::string rest = trimM4(lv.substr(7));
        if (!rest.empty()) cvar = rest;
      }
    } else {
      want = lv;
    }
  } else if (!cvar.empty()) {
    // Stata's legacy bare `cluster(v)` is a synonym for vce(cluster v) — the
    // BUG-055 class. Honouring it here keeps these verbs from silently running
    // iid while the user believes they clustered. BUG-537: tested BEFORE the
    // bare `robust` flag -- `robust cluster(v)` together is cluster-robust in
    // Stata (w2_pweight.log R5_pw_robust_AND_cluster_equals_R4); robust-first
    // dropped the cluster. Same ordering as vcovFromOpts above.
    want = "cluster";
  } else if (robustFlag) {
    want = "robust";
  }
  if (want.empty()) return r;  // engine default (iid)

  bool allowed = false;
  for (const auto& s : vceAllowed)
    if (s == want) { allowed = true; break; }
  if (!allowed) {
    r.errored = true;
    r.errorBlock = engineErrorBlockM4(
        "Option `vce(" + want + ")` is a valid Stata option that this version of PIET does not "
        "implement for `" + cobraCmd + "`.",
        "unsupported_option");
    return r;
  }
  if (want == "cluster") {
    // A second cluster variable would be two-way clustering, which none of these
    // ML estimators implements — rejecting is parity, quietly using the first
    // variable is not.
    std::istringstream iss(cvar);
    std::vector<std::string> toks;
    std::string t;
    while (iss >> t) toks.push_back(t);
    if (toks.empty()) {
      r.errored = true;
      r.errorBlock = engineErrorBlockM4("Cluster variable required: vce(cluster varname)",
                                        "unsupported_option");
      return r;
    }
    if (toks.size() > 1) {
      r.errored = true;
      r.errorBlock = engineErrorBlockM4(
          "Two-way clustering is not implemented for `" + cobraCmd + "`.", "unsupported_option");
      return r;
    }
    r.argv.push_back("--vce");
    r.argv.push_back("cluster");
    r.argv.push_back("--cluster");
    r.argv.push_back(toks[0]);
  } else {
    r.argv.push_back("--vce");
    r.argv.push_back(want);
  }
  return r;
}

// rawOut, when given, receives the engine payload BEFORE cobraToEstimationM4
// narrows it to the known block keys — the ordered models need it because their
// cutpoints ride in a key the mapper does not carry through.
// `stataCmd` is the e(cmd) word LastModel records (cacheLastModelM4); empty
// publishes cobraCmd, as every runP7Mle caller does.
static ojson runP7MleAsM4(const std::string& cobraCmd, std::vector<std::string> cobraArgs,
                          SessionState& session, ojson* rawOut, const std::string& stataCmd) {
  cobraArgs.push_back("--data");
  cobraArgs.push_back(*session.currentHbin());
  cobraArgs.push_back("--tidy");
  ojson raw;
  try {
    raw = runCobraJsonM4(cobraArgs);
  } catch (const CobraError& ce) {
    return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
  }
  if (rawOut) *rawOut = raw;
  ojson block = cobraToEstimationM4(raw, cobraCmd, &session);
  fillCoeftableStatsM4(block);
  cacheLastModelM4(session, cobraCmd, cobraArgs, block, raw, std::string(), stataCmd);
  return block;
}

ojson runP7Mle(const std::string& cobraCmd, std::vector<std::string> cobraArgs,
               SessionState& session, ojson* rawOut) {
  return runP7MleAsM4(cobraCmd, std::move(cobraArgs), session, rawOut, std::string());
}

// BUG-1196: the depvar equation of an engine coefficient array as Stata names
// it -- `depvar:name`, `(intercept)` spelled `_cons` and placed last. False when
// a name is not a plain variable name (a factor level, an interaction, a
// time-series operator): Stata's columns there (base levels, `o.` marks) are
// not the engine's, and nothing is posted.
bool depvarEqColsM4(const std::string& depvar, const ojson& coefs, const char* bKey,
                    std::vector<AncillaryEbColM4>& out) {
  static const std::regex rePlain(R"(^[A-Za-z_][A-Za-z0-9_]*$)");
  if (!std::regex_match(depvar, rePlain) || !coefs.is_array() || coefs.empty()) return false;
  std::vector<AncillaryEbColM4> cols;
  std::optional<AncillaryEbColM4> cons;
  for (const auto& c : coefs) {
    if (!c.is_object() || !c.contains("name") || !c["name"].is_string() || !c.contains(bKey) ||
        !c[bKey].is_number())
      return false;
    const std::string nm = c["name"].get<std::string>();
    AncillaryEbColM4 col{std::string(), nm, c[bKey].get<double>()};
    if (nm == "(intercept)" || nm == "_cons") {
      col.stata = depvar + ":_cons";
      cons = col;
      continue;
    }
    if (!std::regex_match(nm, rePlain)) return false;
    col.stata = depvar + ":" + nm;
    cols.push_back(std::move(col));
  }
  if (cons.has_value()) cols.push_back(*cons);
  out.insert(out.end(), cols.begin(), cols.end());
  return true;
}

// BUG-1196: e(b)/e(V) of a fit whose Stata stripe has equations or ancillary
// parameters that the coefficient table does not carry (tobit, ologit/oprobit,
// nbreg, mlogit), posted as e() matrices, which every e(b)/e(V) reader answers
// before the stripe table's refusal (EvalSession::resolveEMatrixFor). `cols`
// are Stata's columns in order: its name, the engine's `e_V` name for it ("" for
// a column Stata posts as a structural 0 -- mlogit's base outcome), and b. e(V)
// is the engine's `--emit-v` covariance, made exactly symmetric as
// canonicalEMatrix makes it. Posts nothing -- the refusal stands -- unless the
// reply's `e_V` names every column with finite cells.
bool postAncillaryEbM4(SessionState& session, const ojson& raw,
                       const std::vector<AncillaryEbColM4>& cols) {
  const auto it = raw.find("e_V");
  if (cols.empty() || it == raw.end() || !it->is_object() || !it->contains("names") ||
      !it->contains("matrix"))
    return false;
  const ojson& vn = (*it)["names"];
  const ojson& vm = (*it)["matrix"];
  if (!vn.is_array() || !vm.is_array() || vm.size() != vn.size()) return false;
  const size_t k = cols.size();
  std::vector<long> at(k, -1);
  for (size_t a = 0; a < k; ++a) {
    if (!std::isfinite(cols[a].b)) return false;
    if (cols[a].engine.empty()) continue;
    for (size_t j = 0; j < vn.size(); ++j)
      if (vn[j].is_string() && vn[j].get<std::string>() == cols[a].engine) {
        at[a] = static_cast<long>(j);
        break;
      }
    if (at[a] < 0) return false;
  }
  const auto cell = [&vm, &vn](long r, long c, double* v) {
    const ojson& row = vm[static_cast<size_t>(r)];
    if (!row.is_array() || row.size() != vn.size() || !row[static_cast<size_t>(c)].is_number())
      return false;
    *v = row[static_cast<size_t>(c)].get<double>();
    return std::isfinite(*v);
  };
  eval::EvalMatrix mb, mv;
  mb.rownames = {"y1"};
  mb.data.emplace_back();
  mv.data.assign(k, std::vector<double>(k, 0.0));
  for (size_t a = 0; a < k; ++a) {
    mb.colnames.push_back(cols[a].stata);
    mb.data[0].push_back(cols[a].b);
    for (size_t c = 0; c < k; ++c) {
      if (at[a] < 0 || at[c] < 0) continue;
      double v1 = 0, v2 = 0;
      if (!cell(at[a], at[c], &v1) || !cell(at[c], at[a], &v2)) return false;
      mv.data[a][c] = 0.5 * (v1 + v2);
    }
  }
  mv.rownames = mb.colnames;
  mv.colnames = mb.colnames;
  for (auto* m : {&mb, &mv}) {
    const std::string nm = m == &mb ? "b" : "V";
    bool put = false;
    for (auto& kv : session.eval.eMatrices)
      if (kv.first == nm) {
        kv.second = *m;
        put = true;
      }
    if (!put) session.eval.eMatrices.emplace_back(nm, *m);
  }
  return true;
}

}  // namespace est_detail

// `ologit depvar indepvars [if] [, vce(robust|cluster V)]` and its probit twin.
ojson runOrdered(const std::string& cobraCmd, const eval::ParsedCommand& parsed,
                 SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  P7MleArgv b = buildP7MleArgv(cobraCmd, parsed, {"iid", "robust", "cluster"});
  if (b.errored) return b.errorBlock;
  b.argv.push_back("--emit-v");  // BUG-1196: the fit's e(V), posted below
  ojson raw;
  ojson block = runP7Mle(cobraCmd, b.argv, session, &raw);
  // BUG-1196: Stata's e(b) is the depvar's equation, then the cutpoints
  // (StataBE 19.5 `yo:x1 yo:x2 /cut1 /cut2`); e(V) is the fit's, in that order.
  if (raw.is_object() && raw.contains("e_V") && raw.contains("coefs") && raw.contains("cuts") &&
      raw["cuts"].is_array()) {
    std::vector<AncillaryEbColM4> cols;
    bool ok = depvarEqColsM4(parsed.varlist[0], raw["coefs"], "b", cols);
    for (const auto& c : raw["cuts"]) {
      if (!ok || !c.is_object() || !c.contains("name") || !c["name"].is_string() ||
          !c.contains("b") || !c["b"].is_number()) {
        ok = false;
        break;
      }
      const std::string nm = c["name"].get<std::string>();
      cols.push_back({nm, nm, c["b"].get<double>()});
    }
    if (ok) (void)postAncillaryEbM4(session, raw, cols);
  }
  // Stata prints the cutpoints in the SAME table as the coefficients, under a
  // /cut1, /cut2 … stanza. The engine emits them under their own `cuts` key
  // (they are not slopes and are excluded from the model Wald test), and
  // cobraToEstimationM4 carries only its known key set through — so without
  // this the rendered table had no cutpoints in it at all: an ordered model
  // missing exactly the parameters that make it ordered. Read from the RAW
  // payload for that reason. z and p stay null because Stata leaves those
  // columns blank for cuts.
  if (raw.contains("cuts") && raw["cuts"].is_array() && !raw["cuts"].empty()) {
    if (!block.contains("coeftable") || !block["coeftable"].is_array())
      block["coeftable"] = ojson::array();
    for (const auto& c : raw["cuts"]) {
      ojson row;
      row["name"] = c.value("name", "");
      row["b"] = c.contains("b") ? c["b"] : ojson(nullptr);
      row["se"] = c.contains("se") ? c["se"] : ojson(nullptr);
      row["t"] = nullptr;
      row["p"] = nullptr;
      row["ci_lo"] = c.contains("ci_lo") ? c["ci_lo"] : ojson(nullptr);
      row["ci_hi"] = c.contains("ci_hi") ? c["ci_hi"] : ojson(nullptr);
      block["coeftable"].push_back(row);
    }
  }
  return block;
}

// `mlogit depvar indepvars [if] [, baseoutcome(#) vce(...)]`.
ojson runMlogit(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  P7MleArgv b = buildP7MleArgv("mlogit", parsed, {"iid", "robust", "cluster"});
  if (b.errored) return b.errorBlock;
  for (const auto& [k, v] : parsed.opts) {
    if ((k == "baseoutcome" || k == "base") && !v.isBool) {
      b.argv.push_back("--base");
      b.argv.push_back(trimM4(v.strValue));
    }
  }
  b.argv.push_back("--emit-v");  // BUG-1196: the fit's e(V), posted below
  ojson raw;
  ojson block = runP7Mle("mlogit", b.argv, session, &raw);
  // BUG-1196: Stata's e(b) has one equation per outcome, ascending, named by the
  // outcome's value; the base outcome's columns are `o.` zeros, as are its rows
  // and columns of e(V) (StataBE 19.5 `1:o.x1 1:o.x2 1:o._cons 2:x1 2:x2 2:_cons
  // 3:x1 3:x2 3:_cons`). A labeled outcome, or one whose value is not a
  // nonnegative integer, names its equation otherwise; nothing is posted then.
  const std::string& y = parsed.varlist[0];
  const auto lbl = session.varLblname.find(y);
  if (raw.is_object() && raw.contains("e_V") && raw.contains("equations") &&
      raw["equations"].is_array() && (lbl == session.varLblname.end() || lbl->second.empty())) {
    std::vector<std::string> coefNames;  // a non-base equation's names, engine spelling
    for (const auto& e : raw["equations"])
      if (e.is_object() && !e.value("base", false) && e.contains("coefs") && e["coefs"].is_array()) {
        for (const auto& c : e["coefs"])
          if (c.is_object() && c.contains("name") && c["name"].is_string())
            coefNames.push_back(c["name"].get<std::string>());
        break;
      }
    std::vector<AncillaryEbColM4> cols;
    bool ok = !coefNames.empty();
    for (const auto& e : raw["equations"]) {
      const double v = e.is_object() && e.contains("outcome") && e["outcome"].is_number()
                           ? e["outcome"].get<double>()
                           : -1.0;
      if (!ok || !(v >= 0.0 && v < 1e9 && v == std::floor(v))) {
        ok = false;
        break;
      }
      char eq[64];
      std::snprintf(eq, sizeof(eq), "%.17g", v);  // the engine's `e_V` spelling of the outcome
      const bool isBase = e.value("base", false);
      ojson coefs = isBase ? ojson::array() : e.value("coefs", ojson::array());
      if (isBase)
        for (const auto& n : coefNames) coefs.push_back({{"name", n}, {"b", 0.0}});
      std::vector<AncillaryEbColM4> ec;  // `y:name`, re-keyed to the outcome's equation
      if (!depvarEqColsM4(y, coefs, "b", ec)) {
        ok = false;
        break;
      }
      for (auto& c : ec) {
        c.stata = std::string(eq) + (isBase ? ":o." : ":") + c.stata.substr(y.size() + 1);
        c.engine = isBase ? std::string() : std::string(eq) + ":" + c.engine;
        cols.push_back(std::move(c));
      }
    }
    if (ok) (void)postAncillaryEbM4(session, raw, cols);
  }
  return block;
}

// `zip`/`zinb depvar indepvars [if], inflate(...) [vce(robust)]`.
// inflate() is REQUIRED by Stata and only the constant-only form is implemented;
// a covariate list there would be a different model, so it rejects rather than
// quietly fitting the constant-only one.
ojson runZeroInflated(const std::string& cobraCmd, const eval::ParsedCommand& parsed,
                      SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  P7MleArgv b = buildP7MleArgv(cobraCmd, parsed, {"iid", "robust", "cluster"});
  if (b.errored) return b.errorBlock;
  std::optional<std::string> inflate;
  for (const auto& [k, v] : parsed.opts)
    if (k == "inflate" && !v.isBool) inflate = trimM4(v.strValue);
  if (!inflate.has_value()) {
    return engineErrorBlockM4("syntax_error: " + cobraCmd + " requires inflate()", "syntax_error");
  }
  if (*inflate != "_cons") {
    return engineErrorBlockM4(
        "Option `inflate(" + *inflate + ")` is a valid Stata option that this version of PIET does "
        "not implement for `" + cobraCmd + "` — only the constant-only inflation equation "
        "`inflate(_cons)` is available.",
        "unsupported_option");
  }
  b.argv.push_back("--inflate");
  b.argv.push_back("_cons");
  return runP7Mle(cobraCmd, b.argv, session);
}

// `fracreg logit depvar indepvars [if] [, vce(robust)]` — the link is a leading
// positional token, not an option.
ojson runFracreg(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  if (parsed.varlist.empty())
    return engineErrorBlockM4("syntax_error: fracreg requires a link and a dependent variable",
                              "syntax_error");
  const std::string link = lowerM4(trimM4(parsed.varlist[0]));
  if (link != "logit" && link != "probit")
    return engineErrorBlockM4("syntax_error: fracreg requires a link (logit or probit)",
                              "syntax_error");
  if (link == "probit")
    return engineErrorBlockM4(
        "`fracreg probit` is a valid Stata command that this version of PIET does not implement "
        "(only the logit link is available).",
        "unsupported_command");
  eval::ParsedCommand shifted = parsed;
  shifted.varlist.erase(shifted.varlist.begin());
  P7MleArgv b = buildP7MleArgv("fracreg", shifted, {"iid", "robust", "cluster"});
  if (b.errored) return b.errorBlock;
  b.argv.push_back("--link");
  b.argv.push_back("logit");
  pushLevelIfGivenM4(parsed, b.argv);
  return runP7Mle("fracreg", b.argv, session);
}

// ===========================================================================
// P7 — vector autoregression: `var`, `vargranger`, `irf create`
//
// ONE engine verb serves all three: it returns the per-equation results, the
// Granger block and the IRF cells in a single payload. `vargranger` is therefore
// a pure re-render of the stored fit — zero new mathematics on this side, which
// is the point — and `irf create` materializes the cells as a `.irf` file (which
// is a Stata `.dta`).

ojson runVar(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  // BUG-657: var.ado runs `tsset, noquery` -> `_ts`, whose refusal is "time
  // variable not set, use tsset varname ..." r(111) (the time-series helper); was
  // unsupported_option = r(198).
  if (!session.timevar.has_value() || session.timevar->empty())
    return ojson(eval::errorBlock("engine_error", "time variable not set, use tsset varname ...",
                                  std::nullopt, std::nullopt, std::nullopt, "r(111)"));
  if (parsed.varlist.size() < 2)
    return engineErrorBlockM4("syntax_error: var requires at least two endogenous variables",
                              "syntax_error");
  std::string lags = "1/2";
  for (const auto& [k, v] : parsed.opts) {
    if (k == "lags" && !v.isBool) lags = trimM4(v.strValue);
    else if ((k == "exog" || k == "constraints" || k == "dfk") && !v.isBool)
      return engineErrorBlockM4(
          "Option `" + k + "()` is a valid Stata option that this version of PIET does not "
          "implement for `var`.",
          "unsupported_option");
  }
  std::vector<std::string> argv = {"var", "--y", joinCommaM4(parsed.varlist), "--lags", lags,
                                   "--tsset", *session.timevar, "--irf-steps", "8"};
  if (parsed.ifExpr.has_value()) {
    argv.push_back("--keepif");
    argv.push_back(*parsed.ifExpr);
  }
  ojson raw;
  ojson block = runP7Mle("var", argv, session, &raw);
  // Keep the whole payload so `vargranger` and `irf create` can re-render it
  // without re-fitting — Stata's own model too is fit once and re-read.
  session.lastVar = raw;
  return block;
}

ojson runVargranger(SessionState& session) {
  if (!session.lastVar.has_value() || !(*session.lastVar).contains("granger"))
    return engineErrorBlockM4("vargranger: no VAR results in memory; run `var` first.",
                              "unsupported_option");
  const ojson& raw = *session.lastVar;
  ojson rows = ojson::array();
  for (const auto& g : raw["granger"]) {
    ojson r;
    r["equation"] = g.value("equation", "");
    r["excluded"] = g.value("excluded", "");
    r["chi2"] = g.contains("chi2") ? g["chi2"] : ojson(nullptr);
    r["df"] = g.contains("df") ? g["df"] : ojson(nullptr);
    r["p"] = g.contains("p") ? g["p"] : ojson(nullptr);
    rows.push_back(r);
  }
  ojson b;
  b["type"] = "vargranger";
  b["title"] = "Granger causality Wald tests";
  b["rows"] = rows;
  return b;
}

// `irf create NAME, step(#) set("PATH", replace)` — materializes the stored IRF
// cells as a Stata `.irf` file, which is an ordinary `.dta`.
ojson runIrf(const eval::ParsedCommand& parsed, SessionState& session) {
  if (parsed.varlist.empty() || lowerM4(trimM4(parsed.varlist[0])) != "create")
    return engineErrorBlockM4(
        "`irf` subcommands other than `create` are valid Stata commands that this version of PIET "
        "does not implement.",
        "unsupported_command");
  if (!session.lastVar.has_value() || !(*session.lastVar).contains("irf"))
    return engineErrorBlockM4("irf create: no VAR results in memory; run `var` first.",
                              "unsupported_option");
  const std::string irfname = parsed.varlist.size() > 1 ? parsed.varlist[1] : "irf1";
  std::string setPath;
  long steps = 8;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "set" && !v.isBool) {
      setPath = trimM4(v.strValue);
      // `set("path", replace)` — strip the option word and the quotes.
      size_t comma = setPath.find(',');
      if (comma != std::string::npos) setPath = trimM4(setPath.substr(0, comma));
      if (setPath.size() >= 2 && setPath.front() == '"' && setPath.back() == '"')
        setPath = setPath.substr(1, setPath.size() - 2);
    } else if (k == "step" && !v.isBool) {
      steps = std::strtol(trimM4(v.strValue).c_str(), nullptr, 10);
    }
  }
  if (setPath.empty())
    return engineErrorBlockM4("syntax_error: irf create requires set(\"filename\")",
                              "syntax_error");
  if (setPath.size() < 4 || setPath.substr(setPath.size() - 4) != ".irf") setPath += ".irf";

  // Build the cell table as CSV, convert it, and export it as the .dta the .irf
  // format actually is. PIET has no dataset writer of its own, so this reuses the
  // engine verbs rather than growing one.
  const ojson& cells = (*session.lastVar)["irf"]["cells"];
  static const char* kNumCols[] = {"irf",     "cirf",     "oirf",   "coirf",  "sirf",
                                   "dm",      "cdm",      "stdirf", "stdcirf", "stdoirf",
                                   "stdcoirf", "stdsirf", "stddm",  "stdcdm", "fevd",
                                   "sfevd",   "mse",      "stdfevd", "stdsfevd"};
  std::ostringstream csv;
  csv << "irfname,step";
  for (const char* c : kNumCols) csv << "," << c;
  csv << ",response,impulse\n";
  int64_t n_written = 0;
  for (const auto& c : cells) {
    if (c.value("step", 0) > steps) continue;
    csv << irfname << "," << c.value("step", 0);
    for (const char* k : kNumCols) {
      csv << ",";
      if (c.contains(k) && !c[k].is_null()) {
        char buf[40];
        std::snprintf(buf, sizeof(buf), "%.17g", c[k].get<double>());
        csv << buf;
      }
    }
    csv << "," << c.value("response", std::string()) << "," << c.value("impulse", std::string())
        << "\n";
    ++n_written;
  }
  const std::string csvPath = nextPathM4(session.workdir(), "irf_cells") + ".csv";
  {
    std::ofstream f(csvPath);
    if (!f) return engineErrorBlockM4("irf create: could not write the intermediate table",
                                      "data_io");
    f << csv.str();
  }
  const std::string hbinPath = nextPathM4(session.workdir(), "irf_frame");
  CobraRunResult cv = runCobraOneShot({"convert", "--data", csvPath, "--out", hbinPath,
                                       "--factor", "irfname,response,impulse"});
  if (!cv.ok)
    return engineErrorBlockM4("irf create: could not build the IRF table (" +
                                  (cv.stderr_text.empty() ? cv.error : cv.stderr_text) + ")",
                              "engine_error");
  CobraRunResult ex = runCobraOneShot({"export-dta", "--data", hbinPath, "--out", setPath});
  if (!ex.ok)
    return engineErrorBlockM4("irf create: could not write " + setPath + " (" +
                                  (ex.stderr_text.empty() ? ex.error : ex.stderr_text) + ")",
                              "engine_error");
  ojson b;
  b["type"] = "irf";
  b["subcommand"] = "create";
  b["irfname"] = irfname;
  b["file"] = setPath;
  b["n_rows"] = n_written;
  b["steps"] = steps;
  b["notes"] = ojson::array({"(file " + setPath + " created)", "(file " + setPath + " now active)",
                             "(file " + setPath + " updated)"});
  return b;
}

// ===========================================================================
// P7 — multiple imputation: `mi set`, `mi register`, `mi impute regress`
//
// The mlong ROWS are real data: the engine writes the whole 4,800 + M×129 frame
// itself (`--out-data`), because it already holds both the source frame and the
// draws — reassembling them here would mean round-tripping the values back
// through a CSV merge, which is more moving parts for the same result and a
// second place for the row order to drift. SessionState keeps the declaration
// only, so `mi impute` can reject honestly on un-set or unregistered data.

ojson runMi(const eval::ParsedCommand& parsed, SessionState& session) {
  if (parsed.varlist.empty())
    return engineErrorBlockM4("syntax_error: mi requires a subcommand", "syntax_error");
  const std::string sub = lowerM4(trimM4(parsed.varlist[0]));

  if (sub == "set") {
    if (parsed.varlist.size() < 2)
      return engineErrorBlockM4("syntax_error: mi set requires a style", "syntax_error");
    const std::string style = lowerM4(trimM4(parsed.varlist[1]));
    if (style != "mlong")
      return engineErrorBlockM4(
          "`mi set " + style + "` is a valid Stata style that this version of PIET does not "
          "implement (only `mlong` is available).",
          "unsupported_command");
    SessionState::MiState st;
    st.style = "mlong";
    session.miState = st;
    ojson b;
    b["type"] = "mi";
    b["subcommand"] = "set";
    b["style"] = "mlong";
    return b;
  }

  if (sub == "register") {
    if (!session.miState.has_value())
      return engineErrorBlockM4("mi register: the data are not mi set; run `mi set mlong` first.",
                                "unsupported_option");
    if (parsed.varlist.size() < 3)
      return engineErrorBlockM4("syntax_error: mi register requires a class and a varlist",
                                "syntax_error");
    const std::string cls = lowerM4(trimM4(parsed.varlist[1]));
    if (cls != "imputed")
      return engineErrorBlockM4(
          "`mi register " + cls + "` is a valid Stata class that this version of PIET does not "
          "implement (only `imputed` is available).",
          "unsupported_command");
    if (!session.currentHbin().has_value()) return eval::noDataLoaded();
    std::vector<std::string> vars(parsed.varlist.begin() + 2, parsed.varlist.end());
    for (const auto& v : vars) session.miState->imputed.push_back(v);
    // Count the rows this marks incomplete — the number Stata prints. Uses the
    // ordinary count path so there is no second missing-value rule here.
    // `eval --summarize` reports N = the NON-missing count per variable, plus the
    // dataset's own row count; the incomplete count Stata prints is the
    // difference. Derived rather than assumed — reading a nonexistent
    // "n_missing" key silently produced 0 here, which is the shape of a count
    // that looks reported but never was.
    int64_t marked = 0;
    for (const auto& v : vars) {
      std::vector<std::string> argv = {"eval", "--data", *session.currentHbin(), "--summarize", v};
      try {
        ojson raw = runCobraJsonM4(argv);
        if (!raw.contains("n_rows") || !raw.contains("summarize") ||
            !raw["summarize"].contains(v) || !raw["summarize"][v].contains("N"))
          return engineErrorBlockM4(
              "mi register: could not determine how many observations of `" + v + "` are missing",
              "engine_error");
        marked += raw["n_rows"].get<int64_t>() - raw["summarize"][v]["N"].get<int64_t>();
      } catch (const CobraError& ce) {
        return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
      }
    }
    ojson b;
    b["type"] = "mi";
    b["subcommand"] = "register";
    b["class"] = "imputed";
    b["vars"] = vars;
    b["n_marked_incomplete"] = marked;
    return b;
  }

  if (sub == "impute") {
    if (!session.currentHbin().has_value()) return eval::noDataLoaded();
    if (!session.miState.has_value())
      return engineErrorBlockM4("mi impute: the data are not mi set; run `mi set mlong` first.",
                                "unsupported_option");
    if (parsed.varlist.size() < 2)
      return engineErrorBlockM4("syntax_error: mi impute requires a method", "syntax_error");
    const std::string method = lowerM4(trimM4(parsed.varlist[1]));
    if (method != "regress")
      return engineErrorBlockM4(
          "`mi impute " + method + "` is a valid Stata method that this version of PIET does not "
          "implement (only `regress` is available).",
          "unsupported_command");
    // `mi impute regress Y = X1 X2` — the tokenizer leaves `=` its own token.
    std::vector<std::string> rest(parsed.varlist.begin() + 2, parsed.varlist.end());
    size_t eq = rest.size();
    for (size_t i = 0; i < rest.size(); ++i)
      if (rest[i] == "=") { eq = i; break; }
    if (eq == rest.size() || eq == 0 || eq + 1 >= rest.size())
      return engineErrorBlockM4("syntax_error: mi impute regress requires `Y = X1 X2`",
                                "syntax_error");
    if (eq != 1)
      return engineErrorBlockM4(
          "`mi impute regress` with more than one imputed variable is a valid Stata form that this "
          "version of PIET does not implement.",
          "unsupported_command");
    const std::string yv = rest[0];
    const std::vector<std::string> xv(rest.begin() + static_cast<long>(eq) + 1, rest.end());
    const auto& reg = session.miState->imputed;
    if (std::find(reg.begin(), reg.end(), yv) == reg.end())
      return engineErrorBlockM4(
          "mi impute regress: `" + yv + "` is not registered as imputed; run "
          "`mi register imputed " + yv + "` first.",
          "unsupported_option");

    std::string add = "5", seed;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "add" && !v.isBool) add = trimM4(v.strValue);
      else if (k == "rseed" && !v.isBool) seed = trimM4(v.strValue);
      else if (k == "update" && !v.isBool)
        return engineErrorBlockM4(
            "Option `update()` is a valid Stata option that this version of PIET does not "
            "implement for `mi impute` (only `add()` is available).",
            "unsupported_option");
    }
    std::string outPath = nextPathM4(session.workdir(), "mi_mlong");
    std::vector<std::string> argv = {"mi-impute-regress", "--data", *session.currentHbin(),
                                     "--y", yv, "--x", joinCommaM4(xv), "--add", add,
                                     "--out-data", outPath};
    if (!seed.empty()) { argv.push_back("--seed"); argv.push_back(seed); }
    // ONE-SHOT deliberately, not the session-routed runner: --out-data writes a
    // dataset, and under the resident-frame serve path commit_frame STAGES the
    // frame instead of writing the file — so the file PIET then loads would never
    // exist. (`--out-data` is not in that path's --data/--out guard, so it took
    // the staging branch silently; measured here as "cannot open <mi_mlong_...>").
    CobraRunResult cr = runCobraOneShot(argv);
    if (!cr.ok) {
      // Prefer the engine's own error envelope when it produced one; fall back to
      // stderr so a launch failure is never reported as an empty message.
      std::string msg, code = "engine_error";
      try {
        ojson e = ojson::parse(cr.stdout_text);
        if (e.contains("error")) {
          msg = e["error"].value("message", std::string());
          code = e["error"].value("code", std::string("engine_error"));
        }
      } catch (const std::exception&) {
      }
      if (msg.empty()) msg = cr.stderr_text.empty() ? cr.error : cr.stderr_text;
      return engineErrorBlockM4("mi impute regress: " + msg, code);
    }
    ojson raw;
    try {
      raw = ojson::parse(cr.stdout_text);
    } catch (const std::exception&) {
      return engineErrorBlockM4("mi impute regress: could not parse the engine response",
                                "engine_error");
    }
    session.load(outPath);
    session.miState->m += static_cast<int>(std::stol(add));
    ojson block = raw;
    block["type"] = "mi";
    block["subcommand"] = "impute";
    block["method"] = "regress";
    return block;
  }

  return engineErrorBlockM4(
      "`mi " + sub + "` is a valid Stata subcommand that this version of PIET does not implement "
      "(set, register and impute regress are available).",
      "unsupported_command");
}

// ===========================================================================
// P7 — dynamic panel: `xtabond depvar indepvars, lags(#) vce(robust)`
//
// Needs the xtset panel context, which the engine takes as `--xtset id,t` (the
// same channel xtreg uses). Only the one-step estimator exists; two-step and the
// conventional (non-robust) GMM VCE reject rather than quietly returning
// one-step numbers under another name.

ojson runXtabond(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  // BUG-657: Stata's `_xt, trequired` refusals (the texts of _xt.ado's
  // time-required branch, both r(459)); was unsupported_option = r(198).
  if (!session.panelvar.has_value() || session.panelvar->empty())
    return ojson(eval::errorBlock("engine_error", "must specify panelvar and timevar; use xtset",
                                  std::nullopt, std::nullopt, std::nullopt, "r(459)"));
  if (!session.timevar.has_value() || session.timevar->empty())
    return ojson(eval::errorBlock("engine_error", "must specify timevar; use xtset", std::nullopt,
                                  std::nullopt, std::nullopt, "r(459)"));
  if (parsed.varlist.size() < 2)
    return engineErrorBlockM4(
        "syntax_error: xtabond requires a dependent variable and at least one regressor",
        "syntax_error");
  std::vector<std::string> argv = {
      "xtabond", "--y", parsed.varlist[0], "--x",
      joinCommaM4(std::vector<std::string>(parsed.varlist.begin() + 1, parsed.varlist.end())),
      "--xtset", *session.panelvar + "," + *session.timevar};
  if (parsed.ifExpr.has_value()) {
    argv.push_back("--keepif");
    argv.push_back(*parsed.ifExpr);
  }
  std::string lags = "1";
  for (const auto& [k, v] : parsed.opts) {
    if (k == "lags" && !v.isBool) lags = trimM4(v.strValue);
    else if (k == "twostep" && v.isBool && v.boolValue)
      return engineErrorBlockM4(
          "Option `twostep` is a valid Stata option that this version of PIET does not implement "
          "for `xtabond` (only the one-step estimator is available).",
          "unsupported_option");
  }
  argv.push_back("--lags");
  argv.push_back(lags);
  // BUG-383 #5: the engine now implements BOTH one-step VCEs, and Stata's bare
  // `xtabond` is the CONVENTIONAL one — the old code here hardcoded --vce
  // robust for every call, which kept the bare form red against the capture
  // even after the engine grew the right default. Forward what the user asked:
  // nothing -> engine default (conventional); vce(robust) -> robust.
  std::string vceReq;  // empty = let the engine default (conventional) stand
  for (const auto& [k, v] : parsed.opts) {
    if ((k == "vce" || k == "vcov") && !v.isBool) {
      std::string lv = lowerM4(trimM4(v.strValue));
      if (lv == "robust") {
        vceReq = "robust";
        continue;
      }
      if (lv != "robust")
        return engineErrorBlockM4(
            "Option `vce(" + lv + ")` is a valid Stata option that this version of PIET does not "
            "implement for `xtabond` (conventional [the default] and vce(robust) are available).",
            "unsupported_option");
    }
  }
  if (!vceReq.empty()) {
    argv.push_back("--vce");
    argv.push_back(vceReq);
  }
  return runP7Mle("xtabond", argv, session);
}

// ===========================================================================
// P7 — nonlinear: `nl (y = EXPR)` and `gmm (RESID), instruments(...) onestep`
//
// The parenthesised expression is passed to the engine VERBATIM. PIET does not
// parse it: the engine owns the one expression evaluator (promoted out of
// nlcom), and a second parse here is exactly the shadow-evaluator shape that
// makes two layers disagree quietly. Macro expansion has already happened
// upstream, which is correct — Stata expands macros inside nl expressions too.

ojson runNlGmm(const std::string& cobraCmd, const std::string& rawLine,
               const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  // Take the expression straight off the raw line: the tokenizer splits on
  // whitespace, so rejoining varlist tokens would silently normalise the
  // spacing inside the user's own formula.
  std::string s = trimM4(rawLine);
  size_t lp = s.find('(');
  if (lp == std::string::npos)
    return engineErrorBlockM4("syntax_error: " + cobraCmd + " requires a parenthesised expression",
                              "syntax_error");
  int depth = 0;
  size_t rp = std::string::npos;
  for (size_t i = lp; i < s.size(); ++i) {
    if (s[i] == '(') ++depth;
    else if (s[i] == ')' && --depth == 0) { rp = i; break; }
  }
  if (rp == std::string::npos)
    return engineErrorBlockM4("syntax_error: " + cobraCmd + " expression has no closing ')'",
                              "syntax_error");
  std::vector<std::string> argv = {cobraCmd, "--expr", s.substr(lp, rp - lp + 1)};
  if (parsed.ifExpr.has_value()) {
    argv.push_back("--keepif");
    argv.push_back(*parsed.ifExpr);
  }
  if (cobraCmd == "gmm") {
    std::string instruments;
    bool onestep = false;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "instruments" && !v.isBool) {
        std::istringstream iss(v.strValue);
        std::vector<std::string> toks;
        std::string t;
        while (iss >> t) toks.push_back(t);
        instruments = joinCommaM4(toks);
      }
      else if (k == "onestep" && v.isBool && v.boolValue) onestep = true;
      else if (k == "twostep" && v.isBool && v.boolValue)
        return engineErrorBlockM4(
            "Option `twostep` is a valid Stata option that this version of PIET does not implement "
            "for `gmm` (only the one-step estimator is available).",
            "unsupported_option");
    }
    if (instruments.empty())
      return engineErrorBlockM4("syntax_error: gmm requires instruments()", "syntax_error");
    argv.push_back("--instruments");
    argv.push_back(instruments);
    if (!onestep)
      return engineErrorBlockM4(
          "`gmm` without `onestep` (two-step GMM) is a valid Stata command that this version of "
          "PIET does not implement.",
          "unsupported_command");
    argv.push_back("--onestep");
  }
  for (const auto& [k, v] : parsed.opts) {
    if ((k == "vce" || k == "robust") && cobraCmd == "nl")
      return engineErrorBlockM4(
          "Option `vce()` is a valid Stata option that this version of PIET does not implement for "
          "`nl` (the small-sample factor is not fixed by any oracle available here).",
          "unsupported_option");
  }
  // [aw=]/[pw=] (BUG-1283): the engine's weighted NLS -- aweights normalized to N
  // with the model-based VCE, pweights the robust sandwich (StataBE 19.5 _se[/b1]
  // .067819537723433465 / .071775268345285762). Stata notes the weight sum first.
  const bool weighted = cobraCmd == "nl" && parsed.weightVar.has_value() &&
                        !parsed.weightVar->empty();
  if (weighted) {
    const std::string wt = lowerM4(parsed.weightType.value_or("aw"));
    argv.push_back("--weight");
    argv.push_back(*parsed.weightVar);
    const bool pw = wt == "pw" || wt == "pweight";
    argv.push_back("--wtype");
    argv.push_back(pw ? "pw" : "aw");
    if (pw) {  // e(vce) robust, e(vcetype) Robust, as Stata posts under a pweight
      argv.push_back("--vce");
      argv.push_back("robust");
    }
  }
  pushLevelIfGivenM4(parsed, argv);
  ojson raw;
  ojson block = runP7Mle(cobraCmd, argv, session, &raw);
  if (!weighted || block.value("type", std::string()) == "error" || !raw.contains("weight") ||
      !raw["weight"].is_object() || !raw["weight"].contains("sum_w") ||
      !raw["weight"]["sum_w"].is_number())
    return block;
  char buf[64];
  std::snprintf(buf, sizeof buf, "(sum of wgt is %.16g)", raw["weight"]["sum_w"].get<double>());
  ojson note;
  note["type"] = "display";
  note["text"] = buf;
  return ojson::array({note, block});
}

// ===========================================================================
// P7 — multilevel: `mixed depvar indepvars || GROUP:, vce(robust)`
//
// Only the random-INTERCEPT form is implemented. A random-slope list after the
// colon, or a second `||` level, is a different model and rejects rather than
// being silently fitted as an intercept-only one.

ojson runMixed(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  // splitWs leaves `||` and `id:` as ordinary tokens.
  size_t bar = parsed.varlist.size();
  for (size_t i = 0; i < parsed.varlist.size(); ++i)
    if (parsed.varlist[i] == "||") { bar = i; break; }
  if (bar == parsed.varlist.size())
    return engineErrorBlockM4(
        "syntax_error: mixed requires a random-effects part, e.g. `mixed y x1 x2 || id:`",
        "syntax_error");
  if (bar < 2)
    return engineErrorBlockM4(
        "syntax_error: mixed requires a dependent variable and at least one regressor before `||`",
        "syntax_error");
  std::vector<std::string> after(parsed.varlist.begin() + static_cast<long>(bar) + 1,
                                 parsed.varlist.end());
  if (after.empty())
    return engineErrorBlockM4("syntax_error: mixed requires a grouping variable after `||`",
                              "syntax_error");
  for (const auto& t : after)
    if (t == "||")
      return engineErrorBlockM4(
          "`mixed` with more than one random-effects level is a valid Stata model that this "
          "version of PIET does not implement (only a single random intercept is available).",
          "unsupported_command");
  std::string grp = after[0];
  bool sawColon = false;
  if (!grp.empty() && grp.back() == ':') { grp.pop_back(); sawColon = true; }
  if (!sawColon)
    return engineErrorBlockM4("syntax_error: mixed's grouping variable must be followed by `:`",
                              "syntax_error");
  if (after.size() > 1)
    return engineErrorBlockM4(
        "`mixed` with random slopes after `|| " + grp + ":` is a valid Stata model that this "
        "version of PIET does not implement (only a random intercept is available).",
        "unsupported_command");

  eval::ParsedCommand fixedPart = parsed;
  fixedPart.varlist.assign(parsed.varlist.begin(), parsed.varlist.begin() + static_cast<long>(bar));
  P7MleArgv b = buildP7MleArgv("mixed", fixedPart, {"iid", "robust", "cluster"});
  if (b.errored) return b.errorBlock;
  for (const auto& [k, v] : parsed.opts)
    if (k == "reml" && v.isBool && v.boolValue)
      return engineErrorBlockM4(
          "Option `reml` is a valid Stata option that this version of PIET does not implement for "
          "`mixed` (the estimator fits by maximum likelihood).",
          "unsupported_option");
  b.argv.push_back("--group");
  b.argv.push_back(grp);
  pushLevelIfGivenM4(parsed, b.argv);
  ojson raw;
  ojson block = runP7Mle("mixed", b.argv, session, &raw);
  // The random-effects stanza (var(_cons), var(Residual)) is a table Stata always
  // prints below the coefficients; like the ordered models' cutpoints it is not a
  // slope, so the mapper does not carry it and it is appended from the raw
  // payload. z/p stay null — Stata leaves those columns blank for variances.
  if (raw.contains("random_effects") && raw["random_effects"].is_array()) {
    if (!block.contains("coeftable") || !block["coeftable"].is_array())
      block["coeftable"] = ojson::array();
    for (const auto& re : raw["random_effects"]) {
      ojson row;
      row["name"] = re.value("name", "");
      row["b"] = re.contains("b") ? re["b"] : ojson(nullptr);
      row["se"] = re.contains("se") ? re["se"] : ojson(nullptr);
      row["t"] = nullptr;
      row["p"] = nullptr;
      row["ci_lo"] = re.contains("ci_lo") ? re["ci_lo"] : ojson(nullptr);
      row["ci_hi"] = re.contains("ci_hi") ? re["ci_hi"] : ojson(nullptr);
      block["coeftable"].push_back(row);
    }
  }
  return block;
}

// ===========================================================================
// P7 — survey design: `svyset` and the `svy:` prefix
//
// The design is SESSION state (as it is in Stata), so the engine stays
// stateless and each `svy:` command is rewritten into the ordinary estimation
// call plus --svy --psu --strata and the design's pweight.
//
// The inner-command whitelist fails CLOSED. An unrecognised inner command must
// reject, never fall through to a plain unweighted dispatch — that would return
// a confident number computed under no survey design at all, which is precisely
// the silent-wrong shape this codebase refuses to ship.

ojson runSvyset(const eval::ParsedCommand& parsed, SessionState& session) {
  if (parsed.varlist.empty() && !parsed.weightVar.has_value()) {
    // Bare `svyset` re-displays the current design (Stata's own behaviour).
    if (!session.svyDesign.has_value())
      return engineErrorBlockM4("svyset: no survey design is currently set.", "unsupported_option");
  }
  for (const auto& [k, v] : parsed.opts) {
    if (k == "clear" && v.isBool && v.boolValue) {
      session.svyDesign.reset();
      ojson b;
      b["type"] = "svyset";
      b["cleared"] = true;
      return b;
    }
  }
  if (!parsed.varlist.empty() || parsed.weightVar.has_value()) {
    if (parsed.varlist.size() != 1)
      return engineErrorBlockM4("syntax_error: svyset takes exactly one sampling-unit variable",
                                "syntax_error");
    if (!parsed.weightVar.has_value())
      return engineErrorBlockM4(
          "svyset: this version of PIET requires a probability weight, e.g. "
          "`svyset PSU [pweight=W], strata(S)`.",
          "unsupported_option");
    std::string wt = parsed.weightType.value_or("pweight");
    if (wt != "pweight" && wt != "pw")
      return engineErrorBlockM4(
          "svyset: only [pweight=] is supported for a survey design (got [" + wt + "=]).",
          "unsupported_option");
    std::string strata;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "strata" && !v.isBool) strata = trimM4(v.strValue);
      else if ((k == "fpc" || k == "vce" || k == "singleunit" || k == "poststrata") && !v.isBool)
        return engineErrorBlockM4(
            "Option `" + k + "()` is a valid Stata option that this version of PIET does not "
            "implement for `svyset`.",
            "unsupported_option");
    }
    // No strata() is ONE stratum, as in Stata (w2t_boottest_family.log:2455 H1:
    // `svyset id [pweight=w]` prints "Strata 1: <one>" and `svy: regress` fits
    // with "Number of strata = 1"). d.strata stays empty; runSvyPrefix then
    // sends no --strata and the engine's feols uses the constant stratum.
    SessionState::SvySettings d;
    d.psu = parsed.varlist[0];
    d.pweight = *parsed.weightVar;
    d.strata = strata;
    session.svyDesign = d;
  }
  const auto& d = *session.svyDesign;
  ojson b;
  b["type"] = "svyset";
  b["pweight"] = d.pweight;
  b["vce"] = "linearized";
  b["strata1"] = d.strata.empty() ? std::string("<one>") : d.strata;
  b["su1"] = d.psu;
  b["fpc1"] = nullptr;
  return b;
}

// `svy: CMD ...` — rewrite into the ordinary engine call plus the design flags.
ojson runSvyPrefix(const std::string& innerRaw, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  if (!session.svyDesign.has_value())
    return engineErrorBlockM4(
        "svy: the data have not been `svyset`; run `svyset PSU [pweight=W], strata(S)` first.",
        "unsupported_option");
  const auto& d = *session.svyDesign;

  eval::ParsedCommand p;
  try {
    p = eval::tokenize(innerRaw);
  } catch (const std::exception&) {
    return engineErrorBlockM4("syntax_error: could not parse the command after `svy:`",
                              "syntax_error");
  }
  const std::string inner = lowerM4(trimM4(p.cmd));
  if (p.weightVar.has_value())
    return engineErrorBlockM4(
        "svy: weights may not be given on the command line — the design's [pweight=] applies.",
        "unsupported_option");

  std::vector<std::string> argv;
  if (inner == "regress" || inner == "reg" || inner == "r" || inner == "feols") {
    if (p.varlist.size() < 2)
      return engineErrorBlockM4("syntax_error: svy: regress requires a dependent variable and at "
                                "least one regressor", "syntax_error");
    argv = {"feols", "--y", p.varlist[0], "--x",
            joinCommaM4(std::vector<std::string>(p.varlist.begin() + 1, p.varlist.end()))};
  } else if (inner == "mean") {
    if (p.varlist.empty())
      return engineErrorBlockM4("syntax_error: svy: mean requires at least one variable",
                                "syntax_error");
    argv = {"mean", "--vars", joinCommaM4(p.varlist)};
  } else if (inner == "logit" || inner == "logistic") {
    if (p.varlist.size() < 2)
      return engineErrorBlockM4("syntax_error: svy: logit requires a dependent variable and at "
                                "least one regressor", "syntax_error");
    argv = {"logit", "--y", p.varlist[0], "--x",
            joinCommaM4(std::vector<std::string>(p.varlist.begin() + 1, p.varlist.end()))};
  } else {
    return engineErrorBlockM4(
        "`svy: " + p.cmd + "` is a valid Stata construction that this version of PIET does not "
        "implement (the survey prefix covers regress, mean and logit).",
        "unsupported_command");
  }
  if (p.ifExpr.has_value()) {
    argv.push_back("--keepif");
    argv.push_back(*p.ifExpr);
  }
  argv.push_back("--weight");
  argv.push_back(d.pweight);
  argv.push_back("--wtype");
  argv.push_back("pw");
  argv.push_back("--svy");
  argv.push_back("--psu");
  argv.push_back(d.psu);
  // A design without strata() is one stratum for every svy verb the engine
  // serves (BUG-1424: its mean and logit paths take it, as feols did).
  if (!d.strata.empty()) {
    argv.push_back("--strata");
    argv.push_back(d.strata);
  }
  // e(cmd) after `svy: regress` is `regress` (w2t_boottest_family.log:2497
  // H1_svy ecmd=regress); the engine's verb stays the cobraCmd every replay
  // keys on. mean and logit keep publishing the engine's word as before.
  ojson raw;
  ojson block = runP7MleAsM4(argv[0], argv, session, &raw,
                             argv[0] == "feols" ? std::string("regress") : std::string());
  // FU-471: as in runLogitProbit, the logit reply's `df_m` stays on the cached
  // reply (e(df_m)) and off the block, which never carried it.
  if (argv[0] == "logit" && block.is_object()) block.erase("df_m");
  // The design's counts from the reply's svy block (BUG-1424: StataBE e(N_psu) 20,
  // e(N_strata) 1, e(df_r) 19 after `svy: mean` / `svy: logit` on a one-stratum design).
  if (block.value("type", std::string()) != "error" && raw.is_object() && raw.contains("svy") &&
      raw["svy"].is_object()) {
    const ojson& sv = raw["svy"];
    for (const auto& [key, name] : std::vector<std::pair<const char*, const char*>>{
             {"n_psu", "N_psu"}, {"n_strata", "N_strata"}, {"design_df", "df_r"},
             {"pop_size", "N_pop"}})
      if (sv.contains(key) && sv[key].is_number())
        putEScalarNL(session, name, sv[key].get<double>());
    // BUG-1513 (StataBE 19.5, one stratum and strata(s)): `svy: regress` e(df_m) 1, the
    // numerator dof of the design-adjusted F the reply sends; e(prefix) svy; e(title)
    // "Survey: " + the command's own title ("Linear regression", "Mean estimation",
    // "Logistic regression" after svy: logit and svy: logistic). e(vce)/e(vcetype)
    // linearized/Linearized come with the model (cacheLastModelM4, setLastModel).
    if (argv[0] == "feols" && sv.contains("f_df1") && sv["f_df1"].is_number())
      putEScalarNL(session, "df_m", sv["f_df1"].get<double>());
    putEStringNL(session, "prefix", "svy");
    putEStringNL(session, "title", argv[0] == "feols"  ? "Survey: Linear regression"
                                   : argv[0] == "mean" ? "Survey: Mean estimation"
                                                       : "Survey: Logistic regression");
  }
  return block;
}

// ===========================================================================
// P7 — survival: stset / stcox / streg
//
// `stset` is an ENGINE verb, not PIET state: it materializes `_t0/_t/_d/_st` as
// real columns, which is data computation, and every later st command then reads
// them as ordinary variables — the same arrangement Stata has. What PIET keeps is
// the DECLARATION (session.stContext), which is what lets stcox/streg reject
// honestly on un-stset data and lets `vce(robust)` cluster on the subject id.

ojson runStset(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  if (parsed.varlist.size() != 1)
    return engineErrorBlockM4("syntax_error: stset requires exactly one time variable",
                              "syntax_error");
  std::string failvar, idvar;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "failure" && !v.isBool) {
      // Stata allows `failure(fail)`, `failure(fail==1)` and `failure(fail!=0)`;
      // only the bare-variable form is implemented, and the comparison forms
      // reject rather than being silently read as the bare one (they select a
      // DIFFERENT failure event).
      std::string fv = trimM4(v.strValue);
      if (fv.find_first_of("=<>!") != std::string::npos)
        return engineErrorBlockM4(
            "Option `failure(" + fv + ")` is a valid Stata option that this version of PIET does "
            "not implement for `stset` — only the bare `failure(varname)` form is available.",
            "unsupported_option");
      failvar = fv;
    } else if (k == "id" && !v.isBool) {
      idvar = trimM4(v.strValue);
    }
  }
  if (failvar.empty())
    return engineErrorBlockM4("syntax_error: stset requires failure(varname)", "syntax_error");

  std::vector<std::string> argv = {"stset", "--data", *session.currentHbin(), "--time",
                                   parsed.varlist[0], "--failure", failvar};
  if (!idvar.empty()) {
    argv.push_back("--id");
    argv.push_back(idvar);
  }
  std::string outPath = nextPathM4(session.workdir(), "stset");
  argv.push_back("--out");
  argv.push_back(outPath);

  ojson raw;
  try {
    raw = runCobraJsonM4(argv);
  } catch (const CobraError& ce) {
    return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
  }
  session.load(outPath);
  SessionState::StContext ctx;
  ctx.timevar = parsed.varlist[0];
  ctx.failvar = failvar;
  ctx.idvar = idvar;
  session.stContext = ctx;

  ojson block = raw;
  block["type"] = "stset";
  return block;
}

namespace {

// Shared entry checks + argv prefix for stcox/streg.
P7MleArgv buildStArgv(const std::string& cobraCmd, const eval::ParsedCommand& parsed,
                      SessionState& session) {
  P7MleArgv r;
  if (!session.stContext.has_value()) {
    r.errored = true;
    r.errorBlock = engineErrorBlockM4(
        cobraCmd + ": the data have not been `stset`; run `stset TIMEVAR, failure(FAILVAR)` first.",
        "unsupported_option");
    return r;
  }
  if (parsed.varlist.empty()) {
    r.errored = true;
    r.errorBlock = engineErrorBlockM4("syntax_error: " + cobraCmd + " requires at least one regressor",
                                      "syntax_error");
    return r;
  }
  r.argv = {cobraCmd, "--x", joinCommaM4(parsed.varlist)};
  if (parsed.ifExpr.has_value()) {
    r.argv.push_back("--keepif");
    r.argv.push_back(*parsed.ifExpr);
  }
  // vce. Stata's own `vce(robust)` on multi-record st data with an id() is
  // subject-clustered, so PIET supplies the id here rather than letting the
  // engine compute a record-level sandwich that would miss the oracle on the
  // second digit.
  if (auto shapeErr = vceOptionShapeErrorM4(parsed)) {
    r.errored = true;
    r.errorBlock = engineErrorBlockM4(*shapeErr, "unsupported_option");
    return r;
  }
  std::optional<std::string> vceVal, clusterOpt;
  bool robustFlag = false;
  for (const auto& [k, v] : parsed.opts) {
    if ((k == "vce" || k == "vcov") && !vceVal.has_value())
      vceVal = v.isBool ? std::string() : v.strValue;
    else if (k == "robust" && v.isBool && v.boolValue)
      robustFlag = true;
    else if (k == "cluster" && !v.isBool)
      clusterOpt = v.strValue;
  }
  std::string want, cvar = clusterOpt.value_or("");
  if (vceVal.has_value() && !vceVal->empty()) {
    std::string lv = lowerM4(*vceVal);
    if (lv.rfind("cluster", 0) == 0) {
      want = "cluster";
      if (lv.size() > 7) {
        std::string rest = trimM4(lv.substr(7));
        if (!rest.empty()) cvar = rest;
      }
    } else {
      want = lv;
    }
  } else if (!cvar.empty()) {
    // BUG-537: cluster() beside `robust` wins (w2_pweight.log R5), as in
    // vcovFromOpts / buildP7MleArgv.
    want = "cluster";
  } else if (robustFlag) {
    want = "robust";
  }
  if (want.empty()) return r;
  if (want != "robust" && want != "cluster" && want != "iid") {
    r.errored = true;
    r.errorBlock = engineErrorBlockM4(
        "Option `vce(" + want + ")` is a valid Stata option that this version of PIET does not "
        "implement for `" + cobraCmd + "`.", "unsupported_option");
    return r;
  }
  if (want == "cluster" && cvar.empty()) {
    r.errored = true;
    r.errorBlock = engineErrorBlockM4("Cluster variable required: vce(cluster varname)",
                                      "unsupported_option");
    return r;
  }
  r.argv.push_back("--vce");
  r.argv.push_back(want);
  if (want == "cluster") {
    r.argv.push_back("--cluster");
    r.argv.push_back(cvar);
  } else if (want == "robust" && !session.stContext->idvar.empty()) {
    r.argv.push_back("--cluster");
    r.argv.push_back(session.stContext->idvar);
  }
  return r;
}

}  // namespace

namespace {

// Stata's stcox/streg print HAZARD RATIOS by default, with the z, p and CI still
// taken from the RAW coefficient scale. The engine keeps `b`/`se` in the raw
// metric like every other estimator and carries `hr`/`se_hr`/`hr_ci_lo`/
// `hr_ci_hi` alongside — which is right for the contract, but the shared
// tidy_coeftable only copies the seven standard fields, so the rendered table
// showed the raw coefficient where Stata shows the ratio. This re-points the
// table at the hazard-ratio columns (from the RAW payload, since the mapper does
// not carry them through) and keeps the raw coefficient available under `b_raw`.
void applyHazardRatioTableM4(ojson& block, const ojson& raw) {
  if (!raw.contains("coefs") || !raw["coefs"].is_array()) return;
  if (!block.contains("coeftable") || !block["coeftable"].is_array()) return;
  std::map<std::string, const ojson*> byName;
  for (const auto& c : raw["coefs"])
    if (c.contains("name") && c.contains("hr")) byName[c["name"].get<std::string>()] = &c;
  for (auto& row : block["coeftable"]) {
    auto it = byName.find(row.value("name", std::string()));
    if (it == byName.end()) continue;
    const ojson& c = *it->second;
    row["b_raw"] = row.contains("b") ? row["b"] : ojson(nullptr);
    row["se_raw"] = row.contains("se") ? row["se"] : ojson(nullptr);
    row["b"] = c["hr"];
    if (c.contains("se_hr")) row["se"] = c["se_hr"];
    if (c.contains("hr_ci_lo")) row["ci_lo"] = c["hr_ci_lo"];
    if (c.contains("hr_ci_hi")) row["ci_hi"] = c["hr_ci_hi"];
  }
  block["coef_metric"] = "hazard_ratio";
}

}  // namespace

ojson runStcox(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  P7MleArgv b = buildStArgv("stcox", parsed, session);
  if (b.errored) return b.errorBlock;
  ojson raw;
  ojson block = runP7Mle("stcox", b.argv, session, &raw);
  applyHazardRatioTableM4(block, raw);
  return block;
}

ojson runStreg(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  P7MleArgv b = buildStArgv("streg", parsed, session);
  if (b.errored) return b.errorBlock;
  std::string dist;
  for (const auto& [k, v] : parsed.opts)
    if ((k == "distribution" || k == "dist" || k == "d") && !v.isBool) dist = lowerM4(trimM4(v.strValue));
  if (dist.empty())
    return engineErrorBlockM4("syntax_error: streg requires distribution()", "syntax_error");
  if (dist != "weibull")
    return engineErrorBlockM4(
        "Option `distribution(" + dist + ")` is a valid Stata option that this version of PIET does "
        "not implement for `streg` — only `distribution(weibull)` is available.",
        "unsupported_option");
  b.argv.push_back("--dist");
  b.argv.push_back("weibull");
  ojson raw;
  ojson block = runP7Mle("streg", b.argv, session, &raw);
  applyHazardRatioTableM4(block, raw);
  // Stata prints /ln_p, p and 1/p in a stanza below the coefficients; like the
  // ordered models' cutpoints, they are not slopes and the mapper drops them, so
  // they are appended here from the raw payload.
  for (const char* key : {"ln_p", "p", "inv_p"}) {
    if (!raw.contains(key) || !raw[key].is_object()) continue;
    const ojson& s = raw[key];
    ojson row;
    row["name"] = std::string(key) == "ln_p" ? "/ln_p" : (std::string(key) == "p" ? "p" : "1/p");
    row["b"] = s.contains("b") ? s["b"] : ojson(nullptr);
    row["se"] = s.contains("se") ? s["se"] : ojson(nullptr);
    row["t"] = s.contains("z") ? s["z"] : ojson(nullptr);
    row["p"] = s.contains("p") ? s["p"] : ojson(nullptr);
    row["ci_lo"] = s.contains("ci_lo") ? s["ci_lo"] : ojson(nullptr);
    row["ci_hi"] = s.contains("ci_hi") ? s["ci_hi"] : ojson(nullptr);
    if (!block.contains("coeftable") || !block["coeftable"].is_array())
      block["coeftable"] = ojson::array();
    block["coeftable"].push_back(row);
  }
  return block;
}

// ===========================================================================
// logit / probit

ojson runLogitProbit(const std::string& cobraCmd, const eval::ParsedCommand& parsed,
                      SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  // FU-585 / BUG-1286: logit, logistic and probit let every weight through the
  // registry gate. Stata takes fweights, iweights and pweights for them
  // (logit.sthlp, probit.sthlp), so an aweight is its own r(101). The engine fits
  // fw and iw under the default and robust VCEs, and logit's fw under
  // vce(cluster) too (StataBE 19.5 `logit d x [fw=wi]` _se[x] .091571881918893761,
  // e(N) 30; probit .056114942772508274); an iweight under vce(cluster), and
  // probit's fweight there, are valid Stata the engine does not fit here.
  if (parsed.weightType.has_value()) {
    const std::string wt = lowerM4(*parsed.weightType);
    if (wt == "aw" || wt == "aweight")
      return ojson(eval::errorBlock("engine_error", "aweights not allowed", std::nullopt,
                                    std::nullopt, std::nullopt, std::string("r(101)")));
    bool clusterVce = false;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "cluster" && !v.isBool) clusterVce = true;
      if (k == "vce" && !v.isBool) {
        const std::string t = lowerM4(trimM4(v.strValue));
        clusterVce = clusterVce || t.rfind("cl", 0) == 0;
      }
    }
    const bool iw = wt == "iw" || wt == "iweight";
    const bool fw = wt == "fw" || wt == "fweight";
    if (clusterVce && (iw || (fw && cobraCmd != "logit")))
      return ojson(eval::errorBlock(
          "unsupported_option",
          "`" + cobraCmd + "` [" + *parsed.weightType + "=] with vce(cluster ...) is not supported "
          "in this version of PIET (it fits fweights and iweights under the default and robust "
          "VCEs" + std::string(cobraCmd == "logit" ? ", and fweights under vce(cluster ...)" : "") +
              "); nothing was fitted.",
          std::nullopt, std::nullopt, std::nullopt, std::string("r(198)")));
  }
  EstimationArgvResult built = buildEstimationArgvM4(cobraCmd, parsed, session);
  if (built.errored) return built.errorBlock;
  std::vector<std::string> cobraArgs = built.argv;
  cobraArgs.push_back("--data");
  cobraArgs.push_back(*session.currentHbin());
  cobraArgs.push_back("--tidy");
  ojson raw;
  // ME07: the fit writes its own e(sample) (the engine's rows after listwise,
  // weight, cluster AND perfect-prediction-screen losses; cmd_logit/cmd_probit
  // --esample-out), installed by cacheLastModelM4 below -- so `count if
  // e(sample)` is e(N) after a screened fit (w2u_binary_ml_edges.log:199
  // bm_l_dz0_fit `esample=190`) and margins averages over those rows
  // (w2v_margins_siblings.log:3574 F_pp_g `N=234`).
  std::string esampleValues;
  try {
    // eb-order T-P1: the fit's full e(V) (the logit/probit options_table rows
    // carry --emit-v, apps/cmds/cmds_mle.cpp add_e_v). Only the fit call gets
    // the flag; the args the LastModel records (cacheLastModelM4) do not.
    std::vector<std::string> fitArgs = cobraArgs;
    fitArgs.push_back("--emit-v");
    esampleValues = esampleFitArgs(session, fitArgs, std::string(), 0);
    raw = runCobraJsonM4(fitArgs);
  } catch (const CobraError& ce) {
    if (!esampleValues.empty()) cecon::native_unlink(esampleValues);
    // FU-547: the engine's `no_obs` (a string variable in the varlist, or no
    // row left) is Stata's "no observations" r(2000) (w2u_binary_ml_edges.log:
    // 3686-3692 `probit ystr x1`), never the rc 1 of a bare engine_error. One
    // row is r(2001) "insufficient observations" (BUG-1269, StataBE 19.5
    // `logit d x if x == 1`).
    if (ce.code == "no_obs" && std::string(ce.what()) == "insufficient observations")
      return ojson(eval::errorBlock("engine_error", "insufficient observations", std::nullopt,
                                    std::nullopt, std::nullopt, std::string("r(2001)")));
    if (ce.code == "no_obs")
      return ojson(eval::errorBlock("engine_error", "no observations", std::nullopt, std::nullopt,
                                    std::nullopt, std::string("r(2000)")));
    return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
  }
  ojson block = cobraToEstimationM4(raw, cobraCmd, &session);
  // FU-471: the reply's `df_m` is e(df_m), read from the cached reply (macros.cpp
  // eScalarKey). The block never carried it for logit/probit, and the shared
  // kOls allowlist would copy it onto every logit/probit block (25 freeze
  // fixtures), so it stays on the reply only -- the e(k) pattern below.
  block.erase("df_m");
  // W:P8: cobra's logit/probit raw JSON carries no "depvar" field at all
  // (live-verified: poisson/tobit/feols all do, these two are the only
  // estimators PIET drives that omit it), so the block published an EMPTY
  // depvar. Two user-visible consequences, both rc 0: `estimates dir` listed
  // the model as "p8_logit  ()", and `estimates table` — whose equation
  // grouping is keyed on exactly this name in Stata (logit's e(b) columns are
  // `ybin:x1`, regress's are bare `x1`) — had nothing to label the group with.
  // The name is the user's own `logit YVAR ...` token echoed back, not an
  // engine-computed quantity, so filling it here is the same honest move the
  // tobit/heckman paths in this file already make for the same engine gap.
  if (block.value("depvar", std::string()).empty() && !parsed.varlist.empty()) {
    block["depvar"] = parsed.varlist[0];
  }
  fillCoeftableStatsM4(block);
  // e(k), the number of parameters (= columns of e(b)): w2b_estim.log:706
  // probit_r_nocons e(k) 2. The engine's logit/probit reply carries no `k`, and
  // an e() name with no key map entry resolves from the cached reply's key of
  // the same name (macros.cpp resolveEScalar), so it is posted on that reply
  // only -- the e(df_a) pattern (handlers.cpp absorbedDofA).
  if (!raw.contains("k") && raw.contains("coefs") && raw["coefs"].is_array()) {
    raw["k"] = raw["coefs"].size();
  }
  if (!raw.is_object() || !raw.contains("esample_n")) {
    cecon::native_unlink(esampleValues);
    esampleValues.clear();
  }
  // BUG-1192: e(cmd) after `logistic` is `logistic` (StataBE 19.5 `logistic yb x1 x2`,
  // whose e() otherwise matches logit's but for e(predict) logistic_p), so it is the
  // e(cmd) word LastModel records; the replays keep keying on cobraCmd "logit".
  cacheLastModelM4(session, cobraCmd, cobraArgs, block, raw, esampleValues,
                   lowerM4(parsed.cmd) == "logistic" ? std::string("logistic") : std::string());
  // FU-30: e(vce) after a default-VCE logit/probit is `oim` -- both are ML
  // estimators, and Stata's ML default VCE is the observed information matrix
  // (tests/eval/golden/aer_w2/w2c_ml_getmata.log:321 ml_pw_vceoim and :356
  // ml_opts_nospace `oim`, the same probit likelihood fitted through `ml`; the
  // engine's default probit SE is the observed-information one already, BUG-140).
  // setLastModel() published lm.vcov's engine spelling `iid`, which the replay
  // machinery keys on, so only the macro store is corrected -- the regress `ols`
  // / xtreg,fe `conventional` pattern. vce(robust)/vce(cluster) keep `robust`/
  // `cluster` with e(vcetype) `Robust` (w2b_estim.log:619 logit_vce_robust).
  if (session.lastModel().has_value() && session.lastModel()->vcov == "iid") {
    session.eval.setEString("vce", "oim");
  }
  // FU-471 / FU-466: e(chi2type) and e(rules). The engine reply carries
  // `chi2type` (LR, or Wald after robust/cluster/noconstant) and `rules`, one
  // row (op, rhs, outcome, #dropped) per perfect predictor the screen dropped,
  // J(1,4,0) when none (logit's ado). e(N_cds)/e(N_cdf)/e(df_m) resolve
  // from the cached reply by name and e(rank) from its `k_vars` key. Both stores
  // are recorded with noteEOnFit so the next model install drops them.
  if (raw.contains("chi2type") && raw["chi2type"].is_string()) {
    session.eval.setEString("chi2type", raw["chi2type"].get<std::string>());
    session.noteEOnFit("chi2type");
  }
  if (raw.contains("rules") && raw["rules"].is_array() && !raw["rules"].empty()) {
    eval::EvalMatrix m;
    const ojson names = raw.value("rules_names", ojson::array());
    size_t r = 0;
    for (const auto& row : raw["rules"]) {
      std::vector<double> cells;
      if (row.is_array())
        for (const auto& c : row) cells.push_back(c.is_number() ? c.get<double>() : 0.0);
      cells.resize(4, 0.0);
      m.data.push_back(std::move(cells));
      m.rownames.push_back(r < names.size() && names[r].is_string()
                               ? names[r].get<std::string>()
                               : "r" + std::to_string(r + 1));
      ++r;
    }
    m.colnames = {"c1", "c2", "c3", "c4"};
    bool replaced = false;
    for (auto& kv : session.eval.eMatrices)
      if (kv.first == "rules") { kv.second = m; replaced = true; }
    if (!replaced) session.eval.eMatrices.emplace_back("rules", std::move(m));
    session.noteEOnFit("rules");
  }
  return block;
}

// ===========================================================================
// csdid / drdid + estat simple/event/calendar/group/pretrend


}  // namespace piet::handlers
