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
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_set>

#include "piet/cobra_runner.h"
#include "piet/cobra_session.h"
#include "piet/display_hardening.h"
#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"
#include "piet/eval/macros.h"
#include "piet/inrange_filter.h"
#include "piet/mutation_result.h"
#include "piet/settings.h"
#include "piet/strutil.h"
#include "io/win_compat.h"  // cecon::native_unlink (BUG-050 wide unlink)
#include "piet/translate.h"
#include "piet/scratch_seq.h"
#include "piet/stata_format.h"

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
#include "estimators/posthest.h"  // cecon::margins_parse_at (header-inline)

// R2: the option-vocab bindings, restored VERBATIM — registering the same
// token twice UNIONS the sets (errors.h, deliberate), and a set unioned with
// itself is itself, so the single-registry law holds across the three TUs.
namespace {
const piet::eval::OptVocab& kVocabTtest = piet::eval::registerOptVocab(
    {"ttest"}, {{"level", "by", "unequal", "welch", "paired", "reverse", "unpaired"},
     {}});
// FU-23 (BUG-757): `vce` and `robust` are accepted and IGNORED by csdid itself,
// whatever their argument: w2l_vce_shape.log csdid_ctl_vcecluster,
// csdid_ctl_vcerobust, csdid_vce_c2, csdid_vce_cluster_novar, csdid_robust_val
// and csdid_vce_robust_x are all rc 0 with the default fit's se11 2.931073697886207.
// Registering the token unions with the two other TUs' copies (see above), so
// adding them here is enough for the dispatcher and for runCsdid's net.
const piet::eval::OptVocab& kVocabCsdid = piet::eval::registerOptVocab(
    {"csdid"}, {{"ivar", "time", "gvar", "method", "comparison", "pscoretrim", "window", "cluster", "long2",
      "vce", "robust",
      // csdid-notyet-agg: csdid `[notyet]`, `agg(str)` (w2ag log:451 ag_ny,
      // :1040-1272 ag_asimple/aevent/agroup/acal, all rc 0). Unions like vce/robust above.
      "notyet", "agg"},
     {}});
const piet::eval::OptVocab& kVocabReg3 = piet::eval::registerOptVocab(
    {"reg3"}, {{"sure", "3sls"},
     {}});
// sureg's syntax: the options this build does not forward to the fit
// (iterated SUR, dfk/small corrections, constraints, the corr/BP test) are
// refused by name; level()/noconstant/vce() reach runReg3's own refusals.
const piet::eval::OptVocab& kVocabSureg = piet::eval::registerOptVocab(
    {"sureg"}, {{"level", "noconstant", "nocons", "vce", "noheader", "notable", "nofooter",
                 "nolog", "log"},
                {"isure", "dfk", "dfk2", "small", "corr", "constraints", "iterate",
                 "tolerance", "trace"}});
// FU-23 (BUG-758): `nograph` suppresses boottest's confidence-curve plot; PIET's
// boottest draws none, so the option has nothing to change. Every boottest arm
// of w2l_vce_shape.log passes it at rc 0, and this net refused it r(198).
// `boottype` (papers 053, 086): runBoottest reads its value (w2t C12, F6).
// `weighttype` (minimum abbreviation `weight`, boottest), not `weights`
// (BUG-1173).
const piet::eval::OptVocab& kVocabBoottest = piet::eval::registerOptVocab(
    {"boottest"}, {{"reps", "seed", "weighttype", "null",
                                                          "cluster", "nograph", "boottype"},
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
    {"estat"}, {{"window", "post",
      // csdid-estat-estore-post: csdid_estat.ado csdid_simple/csdid_event
      // `syntax, [estore(name) ...]` (w2ag log:1359 ag_evst_restore rc 0). Unions
      // with the two other TUs' copies (registerOptVocab, errors.h).
      "estore"},
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
// w2b-iv-options: `first` (w2b_estim.log:879 ivregress_first, :1064
// ivreghdfe_first, both rc 0). Registered on its own rather than edited into
// the verbatim copy above: the registry UNIONS per token, so this lands in the
// same `reghdfe` slot kVocabIvRegress names, which is runIvRegress's net (and,
// for canonicalisation only, the non-IV reghdfe word, whose own hand-written
// net in runRegress still refuses `first`).
const piet::eval::OptVocab& kVocabIvFirst = piet::eval::registerOptVocab(
    {"ivregress", "ivreg", "ivreghdfe", "reghdfe"}, {{"first"}, {}});
// w2b-iv-options: ppmlhdfe's `d` / `d(newvar)` (w2b_estim.log:1305
// ppmlhdfe_d, :1336 ppmlhdfe_absorb_d_named). ppmlhdfe's slot only: plain
// `poisson` has no such option.
const piet::eval::OptVocab& kVocabPpmlD = piet::eval::registerOptVocab(
    {"ppmlhdfe"}, {{"d"}, {}});
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

namespace {

ojson coefRowM4(const std::string& name, const ojson& att, const ojson& se) {
  ojson r;
  r["name"] = name;
  if (att.is_null() || se.is_null()) {
    r["b"] = att;
    r["se"] = se;
    r["t"] = nullptr;
    r["p"] = nullptr;
    r["ci_lo"] = nullptr;
    r["ci_hi"] = nullptr;
    return r;
  }
  try {
    double attF = att.get<double>();
    double seF = se.get<double>();
    double z = (seF != 0.0) ? attF / seF : std::nan("");
    r["b"] = attF;
    r["se"] = seF;
    r["t"] = z;
    r["p"] = seF != 0.0 ? ojson(normalPvalue(z)) : ojson(nullptr);
    r["ci_lo"] = attF - kZ975 * seF;
    r["ci_hi"] = attF + kZ975 * seF;
  } catch (...) {
    r["b"] = att;
    r["se"] = se;
    r["t"] = nullptr;
    r["p"] = nullptr;
    r["ci_lo"] = nullptr;
    r["ci_hi"] = nullptr;
  }
  return r;
}

// _csdid_to_estimation (dispatcher.py) — normalise a csdid result JSON into
// an estimation block for the requested aggregate.
// csdidToEstimationM4: hoisted below the anonymous block (R2) — TU c consumes it.

void cacheCsdidModel(SessionState& session, const std::vector<std::string>& argv, const ojson& raw) {
  auto optFor = [&](const std::string& flag) -> std::optional<std::string> {
    for (size_t i = 0; i < argv.size(); ++i) {
      if (argv[i] == flag && i + 1 < argv.size()) return argv[i + 1];
    }
    return std::nullopt;
  };
  LastModel lm;
  lm.cobraCmd = "csdid";
  lm.y = optFor("--y").value_or("");
  std::string xStr = optFor("--x").value_or("");
  if (!xStr.empty()) {
    std::string cur;
    for (char c : xStr) {
      if (c == ',') {
        if (!cur.empty()) lm.x.push_back(cur);
        cur.clear();
      } else {
        cur += c;
      }
    }
    if (!cur.empty()) lm.x.push_back(cur);
  }
  lm.depvar = lm.y;
  captureFitFilterM4(lm, argv);
  lm.dataPath = session.currentHbin().value_or("");
  lm.ivar = optFor("--ivar");
  lm.timevar = optFor("--time");
  lm.gvar = optFor("--gvar");
  lm.method = optFor("--method");
  lm.comparison = optFor("--comparison");
  lm.pscoretrim = optFor("--pscoretrim");
  auto clusterVar = optFor("--cluster");
  if (clusterVar.has_value()) lm.cluster = {*clusterVar};
  lm.long2 = std::find(argv.begin(), argv.end(), "--long2") != argv.end();
  // csdid posts no e(vce)/e(vcetype) (csdid; w2ag log:167
  // ag_def_vce `||||.|`, w2l_vce_shape_det csdid_* `csdid|||.|180`): an empty
  // vcov makes setLastModel erase both.
  lm.vcov = "";
  lm.result = raw;
  session.setLastModel(lm);
}

// FU-23 (BUG-756): the SHAPE of the VCE options, read once for the four
// handlers below (csdid, ivprobit/ivtobit, boottest, xtpoisson/xtlogit ,fe).
// Each of them used to read `cluster` only when it carried a value
// (`k == "cluster" && !v.isBool`), so a bare `cluster` never reached their
// checks: xtpoisson/xtlogit ,fe and ivprobit/ivtobit fitted the iid model at
// rc 0, and boottest ran with the fit's clusters. A valued `robust(x)` and a
// trailing token in `vce(robust x)` were skipped the same way. What each
// command does with each shape differs and is the oracle's to say
// (tests/eval/golden/aer_w2/w2l_vce_shape.log), so this only classifies; the
// handlers map a shape to Stata's answer.
enum class OptShapeM4b { kAbsent, kBare, kEmpty, kOne, kMany };

struct VceShapesM4b {
  OptShapeM4b cluster = OptShapeM4b::kAbsent;
  std::string clusterRaw;  // trimmed cluster(...) text
  std::vector<std::string> clusterVars;
  OptShapeM4b robust = OptShapeM4b::kAbsent;  // kBare = the flag; kOne/kMany = robust(x)
  std::string robustRaw;
  bool sawVce = false;                    // vce()/vcov(), first one wins (as every loop here)
  std::string vceRaw;                     // trimmed
  std::string vceType;                    // canonicalVceType(); "" when unrecognised
  std::vector<std::string> vceRest;       // the tokens after the type
};

OptShapeM4b shapeOfM4b(const eval::OptValue& v, std::vector<std::string>* toks) {
  if (v.isBool) return OptShapeM4b::kBare;
  *toks = splitTokensM4(v.strValue);
  if (toks->empty()) return OptShapeM4b::kEmpty;
  return toks->size() == 1 ? OptShapeM4b::kOne : OptShapeM4b::kMany;
}

VceShapesM4b readVceShapesM4b(const eval::ParsedCommand& parsed) {
  VceShapesM4b s;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "cluster") {
      s.cluster = shapeOfM4b(v, &s.clusterVars);
      s.clusterRaw = v.isBool ? std::string() : trimM4(v.strValue);
    } else if (k == "robust") {
      std::vector<std::string> toks;
      s.robust = shapeOfM4b(v, &toks);
      s.robustRaw = v.isBool ? std::string() : trimM4(v.strValue);
    } else if ((k == "vce" || k == "vcov") && !s.sawVce) {
      s.sawVce = true;
      s.vceRaw = v.isBool ? std::string() : trimM4(v.strValue);
      std::string rest;
      s.vceType = canonicalVceType(s.vceRaw, &rest);
      s.vceRest = splitTokensM4(rest);
    }
  }
  return s;
}

// A refusal Stata gives itself, carried with Stata's own rc (coarseRc reads the
// `r(NNN)` reason first) and the oracle's wording.
ojson stataRefusalM4b(const std::string& message, int rc) {
  return ojson(eval::errorBlock("syntax_error", message, std::nullopt, std::nullopt, std::nullopt,
                                "r(" + std::to_string(rc) + ")"));
}

// Defined after runDrdid (below): drdid's fit with the treatment given as an
// expression; runCsdid's cluster()==ivar path re-runs csdid's last cell with it.
ojson drdidFitCoreM4(SessionState& session, const std::string& y,
                     const std::vector<std::string>& xs, const std::string& ivarName,
                     const std::string& timeVarName, const std::string& treatExpr,
                     const std::string& treatName, const std::optional<std::string>& ifExpr);

}  // namespace

// R2: hoisted from the anonymous namespace above so estimation_m4c.cpp can link it.
ojson csdidToEstimationM4(const ojson& raw, const std::string& subcmd) {
  ojson block;
  std::string estimator = raw.value("estimator", std::string("csdid"));
  block["type"] = "estimation";
  block["estimator"] = estimator;
  block["estat"] = subcmd;
  block["depvar"] = raw.value("depvar", "");
  block["n_obs"] = raw.contains("n_obs") ? raw["n_obs"] : raw.value("n_units", ojson(0));
  if (raw.contains("method") && !raw["method"].is_null() &&
      !(raw["method"].is_string() && raw["method"].get<std::string>().empty())) {
    block["method"] = raw["method"];
  }
  if (raw.contains("comparison") && !raw["comparison"].is_null() &&
      !(raw["comparison"].is_string() && raw["comparison"].get<std::string>().empty())) {
    block["comparison"] = raw["comparison"];
  }

  if (subcmd == "simple") {
    ojson att = raw.contains("simple_att") ? raw["simple_att"] : ojson(nullptr);
    ojson se = raw.contains("simple_se") ? raw["simple_se"] : ojson(nullptr);
    block["coeftable"] = ojson::array({coefRowM4("ATT", att, se)});
  } else if (subcmd == "event") {
    ojson rows = ojson::array();
    ojson eventStudy = ojson::array();
    for (const auto& ev : raw.value("events", ojson::array())) {
      ojson eVal = ev.value("e", ojson(nullptr));
      std::string name;
      if (eVal.is_number()) {
        double d = eVal.get<double>();
        if (d == std::floor(d)) {
          name = std::to_string(static_cast<long long>(d));
        } else {
          std::ostringstream oss;
          oss << d;
          name = oss.str();
        }
      } else if (eVal.is_string()) {
        name = eVal.get<std::string>();
      }
      ojson row = coefRowM4(name, ev.value("att", ojson(nullptr)), ev.value("se", ojson(nullptr)));
      rows.push_back(row);
      ojson es;
      es["e"] = name;
      es["att"] = row["b"];
      es["se"] = row["se"];
      es["ci_lo"] = row["ci_lo"];
      es["ci_hi"] = row["ci_hi"];
      eventStudy.push_back(es);
    }
    block["coeftable"] = rows;
    block["event_study"] = eventStudy;
    if (raw.contains("window") && !raw["window"].is_null() &&
        !(raw["window"].is_array() && raw["window"].empty())) {
      block["window"] = raw["window"];
    }
    if (raw.contains("pre_avg") && !raw["pre_avg"].is_null()) {
      block["pre_avg"] = raw["pre_avg"];
      block["pre_avg_se"] = raw.contains("pre_avg_se") ? raw["pre_avg_se"] : ojson(nullptr);
      block["post_avg"] = raw.contains("post_avg") ? raw["post_avg"] : ojson(nullptr);
      block["post_avg_se"] = raw.contains("post_avg_se") ? raw["post_avg_se"] : ojson(nullptr);
    }
  } else if (subcmd == "calendar" || subcmd == "group") {
    // calendar: ATT per outcome period, keyed "t". group: ATT per treatment
    // cohort, keyed "g" (distinct aggregations — the tables must differ).
    const char* arrKey = (subcmd == "group") ? "groups" : "calendar";
    const char* entKey = (subcmd == "group") ? "g" : "t";
    ojson rows = ojson::array();
    for (const auto& ent : raw.value(arrKey, ojson::array())) {
      ojson tVal = ent.value(entKey, ojson(nullptr));
      std::string name;
      if (tVal.is_number()) {
        double d = tVal.get<double>();
        if (d == std::floor(d)) {
          name = std::to_string(static_cast<long long>(d));
        } else {
          std::ostringstream oss;
          oss << d;
          name = oss.str();
        }
      } else if (tVal.is_string()) {
        name = tVal.get<std::string>();
      }
      rows.push_back(coefRowM4(name, ent.value("att", ojson(nullptr)), ent.value("se", ojson(nullptr))));
    }
    block["coeftable"] = rows;
  } else if (subcmd == "pretrend") {
    ojson pt = raw.value("pretrend", ojson::object());
    ojson pretrend;
    pretrend["chi2"] = pt.contains("chi2") ? pt["chi2"] : ojson(nullptr);
    pretrend["df"] = pt.contains("df") ? pt["df"] : ojson(nullptr);
    pretrend["p"] = pt.contains("p") ? pt["p"] : ojson(nullptr);
    block["pretrend"] = pretrend;
    block["coeftable"] = ojson::array();
  } else {
    block["coeftable"] = ojson::array();
  }
  return block;
}

// csdid-ematrix-post (w2ag_csdid_drdid): the engine already sends every posting
// Stata makes, with Stata's names (apps/cmds/cmds_panel.cpp cmd_csdid):
//   attgt          {names, b, V}  csdid's default e(b)/e(V): the K ATT(g,t) cells
//                                 `g#:t_#_#` then the K `wgt:w#_#` columns
//                                 (csdid; w2ag log:114 ag_def_names)
//   agg_<form>     {names, b, V}  agg(simple|event|group|calendar) (csdid)
//   estat_event    {e_b, e_V}     `estat event`'s block-diagonal posting
//                                 (csdid_estat.ado csdid_event; w2ag log:1384 v13 = 0)
// The V is copied verbatim: no PIET-side construction.
std::optional<CsdidPostingM4> csdidPostingM4(const ojson& raw, const std::string& key) {
  if (!raw.is_object() || !raw.contains(key) || !raw[key].is_object()) return std::nullopt;
  const ojson& p = raw[key];
  auto num = [](const ojson& x) -> double {
    return x.is_number() ? x.get<double>() : std::numeric_limits<double>::quiet_NaN();
  };
  CsdidPostingM4 P;
  const ojson* names = nullptr;
  const ojson* b = nullptr;
  const ojson* V = nullptr;
  if (key == "estat_event") {
    if (!p.contains("e_b") || !p["e_b"].is_object() || !p.contains("e_V") || !p["e_V"].is_object())
      return std::nullopt;
    names = p["e_b"].contains("names") ? &p["e_b"]["names"] : nullptr;
    b = p["e_b"].contains("values") ? &p["e_b"]["values"] : nullptr;
    V = p["e_V"].contains("matrix") ? &p["e_V"]["matrix"] : nullptr;
  } else {
    names = p.contains("names") ? &p["names"] : nullptr;
    b = p.contains("b") ? &p["b"] : nullptr;
    V = p.contains("V") ? &p["V"] : nullptr;
  }
  if (names == nullptr || b == nullptr || V == nullptr || !names->is_array() || !b->is_array() ||
      !V->is_array())
    return std::nullopt;
  const size_t K = names->size();
  if (K == 0 || b->size() != K || V->size() != K) return std::nullopt;
  for (const auto& n : *names) {
    if (!n.is_string()) return std::nullopt;
    P.names.push_back(n.get<std::string>());
  }
  for (const auto& x : *b) P.b.push_back(num(x));
  P.V.reserve(K * K);
  for (const auto& row : *V) {
    if (!row.is_array() || row.size() != K) return std::nullopt;
    for (const auto& x : row) P.V.push_back(num(x));
  }
  return P;
}

ojson csdidPostedCoeftableM4(const CsdidPostingM4& P) {
  ojson rows = ojson::array();
  const size_t K = P.names.size();
  for (size_t i = 0; i < K; ++i) {
    const double v = P.V[i * K + i];
    const double se = (std::isnan(v) || v < 0) ? std::numeric_limits<double>::quiet_NaN()
                                               : std::sqrt(v);
    rows.push_back(coefRowM4(P.names[i], std::isnan(P.b[i]) ? ojson(nullptr) : ojson(P.b[i]),
                             std::isnan(se) ? ojson(nullptr) : ojson(se)));
  }
  return rows;
}

void postCsdidPostingM4(SessionState& session, const CsdidPostingM4& P, const std::string& bName,
                        const std::string& vName) {
  auto setE = [&session](const std::string& nm, eval::EvalMatrix m) {
    for (auto& kv : session.eval.eMatrices) {
      if (kv.first == nm) {
        kv.second = std::move(m);
        return;
      }
    }
    session.eval.eMatrices.emplace_back(nm, std::move(m));
  };
  const size_t K = P.names.size();
  eval::EvalMatrix b;
  b.data.push_back(P.b);
  b.rownames = {"y1"};
  b.colnames = P.names;
  eval::EvalMatrix V;
  for (size_t i = 0; i < K; ++i)
    V.data.emplace_back(P.V.begin() + static_cast<std::ptrdiff_t>(i * K),
                        P.V.begin() + static_cast<std::ptrdiff_t>((i + 1) * K));
  V.rownames = P.names;
  V.colnames = P.names;
  setE(bName, std::move(b));
  setE(vName, std::move(V));
}

namespace {

std::string numTextM4b(double v) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.17g", v);
  return buf;
}

// csdid_ctl_cluster (w2l_vce_shape_det log:2031-2050): with cluster() naming
// the ivar, csdid.ado passes `_xtreg_chk_cl2`, runs every (g,t)
// cell as `capture drdid ... ivar() time() treatment() <method>` (no cluster is passed, each success posts e()), and only then fails r(198)
// at `collapse.., by(ivar)`, where the cluster variable is both
// a target and by(). csdid never re-posts, so e() is the LAST successful
// cell's drdid posting (e(cmd) drdid, e(N) of that cell, no vce/vcetype).
// This re-runs that cell through drdid's own fit (drdidFitCoreM4). The cell
// order is csdid.ado's: glev = the positive gvar levels, tlev = the time grid
// range(min, max, min step) without its first point (data_check);
// the short-gap base time1 starts at time0 and moves to j after every j < g,
// and long2 fixes it at g - gdelta over the whole grid. The comparison group is never-treated `inlist(gvar,0,g)` or
// not-yet-treated. Only dripw reaches the engine's drdid, so any
// other method (csdid's default is reg without covariates) leaves
// e() as it was; so does a cell sample the fit refuses (the next-to-last
// cell is tried, and so on, as csdid's `capture` skips a failed cell).
void csdidRepostLastCellM4(SessionState& session, const std::optional<std::string>& ifExpr,
                           const std::string& y, const std::vector<std::string>& xs,
                           const std::string& ivar, const std::string& time,
                           const std::string& gvar, const std::optional<std::string>& method,
                           const std::optional<std::string>& comparison, bool long2) {
  const std::string m = (method.has_value() && !method->empty()) ? *method
                        : (xs.empty() ? std::string("reg") : std::string("dripw"));
  if (m != "dripw") return;
  const bool notyet = comparison.has_value() && *comparison == "notyet";

  ojson ifErr;
  auto basePathOpt = applyIfFilterM4(session, *session.currentHbin(), ifExpr, ifErr);
  if (!basePathOpt.has_value()) return;
  const std::string basePath = *basePathOpt;
  const bool madeIfTemp = basePath != *session.currentHbin();
  std::optional<Datastore> tempDs;
  Datastore* ds = nullptr;
  if (madeIfTemp) {
    tempDs.emplace(basePath);
    ds = &*tempDs;
  } else {
    ds = session.datastore();
  }
  std::set<double> tSet, gSet;
  bool ok = ds != nullptr;
  if (ok) {
    std::vector<std::string> cols = {y, ivar, time, gvar};
    cols.insert(cols.end(), xs.begin(), xs.end());
    std::vector<const double*> ptr;
    int64_t n = -1;
    for (const auto& c : cols) {
      if (!ds->has(c) || ds->dtype(c) != "f64") {
        ok = false;
        break;
      }
      const cecon::Column& col = ds->col(c);
      ptr.push_back(col.f64());
      n = col.nbytes / static_cast<int64_t>(sizeof(double));
    }
    if (ok) {
      const double* tcol = ptr[2];
      const double* gcol = ptr[3];
      for (int64_t r = 0; r < n; ++r) {
        bool miss = false;
        for (const double* p : ptr) miss = miss || std::isnan(p[r]);
        if (miss) continue;
        tSet.insert(tcol[r]);
        if (gcol[r] > 0) gSet.insert(gcol[r]);
      }
    }
  }
  tempDs.reset();
  if (madeIfTemp) cecon::native_unlink(basePath);
  if (!ok || tSet.size() < 2 || gSet.empty()) return;

  const std::vector<double> tv(tSet.begin(), tSet.end());
  double dt = std::numeric_limits<double>::infinity();
  for (size_t k = 1; k < tv.size(); ++k) dt = std::min(dt, tv[k] - tv[k - 1]);
  std::vector<double> grid;  // range(min, max, dt)
  for (int64_t k = 0;; ++k) {
    const double v = tv.front() + static_cast<double>(k) * dt;
    if (v > tv.back() + 1e-9 * std::max(1.0, std::fabs(tv.back()))) break;
    grid.push_back(v);
  }
  struct Cell {
    double g, t1, j;
  };
  std::vector<Cell> cells;
  for (double g : gSet) {
    if (!long2) {
      double time1 = grid.front();
      for (size_t k = 1; k < grid.size(); ++k) {
        const double j = grid[k];
        cells.push_back({g, time1, j});
        if (j < g) time1 = j;
      }
    } else {
      const double time1 = g - dt;
      for (double j : grid)
        if (time1 != j) cells.push_back({g, time1, j});
    }
  }
  const std::string base = (ifExpr.has_value() && !trimM4(*ifExpr).empty())
                               ? "(" + *ifExpr + ") & "
                               : std::string();
  for (auto it = cells.rbegin(); it != cells.rend(); ++it) {
    const std::string g = numTextM4b(it->g), j = numTextM4b(it->j), t1 = numTextM4b(it->t1);
    const std::string gsel =
        notyet ? "(" + gvar + "==0 | " + gvar + "==" + g + " | (" + gvar + ">" + g + " & " + gvar +
                     ">" + j + "))"
               : "inlist(" + gvar + ",0," + g + ")";
    const std::string cellIf = base + gsel + " & inlist(" + time + "," + t1 + "," + j + ")";
    // The treatment is csdid's tempvar `tr` = (gvar == g); the
    // ATET column carries a tempvar-shaped name, as Stata's does.
    const ojson res = drdidFitCoreM4(session, y, xs, ivar, time, gvar + "==" + g, "__000000",
                                     cellIf);
    if (res.value("type", std::string()) != "error") return;
  }
}

}  // namespace

ojson runCsdid(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  if (parsed.varlist.empty()) {
    return engineErrorBlockM4("syntax_error: csdid requires a depvar", "syntax_error");
  }
  std::string y = parsed.varlist[0];
  std::vector<std::string> xs(parsed.varlist.begin() + 1, parsed.varlist.end());

  std::optional<std::string> ivar, time, gvar, method, comparison, pscoretrim, window;
  bool long2 = false;
  std::string agg;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "ivar" && !v.isBool) ivar = v.strValue;
    if (k == "time" && !v.isBool) time = v.strValue;
    if (k == "gvar" && !v.isBool) gvar = v.strValue;
    if (k == "method" && !v.isBool) method = v.strValue;
    if (k == "comparison" && !v.isBool) comparison = v.strValue;
    if (k == "pscoretrim" && !v.isBool) pscoretrim = v.strValue;
    if (k == "window" && !v.isBool) window = v.strValue;
    // csdid `long2` — Callaway-Sant'Anna long-difference (fixed g-1 base)
    // event-study parametrization. Bare flag; pass through to the engine.
    if (k == "long2") long2 = true;
    // csdid-notyet-agg: Stata's own spelling of the not-yet-treated comparison
    // is the bare flag `notyet` (csdid; w2ag log:451 ag_ny rc 0,
    // e(control_group) "Not yet Treated"). PIET's `comparison()` stays.
    if (k == "notyet") comparison = std::string("notyet");
    if (k == "agg" && !v.isBool) agg = trimM4(v.strValue);
  }
  // FU-23 (BUG-757): cluster() by shape, w2l_vce_shape.log. A bare `cluster`
  // and an empty `cluster()` are ignored (csdid_bare_cluster/_empty_cluster rc 0,
  // the default fit); two variables are r(103) "cluster():  too many variables
  // specified" (csdid_opt_c2), where this loop used to forward the FIRST one.
  // vce()/robust are ignored by csdid whatever they say (the vocabulary note at
  // the top of the file), so they are read nowhere.
  const VceShapesM4b vceShape = readVceShapesM4b(parsed);
  std::optional<std::string> cluster;
  if (vceShape.cluster == OptShapeM4b::kMany) {
    return stataRefusalM4b("cluster():  too many variables specified", 103);
  }
  if (vceShape.cluster == OptShapeM4b::kOne) cluster = vceShape.clusterVars[0];
  if (!ivar.has_value() || ivar->empty()) {
    return engineErrorBlockM4(
        "csdid requires `ivar(varname)` — e.g. `csdid y x, ivar(id) time(t) gvar(g) method(dripw)`.",
        "unsupported_option");
  }
  if (!time.has_value() || time->empty()) {
    return engineErrorBlockM4(
        "csdid requires `time(varname)` — e.g. `csdid y x, ivar(id) time(t) gvar(g) method(dripw)`.",
        "unsupported_option");
  }
  if (!gvar.has_value() || gvar->empty()) {
    return engineErrorBlockM4(
        "csdid requires `gvar(varname)` — e.g. `csdid y x, ivar(id) time(t) gvar(g) method(dripw)`.",
        "unsupported_option");
  }
  // csdid-notyet-agg, csdid: an empty agg() is attgt; anything but
  // the five forms is "Aggregation not Allowed", exit 10 (the ado compares the
  // text as typed, so `agg(Simple)` is refused too).
  if (agg.empty()) agg = "attgt";
  if (agg != "attgt" && agg != "simple" && agg != "group" && agg != "calendar" &&
      agg != "event") {
    return stataRefusalM4b("Aggregation not Allowed", 10);
  }

  std::vector<std::string> argv = {"csdid", "--y", y, "--ivar", *ivar, "--time", *time, "--gvar", *gvar};
  if (!xs.empty()) {
    argv.push_back("--x");
    argv.push_back(joinCommaM4(xs));
  }
  if (method.has_value() && !method->empty()) {
    argv.push_back("--method");
    argv.push_back(*method);
  }
  if (comparison.has_value() && !comparison->empty()) {
    argv.push_back("--comparison");
    argv.push_back(*comparison);
  }
  if (pscoretrim.has_value() && !pscoretrim->empty()) {
    argv.push_back("--pscoretrim");
    argv.push_back(*pscoretrim);
  }
  if (window.has_value() && !window->empty()) {
    std::istringstream iss(*window);
    std::vector<std::string> parts;
    std::string tok;
    while (iss >> tok) parts.push_back(tok);
    std::string joined;
    for (size_t i = 0; i < parts.size(); ++i) {
      if (i) joined += ",";
      joined += parts[i];
    }
    argv.push_back("--window");
    argv.push_back(joined.empty() ? *window : joined);
  }
  if (parsed.ifExpr.has_value()) {
    argv.push_back("--keepif");
    argv.push_back(*parsed.ifExpr);
  }
  bool clusterIsIvar = false;
  if (cluster.has_value()) {
    // csdid_ctl_cluster: `cluster(id)` with `ivar(id)` is r(198) "id may not be
    // both target and by()". The engine refuses the same literal name match
    // (apps/cmds/cmds_panel.cpp, "name the same variable"), but its error left
    // here as rc 1; the rule is on the name token, so it is checked here.
    // The refusal itself comes after the option net below, with csdid's
    // last drdid cell posted first (csdidRepostLastCellM4).
    if (*cluster == trimM4(*ivar)) {
      clusterIsIvar = true;
    } else {
      argv.push_back("--cluster");
      argv.push_back(*cluster);
    }
  }
  if (long2) argv.push_back("--long2");

  // Audit fix: every neighboring estimator in this file (ivregress/reg3/
  // etc.) closes its option parsing with eval::rejectUnknownOptions —
  // csdid never did, so a typo (e.g. `methdo(dripw)` instead of
  // `method(dripw)`) silently fell through the opts loop above (none of
  // its `if (k == ...)` arms match, so the mistyped key is simply never
  // read) instead of erroring, producing a plausible-looking but WRONG
  // result (a different, unintended specification) rather than naming the
  // bad option.
  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kCsdidKnownOpts = kVocabCsdid.known;
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kCsdidKnownOpts)) {
    return *err;
  }
  if (clusterIsIvar) {
    csdidRepostLastCellM4(session, parsed.ifExpr, y, xs, trimM4(*ivar), trimM4(*time),
                          trimM4(*gvar), method, comparison, long2);
    return stataRefusalM4b(*cluster + " may not be both target and by()", 198);
  }

  argv.push_back("--data");
  argv.push_back(*session.currentHbin());
  argv.push_back("--tidy");

  ojson raw;
  try {
    raw = runCobraJsonM4(argv);
  } catch (const CobraError& ce) {
    // csdid_ctl_cluster_id2: a cluster variable that varies within a panel is
    // r(498) "panels are not nested within clusters". The engine detects it
    // (apps/cmds/cmds_panel.cpp, "is not nested within --ivar") but its
    // bad_input code has no rc of its own, so it became rc 1. The engine's
    // message is the only signal this side receives; it is matched on the
    // clause that names the rule.
    if (cluster.has_value() &&
        std::string(ce.what()).find("is not nested within --ivar") != std::string::npos) {
      return stataRefusalM4b("panels are not nested within clusters", 498);
    }
    return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
  }
  ojson block = csdidToEstimationM4(raw, "simple");
  // csdid-ematrix-post / csdid-notyet-agg: e(b)/e(V) are the posting agg()
  // asked for (csdid `ereturn post b1 s1`), and the coefficient
  // table is its columns, as Stata prints it: `_b[g4:t_3_4]` answers after a
  // default fit and `_b[ATT]` is r(111) "[ATT] not found" (w2ag log:136, :141).
  const std::string postKey = agg == "attgt" ? std::string("attgt") : "agg_" + agg;
  const std::optional<CsdidPostingM4> posting = csdidPostingM4(raw, postKey);
  if (!posting.has_value() && agg != "attgt") {
    return engineErrorBlockM4("csdid, agg(" + agg + "): the engine reply has no `" + postKey +
                                  "` posting (cobra too old); nothing was posted.",
                              "engine_error");
  }
  if (posting.has_value()) block["coeftable"] = csdidPostedCoeftableM4(*posting);
  block["agg"] = agg;
  // eb-order T-P3 part 0: the block is written AFTER the fresh setLastModel
  // (inside cacheCsdidModel), which clears the previous fit's block.
  cacheCsdidModel(session, argv, raw);
  session.lastEstBlock = block;
  if (posting.has_value()) postCsdidPostingM4(session, *posting);
  // e(b_attgt)/e(V_attgt) carry the cells whatever agg() posted (csdid).
  if (auto cells = csdidPostingM4(raw, "attgt")) postCsdidPostingM4(session, *cells, "b_attgt", "V_attgt");
  // csdid, the e() macros (w2ag log:120 ag_def_emac
  // `attgt|reg|Never Treated|default|`, :548 ag_l2_emac `...|long2|`, :404
  // ag_cl_vce `|||cl|15|`).
  // Each is recorded on top of the fit (noteEOnFit) so the NEXT model install
  // drops it: e(agg) must not answer after a later `regress`.
  auto setOnFit = [&session](const std::string& name, const std::string& value) {
    session.eval.setEString(name, value);
    session.noteEOnFit(name);
  };
  setOnFit("estat_cmd", "csdid_estat");
  setOnFit("agg", agg);
  setOnFit("ggroup", *gvar);
  setOnFit("id", *ivar);
  setOnFit("method", raw.value("method", method.value_or(std::string())));
  setOnFit("typebase", long2 ? "long2" : "default");
  if (cluster.has_value()) setOnFit("clustvar", *cluster);
  {
    const std::string cmp = raw.value("comparison", comparison.value_or(std::string("never")));
    setOnFit("control_group", cmp == "notyet" ? "Not yet Treated" : "Never Treated");
  }
  session.noteEOnFit("b_attgt");
  session.noteEOnFit("V_attgt");
  return block;
}

// ===========================================================================
// ivregress / ivreghdfe / reg3 (BETA D1 batch: BS-063/BS-064/BS-065)

namespace est_detail {  // R2: reopened — splitTokensM4/parseAtNumlistM4 cross to TU c

// Splits a plain string on whitespace AND commas — the same token grammar
// the tokenizer's own splitWs already applied to ParsedCommand::varlist
// (each `(endog = instruments)`/reg3-equation substring below is a plain
// std::string this file reconstructs BY HAND from already-split varlist
// tokens, joined back with single spaces — see parseIvParenM4's doc comment
// — so it needs the identical space-or-comma splitter applied a second
// time, not a paren-aware re-tokenize).
std::vector<std::string> splitTokensM4(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == ' ' || c == '\t' || c == ',') {
      if (!cur.empty()) {
        out.push_back(cur);
        cur.clear();
      }
    } else {
      cur += c;
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

std::string joinSpaceM4(const std::vector<std::string>& v) {
  std::string s;
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) s += " ";
    s += v[i];
  }
  return s;
}

// P7: Stata's `at()` value-list grammar, `at(v1=(a b c) v2=#)`. Returns ONE
// engine `--at` spec per point in the cartesian product, each spelled
// `v=val[,v2=val2]` with the value at %.17g. The grammar is the kernel's own
// (cecon::margins_parse_at, the parser the margins grid uses): this function
// used to carry a second one that closed a parenthesised list at its first
// `)`, so after logit/probit/poisson `at(c1=(1(1)3))` was the one point
// `c1=1(1` and `at(c1=(1/3))` the one point `c1=1/3`, both at rc 0 (Stata: three
// points, w2v_margins_siblings.log:2961-3012 F_range_numlist_lg, :3018-3069
// F_range_slash_lg). A list the kernel rejects gives no spec; the caller
// refuses it by name with the kernel's rc (runMargins, marginsAtRefusalM4).
std::vector<std::string> parseAtNumlistM4(const std::string& at) {
  const cecon::MarginsAtParse p = cecon::margins_parse_at(at);
  std::vector<std::string> out;
  if (p.rc != 0) return out;
  for (const auto& pt : p.points) {
    std::string spec;
    for (const auto& [var, val] : pt.fix) {
      char buf[40];
      std::snprintf(buf, sizeof(buf), "%.17g", val);
      spec += (spec.empty() ? "" : ",") + var + "=" + buf;
    }
    out.push_back(spec);
  }
  return out;
}

// BS-063: `ivregress 2sls|gmm|liml depvar [exog...] (endog = instruments)` /
// `ivreghdfe depvar [exog...] (endog = instruments), absorb(F)` / the
// `reghdfe depvar (endog=instruments), absorb(F)` paren form all share this
// ONE varlist shape: [depvar, exog...]  "(" endog "=" instruments ")"
// [exog...] (the trailing list only where exogAfterParen says so).
// splitWs() (eval/tokenizer.cpp) is whitespace-only, so it already broke
// "(beertax = spirits unemp)" into SEPARATE varlist tokens ("(beertax", "=",
// "spirits", "unemp)") — this rejoins them with single spaces (Stata's own
// whitespace inside the parens carries no meaning) and re-splits on the
// paren/equals structure instead.
struct IvParenResult {
  std::string y;
  std::vector<std::string> exog;
  std::vector<std::string> endog;
  std::vector<std::string> instruments;
  bool errored = false;
  std::string errorMsg;
};

IvParenResult parseIvParenM4(const std::vector<std::string>& tokens, bool exogAfterParen) {
  IvParenResult r;
  std::string joined = joinSpaceM4(tokens);
  size_t lp = joined.find('(');
  if (lp == std::string::npos) {
    r.errored = true;
    r.errorMsg = "requires an endogenous-regressors block: (endog = instruments)";
    return r;
  }
  size_t rpLast = joined.rfind(')');
  if (rpLast == std::string::npos || rpLast < lp) {
    r.errored = true;
    r.errorMsg = "malformed (endog = instruments) block: missing closing ')'";
    return r;
  }
  std::string beforeStr = joined.substr(0, lp);
  std::string insideStr = joined.substr(lp + 1, rpLast - lp - 1);
  std::string afterStr = joined.substr(rpLast + 1);
  // Only ONE parenthesised block is valid ivregress/ivreghdfe syntax — a
  // stray extra '('/')' anywhere else (before/inside/after) means either a
  // second block or an unbalanced paren; reject rather than silently
  // mis-slicing the wrong substring as the endog/instrument list.
  if (beforeStr.find(')') != std::string::npos || insideStr.find('(') != std::string::npos ||
      insideStr.find(')') != std::string::npos || afterStr.find('(') != std::string::npos ||
      afterStr.find(')') != std::string::npos) {
    r.errored = true;
    r.errorMsg = "malformed syntax: only one (endog = instruments) block is supported";
    return r;
  }
  std::vector<std::string> before = splitTokensM4(beforeStr);
  std::vector<std::string> after = splitTokensM4(afterStr);
  if (before.empty()) {
    r.errored = true;
    r.errorMsg = "requires a dependent variable before the (endog = instruments) block";
    return r;
  }
  if (!after.empty() && !exogAfterParen) {
    r.errored = true;
    r.errorMsg = "unexpected tokens after the (endog = instruments) block: " + joinSpaceM4(after);
    return r;
  }
  size_t eq = insideStr.find('=');
  if (eq == std::string::npos) {
    r.errored = true;
    r.errorMsg = "malformed (endog = instruments) block: missing '='";
    return r;
  }
  if (insideStr.find('=', eq + 1) != std::string::npos) {
    r.errored = true;
    r.errorMsg = "malformed (endog = instruments) block: only one '=' is allowed";
    return r;
  }
  std::vector<std::string> endog = splitTokensM4(insideStr.substr(0, eq));
  std::vector<std::string> instr = splitTokensM4(insideStr.substr(eq + 1));
  if (endog.empty() || instr.empty()) {
    r.errored = true;
    r.errorMsg = "malformed (endog = instruments) block: need at least one endogenous regressor "
                 "and at least one instrument";
    return r;
  }
  r.y = before[0];
  r.exog.assign(before.begin() + 1, before.end());
  // Exogenous regressors typed AFTER the block join the ones typed before it,
  // in typed order: w2b_estim.log:923-945 `ivregress 2sls y (x1 = z1) x2` fits
  // the `y x2 (x1 = z1)` model (b_x1 1.940390224456596 both ways, "Exogenous:
  // x2 z1"), and :1185/:1243 do the same for ivreghdfe.
  r.exog.insert(r.exog.end(), after.begin(), after.end());
  r.endog = endog;
  r.instruments = instr;
  return r;
}

}  // namespace est_detail

namespace {

// ── `first` / ivreg2 `partial()` helpers (task w2b-iv-options) ──────────────

// Stata's coefficient-table name column: 12 wide, right-aligned, long names
// abbreviated with `~` the way the regress family does.
std::string coefStubM4b(const std::string& name) {
  std::string n = (name == "(intercept)") ? std::string("_cons") : name;
  // Widths count CHARACTERS (StataBE, scratch-r150/uni-v/p4.do): a name past 12
  // keeps its first 10 characters, `~` and its last character, never splitting a
  // UTF-8 sequence.
  const size_t cps = cecon::ident::codePointCount(n);
  if (cps > 12) {
    size_t last = n.size();
    do {
      --last;
    } while (last > 0 && (static_cast<unsigned char>(n[last]) & 0xC0) == 0x80);
    n = cecon::ident::truncateCodePoints(n, 10) + "~" + n.substr(last);
    return n;  // exactly 12 characters
  }
  return std::string(12 - cps, ' ') + n;
}

std::string padLeftM4b(const std::string& s, size_t w) {
  const size_t n = cecon::ident::codePointCount(s);
  return n >= w ? s : std::string(w - n, ' ') + s;
}

double numberAtM4b(const ojson& obj, const char* key, const std::string& name) {
  if (!obj.is_object() || !obj.contains(key) || !obj[key].is_object()) return std::nan("");
  const ojson& m = obj[key];
  if (!m.contains(name) || !m[name].is_number()) return std::nan("");
  return m[name].get<double>();
}

// One first stage's coefficient table, 78 columns, from the engine's own
// `--savefirst` estimate (apps/cmds/common.cpp add_first_stage: b, se, t, p and
// the CI off df_r): the cluster note, the rules and one row per name in `names`
// order. Shared by firstStageTextM4b and ivregress's report (ivregressFirstHeaderM4b).
std::string firstStageTableM4b(const ojson& fs, const std::vector<std::string>& names,
                               const std::string& vcov, const std::string& clustvar) {
  const std::string depvar = fs.value("depvar", std::string());
  std::string out;
  char buf[160];
  const bool robust = vcov != "iid" && !vcov.empty();
  if (vcov == "cluster" && fs.contains("n_clusters") && fs["n_clusters"].is_number()) {
    const std::string adj = "(Std. err. adjusted for " +
                            std::to_string(static_cast<long long>(fs["n_clusters"].get<double>())) +
                            " clusters in " + clustvar + ")";
    out += padLeftM4b(adj, 78) + "\n";
  }
  out += std::string(78, '-') + "\n";
  if (robust) out += "             |               Robust\n";
  out += coefStubM4b(depvar) + " | Coefficient  " + (robust ? "std. err." : "Std. err.") +
         "      t    P>|t|     [95% conf. interval]\n";
  out += std::string(13, '-') + "+" + std::string(64, '-') + "\n";
  // The cells are the coefficient table's %9.0g (w2b_estim.log:896 `_cons |
  // 2.505772   1.106975 ... .2890918    4.722452`), not display's default, which
  // gives 2.5057717 (BUG-1548).
  auto cell = [](double v) { return fmt::general(v, 9, 0); };
  for (const auto& name : names) {
    const double b = numberAtM4b(fs, "coef", name);
    const double se = numberAtM4b(fs, "se", name);
    const double t = numberAtM4b(fs, "tstat", name);
    const double p = numberAtM4b(fs, "pvalue", name);
    const double lo = numberAtM4b(fs, "ci_lo", name);
    const double hi = numberAtM4b(fs, "ci_hi", name);
    std::string row = coefStubM4b(name) + " |";
    if (!std::isfinite(b) || !std::isfinite(se)) {
      row += padLeftM4b("(omitted)", 11);
    } else {
      row += padLeftM4b(cell(b), 11) + padLeftM4b(cell(se), 11);
      std::snprintf(buf, sizeof(buf), "%9.2f%8.3f", t, p);
      row += buf;
      row += padLeftM4b(cell(lo), 13) + padLeftM4b(cell(hi), 12);
    }
    out += row + "\n";
  }
  out += std::string(78, '-');
  return out;
}

// One first stage as ivreghdfe/ivreg2's `first` prints it here: a title, the
// stage's Number of obs and Root MSE, then its table (firstStageTableM4b). Only
// what the engine's estimate carries is printed. `names` is the row order.
std::string firstStageTextM4b(const ojson& fs, const std::vector<std::string>& names,
                              const std::string& vcov, const std::string& clustvar) {
  const std::string depvar = fs.value("depvar", std::string());
  std::string out = "first-stage regression of " + depvar + " (option first)\n";
  char buf[160];
  if (fs.contains("n_obs") && fs["n_obs"].is_number()) {
    std::snprintf(buf, sizeof(buf), "%56s%-14s= %6lld\n", "", "Number of obs",
                  static_cast<long long>(fs["n_obs"].get<double>()));
    out += buf;
  }
  if (fs.contains("rmse") && fs["rmse"].is_number()) {
    std::snprintf(buf, sizeof(buf), "%56s%-14s= %6.4f\n", "", "Root MSE", fs["rmse"].get<double>());
    out += buf;
  }
  out += "\n";
  return out + firstStageTableM4b(fs, names, vcov, clustvar);
}

// The header ivregress prints above each first-stage table (ivregress's `first` report):
// Number of obs, F(df_m,
// df_r), Prob > F, R-squared, Adj R-squared, Root MSE (StataBE 19.5, BUG-1227;
// w2b_estim.log:883-890 `ivregress 2sls y x2 (x1 = z1), first`: F(2, 57) 4.11, Prob >
// F 0.0215, R-squared 0.1261, Adj R-squared 0.0954, Root MSE 3.0236). From the
// engine's stage: n_obs, df_r (N - K), rss, rmse and e_V; `tss` is the stage depvar's
// sum of squares over e(sample), centred when the stage has a constant and raw under
// noconstant, as regress takes it (NaN when not known: the two R-squared lines then
// read `.`; `ivregress ..., first noconstant` R-squared .7476, Adj R-squared .7389 =
// 1 - (1 - R2) N / df_r). F is the Wald test of every slope with the stage's own VCE
// (ivregress's first-stage `test`), which under the default VCE is regress's
// model F; Prob > F is Ftail(q, df_r, F), df_r staying N - K under cluster().
std::string ivregressFirstHeaderM4b(const ojson& fs, double tss, bool hasCons) {
  auto num = [&fs](const char* k) {
    return fs.contains(k) && fs[k].is_number() ? fs[k].get<double>() : std::nan("");
  };
  const double n = num("n_obs"), dfR = num("df_r"), rss = num("rss"), rmse = num("rmse");
  // Wald F over the slopes: b_s' V_ss^{-1} b_s / q, a slope with no positive
  // variance (omitted) left out of the test as `test` drops it.
  double F = std::nan(""), q = 0;
  if (fs.contains("e_V") && fs["e_V"].is_object() && fs["e_V"].contains("names") &&
      fs["e_V"].contains("matrix")) {
    const ojson& vn = fs["e_V"]["names"];
    const ojson& vm = fs["e_V"]["matrix"];
    std::vector<size_t> idx;
    for (size_t i = 0; i < vn.size(); ++i) {
      const std::string nm = vn[i].is_string() ? vn[i].get<std::string>() : std::string();
      if (nm.empty() || nm == "(intercept)" || nm == "_cons") continue;
      if (i < vm.size() && vm[i].is_array() && i < vm[i].size() && vm[i][i].is_number() &&
          vm[i][i].get<double>() > 0 && std::isfinite(numberAtM4b(fs, "coef", nm)))
        idx.push_back(i);
    }
    const size_t k = idx.size();
    std::vector<std::vector<double>> a(k, std::vector<double>(k + 1, 0.0));
    bool ok = k > 0;
    for (size_t r = 0; ok && r < k; ++r) {
      for (size_t c = 0; c < k; ++c) {
        const ojson& cell = vm[idx[r]][idx[c]];
        if (!cell.is_number()) ok = false;
        else a[r][c] = cell.get<double>();
      }
      a[r][k] = numberAtM4b(fs, "coef", vn[idx[r]].get<std::string>());
    }
    // Gaussian elimination with partial pivoting: x = V_ss^{-1} b_s.
    for (size_t c = 0; ok && c < k; ++c) {
      size_t piv = c;
      for (size_t r = c + 1; r < k; ++r)
        if (std::fabs(a[r][c]) > std::fabs(a[piv][c])) piv = r;
      if (!(std::fabs(a[piv][c]) > 0)) {
        ok = false;
        break;
      }
      std::swap(a[c], a[piv]);
      for (size_t r = 0; r < k; ++r) {
        if (r == c) continue;
        const double f = a[r][c] / a[c][c];
        for (size_t j = c; j <= k; ++j) a[r][j] -= f * a[c][j];
      }
    }
    if (ok) {
      double w = 0;
      for (size_t r = 0; r < k; ++r)
        w += numberAtM4b(fs, "coef", vn[idx[r]].get<std::string>()) * (a[r][k] / a[r][r]);
      q = static_cast<double>(k);
      F = w / q;
    }
  }
  double p = std::nan("");
  if (std::isfinite(F) && q > 0 && std::isfinite(dfR) && dfR > 0) {
    char expr[160];
    std::snprintf(expr, sizeof(expr), "Ftail(%.17g,%.17g,%.17g)", q, dfR, F);
    if (const auto v = eval::evalArith(expr, nullptr)) {
      try {
        p = std::stod(*v);
      } catch (...) {
      }
    }
  }
  const double r2 = (std::isfinite(tss) && tss > 0 && std::isfinite(rss)) ? 1.0 - rss / tss
                                                                         : std::nan("");
  const double r2a = std::isfinite(r2) && dfR > 0
                         ? 1.0 - (1.0 - r2) * (hasCons ? n - 1.0 : n) / dfR
                         : std::nan("");
  char buf[160];
  auto line = [&buf](const std::string& label, const char* fmt, double v) {
    char val[64];
    if (std::isfinite(v)) std::snprintf(val, sizeof(val), fmt, v);
    else std::snprintf(val, sizeof(val), "%6s", ".");
    std::snprintf(buf, sizeof(buf), "%56s%-14s= %s\n", "", label.c_str(), val);
    return std::string(buf);
  };
  std::string out;
  if (std::isfinite(n)) {
    // _coef_table_header's %#.0fc: thousands separated by commas.
    std::string digits = std::to_string(static_cast<long long>(n));
    for (int i = static_cast<int>(digits.size()) - 3; i > 0; i -= 3) digits.insert(i, ",");
    std::snprintf(buf, sizeof(buf), "%56s%-14s= %6s\n", "", "Number of obs", digits.c_str());
    out += buf;
  }
  char fl[64];
  std::snprintf(fl, sizeof(fl), "F(%.0f, %.0f)", q, dfR);
  out += line(fl, "%6.2f", F);
  out += line("Prob > F", "%6.4f", p);
  out += line("R-squared", "%6.4f", r2);
  out += line("Adj R-squared", "%6.4f", r2a);
  out += line("Root MSE", "%6.4f", rmse);
  return out;
}

// ivreg2's `ffirst` output: the summary block of the first-stage statistics, one row
// per endogenous regressor, from the engine's e(first) column (`ffirst`, rows in
// cecon::ivreg2_first_stat_rows() order: F df df_r pvalue at 3-6, SW chi2/p at 11-12,
// SW F at 7). The full first-stage tables are `first`'s, not printed here.
std::string ffirstSummaryTextM4b(const ojson& stages, const std::vector<std::string>& endog) {
  std::string out = "Summary results for first-stage regressions\n"
                    "-------------------------------------------\n\n";
  char buf[200];
  for (const auto& e : endog) {
    if (!stages.contains(e) || !stages[e].contains("ffirst") || !stages[e]["ffirst"].is_array() ||
        stages[e]["ffirst"].size() != 21)
      continue;
    const ojson& f = stages[e]["ffirst"];
    auto at = [&](size_t i) { return f[i].is_number() ? f[i].get<double>() : std::nan(""); };
    // The name column is 12 CHARACTERS wide, left-aligned (not bytes).
    std::string eStub = cecon::ident::truncateCodePoints(e, 12);
    eStub += std::string(12 - cecon::ident::codePointCount(eStub), ' ');
    std::snprintf(buf, sizeof(buf),
                  "%s | F(%3.0f,%6.0f) = %9.2f  P-val = %6.4f | SW Chi-sq(%3.0f) = %9.2f  P-val = "
                  "%6.4f | SW F(%3.0f,%6.0f) = %9.2f\n",
                  eStub.c_str(), at(4), at(5), at(3), at(6), at(8), at(11), at(12), at(8),
                  at(9), at(7));
    out += buf;
  }
  return out;
}

// ivreg2 `savefirst`: one first stage stored as `estimates store NAME, title("First-
// stage regression: ENDOG")` (ivreg2), from the engine's stage (b, se,
// e_V, N, rmse, df_r, df_m, k_eq, n_clusters; apps/cmds/common.cpp add_first_stage).
// The stage is made the active e() for the store and then superseded by the parent
// fit, as in ivreg2, which stores the stages before it posts its own results: the
// parent's `lm` is copied for the data, sample, weights and VCE (the first stage is
// `regress ENDOG EXEXOG INEXOG` under the parent's vce, so predict after `estimates
// restore` replays that regression), with a fresh fit serial. Every e() name it posts
// beyond setLastModel's own is noted on the fit, so the parent's setLastModel drops
// them again. e() as the oracle's probe lists it: cmd ivreg2, model first, depvar,
// N, rmse, df_r, df_m, k_eq, N_clust, vce `robust cluster`, vcetype Robust, clustvar.
// xtivreg2 (`xt` non-null) re-stores each stage with e(cmd) xtivreg2, the panel
// scalars and an EMPTY title (xtivreg2: the stage is restored, re-posted
// and stored with title(`e(_estimates_title)'), which a restore does not set;
// w2d_uncovered_xtivreg2.log:2714 `st1x1 | xtivreg2  x1  3` with no title).
// Returns the error block of a failed store, or nullopt.
struct XtivPanelM4b {
  double N_g = 0, g_min = 0, g_avg = 0, g_max = 0;
  bool clustered = false;
  std::string ivar, tvar;
};
std::optional<ojson> storeIvreg2FirstStageM4b(const ojson& fs, const LastModel& parent,
                                              SessionState& session,
                                              const XtivPanelM4b* xt = nullptr) {
  const std::string endog = fs.value("depvar", std::string());
  const std::string name = fs.value("eqname", std::string());
  if (endog.empty() || name.empty() || !fs.contains("names") || !fs["names"].is_array())
    return engineErrorBlockM4("savefirst: the engine's first stage is incomplete", "engine_error");
  LastModel f = parent;
  f.fitSerial = LastModel().fitSerial;
  f.cobraCmd = "feols";
  f.stataCmd = xt != nullptr ? "xtivreg2" : "ivreg2";
  f.y = endog;
  f.depvar = endog;
  // The regressors as typed (factor/operator tokens intact, as every replay takes
  // them): excluded instruments, then the included exogenous ones.
  f.x = parent.ivInstruments;
  for (const auto& x : parent.ivExog) f.x.push_back(x);
  f.ivEndog.clear();
  f.ivExog.clear();
  f.ivInstruments.clear();
  f.fe.clear();
  f.eVce.reset();
  ojson ct = ojson::array();
  for (const auto& n : fs["names"]) {
    if (!n.is_string()) continue;
    const std::string nm = n.get<std::string>();
    auto num = [&](const char* key) -> ojson {
      return fs.contains(key) && fs[key].is_object() && fs[key].contains(nm) ? fs[key][nm]
                                                                             : ojson(nullptr);
    };
    ct.push_back(ojson{{"name", nm}, {"b", num("coef")}, {"se", num("se")}, {"t", num("tstat")},
                       {"p", num("pvalue")}, {"ci_lo", num("ci_lo")}, {"ci_hi", num("ci_hi")}});
  }
  ojson raw = fs;
  raw["coeftable"] = ct;
  raw["estimator"] = "ivreg2";
  raw.erase("ffirst");
  raw.erase("rss");  // ivreg2's stored first stage posts no e(rss)
  f.result = raw;
  ojson block = cobraToEstimationM4(raw, "feols", &session);
  session.setLastModel(f);
  session.lastEstBlock = block;
  session.attachFitCovariance(raw);
  auto put = [&](const std::string& nm, const char* key) {
    if (fs.contains(key) && fs[key].is_number()) {
      session.eval.setEScalar(nm, fs[key].get<double>());
      session.noteEOnFit(nm);
    }
  };
  put("N", "n_obs");
  put("rmse", "rmse");
  put("df_r", "df_r");
  put("df_m", "df_m");
  put("k_eq", "k_eq");
  auto putS = [&](const std::string& nm, const std::string& v) {
    session.eval.setEString(nm, v);
    session.noteEOnFit(nm);
  };
  putS("model", "first");
  if (xt != nullptr) {
    session.eval.setEString("cmd", "xtivreg2");
    auto putD = [&](const std::string& nm, double v) {
      session.eval.setEScalar(nm, v);
      session.noteEOnFit(nm);
    };
    const double dfb = fs.contains("df_m") && fs["df_m"].is_number() ? fs["df_m"].get<double>()
                                                                     : std::nan("");
    putD("N_g", xt->N_g);
    putD("df_a", xt->N_g);
    putD("df_b", dfb);
    // :519-524: FEs with cluster use up no degrees of freedom.
    putD("df_m", xt->clustered ? dfb : xt->N_g + dfb);
    if (fs.contains("rmse") && fs["rmse"].is_number()) putD("sigma_e", fs["rmse"].get<double>());
    putD("g_min", xt->g_min);
    putD("g_avg", xt->g_avg);
    putD("g_max", xt->g_max);
    putS("xtmodel", "fe");
    putS("predict", "xtivreg2_p");
    putS("ivar", xt->ivar);
    putS("tvar", xt->tvar);
  }
  if (parent.vcov == "cluster" && !parent.cluster.empty()) {
    put("N_clust", "n_clusters");
    putS("vce", "robust cluster");
    putS("vcetype", "Robust");
    putS("clustvar", parent.cluster.front());
  } else if (parent.vcov == "hc1") {
    putS("vce", "robust");
    putS("vcetype", "Robust");
  }
  const ojson stored = runEststo(
      name, xt != nullptr ? std::string() : std::string("First-stage regression: ") + endog, session,
      "", false);
  if (stored.is_object() && stored.value("type", std::string()) == "error") return stored;
  return std::nullopt;
}

// Drops `drop` from every per-coefficient map of an iv reply and from its
// coeftable. Used by ivreg2's partial(): by Frisch-Waugh-Lovell the remaining
// coefficients and their standard errors are the full fit's (w2b_estim.log:1013
// ivreg2_partial .1611032742759026 against :1060 ivreg2_ctl .1611032742759016),
// and ivreg2 counts the partialled-out variables in K for every small-sample
// adjustment (its own footnote, :1004-1006), so the full fit's dof are already
// the right ones.
void dropCoefsM4b(ojson& r, const std::set<std::string>& drop) {
  for (const char* key : {"coef", "se", "tstat", "pvalue", "ci_lo", "ci_hi"}) {
    if (!r.contains(key) || !r[key].is_object()) continue;
    for (const auto& name : drop) r[key].erase(name);
  }
  if (r.contains("coeftable") && r["coeftable"].is_array()) {
    ojson kept = ojson::array();
    for (const auto& row : r["coeftable"]) {
      if (row.is_object() && drop.count(row.value("name", std::string()))) continue;
      kept.push_back(row);
    }
    r["coeftable"] = kept;
  }
}

}  // namespace

// Stata's ordered e() name lists after ivregress 2sls, multiway (estimation_m4.cpp).
void postENameListsIvregressM4(SessionState& session, const ojson& raw, size_t kEndog);

// `reportOut` (BUG-1227): ivregress's `first` report -- "First-stage regressions" and
// each stage's header and table (ivregressFirstHeaderM4b) -- as a display block the
// caller pushes ahead of the estimation block, where Stata prints it. The dispatcher's
// ivregress arm passes it (declared there: piet/handlers.h keeps the seven-argument
// form, which keeps every first stage in the block's warnings, as before).
ojson runIvRegress(const std::string& method, const eval::ParsedCommand& parsed, bool allowAbsorb,
                    const std::string& stataCmd, SessionState& session, bool smallDefault,
                    bool forceSmall, ojson* reportOut) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();

  IvParenResult iv = parseIvParenM4(parsed.varlist, /*exogAfterParen=*/true);
  if (iv.errored) {
    return engineErrorBlockM4("syntax_error: `" + stataCmd + "` " + iv.errorMsg, "syntax_error");
  }
  // FU-475: the engine applies --keepif after its time-series operators only
  // for a restriction it can evaluate on the full frame.
  if (auto refused = tsFitRestrictionRefusal(parsed, stataCmd)) return *refused;

  bool isLiml = (method == "liml");
  std::string cobraCmd = isLiml ? "liml" : "iv";

  // `first` (w2b_estim.log:879 ivregress_first, :1064 ivreghdfe_first):
  // each first stage is displayed, the fit itself is unchanged (both arms'
  // _b/_se are the option-free fits' -- :919 against ivreg2_ctl :1060, :1185
  // against ivreghdfe_ctl :1301). The stages come from the
  // engine's `--savefirst` estimate: `regress ENDOG EXCLUDED_IVS EXOG` under the
  // parent's vce, small-sample statistics always (cmd_iv's usage text; the
  // FE-absorbed stage reproduces :1078-1079, z1 .2657743 / .1072108). gmm and
  // liml are refused: their first-stage tables are not measured, and liml's
  // engine command has no --savefirst at all.
  bool wantFirst = false;
  // ivreg2's partial(varlist) (w2b_estim.log:969-1013 ivreg2_partial):
  // Frisch-Waugh-Lovell -- see dropCoefsM4b. ivreg2/xtivreg2 only: ivregress and
  // ivreghdfe keep refusing the key through the net below.
  const bool partialAllowed = (stataCmd == "ivreg2" || stataCmd == "xtivreg2");
  bool wantPartial = false;
  std::vector<std::string> partialVars;
  // ivregress's `small` (t/F small-sample statistics; w2s_multiway_class.log
  // :875/:880 mw_iv_c2s `vce(cluster id id2) small`: e(small) `small`, df_r 6).
  bool wantSmallOpt = false;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "small" && stataCmd == "ivregress" && lowerM4(parsed.cmd) == "ivregress") {
      if (!v.isBool) return stataRefusalM4b("option small() not allowed", 198);
      wantSmallOpt = true;
    }
  }
  if (wantSmallOpt) forceSmall = true;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "first") {
      if (!v.isBool) return stataRefusalM4b("option first() not allowed", 198);
      wantFirst = true;
    } else if (k == "partial" && partialAllowed) {
      if (v.isBool || splitTokensM4(v.strValue).empty()) {
        return stataRefusalM4b("option partial() requires a varlist", 198);
      }
      wantPartial = true;
      for (const auto& t : splitTokensM4(v.strValue)) partialVars.push_back(t);
    }
  }
  // ivreg2's ffirst / savefirst / savefprefix(P) (ivreg2 runs the first-stage
  // block for any of first/ffirst/savefirst; savefprefix implies savefirst).
  // ffirst prints the summary statistics only; savefirst stores each first stage as
  // `estimates store <prefix><endog>` (default prefix _ivreg2_); both post
  // e(first) (:2071-2073). w2d_uncovered.log:1506 ivreg2_ffirst_savefirst_068,
  // :1623 ivreg2_cluster_ffirst_057. xtivreg2 passes first/ffirst/savefprefix() on to
  // ivreg2 (xtivreg2): its default prefix is `_xtivreg2_`, and a
  // bare `first` saves the stages only to drop them again, so it
  // posts e(first) without stored estimates or e(firsteqs).
  const bool ivreg2Typed = stataCmd == "ivreg2" && lowerM4(parsed.cmd) == "ivreg2";
  const bool xtivreg2Typed = stataCmd == "xtivreg2" && lowerM4(parsed.cmd) == "xtivreg2";
  const bool ivreg2Family = ivreg2Typed || xtivreg2Typed;
  bool wantFfirst = false, wantSavefirst = false;
  std::string savefprefix;
  if (ivreg2Family) {
    for (const auto& [k, v] : parsed.opts) {
      if (k == "ffirst" || k == "savefirst") {
        if (!v.isBool) return stataRefusalM4b("option " + k + "() not allowed", 198);
        (k == "ffirst" ? wantFfirst : wantSavefirst) = true;
      } else if (k == "savefprefix") {
        const std::string pre = v.isBool ? std::string() : trimM4(v.strValue);
        if (pre.empty() || pre.find_first_of(" \t") != std::string::npos)
          return stataRefusalM4b("option savefprefix() incorrectly specified", 198);
        savefprefix = pre;
        wantSavefirst = true;
      }
    }
    if (xtivreg2Typed && wantSavefirst && savefprefix.empty()) savefprefix = "_xtivreg2_";
  }
  if (wantFirst && method != "2sls") {
    return engineErrorBlockM4(
        "Option `first` is a valid Stata option that this version of PIET implements for the "
        "2SLS estimator only (`" + stataCmd + "` with " + method + " has no measured "
        "first-stage table).",
        "unsupported_option");
  }
  if (wantPartial) {
    // The partialled-out variables must be included exogenous regressors typed
    // as plain names (`_cons` alone partials the constant only). A factor or
    // operator token has no single coefficient to drop, so it is refused rather
    // than guessed at.
    for (const auto& pv : partialVars) {
      if (pv == "_cons") continue;
      if (pv.find_first_of(".#()") != std::string::npos) {
        return engineErrorBlockM4(
            "partial(" + pv + "): this version of PIET partials out plain exogenous "
            "variables only (no factor-variable or time-series operators).",
            "unsupported_option");
      }
      if (std::find(iv.exog.begin(), iv.exog.end(), pv) == iv.exog.end()) {
        return stataRefusalM4b("partial(): " + pv + " is not an included exogenous regressor",
                               198);
      }
    }
    if (method != "2sls") {
      // Efficient two-step GMM re-weights with the partialled-out regressors in
      // the moment conditions; FWL invariance is not measured there.
      return engineErrorBlockM4(
          "Option `partial()` is a valid Stata option that this version of PIET implements for "
          "the 2SLS estimator only.",
          "unsupported_option");
    }
  }

  std::vector<std::string> argv = {cobraCmd,        "--y",      iv.y,
                                    "--endog",       joinCommaM4(iv.endog),
                                    "--instruments", joinCommaM4(iv.instruments)};
  // cobra iv/liml both REJECT an explicit `--x "(intercept)"` placeholder
  // (live-verified: "column not found: (intercept)") — unlike feols/xtreg,
  // which REQUIRE that literal when there are no exogenous controls. Only
  // push --x when there is a genuine exogenous-controls list.
  if (!iv.exog.empty()) {
    argv.push_back("--x");
    argv.push_back(joinCommaM4(iv.exog));
  }
  // ivreghdfe's cluster() with three or more variables (StataBE 19.5, BUG-1189;
  // scratch-laneA/agents/p-est-a/k3.do): ivreghdfe clusters on the first two only
  // (ivreghdfe `tokenize `cluster''), drops the rows missing any listed
  // variable (markout) and posts e(clustvar) with every one. cluster(id id2
  // id3) is the cluster(id id2) fit -- N 180, N_clust 7 (30 and 7), _se[x1]
  // .15468183635897548, e(vce) "robust two-way cluster", e(clustvar) "id id2 id3" --
  // and with id3 missing in 12 rows it is that fit `if !missing(id3)` (N 168). `pv` is
  // the parse the VCE reader sees: the list cut to its first two names.
  eval::ParsedCommand pv = parsed;
  std::vector<std::string> extraClusterVars;
  std::string allClusterVars;
  if (stataCmd == "ivreghdfe" && lowerM4(parsed.cmd) == "ivreghdfe") {
    for (auto& [k, v] : pv.opts) {
      if (v.isBool) continue;
      std::vector<std::string> toks;
      std::string head;
      if (k == "cluster") {
        toks = splitTokensM4(v.strValue);
      } else if (k == "vce" || k == "vcov") {
        std::string rest;
        if (canonicalVceType(v.strValue, &rest) == "cluster") {
          toks = splitTokensM4(rest);
          head = "cluster ";
        }
      }
      if (toks.size() < 3) continue;
      allClusterVars = joinSpaceM4(toks);
      extraClusterVars.assign(toks.begin() + 2, toks.end());
      v.strValue = head + toks[0] + " " + toks[1];
    }
  }
  std::optional<std::string> keepif = parsed.ifExpr;
  if (!extraClusterVars.empty()) {
    std::string present;
    for (const auto& c : extraClusterVars)
      present += (present.empty() ? "" : " & ") + std::string("!missing(") + c + ")";
    keepif = keepif.has_value() ? "(" + *keepif + ") & " + present : present;
  }
  if (keepif.has_value()) {
    argv.push_back("--keepif");
    argv.push_back(*keepif);
  }

  // --fe via absorb() — ivreghdfe / the reghdfe-paren form ONLY. Real
  // Stata's `ivregress` has no absorb() option of its own at all.
  std::string feRaw;
  bool sawAbsorb = false;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "absorb") {
      sawAbsorb = true;
      if (!v.isBool) feRaw = v.strValue;
    }
  }
  if (sawAbsorb) {
    if (!allowAbsorb) {
      return engineErrorBlockM4("option `absorb` not allowed", "unsupported_option");
    }
    std::vector<std::string> feToks = splitTokensM4(feRaw);
    if (!feToks.empty()) {
      argv.push_back("--fe");
      argv.push_back(joinCommaM4(feToks));
    }
  }

  // vce()/robust/legacy cluster(). cobra `iv` supports iid/hc1/cluster/
  // twoway (BS-067's splitClusterVarsM4 reused unchanged). cobra `liml` has
  // NO working vcov surface at all — live-verified: `--vcov hc1` is
  // silently ACCEPTED (exit 0, no error) but produces the IDENTICAL
  // classical/asymptotic SE either way, i.e. a true engine no-op rather
  // than a rejection. Forwarding a user's vce()/robust/cluster() to liml
  // would silently claim a robust/clustered SE the engine never actually
  // computed — an honest reject before ever building the argv, matching
  // this batch's "never forward a flag the engine demonstrably ignores"
  // rule (see also noconstant/level below, and the report's cobra_runner.cpp
  // masking-bug finding for the OTHER half of that rule: options the engine
  // hard-errors on but whose stdout gets parsed as a false success anyway).
  // Observation weights: [aw=]/[pw=] reach here for ivreg2, xtivreg2, ivregress and
  // ivreghdfe (their registry rows admit them). ivreg2 treats pweight as
  // aweight plus robust, so a bare pweight fits robust, as regress does (BUG-536's twin).
  std::string ivWtype;
  if (parsed.weightType.has_value() && parsed.weightVar.has_value() && !parsed.weightVar->empty()) {
    const std::string wt = lowerM4(*parsed.weightType);
    if (wt == "aw" || wt == "aweight") ivWtype = "aw";
    else if (wt == "pw" || wt == "pweight") ivWtype = "pw";
    // xtivreg2's fe and ivreghdfe are the absorbed fits that admit them: the within
    // transform uses the weighted group means (xtivreg2), then the
    // weighted 2SLS (BUG-1283; ivregress's own absorb() is not measured weighted).
    if (ivWtype.empty() || isLiml || (sawAbsorb && !xtivreg2Typed && stataCmd != "ivreghdfe")) {
      return engineErrorBlockM4(
          "[" + *parsed.weightType + "=] is a valid Stata weight that this version of PIET does not "
          "implement for `" + stataCmd + "`" + (sawAbsorb ? " together with absorbed fixed effects" : "") +
              " (aweight and pweight are implemented for the unabsorbed 2SLS/GMM fit).",
          "unsupported_option");
    }
  }
  // The engine's liml now takes --vcov hc1 and --vcov cluster --cluster C, the
  // robust/cluster sandwich of ivregress liml (score rows (P_Z X)_i u_i,
  // `small` rescale); w2t_boottest_family.log:1060-1075 D2 `ivregress liml ...,
  // vce(cluster id)` x1 se .0647215. Multiway clustering stays refused for liml
  // (Stata's multiway-cluster liml path is not implemented).
  {
    // BUG-1278: ivregress's own parser refuses a cluster variable that is not in
    // the data (r(198) under vce(), r(111) under cluster()), not the engine's
    // "[missing_column]", and vce() with cluster(). Not ivreg's: Stata's ivreg
    // refuses vce() outright and words cluster() as regress does (r(198));
    // ivreghdfe's is not measured. vce(cluster a b) is multiway here.
    if (stataCmd == "ivregress" && lowerM4(parsed.cmd) == "ivregress") {
      if (auto bad = clusterSpecRefusalM4(parsed, session, ClusterOptParserM4::kIvregress,
                                          /*multiwayVceRefused=*/false))
        return *bad;
    }
    VcovResult vr = vcovFromOpts(pv);
    if (vr.errored) {
      return vcovErrorBlockM4(vr);
    }
    if (vr.argv.empty() && ivWtype == "pw") vr.argv = {"--vcov", "hc1"};
    if (isLiml) {
      for (size_t i = 0; i + 1 < vr.argv.size(); ++i) {
        if (vr.argv[i] == "--vcov" && vr.argv[i + 1] != "iid" && vr.argv[i + 1] != "hc1" &&
            vr.argv[i + 1] != "cluster") {
          return engineErrorBlockM4(
              "vce(" + vr.argv[i + 1] + ") is not supported for `ivregress liml` in this version "
              "of PIET -- the engine's liml computes the classical, robust and one-way cluster "
              "VCE only.",
              "unsupported_option");
        }
      }
    }
    for (const auto& a : vr.argv) argv.push_back(a);
  }
  if (!ivWtype.empty()) {
    argv.push_back("--weight");
    argv.push_back(*parsed.weightVar);
    argv.push_back("--wtype");
    argv.push_back(ivWtype);
  }

  // ivreghdfe (including the `reghdfe ... (endog=instr), absorb()` alias)
  // reports the absorbed/cluster small-sample correction by default. cobra
  // `iv` exposes that convention explicitly as --small; plain ivregress must
  // retain its ASYMPTOTIC default. Until 2026-09-09 that distinction rode on
  // `allowAbsorb`, which made "accepts absorb()" and "reports small-sample SEs"
  // the same bit — so BUG-474's one-line flip moved ivregress's standard errors
  // (D1a/D1f selftest RED). `smallDefault` now carries the spelling's own
  // convention; `forceSmall` is `ivreg2 ..., small` arriving from runIvreg2,
  // which used to parse that flag and throw it away. Either reason is
  // sufficient and the flag is idempotent, so this is an OR.
  if (smallDefault || forceSmall) argv.push_back("--small");
  // ivregress ..., small under a multiway cluster: the built-in convention's
  // per-combination factors (engine FU-387;
  // w2s_multiway_class.log:880 mw_iv_c2s_se .0585934519606149).
  if (wantSmallOpt && method != "gmm" && !sawAbsorb) {
    for (size_t i = 0; i + 1 < argv.size(); ++i)
      if (argv[i] == "--vcov" && (argv[i + 1] == "twoway" || argv[i + 1] == "multiway")) {
        argv.push_back("--multiway-subset-factors");
        break;
      }
  }
  bool ivMultiway = false;
  for (size_t i = 0; i + 1 < argv.size(); ++i)
    if (argv[i] == "--vcov" && (argv[i + 1] == "twoway" || argv[i + 1] == "multiway")) ivMultiway = true;
  if ((wantFfirst || wantSavefirst) && ivMultiway) {
    return engineErrorBlockM4(
        "Options ffirst and savefirst are valid ivreg2 options that this version of PIET does not "
        "implement together with a multiway cluster() (ivreg2's two-way first-stage statistics are "
        "not computed).",
        "unsupported_option");
  }
  if (wantFirst || wantFfirst || wantSavefirst) argv.push_back("--savefirst");
  if (wantSavefirst && !savefprefix.empty()) {
    argv.push_back("--savefprefix");
    argv.push_back(savefprefix);
  }
  // ivreg2's two-way cluster variance is S1 + S2 - S12 left as it is (no repair of a
  // negative eigenvalue), so a coefficient whose variance comes out negative has a
  // missing standard error (w2d_uncovered.log:1866 ivreg2_cluster2_aw_030 `|.|`).
  if (ivreg2Typed && ivMultiway) argv.push_back("--multiway-no-psd-fix");
  // xtivreg2's ivreg2 call: dofminus(N_g) on the within-transformed data
  // (xtivreg2) -- a large-sample adjustment (rss/(N-N_g), robust meat
  // N/(N-N_g), cluster untouched), never a small-sample one (cobra iv --dofminus-fe).
  // Two-way clustering is not implemented there and is refused here.
  if (xtivreg2Typed && sawAbsorb) {
    if (ivMultiway || method == "gmm")
      return engineErrorBlockM4(
          "xtivreg2 with " + std::string(ivMultiway ? "a two-way cluster()" : "gmm2s") +
              " is a valid Stata call that this version of PIET does not implement.",
          "unsupported_option");
    argv.push_back("--dofminus-fe");
  }

  // gmm sub-estimator (Stata `ivregress gmm`): efficient two-step GMM,
  // robust weight matrix + robust SEs by default; combines with --vcov
  // cluster for the cluster-robust GMM weight matrix (cobra iv's own doc).
  if (method == "gmm") argv.push_back("--gmm");

  // keepsingletons (BS-068) — reghdfe's own option name. cobra `iv` DOES
  // expose --keep-singletons (live-verified, despite it not being printed
  // in `cobra iv`'s own usage banner); cobra `liml` has no FE machinery at
  // all (absorb() is already rejected above for it via !allowAbsorb, or
  // simply structurally unreachable for plain ivregress), so
  // keepsingletons is meaningless there — honest reject instead of a
  // silent no-op.
  bool sawKeepSingletons = false;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "keepsingletons" && v.isBool && v.boolValue) sawKeepSingletons = true;
  }
  if (sawKeepSingletons) {
    if (isLiml) {
      return engineErrorBlockM4("option `keepsingletons` not allowed", "unsupported_option");
    }
    argv.push_back("--keep-singletons");
  }

  // 2SLS/GMM now supports --no-intercept. LIML and absorb() combinations
  // stay refused below, as does level(): those engine surfaces are absent.
  bool ivNoConstant = false;
  for (const auto& [k, v] : parsed.opts) {
    if ((k == "noconstant" || k == "nocons") && v.isBool && v.boolValue) {
      // The engine's 2SLS/GMM fit takes --no-intercept (w2d_uncovered.log
      // ivregress_nocons_ctl b[x1] 2.8397 / ivregress_nocons_vce_cluster_106);
      // LIML has no such surface and absorb() already removes the constant.
      if (isLiml || sawAbsorb) {
        return engineErrorBlockM4(
            "Option `" + k + "` is a valid Stata option that this version of PIET does not yet "
            "implement for `" + stataCmd + "` " +
                (isLiml ? std::string("with liml.") : std::string("together with absorb().")),
            "unsupported_option");
      }
      ivNoConstant = true;
      continue;
    }
    if (k == "level" && !v.isBool) {
      return engineErrorBlockM4(
          "Option `level` is a valid Stata option that this version of PIET does not yet "
          // BUG-286: this used to say the engine "ignores --level", which is a
          // description of a WORSE behaviour than the real one and would send a
          // reader looking for a silent drop. Re-measured 2026-08-08:
          // `cobra iv ... --level 90` answers `unrecognized option '--level' for
          // 'iv'` (bad_input) with an empty result — it has no --level surface at
          // all. The reject PIET gives here is therefore the honest mirror of a
          // missing engine flag, not a guard against a silent drop.
          "implement for `" + stataCmd + "` (the engine's IV/LIML estimator has no --level "
          "surface — it answers `unrecognized option '--level' for 'iv'`).",
          "unsupported_option");
    }
  }
  if (ivNoConstant) argv.push_back("--no-intercept");

  // kVocabIvFirst is the same registry slot as kVocabIvRegress (see their
  // registrations), so `first` is in this set; partial() joins it for ivreg2.
  std::unordered_set<std::string> known = kVocabIvFirst.known;
  if (partialAllowed) known.insert("partial");
  if (wantSmallOpt) known.insert("small");
  if (ivreg2Family) {
    known.insert("ffirst");
    known.insert("savefirst");
    known.insert("savefprefix");
  }
  if (auto err = eval::rejectUnknownOptions(parsed.opts, known)) {
    return *err;
  }

  // BUG-1189: reghdfe counts an absorbed effect redundant when it is nested in ANY
  // listed cluster variable (k3.do: absorb(id) cluster(id2 id3 id) gives _se[x1]
  // .17410222432235206 because id nests in the third), which the engine, fitting on
  // the first two, cannot see. Such a fit is refused: an effect nested in a listed
  // variable past the second (checked on every row where both are present, a
  // superset of the sample), or an absorb term that is not a plain numeric variable.
  if (!extraClusterVars.empty()) {
    auto refuseNested = [&](const std::string& why) {
      return engineErrorBlockM4(
          "ivreghdfe with three or more cluster variables (" + allClusterVars + ") is served "
          "when no absorbed effect is nested in a cluster variable past the second; " + why +
              ", which this version of PIET does not implement.",
          "unsupported_option");
    };
    Datastore* ds = session.datastore();
    if (ds == nullptr) return refuseNested("the data could not be read");
    for (const auto& f : splitTokensM4(feRaw)) {
      if (!ds->has(f) || ds->dtype(f) != "f64")
        return refuseNested("absorb term `" + f + "` is not a numeric variable");
      for (const auto& c : extraClusterVars) {
        if (!ds->has(c) || ds->dtype(c) != "f64")
          return refuseNested("cluster variable `" + c + "` is not numeric");
        const double* fv = ds->col(f).f64();
        const double* cv = ds->col(c).f64();
        std::map<double, double> seen;
        bool nested = true;
        for (int64_t i = 0; i < ds->nRows() && nested; ++i) {
          if (std::isnan(fv[i]) || std::isnan(cv[i])) continue;
          const auto [it, fresh] = seen.emplace(fv[i], cv[i]);
          if (!fresh && it->second != cv[i]) nested = false;
        }
        if (nested && !seen.empty())
          return refuseNested("`" + f + "` is nested in `" + c + "`");
      }
    }
  }

  argv.push_back("--data");
  argv.push_back(*session.currentHbin());
  argv.push_back("--tidy");
  // e(sample) T2: `iv` writes the fit's admitted rows over the --data rows (its
  // own --keepif, FE singleton drops and listwise deletion included); installed
  // below, before setLastModel. `liml` has no such flag, so its e(sample) stays
  // the named r(198) (LastModel::EsampleState::kUnavailable).
  const std::vector<std::string> fitArgv = argv;  // captureFitFilterM4 reads the fit's own flags
  const std::string esampleValues =
      isLiml ? std::string() : esampleFitArgs(session, argv, std::string(), 0);
  // eb-order T-P1: the fit's full e(V) (`iv` and, since BUG-1196, `liml` carry
  // --emit-v). Not recorded in fitArgv.
  argv.push_back("--emit-v");

  ojson raw;
  try {
    raw = runCobraJsonM4(argv);
  } catch (const CobraError& ce) {
    if (!esampleValues.empty()) cecon::native_unlink(esampleValues);
    // xtivreg2 (exit 198).
    if (ce.code == "cluster_spans_panel")
      return stataRefusalM4b("cluster option not supported if a panel spans more than one cluster",
                             198);
    // BUG-1269: ivreghdfe with absorb(), like reghdfe (handlers.cpp runRegress),
    // calls an empty sample insufficient, r(2001), and so a one-row one, whose
    // single row the singleton drop removes (StataBE 19.5: `ivreghdfe y (x = w) if
    // x > 20, absorb(g)` and `if x == 1`); without absorb() it is r(2000).
    if (stataCmd == "ivreghdfe" && sawAbsorb && ce.code == "no_obs" &&
        std::string(ce.what()) == "no observations")
      return ojson(eval::errorBlock("engine_error", "insufficient observations", std::nullopt,
                                    std::nullopt, std::nullopt, std::string("r(2001)")));
    return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
  }

  // partial(): drop the partialled-out regressors and the constant from the
  // reported fit (dropCoefsM4b). The model Wald statistic then covers the
  // remaining slopes: with one left it is that coefficient's own z^2 (ivreg2
  // reports F(1,57) 137.81 = 1.94039^2/.1611033^2 * 57/60, :978); with more the
  // joint test needs the covariance the reply does not carry, so it is not
  // reported rather than left describing the full model.
  std::set<std::string> partialled;
  if (wantPartial) {
    for (const auto& pv : partialVars) {
      if (pv == "_cons") continue;
      if (!raw.contains("coef") || !raw["coef"].is_object() || !raw["coef"].contains(pv)) {
        return engineErrorBlockM4("partial(): the fit has no coefficient for `" + pv + "`",
                                  "engine_error");
      }
      partialled.insert(pv);
    }
    partialled.insert("(intercept)");
    dropCoefsM4b(raw, partialled);
    raw.erase("chi2");
    raw.erase("chi2_df");
    if (raw.contains("coeftable") && raw["coeftable"].is_array() &&
        raw["coeftable"].size() == 1 && raw["coeftable"][0].contains("t") &&
        raw["coeftable"][0]["t"].is_number()) {
      const double t = raw["coeftable"][0]["t"].get<double>();
      raw["chi2"] = t * t;
      raw["chi2_df"] = 1;
    }
  }

  // first: one display text per endogenous regressor, built from the engine's
  // stage, then the stage is taken OUT of the cached reply -- `--savefirst`'s
  // first_stage/firsteqs are ivreg2's stored-estimate surface, which `first`
  // does not create (e(firsteqs) stays unset, as after a plain fit).
  ojson firstStages = ojson::array();
  std::vector<std::string> firstTexts;
  // The engine's stages as sent, before `first` below takes them out of the reply:
  // ffirst/savefirst/first all read e(first) from them, savefirst the stored estimates.
  const bool wantFirstMatrix = ivreg2Family && (wantFirst || wantFfirst || wantSavefirst);
  ojson savedFirst;
  if (wantFirstMatrix) {
    if (!raw.contains("first_stage") || !raw["first_stage"].is_object()) {
      return engineErrorBlockM4("ffirst/savefirst: the engine returned no first-stage estimates",
                                "engine_error");
    }
    savedFirst = raw["first_stage"];
    if (wantSavefirst && wantPartial) {
      return engineErrorBlockM4(
          "Option savefirst is a valid ivreg2 option that this version of PIET does not implement "
          "together with partial() (the stored first stages would have to be the partialled ones).",
          "unsupported_option");
    }
  }
  // BUG-1227: ivregress's own report takes its first stages; it is built below, once
  // e(sample) is installed (the R-squareds need the stage depvar's TSS over it).
  const bool firstReport = wantFirst && stataCmd == "ivregress" && reportOut != nullptr;
  std::string firstVcov = "iid", firstClustvar;
  if (wantFirst) {
    if (!raw.contains("first_stage") || !raw["first_stage"].is_object()) {
      return engineErrorBlockM4("first: the engine returned no first-stage estimates",
                                "engine_error");
    }
    std::string clustvar;
    for (size_t i = 0; i + 1 < argv.size(); ++i) {
      if (argv[i] == "--cluster") clustvar = argv[i + 1];
    }
    std::string vcovArg = "iid";
    for (size_t i = 0; i + 1 < argv.size(); ++i) {
      if (argv[i] == "--vcov") vcovArg = argv[i + 1];
    }
    firstVcov = vcovArg;
    firstClustvar = clustvar;
    const std::set<std::string> instrSet(iv.instruments.begin(), iv.instruments.end());
    for (const auto& endogName : iv.endog) {
      if (!raw["first_stage"].contains(endogName)) continue;
      ojson fs = raw["first_stage"][endogName];
      if (!partialled.empty()) dropCoefsM4b(fs, partialled);
      std::vector<std::string> names;
      for (const auto& n : fs.value("names", ojson::array())) {
        if (n.is_string() && !partialled.count(n.get<std::string>())) {
          names.push_back(n.get<std::string>());
        }
      }
      if (stataCmd == "ivregress") {
        // ivregress lists the stage as exog, excluded instruments, _cons
        // (:894-896 x2 z1 _cons; w2d_uncovered.log:2070-2074 w1 w2 z1 z2 _cons);
        // the engine's order is ivreg2's (excluded instruments first), which is
        // what ivreghdfe prints (:1078-1079 z1 x2).
        std::vector<std::string> ordered;
        for (const auto& n : names)
          if (!instrSet.count(n) && n != "(intercept)") ordered.push_back(n);
        for (const auto& n : names)
          if (instrSet.count(n)) ordered.push_back(n);
        for (const auto& n : names)
          if (n == "(intercept)") ordered.push_back(n);
        names = ordered;
      }
      fs["names"] = names;
      if (!firstReport) firstTexts.push_back(firstStageTextM4b(fs, names, vcovArg, clustvar));
      firstStages.push_back(fs);
    }
    raw.erase("first_stage");
    raw.erase("firsteqs");
  }
  if (wantFirstMatrix) {
    raw.erase("first_stage");
    raw.erase("firsteqs");
    if (wantFfirst && !wantFirst) firstTexts.push_back(ffirstSummaryTextM4b(savedFirst, iv.endog));
  }

  ojson block = cobraToEstimationM4(raw, cobraCmd, &session);
  // The first stages print ABOVE the IV table, which is where the estimation
  // block's `warnings` render (console.js renderEstimation); the structured
  // stages ride along under `first_stage` for any consumer that reads numbers.
  if (wantFirst || wantPartial || !firstTexts.empty()) {
    ojson notes = block.contains("warnings") && block["warnings"].is_array() ? block["warnings"]
                                                                             : ojson::array();
    for (const auto& t : firstTexts) notes.push_back(t);
    if (wantPartial) {
      std::string list;
      for (const auto& pv : partialVars)
        if (pv != "_cons") list += pv + " ";
      notes.push_back("Partialled-out: " + list + "_cons (total SS, model F/chi2 and R2s are after "
                      "partialling-out; small-sample adjustments count the partialled-out "
                      "variables in K)");
    }
    if (!notes.empty()) block["warnings"] = notes;
    if (wantFirst) block["first_stage"] = firstStages;
  }
  // Estimator label (task requirement): "ivregress 2sls"/"ivregress gmm"/
  // "ivregress liml"/"ivreghdfe" — independent of cobra's own raw
  // "estimator" field ("iv2sls"/"liml") and independent of whether the
  // ivreghdfe form was typed as `ivreghdfe` or as the `reghdfe (...)` paren
  // form (both compute the identical cobra iv --fe call).
  block["estimator"] = (stataCmd == "ivregress") ? ("ivregress " + method) : std::string("ivreghdfe");

  LastModel lm;
  lm.cobraCmd = cobraCmd;  // "iv" or "liml" — margins' kLinearCmds already
                           // anticipates "iv" (pre-existing) and now "liml"
                           // (this batch's addition, see runMargins above).
  // FU-748: e(cmd) is the command typed -- `ivreghdfe` after ivreghdfe
  // (w2t_boottest_family.log:1197 D3, :1282 D4), `ivregress` after ivregress
  // (:1047 D1, :1411 D6) -- where it read the engine's `iv`. Set only when the
  // typed word is the Stata command this path answers for (ivregress, ivreghdfe,
  // ivreg2, xtivreg2): `ivreg` and reghdfe's IV paren form reach here under
  // another name and keep the engine's spelling, as before.
  if (lowerM4(parsed.cmd) == stataCmd) lm.stataCmd = stataCmd;
  lm.y = iv.y;
  // Stata's own ivregress coefficient table lists ENDOGENOUS regressors
  // first, then exogenous (oracle-verified: D1b's table shows `beertax`
  // before `emppop` even though `emppop` was typed BEFORE the paren block)
  // — matches cobra iv's own coef-dict insertion order too. test/lincom/
  // margins/predict after an IV fit are a KNOWN coarse approximation
  // (cobra's own test/lincom/predict CLIs always re-fit as plain OLS from
  // --y/--x — they have no concept of endog/instruments/gmm at all; this is
  // a pre-existing, disclosed limitation shared by every non-feols
  // estimator already wired into LastModel, not something introduced here).
  lm.x = iv.endog;
  for (const auto& e : iv.exog) lm.x.push_back(e);
  // BETA Phase G (estat endogenous/overid wiring): preserve the role split
  // `lm.x` above discards — see LastModel's own comment (session_state.h).
  lm.ivEndog = iv.endog;
  lm.ivExog = iv.exog;
  lm.ivInstruments = iv.instruments;
  if (sawAbsorb && allowAbsorb) {
    for (const auto& f : splitTokensM4(feRaw)) lm.fe.push_back(f);
  }
  // liml included: its --vcov/--cluster are served now (cmd_liml, ivregress liml).
  for (size_t i = 0; i < argv.size(); ++i) {
    if (argv[i] == "--vcov" && i + 1 < argv.size()) lm.vcov = argv[i + 1];
    if (argv[i] == "--cluster" && i + 1 < argv.size()) {
      for (const auto& c : splitTokensM4(argv[i + 1])) lm.cluster.push_back(c);
    }
  }
  lm.depvar = block.value("depvar", "");
  // ivreghdfe's e(vce) is ivreg2's spelling (ivreghdfe.ado posts ivreg2's
  // `vce`): `robust cluster` for one cluster variable, `robust two-way cluster`
  // for two, `robust` under vce(robust), and none for the default
  // (w2l_vce_shape_det.log ivreghdfe_ctl_cluster `|robust cluster|Robust|30|`,
  // :3340 ivreghdfe_vce_c2 `|robust two-way cluster|Robust|7|`, :3508
  // ivreghdfe_empty_cluster `0|ivreghdfe|||.|180`).
  if (lm.stataCmd == "ivreghdfe") {
    if (lm.vcov == "cluster") lm.eVce = "robust cluster";
    else if (lm.vcov == "twoway") lm.eVce = "robust two-way cluster";
    else if (lm.vcov == "iid") lm.eVce = "";
  }
  captureFitFilterM4(lm, fitArgv);
  lm.dataPath = *session.currentHbin();
  (void)esampleInstallOnHead(session, esampleValues, raw, lm);  // e(sample) T2
  lm.result = raw;
  // ivregress: e(small) is the macro `small` under the option and empty
  // otherwise (never the engine's 0/1), and a multiway cluster posts no
  // e(N_clust) (w2s_multiway_class.log:811 mw_iv_c2_names `|.|.|3|180||`, :875
  // mw_iv_c2s_names `|.|6|3|180|small|`).
  const bool ivregressTyped = lm.stataCmd == "ivregress";
  // xtivreg2: e(small) is ivreg2's macro (posted below), and e(df_r) exists only
  // under `small`: N-K-N_g, or N_clust-1 under cluster (ivreg2;
  // probe fe_iid_small 28, fe_cl_small 9).
  if (xtivreg2Typed && sawAbsorb && lm.result->is_object()) {
    lm.result->erase("small");
    if (smallDefault || forceSmall) {
      ojson& rr = *lm.result;
      if (lm.vcov == "cluster" && rr.contains("n_clusters") && rr["n_clusters"].is_number())
        rr["df_r"] = rr["n_clusters"].get<double>() - 1.0;
      else if (rr.contains("dof_resid") && rr["dof_resid"].is_number())
        rr["df_r"] = rr["dof_resid"];
    }
  }
  if (ivregressTyped && lm.result->is_object()) {
    lm.result->erase("small");
    if (lm.vcov == "twoway" || lm.vcov == "multiway") {
      // `small`: e(df_r) is the smallest cluster count less one (mw_iv_c2s
      // df_r 6 with G = 30 and 7).
      if (wantSmallOpt && !lm.result->contains("df_r") && lm.result->contains("n_clusters") &&
          (*lm.result)["n_clusters"].is_number())
        (*lm.result)["df_r"] = (*lm.result)["n_clusters"].get<double>() - 1.0;
      lm.result->erase("n_clusters");
    }
  }
  // ivreghdfe's e(df_a) and e(df_r) (StataBE 19.5, BUG-1224: absorb(id) -> df_a 30, df_r
  // 148 = 180 - 2 - 30; absorb(id) cluster(id) -> df_a 0, df_r 29). df_a is reghdfe's
  // count of the absorbed coefficients (ivreghdfe HDFE.df_a), absorbedDofA's rule.
  // absorb() implies small (ivreghdfe), so e(df_r) is N - K - df_a, the
  // engine's dof_resid, or N_clust - 1 under cluster, N_clust being the smaller count of a
  // two-way cluster (ivreghdfe; the reply's n_clusters is that smallest).
  // Cached reply only (key == name fall-through), as after reghdfe.
  if (lm.stataCmd == "ivreghdfe" && sawAbsorb && lm.result->is_object()) {
    ojson& rr = *lm.result;
    if (const auto dfA = absorbedDofA(rr, "reghdfe")) rr["df_a"] = *dfA;
    const bool clustered = lm.vcov == "cluster" || lm.vcov == "twoway" || lm.vcov == "multiway";
    if (clustered && rr.contains("n_clusters") && rr["n_clusters"].is_number() &&
        rr["n_clusters"].get<double>() > 0)
      rr["df_r"] = rr["n_clusters"].get<double>() - 1.0;
    else if (!clustered && rr.contains("dof_resid") && rr["dof_resid"].is_number())
      rr["df_r"] = rr["dof_resid"];
  }
  // ivregress's absorb() (Stata 19): e(k_absorb) the absorbed categories, e(df_a) one
  // fewer per absorbed variable, with no redundancy between them (StataBE 19.5, BUG-1501,
  // 180 rows: absorb(id) 30 / 29, absorb(id id2) 37 / 35, absorb(id id2 id3) 42 / 39, and
  // absorb(id id4) 33 / 31 with id4 nested in id; the same under vce(robust),
  // vce(cluster id) and [aw=]). e(absvar) is posted with the model below. Cached reply
  // only, like ivreghdfe's e(df_a) above.
  const bool ivregressAbsorb = ivregressTyped && sawAbsorb && !lm.fe.empty() &&
                               lm.result->is_object() && lm.result->contains("absorbed") &&
                               (*lm.result)["absorbed"].is_object() &&
                               (*lm.result)["absorbed"].size() == lm.fe.size();
  if (ivregressAbsorb) {
    ojson& rr = *lm.result;
    double kAbsorb = 0.0;
    bool ok = true;
    for (const auto& f : lm.fe) {
      ok = ok && rr["absorbed"].contains(f) && rr["absorbed"][f].is_number();
      if (ok) kAbsorb += rr["absorbed"][f].get<double>();
    }
    if (ok) {
      rr["k_absorb"] = kAbsorb;
      rr["df_a"] = kAbsorb - static_cast<double>(lm.fe.size());
    }
  }
  // savefirst: the first stages are stored BEFORE the parent posts (see
  // storeIvreg2FirstStageM4b); e(firsteqs) lists the names that were stored.
  std::string firstEqs;
  // xtivreg2's panel scalars (xtivreg2): N_g the groups in the
  // estimation sample, g_min/g_avg/g_max their row counts (unweighted for aw/pw).
  std::optional<XtivPanelM4b> xtPanel;
  if (xtivreg2Typed && sawAbsorb && session.panelvar.has_value() &&
      lm.esample == LastModel::EsampleState::kColumn) {
    try {
      Datastore* ds = session.datastore();
      const std::string& pv = *session.panelvar;
      if (ds != nullptr && ds->has(pv) && ds->dtype(pv) == "f64" && ds->has(kEsampleColumn)) {
        const double* idv = ds->col(pv).f64();
        const double* mk = ds->col(kEsampleColumn).f64();
        const double serial = static_cast<double>(lm.esampleSerial);
        std::map<double, int64_t> sizes;
        int64_t nIn = 0;
        for (int64_t i = 0; i < ds->nRows(); ++i)
          if (mk[i] == serial) {
            ++sizes[idv[i]];
            ++nIn;
          }
        if (!sizes.empty()) {
          XtivPanelM4b xp;
          xp.N_g = static_cast<double>(sizes.size());
          int64_t mn = sizes.begin()->second, mx = mn;
          for (const auto& [k, c] : sizes) {
            (void)k;
            mn = std::min(mn, c);
            mx = std::max(mx, c);
          }
          xp.g_min = static_cast<double>(mn);
          xp.g_max = static_cast<double>(mx);
          xp.g_avg = static_cast<double>(nIn) / xp.N_g;
          xp.clustered = lm.vcov == "cluster";
          xp.ivar = pv;
          xp.tvar = session.timevar.value_or(std::string());
          xtPanel = xp;
        }
      }
    } catch (const std::exception&) {
      // an unreadable frame leaves the panel scalars unposted (missing)
    }
  }
  if (wantSavefirst) {
    for (const auto& endogName : iv.endog) {
      if (!savedFirst.contains(endogName)) continue;
      const ojson& fs = savedFirst[endogName];
      // ivreg2 `capture est store`: a name Stata cannot store is skipped.
      if (!fs.value("stored", false)) continue;
      // ... and so is one whose store fails here (the parent posts regardless).
      if (storeIvreg2FirstStageM4b(fs, lm, session, xtPanel ? &*xtPanel : nullptr).has_value())
        continue;
      firstEqs += (firstEqs.empty() ? "" : " ") + fs.value("eqname", std::string());
    }
  }
  session.setLastModel(lm);
  if (ivregressTyped && wantSmallOpt) {
    session.eval.setEString("small", "small");
    session.noteEOnFit("small");
  }
  // BUG-1501: e(absvar), the absorbed variables in absorb() order (StataBE 19.5 "id id2",
  // "id4 id"; e(absvars) stays empty).
  if (ivregressAbsorb) {
    std::string absvar;
    for (const auto& f : lm.fe) absvar += (absvar.empty() ? "" : " ") + f;
    session.eval.setEString("absvar", absvar);
    session.noteEOnFit("absvar");
  }
  // BUG-1189: e(clustvar) names every listed cluster variable (k3.do `id id2 id3`);
  // the fit itself, and so lm.cluster, has the first two.
  if (!allClusterVars.empty()) {
    session.eval.setEString("clustvar", allClusterVars);
    session.noteEOnFit("clustvar");
  }
  // FU-28 (BUG-759): e(vce) after a default-VCE `ivregress 2sls` is
  // `unadjusted` (w2d_uncovered.log:2187 ivregress_first_2endog_ctl, :2323
  // ivregress_factors_ctl, :2403 ivregress_nocons_ctl, :2536
  // ivregress_exog_before_ctl); setLastModel() published lm.vcov's engine
  // spelling `iid`, which the replay machinery keys on, so only the macro store
  // is corrected (regress `ols` and xtreg ,fe `conventional` do the same).
  // robust -> `robust` and cluster -> `cluster` are already Stata's, with
  // e(vcetype) `Robust`; `unadjusted` keeps the empty e(vcetype) setLastModel
  // gave iid. gmm and liml defaults have no captured arm and are left alone.
  if (stataCmd == "ivregress" && method == "2sls" && lm.vcov == "iid") {
    session.eval.setEString("vce", "unadjusted");
  }
  session.lastEstBlock = block;
  // eb-order T-P1: e(V) is the fit's own covariance. Not after partial(): the
  // reported fit is not the one the engine's e_V describes, so e(V) stays the
  // named refusal there rather than a sub-block of another model's matrix.
  if (!wantPartial) session.attachFitCovariance(raw);
  // ivregress-escalars (w2j_e3_acreg.log:2148 I05_ivregress_ctl rss/tss/r2):
  // ivregress 2sls posts e(mss), the hidden e(tss), e(rss), e(r2),
  // e(r2_a) and e(df_m). The engine sends rmse = sqrt(RSS/N) (iv2sls.cpp, the
  // asymptotic scale whatever `small` says), so RSS = N*rmse^2; TSS is the
  // centred sum of squares of the depvar over e(sample). Only the plain fit
  // (no absorb, no weights, with the constant) is published.
  if (stataCmd == "ivregress" && !ivNoConstant && !sawAbsorb && !parsed.weightType.has_value() &&
      lm.esample == LastModel::EsampleState::kColumn && raw.contains("rmse") &&
      raw["rmse"].is_number() && raw.contains("n_obs") && raw["n_obs"].is_number()) {
    try {
      const double N = raw["n_obs"].get<double>();
      // BUG-1088: the engine sends TSS. Summing it here from the working frame
      // wrote the whole frame to disk to read two columns; that stays only for
      // an engine that does not send it.
      std::optional<double> tssOpt;
      if (raw.contains("tss_centred") && raw["tss_centred"].is_number()) {
        if (N > 0) tssOpt = raw["tss_centred"].get<double>();
      } else if (Datastore* ds = session.datastore();
                 ds != nullptr && ds->has(lm.depvar) && ds->dtype(lm.depvar) == "f64" &&
                 ds->has(kEsampleColumn)) {
        const double* yv = ds->col(lm.depvar).f64();
        const double* mk = ds->col(kEsampleColumn).f64();
        const double serial = static_cast<double>(lm.esampleSerial);
        double n = 0, sum = 0;
        for (int64_t i = 0; i < ds->nRows(); ++i)
          if (mk[i] == serial && !std::isnan(yv[i])) {
            ++n;
            sum += yv[i];
          }
        if (n == N && n > 0) {
          const double ybar = sum / n;
          double tss = 0;
          for (int64_t i = 0; i < ds->nRows(); ++i)
            if (mk[i] == serial && !std::isnan(yv[i])) tss += (yv[i] - ybar) * (yv[i] - ybar);
          tssOpt = tss;
        }
      }
      if (tssOpt.has_value()) {
        const double tss = *tssOpt;
        const double rmse = raw["rmse"].get<double>();
        const double rss = rmse * rmse * N;
        const double K = block.contains("coeftable") && block["coeftable"].is_array()
                             ? static_cast<double>(block["coeftable"].size())
                             : 0.0;
        const double r2 = 1 - rss / tss;
        auto put = [&session](const std::string& nm, double v) {
          session.eval.setEScalar(nm, v);
          session.noteEOnFit(nm);
        };
        put("rss", rss);
        put("tss", tss);
        put("mss", tss - rss);
        put("r2", r2);
        if (K > 0 && N > K) put("r2_a", 1 - (1 - r2) * (N - 1) / (N - K));
        if (K > 0) put("df_m", K - 1);
      }
    } catch (const std::exception&) {
      // a frame that cannot be read leaves these e() scalars unposted (missing)
    }
  }
  // BUG-1227: ivregress's `first` report (the "First-stage
  // regressions" title and its rule, then per endogenous regressor a blank line, the
  // stage's header, a blank line and its table). Each stage's TSS is its depvar's
  // sum of squares over e(sample) (centred unless the stage has no constant), read as
  // e(tss) is above, and only when the mask holds the stage's own row count.
  if (firstReport) {
    auto sumSq = [&](const std::string& var, double nExpect, bool centred) -> double {
      try {
        Datastore* ds = session.datastore();
        if (ds == nullptr || lm.esample != LastModel::EsampleState::kColumn ||
            parsed.weightType.has_value() || !ds->has(var) || ds->dtype(var) != "f64" ||
            !ds->has(kEsampleColumn))
          return std::nan("");
        const double* v = ds->col(var).f64();
        const double* mk = ds->col(kEsampleColumn).f64();
        const double serial = static_cast<double>(lm.esampleSerial);
        double n = 0, sum = 0;
        for (int64_t i = 0; i < ds->nRows(); ++i)
          if (mk[i] == serial && !std::isnan(v[i])) {
            ++n;
            sum += v[i];
          }
        if (n == 0 || n != nExpect) return std::nan("");
        const double mean = centred ? sum / n : 0.0;
        double tss = 0;
        for (int64_t i = 0; i < ds->nRows(); ++i)
          if (mk[i] == serial && !std::isnan(v[i])) tss += (v[i] - mean) * (v[i] - mean);
        return tss;
      } catch (const std::exception&) {
        return std::nan("");
      }
    };
    std::string text = "\nFirst-stage regressions\n-----------------------\n";
    for (const auto& fs : firstStages) {
      std::vector<std::string> names;
      for (const auto& n : fs.value("names", ojson::array()))
        if (n.is_string()) names.push_back(n.get<std::string>());
      const double nObs =
          fs.contains("n_obs") && fs["n_obs"].is_number() ? fs["n_obs"].get<double>() : -1.0;
      const bool hasCons = std::find(names.begin(), names.end(), "(intercept)") != names.end();
      const double tss = sumSq(fs.value("depvar", std::string()), nObs, hasCons);
      text += "\n" + ivregressFirstHeaderM4b(fs, tss, hasCons) + "\n" +
              firstStageTableM4b(fs, names, firstVcov, firstClustvar) + "\n";
    }
    ojson rep;
    rep["type"] = "display";
    rep["text"] = text;
    *reportOut = rep;
  }
  // ivreg2's identification statistics (ivreg2):
  // e(cdf) the Cragg-Donald F (one endogenous regressor: the first-stage F of the
  // excluded instruments; several: the minimum-eigenvalue statistic), e(rkf) the
  // Kleibergen-Paap rk Wald F under robust/cluster, and e(widstat) = rkf under
  // robust/cluster/kernel, else cdf (w2d_uncovered.log:1687 ivreg2_ffirst_ctl
  // `|128.5633117654573|` under cluster(g)). A statistic the engine did not compute
  // (a multiway cluster's rkf, say) stays unposted, i.e. missing.
  if (ivreg2Family && raw.is_object()) {
    auto put = [&session](const std::string& nm, double v) {
      if (!std::isfinite(v)) return;
      session.eval.setEScalar(nm, v);
      session.noteEOnFit(nm);
    };
    auto num = [&raw](const char* key) {
      return raw.contains(key) && raw[key].is_number() ? raw[key].get<double>() : std::nan("");
    };
    // xtivreg2's own e() over ivreg2's (xtivreg2): N_g, df_a = N_g,
    // df_b = ivreg2's df_m (K), df_m = N_g + K, or K under cluster ("FEs with cluster
    // don't use up degrees of freedom"), sigma_e = rmse, singleton, xtmodel, ivar,
    // tvar; ivreg2 posts dofminus(N_g) and sdofminus 0. StataBE probe
    // (impl_F2/stata_probe/probe.log fe_iid df_m 12, fe_cl_ffirst df_m 2).
    if (xtivreg2Typed && xtPanel) {
      const double K = block.contains("coeftable") && block["coeftable"].is_array()
                           ? static_cast<double>(block["coeftable"].size())
                           : std::nan("");
      put("N_g", xtPanel->N_g);
      put("df_a", xtPanel->N_g);
      put("df_b", K);
      put("df_m", xtPanel->clustered ? K : xtPanel->N_g + K);
      put("g_min", xtPanel->g_min);
      put("g_avg", xtPanel->g_avg);
      put("g_max", xtPanel->g_max);
      put("sigma_e", num("rmse"));
      put("rmse", num("rmse"));
      put("dofminus", xtPanel->N_g);
      put("sdofminus", 0);
      put("singleton", num("n_dropped_singletons"));
      // ivreg2's e(small) is `small` or empty and its e(vce) `robust cluster` /
      // `robust` / empty (probe R2 rows), not the engine's 0/1 and iid/hc1/cluster.
      const std::string vceMacro =
          lm.vcov == "cluster" ? "robust cluster" : (lm.vcov == "hc1" ? "robust" : "");
      const bool smallOn = raw.contains("small") && raw["small"].is_boolean() && raw["small"].get<bool>();
      for (const auto& [nm, v] :
           {std::pair<std::string, std::string>{"xtmodel", "fe"}, {"ivar", xtPanel->ivar},
            {"tvar", xtPanel->tvar}, {"predict", "xtivreg2_p"}, {"vce", vceMacro},
            {"small", smallOn ? "small" : ""}}) {
        session.eval.setEString(nm, v);
        session.noteEOnFit(nm);
      }
    }
  }
  if (ivreg2Family && raw.is_object()) {
    auto put = [&session](const std::string& nm, double v) {
      if (!std::isfinite(v)) return;
      session.eval.setEScalar(nm, v);
      session.noteEOnFit(nm);
    };
    auto num = [&raw](const char* key) {
      return raw.contains(key) && raw[key].is_number() ? raw[key].get<double>() : std::nan("");
    };
    double cdf = std::nan("");
    if (iv.endog.size() == 1 && raw.contains("first_stage_F") && raw["first_stage_F"].is_object() &&
        raw["first_stage_F"].contains(iv.endog[0]) && raw["first_stage_F"][iv.endog[0]].is_number())
      cdf = raw["first_stage_F"][iv.endog[0]].get<double>();
    else if (iv.endog.size() > 1)
      cdf = num("cragg_donald_F");
    const bool robustVce = lm.vcov == "hc1" || lm.vcov == "cluster";
    const double rkf = robustVce ? num("kp_wald_F") : std::nan("");
    // xtivreg2: the identification statistics carry dofminus/sdofminus conventions
    // the engine's kp/cdf numbers are not verified against, so they stay unposted.
    if (ivreg2Typed) {
    put("cdf", cdf);
    put("rkf", rkf);
    put("widstat", robustVce ? rkf : (lm.vcov == "iid" ? cdf : std::nan("")));
    }
    if (ivreg2Typed && robustVce && iv.endog.size() == 1) {
      put("idstat", num("kp_lm"));
      put("idp", num("kp_lm_p"));
      put("iddf", num("kp_lm_df"));
    }
    // Two-way cluster: e(N_clust) is the smaller count, and each dimension's own
    // (ivreg2; w2d_uncovered.log:1866 `|5|5|6|`).
    if (ivMultiway && raw.contains("n_clusters_by") && raw["n_clusters_by"].is_array() &&
        raw["n_clusters_by"].size() == 2) {
      for (size_t d = 0; d < 2; ++d)
        if (raw["n_clusters_by"][d].is_number())
          put("N_clust" + std::to_string(d + 1), raw["n_clusters_by"][d].get<double>());
    }
    // e(first): rows rmse ... APr2, one column per endogenous regressor
    // (ivreg2; posted with first, ffirst or savefirst).
    if (wantFirstMatrix) {
      static const std::vector<std::string> kRows = {
          "rmse", "sheapr2", "pr2", "F", "df", "df_r", "pvalue",
          "SWF", "SWFdf1", "SWFdf2", "SWFp", "SWchi2", "SWchi2p", "SWr2",
          "APF", "APFdf1", "APFdf2", "APFp", "APchi2", "APchi2p", "APr2"};
      eval::EvalMatrix m;
      m.rownames = kRows;
      m.data.assign(kRows.size(), {});
      bool complete = !iv.endog.empty();
      for (const auto& e : iv.endog) {
        if (!savedFirst.contains(e) || !savedFirst[e].contains("ffirst") ||
            !savedFirst[e]["ffirst"].is_array() || savedFirst[e]["ffirst"].size() != kRows.size()) {
          complete = false;
          break;
        }
        m.colnames.push_back(e);
        for (size_t r = 0; r < kRows.size(); ++r) {
          const ojson& v = savedFirst[e]["ffirst"][r];
          m.data[r].push_back(v.is_number() ? v.get<double>() : std::nan(""));
        }
      }
      if (complete) {
        session.eval.eMatrices.emplace_back("first", std::move(m));
        session.noteEOnFit("first");
      }
    }
    if (wantSavefirst) {
      session.eval.setEString("firsteqs", firstEqs);
      session.noteEOnFit("firsteqs");
    }
  }
  // A noconstant fit's e(b) has no `_cons` (resolveEMatrixFor's kNeedCons
  // reads this mark, as after `regress ..., noconstant`).
  if (ivNoConstant && session.lastEstBlock.has_value() && session.lastEstBlock->is_object())
    (*session.lastEstBlock)["noconstant"] = true;
  // Stata's ordered `: e(scalars)' after the built-in multiway 2SLS fit
  // (w2s_multiway_class mw_iv_c2_scalars); every other IV fit keeps the refusal.
  session.eNameContract.reset();
  if (stataCmd == "ivregress" && method == "2sls" && !ivNoConstant && !sawAbsorb &&
      !wantSmallOpt && !wantPartial && !parsed.weightType.has_value() &&
      (lm.vcov == "twoway" || lm.vcov == "multiway"))
    postENameListsIvregressM4(session, raw, lm.ivEndog.size());
  return block;
}

ojson runIvRegress(const std::string& method, const eval::ParsedCommand& parsed, bool allowAbsorb,
                    const std::string& stataCmd, SessionState& session, bool smallDefault,
                    bool forceSmall) {
  return runIvRegress(method, parsed, allowAbsorb, stataCmd, session, smallDefault, forceSmall,
                      nullptr);
}

// ===========================================================================
// P7 — IV maximum likelihood: ivprobit / ivtobit
//
// `ivprobit ybin x1 (endog=z), vce(robust)` — the same `(endog = instruments)`
// varlist shape ivregress already parses, so parseIvParenM4 is reused rather
// than given a second, subtly different implementation.

// Install the engine's actual named full parameter vector/covariance. The
// displayed structural table stays compact; every stored coefficient reader
// sees the full equation stripe on the session block.
static bool postNamedMleFitM4b(SessionState& session, const ojson& raw) {
  if (!raw.contains("e_b") || !raw.contains("e_V") || !session.lastEstBlock.has_value()) return false;
  const auto& eb = raw["e_b"];
  const auto& ev = raw["e_V"];
  if (!eb.contains("names") || !eb.contains("values") || !ev.contains("names") ||
      !ev.contains("matrix") || eb["names"] != ev["names"]) return false;
  const size_t k = eb["names"].size();
  if (eb["values"].size() != k || ev["matrix"].size() != k) return false;
  eval::EvalMatrix b, V;
  b.rownames = {"y1"};
  b.data.emplace_back();
  ojson ct = ojson::array();
  for (size_t i = 0; i < k; ++i) {
    if (!eb["names"][i].is_string() || !eb["values"][i].is_number() ||
        !ev["matrix"][i].is_array() || ev["matrix"][i].size() != k) return false;
    const std::string name = eb["names"][i].get<std::string>();
    const double value = eb["values"][i].get<double>();
    const double se = std::sqrt(std::max(0.0, ev["matrix"][i][i].get<double>()));
    b.colnames.push_back(name);
    b.data[0].push_back(value);
    V.colnames.push_back(name);
    V.rownames.push_back(name);
    V.data.push_back(ev["matrix"][i].get<std::vector<double>>());
    ct.push_back(ojson{{"name", name}, {"b", value}, {"se", se}});
  }
  (*session.lastEstBlock)["coeftable"] = std::move(ct);
  (*session.lastEstBlock)["e_V"] = ev;
  session.eval.eMatrices.emplace_back("b", std::move(b));
  session.eval.eMatrices.emplace_back("V", std::move(V));
  return true;
}

// BUG-1500: the two-step fit of runIvMl, `ivprobit ..., twostep`. The estimator
// has its own covariance and no ML option (StataBE 19.5, scratch-laneA2 feat/i1.do):
// vce(robust) / vce(cluster id) / vce(oim) are r(198) "vcetype robust not allowed",
// the `robust` and `cluster(id)` options r(198) "option robust not allowed" /
// "option cluster(id) not allowed", and vce(twostep) is the default. vce(bootstrap)
// and vce(jackknife) are served by Stata and refused here by name. After the fit
// Stata posts e(method) e(vce) "twostep", e(chi2type) "Wald", e(predict)
// "ivprobit_p", e(depvar) "ybe xe", e(exog) "x1 z2" (exogenous, then the
// instruments), e(endog) "xe" and e(properties) "b V"; the e() scalars come from
// the engine reply (chi2 df_m p_chi2 chi2_exog df_exog p_exog k_vars N_cds N_cdf).
static ojson runIvProbitTwostepM4b(const eval::ParsedCommand& parsed, SessionState& session,
                                   std::vector<std::string> argv, const IvParenResult& iv) {
  for (const auto& [k, v] : parsed.opts) {
    if (k == "robust") return stataRefusalM4b("option robust not allowed", 198);
    if (k == "cluster")
      return stataRefusalM4b(v.isBool ? std::string("option cluster not allowed")
                                      : "option cluster(" + trimM4(v.strValue) + ") not allowed",
                             198);
    if ((k == "vce" || k == "vcov") && !v.isBool) {
      const std::string raw = trimM4(v.strValue);
      const std::string word = lowerM4(raw.substr(0, raw.find_first_of(" \t,")));
      if (word == "twostep" && raw.find_first_of(" \t,") == std::string::npos) continue;
      if (word == "bootstrap" || word == "jackknife")
        return engineErrorBlockM4("Option `vce(" + word + ")` is a valid Stata option that this "
                                  "version of PIET does not implement for `ivprobit, twostep`.",
                                  "unsupported_option");
      const std::string canon = canonicalVceType(raw, nullptr);
      return stataRefusalM4b("vcetype " + (canon.empty() ? word : canon) + " not allowed", 198);
    }
  }
  argv.push_back("--twostep");
  pushLevelIfGivenM4(parsed, argv);
  argv.push_back("--emit-v");
  ojson raw;
  ojson block = runP7Mle("ivprobit", argv, session, &raw);
  if (block.value("type", std::string()) == "error") return block;
  if (!postNamedMleFitM4b(session, raw))
    return engineErrorBlockM4("the two-step fit returned an incomplete parameter covariance");
  auto putE = [&](const char* name, const std::string& value) {
    session.eval.setEString(name, value);
    session.noteEOnFit(name);
  };
  // e(vce) is republished by every model install, as logit's `oim` (FU-30).
  session.eval.setEString("vce", "twostep");
  putE("method", "twostep");
  putE("chi2type", "Wald");
  putE("predict", "ivprobit_p");
  putE("properties", "b V");
  std::vector<std::string> exog(iv.exog);
  exog.insert(exog.end(), iv.instruments.begin(), iv.instruments.end());
  putE("depvar", iv.y + " " + joinSpaceM4(iv.endog));
  putE("exog", joinSpaceM4(exog));
  putE("endog", joinSpaceM4(iv.endog));
  // e(p), the model test's p-value; e(p) resolves from the reply's `p_F`, which a
  // chi2 test does not carry.
  if (raw.contains("p_chi2") && raw["p_chi2"].is_number()) {
    session.eval.setEScalar("p", raw["p_chi2"].get<double>());
    session.noteEOnFit("p");
  }
  return block;
}

ojson runIvMl(const std::string& cobraCmd, const eval::ParsedCommand& parsed,
              SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  // BUG-1500: `ivprobit ..., twostep`, Newey's two-step estimator (the engine's
  // --twostep; cmds_ivml.cpp run_ivprobit_twostep). ivtobit's two-step form is
  // not implemented.
  bool twostep = false;
  for (const auto& [k, v] : parsed.opts)
    if (k == "twostep" && v.isBool && v.boolValue) twostep = true;
  if (twostep && cobraCmd == "ivtobit")
    return engineErrorBlockM4("Option `twostep` is a valid Stata option that this version of PIET "
                              "does not implement for `ivtobit`.", "unsupported_option");
  // ivprobit/ivtobit keep refusing exogenous regressors after the block: no
  // captured arm measures that spelling for them.
  IvParenResult iv = parseIvParenM4(parsed.varlist, /*exogAfterParen=*/false);
  if (iv.errored) return engineErrorBlockM4("syntax_error: " + cobraCmd + " " + iv.errorMsg,
                                            "syntax_error");
  std::vector<std::string> argv = {cobraCmd, "--y", iv.y};
  if (!iv.exog.empty()) {
    argv.push_back("--x");
    argv.push_back(joinCommaM4(iv.exog));
  }
  argv.push_back("--endog");
  argv.push_back(joinCommaM4(iv.endog));
  argv.push_back("--instruments");
  argv.push_back(joinCommaM4(iv.instruments));
  if (parsed.ifExpr.has_value()) {
    argv.push_back("--keepif");
    argv.push_back(*parsed.ifExpr);
  }
  if (cobraCmd == "ivtobit") {
    bool sawLimit = false;
    for (const auto& [k, v] : parsed.opts) {
      if ((k == "ll" || k == "ul") && !v.isBool) {
        // Stata's bare `ll`/`ul` (censor at the observed extreme) is a different
        // request from `ll(0)`, and the engine has no bare form — rejecting is
        // parity; guessing a limit would answer a different question.
        argv.push_back(k == "ll" ? "--ll" : "--ul");
        argv.push_back(trimM4(v.strValue));
        sawLimit = true;
      } else if ((k == "ll" || k == "ul") && v.isBool) {
        return engineErrorBlockM4(
            "Option `" + k + "` without a value is a valid Stata option that this version of PIET "
            "does not implement for `ivtobit` — give an explicit limit, e.g. ll(0).",
            "unsupported_option");
      }
    }
    if (!sawLimit)
      return engineErrorBlockM4("syntax_error: ivtobit requires at least one of ll()/ul()",
                                "syntax_error");
  }
  if (twostep) return runIvProbitTwostepM4b(parsed, session, std::move(argv), iv);
  // vce
  // FU-23 (BUG-756): ivprobit/ivtobit take the cluster VCE ONLY as
  // vce(cluster var). w2l_vce_shape.log refuses every other spelling r(198):
  // the cluster() option in any shape -- `cluster(id)` (ivprobit_ctl_cluster
  // "option cluster(id) not allowed"), bare `cluster`, `cluster()`,
  // `cluster(id id2)` -- a valued `robust(x1)`, `vce(cluster id id2)`
  // ("multiway cluster VCE not allowed"), `vce(cluster)` ("option vce(cluster
  // clustvar) incorrectly specified") and `vce(robust x1)` ("option vce()
  // incorrectly specified"); ivtobit alike. This loop used to fit a clustered
  // model for cluster(id), skip a bare `cluster` and `robust(x1)` (iid at rc 0),
  // drop the `x1` of vce(robust x1), and hand `id id2` to the engine as ONE
  // column name (rc 1).
  const VceShapesM4b vceShape = readVceShapesM4b(parsed);
  if (vceShape.cluster != OptShapeM4b::kAbsent) {
    return stataRefusalM4b(vceShape.cluster == OptShapeM4b::kBare
                               ? std::string("option cluster not allowed")
                               : "option cluster(" + vceShape.clusterRaw + ") not allowed",
                           198);
  }
  if (vceShape.robust == OptShapeM4b::kEmpty || vceShape.robust == OptShapeM4b::kOne ||
      vceShape.robust == OptShapeM4b::kMany) {
    return stataRefusalM4b("option robust(" + vceShape.robustRaw + ") not allowed", 198);
  }
  if (vceShape.vceType == "cluster" && vceShape.vceRest.size() > 1) {
    return stataRefusalM4b("multiway cluster VCE not allowed", 198);
  }
  if (vceShape.vceType == "cluster" && vceShape.vceRest.empty()) {
    return stataRefusalM4b("option vce(cluster clustvar) incorrectly specified", 198);
  }
  if ((vceShape.vceType == "robust" || vceShape.vceType == "hc1" || vceShape.vceType == "iid") &&
      !vceShape.vceRest.empty()) {
    return stataRefusalM4b("option vce() incorrectly specified", 198);
  }
  std::optional<std::string> vceVal;
  bool robustFlag = false;
  for (const auto& [k, v] : parsed.opts) {
    if ((k == "vce" || k == "vcov") && !vceVal.has_value())
      vceVal = v.isBool ? std::string() : v.strValue;
    else if (k == "robust" && v.isBool && v.boolValue)
      robustFlag = true;
  }
  std::string want, cvar;
  if (vceVal.has_value() && !vceVal->empty()) {
    // W1b: one canonicaliser (piet/handlers.h), measured abbreviation floors.
    std::string vceRest;
    const std::string lv = canonicalVceType(*vceVal, &vceRest);
    if (lv == "cluster") {
      want = "cluster";
      cvar = vceRest;
    } else {
      want = lv.empty() ? lowerM4(*vceVal) : lv;
    }
  } else if (robustFlag) {
    want = "robust";
  }
  if (!want.empty()) {
    if (want != "iid" && want != "robust" && want != "cluster")
      return engineErrorBlockM4(
          "Option `vce(" + want + ")` is a valid Stata option that this version of PIET does not "
          "implement for `" + cobraCmd + "`.", "unsupported_option");
    argv.push_back("--vce");
    argv.push_back(want);
    if (want == "cluster") {
      argv.push_back("--cluster");
      argv.push_back(cvar);
    }
  }
  pushLevelIfGivenM4(parsed, argv);
  argv.push_back("--emit-v");
  ojson raw;
  ojson block = runP7Mle(cobraCmd, argv, session, &raw);
  if (block.value("type", std::string()) == "error") return block;
  if (!postNamedMleFitM4b(session, raw))
    return engineErrorBlockM4("the IV-ML fit returned an incomplete parameter covariance");
  return block;
}

// ===========================================================================
// reg3 (BS-065): `reg3 (y1 x1 x2) (y2 x3 y1) [(y3 ...) ...] [, sure]`

ojson runReg3(const eval::ParsedCommand& parsed, SessionState& session,
              const std::string& stataCmd) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  // sureg is `reg3 <eqns> ..., sure` with e(cmd)
  // set to sureg; its isure/dfk/small/corr/constraints are not
  // forwarded by this build, so they are refused by name below, never ignored.
  const bool isSureg = stataCmd == "sureg";
  if (isSureg) {
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabSureg.known,
                                              kVocabSureg.unimplemented)) {
      return *err;
    }
  }

  // splitWs() already broke each parenthesised equation into separate
  // varlist tokens; rejoin with single spaces and re-split on the paren
  // structure (same rationale as parseIvParenM4 above — Stata's own
  // whitespace inside the parens carries no meaning).
  std::string joined = joinSpaceM4(parsed.varlist);
  std::vector<std::string> groups;
  bool parenOk = true;
  {
    size_t i = 0;
    while (i < joined.size()) {
      while (i < joined.size() && (joined[i] == ' ' || joined[i] == '\t')) ++i;
      if (i >= joined.size()) break;
      if (joined[i] != '(') {
        parenOk = false;
        break;
      }
      size_t close = joined.find(')', i);
      if (close == std::string::npos) {
        parenOk = false;
        break;
      }
      groups.push_back(joined.substr(i + 1, close - i - 1));
      i = close + 1;
    }
  }
  if (!parenOk) {
    return engineErrorBlockM4(
        "syntax_error: reg3 requires each equation in parentheses: (depvar regressors...)",
        "syntax_error");
  }
  if (groups.size() < 2) {
    return engineErrorBlockM4(
        "syntax_error: reg3 requires at least two equations: (y1 x1 x2) (y2 x3 y1)",
        "syntax_error");
  }

  std::vector<std::string> eqArgs;
  for (const auto& g : groups) {
    std::vector<std::string> toks = splitTokensM4(g);
    if (toks.size() < 2) {
      return engineErrorBlockM4(
          "syntax_error: malformed reg3 equation `(" + g + ")`: needs a dependent variable and "
          "at least one regressor",
          "syntax_error");
    }
    std::string depvar = toks[0];
    std::vector<std::string> regs(toks.begin() + 1, toks.end());
    eqArgs.push_back(depvar + "=" + joinCommaM4(regs));
  }

  bool sawSure = isSureg, sawExplicit3sls = false;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "sure" && v.isBool && v.boolValue) sawSure = true;
    if (k == "3sls" && v.isBool && v.boolValue) sawExplicit3sls = true;
  }
  if (sawSure && sawExplicit3sls) {
    return engineErrorBlockM4("syntax_error: `sure` and `3sls` may not be combined", "syntax_error");
  }

  // vce()/robust/cluster/noconstant/level — cobra reg3 has NO --vcov/
  // --no-intercept/--level surface at all (live-verified: all three hard-
  // error "unrecognized option" at cobra's own CLI layer, exit nonzero, but
  // (--no-intercept/--level only, NOT --vcov) still print a full valid
  // stdout JSON first — the SAME cobra_runner.cpp BUG-039 masking shape the
  // ivregress/liml branch above documents; --vcov itself errors cleanly
  // with empty stdout, but is rejected here anyway for a uniform, honest
  // message rather than relying on that distinction). Reject before ever
  // building the argv.
  for (const auto& [k, v] : parsed.opts) {
    (void)v;
    if (k == "vce" || k == "vcov" || k == "robust" || k == "cluster") {
      return engineErrorBlockM4(
          "Option `" + k + "` is a valid Stata option that this version of PIET does not yet "
          "implement for `" + stataCmd + "` (the engine's reg3 estimator has no vcov surface yet — classical "
          "SEs only).",
          "unsupported_option");
    }
  }
  for (const auto& [k, v] : parsed.opts) {
    if ((k == "noconstant" || k == "nocons") && v.isBool && v.boolValue) {
      return engineErrorBlockM4(
          "Option `" + k + "` is a valid Stata option that this version of PIET does not yet "
          "implement for `" + stataCmd + "`.",
          "unsupported_option");
    }
    if (k == "level" && !v.isBool) {
      return engineErrorBlockM4(
          "Option `level` is a valid Stata option that this version of PIET does not yet "
          "implement for `" + stataCmd + "`.",
          "unsupported_option");
    }
  }

  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kKnown = kVocabReg3.known;
  if (!isSureg) {
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kKnown)) {
      return *err;
    }
  }

  std::vector<std::string> argv = {"reg3"};
  for (const auto& e : eqArgs) {
    argv.push_back("--eq");
    argv.push_back(e);
  }
  // Stata's OWN default is 3SLS (sure only when explicitly requested) —
  // oracle-verified (D1g, no options, matches --method 3sls exactly) —
  // while cobra's reg3 default is the OPPOSITE (SUR unless --method 3sls is
  // given — oracle D1h/`sure` matches cobra's no-flag default exactly). So
  // --method must ALWAYS be sent explicitly; relying on cobra's own default
  // would silently swap which method every plain `reg3 (...) (...)` (no
  // options) call actually ran.
  argv.push_back("--method");
  argv.push_back(sawSure ? "sure" : "3sls");
  if (parsed.ifExpr.has_value()) {
    argv.push_back("--keepif");
    argv.push_back(*parsed.ifExpr);
  }
  argv.push_back("--data");
  argv.push_back(*session.currentHbin());
  argv.push_back("--tidy");

  ojson raw;
  try {
    // --emit-v: the stacked system e(V) (y:x1 ... x3:_cons) that e(b)/e(V)
    // and estout's matrix mode read; not part of argv (a refit flag).
    std::vector<std::string> fitArgv = argv;
    fitArgv.push_back("--emit-v");
    raw = runCobraJsonM4(fitArgv);
  } catch (const CobraError& ce) {
    return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
  }

  ojson block = cobraToEstimationM4(raw, "reg3", &session);

  LastModel lm;
  lm.cobraCmd = "reg3";
  if (isSureg) lm.stataCmd = "sureg";
  std::vector<std::string> firstToks = splitTokensM4(groups[0]);
  lm.y = firstToks.empty() ? std::string() : firstToks.front();
  // No single lm.x is fully coherent for a multi-equation system; best-
  // effort populate from the FIRST equation only (test/lincom/margins/
  // predict after reg3 inherit the SAME coarse-approximation caveat noted
  // on the IV path above — a pre-existing, system-wide LastModel
  // simplification, not new here).
  if (firstToks.size() > 1) lm.x.assign(firstToks.begin() + 1, firstToks.end());
  lm.depvar = lm.y;
  captureFitFilterM4(lm, argv);
  lm.dataPath = *session.currentHbin();
  lm.result = raw;
  session.setLastModel(lm);
  session.lastEstBlock = block;
  session.attachFitCovariance(raw);  // e(V): the engine's stacked system covariance
  return block;
}

// ===========================================================================
// boottest (BS-066): wild cluster bootstrap post-estimation, requires a
// preceding vce(cluster ...) fit in session.lastModel().

namespace {

// FU-133 (round 4) and boottest-h0-noest-served-returns: the text boottest.ado
// itself prints for a fit its estimator checks refuse, or "" for a fit it goes
// on to bootstrap. Each message is printed at rc 0, not 198: the whole of
// boottest.ado's body runs inside `cap noi { }` (ado to its close at ado,
// `end` at ado, no re-raise), so its _assert rc(198) only prints. The checks
// run in the ado's own order (ado), each keyed on the condition the ado
// tests:
//  - ado `e(prefix)` "svy": a `svy:` fit is cached with the engine's "svy"
//    block (runSvyPrefix; the same mark postestRefitMatchesFitM4 reads).
//    w2t_boottest_family.log:2491 `Doesn't work after svy` (H1).
//  - ado xtreg with e(model) other than "fe": the ado prints `{`cmd',
//    `e(model)'}` with the braces kept (no SMCL directive). runXtreg caches its
//    RE arm under cobraCmd "xtreg" (its FE arm is feols with stataCmd "xtreg",
//    and be/mle/pa never fit), so cobraCmd "xtreg" is exactly xtreg, re.
//    :1834 `Doesn't work after {xtreg, re}` (E7).
//  - ado two or more words in `fvunab e(absvars)` after reghdfe/ivreghdfe:
//    this counts the absorb() TERMS the fit recorded in lm.fe (reghdfe's --fe
//    list, handlers.cpp's absorb arm; the ivreghdfe list at the IV path's absorb
//    branch above), so an `a#b` term is one term and is served, as there. Truth:
//    p9_stata.log:223/:228/:231 (reghdfe), p5_stata.log:843/:848/:851 (ivreghdfe);
//    w2t :1276/:1282 (D4), :1361/:1367 (D5), :1715/:1721 (E5), :1893/:1899 (E8).
//  - ado `ivregress` with e(estimator) "gmm", or ivreg2 (an ivreghdfe without
//    absorb() counts as ivreg2, ado) with e(model) "gmm2s": the engine's iv
//    reply names its two-step GMM fit "ivgmm" (apps/cmds/common.cpp, cmd_iv's
//    `estimator`), the only record of the method LastModel keeps. An ivreghdfe
//    that absorbs keeps cmd ivreghdfe there and is not this check's.
//    :1405 `GMM no longer supported` (D6). `ivreg2 ..., gmm2s` without robust or
//    cluster() fits as 2SLS (the two weight matrices coincide) but posts e(model)
//    gmm2s, which is what the ado reads: `eModel` (StataBE 19.5, BUG-1223).
//  - `ivregress ..., absorb()` keeps cmd ivregress, which the absvars check above
//    does not read (BUG-1179); runBoottest answers it after its option checks.
std::string boottestStataRefusalM4b(const LastModel& lm, const std::string& eModel) {
  if (lm.result.has_value() && lm.result->is_object() && lm.result->contains("svy")) {
    return "Doesn't work after svy";
  }
  if (lm.cobraCmd == "xtreg") return "Doesn't work after {xtreg, re}";
  if (lm.fe.size() >= 2) {
    if (lm.stataCmd == "reghdfe") {
      return "Doesn't work after reghdfe with more than one set of absorbed fixed effects or "
             "with absorbed interaction terms";
    }
    // An IV fit's lm.fe is absorb() on the ivreghdfe path or on ivregress's own.
    if ((lm.cobraCmd == "iv" || lm.cobraCmd == "liml") && lm.stataCmd != "ivregress") {
      return "Doesn't work after ivreghdfe with more than one set of absorbed fixed effects or "
             "with absorbed interaction terms";
    }
  }
  if (lm.cobraCmd == "iv" && lm.fe.empty() && lm.result.has_value() && lm.result->is_object()) {
    const std::string est = lm.result->value("estimator", std::string());
    if (est == "ivgmm" || est == "ivgmm_multi") return "GMM no longer supported";
  }
  if ((lm.stataCmd == "ivreg2" || lm.stataCmd == "xtivreg2") && eModel == "gmm2s")
    return "GMM no longer supported";
  return std::string();
}

// boottest.ado's own message: one display block, rc 0, no bootstrap, and r()
// emptied -- every message record of the capture reads `mean=. N=.` after a
// summarize and prints an empty `return list` (B1 :104-:231, D4_r :1293, G1_r
// :2444). replaceRClass, never a clear of rStrings, which also holds s()
// (FU-168/FU-238); the empty declaration makes r() known-empty, so an unset
// r(p) reads `.` and `r(clustvars)' reads empty, as Stata's do.
ojson boottestMessageM4b(SessionState& session, const std::string& text) {
  eval::replaceRClass(session.eval);
  eval::declareRPosted(session.eval, "boottest", {});
  ojson note;
  note["type"] = "display";
  note["text"] = text;
  return note;
}

// ado: `local ML = e(converged) != . & !inlist("`cmd'", "reghdfe",
// "ivreghdfe", "reghdfejl")`. e(converged) is read where the macro layer reads
// it (eval/macros.cpp resolveEScalar): the fit's cached reply first, then a
// posted e-scalar. logit, probit, poisson, glm, nbreg, tobit and ivprobit carry
// it; regress, areg, reghdfe, xtreg and ivregress do not (bin/main probe
// 2026-10-14: `display e(converged)` 1 and `.` respectively). StataNow's
// `ivregress ..., absorb()` (the Halperin APM fit) sets it: boottest takes that
// fit for an ML one and re-fits it under the null (StataBE 19.5, BUG-1179:
// "Re-running regression with null imposed."), though PIET's reply has no key.
bool boottestFitIsMlM4b(const SessionState& session, const LastModel& lm) {
  if (lm.stataCmd == "reghdfe" || lm.stataCmd == "ivreghdfe") return false;
  if (lm.stataCmd == "ivregress" && !lm.fe.empty()) return true;
  if (lm.result.has_value() && lm.result->is_object() && lm.result->contains("converged")) {
    const ojson& c = (*lm.result)["converged"];
    if (c.is_boolean()) return true;
    if (c.is_number()) {
      const double v = c.get<double>();
      return std::isfinite(v) && std::fabs(v) < eval::kStataMissingSentinel;
    }
  }
  const double* conv = session.eval.eScalars.find("converged");
  return conv != nullptr && std::isfinite(*conv) && std::fabs(*conv) < eval::kStataMissingSentinel;
}

// boottest after an IV fit is the wild restricted efficient bootstrap
// (boottest: IV = e(instd) set, WRE = !score & IV & reps), served by
// the engine's `wildboot --wre` for the fits whose model this path can rebuild:
// `ivregress 2sls`/`ivregress liml` and `ivreghdfe` (2SLS, one absorbed set --
// two sets answer with the ado's message in boottestStataRefusalM4b, as GMM
// does). Weighted fits stay refused (the engine's WRE takes no weights).
bool boottestIvWreM4b(const LastModel& lm) {
  if (lm.cobraCmd != "iv" && lm.cobraCmd != "liml") return false;
  if (lm.stataCmd != "ivregress" && lm.stataCmd != "ivreghdfe") return false;
  if (lm.ivEndog.empty() || lm.ivInstruments.empty()) return false;
  if (lm.stataCmd == "ivreghdfe" && (lm.fe.size() != 1 || lm.cobraCmd != "iv")) return false;
  if (lm.stataCmd == "ivregress" && !lm.fe.empty()) return false;
  if (lm.result.has_value() && lm.result->is_object()) {
    const std::string est = lm.result->value("estimator", std::string());
    if (est == "ivgmm" || est == "ivgmm_multi" || est == "fuller") return false;
  }
  return true;
}

// The hypothesis before the comma. boottest.ado (ado) wraps it in `{}`
// and hands each `(...)` part to `constraint`, so `x1`, `x1 = 0.0625` and
// `x1=0.0625` (w2t C1 :129, C10 :247, C11 :260) are all one constraint, the
// coefficient on x1 equal to the number (0 when no `=`). The engine tests one
// coefficient against one number (wildboot --param, --null), so exactly that
// shape is read here: a single coefficient name, optionally `= <number>`.
// Anything else -- a second term (`x1 x2`, which bin/main served as a test of x1
// alone), a `{...}` or `(...)` list, a linear combination (`x1+x2=1`, `2*x1=1`),
// `_b[x1]`, or a right-hand side that is not a number -- returns false, and
// runBoottest refuses it by name (PIET has no constraint machinery for
// boottest; no captured line covers those shapes).
bool parseBoottestHypothesisM4b(const eval::ParsedCommand& parsed, std::string& param,
                                std::optional<std::string>& nullValue) {
  std::string text = trimM4(parsed.argText);
  if (text.empty()) text = joinSpaceM4(parsed.varlist);
  static const std::regex kOneCoef(
      R"(^([A-Za-z0-9_][A-Za-z0-9_.#]*)\s*(?:=\s*([+-]?(?:[0-9]+\.?[0-9]*|\.[0-9]+)(?:[eE][+-]?[0-9]+)?))?$)");
  std::smatch m;
  if (!std::regex_match(text, m, kOneCoef)) return false;
  param = m[1].str();
  if (m[2].matched) nullValue = m[2].str();
  return true;
}

// The report boottest prints for one wild-bootstrapped hypothesis (boottest),
// handed to the dispatcher as a display block ahead of the estimation block (StataBE
// 19.5, BUG-1175, 7 clusters, reps(999)):
//
//   Warning: with 7 bootstrap clusters, the number of replications, 999, exceeds the
//   universe of Rademacher draws, 2^7 = 128. Sampling each once.        (one line)
//   Consider Webb weights instead, using weight(webb).
//
//   Wild bootstrap-t, null imposed, 128 replications, Wald test, bootstrap clustering
//   by cl, Rademacher weights:                                          (one line)
//     x1
//
//                               t(6) =    24.4529
//                           Prob>|t| =     0.0000
//
// The warning is printed when 2^G falls short of the replications asked for (Rademacher
// or Mammen weights; :834-838), the note on the level when G > 12 or the weights are not
// Rademacher and level*(B+1)/100 is not whole. `clustering by` names boottest's own
// cluster() (the override), `bootstrap clustering by` the bootstrap clusters.
// The statistic is t(df_r) under small and z otherwise, its line left out
// when PIET does not know boottest's statistic (r(t) is refused then too). `ciStr` is
// r(CIstr) where PIET computed the confidence set (boottestCiM4b), printed after the
// statistic as "95% confidence set for null hypothesis expression: [...]".
std::string boottestReportM4b(const std::string& preNotes, long long repsAsked, const ojson& raw,
                              double level, const std::string& clusterOpt,
                              const std::vector<std::string>& bootClusters,
                              const std::string& hypothesis, bool small,
                              std::optional<double> stat, std::optional<double> dfR,
                              const std::string& ciStr) {
  auto num = [&raw](const char* key) {
    return raw.contains(key) && raw[key].is_number() ? raw[key].get<double>() : std::nan("");
  };
  const double nBoot = num("n_clusters");
  const double reps = num("reps");
  const std::string wt = raw.value("weights", std::string("rademacher"));
  const std::string wtProper = wt.empty() ? wt : static_cast<char>(std::toupper(
                                                     static_cast<unsigned char>(wt[0]))) + wt.substr(1);
  char buf[512];
  std::string out = preNotes;
  if (std::isfinite(nBoot) && nBoot < 63 && (wt == "rademacher" || wt == "mammen") &&
      std::ldexp(1.0, static_cast<int>(nBoot)) < static_cast<double>(repsAsked)) {
    // Mammen weights get the warning too, without "Sampling each once." (they are
    // drawn, not enumerated); the line keeps its trailing blank (StataBE 19.5,
    // BUG-1519: 8 clusters, reps(999), "... universe of Mammen draws, 2^8 = 256. ").
    std::snprintf(buf, sizeof(buf),
                  "\nWarning: with %.0f bootstrap clusters, the number of replications, %lld, "
                  "exceeds the universe of %s draws, 2^%.0f = %.0f. %s"
                  "\nConsider Webb weights instead, using weight(webb).\n",
                  nBoot, repsAsked, wtProper.c_str(), nBoot,
                  std::ldexp(1.0, static_cast<int>(nBoot)),
                  wt == "rademacher" ? "Sampling each once." : "");
    out += buf;
  }
  if (std::isfinite(reps) && reps > 0 && std::isfinite(nBoot) && (nBoot > 12 || wt != "rademacher")) {
    const double lv = level / 100.0 * (reps + 1.0);
    if (std::floor(lv) != lv) {
      std::snprintf(buf, sizeof(buf),
                    "\nNote: The bootstrap usually performs best when the confidence level (here, "
                    "%g%%)\n      times the number of replications plus 1 (%.0f+1=%.0f) is an "
                    "integer.\n",
                    level, reps, reps + 1.0);
      out += buf;
    }
  }
  std::snprintf(buf, sizeof(buf), "\nWild bootstrap-t, null imposed, %.0f replications, Wald test",
                reps);
  out += buf;
  if (!clusterOpt.empty()) out += ", clustering by " + clusterOpt;
  if (!bootClusters.empty()) out += ", bootstrap clustering by " + joinSpaceM4(bootClusters);
  out += ", " + wtProper + " weights:\n  " + hypothesis + "\n";
  if (stat.has_value()) {
    char line1[64];
    if (small && dfR.has_value())
      std::snprintf(line1, sizeof(line1), "t(%.0f)", *dfR);
    else
      std::snprintf(line1, sizeof(line1), "%s", small ? "t" : "z");
    const std::string ptext = small ? "Prob>|t|" : "Prob>|z|";
    const double p = num("p_symmetric");
    auto f104 = [](double v) {
      char b[64];
      if (std::isfinite(v)) std::snprintf(b, sizeof(b), "%10.4f", v);
      else std::snprintf(b, sizeof(b), "%10s", ".");
      return std::string(b);
    };
    out += "\n" + std::string(32 - std::min<size_t>(32, std::strlen(line1)), ' ') + line1 +
           " = " + f104(*stat) + "\n" + std::string(32 - ptext.size(), ' ') + ptext + " = " +
           f104(p);
  }
  if (!ciStr.empty()) {
    std::snprintf(buf, sizeof(buf), "\n\n%g%% confidence set for null hypothesis expression: ",
                  level);
    out += buf + ciStr;
  }
  return out;
}

// boottest's confidence set for one hypothesis by test inversion (boottest.mata
// plot() and search(), the one-dimensional case; StataBE 19.5, BUG-1174: 8 clusters,
// 2^8 draws, `boottest x1 = 2` r(CI) (1.9097521809349358, 2.087247394569717), r(CIstr)
// "[1.91, 2.087]", r(plot) 25 x 2). `pAt(r)` is the bootstrap's symmetric p-value with
// the null at r, on the same draws. The method, as boottest's:
//  - the grid: the estimate b plus and minus 1.5 * invnormal(1 - alpha/2) * sqrt(V)
//    (V = r(V)); the bound farther from the tested null is checked first, and while
//    a bound's p-value exceeds alpha the grid is widened by its own width on both
//    sides, at most ten times per side (p at the near bound is not re-read when the
//    far side's widening moves it);
//  - 25 equally spaced points (rangen: a + k(b-a)/24, the ends exact) and the estimate
//    itself at p = 1, inserted in order;
//  - each crossing of alpha between neighbours is refined by Chandrupatla's (1997)
//    hybrid quadratic-bisection search, stopping when a p-value lies within 1/B of
//    alpha (B the feasible replications; the symmetric p's tolerance) or the
//    bracket's relative width falls under ptol .001 (r(ptol)); the bracket end whose p
//    is nearer alpha is the bound;
//  - a non-rejection region open at either end of the grid is unbounded on that side.
// r(plot) is the 25-point grid without the estimate. nullopt when any p-value could not
// be read (no confidence set is posted then, as before).
struct BoottestCiM4b {
  std::vector<std::pair<double, double>> sets;  // NaN = unbounded on that side
  std::vector<double> plotX, plotY;
};
template <class PAt>
std::optional<BoottestCiM4b> boottestCiM4b(PAt&& pAt, double b, double V, double r0, double level,
                                           double bFeas) {
  const double alpha = 1.0 - level / 100.0;
  if (!(alpha > 0) || !(V > 0) || !std::isfinite(b) || !(bFeas > 0)) return std::nullopt;
  double z = std::nan("");
  {
    char expr[64];
    std::snprintf(expr, sizeof(expr), "invnormal(%.17g)", alpha / 2.0);
    if (const auto v = eval::evalArith(expr, nullptr)) {
      try {
        z = std::stod(*v);
      } catch (...) {
      }
    }
  }
  if (!std::isfinite(z)) return std::nullopt;
  const double halfwidth = -1.5 * z * std::sqrt(V);
  const double peak = b;
  double lo = peak - halfwidth, hi = peak + halfwidth;
  double pLo = std::nan(""), pHi = std::nan("");
  auto widenLow = [&] {
    for (int i = 10; i > 0; --i) {
      pLo = pAt(lo);
      if (!(pLo > alpha)) break;
      const double w = hi - lo;
      lo -= w;
      hi += w;
    }
  };
  auto widenHigh = [&] {
    for (int i = 10; i > 0; --i) {
      pHi = pAt(hi);
      if (!(pHi > alpha)) break;
      const double w = hi - lo;
      lo -= w;
      hi += w;
    }
  };
  if (std::fabs(lo - r0) > std::fabs(hi - r0)) {
    widenLow();
    widenHigh();
  } else {
    widenHigh();
    widenLow();
  }
  constexpr int kGrid = 25;
  std::vector<double> X(kGrid), Y(kGrid, std::nan(""));
  for (int k = 0; k < kGrid; ++k) X[k] = k * ((hi - lo) / (kGrid - 1)) + lo;
  X[0] = lo;
  X[kGrid - 1] = hi;
  Y[0] = pLo;
  Y[kGrid - 1] = pHi;
  size_t c;  // the estimate's index, 0-based
  if (peak < lo) {
    c = 0;
  } else if (peak > hi) {
    c = kGrid;
  } else {
    c = static_cast<size_t>(std::floor((peak - lo) / (hi - lo) * (kGrid - 1))) + 1;
  }
  X.insert(X.begin() + static_cast<std::ptrdiff_t>(c), peak);
  Y.insert(Y.begin() + static_cast<std::ptrdiff_t>(c), 1.0);
  for (size_t k = 0; k < Y.size(); ++k)
    if (std::isnan(Y[k])) Y[k] = pAt(X[k]);
  for (double y : Y)
    if (!std::isfinite(y)) return std::nullopt;

  constexpr double kPtol = 1e-3;
  bool failed = false;
  auto search = [&](double f1, double x1, double f2, double x2) {
    double t = 0.5, x3 = x2, f3 = f2;
    for (int guard = 0; guard < 1000; ++guard) {
      const double x = x1 + t * (x2 - x1);
      const double fx = pAt(x);
      if (!std::isfinite(fx)) {
        failed = true;
        return x;
      }
      if ((fx > f1) == (fx > f2)) return x;  // monotonicity broken: as good as it gets
      if ((fx < alpha) == (f1 < alpha)) {
        x3 = x1;
        x1 = x;
        f3 = f1;
        f1 = fx;
      } else {
        x3 = x2;
        x2 = x1;
        x1 = x;
        f3 = f2;
        f2 = f1;
        f1 = fx;
      }
      if (std::fabs(fx - alpha) < 1.0 / bFeas * 1.000001 ||
          std::fabs(x2 - x1) / (std::fabs(x1) + 1.0) < kPtol)
        return std::fabs(f1 - alpha) < std::fabs(f2 - alpha) ? x1 : x2;
      const double phi1 = (f1 - f2) / (f3 - f2);
      const double phi1Sq = phi1 * phi1;
      const double xi1 = (x1 - x2) / (x3 - x2);
      if (phi1Sq > xi1 || xi1 > phi1 + phi1 - phi1Sq) {
        t = 0.5;
      } else {
        t = ((f3 - alpha) / (f1 - f2) + (x3 - x1) / ((x2 - x1) * (f3 - f1)) * (f2 - alpha)) *
            (f1 - alpha) / (f3 - f2);
        t = std::min(0.999999, std::max(0.000001, t));
      }
    }
    failed = true;
    return x1;
  };
  // Crossings: a rising step (p up through alpha) opens a non-rejection region, a
  // falling one closes it; regions open at the grid's ends are unbounded there.
  std::vector<long> ups, downs;
  for (size_t k = 0; k + 1 < Y.size(); ++k) {
    const int d = static_cast<int>(Y[k + 1] > alpha) - static_cast<int>(Y[k] > alpha);
    if (d == 1) ups.push_back(static_cast<long>(k));
    if (d == -1) downs.push_back(static_cast<long>(k));
  }
  BoottestCiM4b out;
  const double kNan = std::nan("");
  if (ups.empty() && downs.empty()) {
    out.sets.push_back({kNan, kNan});
  } else {
    if (ups.empty()) ups.push_back(-1);
    else if (downs.empty()) downs.push_back(-1);
    else {
      if (ups.front() > downs.front()) ups.insert(ups.begin(), -1);
      if (ups.back() > downs.back()) downs.push_back(-1);
    }
    if (ups.size() != downs.size()) return std::nullopt;
    for (size_t i = 0; i < ups.size(); ++i) {
      const double l = ups[i] < 0 ? kNan
                                  : search(Y[ups[i]], X[ups[i]], Y[ups[i] + 1], X[ups[i] + 1]);
      const double h = downs[i] < 0
                           ? kNan
                           : search(Y[downs[i]], X[downs[i]], Y[downs[i] + 1], X[downs[i] + 1]);
      out.sets.push_back({l, h});
    }
  }
  if (failed) return std::nullopt;
  X.erase(X.begin() + static_cast<std::ptrdiff_t>(c));
  Y.erase(Y.begin() + static_cast<std::ptrdiff_t>(c));
  out.plotX = std::move(X);
  out.plotY = std::move(Y);
  return out;
}

// r(CIstr) (boottest): each set as `[lo, hi]` in %10.4g, an unbounded side
// as `(-∞` / `∞)`, sets joined by ` ∪ `, every `-` then printed as the minus sign `−`.
std::string boottestCiStrM4b(const BoottestCiM4b& ci) {
  std::string out;
  auto num = [](double v) { return strutil::trim(fmt::general(v, 10, 4)); };
  for (size_t i = 0; i < ci.sets.size(); ++i) {
    if (i > 0) out += " ∪ ";
    const auto& [l, h] = ci.sets[i];
    out += (std::isnan(l) ? std::string("(-∞") : "[" + num(l)) + ", " +
           (std::isnan(h) ? std::string("∞)") : num(h) + "]");
  }
  std::string minus;
  for (char ch : out) minus += ch == '-' ? std::string("−") : std::string(1, ch);
  return minus;
}

}  // namespace

// `reportOut` (BUG-1175): boottest's printed report (boottestReportM4b), a display block
// the dispatcher pushes ahead of the estimation block (declared there: piet/handlers.h
// keeps the two-argument form, below).
ojson runBoottest(const eval::ParsedCommand& parsed, SessionState& session, ojson* reportOut) {
  const auto& lmOpt = session.lastModel();
  // FU-240: ado `_assert "`e(cmd)'" != "", msg(No estimates detected)` inside
  // the ado's `cap noi` block: the message at rc 0 with r() emptied (w2t B1
  // :216/:221, B2 :237/:243/:248 `mean=. N=.`, `return list` :231 empty). This
  // was rc 1 ("No previous estimation result."). A posted e(cmd) with no model
  // behind it (`ereturn post` then `ereturn local cmd`) is a fit Stata would go
  // on to bootstrap, and PIET has nothing to bootstrap: refused by name.
  if (!lmOpt.has_value()) {
    const std::string eCmd = session.eval.eStrings.get("cmd", std::string());
    if (!eCmd.empty()) {
      return engineErrorBlockM4(
          "boottest after e() posted by `" + eCmd + "` without a fitted model is not "
          "supported in this version of PIET: there is no model to bootstrap.",
          "unsupported_option");
    }
    return boottestMessageM4b(session, "No estimates detected");
  }
  const LastModel& lm = *lmOpt;
  // BUG-1210: results read by `estimates use` carry no e(sample); boottest.ado
  // says so inside its `cap noi` block and returns rc 0 with r() empty (StataBE).
  if (lm.esample == LastModel::EsampleState::kNone)
    return boottestMessageM4b(session,
                              "Sample marker for the regression not found. Perhaps the data "
                              "set was cleared and reconstructed");
  // Only specification-proven, unweighted ML models use the score path.
  // Offsets and restricted-sample layouts have independent refit contracts.
  std::string scoreModel;
  if (lm.result && lm.result->is_object() && lm.fe.empty() && lm.fitWeight.empty() &&
      lm.fitKeepif.empty() && lm.fitDataPath.empty() && !lm.result->contains("svy")) {
    const auto& fit = *lm.result;
    bool hasConstant = false;
    const char* ck = fit.contains("coeftable") ? "coeftable" : "coefs";
    if (fit.contains(ck) && fit[ck].is_array()) for (const auto& row : fit[ck]) {
      const std::string name = row.is_object() ? row.value("name", std::string()) : std::string();
      if (name == "_cons" || name == "(intercept)") hasConstant = true;
    }
    if (hasConstant && (lm.cobraCmd == "logit" || lm.cobraCmd == "probit" || lm.cobraCmd == "poisson") &&
        fit.contains("has_offset") && fit["has_offset"].is_boolean() && !fit["has_offset"].get<bool>() &&
        fit.value("N_cdf", 0) == 0 && fit.value("N_cds", 0) == 0) scoreModel = lm.cobraCmd;
    if (hasConstant && lm.cobraCmd == "glm" && fit.contains("glm_model")) {
      const auto& gm = fit["glm_model"];
      // specToJson stores exposure as a boolean; offset is its variable name.
      if (gm.is_object() && gm.contains("offset") && gm["offset"].is_string() &&
          gm["offset"].get<std::string>().empty() && gm.contains("exposure") &&
          gm["exposure"].is_boolean() && !gm["exposure"].get<bool>() &&
          gm.contains("family") && gm["family"].is_number_integer() &&
          gm.contains("link") && gm["link"].is_number_integer() &&
          (!gm.contains("if") || gm["if"].is_null()) && (!gm.contains("in") || gm["in"].is_null())) {
        if (gm.value("family", -1) == 2 && gm.value("link", -1) == 1) scoreModel = "glm-poisson";
        if (gm.value("family", -1) == 0 && gm.value("link", -1) == 0) scoreModel = "glm-gaussian";
      }
    }
  }
  // boottest.ado's estimator checks (ado) run before its syntax (ado),
  // its option messages (ado) and its boottype() check (ado), so
  // a fit it refuses answers with its message ahead of every check below: E8
  // (w2t_boottest_family.log:1893) prints the estimator text, not "Option cluster
  // not allowed.".
  if (const std::string text =
          boottestStataRefusalM4b(lm, session.eval.eStrings.get("model", std::string()));
      !text.empty()) {
    return boottestMessageM4b(session, text);
  }
  // FU-23 (BUG-758): cluster()/robust by shape, w2l_vce_shape.log (each arm
  // after `regress y x1 x2, vce(cluster id)`).
  //  - boottest_bare_cluster / boottest_robust_val: boottest prints "Option
  //    cluster not allowed." / "Option robust(x1) not allowed." and stops at
  //    rc 0 with nothing returned (r(p) ., r(clustvars) empty). This handler
  //    skipped the bare `cluster` and bootstrapped anyway; `robust(x1)` hit the
  //    reject net r(198). Answered as boottest does: the message, rc 0, r()
  //    emptied, no bootstrap. These are boottest.ado's `syntax` leftovers
  //    (ado), so they come before every refusal of PIET's own below.
  //  - boottest_empty_cluster: `cluster()` is the fit's clustering (p 0,
  //    clustvars id); it was compared as an empty list and refused.
  //  - boottest_ctl_cluster_id2: `cluster(id2)` after a fit clustered on id is
  //    rc 0, "Overriding estimator's cluster/robust settings with cluster(id2)",
  //    clustvars id2. It was refused as a mismatch; the engine's wildboot refits
  //    on --cluster, so the override is forwarded and said in the block's note.
  //  - boottest_opt_c2: `cluster(id id2)` is a two-way bootstrap (rc 0, p .).
  //    The engine's wild bootstrap is one-way (src/inference/wildboot.cpp reads
  //    md.cluster.at(cluster_index), index 0): refused by name, never truncated
  //    to the first variable -- and so is a two-way FIT with no cluster(),
  //    which used to be forwarded as `--cluster a,b` and bootstrapped on `a`.
  // FU-238 (FU-168): r() is emptied through boottestMessageM4b (replaceRClass);
  // the triple clear this used wiped rStrings whole, s() included.
  const VceShapesM4b vceShape = readVceShapesM4b(parsed);
  if (vceShape.cluster == OptShapeM4b::kBare ||
      (vceShape.robust != OptShapeM4b::kAbsent && vceShape.robust != OptShapeM4b::kBare)) {
    return boottestMessageM4b(session, vceShape.cluster == OptShapeM4b::kBare
                                           ? std::string("Option cluster not allowed.")
                                           : "Option robust(" + vceShape.robustRaw +
                                                 ") not allowed.");
  }

  // boottype() (papers 053 and 086 write `boottype(wild)`; DIAGNOSIS Q5). The
  // ado reads it with `syntax, [Wild SCore]` (ado): `wild` is the
  // default after a non-ML fit, so there it changes nothing (w2t C12 :987/:993
  // equal C1 :340/:346); after a Maximum-Likelihood fit (ado) the ado prints
  // its message at rc 0, after the cluster override line it printed at ado
  // (F6 :2377-2378). `score` asks for the score bootstrap, which the engine does
  // not have, and any other value has no captured line: both refused by name.
  bool boottypeWild = false;
  for (const auto& [k, v] : parsed.opts) {
    if (k != "boottype") continue;
    const std::string bt = v.isBool ? std::string() : lowerM4(trimM4(v.strValue));
    if (!scoreModel.empty() && !bt.empty() && std::string("score").compare(0, bt.size(), bt) == 0) continue;
    if (!bt.empty() && std::string("wild").compare(0, bt.size(), bt) == 0) {
      boottypeWild = true;
      continue;
    }
    return engineErrorBlockM4(
        "boottest, boottype(" + (v.isBool ? std::string() : trimM4(v.strValue)) +
            "): this version of PIET runs only the wild bootstrap, boottype(wild); the score "
            "bootstrap and any other boottype() are not implemented.",
        "unsupported_option");
  }
  if (boottypeWild && boottestFitIsMlM4b(session, lm)) {
    std::string text;
    if (vceShape.cluster == OptShapeM4b::kOne || vceShape.cluster == OptShapeM4b::kMany) {
      text = "Overriding estimator's cluster/robust settings with cluster(" +
             joinSpaceM4(vceShape.clusterVars) + ")\n";
    } else if (vceShape.robust == OptShapeM4b::kBare) {
      text = "Overriding estimator's cluster/robust settings with robust\n";
    }
    return boottestMessageM4b(
        session, text + "boottype(wild) not accepted after Maximum Likelihood-based estimation");
  }
  // An absorbed ivregress is an ML fit to boottest (boottestFitIsMlM4b), so with the
  // null imposed it re-runs the command line with constraints() (ado), which
  // ivregress refuses; the ado's `cap noi` body prints both messages at rc 0 and posts
  // nothing (StataBE 19.5, BUG-1179: absorb(a b) and absorb(a) alike, r(p) and r(reps)
  // missing). 1998 is the free constraint number once the hypothesis holds 1999.
  if (lm.stataCmd == "ivregress" && !lm.fe.empty()) {
    return boottestMessageM4b(
        session,
        "\nRe-running regression with null imposed.\n\noption constraints(1998) not allowed\n"
        "Error imposing null. Perhaps ivregress does not accept the constraints(), from(), and "
        "iterate() options");
  }

  if (auto refused = postestTsKeepifRefusalM4("boottest", lm)) return *refused;
  if (lm.cluster.empty()) {
    return engineErrorBlockM4(
        "boottest requires a preceding regression with vce(cluster ...)", "unsupported_option");
  }
  if (parsed.varlist.empty()) {
    return engineErrorBlockM4("syntax_error: boottest requires a coefficient name", "syntax_error");
  }
  // IV: the WRE (boottestIvWreM4b) rebuilds the IV model itself, so the OLS
  // refit guard does not apply to it; any other IV fit keeps that guard's refusal.
  const bool ivWre = scoreModel.empty() && boottestIvWreM4b(lm);
  if (ivWre && !lm.fitWeight.empty()) {
    return engineErrorBlockM4(
        "boottest after a weighted `" + lm.stataCmd + "` fit is not supported in this version "
        "of PIET: the engine's wild restricted efficient bootstrap takes no weights.",
        "unsupported_option");
  }
  {
    ojson reject;
    if (scoreModel.empty() && !ivWre && !boottestRefitMatchesFitM4(lm, reject)) return reject;
  }
  // The null: `boottest x1 = 0.0625` is the hypothesis x1 = 0.0625 (w2t C10 :881
  // r(t) -.2359095793768099, C11 :937 the same written `x1=0.0625`). This read
  // parsed.varlist[0] alone, so the spaced form tested x1 = 0 at rc 0 (C1's
  // t 1.001001389919719) and the compact one reached the engine as the
  // coefficient name `x1=0.0625` (rc 1).
  std::string param;
  std::optional<std::string> nullValue;
  if (!parseBoottestHypothesisM4b(parsed, param, nullValue)) {
    return engineErrorBlockM4(
        "boottest hypothesis `" + trimM4(parsed.argText) +
            "`: this version of PIET tests one coefficient against a number (`boottest x1` or "
            "`boottest x1 = 0.5`). A hypothesis with more than one term, a {...} or (...) "
            "constraint list, a linear combination or a right-hand side that is not a number "
            "is not implemented; refused rather than tested as a different hypothesis.",
        "unsupported_option");
  }
  for (const auto& [k, v] : parsed.opts) {
    if (k == "null" && nullValue.has_value()) {
      return engineErrorBlockM4(
          "boottest: the null is given twice, as `= " + *nullValue + "` and as null(); give it once.",
          "unsupported_option");
    }
  }
  // The hypothesis as typed: boottest prints it under the report's title and names
  // r(b)'s column, r(V)'s row and column and r(CI)'s row by it (boottest,
  // :997 -- the constraint's own text). StataBE 19.5, BUG-1491: `boottest x1 = 2`
  // names them `x1 = 2`, `x1=2` and `x1  =  2` keep their spacing, plain `x1` is `x1`.
  std::string h0text = trimM4(parsed.argText);
  if (h0text.empty()) h0text = joinSpaceM4(parsed.varlist);
  // A two-way fit records its clusters as ONE comma-joined element (runRegress
  // pushes the `--cluster id,id2` argv value), so the fit's list is split here
  // before it is counted.
  std::vector<std::string> bootClusters;
  for (const auto& c : lm.cluster) {
    std::string spaced = c;
    for (auto& ch : spaced) {
      if (ch == ',') ch = ' ';
    }
    for (const auto& t : splitTokensM4(spaced)) bootClusters.push_back(t);
  }
  const std::vector<std::string> fitClusters = bootClusters;
  if (vceShape.cluster == OptShapeM4b::kOne || vceShape.cluster == OptShapeM4b::kMany) {
    bootClusters = vceShape.clusterVars;
  } else if (splitTokensM4(session.eval.eStrings.get("clustvar", std::string())).size() >
             fitClusters.size()) {
    // ivreghdfe with three or more cluster() names fits on the first two but lists
    // them all in e(clustvar) (BUG-1189), which is what boottest bootstraps over.
    return engineErrorBlockM4(
        "boottest after a fit whose e(clustvar) lists more cluster variables than it was "
        "fitted on (" + session.eval.eStrings.get("clustvar", std::string()) +
            ") is not implemented in this version of PIET: boottest would bootstrap over "
            "all of them. Name the bootstrap clustering in cluster().",
        "unsupported_option");
  }
  if (!scoreModel.empty() && (fitClusters.size() != 1 || bootClusters != fitClusters)) {
    return engineErrorBlockM4("boottest score bootstrap requires the same single error and bootstrap cluster variable", "unsupported_option");
  }
  if (bootClusters.empty()) {
    return engineErrorBlockM4(
        "boottest requires a preceding regression with vce(cluster ...)", "unsupported_option");
  }
  // boottest_opt_c2 (w2l_vce_shape_det.log:4142-4159): `cluster(id id2)` after
  // regress is a multiway bootstrap: the error variance sums every non-empty
  // intersection of the clusterings with parity signs and the bootstrap
  // clusters are their full intersection (boottest, boottest.mata
  // Init) -- the engine's direct-refit kernel (wildboot --cluster a,b).
  // Served for unweighted, unabsorbed least squares and for the IV WRE; an
  // absorbed or weighted multiway bootstrap stays refused, never truncated to
  // the first variable.
  const bool multiway = bootClusters.size() > 1;
  if (multiway && !ivWre &&
      (!scoreModel.empty() || !lm.fe.empty() || !lm.fitWeight.empty())) {
    return engineErrorBlockM4(
        "boottest over more than one cluster variable (" + joinSpaceM4(bootClusters) +
            ") after an absorbed, weighted or maximum-likelihood fit is not implemented in this "
            "version of PIET: the engine's multiway wild bootstrap refits unweighted, "
            "unabsorbed least squares only. Name one variable in cluster().",
        "unsupported_option");
  }
  const bool direct = ivWre || multiway;

  // BUG-163: wildboot has no --keepif on its CLI, so the fitted sample must
  // arrive as a FILE. fitDataPath already is one (runRegress's materialized
  // if/in filter); a --keepif-style fit is materialized here through the same
  // applyIfAndInFilter every other filtered handler uses.
  std::string bootData = lm.fitDataPath.empty() ? lm.dataPath : lm.fitDataPath;
  if (!lm.fitKeepif.empty()) {
    try {
      auto bfr = piet::handlers::applyIfAndInFilter(session, bootData, lm.fitKeepif, std::nullopt);
      if (bfr.ok) bootData = bfr.dataPath;
    } catch (const CobraError& ce) {
      return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
    }
  }
  // Direct-refit settings (boottest.ado): small = e(df_r) != . (ado) --
  // ivregress posts it under `small` (e(small) "small"), regress always, and
  // ivreghdfe always (t(29) at w2t_boottest_family.log:1189) though PIET's
  // ivreghdfe does not post e(df_r) itself; FEdfadj (ado) is 0 after
  // regress/ivregress and max(1, e(df_a)) after ivreghdfe.
  bool directSmall = false;
  double feDofAdj = 0.0;
  bool directNoCons = false;
  if (direct) {
    if (!ivWre) {
      directSmall = true;
      directNoCons = boottestFitLacksConstantM4(lm);
    } else if (lm.stataCmd == "ivregress") {
      directSmall = session.eval.eStrings.get("small", std::string()) == "small";
      if (lm.result.has_value() && lm.result->is_object()) {
        bool hasCons = false;
        const char* ck = lm.result->contains("coeftable") ? "coeftable" : "coefs";
        if (lm.result->contains(ck) && (*lm.result)[ck].is_array())
          for (const auto& row : (*lm.result)[ck])
            if (row.is_object() && (row.value("name", std::string()) == "_cons" ||
                                    row.value("name", std::string()) == "(intercept)"))
              hasCons = true;
        if (lm.result->contains("coef") && (*lm.result)["coef"].is_object() &&
            (*lm.result)["coef"].contains("(intercept)"))
          hasCons = true;
        directNoCons = !hasCons;
      }
    } else {
      // ivreghdfe (boottest.ado, the ado's own reading of the fit): `cmd` stays
      // "ivreghdfe" (ado, e(extended_absvars) is set), so the df_a locals
      // that only the reghdfe branch fills (ado) are empty and
      // NFE = max(0e(K1), 0df_a, 0df_a_initial) = 0 (ado): no fixed
      // effect is passed to the bootstrap. e(b) has no _cons and
      // e(partialcons) is not 1, so `cons & !NFE` (ado) leaves the
      // model without a constant. Yet FEdfadj = max(1, e(df_a)) (ado) still
      // enters the dof. So boottest bootstraps the unabsorbed, constant-free 2SLS
      // with the small-sample factor (N-1)/(N-k-df_a) and G/(G-1): Stata's r(t)
      // 2.286824317666255 (D3_ivreghdfe_a1_st :1203) is that model's
      // (`ivregress 2sls yw x1 (endog=z), nocons vce(cluster id) small` t
      // 2.326368879167318 rescaled by sqrt(172/178)). This is boottest's
      // behaviour, mirrored, not a statistical choice. e(df_a) is reghdfe's
      // count for the one absorbed set: its levels less those nested in a
      // cluster variable (the fit reply's `absorbed` / `nested_fe`).
      directSmall = true;
      directNoCons = true;
      double dfA = 0.0;
      if (lm.result.has_value() && lm.result->is_object() && lm.result->contains("absorbed") &&
          (*lm.result)["absorbed"].is_object() && (*lm.result)["absorbed"].size() == 1) {
        const auto& ab = (*lm.result)["absorbed"];
        const std::string feName = ab.begin().key();
        if (ab.begin().value().is_number()) dfA = ab.begin().value().get<double>();
        if (lm.result->contains("nested_fe") && (*lm.result)["nested_fe"].is_array())
          for (const auto& nf : (*lm.result)["nested_fe"])
            if (nf.is_string() && nf.get<std::string>() == feName) dfA = 0.0;
      } else {
        return engineErrorBlockM4(
            "boottest after `ivreghdfe`: the fit reply does not report its absorbed levels, so "
            "boottest's degrees-of-freedom adjustment e(df_a) is unknown; refused.",
            "engine_error");
      }
      feDofAdj = std::max(1.0, dfA);
    }
  }
  // BUG-1175: the printed report (boottestReportM4b), built once the reply is in. Ahead
  // of it, in boottest's order: the override line (ado, after a blank line) and,
  // for a multiway bootstrap, the assumed bootcluster() (ado; PIET takes no
  // bootcluster(), so it is always assumed). reps() is boottest's, 999 by default.
  auto emitReport = [&](const ojson& reply, bool small, std::optional<double> stat,
                        std::optional<double> dfR, const std::string& ciStr) {
    if (reportOut == nullptr) return;
    std::string pre;
    std::string clusterOpt;
    if (vceShape.cluster == OptShapeM4b::kOne || vceShape.cluster == OptShapeM4b::kMany) {
      clusterOpt = joinSpaceM4(vceShape.clusterVars);
      pre += "\nOverriding estimator's cluster/robust settings with cluster(" + clusterOpt + ")\n";
    }
    if (bootClusters.size() > 1)
      pre += "(bootcluster(" + joinSpaceM4(bootClusters) + ") assumed)\n";
    long long repsAsked = 999;
    for (const auto& [k, v] : parsed.opts) {
      if (k != "reps" || v.isBool) continue;
      try {
        repsAsked = std::stoll(trimM4(v.strValue));
      } catch (...) {
      }
    }
    ojson rep;
    rep["type"] = "display";
    rep["text"] = boottestReportM4b(pre, repsAsked, reply, static_cast<double>(session.level),
                                    clusterOpt, bootClusters, h0text, small, stat, dfR,
                                    ciStr);
    *reportOut = rep;
  };
  std::vector<std::string> args;
  if (ivWre) {
    args = {"wildboot", "--data", bootData, "--y", lm.y, "--endog", joinCommaM4(lm.ivEndog),
            "--instruments", joinCommaM4(lm.ivInstruments), "--param", param, "--cluster",
            joinCommaM4(bootClusters), "--wre"};
    if (!lm.ivExog.empty()) {
      args.push_back("--x");
      args.push_back(joinCommaM4(lm.ivExog));
    }
    if (lm.cobraCmd == "liml") args.push_back("--liml");
  } else if (multiway) {
    args = {"wildboot", "--data", bootData, "--y", lm.y, "--x",
            lm.x.empty() ? "(intercept)" : joinCommaM4(lm.x), "--param", param, "--cluster",
            joinCommaM4(bootClusters)};
  } else {
    args = {"wildboot",
            "--data",
            bootData,
            "--y",
            lm.y,
            "--x",
            lm.x.empty() ? "(intercept)" : joinCommaM4(lm.x),
            "--param",
            param,
            "--cluster",
            bootClusters[0]};
  }
  if (direct) {
    if (directSmall) args.push_back("--small");
    if (directNoCons) args.push_back("--no-intercept");
    if (feDofAdj != 0.0) {
      args.push_back("--fe-dof-adj");
      args.push_back(std::to_string(static_cast<long long>(feDofAdj)));
    }
  }
  // The direct kernel takes no FE, weights or singleton flags (ivreghdfe's FE is
  // left out on purpose, see above).
  if (!direct && !scoreModel.empty()) {
    args.push_back("--model");
    args.push_back(scoreModel);
  }
  if (!direct && !lm.fe.empty()) {
    args.push_back("--fe");
    args.push_back(joinCommaM4(lm.fe));
  }
  if (!direct && boottestFitLacksConstantM4(lm)) args.push_back("--no-intercept");
  if (!direct && !lm.fitWeight.empty()) {
    args.push_back("--weight");
    args.push_back(lm.fitWeight);
    args.push_back("--wtype");
    args.push_back(lm.fitWtype);
  }
  if (nullValue.has_value()) {
    args.push_back("--null");
    args.push_back(*nullValue);
  }
  // FU-733: areg and xtreg,fe keep their singleton groups (LastModel::
  // keepSingletons), and cmd_wildboot resamples them too under
  // --keep-singletons (apps/cmds/cmds_linear.cpp, landed with the e(sample) T2
  // fix round), as every other refit forwards it (fitConventionFlagsM4).
  // Without it the bootstrap dropped id 11 and the guard below refused
  // w2_esample_tsrefit.log:393 W3 (xtreg,fe, 59 obs, Stata p .0720720720720721).
  if (!direct && lm.keepSingletons && !lm.fe.empty()) args.push_back("--keep-singletons");

  for (const auto& [k, v] : parsed.opts) {
    if (k == "reps" && !v.isBool) {
      args.push_back("--reps");
      args.push_back(v.strValue);
    } else if (k == "seed" && !v.isBool) {
      args.push_back("--seed");
      args.push_back(v.strValue);
    } else if (k == "weighttype" && !v.isBool) {
      // boottest's five types by their minimum abbreviations (boottest,
      // `syntax, [RADemacher MAMmen NORmal WEBb GAMma]`); the engine takes the full
      // name, which r(weighttype) posts (StataBE 19.5, BUG-1519: weight(mam) is mammen;
      // an empty weight() is the default, rademacher, ado).
      const std::string w = lowerM4(trimM4(v.strValue));
      if (w.empty()) continue;
      std::string full;
      for (const char* name : {"rademacher", "mammen", "normal", "webb", "gamma"})
        if (w.size() >= 3 && std::string(name).compare(0, w.size(), w) == 0) full = name;
      if (full.empty()) {
        return engineErrorBlockM4("weighttype() must be rademacher, mammen, normal, webb or gamma",
                                  "unsupported_option");
      }
      args.push_back("--weights");
      args.push_back(full);
    } else if (k == "null" && !v.isBool) {
      args.push_back("--null");
      args.push_back(v.strValue);
    }
    // cluster(): read by shape above (bootClusters). W:P5 refused a cluster()
    // that differed from the fit's on the premise that it "would silently
    // bootstrap over clusters the model never used"; boottest itself overrides
    // the fit and says so (boottest_ctl_cluster_id2), which the note below
    // carries. nograph: nothing to suppress (vocabulary note at file top).
  }
  // reps() is boottest's, 999 by default (boottest `Reps(integer 999)`); the
  // engine's own default is 9999 (StataBE 19.5, BUG-1546: 30
  // clusters, `boottest x1 = 2, seed(1)` r(reps) 999, r(p) .7977977977977978, where
  // PIET drew 9999 and answered .8159).
  if (std::none_of(parsed.opts.begin(), parsed.opts.end(),
                   [](const auto& kv) { return kv.first == "reps" && !kv.second.isBool; })) {
    args.push_back("--reps");
    args.push_back("999");
  }
  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kKnown = kVocabBoottest.known;
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kKnown)) {
    // boottest runs its body under `cap noi`: an option its syntax does not
    // declare prints "Option weights(webb) not allowed.", runs no test, leaves
    // r() empty and returns rc 0 (StataBE, BUG-1173). Options this build knows
    // of but has not implemented keep their named refusal.
    for (const auto& [k, v] : parsed.opts) {
      if (kKnown.count(k) || kVocabBoottest.unimplemented.count(k)) continue;
      eval::replaceRClass(session.eval);
      eval::declareRPosted(session.eval, "boottest", {});  // r() is known-empty: r(p) reads `.`
      ojson note;
      note["type"] = "display";
      note["text"] = "Option " + k + (v.isBool ? std::string() : "(" + v.strValue + ")") +
                     " not allowed.";
      return note;
    }
    return *err;
  }

  // Stata's draws (core/rng_mt64.h): boottest runs
  // `if "`seed'"=="" local seed = c(seed)` then `set seed `seed'`, and c(seed)
  // is the current state string, so seed(#) restarts the session stream at #
  // and no seed() continues it. boottest.mata then draws the Rademacher weights
  // from that state. The engine draws them the same way from the
  // state sent here and reports how many it took; the session stream then
  // stands where Stata's would (and seed(#) is now c(seed), as after `set seed`).
  std::optional<long long> bootSeed;
  for (const auto& [k, v] : parsed.opts) {
    if (k != "seed" || v.isBool) continue;
    try {
      size_t pos = 0;
      const std::string t = trimM4(v.strValue);
      const long long sv = std::stoll(t, &pos);
      if (pos == t.size()) bootSeed = sv;
    } catch (...) {
    }
  }
  const cecon::Mt64 bootStream =
      bootSeed.has_value() ? cecon::Mt64(static_cast<uint64_t>(*bootSeed)) : session.seed.stream();
  args.push_back("--rng-mt64");
  args.push_back(bootStream.state_string());
  args.push_back("--tidy");
  ojson raw;
  try {
    raw = runCobraJsonM4(args);
  } catch (const CobraError& ce) {
    return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
  }
  if (bootSeed.has_value()) session.seed = *bootSeed;
  if (raw.is_object() && raw.contains("rng_draws") && raw["rng_draws"].is_number_integer() &&
      raw["rng_draws"].get<long long>() > 0)
    session.seed.stream().discard(static_cast<uint64_t>(raw["rng_draws"].get<long long>()));
  // e(sample) T2: areg and xtreg,fe keep their singleton groups (LastModel::
  // keepSingletons; w2_esample.log:925 D4 areg e(N) 59). The engine's wild
  // bootstrap drops them unless told otherwise (--keep-singletons above), and
  // `areg y x1 x2, absorb(id) vce(cluster id)` (e(N) 59, 11 clusters) then
  // `boottest x1` answered n_obs 58 / 10 clusters at rc 0, the numbers of the
  // fit without id 11. Still refused by name whenever the bootstrap's sample is
  // not the fit's.
  if (!direct && lm.keepSingletons && !lm.fe.empty() && lm.result.has_value() &&
      lm.result->is_object() && lm.result->contains("n_obs") &&
      (*lm.result)["n_obs"].is_number() && raw.is_object() && raw.contains("n_obs") &&
      raw["n_obs"].is_number() &&
      raw["n_obs"].get<double>() != (*lm.result)["n_obs"].get<double>()) {
    return engineErrorBlockM4(
        "boottest after `" + (lm.stataCmd.empty() ? lm.cobraCmd : lm.stataCmd) +
            "` with a singleton group is not supported in this version of PIET -- the "
            "engine's wild bootstrap drops singleton groups, which this fit keeps, so it "
            "would bootstrap " + std::to_string(raw["n_obs"].get<long long>()) +
            " observations instead of the fit's " +
            std::to_string((*lm.result)["n_obs"].get<long long>()) +
            ". Refusing rather than reporting a number that is not the one asked for.",
        "unsupported_option");
  }
  if (direct) {
    // boottest's r() after the WRE / multiway bootstrap (ado):
    // r(t), r(F) = t^2 and r(df_r) under `small`, r(z) and r(chi2) = z^2
    // otherwise (D1_st :1053 z .1434202311956093 with t and df_r `.`; D3_st :1203
    // t 2.286824317666255, df_r 29); r(df_r) is the smallest error-clustering
    // count less one (boottest: boottest_opt_c2 df_r 6 with 30 and 7).
    // r(N_clust) is not posted (`.`). b and V as for the one-way path: V = se^2 *
    // (N-1)/_df (getV, boottest; _df/(N-1) is 1 without small).
    const ojson* fitJson =
        lm.result.has_value() && lm.result->is_object() ? &*lm.result : nullptr;
    const bool sameSample = fitJson != nullptr && fitJson->contains("n_obs") &&
                            (*fitJson)["n_obs"].is_number() && raw.contains("n_obs") &&
                            raw["n_obs"].is_number() &&
                            (*fitJson)["n_obs"].get<double>() == raw["n_obs"].get<double>();
    if (!sameSample) {
      return engineErrorBlockM4(
          "boottest after `" + (lm.stataCmd.empty() ? lm.cobraCmd : lm.stataCmd) +
              "`: the bootstrap's sample is not the fit's (singleton or missing-value rows "
              "differ); refused rather than bootstrapping a different sample.",
          "unsupported_option");
    }
    ojson block = cobraToEstimationM4(raw, "boottest", &session);
    std::string note;
    if (multiway && vceShape.cluster == OptShapeM4b::kMany)
      note = "(bootcluster(" + joinSpaceM4(bootClusters) + ") assumed)";
    if (vceShape.cluster == OptShapeM4b::kOne || vceShape.cluster == OptShapeM4b::kMany) {
      const std::string text = "Overriding estimator's cluster/robust settings with cluster(" +
                               joinSpaceM4(bootClusters) + ")";
      note = note.empty() ? text : (note + "  " + text);
    }
    if (!note.empty()) {
      const std::string cur = block.contains("note") && block["note"].is_string()
                                  ? block["note"].get<std::string>()
                                  : std::string();
      block["note"] = cur.empty() ? note : (cur + "  " + note);
    }
    block["statistic"] = directSmall ? "t" : "z";
    eval::replaceRClass(session.eval);
    const bool haveT = raw.contains("t0") && raw["t0"].is_number();
    if (haveT) {
      const double t = raw["t0"].get<double>();
      session.eval.rValues.set(directSmall ? "t" : "z", t);
      session.eval.rValues.set(directSmall ? "F" : "chi2", t * t);
      if (!directSmall) block["z"] = t;
    }
    if (directSmall && raw.contains("df_r") && raw["df_r"].is_number())
      session.eval.rValues.set("df_r", raw["df_r"].get<double>());
    if (raw.contains("p_symmetric") && raw["p_symmetric"].is_number())
      session.eval.rValues.set("p", raw["p_symmetric"].get<double>());
    if (haveT && raw.contains("se") && raw["se"].is_number() && raw.contains("beta") &&
        raw["beta"].is_number() && raw.contains("null_beta") && raw["null_beta"].is_number()) {
      const double nObs = raw["n_obs"].get<double>();
      const double kModel = static_cast<double>(directNoCons ? 0 : 1) +
                            static_cast<double>(ivWre ? lm.ivExog.size() + lm.ivEndog.size()
                                                      : lm.x.size());
      const double dfres = nObs - kModel - feDofAdj;
      const double se = raw["se"].get<double>();
      if (!directSmall || dfres > 0) {
        eval::EvalMatrix rb;
        rb.rownames = {"r1"};
        rb.colnames = {h0text};
        rb.data.push_back({raw["beta"].get<double>() - raw["null_beta"].get<double>()});
        eval::EvalMatrix rv;
        rv.rownames = {h0text};
        rv.colnames = {h0text};
        rv.data.push_back({directSmall ? se * se * (nObs - 1.0) / dfres : se * se});
        session.eval.rMatrices.emplace_back("V", std::move(rv));
        session.eval.rMatrices.emplace_back("b", std::move(rb));
      }
    }
    session.eval.rValues.set("df", 1.0);
    if (raw.contains("reps") && raw["reps"].is_number())
      session.eval.rValues.set("reps", raw["reps"].get<double>());
    session.eval.rValues.set("NH0s", 1.0);
    session.eval.rValues.set("null", 1.0);
    session.eval.rValues.set("ptol", 0.001);
    session.eval.rValues.set("level", static_cast<double>(session.level));
    session.eval.rStrings.set("clustvars", joinSpaceM4(bootClusters));
    session.eval.rStrings.set("boottype", "wild");
    if (raw.contains("weights") && raw["weights"].is_string())
      session.eval.rStrings.set("weighttype", raw["weights"].get<std::string>());
    session.eval.rStrings.set("statistic", "t");
    session.eval.rStrings.set("robust", "robust");
    for (const auto& [k, v] : parsed.opts)
      if (k == "seed" && !v.isBool && !trimM4(v.strValue).empty())
        session.eval.rStrings.set("seed", trimM4(v.strValue));
    std::vector<std::string> posted = {"NH0s", "reps", "null", "ptol", "level", "p", "df",
                                       "clustvars", "boottype", "weighttype", "statistic",
                                       "robust", "CIstr", "seed", "CI", "plot", "V", "b"};
    if (directSmall) {
      for (const char* nm : {"df_r", "F", "t"}) posted.push_back(nm);
    } else {
      for (const char* nm : {"chi2", "z"}) posted.push_back(nm);
    }
    eval::declareRPosted(session.eval, "boottest", posted);
    emitReport(raw, directSmall,
               haveT ? std::optional<double>(raw["t0"].get<double>()) : std::nullopt,
               raw.contains("df_r") && raw["df_r"].is_number()
                   ? std::optional<double>(raw["df_r"].get<double>())
                   : std::nullopt,
               std::string());
    return block;
  }
  if (!scoreModel.empty()) {
    if (!raw.contains("n_obs") || !raw["n_obs"].is_number() || !lm.result->contains("n_obs") ||
        !(*lm.result)["n_obs"].is_number() || raw["n_obs"] != (*lm.result)["n_obs"])
      return engineErrorBlockM4("boottest score bootstrap: refit sample does not match the original model", "unsupported_option");
    if (!raw.contains("z") || !raw["z"].is_number() || !raw.contains("p_symmetric") || !raw["p_symmetric"].is_number())
      return engineErrorBlockM4("boottest score bootstrap: engine did not publish a score statistic", "engine_error");
    eval::replaceRClass(session.eval);
    const double z = raw["z"].get<double>();
    session.eval.rValues.set("z", z);
    session.eval.rValues.set("chi2", z * z);
    session.eval.rValues.set("p", raw["p_symmetric"].get<double>());
    session.eval.rValues.set("df", 1.0);
    session.eval.rValues.set("reps", raw["reps"].get<double>());
    session.eval.rValues.set("NH0s", 1.0); session.eval.rValues.set("null", 1.0);
    session.eval.rValues.set("ptol", .001); session.eval.rValues.set("level", session.level);
    session.eval.rStrings.set("clustvars", bootClusters[0]);
    session.eval.rStrings.set("boottype", "score"); session.eval.rStrings.set("statistic", "z");
    session.eval.rStrings.set("robust", "robust"); session.eval.rStrings.set("weighttype", raw["weights"].get<std::string>());
    for (const auto& [k, v] : parsed.opts) if (k == "seed" && !v.isBool) session.eval.rStrings.set("seed", trimM4(v.strValue));
    eval::declareRPosted(session.eval, "boottest", {"z", "chi2", "p", "df", "reps", "NH0s", "null", "ptol", "level",
      "clustvars", "boottype", "statistic", "robust", "weighttype", "seed", "CI", "CIstr", "plot", "b", "V"});
    ojson block = cobraToEstimationM4(raw, "boottest", &session);
    block["z"] = z;
    block["statistic"] = "z";
    return block;
  }
  ojson block = cobraToEstimationM4(raw, "boottest", &session);
  if (vceShape.cluster == OptShapeM4b::kOne) {
    const std::string text =
        "Overriding estimator's cluster/robust settings with cluster(" + bootClusters[0] + ")";
    const std::string cur = block.contains("note") && block["note"].is_string()
                                ? block["note"].get<std::string>()
                                : std::string();
    block["note"] = cur.empty() ? text : (cur + "  " + text);
  }
  // boottest is r-class: it replaces all of r() (w2t C1_r :351 `mean=. N=.`
  // after a summarize) through replaceRClass, which keeps s(); this cleared
  // rStrings whole (FU-238). The set it posts after a one-hypothesis wild
  // bootstrap of a clustered least-squares fit is the `return list` at w2t
  // :356-:381 (C1), from ado: scalars NH0s reps null ptol
  // level p df df_r F t; macros clustvars boottype weighttype statistic robust
  // CIstr seed; matrices CI plot V b. All of it is declared, so a name PIET does
  // not post reads as a named r(111) (rTableAfterCommand's rUnposted), never as
  // the `.` an undeclared name gets -- that `.` is what Stata prints only for a
  // name outside its set, r(N_clust) and r(z) after least squares (C1_st :346).
  // Posted, each as boottest.ado computes it for this case:
  //  - p: the symmetric p (ado; T5, TOLERANCE.md §3.5); reps: the engine's
  //    draw count, 2^G when it enumerates (ado, repsname);
  //  - t: the statistic at the fit's estimates (ado), F = t^2 (ado);
  //    see boottestT below for when PIET knows boottest's t;
  //  - df 1 (one constraint), df_r = G - 1 (boottest, one cluster
  //    variable; C1_st :346 df_r 29 with 30 clusters), NH0s 1, null 1 (the
  //    null is imposed: this build has no `nonull`), ptol .001 and level
  //    c(level) (the syntax defaults at ado; neither option is in this
  //    command's vocabulary);
  //  - clustvars, boottype wild, weighttype (the engine's draw type), statistic
  //    t, robust robust (ado, a cluster is set), seed (ado: the seed()
  //    text; without seed() it is c(seed), which PIET does not have).
  //  - b and V (ado, boottest): the numerator R*b - r and
  //    the denominator with boottest's small-sample factor _df/(N-1) divided
  //    OUT (getV: statDenom / smallsample^2, boottest), so
  //    V = se^2 * (N-1)/_df where t = b/se is boottest's t: W1/W3/W5_st :342/
  //    :399/:463 (areg, xtreg,fe, reghdfe) agree to 1e-15. One cell each,
  //    named by the hypothesis as typed (h0text; ado, BUG-1491).
  // Not posted, so refused: CIstr and CI (no confidence set is computed), plot,
  // seed without seed(), and t/F/b/V when boottestT has no value.
  std::optional<double> boottestT;
  std::optional<double> boottestSe;  // the se with boottestT = b / boottestSe
  std::optional<double> boottestDf;  // boottest _df
  const ojson* fitJson =
      lm.result.has_value() && lm.result->is_object() ? &*lm.result : nullptr;
  const bool sameSample = fitJson != nullptr && fitJson->contains("n_obs") &&
                          (*fitJson)["n_obs"].is_number() && raw.contains("n_obs") &&
                          raw["n_obs"].is_number() &&
                          (*fitJson)["n_obs"].get<double>() == raw["n_obs"].get<double>();
  auto fitNumber = [&](const char* key) -> std::optional<double> {
    if (fitJson == nullptr || !fitJson->contains(key) || !(*fitJson)[key].is_number()) {
      return std::nullopt;
    }
    return (*fitJson)[key].get<double>();
  };
  if (raw.contains("t0") && raw["t0"].is_number()) {
    const double t0 = raw["t0"].get<double>();
    // The engine scales t0 by cluster_ssf, the reghdfe convention: (N-1)/(N-k)
    // with k = the slopes plus every absorbed level except one per FE nested in
    // the cluster -- the fit's own cluster_dof_k when it clusters on the same
    // variable. boottest uses _df = N - kZ - FEdfadj:
    //  - regress: no FE, the same k (C0 :295, C1 :346, C7-C9 :713/:769/:825);
    //  - reghdfe: FEdfadj = max(1, e(df_a)) (ado), the engine's k when the
    //    bootstrap clusters on the fit's cluster variable (E3 :1601, E4 :1668);
    //    under another cluster() the fit's df_a and the engine's nesting can part
    //    ways, so t is left unposted;
    //  - xtreg, fe: FEdfadj = 0 without dfadj (ado; this build's xtreg
    //    takes no dfadj), so _df = N - kZ while the engine charged one more
    //    (E6_st :1792 t 1.268217344494529 against the engine's 1.2646499187370834
    //    = E4's, FU-742): t0 * sqrt((N - kZ) / (N - cluster_dof_k)), kZ the
    //    engine's slope count k_vars (runXtreg rewrites the reply's df_m to
    //    xtreg's e(df_m)), N and cluster_dof_k the fit's, on the fit's cluster.
    //  - areg (FU-732): FEdfadj = the number of absorbed groups (ado), an FE
    //    nested in the cluster included, which is the fit's own clustered dof
    //    (BUG-568, --keep-nested-fe-dof) and not cluster_ssf's: t = (b - null) / the areg
    //    table's se, on the fit's cluster (W1_st :342 t 1.694959401854210;
    //    E2_areg_id_st :1535, id nested in id).
    const bool sameCluster = lm.cluster.size() == 1 && lm.cluster[0] == bootClusters[0];
    const std::optional<double> n = sameSample ? fitNumber("n_obs") : std::nullopt;
    const std::optional<double> kEngine = fitNumber("cluster_dof_k");
    const double seEngine =
        raw.contains("se") && raw["se"].is_number() ? raw["se"].get<double>() : std::nan("");
    if (lm.fe.empty()) {
      boottestT = t0;
      if (n && kEngine && *n - *kEngine > 0) {
        boottestSe = seEngine;
        boottestDf = *n - *kEngine;
      }
    } else if (sameCluster && lm.stataCmd == "reghdfe") {
      boottestT = t0;
      if (n && kEngine && *n - *kEngine > 0) {
        boottestSe = seEngine;
        boottestDf = *n - *kEngine;
      }
    } else if (sameCluster && lm.stataCmd == "xtreg" && lm.cobraCmd == "feols") {
      const std::optional<double> kz = fitNumber("k_vars");
      if (n && kz && kEngine && *n - *kz > 0 && *n - *kEngine > 0) {
        const double scale = std::sqrt((*n - *kz) / (*n - *kEngine));
        boottestT = t0 * scale;
        boottestSe = seEngine / scale;
        boottestDf = *n - *kz;
      }
    } else if (sameCluster && lm.stataCmd == "areg" && n && kEngine && *n - *kEngine > 0 &&
               fitJson->contains("se") && (*fitJson)["se"].is_object() &&
               (*fitJson)["se"].contains(param) && (*fitJson)["se"][param].is_number() &&
               raw.contains("beta") && raw["beta"].is_number() && raw.contains("null_beta") &&
               raw["null_beta"].is_number()) {
      const double seFit = (*fitJson)["se"][param].get<double>();
      if (seFit > 0) {
        boottestT = (raw["beta"].get<double>() - raw["null_beta"].get<double>()) / seFit;
        boottestSe = seFit;
        boottestDf = *n - *kEngine;
      }
    }
  }
  // K6 (FIX-SPEC.md): the block shows the t boottest reports; where PIET does
  // not know it, the engine's t0 stays on display and r(t) refuses.
  if (boottestT.has_value()) block["t0"] = *boottestT;
  eval::replaceRClass(session.eval);
  if (raw.contains("p_symmetric") && raw["p_symmetric"].is_number()) {
    session.eval.rValues.set("p", raw["p_symmetric"].get<double>());
  }
  if (boottestT.has_value()) {
    session.eval.rValues.set("t", *boottestT);
    session.eval.rValues.set("F", *boottestT * *boottestT);
  }
  std::string ciStr;
  if (boottestT.has_value() && boottestSe.has_value() && boottestDf.has_value() &&
      std::isfinite(*boottestSe) && raw.contains("beta") && raw["beta"].is_number() &&
      raw.contains("null_beta") && raw["null_beta"].is_number() && sameSample) {
    const double nObs = raw["n_obs"].get<double>();
    const double vb = *boottestSe * *boottestSe * (nObs - 1.0) / *boottestDf;
    eval::EvalMatrix rb;
    rb.rownames = {"r1"};
    rb.colnames = {h0text};
    rb.data.push_back({raw["beta"].get<double>() - raw["null_beta"].get<double>()});
    eval::EvalMatrix rv;
    rv.rownames = {h0text};
    rv.colnames = {h0text};
    rv.data.push_back({vb});
    session.eval.rMatrices.emplace_back("V", std::move(rv));
    session.eval.rMatrices.emplace_back("b", std::move(rb));
    // BUG-1174: r(CI), r(CIstr) and r(plot), by test inversion over the null
    // (boottestCiM4b): the same bootstrap re-run with --null r, the same draws (the
    // same --rng-mt64 state; an enumerated universe is the same in any case), its
    // symmetric p read back -- on the fit's own sample, or not at all.
    if (raw.contains("reps") && raw["reps"].is_number()) {
      auto pAt = [&](double r) -> double {
        std::vector<std::string> a = args;
        char rb17[64];
        std::snprintf(rb17, sizeof(rb17), "%.17g", r);
        bool replaced = false;
        for (size_t i = 0; i + 1 < a.size(); ++i)
          if (a[i] == "--null") {
            a[i + 1] = rb17;
            replaced = true;
          }
        if (!replaced) {
          a.push_back("--null");
          a.push_back(rb17);
        }
        try {
          const ojson rr = runCobraJsonM4(a);
          if (rr.contains("p_symmetric") && rr["p_symmetric"].is_number() &&
              rr.contains("n_obs") && rr["n_obs"] == raw["n_obs"])
            return rr["p_symmetric"].get<double>();
        } catch (const CobraError&) {
        }
        return std::nan("");
      };
      if (const auto ci = boottestCiM4b(pAt, raw["beta"].get<double>(), vb,
                                        raw["null_beta"].get<double>(),
                                        static_cast<double>(session.level),
                                        raw["reps"].get<double>())) {
        eval::EvalMatrix cm;
        cm.colnames = {"lo", "hi"};
        for (const auto& [l, h] : ci->sets) {
          cm.rownames.push_back(h0text);
          cm.data.push_back({l, h});
        }
        eval::EvalMatrix pm;
        pm.colnames = {"c1", "c2"};
        for (size_t k = 0; k < ci->plotX.size(); ++k) {
          pm.rownames.push_back("r" + std::to_string(k + 1));
          pm.data.push_back({ci->plotX[k], ci->plotY[k]});
        }
        ciStr = boottestCiStrM4b(*ci);
        session.eval.rMatrices.emplace_back("CI", std::move(cm));
        session.eval.rMatrices.emplace_back("plot", std::move(pm));
        session.eval.rStrings.set("CIstr", ciStr);
      }
    }
  }
  session.eval.rValues.set("df", 1.0);
  if (raw.contains("n_clusters") && raw["n_clusters"].is_number()) {
    session.eval.rValues.set("df_r", raw["n_clusters"].get<double>() - 1.0);
  }
  if (raw.contains("reps") && raw["reps"].is_number()) {
    session.eval.rValues.set("reps", raw["reps"].get<double>());
  }
  session.eval.rValues.set("NH0s", 1.0);
  session.eval.rValues.set("null", 1.0);
  session.eval.rValues.set("ptol", 0.001);
  session.eval.rValues.set("level", static_cast<double>(session.level));
  session.eval.rStrings.set("clustvars", bootClusters[0]);
  session.eval.rStrings.set("boottype", "wild");
  if (raw.contains("weights") && raw["weights"].is_string()) {
    session.eval.rStrings.set("weighttype", raw["weights"].get<std::string>());
  }
  session.eval.rStrings.set("statistic", "t");
  session.eval.rStrings.set("robust", "robust");
  for (const auto& [k, v] : parsed.opts) {
    if (k == "seed" && !v.isBool && !trimM4(v.strValue).empty()) {
      session.eval.rStrings.set("seed", trimM4(v.strValue));
    }
  }
  eval::declareRPosted(session.eval, "boottest",
                       {"NH0s", "reps", "null", "ptol", "level", "p", "df", "df_r", "F", "t",
                        "clustvars", "boottype", "weighttype", "statistic", "robust", "CIstr",
                        "seed", "CI", "plot", "V", "b"});
  emitReport(raw, /*small=*/true, boottestT,
             raw.contains("n_clusters") && raw["n_clusters"].is_number()
                 ? std::optional<double>(raw["n_clusters"].get<double>() - 1.0)
                 : std::nullopt,
             ciStr);
  return block;
}

ojson runBoottest(const eval::ParsedCommand& parsed, SessionState& session) {
  return runBoottest(parsed, session, nullptr);
}

// ===========================================================================
// tobit (BS-069, BETA D2 batch): `tobit depvar indepvars [if], ll[(#)]
// ul[(#)] [level(#)]` -> `cobra tobit --ll/--ul`. Oracle:
// internal capture d2-oracle-stata.log
// (D2a: ul(15000), D2b: ll(11000)) — both coefficients AND the reported
// variance/uncensored/censored counts match cobra's `tobit` byte-for-byte
// (live-verified against this exact engine build, no convention gap here
// unlike D1's ivregress SE denominator). At least one of ll/ul is required
// (matches both Stata's own requirement and cobra's own usage banner: "at
// least one of --ll/--ul required").
ojson runTobit(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  if (parsed.varlist.size() < 2) {
    return engineErrorBlockM4(
        "syntax_error: tobit requires a dependent variable and at least one regressor",
        "syntax_error");
  }
  std::string y = parsed.varlist[0];
  std::vector<std::string> xs(parsed.varlist.begin() + 1, parsed.varlist.end());

  std::optional<std::string> llVal, ulVal;
  bool sawLl = false, sawUl = false;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "ll") {
      sawLl = true;
      if (!v.isBool) llVal = v.strValue;
    }
    if (k == "ul") {
      sawUl = true;
      if (!v.isBool) ulVal = v.strValue;
    }
  }
  if (!sawLl && !sawUl) {
    return engineErrorBlockM4("syntax_error: tobit requires at least one of ll()/ul()",
                              "syntax_error");
  }

  std::vector<std::string> argv = {"tobit", "--y", y, "--x", joinCommaM4(xs)};

  // Stata's BARE `ll`/`ul` (no numeric value) means "censor at the depvar's
  // own observed min/max" — cobra's tobit has no bare-flag equivalent
  // (live-verified: `--ll` with no value fails inside cobra's own arg
  // parser, "stod: no conversion", exit 1), so PIET computes the value
  // itself (computeYMinMaxM4 above) and always forwards an explicit number.
  if (sawLl) {
    std::string numStr;
    if (llVal.has_value() && !trimM4(*llVal).empty()) {
      numStr = trimM4(*llVal);
    } else {
      double mn = 0, mx = 0;
      ojson errBlock;
      if (!computeYMinMaxM4(session, y, parsed.ifExpr, mn, mx, errBlock)) return errBlock;
      char buf[64];
      std::snprintf(buf, sizeof(buf), "%.17g", mn);
      numStr = buf;
    }
    argv.push_back("--ll");
    argv.push_back(numStr);
  }
  if (sawUl) {
    std::string numStr;
    if (ulVal.has_value() && !trimM4(*ulVal).empty()) {
      numStr = trimM4(*ulVal);
    } else {
      double mn = 0, mx = 0;
      ojson errBlock;
      if (!computeYMinMaxM4(session, y, parsed.ifExpr, mn, mx, errBlock)) return errBlock;
      char buf[64];
      std::snprintf(buf, sizeof(buf), "%.17g", mx);
      numStr = buf;
    }
    argv.push_back("--ul");
    argv.push_back(numStr);
  }
  if (parsed.ifExpr.has_value()) {
    argv.push_back("--keepif");
    argv.push_back(*parsed.ifExpr);
  }

  // level() — NEVER forwarded. cobra tobit's own usage banner does NOT list
  // --level at all (unlike qreg below), and live-verified: `cobra tobit
  // HISTORICAL, kept because the reasoning still teaches: `cobra tobit --level 90` used to
  // hard-error "unrecognized option '--level' for 'tobit'" (bad_input, exit 3) with a CLEAN
  // empty stdout — not the BUG-039 masking shape — which is why PIET refused honestly here
  // rather than forwarding into a silent drop. BUG-202 gave the engine the surface, so the
  // refusal's premise expired and the arm below forwards instead (BUG-274).
  for (const auto& [k, v] : parsed.opts) {
    if (k == "level" && !v.isBool) {
      // BUG-274: this used to reject, on the premise that "the engine's tobit estimator
      // has no --level surface at all". That premise was TRUE when written and is FALSE now —
      // BUG-202 gave tobit `--level`, with the multiplier chosen per estimator rather than
      // blanket-swapped (tobit takes t(df_r) to match its own p-value dispatch). A refusal whose
      // stated reason has expired is worse than no refusal: it tells the user the capability does
      // not exist when it does. Forward it, using this file's own established idiom.
      argv.push_back("--level");
      argv.push_back(v.strValue);
      continue;
    }
  }

  // W:P5: `tobit …, vce(robust)`. The engine's tobit now carries a
  // score-based observation-level sandwich (`--vce robust`); cluster/twoway
  // do not exist there, so vcovFromOpts's other shapes stay rejected rather
  // than silently downgraded to iid.
  for (const auto& [k, v] : parsed.opts) {
    if (k != "vce" && k != "robust") continue;
    const std::string want = v.isBool ? std::string("robust") : lowerM4(v.strValue);
    if (want != "robust") {
      return engineErrorBlockM4(
          "vce(" + want + ") is not supported for `tobit` in this engine; vce(robust) is",
          "unsupported_option");
    }
    argv.push_back("--vce");
    argv.push_back("robust");
  }
  // BUG-1544: tobit takes aweights, fweights, iweights and pweights (tobit.ado's
  // gsem; the engine fits them all). A pweight implies vce(robust), the
  // buildEstimationArgvM4 rule (BUG-536; StataBE 19.5 `tobit yc x z [pw=wf],
  // ll(1.2)` e(vce) robust, _se[x] .19377991916672471).
  if (parsed.weightType.has_value() && parsed.weightVar.has_value() &&
      !parsed.weightVar->empty()) {
    const std::string wt = lowerM4(*parsed.weightType);
    const std::string wtype = (wt == "aw" || wt == "aweight")   ? "aw"
                              : (wt == "fw" || wt == "fweight") ? "fw"
                              : (wt == "iw" || wt == "iweight") ? "iw"
                              : (wt == "pw" || wt == "pweight") ? "pw"
                                                                : "";
    if (!wtype.empty()) {
      if (wtype == "pw" && std::find(argv.begin(), argv.end(), "--vce") == argv.end()) {
        argv.push_back("--vce");
        argv.push_back("robust");
      }
      argv.push_back("--weight");
      argv.push_back(*parsed.weightVar);
      argv.push_back("--wtype");
      argv.push_back(wtype);
    }
  }

  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kKnown = kVocabTobit.known;
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kKnown)) {
    return *err;
  }

  argv.push_back("--emit-v");  // BUG-1196: the fit's e(V), posted below
  argv.push_back("--data");
  argv.push_back(*session.currentHbin());
  argv.push_back("--tidy");

  ojson raw;
  try {
    raw = runCobraJsonM4(argv);
  } catch (const CobraError& ce) {
    return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
  }

  ojson block = cobraToEstimationM4(raw, "tobit", &session);
  block["depvar"] = y;  // cobra tobit's raw JSON carries no "depvar" field at all (live-verified)
  static const char* kTobitFields[] = {
      "n_left_censored", "n_right_censored", "n_uncensored", "ll_limit",
      "ul_limit",        "sigma",            "se_sigma",     "variance",
      "se_variance",
  };
  for (const char* k : kTobitFields) {
    if (raw.contains(k)) block[k] = raw[k];
  }

  LastModel lm;
  lm.cobraCmd = "tobit";  // already anticipated in runMargins's kLinearCmds (below) — dydx after
                          // tobit correctly rejects, predictive margins/(x) still coarse-OLS.
  lm.y = y;
  lm.x = xs;
  lm.depvar = y;
  captureFitFilterM4(lm, argv);
  lm.dataPath = *session.currentHbin();
  lm.result = raw;
  session.setLastModel(lm);
  session.lastEstBlock = block;
  // BUG-1196: Stata's e(b) is the depvar's equation, then the residual variance
  // (StataBE 19.5 `yc:x1 yc:x2 yc:_cons /var(e.yc)`); e(V) is the fit's, its var
  // row/column in Stata's variance metric (cmds_mle.cpp cmd_tobit --emit-v).
  if (raw.contains("e_V") && raw.contains("coefs") && raw.contains("variance") &&
      raw["variance"].is_number()) {
    std::vector<AncillaryEbColM4> cols;
    if (depvarEqColsM4(y, raw["coefs"], "b", cols)) {
      const std::string var = "/var(e." + y + ")";
      cols.push_back({var, var, raw["variance"].get<double>()});
      (void)postAncillaryEbM4(session, raw, cols);
    }
  }
  return block;
}

// ===========================================================================
// heckman (BS-070, BETA D2 batch): `heckman depvar indepvars [if], select
// (selvar = selvars) [twostep]` -> `cobra heckman --select "S=Z1,Z2"`.
// Oracle: d2-oracle-stata.log D2c (twostep) / D2d (Stata's own default MLE,
// for comparison only — NOT wired, see below).
//
// Stata's DEFAULT (no `twostep`) fits full MLE; cobra's heckman usage
// banner says it implements ONLY "two-step Heckit (probit selection +
// inverse-Mills correction)" — live-verified further and more subtly: a
// BARE `cobra heckman` call (no --twostep flag at all) ALREADY computes the
// two-step result (matches oracle D2c exactly: beertax b=-606.9695,
// se=338.0717, lambda=-2436.094/se=12262.59, rho=-0.87975, sigma=2769.076,
// all byte-for-byte), while EXPLICITLY passing the CLI's own documented
// `--twostep` flag is actively HARMFUL: it hard-errors "unrecognized option
// '--twostep' for 'heckman'" on stderr (exit 3) while STILL printing the
// exact same full, valid result JSON to stdout first — the identical
// cobra_runner.cpp stdout/exit-code masking shape the D1 report already
// flagged for ivregress/reg3's --level/--no-intercept. So this NEVER sends
// --twostep: Stata's `twostep` maps to the engine's only-ever-computed
// default, and Stata's bare-MLE form has no engine equivalent — honest
// reject rather than silently answering an MLE request with two-step
// numbers (gate ①).
ojson runHeckman(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  if (parsed.varlist.size() < 2) {
    return engineErrorBlockM4(
        "syntax_error: heckman requires a dependent variable and at least one regressor",
        "syntax_error");
  }
  std::string y = parsed.varlist[0];
  std::vector<std::string> xs(parsed.varlist.begin() + 1, parsed.varlist.end());

  std::optional<std::string> selectRaw;
  bool sawTwostep = false;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "select" && !v.isBool) selectRaw = v.strValue;
    if (k == "twostep" && v.isBool && v.boolValue) sawTwostep = true;
  }
  if (!selectRaw.has_value() || trimM4(*selectRaw).empty()) {
    return engineErrorBlockM4("syntax_error: heckman requires select(depvar = varlist)",
                              "syntax_error");
  }
  size_t eq = selectRaw->find('=');
  if (eq == std::string::npos) {
    return engineErrorBlockM4("syntax_error: malformed select(): missing '='", "syntax_error");
  }
  if (selectRaw->find('=', eq + 1) != std::string::npos) {
    return engineErrorBlockM4("syntax_error: malformed select(): only one '=' is allowed",
                              "syntax_error");
  }
  std::vector<std::string> selDep = splitTokensM4(selectRaw->substr(0, eq));
  std::vector<std::string> selVars = splitTokensM4(selectRaw->substr(eq + 1));
  if (selDep.size() != 1 || selVars.empty()) {
    return engineErrorBlockM4(
        "syntax_error: malformed select(): need exactly one selection-equation dependent "
        "variable and at least one identifying variable",
        "syntax_error");
  }

  if (!sawTwostep) {
    return engineErrorBlockM4(
        "Full maximum-likelihood `heckman` (Stata's default when `twostep` is omitted) is not "
        "supported by this version of PIET — the engine's heckman estimator only implements the "
        "two-step Heckit method. Use `heckman ..., select(...) twostep`.",
        "unsupported_option");
  }

  std::vector<std::string> argv = {"heckman", "--y", y, "--x", joinCommaM4(xs), "--select",
                                    selDep[0] + "=" + joinCommaM4(selVars)};
  // Deliberately no --twostep (see the function-header comment: the flag
  // itself triggers a cobra CLI masking bug; the engine's bare default
  // already computes the two-step estimate the oracle pins).
  if (parsed.ifExpr.has_value()) {
    argv.push_back("--keepif");
    argv.push_back(*parsed.ifExpr);
  }

  // BUG-274: `level()` used to fall through to the generic reject net below
  // ("option `level` not allowed" — `level` was simply absent from kKnown).
  // BUG-202 gave cobra's heckman a real `--level` (ci_lo/ci_hi on BOTH the
  // outcome coeftable and select_coefs, asymptotic z — live-verified on this
  // tree's engine), so it is now in kKnown and forwards, same idiom as
  // runTobit/runNbreg. The engine validates the range itself ([10, 99.99]).
  for (const auto& [k, v] : parsed.opts) {
    if (k == "level" && !v.isBool) {
      argv.push_back("--level");
      argv.push_back(v.strValue);
      continue;
    }
  }

  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kKnown = kVocabHeckman.known;
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

  // Stata's scalar names, from the values this fit actually computed.
  raw["N_selected"] = raw.at("n_selected");
  raw["N_nonselected"] = raw.at("n_obs").get<double>() - raw.at("n_selected").get<double>();
  raw["selambda"] = raw.at("se_lambda");
  if (raw.contains("converged") && raw["converged"].is_boolean())
    raw["fconverged"] = raw["converged"].get<bool>() ? 1.0 : 0.0;
  ojson block = cobraToEstimationM4(raw, "heckman", &session);
  block["depvar"] = y;  // cobra heckman's raw JSON carries no "depvar" field at all (live-verified)
  block["estimator"] = "heckman";  // Stata's own display name is "heckman" regardless of twostep
  static const char* kHeckmanFields[] = {
      "lambda", "se_lambda", "rho", "sigma", "n_selected", "select_coefs",
  };
  for (const char* k : kHeckmanFields) {
    if (raw.contains(k)) block[k] = raw[k];
  }
  // Multi-equation shape (outcome + selection): reuse the SAME
  // {depvar, coeftable, r2, r2_within, dof_resid}-per-entry "outcomes" array
  // shape D1's reg3 adapter already established (cobraToEstimationM4's
  // "equations"->"outcomes" remap above), so any future "iterate
  // block.outcomes[]" frontend renderer picks up heckman's two equations
  // for free. cobra's select_coefs rows carry name/b/se/t always, and — since
  // BUG-202 gave heckman `--level` — ci_lo/ci_hi too WHEN level() was
  // forwarded (live-verified both ways on this tree's engine: absent without
  // --level, populated with it). Carry what the engine sent; p is genuinely
  // never emitted on select rows and stays null, not fabricated (BUG-274
  // un-staled the old "no p/CI ever" wording here).
  if (raw.contains("select_coefs") && raw["select_coefs"].is_array()) {
    ojson selCt = ojson::array();
    for (const auto& c : raw["select_coefs"]) {
      ojson r;
      r["name"] = c.value("name", std::string());
      r["b"] = c.contains("b") ? c["b"] : ojson(nullptr);
      r["se"] = c.contains("se") ? c["se"] : ojson(nullptr);
      r["t"] = c.contains("t") ? c["t"] : ojson(nullptr);
      r["p"] = ojson(nullptr);
      r["ci_lo"] = c.contains("ci_lo") ? c["ci_lo"] : ojson(nullptr);
      r["ci_hi"] = c.contains("ci_hi") ? c["ci_hi"] : ojson(nullptr);
      selCt.push_back(r);
    }
    ojson outcomeEq;
    outcomeEq["depvar"] = y;
    outcomeEq["coeftable"] = block.value("coeftable", ojson::array());
    outcomeEq["r2"] = ojson(nullptr);
    outcomeEq["r2_within"] = ojson(nullptr);
    outcomeEq["dof_resid"] = ojson(nullptr);
    ojson selectEq;
    selectEq["depvar"] = selDep[0];
    selectEq["coeftable"] = selCt;
    selectEq["r2"] = ojson(nullptr);
    selectEq["r2_within"] = ojson(nullptr);
    selectEq["dof_resid"] = ojson(nullptr);
    ojson outcomes = ojson::array();
    outcomes.push_back(outcomeEq);
    outcomes.push_back(selectEq);
    block["outcomes"] = outcomes;
  }

  LastModel lm;
  lm.cobraCmd = "heckman";  // added to runMargins's kLinearCmds below — heckman's outcome
                            // equation is a plain linear fit, so dydx is equally meaningless.
  lm.y = y;
  lm.x = xs;
  lm.depvar = y;
  captureFitFilterM4(lm, argv);
  lm.dataPath = *session.currentHbin();
  lm.result = raw;
  session.setLastModel(lm);
  session.lastEstBlock = block;
  // Both fitted equations and the Mills coefficient are part of e(b), even
  // though the console presents the equations separately. Preserve their
  // actual SEs for qualified _se[]; a full cross-equation e(V) is not invented.
  ojson full = ojson::array();
  auto appendEq = [&full](const ojson& rows, const std::string& eq) {
    for (const auto& c : rows) {
      ojson row = c;
      std::string name = row.value("name", std::string());
      if (name == "(intercept)") name = "_cons";
      row["name"] = eq + ":" + name;
      full.push_back(std::move(row));
    }
  };
  appendEq(block.value("coeftable", ojson::array()), y);
  appendEq(raw.value("select_coefs", ojson::array()), selDep[0]);
  full.push_back(ojson{{"name", "/mills:lambda"}, {"b", raw["lambda"]},
                       {"se", raw["se_lambda"]}});
  eval::EvalMatrix eb;
  eb.rownames = {"y1"};
  eb.data.emplace_back();
  for (const auto& row : full) {
    eb.colnames.push_back(row["name"].get<std::string>());
    eb.data[0].push_back(row["b"].get<double>());
  }
  (*session.lastEstBlock)["coeftable"] = std::move(full);
  session.eval.eMatrices.emplace_back("b", std::move(eb));
  return block;
}

// ===========================================================================
// qreg (BS-071, BETA D2 batch): `qreg depvar indepvars [if] [, quantile(#)
// level(#)]` -> `cobra qreg --tau --level`. Oracle: d2-oracle-stata.log D2e
// (median, no quantile() at all — cobra's own --tau default is 0.5,
// matching Stata's) / D2f (quantile(.25)).
//
// Stata's quantile() accepts EITHER a bare fraction in (0,1) exclusive
// (`.25`/`0.25`) OR a percentage in (1,100) exclusive (`25`) — normalized
// here to cobra's own `--tau` fraction convention (values > 1 are treated
// as a percentage). Unlike tobit/ivregress/reg3's `level()` (never
// forwarded — see above), qreg's own usage banner DOES list `--level`, and
// live-verified it genuinely changes the returned CI (and even the SE/t/p —
// a real per-level rank-based interval, not a cosmetic no-op: `--level 90`
// vs `--level 95` on the same fit produced different se/t/p, not just a
// wider/narrower ci_lo/ci_hi off a fixed se) — so `level()` DOES forward here.
ojson runQreg(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  if (parsed.varlist.size() < 2) {
    return engineErrorBlockM4(
        "syntax_error: qreg requires a dependent variable and at least one regressor",
        "syntax_error");
  }
  std::string y = parsed.varlist[0];
  std::vector<std::string> xs(parsed.varlist.begin() + 1, parsed.varlist.end());

  std::vector<std::string> argv = {"qreg", "--y", y, "--x", joinCommaM4(xs)};

  for (const auto& [k, v] : parsed.opts) {
    if (k == "quantile" && !v.isBool) {
      double q = 0;
      try {
        q = std::stod(trimM4(v.strValue));
      } catch (...) {
        return engineErrorBlockM4("syntax_error: quantile() must be numeric", "syntax_error");
      }
      if (q > 1.0) q /= 100.0;  // Stata allows either a (0,1) fraction or a (1,100) percentage
      if (!(q > 0.0 && q < 1.0)) {
        return engineErrorBlockM4(
            "syntax_error: quantile() must be strictly between 0 and 1 (or 0 and 100 as a "
            "percentage)",
            "syntax_error");
      }
      char buf[64];
      std::snprintf(buf, sizeof(buf), "%.17g", q);
      argv.push_back("--tau");
      argv.push_back(buf);
    }
  }
  if (parsed.ifExpr.has_value()) {
    argv.push_back("--keepif");
    argv.push_back(*parsed.ifExpr);
  }
  for (const auto& [k, v] : parsed.opts) {
    if (k == "level" && !v.isBool) {
      argv.push_back("--level");
      argv.push_back(v.strValue);
    }
  }

  // w2b-estim-vce-options: vce(robust) and [pw=] -> `cobra qreg --vce robust
  // [--weight W --wtype pw]` (qreg_fit_robust; w2b_estim.log:818
  // qreg_vce_robust, :791 qreg_pw_vce_robust `robust`). vce(iid) is the
  // default and forwards nothing. Any other vce() -- Stata's own denmethod/
  // bwidth suboptions, `vce(robust, kernel)` etc. -- is refused: only the
  // default fitted/hsheather pair is implemented and oracle-pinned.
  bool qregRobust = false;
  bool qregIid = false;  // vce(iid) spelled out: r(404) beside a pweight, below
  for (const auto& [k, v] : parsed.opts) {
    if (k != "vce") continue;
    const std::string lv = v.isBool ? std::string() : lowerM4(trimM4(v.strValue));
    if (lv == "robust") {
      qregRobust = true;
    } else if (lv == "iid") {
      qregIid = true;
    } else {
      return engineErrorBlockM4(
          "Option `vce(" + (v.isBool ? std::string() : v.strValue) +
              ")` is not implemented for `qreg` in this version of PIET; vce(iid) and "
              "vce(robust) are.",
          "unsupported_option");
    }
  }
  // FU-106 (w2p_estim_ev.log section A): Stata's qreg treats a pweight as
  // implying vce(robust). `qreg y x1 x2 [pw=w], quantile(q)` WITHOUT vce() runs
  // at rc 0 as vce(robust) (A_qreg_pw_q50_novce :299 == A_qreg_pw_q50_robust
  // :364, e(vce) robust, e(vcetype) Robust), so the pweight sets the robust
  // path here, before the --vce argv is built; the forwarded argv is then the
  // explicit vce(robust) one. An explicit vce(iid) is r(404) (:405, :411).
  const bool qregWeighted = parsed.weightType.has_value() && parsed.weightVar.has_value() &&
                            !parsed.weightVar->empty();
  if (qregWeighted) {
    const std::string wt = lowerM4(*parsed.weightType);
    if (wt != "pw" && wt != "pweight") {
      // [fw=]/[iw=] with the default vce(iid) are fitted natively
      // (runQregNative); only their other VCE forms reach this line.
      return engineErrorBlockM4("`qreg` [" + wt + "=] is implemented only with the default "
                                "vce(iid) in this version of PIET.",
                                "unsupported_option");
    }
    if (qregIid) {
      return ojson(eval::errorBlock(
          "engine_error",
          "option vce(iid) is not possible with pweighted data; pweights imply robust",
          std::nullopt, std::nullopt, std::nullopt, "r(404)"));
    }
    qregRobust = true;
  }
  if (qregRobust) {
    argv.push_back("--vce");
    argv.push_back("robust");
  }
  if (qregWeighted) {
    argv.push_back("--weight");
    argv.push_back(*parsed.weightVar);
    argv.push_back("--wtype");
    argv.push_back("pw");
  }

  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kKnown = kVocabQreg.known;
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

  ojson block = cobraToEstimationM4(raw, "qreg", &session);
  block["depvar"] = y;  // cobra qreg's raw JSON carries no "depvar" field at all (live-verified)
  static const char* kQregFields[] = {"tau", "sparsity", "bwidth", "sum_adev"};
  for (const char* k : kQregFields) {
    if (raw.contains(k)) block[k] = raw[k];
  }
  // FU-184: e(q) is the fitted quantile (w2p_estim_ev.log :104/:140/:170/:203/
  // :238/:267: .25/.75/.5). Both e()-scalar resolvers (eval/macros.cpp
  // resolveEScalar, display_hardening.cpp) read lm.result[key] with key == the
  // Stata name when the name is absent from eScalarKey(), so `q` needs no table
  // edit: publish it in the engine reply that becomes lm.result -- the model
  // result only, NOT the block, whose bytes internal capture qreg.json pins. The `tau` key stays (selftest.cpp:27497-27526 pins it).
  if (raw.contains("tau") && raw["tau"].is_number()) raw["q"] = raw["tau"];

  LastModel lm;
  lm.cobraCmd = "qreg";  // already anticipated in runMargins's kLinearCmds (below).
  lm.y = y;
  lm.x = xs;
  lm.depvar = y;
  // setLastModel publishes lm.vcov "hc1" as e(vce) `robust` / e(vcetype)
  // `Robust` (w2b_estim.log:791); the default stays "iid" (qreg_ctl :844 `iid`).
  if (qregRobust) lm.vcov = "hc1";
  captureFitFilterM4(lm, argv);
  lm.dataPath = *session.currentHbin();
  lm.result = raw;
  session.setLastModel(lm);
  session.lastEstBlock = block;
  return block;
}

// ===========================================================================
// mediate (BS-072, BETA D2 batch): Stata 18 syntax `mediate (Y [controls])
// (M [controls]) (T, continuous(#L0 #L1))` -> `cobra mediate --y Y
// --mediator M --treat T [--controls ...] [--reps N] [--seed N] [--level
// N]`. Oracle: probes/d2b-mediate.log (the ONLY working Stata invocation
// against this dataset — a bare continuous treatment var with no
// continuous() at all is Stata's OWN r(459); probes/d2-oracle-stata.log's
// D2g shows a bare `continuous` flag with no levels is Stata's OWN r(198))
// — so only the continuous(#L0 #L1) two-level form is in beta scope; every
// other treatment shape honest-rejects naming this as the supported form.
namespace {

// Depth-aware top-level paren-group splitter — unlike parseIvParenM4/
// reg3's own naive find(')')-based splitter above (safe there because
// neither ivregress's (endog=instruments) block nor a reg3 equation ever
// nests a SECOND paren inside), mediate's `(treat, continuous(L0 L1))`
// block genuinely nests one paren inside another; a naive first-')' scan
// would truncate at continuous('s OWN close paren instead of the outer
// block's.
struct ParenGroupsResultM4 {
  std::vector<std::string> groups;
  bool errored = false;
  std::string errorMsg;
};

ParenGroupsResultM4 splitParenGroupsM4(const std::string& joined) {
  ParenGroupsResultM4 r;
  size_t i = 0, n = joined.size();
  while (i < n) {
    while (i < n && (joined[i] == ' ' || joined[i] == '\t')) ++i;
    if (i >= n) break;
    if (joined[i] != '(') {
      r.errored = true;
      r.errorMsg = "expected a parenthesised block";
      return r;
    }
    size_t start = i + 1;
    int depth = 1;
    ++i;
    while (i < n && depth > 0) {
      if (joined[i] == '(') ++depth;
      else if (joined[i] == ')') --depth;
      ++i;
    }
    if (depth != 0) {
      r.errored = true;
      r.errorMsg = "unbalanced parentheses";
      return r;
    }
    r.groups.push_back(joined.substr(start, (i - 1) - start));
  }
  return r;
}

}  // namespace

ojson runMediate(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();

  std::string joined = joinSpaceM4(parsed.varlist);
  ParenGroupsResultM4 pg = splitParenGroupsM4(joined);
  if (pg.errored || pg.groups.size() != 3) {
    return engineErrorBlockM4(
        "syntax_error: mediate requires exactly three parenthesised blocks: "
        "(outcome [controls]) (mediator [controls]) (treatment, continuous(#level0 #level1))",
        "syntax_error");
  }

  std::vector<std::string> yToks = splitTokensM4(pg.groups[0]);
  std::vector<std::string> mToks = splitTokensM4(pg.groups[1]);
  if (yToks.empty() || mToks.empty()) {
    return engineErrorBlockM4(
        "syntax_error: mediate's outcome/mediator blocks each require a variable name",
        "syntax_error");
  }
  std::string yVar = yToks[0];
  std::string mVar = mToks[0];
  std::vector<std::string> controls;
  for (size_t k = 1; k < yToks.size(); ++k) controls.push_back(yToks[k]);
  for (size_t k = 1; k < mToks.size(); ++k) {
    bool already = false;
    for (const auto& c : controls) {
      if (c == mToks[k]) already = true;
    }
    if (!already) controls.push_back(mToks[k]);
  }

  // Split the treatment block on its FIRST top-level comma into `tvar` and
  // its own option tail (the same comma-then-options grammar parseOpts
  // applies to a whole command line, applied here to this ONE
  // already-isolated group).
  const std::string& tGroup = pg.groups[2];
  size_t tComma = std::string::npos;
  {
    int depth = 0;
    for (size_t k = 0; k < tGroup.size(); ++k) {
      if (tGroup[k] == '(') {
        ++depth;
      } else if (tGroup[k] == ')') {
        --depth;
      } else if (tGroup[k] == ',' && depth == 0) {
        tComma = k;
        break;
      }
    }
  }
  std::string tVarPart = tComma == std::string::npos ? tGroup : tGroup.substr(0, tComma);
  std::string tOptPart = tComma == std::string::npos ? std::string() : tGroup.substr(tComma + 1);
  std::vector<std::string> tToks = splitTokensM4(tVarPart);
  if (tToks.size() != 1) {
    return engineErrorBlockM4(
        "syntax_error: mediate's treatment block must name exactly one variable", "syntax_error");
  }
  std::string tVar = tToks[0];

  std::vector<std::pair<std::string, eval::OptValue>> tOpts = eval::parseOpts(tOptPart);
  // BUG-263: this is a second-source opts vector (same class as BUG-262's
  // runLog re-parse) — the dispatcher-level canonicalization point never sees
  // it, and until this net existed a typo'd or unknown sub-block key was
  // silently dropped and the fall-through reject below misdiagnosed the
  // user's SPECIFICATION ("only a continuous treatment ... is supported")
  // when the actual cause was an unread option key. Canonicalize FIRST so the
  // `k == "continuous"` reads below see canonical spellings (the BUG-262
  // one-vector law), then reject anything the sub-block grammar doesn't have.
  eval::canonicalizeOptionKeys(tOpts, kVocabMediateTreat.known);
  if (auto err = eval::rejectUnknownOptions(tOpts, kVocabMediateTreat.known,
                                            kVocabMediateTreat.unimplemented)) {
    return *err;
  }
  std::optional<std::string> continuousVal;
  bool sawContinuousFlag = false;
  for (const auto& [k, v] : tOpts) {
    if (k == "continuous") {
      if (v.isBool) sawContinuousFlag = true;
      else continuousVal = v.strValue;
    }
  }
  // Only a continuous treatment WITH two explicit levels is in beta scope
  // (see the function-header comment) — binary/default treatment (no
  // continuous() at all) and a bare `continuous` flag (no levels) both
  // honest-reject naming continuous(#level0 #level1) as the only supported
  // form, rather than silently answering with a different treatment model.
  if (!sawContinuousFlag && !continuousVal.has_value()) {
    return engineErrorBlockM4(
        "mediate: only a continuous treatment with two explicit levels is supported in this "
        "version of PIET — use `mediate (...) (...) (treatvar, continuous(#level0 #level1))`.",
        "unsupported_option");
  }
  if (!continuousVal.has_value() || trimM4(*continuousVal).empty()) {
    return engineErrorBlockM4(
        "mediate: continuous() requires exactly two numeric levels, e.g. continuous(0.5 1) — a "
        "bare `continuous` flag with no levels is not supported.",
        "unsupported_option");
  }
  std::vector<std::string> levelToks = splitTokensM4(*continuousVal);
  if (levelToks.size() != 2) {
    return engineErrorBlockM4(
        "mediate: only a two-level continuous() contrast is supported in this version of PIET "
        "(got " +
            std::to_string(levelToks.size()) + " level(s)) — use continuous(#level0 #level1).",
        "unsupported_option");
  }
  double l0 = 0, l1 = 0;
  try {
    l0 = std::stod(levelToks[0]);
    l1 = std::stod(levelToks[1]);
  } catch (...) {
    return engineErrorBlockM4("syntax_error: continuous() levels must be numeric", "syntax_error");
  }
  if (l0 == l1) {
    return engineErrorBlockM4("syntax_error: continuous() levels must differ", "syntax_error");
  }

  std::vector<std::string> argv = {"mediate", "--y", yVar, "--mediator", mVar, "--treat", tVar};
  if (!controls.empty()) {
    argv.push_back("--controls");
    argv.push_back(joinCommaM4(controls));
  }
  for (const auto& [k, v] : parsed.opts) {
    if (k == "reps" && !v.isBool) {
      argv.push_back("--reps");
      argv.push_back(v.strValue);
    } else if (k == "seed" && !v.isBool) {
      argv.push_back("--seed");
      argv.push_back(v.strValue);
    } else if (k == "level" && !v.isBool) {
      argv.push_back("--level");
      argv.push_back(v.strValue);
    }
  }
  if (parsed.ifExpr.has_value()) {
    argv.push_back("--keepif");
    argv.push_back(*parsed.ifExpr);
  }

  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kKnown = kVocabMediate.known;
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
  block["estimator"] = "mediate";
  block["depvar"] = yVar;
  block["n_obs"] = raw.value("n_obs", 0);
  block["treatment_levels"] = ojson::array({l0, l1});
  // ENGINE-PARITY DISCLOSURE, NOT STATA-PARITY (unlike every estimator
  // above, where point estimates match the oracle): cobra's `mediate`
  // computes a plain linear (Baron-Kenny, NO treatment-mediator
  // interaction), PER-UNIT causal-mediation decomposition — verified by
  // hand against this exact oracle by refitting the a-path (mediator~treat)
  // and b-path (outcome~treat+mediator) via `cobra feols` directly: cobra
  // mediate's own a/b/direct/indirect fields are EXACTLY those no-
  // interaction OLS coefficients (a=-0.1279895, b=1384.099235, both match
  // feols byte-for-byte). Stata's `continuous(#L0 #L1)` mediate instead
  // fits a RICHER model with an explicit treatment#mediator interaction
  // term (the oracle's own footer: "Outcome equation includes
  // treatment–mediator interaction") and reports the VanderWeele-formula
  // NDE/NIE AT THE SPECIFIC (L0,L1) contrast. Manually reconstructing that
  // formula from two `feols` calls (the a-path plus an interaction-
  // augmented b-path) DOES reproduce the oracle's NDE=-855.2799/
  // NIE=-107.2333 exactly — confirming the missing interaction term is the
  // entire source of the gap — but cobra's `mediate` subcommand itself has
  // NO --level0/--level1/interaction surface at all (checked its usage
  // banner AND a `strings` scan of the binary for hidden flags: none).
  // Hand-rolling the VanderWeele formula natively (plus its own bootstrap
  // inference, since cobra's own SEs/CIs would no longer apply once the
  // model changes) is out of this batch's realistic scope and arguably
  // cobra's job, not PIET's wiring layer. So: dispatch IS wired (the
  // command is recognized, a real argv is built, the engine is actually
  // invoked, real numbers come back) but the point estimates below are the
  // ENGINE's own per-unit, no-interaction decomposition, NOT a
  // Stata-matching (L0 vs L1)-contrast result — `direct`/`indirect`/
  // `total` are NOT literal NDE/NIE/TE. Flagged to the campaign as a
  // possible engine-side feature request (interaction term + explicit
  // contrast levels), the same disclosure shape as D1's ivregress
  // SE-convention gap, just a materially larger (whole-model, not
  // SE-formula) gap here.
  static const char* kMediateFields[] = {
      "a",           "b",           "direct",       "indirect",  "total",
      "se_a",        "se_b",        "se_aroian",    "se_goodman", "se_sobel",
      "z",           "p",           "ci_lo",        "ci_hi",     "proportion_mediated",
      "bootstrap",
  };
  for (const char* k : kMediateFields) {
    if (raw.contains(k)) block[k] = raw[k];
  }

  LastModel lm;
  lm.cobraCmd = "mediate";  // added to runMargins's kLinearCmds below — margins after mediate has
                            // no coherent meaning; dydx/AME should reject, not silently compute.
  lm.y = yVar;
  lm.x = {mVar, tVar};
  lm.depvar = yVar;
  captureFitFilterM4(lm, argv);
  lm.dataPath = *session.currentHbin();
  lm.result = raw;
  session.setLastModel(lm);
  session.lastEstBlock = block;
  return block;
}

// ===========================================================================
// poisson / ppmlhdfe / fepois / nbreg / xtpoisson / xtlogit / drdid (BETA D3
// batch, panel/count wirings: BS-073/074/075/076). Oracle (poisson-family,
// nbreg, xtpoisson, xtlogit): internal capture
// d3-oracle-stata.log (StataBE 19.5 on piet/packaging/sample/
// fatalities.csv, `gen cnt=round(spirits*3)` + `encode state, gen(snum)` +
// `xtset snum year`; sections D3a-D3g). Oracle (drdid): tests/validate/
// drdid2x2.{do,dta,log}.

namespace est_detail {  // R2: reopened — fillCoeftableStatsM4 is declared in TU 1

// nbreg/xtpois/xtlogit's own raw "coeftable" always carries p=ci_lo=ci_hi=
// null (live-verified against all three CLIs — only "b"/"se"/"t" are ever
// populated, t==b/se already). fepois's own coeftable, by contrast, DOES
// compute p/ci itself (also live-verified) so is never routed through this.
// Backfills the null fields from b/se/t the SAME Wald-normal way coefRowM4
// (csdid, above) already does, so the PIET UI doesn't ship a coeftable with
// three permanently-blank columns for these three estimator families.
void fillCoeftableStatsM4(ojson& block) {
  if (!block.contains("coeftable") || !block["coeftable"].is_array()) return;
  for (auto& row : block["coeftable"]) {
    if (!row.contains("b") || row["b"].is_null() || !row.contains("se") || row["se"].is_null()) {
      continue;
    }
    double b = row["b"].get<double>();
    double se = row["se"].get<double>();
    double z;
    if (row.contains("t") && row["t"].is_number()) {
      z = row["t"].get<double>();
    } else if (se != 0.0) {
      z = b / se;
      row["t"] = z;
    } else {
      continue;
    }
    if (!row.contains("p") || row["p"].is_null()) row["p"] = normalPvalue(z);
    if (!row.contains("ci_lo") || row["ci_lo"].is_null()) row["ci_lo"] = b - kZ975 * se;
    if (!row.contains("ci_hi") || row["ci_hi"].is_null()) row["ci_hi"] = b + kZ975 * se;
  }
}

}  // namespace est_detail

// poisson / ppmlhdfe / fepois (BS-073): count-model wiring -> cobra fepois.
// Oracle D3a (bare poisson b/se), D3b (offset()), D3c (vce(robust)), D3d
// (ppmlhdfe/absorb()).
//
// D0 finding (pre-existing, re-confirmed live): cobra's `fepois` WITHOUT
// --fe reproduces Stata's bare `poisson` exactly (D3a: beertax b=-.0537468
// se=.0514636, unemp b=-.0349375 se=.0097193, _cons b=1.940487 se=.076412 —
// all byte-for-byte once `cnt` is built with Stata's own round()
// convention, which cobra's `eval --gen round(...)` already matches
// live-verified). Real Stata's `poisson` has NO absorb() option at all;
// `ppmlhdfe` and cobra's own `fepois` command name both DO (-> --fe).
//
// offset() (D3b): LIVE-VERIFIED literal passthrough. Stata's own offset()
// option already expects an ln-scale variable (offset(lpop) where
// lpop=ln(income) — that IS what Stata's offset() has always meant); cobra's
// `--offset` expects the exact same thing. beertax b=.0753822 se=.0495923,
// _cons b=-7.914716 se=.0343278 — byte-for-byte with NO PIET-side ln()
// transform. exposure() (Stata's OTHER, raw-exposure count-model option) has
// no oracle coverage and no engine CLI equivalent — honest reject.
//
// ppmlhdfe/fepois DEFAULT to vce(robust) when the user gives no vce() at
// all — matching real ppmlhdfe's own default (the D3d oracle's own
// `ppmlhdfe cnt beertax, absorb(snum)` never types vce() yet its Stata
// table header reads "Robust std. err.", live-verified). Plain `poisson`
// keeps cobra's own iid default (matches D3a, which likewise never types
// vce() and gets the plain "Std. err." table).
//
// noconstant/level() are NEVER forwarded — live-verified the SAME
// cobra_runner.cpp stdout/exit-code masking bug the D1 report already
// flagged for ivregress/tobit: `cobra fepois --no-intercept` / `--level 90`
// BOTH hard-error on stderr ("unrecognized option", exit 3) while STILL
// printing a full, valid, WRONG (intercept-included / default-CI) result to
// stdout first.
//
// ENGINE FINDING (cobra-side, forbidden this batch — not fixed, reported):
// vce(robust)'s HC1 small-sample correction has a real numeric gap vs
// Stata's own poisson/ppmlhdfe robust SE. Point estimates (b) match Stata
// EXACTLY in every case tried. For the bare fepois case (D3c) the SE gap is
// small (~0.3% relative: engine .0321597 vs Stata .0320636). For the --fe
// (ppmlhdfe, D3d) case the gap is MATERIAL (~8% relative: engine .1238974
// vs Stata .1146782) — apparently from how absorbed-FE degrees of freedom
// enter cobra's small-sample correction factor (dof_resid IS reported
// correctly as 287 = N-k-FE in both the b and se paths, yet the se scale
// doesn't reflect it the way Stata's does). Selftest pins to the ENGINE's
// OWN actual value (not the Stata figure) with a comment, so the gate stays
// meaningful without permanently failing on a gap this batch cannot touch.
namespace {

// ppmlhdfe's `d`: the sum of the fixed effects, built on the head the fit read
// and written to a new head whose path is returned ("" and `err` on refusal).
//
// ppmlhdfe stores d = z - resid - x*b, demeaned with the IRLS
// weights mu: at convergence z - resid is eta = ln(mu), so
//   d_i = ln(mu_i) - x_i'b - c,   c = sum mu (ln mu - x'b) / sum mu,
// and c is exactly the reported _cons (:461-465 recovers _cons as the
// mu-weighted mean of ln(mu) - x'b). With ONE absorbed variable the FE score
// equations close the form: within group g, exp(eta) = exp(x'b) * Y_g / E_g with
// Y_g = sum y and E_g = sum exp(x'b), and sum_g mu = Y_g, so
//   d_i = ln(Y_g) - ln(E_g) - c,   c = sum_g Y_g (ln Y_g - ln E_g) / sum Y.
// Measured on the w2b_estim fixture (float-stored, absorb(id)): c from the
// engine's b is -2.0577731961463126, the engine's _cons -2.057773196146313 and
// the oracle's table -2.057773 (w2b_estim.log:1357). Without absorb() the only
// absorbed term is the constant, so d is 0 on the estimation sample.
// The sample is rebuilt here (nonmissing, if, singleton and all-zero-y groups
// out, as the engine drops them); a count that differs from the fit's N refuses
// rather than writing d on the wrong rows.
// The closed form is the UNWEIGHTED one (every sum below is unweighted). Weights never
// reach it: runPoisson refuses d/d() together with any weight before the fit.
std::string ppmlhdfeSaveDM4b(SessionState& session, const ojson& raw, const std::string& y,
                             const std::vector<std::string>& xs,
                             const std::vector<std::string>& fes,
                             const eval::ParsedCommand& parsed, const std::string& dName,
                             ojson& err) {
  auto refuse = [&](const std::string& why) {
    err = ojson(eval::errorBlock("unsupported_option", "ppmlhdfe d: " + why, std::nullopt,
                                 std::nullopt, std::nullopt, "r(198)"));
    return std::string();
  };
  Datastore* ds = session.datastore();
  if (ds == nullptr) return refuse("no dataset in memory");
  for (const auto& f : fes) {
    if (!ds->has(f) || ds->dtype(f) != "f64") {
      err = engineErrorBlockM4(
          "Option `d` is a valid ppmlhdfe option that this version of PIET implements for a "
          "numeric absorb() variable only (`" + f + "`).",
          "unsupported_option");
      return std::string();
    }
  }
  char buf[64];
  std::string xb;
  for (const auto& x : xs) {
    const double b = numberAtM4b(raw, "coef", x);
    if (!std::isfinite(b)) return refuse("the fit has no coefficient for `" + x + "`");
    std::snprintf(buf, sizeof(buf), "%.17g", b);
    xb += (xb.empty() ? "" : "+") + std::string("(") + buf + ")*(" + x + ")";
  }

  std::string sample = "!missing(" + y + ")";
  for (const auto& x : xs) sample += "&!missing(" + x + ")";
  for (const auto& f : fes) sample += "&!missing(" + f + ")";
  if (parsed.weightVar.has_value() && !parsed.weightVar->empty()) {
    sample += "&!missing(" + *parsed.weightVar + ")";
  }
  if (parsed.ifExpr.has_value() && !parsed.ifExpr->empty()) {
    sample += "&(" + *parsed.ifExpr + ")";
  }

  // Scratch column names (design decision: no shared helper mints one); the
  // double-underscore shape of handlers.cpp's residual mask.
  const std::string kS = "__piet_ppmld_s__", kN = "__piet_ppmld_n__", kY = "__piet_ppmld_y__",
                    kK = "__piet_ppmld_k__", kE = "__piet_ppmld_e__", kEs = "__piet_ppmld_es__",
                    kA = "__piet_ppmld_a__", kT1 = "__piet_ppmld_t1__", kT2 = "__piet_ppmld_t2__";
  std::vector<std::string> temps;
  std::vector<std::string> args = {"eval", "--data", *session.currentHbin()};
  auto gen = [&](const std::string& name, const std::string& expr) {
    args.push_back("--gen");
    args.push_back(name + "=" + expr);
    if (name != dName) temps.push_back(name);
  };
  auto egen = [&](const std::string& name, const std::string& expr) {
    args.push_back("--egen");
    args.push_back(name + "=" + expr);
    temps.push_back(name);
  };
  gen(kS, "(" + sample + ")");
  if (fes.empty()) {
    gen(dName, "cond(" + kS + "==1,0,.)");
  } else {
    const std::string& g = fes[0];
    egen(kN, "sum(" + kS + "), by(" + g + ")");
    egen(kY, "sum(cond(" + kS + "==1," + y + ",0)), by(" + g + ")");
    gen(kK, "(" + kS + "==1)&(" + kN + ">1)&(" + kY + ">0)");
    gen(kE, "cond(" + kK + "==1,exp(" + (xb.empty() ? std::string("0") : xb) + "),0)");
    egen(kEs, "sum(" + kE + "), by(" + g + ")");
    gen(kA, "cond(" + kK + "==1,ln(" + kY + ")-ln(" + kEs + "),.)");
    egen(kT1, "sum(cond(" + kK + "==1," + y + "*" + kA + ",0))");
    egen(kT2, "sum(cond(" + kK + "==1," + y + ",0))");
    gen(dName, "cond(" + kK + "==1," + kA + "-" + kT1 + "/" + kT2 + ",.)");
  }
  for (const auto& t : temps) {
    args.push_back("--drop");
    args.push_back(t);
  }
  args.push_back("--summarize");
  args.push_back(dName);
  const std::string outPath = nextPathM4(session.workdir(), "ppmlhdfe_d");
  args.push_back("--out");
  args.push_back(outPath);

  nlohmann::json res;
  try {
    res = runCobraJsonM4(args);
  } catch (const CobraError& ce) {
    err = engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
    return std::string();
  }
  double nD = std::nan("");
  try {
    nD = res.at("summarize").at(dName).at("N").get<double>();
  } catch (...) {
  }
  const double nFit = raw.contains("n_obs") && raw["n_obs"].is_number()
                          ? raw["n_obs"].get<double>()
                          : std::nan("");
  if (!std::isfinite(nD) || !std::isfinite(nFit) || nD != nFit) {
    cecon::native_unlink(outPath);
    return refuse("could not reproduce the estimation sample (" +
                  (std::isfinite(nD) ? std::to_string(static_cast<long long>(nD)) : "?") +
                  " rows against the fit's " +
                  (std::isfinite(nFit) ? std::to_string(static_cast<long long>(nFit)) : "?") +
                  "); d is not saved.");
  }
  return outPath;
}

}  // namespace

// e(sample) T4 (internal note esample/SPEC.md §3.4 T1b; oracle
// w2_esample.log:1692 H1 logit `0|60|60|`, :1725 H2 probit `if` `0|47|47|`,
// :1928 H7 poisson `0|60|60|`). The engine's ML verbs (logit, probit, fepois)
// have no `--esample-out` (grep esample_request: feols/iv/ols/xtreg/newey/prais
// only), and the engine is not this change's to edit. DESIGN DECISION: the mask
// is the engine's own feols admission rule on the fit's y/x/if/cluster/weight,
// written by feols `--esample-out` and installed through T2's one install
// (esampleInstallOnHead: the same hidden column, serial and predicate), but
// only when the ML fit's own row count equals it. Why that is exact and never a
// guess: load_glm (apps/cmds/common.cpp) admits the rows build_model admits for
// those columns -- the function feols' fit calls, with no FE here -- and then
// the ML screen can only DROP rows (perfect predictors). The
// ML sample is therefore a subset of the feols sample, and a subset of equal
// size is the same set. Anything the auxiliary call does not replay (offset(),
// svy design columns, a TS operator under `if`, whose feols mask is computed on
// the full series) can only make the feols set larger, so it fails the count
// and the model keeps the named r(198) (kUnavailable). So does every refusal
// below. Returns esampleInstallOnHead's answer; `lm`/`raw` as it leaves them.
// Cost: one extra feols over the fit's columns per ML fit.
// ppmlhdfe/fepois with absorb() (BUG-1197; StataBE 19.5: `ppmlhdfe cnt x1 x2,
// absorb(id)` e(N) 176 = count if e(sample)): the auxiliary feols absorbs the same
// effects and drops the same singleton groups, so the fepois sample (singletons and
// separated rows out) is again a subset of it, equal when nothing was separated.
// `ontoFrame` (ppmlhdfe d): the column goes onto that frame -- the head plus the d
// column, which becomes the head after e() is posted -- and *ontoFrame names the
// frame with the mask, as runRegress installs it on its residuals() frame.
bool esampleInstallMlMask(SessionState& session, LastModel& lm, ojson& raw,
                          std::string* ontoFrame) {
  // tobit, nbreg and mlogit admit rows the same way and screen none out
  // (BUG-1214: StataBE's `estimates store` writes their _est_NAME).
  static const std::set<std::string> kMl = {"logit", "probit", "poisson", "tobit", "nbreg",
                                            "mlogit", "fepois"};
  if (kMl.count(lm.cobraCmd) == 0) return false;
  if (!lm.fe.empty() && lm.cobraCmd != "fepois") return false;
  if (!lm.fitDataPath.empty() || lm.cluster.size() > 1) return false;
  if (!session.currentHbin().has_value() || *session.currentHbin() != lm.dataPath) return false;
  if (!raw.is_object() || lm.y.empty()) return false;
  // The handshake counts ROWS. Under fweight/iweight an ML reply's n_obs is Σw
  // (cmd_logit: `n_obs` = Σw under Frequency) and it carries no n_rows, so the
  // feols superset's row count can equal it by accident (screened-out rows whose
  // weights add up to the shortfall) and be installed as e(sample). No row
  // count, no install: the model keeps the named r(198) (review 2026-10-15).
  const bool sumWeights = lm.fitWtype == "fw" || lm.fitWtype == "fweight" ||
                          lm.fitWtype == "iw" || lm.fitWtype == "iweight";
  if (sumWeights && !(raw.contains("n_rows") && raw["n_rows"].is_number())) return false;
  std::vector<std::string> aux = {"feols", "--y", lm.y, "--x",
                                  lm.x.empty() ? std::string("(intercept)") : joinCommaM4(lm.x)};
  if (!lm.fe.empty()) {
    aux.push_back("--fe");
    aux.push_back(joinCommaM4(lm.fe));
  }
  if (!lm.fitKeepif.empty()) {
    aux.push_back("--keepif");
    aux.push_back(lm.fitKeepif);
  }
  // The variance never moves a row; a sandwich is what feols accepts under
  // every weight kind (it refuses an iid pweight fit).
  aux.push_back("--vcov");
  if (lm.cluster.size() == 1) {
    aux.push_back("cluster");
    aux.push_back("--cluster");
    aux.push_back(lm.cluster.front());
  } else {
    aux.push_back("hc1");
  }
  if (!lm.fitWeight.empty()) {
    aux.push_back("--weight");
    aux.push_back(lm.fitWeight);
    if (!lm.fitWtype.empty()) {
      aux.push_back("--wtype");
      aux.push_back(lm.fitWtype);
    }
  }
  aux.push_back("--data");
  aux.push_back(lm.dataPath);
  const std::string values = esampleFitArgs(session, aux, std::string(), 0);
  ojson auxRaw;
  try {
    auxRaw = runCobraJsonM4(aux);
  } catch (const CobraError&) {
    cecon::native_unlink(values);
    return false;
  }
  if (!auxRaw.is_object() || !auxRaw.contains("esample_n") || !auxRaw["esample_n"].is_number()) {
    cecon::native_unlink(values);
    return false;
  }
  // The handshake (handlers.cpp esampleHandshakeOk) compares these 1s with the
  // ML reply's own row count (n_rows when it has one, else n_obs).
  raw["esample_n"] = auxRaw["esample_n"];
  if (ontoFrame == nullptr) return esampleInstallOnHead(session, values, raw, lm);
  unsigned long long serial = 0;
  const std::string withMask =
      esampleInstallColumn(session, values, raw, *ontoFrame, false, &serial);
  if (withMask.empty()) return false;
  *ontoFrame = withMask;
  lm.esample = LastModel::EsampleState::kColumn;
  lm.esampleSerial = serial;
  return true;
}

bool esampleInstallMlMask(SessionState& session, LastModel& lm, ojson& raw) {
  return esampleInstallMlMask(session, lm, raw, nullptr);
}

// `dMutationOut` (BUG-1251): ppmlhdfe's d/d() adds a column to the live frame after
// the fit; the handler still returns the one estimation block and writes the
// data_mutation announcing the column here, for the caller to push as its own block
// (runRegress's residMutationOut). The dispatcher's poisson arm passes it (declared
// there: piet/handlers.h keeps the three-argument form, below).
ojson runPoisson(const std::string& stataCmd, const eval::ParsedCommand& parsed,
                  SessionState& session, ojson* dMutationOut) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  if (parsed.varlist.empty()) {
    return engineErrorBlockM4("syntax_error: `" + stataCmd + "` requires a dependent variable",
                              "syntax_error");
  }
  std::string y = parsed.varlist[0];
  std::vector<std::string> xs(parsed.varlist.begin() + 1, parsed.varlist.end());

  bool allowFe = (stataCmd == "ppmlhdfe" || stataCmd == "fepois");

  std::string feRaw;
  bool sawAbsorb = false;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "absorb") {
      sawAbsorb = true;
      if (!v.isBool) feRaw = v.strValue;
    }
  }
  if (sawAbsorb && !allowFe) {
    return engineErrorBlockM4(
        "option `absorb` not allowed — real Stata's `poisson` has no absorb() option; use "
        "`ppmlhdfe` for a fixed-effects Poisson (PPML) fit.",
        "unsupported_option");
  }

  // ppmlhdfe `d` / `d(newvar)`: save the sum of the fixed effects
  // (w2b_estim.log:1305 ppmlhdfe_d, :1336 ppmlhdfe_absorb_d_named, both rc 0
  // with the fit of the option-free command). ppmlhdfe: bare `d`
  // DROPS any existing _ppmlhdfe_d and names the column that; d(newvar) is
  // `confirm new variable` (r(110) when it exists), checked before the fit.
  // The column is built after the fit by ppmlhdfeSaveDM4b. Only the shapes it
  // reproduces exactly are taken; the rest are refused by name.
  std::string dName;
  if (stataCmd == "ppmlhdfe") {
    for (const auto& [k, v] : parsed.opts) {
      if (k != "d") continue;
      if (v.isBool) {
        dName = "_ppmlhdfe_d";
        // ppmlhdfe drops the old column before the fit reads its
        // variables, so a model that USES _ppmlhdfe_d no longer finds it.
        const std::vector<std::string> feToks = splitTokensM4(feRaw);
        if (y == dName || std::find(xs.begin(), xs.end(), dName) != xs.end() ||
            std::find(feToks.begin(), feToks.end(), dName) != feToks.end()) {
          return stataRefusalM4b("variable " + dName + " not found", 111);
        }
        continue;
      }
      dName = trimM4(v.strValue);
      if (dName.empty() || !strutil::isValidStataIdentifier(dName)) {
        return stataRefusalM4b("d(): invalid name `" + dName + "'", 198);
      }
      Datastore* ds = session.datastore();
      if (ds != nullptr && ds->has(dName)) {
        return stataRefusalM4b("variable " + dName + " already defined", 110);
      }
    }
  }
  // More than one absorbed effect: the sum has no closed form, so the engine
  // writes it from the fit itself (fepois --d-out; w2d_uncovered
  // ppmlhdfe_xi_absorb_cluster2_d_001, `absorb(id t) ... d`). The engine maps the
  // fit's rows onto the frame only without a row filter, so `if` stays refused.
  bool engineD = false;
  if (!dName.empty()) {
    std::string why;
    if (splitTokensM4(feRaw).size() > 1) {
      if (parsed.ifExpr.has_value() && !parsed.ifExpr->empty()) {
        why = "an `if` qualifier and more than one absorbed fixed effect";
      } else {
        engineD = true;
      }
    }
    for (const auto& [k, v] : parsed.opts) {
      if (k == "offset" || k == "exposure") why = "offset()/exposure()";
    }
    if (parsed.inRange.has_value() && !parsed.inRange->empty()) why = "an `in` range";
    for (const auto& x : xs) {
      if (x.find_first_of(".#()") != std::string::npos) {
        why = "factor-variable or time-series regressors";
      }
    }
    // Weights (last, so it wins). Corpus: 0 of 81 ppmlhdfe statements carry a weight. The
    // weighted closed form is source-derived (ppmlhdfe,478-479, reghdfe5:
    // the FE sum is demeaned with W = w*mu) but uncaptured (draft w2q_ppml_d_weights), and
    // ppmlhdfeSaveDM4b only knows the unweighted sums. The engine refuses weighted --fe
    // (src/estimators/fepois.cpp:30-33), so no weighted absorb() fit could exercise it.
    if (parsed.weightVar.has_value() && !parsed.weightVar->empty()) why = "weights";
    if (!why.empty()) {
      return ojson(eval::errorBlock(
          "unsupported_option",
          "Option `d` is a valid ppmlhdfe option that this version of PIET does not implement "
          "together with " + why + ".",
          std::nullopt, std::nullopt, std::nullopt, "r(198)"));
    }
  }

  // No regressors (`ppmlhdfe cnt`, `ppmlhdfe cnt, absorb(t)`): the engine's
  // --no-x fits the constant alone, or the absorbed effects with _cons
  // recovered (w2d_uncovered ppmlhdfe_noreg_noabsorb_ctl `_b[_cons]` =
  // ln(mean y) 2.083599551828503, ppmlhdfe_absorb_noreg_d_062 2.091719879995292).
  std::vector<std::string> argv = {"fepois", "--y", y};
  if (xs.empty()) {
    argv.push_back("--no-x");
  } else {
    argv.push_back("--x");
    argv.push_back(joinCommaM4(xs));
  }

  if (parsed.ifExpr.has_value()) {
    argv.push_back("--keepif");
    argv.push_back(*parsed.ifExpr);
  }

  // W:P5: `poisson … [pweight=w]`. registry.cpp's row for this command decides
  // which weight types get this far; the engine's fepois honours --weight/
  // --wtype. Same normalization the shared builder and runRegress use.
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

  if (sawAbsorb) {
    std::vector<std::string> feToks = splitTokensM4(feRaw);
    if (!feToks.empty()) {
      argv.push_back("--fe");
      argv.push_back(joinCommaM4(feToks));
    }
  }

  // offset(varname) — D3b oracle-verified LITERAL passthrough (see header comment).
  std::optional<std::string> offsetVar;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "offset" && !v.isBool) offsetVar = v.strValue;
  }
  if (offsetVar.has_value() && !offsetVar->empty()) {
    argv.push_back("--offset");
    argv.push_back(*offsetVar);
  }
  for (const auto& [k, v] : parsed.opts) {
    if (k == "exposure" && !v.isBool) {
      return engineErrorBlockM4(
          "Option `exposure()` is a valid Stata option that this version of PIET does not yet "
          "implement for `" + stataCmd + "` (no oracle coverage and no engine CLI equivalent — "
          "use offset() with a pre-computed ln(exposure) variable instead).",
          "unsupported_option");
    }
  }

  // BUG-1278: a cluster variable that is not in the data is r(111) "variable V
  // not found" before the engine runs, and poisson, which takes one cluster
  // variable, refuses a second (r(198) under vce(), first; r(103) under
  // cluster()). ppmlhdfe clusters multiway and has its own parser.
  const bool poissonCmd = stataCmd == "poisson";
  if (auto bad = clusterSpecRefusalM4(
          parsed, session,
          poissonCmd ? ClusterOptParserM4::kVceParse : ClusterOptParserM4::kPpmlhdfe,
          /*multiwayVceRefused=*/poissonCmd, /*legacyOneVar=*/poissonCmd))
    return *bad;
  VcovResult vr = vcovFromOpts(parsed);
  if (vr.errored) {
    return vcovErrorBlockM4(vr);
  }
  bool sawExplicitVce = !vr.argv.empty();
  // ppmlhdfe/fepois DEFAULT to robust when the user specified no vce() at
  // all (see header comment) — plain `poisson` keeps cobra's iid default.
  // BUG-1538: a probability weight implies vce(robust) on `poisson` too, the
  // buildEstimationArgvM4 rule (BUG-536; StataBE 19.5 `poisson cnt x z [pw=wf]`
  // e(vce) robust, _se[x] .41060909851198435).
  const std::string wtLower = parsed.weightType.has_value() ? lowerM4(*parsed.weightType) : "";
  const bool pweighted = parsed.weightVar.has_value() && !parsed.weightVar->empty() &&
                         (wtLower == "pw" || wtLower == "pweight");
  if (!sawExplicitVce && (allowFe || pweighted)) {
    vr.argv = {"--vcov", "hc1"};
  }
  for (const auto& a : vr.argv) argv.push_back(a);

  // noconstant/level() — NEVER forwarded (masking-bug precedent, see header comment).
  for (const auto& [k, v] : parsed.opts) {
    if ((k == "noconstant" || k == "nocons") && v.isBool && v.boolValue) {
      return engineErrorBlockM4(
          "Option `" + k + "` is a valid Stata option that this version of PIET does not yet "
          "implement for `" + stataCmd + "` (live-verified: cobra fepois's `--no-intercept` "
          "hard-errors on stderr while still printing a full WRONG result to stdout — never "
          "forwarded).",
          "unsupported_option");
    }
    if (k == "level" && !v.isBool) {
      return engineErrorBlockM4(
          "Option `level` is a valid Stata option that this version of PIET does not yet "
          "implement for `" + stataCmd + "` (same stdout/exit-code masking gap as noconstant "
          "above).",
          "unsupported_option");
    }
  }

  // kVocabPpmlD is ppmlhdfe's own slot: the shared count-model set plus `d`.
  const std::unordered_set<std::string>& known =
      (stataCmd == "ppmlhdfe") ? kVocabPpmlD.known : kVocabPoisson.known;
  if (auto err = eval::rejectUnknownOptions(parsed.opts, known)) {
    return *err;
  }

  // TS-context injection (MI-083 §16), as buildEstimationArgvM4 does for logit/probit:
  // after `xtset id t`, `poisson cnt L.x1 x2` needs the panel and time variables to
  // resolve L.x1 (StataBE 19.5, BUG-1484: N 150, _b[L.x1] -.050220717740893102).
  std::vector<std::string> tsTokens = {y};
  for (const auto& x : xs) tsTokens.push_back(x);
  if (translate::hasTsToken(tsTokens)) argv = translate::injectTsContext(argv, session);

  argv.push_back("--data");
  argv.push_back(*session.currentHbin());
  argv.push_back("--tidy");

  ojson raw;
  std::string engineDPath;
  try {
    // FU-859 (eb-order T-P1): the fit's full e(V) (the fepois options_table row
    // carries --emit-v; apps/cmds/common.cpp run_fepois_emit_v). Only the fit
    // call gets the flag, not `argv`, which the LastModel filter reads. The
    // recovered _cons and its full covariance are retained on absorbed fits too.
    std::vector<std::string> fitArgs = argv;
    fitArgs.push_back("--emit-v");
    if (engineD) {
      engineDPath = nextPathM4(session.workdir(), "ppmlhdfe_d");
      fitArgs.push_back("--d-out");
      fitArgs.push_back(engineDPath);
      fitArgs.push_back("--d-name");
      fitArgs.push_back(dName);
    }
    raw = runCobraJsonM4(fitArgs);
  } catch (const CobraError& ce) {
    if (!engineDPath.empty()) cecon::native_unlink(engineDPath);
    // BUG-1269: ppmlhdfe, like reghdfe, calls an empty sample insufficient,
    // r(2001), and so a one-row one, whose single row the singleton drop removes
    // (StataBE 19.5: `ppmlhdfe c x if x > 20, absorb(g)` and `if x == 1`).
    if (stataCmd == "ppmlhdfe" && ce.code == "no_obs" && std::string(ce.what()) == "no observations")
      return ojson(eval::errorBlock("engine_error", "insufficient observations", std::nullopt,
                                    std::nullopt, std::nullopt, std::string("r(2001)")));
    return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
  }

  // d: built on the head the fit read, BEFORE e() is posted, so a column that
  // cannot be reproduced refuses the whole command instead of leaving e()
  // describing a fit whose d is absent.
  std::string dPath;
  if (engineD) {
    if (!std::filesystem::exists(cecon::to_native_path(engineDPath))) {
      return ojson(eval::errorBlock("unsupported_option",
                                    "ppmlhdfe d: the engine did not write the sum of the fixed "
                                    "effects; d is not saved.",
                                    std::nullopt, std::nullopt, std::nullopt, "r(198)"));
    }
    dPath = engineDPath;
  } else if (!dName.empty()) {
    ojson err;
    dPath = ppmlhdfeSaveDM4b(session, raw, y, xs, splitTokensM4(feRaw), parsed, dName, err);
    if (dPath.empty()) return err;
  }

  ojson block = cobraToEstimationM4(raw, "fepois", &session);
  block["estimator"] = stataCmd;  // literal Stata command word typed, not cobra's internal name.
  block["depvar"] = y;

  LastModel lm;
  lm.cobraCmd = sawAbsorb ? "fepois" : "poisson";  // margins' kModelMap already maps BOTH keys
                                                    // to "--model poisson" for AME dydx.
  // e(cmd) is `ppmlhdfe` after ppmlhdfe (w2l_vce_shape_det.log:2526
  // ppmlhdfe_ctl_cluster `0|ppmlhdfe|cluster|Robust|30|180`; FU-967); the
  // replay machinery keeps keying on cobraCmd.
  if (stataCmd == "ppmlhdfe") lm.stataCmd = "ppmlhdfe";
  lm.y = y;
  lm.x = xs;
  if (sawAbsorb) {
    for (const auto& f : splitTokensM4(feRaw)) lm.fe.push_back(f);
  }
  for (size_t i = 0; i < argv.size(); ++i) {
    if (argv[i] == "--vcov" && i + 1 < argv.size()) lm.vcov = argv[i + 1];
    if (argv[i] == "--cluster" && i + 1 < argv.size()) {
      for (const auto& c : splitTokensM4(argv[i + 1])) lm.cluster.push_back(c);
    }
  }
  lm.depvar = y;
  captureFitFilterM4(lm, argv);
  lm.dataPath = *session.currentHbin();
  lm.result = raw;
  // e(sample) T4 (w2_esample.log:1928 H7): plain poisson, and ppmlhdfe/fepois with
  // absorb() (BUG-1197). BUG-518 order: the mask, then setLastModel. With d the mask
  // goes onto the d frame, the head once e() is posted.
  (void)esampleInstallMlMask(session, lm, *lm.result, dPath.empty() ? nullptr : &dPath);
  // BUG-1481: ppmlhdfe's cluster bookkeeping, reghdfe's (StataBE 19.5, 180 rows):
  // e(N_clustervars) the number of cluster variables, e(N_clust1..k) each one's
  // clusters in cluster() order (vce(cluster id) 30; vce(cluster id id2) 30/7;
  // vce(cluster id id2 id3) 30/7/5, while e(N_clust) is the smallest) and
  // e(clustvar1..k) their names (below, with the model); none under vce(robust).
  // One-way, e(N_clust1) is the reply's n_clusters; multiway, each dimension's own G
  // (n_clusters_by, read before any key is added). Cached reply only, as after
  // reghdfe (handlers.cpp).
  const bool ppmlClustered = lm.stataCmd == "ppmlhdfe" && !lm.cluster.empty() &&
                             (lm.vcov == "cluster" || lm.vcov == "twoway" ||
                              lm.vcov == "multiway") &&
                             lm.result->is_object() && lm.result->contains("n_clusters") &&
                             (*lm.result)["n_clusters"].is_number();
  if (ppmlClustered) {
    ojson& rr = *lm.result;
    std::vector<double> counts;
    if (lm.cluster.size() == 1) {
      counts.push_back(rr["n_clusters"].get<double>());
    } else if (rr.contains("n_clusters_by") && rr["n_clusters_by"].is_array() &&
               rr["n_clusters_by"].size() == lm.cluster.size()) {
      for (const auto& g : rr["n_clusters_by"])
        if (g.is_number()) counts.push_back(g.get<double>());
      if (counts.size() != lm.cluster.size()) counts.clear();
    }
    rr["N_clustervars"] = static_cast<double>(lm.cluster.size());
    for (size_t j = 0; j < counts.size(); ++j) rr["N_clust" + std::to_string(j + 1)] = counts[j];
  }
  session.setLastModel(lm);
  session.lastEstBlock = block;
  session.attachFitCovariance(raw);  // FU-859: e(V) is the fit's own, never diag(se^2)
  if (ppmlClustered) {
    for (size_t j = 0; j < lm.cluster.size(); ++j) {
      const std::string nm = "clustvar" + std::to_string(j + 1);
      session.eval.setEString(nm, lm.cluster[j]);
      session.noteEOnFit(nm);
    }
  }
  if (!dPath.empty()) {
    // The head the fit (and lm.dataPath) read stays untouched until e() is
    // posted; the new column lands now (the reghdfe residuals() order).
    Datastore* before = session.datastore();
    const bool dExisted = before != nullptr && before->has(dName);  // bare d's old _ppmlhdfe_d
    session.load(dPath);
    // d is saved double, %10.0g, labelled "Sum of fixed effects" (ppmlhdfe), and
    // ppmlhdfe posts its name as e(d) (StataBE 19.5, BUG-1250/BUG-1252: e(d) "dd"
    // after d(dd), "_ppmlhdfe_d" after d, empty after a later fit without d -- so the
    // macro is noted on this fit, and the next install drops it).
    VarMeta& meta = session.varMeta[dName];
    meta.stataType = "double";
    meta.format = "%10.0g";
    meta.label = "Sum of fixed effects";
    session.eval.setEString("d", dName);
    session.noteEOnFit("d");
    // BUG-1251: the column is announced as every new variable is, so the variables
    // panel and the n_missing fast path see it (the reghdfe residuals() hand-off).
    if (dMutationOut != nullptr) {
      std::vector<std::string> added, rewritten;
      (dExisted ? rewritten : added).push_back(dName);
      *dMutationOut = mutationResult(session, "gen", added, {}, rewritten);
    }
  }
  return block;
}

ojson runPoisson(const std::string& stataCmd, const eval::ParsedCommand& parsed,
                  SessionState& session) {
  return runPoisson(stataCmd, parsed, session, nullptr);
}

// nbreg (BS-074): negative-binomial (NB2, "dispersion(mean)" — Stata's
// default) regression -> cobra nbreg. Oracle D3e.
//
// ENGINE FIX (cobra 0.25.0): the Newton optimizer's convergence gap
// described below is fixed. cobra's nbreg now converges on the D3e oracle's
// own data (`nbreg cnt beertax unemp` on fatalities.csv) — the true MLE
// sits at the alpha->0 boundary (the Poisson limit: this data has no
// detectable overdispersion once beertax/unemp are controlled for), and
// cobra now detects and reports that boundary cleanly via a Poisson
// warm-start feeding the joint (beta, ln-alpha) Newton fit plus explicit
// boundary reporting, instead of the previous "no_convergence" failure.
// (History: through 0.24.x, cobra's nbreg Newton optimizer FAILED TO
// CONVERGE on this and several independently-constructed synthetic
// overdispersed count datasets whenever the fitted intercept sat
// appreciably away from zero — a genuine intercept-initialization/step-size
// gap in the engine's joint Newton fit, not a PIET-wiring problem.) The
// wiring below is complete and correct; selftest now pins b/se/alpha/
// lnalpha against the small SYNTHETIC dataset (argv/JSON round-trip proof,
// not Stata-oracle-pinned) AND separately oracle-pins the D3e leg's own
// exact call (`nbreg cnt beertax unemp`) against its Stata coefficients,
// ll, and the alpha=0 boundary (selftest.cpp).
ojson runNbreg(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  if (parsed.varlist.size() < 2) {
    return engineErrorBlockM4("syntax_error: `nbreg` requires a dependent variable and at least "
                              "one regressor",
                              "syntax_error");
  }
  std::string y = parsed.varlist[0];
  std::vector<std::string> xs(parsed.varlist.begin() + 1, parsed.varlist.end());

  std::vector<std::string> argv = {"nbreg", "--y", y, "--x", joinCommaM4(xs)};

  if (parsed.ifExpr.has_value()) {
    argv.push_back("--keepif");
    argv.push_back(*parsed.ifExpr);
  }

  bool noConstant = false;
  for (const auto& [k, v] : parsed.opts) {
    if ((k == "noconstant" || k == "nocons") && v.isBool && v.boolValue) noConstant = true;
  }
  // Unlike fepois above, cobra nbreg's `--no-intercept` is DOCUMENTED and
  // live-verified genuinely honored (not a masking no-op) — safe to forward.
  if (noConstant) argv.push_back("--no-intercept");

  std::optional<std::string> offsetVar;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "offset" && !v.isBool) offsetVar = v.strValue;
  }
  if (offsetVar.has_value() && !offsetVar->empty()) {
    argv.push_back("--offset");
    argv.push_back(*offsetVar);
  }
  for (const auto& [k, v] : parsed.opts) {
    if (k == "exposure" && !v.isBool) {
      return engineErrorBlockM4(
          "Option `exposure()` is a valid Stata option that this version of PIET does not yet "
          "implement for `nbreg` (no oracle coverage and no engine CLI equivalent — use offset() "
          "with a pre-computed ln(exposure) variable instead).",
          "unsupported_option");
    }
    if (k == "dispersion" && !v.isBool) {
      return engineErrorBlockM4(
          "Option `dispersion()` is a valid Stata option that this version of PIET does not yet "
          "implement for `nbreg` (only the default NB2 \"mean\" dispersion is wired — no oracle "
          "coverage for dispersion(constant)/NB1).",
          "unsupported_option");
    }
  }

  // BUG-1278: _vce_parse's refusals come first: a second cluster variable (nbreg
  // takes one: vce(cluster a b) r(198) before any lookup, cluster(a b) r(103)
  // after it), a variable that is not in the data (r(111)), vce() paired with
  // cluster() (r(198)/r(100)).
  if (auto bad = clusterSpecRefusalM4(parsed, session, ClusterOptParserM4::kVceParse,
                                      /*multiwayVceRefused=*/true))
    return *bad;
  // vce(): nbreg's own CLI shape is `--vce iid|robust|cluster [--cluster C]`
  // (NOT feols/fepois's `--vcov`/hc1/twoway shape) — reuse vcovFromOpts for
  // the Stata-side parsing, then translate its {"--vcov",TYPE,...} output.
  // "twoway" (2 cluster vars) has no nbreg equivalent (usage banner: a
  // single --cluster slot only) — honest reject. A missing cluster var
  // inside vce(cluster) already fails inside vcovFromOpts itself (shared
  // discipline with feols/logit) before ever reaching here, so nbreg's own
  // live-verified `--vce cluster`-with-no-`--cluster` silent-no-op shape
  // (identical numbers to vce(robust)) is structurally unreachable from
  // this handler.
  VcovResult vr = vcovFromOpts(parsed);
  if (vr.errored) {
    return vcovErrorBlockM4(vr);
  }
  if (vr.argv.size() >= 2 && vr.argv[0] == "--vcov") {
    const std::string& type = vr.argv[1];
    if (type == "hc1") {
      argv.push_back("--vce");
      argv.push_back("robust");
    } else if (type == "cluster") {
      argv.push_back("--vce");
      argv.push_back("cluster");
      argv.push_back("--cluster");
      argv.push_back(vr.argv[3]);  // vr.argv == {"--vcov","cluster","--cluster",VAR}
    } else if (type == "twoway") {
      return engineErrorBlockM4(
          "vce(cluster A B) (two-way clustering) is not supported for `nbreg` — the engine's "
          "nbreg CLI has only a single --cluster slot.",
          "unsupported_option");
    } else if (type == "iid") {
      argv.push_back("--vce");
      argv.push_back("iid");
    }
  }
  // BUG-1537: nbreg's fweights, iweights and pweights (the engine fits them; its
  // aweight is Stata's r(101), the tokenizer's kStataLegalWeights row). A pweight
  // implies vce(robust), the buildEstimationArgvM4 rule (BUG-536; StataBE 19.5
  // `nbreg cnt x z [pw=wf]` e(vce) robust, _se[x] .4424215353885973).
  if (parsed.weightType.has_value() && parsed.weightVar.has_value() &&
      !parsed.weightVar->empty()) {
    const std::string wt = lowerM4(*parsed.weightType);
    const std::string wtype = (wt == "fw" || wt == "fweight")   ? "fw"
                              : (wt == "iw" || wt == "iweight") ? "iw"
                              : (wt == "pw" || wt == "pweight") ? "pw"
                                                                : "";
    if (!wtype.empty()) {
      if (wtype == "pw" && vr.argv.empty()) {
        argv.push_back("--vce");
        argv.push_back("robust");
      }
      argv.push_back("--weight");
      argv.push_back(*parsed.weightVar);
      argv.push_back("--wtype");
      argv.push_back(wtype);
    }
  }

  for (const auto& [k, v] : parsed.opts) {
    if (k == "level" && !v.isBool) {
      // BUG-274: this used to reject, on the premise that "cobra nbreg's --level hard-errors
      // on stderr while still printing a full WRONG result to stdout". That premise is doubly
      // expired: BUG-202 gave nbreg a real `--level` (per-coef ci_lo/ci_hi at the asked level,
      // z-based like Stata's own nbreg table — live-verified `--level 90` on this tree's
      // engine), and even the PRE-fix half was stale against the current pin (WS-S re-measured:
      // stdout was EMPTY on the hard-error, not a masked result). Forward, same idiom as
      // runTobit/runQreg above. The engine validates the range itself ([10, 99.99]) and its
      // error envelope surfaces through CobraError below.
      argv.push_back("--level");
      argv.push_back(v.strValue);
      continue;
    }
  }

  const std::unordered_set<std::string>& known = kVocabNbreg.known;
  if (auto err = eval::rejectUnknownOptions(parsed.opts, known)) {
    return *err;
  }

  // BUG-1196: the fit's e(V), posted below (none at the α→0 boundary).
  argv.push_back("--emit-v");
  argv.push_back("--data");
  argv.push_back(*session.currentHbin());
  argv.push_back("--tidy");

  ojson raw;
  try {
    raw = runCobraJsonM4(argv);
  } catch (const CobraError& ce) {
    return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
  }

  ojson block = cobraToEstimationM4(raw, "nbreg", &session);
  block["estimator"] = "nbreg";
  block["depvar"] = y;
  fillCoeftableStatsM4(block);
  if (raw.contains("alpha")) block["alpha"] = raw["alpha"];
  if (raw.contains("lnalpha")) block["lnalpha"] = raw["lnalpha"];
  if (raw.contains("se_lnalpha_iid")) block["se_lnalpha"] = raw["se_lnalpha_iid"];
  // ll: cobra emits `loglik`; the block key is `ll` like every other MLE block
  // (logit/probit) — was omitted here, leaving nbreg the odd one out.
  if (raw.contains("loglik")) block["ll"] = raw["loglik"];

  LastModel lm;
  lm.cobraCmd = "nbreg";
  lm.y = y;
  lm.x = xs;
  for (size_t i = 0; i < argv.size(); ++i) {
    if (argv[i] == "--vce" && i + 1 < argv.size()) lm.vcov = argv[i + 1];
    if (argv[i] == "--cluster" && i + 1 < argv.size()) lm.cluster = {argv[i + 1]};
  }
  lm.depvar = y;
  captureFitFilterM4(lm, argv);
  lm.dataPath = *session.currentHbin();
  lm.result = raw;
  session.setLastModel(lm);
  session.lastEstBlock = block;
  // BUG-1196: Stata's e(b) is the depvar's equation, then `/lnalpha` (StataBE
  // 19.5 `cnt:x1 cnt:x2 cnt:_cons /lnalpha`); e(V) is the fit's, in that order.
  if (raw.contains("e_V") && raw.contains("coefs") && raw.contains("lnalpha") &&
      raw["lnalpha"].is_number()) {
    std::vector<AncillaryEbColM4> cols;
    if (depvarEqColsM4(y, raw["coefs"], "beta", cols)) {
      cols.push_back({"/lnalpha", "/lnalpha", raw["lnalpha"].get<double>()});
      (void)postAncillaryEbM4(session, raw, cols);
    }
  }
  return block;
}

// xtpoisson / xtlogit, fe (BS-075): conditional fixed-effects panel count/
// binary MLE. Oracle D3f (xtpoisson,fe), D3g (xtlogit,fe; depvar
// hi2=(spirits>1.5)). Both reproduce the oracle's b/se EXACTLY (bit-for-bit
// at 8+ significant digits, including D3g's 37-groups-omitted/n_obs=77
// all-positive-or-negative-outcome dropping and D3f's loglik=-460.6417) — no
// engine gap found for either, unlike BS-073/074 above.
//
// Real Stata's DEFAULT (bare `xtpoisson`/`xtlogit`, no fe/re/pa) is
// RANDOM-EFFECTS, not fixed — this engine has NO xtpoisson/xtlogit RE (or
// PA) estimator at all (only the conditional-FE MLE), so `,re`/`,pa` and
// the bare/no-option form ALL honest-reject naming `,fe` as the only
// supported form, matching runXtreg's own fe/re model-select precedent.
//
// vce(robust) genuinely changes the SE for BOTH (live-verified: se_robust
// != se_iid, real numbers, not a no-op). vce(cluster ...)/cluster() is a
// SILENT NO-OP for both — live-verified on the compiled engine directly:
// neither CLI's usage banner even lists a --cluster flag, yet passing
// `--vce cluster` to EITHER is accepted with exit 0 and produces the SAME
// numbers as the non-robust default (xtlogit's case is worse: se==se_iid,
// not even se_robust) — honest reject rather than forward a flag that would
// silently claim a clustered SE the engine never computed. noconstant/
// level() are NEVER forwarded on either CLI (same stdout/exit-code
// masking-bug shape as BS-073/074 above, live-verified on both).
ojson runXtPoissonLogit(const std::string& cobraCmd, const std::string& stataCmd,
                         const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();

  bool fePresent = false, rePresent = false, paPresent = false;
  eval::OptValue feVal;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "fe") {
      fePresent = true;
      feVal = v;
    }
    if (k == "re") rePresent = true;
    if (k == "pa") paPresent = true;
  }
  bool isFe = fePresent && feVal.isBool && feVal.boolValue;
  if (!isFe) {
    std::string which = rePresent ? ",re" : (paPresent ? ",pa" : "the default (random-effects)");
    return engineErrorBlockM4(
        "This version of PIET supports only `" + stataCmd + " ,fe` (conditional fixed-effects "
        "MLE). `" + stataCmd + "` with " + which + " is not available — the engine has no "
        "random-effects or population-averaged " + stataCmd + " estimator.",
        "unsupported_option");
  }

  if (parsed.varlist.size() < 2) {
    return engineErrorBlockM4(
        "syntax_error: `" + stataCmd + "` requires a dependent variable and at least one "
        "regressor",
        "syntax_error");
  }
  std::string y = parsed.varlist[0];
  std::vector<std::string> xs(parsed.varlist.begin() + 1, parsed.varlist.end());

  // BUG-657: xtlogit.ado / xtpoisson.ado call `_xt, i()`, whose refusal is
  // "must specify panelvar; use xtset" r(459) (as xtreg: w2_eststo_prefix.log:
  // 437-441); was unsupported_option = r(198).
  if (!session.panelvar.has_value() || session.panelvar->empty()) {
    return ojson(eval::errorBlock("engine_error", "must specify panelvar; use xtset", std::nullopt,
                                  std::nullopt, std::nullopt, "r(459)"));
  }

  std::vector<std::string> argv = {cobraCmd, "--y",    y,
                                    "--x",    joinCommaM4(xs), "--ivar", *session.panelvar};

  if (parsed.ifExpr.has_value()) {
    argv.push_back("--keepif");
    argv.push_back(*parsed.ifExpr);
  }

  // offset() — xtpoisson only (cobra's xtpois CLI has --offset; xtlogit's
  // does not, per its own usage banner — an offset() on xtlogit falls
  // through to the leftover unknown-option reject below).
  if (cobraCmd == "xtpois") {
    std::optional<std::string> offsetVar;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "offset" && !v.isBool) offsetVar = v.strValue;
    }
    if (offsetVar.has_value() && !offsetVar->empty()) {
      argv.push_back("--offset");
      argv.push_back(*offsetVar);
    }
  }

  // FU-23 (BUG-756): the cluster VCE is refused by Stata itself for both ,fe
  // estimators, in every spelling but one, and this loop read `cluster` only
  // when valued -- so a bare `cluster` fitted the iid model at rc 0.
  // w2l_vce_shape.log, r(198) unless noted (xtpoisson message / xtlogit message):
  //   cluster(id), cluster(id2), cluster(id id2)  "option cluster() not allowed" (both)
  //   bare `cluster`                "option cluster not allowed" / "cluster invalid"
  //   vce(cluster id), vce(cluster) "option cluster not allowed" / "vcetype cluster not allowed"
  //   vce(cluster id id2)           "multiway cluster VCE not allowed" (both)
  //   robust(x1)                    "option robust() not allowed" / "robust(x1) invalid"
  //   vce(robust x1)                "option x1 not allowed" / "vcetype robust not allowed"
  //   xtlogit vce(robust)           "vcetype robust not allowed" (xtlogit_fe_ctl_vcerobust;
  //                                 xtpoisson_fe_ctl_vcerobust fits, rc 0)
  //   EMPTY `cluster()`             rc 0, IGNORED: the default fit (xtpoisson e(vce) oim;
  //                                 xtlogit runs as clogit, e(cmd) clogit)
  // The engine could not honour a cluster VCE anyway (its conditional-FE CLIs
  // accept `--vce cluster` and silently compute the non-clustered SE), so no
  // refusal here hides a capability.
  const bool isXtlogit = (cobraCmd == "xtlogit");
  const VceShapesM4b vceShape = readVceShapesM4b(parsed);
  if (vceShape.cluster == OptShapeM4b::kBare) {
    return stataRefusalM4b(isXtlogit ? "cluster invalid" : "option cluster not allowed", 198);
  }
  if (vceShape.cluster == OptShapeM4b::kOne || vceShape.cluster == OptShapeM4b::kMany) {
    return stataRefusalM4b("option cluster() not allowed", 198);
  }
  if (vceShape.robust != OptShapeM4b::kAbsent && vceShape.robust != OptShapeM4b::kBare) {
    return stataRefusalM4b(isXtlogit ? "robust(" + vceShape.robustRaw + ") invalid"
                                     : std::string("option robust() not allowed"),
                           198);
  }
  if (vceShape.vceType == "cluster") {
    if (vceShape.vceRest.size() > 1) return stataRefusalM4b("multiway cluster VCE not allowed", 198);
    return stataRefusalM4b(isXtlogit ? "vcetype cluster not allowed" : "option cluster not allowed",
                           198);
  }
  const bool vceRobustFamily = vceShape.vceType == "robust" || vceShape.vceType == "hc1";
  if (vceRobustFamily && !vceShape.vceRest.empty()) {
    return stataRefusalM4b(isXtlogit ? std::string("vcetype robust not allowed")
                                     : "option " + vceShape.vceRest[0] + " not allowed",
                           198);
  }
  // xtlogit ,fe takes no robust VCE at all. The bare `robust` flag is the same
  // request as vce(robust) in this handler (and in Stata's vce parsing); it has
  // no arm of its own in the oracle, so refusing it rests on vce(robust)'s.
  if (isXtlogit && (vceRobustFamily || vceShape.robust == OptShapeM4b::kBare)) {
    return stataRefusalM4b("vcetype robust not allowed", 198);
  }
  bool sawRobustFlag = false;
  std::optional<std::string> vceVal;
  for (const auto& [k, v] : parsed.opts) {
    if ((k == "vce" || k == "vcov") && !vceVal.has_value()) {
      vceVal = v.isBool ? std::string() : v.strValue;
    }
    if (k == "robust" && v.isBool && v.boolValue) sawRobustFlag = true;
  }
  bool wantsRobust =
      sawRobustFlag ||
      (vceVal.has_value() && (canonicalVceType(*vceVal, nullptr) == "robust" ||
                              canonicalVceType(*vceVal, nullptr) == "hc1"));
  if (vceVal.has_value() && !vceVal->empty()) {
    const std::string lv = canonicalVceType(*vceVal, nullptr);
    if (lv != "robust" && lv != "hc1" && lv != "iid") {
      if (!unimplementedVceType(*vceVal).empty()) {
        return engineErrorBlockM4("Option `vce(" + unimplementedVceType(*vceVal) + ")` is a valid Stata option that this version of PIET does not implement", "unsupported_option");
      }
      return engineErrorBlockM4("Unknown vce type: " + *vceVal, "unsupported_option");
    }
  }
  if (wantsRobust) {
    argv.push_back("--vce");
    argv.push_back("robust");
  }

  for (const auto& [k, v] : parsed.opts) {
    if ((k == "noconstant" || k == "nocons") && v.isBool && v.boolValue) {
      return engineErrorBlockM4(
          "Option `" + k + "` is a valid Stata option that this version of PIET does not yet "
          "implement for `" + stataCmd + "` (conditional-FE MLE has no separate constant to "
          "suppress — live-verified the engine's CLI hard-errors while still masking a WRONG "
          "result to stdout, same precedent as the other D3 estimators above).",
          "unsupported_option");
    }
    if (k == "level" && !v.isBool) {
      // BUG-274, xtpois half: BUG-202 gave cobra's `xtpois` CLI a real `--level`
      // (live-verified on this tree's engine: `--level 90` emits per-coef
      // ci_lo/ci_hi), so the old shared refusal's premise expired for xtpoisson
      // and it forwards, same idiom as runTobit/runNbreg. The xtlogit half of
      // the premise is still TRUE — re-measured on the same build: `cobra
      // xtlogit --level 90` hard-errors "unrecognized option" (exit 3) with a
      // CLEAN empty stdout (the old "masking gap" wording was stale too: no
      // masked result prints), so xtlogit keeps refusing, now for the measured
      // reason.
      if (cobraCmd == "xtpois") {
        argv.push_back("--level");
        argv.push_back(v.strValue);
        continue;
      }
      return engineErrorBlockM4(
          "Option `level` is a valid Stata option that this version of PIET does not yet "
          "implement for `" + stataCmd + "` (the engine's xtlogit CLI has no --level surface — "
          "live-verified: `cobra xtlogit --level` hard-errors 'unrecognized option', exit 3, "
          "with a clean empty stdout).",
          "unsupported_option");
    }
  }

  // BUG-233: the base set is the file-scope literal; `offset` stays conditional
  // here because xtlogit must keep REJECTING it. The registered vocabulary is
  // the union of the two, which is correct for canonicalization: resolution
  // only rewrites a key to a canonical spelling, and the reject net below is
  // still the authority on whether that spelling is allowed for THIS command.
  std::unordered_set<std::string> known = kXtPoissonLogitBase;
  if (cobraCmd == "xtpois") known.insert("offset");
  if (auto err = eval::rejectUnknownOptions(parsed.opts, known)) {
    return *err;
  }

  argv.push_back("--data");
  argv.push_back(*session.currentHbin());
  argv.push_back("--tidy");

  ojson raw;
  try {
    // ME06: xtpoisson, fe sends its e(V) (--emit-v), the matrix margins reads.
    std::vector<std::string> fitArgv = argv;
    fitArgv.push_back("--emit-v");
    raw = runCobraJsonM4(fitArgv);
  } catch (const CobraError& ce) {
    return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
  }

  ojson block = cobraToEstimationM4(raw, cobraCmd, &session);
  block["estimator"] = stataCmd + "_fe";
  block["depvar"] = y;
  fillCoeftableStatsM4(block);
  static const char* kXtFields[] = {"n_groups", "n_dropped_const", "n_dropped_missing",
                                    "n_dropped_size"};
  for (const char* k : kXtFields) {
    if (raw.contains(k)) block[k] = raw[k];
  }

  LastModel lm;
  lm.cobraCmd = cobraCmd;  // "xtpois"/"xtlogit" — already anticipated in margins' kModelMap.
  // e(cmd) is Stata's name, not the engine's: `xtpoisson` (w2l_vce_shape.log
  // xtpoisson_fe_ctl_vcerobust, where main published `xtpois`), and `clogit` for
  // every xtlogit ,fe fit: xtlogit ,fe runs `clogit ... group(ivar)` and
  // never reposts e(cmd), only e(cmd2) "xtlogit" (w2p_estim_ev D_xtlogit_fe
  // `|clogit|`, w2l_vce_shape xtlogit_fe_empty_cluster).
  lm.stataCmd = isXtlogit ? std::string("clogit") : stataCmd;
  // FU-102: e(N_g) and the panel row counts over the fit's panels (after the
  // all-0/all-1/all-zero and singleton drops) for xtlogit and
  // xtpoisson (w2p_estim_ev D_xtlogit_fe / D_xtpoisson_fe
  // `0|54|9|6|6|`). The engine emits g_min/g_avg/g_max; N_g is its n_groups.
  if (raw.is_object() && raw.contains("n_groups") && raw["n_groups"].is_number())
    raw["N_g"] = raw["n_groups"];
  lm.y = y;
  lm.x = xs;
  lm.ivar = *session.panelvar;
  if (wantsRobust) lm.vcov = "hc1";
  // The fe fits are ML: the default e(vce) is `oim` (w2l_vce_shape_det.log:1003
  // xtpoisson_fe_empty_cluster `0|xtpoisson|oim||.|180`, xtlogit_fe_empty_cluster
  // `0|clogit|oim||.|180`).
  if (lm.vcov == "iid") lm.eVce = "oim";
  lm.depvar = y;
  captureFitFilterM4(lm, argv);
  lm.dataPath = *session.currentHbin();
  lm.result = raw;
  session.setLastModel(lm);
  session.lastEstBlock = block;
  session.attachFitCovariance(raw);  // e(V) is the fit's own, for both conditional FE families
  if (isXtlogit) {
    session.eval.setEString("cmd2", "xtlogit");
    session.noteEOnFit("cmd2");
  }
  return block;
}

ojson runXtPoisson(const eval::ParsedCommand& parsed, SessionState& session) {
  return runXtPoissonLogit("xtpois", "xtpoisson", parsed, session);
}

ojson runXtLogitFe(const eval::ParsedCommand& parsed, SessionState& session) {
  return runXtPoissonLogit("xtlogit", "xtlogit", parsed, session);
}

// drdid (BS-076): the classic 2-period/2-group doubly-robust DiD estimator
// -> the engine's csdid --method dripw machinery, ONE (g,t) cell. Oracle:
// tests/validate/drdid2x2.{do,log} — `drdid Y x1 x2, ivar(id) time(t)
// treatment(treat) dripw` (the fixture's EXACT invocation; this is the ONLY
// drdid surface wired — other method()s (reg/ipw/std_ipw/trad) and the
// bare/no-`dripw` form have no oracle coverage and honest-reject).
//
// Implementation boundary:
// this wiring touches ZERO cobra-side code — src/estimators/csdid.cpp's
// estimator runs completely unmodified, exactly as `csdid`/`estat` already
// call it. The ONLY new logic is PIET-side: constructing a synthetic `gvar`
// column from the fixture's own treatment()/time() variables
// (treatment()!=0 -> gvar=max(time), the post period; treatment()==0 ->
// gvar=0, CS21's "never-treated" sentinel — csdid_dripw_run only ever forms
// a cohort from gvar>0 rows, so 0 is a safe comparison-group marker
// regardless of how the panel's own time variable happens to be coded) via
// cobra's GENERIC `eval --gen` data-prep verb (the same verb `generate`/
// `egen` already route through elsewhere in this codebase — not
// csdid-specific), then calling the UNMODIFIED `cobra csdid --method dripw
// --comparison never` on the augmented file.
//
// LIVE-VERIFIED against the oracle (drdid2x2.dta, 600 obs, 300 units x 2
// periods): ATT = 2.0561083418597157 (oracle 2.05610834185971), SE =
// 0.12400508894658192 (oracle .124005088944043) — matches to ~1e-13
// relative, i.e. this construction is mathematically exact, not an
// approximation.
//
// Restricted to the fixture's own 2-period design (time() must have EXACTLY
// 2 distinct values) — matching the command's own name ("2x2") and its real
// Stata namesake, which is likewise a strictly-2-period estimator.
namespace {

// Reads a numeric (f64) column's distinct non-missing values and max —
// mirrors computeYMinMaxM4's direct-Datastore-read pattern above (BS-069),
// minus the if-filter machinery (the caller here already staged the exact
// sample it wants read into `ds`).
bool numericColumnStatsM4(Datastore& ds, const std::string& name, std::set<double>& distinct,
                          double& outMax, ojson& errBlockOut, const std::string& ctx) {
  if (!ds.has(name) || ds.dtype(name) != "f64") {
    errBlockOut =
        engineErrorBlockM4(ctx + ": `" + name + "` is not a numeric variable", "engine_error");
    return false;
  }
  const cecon::Column& c = ds.col(name);
  const double* d = c.f64();
  int64_t count = c.nbytes / static_cast<int64_t>(sizeof(double));
  double mx = -std::numeric_limits<double>::infinity();
  bool any = false;
  for (int64_t i = 0; i < count; ++i) {
    double v = d[i];
    if (std::isnan(v)) continue;
    any = true;
    if (v > mx) mx = v;
    distinct.insert(v);
  }
  if (!any) {
    errBlockOut =
        engineErrorBlockM4(ctx + ": `" + name + "` has no non-missing values", "engine_error");
    return false;
  }
  outMax = mx;
  return true;
}

}  // namespace

ojson runDrdid(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  if (parsed.varlist.empty()) {
    return engineErrorBlockM4("syntax_error: drdid requires a dependent variable", "syntax_error");
  }
  std::string y = parsed.varlist[0];
  std::vector<std::string> xs(parsed.varlist.begin() + 1, parsed.varlist.end());

  std::optional<std::string> ivar, timeVar, treatVar;
  bool sawDripw = false;
  static const std::set<std::string> kOtherMethods = {"reg", "ipw", "std_ipw", "trad"};
  std::vector<std::string> otherMethodsSeen;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "ivar" && !v.isBool) ivar = v.strValue;
    if (k == "time" && !v.isBool) timeVar = v.strValue;
    if (k == "treatment" && !v.isBool) treatVar = v.strValue;
    if (k == "dripw" && v.isBool && v.boolValue) sawDripw = true;
    if (kOtherMethods.count(k) && v.isBool && v.boolValue) otherMethodsSeen.push_back(k);
  }
  if (!ivar.has_value() || ivar->empty()) {
    return engineErrorBlockM4(
        "drdid requires `ivar(varname)` — e.g. `drdid y x, ivar(id) time(t) treatment(d) dripw`.",
        "unsupported_option");
  }
  if (!timeVar.has_value() || timeVar->empty()) {
    return engineErrorBlockM4(
        "drdid requires `time(varname)` — e.g. `drdid y x, ivar(id) time(t) treatment(d) dripw`.",
        "unsupported_option");
  }
  if (!treatVar.has_value() || treatVar->empty()) {
    return engineErrorBlockM4(
        "drdid requires `treatment(varname)` — e.g. `drdid y x, ivar(id) time(t) treatment(d) "
        "dripw`.",
        "unsupported_option");
  }
  if (!otherMethodsSeen.empty()) {
    return engineErrorBlockM4(
        "drdid, " + otherMethodsSeen[0] + ": only the doubly-robust IPW estimator (`dripw`) is "
        "wired to the engine in this version of PIET — reg/ipw/std_ipw/trad have no engine "
        "equivalent.",
        "unsupported_option");
  }
  if (!sawDripw) {
    return engineErrorBlockM4(
        "drdid requires the explicit `dripw` option in this version of PIET (the engine only "
        "implements the doubly-robust IPW estimator) — e.g. `drdid y x, ivar(id) time(t) "
        "treatment(d) dripw`.",
        "unsupported_option");
  }

  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kKnown = kVocabDrdid.known;
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kKnown)) {
    return *err;
  }

  return drdidFitCoreM4(session, y, xs, *ivar, *timeVar, *treatVar, *treatVar, parsed.ifExpr);
}

namespace {

// runDrdid's fit after its option parsing, shared with csdid's failure path
// (runCsdid, cluster()==ivar): `treatExpr` is the treatment as an expression
// (a variable name for drdid itself) and `treatName` is the name the ATET
// column carries (`ATET:r1vs0.<treatName>`, drdid).
ojson drdidFitCoreM4(SessionState& session, const std::string& y,
                     const std::vector<std::string>& xs, const std::string& ivarName,
                     const std::string& timeVarName, const std::string& treatExpr,
                     const std::string& treatName, const std::optional<std::string>& ifExpr) {
  const std::optional<std::string> ivar = ivarName;
  const std::optional<std::string> timeVar = timeVarName;
  const bool treatIsVar = treatExpr == treatName;
  // If-filter first (mirrors applyIfFilterM4's established idiom, BS-069)
  // so the gvar construction below reads/derives from the SAME sample cobra
  // will actually estimate on.
  ojson ifErr;
  auto basePathOpt = applyIfFilterM4(session, *session.currentHbin(), ifExpr, ifErr);
  if (!basePathOpt.has_value()) return ifErr;
  std::string basePath = *basePathOpt;

  bool madeIfTemp = (basePath != *session.currentHbin());
  std::optional<Datastore> tempDs;
  Datastore* ds = nullptr;
  if (madeIfTemp) {
    tempDs.emplace(basePath);
    ds = &*tempDs;
  } else {
    ds = session.datastore();
  }
  if (ds == nullptr) {
    if (madeIfTemp) cecon::native_unlink(basePath);
    return engineErrorBlockM4("drdid: could not read the current dataset", "engine_error");
  }

  std::set<double> timeLevels;
  double tmax = 0;
  ojson statErr;
  if (!numericColumnStatsM4(*ds, *timeVar, timeLevels, tmax, statErr, "drdid")) {
    if (madeIfTemp) cecon::native_unlink(basePath);
    return statErr;
  }
  if (timeLevels.size() != 2) {
    if (madeIfTemp) cecon::native_unlink(basePath);
    return engineErrorBlockM4(
        "drdid: `" + *timeVar + "` has " + std::to_string(timeLevels.size()) +
        " distinct values — this version of PIET's drdid only supports the classic two-period "
        "(2x2) panel design (exactly 2 time() values), matching the command's own real-Stata "
        "restriction.",
        "unsupported_option");
  }
  if (treatIsVar && (!ds->has(treatExpr) || ds->dtype(treatExpr) != "f64")) {
    if (madeIfTemp) cecon::native_unlink(basePath);
    return engineErrorBlockM4("drdid: `" + treatExpr + "` is not a numeric variable", "engine_error");
  }

  char tmaxBuf[64];
  std::snprintf(tmaxBuf, sizeof(tmaxBuf), "%.17g", tmax);
  static const char* kGvarName = "__piet_drdid_gvar__";
  std::string genExpr =
      std::string(kGvarName) + "=cond((" + treatExpr + ")!=0," + tmaxBuf + ",0)";
  std::string gvarPath = nextPathM4(session.workdir(), "drdid_gvar");
  std::vector<std::string> genArgs = {"eval", "--data", basePath, "--gen", genExpr, "--out",
                                      gvarPath};
  try {
    runCobraJsonM4(genArgs);
  } catch (const CobraError& ce) {
    if (madeIfTemp) cecon::native_unlink(basePath);
    return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
  }
  if (madeIfTemp) cecon::native_unlink(basePath);

  std::vector<std::string> argv = {"csdid",  "--y", y,        "--ivar", *ivar,
                                    "--time", *timeVar, "--gvar", kGvarName};
  if (!xs.empty()) {
    argv.push_back("--x");
    argv.push_back(joinCommaM4(xs));
  }
  argv.push_back("--method");
  argv.push_back("dripw");
  argv.push_back("--comparison");
  argv.push_back("never");
  argv.push_back("--data");
  argv.push_back(gvarPath);
  argv.push_back("--tidy");

  ojson raw;
  try {
    raw = runCobraJsonM4(argv);
  } catch (const CobraError& ce) {
    return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
  }

  ojson block = csdidToEstimationM4(raw, "simple");
  block["estimator"] = "drdid";
  block["depvar"] = y;
  // BUG-112(5): the drdid ATET is a SINGLE-row table (csdid simple aggregation) and
  // that lone row is renamed. Guard on size==1 so that if the underlying
  // aggregation ever grows to multiple rows, we don't silently mislabel row 0 by
  // position — the rename only applies while the one-row invariant holds.
  // drdid-ematrix-post: Stata's name is `ATET:r1vs0.<treat>` (drdid;
  // w2ag log:1810 ag_drdid `drdid|120|1|ATET:r1vs0.treat|dripw|`), so `_b[treat]`
  // is r(111) (log:1821) and `_b[ATET:r1vs0.treat]` answers.
  const std::string atetName = "ATET:r1vs0." + treatName;
  std::optional<CsdidPostingM4> drPost;
  if (block.contains("coeftable") && block["coeftable"].is_array() &&
      block["coeftable"].size() == 1) {
    block["coeftable"][0]["name"] = atetName;
    // The one cell's b and V (the engine's attgt cell 1; the simple aggregate
    // of one cell is that cell).
    if (auto cells = csdidPostingM4(raw, "attgt"); cells.has_value() && !cells->names.empty()) {
      CsdidPostingM4 p;
      p.names = {atetName};
      p.b = {cells->b[0]};
      p.V = {cells->V[0]};
      drPost = p;
    }
  }

  // NOT cacheCsdidModel() (above): that helper hardcodes lm.dataPath =
  // session.currentHbin(), which is the ORIGINAL dataset — missing the
  // synthesized gvar column. A later `estat simple/event/calendar/group/
  // pretrend` re-run needs `gvarPath` (this drdid call's own augmented
  // file), or it would hard-error "column not found" instead of
  // reproducing this result.
  LastModel lm;
  lm.cobraCmd = "csdid";  // it really IS the same underlying engine call — `estat` after `drdid`
                          // keeps working, same as after a genuine `csdid` call.
  lm.y = y;
  lm.x = xs;
  lm.depvar = y;
  lm.dataPath = gvarPath;
  lm.ivar = *ivar;
  lm.timevar = *timeVar;
  lm.gvar = std::string(kGvarName);
  lm.method = std::string("dripw");
  lm.comparison = std::string("never");
  // drdid-ematrix-post: e(cmd) drdid (w2ag log:1810); cobraCmd stays "csdid" so
  // estat keeps replaying the engine call. No e(vce)/e(vcetype) is posted.
  lm.stataCmd = "drdid";
  lm.vcov = "";
  lm.result = raw;
  session.setLastModel(lm);
  session.lastEstBlock = block;
  if (drPost.has_value()) postCsdidPostingM4(session, *drPost);
  session.eval.setEString("method", "dripw");
  session.noteEOnFit("method");  // dropped by the next install
  return block;
}

}  // namespace


}  // namespace piet::handlers
