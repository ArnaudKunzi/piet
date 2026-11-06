// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// M2 vertical-slice command handlers — C++ ports of the corresponding
// native_handler branches in piet/backend/piet/dispatcher.py's _run_native()
// plus the cobra-estimator path (dispatch()'s "4b. Cobra estimator" section)
// for `regress`/`reg`/`feols`.
//
// Scope (ROADMAP M2): use, gen, replace, describe, summarize, regress. Each
// handler mirrors the oracle's behavior BUG-FOR-BUG (see use's BUG-023 note)
// and returns a single ResultBlock (nlohmann::ordered_json object) — the
// caller (dispatcher.cpp) appends it after the echo block, exactly like
// Python's `blocks.append(result_block)`.
//
// Handlers that hit a case outside the vertical slice (string-typed gen,
// bysort _n/_N gen, an if-filtered describe, …) return an explicit
// unsupported_* error block rather than attempting a partial port — M3 is
// where those land (see each handler's own comment for the exact carve-out).
#ifndef PIET_NATIVE_HANDLERS_H
#define PIET_NATIVE_HANDLERS_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "piet/eval/tokenizer.h"
#include "piet/session_state.h"
#include "piet/twoway_parse.h"
#include "vendor/nlohmann/json.hpp"

namespace piet::handlers {

// Bounded, expression-compiled generic lf likelihoods; no evaluator execution.
nlohmann::ordered_json runMl(const eval::ParsedCommand& parsed, SessionState& session);

// `use FILEPATH [, clear]` — load a dataset (dta/csv/hbin) into the session.
// M3b: .dta loads via cobra 0.18.0's native `import-dta` (formats 115/117/
// 118) — value-labeled numerics stay f64 raw codes, genuine strings become
// factor columns; labels/formats/value-labels applied to SessionState from
// the --out-meta sidecar (mirrors routes_data.py's _convert_to_hbin +
// _apply_dta_var_labels).
// BUG-023 (bug-for-bug, frozen 1.16.0 oracle, the internal bug ledger):
// unconditionally rewrites session.cwd to the loaded file's directory. Fixing
// this is an explicit 2.0-scope decision deferred to M3/M4 adjudication +
// Eval corpus regeneration — NOT this milestone's call.
nlohmann::ordered_json runUse(const std::string& filepathArg, SessionState& session);

// `generate NEWVAR = EXPR [if COND]` — the NUMERIC route: `cobra eval --gen`.
// The dispatcher still detects string-typed (str#/strL) and bysort `_n`/`_N`
// expressions before this handler and routes them elsewhere — but they are no
// longer OUT OF SCOPE, which is what the TODO(M3) here used to say: they land in
// `strmutate::runStringGen` and `egen::runBysortGen`. Marker retired 2026-08-18
// (INSPECT-2608 census); the routing split it describes is current, the
// "unsupported_command" half was stale.
// `storeType` (BUG-471 T2): the resolved Stata storage type the engine stores
// the result as (`--store-type`); "" or "double" = the full double.
nlohmann::ordered_json runGen(const std::string& expr, const std::optional<std::string>& ifExpr,
                               SessionState& session, const std::string& storeType = "");

// `replace VARNAME = EXPR [if COND]` — numeric-only vertical slice, mirrors
// mutate.py's run_replace (cond()-based in-place rewrite via cobra eval).
// `storeType` (BUG-471 T2) is the variable's resolved type; an integer type
// widens by Stata's replace lattice unless `noPromote` (piet/storage_type.h).
nlohmann::ordered_json runReplace(const std::string& expr, const std::optional<std::string>& ifExpr,
                                   SessionState& session, const std::string& storeType = "",
                                   bool noPromote = false);

// `describe` / `desc` — VarInfo table, C++ port of describe.py's run_describe.
nlohmann::ordered_json runDescribe(const std::optional<std::vector<std::string>>& varlist,
                                    SessionState& session);

// `summarize` / `sum [varlist] [if] [, detail]` — routes numeric vars to
// `cobra eval --summarize`, C++ port of summarize.py's run_summarize.
// `if COND` filtering: when ifExpr is set, the row filter is applied FIRST via
// `cobra eval --keepif COND` to a transient hbin (same validated keepif the
// data verbs use), then the ordinary summarize runs over the filtered data —
// so the numeric path (cobra --summarize) and the factor/fallback path both
// see the restricted sample uniformly (Doctrine ①: the qualifier reaches the
// engine, never silently dropped).
// `weightVar` (Step-1 D3): when set (tokenizer only lets this through for
// `[fw=]`/`[fweight=]`), forwarded as `--fweight weightVar` to the cobra call.
// gate-①: if a weight is set and the cobra call fails, the native numpyStats
// fallback is skipped and the cobra error is returned instead — an unweighted
// answer to a weighted question would be a silent-wrong-number parity bug.
// `inRange` (BS-090, BETA Phase G): `in a/b` row-range, composed with `ifExpr`
// via AND (piet::handlers::applyIfAndInFilter) before either the numeric
// cobra --summarize path or the factor/fallback path ever sees the data —
// same "restrict first, then compute" shape the `if`-only path already used.
nlohmann::ordered_json runSummarize(const std::optional<std::vector<std::string>>& varlist,
                                     bool detail, SessionState& session,
                                     const std::optional<std::string>& ifExpr = std::nullopt,
                                     const std::optional<std::string>& weightVar = std::nullopt,
                                     const std::optional<std::string>& inRange = std::nullopt);

// BS-092 (BETA campaign, Phase G): the pure per-Datastore stats computation
// runSummarize uses once it has resolved which Datastore to read — exposed
// so dispatcher.cpp's `by:`/`bysort: summarize` grouped-dispatch loop can
// invoke the EXACT SAME computation once per by-group's own scratch slice.
nlohmann::ordered_json summarizeCoreStats(Datastore& ds,
                                          const std::optional<std::vector<std::string>>& varlist,
                                          bool detail,
                                          const std::optional<std::string>& weightVar = std::nullopt);

// `regress`/`reg`/`feols`/`reghdfe` y x1 x2 [, robust|cluster(v)|noconstant|
// level(#)|absorb(...)] — C++ port of the classify()->build_estimation_argv()
// ->cobra feols->_cobra_to_estimation() pipeline, IV-free vertical slice
// (plain OLS/feols/HDFE with iid, hc1, or cluster vcov). M3a: TS-context
// injection (translate.cpp) wired in. The TODO(M3b) that sat here said IV/xtreg
// routing was still out of scope; both have their own registry rows and handlers
// (estimation_m4.cpp), so the marker was retired 2026-08-18 (INSPECT-2608
// census). THIS function stays the IV-free route — that part was never stale.
// `stataCmd` is the ORIGINAL Stata command word (defaults to "regress").
// It is display-label-only: it never changes the cobra invocation (still
// always "feols", since cobra has no separate reghdfe estimator — reghdfe
// IS feols --fe) nor LastModel.cobraCmd (re-estimation/eststo/estat replay
// must keep invoking cobra's real "feols" binary). It only lets the
// resulting block's "estimator" field read "reghdfe" when the user typed
// `reghdfe`, matching Stata's own output label.
// `dataOverride` (P4 wave): when non-null, fit against THIS hbin path instead of
// the session's current dataset, and skip the internal `if`/`in` materialization
// — the caller has already produced a slice with those qualifiers applied. The
// only caller is the `by:`/`bysort:` group driver, which must run one fit per
// contiguous group; passing the group's own slice is what makes each fit see its
// group's sample and nothing else. nullptr (the default) is the ordinary path,
// byte-unchanged.
// `residMutationOut` (W2 estimopts, BUG-539): reghdfe's `residuals[(newvar)]`
// adds a column to the live frame AFTER the fit. The handler still returns the
// one estimation block; the data_mutation announcing that column is written
// here for the caller to push as its own block. nullptr means the caller
// cannot carry a second block (the by: driver) and residuals() is refused.
nlohmann::ordered_json runRegress(const eval::ParsedCommand& parsed, SessionState& session,
                                   const std::string& stataCmd = "regress",
                                   const std::string* dataOverride = nullptr,
                                   nlohmann::ordered_json* residMutationOut = nullptr);

// -- M3a: handler-surface expansion (ROADMAP M3a item 4) ---------------------

// One tsset/xtset declaration (`tsset [panelvar] timevar` and the two-variable
// `xtset`). Validates the variables, the format, the time values and delta,
// sorts by (panelvar) timevar, installs session.panelvar/timevar/delta + the
// _dta chars and posts the declaration's r() set under `rCmd`. BUG-896.
struct TsDeclare {
  std::optional<std::string> panelvar;  // nullopt = time series only
  std::string timevar;
  std::optional<double> delta;          // delta(#); nullopt = 1
  std::string period;                   // "yearly", ... ("" = none)
  std::string format;                   // format(%fmt) ("" = none)
  // The display options tsset and xtset take: panelname()
  // relabels the panel line and displayindent() indents every line, each label
  // right-aligned to the longest; noblank and mi change nothing this build draws
  // (it draws no blank line). `display` = one was typed.
  std::string panelLabel = "Panel";
  int indent = 0;
  bool display = false;
};
// Every declaration answers with the panel/time/delta report lines (BUG-1161).
nlohmann::ordered_json runTsDeclare(const TsDeclare& d, SessionState& session,
                                     const std::string& rCmd);
// Bare `tsset` / `xtset`: re-run the live declaration (Stata's echo and r()).
// `given` carries the options typed with the bare command; `anyOption` = some
// period/format/delta option was typed (then only those are used, as in Stata).
nlohmann::ordered_json runTsRedisplay(SessionState& session, const std::string& rCmd,
                                       const TsDeclare& given, bool anyOption);
// The panel-only form: `xtset ivar` (declare) or its bare re-display. `look`
// carries the display options (TsDeclare's last three fields).
nlohmann::ordered_json runXtsetPanelOnly(const std::string& ivar, SessionState& session,
                                          bool declare, const TsDeclare& look = TsDeclare{});

// `xtset ivar [tvar] [, delta(#)]` — the two-variable form is runTsDeclare
// (validated like tsset since BUG-896); one variable is runXtsetPanelOnly.
nlohmann::ordered_json runXtset(const std::string& ivar, const std::optional<std::string>& tvar,
                                 SessionState& session, std::optional<double> delta = std::nullopt);

// `drop VARLIST` / `keep VARLIST` (no `if`) — mutate.py's run_drop/run_keep.
nlohmann::ordered_json runDrop(const std::vector<std::string>& varlist, SessionState& session);
nlohmann::ordered_json runKeep(const std::vector<std::string>& varlist, SessionState& session);

// `drop if COND [in RANGE]` / `keep if COND [in RANGE]` — numeric-only
// vertical slice (routes to cobra eval --keepif); string/factor-referencing
// conditions are OUT of the M3a slice (strmutate.py's row-wise
// filter_rows_if is M3b). `ifExpr` may be an empty string when only
// `inRange` is present (BS-090, BETA Phase G) — piet::handlers::
// applyIfAndInFilter treats an empty ifExpr as absent, same as everywhere
// else in this codebase.
nlohmann::ordered_json runDropIf(const std::string& ifExpr, SessionState& session,
                                  const std::optional<std::string>& inRange = std::nullopt);

// BUG-532: `by BYVARS [(SORTVARS)]: keep if COND` (drop=false) / `drop if COND`
// (drop=true). COND is evaluated per by-group (group-local _n/_N, x[k], sum())
// in the rows' PHYSICAL order. `stataSortKeys` empty: the caller has checked
// that the rows are already in BYVARS++SORTVARS order (plain `by`, or a
// `bysort` with nothing to do). Non-empty: the rows are first sorted in place on
// those keys in STATA's order (stringKeyOrder below), the `bysort` half. One
// fused `eval --bygen --keepif --drop` call does the rest. A missing COND counts
// as true, as in Stata (COND's own `!`, `&`, `|` keep the engine's reading of
// a missing operand -- BUG-620, an engine fix). A comparison of a string
// variable with the blank literal "" is evaluated PIET-side (the engine folds
// `s==""` to a constant whenever "" is not a dictionary level — BUG-532); any
// other string condition is refused by name, r(199).
nlohmann::ordered_json runByKeepIf(SessionState& session, const std::string& ifExpr,
                                    const std::vector<std::string>& byVars,
                                    const std::vector<std::string>& stataSortKeys, bool drop);

// BUG-532: Stata orders a string by its bytes, the engine orders a factor by
// its dictionary CODE (first-seen order; a -1 code, a blank read from .dta or
// CSV, first). StringKeyOrder maps each code to the rank of its level in byte
// order: `rank[code]` for code >= 0, `blankRank` for code -1 (Stata reads it
// as ""). `codeOrderIsStata`: the dictionary is already strictly byte-ordered
// and has no "" level, so the engine's code order IS Stata's order.
// `blankLevel`: "" is also a level, so a -1 row and a "" row are one Stata
// value under two codes (a by-group the engine would split).
struct StringKeyOrder {
  std::vector<double> rank;
  double blankRank = 0;
  bool codeOrderIsStata = true;
  bool blankLevel = false;
};
StringKeyOrder stringKeyOrder(const std::vector<std::string>& levels);

nlohmann::ordered_json runKeepIf(const std::string& ifExpr, SessionState& session,
                                  const std::optional<std::string>& inRange = std::nullopt);

// `sort VARLIST` — editor.py's run_sort (ascending, stable). BUG-532: when the
// head's sort claim already begins with VARLIST the rows are not touched and
// the (longer) claim is kept, Stata's rule ([D] sort: a sort the data already
// satisfy does nothing).
nlohmann::ordered_json runSort(const std::vector<std::string>& varlist, SessionState& session);

// BUG-532: Stata's `bysort KEYS [(SORT)]:` half — sort the data in place on
// `keys` (KEYS then SORT) in STATA's order and leave them marked sorted by
// `keys`. A string key whose dictionary is not byte-ordered is sorted through a
// byte-rank scratch column (stringKeyOrder, the runByKeepIf mechanism; the
// engine's own sort orders a factor by its first-seen code) — the engine then
// mints no claim, as for every string key; every other key list goes through
// runSort. The engine's gsort moves no row when the marker already begins with
// `keys`, and the marker becomes `keys` (Frame::gsort; w2e_probes.log
// bo_sort_prefix_noop). Returns the error block, or nullopt.
std::optional<nlohmann::ordered_json> sortInStataOrder(SessionState& session,
                                                        const std::vector<std::string>& keys);

// BS-092 (BETA campaign, Phase G): NON-MUTATING sibling of runSort — sorts
// `basePath` by `byVars` into a fresh scratch .hbin without touching
// session state (unlike runSort/runGsort, which session.load() the
// result). Throws CobraError on failure. See the .cpp comment for why a
// read-only `by:`/`bysort:` grouped-dispatch loop needs this instead of the
// mutating sort handlers.
std::string sortForGrouping(SessionState& session, const std::string& basePath,
                            const std::vector<std::string>& byVars);

// `count [if COND] [in RANGE]` — numeric-only vertical slice (cobra eval
// --keepif then row count); string-referencing conditions are OUT of the
// M3a slice. `inRange` (BS-090, BETA Phase G) composes with `ifExpr` via AND
// (piet::handlers::applyIfAndInFilter).
nlohmann::ordered_json runCount(const std::optional<std::string>& ifExpr, SessionState& session,
                                 const std::optional<std::string>& inRange = std::nullopt);

// `assert EXP [if COND] [in RANGE]` (P4 wave) — Stata's claim-verifier. Silent
// on success; on failure reports the contradiction count and errors with r(9).
// Deliberately implemented ON TOP of runCount (`count if !(EXP)`) rather than
// with its own evaluator: `count if` already routes through BOTH the native
// CiParser fast path and the cobra keepif fallback, so assert inherits whatever
// those two agree on. A second expression evaluator here would be a second place
// for the two layers to disagree — the exact defect class that makes a wrong
// answer silent.
nlohmann::ordered_json runAssert(const std::string& expr, SessionState& session,
                                 const std::optional<std::string>& ifExpr = std::nullopt,
                                 const std::optional<std::string>& inRange = std::nullopt);

// `list [VARLIST] [if COND] [in RANGE]` — browse.py's run_browse in list mode
// (numeric-only if/in; factor-referencing conditions are OUT of scope).
nlohmann::ordered_json runList(const std::optional<std::vector<std::string>>& varlist,
                                const std::optional<std::string>& ifExpr,
                                const std::optional<std::string>& inRange, SessionState& session);

// `scalar NAME = EXPR` is captured by the macro layer (session.eval.scalars)
// — no separate handler needed; see dispatcher.cpp's try_capture_definition
// call. `scalar list [NAME]` / `scalar dir` ARE separate handlers:
nlohmann::ordered_json runScalarList(const std::vector<std::string>& names, SessionState& session);

// `ereturn list` (BS-039) — display-only dump of the e() values PIET
// actually stores after an estimation (session.lastModel()). The other
// sub-verbs are runEreturn's (below).
nlohmann::ordered_json runEreturnList(SessionState& session);

// W2D-T01 (handlers_ereturn.cpp): every `ereturn` sub-verb except `list` --
// `clear` (empties every e() store), `post [b [V]] [, obs() depname() dof()
// properties()]` (a NEW e(); esample() is refused by name until PIET keeps an
// e(sample) mask), `local NAME ...`, and a named refusal for the rest. Returns
// one error block, or an empty array on silent success.
nlohmann::ordered_json runEreturn(const eval::ParsedCommand& parsed, SessionState& session);

// `matrix NAME = e(b)` / `matrix NAME = e(V)` — capture the last estimation's
// coefficient vector / diagonal se^2 matrix into session.eval.matrices. Silent
// on success; other matrix forms return an explicit unsupported error.
nlohmann::ordered_json runMatrixDef(const std::string& raw, SessionState& session);

// A row or column name as `matrix list` and `matlist` print it: a factor
// level's `bn` marker is not shown (StataBE lists `2bn.g` as `2.g`, while
// `: colnames` keeps `2bn.g`).
std::string matrixNameAsListed(const std::string& name);

// `label variable VAR "text"` / `label values VAR [LBLNAME]` — session
// var-editor metadata (label.py's run_label / label_define.py's
// apply_label_values). lblname=nullopt (or empty) detaches the label.
nlohmann::ordered_json runLabelVariable(const std::string& varname, const std::string& text,
                                         SessionState& session);
nlohmann::ordered_json runLabelValues(const std::string& varname,
                                       const std::optional<std::string>& lblname,
                                       SessionState& session, bool nofix = false);

// `format VARLIST %fmt` — BS-005/D6 (BETA B-data): sets a display-layer
// session attribute (varMeta[name].format) honored by browse/list; NOT
// persisted into hbin (post-beta P1/hbin-v2). Unknown/malformed %fmt is an
// honest reject. See handlers_m3.cpp's runFormat for the recognized format
// grammar (numeric/string/date families — not a full typed-format engine).
nlohmann::ordered_json runFormat(const std::vector<std::string>& varlist, const std::string& fmt,
                                  SessionState& session);
// The %fmt grammar `format` accepts (anything else is r(120) "invalid
// %format"), and whether a valid one is a string format (%#s, %-#s, %~#s).
// `file write` reads the same two rules.
bool isValidDisplayFormat(const std::string& fmt);
bool isStringDisplayFormat(const std::string& fmt);

// `label define LBLNAME # "text" ... [, add|modify]` / `label list
// [LBLNAME...]` / `label dir` / `label drop LBLNAME...` — M3b, C++ port of
// label_define.py.
nlohmann::ordered_json runLabelDefine(const std::string& rawLine, SessionState& session);
nlohmann::ordered_json runLabelList(const std::vector<std::string>& lblnames, SessionState& session);
nlohmann::ordered_json runLabelDir(SessionState& session);
nlohmann::ordered_json runLabelDrop(const std::vector<std::string>& lblnames, SessionState& session);
// `label language [NAME] [, new copy delete rename]` (LBL-LANGUAGE).
nlohmann::ordered_json runLabelLanguage(const std::vector<std::string>& args,
                                         const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                         SessionState& session);

// `labelbook [LBLNAME...]` — P1-W2-0: direct-read report over
// session.labelDefs/valueLabels/varLblname (no engine change — the data
// already arrives via `use`'s --out-meta sidecar, session_state.h:1054-1067).
// Scope = label set + definition + attached variables, NOT Stata's full
// diagnostic block (Range/Gaps/Unique/Missing — those need a data scan this
// direct-read report does not perform).
nlohmann::ordered_json runLabelbook(const std::vector<std::string>& lblnames, SessionState& session);

// `datasignature` — the engine's content fingerprint, forwarded verbatim.
// Stata-SHAPED (`<rows>:<cols>(<meta>):<a>:<b>`) but deliberately NOT
// Stata-comparable: Stata publishes no algorithm for its two checksums and its
// parenthetical term is not self-consistent across fixtures, so the engine
// pins the CONTRACT instead (deterministic · reorder/value/narrowing-sensitive ·
// format-insensitive) — see src/data/datasignature.h's Q9 ruling. The reported
// string is the engine's own; do not present it as a Stata cross-check.
nlohmann::ordered_json runDatasignature(SessionState& session);
// BUG-1461: `datasignature set [, reset]`, `confirm [, strict]` and `clear`, the
// signature kept in the dataset's _dta[datasignature_*] characteristics.
nlohmann::ordered_json runDatasignatureSub(
    const std::string& sub, const std::vector<std::pair<std::string, eval::OptValue>>& opts,
    SessionState& session);

// `preserve` / `restore` — preserve_restore.py's LIFO hbin-snapshot stack.
// `restore, preserve` (B5-D14, BUG-454): `rearm` restores AND re-snapshots,
// so a following `restore` can pop the same state again.
nlohmann::ordered_json runPreserve(SessionState& session);
nlohmann::ordered_json runRestore(SessionState& session, bool rearm = false);

// `tempfile NAME [NAME...]` / `tempname NAME [NAME...]` — S5/U4, silent on
// success (returns an EMPTY array — caller treats non-empty as error).
nlohmann::ordered_json runTempfile(const std::vector<std::string>& names, SessionState& session);
nlohmann::ordered_json runTempname(const std::vector<std::string>& names, SessionState& session);
// `tempvar NAME...` (BS-037) — sibling of tempfile/tempname above: mints
// unique, LEGAL, collision-free new variable names bound as local macros.
nlohmann::ordered_json runTempvar(const std::vector<std::string>& names, SessionState& session);

// `levelsof VAR [, local(NAME) clean]` — distinct sorted values; numeric
// vertical slice (factor/string levelsof is M3b).
nlohmann::ordered_json runLevelsof(const std::string& varname,
                                    const std::optional<std::string>& localName, bool clean,
                                    const std::optional<std::string>& ifExpr, SessionState& session);

// `clear [subcmd]` — BS-044 (BETA campaign C4): `sub` is the lowered
// subcommand token (""/"all"/"*"/"results"/"matrix"/"programs"/"mata"/
// "ado"/"rngstream"/"rngstreams"/"frames"/"collect"/anything else); each
// gets its OWN scoped clear (bare drops only the dataset, `all` drops
// everything, `results`/`matrix` drop e()/r() state only, etc.) — see the
// .cpp definition for the full per-subcommand behavior.
nlohmann::ordered_json runClear(const std::string& sub, SessionState& session);

// `set obs N` — construct/extend the dataset to N observations.
nlohmann::ordered_json runSetObs(const std::string& nArg, SessionState& session);

// `cd [DIR]` / `pwd` — working-directory management.
nlohmann::ordered_json runCd(const std::string& dirArg, SessionState& session);
nlohmann::ordered_json runPwd(SessionState& session);

// `log using FILE [, replace|append] [text]` / `log close|off|on` (BUG-028,
// BS-006). Really opens FILE now and tees every subsequent outermost
// command's output to it as a plain-text transcript (SessionState::openLog/
// logTee; see this function's own doc comment in handlers_m3.cpp for the
// replace/append/text-vs-smcl rules). Returns an empty array (no block —
// Stata is silent on a successful log open/close/off/on), matching the
// oracle's `return None` handler convention; a single ojson object on
// failure (e.g. unknown option, file already exists, smcl requested).
nlohmann::ordered_json runLog(const std::string& raw, SessionState& session);

// U7/S9 offline shims (mirrors piet/backend/piet/shims.py) — package-
// presence / ssc-install / file-existence / directory / file-delete stubs
// so classroom do-files (e.g. analysis.do's `capture which drdid` /
// `capture confirm variable ...` guards) never halt on these no-data ops.
nlohmann::ordered_json runWhich(const std::string& raw, const std::vector<std::string>& varlist);
nlohmann::ordered_json runSsc(const std::vector<std::string>& varlist);
nlohmann::ordered_json runConfirm(const std::string& raw, const std::vector<std::string>& varlist,
                                   SessionState& session);
nlohmann::ordered_json runMkdir(const std::string& raw, const std::vector<std::string>& varlist,
                                 const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                 SessionState& session);
// BUG-180 (P2-PREP): `rmdir DIR` — remove one EMPTY directory. Same shape as
// runMkdir (it takes opts only so the shared reject net can refuse them —
// real Stata's rmdir has no options).
nlohmann::ordered_json runRmdir(const std::string& raw, const std::vector<std::string>& varlist,
                                 const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                 SessionState& session);
nlohmann::ordered_json runErase(const std::string& raw, const std::vector<std::string>& varlist,
                                 SessionState& session);

// ---- AER-SCOPE1 bundle C (GTOOLS, 2026-09-03) ------------------------------
// gtools/ftools are drop-in fast replacements for commands PIET already has, so
// the campaign spends ONE translation step rather than ten reimplementations.
// The shim drops options that only describe SPEED (`fast`, `verbose`), renames
// the ones that differ only in spelling, and REFUSES by name the ones that would
// change the answer (`gcollapse, merge` is not a collapse; `gquantiles, by()`
// wants per-group cutpoints). What it never does is forward an option the native
// command would silently ignore.
struct GtoolsTranslation {
  eval::ParsedCommand parsed;   // rewritten: canonical head, translated options
  std::string canonical;        // the native command to dispatch as
  nlohmann::ordered_json error; // non-empty -> refuse with this block
};
bool isGtoolsAlias(const std::string& canonical);
// Name-only fold, for the by-prefix path: `bysort g: gegen ...` has to reach the
// egen by()-injection branch, which compares the sub-command name before any
// translation has happened.
std::string gtoolsCanonicalName(const std::string& canonical);
GtoolsTranslation translateGtoolsAlias(const eval::ParsedCommand& parsed,
                                        const std::string& canonical, SessionState& session);
// `gquantiles x, _pctile nq(#)` / `pctile new = x, nq(#)` -- the one form with no
// native twin. The percentile rule is transcribed from the engine's own
// stata_pctile so the two cannot disagree.
nlohmann::ordered_json runGQuantilesPctile(const eval::ParsedCommand& parsed,
                                            SessionState& session);
// `gstats winsor|sum` -- a multiplex, not an alias: the two sub-commands forward
// to different native handlers and the rest are refused by name.
nlohmann::ordered_json runGstats(const eval::ParsedCommand& parsed, SessionState& session);

// ---- AER-SCOPE1 bundle B (RESULTS, 2026-09-03) -----------------------------
// r() made visible, and the matrix<->dataset round trip.
//
// `return list` prints what `ereturn list` prints, from the other store, so it
// reuses that handler's formatter rather than growing a second one that can
// drift from it (handlers_m3.cpp::runEreturnList).
nlohmann::ordered_json runReturnList(SessionState& session);
// `svmat [type] A [, names(col|eqcol|matcol|stub)]` -- matrix columns become
// variables. Extends _N when the matrix has more rows than the dataset, which
// is Stata's documented behaviour and the reason this cannot be a pure read.
nlohmann::ordered_json runSvmat(const eval::ParsedCommand& parsed, SessionState& session);
// `getmata getlist [, double force replace]` -- Mata vectors/matrices from the
// SEPARATE Mata store (mata_subset.h) become variables, with getmata.ado's rc
// rules (111 not found, 102/103 name count, 110 exists, 459 rows != _N) and
// its r(K_new)/r(K_existing). `update` and `id()` are refused by name. Always
// returns exactly one object.
nlohmann::ordered_json runGetmata(const eval::ParsedCommand& parsed, SessionState& session);
// `mkmat varlist [if] [in], matrix(M) [rownames(v)] [nomissing] [obs]` -- the
// other direction.
nlohmann::ordered_json runMkmat(const eval::ParsedCommand& parsed, SessionState& session);
// `matlist EXP [, format(%fmt) ...]` where EXP is a matrix name or e(b)/e(V).
nlohmann::ordered_json runMatlist(const eval::ParsedCommand& parsed, SessionState& session);

// ---- AER-SCOPE1 bundle E (TABLES, 2026-09-04) ------------------------------
// The `regress -> estadd -> eststo -> estout|esttab using f.tex` pipeline, plus
// the other table commands that write a FILE. Before this bundle `runEsttab`
// returned a JSON block and wrote nothing to disk, and `makeSnapshot` carried no
// e() scalar bag, so `estadd` had nowhere to attach.
//
// Two shared pieces keep the five renderers from becoming five copies of
// `\begin{tabular}`: one `Table` model built from the stored estimates, and
// three renderers over it (TeX / fixed-width / delimited). They live inside the
// TU; only the entry points are declared here.

// `estadd sub ...` -- attach a local, scalar or matrix to the ACTIVE e() (Stata
// semantics: estadd runs BEFORE eststo, and the corpus writes it that way 5,725
// times). Stored in SessionState::estAdded, keyed by a fingerprint of the active
// estimate so a later, different regression discards the bag instead of letting
// it bleed onto the wrong model.
nlohmann::ordered_json runEstadd(const eval::ParsedCommand& parsed, SessionState& session);
// `estpost summarize|tabstat ...` -- a pseudo-estimate whose e(mean)/e(sd)/...
// 1xk matrices are what `esttab, cells(mean)` then reads. Values come from the
// SAME summarize/tabstat cores the native commands use, never a second
// implementation of the statistics.
nlohmann::ordered_json runEstpost(const eval::ParsedCommand& parsed, SessionState& session);
// `estout [NAME...] using f [, style() cells() stats() ...]`.
nlohmann::ordered_json runEstout(const eval::ParsedCommand& parsed, SessionState& session);
// `outreg2 using f [, replace|append tex excel ctitle() addtext() addstat()]`.
// The `.txt` ledger is the source of truth for `append`, exactly as in the
// original: Stata's outreg2 appends across SESSIONS, so an in-memory ledger
// would answer differently on the second run of the same do-file.
nlohmann::ordered_json runOutreg2(const eval::ParsedCommand& parsed, SessionState& session);
// `texsave varlist using f.tex [, ...]` -- the DATASET as a LaTeX table.
nlohmann::ordered_json runTexsave(const eval::ParsedCommand& parsed, SessionState& session);
// `texdoc init|close` plus the `tex <text>` line verb that writes into the open
// file. Split in two because `tex` is its own registry row.
nlohmann::ordered_json runTexdoc(const eval::ParsedCommand& parsed, SessionState& session);
nlohmann::ordered_json runTexdocLine(const eval::ParsedCommand& parsed, SessionState& session);
// `frmttable using f, statmat(M) tex ...` -- TeX only. The default output is
// RTF/.doc, which this build has no writer for and refuses by name rather than
// writing a differently-formatted file under the requested extension.
nlohmann::ordered_json runFrmttable(const eval::ParsedCommand& parsed, SessionState& session);
// `regsave [using f.dta]` / `parmest, saving(f.dta)` -- the estimates as a
// DATASET. Same builder, different column vocabulary.
nlohmann::ordered_json runRegsave(const eval::ParsedCommand& parsed, SessionState& session);
nlohmann::ordered_json runParmest(const eval::ParsedCommand& parsed, SessionState& session);
// `mat2txt, matrix(M) saving(f) [replace append title() format() note()]`.
nlohmann::ordered_json runMat2txt(const eval::ParsedCommand& parsed, SessionState& session);
// `erepost b = M [, rename]` -- overwrite the active e(b) from a matrix.
nlohmann::ordered_json runErepost(const eval::ParsedCommand& parsed, SessionState& session);
// The `using` half of `esttab`. Takes the block `runEsttab` already built and
// writes it, so the on-screen table and the file can never disagree: there is
// one renderer and the file is what it produced.
nlohmann::ordered_json writeEsttabFile(const nlohmann::ordered_json& esttabBlock,
                                        const std::string& path,
                                        const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                        SessionState& session);
// The factor base levels of a fit's coefficient table, as estout shows them
// (`0.clinic` 0 / (.)): [{var, level, name, before, after}], read from the
// data and the live e(sample). Empty when there is none or no data.
nlohmann::ordered_json factorBaseLevels(const nlohmann::ordered_json& coeftable,
                                        SessionState& session);
// The screen form of `esttab` (no `using`): the same translation and writer run
// without a file, so the options are validated exactly as the file form does
// (mode exclusivity, keep() patterns) and r() is posted. Null on success.
nlohmann::ordered_json esttabScreen(const nlohmann::ordered_json& esttabBlock,
                                     const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                     SessionState& session);

// ---- AER-SCOPE1 bundle F (RESHAPE, 2026-09-04) -----------------------------
// Seventeen shape/sample verbs on top of the established read/write pair:
// reading is `fetchColumns`, writing is a cobra verb + `session.load` +
// `mutationResult` (handlers.cpp:787-841). **No new cobra verb** -- every one
// of these is a composition of setcol / convert / append / merge / eval, so the
// engine binary and the engine lane are untouched by this bundle.
//
// Randomness is Philox (`rng.h`) keyed on `session.seed`, never a global
// generator: `set seed 7` followed by the same command twice must give the same
// rows, and a per-row key makes that true without ordering assumptions.

// `split v, parse() gen() limit() destring` -- PIET-side decomposition, then k
// `setcol --new` calls. Deliberately NOT `gen stubj = word(v,j)`: `word()` is
// whitespace-only, and `parse("-")` is the corpus's most common form.
nlohmann::ordered_json runSplit(const eval::ParsedCommand& parsed, SessionState& session);
// `xpose, clear [varname]` -- whole-frame transpose through a CSV round trip.
nlohmann::ordered_json runXpose(const eval::ParsedCommand& parsed, SessionState& session);
// `recast type varlist [, force]` -- METADATA only; the frame does not move, so
// this deliberately does not call `mutationResult`.
nlohmann::ordered_json runRecast(const eval::ParsedCommand& parsed, SessionState& session);
// `insobs # [, before(#) after(#)]`.
nlohmann::ordered_json runInsobs(const eval::ParsedCommand& parsed, SessionState& session);
// `stack varlist, into() clear [wide group()]`.
nlohmann::ordered_json runStack(const eval::ParsedCommand& parsed, SessionState& session);
// `separate v, by(g) [shortlabel generate()]`.
nlohmann::ordered_json runSeparate(const eval::ParsedCommand& parsed, SessionState& session);
// `mvencode varlist, mv(#) [override]`.
nlohmann::ordered_json runMvencode(const eval::ParsedCommand& parsed, SessionState& session);
// `sample # [, count]` -- Philox key per row, keep the k smallest.
nlohmann::ordered_json runSample(const eval::ParsedCommand& parsed, SessionState& session);
// `splitsample [, generate() nsplit() rseed() show]`.
nlohmann::ordered_json runSplitsample(const eval::ParsedCommand& parsed, SessionState& session);
// `drawnorm v1 v2 ..., n() means() sds() corr()|cov() seed() clear` -- a small
// dense Cholesky; a non-PD matrix is refused by name rather than sampled from.
nlohmann::ordered_json runDrawnorm(const eval::ParsedCommand& parsed, SessionState& session);
// `input varlist` ... `end`. The BLOCK is folded into one line by
// `captureInputBlocks` during preprocessing (0x1F separators -- a byte no Stata
// source contains), the same shape `program`/`end` already uses, so the
// dispatcher sees a single command rather than a statement stream.
nlohmann::ordered_json runInput(const std::string& rawCmd, SessionState& session);
std::vector<std::string> captureInputBlocks(const std::vector<std::string>& lines);
// `notes[:] ...` / `note` -- reads and writes `session.characteristics`, which
// the .dta writer already round-trips verbatim, so persistence is free.
nlohmann::ordered_json runNotes(const eval::ParsedCommand& parsed, SessionState& session);
// `compare v1 v2` -- display only.
nlohmann::ordered_json runCompare(const eval::ParsedCommand& parsed, SessionState& session);
// `cumul v, gen(c) [equal freq]`.
nlohmann::ordered_json runCumul(const eval::ParsedCommand& parsed, SessionState& session);
// `bsample [#] [, cluster() idcluster()]`.
// Whether `bsample` takes bsample.ado's fast method (no #, no cluster/strata/
// idcluster/weight): Stata's own rows from the session stream, with e(sample)
// kept on the drawn originals and cleared on the copies.
bool bsampleIsStataFastPath(const eval::ParsedCommand& parsed);
nlohmann::ordered_json runBsample(const eval::ParsedCommand& parsed, SessionState& session);
// `pctile new = v, nq()|percentiles() [genp()]` and the r-class `_pctile` form.
// Both use the SAME percentile rule as `xtile`/`winsor2` (transcribed from the
// engine's own stata_pctile), because two percentile rules in one product is a
// silently wrong number waiting for a fixture that does not exist.
nlohmann::ordered_json runPctile(const eval::ParsedCommand& parsed, SessionState& session);
nlohmann::ordered_json runPctileR(const eval::ParsedCommand& parsed, SessionState& session);

// ---- AER-SCOPE1 bundle G (IO-TEXT, 2026-09-04) -----------------------------
// The older text I/O verbs, as thin shims over the loaders that already exist,
// plus `putexcel` as a cell-addressed extension of the xlsx writer. Nothing
// here is a second reader or a second writer: `insheet` IS `import delimited`
// with Stata's own naming rules applied afterwards, `outsheet` IS
// `export delimited` with a tab default, and `saveold` IS `save` with an
// honest note about the format actually written.

// `insheet using f [, clear names nonames comma tab delimiter() case]`.
nlohmann::ordered_json runInsheet(const eval::ParsedCommand& parsed, SessionState& session);
// `outsheet [varlist] using f [, comma replace noquote nolabel names]`.
nlohmann::ordered_json runOutsheet(const eval::ParsedCommand& parsed, SessionState& session);
// `infile varlist using f [, clear]` -- free format: whitespace/comma tokens,
// quoted strings, `.` for missing, records wrapping every `varlist.size()`
// values regardless of source line breaks (Stata's own reader).
nlohmann::ordered_json runInfile(const eval::ParsedCommand& parsed, SessionState& session);
// `infix [type] var #-# ... using f [, clear]` -- fixed columns, 1-based and
// inclusive.
nlohmann::ordered_json runInfix(const eval::ParsedCommand& parsed, SessionState& session);
// `outfile [varlist] using f [, replace noquote comma nolabel]`.
nlohmann::ordered_json runOutfile(const eval::ParsedCommand& parsed, SessionState& session);
// `saveold f [, replace version(#)]`. This build writes Stata format 118 and
// SAYS so on every call rather than silently producing a 118 file under a name
// that promises an older format -- the note is the whole point of the row.
nlohmann::ordered_json runSaveold(const eval::ParsedCommand& parsed, SessionState& session);
// `putexcel set|A1 = ...|save|clear|describe`. Stateful across statements; the
// cell map lives in `SessionState::putexcel` and the workbook is rewritten in
// full on every assignment.
nlohmann::ordered_json runPutexcel(const std::string& rawCmd, SessionState& session);
// `filefilter in out, from() to() [replace]` -- publishes r(occurrences).
nlohmann::ordered_json runFilefilter(const eval::ParsedCommand& parsed, SessionState& session);
// `copy src dst [, replace text]` -- a URL source is refused by name; this
// build has no network access from Stata verbs.
nlohmann::ordered_json runCopy(const eval::ParsedCommand& parsed, SessionState& session);

// ---- AER-SCOPE1 bundle I (UTIL, 2026-09-04) --------------------------------
// Data-tidying utilities, built on primitives that already exist: the card's
// missing counts, runDrop/runDropIf, runEgen's rowmiss, runLabelDefine/
// runLabelValues, runRename, runTempvar. Nothing here computes a statistic a
// second time.
//
// Three of the seventeen commands in the plan's scope table are ABSENT on
// purpose. `dropmiss` and `renvars` have no package under their own name and
// did not arrive with a sibling, so their authorship could not be confirmed and
// the campaign's attribution rule gives them no row (an unregistered name
// already answers r(199), so nothing is lost by waiting). `strdist`/`matchit`/
// `reclink` are record linkage, refused for the same reason plus scope.

// `unique varlist [if] [in] [, by() gen()]` (and gtools' `gunique`).
nlohmann::ordered_json runUnique(const eval::ParsedCommand& parsed, SessionState& session);
// `distinct [varlist] [, joint missing]`.
nlohmann::ordered_json runDistinct(const eval::ParsedCommand& parsed, SessionState& session);
// `missings [varlist], report|dropvars|dropobs|tag()` -- each sub-verb routes to
// an existing primitive rather than re-deriving missingness.
nlohmann::ordered_json runMissings(const eval::ParsedCommand& parsed, SessionState& session);
// `mdesc [varlist]` -- the missing-value table; publishes r(miss)/r(total)/r(percent).
nlohmann::ordered_json runMdesc(const eval::ParsedCommand& parsed, SessionState& session);
// `labmask numvar, values(var) [lblname()]` -- synthesise a value label from a
// second variable's text and attach it.
nlohmann::ordered_json runLabmask(const eval::ParsedCommand& parsed, SessionState& session);
// `renvarlab [varlist], prefix()|suffix()|upper|lower`.
nlohmann::ordered_json runRenvarlab(const eval::ParsedCommand& parsed, SessionState& session);

// ---- AER scope 2, bundle K1 (handlers_aer_k1.cpp) ------------------------
// `tolower [varlist]` -- lowercase variable NAMES. NOT atomic: it renames until
// one fails, which is what its five-line ado does (measured rc 110 on a name
// that is already lowercase).
nlohmann::ordered_json runTolower(const eval::ParsedCommand& parsed, SessionState& session);
// `move v1 v2` -- put v1 in v2's POSITION. Direction-dependent: before() when
// v1 is later, after() when v1 is earlier. `before()` alone is wrong one way.
nlohmann::ordered_json runMove(const eval::ParsedCommand& parsed, SessionState& session);
// `renvars [varlist] \ newlist` or `renvars [varlist], <one transformation>`.
// presub()/postsub() substitute at the START/END only -- not everywhere.
nlohmann::ordered_json runRenvars(const eval::ParsedCommand& parsed, SessionState& session);
// `strdist {var|"lit"} {var|"lit"} [, gen(name) maxdist(#)]` -- byte-wise
// Levenshtein. Default new-variable name is the literal word `strdist`; the
// two-literal form with no gen() displays the number and sets r(d).
nlohmann::ordered_json runStrdist(const eval::ParsedCommand& parsed, SessionState& session);
// `splitvallabels var [, length(#)]` -- publishes r(relabel).
nlohmann::ordered_json runSplitvallabels(const eval::ParsedCommand& parsed, SessionState& session);
// `marksample name [, novarlist]` -- PROGRAM-INTERNAL only; refuses at top level
// because the local it reads (`varlist`) is set by `syntax`, which only runs
// inside a program.
nlohmann::ordered_json runMarksample(const eval::ParsedCommand& parsed, SessionState& session);
// `filelist [, directory() pattern() norecursive]` -- REPLACES the dataset with
// one row per file (dirname, filename, fsize).
nlohmann::ordered_json runFilelist(const eval::ParsedCommand& parsed, SessionState& session);
// `carryforward varlist, gen()|replace` -- last-non-missing forward fill, reset
// at each by-group boundary.
nlohmann::ordered_json runCarryforward(const eval::ParsedCommand& parsed, SessionState& session);
// `rangestat (stat) var, interval(key lo hi) [by()]` -- a closed window
// [key+lo, key+hi] including the row itself; results in ORIGINAL row order.
nlohmann::ordered_json runRangestat(const eval::ParsedCommand& parsed, SessionState& session);
// `tsspell [varname | , cond()] [, spell() seq() end()]` -- needs tsset.
nlohmann::ordered_json runTsspell(const eval::ParsedCommand& parsed, SessionState& session);
// Package-007 unblock (T7). `lookfor word [word ...]` -- variables whose name or
// label contains a word (case-insensitive); describes them and sets r(varlist).
nlohmann::ordered_json runLookfor(const eval::ParsedCommand& parsed, SessionState& session);
// `ihstrans varlist [if] [in] [, keepusing() prefix() casewise]` (SSC) -- new
// ihs_<var> columns = log(v+sqrt(v^2+1)).
nlohmann::ordered_json runIhstrans(const eval::ParsedCommand& parsed, SessionState& session);
// `sctorezone shift, force [only() exclude()]` (SSC scto) -- shift %tc variables
// by a fixed number of hours.
nlohmann::ordered_json runSctorezone(const eval::ParsedCommand& parsed, SessionState& session);
// Package-007 lane LB. `leebounds depvar treatvar [if] [in] [, select() level()
// vce(analytic)]` (SSC, Harald Tauchmann) -- Lee (2009) treatment-effect bounds,
// e(b) striped treatvar:lower treatvar:upper. Returns an ARRAY of blocks.
nlohmann::ordered_json runLeebounds(const eval::ParsedCommand& parsed, SessionState& session);

// AER scope-2 B2-3: `bcal create|describe|load|dir|check` (Stata business
// calendars). The reader/writer/index arithmetic are in piet/bcal.h; this is
// the command surface.
nlohmann::ordered_json runBcal(const eval::ParsedCommand& parsed, SessionState& session);

// ---- AER-SCOPE1 bundle J (GRAPH-X, 2026-09-04) -----------------------------
// Community graph and geo verbs as thin shims over the graph/column paths that
// already exist. Nothing here adds a plot type: `cdfplot` emits the twoway line
// block the renderer already draws, `event_plot` forwards to coefplot, and
// `grc1leg2` is `graph combine` with one panel's legend kept.
//
// SIX commands, not the plan's fourteen. `grc1leg` itself is absent: its
// package makes no authorship statement -- no author line in the ado and none
// in the help file -- and a distribution URL is not the package's own claim, so
// the attribution rule gives it no row. The eight refusals (cibar, binsreg,
// spmap, maptile, shp2dta, geoinpoly, heatplot, distplot) are community names
// too, so they are unregistered for the same reason and answer a generic
// r(199), which is what a named refusal would have said anyway.

// `grc1leg2 name1 name2 ..., [legendfrom() rows() cols() title() ...]` --
// `runGraphCombine` with every panel but one having `legend.off` set, which the
// frontend already honours.
nlohmann::ordered_json runGrc1leg(const eval::ParsedCommand& parsed, SessionState& session);
// `geodist lat1 lon1 lat2 lon2, gen(newvar) [miles sphere radius()]` --
// Vincenty on WGS84 by default; `sphere` is haversine.
nlohmann::ordered_json runGeodist(const eval::ParsedCommand& parsed, SessionState& session);
// `geonear nid nlat nlon using F, neighbors(...) [nearcount() miles ...]`.
nlohmann::ordered_json runGeonear(const eval::ParsedCommand& parsed, SessionState& session);
// `cdfplot var [, by()]` -- the ECDF as a twoway line block.
nlohmann::ordered_json runCdfplot(const eval::ParsedCommand& parsed, SessionState& session);
// `event_plot [name], stub_lag() stub_lead() ...` -- forwards to runCoefplot.
nlohmann::ordered_json runEventPlot(const eval::ParsedCommand& parsed, SessionState& session);

// ---- AER-SCOPE1 bundle H (ESTIM, 2026-09-05) -------------------------------
// E1 aliases. `areg` IS regress with an absorb(), so it delegates rather than
// re-deriving; absorb() missing is r(198), not a silent plain regression.
nlohmann::ordered_json runAreg(const eval::ParsedCommand& parsed, SessionState& session);
// `ivreg2`/`xtivreg2` -> runIvRegress. `stataCmd` is the spelling the user
// typed, which is what the estimator label and the community gate both read.
nlohmann::ordered_json runIvreg2(const eval::ParsedCommand& parsed, SessionState& session,
                                  const std::string& stataCmd);
// E2, computed PIET-side with no engine call.
nlohmann::ordered_json runSpearman(const eval::ParsedCommand& parsed, SessionState& session);
nlohmann::ordered_json runSktest(const eval::ParsedCommand& parsed, SessionState& session);
nlohmann::ordered_json runSignrank(const eval::ParsedCommand& parsed, SessionState& session);
nlohmann::ordered_json runCentile(const eval::ParsedCommand& parsed, SessionState& session);
// E2, engine-backed. Each ASKS the engine whether its subcommand exists and
// refuses by name when it does not, so a PIET built against an older cobra says
// so instead of producing a wrong number.
nlohmann::ordered_json runTsfilterHp(const eval::ParsedCommand& parsed, SessionState& session);
nlohmann::ordered_json runLpoly(const eval::ParsedCommand& parsed, SessionState& session);
nlohmann::ordered_json runLowess(const eval::ParsedCommand& parsed, SessionState& session);
nlohmann::ordered_json runPca(const eval::ParsedCommand& parsed, SessionState& session);
// AER scope-2 B2: `oaxaca ..., by(g) pooled` (Jann), twofold with robust suest V.
nlohmann::ordered_json runOaxaca(const eval::ParsedCommand& parsed, SessionState& session);
// X2-GLM (estimation_glm.cpp): `glm` fitted in-process through the engine's
// cecon::glm_fit, Stata's default ML lifecycle (oracle w2_glm_lifecycle). Returns
// an ARRAY of blocks (weight note, iteration log, header, estimation, warning);
// the caller pushes each element. runGlmMargins answers `margins, dydx() [post]`
// when the model in memory is a glm -- the engine's margins cannot (it would
// answer a logit AME), so the dispatcher routes here instead.
nlohmann::ordered_json runGlm(const eval::ParsedCommand& parsed, SessionState& session);
nlohmann::ordered_json runGlmMargins(const eval::ParsedCommand& parsed, SessionState& session);
// E3 scaffold stubs (handlers_e3_<cmd>.cpp): each returns an ARRAY holding the
// block `refused:e3_deferred` (rc 199) until its native task replaces the body.
// runLasso answers both `lasso` and `rlasso` (it reads the typed word from
// parsed.cmd). `honestdid` has no handler: its refusal stays in the dispatcher.
nlohmann::ordered_json runSwindex(const eval::ParsedCommand& parsed, SessionState& session);
nlohmann::ordered_json runQreg2(const eval::ParsedCommand& parsed, SessionState& session);
// `margins, dydx(varlist) [post]` while the model in memory is a qreg2 (its
// linear-index dy/dx from the posted e(b)/e(V); handlers_e3_qreg2.cpp).
nlohmann::ordered_json runQreg2Margins(const eval::ParsedCommand& parsed, SessionState& session);
// Stata `qreg` with its default vce(iid) (fitted/hsheather), fitted natively with
// the exact LP vertex (handlers_e3_qreg2.cpp). A null reply means "not this
// path's form" (vce(robust), weights, other options): runQreg answers those.
nlohmann::ordered_json runQregNative(const eval::ParsedCommand& parsed, SessionState& session);
nlohmann::ordered_json runLasso(const eval::ParsedCommand& parsed, SessionState& session);
nlohmann::ordered_json runLassoPredict(const eval::ParsedCommand& parsed, SessionState& session);
nlohmann::ordered_json runLassoCoef(const eval::ParsedCommand& parsed, SessionState& session);
nlohmann::ordered_json runSuest(const eval::ParsedCommand& parsed, SessionState& session);
nlohmann::ordered_json runRitest(const eval::ParsedCommand& parsed, SessionState& session);
// `ritest` from its raw line (dispatcher step 0a-ter): REST is the text after the
// command word, unexpanded -- the exp_list's e()/r()/_b[] are per replicate.
nlohmann::ordered_json runRitestLine(const std::string& rest, SessionState& session);
nlohmann::ordered_json runAcreg(const eval::ParsedCommand& parsed, SessionState& session);
// glm's e(rank): rowsof(V) - diag0cnt(invsym(V)) on the k x k row-major V,
// with invsym's measured drop rule (comment at the definition).
int64_t glmVceRank(const std::vector<double>& V, size_t k);
// The e() entries glm posts that no other estimator clears (e(b) e(V) e(ilog)
// e(gradient), e(varfunct) e(linkt) e(opt) e(technique) e(a)). The dispatcher
// snapshots them while the model in memory is a glm (or its posted margins)
// and, once a command replaces that model, drops each entry that is STILL
// exactly what glm posted -- so `glm` then `regress` then `matrix V = e(V)`
// cannot read the glm's V. An entry the new command wrote itself differs and
// is kept.
struct GlmEStores {
  std::vector<std::pair<std::string, eval::EvalMatrix>> matrices;
  std::vector<std::pair<std::string, std::string>> strings;
};
std::optional<GlmEStores> glmOwnedEStores(const SessionState& session);
void dropStaleGlmEStores(SessionState& session, const GlmEStores& before);
// AER scope-2 B2-4: `teffects nnmatch (ovar mvarlist) (tvar) [if] [in], [gen() ...]`
// (StataCorp [CAUSAL]). `sub` is the sub-command word the dispatcher stripped.
nlohmann::ordered_json runTeffects(const std::string& sub, const eval::ParsedCommand& parsed,
                                   SessionState& session);
// `xi [, prefix() noomit] i.a [i.b*i.c ...]` -- ALWAYS materializes the _I*
// columns; there is no pass-through form.
nlohmann::ordered_json runXi(const eval::ParsedCommand& parsed, SessionState& session);
// The `xi: <command>` prefix form. Materializes exactly as runXi does, then
// hands back the wrapped command with each i. term replaced by the columns it
// produced (`rewritten`) plus the mapping lines to print (`displayText`); the
// caller re-dispatches. Returns a non-null error block on refusal.
nlohmann::ordered_json runXiPrefix(const std::vector<std::string>& terms,
                                    const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                    const std::string& wrapped, SessionState& session,
                                    std::string* rewritten, std::string* displayText);

// ---- AER-SCOPE1 bundle A (2026-09-03) --------------------------------------
// Cox's `winsor VAR, gen(NEW) p(#) | h(#) [highonly|lowonly]`. Deliberately NOT
// routed to runWinsor2: winsor2 clamps at the `_pctile` value, which averages
// the two neighbours when N*p lands on an integer, while winsor.ado replaces
// the h lowest observations with the (h+1)-th ORDER STATISTIC. Same intent,
// different number, and the difference is invisible in the output -- an alias
// here would be a silent wrong answer rather than a missing feature.
nlohmann::ordered_json runWinsorCox(const eval::ParsedCommand& parsed, SessionState& session);

// ---- AER-SCOPE1 bundle D (ENV, 2026-09-03) ---------------------------------
// The environment verbs the ninety-paper corpus opens with. Three shapes:
//
//   accept-note  net / ado / adopath / sysdir / pause / gr_edit -- PIET has no
//                ado directory and no network, so the honest answer is a
//                visible one-line note and rc 0. A silent no-op would be the
//                same behaviour with the evidence removed, and a refusal would
//                stop the do-file on its first line (these sit in the preamble
//                of 25, 15, 18 and 9 packages respectively).
//   translate    shell -- an allowlist of file-manipulation verbs mapped onto
//                PIET's own erase/mkdir/rmdir/copy, so the effect the do-file
//                depended on actually happens. NO OS process is ever spawned;
//                anything outside the allowlist is refused by name.
//   implement    type / dir / findfile / sleep / error / frames reset -- small
//                real behaviours with no engine involvement.
nlohmann::ordered_json runShell(const std::string& args, SessionState& session);
nlohmann::ordered_json runType(const std::string& raw, const std::vector<std::string>& varlist,
                                const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                SessionState& session);
nlohmann::ordered_json runDir(const std::string& raw, const std::vector<std::string>& varlist,
                               const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                               SessionState& session);
nlohmann::ordered_json runNet(const std::string& raw, const std::vector<std::string>& varlist,
                               const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                               SessionState& session);
nlohmann::ordered_json runAdo(const std::string& raw, const std::vector<std::string>& varlist);
nlohmann::ordered_json runAdopath(const std::string& raw, const std::vector<std::string>& varlist,
                                   SessionState& session);
nlohmann::ordered_json runSysdir(const std::string& raw, const std::vector<std::string>& varlist,
                                  SessionState& session);
nlohmann::ordered_json runFindfile(const std::string& raw, const std::vector<std::string>& varlist,
                                    const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                    SessionState& session);
// The first FNAME on the session adopath, searched as findfile searches it
// (each entry, then its letter subdirectory); nullopt when none. The
// dispatcher's ado autoload reads it.
std::optional<std::string> findOnAdopath(SessionState& session, const std::string& fname);
nlohmann::ordered_json runSleep(const std::vector<std::string>& varlist);
nlohmann::ordered_json runErrorCode(const std::vector<std::string>& varlist);
nlohmann::ordered_json runFramesReset(SessionState& session);

// `timer on|off|clear|list [N]` (BS-124, BETA campaign C4) — `sub` is the
// lowered sub-command ("on"/"off"/"clear"/"list"); `slotArg` is the parsed
// slot number (1..100) when given, unset for the bare `timer clear`/
// `timer list` forms. See the .cpp definition for the full state machine.
nlohmann::ordered_json runTimer(const std::string& sub, const std::optional<int>& slotArg,
                                 SessionState& session);

// -- M3b: MI-066 data verbs + egen (ROADMAP M3b) -----------------------------

// `egen NEWVAR = fn(...) [if COND] [in RANGE] [, by(g) missing p(#) ...]` —
// C++ port of egen.py + mutate.py's run_egen. Row functions (rowmean/
// rowsd/...) route to cobra eval --rowegen; everything else routes to
// --egen. `by` is the already-extracted by()-clause value (bysort's
// inline-injection path AND a plain `egen X=fn(y), by(g)` both funnel
// through here with `by` populated from opts["by"] or the bysort prefix).
// `inRange` (BS-090, BETA Phase G): composes with `ifExpr` via AND, reusing
// the SAME synthetic row-index column this handler already materializes
// for the if-merge-back key (see the .cpp comment).
// `storeType` (BUG-471 T2): the resolved result type; withheld from tag()/
// seq()/mode(), whose ados ignore it.
// `fixedTypeOut`: the result's type where the function, not the declaration,
// decides it -- for seq() the type Stata's widening replace leaves (BUG-471 T2
// fix-up F3: egen seq() generates a byte, then widens, chosen from the stored
// min/max the computing call itself summarizes); for mode() of a string the
// str# its compress leaves (BUG-1109); for cut() and the rest of
// runEgenAdoFunction the type they recorded for the result (BUG-1108). Untouched
// otherwise.
// `gtools`: the line was gtools' gegen, whose own total()/sum()/mean() sum in
// data order where official egen reads summarize's exact sum.
nlohmann::ordered_json runEgen(const std::string& expr, const std::string& by,
                                const std::optional<std::string>& ifExpr,
                                const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                SessionState& session,
                                const std::optional<std::string>& inRange = std::nullopt,
                                const std::string& storeType = "",
                                std::string* fixedTypeOut = nullptr, bool gtools = false);

// `[bysort VARLIST:] gen/replace newvar = expr` — C++ port of bysort.py's
// run_bysort_gen (cobra eval --bygen, native _n/_N group-context support).
// byVars=[] means global (no by()/sort() clause). Called by the dispatcher's
// bysort/by prefix interception (any gen/replace sub-command, not just ones
// referencing _n/_N — matches the oracle's "keep separate for clarity"
// design, see egen.cpp) AND by the direct (non-bysort) gen/replace dispatch
// whenever `_n`/`_N` appears OR an `in a/b` range is present (BUG-077).
// `inRange` (default nullopt) is the raw tokenizer-captured qualifier string
// (e.g. "1/10", "f/l"); only the direct-dispatch call sites pass it — the
// bysort/by-prefix call site never does (a by-group `_n` can't express
// Stata's GLOBAL `in` range; see the tokenizer's inRangeAllowed comment).
// `sortVars` (default nullopt = preserve the old sortFirst-driven default)
// is BS-002's explicit parenthetical sort key (`bysort byvar (sortvar):`) —
// only the bysort/by-prefix call site ever passes a non-empty value.
// BUG-532: `bysort KEYS (SORT):`'s in-place sort for generate/replace, in
// Stata's order and with Stata's marker; skipped when KEYS SORT already lead
// the marker. Returns the error block, or nullopt.
std::optional<nlohmann::ordered_json> sortForBysort(
    SessionState& session, const std::vector<std::string>& byVars,
    const std::optional<std::vector<std::string>>& sortVars);

nlohmann::ordered_json runBysortGen(
    SessionState& session, const std::string& expr, const std::vector<std::string>& byVars,
    const std::optional<std::string>& ifExpr, bool sortFirst, bool isReplace,
    const std::optional<std::string>& inRange = std::nullopt,
    const std::optional<std::vector<std::string>>& sortVars = std::nullopt,
    const std::string& storeType = "", bool noPromote = false);

// `postfile H type1 name1 ... using PATH [, replace]` / `post H (e1)(e2)...`
// / `postclose H` — C++ port of postfile.py (S8). All three take the
// post-macro-expansion raw line (postfile/post) or bare handle name
// (postclose); silent (empty array) on success like the oracle.
nlohmann::ordered_json runPostfile(const std::string& rawLine, SessionState& session);

// BUG-343: `file open|write|read|close` (text mode). One entry point taking the
// RAW line, like runPostfile above — the sub-verb, the handle name, the `using`
// path and the write payload all have to be read off the original text (quotes
// and all), which a pre-parsed varlist has already destroyed.
nlohmann::ordered_json runFileCmd(const std::string& rawLine, SessionState& session);
nlohmann::ordered_json runPost(const std::string& rawLine, SessionState& session);
nlohmann::ordered_json runPostclose(const std::string& handle, SessionState& session);

// `save FILE [, replace]` — persist the current dataset to FILE.
// `dtaRelease` > 0 writes a genuine Stata .dta of that release (115/117/118)
// whatever FILE's extension is (`saveold`'s target; a `.tmp` tempfile included).
nlohmann::ordered_json runSaveData(const std::string& filepath, bool replace,
                                    SessionState& session, int dtaRelease = 0);

// `rename OLDVAR NEWVAR` — two-arg form only.
nlohmann::ordered_json runRename(const std::string& oldvar, const std::string& newvar,
                                  SessionState& session);

// `clonevar NEWVAR = OLDVAR [if COND] [in RANGE]` — value copy + metadata clone
// (BUG-003; `if` per BS-031(c), `in` per BUG-1096 — see the .cpp comment).
nlohmann::ordered_json runClonevar(const std::string& newvar, const std::string& oldvar,
                                    SessionState& session,
                                    const std::optional<std::string>& ifExpr = std::nullopt,
                                    const std::optional<std::string>& inRange = std::nullopt);

// `collapse (stat) varlist [(stat) varlist...] [, by(g1 g2)] [if COND] [in RANGE]`.
// `weightVar` (Step-1 D3): when set (tokenizer only admits this for
// `[fw=]`/`[fweight=]`), forwarded as `--fweight weightVar` to cobra's
// `collapse` alongside `--by` — cobra does the actual Σw math and rejects
// unsupported combinations (e.g. weighted median/p##) itself. `inRange`
// (BS-090, BETA Phase G): `in a/b`, composed with `ifExpr` via AND
// (piet::handlers::applyIfAndInFilter) BEFORE cobra's collapse call, rather
// than forwarded as a raw `--keepif` (see the .cpp for why).
// `opts`: the line's options as the dispatcher resolved them; the option words
// (cw, clabel, nocompress, append, smart, fast) are read off their keys.
nlohmann::ordered_json runCollapse(const std::string& rawLine,
                                    const std::optional<std::string>& ifExpr,
                                    SessionState& session,
                                    const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                    const std::optional<std::string>& weightVar = std::nullopt,
                                    const std::optional<std::string>& inRange = std::nullopt,
                                    const std::string& flavor = "collapse",
                                    const std::optional<std::string>& weightType = std::nullopt);

// `gsort [-]VAR ...` — generalised per-key ascending/descending sort.
nlohmann::ordered_json runGsort(const std::vector<std::string>& tokens, SessionState& session);

// `order VARLIST [, first|last|before(VAR)|after(VAR)]` — column reorder.
nlohmann::ordered_json runOrder(const std::vector<std::string>& varlist,
                                 const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                 SessionState& session);

// `recode VARNAME (rules...) [if COND] [, gen(NEWVAR)]` — value recoding.
// `if` requires `gen()` in this build (BS-031(b) — see .cpp comment).
nlohmann::ordered_json runRecode(const std::string& rawLine, SessionState& session,
                                  const std::optional<std::string>& ifExpr = std::nullopt);

// `tsfill [, full]` — BS-081 (BETA D4 batch). Fills gaps in the CURRENT
// tsset/xtset time range (never extrapolates past the observed max — see
// the .cpp header comment for the D4f oracle's own byte-for-byte count
// match, 335 not 336). Requires a prior `tsset timevar` (or `xtset panelvar
// timevar`); real Stata's `full` option (materializes every panelvar x
// timevar combination even for panels that never had ANY observation at a
// given period) has no engine equivalent — honest-rejected, not silently
// dropped.
// BUG-370 — xtile / ipolate / tssmooth ma. The engine half of all three already
// shipped (`cobra eval --xtile/--ipolate/--tsma`, Stata-parity tested in
// tests/unit/test_xtipo.cpp and test_tsma.cpp); what was missing was the command
// table row pointing at it. Each of these is argument validation plus one engine
// call, and each refuses BY NAME the Stata options the engine has no equivalent
// for, rather than dropping them silently.
// BUG-371 — `decode` (the inverse of encode: value-label codes back to strings)
// and `inspect` (the numeric composition of one column). Both are PIET-native:
// decode reads session.valueLabels and writes ONE column via `cobra setcol`, and
// inspect is arithmetic over an mmap'd column, the same reason `isid` is native.
nlohmann::ordered_json runDecode(const std::string& varname,
                                 const std::optional<std::string>& genVar, SessionState& session,
                                 size_t maxLength = 32000);
nlohmann::ordered_json runInspect(const std::vector<std::string>& varlist, SessionState& session);
// BUG-371 — panel summaries. Both PIET-native (no engine verb exists), and both
// require xtset: the whole content of xtsum is the BETWEEN/WITHIN split, which is
// meaningless without knowing what a unit is.
nlohmann::ordered_json runXtsum(const std::vector<std::string>& varlist, SessionState& session);
nlohmann::ordered_json runXttab(const std::vector<std::string>& varlist, SessionState& session);

nlohmann::ordered_json runXtile(const std::string& newvar, const std::string& srcvar,
                                const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                const std::optional<std::string>& ifExpr, SessionState& session,
                                const std::optional<std::string>& weightType = std::nullopt,
                                const std::optional<std::string>& weightVar = std::nullopt);
// xtile's weighted form (handlers_aer_reshape.cpp, beside the weighted
// percentile rule it shares with pctile/_pctile).
nlohmann::ordered_json runXtileWeighted(const std::string& newvar, const std::string& srcvar,
                                        int nq, const std::string& weightVar,
                                        const std::string& weightType, SessionState& session);
nlohmann::ordered_json runIpolate(const std::vector<std::string>& varlist,
                                  const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                  const std::optional<std::string>& ifExpr, SessionState& session);
nlohmann::ordered_json runTssmoothMa(const std::string& newvar, const std::string& srcvar,
                                     const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                     SessionState& session);

// `tsfill [, full]` (BUG-897: varlist/if/in are tsfill.ado's r(101); `full`
// forwards cobra tsfill --full).
nlohmann::ordered_json runTsfill(const eval::ParsedCommand& parsed, SessionState& session);

// `duplicates report|drop|tag|list [VARLIST] [if COND] [in RANGE] [, gen(NEWVAR)]`.
// `ifExpr` (BUG-042, BS-031(a)): report/tag/list all honor `if` (tag/list
// fixed this stint — see their own .cpp comments); `drop` honestly rejects
// `if` instead of ignoring it (see runDuplicatesDrop's .cpp comment for why
// that one stayed a reject rather than a full fix this stint). `inRange`
// (BS-090, BETA Phase G) follows the SAME split: report/tag/list now honor
// `in` (composed with `if` via AND); `drop` honestly rejects `in` too, for
// the identical reason it already rejects `if` (see runDuplicatesDrop's
// .cpp comment — restricting the sample before dedup-and-splice-back needs
// more machinery than a simple filter, for `in` exactly as much as `if`).
nlohmann::ordered_json runDuplicates(const std::string& sub,
                                      const std::optional<std::vector<std::string>>& varlist,
                                      const std::optional<std::string>& genvar,
                                      SessionState& session,
                                      const std::optional<std::string>& ifExpr = std::nullopt,
                                      const std::optional<std::string>& inRange = std::nullopt);

// `reshape long|wide STUB... , i(IDVARS) j(JVAR)`.
nlohmann::ordered_json runReshape(const std::vector<std::string>& stubs,
                                   const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                   SessionState& session);

// `append using FILE [, generate(NAME) keep(VARLIST)]` — union-concat rows from a
// second dataset. `generate` (B5-D10), when given, marks the appended rows: 0 for
// the original master rows, 1 for the nUsing rows appended from `using`. `keepSpec`
// (BUG-549) is keep()'s raw varlist, expanded against the USING file's names.
// A variable numeric on one side and string on the other is r(106) unless
// `force` (BUG-550), which takes the using cells as missing of the master's type.
// A .dta using's value-label definitions are copied unless `nolabel` (BUG-551).
// The using file's notes are appended after the master's unless `nonotes`.
// Each master variable the using data widen adds Stata's "(variable x was float,
// now double to accommodate using data's values)" line to `typeNotes` (BUG-1092).
nlohmann::ordered_json runAppend(const std::string& filepath, SessionState& session,
                                  const std::optional<std::string>& genVar = std::nullopt,
                                  const std::optional<std::string>& keepSpec = std::nullopt,
                                  bool force = false, bool nolabel = false, bool nonotes = false,
                                  std::vector<std::string>* typeNotes = nullptr);

// `append using FILE [FILE ...] [, ...]` — append.ado's loop: every file
// described first (r(601)), r(N) = the last file's _N, generate() codes 1..k,
// and an empty master takes the first file whole.
nlohmann::ordered_json runAppendFiles(const std::vector<std::string>& files, SessionState& session,
                                       const std::optional<std::string>& genVar = std::nullopt,
                                       const std::optional<std::string>& keepSpec = std::nullopt,
                                       bool force = false, bool nolabel = false,
                                       bool nonotes = false,
                                       std::vector<std::string>* typeNotes = nullptr);

// `merge 1:1|m:1|1:m|m:m KEYLIST using FILE [, opts]` (mergeType already mapped by
// map_mtype); `sequential` = `merge 1:1 _n` (keylist empty).
nlohmann::ordered_json runMerge(const std::string& mergeType, const std::vector<std::string>& keylist,
                                 const std::string& usingPath,
                                 const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                 SessionState& session, bool sequential = false,
                                 bool oldSyntax = false, bool oldSort = false);

// `encode STRVAR, gen(NEWVAR) [label(LBLNAME)]` — string(factor)->numeric
// (1..K, alphabetical) with a value-label set registered + attached. C++
// port of encode_op.py's run_encode. Blocking the debug_all.do M4a depth
// boundary at line 157 — folded in alongside the M4a estimation surface
// even though it predates this campaign's explicit item list, since it
// unblocks stream-replay depth for near-zero marginal scope.
// BUG-042: `ifExpr` restricts BOTH which rows get a real encoded value
// (matching Stata's `encode ... if COND` — rows failing COND come back
// missing on `newvar`) AND nothing else (the level->code mapping itself
// still comes from the whole variable — see egen.cpp's runEgen for why the
// mapping and the per-row gating are legitimately different scopes).
nlohmann::ordered_json runEncode(const std::string& strvar, const std::string& newvar,
                                  const std::optional<std::string>& lblname, SessionState& session,
                                  const std::optional<std::string>& ifExpr = std::nullopt,
                                  bool noExtend = false);

// `destring VAR, {replace | gen(NEWVAR)} [ignore("chars") force float percent
// dpcomma]` -- BS-003/D4 (BETA B-data): PIET-native string(factor)->numeric
// conversion of ONE variable; the dispatcher's `destring`/`tostring` arm runs
// the varlist loop and the option checks, and calls this once per variable.
// Behaviour as measured on StataBE 19.5. In short: an
// already-numeric VAR, or one holding a value that does not read as a number
// (without `force`), is declined with a `display` note at rc 0 and nothing is
// created; otherwise the parsed f64 column replaces VAR or is added as NEWVAR
// right after VAR, and VarMeta records the storage type Stata's destring
// reports (byte/int/long when every value is integral and in range, else
// double, or float under `float`). `opts` carries the three options beyond
// replace/generate/ignore/force: float (create as float instead of double),
// dpcomma (comma as the decimal separator) and percent (ignore `%` and divide
// by 100).
struct DestringOpts {
  bool asFloat = false;
  bool dpcomma = false;
  bool percent = false;
};
nlohmann::ordered_json runDestring(const std::string& varname, bool doReplace,
                                    const std::optional<std::string>& genVar,
                                    const std::optional<std::string>& ignoreChars, bool force,
                                    SessionState& session, const DestringOpts& opts = {});

// `tostring VAR, {gen(NEWVAR) | replace} [usedisplayformat]` — BS-003/D4
// (BETA B-data): PIET-NATIVE numeric->string(factor) conversion, the mirror
// of runDestring above. Default rendering matches strmutate.cpp's own
// coerceStr() convention (integer -> plain digits, non-integer ->
// eval::gFormat, missing -> ""); `usedisplayformat` instead renders via
// eval::applyStataFmt() against the variable's OWN session.varMeta[...].
// format (BS-005/D6's same field; "%9.0g" default when unset) — note
// applyStataFmt only recognizes the numeric %{f,g,e} families, so a
// date/time display format falls back to its own generic-number rendering
// (a pre-existing limitation of that shared helper, not newly introduced
// here). Scoped to exactly ONE variable per call, same rationale as
// runDestring.
//
// BUG-665: `opts.stataGate` (set by the `tostring` command arm; left off by
// import excel's allstring, which converts unconditionally) switches on
// tostring's own rules: the rendering format is format() when
// given, the variable's display format under usedisplayformat, else %12.0g;
// the value rendered is the STORED value (a float-typed variable is rounded
// to float first -- w2b_dataio.log tostring_gen_force renders float 7.01 as
// "7.010000229"); a variable any of whose rendered values does not read back
// equal is left as is with "VAR cannot be converted reversibly; no
// replace|generate" (rc 0) unless `force`; an already-string variable is a
// note, not an error; `replace` on a value-labelled variable is refused by
// note unless `force`. `forcedLoss`, when given, says whether a conversion
// went through only because of `force` (some value does not read back), for
// the ado's "VAR was forced to string; some loss of information" line.
struct TostringOpts {
  bool stataGate = false;
  std::optional<std::string> format;
  bool force = false;
};
nlohmann::ordered_json runTostring(const std::string& varname, bool doReplace,
                                    const std::optional<std::string>& genVar, bool useDisplayFormat,
                                    SessionState& session, const TostringOpts& opts = {},
                                    bool* forcedLoss = nullptr);

// `joinby key1 [key2 ...] using FILE` (bare form only) — BS-004/D5 (BETA
// B-data): PIET-NATIVE inner key-join (the analysis.do:451 shape). For
// every master row, emits one output row per USING row sharing its key
// values (the full within-key-group cross-product — joinby's own
// documented, order-independent behavior for repeated keys on either side,
// DISTINCT from Stata's discouraged `merge, m:m` row-order-paired
// semantics, which this build does not implement); a master row with zero
// using matches is dropped (unmatched(none), the default and — per D5 —
// the ONLY mode this build implements). ANY joinby option
// (unmatched()/update/updatemiss/_merge()/nolabel/...) is an honest reject
// naming the option (post-beta WP-18) — the dispatcher arm enforces this
// via the zero-known-options net before calling this handler. The join
// itself is ONE engine call (`cobra joinby --data --using --keys --out`,
// L1b) — PIET keeps the using-file resolution (.dta/.csv -> .hbin
// pre-convert; BUG-125 virtual-save materialization) and all
// pre-validation (keys present on both sides, non-key column-name
// overlap) so the established PIET-side error messages still fire before
// the engine is reached.
// `unmatched` is joinby.ado's UnMatch value (none|master|using|both);
// `mergeVar` its _merge() name (the marker exists when either asks for it).
nlohmann::ordered_json runJoinby(const std::vector<std::string>& keylist,
                                  const std::string& usingPathArg, SessionState& session,
                                  const std::string& unmatched = "none",
                                  const std::optional<std::string>& mergeVar = std::nullopt);

// -- resampling prefixes (P6 wave, handlers_resample.cpp) -------------------
// `bootstrap:` / `jackknife:` / `permute:` — the loop runs PIET-side and the
// inner command goes through ordinary dispatch, so the prefixes are general
// (any command PIET supports, any exp_list the expression layer can evaluate).
// `expListRaw` MUST arrive verbatim: its r()/e()/_b[] references are evaluated
// once per replicate, not at parse time (see the .cpp header for why dispatch
// intercepts these before macro expansion). `permuteVar` is empty except for
// permute.
nlohmann::ordered_json runResample(const std::string& method, const std::string& permuteVar,
                                   const std::string& expListRaw, const std::string& optsRaw,
                                   const std::string& innerCmd, SessionState& session);

// `rolling explist, window(N) saving("FILE"[, replace]): command` — the same
// PIET-side skeleton as the resampling prefixes, with a WINDOW SLICE of the
// tsset time variable in place of a resample, so the inner command stays
// general. Requires a prior tsset/xtset.
nlohmann::ordered_json runRolling(const std::string& expListRaw, const std::string& optsRaw,
                                  const std::string& innerCmd, SessionState& session);

// `statsby _b _se, clear [double]: command` — the ungrouped form only (no by());
// see handlers_resample.cpp for the naming and storage rules it follows.
nlohmann::ordered_json runStatsby(const std::string& expListRaw, const std::string& optsRaw,
                                  const std::string& innerCmd, SessionState& session);

// -- frames (P6 wave, handlers_frames.cpp) ----------------------------------
// Stata `frame`s: additional named datasets beside the current one. A PIET
// session concept only — each frame is one hbin in the session workdir, and
// every operation is expressed with engine verbs that already exist (eval,
// merge, rename), so no contract surface moves. See handlers_frames.cpp's
// header comment for the design and the deliberate scope limits (the current
// frame is always `default`; frame create/change/pwf are honest rejects).
nlohmann::ordered_json runFramePut(const std::vector<std::string>& varlist,
                                   const std::optional<std::string>& ifExpr,
                                   const std::string& into, SessionState& session);
nlohmann::ordered_json runFrameDrop(const std::string& name, SessionState& session);
// `frame NAME: CMD` — runs CMD with NAME as the current dataset, then restores.
// Mutations persist into the frame, as they do in Stata.
nlohmann::ordered_json runFramePrefix(const std::string& name, const std::string& innerCmd,
                                      SessionState& session);
// `frlink m:1|1:1 KEY..., frame(F) generate(G)` — G holds each row's matching
// observation number in F (missing when unmatched), computed as an m:1 merge
// against a helper carrying F's row index.
nlohmann::ordered_json runFrlink(const std::string& linkType,
                                 const std::vector<std::string>& keylist,
                                 const std::string& frameName, const std::string& genName,
                                 SessionState& session);
// `frget NEW=VAR [...], from(LINKVAR)` — copies variables out of the frame the
// link points into, matched through the link.
nlohmann::ordered_json runFrget(const std::vector<std::pair<std::string, std::string>>& renames,
                                const std::string& fromLink, SessionState& session);

// `isid VARLIST` — assert that VARLIST uniquely identifies the observations.
// Silent (no output, rc 0) when it does; Stata's r(459) with its own wording
// when it does not, r(111) for an unknown variable. Oracle: StataBE 19.5 —
// `isid make` on auto.dta → rc 0, `isid foreign` → "variable foreign does not
// uniquely identify the observations" + r(459), `isid nosuchvar` → r(111).
// PIET-native (the engine has no isid verb): a single pass over the key
// columns' mmap'd payloads, no scratch file written.
nlohmann::ordered_json runIsid(const std::vector<std::string>& varlist, SessionState& session,
                                bool missok = false, bool sort = false);

// P1 wave data verbs — thin forwarders onto the engine subcommands of the same
// name (the joinby/tsfill shape: PIET validates and resolves the `using` file,
// the engine does the transformation). Oracle pins live on the engine side.
//   `expand N`               — each observation becomes N copies
//   `fillin v1 v2 [...]`     — rectangularize + `_fillin` flag
//   `cross using FILE`       — Cartesian product (master wins a shared name)
//   `contract varlist`       — distinct combinations + `_freq`
// B4-W3b: `generate(newvar)` marks each row 0 (an original) or 1 (a copy the
// expansion added). Measured (StataBE 19.5, pins_oracle_g.log:227-233,
// `sysuse auto` + `expand 2, generate(dupflag)`): _N 74 -> 148, dupflag[1]==0,
// and `tabulate dupflag` is 74 "Original observation" / 74 "Duplicated
// observation" — i.e. the copies are APPENDED, which cobra's own expand does
// too (verified on this build: an `origrow=_n` column reads 1,2,... at rows
// 1,2 and again at rows N+1,N+2).
nlohmann::ordered_json runExpand(const std::string& countArg,
                                 const std::optional<std::string>& genVar,
                                 SessionState& session);
nlohmann::ordered_json runFillin(const std::vector<std::string>& varlist, SessionState& session);
nlohmann::ordered_json runCross(const std::string& usingPathArg, SessionState& session);
nlohmann::ordered_json runContract(const std::vector<std::string>& varlist, SessionState& session);

// `export delimited [varlist] using FILE [, replace delimiter(D) nolabel]` and
// `import delimited [using] FILE [, clear delimiter(D)]` — P1 wave.
// PIET-native rather than engine-side because the WRITER needs the session's
// value labels: Stata's export delimited writes a labeled numeric as its LABEL
// text by default (`nolabel` writes the code), and those labels live only in
// PIET's session, not in the .hbin. Missing cells are written the way Stata
// writes them — `.`, or the extended code `.a`..`.z` (oracle: the Stata-written
// p1_roundtrip.csv carries literal `.a` in 22 cells of `y`).
// import delegates to the same content-sniffing loader `use` already uses.
nlohmann::ordered_json runExportDelimited(const std::vector<std::string>& varlist,
                                          const std::string& pathArg,
                                          const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                          SessionState& session);
// The same with `[if] [in]` (export delimited takes [if] [in]; w2ar dx_exd_ifin log:746).
nlohmann::ordered_json runExportDelimited(const std::vector<std::string>& varlist,
                                          const std::string& pathArg,
                                          const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                          const std::optional<std::string>& ifExpr,
                                          const std::optional<std::string>& inRange,
                                          SessionState& session);

// One writer for export delimited and outsheet: the two differ only in these
// switches (w2ar log:453-929). `quote`:
//   kMinimal     quote a string only when it holds the delimiter, `"`, CR or
//                LF; double an inner quote (export delimited's default)
//   kAll         quote every string and value-label text, doubling inner
//                quotes (export delimited, quote)
//   kAllVerbatim quote every string and label text, inner quotes NOT doubled
//                (outsheet's default: `"q"r"`, log:815)
//   kNone        never quote (outsheet, noquote)
// `dataFmt` writes every numeric through its display format (export
// delimited, datafmt; outsheet always). Dates are written formatted either
// way.
struct DelimWriteSpec {
  enum class Quote { kMinimal, kAll, kAllVerbatim, kNone };
  char delim = ',';
  bool header = true;
  bool noLabel = false;
  bool dataFmt = false;
  Quote quote = Quote::kMinimal;
  std::string verb = "export delimited";
  // The file as the user typed it, suffix included: every message names it, never
  // the resolved path, which would put this machine's working directory into each
  // captured transcript (BUG-410). Empty = the path itself.
  std::string shown;
};
// Writes `resolvedPath` (already resolved; the caller owns the exists/replace
// decision) from the rows `ifExpr`/`inRange` select.
nlohmann::ordered_json writeDelimitedFile(const std::vector<std::string>& varlist,
                                          const std::string& resolvedPath,
                                          const DelimWriteSpec& spec,
                                          const std::optional<std::string>& ifExpr,
                                          const std::optional<std::string>& inRange,
                                          SessionState& session);
// A block whose text or message names the RESOLVED path (the xlsx writer's
// exceptions), re-spoken with the file as the user typed it in that place
// (BUG-410). A miss leaves the block as it was.
nlohmann::ordered_json reEchoPath(nlohmann::ordered_json block, const std::string& resolved,
                                  const std::string& shown);
nlohmann::ordered_json runImportDelimited(const std::string& pathArg,
                                          const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                          SessionState& session);
// `import delimited [namelist] using FILE [, options]`.
nlohmann::ordered_json runImportDelimited(const std::vector<std::string>& varlist,
                                          const std::string& pathArg,
                                          const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                          SessionState& session);

// `export excel [varlist] using FILE [, replace firstrow(variables) nolabel]`
// `import excel [using] FILE [, firstrow clear]` — P1 wave. Same value-label
// rule as the delimited pair. The .xlsx layer is piet/native/src/xlsx.{h,cpp}.
nlohmann::ordered_json runExportExcel(const std::vector<std::string>& varlist,
                                      const std::string& pathArg,
                                      const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                      SessionState& session);
nlohmann::ordered_json runExportExcel(const std::vector<std::string>& varlist,
                                      const std::string& pathArg,
                                      const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                      const std::optional<std::string>& ifExpr,
                                      const std::optional<std::string>& inRange,
                                      SessionState& session);
nlohmann::ordered_json runImportExcel(const std::string& pathArg,
                                      const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                      SessionState& session);
// The file `import delimited` (excel=false) or `import excel` (excel=true)
// reads for `pathArg`, with the suffix a stem takes. The dispatcher's r(4)
// check asks it too: a missing file is answered first, r(601).
std::string importSourcePath(SessionState& session, const std::string& pathArg, bool excel);

// -- M4a: output-completion surface (ROADMAP M4a) ----------------------------

// `winsor2 VARLIST [, cuts(p_lo p_hi) trim by(G) suffix(SUF) replace]` — C++
// port of winsor2.py's run_winsor2 (SSC winsorize via cobra eval --winsor).
nlohmann::ordered_json runWinsor2(SessionState& session, const std::vector<std::string>& varlist,
                                   double cutLo, double cutHi, bool trim, const std::string& by,
                                   const std::string& suffix, bool replace);

// `tabulate`/`tab`/`tab1`/`tab2` VARLIST [, chi2 sum(V)|summarize(V)] — C++
// port of tabulate.py's run_tabulate (oneway/twoway freq, tab1/tab2, the
// summarize()-mode per-category mean/sd/N table).
// `weightVar` (Step-1 D3): when set (tokenizer only admits this for
// `[fw=]`/`[fweight=]` on tabulate/tab/tab1/tab2), PIET reads/validates it
// itself (no cobra call underlies native tabulate) and weights every cell
// freq by Σw; missing-weight rows are dropped, non-integer weights error.
// NOT yet honoured for the `sum()` per-category mean/sd/N variant — that
// combination returns an explicit unsupported_option error instead of a
// silent unweighted answer.
nlohmann::ordered_json runTabulate(Datastore& ds, const std::vector<std::string>& varlist,
                                    bool chi2, const std::string& cmd,
                                    const std::optional<std::string>& sumvar,
                                    const std::optional<std::string>& weightVar = std::nullopt,
                                   // BUG-371: `tabulate, missing` — count missing as its OWN
                                   // category rather than dropping those rows. Defaulted so
                                   // every pre-existing call site is byte-identical.
                                   bool includeMissing = false,
                                   // B5-D-TAB-SORT: one-way `, sort` — rows by DESCENDING
                                   // frequency instead of ascending category value. Defaulted
                                   // so every pre-existing call site is byte-identical.
                                   bool sortByFreq = false,
                                   // The typed weight (fw/aw/iw and their long names);
                                   // nullopt reads `weightVar` as an fweight.
                                   const std::optional<std::string>& weightType = std::nullopt,
                                   // BUG-1514: the two-way statistics beyond chi2, a mask of
                                   // the kTab* bits below. 0 keeps every call site byte-identical.
                                   unsigned twoWayStats = 0);

// BUG-1514: tabulate_twoway's lrchi2, V, gamma and taub ([R] tabulate twoway,
// Methods and formulas), the bits of runTabulate's `twoWayStats`.
constexpr unsigned kTabLrchi2 = 1u;
constexpr unsigned kTabCramersV = 2u;
constexpr unsigned kTabGamma = 4u;
constexpr unsigned kTabTaub = 8u;
constexpr unsigned kTabExact = 16u;  // Fisher's exact, served on 2 x 2 tables

// `table ROWVAR [COLVAR] [, statistic(STAT VAR)]` — C++ port of table.py's
// run_table (aggregate statistic by cross-classification).
// BUG-160 CLOSURE: delegates to runTableMulti (below) with a single stat
// spec, so the one-statistic and multi-statistic paths share ONE Total-
// margin/label-header computation — see runTableMulti's own comment for why
// both paths were wrong the same way.
nlohmann::ordered_json runTable(Datastore& ds, const std::string& rowvar,
                                 const std::optional<std::string>& colvar, const std::string& stat,
                                 const std::optional<std::string>& statvar,
                                 SessionState* session = nullptr);

// BUG-160: `table (row) (col), statistic(A) statistic(B) ...` — ONE
// `table_result` with every requested statistic nested under each row
// level, `Total` row/column margins, and value-label-aware column/row
// headers (Stata's `table` prints all three for ONE statistic too, which is
// why runTable above just forwards here with a one-element list rather than
// duplicating the margin/label logic). `statSpecs` is (normalized stat name,
// optional stat variable) in the order the caller wants them nested/emitted.
// `session`, when non-null, supplies value-label lookups
// (session->valueLabels[rowvar-or-colvar]) for a NUMERIC column carrying
// Stata value labels (e.g. `treated` 0/1 -> "no"/"yes") — distinct from a
// genuine factor/string column, which already carries its own dictionary
// via GroupCol::levels. Passing nullptr skips label substitution (numeric
// codes render as their own string).
nlohmann::ordered_json runTableMulti(
    Datastore& ds, const std::string& rowvar, const std::optional<std::string>& colvar,
    const std::vector<std::pair<std::string, std::optional<std::string>>>& statSpecs,
    SessionState* session = nullptr);

// `tabstat VARLIST [, by(g) stats(mean sd ...)]` — direct cobra argv (B9/C10
// translate.py path), estimation-block wrapped. `ifExpr` (BUG-042): tabstat's
// own cobra CLI has no --keepif of its own, so this pre-filters via the same
// `cobra eval --keepif` row-filter infra summarize/count already use.
// B4-W3b: `wantGroups` carries the engine's per-group block onto the result
// (`save`/`nototal` only — see runTabstatCore's comment for why it is not
// unconditional), `noTotal` drops the overall row for `nototal`.
nlohmann::ordered_json runTabstat(const std::vector<std::string>& varlist,
                                   const std::optional<std::string>& byVal,
                                   const std::optional<std::string>& statsVal,
                                   SessionState& session,
                                   const std::optional<std::string>& ifExpr = std::nullopt,
                                   bool wantGroups = false, bool noTotal = false);

// W:P5 — `by G: tabstat VARLIST, statistics(...)`. The by-prefix driver in
// dispatcher.cpp has already materialized this group's slice (its rows ANDed
// with the sub-command's own `if`), so this entry point skips the row-filter
// step runTabstat performs and shares the rest of the body with it. `by()` is
// absent by construction: the prefix owns grouping, and Stata rejects both
// grouping axes at once anyway.
nlohmann::ordered_json runTabstatOnData(const std::vector<std::string>& varlist,
                                         const std::optional<std::string>& statsVal,
                                         SessionState& session, const std::string& dataPath);

// `corr`/`correlate`/`pwcorr` VARLIST [, sig obs pairwise] — C++ port of
// translate.py's build_corr_argv. `ifExpr` (BUG-042): same keepif prefilter
// as runTabstat above (corr's cobra CLI has no --keepif of its own either).
// `inRange` (BS-090, BETA Phase G): `in a/b`, composed with `ifExpr` via AND.
nlohmann::ordered_json runCorr(const std::vector<std::string>& varlist, bool pairwise, bool sig,
                                SessionState& session,
                                const std::optional<std::string>& ifExpr = std::nullopt,
                                const std::optional<std::string>& inRange = std::nullopt);

// `ttest VAR [== MU] [, by(G) unequal level(#)]` / `ttest V1 == V2 [, paired]`
// — C++ port of translate.py's build_ttest_argv.
nlohmann::ordered_json runTtest(const eval::ParsedCommand& parsed, SessionState& session);

// `xtreg Y X..., fe|re [vce(...)]` — fe routes to feols with --fe=panelvar
// (build_estimation_argv("feols", extra={"fe":panelvar})); re routes to
// cobra xtreg with --ivar=panelvar (no vcov support). Requires a prior
// `xtset ivar [tvar]`.
nlohmann::ordered_json runXtreg(const eval::ParsedCommand& parsed, SessionState& session);

// `logit`/`probit` Y X... [, vce(robust)] — C++ port of the _DIRECT_MAP path
// through build_estimation_argv (GLM --vce swap applied).
// --- P7 wave: vector autoregression -----------------------------------------
// One engine verb backs all three; vargranger and irf create re-render the fit.
nlohmann::ordered_json runVar(const eval::ParsedCommand& parsed, SessionState& session);
nlohmann::ordered_json runVargranger(SessionState& session);
nlohmann::ordered_json runIrf(const eval::ParsedCommand& parsed, SessionState& session);

// --- P7 wave: multiple imputation -------------------------------------------
// The mlong rows are written by the engine (--out-data); SessionState keeps only
// the declaration, so `mi impute` can reject on un-set or unregistered data.
nlohmann::ordered_json runMi(const eval::ParsedCommand& parsed, SessionState& session);

// --- P7 wave: dynamic panel -------------------------------------------------
nlohmann::ordered_json runXtabond(const eval::ParsedCommand& parsed, SessionState& session);

// --- P7 wave: nonlinear -----------------------------------------------------
// The parenthesised expression goes to the engine VERBATIM off the raw line —
// PIET must not grow a second parser for it.
nlohmann::ordered_json runNlGmm(const std::string& cobraCmd, const std::string& rawLine,
                                 const eval::ParsedCommand& parsed, SessionState& session);

// --- P7 wave: multilevel ----------------------------------------------------
// Random INTERCEPT only; random slopes and a second `||` level reject.
nlohmann::ordered_json runMixed(const eval::ParsedCommand& parsed, SessionState& session);

// --- P7 wave: survey design -------------------------------------------------
// The design is session state; `svy:` rewrites the inner command into the
// ordinary engine call plus --svy/--psu/--strata. The inner-command whitelist
// fails CLOSED — an unrecognised inner command rejects rather than running
// unweighted.
nlohmann::ordered_json runSvyset(const eval::ParsedCommand& parsed, SessionState& session);
nlohmann::ordered_json runSvyPrefix(const std::string& innerRaw, SessionState& session);

// --- P7 wave: IV maximum likelihood -----------------------------------------
// Shares ivregress's `(endog = instruments)` varlist parser rather than growing
// a second one.
nlohmann::ordered_json runIvMl(const std::string& cobraCmd, const eval::ParsedCommand& parsed,
                                SessionState& session);

// --- P7 wave: survival ------------------------------------------------------
// stset is an engine verb that materializes _t0/_t/_d/_st as real columns;
// SessionState keeps only the declaration, which is what lets stcox/streg reject
// honestly on un-stset data and cluster vce(robust) on the subject id.
nlohmann::ordered_json runStset(const eval::ParsedCommand& parsed, SessionState& session);
nlohmann::ordered_json runStcox(const eval::ParsedCommand& parsed, SessionState& session);
nlohmann::ordered_json runStreg(const eval::ParsedCommand& parsed, SessionState& session);

// --- P7 wave: ordered / multinomial / zero-inflated / fractional MLE ---------
// These take the engine's ML-family `--vce` spelling (not `--vcov`) and each
// honest-rejects the option shapes its estimator does not implement.
nlohmann::ordered_json runOrdered(const std::string& cobraCmd, const eval::ParsedCommand& parsed,
                                  SessionState& session);
nlohmann::ordered_json runMlogit(const eval::ParsedCommand& parsed, SessionState& session);
nlohmann::ordered_json runZeroInflated(const std::string& cobraCmd,
                                       const eval::ParsedCommand& parsed, SessionState& session);
nlohmann::ordered_json runFracreg(const eval::ParsedCommand& parsed, SessionState& session);

nlohmann::ordered_json runLogitProbit(const std::string& cobraCmd, const eval::ParsedCommand& parsed,
                                       SessionState& session);

// `csdid`/`drdid` Y X..., ivar(ID) time(T) gvar(G) [method() comparison()
// pscoretrim() window() cluster()] — C++ port of build_csdid_argv +
// _csdid_to_estimation("simple") (the initial estimation block; estat
// event/calendar/group/pretrend read the cached result back via runEstat).
nlohmann::ordered_json runCsdid(const eval::ParsedCommand& parsed, SessionState& session);

// `ivregress 2sls|gmm|liml depvar [exog...] (endog = instruments) [if] [,
// vce(robust|cluster C)]` / `ivreghdfe depvar [exog...] (endog=instr),
// absorb(F) [vce(...)]` / the `reghdfe depvar (endog=instr), absorb(F)`
// paren form (dispatcher.cpp routes all three here) — BS-063/BS-064 (BETA
// D1). `method` is "2sls"|"gmm"|"liml" (ivregress's own sub-command word;
// ivreghdfe/the reghdfe-paren form always pass "2sls" — neither has a gmm/
// liml sub-variant). `allowAbsorb` gates whether absorb() is read as --fe:
// true for ivreghdfe/reghdfe-paren, false for plain ivregress (which has no
// absorb() option in real Stata — an unknown-option reject otherwise).
// `stataCmd` is the literal Stata command word actually typed ("ivregress"/
// "ivreghdfe"/"reghdfe") — display-label-only, drives the block's
// "estimator" field ("ivregress 2sls"/"ivregress gmm"/"ivregress liml"/
// "ivreghdfe").
// `forceSmall` (B4 P0-C) asks for cobra's `--small` t/F small-sample correction
// independently of `allowAbsorb`. `ivreg2 ..., small` is exactly that request,
// and before this argument existed the caller parsed the flag and discarded it,
// so the option was certified and dropped. `allowAbsorb` still implies --small
// on its own — ivreghdfe reports the absorbed correction by default — so this
// only ever ADDS the flag, never removes it.
// 2026-09-09 (BUG-474): `allowAbsorb` used to mean TWO things — "this spelling accepts
// absorb()" AND "this spelling reports small-sample SEs" — and the second meaning was
// invisible at the call sites. Flipping plain `ivregress` to accept absorb() therefore
// silently moved its standard errors off Stata's ASYMPTOTIC default (D1a beertax se
// 13643.678807996115 -> 13684.467176). The selftest caught it; the fix is to say the two
// things separately. `smallDefault` is the spelling's own convention (ivreghdfe reports the
// absorbed correction by default, plain ivregress does not); `forceSmall` is a user request
// arriving as `ivreg2 ..., small`. Either is sufficient, so the two are OR-ed.
nlohmann::ordered_json runIvRegress(const std::string& method, const eval::ParsedCommand& parsed,
                                     bool allowAbsorb, const std::string& stataCmd,
                                     SessionState& session, bool smallDefault, bool forceSmall);
// NEITHER small flag has a default, deliberately. When `smallDefault` carried one, the ivreg2
// caller inherited it and `ivreg2` bare moved from se[mpg] 65.331433 to 66.232599 — the same
// class of silent value change this split exists to prevent, one layer down. An omitted
// argument must be a compile error, not a different number.

// `reg3 (y1 x1 x2) (y2 x3 y1) [(y3 ...) ...] [, sure]` — BS-065 (BETA D1).
// Stata's own default is 3SLS (`sure` opts into SUR); cobra's reg3 CLI
// default is the OPPOSITE (SUR unless --method 3sls), so this always sends
// --method explicitly rather than relying on either side's default.
// `sureg (y1 x1 x2) (y2 x3)` is the same SUR fit (stataCmd "sureg": SUR always,
// e(cmd) `sureg`; sureg.ado's default is two-step FGLS, no dfk/small).
nlohmann::ordered_json runReg3(const eval::ParsedCommand& parsed, SessionState& session,
                               const std::string& stataCmd = "reg3");

// `boottest paramname [, reps(#) seed(#) weights(rademacher|webb) null(#)]`
// — wild cluster bootstrap post-estimation (BS-066), requires a preceding
// vce(cluster ...) fit in session.lastModel() (honest reject otherwise).
nlohmann::ordered_json runBoottest(const eval::ParsedCommand& parsed, SessionState& session);

// `tobit depvar indepvars [if], ll[(#)] ul[(#)]` — BS-069 (BETA D2). At
// least one of ll()/ul() required; a bare ll/ul (no numeric value) is
// PIET-computed from the depvar's own observed min/max (cobra's tobit CLI
// has no bare-flag equivalent). `level()` is never forwarded (cobra tobit
// has no --level surface at all — honest reject).
nlohmann::ordered_json runTobit(const eval::ParsedCommand& parsed, SessionState& session);

// `heckman depvar indepvars [if], select(selvar = selvars) [twostep]` —
// BS-070 (BETA D2). cobra's heckman implements ONLY the two-step Heckit
// estimator (its bare/no-flag default already computes it — the CLI's own
// `--twostep` flag is NEVER forwarded, it triggers a cobra stdout/exit-code
// masking bug); Stata's default (no `twostep`, full MLE) has no engine
// equivalent — honest reject.
nlohmann::ordered_json runHeckman(const eval::ParsedCommand& parsed, SessionState& session);

// `qreg depvar indepvars [if] [, quantile(#) level(#)]` — BS-071 (BETA D2).
// quantile() accepts a (0,1) fraction or a (1,100) percentage (normalized to
// cobra's --tau); unlike tobit/ivregress/reg3, qreg's `level()` DOES forward
// (live-verified: cobra qreg's --level genuinely changes the CI/SE/t/p).
nlohmann::ordered_json runQreg(const eval::ParsedCommand& parsed, SessionState& session);

// `mediate (Y [controls]) (M [controls]) (T, continuous(#level0 #level1))`
// — BS-072 (BETA D2), Stata 18 syntax. Only the continuous-treatment,
// two-explicit-level form is in beta scope (the oracle's sole working
// invocation); every other treatment shape honest-rejects naming this form.
// ENGINE-PARITY, NOT STATA-PARITY: cobra's `mediate` computes a plain
// linear (no treatment-mediator interaction) per-unit decomposition, while
// Stata's continuous(#L0 #L1) fits a richer interaction model evaluated at
// the specific contrast — verified these are NOT numerically equivalent
// (see the .cpp function-header comment for the full reconstruction/
// disclosure). Point estimates below are the ENGINE's own values, not a
// Stata-matching result.
nlohmann::ordered_json runMediate(const eval::ParsedCommand& parsed, SessionState& session);

// `poisson`/`ppmlhdfe`/`fepois` Y X... [if] [, vce(robust|cluster C)
// offset(O) absorb(F)]` — BS-073 (BETA D3 batch). `stataCmd` is the literal
// command word typed ("poisson"/"ppmlhdfe"/"fepois" — all three route
// through cobra's `fepois`; D0 finding: WITHOUT --fe it reproduces Stata's
// bare `poisson` exactly). absorb() only allowed for ppmlhdfe/fepois (real
// Stata's `poisson` has no absorb() option) -> --fe. offset() is a VERIFIED
// literal passthrough (cobra's --offset expects the same already-logged
// exposure variable Stata's own offset() does — no ln() transform needed).
// ppmlhdfe/fepois (fe form) DEFAULT to vce(robust) when the user gives no
// vce() at all, matching real ppmlhdfe's own default (live-verified against
// the D3d oracle, which never types vce() yet the Stata table header reads
// "Robust std. err."). See runPoisson's header comment (estimation_m4.cpp)
// for the ENGINE FINDING disclosure (HC1 small-sample-correction gap).
nlohmann::ordered_json runPoisson(const std::string& stataCmd, const eval::ParsedCommand& parsed,
                                   SessionState& session);

// `nbreg` Y X... [if] [, vce(robust|cluster C) offset(O) noconstant]` —
// BS-074 (BETA D3 batch), NB2 ("dispersion(mean)", Stata's default). See
// runNbreg's header comment (estimation_m4.cpp) for the ENGINE BLOCKER
// disclosure: cobra's nbreg fails to converge on data whose fitted
// intercept sits away from zero, including the D3e oracle's own data — a
// live-verified engine-side gap, not a wiring gap.
nlohmann::ordered_json runNbreg(const eval::ParsedCommand& parsed, SessionState& session);

// `xtpoisson`/`xtlogit` Y X..., fe [vce(robust)]` — BS-075 (BETA D3 batch).
// `,fe` ONLY: the engine has no random-effects or population-averaged
// xtpoisson/xtlogit estimator, so Stata's own DEFAULT (bare / `,re` / `,pa`
// — real Stata's default for both commands IS random-effects) honest-
// rejects naming `,fe` as the only supported form. Requires a prior
// `xtset ivar [tvar]` (same panelvar spine xtreg's `,fe` router reads).
// vce(cluster ...)/cluster() honest-rejects for both (live-verified silent
// no-op on the engine's own CLI — see the .cpp header comment); vce(robust)
// genuinely changes the SE for both (not a masking no-op). xtpoisson's
// offset() forwards (xtpois's own CLI has --offset); xtlogit's does not.
nlohmann::ordered_json runXtPoisson(const eval::ParsedCommand& parsed, SessionState& session);
nlohmann::ordered_json runXtLogitFe(const eval::ParsedCommand& parsed, SessionState& session);

// `drdid` Y X..., ivar(ID) time(T) treatment(D) dripw` — BS-076 (BETA D3
// batch). The classic two-period/two-group doubly-robust DiD, mirroring
// ONLY the tests/validate/drdid2x2.do fixture's exact invocation surface
// (the sole oracle-covered form; other method()s and the bare/no-`dripw`
// form honest-reject). Routes through the SAME engine csdid --method dripw
// machinery `csdid`/`estat` already use:
// zero cobra-side csdid code touched, only a PIET-side synthesized `gvar`
// column (treatment()!=0 -> gvar=max(time), the post period; ==0 -> gvar=0,
// CS21's never-treated sentinel) built via cobra's GENERIC `eval --gen`
// data-prep verb. Restricted to exactly 2 distinct time() values (the
// command's own "2x2" design, matching real drdid.ado's same restriction).
// See runDrdid's header comment (estimation_m4.cpp) for the live-verified
// oracle match (ATT/SE to ~1e-13 relative).
nlohmann::ordered_json runDrdid(const eval::ParsedCommand& parsed, SessionState& session);

// `test EXPR` (joint or constraint Wald test), `lincom EXPR`, `nlcom [LABEL:]
// EXPR` — all reconstruct the cobra call from session.lastModel(). Returns
// "No previous estimation result" if lastModel is unset.
nlohmann::ordered_json runTestCmd(const std::string& expr, SessionState& session);
// `test ..., accumulate`: the restrictions of the previous `test` on the SAME
// estimation are tested jointly with these (session state; see runTestCmd).
nlohmann::ordered_json runTestCmd(const std::string& expr, SessionState& session,
                                  bool accumulate);
// BUG-367: Stata `testparm varlist` — the joint zero test written the way a
// researcher writes it. It is `test`'s varlist form, not a second estimator: the
// only thing it adds is expanding wildcards and `i.var` against the LAST MODEL's
// coefficient names (not the dataset's variables — a coefficient is what can be
// constrained), after which it delegates to runTestCmd unchanged.
nlohmann::ordered_json runTestparm(const std::vector<std::string>& varlist,
                                   SessionState& session);
nlohmann::ordered_json runLincom(const std::string& expr, SessionState& session,
                                 const std::vector<std::pair<std::string, eval::OptValue>>& opts = {});
nlohmann::ordered_json runNlcom(const std::string& expr, SessionState& session,
                                const std::vector<std::pair<std::string, eval::OptValue>>& opts = {});

// r(table) (w2c_postest family 1) — handlers_aer_estim.cpp. ONE builder for every
// estimation block: rows `b se t|z pvalue ll ul df crit eform`, columns in Stata's
// e(b) order including a factor's base-level column. `level` is the DECLARED
// confidence level (the command's level(#), else `set level`). Returns false (and
// builds nothing) when Stata's table cannot be reproduced exactly — including a
// block whose p-values/intervals disagree with that level and reference.
bool buildRTableFromBlock(SessionState& session, const nlohmann::ordered_json& block,
                          double level, eval::EvalMatrix& out);
// Build and post r(table) from `block`, recording it (with the live e() identity)
// as the estimation machinery's own. False when nothing could be built (nothing
// is posted).
bool postRTableFromBlock(SessionState& session, const nlohmann::ordered_json& block,
                         double level);
// Identity of the live e() block (0 = none). Changes whenever e() is replaced;
// O(1), so dispatch() can take it on every entry.
std::uintptr_t liveEstimationIdentity(const SessionState& session);
// The r-class replacement for every handler that MERGES its keys into r(): if r()
// was written since `mark` (a store generation moved, or an r() matrix was posted
// or erased), erase every r() scalar, user r() macro and r() matrix last written
// at or before it. Reserved \x01 keys (s(), test's accumulate set) stay. True if
// r() was written. See eval/session.h nextStoreGeneration.
bool replaceStaleRClass(eval::EvalSession& ev, const eval::RStoreMark& mark);
// dispatcher.cpp's dispatch() calls this after every command with the e() identity
// and the r() mark it took on entry, and whether the command was a LEAF (no
// dispatch() ran inside it): r() written by the command replaces all of r()
// (replaceStaleRClass; a container is judged from its last inner command on); a
// leaf that replaced e() replaces r() and posts the table of the block it returned;
// a container that replaced e() keeps only a table posted for the e() now live;
// `margins` retires it.
void rTableAfterCommand(SessionState& session, const nlohmann::ordered_json& result,
                        const std::string& rawCmd, std::uintptr_t eIdAtEntry, bool leaf,
                        const eval::RStoreMark& markAtEntry);

// `margins [marginlist] [, dydx(VAR...) | at(...) atmeans post]`. `marginlist` is
// the command's positional terms (the dispatcher's parsed.varlist): one factor
// variable of a least-squares fit gives the predictive margin at each of its
// levels (FU-94). Every served form replaces r() with r(b)/r(V)/r(N) (FU-179).
nlohmann::ordered_json runMargins(const std::vector<std::string>& marginlist,
                                   const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                   SessionState& session);

// What margins needs from a fit to answer over at() points, a marginlist factor
// or atmeans from the fit's own e(b)/e(V) (cecon::margins_grid, posthest.h):
// built by runMargins for a least-squares or logit/probit fit and by
// runGlmMargins for a glm. `names` are e(b)'s column names (no equation prefix,
// the engine's spelling or Stata's), `data` every variable a name's atoms read,
// over the e(sample) rows. `dfR` is e(df_r) when the fit's margins table is
// t-referenced (regress, areg), NaN for z (xtreg, reghdfe, logit, glm).
struct MarginsGridFit {
  std::vector<std::string> names;
  std::vector<double> b, V;  // k, k*k
  int link = 0;              // cecon::GlmLinkFn
  int64_t n = 0;
  std::map<std::string, std::vector<double>> data;
  std::vector<double> weights;  // n fit weights, or empty
  bool fweight = false;         // r(N) is the weight sum
  double dfR = 0.0;
  bool tRef = false;
  bool cluster = false;
  std::string depvar, dataPath;
};

// Serves `margins [factor] [, at() atmeans dydx() post]` through the grid. nullopt
// = a form the grid does not take (dydx(*), a marginlist with at(), several
// marginlist terms); the caller decides whether that is a refusal.
std::optional<nlohmann::ordered_json> runMarginsGrid(
    SessionState& session, const MarginsGridFit& fit, const std::vector<std::string>& marginlist,
    const std::vector<std::pair<std::string, eval::OptValue>>& opts);

// margins' r() for `margins, dydx() [post]` after a glm without an interaction
// (runGlmMargins' own path): r(N) = the block's n_obs, r(b)/r(V) over `names`
// (a factor's base level included, 0 with variance 0), r(table) from
// `tableRows` (one row per name; a base row's se/t/p/ci null), and the
// declared set of the runMargins grid (w2v_margins_siblings C8_gamif_dydx_x).
void postGlmDydxRClass(SessionState& session, const nlohmann::ordered_json& block,
                       const std::vector<std::string>& names, const std::vector<double>& b,
                       const std::vector<std::vector<double>>& V, const nlohmann::ordered_json& tableRows,
                       const std::string& xvars, bool cluster);

// `predict [NEWVAR] [, xb|resid|residuals|fe]` — reconstructs the last model
// and calls cobra predict; unsupported statistics (cooksd/leverage/hat/...)
// are rejected explicitly (mirrors the oracle's _UNSUPPORTED_PREDICT_STATS
// allow-list — never silently defaults to xb).
// BUG-516: computes on the LIVE frame and adds exactly one column to it (it used
// to adopt the estimation-time frame as the new head). BUG-517: `ifExpr` is the
// command's `if` qualifier -- rows that fail it get missing, as in Stata.
nlohmann::ordered_json runPredict(const std::vector<std::string>& varlist,
                                   const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                   SessionState& session,
                                   const std::optional<std::string>& ifExpr = std::nullopt);

// `estat SUBCMD [...]` — two families: (i) hettest/ovtest/vif/dwatson/
// bgodfrey/archlm rewrite to the equivalent top-level diagnostic command
// (injecting lastModel's y+x when the estat call has no varlist of its own);
// (ii) simple/event/calendar/group/pretrend read back the cached csdid
// result. `rawVarlistAfterSub` is the token list AFTER the subcommand word
// (e.g. `estat hettest x1 x2` -> ["x1","x2"]).
nlohmann::ordered_json runEstat(const std::string& subcmd,
                                 const std::vector<std::string>& rawVarlistAfterSub,
                                 const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                 const std::optional<std::string>& ifExpr, SessionState& session);

// -- BETA campaign Phase D batch D4/D5: time-series + post-estimation/diag/
// misc wirings (BS-077/078/079/080/081/085/021/082/086/083/084/087). Oracle:
// internal capture d45-oracle-stata.log
// (StataBE 19.5 on piet/packaging/sample/fatalities.csv, `gen high=(beertax
// >.5)` + `encode state, gen(snum)` + `gen t=_n` + `tsset t`; sections
// D4a-D4f, D5a-D5g). See each handler's own .cpp header comment
// (estimation_m4.cpp) for the full oracle-match / engine-gap disclosure.

// `newey depvar indepvars [if], lag(#)` — BS-077. lag() is REQUIRED (matches
// real Stata, which errors "option lag() required" if omitted); the time
// variable is read from the session's own `tsset timevar` spine.
nlohmann::ordered_json runNewey(const eval::ParsedCommand& parsed, SessionState& session);

// `prais depvar indepvars [if] [, corc twostep rhotype(regress|tscorr)]` —
// BS-078. Requires a prior `tsset timevar`.
nlohmann::ordered_json runPrais(const eval::ParsedCommand& parsed, SessionState& session);

// `dfuller varname [if] [, lags(#) noconstant|drift|trend]` / `pperron
// varname [if] [, lags(#) noconstant|trend]` — BS-079. `cobraCmd` selects
// which engine CLI ("dfuller"/"pperron"); pperron has no `drift` form in
// real Stata (honest-rejected there, unlike dfuller). Requires a prior
// `tsset timevar`.
nlohmann::ordered_json runDfullerPperron(const std::string& cobraCmd,
                                          const eval::ParsedCommand& parsed, SessionState& session);

// `wntestq varname [if] [, lags(#)]` — BS-080. No timevar requirement (the
// engine's own CLI has no --timevar flag — rows are read in on-disk order).
// The engine reproduces Stata's own default lag rule (min(n/2-2,40))
// automatically when lags() is omitted — PIET never computes it itself.
nlohmann::ordered_json runWntestq(const eval::ParsedCommand& parsed, SessionState& session);

// `sdtest varname [if], by(groupvar)` (two-sample variance-ratio test) /
// `sdtest varname [if] = #` (one-sample, against a fixed SD) — BS-085.
// Exactly one of by()/`= #` is required. The `= #` form has no oracle
// coverage this batch — wired but not Stata-pinned.
nlohmann::ordered_json runSdtest(const std::string& varName,
                                  const std::optional<std::string>& againstVal,
                                  const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                  const std::optional<std::string>& ifExpr, SessionState& session);

// `hausman NAME1 NAME2` — BS-083. Validates both eststo/estimates-store
// names exist and are the `xtreg ,fe`/`xtreg ,re` pair of the SAME spec
// (same depvar, same regressor set, current xtset panelvar matches the fe
// store's own absorbed variable), then calls cobra's own `hausman --y --x
// --ivar` directly (the engine recomputes BOTH sides from raw data itself).
// See the .cpp header comment for the eststo-snapshot limitations this
// works around (no stored ivar/dataPath — uses the CURRENT xtset context
// and currently-loaded dataset, a disclosed workflow assumption).
nlohmann::ordered_json runHausman(const std::string& name1, const std::string& name2,
                                   const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                   SessionState& session);

// `chow depvar indepvars [if], by(groupvar)` — BS-084. A PIET-ONLY
// convenience wrapper (real Stata has NO `chow` command at all — the
// oracle's own D5f section computes it by hand: pooled vs split-sample SSR
// F-test); routes to cobra's own `chow --by` CLI, which computes the
// IDENTICAL classic homoskedastic F test as its "classic" block.
nlohmann::ordered_json runChow(const eval::ParsedCommand& parsed, SessionState& session);

// `mkspline stub = varname [, cubic [nknots(#) | knots(v1,v2,...)]]` — BS-087
// (part). ONLY the `cubic` (restricted cubic spline) form is supported —
// real Stata's linear/piecewise-linear default (no `cubic` keyword) has no
// engine equivalent (live-verified: the engine ALWAYS computes a cubic
// basis regardless of the keyword) and is honest-rejected rather than
// silently answered with a cubic result. `if` is honest-rejected too (the
// engine's --keepif DROPS rows from the whole dataset rather than leaving
// the new spline vars missing, unlike real Stata's if-qualified generate-
// family semantics). See the .cpp header comment for the nknots(#) ENGINE
// BUG disclosure (flagged for Auto Patcher, not fixed here).
nlohmann::ordered_json runMkspline(const std::string& stubIn, const std::string& srcVarIn,
                                    const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                    const std::optional<std::string>& ifExpr, SessionState& session);

// -- M4a item 3: stored estimates + tables (ROADMAP M4a) ---------------------

// `eststo [NAME]` / `est store [NAME]` / `estimates store [NAME]` — snapshot
// the current estimation result (session.lastEstBlock, falling back to
// lastModel().result) into the named estimates store. C++ port of
// dispatcher.py's "eststo" handler + esttab.py's make_snapshot.
// B4-W3b: `title("...")` is a real `estimates store` option (73 corpus uses,
// `est store r`i', title("")`). Before this it was in kVocabEstimates's
// UNIMPLEMENTED set and answered r(198); it now lands on the snapshot under
// the "title" key and `estimates dir` prints it. It is NOT coefplot's
// `title()` — that one (eststo_esttab.cpp:817) is the plot's own graph title,
// read from coefplot's opts and never from a stored snapshot.
// W2 eststoprefix (BUG-521): `autoPrefix` is eststo's `prefix()` option (the
// stem of the auto-name, estout's default "est"), and `viaEststo` says whether
// the call is estout's `eststo` (which owns the auto-name counter) or official
// `estimates store` (which never touches it) -- see SessionState::eststo.
nlohmann::ordered_json runEststo(const std::optional<std::string>& name,
                                 const std::optional<std::string>& title, SessionState& session,
                                 const std::string& autoPrefix = "est", bool viaEststo = true);

// `estimates save FILE [, replace]` / `estimates use FILE` (B4-W3b, BUG-470).
// The round trip Stata runs at rc 0 on every link: save writes a PIETEST1
// envelope carrying BOTH the e()-block snapshot AND the full LastModel, and
// use restores both — so `predict` after a `use` predicts off the RESTORED
// model, not off whatever regression happens to still be live. That last part
// is the whole point of the entry: an rc-only round trip passes for the wrong
// reason when a model is still in memory.
nlohmann::ordered_json runEstimatesSave(const std::string& fileArg, bool replace,
                                        SessionState& session);
nlohmann::ordered_json runEstimatesUse(const std::string& fileArg, SessionState& session);

// `eststo dir` / `est dir` / `estimates dir` — list stored estimate names.
nlohmann::ordered_json runEststoDir(SessionState& session);
// Every stored estimate and the `_est_NAME` variables written for them, as
// `estimates clear` and `clear matrix` (its `_est drop _all`) drop them (BUG-1212).
void clearStoredEstimates(SessionState& session);
// Empty every e() store a later reader can see: `ereturn clear`, and permute and
// jackknife before what they post (handlers_ereturn.cpp).
void clearEResults(SessionState& session);

// `eststo drop|clear [NAME]` / `est drop|clear [NAME]` / `estimates
// drop|clear [NAME]`.
nlohmann::ordered_json runEststoManage(const std::string& sub, const std::optional<std::string>& name,
                                        SessionState& session);

// `esttab [NAME...] [, se|t b(%fmt) star(...) nostar label]` — multi-column
// regression table from the stored estimates (all stored models if no names
// given). C++ port of esttab.py's parse_esttab_opts + render_esttab.
nlohmann::ordered_json runEsttab(const std::vector<std::string>& modelNames,
                                  const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                  SessionState& session);

// `estimates table [NAME...] [, b se]` (P4 wave) — Stata's own side-by-side
// comparison of stored estimates, distinct from `esttab` (a user-written
// package with a different layout). Coefficient rows are the UNION of the
// models' names in first-seen order; a model missing a coefficient leaves its
// cell blank rather than printing a zero, because a blank means "not in this
// model" while a 0 would read as an estimated null effect. With no names, the
// whole store is used, matching Stata.
nlohmann::ordered_json runEstimatesTable(const std::vector<std::string>& modelNames,
                                          const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                          SessionState& session);

// `coefplot [NAME...] [, drop() keep() xline() level() title() xtitle()
// ytitle()]` — coefficient-plot graph block from the stored estimates (or
// session.lastModel() as a single-model "last" fallback when the store is
// empty). C++ port of coefplot.py's run_coefplot.
// coefplot-family AN-1..AN-4 (handlers_coefplot.cpp): coefplot 1.8.8's data
// pipeline -- the subgraph/plot/model grammar, keep/drop/order/rename/levels/
// ci()/matrix(), generate(), r(). Returns the blocks to emit before the graph
// (notes, the generate() mutation) or a refusal; `drawRows` receives the
// plotted rows (empty when there is nothing to plot).
nlohmann::ordered_json runCoefplotJann(SessionState& session, const eval::ParsedCommand& parsed,
                                       nlohmann::ordered_json* drawRows);

nlohmann::ordered_json runCoefplot(const std::vector<std::string>& names,
                                    const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                    SessionState& session);

// -- M4a item 4: graph JSON surface (ROADMAP M4a) -----------------------------
// Block STRUCTURE/JSON only (matches the frozen corpus's schema) — SVG/PNG
// rendering is frontend M4b scope. C++ ports of piet/backend/piet/graph.py's
// run_scatter/run_histogram/run_kdensity/run_line/run_xtline/run_tsline.
//
// `ds` is the (possibly `if`-filtered, temp-hbin-backed) Datastore the
// dispatcher resolves BEFORE calling these — mirrors dispatcher.py's own
// if_expr -> temp-hbin-swap done once, upstream of the graph_cmd branch.

nlohmann::ordered_json runScatter(Datastore& ds, const std::vector<std::string>& varlist,
                                   const std::vector<std::pair<std::string, eval::OptValue>>& opts);

// gfx-histogram-kdensity-family (oracle aer_w2 w2aq_histogram_family): ONE
// binning rule ([R] histogram) and ONE density rule ([R] kdensity, Methods and
// formulas). histogram/hist, twoway__histogram_gen and kdensity read them; the
// twoway histogram/kdensity layers read the same.
struct HistGenSpec {
  std::string varname;            // for start()'s message
  bool discrete = false;
  std::optional<int64_t> bins;    // bins()/bin(), integer > 0
  std::optional<double> width;    // width(), > 0
  std::optional<double> start;    // start()
  std::string type = "density";   // density | fraction | frequency | percent
  bool genFloat = true;           // generated values are stored as c(type) (float by default)
};
struct HistGenResult {
  int rc = 0;                     // 198: start() above the minimum
  std::string message;
  bool empty = false;             // r(N) == 0: Stata's empty-sample results
  double area = 0, max = 0, min = 0, start = 0, width = 0, N = 0;
  int64_t bin = 1;
  int64_t nx = 0;                 // r(n_x): rows of the generated centre
  bool userStart = false;
  bool closingRow = false;        // the extra (x = start, y = .) row
  std::string type;
  // generate(): one entry per OCCUPIED bin, ascending; values as stored
  // (float-rounded under c(type) float). `centre` may carry one more entry,
  // the closing row.
  std::vector<double> binIndex, count, centre, height;
};
HistGenResult histogramGen(const std::vector<double>& v, const std::vector<double>* w,
                           const HistGenSpec& spec, int64_t nDataRows, bool wantGenerate);

struct KdensitySpec {
  std::string kernel = "epanechnikov";
  double width = 0.0;   // 0 = the default bandwidth
  int64_t n = 50;
  std::string wtype;    // "" | fweight | aweight | iweight
};
struct KdensityResult {
  std::string kernel;
  double bwidth = 0, scale = 0, mean = 0, sd = 0;
  int64_t n = 0;
  bool nCapped = false;            // n() > _N: "(n() set to #)"
  std::vector<double> grid, density;  // grid mode: n points; at(): one per row
};
// The kernel K(z) by its full name; NaN for an unknown name.
double kdeKernel(const std::string& kernel, double z);
// The kernel a typed kernel() word names (Stata's abbreviations), nullopt when invalid.
std::optional<std::string> kdensityKernelName(const std::string& typed);
// The kernel density estimate at each point of `at` (NaN points stay NaN).
std::vector<double> kdensityAt(const std::vector<double>& x, const std::vector<double>& w,
                               const std::string& wtype, const std::string& kernel, double h,
                               const std::vector<double>& at);
// kdensity's default bandwidth ([R] kdensity), with the summarize min/max/mean/sd it reads.
double kdensityDefaultWidth(const std::vector<double>& x, const std::vector<double>& w,
                            const std::string& wtype, double* outMin, double* outMax,
                            double* outMean, double* outSd);
KdensityResult kdensityEstimate(const std::vector<double>& x, const std::vector<double>& w,
                                const KdensitySpec& spec, int64_t nDataRows,
                                const std::vector<double>* at);

// `histogram`/`hist`: `ds` is the (if-filtered) frame. Returns the graph block
// or an error block; `pre` receives the blocks Stata prints before the graph
// (the `(bin=#, start=#, width=#)` note).
// serset-twoway HIST-1: `sersets`, when given, receives the graph's sersets in
// plot order (histogram or by-panels, kdensity overlay, normal overlay).
nlohmann::ordered_json runHistogram(SessionState& session, Datastore& ds,
                                     const std::vector<std::string>& varlist,
                                     const eval::ParsedCommand& parsed,
                                     std::vector<nlohmann::ordered_json>* pre,
                                     std::vector<serset::Serset>* sersets = nullptr);

// `kdensity`: the graph block (empty object under nograph; *graphDrawn says
// which) or an error block; posts r() and writes generate(). `pre` receives
// the `(n() set to #)` note and the generate() mutation block.
// serset-twoway HIST-1: `sersets` receives the drawn graph's sersets (the
// density line, then the normal overlay); none under nograph.
nlohmann::ordered_json runKdensity(SessionState& session, Datastore& ds,
                                    const std::vector<std::string>& varlist,
                                    const eval::ParsedCommand& parsed,
                                    std::vector<nlohmann::ordered_json>* pre, bool* graphDrawn,
                                    std::vector<serset::Serset>* sersets = nullptr);

// A graph command's saving(filename[, replace]): writes the block as `graph
// save` does, without registering a name. Null on success, else an error block.
// serset-twoway GST: `sersets`, when given, are written beside the block
// (doc["sersets"]) so `graph use` can restore them. BUG-1529: on success
// `savedNote` (when given) receives the `file ... saved` note to print after the
// graph.
nlohmann::ordered_json saveGraphOption(const nlohmann::ordered_json& block, const std::string& savingText,
                                       SessionState& session,
                                       const std::vector<serset::Serset>* sersets = nullptr,
                                       nlohmann::ordered_json* savedNote = nullptr);

// graph-family AI-1/AI-2 (graph_bar.cpp): `graph bar|hbar|box|hbox` over `ds`
// (the if/in-filtered frame), `args` = the words after the subcommand (the
// statlist). `sets` holds the graph's one serset (`serset use`), `block` the
// graph block or the refusal.
struct BarGraphResult {
  nlohmann::ordered_json block;
  std::vector<serset::Serset> sets;
  bool ok = false;
};
BarGraphResult runGraphBarBox(SessionState& session, Datastore& ds, const std::string& kind,
                              const std::vector<std::string>& args, const eval::ParsedCommand& parsed);

// graph-family AI-4/AI-6/AI-7 (graph_store.cpp): `graph describe [name |
// "file.gph"]` (argText = the text after the subcommand, before the comma),
// `graph copy|rename [old] new [, replace]`, `graph display|draw [name] [,
// xsize() ysize() ...]`. Each returns the blocks to emit.
nlohmann::ordered_json runGraphDescribe(SessionState& session, const std::string& argText);
nlohmann::ordered_json runGraphCopyRename(SessionState& session, bool rename,
                                          const std::vector<std::string>& names, bool replace);
nlohmann::ordered_json runGraphDisplay(SessionState& session, const std::string& nameArg,
                                       const std::vector<std::pair<std::string, eval::OptValue>>& opts);

// serset-twoway GST: `graph use filename [, name(nm[, replace]) nodraw ...]`
// (gr_use.ado): opens filename (.gph appended to a name without a suffix;
// r(601) `file <path> not found`), registers the graph under name() or the
// file's base name and restores its sersets (w2al tf_st_use_sc `2|1|0 1|`,
// log:5100-5118). Returns the blocks to emit.
nlohmann::ordered_json runGraphUse(const std::string& fileArg,
                                   const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                   SessionState& session);

// `twoway__histogram_gen` (Stata's internal program histogram calls): an ARRAY
// of blocks (the display note, the generate() mutation) or one error object.
nlohmann::ordered_json runTwowayHistogramGen(const eval::ParsedCommand& parsed, SessionState& session);

nlohmann::ordered_json runGraphLine(Datastore& ds, const std::vector<std::string>& varlist,
                                     const std::vector<std::pair<std::string, eval::OptValue>>& opts);

nlohmann::ordered_json runXtline(Datastore& ds, const std::vector<std::string>& varlist,
                                  const std::optional<std::string>& panelvar,
                                  const std::optional<std::string>& timevar,
                                  const std::vector<std::pair<std::string, eval::OptValue>>& opts);

nlohmann::ordered_json runTsline(Datastore& ds, const std::vector<std::string>& varlist,
                                  const std::optional<std::string>& timevar,
                                  const std::vector<std::pair<std::string, eval::OptValue>>& opts);

// `twoway (plottype ...) (plottype ...) ...` — the parenthesised multi-layer
// form (M4a-remaining, closed by the analysis.do acceptance campaign). C++
// port of graph.py's run_twoway_layers(): renders each parsed LayerSpec
// (piet::twoway::parseTwoway output) against `dsPtr` (the caller resolves
// per-command `if`-filtering exactly as the bare-twoway path does; per-LAYER
// `if` composition happens INSIDE this function, mirroring the oracle).
// `dsPtr == nullptr` -> no_data_loaded. Returns a `kind:"twoway"` graph block
// (or an error block) — same shape buildSvgTwoway() (graph_m4.cpp's SVG
// structural builder) already renders.
// serset-twoway SS: `sersetPlots`, when given, receives one serset::PlotSpec per
// drawn layer, in layer order (the input of serset::buildPlotSersets).
// serset-twoway IN: `globalIn` is the `in` range after the last group; it
// intersects each layer's own `in` and `if`.
nlohmann::ordered_json runTwowayLayers(
    Datastore* dsPtr, SessionState& session, const std::vector<twoway::LayerSpec>& layers,
    const std::vector<std::pair<std::string, eval::OptValue>>& globalOpts,
    const std::optional<std::string>& globalIf,
    std::vector<serset::PlotSpec>* sersetPlots = nullptr,
    const std::optional<std::string>& globalIn = std::nullopt);

// `marginsplot` — reads session.lastMargins (cached by runMargins), plots one
// dot + 95% CI whisker per AME. C++ port of dispatcher.py's _run_marginsplot.
nlohmann::ordered_json runMarginsplot(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                       SessionState& session);

// -- M4b: graph save/combine/export (ROADMAP M4a-remaining + M4b item 4) ----

// `graph save [NAME] ["file.gph"] [, replace]` — register session.lastGraph
// (or an already-named graph) under NAME in session.namedGraphs, optionally
// persisting a JSON `.gph` sidecar ({"_magic":"PIETGPH1","block":<block>}).
// C++ port of dispatcher.py's `graph_save` handler branch.
nlohmann::ordered_json runGraphSave(const std::optional<std::string>& name,
                                     const std::optional<std::string>& filepath,
                                     const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                     SessionState& session);

// `graph combine NAME1 NAME2 ...` — tile N panels into a grid
// (`kind:"graph_combine"`), sets session.lastGraph. Each token resolves as
// EITHER an in-memory named graph (session.namedGraphs) OR, if no such name
// exists, an on-disk `.gph` spec-JSON path (loadGphFile, graph_m4.cpp) —
// BS-007/D3: analysis.do's loop-then-`graph save`-then-combine idiom passes
// FILE PATHS here (a Stata tempfile local reused every loop iteration means
// the in-memory name only ever holds the LAST iteration's graph). The .gph
// loader is a plain JSON deserialize — NEVER a raster/frozen-pixel format
// (D3 hard-stop: no more than serialize+load). C++ port of
// graph_combine.py's run_graph_combine, extended per BS-007.
nlohmann::ordered_json runGraphCombine(const std::vector<std::string>& names,
                                        const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                        SessionState& session);

// `graph export FILE[.svg|.png|.pdf] [, as(svg|png|pdf) replace width() height()]`
// — reads session.lastGraph. Format resolves from `as()` if given (BS-012),
// else the filename's extension. `svg`: pure-C++ structural SVG matching
// graph_export.py's `_build_svg*` geometry exactly (W=520,H=360, margins
// {34,18,48,62}) — scatter/histogram/line/twoway/graph_combine/coefplot/
// marginsplot kinds. `png`/`pdf`: real webview offscreen rasterization is
// deferred post-beta (D8) — PRE-AUTHORIZED FALLBACK writes an honest `.svg`
// file (basename-substituted, never SVG bytes under the requested .png/.pdf
// name) plus an explicit "rasterizer pending" warning block. C++ port of
// graph_export.py's run_graph_export (SVG half), extended per BS-012.
nlohmann::ordered_json runGraphExport(const std::string& filepathArg,
                                       const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                       SessionState& session);

// graph-family AI-13 (graph_export_formats.cpp): the structural SVG of a graph
// replayed as `fmt` ("png" | "pdf" | "eps"/"ps"); width/height in pixels for a
// png (0 = derived). False for any other format.
bool graphSvgToFormat(const std::string& svg, const std::string& fmt, int widthPx, int heightPx,
                      std::string* out);

// Builds the structural SVG string for one graph block (dispatches on
// block["kind"]). Exposed for runGraphExport and (if ever needed) direct
// testing; matches graph_export.py's `_build_svg` dispatcher exactly.
std::string buildGraphSvg(const nlohmann::ordered_json& block);

// L2-FIX-B (BUG-066 / C3-F5): thin a graph block's plotted point arrays for the
// LIVE VIEW ONLY (order-preserving proportional stride), tagging
// n_shown_points / n_total_points when anything was thinned so the frontend can
// draw the honest "(showing N of M points)" caption. MUST be called only AFTER
// the full block is cached into session.lastGraph / namedGraphs -- `graph
// export` and the SVG goldens read those and keep every point. No-op on
// non-graph blocks and on blocks already under the view budget.
void decimateGraphBlockForView(nlohmann::ordered_json& block);

// -- Arc F: the seven QE / RD commands (handlers_qe.cpp) ---------------------
// Each parses its Stata line, calls the already-merged cobra verb, and renders
// the table Stata prints. All seven return an ARRAY of blocks (one `display`
// per printed line, then ONE structured `qe_result` block) — the
// handlers_resample.cpp shape, NOT the single-block shape runCsdid/runMixed
// use — so the dispatcher call sites splice rather than push_back. binscatter
// is the deliberate exception that prints zero lines: real Stata prints none,
// so its array carries the structured block only.
nlohmann::ordered_json runRdrobust(const eval::ParsedCommand& parsed, SessionState& session);
nlohmann::ordered_json runRddensity(const eval::ParsedCommand& parsed, SessionState& session);
nlohmann::ordered_json runRdplot(const eval::ParsedCommand& parsed, SessionState& session);
nlohmann::ordered_json runBinscatter(const eval::ParsedCommand& parsed, SessionState& session);

// THE varlist-name resolution of the by/bysort keys (dispatcher.cpp, BUG-532):
// wildcards and ranges expand through loops::expandVarlist, an exact name
// stays, a unique prefix stands for its variable, several prefix matches are
// r(111) "<tok> ambiguous abbreviation"; a token that matches nothing is left
// as typed. `names` is rewritten in place; the return is an error block or
// nullopt. binscatter's by()/controls()/absorb() use it too (FU-1088).
std::optional<nlohmann::ordered_json> resolveByKeyNames(SessionState& session,
                                                        std::vector<std::string>& names);
nlohmann::ordered_json runEventstudyinteract(const eval::ParsedCommand& parsed,
                                              SessionState& session);
nlohmann::ordered_json runDidImputation(const eval::ParsedCommand& parsed, SessionState& session);
nlohmann::ordered_json runSdid(const eval::ParsedCommand& parsed, SessionState& session);

// ---------------------------------------------------------------------------
// `vce()`'s ARGUMENT abbreviates, and not uniformly. Measured
// (tests/eval/golden/aer_w1/w1b_vce_abbrev.log, 26 arms on `regress`):
//
//   robust    vce(r) .. vce(robust)        ALL rc 0  -- ONE character is enough
//   cluster   vce(cl x) .. vce(cluster x)  rc 0      -- but vce(c x) is r(198)
//   bootstrap vce(bootstrap) only          vce(boo)/vce(b) are r(198)
//   jackknife vce(jackknife) only          vce(j) is r(198)
//   oim/opg   r(198) OUTRIGHT              regress does not take them at all
//
// `vce(r)` alone is 83 corpus uses in one paper, and this build answered every
// one of them `Unknown vce type: r` while the BARE `robust` option abbreviated
// fine -- the gap was the sub-argument. So the floors are the measured ones, per
// type: a prefix of `robust` at least 1 long, a prefix of `cluster` at least 2.
// Nothing is added on a guess: `bootstrap`/`jackknife`/`hc2`/`hc3` are valid Stata
// this build does not implement and stay refused BY NAME, and `oim`/`opg` are not
// added at all because the oracle refuses them here.
//
// Declared here because THREE translation units recognised vce types
// independently -- estimation_m4.cpp, estimation_m4b.cpp (twice) and handlers.cpp,
// each with its own `lowerM4(v) == "robust"` and `rfind("cluster", 0)`. That is
// why `vce(r)` could only be fixed in one place by being fixed in one place.
//
// Returns the CANONICAL type for a recognised spelling, or "" otherwise. `rest`
// receives whatever followed the type token (the cluster varlist), trimmed.
//
// The case tolerance is DELIBERATELY unchanged: Stata is case-sensitive here
// (`vce(R)` is r(198)) and this build lowercases, so it accepts a spelling Stata
// refuses. The corpus has ZERO uppercase `vce(` uses, so narrowing it would only
// risk the callers that depend on the tolerance for no measured gain -- named,
// not silently kept.
inline std::string canonicalVceType(const std::string& raw, std::string* rest) {
  if (rest != nullptr) rest->clear();
  size_t a = raw.find_first_not_of(" \t");
  if (a == std::string::npos) return std::string();
  size_t e = raw.find_first_of(" \t", a);
  std::string tok = raw.substr(a, (e == std::string::npos ? raw.size() : e) - a);
  if (rest != nullptr && e != std::string::npos) {
    const size_t b = raw.find_first_not_of(" \t", e);
    if (b != std::string::npos) {
      size_t z = raw.find_last_not_of(" \t");
      *rest = raw.substr(b, z - b + 1);
    }
  }
  for (auto& c : tok) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  auto isPrefixOf = [&](const char* full, size_t minLen) {
    const std::string f(full);
    return tok.size() >= minLen && tok.size() <= f.size() && f.compare(0, tok.size(), tok) == 0;
  };
  if (isPrefixOf("robust", 1)) return "robust";
  if (isPrefixOf("cluster", 2)) return "cluster";
  if (tok == "hc1") return "hc1";
  if (tok == "iid") return "iid";
  return std::string();
}

// The vce types Stata accepts on `regress` that this build does NOT implement.
// Measured rc 0 in the oracle and r(198) here (w1b_vce_abbrev.log v_boot/v_jack/
// v_hc2/v_hc3), so they are a NAMED refusal rather than `Unknown vce type` --
// "unknown" is the wrong diagnosis for a real Stata option, which is the whole
// point of the vocabulary split this campaign runs on.
//
// EXACT spellings only, because that is what the oracle says: `vce(boo)` and
// `vce(b)` are r(198) in Stata too, and `vce(j)` likewise -- unlike `robust`,
// these do not abbreviate at all. Recognising an abbreviation here would invent
// a refusal Stata does not give.
inline std::string unimplementedVceType(const std::string& raw) {
  size_t a = raw.find_first_not_of(" \t");
  if (a == std::string::npos) return std::string();
  size_t e = raw.find_first_of(" \t", a);
  std::string tok = raw.substr(a, (e == std::string::npos ? raw.size() : e) - a);
  for (auto& c : tok) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (tok == "bootstrap" || tok == "jackknife" || tok == "hc2" || tok == "hc3") return tok;
  return std::string();
}

// ---------------------------------------------------------------------------
// esttab / estout `transform()` -- 18 corpus uses over four papers (12 on estout
// in paper 016, 6 on esttab in 015/083/104), and measurably ONE option: every
// esttab arm in tests/eval/golden/aer_w1/w1b_transform2.log has an estout twin
// with the same numbers (y3/y4/y5/y6).
//
// Declared HERE rather than kept private to handlers_aer_tables.cpp because the
// two writers build their rows in different translation units --
// eststo_esttab.cpp for esttab, handlers_aer_tables.cpp for estout -- and
// BUG-496 is what a rule implemented twice in exactly that shape costs. One
// semantics, two call sites, both pinned by selftest arms.
//
// The grammar is estout `_TransformCoefs`, read before implementing and
// then measured: the value is a flat whitespace-separated list taken as TRIPLES
// (pattern, f, df), and a trailing group of two is (f, df) with no pattern -- so
// a one- or two-token list applies to every row.
struct TransformGroup {
  std::string pat;  // coefficient pattern; empty or `*` matches every row
  std::string f;    // REPLACES the coefficient; `@` is that coefficient
  std::string df;   // MULTIPLIES the se by abs(); `@` is the COEFFICIENT, not
                    // the se. Empty leaves the se alone -- measured, not
                    // assumed (w1b_transform.log r1 doubles b and holds se).
};

struct TransformSpec {
  std::vector<TransformGroup> groups;
  // Per-model words from `pattern()`. EMPTY means every model, which covers both
  // an absent pattern() and an explicitly empty one (w1b_transform2.log z4); a
  // word missing for a model means "do not transform it" (z1, where pattern(1)
  // over two models leaves the second alone).
  std::vector<std::string> pattern;
  bool empty() const { return groups.empty(); }
  bool appliesToModel(size_t i) const {
    if (pattern.empty()) return true;
    return i < pattern.size() && pattern[i] == "1";
  }
};

// Parses the INSIDE of `transform(...)`; false + `err` for what estout itself
// rejects (a pattern() word that is neither 0 nor 1 is r(198) there).
bool parseTransformOpt(const std::string& raw, TransformSpec* out, std::string* err);

struct TransformCell {
  bool matched = false;
  bool bMissing = false;           // the expression did not evaluate finitely
  std::optional<double> b;         // the new coefficient
  std::optional<double> seFactor;  // multiply the se by this; nullopt = leave it
};

// One (model, coefficient) cell. `snap` is THAT model's own snapshot and is made
// current while the expression is evaluated, because `_b[]` inside a transform
// resolves per model: `transform(@/(-_b[weight]))` over two models divides each
// column by ITS OWN weight coefficient (w1b_transform2.log y1).
TransformCell transformOneCell(const TransformSpec& spec, size_t modelIdx,
                               const std::string& coefName, double b,
                               const nlohmann::ordered_json& snap, SessionState& session);

// ---- AER tier-1 (2026-10-28), handlers_t1_*.cpp ----------------------------
// The difficulty-1 bundle: every command below is wiring over pieces that
// already existed. Answers: tests/eval/golden/aer_w2/w2ax_t1_official.log and
// w2ax_t1_community.log. One TU per lane, each lane writing only its own file.
//
// handlers_t1_zip.cpp -- the ZIP verbs over zip_io.h.
// `unzipfile zipfile[.zip] [, replace]`; existing files are skipped without
// replace (rc 0, "can't overwrite ... skipping").
nlohmann::ordered_json runUnzipfile(const eval::ParsedCommand& parsed, SessionState& session);
// `zipfile file [file ...], saving(name[, replace])` -- stored, not deflated.
nlohmann::ordered_json runZipfile(const eval::ParsedCommand& parsed, SessionState& session);
// `zipuse [varlist] using file | zipuse file [, clear]` (zipsave package).
nlohmann::ordered_json runZipuse(const eval::ParsedCommand& parsed, SessionState& session);
// `zipsave file [, replace]` (zipsave package) -- writes <file>.dta.zip.
nlohmann::ordered_json runZipsave(const eval::ParsedCommand& parsed, SessionState& session);
//
// handlers_t1_env.cpp -- macro and program-environment verbs.
// `macro drop|define|shift|list|dir ...`, consumed as a raw line before the
// tokenizer (whose unsupported_macro net names `macro`). Called twice, like
// tryTempDeclIntercept: on the raw line (`expanded` false, which defers any
// line still holding `$` or a backtick) and on the macro-expanded line.
// Returns true when it consumed the line, with the reply in `out`.
bool tryMacroVerbIntercept(const std::string& line, SessionState& session,
                           const nlohmann::ordered_json& echo, nlohmann::ordered_json& out,
                           bool expanded);
// `require pkg[op version] ... [, install strict]` (Correia/Seay) -- accept-note.
nlohmann::ordered_json runRequire(const eval::ParsedCommand& parsed, SessionState& session);
// `_dots 0, title() reps()` header and `_dots # rc` progress marks.
nlohmann::ordered_json runDots(const eval::ParsedCommand& parsed, SessionState& session);
// `postutil clear | dir`.
nlohmann::ordered_json runPostutil(const eval::ParsedCommand& parsed, SessionState& session);
// `markout markvar [varlist] [, strok]`.
nlohmann::ordered_json runMarkout(const eval::ParsedCommand& parsed, SessionState& session);
// `lrtest name1 [name2] [, df() force]` -- sets r(chi2) r(df) r(p).
nlohmann::ordered_json runLrtest(const eval::ParsedCommand& parsed, SessionState& session);
//
// handlers_t1_meta.cpp -- verbs that read or rename, never compute.
// `findname [varlist] [, varlabeltext()]` (Cox) -- sets r(varlist).
nlohmann::ordered_json runFindname(const eval::ParsedCommand& parsed, SessionState& session);
// `xtdescribe [if] [in] [, patterns()]` -- r() are summarize, detail of T_i.
nlohmann::ordered_json runXtdescribe(const eval::ParsedCommand& parsed, SessionState& session);
// tsset's delta phrase ("1 unit", "2 quarters", the %tc breakdown); defined in
// handlers_m3.cpp beside the tsset/xtset arm that prints it too.
std::string tsDeltaText(double delta, const std::string& fmt);
// `cf varlist using file [, verbose]` -- rc 9 on any difference.
nlohmann::ordered_json runCf(const eval::ParsedCommand& parsed, SessionState& session);
// `fvunab lmacname : varlist` -- operator prefixes kept, canonical case.
nlohmann::ordered_json runFvunab(const eval::ParsedCommand& parsed, SessionState& session);
// `numlabel [lblnames], add|remove [mask() force]`.
nlohmann::ordered_json runNumlabel(const eval::ParsedCommand& parsed, SessionState& session);
// `labvars varlist "label" ... [, remove]` (Klein).
nlohmann::ordered_json runLabvars(const eval::ParsedCommand& parsed, SessionState& session);
// `renpfix old [new]` -- renames in order, stopping at the first clash.
nlohmann::ordered_json runRenpfix(const eval::ParsedCommand& parsed, SessionState& session);
//
// handlers_t1_gen.cpp -- verbs that write a column.
// `mvdecode varlist, mv(numlist[=.] [\ numlist[=.]])`.
nlohmann::ordered_json runMvdecode(const eval::ParsedCommand& parsed, SessionState& session);
// `integ yvar xvar [if] [in] [, generate() trapezoid]` -- sets r(integral).
nlohmann::ordered_json runInteg(const eval::ParsedCommand& parsed, SessionState& session);
// `sencode strvar [if] [in], generate()|replace [label() gsort()]` (Newson).
nlohmann::ordered_json runSencode(const eval::ParsedCommand& parsed, SessionState& session);
// `fillmissing varlist [if] [in] [, with()]` (Shah); the by-prefix arrives as by().
nlohmann::ordered_json runFillmissing(const eval::ParsedCommand& parsed, SessionState& session);
// `astile newvar = var [if] [in] [, nq() by()]` (Shah); the by-prefix arrives as by().
nlohmann::ordered_json runAstile(const eval::ParsedCommand& parsed, SessionState& session);
//
// handlers_t1_matrix.cpp -- the matrix store.
// `matsort matname [col] [up|down]` (Millar).
nlohmann::ordered_json runMatsort(const eval::ParsedCommand& parsed, SessionState& session);
// `matselrc m1 m2 [, row() col() names]` (Cox, STB-56 dm79).
nlohmann::ordered_json runMatselrc(const eval::ParsedCommand& parsed, SessionState& session);

}  // namespace piet::handlers

