// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// options_table.cpp — B.0.3 option-forwarding contract (Boundary A §option-forwarding).
//
// The single declarative source of truth for "which --flags each cobra subcommand
// accepts". Serialized by `cobra options --json` so PIET can pre-validate an option
// BEFORE forwarding it (honest Stata-style reject at the PIET layer, no round-trip),
// and cross-checked at test time by the drift detector (COBRA_OPTIONS_STRICT, see
// check_unconsumed_flags in common.cpp): every flag a command actually consumes via
// opt()/has()/opt_all() must appear here, or ctest fails.
//
// The engine's OWN correctness does NOT depend on this table: an unrecognized flag is
// rejected by the consumed-marks net under buffered dispatch (cecon_cli.cpp) — which
// discards the would-be result BEFORE it reaches stdout — using the flags actually
// read, not this declaration. So a table that slightly over-declares can never cause a
// silently-wrong answer (the historical BUG-110 failure mode); it can only make PIET's
// advisory pre-check permissive, with the engine as the authoritative backstop.
//
// Keys are DISPATCH names (argv[0]), including aliases (tab, pwcorr, iv-diag,
// import-dta). Globals --data/--tidy/--format/--no-provenance are read in main() for
// every command; --out/--keepif are per-command. arity: true = takes a value
// (opt/opt_all), false = boolean (has). Generated first-cut from the apps/cmds opt()
// call sites + hand-verified helper reads (dfuller/pperron trend, feols/iv tsset);
// maintain by hand thereafter — the drift detector is the guard.
#include "common.h"
#include "commands.h"

