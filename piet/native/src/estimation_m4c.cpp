// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// R2 (giant-file split, 2026-09-01): continuation TU of estimation_m4.cpp —
// a MOVE-ONLY cut at pre-existing section banners. Preamble = the original
// file head verbatim minus its option-vocab anon block (single registration
// stays in the first TU); shared helpers come from estimation_m4_internal.h.
// This TU carries -ffp-contract=off like its parent (CMake) — FMA fusion
// here would drift golden numerics.
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

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_set>

#include "piet/cobra_runner.h"
#include "piet/cobra_session.h"
#include "piet/display_hardening.h"  // exprUnboundNameRefusal (predict, BUG-1248)
#include "piet/eval/errors.h"
#include "piet/inrange_filter.h"
#include "piet/settings.h"
#include "piet/storage_type.h"
#include "piet/strutil.h"
#include "io/win_compat.h"  // cecon::native_unlink (BUG-050 wide unlink)
#include "piet/translate.h"
#include "piet/scratch_seq.h"

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

#include "estimation_m4_internal.h"
#include "estimators/postest_names.h"  // parse_level_ref (the engine's one resolver)
#include "estimators/posthest.h"  // margins_grid / margins_parse_at (header-inline)

// R2: the option-vocab bindings, restored VERBATIM — registering the same
// token twice UNIONS the sets (errors.h, deliberate), and a set unioned with
// itself is itself, so the single-registry law holds across the three TUs.
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
// Item 4 (User ruling 2026-09-09, BUG-467's sibling surface): `post` REPLACES
// e() with the margin (Cells `B4REC|margins|dydx_post|0|`,
// `B4VAL|ecmd|margins|b_mpg|-1.8c19072612ae4X+005`, oracle_r.log). `margins`
// is in kOptMinComplete (opt_minimums_bug264.inc), so the key arrives
// exactOnly -- adding it to `known` accepts ONLY the exact spelling `post`;
// no abbreviation surface widens. THREE byte-identical copies (this file,
// estimation_m4.cpp, estimation_m4b.cpp) -- registerOptVocab unions
// registrations for the same command, so all three must move together or
// two would silently drift (IDIOMS Duplication). `atmeans` (FU-94,
// w2b_estim.log:499/:558/:588) joins the same way, exact spelling only.
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
}  // namespace

namespace piet::handlers {
using ojson = nlohmann::ordered_json;

using namespace est_detail;

// ===========================================================================
// test / lincom / nlcom (post-estimation, session.lastModel()-driven)

namespace {

// Stata's "last estimates not found" r(301) (w2u_binary_ml_edges.log:965-977: `predict`
// after a fit that stopped at r(2000) is rc 301), never the rc 1 of a bare engine_error.
// The message is kept: internal script replay_package.py classifies it by text.
ojson noPreviousEstimation() {
  return ojson(eval::errorBlock("engine_error", "No previous estimation result. Run a regression first.",
                                std::nullopt, std::nullopt, std::nullopt, std::string("r(301)")));
}

// eb-order T-P3: with no fitted model, a live e() that posted no e(b) (an
// estpost posting, a restored or `estimates use`d one) is not "no estimates" for
// every command. Stata's test, lincom and predict answer r(321) "requested
// action not valid after most recent estimation command" there
// (w2v_estpost_ematrix.log ep14_test, ep14_lincom, ep14_predict, ep14n_*,
// ep14s_predict/_lincom), while testparm, nlcom and margins keep r(301) "last
// estimates not found" (ep14_testparm, ep14_nlcom, ep14_margins). Every other
// case is noPreviousEstimation's r(301).
ojson noPreviousEstimationFor(const SessionState& session, bool invalidAfterPosting) {
  if (invalidAfterPosting && session.eval.eLive() && session.eval.coefSource() == nullptr)
    return ojson(eval::errorBlock("engine_error",
                                  "requested action not valid after most recent estimation command",
                                  std::nullopt, std::nullopt, std::nullopt, std::string("r(321)")));
  return noPreviousEstimation();
}

// The clustered small-sample convention the FIT ran under, as the feols flags
// runRegress (handlers.cpp) pushed for it -- the same keys, so the refit never
// answers under a different convention than the table it follows:
//  - BUG-568 --keep-nested-fe-dof: the areg word with an absorbed effect and a
//    cluster/twoway VCE (w2h_postest_refit.log:1081 A_cl_lincomx1 r(se)
//    .27856854160853 = _se[x1]; reghdfe/xtreg,fe exempt the nested FE, :2005
//    H_cl_ctl .2613712121703248).
//  - FU-365/BUG-848 --multiway-subset-factors: regress/areg with a twoway or
//    multiway VCE (w2l_vce_shape_det.log:263 regress_vce_c2_sex1
//    .0643525900770342, :619 areg_vce_c2_sex1 .0778721308909151).
// LastModel records no flag of its own; lm.stataCmd is the only record of the
// command word (runRegress keys both flags on it). Empty for every other fit,
// whose argv stays byte-identical.
//  - FU-386 --multiway-subset-factors after xtreg,fe with a twoway or multiway
//    VCE (runXtreg; w2s_multiway_class.log:696 mw_xt_cg_se .0532737214421747); the
//    engine exempts the panel FE nested in the clusters (minus = rank + dfax).
//  - FU-406: areg's --keep-nested-fe-dof under a three-way (`multiway`) VCE too
//    (log:1165 mw_areg_c3 .0720220388954095).
std::vector<std::string> fitConventionFlagsM4(const LastModel& lm) {
  std::vector<std::string> flags;
  if (lm.cobraCmd != "feols") return flags;
  if (lm.stataCmd == "areg" && !lm.fe.empty() &&
      (lm.vcov == "cluster" || lm.vcov == "twoway" || lm.vcov == "multiway"))
    flags.push_back("--keep-nested-fe-dof");
  if ((lm.stataCmd == "regress" || lm.stataCmd == "reg" || lm.stataCmd == "areg" ||
       lm.stataCmd == "xtreg") &&
      (lm.vcov == "twoway" || lm.vcov == "multiway"))
    flags.push_back("--multiway-subset-factors");
  // e(sample) T2: the fit kept its singleton groups (areg, xtreg,fe, reghdfe's
  // keepsingletons), so the refit does too (cmds_postest.cpp postest_fit_spec):
  // dropping them refits a different sample (w2_esample.log:897 D3 59, :925 D4
  // 59 against the default drop's 58).
  if (lm.keepSingletons && !lm.fe.empty()) flags.push_back("--keep-singletons");
  // BUG-1067: the fit omitted the later member of each collinear set (reghdfe,
  // areg, xtreg,fe), so the refit omits the same columns: otherwise `test`/`lincom`
  // after `reghdfe y x z` (z = 2*x) refit with x omitted and answered for _b[z].
  if (lm.collinearForward) flags.push_back("--collinear-forward");
  // Stata regress's exact-fit rule (runRegress pushes --regress-exact-fit on the
  // same condition): the refit of `test`/`lincom`/`nlcom` sees the fit's V = 0, so
  // `test x` after an exact `regress` answers F and p missing as StataBE does.
  // BUG-1055: areg and an aw/fw/iw-weighted regress too, under the classic VCE.
  if ((((lm.stataCmd == "regress" || lm.stataCmd == "reg") && lm.fe.empty()) ||
       lm.stataCmd == "areg") &&
      lm.fitWtype != "pw" && lm.fitWtype != "pweight" && (lm.vcov.empty() || lm.vcov == "iid") &&
      lm.cluster.empty() &&
      !(lm.result.has_value() && lm.result->is_object() && lm.result->contains("svy")))
    flags.push_back("--regress-exact-fit");
  return flags;
}

// FU-541: a logit/probit fitted `asis` kept its perfect predictors, so every
// engine refit of it (lincom/nlcom/test, predict, margins) must skip Stata's
// perfect-prediction screen too; the engine marks such a reply `asis`. Any other
// fit adds nothing, so those argvs are byte-identical.
// FU-426: the same refits carry the fit's maximize options, which the engine
// echoes as the flags it was given (`ml_max_argv`, cmds_mle.cpp
// echo_ml_max_options), so the refit stops at the iterate the fit posted: after
// `logit d x1 x2 i.c, tolerance(1e-12) ...` (e(ic) 10) `test 2.c` answered from
// the default e(ic) 8 refit (w2e_probes.log:249 lp_tight r(chi2) against :141
// lp_default). A fit given none has no key and adds nothing.
// cloglog's reply carries both keys too (cmds_mle.cpp cmd_cloglog; BUG-1547).
void pushFitAsisM4(std::vector<std::string>& args, const LastModel& lm) {
  if (!(lm.cobraCmd == "logit" || lm.cobraCmd == "probit" || lm.cobraCmd == "cloglog") ||
      !lm.result.has_value() || !lm.result->is_object())
    return;
  if (lm.result->value("asis", false)) args.push_back("--asis");
  const auto it = lm.result->find("ml_max_argv");
  if (it == lm.result->end() || !it->is_array()) return;
  for (const auto& f : *it)
    if (f.is_string()) args.push_back(f.get<std::string>());
}

bool postestFitLacksConstantM4(const LastModel& lm);  // defined with the refit guards below

// FU-385/FU-387: the `--model iv` 2SLS refit of lincom/test/nlcom after
// `ivregress 2sls` (postestIvRefitServedM4): --x carries the included exogenous
// regressors only (or the constant placeholder), --endog/--instruments the fit's
// own lists, and --small / --multiway-subset-factors the fit's variance convention
// (runIvRegress; the reply keeps `small` true only after `small`).
bool ivFitSmallM4(const LastModel& lm) {
  return lm.result.has_value() && lm.result->is_object() && lm.result->contains("small") &&
         (*lm.result)["small"].is_boolean() && (*lm.result)["small"].get<bool>();
}
bool ivRefitVerbM4(const std::string& verb) {
  return verb == "lincom" || verb == "test" || verb == "nlcom";
}

std::vector<std::string> lastModelBaseArgv(const std::string& cobraCmd, const LastModel& lm) {
  // W:P5 fit-sample fidelity (BUG-163): refit from the sample the model was
  // ACTUALLY fitted on. fitDataPath is runRegress's materialized if/in scratch
  // file; fitKeepif replays a --keepif that went straight to the engine. Both
  // empty => byte-identical to the historical full-dataPath call.
  const bool ivRefit = lm.cobraCmd == "iv" && ivRefitVerbM4(cobraCmd);
  std::vector<std::string> args = {cobraCmd, "--data",
                                    lm.fitDataPath.empty() ? lm.dataPath : lm.fitDataPath,
                                    "--y", lm.y, "--x",
                                    ivRefit ? (lm.ivExog.empty() ? std::string("(intercept)")
                                                                 : joinCommaM4(lm.ivExog))
                                            : (lm.x.empty() ? "(intercept)" : joinCommaM4(lm.x))};
  if (ivRefit) {
    args.insert(args.end(), {"--endog", joinCommaM4(lm.ivEndog), "--instruments",
                             joinCommaM4(lm.ivInstruments)});
    if (ivFitSmallM4(lm)) {
      args.push_back("--small");
      if (lm.vcov == "twoway" || lm.vcov == "multiway") args.push_back("--multiway-subset-factors");
    }
  }
  if (!lm.fitKeepif.empty()) {
    args.push_back("--keepif");
    args.push_back(lm.fitKeepif);
  }
  if (!lm.fe.empty()) {
    args.push_back("--fe");
    args.push_back(joinCommaM4(lm.fe));
  }
  // BS-098/BS-013 (BETA A4): forward the FITTED model's vcov/cluster so
  // post-estimation recomputes SEs under the SAME vcov the original model
  // used, instead of cobra's test/lincom/nlcom CLI silently defaulting to
  // iid (the §1f probe: lincom SE 0.0543 iid vs 0.0044 cluster after a
  // `vce(cluster ...)` model — cmds_postest.cpp's cmd_lincom/cmd_test both
  // do `parse_vcov(opt(a, "--vcov", "iid"))`, i.e. iid whenever --vcov is
  // simply never sent, which is what happened for EVERY post-estimation
  // call before this fix). lm.vcov defaults to "iid" (session_state.h) and
  // that IS cobra's own default too, so this is a byte-identical no-op for
  // the (overwhelmingly common) unclustered/non-robust case — the ONLY
  // observable change is for a lastModel that actually used a non-default
  // vcov.
  args.push_back("--vcov");
  args.push_back(lm.vcov);
  if (!lm.cluster.empty()) {
    args.push_back("--cluster");
    args.push_back(joinCommaM4(lm.cluster));
  }
  // BUG-571: the fit's weight. Without it the refit was the UNWEIGHTED model at
  // rc 0 (w2h_postest_refit.log:383 R_pw_lincomx1 r(estimate) 2.381531911602316;
  // main answered 2.4882849226167476). Empty for an unweighted fit, so that argv
  // is byte-identical to before. postestRefitMatchesFitM4 keeps refusing the
  // weighted fits this refit cannot reproduce (ML estimators, svy:).
  if (!lm.fitWeight.empty()) {
    args.push_back("--weight");
    args.push_back(lm.fitWeight);
    if (!lm.fitWtype.empty()) {
      args.push_back("--wtype");
      args.push_back(lm.fitWtype);
    }
  }
  for (const auto& f : fitConventionFlagsM4(lm)) args.push_back(f);
  pushFitAsisM4(args, lm);
  // FU-131: the fit's `noconstant`. postest_fit_spec refits without the constant
  // (feols_fit's no_intercept; logit/probit drop load_glm's trailing one), so
  // lincom/test/testparm/nlcom answer for the fitted model (w2p_estim_ev.log:1294
  // G_nc_lincom_x1 2.356553697480766 = :1275 G_nc_fit _b[x1]). Absent for a fit
  // with a constant, whose argv stays byte-identical.
  if (postestFitLacksConstantM4(lm)) args.push_back("--no-intercept");
  return args;
}

// ---------------------------------------------------------------------------
// contract_A §31 — post-estimation by handle, without the re-fit.
//
// WHAT THIS COSTS TODAY. lastModelBaseArgv above rebuilds the ENTIRE model
// specification on every lincom/nlcom/test, and the engine answers it by
// fitting that model again. Measured 2026-07-21 on the real analysis: in ONE
// dependent-variable pass `lincom` ran 54 times for 22.61 s — 419 ms each, 17 %
// of that pass's 131 s, the largest single engine-side consumer. At the
// 160-depvar scale that is roughly an hour spent recomputing fits that already
// existed.
//
// WHAT IS AND IS NOT BEING PAID — measured, not assumed. These calls are
// ALREADY resident: the argv carries --data but no --out and no --keepif, so
// SessionState's intercept takes its `goResident` branch (session_state.cpp)
// and reissues them as a frame `run` with --data stripped. That same branch's
// own comment names `test`/`margins`/`lincom` as the post-estimation case it
// resyncs for. So the 164 MB re-read is NOT part of the 419 ms — only the
// resync after a `gen`/`replace` moved the head re-reads, and that is one call,
// not 54. The re-fit is the whole remaining term, and the only thing this
// change buys.
//
// WHY THE FIRST CALL STILL FITS. §31 mints only on lincom/nlcom/test, not on
// feols itself, so the first post-estimation call after each fit still fits
// once and publishes the result; the remaining N−1 are free. On the measured
// flow that is 1 fit instead of 54 per dependent variable.
//
// STORE PRESSURE, AND WHY EVICTION CANNOT BITE THE LIVE HANDLE. Because the
// fitting form already arrives as a frame `run`, the engine mints on EVERY
// post-estimation call PIET issues today — ~54 per depvar, ~8,600 across
// analysis.do, each storing a k×k covariance. This change is what stops that:
// only the first call per model still fits, so mint traffic drops with the
// re-fits (~54/depvar -> 1). The engine caps the store at 32 MODELS (not
// bytes), evicting in mint order. PIET holds at most ONE handle at a time — it
// lives on LastModel, and step (3) below overwrites it with each new mint — so
// the handle PIET is holding is always the MOST RECENTLY minted one, i.e. the
// last thing the cap would ever evict. Eviction would have to be preceded by 32
// further mints while PIET still held this one, which the single-handle
// invariant forbids. And if it somehow happened anyway, it is not a user-facing
// failure: exit 3, the handle is dropped, the full specification runs and
// answers correctly one re-fit slower, and re-mints for the next call.
//
// FEATURE-DETECTED, NEVER VERSION-GATED. See CobraSession::postestState() —
// the engine implementing §31 still advertises contract_a 1.19.0, and Art 7
// means a shipped PIET pins the ARCHIVED engine, so PIET must keep working
// against a binary with no `postest` op at all. Every failure path below lands
// on the unchanged full-specification call.

// Write `handle` onto the session's LastModel (empty clears it). Copy-modify-set
// through setLastModel is the only supported mutation: it also refreshes
// eval.lastModelResult, which the _b[]/_se[]/e() resolver reads. The equality
// guard keeps the common case (handle unchanged across a run of combinations)
// from re-assigning that JSON on every single call.
void setEstHandleM4(SessionState& session, const std::string& handle) {
  const auto& cur = session.lastModel();
  if (!cur.has_value() || cur->estHandle == handle) return;
  LastModel updated = *cur;
  updated.estHandle = handle;
  session.setLastModel(std::move(updated));
}

// BUG-203. Defined below, next to the estimator-membership sets it reads (they
// belong beside runLincom/runNlcom/runTestCmd's guard, not here); forward-
// declared because the fallback argv is assembled in this function.
std::string postestModelFlagM4(const LastModel& lm);

// Run ONE post-estimation combination, preferring the §31 handle path.
//   `verb`  — "lincom" | "nlcom" | "test"
//   `combo` — the combination-only flags (--exp/--expr/--joint/--constraint,
//             --tidy). Must contain NOTHING that specifies the model: on the
//             handle path the stored model IS the specification, and §31
//             rejects an argv carrying both rather than letting one silently
//             win.
// Returns the raw cobra result JSON, and throws CobraError exactly where
// runCobraJsonM4 does — so all three call sites keep their existing catch.
ojson runPostEstimationM4(const std::string& verb, const std::vector<std::string>& combo,
                          SessionState& session, const LastModel& lm) {
  CobraSession* cs = activeCobraSession();
  const std::string handle = lm.estHandle;

  // Built up front, while `lm` is unambiguously live. setEstHandleM4 below
  // writes through setLastModel, and `lm` is a reference INTO the session's own
  // LastModel — reading it across that write is a lifetime question not worth
  // having. A dozen small string copies on the fast path does not buy back the
  // subtlety of avoiding them.
  std::vector<std::string> fallbackArgs = lastModelBaseArgv(verb, lm);
  // BUG-203: name the ESTIMATOR on the full-specification path, so the engine
  // refits with it instead of with feols_fit(). Empty for the least-squares
  // family, which keeps its argv byte-for-byte. NOT added to `combo`: the
  // handle path passes combo verbatim and §31 rejects an argv that carries both
  // a handle and a model specification — and it does not need the flag anyway,
  // because the stored fit already IS the maximum-likelihood one (cobra's
  // PostestModel carries its own `asymptotic` marker for exactly this reason).
  {
    const std::string modelFlag = postestModelFlagM4(lm);
    if (!modelFlag.empty()) {
      fallbackArgs.push_back("--model");
      fallbackArgs.push_back(modelFlag);
    }
  }
  fallbackArgs.insert(fallbackArgs.end(), combo.begin(), combo.end());
  // BUG-369: the fallback REFITS from the original specification, so an `L.x`
  // regressor needs the same tsset context the original fit was given
  // (buildEstimationArgvM4's injectTsContext at :793). Without it, a model that
  // fitted perfectly well could not be post-estimated: `regress fatal beertax
  // L.beertax` then `test L.beertax` came back ts_no_tsset — the fit knew about
  // the time variable and its own `test` did not.
  {
    std::vector<std::string> tsTokens = lm.x;
    tsTokens.push_back(lm.y);
    tsTokens.insert(tsTokens.end(), lm.fe.begin(), lm.fe.end());
    bool needTs = translate::hasTsToken(tsTokens);
    // The restriction itself can name one too (`test L.x`, `testparm L.x`), and
    // it arrives as a `--joint`/`--constraint` VALUE rather than a bare token.
    if (!needTs)
      for (const auto& c : combo)
        if (translate::exprHasTsToken(c)) { needTs = true; break; }
    if (needTs) fallbackArgs = translate::injectTsContext(fallbackArgs, session);
  }

  // ---- (1) handle path: no data read, no re-fit -------------------------
  if (!handle.empty() && cs != nullptr && cs->postestState() != CobraSession::Postest::No) {
    std::vector<std::string> argv = {verb};
    argv.insert(argv.end(), combo.begin(), combo.end());
    // Mirror runCobraJsonM4's choke point so stdout has the SAME shape on both
    // paths (§31 guarantees the estimator numbers are byte-equal; provenance is
    // the documented carve-out, and this is the flag that governs it).
    if (!piet::settings::load().provenanceHash) argv.push_back("--no-provenance");
    // No out-param: nothing here needs the echoed handle or `fits` (the gate
    // leg reads those through lastEst()); the numbers come from stdout.
    CobraRunResult r = cs->framePostest(handle, argv, nullptr);
    if (r.ok) {
      try {
        return ojson::parse(r.stdout_text);
      } catch (const nlohmann::json::exception&) {
        // An exit-0 postest with unparseable stdout is not a shape §31 permits.
        // Treat it as a miss and re-run the full specification rather than
        // surfacing a synthesized failure for something the slow path can
        // answer correctly.
      }
    }
    // ANY miss falls through: exit 3 (handle expired / never minted / evicted,
    // or the E6 unconsumed-flag net), exit 2 (engine predates §31), a dead
    // worker. §31 is explicit that the client must never assume, reuse or
    // synthesize an answer on a stale handle — re-fitting is the only correct
    // response, and dropping the handle first keeps a permanently-dead one from
    // costing a failed round trip on every subsequent call.
    setEstHandleM4(session, "");
  }

  // ---- (2) full-specification fallback — today's path, byte-for-byte -----
  // fallbackArgs came from lastModelBaseArgv, which forwards the FITTED model's
  // --vcov/--cluster; that is what keeps BS-098/BS-013 alive on this path (see
  // its comment above). Nothing here is conditional on §31 — an engine without
  // it reaches this line on every single call, exactly as today.
  const long long seqBefore = (cs != nullptr) ? cs->responseSeq() : 0;
  ojson raw = runCobraJsonM4(fallbackArgs);  // throws CobraError -> caller's catch

  // ---- (3) retain whatever handle that fit just minted -------------------
  // Re-fetched, not reused: the call above may be the FIRST thing on this
  // thread to reach the worker at all, in which case `cs` was null on entry and
  // the session that just serviced it is only discoverable now.
  //
  // Guarded on responseSeq() having MOVED, because the call may equally have
  // bypassed the worker entirely (serve disabled, §25-only engine, one-shot
  // fallback) — and then lastEst() still describes some EARLIER response.
  // Storing that onto the model just fitted is exactly the wrong-model hazard,
  // so an unmoved sequence must read as "no mint", never as "reuse the last
  // one". A null-on-entry cs contributes seqBefore == 0, which is safe: any
  // session that parsed a response has responseSeq() >= 1.
  CobraSession* csAfter = activeCobraSession();
  if (csAfter != nullptr && csAfter->responseSeq() != seqBefore) {
    const CobraEstMeta& est = csAfter->lastEst();
    if (est.valid) setEstHandleM4(session, est.handle);
  }
  return raw;
}

// BUG-203 — WHICH ESTIMATOR PRODUCED THE MODEL IS PART OF THE QUESTION.
//
// cobra's `lincom`/`nlcom`/`test` CLIs take a model SPECIFICATION (--y/--x/
// --fe/--vcov) and no estimator at all, so cmds_postest.cpp answers every one
// of them with `feols_fit()`. runLincom/runNlcom/runTestCmd checked only that a
// lastModel EXISTS, never which estimator produced it, so `logit y x1 x2` ->
// `lincom x1` silently reported the LINEAR-PROBABILITY-MODEL coefficient as the
// logit one. The knowledge was already in this file (runIvRegress's own
// comment: cobra's post-estimation CLIs "always re-fit as plain OLS") — it had
// just never been followed to the user-visible surface.
//
// Measured against StataNow 19.5 (n=120, seed 203203, `logit ybin x1 x2` then
// `lincom x1`): Stata 1.087742905079 (se .266346342116), PIET 0.221521751160
// (se .046231897247) — and `poisson ybin x1 x2` then `lincom x1` returned that
// SAME 0.221521751160 to every digit, which is the proof the estimator was not
// read at all rather than merely mishandled. 0.221521751160 is exactly what
// Stata's own `regress ybin x1 x2` -> `lincom x1` prints.
//
// kPostestOlsCmds is exactly the set of lm.cobraCmd values for which the
// engine's OLS refit IS the fitted model's own coefficients — the same
// membership argument runMargins' kAtSafeCmds makes further down (plain
// regress/feols, including reghdfe and xtreg's ,fe path, all of which already
// share these cobraCmd spellings per runXtreg/handlers.cpp::runRegress).
//
// kPostestGlmCmds is the set cobra's `--model` refit covers: the same
// logit/probit/poisson family cmd_margins' GLM branch already fits through
// load_glm + logit_fit/probit_fit/fepois_fit. For those PIET forwards
// `--model <estimator>` and the engine answers off the REAL maximum-likelihood
// fit, referencing the combination to the normal (and `test` to chi2) the way
// Stata does.
//
// Everything else honest-rejects: `nbreg`, `tobit`/`heckman`/`qreg`/`mediate`
// (non-OLS point estimates), `newey`/`prais` (HAC/AR1 transforms),
// `iv`/`liml`/`reg3` (2SLS/GMM/SUR), `xtreg`'s random-effects path, `fepois`/
// `ppmlhdfe`/`xtpois`/`xtlogit` (the engine's ML refit has no absorbed-effects
// path), `csdid` — none of these estimators' fitted coefficients equal a plain
// OLS refit of y on x, and none has a post-estimation refit in the engine yet.
// Honest refusal beats a silent wrong answer (the same doctrine BS-023/BS-052
// and runMargins' own kAtSafeCmds reject already apply on the neighbouring
// `margins, at()` surface).
const std::set<std::string>& postestOlsCmdsM4() {
  static const std::set<std::string> kPostestOlsCmds = {"feols", "ols", "regress", "reg", "r"};
  return kPostestOlsCmds;
}
// BUG-1547: cloglog joined the engine's ML refits (9e2e97c8: predict pr/xb, the
// margins AME and at() branches under OIM/robust/cluster/pweight, lincom/nlcom/test
// off cloglog_ml), matching StataBE 19.5 within 2e-15 (scratch-laneA/agents/eng-ml
// x21.log, x22.log).
const std::set<std::string>& postestGlmCmdsM4() {
  static const std::set<std::string> kPostestGlmCmds = {"logit", "probit", "poisson", "cloglog"};
  return kPostestGlmCmds;
}

// The `--model` value to forward for this lastModel, or empty for the
// least-squares family (which forwards nothing and keeps its historical argv
// byte-for-byte). Callers must first have passed postestEstimatorOkM4.
std::string postestModelFlagM4(const LastModel& lm) {
  if (lm.cobraCmd == "iv") return "iv";  // FU-385: the 2SLS refit (postestIvRefitServedM4)
  return postestGlmCmdsM4().count(lm.cobraCmd) ? lm.cobraCmd : std::string();
}

// FU-385/FU-387: lincom/test/testparm/nlcom after `ivregress 2sls` are served by the
// engine's `--model iv` refit (cmds_postest.cpp postest_fit_spec), which runs the
// fit's own iv2sls_fit under its --vcov/--cluster/--small and multiway convention:
// w2s_multiway_class.log:840 mw_iv_c2_lincom r(se) .0503059336995026 = _se[x1] :816,
// r(df) `.` (no `small`: the normal reference). Only that shape: ivreghdfe/ivreg2
// (absorbed effects, their own small-sample rules), gmm (its own weight matrix) and
// liml (cobraCmd liml) keep the refusal below.
bool postestIvRefitServedM4(const std::string& verb, const LastModel& lm) {
  if (lm.cobraCmd != "iv" || lm.stataCmd != "ivregress" || !lm.fe.empty()) return false;
  if (verb != "lincom" && verb != "test" && verb != "testparm" && verb != "nlcom") return false;
  return lm.result.has_value() && lm.result->is_object() &&
         lm.result->value("estimator", std::string()) == "iv2sls";
}

// Does the engine's post-estimation refit answer for the FITTED model?
// test/testparm/lincom/nlcom refit through lastModelBaseArgv, which carries the
// fit's weight (BUG-571) and its clustered small-sample convention
// (fitConventionFlagsM4: BUG-568 areg, FU-365 multiway) into cmds_postest.cpp
// postest_fit_spec; the §31 handle is minted by that same refit, so it carries
// them too. `margins, at()` (runMargins' own argv, cmd_margins' --at branches)
// carries only --multiway-subset-factors: it reads --weight only to mark it
// consumed and has no areg dof, so a weighted or areg-cluster fit still refuses
// there by name, as predict does for weights (BUG-519). Returns true when the
// refit answers for the fitted model; returns false and fills `reject` otherwise.
// A least-squares / logit / probit / poisson fit made WITHOUT a constant
// (`noconstant`, buildEstimationArgvM4's --no-intercept) leaves no intercept row
// in the cached coefficient table. LastModel records no noconstant flag, so this
// is the record: every engine post-estimation refit that would otherwise append
// the intercept (lastModelBaseArgv, runMargins, runPredict) forwards
// --no-intercept when it is true (FU-131), and the engine refits the fitted
// model (w2p_estim_ev.log G_nc_* / G_pnc_*). Before FU-131 those verbs refused
// here (BUG-603): the refit answered the WITH-constant model at rc 0
// (w2b_estim.log:732 probit_noconstant _b[x1] .4484790120538529 against :762
// probit_ctl 1.355178). Absorbed-FE fits are excluded: their tables carry no
// intercept row by design and have their own refit guards.
bool postestFitLacksConstantM4(const LastModel& lm) {
  if (!lm.fe.empty() || !lm.result.has_value()) return false;
  if (!postestGlmCmdsM4().count(lm.cobraCmd) && !postestOlsCmdsM4().count(lm.cobraCmd)) return false;
  const auto& r = *lm.result;
  const char* key = r.is_object() && r.contains("coefs") ? "coefs" : "coeftable";
  if (!r.is_object() || !r.contains(key) || !r[key].is_array() || r[key].empty()) {
    return false;
  }
  for (const auto& row : r[key]) {
    if (!row.is_object() || !row.contains("name") || !row["name"].is_string()) return false;
    const std::string n = row["name"].get<std::string>();
    if (n == "(intercept)" || n == "_cons" || n == "cons") return false;
  }
  return true;
}

// The refusal for a no-constant fit whose engine refit cannot drop the constant:
// the poisson refit (cmds_postest.cpp refuses --no-intercept with --model
// poisson; PIET also refuses `poisson ..., noconstant` at the fit, so this is
// the defensive half). By name, rc 198, never the with-constant model's number.
ojson postestNoConstantRejectM4(const std::string& verb, const LastModel& lm) {
  return engineErrorBlockM4(
      verb + " after `" + lm.cobraCmd +
          " ..., noconstant` is not supported in this version of PIET -- the engine's "
          "post-estimation re-fit of this model always includes a constant, so it would "
          "answer for a different model than the one fitted. Refusing rather than reporting "
          "a number that is not the one asked for.",
      "unsupported_option");
}

}  // namespace

// Declared in estimation_m4_internal.h: boottest (estimation_m4b.cpp) asks too.
// FU-475's refit half. A fit whose regressors carry a time-series operator and
// whose `if` the engine applied as --keepif (runRegress's TS route, and the iv,
// xtreg, newey and prais routes, which always send --keepif) kept the rows
// whose lag reads a row outside the restriction (w2_esample.log:751-775 C6,
// e(N) 40). A refit that replays lm.fitKeepif as a PHYSICAL pre-filter loses
// those lags: a smaller sample at rc 0. Measured on main 2026-10-13 with the
// w2_esample.do data: `xtreg y L.x1 x2 if t>=3, fe` fits _b[L.x1] -.7953699
// (N 40) and `lincom L.x1` answered -.8123398. The engine refits that apply the
// restriction after the operators (plan_keepif) are served: test/lincom/nlcom
// (cmds_postest.cpp postest_refit), the linear margins refit (cmd_margins) and
// predict (cmd_predict --fit-keepif, with --fit-data once the head moved).
// Refused by name for the rest: boottest (applyIfAndInFilter), the ML refits,
// the native predict statistics and predict after absorbed effects.
std::optional<ojson> postestTsKeepifRefusalM4(const std::string& verb, const LastModel& lm) {
  if (lm.fitKeepif.empty()) return std::nullopt;
  std::vector<std::string> terms = lm.x;
  terms.insert(terms.end(), lm.fe.begin(), lm.fe.end());
  terms.push_back(lm.y);  // an operated depvar too (BUG-1277: `regress D.y x if t>=3`)
  if (!translate::hasTsToken(terms)) return std::nullopt;
  return engineErrorBlockM4(
      verb + " after a fit with time-series operators and an `if` restriction is not "
          "supported in this version of PIET -- the engine's post-estimation re-fit applies the "
          "restriction before the operators read the full series, so it would answer for a "
          "smaller sample than the one fitted. Refusing rather than reporting a number that is "
          "not the one asked for.",
      "unsupported_option");
}

namespace {

// FU-131: true when `lm` is a no-constant fit the engine's refit cannot reproduce
// (only the poisson refit; feols/logit/probit take --no-intercept).
bool postestNoConstantUnrefittableM4(const LastModel& lm) {
  return lm.cobraCmd == "poisson" && postestFitLacksConstantM4(lm);
}

bool postestRefitMatchesFitM4(const std::string& verb, const LastModel& lm, ojson& reject) {
  // The verbs whose refit is lastModelBaseArgv's (weight + convention flags).
  const bool fullRefit = verb == "lincom" || verb == "test" || verb == "testparm" || verb == "nlcom";
  // FU-475: those four replay lm.fitKeepif as the engine's --keepif on the fit's
  // full frame, and cmds_postest.cpp's refit (postest_refit) applies it as a row
  // mask after the time-series operators, as the fit did -- served. So does the
  // linear refit of margins (cmd_margins' --at/--marginlist path, plan_keepif
  // likewise) after a least-squares fit. Every other verb's refit still filters
  // before the operators and refuses by name.
  const bool linearMargins =
      verb.rfind("margins", 0) == 0 && postestOlsCmdsM4().count(lm.cobraCmd) > 0;
  if (!fullRefit && !linearMargins) {
    if (auto refused = postestTsKeepifRefusalM4(verb, lm)) {
      reject = *refused;
      return false;
    }
  }
  //  - A weighted fit: measured on main 2026-09-28, `regress y x1 x2 [pweight=w]`
  //    (w2_pweight.do fixture) fits _b[x1] 2.38153191 and `lincom x1` answered
  //    2.48828492 -- the unweighted coefficient; `test x1` F 210.02 against the
  //    fit's (_b/_se)^2 135.33. Served since the refit carries the weight, for
  //    the least-squares family; still refused for `margins, at()` and for an
  //    estimator whose refit is not feols (the ML refit takes no weight).
  //  - A `svy:` fit (cached as a pweighted feols, runSvy): its linearized VCE
  //    has no refit at all, and the weight alone would answer under HC1.
  //  - areg with a cluster VCE: its SE charges an FE nested in the cluster
  //    (BUG-568). Served for the full refit (--keep-nested-fe-dof); `margins,
  //    at()` would apply reghdfe's exemption, so it refuses for any cluster
  //    (whether an FE is nested in the cluster is a property of the data).
  //  - regress with a two-way cluster VCE (BUG-850): the fit runs Stata's
  //    built-in multiway convention (--multiway-subset-factors, BUG-848;
  //    w2l_vce_shape_det.log:258 regress_vce_c2_se .0643525900770342).
  //    lastModelBaseArgv forwards the flag (fitConventionFlagsM4), so the full
  //    refit and `margins, at()` answer under it (FU-385; :263 lincom
  //    .0643525900770342) and nothing here refuses.
  // No-constant least-squares fits, including wildboot, carry --no-intercept.
  // The Poisson refit still has no constant-free implementation and refuses.
  if (postestNoConstantUnrefittableM4(lm)) {
    reject = postestNoConstantRejectM4(verb, lm);
    return false;
  }
  if (lm.result.has_value() && lm.result->is_object() && lm.result->contains("svy")) {
    reject = engineErrorBlockM4(
        verb + " after `svy:` is not supported in this version of PIET -- the engine's "
            "post-estimation re-fit has no survey-design input, so it would answer under a "
            "different variance than the linearized one fitted. Refusing rather than reporting "
            "a number that is not the one asked for.",
        "unsupported_option");
    return false;
  }
  const bool weightedWildboot = verb == "boottest" && postestOlsCmdsM4().count(lm.cobraCmd) && lm.fe.empty();
  if (!lm.fitWeight.empty() && !weightedWildboot && (!fullRefit || !postestOlsCmdsM4().count(lm.cobraCmd))) {
    reject = engineErrorBlockM4(
        verb + " after a weighted fit ([" +
            (lm.fitWtype.empty() ? std::string("weight") : lm.fitWtype) + "=" + lm.fitWeight +
            "]) is not supported in this version of PIET -- the engine's post-estimation "
            "re-fit has no weight input, so it would answer for the unweighted model. "
            "Refusing rather than reporting a number that is not the one asked for.",
        "unsupported_option");
    return false;
  }
  // boottest is exempt (FU-732): its p is a ratio of t statistics that share one
  // scale, so the clustered dof never moves it, and runBoottest takes the t it
  // reports from the areg table's own standard error (boottestRefitMatchesFitM4).
  if (!fullRefit && verb != "boottest" && lm.stataCmd == "areg" &&
      (!lm.cluster.empty() || lm.vcov == "cluster" || lm.vcov == "twoway" ||
       lm.vcov == "multiway")) {
    reject = engineErrorBlockM4(
        verb + " after `areg ..., vce(cluster)` is not supported in this version of PIET -- "
            "the engine's post-estimation re-fit applies the reghdfe/xtreg,fe clustered "
            "degrees of freedom (an absorbed effect nested in the cluster is exempt), not "
            "areg's, so its standard errors would differ from the areg table. Refusing "
            "rather than reporting a number that is not the one asked for.",
        "unsupported_option");
    return false;
  }
  return true;
}

// Gate a post-estimation verb on the estimator that produced lastModel.
// Returns true when the call may proceed; returns false and fills `reject` with
// the honest-refusal block otherwise.
bool postestEstimatorOkM4(const std::string& verb, const LastModel& lm, ojson& reject) {
  if (!postestRefitMatchesFitM4(verb, lm, reject)) return false;
  if (postestOlsCmdsM4().count(lm.cobraCmd)) return true;
  if (postestIvRefitServedM4(verb, lm)) return true;
  if (postestGlmCmdsM4().count(lm.cobraCmd)) {
    // The engine's ML refit is the plain OIM fit: no absorbed effects, no
    // alternative VCE (exactly what cmd_margins' GLM branch supports). A
    // lastModel carrying either would be answered under a DIFFERENT
    // specification than it was fitted under — the same silent-wrong shape this
    // whole fix removes — so it refuses rather than forwarding a flag the
    // engine would have to ignore. cobra's own postest_fit_spec makes the
    // identical check, so a direct CLI caller cannot slip past this either.
    if (!lm.fe.empty() || !lm.cluster.empty() || (!lm.vcov.empty() && lm.vcov != "iid")) {
      reject = engineErrorBlockM4(
          verb + " is not supported after `" + lm.cobraCmd +
              "` with absorbed fixed effects or a non-default vce() in this version of PIET — "
              "the engine's maximum-likelihood post-estimation refit covers only the plain "
              "(OIM) fit. Refusing rather than answering under a specification the model was "
              "not fitted under.",
          "unsupported_option");
      return false;
    }
    return true;
  }
  reject = engineErrorBlockM4(
      verb + " is not supported after `" + lm.cobraCmd +
          "` in this version of PIET — the engine's post-estimation path re-fits the model as "
          "plain OLS from the stored dependent and independent variables, so the answer would "
          "be a least-squares one reported as if it came from the `" +
          lm.cobraCmd +
          "` fit. Refusing rather than reporting a number that is not the one asked for.",
      "unsupported_option");
  return false;
}

}  // namespace

// boottest refits the stored least-squares model: wildboot accepts the fit's
// no-intercept flag and unabsorbed aw/fw/iw/pw weights. Weighted absorbed fits
// remain named refusals; none is silently reduced to OLS. The IV WRE (ivregress
// 2sls/liml, one-absorb ivreghdfe) and the unweighted, unabsorbed multiway
// bootstrap do not come through this guard: runBoottest serves them with the
// engine's direct-refit kernel (wildboot --wre / --cluster a,b), and any other
// IV fit is still refused here.
// The helper's areg-with-cluster rule does NOT apply to boottest (FU-732):
// wildboot's t0/se come from cluster_ssf (src/inference/wildboot.cpp), the
// reghdfe/xtreg,fe convention that exempts an FE nested in the cluster, while
// areg's clustered table (BUG-568) does not -- but cluster_ssf is one constant
// for t0 and every bootstrap t, so the symmetric p does not depend on it
// (w2_esample_tsrefit.log:336 W1 p .3125, the engine's 128-draw p), and
// runBoottest reports areg's t from the fit's own standard error, not t0
// (E2_areg_id_st t 1.156417206472481 at :1535 is areg's, not reghdfe's
// 1.264649918737085 at :1668; W1_st :342). Absorbed-FE fits stay served:
// cluster_ssf is the same dof as the reghdfe/xtreg,fe fit they follow.
bool boottestFitLacksConstantM4(const LastModel& lm) { return postestFitLacksConstantM4(lm); }

bool boottestRefitMatchesFitM4(const LastModel& lm, ojson& reject) {
  if (!postestOlsCmdsM4().count(lm.cobraCmd)) {
    std::string kind = lm.stataCmd.empty() ? lm.cobraCmd : lm.stataCmd;
    if (lm.cobraCmd == "iv" || lm.cobraCmd == "liml") kind = "ivregress/ivreg2/ivreghdfe (" + kind + ")";
    reject = engineErrorBlockM4(
        "boottest after `" + kind +
            "` is not supported in this version of PIET -- the engine's wild bootstrap "
            "re-fits the model as unweighted least squares of the dependent variable on the "
            "stored regressors (with a constant), so after an instrumental-variables, "
            "maximum-likelihood or other non-least-squares fit it would bootstrap a "
            "different model than the one fitted. Refusing rather than reporting a t and p "
            "that are not the ones asked for.",
        "unsupported_option");
    return false;
  }
  return postestRefitMatchesFitM4("boottest", lm, reject);
}

namespace {

// Stata-style glob for a coefficient NAME: `*` any run, `?` one CHARACTER (a
// UTF-8 sequence, not a byte: Unicode names). The shared iterative matcher.
bool globMatchM4(const std::string& pat, const std::string& str) {
  return strutil::globMatch(pat, str);
}

// Does coefficient `name` belong to variable `var`? Two shapes, because that is
// how our coeftable actually names things (measured, not assumed): a continuous
// regressor is its own name (`beertax`), and a factor level is `<level>.<var>`
// (`1983.dyear`) — the same spelling Stata uses.
bool coefBelongsToVarM4(const std::string& name, const std::string& var) {
  if (name == var) return true;
  if (name.size() <= var.size() + 1) return false;
  if (name.compare(name.size() - var.size(), var.size(), var) != 0) return false;
  if (name[name.size() - var.size() - 1] != '.') return false;
  // Everything before the dot must be the level value (digits, or a negative /
  // decimal level), never another variable's name.
  for (size_t k = 0; k + var.size() + 1 < name.size(); ++k) {
    char c = name[k];
    if (!std::isdigit(static_cast<unsigned char>(c)) && c != '-' && c != '.') return false;
  }
  return true;
}

// The VARIABLE a coefficient came from: `beertax` -> "beertax",
// `1983.dyear` -> "dyear". Stata's `testparm x*` globs VARIABLE names and then
// tests every coefficient those variables produced, so the glob has to be
// matched here and not against the printed coefficient label.
std::string coefSourceVarM4(const std::string& name) {
  size_t dot = name.rfind('.');
  if (dot == std::string::npos || dot + 1 >= name.size()) return name;
  for (size_t k = 0; k < dot; ++k) {
    char c = name[k];
    if (!std::isdigit(static_cast<unsigned char>(c)) && c != '-' && c != '.') return name;
  }
  return name.substr(dot + 1);
}

bool isInterceptNameM4(const std::string& n) {
  return n == "(intercept)" || n == "_cons" || n == "cons";
}

}  // namespace

ojson runTestparm(const std::vector<std::string>& varlist, SessionState& session) {
  const auto& lmOpt = session.lastModel();
  if (!lmOpt.has_value()) return noPreviousEstimation();
  const LastModel& lm = *lmOpt;
  {  // BUG-203 — the same estimator gate `test` carries; testparm IS `test`.
    ojson reject;
    if (!postestEstimatorOkM4("testparm", lm, reject)) return reject;
  }
  if (varlist.empty())
    return engineErrorBlockM4("testparm: varlist required", "syntax_error");

  // Coefficient names come from the stored fit, in coeftable order, so the
  // constraint list reads in the order the user saw the model printed.
  std::vector<std::string> coefs;
  if (lm.result.has_value() && lm.result->contains("coeftable") &&
      (*lm.result)["coeftable"].is_array()) {
    for (const auto& row : (*lm.result)["coeftable"]) {
      std::string nm = row.value("name", std::string());
      if (!nm.empty()) coefs.push_back(nm);
    }
  }
  if (coefs.empty())
    return engineErrorBlockM4(
        "testparm: the last estimation exposes no coefficient names to expand against",
        "unimplemented");

  std::vector<std::string> picked;
  auto addOnce = [&picked](const std::string& n) {
    for (const auto& q : picked)
      if (q == n) return;
    picked.push_back(n);
  };

  for (const std::string& rawTok : varlist) {
    std::string tok = trimM4(rawTok);
    if (tok.empty()) continue;
    // `i(numlist).var` — the level-restricted factor spelling (paper 077's
    // `testparm i(2/4).topicn`; w2c_postest arms testparm_i23_c / testparm_i24_g):
    // exactly the coefficients of the listed levels of `var`. The numlist takes
    // `a/b` ranges and space/comma separated levels. Levels the model has no
    // coefficient for (the base, an absent level) are simply not tested; a list
    // that selects nothing is refused below.
    {
      static const std::regex reLevelRange(R"(^i\(([^()]*)\)\.([A-Za-z_]\w*)$)");
      std::smatch lm2;
      if (std::regex_match(tok, lm2, reLevelRange)) {
        std::vector<double> lv;
        std::string nl = lm2.str(1);
        for (char& ch : nl)
          if (ch == ',') ch = ' ';
        std::istringstream in(nl);
        std::string item;
        bool bad = false;
        while (in >> item) {
          const size_t slash = item.find('/');
          try {
            if (slash == std::string::npos) {
              lv.push_back(std::stod(item));
            } else {
              const double lo = std::stod(item.substr(0, slash));
              const double hi = std::stod(item.substr(slash + 1));
              if (hi < lo || hi - lo > 1e6) { bad = true; break; }
              for (double v = lo; v <= hi; v += 1.0) lv.push_back(v);
            }
          } catch (...) {
            bad = true;
            break;
          }
        }
        if (bad || lv.empty())
          return engineErrorBlockM4("testparm: invalid numlist in `" + tok + "'",
                                    "syntax_error");
        const size_t before = picked.size();
        for (const std::string& c : coefs) {
          double lvl = 0;
          std::string var;
          if (!cecon::postest::parse_level_ref(c, lvl, var) || var != lm2.str(2)) continue;
          if (std::find(lv.begin(), lv.end(), lvl) != lv.end()) addOnce(c);
        }
        if (picked.size() == before)
          return engineErrorBlockM4(
              "testparm: `" + tok + "' matches no coefficient in the last estimation",
              "bad_input");
        continue;
      }
    }
    // `i.`/`c.`/`ib#.`/`o.` prefixes name the SOURCE variable; the coefficients
    // they produce are what gets tested, so strip the prefix and match the rest.
    std::string bare = tok;
    for (const char* pfx : {"i.", "c.", "o.", "ibn."}) {
      size_t plen = std::strlen(pfx);
      if (bare.size() > plen && bare.compare(0, plen, pfx) == 0) {
        bare = bare.substr(plen);
        break;
      }
    }
    const bool isGlob = bare.find('*') != std::string::npos || bare.find('?') != std::string::npos;
    size_t before = picked.size();
    for (const std::string& c : coefs) {
      // The intercept is never swept in by a pattern — Stata's testparm tests
      // the named parameters, and `_cons` has to be asked for by name.
      if (isGlob) {
        // Stata's testparm globs VARIABLE names: `dyear*` -> every `NNNN.dyear`,
        // and `*1*` -> `x1` but NOT the level `1.c` (w2c_postest arm
        // testparm_wildcard_substr: df 1, F 1046.48 — matching the printed level
        // names too swept in 1.c and 1.g, df 3). The printed name is matched only
        // when the pattern itself spells a level (`198*.dyear`).
        const bool patternNamesLevel = bare.find('.') != std::string::npos;
        if (!isInterceptNameM4(c) &&
            (globMatchM4(bare, coefSourceVarM4(c)) ||
             (patternNamesLevel && globMatchM4(bare, c))))
          addOnce(c);
      } else if (coefBelongsToVarM4(c, bare)) {
        addOnce(c);
      }
    }
    if (picked.size() == before)
      return engineErrorBlockM4(
          "testparm: `" + tok + "' matches no coefficient in the last estimation",
          "bad_input");
  }

  // Delegate. Everything above is name resolution; the arithmetic, the vcov, the
  // r-class fill and the estimator gate all stay in the one place that owns them.
  std::string joined;
  for (const std::string& c : picked) {
    if (!joined.empty()) joined += ' ';
    joined += c;
  }
  return runTestCmd(joined, session);
}

namespace {

// A post-estimation name the model does not have. The engine raises it as
// EngineError("coef_not_found", "<name> not found") — one code for test, testparm
// and lincom, from the one resolver (src/estimators/postest_names.h) — and Stata
// answers the same thing with r(111): `test 9.c` -> "9.c not found", r(111)
// (w2c_postest.log arm test_level_not_in_model). Any other engine failure keeps the
// historical engine_error block.
ojson postestErrorBlockM4(const CobraError& ce) {
  if (ce.code == "coef_not_found")
    return ojson(eval::errorBlock("engine_error", ce.what(), std::nullopt, std::nullopt,
                                  std::nullopt, std::string("r(111)")));
  return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
}

// Split Stata's three `test` spellings into one restriction per entry:
//   test coeflist               -> each name is `name = 0`
//   test exp = exp              -> ONE equation (spaces allowed inside it)
//   test (spec) [(spec) ...]    -> each parenthesised spec is either of the above
// Parentheses INSIDE an equation are arithmetic and are kept: the old flattening
// (strip every paren, then split on spaces) turned `2.c = 0.5*(1.c+3.c)` into
// `2.c=0.5*1.c+3.c` — a different restriction, answered silently.
// Returns false (with `err`) on unbalanced parentheses or text between groups.
struct TestItem {
  bool joint;        // true: `name`; false: an equation for --constraint
  std::string text;
};
std::string collapseOpsM4(const std::string& in) {
  // BUG-015: whitespace around = + - * / is not significant.
  std::string out;
  for (size_t i = 0; i < in.size(); ++i) {
    const char c = in[i];
    if (c == '=' || c == '+' || c == '-' || c == '*' || c == '/') {
      while (!out.empty() && out.back() == ' ') out.pop_back();
      out += c;
      while (i + 1 < in.size() && (in[i + 1] == ' ' || in[i + 1] == '\t')) ++i;
    } else {
      out += (c == '\t') ? ' ' : c;
    }
  }
  return trimM4(out);
}
void addTestSpecM4(const std::string& specIn, std::vector<TestItem>& out) {
  const std::string spec = collapseOpsM4(specIn);
  if (spec.empty()) return;
  const size_t eq = spec.find('=');
  if (eq == std::string::npos) {
    std::string cur;
    for (char c : spec) {
      if (c == ' ' || c == ',') {
        if (!cur.empty()) out.push_back({true, cur});
        cur.clear();
      } else {
        cur += c;
      }
    }
    if (!cur.empty()) out.push_back({true, cur});
    return;
  }
  // `name = 0` stays a joint name, as it always has (the engine answers both the
  // same way; the joint spelling keeps the printed restriction `name = 0`).
  const std::string lhs = trimM4(spec.substr(0, eq));
  const std::string rhs = trimM4(spec.substr(eq + 1));
  bool rhsZero = false;
  try {
    size_t pos = 0;
    const double v = std::stod(rhs, &pos);
    rhsZero = pos == rhs.size() && v == 0.0;
  } catch (...) {
  }
  const bool bareName = !lhs.empty() && lhs.find_first_of(" +-*/()") == std::string::npos;
  if (rhsZero && bareName) out.push_back({true, lhs});
  else out.push_back({false, spec});
}
bool splitTestSpecsM4(const std::string& expr, std::vector<TestItem>& out, std::string& err) {
  const std::string e = trimM4(expr);
  if (e.empty() || e.front() != '(') {
    addTestSpecM4(e, out);
    return true;
  }
  size_t i = 0;
  while (i < e.size()) {
    while (i < e.size() && std::isspace(static_cast<unsigned char>(e[i]))) ++i;
    if (i >= e.size()) break;
    if (e[i] != '(') {
      err = "invalid syntax: text between parenthesised test specifications";
      return false;
    }
    int depth = 0;
    size_t j = i;
    for (; j < e.size(); ++j) {
      if (e[j] == '(') ++depth;
      else if (e[j] == ')' && --depth == 0) break;
    }
    if (j >= e.size()) {
      err = "unmatched '(' in test specification";
      return false;
    }
    addTestSpecM4(e.substr(i + 1, j - i - 1), out);
    i = j + 1;
  }
  return true;
}

// `test ..., accumulate` — SESSION STATE, by design (dispatcher.cpp's `test` arm).
// Stata tests the new restrictions jointly with the ones the previous `test`
// tested (w2c_postest arm test_2c_accumulate: `test 3.c` then `test 2.c, accumulate`
// equals `test 2.c 3.c`, F 6.3024 on (2, 54)). The previous test's restriction set
// is kept here after every successful `test`, REPLACED by the next plain `test` and
// extended by the next `test ..., accumulate`.
//
// Where it lives: a reserved r()-macro key (lead byte \x01, the convention
// sreturn's "\x01s:" established; `return list`/`return clear`/`return add` all skip
// such keys), so it dies with the session and with `clear all`/`clear results`, and
// never shows up as a user r() value. It is BOUND to the estimation it was built
// against by a fingerprint of the live e() block: a set recorded against an earlier
// model is never accumulated onto a new one (Stata's constraints belong to the
// estimation they were tested on).
constexpr const char* kTestAccumKey = "\x01test:accum";
std::string estFingerprintM4(const SessionState& session) {
  if (!session.lastEstBlock.has_value()) return std::string();
  return std::to_string(std::hash<std::string>{}(session.lastEstBlock->dump()));
}
std::vector<TestItem> loadAccumM4(const SessionState& session) {
  std::vector<TestItem> items;
  const std::string* raw = session.eval.rStrings.find(kTestAccumKey);
  if (raw == nullptr) return items;
  try {
    const ojson j = ojson::parse(*raw);
    if (j.value("fp", std::string()) != estFingerprintM4(session)) return items;
    for (const auto& it : j.value("items", ojson::array()))
      if (it.is_array() && it.size() == 2)
        items.push_back({it[0].get<std::string>() == "j", it[1].get<std::string>()});
  } catch (const nlohmann::json::exception&) {
  }
  return items;
}
void storeAccumM4(SessionState& session, const std::vector<TestItem>& items) {
  ojson j;
  j["fp"] = estFingerprintM4(session);
  j["items"] = ojson::array();
  for (const auto& it : items) j["items"].push_back(ojson::array({it.joint ? "j" : "c", it.text}));
  session.eval.rStrings.set(kTestAccumKey, j.dump());
}

// posted-matrix-wald: the live e() is a results set whose e(b)/e(V) PIET posted
// itself and the engine cannot re-fit -- csdid/drdid (cobraCmd "csdid"), or a
// posting with no fitted model behind it (`estat simple, post`, `ereturn post b
// V`). test/lincom/nlcom then read e(b)/e(V) as Stata's test.ado/lincom.ado do
// (w2ag log:1085 ag_asimple_test `test ATT` chi2 102.9610082305245).
bool postedWaldServesM4(const SessionState& session) {
  if (session.eval.findClassMatrix('e', "b") == nullptr ||
      session.eval.findClassMatrix('e', "V") == nullptr)
    return false;
  const auto& lm = session.lastModel();
  if (!lm.has_value()) return session.ePostedLive;
  // Fits whose e(b)/e(V) are PIET's own posting (no engine refit exists):
  // csdid/drdid, acreg (handlers_e3_acreg.cpp; w2j_e3_acreg.log:710 D01 `test x1`),
  // qreg2 and its posted margins (handlers_e3_qreg2.cpp; w2j_e3_qreg2.log T01, M02).
  // bootstrap (handlers_resample.cpp runBootstrap): e(V) is the replicate
  // covariance, which no engine refit reproduces.
  return lm->cobraCmd == "csdid" || lm->cobraCmd == "acreg" || lm->cobraCmd == "suest" ||
         lm->cobraCmd == "qreg2" || lm->cobraCmd == "qreg2_margins" || lm->cobraCmd == "bootstrap";
}

// The engine's `--posted FILE` mode (apps/cmds/cmds_postest.cpp load_posted_model):
// writes {names, b, V, df_r, n} from the live e() and runs `cobra VERB --posted FILE
// <combo>`. e(df_r) present -> the F / t reference, absent -> chi2 / normal.
ojson runPostedPostestM4(const std::string& verb, const std::vector<std::string>& combo,
                         SessionState& session) {
  const eval::EvalMatrix* b = session.eval.findClassMatrix('e', "b");
  const eval::EvalMatrix* V = session.eval.findClassMatrix('e', "V");
  ojson j;
  j["names"] = b->colnames;
  ojson bv = ojson::array();
  for (double x : b->data.empty() ? std::vector<double>() : b->data[0])
    bv.push_back(std::isnan(x) ? ojson(nullptr) : ojson(x));
  j["b"] = bv;
  ojson Vm = ojson::array();
  for (const auto& row : V->data) {
    ojson r = ojson::array();
    for (double x : row) r.push_back(std::isnan(x) ? ojson(nullptr) : ojson(x));
    Vm.push_back(r);
  }
  j["V"] = Vm;
  const double* dfr = session.eval.eScalars.find("df_r");
  j["df_r"] = (dfr != nullptr && std::isfinite(*dfr)) ? ojson(*dfr) : ojson(nullptr);
  // lincom.ado is `version 6.0`, and under it `test` reads the posting's own dof
  // rather than e(df_r): qreg2 records it (w2j_e3_qreg2.log L05 r(df) 201 where
  // e(df_r) is 196; handlers_e3_qreg2.cpp postQregE).
  if (verb == "lincom") {
    const auto& lmNow = session.lastModel();
    if (lmNow.has_value() && lmNow->cobraCmd == "qreg2" && lmNow->result.has_value() &&
        lmNow->result->contains("qreg2_lincom_dof") &&
        (*lmNow->result)["qreg2_lincom_dof"].is_number())
      j["df_r"] = (*lmNow->result)["qreg2_lincom_dof"];
  }
  const double* nObs = session.eval.eScalars.find("N");
  if (nObs != nullptr && std::isfinite(*nObs)) {
    j["n"] = *nObs;
  } else if (session.lastEstBlock.has_value() && session.lastEstBlock->contains("n_obs") &&
             (*session.lastEstBlock)["n_obs"].is_number()) {
    j["n"] = (*session.lastEstBlock)["n_obs"];
  } else {
    j["n"] = 0;
  }
  const std::string path = nextPathM4(session.workdir(), "posted_eb");
  {
    std::ofstream out(path);
    out << j.dump();
  }
  std::vector<std::string> argv = {verb, "--posted", path};
  argv.insert(argv.end(), combo.begin(), combo.end());
  try {
    nlohmann::json raw = runCobraJsonM4(argv);
    cecon::native_unlink(path);
    return ojson::parse(raw.dump());
  } catch (...) {
    cecon::native_unlink(path);
    throw;
  }
}

}  // namespace

ojson runTestCmd(const std::string& exprIn, SessionState& session) {
  return runTestCmd(exprIn, session, /*accumulate=*/false);
}

ojson runTestCmd(const std::string& exprIn, SessionState& session, bool accumulate) {
  const bool posted = postedWaldServesM4(session);
  const auto& lmOpt = session.lastModel();
  if (!posted && !lmOpt.has_value()) return noPreviousEstimationFor(session, true);
  // BUG-203: refuse before any parsing — the OLS-refit answer is not this
  // model's answer for any estimator outside kPostestOlsCmds.
  if (!posted) {
    ojson reject;
    if (!postestEstimatorOkM4("test", *lmOpt, reject)) return reject;
  }

  std::vector<TestItem> items;
  if (accumulate) items = loadAccumM4(session);
  {
    std::vector<TestItem> fresh;
    std::string err;
    if (!splitTestSpecsM4(exprIn, fresh, err)) return engineErrorBlockM4(err, "syntax_error");
    if (fresh.empty())
      return engineErrorBlockM4("test: a coefficient list or an equation is required",
                                "syntax_error");
    items.insert(items.end(), fresh.begin(), fresh.end());
  }

  // Combination-only flags. On §31's handle path the stored fit IS the model,
  // so nothing accumulated here may name it; runPostEstimationM4 prepends
  // lastModelBaseArgv only when it falls back to the full specification.
  // The items go in the order they were typed (each run of names as one --joint):
  // the engine numbers the restrictions that way, as Stata's "( 1)" lines and
  // r(dropped_#) do (BUG-1276: `test (2*x1=0) (x1=0)` drops constraint 2).
  std::vector<std::string> combo;
  for (size_t i = 0; i < items.size();) {
    if (!items[i].joint) {
      combo.push_back("--constraint");
      combo.push_back(items[i++].text);
      continue;
    }
    std::vector<std::string> run;
    for (; i < items.size() && items[i].joint; ++i) run.push_back(items[i].text);
    combo.push_back("--joint");
    combo.push_back(joinCommaM4(run));
  }

  combo.push_back("--tidy");
  ojson raw;
  try {
    raw = posted ? runPostedPostestM4("test", combo, session)
                 : runPostEstimationM4("test", combo, session, *lmOpt);
  } catch (const CobraError& ce) {
    return postestErrorBlockM4(ce);
  }
  ojson blk = cobraToEstimationM4(raw, "test", &session);
  // Restrictions that named only omitted coefficients were dropped by the engine,
  // and every one when R V R' is zero (BUG-1276); the dispatcher publishes r(drop)
  // from the count and captureRFromBlock r(dropped_#) from their numbers.
  if (raw.contains("dropped")) blk["dropped"] = raw["dropped"];
  if (raw.contains("dropped_rows")) blk["dropped_rows"] = raw["dropped_rows"];
  storeAccumM4(session, items);
  return blk;
}

// BUG-333: lincom/nlcom now take the parsed option vector. `level(#)` forwards
// to cobra's new --level (engine half landed this wave); lincom's eform family
// (eform/or/hr/irr/rrr/shr) is a DISPLAY metric applied to the returned block,
// exactly as `logit, or` is — Stata computes the linear combination and its CI
// in the linear metric and exponentiates for display, which is why the SE uses
// the delta factor exp(b) and t/p are left alone.
namespace {
const char* eformLabelM4(const std::string& k) {
  if (k == "or") return "odds_ratio";
  if (k == "hr") return "hazard_ratio";
  if (k == "irr") return "incidence_rate_ratio";
  if (k == "rrr") return "relative_risk_ratio";
  if (k == "shr") return "subhazard_ratio";
  return "exp_b";
}
void applyEformM4(ojson& block, const char* metric) {
  if (block.value("type", "") == "error") return;
  if (!block.contains("coeftable") || !block["coeftable"].is_array()) return;
  for (auto& row : block["coeftable"]) {
    if (!row.contains("b") || row["b"].is_null()) continue;
    const double e = std::exp(row["b"].get<double>());
    row["b"] = e;
    if (row.contains("se") && row["se"].is_number()) row["se"] = e * row["se"].get<double>();
    if (row.contains("ci_lo") && row["ci_lo"].is_number())
      row["ci_lo"] = std::exp(row["ci_lo"].get<double>());
    if (row.contains("ci_hi") && row["ci_hi"].is_number())
      row["ci_hi"] = std::exp(row["ci_hi"].get<double>());
  }
  block["coef_metric"] = metric;
}

// Items 4/5 (User ruling 2026-09-09): `margins ..., post` and
// `nlcom ..., post` REPLACE the posted estimation results wholesale --
// e(cmd) becomes "margins"/"nlcom" and _b[x] becomes the margin/combination
// afterwards. Same shape as eststo_esttab.cpp's runEstimatesUse (the
// canonical "replace e() wholesale" precedent) and the SAME order its own
// comment insists on: setLastModel() (republishes e(cmd)/e(depvar)/e(vce)
// and re-points eval.lastModelResult) BEFORE lastEstBlock, then
// estAddedClear() (the estadd bag belongs to the estimate being replaced).
// y/x/fe are left EMPTY on the new LastModel — postestEstimatorOkM4 (below)
// refuses every later lincom/test/predict verb BY NAME for a cobraCmd it
// does not recognize (neither postestOlsCmdsM4() nor postestGlmCmdsM4()
// list "margins"/"nlcom"), which turns a post-post replay into an honest
// named refusal instead of a silent wrong number computed from stale
// y/x/fe — leaving them stale would be exactly the silent-wrong shape this
// codebase's Doctrine gate ① forbids.
//
// KNOWN OPEN GAP (BUG-467, filed 2026-09-09, OPEN): setLastModel does NOT
// clear session.eval.eScalars/eStrings, so an e-scalar/e-string posted by
// an EARLIER model (oaxaca/pca/teffects/estadd) still answers after this
// publish. This helper does not fix that — it is a NEW CALLER of
// setLastModel, one more surface the open bug reaches, not a new instance
// of the bug itself.
void postEstimationBlockM4(SessionState& session, const ojson& block, const std::string& cmdName,
                           const std::string& depvar, const std::string& dataPath) {
  LastModel lm2;
  lm2.cobraCmd = cmdName;
  lm2.stataCmd = cmdName;
  lm2.depvar = depvar;
  lm2.dataPath = dataPath;
  lm2.result = block;
  session.setLastModel(lm2);
  session.lastEstBlock = block;
  session.estAddedClear();
}

// AER-B4 margins-dydx-OLS (2026-09-09) — `margins, dydx(x)` AFTER A PLAIN OLS FIT.
//
// Oracle: tests/eval/golden/aer_b4/pins_oracle_r.do:75-79 (log lines
// 199-224) fits `quietly regress price mpg weight` and asks for
// `margins, dydx(mpg) post`. Stata answers rc 0 with
//   dy/dx -49.51222  std. err. 86.15604  t -0.57  P>|t| 0.567
//   [95% conf. interval] -221.3025 .. 122.278   Number of obs = 74
// and every one of those columns is IDENTICAL to the `mpg` row of that
// regression's own coefficient table (measured on this build:
// b -49.51222066635501, se 86.1560388671846, t -0.5746807921692114,
// p 0.5673237265003643, ci -221.302482965428 .. 122.27804163271796). That is
// not a coincidence: a linear model's dy/dx IS its coefficient — no sample
// average enters the derivative, so the AME does not depend on the data at
// all, and the delta-method variance of a single coefficient is that
// coefficient's own variance. Stata's header says so too ("Model VCE: OLS",
// "Expression: Linear prediction, predict()"), and the t/CI it prints are the
// fit's own t-distribution ones, NOT the normal-approximation pair
// cobraToEstimationM4's BUG-006 AME remap builds for the GLM family.
//
// Why it is NOT enough to drop feols/regress from runMargins' kLinearCmds:
// cmd_margins (apps/cmds/cmds_postest.cpp:929) reads
// `std::string model = opt(a, "--model", "logit");` and its AME path computes
// the logit/probit/poisson family only — with no --model flag a continuous
// outcome like `price` would be pushed through a LOGISTIC AME, a silent wrong
// number strictly worse than the honest reject it replaced (Doctrine gate ①).
// So the answer is built HERE, from the fitted model's own already-published
// coefficient table (LastModel::result — the same payload macros.cpp's
// _b[]/_se[] resolver reads), and the engine is not called at all.
//
// Two shapes stay REFUSED, because for them the coefficient is NOT the
// marginal effect:
//   * an INTERACTED regressor. Measured on this build, `regress price
//     c.mpg##c.mpg weight` names its rows "(intercept)", "mpg", "mpg#mpg",
//     "weight", and `regress price c.mpg#c.weight mpg weight` names one
//     "mpg#weight". Wherever the requested variable appears inside a `#` term
//     the chain rule puts a sample average into dy/dx (AME = b_x +
//     b_{x#z}*mean(z)), so the bare coefficient is wrong. `dydx(*)` refuses
//     when ANY row is interacted, since it asks for every covariate.
//   * a requested name no coefficient belongs to — an empty margins table is
//     a silent no-op, so say which name found nothing.
// A FACTOR regressor IS answered: a `<level>.<var>` row carries the predicted
// difference between that level and the base level, which for a linear model
// is exactly that level's coefficient and its own SE. `<level>.<var>` is the
// spelling coefBelongsToVarM4 (above) and runMargins' own dydx row filter
// already key on, so one predicate covers both.
//
// Fills `out` with a payload shaped like cobra's own `margins --tidy` wire
// (estimator/n_obs/coeftable/coefs, no depvar — measured against the engine's
// GLM AME output) so the caller's shared tail — marginsplot caching, the dydx
// row filter, `post` — runs over it unchanged. Returns false and fills
// `reject` with the honest-refusal block instead when it cannot.
bool buildLinearAmeRawM4(const LastModel& lm, const std::vector<std::string>& dydxVars,
                         ojson& out, ojson& reject) {
  if (!lm.result.has_value() || !lm.result->contains("coeftable") ||
      !(*lm.result)["coeftable"].is_array() || (*lm.result)["coeftable"].empty()) {
    reject = engineErrorBlockM4(
        "margins, dydx(...): the fitted model's coefficient table is not in memory, so its "
        "marginal effects cannot be read from it.",
        "unsupported_option");
    return false;
  }
  const ojson& ct = (*lm.result)["coeftable"];

  auto rowName = [](const ojson& row) {
    return (row.contains("name") && row["name"].is_string()) ? row["name"].get<std::string>()
                                                             : std::string();
  };
  // Does `var` appear as one component of some `#` interaction row?
  auto isInteracted = [&ct, &rowName](const std::string& var) {
    for (const auto& row : ct) {
      const std::string name = rowName(row);
      if (name.find('#') == std::string::npos) continue;
      size_t start = 0;
      for (;;) {
        const size_t hash = name.find('#', start);
        const std::string part =
            (hash == std::string::npos) ? name.substr(start) : name.substr(start, hash - start);
        if (coefBelongsToVarM4(part, var)) return true;
        if (hash == std::string::npos) break;
        start = hash + 1;
      }
    }
    return false;
  };
  // `what` names the request (a variable, or "*" for every covariate); `who`
  // names what is actually interacted, so the `*` form reads as a sentence.
  auto interactionReject = [](const std::string& what, const std::string& who) {
    return engineErrorBlockM4(
        "margins, dydx(" + what + ") after a linear model is not supported while " + who +
            " enters an interaction: the marginal effect is then the coefficient plus the "
            "interacted terms weighted by the sample, not the coefficient itself. Fit without "
            "the interaction, or use `margins, at(...)` at the point of interest.",
        "unsupported_option");
  };

  ojson coeftable = ojson::array();
  ojson coefs = ojson::array();
  auto take = [&coeftable, &coefs, &rowName](const ojson& row) {
    coeftable.push_back(row);
    ojson c;
    c["name"] = rowName(row);
    // Mirrors cobra's own AME wire shape, which marginsplot (graph_m4.cpp)
    // reads by `dydx`/`se` — the coeftable copy above is what the block and
    // `post` read.
    c["dydx"] = row.contains("b") ? row["b"] : ojson(nullptr);
    c["se"] = row.contains("se") ? row["se"] : ojson(nullptr);
    c["t"] = row.contains("t") ? row["t"] : ojson(nullptr);
    coefs.push_back(c);
  };

  if (dydxVars.empty()) {
    // `dydx(*)` (and `dydx()` with only a `*` token) — every covariate, which
    // is also what Stata prints, and never the intercept.
    for (const auto& row : ct) {
      const std::string name = rowName(row);
      if (name.empty() || isInterceptNameM4(name)) continue;
      if (name.find('#') != std::string::npos) {
        reject = interactionReject("*", "`" + name + "`");
        return false;
      }
      take(row);
    }
  } else {
    for (const auto& var : dydxVars) {
      if (isInteracted(var)) {
        reject = interactionReject(var, var);
        return false;
      }
    }
    // Iterated over the MODEL's row order (not the request's), matching the
    // order the engine's GLM AME path returns its rows in.
    std::vector<bool> matched(dydxVars.size(), false);
    for (const auto& row : ct) {
      const std::string name = rowName(row);
      if (name.empty() || isInterceptNameM4(name)) continue;
      for (size_t i = 0; i < dydxVars.size(); ++i) {
        if (!coefBelongsToVarM4(name, dydxVars[i])) continue;
        matched[i] = true;
        take(row);
        break;
      }
    }
    for (size_t i = 0; i < dydxVars.size(); ++i) {
      if (!matched[i]) {
        reject = engineErrorBlockM4(
            "margins, dydx(" + dydxVars[i] + "): `" + dydxVars[i] +
                "` is not a covariate of the last estimation.",
            "unsupported_option");
        return false;
      }
    }
  }
  if (coeftable.empty()) {
    reject = engineErrorBlockM4(
        "margins, dydx(...): the last estimation has no covariate to take a marginal effect of.",
        "unsupported_option");
    return false;
  }

  out = ojson::object();
  out["estimator"] = "margins";
  if (lm.result->contains("n_obs")) out["n_obs"] = (*lm.result)["n_obs"];
  out["coeftable"] = coeftable;
  out["coefs"] = coefs;
  return true;
}
}  // namespace

ojson runLincom(const std::string& expr, SessionState& session,
                const std::vector<std::pair<std::string, eval::OptValue>>& opts) {
  const bool posted = postedWaldServesM4(session);  // posted-matrix-wald
  const auto& lmOpt = session.lastModel();
  if (!posted && !lmOpt.has_value()) return noPreviousEstimationFor(session, true);
  if (!posted) {  // BUG-203 — see postestEstimatorOkM4.
    ojson reject;
    if (!postestEstimatorOkM4("lincom", *lmOpt, reject)) return reject;
  }
  ojson raw;
  try {
    std::vector<std::string> argv = {"--exp", expr, "--tidy"};
    for (const auto& [k, v] : opts)
      if (k == "level" && !v.isBool) { argv.push_back("--level"); argv.push_back(v.strValue); break; }
    raw = posted ? runPostedPostestM4("lincom", argv, session)
                 : runPostEstimationM4("lincom", argv, session, *lmOpt);
  } catch (const CobraError& ce) {
    return postestErrorBlockM4(ce);
  }
  ojson blk = cobraToEstimationM4(raw, "lincom", &session);
  // A normal-reference combination (the engine's df 0: an ML fit, or the 2SLS
  // refit without `small`) has no degrees of freedom, and Stata's lincom posts no
  // r(df) then:
  // w2s_multiway_class.log:840 mw_iv_c2_lincom r(df) `.`, w2o_rnames.log:125 N04
  // after logit `.`. A t-reference fit always has a positive dof.
  if (blk.contains("df") && blk["df"].is_number() && blk["df"].get<double>() == 0.0)
    blk["df"] = nullptr;
  // Two eform-family options at once is r(198) in Stata (measured: `lincom unemp,
  // or hr` -> 198, while each alone is rc 0 — including after `regress`, where the
  // exponentiated OLS coefficient is meaningless but Stata still prints it, so we
  // do not second-guess the estimator either).
  std::string metricKey;
  int eformCount = 0;
  bool coef = false;
  for (const auto& [k, v] : opts) {
    if ((k == "eform" || k == "or" || k == "hr" || k == "irr" || k == "rrr" || k == "shr") &&
        v.isBool && v.boolValue) {
      ++eformCount;
      if (metricKey.empty()) metricKey = k;
    }
    if (k == "coef" && v.isBool && v.boolValue) coef = true;
  }
  if (eformCount > 1)
    return engineErrorBlockM4("option `" + metricKey + "` may not be combined with another "
                              "eform option", "unsupported_option");
  // `coef` excludes the eform family (StataBE 19.5 `lincom x1, or coef`: "only one
  // of options or or coef is allowed", r(198)).
  if (coef && eformCount == 1)
    return engineErrorBlockM4("only one of options " + metricKey + " or coef is allowed",
                              "unsupported_option");
  // BUG-1480: after `logistic` lincom reports odds ratios unless `coef` is given --
  // lincom.ado sets `or` when e(cmd) is logistic, whichever eform option was typed
  // (StataBE 19.5, p-est-b/lg2.do and scratch-laneA2/agents/post/l1.do: `lincom x1`
  // "Odds ratio" 4.403289 (1.051442), r(estimate) 4.4032885449567045 = exp(_b[x1]),
  // r(se) 1.0514416968201232, r(lb)/r(ub) the exponentiated interval; `lincom x1,
  // irr` the same "Odds ratio" table; `lincom x1, coef` 1.482352 (.2387856)).
  const std::string* ecmd = session.eval.eStrings.find("cmd");
  if (!coef && ecmd != nullptr && *ecmd == "logistic") {
    metricKey = "or";
    eformCount = 1;
  }
  if (eformCount == 1) applyEformM4(blk, eformLabelM4(metricKey));
  return blk;
}

ojson runNlcom(const std::string& exprIn, SessionState& session,
               const std::vector<std::pair<std::string, eval::OptValue>>& opts) {
  const auto& lmOpt = session.lastModel();
  if (!lmOpt.has_value()) return noPreviousEstimation();
  const LastModel& lm = *lmOpt;
  {  // BUG-203 — see postestEstimatorOkM4.
    ojson reject;
    if (!postestEstimatorOkM4("nlcom", lm, reject)) return reject;
  }

  std::string raw = trimM4(exprIn);
  // w2c_postest family 5: Stata's `nlcom (name1: exp1) (name2: exp2) ...` takes
  // SEVERAL combinations, each in its own balanced paren pair (paper 077:
  // `nlcom (Info: _b[_cons]) (Click: _b[_cons]+_b[1.clicktreat]) ...`, arm
  // nlcom_multi_nopost_rtable; paper 044's `..., post`, arm nlcom_multi_post).
  // The string splits into groups ONLY when it is nothing but top-level paren
  // groups; a leading paren that is expression grouping (`(_b[x1]+_b[x2])/2`)
  // leaves text after its group, so it stays one expression. One group is Item 5
  // defect (a)'s whole-string wrap (User ruling 2026-09-09): `(r: expr)` is the
  // single labelled combination; freeze/nlcom.json's bare `_b[x1]/_b[x2]` has no
  // leading paren and is untouched.
  std::vector<std::string> specs;
  if (!raw.empty() && raw.front() == '(') {
    size_t i = 0;
    bool allGroups = true;
    std::vector<std::string> groups;
    while (i < raw.size()) {
      while (i < raw.size() && std::isspace(static_cast<unsigned char>(raw[i]))) ++i;
      if (i >= raw.size()) break;
      if (raw[i] != '(') { allGroups = false; break; }
      int depth = 0;
      size_t j = i;
      for (; j < raw.size(); ++j) {
        if (raw[j] == '(') ++depth;
        else if (raw[j] == ')' && --depth == 0) break;
      }
      if (j >= raw.size()) { allGroups = false; break; }
      groups.push_back(trimM4(raw.substr(i + 1, j - i - 1)));
      i = j + 1;
    }
    if (allGroups && !groups.empty()) specs = groups;
  }
  if (specs.empty()) specs.push_back(raw);
  std::vector<std::string> exprArgs;
  {
    std::regex lblRe(R"(^(\w+)\s*:\s*(.+)$)");
    for (size_t k = 0; k < specs.size(); ++k) {
      std::string label = "_nl_" + std::to_string(k + 1);
      std::string body = specs[k];
      std::smatch m;
      if (std::regex_match(specs[k], m, lblRe)) {
        label = m[1].str();
        body = trimM4(m[2].str());
      }
      // e10_nlcom (w2g_ebmat.do:648, log:1311-1338): an element of e(b) is a
      // parameter, not a constant. Stata differentiates the combination with
      // respect to it, so `el(e(b),1,k)` and `e(b)[1,k]` give the same estimate
      // and se as `_b[<column k>]` (measured). They are rewritten to that name
      // (the full `eq:name` stripe, as e(b) carries it). e(V) is not a
      // parameter, so `el(e(V),...)` stays a constant and is left.
      static const std::regex ebElement(
          R"(\bel\(\s*e\(\s*b\s*\)\s*,\s*1\s*,\s*([0-9]+)\s*\)|\be\(\s*b\s*\)\s*\[\s*1\s*,\s*([0-9]+)\s*\])");
      if (std::regex_search(body, ebElement)) {
        const std::optional<eval::EvalMatrix> eb =
            session.eval.resolveEMatrix("b", nullptr, nullptr, eval::EvalSession::EReader::kNames);
        std::string translated;
        size_t offset = 0;
        for (std::sregex_iterator it(body.begin(), body.end(), ebElement), end; it != end; ++it) {
          const std::smatch& el = *it;
          const size_t column = std::stoull(el[1].matched ? el.str(1) : el.str(2));
          if (!eb.has_value() || column == 0 || column > eb->colnames.size())
            return engineErrorBlockM4("nlcom: " + el.str(0) + " is not an element of e(b)",
                                      "syntax_error");
          translated += body.substr(offset, static_cast<size_t>(el.position()) - offset);
          translated += "_b[" + eb->colnames[column - 1] + "]";
          offset = static_cast<size_t>(el.position() + el.length());
        }
        body = translated + body.substr(offset);
      }
      exprArgs.push_back("--expr");
      exprArgs.push_back(label + "=" + body);
    }
  }

  // Item 5 defect (b), User ruling 2026-09-09: `post` moved from
  // `unimplemented` to `known` in kVocabNlcom (dispatcher.cpp) — read it
  // here the same way `level` is read just below.
  bool postOpt = false;
  for (const auto& [k, v] : opts) {
    if (k == "post" && (!v.isBool || v.boolValue)) postOpt = true;
  }

  // BS-098/BS-013 (BETA A4): shares lastModelBaseArgv with test/lincom
  // (above, now via runPostEstimationM4's fallback) — nlcom ALSO inherits
  // vcov/cluster, not just fe (previously this hand-rolled the same
  // {data,y,x,fe} skeleton WITHOUT vcov/cluster, duplicating and diverging
  // from lastModelBaseArgv). On the §31 handle path the same guarantee holds
  // structurally instead: the stored model carries its vcov as fitted, and a
  // postest passes no --vcov at all, so there is no default to fall back to.
  ojson rawJson;
  try {
    std::vector<std::string> nargv = exprArgs;
    nargv.push_back("--tidy");
    for (const auto& [k, v] : opts)
      if (k == "level" && !v.isBool) { nargv.push_back("--level"); nargv.push_back(v.strValue); break; }
    rawJson = runPostEstimationM4("nlcom", nargv, session, lm);
  } catch (const CobraError& ce) {
    return postestErrorBlockM4(ce);
  }
  ojson blk = cobraToEstimationM4(rawJson, "nlcom", &session);
  // dispatcher.cpp's captureRFromBlock (Branch 2) still runs on the RETURNED
  // block regardless of postOpt — Stata's nlcom is BOTH r-class (always)
  // and, under `post`, ALSO e-class. This only adds the e() publish.
  if (postOpt) {
    postEstimationBlockM4(session, blk, "nlcom", lm.depvar, lm.dataPath);
  }
  // FU-410: the engine's G V G' (cmd_nlcom `V`, in the order of its coefs) rides
  // to the dispatcher's nlcom arm, which posts it as r(V) and removes the key
  // before the block is shown; it is attached after the `post` publish, so e()
  // never carries it.
  if (rawJson.is_object() && rawJson.contains("V") && rawJson["V"].is_array())
    blk["nlcom_V"] = rawJson["V"];
  return blk;
}

// ===========================================================================
// margins

namespace {

// A margins refusal carrying an explicit Stata rc: coarseRc reads the `r(NNN)`
// reason before the code (BUG-187), so `display _rc` after `capture margins ...`
// is never the generic 1 on these paths.
ojson marginsRefusalM4(const std::string& message, int rc,
                       const std::string& code = "unsupported_option") {
  return ojson(eval::errorBlock(code, message, std::nullopt, std::nullopt, std::nullopt,
                                "r(" + std::to_string(rc) + ")"));
}

// What a served margins form knows about itself for r() (FU-179, FU-276).
// One engine `--at` spec (`v=val[,v2=val2]`, parseAtNumlistM4) with each numeric
// value replaced by float(value): Stata evaluates an at() value at float
// precision whatever the variable's storage type (w2v_margins_siblings
// FU668_at_dvar_atmeans_b/_rt, `margins, at(dvar=0.1) atmeans` on a double dvar
// is 4.921007387529768 at float(0.1), 4.33e-10 away at the double 0.1; the grid
// applies the same rule, posthest.h margins_grid). A value that is not a plain
// number goes through unchanged for the engine to judge.
std::string floatAtSpecM4(const std::string& spec) {
  std::string out;
  size_t pos = 0;
  while (pos <= spec.size()) {
    const size_t comma = spec.find(',', pos);
    std::string tok = spec.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
    const size_t eq = tok.find('=');
    if (eq != std::string::npos) {
      const std::string val = trimM4(tok.substr(eq + 1));
      char* end = nullptr;
      const double v = val.empty() ? 0.0 : std::strtod(val.c_str(), &end);
      if (!val.empty() && end != nullptr && *end == '\0' && std::isfinite(v)) {
        char buf[40];
        std::snprintf(buf, sizeof(buf), "%.17g", static_cast<double>(static_cast<float>(v)));
        tok = tok.substr(0, eq + 1) + buf;
      }
    }
    out += (out.empty() && pos == 0 ? "" : ",") + tok;
    if (comma == std::string::npos) break;
    pos = comma + 1;
  }
  return out;
}

// BUG-1449: what margins' r() records about the FIT (_marg_work.class
// ._parse_vce :2717-2721 and its return list), read while e() is still the
// fit's: `post` replaces e() before r() is written on some paths. Measured on
// StataBE 19.5 (scratch-laneA/agents/p-margins s1.do, after `regress y x1 x2
// i.c`): r(est_cmd) regress, r(est_cmdline) "regress y x1 x2 i.c",
// r(model_vce) ols (robust/cluster after vce(robust)/vce(cluster id), with
// r(model_vcetype) Robust; oim after logit), r(predict1_label) "Linear
// prediction" ("Pr(d)" after `logit d ...`), r(cmdline) "margins , dydx(x1)" --
// the command word, a blank, then the rest as typed ("margins " when bare).
struct MarginsFitInfoM4 {
  bool captured = false;
  std::string estCmd, estCmdline, modelVce, modelVcetype;
  std::string predictLabel;          // "" = this fit's default prediction is not known here
  std::string cmdline;               // "" = the typed line is not known
  std::vector<std::string> ebNames;  // e(b)'s stripe: r(Jacobian)'s columns
  std::vector<double> ebValues;      // e(b) itself (the Jacobian's check)
};

struct MarginsRInfoM4 {
  bool leastSquares = false;  // a least-squares fit: t(e(df_r))-referenced, r(df_r) posted
  bool dydx = false;          // a dydx() form: r(derivatives)/r(xvars)
  std::string xvars;          // the dydx() varlist as given; "" = not known exactly (dydx(*))
  int kMargins = 0;           // terms in the marginlist (r(k_margins))
  int atPoints = 0;           // at() points, atmeans alone counting one (r(at), r(atstats#))
  bool cluster = false;       // a clustered fit (r(N_clust), r(clustvar))
  // BUG-1449: the rest of Stata's set, posted when `fit` was captured (the
  // forms runMargins serves; glm/qreg2 margins keep their historical set).
  MarginsFitInfoM4 fit;
  std::string title;              // r(title); "" = not known
  int kAt = 0;                    // r(k_at): at() options, atmeans alone counting one
  int kBy = 1;                    // r(k_by): over() groups
  int continuous = -1;            // r(continuous): 1 every dydx() variable continuous, 0 not, -1 unknown
  std::string bRow = "r1";        // r(b)'s row name
  std::vector<std::vector<double>> jacobian;  // r(Jacobian) rows over fit.ebNames; empty = unknown
  bool nKnown = false;            // r(_N): r(N) per column, 0 for a dydx() base level
};

// _marg_work.class :900-913 titles the results by `.noasobs` (:4001-4016): no
// covariate is left at its observed values -- atmeans, or at() fixing every
// covariate of e(b), the marginlist factor counting as fixed (StataBE 19.5,
// s1.do/s2.do: `margins, atmeans` and at(x1=1 x2=2 c=1) "Adjusted predictions",
// at(x1=(2 4)) and `margins c` "Predictive margins", `dydx(x1) atmeans`
// "Conditional marginal effects", `dydx(x1) at(x2=1)` "Average marginal
// effects"). "" when e(b) names a term that is not read here.
std::string marginsTitleM4(bool dydx, bool atmeans, const std::string& atText,
                           const std::string& marginVar, const std::vector<std::string>& ebNames) {
  bool noAsObs = atmeans;
  if (!noAsObs && !trimM4(atText).empty()) {
    const cecon::MarginsAtParse p = cecon::margins_parse_at(atText);
    if (p.rc != 0 || ebNames.empty()) return std::string();
    std::set<std::string> fixed(p.vars.begin(), p.vars.end());
    if (!marginVar.empty()) fixed.insert(marginVar);
    bool all = true;
    for (const auto& nm : ebNames) {
      const size_t colon = nm.find(':');
      bool ok = true;
      for (const auto& a : cecon::margins_parse_column(colon == std::string::npos ? nm : nm.substr(colon + 1), &ok))
        all = all && fixed.count(a.var) > 0;
      if (!ok) return std::string();
    }
    noAsObs = all;
  }
  if (dydx) return noAsObs ? "Conditional marginal effects" : "Average marginal effects";
  return noAsObs ? "Adjusted predictions" : "Predictive margins";
}

// The names Stata posts in r() for THIS margins invocation (RULING.md C.3/D.3;
// margins's help), each measured on live StataBE 19.5 (2026-10-13,
// `return list` after `margins`, `margins t`, `margins, dydx(x1)`, `margins,
// at(x1=5)`, `margins, atmeans` after `regress y x1 x2 i.t`, and `margins,
// dydx(x1)` after `logit d x1 x2`). Declared whether or not PIET posts each one:
// rTableAfterCommand turns declared-but-absent into a named r(111), never `.`.
// The cluster pair is the help file's, not measured: declaring it only makes an
// unposted read refuse.
std::vector<std::string> marginsDeclaredRM4(const MarginsRInfoM4& info) {
  std::vector<std::string> names = {
      "N",          "level",      "k_at",          "k_by",        "k_predict", "k_margins",
      "cmd",        "cmdline",    "est_cmdline",   "est_cmd",     "emptycells", "mcmethod",
      "predict1_label", "expression", "vcetype",   "vce",         "model_vce", "title",
      "table",      "chainrule",  "V",             "Jacobian",    "error",     "b",
      "_N"};
  if (info.leastSquares) names.push_back("df_r");  // posted after regress, absent after logit
  if (info.kMargins > 0) names.push_back("margins");
  if (info.dydx) {
    // r(continuous) is absent when a dydx() variable is a factor (s1.do: dydx(c),
    // dydx(x1 c), dydx(*) after `regress y x1 x2 i.c`).
    if (info.continuous != 0) names.push_back("continuous");
    names.push_back("derivatives");
    names.push_back("xvars");
  }
  // After vce(robust) / vce(cluster) (s1.do J/K); not posted when e(vcetype) is empty.
  if (!info.fit.modelVcetype.empty()) names.push_back("model_vcetype");
  if (info.atPoints > 0) {
    names.push_back("at");
    for (int i = 1; i <= info.atPoints; ++i) names.push_back("atstats" + std::to_string(i));
  }
  if (info.cluster) {
    names.push_back("N_clust");
    names.push_back("clustvar");
  }
  return names;
}

// FU-179/FU-276: margins is r-class. Every served form replaces r() wholesale --
// a stale r() from the previous command must not answer for it -- posts the
// results this handler knows exactly, and declares Stata's set for the
// invocation (marginsDeclaredRM4). Posted: r(N), r(level) (margins takes no
// level(); `set level`), r(k_margins), r(df_r) after a least-squares fit (its
// e(df_r): StataBE `margins` after `regress` posts 52 = e(df_r)), r(cmd),
// r(derivatives)/r(xvars) for dydx(), and -- when `names` is non-empty -- r(b)
// (1 x k, row `r1`), r(V) (k x k) and r(table) (9 x k; posted only when the
// printed p-values and intervals reproduce at the declared level and reference,
// handlers::buildRTableFromBlock's own check). An off-diagonal element no engine
// path computed arrives as NaN (Stata's missing), never 0: a zero covariance
// would be a number nobody measured.
void postMarginsRClassM4(SessionState& session, const ojson& block, const MarginsRInfoM4& info,
                         const std::vector<std::string>& names, const std::vector<double>& b,
                         const std::vector<std::vector<double>>& V,
                         const ojson& tableRows = ojson::array()) {
  auto& ev = session.eval;
  // e(df_r) is the fit's (the key macros.cpp's resolveEScalar reads off the
  // model result, else a posted e-scalar): read before any `post` replaces e().
  double dfR = std::numeric_limits<double>::quiet_NaN();
  if (info.leastSquares) {
    const auto& lmNow = session.lastModel();
    if (lmNow.has_value() && lmNow->result.has_value() && lmNow->result->is_object() &&
        lmNow->result->contains("df_r") && (*lmNow->result)["df_r"].is_number()) {
      dfR = (*lmNow->result)["df_r"].get<double>();
    } else if (const double* dfrE = ev.eScalars.find("df_r")) {
      dfR = *dfrE;
    }
    if (!(dfR > 0)) dfR = std::numeric_limits<double>::quiet_NaN();
  }
  eval::replaceRClass(ev);
  if (block.contains("n_obs") && block["n_obs"].is_number()) {
    ev.rValues.set("N", block["n_obs"].get<double>());
  }
  ev.rValues.set("level", static_cast<double>(session.level));
  ev.rValues.set("k_margins", static_cast<double>(info.kMargins));
  if (std::isfinite(dfR)) ev.rValues.set("df_r", dfR);
  ev.rStrings.set("cmd", "margins");
  if (info.dydx) {
    ev.rStrings.set("derivatives", "dy/dx");
    if (!info.xvars.empty()) ev.rStrings.set("xvars", info.xvars);
  }
  // BUG-1449: the rest of the set (s1.do return lists). The method macros are
  // constants of every form served here: the delta method (no vce()), no
  // multiple-comparison adjustment, emptycells(strict), the default prediction.
  if (info.fit.captured) {
    ev.rValues.set("k_at", static_cast<double>(info.kAt));
    ev.rValues.set("k_by", static_cast<double>(info.kBy));
    ev.rValues.set("k_predict", 1.0);
    ev.rStrings.set("vce", "delta");
    ev.rStrings.set("vcetype", "Delta-method");
    ev.rStrings.set("expression", "predict()");
    ev.rStrings.set("mcmethod", "noadjust");
    ev.rStrings.set("emptycells", "strict");
    if (!info.title.empty()) ev.rStrings.set("title", info.title);
    if (!info.fit.estCmd.empty()) ev.rStrings.set("est_cmd", info.fit.estCmd);
    if (!info.fit.estCmdline.empty()) ev.rStrings.set("est_cmdline", info.fit.estCmdline);
    if (!info.fit.modelVce.empty()) ev.rStrings.set("model_vce", info.fit.modelVce);
    if (!info.fit.modelVcetype.empty()) ev.rStrings.set("model_vcetype", info.fit.modelVcetype);
    if (!info.fit.predictLabel.empty()) ev.rStrings.set("predict1_label", info.fit.predictLabel);
    if (!info.fit.cmdline.empty()) ev.rStrings.set("cmdline", info.fit.cmdline);
    if (info.dydx && info.continuous == 1) ev.rStrings.set("continuous", "continuous");
  }
  if (!names.empty() && b.size() == names.size() && V.size() == names.size()) {
    eval::EvalMatrix rb;
    rb.rownames = {info.bRow};
    rb.colnames = names;
    rb.data.push_back(b);
    eval::EvalMatrix rv;
    rv.rownames = names;
    rv.colnames = names;
    rv.data = V;
    ev.rMatrices.emplace_back("b", std::move(rb));
    ev.rMatrices.emplace_back("V", std::move(rv));
    if (tableRows.is_array() && tableRows.size() == names.size()) {
      ojson tb;
      tb["estimator"] = "margins_rtable";
      if (std::isfinite(dfR)) tb["df_r"] = dfR;
      tb["coeftable"] = tableRows;
      (void)postRTableFromBlock(session, tb, static_cast<double>(session.level));
    }
    // BUG-1449 matrices (s1.do/s2.do): r(error) 0 per column (every column was
    // computed); r(_N) the observations behind each column, 0 for a dydx() base
    // level (`x1 0b.c 1.c ...` -> 60 0 60 ...); r(chainrule) 1 x 3 (1, k, k) for
    // a single-equation e(b) whose last column is _cons (7 columns after `regress
    // y x1 x2 i.c`, 3 after `logit d x1 x2`; absent after a noconstant fit);
    // r(Jacobian) d(column)/d(e(b)), one row per column over e(b)'s stripe.
    if (info.fit.captured) {
      auto row1 = [](const std::vector<std::string>& cols, std::vector<double> vals) {
        eval::EvalMatrix m;
        m.rownames = {"r1"};
        m.colnames = cols;
        m.data.push_back(std::move(vals));
        return m;
      };
      ev.rMatrices.emplace_back("error", row1(names, std::vector<double>(names.size(), 0.0)));
      if (info.nKnown && block.contains("n_obs") && block["n_obs"].is_number()) {
        const double n = block["n_obs"].get<double>();
        std::vector<double> nv;
        // A dydx() column with no at() point is r(_N)'s equation over `_cons`
        // (s1.do `matrix list r(_N)` after dydx(x1 c): `x1:_cons 0b.c:_cons ...`).
        std::vector<std::string> ncols = names;
        if (info.dydx)
          for (auto& nm : ncols)
            if (nm.find(':') == std::string::npos) nm += ":_cons";
        for (const auto& nm : names) {
          // A dydx() base level: the head's level flags carry `b` but not `bn`.
          const std::string head = nm.substr(0, nm.find(':'));
          const size_t dot = head.find('.');
          const std::string flags = dot == std::string::npos ? std::string() : head.substr(0, dot);
          const bool base = info.dydx && flags.find('b') != std::string::npos &&
                            flags.find("bn") == std::string::npos;
          nv.push_back(base ? 0.0 : n);
        }
        ev.rMatrices.emplace_back("_N", row1(ncols, std::move(nv)));
      }
      const std::vector<std::string>& eb = info.fit.ebNames;
      auto eqOf = [](const std::string& s) {
        const size_t c = s.find(':');
        return c == std::string::npos ? std::string() : s.substr(0, c);
      };
      const bool oneEq = !eb.empty() && std::all_of(eb.begin(), eb.end(), [&](const std::string& s) {
                           return eqOf(s) == eqOf(eb.front());
                         });
      if (oneEq && eb.back().substr(eb.back().find(':') == std::string::npos ? 0 : eb.back().find(':') + 1) ==
                       "_cons") {
        const double k = static_cast<double>(eb.size());
        ev.rMatrices.emplace_back("chainrule", row1({"c1", "c2", "c3"}, {1.0, k, k}));
      }
      if (!eb.empty() && info.jacobian.size() == names.size() &&
          std::all_of(info.jacobian.begin(), info.jacobian.end(),
                      [&eb](const std::vector<double>& r) { return r.size() == eb.size(); })) {
        eval::EvalMatrix jm;
        jm.rownames = names;
        jm.colnames = eb;
        jm.data = info.jacobian;
        ev.rMatrices.emplace_back("Jacobian", std::move(jm));
      }
    }
  }
  eval::declareRPosted(ev, "margins", marginsDeclaredRM4(info));
}

// BUG-1449: the fit's half of margins' r() set (MarginsFitInfoM4), captured on
// entry. r(predict1_label) is posted for the default predictions measured (s1.do,
// s3.do): the linear prediction after the least-squares family and `xtpoisson,
// fe`, Pr(depvar) after logit/probit, "Predicted number of events" after poisson.
MarginsFitInfoM4 marginsFitInfoM4(SessionState& session) {
  MarginsFitInfoM4 f;
  f.captured = true;
  const auto& ev = session.eval;
  if (const std::string* s = ev.eStrings.find("cmd")) f.estCmd = *s;
  if (const std::string* s = ev.eStrings.find("cmdline")) f.estCmdline = *s;
  if (const std::string* s = ev.eStrings.find("vce")) f.modelVce = *s;
  if (const std::string* s = ev.eStrings.find("vcetype")) f.modelVcetype = *s;
  const auto& lm = session.lastModel();
  if (lm.has_value()) {
    if (postestOlsCmdsM4().count(lm->cobraCmd) || lm->cobraCmd == "xtpois")
      f.predictLabel = "Linear prediction";
    else if ((lm->cobraCmd == "logit" || lm->cobraCmd == "probit" || lm->cobraCmd == "cloglog") &&
             !lm->depvar.empty())
      f.predictLabel = "Pr(" + lm->depvar + ")";  // cloglog: agents/post/cl.do MD "Pr(yb)"
    else if (lm->cobraCmd == "poisson")
      f.predictLabel = "Predicted number of events";
    else if (lm->cobraCmd == "xtlogit" && !lm->depvar.empty())
      f.predictLabel = "Pr(" + lm->depvar + "|fixed effect is 0)";  // clogit_p.ado's pu0
  }
  // margins' `0` with its leading blanks dropped, after "margins " (the leaf's
  // macro-expanded line, as the fit hook's e(cmdline) reads it).
  const std::string& s = session.leafCmdExpanded;
  const size_t i = s.find_first_not_of(" \t");
  if (i != std::string::npos && s.compare(i, 7, "margins") == 0 &&
      (i + 7 == s.size() || s[i + 7] == ' ' || s[i + 7] == '\t' || s[i + 7] == ',')) {
    const size_t k = s.find_first_not_of(" \t", i + 7);
    f.cmdline = "margins " + (k == std::string::npos ? std::string() : s.substr(k));
  }
  const eval::EvalSession::EMatrixAnswer eb =
      session.eval.resolveEMatrixFor("b", eval::EvalSession::EReader::kCopy);
  if (eb.matrix.has_value() && eb.matrix->data.size() == 1 &&
      eb.matrix->data[0].size() == eb.matrix->colnames.size()) {
    f.ebNames = eb.matrix->colnames;
    f.ebValues = eb.matrix->data[0];
  }
  return f;
}

double marginsNumM4(const ojson& row, const char* key) {
  return (row.contains(key) && row[key].is_number()) ? row[key].get<double>()
                                                     : std::numeric_limits<double>::quiet_NaN();
}

// k x k with se^2 on the diagonal and NaN elsewhere (see postMarginsRClassM4).
std::vector<std::vector<double>> marginsDiagVM4(const std::vector<double>& se) {
  const size_t k = se.size();
  std::vector<std::vector<double>> V(k, std::vector<double>(k, std::numeric_limits<double>::quiet_NaN()));
  for (size_t i = 0; i < k; ++i) V[i][i] = se[i] * se[i];
  return V;
}

// r() from a margins block: its coeftable (the dydx forms and the per-level
// predictive margins), else the engine's predictive `margins` array (the at()
// forms; one point is Stata's `_cons` column, several are `#._at`). `V` is the
// engine's full covariance across points when it sent one of the right size.
// `levelBn`: the coeftable rows are the levels of a marginlist factor, and r()'s
// first column carries `bn` (BUG-1240, StataBE 19.5 `margins c`: `0bn.c 1.c 2.c
// 3.c`; after `ib2.c` still `0bn.c`, the first level, not the base) while the
// table keeps the plain level.
void postMarginsFromBlockM4(SessionState& session, const ojson& block, const ojson& raw,
                            const MarginsRInfoM4& info, bool levelBn = false) {
  std::vector<std::string> names;
  std::vector<double> b, se;
  ojson rows = ojson::array();
  auto tableRow = [](const std::string& name, const ojson& src, const char* bKey) {
    ojson r;
    r["name"] = name;
    for (const auto& [from, to] : std::vector<std::pair<const char*, const char*>>{
             {bKey, "b"}, {"se", "se"}, {"t", "t"}, {"z", "z"}, {"p", "p"}, {"ci_lo", "ci_lo"},
             {"ci_hi", "ci_hi"}}) {
      if (src.contains(from)) r[to] = src[from];
    }
    return r;
  };
  if (block.contains("coeftable") && block["coeftable"].is_array()) {
    for (const auto& row : block["coeftable"]) {
      names.push_back(row.value("name", std::string()));
      const size_t dot = names.back().find('.');
      if (levelBn && names.size() == 1 && dot != std::string::npos) names.back().insert(dot, "bn");
      b.push_back(marginsNumM4(row, "b"));
      se.push_back(marginsNumM4(row, "se"));
      rows.push_back(tableRow(names.back(), row, "b"));
    }
  } else if (block.contains("margins") && block["margins"].is_array()) {
    const auto& arr = block["margins"];
    for (size_t i = 0; i < arr.size(); ++i) {
      // Stata's colfullnames: the first point is `1bn._at` (w2v_margins_siblings
      // F_range_numlist_lg_cols `1bn._at 2._at 3._at`).
      names.push_back(arr.size() == 1 ? std::string("_cons")
                                      : std::to_string(i + 1) + (i == 0 ? "bn" : "") + "._at");
      b.push_back(marginsNumM4(arr[i], "margin"));
      se.push_back(marginsNumM4(arr[i], "se"));
      rows.push_back(tableRow(names.back(), arr[i], "margin"));
    }
  }
  std::vector<std::vector<double>> V = marginsDiagVM4(se);
  if (raw.is_object() && raw.contains("V") && raw["V"].is_array() && raw["V"].size() == names.size()) {
    bool ok = true;
    std::vector<std::vector<double>> full;
    for (const auto& vr : raw["V"]) {
      if (!vr.is_array() || vr.size() != names.size()) {
        ok = false;
        break;
      }
      std::vector<double> row;
      for (const auto& x : vr) row.push_back(x.is_number() ? x.get<double>()
                                                           : std::numeric_limits<double>::quiet_NaN());
      full.push_back(std::move(row));
    }
    if (ok) V = std::move(full);
  }
  postMarginsRClassM4(session, block, info, names, b, V, rows);
}

// FU-94: how the marginlist term `var` enters the fitted model. lm.x carries the
// fit's own term spellings (`x1`, `i.t`, `ib2.t`, `i.t#c.x1`, `c.x1`).
enum class MarginsTermM4 { Absent, Continuous, Factor, Other };
MarginsTermM4 marginsTermRoleM4(const LastModel& lm, const std::string& var) {
  bool factor = false, continuous = false, other = false;
  for (const std::string& term : lm.x) {
    const bool interaction = term.find('#') != std::string::npos;
    size_t pos = 0;
    while (pos <= term.size()) {
      const size_t hash = term.find('#', pos);
      const std::string seg =
          term.substr(pos, hash == std::string::npos ? std::string::npos : hash - pos);
      const size_t dot = seg.rfind('.');
      const std::string op = dot == std::string::npos ? std::string() : seg.substr(0, dot);
      const std::string name = dot == std::string::npos ? seg : seg.substr(dot + 1);
      if (name == var) {
        if (interaction) {
          other = true;
        } else if (op.empty() || op == "c") {
          continuous = true;
        } else if (op[0] == 'i') {
          factor = true;
        } else {
          other = true;  // a level spelling (`2.t`) or a time-series operator
        }
      }
      if (hash == std::string::npos) break;
      pos = hash + 1;
    }
  }
  if (other) return MarginsTermM4::Other;
  if (factor) return MarginsTermM4::Factor;
  if (continuous) return MarginsTermM4::Continuous;
  return MarginsTermM4::Absent;
}

// FU-342: does `var` appear inside a `#` term of the fit -- in its own term
// spellings (lm.x: `c.x1##c.x1`, `i.t#c.x1`) or in its coefficient names
// (`x1#x1`, `2.t#c.x1`)? Each `#` segment is compared after its operator
// prefix (`c.`, `i.`, `2.`, `ib2.`), so `x1` never matches `x10`.
bool marginsVarInInteractionM4(const LastModel& lm, const std::string& var) {
  auto mentions = [&var](const std::string& term) {
    if (term.find('#') == std::string::npos) return false;
    size_t pos = 0;
    while (pos <= term.size()) {
      const size_t hash = term.find('#', pos);
      const std::string seg =
          term.substr(pos, hash == std::string::npos ? std::string::npos : hash - pos);
      const size_t dot = seg.rfind('.');
      if ((dot == std::string::npos ? seg : seg.substr(dot + 1)) == var) return true;
      if (hash == std::string::npos) break;
      pos = hash + 1;
    }
    return false;
  };
  for (const std::string& term : lm.x) {
    if (mentions(term)) return true;
  }
  if (lm.result.has_value() && lm.result->contains("coeftable") &&
      (*lm.result)["coeftable"].is_array()) {
    for (const auto& row : (*lm.result)["coeftable"]) {
      if (row.contains("name") && row["name"].is_string() &&
          mentions(row["name"].get<std::string>())) {
        return true;
      }
    }
  }
  return false;
}

// BUG-1502: margins reads every dydx() and at() name against the fit's
// covariates before anything is computed, and a name that is none of them (not
// in the data, or in the data but not in the model) is r(322) after regress,
// logit, probit and poisson alike (StataBE 19.5, scratch-laneA/agents/file-new-1/
// i11.do and rlife/m1s.do): "invalid dydx() option;" / "nosuchvar not found in
// list of covariates", "invalid at() option;" / "variable nosuchvar not found in
// list of covariates". The covariates are the fit's term spellings (lm.x), its
// absorbed variables and its coefficient names, each `#` segment read after its
// operator and `eq:` prefixes. A keyword (`_all`), a wildcard, an operated name
// or one that abbreviates a covariate is left to the paths below. Only the
// one-equation fits measured (the least-squares family, logit, probit, poisson)
// are checked: a multi-equation fit's other equations (heckman's selection) are
// not in its coefficient table, and a fit whose table is not in memory refuses
// nothing here.
std::optional<ojson> marginsCovariateRefusalM4(const LastModel& lm,
                                               const std::optional<std::string>& dydxVal,
                                               const std::optional<std::string>& atVal) {
  if (!postestOlsCmdsM4().count(lm.cobraCmd) && !postestGlmCmdsM4().count(lm.cobraCmd))
    return std::nullopt;
  if (!lm.result.has_value() || !lm.result->contains("coeftable") ||
      !(*lm.result)["coeftable"].is_array() || (*lm.result)["coeftable"].empty())
    return std::nullopt;
  std::vector<std::string> covariates;
  auto addTerm = [&covariates](std::string term) {
    const size_t colon = term.rfind(':');
    if (colon != std::string::npos) term = term.substr(colon + 1);
    size_t pos = 0;
    while (pos <= term.size()) {
      const size_t hash = term.find('#', pos);
      const std::string seg =
          term.substr(pos, hash == std::string::npos ? std::string::npos : hash - pos);
      const size_t dot = seg.rfind('.');
      covariates.push_back(dot == std::string::npos ? seg : seg.substr(dot + 1));
      if (hash == std::string::npos) break;
      pos = hash + 1;
    }
  };
  for (const std::string& term : lm.x) addTerm(term);
  for (const std::string& term : lm.fe) addTerm(term);
  for (const auto& row : (*lm.result)["coeftable"])
    if (row.contains("name") && row["name"].is_string()) addTerm(row["name"].get<std::string>());
  auto judgedBelow = [&covariates](const std::string& name) {
    if (name.empty() || name[0] == '_' || !strutil::isValidStataIdentifier(name)) return true;
    for (const std::string& c : covariates)
      if (c.compare(0, name.size(), name) == 0) return true;  // the covariate or its abbreviation
    return false;
  };
  if (dydxVal.has_value()) {
    for (std::string v : splitTokensM4(*dydxVal)) {
      if (v.rfind("i.", 0) == 0 || v.rfind("c.", 0) == 0) v = v.substr(2);
      if (!judgedBelow(v))
        return marginsRefusalM4("invalid dydx() option;\n" + v + " not found in list of covariates",
                                322, "engine_error");
    }
  }
  if (atVal.has_value() && !trimM4(*atVal).empty()) {
    const cecon::MarginsAtParse p = cecon::margins_parse_at(*atVal);
    if (p.rc == 0)
      for (const std::string& v : p.vars)
        if (!judgedBelow(v))
          return marginsRefusalM4(
              "invalid at() option;\nvariable " + v + " not found in list of covariates", 322,
              "engine_error");
  }
  return std::nullopt;
}

// BUG-1449: r(Jacobian) of a least-squares predictive margin, d(margin)/d(e(b)):
// the e(sample) mean of each e(b) column, a marginlist factor's columns set to
// the level of each row (StataBE 19.5 s1.do after `regress y x1 x2 i.c`:
// `margins` 5.25 2.31 0 .25 .25 .25 1; `margins c`, level 1: 5.25 2.31 0 1 0 0
// 1). A base or omitted column is 0. Each row must reproduce its margin (J b),
// or nothing is returned.
bool marginsLinearJacobianM4(SessionState& session, const MarginsFitInfoM4& fit,
                             const std::string& factorVar, const std::vector<double>& levels,
                             const std::vector<double>& margins,
                             std::vector<std::vector<double>>& J) {
  const std::vector<std::string>& eb = fit.ebNames;
  if (eb.empty() || fit.ebValues.size() != eb.size() || margins.size() != levels.size()) return false;
  struct Atom {
    const double* v = nullptr;
    bool factor = false;
    double level = 0;
    bool fixed = false;  // the marginlist factor: set per row
  };
  struct Col {
    bool constant = false, zero = false;
    std::vector<Atom> atoms;
  };
  Datastore* ds = nullptr;
  std::vector<int64_t> rows;
  try {
    const SessionState::EsampleReading er = session.esampleReading();
    if (er.kind != SessionState::EsampleReading::Kind::kColumn) return false;
    ds = session.datastore();
    if (ds == nullptr || !ds->has(kEsampleColumn) || ds->col(kEsampleColumn).f64() == nullptr) return false;
    const double* m = ds->col(kEsampleColumn).f64();
    for (int64_t i = 0; i < ds->nRows(); ++i)
      if (m[i] == static_cast<double>(er.serial)) rows.push_back(i);
  } catch (const std::exception&) {
    return false;
  }
  if (rows.empty()) return false;
  std::vector<Col> cols;
  for (const std::string& nm : eb) {
    Col c;
    if (nm.find(':') != std::string::npos) return false;
    if (nm == "_cons") {
      c.constant = true;
      cols.push_back(std::move(c));
      continue;
    }
    size_t pos = 0;
    bool mentionsFactor = false;
    while (true) {
      const size_t hash = nm.find('#', pos);
      const std::string seg = nm.substr(pos, hash == std::string::npos ? std::string::npos : hash - pos);
      const size_t dot = seg.find('.');
      const std::string op = dot == std::string::npos ? std::string() : seg.substr(0, dot);
      const std::string var = dot == std::string::npos ? seg : seg.substr(dot + 1);
      size_t d = 0;
      while (d < op.size() && std::isdigit(static_cast<unsigned char>(op[d]))) ++d;
      const std::string flags = op.substr(d);
      Atom a;
      if (d > 0) {
        if (flags.find_first_not_of("bno") != std::string::npos) return false;
        a.factor = true;
        a.level = std::stod(op.substr(0, d));
        if (flags.find('o') != std::string::npos ||
            (flags.find('b') != std::string::npos && flags.find("bn") == std::string::npos))
          c.zero = true;
      } else if (op == "o" || op == "co" || op == "oc") {
        c.zero = true;
      } else if (!(op.empty() || op == "c")) {
        return false;  // a time-series operator: not a column read here
      }
      if (var.empty() || !ds->has(var) || ds->col(var).f64() == nullptr) return false;
      a.v = ds->col(var).f64();
      a.fixed = !factorVar.empty() && var == factorVar;
      if (a.fixed) {
        if (!a.factor) return false;
        mentionsFactor = true;
      }
      c.atoms.push_back(a);
      if (hash == std::string::npos) break;
      pos = hash + 1;
    }
    // The engine's per-level path refuses a term derived from the factor.
    if (mentionsFactor && c.atoms.size() > 1) return false;
    cols.push_back(std::move(c));
  }
  J.assign(levels.size(), std::vector<double>(eb.size(), 0.0));
  for (size_t li = 0; li < levels.size(); ++li) {
    double fitted = 0.0, scale = 0.0;
    for (size_t j = 0; j < cols.size(); ++j) {
      const Col& c = cols[j];
      double mean = 0.0;
      if (c.constant) {
        mean = 1.0;
      } else if (!c.zero) {
        double s = 0.0;
        for (int64_t r : rows) {
          double v = 1.0;
          for (const Atom& a : c.atoms) {
            const double x = a.fixed ? levels[li] : a.v[r];
            v *= a.factor ? (x == a.level ? 1.0 : 0.0) : x;
          }
          s += v;
        }
        mean = s / static_cast<double>(rows.size());
      }
      J[li][j] = mean;
      fitted += mean * fit.ebValues[j];
      scale += std::fabs(mean * fit.ebValues[j]);
    }
    if (!std::isfinite(fitted) || std::fabs(fitted - margins[li]) > 1e-9 * std::max(1.0, scale)) return false;
  }
  return true;
}

// BUG-1549: margins takes e(df_r) only when every prediction is `xb`, which the
// default prediction is when the fit's predict program declares `default_xb`
// (_marg_work.class; regres_p and areg_p do). reghdfe_p and xtreg's xtrefe_p do
// not, so after `reghdfe` (absorbed or not) and `xtreg, fe` every margins form is
// normal-referenced: no r(df_r), r(table)'s df `.` and crit 1.959964 (StataBE
// 19.5, p-margins/s4.do; scratch-laneA2/agents/post/m1.do, m2.do: `margins,
// dydx(x1)` z 45.07, interval 1.891077 2.063039, where after areg t(48) and
// 1.888854 2.065262). marginsGridFitM4's reference (t after regress and areg
// only) agrees.
bool marginsNormalRefM4(const LastModel& lm) {
  return lm.stataCmd == "reghdfe" || lm.stataCmd == "xtreg";
}

// A t-referenced margins row re-referenced to the normal, runMarginsGrid's rule:
// z = b/se, p = 2*normal(-|z|), the interval b -+ 1.959964*se, `t` mirroring `z`.
// A row with no positive se keeps its missing statistics.
void marginsNormalRowM4(ojson& row) {
  const double b = marginsNumM4(row, "b"), se = marginsNumM4(row, "se");
  if (!std::isfinite(b) || !(se > 0)) return;
  const double z = b / se, crit = cecon::z_critical(0.05);
  row["z"] = z;
  row["t"] = z;
  row["p"] = std::erfc(std::fabs(z) / std::sqrt(2.0));
  row["ci_lo"] = b - crit * se;
  row["ci_hi"] = b + crit * se;
}

// The predictive margins of a least-squares fit without absorbed effects:
// `marginVar` empty = Stata's bare `margins` (one `_cons` row, the mean
// prediction over e(sample)), else one row per level of that factor. The engine's
// linear branch refits the fit's own y/x/vce (the caller has gated the family),
// and `--atmeans` holds the other design columns at their float-stored means.
ojson runLinearPredictiveMarginsM4(const LastModel& lm, SessionState& session,
                                   std::vector<std::string> args, const std::string& marginVar,
                                   bool atmeans, bool postOpt, const MarginsFitInfoM4& fitInfo) {
  if (marginVar.empty()) {
    args.push_back("--at");
    args.push_back("");
  } else {
    args.push_back("--marginlist");
    args.push_back(marginVar);
  }
  if (atmeans) args.push_back("--atmeans");
  if (!lm.vcov.empty() && lm.vcov != "iid") {
    args.push_back("--vcov");
    args.push_back(lm.vcov);
  }
  if (!lm.cluster.empty()) {
    args.push_back("--cluster");
    args.push_back(joinCommaM4(lm.cluster));
  }
  // FU-385, as the at() path: a two-way regress refits under the fit's own
  // multiway convention, not reghdfe's Gmin factor.
  for (const auto& f : fitConventionFlagsM4(lm))
    if (f == "--multiway-subset-factors" || f == "--keep-singletons" ||
        f == "--collinear-forward")
      args.push_back(f);
  args.push_back("--tidy");
  ojson raw;
  try {
    raw = runCobraJsonM4(args);
  } catch (const CobraError& ce) {
    // The engine's own refusals of these forms (a level spelling it cannot map,
    // a factor the design does not carry) are named, with an explicit rc.
    if (ce.code == "bad_input" || ce.code == "unsupported") {
      return marginsRefusalM4(cobraErrorStrM4(ce), 198);
    }
    return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
  }
  ojson ct = ojson::array();
  if (raw.contains("margins") && raw["margins"].is_array()) {
    for (const auto& m : raw["margins"]) {
      ojson row;
      row["name"] = marginVar.empty() ? std::string("_cons") : m.value("at", std::string());
      for (const auto& [from, to] : std::vector<std::pair<const char*, const char*>>{
               {"margin", "b"}, {"se", "se"}, {"t", "t"}, {"p", "p"}, {"ci_lo", "ci_lo"},
               {"ci_hi", "ci_hi"}}) {
        row[to] = m.contains(from) ? m[from] : ojson(nullptr);
      }
      if (marginsNormalRefM4(lm)) marginsNormalRowM4(row);  // BUG-1549: after reghdfe
      ct.push_back(std::move(row));
    }
  }
  ojson shaped;
  shaped["estimator"] = "margins";
  shaped["n_obs"] = raw.value("n_obs", 0);
  if (raw.contains("df")) shaped["df"] = raw["df"];
  shaped["coeftable"] = ct;
  ojson est = cobraToEstimationM4(shaped, "margins_cmd", &session);
  session.lastMargins = raw;
  session.lastMarginsDydx.clear();
  MarginsRInfoM4 info;
  info.leastSquares = !marginsNormalRefM4(lm);
  info.kMargins = marginVar.empty() ? 0 : 1;
  info.atPoints = atmeans ? 1 : 0;  // StataBE `margins, atmeans`: r(k_at) 1, r(atstats1)
  info.cluster = !lm.cluster.empty();
  info.fit = fitInfo;
  info.kAt = atmeans ? 1 : 0;
  info.title = marginsTitleM4(false, atmeans, std::string(), marginVar, fitInfo.ebNames);
  // Under atmeans StataBE names r(_N)'s column `c1` (s2.do B_am_N), and the
  // Jacobian is taken at the float means: neither is posted there.
  if (!atmeans && raw.contains("margins") && raw["margins"].is_array()) {
    info.nKnown = true;
    std::vector<double> levels, mg;
    bool ok = true;
    for (const auto& m : raw["margins"]) {
      mg.push_back(marginsNumM4(m, "margin"));
      if (marginVar.empty()) {
        levels.push_back(0.0);
        continue;
      }
      const std::string at = m.value("at", std::string());  // `<level>.<var>`
      const std::string lvText = at.substr(0, at.find('.'));
      char* end = nullptr;
      const double lv = std::strtod(lvText.c_str(), &end);
      ok = ok && !lvText.empty() && at.find('.') != std::string::npos && end != nullptr && *end == '\0';
      levels.push_back(lv);
    }
    if (ok && !marginsLinearJacobianM4(session, fitInfo, marginVar, levels, mg, info.jacobian))
      info.jacobian.clear();
  }
  postMarginsFromBlockM4(session, est, raw, info, !marginVar.empty());
  if (postOpt) postEstimationBlockM4(session, est, "margins", lm.depvar, lm.dataPath);
  return est;
}

// ---------------------------------------------------------------------------
// margins over at() points, a marginlist factor and atmeans, from the fit's own
// e(b)/e(V) (cecon::margins_grid, src/estimators/posthest.h). Oracle:
// tests/eval/golden/aer_w2/w2v_margins_corpus.log. The engine's margins refit
// sets the at() variable's own column and keeps every derived column (`z#x`,
// `x#x`) at its observed values, cannot carry the fit's weight, and has no
// response-scale path after a nonlinear fit; the grid rebuilds every column
// from the covariates at each point and takes V = G e(V) G' over the fit's own
// covariance.

// Stata's full column name -> the coefficient name `_b[]` reads: the `bn` and
// `o` flags dropped (`x:1bn._at` -> `x:1._at`, `1.tt:0bn.dcat` -> `1.tt:0.dcat`,
// `0b.tt:2o._at` -> `0b.tt:2._at`); a base flag `b` stays (`0b.treatment`,
// F5_dydx_post_bname2).
std::string marginsCoefNameM4(const std::string& full) {
  std::string out;
  size_t i = 0;
  while (i < full.size()) {
    size_t j = i;
    while (j < full.size() && std::isdigit(static_cast<unsigned char>(full[j]))) ++j;
    const bool levelStart = j > i && (i == 0 || full[i - 1] == ':' || full[i - 1] == '#');
    if (!levelStart) {
      out += full[i++];
      continue;
    }
    out.append(full, i, j - i);
    size_t k = j;
    std::string flags;
    while (k < full.size() && std::isalpha(static_cast<unsigned char>(full[k]))) flags += full[k++];
    if (k < full.size() && full[k] == '.') {
      const size_t bn = flags.find("bn");
      if (bn != std::string::npos) flags.erase(bn, 2);
      flags.erase(std::remove(flags.begin(), flags.end(), 'o'), flags.end());
      out += flags;
    } else {
      out.append(full, j, k - j);
    }
    i = k;
  }
  return out;
}

std::optional<ojson> pushGlmAtVceM4(std::vector<std::string>& args, const LastModel& lm,
                                    const std::string& form);  // defined below

// What the grid reads off a least-squares, logit/probit or poisson fit
// (MarginsGridFit). e(b)/e(V): the fit replayed once with `--emit-v`
// (lastModelBaseArgv, the refit test/lincom/nlcom already use; after poisson,
// whose engine fit has no --emit-v, `margins --model poisson --emit-v` under the
// fit's --vce/--cluster, the fit cmd_margins' GLM at() branches evaluate), whose
// coefficients and standard errors must reproduce the fitted table or nothing is
// returned. e(sample): the mask the fit installed on the working frame; a
// logit/probit/poisson fit carries none, so its rows are the fit frame's complete
// cases, which must count to e(N).
bool marginsGridFitM4(SessionState& session, const LastModel& lm, MarginsGridFit& fit,
                      std::string& why) {
  static const std::set<std::string> kEmitV = {"feols", "logit", "probit", "poisson"};
  if (!kEmitV.count(lm.cobraCmd) || !lm.result.has_value() || !lm.result->is_object()) {
    why = "the fit's covariance matrix is not available to margins after `" + lm.cobraCmd + "`";
    return false;
  }
  const ojson& res = *lm.result;
  std::map<std::string, std::pair<double, double>> fitted;  // name -> (b, se)
  if (res.contains("coeftable") && res["coeftable"].is_array())
    for (const auto& row : res["coeftable"])
      fitted[row.value("name", std::string())] = {marginsNumM4(row, "b"), marginsNumM4(row, "se")};
  const bool poisson = lm.cobraCmd == "poisson";
  if (poisson && (!lm.fitWeight.empty() || !lm.fitKeepif.empty() || !lm.fitDataPath.empty())) {
    why = std::string("the estimation sample of a ") +
          (lm.fitWeight.empty() ? "restricted" : "weighted") + " `poisson` fit is not recorded";
    return false;
  }
  nlohmann::json raw;
  try {
    std::vector<std::string> argv = lastModelBaseArgv(lm.cobraCmd, lm);
    if (poisson) {
      argv = {"margins", "--data", lm.dataPath, "--y", lm.y, "--x",
              lm.x.empty() ? "(intercept)" : joinCommaM4(lm.x), "--model", "poisson"};
      if (pushGlmAtVceM4(argv, lm, "atmeans or a marginlist").has_value()) {
        why = "the fit's VCE cannot be carried to margins after `poisson`";
        return false;
      }
    } else if (lm.cobraCmd != "feols") {
      // logit/probit spell the VCE `--vce` (buildEstimationArgvM4); lm.vcov holds
      // the linear spelling (cacheLastModelM4).
      for (size_t i = 0; i + 1 < argv.size(); ++i)
        if (argv[i] == "--vcov") {
          argv[i] = "--vce";
          if (argv[i + 1] == "hc1") argv[i + 1] = "robust";
        }
    }
    argv.push_back("--emit-v");
    argv.push_back("--tidy");
    raw = runCobraJsonM4(argv);
  } catch (const CobraError& ce) {
    why = "the fit could not be replayed for its covariance matrix (" + cobraErrorStrM4(ce) + ")";
    return false;
  }
  if (!raw.is_object() || !raw.contains("e_V") || !raw["e_V"].is_object() ||
      !raw.contains("coeftable") || !raw["coeftable"].is_array()) {
    why = "the engine's replay of the fit returned no covariance matrix";
    return false;
  }
  const auto& eV = raw["e_V"];
  std::vector<std::string> names;
  for (const auto& nm : eV.value("names", nlohmann::json::array())) names.push_back(nm.get<std::string>());
  const size_t k = names.size();
  std::map<std::string, double> bOf, seOf;
  for (const auto& row : raw["coeftable"]) {
    const std::string nm = row.value("name", std::string());
    bOf[nm] = row.contains("b") && row["b"].is_number() ? row["b"].get<double>() : 0.0;
    seOf[nm] = row.contains("se") && row["se"].is_number() ? row["se"].get<double>() : 0.0;
  }
  fit.names = names;
  fit.b.assign(k, 0.0);
  fit.V.assign(k * k, 0.0);
  const auto& mat = eV.value("matrix", nlohmann::json::array());
  if (mat.size() != k) {
    why = "the engine's covariance matrix does not match its coefficient names";
    return false;
  }
  for (size_t i = 0; i < k; ++i) {
    if (!mat[i].is_array() || mat[i].size() != k) {
      why = "the engine's covariance matrix does not match its coefficient names";
      return false;
    }
    for (size_t j = 0; j < k; ++j)
      fit.V[i * k + j] = mat[i][j].is_number() ? mat[i][j].get<double>() : 0.0;
    auto bit = bOf.find(names[i]);
    if (bit != bOf.end()) fit.b[i] = bit->second;
    // The replay must be the fit: every coefficient the fitted table shows, b and se.
    auto fi = fitted.find(names[i]);
    if (fi == fitted.end()) continue;
    const double b0 = fi->second.first, se0 = fi->second.second, se1 = std::sqrt(fit.V[i * k + i]);
    if (std::fabs(fit.b[i] - b0) > 1e-9 * std::max(1.0, std::fabs(b0)) ||
        (std::isfinite(se0) && se0 > 0 && std::fabs(se1 - se0) > 1e-8 * se0)) {
      why = "the engine's replay of the fit did not reproduce the coefficient of `" + names[i] + "`";
      return false;
    }
  }
  if (lm.cobraCmd == "logit") fit.link = static_cast<int>(cecon::GlmLinkFn::Logit);
  else if (lm.cobraCmd == "probit") fit.link = static_cast<int>(cecon::GlmLinkFn::Probit);
  else if (poisson) fit.link = static_cast<int>(cecon::GlmLinkFn::Log);
  else fit.link = static_cast<int>(cecon::GlmLinkFn::Identity);

  // The variables the columns are built from.
  std::set<std::string> vars;
  for (const auto& nm : names) {
    bool ok = true;
    for (const auto& a : cecon::margins_parse_column(nm, &ok)) vars.insert(a.var);
    if (!ok) {
      why = "the term `" + nm + "` cannot be rebuilt at an at() point";
      return false;
    }
  }
  const double eN = res.contains("n_obs") && res["n_obs"].is_number() ? res["n_obs"].get<double>() : -1;
  std::unique_ptr<Datastore> owned;
  Datastore* ds = nullptr;
  std::vector<int64_t> rows;
  bool sampleFromColumn = false;
  try {
    const SessionState::EsampleReading er = session.esampleReading();
    if (er.kind == SessionState::EsampleReading::Kind::kColumn) {
      sampleFromColumn = true;
      ds = session.datastore();
      if (ds == nullptr || !ds->has(kEsampleColumn)) {
        why = "the estimation sample is not on the data in memory";
        return false;
      }
      const double* m = ds->col(kEsampleColumn).f64();
      const double want = static_cast<double>(er.serial);
      for (int64_t i = 0; i < ds->nRows(); ++i)
        if (m != nullptr && m[i] == want) rows.push_back(i);
    } else if (lm.cobraCmd == "logit" || lm.cobraCmd == "probit" || poisson) {
      if (!lm.fitKeepif.empty() || !lm.fitDataPath.empty()) {
        why = "the estimation sample of a restricted `" + lm.cobraCmd + "` fit is not recorded";
        return false;
      }
      owned = std::make_unique<Datastore>(lm.dataPath);
      ds = owned.get();
      std::vector<std::string> need(vars.begin(), vars.end());
      need.push_back(lm.y);
      if (!lm.fitWeight.empty()) need.push_back(lm.fitWeight);
      for (const auto& c : lm.cluster) need.push_back(c);
      std::vector<const double*> cols;
      for (const auto& v : need) {
        if (!ds->has(v) || ds->col(v).dtype != cecon::DType::F64 || ds->col(v).f64() == nullptr) {
          why = "variable " + v + " is not a numeric column of the fit's data";
          return false;
        }
        cols.push_back(ds->col(v).f64());
      }
      for (int64_t i = 0; i < ds->nRows(); ++i) {
        bool ok = true;
        for (const double* c : cols) ok = ok && !std::isnan(c[i]);
        if (ok) rows.push_back(i);
      }
    } else {
      why = "the estimation sample of this fit is not recorded (" +
            (er.refusal.empty() ? std::string("no e(sample) marker") : er.refusal) + ")";
      return false;
    }
  } catch (const std::exception& e) {
    why = std::string("the estimation sample cannot be read: ") + e.what();
    return false;
  }
  fit.n = static_cast<int64_t>(rows.size());
  auto column = [&](const std::string& v, std::vector<double>& out) {
    if (!ds->has(v) || ds->col(v).dtype != cecon::DType::F64 || ds->col(v).f64() == nullptr) return false;
    const double* c = ds->col(v).f64();
    out.resize(rows.size());
    for (size_t r = 0; r < rows.size(); ++r) out[r] = c[rows[r]];
    return true;
  };
  for (const auto& v : vars) {
    if (!column(v, fit.data[v])) {
      why = "variable " + v + " is not a numeric column of the data in memory";
      return false;
    }
  }
  if (!lm.fitWeight.empty() && !column(lm.fitWeight, fit.weights)) {
    why = "the fit's weight " + lm.fitWeight + " is not a numeric column of the data in memory";
    return false;
  }
  fit.fweight = lm.fitWtype == "fw" || lm.fitWtype == "fweight";
  double count = static_cast<double>(fit.n);
  if (fit.fweight) {
    count = 0;
    for (double w : fit.weights) count += w;
  }
  // The live e(sample) column follows the rows: rows dropped after the fit are
  // gone from it, and Stata's margins averages over what is left (w2v_margins_
  // siblings.log:3360 F_dropped_at `N=200` after `drop if t==1` on a 240-row
  // fit; r(table)'s df stays the fit's e(df_r) 236). A reconstructed sample
  // must still be the fit's exactly.
  if (eN >= 0 && (sampleFromColumn ? count > eN : count != eN)) {
    why = "the estimation sample margins would average over (" + std::to_string(count) +
          ") is not the fit's e(N)";
    return false;
  }
  // r(table)'s reference (w2v_margins_corpus r(table) df rows): t(e(df_r)) after
  // regress and areg (F1r 196, F3a 39, F4 236, F5 231), the normal after xtreg,
  // reghdfe and logit (F1 p 1.369e-36 is the normal tail of 12.634).
  fit.tRef = lm.stataCmd == "regress" || lm.stataCmd == "reg" || lm.stataCmd == "areg";
  if (fit.tRef) {
    fit.dfR = res.contains("df_r") && res["df_r"].is_number() ? res["df_r"].get<double>() : 0.0;
    if (!(fit.dfR > 0)) {
      why = "the fit's residual degrees of freedom are not recorded";
      return false;
    }
  }
  fit.cluster = !lm.cluster.empty();
  fit.depvar = lm.depvar;
  fit.dataPath = lm.dataPath;
  return true;
}

// An at() Stata itself rejects (margins_parse_at rc 121, `at(d=(min(.2)max))`;
// rc 198 for the forms the parser names): StataBE stops before computing and
// leaves r() empty -- w2v_margins_corpus F7_dydx_minmax reads r(N) as `.` and
// colsof(r(b)) as a type mismatch -- so r() is replaced by the known-empty set
// rather than refused.
ojson marginsAtRefusalM4(SessionState& session, const cecon::MarginsAtParse& p) {
  if (p.rc == 121) {
    eval::replaceRClass(session.eval);
    eval::declareRPosted(session.eval, "margins", {});
    return marginsRefusalM4(p.error, 121, "engine_error");
  }
  return marginsRefusalM4(p.error, p.rc);
}

// BUG-1497: the name errors Stata's margins raises too leave r() empty as well:
// r(N) `.` and no r() scalar after `margins, dydx(nosuchvar)` and `margins,
// at(nosuchvar=1)` r(322), `margins nosuchvar` and `margins, dydx(x1)
// over(nosuchvar)` r(111) (StataBE 19.5, rlife/side.do; file-new-1/i7.do).
ojson marginsStataErrorM4(SessionState& session, ojson refusal) {
  eval::replaceRClass(session.eval);
  eval::declareRPosted(session.eval, "margins", {});
  return refusal;
}

// A fit whose covariance is not the model's OIM one: vce(robust), vce(cluster)
// or a cluster variable (lm.vcov carries the linear spelling, `hc1`).
bool marginsFitNonOimM4(const LastModel& lm) {
  return (!lm.vcov.empty() && lm.vcov != "iid" && lm.vcov != "oim") || !lm.cluster.empty();
}

// The fit's VCE for cmd_margins' GLM at() branches (cmds_postest.cpp
// ml_margins_fit, the fit the AME branch already uses): `--vce robust|cluster`
// and `--cluster C`. Those branches used to fit OIM whatever the fit's vce, so
// after `poisson yc x c1, vce(robust)` `margins, dydx(x) at(c1=(1 3))` answered
// the OIM se at rc 0 (Stata: the robust one, w2v_margins_siblings.log:2871-2921
// F_robust_dydx_at_po). After xtlogit/xtpois/fepois they refit the pooled model,
// whose sandwich is not the fit's, and probit takes no cluster sandwich there:
// such a fit is refused by name.
std::optional<ojson> pushGlmAtVceM4(std::vector<std::string>& args, const LastModel& lm,
                                    const std::string& form) {
  if (!marginsFitNonOimM4(lm)) return std::nullopt;
  std::string vce;
  if (lm.vcov == "hc1" || lm.vcov == "robust") vce = "robust";
  if (lm.vcov == "cluster" || !lm.cluster.empty()) vce = "cluster";
  const bool served = (lm.cobraCmd == "logit" || lm.cobraCmd == "poisson" ||
                       lm.cobraCmd == "cloglog" || (lm.cobraCmd == "probit" && vce == "robust")) &&
                      !vce.empty() && (vce == "robust" || lm.cluster.size() == 1);
  if (!served)
    return marginsRefusalM4("margins, " + form + " after `" + lm.cobraCmd + "` with vce(" +
                                (lm.vcov.empty() ? std::string("cluster") : lm.vcov) +
                                ") is not implemented in this version of PIET (the engine's "
                                "at() refit cannot carry that VCE); nothing was computed.",
                            198);
  args.push_back("--vce");
  args.push_back(vce);
  if (vce == "cluster") {
    args.push_back("--cluster");
    args.push_back(lm.cluster[0]);
  }
  return std::nullopt;
}

// Which margins forms runMargins answers through the grid: the ones the engine
// refit cannot answer as the fit (or answers with derived columns at observed
// values or means). Every other form keeps its landed path byte-for-byte
// (w2b_estim margins_atmeans_post/margins_overall_atmeans/margins_dydx_atmeans,
// w2p_estim_ev G_c_margins_at/G_pc_margins_dydx).
bool marginsGridWantedM4(const LastModel& lm, const std::vector<std::string>& marginlist,
                         const std::optional<std::string>& atVal,
                         const std::optional<std::string>& dydxVal, bool atmeans) {
  const bool linear = postestOlsCmdsM4().count(lm.cobraCmd) > 0;
  const bool binary = lm.cobraCmd == "logit" || lm.cobraCmd == "probit";
  const bool hasAt = atVal.has_value() && !trimM4(*atVal).empty();
  const bool hasDydx = dydxVal.has_value() && !trimM4(*dydxVal).empty();
  // After poisson (no absorbed effects: cobraCmd "fepois" is the absorb() fit),
  // the two forms no engine path computes: `margins, atmeans` and a one-factor
  // marginlist `margins g`, on the response scale exp(xb) with the fit's own
  // OIM, robust or cluster e(V) (w2v_margins_vce.log:1854-1958 V5_po_atmeans /
  // V5_po_g after vce(robust), :2205-2309 V6 after vce(cluster region)). Every
  // other poisson form keeps its landed path: dydx() (the engine AME), at() and
  // dydx()+at() (cmd_margins' GLM at() branches, V5_po_at / V5_po_dydx_at), and
  // the combinations no record pins (atmeans with at()/dydx() or a marginlist)
  // stay refused by name there.
  // A marginlist term that is no plain factor of the model keeps the landed
  // checks (r(111) / r(452) / r(322), FU-94) and their refusals.
  if (lm.cobraCmd == "poisson") {
    if (hasAt || hasDydx) return false;
    if (marginlist.empty()) return atmeans;
    const std::string& term = marginlist[0];
    return !atmeans && marginlist.size() == 1 &&
           marginsTermRoleM4(lm, term.rfind("i.", 0) == 0 ? term.substr(2) : term) ==
               MarginsTermM4::Factor;
  }
  if (!linear && !binary) return false;
  // A time-series operator column (`regress y L.x d`) is not a design column the
  // grid can rebuild from an at() point, and the grid's --emit-v replay carries
  // no tsset context. Such fits keep the landed path, which injects it (BUG-369)
  // and refuses an interacted at() variable by name (FU-342), as on main.
  {
    std::vector<std::string> tsTokens = lm.x;
    tsTokens.insert(tsTokens.end(), lm.fe.begin(), lm.fe.end());
    tsTokens.push_back(lm.y);
    if (translate::hasTsToken(tsTokens)) return false;
  }
  bool interacted = false;
  for (const std::string& term : lm.x) interacted = interacted || term.find('#') != std::string::npos;
  std::vector<std::string> atVars;
  size_t atPoints = 0;
  if (hasAt) {
    const cecon::MarginsAtParse p = cecon::margins_parse_at(*atVal);
    if (p.rc != 0) return true;  // the grid names the refusal with Stata's rc
    atVars = p.vars;
    atPoints = p.points.size();
  }
  bool atInteracted = false;
  for (const auto& v : atVars) atInteracted = atInteracted || marginsVarInInteractionM4(lm, v);
  bool dydxInteracted = false;
  if (hasDydx)
    for (std::string v : splitTokensM4(*dydxVal)) {
      if (v.rfind("i.", 0) == 0 || v.rfind("c.", 0) == 0) v = v.substr(2);
      dydxInteracted = dydxInteracted || marginsVarInInteractionM4(lm, v);
    }
  // A marginlist after logit/probit (F6 `margins year[, atmeans post]`); after a
  // least-squares fit the landed per-level path answers it.
  if (!marginlist.empty()) return binary && !hasAt;
  // After an fweighted logit/probit (F6's `[fweight=w]`) the landed engine path
  // cannot carry the weight (cmd_margins takes --wtype pw only, which surfaced
  // as rc 1), so every dydx()/at()/atmeans form goes to the grid, whose fw rows
  // equal the expanded-rows fit; the forms it cannot serve are refused by name.
  const bool fwBinary = binary && (lm.fitWtype == "fw" || lm.fitWtype == "fweight");
  if (fwBinary && (hasAt || hasDydx || atmeans)) return true;
  // at(): after a least-squares fit everything but one point on an unweighted,
  // unabsorbed fit whose at() variables enter no interaction (F1-F4, X1).
  // After a robust or cluster logit/probit, at() goes to the grid, whose V is the
  // fit's own (the --emit-v replay under the fit's --vce): the landed dydx()+at()
  // branch answered the OIM se at rc 0 (w2v_margins_siblings.log:2787-2837
  // F_robust_dydx_at_lg, `Model VCE: Robust`, se .0098585997756714). A weighted
  // fit keeps the landed path's named refusal (postestRefitMatchesFitM4).
  // A restricted robust/cluster fit (`logit ... if t>2, vce(robust)`) keeps the
  // landed path too: the grid's sample reader cannot read its estimation sample,
  // and that path refits on the fit's `if` under the fit's VCE (pushGlmAtVceM4;
  // w2v_margins_vce.log V9_lg_at / V9_lg_dydx_at).
  if (hasAt) {
    const bool restrictedFit = !lm.fitKeepif.empty() || !lm.fitDataPath.empty();
    if (binary)
      return atmeans || atInteracted || dydxInteracted ||
             (marginsFitNonOimM4(lm) && lm.fitWeight.empty() && !restrictedFit);
    return !(lm.fe.empty() && lm.fitWeight.empty() && atPoints == 1 && !atInteracted &&
             !(atmeans && interacted) && !hasDydx);
  }
  // dydx() of a variable inside an interaction (F5 `dydx(treatment)` after
  // treatment##i.blockcohort); atmeans after an interaction (X1_atmeans). After
  // logit/probit, dydx() without at() keeps the landed engine AME: it carries the
  // fit's `if` (BUG-163) and its perfect-prediction drops, and it is the path
  // W:P5 pins against Stata (p5_target_stata.log:177/:180, `margins, dydx(x1
  // treated)` after a restricted pweighted logit), which the grid's sample reader
  // would refuse.
  // After a cluster probit the landed AME cannot carry the VCE (cmd_margins has
  // no probit cluster sandwich and exited 2, which surfaced as rc 1); the grid
  // reads the fit's own clustered e(V), or refuses a fit it cannot read by name.
  if (hasDydx)
    return (!binary && dydxInteracted) ||
           (lm.cobraCmd == "probit" && (lm.vcov == "cluster" || !lm.cluster.empty()));
  return atmeans && interacted;
}

// margins' at() record ([R] margins, Stored results): r(at)/e(at) has one row
// per at() point (`1._at` ...) and one column per covariate of the model, the
// fixed value where the point sets the covariate and missing where it is left
// as observed; r(atstats#)/e(atstats#) names each column's statistic in that
// row ("values" or "asobserved"); e(k_at) counts the at() options. coefplot's at()
// reads exactly these (coefplot.ado collect_coefs: the `values`
// columns of e(atstats#), row # of e(at)); w2an_coefplot_family.log cf_at_mg
// (`margins, dydx(x1) at(x2=(0 2 4)) post` -> at 0 2 4). Posted only for a
// model whose covariates are all continuous: a factor covariate's at() column
// is one per level (`1.g 2.g ...`), which is not built here, so then nothing is
// posted and every reader keeps its refusal.
void postMarginsAtM4(SessionState& session, const std::vector<std::string>& modelNames,
                     const std::vector<cecon::MarginsGridPoint>& points, bool post) {
  if (points.empty()) return;
  std::vector<std::string> cov;
  for (const auto& nm : modelNames) {
    bool ok = true;
    const std::vector<cecon::MarginsAtom> atoms = cecon::margins_parse_column(nm, &ok);
    if (!ok) return;
    for (const auto& a : atoms) {
      if (a.factor) return;
      if (std::find(cov.begin(), cov.end(), a.var) == cov.end()) cov.push_back(a.var);
    }
  }
  if (cov.empty()) return;
  for (const auto& p : points)
    for (const auto& [v, val] : p.fix) {
      (void)val;
      if (std::find(cov.begin(), cov.end(), v) == cov.end()) return;
    }
  eval::EvalMatrix at;
  at.colnames = cov;
  std::vector<std::string> stats;
  for (size_t i = 0; i < points.size(); ++i) {
    at.rownames.push_back(std::to_string(i + 1) + "._at");
    std::vector<double> row;
    std::string st;
    for (const auto& v : cov) {
      double val = std::numeric_limits<double>::quiet_NaN();
      bool fixed = false;
      for (const auto& [fv, fval] : points[i].fix)
        if (fv == v) {
          val = fval;
          fixed = true;
        }
      row.push_back(val);
      st += (st.empty() ? "" : " ") + std::string(fixed ? "values" : "asobserved");
    }
    at.data.push_back(std::move(row));
    stats.push_back(st);
  }
  auto& ev = session.eval;
  // k_at counts at() OPTIONS, not points (margins.sthlp; StataBE 19.5 s1.do
  // `margins, at(x1=(2 4))`: r(k_at) 1 with r(at) 2 x 6), and this handler
  // takes one at() (BUG-1449).
  ev.rValues.set("k_at", 1.0);
  ev.rMatrices.emplace_back("at", at);
  for (size_t i = 0; i < stats.size(); ++i) ev.rStrings.set("atstats" + std::to_string(i + 1), stats[i]);
  if (!post) return;
  ev.setEScalar("k_at", 1.0);
  bool replaced = false;
  for (auto& p : ev.eMatrices)
    if (p.first == "at") {
      p.second = at;
      replaced = true;
    }
  if (!replaced) ev.eMatrices.emplace_back("at", at);
  for (size_t i = 0; i < stats.size(); ++i) ev.setEString("atstats" + std::to_string(i + 1), stats[i]);
}

}  // namespace

std::optional<ojson> runMarginsGrid(SessionState& session, const MarginsGridFit& fit,
                                    const std::vector<std::string>& marginlist,
                                    const std::vector<std::pair<std::string, eval::OptValue>>& opts) {
  std::string atText, dydxText;
  bool hasAt = false, atmeans = false, post = false;
  for (const auto& [k, v] : opts) {
    if (k == "at" && !v.isBool) {
      atText = v.strValue;
      hasAt = !trimM4(atText).empty();
    }
    if (k == "dydx" && !v.isBool) dydxText = v.strValue;
    if (k == "atmeans") {
      if (!v.isBool) return marginsRefusalM4("option atmeans() not allowed", 198, "syntax_error");
      atmeans = v.boolValue;
    }
    if (k == "post" && (!v.isBool || v.boolValue)) post = true;
  }
  if (marginlist.size() > 1) return std::nullopt;
  std::string over;
  if (!marginlist.empty()) {
    over = marginlist[0].rfind("i.", 0) == 0 ? marginlist[0].substr(2) : marginlist[0];
    if (over.find_first_of("#()") != std::string::npos || hasAt) return std::nullopt;
  }
  std::vector<std::string> dydx;
  bool dydxAll = false;
  for (std::string v : splitTokensM4(dydxText)) {
    if (v == "*" || v == "_all") {
      dydxAll = true;
      continue;
    }
    if (v.rfind("i.", 0) == 0 || v.rfind("c.", 0) == 0) v = v.substr(2);
    if (std::find(dydx.begin(), dydx.end(), v) == dydx.end()) dydx.push_back(v);
  }
  // dydx(*) / dydx(_all): every covariate of the model, each once, in e(b)
  // order (w2v_margins_siblings.log:1907 F_fw_all_cols `x c1 1b.g 2.g 3.g`
  // after `logit remote x c1 i.g`); a factor contributes its level columns,
  // the base included, exactly as dydx(g) does.
  if (dydxAll) {
    std::vector<std::string> all;
    for (const auto& nm : fit.names) {
      bool ok = true;
      for (const auto& a : cecon::margins_parse_column(nm, &ok))
        if (!a.var.empty() && std::find(all.begin(), all.end(), a.var) == all.end())
          all.push_back(a.var);
      if (!ok) return std::nullopt;
    }
    for (const auto& v : dydx)
      if (std::find(all.begin(), all.end(), v) == all.end()) all.push_back(v);
    dydx = std::move(all);
  }

  cecon::MarginsGridInput in;
  in.names = fit.names;
  in.beta = fit.b;
  in.V = fit.V;
  in.link = static_cast<cecon::GlmLinkFn>(fit.link);
  in.n = fit.n;
  in.data = fit.data;
  in.weights = fit.weights;
  in.atmeans = atmeans;
  in.dydx = dydx;
  std::vector<double> overLevels;
  size_t atPoints = 0;
  if (!over.empty()) {
    // The marginlist term is a factor of the model (FU-94's r(322) otherwise);
    // its levels are its e(sample) values (F6: 2010bn.year .. 2013.year).
    bool factor = false;
    for (const auto& nm : fit.names) {
      bool ok = true;
      for (const auto& a : cecon::margins_parse_column(nm, &ok)) factor = factor || (a.factor && a.var == over);
    }
    auto it = fit.data.find(over);
    if (!factor || it == fit.data.end())
      return marginsRefusalM4("factor " + over + " not found in list of covariates", 322, "engine_error");
    std::set<double> lv(it->second.begin(), it->second.end());
    for (double l : lv) {
      overLevels.push_back(l);
      in.points.push_back(cecon::MarginsGridPoint{{{over, l}}});
    }
  } else if (hasAt) {
    const cecon::MarginsAtParse p = cecon::margins_parse_at(atText);
    if (p.rc != 0) return marginsAtRefusalM4(session, p);
    in.points = p.points;
    atPoints = p.points.size();
  } else {
    in.points = {cecon::MarginsGridPoint{}};
  }
  const cecon::MarginsGridResult r = cecon::margins_grid(in);
  if (!r.ok) {
    if (!r.notFound.empty())
      return marginsRefusalM4("variable " + r.notFound + " not found in list of covariates", 322,
                              "engine_error");
    return marginsRefusalM4(r.error + "; nothing was computed.", 198);
  }
  const bool single = over.empty() && in.points.size() == 1;
  const std::vector<std::string> names = cecon::margins_grid_colnames(r, single, over, overLevels);
  const size_t m = r.b.size();

  double nObs = static_cast<double>(fit.n);
  if (fit.fweight) {
    nObs = 0;
    for (double w : fit.weights) nObs += w;
  }
  const double crit = fit.tRef ? cecon::t_critical(0.05, fit.dfR) : cecon::z_critical(0.05);
  ojson tableRows = ojson::array(), ct = ojson::array(), coefs = ojson::array();
  std::vector<std::vector<double>> V(m, std::vector<double>(m, 0.0));
  for (size_t a = 0; a < m; ++a)
    for (size_t c = 0; c < m; ++c) V[a][c] = r.V[a * m + c];
  for (size_t j = 0; j < m; ++j) {
    ojson row;
    row["name"] = names[j];
    row["b"] = r.b[j];
    const double se = std::sqrt(std::max(0.0, r.V[j * m + j]));
    if (r.cols[j].base || !(se > 0)) {
      // A base level's column (F5_dydx 0b.treatment): 0 with variance 0, and
      // r(table) prints `.` for its se, statistic, p and interval.
      row["se"] = nullptr;
      row[fit.tRef ? "t" : "z"] = nullptr;
      row["p"] = nullptr;
      row["ci_lo"] = nullptr;
      row["ci_hi"] = nullptr;
    } else {
      const double t = r.b[j] / se;
      row["se"] = se;
      row[fit.tRef ? "t" : "z"] = t;
      row["p"] = fit.tRef ? 2.0 * cecon::stata_ttail(fit.dfR, std::fabs(t))
                          : std::erfc(std::fabs(t) / std::sqrt(2.0));
      row["ci_lo"] = r.b[j] - crit * se;
      row["ci_hi"] = r.b[j] + crit * se;
    }
    tableRows.push_back(row);
    ojson shown = row;
    shown["name"] = marginsCoefNameM4(names[j]);
    if (!fit.tRef) shown["t"] = row["z"];
    // _se[] of a base column is 0 (F5_dydx_post_bname2 `0b.treatment|0|0`); only
    // r(table) prints `.` for it.
    if (row["se"].is_null()) shown["se"] = 0.0;
    ct.push_back(shown);
    coefs.push_back(ojson{{"name", shown["name"]}, {"margin", r.b[j]}, {"se", row["se"]}});
  }
  ojson est;
  est["type"] = "estimation";
  est["estimator"] = "margins";
  est["depvar"] = fit.depvar;
  est["n_obs"] = nObs;
  est["coeftable"] = ct;
  // postMarginsRClassM4 reads e(df_r) off the live model, which is this block
  // once `post` has replaced e().
  if (fit.tRef) est["df_r"] = fit.dfR;
  session.lastMargins = ojson{{"model", "margins"}, {"n_obs", nObs}, {"coefs", coefs}};
  session.lastMarginsDydx = dydx;

  MarginsRInfoM4 info;
  info.leastSquares = fit.tRef;
  info.dydx = !dydx.empty();
  for (const auto& v : dydx) info.xvars += (info.xvars.empty() ? "" : " ") + v;
  info.kMargins = over.empty() ? 0 : 1;
  info.atPoints = hasAt ? static_cast<int>(atPoints) : (atmeans ? 1 : 0);
  info.cluster = fit.cluster;
  // BUG-1449: e() is still the fit's here (`post` is below).
  info.fit = marginsFitInfoM4(session);
  info.kAt = hasAt || atmeans ? 1 : 0;
  info.title = marginsTitleM4(info.dydx, atmeans, atText, over, info.fit.ebNames);
  info.nKnown = !atmeans;
  if (info.dydx) {
    bool factor = false;
    for (const auto& c : r.cols) factor = factor || c.factor;
    info.continuous = factor ? 0 : 1;
    // r(xvars) is the effects' columns in e(b) order, a factor's base level
    // included (StataBE 19.5 s1.do `dydx(x1 c)`: "x1 0b.c 1.c 2.c 3.c"; s4.do
    // `dydx(x2 x1)`: "x1 x2"), each once over the at() points.
    info.xvars.clear();
    for (const auto& nm : names) {
      const std::string head = nm.substr(0, nm.find(':'));
      if ((" " + info.xvars + " ").find(" " + head + " ") == std::string::npos)
        info.xvars += (info.xvars.empty() ? "" : " ") + head;
    }
    if (factor) info.bRow = "y1";  // r(b)'s row after a factor dydx() (s2.do B_dxc_br)
  }
  if (post) {
    // `post` replaces e() with the margins (w2v_margins_corpus *_post_e: e(cmd)
    // margins, e(N) = r(N)); e(b)/e(V) carry Stata's column names.
    postEstimationBlockM4(session, est, "margins", fit.depvar, fit.dataPath);
    eval::EvalMatrix eb, eV;
    eb.rownames = {"y1"};
    eb.colnames = names;
    eb.data.push_back(r.b);
    eV.rownames = names;
    eV.colnames = names;
    eV.data = V;
    auto setE = [&session](const std::string& nm, eval::EvalMatrix mat) {
      for (auto& p : session.eval.eMatrices) {
        if (p.first == nm) {
          p.second = std::move(mat);
          return;
        }
      }
      session.eval.eMatrices.emplace_back(nm, std::move(mat));
    };
    setE("b", std::move(eb));
    setE("V", std::move(eV));
  }
  // r() after e(): `post` installs a new estimation, and the r(table) posted
  // under the fit would be retired with it (F1_dydx_at_post_rt reads r(table)
  // after `post`, StataBE keeps it).
  postMarginsRClassM4(session, est, info, names, r.b, V, tableRows);
  if (hasAt && !atmeans) postMarginsAtM4(session, fit.names, in.points, post);
  return est;
}

void postGlmDydxRClass(SessionState& session, const ojson& block, const std::vector<std::string>& names,
                       const std::vector<double>& b, const std::vector<std::vector<double>>& V,
                       const ojson& tableRows, const std::string& xvars, bool cluster) {
  MarginsRInfoM4 info;
  info.dydx = true;
  info.xvars = xvars;
  info.cluster = cluster;
  postMarginsRClassM4(session, block, info, names, b, V, tableRows);
}

// ME06: the grid fit of an `xtpoisson, fe` model -- e(b)/e(V) from the fit's
// reply (--emit-v), the linear index with no constant (identity link), and the
// estimation sample rebuilt as the fit drew it: rows with y, the panel id and
// every covariate present, less the panels xtpoisson drops (all-zero outcome,
// or a single row). The count must be the fit's n_obs; otherwise nothing.
// BUG-1235: `xtlogit, fe` the same way, its default prediction pu0 = Pr(y = 1 |
// fixed effect 0) = invlogit(xb) (logit link, no constant), its sample less the
// panels clogit drops (an outcome that does not vary); StataBE 19.5
// (scratch-laneA/agents/N2/e14.do) `margins, dydx(x)` .090159189631237813.
bool xtpoisMarginsFitM4(const LastModel& lm, MarginsGridFit& fit, std::string& why) {
  const bool xtlogit = lm.cobraCmd == "xtlogit";
  if (!lm.result.has_value() || !lm.result->is_object() || !lm.result->contains("e_V") ||
      !lm.ivar.has_value()) {
    why = "the fit's covariance matrix is not recorded";
    return false;
  }
  if (!lm.fitKeepif.empty() || !lm.fitDataPath.empty() || !lm.fitWeight.empty()) {
    why = "the estimation sample of a restricted fit is not recorded";
    return false;
  }
  const ojson& raw = *lm.result;
  const ojson& ev = raw["e_V"];
  if (!ev.contains("names") || !ev.contains("matrix") || !raw.contains("coefs") ||
      !raw["coefs"].is_array() || !raw.contains("n_obs")) {
    why = "the fit's covariance matrix is not recorded";
    return false;
  }
  for (const auto& nm : ev["names"]) {
    if (!nm.is_string()) return false;
    const std::string n = nm.get<std::string>();
    if (n.find_first_of(".#") != std::string::npos) {
      why = "a factor or operator term after xtpoisson, fe";
      return false;
    }
    fit.names.push_back(n);
  }
  const size_t k = fit.names.size();
  for (const auto& nm : fit.names) {
    double b = std::numeric_limits<double>::quiet_NaN();
    for (const auto& c : raw["coefs"])
      if (c.value("name", std::string()) == nm && c.contains("beta") && c["beta"].is_number())
        b = c["beta"].get<double>();
    if (std::isnan(b)) return false;
    fit.b.push_back(b);
  }
  if (ev["matrix"].size() != k) return false;
  for (const auto& row : ev["matrix"]) {
    if (!row.is_array() || row.size() != k) return false;
    for (const auto& v : row) fit.V.push_back(v.is_number() ? v.get<double>() : 0.0);
  }
  try {
    Datastore ds(lm.dataPath);
    std::vector<std::string> need = fit.names;
    need.push_back(lm.y);
    need.push_back(*lm.ivar);
    for (const auto& v : need)
      if (!ds.has(v) || ds.col(v).dtype != cecon::DType::F64 || ds.col(v).f64() == nullptr) {
        why = "variable " + v + " is not a numeric column of the fit's data";
        return false;
      }
    const double* yv = ds.col(lm.y).f64();
    const double* iv = ds.col(*lm.ivar).f64();
    std::vector<int64_t> rows;
    for (int64_t i = 0; i < ds.nRows(); ++i) {
      bool ok = !std::isnan(yv[i]) && !std::isnan(iv[i]);
      for (const auto& v : fit.names) ok = ok && !std::isnan(ds.col(v).f64()[i]);
      if (ok) rows.push_back(i);
    }
    std::map<double, std::pair<int64_t, double>> grp;  // panel -> (rows, sum y; xtlogit: rows with y != 0)
    for (int64_t i : rows) {
      auto& g = grp[iv[i]];
      ++g.first;
      g.second += xtlogit ? (yv[i] != 0.0 ? 1.0 : 0.0) : yv[i];
    }
    std::vector<int64_t> used;
    for (int64_t i : rows) {
      const auto& g = grp[iv[i]];
      if (xtlogit ? (g.second > 0 && g.second < static_cast<double>(g.first)) : (g.first > 1 && g.second > 0))
        used.push_back(i);
    }
    if (static_cast<double>(used.size()) != raw["n_obs"].get<double>()) {
      why = "the estimation sample margins would average over is not the fit's e(N)";
      return false;
    }
    fit.n = static_cast<int64_t>(used.size());
    for (const auto& v : fit.names) {
      const double* c = ds.col(v).f64();
      auto& out = fit.data[v];
      for (int64_t i : used) out.push_back(c[i]);
    }
  } catch (const std::exception& e) {
    why = std::string("the estimation sample cannot be read: ") + e.what();
    return false;
  }
  fit.link = static_cast<int>(xtlogit ? cecon::GlmLinkFn::Logit : cecon::GlmLinkFn::Identity);
  fit.tRef = false;
  fit.cluster = lm.vcov == "hc1";
  fit.depvar = lm.depvar;
  fit.dataPath = lm.dataPath;
  return true;
}

namespace {

// BUG-1237/BUG-1241: r() of a dydx() a least-squares fit answers from its own
// coefficients (buildLinearAmeRawM4). Stata's r(b) holds every e(b) column of
// each dydx() variable in e(b) order -- a factor's base level included, `0b.c`
// with b 0 -- r(V) is e(V) over those columns, off-diagonals included, and
// r(Jacobian) the unit row of each column (zero for the base): StataBE 19.5
// (scratch-laneA/agents/E3/rx2.do, p-margins/s1.do) after `regress y x1 x2 i.c`,
// dydx(x1 c) r(b) `x1 0b.c 1.c 2.c 3.c` with V[1.c,x1] -.00102863 =
// e(V)[1.c,x1]; dydx(x1 x2) el(r(V),1,2) -.0007857769903082221 = e(V)[x2,x1].
// false (nothing built) for a column this does not read: an omitted or
// interacted term of a dydx() variable, an equation-prefixed stripe.
struct LinearAmeRM4 {
  std::vector<std::string> names;
  std::vector<double> b;
  std::vector<std::vector<double>> V, J;
  ojson tableRows = ojson::array();
  bool factor = false;
};
bool linearAmeRM4(SessionState& session, const MarginsFitInfoM4& fit,
                  const std::vector<std::string>& dydxVars, const ojson& est, LinearAmeRM4& out) {
  const std::vector<std::string>& eb = fit.ebNames;
  if (eb.empty() || fit.ebValues.size() != eb.size() || !est.contains("coeftable") ||
      !est["coeftable"].is_array())
    return false;
  const eval::EvalSession::EMatrixAnswer va =
      session.eval.resolveEMatrixFor("V", eval::EvalSession::EReader::kCopy);
  if (!va.matrix.has_value() || va.matrix->colnames != eb || va.matrix->data.size() != eb.size())
    return false;
  const auto& eV = va.matrix->data;
  std::vector<size_t> sel;
  std::vector<bool> base;
  std::set<std::string> matched;
  for (size_t j = 0; j < eb.size(); ++j) {
    const std::string& nm = eb[j];
    if (nm.find(':') != std::string::npos) return false;
    if (nm == "_cons") continue;
    if (nm.find('#') != std::string::npos) {
      bool ok = true;
      for (const auto& a : cecon::margins_parse_column(nm, &ok))
        if (dydxVars.empty() ||
            std::find(dydxVars.begin(), dydxVars.end(), a.var) != dydxVars.end())
          return false;
      continue;
    }
    const size_t dot = nm.find('.');
    const std::string op = dot == std::string::npos ? std::string() : nm.substr(0, dot);
    const std::string var = dot == std::string::npos ? nm : nm.substr(dot + 1);
    if (!dydxVars.empty() && std::find(dydxVars.begin(), dydxVars.end(), var) == dydxVars.end()) continue;
    size_t d = 0;
    while (d < op.size() && std::isdigit(static_cast<unsigned char>(op[d]))) ++d;
    const std::string flags = op.substr(d);
    if (op.find('o') != std::string::npos || (d == 0 && !(op.empty() || op == "c"))) return false;
    sel.push_back(j);
    base.push_back(d > 0 && flags.find('b') != std::string::npos && flags.find("bn") == std::string::npos);
    out.factor = out.factor || d > 0;
    matched.insert(var);
  }
  if (sel.empty()) return false;
  for (const auto& v : dydxVars)
    if (!matched.count(v)) return false;
  std::map<std::string, const ojson*> shown;
  for (const auto& row : est["coeftable"]) shown[row.value("name", std::string())] = &row;
  for (size_t a = 0; a < sel.size(); ++a) {
    const std::string& nm = eb[sel[a]];
    out.names.push_back(nm);
    out.b.push_back(base[a] ? 0.0 : fit.ebValues[sel[a]]);
    std::vector<double> vr, jr(eb.size(), 0.0);
    for (size_t c = 0; c < sel.size(); ++c) vr.push_back(eV[sel[a]][sel[c]]);
    out.V.push_back(std::move(vr));
    if (!base[a]) jr[sel[a]] = 1.0;
    out.J.push_back(std::move(jr));
    ojson tr;
    tr["name"] = nm;
    if (base[a]) {
      // r(table) prints `.` for a base level's se, statistic, p and interval.
      tr["b"] = 0.0;
      for (const char* key : {"se", "t", "p", "ci_lo", "ci_hi"}) tr[key] = nullptr;
    } else {
      const auto it = shown.find(nm);
      if (it == shown.end()) return false;
      for (const char* key : {"b", "se", "t", "z", "p", "ci_lo", "ci_hi"})
        if (it->second->contains(key)) tr[key] = (*it->second)[key];
    }
    out.tableRows.push_back(std::move(tr));
  }
  return true;
}

// BUG-1241: the engine's AME (cmd_margins after logit/probit/poisson) sends each
// effect's standard error, not their covariances. The grid computes the same
// effects from the fit's own e(b)/e(V) with V = G e(V) G'; when it reads the fit
// and reproduces every effect and standard error, its off-diagonals complete
// r(V) (StataBE 19.5 rx2.do: `logit d x1 x2` / `margins, dydx(x1 x2)`
// el(r(V),1,2) -.0000380840715475375). Otherwise nothing is returned and the
// off-diagonals stay missing.
bool marginsAmeCrossVM4(SessionState& session, const LastModel& lm, const ojson& est,
                        std::vector<std::vector<double>>& V) {
  if (!est.contains("coeftable") || !est["coeftable"].is_array()) return false;
  const ojson& ct = est["coeftable"];
  if (ct.size() < 2) return false;
  MarginsGridFit fit;
  std::string why;
  if (!marginsGridFitM4(session, lm, fit, why)) return false;
  cecon::MarginsGridInput in;
  in.names = fit.names;
  in.beta = fit.b;
  in.V = fit.V;
  in.link = static_cast<cecon::GlmLinkFn>(fit.link);
  in.n = fit.n;
  in.data = fit.data;
  in.weights = fit.weights;
  in.points = {cecon::MarginsGridPoint{}};
  for (const auto& row : ct) in.dydx.push_back(row.value("name", std::string()));
  const cecon::MarginsGridResult r = cecon::margins_grid(in);
  if (!r.ok) return false;
  const std::vector<std::string> cols = cecon::margins_grid_colnames(r, true, std::string(), {});
  const size_t m = r.b.size();
  std::vector<size_t> at;
  for (const auto& row : ct) {
    const std::string nm = row.value("name", std::string());
    const auto it = std::find(cols.begin(), cols.end(), nm);
    if (it == cols.end()) return false;
    const size_t j = static_cast<size_t>(it - cols.begin());
    const double b = marginsNumM4(row, "b"), se = marginsNumM4(row, "se");
    const double seGrid = std::sqrt(std::max(0.0, r.V[j * m + j]));
    if (!std::isfinite(b) || !(se > 0) || std::fabs(r.b[j] - b) > 1e-9 * std::max(1.0, std::fabs(b)) ||
        std::fabs(seGrid - se) > 1e-8 * se)
      return false;
    at.push_back(j);
  }
  V.assign(ct.size(), std::vector<double>(ct.size(), 0.0));
  for (size_t a = 0; a < ct.size(); ++a)
    for (size_t c = 0; c < ct.size(); ++c) {
      const double sa = marginsNumM4(ct[a], "se");
      V[a][c] = a == c ? sa * sa : r.V[at[a] * m + at[c]];
    }
  return true;
}

}  // namespace

ojson runMargins(const std::vector<std::string>& marginlist,
                  const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                  SessionState& session) {
  const auto& lmOpt = session.lastModel();
  if (!lmOpt.has_value()) return noPreviousEstimation();
  const LastModel& lm = *lmOpt;
  // BUG-1210: results read by `estimates use` carry no e(sample) (it is 0
  // everywhere), and margins computes over e(sample): StataBE r(322).
  if (lm.esample == LastModel::EsampleState::kNone)
    return eval::errorBlock("engine_error", "e(sample) does not identify the estimation sample",
                            std::nullopt, std::nullopt, std::nullopt, "r(322)");
  // BUG-1449: the fit's half of r(), read before any path's `post` replaces e().
  const MarginsFitInfoM4 fitInfo = marginsFitInfoM4(session);
  // FU-475: after a least-squares fit the engine's margins refit applies a TS
  // fit's --keepif after the operators (cmds_postest.cpp cmd_margins,
  // plan_keepif), and dydx() answers from the coefficient table. The ML refits
  // have no row-mask input, so they keep the named refusal.
  if (!postestOlsCmdsM4().count(lm.cobraCmd)) {
    if (auto refused = postestTsKeepifRefusalM4("margins", lm)) return *refused;
  }

  // BUG-163: margins computes over e(sample) (oracle: N=4,282 after a fit
  // with `if t>=3`), so replay the fitted sample exactly as
  // lastModelBaseArgv does. cmd_margins consumes --keepif (cmds_postest).
  std::vector<std::string> args = {"margins", "--data",
                                    lm.fitDataPath.empty() ? lm.dataPath : lm.fitDataPath,
                                    "--y", lm.y, "--x",
                                    lm.x.empty() ? "(intercept)" : joinCommaM4(lm.x)};
  if (!lm.fitKeepif.empty()) {
    args.push_back("--keepif");
    args.push_back(lm.fitKeepif);
  }
  if (!lm.fe.empty()) {
    args.push_back("--fe");
    args.push_back(joinCommaM4(lm.fe));
  }
  // BUG-369's shape for margins: an `L.x` regressor needs the tsset context the
  // fit had, or the refit cannot build it (`margins` after `regress y L.x1 x2`
  // was ts_no_tsset, r(111), on main 2026-10-13).
  {
    std::vector<std::string> tsTokens = lm.x;
    tsTokens.insert(tsTokens.end(), lm.fe.begin(), lm.fe.end());
    tsTokens.push_back(lm.y);  // an operated depvar too (BUG-1277)
    if (translate::hasTsToken(tsTokens)) args = translate::injectTsContext(args, session);
  }

  std::optional<std::string> atVal;
  std::optional<std::string> dydxVal;
  bool postOpt = false;
  bool atmeansOpt = false;
  for (const auto& [k, v] : opts) {
    if (k == "at" && !v.isBool) atVal = v.strValue;
    if (k == "dydx" && !v.isBool) dydxVal = v.strValue;
    // w2b_estim.do:111/:121/:126 (`margins t, atmeans post`, `margins, atmeans`,
    // `margins, dydx(x1) atmeans`). A valued `atmeans(...)` is not Stata syntax.
    if (k == "atmeans") {
      if (!v.isBool) return marginsRefusalM4("option atmeans() not allowed", 198, "syntax_error");
      atmeansOpt = v.boolValue;
    }
    // Item 4 (User ruling 2026-09-09): `post` REPLACES e() with the margin
    // (Cells `B4REC|margins|dydx_post|0|`,
    // `B4VAL|ecmd|margins|b_mpg|-1.8c19072612ae4X+005`). Read here, applied
    // at both success return points below via postEstimationBlockM4.
    if (k == "post" && (!v.isBool || v.boolValue)) postOpt = true;
  }

  // BS-097: `at`/`dydx`/`post`/`atmeans` are the ONLY margins options this
  // handler reads (B6 audit: margins had ZERO unknown-option net; `post` joined
  // 2026-09-09, `atmeans` with FU-94). --fe/--vcov/--cluster (BS-086, BETA D5 batch) are
  // forwarded from session.lastModel() itself, not from a new opts-derived
  // key here, so they don't collide with this net. Any other leftover key
  // (e.g. `level()`, or a typo) reads as an honest reject instead of a
  // silent no-op.
  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kMarginsKnownOpts = kVocabMargins.known;
  // BUG-1502: an over() name that is no variable is r(111) "variable nosuchvar
  // not found" (StataBE 19.5, i11.do `margins, dydx(x1) over(nosuchvar)`), ahead
  // of the net below, which refuses over() itself (not served, BUG-1238). A name
  // that abbreviates a variable is left to the net.
  for (const auto& [k, v] : opts) {
    if (k != "over" || v.isBool) continue;
    std::vector<std::string> names;
    if (const std::vector<VarInfo>* card = session.residentReadCard()) {
      for (const auto& vi : *card) names.push_back(vi.name);
    } else {
      Datastore* live = nullptr;
      try {
        live = session.datastore();
      } catch (...) {
        live = nullptr;
      }
      if (live != nullptr) names = live->names();
    }
    for (std::string tok : splitTokensM4(v.strValue)) {
      if (tok.rfind("i.", 0) == 0) tok = tok.substr(2);
      if (!strutil::isValidStataIdentifier(tok) || names.empty()) continue;
      if (std::none_of(names.begin(), names.end(), [&tok](const std::string& n) {
            return n.compare(0, tok.size(), tok) == 0;
          }))
        return marginsStataErrorM4(
            session, marginsRefusalM4("variable " + tok + " not found", 111, "engine_error"));
    }
  }
  if (auto err = eval::rejectUnknownOptions(opts, kMarginsKnownOpts)) {
    return *err;
  }
  if (auto refused = marginsCovariateRefusalM4(lm, dydxVal, atVal))
    return marginsStataErrorM4(session, std::move(*refused));

  // A no-constant fit (FU-131): every engine margins form below refits, so each
  // argv carries --no-intercept (pushed here, ahead of the branch flags; cmd_margins
  // reads it on every path) and the engine drops the constant the fit never had
  // (w2p_estim_ev.log:1372 G_nc_margins_at, :1628 G_pnc_margins_dydx). The linear
  // dydx() answer reads the coefficient table and never calls the engine. Only the
  // poisson refit cannot drop it, and refuses by name.
  if (postestNoConstantUnrefittableM4(lm)) return postestNoConstantRejectM4("margins", lm);
  if (postestFitLacksConstantM4(lm)) args.push_back("--no-intercept");

  // A fixed-effects ML fit (xtpoisson/xtlogit, ppmlhdfe with absorb()): every
  // engine margins path refits the POOLED model, which is not the fit. After
  // `xtpoisson yc x c1, fe` `margins, dydx(x)` answered .4846500614524408 at rc 0
  // (Stata: .1088590169532439, the fit's own linear prediction,
  // w2v_margins_vce.log V7_xtp_dydx_x_b / V7_xtp_at_b / V7r_xtp_dydx_x_b), and
  // after ppmlhdfe with absorb() the refit was rc 1 (cmd_margins takes no --fe
  // in its GLM modes). Refused by name until margins answers from e(b)/e(V).
  // ME06: after `xtpoisson, fe` margins' default prediction is the linear index
  // xb with no constant and no fixed effect, so every form is answered from the
  // fit's own e(b)/e(V) over e(sample) by the grid (identity link, z reference):
  // w2v_margins_vce.log:2380 V7_xtp_dydx_x .1088590169532439 = _b[x], :2433
  // V7_xtp_at `.5601492709718607|.5319851840546966`.
  if (lm.cobraCmd == "xtpois" || lm.cobraCmd == "xtlogit") {  // BUG-1235: xtlogit's pu0
    MarginsGridFit fit;
    std::string why;
    if (xtpoisMarginsFitM4(lm, fit, why)) {
      if (atVal.has_value() && !trimM4(*atVal).empty()) {
        const cecon::MarginsAtParse p = cecon::margins_parse_at(*atVal);
        if (p.rc != 0) return marginsAtRefusalM4(session, p);
      }
      if (auto served = runMarginsGrid(session, fit, marginlist, opts)) return *served;
    }
  }
  if (lm.cobraCmd == "xtpois" || lm.cobraCmd == "xtlogit" ||
      ((lm.cobraCmd == "fepois" || lm.cobraCmd == "ppmlhdfe") && !lm.fe.empty())) {
    return marginsRefusalM4("margins after a fixed-effects `" + lm.cobraCmd +
                                "` fit is not implemented in this version of PIET (the engine "
                                "refits the pooled model, which is not the fit); nothing was "
                                "computed.",
                            198);
  }

  // margins-corpus-shapes: the forms the grid answers from the fit's own e(b)/e(V)
  // (marginsGridWantedM4); a fit it cannot read is refused by name rather than
  // handed to a refit that answers a different model.
  if (marginsGridWantedM4(lm, marginlist, atVal, dydxVal, atmeansOpt)) {
    if (atVal.has_value() && !trimM4(*atVal).empty()) {
      const cecon::MarginsAtParse p = cecon::margins_parse_at(*atVal);
      if (p.rc != 0) return marginsAtRefusalM4(session, p);
    }
    MarginsGridFit fit;
    std::string why;
    if (!marginsGridFitM4(session, lm, fit, why))
      return marginsRefusalM4("margins: " + why + "; nothing was computed.", 198);
    // The FE estimator recovers _cons and its full covariance after compacting
    // omitted columns (FU-633). Older/incomplete fit payloads still cannot serve
    // a predictive margin without that row; dydx() can read their slopes alone.
    if (!lm.fe.empty() && (!dydxVal.has_value() || trimM4(*dydxVal).empty()) &&
        std::none_of(fit.names.begin(), fit.names.end(),
                     [](const std::string& n) { return n == "_cons" || n == "(intercept)"; }))
      return marginsRefusalM4("margins: the fit's coefficient vector has no intercept (a factor "
                              "interaction with the absorbed variable); predictive margins after it "
                              "are not implemented in this version of PIET; nothing was computed.",
                              198);
    if (auto served = runMarginsGrid(session, fit, marginlist, opts)) return *served;
    return marginsRefusalM4(
        "margins: this combination of a marginlist, at(), dydx(*) or several marginlist terms is "
        "not implemented in this version of PIET; nothing was computed.",
        198);
  }

  const bool hasAt = atVal.has_value() && !atVal->empty();
  const bool hasDydx = dydxVal.has_value() && !dydxVal->empty();
  // The least-squares family whose engine refit IS the fitted model (the at()
  // path's kAtSafeCmds below names the same five spellings).
  const bool leastSquares = postestOlsCmdsM4().count(lm.cobraCmd) > 0;

  // `atmeans` holds the COVARIATES at their means and rebuilds every term made
  // from them; the engine's --atmeans holds each expanded DESIGN COLUMN at its
  // mean. The two agree only while every column is a covariate or a factor
  // indicator. An interaction or a polynomial (`c.x1##c.x1`, `i.id#c.x2`) has a
  // column whose mean is not the product of its parts' means: after `regress y
  // c.x1##c.x1 x2 i.t`, `margins, atmeans` answered 12.673333 at rc 0 where the
  // fitted point at the means gives 12.950175. Refused by name for every form
  // the held point reaches (marginlist, bare, at()); dydx() after a linear fit
  // is the coefficient at any point, and buildLinearAmeRawM4 already refuses an
  // interacted dydx() variable.
  if (atmeansOpt && !(dydxVal.has_value() && !dydxVal->empty() &&
                      !(atVal.has_value() && !atVal->empty()))) {
    bool interacted = false;
    for (const std::string& term : lm.x) interacted = interacted || term.find('#') != std::string::npos;
    if (lm.result.has_value() && lm.result->contains("coeftable") &&
        (*lm.result)["coeftable"].is_array()) {
      for (const auto& row : (*lm.result)["coeftable"]) {
        interacted = interacted || row.value("name", std::string()).find('#') != std::string::npos;
      }
    }
    if (interacted) {
      return marginsRefusalM4(
          "margins, atmeans after a model with an interaction or polynomial term is not "
          "implemented in this version of PIET (the covariates would be held at their means "
          "and the derived terms rebuilt from them); nothing was computed.",
          198);
    }
  }

  // FU-94: the marginlist (the positional terms) used to be dropped at the
  // dispatcher, so `margins t` answered what bare `margins` answers, at rc 0.
  // Each term is checked the way StataBE 19.5 checks it (live, 2026-10-12, not a
  // golden): a name that is no variable is r(111), a variable with non-integer
  // values r(452) ("x1:  factor variables may not contain noninteger values"),
  // an integer variable that is no factor of the model r(322) ("factor id not
  // found in list of covariates"). `i.VAR` is the same term as `VAR`.
  if (!marginlist.empty()) {
    std::vector<std::string> vars;
    for (const std::string& term : marginlist) {
      if (term.find('#') != std::string::npos || term.find('(') != std::string::npos) {
        return marginsRefusalM4("margins: the marginlist term `" + term +
                                    "` (an interaction or a level list) is not implemented in "
                                    "this version of PIET; nothing was computed.",
                                198);
      }
      const std::string var = term.rfind("i.", 0) == 0 ? term.substr(2) : term;
      const MarginsTermM4 role = marginsTermRoleM4(lm, var);
      if (role == MarginsTermM4::Other) {
        return marginsRefusalM4("margins: " + var + " enters the model in an interaction or "
                                    "under an operator; per-level margins of it are not "
                                    "implemented in this version of PIET.",
                                198);
      }
      if (role != MarginsTermM4::Factor) {
        Datastore* live = nullptr;
        try {
          live = session.datastore();
        } catch (...) {
          live = nullptr;
        }
        if (live != nullptr && !live->has(var)) {
          return marginsStataErrorM4(
              session, marginsRefusalM4("variable " + var + " not found", 111, "engine_error"));
        }
        bool nonInteger = false;
        if (live != nullptr) {
          const cecon::Column& c = live->col(var);
          if (c.dtype == cecon::DType::F64 && c.f64() != nullptr) {
            const double* pv = c.f64();
            for (int64_t r = 0; r < live->nRows() && !nonInteger; ++r) {
              nonInteger = !std::isnan(pv[r]) && pv[r] != std::floor(pv[r]);
            }
          }
        }
        if (nonInteger) {
          return marginsRefusalM4(var + ":  factor variables may not contain noninteger values",
                                  452, "engine_error");
        }
        return marginsRefusalM4("factor " + var + " not found in list of covariates", 322,
                                "engine_error");
      }
      vars.push_back(var);
    }
    if (vars.size() > 1) {
      return marginsRefusalM4("margins: a marginlist of several terms is not implemented in this "
                              "version of PIET; ask for one factor at a time.",
                              198);
    }
    if (hasAt || hasDydx) {
      return marginsRefusalM4("margins " + vars[0] + " combined with " +
                                  std::string(hasDydx ? "dydx()" : "at()") +
                                  " is not implemented in this version of PIET; nothing was "
                                  "computed.",
                              198);
    }
    if (!leastSquares || !lm.fe.empty()) {
      return marginsRefusalM4("margins " + vars[0] + " after `" + lm.cobraCmd + "`" +
                                  std::string(lm.fe.empty() ? "" : " with absorbed effects") +
                                  " is not implemented in this version of PIET (per-level "
                                  "predictive margins cover regress without absorbed effects).",
                              198);
    }
    {
      ojson reject;
      if (!postestRefitMatchesFitM4("margins " + vars[0], lm, reject)) return reject;
    }
    return runLinearPredictiveMarginsM4(lm, session, args, vars[0], atmeansOpt, postOpt, fitInfo);
  }

  // FU-95: bare `margins` is the predictive margin of the response -- one row,
  // the mean prediction over e(sample) (live StataBE 19.5: el(r(b),1,1)
  // 12.67333335032065733 after `regress y x1 x2 i.t`, the mean of y). The
  // engine's no---dydx path is the logit/probit/poisson AME of every covariate,
  // which is what bare `margins` used to return after a nonlinear fit, at rc 0.
  // After a nonlinear fit the predictive margin waits for its oracle arm (the
  // w2p capture), so it refuses by name; it never answers with effects.
  if (!hasAt && !hasDydx) {
    if (!leastSquares) {
      return marginsRefusalM4(
          "margins without a marginlist, dydx() or at() after `" + lm.cobraCmd +
              "` (the average predicted response) is not implemented in this version of PIET; "
              "nothing was computed. Marginal effects are `margins, dydx(varlist)`.",
          198);
    }
    if (!lm.fe.empty()) {
      return marginsRefusalM4(
          "margins without a marginlist, dydx() or at() after a fit with absorbed effects is "
          "not implemented in this version of PIET; nothing was computed.",
          198);
    }
    {
      ojson reject;
      if (!postestRefitMatchesFitM4("margins", lm, reject)) return reject;
    }
    return runLinearPredictiveMarginsM4(lm, session, args, std::string(), atmeansOpt, postOpt, fitInfo);
  }

  // `atmeans` with at()/dydx(): after a least-squares fit dy/dx is the
  // coefficient (at any covariate values) and at() holds the other covariates
  // at their float-stored means (engine --atmeans). After a nonlinear fit both
  // become effects/predictions AT the means, which no engine path computes.
  if (atmeansOpt && (!leastSquares || !lm.fe.empty())) {
    return marginsRefusalM4("margins, atmeans after `" + lm.cobraCmd + "`" +
                                std::string(lm.fe.empty() ? "" : " with absorbed effects") +
                                " is not implemented in this version of PIET; nothing was "
                                "computed.",
                            198);
  }

  std::vector<std::string> dydxVars;
  // AER-B4 margins-dydx-OLS: set by the linear-AME branch below, which answers from the fitted
  // model's own coefficient table instead of calling the engine. When it holds
  // a value the shared tail skips runCobraJsonM4 and runs on this payload.
  std::optional<ojson> linearAmeRaw;
  if (atVal.has_value() && !atVal->empty()) {
    // An at() list Stata rejects (`d=(min(.2)max)` r(121), `(stat) varlist`
    // r(198)) is refused with the kernel parser's rc before anything runs; every
    // list below is expanded by that same parser (parseAtNumlistM4).
    {
      const cecon::MarginsAtParse atParse = cecon::margins_parse_at(*atVal);
      if (atParse.rc != 0) return marginsAtRefusalM4(session, atParse);
    }
    // BUG-571 sibling: every at() form below is answered by an engine refit
    // (the linear --at branch refits feols from y/x/fe; the GLM at() and
    // dydx()+at() branches refit from load_glm), and none of them carries the
    // fit's weight or areg's clustered dof. Measured on main 2026-09-29
    // (BUG-571 probe frame): after `regress y x1 x2 [aweight=w]`,
    // `margins, at(x1=1)` answered 3.3492323 at rc 0 -- the unweighted fit's
    // _b[_cons]+_b[x1]+_b[x2]*mean(x2) -- where the weighted fit's is
    // 3.5853964; after `areg ..., absorb(id) vce(cluster id)` its SE is the
    // reghdfe fit's, byte for byte. The same refusal as test/lincom/nlcom.
    {
      ojson reject;
      if (!postestRefitMatchesFitM4("margins, at()", lm, reject)) return reject;
    }
    // FU-342: an at() variable that also enters an interaction or polynomial
    // term (`c.x1##c.x1`, `i.t#c.x1`) owns a derived design column the engine's
    // at() paths cannot rebuild -- they override the column named x1 and keep
    // x1#x1 at its observed values (39fd4ccb cmds_postest.cpp:1030-1040), so
    // `margins, at(x1=5)` after `regress y c.x1##c.x1 x2` answered with x1#x1
    // at its sample mean instead of 25, at rc 0. Refused by name here for every
    // at() form; the engine's dydx()+at() branch already refuses the same shape.
    for (const auto& spec : parseAtNumlistM4(*atVal)) {
      size_t pos = 0;
      while (pos <= spec.size()) {
        const size_t comma = spec.find(',', pos);
        const std::string tok =
            spec.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        const std::string var = trimM4(tok.substr(0, tok.find('=')));
        if (!var.empty() && marginsVarInInteractionM4(lm, var)) {
          return marginsRefusalM4(
              "margins, at(" + var + "=...) after a model where " + var +
                  " enters an interaction or polynomial term is not implemented in this version "
                  "of PIET (the derived terms would have to be rebuilt from the at() value); "
                  "nothing was computed.",
              198);
        }
        if (comma == std::string::npos) break;
        pos = comma + 1;
      }
    }
    // BS-086 (BETA D5 batch) ENGINE FINDING: cobra's `margins --at`
    // (predictive margins) ALWAYS internally RE-ESTIMATES a plain OLS
    // regression from the model's own --y/--x/--fe, regardless of any
    // --model flag — live-verified DIRECTLY against the compiled engine:
    // `--model logit`, `--model probit`, `--model poisson`, and even a
    // BOGUS --model value all produce the IDENTICAL byte-for-byte output
    // in --at mode as no --model flag at all, and that shared output
    // matches a from-scratch plain-OLS refit of y~x to 10+ significant
    // digits. This is NOT a bug this batch introduced — the pre-existing
    // `--at` branch never forwarded --model at all either — but it means
    // `margins, at(...)` after ANY non-OLS estimator silently returns the
    // WRONG number: confirmed against the D5c/D5d oracle itself,
    // `margins, at(income=13000)` after `logit high income unemp` returns
    // this engine's 0.41098... vs Stata's real logistic-link 0.4074234 (a
    // ~1% relative gap, not rounding noise) — a "silently wrong" shape
    // this codebase's honest-reject doctrine forbids shipping (see e.g.
    // BS-023/BS-052). The SAME mechanism (raw --y/--x OLS refit) means
    // this is equally wrong after tobit/heckman/qreg/mediate (non-OLS
    // point estimates), newey/prais (HAC/AR1 transforms), iv/liml/reg3
    // (2SLS/GMM/SUR), and xtreg's random-effects GLS path (lm.cobraCmd
    // =="xtreg" — NOT the ,fe path, which routes through cobraCmd=="feols"
    // and is genuinely safe, see below) — none of these estimators'
    // actual fitted coefficients equal a plain OLS refit of y on x.
    // kAtSafeCmds is exactly the set of lm.cobraCmd values for which the
    // engine's OLS-refit IS the model's own true coefficients (plain
    // regress/feols, including reghdfe and xtreg's ,fe path, both of
    // which already share cobraCmd=="feols"/"ols"/"regress"/"reg"/"r" per
    // runXtreg/handlers.cpp::runRegress above) — everything else honest-
    // rejects rather than silently answer a different question than the
    // one the user's `at()` call asked.
    static const std::set<std::string> kAtSafeCmds = {"feols", "ols", "regress", "reg", "r"};
    // Hoisted out of the dydx+at() branch below so the plain at() branch
    // (BUG-188-nonOLS mirror, further down) can test the same set — both
    // branches route the identical GLM family to cobra's real fitted-model
    // margins machinery instead of the OLS-refit path.
    static const std::set<std::string> kDydxAtModels = {"logit", "probit", "poisson", "fepois",
                                                        "ppmlhdfe", "xtpois", "xtlogit",
                                                        "cloglog"};  // BUG-1547
    // P7: `margins, dydx(...) at(...)` is a different request from the
    // predictive margin BS-086 describes above, and the engine gained a real
    // branch for it — the model is fitted once, then the AME is taken over a
    // design copy with the at() variables fixed counterfactually. That branch
    // reuses the ACTUAL fitted model, so the OLS-refit hazard the note above
    // documents does not apply to it, and the GLM estimators are supported.
    if (dydxVal.has_value() && !dydxVal->empty()) {
      if (!kDydxAtModels.count(lm.cobraCmd)) {
        return engineErrorBlockM4(
            "margins, dydx(...) at(...) is not supported after `" + lm.cobraCmd + "` in this "
            "version of PIET (the combined marginal-effects-at-a-point path covers the "
            "logit/probit/poisson family).",
            "unsupported_option");
      }
      for (const auto& spec : parseAtNumlistM4(*atVal)) {
        args.push_back("--at");
        args.push_back(floatAtSpecM4(spec));
      }
      args.push_back("--dydx");
      args.push_back(joinCommaM4(splitTokensM4(*dydxVal)));
      args.push_back("--model");
      args.push_back(lm.cobraCmd == "logit" || lm.cobraCmd == "probit" || lm.cobraCmd == "cloglog"
                         ? lm.cobraCmd
                         : std::string("poisson"));
      pushFitAsisM4(args, lm);
      if (auto refused = pushGlmAtVceM4(args, lm, "dydx(...) at(...)")) return *refused;
      // No --data here: this handler's shared tail runs against the RESIDENT
      // frame, which is the data source, and the serve path rejects an argv
      // that names --data or --out.
      std::vector<std::string> finalArgs = args;
      finalArgs.push_back("--tidy");
      ojson raw;
      try {
        raw = runCobraJsonM4(finalArgs);
      } catch (const CobraError& ce) {
        return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
      }
      ojson block = cobraToEstimationM4(raw, "margins", &session);
      // session.lastMargins/lastMarginsDydx feed marginsplot — publish the
      // `post` e() AFTER them, so marginsplot keeps working post-`post`.
      session.lastMargins = raw;
      // FU-179: r() is replaced. r(b)/r(V)/r(table) carry one column per effect
      // and point, the effects in the order their variables enter e(b), each
      // over the points: `x:1bn._at x:2._at` (w2v_margins_siblings.log:2894-2921
      // F_robust_dydx_at_po_cols/_b/_v/_rt), V across points from the engine's
      // Jacobians. A factor effect's matrix also carries the base level's column
      // (b 0, V 0), which no engine row represents; such a form posts r(N) only,
      // as the AME branch below does.
      {
        MarginsRInfoM4 info;
        info.dydx = true;
        if (dydxVal->find('*') == std::string::npos)  // dydx(*): expansion not reproduced
          for (const auto& v : splitTokensM4(*dydxVal))
            info.xvars += (info.xvars.empty() ? "" : " ") + v;
        info.atPoints = static_cast<int>(parseAtNumlistM4(*atVal).size());
        info.cluster = !lm.cluster.empty();
        info.fit = fitInfo;  // BUG-1449
        info.kAt = 1;
        info.title = marginsTitleM4(true, false, *atVal, std::string(), fitInfo.ebNames);
        info.nKnown = true;
        // Flat engine order: point by point, the effects in --dydx order within each.
        struct Cell {
          std::string var;
          size_t point = 0;
          size_t flat = 0;
          ojson row;
        };
        std::vector<Cell> cells;
        bool factorEffect = false, shapeOk = raw.contains("equations") && raw["equations"].is_array();
        size_t flat = 0;
        if (shapeOk) {
          const ojson& eqs = raw["equations"];
          for (size_t pnt = 0; pnt < eqs.size(); ++pnt) {
            const ojson& rows = eqs[pnt].contains("coeftable") ? eqs[pnt]["coeftable"] : ojson::array();
            for (const auto& row : rows) {
              const std::string name = row.value("name", std::string());
              const size_t dot = name.find('.');
              if (dot != std::string::npos && dot > 0 &&
                  std::isdigit(static_cast<unsigned char>(name[0])))
                factorEffect = true;
              cells.push_back(Cell{name, pnt, flat++, row});
            }
          }
          shapeOk = raw.contains("V") && raw["V"].is_array() && raw["V"].size() == flat && flat > 0;
        }
        if (shapeOk) info.continuous = factorEffect ? 0 : 1;
        if (shapeOk && !factorEffect) {
          auto fitIndex = [&](const std::string& var) {
            if (!lm.result.has_value() || !lm.result->contains("coeftable") ||
                !(*lm.result)["coeftable"].is_array())
              return static_cast<size_t>(0);
            const ojson& fitCt = (*lm.result)["coeftable"];
            for (size_t j = 0; j < fitCt.size(); ++j)
              if (coefBelongsToVarM4(fitCt[j].value("name", std::string()), var)) return j;
            return fitCt.size();
          };
          std::stable_sort(cells.begin(), cells.end(), [&](const Cell& x, const Cell& y) {
            const size_t fx = fitIndex(x.var), fy = fitIndex(y.var);
            if (fx != fy) return fx < fy;
            if (x.var != y.var) return x.var < y.var;
            return x.point < y.point;
          });
          const bool single = raw["equations"].size() == 1;
          std::vector<std::string> names;
          std::vector<double> bv;
          ojson tableRows = ojson::array();
          for (const auto& c : cells) {
            names.push_back(single ? c.var
                                   : c.var + ":" + std::to_string(c.point + 1) +
                                         (c.point == 0 ? "bn" : "") + "._at");
            bv.push_back(marginsNumM4(c.row, "b"));
            ojson tr;
            tr["name"] = names.back();
            for (const char* key : {"b", "se", "t", "z", "p", "ci_lo", "ci_hi"})
              if (c.row.contains(key)) tr[key] = c.row[key];
            tableRows.push_back(tr);
          }
          std::vector<std::vector<double>> V(cells.size(), std::vector<double>(cells.size(), 0.0));
          bool vOk = true;
          for (size_t i = 0; i < cells.size() && vOk; ++i) {
            const ojson& vr = raw["V"][cells[i].flat];
            vOk = vr.is_array() && vr.size() == flat;
            for (size_t j = 0; j < cells.size() && vOk; ++j)
              V[i][j] = vr[cells[j].flat].is_number() ? vr[cells[j].flat].get<double>()
                                                      : std::numeric_limits<double>::quiet_NaN();
          }
          if (vOk) {
            postMarginsRClassM4(session, block, info, names, bv, V, tableRows);
          } else {
            postMarginsRClassM4(session, block, info, {}, {}, {});
          }
        } else {
          postMarginsRClassM4(session, block, info, {}, {}, {});
        }
      }
      if (postOpt) postEstimationBlockM4(session, block, "margins", lm.depvar, lm.dataPath);
      return block;
    }
    // BUG-188-nonOLS: mirror of the dydx+at() branch above. `margins, at(...)`
    // with no dydx() is a predictive margin, and cobra's GLM predictive-margins
    // branch (apps/cmds/cmds_postest.cpp, `--at .. --model ..` with no --dydx)
    // already fits logit/probit/poisson for real and computes the average
    // adjusted prediction correctly (BUG-194's own oracle table exercises this
    // exact path) — the OLS-refit hazard the reject below exists for does not
    // apply to this family, so forward --model instead of rejecting.
    if (kDydxAtModels.count(lm.cobraCmd)) {
      args.push_back("--model");
      args.push_back(lm.cobraCmd == "logit" || lm.cobraCmd == "probit" || lm.cobraCmd == "cloglog"
                         ? lm.cobraCmd
                         : std::string("poisson"));
      pushFitAsisM4(args, lm);
      if (auto refused = pushGlmAtVceM4(args, lm, "at(...)")) return *refused;
    } else if (!kAtSafeCmds.count(lm.cobraCmd)) {
      return engineErrorBlockM4(
          "margins, at(...) is not supported after `" + lm.cobraCmd + "` in this version of PIET "
          "— the engine's predictive-margins machinery always re-estimates a plain OLS regression "
          "from the model's y/x (and fe, if any) rather than reusing the model actually fit, and "
          "completely ignores any --model flag in this mode (live-verified) — silently wrong for "
          "any non-OLS estimator. Only `regress`/`feols`/`reghdfe`/`xtreg ,fe` are supported for "
          "margins, at(...) in this version of PIET.",
          "unsupported_option");
    }
    // BUG-188 (P2-PERF ARC-P). This split at() on whitespace and commas, which
    // is exactly the bug P7 already fixed for the dydx+at branch: it turns
    // `at(x1=(0 1))` into the two nonsense fragments `x1=(0` and `1)` and the
    // engine answers "'x1=(0' has a non-numeric value". The parser that
    // understands Stata's real at() grammar — including the parenthesised
    // numlist and the cartesian product across several variables — is
    // parseAtNumlistM4, sitting a few hundred lines up and used by the other
    // branch. Use it here too rather than keep a second, worse grammar.
    //
    // A bare single-value at() (`at(x1=3)`, `at(x1=3 x2=4)`) produces exactly
    // the same argv it did before, so the previously-working spellings are
    // untouched.
    for (const auto& spec : parseAtNumlistM4(*atVal)) {
      args.push_back("--at");
      args.push_back(floatAtSpecM4(spec));
    }
    // Reached only by the least-squares family (atmeans after any other fit was
    // refused above): the covariates at() does not set are held at their
    // float-stored means rather than their exact ones.
    if (atmeansOpt) args.push_back("--atmeans");
    // BS-086: forward the SAME vcov/cluster context the original fit used
    // (cobra margins' own usage banner: `predictive margins (linear): ...
    // [--vcov ..] [--cluster C]`) — safe here specifically because
    // kAtSafeCmds (above) is exactly the set of estimators for which the
    // engine's internal OLS refit reproduces the ORIGINAL fit's own
    // coefficients; the reported SE should likewise reflect whatever
    // vce() the original model specified rather than silently reverting
    // to iid.
    // Only for the OLS-refit family: the GLM branches (kDydxAtModels, just
    // above) spell the fit's VCE `--vce`/`--cluster` (pushGlmAtVceM4).
    if (!kDydxAtModels.count(lm.cobraCmd)) {
      if (!lm.vcov.empty() && lm.vcov != "iid") {
        args.push_back("--vcov");
        args.push_back(lm.vcov);
      }
      if (!lm.cluster.empty()) {
        args.push_back("--cluster");
        args.push_back(joinCommaM4(lm.cluster));
      }
      // FU-385: after `regress ..., vce(cluster a b)` the refit runs the fit's
      // multiway convention, not reghdfe's Gmin factor (main served the Gmin
      // margin SE at rc 0). --keep-nested-fe-dof never reaches here: areg with a
      // cluster refuses above (postestRefitMatchesFitM4).
      for (const auto& f : fitConventionFlagsM4(lm))
        if (f == "--multiway-subset-factors" || f == "--keep-singletons" ||
            f == "--collinear-forward")
          args.push_back(f);
    }
  } else {
    // BS-063/BS-064 (BETA D1): "iv"/"reg3" were already anticipated here
    // (pre-dating this batch); "liml" is this batch's new lm.cobraCmd value
    // and needs the SAME linear-model dydx reject — without it, `ivregress
    // liml ...` followed by `margins, dydx(*)` would fall into the ELSE
    // branch below (kModelMap lookup misses "liml" too, so no --model flag
    // either) and silently run cobra's AME machinery over what is actually
    // a linear IV fit. "heckman"/"mediate" are this batch's (BETA D2, BS-070/
    // 072) additions: heckman's outcome equation is itself a plain linear
    // fit (dydx of a linear coefficient is a meaningless constant, same
    // rationale as tobit/qreg above), and margins has no coherent meaning
    // at all after `mediate` (lm.x is a best-effort {mediator, treatment}
    // placeholder, not a real regression spec) — both need the SAME reject
    // rather than falling to the ELSE branch's silent AME computation.
    // AER-B4 margins-dydx-OLS (2026-09-09) closes the half of this reject the oracle
    // contradicts. tests/eval/golden/aer_b4/pins_oracle_r.do:75-79 fits
    // `quietly regress price mpg weight` then `margins, dydx(mpg) post` and
    // Stata answers rc 0 — PIET refused the combination outright. The fix is
    // NOT to drop feols/regress from kLinearCmds and let this branch's engine
    // call handle them: cmd_margins (apps/cmds/cmds_postest.cpp:929) reads
    // `opt(a, "--model", "logit")` and its AME path computes the
    // logit/probit/poisson family only, so a continuous outcome like `price`
    // sent with no --model would be answered through a LOGISTIC AME — the
    // silent wrong number Doctrine gate ① forbids, strictly worse than the
    // honest reject. buildLinearAmeRawM4 (above) instead reads the answer off
    // the fitted model's own coefficient table, where a linear model's dy/dx
    // already sits, and never calls the engine.
    //
    // kLinearCmds therefore keeps all 15 names, and the OLS family is
    // diverted BEFORE it — only for a `dydx()` request. A `margins` with NO
    // dydx() after a linear model is a different question (the unconditional
    // predictive margin over the sample, which is not any coefficient), and
    // this engine still has no path to it, so it keeps refusing. Narrowing
    // kLinearCmds instead would have dropped that case through to the
    // kModelMap miss below and answered it as a logit.
    static const std::set<std::string> kLinearAmeCmds = {"feols", "ols", "regress", "reg", "r"};
    // Hoisted above the rejects (it used to sit after the kModelMap lookup) so
    // the linear-AME branch can see which variables were asked for. Parsing is
    // byte-for-byte the same; nothing between here and its old position reads
    // dydxVars.
    if (dydxVal.has_value() && !dydxVal->empty()) {
      std::string cur;
      for (char c : *dydxVal) {
        if (c == ' ' || c == ',' || c == '\t') {
          if (!cur.empty() && cur != "*") dydxVars.push_back(cur);
          cur.clear();
        } else {
          cur += c;
        }
      }
      if (!cur.empty() && cur != "*") dydxVars.push_back(cur);
    }
    if (kLinearAmeCmds.count(lm.cobraCmd) && dydxVal.has_value() && !dydxVal->empty()) {
      ojson built;
      ojson reject;
      if (!buildLinearAmeRawM4(lm, dydxVars, built, reject)) return reject;
      // BUG-1549: the fit's rows are t(e(df_r))-referenced; after reghdfe and
      // xtreg, fe margins' are normal-referenced.
      if (marginsNormalRefM4(lm))
        for (auto& row : built["coeftable"]) marginsNormalRowM4(row);
      linearAmeRaw = std::move(built);
    }
    // BUG-473-sibling (item 4, 2026-09-09): the remaining ten names have real
    // structural reasons — iv/liml/reg3's endogenous system, xtreg's
    // random-effects GLS path, and tobit/qreg/heckman/mediate's non-OLS or
    // non-coefficient marginal effects — none of which the coefficient table
    // above answers, and none of which the engine's GLM AME answers either.
    static const std::set<std::string> kLinearCmds = {
        "feols",  "ols",     "regress", "reg",    "r",      "iv",       "liml",
        "reg3",   "xtreg",   "newey",   "prais",  "tobit",  "qreg",     "heckman",
        "mediate",
    };
    // FU-326: an option this build will not take is r(198); the default
    // engine_error code made `display _rc` 1 here (after tobit/qreg/...).
    if (!linearAmeRaw.has_value() && kLinearCmds.count(lm.cobraCmd)) {
      return engineErrorBlockM4(
          "margins: marginal effects (dydx) after a linear model are not supported. "
          "Use predictive margins, e.g. `margins, at(mpg=20)`.",
          "unsupported_option");
    }
    static const std::map<std::string, std::string> kModelMap = {
        {"logit", "logit"}, {"probit", "probit"}, {"fepois", "poisson"},
        {"poisson", "poisson"}, {"xtpois", "poisson"}, {"xtlogit", "logit"},
        {"cloglog", "cloglog"},  // BUG-1547
    };
    // BUG-702: a cobraCmd missing from kModelMap used to be sent with no
    // --model at all, and cmd_margins answers that through its `--model logit`
    // default. After `nbreg cnt x1 x2` that was rc 1 with a "not binary"
    // engine message; after `oprobit` on a 0/1 outcome it was a logit AME
    // (-.0012381763, where `probit` on the same data gives -.0012386098) at
    // rc 0. The same honest refusal the at() branches above give, by name.
    // The linear-AME answer (linearAmeRaw) never reaches the engine, so it
    // needs no --model and is not refused here.
    auto mit = kModelMap.find(lm.cobraCmd);
    if (mit != kModelMap.end()) {
      args.push_back("--model");
      args.push_back(mit->second);
      pushFitAsisM4(args, lm);
    } else if (!linearAmeRaw.has_value()) {
      return engineErrorBlockM4(
          "margins is not supported after `" + lm.cobraCmd + "` in this version of PIET "
          "(the engine's marginal-effects path covers the logit/probit/poisson family "
          "and the linear dydx() above; any other model would be answered as a logit).",
          "unsupported_option");
    }
    // W:P5: `--dydx` now takes a VALUE on the engine's CLI (it selects which
    // variables get an effect, and drives the interaction chain rule and the
    // factor discrete-change contrast). It used to be pushed as a bare flag
    // with the selection applied only as a post-hoc row filter here — which
    // meant the next argv element (`--tidy`) was swallowed as its value.
    // Stata's `dydx(*)` means "every covariate", which is the engine's own
    // behaviour when no `--dydx` is given — so an empty selection sends no
    // flag rather than forwarding a literal `*` the engine would reject as
    // an unknown variable name.
    if (!dydxVars.empty()) {
      args.push_back("--dydx");
      args.push_back(joinCommaM4(dydxVars));
    }

    // W:P5: the AME must be computed under the SAME weights and VCE the
    // model was fitted with — Stata's margins reports `Model VCE: Robust`
    // after a clustered pweighted logit, and the delta-method SE is built
    // from that VCE. Forwarding these was the difference between
    // dy/dx x1 = .293147 (wrong) and .2717222 (Stata) on the P5 answerbook.
    // BUG-1547: the engine's AME takes a pweight only (an fweighted logit/probit
    // goes to the grid, which has no cloglog link), so a cloglog fit under another
    // weight is refused by name, not with the engine's rc 1.
    if (lm.cobraCmd == "cloglog" && !lm.fitWeight.empty() && lm.fitWtype != "pw" &&
        lm.fitWtype != "pweight")
      return marginsRefusalM4("margins after a [" + lm.fitWtype + "=" + lm.fitWeight +
                                  "]-weighted `cloglog` fit is not implemented in this version "
                                  "of PIET; nothing was computed.",
                              198);
    if (!lm.fitWeight.empty()) {
      args.push_back("--weight");
      args.push_back(lm.fitWeight);
      if (!lm.fitWtype.empty()) {
        args.push_back("--wtype");
        args.push_back(lm.fitWtype);
      }
    }
    if (!lm.vcov.empty() && lm.vcov != "iid") {
      args.push_back("--vce");
      // lm.vcov carries the engine's linear spelling ("hc1"); the GLM
      // margins path names the same shape "robust", as cobra's own
      // logit/probit CLI does.
      args.push_back(lm.vcov == "hc1" ? std::string("robust") : lm.vcov);
    }
    if (!lm.cluster.empty()) {
      args.push_back("--cluster");
      args.push_back(joinCommaM4(lm.cluster));
    }
  }

  ojson raw;
  const bool linearAme = linearAmeRaw.has_value();
  if (linearAme) {
    // AER-B4 margins-dydx-OLS: the linear-AME branch already holds the whole answer, read off
    // the fitted model's own coefficient table — there is no engine call to
    // make, and `args` (built for one) is discarded. Everything below runs on
    // this payload unchanged: marginsplot caching, the dydx row filter, `post`.
    raw = std::move(*linearAmeRaw);
  } else {
    args.push_back("--tidy");
    try {
      raw = runCobraJsonM4(args);
    } catch (const CobraError& ce) {
      return engineErrorBlockM4(cobraErrorStrM4(ce));
    }
  }
  // 1.5.0 marginsplot (dispatcher.py's session.set_last_margins): cache the
  // RAW cobra JSON (not the formatted estimation block) + the requested
  // dydx var(s) so `marginsplot` can plot it without re-running cobra.
  session.lastMargins = raw;
  session.lastMarginsDydx = dydxVars;

  ojson est = cobraToEstimationM4(raw, "margins_cmd", &session);
  if (!dydxVars.empty() && est.contains("coeftable") && est["coeftable"].is_array()) {
    std::set<std::string> dydxSet(dydxVars.begin(), dydxVars.end());
    ojson filtered = ojson::array();
    for (const auto& row : est["coeftable"]) {
      if (!row.contains("name")) continue;
      const std::string name = row["name"].get<std::string>();
      // W:P5: a FACTOR variable's effect row is named `<level>.<var>`
      // (BUG-101), but the user asked for the bare `<var>` — `margins,
      // dydx(x1 treated)` must keep the `1.treated` row. Matching only the
      // literal name silently dropped every factor effect from the table
      // while the engine had computed it correctly.
      bool wanted = dydxSet.count(name) > 0;
      if (!wanted) {
        const size_t dot = name.find('.');
        if (dot != std::string::npos && dot + 1 < name.size()) {
          wanted = dydxSet.count(name.substr(dot + 1)) > 0;
        }
      }
      if (wanted) filtered.push_back(row);
    }
    // Stata posts the effects in the order the variables enter e(b), not the
    // dydx() order (w2v_margins_siblings C2_dydx_tr_x_cols `x 0b.treatment
    // 1.treatment` for dydx(treatment x), C7_dydx_c1x_cols `x c1` for
    // dydx(c1 x)); the engine's AME answers in --dydx order. The grid
    // (posthest.h margins_grid) and buildLinearAmeRawM4 already use the fit's.
    if (lm.result.has_value() && lm.result->contains("coeftable") && (*lm.result)["coeftable"].is_array()) {
      const ojson& fitCt = (*lm.result)["coeftable"];
      auto fitIndex = [&](const ojson& row) {
        const std::string var = coefSourceVarM4(row.value("name", std::string()));
        for (size_t j = 0; j < fitCt.size(); ++j)
          if (coefBelongsToVarM4(fitCt[j].value("name", std::string()), var)) return j;
        return fitCt.size();
      };
      std::vector<std::pair<size_t, ojson>> keyed;
      for (const auto& row : filtered) keyed.emplace_back(fitIndex(row), row);
      std::stable_sort(keyed.begin(), keyed.end(),
                       [](const auto& a, const auto& b) { return a.first < b.first; });
      filtered = ojson::array();
      for (auto& kr : keyed) filtered.push_back(std::move(kr.second));
    }
    est["coeftable"] = filtered;
  }
  // FU-179: r(b)/r(V)/r(N) from the rows the user sees -- after the dydx filter
  // above, so r(b) holds exactly the effects asked for. A FACTOR effect row
  // (`2.t`) is the exception: Stata's matrix also carries the base level as a
  // column of its own (b 0, V 0; w2_glm_lifecycle.log:1663 gam_cl_margins
  // column 1 after `dydx(g)`), which no row here represents, so el(r(b),1,1)
  // would read the 2.t effect where Stata reads 0. Until that layout (and its
  // order beside continuous effects) is pinned for r(), such a form replaces
  // r() with r(N) only and r(b) stays honestly not set.
  bool factorEffect = false;
  if (est.contains("coeftable") && est["coeftable"].is_array()) {
    for (const auto& row : est["coeftable"]) {
      const std::string name = row.value("name", std::string());
      const size_t dot = name.find('.');
      if (dot == std::string::npos || dot == 0) continue;
      bool digits = true;
      for (size_t c = (name[0] == '-' ? 1 : 0); c < dot; ++c)
        if (!std::isdigit(static_cast<unsigned char>(name[c]))) digits = false;
      if (digits && !(name[0] == '-' && dot == 1)) factorEffect = true;
    }
  }
  MarginsRInfoM4 info;
  info.leastSquares = leastSquares && !marginsNormalRefM4(lm);  // BUG-1549
  info.dydx = hasDydx;
  if (hasDydx && !factorEffect && dydxVal->find('*') == std::string::npos) {
    // In e(b) order, as the effects (StataBE 19.5 s4.do `dydx(x2 x1)` after
    // regress and after logit: r(xvars) "x1 x2"); the rows above are sorted so.
    std::string xv;
    if (est.contains("coeftable") && est["coeftable"].is_array())
      for (const auto& row : est["coeftable"])
        xv += (xv.empty() ? "" : " ") + row.value("name", std::string());
    info.xvars = xv;  // not for dydx(*): Stata's expansion is not reproduced
  }
  info.atPoints = hasAt ? static_cast<int>(parseAtNumlistM4(*atVal).size()) : (atmeansOpt ? 1 : 0);
  info.cluster = !lm.cluster.empty();
  // BUG-1449
  info.fit = fitInfo;
  info.kAt = hasAt || atmeansOpt ? 1 : 0;
  info.title = marginsTitleM4(hasDydx, atmeansOpt, hasAt ? *atVal : std::string(), std::string(),
                              fitInfo.ebNames);
  info.nKnown = !atmeansOpt;
  if (hasDydx) info.continuous = factorEffect ? 0 : 1;
  LinearAmeRM4 lin;
  const bool linR = linearAme && linearAmeRM4(session, fitInfo, dydxVars, est, lin);
  if (linR) {
    // r(xvars) is the columns (s1.do dydx(c) "0b.c 1.c 2.c 3.c", dydx(*) "x1 x2
    // 0b.c 1.c 2.c 3.c") and r(b)'s row is `y1` after a factor dydx().
    info.xvars.clear();
    for (const auto& nm : lin.names) info.xvars += (info.xvars.empty() ? "" : " ") + nm;
    if (lin.factor) info.bRow = "y1";
    info.jacobian = lin.J;
    postMarginsRClassM4(session, est, info, lin.names, lin.b, lin.V, lin.tableRows);
  } else if (factorEffect) {
    postMarginsRClassM4(session, est, info, {}, {}, {});
  } else {
    std::vector<std::vector<double>> crossV;
    if (!linearAme && hasDydx && !hasAt && marginsAmeCrossVM4(session, lm, est, crossV)) {
      ojson withV = raw;
      withV["V"] = crossV;
      postMarginsFromBlockM4(session, est, withV, info);
    } else {
      postMarginsFromBlockM4(session, est, raw, info);
    }
  }
  // Item 4, `post`: published AFTER the dydxVars coeftable filter above, so
  // e(b)/_b[x] resolve from the FILTERED table (the margin rows the user
  // asked for), not from every row cobra returned. Also after
  // lastMargins/lastMarginsDydx (just above), so marginsplot keeps working.
  if (postOpt) {
    postEstimationBlockM4(session, est, "margins", lm.depvar, lm.dataPath);
    if (linR) {
      // e(b)/e(V) are r(b)/r(V) (BUG-1237, StataBE `margins, dydx(c) post`:
      // e(b) `0b.c 1.c 2.c 3.c`, el(e(b),1,1) 0, row `y1`).
      eval::EvalMatrix eb, eV;
      eb.rownames = {"y1"};
      eb.colnames = lin.names;
      eb.data.push_back(lin.b);
      eV.rownames = lin.names;
      eV.colnames = lin.names;
      eV.data = lin.V;
      for (auto* m : {&eb, &eV}) {
        const std::string nm = m == &eb ? "b" : "V";
        bool replaced = false;
        for (auto& p : session.eval.eMatrices)
          if (p.first == nm) {
            p.second = *m;
            replaced = true;
          }
        if (!replaced) session.eval.eMatrices.emplace_back(nm, *m);
      }
    }
  }
  return est;
}

// ===========================================================================
// predict

namespace {

std::string nextPathEst(const std::string& workdir, const std::string& prefix) {
  return mintScratchPath(workdir, prefix);  // BUG-389: one body, registered at mint
}

// BUG-516: does every column the cached model READS still hold, in the LIVE
// frame, exactly what it held in the estimation-time frame, row for row?
//
// Only the absorbed-FE family asks. The engine's by-name prediction path
// (`--fit-data`) refuses `--fe` -- an absorbed effect cannot be evaluated on a
// row the fit never saw -- so once the head has moved, the one way left to
// predict on the live frame is to fit THERE. That is the fitted model if and
// only if the answer here is yes, and the answer is MEASURED, never assumed: a
// `replace x = ...`, a `drop if`, a `sort` between the fit and the predict each
// leave a frame that refits to different coefficients at rc 0. The ordinary
// history -- an earlier `predict`, a `generate` (w2_smalls.log section C is
// exactly that) -- only ADDS columns, and passes.
//
// The column set is "every identifier in y/x/fe that the estimation-time frame
// confirms is a column" -- column_fetch.h's predicate: never miss a real column.
// An operator prefix (`i`, `c`, `L2`) that happens to also be a column is
// compared too, which can only refuse more. A term that names NO known column
// (an abbreviation, a wildcard) cannot be verified, and refuses.
bool fitColumnsUnchangedM4(SessionState& session, const LastModel& lm, std::string& why,
                           const std::vector<std::string>& extraTerms = {}) {
  Datastore* live = nullptr;
  std::unique_ptr<Datastore> fit;
  try {
    live = session.datastore();
    fit = std::make_unique<Datastore>(lm.dataPath);
  } catch (const std::exception& e) {
    why = std::string("the estimation-time data could not be read back (") + e.what() + ")";
    return false;
  } catch (...) {
    why = "the estimation-time data could not be read back";
    return false;
  }
  if (live == nullptr) {
    why = "no dataset is in memory";
    return false;
  }
  if (live->nRows() != fit->nRows()) {
    why = "the number of observations changed (" + std::to_string(fit->nRows()) +
          " at the fit, " + std::to_string(live->nRows()) + " now)";
    return false;
  }
  std::set<std::string> cols;
  std::vector<std::string> terms = lm.x;
  terms.push_back(lm.y);
  terms.insert(terms.end(), lm.fe.begin(), lm.fe.end());
  terms.insert(terms.end(), extraTerms.begin(), extraTerms.end());
  for (const std::string& term : terms) {
    if (term.empty()) continue;
    bool any = false;
    for (size_t i = 0; i < term.size();) {
      const unsigned char c = static_cast<unsigned char>(term[i]);
      if (!std::isalpha(c) && c != '_') {
        ++i;
        continue;
      }
      size_t j = i + 1;
      while (j < term.size() &&
             (std::isalnum(static_cast<unsigned char>(term[j])) || term[j] == '_')) {
        ++j;
      }
      const std::string tok = term.substr(i, j - i);
      if (fit->has(tok)) {
        cols.insert(tok);
        any = true;
      }
      i = j;
    }
    if (!any) {
      why = "`" + term + "` names no variable of the estimation-time data";
      return false;
    }
  }
  // FU-768: a restricted fit refits under its `if` on the live frame, so every
  // column that expression reads is compared too (a name that is no column -- a
  // function, `_n` -- is not data and is skipped).
  for (size_t i = 0; i < lm.fitKeepif.size();) {
    const unsigned char c = static_cast<unsigned char>(lm.fitKeepif[i]);
    if (!std::isalpha(c) && c != '_') {
      ++i;
      continue;
    }
    size_t j = i + 1;
    while (j < lm.fitKeepif.size() &&
           (std::isalnum(static_cast<unsigned char>(lm.fitKeepif[j])) || lm.fitKeepif[j] == '_')) {
      ++j;
    }
    const std::string tok = lm.fitKeepif.substr(i, j - i);
    if (fit->has(tok)) cols.insert(tok);
    i = j;
  }
  const size_t n = static_cast<size_t>(fit->nRows());
  for (const std::string& name : cols) {
    if (!live->has(name)) {
      // A string condition's row mask (piet/datastore.h kQualifierColumnPrefix)
      // lives on the fit's frame only.
      why = isQualifierColumn(name)
                ? std::string("the fit's string `if` condition is not on the data in memory")
                : "variable " + name + " is no longer in the data";
      return false;
    }
    const cecon::Column& a = fit->col(name);
    const cecon::Column& b = live->col(name);
    bool same = a.dtype == b.dtype;
    if (same && a.dtype == cecon::DType::F64) {
      const double* pa = a.f64();
      const double* pb = b.f64();
      same = pa != nullptr && pb != nullptr;
      // Missing == missing: NaN never compares equal to itself, and a column that
      // was only ever carried forward must not read as "changed".
      for (size_t r = 0; same && r < n; ++r) {
        same = pa[r] == pb[r] || (std::isnan(pa[r]) && std::isnan(pb[r]));
      }
    } else if (same) {
      const int32_t* ca = a.codes();
      const int32_t* cb = b.codes();
      same = ca != nullptr && cb != nullptr && a.levels == b.levels;
      for (size_t r = 0; same && r < n; ++r) same = ca[r] == cb[r];
    }
    if (!same) {
      why = "variable " + name + " changed";
      return false;
    }
  }
  return true;
}

// ===========================================================================
// W2B predict floors (BUG-596). The statistics `predict` answers after the four
// least-squares commands the AER corpus leans on, per their own predict
// programs as StataBE answered them (tests/eval/golden/aer_w2/w2b_estim.log,
// the predict_* arms; aer_b4/predict-e-xbd.log for the reghdfe `resid` rule,
// the xtreg,fe "not allowed" pair and the estimation-sample masking):
//
//   regress   xb residuals score stdp stdf stdr rstandard rstudent leverage|hat
//             cooksd (`e`, `d`: r(198) "option e not allowed")
//   areg      xb stdp dresiduals, and on e(sample) only: xbd d residuals score
//   xtreg,fe  xb stdp ue residuals (= ue), and on e(sample) only: xbu u e
//             (`xbd`, `d`: r(198))
//   reghdfe   xb stdp, and on e(sample) only: xbd d residuals; xbd/d need the
//             fit to have kept its residuals (`resid`), else r(9) (`e`: r(198))
//
// Definitions ([R] regress postestimation and [R] areg postestimation, Methods
// and formulas; [XT] xtreg postestimation): xb = x b INCLUDING _cons. With the
// within residual e = y - xb - d, the absorbed component is d = y - xb - e over
// e(sample) and xbd = y - e. stdp = sqrt(x V x'). Under vce(ols) h = x V x'/s^2,
// stdr = s sqrt(1-h), stdf = s sqrt(1+h), rstandard = e/stdr and rstudent =
// e/(s_(j) sqrt(1-h)), s_(j)^2 = (s^2 df_r - e^2/(1-h))/(df_r - 1), and cooksd =
// e^2 h / (k s^2 (1-h)^2) with k the number of estimated coefficients (BUG-925:
// w2b_estim.log predict_reg_cooksd; w2aa_predict_type_restricted.log a_cooksd,
// a_if_cooksd -- after a restricted fit every row with y and x, as hat).
//
// Measured on main before this (frozen binary, w2b_estim.do lines 1-17 + 50-108):
// every statistic above except xb/residuals refused, `r` refused, and xb after
// areg / xtreg,fe / reghdfe wrote the WITHIN prediction x~b -- r(sum) 1.8e-15,
// r(sd) 6.196 where the oracle's column sums to 760.4 with sd 6.324 -- at rc 0.
// That last one is the silent wrong answer this block exists for: an absorbed-FE
// fit's `predict p` is now x b + _cons, evaluated here, never the engine's
// demeaned index.
//
// Nothing below is filled that was not computed for THIS model: the design is
// read off e(b)'s own names (a name this reader cannot evaluate refuses), the
// estimation sample is the one the engine's refit reports (its within residual
// is missing off the sample, and the count must equal e(N)), and each quantity
// is checked against an independent one the engine produced before it is
// written (see computePredictNativeM4).

enum class PredictFamilyM4 { Other, Regress, Areg, XtregFe, Reghdfe };

PredictFamilyM4 predictFamilyM4(const LastModel& lm) {
  if (lm.fe.empty()) return lm.stataCmd == "regress" ? PredictFamilyM4::Regress : PredictFamilyM4::Other;
  if (lm.stataCmd == "areg") return PredictFamilyM4::Areg;
  if (lm.stataCmd == "reghdfe") return PredictFamilyM4::Reghdfe;
  // `xtreg, fe` records stataCmd "xtreg" (its e(cmd)); the N_g it copies into
  // the cached reply (runXtreg's fixed-effects arm, BUG-538's e() half) is the
  // mark that tells it from xtreg, re -- nothing else in the tree writes that key.
  if ((lm.stataCmd.empty() || lm.stataCmd == "xtreg") && lm.cobraCmd == "feols" && lm.result.has_value() &&
      lm.result->is_object() && lm.result->contains("N_g")) {
    return PredictFamilyM4::XtregFe;
  }
  return PredictFamilyM4::Other;
}

std::string predictFamilyCmdM4(PredictFamilyM4 f, const LastModel& lm) {
  return f == PredictFamilyM4::XtregFe ? std::string("xtreg, fe") : lm.stataCmd;
}

// A typed spelling -> the statistic it names. The dispatcher's option vocabulary
// (dispatcher.cpp kVocabPredict) already canonicalizes the unique prefixes
// (`rsta`, `rstu`, `lev`, `sc`, `dr`) and the `re`/`res`/`resi` aliases; `r` is a
// prefix of four names there and arrives as typed. Each row is an oracle spelling
// (w2b_estim.do lines 52, 85, 96, 107).
std::string predictStatisticM4(const std::string& k) {
  if (k == "r" || k == "re" || k == "res" || k == "resi" || k == "resid" || k == "residuals") {
    return "residuals";
  }
  if (k == "sc" || k == "score") return "score";
  if (k == "rsta" || k == "rstandard") return "rstandard";
  if (k == "rstu" || k == "rstudent") return "rstudent";
  if (k == "lev" || k == "leverage" || k == "hat") return "hat";
  if (k == "dr" || k == "dresid" || k == "dresiduals") return "dresiduals";
  return k;
}

// BUG-1257: a typed spelling -> the statistic predict reads after logit, probit
// or poisson, "" = Stata's "option <k> not allowed", r(198). Each spelling is
// StataBE 19.5's (2026-10-29): the minimum abbreviations of logit_p/probit_p/
// poisso_p, `i`..`index` the linear index there as everywhere (after poisson too:
// `i` is xb, not ir), `p` pr, `n` number after logit and n after poisson, `dx`
// dx2; `x`, `st`, `std`, `ru`, `as`, `e` and every other statistic refused. The
// dispatcher's prefix rule (kVocabPredict) may already have spelled `h` as `hat`,
// `de` as `deviance`, `sc` as `score`, `res` as `residuals`.
std::string mlPredictStatisticM4(const std::string& cmd, const std::string& k) {
  auto abbrev = [&k](const char* full, size_t minLen) {
    const std::string f(full);
    return k.size() >= minLen && k.size() <= f.size() && f.compare(0, k.size(), k) == 0;
  };
  if (k == "xb" || abbrev("index", 1)) return "xb";
  if (k == "stdp") return "stdp";
  if (abbrev("scores", 2)) return "score";
  if (abbrev("nooffset", 5)) return "nooffset";
  if (cmd == "poisson") {
    if (k == "n") return "n";
    if (k == "ir") return "ir";
    return "";
  }
  if (abbrev("pr", 1)) return "pr";
  if (abbrev("deviance", 2)) return "deviance";
  if (abbrev("rules", 4)) return "rules";
  if (k == "asif") return "asif";
  if (cmd != "logit") return "";
  if (abbrev("number", 1)) return "number";
  if (abbrev("residuals", 1)) return "residuals";
  if (abbrev("rstandard", 2)) return "rstandard";
  if (abbrev("dx2", 2)) return "dx2";
  if (abbrev("ddeviance", 2)) return "ddeviance";
  if (abbrev("dbeta", 2)) return "dbeta";
  if (abbrev("hat", 1)) return "hat";
  return "";
}

bool predictServesM4(PredictFamilyM4 f, const std::string& s) {
  static const std::set<std::string> kRegress = {"xb",   "residuals", "score",    "stdp",    "stdf",
                                                 "stdr", "rstandard", "rstudent", "hat",
                                                 "cooksd"};
  static const std::set<std::string> kAreg = {"xb", "stdp", "dresiduals", "xbd", "d", "residuals",
                                              "score"};
  static const std::set<std::string> kXtregFe = {"xb", "stdp", "ue", "xbu", "u", "e", "residuals"};
  // BUG-1259: reghdfe's dresiduals (y - xb) and score (the residual), as areg's
  // (StataBE 19.5 scratch-laneA/agents/N2/e12.do: dresiduals[8] -2.0888238492587301,
  // score[8] -.89046608668893679 after `reghdfe ... if t>=2, absorb(id) resid`).
  static const std::set<std::string> kReghdfe = {"xb",        "stdp",       "xbd",  "d",
                                                 "residuals", "dresiduals", "score"};
  switch (f) {
    case PredictFamilyM4::Regress: return kRegress.count(s) > 0;
    case PredictFamilyM4::Areg: return kAreg.count(s) > 0;
    case PredictFamilyM4::XtregFe: return kXtregFe.count(s) > 0;
    case PredictFamilyM4::Reghdfe: return kReghdfe.count(s) > 0;
    default: return false;
  }
}

// FU-768: after a RESTRICTED absorbed fit (`areg|xtreg|reghdfe ... if exp`, no
// time-series operator) the statistics w2aa_predict_type_restricted.log pins: xb
// over every row with x (b_if_xb, c_xt_if_xb, c_hdfe_if_xb), ue over every row
// with y and x (c_xt_if_ue), and on e(sample) only residuals/d/xbd (b_if_*,
// c_hdfe_if_*) and e/u/xbu (c_xt_if_*). BUG-1259 measured the rest the same way
// (StataBE 19.5 scratch-laneA/agents/N2/e12.do, `... if t>=2`): stdp, dresiduals
// and score after areg/reghdfe, residuals (= ue, every row with y and x: 60) and
// stdp after xtreg, fe -- each the unrestricted fit's statistic over the
// restricted fit's e(sample). Every statistic of the family is served.
bool predictServesRestrictedM4(PredictFamilyM4 f, const std::string& s) {
  return predictServesM4(f, s);
}

// Stata's own refusal, r(198) "option X not allowed": w2b_estim.log predict_reg_e /
// predict_reg_d; predict-e-xbd.log B_stat_xbd / B_stat_d (xtreg,fe) and P_res_e
// (reghdfe).
bool predictNotAllowedM4(PredictFamilyM4 f, const std::string& s) {
  switch (f) {
    case PredictFamilyM4::Regress: return s == "e" || s == "d";
    case PredictFamilyM4::XtregFe: return s == "xbd" || s == "d";
    case PredictFamilyM4::Reghdfe: return s == "e";
    default: return false;
  }
}

// The statistics the engine's own predict already writes as Stata defines them,
// so they keep the historical argv byte for byte: `--xb` after regress, and the
// residual (`--resid`, the within residual under --fe, missing off e(sample)).
// "" = computed here.
std::string predictEngineServedM4(PredictFamilyM4 f, const std::string& s) {
  switch (f) {
    case PredictFamilyM4::Regress:
      if (s == "xb") return "xb";
      if (s == "residuals" || s == "score") return "resid";
      return "";
    case PredictFamilyM4::Areg: return (s == "residuals" || s == "score") ? "resid" : "";
    case PredictFamilyM4::XtregFe: return s == "e" ? "resid" : "";
    case PredictFamilyM4::Reghdfe: return (s == "residuals" || s == "score") ? "resid" : "";
    default: return "";
  }
}

// The one statistic the options name, or the refusal block.
std::optional<ojson> resolvePredictStatM4(PredictFamilyM4 f, const LastModel& lm,
                                          const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                          std::string& stat) {
  stat.clear();
  for (const auto& [k, v] : opts) {
    if (k == "quietly" || k == "noisily") continue;
    const std::string s = predictStatisticM4(k);
    const bool bare = v.isBool && v.boolValue;
    if (bare && predictNotAllowedM4(f, s)) {
      return engineErrorBlockM4("option " + k + " not allowed", "syntax_error");
    }
    // BUG-1260: regress's e(a,b), E(y | a < y < b) (regre_p2.ado CalcE; StataBE
    // 19.5 `predict double OLS_ec, e(0,.)` rc 0), carried as "e(a,b)".
    if (f == PredictFamilyM4::Regress && k == "e" && !v.isBool) {
      if (!stat.empty()) return engineErrorBlockM4("predict: only one statistic may be specified", "syntax_error");
      stat = "e(" + v.strValue + ")";
      continue;
    }
    if (!bare || !predictServesM4(f, s)) {
      return engineErrorBlockM4("predict " + k + ": not supported after `" + predictFamilyCmdM4(f, lm) +
                                    "` in this version of PIET. Nothing was written to your data.",
                                "unsupported_option");
    }
    if (!stat.empty() && stat != s) {
      return engineErrorBlockM4("predict: only one statistic may be specified", "syntax_error");
    }
    stat = s;
  }
  if (stat.empty()) stat = "xb";  // Stata's default after all four
  return std::nullopt;
}

// One e(b) column of the design, as a product of live-frame columns. `level` < 0
// is the column's value (a plain or `c.` term; an `o.` omitted term, whose
// coefficient is 0); level >= 0 is the indicator `var == level` of a factor term
// (`2.t`, the base `1b.t`), missing where var is. `constant` is _cons.
struct DesignColM4 {
  bool constant = false;
  std::vector<std::pair<const double*, long long>> parts;
};

// Read e(b)'s names as design columns of the live frame. Accepts exactly the
// shapes above joined by `#`; anything else -- a time-series operator (`L.x`),
// a string column, a name that is not a column -- is not guessed at: `why` names
// it and the caller refuses. `tsCols` (BUG-1259): a time-series term's values,
// one per row of the live frame, as the engine generated them
// (materializeTsColumnsM4); such a segment reads them.
bool buildDesignM4(const Datastore& ds, const std::vector<std::string>& names,
                   std::vector<DesignColM4>& cols, std::string& why,
                   const std::map<std::string, std::vector<double>>* tsCols = nullptr) {
  cols.clear();
  for (const std::string& nm : names) {
    DesignColM4 c;
    if (nm == "_cons" || nm == "(intercept)") {
      c.constant = true;
      cols.push_back(std::move(c));
      continue;
    }
    size_t pos = 0;
    while (true) {
      const size_t hash = nm.find('#', pos);
      const std::string seg =
          nm.substr(pos, hash == std::string::npos ? std::string::npos : hash - pos);
      if (tsCols != nullptr) {
        const auto ts = tsCols->find(seg);
        if (ts != tsCols->end() && ts->second.size() == static_cast<size_t>(ds.nRows())) {
          c.parts.emplace_back(ts->second.data(), -1);
          if (hash == std::string::npos) break;
          pos = hash + 1;
          continue;
        }
      }
      std::string var = seg;
      long long level = -1;
      const size_t dot = seg.find('.');
      if (dot != std::string::npos) {
        const std::string op = seg.substr(0, dot);
        var = seg.substr(dot + 1);
        if (op != "c" && op != "o") {
          size_t d = 0;
          while (d < op.size() && std::isdigit(static_cast<unsigned char>(op[d]))) ++d;
          const std::string suffix = op.substr(d);
          if (d == 0 || d > 15 ||
              !(suffix.empty() || suffix == "b" || suffix == "o" || suffix == "bn" || suffix == "n")) {
            why = "the coefficient `" + nm + "` is not a term this version of PIET can evaluate";
            return false;
          }
          level = std::stoll(op.substr(0, d));
        }
      }
      if (var.empty() || var.find('.') != std::string::npos || !ds.has(var) ||
          ds.col(var).dtype != cecon::DType::F64 || ds.col(var).f64() == nullptr) {
        why = "the coefficient `" + nm + "` does not name a numeric variable of the data in memory";
        return false;
      }
      c.parts.emplace_back(ds.col(var).f64(), level);
      if (hash == std::string::npos) break;
      pos = hash + 1;
    }
    cols.push_back(std::move(c));
  }
  return true;
}

double designValueM4(const DesignColM4& c, size_t r) {
  if (c.constant) return 1.0;
  double v = 1.0;
  for (const auto& [p, level] : c.parts) {
    const double x = p[r];
    if (std::isnan(x)) return std::numeric_limits<double>::quiet_NaN();
    v *= (level < 0) ? x : (x == static_cast<double>(level) ? 1.0 : 0.0);
  }
  return v;
}

// BUG-1259: the time-series segments of e(b)'s names (`L.x1`, `D.x2`; a factor
// level, `c.` and `o.` are the design reader's own) over every row of the live
// frame, generated by the engine under the session's tsset/xtset -- the
// expression path `generate z = L.x1` takes (cobra eval --gen) -- so a lag reads
// across the fit's `if` exactly as the fit's design did. The frame comes back row
// for row, or nothing is returned.
bool materializeTsColumnsM4(SessionState& session, const std::vector<std::string>& names,
                            std::map<std::string, std::vector<double>>& out, std::string& why) {
  std::vector<std::string> segs;
  for (const std::string& nm : names) {
    size_t pos = 0;
    while (true) {
      const size_t hash = nm.find('#', pos);
      const std::string seg = nm.substr(pos, hash == std::string::npos ? std::string::npos : hash - pos);
      const size_t dot = seg.find('.');
      if (dot != std::string::npos) {
        const std::string op = seg.substr(0, dot);
        const bool level = !op.empty() && std::isdigit(static_cast<unsigned char>(op[0]));
        if (!level && op != "c" && op != "o" && std::find(segs.begin(), segs.end(), seg) == segs.end())
          segs.push_back(seg);
      }
      if (hash == std::string::npos) break;
      pos = hash + 1;
    }
  }
  if (segs.empty()) return true;
  if (!session.currentHbin().has_value()) {
    why = "no dataset is in memory";
    return false;
  }
  const std::string head = *session.currentHbin();
  const std::string scratch = nextPathEst(session.workdir(), "predict_ts");
  std::vector<std::string> argv = {"eval", "--data", head};
  std::string exprs;
  for (size_t i = 0; i < segs.size(); ++i) {
    const std::string gen = "__piet_ts" + std::to_string(i) + "__=" + segs[i];
    argv.insert(argv.end(), {"--gen", gen});
    exprs += gen + " ";
  }
  argv.insert(argv.end(), {"--out", scratch});
  try {
    (void)runCobraJsonM4(translate::injectTsContextForExpr(argv, exprs, session));
    Datastore gen(scratch);
    Datastore* live = session.datastore();
    if (live == nullptr || gen.nRows() != live->nRows()) throw std::runtime_error("rows changed");
    for (size_t i = 0; i < segs.size(); ++i) {
      const std::string col = "__piet_ts" + std::to_string(i) + "__";
      const std::string var = segs[i].substr(segs[i].find('.') + 1);
      if (!gen.has(col) || gen.col(col).f64() == nullptr || !gen.has(var) || !live->has(var) ||
          gen.col(var).f64() == nullptr || live->col(var).f64() == nullptr)
        throw std::runtime_error("the generated columns are not readable");
      // The frame must come back in the live frame's row order.
      const double* a = gen.col(var).f64();
      const double* b = live->col(var).f64();
      for (int64_t r = 0; r < live->nRows(); ++r)
        if (!(a[r] == b[r] || (std::isnan(a[r]) && std::isnan(b[r]))))
          throw std::runtime_error("the generated frame is not the live frame row for row");
      const double* p = gen.col(col).f64();
      out[segs[i]].assign(p, p + gen.nRows());
    }
  } catch (const CobraError& ce) {
    cecon::native_unlink(scratch);
    why = "the time-series terms could not be evaluated (" + cobraErrorStrM4(ce) + ")";
    return false;
  } catch (const std::exception& e) {
    cecon::native_unlink(scratch);
    why = std::string("the time-series terms could not be evaluated (") + e.what() + ")";
    return false;
  }
  cecon::native_unlink(scratch);
  return true;
}

// e(V) over `names` (row-major), from the engine's own lincom: Var(b_j) and
// Var(b_j + b_l) in ONE call, V_jl = (Var(b_j + b_l) - V_jj - V_ll) / 2. It is the
// covariance every lincom/test answer already rests on (the refit forwards the
// fitted --vcov/--cluster, BS-098) -- and the only full one reachable: the fit's
// reply carries standard errors, not V, and `matrix V = e(V)` synthesizes a
// diagonal. The FULL-specification call (lastModelBaseArgv), deliberately not
// runPostEstimationM4: minting a §31 handle goes through setLastModel, which
// erases e(resid), and a later `predict, xbd` after `reghdfe ..., resid` would
// then refuse r(9) on a fit that kept its residuals. Its b and its standard errors
// must reproduce the fit's, or the V it carries belongs to some other fit (a refit
// on another sample, another VCE) and nothing is returned.
bool lincomVcovM4(const SessionState& session, const LastModel& lm, const std::vector<std::string>& names,
                  const std::vector<double>& b, const std::vector<double>& seFit,
                  std::vector<double>& V, std::string& why) {
  const size_t k = names.size();
  // k + k(k-1)/2 expressions ride on one argv; past this the list outgrows it.
  constexpr size_t kMaxTerms = 160;
  if (k == 0 || k > kMaxTerms) {
    why = "the model has " + std::to_string(k) + " coefficients (this version of PIET computes "
          "the prediction's variance for at most " + std::to_string(kMaxTerms) + ")";
    return false;
  }
  std::vector<std::string> spell(k);
  for (size_t j = 0; j < k; ++j) {
    spell[j] = names[j] == "(intercept)" ? std::string("_cons") : names[j];
    if (spell[j].find_first_of("+-* \t") != std::string::npos) {
      why = "the coefficient `" + names[j] + "` cannot be addressed by the engine's lincom";
      return false;
    }
  }
  std::vector<std::string> argv = lastModelBaseArgv("lincom", lm);
  // BUG-1257: after logit/probit/poisson the refit names its estimator, as
  // lincom's own does (BUG-203).
  if (postestGlmCmdsM4().count(lm.cobraCmd)) argv.insert(argv.end(), {"--model", lm.cobraCmd});
  for (size_t j = 0; j < k; ++j) argv.insert(argv.end(), {"--exp", spell[j]});
  for (size_t j = 0; j < k; ++j)
    for (size_t l = j + 1; l < k; ++l) argv.insert(argv.end(), {"--exp", spell[j] + " + " + spell[l]});
  argv.push_back("--tidy");
  // BUG-1259 (BUG-369's shape): an `L.x` regressor refits under the fit's tsset context.
  {
    std::vector<std::string> tsTokens = lm.x;
    tsTokens.insert(tsTokens.end(), lm.fe.begin(), lm.fe.end());
    if (translate::hasTsToken(tsTokens)) argv = translate::injectTsContext(argv, session);
  }
  const nlohmann::json raw = runCobraJsonM4(argv);  // CobraError -> the caller's catch
  const size_t want = k + k * (k - 1) / 2;
  if (!raw.is_object() || !raw.contains("coefs") || !raw["coefs"].is_array() ||
      raw["coefs"].size() != want) {
    why = "the engine's lincom returned an unexpected reply";
    return false;
  }
  const auto& co = raw["coefs"];
  auto num = [&co](size_t i, const char* key) {
    return (co[i].is_object() && co[i].contains(key) && co[i][key].is_number())
               ? co[i][key].get<double>()
               : std::numeric_limits<double>::quiet_NaN();
  };
  V.assign(k * k, 0.0);
  for (size_t j = 0; j < k; ++j) {
    const double bj = num(j, "b");
    const double se = num(j, "se");
    if (!std::isfinite(bj) || !std::isfinite(se) || !std::isfinite(seFit[j]) ||
        std::fabs(bj - b[j]) > 1e-9 * std::max({1.0, std::fabs(b[j]), se}) ||
        std::fabs(se - seFit[j]) > 1e-9 * seFit[j]) {
      why = "the engine's post-estimation refit did not reproduce the fitted coefficient and "
            "standard error of `" + names[j] + "`";
      return false;
    }
    V[j * k + j] = se * se;
  }
  size_t idx = k;
  for (size_t j = 0; j < k; ++j) {
    for (size_t l = j + 1; l < k; ++l, ++idx) {
      const double se = num(idx, "se");
      if (!std::isfinite(se)) {
        why = "the engine's lincom returned no standard error for `" + spell[j] + " + " + spell[l] + "`";
        return false;
      }
      const double c = (se * se - V[j * k + j] - V[l * k + l]) / 2.0;
      V[j * k + l] = c;
      V[l * k + j] = c;
    }
  }
  return true;
}

double jsonNumM4(const ojson& o, const char* key) {
  return (o.is_object() && o.contains(key) && o[key].is_number())
             ? o[key].get<double>()
             : std::numeric_limits<double>::quiet_NaN();
}

// Compute statistic `stat` (one the engine does not write itself) over every row
// of the live frame. `engineArgs` is runPredict's replay argv up to and including
// --fe (no --xb/--resid/--out): it is re-run here as a ONE-SHOT engine call into a
// scratch file whose two columns this function reads -- the engine's xb (after
// regress, the exact linear predictor over the rows the engine evaluates) and its
// residual (under --fe the within residual, missing off e(sample)). One-shot, so
// the output is a file on disk whatever plane the session runs on; every input
// path must therefore be on disk too, and is checked.
//
// Cross-checks before anything is returned (each failure refuses, naming why):
//   regress  the xb evaluated here equals the engine's on every row it evaluates;
//   FE       the engine's refit sample has e(N) rows, xb and y exist on all of
//            them, and sum over e(sample) of d is 0 (it is, exactly, when _cons =
//            ybar - xbar b, the engine's own convention -- GAP B in feols.cpp);
//   stdp     lincom's b reproduces e(b) (lincomVcovM4); under FE additionally
//            se(_cons)^2 == s^2/N + xbar V xbar', the engine's own extended-inverse
//            identity, which also proves the sample means used below.
std::optional<ojson> computePredictNativeM4(SessionState& session, const LastModel& lm,
                                            PredictFamilyM4 fam, const std::string& stat,
                                            std::vector<std::string> engineArgs,
                                            std::vector<double>& vals) {
  const std::string cmd = predictFamilyCmdM4(fam, lm);
  auto refuse = [&](const std::string& why) {
    return engineErrorBlockM4("predict, " + stat + " after `" + cmd +
                                  "` is not supported here in this version of PIET: " + why +
                                  ". Refusing rather than writing a number that was not computed "
                                  "for this model. Nothing was written to your data.",
                              "unsupported_option");
  };
  const bool fe = fam != PredictFamilyM4::Regress;
  const bool iid = lm.vcov == "iid" && lm.cluster.empty();
  // An absorbed-effects fit whose time-series regressors read rows outside its
  // `if` (the restriction rides as --fit-keepif, a row mask after the operators).
  const bool tsMaskFe =
      fe && std::find(engineArgs.begin(), engineArgs.end(), "--fit-keepif") != engineArgs.end() &&
      postestTsKeepifRefusalM4("predict", lm).has_value();

  // Stata: rstandard/rstudent/leverage/stdf/stdr are "not available if any vce()
  // other than vce(ols) was specified" ([R] regress postestimation). Under an
  // absorbed FE the prediction's variance needs _cons's covariances, which only
  // the iid algebra reaches from here (the engine's lincom cannot address the
  // reconstructed _cons row).
  if (!iid && (stat == "hat" || stat == "stdr" || stat == "stdf" || stat == "rstandard" ||
               stat == "rstudent" || stat == "cooksd")) {
    return engineErrorBlockM4("predict, " + stat + " is not available after a `regress` with a "
                              "robust or clustered VCE; Stata computes it only under vce(ols)",
                              "unsupported_option");
  }
  if (fe && stat == "stdp" && !iid) {
    return refuse("the standard error of the prediction after absorbed fixed effects is computed "
                  "only under the default (iid) VCE");
  }
  // BUG-1260: regress's e(a,b) = E(xb + u | a < xb + u < b) with u ~ N(0, e(rmse)^2)
  // (regre_p2.ado CalcE); each bound a number or `.` (unbounded). A variable or
  // expression bound is not read here.
  const bool eStat = stat.rfind("e(", 0) == 0;
  double eLo = std::numeric_limits<double>::quiet_NaN(), eHi = eLo;
  if (eStat) {
    const std::string in = stat.substr(2, stat.size() - 3);
    const size_t comma = in.find(',');
    if (comma == std::string::npos || in.find(',', comma + 1) != std::string::npos)
      return engineErrorBlockM4("option e() incorrectly specified", "syntax_error");
    auto bound = [](std::string t, double& out) {
      t = trimM4(t);
      if (t == ".") return true;
      char* end = nullptr;
      const double v = std::strtod(t.c_str(), &end);
      if (t.empty() || end == nullptr || *end != '\0' || !std::isfinite(v)) return false;
      out = v;
      return true;
    };
    if (!bound(in.substr(0, comma), eLo) || !bound(in.substr(comma + 1), eHi))
      return refuse("a bound of e(a,b) that is not a number or `.`");
    if (!std::isnan(eLo) && !std::isnan(eHi) && eLo > eHi)
      return engineErrorBlockM4("option e(a,b) incorrectly specified; b must be >= a", "syntax_error");
  }

  // reghdfe answers xbd/d only when the fit kept its residuals (reghdfe_p: r(9)),
  // and reads them from that variable (predict-e-xbd.log R_drop_xbd: dropped ->
  // r(111)). w2b_estim.log predict_reghdfe_xbd_noresid is the r(9) arm.
  if (fam == PredictFamilyM4::Reghdfe && (stat == "xbd" || stat == "d")) {
    const std::string* rv = session.eval.eStrings.find("resid");
    if (rv == nullptr || rv->empty()) {
      return ojson(eval::errorBlock(
          "engine_error", "you must add the resid option to reghdfe before running this prediction",
          std::nullopt, std::nullopt, std::nullopt, "r(9)"));
    }
    Datastore* live0 = nullptr;
    try {
      live0 = session.datastore();
    } catch (const std::exception& e) {
      return refuse(e.what());
    }
    if (live0 == nullptr || !live0->has(*rv)) {
      return ojson(eval::engineError("variable " + *rv + " not found"));
    }
  }

  // Coefficients: e(b) as the engine reported it for this fit.
  std::vector<std::string> names;
  std::vector<double> b, se;
  if (!lm.result.has_value() || !lm.result->is_object() || !lm.result->contains("coeftable") ||
      !(*lm.result)["coeftable"].is_array()) {
    return refuse("the fitted coefficients are not available");
  }
  for (const auto& row : (*lm.result)["coeftable"]) {
    if (!row.is_object() || !row.contains("name") || !row["name"].is_string()) {
      return refuse("the fitted coefficients are not available");
    }
    names.push_back(row["name"].get<std::string>());
    b.push_back(jsonNumM4(row, "b"));
    se.push_back(jsonNumM4(row, "se"));
    if (!std::isfinite(b.back())) return refuse("the coefficient `" + names.back() + "` is missing");
  }
  size_t consIdx = names.size();
  for (size_t j = 0; j < names.size(); ++j)
    if (names[j] == "_cons" || names[j] == "(intercept)") consIdx = j;
  if (fe && consIdx == names.size()) {
    return refuse("the fit reports no _cons, and xb after absorbed effects is x b + _cons");
  }
  const double sigma2 = [&] {
    const double s2 = jsonNumM4(*lm.result, "sigma2");
    if (std::isfinite(s2)) return s2;
    const double rm = jsonNumM4(*lm.result, "rmse");
    return rm * rm;
  }();
  const double nObs = jsonNumM4(*lm.result, "n_obs");
  const double dfR = jsonNumM4(*lm.result, "df_r");
  // cooksd's k: the coefficients the fit estimated, e(N) - e(df_r) (regress only).
  const double kEst = nObs - dfR;
  if (stat == "cooksd" && !(std::isfinite(kEst) && kEst > 0.0)) {
    return refuse("the fit reports no residual degrees of freedom");
  }

  // The covariance, before any data is read (lincom never touches the session).
  const bool needV = stat == "stdp" || stat == "hat" || stat == "stdr" || stat == "stdf" ||
                     stat == "rstandard" || stat == "rstudent" || stat == "cooksd";
  std::vector<std::string> vNames;
  std::vector<double> vB, vSe, V;
  if (needV) {
    for (size_t j = 0; j < names.size(); ++j) {
      if (fe && j == consIdx) continue;  // the reconstructed _cons: identity below
      vNames.push_back(names[j]);
      vB.push_back(b[j]);
      vSe.push_back(se[j]);
    }
    std::string why;
    try {
      if (!lincomVcovM4(session, lm, vNames, vB, vSe, V, why)) return refuse(why);
    } catch (const CobraError& ce) {
      return refuse(cobraErrorStrM4(ce));
    }
  }
  if ((stat == "hat" || stat == "stdr" || stat == "stdf" || stat == "rstandard" ||
       stat == "rstudent" || stat == "cooksd") && !(std::isfinite(sigma2) && sigma2 > 0.0)) {
    return refuse("the fit reports no residual variance");
  }

  // The engine's refit, one-shot into a scratch file on disk.
  Datastore* live = nullptr;
  try {
    live = session.datastore();  // materializes a virtual head before anything reads it
  } catch (const std::exception& e) {
    return refuse(e.what());
  }
  if (live == nullptr) return ojson(eval::noDataLoaded());
  for (size_t i = 0; i + 1 < engineArgs.size(); ++i) {
    if ((engineArgs[i] == "--data" || engineArgs[i] == "--fit-data") &&
        !cecon::native_exists(engineArgs[i + 1])) {
      return refuse("the data the model was fitted on are not available on disk");
    }
  }
  const std::string kXb = "__piet_predict_xb__";
  const std::string kE = "__piet_predict_e__";
  const std::string scratch = nextPathEst(session.workdir(), "predict_refit");
  engineArgs.insert(engineArgs.end(), {"--xb", kXb, "--resid", kE, "--out", scratch});
  const CobraRunResult rr = runCobraOneShot(engineArgs);
  if (!rr.ok || !cecon::native_exists(scratch)) {
    cecon::native_unlink(scratch);
    const std::string err = rr.stderr_text.empty() ? rr.error : rr.stderr_text;
    return refuse("the engine's refit failed (" + strutil::trim(err) + ")");
  }
  std::vector<double> engXb, engE;
  try {
    Datastore eng(scratch);
    if (eng.nRows() != live->nRows() || !eng.has(kXb) || !eng.has(kE) ||
        eng.col(kXb).f64() == nullptr || eng.col(kE).f64() == nullptr) {
      throw std::runtime_error("the engine's refit returned an unexpected frame");
    }
    const double* px = eng.col(kXb).f64();
    const double* pe = eng.col(kE).f64();
    engXb.assign(px, px + eng.nRows());
    engE.assign(pe, pe + eng.nRows());
  } catch (const std::exception& e) {
    cecon::native_unlink(scratch);
    return refuse(e.what());
  }
  cecon::native_unlink(scratch);
  // BUG-1259: x V x' after time-series operators under `if` reads the operator
  // columns as the engine generates them over the live frame.
  std::map<std::string, std::vector<double>> tsCols;
  if (tsMaskFe && needV) {
    std::string why;
    if (!materializeTsColumnsM4(session, vNames, tsCols, why)) return refuse(why);
  }

  // The live frame again: nothing above may have kept the pointer current.
  try {
    live = session.datastore();
  } catch (const std::exception& e) {
    return refuse(e.what());
  }
  if (live == nullptr) return ojson(eval::noDataLoaded());
  const size_t n = static_cast<size_t>(live->nRows());
  if (engXb.size() != n) return refuse("the engine's refit returned an unexpected frame");

  const double* y = nullptr;
  if (live->has(lm.y) && live->col(lm.y).dtype == cecon::DType::F64) y = live->col(lm.y).f64();
  const bool needY = fe || stat == "residuals" || stat == "rstandard" || stat == "rstudent" ||
                     stat == "cooksd";
  if (needY && y == nullptr) return refuse("the dependent variable `" + lm.y + "` is not a numeric variable");

  const double kNaN = std::numeric_limits<double>::quiet_NaN();
  std::vector<double> xb(n, kNaN);
  std::vector<DesignColM4> design, vDesign;
  if (tsMaskFe) {
    // FU-759 (w2_esample_tsrefit.log:251-263 T7): e(b) names a time-series term
    // this reader cannot evaluate, so the engine evaluates it over the full
    // series -- cmd_predict's --fe under the --fit-keepif row mask writes
    // x b + _cons, the refit's reconstructed constant. The checks below (the
    // refit's sample is e(N), the absorbed part sums to zero over it) hold that
    // prediction to the fitted model as they hold the design's. BUG-1259: every
    // statistic built from xb, y and the refit's within residual follows (StataBE
    // 19.5 scratch-laneA/agents/N2/e12.do: `xtreg y L.x1 x2 if t>=3, fe` u[9]
    // -2.8045004048291329 on 40 rows, xbu[9] 13.253036355985744; areg d/xbd the
    // same), and stdp reads the operator columns materialized above.
    xb = engXb;
    std::string why;
    if (needV && !buildDesignM4(*live, vNames, vDesign, why, &tsCols)) return refuse(why);
  } else {
    std::string why;
    if (!buildDesignM4(*live, names, design, why)) return refuse(why);
    if (needV && !buildDesignM4(*live, vNames, vDesign, why)) return refuse(why);
    for (size_t r = 0; r < n; ++r) {
      double s = 0.0;
      bool ok = true;
      for (size_t j = 0; j < design.size() && ok; ++j) {
        const double v = designValueM4(design[j], r);
        if (std::isnan(v)) ok = false;
        else s += v * b[j];
      }
      if (ok) xb[r] = s;
    }
  }

  // The within residual and the estimation sample (FE only).
  std::vector<double> d;
  std::vector<char> inSample;
  if (!fe) {
    // After regress the engine evaluates the fitted coefficients itself: the xb
    // built here from e(b)'s names must be its xb, row for row.
    for (size_t r = 0; r < n; ++r) {
      if (std::isnan(engXb[r])) continue;
      if (std::isnan(xb[r]) || std::fabs(xb[r] - engXb[r]) > 1e-9 * (1.0 + std::fabs(engXb[r]))) {
        return refuse("the design read from e(b) does not reproduce the engine's linear prediction");
      }
    }
  } else {
    inSample.assign(n, 0);
    d.assign(n, kNaN);
    size_t count = 0;
    double sumD = 0.0, scale = 0.0;
    for (size_t r = 0; r < n; ++r) {
      if (std::isnan(engE[r])) continue;
      if (y == nullptr || std::isnan(y[r]) || std::isnan(xb[r])) {
        return refuse("an estimation-sample row has no linear prediction");
      }
      inSample[r] = 1;
      ++count;
      d[r] = y[r] - xb[r] - engE[r];
      sumD += d[r];
      scale += std::fabs(y[r] - engE[r]) + std::fabs(xb[r]);
    }
    if (!std::isfinite(nObs) || static_cast<double>(count) != nObs) {
      return refuse("the engine's refit sample (" + std::to_string(count) +
                    " observations) is not the fitted one");
    }
    if (std::fabs(sumD) > 1e-9 * scale) {
      return refuse("the absorbed component does not sum to zero over the estimation sample, so "
                    "the design read from e(b) is not the fitted one");
    }
  }

  // x V x' (stdp; h = x V x'/s^2 under vce(ols)).
  std::vector<double> q;
  if (needV) {
    const size_t k = vDesign.size();
    std::vector<double> xbar(k, 0.0);
    if (fe) {
      // Frisch-Waugh: under iid the engine's extended inverse gives Cov(b, _cons)
      // = -V xbar' and Var(_cons) = s^2/N + xbar V xbar' (feols.cpp GAP B), so
      // x_c V_full x_c' = (x - xbar) V (x - xbar)' + s^2/N.
      size_t cnt = 0;
      for (size_t r = 0; r < n; ++r) {
        if (!inSample[r]) continue;
        ++cnt;
        for (size_t j = 0; j < k; ++j) xbar[j] += designValueM4(vDesign[j], r);
      }
      for (size_t j = 0; j < k; ++j) xbar[j] /= static_cast<double>(cnt);
      double quad = 0.0;
      for (size_t j = 0; j < k; ++j)
        for (size_t l = 0; l < k; ++l) quad += xbar[j] * V[j * k + l] * xbar[l];
      const double want = sigma2 / nObs + quad;
      const double got = se[consIdx] * se[consIdx];
      if (!(std::isfinite(want) && std::isfinite(got)) || std::fabs(want - got) > 1e-8 * std::fabs(got)) {
        return refuse("Var(_cons) does not satisfy the fit's own identity, so the estimation-sample "
                      "means are not the fitted ones");
      }
    }
    q.assign(n, kNaN);
    std::vector<double> x(k);
    for (size_t r = 0; r < n; ++r) {
      bool ok = true;
      for (size_t j = 0; j < k && ok; ++j) {
        x[j] = designValueM4(vDesign[j], r);
        if (std::isnan(x[j])) ok = false;
        else x[j] -= xbar[j];
      }
      if (!ok) continue;
      double s = 0.0;
      for (size_t j = 0; j < k; ++j) {
        double row = 0.0;
        for (size_t l = 0; l < k; ++l) row += V[j * k + l] * x[l];
        s += x[j] * row;
      }
      if (fe) s += sigma2 / nObs;
      q[r] = s;
    }
  }

  // e(a,b)'s scale is e(rmse) (regre_p2's "e(rmse)" argument).
  const double eSigma = [&] {
    const double rm = jsonNumM4(*lm.result, "rmse");
    return std::isfinite(rm) ? rm : std::sqrt(sigma2);
  }();
  if (eStat && !(eSigma > 0.0)) return refuse("the fit reports no root mean squared error");
  auto normalCdf = [](double z) { return 0.5 * std::erfc(-z / std::sqrt(2.0)); };
  auto normalDen = [](double z) { return std::exp(-0.5 * z * z) / std::sqrt(2.0 * 3.14159265358979323846); };

  vals.assign(n, kNaN);
  for (size_t r = 0; r < n; ++r) {
    const double yr = y != nullptr ? y[r] : kNaN;
    const double res = yr - xb[r];  // NaN wherever either is
    if (stat == "xb") {
      vals[r] = xb[r];
    } else if (eStat) {
      if (std::isnan(xb[r])) continue;
      const double L = (eLo - xb[r]) / eSigma, U = (eHi - xb[r]) / eSigma;
      double v = xb[r];
      if (!std::isnan(eLo) && std::isnan(eHi)) v = xb[r] + eSigma * normalDen(L) / normalCdf(-L);
      else if (std::isnan(eLo) && !std::isnan(eHi)) v = xb[r] - eSigma * normalDen(U) / normalCdf(U);
      else if (!std::isnan(eLo))
        v = xb[r] - eSigma * (normalDen(U) - normalDen(L)) / (normalCdf(U) - normalCdf(L));
      vals[r] = std::isfinite(v) ? v : kNaN;
    } else if (stat == "stdp") {
      vals[r] = std::isnan(q[r]) ? kNaN : std::sqrt(std::max(0.0, q[r]));
    } else if (stat == "dresiduals" || stat == "ue" ||
               (fam == PredictFamilyM4::XtregFe && stat == "residuals")) {
      vals[r] = res;
    } else if (stat == "xbd" || stat == "xbu") {
      vals[r] = inSample[r] ? xb[r] + d[r] : kNaN;
    } else if (stat == "d" || stat == "u") {
      vals[r] = inSample[r] ? d[r] : kNaN;
    } else if (q.size() == n) {
      // regress, vce(ols): h, stdr, stdf, rstandard, rstudent.
      if (std::isnan(q[r])) continue;
      const double h = q[r] / sigma2;
      const double s = std::sqrt(sigma2);
      if (stat == "hat") {
        vals[r] = h;
      } else if (stat == "stdf") {
        vals[r] = s * std::sqrt(1.0 + h);
      } else if (stat == "stdr") {
        vals[r] = h < 1.0 ? s * std::sqrt(1.0 - h) : kNaN;
      } else if (stat == "rstandard") {
        vals[r] = (h < 1.0 && !std::isnan(res)) ? res / (s * std::sqrt(1.0 - h)) : kNaN;
      } else if (stat == "rstudent") {
        if (h < 1.0 && !std::isnan(res) && dfR > 1.0) {
          const double sj2 = (sigma2 * dfR - res * res / (1.0 - h)) / (dfR - 1.0);
          vals[r] = sj2 > 0.0 ? res / (std::sqrt(sj2) * std::sqrt(1.0 - h)) : kNaN;
        }
      } else if (stat == "cooksd") {
        if (h < 1.0 && !std::isnan(res) && kEst > 0.0) {
          vals[r] = res * res * h / (kEst * sigma2 * (1.0 - h) * (1.0 - h));
        }
      }
    }
  }
  return std::nullopt;
}

// BUG-1260: the statistics served after a weighted regress or areg -- the ones
// computePredictWeightedM4 builds from e(b), y and the estimation sample.
bool predictServesWeightedM4(PredictFamilyM4 f, const std::string& s) {
  if (f == PredictFamilyM4::Regress) return s == "xb" || s == "residuals" || s == "score" || s == "stdp";
  if (f == PredictFamilyM4::Areg)
    return s == "xb" || s == "xbd" || s == "d" || s == "residuals" || s == "score" || s == "dresiduals";
  return false;
}

// BUG-1260: predict after a weighted regress or areg ([aw]/[fw]/[pw]/[iw]). The
// engine's predict refits without the weight (BUG-519), so nothing here calls
// it: xb is e(b)'s design over every row with the covariates; after areg the
// absorbed effect of a level is the weighted mean of y - xb over its e(sample)
// rows -- the normal equation of the level's indicator in the weighted least
// squares the fit solved -- whose weighted sum over e(sample) is zero exactly
// when _cons is the weighted ybar - xbar b, checked before anything is returned
// (StataBE 19.5 scratch-laneA/agents/p-margins/s6.do, `areg y x [aw=wv],
// absorb(f2)`: xbd[5] 40.406576265917387 on the 237 e(sample) rows, the weighted
// mean of d 1.6e-15; the [fw] fit the same). stdp after regress is sqrt(x V x')
// over the weighted fit's own e(V) (lincom's refit carries the weight, BUG-571).
std::optional<ojson> computePredictWeightedM4(SessionState& session, const LastModel& lm,
                                              PredictFamilyM4 fam, const std::string& stat,
                                              std::vector<double>& vals) {
  const std::string cmd = predictFamilyCmdM4(fam, lm);
  auto refuse = [&](const std::string& why) {
    return engineErrorBlockM4("predict, " + stat + " after a weighted `" + cmd +
                                  "` is not supported here in this version of PIET: " + why +
                                  ". Nothing was written to your data.",
                              "unsupported_option");
  };
  if (!lm.result.has_value() || !lm.result->is_object() || !lm.result->contains("coeftable") ||
      !(*lm.result)["coeftable"].is_array())
    return refuse("the fitted coefficients are not available");
  std::vector<std::string> names;
  std::vector<double> b, se;
  for (const auto& row : (*lm.result)["coeftable"]) {
    names.push_back(row.value("name", std::string()));
    b.push_back(jsonNumM4(row, "b"));
    se.push_back(jsonNumM4(row, "se"));
    if (!std::isfinite(b.back())) return refuse("the coefficient `" + names.back() + "` is missing");
  }
  const bool areg = fam == PredictFamilyM4::Areg;
  if (areg && lm.fe.size() != 1) return refuse("areg absorbs one variable");
  std::vector<double> V;
  if (stat == "stdp") {
    std::string why;
    try {
      if (!lincomVcovM4(session, lm, names, b, se, V, why)) return refuse(why);
    } catch (const CobraError& ce) {
      return refuse(cobraErrorStrM4(ce));
    }
  }
  Datastore* live = nullptr;
  try {
    live = session.datastore();
  } catch (const std::exception& e) {
    return refuse(e.what());
  }
  if (live == nullptr) return ojson(eval::noDataLoaded());
  const size_t n = static_cast<size_t>(live->nRows());
  std::vector<DesignColM4> design;
  std::string why;
  if (!buildDesignM4(*live, names, design, why)) return refuse(why);
  const double kNaN = std::numeric_limits<double>::quiet_NaN();
  std::vector<double> xb(n, kNaN);
  for (size_t r = 0; r < n; ++r) {
    double s = 0.0;
    bool ok = true;
    for (size_t j = 0; j < design.size() && ok; ++j) {
      const double v = designValueM4(design[j], r);
      if (std::isnan(v)) ok = false;
      else s += v * b[j];
    }
    if (ok) xb[r] = s;
  }
  const double* y = nullptr;
  if (live->has(lm.y) && live->col(lm.y).f64() != nullptr) y = live->col(lm.y).f64();
  if (y == nullptr && stat != "xb" && stat != "stdp")
    return refuse("the dependent variable `" + lm.y + "` is not a numeric variable");
  vals.assign(n, kNaN);
  if (stat == "xb") {
    vals = xb;
    return std::nullopt;
  }
  if (stat == "stdp") {
    const size_t k = names.size();
    for (size_t r = 0; r < n; ++r) {
      std::vector<double> x(k);
      bool ok = true;
      for (size_t j = 0; j < k && ok; ++j) {
        x[j] = designValueM4(design[j], r);
        ok = !std::isnan(x[j]);
      }
      if (!ok) continue;
      double q = 0.0;
      for (size_t j = 0; j < k; ++j)
        for (size_t l = 0; l < k; ++l) q += x[j] * V[j * k + l] * x[l];
      vals[r] = std::sqrt(std::max(0.0, q));
    }
    return std::nullopt;
  }
  if (!areg || stat == "dresiduals") {
    // regress residuals/score and areg dresiduals: y - xb wherever both exist.
    for (size_t r = 0; r < n; ++r) vals[r] = y[r] - xb[r];
    return std::nullopt;
  }
  // areg's absorbed effects over e(sample).
  const SessionState::EsampleReading er = session.esampleReading();
  if (er.kind != SessionState::EsampleReading::Kind::kColumn || !live->has(kEsampleColumn) ||
      live->col(kEsampleColumn).f64() == nullptr)
    return refuse("the estimation sample is not on the data in memory");
  const double* m = live->col(kEsampleColumn).f64();
  const std::string& g = lm.fe.front();
  if (!live->has(g) || live->col(g).f64() == nullptr || !live->has(lm.fitWeight) ||
      live->col(lm.fitWeight).f64() == nullptr)
    return refuse("the absorbed variable or the weight is not a numeric variable of the data");
  const double* gv = live->col(g).f64();
  const double* w = live->col(lm.fitWeight).f64();
  std::map<double, std::pair<double, double>> grp;  // level -> (sum w, sum w (y - xb))
  double count = 0.0, sumW = 0.0;
  for (size_t r = 0; r < n; ++r) {
    if (m[r] != static_cast<double>(er.serial)) continue;
    if (std::isnan(y[r]) || std::isnan(xb[r]) || std::isnan(gv[r]) || std::isnan(w[r]))
      return refuse("an estimation-sample row has no linear prediction");
    auto& a = grp[gv[r]];
    a.first += w[r];
    a.second += w[r] * (y[r] - xb[r]);
    count += 1.0;
    sumW += w[r];
  }
  const double eN = jsonNumM4(*lm.result, "n_obs");
  const bool fw = lm.fitWtype == "fw" || lm.fitWtype == "fweight";
  if (!(std::isfinite(eN) && (fw ? sumW : count) == eN))
    return refuse("the estimation sample on the data in memory is not the fitted one");
  double wsumD = 0.0, scale = 0.0;
  for (const auto& [lv, a] : grp) {
    if (!(a.first > 0.0)) return refuse("a level of the absorbed variable carries no weight");
    wsumD += a.second;
    scale += std::fabs(a.second);
  }
  for (size_t r = 0; r < n; ++r)
    if (m[r] == static_cast<double>(er.serial)) scale += std::fabs(w[r] * (y[r] - xb[r]));
  if (std::fabs(wsumD) > 1e-9 * std::max(1.0, scale))
    return refuse("the absorbed effects do not sum to zero over the estimation sample, so the "
                  "design read from e(b) is not the fitted one");
  for (size_t r = 0; r < n; ++r) {
    if (m[r] != static_cast<double>(er.serial)) continue;
    const auto& a = grp[gv[r]];
    const double d = a.second / a.first;
    if (stat == "d") vals[r] = d;
    else if (stat == "xbd") vals[r] = xb[r] + d;
    else vals[r] = y[r] - xb[r] - d;  // residuals, score
  }
  return std::nullopt;
}

// BUG-1257: stdp, score and ir after logit/probit/poisson, over every row whose
// covariates are there (out of the estimation sample too, as Stata fills them),
// from e(b) and the design read off the live frame. `engineArgs` is runPredict's
// --model replay argv (no --xb/--pred/--out); its xb must be the one built here
// on every row, which also refuses a fit whose linear index carries something
// e(b) does not (an offset). stdp is sqrt(x V x'), V from the engine's lincom
// (lincomVcovM4). The equation score, d lnL / d xb, is StataBE 19.5's: logit
// (y != 0) - p and probit phi/Phi or -phi/(1 - Phi), where a missing y counts as
// nonzero as `y != 0` does in Stata; cloglog's f/F or -exp(xb), missing with y;
// poisson y - exp(xb), missing with y. ir is exp(xb).
std::optional<ojson> computePredictMlM4(SessionState& session, const LastModel& lm,
                                        const std::string& stat,
                                        std::vector<std::string> engineArgs,
                                        std::vector<double>& vals) {
  auto refuse = [&](const std::string& why) {
    return engineErrorBlockM4("predict, " + stat + " after `" + lm.cobraCmd +
                                  "` is not supported here in this version of PIET: " + why +
                                  ". Nothing was written to your data.",
                              "unsupported_option");
  };
  std::vector<std::string> names;
  std::vector<double> b, se;
  if (!lm.result.has_value() || !lm.result->is_object() || !lm.result->contains("coeftable") ||
      !(*lm.result)["coeftable"].is_array()) {
    return refuse("the fitted coefficients are not available");
  }
  for (const auto& row : (*lm.result)["coeftable"]) {
    if (!row.is_object() || !row.contains("name") || !row["name"].is_string()) {
      return refuse("the fitted coefficients are not available");
    }
    names.push_back(row["name"].get<std::string>());
    b.push_back(jsonNumM4(row, "b"));
    se.push_back(jsonNumM4(row, "se"));
    if (!std::isfinite(b.back())) return refuse("the coefficient `" + names.back() + "` is missing");
  }
  std::vector<double> V;
  if (stat == "stdp") {
    std::string why;
    try {
      if (!lincomVcovM4(session, lm, names, b, se, V, why)) return refuse(why);
    } catch (const CobraError& ce) {
      return refuse(cobraErrorStrM4(ce));
    }
  }

  Datastore* live = nullptr;
  try {
    live = session.datastore();
  } catch (const std::exception& e) {
    return refuse(e.what());
  }
  if (live == nullptr) return ojson(eval::noDataLoaded());
  for (size_t i = 0; i + 1 < engineArgs.size(); ++i) {
    if ((engineArgs[i] == "--data" || engineArgs[i] == "--fit-data") &&
        !cecon::native_exists(engineArgs[i + 1])) {
      return refuse("the data the model was fitted on are not available on disk");
    }
  }
  const std::string kXb = "__piet_predict_xb__";
  const std::string scratch = nextPathEst(session.workdir(), "predict_refit");
  engineArgs.insert(engineArgs.end(), {"--xb", kXb, "--out", scratch});
  const CobraRunResult rr = runCobraOneShot(engineArgs);
  if (!rr.ok || !cecon::native_exists(scratch)) {
    cecon::native_unlink(scratch);
    const std::string err = rr.stderr_text.empty() ? rr.error : rr.stderr_text;
    return refuse("the engine's refit failed (" + strutil::trim(err) + ")");
  }
  std::vector<double> engXb;
  try {
    Datastore eng(scratch);
    if (eng.nRows() != live->nRows() || !eng.has(kXb) || eng.col(kXb).f64() == nullptr) {
      throw std::runtime_error("the engine's refit returned an unexpected frame");
    }
    const double* px = eng.col(kXb).f64();
    engXb.assign(px, px + eng.nRows());
  } catch (const std::exception& e) {
    cecon::native_unlink(scratch);
    return refuse(e.what());
  }
  cecon::native_unlink(scratch);

  try {
    live = session.datastore();
  } catch (const std::exception& e) {
    return refuse(e.what());
  }
  if (live == nullptr) return ojson(eval::noDataLoaded());
  const size_t n = static_cast<size_t>(live->nRows());
  if (engXb.size() != n) return refuse("the engine's refit returned an unexpected frame");
  const double* y = nullptr;
  if (live->has(lm.y) && live->col(lm.y).dtype == cecon::DType::F64) y = live->col(lm.y).f64();
  if (stat == "score" && y == nullptr) {
    return refuse("the dependent variable `" + lm.y + "` is not a numeric variable");
  }
  std::vector<DesignColM4> design;
  std::string why;
  if (!buildDesignM4(*live, names, design, why)) return refuse(why);

  const double kNaN = std::numeric_limits<double>::quiet_NaN();
  const size_t k = design.size();
  std::vector<double> x(k);
  vals.assign(n, kNaN);
  for (size_t r = 0; r < n; ++r) {
    bool ok = true;
    double xb = 0.0;
    for (size_t j = 0; j < k && ok; ++j) {
      x[j] = designValueM4(design[j], r);
      if (std::isnan(x[j])) ok = false;
      else xb += x[j] * b[j];
    }
    if (!ok) {
      if (!std::isnan(engXb[r])) return refuse("the design read from e(b) does not cover a row the engine predicts");
      continue;
    }
    if (std::isnan(engXb[r]) || std::fabs(xb - engXb[r]) > 1e-9 * (1.0 + std::fabs(engXb[r]))) {
      return refuse("the design read from e(b) does not reproduce the engine's linear prediction");
    }
    if (stat == "stdp") {
      double s = 0.0;
      for (size_t j = 0; j < k; ++j) {
        double row = 0.0;
        for (size_t l = 0; l < k; ++l) row += V[j * k + l] * x[l];
        s += x[j] * row;
      }
      vals[r] = std::sqrt(std::max(0.0, s));
    } else if (stat == "ir") {
      vals[r] = std::exp(xb);
    } else if (lm.cobraCmd == "poisson") {
      vals[r] = y[r] - std::exp(xb);  // missing with y
    } else if (lm.cobraCmd == "logit") {
      vals[r] = (y[r] != 0.0 ? 1.0 : 0.0) - 1.0 / (1.0 + std::exp(-xb));
    } else if (lm.cobraCmd == "cloglog") {
      // F = 1 - exp(-exp(xb)): f/F = exp(xb) exp(-exp(xb)) / F when y != 0, -exp(xb)
      // else; missing with y, unlike logit's (StataBE 19.5, BUG-1257.do's data:
      // obs1 -.504196078796196, obs2 .713718029916928, obs30 with d missing `.`).
      const double ex = std::exp(xb);
      vals[r] = std::isnan(y[r]) ? kNaN : y[r] != 0.0 ? ex * std::exp(-ex) / -std::expm1(-ex) : -ex;
    } else {
      const double phi = std::exp(-0.5 * xb * xb) / std::sqrt(2.0 * 3.14159265358979323846);
      vals[r] = y[r] != 0.0 ? phi / (0.5 * std::erfc(-xb / std::sqrt(2.0)))
                            : -phi / (0.5 * std::erfc(xb / std::sqrt(2.0)));
    }
  }
  return std::nullopt;
}

// Write `vals` as the new f64 column `name` of the live frame into `outPath`
// (`cobra setcol --new`, the payload shape estimWriteColumn uses: one value per
// row, an EMPTY line for missing). `storeType` rides as --store-type: the engine
// stores each value as that Stata type holds it (BUG-924).
std::optional<ojson> writePredictColumnM4(SessionState& session, const std::string& liveHead,
                                          const std::string& name, const std::vector<double>& vals,
                                          const std::string& outPath, const std::string& storeType) {
  const std::string hb = mintScratchPath(session.workdir(), "predict_values");
  forgetScratchMint(hb);
  const std::string csv = hb.substr(0, hb.size() - 5) + ".csv";  // 5 == strlen(".hbin")
  registerScratchMint(csv);
  {
    std::ofstream f(cecon::to_native_path(csv));
    if (!f) return engineErrorBlockM4("predict: could not write the values payload");
    f.setf(std::ios::fmtflags(0), std::ios::floatfield);
    f.precision(17);
    for (double v : vals) {
      if (!std::isnan(v)) f << v;
      f << "\n";
    }
    if (!f) return engineErrorBlockM4("predict: could not write the values payload");
  }
  std::vector<std::string> argv = {"setcol", "--data", liveHead, "--var", name, "--values", csv,
                                   "--dtype", "f64", "--new", "--out", outPath};
  storage::appendStoreTypeArg(argv, storeType);
  const CobraRunResult r = runCobra(argv);
  cecon::native_unlink(csv);
  if (!r.ok) {
    return engineErrorBlockM4("predict: could not create " + name + " (" +
                              strutil::trim(r.stderr_text) + ")");
  }
  // The engine's writer creates --out before it can detect a write failure (the
  // estimWriteColumn note): a missing file is a failed write, not a head.
  if (!cecon::native_exists(outPath)) {
    return engineErrorBlockM4("predict: failed writing output dataset (disk full or I/O error)");
  }
  return std::nullopt;
}

// BUG-466: predict after `teffects nnmatch` (_teffects_match_p.ado): te, po and
// distance, read off the fit's own reply -- the engine returned them with the
// estimate (cmds_nnmatch.cpp: te, po0, po1 and each row's match distances,
// null off e(sample)) and LastModel holds that reply. StataBE 19.5
// (internal capture oracle.txt CASE 1, data_1.csv):
// `predict double te_, te`, `predict double po0_ po1_, po` and `predict double
// dd_*, distance` rc 0. The ado's own checks: generate() at estimation (else
// r(322)), the observation count unchanged (else r(459)), one statistic (else
// r(184)), te by default; one name for po is the control's outcome unless
// tlevel() names the treated level; distance makes one variable per index
// variable (a stub* or that many names); no if/in in its syntax.
ojson runNnmatchPredictM4(const std::vector<std::string>& varlist,
                          const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                          SessionState& session, const LastModel& lm,
                          const std::optional<std::string>& ifExpr) {
  auto err = [](const std::string& msg, int rc) {
    return ojson(eval::errorBlock("engine_error", msg, std::nullopt, std::nullopt, std::nullopt,
                                  "r(" + std::to_string(rc) + ")"));
  };
  if (ifExpr.has_value() && !ifExpr->empty()) return err("if not allowed", 101);
  std::string which, tlevel;
  int nWhich = 0;
  for (const auto& [k, v] : opts) {
    if (k == "quietly" || k == "noisily") continue;
    if (k == "te" || k == "po" || k == "distance" || k == "dist" || k == "dista" || k == "distan" ||
        k == "distanc" || k == "biasadj" || k == "km" || k == "kn") {
      which = k.rfind("dist", 0) == 0 ? "distance" : k;
      ++nWhich;
    } else if (k == "tlevel" || k == "tl" || k == "tle" || k == "tlev" || k == "tleve") {
      tlevel = trimM4(v.strValue);
    } else if (k == "ps" || k == "lnsigma" || k == "lns" || k == "lnsi" || k == "lnsig" || k == "lnsigm") {
      return err("option " + std::string(k == "ps" ? "ps" : "lnsigma") + " not allowed", 198);
    } else {
      return err("option " + k + " not allowed", 198);
    }
  }
  if (nWhich > 1)
    return err("only one of te, po, km, kn, ps, lnsigma or distance may be specified", 184);
  if (which.empty()) which = "te";
  if (which == "biasadj") return err("option biasadj not allowed; bias adjustment was not used at estimation", 322);
  if (which == "km" || which == "kn")
    return engineErrorBlockM4("predict, " + which + " after `teffects nnmatch` is not supported in this "
                              "version of PIET. Nothing was written to your data.",
                              "unsupported_option");
  const std::string* indexvar = session.eval.eStrings.find("indexvar");
  if (indexvar == nullptr || indexvar->empty())
    return err("postestimation can only be performed when the generate(stub) option is used during "
               "estimation; see help generate(stub)", 322);
  if (!lm.result.has_value() || !lm.result->is_object() || !lm.result->contains("te") ||
      !(*lm.result)["te"].is_array())
    return engineErrorBlockM4("predict after `teffects nnmatch`: the fit's results are not available",
                              "unsupported_option");
  const ojson& res = *lm.result;
  const size_t nFit = res["te"].size();
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  Datastore* live = nullptr;
  try {
    live = session.datastore();
  } catch (const std::exception& e) {
    return engineErrorBlockM4(std::string("predict: ") + e.what());
  }
  if (live == nullptr) return eval::noDataLoaded();
  if (static_cast<size_t>(live->nRows()) != nFit)
    return err("there were " + std::to_string(nFit) + " observations at estimation time but there are " +
                   std::to_string(live->nRows()) + " now; nearest-neighbor indices are no longer valid",
               459);
  // Names: a leading storage type, then names or one stub*.
  std::vector<std::string> toks = varlist;
  std::string declaredType;
  if (toks.size() >= 2 && storage::isNumericType(lowerM4(toks[0]))) {
    declaredType = lowerM4(toks[0]);
    toks.erase(toks.begin());
  }
  const double control = res.value("control_level", 0.0), treated = res.value("treated_level", 1.0);
  const std::string tvar = res.value("tvar", std::string());
  auto levelText = [&](double lv) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(std::llround(lv)));
    std::string t = buf;
    const auto vl = session.valueLabels.find(tvar);
    if (vl != session.valueLabels.end()) {
      const auto it = vl->second.find(static_cast<int>(std::llround(lv)));
      if (it != vl->second.end()) t = it->second;
    }
    return t;
  };
  int kvar = 0;
  for (size_t p = 0; p < indexvar->size();) {
    const size_t q = indexvar->find_first_not_of(' ', p);
    if (q == std::string::npos) break;
    const size_t e = indexvar->find(' ', q);
    ++kvar;
    p = e == std::string::npos ? indexvar->size() : e;
  }
  std::vector<std::string> names, labels;
  std::vector<std::vector<double>> cols;
  auto arr = [&](const ojson& a, size_t r) {
    return r < a.size() && a[r].is_number() ? a[r].get<double>() : std::numeric_limits<double>::quiet_NaN();
  };
  const size_t n = nFit;
  if (which == "te") {
    if (toks.size() != 1 || toks[0].find('*') != std::string::npos)
      return err(toks.size() > 1 ? "too many variables specified" : "varlist required", toks.size() > 1 ? 103 : 100);
    std::vector<double> c(n);
    for (size_t r = 0; r < n; ++r) c[r] = arr(res["te"], r);
    names.push_back(toks[0]);
    labels.push_back("treatment effects");
    cols.push_back(std::move(c));
  } else if (which == "po") {
    std::vector<std::string> pn = toks;
    if (pn.size() == 1 && pn[0].size() > 1 && pn[0].back() == '*' && tlevel.empty()) {
      const std::string stub = pn[0].substr(0, pn[0].size() - 1);
      pn = {stub + "0", stub + "1"};
    }
    if (pn.size() == 2 && tlevel.empty()) {
      std::vector<double> c0(n), c1(n);
      for (size_t r = 0; r < n; ++r) {
        c0[r] = arr(res["po0"], r);
        c1[r] = arr(res["po1"], r);
      }
      names = pn;
      labels = {"potential outcome, " + tvar + "=" + levelText(control),
                "potential outcome, " + tvar + "=" + levelText(treated)};
      cols.push_back(std::move(c0));
      cols.push_back(std::move(c1));
    } else if (pn.size() == 1 && pn[0].find('*') == std::string::npos) {
      double lv = std::min(control, treated);
      if (!tlevel.empty()) {
        char* end = nullptr;
        lv = std::strtod(tlevel.c_str(), &end);
        if (end == nullptr || *end != '\0' || (lv != control && lv != treated))
          return err("tlevel() must be valid treatment level", 198);
      }
      const bool isTreated = lv == treated;
      std::vector<double> c(n);
      for (size_t r = 0; r < n; ++r) c[r] = arr(res[isTreated ? "po1" : "po0"], r);
      names = pn;
      labels = {"potential outcome, " + tvar + "=" + levelText(isTreated ? treated : control)};
      cols.push_back(std::move(c));
    } else {
      return err("po requires two new variable names or one with tlevel()", 198);
    }
  } else {  // distance
    if (!tlevel.empty()) return err("tlevel() may only be specified with po or ps", 198);
    if (!res.contains("dist") || !res["dist"].is_array() || res["dist"].size() != n)
      return engineErrorBlockM4("predict, distance after `teffects nnmatch`: the fit's match distances "
                                "are not available",
                                "unsupported_option");
    std::vector<std::string> dn = toks;
    if (dn.size() == 1 && dn[0].size() > 1 && dn[0].back() == '*') {
      const std::string stub = dn[0].substr(0, dn[0].size() - 1);
      dn.clear();
      for (int k = 1; k <= kvar; ++k) dn.push_back(stub + std::to_string(k));
    }
    if (static_cast<int>(dn.size()) != kvar)
      return err(static_cast<int>(dn.size()) > kvar ? "too many variables specified"
                                                    : "too few variables specified",
                 static_cast<int>(dn.size()) > kvar ? 103 : 102);
    const std::string metric = res.value("metric", std::string("mahalanobis"));
    for (int k = 0; k < kvar; ++k) {
      std::vector<double> c(n);
      for (size_t r = 0; r < n; ++r) {
        const ojson& d = res["dist"][r];
        c[r] = d.is_array() && static_cast<int>(d.size()) > k && d[k].is_number()
                   ? d[k].get<double>()
                   : std::numeric_limits<double>::quiet_NaN();
      }
      names.push_back(dn[k]);
      labels.push_back("nearest-neighbor distance using " + metric);
      cols.push_back(std::move(c));
    }
  }
  for (const auto& nm : names) {
    if (!strutil::isValidStataIdentifier(nm)) return err(nm + " invalid name", 198);
    if (live->has(nm)) return ojson(eval::engineError("variable " + nm + " already defined"));
  }
  const std::string storeType = storage::resolveCreateType(session, declaredType);
  std::string head = *session.currentHbin();
  std::vector<std::string> written;
  for (size_t i = 0; i < names.size(); ++i) {
    const std::string out = nextPathEst(session.workdir(), "predict");
    if (auto rej = writePredictColumnM4(session, head, names[i], cols[i], out, storeType)) {
      for (const auto& w : written) cecon::native_unlink(w);
      return *rej;
    }
    written.push_back(out);
    head = out;
  }
  session.load(head);
  session.invalidateDatastore();
  for (size_t i = 0; i < names.size(); ++i) {
    storage::recordCreateType(session, names[i], declaredType, storeType);
    session.varMeta[names[i]].label = labels[i];
  }
  session.syncEvalVarLabels();
  Datastore* ds = session.datastore();
  session.seedNMissForPureAdd(ds, "gen", names, /*droppedVars=*/{}, /*rewrittenVars=*/{});
  ojson out;
  out["type"] = "data_mutation";
  out["action"] = "gen";
  ojson varlistAfter = ojson::array();
  auto vlist = session.varlistWithMeta(ds);
  for (const auto& v : vlist) varlistAfter.push_back(v.toJson());
  session.updateNMissCache(vlist);
  out["varlist_after"] = varlistAfter;
  out["n_rows_after"] = ds ? ds->nRows() : 0;
  out["new_vars"] = names;
  return out;
}

}  // namespace

ojson runPredict(const std::vector<std::string>& varlist,
                  const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                  SessionState& session, const std::optional<std::string>& ifExpr) {
  const auto& lmOpt = session.lastModel();
  if (!lmOpt.has_value()) {
    // BUG-1248: with no estimates predict still reads its new variable and its `if`
    // before e(), with data in memory or none (StataBE 19.5): `predict` is r(100)
    // "varlist required", `predict 1p` r(198) "1p invalid name", `predict x1`
    // r(110) "variable x1 already defined", `predict p if nosuch > 1` r(111)
    // "nosuch not found"; `predict p`, `predict p if 1`, `predict p, bogus` and
    // `predict double p` go on to r(301).
    auto refuse = [](const std::string& message, int rc) {
      return ojson(eval::errorBlock("syntax_error", message, std::nullopt, std::nullopt,
                                    std::nullopt, "r(" + std::to_string(rc) + ")"));
    };
    if (varlist.empty()) return refuse("varlist required", 100);
    const size_t first =
        varlist.size() >= 2 && storage::isNumericType(lowerM4(varlist[0])) ? 1 : 0;
    for (size_t i = first; i < varlist.size(); ++i) {
      const std::string& nm = varlist[i];
      if (!strutil::isValidStataIdentifier(nm)) return refuse(nm + " invalid name", 198);
      if (resolveVarName(nm, session) == nm)
        return refuse("variable " + nm + " already defined", 110);
    }
    if (ifExpr.has_value()) {
      if (auto unbound = exprUnboundNameRefusal(*ifExpr, session))  // "r(111): NAME not found"
        return refuse(unbound->substr(unbound->find(": ") + 2), 111);
    }
    return noPreviousEstimationFor(session, true);
  }
  const LastModel& lm = *lmOpt;
  if (lm.cobraCmd == "nnmatch") return runNnmatchPredictM4(varlist, opts, session, lm, ifExpr);  // BUG-466
  // BUG-204 (BUG-203's sibling, and the worse half): `predict` never read the
  // estimator either. The argv built below carries --data/--y/--x and no model,
  // and cobra's cmd_predict calls feols_fit() unconditionally, so `logit y x1 x2`
  // -> `predict phat` wrote the LINEAR-PROBABILITY fitted values into the user's
  // dataset under a name they believe holds Pr(y=1). Measured against StataNow
  // 19.5: min -0.06666 / max 1.20595 where Stata's own predict gives min 0.04791
  // / max 0.97712 — values outside [0,1] for a probability, and they PERSIST as
  // data, surviving save/export, which is why this outranks the lincom shape.
  //
  // Why no gate saw it: OLS fitted values average to ybar and so do logit
  // predicted probabilities, so the MEAN agrees to 15 digits (0.5633333...) and
  // every mean-based sanity check passed identically. Any pin on this path must
  // assert the RANGE, not central tendency.
  //
  // BUG-297 closes BUG-204's open half for logit/probit/poisson: cobra's
  // cmd_predict now HAS a `--model` GLM prediction path (the postest_fit_spec
  // dispatcher BUG-203 built — the same refit lincom/nlcom/test forward to),
  // emitting Stata's default statistic for the model: `pr` after logit/probit,
  // `n` after poisson, `xb` on request. So the refusal NARROWS to exactly the
  // estimators the engine still cannot re-fit (nbreg, tobit, heckman, qreg,
  // xtreg-re, fepois/ppmlhdfe/xtpois/xtlogit — no absorbed-effects ML refit —
  // newey, prais, iv/liml/reg3, csdid, ...), same membership as lincom/nlcom/
  // test's postestGlmCmdsM4 gate. Unlike those verbs, predict is a POINT
  // prediction — it reads only beta, which vce() never moves — so a
  // logit/probit/poisson fitted with vce(robust)/vce(cluster) still predicts
  // (the argv below carries no --vcov/--cluster and the engine refit is the
  // plain fit with the identical beta). Absorbed FE would move beta, so that
  // still refuses.
  const bool glmPredict = postestGlmCmdsM4().count(lm.cobraCmd) > 0;
  if (!postestOlsCmdsM4().count(lm.cobraCmd) && !glmPredict) {
    return engineErrorBlockM4(
        "predict is not supported after `" + lm.cobraCmd +
            "` in this version of PIET — the engine can compute predictions only for the "
            "least-squares family and for logit/probit/poisson (via a maximum-likelihood "
            "re-fit); `" + lm.cobraCmd +
            "` has neither, so the column it could write would not be this model's "
            "prediction. Refusing rather than writing a number that is not the one asked "
            "for into your data.",
        "unsupported_option");
  }
  // FU-131: a no-constant fit predicts from the no-constant refit (--no-intercept
  // below; w2p_estim_ev.log:1387 G_nc_predict_xb, :1402 G_nc_predict_res), except
  // where the refit cannot drop the constant.
  if (postestNoConstantUnrefittableM4(lm)) return postestNoConstantRejectM4("predict", lm);
  if (glmPredict && !lm.fe.empty()) {
    return engineErrorBlockM4(
        "predict is not supported after `" + lm.cobraCmd +
            "` with absorbed fixed effects in this version of PIET — the engine's "
            "maximum-likelihood re-fit has no absorbed-effects path, so its coefficients "
            "(and therefore its predictions) would differ from the model you fitted. "
            "Refusing rather than predicting from a different model.",
        "unsupported_option");
  }
  // BUG-519: predict after a WEIGHTED fit wrote the UNWEIGHTED model's
  // predictions at rc 0. The fit's [aw/fw/pw/iw=] lives in lm.fitWeight (margins
  // forwards it), but the engine's predict has no weight input at all --
  // cmd_predict's refit is `build_model(*fit_ds, y, xs, fes, {})` and its ML path
  // reads only --model/--fe/--vcov/--cluster -- so every branch below (head
  // unmoved, `--fit-data`, the absorbed-FE refit, the logit/probit/poisson ML
  // refit) re-estimated without the weight. Measured on main 2026-09-22:
  // `regress y x2 t [aw=w]` / `predict p` -> p differs from _b[_cons] + _b[x2]*x2
  // + _b[t]*t by up to 0.38 on 12 rows, rc 0; the same under [fw=]. Stata serves
  // it (aer_b4/predict-e-xbd.log J_build_areg: `predict double A_xb, xb` after
  // `areg ... [aw=wv]`, rc 0) -- this build cannot, so it refuses by name and
  // writes nothing. BUG-1260: after a weighted regress or areg the statistics
  // computePredictWeightedM4 builds without the engine are served; every other
  // weighted fit and statistic keeps this refusal (the statistic's half below,
  // once the options are read).
  const bool weightedFit = !lm.fitWeight.empty();
  auto weightedRefusal = [&lm]() {
    return engineErrorBlockM4(
        "predict after a weighted fit ([" + (lm.fitWtype.empty() ? std::string("weight") : lm.fitWtype) +
            "=" + lm.fitWeight + "]) is not supported in this version of PIET -- the "
            "engine's prediction re-fits the model without the weights, so the column it "
            "could write would hold a different model's predictions. Refusing rather than "
            "writing a number that is not the one asked for into your data. Nothing was "
            "written to your data.",
        "unsupported_option");
  };
  if (weightedFit && (glmPredict || (predictFamilyM4(lm) != PredictFamilyM4::Regress &&
                                     predictFamilyM4(lm) != PredictFamilyM4::Areg))) {
    return weightedRefusal();
  }

  static const std::set<std::string> kSupported = {"xb", "resid", "residuals", "fe"};
  static const std::set<std::string> kUnsupportedStats = {
      "cooksd", "leverage", "hat",     "stdp",     "stdf",   "stdr",  "rstandard",
      "rstudent", "dfbeta",  "pr",     "xbu",      "dresid", "anscombe",
      "deviance", "pearson", "response", "score",  "working",
  };
  bool wantXb = false, wantResid = false, wantMlDefault = false;
  std::string mlStat;  // stdp/score/ir after logit/probit/poisson (computePredictMlM4)
  const PredictFamilyM4 family = glmPredict ? PredictFamilyM4::Other : predictFamilyM4(lm);
  // FU-768: an absorbed-effects fit restricted by `if`/`in` without time-series
  // operators (those keep FU-759's own gate below). The engine refits it under
  // the restriction as a row mask over the live frame (cmd_predict --fit-keepif
  // with --fe), so its within residual lands on the fit's rows.
  const bool restrictedAbsorbed = !glmPredict && !lm.fe.empty() &&
                                  (!lm.fitKeepif.empty() || !lm.fitDataPath.empty()) &&
                                  !postestTsKeepifRefusalM4("predict", lm).has_value();
  std::string predictStat;  // the statistic, for the four families above
  bool nativeStat = false;  // computed here rather than by the engine's predict
  if (glmPredict) {
    // The ML option surface, per Stata: bare `predict newvar` = the model's
    // default statistic (pr after logit/probit, n after poisson); the explicit
    // spelling of that default (`pr` / `n`) is accepted; `xb` is the linear
    // index. EVERYTHING else refuses BY NAME — including `resid`/`residuals`,
    // which after an ML fit is a DIFFERENT statistic in Stata (a Pearson
    // residual, not y - xb), so serving the OLS shape under that name would be
    // BUG-204's silent-wrong all over again.
    //
    // BUG-1257: the refusal was rc 1. A statistic the estimator's own predict does
    // not take is Stata's r(198) "option ir not allowed"; one it takes that this
    // build does not compute is refused by name, r(198) as every option this build
    // will not take. stdp, score and ir are computed here (computePredictMlM4).
    const std::string defaultStat = (lm.cobraCmd == "poisson") ? "n" : "pr";
    for (const auto& [k, v] : opts) {
      if (k == "quietly" || k == "noisily") continue;
      // poisson's pr() takes its count(s): `pr(0)`, `pr(1,3)`; bare `pr` is refused.
      const std::string stat = (lm.cobraCmd == "poisson" && k == "pr" && !v.isBool)
                                   ? std::string("pr()")
                                   : mlPredictStatisticM4(lm.cobraCmd, k);
      if (stat.empty() || (stat != "pr()" && !(v.isBool && v.boolValue))) {
        return engineErrorBlockM4("option " + k + " not allowed", "syntax_error");
      }
      if (stat == "xb") { wantXb = true; continue; }
      if (stat == defaultStat) { wantMlDefault = true; continue; }
      if (stat == "stdp" || stat == "score" || stat == "ir") { mlStat = stat; continue; }
      return engineErrorBlockM4(
          "predict " + k + ": not supported after `" + lm.cobraCmd +
              "` in this version of PIET (supported there: the default `" + defaultStat +
              "` statistic, `xb`, `stdp`, `score`" +
              (lm.cobraCmd == "poisson" ? std::string(", `ir`") : std::string()) +
              "). Nothing was written to your data.",
          "unsupported_option");
    }
    if (!wantXb && mlStat.empty()) wantMlDefault = true;  // bare `predict newvar`
  } else if (family != PredictFamilyM4::Other) {
    // BUG-596: regress / areg / xtreg,fe / reghdfe answer their OWN statistic
    // vocabulary (the block above computePredictNativeM4). The engine keeps the
    // two it already writes as Stata defines them; everything else is computed
    // below from e(b), e(V) and the engine refit's estimation sample.
    if (auto rej = resolvePredictStatM4(family, lm, opts, predictStat)) return *rej;
    if (restrictedAbsorbed && !predictServesRestrictedM4(family, predictStat)) {
      return engineErrorBlockM4("predict, " + predictStat + " after a restricted `" +
                                    predictFamilyCmdM4(family, lm) + " ... if` fit is not supported "
                                    "in this version of PIET. Nothing was written to your data.",
                                "unsupported_option");
    }
    const std::string served = predictEngineServedM4(family, predictStat);
    wantXb = served == "xb";
    wantResid = served == "resid";
    nativeStat = served.empty();
    if (weightedFit) {
      // BUG-1260: no engine refit after a weighted fit (computePredictWeightedM4).
      if (!predictServesWeightedM4(family, predictStat)) return weightedRefusal();
      wantXb = wantResid = false;
      nativeStat = true;
    }
  } else {
  if (restrictedAbsorbed) {
    // FU-768: only the four families above have a captured restricted-fit answer.
    return engineErrorBlockM4("predict after a restricted fit with absorbed effects is not "
                              "supported after `" + lm.cobraCmd + "` in this version of PIET. "
                              "Nothing was written to your data.",
                              "unsupported_option");
  }
  for (const auto& [k, v] : opts) {
    if (kUnsupportedStats.count(k)) {
      // B4 P1: `unsupported_option` -> r(198). This refusal defaulted to
      // `engine_error`, i.e. rc 1 — a code no Stata command answers with, and
      // the one the standing rule reserves for nothing at all. An option this
      // build will not take is r(198), whatever the reason.
      return engineErrorBlockM4("predict " + k +
                                ": not supported (PIET supports xb/resid/fe). The " + k +
                                " statistic requires influence diagnostics not yet implemented.",
                                "unsupported_option");
    }
    // Audit fix: this check used to also require `v.isBool && v.boolValue`,
    // so it only ever caught unknown BOOLEAN flags — an unknown VALUED
    // option (e.g. a typo'd `predict yhat, level(95)`, where `level` isn't
    // a real predict() option and v.isBool is false) matched neither this
    // branch nor kUnsupportedStats above, and fell through completely
    // silently (defaulting to plain `xb`) instead of naming the bad
    // option. Any key not in kSupported/kUnsupportedStats/quietly/noisily
    // is unrecognized regardless of whether it was written as a bare flag
    // or a valued option.
    if (!kSupported.count(k) && k != "quietly" && k != "noisily") {
      // B4 P1: same rc correction as the branch above — see its comment.
      return engineErrorBlockM4("predict " + k +
                                ": not supported (PIET supports xb/resid/fe). The " + k +
                                " statistic requires influence diagnostics not yet implemented.",
                                "unsupported_option");
    }
  }

  for (const auto& [k, v] : opts) {
    if (k == "xb" && v.isBool && v.boolValue) wantXb = true;
    if ((k == "resid" || k == "residuals") && v.isBool && v.boolValue) wantResid = true;
  }
  if (!wantXb && !wantResid) wantXb = true;
  }

  // BUG-127: `predict <type> newvar` used to take the storage-type token AS
  // the new variable's name and silently discard the real name (`predict
  // double yh, xb` created a column literally named `double`; `yh` never
  // existed — gate-① silent-wrong). The leading token is stripped the way
  // gen/egen's stripStorageType does.
  //
  // BUG-924: and the new variable HAS that type, values included. Stata's predict
  // creates it as the declared type, else c(type) -- float by default -- and
  // stores the values that type holds (w2aa_predict_type_restricted.log a_p_default
  // `+1.dda8fe0000000X+003|float`, a_pf_float_xb, a_pi_int_xb `int` truncated,
  // a_p_settype_double `double`; w2e_float_default.log predict_default). PIET
  // stored the double under a float label, so `count if p == pd` was 60 where
  // Stata's is 0 (a_eq_p_pd). One resolver (storage::resolveCreateType, generate's)
  // decides the type and the engine rounds on store (--store-type, BUG-471 T1).
  std::vector<std::string> nameToks = varlist;
  std::string declaredType;  // the typed token, "" = none
  if (nameToks.size() >= 2 && storage::isNumericType(lowerM4(nameToks[0]))) {
    declaredType = lowerM4(nameToks[0]);
    nameToks.erase(nameToks.begin());
  }
  std::string storeType;
  if (family == PredictFamilyM4::Reghdfe || lm.stataCmd == "reghdfe") {
    // reghdfe's predict writes double whatever type it is given:
    // predict-e-xbd.log P_res_xb/xbd/d/residuals (`predict float Q_<stat>`) are
    // rc 0 and type_xbd reads `double`; w2aa c_hdfe_* untyped read `double`.
    // An integer type there is uncaptured.
    if (storage::isIntegerType(declaredType)) {
      return engineErrorBlockM4("predict: storage type " + declaredType +
                                    " is not supported after `reghdfe` in this version of PIET",
                                "unsupported_option");
    }
    storeType = "double";
  } else {
    storeType = storage::resolveCreateType(session, declaredType);
  }
  std::string newvar = nameToks.empty() ? std::string() : nameToks[0];
  if (newvar.empty()) {
    return engineErrorBlockM4("predict: a new variable name is required, e.g. `predict yhat`.",
                              "syntax_error");
  }
  // BUG-1120: `predict _cons' and `predict _b, resid' are "_cons invalid varname"
  // r(198), `predict _all, resid' "_all not allowed" r(101); `predict _se, stdp'
  // is legal (StataBE 19.5).
  if (strutil::isReservedVarName(newvar) || newvar == "_skip" || newvar == "_all") {
    const bool skipAll = !strutil::isReservedVarName(newvar);
    return ojson(eval::errorBlock("syntax_error",
                                  newvar + (skipAll ? " not allowed" : " invalid varname"),
                                  std::nullopt, std::nullopt, std::nullopt,
                                  skipAll ? "r(101)" : "r(198)"));
  }
  // BUG-597: `predict z` with a variable z already in the data REPLACED it at
  // rc 0 -- the engine's predict writes its column by name, and set_f64_column
  // overwrites (measured on main: `generate double z = 7` / `predict z, xb`
  // left z holding the fitted values, rc 0). Stata's predict creates a NEW
  // variable and answers r(110) "variable z already defined", writing nothing.
  // Card first, datastore as the fallback (residentReadCard's contract), so a
  // dirty virtual head is not snapshotted just to answer a name question.
  {
    bool exists = false;
    if (const std::vector<VarInfo>* card = session.residentReadCard()) {
      for (const VarInfo& vi : *card) {
        if (vi.name == newvar) exists = true;
      }
    } else {
      Datastore* cur = nullptr;
      try {
        cur = session.datastore();
      } catch (const std::exception& e) {
        return engineErrorBlockM4(std::string("predict: ") + e.what());
      }
      exists = cur != nullptr && cur->has(newvar);
    }
    if (exists) return ojson(eval::engineError("variable " + newvar + " already defined"));
  }

  // BUG-516: predict computes on the LIVE frame and ADDS one column to it.
  //
  // `--data` used to be lm.dataPath -- the frame as it stood when the model was
  // FIT -- and the engine's output (that frame + newvar) was then adopted as the
  // session head. The first predict after a fit hid it (the head still WAS that
  // frame). Every later one silently REPLACED the user's data with the
  // estimation-time frame: `regress y x2 t` / `predict r1, residuals` /
  // `generate foo = 1` / `predict p2, xb` left a dataset with neither r1 nor
  // foo, at rc 0 (StataBE, w2_smalls.log section C: predict_twice rc 0, r1 still
  // summarizes over N 12). Rows dropped since the fit came BACK, a `replace` was
  // undone, a sort was reverted -- all the same adoption.
  //
  // So `--data` is the live head, always. What the fit needs rides separately:
  //  * head unmoved (the first predict): the historical argv, byte for byte.
  //  * head moved, no absorbed FE: the estimation-time sample goes in as
  //    `--fit-data`, the split BUG-163 built -- the engine fits THERE and
  //    evaluates the named coefficients over every live row, which is Stata's
  //    rule (current rows, current x values, the fitted coefficients).
  //  * head moved, absorbed FE: `--fit-data` refuses `--fe`, so the live frame
  //    is refit -- after fitColumnsUnchangedM4 has PROVED that is the same fit.
  //
  // BUG-163 (W:P5), unchanged: `--data` is the FULL dataset -- Stata's `predict`
  // writes a value for every row whose covariates are non-missing, not just the
  // rows the model was fitted on -- while the coefficients must come from the
  // FITTED sample (`--fit-data`/`--fit-keepif`).
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  const std::string liveHead = *session.currentHbin();
  // e(sample) T2: a wired fit leaves the head on its mask-install frame
  // (LastModel::esampleInstallPath) -- the fit's own frame plus the hidden
  // column, same rows and values -- so that head has not moved either.
  const bool headMoved = liveHead != lm.dataPath &&
                         (lm.esampleInstallPath.empty() || liveHead != lm.esampleInstallPath);
  // FU-475: lm.fitKeepif rides as `--fit-keepif` (with `--fit-data` once the
  // head moved), which cmds_postest.cpp cmd_predict routes through plan_keepif:
  // the restriction as a row mask after the operators (w2_esample.log:751-775
  // C6 e(N) 40) -- served. The ML refit takes no row mask, the native statistics
  // (computePredictNativeM4) read the refit's sample as a filtered frame, and
  // the engine refuses a restricted fit with absorbed effects (an engine error,
  // not r(198)): those keep the named refusal.
  const bool tsKeepif = postestTsKeepifRefusalM4("predict", lm).has_value();
  // FU-759: xb after an absorbed-effects fit (xtreg,fe / areg / reghdfe) is
  // served on an unmoved head -- the engine's predict takes --fe under the
  // --fit-keepif row mask over that same frame (w2_esample_tsrefit.log:251-263
  // T7 rc 0, r(N) 40, the mean of T1's regress xb). BUG-1259: so is every other
  // statistic of the family -- the engine writes the within residual on the fit's
  // rows (e, areg/reghdfe residuals and score), and computePredictNativeM4 builds
  // the rest from that xb and residual (StataBE 19.5 N2/e12.do `xtreg y L.x1 x2 if
  // t>=3, fe` u/e/xbu/stdp, `areg ...` d/xbd). A moved head refits the live frame
  // once fitColumnsUnchangedM4 has proved it is the fit's (the absorbed-FE rule).
  const bool tsFeXb = tsKeepif && !lm.fe.empty() && !glmPredict && family != PredictFamilyM4::Other &&
                      lm.fitDataPath.empty() &&
                      !(namesHiddenColumn(lm.fitKeepif) && liveHead != lm.dataPath);
  if ((glmPredict || nativeStat || !lm.fe.empty()) && !tsFeXb) {
    if (auto refused = postestTsKeepifRefusalM4("predict", lm)) return *refused;
  }
  std::vector<std::string> args = {"predict", "--data", liveHead, "--y", lm.y, "--x",
                                    lm.x.empty() ? "(intercept)" : joinCommaM4(lm.x)};
  // FU-768: the restriction a restricted absorbed fit is refit under, as a row
  // mask over the live frame. xtreg's route kept its `if` (lm.fitKeepif); areg and
  // reghdfe fit a materialized copy (lm.fitDataPath, runRegress), whose rows are
  // the model's e(sample) -- the mask its install put on the frame, which follows
  // the rows. No mask on the data: refused by name.
  std::string restrictedKeepif;
  if (restrictedAbsorbed) {
    if (!lm.fitKeepif.empty() && lm.fitDataPath.empty()) {
      restrictedKeepif = lm.fitKeepif;
    } else {
      const SessionState::EsampleReading er = session.esampleReading();
      if (er.kind != SessionState::EsampleReading::Kind::kColumn) {
        return engineErrorBlockM4(
            "predict after a restricted `" + predictFamilyCmdM4(family, lm) +
                "` fit is not supported here in this version of PIET: the fit's estimation "
                "sample is not on the data in memory. Nothing was written to your data.",
            "unsupported_option");
      }
      restrictedKeepif = std::string("(") + kEsampleColumn + "==" + std::to_string(er.serial) + ")";
    }
  }
  if (!headMoved) {
    if (restrictedAbsorbed) {
      args.push_back("--fit-keepif");
      args.push_back(restrictedKeepif);
    } else if (!lm.fitDataPath.empty()) {
      args.push_back("--fit-data");
      args.push_back(lm.fitDataPath);
      // BUG-1166: a by-prefixed time-series fit read its group's whole slice
      // and applied its `if` (a mask column on that slice) after the operators;
      // the refit does the same (cmd_predict restricts --fit-data by
      // --fit-keepif). Without it the refit used every row of the group.
      if (!lm.fitKeepif.empty()) {
        args.push_back("--fit-keepif");
        args.push_back(lm.fitKeepif);
      }
    } else if (!lm.fitKeepif.empty()) {
      // e(sample) T2: a --keepif that names the mask reads the cells the fit
      // read, under its predecessor's serial; the head is now this fit's install
      // frame, whose mask the install rewrote (0 rows: `[no_obs]` rc 1). The
      // fit's frame (lm.dataPath, kept by esampleInstallOnHead) goes along.
      if (namesHiddenColumn(lm.fitKeepif) && liveHead != lm.dataPath) {
        args.push_back("--fit-data");
        args.push_back(lm.dataPath);
      }
      args.push_back("--fit-keepif");
      args.push_back(lm.fitKeepif);
    }
  } else {
    std::string fitFile = lm.fitDataPath.empty() ? lm.dataPath : lm.fitDataPath;
    {
      // The refit reads the estimation-time sample AS A FILE. BUG-518: a
      // working frame that a resident mutation advanced before anything wrote
      // it to disk had none (`regress` / `generate` / `predict`: the fit-time
      // head only ever existed in the worker) -- SessionState::setLastModel now
      // pins that frame to disk at the fit, so this branch is the remainder:
      // a snapshot that failed (BUG-384's WARN), or an `estimates restore` of a
      // model whose scratch file reclaimSupersededScratch has since released
      // (exclusion (7) protects only the LIVE model's paths). Named here,
      // because the alternative -- fitting whatever the live frame now holds --
      // is a different model at rc 0.
      std::error_code fec;
      if (!std::filesystem::exists(cecon::to_native_path(fitFile), fec)) {
        return engineErrorBlockM4(
            "predict: the data `" + (lm.stataCmd.empty() ? lm.cobraCmd : lm.stataCmd) +
                "` was fitted on are no longer available to re-fit from in this version of "
                "PIET (the dataset changed before that working frame was written to disk). "
                "Re-run the estimation command, then predict. Nothing was written to your "
                "data.",
            "unsupported_option");
      }
    }
    const bool restrictedFit = !lm.fitDataPath.empty() || !lm.fitKeepif.empty();
    if (!lm.fe.empty() && (!restrictedFit || restrictedAbsorbed || tsFeXb)) {
      std::string why;
      // BUG-1259: after time-series operators the panel and time variables are
      // read by the lags too.
      std::vector<std::string> tsVars;
      if (tsFeXb) {
        if (session.panelvar.has_value()) tsVars.push_back(*session.panelvar);
        if (session.timevar.has_value()) tsVars.push_back(*session.timevar);
      }
      if (!fitColumnsUnchangedM4(session, lm, why, tsVars)) {
        return engineErrorBlockM4(
            "predict after a model with absorbed fixed effects is not supported once the "
                "model's variables or the observations have changed since the fit, in this "
                "version of PIET (" + why + "). The engine predicts an absorbed-effects "
                "model by re-fitting it, and re-fitting the changed data would be a "
                "different model. Re-run the estimation command, then predict. Nothing was "
                "written to your data.",
            "unsupported_option");
      }
      // Proven identical: fitting the live frame IS the fitted model. FU-768: a
      // restricted one refits under its restriction as a row mask over the live
      // frame (fitColumnsUnchangedM4 compared the columns an `if` reads too; the
      // e(sample) mask follows the rows).
      if (restrictedAbsorbed) {
        args.push_back("--fit-keepif");
        args.push_back(restrictedKeepif);
      } else if (tsFeXb) {
        // The fit's `if` as a row mask after the operators, over the live frame.
        args.push_back("--fit-keepif");
        args.push_back(lm.fitKeepif);
      }
    } else {
      // (A restricted fit WITH absorbed FE that is not served above lands here too
      // and is refused below by name: the engine refuses `--fit-data` with `--fe`.)
      if (tsKeepif) {
        // The fit's frame and its `if` go to the engine together, which applies
        // the `if` after the operators (cmd_predict --fit-data + --fit-keepif).
        args.push_back("--fit-keepif");
        args.push_back(lm.fitKeepif);
      } else if (!lm.fitKeepif.empty()) {
        // `--fit-keepif` filters `--data`, which is no longer the frame the fit
        // saw -- materialize the fitted sample instead (boottest's BUG-163 shape).
        try {
          auto ffr = piet::handlers::applyIfAndInFilter(session, fitFile, lm.fitKeepif,
                                                        std::nullopt);
          if (ffr.ok) fitFile = ffr.dataPath;
        } catch (const CobraError& ce) {
          return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
        }
      }
      args.push_back("--fit-data");
      args.push_back(fitFile);
    }
  }
  if (!lm.fe.empty() && std::find(args.begin(), args.end(), "--fit-data") != args.end()) {
    // FU-972: the engine cannot refit absorbed effects on a separate fit frame
    // (cmd_predict refuses --fit-data with --fe), and that refusal reached the
    // user as rc 1. Named here, r(198), before anything runs.
    return engineErrorBlockM4(
        "predict after `" + (lm.stataCmd.empty() ? lm.cobraCmd : lm.stataCmd) +
            "` with absorbed effects fitted on a restricted sample (an `in` range, or an `if` "
            "on data that changed since the fit) is not supported in this version of PIET. "
            "Nothing was written to your data.",
        "unsupported_option");
  }
  if (!lm.fe.empty()) {
    args.push_back("--fe");
    args.push_back(joinCommaM4(lm.fe));
    // e(sample) T2: the refit keeps the singleton groups the fit kept, or their
    // rows get no prediction (fitConventionFlagsM4).
    if (lm.keepSingletons) args.push_back("--keep-singletons");
  }
  // BUG-1067: the fit's collinear omission (fitConventionFlagsM4), so the refit's
  // coefficients are the posted e(b).
  for (const auto& f : fitConventionFlagsM4(lm))
    if (f == "--collinear-forward") args.push_back(f);
  // FU-131: part of the bare replay, so computePredictNativeM4's own refit (and
  // its cross-check against e(b)) is the no-constant model too.
  if (postestFitLacksConstantM4(lm)) args.push_back("--no-intercept");
  if (glmPredict) {
    // BUG-297: forward the estimator; the engine re-fits through
    // postest_fit_spec and writes Stata's default statistic (--pred) or the
    // linear index (--xb). wantResid can never be true here (refused above).
    args.push_back("--model");
    args.push_back(lm.cobraCmd);
    pushFitAsisM4(args, lm);
    if (!mlStat.empty()) {
      // BUG-1257: the replay only; the statistic is computed below.
    } else if (wantMlDefault) {
      args.push_back("--pred");
      args.push_back(newvar);
    } else {
      args.push_back("--xb");
      args.push_back(newvar);
    }
  } else if (nativeStat) {
    // BUG-596: `args` stays the bare replay (through --fe); the statistic is
    // computed below and the engine's refit only supplies its checks.
  } else if (wantXb) {
    args.push_back("--xb");
    args.push_back(newvar);
  } else {
    args.push_back("--resid");
    args.push_back(newvar);
  }

  // BUG-369's shape for predict: an `L.x` regressor needs the tsset context the
  // fit had (bare `predict` after `regress y L.x1 x2` was ts_no_tsset, r(111),
  // on main 2026-10-13).
  {
    std::vector<std::string> tsTokens = lm.x;
    tsTokens.insert(tsTokens.end(), lm.fe.begin(), lm.fe.end());
    tsTokens.push_back(lm.y);  // an operated depvar too (BUG-1277)
    if (translate::hasTsToken(tsTokens)) args = translate::injectTsContext(args, session);
  }
  std::string outPath = nextPathEst(session.workdir(), "predict");
  if (nativeStat || !mlStat.empty()) {
    std::vector<double> vals;
    if (weightedFit) {
      // BUG-1260 (nativeStat is set): no engine refit after a weighted fit.
      if (auto rej = computePredictWeightedM4(session, lm, family, predictStat, vals)) return *rej;
    } else if (nativeStat) {
      if (auto rej = computePredictNativeM4(session, lm, family, predictStat, args, vals)) return *rej;
    } else if (auto rej = computePredictMlM4(session, lm, mlStat, args, vals)) {
      return *rej;
    }
    if (auto rej = writePredictColumnM4(session, liveHead, newvar, vals, outPath, storeType)) return *rej;
  } else {
    storage::appendStoreTypeArg(args, storeType);
    args.push_back("--out");
    args.push_back(outPath);

    try {
      runCobraJsonM4(args);
    } catch (const CobraError& ce) {
      return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
    }
  }

  // BUG-517: `predict newvar if exp` fills ONLY the rows that satisfy exp; the
  // rest are missing (StataBE, aer_b4/predict-e-xbd.log C_if_half: `predict
  // double P_e_half if half==1, e` -> "(135 missing values generated)", rc 0).
  // The qualifier used to be accepted and DROPPED -- runPredict never saw it --
  // so every row was filled at rc 0. Masked here, on the engine's output and
  // BEFORE it is adopted, so a bad expression leaves the user's data untouched
  // (Stata creates no variable on that error either). Same cond() fold `generate
  // ... if` uses (handlers.cpp runGen), so the two agree on a missing condition.
  if (ifExpr.has_value() && !ifExpr->empty()) {
    const std::string maskExpr = newvar + "=cond((" + *ifExpr + ")," + newvar + ",.)";
    const std::string maskedPath = nextPathEst(session.workdir(), "predict_if");
    std::vector<std::string> maskArgs = {"eval", "--data", outPath, "--gen", maskExpr,
                                         "--out", maskedPath};
    storage::appendStoreTypeArg(maskArgs, storeType);  // the column keeps its type
    try {
      runCobraJsonM4(translate::injectTsContextForExpr(maskArgs, maskExpr, session));
    } catch (const CobraError& ce) {
      cecon::native_unlink(outPath);
      return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
    }
    cecon::native_unlink(outPath);  // never a head: nothing else can name it
    outPath = maskedPath;
  }

  session.load(outPath);
  session.invalidateDatastore();
  // BUG-598: the new column's storage type, as `: type`/describe and the .dta
  // writer read it -- the type the values were stored as (BUG-924 above), recorded
  // by generate's rule (storage::recordCreateType): `predict double` -> double
  // (w2b_estim.log predict_double), reghdfe -> double (predict-e-xbd.log type_xbd),
  // `set type double` -> double (w2aa a_p_settype_double), untyped under the
  // factory default -> BUG-295's implicit float plane.
  storage::recordCreateType(session, newvar, declaredType, storeType);
  // reghdfe_p labels its default as its xb (BUG-1508 below).
  if (opts.empty() && postestOlsCmdsM4().count(lm.cobraCmd) && lm.fe.empty() &&
      lm.stataCmd != "reghdfe") {
    session.varMeta[newvar].label = "Fitted values";
    session.syncEvalVarLabels();
  } else {
    // BUG-1260/BUG-1508: the labels the predict programs give (StataBE 19.5
    // scratch-laneA/agents/p-margins/s6.do, file-new-1/i27.do and scratch-laneA2/
    // agents/post/pl.do): _predict's own for xb, stdp, stdf, stdr, hat (leverage),
    // cooksd, rstandard and rstudent; "Residuals" for residuals and score;
    // xtrefe_p.ado's u/e family; areg_p's "Xb" default and the `d[absvar]` family,
    // reghdfe_p's over e(absvars) (its dresiduals unlabelled); regre_p2.ado CalcE for
    // e(a,b); logit/probit/poisson's pr, n and xb ("Linear prediction (log odds)"
    // after logit and logistic).
    std::string label;
    const std::string& s = predictStat;
    const std::string dv = lm.depvar.empty() ? lm.y : lm.depvar;
    if (glmPredict) {
      // In the order the engine argv above reads the two flags.
      if (wantMlDefault)
        label = lm.cobraCmd == "poisson" ? "Predicted number of events" : "Pr(" + dv + ")";
      else if (wantXb)
        label = lm.cobraCmd == "logit" ? "Linear prediction (log odds)" : "Linear prediction";
      // BUG-1257's stdp/score/ir (StataBE 19.5, 2026-11-03, auto.dta after logit,
      // probit, poisson, logistic and cloglog): "S.E. of the prediction",
      // "equation-level score from logit|probit|poisson|logistic|cloglog",
      // "Predicted incidence rate".
      else if (mlStat == "stdp")
        label = "S.E. of the prediction";
      else if (mlStat == "score")
        label = "equation-level score from " +
                (lm.stataCmd == "logistic" ? lm.stataCmd : lm.cobraCmd);  // also cloglog
      else if (mlStat == "ir")
        label = "Predicted incidence rate";
    } else if (family == PredictFamilyM4::Other) {
      if (lm.stataCmd == "reghdfe") label = wantXb ? "Linear prediction" : "Residuals";  // noabsorb
    } else if (s.rfind("e(", 0) == 0) {
      const std::string in = s.substr(2, s.size() - 3);
      const size_t comma = in.find(',');
      const std::string lo = trimM4(in.substr(0, comma)), hi = trimM4(in.substr(comma + 1));
      if (lo == "." && hi == ".") label = "E(" + dv + ")";
      else if (lo == ".") label = "E(" + dv + "|" + dv + "<" + hi + ")";
      else if (hi == ".") label = "E(" + dv + "|" + dv + ">" + lo + ")";
      else label = "E(" + dv + "|" + lo + "<" + dv + "<" + hi + ")";
    } else if (family == PredictFamilyM4::Regress) {
      static const std::map<std::string, std::string> kRegressLabels = {
          {"xb", "Linear prediction"},
          {"residuals", "Residuals"},
          {"score", "Residuals"},
          {"stdp", "S.E. of the prediction"},
          {"stdf", "S.E. of the forecast"},
          {"stdr", "S.E. of the residual"},
          {"hat", "Leverage"},
          {"cooksd", "Cook's D"},
          {"rstandard", "Standardized residuals"},
          {"rstudent", "Studentized residuals"}};
      const auto it = kRegressLabels.find(s);
      if (it != kRegressLabels.end()) label = it->second;
    } else if (family == PredictFamilyM4::Areg && !lm.fe.empty()) {
      const std::string& a = lm.fe.front();
      if (opts.empty()) label = "Xb";
      else if (s == "xbd") label = "Xb + d[" + a + "]";
      else if (s == "d") label = "d[" + a + "]";
      else if (s == "dresiduals") label = "d[" + a + "] + residual";
      else if (s == "residuals" || s == "score") label = "Residuals";
      else if (s == "xb") label = "Linear prediction";
      else if (s == "stdp") label = "S.E. of the prediction";
    } else if (family == PredictFamilyM4::XtregFe) {
      const std::string iv = lm.ivar.has_value() && !lm.ivar->empty()
                                 ? *lm.ivar
                                 : (lm.fe.empty() ? std::string() : lm.fe.front());
      if (s == "e") label = "e[" + iv + ",t]";
      else if (s == "u") label = "u[" + iv + "]";
      else if (s == "xbu") label = "Xb + u[" + iv + "]";
      else if (s == "ue") label = "u[" + iv + "] + e[" + iv + ",t]";
      else if (s == "xb") label = "Linear prediction";
      else if (s == "stdp") label = "S.E. of the prediction";
      else if (s == "residuals") label = "Residuals";
    } else if (family == PredictFamilyM4::Reghdfe && !lm.fe.empty()) {
      std::string a;
      for (const auto& f : lm.fe) a += (a.empty() ? "" : " ") + f;
      if (s == "xbd") label = "Xb + d[" + a + "]";
      else if (s == "d") label = "d[" + a + "]";
      else if (s == "residuals" || s == "score") label = "Residuals";
      else if (s == "xb") label = "Linear prediction";
      else if (s == "stdp") label = "S.E. of the prediction";
    }
    if (!label.empty()) {
      session.varMeta[newvar].label = label;
      session.syncEvalVarLabels();
    }
  }
  Datastore* ds = session.datastore();

  // C1-CARD-REBUILD-REDUCE A11 sibling: `predict` (engine `predict --xb/--resid`)
  // appends only `newvar` and byte-preserves every other column, so seed the
  // unchanged columns' missing-counts (action "gen" pure-ADD, newVars={newvar})
  // BEFORE the varlist rebuild below skips each column's O(nrows) nMissing scan.
  // Byte-identical vs PIET_NO_NMISS_CACHE=1.
  session.seedNMissForPureAdd(ds, "gen", {newvar}, /*droppedVars=*/{}, /*rewrittenVars=*/{});

  ojson out;
  out["type"] = "data_mutation";
  out["action"] = "gen";
  ojson varlistAfter = ojson::array();
  auto vlist = session.varlistWithMeta(ds);
  for (const auto& v : vlist) varlistAfter.push_back(v.toJson());
  session.updateNMissCache(vlist);  // C2-F13a: keep the cache current for the next mutation
  out["varlist_after"] = varlistAfter;
  out["n_rows_after"] = ds ? ds->nRows() : 0;
  out["new_vars"] = ojson::array({newvar});
  return out;
}

// ===========================================================================
// estat (hettest/ovtest/vif/dwatson/bgodfrey/archlm/durbinalt diagnostic
// rewrite; endogenous/overid/firststage IV diagnostics (BS-082, BETA D5
// batch); simple/event/calendar/group/pretrend csdid aggregate readback)

// `estat event, post` — mirror dispatcher.py's WS-D: post e(b)/e(V) from
// cobra's `estat_event` block (Stata Pre_avg/Post_avg/Tm#/Tp# names, full E×E
// covariance) into session.eval.eMatrices so `matrix b = e(b)` / `matrix V =
// e(V)` read the posted event-study coefficients. Any later estimation
// overwrites them (matched by name).
void postCsdidEventEMatrices(SessionState& session, const ojson& raw) {
  if (!raw.contains("estat_event") || !raw["estat_event"].is_object()) return;
  const ojson& ee = raw["estat_event"];
  auto numOf = [](const ojson& x) -> double {
    return x.is_null() ? std::numeric_limits<double>::quiet_NaN() : x.get<double>();
  };
  auto setE = [&session](const std::string& nm, eval::EvalMatrix m) {
    for (auto& p : session.eval.eMatrices) {
      if (p.first == nm) { p.second = std::move(m); return; }
    }
    session.eval.eMatrices.emplace_back(nm, std::move(m));
  };
  if (ee.contains("e_b") && ee["e_b"].is_object()) {
    const ojson& eb = ee["e_b"];
    eval::EvalMatrix b;
    for (const auto& n : eb.value("names", ojson::array())) b.colnames.push_back(n.get<std::string>());
    b.rownames = {"y1"};
    std::vector<double> vals;
    for (const auto& x : eb.value("values", ojson::array())) vals.push_back(numOf(x));
    b.data.push_back(std::move(vals));
    setE("b", std::move(b));
  }
  if (ee.contains("e_V") && ee["e_V"].is_object()) {
    const ojson& eV = ee["e_V"];
    eval::EvalMatrix V;
    for (const auto& n : eV.value("names", ojson::array())) V.colnames.push_back(n.get<std::string>());
    V.rownames = V.colnames;
    for (const auto& row : eV.value("matrix", ojson::array())) {
      std::vector<double> r;
      for (const auto& x : row) r.push_back(numOf(x));
      V.data.push_back(std::move(r));
    }
    setE("V", std::move(V));
  }
}

ojson runEstat(const std::string& subcmdIn, const std::vector<std::string>& rawVarlistAfterSub,
               const std::vector<std::pair<std::string, eval::OptValue>>& opts,
               const std::optional<std::string>& ifExpr, SessionState& session) {
  std::string sub = lowerM4(subcmdIn);
  // BUG-1248: estat.ado refuses an empty e(cmd) before it reads its subcommand:
  // with no estimates every `estat` -- a diagnostic with an explicit varlist, a
  // csdid aggregate, an unknown subcommand, none at all -- is r(301) "last
  // estimates not found" (StataBE 19.5; noPreviousEstimation's r(301) here). It
  // answered r(198), r(199) or rc 1. A posting with no command name behind it
  // (`ereturn post b V`) is the same empty e(cmd): `estat ic`, `estat vce`, bare
  // `estat` are r(301) there too.
  const std::string* eCmd = session.eval.eStrings.find("cmd");
  if (!session.eval.eLive() || (session.ePostedLive && (eCmd == nullptr || eCmd->empty())))
    return noPreviousEstimation();

  static const std::map<std::string, std::string> kEstatToDiag = {
      {"hettest", "hettest"}, {"ovtest", "ovtest"},     {"vif", "vif"},
      {"dwatson", "dwatson"}, {"bgodfrey", "bgodfrey"}, {"archlm", "archlm"},
      // BS-021 (BETA D5 batch) fix: `estat durbinalt` was previously
      // ALIASED onto cobra's `bgodfrey` command — a mislabeled WRONG test
      // (Breusch-Godfrey LM, not Durbin's alternative test). The engine
      // has its OWN dedicated `durbinalt` CLI (`cobra durbinalt --y --x
      // [--lags "N"] [--small] [--robust] [--nomiss0]`), live-verified
      // against oracle D5b: bare (chi2=372.934, df=1) and `, small
      // lags(2)` (F=189.005, df=(2,331)) both byte-for-byte engine
      // matches — routed to its own diagCmd now, with its own known-
      // options set (small/lags) below, rather than sharing the empty
      // known-set every other row in this map uses.
      {"durbinalt", "durbinalt"},
  };
  auto diagIt = kEstatToDiag.find(sub);
  if (diagIt != kEstatToDiag.end()) {
    std::string diagCmdPeek = diagIt->second;
    // BUG-1222: estat hettest is regress's (regress_estat). After any other
    // estimator StataBE answers r(321) whatever the varlist -- "estat hettest not
    // valid" after areg, xtreg fe/re, ivregress, newey, prais, logit, probit and
    // poisson, "invalid subcommand hettest" from reghdfe's own estat -- and after
    // a robust or clustered regress r(498) (StataBE 19.5, scratch-laneA/agents/N1
    // b2.do, p-margins s3.do/s4.do; `[aw]` and `, rhs` are served). The z-list
    // form used to test a plain-OLS refit at rc 0 after all of them.
    if (diagCmdPeek == "hettest") {
      const std::string* ecmd = session.eval.eStrings.find("cmd");
      if (ecmd != nullptr && !ecmd->empty() && *ecmd != "regress" && *ecmd != "anova") {
        return ojson(eval::errorBlock(
            "engine_error", *ecmd == "reghdfe" ? "invalid subcommand hettest" : "estat hettest not valid",
            std::nullopt, std::nullopt, std::nullopt, "r(321)"));
      }
      const std::string* evce = session.eval.eStrings.find("vce");
      if (ecmd != nullptr && *ecmd == "regress" && evce != nullptr &&
          (*evce == "robust" || *evce == "cluster")) {
        return ojson(eval::errorBlock(
            "engine_error", "hettest not appropriate after estimation using robust or clustered VCE",
            std::nullopt, std::nullopt, std::nullopt, "r(498)"));
      }
    }
    // BS-097: the hettest/ovtest/vif/dwatson/bgodfrey/archlm diagnostic
    // branch reads NO options at all today (real Stata has e.g. `vif,
    // uncentered` / `bgodfrey/archlm, lags(#)` — none implemented here) —
    // known-set is the empty set. `durbinalt` (BS-021, BETA D5 batch) is
    // the ONE exception: real Stata's `estat durbinalt [, small lags(#)]`
    // genuinely has two options, and cobra's OWN durbinalt CLI accepts
    // both — its known-set is {small, lags} instead. Branch-scoped
    // deliberately: the csdid sub-branch below (`estat event, window(...)
    // post`) has its OWN, non-empty known set, so a single net at function
    // entry would either wrongly reject `window`/`post`/`small`/`lags`
    // here or wrongly accept them for the WRONG branch — neither is
    // correct, hence one check per branch.
    // BUG-233: the one copy of this vocabulary now lives in the file-scope
    // registration block (see top of file); this binding keeps every existing
    // use — including the reject net below — reading the same set.
    const std::unordered_set<std::string>& kDurbinaltKnownOpts = kVocabEstatDurbinalt.known;
    if (auto err = eval::rejectUnknownOptions(
            opts, diagCmdPeek == "durbinalt" ? kDurbinaltKnownOpts
                                             : std::unordered_set<std::string>{})) {
      return *err;
    }
    if (!session.currentHbin().has_value()) return eval::noDataLoaded();
    std::string diagCmd = diagIt->second;
    // FU-504: cobra's diagnostics (cmds_diag.cpp) read only --y/--x and refit
    // least squares WITH a constant; they have no --no-intercept. After
    // `regress y x1 x2, noconstant` the fallback below (and hettest's z-list
    // path, which keeps the fit's y/x) therefore tested the with-constant model
    // at rc 0: `estat hettest` printed the same F/chi2 as after `regress y x1 x2`.
    // No oracle captures these diagnostics after a no-constant fit, so refuse by
    // name (r(198)) on the two paths that take y/x from the last model. An
    // explicit `estat <diag> y x1 x2` still runs as typed.
    auto noConstantDiagReject = [&sub](const LastModel& lm) {
      const std::string fitCmd = lm.stataCmd.empty() ? lm.cobraCmd : lm.stataCmd;
      return engineErrorBlockM4(
          "estat " + sub + " after `" + fitCmd +
              " ..., noconstant` is not supported in this version of PIET -- the engine's "
              "diagnostic refits the model with a constant, so it would test a different model "
              "than the one fitted. Refusing rather than reporting a statistic that is not the "
              "one asked for.",
          "unsupported_option");
    };
    std::vector<std::string> varlist = rawVarlistAfterSub;
    // BS-024 (BETA Phase C batch C1): captured BEFORE the fallback below
    // (which appends lastModel's y/x into `varlist` when the user gave
    // none) — distinguishes "the user actually typed a varlist" from "we
    // fell back to lastModel", needed by the hettest-specific y/xs split
    // further down.
    bool userGaveVarlist = !varlist.empty();
    if (varlist.empty()) {
      const auto& lmOpt = session.lastModel();
      if (lmOpt.has_value() && !lmOpt->y.empty() && !lmOpt->x.empty()) {
        // BS-013 (BETA A4): the diagnostic family has NO --fe flag on
        // cobra's own CLI at all (verified: apps/cmds/cmds_diag.cpp's
        // cmd_hettest/cmd_ovtest/cmd_vif/cmd_dwatson/cmd_durbinalt/
        // cmd_bgodfrey/cmd_archlm each read only --y/--x/--keepif — there is
        // no fixed-effects parameter to forward even in principle). Running
        // a plain-OLS diagnostic against a lastModel that actually had fixed
        // effects would silently answer a DIFFERENT question than the one
        // just estimated — honest-reject instead (this codebase's
        // established "silent-wrong -> honest reject" idiom, e.g. BS-023/
        // BS-052) rather than fabricate FE support the engine doesn't have.
        // Only the varlist-FALLBACK path is affected: an explicit
        // `estat hettest y x1 x2` still runs exactly as the user asked.
        if (!lmOpt->fe.empty()) {
          return engineErrorBlockM4(
              "estat " + sub + ": not supported after a fixed-effects model in this version of "
              "PIET (the fixed-effects structure cannot be forwarded to this diagnostic). Pass "
              "an explicit varlist instead, e.g. `estat " + sub + " y x1 x2`.",
              "unsupported_option");
        }
        // BUG-204's remaining half, and the same argument one line up. These
        // diagnostics are OLS constructs: cobra's cmd_hettest/cmd_ovtest/cmd_vif/
        // ... read only --y/--x and refit least squares. After an ML fit the
        // varlist fallback therefore answers a DIFFERENT question than the one
        // just estimated — measured: `estat hettest` after `logit` returned
        // F .0064338 / chi2 .0025107 with rc 0, while Stata rejects it outright.
        // Same shape as BUG-203/BUG-297, whose `predict` half is already closed:
        // an OLS refit smuggled in behind a non-OLS fit. Honest reject, and only
        // on the FALLBACK path — an explicit varlist still runs as asked.
        static const std::set<std::string> kOlsLikeForDiag = {
            "feols", "regress", "reghdfe", "ivregress", "ivreghdfe", "areg", "xtreg", "newey",
            "prais", "reg3",
        };
        if (!lmOpt->cobraCmd.empty() && kOlsLikeForDiag.count(lmOpt->cobraCmd) == 0) {
          return engineErrorBlockM4(
              "estat " + sub + ": not supported after `" + lmOpt->cobraCmd + "` — this diagnostic "
              "is a least-squares construct and PIET would refit OLS behind your model, answering "
              "a different question. Stata rejects it here too. Pass an explicit varlist if you "
              "mean the OLS diagnostic, e.g. `estat " + sub + " y x1 x2`.",
              "unsupported_option");
        }
        if (postestFitLacksConstantM4(*lmOpt)) return noConstantDiagReject(*lmOpt);
        varlist.push_back(lmOpt->y);
        for (const auto& x : lmOpt->x) varlist.push_back(x);
      }
    }
    if (varlist.empty()) {
      return engineErrorBlockM4("estat " + sub + ": no variables — run a regression first or " +
                                "pass a varlist explicitly.");
    }
    std::string y;
    std::vector<std::string> xs;
    std::vector<std::string> hettestZ;  // BS-024: explicit aux vars → cobra's --z
    if (userGaveVarlist && diagCmd == "hettest") {
      // BS-024: `estat hettest z1 z2` — z1/z2 are the AUXILIARY explanatory
      // variables for the Breusch-Pagan test (real Stata: "estat hettest
      // [varlist] ... = BP using z1 z2 as explanatory vars"), NOT a
      // replacement (y, x) pair for the base regression. Previously this
      // arm applied the SAME varlist[0]-is-y / rest-is-x split used by the
      // lastModel FALLBACK path above to an EXPLICIT user varlist too —
      // e.g. `estat hettest spirits unemp` silently became y=spirits,
      // x=unemp (dropping "spirits" as a test regressor entirely and
      // testing an unrelated income~unemp model instead of the actually-
      // fitted income~beertax one), which is why the varlist form's chi2
      // came back with the wrong df and the wrong value (df=1 off one
      // dropped variable, instead of Stata's real df=2 using both). The
      // base regression's y AND x must stay the LAST-FITTED model's (the
      // residuals under test come from income~beertax, not a refit); the
      // user varlist maps to cobra hettest's dedicated `--z Z1,Z2` flag
      // (aux explanatory vars for the variance regression) — NOT to --x,
      // which would silently refit the base model on the aux vars (the
      // first attempt's bug: chi2 came back from the wrong base model).
      const auto& lmOptHettest = session.lastModel();
      if (!lmOptHettest.has_value() || lmOptHettest->y.empty()) {
        return engineErrorBlockM4(
            "estat hettest: no variables — run a regression first or pass a varlist "
            "explicitly.");
      }
      if (postestFitLacksConstantM4(*lmOptHettest)) return noConstantDiagReject(*lmOptHettest);
      y = lmOptHettest->y;
      xs = lmOptHettest->x;
      hettestZ = varlist;
    } else {
      y = varlist[0];
      xs = std::vector<std::string>(varlist.begin() + 1, varlist.end());
    }
    std::vector<std::string> args = {diagCmd, "--y", y};
    if (!xs.empty()) {
      args.push_back("--x");
      args.push_back(joinCommaM4(xs));
    }
    if (!hettestZ.empty()) {
      args.push_back("--z");
      args.push_back(joinCommaM4(hettestZ));
    }
    // BS-021 (BETA D5 batch): durbinalt's own small/lags() options -> cobra
    // durbinalt's --small/--lags. Oracle D5b: bare = chi2 form (no --small,
    // no --lags — matches the engine's own bare default of lag order 1);
    // `, small lags(2)` = F form with lag order 2 (engine's `--lags "2"`
    // means "test lag orders 1 through 2", i.e. Stata's own lags(#) max-
    // order semantics — live-verified NOT a literal single-lag selector).
    if (diagCmd == "durbinalt") {
      bool small = false;
      std::optional<std::string> lagsVal;
      for (const auto& [k, v] : opts) {
        if (k == "small" && v.isBool && v.boolValue) small = true;
        if (k == "lags" && !v.isBool) lagsVal = v.strValue;
      }
      if (small) args.push_back("--small");
      if (lagsVal.has_value() && !trimM4(*lagsVal).empty()) {
        args.push_back("--lags");
        args.push_back(trimM4(*lagsVal));
      }
    }
    if (ifExpr.has_value()) {
      args.push_back("--keepif");
      args.push_back(*ifExpr);
    }
    args.push_back("--data");
    args.push_back(*session.currentHbin());
    args.push_back("--tidy");
    ojson raw;
    try {
      raw = runCobraJsonM4(args);
    } catch (const CobraError& ce) {
      return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
    }
    ojson diagBlock = cobraToEstimationM4(raw, diagCmd, &session);
    // BUG-1496: hettest and ovtest are r-class and replace r() with their test
    // alone (StataBE 19.5, scratch-laneA/agents/file-new-1/i14.do, rlife/h1s.do:
    // `return list` after `estat hettest` holds exactly r(p), r(df), r(chi2), the
    // regress's r(table) and r(level) gone; after `estat ovtest` r(p), r(F), r(df)
    // (the numerator df), r(df_r)). Posted in reverse of that listing, which
    // `return list` prints newest first.
    if (diagCmd == "hettest" || diagCmd == "ovtest") {
      auto num = [&raw](const char* k) {
        return raw.contains(k) && raw[k].is_number() ? raw[k].get<double>()
                                                     : std::numeric_limits<double>::quiet_NaN();
      };
      const std::vector<std::pair<std::string, double>> posted =
          diagCmd == "hettest"
              ? std::vector<std::pair<std::string, double>>{{"chi2", num("chi2")},
                                                            {"df", num("df")},
                                                            {"p", num("p")}}
              : std::vector<std::pair<std::string, double>>{{"df_r", num("df_r")},
                                                            {"df", num("df_m")},
                                                            {"F", num("F")},
                                                            {"p", num("p")}};
      eval::replaceRClass(session.eval);
      std::vector<std::string> names;
      for (const auto& [k, v] : posted) {
        session.eval.rValues.set(k, v);
        names.push_back(k);
      }
      eval::declareRPosted(session.eval, "estat", std::move(names));
    }
    return diagBlock;
  }

  // estat endogenous / overid / firststage (BS-082, BETA D5 batch; BETA
  // Phase G REWIRE): IV post-estimation diagnostics after `ivregress
  // 2sls`/`ivreghdfe` (BS-063, BETA D1). Oracle D5c: `ivregress 2sls income
  // emppop (beertax = spirits unemp)` (bare, no vce()) then `estat
  // endogenous`/`estat overid`/`estat firststage`.
  //
  // estat endogenous (Durbin score chi2 / Wu-Hausman F) / estat overid
  // (Sargan chi2 / Basmann): the D5 batch's own honest-reject above was
  // correct about `cobra iv --tidy`'s OWN payload (no durbin/wu-hausman
  // field of any kind; its one overidentification field, hansen_j, is a
  // DIFFERENT GMM-weighted J statistic, absent entirely under the model's
  // default iid vcov) — but wrong that the engine has "no such test at
  // all": a SEPARATE cobra CLI command, `cobra iv-diag` (hyphenated —
  // apps/cmds/cmds_diag.cpp::cmd_ivdiag on the cobra side, NOT part of
  // `cobra iv`'s own --tidy output), computes exactly this classical-iid
  // Durbin/Wu-Hausman/Sargan/Basmann family directly from the raw
  // data/varlist (`--data --y --endog --instruments [--x] [--keepif]`),
  // independent of whatever `cobra iv --tidy` did or didn't emit. This
  // branch now re-invokes it, reconstructing the endog/exog/instruments
  // split from LastModel's ivEndog/ivExog/ivInstruments (session_state.h;
  // `lm.x` alone conflates endog+exog and cannot be un-mixed).
  //
  // iv-diag is classical/iid-only (no --vcov flag on its CLI at all,
  // src/estimators/iv_diagnostics.h's own doc comment: "Robust/HAC
  // (Wooldridge) ... not implemented") — mirrors real Stata's own
  // vce()-dependent test-family switch (a non-default vce() makes Stata
  // itself compute a DIFFERENT robust score test). Forwarding the
  // classical statistic after a robust/clustered fit would silently answer
  // a different question than the one actually estimated, so this is
  // gated on lm.vcov=="iid" (LastModel's own default) and honestly rejects
  // otherwise, rather than fabricating a robust-form endogeneity/overid
  // test the engine does not have.
  //
  // estat overid specifically: iv-diag's own JSON either carries a real
  // {"sargan","sargan_p","basmann","basmann_p","df"} block (m > p1,
  // overidentified) or a `{"note": "just-identified ..."}` placeholder (m
  // == p1) — real Stata itself says "not overidentified" rather than
  // fabricating a stat in the exactly-identified case; this mirrors that
  // rather than inventing zeros. NOTE (basmann's actual distribution): the
  // engine's OWN implementation computes r.basmann_p via chi2_pvalue(...,
  // overid_df) — the SAME df and distribution family as Sargan, not an
  // F(df1,df2) the way real Stata's Basmann is traditionally described —
  // so this renders it labeled "Basmann chi2" (what the engine actually
  // computes), not "Basmann F" (what real Stata's test is traditionally
  // named), to avoid asserting a distribution/df pair this engine build
  // does not actually use.
  //
  // estat firststage (UNCHANGED from the D5 batch): the engine's
  // `first_stage_F`/`first_stage_F_iid` (per-endogenous-regressor dict) IS
  // present under every --vcov tried and matches the oracle's
  // F(2,332)=2.56678 byte-for-byte — this still reads it straight off the
  // STORED raw `cobra iv --tidy` payload (session.lastModel()->result)
  // rather than re-running cobra, no iv-diag involved. Real Stata's own
  // firststage table also reports R-sq/Adjusted R-sq/Partial R-sq per
  // regressor plus a minimum-eigenvalue statistic and a Stock-Yogo weak-
  // instrument critical-value table — NONE of these appear anywhere in the
  // engine's iv payload (live-verified against the full key list) and
  // Prob>F itself is not computed engine-side either (no F-distribution CDF
  // exists anywhere in this codebase to compute it PIET-side without
  // fabricating a value) — so this renders ONLY the F statistic per
  // endogenous regressor (oracle-matched), leaving every other column
  // absent rather than fabricated (same "show what's real, never fabricate
  // the rest" precedent as reg3's r2_within/dof_resid null fallback,
  // cobraToEstimationM4 above).
  if (sub == "endogenous" || sub == "overid" || sub == "firststage") {
    if (auto err = eval::rejectUnknownOptions(opts, {})) return *err;
    const auto& lmOptIv = session.lastModel();
    if (!lmOptIv.has_value() || lmOptIv->cobraCmd != "iv" || !lmOptIv->result.has_value()) {
      return engineErrorBlockM4(
          "estat " + sub + ": no `ivregress 2sls`/`ivreghdfe` result in memory. Run an IV "
          "regression first.",
          "unsupported_option");
    }
    const ojson& ivRaw = *lmOptIv->result;
    if (sub == "endogenous" || sub == "overid") {
      if (lmOptIv->vcov != "iid") {
        return engineErrorBlockM4(
            "estat " + sub + ": not supported after a non-default vce() in this version of PIET "
            "— the engine's iv-diag test (Durbin/Wu-Hausman/Sargan/Basmann) is classical/iid-only; "
            "a robust or clustered fit would need a robust score test this engine does not "
            "implement. Re-run `ivregress 2sls`/`ivreghdfe` with the default vce() to use estat " +
                sub + ".",
            "unsupported_option");
      }
      std::vector<std::string> diagArgv = {"iv-diag", "--y", lmOptIv->y, "--endog",
                                            joinCommaM4(lmOptIv->ivEndog), "--instruments",
                                            joinCommaM4(lmOptIv->ivInstruments)};
      if (!lmOptIv->ivExog.empty()) {
        diagArgv.push_back("--x");
        diagArgv.push_back(joinCommaM4(lmOptIv->ivExog));
      }
      if (ifExpr.has_value()) {
        diagArgv.push_back("--keepif");
        diagArgv.push_back(*ifExpr);
      }
      diagArgv.push_back("--data");
      diagArgv.push_back(lmOptIv->dataPath);
      ojson diagRaw;
      try {
        diagRaw = runCobraJsonM4(diagArgv);
      } catch (const CobraError& ce) {
        return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
      }
      if (sub == "endogenous") {
        if (!diagRaw.contains("endogeneity") || !diagRaw["endogeneity"].is_object()) {
          return engineErrorBlockM4(
              "estat endogenous: the engine's iv-diag call returned no endogeneity block.");
        }
        const ojson& e = diagRaw["endogeneity"];
        ojson block;
        block["type"] = "estimation";
        block["estimator"] = "estat_endogenous";
        block["durbin_chi2"] = e.value("durbin", 0.0);
        block["durbin_df"] = e.value("durbin_df", 0);
        block["durbin_p"] = e.value("durbin_p", 0.0);
        block["wu_hausman_F"] = e.value("wu_hausman", 0.0);
        block["wu_hausman_df"] = e.value("wu_df", 0);
        block["wu_hausman_df_r"] = e.value("wu_df_r", 0.0);
        block["wu_hausman_p"] = e.value("wu_p", 0.0);
        return block;
      }
      // overid
      if (!diagRaw.contains("overid") || !diagRaw["overid"].is_object()) {
        return engineErrorBlockM4(
            "estat overid: the engine's iv-diag call returned no overid block.");
      }
      const ojson& o = diagRaw["overid"];
      ojson block;
      block["type"] = "estimation";
      block["estimator"] = "estat_overid";
      if (o.contains("note")) {
        // BUG-187 (P2-PERF ARC-P). Exactly identified (instrument count ==
        // endogenous-regressor count): there is no overidentifying restriction
        // to test, and we never fabricated a statistic for it — the TEXT was
        // always honest. The RETURN CODE was not. Stata answers r(498), and
        // its convention is that an unavailable test is an ERROR, so do-files
        // are written as `capture estat overid` / `if _rc == 0 { use the
        // Sargan p }`. Returning a note with rc 0 sends that branch into
        // statistics which were never produced — the human reading the
        // transcript learned the right thing and the program did not.
        //
        // The message is carried unchanged; only its channel moves.
        return eval::errorBlock(
            "engine_error",
            o.value("note",
                    std::string("model is exactly identified: no overidentifying restrictions to "
                                 "test (instrument count equals endogenous-regressor count).")),
            std::nullopt, std::nullopt, std::nullopt, "r(498)");
      }
      block["sargan_chi2"] = o.value("sargan", 0.0);
      block["sargan_df"] = o.value("df", 0);
      block["sargan_p"] = o.value("sargan_p", 0.0);
      // "Basmann chi2", not "Basmann F" — see the header comment above:
      // this engine build's own r.basmann_p is a chi2_pvalue(...,
      // overid_df) call, the SAME distribution/df as Sargan, not an
      // F(df1,df2) the way real Stata's Basmann is traditionally named.
      block["basmann_chi2"] = o.value("basmann", 0.0);
      block["basmann_df"] = o.value("df", 0);
      block["basmann_p"] = o.value("basmann_p", 0.0);
      return block;
    }
    // firststage
    if (!ivRaw.contains("first_stage_F") || !ivRaw["first_stage_F"].is_object() ||
        ivRaw["first_stage_F"].empty()) {
      return engineErrorBlockM4(
          "estat firststage: the stored IV result has no first-stage F statistic (re-run the "
          "ivregress/ivreghdfe fit).",
          "unsupported_option");
    }
    ojson block;
    block["type"] = "estimation";
    block["estimator"] = "estat_firststage";
    ojson rows = ojson::array();
    for (auto it = ivRaw["first_stage_F"].begin(); it != ivRaw["first_stage_F"].end(); ++it) {
      ojson row;
      row["name"] = it.key();
      row["F"] = it.value();
      rows.push_back(row);
    }
    block["firststage"] = rows;
    block["note"] =
        "First-stage F only (oracle-matched) — R-sq/Adjusted R-sq/Partial R-sq/Prob>F/minimum-"
        "eigenvalue/weak-instrument critical values are not computed by the engine and are not "
        "shown (not fabricated).";
    return block;
  }

  // csdid/DiD aggregate.
  if (sub.empty()) {
    return engineErrorBlockM4(
        "estat (no subcommand) — try `estat simple`, `estat event`, "
        "`estat pretrend`, `estat calendar`, or `estat group` after csdid.",
        "unsupported_command");
  }
  const auto& lmOpt = session.lastModel();
  if (sub == "ic" && session.eval.eStrings.get("cmd") == "ml") {
    if (auto err = eval::rejectUnknownOptions(opts, {})) return ojson(*err);
    const double* ll = session.eval.eScalars.find("ll");
    const double* n = session.eval.eScalars.find("N");
    const double* rank = session.eval.eScalars.find("rank");
    if (ll == nullptr || n == nullptr || rank == nullptr || !std::isfinite(*ll) || !(*n > 0))
      return ojson(eval::errorBlock("engine_error", "likelihood information not found in last estimation results",
                                    std::nullopt, std::nullopt, std::nullopt, std::string("r(321)")));
    eval::EvalMatrix information;
    information.rownames = {"."};
    information.colnames = {"N", "ll_0", "ll", "df", "AIC", "BIC"};
    const double* ll0 = session.eval.eScalars.find("ll_0");
    information.data = {{*n, ll0 ? *ll0 : std::nan(""), *ll, *rank,
                         -2.0 * *ll + 2.0 * *rank, -2.0 * *ll + std::log(*n) * *rank}};
    eval::replaceRClass(session.eval);
    session.eval.rMatrices.emplace_back("S", std::move(information));
    return ojson{{"type", "display"}, {"text", "AIC = " + std::to_string(-2.0 * *ll + 2.0 * *rank) +
                                             "; BIC = " + std::to_string(-2.0 * *ll + std::log(*n) * *rank)}};
  }
  // eb-order T-P3: `estat ic` after a posting with no fitted model (estpost)
  // finds no e(ll): Stata's r(321) "likelihood information not found in last
  // estimation results" (w2v_estpost_ematrix.log ep14_estat_ic, ep14n_estat_ic).
  if (!lmOpt.has_value() && sub == "ic" && session.eval.eLive() &&
      session.eval.coefSource() == nullptr) {
    return ojson(eval::errorBlock("engine_error",
                                  "likelihood information not found in last estimation results",
                                  std::nullopt, std::nullopt, std::nullopt, std::string("r(321)")));
  }
  if (!lmOpt.has_value() || lmOpt->cobraCmd != "csdid") {
    // FU-437: a subcommand this build does not serve here is r(198), never the
    // default engine_error's rc 1 (`regress` then `estat ic` printed 1).
    return engineErrorBlockM4(
        "estat: no csdid result in memory. Run a `csdid ...` command first "
        "(estat simple/event/pretrend/calendar/group operate on the last csdid).",
        "unsupported_option");
  }
  static const std::set<std::string> kValid = {"simple", "event", "calendar", "group", "pretrend"};
  if (!kValid.count(sub)) {
    return engineErrorBlockM4("estat " + sub +
                              ": not supported. After csdid, use one of: "
                              "simple, event, pretrend, calendar, group.",
                              "unsupported_option");
  }

  // BS-097: only `estat event` reads any options (window()/post below); the
  // other 4 csdid sub-forms (simple/calendar/group/pretrend) read none —
  // branch-scoped by `sub` for the same reason as the diagnostic branch
  // above (event's known set must not leak into the other four, and vice
  // versa).
  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  // csdid-estat-estore-post: `estat simple` takes estore()/post (csdid_estat.ado
  // csdid_simple `syntax, [estore(name) ... post ...]`; w2ag log:1447 ag_spost_live).
  const std::unordered_set<std::string>& kEstatEventKnownOpts = kVocabEstatEvent.known;
  static const std::unordered_set<std::string> kEstatSimpleKnownOpts = {"estore", "post"};
  if (auto err = eval::rejectUnknownOptions(opts, sub == "event"    ? kEstatEventKnownOpts
                                                  : sub == "simple" ? kEstatSimpleKnownOpts
                                                                    : std::unordered_set<std::string>{})) {
    return *err;
  }
  std::optional<std::string> estoreName;
  bool doPost = false;
  for (const auto& [k, v] : opts) {
    if (k == "post") doPost = true;
    if (k == "estore" && !v.isBool) estoreName = trimM4(v.strValue);
  }
  if (estoreName.has_value() && !strutil::isValidStataIdentifier(*estoreName)) {
    return ojson(eval::errorBlock("syntax_error", "estore(): " + *estoreName + " invalid name",
                                  std::nullopt, std::nullopt, std::nullopt, std::string("r(198)")));
  }

  LastModel lm = *lmOpt;
  ojson raw = lm.result.value_or(ojson::object());

  // `estat group` needs the per-cohort aggregation (cobra `groups` array,
  // engine >= 0.20.0 / contract 1.10.0). A cached result without it means the
  // pinned cobra predates the aggregation — error honestly rather than show a
  // wrong table.
  if (sub == "group" && !raw.contains("groups")) {
    return engineErrorBlockM4(
        "estat group: the csdid result has no per-cohort aggregation "
        "(cobra engine too old — expected a `groups` array). Re-run csdid "
        "with a cobra build that includes the group aggregation.");
  }

  if (sub == "event") {
    std::optional<std::string> winOpt;
    for (const auto& [k, v] : opts) {
      if (k == "window" && !v.isBool) winOpt = v.strValue;
    }
    if (winOpt.has_value() && !winOpt->empty()) {
      std::istringstream iss(*winOpt);
      std::vector<std::string> parts;
      std::string tok;
      while (iss >> tok) parts.push_back(tok);
      std::string wantWin;
      for (size_t i = 0; i < parts.size(); ++i) {
        if (i) wantWin += ",";
        wantWin += parts[i];
      }
      std::string haveWin;
      if (raw.contains("window") && raw["window"].is_array()) {
        for (size_t i = 0; i < raw["window"].size(); ++i) {
          if (i) haveWin += ",";
          std::ostringstream oss;
          oss << raw["window"][i];
          haveWin += oss.str();
        }
      }
      if (!wantWin.empty() && wantWin != haveWin) {
        std::vector<std::string> argv = {"csdid", "--data", lm.dataPath, "--y", lm.y};
        if (!lm.x.empty()) {
          argv.push_back("--x");
          argv.push_back(joinCommaM4(lm.x));
        }
        argv.push_back("--ivar");
        argv.push_back(lm.ivar.value_or(""));
        argv.push_back("--time");
        argv.push_back(lm.timevar.value_or(""));
        argv.push_back("--gvar");
        argv.push_back(lm.gvar.value_or(""));
        if (lm.method.has_value()) {
          argv.push_back("--method");
          argv.push_back(*lm.method);
        }
        if (lm.comparison.has_value()) {
          argv.push_back("--comparison");
          argv.push_back(*lm.comparison);
        }
        if (lm.pscoretrim.has_value()) {
          argv.push_back("--pscoretrim");
          argv.push_back(*lm.pscoretrim);
        }
        if (!lm.cluster.empty()) {
          argv.push_back("--cluster");
          argv.push_back(lm.cluster[0]);
        }
        // csdid-estat-estore-post: the fit's `long2` and `if` are part of the
        // model the window re-run must reproduce (w2ag log:670-683 ag_l2ny_ev:
        // `long2 notyet` + window(-2 2) gives Pre_avg Post_avg Tm3 Tm2 Tp0 Tp1
        // Tp2, where the short cells give another set).
        if (lm.long2) argv.push_back("--long2");
        if (!lm.fitKeepif.empty()) {
          argv.push_back("--keepif");
          argv.push_back(lm.fitKeepif);
        }
        argv.push_back("--window");
        argv.push_back(wantWin);
        argv.push_back("--tidy");
        try {
          raw = runCobraJsonM4(argv);
          LastModel updated = lm;
          updated.result = raw;
          // The re-run is the SAME e() (same fitSerial): setLastModel keeps its
          // block and e(b)/e(V), and the e() macros csdid wrote on top of the
          // fit (e(agg), e(clustvar), ...) are put back as they were.
          auto strings = session.eval.eStrings;
          auto scalars = session.eval.eScalars;
          auto matrices = session.eval.eMatrices;
          auto onFit = session.eOnFitNames;
          session.setLastModel(updated);
          session.eval.eStrings = std::move(strings);
          session.eval.eScalars = std::move(scalars);
          session.eval.eMatrices = std::move(matrices);
          session.eOnFitNames = std::move(onFit);
        } catch (const CobraError& ce) {
          return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
        }
      }
    }
  }

  // csdid-estat-estore-post (csdid_estat.ado csdid_simple / csdid_event): the
  // aggregate is posted as a NEW e() (`ereturn post b V`, e(cmd) estat,
  // e(cmdline), e(agg)), stored under estore() when given, and -- without
  // `post` -- csdid's e() is put back (`est restore lastreg`). r(b), r(V),
  // r(table) and r(agg) are returned either way. Records: w2ag log:1359
  // ag_evst_restore, :1372 ag_evst_cs `estat|event|8|...`, :1447 ag_spost_live,
  // :1564 ag_simple_nopost_r, :1616 ag_event_nopost_r, :1691 ag_evwin_post.
  if (sub == "simple" || sub == "event") {
    const std::optional<CsdidPostingM4> P =
        csdidPostingM4(raw, sub == "simple" ? std::string("agg_simple") : std::string("estat_event"));
    if (P.has_value()) {
      ojson display = csdidToEstimationM4(raw, sub);
      const ojson coeftable = csdidPostedCoeftableM4(*P);
      display["coeftable"] = coeftable;

      // r(): csdid_estat's programs are rclass; r(table) is the coefficient table's on the
      // posted b/V (no e(df_r): z rows, df missing, crit invnormal(.975)).
      eval::replaceRClass(session.eval);
      {
        const size_t K = P->names.size();
        eval::EvalMatrix tbl;
        tbl.rownames = {"b", "se", "z", "pvalue", "ll", "ul", "df", "crit", "eform"};
        tbl.colnames = P->names;
        tbl.data.assign(9, std::vector<double>(K, std::numeric_limits<double>::quiet_NaN()));
        for (size_t j = 0; j < K; ++j) {
          const ojson& row = coeftable[j];
          auto num = [&row](const char* key) {
            return row.contains(key) && row[key].is_number() ? row[key].get<double>()
                                                              : std::numeric_limits<double>::quiet_NaN();
          };
          tbl.data[0][j] = num("b");
          tbl.data[1][j] = num("se");
          tbl.data[2][j] = num("t");
          tbl.data[3][j] = num("p");
          tbl.data[4][j] = num("ci_lo");
          tbl.data[5][j] = num("ci_hi");
          tbl.data[7][j] = kZ975;
          tbl.data[8][j] = 0.0;
        }
        eval::EvalMatrix rb;
        rb.data.push_back(P->b);
        rb.rownames = {"y1"};
        rb.colnames = P->names;
        eval::EvalMatrix rV;
        for (size_t i = 0; i < K; ++i)
          rV.data.emplace_back(P->V.begin() + static_cast<std::ptrdiff_t>(i * K),
                               P->V.begin() + static_cast<std::ptrdiff_t>((i + 1) * K));
        rV.rownames = P->names;
        rV.colnames = P->names;
        session.eval.rMatrices.emplace_back("table", std::move(tbl));
        session.eval.rMatrices.emplace_back("b", std::move(rb));
        session.eval.rMatrices.emplace_back("V", std::move(rV));
        session.eval.rStrings.set("agg", sub);
        eval::declareRPosted(session.eval, "estat", {"table", "b", "V", "agg"});
      }

      if (doPost || estoreName.has_value()) {
        // The live e() as it stands, moved (not copied) so that putting it back
        // restores the very same estimation identity (liveEstimationIdentity).
        struct Snapshot {
          std::optional<LastModel> lm;
          std::optional<ojson> block;
          std::optional<ojson> lmResult;
          eval::OrderedFloatDict scalars;
          eval::OrderedDict strings;
          std::vector<std::pair<std::string, eval::EvalMatrix>> matrices;
          bool posted = false;
          std::vector<std::string> onFit;
          std::string activeName;
          ojson estAdded;
          std::string estAddedFp;
        } snap;
        if (!doPost) {
          snap.lm = session.lastModel();
          snap.block = std::move(session.lastEstBlock);
          snap.lmResult = session.eval.lastModelResult;
          snap.scalars = session.eval.eScalars;
          snap.strings = session.eval.eStrings;
          snap.matrices = session.eval.eMatrices;
          snap.posted = session.ePostedLive;
          snap.onFit = session.eOnFitNames;
          snap.activeName = session.activeEstimatesName;
          snap.estAdded = session.estAdded;
          snap.estAddedFp = session.estAddedFp;
        }
        // The estat e(): no fitted model behind it (ereturn post b V).
        session.lastModel_reset();
        session.eval.eScalars = eval::OrderedFloatDict();
        session.eval.eStrings.clear();
        session.eval.eMatrices.clear();
        ojson eblk;
        eblk["type"] = "estimation";
        eblk["estimator"] = "estat";
        eblk["estat"] = sub;
        eblk["depvar"] = "";
        eblk["coeftable"] = coeftable;
        session.lastEstBlock = eblk;
        postCsdidPostingM4(session, *P);
        session.eval.setEString("cmd", "estat");
        session.eval.setEString("cmdline", "estat " + sub);
        session.eval.setEString("agg", sub);
        session.eval.setEString("properties", "b V");
        session.ePostedLive = true;
        if (estoreName.has_value()) {
          ojson stored = handlers::runEststo(*estoreName, std::nullopt, session, "est",
                                             /*viaEststo=*/false);
          if (stored.value("type", std::string()) == "error") return stored;
        }
        if (!doPost) {
          if (snap.lm.has_value()) {
            session.setLastModel(*snap.lm);
          } else {
            session.lastModel_reset();
          }
          session.lastEstBlock = std::move(snap.block);
          session.eval.lastModelResult = std::move(snap.lmResult);
          session.eval.eScalars = std::move(snap.scalars);
          session.eval.eStrings = std::move(snap.strings);
          session.eval.eMatrices = std::move(snap.matrices);
          session.ePostedLive = snap.posted;
          session.eOnFitNames = std::move(snap.onFit);
          session.activeEstimatesName = std::move(snap.activeName);
          session.estAdded = std::move(snap.estAdded);
          session.estAddedFp = std::move(snap.estAddedFp);
        }
      }
      return display;
    }
  }

  if (sub == "event" && doPost) postCsdidEventEMatrices(session, raw);

  return csdidToEstimationM4(raw, sub);
}

// ===========================================================================
// BETA campaign Phase D batch D4/D5: time-series + post-estimation/diag/misc
// wirings (BS-077/078/079/080/081/085/021/082/086/083/084/087). Oracle:
// internal capture d45-oracle-stata.log
// (StataBE 19.5 on piet/packaging/sample/fatalities.csv, `gen high=(beertax
// >.5)` + `encode state, gen(snum)` + `gen t=_n` + `tsset t`; sections
// D4a-D4f, D5a-D5g). durbinalt's fix (BS-021) and the endogenous/overid/
// firststage IV diagnostics (BS-082) live inside runEstat above; the
// margins ,at() linear-only gate (BS-086) lives inside runMargins above.
// tsfill (BS-081) lives in handlers_data_verbs.cpp (a data-mutation verb,
// alongside gsort/order/recode/etc — this file's tsfill-adjacent handlers
// below all read the SAME session.timevar/panelvar/delta tsset/xtset spine
// runTsfill does).

// newey (BS-077): `newey depvar indepvars [if], lag(#)` -> cobra newey
// --timevar T --lag M. Oracle D4a (lag(2): beertax b=-1697.15 se=353.9058,
// spirits b=1384.099 se=292.4321, _cons b=12323.98 se=533.9508 — byte-for-
// byte engine match, including df_r=333). Real Stata's lag() is REQUIRED
// (Stata itself errors "option lag() required" if omitted) — this handler
// rejects the same way, not a silent default. The time variable is read
// from the session's own `tsset timevar` spine (the SAME context BS-075's
// xtpoisson/xtlogit read session.panelvar from — see runXtPoissonLogit's
// header comment above — mirrored here for the time-series-only case).
// Real Stata's newey also has t()/level() options; none have oracle coverage
// this batch and are left as honest rejects via the unknown-option net below.
// BUG-1503: after xtset the panel variable goes to the engine (--panelvar: the
// Newey-West sum pairs rows of one panel only), and `force` is forwarded: Stata
// refuses a time variable that is not regularly spaced, as `xtset id t` makes
// it (t repeats across panels), r(198) "t is not regularly spaced" unless force
// (newey.ado Checkt, checked by the engine). StataBE 19.5 (i3.do): force gives
// _se[x1] .04395156556168258, as `xtset id tt` (tt unique) does without it.
ojson runNewey(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  if (parsed.varlist.size() < 2) {
    return engineErrorBlockM4(
        "syntax_error: `newey` requires a dependent variable and at least one regressor",
        "syntax_error");
  }
  if (!session.timevar.has_value() || session.timevar->empty()) {
    return engineErrorBlockM4("newey: run `tsset timevar` first so PIET knows the time variable.",
                              "unsupported_option");
  }
  std::string y = parsed.varlist[0];
  std::vector<std::string> xs(parsed.varlist.begin() + 1, parsed.varlist.end());

  std::optional<std::string> lagVal;
  bool force = false;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "lag" && !v.isBool) lagVal = v.strValue;
    if (k == "force" && v.isBool && v.boolValue) force = true;
  }
  if (!lagVal.has_value() || trimM4(*lagVal).empty()) {
    return engineErrorBlockM4("syntax_error: option lag() required for `newey`", "syntax_error");
  }

  std::vector<std::string> argv = {"newey",  "--y",  y,      "--x",       joinCommaM4(xs),
                                    "--timevar", *session.timevar, "--lag", trimM4(*lagVal)};
  if (session.panelvar.has_value() && !session.panelvar->empty()) {
    argv.push_back("--panelvar");
    argv.push_back(*session.panelvar);
  }
  if (force) argv.push_back("--force");
  if (parsed.ifExpr.has_value()) {
    argv.push_back("--keepif");
    argv.push_back(*parsed.ifExpr);
  }
  if (session.delta.has_value()) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.17g", *session.delta);
    argv.push_back("--delta");
    argv.push_back(buf);
  }

  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kKnown = kVocabNewey.known;
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kKnown)) {
    return *err;
  }

  argv.push_back("--data");
  argv.push_back(*session.currentHbin());
  argv.push_back("--tidy");
  // e(sample) T2: the engine writes the fit's admitted rows over the --data
  // rows (its own --keepif included); installed below, before setLastModel.
  const std::vector<std::string> fitArgv = argv;  // captureFitFilterM4 reads the fit's own flags
  const std::string esampleValues = esampleFitArgs(session, argv, std::string(), 0);
  argv.push_back("--emit-v");  // BUG-1196: the fit's e(V), joined below

  ojson raw;
  try {
    raw = runCobraJsonM4(argv);
  } catch (const CobraError& ce) {
    cecon::native_unlink(esampleValues);
    // Stata's own stops (newey.ado): the spacing check r(198), and a lag that
    // pairs no row, r(2000) "no observations", or one row, r(2001) "insufficient
    // observations" (`mat vecaccum`; StataBE 19.5 lag(7) on 8 rows).
    if (ce.detail == "ts_not_regular")
      return ojson(eval::errorBlock("engine_error", ce.what(), std::nullopt, std::nullopt,
                                    std::nullopt, std::string("r(198)")));
    if (ce.detail == "no_obs" || ce.detail == "insufficient_obs")
      return ojson(eval::errorBlock("engine_error", ce.what(), std::nullopt, std::nullopt,
                                    std::nullopt,
                                    std::string(ce.detail == "no_obs" ? "r(2000)" : "r(2001)")));
    return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
  }

  ojson block = cobraToEstimationM4(raw, "newey", &session);
  block["estimator"] = "newey";
  block["depvar"] = y;
  if (raw.contains("lag")) block["lag"] = raw["lag"];

  LastModel lm;
  lm.cobraCmd = "newey";
  lm.y = y;
  lm.x = xs;
  lm.depvar = y;
  captureFitFilterM4(lm, fitArgv);
  lm.dataPath = *session.currentHbin();
  (void)esampleInstallOnHead(session, esampleValues, raw, lm);  // e(sample) T2
  lm.result = raw;
  session.setLastModel(lm);
  session.lastEstBlock = block;
  session.attachFitCovariance(raw);  // BUG-1196: e(V) is the fit's HAC covariance
  return block;
}

// prais (BS-078): `prais depvar indepvars [if] [, corc twostep
// rhotype(regress|tscorr)]` -> cobra prais --timevar T [--corc] [--twostep]
// [--rhotype ...]. Oracle D4b: `, twostep` (beertax b=-1997.243 se=281.7461,
// rho=.8034461, dw=1.902251/dw_0=.392886) and `, corc` (beertax b=-1972.609
// se=284.8613, rho=.8038026, dw=1.902573/dw_0=.392886) — BOTH byte-for-byte
// engine matches. Real Stata's DEFAULT (bare `prais`, neither corc nor
// twostep) is the ITERATED Prais-Winsten estimator; cobra's own bare-flag
// default is UNVERIFIED against Stata (no oracle coverage for the bare/no-
// flag form — the oracle only exercises `, twostep` and `, corc`), so it is
// wired through faithfully (same argv shape minus the two flags) but not
// selftest-pinned to a Stata value.
ojson runPrais(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  if (parsed.varlist.size() < 2) {
    return engineErrorBlockM4(
        "syntax_error: `prais` requires a dependent variable and at least one regressor",
        "syntax_error");
  }
  if (!session.timevar.has_value() || session.timevar->empty()) {
    return engineErrorBlockM4("prais: run `tsset timevar` first so PIET knows the time variable.",
                              "unsupported_option");
  }
  std::string y = parsed.varlist[0];
  std::vector<std::string> xs(parsed.varlist.begin() + 1, parsed.varlist.end());

  bool corc = false, twostep = false;
  std::optional<std::string> rhotype;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "corc" && v.isBool && v.boolValue) corc = true;
    if (k == "twostep" && v.isBool && v.boolValue) twostep = true;
    if (k == "rhotype" && !v.isBool) rhotype = v.strValue;
  }
  if (corc && twostep) {
    return engineErrorBlockM4("prais: corc and twostep may not be combined.", "syntax_error");
  }
  std::string rhotypeNorm;
  if (rhotype.has_value()) {
    rhotypeNorm = lowerM4(trimM4(*rhotype));
    if (rhotypeNorm != "regress" && rhotypeNorm != "tscorr") {
      return engineErrorBlockM4(
          "prais: rhotype() must be regress or tscorr, got `" + *rhotype + "`", "syntax_error");
    }
  }

  std::vector<std::string> argv = {"prais", "--y", y, "--x", joinCommaM4(xs), "--timevar",
                                    *session.timevar};
  // BUG-1507: after xtset, rho and the transform restart at every panel change
  // (prais.ado `_ts timevar panvar, panel`; StataBE 19.5 i2.do e(rho)
  // .55064701722608933, e(N_gaps) 39), so the engine gets the panel variable.
  if (session.panelvar.has_value() && !session.panelvar->empty()) {
    argv.push_back("--panelvar");
    argv.push_back(*session.panelvar);
  }
  if (parsed.ifExpr.has_value()) {
    argv.push_back("--keepif");
    argv.push_back(*parsed.ifExpr);
  }
  if (corc) argv.push_back("--corc");
  if (twostep) argv.push_back("--twostep");
  if (!rhotypeNorm.empty()) {
    argv.push_back("--rhotype");
    argv.push_back(rhotypeNorm);
  }
  if (session.delta.has_value()) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.17g", *session.delta);
    argv.push_back("--delta");
    argv.push_back(buf);
  }

  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kKnown = kVocabPrais.known;
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kKnown)) {
    return *err;
  }

  argv.push_back("--data");
  argv.push_back(*session.currentHbin());
  argv.push_back("--tidy");
  // e(sample) T2: the engine writes the fit's admitted rows over the --data
  // rows (its own --keepif included); installed below, before setLastModel.
  const std::vector<std::string> fitArgv = argv;  // captureFitFilterM4 reads the fit's own flags
  // The engine does not expose the Cochrane-Orcutt rows (apps/cmds/cmds_ts.cpp
  // refuses --esample-out with --corc), so `prais, corc` stays kUnavailable:
  // e(sample) after it is the named r(198).
  const std::string esampleValues =
      corc ? std::string() : esampleFitArgs(session, argv, std::string(), 0);
  argv.push_back("--emit-v");  // BUG-1196: the fit's e(V), joined below

  ojson raw;
  try {
    raw = runCobraJsonM4(argv);
  } catch (const CobraError& ce) {
    cecon::native_unlink(esampleValues);
    return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
  }

  ojson block = cobraToEstimationM4(raw, "prais", &session);
  block["estimator"] = "prais";
  block["depvar"] = y;
  static const char* kPraisFields[] = {"rho", "dw", "dw_0", "method"};
  for (const char* k : kPraisFields) {
    if (raw.contains(k)) block[k] = raw[k];
  }

  LastModel lm;
  lm.cobraCmd = "prais";
  lm.y = y;
  lm.x = xs;
  lm.depvar = y;
  captureFitFilterM4(lm, fitArgv);
  lm.dataPath = *session.currentHbin();
  (void)esampleInstallOnHead(session, esampleValues, raw, lm);  // e(sample) T2
  lm.result = raw;
  session.setLastModel(lm);
  session.lastEstBlock = block;
  session.attachFitCovariance(raw);  // BUG-1196: e(V) is the transformed fit's covariance
  return block;
}

// dfuller / pperron (BS-079): `dfuller varname [if] [, lags(#)
// noconstant|drift|trend]` / `pperron varname [if] [, lags(#)
// noconstant|trend]` -> cobra dfuller/pperron --var V --timevar T [--lags
// k] [--noconstant|--drift|--trend]. Oracle D4c (dfuller bare: Zt=-6.004,
// cv 1/5/10=-3.453/-2.877/-2.570, p byte-exact MacKinnon p; dfuller lags(2)
// trend: Zt=-5.873, cv=-3.987/-3.427/-3.130), D4d (pperron bare:
// Zrho=-65.302, Zt=-6.041, SAME cv row as dfuller's constant case,
// newey_west_lags=5 auto-selected by the engine) — ALL fields (test stat,
// critical values, AND p) are byte-for-byte engine matches, so unlike this
// item's original "if the engine lacks critical-value fields, pin stat+p
// only" fallback instruction, the FULL table pins here (the engine already
// carries cv_1/cv_5/cv_10 — no gap found). pperron has no `drift` variant in
// real Stata (only noconstant/trend) — honest-rejected there, unlike
// dfuller.
ojson runDfullerPperron(const std::string& cobraCmd, const eval::ParsedCommand& parsed,
                        SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  bool allowDrift = (cobraCmd == "dfuller");
  if (parsed.varlist.size() != 1) {
    return engineErrorBlockM4("syntax_error: `" + cobraCmd + "` requires exactly one variable",
                              "syntax_error");
  }
  if (!session.timevar.has_value() || session.timevar->empty()) {
    return engineErrorBlockM4(
        cobraCmd + ": run `tsset timevar` first so PIET knows the time variable.",
        "unsupported_option");
  }
  std::string v = parsed.varlist[0];

  std::optional<std::string> lagsVal;
  bool noConstant = false, drift = false, trend = false;
  for (const auto& [k, v2] : parsed.opts) {
    if (k == "lags" && !v2.isBool) lagsVal = v2.strValue;
    if ((k == "noconstant" || k == "nocons") && v2.isBool && v2.boolValue) noConstant = true;
    if (k == "drift" && v2.isBool && v2.boolValue) drift = true;
    if (k == "trend" && v2.isBool && v2.boolValue) trend = true;
  }
  int modeCount = (noConstant ? 1 : 0) + (drift ? 1 : 0) + (trend ? 1 : 0);
  if (modeCount > 1) {
    return engineErrorBlockM4(cobraCmd + ": noconstant/drift/trend may not be combined.",
                              "syntax_error");
  }
  if (drift && !allowDrift) {
    return engineErrorBlockM4(
        "Option `drift` is not a valid Stata option for `" + cobraCmd + "` (real Stata's " +
        cobraCmd + " has no drift() form — only noconstant/trend).",
        "unsupported_option");
  }

  std::vector<std::string> argv = {cobraCmd, "--var", v, "--timevar", *session.timevar};
  if (lagsVal.has_value() && !trimM4(*lagsVal).empty()) {
    argv.push_back("--lags");
    argv.push_back(trimM4(*lagsVal));
  }
  if (noConstant) argv.push_back("--noconstant");
  if (drift) argv.push_back("--drift");
  if (trend) argv.push_back("--trend");
  if (parsed.ifExpr.has_value()) {
    argv.push_back("--keepif");
    argv.push_back(*parsed.ifExpr);
  }

  // BUG-233: see runXtPoissonLogit — `drift` is dfuller-only, so it stays a
  // conditional insert here while the registered vocabulary carries the union.
  std::unordered_set<std::string> known = kDfullerPperronBase;
  if (allowDrift) known.insert("drift");
  if (auto err = eval::rejectUnknownOptions(parsed.opts, known)) {
    return *err;
  }

  argv.push_back("--data");
  argv.push_back(*session.currentHbin());
  argv.push_back("--tidy");

  ojson raw;
  try {
    raw = runCobraJsonM4(argv);
  } catch (const CobraError& ce) {
    return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
  }

  ojson block = cobraToEstimationM4(raw, cobraCmd, &session);
  block["estimator"] = cobraCmd;
  static const char* kUnitRootFields[] = {
      "Zt", "Zrho", "cv_1", "cv_5", "cv_10", "case", "lags", "newey_west_lags", "test", "var",
  };
  for (const char* k : kUnitRootFields) {
    if (raw.contains(k)) block[k] = raw[k];
  }
  // No LastModel caching — dfuller/pperron are r-class diagnostic tests in
  // real Stata, not e-class "current model" estimation commands (no
  // margins/test/predict after dfuller in real Stata either).
  return block;
}

// wntestq (BS-080): `wntestq varname [if] [, lags(#)]` -> cobra wntestq
// --var V [--lags P]. Oracle D4e: bare (Q=839.9262, df=40=lags, p byte-
// exact) and `, lags(10)` (Q=557.2013, df=10) — BOTH engine-exact. Real
// Stata's own default lag rule is min(n/2-2, 40); the engine reproduces
// this AUTOMATICALLY when --lags is omitted (live-verified on both n=336 ->
// df=40 and a synthetic n=30 subset -> df=13 == min(30/2-2,40)=13) — PIET
// does NOT need to replicate the rule itself, it only forwards lags(#) when
// the user actually types it. No --timevar on this CLI at all (rows are
// read in on-disk order) — unlike newey/prais/dfuller/pperron above, no
// prior `tsset` is required.
ojson runWntestq(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  if (parsed.varlist.size() != 1) {
    return engineErrorBlockM4("syntax_error: `wntestq` requires exactly one variable",
                              "syntax_error");
  }
  std::string v = parsed.varlist[0];

  std::optional<std::string> lagsVal;
  for (const auto& [k, val] : parsed.opts) {
    if (k == "lags" && !val.isBool) lagsVal = val.strValue;
  }

  std::vector<std::string> argv = {"wntestq", "--var", v};
  if (lagsVal.has_value() && !trimM4(*lagsVal).empty()) {
    argv.push_back("--lags");
    argv.push_back(trimM4(*lagsVal));
  }
  if (parsed.ifExpr.has_value()) {
    argv.push_back("--keepif");
    argv.push_back(*parsed.ifExpr);
  }

  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kKnown = kVocabWntestq.known;
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kKnown)) {
    return *err;
  }

  argv.push_back("--data");
  argv.push_back(*session.currentHbin());
  argv.push_back("--tidy");

  ojson raw;
  try {
    raw = runCobraJsonM4(argv);
  } catch (const CobraError& ce) {
    return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
  }

  ojson block = cobraToEstimationM4(raw, "wntestq", &session);
  block["estimator"] = "wntestq";
  static const char* kWntestqFields[] = {"Q", "df", "lags", "var", "test"};
  for (const char* k : kWntestqFields) {
    if (raw.contains(k)) block[k] = raw[k];
  }
  return block;
}

// sdtest (BS-085): `sdtest varname [if], by(groupvar)` (two-sample
// variance-ratio test) / `sdtest varname [if] = #` (one-sample, against a
// fixed SD) -> cobra sdtest --var V (--by G | --against SD0) [--level L].
// Oracle D5a (by(high)): F=1.3992 exactly, df1=229 df2=105, p_lower/
// p_upper/p all byte-exact against Stata's Pr(F<f)/Pr(F>f)/2*Pr(F>f). The
// `= #` (one-sample) form is wired too (cobra's own --against flag) but has
// no oracle coverage this batch — wiring-only, not Stata-pinned.
ojson runSdtest(const std::string& varName, const std::optional<std::string>& againstVal,
                const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                const std::optional<std::string>& ifExpr, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  if (varName.empty()) {
    return engineErrorBlockM4("syntax_error: `sdtest` requires a variable", "syntax_error");
  }

  std::optional<std::string> byVal;
  for (const auto& [k, v] : opts) {
    if (k == "by" && !v.isBool) byVal = v.strValue;
  }
  bool hasAgainst = againstVal.has_value() && !trimM4(*againstVal).empty();
  if (byVal.has_value() == hasAgainst) {
    return engineErrorBlockM4(
        "sdtest: specify exactly one of `by(groupvar)` or `= #` (two-sample vs one-sample form).",
        "syntax_error");
  }

  std::vector<std::string> argv = {"sdtest", "--var", varName};
  if (byVal.has_value()) {
    argv.push_back("--by");
    argv.push_back(*byVal);
  } else {
    argv.push_back("--against");
    argv.push_back(trimM4(*againstVal));
  }
  if (ifExpr.has_value()) {
    argv.push_back("--keepif");
    argv.push_back(*ifExpr);
  }
  std::optional<std::string> levelVal;
  for (const auto& [k, v] : opts) {
    if (k == "level" && !v.isBool) levelVal = v.strValue;
  }
  if (levelVal.has_value() && !trimM4(*levelVal).empty()) {
    argv.push_back("--level");
    argv.push_back(trimM4(*levelVal));
  }

  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kKnown = kVocabSdtest.known;
  if (auto err = eval::rejectUnknownOptions(opts, kKnown)) {
    return *err;
  }

  argv.push_back("--data");
  argv.push_back(*session.currentHbin());
  argv.push_back("--tidy");

  ojson raw;
  try {
    raw = runCobraJsonM4(argv);
  } catch (const CobraError& ce) {
    return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
  }

  ojson block = cobraToEstimationM4(raw, "sdtest", &session);
  block["estimator"] = "sdtest";
  static const char* kSdtestFields[] = {
      "F", "df1", "df2", "p_lower", "p_upper", "mode", "sd", "sd_1", "sd_2",
      "groups", "combined", "level", "chi2", "sd0", "se", "ci_lo", "ci_hi", "var",
  };
  for (const char* k : kSdtestFields) {
    if (raw.contains(k)) block[k] = raw[k];
  }
  return block;
}

// hausman (BS-083): `hausman NAME1 NAME2` — the classic Stata idiom
// `xtreg,fe` + `estimates store A` + `xtreg,re` + `estimates store B` +
// `hausman A B`. Validates both eststo/estimates-store names exist AND are
// the fe/re pair of the SAME spec (same depvar, same regressor SET; one fe
// one re), then calls cobra's OWN `hausman --y --x --ivar` directly (the
// engine recomputes BOTH sides itself from raw data — it does NOT consume
// the two stored coefficient tables at all, unlike real Stata's own
// hausman, which diffs two ALREADY-FITTED e(b)/e(V)). Oracle D5e: chi2=2.62,
// p=0.2697, per-coef b_fe/b_re/diff (beertax diff=376.6603, unemp
// diff=-.6916189) — byte-for-byte engine match.
//
// DISCLOSED LIMITATION: eststo's own snapshot (eststo_esttab.cpp's
// makeSnapshot) captures only {coeftable, n_obs, r2, depvar, estimator, fe}
// — no x-regressor list, dataPath, or ivar field of its own. x is
// reconstructed from the stored coeftable's own row names (dropping
// "_cons") — lossless for THIS purpose (set membership, not display
// order). depvar is a SECOND, independent gap: live-verified `cobra xtreg
// --ivar ...` (the RE path)'s own raw JSON carries NO "depvar" field at all
// (unlike `cobra feols --fe ...`, which does), so the RE snapshot's depvar
// is ALWAYS empty — the depvar cross-check below only fires when the RE
// side actually has one (never today), leaning on the regressor-SET check
// as the real "same specification" guard in the meantime. The FE store is
// identified by estimator=="feols" AND a non-null
// single-key "fe" (absorbed) object (xtreg,fe routes through
// cobraToEstimationM4(raw,"feols") with --fe=panelvar, which populates
// raw["absorbed"]={panelvar:k}; a PLAIN `feols`/`regress` fit has no
// absorb() and so no "fe" object at all here — this is a real, non-spurious
// signal). The RE store is identified by estimator=="xtreg_re" (cobra's own
// raw "estimator" string for the --ivar RE path, live-verified — NOT
// "xtreg", which is only the block's display default). Neither eststo's
// snapshot NOR runXtreg's own cacheLastModelM4 records the RE fit's ivar
// anywhere at all — this handler uses the CURRENT session.panelvar (the
// active `xtset` context) as the shared ivar for BOTH sides, and requires
// it to equal the FE store's own absorbed-variable name (its one source of
// ground truth), i.e. hausman assumes the user has not re-`xtset`'d to a
// different panel variable since fitting the RE side. Similarly there is no
// stored dataPath in the snapshot either — this uses session.currentHbin()
// (the CURRENTLY loaded dataset), i.e. hausman also assumes the same
// dataset is still loaded. Both are disclosed, reasonable-workflow
// assumptions (matching the D5e oracle's own single-tsset, single-dataset
// session), not silent wrong answers.
ojson runHausman(const std::string& name1, const std::string& name2,
                  const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                  SessionState& session) {
  // BS-097: real Stata `hausman` takes constant/sigmamore/sigmaless/force/
  // alleqs/skipeqs()/equations()/df()/tconsistent()/tefficient() ([R]
  // hausman, r.pdf p.1091-1092) — none implemented by this handler, it
  // always recomputes both sides from raw data via cobra's own --tidy
  // hausman, which has no such knobs — every one of them now reads as an
  // honest named reject instead of a silent no-op.
  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kHausmanKnownOpts = kVocabHausman.known;
  // BUG-105 secondary ⑤: the options above are documented Stata syntax, not
  // typos, so they get the "not yet implemented" wording; anything else
  // (a genuine bogus token) keeps the plain "not allowed" reject.
  const std::unordered_set<std::string>& kHausmanKnownButUnimplemented = kVocabHausman.unimplemented;
  if (auto err =
          eval::rejectUnknownOptions(opts, kHausmanKnownOpts, kHausmanKnownButUnimplemented)) {
    return *err;
  }
  const ojson* s1 = session.estimatesGet(name1);
  const ojson* s2 = session.estimatesGet(name2);
  if (s1 == nullptr) {
    return engineErrorBlockM4(
        "hausman: no stored estimation result named `" + name1 + "`. Run `eststo " + name1 +
        "` (or `estimates store " + name1 + "`) after fitting the model.",
        "unsupported_option");
  }
  if (s2 == nullptr) {
    return engineErrorBlockM4(
        "hausman: no stored estimation result named `" + name2 + "`. Run `eststo " + name2 +
        "` (or `estimates store " + name2 + "`) after fitting the model.",
        "unsupported_option");
  }

  auto xset = [](const ojson& snap) {
    std::set<std::string> xs;
    if (snap.contains("coeftable") && snap["coeftable"].is_array()) {
      for (const auto& row : snap["coeftable"]) {
        std::string nm = row.value("name", std::string());
        if (nm != "_cons" && !nm.empty()) xs.insert(nm);
      }
    }
    return xs;
  };
  auto isFeSnap = [](const ojson& snap) {
    std::string estimator = snap.value("estimator", std::string());
    return (estimator == "feols" || estimator == "reghdfe") && snap.contains("fe") &&
           snap["fe"].is_object() && snap["fe"].size() == 1;
  };
  auto isReSnap = [](const ojson& snap) {
    return snap.value("estimator", std::string()) == "xtreg_re";
  };

  const ojson *feSnap = nullptr, *reSnap = nullptr;
  std::string feName, reName;
  if (isFeSnap(*s1) && isReSnap(*s2)) {
    feSnap = s1;
    reSnap = s2;
    feName = name1;
    reName = name2;
  } else if (isFeSnap(*s2) && isReSnap(*s1)) {
    feSnap = s2;
    reSnap = s1;
    feName = name2;
    reName = name1;
  } else {
    return engineErrorBlockM4(
        "hausman: `" + name1 + "` and `" + name2 + "` must be one `xtreg ,fe` result and one "
        "`xtreg ,re` result of the SAME specification (e.g. `xtreg y x, fe` / `eststo A` / "
        "`xtreg y x, re` / `eststo B` / `hausman A B`) — got estimator=`" +
        s1->value("estimator", std::string()) + "` and estimator=`" +
        s2->value("estimator", std::string()) + "`.",
        "unsupported_option");
  }

  // Depvar cross-check is BEST-EFFORT only: live-verified the engine's own
  // `cobra xtreg --ivar ...` (RE) raw JSON has NO "depvar" field at all
  // (unlike `cobra feols --fe ...`, which does) — cobraToEstimationM4
  // therefore always stores an EMPTY depvar for the RE side, and eststo's
  // snapshot inherits that emptiness. Comparing an always-empty RE depvar
  // against the FE side's real one would reject EVERY legitimate fe/re
  // pair, so this only runs the check when the RE snapshot actually HAS a
  // non-empty depvar (inert today, activates automatically if a future
  // engine build starts populating it) — the regressor-SET check right
  // below is the real safety net for "same specification" in the meantime.
  std::string reDepvar = reSnap->value("depvar", std::string());
  if (!reDepvar.empty() && feSnap->value("depvar", std::string()) != reDepvar) {
    return engineErrorBlockM4(
        "hausman: `" + feName + "` and `" + reName + "` have different dependent variables (`" +
        feSnap->value("depvar", std::string()) + "` vs `" + reDepvar +
        "`) — hausman requires the fe/re pair of the SAME specification.",
        "unsupported_option");
  }
  std::set<std::string> xsFe = xset(*feSnap), xsRe = xset(*reSnap);
  if (xsFe != xsRe) {
    return engineErrorBlockM4(
        "hausman: `" + feName + "` and `" + reName + "` fit different regressor lists — hausman "
        "requires the fe/re pair of the SAME specification.",
        "unsupported_option");
  }

  std::string ivar;
  for (const auto& [k, v] : (*feSnap)["fe"].items()) {
    (void)v;
    ivar = k;  // single-key object, validated by isFeSnap() above.
  }
  if (!session.panelvar.has_value() || session.panelvar->empty() || *session.panelvar != ivar) {
    return engineErrorBlockM4(
        "hausman: run `xtset " + ivar + " ...` (the SAME panel variable `" + feName +
        "` was fit with) before calling `hausman` — the current xtset context (" +
        session.panelvar.value_or(std::string("<none>")) + ") does not match.",
        "unsupported_option");
  }
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();

  std::vector<std::string> xs(xsFe.begin(), xsFe.end());
  std::vector<std::string> argv = {"hausman",       "--y",     feSnap->value("depvar", std::string()),
                                    "--x",           joinCommaM4(xs), "--ivar", ivar,
                                    "--data",        *session.currentHbin(), "--tidy"};
  ojson raw;
  try {
    raw = runCobraJsonM4(argv);
  } catch (const CobraError& ce) {
    return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
  }

  ojson block;
  block["type"] = "estimation";
  block["estimator"] = "hausman";
  block["depvar"] = feSnap->value("depvar", std::string());
  block["n_obs"] = raw.value("n_obs", 0);
  static const char* kHausmanFields[] = {"chi2", "p", "dof", "coefs", "not_pd"};
  for (const char* k : kHausmanFields) {
    if (raw.contains(k)) block[k] = raw[k];
  }
  // BUG-1940: Stata's two notes under the statistic, in its words.
  std::vector<std::string> notes;
  if (raw.value("not_pd", false)) notes.push_back("(V_b-V_B is not positive definite)");
  if (raw.contains("chi2") && raw["chi2"].is_number() && raw["chi2"].get<double>() < 0) {
    notes.push_back("chi2<0 ==> model fitted on these data fails to meet the asymptotic "
                    "assumptions of the Hausman test; see suest for a generalized test");
  }
  if (!notes.empty()) block["notes"] = notes;
  return block;
}

// chow (BS-084): `chow depvar indepvars [if], by(groupvar)` — a PIET-ONLY
// convenience wrapper; real Stata has NO `chow` command at all (the
// oracle's own D5f section computes the classic Chow F test by hand: fit
// pooled + both subsamples, chowF = ((rss_pooled - rss_0 - rss_1)/k) /
// ((rss_0+rss_1)/(n-2k)) — see d45-oracle-stata.do's own comment "Chow test
// income~beertax split by high: pooled + subsamples"). cobra's own `chow`
// CLI computes the IDENTICAL classic homoskedastic SSR-based F test as its
// "classic" block (live-verified: engine classic.F=28.07417107825576,
// df1=2, df2=332 — byte-for-byte the oracle's own manually-computed
// chowF=28.074171, df1=2, df2=332) — plus a bonus heteroskedasticity-robust
// Wald variant (raw.robust, no oracle coverage, not selftest-pinned) this
// handler also surfaces. vce()/cluster() are NOT forwarded this batch (no
// oracle coverage for cobra chow's own --vcov/--cluster surface) — left as
// honest rejects via the unknown-option net.
ojson runChow(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  if (parsed.varlist.size() < 2) {
    return engineErrorBlockM4(
        "syntax_error: `chow` requires a dependent variable and at least one regressor",
        "syntax_error");
  }
  std::string y = parsed.varlist[0];
  std::vector<std::string> xs(parsed.varlist.begin() + 1, parsed.varlist.end());

  std::optional<std::string> byVal;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "by" && !v.isBool) byVal = v.strValue;
  }
  if (!byVal.has_value() || trimM4(*byVal).empty()) {
    return engineErrorBlockM4("syntax_error: `chow` requires by(groupvar)", "syntax_error");
  }

  std::vector<std::string> argv = {"chow", "--y", y, "--x", joinCommaM4(xs), "--by",
                                    trimM4(*byVal)};
  if (parsed.ifExpr.has_value()) {
    argv.push_back("--keepif");
    argv.push_back(*parsed.ifExpr);
  }

  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kKnown = kVocabChow.known;
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kKnown)) {
    return *err;
  }

  argv.push_back("--data");
  argv.push_back(*session.currentHbin());
  argv.push_back("--tidy");

  ojson raw;
  try {
    raw = runCobraJsonM4(argv);
  } catch (const CobraError& ce) {
    return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
  }

  ojson block;
  block["type"] = "estimation";
  block["estimator"] = "chow";
  block["depvar"] = y;
  block["n_obs"] = raw.value("n_obs", 0);
  static const char* kChowFields[] = {"G", "by", "group_sizes", "classic", "robust"};
  for (const char* k : kChowFields) {
    if (raw.contains(k)) block[k] = raw[k];
  }
  block["note"] =
      "Chow test (real Stata has no built-in `chow` command — this is a PIET convenience wrapper "
      "reproducing the classic manual F-test: pooled vs split-sample SSR).";
  return block;
}

// mkspline (BS-087, part): `mkspline stub = varname [if], cubic [nknots(#) |
// knots(v1,v2,...)]` -> cobra eval --mkspline "stub=var, cubic[, nknots(N)|,
// knots(...)]" --out new.hbin (a data-mutation verb, like egen — adds
// stub1..stubK-1 columns). Oracle D5g: nknots(4) gives 3 columns
// (inc_sp1/2/3, pinned means/sds) — see the fix disclosure below;
// only the CUBIC form is implemented. Real Stata's LINEAR/piecewise-linear
// default (mkspline with NO `cubic` keyword) has NO engine equivalent —
// live-verified the engine's `eval --mkspline` op ALWAYS computes a
// restricted cubic spline basis regardless of whether `cubic` was typed in
// the spec string (omitting it does NOT fall back to a linear basis, it
// silently produces the SAME cubic one). Rather than silently answer
// linear-mkspline with a cubic result, PIET requires `cubic` to be typed
// and has no linear mkspline of its own.
//
// ENGINE FIX (cobra 0.25.0): `eval --mkspline`'s own `nknots(#)` clause
// now works. LOWERCASE `nknots(N)` — the engine's own documented spelling,
// per `cobra eval`'s usage banner — places knots at Harrell's percentile
// method (e.g. nknots(4) -> the 5/35/65/95 percentiles of the source
// variable) and returns the correct N-1 basis columns, verified live
// against StataBE (byte-for-byte knot positions and basis-column summary
// stats). The old failure mode was a paren_arg substring collision at the
// CLI arg-parsing layer, present through 0.24.0: LOWERCASE `nknots(N)`
// hard-errored "mkspline: need at least 3 knots" for EVERY N tried from 1
// to 10 (the value never actually parsed), while UPPERCASE `NKNOTS(N)` was
// silently ACCEPTED but the value N was DISCARDED — it always fell back to
// the same hardcoded 5-knot/4-column default regardless of N. PIET forwards
// `nknots(N)` in LOWERCASE, per the engine's own documented spelling — the
// D5g oracle's own exact invocation (`nknots(4)`) now succeeds cleanly and
// is oracle-pinned in D5g's own selftest leg (selftest.cpp). The explicit
// `knots(v1,v2,...)` form (caller-supplied exact knot positions, bypassing
// nknots() entirely) also works mechanically (live-verified) and is
// independently wired/checked in its own selftest leg (a fresh stub name,
// since it now runs alongside the fixed nknots() leg rather than as its
// sole proof of mechanical wiring).
ojson runMkspline(const std::string& stubIn, const std::string& srcVarIn,
                   const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                   const std::optional<std::string>& ifExpr, SessionState& session) {
  // C4-L4B (FIX-C4L4B-RESIDUALS ①): entry validation reads a NAME and the
  // column NAME SET and nothing else, so the resident card answers both (the
  // D1 rule) and the datastore opens LAZILY. Escape (PIET_NO_RESIDENT_READ)
  // keeps the eager entry — and the whole stateless tail below — VERBATIM, so
  // the floor RED arm reproduces the pre-C4 path with one env.
  if (piet::residentReadDisabled()) {
    if (!session.currentHbin().has_value() || session.datastore() == nullptr) {
      return eval::noDataLoaded();
    }
  } else if (!session.hasData()) {
    return eval::noDataLoaded();
  }
  std::string stub = trimM4(stubIn);
  std::string srcVar = trimM4(srcVarIn);
  if (stub.empty() || stub.find_first_of(" \t") != std::string::npos) {
    return engineErrorBlockM4(
        "syntax_error: `mkspline` requires a single STUB name on the left of `=` (the multi-"
        "newvar form `mkspline v1 v2 v3 = x` is not supported in this version of PIET).",
        "syntax_error");
  }
  if (srcVar.empty() || srcVar.find_first_of(" \t") != std::string::npos) {
    return engineErrorBlockM4("syntax_error: `mkspline` requires a single source variable",
                              "syntax_error");
  }
  Datastore* ds = nullptr;
  auto ensureDs = [&]() -> Datastore* {
    if (ds == nullptr) ds = session.datastore();
    return ds;
  };
  const std::vector<VarInfo>* card = session.residentReadCard();
  auto cardHas = [&](const std::string& name) -> bool {
    for (const auto& vi : *card) {
      if (vi.name == name) return true;
    }
    return false;
  };
  if (card != nullptr ? !cardHas(srcVar) : !ensureDs()->has(srcVar)) {
    return engineErrorBlockM4("Variable '" + srcVar + "' not found.");
  }
  if (ifExpr.has_value()) {
    return engineErrorBlockM4(
        "mkspline: `if` is not supported in this version of PIET (live-verified: the engine's "
        "`eval --keepif` DROPS non-matching rows from the WHOLE dataset rather than leaving the "
        "new spline variables missing for them the way real Stata's if-qualified generate-family "
        "commands do — forwarding it would silently shrink the dataset).",
        "unsupported_option");
  }

  bool cubic = false;
  for (const auto& [k, v] : opts) {
    if (k == "cubic" && v.isBool && v.boolValue) cubic = true;
  }
  if (!cubic) {
    return engineErrorBlockM4(
        "mkspline: only the `cubic` (restricted cubic spline) form is supported in this version "
        "of PIET — the engine's eval --mkspline op has no linear/piecewise-linear basis (it "
        "always computes a cubic basis regardless of the `cubic` keyword), so PIET requires "
        "`cubic` to be typed explicitly rather than silently answer the linear form with a cubic "
        "result.",
        "unsupported_option");
  }
  for (const auto& [k, v] : opts) {
    if (k == "displayknots" && v.isBool && v.boolValue) {
      return engineErrorBlockM4(
          "Option `displayknots` is a valid Stata option that this version of PIET does not yet "
          "implement for `mkspline` (no engine CLI equivalent — knot positions are never "
          "surfaced back to PIET).",
          "unsupported_option");
    }
  }

  std::optional<std::string> nknotsVal, knotsVal;
  for (const auto& [k, v] : opts) {
    if (k == "nknots" && !v.isBool) nknotsVal = v.strValue;
    if (k == "knots" && !v.isBool) knotsVal = v.strValue;
  }
  if (nknotsVal.has_value() && knotsVal.has_value()) {
    return engineErrorBlockM4("mkspline: nknots() and knots() may not be combined.",
                              "syntax_error");
  }

  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kKnown = kVocabMkspline.known;
  if (auto err = eval::rejectUnknownOptions(opts, kKnown)) {
    return *err;
  }

  std::string mkArg = stub + "=" + srcVar + ", cubic";
  if (nknotsVal.has_value() && !trimM4(*nknotsVal).empty()) {
    mkArg += ", nknots(" + trimM4(*nknotsVal) + ")";
  } else if (knotsVal.has_value() && !trimM4(*knotsVal).empty()) {
    mkArg += ", knots(" + trimM4(*knotsVal) + ")";
  }

  std::set<std::string> beforeSet;
  if (card != nullptr) {
    for (const auto& vi : *card) beforeSet.insert(vi.name);
  } else {
    std::vector<std::string> before = ensureDs()->names();
    beforeSet.insert(before.begin(), before.end());
  }

  std::vector<std::string> argv = {"eval", "--data", *session.currentHbin(), "--mkspline", mkArg};
  std::string outPath = nextPathEst(session.workdir(), "mkspline");
  argv.push_back("--out");
  argv.push_back(outPath);

  {
    // C4-L4B: `eval --mkspline` is a single TERMINAL mutation — its `--out`
    // becomes the head and is never read back as a file — so it takes the
    // resident frame plane, the same opt-in `recode`'s no-`if` arm has held
    // since MAXPERF Stage 2. Escape: pre-C4 stateless routing, verbatim.
    std::optional<SessionState::ResidentEligible> mksplineResident;
    if (!piet::residentReadDisabled()) mksplineResident.emplace(session);
    try {
      runCobraJsonM4(argv);
    } catch (const CobraError& ce) {
      return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
    }
  }

  session.load(outPath);
  // C4-L4B resident tail: fold the frame delta into the shadow card instead of
  // invalidate+reopen. mkspline is the one verb in this class that cannot NAME
  // its new columns up front — the engine reports the generated basis names on
  // stderr only (apps/cmds/cmds_data.cpp:144), never in the JSON PIET reads —
  // so `new_vars` is diffed off the freshly FOLDED card and appended last,
  // which is exactly where the stateless block below has always put it
  // (ordered_json preserves insertion order; the two blocks are key-for-key
  // identical). The delta is consumed here, so a fold that DECLINES falls
  // through to the stateless rebuild below with nothing left pending — the
  // same landing spot mutationResult uses for its own nullopt.
  if (!piet::residentReadDisabled()) {
    if (auto folded = session.tryResidentMutationCard("mkspline", {}, {}, {})) {
      if (const std::vector<VarInfo>* after = session.residentReadCard()) {
        ojson out = std::move(*folded);
        ojson newVars = ojson::array();
        for (const auto& vi : *after) {
          if (!beforeSet.count(vi.name)) newVars.push_back(vi.name);
        }
        out["new_vars"] = newVars;
        return out;
      }
    }
  }
  session.invalidateDatastore();
  Datastore* dsAfter = session.datastore();

  std::vector<std::string> newVarsVec;
  if (dsAfter != nullptr) {
    for (const auto& n : dsAfter->names()) {
      if (!beforeSet.count(n)) newVarsVec.push_back(n);
    }
  }
  ojson newVars = ojson::array();
  for (const auto& n : newVarsVec) newVars.push_back(n);

  // C1-CARD-REBUILD-REDUCE A11 sibling: `mkspline` (engine `eval --mkspline`)
  // appends only the new basis columns (newVarsVec = names present after but not
  // before) and byte-preserves every other column, so seed the unchanged
  // columns' missing-counts (kPureAdd branch 1, action "mkspline") BEFORE the
  // varlist rebuild below skips each column's O(nrows) nMissing scan. Byte-
  // identical vs PIET_NO_NMISS_CACHE=1.
  session.seedNMissForPureAdd(dsAfter, "mkspline", newVarsVec, /*droppedVars=*/{},
                             /*rewrittenVars=*/{});

  ojson out;
  out["type"] = "data_mutation";
  out["action"] = "mkspline";
  ojson varlistAfter = ojson::array();
  auto vlist = session.varlistWithMeta(dsAfter);
  for (const auto& v : vlist) varlistAfter.push_back(v.toJson());
  session.updateNMissCache(vlist);  // C2-F13a: keep the cache current for the next mutation
  out["varlist_after"] = varlistAfter;
  out["n_rows_after"] = dsAfter ? dsAfter->nRows() : 0;
  out["new_vars"] = newVars;
  return out;
}


}  // namespace piet::handlers