// Canonical Stata-style user-supplied-path resolver (native-paths
// stabilization, item 2). The ONE place every handler that accepts a path a
// USER wrote in a do-file (use/save/append using/merge using/postfile
// using/graph save/graph export/erase/confirm file/mkdir) resolves it:
//   1. dequote (matching handlers::runUse's own historical dequote rule).
//   2. `~` expansion (HOME-relative only — same rule runCd used).
//   3. absolute paths pass through unchanged (POSIX leading '/'; on Windows
//      also a drive letter like "C:\" or "C:/", or a leading backslash).
//   4. otherwise join against session.cwd.
//   5. if session.cwd is unset (a headless driver that never seeds it),
//      fall back to getcwd().
// Deliberately declared in `piet` (not `piet::handlers`) — graph_m4.cpp's
// own resolvePathG/dequoteG duplicate used to live outside the handlers TU
// for the same reason (a second `piet::handlers`-only symbol would still
// need re-declaring there); one shared symbol replaces both forks.
namespace piet {
std::string resolveUserPath(const SessionState& session, const std::string& rawPath);
// The same path as a message names it: step 1 above and nothing else. Stata's
// "file X not found" names the file as typed (`file sub/nofile.txt not found`),
// never the resolved absolute path, which would put this machine's working
// directory into every captured transcript (BUG-410).
std::string typedUserPath(const std::string& rawPath);
}  // namespace piet

#endif  // PIET_NATIVE_HANDLERS_H