#include <cstdio>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace cecon {
namespace cli {

const std::map<std::string, std::vector<OptSpec>>& options_table() {
    static const std::map<std::string, std::vector<OptSpec>> T = {
  {"nnmatch", {{"--data",true}, {"--y",true}, {"--t",true}, {"--x",true}, {"--fv",true}, {"--nn",true}, {"--metric",true}, {"--stat",true}, {"--vce-nn",true}, {"--dtolerance",true}, {"--level",true}, {"--keepif",true}, {"--out",true}, {"--tidy",false}, {"--format",true}, {"--no-provenance",false}}},
  {"oaxaca", {{"--data",true}, {"--y",true}, {"--x",true}, {"--by",true}, {"--pooled",false}, {"--relax",false}, {"--level",true}, {"--keepif",true}, {"--out",true}, {"--tidy",false}, {"--format",true}, {"--no-provenance",false}}},
  {"options", {{"--data",true}, {"--format",true}, {"--json",false}, {"--no-provenance",false}, {"--tidy",false}}},
  {"functions", {{"--data",true}, {"--egen",false}, {"--format",true}, {"--json",false}, {"--no-provenance",false}, {"--tidy",false}}},
  {"append", {{"--data",true}, {"--format",true}, {"--keepif",true}, {"--keepusing",true}, {"--no-provenance",false}, {"--out",true}, {"--tidy",false}, {"--type-clash",true}, {"--using",true}}},
  {"archlm", {{"--data",true}, {"--format",true}, {"--keepif",true}, {"--lags",true}, {"--no-provenance",false}, {"--out",true}, {"--tidy",false}, {"--x",true}, {"--y",true}}},
  {"bgodfrey", {{"--data",true}, {"--format",true}, {"--keepif",true}, {"--lags",true}, {"--no-provenance",false}, {"--nomiss0",false}, {"--out",true}, {"--small",false}, {"--tidy",false}, {"--x",true}, {"--y",true}}},
  {"chow", {{"--by",true}, {"--data",true}, {"--format",true}, {"--keepif",true}, {"--no-provenance",false}, {"--out",true}, {"--tidy",false}, {"--vcov",true}, {"--x",true}, {"--y",true}}},
  {"clonevar", {{"--data",true}, {"--format",true}, {"--keepif",true}, {"--no-provenance",false}, {"--out",true}, {"--pairs",true}, {"--tidy",false}}},
  {"codebook", {{"--data",true}, {"--format",true}, {"--keepif",true}, {"--no-provenance",false}, {"--out",true}, {"--tidy",false}, {"--vars",true}}},
  {"collapse", {{"--by",true}, {"--data",true}, {"--format",true}, {"--fweight",true}, {"--keepif",true}, {"--no-provenance",false}, {"--out",true}, {"--stat",true}, {"--tidy",false}}},
  {"convert", {{"--data",true}, {"--delim",true}, {"--factor",true}, {"--format",true}, {"--no-provenance",false}, {"--out",true}, {"--tidy",false}}},
  {"corr", {{"--aweight",true}, {"--cov",false}, {"--data",true}, {"--format",true}, {"--fweight",true}, {"--keepif",true}, {"--no-provenance",false}, {"--out",true}, {"--pairwise",false}, {"--sig",false}, {"--tidy",false}, {"--vars",true}}},
  {"csdid", {{"--cluster",true}, {"--comparison",true}, {"--data",true}, {"--format",true}, {"--gvar",true}, {"--ivar",true}, {"--keepif",true}, {"--long2",false}, {"--method",true}, {"--no-provenance",false}, {"--out",true}, {"--pscoretrim",true}, {"--tidy",false}, {"--time",true}, {"--window",true}, {"--x",true}, {"--y",true}}},
  // Globals (--format/--no-provenance/--tidy) are consumed by main() for EVERY
  // command (cecon_cli.cpp:1231,1234), so the strict drift check requires them
  // declared here even though a one-line text verb has no coeftable to shape —
  // same set, same reason, as `info` below.
  {"datasignature", {{"--data",true}, {"--format",true}, {"--no-provenance",false}, {"--tidy",false}, {"--types",true}, {"--unordered",false}}},
  {"dfuller", {{"--data",true}, {"--delta",true}, {"--drift",false}, {"--format",true}, {"--keepif",true}, {"--lags",true}, {"--no-provenance",false}, {"--noconstant",false}, {"--out",true}, {"--tidy",false}, {"--timevar",true}, {"--trend",false}, {"--var",true}}},
  {"duplicates", {{"--data",true}, {"--format",true}, {"--gen",true}, {"--keepif",true}, {"--no-provenance",false}, {"--out",true}, {"--tidy",false}, {"--vars",true}}},
  {"durbinalt", {{"--data",true}, {"--format",true}, {"--keepif",true}, {"--lags",true}, {"--no-provenance",false}, {"--nomiss0",false}, {"--out",true}, {"--robust",false}, {"--small",false}, {"--tidy",false}, {"--x",true}, {"--y",true}}},
  {"dwatson", {{"--data",true}, {"--format",true}, {"--keepif",true}, {"--no-provenance",false}, {"--out",true}, {"--tidy",false}, {"--x",true}, {"--y",true}}},
  {"encode", {{"--data",true}, {"--format",true}, {"--gen",true}, {"--keepif",true}, {"--no-provenance",false}, {"--out",true}, {"--tidy",false}, {"--var",true}}},
  {"eval", {{"--data",true}, {"--detail",false}, {"--format",true}, {"--fweight",true}, {"--aweight",true}, {"--iweight",true}, {"--gen-in",true}, {"--gen-mask",true}, {"--gen-replace",false}, {"--gen-strict",false}, {"--rng-mt64",true}, {"--rng-seed",true}, {"--rng-stream",true}, {"--store-type",true}, {"--tsfmt",true}, {"--no-provenance",false}, {"--out",true}, {"--tidy",false},
            // the ordered ops cmd_eval's own scanner applies (BUG-1181)
            {"--xtset",true}, {"--tsset",true}, {"--tsdelta",true}, {"--tsma",true}, {"--gen",true}, {"--egen",true}, {"--rowegen",true}, {"--mkspline",true}, {"--bygen",true}, {"--winsor",true}, {"--xtile",true}, {"--ipolate",true}, {"--drop",true}, {"--keepif",true}, {"--summarize",true}}},
  // export-dta writes a .dta rather than an .hbin, so its --out is a Stata file path;
  // --in-meta is the symmetric counterpart of import-dta's --out-meta (both take a
  // value). The three globals ride along for the same reason as every other verb.
  {"export-dta", {{"--data",true}, {"--format",true}, {"--in-meta",true}, {"--no-provenance",false}, {"--out",true}, {"--release",true}, {"--strict-string-missing",false}, {"--tidy",false}}},
  {"feols", {{"--cluster",true}, {"--data",true}, {"--fe",true}, {"--format",true}, {"--keep-singletons",false}, {"--collinear-forward",false}, {"--keepif",true}, {"--level",true}, {"--no-intercept",false}, {"--no-provenance",false}, {"--out",true}, {"--tidy",false}, {"--tsdelta",true}, {"--tsset",true}, {"--vcov",true}, {"--x",true}, {"--xtset",true}, {"--y",true}, {"--weight",true}, {"--wtype",true}, {"--psu",true}, {"--strata",true}, {"--svy",false}, {"--esample-out",true}, {"--esample-index",true}, {"--esample-n",true}, {"--keep-nested-fe-dof",false}, {"--emit-v",false}, {"--multiway-subset-factors",false}, {"--fe-nested-in-clusters",false}, {"--regress-exact-fit",false}}},
  {"fepois", {{"--cluster",true}, {"--data",true}, {"--fe",true}, {"--format",true}, {"--keep-zero-fe",false}, {"--keepif",true}, {"--no-provenance",false}, {"--offset",true}, {"--out",true}, {"--scale",true}, {"--tidy",false}, {"--vcov",true}, {"--weight",true}, {"--wtype",true}, {"--x",true}, {"--y",true}, {"--emit-v",false}, {"--no-x",false}, {"--d-out",true}, {"--d-name",true}, {"--tsdelta",true}, {"--tsset",true}, {"--xtset",true}}},
  {"grid", {{"--data",true}, {"--format",true}, {"--no-provenance",false}, {"--out",true}, {"--tidy",false}}},
  {"gsort", {{"--data",true}, {"--format",true}, {"--keepif",true}, {"--no-provenance",false}, {"--out",true}, {"--tidy",false}, {"--vars",true}}},
  {"hausman", {{"--data",true}, {"--format",true}, {"--ivar",true}, {"--no-provenance",false}, {"--out",true}, {"--tidy",false}, {"--x",true}, {"--y",true}}},
  {"heckman", {{"--data",true}, {"--format",true}, {"--keepif",true}, {"--level",true}, {"--no-provenance",false}, {"--out",true}, {"--select",true}, {"--tidy",false}, {"--x",true}, {"--y",true}}},
  {"hettest", {{"--data",true}, {"--format",true}, {"--keepif",true}, {"--no-provenance",false}, {"--out",true}, {"--rhs",false}, {"--tidy",false}, {"--white",false}, {"--x",true}, {"--y",true}, {"--z",true}}},
  {"import-dta", {{"--data",true}, {"--format",true}, {"--no-provenance",false}, {"--out",true}, {"--out-meta",true}, {"--tidy",false}}},
  {"info", {{"--data",true}, {"--format",true}, {"--no-provenance",false}, {"--tidy",false}}},
  {"iv", {{"--cluster",true}, {"--data",true}, {"--endog",true}, {"--fe",true}, {"--format",true}, {"--gmm",false}, {"--instruments",true}, {"--keep-singletons",false}, {"--keepif",true}, {"--no-intercept",false}, {"--no-provenance",false}, {"--out",true}, {"--small",false}, {"--tidy",false}, {"--tsdelta",true}, {"--tsset",true}, {"--vcov",true}, {"--x",true}, {"--xtset",true}, {"--y",true}, {"--z",true}, {"--esample-out",true}, {"--esample-index",true}, {"--esample-n",true}, {"--emit-v",false},
          // w2b-iv-options: cmd_iv has consumed these since ivreg2 savefirst
          // (cmds_linear.cpp has()/opt()) without a row entry; PIET now forwards
          // --savefirst for `ivregress/ivreghdfe ..., first`, which the strict
          // drift check would reject as undeclared.
          {"--savefirst",false}, {"--savefprefix",true},
          // FU-387: ivregress `small` multiway variance (cmd_iv, run_iv).
          {"--multiway-subset-factors",false},
          // ivreg2 [aw=]/[pw=] and its unrepaired two-way variance (cmd_iv, run_iv).
          {"--weight",true}, {"--wtype",true}, {"--multiway-no-psd-fix",false},
          {"--dofminus-fe",false}}},
  {"iv-diag", {{"--data",true}, {"--endog",true}, {"--format",true}, {"--instruments",true}, {"--keepif",true}, {"--no-provenance",false}, {"--out",true}, {"--tidy",false}, {"--x",true}, {"--y",true}}},
  {"joinby", {{"--data",true}, {"--format",true}, {"--keepif",true}, {"--keys",true}, {"--no-provenance",false}, {"--out",true}, {"--tidy",false}, {"--using",true}}},
  {"expand", {{"--data",true}, {"--format",true}, {"--keepif",true}, {"--n",true}, {"--no-provenance",false}, {"--out",true}, {"--tidy",false}}},
  {"fillin", {{"--data",true}, {"--format",true}, {"--keepif",true}, {"--no-provenance",false}, {"--out",true}, {"--tidy",false}, {"--vars",true}}},
  {"cross", {{"--data",true}, {"--format",true}, {"--keepif",true}, {"--no-provenance",false}, {"--out",true}, {"--tidy",false}, {"--using",true}}},
  {"contract", {{"--data",true}, {"--format",true}, {"--keepif",true}, {"--no-provenance",false}, {"--out",true}, {"--tidy",false}, {"--vars",true}}},
  {"liml", {{"--data",true}, {"--vcov",true}, {"--cluster",true}, {"--endog",true}, {"--format",true}, {"--fuller",true}, {"--instruments",true}, {"--keepif",true}, {"--no-provenance",false}, {"--out",true}, {"--small",false}, {"--tidy",false}, {"--x",true}, {"--y",true}, {"--emit-v",false}}},
  // lincom/nlcom/test (and testparm, which PIET sends as `test --joint`) refit the
  // fitted model through postest_fit_spec: --model names the ML refit (BUG-203),
  // and --weight/--wtype/--keep-nested-fe-dof/--multiway-subset-factors carry the
  // fit's weight and clustered small-sample convention (BUG-571/BUG-568/FU-385),
  // and --no-intercept its `noconstant` (FU-131; margins and predict refit with it too).
  // --endog/--instruments/--small are the `--model iv` 2SLS refit's (FU-385/FU-387).
  {"lincom", {{"--asis",false}, {"--cluster",true}, {"--data",true}, {"--tsdelta",true}, {"--tsset",true}, {"--xtset",true}, {"--level",true}, {"--exp",true}, {"--fe",true}, {"--format",true}, {"--keepif",true}, {"--no-provenance",false}, {"--out",true}, {"--tidy",false}, {"--vcov",true}, {"--x",true}, {"--y",true}, {"--model",true}, {"--weight",true}, {"--wtype",true}, {"--keep-nested-fe-dof",false}, {"--multiway-subset-factors",false}, {"--regress-exact-fit",false}, {"--endog",true}, {"--instruments",true}, {"--small",false}, {"--keep-singletons",false}, {"--collinear-forward",false}, {"--no-intercept",false}, {"--tolerance",true}, {"--ltolerance",true}, {"--nrtolerance",true}, {"--iterate",true}, {"--difficult",false}}},
  {"logit", {{"--cluster",true}, {"--data",true}, {"--tsdelta",true}, {"--tsset",true}, {"--xtset",true}, {"--format",true}, {"--iterate",true}, {"--keepif",true}, {"--level",true}, {"--no-intercept",false}, {"--no-provenance",false}, {"--offset",true}, {"--out",true}, {"--tidy",false}, {"--vce",true}, {"--weight",true}, {"--wtype",true}, {"--x",true}, {"--y",true}, {"--psu",true}, {"--strata",true}, {"--svy",false}, {"--emit-v",false}, {"--tolerance",true}, {"--ltolerance",true}, {"--nrtolerance",true}, {"--difficult",false}, {"--asis",false}, {"--esample-out",true}, {"--esample-index",true}, {"--esample-n",true}}},
  {"margins", {{"--asis",false}, {"--at",true}, {"--atmeans",false}, {"--cluster",true}, {"--data",true}, {"--tsdelta",true}, {"--tsset",true}, {"--xtset",true}, {"--dydx",true}, {"--fe",true}, {"--format",true}, {"--keepif",true}, {"--marginlist",true}, {"--model",true}, {"--no-provenance",false}, {"--out",true}, {"--tidy",false}, {"--vce",true}, {"--vcov",true}, {"--weight",true}, {"--wtype",true}, {"--x",true}, {"--y",true}, {"--multiway-subset-factors",false}, {"--keep-singletons",false}, {"--collinear-forward",false}, {"--no-intercept",false}, {"--tolerance",true}, {"--ltolerance",true}, {"--nrtolerance",true}, {"--iterate",true}, {"--difficult",false}, {"--emit-v",false}}},
  {"mediate", {{"--controls",true}, {"--data",true}, {"--format",true}, {"--keepif",true}, {"--level",true}, {"--mediator",true}, {"--no-provenance",false}, {"--out",true}, {"--reps",true}, {"--seed",true}, {"--tidy",false}, {"--treat",true}, {"--y",true}}},
  {"merge", {{"--assert",true}, {"--data",true}, {"--format",true}, {"--generate",true}, {"--keep",true}, {"--keepif",true}, {"--keepusing",true}, {"--key",true}, {"--no-provenance",false}, {"--nogenerate",false}, {"--out",true}, {"--replace",false}, {"--tidy",false}, {"--type",true}, {"--update",false}, {"--using",true}}},
  {"nbreg", {{"--cluster",true}, {"--data",true}, {"--format",true}, {"--keepif",true}, {"--level",true}, {"--no-intercept",false}, {"--no-provenance",false}, {"--offset",true}, {"--out",true}, {"--tidy",false}, {"--vce",true}, {"--x",true}, {"--y",true}, {"--emit-v",false}, {"--weight",true}, {"--wtype",true}}},
  {"newey", {{"--data",true}, {"--delta",true}, {"--format",true}, {"--keepif",true}, {"--lag",true}, {"--no-provenance",false}, {"--out",true}, {"--tidy",false}, {"--timevar",true}, {"--x",true}, {"--y",true}, {"--esample-out",true}, {"--esample-index",true}, {"--esample-n",true}, {"--emit-v",false}, {"--panelvar",true}, {"--force",false}}},
  {"nlcom", {{"--asis",false}, {"--cluster",true}, {"--data",true}, {"--tsdelta",true}, {"--tsset",true}, {"--xtset",true}, {"--level",true}, {"--expr",true}, {"--fe",true}, {"--format",true}, {"--keepif",true}, {"--no-provenance",false}, {"--out",true}, {"--tidy",false}, {"--vcov",true}, {"--x",true}, {"--y",true}, {"--model",true}, {"--weight",true}, {"--wtype",true}, {"--keep-nested-fe-dof",false}, {"--multiway-subset-factors",false}, {"--regress-exact-fit",false}, {"--endog",true}, {"--instruments",true}, {"--small",false}, {"--keep-singletons",false}, {"--collinear-forward",false}, {"--no-intercept",false}, {"--tolerance",true}, {"--ltolerance",true}, {"--nrtolerance",true}, {"--iterate",true}, {"--difficult",false}}},
  {"ols", {{"--data",true}, {"--format",true}, {"--no-intercept",false}, {"--no-provenance",false}, {"--out",true}, {"--tidy",false}, {"--x",true}, {"--y",true}, {"--esample-out",true}, {"--esample-index",true}, {"--esample-n",true}}},
  {"order", {{"--after",true}, {"--before",true}, {"--data",true}, {"--first",false}, {"--format",true}, {"--keepif",true}, {"--last",false}, {"--no-provenance",false}, {"--out",true}, {"--tidy",false}, {"--vars",true}}},
  {"ovtest", {{"--data",true}, {"--format",true}, {"--keepif",true}, {"--no-provenance",false}, {"--out",true}, {"--tidy",false}, {"--x",true}, {"--y",true}}},
  {"pperron", {{"--data",true}, {"--delta",true}, {"--format",true}, {"--keepif",true}, {"--lags",true}, {"--no-provenance",false}, {"--noconstant",false}, {"--out",true}, {"--tidy",false}, {"--timevar",true}, {"--trend",false}, {"--var",true}}},
  {"prais", {{"--corc",false}, {"--data",true}, {"--delta",true}, {"--format",true}, {"--keepif",true}, {"--no-provenance",false}, {"--out",true}, {"--rhotype",true}, {"--tidy",false}, {"--timevar",true}, {"--twostep",false}, {"--x",true}, {"--y",true}, {"--esample-out",true}, {"--esample-index",true}, {"--esample-n",true}, {"--emit-v",false}, {"--panelvar",true}}},
  {"predict", {{"--asis",false}, {"--data",true}, {"--tsdelta",true}, {"--tsset",true}, {"--xtset",true}, {"--fe",true}, {"--fit-data",true}, {"--fit-keepif",true}, {"--format",true}, {"--model",true}, {"--no-provenance",false}, {"--out",true}, {"--pred",true}, {"--resid",true}, {"--store-type",true}, {"--tidy",false}, {"--x",true}, {"--xb",true}, {"--y",true}, {"--keep-singletons",false}, {"--collinear-forward",false}, {"--no-intercept",false}, {"--tolerance",true}, {"--ltolerance",true}, {"--nrtolerance",true}, {"--iterate",true}, {"--difficult",false}}},
  {"probit", {{"--cluster",true}, {"--data",true}, {"--tsdelta",true}, {"--tsset",true}, {"--xtset",true}, {"--format",true}, {"--keepif",true}, {"--level",true}, {"--no-intercept",false}, {"--no-provenance",false}, {"--offset",true}, {"--out",true}, {"--tidy",false}, {"--vce",true}, {"--weight",true}, {"--wtype",true}, {"--x",true}, {"--y",true}, {"--emit-v",false}, {"--iterate",true}, {"--tolerance",true}, {"--ltolerance",true}, {"--nrtolerance",true}, {"--difficult",false}, {"--asis",false}, {"--esample-out",true}, {"--esample-index",true}, {"--esample-n",true}}},
  {"cloglog", {{"--cluster",true}, {"--data",true}, {"--tsdelta",true}, {"--tsset",true}, {"--xtset",true}, {"--format",true}, {"--keepif",true}, {"--level",true}, {"--no-intercept",false}, {"--no-provenance",false}, {"--out",true}, {"--tidy",false}, {"--vce",true}, {"--weight",true}, {"--wtype",true}, {"--x",true}, {"--y",true}, {"--emit-v",false}, {"--iterate",true}, {"--tolerance",true}, {"--ltolerance",true}, {"--nrtolerance",true}, {"--difficult",false}, {"--asis",false}, {"--esample-out",true}, {"--esample-index",true}, {"--esample-n",true}}},
  {"pwcorr", {{"--aweight",true}, {"--cov",false}, {"--data",true}, {"--format",true}, {"--fweight",true}, {"--keepif",true}, {"--no-provenance",false}, {"--out",true}, {"--pairwise",false}, {"--sig",false}, {"--tidy",false}, {"--vars",true}}},
  {"qreg", {{"--data",true}, {"--format",true}, {"--keepif",true}, {"--level",true}, {"--no-provenance",false}, {"--out",true}, {"--tau",true}, {"--tidy",false}, {"--vce",true}, {"--weight",true}, {"--wtype",true}, {"--x",true}, {"--y",true}}},
  {"recode", {{"--data",true}, {"--format",true}, {"--gen",true}, {"--keepif",true}, {"--no-provenance",false}, {"--out",true}, {"--rules",true}, {"--tidy",false}, {"--var",true}}},
  {"reg3", {{"--data",true}, {"--emit-v",false}, {"--eq",true}, {"--exog",true}, {"--format",true}, {"--keepif",true}, {"--method",true}, {"--no-provenance",false}, {"--out",true}, {"--tidy",false}}},
  {"rename", {{"--data",true}, {"--format",true}, {"--keepif",true}, {"--no-provenance",false}, {"--out",true}, {"--pairs",true}, {"--tidy",false}}},
  {"reshape", {{"--data",true}, {"--format",true}, {"--i",true}, {"--j",true}, {"--keepif",true}, {"--no-provenance",false}, {"--out",true}, {"--stubs",true}, {"--tidy",false}}},
  {"run", {{"--data",true}, {"--format",true}, {"--no-provenance",false}, {"--out",true}, {"--tidy",false}}},
  {"sdtest", {{"--against",true}, {"--by",true}, {"--data",true}, {"--format",true}, {"--keepif",true}, {"--level",true}, {"--no-provenance",false}, {"--out",true}, {"--tidy",false}, {"--var",true}, {"--vars",true}}},
  {"setcol", {{"--data",true}, {"--dtype",true}, {"--format",true}, {"--keepif",true}, {"--new",false}, {"--no-provenance",false}, {"--ones-as",true}, {"--out",true}, {"--store-type",true}, {"--tidy",false}, {"--values",true}, {"--var",true}}},
  {"tab", {{"--data",true}, {"--format",true}, {"--fweight",true}, {"--keepif",true}, {"--no-provenance",false}, {"--out",true}, {"--tidy",false}, {"--vars",true}}},
  {"tabstat", {{"--aweight",true}, {"--by",true}, {"--data",true}, {"--format",true}, {"--fweight",true}, {"--keepif",true}, {"--no-provenance",false}, {"--out",true}, {"--stats",true}, {"--tidy",false}, {"--vars",true}}},
  {"tabulate", {{"--data",true}, {"--format",true}, {"--fweight",true}, {"--keepif",true}, {"--no-provenance",false}, {"--out",true}, {"--tidy",false}, {"--vars",true}}},
  {"test", {{"--asis",false}, {"--cluster",true}, {"--constraint",true}, {"--data",true}, {"--tsdelta",true}, {"--tsset",true}, {"--xtset",true}, {"--fe",true}, {"--format",true}, {"--joint",true}, {"--keepif",true}, {"--no-provenance",false}, {"--out",true}, {"--tidy",false}, {"--vcov",true}, {"--x",true}, {"--y",true}, {"--model",true}, {"--weight",true}, {"--wtype",true}, {"--keep-nested-fe-dof",false}, {"--multiway-subset-factors",false}, {"--regress-exact-fit",false}, {"--endog",true}, {"--instruments",true}, {"--small",false}, {"--keep-singletons",false}, {"--collinear-forward",false}, {"--no-intercept",false}, {"--tolerance",true}, {"--ltolerance",true}, {"--nrtolerance",true}, {"--iterate",true}, {"--difficult",false}}},
  // --- P7 wave: the advanced-model verbs, plus `mean`/`proportion` which had
  // shipped since P3 with no row at all (and so no drift coverage whatsoever).
  {"fracreg", {{"--cluster",true}, {"--data",true}, {"--format",true}, {"--keepif",true}, {"--level",true}, {"--link",true}, {"--no-provenance",false}, {"--out",true}, {"--tidy",false}, {"--vce",true}, {"--x",true}, {"--y",true}}},
  {"gmm", {{"--data",true}, {"--expr",true}, {"--format",true}, {"--instruments",true}, {"--keepif",true}, {"--level",true}, {"--no-provenance",false}, {"--onestep",false}, {"--out",true}, {"--tidy",false}, {"--vce",true}}},
  {"ivprobit", {{"--cluster",true}, {"--data",true}, {"--emit-v",false}, {"--endog",true}, {"--format",true}, {"--instruments",true}, {"--keepif",true}, {"--level",true}, {"--no-provenance",false}, {"--out",true}, {"--tidy",false}, {"--twostep",false}, {"--vce",true}, {"--x",true}, {"--y",true}}},
  {"ivtobit", {{"--cluster",true}, {"--data",true}, {"--emit-v",false}, {"--endog",true}, {"--format",true}, {"--instruments",true}, {"--keepif",true}, {"--level",true}, {"--ll",true}, {"--no-provenance",false}, {"--out",true}, {"--tidy",false}, {"--ul",true}, {"--vce",true}, {"--x",true}, {"--y",true}}},
  {"mean", {{"--data",true}, {"--format",true}, {"--keepif",true}, {"--level",true}, {"--no-provenance",false}, {"--out",true}, {"--psu",true}, {"--strata",true}, {"--svy",false}, {"--tidy",false}, {"--vars",true}, {"--weight",true}, {"--wtype",true}}},
  {"mi-impute-regress", {{"--add",true}, {"--data",true}, {"--format",true}, {"--keepif",true}, {"--no-provenance",false}, {"--out",true}, {"--seed",true}, {"--tidy",false}, {"--x",true}, {"--y",true}, {"--out-data",true}}},
  {"mixed", {{"--cluster",true}, {"--data",true}, {"--format",true}, {"--group",true}, {"--keepif",true}, {"--level",true}, {"--no-provenance",false}, {"--out",true}, {"--tidy",false}, {"--vce",true}, {"--x",true}, {"--y",true}}},
  {"mlogit", {{"--base",true}, {"--cluster",true}, {"--data",true}, {"--format",true}, {"--keepif",true}, {"--level",true}, {"--no-provenance",false}, {"--out",true}, {"--tidy",false}, {"--vce",true}, {"--x",true}, {"--y",true}, {"--emit-v",false}}},
  {"nl", {{"--data",true}, {"--expr",true}, {"--format",true}, {"--keepif",true}, {"--level",true}, {"--no-provenance",false}, {"--out",true}, {"--tidy",false}, {"--vce",true}, {"--weight",true}, {"--wtype",true}}},
  {"ologit", {{"--cluster",true}, {"--data",true}, {"--format",true}, {"--keepif",true}, {"--level",true}, {"--no-provenance",false}, {"--out",true}, {"--tidy",false}, {"--vce",true}, {"--x",true}, {"--y",true}, {"--emit-v",false}}},
  {"oprobit", {{"--cluster",true}, {"--data",true}, {"--format",true}, {"--keepif",true}, {"--level",true}, {"--no-provenance",false}, {"--out",true}, {"--tidy",false}, {"--vce",true}, {"--x",true}, {"--y",true}, {"--emit-v",false}}},
  {"proportion", {{"--data",true}, {"--format",true}, {"--keepif",true}, {"--level",true}, {"--no-provenance",false}, {"--out",true}, {"--tidy",false}, {"--vars",true}, {"--weight",true}, {"--wtype",true}}},
  {"stcox", {{"--cluster",true}, {"--data",true}, {"--format",true}, {"--keepif",true}, {"--level",true}, {"--no-provenance",false}, {"--out",true}, {"--tidy",false}, {"--vce",true}, {"--x",true}}},
  {"streg", {{"--cluster",true}, {"--data",true}, {"--dist",true}, {"--format",true}, {"--keepif",true}, {"--level",true}, {"--no-provenance",false}, {"--out",true}, {"--tidy",false}, {"--vce",true}, {"--x",true}}},
  {"stset", {{"--data",true}, {"--failure",true}, {"--format",true}, {"--id",true}, {"--keepif",true}, {"--no-provenance",false}, {"--out",true}, {"--tidy",false}, {"--time",true}}},
  {"var", {{"--data",true}, {"--format",true}, {"--irf-steps",true}, {"--keepif",true}, {"--lags",true}, {"--level",true}, {"--no-provenance",false}, {"--out",true}, {"--tidy",false}, {"--tsdelta",true}, {"--tsset",true}, {"--y",true}}},
  {"xtabond", {{"--cluster",true}, {"--data",true}, {"--format",true}, {"--keepif",true}, {"--lags",true}, {"--level",true}, {"--no-provenance",false}, {"--out",true}, {"--tidy",false}, {"--vce",true}, {"--x",true}, {"--xtset",true}, {"--y",true}}},
  {"zinb", {{"--cluster",true}, {"--data",true}, {"--format",true}, {"--inflate",true}, {"--keepif",true}, {"--level",true}, {"--no-provenance",false}, {"--out",true}, {"--tidy",false}, {"--vce",true}, {"--x",true}, {"--y",true}}},
  {"zip", {{"--cluster",true}, {"--data",true}, {"--format",true}, {"--inflate",true}, {"--keepif",true}, {"--level",true}, {"--no-provenance",false}, {"--out",true}, {"--tidy",false}, {"--vce",true}, {"--x",true}, {"--y",true}}},
  {"tobit", {{"--data",true}, {"--format",true}, {"--keepif",true}, {"--level",true}, {"--ll",true}, {"--no-provenance",false}, {"--out",true}, {"--tidy",false}, {"--ul",true}, {"--vce",true}, {"--x",true}, {"--y",true}, {"--emit-v",false}, {"--weight",true}, {"--wtype",true}}},
  {"tsfill", {{"--data",true}, {"--delta",true}, {"--format",true}, {"--full",false}, {"--keepif",true}, {"--no-provenance",false}, {"--out",true}, {"--panelvar",true}, {"--tidy",false}, {"--timevar",true}}},
  {"tsops", {{"--data",true}, {"--delta",true}, {"--drift",false}, {"--format",true}, {"--gen",true}, {"--keepif",true}, {"--no-provenance",false}, {"--noconstant",false}, {"--out",true}, {"--panelvar",true}, {"--tidy",false}, {"--timevar",true}, {"--trend",false}}},
  {"ttest", {{"--against",true}, {"--by",true}, {"--data",true}, {"--format",true}, {"--keepif",true}, {"--level",true}, {"--no-provenance",false}, {"--out",true}, {"--reverse",false}, {"--tidy",false}, {"--unequal",false}, {"--unpaired",false}, {"--var",true}, {"--vars",true}, {"--welch",false}}},
  {"version", {{"--data",true}, {"--format",true}, {"--json",false}, {"--no-provenance",false}, {"--tidy",false}}},
  {"vif", {{"--data",true}, {"--format",true}, {"--keepif",true}, {"--no-provenance",false}, {"--out",true}, {"--tidy",false}, {"--x",true}, {"--y",true}}},
  {"wildboot", {{"--model",true}, {"--wre",false}, {"--liml",false}, {"--small",false}, {"--fe-dof-adj",true}, {"--no-intercept",false}, {"--weight",true}, {"--wtype",true}, {"--cluster",true}, {"--data",true}, {"--endog",true}, {"--fe",true}, {"--format",true}, {"--instruments",true}, {"--keep-singletons",false}, {"--no-provenance",false}, {"--null",true}, {"--out",true}, {"--param",true}, {"--reps",true}, {"--rng-mt64",true}, {"--seed",true}, {"--tidy",false}, {"--weights",true}, {"--x",true}, {"--y",true}, {"--z",true}}},
  {"wntestq", {{"--data",true}, {"--format",true}, {"--keepif",true}, {"--lags",true}, {"--no-provenance",false}, {"--out",true}, {"--tidy",false}, {"--var",true}}},
  {"xtlogit", {{"--data",true}, {"--emit-v",false}, {"--format",true}, {"--ivar",true}, {"--keepif",true}, {"--no-provenance",false}, {"--out",true}, {"--tidy",false}, {"--vce",true}, {"--x",true}, {"--y",true}}},
  {"xtpois", {{"--data",true}, {"--emit-v",false}, {"--format",true}, {"--ivar",true}, {"--keepif",true}, {"--level",true}, {"--no-provenance",false}, {"--offset",true}, {"--out",true}, {"--tidy",false}, {"--vce",true}, {"--x",true}, {"--y",true}}},
  {"xtreg", {{"--data",true}, {"--format",true}, {"--ivar",true}, {"--keepif",true}, {"--no-provenance",false}, {"--out",true}, {"--tidy",false}, {"--x",true}, {"--y",true}, {"--esample-out",true}, {"--esample-index",true}, {"--esample-n",true}, {"--emit-v",false}, {"--be",false}, {"--level",true}}},
  // P-QE wave — RD/QE and RD-adjacent estimators (cmds_rd.cpp/cmds_eventstudy.cpp/
  // cmds_sdid.cpp/cmds_binscatter.cpp). The scaffold-time rows declared only the
  // Stata-level options(); these were corrected against the merged implementations
  // to also cover the cobra CLI's variable-naming flags (--y/--x/--ivar/...) and
  // every option a handler reads ONLY to refuse it by name (refusing an option the
  // user typed is still consuming it — the row must say so, or `cobra options
  // --json` under-reports it to PIET).
  {"rdrobust", {{"--b",true}, {"--bwselect",true}, {"--c",true}, {"--cluster",true}, {"--covs",true}, {"--data",true}, {"--format",true}, {"--fuzzy",true}, {"--h",true}, {"--keepif",true}, {"--kernel",true}, {"--level",true}, {"--masspoints",true}, {"--nnmatch",true}, {"--no-provenance",false}, {"--nomasspoints",false}, {"--out",true}, {"--p",true}, {"--q",true}, {"--tidy",false}, {"--vce",true}, {"--x",true}, {"--y",true}}},
  {"rddensity", {{"--bwselect",true}, {"--c",true}, {"--cluster",true}, {"--covs",true}, {"--data",true}, {"--format",true}, {"--fuzzy",true}, {"--h",true}, {"--keepif",true}, {"--kernel",true}, {"--masspoints",true}, {"--no-provenance",false}, {"--nomasspoints",false}, {"--out",true}, {"--p",true}, {"--q",true}, {"--tidy",false}, {"--vce",true}, {"--x",true}}},
  {"rdplot", {{"--binselect",true}, {"--c",true}, {"--cluster",true}, {"--covs",true}, {"--data",true}, {"--format",true}, {"--fuzzy",true}, {"--h",true}, {"--keepif",true}, {"--kernel",true}, {"--masspoints",true}, {"--nbins",true}, {"--no-provenance",false}, {"--nomasspoints",false}, {"--out",true}, {"--p",true}, {"--scale",true}, {"--support",true}, {"--tidy",false}, {"--x",true}, {"--y",true}}},
  {"binscatter", {{"--absorb",true}, {"--by",true}, {"--controls",true}, {"--data",true}, {"--discrete",false}, {"--double",false}, {"--format",true}, {"--genxq",false}, {"--inrange",true}, {"--keepif",true}, {"--linetype",true}, {"--medians",false}, {"--no-provenance",false}, {"--noaddmean",false}, {"--nquantiles",true}, {"--out",true}, {"--reportreg",false}, {"--tidy",false}, {"--weight",true}, {"--wtype",true}, {"--x",true}, {"--y",true}}},
  {"eventstudyinteract", {{"--absorb",true}, {"--cluster",true}, {"--cohort",true}, {"--control_cohort",true}, {"--data",true}, {"--format",true}, {"--keepif",true}, {"--level",true}, {"--no-provenance",false}, {"--out",true}, {"--rel",true}, {"--tidy",false}, {"--vce",true}, {"--y",true}, {"--weight",true}, {"--wtype",true}}},
  {"did_imputation", {{"--autosample",false}, {"--cluster",true}, {"--data",true}, {"--format",true}, {"--gvar",true}, {"--horizons",true}, {"--ivar",true}, {"--keepif",true}, {"--level",true}, {"--no-provenance",false}, {"--out",true}, {"--pretrends",true}, {"--tidy",false}, {"--time",true}, {"--weight",true}, {"--y",true}}},
  {"sdid", {{"--data",true}, {"--format",true}, {"--ivar",true}, {"--keepif",true}, {"--level",true}, {"--method",true}, {"--no-provenance",false}, {"--out",true}, {"--reps",true}, {"--seed",true}, {"--tidy",false}, {"--time",true}, {"--treated",true}, {"--vce",true}, {"--x",true}, {"--y",true}}},
  {"hpfilter", {{"--data",true}, {"--format",true}, {"--keepif",true}, {"--lambda",true}, {"--no-provenance",false}, {"--out",true}, {"--panel",true}, {"--tidy",false}, {"--tsfmt",true}, {"--y",true}}},
  {"lowess", {{"--bw",true}, {"--data",true}, {"--emit-touse",false}, {"--format",true}, {"--inrange",true}, {"--keepif",true}, {"--mean",false}, {"--no-provenance",false}, {"--noweight",false}, {"--out",true}, {"--tidy",false}, {"--x",true}, {"--y",true}}},
  {"lpoly", {{"--at",true}, {"--bw",true}, {"--data",true}, {"--degree",true}, {"--emit-touse",false}, {"--format",true}, {"--inrange",true}, {"--keepif",true}, {"--kernel",true}, {"--n",true}, {"--no-provenance",false}, {"--out",true}, {"--pwidth",true}, {"--se",false}, {"--tidy",false}, {"--weight",true}, {"--wtype",true}, {"--x",true}, {"--y",true}}},
  {"pca", {{"--comp",true}, {"--correlation",false}, {"--covariance",false}, {"--data",true}, {"--format",true}, {"--keepif",true}, {"--mineigen",true}, {"--no-provenance",false}, {"--out",true}, {"--tidy",false}, {"--x",true}}},
    };
    return T;
}

// `cobra options` — dump the accepted-flag table. `--json` gives the machine form
// PIET loads at startup for its pre-forward validation; bare form is human-readable.
int cmd_options(const std::vector<std::string>& a) {
    const auto& T = options_table();
    if (has(a, "--json")) {
        json o;
        o["contract_version"] = CONTRACT_VERSION;
        o["cobra_version"] = COBRA_VERSION;
        json cmds = json::object();
        for (const auto& kv : T) {
            json flags = json::array();
            for (const auto& s : kv.second)
                flags.push_back({{"flag", s.flag}, {"takes_value", s.takes_value}});
            cmds[kv.first] = flags;
        }
        o["commands"] = cmds;
        std::cout << o << "\n";
    } else {
        for (const auto& kv : T) {
            std::printf("%s:", kv.first.c_str());
            for (const auto& s : kv.second) std::printf(" %s", s.flag);
            std::printf("\n");
        }
    }
    return 0;
}

// `cobra functions` — dump the function vocabulary the parsers actually accept, so a
// corpus census can ask "does the engine know this name?" without restating the rule.
// Two arms: the expression functions (--gen/--keepif) and, with --egen, the egen
// families. Both read the same tables the parsers resolve against, so the answer
// cannot drift from the behaviour.
//
// The JSON form is a BARE ARRAY, deliberately unlike `options --json`'s
// {contract_version, cobra_version, commands} envelope: this is a list of rows a
// consumer iterates, with no per-call metadata to carry, and `jq length` on it is the
// count rather than the field count of a wrapper.
int cmd_functions(const std::vector<std::string>& a) {
    const bool egen = has(a, "--egen");
    if (has(a, "--json")) {
        json arr = json::array();
        if (egen) {
            for (const EgenFnInfo& f : egen_function_table())
                arr.push_back({{"name", f.name}, {"kind", f.kind}});
        } else {
            for (const ExprFnInfo& f : expr_function_table())
                arr.push_back({{"name", f.name}, {"min_arity", f.min_arity},
                               {"max_arity", f.max_arity}});
        }
        std::cout << arr << "\n";
    } else if (egen) {
        for (const EgenFnInfo& f : egen_function_table())
            std::printf("%s\t%s\n", f.name, f.kind);
    } else {
        for (const ExprFnInfo& f : expr_function_table())
            std::printf("%s\t%d\t%d\n", f.name, f.min_arity, f.max_arity);
    }
    return 0;
}

}  // namespace cli
}  // namespace cecon
