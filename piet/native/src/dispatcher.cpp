// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/dispatcher.h"
#include "piet/translate.h"

#include "piet/bcal.h"  // AER scope-2 B2-3: the calendar search directory follows session.cwd

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <functional>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_set>

#include "piet/bundle_paths.h"
#include "piet/cclass.h"
#include "piet/cobra_runner.h"
#include "piet/counters.h"
#include "piet/display_hardening.h"
#include "piet/doexec.h"
#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"  // BUG-476: kStataMissingSentinel
#include "piet/eval/gettoken.h"
#include "piet/eval/macros.h"
#include "piet/eval/pyvalue.h"
#include "piet/eval/tokenizer.h"
#include "piet/handlers.h"
#include "piet/inrange_filter.h"
#include "piet/loops.h"
#include "piet/mata_subset.h"
#include "piet/program.h"
#include "piet/strmutate.h"
#include "piet/storage_type.h"  // BUG-471 T2: the store-type resolver
#include "piet/strutil.h"
#include "dispatch_profile.h"  // WS-D1: per-zone dispatch attribution (stderr, opt-in)
#include "io/win_compat.h"  // cecon::to_native_path / native_unlink (BUG-050 wide open)
#include "piet/delimited_io.h"  // describe, replace: the description staged as a dataset
#include "piet/mutation_result.h"
#include "data/missing.h"   // BUG-532: cecon::missing_kind, the `.a`-`.z` test on a by-key
#include "core/expr_blanks.h"  // FU-875: the blanks Stata drops inside ( ) and [ ]
#include "data/stata_dates.h"  // the literal pseudo-function scan (preResolveGenExpr)
#include "piet/twoway_parse.h"
#include "piet/scratch_seq.h"
#include "piet/settings.h"  // WS-B: settings::extcompatEnabled() (the extcompat gate below)
#include "piet/preflight.h"  // SPEC II-4: memory pre-flight before data-size-scaling verbs
#include "registry.h"
#include "estimation_m4_internal.h"  // e(sample) T2: runExpandIf and the copy verbs' mask helpers

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
// B5-D-USE-REPLACE (92 corpus uses inside the twelve AER papers, this lane's
// highest-weight item): `replace` joins `clear`. Measured StataBE 19.5
// (tests/eval/golden/aer_b4/b5_io_pins.log:37-38 `B5REC|use|replace|0|` +
// :49-61 the discriminating pair: a variable `gen`erated AFTER the prior
// `use` does NOT survive `use FILE, replace` — `confirm variable junk`
// answers r(111) afterwards, exactly what `, clear` does too) —
// `, replace` DISCARDS unsaved changes, it is not a no-op and not a merge.
// `use` is in kOptMinComplete (generated/opt_minimums_bug264.inc) with NO
// measured minimum row for either spelling, so only the EXACT literal
// "clear"/"replace" resolve here — no abbreviation at all. That costs
// nothing measured: all 92 census occurrences spell the option out in full
// (internal capture lexgaps-12.json, command="use" option="replace",
// every one of the 92 `uses[].text` entries matches `,\s*replace` verbatim).
const piet::eval::OptVocab& kVocabUse = piet::eval::registerOptVocab(
    {"use"}, {{"clear", "replace", "nolabel"},
     {}});
// The period options are xtset.ado's too (xtset hands `panelvar timevar` to
// tsset): `xtset id year, yearly` formats year %ty and posts r(unit) yearly
// (aer_w2 w2b_dataio xtset_yearly / xtset_monthly, w2b_dataio.log:256-301).
// mi, noblank, panelname() and displayindent() are tsset.ado's display options,
// which xtset.ado's PanelSet takes too (takeTsDisplayOptions; BUG-1163 review).
const piet::eval::OptVocab& kVocabXtset = piet::eval::registerOptVocab(
    {"xtset"}, {{"delta", "clear", "yearly", "halfyearly", "quarterly", "monthly", "weekly",
                 "daily", "generic", "clocktime", "format", "mi", "noblank", "panelname",
                 "displayindent"},
     {}});
// B4-W3b: the PERIOD options. Each names a Stata date format and nothing else
// (`tsset t, quarterly` -> r(tsfmt) "%tq" and `format t %tq`), so the table
// below IS the implementation, not a lookup beside one. `generic` is Stata's
// own name for the %tg default; `clock`/`Clock` are the datetime pair.
const std::map<std::string, std::string>& kTssetPeriodFmt() {
  static const std::map<std::string, std::string> m = {
      {"yearly", "%ty"},   {"halfyearly", "%th"}, {"quarterly", "%tq"},
      {"monthly", "%tm"},  {"weekly", "%tw"},     {"daily", "%td"},
      {"generic", "%tg"},  {"clock", "%tc"},      {"clocktime", "%tc"},
      {"Clock", "%tC"}};
  return m;
}
// BUG-896: `clear` and `clocktime` are tsset.ado's own option names (its
// `syntax` line: CLEAR ... Clocktime). BUG-1163: the obsolete `tset` is tset.ado's
// one line `tsset `0'`, so it reads the same options.
const piet::eval::OptVocab& kVocabTsset = piet::eval::registerOptVocab(
    {"tsset", "tset"},
    {{"delta", "yearly", "halfyearly", "quarterly", "monthly", "weekly", "daily", "generic",
      "clock", "clocktime", "format", "clear", "mi", "noblank", "panelname", "displayindent"},
     {}});
// BUG-664: float/dpcomma/percent are destring options documented in [D] destring,
// read by runDestring (DestringOpts). `g` is declared for the same
// reason as tostring's below -- w2b_dataio.log destring_g_force runs
// `destring s, g(sn) force` at rc 0.
const piet::eval::OptVocab& kVocabDestring = piet::eval::registerOptVocab(
    {"destring"}, {{"replace", "gen", "generate",
                                                                        "ignore", "force", "float",
                                                                        "dpcomma", "percent"},
     {},
     {{"g", "generate"}, {"ge", "generate"}}});
// B5-D1 / BUG-665: `force` overrides tostring.ado's reversibility test (a
// value whose string does not read back equal is converted anyway instead of
// the "cannot be converted reversibly" note); without the token a legal script
// died at r(198) (measured B4REC|tostring|replace_force|0| —
// tests/eval/golden/aer_b4/pins_oracle_r.log:29; StataBE renders 42/./.a
// with `force` on a double, internal capture
// tostring_missing_oracle.log:67, rc 0). `format` is the ado's `format(str)`.
// `g` is Stata's abbreviation of `generate`; it cannot resolve via the
// unique-prefix rule because `gen`/`generate` are BOTH already-registered
// literal spellings that `g` prefixes ambiguously, so it is declared instead
// (BUG-430 aliases mechanism, kVocabPredict precedent above). `ge` is the same
// case one letter longer: it prefixes both spellings, and the ado's
// `Generate(string)` accepts it (w2z_tostring tz_ge_abbrev: `tostring vfi,
// ge(vfi_s)` at rc 0, str2; destring documents the same g[enerate]() abbreviation).
const piet::eval::OptVocab& kVocabTostring = piet::eval::registerOptVocab(
    {"tostring"}, {{"replace", "gen", "generate",
                                                                        "usedisplayformat", "force",
                                                                        "format"},
     {},
     {{"g", "generate"}, {"ge", "generate"}}});
// BUG-370. `nq` is Stata's own abbreviation of nquantiles, and the vocab layer
// resolves prefixes, so registering the long spelling accepts both.
// BUG-1096: maxlength() is read by runDecode; its minimum `maxl' is a floor row
// (`max(3)' is "option max() not allowed", StataBE 19.5). BUG-1141: Generate()
// takes `g()', below the prefix floor, so declared.
const piet::eval::OptVocab& kVocabDecode = piet::eval::registerOptVocab(
    {"decode"}, {{"generate", "maxlength"},
     {},
     {{"g", "generate"}}});
// B5-D7: `not` moves to known -- see the `ds` arm below for the complement
// implementation and the measured receipt. has/alpha/varwidth/detail stay
// unimplemented (refused by name, not silently dropped).
const piet::eval::OptVocab& kVocabDs = piet::eval::registerOptVocab(
    {"ds"}, {{"not"},
     {"detail", "varwidth", "alpha", "has"}});
// BUG-1015: `n()` is xtile.ado's own minimum abbreviation of nquantiles()
// (`Nquantiles(string)`), below the 2-character prefix floor, so it is declared
// (w2as_summary_stats_family sx_xtile_n: rc 0 in Stata, r(198) here before).
const piet::eval::OptVocab& kVocabXtile = piet::eval::registerOptVocab(
    {"xtile"}, {{"nquantiles", "cutpoints"},
     {"altdef"},
     {{"n", "nquantiles"}, {"c", "cutpoints"}}});
const piet::eval::OptVocab& kVocabIpolate = piet::eval::registerOptVocab(
    {"ipolate"}, {{"generate", "epolate", "by"},
     {}});
const piet::eval::OptVocab& kVocabTssmooth = piet::eval::registerOptVocab(
    {"tssmooth"}, {{"window"},
     {"replace"}});
// B5-D10: `generate(NAME)` accepted -- see the `append` arm below for the
// src-marker implementation and the measured receipt
// (B4REC|append|generate|0|). `append` is in kOptMinComplete
// (opt_minimums_bug264.inc), so this net runs exactOnly: only the EXACT
// spelling `generate` resolves -- `gen(...)` stays r(198), because no oracle
// cell measures that abbreviation on THIS verb (fabricating acceptance would
// be unmeasured parity). BUG-549: `keep(varlist)` restricts which USING
// columns are taken (master's own columns are padded missing on the using rows):
// runAppend expands it against the using names and cmd_append projects the using
// frame (`--keepusing`) before the concat. Oracle: w2_data_io.log app1-app7,
// app10, app12-app14. (That log's app9 now measures `gen(src)` at rc 0, so the
// r(198) above is stale -- still pinned by selftest.cpp's B5-D10 arm, left for
// its owner.) BUG-550: a string/numeric clash is r(106) without `force` and
// missing on the using rows with it (app15-app17), see the append arm below.
// BUG-551: a .dta using's value-label definitions are copied unless `nolabel`
// (app1-app13), and generate()'s marker carries append.ado's `_append` label.
// `nonotes` (app13: rc 0, same result as keep(id a)) asks that the using file's
// notes not be copied; runAppend copies none, so it is honoured as given.
const piet::eval::OptVocab& kVocabAppend = piet::eval::registerOptVocab(
    {"append"}, {{"force", "generate", "keep", "nolabel", "nonotes"},
     {}});
const piet::eval::OptVocab& kVocabFramePut = piet::eval::registerOptVocab(
    {"frame"}, {{"into"},
     {}});
const piet::eval::OptVocab& kVocabFrlink = piet::eval::registerOptVocab(
    {"frlink"}, {{"frame", "generate", "gen"},
     {}});
const piet::eval::OptVocab& kVocabFrget = piet::eval::registerOptVocab(
    {"frget"}, {{"from"},
     {}});
// B4-W3b: `force` joins the KNOWN set. In Stata it is an ACKNOWLEDGEMENT, not
// a modifier — `duplicates drop VARLIST` requires it precisely because dropping
// on a narrowed key discards rows that differ elsewhere, and Stata makes you
// say so. It changes no value: `duplicates drop foreign, force` on
// `sysuse auto` is rc 0 with _N 74 -> 2 (pins_oracle_g.log:250-256), the
// same rows this build's varlist path already keeps. So it is accepted and
// read as the acknowledgement it is, rather than rejected as unknown.
// `ge` prefixes both registered spellings, so it is declared too: duplicates
// tag's Generate() takes it (aer_w2 w2b_dataio duptag_ge(dup) rc 0).
// BUG-1096: examples and list hand every other option to `list` (duplicates.ado
// `[, *]`), so list's own options are real there ([D] list's syntax table;
// StataBE 19.5 runs `duplicates examples s, sepby(s)`, `ab(4)`, `clean`, `nol`,
// `sep(0)` and `N`, and refuses `bogus` as list does). This build draws list's
// default table only: they are the unimplemented set, refused by name on those
// two subcommands and not allowed on the others. `ab` and `sep` are declared
// because each prefixes two of them (absolute; sepby, sepbyexp).
const piet::eval::OptVocab& kVocabDuplicates = piet::eval::registerOptVocab(
    {"duplicates"}, {{"gen", "generate", "force"},
     {"compress", "nocompress", "fast", "abbreviate", "string", "noobs", "fvall", "table",
      "display", "header", "noheader", "footer", "clean", "divider", "separator", "sepby",
      "sepbyexp", "ds", "nolabel", "mean", "sum", "n", "labvar", "constant", "notrim",
      "absolute", "relative", "nodotz", "subvarname", "linesize"},
     {{"g", "generate"}, {"ge", "generate"}, {"ab", "abbreviate"}, {"sep", "separator"}}});
const piet::eval::OptVocab& kVocabLevelsof = piet::eval::registerOptVocab(
    {"levelsof"}, {{"local", "clean", "missing",
                                                                        "separate"},
     {}});
const piet::eval::OptVocab& kVocabWinsor2 = piet::eval::registerOptVocab(
    {"winsor2"}, {{"cuts", "suffix", "by", "trim", "replace"},
     {}});
// B4-W3b: `generate(stub)` (13 corpus uses) joins the KNOWN set — it now
// creates the level dummies rather than answering r(198).
// B5-D-TAB-DISPLAY: `sort`/`cell`/`col`/`row`/`nofreq` join the KNOWN set too
// (all five rc 0 in StataBE 19.5, tests/eval/golden/aer_b4/
// b5_importcase_pins.log:103-243; oracle receipts: `B5REC|tabulate|sort|0|`
// `B5REC|tabulate|cell|0|` `B5REC|tabulate|nofreq|0|` `B5REC|tabulate|col|0|`
// `B5REC|tabulate|row|0|`). The oracle captures rc ONLY — it does NOT capture
// the rendered table text for any of these five, and PIET's own table
// renderer lives in the frontend (piet/frontend/**, out of scope this lane).
// `sort` is the one with an observable, cheap, IN-SCOPE effect (row order in
// the JSON `table.rows` this file already builds), so it is genuinely
// implemented — see runTabulate/tabulateOne's `sortByFreq` param.
// `cell`/`col`/`row` (percentage lines) and `nofreq` (suppress the frequency
// line) are accepted as VALIDATED NO-OPS, deliberately: this backend never
// invents percentage-line formatting to pin against an oracle cell that does
// not exist, and there is no renderer here (or byte-pin) for this lane to
// wire one into anyway. The rendered output under these four options is
// UNMEASURED and this build does NOT claim Stata parity for that layout —
// only rc and the option vocabulary are.
// W1a-1 (2026-09-14): `nolabel` and `nokey` join the KNOWN set. Both are rc 0
// in StataBE 19.5 at the full spelling (tests/eval/golden/aer_w1/
// w1a_optmin_pins3.log), and their ABSENCE is what BUG-485 was made of: with
// neither in the net, `no` looked like a unique prefix of `nofreq`. `nolabel`
// ([R] tabulate oneway: "display numeric codes rather than value labels",
// minimum `nol` -- measured `n`/`no` r(198), `nol` rc 0) is IMPLEMENTED: the
// arm below skips the BUG-355 value-label substitution. `nokey` (the two-way
// key line printed above cell/col/row percentages) is a validated no-op for
// the same reason cell/col/row are: this build draws no key to suppress.
// BUG-1514: the two-way statistics lrchi2, V (keys arrive lower-cased: `v`), gamma
// and taub, and `all` (= chi2 lrchi2 V gamma taub), are implemented (runTabulate's
// twoWayStats). `column` is the full name of `col` (StataBE 19.5 `tabulate c d,
// column row` rc 0; `co`, `colu`, `colum` rc 0 too), declared as aliases so `co`
// stays a unique prefix of `col`. `wrap` ("do not wrap wide tables") holds as
// written: this table is never wrapped. Fisher's `exact` (and `exact(#)`, whose #
// only sizes FEXACT's workspace) is served on 2 x 2 tables and refused by name on
// larger ones (tabulate.cpp); its minimum is `ex` (StataBE `tabulate c d, ex` rc 0,
// `exp` is expected). `nolog`, the FEXACT enumeration log's switch, is allowed
// only with it (`tabulate c d, chi2 nolog` r(198)); `nol` stays nolabel through
// the measured minimums table. The cell-content options cchi2, clrchi2 and
// expected are real options this build does not compute, refused by name.
const piet::eval::OptVocab& kVocabTabulate = piet::eval::registerOptVocab(
    {"tabulate", "tab", "ta", "tab1", "tab2"},
    {{"chi2", "sum", "summarize", "missing", "generate", "sort", "cell", "col", "row", "nofreq",
      "nolabel", "nokey", "matcell", "matrow", "matcol", "lrchi2", "v", "gamma", "taub", "all",
      "wrap", "exact", "nolog"},
     {"cchi2", "clrchi2", "expected"},
     {{"column", "col"}, {"colu", "col"}, {"colum", "col"}, {"ex", "exact"}}});
const piet::eval::OptVocab& kVocabTable = piet::eval::registerOptVocab(
    {"table"}, {{"statistic", "stat", "contents"},
     {},
     {{"c", "contents"}}});  // BUG-1015: c() is table_16's own spelling of contents()
// B4-W3b: `nototal` and `save` join the KNOWN set — both change which numbers
// appear (`nototal` drops the overall row, `save` publishes r(Stat#)/r(name#)),
// which is exactly why they were rejected before and exactly why they are
// implemented rather than accepted-and-ignored now.
// tabstat: `f` is format's declared minimum
// abbreviation (w2d_uncovered.log tabstat_f_abbrev_077 `f(%9.3g)`, rc 0).
const piet::eval::OptVocab& kVocabTabstat = piet::eval::registerOptVocab(
    {"tabstat"}, {{"by", "stat", "stats",
                   "statistics", "columns", "nototal", "save", "format"},
     {},
     {{"f", "format"}}});
const piet::eval::OptVocab& kVocabCorr = piet::eval::registerOptVocab(
    {"corr", "cor", "pwcorr", "correlate"}, {{"pairwise", "sig", "obs", "covariance"},
     {}});

// ===========================================================================
// AER scope-2 W2 — THE OPTION-VOCABULARY REGISTRATION SWEEP.
//
// WHAT THIS IS FOR, and it is not the same thing BUG-233 was for. BUG-233
// registered the vocabularies of nets that already CALLED
// eval::rejectUnknownOptions, so the dispatcher could canonicalize their keys.
// Everything below is the other population: commands whose option surface the
// registry could not see AT ALL, so `piet_native --dump-optvocab --resolve
// <cmd> <opt>` answered `vocab_registered:false` for every spelling — the
// census's UNMEASURED answer, not a gap and not a clean bill either.
//
// Measured on the 12 AER packages the morning of 2026-09-09, before this block:
//   option_pairs=596 known=153 unknown=74 unimplemented=20 no_vocabulary=349
// `no_vocabulary=349` is 349 (command, option) pairs the papers really use
// where the product could not say whether it knows the option.
//
// THE CLASSIFICATION RULE, which is the whole point and is not negotiable:
//   `known`         = ONLY the option keys the handler ACTUALLY READS. Every
//                     row below was produced by grepping the named handler
//                     body for that key; the file:line of the read is cited.
//   `unimplemented` = a real option of that command which the handler does not
//                     read (an honest refusal, and the bucket W3/W4/W5 empty).
//   neither         = the product does not know the spelling at all; the census
//                     reports it `unknown`, which is a MEASURED answer.
// An option wrongly in `known` is a silent wrong answer — the product claiming
// to honour something it ignores. When in doubt this block refuses.
//
// THE ONE SIDE EFFECT, stated rather than buried. Registering a vocabulary
// arms eval::canonicalizeForCommand for that command, so a >=2-char unique
// prefix of a `known` key now reaches the handler under its canonical
// spelling. For the nets already calling rejectUnknownOptions that is
// bit-neutral (errors.h's one-sidedness argument: the net resolved the same
// prefixes itself). For the HAND-ROLLED nets below (regress, xtreg,
// summarize, save, predict, estimates table) it is not: their loops compare
// raw keys, so `regress y x, vc(robust)` was rejected before and is honoured
// now. That is BUG-231's own repair direction — accepted-then-dropped is the
// defect, not the fix — but it is a real widening and is called out here so
// the next audit does not have to rediscover it.
//
// Commands deliberately NOT registered here, each with its reason:
//   odbc            designed refusal (registry.cpp:1241 `refused:no_odbc_driver`).
//   gtools, dropmiss  not commands in this build at all (absent from
//                   --dump-registry's 299 rows), so they answer refusedCommand;
//                   an option vocabulary for a refused name would be a claim
//                   about a surface that does not exist.
//   the graph family  W4's (see the graph arm's own note).

// --- the BUG-336 batch, promoted from inline literals ----------------------
// These sets are UNCHANGED. Each net below now reads its own registration
// instead of a brace literal, so the vocabulary is written once and the
// dispatcher can see it; the accepted/rejected sets are byte-identical.
const piet::eval::OptVocab& kVocabCodebook = piet::eval::registerOptVocab(
    {"codebook"}, {{"compact"},
     {"mv", "tabulate", "notes", "detail", "all", "header", "dots", "languages", "problems"}});
// B5-D5: `fullnames` moves to known -- a VALIDATED NO-OP, not a semantics
// change. The describe block always carries the FULL, untruncated variable
// name (handlers.cpp's describeEntry: entry["name"] = name;) and the one
// renderer sizes its name column to the longest name present
// (console.js:formatDescribe, nameW = Math.max(13, ...v.name.length)) -- PIET
// never abbreviates, so `fullnames` is satisfied by construction. Measured
// B4REC|describe|fullname|0| (tests/eval/golden/aer_b4/pins_oracle_r.log:
// 130-131) and the in-tree StataBE rendered table at tests/eval/golden/
// f16_tw_transport_oracle.log:43,85 (`describe, fullnames`). short/simple/
// numbers/detail stay unimplemented -- those DO change what prints.
// short/simple/varlist/replace/clear (aer_w2 w2b_dataio log:162-238): short
// prints the header and posts r(N) r(k) r(changed); simple lists the names;
// varlist posts r(varlist) and r(sortlist); replace [clear] makes the
// description the dataset.
const piet::eval::OptVocab& kVocabDescribe = piet::eval::registerOptVocab(
    {"describe"}, {{"fullnames", "short", "simple", "varlist", "replace", "clear"},
     {"numbers", "detail"}});
// rename's 1:1 and wildcard forms read no option at all and Stata's own 1:1
// form has none either (dispatcher.cpp's rename arm). Registering the EMPTY
// vocabulary is not a no-op: it is the difference between "we looked and there
// are none" and "nobody has looked".
// B4-W3b: the GROUP form's case options (29 corpus uses of `rename _all,
// lower` / `rename *, upper`). Measured rc 0 on StataBE 19.5
// (pins_oracle_g.log:101-105), which also shows the note Stata prints when
// nothing moves: "(all newnames==oldnames)".
const piet::eval::OptVocab& kVocabRename = piet::eval::registerOptVocab(
    {"rename"}, {{"lower", "upper", "proper", "renumber", "addnumber", "replace"},
     {}});
const piet::eval::OptVocab& kVocabGenerate = piet::eval::registerOptVocab(
    {"generate"}, {{},
     {"before", "after"}});
// BUG-471 T2: replace's one option (generate.sthlp: `replace ... [, nopromote]`;
// w2e_float_default.log replace_byte_nopromote :827). It used to be dropped on
// the floor with every other option, so `nopromote` changed nothing.
const piet::eval::OptVocab& kVocabReplace = piet::eval::registerOptVocab(
    {"replace"}, {{"nopromote"},
     {}});
const piet::eval::OptVocab& kVocabNlGmm = piet::eval::registerOptVocab(
    {"nl", "gmm"}, {{"instruments", "onestep", "vce", "vcov", "robust", "cluster", "level"},
     {}});
const piet::eval::OptVocab& kVocabMixed = piet::eval::registerOptVocab(
    {"mixed"}, {{"vce", "vcov", "robust", "cluster", "level"},
     {}});
// BUG-1500: `twostep`, ivprobit's Newey two-step estimator (runIvMl); ivtobit's
// two-step form is refused there by name.
const piet::eval::OptVocab& kVocabIvMl = piet::eval::registerOptVocab(
    {"ivprobit", "ivtobit"}, {{"vce", "vcov", "robust", "cluster", "ll", "ul", "level", "twostep"},
     {}});
const piet::eval::OptVocab& kVocabFracreg = piet::eval::registerOptVocab(
    {"fracreg"}, {{"vce", "vcov", "robust", "cluster", "level"},
     {}});
const piet::eval::OptVocab& kVocabCompress = piet::eval::registerOptVocab(
    {"compress"}, {{},
     {"nocoalesce"}});
// `stable` keeps tied rows in their current order; PIET's sort is always
// stable (the engine's radix gsort), so the option is served as written
// (w2_smalls.log:148 sort_stable rc 0).
const piet::eval::OptVocab& kVocabSort = piet::eval::registerOptVocab(
    {"sort"}, {{"stable"},
     {}});
const piet::eval::OptVocab& kVocabGsort = piet::eval::registerOptVocab(
    {"gsort"}, {{},
     {"generate", "mfirst"}});
// data-verbs-effects-rename-recode-label: recode's options per [D] recode's
// syntax diagram are g:enerate(), into(), l:abel(), pre:fix(), copy:rest and
// t:est. `g' (w2ah_data_verbs.log ah_recode_g_lab, rc 0) cannot resolve by
// prefix, so it is declared, as are `l' and `t' (BUG-1141); runRecode reads them
// all, and the prefix/copyrest minimums are floor rows.
const piet::eval::OptVocab& kVocabRecode = piet::eval::registerOptVocab(
    {"recode"}, {{"generate", "into", "label", "prefix", "copyrest", "test"},
     {},
     {{"g", "generate"}, {"l", "label"}, {"t", "test"}}});
// B5-D6: `separator` moves to known -- runList renders a GUI grid (same
// validated-no-op class as noobs/clean just above: the grid draws no
// every-N-rows rule either way, so accepting and discarding the count changes
// nothing observable). Measured B4REC|list|sep(0)|0| (tests/eval/golden/
// aer_b4/pins_oracle_r.log:143-144) and the in-tree StataBE rendered
// table at pins_oracle_h.log:94 / pins_oracle_i.log:126 (`list ...,
// sep(0) noobs`). `sep` is a prefix of BOTH `separator` and `sepby`, so the
// unique-prefix rule alone leaves it ambiguous -> r(198) even after this
// move; the aliases row below is the only mechanism that can carry it
// (BUG-430, kVocabPredict precedent). `sepby` stays unimplemented -- it takes
// a varlist and has no rendering support.
// W1a-1 (2026-09-14): `abbreviate(#)` moves from unimplemented to KNOWN. [D]
// list: "abbreviate variable names to # display columns; default is ab(8)" --
// a width knob for the TEXT header. Measured: `list price in 1/2, ab(25)` rc 0
// and `a` r(198) (tests/eval/golden/aer_w1/w1a_optmin_pins.log), so the
// minimum is 2; `ab` collides with `absolute` only in THIS net, not in Stata,
// which is why the generated minimum carries a re-adjudication (gen_table.py).
// runList renders a grid whose column heads are never abbreviated, so the
// value is validated (strict integer, the separator idiom) and then dropped:
// the corpus spelling is `abbreviate(40)` -- "do not shorten" -- which is what
// the grid already does. A # smaller than a name would shorten Stata's header
// and not ours; that layout is UNMEASURED and this build does not claim it.
const piet::eval::OptVocab& kVocabList = piet::eval::registerOptVocab(
    {"list"}, {{"noobs", "clean", "separator", "abbreviate"},
     {"mean", "sum", "noheader", "nolabel", "sepby", "divider",
      "table", "display", "nocompress", "compress", "fast", "header", "notrim", "nodotz",
      "absolute", "subvarname"},
     {{"sep", "separator"}}});
// B5-D3/D4: BUG-475 -- `label define LB ...` without add/modify/replace on an
// EXISTING label now rejects r(110) ("label already defined", matching
// Stata's own text -- oracle_r.log:58), and `replace` moves from
// unimplemented into known so the wholesale-redefine spelling itself is
// accepted (B4REC|label_define|<redefine_no_replace>|110| and
// B4REC|label_define|replace|0|, tests/eval/golden/aer_b4/
// pins_oracle_r.log:60-66). KNOWN NARROWING: runLabelDefine re-parses
// the RAW option tail by substring match (same as the pre-existing add/modify
// scan), so an ABBREVIATED `repl`/`repla`/... reaches this net (now resolved
// to "replace" by the dispatcher's prefix rule) but is invisible to the raw
// scan and falls through to the r(110) guard -- a pre-existing class of gap
// (the same is true of `mod`/`ad` today), recorded rather than fixed here.
// BUG-524: `label language`'s new/rename/delete join the unimplemented set so
// the refusal names them as valid Stata (rc 198), instead of "not allowed".
// BUG-527: `label language NAME, new` is intercepted BEFORE this net runs
// (oracle rc 0, arm lang_new) -- `new` stays listed here so `rename`/`delete`
// (unmeasured) and `, new` on every OTHER label subcommand still get the
// honest "not yet implemented" wording instead of "not allowed".
const piet::eval::OptVocab& kVocabLabel = piet::eval::registerOptVocab(
    {"label"}, {{"add", "modify", "replace", "nofix"},
     {"new", "rename", "delete"}});
const piet::eval::OptVocab& kVocabLabelbook = piet::eval::registerOptVocab(
    {"labelbook"}, {{},
     {"alpha", "length", "list", "problems", "detail"}});
// import/export: the net is per (command, subcommand), so the four branch sets
// stay the literals the net uses and the REGISTRATION is their union per
// command token. Unioning across subcommands is the registry's documented
// behaviour (errors.h: csdid and its estat sub-net), and the branch net is
// still the authority on what is accepted.
// export delimited's grammar: DELIMiter(string) NOVARnames NOLABel DATAFmt
// Quote REPLACE. novarnames/datafmt/quote are written by runExportDelimited
// (w2ar dx_exd_novarnames log:501, dx_exd_datafmt :629, dx_exd_quote :703).
const std::unordered_set<std::string> kExportDelimKnown = {"delimiter", "delim", "nolabel",
                                                           "replace", "quote", "novarnames",
                                                           "datafmt"};
const std::unordered_set<std::string> kExportDelimUnimpl = {};
// AER scope-2 W3a: `sheet()` and `sheetreplace`/`sheetmodify` land; `cell()`
// (a start cell for the rectangle) and `keepcellfmt` (needs a styles part this
// writer does not author) stay named gaps.
// keepcellfmt is accepted: this writer authors no styles part, so there is no
// cell format to keep, and a sheetreplace keeps every other part of the
// workbook verbatim (w2ar dx_exx_sheetreplace_keep rc 0, log:1207).
const std::unordered_set<std::string> kExportExcelKnown = {"replace", "nolabel", "firstrow",
                                                           "sheet", "sheetreplace", "sheetmodify",
                                                           "keepcellfmt"};
// `datestring()`/`missing()` are deliberately NOT listed: adding `datestring`
// would make `da` an ambiguous prefix against export delimited's `datafmt` in
// the per-command union, trading one honest message for a worse one on a
// spelling that already resolved.
const std::unordered_set<std::string> kExportExcelUnimpl = {"cell", "locale"};
// AER scope-2 W3a: `varnames`/`rowrange`/`colrange`/`numericcols`/`encoding`
// land. BUG-477: `case` for DELIMITED lands too -- it is a DIFFERENT rule
// from excel's (delimited's own default is `lower`, not `preserve` --
// b5_importcase_pins.log) but the same three-value grammar, so it moves to
// KNOWN alongside them. `asdouble`/`bindquote`/`stringcols` stay named gaps.
// 2026-10-21: every option help import delimited documents is read by the
// native reader (delimited_io.cpp): DELIMiters (with `delimiter`/`delim` kept as exact
// spellings), BINDQuotes, STRIPQuotes, MAXQUOTEDRows, STRINGCols, NUMERICCols,
// ASDOUBle, ASFLOat, COLLAPSEDelimiters, EMPTYLines, CHARSET (w2ar
// log:2570-3513). The locale/separator knobs stay named gaps.
const std::unordered_set<std::string> kImportDelimKnown = {
    "delimiter",  "delim",       "delimiters",  "clear",      "varnames",
    "rowrange",   "colrange",    "numericcols", "stringcols", "encoding",
    "charset",    "case",        "bindquotes",  "stripquotes", "maxquotedrows",
    "asdouble",   "asfloat",     "collapsedelimiters", "emptylines"};
const std::unordered_set<std::string> kImportDelimUnimpl = {
    "favorstrfixed", "parselocale", "groupseparator", "decimalseparator"};
// AER scope-2 W3a: this is the highest-frequency option cluster in the whole
// twelve-paper corpus (`import excel` appears 273 times, 243 in paper 015).
// locale(): the reader decodes the workbook's own UTF-8 XML whatever the
// locale names, so it is accepted (w2ar dx_imx_locale `locale("UTF-8")` rc 0,
// log:2055).
const std::unordered_set<std::string> kImportExcelKnown = {
    "firstrow", "clear", "sheet", "cellrange", "allstring", "case", "describe", "locale"};
const std::unordered_set<std::string> kImportExcelUnimpl = {};
std::unordered_set<std::string> unionOf(const std::unordered_set<std::string>& a,
                                        const std::unordered_set<std::string>& b) {
  std::unordered_set<std::string> out = a;
  out.insert(b.begin(), b.end());
  return out;
}
// `delimi`/`delimit`/`delimite` are DELIMiters abbreviations the prefix rule
// finds ambiguous (they also prefix the exact spelling `delimiter`).
const piet::eval::OptVocab& kVocabImport = piet::eval::registerOptVocab(
    {"import"}, {unionOf(kImportDelimKnown, kImportExcelKnown),
     unionOf(kImportDelimUnimpl, kImportExcelUnimpl),
     {{"delimi", "delimiters"}, {"delimit", "delimiters"}, {"delimite", "delimiters"}}});
// Declared spellings the prefix rule cannot reach: export excel's
// `SHeet(string asis)` (`sh` also prefixes sheetreplace/sheetmodify; w2ar
// dx_exx_sheet_sub_replace `sh("2", replace)` rc 0, log:1259) and
// export delimited's `Quote` (one letter, below the 2-char floor).
const piet::eval::OptVocab& kVocabExport = piet::eval::registerOptVocab(
    {"export"}, {unionOf(kExportDelimKnown, kExportExcelKnown),
     unionOf(kExportDelimUnimpl, kExportExcelUnimpl),
     {{"sh", "sheet"}, {"q", "quote"}}});
// Item 5 (User ruling 2026-09-09): `post` moves from `unimplemented` to
// `known` — it now really is implemented (estimation_m4c.cpp's runNlcom).
// `nlcom` is NOT in kOptMinComplete (generated/opt_minimums_bug264.inc), so
// this arms the generic >=2-char unique-prefix rule for `post` (`po`/`pos`
// would start resolving) — an UNMEASURED widening, not captured by any
// oracle cell here; recorded rather than floored (no StataBE measurement of
// `nlcom ..., po` to floor against). `lincom`'s own inline `{"post"}`
// unimplemented list (this file, the lincom arm above) is the same class
// and is DELIBERATELY untouched — out of scope for this item.
const piet::eval::OptVocab& kVocabNlcom = piet::eval::registerOptVocab(
    {"nlcom"}, {{"level", "post"},
     {}});
// B4-W3b: `generate` moves to KNOWN — it now writes the original/copy marker.
const piet::eval::OptVocab& kVocabExpand = piet::eval::registerOptVocab(
    {"expand"}, {{"generate"},
     {}});
// item3 (this lane, BUG-477 brief): `unmatched` moves to KNOWN -- it is a real
// Stata joinby option, and `unmatched(none)` is a legal VALUE this build can
// answer honestly today (see the dispatch site's own comment for why). The
// NAME is accepted here; the VALUE is what the dispatch site still polices --
// `master`/`both` stay named, honest gaps.
const piet::eval::OptVocab& kVocabJoinby = piet::eval::registerOptVocab(
    {"joinby"}, {{"unmatched", "_merge"},
     {"nolabel", "update", "replace"}});
// `g' and `ge' (w2ah_data_verbs.log ah_encode_g: `encode es, g(sector)' at rc
// 0) are below the prefix rule's 2-character floor or ambiguous, so declared.
// BUG-1135: isid.ado's `syntax varlist [, Sort Missok]`; the one-letter
// minimums are below the prefix floor, so declared.
const piet::eval::OptVocab& kVocabIsid = piet::eval::registerOptVocab(
    {"isid"}, {{"sort", "missok"},
     {},
     {{"s", "sort"}, {"m", "missok"}}});
const piet::eval::OptVocab& kVocabEncode = piet::eval::registerOptVocab(
    {"encode"}, {{"generate", "label", "noextend"},
     {},
     {{"g", "generate"}, {"ge", "generate"}}});

// --- the hand-rolled nets: registered, NOT rewired --------------------------
// Each of these rejects unknown options with its own `k == "..."` loop rather
// than through eval::rejectUnknownOptions, so there is no set to bind a
// reference to. The vocabulary is transcribed here from the loop, the read
// site is cited, and testAerW2OptVocabTwoWay() pins the transcription against
// the live dispatcher — a drifted copy fails the selftest, it does not rot.
//
// regress/feols: handlers.cpp:1571-1615 reads vce/vcov/robust/cluster/
// noconstant/nocons/level/absorb; :1645 names `beta` as the real-but-unwired
// one. `keepsingletons` is deliberately in NEITHER set: handlers.cpp:1638
// rejects it outright for regress/feols (real Stata has it on reghdfe only),
// and reghdfe's own vocabulary is kVocabIvRegress (estimation_m4.cpp:141).
// BUG-473: `absorb` stays in this shared set (so `ab()` keeps resolving to
// the full spelling for the reject message) but is gated by the ACTUAL Stata
// word the user typed, same shape as `keepsingletons` — handlers.cpp's own
// reject net answers r(198) "option absorb() not allowed" when stataCmd ==
// "regress", and still builds the FE model for feols/reghdfe/areg.
const piet::eval::OptVocab& kVocabRegress = piet::eval::registerOptVocab(
    {"regress", "feols"}, {{"vce", "vcov", "robust", "cluster", "noconstant", "nocons",
                            "level", "absorb"},
     {"beta"}});
// xtreg: estimation_m4.cpp:1316-1324 reads fe/re; :1386 the RE vcov guard;
// the shared builder at :702-725 reads noconstant/nocons/level and :617
// absorb. :1303-1310 refuses gls/mle/ec1/pa BY NAME as real xtreg
// estimators this build does not have — the definition of unimplemented;
// `be` fits (BUG-1515), its `wls` is refused by name.
const piet::eval::OptVocab& kVocabXtreg = piet::eval::registerOptVocab(
    {"xtreg"}, {{"fe", "re", "be", "vce", "vcov", "robust", "cluster", "noconstant", "nocons",
                 "level", "absorb"},
     {"gls", "mle", "ec1", "pa", "wls"}});
// summarize: the arm reads detail/d (the `detail` bool) and separator (the
// renderer field). `d` is BELOW resolveOptionAbbrev's 2-char floor and so
// unreachable by the prefix rule — a DECLARED alias (the BUG-430 mechanism),
// and the in-tree evidence for the declaration is the arm's own
// `k == "detail" || k == "d"` plus registry.cpp's summarize row, whose
// known_options list is literally {detail, d, meanonly}.
//
// `meanonly` IS read by that arm and is DELIBERATELY NOT REGISTERED. Putting a
// key in `known` arms the generic 2-char unique-prefix rule for it, and
// BUG-264 measured StataBE 19.5 refusing `summarize x, mea` — meanonly's
// minimum is FOUR (generated/opt_minimums_bug264.inc:107). summarize is not in
// kOptMinComplete (detail/separator were never probed), so the narrowing flip
// that would honour the measured minimum is unavailable here, and registering
// meanonly would make PIET accept a spelling Stata rejects. An unmeasured
// widening is fabricated parity, so the census reads `summarize|meanonly` as
// `unknown` — an under-claim, which is the safe direction, and 1 use across the
// 12 papers. The fix is upstream of W2: adjudicate summarize's remaining
// options so it can join kOptMinComplete. testBUG264 pins both sides today.
const piet::eval::OptVocab& kVocabSummarize = piet::eval::registerOptVocab(
    {"summarize"}, {{"detail", "separator"},
     {},
     {{"d", "detail"}}});
// save: dispatcher.cpp's save arm re-parses the RAW tail, so canonicalization
// cannot reach it — registering is pure measurement here, exactly the case the
// W2 brief carves out. `replace`/`emptyok` are the spellings the loop reads;
// the three remaining unimplemented names are the arm's OWN reject message,
// not a guess.
// B5-D9: `emptyok` accepted as a validated no-op. Measured
// B4REC|save|replace_emptyok|0| (tests/eval/golden/aer_b4/pins_oracle_r.log
// :90-93) on `clear` + `set obs 0` + `save F, replace emptyok`. runSetObs's
// zero-data-in-memory branch (handlers_m3.cpp:runSetObs) already loads a real
// {n_rows:0, columns:[]} hbin at that point, so runSaveData's only gate
// (session.hasData()) already passes and the ordinary copy-the-current-hbin
// path already writes a valid empty file — there is no refusal here for
// `emptyok` to relax, unlike saveold's emptyok (handlers_aer_iotext.cpp:
// 163-166), which stays unimplemented because saveold's writer DOES drop
// metadata under nolabel/orphans/emptyok. `save` has no such lossy path.
const piet::eval::OptVocab& kVocabSave = piet::eval::registerOptVocab(
    {"save"}, {{"replace", "emptyok"},
     {"nolabel", "orphans", "version"}});
// predict: a CENSUS row. No rejectUnknownOptions call reads kVocabPredict;
// runPredict (estimation_m4c.cpp) refuses by name itself. The registry row feeds
// canonicalizeForCommand (prefix and alias resolution), the floor table's exact-
// spelling test and `piet_native --dump-optvocab`. FU-19: `known` is the set
// runPredict serves -- xb/resid/residuals/fe (kSupported), pr/n after
// logit/probit/poisson (its glmPredict arm), and predictServesM4's four family
// sets (regress: score stdp stdf stdr rstandard rstudent hat|leverage; areg:
// dresid xbd d; xtreg,fe: xbu u e ue). `unimplemented` keeps what it still
// refuses (cooksd, which selftest.cpp pins, dfbeta, the GLM residual family,
// equation). The abbreviations predictStatisticM4 also reads (rsta rstu lev sc
// dr) are not listed: the prefix rule already resolves each to a known name,
// so an alias would break BUG-430's "only what the prefix rule cannot produce",
// and as names they would join the prefix search -- `le`, Stata's
// {opt l:everage} (regress_postestimation's help), resolved to leverage
// today, would turn ambiguous between `lev` and `leverage`.
// B4 P1: the three DECLARED abbreviations of `residuals`, and why the prefix
// rule cannot produce them. `resid` and `residuals` are both in `known` — an
// INTERNAL synonym pair, one Stata option this arm reads under two spellings —
// so `re`/`res`/`resi` prefix two members and the uniqueness test calls them
// ambiguous. They then fell through to the unimplemented vocabulary, where
// `response` is the unique `re`/`res` match, and PIET refused naming a
// statistic the user never typed.
//
// StataBE resolves all three to `residuals` and runs: oracle_p.log:87-90
// (`res`), :92-95 (`re`), :97-100 (`resi`), rc 0 every time. This is exactly
// what OptVocab::aliases is for (BUG-430) — a spelling the prefix rule cannot
// reach, declared instead of guessed — and each row here is one captured line.
const piet::eval::OptVocab& kVocabPredict = piet::eval::registerOptVocab(
    {"predict"}, {{"xb", "resid", "residuals", "fe", "pr", "n", "score", "stdp", "stdf", "stdr",
                   "rstandard", "rstudent", "leverage", "hat", "dresid", "xbd", "d", "xbu", "u",
                   "e", "ue"},
     {"cooksd", "dfbeta", "anscombe", "deviance", "pearson", "response", "working",
      "equation"},
     {{"re", "residuals"}, {"res", "residuals"}, {"resi", "residuals"}}});
// ivreg2/xtivreg2: handlers_aer_estim.cpp:326-338 consumes r/robust/cl/cluster/
// gmm2s/fe/small and passes everything else to runIvRegress, whose net is
// kVocabIvRegress (estimation_m4.cpp:141) — so the known set is the union of
// the two. The unimplemented names are the ivreg2 options the 12 papers use
// that neither layer reads (sfirst/saverfprefix); each
// is corpus-attested Stata that ran, not a recollection. FU-252: `first` and
// `partial` are read by runIvRegress since w2b-iv-options (estimation_m4b.cpp
// wantFirst/wantPartial, w2b_estim.log ivreg2_partial), so they are known, and so
// are ffirst/savefirst/savefprefix for ivreg2 (wantFfirst/wantSavefirst,
// w2d_uncovered.log ivreg2_ffirst_savefirst_068). A census row like kVocabPredict:
// no rejectUnknownOptions call reads it.
const piet::eval::OptVocab& kVocabIvreg2 = piet::eval::registerOptVocab(
    {"ivreg2", "xtivreg2"}, {{"r", "robust", "cl", "cluster", "gmm2s", "fe", "small",
                              "vce", "vcov", "gmm", "absorb", "noconstant", "nocons", "level",
                              "first", "partial", "ffirst", "savefirst", "savefprefix"},
     {"sfirst", "saverfprefix"}});
// estimates/eststo: `estimates table` is the one sub-form carrying options and
// eststo_esttab.cpp:938-940 reads b/se/stats. B4-W3b moved `title` and
// `replace` from "unimplemented"/"absent" into the KNOWN set, because both are
// now read: `estimates store NAME, title("…")` (73 corpus uses) lands on the
// snapshot and prints in `estimates dir`, and `estimates save F, replace` (86
// uses) is the overwrite guard. The net itself is applied PER SUB-VERB at the
// routing site below — a flat union would let `estimates store m1, replace`
// through, which Stata rejects.
const piet::eval::OptVocab& kVocabEstimates = piet::eval::registerOptVocab(
    {"estimates", "eststo"}, {{"b", "se", "stats", "title", "replace"},
     {}});
// W2 eststoprefix (BUG-520): `eststo`'s OWN option list, which is not
// `estimates store`'s. It is a community command, so the package's spelling is
// the contract -- eststo.ado 1.1.0's syntax line reads
//   [name] [, Title() Prefix(name) Refresh Refresh2() ADDscalars() noEsample
//            noCopy MISsing svy] [: command]
// `prefix()` is READ: w2_eststo_prefix.log A7 (`eststo, prefix(zz): regress y w`
// is rc 0 and answers "(zz9 stored)"). The others are real options this build
// does not implement, so they get the "valid Stata option" wording rather than
// "not allowed". Registered under the `eststo` token ONLY -- `estimates store
// m1, prefix(zz)` stays rejected by its own per-sub-verb net below.
const piet::eval::OptVocab& kVocabEststoStore = piet::eval::registerOptVocab(
    {"eststo"}, {{"title", "prefix"},
     {"refresh", "addscalars", "noesample", "nocopy", "missing", "svy"}});

// --- registered, and no net exists to bind ---------------------------------
// Measured 2026-09-09 by driving `<cmd> ..., zzzfake` through --do: these six
// answer rc 0 and silently drop the token. Registering does not change that
// (the W2 brief forbids adding a net where the handler re-parses raw), it only
// makes the surface visible to the census. The missing net is the finding.
//
// collapse: handlers_data_verbs.cpp:1020 pulls `by(...)` out of the RAW line
// with its own regex; nothing reads `fast`. BUG-477/item2: `fast` moves to
// KNOWN -- it was classified unimplemented even though the handler already
// answers rc 0 for it (this build has no "original data" copy to skip
// restoring, so `fast`'s only real effect in Stata is a no-op acknowledgement
// here too). Measured cell-for-cell against `collapse (mean) price,
// by(foreign) fast`: B5REC|collapse|fast|0| / B5VAL|rows_collapse|2|x|0
// (tests/eval/golden/aer_b4/b5_importcase_pins.log) -- this build already
// answers both.
// collapse's syntax line is BY(varlist) CW FAST CLABEL frame(name):
// runCollapse reads `cw` and `clabel` off the option keys the dispatcher
// resolved (BUG-962, BUG-1100), and frame(name) is parsed and then never read
// by collapse_17 -- StataBE 19.5 collapses in memory and makes no frame -- so
// the collapse arm checks the name and drops it. The arm runs this net: an
// unknown word is r(198) (w2au_collapse_family.log cz_c_err_opt).
const piet::eval::OptVocab& kVocabCollapse = piet::eval::registerOptVocab(
    {"collapse"}, {{"by", "cw", "fast", "clabel", "frame"},
     {}});
// file: file_io.cpp:153-160 greps the raw option tail for read/write/append/
// replace and refuses `binary` by name. `text` is known on the import/`clear`
// precedent (dispatcher.cpp's import net): this build's file I/O is
// unconditionally text mode, so the flag's semantic is satisfied, not dropped.
const piet::eval::OptVocab& kVocabFile = piet::eval::registerOptVocab(
    {"file"}, {{"read", "write", "append", "replace", "text"},
     {"binary"}});
// tsfilter: handlers_aer_estim.cpp:1046-1048 reads trend/smooth.
const piet::eval::OptVocab& kVocabTsfilter = piet::eval::registerOptVocab(
    {"tsfilter"}, {{"trend", "smooth"},
     {}});
// Three env/session verbs whose handlers take no option vector at all
// (runNet/runSsc, and `set`'s note arm). Each unimplemented name is the
// spelling the 12 papers actually use, so none of them is recalled:
//   net install ftools, from("https://…")      ssc install `pkg', replace
//   set more off , perm
const piet::eval::OptVocab& kVocabNet = piet::eval::registerOptVocab(
    {"net"}, {{},
     {"from"}});
const piet::eval::OptVocab& kVocabSsc = piet::eval::registerOptVocab(
    {"ssc"}, {{},
     {"replace"}});
// B5-D14 (BUG-454): `preserve` moves to known -- the `restore` arm below now
// reads it (it used to call NO reject net at all and read no options, so
// `restore, preserve` was rc 0 and the option was silently dropped). See the
// arm and runRestore's `rearm` parameter for the re-arm implementation and
// the measured receipts (B5REC|restore|preserve_nopreserve|622| and
// B5REC|restore|preserve_after_preserve|0|, tests/eval/golden/aer_b4/
// b5_misc_pins.log:90-98).
// BUG-922: `not` (cancel the restore; oracle w2_prog_controlflow pc_pr_not).
const piet::eval::OptVocab& kVocabRestore = piet::eval::registerOptVocab(
    {"restore"}, {{"preserve", "not"},
     {}});
const piet::eval::OptVocab& kVocabSet = piet::eval::registerOptVocab(
    {"set"}, {{},
     {"permanently"}});

// --- the BUG-336 GRAPH arm, promoted -------------------------------------
// The 17th and last of the BUG-336 batch. The four sets are the arm's own
// literals, unchanged; only their storage moved. What is new is that the nine
// command tokens the arm serves now CARRY a vocabulary, so the census can say
// what a graph command knows instead of answering `no_vocabulary` for all of
// it. This is the measurement half only — implementing the graph options the
// papers actually use is W4's, and no graph handler is touched here.
//
// The per-token split follows the arm: it assembles its known set from `gc`,
// the resolved PLOT TYPE. For scatter/line/tsline the token IS the plot type;
// for histogram/kdensity/xtline the token is the type and carries that type's
// extras; for `twoway` and `graph` the type is derived from the varlist
// (`twoway histogram x` -> gc "histogram"), so those two tokens carry the
// UNION — anything less would make the registry claim `twoway histogram x,
// width(2)` is unknown when the arm accepts and honours it.
//
// `legend` sits in `known` although nothing reads it. That is not an oversight
// of this sweep: the arm's own note adjudicates it (p5.do:167 uses it, and a
// graph is a visual surface where an unhonoured hint is not a wrong answer —
// the same call already made for `list, noobs`). Promoting a set verbatim is
// the whole contract here, so the adjudication is carried, not re-opened.
const std::unordered_set<std::string> kGraphBaseKnown = {"title", "xtitle", "ytitle", "note",
                                                          "name", "legend"};
// --- AER scope-2 W4 ------------------------------------------------------
// W2 registered the arm's vocabulary so the census could SEE it; W4 makes the
// arm answer for it. Three new sets, each MEASURED from the code that reads it:
//
//   kGraphTwowayGlobals -- the eight globals `runTwowayLayers` honours that no
//     bare handler reads (graph_m4.cpp:1595-1668: axes.xline/yline, the
//     xscale/yscale `noline` flags, the xlabel/ylabel `none` flags, the legend
//     order/cols/size/off block, and `subtitle`). title/xtitle/ytitle/note are
//     honoured by BOTH surfaces and stay in kGraphBaseKnown. The presence of one
//     of THESE eight is what re-routes a single-arm scatter/line/connected into
//     the layer renderer (the arm below): the machinery already exists and only
//     one surface could reach it, which is a wiring gap, not a missing feature.
//
//   kGraphLayerStyle -- every key `twoway::layerStyle` reads
//     (twoway_opts.cpp:248-284). The short spellings are listed because
//     layerStyle reads them ITSELF (`lw`, `lp`, `mc`, `ms`, `mz`, `msi`), so
//     they are canonical names here, not abbreviations.
//
//   kGraphCosmetic -- options this build ACCEPTS and does not apply. They reach
//     no renderer, so silence would be defect class 1 all over again: the arm
//     appends ONE note per block naming exactly the ones the user typed (see
//     graphAcceptedNotApplied below). An option that would change a NUMBER is
//     never in here -- `histogram, discrete` moves the bin edges and `kdensity,
//     gen()` creates variables, so both stay in the unimplemented half and are
//     refused.
const std::unordered_set<std::string> kGraphTwowayGlobals = {
    "xline", "yline", "xscale", "yscale", "xlabel", "ylabel", "legend", "subtitle"};
const std::unordered_set<std::string> kGraphLayerStyle = {
    "color", "fcolor", "lcolor", "lwidth", "lw", "lpattern", "lp",
    "mcolor", "mc", "msymbol", "ms", "msize", "mz", "msi"};
const std::unordered_set<std::string> kGraphCosmetic = {
    "graphregion", "plotregion", "scheme", "saving", "caption",
    "xsize", "ysize", "fxsize", "fysize", "scale", "bgcolor", "lstyle",
    "text", "ttext", "xtick", "ytick", "xmtick", "ymtick",
    "horizontal", "yaxis", "xaxis", "sort", "nodraw", "barwidth",
    "range", "connect", "cmissing", "jitter", "xcommon", "ycommon",
    // serset-twoway VOC: the twoway plot and axis options the w2al corpus
    // writes and Stata answers at rc 0 (marker labels, marker/line/area/bar
    // colours, the t-axis family, minor labels, aspectratio, recast, pstyle).
    // Drawing choices only: nothing here moves a plotted number.
    "mlabposition", "mlabformat", "mlabcolor", "mlabsize", "mlabvposition", "mlabangle",
    "mlabgap", "mlabstyle", "mlabtextstyle", "mfcolor", "mlcolor", "mlwidth", "mlalign",
    "clcolor", "clpattern", "clwidth", "acolor", "alcolor", "afcolor", "alwidth", "alpattern",
    "bcolor", "bfcolor", "blcolor", "blwidth", "fintensity", "tline", "tlabel", "ttick",
    "tmlabel", "ttitle", "tscale", "xmlabel", "ymlabel", "aspectratio", "recast", "pstyle",
    "dropline", "msangle", "base", "mlabel"};
// What is left after W4: `aspectratio` has no renderer concept at all, and
// `by()` is graph FACETING -- a different set of panels, which is a different
// answer, and which `rejectByFacet` (graph_m4.cpp:93) already refuses by name on
// the twoway path. It sits in the unimplemented half so the bare arm says "valid
// Stata option this build does not implement" instead of "not allowed".
// serset-twoway VOC: aspectratio left this set -- Stata draws with it at rc 0
// (w2al tf_wt_077a/b `aspectratio(1)`, tf_tw_072 `aspect(1)`).
const std::unordered_set<std::string> kGraphBaseUnimpl = {"by"};
// The shared known set every graph token starts from. The per-type extras are
// unioned on top of it, by the arm for its own reject and by the registrations
// below for the census.
const std::unordered_set<std::string> kGraphSharedKnown =
    unionOf(unionOf(kGraphBaseKnown, kGraphTwowayGlobals),
            unionOf(kGraphLayerStyle, kGraphCosmetic));
// gfx-histogram-kdensity-family (oracle w2aq_histogram_family): histogram's
// documented options -- discrete, bins()/width()/start(), the four height types, the
// kdensity/normal overlays and their opts(), addlabels, barwidth, horizontal,
// binrescale -- and by() panels are served by runHistogram (graph_m4.cpp).
// addplot()/addlabopts()/barwidth()/blcolor() are carried, not drawn, and so
// stay out of the applied set (graphAcceptedNotApplied names them).
const std::unordered_set<std::string> kGraphHistExtra = {
    "width", "bin", "freq", "frequency", "fraction", "percent", "density", "normal", "normopts",
    "kdensity", "kdenopts", "discrete", "start", "by", "addplot", "addlabels", "addlabopts",
    "blcolor", "binrescale"};
// Item 6 (User ruling 2026-09-09): `gen`/`generate` moved OUT of
// kGraphKdensUnimpl (below) and INTO here. gfx-histogram-kdensity-family:
// at(), nograph, width() (the old bandwidth spelling), the old kernel names
// and normopts() are kdensity's own options ([R] kdensity) and are served too.
const std::unordered_set<std::string> kGraphKdensExtra = {
    "kernel", "bwidth", "n", "normal", "normopts", "gen", "generate", "at", "nograph", "width",
    "biweight", "cosine", "epanechnikov", "gaussian", "parzen", "rectangle", "triangle", "epan2"};
const std::unordered_set<std::string> kGraphXtlineExtra = {"overlay"};
// serset-twoway FIT: twoway_lfit_parse.class / twoway_lfitci_parse.class's own
// options. n(), range(), level() and stdp/stdf/stdr change the fitted numbers
// and are APPLIED (graph_m4.cpp parseFitOptions); the rest are drawing choices.
const std::unordered_set<std::string> kGraphFitExtra = {
    "level", "stdp", "stdf", "stdr", "nofit", "fitplot", "ciplot", "atobs", "estopts", "predopts"};
const std::unordered_set<std::string> kGraphFitApplied = {"n", "range", "level", "stdp", "stdf", "stdr"};
// Per-type refusal, for the option that would change a NUMBER rather than a
// colour: `histogram, discrete` puts one bin on each distinct value
// (different bin edges, different heights). Accepting it quietly would be a
// wrong answer, so it is refused by name instead of noted.
// gfx-histogram-kdensity-family: discrete, addplot and by() left this set when
// runHistogram served them; at() left the kdensity set the same way.
const std::unordered_set<std::string> kGraphHistUnimpl = {};
const std::unordered_set<std::string> kGraphKdensUnimpl =
    unionOf(kGraphBaseUnimpl, {"addplot", "student", "stopts"});
// The umbrella union: for `twoway` and `graph` the plot TYPE is derived from the
// varlist, not from the dispatched token, so those two carry every type's extras.
// `by` is histogram's own (runHistogram's panels); the layer renderer still
// refuses it (rejectByFacet), so it stays in kGraphBaseUnimpl for these two.
const std::unordered_set<std::string> kGraphAllTypesKnown = [] {
  auto u = unionOf(unionOf(kGraphSharedKnown, kGraphHistExtra),
                   unionOf(kGraphKdensExtra, kGraphXtlineExtra));
  u = unionOf(u, kGraphFitExtra);
  u.erase("by");
  return u;
}();
// TWO DECLARED SPELLINGS (BUG-430's mechanism, errors.h). W4 put `xtick`/`ytick`
// in the vocabulary, and that made `xti` a prefix of BOTH `xtitle` and `xtick`
// -- ambiguous, therefore left alone, therefore refused. `xti("...")` /
// `yti("...")` are 6 corpus statements (scatter, graph, binscatter) and THIS
// BUILD ALREADY RESOLVED THEM TO xtitle/ytitle, measured on main's binary before
// this change (`--dump-optvocab --resolve scatter xti` -> in_known). So the map
// records a resolution that already existed and that W4's own additions would
// otherwise have taken away; it is not a new claim about Stata's abbreviation
// table, which was not consulted (no oracle run this stint).
// serset-twoway VOC: `m()` = msymbol and `c()` = connect are Stata's own
// one-letter spellings, below the prefix rule's two-character floor (w2al
// tf_sc_073m, tf_tw_084); `mlabp` is ambiguous by prefix (mlabposition /
// mlabpos...) and is Stata's documented short form; xlabels/ylabels are
// Stata's synonyms of xlabel/ylabel (tf_ts_064).
const std::unordered_map<std::string, std::string> kGraphAliases = {
    {"xti", "xtitle"},      {"yti", "ytitle"},   {"m", "msymbol"},   {"c", "connect"},
    {"mlabp", "mlabposition"}, {"xlabels", "xlabel"}, {"ylabels", "ylabel"}};
const piet::eval::OptVocab& kVocabGraphPlain = piet::eval::registerOptVocab(
    {"scatter", "line", "tsline"}, {kGraphSharedKnown, kGraphBaseUnimpl, kGraphAliases});
// histogram accepts `w()` for width() and `bins()` for bin(): `w` is below the prefix rule's
// two-character floor and `bins` is no prefix of `bin` (hk_h_005w, hk_h_097bins).
const std::unordered_map<std::string, std::string> kGraphHistAliases = {
    {"xti", "xtitle"}, {"yti", "ytitle"}, {"w", "width"}, {"bins", "bin"},
    {"xlabels", "xlabel"}, {"ylabels", "ylabel"}};
const piet::eval::OptVocab& kVocabGraphHist = piet::eval::registerOptVocab(
    {"histogram", "hist"},
    {unionOf(kGraphSharedKnown, kGraphHistExtra), kGraphHistUnimpl, kGraphHistAliases});
// twoway__histogram_gen's options (help twoway__histogram_gen). `w` and `bins` are the
// declared spellings the prefix rule cannot reach (hk_h_005w_tw, hk_h_097bins_tw);
// frac/freq/disc/gen/ret resolve by prefix.
const piet::eval::OptVocab& kVocabTwowayHistGen = piet::eval::registerOptVocab(
    {"twoway__histogram_gen"},
    {{"discrete", "bin", "width", "start", "density", "fraction", "frequency", "percent", "return",
      "generate", "display"},
     {},
     {{"w", "width"}, {"bins", "bin"}}});
const piet::eval::OptVocab& kVocabGraphKdens = piet::eval::registerOptVocab(
    {"kdensity"},
    {unionOf(kGraphSharedKnown, kGraphKdensExtra), kGraphKdensUnimpl, kGraphAliases});
const piet::eval::OptVocab& kVocabGraphXtline = piet::eval::registerOptVocab(
    {"xtline"},
    {unionOf(kGraphSharedKnown, kGraphXtlineExtra), kGraphBaseUnimpl, kGraphAliases});
const piet::eval::OptVocab& kVocabGraphTwoway = piet::eval::registerOptVocab(
    {"twoway"}, {kGraphAllTypesKnown, kGraphBaseUnimpl, kGraphAliases});
// W4: the PLOT-TYPE heads. These are not dispatcher commands -- `connected`,
// `lfit`, `rarea`, `function` and friends are read by twoway_parse.cpp's
// `parseLayerInterior`, which never consults the command registry. They are
// registered anyway for two reasons, both measured:
//   * `runTwowayLayers` now runs an unknown-option net over each layer's own
//     interior (graph_m4.cpp), and it reads its vocabulary back from HERE under
//     the non-command token `twoway_layer` -- the same "a net owns one copy of
//     its set" contract every other registration follows (precedent:
//     `mediate:tvar`, estimation_m4b.cpp:102).
//   * The AER option census attributes a `||` chain segment's options to that
//     SEGMENT's plot type, which is Stata's grammar, so `connected`/`lfit`/
//     `area`/`rarea`/`rcap`/`function` are commands as far as the census is
//     concerned and answered `no_vocabulary` for all 61 of their option rows.
// The set is the umbrella union, not the style keys alone, because Stata allows
// every twoway_option inside an individual plot's option list as well as in the
// global block -- `(scatter y x, xlabel(1 2))` is legal Stata, and a layer net
// carrying only layerStyle's keys would refuse it.
const piet::eval::OptVocab& kVocabGraphLayer = piet::eval::registerOptVocab(
    {"twoway_layer", "connected", "area", "rarea", "rcap", "rcapsym", "rspike",
     "spike", "dropline", "lfit", "qfit", "lfitci", "qfitci", "function",
     "rline", "pcspike", "pcarrow", "bar", "dot"},
    {kGraphAllTypesKnown, kGraphBaseUnimpl, kGraphAliases});
// `graph` is a MULTIPLEX, and this cost a seeded RED before it was a comment.
// The token routes export/save/combine/drop as well as the plot arm, so a
// registration carrying only the plot vocabulary is not merely incomplete — it
// MIS-RESOLVES: `graph export "f", as(png)` has `as` rewritten to
// `aspectratio` (the plot arm's unimplemented list), and the export handler,
// which reads `as` at graph_m4.cpp:2726, stops seeing its format. Six selftest
// legs (BS-012, BUG-103) named it immediately. So the graph token's `known` is
// the union of the plot arm's and the SUBCOMMAND handlers' own reads:
//   graph export   as / name / replace          (graph_m4.cpp:2674,2726,2743)
//   graph save     replace                      (:2504)
//   graph combine  rows / cols / title / subtitle / note / name / imargin /
//                  graphregion                  (:2606,2607,2626,2627,2628,2634,2635,2642)
// `subtitle` and `graphregion` are in the plot arm's UNIMPLEMENTED list and in
// combine's READ list; the registry unions both and `known` wins at every
// decision point, which is the right answer for the token as a whole.
// W4 adds `scale`/`xcommon`/`ycommon` -- three keys `graph combine` is given in
// the corpus (26 uses across the twelve papers) and does not read
// (runGraphCombine's reads are the eight listed above). They arrive through
// kGraphCosmetic, so the combine arm notes them by name rather than dropping
// them. One ABBREVIATION cost is recorded rather than hidden: `cols` and
// `color` now share the prefix `col`, so a bare `col(2)` is ambiguous and left
// alone (i.e. refused) where it used to resolve to `cols`. The full spelling is
// unaffected and no corpus statement or self-test uses the 3-letter form.
const std::unordered_set<std::string> kGraphSubcmdKnown = {
    "as", "name", "replace", "rows", "cols", "title", "subtitle", "note",
    "imargin", "graphregion"};
const piet::eval::OptVocab& kVocabGraphMultiplex = piet::eval::registerOptVocab(
    {"graph"},
    {unionOf(kGraphAllTypesKnown, kGraphSubcmdKnown), kGraphBaseUnimpl, kGraphAliases});

// W4 -- coefplot / marginsplot / binscatter. All three ARE graph commands and
// all three answered `no_vocabulary` for every option row in the census (47 + 5
// + 12). coefplot and marginsplot had no unknown-option net at all, so a typo
// was accepted and dropped exactly like the multi-layer twoway path's.
//
// `known` here is split the same way as the plot arm's: the keys the handler
// READS, plus the keys it accepts and reports. The reads are measured, not
// guessed:
//   coefplot     level / drop / keep / xline      (eststo_esttab.cpp:666,684,689,698)
//                title / xtitle / ytitle          (:817-819)
//                name                             (registerGraphOutput, this file)
//   marginsplot  title / note                     (graph_m4.cpp:1736,1739) + name
//   binscatter   nquantiles                       (handlers_qe.cpp:970 pushOpt)
// Everything else the twelve papers hand these three is a drawing instruction
// -- `recast(bar)`, `ciopts()`, `vertical`, `coeflabels()`, `groups()` -- and a
// drawing instruction is not a number (the §B4(c) call). Those are accepted and
// named in the block's note, never silently dropped.
//
// TWO exceptions, because they are NOT merely cosmetic and are therefore left in
// the unimplemented half so they say so: coefplot's `levels()` and `ci()` set
// WHICH interval is drawn (this build draws `level()`'s, defaulting to 95), and
// accepting them quietly would draw a 95% band for a statement that asked for
// 90%. binscatter's own `by`/`linetype`/`controls`/`absorb` (handlers_qe.cpp:85)
// keep the refusals they already had -- those change the fitted values.
const std::unordered_set<std::string> kCoefplotSpecific = {
    "at",        "base",     "ciopts",   "citop",       "coeflabels", "eqlabels",
    "format",    "groups",   "headings", "mlabel",      "mlabposition", "mlabsize",
    "noeqlabel", "nokey",    "nolabel",  "offset",      "omitted",    "order",
    "recast",    "relocate", "rename",   "rescale",     "swapnames",  "vertical",
    "byopts",    "keep",     "drop",     "level",       "grid",       "aseq"};
const piet::eval::OptVocab& kVocabCoefplot = piet::eval::registerOptVocab(
    {"coefplot"},
    {unionOf(kGraphSharedKnown, kCoefplotSpecific),
     unionOf(kGraphBaseUnimpl, {"levels", "ci"}), kGraphAliases});
const piet::eval::OptVocab& kVocabMarginsplot = piet::eval::registerOptVocab(
    {"marginsplot"}, {unionOf(kGraphSharedKnown, {"recast", "recastci", "plotopts",
                                                  "ciopts", "plot", "x", "noci"}),
                      kGraphBaseUnimpl, kGraphAliases});
// binscatter already has a net and a vocabulary (handlers_qe.cpp kVocabBinscatter).
// This registration UNIONS the graph drawing keys into the SAME registry slot --
// the documented merge rule (errors.h) -- so `binscatter y x, nq(20)
// xtitle("...") scheme(s1mono)` is accepted, its twoway keys carried into
// e(graphcmd) and noted on the drawn block.
const piet::eval::OptVocab& kVocabBinscatterGraph = piet::eval::registerOptVocab(
    {"binscatter"}, {kGraphSharedKnown, {}, kGraphAliases});
// The keys each of the three actually reads -- the `applied` argument to
// graphAcceptedNotApplied at their three dispatch arms.
const std::unordered_set<std::string> kCoefplotApplied = {
    "level", "drop", "keep", "xline", "title", "xtitle", "ytitle", "name"};
const std::unordered_set<std::string> kMarginsplotApplied = {"title", "note", "name"};
// gfx-binscatter: every option of handlers_qe.cpp's kVocabBinscatter known half
// is read by runBinscatter (bins, residualization, fits, colors and symbols in
// the drawn layers, savedata); the twoway keys stay noted.
const std::unordered_set<std::string> kBinscatterApplied = {
    "by",        "nquantiles", "genxq",   "discrete", "medians", "controls",
    "absorb",    "noaddmean",  "linetype", "reportreg", "colors", "mcolors",
    "lcolors",   "msymbols",   "savedata", "replace"};

// lpoly / lowess (goal gfx-lpoly-lowess). Both commands hand options they do
// not know to twoway only when the graph is BUILT, so a typo is r(198) under
// nodraw and silently ignored under nograph (w2ae lpl_typo_draw /
// lpl_typo_nograph, lpl_lw_typo_draw / lpl_lw_typo_nograph); the handlers run
// the net only on the drawn path. `known` = each command's documented options
// plus the graph vocabulary, plus `ttick` (086 reduced_form_est.do:685, a
// time-axis tick option the shared graph set does not carry). The aliases are
// the documented minimums the >=2-char unique-prefix rule cannot reach:
// GENerate (gen..generat prefix generateci/generatevar as well),
// Kernel, Level, Var, EPanechnikov (`ep`/`epa`/`epan` also prefix epan2), and
// lowess's Adjust / Mean / Sort.
const std::unordered_set<std::string> kLpolyOwn = {
    "ci", "generate", "at", "n", "degree", "bwidth", "pwidth", "var", "se", "level", "kernel",
    "nograph", "noscatter", "biweight", "cosine", "epanechnikov", "gaussian", "parzen",
    "rectangle", "triangle", "epan2", "generateci", "generatevar", "legend", "lineopts",
    "ciopts", "scheme", "ttick"};
const std::unordered_set<std::string> kLowessOwn = {
    "nograph", "generate", "adjust", "logit", "sort", "bwidth", "mean", "noweight",
    "plotopts", "lineopts", "rlopts", "ttick"};
const piet::eval::OptVocab& kVocabLpoly = piet::eval::registerOptVocab(
    {"lpoly"},
    {unionOf(kGraphSharedKnown, kLpolyOwn), unionOf(kGraphBaseUnimpl, {"addplot"}),
     {{"xti", "xtitle"}, {"yti", "ytitle"}, {"gen", "generate"}, {"gene", "generate"},
      {"gener", "generate"}, {"genera", "generate"}, {"generat", "generate"},
      {"k", "kernel"}, {"l", "level"}, {"v", "var"}, {"ep", "epanechnikov"},
      {"epa", "epanechnikov"}, {"epan", "epanechnikov"}}});
const piet::eval::OptVocab& kVocabLowess = piet::eval::registerOptVocab(
    {"lowess"},
    {unionOf(kGraphSharedKnown, kLowessOwn), unionOf(kGraphBaseUnimpl, {"plot", "addplot"}),
     {{"xti", "xtitle"}, {"yti", "ytitle"}, {"a", "adjust"}, {"m", "mean"}, {"s", "sort"}}});
// What the two handlers' graph blocks actually read: the computation options
// (they shape the drawn smooth) plus title/xtitle/ytitle/note. Every other key
// the user typed -- legend, xlabel, graphregion, lwidth, scheme, ... -- is
// named in the block's note by graphAcceptedNotApplied.
const std::unordered_set<std::string> kLpolyApplied = {
    "ci", "generate", "at", "n", "degree", "bwidth", "pwidth", "se", "level", "kernel",
    "nograph", "noscatter", "biweight", "cosine", "epanechnikov", "gaussian", "parzen",
    "rectangle", "triangle", "epan2", "title", "xtitle", "ytitle", "note", "name"};
const std::unordered_set<std::string> kLowessApplied = {
    "nograph", "generate", "adjust", "logit", "bwidth", "mean", "noweight", "sort",
    "title", "xtitle", "ytitle", "note", "name"};

// W4, "honesty over silence". A graph arm pushes exactly ONE block, so a note
// cannot be a sibling display block; it is appended to the block's own `note`
// string, which every graph renderer already draws (graph_m4.cpp:75 gives every
// bare block the key, and handlers_aer_graphx.cpp:294 does the same thing for
// grc1leg/event_plot -- this is that helper, at the dispatcher).
//
// `applied` is the set of keys the path that BUILT this block actually read, so
// the note is a statement about the code that ran, not about a hand-maintained
// list: re-routing a `scatter ... , xline(0)` into the layer renderer moves
// `xline` from the noted set to the applied one automatically, because the two
// call sites pass different `applied` sets. Keys the user did not type never
// appear, and a block with nothing to report is left byte-identical -- which is
// what keeps the freeze fixtures (all of them bare commands, no options) pinned.
void graphAcceptedNotApplied(nlohmann::ordered_json& block, const std::string& cmdName,
                             const std::vector<std::pair<std::string, piet::eval::OptValue>>& opts,
                             const std::unordered_set<std::string>& applied) {
  if (!block.is_object() || block.value("type", std::string()) != "graph") return;
  std::vector<std::string> noted;
  for (const auto& kv : opts) {
    if (applied.count(kv.first) != 0) continue;
    if (std::find(noted.begin(), noted.end(), kv.first) != noted.end()) continue;
    noted.push_back(kv.first);
  }
  if (noted.empty()) return;
  std::string joined;
  for (size_t i = 0; i < noted.size(); ++i) {
    if (i != 0) joined += ", ";
    joined += noted[i];
  }
  const std::string text = "(" + cmdName + ": " + joined + " accepted and not applied — this "
                           "build draws the plot without " +
                           (noted.size() == 1 ? std::string("it") : std::string("them")) + ".)";
  std::string cur;
  if (block.contains("note") && block["note"].is_string()) cur = block["note"].get<std::string>();
  block["note"] = cur.empty() ? text : (cur + "  " + text);
}
}  // namespace

namespace piet {

using ojson = nlohmann::ordered_json;

// P3 weights wave: forward-declared here rather than added to
// piet/handlers.h (out of this batch's edit manifest — owned by a different
// stint). Defined in handlers.cpp within the same piet::handlers namespace
// (external linkage, same TU-independent matching every other
// piet::handlers::runXxx symbol already relies on) — this TU can call them
// without a header change. runSummarizeAweight is the [aw=] sibling of the
// already-header-declared runSummarize (picked between at the summarize
// dispatch arm below, off parsed.weightType); runMean/runProportion back
// the two brand-new registry rows.
namespace handlers {
ojson runSummarizeAweight(const std::optional<std::vector<std::string>>& varlist, bool detail,
                          SessionState& session, const std::optional<std::string>& ifExpr,
                          const std::optional<std::string>& weightVar,
                          const std::optional<std::string>& inRange,
                          const char* weightFlag = "--aweight");
ojson runMean(const eval::ParsedCommand& parsed, SessionState& session);
ojson runProportion(const eval::ParsedCommand& parsed, SessionState& session);
// BUG-1015 (handlers_data_verbs3.cpp): xtile with if/in, weights and an expression.
ojson runXtileStata(const std::string& newvar, const std::string& expText,
                    const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                    const std::optional<std::string>& ifExpr,
                    const std::optional<std::string>& inRange,
                    const std::optional<std::string>& weightVar, SessionState& session,
                    const std::optional<std::string>& weightType);
// BUG-1014 (estimation_m4.cpp): correlate/pwcorr with covariance and weights.
ojson runCorrStata(const std::vector<std::string>& varlistIn, bool pairwise, bool sig, bool covar,
                   const std::string& weightFlag, const std::string& weightVar,
                   SessionState& session, const std::optional<std::string>& ifExpr,
                   const std::optional<std::string>& inRange);
// BUG-1014 (estimation_m4.cpp): tabstat with [aw=]/[fw=].
ojson runTabstatWeighted(const std::vector<std::string>& varlist,
                         const std::optional<std::string>& byVal,
                         const std::optional<std::string>& statsVal, SessionState& session,
                         const std::optional<std::string>& ifExpr, bool wantGroups, bool noTotal,
                         const std::string& weightFlag, const std::string& weightVar);
// BUG-1520: the weighted per-group calls of the by-prefix driver -- handlers.cpp
// (summarize; `weightFlag` --aweight/--fweight/--iweight) and estimation_m4.cpp
// (tabstat on a group slice; --aweight/--fweight).
ojson summarizeCoreStatsWeighted(Datastore& ds,
                                 const std::optional<std::vector<std::string>>& varlist,
                                 bool detail, const std::optional<std::string>& weightVar,
                                 const char* weightFlag);
ojson runTabstatOnDataWeighted(const std::vector<std::string>& varlist,
                               const std::optional<std::string>& statsVal,
                               const std::string& dataPath, const std::string& weightFlag,
                               const std::string& weightVar);
// W2 esttabnames (BUG-552): `estimates dir NAMELIST` -- eststo_esttab.cpp,
// declared here on the same pattern (handlers.h is outside that lane).
ojson runEststoDirNames(const std::vector<std::string>& namelist, SessionState& session);
// e(sample) T3 (eststo_esttab.cpp): `estimates restore NAME` with its mask, and
// `estimates esample[:]`.
ojson runEstimatesRestore(const std::string& name, SessionState& session);
ojson runEstimatesEsample(bool colon, const std::vector<std::string>& varlist,
                          const std::optional<std::string>& ifExpr,
                          const std::optional<std::string>& inRange, bool hasWeight, bool replace,
                          SessionState& session);
// e(sample) T4 (estimation_m4b.cpp): the ML fits' mask install.
bool esampleInstallMlMask(SessionState& session, LastModel& lm, ojson& raw);
// BUG-1251 (estimation_m4b.cpp): runPoisson with ppmlhdfe d's data_mutation hand-off.
ojson runPoisson(const std::string& stataCmd, const eval::ParsedCommand& parsed,
                 SessionState& session, ojson* dMutationOut);
// BUG-1175 (estimation_m4b.cpp): runBoottest with boottest's printed report.
ojson runBoottest(const eval::ParsedCommand& parsed, SessionState& session, ojson* reportOut);
// BUG-1227 (estimation_m4b.cpp): runIvRegress with ivregress's `first` report.
ojson runIvRegress(const std::string& method, const eval::ParsedCommand& parsed, bool allowAbsorb,
                   const std::string& stataCmd, SessionState& session, bool smallDefault,
                   bool forceSmall, ojson* reportOut);
}  // namespace handlers

namespace {

// tabstat's Stats subprogram, shared by the tabstat arm and the by-prefix driver
// (BUG-1520): each word is `syntax [, n MEan sd Variance SUm COunt MIn MAx Range
// SKewness Kurtosis SDMean SEMean p1 ... p99 iqr q MEDian CV]` (capitals = the
// minimum abbreviation), lower-cased; anything else is "unknown statistic",
// r(198) (sx_tabstat_ref_stat) -- that word is returned. The engine gets the
// canonical keys; the row names are the ado's.
std::optional<std::string> parseTabstatStats(const std::optional<std::string>& statisticsVal,
                                             std::vector<std::string>& statKeys,
                                             std::vector<std::string>& statRows) {
  struct TsStat { const char* full; size_t minLen; const char* key; const char* row; };
  static const TsStat kTsStats[] = {
      {"n", 1, "n", "N"},           {"mean", 2, "mean", "Mean"},
      {"sd", 2, "sd", "SD"},        {"variance", 1, "variance", "Variance"},
      {"sum", 2, "sum", "Sum"},     {"count", 2, "n", "N"},
      {"min", 2, "min", "Min"},     {"max", 2, "max", "Max"},
      {"range", 1, "range", "Range"}, {"skewness", 2, "skewness", "Skewness"},
      {"kurtosis", 1, "kurtosis", "Kurtosis"}, {"sdmean", 3, "semean", "se(mean)"},
      {"semean", 3, "semean", "se(mean)"}, {"p1", 2, "p1", "p1"},
      {"p5", 2, "p5", "p5"},        {"p10", 3, "p10", "p10"},
      {"p25", 3, "p25", "p25"},     {"p50", 3, "p50", "p50"},
      {"p75", 3, "p75", "p75"},     {"p90", 3, "p90", "p90"},
      {"p95", 3, "p95", "p95"},     {"p99", 3, "p99", "p99"},
      {"iqr", 3, "iqr", "IQR"},     {"median", 3, "p50", "p50"},
      {"cv", 2, "cv", "CV"}};
  std::string list = statisticsVal.has_value() ? *statisticsVal : std::string("mean");
  std::string flat;
  for (char ch : list) flat += (ch == '"' || ch == ',') ? ' ' : ch;
  std::istringstream iss(flat);
  std::string w;
  while (iss >> w) {
    std::string lw = w;
    for (auto& ch : lw) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (lw == "q") {
      for (const char* p : {"p25", "p50", "p75"}) {
        statKeys.push_back(p);
        statRows.push_back(p);
      }
      continue;
    }
    const TsStat* hit = nullptr;
    for (const auto& t : kTsStats) {
      const std::string full = t.full;
      if (lw.size() >= t.minLen && lw.size() <= full.size() && full.compare(0, lw.size(), lw) == 0) {
        hit = &t;
        break;
      }
    }
    if (hit == nullptr) return w;
    statKeys.push_back(hit->key);
    statRows.push_back(hit->row);
  }
  if (statKeys.empty()) {
    statKeys.push_back("mean");
    statRows.push_back("Mean");
  }
  return std::nullopt;
}

// One of tabstat's saved matrices (r(Stat#), r(StatTotal)): a row per statistic,
// a column per variable, from a block's per-variable stats object.
eval::EvalMatrix tabstatStatMatrix(const ojson& statsObj, const std::vector<std::string>& statKeys,
                                   const std::vector<std::string>& statRows,
                                   const std::vector<std::string>& colVars) {
  eval::EvalMatrix m;
  m.rownames = statRows;
  m.colnames = colVars;
  for (const auto& sk : statKeys) {
    std::vector<double> row;
    for (const auto& vn : colVars) {
      double cell = std::numeric_limits<double>::quiet_NaN();
      if (statsObj.contains(vn) && statsObj[vn].is_object() && statsObj[vn].contains(sk) &&
          statsObj[vn][sk].is_number())
        cell = statsObj[vn][sk].get<double>();
      row.push_back(cell);
    }
    m.data.push_back(std::move(row));
  }
  return m;
}

// ---- tabstat's text table ------------------------------------------------------
// Layout measured on StataBE at linesize 80; widths count display characters.

constexpr size_t kTabstatLineWidth = 80;

std::string tsPadLeft(const std::string& text, size_t width) {
  const size_t have = eval::utf8Len(text);
  return have >= width ? text : std::string(width - have, ' ') + text;
}
std::string tsPadRight(const std::string& text, size_t width) {
  const size_t have = eval::utf8Len(text);
  return have >= width ? text : text + std::string(width - have, ' ');
}
std::string tsClip(const std::string& text, size_t width) {
  return eval::utf8Len(text) <= width ? text : eval::utf8Slice(text, 0, width);
}

// Stata's name abbreviation: a run of digits ending the name survives when room
// is left for at least one leading character.
std::string tsShortName(const std::string& name, size_t room) {
  const size_t length = eval::utf8Len(name);
  if (length <= room) return name;
  size_t digits = 0;
  while (digits < name.size() &&
         std::isdigit(static_cast<unsigned char>(name[name.size() - 1 - digits])))
    ++digits;
  if (digits > 0 && room >= digits + 2)
    return eval::utf8Slice(name, 0, room - 1 - digits) + "~" + name.substr(name.size() - digits);
  return eval::utf8Slice(name, 0, room - 2) + "~" + eval::utf8Slice(name, length - 1, length);
}

bool tsIsLeftJustified(const std::string& fmt) { return fmt.size() > 1 && fmt[1] == '-'; }

bool tsIsTimeFormat(const std::string& fmt) {
  const size_t at = tsIsLeftJustified(fmt) ? 2 : 1;
  return at < fmt.size() && (fmt[at] == 't' || fmt[at] == 'd');
}

// The width a format asks for; the %t family states none, so its printed text
// decides.
size_t tsFormatWidth(const std::string& fmt) {
  size_t at = 1;
  while (at < fmt.size() && (fmt[at] == '-' || fmt[at] == '0' || fmt[at] == '+')) ++at;
  size_t width = 0, used = 0;
  for (; at < fmt.size() && std::isdigit(static_cast<unsigned char>(fmt[at])); ++at, ++used)
    width = width * 10 + static_cast<size_t>(fmt[at] - '0');
  return used > 0 ? width : eval::utf8Len(eval::applyStataFmt(0.0, fmt));
}

// A number under `fmt`, keeping the format's own padding.
std::string tsFormatted(double value, const std::string& fmt, size_t width) {
  const std::string text = eval::applyStataFmt(value, fmt);
  return tsIsLeftJustified(fmt) ? tsPadRight(text, width) : tsPadLeft(text, width);
}

std::optional<VarInfo> tsVarCard(SessionState& session, const std::string& name) {
  if (const std::vector<VarInfo>* card = session.residentReadCard())
    for (const VarInfo& info : *card)
      if (info.name == name) return info;
  Datastore* frame = session.datastore();
  if (frame == nullptr || !frame->has(name)) return std::nullopt;
  return frame->varinfo(name);
}

std::string tsDeclaredFormat(SessionState& session, const std::string& name) {
  const std::optional<VarInfo> info = tsVarCard(session, name);
  return info.has_value() ? session.displayFormat(*info) : std::string("%9.0g");
}

// How a column's cells are printed, and the width they ask of the column.
struct TabstatCellStyle {
  std::string fmt;
  size_t width = 9;
};

// The display options the table reads, already validated.
struct TabstatLookOptions {
  bool ownFormats = false;
  std::string commonFormat;
  bool longStubAsked = false;
  bool varWidthGiven = false;
  bool separators = true;
  size_t labelWidth = 16;
  size_t varWidth = 12;
  std::string notes;
};

size_t tsClampedWidth(const eval::OptValue& given, const char* option, size_t fallback,
                      std::string& notes) {
  char* stop = nullptr;
  const std::string text = strutil::trim(given.strValue);
  const double asked = std::strtod(text.c_str(), &stop);
  if (text.empty() || *stop != '\0') return fallback;
  if (asked >= 8.0 && asked <= 32.0) return static_cast<size_t>(asked);
  const size_t bound = asked < 8.0 ? 8 : 32;
  notes += std::string("(option ") + option + "() outside valid range 8..32; " +
           std::to_string(bound) + " assumed)\n";
  return bound;
}

// False when `refusal` was set.
bool tsReadLookOptions(const eval::ParsedCommand& parsed, TabstatLookOptions& look,
                       std::optional<ojson>& refusal) {
  const auto refuseWith = [&refusal](const std::string& message, const char* rc) {
    refusal = ojson(eval::errorBlock("syntax_error", message, std::nullopt, std::nullopt,
                                     std::nullopt, std::string(rc)));
    return false;
  };
  const auto isOn = [](const eval::OptValue& v) { return !v.isBool || v.boolValue; };
  std::optional<std::string> typedFormat;
  for (const auto& [key, value] : parsed.opts) {
    if (key == "format") {
      if (value.isBool) {
        look.ownFormats = look.ownFormats || value.boolValue;
      } else if (const std::string typed = strutil::trim(value.strValue); !typed.empty()) {
        typedFormat = typed;
      }
    } else if (key == "longstub") {
      look.longStubAsked = isOn(value);
    } else if (key == "noseparator") {
      look.separators = !isOn(value);
    } else if (key == "labelwidth" && !value.isBool) {
      look.labelWidth = tsClampedWidth(value, "labelwidth", look.labelWidth, look.notes);
    } else if (key == "varwidth" && !value.isBool) {
      look.varWidth = tsClampedWidth(value, "varwidth", look.varWidth, look.notes);
      look.varWidthGiven = true;
    }
  }
  if (look.ownFormats && typedFormat.has_value())
    return refuseWith("options format and format() may not be specified together", "r(198)");
  if (typedFormat.has_value()) {
    if (!handlers::isValidDisplayFormat(*typedFormat))
      return refuseWith("invalid %fmt in option format(): " + *typedFormat, "r(120)");
    look.commonFormat = *typedFormat;
  }
  return true;
}

TabstatCellStyle tsCellStyleFor(SessionState& session, const TabstatLookOptions& look,
                                const std::string& var) {
  TabstatCellStyle style;
  if (!look.commonFormat.empty()) {
    style.width = tsFormatWidth(look.commonFormat);
    // A string format still sets the width, but a number prints in the variable's
    // own format.
    style.fmt = handlers::isStringDisplayFormat(look.commonFormat) ? tsDeclaredFormat(session, var)
                                                                   : look.commonFormat;
    return style;
  }
  style.fmt = look.ownFormats ? tsDeclaredFormat(session, var) : std::string("%9.0g");
  style.width = tsFormatWidth(style.fmt);
  return style;
}

// The by() column: its width, its header word, and each group's text.
struct TabstatByColumn {
  size_t width = 0;
  std::string headerName;
  bool clipGroups = true;
  std::function<std::string(const ojson&)> groupText;
};

// Group texts come from the value label when one applies, then from a 18-digit
// general rendering; a date-like by() variable prints through its own format and
// is allowed to overflow the column.
TabstatByColumn tsLayOutByColumn(SessionState& session, const std::string& byName,
                                 const ojson& groups, size_t labelWidth, bool totalRow) {
  TabstatByColumn column;
  column.headerName = byName;
  const size_t nameWidth = eval::utf8Len(byName);
  const std::optional<VarInfo> card = tsVarCard(session, byName);
  const bool isString = card.has_value() && card->dtype == "factor";
  const auto plainNumber = [](double v) {
    return strutil::trim(eval::applyStataFmt(v, "%18.0g"));
  };
  const auto withTotalFloor = [totalRow](size_t width) {
    return totalRow ? std::max<size_t>(width, 6) : width;
  };

  if (isString) {
    size_t declared = 0;
    const auto meta = session.varMeta.find(byName);
    const std::string storage =
        meta != session.varMeta.end() ? meta->second.stataType.value_or("") : std::string();
    if (storage == "strL" || storage == "strl") {
      declared = 33;
    } else if (storage.rfind("str", 0) == 0 && storage.size() > 3) {
      declared = static_cast<size_t>(std::strtoul(storage.c_str() + 3, nullptr, 10));
    } else if (groups.is_array()) {
      for (const auto& g : groups)
        if (g.contains("value") && g["value"].is_string())
          declared = std::max(declared, eval::utf8Len(g["value"].get<std::string>()));
    }
    column.width = withTotalFloor(std::max(nameWidth, std::min(labelWidth, declared)));
    column.groupText = [](const ojson& v) { return v.is_string() ? v.get<std::string>() : std::string(); };
    return column;
  }

  const auto labelSet = session.valueLabels.find(byName);
  if (labelSet != session.valueLabels.end() && !labelSet->second.empty()) {
    size_t longest = 0;
    for (const auto& [code, text] : labelSet->second) longest = std::max(longest, eval::utf8Len(text));
    column.width = withTotalFloor(std::max(nameWidth, std::min(labelWidth, longest)));
    const std::map<int, std::string>* labels = &labelSet->second;
    column.groupText = [labels, plainNumber](const ojson& v) -> std::string {
      if (v.is_string()) return v.get<std::string>();
      const double d = v.is_number() ? v.get<double>() : std::numeric_limits<double>::quiet_NaN();
      if (std::isfinite(d) && d == std::floor(d) && std::fabs(d) < 2147483647.0) {
        const auto hit = labels->find(static_cast<int>(d));
        if (hit != labels->end()) return hit->second;
      }
      return plainNumber(d);
    };
    return column;
  }

  const std::string ownFormat = tsDeclaredFormat(session, byName);
  if (tsIsTimeFormat(ownFormat)) {
    column.width = 9;
    column.headerName = tsShortName(byName, 9);
    column.clipGroups = false;
    // %tm and %tw print at width 7 whatever the value's own length (` 2020m2`).
    const size_t fullWidth = ownFormat == "%tm" || ownFormat == "%tw" ? 7 : 0;
    column.groupText = [ownFormat, fullWidth](const ojson& v) {
      const double cell = v.is_number() ? v.get<double>() : std::nan("");
      return tsPadLeft(eval::applyStataFmt(cell, ownFormat), fullWidth);
    };
    return column;
  }
  column.width = std::max<size_t>(8, nameWidth);
  column.groupText = [plainNumber](const ojson& v) -> std::string {
    if (v.is_string()) return v.get<std::string>();
    return plainNumber(v.is_number() ? v.get<double>() : std::nan(""));
  };
  return column;
}

// tabstat's text table, as a `log using` transcript records it in place of the
// structured block (SessionState::pendingTabstatLogText). Returns the table text;
// a refused display option sets `refusal` to the error block and returns "".
std::string renderTabstatTable(SessionState& session, const eval::ParsedCommand& parsed,
                               const ojson& tsBlock, const std::optional<std::string>& byVal,
                               const std::optional<std::string>& columnsVal, bool noTotal,
                               const std::vector<std::string>& statKeys,
                               const std::vector<std::string>& statRows,
                               std::optional<ojson>& refusal) {
  TabstatLookOptions look;
  if (!tsReadLookOptions(parsed, look, refusal)) return std::string();

  std::vector<std::string> vars;
  if (tsBlock.contains("vars") && tsBlock["vars"].is_array())
    for (const auto& name : tsBlock["vars"])
      if (name.is_string()) vars.push_back(name.get<std::string>());

  const bool byMode = byVal.has_value() && !byVal->empty();
  if (!byMode && noTotal) return look.notes + "nothing to display\n";

  // Which way the table reads: statistics across, or variables across.
  bool statsAcross = vars.size() < 2;
  if (columnsVal.has_value() && !columnsVal->empty())
    statsAcross = !(std::string("variables").compare(0, columnsVal->size(), *columnsVal) == 0);

  std::vector<TabstatCellStyle> styles;
  size_t cellWidth = 9;
  for (const std::string& var : vars) {
    styles.push_back(tsCellStyleFor(session, look, var));
    cellWidth = std::max(cellWidth, styles.back().width);
  }
  cellWidth += 1;

  const ojson emptyList = ojson::array();
  const ojson& groups =
      tsBlock.contains("groups") && tsBlock["groups"].is_array() ? tsBlock["groups"] : emptyList;
  const bool longStub = byMode && (look.longStubAsked || look.varWidthGiven);
  TabstatByColumn byColumn;
  if (byMode) byColumn = tsLayOutByColumn(session, *byVal, groups, look.labelWidth, !noTotal);

  const std::string rowHeading = statsAcross ? "Variable" : "Stats";
  const size_t rowNameWidth = statsAcross ? look.varWidth : 8;
  const auto rowNameAt = [&](size_t row) {
    return statsAcross ? tsShortName(vars[row], look.varWidth) : statRows[row];
  };
  const size_t rowCount = statsAcross ? vars.size() : statRows.size();
  const size_t columnCount = statsAcross ? statRows.size() : vars.size();

  // Left part of a line, for the heading or a body row. `groupLabel` is empty on a
  // group's later rows; `overflowOk` lets a date-like group text run past the column.
  const auto bodyStub = [&](const std::string& groupLabel, size_t row, bool overflowOk) {
    if (!byMode) return tsPadLeft(rowNameAt(row), rowNameWidth);
    const std::string shown =
        overflowOk ? groupLabel : tsClip(groupLabel, byColumn.width);
    if (!longStub) return tsPadLeft(shown, byColumn.width);
    return tsPadRight(shown, byColumn.width) + " " + tsPadLeft(rowNameAt(row), rowNameWidth);
  };
  std::string headStub;
  if (!byMode)
    headStub = tsPadLeft(rowHeading, rowNameWidth);
  else if (!longStub)
    headStub = tsPadLeft(byColumn.headerName, byColumn.width);
  else
    headStub = tsPadRight(byColumn.headerName, byColumn.width) + " " + tsPadLeft(rowHeading, rowNameWidth);
  const size_t stubWidth = eval::utf8Len(headStub);

  const auto statAt = [&](const ojson& statsObj, size_t row, size_t col) {
    const std::string& var = statsAcross ? vars[row] : vars[col];
    const std::string& key = statsAcross ? statKeys[col] : statKeys[row];
    if (statsObj.is_object() && statsObj.contains(var) && statsObj[var].is_object() &&
        statsObj[var].contains(key) && statsObj[var][key].is_number())
      return statsObj[var][key].get<double>();
    return std::numeric_limits<double>::quiet_NaN();
  };
  const auto cellText = [&](const ojson& statsObj, size_t row, size_t col) {
    const double value = statAt(statsObj, row, col);
    if (std::isnan(value)) return tsPadLeft(".", cellWidth);
    const TabstatCellStyle& style = styles[statsAcross ? row : col];
    return tsPadLeft(tsFormatted(value, style.fmt, style.width), cellWidth);
  };

  const ojson* totalStats =
      !noTotal && tsBlock.contains("total") && tsBlock["total"].is_object() ? &tsBlock["total"] : nullptr;

  std::string text = look.notes;
  text += "\n";
  if (byMode && !longStub) {
    const auto joined = [](const std::vector<std::string>& words, const char* between) {
      std::string line;
      for (size_t i = 0; i < words.size(); ++i) line += (i == 0 ? "" : between) + words[i];
      return line;
    };
    text += statsAcross ? "Summary for variables: " + joined(vars, " ")
                        : "Summary statistics: " + joined(statRows, ", ");
    text += "\nGroup variable: " + *byVal + " ";
    const auto meta = session.varMeta.find(*byVal);
    if (meta != session.varMeta.end() && meta->second.label.has_value() && !meta->second.label->empty())
      text += "(" + *meta->second.label + ")";
    text += "\n\n";
  }

  const size_t usable = kTabstatLineWidth - std::min(kTabstatLineWidth, stubWidth + 2);
  const size_t perBlock = std::max<size_t>(1, usable / cellWidth);
  for (size_t from = 0; from < columnCount || from == 0; from += perBlock) {
    if (from > 0) text += "\n";
    const size_t to = std::min(columnCount, from + perBlock);
    const size_t bandWidth = cellWidth * (to - from);
    const std::string rule = std::string(stubWidth + 1, '-') + "+" + std::string(bandWidth, '-') + "\n";

    text += headStub + " |";
    for (size_t col = from; col < to; ++col)
      text += tsPadLeft(statsAcross ? statRows[col] : tsShortName(vars[col], 8), cellWidth);
    text += "\n" + rule;

    const auto emitRows = [&](const std::string& label, const ojson& statsObj, bool overflowOk) {
      for (size_t row = 0; row < rowCount; ++row) {
        text += bodyStub(row == 0 ? label : std::string(), row, overflowOk) + " |";
        for (size_t col = from; col < to; ++col) text += cellText(statsObj, row, col);
        text += "\n";
      }
    };
    if (!byMode) {
      if (totalStats != nullptr) emitRows(std::string(), *totalStats, false);
    } else {
      const bool ruleBetween = rowCount > 1 && look.separators;
      for (size_t g = 0; g < groups.size(); ++g) {
        if (g > 0 && ruleBetween) text += rule;
        const ojson& group = groups[g];
        const std::string label =
            group.contains("value") ? byColumn.groupText(group["value"]) : std::string();
        emitRows(label, group.contains("stats") ? group["stats"] : emptyList, !byColumn.clipGroups);
      }
      if (totalStats != nullptr) {
        text += rule;
        emitRows("Total", *totalStats, false);
      }
    }
    text += std::string(stubWidth + 2 + bandWidth, '-') + "\n";
    if (columnCount == 0) break;
  }
  return text;
}

// e(sample) T4 (w2_esample.log:1692 H1 `0|60|60|`, :1725 H2 `0|47|47|`): an ML
// fit installed its model (estimation_m4.cpp cacheLastModelM4) with no mask;
// esampleInstallMlMask writes it on the head and only the e(sample) fields of
// the live model move (setLastModelEsample) -- a second setLastModel would drop
// the e() keys the handler posted after its own (e(chi2type), e(rules)). A mask
// it cannot vouch for stays the named r(198) of kUnavailable.
// BUG-1213: a store that wrote `_est_NAME` (estimates.ado's esample variable)
// changed the variable list, so the data_mutation block carries the new list to
// the variables panel. Stata prints nothing about the variable; neither does it.
void pushEstStoreMutation(const ojson& stored, ojson& blocks, SessionState& session) {
  if (!stored.is_object() || stored.value("type", std::string()) == "error") return;
  const std::string& name = session.activeEstimatesName;
  const auto it = session.estimatesModels.find(name);
  if (name.empty() || it == session.estimatesModels.end() || !it->second ||
      !it->second->esampleVar)
    return;
  blocks.push_back(handlers::mutationResult(session, "estimates_store", {"_est_" + name}));
}

void installMlEsampleAfterFit(const ojson& block, SessionState& session) {
  if (block.value("type", "") == "estimation" && session.lastModel().has_value() &&
      session.lastModel()->esample == LastModel::EsampleState::kUnavailable &&
      session.lastModel()->result.has_value()) {
    LastModel lm = *session.lastModel();
    ojson raw = *lm.result;
    if (handlers::esampleInstallMlMask(session, lm, raw)) {
      session.setLastModelEsample(lm.esample, lm.esampleSerial, lm.esampleInstallPath,
                                  lm.dataPath);
    }
  }
}

ojson echoBlock(const std::string& cmd) {
  dprof::noteJsonNode();
  ojson e;
  e["type"] = "echo";
  // A folded `mata:` block (mata::captureMataBlocks) is one line joined by a
  // control byte; show it the way Stata's log does, one `: stmt` per line. The
  // byte occurs in no other line, so every other echo is unchanged.
  e["text"] = ".  " + (cmd.find(mata::kMataStmtSep) == std::string::npos &&
                               cmd.find(mata::kMataUnterminated) == std::string::npos
                           ? cmd
                           : mata::renderFoldedMataForEcho(cmd));
  return e;
}

// A5-04: forwards to the shared strutil copy (piet/strutil.h) — kept as a
// thin wrapper (not removed) so this file's many `lower(...)` call sites
// didn't need touching. Pure dedup, no behavior change.
std::string lower(std::string s) { return strutil::lower(std::move(s)); }

std::string rstripColon(std::string s) {
  while (!s.empty() && s.back() == ':') s.pop_back();
  return s;
}

// BUG-105 (WS-K) — the escape hatch for handlers that re-parse the RAW line
// with their OWN command-word pattern instead of reading the routed token.
//
// The 2026-07-31 design assumed normalising the routed token at the single `cmd`
// formation site subsumed every second copy of a spelling set. Measured: it does
// for the if-chain arms, kNoDataCommands and kFixedArityCommands, but NOT for
// two handlers that hold their own copy —
//   · handlers_m3.cpp's runMatrixDef (five `^\s*mat(?:rix)?\s+…` regexes)
//   · handlers_m3.cpp's runLabelDefine (`^label\s+define\s+`)
// — because they receive `parsed.raw`, never `cmd`. Under the widened sets
// `matr list X` and `labe define L 1 "a"` would have fallen through to those
// handlers' generic "unsupported form" rejects: a WRONG ANSWER wearing a
// plausible message, which is worse than the r(199) they used to get.
//
// Rewriting only the LEADING token keeps the spelling knowledge in the registry
// and leaves those handlers' patterns untouched. Case-insensitive on the token
// (the tokenizer lowercases the routed name; the raw line keeps the user's
// case), and a no-op whenever the typed spelling is already canonical — so
// every non-abbreviated line is byte-for-byte unchanged.
std::string rawWithCanonicalCmd(const std::string& raw, const std::string& typed,
                                const std::string& canonical) {
  if (typed == canonical || typed.empty()) return raw;
  size_t b = raw.find_first_not_of(" \t");
  if (b == std::string::npos) return raw;
  if (raw.size() - b < typed.size()) return raw;
  for (size_t i = 0; i < typed.size(); ++i) {
    if (static_cast<char>(std::tolower(static_cast<unsigned char>(raw[b + i]))) != typed[i]) {
      return raw;
    }
  }
  size_t e = b + typed.size();
  // Must be a whole token, not a prefix of a longer word (`sav` inside `saved`).
  if (e < raw.size() && cecon::ident::isIdentByte(raw[e])) {  // `diقيمة` is one word too
    return raw;
  }
  return raw.substr(0, b) + canonical + raw.substr(e);
}

// BUG-251 (WS-Q) — the argument tail of a command line, derived WITHOUT any
// knowledge of the command's spelling.
//
// BUG-232 fixed four sites that located the command word in `parsed.raw` with a
// LITERAL (`find("save")`, `find("label")`), by switching them to the typed
// spelling `cmdTyped`. That closes the reachable crashes, but it leaves the
// mechanism intact: every one of those sites still asks "where in this line is
// the command word?" and still indexes off the answer with no npos guard. The
// answer is only ever "at the front", because eval::tokenize() sets
// `pc.raw = strip(line)` — the raw it hands the arms is the very line the
// command word was tokenized out of, leading whitespace already gone.
//
// So the honest formulation is not "find the word" but "skip the leading
// token", which is what every one of these call sites actually wanted. It
// carries ZERO spelling knowledge, so it is not a second copy of any spelling
// set and cannot drift from the registry; and it cannot throw, whatever the
// line looks like.
//
// The token ends at the first character that is not [A-Za-z0-9_], NOT at the
// first space — deliberately, to stay byte-compatible with the `find(cmd) +
// cmd.size()` arithmetic it replaces: `dis(2+2)` must still yield `(2+2)` and
// `cd/tmp` must still yield `/tmp`, which a whitespace-delimited split would
// swallow whole.
//
// WHY THE OLD SHAPE WAS A CRASH AND NOT MERELY WRONG: `find()` returns npos ==
// SIZE_MAX, and the sites add a small constant to it. That addition WRAPS —
// `npos + 3` is 2, not a huge number — so `substr(npos + 3)` silently reads
// from offset 2 for any line at least 2 long (a wrong answer, quietly), and
// throws std::out_of_range only for a line SHORTER than that. Measured: with
// `use` given its Stata abbreviations, the one-character line `u` threw
// `cpp_exception: basic_string` and killed the whole --do run, while `us` and
// `u , clear` returned plausible output from the wrong offset. A bare
// `raw.substr(labelPos)` with labelPos == npos throws unconditionally.
std::string rawArgsAfterCmd(const std::string& raw) {
  size_t b = raw.find_first_not_of(" \t");
  if (b == std::string::npos) return std::string();
  size_t e = b;
  while (e < raw.size() && cecon::ident::isIdentByte(raw[e])) ++e;  // core/ident.h
  if (e == b) return std::string();  // no leading identifier token at all
  size_t a = raw.find_first_not_of(" \t", e);
  // strutil::trim, not trimWs() — the latter is this file's thin wrapper over
  // it and is not declared until further down.
  return (a == std::string::npos) ? std::string() : strutil::trim(raw.substr(a));
}

// mutate.py's storage-type prefix strip (`gen byte edu_hi = ...`) — item 3.
// Also detects string storage types (str#, strL) so the caller can route to
// unsupported_command instead of silently mis-generating a numeric column.
struct StorageTypeInfo {
  std::string expr;     // with the leading storage-type token removed
  bool stringTyped = false;
  // BUG-250: the DECLARED token itself (lowercased: "double", "byte", "str8",
  // ...; "" = no declaration). The strip used to be where the user's storage
  // subtype DIED — parsed, used only for the string-typed routing decision,
  // and discarded — so `describe` called every engine-created column "float"
  // and `: type` had nothing to answer. The creation arms below now record it
  // into VarMeta::stataType (the field the .dta import path already fills and
  // describe/`: type`/the save sidecar already read). It records the CLAIM,
  // not the storage: every numeric column is f64 in the datastore and stays
  // f64 — Stata's storage type is a declaration carried in metadata, and
  // whether storage should ever follow the claim is a different question,
  // named in BUG-250's ledger close.
  std::string typeToken;
};

StorageTypeInfo stripStorageType(const std::string& raw) {
  StorageTypeInfo info;
  info.expr = raw;
  std::string beforeEq = raw.substr(0, raw.find('='));
  size_t start = beforeEq.find_first_not_of(" \t");
  if (start == std::string::npos) return info;
  std::string trimmed = beforeEq.substr(start);
  // tokens: byte|int|long|float|double|str#|str#L|strL (case-insensitive),
  // followed by whitespace.
  size_t sp = trimmed.find_first_of(" \t");
  if (sp == std::string::npos) return info;
  std::string tok = lower(trimmed.substr(0, sp));
  bool isNumericType = (tok == "byte" || tok == "int" || tok == "long" || tok == "float" ||
                        tok == "double");
  bool isStrType = false;
  if (tok.rfind("str", 0) == 0) {
    // str, str#, str#L, strL — everything after "str" is digits and/or "l".
    std::string rest = tok.substr(3);
    bool ok = true;
    for (char c : rest) {
      if (!std::isdigit(static_cast<unsigned char>(c)) && c != 'l') {
        ok = false;
        break;
      }
    }
    isStrType = ok;
  }
  if (isNumericType || isStrType) {
    size_t consumed = (start + sp);
    // consume the run of whitespace after the type token too
    size_t after = raw.find_first_not_of(" \t", consumed);
    info.expr = (after == std::string::npos) ? "" : raw.substr(after);
    info.stringTyped = isStrType;
    info.typeToken = tok;  // BUG-250: keep the declaration alive
  }
  return info;
}

// BUG-471 T2: an UNTYPED creation under `set type double` is a double, and its
// label says so (w2e_float_default.log settype_double_gen :1252,
// egen_mean_settype_double :1196). Under the factory default float the record is
// BUG-295's implicit plane exactly as before, so the default path is unchanged.
StorageTypeInfo withSessionDefaultType(const SessionState& session, StorageTypeInfo info) {
  if (info.typeToken.empty() && session.defaultStorageType != "float")
    info.typeToken = session.defaultStorageType;
  return info;
}

// format-display-default-lifecycle: a creation arm renders its varlist card
// BEFORE the arm records the new variable's storage type, so the card's format
// for it was resolved with no type (%9.0g for a `generate double`). Re-resolve
// that one entry once the type is recorded, so the card states what describe
// and `: format` answer. A just-created column states no format of its own (the
// engine never sends one for a computed column, BUG-365).
void refreshCardFormat(const SessionState& session, ojson& blk, const std::string& varname) {
  if (varname.empty() || !blk.is_object() || !blk.contains("varlist_after") ||
      !blk["varlist_after"].is_array())
    return;
  for (auto& e : blk["varlist_after"]) {
    if (!e.is_object() || e.value("name", std::string()) != varname) continue;
    VarInfo v;
    v.name = varname;
    v.dtype = e.value("dtype", std::string());
    e["format"] = session.displayFormat(v);
  }
}

// BUG-250: record a declared storage subtype after a creation arm succeeded.
// `blk` is the block the arm is about to return — an "error" block means the
// column was not created and nothing may be recorded (a stale claim on a
// non-column is the BUG-190 defect family). No declaration or no name = no-op,
// so every undeclared `gen` stays byte-identical.
void recordDeclaredStataType(SessionState& session, const std::string& varname,
                             const StorageTypeInfo& info, ojson& blk) {
  if (varname.empty()) return;
  if (blk.is_object() && blk.value("type", std::string()) == "error") return;
  if (info.typeToken.empty() || info.typeToken == "str") {
    // FU-936: an untyped (or bare `str`) string generate has its str# recorded
    // by the row writer (strmutate writeNew, from the longest value); keep it
    // and re-resolve the card's format from it.
    auto it = session.varMeta.find(varname);
    if (it != session.varMeta.end() && it->second.stataType.has_value() &&
        it->second.stataType->compare(0, 3, "str") == 0) {
      session.implicitFloatVars.erase(varname);
      refreshCardFormat(session, blk, varname);
      return;
    }
  }
  if (info.typeToken.empty()) {
    // BUG-295: no declaration means Stata's own default, `float` — record it on
    // the implicit plane so the .dta export stops handing this column to the
    // writer's narrowest-lossless scan (which stored `byte` for `gen z = _n`,
    // and made the SAME column answer float before a save and byte after).
    // Not written to varMeta.stataType: `describe`/`: type` already answer
    // float here via BUG-250's fallback, and a declaration this layer never
    // saw must not start looking like one.
    session.implicitFloatVars.insert(varname);
    return;
  }
  session.implicitFloatVars.erase(varname);  // an explicit declaration wins
  // The token is lowercased by the strip; Stata spells the long-string type
  // `strL` (`: type` answers strL, and the .dta reader labels it so).
  session.varMeta[varname].stataType = info.typeToken == "strl" ? std::string("strL")
                                                                  : info.typeToken;
  refreshCardFormat(session, blk, varname);
}

// BUG-1123: generate's "(N missing values generated)", "(1 missing value
// generated)" for one and nothing for none (StataBE 19.5, numeric and string
// alike: an empty string counts). The count is the new column's n_missing on the
// data_mutation block the arm just pushed; a display block, so `quietly` drops it
// as Stata does. It used to reach the console only as that field.
// Stata counts the expression's missing values, before the storage type turns the
// ones it cannot hold into missing too: `gen byte b = 300` and `gen float f =
// 1e40` print nothing, `gen int i = 1e6 if _n>1` on 5 rows "(1 missing value
// generated)" (StataBE 19.5). Those stores are the engine's `store_overflow`,
// which runGen/runBysortGen leave on the block (translate::carryStoreOverflow),
// and the string-function path's own count (strmutate writeNew's storeAs);
// they are taken off the count, and the field goes no further.
void appendGenMissingNote(ojson& blocks, const std::string& varname) {
  if (varname.empty() || blocks.empty()) return;
  ojson& blk = blocks.back();
  if (!blk.is_object() || blk.value("type", std::string()) != "data_mutation") return;
  long long overflow = 0;
  if (auto it = blk.find("store_overflow"); it != blk.end()) {
    if (it->is_number_integer()) overflow = it->get<long long>();
    blk.erase(it);
  }
  if (!blk.contains("varlist_after") || !blk["varlist_after"].is_array()) return;
  for (const auto& e : blk["varlist_after"]) {
    if (!e.is_object() || e.value("name", std::string()) != varname) continue;
    const auto nm = e.find("n_missing");
    if (nm == e.end() || !nm->is_number_integer()) return;
    const long long n = nm->get<long long>() - overflow;
    if (n <= 0) return;
    std::string digits = std::to_string(n), count;  // "(1,500 missing values generated)"
    for (size_t i = 0; i < digits.size(); ++i) {
      if (i > 0 && (digits.size() - i) % 3 == 0) count += ',';
      count += digits[i];
    }
    ojson note;
    note["type"] = "display";
    note["class"] = "note";
    note["text"] = "(" + count + " missing value" + (n == 1 ? "" : "s") + " generated)";
    blocks.push_back(std::move(note));
    return;
  }
}

// format-display-default-lifecycle: `compress [varlist]`. It was an
// accept-and-ignore no-op, so a compressed variable kept answering the type
// (and the default format) it had before. METADATA only, like recast: the
// frame stores every number as f64 and every string as a factor, so the
// demotion is the declared storage type ([D] compress: double to long, int or
// byte; float, long to int or byte; int to byte; str# to the longest value's
// width). A numeric keeps its format and a string's default follows its new
// width (prepareStorageChange): w2af_format_lifecycle.log af_compress
// `byte|%9.0g|int|%10.0g|byte|%12.0g|str2|%9s`, and the messages at
// :763-767. A string column with no recorded width is left as it is: there
// is no "was" type to report. BUG-1102 (FU-1043): a strL becomes the str# of
// its longest value, at least str1, when every value fits a str# (2045 bytes),
// none holds a binary 0, and the str# takes less memory than the strL would
// once its duplicates were stored once ([D] compress: it "will do nothing if
// it cannot save memory by changing a strL"). Stata counts a strL observation
// as 8 bytes when empty, 81 bytes plus the text when it holds its own copy,
// and 32 bytes when it shares an earlier copy. StataBE 19.5, measured
// 2026-11-03: "ab" in 2 observations saves 111, in 3 saves 141; 100
// observations of string(_n) save 7,992 as str3, but with one 2,000-byte
// value they stay strL ("(0 bytes saved)"), as does one `generate` of a
// 50-character text over 100 observations; 10 observations alternating two
// 50-character texts become str50 (810 saved), alternating 55-character ones
// stay; a tie stays. Whether a value shares a copy depends on how it was
// written: one command shares a value equal to the last non-empty one it
// wrote, so the reported saving takes a value equal to the previous non-empty
// one as shared. That misses copies written by separate commands (Stata
// counts them in full) and copies a sort moved apart (Stata still shares
// them); the decision itself does not depend on it. A strL that stays is not
// coalesced (Stata's `s is strL now coalesced`). Its line follows the str#
// lines (`s3 was str20 now str3` before `sl was strL now str2`; w2_geo_strl
// .log:231 `variable t was strL now str5`). The messages are command
// output ("display" blocks, as destringNoteBlock): `quietly`/`capture`
// suppress them and `noisily`/`capture noisily` print them (log:762-767). An
// "echo" block is the command echo, which those prefixes treat the other way.
std::vector<ojson> runCompressMeta(const eval::ParsedCommand& parsed, SessionState& session) {
  std::vector<ojson> out;
  auto echo = [&out](const std::string& text) {
    ojson b;
    b["type"] = "display";
    b["text"] = text;
    out.push_back(std::move(b));
  };
  auto commas = [](int64_t v) {
    std::string d = std::to_string(v), r;
    for (size_t i = 0; i < d.size(); ++i) {
      if (i > 0 && (d.size() - i) % 3 == 0) r += ',';
      r += d[i];
    }
    return r;
  };
  if (!session.hasData()) {
    echo("  (0 bytes saved)");
    return out;
  }
  std::vector<std::string> vars;
  {
    std::string spec;
    for (const std::string& tok : parsed.varlist) spec += (spec.empty() ? "" : " ") + tok;
    auto vr = loops::expandVarlist(spec.empty() ? std::string("_all") : spec, session);
    if (vr.isError) {
      out.push_back(vr.error);
      return out;
    }
    vars = vr.tokens;
  }
  Datastore* ds = session.datastore();
  if (ds == nullptr) {
    out.push_back(ojson(eval::noDataLoaded()));
    return out;
  }
  const int64_t n = ds->nRows();
  auto bytesOf = [](const std::string& t) -> int64_t {
    if (t == "byte") return 1;
    if (t == "int") return 2;
    if (t == "long" || t == "float") return 4;
    if (t == "double") return 8;
    if (t.size() > 3 && t.compare(0, 3, "str") == 0 &&
        std::all_of(t.begin() + 3, t.end(), [](unsigned char c) { return std::isdigit(c) != 0; }))
      return std::stoll(t.substr(3));
    return 0;
  };
  struct Change {
    std::string name, from, to;
    int64_t saved = 0;
  };
  // Stata reports floats, then ints, longs and doubles, then strings, each in
  // varlist order (af_compress :763-766: float, long, double, str#), then strLs.
  static const std::vector<std::string> kOrder = {"float", "int",  "long",
                                                  "double", "str", "strL"};
  std::map<std::string, std::vector<Change>> byFrom;
  for (const std::string& v : vars) {
    if (!ds->has(v)) continue;
    auto mit = session.varMeta.find(v);
    std::string cur = (mit != session.varMeta.end() && mit->second.stataType.has_value())
                          ? *mit->second.stataType
                          : std::string();
    const bool isStr = ds->dtype(v) == "factor";
    const bool isStrL = isStr && cur == "strL";
    std::string to;
    int64_t strLBytes = 0;
    if (isStr) {
      if (!isStrL && (bytesOf(cur) == 0 || cur.compare(0, 3, "str") != 0)) continue;
      const cecon::Column& c = ds->col(v);
      const int32_t* codes = c.codes();
      size_t longest = 0;
      bool binary = false;
      int64_t coalescedBytes = 0;
      int32_t lastCode = -1;
      std::vector<char> seen(isStrL ? c.levels.size() : 0, 0);
      for (int64_t i = 0; i < n; ++i) {
        const int32_t k = codes[i];
        size_t len = 0;
        if (k >= 0 && static_cast<size_t>(k) < c.levels.size()) {
          const std::string& text = c.levels[static_cast<size_t>(k)];
          len = text.size();
          binary = binary || text.find('\0') != std::string::npos;
        }
        longest = std::max(longest, len);
        if (!isStrL) continue;
        const int64_t copy = 81 + static_cast<int64_t>(len);
        if (len == 0) {
          strLBytes += 8;
          coalescedBytes += 8;
          continue;
        }
        strLBytes += k == lastCode ? 32 : copy;
        coalescedBytes += seen[static_cast<size_t>(k)] != 0 ? 32 : copy;
        seen[static_cast<size_t>(k)] = 1;
        lastCode = k;
      }
      const std::string t = "str" + std::to_string(std::max<size_t>(1, longest));
      if (isStrL ? (longest <= 2045 && !binary && bytesOf(t) * n < coalescedBytes)
                 : bytesOf(t) < bytesOf(cur))
        to = t;
    } else {
      if (cur.empty()) cur = "float";  // BUG-250/BUG-295: `: type` answers float
      if (cur == "byte" || bytesOf(cur) == 0) continue;
      const double* d = ds->col(v).f64();
      bool integral = true;
      double lo = 0.0, hi = 0.0;
      bool any = false;
      for (int64_t i = 0; i < n && integral; ++i) {
        const double x = d[i];
        if (std::isnan(x)) continue;
        if (!std::isfinite(x) || std::trunc(x) != x) {
          integral = false;
          break;
        }
        if (!any || x < lo) lo = x;
        if (!any || x > hi) hi = x;
        any = true;
      }
      if (!integral) continue;
      std::string t;
      if (lo >= -127.0 && hi <= 100.0) t = "byte";
      else if (lo >= -32767.0 && hi <= 32740.0) t = "int";
      else if (lo >= -2147483647.0 && hi <= 2147483620.0) t = "long";
      if (!t.empty() && bytesOf(t) < bytesOf(cur)) to = t;
    }
    if (to.empty()) continue;
    const int64_t fromBytes = isStrL ? strLBytes : bytesOf(cur) * n;
    byFrom[isStrL ? cur : isStr ? std::string("str") : cur].push_back(
        {v, cur, to, fromBytes - bytesOf(to) * n});
  }
  int64_t saved = 0;
  for (const std::string& from : kOrder) {
    auto it = byFrom.find(from);
    if (it == byFrom.end()) continue;
    for (const Change& c : it->second) {
      VarInfo vi;
      vi.name = c.name;
      vi.dtype = from == "str" || from == "strL" ? "factor" : "f64";
      session.prepareStorageChange(vi);
      session.varMeta[c.name].stataType = c.to;
      session.implicitFloatVars.erase(c.name);
      saved += c.saved;
      echo("  variable " + c.name + " was " + c.from + " now " + c.to);
    }
  }
  echo("  (" + commas(saved) + " bytes saved)");
  return out;
}

// BUG-373: collapse reports r(N) = the number of observations it PROCESSED, the
// row count BEFORE it ran (live StataBE: 336 on a 336-row fixture, one row left).
// contract and fillin post nothing: their 336 in that capture was the previous
// command's r(N) (BUG-1451's discriminating probe: 28 from `summarize` survives
// them on 30 rows).
namespace {
int64_t rowsBeforeForRN(SessionState& session) {
  if (std::optional<int64_t> r = session.residentRowsIfCurrent()) return *r;
  Datastore* ds = session.datastore();
  return ds != nullptr ? ds->nRows() : -1;
}
}  // namespace

// BUG-922: one more preserve level (SessionState::doFileDepth) for a nested
// do/run file or a `parallel` child while it runs, restored on every exit path.
// Without it a sub-file that preserves while its caller holds a copy was
// r(621) "already preserved" (Stata gives each do-file its own level).
namespace {
class DoFileLevelGuard {
 public:
  explicit DoFileLevelGuard(SessionState& session) : session_(session) { ++session_.doFileDepth; }
  ~DoFileLevelGuard() { --session_.doFileDepth; }
  DoFileLevelGuard(const DoFileLevelGuard&) = delete;
  DoFileLevelGuard& operator=(const DoFileLevelGuard&) = delete;

 private:
  SessionState& session_;
};
}  // namespace

std::string joinVarlist(const std::vector<std::string>& varlist) {
  std::string s;
  for (size_t i = 0; i < varlist.size(); ++i) {
    if (i) s += " ";
    s += varlist[i];
  }
  return s;
}

// generate/replace expression text (FU-639/FU-652): joinVarlist(varlist) puts
// one blank between the blank-split tokens, which collapses a run of blanks
// inside a string literal (`generate b = "x  y"` stored "x y"). argText is the
// same text as typed; blank runs outside quotes still collapse to one blank, so
// everything downstream sees the joinVarlist spelling except inside literals.
// A varlist that no longer matches argText (rewritten after tokenize) keeps the
// joinVarlist text.
std::string genExprText(const eval::ParsedCommand& parsed) {
  std::istringstream iss(parsed.argText);
  std::vector<std::string> toks;
  std::string tok;
  while (iss >> tok) toks.push_back(tok);
  if (toks != parsed.varlist) return joinVarlist(parsed.varlist);
  const std::string& a = parsed.argText;
  const std::vector<bool> quoted = eval::quotedSpanMask(a);
  std::string out;
  bool pendingBlank = false;
  for (size_t i = 0; i < a.size(); ++i) {
    const bool ws = std::isspace(static_cast<unsigned char>(a[i])) != 0;
    if (ws && !quoted[i]) {
      pendingBlank = !out.empty();
      continue;
    }
    if (pendingBlank) out += ' ';
    pendingBlank = false;
    out += a[i];
  }
  return out;
}

// --- sysuse (mission 3, hardening queue) path helpers -----------------------
// `sysuse DATASET` resolves a bundled sample dataset by bare name, no path.
// PIET ships exactly one today — piet/packaging/sample/fatalities.csv, the
// SAME fixture main.cpp's runUiSmoke() already loads bundle-first via
// piet::bundleResourcesDir()+"/sample/fatalities.csv", dev-tree
// piet/packaging/sample/fatalities.csv otherwise (see main.cpp:957-968).
// This mirrors that exact resolution order rather than inventing a second
// convention; the dev-tree anchor itself is shared (piet/bundle_paths.h,
// devTreePath) since BUG-1939 showed that a per-site __FILE__ anchor breaks
// identically at every site under -ffile-prefix-map.
bool fileExistsLocal(const std::string& path) {
  if (path.empty()) return false;
  std::ifstream f(cecon::to_native_path(path));
  return f.good();
}

std::string devTreeSampleFatalitiesCsv() {
  // BUG-1939: resolved through piet::devTreePath (executable-relative with the
  // compile-time anchor only as a first candidate), not through __FILE__ alone.
  return piet::devTreePath("/piet/packaging/sample/fatalities.csv");
}

// bysort.py's needs_bygen: `_n` or `_N` as a whole identifier token anywhere
// in expr (not part of a longer identifier). A conservative scan mirroring
// the oracle's regex intent without pulling in <regex> for two literals.
std::string dispatcherNextPath(const std::string& workdir, const std::string& prefix) {
  return mintScratchPath(workdir, prefix);  // BUG-389: one body, registered at mint
}

bool hasNorCapN(const std::string& expr) {
  // A byte >= 0x80 is a name byte (core/ident.h): `_nملاحظة` is a name, not `_n`.
  auto isIdentChar = [](char c) { return cecon::ident::isIdentByte(c); };
  for (size_t i = 0; i + 1 < expr.size() + 1; ++i) {
    for (const char* needle : {"_n", "_N"}) {
      size_t len = 2;
      if (i + len > expr.size()) continue;
      if (expr.compare(i, len, needle) != 0) continue;
      bool leftOk = (i == 0) || !isIdentChar(expr[i - 1]);
      bool rightOk = (i + len >= expr.size()) || !isIdentChar(expr[i + len]);
      if (leftOk && rightOk) return true;
    }
  }
  return false;
}

}  // namespace

namespace {

// A5-04: forwards to the shared strutil copy (piet/strutil.h) — see the
// lower() wrapper above for why this stays a thin pass-through.
std::string trimWs(const std::string& s) { return strutil::trim(s); }

// D1-②: the prefix table, sorted once instead of once per call.
//
// WHY. stripPrefix used to be handed a `std::set<std::string>` and, ON EVERY
// CALL, copy it into a vector (one heap allocation per spelling — 5 for the
// capture set, 11 for the quietly set) and sort that vector, then throw both
// away. It runs twice per dispatched line, so `local q 5` paid ~32 string
// allocations and two sorts to answer "does this line begin with `capture`?".
// The WS-D1 zone profile put the pair at 10.7% of the whole per-line budget,
// second only to the macro-definition regexes; the sampling profiler agreed
// (9.5%, of which 17% was the set->vector copy and 11% more the sort).
//
// The std::set stays the declared source of truth at each call site; the table
// is DERIVED from it in a function-local static, so it is built once and
// cannot drift from the set it came from. `first` is a first-byte bitmap that
// rejects a line which cannot begin with any spelling in the table before a
// single std::string is touched.
struct PrefixTable {
  std::vector<std::string> byLenDesc;  // longest-first (see the loop below)
  bool first[256] = {false};

  explicit PrefixTable(const std::set<std::string>& prefixes)
      : byLenDesc(prefixes.begin(), prefixes.end()) {
    std::sort(byLenDesc.begin(), byLenDesc.end(),
              [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
    for (const auto& p : byLenDesc) {
      if (p.empty()) continue;
      unsigned char lo = static_cast<unsigned char>(p[0]);
      first[lo] = true;
      if (lo >= 'a' && lo <= 'z') first[lo - 'a' + 'A'] = true;
    }
  }
};

inline char asciiLower(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

// dispatcher.py's `_strip_prefix`: if `line` starts with a complete word in
// `prefixes` (optionally followed by ':'), return the remainder; else nullopt.
std::optional<std::string> stripPrefix(const std::string& line, const PrefixTable& table) {
  // First-byte reject, ahead of the trim's allocation. trimWs() is
  // strutil::trim, whose leading edge uses the same isspace class this scan
  // does, so the byte found here is exactly the byte the old
  // `strippedLower.rfind(pfx, 0)` compared first.
  size_t b = 0;
  while (b < line.size() && std::isspace(static_cast<unsigned char>(line[b]))) ++b;
  if (b >= line.size() || !table.first[static_cast<unsigned char>(line[b])]) return std::nullopt;

  dprof::notePrefixSetSort();
  std::string stripped = trimWs(line);
  // Longest-first so "quiet"/"quietly" don't shadow each other incorrectly
  // (mirrors Python's `sorted(prefixes, key=len, reverse=True)`).
  for (const auto& pfx : table.byLenDesc) {
    if (stripped.size() < pfx.size()) continue;
    bool eq = true;
    for (size_t i = 0; i < pfx.size(); ++i) {
      if (asciiLower(stripped[i]) != pfx[i]) {
        eq = false;
        break;
      }
    }
    if (!eq) continue;
    std::string rest = stripped.substr(pfx.size());
    if (!rest.empty() && rest[0] != ' ' && rest[0] != '\t' && rest[0] != ':') continue;
    // strip a single leading ':' then whitespace
    size_t i = 0;
    if (i < rest.size() && rest[i] == ':') ++i;
    rest = trimWs(rest.substr(i));
    if (!rest.empty()) return rest;
  }
  return std::nullopt;
}

// Is `varname` a variable of the loaded data? nullopt = no datastore to ask.
std::optional<bool> targetVarExists(SessionState& session, const std::string& varname) {
  // C4-L2A (D1 card plane): existence is card-answerable — a CURRENT card is
  // authoritative for name presence either way (handlers_m3.cpp::runConfirm's
  // idiom). nullptr means "answer from the datastore", never "assume absent".
  if (const std::vector<VarInfo>* card = session.residentReadCard()) {
    for (const auto& vi : *card) {
      if (vi.name == varname) return true;
    }
    return false;
  }
  Datastore* ds = session.datastore();
  if (ds == nullptr) return std::nullopt;
  return ds->has(varname);
}

// BUG-112(1) PIET-half: Stata's `generate` REFUSES a name that already exists
// (r(110) "variable X already defined"); `replace` is the verb that overwrites.
// PIET honoured that on exactly ONE of its four gen sinks — strmutate::
// runStringGen guards (strmutate.cpp:977) — while the three NUMERIC sinks
// (handlers::runGen · egen::runBysortGen via the standalone `_n`/`_N` + `in`
// route · the same handler via the `bysort v:` prefixed route) all shipped
// straight to `cobra eval --gen`, whose Frame::generate is create-or-replace.
// Live-measured on this tree before the fix: `gen x = 2` over an existing x==1
// moved summarize's mean 1 -> 2 with NO diagnostic, and likewise `gen z = 77`
// over `gen z = _n` (3 -> 77) and `bysort x: gen y = 99` (3 -> 99).
//
// The guard lives HERE, at the VERB, not in the handlers: runGen/runBysortGen
// are also the internal engine of ~10 deliberately create-or-replace callers
// (reshape/egen/encode/bygen-tag/merge-cardinality/mkspline — and `replace`
// itself, which builds the identical `--gen` call), so a handler-level guard
// would reject all of them; selftest's direct piet::handlers::runGen() legs
// bypass dispatch() and stay untouched for the same reason. Verb-level also
// keeps the fix engine-INDEPENDENT — it takes no dependency on the cobra-half's
// `--gen-strict` flag (BUG-112(1) engine-half), so the Art-7 archive-pin arm
// behaves identically to a fresh build instead of failing on an unknown flag.
//
// Message spelling is strmutate's VERBATIM: errors.cpp::coarseRc keys r(110)
// off the "already defined" substring, so a `capture`-wrapped gen sets the same
// _rc Stata does, and the two sinks cannot drift apart in wording.
//
// BUG-1120: a reserved name is refused at the same point, for all four sinks
// alike, by generate's own rule (strutil::generateNameRefusal, with _se and the
// r(table) row names): `generate _cons = 1` is "_cons invalid varname" r(198),
// `generate _skip = 1` "_skip not allowed" r(101), `generate in = 1` "'=' invalid
// observation number" r(198), `generate double float = 1` "too few variables
// specified" r(102) (StataBE 19.5, typed or not, with `if`/`in` or not). They used
// to be created. `typeWord` is the storage type stripStorageType took off the
// line, as typed: when nothing stands between it and `=` (`generate byte = 2`),
// Stata read it as the type of a variable never named, r(102) -- this used to
// create a variable with an empty name. str, strl and str0 are names to Stata
// that the strip takes for types; those are left as they were.
std::optional<ojson> genTargetNameError(SessionState& session, const std::string& varname,
                                        const std::string& typeWord = std::string()) {
  std::optional<strutil::NameRefusal> refusal =
      strutil::generateNameRefusal(varname.empty() ? typeWord : varname, /*resultRows=*/true);
  if (refusal.has_value()) {
    return ojson(eval::errorBlock("engine_error", refusal->message, std::nullopt, std::nullopt,
                                  std::nullopt, "r(" + std::to_string(refusal->rc) + ")"));
  }
  if (varname.empty() || !session.hasData()) return std::nullopt;
  if (!targetVarExists(session, varname).value_or(false)) return std::nullopt;
  return ojson(eval::engineError("variable " + varname + " already defined"));
}

// BUG-1142 (FU-971): `replace` changes an EXISTING variable and never creates
// one, and Stata reads the target before anything else (StataBE 19.5, measured
// 2026-10-29): a storage type is r(101) "double not allowed"; a name that is no
// variable is r(111) "variable qq not found", with or without an `=exp`; a
// wildcard matching several is r(103) "too many variables specified"; an
// existing name with no `=exp` is r(100) "=exp required". Both sinks build the
// same create-or-replace `--gen` call, so `replace qq = 7` used to make qq at
// rc 0. The target may be an abbreviation or a one-match wildcard ([U] 11.2):
// StataBE rp.do `replace edu_category = 0 if edu_category==.` replaces
// edu_category_BL (007's Alt_main_results_alternate_controls.do), so
// `info.expr` is rewritten onto the full name. Other target shapes (`x[2]`,
// `x:lbl`, `x y`) are left to the sinks' own refusals.
std::optional<ojson> replaceTargetError(SessionState& session, StorageTypeInfo& info) {
  if (!info.typeToken.empty()) {
    return ojson(eval::errorBlock("syntax_error", info.typeToken + " not allowed", std::nullopt,
                                  std::nullopt, std::nullopt, "r(101)"));
  }
  if (!session.hasData()) return std::nullopt;
  const size_t eq = info.expr.find('=');
  const std::string target =
      trimWs(eq == std::string::npos ? info.expr : info.expr.substr(0, eq));
  if (target.empty() || !cecon::ident::isIdentStartByte(target.front()) ||
      target.find_first_of(" []():") != std::string::npos)
    return std::nullopt;
  std::vector<std::string> names = {target};
  if (auto err = handlers::resolveByKeyNames(session, names)) return err;
  if (names.size() > 1) {
    return ojson(eval::errorBlock("syntax_error", "too many variables specified", std::nullopt,
                                  std::nullopt, std::nullopt, "r(103)"));
  }
  if (names.empty() || !targetVarExists(session, names.front()).value_or(true)) {
    return ojson(eval::errorBlock("engine_error", "variable " + target + " not found",
                                  std::nullopt, std::nullopt, std::nullopt, "r(111)"));
  }
  if (eq == std::string::npos) {
    return ojson(eval::errorBlock("syntax_error", "=exp required", std::nullopt, std::nullopt,
                                  std::nullopt, "r(100)"));
  }
  if (names.front() != target)
    info.expr = names.front() + " = " + trimWs(info.expr.substr(eq + 1));
  return std::nullopt;
}

// BUG-1144: replace's one option is a flag, given once. `replace f = 2.5,
// nopromote(1)` is r(198) "option nopromote() incorrectly specified" on StataBE
// 19.5 and f stays int, and `replace f = 1, nopromote nopromote` is r(198)
// "option nopromote not allowed" (measured 2026-10-29); kVocabReplace's net
// accepts the key whatever it carries and however often, and a valued key never
// set the flag, so f was promoted to float.
std::optional<ojson> replaceNoPromote(
    const std::vector<std::pair<std::string, eval::OptValue>>& opts, bool& noPromote) {
  bool seen = false;
  for (const auto& [k, v] : opts) {
    if (k != "nopromote") continue;
    if (!v.isBool) {
      return ojson(eval::errorBlock("syntax_error", "option nopromote() incorrectly specified",
                                    std::nullopt, std::nullopt, std::nullopt, "r(198)"));
    }
    if (seen) {
      return ojson(eval::errorBlock("syntax_error", "option nopromote not allowed", std::nullopt,
                                    std::nullopt, std::nullopt, "r(198)"));
    }
    seen = true;
    if (v.boolValue) noPromote = true;
  }
  return std::nullopt;
}

// `[type] NEWVAR = expr` (post-stripStorageType text) -> NEWVAR. First `=`
// wins, matching both the gen block's own inline split and handlers.cpp::
// splitAssign. No `=` at all -> empty, i.e. NO exists-guard: that form is a
// syntax error the downstream handler already owns, and Stata likewise reports
// the syntax problem rather than the name one.
std::string genTargetName(const std::string& exprAfterType) {
  size_t eq = exprAfterType.find('=');
  if (eq == std::string::npos) return std::string();
  return trimWs(exprAfterType.substr(0, eq));
}

// The storage-type word stripStorageType took off `raw`, as the user typed it
// ("" when it took none): the strip folds case, and Stata's types do not.
std::string strippedTypeWord(const std::string& raw, const StorageTypeInfo& info) {
  if (info.typeToken.empty()) return std::string();
  std::istringstream iss(raw);
  std::string word;
  iss >> word;
  return word;
}

// `generate if = 1`, `clonevar if = x`: the tokenizer takes `if = 1` for the
// condition, and so does Stata, which then refuses the condition's first word,
// "=1 invalid name" r(198) (StataBE 19.5). Only a plain name or number is answered
// here; `generate if = (x+1)` is Stata's r(133) "unknown function =()" and keeps
// the engine's own refusal.
std::optional<ojson> conditionAsNameError(const std::optional<std::string>& ifExpr) {
  if (!ifExpr.has_value()) return std::nullopt;
  const std::string cond = trimWs(*ifExpr);
  if (cond.empty() || cond[0] != '=') return std::nullopt;
  std::istringstream iss(cond.substr(1));
  std::string word;
  iss >> word;
  if (word.empty()) return std::nullopt;
  for (char c : word)
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.')) return std::nullopt;
  return ojson(eval::errorBlock("syntax_error", "=" + word + " invalid name", std::nullopt,
                                std::nullopt, std::nullopt, "r(198)"));
}

}  // namespace

// ---------------------------------------------------------------------------
// M3b: bysort/by prefix — C++ port of bysort.py's _parse_bysort_prefix +
// run_bysort_cmd. Split out (not in the anonymous namespace above) only so
// it can stay grouped with its own small helpers; still translation-unit
// local.
namespace {

struct BysortPrefix {
  bool isBysort = false;      // false = not a bysort/by line at all
  bool sortFirst = false;     // true=bysort (re-sort), false=by (no re-sort)
  std::vector<std::string> byVars;
  // BS-002: `bysort byvar (sortvar1 sortvar2):` — vars inside the
  // parenthetical are a SORT-ONLY tiebreak key (Stata: equivalent to `sort
  // byvar sortvar1 sortvar2` before grouping on byvar alone), never part of
  // the by-GROUP key. Empty when no parenthetical was present.
  std::vector<std::string> sortVars;
  std::string subCmd;
  std::string subRaw;
};

// _parse_bysort_prefix: cmd="bysort"/"by", varlist=["foreign:","gen","idx=_n"]
// (or the colon as a standalone token), raw="bysort foreign: gen idx=_n".
BysortPrefix parseBysortPrefix(const std::string& cmdLower, const std::vector<std::string>& varlist,
                               const std::string& raw) {
  BysortPrefix p;
  if (cmdLower != "bysort" && cmdLower != "by") return p;
  p.isBysort = true;
  p.sortFirst = (cmdLower == "bysort");

  // BS-002: byVars-collection loop, now paren-aware. A token opening with
  // '(' (and everything up to the token that closes it with ')') is the
  // sort-only parenthetical — collected into sortVars instead of byVars.
  // The trailing ':' that ends the by-spec may be glued to a plain byVar
  // token, to the token that CLOSES the parenthetical, or stand alone —
  // all three are handled uniformly below.
  std::optional<size_t> colonIdx;
  bool inParen = false;
  for (size_t i = 0; i < varlist.size(); ++i) {
    std::string tok = varlist[i];

    if (tok == ":") {
      colonIdx = i;
      break;
    }

    bool endsWithColon = (!tok.empty() && tok.back() == ':');
    if (endsWithColon) tok.pop_back();

    if (!inParen && !tok.empty() && tok.front() == '(') {
      tok.erase(tok.begin());
      inParen = true;  // may close within this same token, handled just below
    }

    if (inParen) {
      size_t closeParen = tok.find(')');
      if (closeParen != std::string::npos) {
        std::string before = tok.substr(0, closeParen);
        if (!before.empty()) p.sortVars.push_back(before);
        inParen = false;
      } else if (!tok.empty()) {
        p.sortVars.push_back(tok);
      }
    } else if (!tok.empty()) {
      p.byVars.push_back(tok);
    }

    if (endsWithColon) {
      colonIdx = i;
      break;
    }
  }

  if (!colonIdx.has_value()) {
    // Malformed — no colon found; everything is by_vars, no sub_cmd.
    return p;
  }

  std::vector<std::string> subTokens(varlist.begin() + static_cast<long>(*colonIdx) + 1,
                                     varlist.end());
  if (subTokens.empty()) return p;
  p.subCmd = subTokens[0];
  std::vector<std::string> subVarlist(subTokens.begin() + 1, subTokens.end());

  size_t colonPos = raw.find(':');
  if (colonPos == std::string::npos) {
    std::string joined = p.subCmd;
    for (const auto& t : subVarlist) joined += " " + t;
    p.subRaw = joined;
  } else {
    p.subRaw = trimWs(raw.substr(colonPos + 1));
  }
  return p;
}

// egen.py-adjacent: split `s` at the first COMMA outside parens (options
// separator) — mirrors bysort.py's _split_top_level_comma, used to inject a
// by() clause into an egen sub-command's option tail.
std::pair<std::string, std::optional<std::string>> splitTopLevelComma(const std::string& s) {
  int depth = 0;
  for (size_t i = 0; i < s.size(); ++i) {
    char ch = s[i];
    if (ch == '(') {
      ++depth;
    } else if (ch == ')') {
      --depth;
    } else if (ch == ',' && depth == 0) {
      return {s.substr(0, i), s.substr(i + 1)};
    }
  }
  return {s, std::nullopt};
}

std::string joinSpace(const std::vector<std::string>& v) {
  std::string s;
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) s += " ";
    s += v[i];
  }
  return s;
}

// P2-PERF ARC-C C1-③ (factored out for BUG-532): is the key tuple `keys`
// already lexicographically non-decreasing, row by row, under gsort's OWN key
// semantics (src/data/frame.cpp `cols_as_double` + radix_sort.h
// `gsort_radix_key`)? A factor's key is its integer CODE as a double, an f64's
// key is its value, NaN sorts LAST, and the sort is stable — so a `true` here
// means a `sort KEYS` would be the identity permutation. The by-summarize
// driver asks exactly that (skip a no-op sort). It is deliberately NOT the
// level-string check the `by:` summarize branch uses for its r(5): that one
// reads factor LEVEL STRINGS, so it calls data PIET itself just sorted on a
// string key "not sorted". Every key must exist in `ds` (callers check first).
//
// `stataStringOrder` (BUG-532, the by-keep/drop branch): a string key is read
// in STATA's order instead — its level's byte-order rank, a blank first
// (handlers::stringKeyOrder) — so `true` means the rows are in the order a
// Stata `sort KEYS` leaves them. The two answers differ only on a string key
// whose dictionary is not byte-ordered.
bool keysAlreadyGrouped(Datastore& ds, const std::vector<std::string>& keyNames,
                        bool stataStringOrder = false) {
  const int64_t nBase = ds.nRows();
  // gsort's key ordering, as a plain comparison: NaN above every number.
  auto keyLess = [stataStringOrder](double a, double b) {
    if (stataStringOrder) return cecon::stata_cmp(a, b) < 0;
    bool an = std::isnan(a), bn = std::isnan(b);
    if (an || bn) return !an && bn;  // NaN last; NaN vs NaN is neither
    return a < b;
  };
  struct GsortKeyView {
    bool isFactor = false;
    const double* f64 = nullptr;
    const int32_t* codes = nullptr;
    std::vector<double> rank;  // stataStringOrder: per code; blankRank for -1
    double blankRank = 0;
    double at(int64_t r) const {
      if (!isFactor) return f64[r];
      if (rank.empty()) return static_cast<double>(codes[r]);
      return codes[r] < 0 ? blankRank : rank[static_cast<size_t>(codes[r])];
    }
  };
  std::vector<GsortKeyView> keys;
  keys.reserve(keyNames.size());
  for (const auto& bv : keyNames) {
    GsortKeyView v;
    v.isFactor = ds.dtype(bv) != "f64";
    const cecon::Column& c = ds.col(bv);
    if (v.isFactor) {
      v.codes = c.codes();
      if (stataStringOrder) {
        handlers::StringKeyOrder o = handlers::stringKeyOrder(c.levels);
        if (!o.codeOrderIsStata) {
          v.rank = std::move(o.rank);
          v.blankRank = o.blankRank;
        }
      }
    } else {
      v.f64 = c.f64();
    }
    keys.push_back(std::move(v));
  }
  for (int64_t r = 1; r < nBase; ++r) {
    for (const auto& k : keys) {
      double prev = k.at(r - 1), cur = k.at(r);
      if (keyLess(cur, prev)) return false;  // strictly decreasing on this key
      if (keyLess(prev, cur)) break;         // strictly increasing -> row pair ok
      // equal on this key -> the next key decides
    }
  }
  return true;
}

// BUG-532: Stata's sortedby marker, as far as PIET can know it, for a plain
// `by KEYS:` (which Stata refuses r(5) unless KEYS lead the marker).
//
// Every `bysort KEYS [(SORT)]: CMD` sorts PIET's rows in place into the order
// Stata's `bysort` leaves (handlers::sortInStataOrder: runBysortGen, the egen
// and summarize/regress/tabstat branches, runByKeepIf), so after any bysort
// PIET's rows ARE Stata's rows and the engine file's own sort claim is the
// marker (below). No side ledger of row orders is kept: it was tried
// (task/w2-bykeep-v2 rounds 3-4) and each review found a new way for a
// ledger mark to disagree with the rows (a macro-spelled gsort, a fresh `use`,
// a re-used file), because a ledger describes lines, not rows.

// (2) The engine's own claim: the head file's `sorted_by` header key (BUG-272).
// Never false where present — Frame::gsort mints it only for ascending f64 keys,
// and every mutation that could void it clears it — and minted exactly where
// Stata's `sort` sets its marker, so a key tuple that does not lead it is one
// Stata calls "not sorted" even when the rows happen to be grouped (arm
// by_two_halfsorted: `sort g b` then `by g h:`). Empty = no claim (factor keys,
// a claimless transform, a file from before BUG-272): the caller falls back to
// the rows themselves. Read off the head's open Datastore (Datastore::sortedBy),
// never by reopening the file.
std::vector<std::string> engineSortClaim(const Datastore& ds) {
  std::vector<std::string> keys;
  std::istringstream in(ds.sortedBy());
  for (std::string k; in >> k;) keys.push_back(k);
  return keys;
}

bool keysLeadClaim(const std::vector<std::string>& keys, const std::vector<std::string>& claim) {
  if (keys.size() > claim.size()) return false;
  for (size_t i = 0; i < keys.size(); ++i)
    if (keys[i] != claim[i]) return false;
  return true;
}

// BUG-532 (review of d82bdfa6, finding F): the by-keys and the parenthesised
// sort keys of a `by/bysort` prefix are a varlist, and Stata resolves them as
// one: a wildcard or range expands (loops::expandVarlist, the BUG-450 varlist
// expander every plain data verb uses), and a name that is not a variable but
// is the unique prefix of one stands for it (varname abbreviation; several
// candidates is r(111) "ambiguous abbreviation"). expandVarlist does not
// resolve abbreviations (a literal token passes through unchanged, and the
// plain `sort` verb answers `sort xv` r(111) on main too), so the prefix step
// is this function's own -- a design decision, no captured arm; [U] 11.2
// "Abbreviation rules". A token that matches nothing is left as typed: each
// branch below refuses it by its own rule (r(111) for keep/drop, the
// summarize driver's own check).
// FU-1088: shared through piet/handlers.h (binscatter's by()/controls()/
// absorb() resolve their names by this same rule), so it lives outside the
// anonymous namespace.
}  // namespace

namespace handlers {

std::optional<ojson> resolveByKeyNames(SessionState& session, std::vector<std::string>& names) {
  if (names.empty() || !session.hasData()) return std::nullopt;
  bool patterned = false;
  for (const auto& t : names)
    if (t.find_first_of("*?-") != std::string::npos || lower(t) == "_all") patterned = true;
  std::vector<std::string> all;
  if (const std::vector<VarInfo>* card = session.shadowVarlistIfCurrent()) {
    for (const auto& v : *card) all.push_back(v.name);
  } else if (Datastore* ds = session.datastore()) {
    all = ds->names();
  }
  if (patterned) {
    loops::VarlistExpandResult ex = loops::expandVarlist(joinSpace(names), session);
    if (ex.isError) {
      // Review of e19291ad (REFUSAL rc): expandVarlist's range branch
      // (`zz-yy` with an endpoint that is no variable) returns a block with no
      // rc, which `capture` reads as 1. Stata answers r(111) "variable zz not
      // found". A block that already carries its rc passes through unchanged.
      const std::string reason = ex.error.value("reason", std::string());
      if (reason.compare(0, 2, "r(") == 0) return ex.error;
      std::string missing;
      for (const auto& t : names) {
        const size_t dash = t.find('-');
        if (dash == std::string::npos || t.find_first_of("*?") != std::string::npos) continue;
        for (const std::string& end : {t.substr(0, dash), t.substr(dash + 1)}) {
          if (missing.empty() && std::find(all.begin(), all.end(), end) == all.end()) missing = end;
        }
      }
      if (missing.empty()) missing = joinSpace(names);
      return ojson(eval::errorBlock("engine_error", "variable " + missing + " not found",
                                    std::nullopt, std::nullopt, std::nullopt, "r(111)"));
    }
    names = ex.tokens;
  }
  for (auto& t : names) {
    if (std::find(all.begin(), all.end(), t) != all.end()) continue;
    std::vector<std::string> hits;
    for (const auto& v : all)
      if (v.size() > t.size() && v.compare(0, t.size(), t) == 0) hits.push_back(v);
    if (hits.size() == 1) {
      t = hits.front();
    } else if (hits.size() > 1) {
      return ojson(eval::errorBlock("engine_error", t + " ambiguous abbreviation", std::nullopt,
                                    std::nullopt, std::nullopt, "r(111)"));
    }
  }
  return std::nullopt;
}

}  // namespace handlers

namespace {

// tsset.ado's and xtset.ado's `syntax [varlist(numeric max=2 default=none)]`
// reads the varlist before any option is looked at, by resolveByKeyNames' rule
// (a wildcard, range or unique abbreviation expands): a name that is no variable
// is r(111), a third variable r(103), a string one r(109). So `xtset zz, foo` is
// 111, `tset s, clear` 109 and `xtset id t s, clear` 103 (StataBE 19.5), never
// the option or `clear` refusal that follows them. Before the count, each word in
// turn: an operator is r(101) "factor-variable and time-series operators not
// allowed", an interaction r(101) "interactions not allowed", a word no name
// starts with r(198) "1 invalid name", a plain name that is no variable r(111) --
// `xtset L.id t` and `xtset id t L.abc` are 101, `xtset 1 L.t` 198, `xtset zz L.t`
// 111 (StataBE 19.5). `names` comes back resolved.
std::optional<ojson> tsVarlistPass(SessionState& session, std::vector<std::string>& names) {
  if (names.empty() || !session.hasData()) return std::nullopt;
  std::map<std::string, bool> isString;
  if (const std::vector<VarInfo>* card = session.shadowVarlistIfCurrent()) {
    for (const auto& v : *card) isString[v.name] = v.dtype != "f64";
  } else if (Datastore* ds = session.datastore()) {
    for (const auto& n : ds->names()) isString[n] = ds->dtype(n) != "f64";
  }
  auto refuse = [](const std::string& msg, const char* rc) {
    return ojson(eval::errorBlock("engine_error", msg, std::nullopt, std::nullopt, std::nullopt, rc));
  };
  for (const auto& tok : names) {
    if (tok.find('.') != std::string::npos)
      return refuse("factor-variable and time-series operators not allowed", "r(101)");
    if (tok.find('#') != std::string::npos) return refuse("interactions not allowed", "r(101)");
    if (!tok.empty() && (std::isdigit(static_cast<unsigned char>(tok[0])) || tok[0] == '"'))
      return refuse(tok + " invalid name", "r(198)");
    if (!strutil::isValidStataIdentifier(tok) || isString.count(tok)) continue;
    int prefixHits = 0;
    for (const auto& [n, str] : isString) prefixHits += n.compare(0, tok.size(), tok) == 0;
    if (prefixHits == 0) return refuse("variable " + tok + " not found", "r(111)");
  }
  if (auto err = handlers::resolveByKeyNames(session, names)) return err;
  for (const auto& n : names)
    if (!isString.count(n)) return refuse("variable " + n + " not found", "r(111)");
  if (names.size() > 2) return refuse("too many variables specified", "r(103)");
  for (const auto& n : names)
    if (isString[n])
      return refuse("string variables not allowed in varlist;\n" + n + " is a string variable",
                    "r(109)");
  return std::nullopt;
}

// tsset.ado's display options, which xtset.ado's PanelSet takes too: mi, noblank,
// panelname(string) and displayindent(integer), each spelled in full (the option
// floors refuse `nob`, `displayi()`). They move out of `opts` into `decl`'s display
// fields (panelname() empty keeps "Panel", a negative indent is none). False when
// displayindent() holds no integer, which tsset answers "option displayindent()
// incorrectly specified" r(198) and PanelSet with its r(101) (StataBE 19.5).
bool takeTsDisplayOptions(std::vector<std::pair<std::string, eval::OptValue>>& opts,
                          handlers::TsDeclare& decl) {
  std::vector<std::pair<std::string, eval::OptValue>> rest;
  for (auto& kv : opts) {
    const std::string& k = kv.first;
    const eval::OptValue& v = kv.second;
    if (k == "mi" || k == "noblank") {
      decl.display = true;
    } else if (k == "panelname") {
      decl.display = true;
      std::string label = v.isBool ? std::string() : trimWs(v.strValue);
      if (label.size() >= 2 && label.front() == '"' && label.back() == '"')
        label = label.substr(1, label.size() - 2);
      if (!label.empty()) decl.panelLabel = label;
    } else if (k == "displayindent") {
      const std::string t = v.isBool ? std::string() : trimWs(v.strValue);
      long long n = 0;
      size_t used = 0;
      try {
        n = std::stoll(t, &used);
      } catch (const std::exception&) {
        return false;
      }
      if (used != t.size()) return false;
      decl.indent = static_cast<int>(std::max(0LL, std::min(n, 80LL)));
      decl.display = true;
    } else {
      rest.push_back(kv);
    }
  }
  opts = std::move(rest);
  return true;
}

ojson tsDisplayIndentError() {
  return ojson(eval::errorBlock("syntax_error", "option displayindent() incorrectly specified",
                                std::nullopt, std::nullopt, std::nullopt, "r(198)"));
}

// The `syntax [, CLEAR]` both ados read when no variable is given: any other
// option is r(198) "option yearly not allowed" (`xtset, foo`, `xtset, delta(1)`,
// `tsset, clear yearly`; StataBE 19.5), in rejectUnknownOptions' wording.
std::optional<ojson> tsOnlyClearAllowed(
    const std::vector<std::pair<std::string, eval::OptValue>>& opts) {
  for (const auto& [k, v] : opts)
    if (k != "clear")
      return ojson(eval::engineError("option `" + k + "` not allowed", "unsupported_option"));
  return std::nullopt;
}

// BS-031(d)/(e) (BETA campaign C2, silent-if-drop sweep): the STRING gen/
// replace path (strmutate::runStringGen/runStringReplace, routed from the
// `generate`/`replace` arms below when needsStringEval() fires) only ever
// received `ifExpr` — never `parsed.inRange` — so `gen str8 sv = "x" in
// 1/10` silently set ALL 336 rows instead of just the 10 in-range ones
// (live-verified this stint: count if sv=="x" -> 10, count if sv=="" ->
// 326; `replace sv = "y" in 5/8` similarly: 4 changed, 332 unchanged). The
// numeric gen/replace path already folds `in a/b` into the same cond() the
// if-expr builds (BUG-077, egen.cpp's runBysortGen) — this is the
// IDENTICAL MVP grammar (bare INT, INT/INT, f/INT, INT/l, f/l; negative
// bounds rejected) reproduced here as a file-local helper so the STRING
// path (strmutate.cpp, a different TU with its own row-wise `_n`/`_N`
// evaluator — see strmutate.cpp's evalRows) can fold `in` into a combined
// RAW (un-rewritten) Stata-syntax ifExpr string BEFORE calling
// runStringGen/runStringReplace, which each internally rewrite_expr() the
// ifExpr they're given exactly like they already do for a real `if`. Not a
// new shared abstraction (matches this codebase's own established per-TU
// duplication convention for small helpers like this).
struct StringInRangeFold {
  bool ok = false;
  bool negative = false;
  // "" means neither `if` nor `in` was present (a genuinely absent cond).
  std::string combinedIf;
};

StringInRangeFold foldStringInRange(const std::optional<std::string>& ifExpr,
                                    const std::optional<std::string>& inRange) {
  StringInRangeFold res;
  bool haveIf = ifExpr.has_value() && !ifExpr->empty();
  bool haveRange = inRange.has_value() && !inRange->empty();
  if (!haveRange) {
    res.ok = true;
    res.combinedIf = haveIf ? *ifExpr : std::string();
    return res;
  }
  auto parseBound = [](const std::string& raw) -> std::optional<std::string> {
    std::string t = trimWs(raw);
    std::string tl = lower(t);
    if (tl == "f") return std::string("1");
    if (tl == "l") return std::string("_N");
    if (t.empty()) return std::nullopt;
    if (t[0] == '-') return std::nullopt;  // negative — caller checks this case separately
    for (char c : t) {
      if (!std::isdigit(static_cast<unsigned char>(c))) return std::nullopt;
    }
    return t;
  };
  std::string t = trimWs(*inRange);
  size_t slash = t.find('/');
  std::string p1 = slash == std::string::npos ? t : t.substr(0, slash);
  std::string p2 = slash == std::string::npos ? t : t.substr(slash + 1);
  if ((!p1.empty() && p1[0] == '-') || (!p2.empty() && p2[0] == '-')) {
    res.negative = true;
    return res;
  }
  auto a = parseBound(p1);
  auto b = parseBound(p2);
  if (!a.has_value() || !b.has_value()) {
    return res;  // ok=false, negative=false -> generic "unsupported form"
  }
  std::string rangePred = "_n>=" + *a + " & _n<=" + *b;
  res.combinedIf = haveIf ? ("(" + *ifExpr + ") & (" + rangePred + ")") : rangePred;
  res.ok = true;
  return res;
}

// Turns a failed foldStringInRange() into the same honest-reject shape
// egen.cpp's runBysortGen already uses for the identical grammar, so a
// string-typed `gen`/`replace ... in ...` gets byte-identical wording to
// its numeric sibling.
ojson stringInRangeError(const StringInRangeFold& fold, const std::string& inRangeRaw) {
  if (fold.negative) {
    return ojson(eval::unsupportedOption(
        "in " + inRangeRaw,
        "Negative (from-the-end) `in` bounds are not supported yet. Use a positive 1-based "
        "row range, e.g. `in 1/10`, `in f/10`, `in 5/l`, `in f/l`."));
  }
  return ojson(eval::unsupportedOption(
      "in " + inRangeRaw,
      "This `in` row-range form is not supported for gen/replace. Supported forms: INT, "
      "INT/INT, f/INT, INT/l, f/l (f = first row, l = last row)."));
}

// BS-011 (BETA B-graph-B): coefplot/marginsplot are graph-family results
// but their dispatch branches (below) return immediately after computing
// the block, never reaching the big scatter/histogram/twoway/... block's
// own tail that caches `session.lastGraph` and captures a bare `name()`
// into `session.namedGraphs`. Without that, `graph export`/`graph save`/
// `graph combine` unconditionally failed with "no graph in memory"
// immediately after a coefplot/marginsplot (BS-011 evidence). This is a
// literal mirror of that existing tail (same name-capture regex/logic,
// copied — not a refactor of the scatter/twoway block's own copy, and not
// a new shared abstraction either; unifying all call sites onto one
// consumer is BS-099's Phase E job, out of this stint's scope).
// graph-family AI-4: the describe record of the graph a statement draws:
// r(command) is the statement after a leading `graph` word as typed (w2ai
// gf_bar_mean_dc `bar (mean) y, over(g) nodraw name(gf_a1, replace)`, log:236;
// gf_tw_plain_dc `twoway scatter y x, ...`, :1313); a bare twoway plot command
// is `twoway <command>`. r(scheme) is scheme(), else stcolor.
ojson graphMetaFor(const std::string& raw, const std::string& family,
                   const std::vector<std::pair<std::string, eval::OptValue>>& opts) {
  const std::string r = strutil::trim(raw);
  size_t e = 0;
  while (e < r.size() && !std::isspace(static_cast<unsigned char>(r[e])) && r[e] != ',') ++e;
  const std::string w0 = strutil::lower(r.substr(0, e));
  std::string command;
  if (w0 == "gr" || w0 == "gra" || w0 == "grap" || w0 == "graph") {
    command = strutil::trim(r.substr(e));
  } else if (w0 == "scatter" || w0 == "sc" || w0 == "line" || w0 == "connected" || w0 == "tsline") {
    command = "twoway " + r;
  } else {
    command = r;
  }
  std::string scheme;
  for (const auto& [k, v] : opts) {
    if ((k == "scheme" || k == "sch" || k == "sche" || k == "schem") && !v.isBool) {
      std::string t = strutil::trim(v.strValue);
      const size_t sp = t.find_first_of(" ,");
      scheme = sp == std::string::npos ? t : t.substr(0, sp);
    }
  }
  return serset::makeGraphMeta(family, command, scheme);
}

void registerGraphOutput(ojson& result, SessionState& session,
                          const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                          const std::string& raw = std::string()) {
  if (!(result.is_object() && result.value("type", std::string()) == "graph")) return;
  std::string nameOpt;
  for (const auto& [k, v] : opts) {
    if (k == "name" && !v.isBool) nameOpt = v.strValue;
  }
  // serset-twoway SS: the one registration (piet/serset.h). These graphs make no
  // sersets of their own yet; an unnamed one is "Graph", as in Stata.
  const ojson meta = graphMetaFor(raw, "twoway", opts);
  serset::registerGraph(session, result, nameOpt, {}, &meta);
}

// serset-twoway SS: the last name() a graph command was given ("" = none).
std::string graphNameOption(const std::vector<std::pair<std::string, eval::OptValue>>& opts) {
  std::string nameOpt;
  for (const auto& [k, v] : opts) {
    if (k == "name" && !v.isBool) nameOpt = v.strValue;
  }
  return nameOpt;
}

// BS-053: `twoway scatter y x || lfit y x` -- Stata's `||` double-pipe
// layer separator, an alternate spelling of the parenthesised
// `twoway (scatter y x) (lfit y x)` form (same layer semantics, just
// without the parens). twoway_parse.cpp's parser only ever recognises
// TOP-LEVEL `(...)` groups as layer boundaries; a bare `||`-separated
// string has none, so it fell into the "no top-level parens" bare
// single-plot branch, which only ever looks at the FIRST plot-type token
// and silently drops everything from the first `||` onward (SCOPE's
// "silently truncates to first plot").
//
// Fix strategy: don't touch twoway_parse.cpp's parenthesised-layer grammar
// at all -- instead, when the raw twoway argument string has NO top-level
// parens but DOES have a top-level `||`, wrap each `||`-delimited segment
// in its own parens (`"scatter y x || lfit y x"` -> `"(scatter y x) (lfit
// y x)"`) and hand the synthesized string to the EXISTING, already-correct
// parenthesised-layer parser unchanged. `||` occurrences inside a quoted
// string (e.g. a `title("a||b")`) or inside a nested `(...)` option value
// are never split, mirroring twoway_parse.cpp's own paren/quote-depth
// scanning convention (scanToplevelParens) so the two stay consistent in
// spirit without sharing code.
std::string expandDoublePipeLayers(const std::string& rawArgs) {
  // serset-twoway HIST-5: the one expander lives in twoway_parse.cpp so
  // histogram's addplot() reads its `||` chain the same way.
  return twoway::expandDoublePipes(rawArgs);
}

// serset-twoway PARA: the twoway-level options (twoway_options) a plot's own
// option list may carry -- they apply to the whole graph (`plot1, opts || plot2,
// opts name(g)` names the graph: w2al tf_conn_082_gd `tf_b15`, log:1374-1414).
const std::unordered_set<std::string> kTwowayLevelOptions = {
    "name",      "saving",     "nodraw",    "draw",      "title",     "subtitle",  "note",
    "caption",   "xtitle",     "ytitle",    "ttitle",    "xlabel",    "ylabel",    "tlabel",
    "xmlabel",   "ymlabel",    "tmlabel",   "xtick",     "ytick",     "ttick",     "xmtick",
    "ymtick",    "tmtick",     "xscale",    "yscale",    "tscale",    "xline",     "yline",
    "tline",     "legend",     "scheme",    "graphregion", "plotregion", "xsize", "ysize",
    "fxsize",    "fysize",     "aspectratio", "scale",   "text",      "ttext",     "by",
    "iscale",    "xcommon",    "ycommon",   "b1title",   "b2title",   "l1title",   "l2title",
    "r1title",   "r2title",    "t1title",   "t2title",   "xoverhangs", "yoverhangs", "play"};

// Moves the twoway-level options out of each layer's own list into the graph's
// (layer order; the graph's own block after them).
void hoistTwowayLevelOptions(twoway::ParseResult& pr) {
  std::vector<std::pair<std::string, eval::OptValue>> hoisted;
  for (auto& l : pr.layers) {
    std::vector<std::pair<std::string, eval::OptValue>> keep;
    for (auto& kv : l.opts) {
      if (kTwowayLevelOptions.count(kv.first) != 0) {
        hoisted.push_back(kv);
      } else {
        keep.push_back(kv);
      }
    }
    l.opts = std::move(keep);
  }
  if (hoisted.empty()) return;
  for (auto& kv : pr.globalOpts) hoisted.push_back(kv);
  pr.globalOpts = std::move(hoisted);
}

// BUG-176 (P2-PREP): the `tempfile`/`tempname`/`tempvar` intercept body,
// lifted verbatim out of dispatchImpl's step (c)/(d) so the SAME matcher can
// run twice — once on the raw line (the literal path, unchanged) and once on
// the macro-EXPANDED line at step 0b-bis. Returns true when it consumed the
// line, writing the reply into `out`; false leaves `out` untouched and the
// caller falls through exactly as before.
//
// Duplicating the body instead would have been the third instance of this
// codebase's recurring one-sided-edit failure (BUG-158/BUG-161): two copies
// of one matcher drift, and the macro-indirected path silently stops
// agreeing with the literal one. One body, two call sites.
bool tryTempDeclIntercept(const std::string& line, SessionState& session,
                          const nlohmann::ordered_json& echo, nlohmann::ordered_json& out) {
  std::string u4Stripped = trimWs(line);
  size_t u4Sp = u4Stripped.find_first_of(" \t");
  std::string u4First = lower(u4Sp == std::string::npos ? u4Stripped : u4Stripped.substr(0, u4Sp));
  if (u4First != "tempfile" && u4First != "tempname" && u4First != "tempvar") return false;
  std::vector<std::string> names;
  if (u4Sp != std::string::npos) {
    std::istringstream namesIss(u4Stripped.substr(u4Sp));
    std::string nm;
    while (namesIss >> nm) names.push_back(nm);
  }
  nlohmann::ordered_json r = (u4First == "tempfile")   ? handlers::runTempfile(names, session)
                             : (u4First == "tempname") ? handlers::runTempname(names, session)
                                                       : handlers::runTempvar(names, session);
  if (r.is_array() && r.empty()) {
    out = nlohmann::ordered_json::array();
    return true;
  }
  out = nlohmann::ordered_json::array({echo, r});
  return true;
}

// BUG-513: `local NAME : display <display-args>` -- the extended macro function
// that stores what `display` would have printed (`local m : display _N` -> 12,
// tests/eval/golden/aer_w2/w2_smalls.log arm ext_display_N). It was the generic
// BUG-216 refusal, "display not allowed" r(101): honest, but it is how a do-file
// formats a number into a macro (`local b : display %5.3f _b[x]`), so every such
// line stopped its file.
//
// Answered HERE and not in eval::tryExtendedMacroFn because the answer IS
// handleDisplay's -- token resolution, %fmt, _b[]/e()/c()/scalar reads -- and
// that needs the full SessionState the eval layer deliberately does not see
// (same reason substituteCRefs runs at this call site). A second, thinner
// display inside the eval layer is the two-layer drift this file keeps paying
// for. One body, two call sites, like tryTempDeclIntercept above.
//
// The line is macro-expanded first (step 0a sees the RAW line), exactly what
// step 0b would have done to an ordinary `display` line. A display ERROR is
// returned as the block it is, reason and all, and the macro stays UNSET --
// BUG-216/217's rule for a failed colon form.
//
// The abbreviations are the display command's own (`di` .. `display`); only
// the full word is in the oracle.
bool tryExtDisplayCapture(const std::string& lineIn, SessionState& session,
                          nlohmann::ordered_json& out) {
  if (lineIn.find(':') == std::string::npos) return false;  // memchr-class gate
  {
    // Second gate, still no regex: only a `local`/`global` line can be this
    // form, and `by g:` / `bysort g:` prefixes are the colon lines that matter.
    const size_t h = lineIn.find_first_not_of(" \t");
    if (h == std::string::npos) return false;
    const std::string head = lower(lineIn.substr(h, 6));
    if (head.compare(0, 5, "local") != 0 && head != "global") return false;
  }
  static const std::regex reLoose(
      R"(^\s*(?:local|global)\s+\S+?\s*:\s*(?:display|displa|displ|disp|dis|di)(?![A-Za-z0-9_]|[^\x00-\x7F]))",
      std::regex::icase);
  if (!std::regex_search(lineIn, reLoose)) return false;
  std::string line = lineIn;
  if (eval::hasMacroRefs(line)) {
    eval::ExpandResult er = eval::expandMacros(line, session.eval);
    if (er.isError) {
      out = nlohmann::ordered_json::array({er.error});
      return true;
    }
    line = er.line;
  }
  // The macro name is eval::kRxName (a Unicode name is one name; StataBE takes
  // `local قيم : display 1` and even `local ١ : display 1`, rc 0 both,
  // scratch-r150/uni-d/badnames.log).
  static const std::regex reStrict(
      std::string(R"(^\s*(local|global)\s+()") + eval::kRxName +
          R"()\s*:\s*(?:display|displa|displ|disp|dis|di)(?![A-Za-z0-9_]|[^\x00-\x7F])(.*)$)",
      std::regex::icase);
  std::smatch m;
  if (!std::regex_match(line, m, reStrict)) return false;  // not this form after all
  // reStrict has THREE capture groups (kind, name, display args); the display
  // arguments are group 3. Group 4 does not exist and std::smatch answers ""
  // for it, which is how the first build stored an EMPTY macro at rc 0.
  nlohmann::ordered_json shown = handleDisplay(trimWs(m.str(3)), session);
  if (!shown.is_object() || shown.value("type", std::string()) != "display" ||
      !shown.contains("text") || !shown.at("text").is_string()) {
    if (shown.is_object()) shown.erase("partial_text");
    out = nlohmann::ordered_json::array({shown});
    return true;
  }
  const std::string text = shown.at("text").get<std::string>();
  if (lower(m.str(1)) == "local") {
    session.eval.locals.set(m.str(2), text);
  } else {
    session.eval.globals.set(m.str(2), text);
  }
  out = nlohmann::ordered_json::array();
  return true;
}

// The INLINE form of BUG-513's function: `` `: display <args>' `` anywhere in a
// line, `local m = strltrim("`: display %9.3f 0.25'")` (w2x_fn_string_math
// fx_strltrim_macro: "0.250"). Each span is replaced by what handleDisplay prints
// for its arguments, field padding included, innermost (rightmost) first, after
// the span's own macros are expanded. A display error is returned as its block.
// Every other inline `: fn' is left for the macro layer (eval/macros.cpp).
std::string substituteInlineDisplay(const std::string& lineIn, SessionState& session,
                                    std::optional<nlohmann::ordered_json>& err) {
  std::string line = lineIn;
  static const std::regex reHead(R"(^\s*(?:display|displa|displ|disp|dis|di)(?:\s+|$))",
                                 std::regex::icase);
  size_t searchEnd = line.size();
  for (int guard = 0; guard < 64; ++guard) {
    const size_t p = searchEnd == 0 ? std::string::npos : line.rfind("`:", searchEnd - 1);
    if (p == std::string::npos) break;
    size_t end = std::string::npos;
    int depth = 0;
    for (size_t i = p; i < line.size(); ++i) {
      if (line[i] == '`') ++depth;
      else if (line[i] == '\'' && --depth == 0) { end = i; break; }
    }
    std::smatch hm;
    const std::string body = end == std::string::npos ? std::string() : line.substr(p + 2, end - p - 2);
    if (end == std::string::npos || !std::regex_search(body, hm, reHead)) {
      searchEnd = p;
      continue;
    }
    std::string args = body.substr(static_cast<size_t>(hm.length(0)));
    if (eval::hasMacroRefs(args)) {
      eval::ExpandResult er = eval::expandMacros(args, session.eval);
      if (er.isError) {
        err = er.error;
        return lineIn;
      }
      args = er.line;
    }
    nlohmann::ordered_json shown = handleDisplay(trimWs(args), session);
    if (!shown.is_object() || shown.value("type", std::string()) != "display" ||
        !shown.contains("text") || !shown.at("text").is_string()) {
      // handleDisplay's internal partial_text (the pieces printed before the
      // failing one) is for a display statement's own caller; the macro the
      // inline display was to expand into shows nothing, as the `local q : display`
      // path above and the failed-display echo below erase it too.
      if (shown.is_object()) shown.erase("partial_text");
      err = shown;
      return lineIn;
    }
    line.replace(p, end - p + 1, shown.at("text").get<std::string>());
    searchEnd = p;
  }
  return line;
}

// The text a failed-expansion display line prints before its first piece
// whose macros do not expand (displayPartialBlock): the pieces are the segments
// handleDisplay itself evaluates one by one (a quoted string, a %fmt with the
// expression after it, a run of unquoted tokens). Prefixes are only EXPANDED
// to find the failing piece; the longest that expands is displayed once.
std::optional<std::string> displayExpandablePrefix(const std::string& args, SessionState& session) {
  if (args.find("`++") != std::string::npos || args.find("`--") != std::string::npos ||
      args.find("++'") != std::string::npos || args.find("--'") != std::string::npos ||
      args.find("_request") != std::string::npos || args.find("`\"") != std::string::npos ||
      args.find("\"'") != std::string::npos || args.find("`=") != std::string::npos)
    return std::nullopt;
  static const std::regex reAsMod(
      R"(^\s*as\s+(?:text|txt|result|res|error|err)(?:_(?:continue|n))?\s+)", std::regex::icase);
  size_t start = 0;
  {
    std::smatch m0;
    if (std::regex_search(args, m0, reAsMod) && m0.position(0) == 0)
      start = static_cast<size_t>(m0.length(0));
  }
  // Piece ends (offsets into args), cut where handleDisplay flushes its segments.
  std::vector<size_t> ends;
  bool fmtPending = false, inRun = false;
  size_t i = start;
  while (i < args.size()) {
    const char c = args[i];
    if (std::isspace(static_cast<unsigned char>(c))) {
      ++i;
      continue;
    }
    if (c == '"') {
      if (inRun) {
        ends.push_back(i);
        inRun = false;
      }
      if (fmtPending) return std::nullopt;  // a format before a string literal
      const size_t close = args.find('"', i + 1);
      if (close == std::string::npos) return std::nullopt;
      i = close + 1;
      ends.push_back(i);
      continue;
    }
    size_t j = i;
    int depth = 0;
    while (j < args.size()) {
      const char d = args[j];
      if (d == '(') {
        ++depth;
      } else if (d == ')') {
        if (depth > 0) --depth;
      } else if (d == '"') {
        if (depth == 0) break;
        const size_t close = args.find('"', j + 1);
        if (close == std::string::npos) return std::nullopt;
        j = close;
      } else if (std::isspace(static_cast<unsigned char>(d)) && depth == 0) {
        break;
      }
      ++j;
    }
    if (depth != 0) return std::nullopt;
    const std::string tok = args.substr(i, j - i);
    static const std::regex reDirective(R"(^_(n|newline|continue|asis|skip|col|column|dup|char)\b.*)");
    if (tok == "as" || (tok[0] == '_' && std::regex_match(tok, reDirective))) return std::nullopt;
    const bool isFmt = tok.size() >= 2 && tok[0] == '%' &&
                       (std::isdigit(static_cast<unsigned char>(tok[1])) || tok[1] == 't' ||
                        tok[1] == '-');
    if (isFmt) {
      if (inRun) {
        ends.push_back(i);
        inRun = false;
      }
      if (fmtPending) return std::nullopt;
      fmtPending = true;
    } else {
      inRun = true;
      fmtPending = false;
    }
    i = j;
  }
  if (inRun) ends.push_back(args.size());
  if (ends.size() < 2) return std::nullopt;
  for (size_t k = ends.size() - 1; k-- > 0;) {
    std::string line = "display " + args.substr(start, ends[k] - start);
    if (eval::hasMacroRefs(line)) {
      eval::ExpandResult er = eval::expandMacros(line, session.eval);
      if (er.isError) continue;
      line = er.line;
    }
    const nlohmann::ordered_json shown = handleDisplay(rawArgsAfterCmd(line), session);
    if (!shown.is_object() || shown.value("type", std::string()) != "display" ||
        !shown.contains("text") || !shown.at("text").is_string())
      return std::nullopt;
    return shown.at("text").get<std::string>();
  }
  return std::nullopt;
}

// A `display` that fails part-way. Stata writes each piece as it evaluates it,
// so the pieces before the failing one stay on the line and the error's message
// follows them on the same line (StataBE 19.5, w2v_margins_corpus:
// `PREC|F7_dydx_minmax|121|N=.|k=type mismatch` for a colsof(r(b)) with r(b)
// absent; `PREC|F7nb_dcat_dydx_post_bname1|0|1.tt#0.dcat|[1.tt#0.dcat] not found`
// for an unknown _b[] name). PIET answered the error block alone, so the line
// was lost. `err` is the error block the line produced; the result is the
// display block to show BEFORE it, or nothing. No piece is ever evaluated twice
// (a runiform() draw or an inline `=exp' would run again):
//  - handleDisplay failed (`evaluated`): it stopped at the failing piece and
//    left the text printed before it on the block as `partial_text`.
//  - macro expansion failed (PIET resolves r() there; Stata reads it in
//    display's own evaluation): display never ran. The longest piece prefix
//    that expands is displayed once. Only lines whose expansion has no side
//    effect are taken (no `++`/`--` macro, no inline `=exp', no _request(), no
//    display directive or mid-line `as`), and a prefix that itself fails gives
//    nothing (the error block already carries the message).
// The message is Stata's sentence: PIET's text up to its first ": " detail.
std::optional<nlohmann::ordered_json> displayPartialBlock(const std::string& args,
                                                          const nlohmann::ordered_json& err,
                                                          SessionState& session, bool evaluated) {
  if (!err.is_object() || err.value("type", std::string()) != "error" || !err.contains("message") ||
      !err.at("message").is_string())
    return std::nullopt;
  std::optional<std::string> printed;
  if (evaluated) {
    if (err.contains("partial_text") && err.at("partial_text").is_string())
      printed = err.at("partial_text").get<std::string>();
  } else {
    printed = displayExpandablePrefix(args, session);
  }
  if (!printed.has_value() || printed->empty()) return std::nullopt;
  std::string msg = err.at("message").get<std::string>();
  if (msg.rfind("r(", 0) == 0) {
    const size_t close = msg.find("): ");
    if (close != std::string::npos) msg = msg.substr(close + 3);
  }
  const size_t detail = msg.find(": ");
  if (detail != std::string::npos) msg = msg.substr(0, detail);
  nlohmann::ordered_json out;
  out["type"] = "display";
  out["text"] = *printed + msg;
  return out;
}

bool isDisplayLine(const std::string& raw) {
  static const std::regex reDisplay(R"(^\s*(?:display|displa|displ|disp|dis|di)(?![A-Za-z0-9_]|[^\x00-\x7F]))");
  return std::regex_search(raw, reDisplay);
}

// FU-602: `c_local NAME [= exp | : fn | text]` sets NAME in the CALLER's local
// scope ([P] macro: "c_local ... sets a local macro in the caller's space";
// oracle w2_esample_copies.do:39-40, the helper program pcnt returns its count
// through it). The definition is evaluated exactly as `local` would evaluate it
// in the CURRENT scope -- the same tryCaptureDefinition call, so `_rc`, r() and
// the macros on the right-hand side read what this program sees -- and the
// value is then moved to the caller's frame (session.scopeStack.back(), the
// frame program.cpp's ScopeGuard and doexec's PrivateLocalScope push), leaving
// this scope's own NAME as it was. With no caller frame (the top level of the
// session) there is no caller to write to: a named r(198). Returns false when
// the line is not a c_local line.
bool tryCLocal(const std::string& rawLine, SessionState& session, nlohmann::ordered_json& out) {
  const std::string t = strutil::trim(rawLine);
  if (t.size() < 7 || lower(t.substr(0, 7)) != "c_local") return false;
  if (t.size() > 7 && std::isspace(static_cast<unsigned char>(t[7])) == 0) return false;
  auto refuse = [&](const std::string& msg) {
    out = nlohmann::ordered_json::array({ojson(eval::errorBlock(
        "syntax_error", msg, std::string("c_local"), std::nullopt, t, "r(198)"))});
    return true;
  };
  if (session.scopeStack.empty()) {
    return refuse("c_local sets a local in the caller's scope; it is valid only inside a "
                  "program or a do-file run by another one");
  }
  std::string rest = strutil::trim(t.substr(7));
  auto identLen = [](const std::string& r) {  // bytes; a Unicode name stays whole
    size_t j = 0;
    if (j < r.size() && cecon::ident::isIdentStartByte(r[j])) {
      while (j < r.size() && cecon::ident::isIdentByte(r[j])) ++j;
    }
    return j;
  };
  // The name may arrive through a macro (`c_local `name' = ...`): expand the
  // line in this scope first, as step 0b-bis does for a macro-named `local`.
  if (identLen(rest) == 0 && eval::hasMacroRefs(rest)) {
    eval::ExpandResult er = eval::expandMacros(rest, session.eval);
    if (er.isError) {
      out = nlohmann::ordered_json::array({er.error});
      return true;
    }
    rest = strutil::trim(er.line);
  }
  const size_t nl = identLen(rest);
  if (nl == 0 || cecon::ident::codePointCount(std::string_view(rest).substr(0, nl)) > 31 ||
      (nl < rest.size() && std::isspace(static_cast<unsigned char>(rest[nl])) == 0 &&
       rest[nl] != '=' && rest[nl] != ':')) {
    return refuse("c_local: " + (rest.empty() ? std::string("a macro name is required")
                                              : "'" + rest.substr(0, rest.find(' ')) +
                                                    "' is not a valid macro name"));
  }
  const std::string name = rest.substr(0, nl);
  std::optional<std::string> prev;
  if (const std::string* p = session.eval.locals.find(name)) prev = *p;
  // NAME is not cleared first: the right-hand side may read it (`c_local x `x'`).
  const std::string defLine = "local " + name + (nl < rest.size() ? " " + rest.substr(nl) : "");
  eval::MacroFnError mfe;
  // `c_local NAME : display ...` is the `local` form's own display (BUG-513);
  // StataBE 19.5: `c_local c2 : display %5.2f 3.14159` gives the caller " 3.14".
  nlohmann::ordered_json shown;
  const bool viaDisplay = tryExtDisplayCapture(defLine, session, shown);
  const bool consumed =
      viaDisplay ||
      eval::tryCaptureDefinition(substituteCRefs(defLine, session), session.eval, &mfe);
  std::optional<std::string> val;
  if (const std::string* v = session.eval.locals.find(name)) val = *v;
  if (prev.has_value()) session.eval.locals.set(name, *prev);
  else session.eval.locals.erase(name);
  if (viaDisplay && !shown.empty()) {  // display's own error; the caller's NAME is untouched
    out = shown;
    return true;
  }
  if (!consumed) return refuse("c_local: cannot read the definition of " + name);
  if (mfe.failed()) {
    out = nlohmann::ordered_json::array({ojson(eval::errorBlock(
        "engine_error", mfe.message, std::nullopt, std::nullopt, t, "r(" + mfe.rc + ")"))});
    return true;
  }
  if (val.has_value()) session.scopeStack.back().set(name, *val);
  else session.scopeStack.back().erase(name);
  out = nlohmann::ordered_json::array();
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// AER scope-2 A1 — THE option-name resolution rule, as one callable function.
//
// This body is dispatchImpl's own step 1a-ter, moved out unchanged so the rule
// has exactly one transcription. dispatchImpl calls it per parsed option key;
// `piet_native --dump-optvocab --resolve` calls the same function, which is the
// entire reason that flag exists — a reporting tool carrying its own copy of
// the rule would answer for a dispatcher that does not exist.
//
// `stage` names the LAST pass that actually rewrote the key. It is not a finer
// breakdown than that on purpose: canonicalizeForCommand's internal
// alias/known-prefix/unimplemented-prefix order is its own, and re-deriving it
// out here would be the second transcription this function exists to prevent.
OptKeyResolution resolveOptionKeyForCommand(const std::string& cmd, const std::string& key) {
  // DELIBERATELY a declared table, not a call to resolveOptionAbbrev over a
  // guessed vocabulary: an over-accepted option spelling is the same fabricated
  // parity the command-name arc rejected. Only rows the oracle measured appear.
  struct OptMinimum {
    const char* full;
    size_t minLen;  // Stata's documented minimum abbreviation length
  };
  // level: estimation_options.sthlp `{opt l:evel(#)}` + live `regress price mpg, l(90)` rc=0.
  // level: estimation_options.sthlp `{opt l:evel(#)}` + live `regress price mpg, l(90)` rc=0.
#include "generated/opt_minimums_bug264.inc"  // NOLINT — table body, see gen_table.py
  const auto& kOptMinimums = kOptMinimumsGenerated;

  // Generated rows a later oracle overrules (the table must not be hand-edited).
  // egen MISSing: its measured minimum is one function's (_gtotal `Missing`, 1),
  // while every egen function declares its own and runEgen resolves each key
  // against the function's own minimums (egenFnVocab). Applied to all of them it
  // made `mode(k), m` the missing variant (StataBE 19.5: `option m not allowed`
  // r(198), _gmode `MISSing`) and `std(x), m(5)` a refused missing (mean(5));
  // BUG-1111.
  static const std::set<std::pair<std::string, std::string>> kGeneratedOverruled = {
      {"egen", "missing"}};

  OptKeyResolution res;
  res.resolved = key;
  auto mins = kOptMinimums.find(cmd);
  if (mins != kOptMinimums.end()) {
    for (const auto& om : mins->second) {
      const std::string full(om.full);
      if (kGeneratedOverruled.count({cmd, full}) != 0) continue;
      if (res.resolved.size() >= om.minLen && res.resolved.size() < full.size() &&
          full.compare(0, res.resolved.size(), res.resolved) == 0) {
        res.resolved = full;
        res.stage = "minimums";
        break;
      }
    }
  }
  // Minimums measured by a later oracle than the generated BUG-264 table (which
  // must not be hand-edited). Each row is a measured accept AND its measured
  // refusal one character shorter:
  //   append GENerate: `gen(src)` rc 0 (aer_w2 w2_data_io.log app9, w2b_dataio
  //   append_gen(src)); `ge(src)` and `g(src)` r(198) (w2b_dataio append_ge,
  //   append_g).
  //   order ALPHAbetic: `order *, alpha` rc 0 (aer_w2 w2b_dataio order_alpha,
  //   w2b_dataio.log:809-810); the minimum is order.ado's syntax line.
  //   fcollapse (ftools 2.49.0) FREQvar REGister Verbose: StataBE 19.5 runs
  //   `freqv(x)`, `reg(x)` (then Mata's r(3499)), `v` and `ve`; `re(x)` is
  //   `option re() not allowed` r(198), and `fre` is r(198) beside FREQ.
  static const std::map<std::string, std::vector<OptMinimum>> kOptMinimumsLater = {
      {"append", {{"generate", 3}}},
      {"order", {{"alphabetic", 5}}},
      {"fcollapse", {{"freqvar", 5}, {"register", 3}, {"verbose", 1}}},
  };
  // Commands whose whole vocabulary a later oracle adjudicated: kOptMinComplete's
  // statement for the rows above. collapse_17's BY CW FAST CLABEL frame() and
  // every other fcollapse option take no abbreviation (BUG-1100, StataBE 19.5:
  // `cla`, `clab`, `clabe`, `fra()`, `fram()`, `fas`, `c` and `b()` on collapse,
  // `app`, `appen`, `sm`, `smar`, `noc`, `nocomp`, `nocompres`, `mer`, `poo()`,
  // `meth()` and `metho()` on fcollapse are `option ... not allowed` r(198)).
  static const std::set<std::string> kOptMinCompleteLater = {"collapse", "fcollapse"};
  auto later = kOptMinimumsLater.find(cmd);
  if (std::string(res.stage) == "unchanged" && later != kOptMinimumsLater.end()) {
    for (const auto& om : later->second) {
      const std::string full(om.full);
      if (res.resolved.size() >= om.minLen && res.resolved.size() < full.size() &&
          full.compare(0, res.resolved.size(), res.resolved) == 0) {
        res.resolved = full;
        res.stage = "minimums";
        break;
      }
    }
  }

  // -------------------------------------------------------------------------
  // AER scope-2 B4 — THE ABBREVIATION FLOOR: the NARROWING companion to the
  // widening table above, and the fix for a destructive regression.
  //
  // WHY IT EXISTS. eval::resolveOptionAbbrev resolves any >=2-char UNIQUE
  // prefix of a net's vocabulary. That rule is a GUESS about Stata, and on
  // 2026-09-09 the guess was measured wrong in the worst possible direction:
  // the W2 vocabulary sweep (e1faab17) put `replace` into the export/save/graph
  // nets, `re` thereby became a unique prefix of it, and
  //   export delimited using "f.csv", re
  // — which real StataBE answers r(198), "option re not allowed" — started
  // OVERWRITING the file. `graph export "f.png", as(png) re` likewise turned
  // the r(602) "file already exists" guard into a silent success. A guess may
  // never be the reason PIET destroys a file or changes an answer.
  //
  // THE RULE. Each row is one MEASURED StataBE refusal: option `full` on
  // command `cmd` accepts no abbreviation shorter than `floorLen`. A key that
  // is a PROPER prefix of `full` and shorter than `floorLen` is left exactly as
  // the user typed it and marked exactOnly, so the net answers r(198) naming
  // that spelling — which is what Stata prints. floorLen == strlen(full) means
  // NO ABBREVIATION AT ALL.
  //
  // WHY A FLOOR AND NOT kOptMinComplete. Joining kOptMinComplete asserts that a
  // command's WHOLE vocabulary has been adjudicated. oracle_p probed a handful
  // of OPTIONS, not whole vocabularies, and claiming a completeness we do not
  // have would start rejecting spellings Stata accepts — `regress ..., noc` is
  // rc 0 in Stata (oracle_p.log:140-156) and has to stay rc 0. A floor row is
  // the narrow statement the capture actually makes, about one option, and
  // nothing else moves.
  //
  // ONE-SIDED, in the safe direction, which is the whole safety argument: a
  // floor can only turn an accept into a reject, and only for a (command,
  // option) pair a live capture measured Stata rejecting. The FULL spelling is
  // never touched — only proper prefixes are tested — and neither is any key
  // that is an exact member of the command's own vocabulary (`import
  // delimited, delim(",")` keeps working even though `delim` is a proper prefix
  // of `delimiter`), which is the same "an exact spelling is never an
  // abbreviation" principle eval::canonicalizeForCommand's step 1 already
  // states.
  struct OptFloor {
    const char* cmd;
    const char* full;
    size_t floorLen;  // shortest ACCEPTED abbreviation; == strlen(full) => none
  };
  static const OptFloor kOptFloors[] = {
      // P0-A, the destructive row. pins/oracle_p.log:23-27 `export
      // delimited using "p_a.csv", re` -> r(198) "option re not allowed";
      // :29-33 `, rep` -> r(198); :16-21 the full `, replace` -> rc 0. So
      // `replace` on export takes NO abbreviation.
      {"export", "replace", 7},
      // oracle_p.log:61-65 `save "p_c.dta", re` -> r(198); :54-59 full
      // `replace` -> rc 0. Same word, same verdict, measured separately.
      {"save", "replace", 7},
      // W1a, 2026-09-10. `tabulate ..., no` ran a NOFREQ tabulation here and is
      // r(198) in Stata. The prefix looked unambiguous only because `nolabel`,
      // `nokey` and `nolog` are ABSENT from kVocabTabulate -- a MISSING
      // vocabulary entry is what made an over-acceptance look unique, which is
      // the same shape as the `re` -> `replace` row above with the cause on the
      // other side. Measured, tests/eval/golden/aer_w1/w1a_optmin_pins3.log:
      // `n` r(198), `no` r(198), `nof` rc 0 -- so the floor is 3. The same log
      // records that `nolabel` and `nokey` are rc 0 in Stata (W1a-1 put them in
      // the net, 2026-09-14) while `nolog` is r(198) (it does not). Filed BUG-485.
      {"tabulate", "nofreq", 3},
      // `graph export`/`graph save` share the identical `replace` word. NOT
      // separately captured in oracle_p — the row is carried from the two
      // measured `replace` verdicts above because it is the same option word on
      // the same file-writing duty, and because the direction is REFUSAL, which
      // cannot invent a Stata behaviour. Measured HERE before the fix: with a
      // file already present, `graph export "g.png", as(png) re` returned rc 0
      // and defeated the r(602) file-exists guard.
      {"graph", "replace", 7},

      // ── P0-B, the measured half. oracle_p.log:41-45 `export delimited ...,
      // nol` -> r(198); :47-51 `, nov` -> r(198). Neither is ambiguous, so the
      // ambiguity repair alone would not reach them: export's option names
      // simply take no abbreviation. Between them they also settle `, no`
      // (:35-39, r(198)) from a second direction.
      // w2ar (2026-10-21) measured the accepting side: `nolab` rc 0
      // (dx_exd_nolab, log:659) and `novar` rc 0 (dx_exd_novar, log:515), the
      // NOLABel / NOVARnames minimums of export delimited's syntax line.
      // `nol`/`nov` stay r(198) (oracle_p above). DATAFmt's minimum is `dataf`
      // and KEEPCELLFmt's `keepcellf` (export delimited / export excel).
      {"export", "nolabel", 5},
      {"export", "novarnames", 5},
      {"export", "datafmt", 5},
      {"export", "keepcellfmt", 9},

      // ── P1, MEASURED MINIMUMS. Each pair is a refusal AND the accepting
      // spelling one character longer, which is what makes it a floor rather
      // than a guess at one.
      // oracle_p.log:68-72 `list make in 1/2, no` -> r(198); :74-84 `, noo` ->
      // rc 0. So noobs' minimum is 3.
      {"list", "noobs", 3},
      // oracle_p.log:159-163 `summarize price mpg, se(2)` -> r(198); :165-173
      // `, sep(2)` -> rc 0. separator's minimum is 3, and PIET's generic 2-char
      // floor sat one character below it.
      {"summarize", "separator", 3},
      // oracle_p.log:176-180 `estimates table m1, st(N)` -> r(198); :182-186
      // `, sta(N)` -> r(198) TOO. Both refused, so the floor is at least 4;
      // `stat` was not probed and a floor of 4 does not accept it — it only
      // declines to block it, leaving today's behaviour untouched. Landing 5
      // would be claiming a measurement nobody took.
      {"estimates", "stats", 4},
      // oracle_p.log:109-113 `regress price mpg, vc(robust)` -> r(198);
      // :121-138 `, vce(robust)` -> rc 0. vce takes NO abbreviation.
      {"regress", "vce", 3},
      // StataBE 19.5, 2026-10-29: `replace e = 3.7, nop` rc 0 (byte e stores 3),
      // `, no` and `, n` r(198) "option no not allowed", plain and by-prefixed
      // alike. NOPromote's minimum is 3.
      {"replace", "nopromote", 3},
      // BUG-1163 review: tsset.ado's display options take no abbreviation --
      // `xtset id t, nob` is r(198) "option nob not allowed", `, displayi(2)`
      // "option displayi() not allowed", and PanelSet answers `xtset id, nob`
      // and `, panel(G)` with its r(101) (StataBE 19.5).
      {"xtset", "noblank", 7},
      {"xtset", "panelname", 9},
      {"xtset", "displayindent", 13},
      {"tsset", "noblank", 7},
      {"tsset", "panelname", 9},
      {"tsset", "displayindent", 13},
      {"tset", "noblank", 7},
      {"tset", "panelname", 9},
      {"tset", "displayindent", 13},
      // BUG-1096, each a refusal and the accepted spelling one longer (StataBE
      // 19.5): `decode e, gen(d) max(3)` r(198), `maxl(3)` rc 0; `encode s, ...
      // noex` r(198), `noext` rc 0.
      {"decode", "maxlength", 4},
      {"encode", "noextend", 5},
      // BUG-1141: [D] recode's pre:fix() and copy:rest minimums (`pr()`, `co`,
      // `cop` r(198)).
      {"recode", "prefix", 3},
      {"recode", "copyrest", 4},

      // ── P1, INTERNAL SYNONYMS — a different kind of row, and the reason it is
      // honest. A PIET `known` set is a statement about what the HANDLER READS,
      // not about Stata's vocabulary: it legitimately carries aliases this build
      // uses internally. `vcov` is one — real Stata's regress has no such option
      // at all, which is why StataBE answers `vco(robust)` with r(198), "option
      // vco() not allowed" (oracle_p.log:115-119). An internal alias exists so a
      // handler can read a key; it must not also widen what PIET accepts. So it
      // keeps its exact spelling and contributes NO abbreviation surface.
      {"regress", "vcov", 4},
      // BUG-1500: ivprobit's TWOstep. StataBE 19.5 `ivprobit yb2 x1 (xe = z2), two`
      // rc 0, `, tw` r(198) "option tw not allowed" (scratch-laneA2 feat/i1.do).
      {"ivprobit", "twostep", 3},

      // gtools' gegen: its own total(), sum() and group() take `missing` only
      // in full (StataBE 19.5 with gtools: `gegen q = total(a), mi` and
      // `group(a), mi` are `option mi not allowed` r(198)), while the functions
      // it hands to egen read their ado's minimum (`rowtotal(a x), mi` rc 0).
      // So the key reaches runEgen as typed and is resolved per function there,
      // as egen's is (BUG-1111).
      {"gegen", "missing", 7},
  };
  {
    // An exact member of the command's own vocabulary is a spelling, not an
    // abbreviation, and is never floored (see the `delim` case above).
    const eval::OptVocab* fv = eval::findOptVocab(cmd);
    const bool exactSpelling =
        fv != nullptr &&
        (fv->known.count(res.resolved) != 0 || fv->unimplemented.count(res.resolved) != 0);
    if (!exactSpelling) {
      for (const auto& of : kOptFloors) {
        if (cmd != of.cmd) continue;
        const std::string full(of.full);
        if (res.resolved.size() < full.size() && res.resolved.size() < of.floorLen &&
            full.compare(0, res.resolved.size(), res.resolved) == 0) {
          // Below the measured floor: do NOT resolve, and tell the net not to
          // resolve either (OptValue::exactOnly reaches
          // eval::rejectUnknownOptions, which otherwise prefix-resolves the key
          // itself — BUG-335's decision point). Hand-rolled nets that never
          // call rejectUnknownOptions are covered by the same act of leaving
          // the key raw: they compare against the full spelling and reject.
          res.exactOnly = true;
          res.stage = "floor";
          return res;
        }
      }
    }
  }

  // BUG-233 / FIX:OPTNET-ABBREV-SWEEP — the GENERAL half of the same point.
  //
  // The table above is the WIDENING half: nine oracle-measured minimums,
  // including the two 1-char ones (`l`->level, `r`->robust) that are below
  // resolveOptionAbbrev's deliberate 2-char floor. It can only be a declared
  // table, because widening acceptance without an oracle row is fabricated
  // parity.
  //
  // This call is the DROP half, and it needs no oracle at all, because it
  // widens nothing: eval::rejectUnknownOptions ALREADY accepts every >=2-char
  // unique prefix of every net's `known` set. canonicalizeForCommand applies
  // that same rule against that same vocabulary, one step earlier, so the
  // accepted set is bit-identical and the only change is that the handler now
  // SEES the option it already agreed to accept. Measured accept-then-drop
  // instances this closes: winsor2 `cut(5 95)` (silently reverted to the 1/99
  // default), ttest `uneq` (pooled SE instead of Welch), esttab `nosta`
  // (stars printed anyway), levelsof `loc(m)` (the local never created).
  //
  // Runs AFTER the table so the 1-char rows still win; a no-op on any key the
  // table already canonicalized.
  // BUG-264/BUG-335 — THE NARROWING FLIP. The first attempt skipped only
  // canonicalizeForCommand and measured 13/18 on controlled boundary checks,
  // with all 3 passes being spellings already rejected for ambiguity: it
  // narrowed nothing, because rejectUnknownOptions resolves prefixes ITSELF.
  // The mark below reaches that decision point (OptValue::exactOnly).
  if (kOptMinComplete.count(cmd) != 0 || kOptMinCompleteLater.count(cmd) != 0) {
    // Whole vocabulary adjudicated: the minimums pass above is a COMPLETE
    // statement of the spellings Stata accepts, so mark the key and let the
    // net answer r(198) for anything else — no unique-prefix fallback.
    res.exactOnly = true;
  } else {
    // canonicalizeForCommand rewrites a whole option vector, and treats each
    // entry independently, so handing it a one-entry vector is the same pass
    // dispatchImpl used to run over all of them at once.
    std::vector<std::pair<std::string, eval::OptValue>> one;
    one.emplace_back(res.resolved, eval::OptValue());
    eval::canonicalizeForCommand(one, cmd);
    if (one.front().first != res.resolved) {
      res.resolved = one.front().first;
      res.stage = "vocab";
    }
  }
  return res;
}

// M2 scope (ROADMAP "M2 — glue + vertical slice") ... M3/M4/BETA extension
// point comment continues to apply unchanged (see this file's top-of-file
// doc comment) — this function is exactly what used to be named
// `dispatch()` before BS-006 (BUG-028's real log write) split it in two.
// The public `dispatch()` (defined at the very bottom of this file, after
// this function's closing brace) is now a thin log-teeing wrapper; this is
// still the ~2470-line M2/M3/M4/BETA routing body, completely unchanged
// otherwise. Every recursive self-call inside this function's own body
// still literally says `dispatch(...)` (not `dispatchImpl(...)`) — quite
// deliberately, so nested calls (capture/quietly/noisily/by/bysort prefix-
// stripping; the eststo/version bare-command forwards; and, via the
// `dispatch` function pointer handed to doexec::executeDoSource/executeDo
// and program::runProgram below, every line of a replayed do-file/loop/
// program body) all flow through the SAME wrapper as any other call —
// see dispatch()'s own doc comment for why that matters.
// ---------------------------------------------------------------------------
// BUG-428 / BUG-432 — THE ROW-PREDICATE SUBSTITUTION POINT.
//
// `local a = M[1,1]` reads a matrix element and `display s` reads a stored
// scalar, because the macro/expression layer substitutes both before evaluating.
// The ROW layer — `assert`, `count if`, `keep if`, the `if` of a `gen`, and
// gen's own expression — never got that pass, so `assert Q[1,1] == 1` came back
// "unknown VARIABLE Q" for a matrix defined on the line above, and
// `gen x = y * scalar(s)` was rc 1 while `display s` printed 3. Two ids, one
// missing pass ([[two-evaluator-layers-predicate-split]]).
//
// Both halves are no-ops on text that names no defined matrix and no defined
// scalar, which keeps the blast radius to expressions that fail today: nothing
// that already works can start substituting.
//
// A VARIABLE always wins over a same-named scalar — Stata's rule, and the reason
// this cannot simply loop `substituteBareName` over the scalar store the way
// evalArith does: evalArith has no frame in hand, and here we do.
// BUG-456 (2026-09-08) — the row plane's CONSTANT STRING COMPARE fold.
//
// The engine's data plane has no string type: its `--keepif` accepts
// `factorvar ==/!= "literal"` and nothing else (src/data/frame.cpp), and
// CiParser bails on any string operand. Stata's row plane takes a constant
// string compare everywhere -- `assert e(cmd)=="regress"` rc 0, `gen z =
// (e(cmd)=="regress")` mean 1, `count if e(cmd)=="nope"` 0 (capture.log
// A1/G1/G10) -- and after the resolver above has turned e(cmd) into `"regress"`
// the claim is a constant. So the constant is decided HERE, before the engine
// sees it: `"lit" op "lit"` folds to 1/0 for ==, !=, ~=, <, <=, >, >= (Stata
// orders strings bytewise, refute3 Q1-Q3), and `"lit" op <number|.>` (either
// order) is r(109) "type mismatch" (A9/A10/D10/N1/M1-M7), never a silent 0.
// Anything with a variable, a function call or a non-constant operand on
// either side is left exactly as it was -- CiParser / the engine own it.
//
// Quote-aware by construction: the operator is found with eval::searchUnquoted
// (tokenizer.h), never a quote-blind regex ([[piet-quote-aware-qualifier]]),
// and each operand is classified from the characters beside the operator.
// NAMED GAP (2026-09-09, review finding 11): the operand scanner below bounds a
// literal with a bare rfind('"')/find('"') and does NOT understand the `\"`
// escaping resolveERef (macros.cpp) and resolveEStringLiteral
// (display_hardening.cpp) emit. Unreachable today -- measured: PIET refuses
// compound double quotes, so no e-string can carry a `"` -- so this is a pin
// owed whenever that gap closes, not a live defect.
// String FUNCTIONS on a constant (`inlist(e(cmd),"a")`, `strpos(e(cmdline),
// "x")`) are NOT folded here; they take the strmutate detour when it can
// evaluate them and are refused honestly by it when it cannot -- named scope,
// not claimed by this pass.
namespace {

bool rowConstNumberToken(const std::string& t) {
  static const std::regex reNum(R"(^([0-9]+\.?[0-9]*|\.[0-9]+|\.)([eE][+-]?[0-9]+)?$)");
  return !t.empty() && std::regex_match(t, reNum);
}

// Operand kinds: 0 = not a constant we fold, 1 = "literal", 2 = number/missing.
struct RowConstOperand {
  int kind = 0;
  size_t begin = 0;  // [begin, end) of the operand text incl. quotes
  size_t end = 0;
  std::string text;  // literal contents (kind 1) or the number token (kind 2)
};

// A byte >= 0x80 is a name byte (core/ident.h): the `2` of `ق2 == "x"` is the
// tail of the string variable ق2, not a number compared with a string (r(109)).
bool rowOperandBoundaryLeft(char c) {  // char just BEFORE a constant operand
  return !(cecon::ident::isIdentByte(c) || c == '"' || c == ')' || c == ']' || c == '.');
}
bool rowOperandBoundaryRight(char c) {  // char just AFTER a constant operand
  return !(cecon::ident::isIdentByte(c) || c == '"' || c == '(' || c == '[' || c == '.');
}

RowConstOperand rowLeftOperand(const std::string& e, size_t opPos) {
  RowConstOperand o;
  size_t l = opPos;
  while (l > 0 && std::isspace(static_cast<unsigned char>(e[l - 1]))) --l;
  if (l == 0) return o;
  if (e[l - 1] == '"') {
    if (l < 2) return o;
    const size_t q = e.rfind('"', l - 2);
    if (q == std::string::npos) return o;
    o.kind = 1;
    o.begin = q;
    o.end = l;
    o.text = e.substr(q + 1, l - 2 - q);
  } else {
    size_t k = l;
    auto isTokChar = [](char c) { return cecon::ident::isIdentByte(c) || c == '.'; };
    while (k > 0 && isTokChar(e[k - 1])) --k;
    // exponent sign: `1e+307` scanned back to `307`; extend over the `e+`.
    if (k >= 2 && (e[k - 1] == '+' || e[k - 1] == '-') && (e[k - 2] == 'e' || e[k - 2] == 'E')) {
      k -= 2;
      while (k > 0 && isTokChar(e[k - 1])) --k;
    }
    const std::string tok = e.substr(k, l - k);
    if (!rowConstNumberToken(tok)) return o;
    o.kind = 2;
    o.begin = k;
    o.end = l;
    o.text = tok;
  }
  if (o.begin > 0 && !rowOperandBoundaryLeft(e[o.begin - 1])) o.kind = 0;
  return o;
}

RowConstOperand rowRightOperand(const std::string& e, size_t opEnd) {
  RowConstOperand o;
  size_t r = opEnd;
  while (r < e.size() && std::isspace(static_cast<unsigned char>(e[r]))) ++r;
  if (r >= e.size()) return o;
  if (e[r] == '"') {
    const size_t q = e.find('"', r + 1);
    if (q == std::string::npos) return o;
    o.kind = 1;
    o.begin = r;
    o.end = q + 1;
    o.text = e.substr(r + 1, q - r - 1);
  } else {
    static const std::regex reNumHead(R"(^([0-9]+\.?[0-9]*|\.[0-9]+|\.)([eE][+-]?[0-9]+)?)");
    std::smatch m;
    const std::string tail = e.substr(r);
    if (!std::regex_search(tail, m, reNumHead)) return o;
    o.kind = 2;
    o.begin = r;
    o.end = r + static_cast<size_t>(m.length(0));
    o.text = m.str(0);
  }
  if (o.end < e.size() && !rowOperandBoundaryRight(e[o.end])) o.kind = 0;
  return o;
}

// Folds in place. Returns false (and fills *mismatch) on a string-vs-number
// compare -- the caller turns that into the r(109) block.
bool foldRowConstStringCompares(std::string& e, std::string* mismatch) {
  if (e.find('"') == std::string::npos) return true;  // nothing constant-string to fold
  static const std::regex reCmp(R"(==|!=|~=|<=|>=|<|>)");
  size_t from = 0;
  while (from < e.size()) {
    std::smatch m;
    const std::string tail = e.substr(from);
    if (!eval::searchUnquoted(tail, reCmp, m)) break;
    const size_t opPos = from + static_cast<size_t>(m.position(0));
    const std::string op = m.str(0);
    const size_t opEnd = opPos + op.size();
    const RowConstOperand L = rowLeftOperand(e, opPos);
    const RowConstOperand R = rowRightOperand(e, opEnd);
    if (L.kind == 0 || R.kind == 0) {  // a variable / call / non-constant: not ours
      from = opEnd;
      continue;
    }
    if (L.kind != R.kind) {
      *mismatch = e.substr(L.begin, R.end - L.begin);
      return false;
    }
    // BUG-1419: an arithmetic operator binds tighter than a comparison, so a
    // literal beside one is only part of its operand: in `s1 + "x" == "-3x"` the
    // left side is s1 + "x", not "x" (StataBE 19.5 counts every row; the fold
    // made it s1 + 0, a type mismatch). The operand's type is still the
    // literal's, so the mismatch above stands (`1 + 2 == "3"` is r(109)).
    const auto isArith = [](char c) { return std::string("+-*/^").find(c) != std::string::npos; };
    size_t lb = L.begin, ra = R.end;
    while (lb > 0 && std::isspace(static_cast<unsigned char>(e[lb - 1]))) --lb;
    while (ra < e.size() && std::isspace(static_cast<unsigned char>(e[ra]))) ++ra;
    const bool partial = (lb > 0 && isArith(e[lb - 1])) || (ra < e.size() && isArith(e[ra]));
    if (L.kind == 2 || partial) {  // number op number, or part of an operand: untouched
      from = opEnd;
      continue;
    }
    // A literal an arithmetic operator binds first is not the compare's whole
    // operand: `s + "c" == "bbc"` compares s + "c", and folding `"c" == "bbc"`
    // left `s + 0`, r(109) where StataBE answers. Its kind still decides a
    // string-vs-number compare above.
    {
      size_t lb = L.begin;
      while (lb > 0 && std::isspace(static_cast<unsigned char>(e[lb - 1]))) --lb;
      size_t ra = R.end;
      while (ra < e.size() && std::isspace(static_cast<unsigned char>(e[ra]))) ++ra;
      static const std::string kArith = "+-*/^";
      if ((lb > 0 && kArith.find(e[lb - 1]) != std::string::npos) ||
          (ra < e.size() && kArith.find(e[ra]) != std::string::npos)) {
        from = opEnd;
        continue;
      }
    }
    const int c = L.text.compare(R.text);
    bool v = false;
    if (op == "==") v = (c == 0);
    else if (op == "!=" || op == "~=") v = (c != 0);
    else if (op == "<") v = (c < 0);
    else if (op == "<=") v = (c <= 0);
    else if (op == ">") v = (c > 0);
    else v = (c >= 0);
    e.replace(L.begin, R.end - L.begin, v ? "1" : "0");
    from = L.begin + 1;
  }
  return true;
}

// BUG-1015/BUG-1459: the version in force is SessionState::callerVersion. It
// is restored when the do-file or program that set it concludes.
struct CallerVersionScope {
  SessionState& s;
  double saved;
  explicit CallerVersionScope(SessionState& ss) : s(ss), saved(ss.callerVersion) {}
  ~CallerVersionScope() { s.callerVersion = saved; }
};

// The name an expression that failed to evaluate is missing, as Stata reports
// it: the first bare name, a name before `(` being a function call (`exit
// 1+nosuch` and `summarize if nosuch==1` are r(111) "nosuch not found").
std::optional<std::string> firstBareName(const std::string& expr) {
  static const std::regex reName(R"(\b([A-Za-z_]\w*)\b(?!\s*\())");
  std::smatch nm;
  if (!std::regex_search(expr, nm, reName)) return std::nullopt;
  return nm.str(1);
}

// BUG-1330: a version number as StataBE 19.5 prints it, in `version` and in its
// r(9) refusal: an integer with one decimal (15.0, 0.0, -1.0), anything else as
// a number without its leading zero (16.25, .5, .99).
std::string versionText(double v) {
  char buf[32];
  if (v == std::floor(v)) {
    std::snprintf(buf, sizeof(buf), "%.1f", v == 0.0 ? 0.0 : v);  // `version -0` is 0.0
    return buf;
  }
  std::string t = eval::fmtNumeric(v);
  if (t.rfind("0.", 0) == 0 || t.rfind("-0.", 0) == 0) t.erase(t.find('0'), 1);
  return t;
}

// BUG-1330: the number of `version #` / `version #:`, or the refusal StataBE 19.5
// gives. The part before the point, sign and leading zeros included, is one or
// two characters with a digit; at most two digits follow the point, and a bare
// trailing point only follows a one-character part. `05`, `+5`, `5.`, `1.55`,
// `01.50` and `+1.55` are numbers; `016`, `100`, `-10`, `+15`, `10.`, `+1.`,
// `1.555`, `19.500`, `.5`, `1e1` and `16.2.1` are r(198) invalid syntax. A
// number outside 1 to 19.5 is r(9): `version 0.5`, `99`, `00` and `-1` alike.
std::optional<ojson> parseVersionNumber(const std::string& tok, double* out) {
  static const std::regex reNum(R"(^(?:[+-]?\d|\d\d)(?:\.\d{1,2})?$|^\d\.$)");
  if (!std::regex_match(tok, reNum))
    return ojson(eval::errorBlock("syntax_error", "invalid syntax", std::nullopt, std::nullopt,
                                  std::nullopt, "r(198)"));
  const double v = std::strtod(tok.c_str(), nullptr);
  if (v < 1.0 || v > 19.5)
    return ojson(eval::errorBlock("engine_error",
                                  "this is version 19.5 of StataNow; it cannot run version " +
                                      versionText(v) + " programs",
                                  std::nullopt, std::nullopt, std::nullopt, "r(9)"));
  *out = v;
  return std::nullopt;
}

// BUG-1330: whether `version #` (or `version #:`) here sets c(userversion) too.
// StataBE 19.5 leaves it in a program body and in a loop body of the level now
// running (`forvalues`/`foreach`/`while`: 12|15 after `version 12` inside one,
// under `version 15`), and sets it at the top level, in an `if` block and in a
// do-file, also one run from a program or loop (CH2|12|12).
bool versionSetsUserVersion(SessionState& session) {
  return session.eval.locals.find(kProgramBodyLocal) == nullptr &&
         loops::loopBodyDepth() <= session.loopBodyFloor;
}

// BUG-1330: the setting a lowercased `set` word names, for those whose c() key
// PIET answers until they are set (cclass.cpp ignoredSettingCValue). StataBE 19.5
// takes `r` to `rmsg`, `tr` to `trace` and `mat` to `matsize`; scheme, varabbrev,
// fredkey, maxvar, segmentsize and max_memory only in full (`set sch` r(199)).
// Any other word is returned as it is.
std::string canonicalSetName(const std::string& word) {
  static const std::pair<std::string, size_t> kAbbrev[] = {
      {"rmsg", 1}, {"trace", 2}, {"matsize", 3}};
  for (const auto& [full, minLen] : kAbbrev)
    if (word.size() >= minLen && word.size() <= full.size() && full.compare(0, word.size(), word) == 0)
      return full;
  return word;
}

// Replace-or-append a user matrix (tabulate's matcell()/matrow()/matcol()).
// The same three lines are file-local twice already (handlers_aer_results.cpp
// storeMatrix, handlers_m3.cpp storeMatrixResult); neither is reachable from
// here. Replace, never append: findMatrix() answers the FIRST match. A scalar
// of the same name is destroyed: scalars and matrices share one namespace ([P]
// matrix define, "Namespace").
void storeUserMatrix(SessionState& session, const std::string& name, eval::EvalMatrix mat) {
  session.eval.scalars.erase(name);
  session.eval.stringScalars.erase(name);
  for (auto& p : session.eval.matrices) {
    if (p.first == name) {
      p.second = std::move(mat);
      return;
    }
  }
  session.eval.matrices.emplace_back(name, std::move(mat));
}

// A variable label keeps its first 80 characters, counted as UTF-8 characters
// (`label variable`, lb_var_long log:190; tabulate's generate() labels alike,
// StataBE 19.5: `s==` and 77 a's, or 77 é's in 157 bytes).
std::string variableLabelText(const std::string& text) {
  size_t chars = 0;
  for (size_t i = 0; i < text.size(); ++i) {
    if ((static_cast<unsigned char>(text[i]) & 0xC0) == 0x80) continue;
    if (chars == 80) return text.substr(0, i);
    ++chars;
  }
  return text;
}

// ---------------------------------------------------------------------------
// B4-W3b: `tabulate VAR, generate(stub)`.
//
// Returns a null json on success, or the error block to emit. The dummies are
// built by dispatching the user's own `generate` (BUG-1012: it was
// piet::handlers::runGen, which skips the string row evaluator) rather than a
// private column writer, so the storage plane, the resident-frame opt-in and
// the identifier validation are the ones already in service and cannot drift
// from them.
//
// BUG-1012 (StataBE 19.5, w2as_summary_stats_family):
//  - the sample: a row outside `if`/`in` gets missing in every dummy, as a row
//    whose source is missing does (sx_tab_g_q, sx_tab_generate: 0 before);
//  - with `missing` the missing rows are a category of their own: that dummy is
//    1 there and every other dummy 0 (sx_tab_gen_m);
//  - a string variable works (sx_tab_g, sx_tab_gen_m: "" is its missing); it
//    failed in the engine with rc 1 before;
//  - the label is `var==` + the value label when the level has one (sx_tab_gen,
//    sx_tabulate_generate: `g==alpha`, `d==no`), the string itself for a string
//    variable (`s==a`), else the value as %11.4f (`z==     3.0000`,
//    `aw==     0.5000`; the label read back through `local : variable label`).
nlohmann::ordered_json runTabulateGenerate(const std::string& stub,
                                           const std::vector<std::string>& varlist,
                                           const std::vector<std::string>& rawRows,
                                           SessionState& session,
                                           const std::optional<std::string>& ifExpr,
                                           const std::optional<std::string>& inRange,
                                           bool includeMissing) {
  using ojson = nlohmann::ordered_json;
  if (varlist.size() != 1) {
    // Stata's `generate()` is a ONE-WAY tabulate option; the two-way syntax
    // does not carry it. Refusing by name rather than inventing a meaning for
    // the cross-tabulated case.
    return ojson(eval::errorBlock(
        "unsupported_option",
        "tabulate: generate() is a one-way option — it makes one indicator per level of a "
        "single variable, and a two-way table has no single level list to make them from."));
  }
  if (!strutil::isValidStataIdentifier(stub)) {
    return ojson(eval::errorBlock(
        "syntax_error",
        "tabulate: invalid variable name `" + stub + "' — " + strutil::stataIdentifierRuleText()));
  }
  if (rawRows.empty()) {
    return ojson(eval::errorBlock("bad_input",
                                  "tabulate: generate() has no levels to make indicators from."));
  }
  // Stata's own guard, and it fires BEFORE any column is written: a collision
  // half-way through would leave the dataset holding stub1..stub3 of a set of
  // five, which is worse than the error.
  Datastore* ds = session.datastore();
  for (size_t i = 0; i < rawRows.size(); ++i) {
    const std::string nm = stub + std::to_string(i + 1);
    bool exists = false;
    if (const auto* card = session.shadowVarlistIfCurrent()) {
      for (const auto& info : *card)
        if (info.name == nm) exists = true;
    } else if (ds != nullptr) {
      exists = ds->has(nm);
    }
    if (exists) {
      return ojson(eval::errorBlock("bad_input", "variable " + nm + " already defined"));
    }
  }

  const std::string& srcVar = varlist[0];
  const bool srcIsString = ds != nullptr && ds->has(srcVar) && ds->dtype(srcVar) == "factor";
  // The sample: the `if`/`in` qualifier as typed, on each generate.
  std::string qual;
  if (ifExpr.has_value() && !strutil::trim(*ifExpr).empty()) qual += " if " + *ifExpr;
  if (inRange.has_value() && !strutil::trim(*inRange).empty()) qual += " in " + *inRange;
  const std::map<int, std::string>* valueLabels = nullptr;
  if (!srcIsString) {
    auto it = session.valueLabels.find(srcVar);
    if (it != session.valueLabels.end()) valueLabels = &it->second;
  }
  for (size_t i = 0; i < rawRows.size(); ++i) {
    const std::string nm = stub + std::to_string(i + 1);
    const std::string& lvl = rawRows[i];
    // A numeric level key is the value's round-trip rendering, or `.`/`.a` for a
    // missing category (runTabulate with `missing`); a string level is the text.
    const bool missingLevel = !srcIsString && !lvl.empty() && lvl[0] == '.' &&
                              (lvl.size() == 1 || (lvl.size() == 2 && std::islower(
                                                       static_cast<unsigned char>(lvl[1]))));
    std::string escaped;
    for (char ch : lvl) {
      if (ch == '"' || ch == '\\') escaped += '\\';
      escaped += ch;
    }
    const std::string cmp = srcIsString ? (srcVar + "==\"" + escaped + "\"") : (srcVar + "==" + lvl);
    // The missing rule (oracle pins_oracle_g.log:170-176): without `missing`,
    // a missing source gives a missing dummy, never 0.
    const std::string missTest = srcIsString ? (srcVar + "==\"\"") : ("missing(" + srcVar + ")");
    const std::string rhs = includeMissing ? cmp : ("cond(" + missTest + ", ., " + cmp + ")");
    // Through the user's own `generate` (a string comparison takes the string
    // row evaluator there, `in` its _n path); runGen alone sent `s==""` to the
    // engine, which compares a string's missing code unequal to "".
    ojson made = dispatch("quietly generate byte " + nm + " = " + rhs + qual, session);
    if (made.is_array())
      for (const auto& b : made)
        if (b.is_object() && b.value("type", std::string()) == "error") return b;
    // Stata makes these `byte`, formats them %8.0g, and labels them `var==`
    // followed by the level's value label, the string, or the value as %11.4f.
    // BUG-1012: the width is 11, read back through `local : variable label`
    // (w2as sx_tab_g_q_v1 `z==     3.0000`, sx_tab_generate_v1
    // `aw==     0.5000`). pins_shapes.log:36 shows `rep78== 1.0000` because
    // `describe` prints the label with its runs of blanks collapsed; the
    // earlier `%7.4f` reading of that line was wrong.
    session.varMeta[nm].stataType = "byte";
    std::string shown = lvl;
    if (!srcIsString) {
      double v = 0;
      bool haveValue = false;
      try {
        v = std::stod(lvl);
        haveValue = !missingLevel;
      } catch (...) {
      }
      const double rv = std::nearbyint(v);
      std::map<int, std::string>::const_iterator vl;
      if (haveValue && valueLabels != nullptr && v == rv && std::fabs(v) < 2147483648.0 &&
          (vl = valueLabels->find(static_cast<int>(rv))) != valueLabels->end()) {
        shown = vl->second;
      } else {
        // BUG-1152: %11.4f as Stata renders it, so a value too wide for it goes
        // to e-notation in the same 11 columns (`z==-1.2346e+05`, where printf
        // wrote 12); applyStataFmt is string()'s rendering, so the field's
        // leading blanks are put back. A missing category: an extended `.a`
        // takes those 11 columns, the system `.` only 7 (StataBE 19.5:
        // `z==         .a` and `z==      .`, for a %9.0g byte and a %20.0g
        // double alike).
        if (haveValue) {
          shown = eval::applyStataFmt(v, "%11.4f");
          if (shown.size() < 11) shown.insert(0, 11 - shown.size(), ' ');
        } else {
          char buf[64];
          std::snprintf(buf, sizeof(buf), lvl == "." ? "%7s" : "%11s", lvl.c_str());
          shown = buf;
        }
      }
    }
    session.setVarLabel(nm, variableLabelText(srcVar + "==" + shown));
  }
  return ojson();  // null => no error
}

}  // namespace

// BUG-1447: describe's r(k) and r(width) are the dataset's, whatever varlist was
// described (StataBE 19.5: `describe x1 c` on eight variables posts r(k) 8 and the
// full r(width) 36). r(width) is one observation's storage in bytes: byte 1, int 2,
// long and float 4, double and strL 8, str# its # (22 for float float byte double
// str5; 36 with an int, a long and a strL added). The types are the ones describe
// prints (the recorded storage type, else describe's default); a string whose width
// was never recorded ("str#") leaves the width unknown, and r(width) refused by name.
// Read off the resident card when it is current, as runDescribe reads it.
struct DescribeShape {
  size_t k = 0;
  std::optional<double> width;
};
static DescribeShape describeDatasetShape(SessionState& session) {
  std::vector<std::pair<std::string, bool>> vars;  // name, string-typed
  if (const std::vector<VarInfo>* card = session.residentReadCard()) {
    for (const auto& v : *card) vars.emplace_back(v.name, v.dtype == "factor");
  } else if (Datastore* ds = session.datastore()) {
    for (const auto& n : ds->names()) vars.emplace_back(n, ds->dtype(n) == "factor");
  }
  DescribeShape shape;
  shape.k = vars.size();
  double width = 0.0;
  for (const auto& [name, isString] : vars) {
    const auto meta = session.varMeta.find(name);
    const std::string t = meta != session.varMeta.end() && meta->second.stataType.has_value()
                              ? *meta->second.stataType
                              : (isString ? "str#" : "float");
    if (t == "byte") width += 1;
    else if (t == "int") width += 2;
    else if (t == "long" || t == "float") width += 4;
    else if (t == "double" || t == "strL") width += 8;
    else if (t.size() > 3 && t.compare(0, 3, "str") == 0 &&
             t.find_first_not_of("0123456789", 3) == std::string::npos)
      width += std::stod(t.substr(3));
    else return shape;
  }
  shape.width = width;
  return shape;
}

// The r() a summarize block leaves behind, shared by the plain `summarize` arm and
// the by-prefixed driver (which captures only the LAST group's block, as Stata does).
static void captureSummarizeR(const ojson& summBlock, bool meanonly, bool detail,
                              SessionState& session) {
  eval::captureRFromBlock(summBlock, session.eval);
  // BUG-476: `meanonly` leaves r(sd) (and everything captureRFromBlock
  // DERIVES from sd — r(Var), computed as sd*sd when sd was present but
  // Var was not; macros.cpp's captureRFromBlock, "Var = sd^2" branch —
  // and r(median), the p50 alias) Stata-missing. ONLY r(sd) is oracle-
  // measured (BUG-476.do's own header: "do not add an arm for it
  // without capturing it first" for r(min)/r(max)); the percentile/
  // skewness/kurtosis keys are pruned here too because they are NEVER
  // computed under Stata's real meanonly (undisputed — meanonly skips
  // the whole variance/percentile pass), not because a cell pins them.
  // NOT erased: Stata's own convention (and the BUG-476 probe's `scalar
  // b476sd = r(sd)` / `assert b476sd >= .`, which must run to rc 0, not
  // error) is that a numeric r()-result the last command did not compute
  // reads back as MISSING, not "not found" — real Stata's `display
  // r(sd)` after `summarize ..., meanonly` succeeds and shows `.`
  // (oracle_r.log:71's `%21x r(sd)` = `+1.0000000000000X+3ff`, exactly
  // 2^1023 = kStataMissingSentinel). Erasing the key instead would make
  // PIET's own command-line r()-read path answer the honest-but-wrong
  // "r(sd) is not set" error (macros.cpp's unsetRAsMissing guard, the
  // FIX-RESTIMATE precedent) — so these are SET to the sentinel, not
  // removed. The RENDERED block (summBlock itself) is untouched — only
  // the r() store captureRFromBlock just built is overwritten, so
  // freeze/summarize.json and every other summarize fixture's displayed
  // table stays byte-identical.
  if (meanonly && summBlock.value("type", std::string()) != "error") {
    for (const char* k : {"sd", "Var", "p1", "p5", "p10", "p25", "p50", "p75",
                           "p90", "p95", "p99", "skewness", "kurtosis"}) {
      session.eval.rValues.set(k, eval::kStataMissingSentinel);
    }
  }
  // The names Stata posts for this invocation (summarize's help; meanonly
  // as well): an absent one is a named r(111), any other absent name `.`
  // (RULING.md C.1, D.3.2).
  if (summBlock.value("type", std::string()) != "error") {
    // BUG-1511: on an empty sample (a string variable too) Stata posts r(N),
    // r(sum_w) and r(sum) and nothing else, with or without detail or meanonly
    // (StataBE 19.5, scratch-laneA2/agents/rprog/x1511.do: `return list` lists
    // those three, in that order; r(mean), r(p50) then read `.` like any name the
    // command does not post). The block's detail percentiles and moments on no
    // rows (0) are not Stata's, nor the meanonly missings set above.
    const double* nObs = session.eval.rValues.find("N");
    if (nObs != nullptr && *nObs == 0.0) {
      eval::OrderedFloatDict posted;  // `return list` lists the newest first
      for (const char* k : {"sum", "sum_w", "N"})
        if (const double* v = session.eval.rValues.find(k)) posted.set(k, *v);
      session.eval.rValues = std::move(posted);
      eval::declareRPosted(session.eval, "summarize", {"N", "sum_w", "sum"});
      return;
    }
    std::vector<std::string> stataNames = {"N", "mean", "min", "max", "sum_w", "sum"};
    if (!meanonly) stataNames.insert(stataNames.end(), {"Var", "sd"});
    if (detail)
      stataNames.insert(stataNames.end(), {"skewness", "kurtosis", "p1", "p5", "p10", "p25",
                                           "p50", "p75", "p90", "p95", "p99"});
    // A name Stata posts but whose value is undefined here (N 1: Var, sd,
    // skewness, kurtosis) is posted as missing, as Stata posts it (BUG-1013; w2as
    // sx_sum_d_single). The block carries those as null, which captureRFromBlock
    // skips, so they were refused as unposted.
    for (const auto& n : stataNames)
      if (session.eval.rValues.find(n) == nullptr)
        session.eval.rValues.set(n, eval::kStataMissingSentinel);
    eval::declareRPosted(session.eval, "summarize", std::move(stataNames));
  }
}

// The stored-SCALAR substitution the row plane has always done, lifted out of
// substituteRowExprRefs unchanged (BUG-456 follow-up, 2026-09-09) so gen's
// pre-resolve can run the same substitution on a COPY, purely to DECIDE the
// type of an expression it must not otherwise rewrite. A VARIABLE always wins
// over a same-named scalar -- Stata's rule, and the reason this asks the read
// card before substituting.
//
// name-binding-display-literals: text inside a string literal is data. The
// pass used to run over the raw text, so a scalar whose name occurred inside a
// literal was substituted into it: with `scalar control = 5`, `count if grp ==
// "control"` compared with "5" and counted 0 of 3, `assert tag == "control"`
// failed rc 9, and a string scalar's own inserted literal (`st` -> "treat") was
// substituted again by a scalar `treat` (w2av_name_binding bd_row_cntlit,
// bd_row_cntT, bd_row_keep, bd_row_dropT, bd_row_strsc, bd_row_asrt). The
// literals are protected once (eval::protectLiterals) and a string scalar's
// value goes in as one more protected literal.
static std::string substituteStoredScalars(const std::string& in, SessionState& session) {
  if (session.eval.scalars.items().empty()) return in;
  std::vector<std::string> spans;
  std::string e = eval::protectLiterals(in, &spans);
  const std::vector<VarInfo>* card = session.residentReadCard();
  for (const auto& [sn, sv] : session.eval.scalars.items()) {
    if (e.find(sn) == std::string::npos) continue;
    bool isVar = false;
    if (card != nullptr) {
      for (const auto& vi : *card)
        if (vi.name == sn) { isVar = true; break; }
    } else if (session.currentHbin().has_value()) {
      Datastore* ds0 = session.datastore();
      isVar = ds0 != nullptr && ds0->has(sn);
    }
    if (isVar) continue;
    // BUG-710: a STRING scalar (`scalar st = s[2]`) is a string literal here,
    // not its bare text: `assert st == "ab2"` reached the engine as `ab2 ==
    // "ab2"`, an unknown variable. Text holding a `"` keeps the old form.
    if (session.eval.isStringScalar(sn) && sv.find('"') == std::string::npos) {
      spans.push_back("\"" + sv + "\"");
      e = eval::substituteBareName(e, sn, "\"0L" + std::to_string(spans.size() - 1) + "L0\"");
      continue;
    }
    e = eval::substituteBareName(e, sn, eval::valueRefText(sv));  // FU-842
  }
  return eval::restoreLiterals(e, spans);
}

// While a command whose `if` does not read the caller's `_rc` runs with `_rc`
// in that `if`, the lines its handler dispatches with the same qualifier do
// not read it either: recode hands its `if` to generate/replace, which would
// otherwise answer it with the caller's value (`recode x (1=10) if _rc == 111,
// gen(rx)` filled rx[1] = 10; StataBE leaves it missing).
thread_local int tl_rcHiddenDepth = 0;
struct RcHiddenScope {
  RcHiddenScope() { ++tl_rcHiddenDepth; }
  ~RcHiddenScope() { --tl_rcHiddenDepth; }
  RcHiddenScope(const RcHiddenScope&) = delete;
  RcHiddenScope& operator=(const RcHiddenScope&) = delete;
};

// FU-169 / BUG-1359: Stata reads the `_rc` system value inside an expression
// (`assert _rc == 0`, aer_estim_oracle.log:412-414; `generate q = _rc`, `keep if
// _rc == 0`, `summarize x1 if _rc == 111`). No row evaluator has the name, so it
// is resolved here, once, for the row callers: the `if` of the commands that
// read the caller's value (ifReadsCallerRc), assert's claim, and
// generate/replace's right-hand side (preResolveGenExpr). Until BUG-1359 only
// runCount did this, so `count if _rc` and `assert _rc` were served and
// gen/replace/keep/drop/summarize reached the engine with `unknown variable
// _rc`. Whole identifier only (substituteBareName's \b rule), never inside a
// "..." literal, and a dataset variable named _rc still wins.
static std::string substituteRcValue(const std::string& in, SessionState& session) {
  if (tl_rcHiddenDepth > 0 || in.find("_rc") == std::string::npos) return in;
  const std::string rc = session.eval.locals.get("_rc", "0");
  if (rc.empty() || rc.find_first_not_of("0123456789") != std::string::npos) return in;
  if (const std::vector<VarInfo>* card = session.residentReadCard()) {
    for (const auto& vi : *card)
      if (vi.name == "_rc") return in;
  } else if (session.currentHbin().has_value()) {
    Datastore* ds0 = session.datastore();
    if (ds0 != nullptr && ds0->has("_rc")) return in;
  }
  std::vector<std::string> spans;
  const std::string e = eval::protectLiterals(in, &spans);
  return eval::restoreLiterals(eval::substituteBareName(e, "_rc", rc), spans);
}

// Whether a command's `if` reads the caller's `_rc`. In Stata many commands run
// captures of their own before they read the `if`, and so see another value:
// after `capture confirm variable nosuch` (rc 111), `regress y x if _rc == 0`,
// `mean`, `pctile`, `egen ... if` and `by g: egen ... if` fit or fill every row
// and `if _rc == 111` is r(2000) or no row, while `recode`, `table` and `ci`
// see neither 0 nor 111 (StataBE 2026-10-29). These are the commands measured
// to see 111. Every other command's `_rc` is left as text, the engine's r(111)
// "unknown variable _rc", as before BUG-1359: an answer read with the caller's
// value would be a silent wrong one.
static bool ifReadsCallerRc(const std::string& canonicalCmd) {
  static const std::unordered_set<std::string> kReadsCallerRc = {
      "generate", "replace", "keep",     "drop",   "count",  "assert",
      "summarize", "list",   "tabulate", "tabstat", "corr",  "pwcorr",
      "xtile",    "ttest",   "centile",  "encode",  "duplicates"};
  return kReadsCallerRc.count(canonicalCmd) != 0;
}

// BUG-1327 / BUG-1093: `assert s[1] == "ab"` and `assert r0[1] == ""` read ONE
// cell of a string variable. Neither row evaluator has that form -- the engine
// has no string operand, the PIET row evaluator no `[` -- so the claim was the
// engine's `expr: expected )` (rc 198 under capture) where StataBE answers. A
// subscript whose index is a constant (digits, `_N`, arithmetic on them) is the
// same value on every row, so it is read here, through display's reader
// (readVarObservation), and goes in as a string literal; the claim then takes
// the string-condition route like any other. Stata's rules (oracle 2026-10-29):
// the index truncates toward zero (`s[1.6]` is s[1], `s[2.5]` is s[2]) and an
// index outside 1.._N is "" (`s[0]`, `s[9]`, `s[-1]`, `s[.]`). Left in place: an index
// naming anything but `_N` (`s[_n-1]` varies by row), a numeric variable (the
// engine reads x[k] itself), a matrix, and text holding a `"`. assert only: it
// is refused under by:, where a subscript counts within the group.
static std::string substituteStringCells(const std::string& in, SessionState& session) {
  if (in.find('[') == std::string::npos || !session.hasData()) return in;
  std::vector<std::string> spans;
  const std::string e = eval::protectLiterals(in, &spans);
  const auto isId0 = [](char c) { return cecon::ident::isIdentStartByte(c); };
  const auto isId = [](char c) { return cecon::ident::isIdentByte(c); };
  std::string out;
  bool any = false;
  size_t i = 0, last = 0;
  int depth = 0;  // parentheses around the current position
  while (i < e.size()) {
    const char c = e[i];
    if (c == '"') {  // a placeholder: data
      const size_t close = e.find('"', i + 1);
      i = close == std::string::npos ? e.size() : close + 1;
      continue;
    }
    if (!isId0(c) || (i > 0 && (isId(e[i - 1]) || e[i - 1] == '.'))) {
      depth += c == '(' ? 1 : c == ')' ? -1 : 0;
      ++i;
      continue;
    }
    size_t j = i;
    while (j < e.size() && isId(e[j])) ++j;
    // A blank before the `[` keeps it a subscript only inside parentheses:
    // StataBE answers `assert (s [1] == "a")` and refuses `assert s [1] == "a"`
    // r(109), so the bare form stays the engine's refusal.
    size_t k = j;
    while (depth > 0 && k < e.size() && (e[k] == ' ' || e[k] == '\t')) ++k;
    const size_t close = k < e.size() && e[k] == '[' ? e.find(']', k + 1) : std::string::npos;
    if (close == std::string::npos) {
      i = j;
      continue;
    }
    const size_t start = i;
    const std::string name = e.substr(i, j - i);
    const std::string idxText = e.substr(k + 1, close - k - 1);
    i = j;
    if (idxText.find_first_of("[,\"") != std::string::npos ||
        session.eval.findMatrix(name) != nullptr)
      continue;
    // A number (`1e0`) and a missing value (`.`, `.a`) are constants too; any
    // other name but `_N` is not.
    bool constIdx = true;
    for (size_t p = 0; p < idxText.size() && constIdx;) {
      const char ch = idxText[p];
      if (std::isdigit(static_cast<unsigned char>(ch)) || ch == '.') {
        size_t q = p;
        while (q < idxText.size() &&
               (std::isdigit(static_cast<unsigned char>(idxText[q])) || idxText[q] == '.'))
          ++q;
        if (q < idxText.size() && (idxText[q] == 'e' || idxText[q] == 'E')) {
          size_t x = q + 1;
          if (x < idxText.size() && (idxText[x] == '+' || idxText[x] == '-')) ++x;
          if (x < idxText.size() && std::isdigit(static_cast<unsigned char>(idxText[x]))) {
            q = x;
            while (q < idxText.size() && std::isdigit(static_cast<unsigned char>(idxText[q]))) ++q;
          }
        } else if (q == p + 1 && ch == '.' && q < idxText.size() && idxText[q] >= 'a' &&
                   idxText[q] <= 'z' && (q + 1 == idxText.size() || !isId(idxText[q + 1]))) {
          ++q;  // `.a`
        }
        p = q;
        continue;
      }
      if (!isId0(ch)) {
        ++p;
        continue;
      }
      size_t q = p;
      while (q < idxText.size() && isId(idxText[q])) ++q;
      constIdx = idxText.compare(p, q - p, "_N") == 0;
      p = q;
    }
    if (!constIdx) continue;
    // A column the card lists as numeric is the engine's (`assert x[1] == 5`),
    // known without opening the frame, which readVarObservation would do.
    if (const std::vector<VarInfo>* card = session.residentReadCard()) {
      const std::optional<std::string> full = resolveVarName(name, session);
      if (!full) continue;
      const auto it = std::find_if(card->begin(), card->end(),
                                   [&](const VarInfo& vi) { return vi.name == *full; });
      if (it != card->end() && it->dtype != "factor") continue;
    }
    bool idxIsStr = false;
    const std::optional<std::string> idxVal =
        eval::evalArith(idxText, &session.eval, nullptr, &idxIsStr);
    if (!idxVal || idxIsStr) continue;
    // A missing index names no observation: "" (StataBE `s[.] == ""` is true).
    const bool missingIdx = !idxVal->empty() && (*idxVal)[0] == '.' &&
                            (idxVal->size() == 1 ||
                             (idxVal->size() == 2 && (*idxVal)[1] >= 'a' && (*idxVal)[1] <= 'z'));
    char* endp = nullptr;
    const double d = missingIdx ? 0.0 : std::strtod(idxVal->c_str(), &endp);
    if (!missingIdx &&
        (endp == idxVal->c_str() || *endp != '\0' || !std::isfinite(d) || std::fabs(d) > 9e15))
      continue;
    const std::optional<VarObservation> cell =
        readVarObservation(name, static_cast<long long>(std::trunc(d)), session);
    if (!cell || !cell->isString || cell->text.find('"') != std::string::npos) continue;
    spans.push_back("\"" + (cell->inRange ? cell->text : std::string()) + "\"");
    out.append(e, last, start - last);
    out += "\"0L" + std::to_string(spans.size() - 1) + "L0\"";
    last = close + 1;
    i = close + 1;
    any = true;
  }
  if (!any) return in;
  out.append(e, last, std::string::npos);
  return eval::restoreLiterals(out, spans);
}

// name-binding-display-literals: a unique abbreviation of a variable in a row
// expression is that variable, as in Stata ([U] 11.2), and it wins over a
// stored scalar of the same name, as the exact name does: `count if lng > 3`
// and `generate g = lng*2` (variable lngdp) were r(111) "unknown variable"
// from the engine (w2av_name_binding bd_row_abbr, bd_row_genab). The names are
// rewritten to the full name before the scalar pass; quoted text, calls
// (`name(`), missing codes (`.a`) and label names (`"x":lbl`) are left alone, and an ambiguous
// abbreviation stays as typed for the engine's own r(111). The variable lookup
// is display's (resolveVarName), so display, the macro layer and the row plane
// bind a name by one rule.
static std::string expandRowVarAbbrevs(const std::string& in, SessionState& session) {
  if (!session.hasData()) return in;
  std::vector<std::string> spans;
  const std::string e = eval::protectLiterals(in, &spans);
  // core/ident.h's scanner tests: a Unicode name (`دخل_2020`) is one name, so its
  // ASCII tail `_2020` is never bound on its own.
  const auto isId0 = [](char c) { return cecon::ident::isIdentStartByte(c); };
  const auto isId = [](char c) { return cecon::ident::isIdentByte(c); };
  std::string out;
  bool any = false;
  size_t i = 0, last = 0;
  while (i < e.size()) {
    const char c = e[i];
    if (c == '"') {  // a placeholder (or an unterminated quote): data
      const size_t close = e.find('"', i + 1);
      i = close == std::string::npos ? e.size() : close + 1;
      continue;
    }
    // `.a` is a missing code and `"x":lbl` a value-label lookup, not names.
    if (!isId0(c) || (i > 0 && (isId(e[i - 1]) || e[i - 1] == '.' || e[i - 1] == ':'))) {
      ++i;
      continue;
    }
    size_t j = i;
    while (j < e.size() && isId(e[j])) ++j;
    size_t k = j;
    while (k < e.size() && (e[k] == ' ' || e[k] == '\t')) ++k;
    if (k < e.size() && e[k] == '(') {
      i = j;
      continue;
    }
    const std::string name = e.substr(i, j - i);
    const std::optional<std::string> full = resolveVarName(name, session);
    if (full && *full != name) {
      out.append(e, last, i - last);
      out += *full;
      last = j;
      any = true;
    }
    i = j;
  }
  if (!any) return in;
  out.append(e, last, std::string::npos);
  return eval::restoreLiterals(out, spans);
}

// e(sample) T2 (internal note esample/SPEC.md §5.1): the ROW
// reading of the token `e(sample)`, outside string literals (eval::
// replaceEsampleToken). One reading for every row caller, decided by
// SessionState::esampleReading:
//   kColumn  -> `(__piet_esample__==SERIAL)`: the hidden mask column the fit
//               installed, whose admitted rows hold that install's serial
//               (rows added since the fit hold missing -> 0, which is
//               Stata's answer for them, w2_esample.log:1076 E6, :1104 E7);
//   kZero    -> `0`: no estimation results, a posted e(), or a model whose
//               column left with the data (w2_esample.log:95-129 A1-A5, :1174
//               E9, :1215 E11, :1272 E14, :1290 E15, :1487 G5, :1634 G11);
//   kRefused -> the named r(198) of SPEC §5.5 (a model PIET has no mask for);
//               `errOut` gets the block and the text is returned unchanged.
// Because this runs BEFORE resolveEstScalarRefs, the resolver's own e(sample)
// arm (the macro layer's observation-1 value) is never reached from a row
// caller: in a row expression e(sample) is a per-row value, not obs 1.
// Layers: the row layer (this) and, through the rewritten text, the CiParser
// and the engine's --keepif/--gen/cond() planes, which see a plain variable
// reference and are unchanged; the macro/arith layer is eval/macros.cpp.
static std::string rewriteEsampleToken(const std::string& in, SessionState& session,
                                       std::optional<ojson>* errOut) {
  if (!eval::hasEsampleToken(in)) return in;
  const SessionState::EsampleReading r = session.esampleReading();
  switch (r.kind) {
    case SessionState::EsampleReading::Kind::kColumn:
      return eval::replaceEsampleToken(
          in, std::string("(") + kEsampleColumn + "==" + std::to_string(r.serial) + ")");
    case SessionState::EsampleReading::Kind::kZero:
      return eval::replaceEsampleToken(in, "0");
    case SessionState::EsampleReading::Kind::kRefused:
      break;
  }
  if (errOut != nullptr) {
    *errOut = ojson(eval::errorBlock("engine_error", r.refusal, std::nullopt, std::nullopt,
                                     std::nullopt, "r(198)"));
  }
  return in;
}

// `errOut` (BUG-456): when non-null, receives the error block for the two
// shapes this pass can now refuse -- an unknown coefficient name (r(111)) and a
// string-vs-number constant compare (r(109)). A null errOut keeps the text as
// substituted so far; the engine then refuses it in its own words.
// `readsCallerRc`: false leaves `_rc` as text (ifReadsCallerRc).
std::string substituteRowExprRefs(const std::string& in, SessionState& session,
                                  std::optional<ojson>* errOut = nullptr,
                                  bool readsCallerRc = true) {
  // FU-875: the text as Stata reads it, blanks inside ( ) and [ ] taken out
  // (core/expr_blanks.h), before any name in it is bound: `generate g = (pri ce)`
  // and `count if (pri ce) == 20` read price, where binding `pri` first made
  // the engine's squeeze join pricece (StataBE 19.5: rc 0, g == price).
  const std::string squeezed = cecon::squeeze_bracketed_blanks(in);
  std::optional<ojson> esErr;
  std::string e = rewriteEsampleToken(squeezed, session, &esErr);  // e(sample) T2, first
  if (esErr.has_value()) {
    if (errOut != nullptr) *errOut = std::move(esErr);
    return e;
  }
  // BUG-1420: c(NAME) is a value in a row expression as in display. cobra has no
  // c(), so `count if c(N) == 3`, `assert c(N) == 3` and `keep if c(N) == 3` were
  // rc 1 "function c() is not supported" where StataBE 19.5 counts, and a name
  // that is no c() key is "c(NAME) undefined", r(133), on an `if`, assert's claim
  // and gen/replace alike. A key Stata has and PIET does not serve stays as text
  // and the engine refuses it.
  if (e.find("c(") != std::string::npos) {
    e = substituteCRefs(e, session);
    if (auto undef = eval::undefinedCRef(e)) {
      if (errOut != nullptr)
        *errOut = ojson(eval::errorBlock("engine_error", *undef, std::nullopt, std::nullopt,
                                         std::nullopt, "r(133)"));
      return e;
    }
  }
  // eb-order T-P1: the same trigger as the macro layer's matrix pre-substitution
  // (eval/macros.cpp evalArith). Keyed on `[` alone, `assert colsof(e(b)) == 3`,
  // `generate k = colsof(B)` and `count if el(B,1,2) == 2` reached the engine as
  // text and were refused rc 1 ("function colsof() is not supported"), while
  // `display colsof(e(b))` answered 3 (w2g_ebmat.log:305 e1_colsof_eb).
  {
    const std::string lc = strutil::lower(e);
    // BUG-1373: rownumb() and mreldif() are the same layer's (`assert
    // rownumb(A,"r2") == 2` was "function rownumb() is not supported", rc 1).
    if (e.find('[') != std::string::npos || lc.find("colsof") != std::string::npos ||
        lc.find("rowsof") != std::string::npos || lc.find("colnumb") != std::string::npos ||
        lc.find("rownumb") != std::string::npos || lc.find("mreldif") != std::string::npos ||
        lc.find("el(") != std::string::npos) {
      e = eval::substituteMatrixRefs(e, session.eval);
    }
  }
  // BUG-456: this used to run ONLY when the text contained `scalar(` -- so the
  // `_b[]`/`_se[]`/e() halves of the resolver never ran on a row predicate
  // without one, and `assert _se[mpg]>0` reached the engine as a variable
  // subscript ("unknown variable _se") while `assert _se[mpg]*scalar(one)>0`
  // passed (capture A2 vs A13, the proof). The trigger is now the union of the
  // resolver's own prefilters. BUG-432 left `_b[`/`_se[` as text for the
  // engine when no e() was live, because the resolver answered `.` there.
  // eb-order T-P3: the resolver now refuses a null coefficient source itself
  // with Stata's r(111) "_b not allowed when e(b) is not present" -- a live e()
  // without an e(b) (w2g_ebmat.log:2196 e15c_gen_z after pca) and no e() at all
  // alike (w2v_estpost_ematrix.log ep15f_local_bx1) -- so the gate is gone and
  // an estpost/erepost e() resolves from the block e(b) comes from.
  const bool wantsEst = e.find("_b[") != std::string::npos || e.find("_se[") != std::string::npos ||
                        e.find("scalar(") != std::string::npos || eval::hasBareEParen(e);
  if (wantsEst) {
    std::string bErr;
    e = eval::resolveEstScalarRefs(e, session.eval, &bErr);
    if (!bErr.empty()) {
      if (errOut != nullptr) {
        // BUG-456 follow-up: r(111) or r(109) -- the resolver says which.
        const std::string code = eval::splitErrCode(&bErr);
        *errOut = ojson(eval::errorBlock("engine_error", bErr, std::nullopt, std::nullopt,
                                         std::nullopt, code));
      }
      return e;
    }
  }
  // `_rc` before the abbreviation pass: the system name is never a short form
  // of a variable such as `_rcode`.
  if (readsCallerRc) e = substituteRcValue(e, session);
  // BUG-1330: c() values, the pre-substitution display and gen/replace already
  // ran (substituteCRefs): `assert c(pi) > 3` and `count if c(N) > 0` reached
  // the engine as text, "function c() is not supported" at rc 1. A c() nothing
  // answers is r(133), as StataBE words it.
  e = substituteCRefs(e, session);
  std::string cRc;
  if (auto cErr = unservedCRefError(e, &cRc)) {
    if (errOut != nullptr)
      *errOut = ojson(eval::errorBlock("engine_error", *cErr, std::nullopt, std::nullopt,
                                       std::nullopt, "r(" + cRc + ")"));
    return e;
  }
  e = expandRowVarAbbrevs(e, session);
  e = substituteStoredScalars(e, session);
  // BUG-456 follow-up (2026-09-09): a NUMBER in a STRING-typed argument slot is
  // r(109) here too, and this test must run whether or not the resolver above
  // did -- the macro layer substitutes a PRESENT e-scalar upstream, so
  // `count if strpos(e(N),"2")` reaches this point as `strpos(20.0,"2")` with
  // `wantsEst` false. A stored scalar (`scalar k = 5` -> `strpos(5,"x")`) is
  // the same shape and is caught by the same pass, after its substitution.
  if (errOut != nullptr) {
    std::string badCall;
    if (eval::stringFnNumericArg(e, &badCall)) {
      std::string msg = eval::stringSlotMismatchError(badCall);
      const std::string code = eval::splitErrCode(&msg);
      *errOut = ojson(eval::errorBlock("engine_error", msg, std::nullopt, std::nullopt,
                                       std::nullopt, code));
      return e;
    }
  }
  // BUG-456: decide the constant string compares the engine cannot (see the
  // fold's own comment above). A string-vs-number compare is r(109).
  {
    std::string mismatch;
    if (!foldRowConstStringCompares(e, &mismatch) && errOut != nullptr) {
      *errOut = ojson(eval::errorBlock(
          "engine_error",
          "type mismatch: `" + mismatch + "` compares a string with a number",
          std::nullopt, std::nullopt, std::nullopt, "r(109)"));
    }
  }
  return e;
}

// BUG-456: gen/replace's own expression, resolved BEFORE strmutate::needsStringEval
// decides the route. `_b[]`/`_se[]`/e()/scalar() go to literals (model-gated
// for the coefficient half, BUG-432), then the constant string compares fold
// (`(e(cmd)=="regress")` -> `(1)`). `rhs` is re-derived so the detour test that
// follows sees the resolved text. Returns the error block for r(111)/r(109).
static std::optional<ojson> preResolveGenExpr(std::string& expr, std::string& rhs,
                                              SessionState& session) {
  // BUG-1420: a c(NAME) that is no c() key is r(133) "c(NAME) undefined" here as
  // on display/local/scalar (StataBE 19.5: generate, replace and a string
  // generate alike); cobra read it as a call of an unknown function c(), rc 1.
  // The served names are substituted first, here rather than after the string
  // detour, which otherwise got c(os) raw: `generate str12 o = c(os)`,
  // `generate L = strlen(c(os))` and `replace s = c(os) in 1` failed at obs 1
  // where StataBE gives MacOSX, 6 and MacOSX.
  if (expr.find("c(") != std::string::npos) {
    expr = substituteCRefs(expr, session);
    if (auto undef = eval::undefinedCRef(expr))
      return ojson(eval::errorBlock("engine_error", *undef, std::nullopt, std::nullopt,
                                    std::nullopt, std::string("r(133)")));
  }
  {  // An impossible date literal is Stata's r(198) (w2y_fn_datetime fd_tq_bad,
     // fd_tm_bad, fd_td_bad: `generate double g = tq(2010q5)`). The engine refuses
     // it too, but as an internal "expr: ... invalid" error that reached the user
     // as rc 1; the string-path evaluator raised it at obs 1 with no code at all.
    // The text as Stata reads it (core/expr_blanks.h), so the message names the
    // literal as Stata prints it: tc(01jan201025:00:00) invalid. Brackets that
    // do not pair come first: `generate g = tq(2010q5) + (1` is r(132).
    if (const std::string u = cecon::bracket_imbalance(expr); !u.empty())
      return ojson(eval::errorBlock("engine_error", u, std::nullopt, std::nullopt, std::nullopt,
                                    std::string("r(132)")));
    std::string bad;
    cecon::for_each_datetime_literal(
        cecon::squeeze_bracketed_blanks(expr),
        [&](const std::string& name, const std::string& raw, size_t, size_t) {
          cecon::LiteralScale ls;
          cecon::literal_scale_for(name, ls);
          if (!std::isnan(cecon::stata_datetime_literal(raw, ls))) return true;
          bad = name + "(" + raw + ")";
          return false;
        });
    if (!bad.empty())
      return ojson(eval::errorBlock("engine_error", bad + " invalid", std::nullopt, std::nullopt,
                                    std::nullopt, std::string("r(198)")));
  }
  {  // e(sample) T2: the per-row mask, before the resolver's obs-1 arm can see it
    std::optional<ojson> esErr;
    expr = rewriteEsampleToken(expr, session, &esErr);
    if (esErr.has_value()) return esErr;
  }
  {  // BUG-1359: `_rc` on the right-hand side, before either route sees it
    const size_t eq = expr.find('=');
    if (eq != std::string::npos)
      expr = expr.substr(0, eq + 1) + substituteRcValue(expr.substr(eq + 1), session);
  }
  {  // BUG-1330: c() values too: the string route runs before the cobra route's
     // own substituteCRefs, so `gen str20 s = c(os) + "!"` failed at obs 1. A
     // c() nothing answers is r(133), not cobra's `function c() is not supported`.
    const size_t eq = expr.find('=');
    if (eq != std::string::npos) {
      const std::string sub = substituteCRefs(expr.substr(eq + 1), session);
      std::string cRc;
      if (auto cErr = unservedCRefError(sub, &cRc))
        return ojson(eval::errorBlock("engine_error", *cErr, std::nullopt, std::nullopt,
                                      std::nullopt, "r(" + cRc + ")"));
      expr = expr.substr(0, eq + 1) + sub;
    }
  }
  // eb-order T-P3: no model gate, as in substituteRowExprRefs above -- the
  // resolver refuses a null coefficient source with r(111) itself.
  const bool wantsEst = expr.find("_b[") != std::string::npos ||
                        expr.find("_se[") != std::string::npos ||
                        expr.find("scalar(") != std::string::npos || eval::hasBareEParen(expr);
  if (wantsEst) {
    std::string bErr;
    expr = eval::resolveEstScalarRefs(expr, session.eval, &bErr);
    if (!bErr.empty()) {
      // BUG-456 follow-up: r(111) or r(109) -- the resolver says which.
      const std::string code = eval::splitErrCode(&bErr);
      return ojson(eval::errorBlock("engine_error", bErr, std::nullopt, std::nullopt,
                                    std::nullopt, code));
    }
  }
  // BUG-456 follow-up (review finding 3, 2026-09-09): gen's TYPE decision is
  // made on a COPY with the stored scalars substituted, because the real
  // substitution happens further down (after the strmutate detour has already
  // claimed anything holding a string literal) and gen must not be re-routed by
  // a scalar. Measured: `scalar one = 1` then `gen z = (e(cmd)==one)` was rc 1
  // "generate: expression evaluation failed at obs 1" with NO r() code, where
  // the identical `count if e(cmd)==one` reported r(109) -- the same claim,
  // two codes. Only the ERROR comes from this copy; `expr` is untouched.
  {
    std::string probe = substituteStoredScalars(expr, session);
    std::string badCall, probeMismatch;
    if (eval::stringFnNumericArg(probe, &badCall)) {
      std::string msg = eval::stringSlotMismatchError(badCall);
      const std::string code = eval::splitErrCode(&msg);
      return ojson(eval::errorBlock("engine_error", msg, std::nullopt, std::nullopt,
                                    std::nullopt, code));
    }
    if (!foldRowConstStringCompares(probe, &probeMismatch)) {
      return ojson(eval::errorBlock(
          "engine_error",
          "type mismatch: `" + probeMismatch + "` compares a string with a number",
          std::nullopt, std::nullopt, std::nullopt, "r(109)"));
    }
  }
  {  // BUG-456 follow-up: the string-slot type test, ungated (see the row plane).
    std::string badCall;
    if (eval::stringFnNumericArg(expr, &badCall)) {
      std::string msg = eval::stringSlotMismatchError(badCall);
      const std::string code = eval::splitErrCode(&msg);
      return ojson(eval::errorBlock("engine_error", msg, std::nullopt, std::nullopt,
                                    std::nullopt, code));
    }
  }
  std::string mismatch;
  if (!foldRowConstStringCompares(expr, &mismatch)) {
    return ojson(eval::errorBlock("engine_error",
                                  "type mismatch: `" + mismatch + "` compares a string with a number",
                                  std::nullopt, std::nullopt, std::nullopt, "r(109)"));
  }
  const size_t eq = expr.find('=');
  rhs = (eq == std::string::npos) ? std::string() : trimWs(expr.substr(eq + 1));
  return std::nullopt;
}

namespace {

// `title("A title")` arrives with its quote characters still on the value (the
// tokenizer keeps them, exactly as the file-part handling has to strip them
// too). One matching pair, so a title that is itself quoted survives.
std::string dequoteEstTitle(std::string t) {
  if (t.size() >= 2 && ((t.front() == '"' && t.back() == '"') ||
                        (t.front() == '\'' && t.back() == '\''))) {
    t = t.substr(1, t.size() - 2);
  }
  return t;
}

// esample T0 (BUG-636): the post-estimation verbs that re-fit the ACTIVE model
// from its data file -- test/testparm/lincom/nlcom (runPostEstimationM4's
// lastModelBaseArgv fallback, their only path) -- replay `fitDataPath` else
// `dataPath` as `--data`. In-session those files are pinned (the live model by
// reclaimSupersededScratch exclusion (7), a stored one by pinStoredModel), but
// `estimates use` reads back the path the .ster recorded at `estimates save`,
// and nothing pins that: a later fit plus a few mutations in the SAME session
// reclaim it, and a .ster saved by another session names that session's
// scratch -- as does the same model after `estimates store` + `estimates
// restore`. The verbs then failed with
// `[data_io] cannot open .../piet_native_XXXX/...hbin` at rc 1. predict already
// refuses this shape by name (estimation_m4c.cpp, the BUG-518 remainder); this
// is its sibling for the verbs whose ONLY path is that replay. NOT margins:
// `margins, dydx()` after a linear fit answers from the coefficient table
// (buildLinearAmeRawM4) without the file, so a guard here would refuse a
// working form; its replay branches need the check inside runMargins. It fires only
// when the replay can NOT work: no engine handle to answer from, the file is
// not the live head (a dirty resident head is legitimately not on disk) and not
// a virtual save, and nothing exists at the path.
std::optional<ojson> refuseIfFitDataGone(const std::string& verb, SessionState& session) {
  const auto& lmOpt = session.lastModel();
  if (!lmOpt.has_value() || !lmOpt->estHandle.empty()) return std::nullopt;
  const LastModel& lm = *lmOpt;
  const std::string& fitFile = lm.fitDataPath.empty() ? lm.dataPath : lm.fitDataPath;
  if (fitFile.empty() || session.isVirtualSave(fitFile)) return std::nullopt;
  if (session.currentHbin().has_value() && *session.currentHbin() == fitFile) return std::nullopt;
  std::error_code ec;
  if (std::filesystem::exists(cecon::to_native_path(fitFile), ec)) return std::nullopt;
  return ojson(eval::engineError(
      verb + ": the data `" + (lm.stataCmd.empty() ? lm.cobraCmd : lm.stataCmd) +
          "` was fitted on are no longer available to re-fit from in this version of PIET "
          "(the working frame it was fitted on has been released -- e.g. results loaded back "
          "by `estimates use`). Re-run the estimation command, then " +
          verb + ".",
      "unsupported_option"));
}

// W2 eststoprefix (BUG-520). `eststo`'s own options, read in ONE place so the
// prefix arm (Pre-0b) and the flat arm cannot drift: the prefix arm never
// reaches the tokenizer with its head, so a net that lived only in the flat arm
// would leave `eststo m1, bogus: regress y x` accepted-and-ignored.
struct EststoStoreOpts {
  std::optional<std::string> title;
  std::string autoPrefix = "est";  // estout's default; w2_eststo_prefix.log A2/A11
};

std::optional<ojson> readEststoStoreOpts(
    const std::vector<std::pair<std::string, eval::OptValue>>& opts, EststoStoreOpts* out) {
  // NOT kVocabEststoStore.known: that slot is the UNION with `estimates`' own
  // vocabulary (b/se/stats/replace), none of which `eststo` takes.
  static const std::unordered_set<std::string> kRead = {"title", "prefix"};
  if (auto err = eval::rejectUnknownOptions(opts, kRead, kVocabEststoStore.unimplemented)) {
    return ojson(*err);
  }
  for (const auto& [k, v] : opts) {
    if (k == "title" && !v.isBool) out->title = dequoteEstTitle(v.strValue);
    if (k == "prefix") {
      // eststo.ado declares Prefix(name): one Stata name, nothing else. A bare
      // `prefix` or a non-name would otherwise mint a key no `esttab` can spell.
      // The one name rule (core/ident.h): `prefix(نموذج)` is a name, `prefix(١m)`
      // is not.
      const std::string p = v.isBool ? std::string() : trimWs(v.strValue);
      if (!strutil::isValidStataIdentifier(p)) {
        return ojson(eval::engineError(
            "eststo: option prefix() takes one name, e.g. `eststo, prefix(m): regress y x`",
            "syntax_error"));
      }
      out->autoPrefix = p;
    }
  }
  return std::nullopt;
}

// The colon that ends an `eststo [name] [, options] :` head: the first one that
// is outside a quoted span (w2_eststo_prefix.log A6 -- `eststo t1, title("a: b"):
// regress y w` is rc 0 and stores t1) AND outside a `...' macro reference, so an
// inline extended function in the name (`eststo m`: word 1 of `l'': ...`) does
// not split the line inside the macro. npos when there is none -- which is what
// keeps the flat `eststo m1, title("a: b")` flat. A colon BEHIND a trailing
// ` // comment` is not one either: tokenize() strips that comment (`\s+//.*$`)
// for a line that reaches it raw, so `eststo m1 // note: keep` is the flat store
// it has always been, not a prefix with the head `m1 // note`.
size_t findEststoPrefixColon(const std::string& s) {
  const std::vector<bool> quoted = eval::quotedSpanMask(s);
  int tick = 0;
  for (size_t i = 0; i < s.size(); ++i) {
    if (quoted[i]) continue;
    const char ch = s[i];
    if (ch == '/' && tick == 0 && i > 0 && (s[i - 1] == ' ' || s[i - 1] == '\t') &&
        i + 1 < s.size() && s[i + 1] == '/') {
      return std::string::npos;
    }
    if (ch == '`') {
      ++tick;
    } else if (ch == '\'' && tick > 0) {
      --tick;
    } else if (ch == ':' && tick == 0) {
      return i;
    }
  }
  return std::string::npos;
}

}  // namespace


// BUG-525 (W2 labelglued; oracle w2_cmdtriage.log arms di_glued, corr_glued,
// levelsof_glued, erase_glued): Stata ends the command word at the first
// character that cannot be part of a name, so `di("glued")`, `corr(a c)`,
// `levelsof(g), local(lv)` and `erase"ct_tmp.dta"` all run (rc 0 each; the
// glued corr's r(rho) equals the spaced control's). PIET cut the word at
// whitespace, so each was looked up as a command literally named
// `di("glued")` -> r(199). Normalising the RAW line once, at the top of
// dispatch, is what makes every downstream reader agree: the prefix
// strippers, the tokenizer, and the handlers that re-parse parsed.raw against
// their own `^cmd\s+` anchors (runErase, handleDisplay, runLabelDefine ...).
// Only the leading name is touched, and only when `(` or `"` is glued to it;
// a line that does not begin with a name (`*`, `//`, a macro reference, a
// quote, a digit) comes back byte-identically. The echo block is built from
// the line as typed, before this runs, so the log shows what the user wrote.
std::string splitGluedHead(const std::string& line) {
  size_t b = 0;
  while (b < line.size() && std::isspace(static_cast<unsigned char>(line[b]))) ++b;
  if (b >= line.size()) return line;
  if (!cecon::ident::isIdentStartByte(line[b])) return line;  // core/ident.h
  size_t e = b + 1;
  while (e < line.size() && cecon::ident::isIdentByte(line[e])) ++e;
  if (e >= line.size() || (line[e] != '(' && line[e] != '"')) return line;
  return line.substr(0, e) + " " + line.substr(e);
}

// BUG-524 (W2 labelglued; oracle w2_cmdtriage.log section B): Stata's `label`
// subcommands abbreviate down to a documented minimum each, and BELOW that
// minimum the line is r(198) "invalid syntax" -- not an unknown subcommand.
// Measured (each full spelling is its abbreviation's control):
//   label define / label def / la de        rc 0    (label_define_full, label_def, la_de)
//   label d                                 r(198)  (label_d_too_short)
//   label values / label val / label value  rc 0    (label_values_full, label_val, label_value)
//   label va                                r(198)  (label_va_too_short; f keeps yn3)
// The remaining floors are the syntax block of `help label`
// (ado/base/l/label.sthlp: `{opt da:ta}` `{opt var:iable}` `{opt di:r}`
// `{opt l:ist}` `{opt lang:uage}`; copy, drop, rename and save carry no colon
// and do not abbreviate) -- the published definition, not an oracle arm, and
// said so here. The command word's own floor (`la`) is the registry's row.
// PIET compared the token to the full spelling only, so about 1,070 corpus
// statements (`label val` 519, `label def` 453, `label de` 79 ...) were
// r(199) hard stops.
struct LabelSubFloor {
  const char* full;
  size_t minLen;
};
constexpr LabelSubFloor kLabelSubFloors[] = {
    {"data", 2}, {"variable", 3}, {"define", 2}, {"values", 3},   {"dir", 2},      {"list", 1},
    {"copy", 4}, {"drop", 4},     {"rename", 6}, {"save", 4},     {"language", 4},
};
enum class LabelSubMatch { kResolved, kTooShort, kUnknown };
// `sub` is the typed token, lowercased. The floors are chosen so that at its
// minimum every subcommand's prefix is unique (`de`/`di`/`da` at 2, `l` at 1 --
// `language` needs `lang`), so the first floor-satisfying hit is the only one.
LabelSubMatch resolveLabelSub(const std::string& sub, std::string* canonical) {
  bool sawPrefix = false;
  for (const auto& f : kLabelSubFloors) {
    const std::string full(f.full);
    if (sub.empty() || sub.size() > full.size() || full.compare(0, sub.size(), sub) != 0) continue;
    sawPrefix = true;
    if (sub.size() >= f.minLen) {
      *canonical = full;
      return LabelSubMatch::kResolved;
    }
  }
  return sawPrefix ? LabelSubMatch::kTooShort : LabelSubMatch::kUnknown;
}

// BUG-524: rawWithCanonicalCmd's idea one token further in. runLabelDefine
// anchors on `^label\s+define\s+` in the raw line, so a subcommand typed as an
// abbreviation is rewritten to its full spelling there (after the command word
// has been). Whole-token match only; anything unexpected returns the line
// unchanged, exactly as the command-word helper does.
std::string rawWithCanonicalLabelSub(const std::string& raw, const std::string& typedSub,
                                     const std::string& canonicalSub) {
  if (typedSub == canonicalSub || typedSub.empty()) return raw;
  size_t b = raw.find_first_not_of(" \t");
  if (b == std::string::npos) return raw;
  size_t e1 = raw.find_first_of(" \t", b);
  if (e1 == std::string::npos) return raw;
  size_t s = raw.find_first_not_of(" \t", e1);
  if (s == std::string::npos) return raw;
  size_t e = raw.find_first_of(" \t", s);
  if (e == std::string::npos) e = raw.size();
  if (lower(raw.substr(s, e - s)) != typedSub) return raw;
  return raw.substr(0, s) + canonicalSub + raw.substr(e);
}

// BUG-733: run a fit that writes its own e() values (eval.eScalars / eStrings /
// eMatrices) BEFORE it calls setLastModel -- grep-measured, exactly runPca,
// runOaxaca and runTeffects (handlers_aer_estim.cpp). setLastModel drops a
// live posting (`ereturn post`, estpost) and the names recorded on top of the
// previous fit (SessionState::eOnFitNames) at its top, which for these three
// would also drop the fit's own values (`ereturn local title "mine"` then
// `pca` would lose pca's own e(title)), so both are ended here, before the fit
// writes anything. A fit that installs no model (refused, or the engine
// failed) leaves e() as it was in Stata, so the stores, the posting flag and
// the record are put back. FU-375: after a successful install every e() name
// the fit left is recorded, so the NEXT install drops it -- the fit's own
// pre-install keys (pca's e(N), oaxaca's posted e(b)) are no longer read for
// a later regress. Recording every name, not a diff, also covers a key the
// fit rewrote with an equal value; setLastModel's own republished macros
// (e(cmd), e(depvar), ...) are republished by the next install anyway.
template <typename Fit>
static ojson runFitWritingEFirst(SessionState& session, Fit&& fit) {
  auto scalars = session.eval.eScalars;
  auto strings = session.eval.eStrings;
  auto matrices = session.eval.eMatrices;
  const bool posted = session.ePostedLive;
  const unsigned long long postedMask = session.ePostedEsampleSerial;
  auto onFit = session.eOnFitNames;
  const unsigned long long installsBefore = session.lastModelInstalls;
  session.supersedePostedE();
  session.dropEOnFit();
  ojson out = fit();
  if (session.lastModelInstalls == installsBefore) {
    session.eval.eScalars = std::move(scalars);
    session.eval.eStrings = std::move(strings);
    session.eval.eMatrices = std::move(matrices);
    session.ePostedLive = posted;
    session.ePostedEsampleSerial = postedMask;
    session.eOnFitNames = std::move(onFit);
    return out;
  }
  for (const auto& [k, v] : session.eval.eScalars.items()) session.noteEOnFit(k);
  for (const auto& [k, v] : session.eval.eStrings.items()) session.noteEOnFit(k);
  for (const auto& [k, m] : session.eval.eMatrices) session.noteEOnFit(k);
  return out;
}

// W2D-T01 round 3: test/testparm/lincom/nlcom/estat/erepost act on the model
// already installed and may re-enter setLastModel on it (estimation_m4c.cpp
// setEstHandleM4, the estat re-call; handlers_aer_tables.cpp runErepost). In
// Stata they leave e() alone (test/lincom/nlcom store r() only; repost changes
// b/V and nothing else), so a live posting and the names recorded on top of
// the fit must survive them; none of the six writes the e() stores itself, so
// the stores are put back exactly. A no-op unless a posting is live or a name
// sits on top of the fit. `nlcom ..., post` and `estat ..., post` install a
// NEW e() and are not wrapped.
// fvgroup-operator (see the call site in dispatchImpl): `op.(t1 t2 ...)` ->
// `op.t1 op.t2 ...`, the operator applied to every `#`/`##` atom of a term that
// carries no operator of its own. The varlist arrives split on blanks, so a group
// spans tokens from `op.(first` to `last)`. A group that does not close on a
// token ending in `)`, or holds a `(`/`=`, is left exactly as typed.
static std::vector<std::string> expandFactorGroupTokens(const std::vector<std::string>& in) {
  static const std::regex reOpen(
      R"(^(.*#)?(i|c|o|bn|ibn|ib\d+|b\d+|ib\((?:first|last|freq)\))\.\((.*)$)", std::regex::icase);
  const auto applyOp = [](const std::string& op, const std::string& term) {
    std::string out, atom;
    auto flush = [&]() {
      if (atom.empty()) return;
      out += atom.find('.') == std::string::npos ? op + "." + atom : atom;
      atom.clear();
    };
    for (char c : term) {
      if (c == '#') {
        flush();
        out += c;
      } else {
        atom += c;
      }
    }
    flush();
    return out;
  };
  std::vector<std::string> out;
  for (size_t i = 0; i < in.size(); ++i) {
    std::smatch m;
    if (!std::regex_match(in[i], m, reOpen)) {
      out.push_back(in[i]);
      continue;
    }
    const std::string interaction = m.str(1), op = m.str(2);
    std::vector<std::string> terms;
    std::string first = m.str(3);
    size_t j = i;
    bool closed = false, bad = false;
    std::string cur = first;
    for (;;) {
      if (cur.find('(') != std::string::npos || cur.find('=') != std::string::npos) {
        bad = true;
        break;
      }
      const size_t close = cur.find(')');
      if (close != std::string::npos) {
        if (close + 1 != cur.size()) {
          bad = true;
          break;
        }
        if (close > 0) terms.push_back(cur.substr(0, close));
        closed = true;
        break;
      }
      if (!cur.empty()) terms.push_back(cur);
      if (++j >= in.size()) break;
      cur = in[j];
    }
    if (!closed || bad || terms.empty()) {
      out.push_back(in[i]);
      continue;
    }
    // `c.( )` puts its operator on the single-variable terms only: a `#` term
    // inside it keeps its atoms as factors, so `c.(a#b z)` is `a#b c.z`
    // (StataBE 19.5: the same fit as `i.a#i.b c.z`, df_m 36; `c.a#c.b` would be
    // a different, continuous product).
    const bool cOp = op.size() == 1 && (op[0] == 'c' || op[0] == 'C');
    for (const auto& t : terms)
      out.push_back(interaction +
                    (cOp && t.find('#') != std::string::npos ? t : applyOp(op, t)));
    i = j;
  }
  return out;
}

template <typename Run>
static ojson runPostestKeepingE(SessionState& session, Run&& run) {
  if (!session.ePostedLive && session.eOnFitNames.empty()) return run();
  auto scalars = session.eval.eScalars;
  auto strings = session.eval.eStrings;
  auto matrices = session.eval.eMatrices;
  const bool posted = session.ePostedLive;
  const unsigned long long postedMask = session.ePostedEsampleSerial;
  auto onFit = session.eOnFitNames;
  ojson out = run();
  session.eval.eScalars = std::move(scalars);
  session.eval.eStrings = std::move(strings);
  session.eval.eMatrices = std::move(matrices);
  session.ePostedLive = posted;
  session.ePostedEsampleSerial = postedMask;
  session.eOnFitNames = std::move(onFit);
  return out;
}

nlohmann::ordered_json dispatchImpl(const std::string& rawCmdTyped, SessionState& session) {
  // BUG-525: the line as TYPED feeds the echo; everything else in this
  // function reads the glued-head-normalised copy (splitGluedHead above).
  const std::string rawCmd = splitGluedHead(rawCmdTyped);
  // C4-L1 window-trace attribution (report-only): remember this command's verb
  // token so an EMH window closure can name its closer. Nested dispatches
  // (capture/quietly strips, do-file replay) re-enter here and overwrite, so
  // the innermost — semantically executing — verb wins. No-op env-off.
  dprof::noteLine();
  ojson echo;
  ojson blocks;
  {
    dprof::Scope __z(dprof::kEntry);
    session.noteC4TraceVerb(rawCmd);
    echo = echoBlock(rawCmdTyped);
    blocks = ojson::array();
    blocks.push_back(echo);
  }

  // ------------------------------------------------------------------
  // Pre-(-1). Loop/if block detection for the console path (M3a, mirrors
  // dispatcher.py's step Pre-(-1) — the tokenizer rejects bare loop
  // keywords, so this must run before tokenize()). Only fires when the raw
  // line ITSELF carries a brace block (single-line `foreach v in ... { ... }`
  // form) — do-file replay routes through doexec::executeDoSource directly,
  // never through here.
  {
    dprof::Scope __z(dprof::kLoopIf);
    std::string strippedForLoop = trimWs(rawCmd);
    if (loops::opensBraceBlock(strippedForLoop)) {
      doexec::ExecuteResult result = doexec::executeDoSource(strippedForLoop, session, dispatch);
      ojson out = ojson::array();
      out.push_back(echo);
      for (const auto& b : result.blocks) out.push_back(b);
      return out;
    }
    // P4 wave: Stata's BRACELESS one-line branch, `if COND command ...`. The
    // braced form has always worked; without this, the braceless one reached
    // the command table as a command literally named `if` and was rejected as
    // unsupported. Handled here rather than in doexec's brace aggregator
    // because both the console and do-file paths funnel through dispatch(),
    // and the condition/body semantics are runIf's already — this only has to
    // find the split point and hand it a one-line body. FU-602: in Stata an
    // `else command` line may follow this form (w2_esample_copies.do:39-40,
    // StataBE runs it). A do-file, loop or program body pairs the two in
    // doexec's brace aggregator (collectElse), which turns the pair into a
    // braced if/else block; only an unpaired one-line `if` reaches this
    // intercept. An `else` that reaches dispatch has no `if` in front of it
    // and is refused by name below.
    if (strippedForLoop.size() >= 4 && lower(strippedForLoop.substr(0, 4)) == "else" &&
        (strippedForLoop.size() == 4 || strippedForLoop[4] == '{' ||
         std::isspace(static_cast<unsigned char>(strippedForLoop[4])) != 0)) {
      ojson out = ojson::array();
      out.push_back(echo);
      out.push_back(ojson(eval::errorBlock(
          "syntax_error",
          "`else` must follow an `if` command: the line before this `else` is not a one-line "
          "`if exp command` and does not close an `if` block",
          std::string("else"), std::nullopt, strutil::trim(rawCmd), "r(198)")));
      return out;
    }
    if (strippedForLoop.size() > 3 && lower(strippedForLoop.substr(0, 3)) == "if " &&
        strippedForLoop.find('{') == std::string::npos) {
      // Stata ends the condition at the first token that starts a command. The
      // condition is an expression, so it cannot contain a bare space-separated
      // word that is not part of an operator — scan for the first top-level
      // (unquoted, unparenthesised) space that follows a complete comparison.
      // In practice the corpus form is `if EXPR <cmd> ...`; splitting on the
      // last top-level relational operand keeps `if !_rc exit 1` and
      // `if `x' != 0 exit `x'` both correct.
      const std::string rest = trimWs(strippedForLoop.substr(3));
      std::string cond, body;
      if (loops::splitOneLineIf(rest, cond, body) && !body.empty()) {
        ojson out = ojson::array();
        out.push_back(echo);
        ojson r = loops::runOneLineIf(cond, body, session, dispatch);
        if (r.is_array()) { for (const auto& b : r) out.push_back(b); }
        else out.push_back(r);
        return out;
      }
      // FU-875: the split refuses a condition whose brackets do not pair; that
      // is Stata's r(132), not a line for the command tokenizer (StataBE 19.5:
      // `if (1 == 1 display "in"` "too few ')' or ']'", `if 1 == 1) display
      // "in"` "too many ')' or ']'").
      if (const std::string u = cecon::bracket_imbalance(rest); !u.empty()) {
        ojson out = ojson::array();
        out.push_back(echo);
        out.push_back(ojson(eval::errorBlock("engine_error", u, std::nullopt, std::nullopt,
                                             std::nullopt, std::string("r(132)"))));
        return out;
      }
    }
    // BUG-547: the legacy `for` command (foreach/forvalues are its
    // modern replacements, but `for` is still valid Stata and 34 corpus lines
    // in 5 papers use it). Handled here, next to the one-line-if intercept
    // above, for the same reason: the tokenizer rejects a bare `for` as an
    // unrecognized command word, so this must run before tokenize(). A
    // `capture noisily for ...`/`quietly for ...` prefix already works
    // without any special-casing here: Pre-0 below strips the prefix and
    // RE-ENTERS dispatchImpl with the bare `for ...` remainder, which hits
    // this same branch on the recursive call.
    if (loops::isLegacyForLine(strippedForLoop)) {
      ojson out = ojson::array();
      out.push_back(echo);
      ojson r = loops::runLegacyFor(strippedForLoop, session, dispatch);
      if (r.is_array()) { for (const auto& b : r) out.push_back(b); }
      else out.push_back(r);
      return out;
    }
  }

  // ------------------------------------------------------------------
  // Pre-(-0.5). `do FILE` / `run FILE` console command (M3a port of
  // dispatcher.py's same-named step). Reads a .do file from disk and
  // replays it through the SAME executor as a future /api/do route.
  {
    dprof::Scope __z(dprof::kDoRun);
    std::string strippedForLoop = trimWs(rawCmd);
    size_t sp = strippedForLoop.find_first_of(" \t");
    std::string firstTok = sp == std::string::npos ? strippedForLoop : strippedForLoop.substr(0, sp);
    std::string firstTokLower = lower(firstTok);
    // W2 doinclude (BUG-528): `include FILE` is the third spelling of this arm.
    // It runs the file through the same executor but SHARES the caller's locals
    // both ways (no private scope, no positional args) -- w2_include_scope.log
    // include_basic / include_nested / include_in_loop / include_in_program.
    const bool isInclude = firstTokLower == "include";
    if (isInclude && sp == std::string::npos) {
      // Bare `include`: a filename is required. Not oracle-covered; answered as
      // invalid syntax rather than falling through to "command not supported".
      ojson out = ojson::array();
      out.push_back(echo);
      out.push_back(ojson(eval::errorBlock("syntax_error", "include: a filename is required",
                                           std::string("include"))));
      return out;
    }
    if ((firstTokLower == "do" || firstTokLower == "run" || isInclude) &&
        sp != std::string::npos) {
      // BUG-089: split the remainder into a filename + positional args instead
      // of treating the whole line as one path. The filename is the first
      // quote-aware token (bind strips a surrounding quote layer so a quoted
      // path with spaces still opens); each further token seeds `1'..`N' via
      // executeDo's args vector — the same mechanism the JSON /api/do bridge uses.
      //
      // BS-018: `do FILE [args] [, nostop]` — split off a trailing options
      // tail at the first UNQUOTED comma (same boundary every other Stata
      // command uses, eval::findUnquotedComma) BEFORE any of the above
      // filename/positional-arg tokenizing. Previously there was no comma
      // handling here at all: `do file.do, nostop` let the comma glue onto
      // the end of the filename token (scanOneToken stops at whitespace, not
      // ','), so the path never resolved; `do file.do , nostop` (space before
      // the comma) instead shredded ","/"nostop" into the positional-args
      // vector, corrupting `1'`/`2'` for any do-file that uses them. Splitting
      // here first means the filename/positional-arg extraction below never
      // sees the options tail at all — behaviorally a no-op for the common
      // no-comma case (findUnquotedComma returns -1, argsAndFileTail ==
      // remainder unchanged).
      std::string remainder = trimWs(strippedForLoop.substr(sp + 1));
      // W2 doinclude (BUG-529): this arm runs BEFORE step 0b's macro expansion,
      // so `do "$DIR/x.do"` used to look for a file literally named `$DIR/x.do`
      // (r(601)) -- 1,766 of 2,786 corpus do/run lines build the path from a
      // macro. Expand the tail here, in the CALLER's scope, with 0b's own call
      // pair and error passthrough (w2_include_scope.log do_gpath /
      // include_gpath / include_gbrace_noext / include_lpath answer rc 0). The
      // args are expanded here too, which is what Stata does: `do f `x'` hands
      // the callee x's value, not the text of a reference its private scope
      // could no longer resolve. Expanding before the comma split matches
      // Stata's expand-then-parse order (a macro holding a comma moves it).
      if (eval::hasMacroRefs(remainder)) {
        eval::ExpandResult er = eval::expandMacros(remainder, session.eval);
        if (er.isError) {
          ojson out = ojson::array();
          out.push_back(echo);
          out.push_back(er.error);
          return out;
        }
        remainder = trimWs(er.line);
      }
      std::string argsAndFileTail = remainder;
      std::string doOptsTail;
      {
        long long commaPos = eval::findUnquotedComma(remainder);
        if (commaPos >= 0) {
          argsAndFileTail = trimWs(remainder.substr(0, static_cast<size_t>(commaPos)));
          doOptsTail = trimWs(remainder.substr(static_cast<size_t>(commaPos) + 1));
        }
      }
      if (!doOptsTail.empty()) {
        // `nostop` is the ONLY real Stata do/run option (syntax diagram:
        // `do filename [args] [, nostop]`) — consumed/recognized here so it
        // no longer gets mangled into doArgs above; its own "continue past
        // an error instead of stopping the do-file" semantic is a SEPARATE
        // follow-up (doexec::executeDoSource's stop-on-first-error loop is
        // unchanged by this fix — out of BS-018's parsing-bug scope). Any
        // OTHER token here is genuinely unsupported -> the same honest
        // r(198)-parity reject every other arm's option net uses (BS-097),
        // rather than silently swallowing it.
        static const std::unordered_set<std::string> kDoRunKnownOpts = {"nostop"};
        // W2 doinclude: `include filename [, adopath]` has its own option set
        // ([P] include); `nostop` is refused there (w2_include_scope.log
        // include_opt_nostop: "option nostop not allowed", r(198)). `adopath`
        // is accepted without its search: no target-paper line uses it.
        static const std::unordered_set<std::string> kIncludeKnownOpts = {"adopath"};
        auto doOpts = eval::parseOpts(doOptsTail);
        if (auto err = eval::rejectUnknownOptions(
                doOpts, isInclude ? kIncludeKnownOpts : kDoRunKnownOpts)) {
          ojson out = ojson::array();
          out.push_back(echo);
          out.push_back(ojson(*err));
          return out;
        }
      }
      auto [fnRaw, argsRest] = eval::scanOneToken(argsAndFileTail, /*bind=*/true);
      std::vector<std::string> doArgs;
      std::string rest = argsRest;
      while (!trimWs(rest).empty()) {
        std::string prev = rest;
        auto [tok, newRest] = eval::scanOneToken(rest, /*bind=*/true);
        rest = newRest;
        if (rest == prev) break;  // defensive: no progress → avoid infinite loop
        doArgs.push_back(tok);
      }
      if (!fnRaw.empty() && isInclude && !doArgs.empty()) {
        // `include` takes no arguments (w2_include_scope.log include_extra_token:
        // "invalid syntax", r(198)).
        ojson out = ojson::array();
        out.push_back(echo);
        out.push_back(ojson(eval::errorBlock("syntax_error", "invalid syntax",
                                             std::string("include"))));
        return out;
      }
      if (!fnRaw.empty()) {
        doexec::ExecuteFileResult fres;
        const int callerPreserveLevel = session.preserveLevel();
        {
          // W2 doinclude (BUG-530): do/run get a PRIVATE local scope; include
          // does not. The guard lives HERE, not in executeDo, so the top-level
          // `--do`/GUI routes and `parallel do` keep today's session scope. The
          // args are seeded by executeDoSource into the fresh scope, so the
          // caller's `0'/`1'.. are no longer clobbered (do_args_private).
          std::optional<doexec::PrivateLocalScope> privateScope;
          // BUG-922: do/run also get their own preserve level; include runs
          // as if typed in its caller and shares the caller's (oracle
          // w2ad_prog_preserve_nest pn_i_pres_held: 621, pn_i_not).
          std::optional<DoFileLevelGuard> levelGuard;
          // BUG-920: and start outside any loop (pn_lm_cb_do).
          std::optional<program::LoopFloorGuard> loopFloor;
          // BUG-1459: a `version` the file sets ends with it.
          std::optional<CallerVersionScope> versionScope;
          if (!isInclude) {
            privateScope.emplace(session);
            levelGuard.emplace(session);
            loopFloor.emplace(session);
            versionScope.emplace(session);
          }
          try {
            fres = doexec::executeDo(doexec::withDefaultDoExtension(fnRaw), session, dispatch,
                                     doArgs);
          } catch (...) {
            // BUG-922: the exception path concludes the level too.
            if (!isInclude) program::concludeLevelAfterThrow(callerPreserveLevel, session);
            throw;
          }
        }
        // BUG-922: when a do/run file concludes, the copy it preserved and did
        // not restore is restored ([P] preserve), on error as on success
        // (w2ad_prog_preserve_nest pn_d_leave, pn_r_leave, pn_d_leave_err,
        // pn_d_leave_exit: ret 5:31, then the caller's restore is r(622)), and
        // its tempvars, tempnames and tempfiles die (pn_tv_do_gone: 111).
        ojson levelRestoreBlocks = ojson::array();
        if (!isInclude)
          program::concludeLevel(callerPreserveLevel, session, levelRestoreBlocks);
        ojson out = ojson::array();
        ojson echoWithPath = echo;
        echoWithPath["resolved_path"] = fres.resolvedPath;
        echoWithPath["source"] = fres.source;
        out.push_back(echoWithPath);
        // BS-036 (BETA campaign C3): real Stata's ONLY documented difference
        // between `do` and `run` is that `run` does not echo the file's own
        // commands as they execute (their OUTPUT — display/summarize/error/
        // etc — still shows exactly as `do` would; only the per-line ". cmd
        // text" echo is suppressed). Previously both were byte-identical.
        // Reuses the SAME block-filtering idiom the quietly/noisily prefix
        // handling above already applies (`b.value("type","") != "echo"`).
        bool suppressEcho = (firstTokLower == "run");
        for (const auto& b : fres.result.blocks) {
          if (suppressEcho && b.is_object() && b.value("type", std::string()) == "echo") continue;
          out.push_back(b);
        }
        for (auto& b : levelRestoreBlocks) out.push_back(std::move(b));
        // W2 doinclude (BUG-531): `exit N` (N != 0) inside the file ends it AND
        // hands the caller r(N) -- w2_include_scope.log do_exit5 / include_exit5:
        // "exit 5 / r(5);", _rc 5 under capture. The inner run consumed and
        // erased its halt sentinel (doexec.cpp handleUncapturedError), so the
        // code was lost and the caller saw rc 0. A bare `exit` still ends only
        // the file (do_exit / include_exit: rc 0, caller continues). The block
        // carries the exact rc in `reason`, like `error N`, and its message is
        // deliberately NOT the "exit N" sentinel shape: the caller is not
        // exiting cleanly, it is receiving an error.
        if (fres.result.stoppedAtExitCode.has_value() && *fres.result.stoppedAtExitCode != 0) {
          const int code = *fres.result.stoppedAtExitCode;
          ojson exitErr = ojson(eval::engineError("do-file ended with exit " + std::to_string(code)));
          exitErr["reason"] = "r(" + std::to_string(code) + ")";
          out.push_back(exitErr);
        }
        if (fres.result.stoppedAtReason.has_value()) {
          std::string linepart;
          if (fres.result.stoppedAtLine.has_value()) {
            linepart = " at line " + std::to_string(*fres.result.stoppedAtLine);
          }
          ojson stopNotice;
          stopNotice["type"] = "echo";
          stopNotice["text"] = "do-file stopped" + linepart + " — " + *fres.result.stoppedAtReason;
          out.push_back(stopNotice);
        }
        return out;
      }
    }
  }

  // ------------------------------------------------------------------
  // Pre-0. `capture`/`quietly`/`noisily` prefix stripping (dispatcher.py's
  // same-named step, items 4-5). Must run BEFORE step (a) continue-sentinel
  // and 0a try_capture_definition so a prefixed line's remainder is what
  // those steps see.
  {
    // BS-016: cap/capt/captu/captur/capture are all legal Stata
    // abbreviations of `capture` (previously only cap/capture were
    // recognised, silently rejecting captu/captur/capt as unknown commands).
    static const std::set<std::string> kCapturePrefixes = {"capture", "captur", "captu", "capt",
                                                             "cap"};
    // N1(a) (BETA Phase C batch C1): Stata accepts n/no/noi/nois/noisi/
    // noisil/noisily as legal abbreviations of `noisily` — this set
    // previously had only {noisily, nois, n}, missing no/noi/noisi/noisil.
    // `noi` is by far the single most common spelling in practice
    // (`cap noi <cmd>`); its absence meant `cap noi X` dispatched "noi" as
    // an unknown command, whose error capture then swallowed X's entire
    // output (rc=199, zero output) — exactly the "cap noi suppresses
    // everything" symptom the C1 probes showed. The isQuietly classifier
    // just below still only lists the QUIETLY-class spellings explicitly;
    // every newly-added noisily-class spelling correctly falls to its
    // `else` (noisily) branch by construction, so no change needed there.
    static const std::set<std::string> kQuietlyPrefixes = {"quietly", "qui",    "quie",   "quiet",
                                                             "noisily", "noisil", "noisi",  "nois",
                                                             "noi",     "no",     "n"};

    // N1(b): does `remainder`'s OWN leading prefix chain hold a noisily?
    // Real Stata: bare `capture cmd` suppresses ALL output — success or
    // error; `capture noisily cmd` shows it (both on success and on error).
    // BUG-1434: a noisily anywhere in the chain after the capture shows the
    // error (StataBE 19.5: `capture quietly noisily error 460` prints it), and
    // the inner dispatch has already decided the output by the chain's last
    // word (`capture noisily quietly display "a3"` prints nothing).
    auto isNoisilyRooted = [](const std::string& remainder) {
      static const std::set<std::string> kNoisilyClass = {"noisily", "noisil", "noisi", "nois",
                                                            "noi",     "no",     "n"};
      static const std::set<std::string> kQuietOrCapture = {
          "quietly", "qui", "quie", "quiet", "capture", "captur", "captu", "capt", "cap"};
      std::string rest = trimWs(remainder);
      while (!rest.empty()) {
        size_t sp = rest.find_first_of(" \t");
        std::string tok = lower(rstripColon(sp == std::string::npos ? rest : rest.substr(0, sp)));
        if (kNoisilyClass.count(tok) != 0) return true;
        if (kQuietOrCapture.count(tok) == 0 || sp == std::string::npos) return false;
        rest = trimWs(rest.substr(sp));
      }
      return false;
    };

    // BUG-526 (W2 labelglued; oracle w2_cmdtriage.log section D): a prefix
    // delivered THROUGH a macro. `local q quietly` + `` `q' summarize a `` is
    // rc 0 with r(N)=12 (macro_quietly); `global qui "qui"` + `${qui} generate
    // double mq = 1` is rc 0 (global_qui); `local cp capture` + `` `cp' drop
    // nosuchvar `` is rc 0 where the bare `drop nosuchvar` is r(111)
    // (macro_capture against ctl_drop_missing). Stata expands macros before it
    // looks for the prefix; this block ran on the RAW line, missed, and step
    // 0b's expansion then handed the tokenizer a command literally named
    // `quietly` -> r(199), an uncaptured stop (paper 073 alone carries 2,981
    // `${qui} cmd` lines).
    //
    // Only the HEAD token is expanded here, and the line re-enters dispatch
    // only when that head became a capture/quietly/noisily word. The rest of
    // the line stays raw and is expanded exactly once, by the inner dispatch's
    // own step 0b -- so a macro in the remainder is not expanded twice, and a
    // head that expands to an ordinary command, or to nothing, keeps today's
    // path (0b expands the whole line and the command runs). The inner echo
    // is dropped: Stata logs the line as typed, and `blocks` already holds it.
    {
      const std::string rawTrim = trimWs(rawCmd);
      if (!rawTrim.empty() && (rawTrim[0] == '`' || rawTrim[0] == '$')) {
        const size_t sp = rawTrim.find_first_of(" \t");
        const std::string head = sp == std::string::npos ? rawTrim : rawTrim.substr(0, sp);
        const std::string rest = sp == std::string::npos ? std::string() : rawTrim.substr(sp);
        if (eval::hasMacroRefs(head)) {
          eval::ExpandResult er = eval::expandMacros(head, session.eval);
          if (!er.isError) {
            const std::string expHead = trimWs(er.line);
            const size_t hs = expHead.find_first_of(" \t");
            const std::string word =
                lower(rstripColon(hs == std::string::npos ? expHead : expHead.substr(0, hs)));
            if (!word.empty() &&
                (kCapturePrefixes.count(word) != 0 || kQuietlyPrefixes.count(word) != 0)) {
              ojson inner = dispatchImpl(expHead + rest, session);
              if (inner.is_array()) {
                for (const auto& b : inner) {
                  if (b.is_object() && b.value("type", std::string()) == "echo") continue;
                  blocks.push_back(b);
                }
              } else if (!inner.is_null()) {
                blocks.push_back(inner);
              }
              return blocks;
            }
          }
        }
      }
    }

    std::optional<std::string> capRemainder;
    {
      dprof::Scope __z(dprof::kPrefixStrip);
      static const PrefixTable kCaptureTable(kCapturePrefixes);
      capRemainder = stripPrefix(rawCmd, kCaptureTable);
    }
    if (capRemainder.has_value()) {
      ojson innerBlocks = dispatch(*capRemainder, session);
      // BUG-116 class (A11 sweep, oracle-verified): a NESTED `capture` that
      // already failed tags its error block captured:true (BS-127) — that
      // statement, AS A COMMAND, still completed without escaping an error
      // (capture never propagates one), so it must not read as THIS
      // capture's own wrapped-command error. Confirmed against real Stata
      // (StataBE 19.5): `capture capture noisily regress y nosuchvar`
      // leaves _rc==0, not the inner regress's rc — the same
      // !captured guard doexec.cpp/loops.cpp's runCaptureBlock use.
      const ojson* errBlock = nullptr;
      for (const auto& b : innerBlocks) {
        if (b.value("type", "") == "error" && !b.value("captured", false)) {
          errBlock = &b;
          break;
        }
      }
      // `exit` with code 0 is no error, and capture does not hold it: StataBE
      // `capture exit 0` ends the do-file (or the program) all the same.
      if (errBlock != nullptr && errBlock->value("message", std::string()) == "exit") {
        return ojson::array({echo, *errBlock});
      }
      // BS-100: coarse Stata rc map (piet::eval::coarseRc) replaces the old
      // boolean-collapsed "_rc=1" hardcode — see errors.h's doc comment for
      // the mapping + BS-118's post-beta exact-code scope.
      session.eval.locals.set("_rc", errBlock != nullptr ? eval::coarseRc(*errBlock) : "0");

      // N1(b): bare `capture cmd` suppresses ALL output regardless of
      // success/failure; `capture noisily cmd` shows it regardless of
      // success/failure. Previously the success path always forwarded
      // output (ignoring noisily) and the error path always suppressed it
      // (also ignoring noisily) — backwards for `capture noisily cmd`,
      // which must show output on BOTH outcomes.
      if (isNoisilyRooted(*capRemainder)) {
        ojson out = ojson::array({echo});
        for (const auto& b : innerBlocks) {
          if (b.value("type", "") != "echo") {
            // BS-127: an error block that reaches the caller under a
            // `capture` prefix is SHOWN (noisily) but must not HALT a
            // running do-file — real Stata shows the message, sets _rc,
            // and continues. doexec's stop-on-first-error loop skips
            // blocks carrying this tag.
            if (b.value("type", "") == "error") {
              // `exit N`'s halt sentinel is no message: StataBE `capture
              // noisily exit 7` prints nothing and leaves _rc 7.
              static const std::regex reExitSentinel(R"(^exit(\s+-?\d+)?$)");
              if (std::regex_match(b.value("message", std::string()), reExitSentinel)) continue;
              ojson tagged = b;
              tagged["captured"] = true;
              out.push_back(tagged);
              continue;
            }
            out.push_back(b);
          }
        }
        return out;
      }
      // BUG-350: a plain `capture` drops the error block entirely — that is
      // correct Stata behaviour and stays. What was missing is that NOTHING
      // recorded it, so a run could hide a disk-full and still look complete.
      // Count it here, at the one place it is dropped, and keep the first
      // message for the end-of-run summary.
      // BUG-1338: the message of an inline `: fn'/`=exp' that failed while the
      // capture line itself expanded still prints (StataBE prints `variable
      // nosuch not found` for `capture display "C`: type nosuch'"`, and for
      // `capture quietly display ...`); one from a line run inside the captured
      // command does not (`capture myq`, whose body expands `=nosuchZ', is
      // silent). That line is the remainder itself or the tail of its prefix
      // chain; a program's body line is neither.
      const std::string capLine = trimWs(*capRemainder);
      const auto isOwnLineNote = [&capLine](const ojson& b) {
        if (!b.is_object() || !b.value("inline_note", false)) return false;
        const std::string of = trimWs(b.value("inline_note_of", std::string()));
        if (of.empty() || of.size() > capLine.size()) return false;
        if (capLine.compare(capLine.size() - of.size(), of.size(), of) != 0) return false;
        if (of.size() == capLine.size()) return true;
        const char before = capLine[capLine.size() - of.size() - 1];
        return before == ' ' || before == '\t' || before == ':';
      };
      ojson out = ojson::array({echo});
      for (const auto& b : innerBlocks) {
        if (b.is_object() && b.value("type", std::string()) == "error") {
          piet::counters::noteSwallowedError(b.value("message", std::string()));
        }
        if (isOwnLineNote(b)) out.push_back(b);
      }
      return out;
    }

    std::optional<std::string> quiRemainder;
    {
      dprof::Scope __z(dprof::kPrefixStrip);
      static const PrefixTable kQuietlyTable(kQuietlyPrefixes);
      quiRemainder = stripPrefix(rawCmd, kQuietlyTable);
    }
    if (quiRemainder.has_value()) {
      std::string firstWord = trimWs(rawCmd);
      size_t sp2 = firstWord.find_first_of(" \t");
      std::string fw = lower(rstripColon(sp2 == std::string::npos ? firstWord : firstWord.substr(0, sp2)));
      // N1(a): this classifier only needs to list the QUIETLY-class
      // spellings explicitly — every noisily-class spelling (including the
      // ones added to kQuietlyPrefixes above: no/noi/noisi/noisil) already
      // falls to the `else` (noisily) branch below by construction, so the
      // expanded abbreviation set needs no change here.
      bool isQuietly = (fw == "quietly" || fw == "qui" || fw == "quie" || fw == "quiet");
      // BUG-189: same reason as the braced form — the filters below run after
      // the inner dispatch has already teed to `log using`, so the scope must be
      // declared around the dispatch itself, not applied to its result.
      ojson innerBlocks;
      {
        SessionState::LogVerbosityScope __lv(session, isQuietly);
        innerBlocks = dispatch(*quiRemainder, session);
      }
      ojson out = ojson::array({echo});

      // BS-040: if the remainder is ITSELF another quietly/noisily-prefixed
      // command, the recursive dispatch() call above has ALREADY resolved
      // its effective visibility — Stata nests these prefixes with the
      // INNERMOST keyword winning (`quietly noisily cmd` must show cmd's
      // output; the reverse must hide it). Re-applying THIS layer's own
      // filter on top of an already-resolved inner result would let an
      // outer `quietly` re-hide what an inner `noisily` correctly chose to
      // show (the exact bug). Detect that case and pass the inner result
      // through untouched instead — only the INNERMOST layer in a chain
      // (whose own remainder is NOT itself quiet/noisy-prefixed) actually
      // applies a filter, via the unchanged isQuietly branch below.
      // BUG-1434: a capture inside the chain resolves the rest the same way
      // (`quietly capture noisily display "a2"` prints a2, StataBE 19.5).
      static const PrefixTable kQuietlyTable2(kQuietlyPrefixes);
      static const PrefixTable kCaptureTable2(kCapturePrefixes);
      if (stripPrefix(*quiRemainder, kQuietlyTable2).has_value() ||
          stripPrefix(*quiRemainder, kCaptureTable2).has_value()) {
        for (const auto& b : innerBlocks) out.push_back(b);
        loops::settleNoisily(out, session);
        return out;
      }

      // BUG-189: hand this layer's verdict to the log tee one frame out. Only
      // the innermost layer reaches here (the chained case returned above
      // without touching the flag), so what the tee sees is the keyword that
      // actually decided visibility.
      session.setLineVisibility(isQuietly ? SessionState::LineVis::Quiet
                                          : SessionState::LineVis::Noisy);
      if (isQuietly) {
        for (const auto& b : innerBlocks) {
          std::string t = b.value("type", "");
          if (t == "error" || t == "echo" || b.value("inline_note", false) ||
              b.value("noisily", false))
            out.push_back(b);
        }
        loops::settleNoisily(out, session);
      } else {
        for (const auto& b : innerBlocks) {
          if (b.value("type", "") != "echo") out.push_back(b);
        }
        // Inside a quiet scope this is a nested noisily's output (BUG-1434).
        loops::markNoisily(out, session);
      }
      return out;
    }
  }

  // ------------------------------------------------------------------
  // Pre-0b. `eststo [NAME] [, options]: command` (BS-020/BS-095) — same
  // unified-prefix philosophy as capture/quietly/noisily above (recognise on
  // the raw line, dispatch the remainder exactly once, apply this prefix's own
  // post-processing): run the remainder FIRST so handlers::runEststo
  // (eststo_esttab.cpp) snapshots the FRESH result, then store it.
  //
  // W2 eststoprefix (BUG-520). This arm used to be the regex
  // `^eststo\s+(\w+)\s*:` on the RAW line, which is blind to everything but a
  // literal name. Three shapes fell past it to the flat `eststo` arm, which
  // never dispatches a remainder:
  //   eststo: regress y w              bare -- one of the commonest corpus lines
  //   eststo mac`i': regress y w       a macro in NAME (unexpanded here: `\w+`
  //                                    stops at the backtick)
  //   eststo t0, title("x"): regress   options before the colon
  // With inner options the flat arm refused the INNER command's first option,
  // r(198). Without them it stored the STALE previous model under a corrupted
  // key (`regress`, `mac3:`) at rc 0 and never ran the command -- silent wrong.
  // Oracle, tests/eval/golden/aer_w2/w2_eststo_prefix.log: A2 bare, A3/A4 local
  // in NAME (A4 in a loop, inner `vce(cluster id)`), A5 global in NAME, A6
  // options before the colon incl. a QUOTED colon, A7 prefix(), A8 a failing
  // inner command stores nothing and returns its own rc.
  //
  // So the split is structural now: first word `eststo`, then the first colon
  // outside quotes and outside a macro reference. Macros are expanded in the
  // HEAD ONLY. The remainder goes to dispatch() raw, because dispatch expands it
  // itself (step 0b) and expanding twice is a bug of its own (`\`'-deferred
  // macros, `++i'). Head first, remainder second is also Stata's left-to-right
  // order, which `eststo m`++i': regress y x`i'` can observe.
  //
  // The no-colon forms -- `eststo`, `eststo NAME`, `eststo dir`, `eststo
  // drop|clear [NAME]` -- never enter here, and dir/drop/clear in the NAME
  // position are still left to the flat arm (which now refuses the colon shape
  // by name instead of storing under it).
  {
    dprof::Scope __z(dprof::kColonPrefix);
    const std::string trimmedRaw = trimWs(rawCmd);
    // A3: all three colon-prefix forms require a literal ':'.
    const size_t wordEnd = trimmedRaw.find(':') == std::string::npos
                               ? std::string::npos
                               : trimmedRaw.find_first_of(" \t,:");
    if (wordEnd != std::string::npos && lower(trimmedRaw.substr(0, wordEnd)) == "eststo") {
      const std::string rest = trimmedRaw.substr(wordEnd);
      const size_t colonPos = findEststoPrefixColon(rest);
      std::string headRaw =
          colonPos == std::string::npos ? std::string() : trimWs(rest.substr(0, colonPos));
      // Checked on the RAW head, before any expansion, so a line this arm hands
      // back to the flat arm has had no macro side effect from here.
      const size_t subEnd = headRaw.find_first_of(" \t,");
      const std::string sub0 = lower(headRaw.substr(0, subEnd));
      if (colonPos != std::string::npos && sub0 != "dir" && sub0 != "drop" && sub0 != "clear") {
        const std::string eststoRemainder = trimWs(rest.substr(colonPos + 1));
        // The community-name gate (WS-B) sits after tokenize, which this arm
        // returns before -- so it is asked here. Without this, `set extcompat
        // off` gated `eststo m1` and let `eststo m1: regress y x` through (it
        // always had, for the literal-name form; bare `eststo:` would have made
        // that the commonest line in the corpus). The INNER command meets the
        // gate on its own, inside its own dispatch.
        if (!settings::extcompatEnabled()) {
          if (const registry::CommunityName* cn = registry::findCommunityName("eststo")) {
            blocks.push_back(ojson(eval::errorBlock(
                "unsupported_command", registry::communityGateMessage(*cn), std::string("eststo"))));
            return blocks;
          }
        }
        if (eval::hasMacroRefs(headRaw)) {
          eval::ExpandResult er = eval::expandMacros(headRaw, session.eval);
          if (er.isError) {
            blocks.push_back(er.error);
            return blocks;
          }
          headRaw = er.line;
        }
        // The head is parsed and validated BEFORE the command runs, as
        // eststo.ado's `syntax` does: a bad head is r(198) and nothing ran.
        auto [namePart, optsPart] = splitTopLevelComma(headRaw);
        std::vector<std::string> nameToks;
        {
          std::istringstream ns(namePart);
          std::string tk;
          while (ns >> tk) nameToks.push_back(tk);
        }
        // `\w+`, exactly what the regex this replaces accepted -- no oracle arm
        // measures an invalid name, so the accepted set is neither widened nor
        // narrowed here. An EMPTY name part is the bare form (A2). A macro that
        // expands to nothing mid-name (`est`undefined'` -> `est`) is a legal name.
        // A byte >= 0x80 is a name byte too (core/ident.h): `eststo نموذج: ...`.
        static const std::regex reEststoName(std::string("^") + eval::kRxNameChar + "+$");
        if (nameToks.size() > 1 ||
            (nameToks.size() == 1 && !std::regex_match(nameToks[0], reEststoName))) {
          blocks.push_back(ojson(eval::engineError(
              "eststo: `" + trimWs(namePart) + "` is not one name -- the prefix form is "
              "`eststo [name] [, options]: command`",
              "syntax_error")));
          return blocks;
        }
        EststoStoreOpts storeOpts;
        if (optsPart.has_value()) {
          std::vector<std::pair<std::string, eval::OptValue>> headOpts;
          try {
            headOpts = eval::parseOpts(trimWs(*optsPart));
          } catch (const eval::TokenizerError& te) {
            blocks.push_back(ojson(te.block));
            return blocks;
          }
          // The head never reaches the tokenizer's resolution point, so it gets
          // the same canonicalization here -- otherwise `ti("x")` would pass the
          // net below and then miss readEststoStoreOpts' `k == "title"` (BUG-231's
          // mechanism), and the BUG-233 late-resolution guard would count it.
          eval::canonicalizeForCommand(headOpts, "eststo");
          if (auto err = readEststoStoreOpts(headOpts, &storeOpts)) {
            blocks.push_back(*err);
            return blocks;
          }
        }
        ojson out = ojson::array({echo});
        bool hasError = false;
        // `eststo NAME:` with nothing after the colon is the flat store --
        // eststo.ado only runs a command when `s(after)` is non-empty.
        if (!eststoRemainder.empty()) {
          ojson innerBlocks = dispatch(eststoRemainder, session);
          for (const auto& b : innerBlocks) {
            // is_object() first: value() on anything else throws type_error.306
            // and takes the whole process with it.
            const std::string t = b.is_object() ? b.value("type", std::string()) : std::string();
            if (t == "error") hasError = true;
            if (t != "echo") out.push_back(b);
          }
        }
        // A8: a failing inner command stores NOTHING (`estimates table bad1` is
        // r(111) afterwards) and the line's rc is the inner command's own. The
        // auto-name counter is not consumed either: the bare store after two
        // failures is est10, the very next number.
        if (!hasError) {
          std::optional<std::string> eststoNameOpt;
          if (!nameToks.empty()) eststoNameOpt = nameToks[0];
          out.push_back(ojson(handlers::runEststo(eststoNameOpt, storeOpts.title, session,
                                                  storeOpts.autoPrefix)));
          pushEstStoreMutation(out.back(), out, session);
        }
        return out;
      }
    }
  }

  // ------------------------------------------------------------------
  // Pre-0c. `version #: command` (BS-041/BS-095) — same unification as
  // eststo NAME: above. Bare `version #` (no colon) is the flat if-chain's
  // `cmd=="set"||cmd=="version"||cmd=="compress"` arm (BUG-1459). This block only
  // fires when a colon+remainder is actually present, which today silently
  // vanishes in its entirety (the whole line no-ops, remainder included —
  // e.g. `version 14: regress y x` runs nothing and reports nothing).
  // `version` has no suppression semantics of its own, so on success this
  // is a transparent passthrough (mirrors `noisily`'s shape: drop the
  // remainder's own echo, keep everything else, including any error).
  {
    dprof::Scope __z(dprof::kColonPrefix);
    // BUG-1330: `version #, user: command` is the prefix too; the options sit
    // between the number and the colon.
    static const std::regex reVersionColon(R"(^version\s+([^\s,:]+)\s*(?:,([^:]*))?:\s*(.+)$)",
                                           std::regex::icase);
    std::smatch vm;
    std::string trimmedRawV = trimWs(rawCmd);
    // A3: all three colon-prefix forms require a literal ':'.
    if (trimmedRawV.find(':') != std::string::npos &&
        std::regex_search(trimmedRawV, vm, reVersionColon)) {
      std::string versionRemainder = trimWs(vm.str(3));
      if (!versionRemainder.empty()) {
        // BUG-1015: the inner command runs under that version (a command whose
        // syntax depends on it reads session.callerVersion: table's c()).
        // BUG-1330: a number `version #` refuses is refused here too
        // (`version 0.5: ...` r(9), `version 1e1: ...` r(198)).
        // The line is not expanded yet: the number and options are, here
        // (`version `v': ...` in a loop).
        std::string head[2] = {vm.str(1), vm.str(2)};
        for (std::string& part : head) {
          if (!eval::hasMacroRefs(part)) continue;
          eval::ExpandResult er = eval::expandMacros(part, session.eval);
          if (er.isError) return ojson::array({echo, er.error});
          part = trimWs(er.line);
        }
        double vnum = 0.0;
        if (auto bad = parseVersionNumber(head[0], &vnum)) return ojson::array({echo, *bad});
        bool user = false;
        std::istringstream opts(head[1]);
        for (std::string t; opts >> t;) user = user || t == "user";
        // BUG-1330: and c(version) reports it there (kVersionLocal), the
        // caller's coming back after; c(userversion) too, except where
        // `version #` leaves it (versionSetsUserVersion). With `user` only
        // c(userversion) is that number, in a program too, and the version in
        // force stays (StataBE 19.5: P1|15|16 under `version 15`, then 15|15;
        // `version 16, user: table ..., contents()` is still r(198)).
        struct VersionScope {
          // session.callerVersion (BUG-1459) is what table reads; the locals are c()'s.
          double saved;
          SessionState& session;
          std::optional<std::string> savedLocal[2];
          VersionScope(double v, bool user, SessionState& s) : saved(s.callerVersion), session(s) {
            const char* const names[2] = {kVersionLocal, kUserVersionLocal};
            for (int k = 0; k < 2; ++k)
              if (const std::string* cur = session.eval.locals.find(names[k])) savedLocal[k] = *cur;
            if (!user) {
              session.callerVersion = v;
              session.eval.locals.set(kVersionLocal, eval::fmtNumeric(v));
            }
            if (user || versionSetsUserVersion(session))
              session.eval.locals.set(kUserVersionLocal, eval::fmtNumeric(v));
          }
          ~VersionScope() {
            session.callerVersion = saved;
            const char* const names[2] = {kVersionLocal, kUserVersionLocal};
            for (int k = 0; k < 2; ++k) {
              if (savedLocal[k])
                session.eval.locals.set(names[k], *savedLocal[k]);
              else
                session.eval.locals.erase(names[k]);
            }
          }
        } versionScope(vnum, user, session);
        ojson innerBlocks = dispatch(versionRemainder, session);
        ojson out = ojson::array({echo});
        for (const auto& b : innerBlocks) {
          if (b.value("type", "") != "echo") out.push_back(b);
        }
        return out;
      }
    }
  }

  // ------------------------------------------------------------------
  // P7 — the `svy:` prefix. Intercepted here, AFTER macro expansion would have
  // happened for the resampling prefixes but structurally alongside the other
  // simple `KEYWORD: command` forms (eststo/version): svy carries no exp_list,
  // so nothing has to be deferred per-replicate and the ordinary expansion
  // order is correct. The design itself lives in SessionState (svyset).
  {
    dprof::Scope __z(dprof::kColonPrefix);
    static const std::regex reSvyColon(R"(^\s*svy\s*(?:,[^:]*)?:\s*(.+)$)",
                                        std::regex::icase);
    std::smatch sm;
    std::string trimmedRawS = trimWs(rawCmd);
    // A3: all three colon-prefix forms require a literal ':'.
    if (trimmedRawS.find(':') != std::string::npos &&
        std::regex_search(trimmedRawS, sm, reSvyColon)) {
      blocks.push_back(ojson(handlers::runSvyPrefix(trimWs(sm.str(1)), session)));
      return blocks;
    }
  }

  // ------------------------------------------------------------------
  // (a) `continue` / `continue, break` (U7/S11) — loop-control bubble,
  // intercepted before any keyword handler. Normalise by removing ALL
  // whitespace so "continue, break" / "continue ,break" / "continue,break"
  // collapse to the same token.
  {
    dprof::Scope __z(dprof::kSmallSteps);
    std::string noSpace;
    for (char c : lower(trimWs(rawCmd))) {
      if (c != ' ' && c != '\t') noSpace += c;
    }
    if (noSpace == "continue" || noSpace == "continue,break") {
      // BUG-483. Inside a loop this is the iteration control it has always
      // been. OUTSIDE every loop it used to return the same sentinel, which
      // doexec turned into `out.control` and an immediate return — the silent
      // end of the do-file, at rc 0, with no stopped_at_line. doexec.h's own
      // contract already said a top-level run "never returns this to its
      // caller"; the code did.
      //
      // Stata, measured (tests/eval/golden/bug483_484_control_oracle.log):
      // `continue` and `continue, break` outside a loop print "statement out of
      // context", set r(119), and the next statement runs. The rc travels in
      // `reason` because coarseRc has no rule that would reach 119 from this
      // message (BUG-187: reason is the only channel carrying an exact rc).
      //
      // BUG-920: a program or nested do/run file run from a loop body starts
      // outside any loop: its `continue` is out of context too (oracle
      // w2ad_prog_preserve_nest pn_lm_cb_prog, pn_lm_cb_do: r(119) at every
      // iteration, and the caller's loop runs on).
      if (loops::loopBodyDepth() <= session.loopBodyFloor) {
        return ojson::array({echo, ojson(eval::errorBlock("engine_error",
                                                          "statement out of context", std::nullopt,
                                                          std::nullopt, std::nullopt, "r(119)"))});
      }
      ojson ctrl;
      ctrl["type"] = "_control";
      ctrl["action"] = (noSpace.find("break") != std::string::npos) ? "break" : "continue";
      return ojson::array({echo, ctrl});
    }
    // BUG-483, the other half. A bare `break` never matched here, so it fell
    // through to the generic arms and came back "No dataset is loaded…" — not
    // silent, so not the headline, but not Stata either. `break` IS a real
    // command (`which break` is rc 0 while `brea` is r(199), so it is also not
    // abbreviable) and it is a NO-OP: rc 0 and nothing printed, measured both
    // at top level and inside forvalues/while/foreach, where it does NOT exit
    // the loop (5 of 5 iterations; `continue, break` is the exit and gives 3).
    // Treating the two keywords as one case would therefore be wrong in one
    // direction whichever case was picked.
    if (noSpace == "break") {
      return ojson::array({echo});
    }
  }

  // ------------------------------------------------------------------
  // (b) `args [name1 name2 …]` (S3/U3-args) — program bodies open with this.
  // Stata: `args a b c` == `local a `1'` / `local b `2'` / `local c `3'`.
  // Silent (no console output), Stata-faithful.
  {
    dprof::Scope __z(dprof::kSmallSteps);
    std::string argsStripped = trimWs(rawCmd);
    if (lower(argsStripped).rfind("args", 0) == 0 &&
        (argsStripped.size() == 4 || argsStripped[4] == ' ' || argsStripped[4] == '\t')) {
      std::istringstream namesIss(argsStripped.substr(4));
      std::string name;
      int idx = 1;
      while (namesIss >> name) {
        session.eval.locals.set(name, session.eval.locals.get(std::to_string(idx), ""));
        ++idx;
      }
      return ojson::array();
    }
  }

  // ------------------------------------------------------------------
  // (c) `gettoken TOK REST : SRC [, options]` (U4) — intercepted before step
  // 0b (mirrors dispatcher.py's step-0 (c): `_u4_first == "gettoken"`); the
  // line's own macros are expanded below, as Stata does.
  // Silent ([]) on success; [echo, error] on a syntax-error failure.
  {
    dprof::Scope __z(dprof::kSmallSteps);
    std::string u4Stripped = trimWs(rawCmd);
    size_t u4Sp = u4Stripped.find_first_of(" \t");
    std::string u4First = lower(u4Sp == std::string::npos ? u4Stripped : u4Stripped.substr(0, u4Sp));
    if (u4First == "gettoken") {
      // BUG-1499: Stata expands the line's macros before gettoken reads it
      // (StataBE 19.5: parse("`pc'") parses on the local's characters and
      // `gettoken `tn' r : s` binds the local `tn' names).
      std::string gtLine = rawCmd;
      if (eval::hasMacroRefs(rawCmd)) {
        eval::ExpandResult ex = eval::expandMacros(rawCmd, session.eval);
        if (ex.isError) return ojson::array({echo, ex.error});
        gtLine = ex.line;
      }
      eval::LocalsStore store;
      store.get = [&session](const std::string& name) -> std::string {
        return session.eval.locals.get(name, "");
      };
      store.set = [&session](const std::string& name, const std::string& value) {
        session.eval.locals.set(name, value);
      };
      store.getGlobal = [&session](const std::string& name) -> std::string {
        return session.eval.globals.get(name, "");
      };
      store.setGlobal = [&session](const std::string& name, const std::string& value) {
        session.eval.globals.set(name, value);
      };
      std::vector<nlohmann::json> gtBlocks = eval::runGettoken(gtLine, store);
      if (!gtBlocks.empty()) {
        ojson out = ojson::array({echo});
        for (const auto& b : gtBlocks) out.push_back(ojson(b));
        return out;
      }
      return ojson::array();  // silent success
    }
  }

  // ------------------------------------------------------------------
  // (c)/(d) `tempfile NAME...` / `tempname NAME...` / `tempvar NAME...`
  // (U4; tempvar added BS-037, BETA campaign C3) — run on the RAW
  // (pre-expansion) line so the names they bind are literal identifiers, not
  // macro refs. Silent ([]) on success; [echo, ...errorBlocks] on failure —
  // mirrors dispatcher.py's `if _gt_blocks: blocks.extend(...); return blocks`
  // / `return []` split. Running BEFORE tokenize() (below) is exactly what
  // makes `tempvar` safe here: eval/tokenizer.cpp's early macro-statement
  // keyword net (Step 1c) still lists "tempvar" among its unconditional
  // "unsupported_macro" rejects — this U4 block intercepts the line first,
  // so tokenize() never sees it, identical to how tempfile/tempname already
  // bypass that same net today.
  // AER-SCOPE1 bundle D: `!cmd` is Stata's shorthand for `shell cmd`. Rewritten
  // here rather than given its own arm because `!` is not a word the tokenizer
  // can carry to a registry lookup, and because doing it before step 0b means
  // the macros inside (`!rm -f $path/`pattern'`, the corpus's actual spelling)
  // expand exactly as they would after `shell`.
  {
    static const std::regex reBangShell(R"(^\s*!\s*(\S.*)$)");
    std::smatch bm;
    if (std::regex_match(rawCmd, bm, reBangShell)) {
      return piet::dispatch("shell " + bm[1].str(), session);
    }
  }

  {
    dprof::Scope __z(dprof::kSmallSteps);
    ojson u4Out;
    if (tryTempDeclIntercept(rawCmd, session, echo, u4Out)) return u4Out;
    // AER tier-1: `macro drop|define|shift|list|dir`. `macro` is in the
    // tokenizer's unsupported_macro net, so it is consumed here, before it.
    // A line still carrying `$`/a backtick is left for the expanded site below.
    if (handlers::tryMacroVerbIntercept(rawCmd, session, echo, u4Out, false)) return u4Out;
  }

  // ------------------------------------------------------------------
  // (e) `return scalar`/`return local` (U5/S7) — must run BEFORE
  // try_capture_definition so `return scalar n = ...` isn't consumed as a
  // macro definition. Staged into session.returnStaging — silent on success.
  {
    dprof::Scope __z(dprof::kSmallSteps);
    ojson returnErr;
    if (program::handleReturnStmt(rawCmd, session, returnErr)) {
      if (!returnErr.empty()) {
        blocks.push_back(returnErr);
        return blocks;
      }
      return ojson::array();
    }
  }

  // ------------------------------------------------------------------
  // (f) `program drop NAME` / `program dir` ("0a-bis") — begins with the
  // `program` keyword the tokenizer rejects, so intercepted here on the raw
  // line, pre-expansion. `program define … end` is captured pre-tokenizer
  // by doexec's program::captureProgams and never reaches dispatch as a
  // bare line.
  {
    dprof::Scope __z(dprof::kSmallSteps);
    std::string progLine = trimWs(rawCmd);
    std::istringstream progIss(progLine);
    std::string progTok0;
    progIss >> progTok0;
    // BUG-434: `prog drop NAME` is `program drop NAME`. The abbreviation is not
    // in the registry — `program` has no row there (it is intercepted before
    // tokenizing), so registry::resolveCanonical cannot help — it lives in
    // program.cpp's own `^prog(?:ram)?` head regexes, which is why `prog def`
    // worked and this arm, matching the full spelling only, answered rc 199 for
    // the drop. One sub-verb honoured the abbreviation and its sibling did not,
    // and the failure said nothing about the spelling. Unguarded it halts the
    // do-file, which is where a corpus package stopped.
    //
    // The accepted set is program.cpp's, deliberately: two spellings of the
    // same head in two files is how they drift apart.
    const std::string progHead = lower(progTok0);
    if (progHead == "program" || progHead == "prog") {
      std::string progSub;
      progIss >> progSub;
      std::string progSubLower = lower(progSub);
      if (progSubLower == "drop" || progSubLower == "dir") {
        std::vector<std::string> progNames;
        std::string nm;
        while (progIss >> nm) progNames.push_back(nm);
        ojson mgmtBlock = program::handleProgramManage(progSubLower, progNames, session);
        if (mgmtBlock.value("type", "") == "error") {
          // BUG-923: `program drop NAME` for an absent program is r(111), as in
          // Stata (oracle w2_prog_controlflow pc_pg_capdrop_absent). It used
          // to be a benign no-op here (Python parity) that `capture` read as 0.
          return ojson::array({echo, mgmtBlock});
        }
        if (mgmtBlock.contains("text") && progSubLower == "dir") {
          return ojson::array({echo, mgmtBlock});
        }
        return ojson::array({echo});
      }
      // BUG-923: a `program define NAME` that reaches dispatch was not taken
      // by the capture pass (program::captureProgams matches only a line that
      // STARTS with the header, so a prefixed `capture noisily program define
      // NAME` lands here). Defining an existing program without dropping it is
      // r(110) in Stata and leaves the old definition in place (oracle
      // pc_pg_redefine: 110, and the old pc_add still runs). Only that case is
      // answered here; any other define form keeps its old reply below.
      {
        std::string defName;
        if (progSubLower == "define" || progSubLower == "def" || progSubLower == "de") {
          progIss >> defName;
        } else if (!progSubLower.empty() && progSubLower != "list") {
          defName = progSub;
        }
        while (!defName.empty() && defName.back() == ',') defName.pop_back();
        if (const size_t comma = defName.find(','); comma != std::string::npos) {
          defName.erase(comma);
        }
        if (!defName.empty()) {
          if (std::optional<std::string> existing = program::findProgram(session, defName)) {
            return ojson::array({echo, ojson(eval::errorBlock(
                                           "engine_error", "program " + *existing + " already defined",
                                           std::nullopt, std::nullopt, std::nullopt,
                                           std::string("r(110)")))});
          }
        }
      }
    }
  }

  // ------------------------------------------------------------------
  // (f-bis) AER-SCOPE1 bundle F: `input varlist` ... `end`. Like `program`,
  // its body is DATA rather than statements, so doexec folds the whole block
  // into ONE line at preprocess time (handlers::captureInputBlocks, 0x1F
  // separators -- a byte no Stata source contains). It is intercepted here on
  // the raw line for the same reason `program` is: the tokenizer would split
  // the folded rows on their commas and whitespace.
  //
  // The 0x1F test is what distinguishes a folded block from a bare `input`
  // typed with no block; the latter falls through to the registry arm below,
  // which answers honestly instead of silently doing nothing.
  {
    dprof::Scope __z(dprof::kSmallSteps);
    std::string inputLine = trimWs(rawCmd);
    // The head word ends at a blank OR at the fold separator: a bare `input`
    // (no varlist, FU-1251) folds to `input\x1F<row>...` with nothing between
    // (w2ar dx_inp_021_bare, log:4664: rows appended to the data in memory).
    const std::string inputTok0 = inputLine.substr(0, inputLine.find_first_of(" \t\x1F"));
    if (lower(inputTok0) == "input" && rawCmd.find('\x1F') != std::string::npos) {
      ojson r = handlers::runInput(rawCmd, session);
      if (r.is_array()) {
        ojson out = ojson::array({echo});
        for (const auto& b : r) out.push_back(b);
        return out;
      }
      return ojson::array({echo, r});
    }
  }

  // ------------------------------------------------------------------
  // (f-ter) ml-getmata T4: Mata. `mata: stmt` one-liners and the `mata:` ...
  // `end` block doexec folds into one line (mata::captureMataBlocks) run the
  // native M0 subset against the SEPARATE Mata store; `mata` without a colon,
  // and any statement outside M0, is refused by name. Intercepted on the RAW
  // line, before macro expansion and the tokenizer, for the reason `input` is:
  // Mata's `\`, `:-`, `[.,1]` and `"NAME"` are not Stata tokens. The handler
  // expands macros per statement itself (030's `(code \ `i')`). Silent on
  // success, so the empty array is FILTERED, never pushed (defect class 1).
  {
    dprof::Scope __z(dprof::kSmallSteps);
    if (mata::isMataLine(rawCmd)) {
      ojson r = mata::runMataLine(rawCmd, session);
      if (!(r.is_array() && r.empty())) blocks.push_back(r);
      return blocks;
    }
  }

  // ------------------------------------------------------------------
  // 0a. Session-state layer: capture macro definitions (local/global/scalar/
  // increment forms). A capture produces NO console output (Stata semantics)
  // — mirrors dispatcher.py's `if captured is not None: return []`.
  // BS-091/BS-101 (BETA A4): c(name) refs resolve to a literal BEFORE
  // tryCaptureDefinition parses this line — `scalar a = c(N)` / `local x =
  // c(N)` need the SAME single-source c()-registry gen/replace/if use
  // (piet::substituteCRefs, cclass.h). tryCaptureDefinition only sees the
  // narrower EvalSession (not full SessionState), so the substitution runs
  // HERE, at the one call site with full session access — rawCmd itself is
  // untouched (the substituted copy is used for THIS call only; every other
  // downstream use of rawCmd, e.g. macro expansion below, sees the original).
  // BUG-208: `local l : variable label x` is answered inside
  // tryCaptureDefinition, in the eval layer, off session.eval.varLabels — a map
  // whose only writer used to be the evalcase harness, so it read "" in real use
  // for a label `describe` was displaying at the same moment. That store is now a
  // PROJECTION of SessionState's live varMeta/card (see SessionState::
  // syncEvalVarLabels), and this is the one place it is refreshed: immediately
  // before the capture layer that consumes it, at the only call site with full
  // session access — the same reason substituteCRefs runs here.
  //
  // Gated on a deliberately cheap textual precondition, NOT run per command. The
  // sync resolves a varlist, and the MAXPERF verb-overhead selftest gates assert
  // that ordinary verbs construct ZERO Datastores; paying it on every dispatched
  // line would move them. The gate is a conservative superset (any line carrying
  // both a colon and "label"), so it can only ever do harmless extra work — it
  // cannot cause a miss, which is the direction that matters.
  // (`:` is tested first so the lowered copy is only ever made for the rare line
  // that carries one at all.)
  // BUG-216/217 widen the gate: `local f : format x` and `local t : value label x`
  // carry a colon but NOT the word "label" in the `: format` case, so the old
  // precondition could never fire for them — and without the sync the projection
  // is not authoritative, which is exactly the condition under which macros.cpp
  // refuses to decide r(111). Still a conservative SUPERSET of the old gate (the
  // "label" clause is kept verbatim and OR-ed), so it cannot cause a miss; the
  // added arm is narrow on purpose. It fires only for a line that BEGINS with
  // `local`/`global` and also carries a colon — i.e. precisely the extended-macro-
  // function definition shape — which deliberately excludes `by group:` and
  // `bysort x:` prefixes, the common colon-bearing lines that would otherwise pay
  // a projection rebuild per command and move the MAXPERF verb-overhead gates.
  if (rawCmd.find(':') != std::string::npos) {
    const std::string lowered = lower(rawCmd);
    const size_t h = lowered.find_first_not_of(" \t");
    const size_t head = (h == std::string::npos) ? 0 : h;
    // BUG-1381: the head is matched as Stata abbreviates it (loc/loca,
    // gl/glo/glob/globa), the spellings tryCaptureDefinition already answers --
    // `loc t : type v` read a never-synced plane and was r(101).
    const std::string headWord = lowered.substr(head, lowered.find_first_of(" \t", head) - head);
    static const std::unordered_set<std::string> kDefHeads{"loc", "loca", "local", "gl", "glo",
                                                            "glob", "globa", "global"};
    if (lowered.find("label") != std::string::npos ||
        lowered.compare(head, 5, "local") == 0 || lowered.compare(head, 6, "global") == 0 ||
        kDefHeads.count(headWord) != 0) {
      session.syncEvalVarLabels();
    }
  }
  {
    dprof::Scope __z(dprof::kCaptureDef);
    // The inline `` `: display ...' `` form, spliced in before anything reads
    // the line; the line is then dispatched again with the typed echo kept.
    if (rawCmd.find("`:") != std::string::npos) {
      std::optional<ojson> inlineErr;
      const std::string spliced = substituteInlineDisplay(rawCmd, session, inlineErr);
      if (inlineErr) {
        blocks.push_back(*inlineErr);
        return blocks;
      }
      if (spliced != rawCmd) {
        ojson inner = dispatchImpl(spliced, session);
        if (inner.is_array() && !inner.empty() && inner[0].is_object() &&
            inner[0].value("type", std::string()) == "echo")
          inner[0] = echo;
        return inner;
      }
    }
    {
      ojson cLocalOut;  // FU-602: `c_local NAME ...` writes the caller's scope
      if (tryCLocal(rawCmd, session, cLocalOut)) return cLocalOut;
    }
    {
      ojson extDisplayOut;  // BUG-513: `local NAME : display ...`
      if (tryExtDisplayCapture(rawCmd, session, extDisplayOut)) return extDisplayOut;
    }
    eval::MacroFnError mfe;
    if (eval::tryCaptureDefinition(substituteCRefs(rawCmd, session), session.eval, &mfe)) {
      // BUG-216/217: a consumed-but-FAILED extended macro function. `reason` is the
      // exact-code channel eval::coarseRc() honours (its BUG-187 note), so `_rc`
      // comes back as Stata's own 101/111 instead of the generic 1.
      if (mfe.failed()) {
        // BUG-339: carry the offending line as `context`. This return builds a
        // FRESH array, so the entry echo is dropped — and under `capture
        // noisily` the wrapper strips inner echoes too, which is exactly how
        // 160 of these per corpus run showed up in a log with nothing naming
        // their source. `context` costs nothing and is contract-legal.
        return ojson::array({ojson(eval::errorBlock("engine_error", mfe.message, std::nullopt,
                                                    std::nullopt, strutil::trim(rawCmd),
                                                    "r(" + mfe.rc + ")"))});
      }
      return ojson::array();
    }
  }

  // ------------------------------------------------------------------
  // 0a-bis. Resampling prefixes (P6) — intercepted BEFORE macro expansion.
  //
  // `bootstrap mean=r(mean), reps(30) seed(...): summarize y` carries an
  // expression list that must be evaluated ONCE PER REPLICATE, after that
  // replicate's inner command has posted its r()/e()/_b[]. Step 0b below would
  // substitute those references here and now — against whatever the PREVIOUS
  // command left, or erroring as unset — which is a different question from the
  // one the user asked. So the exp_list is carried verbatim into the driver;
  // the options and the inner command ARE expanded (they legitimately hold
  // locals, e.g. `reps(`R')` or `: regress `depvar' x`).
  {
    dprof::Scope __z(dprof::kResample);
    static const std::regex reResamplePrefix(
        R"(^\s*(bootstrap|jackknife|permute|rolling|statsby)\b(.*)$)", std::regex::icase);
    std::smatch rm;
    if (std::regex_match(rawCmd, rm, reResamplePrefix)) {
      const std::string method = lower(trimWs(rm.str(1)));
      std::string rest = rm.str(2);
      // The prefix colon is the first one outside quotes AND outside brackets:
      // an exp_list element `_b[treat:lower]` (an equation-striped coefficient)
      // carries a colon of its own (lane BS; StataBE scratch-r150/lee/bs/o5.log U1).
      size_t colonPos = std::string::npos;
      {
        int depth = 0;
        const std::vector<bool> quoted = eval::quotedSpanMask(rest);  // compound quotes too
        for (size_t i = 0; i < rest.size(); ++i) {
          const char ch = rest[i];
          if (i < quoted.size() && quoted[i]) continue;
          if (ch == '(' || ch == '[' || ch == '{') ++depth;
          else if ((ch == ')' || ch == ']' || ch == '}') && depth > 0) --depth;
          else if (ch == ':' && depth == 0) {
            colonPos = i;
            break;
          }
        }
      }
      // A bare `bootstrap` (no colon) is not the prefix form — let it fall
      // through to the unsupported-command path, which is what Stata's own
      // "command bootstrap is unrecognized" corresponds to here.
      if (colonPos != std::string::npos) {
        std::string headPart = trimWs(rest.substr(0, colonPos));
        std::string innerRaw = trimWs(rest.substr(colonPos + 1));
        auto [expAndVar, optsOpt] = splitTopLevelComma(headPart);
        std::string expListRaw = trimWs(expAndVar);
        std::string optsRaw = optsOpt.has_value() ? trimWs(*optsOpt) : std::string();
        // `permute VAR explist` — the permutation variable sits between the
        // command word and the expression list.
        std::string permuteVar;
        if (method == "permute") {
          size_t sp = expListRaw.find_first_of(" \t");
          if (sp == std::string::npos) {
            blocks.push_back(ojson(eval::engineError(
                "permute: usage is `permute PERMVAR name=exp [...] [, options]: command`")));
            return blocks;
          }
          permuteVar = trimWs(expListRaw.substr(0, sp));
          expListRaw = trimWs(expListRaw.substr(sp));
        }
        // Expand the halves that MAY hold macros; the exp_list stays raw.
        auto expandHalf = [&](const std::string& in, bool* failed) -> std::string {
          if (in.empty() || !eval::hasMacroRefs(in)) return in;
          eval::ExpandResult er = eval::expandMacros(in, session.eval);
          if (er.isError) {
            blocks.push_back(er.error);
            *failed = true;
            return in;
          }
          return er.line;
        };
        bool failed = false;
        optsRaw = expandHalf(optsRaw, &failed);
        if (failed) return blocks;
        std::string innerCmd = expandHalf(innerRaw, &failed);
        if (failed) return blocks;

        ojson res = (method == "statsby")
                        ? handlers::runStatsby(expListRaw, optsRaw, innerCmd, session)
                    : (method == "rolling")
                        ? handlers::runRolling(expListRaw, optsRaw, innerCmd, session)
                        : handlers::runResample(method, permuteVar, expListRaw, optsRaw, innerCmd,
                                                session);
        if (res.is_array()) {
          for (const auto& b : res) blocks.push_back(b);
        } else {
          blocks.push_back(res);
        }
        return blocks;
      }
    }
  }

  // ------------------------------------------------------------------
  // 0a-ter. `ritest` (E3, handlers_e3_ritest.cpp) -- intercepted before macro
  // expansion for the resampling prefixes' reason: its exp_list (`_b[tri]`,
  // `e(n)`) is evaluated after every replicate, never against the e() the
  // previous command left. The driver expands every other part itself.
  {
    static const std::regex reRitest(R"(^\s*ritest(\s.*)?$)", std::regex::icase);
    std::smatch rm;
    if (std::regex_match(rawCmd, rm, reRitest)) {
      ojson res = handlers::runRitestLine(rm[1].matched ? rm.str(1) : std::string(), session);
      for (const auto& b : res) blocks.push_back(b);
      return blocks;
    }
  }

  // ------------------------------------------------------------------
  // 0b. Session-state layer: expand macro / r() references in the raw line.
  std::string cmdForDispatch = rawCmd;
  // EM3a: the expanded line with every macval() value still encoded -- what
  // tokenize()'s unexpanded-macro net must read (it decodes right after the net).
  // Every other consumer below reads cmdForDispatch, the real characters.
  std::string cmdVerbatim;
  bool haveVerbatim = false;
  // BUG-1527: the expanded line with its compound quotes as typed, when the fold
  // below changed it ("" = cmdForDispatch): graph describe's r(command) is the
  // command as typed (StataBE 19.5: `title(`"T cq"')` stays compound). BUG-1049:
  // a program's `0' is taken from it too (Stata binds `0' to the unparsed text,
  // so `echo0 `"p"'' sees `"p"', not the "p" every other consumer reads).
  std::string lineAsTyped;
  {
  dprof::Scope __z(dprof::kExpand);
  if (eval::hasMacroRefs(rawCmd)) {
    eval::ExpandResult r = eval::expandMacros(rawCmd, session.eval, false, /*keepVerbatim=*/true);
    if (r.isError) {
      // A display whose later piece fails to expand prints its earlier pieces
      // first (displayPartialBlock).
      if (isDisplayLine(rawCmd)) {
        if (auto partial = displayPartialBlock(rawArgsAfterCmd(rawCmd), r.error, session, false))
          blocks.push_back(*partial);
      }
      blocks.push_back(r.error);
      return blocks;
    }
    cmdVerbatim = r.line;
    haveVerbatim = true;
    cmdForDispatch = eval::decodeVerbatim(r.line);
    if (!r.typed.empty()) lineAsTyped = eval::decodeVerbatim(r.typed);
  }

  // ------------------------------------------------------------------
  // 0b-bis. BUG-176 (P2-PREP): re-apply the two PRE-expansion matchers above
  // to the line macro expansion just produced.
  //
  // Steps (c)/(d) (tempfile/tempname/tempvar) and 0a (tryCaptureDefinition)
  // both pattern-match the RAW line — deliberately, so the names they bind
  // stay literal identifiers (see those steps' own comments). But that makes
  // the whole matcher blind whenever the COMMAND WORD itself arrives through
  // a macro: `global L "tempvar t1"` + `capture quietly $L` reached step (c)
  // as the literal token `$L`, missed, and fell through to
  // eval/tokenizer.cpp's macro-statement keyword net, which rejects
  // "tempvar" unconditionally -> `unsupported_macro`, rc 198. The `scalar`
  // twin fell past the list/dir-only `scalar` arm further down and returned
  // rc 199. Both are the same defect: expansion happens AFTER the matchers
  // that need to see what it produced. Measured on a do-file that dispatches
  // `capture quietly $BENCH_LINE`.
  //
  // Guarded on `cmdForDispatch != rawCmd`, so the literal path is untouched:
  // a line with no macro refs never re-enters here at all, and one whose
  // expansion is a no-op is byte-identically excluded. Nor can this
  // double-fire — every arm above RETURNS on a match, so reaching this point
  // proves the raw line matched none of them. The only lines that can be
  // consumed here are precisely those whose leading keyword came out of a
  // macro, which is the class being fixed.
  //
  // Deliberately NOT re-applied: steps (e) `return scalar`/`return local`,
  // (f) `program drop|dir`, and 0a-bis's resampling prefixes. Each has
  // ordering semantics of its own (0a-bis in particular MUST see its
  // exp_list unexpanded), and none was implicated by a measurement — left
  // for a follow-up that can measure them.
  if (cmdForDispatch != rawCmd) {
    ojson u4Out;
    if (tryTempDeclIntercept(cmdForDispatch, session, echo, u4Out)) return u4Out;
    if (handlers::tryMacroVerbIntercept(cmdForDispatch, session, echo, u4Out, true)) {
      return u4Out;  // AER tier-1, the macro-expanded `macro ...` line
    }
    // BUG-216/217: same error channel as step 0a's site. This is the arm that sees
    // a colon form whose COMMAND WORD arrived through a macro, so it needs the
    // same rc translation or that shape would still store literal text.
    {
      ojson extDisplayOut;  // BUG-513, the macro-delivered command word (BUG-176's class)
      if (tryExtDisplayCapture(cmdForDispatch, session, extDisplayOut)) return extDisplayOut;
    }
    eval::MacroFnError mfe2;
    if (eval::tryCaptureDefinition(substituteCRefs(cmdForDispatch, session), session.eval,
                                   &mfe2)) {
      if (mfe2.failed()) {
        // BUG-339 twin of the step-0a site above: name the line that failed.
        return ojson::array({ojson(eval::errorBlock("engine_error", mfe2.message, std::nullopt,
                                                    std::nullopt, strutil::trim(rawCmd),
                                                    "r(" + mfe2.rc + ")"))});
      }
      return ojson::array();
    }
  }
  }  // dprof kExpand
  // EM3b: a line with no macro reference never reached expandMacros' compound-quote
  // normalization; a `"..."' holding no '"' is a plain literal here too.
  if (!haveVerbatim) {
    if (cmdForDispatch.find("`\"") != std::string::npos) {
      std::string folded = eval::normalizeCompoundQuotes(cmdForDispatch);
      if (folded.size() != cmdForDispatch.size()) lineAsTyped = cmdForDispatch;  // BUG-1527
      cmdForDispatch = std::move(folded);
    }
    cmdVerbatim = cmdForDispatch;
  }

  // ------------------------------------------------------------------
  // 1. Tokenize (on the macro-expanded line).
  eval::ParsedCommand parsed;
  // `xi [, opts] : <command>` (AER bundle H). Intercepted BEFORE tokenize
  // because the tokenizer would read `xi:` as the command word and the wrapped
  // line as its varlist. Materializes the _I* columns, then re-dispatches the
  // wrapped command with each i. term replaced by the names it produced -- which
  // is what makes `xi: regress y i.region` estimate on real columns and report
  // `_Iregion_2` in its coefficient table, exactly as Stata does.
  {
    static const std::regex reXiPrefix(R"(^\s*xi\s*(?:,\s*([^:]*?))?\s*:\s*(\S.*)$)",
                                       std::regex::icase);
    std::smatch mxi;
    if (std::regex_match(cmdForDispatch, mxi, reXiPrefix)) {
      const std::string optText = mxi.str(1);
      // xi-glued-comma: `xi: qreg2 y i.mr, cl(cid)` (w2j_e3_qreg2.log:1672, rc 0)
      // glues the options comma to the last i. term, so the token `i.mr,` named a
      // variable `mr,` (r(111)). The first top-level comma is set off by a space;
      // the rewritten line keeps the options verbatim after it.
      const std::string wrapped = [](std::string w) {
        int depth = 0;
        bool inQuote = false;
        for (size_t i = 0; i < w.size(); ++i) {
          const char c = w[i];
          if (c == '"') inQuote = !inQuote;
          if (inQuote) continue;
          if (c == '(') {
            ++depth;
          } else if (c == ')') {
            --depth;
          } else if (c == ',' && depth == 0) {
            if (i > 0 && !std::isspace(static_cast<unsigned char>(w[i - 1]))) w.insert(i, " ");
            break;
          }
        }
        return w;
      }(mxi.str(2));
      std::vector<std::pair<std::string, eval::OptValue>> xiOpts;
      if (optText.find("noomit") != std::string::npos) {
        eval::OptValue b;
        b.isBool = true;
        b.boolValue = true;
        xiOpts.emplace_back("noomit", b);
      }
      static const std::regex rePfx(std::string(R"(prefix\(\s*()") + eval::kRxName + R"()\s*\))",
                                    std::regex::icase);
      std::smatch mp;
      // A Unicode stub is a name (core/ident.h); a non-name is not taken, as the
      // ASCII pattern never took one.
      if (std::regex_search(optText, mp, rePfx) && strutil::isValidStataIdentifier(mp.str(1))) {
        eval::OptValue p;
        p.strValue = mp.str(1);
        xiOpts.emplace_back("prefix", p);
      }
      std::vector<std::string> terms;
      {
        std::istringstream ts(wrapped);
        std::string tk;
        while (ts >> tk)
          if (tk.compare(0, 2, "i.") == 0) {
            // a glued option comma (`i.c, fe`) is not part of the term
            const size_t comma = tk.find(',');
            terms.push_back(comma == std::string::npos ? tk : tk.substr(0, comma));
          }
      }
      if (terms.empty()) {
        // No i. term to expand: Stata still runs the command. Forward it
        // unchanged rather than inventing an error.
        return dispatchImpl(wrapped, session);
      }
      std::string rewritten, xiDisplay;
      ojson xerr =
          handlers::runXiPrefix(terms, xiOpts, wrapped, session, &rewritten, &xiDisplay);
      if (!xerr.is_null()) {
        blocks.push_back(xerr);
        return blocks;
      }
      if (!xiDisplay.empty()) {
        ojson db;
        db["type"] = "display";
        db["text"] = xiDisplay;
        blocks.push_back(db);
      }
      ojson inner = dispatchImpl(rewritten, session);
      if (inner.is_array())
        for (const auto& b : inner) blocks.push_back(b);
      else if (!inner.is_null())
        blocks.push_back(inner);
      return blocks;
    }
  }

  std::string cmd;
  std::string cmdTyped;  // BUG-105/WS-K: the spelling the USER typed, kept for
                         // the extcompat gate, the `display` raw-argument scan
                         // and every message that echoes the command word.
  std::optional<RcHiddenScope> rcHidden;  // ifReadsCallerRc, for this command's run
  {
    dprof::Scope __z(dprof::kTokenize);
    // BUG-1049: a user program parses its own arguments (`0'), so a compound
    // literal holding a '"' is the program's to read, not the tokenizer's to
    // refuse; the call needs only the program's name.
    const std::string headWord0 = [&] {
      const std::string t = trimWs(cmdForDispatch);
      return t.substr(0, t.find_first_of(" \t"));
    }();
    try {
      parsed = eval::tokenize(cmdVerbatim);
    } catch (const eval::TokenizerError& te) {
      if (te.block.value("token", "") == "`\"" &&
          program::findProgram(session, headWord0).has_value()) {
        parsed = eval::ParsedCommand{};
        parsed.cmd = headWord0;
        parsed.raw = cmdForDispatch;
      } else {
        if (te.block.value("code", "") == "syntax_error" &&
            te.block.value("message", "").find("comment") != std::string::npos) {
          return ojson::array();  // pure comment lines produce nothing
        }
        // BUG-535: under a by-prefix, `in` is Stata's own refusal — "'in' may not
        // be combined with 'by'", r(190) — for every sub-command
        // (w2_bykeep.log arms by_keep_in / by_keep_if_in), not PIET's generic
        // "qualifier unsupported" r(198). The tokenizer rejects `in` on a
        // by-line because `by` has no registry row; only the rc and the words
        // change here, the line still does not run.
        if (te.block.value("code", "") == "unsupported_option" &&
            te.block.value("token", "").rfind("in ", 0) == 0) {
          const std::string head = lower(trimWs(cmdForDispatch).substr(
              0, trimWs(cmdForDispatch).find_first_of(" \t")));
          if (head == "by" || head == "bys" || head == "byso" || head == "bysor" ||
              head == "bysort") {
            blocks.push_back(ojson(eval::errorBlock(
                "syntax_error", "'in' may not be combined with 'by'", te.block.value("token", ""),
                std::nullopt, std::nullopt, "r(190)")));
            return blocks;
          }
        }
        blocks.push_back(ojson(te.block));
        return blocks;
      }
    }
    session.leafCmdExpanded = cmdForDispatch;  // the fit hook's e(cmdline)
    // BUG-428/432: the row layer gets the matrix/scalar substitution the macro
    // layer has always had. One call site per expression that reaches the row
    // evaluator — the `if` here, `assert`'s claim and gen/replace's expression.
    if (parsed.ifExpr.has_value() && !parsed.ifExpr->empty()) {
      std::optional<ojson> rowErr;
      // A by-prefixed line's sub-command decides at the by: driver.
      const std::string rowCmd = registry::resolveCanonical(rstripColon(lower(parsed.cmd)));
      const bool byLine = rowCmd == "by" || rowCmd == "bys" || rowCmd == "byso" ||
                          rowCmd == "bysor" || rowCmd == "bysort";
      const bool readsRc = ifReadsCallerRc(rowCmd);
      if (!readsRc && !byLine && substituteRcValue(*parsed.ifExpr, session) != *parsed.ifExpr)
        rcHidden.emplace();
      parsed.ifExpr = substituteRowExprRefs(*parsed.ifExpr, session, &rowErr, readsRc);
      if (rowErr.has_value()) {  // BUG-456: r(111) / r(109), never a silent `.`/0
        blocks.push_back(*rowErr);
        return blocks;
      }
    }
    cmdTyped = rstripColon(lower(parsed.cmd));
    // BUG-105 (WS-K) — THE ONE COMMAND-NAME RESOLUTION POINT. Every arm below,
    // plus kNoDataCommands and kFixedArityCommands, now compares the CANONICAL
    // spelling, so adding a Stata abbreviation is a one-line registry edit
    // instead of the two-file (registry + if-chain literal) edit the 07-31
    // BUG-105 stopgap needed. Non-registry tokens (by/bysort, the pre-tokenize
    // keywords) come back unchanged, and a community-package name comes back
    // unchanged on purpose — see registry.h's resolveCanonical() comment.
    // Reverting this one line is the seeded-RED leg selftest's
    // testWsKAbbrevResolutionIsLoadBearing() asserts against.
    cmd = registry::resolveCanonical(cmdTyped);

    // BUG-450 — THE VARLIST EXPANSION POINT, the third sibling of the command
    // and option resolution points above and below it.
    //
    // `loops::expandVarlist` calls itself "the ONE varlist wildcard/range
    // expander", and until now its callers were the loop header, `ds`/`unab`
    // and the handful of AER handlers written against it directly. The plain
    // data verbs were not among them: `drop t*` answered `variable t* not
    // found` r(111) while `foreach v of varlist t*` expanded correctly in the
    // same session on the same data. In the ninety-package AER corpus that is
    // 1,214 `drop` lines and 989 `keep` lines carrying a wildcard, plus the
    // range forms — the ordinary way that corpus subsets a dataset.
    //
    // Two guards keep this from being a blanket change:
    //   1. The ROW must opt in (`varlistIsPlain`). `rename t* T*` is Stata's
    //      rename-GROUPS syntax and must reach its arm unexpanded; estimation
    //      rows carry `i.x`/`L.x` in the same field. See registry.cpp's list.
    //   2. The TOKEN must actually look like a pattern. A varlist with no `*`,
    //      `?`, `-` or `_all` is passed through byte-identically, so the
    //      common case is untouched and cannot regress.
    // A no-match answers r(111), which is what Stata answers.
    if (!parsed.varlist.empty()) {
      const registry::CommandSpec* spec = registry::findByName(cmd);
      if (spec != nullptr && spec->varlistIsPlain) {
        // BUG-525, second half (oracle corr_glued == ctl_corr: r(rho)
        // .0171895075182102 both ways; levelsof_glued -> `0 1 2`): Stata's
        // varlist parser accepts ONE pair of parentheses around a plain
        // varlist, and the glued heads deliver exactly that shape once the
        // word is split (`corr (a c)`, `levelsof (g)`). PIET forwarded `(a`
        // and `c)` as names and answered "column '(a' not found". Unwrapped
        // here, at the one varlist site -- but gated on its OWN allow-list
        // (kParenVarlistUnwrap), not on `varlistIsPlain`: the oracle has an
        // arm for exactly corr_glued and levelsof_glued, not for the other
        // thirteen plain-varlist rows (drop/keep/summarize/destring/...),
        // and reusing the broad flag would have changed their answer for
        // `(x1 x2)`-shaped input with no oracle arm behind it (review
        // finding, W2 labelglued fix round). The whole field must be the
        // one group -- an inner paren, `=` or quote leaves it alone -- so
        // `test (a = b)` and `(y1 x) (y2 x)` shapes are untouched even on a
        // plain row.
        static const std::unordered_set<std::string> kParenVarlistUnwrap = {"corr", "levelsof"};
        if (kParenVarlistUnwrap.count(cmd) != 0 &&
            parsed.varlist.front().front() == '(' && parsed.varlist.back().back() == ')') {
          std::string joined;
          for (size_t i = 0; i < parsed.varlist.size(); ++i) {
            if (i != 0) joined += ' ';
            joined += parsed.varlist[i];
          }
          const std::string inner =
              joined.size() >= 2 ? joined.substr(1, joined.size() - 2) : std::string();
          if (!inner.empty() && inner.find_first_of("()=\"`'") == std::string::npos) {
            std::vector<std::string> unwrapped;
            std::istringstream is(inner);
            for (std::string tk; is >> tk;) unwrapped.push_back(tk);
            if (!unwrapped.empty()) parsed.varlist = unwrapped;
          }
        }
        bool patterned = false;
        for (const auto& t : parsed.varlist) {
          if (t.find('*') != std::string::npos || t.find('?') != std::string::npos ||
              t.find('-') != std::string::npos || lower(t) == "_all") {
            patterned = true;
            break;
          }
        }
        if (patterned && session.hasData()) {
          std::string spec_str;
          for (size_t i = 0; i < parsed.varlist.size(); ++i) {
            if (i != 0) spec_str += ' ';
            spec_str += parsed.varlist[i];
          }
          loops::VarlistExpandResult ex = loops::expandVarlist(spec_str, session);
          if (ex.isError) {
            blocks.push_back(ex.error);
            return blocks;
          }
          parsed.varlist = ex.tokens;
        }
        // Variable-name abbreviation ([U] 11.2) for every plain varlist: a name
        // that is no variable but the unique prefix of one stands for it, several
        // are r(111) "ambiguous abbreviation", a name matching nothing is left as
        // typed for the command's own refusal (handlers::resolveByKeyNames, the
        // rule keep/drop/rename and the by keys use). StataBE ec.do: `tab
        // edu_category` tabulates edu_category_BL (007's Stats_for_paper.do).
        if (session.hasData()) {
          if (auto err = handlers::resolveByKeyNames(session, parsed.varlist)) {
            blocks.push_back(*err);
            return blocks;
          }
        }
      } else if (spec != nullptr && session.hasData()) {
        // A single-equation fit's `syntax varlist(fv ts)` expands a range or a
        // wildcard like any varlist (`xtreg y x1-x3, fe`: w2d_uncovered.log
        // xtreg_fe_panelonly_034 b[x1] 2.0995 over x1 x2 x3). These rows keep
        // `i.x`/`L.x`/`a#b`/`(x = z)` in the same field, so only a token that is
        // a pure name pattern -- letters, digits, `_`, `*`, `?`, `~`, and an
        // inner `-` -- is expanded, each on its own; every other token passes
        // through byte-identically. Post-estimation rows (lincom/test/nlcom/
        // margins) are not listed: `x1-x2` is an expression there.
        static const std::unordered_set<std::string> kFitVarlistPatterns = {
            "regress", "areg",    "xtreg",  "reghdfe", "logit",   "probit", "logistic",
            "poisson", "nbreg",   "qreg",   "tobit",   "newey",   "prais",  "glm",
            "xtlogit", "xtprobit", "xtpoisson", "ologit", "oprobit", "mlogit", "cloglog",
            "ppmlhdfe", "ivregress", "ivreg2", "ivreghdfe", "feols", "fepois", "xtivreg2"};
        if (kFitVarlistPatterns.count(cmd) != 0) {
          const auto isNamePattern = [](const std::string& t) {
            if (lower(t) == "_all") return true;
            if (t.empty() || t.front() == '-' || t.back() == '-') return false;
            // core/ident.h: `ملاح*` / `دخل_2020-ق3` are name patterns too.
            if (!(cecon::ident::isIdentStartByte(t.front()) || t.front() == '*' ||
                  t.front() == '?'))
              return false;
            bool pattern = false;
            for (char ch : t) {
              if (ch == '*' || ch == '?' || ch == '-' || ch == '~') {
                pattern = true;
              } else if (!cecon::ident::isIdentByte(ch)) {
                return false;
              }
            }
            return pattern;
          };
          std::vector<std::string> out;
          bool changed = false;
          for (const auto& t : parsed.varlist) {
            if (!isNamePattern(t)) {
              out.push_back(t);
              continue;
            }
            loops::VarlistExpandResult ex = loops::expandVarlist(t, session);
            if (ex.isError) {
              blocks.push_back(ex.error);
              return blocks;
            }
            out.insert(out.end(), ex.tokens.begin(), ex.tokens.end());
            changed = true;
          }
          if (changed) parsed.varlist = std::move(out);
          // Variable-name abbreviation ([U] 11.2) in a fit's varlist, depvar
          // included: StataBE uni-oracle/fa.do `regress y miss_unemployed` fits
          // miss_unemployed_BL (F1), and so do `i.edu_category`,
          // `c.miss_unemployed#i.edu_cat`, `L.miss_unemployed` and reghdfe
          // (F2-F5, e(b) carries the full names); `edu` / `i.edu` with two
          // candidates is "edu ambiguous abbreviation" r(111) (F7/F8). Each `#`
          // atom resolves the name after its operator (handlers::
          // resolveByKeyNames, the rule the plain varlists use). DESIGN
          // DECISION (no oracle arm): a token holding `(`, `)` or `=` (an IV
          // block, `ib(last).x`) and ivregress's estimator word pass unchanged.
          const auto isPlainName = [](const std::string& s) {
            if (s.empty() || !cecon::ident::isIdentStartByte(s.front())) return false;
            for (char ch : s)
              if (!cecon::ident::isIdentByte(ch)) return false;
            return true;
          };
          for (size_t i = (cmd == "ivregress" ? 1 : 0); i < parsed.varlist.size(); ++i) {
            std::string& tok = parsed.varlist[i];
            if (tok.find_first_of("()=") != std::string::npos) continue;
            std::string rebuilt;
            size_t pos = 0;
            while (pos <= tok.size()) {
              size_t hash = tok.find('#', pos);
              if (hash == std::string::npos) hash = tok.size();
              std::string atom = tok.substr(pos, hash - pos);
              const size_t dot = atom.rfind('.');
              std::string name = dot == std::string::npos ? atom : atom.substr(dot + 1);
              if (isPlainName(name)) {
                std::vector<std::string> one = {name};
                if (auto err = handlers::resolveByKeyNames(session, one)) {
                  blocks.push_back(*err);
                  return blocks;
                }
                atom = (dot == std::string::npos ? std::string() : atom.substr(0, dot + 1)) +
                       one.front();
              }
              rebuilt += atom;
              size_t next = hash;
              while (next < tok.size() && tok[next] == '#') rebuilt += tok[next++];
              if (hash == tok.size()) break;
              pos = next;
            }
            tok = rebuilt;
          }
          // BUG-1437: the cluster variables of vce(cluster ...) and cluster() are
          // variable names too, abbreviated by the same rule: StataBE 19.5
          // (2026-11-03) fits `vce(cluster kgr)` and `cluster(kgr)` on kgroup
          // with every one of regress, areg, logit, probit, poisson, nbreg,
          // xtreg, ivregress, reghdfe and ppmlhdfe (e(clustvar) kgroup), and
          // vce(cluster kgroup) with cluster(kgr) is the one variable twice. Only
          // a unique prefix is rewritten here; a name matching nothing or
          // several is left as typed for the fit's own parser, which words it
          // (clusterSpecRefusal / clusterSpecRefusalM4).
          for (auto& [k, v] : parsed.opts) {
            if (v.isBool) continue;
            const bool vceOpt = k == "vce" || k == "vcov";
            if (!vceOpt && k != "cluster") continue;
            std::string head;
            std::string rest = v.strValue;
            if (vceOpt) {
              std::string vceRest;
              if (handlers::canonicalVceType(v.strValue, &vceRest) != "cluster" || vceRest.empty())
                continue;
              std::istringstream hs(v.strValue);
              hs >> head;
              rest = vceRest;
            }
            std::vector<std::string> names;
            std::istringstream rs(rest);
            for (std::string t; rs >> t;) names.push_back(t);
            bool rewrote = false;
            for (auto& n : names) {
              if (!isPlainName(n)) continue;
              std::vector<std::string> one = {n};
              if (handlers::resolveByKeyNames(session, one).has_value()) continue;
              if (one.front() != n) {
                n = one.front();
                rewrote = true;
              }
            }
            if (!rewrote) continue;
            std::string joined = head;
            for (const auto& n : names) joined += (joined.empty() ? "" : " ") + n;
            v.strValue = joined;
          }
        }
      }
      // fvgroup-operator: Stata's grouped factor operator `i.(a b c#d)` distributes
      // the operator over every term and every `#` atom (`i.a i.b i.c#i.d`); the
      // 069 form `qreg y z i.(tr month sid#cash#risk clinic), q(.37)` is rc 0
      // (w2j_e3_qreg2.log:3103). Estimation rows only; an old-style IV block
      // `(y = z)` carries no operator prefix and is left alone.
      if (spec != nullptr && spec->family == registry::Family::Estimation)
        parsed.varlist = expandFactorGroupTokens(parsed.varlist);
    }
  }

  // ATTR-20260812: the one place a command name becomes canonical, so the one
  // place a per-item timing row can learn what it is actually running. The
  // executor bills by the item's leading token, which makes `capture noisily
  // regress` a `capture` row and files `reg`/`regress` apart; prefixes are
  // stripped upstream of here, so the FIRST resolution inside an item is the
  // real verb. No-op unless PIET_EMIT_TIMING >= 2.
  piet::counters::noteResolvedVerb(cmd);
  // e(wexp) as Stata keeps it (session_state.h commandWexp / LastModel::fitWexp).
  session.commandWexp = parsed.weightExp.value_or(std::string());
  // BUG-1527: the statement a graph's describe record keeps (graphMetaFor trims it).
  const std::string& graphLineTyped = lineAsTyped.empty() ? parsed.raw : lineAsTyped;

  // THE STRING-CONDITION ROUTE (goal qualifier-if-strfn-weight-spelling). An `if`
  // holding a string function, or comparing against "", is read by the engine's
  // f64 evaluator, which has no string function (src/data/frame.cpp FNS): `sum x
  // if strpos(s,"a") > 0` was rc 1 and `sum x if s != ""` kept the empty row
  // (w2aj_qualifiers qf_if_*). count/keep/drop/generate/replace evaluate such a
  // condition row-wise themselves (BUG-032); every other command reads it from the
  // same evaluator through a hidden row-mask column (strmutate::
  // installConditionMask), so the engine receives a row selection, never the
  // string-function text, and every `--keepif`/applyIfAndInFilter site is served
  // at once. The by-prefix driver takes the same route for its sub-command below.
  // The mask leaves the frame when this dispatch returns (QualifierColumnGuard).
  struct QualifierColumnGuard {
    SessionState& session;
    std::vector<std::string> columns;
    ~QualifierColumnGuard() {
      for (auto it = columns.rbegin(); it != columns.rend(); ++it) {
        try {
          strmutate::removeConditionMask(session, *it);
        } catch (...) {
        }
      }
    }
  };
  QualifierColumnGuard conditionMask{session, {}};
  {
    static const std::unordered_set<std::string> kOwnRowCondition = {
        "generate", "gen", "g", "replace", "count", "keep", "drop",
        "by", "bys", "byso", "bysor", "bysort"};
    // Graph commands read their `if` per plot layer (graph_m4.cpp), not from here.
    const registry::CommandSpec* routeSpec = registry::findByName(cmd);
    const bool graphFamily = routeSpec != nullptr && routeSpec->family == registry::Family::Graph;
    // BUG-1327 / BUG-1093: assert's `if` reads its constant string cells
    // (substituteStringCells) before it is routed: the row evaluator has no `[`,
    // so `assert m == "a" if m[1] == "b" & _n > 1` failed in the mask below.
    if (cmd == "assert" && parsed.ifExpr.has_value())
      parsed.ifExpr = substituteStringCells(*parsed.ifExpr, session);
    if (parsed.ifExpr.has_value() && !kOwnRowCondition.count(cmd) && !graphFamily &&
        session.hasData() && strmutate::needsConditionMask(session, *parsed.ifExpr)) {
      strmutate::ConditionMaskInstall mi = strmutate::installConditionMask(session, *parsed.ifExpr);
      if (!mi.ok) {
        blocks.push_back(mi.error);
        return blocks;
      }
      conditionMask.columns.push_back(mi.column);
      parsed.ifExpr = mi.condition;
    }
    // A weight that is an expression (`[pw=1/wf]`, `[aw = 1/wf]`: w2aj_qualifiers
    // qf_rw_pw_expr(_sp), qf_sw_aw_expr(_sp)) is generated once into a hidden
    // column the command receives as its weight variable; e(wexp) keeps the text
    // (commandWexp above). Every weight reader takes a variable name, so this is
    // the one place an expression becomes one.
    if (parsed.weightVar.has_value() && !parsed.weightVar->empty() && session.hasData() &&
        !strutil::isValidStataIdentifier(*parsed.weightVar) &&
        !(cmd == "by" || cmd == "bys" || cmd == "byso" || cmd == "bysor" || cmd == "bysort")) {
      strmutate::ConditionMaskInstall wi =
          strmutate::installWeightExpression(session, *parsed.weightVar);
      if (!wi.ok) {
        blocks.push_back(wi.error);
        return blocks;
      }
      conditionMask.columns.push_back(wi.column);
      parsed.weightVar = wi.column;
    } else if (parsed.weightVar.has_value() && !parsed.weightVar->empty() && session.hasData() &&
               !(cmd == "by" || cmd == "bys" || cmd == "byso" || cmd == "bysor" ||
                 cmd == "bysort")) {
      // A weight variable is read as Stata reads a name in an expression: the
      // variable, else its unique abbreviation, else r(111) "variable X not found"
      // (qf_sw_aw_nosuch, qf_rw_aw_nosuch). Each reader forwarded the text to the
      // engine, whose `no column: X` was rc 1.
      std::vector<std::string> known;
      if (const std::vector<VarInfo>* card = session.residentReadCard()) {
        for (const auto& vi : *card) known.push_back(vi.name);
      } else if (Datastore* wds = session.datastore()) {
        known = wds->names();
      }
      const std::string& wv = *parsed.weightVar;
      if (!known.empty() && std::find(known.begin(), known.end(), wv) == known.end()) {
        std::vector<std::string> hits;
        for (const auto& k : known)
          if (k.compare(0, wv.size(), wv) == 0) hits.push_back(k);
        if (hits.size() == 1) {
          parsed.weightVar = hits.front();
        } else {
          blocks.push_back(ojson(eval::errorBlock(
              "engine_error",
              hits.empty() ? "variable " + wv + " not found" : wv + " ambiguous abbreviation",
              std::nullopt, std::nullopt, std::nullopt, "r(111)")));
          return blocks;
        }
      }
    }
  }

  // INSPECT-2608: the per-FORM key, built at the same seam and for the same
  // reason — this is where the command is canonical AND fully parsed. A form is
  // the verb plus PRESENCE flags for the qualifiers that change how much work
  // the command does; values never enter the key, so `if x > .5` and `if x > .9`
  // are one row and the table stays bounded. Per-verb alone would average a
  // filtered `summarize` together with a whole-frame one, which is exactly the
  // distinction a per-command efficiency census exists to see.
  // No-op below PIET_EMIT_TIMING=4.
  if (piet::counters::timingLevel() >= 4) {
    std::string form = cmd;
    if (parsed.ifExpr.has_value()) form += "|if";
    if (parsed.inRange.has_value()) form += "|in";
    if (parsed.weightType.has_value()) form += "|w";
    piet::counters::noteResolvedForm(form);
  }

  // ------------------------------------------------------------------
  // 1a-ter. BUG-105 DoD ③ / FIX:OPTNET-ABBREV-SWEEP — THE OPTION-NAME
  // RESOLUTION POINT, the sibling of the command one above.
  //
  // Implements internal note BUG105-OPTABBREV-AUDIT-20260731.md §2's nine measured
  // minimums. Unlike command names, OPTIONS really do follow a prefix rule in
  // Stata: each option documents its own minimum via `{opt l:evel(#)}` markup
  // and every prefix at-or-longer-than that minimum is accepted. So the table
  // below is (option, minimum length) and every prefix in that range is
  // generated — which is also what makes it a FIX and not just a widening:
  //
  // BUG-231 (measured this stint, not predicted by the audit): eval::
  // rejectUnknownOptions already resolves a >=2-char unique prefix against the
  // net's `known` set, so an abbreviated option ALREADY PASSED the gate — while
  // the handler's own `k == "offset"` comparisons still saw the raw key and
  // skipped it. The option was accepted and then silently dropped:
  //   `poisson fatal beertax, offset(pop)` -> b = -8861267.42121
  //   `poisson fatal beertax, offs(pop)`   -> b =        6.78117  (== no option)
  //   `poisson fatal beertax, exp(pop)`    -> runs; `exposure(pop)` rejects
  // A wrong number behind a right-looking command, with no diagnostic. Rewriting
  // the key HERE — before any handler compares it — is what closes that, because
  // it is the only point that is ordering-correct for every net regardless of
  // where in the handler its reject net happens to sit.
  //
  // AER scope-2 A1: the three passes themselves now live in ONE function,
  // resolveOptionKeyForCommand() (this file, immediately above dispatchImpl),
  // so `piet_native --dump-optvocab --resolve` can report the resolution this
  // loop performs instead of carrying a second copy of the rule. Nothing about
  // the rule moved with it: each pass was already independent per key, so one
  // per-key call produces the same rewrite the two whole-vector passes did.
  for (auto& kv : parsed.opts) {
    const OptKeyResolution optRes = resolveOptionKeyForCommand(cmd, kv.first);
    kv.first = optRes.resolved;
    if (optRes.exactOnly) kv.second.exactOnly = true;
  }

  // ------------------------------------------------------------------
  // 1a-bis. WS-B (extcompat, 2026-08-06) — THE community-name gate. One
  // point, immediately after name resolution, so every route into a command
  // passes it exactly once: a `capture`/`quietly`/`noisily` prefix strips and
  // re-enters dispatch() (Pre-0 above), and `bysort G: reghdfe ...` recurses
  // through dispatch() with the sub-command, so both are covered here without
  // a second check anywhere.
  //
  // ORDER MATTERS for gate ②. The setting is checked FIRST — one relaxed
  // atomic load — and the registry lookup happens only in the off case, which
  // is not the default. On the default path (and therefore in every gate,
  // wave, freeze and answerbook run) this costs one predictable branch per
  // line and touches no table.
  //
  // The message is a REJECT, not a notice: it is only ever produced on the
  // failure path, so no successful command's output can carry an attribution
  // string (LEGAL-DISTRIBUTION §2.7(c) — the display surface is what the
  // answerbook gates byte-compare). Code `unsupported_command` maps to Stata
  // r(199) via eval::coarseRc, so `capture reghdfe ...` sets _rc = 199.
  {
    dprof::Scope __z(dprof::kNameGate);
    if (!settings::extcompatEnabled()) {
      // BUG-105/WS-K: this gate reads cmdTyped, not cmd. resolveCanonical()
      // already exempts community names so the two are equal here today, but
      // the gate's question is literally "did the user type a community name",
      // and it must stay answerable from the typed token alone.
      if (const registry::CommunityName* cn = registry::findCommunityName(cmdTyped)) {
        blocks.push_back(ojson(eval::errorBlock("unsupported_command",
                                                registry::communityGateMessage(*cn), cmdTyped)));
        return blocks;
      }
    }
  }
  // Everything from here to whichever `return` fires is the handler zone; the
  // RAII scope closes on any of them.
  dprof::Scope __zHandler(dprof::kHandler);

  // AER-SCOPE1 bundle C: gtools/ftools translation. Placed AFTER the extcompat
  // community gate (so `set extcompat off` still refuses these names) and
  // BEFORE the by/bysort interception and the no-data guard (so the rewritten
  // head is what those see -- `gegen` must be `egen` by the time the by-prefix
  // branch compares sub-command names, and by the time kNoDataCommands is
  // consulted). One step for ten commands, rather than ten reimplementations.
  if (handlers::isGtoolsAlias(cmd)) {
    handlers::GtoolsTranslation tr = handlers::translateGtoolsAlias(parsed, cmd, session);
    if (!tr.error.empty()) {
      blocks.push_back(tr.error);
      return blocks;
    }
    parsed = tr.parsed;
    cmd = tr.canonical;  // cmdTyped keeps the user's spelling, for messages
  }

  // ------------------------------------------------------------------
  // 1b. M3b: bysort/by prefix interception — BEFORE classify()/the program
  // guard, mirroring dispatcher.py's step 1b (bysort.py's run_bysort_cmd).
  // `bysort VARLIST: CMD` / `by VARLIST: CMD`:
  //   - gen/replace (any expr, not just _n/_N) -> cobra eval --bygen with
  //     by()/sort() clauses (run_bysort_gen).
  //   - egen -> inject `, by(VARLIST)` into the sub-command and recurse
  //     through dispatch() (respecting an explicit by() already present).
  //   - anything else -> `bysort` sorts by VARLIST first (in place), `by`
  //     does not, then the sub-command is dispatched normally (recursion).
  // BS-017: bys/byso/bysor are legal Stata abbreviations of `bysort` (`by`
  // has no shorter abbreviation of its own) — normalise to "bysort" before
  // calling parseBysortPrefix so its existing cmdLower=="bysort" checks
  // (sortFirst included) need no changes of their own.
  if (cmd == "bysort" || cmd == "by" || cmd == "bys" || cmd == "byso" || cmd == "bysor") {
    // BUG-535: `by KEYS, sort: CMD` is Stata's long spelling of `bysort KEYS:
    // CMD` (w2_bykeep.log arm by_sortopt, rc 0). The prefix parser below has no
    // notion of prefix options — the comma sent `sort:` into the option list and
    // the line died "no subcommand found after colon" — so the one option is
    // rewritten here as `bysort KEYS: CMD` and the rewritten line is
    // re-TOKENIZED in place, not re-dispatched. parsed.raw is already
    // macro-expanded; dispatch() would expand it a second time (an expanded
    // value that itself holds `$name` or a backtick would be expanded again).
    // No dispatcher entry point sits below macro expansion (the egen and xi:
    // re-dispatches go through dispatch()/dispatchImpl() too), and this branch
    // reads nothing of the line but its raw text and varlist, so a local
    // re-tokenize is the whole re-entry. Any other prefix option keeps the old
    // path.
    std::string bysortCmdNorm = (cmd == "by") ? cmd : std::string("bysort");
    const eval::ParsedCommand* byParsed = &parsed;
    eval::ParsedCommand sortOptParsed;
    {
      const std::string& raw = parsed.raw;
      const size_t colon = raw.find(':');
      const size_t sp = raw.find_first_of(" \t");
      if (colon != std::string::npos && sp != std::string::npos && sp < colon) {
        auto [keysPart, prefixOpts] = splitTopLevelComma(raw.substr(sp, colon - sp));
        if (prefixOpts.has_value() && lower(trimWs(*prefixOpts)) == "sort" &&
            !trimWs(keysPart).empty()) {
          try {
            sortOptParsed = eval::tokenize("bysort " + trimWs(keysPart) + ": " +
                                           trimWs(raw.substr(colon + 1)));
          } catch (const eval::TokenizerError& te) {
            blocks.push_back(ojson(te.block));
            return blocks;
          }
          byParsed = &sortOptParsed;
          bysortCmdNorm = "bysort";
        }
      }
    }
    BysortPrefix bp = parseBysortPrefix(bysortCmdNorm, byParsed->varlist, byParsed->raw);
    if (bp.subCmd.empty()) {
      blocks.push_back(ojson(eval::engineError(
          "bysort/by: no subcommand found after colon in `" + byParsed->raw + "`")));
      return blocks;
    }
    // BUG-532: abbreviated / wildcard keys resolve before any branch reads them.
    for (std::vector<std::string>* names : {&bp.byVars, &bp.sortVars}) {
      if (std::optional<ojson> err = handlers::resolveByKeyNames(session, *names)) {
        blocks.push_back(*err);
        return blocks;
      }
    }
    // BUG-105 (WS-K) — the by/bysort sibling of the resolution point above, so
    // `bysort region: summ price` reaches the same per-group implementation
    // `bysort region: summarize price` does. The typed spelling is kept for the
    // reject messages further down, which quote what the user wrote.
    const std::string subCmdTyped = lower(rstripColon(bp.subCmd));
    std::string subCmdLower = registry::resolveCanonical(subCmdTyped);
    // Same fold on the by-prefix path: `bysort g: gegen x = mean(y)` has to
    // reach the egen by()-injection branch below, which compares this name. The
    // recursive dispatch of the inner command then passes the main translation
    // point above, so options are still translated exactly once.
    subCmdLower = handlers::gtoolsCanonicalName(subCmdLower);

    if (subCmdLower == "gen" || subCmdLower == "generate" || subCmdLower == "g" ||
        subCmdLower == "replace") {
      bool isReplace = (subCmdLower == "replace");
      // A plain by: generate/replace has the same sortedness requirement as
      // by: keep. In w2_bykeep the final rcby field probes this later command,
      // independently of whether the preceding keep/drop succeeded.
      if (!bp.sortFirst) {
        std::vector<std::string> keys = bp.byVars;
        keys.insert(keys.end(), bp.sortVars.begin(), bp.sortVars.end());
        Datastore* ds = session.datastore();
        if (ds == nullptr) { blocks.push_back(ojson(eval::noDataLoaded())); return blocks; }
        const std::vector<std::string> claim = engineSortClaim(*ds);
        const bool inOrder = claim.empty() ? keysAlreadyGrouped(*ds, keys, true)
                                          : keysLeadClaim(keys, claim);
        if (!inOrder) {
          blocks.push_back(ojson(eval::errorBlock(
              "not_sorted", "not sorted — `by " + joinSpace(bp.byVars) +
                  ":` requires the data to already be sorted by " + joinSpace(keys) + ".",
              std::nullopt, std::nullopt, std::nullopt, "r(5)")));
          return blocks;
        }
      }
      std::string subJoined = bp.subRaw;
      // strip the sub-command token itself (first word) off subRaw.
      {
        size_t sp = subJoined.find_first_of(" \t");
        subJoined = (sp == std::string::npos) ? std::string() : trimWs(subJoined.substr(sp));
      }
      // BUG-1144 (FU-216, FU-227): the sub-command's own options, behind the
      // first comma outside quotes, parentheses and subscripts. They take the
      // plain arms' nets (kVocabGenerate, kVocabReplace); the tail used to stay
      // inside the expression, so `bysort g: replace b = 2.7, nopromote` stored
      // 2.7 in a byte column promoted to float, where StataBE keeps byte b and
      // stores 2 (measured 2026-10-29).
      bool noPromote = false;
      // A replace reads its target before its options (replaceTargetError), so
      // the options' refusal waits for it.
      std::optional<ojson> optionError;
      {
        const long long comma = eval::findUnquotedComma(subJoined);
        if (comma >= 0) {
          eval::ParsedCommand optParsed;
          try {
            optParsed = eval::tokenize(bp.subRaw);
          } catch (const eval::TokenizerError& te) {
            blocks.push_back(ojson(te.block));
            return blocks;
          }
          // The plain arms' keys are resolved at dispatchImpl's resolution
          // point before their nets run; these never pass it, so an accepted
          // abbreviation (`nopro`) reached the `nopromote` compare below raw and
          // was dropped: b promoted to float, where StataBE keeps byte b.
          const char* optCmd = isReplace ? "replace" : "generate";
          for (auto& kv : optParsed.opts) {
            const OptKeyResolution optRes = resolveOptionKeyForCommand(optCmd, kv.first);
            kv.first = optRes.resolved;
            if (optRes.exactOnly) kv.second.exactOnly = true;
          }
          const eval::OptVocab& vocab = isReplace ? kVocabReplace : kVocabGenerate;
          if (auto err =
                  eval::rejectUnknownOptions(optParsed.opts, vocab.known, vocab.unimplemented)) {
            optionError = ojson(*err);
          } else if (isReplace) {
            optionError = replaceNoPromote(optParsed.opts, noPromote);
          }
          if (optionError.has_value() && !isReplace) {
            blocks.push_back(*optionError);
            return blocks;
          }
          subJoined = trimWs(subJoined.substr(0, static_cast<size_t>(comma)));
        }
      }
      // extract trailing `if COND` (first occurrence outside any quoted
      // string — BUG-115 class sweep: `bysort v: gen x = "if you see this"`
      // must not have its quoted literal mis-split as an `if` qualifier).
      // Only the `if` keyword itself must stand outside the quotes: the
      // condition after it may hold a string literal (`by g: replace x = 30 if
      // s != "a"` was r(198) `trailing input` when the whole tail had to be
      // unquoted; StataBE 19.5 makes 6 real changes, measured 2026-10-29).
      std::optional<std::string> ifExpr;
      static const std::regex reIf(R"(\s+if\s+)", std::regex::icase);
      std::smatch ifm;
      if (eval::searchUnquoted(subJoined, reIf, ifm)) {
        const size_t at = static_cast<size_t>(ifm.position(0));
        ifExpr = trimWs(subJoined.substr(at + static_cast<size_t>(ifm.length(0))));
        subJoined = trimWs(subJoined.substr(0, at));
      }
      // storage-type prefix strip (gen byte idx = _n).
      StorageTypeInfo info = stripStorageType(subJoined);
      // BUG-1142: a by-prefixed replace reads its target as the plain arm does,
      // and before its options (`by g: replace qq = 1, foo` is r(111) on StataBE
      // 19.5, measured 2026-10-29).
      if (isReplace) {
        if (auto err = replaceTargetError(session, info)) {
          blocks.push_back(*err);
          return blocks;
        }
        if (optionError.has_value()) {
          blocks.push_back(*optionError);
          return blocks;
        }
      }
      // e(sample) T2/T4: this route hands its own `if` and expression to the
      // engine as raw text (runBysortGen), past the whole-line rewrite, so they
      // take the SAME row rewrite the plain gen/replace arms take
      // (substituteRowExprRefs: e(sample) first, then _b[]/_se[]/e()/scalar()/
      // stored scalars) -- else `e()` was rc 1 (w2_esample.log:560 B27 `by id:
      // generate ... = sum(1) if e(sample)` |0|28|6|), and before this round
      // `by g: gen z = 1 if x > scalar(s)` was rc 1 `function scalar() is not
      // supported` and `... if x > _b[x]` r(111) `unknown variable _b` while the
      // same line without `by` ran (review 2026-10-15). Only the right-hand
      // side is rewritten, never the target name: a stored scalar may share
      // the new variable's name. generate/replace do not touch e(), so one
      // reading serves every group.
      {
        std::optional<ojson> rowErr;
        if (ifExpr.has_value()) ifExpr = substituteRowExprRefs(*ifExpr, session, &rowErr);
        const size_t eq = info.expr.find('=');
        if (!rowErr.has_value() && eq != std::string::npos) {
          info.expr = info.expr.substr(0, eq + 1) + " " +
                      substituteRowExprRefs(trimWs(info.expr.substr(eq + 1)), session, &rowErr);
        }
        if (rowErr.has_value()) {
          blocks.push_back(*rowErr);
          return blocks;
        }
      }
      if (info.stringTyped) {
        blocks.push_back(ojson(eval::unsupportedCommand(
            "bysort gen (string-typed)", std::nullopt, std::nullopt, "piet_gap",
            eval::UnsupportedCommandKind::kNotImplemented)));
        return blocks;
      }
      // BUG-112(1) PIET-half: `bysort v: gen NEW = ...` is still the `generate`
      // verb, so it takes the identical exists-guard and message. The `replace`
      // sub-command sharing this block keeps overwriting, which is its job.
      if (!isReplace) {
        if (auto genExists = genTargetNameError(session, genTargetName(info.expr),
                                                strippedTypeWord(subJoined, info))) {
          blocks.push_back(*genExists);
          return blocks;
        }
      }
      // BS-002: an explicit parenthetical sort key (if present) overrides
      // the sortFirst-driven default sort() clause — see makeBygenExpr's
      // by()/sort() split (egen.cpp) and BysortPrefix::sortVars above.
      std::optional<std::vector<std::string>> sortVarsOpt;
      if (!bp.sortVars.empty()) sortVarsOpt = bp.sortVars;
      // BUG-532: under `bysort`, runBysortGen sorts the rows in place by KEYS
      // SORT first (Stata's order and marker), then computes.
      // BUG-471 T2: the same resolver as the plain arms (bysort_gen_untyped :686,
      // bysort_gen_double :700); a by-prefixed replace keeps the variable's type.
      const std::string byStoreType =
          isReplace ? storage::resolveReplaceType(session, genTargetName(info.expr))
                    : storage::resolveCreateType(session, info.typeToken);
      // BUG-1436: a string condition is one the engine's --bygen cannot read
      // (`bysort g: replace x = 10 if strlen(s) > 2` was rc 1 `function strlen()
      // is not supported`; StataBE 19.5 replaces 3 rows). It reads one row at a
      // time, so the whole-frame row mask is every group's answer and follows the
      // rows through the sort, as on the by-driver below. `_n`/`_N` count within
      // the group: the mask reads them numbered per group, over rows already in
      // bysort's order (runBysortGen then finds them sorted).
      if (ifExpr.has_value() && session.hasData() &&
          strmutate::needsConditionMask(session, *ifExpr)) {
        const bool groupObs = std::regex_search(*ifExpr, std::regex(R"(\b_[nN]\b)"));
        if (groupObs && bp.sortFirst) {
          if (auto sortErr = handlers::sortForBysort(session, bp.byVars, sortVarsOpt)) {
            blocks.push_back(*sortErr);
            return blocks;
          }
        }
        strmutate::ConditionMaskInstall mi = strmutate::installConditionMask(
            session, *ifExpr, groupObs ? bp.byVars : std::vector<std::string>{});
        if (!mi.ok) {
          blocks.push_back(mi.error);
          return blocks;
        }
        conditionMask.columns.push_back(mi.column);
        ifExpr = mi.condition;
      }
      ojson mutBlock = handlers::runBysortGen(session, info.expr, bp.byVars, ifExpr, bp.sortFirst,
                                              isReplace, /*inRange=*/std::nullopt, sortVarsOpt,
                                              byStoreType, noPromote);
      blocks.push_back(mutBlock);
      // BUG-250: `bysort id: gen double lagy = ...` is still the generate verb
      // (p2perf_bench.do:207-208 is exactly this shape). replace declares no
      // type in Stata, so only the gen half records.
      if (!isReplace) {
        recordDeclaredStataType(session, genTargetName(info.expr),
                                withSessionDefaultType(session, info), blocks.back());
        appendGenMissingNote(blocks, genTargetName(info.expr));  // BUG-1123
      }
      return blocks;
    }

    if (subCmdLower == "egen" && !bp.byVars.empty()) {
      auto [head, opts] = splitTopLevelComma(bp.subRaw);
      std::string byClause = "by(" + joinSpace(bp.byVars) + ")";
      std::string inner;
      static const std::regex reByOpt(R"(\bby\s*\()", std::regex::icase);
      if (!opts.has_value()) {
        inner = trimWs(head) + ", " + byClause;
      } else if (std::regex_search(*opts, reByOpt)) {
        inner = bp.subRaw;  // explicit by() wins
      } else {
        inner = trimWs(head) + ", " + trimWs(*opts) + " " + byClause;
      }
      // BUG-532: Stata's `bysort KEYS (SORT): egen` sorts the data in place by
      // KEYS SORT first and leaves them sorted (see runBysortGen); egen's own
      // by() then computes on those rows.
      if (bp.sortFirst) {
        std::vector<std::string> sortKeys = bp.byVars;
        sortKeys.insert(sortKeys.end(), bp.sortVars.begin(), bp.sortVars.end());
        if (std::optional<ojson> sortErr = handlers::sortInStataOrder(session, sortKeys)) {
          blocks.push_back(*sortErr);
          return blocks;
        }
      }
      ojson innerBlocks = dispatch(inner, session);
      for (const auto& b : innerBlocks) {
        if (!(b.is_object() && b.value("type", "") == "echo")) blocks.push_back(b);
      }
      return blocks;
    }

    // AER-SCOPE1 bundle I (UTIL): `bysort id: carryforward y, gen(f)` and
    // `bysort id: tsspell x` reach their handlers the way egen does -- the
    // by-variables are injected as a `by()` option and the line is
    // re-dispatched. Both handlers are group-aware; the prefix supplies the
    // grouping, not a loop.
    //
    // It sorts for BOTH `bysort` and plain `by:`, which is the same choice the
    // `by: summarize` branch below makes and for a stronger reason. Real Stata
    // refuses a plain `by:` on unsorted data (r(5), "not sorted"); replicating
    // that needs a sortedness oracle PIET does not expose to this layer -- the
    // `sorted_by` claim lives in the .hbin header and has no accessor here. Of
    // the two ways to be wrong, sorting is the safe one PRECISELY BECAUSE these
    // two verbs are order-dependent: carryforward propagates the last
    // non-missing value forward and tsspell walks runs, so running them on an
    // unsorted frame produces a different, entirely plausible answer. Sorting
    // makes them right; refusing would only be more faithful.
    //
    // BUG-623: the sort is on the by-keys AND the parenthesised sort keys.
    // `bysort id (t): carryforward y, gen(f)` sorted by id alone, so the fill
    // walked each id in whatever order the rows happened to be in, not by t
    // (main, t = 4 3 2 1 with y set only at t = 1: f filled 1 row, Stata 4).
    // Stata's `bysort id (t):` sorts by id t, and a plain `by id (t):` needs
    // the data in that order.
    // BUG-1015: cumul is byable(onecall) too -- its by() option IS the prefix
    // (cumul's by() option takes the prefix's variables), so it rides the same injection.
    // ipolate is byable(onecall) the same way (a by() option beside the prefix
    // is r(190)); its own
    // `sort` attribute restores the order, so the bysort order is the one left.
    // w2d_uncovered.log ipolate_by_generate_090 / ipolate_bys_gen_ctl `0|1|3|84|`.
    // AER tier-1: `by id: fillmissing x, with(any)` (023, all 35 lines) and
    // `bysort g: astile q = x, nq(3)` ride the same injection; both handlers
    // read by() as their grouping.
    if ((subCmdLower == "carryforward" || subCmdLower == "tsspell" || subCmdLower == "cumul" ||
         subCmdLower == "ipolate" || subCmdLower == "ipol" || subCmdLower == "ipo" ||
         subCmdLower == "fillmissing" || subCmdLower == "astile") &&
        !bp.byVars.empty()) {
      if (subCmdLower.rfind("ipo", 0) == 0) {
        auto [ihead, iopts] = splitTopLevelComma(bp.subRaw);
        (void)ihead;
        static const std::regex reIpoBy(R"((^|[\s,])by\s*\()", std::regex::icase);
        if (iopts.has_value() && std::regex_search(*iopts, reIpoBy)) {
          blocks.push_back(ojson(eval::errorBlock(
              "syntax_error", "option by() may not be combined with by prefix", std::nullopt,
              std::nullopt, std::nullopt, "r(190)")));
          return blocks;
        }
      }
      std::vector<std::string> cfSortKeys = bp.byVars;
      cfSortKeys.insert(cfSortKeys.end(), bp.sortVars.begin(), bp.sortVars.end());
      ojson sortBlocks = dispatch("sort " + joinSpace(cfSortKeys), session);
      for (const auto& b : sortBlocks) {
        if (b.is_object() && b.value("type", "") == "error") {
          blocks.push_back(b);
          return blocks;
        }
      }
      auto [head, opts] = splitTopLevelComma(bp.subRaw);
      std::string byClause = "by(" + joinSpace(bp.byVars) + ")";
      std::string inner;
      static const std::regex reByOpt2(R"(\bby\s*\()", std::regex::icase);
      if (!opts.has_value()) {
        inner = trimWs(head) + ", " + byClause;
      } else if (std::regex_search(*opts, reByOpt2)) {
        // fillmissing.ado refuses the pair -- two lines `as error`, then a
        // plain `exit`, so rc 0 and nothing filled (StataBE, review of the
        // tier-1 GEN lane, review-gen/ga.log A5). The data stay sorted, as
        // the bysort prefix left them.
        if (subCmdLower == "fillmissing") {
          for (const char* line : {"Options bysort and by() cannot be combined",
                                   "Use either bysort or by(varlist)"}) {
            ojson d;
            d["type"] = "display";
            d["text"] = line;
            blocks.push_back(d);
          }
          return blocks;
        }
        inner = bp.subRaw;  // explicit by() wins, same rule as egen
      } else {
        inner = trimWs(head) + ", " + trimWs(*opts) + " " + byClause;
      }
      ojson innerBlocks = dispatch(inner, session);
      for (const auto& b : innerBlocks) {
        if (!(b.is_object() && b.value("type", "") == "echo")) blocks.push_back(b);
      }
      return blocks;
    }

    // BUG-532: `by/bysort KEYS [(SORT)]: keep if COND` and `drop if COND` — the
    // only by-able forms of keep/drop ([D] drop). COND is evaluated per by-group
    // (group-local _n/_N, x[k], sum()); a missing COND counts as true. Oracle:
    // tests/eval/golden/aer_w2/w2_bykeep.log.
    //
    // Order is established HERE, the way Stata does it: `bysort` sorts the data
    // in place on KEYS++SORT in Stata's order (a string key by its bytes, not by
    // the engine's first-seen dictionary code — arm bys_p_str) and leaves it
    // sorted; plain `by` requires the rows to be in that order already and
    // refuses with r(5) otherwise, data untouched. For plain `by`, "in order" is
    // Stata's sortedby verdict as far as PIET can know it: the engine file's own
    // sort claim (engineSortClaim — arms by_p_after_bysgen, by_two_halfsorted),
    // else the rows themselves, measured under Stata's string order or the
    // engine's (keysAlreadyGrouped; the latter is the order PIET's own `sort` on
    // a string key leaves). The level-string, first-key-only check the summarize
    // branch below uses would call a string key PIET just sorted "not sorted".
    if ((subCmdLower == "keep" || subCmdLower == "drop") && !bp.byVars.empty()) {
      eval::ParsedCommand subParsed;
      try {
        subParsed = eval::tokenize(bp.subRaw);
      } catch (const eval::TokenizerError& te) {
        blocks.push_back(ojson(te.block));
        return blocks;
      }
      if (auto err = eval::rejectUnknownOptions(subParsed.opts, {})) {
        blocks.push_back(ojson(*err));
        return blocks;
      }
      if (subParsed.inRange.has_value()) {  // arms by_keep_in / by_keep_if_in
        blocks.push_back(ojson(eval::errorBlock("syntax_error",
                                                "'in' may not be combined with 'by'",
                                                "in " + *subParsed.inRange, std::nullopt,
                                                std::nullopt, "r(190)")));
        return blocks;
      }
      // `by g: keep VARLIST` / `by g: drop VARLIST` is r(198) invalid syntax in
      // Stata (arms by_keep_varlist / by_drop_varlist): only the `if` form is
      // by-able.
      if (!subParsed.ifExpr.has_value() || trimWs(*subParsed.ifExpr).empty() ||
          !subParsed.varlist.empty()) {
        blocks.push_back(ojson(eval::errorBlock(
            "syntax_error",
            "invalid syntax — under `by:` only `" + subCmdLower + " if EXP` is allowed; `" +
                subCmdLower + " VARLIST` takes no by-prefix",
            subCmdTyped, std::nullopt, std::nullopt, "r(198)")));
        return blocks;
      }
      std::optional<ojson> rowErr;
      const std::string cond = substituteRowExprRefs(*subParsed.ifExpr, session, &rowErr);
      if (rowErr.has_value()) {
        blocks.push_back(*rowErr);
        return blocks;
      }
      if (!session.hasData()) {
        blocks.push_back(ojson(eval::noDataLoaded()));
        return blocks;
      }
      Datastore* baseDs = session.datastore();
      if (baseDs == nullptr) {
        blocks.push_back(ojson(eval::noDataLoaded()));
        return blocks;
      }
      std::vector<std::string> keys = bp.byVars;
      keys.insert(keys.end(), bp.sortVars.begin(), bp.sortVars.end());
      for (const auto& k : keys) {
        if (!baseDs->has(k)) {  // arms bys_nosuchvar / bys_p_nosuchkey: r(111)
          blocks.push_back(ojson(eval::engineError("variable " + k + " not found")));
          return blocks;
        }
      }
      // Stata orders the missing values of a numeric key `. < .a < ... < .z`
      // and groups each kind apart. The engine's gsort/by() collapse every
      // missing kind into one NaN (src/data/frame2.cpp Frame::gsort's BUG-272
      // comment), and so does keysAlreadyGrouped: a key holding an extended
      // missing could be grouped and ordered differently from Stata, so it is
      // refused by name before anything moves (the sort primitive itself
      // belongs to the engine lane).
      for (const auto& k : keys) {
        if (baseDs->dtype(k) != "f64") continue;
        const double* v = baseDs->col(k).f64();
        for (int64_t r = 0; r < baseDs->nRows(); ++r) {
          if (cecon::missing_kind(v[r]) > 0) {
            blocks.push_back(ojson(eval::errorBlock(
                "unsupported_command",
                "`" + std::string(bp.sortFirst ? "bysort" : "by") + " ...: " + subCmdLower +
                    " if` is not supported yet when a key variable holds extended missing "
                    "values (.a-.z): `" + k +
                    "` does, and this engine would order and group every missing kind as "
                    "one value.",
                std::nullopt, std::nullopt, std::nullopt, "piet_gap")));
            return blocks;
          }
        }
      }
      // `bysort` sorts in place on KEYS++SORT (runByKeepIf, through runSort),
      // and the marker becomes KEYS++SORT (w2e_probes.log bo_sort_prefix_noop:
      // a coarser `sort g` on data sorted by g x leaves "g"). Plain `by`
      // never sorts: it runs when Stata's sortedby marker --
      // the engine file's own claim, which every `bysort` now leaves on PIET's
      // physically sorted rows -- leads with KEYS++SORT (arms by_p_after_bysgen,
      // by_p_sorted), and is r(5) otherwise (by_p_wrongkey, by_two_halfsorted).
      // With no claim at all (a string key -- the engine mints none -- or data
      // from a claimless transform) the rows themselves are measured: the named
      // permissive residual, rc 0 where Stata may answer r(5).
      std::vector<std::string> sortOn;  // empty = the rows are in order already
      if (bp.sortFirst) {
        sortOn = keys;
      } else {
        bool inOrder = false;
        if (const std::vector<std::string> claim = engineSortClaim(*baseDs); !claim.empty()) {
          inOrder = keysLeadClaim(keys, claim);
        } else {
          inOrder = keysAlreadyGrouped(*baseDs, keys, /*stataStringOrder=*/true) ||
                    keysAlreadyGrouped(*baseDs, keys, /*stataStringOrder=*/false);
        }
        if (!inOrder) {  // arms by_unsorted / by_p_wrongkey / by_drop_unsorted / by_two_halfsorted
          blocks.push_back(ojson(eval::errorBlock(
              "not_sorted",
              "not sorted — `by " + joinSpace(bp.byVars) +
                  (bp.sortVars.empty() ? std::string() : " (" + joinSpace(bp.sortVars) + ")") +
                  ":` requires the data to already be sorted by " + joinSpace(keys) +
                  ". Use `bysort` to sort first, or run `sort " + joinSpace(keys) +
                  "` before this command.",
              std::nullopt, std::nullopt, std::nullopt, "r(5)")));
          return blocks;
        }
      }
      // baseDs is not used past this point: the handler may replace the head.
      blocks.push_back(ojson(handlers::runByKeepIf(session, cond, bp.byVars, sortOn,
                                                   /*drop=*/subCmdLower == "drop")));
      return blocks;
    }

    // BS-092 (BETA campaign, Phase G): `by:`/`bysort: summarize` — the ONE
    // OTHER subcommand (besides gen/replace/egen above) that gets a genuine
    // PIET-side per-group implementation this phase; the other 7
    // Stata-legal-but-unimplemented stems the fallback message below still
    // names (correlate/pwcorr, sdtest, ttest, tabstat, tabulate family,
    // count, list) stay OUT of this batch's scope (BS-107, unchanged).
    //
    // Real Stata's `by G: summarize X` sorts by G (`bysort` re-sorts in
    // place; plain `by` requires the data ALREADY sorted, erroring "not
    // sorted" otherwise) then re-runs `summarize` once per contiguous
    // distinct-G block, printing a `-> G = value` header ahead of each
    // group's own stats table (live-verified this stint, StataBE:
    // `bysort state: summarize beertax` -> "-> state = al" then the usual
    // Variable/Obs/Mean/... table). This sorts UNCONDITIONALLY for both
    // `by` and `bysort` (the only divergence from real Stata is a plain
    // `by:` on genuinely unsorted data, where Stata refuses to run at all —
    // silently sorting instead of replicating that reject is the
    // conservative choice: it always answers the CORRECTLY grouped
    // question rather than risking a wrong one) into a FRESH scratch copy
    // (piet::handlers::sortForGrouping) — summarize is read-only, so the
    // live session dataset's row order must never change as a side effect.
    // (BUG-532, 2026-09-26: that holds for a plain `by:` only. Stata's
    // `bysort` sorts the data in place, and so does this driver now, through
    // handlers::sortInStataOrder below; the copy is then skipped.)
    // BUG-105 abbreviation stopgap: `su` is matched here alongside `sum`
    // so `bysort G: su X` reaches this real per-group implementation. Left
    // out, it would fall through to the honest "by:/bysort: is only
    // implemented for gen/replace/egen/summarize" reject below — true of
    // the token, false of the command it abbreviates.
    // P4 wave: the same driver now also carries `regress`/`reg`. Everything from
    // the sort through the boundary scan and the per-group slice is command-
    // agnostic — only the option net and the one per-group call below differ —
    // so `regress` graduates by joining this condition rather than by growing a
    // second copy of the group machinery. (ROADMAP P4: "generic group driver".)
    // W:P5: `tabstat` joins on the same terms regress did in P4 — the sort,
    // the boundary scan and the per-group slice are command-agnostic, so the
    // only additions are one flag and one per-group call below. Real Stata
    // prints a `-> region = N` header then that group's own table, which is
    // exactly the shape this driver already emits.
    const bool byIsSummarize =
        (subCmdLower == "summarize" || subCmdLower == "sum" || subCmdLower == "su");
    const bool byIsRegress = (subCmdLower == "regress" || subCmdLower == "reg");
    const bool byIsTabstat = (subCmdLower == "tabstat");
    // `count` joins the same driver (FU-163): it takes no varlist and no options,
    // and Stata leaves r(N) = the LAST group's count.
    const bool byIsCount = (subCmdLower == "count");
    if (byIsSummarize || byIsRegress || byIsTabstat || byIsCount) {
      if (bp.byVars.empty()) {
        blocks.push_back(ojson(eval::engineError("bysort/by: no group variable(s) given")));
        return blocks;
      }
      eval::ParsedCommand subParsed;
      try {
        subParsed = eval::tokenize(bp.subRaw);
      } catch (const eval::TokenizerError& te) {
        blocks.push_back(ojson(te.block));
        return blocks;
      }
      // Stata's own refusal comes first: summarize's [iw=] is illegal with
      // detail, under by: too (StataBE r(101), BUG-1444; the unprefixed rule
      // is QF-SUM-IW below).
      const bool subDetail = std::any_of(
          subParsed.opts.begin(), subParsed.opts.end(), [](const auto& kv) {
            return (kv.first == "detail" || kv.first == "d") && kv.second.isBool &&
                   kv.second.boolValue;
          });
      if (byIsSummarize && subDetail && subParsed.weightType.has_value() &&
          (lower(*subParsed.weightType) == "iw" || lower(*subParsed.weightType) == "iweight")) {
        blocks.push_back(ojson(eval::errorBlock("engine_error", "iweights not allowed",
                                                std::nullopt, std::nullopt, std::nullopt,
                                                "r(101)")));
        return blocks;
      }
      // BUG-1520: summarize's [aw=]/[fw=]/[iw=] and tabstat's [aw=]/[fw=] run in
      // each group with the engine weight flag their unprefixed arms pass
      // (StataBE 19.5: `by g: summarize x [aw=wf]` leaves the last group's r(N)
      // 4, r(sum_w) 6.5). Any other weighted by-prefixed summarize/tabstat/count
      // is refused by name rather than run unweighted (a by-line's weight
      // passes the tokenizer under the sub-command's policy).
      std::string byWeightFlag;
      if (subParsed.weightType.has_value() && subParsed.weightVar.has_value() &&
          (byIsSummarize || byIsTabstat)) {
        const std::string wt = lower(*subParsed.weightType);
        if (wt == "aw" || wt == "aweight") byWeightFlag = "--aweight";
        else if (wt == "fw" || wt == "fweight") byWeightFlag = "--fweight";
        else if (byIsSummarize && (wt == "iw" || wt == "iweight")) byWeightFlag = "--iweight";
      }
      if (!byIsRegress && subParsed.weightType.has_value() && byWeightFlag.empty()) {
        blocks.push_back(ojson(eval::unsupportedOption(
            "[" + *subParsed.weightType + "=" + subParsed.weightVar.value_or("") + "]",
            "a weight on a by-prefixed `" + subCmdTyped +
                "` is not implemented in this version of PIET; run it separately for each group.")));
        return blocks;
      }
      std::optional<std::string> tabstatIfAfterRows;  // see the `_rc` note below
      // e(sample) T2/T4: the per-group `if` reaches applyIfAndInFilter as raw
      // text (below), past the whole-line rewrite, so it takes the SAME row
      // rewrite every other `if` takes (substituteRowExprRefs: e(sample) first,
      // then _b[]/_se[]/e()/scalar()/stored scalars). Before T4 only e(sample)
      // was rewritten here, and `by id: count if x > scalar(s)` / `... if x >
      // _b[x]` reached the engine as text (rc 1 `function scalar() is not
      // supported`, r(111) `unknown variable _b`). summarize/tabstat/count leave
      // e() alone, so the text resolved once is every group's. A by-prefixed
      // REGRESS replaces e() after each group, so a later group's `if` reads the
      // previous group's fit in Stata; that is not followed here and is refused
      // by name (r(198)) instead of answered with the pre-loop results.
      if (subParsed.ifExpr.has_value()) {
        const std::string ie = *subParsed.ifExpr;  // a copy: the `if` is replaced below
        // BUG-1518: a `_b[`/`_se[` that ends a longer name (`g_b[1]`) is that
        // variable's subscript, not a coefficient.
        auto coefRef = [&ie](const char* tok) {
          for (size_t p = ie.find(tok); p != std::string::npos; p = ie.find(tok, p + 1))
            if (p == 0 || !cecon::ident::isIdentByte(ie[p - 1])) return true;
          return false;
        };
        if (byIsRegress && (eval::hasEsampleToken(ie) || coefRef("_b[") || coefRef("_se[") ||
                            eval::hasBareEParen(ie))) {
          blocks.push_back(ojson(eval::errorBlock(
              "engine_error",
              "e(sample), _b[], _se[] and e() in the `if` of a by-prefixed estimation command are "
              "not available in this version of PIET: each group's fit replaces e() before the "
              "next group's `if` is read.",
              std::nullopt, std::nullopt, std::nullopt, "r(198)")));
          return blocks;
        }
        auto prepareIf = [&](std::string& out) -> bool {
          std::optional<ojson> rowErr;
          out = substituteRowExprRefs(ie, session, &rowErr, !byIsRegress);
          if (rowErr.has_value()) {
            blocks.push_back(*rowErr);
            return false;
          }
          // The string-condition route (see dispatchImpl's QualifierColumnGuard): a
          // string function reads one row at a time, so the whole-frame mask is
          // every group's answer, and it follows the rows through the sort below
          // (qf_if_by_sum_strpos / qf_if_by_count_strpos; FU-501). `_n`/`_N` are
          // group-relative under by: and a whole-frame mask cannot carry them.
          if (strmutate::needsConditionMask(session, out) && session.hasData() &&
              !std::regex_search(out, std::regex(R"(\b_[nN]\b)"))) {
            strmutate::ConditionMaskInstall mi = strmutate::installConditionMask(session, out);
            if (!mi.ok) {
              blocks.push_back(mi.error);
              return false;
            }
            conditionMask.columns.push_back(mi.column);
            out = mi.condition;
          }
          return true;
        };
        std::string prepared;
        if (!prepareIf(prepared)) return blocks;
        subParsed.ifExpr = prepared;
        // tabstat is an ado command: each group's run leaves `_rc` 0 once it
        // has rows to tabulate (a group with none leaves it as it was), so only
        // the groups up to the first with rows read the caller's `_rc` (StataBE
        // 19.5, 2026-11-03, three groups, caller _rc 111: `by g: tabstat x if
        // _rc == 111` tabulates the first group only, `... if _rc == 0` none,
        // `... if _rc == 0 | g == 0` all three). The `if` the later groups read
        // is the same text with `_rc` 0.
        const std::string callerRc = session.eval.locals.get("_rc", "0");
        if (byIsTabstat && callerRc != "0" && substituteRcValue(ie, session) != ie) {
          session.eval.locals.set("_rc", "0");
          std::string afterRows;
          const bool ok = prepareIf(afterRows);
          session.eval.locals.set("_rc", callerRc);
          if (!ok) return blocks;
          tabstatIfAfterRows = afterRows;
        }
      }
      bool detail = false;
      bool byMeanonly = false;
      if (byIsCount) {
        // The plain `count` arm's two nets, in the same order: leftover tokens
        // (the BUG-010 fixed-arity guard) then the empty option vocabulary
        // (BS-097). Both are rc 198; neither may degrade to rc 1.
        if (!subParsed.varlist.empty()) {
          blocks.push_back(ojson(eval::engineError(
              bp.subCmd + ": unexpected extra tokens after `count [if] [in] — takes no variable "
                       "list`: " + joinVarlist(subParsed.varlist) + ".",
              "syntax_error")));
          return blocks;
        }
        if (auto err = eval::rejectUnknownOptions(subParsed.opts, {})) {
          blocks.push_back(ojson(*err));
          return blocks;
        }
      }
      if (byIsSummarize) {
        for (const auto& [k, v] : subParsed.opts) {
          if ((k == "detail" || k == "d") && v.isBool && v.boolValue) detail = true;
          if (k == "meanonly" && (!v.isBool || v.boolValue)) byMeanonly = true;
        }
        // Mirrors the plain `summarize` arm's own option net (BS-097) exactly
        // — detail/d/meanonly only, anything else an honest r(198)-parity
        // reject naming the token.
        for (const auto& [k, v] : subParsed.opts) {
          (void)v;
          if (k == "detail" || k == "d" || k == "meanonly") continue;
          blocks.push_back(
              ojson(eval::engineError("option `" + k + "` not allowed", "unsupported_option")));
          return blocks;
        }
      }
      // regress has no option net here on purpose: runRegress owns its own
      // (vce/robust/cluster/level/noconstant/absorb…), and duplicating it would
      // be a second copy to drift out of sync. A bad option surfaces from the
      // per-group call with regress's own wording.
      // tabstat likewise: its own arm below owns the stat/statistics/columns
      // net, and this driver forwards the parsed options to the same handler.
      std::optional<std::string> byTabstatStats;
      bool byTabstatSave = false;
      std::vector<std::string> byTsKeys, byTsRows;
      if (byIsTabstat) {
        for (const auto& [k, v] : subParsed.opts) {
          if ((k == "stat" || k == "stats" || k == "statistics") && !v.isBool) {
            byTabstatStats = v.strValue;
          } else if (k == "columns" && !v.isBool) {
            // Presentation-only in Stata (which axis carries the statistics);
            // PIET renders one fixed layout, so accept and note rather than
            // reject a legal option or silently swallow it.
            continue;
          } else if (k == "save" && v.isBool) {
            byTabstatSave = v.boolValue;
          } else {
            blocks.push_back(
                ojson(eval::engineError("option `" + k + "` not allowed", "unsupported_option")));
            return blocks;
          }
        }
        // `by G: tabstat X, by(H)` is illegal in Stata (two grouping axes);
        // the by() option cannot reach here anyway — the prefix owns grouping.
        // BUG-1520: the unprefixed arm's statistic words and row names.
        if (const auto bad = parseTabstatStats(byTabstatStats, byTsKeys, byTsRows)) {
          blocks.push_back(ojson(eval::errorBlock("syntax_error", "unknown statistic: " + *bad,
                                                  std::nullopt, std::nullopt, std::nullopt,
                                                  "r(198)")));
          return blocks;
        }
        byTabstatStats = std::string();
        for (const auto& k : byTsKeys) *byTabstatStats += (byTabstatStats->empty() ? "" : " ") + k;
      }
      if (!session.hasData()) {
        blocks.push_back(ojson(eval::noDataLoaded()));
        return blocks;
      }
      Datastore* baseDs = session.datastore();
      if (baseDs == nullptr) {
        blocks.push_back(ojson(eval::noDataLoaded()));
        return blocks;
      }
      for (const auto& bv : bp.byVars) {
        if (!baseDs->has(bv)) {
          blocks.push_back(ojson(eval::engineError("variable " + bv + " not found")));
          return blocks;
        }
      }

      // BUG-157 (P4 wave): plain `by` REQUIRES the data to already be sorted by
      // the by-variables; Stata refuses with r(5) "not sorted" otherwise, and
      // that is exactly what `bysort` exists to avoid. BS-092 sorted
      // unconditionally for both spellings and called the silent sort "the
      // conservative choice"; it is not — it answers a question the user's
      // program did not ask for and hides a real bug in their code. `bysort`
      // still sorts, unchanged.
      //
      // Sortedness is MEASURED here (adjacent-row comparison on the base data),
      // never inferred from a stored flag: a flag can be stale after any verb
      // that reorders rows, and a wrong "already sorted" answer would put rows
      // from two different groups into one fit.
      if (!bp.sortFirst) {
        std::string unsortedOn;
        const int64_t nBase = baseDs->nRows();
        for (const auto& bv : bp.byVars) {
          const cecon::Column& c = baseDs->col(bv);
          const bool isFac = baseDs->dtype(bv) != "f64";
          std::vector<std::string> levels;
          if (isFac) levels = baseDs->varinfo(bv).levels;
          auto keyAt = [&](int64_t r) -> std::string {
            if (!isFac) return std::string();
            int32_t code = c.codes()[r];
            return (code >= 0 && static_cast<size_t>(code) < levels.size())
                       ? levels[static_cast<size_t>(code)]
                       : std::string("\xff");  // missing sorts last, like Stata
          };
          for (int64_t r = 1; r < nBase; ++r) {
            bool decreasing;
            if (isFac) {
              decreasing = keyAt(r) < keyAt(r - 1);
            } else {
              // Stata orders missing AFTER every number, so a NaN following a
              // number is sorted, and a number following a NaN is not.
              double a = c.f64()[r - 1], b = c.f64()[r];
              bool an = std::isnan(a), bn = std::isnan(b);
              decreasing = (an && !bn) ? true : ((!an && !bn) ? (b < a) : false);
            }
            if (decreasing) { unsortedOn = bv; break; }
          }
          if (!unsortedOn.empty()) break;
          // Only the FIRST by-variable can be checked independently; a later one
          // need only be sorted within each block of the earlier ones. Checking
          // it globally would reject legitimately sorted data, so stop here —
          // this catches the filed defect (a `by:` on data never sorted at all)
          // without inventing a stricter rule than Stata's.
          break;
        }
        if (!unsortedOn.empty()) {
          blocks.push_back(ojson(eval::errorBlock(
              "not_sorted",
              "not sorted — `by " + joinSpace(bp.byVars) + ":` requires the data to already be "
              "sorted by " + joinSpace(bp.byVars) + " (`" + unsortedOn + "` is not in order). "
              "Use `bysort " + joinSpace(bp.byVars) + ":` to sort first, or run `sort " +
              joinSpace(bp.byVars) + "` before this command.",
              std::nullopt, std::nullopt, std::nullopt, "r(5)")));
          return blocks;
        }
      }

      // BUG-532: Stata's `bysort KEYS [(SORT)]:` sorts the data in place, and
      // leaves them marked sorted by KEYS++SORT, before it runs the command. So
      // does this driver (handlers::sortInStataOrder: a string key in byte order;
      // the engine moves no row when the marker already leads with KEYS++SORT).
      // It used to sort only a scratch copy, leaving PIET's rows where Stata's
      // were not.
      // The keys were resolved above (abbreviations, review of d82bdfa6); a
      // parenthesised key that names no variable is r(111), as in Stata.
      if (bp.sortFirst) {
        std::vector<std::string> sortKeys = bp.byVars;
        sortKeys.insert(sortKeys.end(), bp.sortVars.begin(), bp.sortVars.end());
        for (const auto& sv : bp.sortVars) {
          if (!baseDs->has(sv)) {
            blocks.push_back(ojson(eval::engineError("variable " + sv + " not found")));
            return blocks;
          }
        }
        if (std::optional<ojson> sortErr = handlers::sortInStataOrder(session, sortKeys)) {
          blocks.push_back(*sortErr);
          return blocks;
        }
        baseDs = session.datastore();
        if (baseDs == nullptr) {
          blocks.push_back(ojson(eval::noDataLoaded()));
          return blocks;
        }
      }

      // P2-PERF ARC-C C1-③: skip the sort when it is PROVABLY the identity.
      //
      // sortForGrouping exists to make same-group rows physically contiguous,
      // and it pays a full re-sort plus a full --out write of the whole dataset
      // to do it — every time, including the overwhelmingly common case where
      // the user has just run `sort region` themselves. It cannot be skipped on
      // the strength of the `by`-spelling sortedness check a few lines up: that
      // check answers a DIFFERENT question (it compares factor LEVEL STRINGS
      // and inspects only the first by-variable, because its job is to reject a
      // user error, not to certify a permutation), and it does not run at all
      // under `bysort`.
      //
      // So certify it against gsort's OWN key semantics instead (the
      // keysAlreadyGrouped helper above, factored out for BUG-532). If the key
      // tuple is already non-decreasing under exactly that order, the
      // permutation gsort would return is the identity, so `take_rows` would
      // hand back a byte-identical frame and the whole round trip is dead work.
      // Any doubt at all — a key that dips — falls through to the sort, so
      // this can only ever remove a no-op.
      const bool alreadyGrouped = keysAlreadyGrouped(*baseDs, bp.byVars);

      std::string sortedPath;
      std::optional<Datastore> sortedOwned;
      Datastore* sortedPtr = nullptr;
      if (alreadyGrouped) {
        sortedPath = baseDs->path();
        sortedPtr = baseDs;
      } else {
        try {
          sortedPath = handlers::sortForGrouping(session, baseDs->path(), bp.byVars);
        } catch (const CobraError& ce) {
          blocks.push_back(ojson(eval::engineError(cobraErrorMessage(ce), ce.code)));
          return blocks;
        }
        sortedOwned.emplace(sortedPath);
        sortedPtr = &*sortedOwned;
      }
      Datastore& sortedDs = *sortedPtr;
      int64_t nRowsSorted = sortedDs.nRows();
      if (nRowsSorted == 0) {
        blocks.push_back(ojson(eval::engineError("no observations")));
        return blocks;
      }

      // Raw column views over the SORTED copy for group-boundary detection
      // (adjacent-row key comparison) and header-value formatting — mirrors
      // the raw cecon::Column access pattern used throughout this file
      // (e.g. the describe/summarize-fallback block a few hundred lines up).
      struct ByColView {
        std::string name;
        bool isFactor = false;
        const double* f64 = nullptr;
        const int32_t* codes = nullptr;
        // Owned COPY, not a pointer into Datastore::varinfo()'s return —
        // that accessor returns VarInfo BY VALUE (datastore.h), so taking
        // `&varinfo(...).levels` would dangle the instant the temporary
        // VarInfo is destroyed at the end of the full-expression.
        std::vector<std::string> levels;
      };
      std::vector<ByColView> byCols;
      for (const auto& bv : bp.byVars) {
        ByColView v;
        v.name = bv;
        v.isFactor = sortedDs.dtype(bv) != "f64";
        const cecon::Column& c = sortedDs.col(bv);
        if (v.isFactor) {
          v.codes = c.codes();
          v.levels = sortedDs.varinfo(bv).levels;
        } else {
          v.f64 = c.f64();
        }
        byCols.push_back(std::move(v));
      }
      auto rowKeyEqual = [&](int64_t a, int64_t b) {
        for (const auto& v : byCols) {
          if (v.isFactor) {
            if (v.codes[a] != v.codes[b]) return false;
          } else {
            double x = v.f64[a], y = v.f64[b];
            bool xn = std::isnan(x), yn = std::isnan(y);
            if (xn != yn) return false;
            if (!xn && x != y) return false;
          }
        }
        return true;
      };
      // P4 wave correction. BS-092 rendered numeric group values with
      // eval::pythonFloatRepr ("1.0"), disclosed at the time as a cosmetic
      // simplification on the grounds that there was "no prior art to match".
      // There is now: the P4 answerbook's real-Stata capture prints
      // `-> region = 1`, not `-> region = 1.0`
      // (internal capture p4_stata.log). An
      // integral-valued double therefore renders WITHOUT the trailing ".0",
      // exactly as tabulate.cpp's tableF64Label already does for the same
      // reason; non-integral values keep their decimals.
      auto formatGroupHeader = [&](int64_t row) {
        std::string out;
        for (size_t i = 0; i < byCols.size(); ++i) {
          if (i) out += " ";
          const ByColView& v = byCols[i];
          out += v.name + " = ";
          if (v.isFactor) {
            int32_t code = v.codes[row];
            out += (code >= 0 && static_cast<size_t>(code) < v.levels.size())
                       ? v.levels[static_cast<size_t>(code)]
                       : ".";
          } else {
            double x = v.f64[row];
            if (std::isnan(x)) {
              out += ".";
            } else if (x == std::floor(x) && std::fabs(x) < 1e15) {
              char buf[32];
              std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(x));
              out += buf;
            } else {
              out += eval::pythonFloatRepr(x);
            }
          }
        }
        return out;
      };

      // P2-PERF ARC-C C1-①: the row-index column every slice below filters on
      // is a property of `sortedPath`, not of the group — build it once.
      handlers::RowIndexCache rowIdx;
      // BUG-1166 (FU-498, FU-530): a regressor list carrying a time-series
      // operator must read the full series. A slice cut with the `if` already
      // applied holds only this group's rows that pass it, so `by id: regress y
      // L.x if t > 2` lost each group's first admitted row (its L.x is the t==2
      // value the slice dropped): e(N) 3 where StataBE fits 4 (measured
      // 2026-10-29). Such a fit gets the group's WHOLE slice and its `if` as a
      // pre-evaluated column, which the engine applies as --keepif after the
      // operators (runRegress's FU-475 route). Stata serves time-series
      // operators under by: only on data sorted by panel and time, so a group
      // that can be fitted is a whole panel: the slice holds its full series.
      std::vector<std::string> byRegressors;
      if (byIsRegress && !subParsed.varlist.empty())
        byRegressors.assign(subParsed.varlist.begin() + 1, subParsed.varlist.end());
      const bool tsRegress = byIsRegress && translate::hasTsToken(byRegressors);
      try {
        rowIdx = handlers::buildRowIndexCache(session, sortedPath);
      } catch (const CobraError& ce) {
        blocks.push_back(ojson(eval::engineError(cobraErrorMessage(ce), ce.code)));
        return blocks;
      }
      // The `if` is read once, over the whole sorted frame. summarize and count
      // are built-ins whose `if` sees the by-group: `_n`, `_N` and x[k] count
      // within it (StataBE 19.5, measured 2026-10-29 on two groups of six: `by
      // id: summarize x if _n > 2` r(N) 4, `by id: count if _n == 1` 1, `by id:
      // count if x[_n-1] < .` 5). regress and tabstat are ado commands that mark
      // their sample over the whole data, so their `_n` is the frame's (`by id:
      // regress y x if _n > 2` e(N) 6, as the slice's --keepif already counts
      // it); they need their `if` as a column only for the time-series fit's
      // engine --keepif, or when it is a string condition naming `_n`/`_N`,
      // which the pre-sort mask above could not take (BUG-1436).
      // Stata reads the `if` in the first group, so a condition that cannot be
      // evaluated is shown under that group's header.
      std::optional<ojson> ifError;
      handlers::RowIndexCache rowIdxAfterRows = rowIdx;  // tabstatIfAfterRows' frame
      std::optional<ojson> ifAfterRowsError;
      if (tabstatIfAfterRows.has_value() && !tabstatIfAfterRows->empty() &&
          strmutate::needsConditionMask(session, *tabstatIfAfterRows)) {
        ifAfterRowsError = handlers::addIfMaskColumn(session, rowIdxAfterRows,
                                                     *tabstatIfAfterRows, {});
        if (!ifAfterRowsError) tabstatIfAfterRows = rowIdxAfterRows.maskVar;
      }
      if (subParsed.ifExpr.has_value() && !subParsed.ifExpr->empty() &&
          (byIsSummarize || byIsCount || tsRegress ||
           strmutate::needsConditionMask(session, *subParsed.ifExpr))) {
        const std::vector<std::string> groupKeys =
            (byIsSummarize || byIsCount) ? bp.byVars : std::vector<std::string>{};
        ifError = handlers::addIfMaskColumn(session, rowIdx, *subParsed.ifExpr, groupKeys);
        if (!ifError) subParsed.ifExpr = rowIdx.maskVar;
      }

      // P2-PERF ARC-C C1-④: every read the sub-command makes below is against a
      // per-group SLICE, never against the session's head. Routing those to the
      // resident plane evicts the head and resyncs it once per group; say so
      // once, here, for the whole loop.
      SessionState::StatelessRead statelessGroupReads(session);

      // The LAST group's results are what Stata leaves in r() when the loop ends.
      std::optional<ojson> lastSummBlock;
      std::optional<int64_t> lastCountN;
      std::optional<ojson> lastTabstatBlock;
      bool tabstatSawRows = false;  // a group's tabstat has run on rows: `_rc` is 0

      int64_t g0 = 0;
      while (g0 < nRowsSorted) {
        int64_t g1 = g0 + 1;
        while (g1 < nRowsSorted && rowKeyEqual(g0, g1)) ++g1;

        ojson header;
        header["type"] = "display";
        header["text"] = "-> " + formatGroupHeader(g0);
        blocks.push_back(header);
        if (ifError) {
          blocks.push_back(*ifError);
          return blocks;
        }

        // Slice this group's own contiguous [g0+1, g1] row range (1-based,
        // inclusive) out of the sorted copy — reusing BS-090's shared
        // row-filter infra (piet::handlers::applyIfAndInFilter) purely for
        // its mechanical row-range materialization, ANDed with any `if`
        // the sub-command itself carried (`by G: summarize X if COND`
        // restricts EACH group's own sample by COND too, same as real
        // Stata — group membership itself is unaffected since it was
        // already fixed by the sort/boundary-scan above).
        std::string rangeStr = std::to_string(g0 + 1) + "/" + std::to_string(g1);
        if (tsRegress) {
          // BUG-1166: the group's whole slice; subParsed's `if` names the mask
          // column the slice keeps, and runRegress hands it to the engine as
          // --keepif, applied after the operators.
          std::string groupSlice;
          try {
            groupSlice = handlers::sliceKeepingIfMask(session, rowIdx, g0 + 1, g1);
          } catch (const CobraError& ce) {
            blocks.push_back(ojson(eval::engineError(cobraErrorMessage(ce), ce.code)));
            return blocks;
          }
          blocks.push_back(
              ojson(handlers::runRegress(subParsed, session, subCmdLower, &groupSlice)));
          g0 = g1;
          continue;
        }
        const bool afterRows = tabstatSawRows && tabstatIfAfterRows.has_value();
        if (afterRows && ifAfterRowsError) {
          blocks.push_back(*ifAfterRowsError);
          return blocks;
        }
        handlers::InRangeFilterResult fr;
        try {
          fr = handlers::applyIfAndInFilter(
              session, sortedPath, afterRows ? tabstatIfAfterRows : subParsed.ifExpr, rangeStr,
              /*complement=*/false, afterRows ? &rowIdxAfterRows : &rowIdx);
        } catch (const CobraError& ce) {
          blocks.push_back(ojson(eval::engineError(cobraErrorMessage(ce), ce.code)));
          return blocks;
        }
        if (!fr.ok) {
          blocks.push_back(ojson(handlers::inRangeRejectError(fr, rangeStr, "summarize")));
          return blocks;
        }
        if (byIsSummarize) {
          Datastore groupDs(fr.dataPath);
          std::optional<std::vector<std::string>> vl;
          if (!subParsed.varlist.empty()) vl = subParsed.varlist;
          lastSummBlock = ojson(
              byWeightFlag.empty()
                  ? handlers::summarizeCoreStats(groupDs, vl, detail, std::nullopt)
                  : handlers::summarizeCoreStatsWeighted(groupDs, vl, detail, subParsed.weightVar,
                                                         byWeightFlag.c_str()));
          blocks.push_back(*lastSummBlock);
        } else if (byIsCount) {
          // The slice already carries this group's rows AND the `if`.
          Datastore groupDs(fr.dataPath);
          lastCountN = groupDs.nRows();
          ojson out;
          out["type"] = "display";
          out["text"] = std::to_string(*lastCountN);
          blocks.push_back(out);
        } else if (byIsTabstat) {
          // The slice IS this group's sample (and already carries the
          // sub-command's own `if`), so pass no ifExpr — re-filtering here
          // would re-apply it against the slice.
          lastTabstatBlock = ojson(
              byWeightFlag.empty()
                  ? handlers::runTabstatOnData(subParsed.varlist, byTabstatStats, session,
                                               fr.dataPath)
                  : handlers::runTabstatOnDataWeighted(subParsed.varlist, byTabstatStats,
                                                       fr.dataPath, byWeightFlag,
                                                       *subParsed.weightVar));
          blocks.push_back(*lastTabstatBlock);
          if (lastTabstatBlock->value("n_obs", int64_t{0}) > 0) tabstatSawRows = true;
        } else {
          // The slice already carries this group's rows AND the sub-command's
          // own `if`, so runRegress must NOT re-filter it — hence the override.
          // Stata leaves the LAST group's fit in e()/`_b[]` when the loop ends,
          // which falls out of calling the ordinary handler once per group.
          blocks.push_back(
              ojson(handlers::runRegress(subParsed, session, subCmdLower, &fr.dataPath)));
        }

        g0 = g1;
      }
      if (lastSummBlock) captureSummarizeR(*lastSummBlock, byMeanonly, detail, session);
      if (lastCountN) session.eval.rValues.set("N", static_cast<double>(*lastCountN));
      // BUG-1520: r() is the last group's tabstat's -- empty, and with `save` its
      // r(StatTotal), as the unprefixed arm posts it (StataBE 19.5, x7.do: `by g:
      // tabstat x [aw=wf], statistics(n mean sd) save` leaves only r(StatTotal),
      // 3x1, rows N Mean SD: 4, 7.1538461538461542, 5.2171296528451023).
      if (tabstatSawRows) session.eval.locals.set("_rc", "0");
      if (lastTabstatBlock) {
        eval::replaceRClass(session.eval);
        std::vector<std::string> posted;
        const ojson& tb = *lastTabstatBlock;
        if (byTabstatSave && tb.value("type", std::string()) != "error" && tb.contains("total") &&
            tb["total"].is_object()) {
          std::vector<std::string> colVars;
          if (tb.contains("vars") && tb["vars"].is_array())
            for (const auto& vn : tb["vars"])
              if (vn.is_string()) colVars.push_back(vn.get<std::string>());
          session.eval.rMatrices.emplace_back(
              "StatTotal", tabstatStatMatrix(tb["total"], byTsKeys, byTsRows, colVars));
          posted.push_back("StatTotal");
        }
        eval::declareRPosted(session.eval, "tabstat", std::move(posted));
      }
      return blocks;
    }

    // BUG-041: any OTHER subcommand under `by:`/`bysort:` used to sort-then-
    // dispatch-once — a Gate-(1) silent-drop (the sub-command runs ONCE
    // over the whole, optionally re-sorted, dataset, not once per by-group,
    // so e.g. `bysort grp: summarize wage` quietly answered the SAME
    // question as unqualified `summarize wage` instead of producing 5
    // per-group tables). True by-group dispatch (genuinely repeating the
    // sub-command once per distinct by-value and stitching results
    // together) is real feature work, queued for unfreeze — this
    // stabilization fix is the other honest half of Gate (1): raise an
    // explicit error naming the qualifier instead of silently flattening
    // it. Only gen/replace/egen/summarize (handled above — summarize added
    // BS-092, BETA Phase G) have a genuine native by-group implementation
    // today; every other command reaching here falls back to this honest
    // rejection, matching the qualifier-matrix sweep's expectation
    // (REJECTED, not SILENT_DROP) across the board rather than an ad hoc
    // per-command allowlist.
    {
      std::string prefixWord = bp.sortFirst ? "bysort" : "by";
      std::string byVarsJoined = joinSpace(bp.byVars);
      // A3 (BETA campaign, BS-096): distinguish two structurally different
      // reasons a subcommand lands in this fallback — audit G3 finding #10
      // (SCOPE.md BS-096) confirmed today's single message ("not supported
      // yet") is actively misleading for 4 stems (codebook/describe/
      // levelsof/table) that real Stata itself refuses to combine with
      // by:/bysort: at all (CONFIRMED live: StataBE answers "may not be
      // combined with by" / "prefix by: not allowed", r(190)) — those will
      // NEVER be supported here no matter how much by-group dispatch work
      // lands, unlike the other 7 Stata-legal-but-not-yet-implemented stems
      // (correlate/pwcorr, sdtest, ttest, tabstat, tabulate family, count,
      // list — queued at BS-107; summarize graduated out of this list this
      // phase, BS-092). registry's byIllegalInStata field carries that
      // distinction (a lookup, not a second hardcoded list); anything NOT
      // found in the registry (e.g. a genuinely unrecognized token) or
      // found-but-not-flagged keeps the ORIGINAL "not supported yet"
      // wording unchanged.
      const registry::CommandSpec* subSpec = registry::findByName(subCmdLower);
      std::string msg;
      // BUG-105/WS-K: the LOOKUP uses the canonical spelling (so an
      // abbreviation gets the right verdict), the MESSAGE quotes what the user
      // typed (so today's bytes are unchanged for every non-abbreviated line).
      if (subSpec && subSpec->byIllegalInStata) {
        msg = "`" + prefixWord + " " + byVarsJoined + ": " + subCmdTyped +
              "` is not allowed — `" + subCmdTyped + "` may not be combined with a `by`/`bysort` "
              "prefix. This is a structural restriction in Stata itself, not a PIET version "
              "limitation, and will not become available in a later milestone. Remove the `" +
              prefixWord + "` prefix and run `" + subCmdTyped + "` on its own.";
      } else {
        msg = "`" + prefixWord + " " + byVarsJoined + ": " + subCmdTyped +
              "` is not supported yet — `by:`/`bysort:` grouped dispatch is only "
              "implemented for `gen`/`replace`/`egen`/`summarize`/`count`/`regress`/`tabstat`/`keep if`/`drop if` "
              "(and `carryforward`/`tsspell`) in this version of PIET. Run `" +
              subCmdTyped + "` separately for each group instead.";
      }
      blocks.push_back(ojson(eval::errorBlock("unsupported_command", msg, std::nullopt, std::nullopt,
                                              std::nullopt, "piet_gap")));
      return blocks;
    }
  }

  // ------------------------------------------------------------------
  // 1c. U5 (S6): user-program invocation — BEFORE the data guard (a program
  // may itself `use`/`sysuse` before touching data). Uses the tokenized
  // parsed.cmd (ORIGINAL case) for case-insensitive program-name lookup,
  // mirroring dispatcher.py's `_find_program(session, parsed.cmd)`.
  {
    // Inside an ado file's program, that file's private programs come first.
    std::optional<std::string> progMatch;
    if (!session.adoContext.empty())
      progMatch = program::findProgram(session, session.adoContext + "." + parsed.cmd);
    if (!progMatch.has_value()) progMatch = program::findProgram(session, parsed.cmd);
    // BUG-442/BUG-1445: a name that is neither a program nor a PIET command is
    // looked up as `<name>.ado` on the adopath (the current directory is on
    // it by default, and `adopath +` extends it), exactly as Stata resolves
    // an unknown command. The file is run once, silently; the program it
    // defines under the command's own name is then called, and its other
    // programs stay private to it. A file that does not define the command is
    // Stata's r(199) "command NAME not defined by NAME.ado".
    if (!progMatch.has_value() && registry::findByName(lower(parsed.cmd)) == nullptr &&
        std::regex_match(parsed.cmd, std::regex("[A-Za-z_][A-Za-z0-9_]{0,31}"))) {
      if (std::optional<std::string> adoFile =
              handlers::findOnAdopath(session, parsed.cmd + ".ado")) {
        // The programs as they were: a file's program that shares its name with
        // one already defined (a helper `v1help` beside the user's own) is the
        // file's private one, and the user's stays as it was (StataBE `program
        // dir` lists `v1cmd.v1help` and `v1help` apart).
        const auto before = session.programs;
        doexec::ExecuteFileResult fres;
        {
          doexec::PrivateLocalScope privateScope(session);
          // A `version` at the file's top level is the file's own (StataBE: the
          // caller's 16 holds inside the command and after it).
          CallerVersionScope versionScope(session);
          fres = doexec::executeDo(*adoFile, session, dispatch, {});
        }
        std::vector<std::string> defined;
        for (const auto& kv : session.programs) {
          auto old = before.find(kv.first);
          if (old == before.end() || old->second.body != kv.second.body ||
              old->second.rclass != kv.second.rclass)
            defined.push_back(kv.first);
        }
        for (const auto& name : defined) {
          ProgramDef def = std::move(session.programs.at(name));
          session.programs.erase(name);
          if (auto old = before.find(name); old != before.end()) session.programs[name] = old->second;
          def.ado = parsed.cmd;
          if (lower(name) == lower(parsed.cmd)) {
            session.programs[name] = std::move(def);
          } else {
            const std::string privateName = parsed.cmd + "." + name;
            def.name = privateName;
            session.programs[privateName] = std::move(def);
          }
        }
        if (fres.result.stoppedAtReason.has_value()) {
          ojson out = ojson::array({echo});
          for (const auto& b : fres.result.blocks)
            if (b.value("type", std::string()) == "error") out.push_back(b);
          return out;
        }
        progMatch = program::findProgram(session, parsed.cmd);
        if (!progMatch.has_value()) {
          ojson err = ojson(eval::errorBlock("unsupported_command",
                                             "command " + parsed.cmd + " not defined by " +
                                                 parsed.cmd + ".ado",
                                             parsed.cmd));
          err["reason"] = "r(199)";
          return ojson::array({echo, err});
        }
      }
    }
    if (progMatch.has_value()) {
      const ProgramDef& progDef = session.programs.at(*progMatch);
      struct AdoContextScope {
        SessionState& s;
        std::string saved;
        AdoContextScope(SessionState& ss, const std::string& ado) : s(ss), saved(ss.adoContext) {
          s.adoContext = ado;
        }
        ~AdoContextScope() { s.adoContext = saved; }
      } adoScope(session, progDef.ado);
      CallerVersionScope versionScope(session);  // BUG-1459: the program's own `version`
      // Argument string: everything after the program-name token in the
      // macro-expanded line (cmdForDispatch), mirroring
      // raw_cmd_for_dispatch[find(parsed.cmd)+len:].strip().
      // BUG-1049: from the line as typed (lineAsTyped), so `0' keeps its `"..."'.
      const std::string& cmdAsTyped = lineAsTyped.empty() ? cmdForDispatch : lineAsTyped;
      std::string afterCmd;
      size_t pos = cmdAsTyped.find(parsed.cmd);
      if (pos != std::string::npos) {
        afterCmd = trimWs(cmdAsTyped.substr(pos + parsed.cmd.size()));
      }
      ojson progBlocks;
      try {
        progBlocks = program::runProgram(progDef, afterCmd, session, dispatch);
      } catch (const std::exception& exc) {
        return ojson::array({echo, eval::engineError("program " + parsed.cmd + ": " + exc.what())});
      }
      // A noisily inside the program does not undo a quietly around its call
      // (StataBE 19.5: `quietly pn` prints nothing of pn's `noisily display`).
      loops::settleNoisily(progBlocks, session, /*force=*/true);
      ojson out = ojson::array({echo});
      for (const auto& b : progBlocks) out.push_back(b);
      return out;
    }
  }

  // ------------------------------------------------------------------
  // 2. Guard: data must be loaded for most commands (dispatcher.py §2's
  // allow-list). M3a widens the M2 slice's `use`-only list to every
  // data-free command this milestone's handler surface actually routes
  // (xtset/tsset/display/di/scalar/label/timer/cd/pwd/preserve/restore
  // /tempfile/tempname); the FULL oracle allow-list (sysuse/webuse/clear/
  // set/import/version/compress/graph/eststo/…) stays M3b until those
  // commands themselves are ported — listing a command here that has no
  // handler arm below would silently bypass the no-data guard with nothing
  // to route it to, so this set is intentionally scoped to M3a's surface.
  static const std::set<std::string> kNoDataCommands = {
      "use",       "sysuse", "xtset",     "tsset",   "display", "di",     "cd",
      "pwd",       "timer",  "label",     "preserve", "restore", "tempfile",
      "tempname",  "scalar",
      // append.ado appends into an empty dataset too: the result is the using
      // data (w2aw_data_combine_family.log dc_a_empty rc 0, N=2);
      // runAppendFiles handles the empty master itself.
      "append",
      // M3b: foundational commands that must work with zero data loaded
      // (clear/set-obs are the `clear -> set obs N -> gen` data-construction
      // idiom's first two steps; version/compress/set are pure no-ops;
      // egen/bysort/postfile/label define et al. can run before `use` too).
      "clear",     "set",    "version",   "compress", "exit",   "egen",
      "bysort",    "by",     "postfile",  "post",     "postclose",
      // BUG-028 + U7/S9 offline shims: `log using`/`log close|off|on`, plus
      // which/ssc/confirm/mkdir/erase, must all work before any `use`
      // (analysis.do's line 52-108 preamble opens a log, checks/installs
      // packages, and confirms the input file exists, all before `use`).
      // BUG-180 (P2-PREP): "rmdir" joins its mkdir/erase neighbours here —
      // scratch-directory teardown must work before any `use`, exactly like
      // the setup half.
      "log",       "which",  "ssc",       "confirm",  "mkdir",  "erase",  "rm",  "rmdir",
      // M4a: stored-estimates commands operate on session state (the
      // estimates store / lastModel), not the live dataset (dispatcher.py
      // §2's own no-data allow-list: "eststo", "est", "estimates",
      // "marginsplot", "graph", "coefplot"). "graph" itself (export/save/
      // combine/drop sub-commands read session.last_graph, not live data;
      // `graph twoway`/plot-type forms DO need data — the graph handler
      // arm re-checks hasData() for those internally, matching
      // dispatcher.py's own `if ds is None: return no_data_loaded()` INSIDE
      // the "graph" handler body, not at this outer guard).
      "eststo",    "est",    "estimates", "marginsplot", "coefplot", "graph",
      // Mission 5 (cosmetic no_data_loaded divergence, native-paths
      // investigation): real Stata's `describe`/`desc` with no dataset in
      // memory answers an EMPTY description (0 vars, 0 obs), not an error.
      // Deliberately does NOT include "codebook" — real Stata's codebook
      // needs actual variable statistics and errors with no data, so it
      // stays on the outer no-data guard, unaffected. The "describe"/"desc"
      // arm below special-cases !session.hasData() itself rather than
      // calling handlers::runDescribe() (handlers.cpp, out of this
      // workstream's manifest), whose own internal guard still returns
      // noDataLoaded() unconditionally.
      // BUG-105 abbreviation stopgap: `d` is listed here for the same
      // reason `desc` is — an alias that routes to the describe arm but
      // dies on the outer no-data guard would answer no_data_loaded where
      // its own canonical answers an empty description, i.e. it would be
      // an alias in name only.
      "describe",  "desc",   "d",
      // P1 wave: `import delimited|excel` LOADS a dataset, so it must run with
      // nothing in memory — the same reason `use`/`sysuse` are here. (`export`
      // genuinely needs data and stays on the outer guard.)
      "import",
      // BUG-343: `file open|write|read|close` is text I/O against the filesystem
      // and never touches the dataset. Registering the row without listing it
      // here would have made every `file` line answer "No dataset is loaded" —
      // the same wrong-class error BUG-342's second layer produced, and the
      // reason a registry row is never the whole of adding a command.
      "file",
      // AER-SCOPE1 bundle D (ENV): every environment verb runs with nothing in
      // memory -- most of them ARE the first line of the do-file, before any
      // `use`. Omitting a row here is BUG-343 exactly: the registry row and the
      // arm are both correct and the command still answers "No dataset is
      // loaded". testAerCampaignRatchets() probes this axis directly.
      "shell",   "winexec", "type",    "dir",      "ls",      "net",
      "ado",     "adopath", "sysdir",  "findfile", "sleep",   "pause",
      "cls",     "gr_edit", "error",   "bcal",     "twoway__histogram_gen",
      "frames",
      // AER-SCOPE1 bundle B: `return`, `svmat` and `matlist` read the session's
      // r()/matrix stores, never the dataset -- `svmat A` on an empty session is
      // how Stata builds a dataset FROM a matrix. `mkmat` is deliberately absent
      // for the mirror reason: it reads variables, so no data is a real error.
      "return",  "svmat",   "matlist",
      // W2D-T01: `ereturn clear/post/local` act on e(), never the dataset;
      // `ereturn clear` opens the golden do-file's helpers before any data
      // exist (w2d_uncovered.log 211-212). The `list` arm re-checks hasData()
      // itself to keep its earlier answer.
      "ereturn",
      // ml-getmata T4: `getmata` reads the Mata store, and on an empty session
      // it is how the dataset comes into existence (getmata: an empty dataset
      // implies force; oracle arm getmata_nodata).
      "getmata", "ml",
      // `matrix`/`mat` join them: a matrix is session state, not data. Stata
      // accepts `matrix A = (1,2\\3,4)` with nothing in memory, and `svmat A`
      // on an empty session is the documented way to build a dataset FROM a
      // matrix -- refusing the definition made that whole path unreachable.
      "matrix",  "mat",
      // `tset` is deliberately ABSENT: like the `tsset`/`xtset` it re-spells, it
      // validates its variables against the data, so running it with nothing in
      // memory is a genuine no-data error rather than a route that was blocked.
      // AER-SCOPE1 bundle E (TABLES): the table verbs that render STORED
      // ESTIMATES or a session MATRIX and write a file. `eststo`/`esttab` are
      // already above for the same reason. The question this list answers is
      // "does this verb READ THE DATA?" (bundle D's lesson), and for these the
      // answer is no -- an estimate outlives the `clear` that follows it, so
      // `regress ... ; clear ; estout using t.tex` is a real corpus shape.
      "estadd",  "estout",   "outreg2",  "frmttable", "mat2txt", "erepost",
      "regsave", "parmest",
      // texdoc holds an open FILE; `tex` writes a literal line into it. Neither
      // has anything to do with the dataset, and `texdoc init` is routinely the
      // first line of the do-file.
      "texdoc",  "tex",
      // Deliberately ABSENT, all three for the same reason `tset` is: they read
      // the dataset. `estpost summarize` summarises variables, `texsave` renders
      // the dataset as a table, and `tabout` cross-tabulates it -- with nothing
      // in memory each is a genuine no-data error, not a blocked route.
      // AER-SCOPE1 bundle F (RESHAPE): `input` and `drawnorm` CREATE the data
      // they operate on, exactly like `use` -- `clear` then `input x y` is the
      // documented way to type a dataset in, and `drawnorm u v, n(50) clear` is
      // how a simulation starts. Every OTHER verb in bundle F reshapes or
      // samples an existing frame and is deliberately absent from this list.
      "input",   "drawnorm",
      // AER-SCOPE1 bundle G (IO-TEXT): the three readers LOAD data, and the
      // file/environment verbs never touch the dataset at all -- `copy`,
      // `filefilter` and `putexcel set` all routinely appear in a preamble
      // before any `use`. Deliberately ABSENT: `outsheet`, `outfile` and
      // `saveold`, which WRITE the frame out, so running them with nothing in
      // memory is a genuine no-data error rather than a blocked route.
      "insheet", "infile",   "infix",    "putexcel", "filefilter", "copy",
      "unzipfile", "zipfile", "odbc",    "spshape2dta",
      // AER tier-1: file, estimate-store, label-store, matrix-store and display
      // verbs that read no dataset (zipsave alone needs one, and checks itself).
      "zipuse", "require", "_dots", "postutil", "lrtest", "matsort", "matselrc", "numlabel",
      "findname",  // rc 0 and silent with no data (StataBE, lane-meta probe mx5)
      // AER-SCOPE1 bundle I (UTIL): `filelist` builds its dataset FROM the
      // filesystem, so like `use` it must run with nothing in memory. Every
      // other verb in bundle I reads or rewrites an existing frame.
      "filelist",
      // AER-SCOPE1 bundle J (GRAPH-X): `grstyle` touches nothing at all, and
      // `grc1leg2`/`event_plot` read named graphs and stored estimates rather
      // than the frame -- all three routinely appear before any `use`.
      // `geodist`, `geonear` and `cdfplot` read the data and stay on the guard.
      "grstyle", "grc1leg2", "event_plot",
      // AER-SCOPE2 bundle K1: `beep`/`window` touch nothing at all, `grc1leg`
      // reads named graphs like its grc1leg2 sibling above, and `parallel`'s
      // initialize/clean subcommands are process bookkeeping. All four appear in
      // do-file preambles before any `use`.
      "beep", "window", "grc1leg",
      // AER scope 2 K1c: `parallel initialize`/`setclusters`/`clean` are
      // process bookkeeping and appear in a driver's preamble, before any
      // `use`. The `do` forms below them load their own data (or run with
      // none), so the whole name is on the guard.
      "parallel",
      // serset-twoway SS: `serset` reads the session's serset store, and with
      // nothing in memory it is rc 0 with r(id) -1 (w2al_twoway_family.log:177);
      // `serset use` is how a serset BECOMES the data.
      "serset",
      // T7 L1: `lookfor x` with nothing in memory is rc 0 and silent in StataBE
      // (a bare `lookfor` is still r(198)); the handler owns both answers.
      "lookfor",
  };
  bool needsNoData = kNoDataCommands.count(cmd) != 0;
  // BUG-786: with no data in memory Stata's `count if` counts zero observations
  // -- `clear` / `count if e(sample)` is 0 at rc 0 (w2_esample.log:1290 E15
  // |0|0|28|). PIET refused it with r(111) "no dataset". Only a condition that
  // names no variable is served (a constant, or e(sample) already rewritten to
  // `0` above); `count if x` still has no `x` to find, and keeps the refusal
  // (Stata: r(111) variable not found). BUG-1104: a BARE `count` is the same
  // zero (StataBE `clear all` / `count` prints 0, r(N) 0).
  if (!session.hasData() && cmd == "count" && parsed.varlist.empty() && parsed.opts.empty() &&
      !parsed.inRange.has_value()) {
    // A Unicode variable name (`count if قيمة > 2`) names a variable too.
    static const std::regex reIdentChar(R"([A-Za-z_]|[^\x00-\x7F])");
    if (!parsed.ifExpr.has_value() || !std::regex_search(*parsed.ifExpr, reIdentChar)) {
      session.eval.rValues.set("N", 0.0);
      eval::declareRPosted(session.eval, "count", {"N"});  // count's help
      ojson out;
      out["type"] = "display";
      out["text"] = "0";
      blocks.push_back(out);
      return blocks;
    }
  }
  // With no data in memory Stata's `assert` evaluates its claim once, as a
  // constant: `clear` / `assert 1==0` is r(9), `assert 601 == 601` rc 0, and a
  // claim naming a variable is r(111) (StataBE 19.5, checked 2026-10-21). PIET
  // refused every form as "no dataset", which stopped probe_b4 after a failed
  // `import excel ..., clear` had (correctly) emptied the data.
  if (!session.hasData() && cmd == "assert" && parsed.opts.empty() &&
      !parsed.inRange.has_value() && !parsed.ifExpr.has_value() && !parsed.varlist.empty()) {
    const std::string claim = genExprText(parsed);  // BUG-1433: a literal keeps its blanks
    std::string raised;
    bool isStr = false;
    auto v = eval::evalArith(claim, &session.eval, &raised, &isStr);
    if (v && !isStr) {
      if (*v == "0")
        blocks.push_back(ojson(eval::errorBlock("engine_error", "assertion is false", std::nullopt,
                                                std::nullopt, std::nullopt, "r(9)")));
      return blocks;
    }
  }
  // FU-1252: after `clear`, `generate` creates a 0-observation variable in
  // Stata (w2ar `clear` + `generate double a = .` + `save ..., emptyok`, k=2
  // N=0, log:4506-4509; `generate byte z = .`, k=1 N=0, log:4552). The empty
  // dataset is the one `set obs 0` materializes.
  // `save f, emptyok` with nothing in memory writes the same empty dataset as a
  // k=0 N=0 file (dx_save_079_emptyok, log:4520); without emptyok it stays
  // r(111) (dx_save_noemptyok, log:4535).
  bool saveEmptyOk = false;
  if (cmd == "save" || cmd == "sa") {
    for (const auto& kv : parsed.opts)
      if (kv.first == "emptyok") saveEmptyOk = true;
  }
  if (!session.hasData() && !needsNoData &&
      (cmd == "generate" || cmd == "gen" || cmd == "g" || saveEmptyOk)) {
    ojson so = ojson(handlers::runSetObs("0", session));
    if (so.is_object() && so.value("type", std::string()) == "error") {
      blocks.push_back(std::move(so));
      return blocks;
    }
  }
  // BUG-1458: with nothing in memory Stata holds an empty dataset, not none.
  // StataBE 19.5, 2026-11-03, after `clear`: `summarize`, `summarize, detail`,
  // `summarize if 1` and `list` are rc 0 and print nothing; `summarize x` and
  // `list x` are r(111) "no variables defined"; `summarize in 1` and `list in
  // 1` are r(198) "observation numbers out of range"; `save f` without
  // emptyok is r(111) "no variables defined". An `if` is read but meets no
  // observation: `summarize if _n==1` and `list if missing(.)` are rc 0, while
  // `if nosuch==1` is r(111) "nosuch not found", `if (` r(132) and `if "a"`
  // r(109), as any expression fails.
  if (!session.hasData() && !needsNoData) {
    auto refuse = [&](const std::string& msg, const std::string& rc) {
      blocks.push_back(ojson(eval::errorBlock("engine_error", msg, std::nullopt, std::nullopt,
                                              std::nullopt, rc)));
      return blocks;
    };
    if (cmd == "summarize" || cmd == "list") {
      if (!parsed.varlist.empty()) return refuse("no variables defined", "r(111)");
      if (parsed.inRange.has_value()) return refuse("observation numbers out of range", "r(198)");
      if (!parsed.ifExpr.has_value()) return blocks;
      // `_n` and `_N` are 0 here: no observation is there to number.
      static const std::regex reObsSpecial(R"(\b_[nN]\b)");
      const std::string cond = std::regex_replace(*parsed.ifExpr, reObsSpecial, "0");
      std::string raised;
      bool isStr = false;
      if (std::optional<std::string> v = eval::evalArith(cond, &session.eval, &raised, &isStr)) {
        if (isStr) return refuse("type mismatch", "r(109)");
        return blocks;
      }
      eval::MacroFnError fe;
      if (eval::classifyFailedExpr(cond, raised, &fe) && fe.failed())
        return refuse(fe.message, "r(" + fe.rc + ")");
      if (std::optional<std::string> name = firstBareName(cond))
        return refuse(*name + " not found", "r(111)");
      return refuse("invalid syntax", "r(198)");
    }
    if (cmd == "save" && !saveEmptyOk) return refuse("no variables defined", "r(111)");
    // StataBE 19.5 after `clear` (with or without `set obs`): a bare `centile`
    // is rc 0 and prints its empty table header; `centile x` is r(111)
    // (measured on StataBE, D4-D6). runCentile answers both.
    if (cmd == "centile") {
      blocks.push_back(ojson(handlers::runCentile(parsed, session)));
      return blocks;
    }
  }
  if (!session.hasData() && !needsNoData) {
    // BUG-1437: a name the command table has no row for is Stata's r(199)
    // "unrecognized" whatever is in memory (StataBE `clear all` / `nosuchcmd`, or
    // a program `program drop _all` removed), which the last arm of dispatchImpl
    // answers once data exist. Every arm below this guard is a registry row
    // (beep and window, the two that are not, are in kNoDataCommands).
    if (registry::findByName(cmd) == nullptr) {
      blocks.push_back(ojson(eval::unsupportedCommand(
          parsed.cmd, std::nullopt, std::nullopt, "piet_gap",
          eval::UnsupportedCommandKind::kUnknownToken)));
      return blocks;
    }
    // BUG-1248: a postestimation verb reads e() before any data. With nothing in
    // memory and no estimates StataBE answers r(301) "last estimates not found"
    // (test, lincom, nlcom, margins, predict, every estat, lassocoef; esttab's
    // "last estimates (.) not found") and boottest its rc-0 "No estimates
    // detected"; each arm answers exactly that first, so with no e() live they
    // go on to it.
    static const std::set<std::string> kEstimatesFirst = {
        "test",  "lincom",   "nlcom",     "margins", "predict",
        "estat", "boottest", "lassocoef", "esttab"};
    if (kEstimatesFirst.count(cmd) == 0 || session.eval.eLive()) {
      blocks.push_back(ojson(eval::noDataLoaded()));
      return blocks;
    }
  }

  // ------------------------------------------------------------------
  // 2b. BUG-010 residue — generalized trailing-token guard. Most handler
  // arms below read only the specific tokens they expect off
  // parsed.varlist and silently ignore anything left over, e.g.
  // `count gen zzz9 = 1` tokenizes to cmd="count",
  // varlist=["gen","zzz9","=","1"]; runCount only ever consults
  // parsed.ifExpr, so the entire "gen zzz9 = 1" tail vanishes and the line
  // silently degrades to a bare `count` (960, not the intended `gen`).
  // Real Stata rejects this at parse time. xtset/tsset already got
  // bespoke arm-local guards for this (see above); rather than writing N
  // more bespoke blocks (or a full parser rewrite that would validate
  // every arm's grammar), this is one small allow-list of the commands
  // whose OWN Stata grammar takes zero or one fixed token — so ANY
  // leftover varlist token is unambiguously a mistake, not a legitimate
  // variable name. Kept deliberately narrow (the "worst offenders" swept
  // from the M3a/M3b arms above): commands with genuinely variable-arity
  // varlists (describe, summarize, regress, list, sort, ...) are correctly
  // left alone since a trailing token there IS the next variable name.
  {
    struct FixedArity {
      size_t maxTokens;  // max allowed parsed.varlist.size() for this cmd
      const char* usage;
    };
    static const std::map<std::string, FixedArity> kFixedArityCommands = {
        {"count", {0, "count [if] [in] — takes no variable list"}},
        {"pwd", {0, "pwd — takes no arguments"}},
        {"clear", {1, "clear [all]"}},
        {"preserve", {0, "preserve — takes no arguments"}},
        {"restore", {0, "restore — takes no arguments"}},
        {"sysuse", {1, "sysuse DATASET | sysuse dir"}},
    };
    auto arityIt = kFixedArityCommands.find(cmd);
    if (arityIt != kFixedArityCommands.end() && parsed.varlist.size() > arityIt->second.maxTokens) {
      std::vector<std::string> extra(
          parsed.varlist.begin() + static_cast<long>(arityIt->second.maxTokens), parsed.varlist.end());
      blocks.push_back(ojson(eval::engineError(
          cmdTyped + ": unexpected extra tokens after `" + arityIt->second.usage +
              "`: " + joinVarlist(extra) + ". Put each command on its own line.",
          "syntax_error")));
      return blocks;
    }
  }

  // ------------------------------------------------------------------
  // 3. Classify + dispatch — the M2 vertical slice's routing table.
  // TODO(M3) EXTENSION POINT: add new `if (cmd == "...")` arms here (mirroring
  // classify()'s Dispatch objects) without touching steps 0a/0b/1/2 above —
  // that pipeline is the frozen step-0a/0b contract this milestone's
  // dispatcher skeleton was built to hold stable for M3.
  if (cmd == "use") {
    // `nolabel` retains labels/attachments but omits definition tables. Parse
    // all options before extracting the filename (lb_rt_nolabel uses both).
    const std::unordered_set<std::string>& kUseKnownOpts = kVocabUse.known;
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kUseKnownOpts)) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    std::string raw = parsed.raw;
    // BUG-251/WS-Q: was `lowerRaw.find("use")` + `substr(pos + 3)` — a LITERAL
    // spelling, unguarded. `use` really is abbreviable in Stata (measured
    // 2026-08-07 on StataBE: `u` and `us` both answer rc 0), so the moment the
    // registry row gained the abbreviations this threw on the line `u`.
    std::string afterUse = rawArgsAfterCmd(raw);
    bool useClears = false, useNoLabel = false;
    for (const auto& [key, value] : parsed.opts) {
      if (value.isBool && !value.boolValue) continue;
      if (key == "clear" || key == "replace") useClears = true;
      else if (key == "nolabel") useNoLabel = true;
    }
    // A comma inside a quoted filename is part of the filename.
    char quote = 0;
    for (size_t i = 0; i < afterUse.size(); ++i) {
      if (quote) { if (afterUse[i] == quote) quote = 0; }
      else if (afterUse[i] == '"') quote = afterUse[i];
      else if (afterUse[i] == ',') { afterUse.resize(i); break; }
    }
    // [D] use: without `clear`, data changed since they were last saved are
    // not discarded -- r(4) "no; dataset in memory has changed since last
    // saved", use's own wording (w2ar_data_io_family.log:4368
    // dx_u_noclear_changed `4|k=7|N=4|`; after `clear` the same line loads,
    // dx_u_077_noclear). The import verbs' r(4) says "data in memory would be lost".
    if (!useClears && session.hasData() && session.datasetChanged) {
      blocks.push_back(ojson(eval::errorBlock("engine_error",
                                              "no; dataset in memory has changed since last saved",
                                              std::nullopt, std::nullopt, std::nullopt,
                                              std::string("r(4)"))));
      return blocks;
    }
    size_t b = afterUse.find_last_not_of(" \t");
    afterUse = (b == std::string::npos) ? "" : afterUse.substr(0, b + 1);
    std::string filepath = !afterUse.empty() ? afterUse
                           : (!parsed.varlist.empty() ? parsed.varlist[0] : "");
    if (filepath.empty()) {
      blocks.push_back(ojson(eval::engineError("syntax_error: `use` requires a filename", "syntax_error")));
      return blocks;
    }
    ojson useResult = handlers::runUse(filepath, session);
    if (useNoLabel && useResult.value("type", std::string()) != "error") {
      session.labelDefs.clear();
      session.valueLabels.clear();
      session.labelSeq.clear();
      // runUse rendered its card before nolabel withdrew the definitions.
      if (useResult.contains("varlist_after"))
        useResult["varlist_after"] = handlers::mutationResult(session, "use")["varlist_after"];
    }
    // BUG A / hbin-sniff honesty note: runUse() attaches a "note" field to
    // its own block when it had to load a mismatched-extension file by
    // sniffing hbin content — lift it into its OWN prepended, UNPREFIXED
    // echo block (same pattern xtset/tsset/cd/pwd/clear already use for a
    // second informational echo — see fuzz_sequences.py's own doc-comment:
    // this is NOT a new command boundary, only a ".  "-prefixed echo is).
    if (useResult.contains("note")) {
      ojson noteBlock;
      noteBlock["type"] = "echo";
      noteBlock["text"] = useResult["note"].get<std::string>();
      blocks.push_back(noteBlock);
      useResult.erase("note");
    }
    blocks.push_back(useResult);
    return blocks;
  }

  // ------------------------------------------------------------------
  // Mission 3 (hardening-queue item, not in BUGLOG): `sysuse DATASET[,
  // clear]` / `sysuse dir`. Real Stata's `sysuse` loads one of a whole
  // library of named sample datasets by bare name (no path); PIET ships
  // exactly one today — piet/packaging/sample/fatalities.csv, the SAME
  // fixture main.cpp's runUiSmoke() already loads (bundle-first via
  // piet::bundleResourcesDir()+"/sample/fatalities.csv", falling back to
  // the dev-tree piet/packaging/sample/fatalities.csv — see main.cpp
  // ~957-968 and its resolveFrontendIndexPath() sibling ~252-269). This
  // arm mirrors that exact bundle-first/dev-tree-fallback order via the
  // fileExistsLocal()/devTreeSampleFatalitiesCsv() helpers above rather
  // than inventing a second convention. Trailing-token arity (1 token —
  // DATASET or `dir`) is enforced by mission 1's shared kFixedArityCommands
  // table above, not re-checked here.
  if (cmd == "sysuse") {
    if (!parsed.varlist.empty() && lower(parsed.varlist[0]) == "dir") {
      ojson dirOut;
      dirOut["type"] = "display";
      dirOut["text"] = "fatalities";
      blocks.push_back(dirOut);
      return blocks;
    }
    std::string dsArg = parsed.varlist.empty() ? std::string() : parsed.varlist[0];
    if (dsArg.empty()) {
      blocks.push_back(ojson(eval::engineError(
          "syntax_error: `sysuse` requires a dataset name (or `sysuse dir` to list what's available)",
          "syntax_error")));
      return blocks;
    }
    std::string dsLower = lower(dsArg);
    static const std::string kDtaSuffix = ".dta";
    if (dsLower.size() > kDtaSuffix.size() &&
        dsLower.compare(dsLower.size() - kDtaSuffix.size(), kDtaSuffix.size(), kDtaSuffix) == 0) {
      dsLower = dsLower.substr(0, dsLower.size() - kDtaSuffix.size());
    }
    if (dsLower != "fatalities") {
      blocks.push_back(ojson(eval::engineError(
          "sysuse: unknown dataset `" + dsArg + "` — the only dataset bundled with this version of "
              "PIET is `fatalities` (run `sysuse dir` to list what's available).",
          "engine_error")));
      return blocks;
    }
    std::string sampleCsv;
    if (std::optional<std::string> resources = bundleResourcesDir()) {
      std::string bundled = *resources + "/sample/fatalities.csv";
      if (fileExistsLocal(bundled)) sampleCsv = bundled;
    }
    if (sampleCsv.empty()) sampleCsv = devTreeSampleFatalitiesCsv();
    blocks.push_back(handlers::runUse(sampleCsv, session));
    return blocks;
  }

  if (cmd == "codebook") {
    // BUG-336 (batch 2): known set measured from the handler body -- codebook reads no options (delegates to runSummarize per-var).
    // BUG-371: `compact` moves from the honest-reject set into the implemented one.
    // Nothing new is computed for it — this arm already gathers obs/unique/mean/
    // min/max/label for every variable; compact only decides which get printed.
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabCodebook.known, kVocabCodebook.unimplemented)) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    // BS-025 (BETA Phase C batch C1): codebook used to be a bare alias of
    // `describe` (same shared arm as below) — so `codebook income if
    // year==1982` silently ignored the `if` entirely (returned the SAME
    // unfiltered stats as plain `codebook income`), and the payload was
    // only describe's type/format/label/n_missing/n_unique, never
    // codebook's own range/mean/sd/percentiles. This arm gives codebook
    // its own implementation: honour `if` via the same keepif-into-
    // temp-hbin idiom already used by the tabulate/table `if` arms above
    // (dispatcherNextPath + `cobra eval --keepif` + a fresh Datastore over
    // the filtered copy), compute per-variable type/format/label/
    // n_missing/n_unique directly off that (filtered) Datastore — mirroring
    // handlers::runDescribe's own numeric-unique-count scan — and pull
    // mean/sd/min/max/p10/p25/p50/p75/p90 from handlers::runSummarize(...,
    // detail=true, ...): cobra's `eval --summarize --detail` raw output
    // already carries every Stata percentile (verified live: p1/p5/p10/
    // p25/p50/p75/p90/p95/p99), and runSummarize forwards that raw per-var
    // object VERBATIM into its "stats" — summarize's own renderer just
    // never displays them. No handlers.cpp changes needed: both
    // runSummarize and runDescribe's per-variable logic are reused/mirrored
    // as-is.
    if (!session.hasData()) {
      blocks.push_back(ojson(eval::noDataLoaded()));
      return blocks;
    }
    Datastore* ds = session.datastore();
    std::unique_ptr<Datastore> codebookIfDs;
    if (parsed.ifExpr.has_value() && !parsed.ifExpr->empty()) {
      std::string codebookIfPath = dispatcherNextPath(session.workdir(), "codebookif");
      // BUG-369 follow-up: `codebook x if L.y>0` — same injection every other
      // user-if surface carries; no-op unless the predicate names a TS operator.
      std::vector<std::string> ifArgs = translate::injectTsContextForExpr(
          {"eval", "--data", ds->path(), "--keepif", *parsed.ifExpr, "--out", codebookIfPath},
          *parsed.ifExpr, session);
      try {
        runCobraJson(ifArgs);
      } catch (const CobraError& ce) {
        blocks.push_back(ojson(eval::engineError(cobraErrorMessage(ce), ce.code)));
        return blocks;
      }
      codebookIfDs = std::make_unique<Datastore>(codebookIfPath);
      ds = codebookIfDs.get();
    }
    std::vector<std::string> names = parsed.varlist.empty() ? ds->names() : parsed.varlist;
    for (const auto& name : names) {
      if (!ds->has(name)) {
        blocks.push_back(ojson(eval::engineError("variable " + name + " not found")));
        return blocks;
      }
    }
    // Batch every numeric var into ONE cobra summarize,detail call (not one
    // per variable) — its raw per-var object already carries mean/sd/min/
    // max/p10/p25/p50/p75/p90, Stata-exact.
    std::vector<std::string> f64Names;
    for (const auto& name : names) {
      if (ds->dtype(name) == "f64") f64Names.push_back(name);
    }
    ojson f64Stats = ojson::object();
    if (!f64Names.empty()) {
      ojson sumBlock = ojson(handlers::runSummarize(f64Names, /*detail=*/true, session,
                                                     parsed.ifExpr, std::nullopt));
      if (sumBlock.value("type", "") == "summarize" && sumBlock.contains("stats")) {
        f64Stats = sumBlock["stats"];
      }
    }

    ojson variables = ojson::array();
    for (const auto& name : names) {
      VarInfo vi = ds->varinfo(name);
      auto metaIt = session.varMeta.find(name);
      std::string stataType;
      std::string fmt;
      if (metaIt != session.varMeta.end() && metaIt->second.stataType.has_value()) {
        stataType = *metaIt->second.stataType;
      } else {
        stataType = (vi.dtype == "factor") ? "str#" : "float";
      }
      // format-display-default-lifecycle: the one resolver describe and `: format`
      // share, not a codebook-local fallback table.
      fmt = session.displayFormat(vi);
      std::string label;
      if (metaIt != session.varMeta.end() && metaIt->second.label.has_value()) {
        label = *metaIt->second.label;
      }

      ojson entry;
      entry["name"] = name;
      entry["dtype"] = stataType;
      entry["format"] = fmt;
      entry["label"] = label;
      entry["n_missing"] = ds->nMissing(name);
      entry["n_obs"] = ds->nRows();

      if (vi.dtype == "factor") {
        entry["n_unique"] = static_cast<int64_t>(vi.levels.size());
      } else {
        const cecon::Column& col = ds->col(name);
        const double* d = col.f64();
        int64_t count = col.nbytes / static_cast<int64_t>(sizeof(double));
        std::vector<double> valid;
        valid.reserve(count);
        for (int64_t i = 0; i < count; ++i) {
          if (!std::isnan(d[i])) valid.push_back(d[i]);
        }
        std::sort(valid.begin(), valid.end());
        valid.erase(std::unique(valid.begin(), valid.end()), valid.end());
        entry["n_unique"] = static_cast<int64_t>(std::min<size_t>(valid.size(), 65536));

        if (f64Stats.contains(name)) {
          const ojson& st = f64Stats[name];
          for (const char* k : {"mean", "sd", "min", "max", "p10", "p25", "p50", "p75", "p90"}) {
            if (st.contains(k)) entry[k] = st[k];
          }
        }
      }
      variables.push_back(entry);
    }
    ojson out;
    // No dedicated frontend renderer exists yet for codebook's full payload
    // (range/unique/mean/sd/percentiles combined) — describe's own switch
    // case already renders type/format/label/n_missing/n_unique from this
    // exact "describe" block shape (console.js's formatDescribe), so reuse
    // it rather than emitting a block type the frontend cannot render at
    // all; the extra numeric fields ride along on each variable entry for
    // when the frontend adds codebook-specific rendering (out of scope for
    // this native-only batch — piet/frontend/** is forbidden here).
    out["type"] = "describe";
    out["variables"] = variables;
    // BUG-371: the compact flag rides on the same block. The frontend switches on
    // it (console.js formatDescribe) to print Stata's one-line-per-variable table
    // instead of the storage/format/label layout; a consumer that ignores the flag
    // still sees every field it saw before, so this is purely additive.
    for (const auto& [k, v] : parsed.opts)
      if (k == "compact" && (!v.isBool || v.boolValue)) out["compact"] = true;
    // BUG-1490: codebook replaces r() with nothing (StataBE 19.5, rlife/side2.do:
    // `: r(scalars)`, `: r(macros)` and `: r(matrices)` are empty after `codebook
    // x1`, `codebook x1 x2` and `codebook`), so the summarize run above leaves none
    // of its r() behind and every r() name reads as missing.
    eval::replaceRClass(session.eval);
    eval::declareRPosted(session.eval, "codebook", {});
    blocks.push_back(out);
    return blocks;
  }

  // BUG-105 abbreviation stopgap: `d` is real Stata's minimum-length
  // abbreviation of `describe` (the general prefix resolver is a separate
  // arc — BACKLOG BUG105-112-HARDENING-REMAINDER). Every spelling added
  // here is ALSO added to this command's registry.cpp `abbrev` list, which
  // is what makes the registry-backed qualifier gates (tokenizer's `in`/
  // weight allow-lists) and testRegistryRoutes see the same alias set.
  if (cmd == "describe" || cmd == "desc" || cmd == "d") {
    // BUG-336 (batch 2): known set measured from the handler body -- runDescribe
    // itself takes no options parameter. B5-D5: `fullnames` is now accepted
    // (kVocabDescribe.known) as a validated no-op -- see that registration's
    // comment for the receipt; short/simple/numbers/detail remain refused by
    // name because they change what prints.
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabDescribe.known, kVocabDescribe.unimplemented)) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    if (!session.hasData()) {
      // Mission 5: empty description (0 vars), matching real Stata — see
      // the kNoDataCommands comment above for why this is handled here
      // rather than in handlers::runDescribe() itself.
      ojson emptyDescribe;
      emptyDescribe["type"] = "describe";
      emptyDescribe["variables"] = ojson::array();
      // BUG-1447: an empty dataset is described too (StataBE 19.5 after `clear`:
      // r(N) 0, r(k) 0, r(width) 0, r(changed) 0).
      session.eval.rValues.set("N", 0.0);
      session.eval.rValues.set("k", 0.0);
      session.eval.rValues.set("width", 0.0);
      session.eval.rValues.set("changed", session.datasetChanged ? 1.0 : 0.0);
      eval::declareRPosted(session.eval, "describe", {"N", "k", "width", "changed"});
      blocks.push_back(emptyDescribe);
      return blocks;
    }
    std::optional<std::vector<std::string>> vl;
    if (!parsed.varlist.empty()) vl = parsed.varlist;
    ojson desc = ojson(handlers::runDescribe(vl, session));
    bool dShort = false, dSimple = false, dVarlist = false, dReplace = false;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "short") dShort = true;
      if (k == "simple") dSimple = true;
      if (k == "varlist") dVarlist = true;
      if (k == "replace") dReplace = true;
    }
    if ((dShort || dSimple || dVarlist || dReplace) && desc.is_object() &&
        desc.value("type", std::string()) != "error" && desc.contains("variables")) {
      std::vector<std::string> dnames;
      for (const auto& v : desc.at("variables")) dnames.push_back(v.value("name", std::string()));
      int64_t nObs = 0;
      if (std::optional<int64_t> rows = session.residentRowsIfCurrent()) nObs = *rows;
      else if (Datastore* dds = session.datastore()) nObs = dds->nRows();
      if (dReplace) {
        // The description becomes the dataset, one observation per variable.
        delimited::Table t;
        t.names = {"position", "name", "type", "isnumeric", "format", "vallab", "varlab"};
        t.labels.assign(t.names.size(), std::string());
        size_t pos = 0;
        for (const auto& v : desc.at("variables")) {
          ++pos;
          const std::string nm = v.value("name", std::string());
          const std::string ty = v.value("dtype", std::string());
          auto lb = session.varLblname.find(nm);
          t.rows.push_back({std::to_string(pos), nm, ty,
                            ty.rfind("str", 0) == 0 ? "0" : "1", v.value("format", std::string()),
                            lb == session.varLblname.end() ? std::string() : lb->second,
                            v.value("label", std::string())});
        }
        t.types.resize(t.names.size());
        for (size_t c = 0; c < t.names.size(); ++c) {
          std::vector<const std::string*> cells;
          for (const auto& r : t.rows) cells.push_back(&r[c]);
          t.types[c] = delimited::typeColumn(cells, c != 0 && c != 3, false, false);
        }
        if (auto err = delimited::stageAndAdopt(session, t, "describe", std::string())) {
          blocks.push_back(*err);
          return blocks;
        }
        blocks.push_back(ojson(handlers::mutationResult(session, "describe")));
        return blocks;
      }
      session.eval.rValues.set("N", static_cast<double>(nObs));
      // BUG-1447: r(k) and r(width) are the dataset's (describeDatasetShape).
      const DescribeShape shape = describeDatasetShape(session);
      session.eval.rValues.set("k", static_cast<double>(shape.k));
      if (shape.width.has_value()) session.eval.rValues.set("width", *shape.width);
      session.eval.rValues.set("changed", session.datasetChanged ? 1.0 : 0.0);
      if (dVarlist) {
        std::string joined;
        for (const auto& nm : dnames) joined += (joined.empty() ? "" : " ") + nm;
        session.eval.rStrings.set("varlist", joined);
        std::string sortlist;
        if (Datastore* dds = session.datastore()) sortlist = dds->sortedBy();
        session.eval.rStrings.set("sortlist", sortlist);
      }
      if (dShort || dSimple) {
        ojson out;
        out["type"] = "display";
        if (dSimple) {
          std::string joined;
          for (const auto& nm : dnames) joined += (joined.empty() ? "" : "  ") + nm;
          out["text"] = joined;
        } else {
          out["text"] = "Contains data\n Observations: " + std::to_string(nObs) +
                        "\n    Variables: " + std::to_string(dnames.size());
        }
        eval::declareRPosted(session.eval, "describe", {"N", "k", "width", "changed"});
        blocks.push_back(out);
        return blocks;
      }
    }
    // BUG-159 (P4 wave): `describe` is r-class in Stata — it posts r(N), r(k)
    // and r(width). PIET posted nothing, so a following `r(k)` errored with
    // "run an r-class command first" after the user had just run one. r(N) is
    // the DATASET's row count (not the described-variable count), and so is
    // r(k) its variable count (BUG-1447), not the number described.
    //
    // The row count is read off the RESIDENT CARD, never via session.datastore().
    // describe's whole D1 card-plane design is that a factor-only describe
    // constructs ZERO Datastores, and a construct here would have silently
    // undone that optimization for every describe — the C4-L2B MECHANISM gate
    // caught exactly that when this first went in. When the card cannot answer
    // (no resident card / not current), r(N) is left alone rather than forcing
    // the construct: an honest absence beats a hidden cost. r(k) and r(width)
    // come off that card too, or else off the Datastore runDescribe above
    // already opened (describeDatasetShape), so they cost no construct.
    {
      if (std::optional<int64_t> rows = session.residentRowsIfCurrent()) {
        session.eval.rValues.set("N", static_cast<double>(*rows));
      }
      // BUG-1447: r(k) and r(width) are the dataset's, r(changed) is c(changed)
      // (StataBE 19.5: r(changed) 1 after edits, 0 right after `save` or `use`).
      const DescribeShape shape = describeDatasetShape(session);
      session.eval.rValues.set("k", static_cast<double>(shape.k));
      if (shape.width.has_value()) session.eval.rValues.set("width", *shape.width);
      session.eval.rValues.set("changed", session.datasetChanged ? 1.0 : 0.0);
      // LBL-DATALABEL: describe posts r(datalabel) when the dataset has a label
      // (w2ao_label_family.log lb_data; lb_var_label reads "" without one). The
      // posted set is declared, so an absent r(datalabel) expands to "" instead
      // of the unknown-set refusal; an r(width) of unknown string widths refuses
      // by name.
      std::vector<std::string> posted{"N", "k", "width", "changed"};
      if (!session.dataLabel.empty()) {
        session.eval.rStrings.set("datalabel", session.dataLabel);
        posted.push_back("datalabel");
      } else {
        session.eval.rStrings.erase("datalabel");
      }
      eval::declareRPosted(session.eval, "describe", std::move(posted));
    }
    blocks.push_back(desc);
    return blocks;
  }

  // BUG-105 abbreviation stopgap: `su` (see the `describe`/`d` note above).
  if (cmd == "summarize" || cmd == "sum" || cmd == "su") {
    bool detail = false;
    for (const auto& [k, v] : parsed.opts) {
      if ((k == "detail" || k == "d") && v.isBool && v.boolValue) detail = true;
    }
    // P4 / BUG-060: unknown-option rejection net. `detail`/`d` and `meanonly`
    // are the summarize options this version of PIET accepts. `meanonly`
    // (BUG-090) tells Stata to skip the variance/percentile pass entirely —
    // r(N)/r(mean)/r(min)/r(max)/r(sum)/r(sum_w) are published, but r(sd)
    // (and the derived r(Var)/r(median)/percentiles/skewness/kurtosis) are
    // NOT: BUG-476, measured StataBE 19.5
    // (tests/eval/golden/aer_b4/pins_oracle_r.log, batch R,
    // `B4VAL|mean|+1.81541bacf914cX+00c|sd_is_missing|+1.0000000000000X+3ff`
    // — mean published, r(sd) is Stata's `.`). PIET's runSummarize still
    // computes the full stats unconditionally (a superset of what meanonly
    // leaves in memory, and cheaper to keep than to special-case) — the
    // DISPLAY and the rendered block are unaffected by meanonly either way
    // (Stata also suppresses meanonly's display; that half is NOT changed
    // here, see the r()-only prune below) — only r() is pruned to match
    // what Stata actually leaves set. Anything else isn't a real summarize
    // option at all -> r(198)-parity reject naming the token.
    bool meanonly = false;
    // BUG-333: `separator(#)` is a display option — Stata draws a horizontal rule
    // every # variables (0 = no rules, default 5). Measured on StataBE 19.5:
    // separator(0)/(2)/(-1) are all rc 0, separator(x) is r(198), so only a
    // non-numeric argument is refused. This loop is kept hand-rolled (rather than
    // switched to rejectUnknownOptions) because it predates the registry and is
    // the summarize arm's only gate; widening it in place is the smaller change.
    long summarizeSeparator = -1;  // -1 = not given; the renderer defaults to 5
    for (const auto& [k, v] : parsed.opts) {
      if (k == "meanonly" && (!v.isBool || v.boolValue)) {
        meanonly = true;
        continue;
      }
      if (k == "detail" || k == "d") continue;
      if (k == "separator") {
        if (v.isBool) {
          blocks.push_back(ojson(eval::engineError(
              "option `separator` requires a number, e.g. separator(5)", "unsupported_option")));
          return blocks;
        }
        try {
          size_t used = 0;
          summarizeSeparator = std::stol(v.strValue, &used);
          if (used != v.strValue.size()) throw std::invalid_argument("trailing");
        } catch (const std::exception&) {
          blocks.push_back(ojson(eval::engineError(
              "option `separator()` incorrectly specified — invalid number", "unsupported_option")));
          return blocks;
        }
        if (summarizeSeparator < 0) summarizeSeparator = 0;  // Stata takes -1 as "no rules"
        continue;
      }
      blocks.push_back(
          ojson(eval::engineError("option `" + k + "` not allowed", "unsupported_option")));
      return blocks;
    }
    std::optional<std::vector<std::string>> vl;
    if (!parsed.varlist.empty()) vl = parsed.varlist;
    // `summarize ... if COND [in RANGE]` is now wired: the row filter
    // reaches the engine via cobra keepif inside runSummarize (Doctrine ①:
    // qualifier honoured, never dropped). BS-090 (BETA Phase G): `in` is now
    // legal here too (registry.cpp's allowInRange), composed with `if` via
    // AND inside runSummarize/applyIfAndInFilter.
    // Step-1 D3 / P3 weights wave: `parsed.weightVar` only survives the
    // tokenizer's guard for this branch when weightType is fw/fweight/aw/
    // aweight (registry.cpp's summarize row is AweightFweight) — any other
    // weight type on summarize was already rejected upstream. Which cobra
    // flag gets used (--fweight vs --aweight) depends on WHICH of those the
    // user actually typed, so branch on parsed.weightType here rather than
    // baking one flag into runSummarize itself.
    bool summarizeIsAweight = parsed.weightType.has_value() &&
        (lower(*parsed.weightType) == "aw" || lower(*parsed.weightType) == "aweight");
    // QF-SUM-IW: summarize takes [iw=] (w2aj_qualifiers.log:826 qf_sw_iw_*), but
    // not with detail: r(101) "iweights not allowed" (log:911-912 qf_sw_iw_detail).
    const bool summarizeIsIweight = parsed.weightType.has_value() &&
        (lower(*parsed.weightType) == "iw" || lower(*parsed.weightType) == "iweight");
    if (summarizeIsIweight && detail) {
      blocks.push_back(ojson(eval::errorBlock("engine_error", "iweights not allowed", std::nullopt,
                                              std::nullopt, std::nullopt, "r(101)")));
      return blocks;
    }
    ojson summBlock = summarizeIsAweight || summarizeIsIweight
        ? ojson(handlers::runSummarizeAweight(vl, detail, session, parsed.ifExpr, parsed.weightVar,
                                              parsed.inRange,
                                              summarizeIsIweight ? "--iweight" : "--aweight"))
        : ojson(handlers::runSummarize(vl, detail, session, parsed.ifExpr, parsed.weightVar,
                                       parsed.inRange));
    // dispatcher.py's post-handler hook: `capture_r_from_block(result_block,
    // session)` — every native-handler result feeds this (summarize is the
    // r-class case the M3a corpus exercises; lincom/nlcom land when ported).
    // Missing entirely through M2 — this is the fix (not new M3a scope).
    // BUG-333: carry the separator to the renderer. Emitted only when the user
    // asked, so every existing summarize fixture stays byte-identical.
    if (summarizeSeparator >= 0 && summBlock.value("type", "") != "error")
      summBlock["separator"] = summarizeSeparator;
    captureSummarizeR(summBlock, meanonly, detail, session);
    blocks.push_back(summBlock);
    return blocks;
  }

  if (cmd == "save") {
    std::string raw = parsed.raw;
    // BUG-105/WS-K: search for the TYPED spelling, not the literal "save".
    // `sav "f.dta"` made find("save") return npos and substr(npos + 4) threw
    // std::out_of_range ("basic_string") — a hard crash of the whole run, not a
    // reject. Found by the spot-check, not by reading: two arms re-derive their
    // arguments from the RAW line by locating the command word in it, so
    // normalising the routed token is necessary but NOT sufficient for them.
    // BUG-251/WS-Q: and searching for `cmdTyped` is still a search — correct
    // only by an invariant nothing here checks, and still unguarded against
    // npos. The tail never depended on the spelling at all.
    std::string after = rawArgsAfterCmd(raw);
    bool inQ = false;
    long commaIdx = -1;
    for (size_t i = 0; i < after.size(); ++i) {
      if (after[i] == '"') {
        inQ = !inQ;
      } else if (after[i] == ',' && !inQ) {
        commaIdx = static_cast<long>(i);
        break;
      }
    }
    bool replace = false;
    bool emptyOk = false;
    if (commaIdx >= 0) {
      std::string optsTail = trimWs(lower(after.substr(static_cast<size_t>(commaIdx) + 1)));
      after = trimWs(after.substr(0, static_cast<size_t>(commaIdx)));
      // Every `save` suboption besides `replace`/`emptyok` (nolabel/orphans/
      // version(#)) is unimplemented — reject the first one by name instead
      // of silently discarding the whole tail (BUG-141 class: a dropped
      // option looks identical to an honoured one from the caller's side).
      // B5-D9: `emptyok` is accepted and otherwise ignored -- see kVocabSave's
      // registration comment for why it is a validated no-op on this verb.
      std::istringstream optsStream(optsTail);
      std::string opt;
      while (optsStream >> opt) {
        if (opt == "replace") {
          replace = true;
          continue;
        }
        if (opt == "emptyok") {
          emptyOk = true;
          continue;
        }
        blocks.push_back(ojson(eval::unsupportedOption(
            opt, "`save` supports only `replace`/`emptyok` in this build; nolabel/orphans/"
                 "version() are not implemented.")));
        return blocks;
      }
    }
    std::string filepath = !after.empty() ? after
                           : (!parsed.varlist.empty() ? parsed.varlist[0] : "");
    if (filepath.empty()) {
      blocks.push_back(ojson(eval::engineError("syntax_error: `save` requires a filename")));
      return blocks;
    }
    // BUG-1458: a dataset with observations but no variables (`clear` / `set
    // obs 3`) is StataBE's r(111) "no variables defined" unless emptyok.
    if (!emptyOk) {
      // The card answers "no variables" without opening the frame (C4-L2A: the
      // save's entry must construct no Datastore and snapshot no dirty head).
      const std::vector<VarInfo>* card = session.residentReadCard();
      Datastore* ds = card != nullptr ? nullptr : session.datastore();
      if (card != nullptr ? card->empty() : (ds != nullptr && ds->names().empty())) {
        blocks.push_back(ojson(eval::errorBlock("engine_error", "no variables defined",
                                                std::nullopt, std::nullopt, std::nullopt,
                                                std::string("r(111)"))));
        return blocks;
      }
    }
    blocks.push_back(ojson(handlers::runSaveData(filepath, replace, session)));
    return blocks;
  }

  if (cmd == "rename" || cmd == "ren") {
    // BUG-336 (batch 2): known set measured from the handler body -- runRename takes no options; Stata rename has none on the 1:1 form either.
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabRename.known, kVocabRename.unimplemented)) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    // ------------------------------------------------------------------
    // B4-W3b: the GROUP form `rename _all, lower` / `rename *, upper`.
    //
    // Routed BEFORE the wildcard reject below, because `*` is exactly the
    // spelling the corpus uses and the reject is about the PATTERN form
    // (`rename v* w*`), which this is not: there is no target pattern here at
    // all, only a case rule applied to every name.
    bool renLower = false, renUpper = false, renProper = false;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "lower" && (!v.isBool || v.boolValue)) renLower = true;
      if (k == "upper" && (!v.isBool || v.boolValue)) renUpper = true;
      if (k == "proper" && (!v.isBool || v.boolValue)) renProper = true;
    }
    if (renLower || renUpper || renProper) {
      if (static_cast<int>(renLower) + static_cast<int>(renUpper) + static_cast<int>(renProper) > 1) {
        blocks.push_back(ojson(eval::errorBlock(
            "bad_input", "rename: lower, upper and proper are alternatives — specify one.")));
        return blocks;
      }
      if (!session.hasData()) {
        blocks.push_back(ojson(eval::noDataLoaded()));
        return blocks;
      }
      std::vector<std::string> names;
      if (const auto* card = session.shadowVarlistIfCurrent()) {
        for (const auto& info : *card) names.push_back(info.name);
      } else if (Datastore* rds = session.datastore()) {
        names = rds->names();
      }
      // rename.ado's case form: `rename oldnames, lower|upper|proper`, the
      // old names a varlist -- names, abbreviations, wildcards, optionally in
      // parentheses (aer_w2 w2_data_io.log:832-848 ren1-ren7: `rename ab,
      // upper` renames abc; `rename *Y, proper` renames XY).
      std::vector<std::string> selected;
      {
        std::string spec = rawArgsAfterCmd(parsed.raw);
        bool inQ = false;
        for (size_t i = 0; i < spec.size(); ++i) {
          if (spec[i] == '"') inQ = !inQ;
          if (spec[i] == ',' && !inQ) { spec = spec.substr(0, i); break; }
        }
        for (char& c : spec)
          if (c == '(' || c == ')') c = ' ';
        std::istringstream iss(spec);
        std::string tok;
        auto glob = [](const std::string& pat, const std::string& str) {
          // * any run, ? one character, ~ one character too (Stata varlist).
          std::function<bool(size_t, size_t)> m = [&](size_t i, size_t j) -> bool {
            if (i == pat.size()) return j == str.size();
            if (pat[i] == '*') {
              for (size_t k = j; k <= str.size(); ++k)
                if (m(i + 1, k)) return true;
              return false;
            }
            if (j < str.size() && (pat[i] == '?' || pat[i] == '~' || pat[i] == str[j]))
              return m(i + 1, j + 1);
            return false;
          };
          return m(0, 0);
        };
        while (iss >> tok) {
          std::vector<std::string> hit;
          if (tok == "_all") {
            hit = names;
          } else if (tok.find_first_of("*?~") != std::string::npos) {
            for (const auto& nm : names)
              if (glob(tok, nm)) hit.push_back(nm);
          } else if (std::find(names.begin(), names.end(), tok) != names.end()) {
            hit.push_back(tok);
          } else {
            for (const auto& nm : names)
              if (nm.size() > tok.size() && nm.compare(0, tok.size(), tok) == 0) hit.push_back(nm);
            if (hit.size() > 1) {
              blocks.push_back(ojson(eval::errorBlock("engine_error", tok + " ambiguous abbreviation",
                                                      std::nullopt, std::nullopt, std::nullopt,
                                                      "r(111)")));
              return blocks;
            }
          }
          if (hit.empty()) {
            blocks.push_back(ojson(eval::errorBlock("engine_error", "variable " + tok + " not found",
                                                    std::nullopt, std::nullopt, std::nullopt,
                                                    "r(111)")));
            return blocks;
          }
          for (const auto& h : hit)
            if (std::find(selected.begin(), selected.end(), h) == selected.end()) selected.push_back(h);
        }
      }
      auto recase = [&](std::string t) {
        for (size_t i = 0; i < t.size(); ++i) {
          char& c = t[i];
          const bool up = renUpper || (renProper && i == 0);
          c = up ? static_cast<char>(std::toupper(static_cast<unsigned char>(c)))
                 : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return t;
      };
      // The collision check runs over the WHOLE mapping before a single rename
      // is issued. Two old names onto one new name is r(198) "new name
      // specified repeatedly" (ren7); a new name that is another variable
      // outside the set is r(110) "variable already exists" (ren3).
      std::map<std::string, std::string> target;  // newname -> the name claiming it
      std::vector<std::pair<std::string, std::string>> moves;
      for (const auto& nm : selected) {
        const std::string nn = recase(nm);
        auto it = target.find(nn);
        if (it != target.end()) {
          blocks.push_back(ojson(eval::errorBlock(
              "engine_error", "1 new name specified repeatedly in newname", std::nullopt,
              std::nullopt, std::nullopt, "r(198)")));
          return blocks;
        }
        target.emplace(nn, nm);
        if (nn != nm) moves.emplace_back(nm, nn);
      }
      for (const auto& [from, to] : moves) {
        if (std::find(names.begin(), names.end(), to) != names.end() &&
            std::find(selected.begin(), selected.end(), to) == selected.end()) {
          blocks.push_back(ojson(eval::errorBlock(
              "engine_error", "1 variable already exists: " + to, std::nullopt, std::nullopt,
              std::nullopt, "r(110)")));
          return blocks;
        }
      }
      if (moves.empty()) {
        ojson note;
        note["type"] = "display";
        note["class"] = "note";
        note["text"] = "(all newnames==oldnames)";
        blocks.push_back(note);
        return blocks;
      }
      // A rename whose TARGET is another variable's current name would be
      // rejected by runRename's own exists-guard even though the mapping as a
      // whole is a valid permutation (`A`,`b` -> `a`,`B` under upper/lower).
      // Two passes through a reserved prefix keeps every intermediate name
      // unique and never collides with a real one (`__b4ren_` is not a legal
      // Stata name a user could hold).
      std::vector<std::string> renamed;
      bool viaTemp = false;
      for (const auto& [from, to] : moves) {
        if (std::find(names.begin(), names.end(), to) != names.end()) viaTemp = true;
      }
      auto doRename = [&](const std::string& from, const std::string& to) -> bool {
        ojson r = handlers::runRename(from, to, session);
        if (r.is_object() && r.value("type", std::string()) == "error") {
          blocks.push_back(r);
          return false;
        }
        return true;
      };
      if (viaTemp) {
        for (size_t i = 0; i < moves.size(); ++i) {
          if (!doRename(moves[i].first, "__b4ren_" + std::to_string(i))) return blocks;
        }
        for (size_t i = 0; i < moves.size(); ++i) {
          if (!doRename("__b4ren_" + std::to_string(i), moves[i].second)) return blocks;
        }
      } else {
        for (const auto& [from, to] : moves) {
          if (!doRename(from, to)) return blocks;
        }
      }
      for (const auto& [from, to] : moves) renamed.push_back(to);
      ojson note;
      note["type"] = "display";
      note["class"] = "note";
      note["text"] = "(" + std::to_string(renamed.size()) + " variable" +
                     (renamed.size() == 1 ? "" : "s") + " renamed)";
      blocks.push_back(note);
      return blocks;
    }
    bool hasWildcard = std::any_of(parsed.varlist.begin(), parsed.varlist.end(),
                                   [](const std::string& t) {
                                     return t.find('*') != std::string::npos ||
                                            t.find('?') != std::string::npos;
                                   });
    std::optional<long long> renumberStart;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "renumber" || k == "addnumber") {
        long long st = 1;
        if (!v.isBool) {
          try {
            st = std::stoll(trimWs(v.strValue));
          } catch (...) {
            blocks.push_back(ojson(eval::errorBlock("engine_error", k + "() invalid", std::nullopt,
                                                    std::nullopt, std::nullopt, "r(198)")));
            return blocks;
          }
        }
        renumberStart = st;
      }
    }
    const bool hasHash = parsed.varlist.size() == 2 &&
                         parsed.varlist[1].find('#') != std::string::npos;
    if (hasWildcard || hasHash) {
      // rename.ado's group form, one old pattern onto one new pattern: `*`/`?`
      // in the old names, `*` (the matched text) or `#` in the new; with
      // renumber[(#)]/addnumber[(#)] the `#` numbers the variables in dataset
      // order from # (default 1) (aer_w2 w2b_dataio rename_renumber `rename v*
      // w#, renumber(10)` -> w10 w11 w12; rename_addnumber `rename w* z#,
      // addnumber` -> z1 z2 z3; w2b_dataio.log:735-752).
      if (parsed.varlist.size() != 2) {
        blocks.push_back(ojson(eval::engineError(
            "rename: this build serves one old pattern onto one new pattern "
            "(`rename v* w*`, `rename v* w#, renumber`)")));
        return blocks;
      }
      if (!session.hasData()) {
        blocks.push_back(ojson(eval::noDataLoaded()));
        return blocks;
      }
      std::vector<std::string> names;
      if (const auto* card = session.shadowVarlistIfCurrent()) {
        for (const auto& info : *card) names.push_back(info.name);
      } else if (Datastore* rds = session.datastore()) {
        names = rds->names();
      }
      const std::string oldPat = parsed.varlist[0], newPat = parsed.varlist[1];
      // Match `oldPat` against `name`; `cap` receives the text the first
      // wildcard covered (`*` any run, `?` one character, `#` a digit run).
      std::function<bool(size_t, size_t, std::string*)> match;
      const std::string* cur = nullptr;
      match = [&](size_t i, size_t j, std::string* cap) -> bool {
        const std::string& str = *cur;
        if (i == oldPat.size()) return j == str.size();
        const char pc = oldPat[i];
        if (pc == '*' || pc == '#') {
          for (size_t k = j; k <= str.size(); ++k) {
            if (pc == '#' && k > j && !std::isdigit(static_cast<unsigned char>(str[k - 1]))) break;
            std::string sub;
            if (match(i + 1, k, &sub)) {
              if (cap != nullptr && cap->empty()) *cap = str.substr(j, k - j);
              return true;
            }
          }
          return false;
        }
        if (j < str.size() && (pc == '?' || pc == str[j])) return match(i + 1, j + 1, cap);
        return false;
      };
      std::vector<std::pair<std::string, std::string>> moves;
      long long counter = renumberStart.value_or(1);
      for (const auto& nm : names) {
        cur = &nm;
        std::string cap;
        if (!match(0, 0, &cap)) continue;
        std::string nn;
        for (char c : newPat) {
          if (c == '*') nn += cap;
          else if (c == '#') nn += renumberStart.has_value() ? std::to_string(counter) : cap;
          else nn += c;
        }
        ++counter;
        moves.emplace_back(nm, nn);
      }
      if (moves.empty()) {
        blocks.push_back(ojson(eval::errorBlock("engine_error", "variable " + oldPat + " not found",
                                                std::nullopt, std::nullopt, std::nullopt, "r(111)")));
        return blocks;
      }
      std::set<std::string> targets;
      for (const auto& [from, to] : moves) {
        if (!targets.insert(to).second) {
          blocks.push_back(ojson(eval::errorBlock(
              "engine_error", "1 new name specified repeatedly in newname", std::nullopt,
              std::nullopt, std::nullopt, "r(198)")));
          return blocks;
        }
        const bool movedAway = std::any_of(moves.begin(), moves.end(),
                                           [&](const auto& m) { return m.first == to; });
        if (std::find(names.begin(), names.end(), to) != names.end() && !movedAway) {
          blocks.push_back(ojson(eval::errorBlock(
              "engine_error", "1 variable already exists: " + to, std::nullopt, std::nullopt,
              std::nullopt, "r(110)")));
          return blocks;
        }
      }
      for (size_t i = 0; i < moves.size(); ++i) {
        if (moves[i].first == moves[i].second) continue;
        ojson r = handlers::runRename(moves[i].first, "__pren_" + std::to_string(i), session);
        if (r.is_object() && r.value("type", std::string()) == "error") {
          blocks.push_back(r);
          return blocks;
        }
      }
      for (size_t i = 0; i < moves.size(); ++i) {
        if (moves[i].first == moves[i].second) continue;
        ojson r = handlers::runRename("__pren_" + std::to_string(i), moves[i].second, session);
        if (r.is_object() && r.value("type", std::string()) == "error") {
          blocks.push_back(r);
          return blocks;
        }
      }
      return blocks;
    }
    if (parsed.varlist.size() != 2) {
      blocks.push_back(
          ojson(eval::engineError("rename requires exactly two names: `rename old new`")));
      return blocks;
    }
    // `replace` on the 1:1 form is accepted (aer_w2 w2b_dataio
    // rename_replace_opt `rename x n2012, replace` rc 0, w2b_dataio.log:720).
    // The OLD name may be an abbreviation ([U] 11.2), resolved by the rule
    // keep/drop and the by keys use (StataBE rn.do: `rename number_people_clicked
    // ppl` renames number_people_clicked_w3; 007's appendix tables).
    std::string oldName = parsed.varlist[0];
    if (oldName.find_first_of("*?-") == std::string::npos) {
      std::vector<std::string> one = {oldName};
      if (auto err = handlers::resolveByKeyNames(session, one)) {
        blocks.push_back(*err);
        return blocks;
      }
      oldName = one.front();
    }
    blocks.push_back(ojson(handlers::runRename(oldName, parsed.varlist[1], session)));
    return blocks;
  }

  if (cmd == "clonevar") {
    // `clonevar new = old` or `clonevar new old` (both forms accepted —
    // strip a literal '=' then split on whitespace, mirrors commands.py's
    // `" ".join(vl).replace("=", " ").split()`).
    // BUG-127 class: an optional storage-type prefix (`clonevar double x = y`)
    // used to make `double` the NEWVAR and silently discard `x` — strip it
    // through the same stripStorageType gen/egen already use.
    StorageTypeInfo cvInfo = stripStorageType(joinVarlist(parsed.varlist));
    // BUG-1120 review: a storage type where NEWVAR stands names nothing, as in
    // generate: `clonevar double = x` and `clonevar str5 = s` are "too few
    // variables specified" r(102) (StataBE 19.5).
    if (!cvInfo.typeToken.empty() && trimWs(cvInfo.expr).rfind('=', 0) == 0) {
      if (auto r = strutil::generateNameRefusal(
              strippedTypeWord(joinVarlist(parsed.varlist), cvInfo))) {
        blocks.push_back(ojson(eval::errorBlock("syntax_error", r->message, std::nullopt,
                                                std::nullopt, std::nullopt,
                                                "r(" + std::to_string(r->rc) + ")")));
        return blocks;
      }
    }
    if (cvInfo.stringTyped) {
      blocks.push_back(ojson(eval::engineError(
          "clonevar: string storage types are not supported — PIET stores f64/factor")));
      return blocks;
    }
    std::string joined = cvInfo.expr;
    for (auto& c : joined) {
      if (c == '=') c = ' ';
    }
    std::istringstream iss(joined);
    std::vector<std::string> toks;
    std::string tok;
    while (iss >> tok) toks.push_back(tok);
    if (toks.empty()) {
      if (auto err = conditionAsNameError(parsed.ifExpr)) {
        blocks.push_back(*err);
        return blocks;
      }
    }
    if (toks.size() < 2) {
      blocks.push_back(
          ojson(eval::engineError("clonevar: syntax is `clonevar NEWVAR = OLDVAR`")));
      return blocks;
    }
    // BS-031(c): parsed.ifExpr was already correctly extracted by the
    // tokenizer (`clonevar inc2 = income if year==1982`) but never forwarded
    // to the handler at all — see runClonevar's own comment for the fix.
    // BUG-1096: `in` rides along too (it was refused by name).
    blocks.push_back(ojson(handlers::runClonevar(toks.front(), toks.back(), session, parsed.ifExpr,
                                                 parsed.inRange)));
    appendGenMissingNote(blocks, toks.front());  // clonevar.ado's own `gen` prints it
    return blocks;
  }

  if (cmd == "generate" || cmd == "gen" || cmd == "g") {
    // BUG-336 (batch 2): known set measured from the handler body -- runGen/runBysortGen take no options.
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabGenerate.known, kVocabGenerate.unimplemented)) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    std::string raw = genExprText(parsed);
    StorageTypeInfo info = stripStorageType(raw);
    // BUG-112(1) PIET-half: the exists-guard fires BEFORE this verb splits into
    // any of its four sinks (string / standalone _n-_N / `in`-range / plain), so
    // all four reject identically. See genTargetNameError's comment for why
    // the guard is at the verb and not in the handlers.
    if (genTargetName(info.expr).empty() && info.expr.find('=') == std::string::npos) {
      if (auto err = conditionAsNameError(parsed.ifExpr)) {
        blocks.push_back(*err);
        return blocks;
      }
    }
    if (genTargetName(info.expr) == "if") {
      // `generate if=1`, which the tokenizer kept whole: the same refusal.
      if (auto err = conditionAsNameError(info.expr.substr(info.expr.find('=')))) {
        blocks.push_back(*err);
        return blocks;
      }
    }
    // `generate = 1` names no variable either (StataBE 19.5: r(102)).
    if (info.typeToken.empty() && info.expr.find('=') != std::string::npos &&
        genTargetName(info.expr).empty()) {
      blocks.push_back(ojson(eval::errorBlock("syntax_error", "too few variables specified",
                                              std::nullopt, std::nullopt, std::nullopt,
                                              "r(102)")));
      return blocks;
    }
    if (auto genExists =
            genTargetNameError(session, genTargetName(info.expr), strippedTypeWord(raw, info))) {
      blocks.push_back(*genExists);
      return blocks;
    }
    // BUG-032: string-valued / string-gated gen must be evaluated row-wise in
    // PIET (cobra is all-f64, cannot do string fns or hold string columns) —
    // mirrors dispatcher.py's `if needs_string_eval(...): return
    // run_string_gen(...)` FIRST, before the numeric cobra path. varname/rhs
    // split matches the oracle's `expr.split("=", 1)`.
    {
      std::string varname, rhs;
      size_t eq = info.expr.find('=');
      if (eq != std::string::npos) {
        varname = trimWs(info.expr.substr(0, eq));
        rhs = trimWs(info.expr.substr(eq + 1));
      } else {
        varname = trimWs(info.expr);
      }
      // BUG-456: the est-ref resolve used to run AFTER this detour decided, so
      // `gen z = (e(cmd)=="regress")` took the string path with a raw e(cmd)
      // in hand and failed at obs 1. Resolve (and fold) first; the detour then
      // sees `("regress"=="regress")` -> `(1)` and routes it numeric.
      if (auto err = preResolveGenExpr(info.expr, rhs, session)) {
        blocks.push_back(*err);
        return blocks;
      }
      if (strmutate::needsStringEval(session, varname, rhs, parsed.ifExpr, info.stringTyped,
                                     /*isReplace=*/false)) {
        // BS-031(d): fold `in` into the ifExpr this path was already
        // missing it from — see foldStringInRange's own comment above.
        StringInRangeFold fold = foldStringInRange(parsed.ifExpr, parsed.inRange);
        if (!fold.ok) {
          blocks.push_back(stringInRangeError(fold, *parsed.inRange));
          return blocks;
        }
        std::optional<std::string> combinedIf =
            fold.combinedIf.empty() ? std::nullopt : std::optional<std::string>(fold.combinedIf);
        // A declared `strN` is on varMeta BEFORE the row write, which truncates
        // to it (strmutate writeNew); a failed generate restores what was there.
        const bool strWidthDeclared = info.typeToken.size() > 3 &&
                                      info.typeToken.compare(0, 3, "str") == 0 &&
                                      info.typeToken != "strl" && !varname.empty();
        std::optional<VarMeta> priorMeta;
        bool hadMeta = false;
        if (strWidthDeclared) {
          auto it = session.varMeta.find(varname);
          hadMeta = it != session.varMeta.end();
          if (hadMeta) priorMeta = it->second;
          session.varMeta[varname].stataType = info.typeToken;
        }
        // BUG-1122: a declared numeric type with a string expression is r(109).
        const bool numTyped = !info.typeToken.empty() && !info.stringTyped;
        blocks.push_back(ojson(strmutate::runStringGen(
            session, info.expr, combinedIf, info.stringTyped,
            storage::resolveCreateType(session, info.typeToken), numTyped, parsed.inRange)));
        if (strWidthDeclared && blocks.back().is_object() &&
            blocks.back().value("type", std::string()) == "error") {
          if (hadMeta) session.varMeta[varname] = *priorMeta;
          else session.varMeta.erase(varname);
        }
        // BUG-250: `gen str8 s = ...` records its declared width like the
        // numeric sinks below record theirs.
        recordDeclaredStataType(session, varname, info, blocks.back());
        appendGenMissingNote(blocks, varname);  // BUG-1123
        return blocks;
      }
    }
    // _b[]/_se[]/e()/scalar() refs resolved to literals BEFORE the expression
    // ships to cobra (which has no estimation state) — the same resolver the
    // scalar/local/if paths use, so gen sees the same value display does. Since
    // BUG-456 this happens in preResolveGenExpr ABOVE the string detour (the
    // `if` was resolved at the parse point); the model gate lives there too.
    // BUG-432: a stored SCALAR does not depend on a model being in memory, and
    // the gate above meant `gen x = y * scalar(s)` was rc 1 in a session with no
    // estimation — while `display s` printed the value. Matrix elements are the
    // same story (BUG-428). The `if` was already substituted at the parse point;
    // this is gen's own expression, which reaches cobra rather than the row
    // evaluator and needs the literals just as much.
    // BUG-456 follow-up (review finding 3): this pass -- the one that
    // substitutes STORED SCALARS, which preResolveGenExpr above does not --
    // discarded its error block, so a mismatch that only appears after scalar
    // substitution (`scalar one = 1` then `gen z = (e(cmd)==one)`) came back as
    // the coarse "generate: expression evaluation failed at obs 1" with no r()
    // code, while the identical `count if e(cmd)==one` reported r(109).
    // name-binding-display-literals: the right-hand side only, as the bysort
    // path above does. The target is a NEW name: neither a stored scalar of
    // that name nor an existing variable it abbreviates (`gen x = 3` beside x2)
    // may rewrite it.
    {
      std::optional<ojson> genExprErr;
      const size_t eq = info.expr.find('=');
      info.expr = eq != std::string::npos
                      ? info.expr.substr(0, eq + 1) + " " +
                            substituteRowExprRefs(trimWs(info.expr.substr(eq + 1)), session,
                                                  &genExprErr)
                      : substituteRowExprRefs(info.expr, session, &genExprErr);
      if (genExprErr.has_value()) {
        blocks.push_back(*genExprErr);
        return blocks;
      }
    }
    // BS-091/BS-101 (BETA A4): c(name) refs ALSO resolve to a literal BEFORE
    // cobra sees this expression — unlike the est-scalar pass above, c()
    // values never depend on a model being in memory, so this is unGATED.
    info.expr = substituteCRefs(info.expr, session);
    if (parsed.ifExpr.has_value()) {
      parsed.ifExpr = substituteCRefs(*parsed.ifExpr, session);
    }
    // BUG-471 T2: the declared type, else c(type) — stored by the engine, not
    // only claimed (gen_untyped_13 :96 float, gen_byte_27 :222 truncated).
    const std::string genStoreType = storage::resolveCreateType(session, info.typeToken);
    if (hasNorCapN(info.expr) || (parsed.inRange.has_value() && !parsed.inRange->empty())) {
      // bysort.py's run_gen_with_n_N (standalone _n/_N, no bysort prefix) —
      // global 1..N context via cobra eval --bygen with by_vars=[]. BUG-077:
      // `in a/b` also routes here (same empty-by-list global _n/_N context)
      // so the range can fold into the same cond() the if-expr builds.
      blocks.push_back(ojson(handlers::runBysortGen(session, info.expr, {}, parsed.ifExpr, false,
                                                     false, parsed.inRange, std::nullopt,
                                                     genStoreType)));
      recordDeclaredStataType(session, genTargetName(info.expr),
                              withSessionDefaultType(session, info), blocks.back());  // BUG-250
      appendGenMissingNote(blocks, genTargetName(info.expr));  // BUG-1123
      return blocks;
    }
    blocks.push_back(ojson(handlers::runGen(info.expr, parsed.ifExpr, session, genStoreType)));
    recordDeclaredStataType(session, genTargetName(info.expr),
                            withSessionDefaultType(session, info), blocks.back());  // BUG-250
    appendGenMissingNote(blocks, genTargetName(info.expr));  // BUG-1123
    return blocks;
  }

  if (cmd == "replace") {
    std::string raw = genExprText(parsed);
    StorageTypeInfo info = stripStorageType(raw);
    // BUG-1142: the target must already exist (an abbreviation resolves; an
    // ambiguous one is r(111)) -- see replaceTargetError. It is read before the
    // options: `replace qq = 1, foo` is r(111) "variable qq not found" on
    // StataBE 19.5, r(198) "option foo not allowed" only for an existing target
    // (measured 2026-10-29).
    if (auto err = replaceTargetError(session, info)) {
      blocks.push_back(*err);
      return blocks;
    }
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabReplace.known,
                                              kVocabReplace.unimplemented)) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    bool noPromote = false;
    if (auto err = replaceNoPromote(parsed.opts, noPromote)) {
      blocks.push_back(*err);
      return blocks;
    }
    {
      std::string varname, rhs;
      size_t eq = info.expr.find('=');
      if (eq != std::string::npos) {
        varname = trimWs(info.expr.substr(0, eq));
        rhs = trimWs(info.expr.substr(eq + 1));
      } else {
        varname = trimWs(info.expr);
      }
      if (auto err = preResolveGenExpr(info.expr, rhs, session)) {  // BUG-456, as in gen
        blocks.push_back(*err);
        return blocks;
      }
      if (strmutate::needsStringEval(session, varname, rhs, parsed.ifExpr, info.stringTyped,
                                     /*isReplace=*/true)) {
        // BS-031(e): fold `in` into the ifExpr this path was already
        // missing it from — see foldStringInRange's own comment above (gen
        // branch).
        StringInRangeFold fold = foldStringInRange(parsed.ifExpr, parsed.inRange);
        if (!fold.ok) {
          blocks.push_back(stringInRangeError(fold, *parsed.inRange));
          return blocks;
        }
        std::optional<std::string> combinedIf =
            fold.combinedIf.empty() ? std::nullopt : std::optional<std::string>(fold.combinedIf);
        std::vector<std::string> notes;
        blocks.push_back(ojson(strmutate::runStringReplace(session, info.expr, combinedIf,
                                                           info.stringTyped, noPromote, &notes,
                                                           parsed.inRange)));
        // BUG-1143: Stata's notes on the values `nopromote` truncated or made
        // missing, command output like compress's lines.
        for (const std::string& text : notes) {
          ojson b;
          b["type"] = "display";
          b["text"] = text;
          blocks.push_back(std::move(b));
        }
        return blocks;
      }
    }
    // Est-scalar pre-resolution: done in preResolveGenExpr above (BUG-456).
    // Same c(name) pre-resolution as gen (BS-091/BS-101, see gen branch above).
    info.expr = substituteCRefs(info.expr, session);
    if (parsed.ifExpr.has_value()) {
      parsed.ifExpr = substituteCRefs(*parsed.ifExpr, session);
    }
    // BUG-471 T2: replace stores into the variable's own type (the handler
    // widens an integer type by Stata's lattice, or truncates under nopromote).
    const std::string replaceStoreType =
        storage::resolveReplaceType(session, genTargetName(info.expr));
    if (hasNorCapN(info.expr) || (parsed.inRange.has_value() && !parsed.inRange->empty())) {
      // BUG-077: `in a/b` also routes here — see the gen branch above.
      blocks.push_back(ojson(handlers::runBysortGen(session, info.expr, {}, parsed.ifExpr, false,
                                                     true, parsed.inRange, std::nullopt,
                                                     replaceStoreType, noPromote)));
      return blocks;
    }
    blocks.push_back(ojson(
        handlers::runReplace(info.expr, parsed.ifExpr, session, replaceStoreType, noPromote)));
    return blocks;
  }

  if (cmd == "egen") {
    // BUG-1433's class: a literal keeps its run of blanks, as in generate.
    std::string raw = genExprText(parsed);
    StorageTypeInfo info = stripStorageType(raw);
    // BUG-1120 review: `egen byte = mean(x)` names no variable after the type this
    // strip took, and egen refuses the word as a name, "byte invalid varname"
    // r(198) (StataBE 19.5); runEgen refuses a reserved NEWVAR the same way. In
    // `egen if = mean(x)` the tokenizer took `if` for the qualifier: "if invalid
    // varname" too.
    if (trimWs(raw).empty() && parsed.ifExpr.has_value() &&
        trimWs(*parsed.ifExpr).rfind('=', 0) == 0) {
      blocks.push_back(ojson(eval::errorBlock("syntax_error", "if invalid varname", std::nullopt,
                                              std::nullopt, std::nullopt, "r(198)")));
      return blocks;
    }
    if (genTargetName(info.expr).empty() && info.expr.find('=') != std::string::npos) {
      if (auto r = strutil::newVarNameRefusal(strippedTypeWord(raw, info))) {
        blocks.push_back(ojson(eval::errorBlock("syntax_error", r->message, std::nullopt,
                                                std::nullopt, std::nullopt,
                                                "r(" + std::to_string(r->rc) + ")")));
        return blocks;
      }
    }
    std::string byVal;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "by" && !v.isBool) {
        byVal = v.strValue;
        break;
      }
    }
    // BS-032 (BETA campaign C2): real Stata rejects `egen ... = group(...)` /
    // `tag(...)` combined with a by-context, `bysort state: egen g1=group(year)`
    // (which injects `by(state)` into the recursed-into sub-command below) as
    // much as an explicit `egen ..., by(state)`. runEgen answers it with the row
    // functions, as _egennoby does: r(190) (BUG-1106; it was r(198) here).
    // BUG-1420: c(NAME) in an egen argument is its value, as in generate
    // (`egen t = total(c(N))`), and a name that is no c() key is r(133).
    if (info.expr.find("c(") != std::string::npos) {
      info.expr = substituteCRefs(info.expr, session);
      if (auto undef = eval::undefinedCRef(info.expr)) {
        blocks.push_back(ojson(eval::errorBlock("engine_error", *undef, std::nullopt,
                                                std::nullopt, std::nullopt, "r(133)")));
        return blocks;
      }
    }
    // BUG-471 T2: the declared type, else c(type) (egen_mean_untyped :953);
    // runEgen withholds it from tag()/seq()/mode(), whose ados ignore it.
    const std::string egenStoreType = storage::resolveCreateType(session, info.typeToken);
    // BUG-464: an egen function of an EXPRESSION (_gmax.ado and its siblings
    // generate the expression into a temporary first) hands it to the engine's
    // f64 evaluator, which has no string function and no bofd()/dofb():
    // `egen mx = max(bofd("cal", bd))` and `egen m = max(strlen(s))` were rc 1
    // "function ... is not supported". Such an argument -- the condition route's
    // own test, strmutate::needsConditionMask -- is evaluated by the row
    // evaluator into a hidden column (strmutate::installRowExpression), which
    // the function then reads; the column leaves with this dispatch, as a
    // condition mask does. The target and info.expr stay as typed. An argument
    // the row evaluator cannot read either (`total(x[_n-1] < x & s == "")`)
    // keeps the engine's answer, as before.
    // An argument naming _n or _N takes the same route under a by-list or
    // if/in: the ados generate it over the whole frame and only then restrict
    // to if/in, where the engine read it on the if/in-filtered copy (StataBE
    // `total(_n) if x > 2` 12, PIET 6). Under by(), total/sum, mean, max, min,
    // sd, std, pc, count, mdev, skew and kurt generate it by the groups, so
    // _n and _N count within each (`egen t = total(x * _n), by(g)` 5 and 26
    // where the engine's frame-wide read gave 5 and 50); median, pctile, iqr,
    // mad and rank generate it once over the whole frame and sort within the
    // groups afterwards, so theirs stay the frame's (`median(x * _n), by(g)` 4
    // and 25 on g = 1 1 1 2 2 2, x = 1..6; `rank(mod(_n,3)), by(g)` ranks the
    // frame's positions). Such an argument the row evaluator cannot read stays
    // the engine's.
    // count() of a string expression counts the values that are not "", which
    // the engine's f64 evaluator cannot read either: `egen c = count(s + "x")`
    // is 5 in StataBE.
    std::string egenExpr = info.expr;
    {
      static const std::regex reExpFn(
          R"(^([^=]*=\s*)(count|iqr|kurt|mad|max|mdev|mean|median|min|pc|pctile|rank|sd|skew|std|sum|total)\s*\((.*)\)\s*$)");
      std::smatch fm;
      if (std::regex_match(info.expr, fm, reExpFn) && session.hasData()) {
        const std::string fn = fm.str(2);
        const std::string arg = fm.str(3);
        const bool isCount = fn == "count";
        const bool engineCannot = strmutate::needsConditionMask(session, arg) ||
                                  (isCount && strmutate::needsStringCount(session, arg));
        const bool groupPositions = !(fn == "median" || fn == "pctile" || fn == "iqr" ||
                                      fn == "mad" || fn == "rank");
        const bool restricted = (groupPositions && !byVal.empty()) ||
                                (parsed.ifExpr.has_value() && !parsed.ifExpr->empty()) ||
                                (parsed.inRange.has_value() && !parsed.inRange->empty());
        if (engineCannot || (restricted && strmutate::namesObsPosition(arg))) {
          strmutate::ConditionMaskInstall xi = strmutate::installRowExpression(
              session, arg, groupPositions ? byVal : std::string(), isCount);
          if (xi.ok) {
            conditionMask.columns.push_back(xi.column);
            egenExpr = fm.str(1) + fm.str(2) + "(" + xi.column + ")";
          } else if (engineCannot && !xi.declined) {
            blocks.push_back(xi.error);
            return blocks;
          }
        }
      }
    }
    std::string egenFixedType;  // F3: seq()'s widened type; mode()'s str#; an ado's own
    blocks.push_back(ojson(handlers::runEgen(egenExpr, byVal, parsed.ifExpr, parsed.opts, session,
                                             parsed.inRange, egenStoreType, &egenFixedType,
                                             cmdTyped == "gegen")));
    // BUG-250: `egen double m = mean(x)` — same declaration carriage as gen
    // (oracle rows J|egen-double -> double, J|egen-default -> float).
    // BUG-471 T2: tag/seq are byte and mode takes its source's type whatever was
    // declared (egen_tag_type, egen_seq_type, egen mode()).
    {
      const std::string egenTarget = genTargetName(info.expr);
      ojson& egenBlk = blocks.back();
      const bool egenFailed =
          egenBlk.is_object() && egenBlk.value("type", std::string()) == "error";
      const size_t egenEq = info.expr.find('=');
      const std::string egenRhs =
          egenEq == std::string::npos ? std::string() : info.expr.substr(egenEq + 1);
      if (egenFailed ||
          !storage::recordEgenFixedType(session, egenTarget, egenRhs, egenFixedType))
        recordDeclaredStataType(session, egenTarget, withSessionDefaultType(session, info),
                                egenBlk);
      else
        refreshCardFormat(session, egenBlk, egenTarget);  // tag() is byte: %8.0g
    }
    return blocks;
  }

  // BUG-105 (WS-K): `r` is GONE from this arm and from the regress registry
  // row. Measured live: `r price mpg` and `re price mpg` are r(199) in real
  // Stata — PIET was fabricating an abbreviation, which is the same defect
  // class as omitting one. `reg`/`regr`/`regre`/`regres` are the measured set
  // and now arrive here already normalised to "regress"; the literals kept
  // below are the two DISTINCT command names this one arm serves.
  if (cmd == "regress" || cmd == "feols" || cmd == "reghdfe") {
    // BS-063 (BETA D1): a varlist token containing '(' means an inline
    // `(endog = instruments)` IV block — real Stata syntax for THIS still
    // splits in two:
    //   - `reghdfe y (d=z), absorb(F)` IS the ivreghdfe shape (the task's
    //     own framing; cobra has no separate ivreghdfe estimator either —
    //     it is `cobra iv --fe`, same as ivreghdfe itself routes to below)
    //     -> route to the real IV handler, labelled "ivreghdfe" either way
    //     the user spelled the command.
    //   - `regress`/`reg`/`r`/`feols` with the SAME inline-paren shape is
    //     the OLD pre-`ivregress` Stata IV syntax, long retired in modern
    //     Stata in favor of `ivregress`/`ivreg2` — the D1 oracle doesn't
    //     probe this reject directly, so this keeps the pre-existing
    //     honest reject, just re-pointing the message at the real command
    //     (`ivregress`) instead of implying regress/feols will ever grow
    //     inline IV support of their own.
    bool hasIvBlock = std::any_of(parsed.varlist.begin(), parsed.varlist.end(),
                                  [](const std::string& t) { return t.find('(') != std::string::npos; });
    if (hasIvBlock) {
      if (cmd == "reghdfe") {
        // reghdfe 6.x has no IV syntax: its varlist parse stops at the `(`
        // with "parentheses unbalanced" r(132), leaving e() alone
        // (w2_esample.log:967-975 D6_reghdfe_iv_aw_cluster `132|57|57|`).
        // The IV fit is `ivreghdfe`, routed below.
        blocks.push_back(ojson(eval::errorBlock("syntax_error", "parentheses unbalanced",
                                                std::nullopt, std::nullopt, std::nullopt,
                                                "r(132)")));
        return blocks;
      }
      blocks.push_back(ojson(eval::unsupportedOption(
          "regress (IV block)",
          "Old-style inline IV syntax `regress y (endog=instruments)` is not supported by "
          "`regress`/`feols` in this version of PIET. Use `ivregress 2sls y (endog=instruments)` "
          "instead.")));
      return blocks;
    }
    // M3a: TS-context injection lands in handlers::runRegress itself
    // (translate.cpp's hasTsToken/injectTsContext, MI-083 CONTRACT-engine
    // §16) — an L./F./D./S. varlist token now injects --xtset/--tsset from
    // session.panelvar/timevar (set by the xtset/tsset handlers below)
    // before cobra sees it; with no TS context set, cobra's own
    // `ts_no_tsset` engine error surfaces, re-wrapped with the `[code]`
    // prefix runRegress's catch clause adds (matches the 2 Eval
    // M2-ratification carve-out cells byte-for-byte — see handlers.cpp).
    // `reghdfe` IS `feols --fe` under the hood (cobra has no separate
    // reghdfe estimator — verified bit-for-bit against the Stata
    // oracle); it is routed through the exact same runRegress path as
    // regress/reg/r/feols, with only the display "estimator" label
    // differing (handlers.cpp's stataCmd param).
    // BUG-539 (W2 estimopts): `reghdfe ..., residuals[(v)]` also creates a
    // column. The handler returns the estimation block alone and hands the
    // data_mutation back through this out-parameter, so it lands as a block
    // of its own (a handler returning an array is the ARRAY-vs-BLOCK abort).
    ojson residMutation;
    blocks.push_back(ojson(handlers::runRegress(parsed, session, cmd, nullptr, &residMutation)));
    if (residMutation.is_object() && !residMutation.empty()) blocks.push_back(residMutation);
    return blocks;
  }

  // ── AER-SCOPE1 bundle H (ESTIM, 2026-09-05) ────────────────────────────────
  // E1: aliases onto estimators PIET already has.
  if (cmd == "areg") {
    blocks.push_back(ojson(handlers::runAreg(parsed, session)));
    return blocks;
  }
  if (cmd == "ivreg2" || cmd == "xtivreg2") {
    blocks.push_back(ojson(handlers::runIvreg2(parsed, session, cmd)));
    return blocks;
  }
  // E2 computed PIET-side.
  if (cmd == "spearman") {
    blocks.push_back(ojson(handlers::runSpearman(parsed, session)));
    return blocks;
  }
  if (cmd == "sktest") {
    ojson skBlock = ojson(handlers::runSktest(parsed, session));
    // BUG-1490: Stata's r() after sktest, the last variable's scalars and r(table)
    // plus the hidden P_* scalars and r(Utest) (StataBE 19.5, rlife/side2.do
    // `: r(scalars)` `N p_skew p_kurt chi2 p_chi2 P_chi2 P_kurt P_skew`,
    // `: r(matrices)` `table Utest`); the hidden four refuse by name (unposted).
    if (skBlock.value("type", std::string()) != "error")
      eval::declareRPosted(session.eval, "sktest",
                           {"N", "p_skew", "p_kurt", "chi2", "p_chi2", "P_chi2", "P_kurt",
                            "P_skew", "table", "Utest"});
    blocks.push_back(std::move(skBlock));
    return blocks;
  }
  if (cmd == "signrank") {
    blocks.push_back(ojson(handlers::runSignrank(parsed, session)));
    return blocks;
  }
  if (cmd == "centile") {
    blocks.push_back(ojson(handlers::runCentile(parsed, session)));
    return blocks;
  }
  // `tsfilter` takes a REQUIRED filter word (hp, bw, bk, cf) as its first
  // token. Only `hp` is in scope; the others are named as unknown SUBcommands
  // rather than as an unknown command, because the head verb does exist.
  if (cmd == "tsfilter") {
    const std::string sub = parsed.varlist.empty() ? std::string() : lower(parsed.varlist[0]);
    if (sub != "hp") {
      blocks.push_back(ojson(eval::unsupportedCommand(
          "tsfilter " + (sub.empty() ? std::string("<none>") : sub), std::nullopt, std::nullopt,
          "piet_gap", eval::UnsupportedCommandKind::kUnknownSubcommand)));
      return blocks;
    }
    ojson r_tsfilter = handlers::runTsfilterHp(parsed, session);
    // A handler here returns EITHER one object, an EMPTY array (silent
    // success), or an array of blocks. Pushing an array where an object belongs
    // is json type_error.306 at the first consumer that reads ["type"].
    if (r_tsfilter.is_array()) {
      for (const auto& b : r_tsfilter) blocks.push_back(b);
    } else if (!r_tsfilter.is_null()) {
      blocks.push_back(r_tsfilter);
    }
    return blocks;
  }
  if (cmd == "lpoly" || cmd == "lowess") {
    const bool isLpoly = (cmd == "lpoly");
    ojson r_smooth =
        isLpoly ? handlers::runLpoly(parsed, session) : handlers::runLowess(parsed, session);
    // A handler here returns EITHER one object, an EMPTY array (silent
    // success), or an array of blocks. Pushing an array where an object belongs
    // is json type_error.306 at the first consumer that reads ["type"]. The
    // drawn form's graph block is one element of the array: it gets the
    // accepted-not-applied note and becomes the graph in memory (BS-011).
    auto finish = [&](ojson b) {
      graphAcceptedNotApplied(b, cmd, parsed.opts, isLpoly ? kLpolyApplied : kLowessApplied);
      registerGraphOutput(b, session, parsed.opts, graphLineTyped);
      blocks.push_back(std::move(b));
    };
    if (r_smooth.is_array()) {
      for (const auto& b : r_smooth) finish(b);
    } else if (!r_smooth.is_null()) {
      finish(r_smooth);
    }
    return blocks;
  }
  if (cmd == "pca") {
    // BUG-733: pca posts its e() before setLastModel (runFitWritingEFirst).
    ojson r_pca = runFitWritingEFirst(session, [&] { return handlers::runPca(parsed, session); });
    // A handler here returns EITHER one object, an EMPTY array (silent
    // success), or an array of blocks. Pushing an array where an object belongs
    // is json type_error.306 at the first consumer that reads ["type"].
    if (r_pca.is_array()) {
      for (const auto& b : r_pca) blocks.push_back(b);
    } else if (!r_pca.is_null()) {
      blocks.push_back(r_pca);
    }
    return blocks;
  }
  if (cmd == "oaxaca") {
    // AER scope-2 B2 (2026-09-08): left the E3 refusal list above once the
    // pooled twofold inference was verified against live Stata. Same
    // array-or-object shape rule as the H1 arms.
    ojson r_ox =  // BUG-733: e() before setLastModel (runFitWritingEFirst)
        runFitWritingEFirst(session, [&] { return handlers::runOaxaca(parsed, session); });
    if (r_ox.is_array()) {
      for (const auto& b : r_ox) blocks.push_back(b);
    } else if (!r_ox.is_null()) {
      blocks.push_back(r_ox);
    }
    return blocks;
  }
  if (cmd == "xi") {
    ojson xr = handlers::runXi(parsed, session);
    if (xr.is_array())
      for (const auto& b : xr) blocks.push_back(b);
    else if (!xr.is_null())
      blocks.push_back(xr);
    return blocks;
  }
  // E3: registered so the NAME resolves and the refusal carries a named cause
  // (`refused:e3_deferred`) that the route drift guard can tell apart from an
  // unrecognised token. The nearest native command for each lives on its
  // registry row's handlerTag, which is where a reader looks for it.
  if (cmd == "teffects") {
    // AER scope-2 B2-4 (2026-09-09): left the E3 refusal list below once
    // `nnmatch`'s match-set rule was measured (and the REPORT's first rule
    // REFUTED) against live Stata. The sub-command word is REQUIRED Stata
    // syntax and lands as parsed.varlist[0], the same shape `ivregress 2sls`
    // handles further down; every OTHER teffects estimator keeps its own named
    // refusal rather than sharing one cause.
    if (parsed.varlist.empty()) {
      blocks.push_back(ojson(eval::engineError(
          "syntax_error: `teffects` requires an estimator: nnmatch, ra, ipw, ipwra, aipw or "
          "psmatch",
          "syntax_error")));
      return blocks;
    }
    eval::ParsedCommand rest = parsed;
    const std::string sub = lower(rest.varlist[0]);
    rest.varlist.erase(rest.varlist.begin());
    ojson r_te =  // BUG-733: e() before setLastModel (runFitWritingEFirst)
        runFitWritingEFirst(session, [&] { return handlers::runTeffects(sub, rest, session); });
    if (r_te.is_array()) {
      for (const auto& b : r_te) blocks.push_back(b);
    } else if (!r_te.is_null()) {
      blocks.push_back(r_te);
    }
    return blocks;
  }
  // X2-GLM: `glm` left E3. runGlm returns an ARRAY of blocks (weight note,
  // iteration log, header, estimation block, convergence warning) -- pushed one
  // by one, never as a single array-valued block (the type_error.306 class).
  if (cmd == "glm") {
    ojson r_glm = handlers::runGlm(parsed, session);
    if (r_glm.is_array()) {
      for (const auto& b : r_glm) blocks.push_back(b);
    } else if (!r_glm.is_null()) {
      blocks.push_back(r_glm);
    }
    return blocks;
  }
  // E3 scaffold: each name routes to its own stub TU (handlers_e3_<cmd>.cpp),
  // which returns an ARRAY pushed element by element (never as one block --
  // the type_error.306 class). `honestdid` has no TU (source/solver question
  // open) and keeps the literal refusal below.
  {
    ojson r_e3;
    bool e3Routed = true;
    if (cmd == "swindex") r_e3 = handlers::runSwindex(parsed, session);
    else if (cmd == "qreg2") r_e3 = handlers::runQreg2(parsed, session);
    else if (cmd == "lasso" || cmd == "rlasso") r_e3 = handlers::runLasso(parsed, session);
    else if (cmd == "suest") r_e3 = handlers::runSuest(parsed, session);
    else if (cmd == "ritest") r_e3 = handlers::runRitest(parsed, session);
    else if (cmd == "acreg") r_e3 = handlers::runAcreg(parsed, session);
    else if (cmd == "leebounds") r_e3 = handlers::runLeebounds(parsed, session);
    else e3Routed = false;
    if (e3Routed) {
      if (r_e3.is_array()) {
        for (const auto& b : r_e3) blocks.push_back(b);
      } else if (!r_e3.is_null()) {
        blocks.push_back(r_e3);
      }
      return blocks;
    }
  }
  if (cmd == "honestdid") {
    blocks.push_back(ojson(eval::refusedCommand(cmdTyped, "e3_deferred")));
    return blocks;
  }

  if (cmd == "ivregress") {
    // BS-063: `ivregress 2sls|gmm|liml depvar [exog...] (endog =
    // instruments) [if] [, vce(...)]` — the estimator word ("2sls"/"gmm"/
    // "liml") is REQUIRED Stata syntax, landing as parsed.varlist[0] (the
    // tokenizer has no concept of a command's own required sub-command
    // token — same shape `estat SUBCMD` already handles elsewhere in this
    // file).
    if (parsed.varlist.empty()) {
      blocks.push_back(ojson(eval::engineError(
          "syntax_error: `ivregress` requires an estimator: 2sls, gmm, or liml", "syntax_error")));
      return blocks;
    }
    std::string method = lower(parsed.varlist[0]);
    if (method != "2sls" && method != "gmm" && method != "liml") {
      blocks.push_back(ojson(eval::engineError(
          "syntax_error: `ivregress " + parsed.varlist[0] + "` is not a recognized estimator "
          "(supported: 2sls, gmm, liml)",
          "syntax_error")));
      return blocks;
    }
    eval::ParsedCommand rest = parsed;
    rest.varlist.erase(rest.varlist.begin());
    // BUG-474: `ivregress ..., absorb()` is rc 0 in Stata's OWN base
    // ivregress.ado (`which ivregress` -> StataNow's ado/base/i/
    // ivregress.ado, tests/eval/golden/aer_b4/b5_which.log — not a
    // third-party rule), measured
    // tests/eval/golden/aer_b4/b5_absorb_class.log:38-62
    // (`B5REC|ivregress|absorb|0|`). This is the mirror of BUG-473
    // (plain `regress` must REJECT absorb()) — the two point in opposite
    // directions through the same runIvRegress `allowAbsorb` gate, so only
    // THIS call site (the plain `ivregress` command) flips; `ivreg`
    // (pre-Stata-11 spelling, below) is untouched — not measured.
    // BUG-1227: `first`'s report is a display block of its own, ahead of the
    // estimation block, as Stata prints the first stages above the IV table.
    ojson firstReport;
    ojson ivBlock =
        ojson(handlers::runIvRegress(method, rest, /*allowAbsorb=*/true, "ivregress", session,
                                     /*smallDefault=*/false, /*forceSmall=*/false, &firstReport));
    if (firstReport.is_object() && !firstReport.empty()) blocks.push_back(firstReport);
    blocks.push_back(std::move(ivBlock));
    return blocks;
  }

  if (cmd == "ivreg") {
    // AER-SCOPE1 bundle A: `ivreg y x (d = z)` is Stata's PRE-11 spelling of
    // 2SLS. Unlike `ivregress` it carries no estimator word -- the estimator
    // IS 2SLS -- so the varlist arrives without the sub-command token that arm
    // strips, and the whole difference between the two commands is that one
    // token. Routed to the same handler with the method supplied here rather
    // than a second copy of the IV path that could drift from it.
    blocks.push_back(
        ojson(handlers::runIvRegress("2sls", parsed, /*allowAbsorb=*/false, "ivregress", session,
                                     /*smallDefault=*/false, /*forceSmall=*/false)));
    return blocks;
  }

  if (cmd == "ivreghdfe") {
    // BS-063: `ivreghdfe depvar [exog...] (endog=instr), absorb(F)
    // [vce(...)]` — real Stata's ivreghdfe has no 2sls/gmm/liml sub-command
    // word of its own (always the 2SLS-family estimator); `absorb()` is
    // required in practice (that's the entire point of the command over
    // plain `ivregress`) but left to the handler's own absorb() reader to
    // enforce/ignore, matching how `reghdfe` itself doesn't hard-require
    // `absorb()` at the dispatch layer either.
    blocks.push_back(
        ojson(handlers::runIvRegress("2sls", parsed, /*allowAbsorb=*/true, "ivreghdfe", session,
                                     /*smallDefault=*/true, /*forceSmall=*/false)));
    return blocks;
  }

  if (cmd == "reg3") {
    // BS-065: `reg3 (y1 x1 x2) (y2 x3 y1) [, sure]` — multiple parenthesised
    // equation groups directly in the varlist (no leading positional
    // depvar/exog before the first paren, unlike ivregress); runReg3 does
    // its own paren-group parsing from parsed.varlist.
    ojson reg3Block = ojson(handlers::runReg3(parsed, session));
    blocks.push_back(std::move(reg3Block));
    return blocks;
  }

  if (cmd == "sureg") {
    // sureg is `reg3 <eqns>, sure` with e(cmd)
    // `sureg`: the same handler, SUR always (w2d_uncovered.log:1073).
    ojson suregBlock = ojson(handlers::runReg3(parsed, session, "sureg"));
    blocks.push_back(std::move(suregBlock));
    return blocks;
  }

  if (cmd == "boottest") {
    // BS-066: post-estimation wild cluster bootstrap — reconstructs the
    // cobra wildboot call from session.lastModel() (requires a preceding
    // vce(cluster ...) fit; runBoottest honest-rejects otherwise).
    // BUG-1175: boottest's printed report is a display block of its own, ahead of
    // the estimation block.
    ojson report;
    ojson bootBlock = ojson(handlers::runBoottest(parsed, session, &report));
    if (report.is_object() && !report.empty()) blocks.push_back(report);
    blocks.push_back(std::move(bootBlock));
    return blocks;
  }

  if (cmd == "var") {
    blocks.push_back(ojson(handlers::runVar(parsed, session)));
    return blocks;
  }

  if (cmd == "vargranger") {
    blocks.push_back(ojson(handlers::runVargranger(session)));
    return blocks;
  }

  if (cmd == "irf") {
    blocks.push_back(ojson(handlers::runIrf(parsed, session)));
    return blocks;
  }

  if (cmd == "mi") {
    blocks.push_back(ojson(handlers::runMi(parsed, session)));
    return blocks;
  }

  if (cmd == "xtabond") {
    blocks.push_back(ojson(handlers::runXtabond(parsed, session)));
    return blocks;
  }

  if (cmd == "ml") {
    blocks.push_back(ojson(handlers::runMl(parsed, session)));
    return blocks;
  }

  if (cmd == "nl" || cmd == "gmm") {
    // BUG-336: this handler consumed its real options but never rejected UNKNOWN
    // ones, so a typo was accepted and the model ran WITHOUT it. The known set is
    // MEASURED from the handler body (runNlGmm reads instruments/onestep; vce/robust stay known so nl's OWN precise reject fires);
    // an EMPTY set here eats the options the handler honours -- that first attempt
    // broke 14 freeze fixtures, and the corpus was never the problem.
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabNlGmm.known, kVocabNlGmm.unimplemented)) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    ojson r = ojson(handlers::runNlGmm(cmd, rawCmd, parsed, session));
    if (r.is_array()) {
      for (auto& b : r) blocks.push_back(std::move(b));  // a weighted nl's note, then its table
    } else {
      blocks.push_back(std::move(r));
    }
    return blocks;
  }

  if (cmd == "mixed") {
    // BUG-336: this handler consumed its real options but never rejected UNKNOWN
    // ones, so a typo was accepted and the model ran WITHOUT it. The known set is
    // MEASURED from the handler body (runMixed -> buildP7MleArgv: vce/vcov/robust/cluster);
    // an EMPTY set here eats the options the handler honours -- that first attempt
    // broke 14 freeze fixtures, and the corpus was never the problem.
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabMixed.known, kVocabMixed.unimplemented)) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    blocks.push_back(ojson(handlers::runMixed(parsed, session)));
    return blocks;
  }

  if (cmd == "svyset") {
    blocks.push_back(ojson(handlers::runSvyset(parsed, session)));
    return blocks;
  }

  if (cmd == "ivprobit" || cmd == "ivtobit") {
    // BUG-336: this handler consumed its real options but never rejected UNKNOWN
    // ones, so a typo was accepted and the model ran WITHOUT it. The known set is
    // MEASURED from the handler body (runIvMl reads ll/ul explicitly, then vce/vcov/robust/cluster);
    // an EMPTY set here eats the options the handler honours -- that first attempt
    // broke 14 freeze fixtures, and the corpus was never the problem.
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabIvMl.known, kVocabIvMl.unimplemented)) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    blocks.push_back(ojson(handlers::runIvMl(cmd, parsed, session)));
    return blocks;
  }

  // ---- P7 wave: survival ----
  if (cmd == "stset") {
    blocks.push_back(ojson(handlers::runStset(parsed, session)));
    return blocks;
  }

  if (cmd == "stcox") {
    blocks.push_back(ojson(handlers::runStcox(parsed, session)));
    return blocks;
  }

  if (cmd == "streg") {
    blocks.push_back(ojson(handlers::runStreg(parsed, session)));
    return blocks;
  }

  // ---- P7 wave: ordered / multinomial / zero-inflated / fractional ----
  if (cmd == "ologit" || cmd == "oprobit") {
    blocks.push_back(ojson(handlers::runOrdered(cmd, parsed, session)));
    return blocks;
  }

  if (cmd == "mlogit") {
    blocks.push_back(ojson(handlers::runMlogit(parsed, session)));
    installMlEsampleAfterFit(blocks.back(), session);  // BUG-1214
    return blocks;
  }

  if (cmd == "zip" || cmd == "zinb") {
    blocks.push_back(ojson(handlers::runZeroInflated(cmd, parsed, session)));
    return blocks;
  }

  if (cmd == "fracreg") {
    // BUG-336: this handler consumed its real options but never rejected UNKNOWN
    // ones, so a typo was accepted and the model ran WITHOUT it. The known set is
    // MEASURED from the handler body (runFracreg -> buildP7MleArgv: same four);
    // an EMPTY set here eats the options the handler honours -- that first attempt
    // broke 14 freeze fixtures, and the corpus was never the problem.
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabFracreg.known, kVocabFracreg.unimplemented)) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    blocks.push_back(ojson(handlers::runFracreg(parsed, session)));
    return blocks;
  }

  if (cmd == "tobit") {
    // BS-069 (BETA D2): `tobit depvar indepvars [if], ll[(#)] ul[(#)]` —
    // runTobit computes a bare ll/ul (no numeric value) from the depvar's
    // own observed min/max itself (cobra's tobit CLI has no bare-flag
    // equivalent); level() always honest-rejects (no --level surface at
    // all on cobra's tobit).
    blocks.push_back(ojson(handlers::runTobit(parsed, session)));
    installMlEsampleAfterFit(blocks.back(), session);  // BUG-1214
    return blocks;
  }

  if (cmd == "heckman") {
    // BS-070 (BETA D2): `heckman depvar indepvars [if], select(selvar =
    // selvars) [twostep]` — runHeckman NEVER forwards --twostep to the
    // engine (cobra's own --twostep flag triggers a stdout/exit-code
    // masking bug; the bare/no-flag default already computes the two-step
    // result); Stata's default (no `twostep`, full MLE) honest-rejects.
    blocks.push_back(ojson(handlers::runHeckman(parsed, session)));
    return blocks;
  }

  if (cmd == "qreg") {
    // BS-071 (BETA D2): `qreg depvar indepvars [if] [, quantile(#)
    // level(#)]` — runQreg normalizes quantile()'s fraction-or-percent
    // syntax to cobra's --tau; level() DOES forward here (live-verified:
    // cobra qreg's --level genuinely changes the CI/SE/t/p, unlike
    // tobit/ivregress/reg3's level(), which never forwards).
    // The default vce(iid) is fitted natively (runQregNative: the exact LP vertex,
    // Stata's fitted/hsheather sparsity, e(b)/e(V) with the base/omitted factor
    // columns, w2j_e3_qreg2.log A01/F00); a null reply is a form it does not take.
    {
      ojson r_qn = handlers::runQregNative(parsed, session);
      if (r_qn.is_array()) {
        for (const auto& b : r_qn) blocks.push_back(b);
        return blocks;
      }
    }
    blocks.push_back(ojson(handlers::runQreg(parsed, session)));
    return blocks;
  }

  if (cmd == "mediate") {
    // BS-072 (BETA D2): Stata 18 `mediate (Y) (M) (T, continuous(#L0 #L1))`
    // — runMediate parses the three parenthesised blocks itself (depth-
    // aware, since the treatment block nests continuous(...) inside its
    // own parens); only the continuous-treatment, two-explicit-level form
    // is in beta scope. ENGINE-PARITY not Stata-parity — see runMediate's
    // own header comment (estimation_m4.cpp) for the full disclosure.
    blocks.push_back(ojson(handlers::runMediate(parsed, session)));
    return blocks;
  }

  // ------------------------------------------------------------------
  // M3a handler-surface expansion (ROADMAP M3a item 4). Each arm mirrors
  // commands.py's classify() routing for the same command word.

  // BUG-896: tsset.ado / xtset.ado's `syntax` lines take no if/in, so either is
  // r(101) (Stata's "if not allowed" / "in range not allowed"), never a qualifier
  // silently dropped. BUG-1422: nor weights; `in` and weights reach this arm
  // (registry.h qualifiersRefusedByArm) and get Stata's texts (StataBE 19.5).
  // `tset` is `tsset` (BUG-1163).
  if ((cmd == "xtset" || cmd == "tsset" || cmd == "tset") &&
      (parsed.ifExpr.has_value() || parsed.inRange.has_value() || parsed.weightType.has_value())) {
    blocks.push_back(ojson(eval::errorBlock(
        "engine_error",
        parsed.ifExpr.has_value() ? "if not allowed"
        : parsed.inRange.has_value() ? "in range not allowed"
                                     : "weights not allowed",
        std::nullopt, std::nullopt, std::nullopt, "r(101)")));
    return blocks;
  }
  if (cmd == "xtset") {
    if (auto err = tsVarlistPass(session, parsed.varlist)) {
      blocks.push_back(*err);
      return blocks;
    }
    // xtset.ado's PanelSet: a panel variable alone takes only the display options
    // (`xtset id, noblank`, `, panelname(Grp)` are rc 0); any other option --
    // `xtset id, delta(1)`, `, yearly`, `, clear`, `, foo`, `, displayindent(x)` --
    // is r(101) (StataBE 19.5). They used to run or say "option not allowed" r(198).
    if (parsed.varlist.size() == 1 && !parsed.opts.empty()) {
      handlers::TsDeclare look;
      auto rest = parsed.opts;
      if (!takeTsDisplayOptions(rest, look) || !rest.empty()) {
        blocks.push_back(ojson(eval::errorBlock(
            "engine_error", "options not allowed unless a time variable is specified",
            std::nullopt, std::nullopt, std::nullopt, "r(101)")));
        return blocks;
      }
      blocks.push_back(
          handlers::runXtsetPanelOnly(parsed.varlist[0], session, /*declare=*/true, look));
      return blocks;
    }
    // W2D-T06: `xtset, clear` (w2b_dataio.log:323 rc 0, w2d_uncovered.log:364 rc 0)
    // drops the declaration AND its _dta chars; anything combined with it is a
    // refusal, never a half-clear.
    bool xtClear = false;
    for (const auto& [k, v] : parsed.opts)
      if (k == "clear") xtClear = true;
    if (xtClear) {
      // The options are read first (`xtset id t, clear foo` is r(198) for foo);
      // then `clear` takes no varlist, r(101) "varlist not allowed" (StataBE 19.5).
      if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabXtset.known)) {
        blocks.push_back(ojson(*err));
        return blocks;
      }
      if (!parsed.varlist.empty()) {
        blocks.push_back(ojson(eval::errorBlock("engine_error", "varlist not allowed", std::nullopt,
                                                std::nullopt, std::nullopt, "r(101)")));
        return blocks;
      }
      if (auto err = tsOnlyClearAllowed(parsed.opts)) {
        blocks.push_back(*err);
        return blocks;
      }
      session.panelvar.reset();
      session.timevar.reset();
      session.delta.reset();
      session.writeTsChars();
      return blocks;
    }
    if (parsed.varlist.empty() && parsed.opts.empty()) {
      // xtset: the bare query re-displays the declaration through the bare
      // `tsset` (w2w_tsset_tsfill.log:138 ts_xtread rc 0, w2b_dataio.log:318
      // xtset_query rc 0), PanelSet's when only a panel is declared, and
      // r(459) with nothing declared (w2d_uncovered.log:326-332).
      blocks.push_back(handlers::runTsRedisplay(session, "xtset", handlers::TsDeclare{}, false));
      return blocks;
    }
    if (parsed.varlist.empty()) {
      // The display options re-display the declaration with them (`xtset,
      // panelname(Grp)` after `xtset id` is "Grp variable: id (balanced)"); any
      // other option is `syntax [, CLEAR]`'s r(198).
      handlers::TsDeclare look;
      auto rest = parsed.opts;
      if (!takeTsDisplayOptions(rest, look)) {
        blocks.push_back(tsDisplayIndentError());
        return blocks;
      }
      if (!rest.empty()) {
        blocks.push_back(*tsOnlyClearAllowed(rest));
        return blocks;
      }
      blocks.push_back(handlers::runTsRedisplay(session, "xtset", look, false));
      return blocks;
    }
    if (parsed.varlist.size() > 2) {
      blocks.push_back(ojson(eval::engineError(
          "xtset: unexpected extra tokens after ivar [tvar]: " + joinVarlist(std::vector<std::string>(
              parsed.varlist.begin() + 2, parsed.varlist.end())) +
              ". Put each command on its own line.",
          "syntax_error")));
      return blocks;
    }
    std::optional<std::string> tvar;
    if (parsed.varlist.size() > 1) tvar = parsed.varlist[1];
    {
      // A period option (or format()) with `panelvar timevar`: tsset's own
      // declaration path, which formats the time variable and posts r(unit).
      // So does a display option, which shows tsset's Query lines with it.
      bool anyPeriod = false;
      for (const auto& [k, v] : parsed.opts)
        if ((kTssetPeriodFmt().count(k) || k == "format") && (!v.isBool || v.boolValue))
          anyPeriod = true;
      handlers::TsDeclare look;
      auto rest = parsed.opts;
      if (!takeTsDisplayOptions(rest, look)) {
        blocks.push_back(tsDisplayIndentError());
        return blocks;
      }
      if ((anyPeriod || look.display) && tvar.has_value()) {
        if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabXtset.known)) {
          blocks.push_back(ojson(*err));
          return blocks;
        }
        handlers::TsDeclare decl = look;
        for (const auto& [k, v] : parsed.opts) {
          if (k == "delta" && !v.isBool) {
            auto d = eval::pythonFloat(v.strValue);
            if (!d.has_value() || *d <= 0.0) {
              blocks.push_back(ojson(eval::errorBlock(
                  "engine_error", "invalid argument in delta(): " + v.strValue, std::nullopt,
                  std::nullopt, std::nullopt, "r(198)")));
              return blocks;
            }
            decl.delta = d;
          } else if (kTssetPeriodFmt().count(k) && (!v.isBool || v.boolValue)) {
            const std::string period = (k == "clock") ? std::string("clocktime") : k;
            if (!decl.period.empty() && decl.period != period) {
              blocks.push_back(ojson(eval::errorBlock(
                  "engine_error",
                  "may only specify one time-scale from daily, weekly, monthly, quarterly, "
                  "halfyearly, yearly, and generic",
                  std::nullopt, std::nullopt, std::nullopt, "r(198)")));
              return blocks;
            }
            decl.period = period;
          } else if (k == "format" && !v.isBool) {
            decl.format = strutil::trim(v.strValue);
          }
        }
        decl.panelvar = parsed.varlist[0];
        decl.timevar = *tvar;
        blocks.push_back(handlers::runTsDeclare(decl, session, "xtset"));
        return blocks;
      }
    }
    // BS-030 (BETA campaign C2): delta() forwards into the ts-context
    // (translate::injectTsContext -> cobra's --tsdelta); every other real
    // Stata xtset/tsset option (9 unitoptions: daily/weekly/monthly/
    // quarterly/halfyearly/yearly/generic/clocktime/business; noquery;
    // clear) has no engine-side representation in this build and is
    // honest-rejected instead of the prior silent no-op (SCOPE BS-030:
    // "all options silently dropped today").
    // BUG-233: the one copy of this vocabulary now lives in the file-scope
    // registration block (see top of file); this binding keeps every existing
    // use — including the reject net below — reading the same set.
    const std::unordered_set<std::string>& kXtsetKnownOpts = kVocabXtset.known;
    std::optional<double> xtsetDelta;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "delta" && !v.isBool) {
        auto d = eval::pythonFloat(v.strValue);
        if (!d.has_value() || *d <= 0.0) {
          blocks.push_back(ojson(eval::engineError(
              "xtset: delta() requires a positive numeric value, got `" + v.strValue + "'",
              "syntax_error")));
          return blocks;
        }
        xtsetDelta = d;
      }
    }
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kXtsetKnownOpts)) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    // The handler declares the r() set it posted (tsset.ado's Query: an empty
    // local such as r(unit) after a %9.0g time variable is not posted).
    blocks.push_back(handlers::runXtset(parsed.varlist[0], tvar, session, xtsetDelta));
    return blocks;
  }

  // BUG-1163: the obsolete `tset` is tset.ado's one line `tsset `0'`: every form,
  // option and refusal is tsset's (it used to read no option at all).
  if (cmd == "tsset" || cmd == "tset") {
    // BUG-896 (FU-581): tsset.ado's `syntax [varlist(numeric max=2)]` -- one
    // variable is a time series, two are `panelvar timevar` with the state xtset
    // leaves (xtset.ado hands its two-variable form to tsset), none re-displays
    // the declaration. Oracle tests/eval/golden/aer_w2/w2w_tsset_tsfill.log.
    //
    // `tsset, clear` (tsset.ado's Clear): drops the declaration and its chars, as
    // `xtset, clear` does (W2D-T06); nothing else may come with it.
    if (auto err = tsVarlistPass(session, parsed.varlist)) {
      blocks.push_back(*err);
      return blocks;
    }
    bool tsClear = false;
    for (const auto& [k, v] : parsed.opts)
      if (k == "clear") tsClear = true;
    if (tsClear) {
      // As xtset's `clear` above: an unknown option first, then the varlist r(101).
      if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabTsset.known)) {
        blocks.push_back(ojson(*err));
        return blocks;
      }
      if (!parsed.varlist.empty()) {
        blocks.push_back(ojson(eval::errorBlock("engine_error", "varlist not allowed", std::nullopt,
                                                std::nullopt, std::nullopt, "r(101)")));
        return blocks;
      }
      if (auto err = tsOnlyClearAllowed(parsed.opts)) {
        blocks.push_back(*err);
        return blocks;
      }
      session.panelvar.reset();
      session.timevar.reset();
      session.delta.reset();
      session.writeTsChars();
      return blocks;
    }
    if (parsed.varlist.size() > 2) {
      // w2w_tsset_tsfill.log:1460 rf_ts_three r(103).
      blocks.push_back(ojson(eval::errorBlock("engine_error", "too many variables specified",
                                              std::nullopt, std::nullopt, std::nullopt,
                                              "r(103)")));
      return blocks;
    }
    // BUG-233: the one copy of this vocabulary lives in the file-scope
    // registration block (see top of file).
    const std::unordered_set<std::string>& kTssetKnownOpts = kVocabTsset.known;
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kTssetKnownOpts)) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    handlers::TsDeclare decl;
    bool anyOption = false;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "delta" && !v.isBool) {
        auto d = eval::pythonFloat(v.strValue);
        if (!d.has_value() || *d <= 0.0) {
          blocks.push_back(ojson(eval::errorBlock(
              "engine_error", "invalid argument in delta(): " + v.strValue, std::nullopt,
              std::nullopt, std::nullopt, "r(198)")));
          return blocks;
        }
        decl.delta = d;
        anyOption = true;
      }
    }
    // B4-W3b: the PERIOD option names a %t format; `format(%fmt)` states one.
    // tsset: at most one period (w2w_tsset_tsfill.log:1542 rf_ts_twoper
    // r(198)), and a period with format() is r(198) (runTsDeclare).
    for (const auto& [k, v] : parsed.opts) {
      if (kTssetPeriodFmt().count(k) && (!v.isBool || v.boolValue)) {
        const std::string period = (k == "clock") ? std::string("clocktime") : k;
        if (!decl.period.empty() && decl.period != period) {
          blocks.push_back(ojson(eval::errorBlock(
              "engine_error",
              "may only specify one time-scale from daily, weekly, monthly, quarterly, "
              "halfyearly, yearly, and generic",
              std::nullopt, std::nullopt, std::nullopt, "r(198)")));
          return blocks;
        }
        decl.period = period;
        anyOption = true;
      } else if (k == "format" && !v.isBool) {
        decl.format = strutil::trim(v.strValue);
        anyOption = true;
      }
    }
    // The display options ride the declaration (runTsDeclare then shows the
    // Query lines with them): `tsset t, displayindent(4)` indents each line.
    {
      auto rest = parsed.opts;
      if (!takeTsDisplayOptions(rest, decl)) {
        blocks.push_back(tsDisplayIndentError());
        return blocks;
      }
    }
    if (parsed.varlist.empty()) {
      // w2w_tsset_tsfill.log:121-125 ts_redisplay, :1574 rf_ts_bare_none r(111).
      blocks.push_back(handlers::runTsRedisplay(session, "tsset", decl, anyOption));
      return blocks;
    }
    if (parsed.varlist.size() == 2) {
      decl.panelvar = parsed.varlist[0];
      decl.timevar = parsed.varlist[1];
    } else {
      decl.timevar = parsed.varlist[0];
    }
    blocks.push_back(handlers::runTsDeclare(decl, session, "tsset"));  // w2w_tsset_tsfill.log:96-99
    return blocks;
  }

  // ------------------------------------------------------------------
  // M3b: clear / set obs / cd / pwd / noop — the foundational surface the
  // `clear -> set obs N -> gen` data-construction idiom (and every do-file
  // header's `set more off` / `version 16`) depends on.
  if (cmd == "clear") {
    // BS-044 (BETA campaign C4): forward the raw subcommand token (if any)
    // — runClear itself now does the full per-subcommand scoping (kept
    // arity-capped to a single token by kFixedArityCommands above).
    std::string clearSub = !parsed.varlist.empty() ? parsed.varlist[0] : std::string();
    ojson clearBlock = ojson(handlers::runClear(clearSub, session));
    // r() is empty after a clear that wrote it (`clear all`, `clear results`;
    // RULING.md D.3.14). A clear that left r() alone records nothing.
    if (!clearBlock.is_object() || clearBlock.value("type", std::string()) != "error")
      eval::declareRPosted(session.eval, "clear", {});
    blocks.push_back(std::move(clearBlock));
    return blocks;
  }

  if (cmd == "set" && !parsed.varlist.empty() && lower(parsed.varlist[0]) == "graphics") {
    // BS-047 (BETA A4): `set graphics on|off` is now genuinely tracked —
    // graph_m4.cpp's twoway/graph_combine builders read session.graphicsOff
    // to populate the wire-schema `suppressed` field (previously hardcoded
    // false; see graph_m4.cpp's own comment at both call sites).
    std::string arg = parsed.varlist.size() > 1 ? lower(parsed.varlist[1]) : std::string();
    if (arg.empty()) {
      std::smatch m;
      static const std::regex reSetGraphics(R"(\bset\s+graphics\s+(\S+))", std::regex::icase);
      if (std::regex_search(parsed.raw, m, reSetGraphics)) arg = lower(m.str(1));
    }
    if (arg == "off") {
      session.graphicsOff = true;
    } else if (arg == "on") {
      session.graphicsOff = false;
    }
    // Neither on/off (a malformed value) silently no-ops, matching this
    // arm's pre-existing "tracked but tolerant" behavior.
    return blocks;
  }

  if (cmd == "set" && !parsed.varlist.empty() && lower(parsed.varlist[0]) == "obs") {
    std::smatch m;
    static const std::regex reSetObs(R"(\bset\s+obs\s+(\S+))", std::regex::icase);
    std::string nArg;
    if (std::regex_search(parsed.raw, m, reSetObs)) {
      nArg = m.str(1);
    } else if (parsed.varlist.size() > 1) {
      nArg = parsed.varlist[1];
    }
    blocks.push_back(ojson(handlers::runSetObs(nArg, session)));
    return blocks;
  }

  if (cmd == "set" && !parsed.varlist.empty() &&
      (lower(parsed.varlist[0]) == "level" || lower(parsed.varlist[0]) == "clevel")) {
    // BS-048 (BETA A4): `set level #` / `set clevel #` — the session
    // default confidence level, consulted ONLY when a command's own
    // `level(#)` option is absent (the existing --level flow:
    // buildEstimationArgvM4 / handlers.cpp::runRegress / runTtest).
    std::smatch m;
    static const std::regex reSetLevel(R"(\bset\s+(?:level|clevel)\s+(\S+))", std::regex::icase);
    std::string nArg = parsed.varlist.size() > 1 ? parsed.varlist[1] : std::string();
    if (std::regex_search(parsed.raw, m, reSetLevel)) nArg = m.str(1);
    bool ok = false;
    int lv = 0;
    try {
      size_t pos = 0;
      lv = std::stoi(nArg, &pos);
      ok = (pos == nArg.size());
    } catch (...) {
      ok = false;
    }
    if (!ok || lv < 1 || lv > 99) {
      blocks.push_back(ojson(eval::engineError(
          "set level(#) must be an integer between 1 and 99, e.g. `set level 90`.",
          "unsupported_option")));
      return blocks;
    }
    session.level = lv;
    return blocks;
  }

  if (cmd == "set" && !parsed.varlist.empty() && lower(parsed.varlist[0]) == "seed") {
    // BS-046 (BETA A4): `set seed #` persistence + c(seed) exposure. No
    // cobra command in this build's dispatch surface yet consumes a
    // session-level default seed (wildboot/mediate/bootstrap land in later
    // D-phase waves, per the wildboot fixed-seed+distribution parity
    // strategy) — state-tracking + c(seed) readback only until then.
    std::smatch m;
    static const std::regex reSetSeed(R"(\bset\s+seed\s+(\S+))", std::regex::icase);
    std::string nArg = parsed.varlist.size() > 1 ? parsed.varlist[1] : std::string();
    if (std::regex_search(parsed.raw, m, reSetSeed)) nArg = m.str(1);
    bool ok = false;
    long long sv = 0;
    try {
      size_t pos = 0;
      sv = std::stoll(nArg, &pos);
      ok = (pos == nArg.size());
    } catch (...) {
      ok = false;
    }
    // `set seed <statecode>` is `set rngstate` ([R] set seed; boottest
    // runs `set seed `=c(seed)'` to continue the current stream).
    if (!ok && !nArg.empty() && (nArg[0] == 'X' || nArg[0] == 'x') &&
        session.seed.stream().restore(nArg))
      return blocks;
    if (!ok) {
      blocks.push_back(ojson(eval::engineError(
          "set seed requires an integer argument, e.g. `set seed 12345`.", "unsupported_option")));
      return blocks;
    }
    session.seed = sv;
    session.rngStream = 0;  // every set seed restarts row draws, including an unchanged seed
    return blocks;
  }

  if (cmd == "set" && !parsed.varlist.empty() &&
      (lower(parsed.varlist[0]) == "ty" || lower(parsed.varlist[0]) == "typ" ||
       lower(parsed.varlist[0]) == "type")) {
    // BUG-471 T2: `set type float|double [, permanently]` — the default storage
    // type of an untyped generate/egen (c(type); w2e_float_default.log
    // settype_double_gen :1252, settype_float_restore_gen :1292,
    // settype_abbrev_ty :1335 for the `ty` spelling). It was the generic
    // "ignored" echo below, so `set type double` changed nothing. `permanently`
    // is accepted and, like `set extcompat`, session-scoped: a do-file line
    // never rewrites the user's configuration.
    static const std::unordered_set<std::string> kSetTypeOpts = {"permanently"};
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kSetTypeOpts)) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    const std::string arg = parsed.varlist.size() == 2 ? parsed.varlist[1] : std::string();
    if (arg != "float" && arg != "double") {
      // settype_bad_rc :1314 — `set type int` is r(198) and leaves c(type) as it
      // was (settype_bad_ctype_unchanged :1324).
      // StataBE's wording: :1309 for int (byte/long take it too); bare and any
      // other token per a17cd14f's StataBE 19.5 measurement.
      blocks.push_back(ojson(eval::engineError(
          arg.empty() ? std::string("nothing found where 'float' or 'double' expected")
          : (arg == "byte" || arg == "int" || arg == "long")
              ? "-set type- may not be set to " + arg + "; float and double are allowed"
              : "'" + arg + "' found where 'float' or 'double' expected",
          "unsupported_option")));
      return blocks;
    }
    session.defaultStorageType = arg;
    return blocks;
  }

  if (cmd == "set" && !parsed.varlist.empty() && lower(parsed.varlist[0]) == "more") {
    // EM1: `set more on|off [, permanently]` sets c(more) (w2af_macro_text_file.log
    // :328-405 mt_cmore_*, mt_set_more_*; w2d_uncovered.log:4505 set_more_off_perm).
    // `perm`, `permanent` and `permanently` are all accepted and, like `set type`'s,
    // session-scoped. Anything but on/off is r(198).
    for (const auto& [k, v] : parsed.opts) {
      (void)v;
      const std::string o = lower(k);
      if (o.size() < 4 || std::string("permanently").compare(0, o.size(), o) != 0) {
        blocks.push_back(ojson(eval::errorBlock("syntax_error", "option " + k + " not allowed",
                                                std::nullopt, std::nullopt, std::nullopt,
                                                "r(198)")));
        return blocks;
      }
    }
    const std::string arg = parsed.varlist.size() == 2 ? lower(parsed.varlist[1]) : std::string();
    if (arg != "on" && arg != "off") {
      blocks.push_back(ojson(eval::errorBlock("syntax_error", "invalid syntax", std::nullopt,
                                              std::nullopt, std::nullopt, "r(198)")));
      return blocks;
    }
    session.moreOn = arg == "on";
    return blocks;
  }

  if (cmd == "set" && !parsed.varlist.empty() && lower(parsed.varlist[0]) == "extcompat") {
    // WS-B (extcompat, 2026-08-06): `set extcompat on|off` — the session
    // switch for the 17 community-package compatibility names (the gate at
    // step 1a-bis above). SESSION SCOPE ONLY, deliberately: a headless
    // `--do` run must never rewrite the user's config file as a side effect
    // of a line in a script, so nothing here calls settings::save(). The
    // DURABLE choice is owned by the first-run consent dialog and the
    // Settings panel, both of which go through settings.set. This mirrors
    // real Stata, where a `set` without `, permanently` lasts the session.
    std::string arg = parsed.varlist.size() > 1 ? lower(parsed.varlist[1]) : std::string();
    if (arg.empty()) {
      std::smatch m;
      static const std::regex reSetExt(R"(\bset\s+extcompat\s+(\S+))", std::regex::icase);
      if (std::regex_search(parsed.raw, m, reSetExt)) arg = lower(m.str(1));
    }
    if (arg == "on") {
      settings::setExtcompatEnabled(true);
    } else if (arg == "off") {
      settings::setExtcompatEnabled(false);
    } else {
      // Unlike `set graphics` above, a malformed value here is NOT tolerated:
      // silently leaving the names enabled when a script asked for them off
      // would be the silent-wrong-answer class.
      blocks.push_back(ojson(eval::engineError(
          "set extcompat requires on or off, e.g. `set extcompat off`.", "unsupported_option")));
    }
    return blocks;
  }

  if (cmd == "set" && !parsed.varlist.empty() && lower(parsed.varlist[0]) == "rngstate") {
    // `set rngstate statecode`: restore the session's Stata stream from a
    // c(rngstate) string (core/rng_mt64.h; the 4-hex check field is not verified,
    // its rule being unidentified). c(seed) is untouched, as in Stata.
    static const std::regex reSetState(R"(\bset\s+rngstate\s+(\S+))", std::regex::icase);
    std::smatch m;
    std::string arg = parsed.varlist.size() > 1 ? parsed.varlist[1] : std::string();
    if (std::regex_search(parsed.raw, m, reSetState)) arg = m.str(1);
    if (!session.seed.stream().restore(arg)) {
      blocks.push_back(ojson(eval::errorBlock("syntax_error",
                                              "set rngstate: invalid RNG state",
                                              std::nullopt, std::nullopt, std::nullopt,
                                              "r(198)")));
    }
    return blocks;
  }

  if (cmd == "set" && !parsed.varlist.empty() && lower(parsed.varlist[0]) == "rng") {
    // `set rng default|mt64|mt64s|kiss32` records the setting c(rng) reads back
    // (w2j_e3_ritest.log D04 `mt64` after `set rng mt64`). PIET's own draws stay
    // Philox (src/core/rng.h): draw identity with Stata is out of scope (1-S),
    // the setting is what a do-file can observe.
    const std::string arg = parsed.varlist.size() > 1 ? lower(parsed.varlist[1]) : std::string();
    if (arg != "default" && arg != "mt64" && arg != "mt64s" && arg != "kiss32") {
      blocks.push_back(ojson(eval::errorBlock("syntax_error",
                                              arg.empty() ? "set rng: generator name required"
                                                          : "set rng: " + arg + " invalid",
                                              std::nullopt, std::nullopt, std::nullopt,
                                              "r(198)")));
      return blocks;
    }
    session.rngSetting = arg;
    return blocks;
  }

  if (cmd == "set" && !parsed.varlist.empty() &&
      canonicalSetName(lower(parsed.varlist[0])) == "matsize") {
    // BUG-1330: StataBE ignores `set matsize #` too but checks the number and
    // keeps it for c(matsize) (cclass.cpp): an integer from 10 to 800 (`+50`,
    // `050`), else r(198) `50.5 found where number expected` / `matsize must
    // be between 10 and 800` (`-5`, `0`); a second word is r(101) `varlist not
    // allowed`, an option but `permanently` r(198). Then the generic arm below
    // prints the note.
    if (parsed.varlist.size() > 2) {
      blocks.push_back(ojson(eval::errorBlock("syntax_error", "varlist not allowed", std::nullopt,
                                              std::nullopt, std::nullopt, "r(101)")));
      return blocks;
    }
    for (const auto& [k, v] : parsed.opts) {
      (void)v;
      const std::string o = lower(k);
      if (o.size() < 4 || o.size() > 11 || std::string("permanently").compare(0, o.size(), o) != 0) {
        blocks.push_back(ojson(eval::errorBlock("syntax_error", "option " + k + " not allowed",
                                                std::nullopt, std::nullopt, std::nullopt,
                                                "r(198)")));
        return blocks;
      }
    }
    const std::string arg = parsed.varlist.size() > 1 ? parsed.varlist[1] : std::string();
    static const std::regex reInteger(R"(^[+-]?\d+$)");
    std::string why;
    if (!std::regex_match(arg, reInteger)) {
      // StataBE words a word that ends in a point with two blanks (`50.  found`).
      why = arg.empty() ? std::string("nothing found where number expected")
                        : arg + (arg.back() == '.' ? "  " : " ") + "found where number expected";
    } else {
      // Past its sign and leading zeros; more than three digits is out of range.
      const size_t lead = arg.find_first_not_of("+-0");
      const long n = lead == std::string::npos ? 0
                     : arg.size() - lead > 3   ? 1000
                                               : std::stol(arg.substr(lead));
      if (arg[0] == '-' || n < 10 || n > 800) why = "matsize must be between 10 and 800";
    }
    if (!why.empty()) {
      blocks.push_back(ojson(eval::errorBlock("syntax_error", why, std::nullopt, std::nullopt,
                                              std::nullopt, "r(198)")));
      return blocks;
    }
  }

  if (cmd == "set" && !parsed.varlist.empty() && lower(parsed.varlist[0]) == "processors") {
    // BUG-1463: PIET answers as Stata/BE (c(MP) 0, c(processors_max) 1; cclass.cpp),
    // where every `set processors #` is r(198) "set processors allowed only with
    // Stata/MP." -- `set processors 1` too, and `set processors `=min(4,
    // c(processors_max))'' (StataBE 19.5). It used to fall to the ignored-setting
    // note below at rc 0.
    blocks.push_back(ojson(eval::errorBlock("syntax_error",
                                            "set processors allowed only with Stata/MP.",
                                            std::nullopt, std::nullopt, std::nullopt, "r(198)")));
    return blocks;
  }

  if (cmd == "set" && !parsed.varlist.empty()) {
    // BS-101 (BETA A4): every OTHER `set` subcommand (more, matsize, scheme,
    // trace, linesize, ...; `type` has its own arm above) is a real, common,
    // harmless Stata idiom —
    // turning it into an error would halt do-files that use it, for no
    // PIET-side benefit. Previously a TOTALLY silent no-op; now an explicit,
    // non-halting note, so at least it's visible this was recognized and
    // deliberately ignored rather than swallowed without a trace.
    const std::string setName = canonicalSetName(lower(parsed.varlist[0]));
    if (setName == "rmsg" || setName == "trace" || setName == "varabbrev") {
      // BUG-1330: StataBE 19.5 takes `on` or `off` only: none is r(100) `on or off
      // required`, a second word r(101) `varlist not allowed`, any other word
      // (`ON` too) r(198) `'foo' found where on or off expected`, as is an option
      // but `permanently`.
      std::optional<std::pair<std::string, std::string>> bad;
      if (parsed.varlist.size() < 2)
        bad = {"on or off required", "r(100)"};
      else if (parsed.varlist.size() > 2)
        bad = {"varlist not allowed", "r(101)"};
      else if (parsed.varlist[1] != "on" && parsed.varlist[1] != "off")
        bad = {"'" + parsed.varlist[1] + "' found where on or off expected", "r(198)"};
      for (const auto& [k, v] : parsed.opts) {
        (void)v;
        const std::string o = lower(k);
        if (!bad && (o.size() < 4 || o.size() > 11 ||
                     std::string("permanently").compare(0, o.size(), o) != 0))
          bad = {"option " + k + " not allowed", "r(198)"};
      }
      if (bad) {
        blocks.push_back(ojson(eval::errorBlock("syntax_error", bad->first, std::nullopt,
                                                std::nullopt, std::nullopt, bad->second)));
        return blocks;
      }
    }
    ojson note;
    note["type"] = "echo";
    note["text"] = "(set " + parsed.varlist[0] + ": not applicable to PIET — ignored)";
    blocks.push_back(note);
    // BUG-1330: c() of this setting no longer answers its default (cclass.cpp);
    // matsize's checked number and the checked on/off are the answer instead.
    // Keyed by the setting's full name, so `set rm on` and `set mat 500` reach
    // c(rmsg) and c(matsize) (they answered the default, off and 400).
    std::string kept = "1";
    if (setName == "matsize")
      kept = std::to_string(std::stoi(parsed.varlist[1]));
    else if (setName == "rmsg" || setName == "trace" || setName == "varabbrev")
      kept = parsed.varlist[1];
    session.eval.globals.set(std::string(kSetMarkerPrefix) + setName, kept);
    return blocks;
  }

  if (cmd == "version") {
    // BUG-1330: `version #` sets the version c(version) reports, for the rest of
    // this do-file or program (kVersionLocal, cclass.h); it was a silent no-op
    // and c(version) the constant 18. c(userversion) follows it outside a
    // program or loop body (versionSetsUserVersion), and only it follows
    // `version #, user` (StataBE 19.5: `version 15` then `version 16, user` is
    // 15 and 16; a program's own `version 12` is 12 and its caller's
    // userversion). Any other option is ignored, as Stata ignores it (`USER`
    // too). A bare `version` prints the version in force, `version 15.0`.
    static const std::regex reHead(R"(^\s*\S+)");
    const std::string body = std::regex_replace(parsed.raw, reHead, "");
    const size_t comma = body.find(',');
    std::istringstream nums(body.substr(0, comma));
    std::vector<std::string> toks;
    for (std::string t; nums >> t;) toks.push_back(t);
    bool user = false;
    if (comma != std::string::npos) {
      std::istringstream opts(body.substr(comma + 1));
      for (std::string t; opts >> t;) user = user || t == "user";
    }
    if (toks.size() > 1 || (toks.empty() && comma != std::string::npos)) {
      blocks.push_back(ojson(eval::errorBlock("syntax_error", "invalid syntax", std::nullopt,
                                              std::nullopt, std::nullopt, "r(198)")));
      return blocks;
    }
    if (toks.empty()) {
      const std::string* cur = session.eval.locals.find(kVersionLocal);
      ojson db;
      db["type"] = "display";
      db["text"] =
          "version " + versionText(cur != nullptr ? std::strtod(cur->c_str(), nullptr) : 19.5);
      blocks.push_back(db);
      return blocks;
    }
    double v = 0.0;
    if (auto bad = parseVersionNumber(toks[0], &v)) {
      blocks.push_back(*bad);
      return blocks;
    }
    // BUG-1459: the caller version table reads (session.callerVersion) moves with
    // it; the do-file or program that set it restores it (CallerVersionScope).
    if (!user) {
      session.eval.locals.set(kVersionLocal, eval::fmtNumeric(v));
      session.callerVersion = v;
    }
    if (user || versionSetsUserVersion(session))
      session.eval.locals.set(kUserVersionLocal, eval::fmtNumeric(v));
    return blocks;
  }

  if (cmd == "set" || cmd == "version" || cmd == "compress") {
    // BUG-336 (batch 2), compress only: it takes no options (honest no-op engine-side);
    // set/version parse their own subcommand grammar and are not option-shaped.
    if (cmd == "compress") {
      if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabCompress.known, kVocabCompress.unimplemented)) {
        blocks.push_back(ojson(*err));
        return blocks;
      }
    }
    if (cmd == "compress") {
      for (ojson& b : runCompressMeta(parsed, session)) blocks.push_back(std::move(b));
      return blocks;
    }
    // Bare `set` (no subcommand at all) / `version #` — silent, returns NO
    // block at all (commands.py's "noop" native_handler; only the echo block
    // appears in output).
    return blocks;
  }

  if (cmd == "exit") {
    // BUG-1462: `exit [[=]exp] [, clear STATA]` -- the code is an expression,
    // so `exit _rc` hands on the last return code (StataBE: `capture error
    // 459` / `exit _rc` ends the do-file with r(459)) and `exit = 3+2` is 5.
    // The exact code also rides in `reason`, so `capture exit 7` leaves _rc 7.
    std::string arg = trimWs(parsed.argText);
    if (!arg.empty() && arg[0] == '=') arg = trimWs(arg.substr(1));
    // StataBE 19.5, 2026-11-03: the code is truncated (`exit 3.7` is 3), keeps
    // its sign (`exit -1` leaves _rc -1), a missing value is 2147483647, and a
    // string is r(109) "type mismatch". An expression that fails fails as any
    // expression does: `exit nosuchname` and `exit y*2` are "... not found"
    // r(111).
    int code = 0;
    if (!arg.empty()) {
      bool isStr = false;
      std::string raised;
      std::optional<std::string> v = eval::evalArith(arg, &session.eval, &raised, &isStr);
      eval::MacroFnError fe;
      if (!v.has_value() && eval::classifyFailedExpr(arg, raised, &fe) && fe.failed()) {
        blocks.push_back(ojson(eval::errorBlock("engine_error", fe.message, std::nullopt,
                                                std::nullopt, std::nullopt, "r(" + fe.rc + ")")));
        return blocks;
      }
      if (std::optional<std::string> name = v.has_value() ? std::nullopt : firstBareName(arg)) {
        blocks.push_back(ojson(eval::errorBlock("engine_error", *name + " not found", std::nullopt,
                                                std::nullopt, std::nullopt, std::string("r(111)"))));
        return blocks;
      }
      if (v.has_value() && isStr) {
        blocks.push_back(ojson(eval::errorBlock("engine_error", "type mismatch", std::nullopt,
                                                std::nullopt, std::nullopt, std::string("r(109)"))));
        return blocks;
      }
      double d = 0.0;
      try {
        if (!v.has_value()) throw std::invalid_argument(arg);
        if (!v->empty() && (*v)[0] == '.') {
          d = 2147483647.0;
        } else {
          size_t used = 0;
          d = std::stod(*v, &used);
          if (used != v->size() || !std::isfinite(d)) throw std::invalid_argument(*v);
        }
      } catch (...) {
        ojson err = ojson(eval::engineError("invalid syntax", "syntax_error"));
        err["reason"] = "r(198)";
        blocks.push_back(std::move(err));
        return blocks;
      }
      code = static_cast<int>(std::max(-2147483648.0, std::min(d, 2147483647.0)));
    }
    std::string text = "exit" + (code != 0 ? (" " + std::to_string(code)) : std::string());
    ojson exitBlock = ojson(eval::engineError(text, "program_exit"));
    exitBlock["reason"] = "r(" + std::to_string(code) + ")";
    blocks.push_back(std::move(exitBlock));
    return blocks;
  }

  if (cmd == "cd") {
    // BUG-251/WS-Q: was `rawLower.find("cd")` + `substr(pos + 2)`. `cd` is NOT
    // abbreviable in Stata (measured: `c /tmp` answers r(199)), so this one was
    // latent rather than live — but it is the same unguarded shape, one
    // registry edit away from the crash the `use` arm actually took.
    std::string dirArg = rawArgsAfterCmd(parsed.raw);
    blocks.push_back(ojson(handlers::runCd(dirArg, session)));
    return blocks;
  }

  if (cmd == "pwd") {
    blocks.push_back(ojson(handlers::runPwd(session)));
    return blocks;
  }

  if (cmd == "log") {
    ojson r = handlers::runLog(parsed.raw, session);
    if (!(r.is_array() && r.empty())) blocks.push_back(r);
    return blocks;
  }

  if (cmd == "which") {
    // NAME not found is rc 0 for a command this build routes or a community ado
    // the oracle machine has installed (its ado/plus tree, 2026-10-21), and
    // r(111) otherwise (aer_w2 w2b_dataio which_gxtile/which_gsieve 111,
    // w2t_boottest_family A2_which_ftools 0; xtivreg2 is routed, rc 0 since the
    // 2026-10-27 install: w2_pweight_xtivreg2 W1_which_xtivreg2 0). It used to answer rc 0 "assumed installed" for every name.
    static const std::set<std::string> kInstalledAdos = {
      "_eststo", "_gcsgvar", "_gmm_dripw", "_gmm_regipw", "_gmm_repeated", "_gtools_internal",
      "_het_did_gmm", "acreg", "acregpackcheck", "appendfile", "artest", "avar", "avar9",
      "binscatter", "binsglm", "binslogit", "binsprobit", "binspwc", "binsqreg", "binsreg",
      "binsreg_irecode", "binsreg_pctile", "binsregselect", "binstest", "boottest", "boottest_1",
      "carryforward", "cdfplot", "cibar", "coefplot", "colorcheck", "colorpalette",
      "colorpalette9", "csdid", "csdid_estat", "csdid_plot", "csdid_rif", "csdid_stats",
      "csdid_table", "did_imputation", "distinct", "drdid", "drdid_display", "drdid_predict",
      "erepost", "estadd", "estfe", "estout", "estpost", "eststo", "esttab", "event_plot",
      "eventstudyinteract", "fasterxtile", "fcollapse", "fegen", "fegen_group", "figout",
      "filelist", "fisid", "flevelsof", "fmerge", "frmttable", "fsort", "ftab", "ftools",
      "gcollapse", "gcontract", "gdistinct", "gduplicates", "gegen", "geodist", "geodist_run",
      "geonear", "gglm", "gisid", "givregress", "glevelsof", "gpoisson", "gquantiles", "grc1leg",
      "grc1leg2", "grc1leg2_examples", "greg", "gregress", "greshape", "grstyle", "grstyle_set",
      "gstats", "gtools", "gtop", "gtoplevelsof", "gunique", "hashsort", "hdfe", "heatplot",
      "hexplot", "ivreg2", "ivreg210", "ivreg210_p", "ivreg28", "ivreg28_cue", "ivreg28_p",
      "ivreg29", "ivreg29_cue", "ivreg29_p", "ivreg2_p", "ivreghdfe", "join", "labcd", "labcopy",
      "labdel", "labdtch", "lablog", "labmap", "labmask", "labnoeq", "labvalch", "labvalclone",
      "labvalcombine", "labvarch", "linepalette", "local_inlist", "mat2txt", "mdesc", "metaparm",
      "missings", "ms_add_comma", "ms_compile_mata", "ms_expand_varlist", "ms_fvstrip",
      "ms_fvunab", "ms_get_version", "ms_parse_absvars", "ms_parse_varlist", "ms_parse_vce",
      "oaxaca", "outreg", "outreg2", "outreg2_prf", "parallel", "parallel_append", "parallel_bs",
      "parallel_map", "parallel_map_template.do", "parallel_sim", "parmby", "parmcip", "parmest",
      "ppmlhdfe", "ppmlhdfe_header", "ppmlhdfe_p", "qreg2", "rangestat", "rangestat_run",
      "ranktest", "ranktest11", "ranktest9", "rdbwdensity", "rdbwselect", "rddensity", "rdplot",
      "rdrobust", "rdrobustplot", "reghdfe", "reghdfe3", "reghdfe3_estat", "reghdfe3_footnote",
      "reghdfe3_p", "reghdfe5", "reghdfe5_estat", "reghdfe5_footnote", "reghdfe5_header",
      "reghdfe5_p", "reghdfe5_parse", "reghdfe_estat", "reghdfe_footnote", "reghdfe_header",
      "reghdfe_p", "regsave", "regsave_tbl", "renvarlab", "renvars", "require", "ritest",
      "scoretest", "sdid", "seeout", "shellout", "splitvallabels", "strdist", "swindex",
      "symbolpalette", "tabout", "tabstatout", "tex", "texdoc", "texsave", "tolower", "tsspell",
      "tsvmat", "unique", "ustrdist", "waldtest", "weakivtest", "winsor2"};
    std::string wname = !parsed.varlist.empty() ? parsed.varlist[0] : std::string();
    if (wname.size() >= 2 && wname.front() == '"' && wname.back() == '"')
      wname = wname.substr(1, wname.size() - 2);
    if (!wname.empty() && registry::findByName(lower(wname)) == nullptr &&
        kInstalledAdos.count(wname) == 0) {
      blocks.push_back(ojson(eval::errorBlock(
          "engine_error", "command " + wname + " not found as either built-in or ado-file",
          std::nullopt, std::nullopt, std::nullopt, "r(111)")));
      return blocks;
    }
    blocks.push_back(ojson(handlers::runWhich(parsed.raw, parsed.varlist)));
    return blocks;
  }

  if (cmd == "ssc") {
    ojson r = handlers::runSsc(parsed.varlist);
    if (!(r.is_array() && r.empty())) blocks.push_back(r);
    return blocks;
  }

  if (cmd == "confirm") {
    ojson r = handlers::runConfirm(parsed.raw, parsed.varlist, session);
    if (!(r.is_array() && r.empty())) blocks.push_back(r);
    return blocks;
  }

  if (cmd == "mkdir") {
    ojson r = handlers::runMkdir(parsed.raw, parsed.varlist, parsed.opts, session);
    if (!(r.is_array() && r.empty())) blocks.push_back(r);
    return blocks;
  }

  // BUG-180 (P2-PREP): `rmdir DIR` had NO arm at all — see runRmdir
  // (handlers_m3.cpp) for the live-probed semantics and the benchmark-harness
  // failure that surfaced it.
  if (cmd == "rmdir") {
    ojson r = handlers::runRmdir(parsed.raw, parsed.varlist, parsed.opts, session);
    if (!(r.is_array() && r.empty())) blocks.push_back(r);
    return blocks;
  }

  if (cmd == "erase" || cmd == "rm") {
    // BS-045 (BETA campaign C4): `rm` is a genuine Stata built-in synonym
    // of `erase` (live-probed `which rm` -> "built-in command: rm"), not
    // an invented alias — same handler, same behavior.
    ojson r = handlers::runErase(parsed.raw, parsed.varlist, session);
    if (!(r.is_array() && r.empty())) blocks.push_back(r);
    return blocks;
  }

  // AER-SCOPE1 bundle D: a handler may return one block, an EMPTY array (silent
  // success, the runSsc/runConfirm convention), or an array OF blocks. Pushing a
  // non-empty array as a single element is not a cosmetic mistake -- nlohmann
  // throws type_error.306 as soon as any consumer reads b["type"] off it, which
  // aborted the whole selftest binary. Flattened once here rather than per arm,
  // because the per-arm version is exactly the fix that got applied to two
  // handlers and missed the third.
  auto pushEnvResult = [&blocks](const ojson& r) {
    if (r.is_array()) {
      for (const auto& b : r) blocks.push_back(b);
    } else if (!r.is_null()) {
      blocks.push_back(r);
    }
  };

  // ==================== AER-SCOPE1 bundle D (ENV) arms ====================
  // Placed beside erase/mkdir/rmdir/ssc/which, the verbs they are siblings of.
  if (cmd == "shell") {
    pushEnvResult(handlers::runShell(rawArgsAfterCmd(parsed.raw), session));
    return blocks;
  }
  if (cmd == "winexec") {
    blocks.push_back(ojson(eval::refusedCommand(cmdTyped, "no_os_process")));
    return blocks;
  }
  if (cmd == "type") {
    pushEnvResult(handlers::runType(parsed.raw, parsed.varlist, parsed.opts, session));
    return blocks;
  }
  if (cmd == "dir" || cmd == "ls") {
    pushEnvResult(handlers::runDir(parsed.raw, parsed.varlist, parsed.opts, session));
    return blocks;
  }
  if (cmd == "net") {
    pushEnvResult(handlers::runNet(parsed.raw, parsed.varlist, parsed.opts, session));
    return blocks;
  }
  if (cmd == "ado") {
    pushEnvResult(handlers::runAdo(parsed.raw, parsed.varlist));
    return blocks;
  }
  if (cmd == "adopath") {
    pushEnvResult(handlers::runAdopath(parsed.raw, parsed.varlist, session));
    return blocks;
  }
  if (cmd == "sysdir") {
    pushEnvResult(handlers::runSysdir(parsed.raw, parsed.varlist, session));
    return blocks;
  }
  if (cmd == "findfile") {
    pushEnvResult(handlers::runFindfile(parsed.raw, parsed.varlist, parsed.opts, session));
    return blocks;
  }
  if (cmd == "sleep") {
    pushEnvResult(handlers::runSleep(parsed.varlist));
    return blocks;
  }
  if (cmd == "error") {
    // `error exp`: the code is an expression too, as for exit (BUG-1462) --
    // `capture error 459` / `error _rc` raises r(459) (StataBE). A literal
    // still goes straight through; anything else is evaluated first.
    // StataBE 19.5, 2026-11-03: the code is rounded (`error 3.7` raises r(4)),
    // a code below 0 or above 99999, or a missing one, is r(100000) with no
    // message (`error 12345678`, `error 1e6`), and a bare `error` ("invalid
    // syntax"), a name that is no value ("abc not found") or a string ("type
    // mismatch") is r(197). A leading `=` is read as the start of a name:
    // `error = 3+2` is "=3 invalid name" r(197).
    std::vector<std::string> codeArg = parsed.varlist;
    const std::string arg = trimWs(parsed.argText);
    auto refuse197 = [&](const std::string& msg) {
      blocks.push_back(ojson(eval::errorBlock("engine_error", msg, std::nullopt, std::nullopt,
                                              std::nullopt, std::string("r(197)"))));
      return blocks;
    };
    if (arg.empty()) return refuse197("invalid syntax");
    if (arg[0] == '=') {
      std::smatch nm;
      const std::string after = trimWs(arg.substr(1));
      std::regex_search(after, nm, std::regex(R"(^[A-Za-z0-9_]*)"));
      return refuse197("=" + nm.str(0) + " invalid name");
    }
    auto clampCode = [](double d) {
      const long long rounded = std::llround(d);
      return std::to_string(rounded < 0 || rounded > 99999 ? 100000LL : rounded);
    };
    if (!std::regex_match(arg, std::regex(R"([+-]?[0-9]+)"))) {
      bool isStr = false;
      std::optional<std::string> v = eval::evalArith(arg, &session.eval, nullptr, &isStr);
      if (!v.has_value()) {
        if (std::regex_match(arg, std::regex(R"([A-Za-z_][A-Za-z0-9_]*)")))
          return refuse197(arg + " not found");
        return refuse197("invalid syntax");
      }
      if (isStr) return refuse197("type mismatch");
      double d = 0.0;
      bool numeric = false;
      try {
        size_t used = 0;
        d = std::stod(*v, &used);
        numeric = used == v->size() && std::isfinite(d);
      } catch (...) {
      }
      if (!v->empty() && (*v)[0] == '.') {
        codeArg = {"100000"};
      } else if (numeric) {
        codeArg = {clampCode(d)};
      } else {
        codeArg = {*v};
      }
    } else {
      codeArg = {clampCode(std::stod(arg))};
    }
    pushEnvResult(handlers::runErrorCode(codeArg));
    return blocks;
  }
  if (cmd == "pause") {
    // `pause on|off` toggles Stata's interactive debugger. PIET is never
    // interactive, so the toggle can have no effect -- said out loud rather
    // than swallowed, because a do-file that pauses expects to stop.
    std::string sub = parsed.varlist.empty() ? "" : lower(parsed.varlist[0]);
    ojson note;
    note["type"] = "display";
    note["text"] = "(pause " + (sub.empty() ? std::string("") : sub) +
                   ": PIET is non-interactive -- no effect)";
    blocks.push_back(note);
    return blocks;
  }
  if (cmd == "cls") {
    // Nothing to clear: PIET's Results pane is the block stream itself, and a
    // command that erased it would erase its own evidence. Silent accept.
    return blocks;
  }
  if (cmd == "gr_edit") {
    ojson note;
    note["type"] = "display";
    note["text"] = "(gr_edit: graph-editor recording, appearance only -- ignored)";
    blocks.push_back(note);
    return blocks;
  }
  if (cmd == "bcal") {
    // AER scope-2 B2-3 (2026-09-09): served (it was a refusal). Same
    // array-or-object shape rule as the H1 arms.
    ojson r_bc = handlers::runBcal(parsed, session);
    if (r_bc.is_array()) {
      for (const auto& b : r_bc) blocks.push_back(b);
    } else if (!r_bc.is_null()) {
      blocks.push_back(r_bc);
    }
    return blocks;
  }
  if (cmd == "twoway__histogram_gen") {
    // gfx-histogram-kdensity-family: served (it was refused as a Stata
    // internal). An array of blocks on success, one error object otherwise.
    ojson r_hg = handlers::runTwowayHistogramGen(parsed, session);
    if (r_hg.is_array()) {
      for (const auto& b : r_hg) blocks.push_back(b);
    } else if (!r_hg.is_null()) {
      blocks.push_back(r_hg);
    }
    return blocks;
  }
  if (cmd == "frames") {
    std::string sub = parsed.varlist.empty() ? "" : lower(parsed.varlist[0]);
    if (sub == "reset") {
      pushEnvResult(handlers::runFramesReset(session));
      return blocks;
    }
    // Every other sub-command is `frame` under its plural spelling. Rewriting
    // and re-dispatching keeps ONE implementation of the frame family rather
    // than a second copy that can drift from it.
    std::string rewritten = parsed.raw;
    size_t at = rewritten.find("frames");
    if (at != std::string::npos) rewritten.replace(at, 6, "frame");
    return piet::dispatch(rewritten, session);
  }
  if (cmd == "timer") {
    // BS-124 (BETA campaign C4): "timer" was already in kNoDataCommands
    // above (correctly — it must work with zero data loaded) but had NO
    // dispatch arm at all, so every call fell through to the bottom
    // unsupported_command fallback. sub/slot parsing here is deliberately
    // minimal (matches the varlist shape `timer SUB [N]`); runTimer does
    // all real validation (slot range, state-machine rules).
    std::string timerSub = !parsed.varlist.empty() ? lower(parsed.varlist[0]) : std::string();
    std::optional<int> timerSlot;
    if (parsed.varlist.size() > 1) {
      try {
        size_t pos = 0;
        int n = std::stoi(parsed.varlist[1], &pos);
        if (pos == parsed.varlist[1].size()) timerSlot = n;
      } catch (...) {
        // leave timerSlot unset; runTimer treats a missing/garbled slot on
        // "on"/"off" as its own "invalid syntax" reject.
      }
    }
    ojson r = handlers::runTimer(timerSub, timerSlot, session);
    if (!(r.is_array() && r.empty())) blocks.push_back(r);
    return blocks;
  }

  // BUG-343: `file open|write|read|close`. Registered under the first word with
  // sub-verb routing inside the handler — the same shape `export`/`import` use
  // (registry.cpp), because the command table has to recognize the LINE before
  // anything can look at what follows it. An empty result means "did its work,
  // says nothing", exactly like postfile below.
  if (cmd == "file") {
    ojson r = handlers::runFileCmd(parsed.raw, session);
    if (!(r.is_array() && r.empty()) && !r.is_null()) blocks.push_back(r);
    return blocks;
  }

  if (cmd == "postfile") {
    ojson r = handlers::runPostfile(parsed.raw, session);
    if (!(r.is_array() && r.empty())) blocks.push_back(r);
    return blocks;
  }

  if (cmd == "post") {
    ojson r = handlers::runPost(parsed.raw, session);
    if (!(r.is_array() && r.empty())) blocks.push_back(r);
    return blocks;
  }

  if (cmd == "postclose") {
    std::string handle = parsed.varlist.empty() ? std::string() : parsed.varlist[0];
    ojson r = handlers::runPostclose(handle, session);
    if (!(r.is_array() && r.empty())) blocks.push_back(r);
    return blocks;
  }

  // BUG-1417: drop and keep are r-class. A drop or keep that ran posts
  // r(k_drop), the variables it removed, or for `if`/`in` r(N_drop), the
  // observations, and replaces all of r(); a failed one leaves r() alone
  // (StataBE 19.5: `drop pv` r(k_drop) 1 and r(sum) `.`; `drop if _n > 38`
  // r(N_drop) 2; `keep` posts r(k_drop) too).
  // The counts come off the resident card and the verb's own result block
  // (n_rows_after, varlist_after) before any datastore() open: a filter must
  // not materialize the head to learn one integer (INSPECT-2608).
  const auto dropKeepCount = [&session](bool rows) -> int64_t {
    if (session.tempDropDepth > 0) return -1;
    try {
      if (rows) {
        if (std::optional<int64_t> n = session.residentRowsIfCurrent()) return *n;
      } else if (const std::vector<VarInfo>* card = session.residentReadCard()) {
        return std::count_if(card->begin(), card->end(),
                             [](const VarInfo& v) { return !isHiddenColumn(v.name); });
      }
      Datastore* ds = session.datastore();
      if (ds == nullptr) return -1;
      if (rows) return ds->nRows();
      const std::vector<std::string> names = ds->names();
      return std::count_if(names.begin(), names.end(),
                           [](const std::string& n) { return !isHiddenColumn(n); });
    } catch (const std::exception&) {
      return -1;
    }
  };
  const auto postDropKeepR = [&session, &dropKeepCount, &cmd](const ojson& blk, bool rows,
                                                         int64_t before) {
    if (session.tempDropDepth > 0) return;
    if (before < 0 || !blk.is_object() || blk.value("type", std::string()) == "error") return;
    int64_t after = -1;
    if (rows && blk.contains("n_rows_after") && blk["n_rows_after"].is_number_integer()) {
      after = blk["n_rows_after"].get<int64_t>();
    } else if (!rows && blk.contains("varlist_after") && blk["varlist_after"].is_array()) {
      after = 0;
      for (const auto& v : blk["varlist_after"]) {
        const std::string nm = v.is_object() ? v.value("name", std::string())
                                             : (v.is_string() ? v.get<std::string>() : "");
        if (!isHiddenColumn(nm)) ++after;
      }
    } else {
      after = dropKeepCount(rows);
    }
    if (after < 0) return;
    const std::string key = rows ? "N_drop" : "k_drop";
    eval::replaceRClass(session.eval);
    session.eval.rValues.set(key, static_cast<double>(before - after));
    eval::declareRPosted(session.eval, cmd, {key});
  };

  if (cmd == "drop") {
    // BS-097: `drop varlist` / `drop if COND` never had comma-options to
    // begin with (neither branch below reads parsed.opts) — any leftover
    // key is unconditionally unconsumed, so the known-set is the empty set.
    if (auto err = eval::rejectUnknownOptions(parsed.opts, {})) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    if (parsed.ifExpr.has_value() || parsed.inRange.has_value()) {
      // BUG-032: a string-referencing condition can't go to cobra's all-f64
      // eval --keepif; filter row-wise. Numeric stays cobra. BS-090 (BETA
      // Phase G): a co-occurring `in` is ANDed into the SAME row-wise mask
      // when the string detour fires (strmutate::filterRowsIf), and
      // composed via piet::handlers::applyIfAndInFilter otherwise.
      const int64_t rowsBefore = dropKeepCount(true);
      ojson res = strmutate::needsStringCount(session, parsed.ifExpr)
                      ? ojson(strmutate::filterRowsIf(session, *parsed.ifExpr, /*keep=*/false,
                                                      parsed.inRange))
                      : ojson(handlers::runDropIf(parsed.ifExpr.value_or(""), session,
                                                  parsed.inRange));
      postDropKeepR(res, true, rowsBefore);
      blocks.push_back(std::move(res));
      return blocks;
    }
    // Variable-name abbreviation ([U] 11.2): a unique prefix stands for its
    // variable, several are r(111) "ambiguous abbreviation" -- the rule the by
    // keys already use (resolveByKeyNames). StataBE ab.do: `keep
    // number_people_clicked` keeps number_people_clicked_w3 (007's Wave 3).
    {
      std::vector<std::string> names = parsed.varlist;
      if (auto err = handlers::resolveByKeyNames(session, names)) {
        blocks.push_back(*err);
        return blocks;
      }
      parsed.varlist = names;
    }
    const int64_t varsBefore = dropKeepCount(false);
    ojson dropped = ojson(handlers::runDrop(parsed.varlist, session));
    // BUG-785: dropping EVERY variable (`drop _all`, or a varlist naming them
    // all) leaves Stata with no observations either -- `drop _all` / `set obs 5`
    // reads "Number of observations (_N) was 0, now 5" (w2_esample.log:1205
    // E11). The engine's column drop kept the row count, so `set obs 5` after
    // it was "cannot reduce the number of observations from 61 to 5", an
    // uncaptured stop (w2_esample.do:403, the replay's first stop). The rows go
    // too, but the dataset stays loaded and EMPTY, not unloaded: Stata still
    // runs `generate` on it (selftest BUG-291 `gen double x` / `drop x` / `gen
    // x = 1` answers float, WSN 291a). A hidden column (the e(sample) mask)
    // keeps its name over zero rows, and rows `set obs` adds later hold missing
    // -- outside the sample, as Stata reads them (E11 counts 0).
    if (dropped.is_object() && dropped.value("type", std::string()) == "data_mutation") {
      bool noVarsLeft = false;
      if (dropped.contains("varlist_after") && dropped["varlist_after"].is_array()) {
        noVarsLeft = dropped["varlist_after"].empty();
      } else {
        try {
          Datastore* dds = session.datastore();
          noVarsLeft = dds != nullptr && dds->names().empty();
        } catch (const std::exception&) {
          noVarsLeft = false;  // unreadable head: leave it to the next verb's honest error
        }
      }
      if (noVarsLeft && session.currentHbin().has_value()) {
        const std::string emptied = mintScratchPath(session.workdir(), "drop_all_rows");
        try {
          runCobraJson({"eval", "--data", *session.currentHbin(), "--keepif", "0", "--out",
                        emptied});
          session.load(emptied);
          if (dropped.contains("n_rows_after")) dropped["n_rows_after"] = 0;
        } catch (const CobraError& ce) {
          blocks.push_back(dropped);
          blocks.push_back(ojson(eval::engineError(
              "drop: every variable was dropped, but the observations could not be removed (" +
              std::string(ce.what()) + ")")));
          return blocks;
        }
      }
    }
    postDropKeepR(dropped, false, varsBefore);
    blocks.push_back(dropped);
    return blocks;
  }

  if (cmd == "keep") {
    // BS-097: same zero-consumption argument as `drop` immediately above.
    if (auto err = eval::rejectUnknownOptions(parsed.opts, {})) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    if (parsed.ifExpr.has_value() || parsed.inRange.has_value()) {
      const int64_t rowsBefore = dropKeepCount(true);
      ojson res = strmutate::needsStringCount(session, parsed.ifExpr)
                      ? ojson(strmutate::filterRowsIf(session, *parsed.ifExpr, /*keep=*/true,
                                                      parsed.inRange))
                      : ojson(handlers::runKeepIf(parsed.ifExpr.value_or(""), session,
                                                  parsed.inRange));
      postDropKeepR(res, true, rowsBefore);
      blocks.push_back(std::move(res));
      return blocks;
    }
    {
      // Abbreviations, as for `drop` just above.
      std::vector<std::string> names = parsed.varlist;
      if (auto err = handlers::resolveByKeyNames(session, names)) {
        blocks.push_back(*err);
        return blocks;
      }
      parsed.varlist = names;
    }
    const int64_t varsBefore = dropKeepCount(false);
    ojson kept = ojson(handlers::runKeep(parsed.varlist, session));
    postDropKeepR(kept, false, varsBefore);
    blocks.push_back(std::move(kept));
    return blocks;
  }

  if (cmd == "sort") {
    // BUG-336 (batch 2): known set measured from the handler body -- runSort takes no options.
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabSort.known, kVocabSort.unimplemented)) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    blocks.push_back(ojson(handlers::runSort(parsed.varlist, session)));
    return blocks;
  }

  if (cmd == "gsort") {
    // BUG-336 (batch 2): known set measured from the handler body -- runGsort takes no options.
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabGsort.known, kVocabGsort.unimplemented)) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    blocks.push_back(ojson(handlers::runGsort(parsed.varlist, session)));
    return blocks;
  }

  if (cmd == "order") {
    blocks.push_back(ojson(handlers::runOrder(parsed.varlist, parsed.opts, session)));
    return blocks;
  }

  if (cmd == "recode") {
    // BUG-336 (batch 2): generate is KNOWN (the debug_all/pangram stanzas use
    // `recode ..., gen(edu3)`). runRecode re-tokenizes the raw line and reads
    // generate()/into()/label() from its canonicalized options, so `g()' (the
    // vocabulary's declared alias) reaches it as generate.
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabRecode.known, kVocabRecode.unimplemented)) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    // BS-031(b): parsed.ifExpr was already correctly extracted by the
    // tokenizer but never forwarded — see runRecode's own comment.
    // Under `test` the handler returns the mutation and test's notes.
    ojson r = handlers::runRecode(parsed.raw, session, parsed.ifExpr);
    if (r.is_array()) {
      for (const auto& b : r) blocks.push_back(b);
    } else {
      blocks.push_back(r);
    }
    return blocks;
  }

  if (cmd == "collapse") {
    const int64_t collapseNBefore = rowsBeforeForRN(session);
    // Step-1 D3: `parsed.weightVar` only survives the tokenizer's guard
    // here when weightType is fw/fweight (collapse is on the fweight
    // allow-list) — any other weight type was already rejected upstream.
    // The flavor the user typed: gcollapse and fcollapse label, format and type
    // their results by their own rules (collapseResultMeta).
    const std::string collapseFlavor =
        (cmdTyped == "gcollapse" || cmdTyped == "fcollapse") ? cmdTyped : std::string("collapse");
    // Each flavor's own option net; an unknown word is r(198) (cz_c_err_opt).
    // collapse_17: by() cw fast clabel frame(). ftools' fcollapse: by() cw fast
    // nocompress append smart verbose here (its alias row refused the rest by
    // name), and `compress`, noCOMPRESS's positive spelling, which asks for the
    // default. gcollapse: what its alias row lets through. clabel and frame()
    // are collapse's alone (BUG-1100: `fcollapse ..., clabel` and `gcollapse
    // ..., clabel` are `option clabel not allowed` r(198) in StataBE 19.5).
    // verbose is read by nothing here; it reaches the net so that the flag
    // check below sees it.
    static const std::unordered_set<std::string> kFcollapseArmOpts = {
        "by", "cw", "fast", "nocompress", "compress", "append", "smart", "verbose"};
    static const std::unordered_set<std::string> kGcollapseArmOpts = {"by", "cw", "fast",
                                                                      "verbose"};
    const std::unordered_set<std::string>& collapseArmOpts =
        collapseFlavor == "fcollapse"   ? kFcollapseArmOpts
        : collapseFlavor == "gcollapse" ? kGcollapseArmOpts
                                        : kVocabCollapse.known;
    if (auto err = eval::rejectUnknownOptions(parsed.opts, collapseArmOpts,
                                              kVocabCollapse.unimplemented)) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    // frame(name): syntax's `name` check, then nothing -- collapse_17 never reads
    // it (StataBE 19.5: frame(fz) collapses in memory and leaves no frame fz;
    // frame(1x) is `1x invalid name` r(198), frame(a b) `option frame():  too
    // many names specified` r(103), frame() rc 0). A bare frame or a bare by, or
    // a flag given an argument (clabel(x), append(x), smart(1), fast(1),
    // verbose(1)), is `option ... incorrectly specified` r(198) in all three
    // flavors; by() is no by() at all (rc 0, one observation).
    for (const auto& [k, v] : parsed.opts) {
      if ((k == "frame" || k == "by") == v.isBool) {
        // The option as typed: an abbreviation reached here under its full key,
        // and the alias row rebuilt parsed.raw from the full keys (StataBE 19.5,
        // ftools 2.49.0: fcollapse `v(1)` is `option v() incorrectly specified`).
        std::string typed = k;
        const long long comma = eval::findUnquotedComma(rawCmd);
        if (!v.isBool && comma >= 0) {
          static const std::regex reCalled(R"(([A-Za-z_][A-Za-z0-9_]*)\s*\()");
          const std::string tail = rawCmd.substr(static_cast<size_t>(comma));
          for (std::sregex_iterator it(tail.begin(), tail.end(), reCalled), end; it != end; ++it) {
            const std::string w = (*it)[1].str();
            if (k.compare(0, w.size(), strutil::lower(w)) == 0) {
              typed = w;
              break;
            }
          }
        }
        blocks.push_back(ojson(eval::errorBlock(
            "syntax_error", "option " + typed + (v.isBool ? "" : "()") + " incorrectly specified",
            std::nullopt, std::nullopt, std::nullopt, std::string("r(198)"))));
        return blocks;
      }
      if (k != "frame") continue;
      std::istringstream names(v.strValue);
      std::string first, more;
      names >> first >> more;
      if (!more.empty()) {
        blocks.push_back(ojson(eval::errorBlock("syntax_error",
                                                "option frame():  too many names specified",
                                                std::nullopt, std::nullopt, std::nullopt,
                                                std::string("r(103)"))));
        return blocks;
      }
      if (!first.empty() && !strutil::isValidStataIdentifier(first)) {
        blocks.push_back(ojson(eval::errorBlock("syntax_error", first + " invalid name",
                                                std::nullopt, std::nullopt, std::nullopt,
                                                std::string("r(198)"))));
        return blocks;
      }
    }
    // noCOMPRESS's two spellings together: StataBE 19.5 `fcollapse ..., compress
    // nocompress` (either order) is `option nocompress not allowed` r(198).
    {
      bool compress = false, nocompress = false;
      for (const auto& kv : parsed.opts) {
        compress = compress || kv.first == "compress";
        nocompress = nocompress || kv.first == "nocompress";
      }
      if (compress && nocompress) {
        blocks.push_back(ojson(eval::errorBlock("syntax_error", "option nocompress not allowed",
                                                std::nullopt, std::nullopt, std::nullopt,
                                                std::string("r(198)"))));
        return blocks;
      }
    }
    ojson collapseBlk = ojson(handlers::runCollapse(parsed.raw, parsed.ifExpr, session,
                                                    parsed.opts, parsed.weightVar,
                                                    parsed.inRange, collapseFlavor,
                                                    parsed.weightType));
    if (collapseBlk.value("type", std::string()) != "error" && collapseNBefore >= 0)
      session.eval.rValues.set("N", static_cast<double>(collapseNBefore));
    // e(sample) T2: runCollapse wrote collapse's marker rule (first observation
    // of each by-group, StataBE capture w2_esample_copies); the gtools/ftools
    // spellings reach the same engine verb, but their own rule is unmeasured.
    if (collapseBlk.value("type", std::string()) != "error" &&
        (cmdTyped == "gcollapse" || cmdTyped == "fcollapse") &&
        !handlers::esampleMarkHeadLost(session, EsampleLostBy::kCollapseShim)) {
      collapseBlk = ojson(eval::engineError(
          cmdTyped + ": the data were collapsed, but the estimation-sample marker could not be "
                     "withdrawn from the result; e(sample) is not reliable until the next "
                     "estimation."));
    }
    blocks.push_back(std::move(collapseBlk));
    return blocks;
  }

  if (cmd == "encode") {
    std::string strvar = parsed.varlist.empty() ? std::string() : parsed.varlist[0];
    if (strvar.empty()) {
      blocks.push_back(ojson(eval::engineError(
          "encode: syntax error — usage: encode STRVAR, gen(NEWVAR) [label(LBLNAME)]")));
      return blocks;
    }
    // BS-097: `generate`/`label` are the encode options this arm reads, and
    // `noextend` since BUG-1096 (runEncode).
    //
    // BUG-177 (P2-PREP): the canonical name registered here used to be the
    // ABBREVIATION `gen`, so Stata's own full spelling `encode grp,
    // generate(newv)` was rejected outright ("option `generate' not
    // allowed"). eval::resolveOptionAbbrev is one-sided by construction — it
    // resolves abbrev -> canonical only — so registering an abbreviation as
    // the canonical name makes the FULL spelling permanently unreachable,
    // the exact inverse of what the helper is for. Registering `generate`
    // and canonicalizing first fixes both directions at once: `gen`, `gene`,
    // ... `generate` all resolve, and the read below sees one spelling.
    static const std::unordered_set<std::string> kEncodeKnownOpts = {"generate", "label",
                                                                     "noextend"};
    auto encodeOpts = parsed.opts;
    eval::canonicalizeOptionKeys(encodeOpts, kEncodeKnownOpts);
    std::optional<std::string> genVal, lblVal;
    bool noExtend = false;
    for (const auto& [k, v] : encodeOpts) {
      if (k == "generate" && !v.isBool) genVal = v.strValue;
      if (k == "label" && !v.isBool) lblVal = v.strValue;
      if (k == "noextend" && v.isBool && v.boolValue) noExtend = true;
    }
    if (auto err = eval::rejectUnknownOptions(encodeOpts, kVocabEncode.known, kVocabEncode.unimplemented)) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    if (!genVal.has_value() || genVal->empty()) {
      // Stata's rc 198 (w2ah_data_verbs.log ah_encode_nogen); main answered 1.
      blocks.push_back(ojson(eval::errorBlock("syntax_error",
                                              "option generate() required", std::nullopt,
                                              std::nullopt, std::nullopt, std::string("r(198)"))));
      return blocks;
    }
    blocks.push_back(
        ojson(handlers::runEncode(strvar, *genVal, lblVal, session, parsed.ifExpr, noExtend)));
    return blocks;
  }

  // BS-003/D4 (BETA B-data): `destring`/`tostring` — PIET-NATIVE (no cobra
  // verb, no hbin-v2 dependency; handlers_data_verbs2.cpp's runDestring/
  // runTostring ride the existing factor-column machinery).
  //
  // BUG-664/665 (closes BUG-440): both now take a VARLIST, as Stata's destring
  // and tostring do -- the handler converts one variable and this arm loops
  // over the varlist. A variable the handler declines (nonnumeric content,
  // irreversible rendering, already the target type) yields its note block and
  // the loop moves on at rc 0; a real error ends the command. generate() takes
  // one new name per source variable, checked up front (a count mismatch is
  // r(198) before any variable is converted, as in Stata).
  if (cmd == "destring" || cmd == "tostring") {
    const bool isDestring = cmd == "destring";
    const std::unordered_set<std::string>& knownOpts =
        isDestring ? kVocabDestring.known : kVocabTostring.known;
    if (auto err = eval::rejectUnknownOptions(parsed.opts, knownOpts)) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    bool doReplace = false, force = false, useDisplayFormat = false;
    handlers::DestringOpts dOpts;
    std::optional<std::string> genVal, ignoreChars, fmtOpt;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "replace" && v.isBool && v.boolValue) doReplace = true;
      if ((k == "gen" || k == "generate") && !v.isBool) genVal = v.strValue;
      if (k == "ignore" && !v.isBool) ignoreChars = v.strValue;
      if (k == "force" && v.isBool && v.boolValue) force = true;
      if (k == "float" && v.isBool && v.boolValue) dOpts.asFloat = true;
      if (k == "dpcomma" && v.isBool && v.boolValue) dOpts.dpcomma = true;
      if (k == "percent" && v.isBool && v.boolValue) dOpts.percent = true;
      if (k == "usedisplayformat" && v.isBool && v.boolValue) useDisplayFormat = true;
      if (k == "format" && !v.isBool) fmtOpt = strutil::trim(v.strValue);
    }
    // Stata's user-error messages and return codes for the option checks.
    if (genVal.has_value() && doReplace) {
      blocks.push_back(ojson(eval::engineError(
          cmd + ": options generate and replace are mutually exclusive", "syntax_error")));
      return blocks;
    }
    if (!genVal.has_value() && !doReplace) {
      blocks.push_back(ojson(eval::engineError(
          cmd + ": must specify either generate or replace option", "syntax_error")));
      return blocks;
    }
    if (!isDestring && fmtOpt.has_value()) {
      if (useDisplayFormat) {
        blocks.push_back(ojson(eval::engineError(
            "tostring: must choose between usedisplayformat and format() options",
            "syntax_error")));
        return blocks;
      }
      if (fmtOpt->find('s') != std::string::npos) {
        blocks.push_back(
            ojson(eval::engineError("tostring: use numeric format in format() option",
                                    "syntax_error")));
        return blocks;
      }
      // BUG-919: the ado's test is `capture di FMT 12345.67890`, so a format
      // `display` cannot use is r(198) before any variable is touched
      // (w2z_tostring tz_fmt_invalid: `format(%5.2z)` is 198, vb stays byte).
      // Only the leading `%` was checked. The families display renders a
      // number with: %[-][0]W(.|,)D(e|f|g)[c], %Wx, %WH/%WL, and the date
      // families %t... / %d... (their details are left to the renderer).
      static const std::regex reTostringFmt(
          R"(^%-?0?\d*[.,]\d+[efg]c?$|^%-?\d*x$|^%\d*[HL]$|^%-?\d*t[A-Za-z].*$|^%-?d.*$)");
      if (fmtOpt->empty() || !std::regex_match(*fmtOpt, reTostringFmt)) {
        blocks.push_back(
            ojson(eval::engineError("tostring: format() option invalid", "syntax_error")));
        return blocks;
      }
    }
    // parseOpts() captures a paren-value VERBATIM (it only tracks paren
    // depth, not Stata's own string-quoting) — real Stata's ignore() syntax
    // is always double-quoted (`ignore(",")`), so the raw strValue here is
    // literally `","` including the quote characters; strip them (same
    // convention the merge/joinby `using "FILE"` arms already use above) so
    // the character SET destring strips from each value is just `,`, not
    // `,` and `"`.
    if (ignoreChars.has_value() && ignoreChars->size() >= 2 && ignoreChars->front() == '"' &&
        ignoreChars->back() == '"') {
      ignoreChars = ignoreChars->substr(1, ignoreChars->size() - 2);
    }
    // destring's varlist is optional (`syntax [varlist]` = every variable);
    // tostring's is required.
    std::vector<std::string> vars;
    if (parsed.varlist.empty()) {
      if (!isDestring) {
        blocks.push_back(ojson(eval::engineError("tostring: varlist required", "syntax_error")));
        return blocks;
      }
      auto vr = piet::loops::expandVarlist("_all", session);
      if (vr.isError) {
        blocks.push_back(vr.error);
        return blocks;
      }
      vars = vr.tokens;
    } else if (parsed.varlist.size() == 1 &&
               parsed.varlist[0].find_first_of("*?-~") == std::string::npos) {
      // A single plain name goes straight to the handler exactly as before
      // (its own not-found message, and no name-list read that could
      // materialize a dirty resident head -- the C4-L4 chain gates).
      vars = parsed.varlist;
    } else {
      auto vr = piet::loops::expandVarlist(joinVarlist(parsed.varlist), session);
      if (vr.isError) {
        blocks.push_back(vr.error);
        return blocks;
      }
      vars = vr.tokens;
    }
    std::vector<std::string> newNames;
    if (genVal.has_value()) {
      std::istringstream gs(*genVal);
      std::string tok;
      while (gs >> tok) newNames.push_back(tok);
      if (newNames.size() != vars.size()) {
        blocks.push_back(ojson(eval::engineError(
            cmd + ": number of variables in varlist must equal number of variables in "
                  "generate(newvarlist)",
            "syntax_error")));
        return blocks;
      }
      // `syntax newvarlist` validates every name before any variable is
      // converted; with one name the handler's own checks are that test.
      if (newNames.size() > 1) {
        std::unordered_set<std::string> seen;
        Datastore* gds = session.datastore();
        for (const auto& nm : newNames) {
          // BUG-1120: the handlers' own reserved-name refusal (runDestring /
          // runTostring), for every name before any variable is converted.
          if (strutil::isReservedVarName(nm) || nm == "_skip" || nm == "_all") {
            blocks.push_back(ojson(eval::errorBlock(
                "syntax_error",
                isDestring ? "generate() contains existing variable(s) and/or illegal variable "
                             "name(s)"
                           : "generate(newvarlist) invalid",
                std::nullopt, std::nullopt, std::nullopt,
                strutil::isReservedVarName(nm) ? "r(198)" : "r(101)")));
            return blocks;
          }
          if (!seen.insert(nm).second || (gds != nullptr && gds->has(nm))) {
            blocks.push_back(ojson(
                eval::engineError(cmd + ": variable '" + nm + "' already defined")));
            return blocks;
          }
          if (!strutil::isValidStataIdentifier(nm)) {
            blocks.push_back(ojson(eval::engineError(
                cmd + ": invalid variable name `" + nm + "' — " +
                    strutil::stataIdentifierRuleText(),
                "syntax_error")));
            return blocks;
          }
        }
      }
    }
    handlers::TostringOpts tOpts;
    tOpts.stataGate = true;
    tOpts.format = fmtOpt;
    tOpts.force = force;
    for (size_t i = 0; i < vars.size(); ++i) {
      std::optional<std::string> target;
      if (genVal.has_value()) target = newNames[i];
      // BUG-1160 (FU-24, FU-32): tostring prints each conversion, "x was
      // double now str3" under replace and "y generated as str3" under
      // generate() (StataBE 19.5, measured 2026-11-03; w2b_dataio.log:77 `big
      // was double now str11`). The old type is the one `: type` answers
      // before the conversion: the recorded claim, else float.
      std::string fromType = "float";
      if (!isDestring) {
        auto mit = session.varMeta.find(vars[i]);
        if (mit != session.varMeta.end() && mit->second.stataType.has_value())
          fromType = *mit->second.stataType;
      }
      // Under `force` a conversion some value does not survive adds "VAR was
      // forced to string; some loss of information", VAR the variable it
      // wrote (StataBE: `gen y = _n + 0.1` / `tostring y, replace force` gives
      // "y was float now str11" and then that line; gen(ys) names ys).
      bool forcedLoss = false;
      ojson blk = isDestring ? ojson(handlers::runDestring(vars[i], doReplace, target,
                                                           ignoreChars, force, session, dOpts))
                             : ojson(handlers::runTostring(vars[i], doReplace, target,
                                                           useDisplayFormat, session, tOpts,
                                                           &forcedLoss));
      const std::string blkType = blk.is_object() ? blk.value("type", std::string()) : "";
      blocks.push_back(std::move(blk));
      if (blkType == "error") return blocks;
      if (!isDestring && blkType == "data_mutation") {
        const std::string out = target.value_or(vars[i]);
        auto mit = session.varMeta.find(out);
        auto note = [&blocks](const std::string& text) {
          ojson b;
          b["type"] = "display";
          b["text"] = text;
          blocks.push_back(std::move(b));
        };
        if (mit != session.varMeta.end() && mit->second.stataType.has_value())
          note(doReplace ? vars[i] + " was " + fromType + " now " + *mit->second.stataType
                         : out + " generated as " + *mit->second.stataType);
        if (forcedLoss) note(out + " was forced to string; some loss of information");
      }
    }
    return blocks;
  }

  if (cmd == "reshape") {
    if (preflight::gate("reshape", session, "", 0, blocks) == preflight::Verdict::Cancel) return blocks;
    ojson reshaped = ojson(handlers::runReshape(parsed.varlist, parsed.opts, session));
    // e(sample) T2 (SPEC §4.3): reshape rebuilds the observations, so the
    // estimation-sample mask cannot follow them -- Stata's marker is gone after
    // it (w2_esample.log:1174 E9 `reshape wide` then `count if e(sample)` 0).
    // The engine carried the hidden column like any other (a wide reshape took
    // one value per i: 3 rows counted), so it is dropped from the new frame:
    // the model's e(sample) then reads 0 (column absent), never rows it was
    // not fitted on. No block: the column was never in the user's varlist.
    if (reshaped.is_object() && reshaped.value("type", std::string()) == "data_mutation" &&
        session.esampleColumnInstalls > 0) {
      try {
        Datastore* rds = session.datastore();
        if (rds != nullptr && rds->has(kEsampleColumn)) {
          const std::string out = mintScratchPath(session.workdir(), "reshape_esample_drop");
          runCobraJson({"eval", "--data", *session.currentHbin(), "--drop", kEsampleColumn,
                        "--out", out});
          session.load(out);
        }
      } catch (const std::exception& e) {
        blocks.push_back(reshaped);
        blocks.push_back(ojson(eval::engineError(
            std::string("reshape: the estimation-sample marker could not be removed from the "
                        "reshaped data (") + e.what() + ")")));
        return blocks;
      }
    }
    blocks.push_back(reshaped);
    return blocks;
  }

  if (cmd == "append") {
    // BS-097: this arm never reads parsed.opts at all today (only parsed.raw
    // via regex + parsed.varlist) — real Stata `append` options (nolabel,
    // generate()) are still rejected rather than silently dropped.
    // W:P5: `force` is accepted. In Stata it permits appending when a
    // variable's type differs between master and using (string vs numeric),
    // filling the using side's cells with missing of the master's type.
    // BUG-550: this was a no-op over the engine's promote-both-to-f64 default,
    // which is NOT what force does (string codes became numbers, and without
    // force the clash was rc 0 instead of r(106)). runAppend now passes
    // `--type-clash error|force` (w2_data_io.log app15/app16/app17).
    // BUG-233: the one copy of this vocabulary now lives in the file-scope
    // registration block (see top of file); this binding keeps every existing
    // use — including the reject net below — reading the same set.
    const std::unordered_set<std::string>& kAppendKnownOpts = kVocabAppend.known;
    // append.ado runs `capture quietly describe using FILE` (r-class) on every
    // using file before `_append` parses the rest of the options, so a call
    // that gets past `using` replaces r() even when it then fails (aer_w2
    // w2b_dataio append_ge(src): `ge(src)` r(198), then r(sum) reads `.`).
    {
      static const std::regex reAppendUsing(R"(\busing\s+\S)", std::regex::icase);
      if (std::regex_search(parsed.raw, reAppendUsing)) {
        eval::replaceRClass(session.eval);
        eval::declareRPosted(session.eval, "append", {});
      }
    }
    // append.ado's syntax line declares `GENerate(name)`: gen ... generate all
    // name it (w2aw_data_combine_family.log dc_a_gen `gen(src)` rc 0). Only that
    // declared minimum is honoured; the rest of the net stays exact.
    std::vector<std::pair<std::string, eval::OptValue>> appendOpts = parsed.opts;
    for (auto& kv : appendOpts) {
      if (kv.first.size() >= 3 && kv.first.size() < 8 &&
          std::string("generate").compare(0, kv.first.size(), kv.first) == 0)
        kv.first = "generate";
    }
    if (auto err = eval::rejectUnknownOptions(appendOpts, kAppendKnownOpts)) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    // B5-D10: `generate(NAME)` -- see kVocabAppend's registration comment.
    std::optional<std::string> appendGenVar, appendKeep;
    bool appendForce = false, appendNolabel = false, appendNonotes = false;
    for (const auto& [k, v] : appendOpts) {
      if (k == "generate" && !v.isBool) appendGenVar = v.strValue;
      if (k == "keep") appendKeep = v.isBool ? std::string() : v.strValue;  // BUG-549
      if (k == "force") appendForce = true;  // BUG-550
      if (k == "nolabel") appendNolabel = true;  // BUG-551
      if (k == "nonotes") appendNonotes = true;
    }
    static const std::regex reUsing(R"(\busing\s+(.+?)(?:\s*,|$))", std::regex::icase);
    std::smatch m;
    std::string filepath;
    if (std::regex_search(parsed.raw, m, reUsing)) {
      filepath = trimWs(m.str(1));
    } else if (parsed.varlist.size() >= 2 && lower(parsed.varlist[0]) == "using") {
      filepath = parsed.varlist[1];
    } else {
      blocks.push_back(ojson(eval::engineError(
          "append: `using` filename required — syntax: append using <file>")));
      return blocks;
    }
    // Several files (append: `using filenames`), quote-aware.
    std::vector<std::string> appendFiles;
    {
      size_t i = 0;
      while (i < filepath.size()) {
        while (i < filepath.size() && std::isspace(static_cast<unsigned char>(filepath[i]))) ++i;
        if (i >= filepath.size()) break;
        if (filepath[i] == '"') {
          const size_t close = filepath.find('"', i + 1);
          const size_t end = close == std::string::npos ? filepath.size() : close;
          appendFiles.push_back(filepath.substr(i + 1, end - i - 1));
          i = end + 1;
        } else if (filepath.compare(i, 2, "`\"") == 0) {
          const size_t close = filepath.find("\"'", i + 2);
          const size_t end = close == std::string::npos ? filepath.size() : close;
          appendFiles.push_back(filepath.substr(i + 2, end - i - 2));
          i = end + 2;
        } else {
          size_t j = i;
          while (j < filepath.size() && !std::isspace(static_cast<unsigned char>(filepath[j]))) ++j;
          appendFiles.push_back(filepath.substr(i, j - i));
          i = j;
        }
      }
    }
    for (const auto& f : appendFiles)
      if (preflight::gate("append", session, f, 0, blocks) == preflight::Verdict::Cancel) return blocks;
    std::vector<std::string> typeNotes;
    ojson appended = handlers::runAppendFiles(appendFiles, session, appendGenVar, appendKeep,
                                              appendForce, appendNolabel, appendNonotes,
                                              &typeNotes);
    // BUG-1092: append's widening lines, command output printed as it appends.
    for (const std::string& text : typeNotes) {
      ojson b;
      b["type"] = "display";
      b["text"] = text;
      blocks.push_back(std::move(b));
    }
    blocks.push_back(std::move(appended));
    return blocks;
  }

  if (cmd == "merge") {
    // merge: the first token is the match type; a
    // token without a colon is the old syntax.
    static const std::regex reMergeHead(R"(^\s*merge\s+([^\s,]+)\s*(.*)$)", std::regex::icase);
    std::smatch m;
    if (!std::regex_match(parsed.raw, m, reMergeHead)) {
      blocks.push_back(ojson(eval::engineError(
          "merge: syntax error. Usage: merge 1:1|m:1|1:m|m:m KEYLIST using FILE [, opts]")));
      return blocks;
    }
    const std::string mtypeRaw = m.str(1);
    std::string rest = m.str(2);
    if (mtypeRaw.find(':') == std::string::npos) {
      // The old merge syntax: `merge [varlist] using file [, update replace
      // keep(varlist) _merge(name) sort unique uniqmaster uniqusing nolabel
      // nosummary]`. keep() names USING variables (merge's keepusing()); both
      // datasets must already be sorted by the varlist unless `sort` (r(5));
      // duplicates pair as m:m unless a uniq option asks otherwise.
      const long long comma = eval::findUnquotedComma(parsed.raw);
      std::string head = comma >= 0 ? parsed.raw.substr(0, static_cast<size_t>(comma)) : parsed.raw;
      static const std::regex reOldHead(R"(^\s*merge\s+(.*?)\busing\b\s*(.*?)\s*$)",
                                        std::regex::icase);
      std::smatch om;
      if (!std::regex_match(head, om, reOldHead)) {
        blocks.push_back(ojson(eval::errorBlock("syntax_error", "using required", std::nullopt,
                                                std::nullopt, std::nullopt, std::string("r(100)"))));
        return blocks;
      }
      std::vector<std::string> oldKeys;
      {
        std::istringstream iss(om.str(1));
        for (std::string t; iss >> t;) oldKeys.push_back(t);
      }
      std::string oldFile = trimWs(om.str(2));
      if (oldFile.size() >= 2 && ((oldFile.front() == '"' && oldFile.back() == '"') ||
                                  (oldFile.front() == '\'' && oldFile.back() == '\''))) {
        oldFile = oldFile.substr(1, oldFile.size() - 2);
      }
      if (oldKeys.empty() || oldFile.empty()) {
        blocks.push_back(ojson(eval::engineError(
            "merge: the old syntax without match variables is not supported by this build")));
        return blocks;
      }
      std::vector<std::pair<std::string, eval::OptValue>> oldOpts =
          comma >= 0 ? eval::parseOpts(parsed.raw.substr(static_cast<size_t>(comma) + 1))
                     : std::vector<std::pair<std::string, eval::OptValue>>{};
      std::vector<std::pair<std::string, eval::OptValue>> newOpts;
      std::string oldType = "m:m";
      bool oldSort = false;
      static const std::unordered_set<std::string> kOldKnown = {
          "update", "replace", "keep",       "_merge",    "sort",    "unique",
          "uniqmaster", "uniqusing", "nolabel", "nosummary", "nokeep", "nonotes"};
      eval::canonicalizeOptionKeys(oldOpts, kOldKnown);
      for (const auto& [k, v] : oldOpts) {
        if (k == "keep" && !v.isBool) newOpts.emplace_back("keepusing", v);
        else if (k == "_merge" && !v.isBool) newOpts.emplace_back("generate", v);
        else if (k == "update" || k == "replace") newOpts.emplace_back(k, v);
        else if (k == "sort") oldSort = true;
        else if (k == "unique") oldType = "1:1";
        else if (k == "uniqmaster") oldType = oldType == "m:1" ? "1:1" : "1:m";
        else if (k == "uniqusing") oldType = oldType == "1:m" ? "1:1" : "m:1";
        else if (k == "nolabel" || k == "nosummary" || k == "nokeep" || k == "nonotes") continue;
        else {
          blocks.push_back(ojson(eval::errorBlock("syntax_error", "option " + k + " not allowed",
                                                  std::nullopt, std::nullopt, std::nullopt,
                                                  std::string("r(198)"))));
          return blocks;
        }
      }
      if (oldSort && oldType == "m:m") oldType = "1:1";  // merge_10: sort implies unique
      if (preflight::gate("merge", session, oldFile, 0, blocks) == preflight::Verdict::Cancel)
        return blocks;
      blocks.push_back(ojson(handlers::runMerge(oldType, oldKeys, oldFile, newOpts, session,
                                                /*sequential=*/false, /*oldSyntax=*/true,
                                                oldSort)));
      return blocks;
    }
    // merge type: n spells m; anything else is r(198).
    std::string mergeType;
    {
      static const std::map<std::string, std::string> kMtype = {
          {"1:1", "1:1"}, {"1:m", "1:m"}, {"m:1", "m:1"}, {"m:m", "m:m"}, {"1:n", "1:m"},
          {"n:1", "m:1"}, {"n:n", "m:m"}, {"n:m", "m:m"}, {"m:n", "m:m"}};
      auto it = kMtype.find(mtypeRaw);
      if (it == kMtype.end()) {
        blocks.push_back(ojson(eval::errorBlock(
            "syntax_error",
            "merge " + mtypeRaw + ":  invalid merge type\n    merge types are 1:1, 1:m, m:1, or m:m",
            std::nullopt, std::nullopt, std::nullopt, std::string("r(198)"))));
        return blocks;
      }
      mergeType = it->second;
    }
    // `merge 1:1 _n` is the sequential merge (merge).
    bool mergeSequential = false;
    {
      static const std::regex reSeq(R"(^_n(?=[\s,]|$)\s*)");
      std::smatch sm;
      if (std::regex_search(rest, sm, reSeq)) {
        if (mergeType != "1:1") {
          blocks.push_back(ojson(eval::errorBlock(
              "syntax_error",
              mtypeRaw + " _n invalid\n    the syntax for a sequential merge is merge 1:1 _n ...",
              std::nullopt, std::nullopt, std::nullopt, std::string("r(198)"))));
          return blocks;
        }
        mergeSequential = true;
        rest = rest.substr(static_cast<size_t>(sm.length(0)));
      }
    }
    // strip trailing options (`, opt...`) — split at the first comma
    // followed by a word char.
    static const std::regex reOptSplit(R"(,\s*(?=\w))");
    std::string restNoOpts;
    {
      std::smatch om;
      if (std::regex_search(rest, om, reOptSplit)) {
        restNoOpts = trimWs(rest.substr(0, static_cast<size_t>(om.position(0))));
      } else {
        restNoOpts = trimWs(rest);
      }
    }
    static const std::regex reUsingSplit(R"(\busing\b)", std::regex::icase);
    std::smatch usingM;
    if (!std::regex_search(restNoOpts, usingM, reUsingSplit)) {
      blocks.push_back(ojson(eval::engineError(
          "merge: missing `using FILE` clause. Usage: merge 1:1 id using \"file.hbin\"")));
      return blocks;
    }
    std::string keyPart = trimWs(restNoOpts.substr(0, static_cast<size_t>(usingM.position(0))));
    std::string filePart =
        trimWs(restNoOpts.substr(static_cast<size_t>(usingM.position(0) + usingM.length(0))));
    if (filePart.size() >= 2 && ((filePart.front() == '"' && filePart.back() == '"') ||
                                 (filePart.front() == '\'' && filePart.back() == '\''))) {
      filePart = filePart.substr(1, filePart.size() - 2);
    }
    std::vector<std::string> keylist;
    {
      std::istringstream iss(keyPart);
      std::string tok;
      while (iss >> tok) keylist.push_back(tok);
    }
    if (mergeSequential && !keylist.empty()) {
      blocks.push_back(ojson(eval::errorBlock(
          "syntax_error", "varlist cannot be specified after 1:1 _n", std::nullopt, std::nullopt,
          std::nullopt, std::string("r(198)"))));
      return blocks;
    }
    if (keylist.empty() && !mergeSequential) {
      blocks.push_back(ojson(eval::errorBlock(
          "syntax_error", "varlist must be specified after merge " + mtypeRaw, std::nullopt,
          std::nullopt, std::nullopt, std::string("r(198)"))));
      return blocks;
    }
    if (filePart.empty()) {
      blocks.push_back(ojson(eval::engineError("merge: empty filename after `using`")));
      return blocks;
    }
    if (preflight::gate("merge", session, filePart, 0, blocks) == preflight::Verdict::Cancel) return blocks;
    // e(sample) T2 / FU-496: a `1:m` merge copies a master row once per
    // matching using row. StataBE keeps the marker on ONE copy, the first in
    // merge's output order, and reads 0 on the others, so the count stays e(N)
    // (w2_esample_copies.log:596 M1 `0|9|0|9|22|matched=9|not_first_of_run=0|`;
    // a review measured 18 for e(N) 9 when the cell rode on every copy).
    // Using-only rows carry no cell and read 0 (M2 `using_only=0`, :664).
    // handlers::esampleKeepFirstCopy applies the rule; a key it cannot read
    // withdraws the marker by name.
    std::optional<size_t> esMasterRows;
    if (mergeType == "1:m") {
      if (auto cells = handlers::esampleHeadMask(session)) esMasterRows = cells->size();
    }
    // Stata's option syntax binds `keep (1 3)` like `keep(1 3)` (w2aw_data_combine_family.log
    // dc_mk_spaced, rc 0): re-read merge's option tail with a blank before `(` closed up.
    std::vector<std::pair<std::string, eval::OptValue>> mergeOpts = parsed.opts;
    {
      const long long comma = eval::findUnquotedComma(parsed.raw);
      if (comma >= 0) {
        static const std::regex reSpacedParen(R"(([A-Za-z_][A-Za-z0-9_]*)\s+\()");
        mergeOpts = eval::parseOpts(std::regex_replace(
            parsed.raw.substr(static_cast<size_t>(comma) + 1), reSpacedParen, "$1("));
      }
    }
    ojson mergeBlock = ojson(handlers::runMerge(mergeType, keylist, filePart, mergeOpts, session,
                                                mergeSequential));
    const bool mergeFailed =
        mergeBlock.is_object() && mergeBlock.value("type", std::string()) == "error";
    blocks.push_back(mergeBlock);
    if (esMasterRows.has_value() && !mergeFailed) {
      size_t masterOrigin = 0;
      if (auto after = handlers::esampleHeadMask(session)) {
        for (double v : *after) masterOrigin += std::isnan(v) ? 0 : 1;
      }
      if (masterOrigin > *esMasterRows &&
          !handlers::esampleKeepFirstCopy(session, keylist, nullptr) &&
          !handlers::esampleMarkHeadLost(session, EsampleLostBy::kMerge1m)) {
        blocks.push_back(ojson(eval::engineError(
            "merge: the datasets were merged, but the estimation-sample marker could not be "
            "withdrawn from the copied observations; e(sample) is not reliable until the next "
            "estimation.")));
      }
    }
    return blocks;
  }

  // ------------------------------------------------------------------
  // frames (P6 wave) — `frame` is BOTH a command family (`frame put/drop`)
  // and a prefix (`frame NAME: CMD`), so the sub-token decides which. See
  // handlers_frames.cpp for the design; the current frame is always `default`
  // in this build and `frame create`/`change`/`pwf` are honest rejects, not
  // silent no-ops.
  if (cmd == "frame") {
    static const std::regex reFrame(R"(^\s*frame\s+(.+)$)", std::regex::icase);
    std::smatch fm;
    if (!std::regex_match(parsed.raw, fm, reFrame)) {
      blocks.push_back(ojson(eval::engineError(
          "frame: syntax error. Usage: frame put VARLIST [if], into(NAME) | frame drop NAME | "
          "frame NAME: COMMAND")));
      return blocks;
    }
    std::string rest = trimWs(fm.str(1));
    // `frame NAME: CMD` — the colon (outside quotes) marks the prefix form.
    // Checked BEFORE the sub-command split so a frame legitimately named
    // `drop` still reaches its own prefix arm rather than being read as the
    // drop sub-command.
    {
      static const std::regex reColon(R"(:)");
      std::smatch cm;
      if (eval::searchUnquoted(rest, reColon, cm)) {
        std::string frameName = trimWs(rest.substr(0, static_cast<size_t>(cm.position(0))));
        std::string innerCmd =
            trimWs(rest.substr(static_cast<size_t>(cm.position(0) + cm.length(0))));
        // A one-token name is the prefix form; anything else (e.g.
        // `frame put x y, into(f)` has no colon at all) never reaches here.
        if (!frameName.empty() && frameName.find_first_of(" \t") == std::string::npos) {
          if (innerCmd.empty()) {
            blocks.push_back(ojson(eval::engineError(
                "frame " + frameName + ": no command found after the colon")));
            return blocks;
          }
          ojson inner = handlers::runFramePrefix(frameName, innerCmd, session);
          if (inner.is_array()) {
            for (const auto& b : inner) blocks.push_back(b);
          } else {
            blocks.push_back(inner);
          }
          return blocks;
        }
      }
    }

    std::string sub;
    std::string subRest;
    {
      size_t sp = rest.find_first_of(" \t");
      if (sp == std::string::npos) {
        sub = lower(rest);
      } else {
        sub = lower(rest.substr(0, sp));
        subRest = trimWs(rest.substr(sp));
      }
    }

    if (sub == "create" || sub == "change" || sub == "cwf" || sub == "pwf" || sub == "rename" ||
        sub == "copy" || sub == "dir") {
      blocks.push_back(ojson(eval::unsupportedCommand(
          "frame " + sub, std::nullopt, std::nullopt, "piet_gap",
          eval::UnsupportedCommandKind::kNotImplemented)));
      return blocks;
    }

    if (sub == "drop") {
      if (subRest.empty()) {
        blocks.push_back(ojson(eval::engineError("frame drop: frame name required")));
        return blocks;
      }
      ojson r = handlers::runFrameDrop(subRest, session);
      if (!(r.is_array() && r.empty())) blocks.push_back(r);
      return blocks;
    }

    if (sub == "put") {
      // `frame put VARLIST [if COND], into(NAME)`
      auto [head, optsPart] = splitTopLevelComma(subRest);
      std::string varsPart = trimWs(head);
      std::optional<std::string> ifExpr;
      {
        static const std::regex reIf(R"(\s+if\s+(.+)$)", std::regex::icase);
        std::smatch im;
        if (eval::searchUnquoted(varsPart, reIf, im)) {
          ifExpr = trimWs(im.str(1));
          varsPart = trimWs(varsPart.substr(0, static_cast<size_t>(im.position(0))));
        }
      }
      std::vector<std::string> varlist;
      {
        std::istringstream iss(varsPart);
        std::string tok;
        while (iss >> tok) varlist.push_back(tok);
      }
      std::string into;
      for (const auto& [k, v] : parsed.opts) {
        if (lower(k) == "into") into = v.strValue;
      }
      // BUG-233: the one copy of this vocabulary now lives in the file-scope
      // registration block (see top of file); this binding keeps every existing
      // use — including the reject net below — reading the same set.
      const std::unordered_set<std::string>& kFramePutKnown = kVocabFramePut.known;
      if (auto err = eval::rejectUnknownOptions(parsed.opts, kFramePutKnown)) {
        blocks.push_back(ojson(*err));
        return blocks;
      }
      (void)optsPart;
      ojson r = handlers::runFramePut(varlist, ifExpr, into, session);
      if (!(r.is_array() && r.empty())) blocks.push_back(r);
      return blocks;
    }

    blocks.push_back(ojson(eval::engineError(
        "frame " + sub +
        ": unrecognized subcommand. This build supports `frame put`, `frame drop` and the "
        "`frame NAME: COMMAND` prefix.")));
    return blocks;
  }

  // `frlink m:1|1:1 KEY [KEY2 ...], frame(F) generate(G)`
  if (cmd == "frlink") {
    static const std::regex reFrlink(R"(^\s*frlink\s+(1:1|m:1)\s+(.+)$)", std::regex::icase);
    std::smatch m;
    if (!std::regex_match(parsed.raw, m, reFrlink)) {
      blocks.push_back(ojson(eval::engineError(
          "frlink: syntax error. Usage: frlink m:1|1:1 KEY [KEY2 ...], frame(FRAMENAME) "
          "generate(NEWVAR)")));
      return blocks;
    }
    std::string linkType = lower(trimWs(m.str(1)));
    auto [keyHead, keyOpts] = splitTopLevelComma(trimWs(m.str(2)));
    (void)keyOpts;
    std::vector<std::string> keylist;
    {
      std::istringstream iss(trimWs(keyHead));
      std::string tok;
      while (iss >> tok) keylist.push_back(tok);
    }
    std::string frameName, genName;
    for (const auto& [k, v] : parsed.opts) {
      const std::string kl = lower(k);
      if (kl == "frame") frameName = v.strValue;
      if (kl == "generate" || kl == "gen") genName = v.strValue;
    }
    // BUG-233: the one copy of this vocabulary now lives in the file-scope
    // registration block (see top of file); this binding keeps every existing
    // use — including the reject net below — reading the same set.
    const std::unordered_set<std::string>& kFrlinkKnown = kVocabFrlink.known;
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kFrlinkKnown)) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    blocks.push_back(ojson(handlers::runFrlink(linkType, keylist, frameName, genName, session)));
    return blocks;
  }

  // `frget NEW=VAR [NEW2=VAR2 ...], from(LINKVAR)`
  if (cmd == "frget") {
    static const std::regex reFrget(R"(^\s*frget\s+(.+)$)", std::regex::icase);
    std::smatch m;
    if (!std::regex_match(parsed.raw, m, reFrget)) {
      blocks.push_back(ojson(eval::engineError(
          "frget: syntax error. Usage: frget NEWVAR=VAR [...], from(LINKVAR)")));
      return blocks;
    }
    auto [head, optsPart] = splitTopLevelComma(trimWs(m.str(1)));
    (void)optsPart;
    std::vector<std::pair<std::string, std::string>> renames;
    {
      std::istringstream iss(trimWs(head));
      std::string tok;
      while (iss >> tok) {
        size_t eq = tok.find('=');
        if (eq == std::string::npos) {
          // Bare `frget VAR` copies VAR under its own name.
          renames.emplace_back(tok, tok);
        } else {
          renames.emplace_back(trimWs(tok.substr(0, eq)), trimWs(tok.substr(eq + 1)));
        }
      }
    }
    std::string fromLink;
    for (const auto& [k, v] : parsed.opts) {
      if (lower(k) == "from") fromLink = v.strValue;
    }
    // BUG-233: the one copy of this vocabulary now lives in the file-scope
    // registration block (see top of file); this binding keeps every existing
    // use — including the reject net below — reading the same set.
    const std::unordered_set<std::string>& kFrgetKnown = kVocabFrget.known;
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kFrgetKnown)) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    blocks.push_back(ojson(handlers::runFrget(renames, fromLink, session)));
    return blocks;
  }

  // BS-004/D5 (BETA B-data): `joinby key1 [key2 ...] using FILE` — bare form
  // (audit-confirmed syntax: `joinby [varlist] using filename [,
  // update replace unmatched(none|both|master|using) _merge(varname)
  // nolabel]`; analysis.do:451's own call uses zero options, the
  // unmatched(none) default). Mirrors the `merge` arm immediately above
  // (same raw-text `using`-split shape), minus merge's type-prefix capture
  // group (joinby has none).
  //
  // item3 (this lane, BUG-477 brief): `unmatched(none)` graduated to a real
  // acceptance below — it is provably the SAME operation as the bare form
  // (Stata's own default), not a new engine capability. `update`/`replace`/
  // `_merge()`/`nolabel`/`unmatched(master|both)` are still an honest reject
  // (D5: -> post-beta WP-18 for the first four; unmatched(master|both) needs
  // engine work this lane's brief scoped out of, see the session report) —
  // enforced by the known-options net below PLUS a value-level check for
  // `unmatched()` specifically, not a per-option allow-list.
  if (cmd == "joinby") {
    static const std::regex reJoinby(R"(^\s*joinby\s+(.+)$)", std::regex::icase);
    std::smatch m;
    if (!std::regex_match(parsed.raw, m, reJoinby)) {
      blocks.push_back(ojson(eval::engineError(
          "joinby: syntax error. Usage: joinby key1 [key2 ...] using \"file\"")));
      return blocks;
    }
    std::string rest = m.str(1);
    // strip trailing options (`, opt...`) — split at the first comma
    // followed by a word char (same grammar the merge arm above uses).
    static const std::regex reOptSplitJoinby(R"(,\s*(?=\w))");
    std::string restNoOpts;
    {
      std::smatch om;
      if (std::regex_search(rest, om, reOptSplitJoinby)) {
        restNoOpts = trimWs(rest.substr(0, static_cast<size_t>(om.position(0))));
      } else {
        restNoOpts = trimWs(rest);
      }
    }
    static const std::regex reUsingSplitJoinby(R"(\busing\b)", std::regex::icase);
    std::smatch usingM;
    if (!std::regex_search(restNoOpts, usingM, reUsingSplitJoinby)) {
      blocks.push_back(ojson(eval::engineError(
          "joinby: missing `using FILE` clause. Usage: joinby key1 [key2 ...] using \"file\"")));
      return blocks;
    }
    std::string keyPart = trimWs(restNoOpts.substr(0, static_cast<size_t>(usingM.position(0))));
    std::string filePart =
        trimWs(restNoOpts.substr(static_cast<size_t>(usingM.position(0) + usingM.length(0))));
    if (filePart.size() >= 2 && ((filePart.front() == '"' && filePart.back() == '"') ||
                                 (filePart.front() == '\'' && filePart.back() == '\''))) {
      filePart = filePart.substr(1, filePart.size() - 2);
    }
    std::vector<std::string> keylist;
    {
      std::istringstream iss(keyPart);
      std::string tok;
      while (iss >> tok) keylist.push_back(tok);
    }
    if (keylist.empty()) {
      blocks.push_back(
          ojson(eval::engineError("joinby: no key variables specified before `using`")));
      return blocks;
    }
    if (filePart.empty()) {
      blocks.push_back(ojson(eval::engineError("joinby: empty filename after `using`")));
      return blocks;
    }
    // joinby.ado's unmatched(none|master|using|both) and _merge(name): the
    // engine keeps the unmatched rows and writes the 1/2/3 marker (cobra joinby
    // --unmatched/--merge-var; w2aw_data_combine_family.log dc_j_*).
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabJoinby.known,
                                              kVocabJoinby.unimplemented)) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    // joinby.ado UnMatch: unmatched() lowercased and prefix-matched
    // to both / master / using / none; anything else r(198) (dc_j_xyz).
    std::string joinUnmatched = "none";
    std::optional<std::string> joinMergeVar;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "_merge" && !v.isBool) joinMergeVar = strutil::trim(v.strValue);
      if (k != "unmatched") continue;
      const std::string val = v.isBool ? std::string() : strutil::lower(strutil::trim(v.strValue));
      if (val.empty()) continue;
      bool found = false;
      for (const char* word : {"both", "master", "using", "none"}) {
        const std::string w(word);
        if (val.size() <= w.size() && w.compare(0, val.size(), val) == 0) {
          joinUnmatched = w;
          found = true;
          break;
        }
      }
      if (!found) {
        blocks.push_back(ojson(eval::errorBlock(
            "syntax_error", strutil::trim(v.strValue) + " is invalid value for unmatch()",
            std::nullopt, std::nullopt, std::nullopt, std::string("r(198)"))));
        return blocks;
      }
    }
    if (preflight::gate("joinby", session, filePart, 0, blocks) == preflight::Verdict::Cancel) return blocks;
    blocks.push_back(
        ojson(handlers::runJoinby(keylist, filePart, session, joinUnmatched, joinMergeVar)));
    return blocks;
  }

  if (cmd == "duplicates") {
    std::string sub = parsed.varlist.empty() ? std::string() : lower(parsed.varlist[0]);
    std::optional<std::vector<std::string>> subVarlist;
    if (parsed.varlist.size() > 1) {
      subVarlist = std::vector<std::string>(parsed.varlist.begin() + 1, parsed.varlist.end());
    }
    std::optional<std::string> genvar;
    for (const auto& [k, v] : parsed.opts) {
      if ((k == "gen" || k == "generate") && !v.isBool) {
        genvar = v.strValue;
        break;
      }
    }
    // BS-097: `gen`/`generate` and `force` are the options this arm reads
    // (force: the drop rules below).
    // BUG-233: the one copy of this vocabulary now lives in the file-scope
    // registration block (see top of file); this binding keeps every existing
    // use — including the reject net below — reading the same set.
    // duplicates' subcommand abbreviations (report, list and tag to one
    // letter; drop only in full) and its `force' rules: `drop varlist' needs
    // force (r(198)), `drop' over all variables refuses it (r(101)), tag's
    // syntax has no force (r(198)). w2ah_data_verbs.log ah_dup_drop_noforce rc
    // 198 with _N 8, ah_dup_drop_all_force rc 101 with _N 8; main dropped rows
    // at rc 0 in both.
    {
      const size_t l = sub.size();
      if (l > 0 && std::string("report").compare(0, l, sub) == 0) sub = "report";
      else if (l > 0 && std::string("examples").compare(0, l, sub) == 0) sub = "examples";
      else if (l > 0 && std::string("list").compare(0, l, sub) == 0) sub = "list";
      else if (l > 0 && std::string("tag").compare(0, l, sub) == 0) sub = "tag";
    }
    const std::unordered_set<std::string>& kDuplicatesKnownOpts = kVocabDuplicates.known;
    const bool listOptions = sub == "examples" || sub == "list";  // BUG-1096, see the vocabulary
    auto dupRefuse = [&](const std::string& msg, int rc) {
      blocks.push_back(ojson(eval::errorBlock("syntax_error", msg, std::nullopt, std::nullopt,
                                              std::nullopt,
                                              "r(" + std::to_string(rc) + ")")));
      return blocks;
    };
    // BUG-1096: examples and list pass every option to `list`, which knows neither
    // generate() nor force and refuses the first option it does not know, as
    // typed: `duplicates list x, g(z)` is "option g() not allowed", `, force g(z)`
    // "option force not allowed", `, nol g(z)` the g() (StataBE 19.5, r(198)).
    // report takes and ignores both. The options before the refused one still go
    // through the net, so an earlier unimplemented one is named first.
    std::optional<size_t> notForList;
    if (listOptions)
      for (size_t i = 0; i < parsed.opts.size() && !notForList; ++i) {
        const std::string& k = parsed.opts[i].first;
        if (k == "gen" || k == "generate" || k == "force") notForList = i;
      }
    if (auto err = eval::rejectUnknownOptions(
            notForList ? std::vector<std::pair<std::string, eval::OptValue>>(
                             parsed.opts.begin(),
                             parsed.opts.begin() + static_cast<std::ptrdiff_t>(*notForList))
                       : parsed.opts,
            kDuplicatesKnownOpts,
            listOptions ? kVocabDuplicates.unimplemented : std::unordered_set<std::string>{})) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    if (notForList) {
      // parsed.opts holds the vocabulary's spelling (`g` reads `generate`), in the
      // typed order; the message names the option as it was typed.
      std::string typed = parsed.opts[*notForList].first;
      try {
        const auto asTyped = eval::tokenize(parsed.raw).opts;
        if (asTyped.size() == parsed.opts.size()) typed = asTyped[*notForList].first;
      } catch (const eval::TokenizerError&) {
      }
      return dupRefuse("option " + typed + (parsed.opts[*notForList].second.isBool ? "" : "()") +
                           " not allowed",
                       198);
    }
    bool dupForce = false;
    for (const auto& [k, v] : parsed.opts)
      if (k == "force" && (!v.isBool || v.boolValue)) dupForce = true;
    if (sub == "drop" && subVarlist.has_value() && !dupForce)
      return dupRefuse("force option required with duplicates drop varlist", 198);
    if (sub == "drop" && !subVarlist.has_value() && dupForce)
      return dupRefuse("options not allowed", 101);
    if (sub == "tag" && dupForce) return dupRefuse("option force not allowed", 198);
    blocks.push_back(ojson(handlers::runDuplicates(sub, subVarlist, genvar, session, parsed.ifExpr,
                                                   parsed.inRange)));
    return blocks;
  }

  if (cmd == "count") {
    // BS-097: `count [if COND]` has no comma-options at all (neither branch
    // below reads parsed.opts) — known-set is the empty set.
    if (auto err = eval::rejectUnknownOptions(parsed.opts, {})) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    // A string condition is counted row-wise inside runCount (BUG-032, BUG-412).
    ojson countBlock = ojson(handlers::runCount(parsed.ifExpr, session, parsed.inRange));
    if (countBlock.value("type", std::string()) != "error")
      eval::declareRPosted(session.eval, "count", {"N"});  // count's help; RULING.md D.3.3
    blocks.push_back(std::move(countBlock));
    return blocks;
  }

  if (cmd == "display") {
    // BUG-105/WS-K: cmdTyped, NOT cmd. This finds the command token inside the
    // raw line; with the canonical spelling, `dis 2+2` would search for
    // "display" in "dis 2+2", get npos, and index off the end of the string.
    // BUG-251/WS-Q: stop searching entirely — the args are simply what follows
    // the leading token. `dis(2+2)` still yields `(2+2)`: rawArgsAfterCmd ends
    // the token at the first non-identifier character, exactly as the
    // `find(cmdTyped) + size()` arithmetic did.
    std::string rawArgs = rawArgsAfterCmd(parsed.raw);
    ojson shown = ojson(handleDisplay(rawArgs, session));
    if (shown.value("type", std::string()) == "error") {
      // The pieces printed before the failing one (handleDisplay's partial_text).
      if (isDisplayLine(rawCmd)) {
        if (auto partial = displayPartialBlock(rawArgsAfterCmd(rawCmd), shown, session, true))
          blocks.push_back(*partial);
      }
      shown.erase("partial_text");
    }
    blocks.push_back(std::move(shown));
    return blocks;
  }

  // BUG-105 abbreviation stopgap: `l` (see the `describe`/`d` note above).
  // `l` joins the `list` REGISTRY ROW's abbrev list, so `l in 1/5` clears
  // the tokenizer's registry-driven `in` gate exactly as `list in 1/5` does
  // (that gate resolves the command token through registry::findByName, so
  // a dispatcher-only alias would route but then be rejected for `in`).
  if (cmd == "list" || cmd == "browse" || cmd == "br" || cmd == "l") {
    // BUG-336 (batch 2): known set measured from the handler body -- runList renders a GUI grid: noobs/clean are presentation flags whose effect the grid already
    // satisfies (output byte-identical, measured) and the p8 wave + maxperf corpus use them.
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabList.known, kVocabList.unimplemented)) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    // B5-D6: `separator`/`sep(#)` is parsed (same strict-numeric idiom as
    // summarize's separator above) rather than blindly discarded, so a
    // non-numeric argument still gets an honest reject. The value itself is
    // then dropped: runList's grid draws no every-N-rows rule (same
    // validated-no-op class as noobs/clean, see kVocabList's comment).
    // W1a-1: `abbreviate(#)` takes the same road -- the number is checked, the
    // grid has no header width to apply it to (kVocabList's comment).
    for (const auto& [k, v] : parsed.opts) {
      if (k != "separator" && k != "abbreviate") continue;
      if (v.isBool) {
        blocks.push_back(ojson(eval::engineError(
            "option `" + k + "` requires a number, e.g. " + k + (k == "separator" ? "(5)" : "(8)"),
            "unsupported_option")));
        return blocks;
      }
      try {
        size_t used = 0;
        (void)std::stol(v.strValue, &used);
        if (used != v.strValue.size()) throw std::invalid_argument("trailing");
      } catch (const std::exception&) {
        blocks.push_back(ojson(eval::engineError(
            "option `" + k + "()` incorrectly specified — invalid number", "unsupported_option")));
        return blocks;
      }
    }
    // BUG-508: a varlist naming a variable this frame does not have used to be
    // DROPPED in silence — `list a nosuchcol` rendered one column at rc 0 and
    // said nothing, which is the failure mode this campaign ranks worst: the
    // run is GREEN and the table is wrong. Stata is r(111) with the name
    // (oracle tests/eval/golden/bug508_varlist_oracle.log: L1/L2 for a bare
    // name, L5 for a wildcard that matches nothing, and rc 0 for `alpha*` and
    // `alpha-beta`, which is why this cannot be a plain has() loop).
    //
    // Shape copied from the `unab` arm below, including its lock: the resident
    // card is the cheap source but it is not always live, and "no card" must
    // NOT mean "skip the check". Wildcards and `a-b` ranges go through
    // piet::loops::expandVarlist so `x*` means here exactly what it means to
    // `foreach ... of varlist` — the expander's own header forbids a second
    // opinion. The expander does not validate a bare unknown name (it passes
    // straight through; unab's lock records the same), so that half is checked
    // against the frame here.
    if (!parsed.varlist.empty()) {
      std::vector<std::string> knownVars;
      if (const std::vector<VarInfo>* card = session.residentReadCard()) {
        for (const auto& vi : *card) knownVars.push_back(vi.name);
      } else if (Datastore* ldsp = session.datastore()) {
        knownVars = ldsp->names();
      } else {
        blocks.push_back(ojson(eval::noDataLoaded()));
        return blocks;
      }
      std::string listSpec;
      for (const auto& t : parsed.varlist) {
        if (!listSpec.empty()) listSpec += " ";
        listSpec += t;
      }
      auto lvr = piet::loops::expandVarlist(listSpec, session);
      if (lvr.isError) {
        // The measured trigger is a wildcard matching nothing, and Stata names
        // the TOKEN the user typed (`variable zz* not found`), not the pattern
        // machinery. Find the one that matched nothing rather than blaming the
        // first wildcard in the list.
        for (const auto& t : parsed.varlist) {
          if (t.empty() || t.back() != '*') continue;
          const std::string pre = t.substr(0, t.size() - 1);
          bool any = false;
          for (const auto& k : knownVars) {
            if (k.rfind(pre, 0) == 0) { any = true; break; }
          }
          if (!any) {
            blocks.push_back(ojson(eval::engineError("variable " + t + " not found")));
            return blocks;
          }
        }
        blocks.push_back(lvr.error);
        return blocks;
      }
      for (const auto& t : lvr.tokens) {
        if (std::find(knownVars.begin(), knownVars.end(), t) == knownVars.end()) {
          blocks.push_back(ojson(eval::engineError("variable " + t + " not found")));
          return blocks;
        }
      }
    }
    std::optional<std::vector<std::string>> vl;
    if (!parsed.varlist.empty()) vl = parsed.varlist;
    blocks.push_back(ojson(handlers::runList(vl, parsed.ifExpr, parsed.inRange, session)));
    return blocks;
  }

  // `format varlist %fmt` and `format %fmt varlist` ([D] format: "are the same
  // commands"; 069 writes `format %td c_dob`, 075 `format %12.0fc N`). The
  // tokenizer has no special handling for a `%`-prefixed token and the plain-
  // varlist expansion passes it through, so the %fmt is the LAST token or the
  // FIRST one. No %fmt at all is `format [varlist]`, which lists the formats.
  if (cmd == "format") {
    if (auto err = eval::rejectUnknownOptions(parsed.opts, {})) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    std::vector<std::string> fmtVarlist;
    std::string fmt;
    const std::vector<std::string>& toks = parsed.varlist;
    size_t nPct = 0;
    for (const auto& t : toks)
      if (!t.empty() && t[0] == '%') ++nPct;
    if (nPct == 0) {
      fmtVarlist = toks;
    } else if (nPct == 1 && toks.back()[0] == '%' && toks.size() > 1) {
      fmt = toks.back();
      fmtVarlist.assign(toks.begin(), toks.end() - 1);
    } else if (nPct == 1 && toks.front()[0] == '%' && toks.size() > 1) {
      fmt = toks.front();
      fmtVarlist.assign(toks.begin() + 1, toks.end());
    } else {
      blocks.push_back(ojson(eval::errorBlock(
          "syntax_error", "format: syntax is `format varlist %fmt` or `format %fmt varlist`",
          std::nullopt, std::nullopt, std::nullopt, "r(198)")));
      return blocks;
    }
    // runFormat returns an empty array as the silent-success sentinel (Stata's
    // `format` prints nothing); pushing it as a block would make downstream
    // block processing call .value("type") on an array, which throws. Guard
    // it exactly like the other silent handlers (matrix/label/log arms).
    ojson fr = handlers::runFormat(fmtVarlist, fmt, session);
    if (!(fr.is_array() && fr.empty())) blocks.push_back(fr);
    return blocks;
  }

  if (cmd == "scalar" && !parsed.varlist.empty() &&
      (lower(parsed.varlist[0]) == "list" || lower(parsed.varlist[0]) == "dir")) {
    std::vector<std::string> names(parsed.varlist.begin() + 1, parsed.varlist.end());
    blocks.push_back(ojson(handlers::runScalarList(names, session)));
    return blocks;
  }

  // `assert EXP [if] [in]` (P4 wave). The claim is the RAW text after the
  // command word, not the tokenized varlist: `assert x1 > 0 & x2 < 5` is one
  // expression, and rebuilding it from tokens would lose the user's spacing
  // inside string comparisons. parsed.raw is the same source the `if`-qualifier
  // scanner already worked from, so the two agree on where the claim ends.
  if (cmd == "assert") {
    if (auto err = eval::rejectUnknownOptions(parsed.opts, {})) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    // EM11: a claim holding a compound-quoted literal (a string with a '"' in it)
    // is a constant the engine's lexer cannot read; the expression layer can.
    // `assert `"`a1'"' != ""` is rc 0 and the == form r(9) (w2af_macro_text_file.log
    // :1366, :1377). Only a whole-raw-claim with no qualifier takes this path.
    if (!parsed.ifExpr && !parsed.inRange && parsed.raw.find("`\"") != std::string::npos) {
      std::string rawClaim = trimWs(parsed.raw);
      const size_t sp = rawClaim.find_first_of(" \t");
      rawClaim = sp == std::string::npos ? std::string() : trimWs(rawClaim.substr(sp));
      std::string raised;
      bool isStr = false;
      auto v = eval::evalArith(rawClaim, &session.eval, &raised, &isStr);
      if (v && !isStr) {
        if (*v == "0") {
          blocks.push_back(ojson(eval::errorBlock("engine_error", "assertion is false",
                                                  std::nullopt, std::nullopt, std::nullopt,
                                                  "r(9)")));
        }
        return blocks;
      }
    }
    // BUG-428/432: `assert EXPR` carries its claim in the VARLIST, not in an
    // `if`, so the substitution point above never saw it — which is how
    // `assert Q[1,1] == 1` reported an unknown VARIABLE for a defined matrix.
    // BUG-1433: the claim is genExprText's, as generate/replace's expression is
    // (BUG-904): rebuilt from the blank-split varlist, `assert s == "a  b"`
    // compared s with "a b".
    std::optional<ojson> claimErr;
    std::string claim = substituteRowExprRefs(genExprText(parsed), session, &claimErr);
    if (claimErr.has_value()) {  // BUG-456: `assert _b[nosuch]==0` r(111), `assert e(cmd)==1` r(109)
      blocks.push_back(*claimErr);
      return blocks;
    }
    claim = substituteStringCells(claim, session);  // BUG-1327 / BUG-1093 (the `if`'s: above)
    // The cells went in after substituteRowExprRefs folded the constant string
    // compares, so the ones they made (`"ab" == "ab"`) are folded here: left as
    // text they sent `assert x[1] == 1 & s[1] == "ab"` -- a subscript, which
    // only the engine reads, beside a string compare, which it cannot -- to rc
    // 1 where StataBE is rc 0.
    for (std::string* part : {&claim, parsed.ifExpr.has_value() ? &*parsed.ifExpr : nullptr}) {
      std::string mismatch;
      if (part != nullptr && !foldRowConstStringCompares(*part, &mismatch)) {
        blocks.push_back(ojson(eval::errorBlock(
            "engine_error", "type mismatch: `" + mismatch + "` compares a string with a number",
            std::nullopt, std::nullopt, std::nullopt, "r(109)")));
        return blocks;
      }
    }
    if (claim.empty()) {
      blocks.push_back(ojson(eval::engineError("assert: no expression given", "syntax_error")));
      return blocks;
    }
    ojson ar = handlers::runAssert(claim, session, parsed.ifExpr, parsed.inRange);
    // Silent success returns an empty array; pushing that would put a non-block
    // into the block list (same guard the `matrix` arm above uses).
    if (!(ar.is_array() && ar.empty())) blocks.push_back(ar);
    return blocks;
  }

  // BS-039 (BETA campaign C3): `ereturn list` — display-only e() dump
  // (handlers_m3.cpp::runEreturnList).
  // W2D-T01: every other sub-verb goes to handlers::runEreturn
  // (handlers_ereturn.cpp: clear/post/local, a named refusal for the rest).
  // `ereturn` is on kNoDataCommands because `ereturn clear` is the first line
  // of a do-file helper that runs before anything is loaded
  // (w2d_uncovered.log 211-212); `ereturn list` keeps the no-data answer it
  // had while the whole command sat behind the guard.
  if (cmd == "ereturn") {
    if (!parsed.varlist.empty() && lower(parsed.varlist[0]) == "list") {
      if (!session.hasData()) {
        blocks.push_back(ojson(eval::noDataLoaded()));
        return blocks;
      }
      if (auto err = eval::rejectUnknownOptions(parsed.opts, {})) {
        blocks.push_back(ojson(*err));
        return blocks;
      }
      blocks.push_back(ojson(handlers::runEreturnList(session)));
      return blocks;
    }
    // One block or an empty array (silent success) -- never pushed as an array.
    ojson r = handlers::runEreturn(parsed, session);
    if (!(r.is_array() && r.empty())) blocks.push_back(r);
    return blocks;
  }

  // ==================== AER-SCOPE1 bundle C (GTOOLS) arms =================
  // Reached only for the forms the translation step could NOT fold onto a
  // native command: `gstats` is a multiplex over two different natives, and
  // `gquantiles` in its `_pctile`/`pctile` form has no native twin at all
  // (the `xtile` form was already rewritten to `xtile` upstream).
  if (cmd == "gstats") {
    ojson r = handlers::runGstats(parsed, session);
    // `gstats sum` renders summarize's table, so it must also PUBLISH what
    // summarize publishes. r() is filled at the dispatcher call site, not
    // inside the handler (the summarize arm above does exactly this), so a
    // forwarding arm that only pushed the block would draw the same table and
    // leave r(N)/r(mean) empty -- and `gstats sum x` followed by
    // `display r(mean)` is precisely how the corpus uses it.
    if (r.is_object() && r.value("type", std::string()) != "error") {
      eval::captureRFromBlock(r, session.eval);
    }
    if (!(r.is_array() && r.empty())) blocks.push_back(r);
    return blocks;
  }
  if (cmd == "gquantiles") {
    ojson r = handlers::runGQuantilesPctile(parsed, session);
    if (!(r.is_array() && r.empty())) blocks.push_back(r);
    return blocks;
  }

  // ==================== AER-SCOPE1 bundle B (RESULTS) arms ================
  if (cmd == "return") {
    std::string sub = parsed.varlist.empty() ? "" : lower(parsed.varlist[0]);
    if (sub == "list") {
      if (auto err = eval::rejectUnknownOptions(parsed.opts, {}, {"all"})) {
        blocks.push_back(ojson(*err));
        return blocks;
      }
      blocks.push_back(ojson(handlers::runReturnList(session)));
      return blocks;
    }
    if (sub == "clear") {
      // `return clear` clears r() ONLY. rStrings doubles as the s() store
      // (`sreturn local` writes there under kSClassKeyPrefix) and `sreturn clear`
      // is its own command, so the reserved \x01 keys stay: eval::replaceRClass
      // is that shape, the one copy every wholesale r() replacement uses.
      // Outside a program there is nothing to return from: `capture return clear`
      // is rc 0 and the previous r() survives (w2o_rnames.log:528 N34 `0|60|60`).
      if (session.progCallDepth == 0) return blocks;
      eval::replaceRClass(session.eval);
      eval::declareRPosted(session.eval, "return clear", {});  // r() is empty (D.3.13)
      return blocks;
    }
    if (sub == "add") {
      // At the top level there is no program to return from, so the values are
      // already r() and `add` is a no-op. Inside one, it copies the CURRENT r()
      // into the staging that the program's return will flush -- which is what
      // makes `foo` then `return add` inside `bar` compose.
      if (session.progCallDepth > 0) {
        for (const auto& kv : session.eval.rValues.items()) {
          char buf[64];
          std::snprintf(buf, sizeof(buf), "%.17g", kv.second);
          session.returnStaging[kv.first] = {"scalar", std::string(buf)};
        }
        for (const auto& kv : session.eval.rStrings.items()) {
          if (!kv.first.empty() && kv.first[0] == '\x01') continue;  // reserved store
          session.returnStaging[kv.first] = {"local", kv.second};
        }
      }
      return blocks;
    }
    // `return scalar|local` reaches program::handleReturnStmt long before this
    // arm (dispatcher step (e), above), so anything still here is a form PIET
    // does not implement -- named as an unknown SUBCOMMAND, because the command
    // itself now exists.
    blocks.push_back(ojson(eval::unsupportedCommand(
        "return " + sub, std::nullopt, std::nullopt, "piet_gap",
        eval::UnsupportedCommandKind::kUnknownSubcommand)));
    return blocks;
  }
  if (cmd == "matlist") {
    blocks.push_back(ojson(handlers::runMatlist(parsed, session)));
    return blocks;
  }
  if (cmd == "svmat") {
    blocks.push_back(ojson(handlers::runSvmat(parsed, session)));
    return blocks;
  }
  if (cmd == "getmata") {
    // runGetmata returns exactly one object (the data_mutation card or one
    // error block), never an array.
    blocks.push_back(ojson(handlers::runGetmata(parsed, session)));
    return blocks;
  }
  if (cmd == "mkmat") {
    // Silent on success, so the empty array must be FILTERED, not pushed: an
    // array reaching the block stream is read with value("type") downstream and
    // throws type_error.306. This is the same defect class that aborted the
    // selftest binary in bundle D, recurring here on a fresh arm -- which is why
    // every silent arm in this file carries this exact guard.
    ojson r = handlers::runMkmat(parsed, session);
    if (!(r.is_array() && r.empty())) blocks.push_back(r);
    return blocks;
  }

  if (cmd == "matrix" || cmd == "mat") {
    // BUG-105/WS-K: runMatrixDef re-parses the raw line with its own
    // `mat(?:rix)?` patterns, so hand it a canonical leading token.
    ojson r = handlers::runMatrixDef(rawWithCanonicalCmd(parsed.raw, cmdTyped, cmd), session);
    if (!(r.is_array() && r.empty())) blocks.push_back(r);  // silent on success
    return blocks;
  }

  if (cmd == "label" && !parsed.varlist.empty()) {
    // BUG-524: canonicalise the subcommand ONCE, here, against the floor
    // table (resolveLabelSub above); every branch below compares the full
    // spelling. Below a floor is Stata's "invalid syntax" r(198)
    // (label_d_too_short, label_va_too_short) and the data are untouched; a
    // token that is not a prefix of any documented subcommand keeps the
    // unknown-subcommand refusal (BUG-105 arm 3 pins that wording; Stata's own
    // rc for it is unmeasured, so it stays what it was).
    //
    // This resolution now runs BEFORE the generic option-vocabulary reject
    // below (BUG-527): `label language NAME, new` needs the resolved
    // subcommand to intercept `, new` on its own terms (oracle rc 0), and
    // that decision has to be made before the generic net answers rc 198
    // for it.
    const std::string subTyped = lower(parsed.varlist[0]);
    std::string sub;
    switch (resolveLabelSub(subTyped, &sub)) {
      case LabelSubMatch::kResolved:
        break;
      case LabelSubMatch::kTooShort:
        blocks.push_back(ojson(eval::errorBlock("syntax_error", "invalid syntax",
                                                "label " + subTyped, std::nullopt, std::nullopt,
                                                "r(198)")));
        return blocks;
      case LabelSubMatch::kUnknown:
        blocks.push_back(ojson(eval::unsupportedCommand(
            "label " + subTyped, std::nullopt, std::nullopt, "piet_gap",
            eval::UnsupportedCommandKind::kUnknownSubcommand)));
        return blocks;
    }
    // BUG-527: `label language` takes its own options (new, copy, delete,
    // rename); kVocabLabel still lists new/rename/delete as unimplemented for
    // every OTHER subcommand.
    // BUG-336 (batch 2): known set measured from the handler body. `label define`
    // reads its own options by word from the raw tail (add/modify/replace/nofix,
    // each abbreviable: `, a` is add, lb_add_a), so it is not netted here; `label
    // values` takes nofix (lb_val_nofix); other label subcommands take none.
    if (sub != "define" && sub != "language") {
      if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabLabel.known,
                                                kVocabLabel.unimplemented)) {
        blocks.push_back(ojson(*err));
        return blocks;
      }
    }
    if (sub == "variable") {
      if (parsed.varlist.size() < 2) {
        blocks.push_back(ojson(eval::engineError("label variable: missing varname")));
        return blocks;
      }
      std::string varname = parsed.varlist[1];
      // Text is everything after the varname token in the raw line, quotes stripped.
      // BUG-105/WS-K: typed spelling, not the literal — see the `save` arm's
      // comment. `labe variable x "t"` threw std::out_of_range here.
      // BUG-251/WS-Q: this one searched for the command word only to slice
      // FROM it — i.e. to strip leading whitespace that eval::tokenize() has
      // already stripped. `raw.substr(npos)` throws unconditionally, with no
      // wrap-around to hide it. trimWs() is the whole of what it wanted.
      std::string rawArgs = trimWs(parsed.raw);
      // strip "label variable VARNAME" prefix (3 tokens) — split on whitespace,
      // keep the 4th+ as text (mirrors commands.py's maxsplit=3).
      size_t p = 0;
      for (int k = 0; k < 3 && p != std::string::npos; ++k) {
        p = rawArgs.find_first_of(" \t", p);
        if (p != std::string::npos) p = rawArgs.find_first_not_of(" \t", p);
      }
      std::string text = (p == std::string::npos) ? "" : trimWs(rawArgs.substr(p));
      // EM9: an outer compound pair `"..."' is stripped too (`label var x `"the "x"
      // var"'`, w2af_macro_text_file.log:1560; w2ao lb_var_lab_cq log:124).
      if (eval::startsCompound(text, 0) && eval::compoundSpanEnd(text, 0) == text.size()) {
        text = text.substr(2, text.size() - 4);
      } else if (text.size() >= 2 && ((text.front() == '"' && text.back() == '"') ||
                                      (text.front() == '\'' && text.back() == '\''))) {
        text = text.substr(1, text.size() - 2);
      }
      // The varname abbreviates (lb_var_abbrev: `label variable miss_age` labels
      // miss_age_PAP); a name that is not a variable is r(111) (lb_var_absent);
      // the label keeps its first 80 characters (lb_var_long, log:190).
      if (session.hasData()) {
        std::vector<std::string> one{varname};
        if (auto err = handlers::resolveByKeyNames(session, one)) {
          blocks.push_back(*err);
          return blocks;
        }
        Datastore* lds = session.datastore();
        if (one.size() != 1 || lds == nullptr || !lds->has(one.front())) {
          blocks.push_back(ojson(eval::errorBlock("engine_error", "variable " + varname +
                                                                      " not found",
                                                  std::nullopt, std::nullopt, std::nullopt,
                                                  "r(111)")));
          return blocks;
        }
        varname = one.front();
      }
      text = variableLabelText(text);
      ojson lv = handlers::runLabelVariable(varname, text, session);
      if (!(lv.is_array() && lv.empty())) blocks.push_back(lv);
      return blocks;
    }
    if (sub == "values") {
      if (parsed.varlist.size() < 2) {
        blocks.push_back(ojson(eval::engineError("label values: requires varname [lblname]")));
        return blocks;
      }
      // LBL-SEMANTICS: `label values VARLIST [LBLNAME|.]` -- the LAST token is the
      // set (`.` or none detaches) and every token before it a variable
      // (w2ao_label_family lb_val_multi `label values timecat fl db to`: all three
      // carry `to`); an absent variable is r(111) (:782 lb_val_absent) and a
      // string one r(181) "may not label strings" (:768 lb_val_str), each before
      // anything is attached.
      std::vector<std::string> vars(parsed.varlist.begin() + 1, parsed.varlist.end());
      std::optional<std::string> lblname;
      if (vars.size() >= 2) {
        lblname = vars.back();
        vars.pop_back();
        if (*lblname == ".") lblname.reset();
      }
      bool nofix = false;
      for (const auto& [k, v] : parsed.opts)
        if (k == "nofix") nofix = true;
      if (session.hasData()) {
        loops::VarlistExpandResult ex = loops::expandVarlist(joinSpace(vars), session);
        if (ex.isError) {
          blocks.push_back(ex.error);
          return blocks;
        }
        vars = ex.tokens;
        if (auto err = handlers::resolveByKeyNames(session, vars)) {
          blocks.push_back(*err);
          return blocks;
        }
        Datastore* lds = session.datastore();
        for (const auto& v : vars) {
          if (lds == nullptr || !lds->has(v)) {
            blocks.push_back(ojson(eval::errorBlock("engine_error", "variable " + v + " not found",
                                                    std::nullopt, std::nullopt, std::nullopt,
                                                    "r(111)")));
            return blocks;
          }
        }
        if (lblname.has_value()) {
          for (const auto& v : vars) {
            if (lds->dtype(v) == "factor") {
              blocks.push_back(ojson(eval::errorBlock("engine_error", "may not label strings",
                                                      std::nullopt, std::nullopt, std::nullopt,
                                                      "r(181)")));
              return blocks;
            }
          }
        }
      }
      for (const auto& v : vars) {
        ojson lv = handlers::runLabelValues(v, lblname, session, nofix);
        if (!(lv.is_array() && lv.empty())) {
          blocks.push_back(lv);
          return blocks;
        }
      }
      return blocks;
    }
    if (sub == "define") {
      // BUG-105/WS-K: runLabelDefine anchors on `^label\s+define\s+`.
      // BUG-524: ... so the typed subcommand (`def`, `de`) is canonicalised in
      // the raw line too, or `label def yn2 0 "no"` would fail its own anchor.
      ojson lv = handlers::runLabelDefine(
          rawWithCanonicalLabelSub(rawWithCanonicalCmd(parsed.raw, cmdTyped, cmd), subTyped, sub),
          session);
      if (!(lv.is_array() && lv.empty())) blocks.push_back(lv);
      return blocks;
    }
    if (sub == "list") {
      std::vector<std::string> names(parsed.varlist.begin() + 1, parsed.varlist.end());
      ojson ll = ojson(handlers::runLabelList(names, session));
      if (ll.is_array()) {
        for (auto& b : ll) blocks.push_back(b);
      } else {
        blocks.push_back(std::move(ll));
      }
      return blocks;
    }
    if (sub == "data") {
      // Stata's documented behaviour: label data sets a dataset label of up to
      // 80 characters, and with no label given it removes the existing one.
      // session.dataLabel is the field the .dta writer already
      // carries (handlers_data_verbs_internal.h); this is its first in-session
      // mutator, so `label data` no longer stops a do-file (14 corpus uses in
      // 5 papers). Not an oracle arm -- the definition is the help file's.
      // Text is everything after the subcommand token, quotes stripped (the
      // `label variable` branch's own slicing, one token earlier).
      std::string rawArgs = trimWs(parsed.raw);
      size_t p = 0;
      for (int k = 0; k < 2 && p != std::string::npos; ++k) {
        p = rawArgs.find_first_of(" \t", p);
        if (p != std::string::npos) p = rawArgs.find_first_not_of(" \t", p);
      }
      std::string text = (p == std::string::npos) ? "" : trimWs(rawArgs.substr(p));
      if (eval::startsCompound(text, 0) && eval::compoundSpanEnd(text, 0) == text.size()) {
        text = text.substr(2, text.size() - 4);  // EM9, as `label variable`
      } else if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
        text = text.substr(1, text.size() - 2);
      }
      session.dataLabel = text;
      return blocks;  // silent success, as `label variable` is
    }
    if (sub == "language") {
      // LBL-LANGUAGE: the whole subcommand (bare r(), new [copy], switch, delete,
      // rename) is handlers::runLabelLanguage; its options are its own.
      std::vector<std::string> args(parsed.varlist.begin() + 1, parsed.varlist.end());
      ojson lv = handlers::runLabelLanguage(args, parsed.opts, session);
      if (!(lv.is_array() && lv.empty())) blocks.push_back(lv);
      return blocks;
    }
    if (sub == "dir") {
      blocks.push_back(ojson(handlers::runLabelDir(session)));
      return blocks;
    }
    if (sub == "drop") {
      std::vector<std::string> names(parsed.varlist.begin() + 1, parsed.varlist.end());
      if (names.empty()) {
        blocks.push_back(ojson(eval::engineError("label drop: specify at least one label name")));
        return blocks;
      }
      ojson lv = handlers::runLabelDrop(names, session);
      if (!(lv.is_array() && lv.empty())) blocks.push_back(lv);
      return blocks;
    }
    // BUG-524: what is left resolved to a documented subcommand PIET has no
    // store for (copy, rename, save). A recognised feature, refused by NAME --
    // never accepted-and-ignored, which for `label save` would look like a
    // file was written.
    blocks.push_back(ojson(eval::unsupportedCommand(
        "label " + sub, std::nullopt, std::nullopt, "piet_gap",
        eval::UnsupportedCommandKind::kNotImplemented)));
    return blocks;
  }

  // P1-W2-0: `labelbook [LBLNAME...]` — a top-level command (not a `label`
  // subcommand), so parsed.varlist holds the optional label-set names
  // directly (no subcommand-token stripping, unlike the `label list` arm
  // above).
  if (cmd == "labelbook") {
    // BUG-336 (batch 2): known set measured from the handler body -- runLabelbook takes no options.
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabLabelbook.known, kVocabLabelbook.unimplemented)) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    // BUG-373: Stata publishes the label-set names it walked as r(names).
    ojson lbBlk = ojson(handlers::runLabelbook(parsed.varlist, session));
    if (lbBlk.value("type", std::string()) != "error") {
      std::string names;
      for (const auto& kv : session.labelDefs) {
        if (!names.empty()) names += " ";
        names += kv.first;
      }
      session.eval.rStrings.set("names", names);
    }
    blocks.push_back(std::move(lbBlk));
    return blocks;
  }

  if (cmd == "export" || cmd == "import") {
    // Two-word I/O verbs: `export delimited|excel ... using FILE` and
    // `import delimited|excel [using] FILE`. The sub-verb decides the format;
    // anything else is an honest reject naming what this build supports.
    static const std::regex reSub(R"(^\s*(?:export|import)\s+(\w+)\b\s*(.*)$)",
                                  std::regex::icase);
    std::smatch m;
    if (!std::regex_search(parsed.raw, m, reSub)) {
      blocks.push_back(ojson(eval::engineError(
          cmdTyped + ": expected `" + cmdTyped + " delimited ...` or `" + cmdTyped +
          " excel ...`")));
      return blocks;
    }
    std::string sub = lower(m.str(1));
    std::string rest = trimWs(m.str(2));
    // import / export dispatch the sub-verb by an exact table of
    // spellings: delim, delimi, delimit, delimite, delimited and exc, exce,
    // excel (w2ar dx_exd_varlist `export delim id s g using ...` rc 0,
    // log:468; dx_imd_038_delim rc 0, log:2708).
    {
      static const std::map<std::string, std::string> kSubSpellings = {
          {"delim", "delimited"}, {"delimi", "delimited"},   {"delimit", "delimited"},
          {"delimite", "delimited"}, {"delimited", "delimited"}, {"exc", "excel"},
          {"exce", "excel"},       {"excel", "excel"}};
      auto it = kSubSpellings.find(sub);
      if (it != kSubSpellings.end()) sub = it->second;
    }
    // Split `[varlist] using "FILE"` (export) / `[using] "FILE"` (import). The
    // option comma-tail is already stripped into parsed.opts. Words are split
    // quote-aware, and an `if`/`in` qualifier (the tokenizer already parsed it
    // into parsed.ifExpr / parsed.inRange) ends the file name.
    std::string spec = rest;
    {
      size_t comma = std::string::npos;
      bool inQ = false;
      for (size_t i = 0; i < spec.size(); ++i) {
        if (spec[i] == '"') inQ = !inQ;
        else if (spec[i] == ',' && !inQ) { comma = i; break; }
      }
      if (comma != std::string::npos) spec = spec.substr(0, comma);
      spec = trimWs(spec);
    }
    std::vector<std::string> words;
    {
      std::string cur;
      size_t i = 0;
      while (i < spec.size()) {
        const char c = spec[i];
        if (std::isspace(static_cast<unsigned char>(c))) {
          if (!cur.empty()) { words.push_back(cur); cur.clear(); }
          ++i;
          continue;
        }
        if (c == '`' && i + 1 < spec.size() && spec[i + 1] == '"') {
          const size_t j = spec.find("\"'", i + 2);
          const size_t end = j == std::string::npos ? spec.size() : j + 2;
          cur += spec.substr(i, end - i);
          i = end;
          continue;
        }
        if (c == '"') {
          const size_t j = spec.find('"', i + 1);
          const size_t end = j == std::string::npos ? spec.size() : j + 1;
          cur += spec.substr(i, end - i);
          i = end;
          continue;
        }
        cur += c;
        ++i;
      }
      if (!cur.empty()) words.push_back(cur);
    }
    {
      // Drop the qualifier tail: `... using f if x < 3 in 1/3`.
      for (size_t i = 0; i < words.size(); ++i) {
        const std::string lw = lower(words[i]);
        if (lw == "if" || lw == "in") { words.resize(i); break; }
      }
    }
    std::string pathArg;
    std::vector<std::string> varlist;
    bool sawUsing = false;
    for (const auto& w : words) {
      if (lower(w) == "using") { sawUsing = true; continue; }
      if (sawUsing) pathArg = w;
      else varlist.push_back(w);
    }
    if (!sawUsing) {
      // No `using`: export delimited / export excel re-parse the line
      // as `using <0>`, so the whole spec names the file (dx_exd_delim_novar
      // `export delimited `e1', delim(",") novar replace` rc 0, log:543;
      // dx_exx_097 `export excel "wb5", ...` log:1353). import takes the
      // same shape (`import delimited "f.csv"`).
      if (varlist.size() == 1) pathArg = varlist.front();
      else if (cmd == "import" && !varlist.empty()) pathArg = varlist.back();
      varlist.clear();
    }
    if (sub == "sas" || sub == "spss") {
      // import sas / import spss (User ruling (b), scope-ge2/data-io-family/
      // DECISION-import-fred-sas-spss.md): no reader. A missing file is
      // Stata's own r(601) with the data untouched (w2ar dx_imsas_missing,
      // dx_imspss_missing: 601, k unchanged, log:4702/4714); an existing one
      // is refused by name.
      if (cmd == "import" && !pathArg.empty()) {
        std::string p = resolveUserPath(session, pathArg);
        std::string shown = typedUserPath(pathArg);  // named as typed (BUG-410)
        const size_t slash = p.find_last_of('/');
        const size_t dot = p.find_last_of('.');
        if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) {
          p += (sub == "sas" ? ".sas7bdat" : ".sav");
          shown += (sub == "sas" ? ".sas7bdat" : ".sav");
        }
        if (!cecon::native_exists(p)) {
          blocks.push_back(ojson(eval::errorBlock("engine_error", "file " + shown + " not found",
                                                  std::nullopt, std::nullopt, std::nullopt,
                                                  "r(601)")));
          return blocks;
        }
      }
    }
    if (sub != "delimited" && sub != "excel") {
      blocks.push_back(ojson(eval::engineError(
          cmdTyped + " " + sub + ": not supported — this build implements `" + cmdTyped +
          " delimited` and `" + cmdTyped + " excel`.")));
      return blocks;
    }
    if (pathArg.empty()) {
      blocks.push_back(ojson(eval::engineError(
          cmdTyped + " " + sub + ": missing the file. Usage: " + cmdTyped + " " + sub +
          (cmd == "export" ? " [varlist] using \"file\"" : " using \"file\""))));
      return blocks;
    }
    // BUG-336 (batch 2): known sets measured from the four handler bodies
    // (hasFlagOpt/parseDelimiterOpt reads). `clear` is KNOWN on import because
    // PIET's import always replaces the in-memory dataset -- the flag's
    // semantic is satisfied, not dropped.
    {
      // AER scope-2 W2: the four branch sets moved to file scope (one copy
      // each) so kVocabImport/kVocabExport can be their per-command union.
      // The branch net still decides; nothing about the sets changed.
      std::unordered_set<std::string> known, unimpl;
      if (cmd == "export" && sub == "delimited") {
        known = kExportDelimKnown;
        unimpl = kExportDelimUnimpl;
      } else if (cmd == "export") {
        known = kExportExcelKnown;
        unimpl = kExportExcelUnimpl;
      } else if (sub == "delimited") {
        known = kImportDelimKnown;
        unimpl = kImportDelimUnimpl;
      } else {
        known = kImportExcelKnown;
        unimpl = kImportExcelUnimpl;
      }
      if (auto err = eval::rejectUnknownOptions(parsed.opts, known, unimpl)) {
        blocks.push_back(ojson(*err));
        return blocks;
      }
    }
    if (cmd == "export") {
      blocks.push_back(ojson(sub == "delimited"
                                 ? handlers::runExportDelimited(varlist, pathArg, parsed.opts,
                                                                parsed.ifExpr, parsed.inRange, session)
                                 : handlers::runExportExcel(varlist, pathArg, parsed.opts,
                                                            parsed.ifExpr, parsed.inRange, session)));
    } else {
      // [D] import delimited / import excel: `clear` is what lets the import
      // replace data changed since they were last saved; without it, r(4)
      // "no; data in memory would be lost", the data untouched
      // (w2ar_data_io_family.log dx_imd_noclear, dx_imx_noclear `4|k=7|N=4|`).
      // `import excel ..., describe` loads nothing and is not refused. A file
      // that is not there is answered first: `import delimited using
      // nofile.csv` on changed data is r(601) in StataBE 19.5, not r(4).
      bool importClears = false, describeOnly = false;
      for (const auto& [k, v] : parsed.opts) {
        if (k == "clear" && (!v.isBool || v.boolValue)) importClears = true;
        if (k == "describe" && (!v.isBool || v.boolValue)) describeOnly = true;
      }
      if (!importClears && !describeOnly && session.hasData() && session.datasetChanged &&
          cecon::native_exists(handlers::importSourcePath(session, pathArg, sub != "delimited"))) {
        blocks.push_back(ojson(eval::errorBlock("engine_error", "no; data in memory would be lost",
                                                std::nullopt, std::nullopt, std::nullopt,
                                                std::string("r(4)"))));
        return blocks;
      }
      blocks.push_back(ojson(sub == "delimited"
                                 ? handlers::runImportDelimited(varlist, pathArg, parsed.opts, session)
                                 : handlers::runImportExcel(pathArg, parsed.opts, session)));
    }
    return blocks;
  }

  if (cmd == "expand") {
    // AER scope-2 W2: `generate(newvar)` is a real Stata expand option this
    // build does not have, so it now says so instead of "not allowed".
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabExpand.known,
                                              kVocabExpand.unimplemented)) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    std::optional<std::string> expandGen;
    for (const auto& [k, v] : parsed.opts)
      if (k == "generate" && !v.isBool) expandGen = strutil::trim(v.strValue);
    // The count is the rest of the line after the verb (a bare number here).
    static const std::regex reExpand(R"(^\s*expand\s+(.+)$)", std::regex::icase);
    std::smatch m;
    std::string arg;
    if (std::regex_search(parsed.raw, m, reExpand)) arg = trimWs(m.str(1));
    // B4-W3b: the regex above takes the whole rest of the line, so with an
    // option present it captured "2, generate(dupflag)" and runExpand rejected
    // it as a per-observation expression. The count is what precedes the option
    // comma.
    {
      size_t comma = arg.find(',');
      if (comma != std::string::npos) arg = trimWs(arg.substr(0, comma));
    }
    // e(sample) T2 (w2_esample.log:1076 E6 `expand 2 if id==3`, rc 0): the
    // qualifiers are the tokenizer's parsed.ifExpr/inRange, so the count is
    // what precedes them (it was refused as a per-observation expression, rc 1).
    if (parsed.ifExpr.has_value() || parsed.inRange.has_value()) {
      static const std::regex reExpandQual(R"(\s+(?:if|in)\s.*$)", std::regex::icase);
      arg = trimWs(std::regex_replace(arg, reExpandQual, ""));
    }
    {
      // The constant form is the only one this build runs (runExpand says so);
      // a non-numeric argument is modelled as x2 for the pre-flight only.
      char* end = nullptr;
      const long long k = arg.empty() ? 0 : std::strtoll(arg.c_str(), &end, 10);
      const int64_t factor = (end && *end == '\0' && k > 0) ? static_cast<int64_t>(k) : 2;
      if (preflight::gate("expand", session, "", factor, blocks) == preflight::Verdict::Cancel) return blocks;
    }
    blocks.push_back(
        ojson(handlers::runExpandIf(arg, expandGen, parsed.ifExpr, parsed.inRange, session)));
    return blocks;
  }

  if (cmd == "fillin") {
    if (auto err = eval::rejectUnknownOptions(parsed.opts, {})) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    // fillin leaves r() alone (BUG-1451, StataBE: the earlier summarize's r(N) 28
    // and r(mean) survive it on 30 rows).
    blocks.push_back(ojson(handlers::runFillin(parsed.varlist, session)));
    return blocks;
  }

  if (cmd == "contract") {
    if (auto err = eval::rejectUnknownOptions(parsed.opts, {})) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    // contract leaves r() alone too (BUG-1451, measured as for fillin).
    blocks.push_back(ojson(handlers::runContract(parsed.varlist, session)));
    return blocks;
  }

  if (cmd == "cross") {
    if (auto err = eval::rejectUnknownOptions(parsed.opts, {})) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    static const std::regex reCross(R"(^\s*cross\s+using\s+(.+)$)", std::regex::icase);
    std::smatch m;
    if (!std::regex_search(parsed.raw, m, reCross)) {
      blocks.push_back(ojson(eval::engineError(
          "cross: missing `using FILE` clause. Usage: cross using \"file\"")));
      return blocks;
    }
    if (preflight::gate("cross", session, trimWs(m.str(1)), 0, blocks) == preflight::Verdict::Cancel) return blocks;
    blocks.push_back(ojson(handlers::runCross(trimWs(m.str(1)), session)));
    return blocks;
  }

  if (cmd == "winsor") {
    blocks.push_back(ojson(handlers::runWinsorCox(parsed, session)));
    return blocks;
  }

  if (cmd == "isid") {
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabIsid.known,
                                              kVocabIsid.unimplemented)) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    bool missok = false, sort = false;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "missok" && v.isBool && v.boolValue) missok = true;
      if (k == "sort" && v.isBool && v.boolValue) sort = true;
    }
    // Success is SILENT (Stata prints nothing) — the handler returns an empty
    // array, which must not be pushed as a block (same shape as preserve/
    // restore above); under `sort` it returns the sort and its note.
    ojson r = handlers::runIsid(parsed.varlist, session, missok, sort);
    if (r.is_array()) {
      for (const auto& b : r) blocks.push_back(b);
    } else {
      blocks.push_back(r);
    }
    return blocks;
  }

  if (cmd == "datasignature") {
    // BUG-1461: set/confirm/clear keep and check the signature (runDatasignatureSub);
    // report and the `using`/saving() file forms are named gaps, and any other
    // word is Stata's r(198) "invalid datasignature subcommand".
    std::string dsSub = parsed.varlist.empty() ? std::string() : lower(parsed.varlist[0]);
    // confirm abbreviates to conf; set and clear do not abbreviate (StataBE 19.5).
    if (dsSub.size() >= 4 && std::string("confirm").rfind(dsSub, 0) == 0) dsSub = "confirm";
    if (!dsSub.empty()) {
      const bool usingForm =
          std::any_of(parsed.varlist.begin(), parsed.varlist.end(),
                      [](const std::string& t) { return lower(t) == "using"; });
      if (dsSub == "report" || usingForm) {
        blocks.push_back(ojson(eval::unsupportedCommand(
            "datasignature " + dsSub + (usingForm ? " using" : ""), std::nullopt, std::nullopt,
            "piet_gap", eval::UnsupportedCommandKind::kUnknownSubcommand)));
        return blocks;
      }
      if (dsSub != "set" && dsSub != "confirm" && dsSub != "clear") {
        blocks.push_back(ojson(eval::errorBlock("engine_error", "invalid datasignature subcommand",
                                                std::nullopt, std::nullopt, std::nullopt,
                                                "r(198)")));
        return blocks;
      }
      if (dsSub == "clear" && !parsed.opts.empty()) {  // StataBE 19.5: r(101), not 198
        blocks.push_back(ojson(eval::errorBlock("engine_error", "options not allowed", std::nullopt,
                                                std::nullopt, std::nullopt, "r(101)")));
        return blocks;
      }
      std::unordered_set<std::string> known;
      if (dsSub == "set") known = {"reset"};
      if (dsSub == "confirm") known = {"strict"};
      if (auto err = eval::rejectUnknownOptions(parsed.opts, known,
                                                dsSub == "set" ? std::unordered_set<std::string>{"saving"}
                                                               : std::unordered_set<std::string>{})) {
        blocks.push_back(ojson(*err));
        return blocks;
      }
      if (dsSub != "clear") session.eval.rValues = eval::OrderedFloatDict();
      if (dsSub != "clear") session.eval.rStrings.clear();
      blocks.push_back(ojson(handlers::runDatasignatureSub(dsSub, parsed.opts, session)));
      return blocks;
    }
    if (auto err = eval::rejectUnknownOptions(parsed.opts, {})) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    // BUG-373: the signature itself is the whole point of the command, and it
    // was printed and not published — a do-file that compares signatures across
    // a run could not read it back.
    ojson dsBlk = ojson(handlers::runDatasignature(session));
    if (dsBlk.value("type", std::string()) == "display") {
      session.eval.rStrings.set("datasignature", trimWs(dsBlk.value("text", std::string())));
      dsBlk["text"] = "  " + dsBlk.value("text", std::string());  // indented, as Stata prints it
    }
    blocks.push_back(std::move(dsBlk));
    return blocks;
  }

  if (cmd == "preserve") {
    ojson r = handlers::runPreserve(session);
    if (!(r.is_array() && r.empty())) blocks.push_back(r);
    return blocks;
  }

  if (cmd == "restore") {
    // B5-D14 (BUG-454): this arm used to call NO reject net at all, so ANY
    // option was silently accepted and dropped -- adding the net here is
    // itself a behavior change (an unknown option on `restore` is now an
    // honest r(198) reject instead of rc 0).
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabRestore.known,
                                              kVocabRestore.unimplemented)) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    bool rearm = false;
    bool cancel = false;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "preserve" && v.isBool && v.boolValue) rearm = true;
      if (k == "not" && v.isBool && v.boolValue) cancel = true;
    }
    // BUG-922: `restore, not` forgets the preserved copy and leaves the data
    // as it is (oracle pc_pr_not: N stays 3; pc_pr_not_then_restore: the next
    // restore is r(622)). With nothing preserved it is r(622) like `restore`.
    // It was refused r(198) (`not` was not in the vocabulary). A level that
    // preserved nothing cannot cancel its caller's copy either: r(622) and the
    // copy stays (oracle w2ad_prog_preserve_nest pn_d_not_caller,
    // pn_p_not_caller: in1 622:4:30, then the caller's restore brings it back;
    // pn_top_none_not: 622).
    if (cancel) {
      if (rearm) {
        blocks.push_back(ojson(eval::errorBlock("syntax_error",
                                                "restore: options not and preserve may not be combined",
                                                std::nullopt, std::nullopt, std::nullopt,
                                                std::string("r(198)"))));
        return blocks;
      }
      if (!session.ownPreserveFrame()) {
        blocks.push_back(ojson(eval::errorBlock(
            "engine_error", "restore: no previous preserve — use `preserve` before `restore`",
            std::nullopt, std::nullopt, std::nullopt, std::string("r(622)"))));
        return blocks;
      }
      session.discardTopPreserve();
      return blocks;
    }
    ojson r = handlers::runRestore(session, rearm);
    if (!(r.is_array() && r.empty())) blocks.push_back(r);
    return blocks;
  }

  if (cmd == "levelsof") {
    if (parsed.varlist.empty()) {
      blocks.push_back(ojson(eval::engineError("levelsof: variable name required")));
      return blocks;
    }
    std::optional<std::string> localName;
    bool clean = false;
    // BS-029 (BETA Phase C batch C1): `missing`/`separate()` are now
    // implemented (previously honest-rejected as unknown options).
    bool missing = false;
    std::optional<std::string> separateSep;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "local" && !v.isBool) localName = v.strValue;
      if (k == "clean" && v.isBool && v.boolValue) clean = true;
      if (k == "missing" && v.isBool && v.boolValue) missing = true;
      if (k == "separate" && !v.isBool) separateSep = v.strValue;
    }
    // BUG-233: the one copy of this vocabulary now lives in the file-scope
    // registration block (see top of file); this binding keeps every existing
    // use — including the reject net below — reading the same set.
    const std::unordered_set<std::string>& kLevelsofKnownOpts = kVocabLevelsof.known;
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kLevelsofKnownOpts)) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    ojson result = ojson(
        handlers::runLevelsof(parsed.varlist[0], localName, clean, parsed.ifExpr, session));
    if (result.value("type", "") == "levelsof" && result.contains("levels") &&
        result["levels"].is_array()) {
      // handlers::runLevelsof (handlers_m3.cpp — outside this batch's file
      // scope) has no missing/separate support at all: its numeric/factor
      // scan explicitly drops NaN/negative codes (missing) and always
      // space-joins. Both are applied here as a post-process over its
      // `levels` array instead of duplicating that scan. `missing` appends
      // the dataset's own missing marker (".") after the ordinary
      // (non-missing) level list — Stata sorts missing last, so simple
      // append-at-end is correct; `separate(SEP)` re-joins whatever level
      // list results with SEP instead of the default space.
      std::vector<std::string> levelList;
      for (const auto& l : result["levels"]) levelList.push_back(l.get<std::string>());
      if (missing) {
        // NOTE: checks the dataset's UNFILTERED missingness — combining
        // `missing` with an `if` that itself changes whether the variable
        // still has missing rows is a known, unexercised edge case (the
        // oracle's `missing` probe carries no `if`).
        Datastore* ds = session.datastore();
        if (ds != nullptr && ds->has(parsed.varlist[0]) && ds->nMissing(parsed.varlist[0]) > 0) {
          levelList.push_back(".");
        }
      }
      std::string sep = separateSep.has_value() ? *separateSep : " ";
      std::string joined;
      for (size_t i = 0; i < levelList.size(); ++i) {
        if (i) joined += sep;
        joined += levelList[i];
      }
      result["levels"] = ojson(levelList);
      result["text"] = joined;
      // r(levels): Stata stores levelsof's level list as a STRING r-class
      // result, read back via the `` `r(levels)' `` macro-delimited form
      // (eval/macros.cpp's rStrings lookup) — this codebase never actually
      // wired that up (runLevelsof only ever sets the OPTIONAL local()
      // name, never r(levels) itself); wire it here since this is the only
      // site that knows the FINAL (missing/separate-adjusted) joined text.
      session.eval.rStrings.set("levels", joined);
      // BUG-373: Stata also publishes the counts. r(r) is the number of DISTINCT
      // levels and r(N) the number of observations they were drawn from — the two
      // a macro-driven do-file actually loops on.
      session.eval.rValues.set("r", static_cast<double>(levelList.size()));
      if (std::optional<int64_t> lvRows = session.residentRowsIfCurrent())
        session.eval.rValues.set("N", static_cast<double>(*lvRows));
      else if (Datastore* lvDs = session.datastore())
        session.eval.rValues.set("N", static_cast<double>(lvDs->nRows()));
      // Re-set local(name) too, in case runLevelsof already set it above
      // with the PRE-adjustment (missing/separate-unaware) text.
      if (localName.has_value() && !localName->empty()) {
        session.eval.locals.set(*localName, joined);
      }
      // levelsof's help; RULING.md D.3.11.
      eval::declareRPosted(session.eval, "levelsof", {"N", "r", "levels"});
    }
    blocks.push_back(result);
    return blocks;
  }

  // ------------------------------------------------------------------
  // M4a: output-completion surface (ROADMAP M4a item 1 — data-summary output)

  if (cmd == "winsor2") {
    double cutLo = 1.0, cutHi = 99.0;
    std::string suffix = "_w";
    std::string byStr;
    bool trim = false, replace = false, suffixGiven = false;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "cuts" && !v.isBool) {
        std::istringstream iss(v.strValue);
        std::vector<std::string> parts;
        std::string tok;
        while (iss >> tok) parts.push_back(tok);
        if (parts.size() == 2) {
          // BUG-112(4): strict-consume each number (mirrors the engine-side collapse
          // pNN fix @9f86577 — C1-F11 literal discipline) — `cuts(5x 95)` must reject,
          // not silently read "5x" as 5.
          size_t consumedLo = 0, consumedHi = 0;
          bool ok = true;
          try {
            cutLo = std::stod(parts[0], &consumedLo);
            cutHi = std::stod(parts[1], &consumedHi);
          } catch (...) {
            ok = false;
          }
          if (!ok || consumedLo != parts[0].size() || consumedHi != parts[1].size()) {
            blocks.push_back(ojson(eval::engineError(
                "winsor2: cuts(" + v.strValue + ") — expected two numbers")));
            return blocks;
          }
        } else {
          blocks.push_back(ojson(eval::engineError(
              "winsor2: cuts() requires exactly two values, got '" + v.strValue + "'")));
          return blocks;
        }
      } else if (k == "suffix" && !v.isBool) {
        suffix = v.strValue;
        suffixGiven = true;
      } else if (k == "by" && !v.isBool) {
        byStr = v.strValue;
      } else if (k == "trim" && v.isBool && v.boolValue) {
        trim = true;
      } else if (k == "replace" && v.isBool && v.boolValue) {
        replace = true;
      }
    }
    // winsor2.ado: with no suffix() the new variable is <var>_w, or <var>_tr
    // under `trim` (007's Wave 2 cleaning labels expectcomm_ehai_amount_tr).
    if (!suffixGiven && trim) suffix = "_tr";
    // BS-097: cuts/suffix/by/trim/replace are the only winsor2 options this
    // arm reads (community-package command; real winsor2 also has `label`).
    // BUG-233: the one copy of this vocabulary now lives in the file-scope
    // registration block (see top of file); this binding keeps every existing
    // use — including the reject net below — reading the same set.
    const std::unordered_set<std::string>& kWinsor2KnownOpts = kVocabWinsor2.known;
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kWinsor2KnownOpts)) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    if (parsed.varlist.empty()) {
      blocks.push_back(ojson(eval::engineError("winsor2: no variables specified")));
      return blocks;
    }
    // BUG-373: winsor2 publishes r(r1)/r(r2) — the two CUT VALUES it winsorised
    // at, i.e. the cutLo/cutHi percentiles of the (last) variable. Verified
    // against the live StataBE capture on beertax: r(r1) 0.04766949 and r(r2)
    // 2.41525412, which are exactly its p1 and p99. Computed here, before the
    // mutation, because afterwards the column has already been clipped to them.
    std::vector<double> w2cuts;
    {
      Datastore* w2ds = session.datastore();
      if (w2ds != nullptr && w2ds->has(parsed.varlist.back()) &&
          w2ds->dtype(parsed.varlist.back()) == "f64") {
        const double* col = w2ds->col(parsed.varlist.back()).f64();
        const int64_t wn = w2ds->nRows();
        std::vector<double> v;
        v.reserve(static_cast<size_t>(wn));
        for (int64_t i = 0; i < wn; ++i)
          if (!std::isnan(col[i])) v.push_back(col[i]);
        std::sort(v.begin(), v.end());
        // Stata's percentile rule, the same one Frame::xtile and summarize,detail
        // use: an exact rank boundary averages the two straddling values.
        auto pct = [&v](double p) {
          const int64_t n = static_cast<int64_t>(v.size());
          if (n == 0) return std::numeric_limits<double>::quiet_NaN();
          if (n == 1) return v[0];
          const double Pi = static_cast<double>(n) * p / 100.0;
          const double fl = std::floor(Pi);
          if (fl == Pi && Pi >= 1 && static_cast<int64_t>(Pi) < n)
            return (v[static_cast<size_t>(Pi) - 1] + v[static_cast<size_t>(Pi)]) / 2.0;
          int64_t idx = static_cast<int64_t>(fl) + 1;
          if (idx > n) idx = n;
          return v[static_cast<size_t>(idx) - 1];
        };
        if (!v.empty()) { w2cuts.push_back(pct(cutLo)); w2cuts.push_back(pct(cutHi)); }
      }
    }
    ojson w2Blk = ojson(handlers::runWinsor2(session, parsed.varlist, cutLo, cutHi, trim, byStr,
                                             suffix, replace));
    if (w2Blk.value("type", std::string()) != "error" && w2cuts.size() == 2) {
      session.eval.rValues.set("r1", w2cuts[0]);
      session.eval.rValues.set("r2", w2cuts[1]);
    }
    blocks.push_back(std::move(w2Blk));
    return blocks;
  }

  // BUG-105 abbreviation stopgap: `ta` (see the `describe`/`d` note above).
  if (cmd == "tabulate" || cmd == "tab" || cmd == "ta" || cmd == "tab1" || cmd == "tab2") {
    if (!session.hasData()) {
      blocks.push_back(ojson(eval::noDataLoaded()));
      return blocks;
    }
    bool chi2 = false;
    std::optional<std::string> sumvar;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "chi2" && v.isBool && v.boolValue) chi2 = true;
      if ((k == "sum" || k == "summarize") && !v.isBool) sumvar = v.strValue;
    }
    // BUG-1514: the two-way statistics lrchi2, V, gamma and taub, `all`, which is
    // chi2 lrchi2 V gamma taub ([R] tabulate twoway; StataBE 19.5 prints and posts
    // the five for `all`, not exact), and Fisher's `exact`. `tabStatWords` keeps
    // the words in the order typed for the refusals below.
    unsigned tabStats = 0;
    std::vector<std::string> tabStatWords;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "exact") {  // `exact` or `exact(#)`
        tabStats |= handlers::kTabExact;
        tabStatWords.push_back(k);
        continue;
      }
      if (!v.isBool || !v.boolValue) continue;
      unsigned bit = 0;
      if (k == "lrchi2") bit = handlers::kTabLrchi2;
      else if (k == "v") bit = handlers::kTabCramersV;
      else if (k == "gamma") bit = handlers::kTabGamma;
      else if (k == "taub") bit = handlers::kTabTaub;
      else if (k == "all")
        bit = handlers::kTabLrchi2 | handlers::kTabCramersV | handlers::kTabGamma | handlers::kTabTaub;
      if (bit == 0 && k != "chi2") continue;
      tabStats |= bit;
      if (k == "all") chi2 = true;
      tabStatWords.push_back(k == "v" ? std::string("V") : k);
    }
    // BS-097: chi2/sum/summarize/missing/generate/sort are the tabulate/tab1/
    // tab2 options this arm actually READS (with BUG-1514's statistics above).
    // `cell`/`col`/
    // `row`/`nofreq` are in kVocabTabulate's KNOWN set (rc 0, matching the
    // oracle) but are validated no-ops here on purpose — see kVocabTabulate's
    // own comment for the measured/unmeasured boundary that decision rests on.
    // BUG-233: the one copy of this vocabulary now lives in the file-scope
    // registration block (see top of file); this binding keeps every existing
    // use — including the reject net below — reading the same set.
    const std::unordered_set<std::string>& kTabulateKnownOpts = kVocabTabulate.known;
    // BUG-371: `missing` graduates from "option not allowed" to implemented — it
    // makes missing an ordinary category instead of a dropped row.
    bool tabMissing = false;
    for (const auto& [k, v] : parsed.opts)
      if (k == "missing" && (!v.isBool || v.boolValue)) tabMissing = true;
    // B5-D-TAB-SORT: `sort` (one-way only; see kVocabTabulate's own comment
    // and runTabulate/tabulateOne's `sortByFreq` param for the implementation
    // and its cited oracle cell).
    bool tabSort = false;
    for (const auto& [k, v] : parsed.opts)
      if (k == "sort" && (!v.isBool || v.boolValue)) tabSort = true;
    // W1a-1: `nolabel` -- "display numeric codes rather than value labels".
    // Read here, applied at the BUG-355 substitution seam below, which is the
    // only place this arm consults session.valueLabels: one flag, one consumer.
    bool tabNoLabel = false;
    for (const auto& [k, v] : parsed.opts)
      if (k == "nolabel" && (!v.isBool || v.boolValue)) tabNoLabel = true;
    // B4-W3b: `generate(stub)`. Read BEFORE the reject net so the net still
    // owns the accept/reject decision (the vocabulary above carries the name).
    std::string tabGenStub;
    for (const auto& [k, v] : parsed.opts)
      if (k == "generate" && !v.isBool) tabGenStub = strutil::trim(v.strValue);
    // BUG-1012: matcell()/matrow()/matcol() -- the frequencies and the row/column
    // values as matrices (tabulate_oneway.sthlp: matcell matrow; tabulate_twoway:
    // + matcol). They are the read-back every display-only option (col, row,
    // nofreq, nolabel, sort) is pinned through in w2as_summary_stats_family.
    std::string tabMatCell, tabMatRow, tabMatCol;
    for (const auto& [k, v] : parsed.opts) {
      if (v.isBool) continue;
      if (k == "matcell") tabMatCell = strutil::trim(v.strValue);
      if (k == "matrow") tabMatRow = strutil::trim(v.strValue);
      if (k == "matcol") tabMatCol = strutil::trim(v.strValue);
    }
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kTabulateKnownOpts,
                                              kVocabTabulate.unimplemented)) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    if (parsed.varlist.empty()) {
      blocks.push_back(ojson(eval::engineError("tabulate requires at least one variable")));
      return blocks;
    }
    // BUG-1012: `tabulate` takes one or two variables; a third is Stata's r(103)
    // "too many variables specified" (sx_tab_ref_three). Before, the third was
    // silently dropped and a two-way table of the first two was answered, rc 0.
    const bool tabPlainCmd = cmd == "tabulate" || cmd == "tab" || cmd == "ta";
    if (tabPlainCmd && parsed.varlist.size() > 2) {
      blocks.push_back(ojson(eval::errorBlock("syntax_error", "too many variables specified",
                                              std::nullopt, std::nullopt, std::nullopt,
                                              std::string("r(103)"))));
      return blocks;
    }
    // BUG-1514: Cramér's V is the upper-case option `V`; `tabulate c d, v` is
    // r(198) "option v not allowed" (StataBE 19.5). Option keys arrive lower-cased,
    // so the spelling is read back from the command as typed.
    if (std::find(tabStatWords.begin(), tabStatWords.end(), "V") != tabStatWords.end()) {
      const long long comma = eval::findUnquotedComma(parsed.raw);
      bool upperV = false;
      if (comma >= 0) {
        std::istringstream iss(parsed.raw.substr(static_cast<size_t>(comma) + 1));
        for (std::string w; iss >> w;)
          if (w == "V") upperV = true;
      }
      if (!upperV) {
        blocks.push_back(ojson(eval::engineError("option `v` not allowed", "unsupported_option")));
        return blocks;
      }
    }
    // BUG-1514: `nolog` silences exact's enumeration log and is refused without it.
    if (!(tabStats & handlers::kTabExact))
      for (const auto& [k, v] : parsed.opts)
        if (k == "nolog") {
          blocks.push_back(ojson(eval::engineError("option `nolog` not allowed", "unsupported_option")));
          return blocks;
        }
    // BUG-1514: the statistics are two-way options. One-way, Stata answers r(198)
    // "option lrchi2 not allowed" (likewise gamma, V, all, and chi2 itself, which
    // this arm accepted and ignored; StataBE 19.5 `tabulate c, chi2`). tab1 prints
    // that refusal under its first variable's heading, tabulates nothing, leaves
    // r() empty and ends rc 0 (`tab1 c d, chi2`, `tab1 c d, lrchi2`); this showed
    // both tables with the option ignored and posted the last one's r().
    const bool tabTwoWay = cmd == "tab2" || (cmd != "tab1" && parsed.varlist.size() == 2);
    if (!tabStatWords.empty() && !tabTwoWay) {
      std::string word;
      for (const auto& w : tabStatWords)
        if (w != "chi2" && word.empty()) word = w;
      if (word.empty()) word = "chi2";
      if (cmd == "tab1") {
        ojson head, refusal;
        head["type"] = "display";
        head["text"] = "-> tabulation of " + parsed.varlist[0] + "  ";
        refusal["type"] = "display";
        refusal["text"] = "option " + word + " not allowed";
        blocks.push_back(std::move(head));
        blocks.push_back(std::move(refusal));
        eval::replaceRClass(session.eval);
        eval::declareRPosted(session.eval, "tabulate", {});
        return blocks;
      }
      blocks.push_back(ojson(eval::engineError("option `" + word + "` not allowed", "unsupported_option")));
      return blocks;
    }
    // BUG-1514: the statistics count frequencies. Under [aw=] StataBE 19.5 answers
    // r(406) "lrchi2 not allowed with aweights" (chi2, V, gamma, taub and all alike),
    // under [iw=] r(198) "option lrchi2 not allowed" (chi2 ... all alike).
    if (!tabStatWords.empty() && tabTwoWay && parsed.weightType.has_value()) {
      const std::string wt = strutil::lower(*parsed.weightType);
      const std::string& word = tabStatWords.front();
      if (wt == "aw" || wt == "aweight") {
        blocks.push_back(ojson(eval::errorBlock("syntax_error", word + " not allowed with aweights",
                                                std::nullopt, std::nullopt, std::nullopt,
                                                std::string("r(406)"))));
        return blocks;
      }
      if (wt == "iw" || wt == "iweight") {
        blocks.push_back(ojson(eval::engineError("option `" + word + "` not allowed", "unsupported_option")));
        return blocks;
      }
    }
    // matcol() is a two-way option and the matrix options belong to tabulate
    // itself: one-way matcol() is Stata's "option matcol() not allowed", r(198).
    // tab1/tab2 build several tables, and this build does not say which one a
    // matrix would hold, so the options are refused there by name.
    if (!tabMatCol.empty() && tabPlainCmd && parsed.varlist.size() == 1) {
      blocks.push_back(ojson(eval::errorBlock("syntax_error", "option matcol() not allowed",
                                              std::nullopt, std::nullopt, std::nullopt,
                                              std::string("r(198)"))));
      return blocks;
    }
    if (!tabPlainCmd && (!tabMatCell.empty() || !tabMatRow.empty() || !tabMatCol.empty())) {
      blocks.push_back(ojson(eval::unsupportedOption(
          "matcell", cmd + ": matcell()/matrow()/matcol() are served for `tabulate` only")));
      return blocks;
    }
    Datastore* ds = session.datastore();
    // BUG-484: a column this frame does not have used to reach runTabulate,
    // where Columnar::col() throws EngineError("column not found: X") — and
    // that throw crossed the whole dispatch stack to main.cpp:382, which
    // replaced the ENTIRE run's blocks with one `run failed:` error. Not a
    // truncation: everything the run had already produced was discarded, at
    // exit 0, with stopped_at_line unset, and `capture` could not contain it.
    //
    // Nothing exotic is needed — `codebook` a few thousand lines up already
    // does exactly this, and the oracle says tabulate is not a special case:
    // `summarize nosuchcol` and `list nosuchcol` are r(111) in Stata too, and
    // so is `tabulate nosuchcol` (one-way, two-way, and with `, missing`).
    // "variable X not found" is the wording eval::coarseRc maps to 111.
    for (const auto& tvar : parsed.varlist) {
      if (!ds->has(tvar)) {
        blocks.push_back(ojson(eval::engineError("variable " + tvar + " not found")));
        return blocks;
      }
    }
    if (sumvar.has_value() && !sumvar->empty() && !ds->has(*sumvar)) {
      blocks.push_back(ojson(eval::engineError("variable " + *sumvar + " not found")));
      return blocks;
    }
    // BUG-042: `tabulate ... if COND` — runTabulate is pure computation over
    // a Datastore reference with no cobra call at all, so there's no
    // `--keepif` to forward; pre-filter into a temp hbin instead and hand
    // THAT Datastore to the (otherwise untouched) handler, mirroring the
    // identical if-prefilter already used for `twoway`/graph commands a few
    // hundred lines up in this same file (dispatcherNextPath + `cobra eval
    // --keepif` + a fresh Datastore over the filtered copy). BS-090 (BETA
    // Phase G): `in` now composes with `if` via AND here too
    // (piet::handlers::applyIfAndInFilter).
    std::unique_ptr<Datastore> tabIfDs;
    if ((parsed.ifExpr.has_value() && !parsed.ifExpr->empty()) ||
        (parsed.inRange.has_value() && !parsed.inRange->empty())) {
      handlers::InRangeFilterResult fr;
      try {
        fr = handlers::applyIfAndInFilter(session, ds->path(), parsed.ifExpr, parsed.inRange);
      } catch (const CobraError& ce) {
        blocks.push_back(ojson(eval::engineError(cobraErrorMessage(ce), ce.code)));
        return blocks;
      }
      if (!fr.ok) {
        blocks.push_back(ojson(handlers::inRangeRejectError(fr, *parsed.inRange, cmdTyped)));
        return blocks;
      }
      tabIfDs = std::make_unique<Datastore>(fr.dataPath);
      ds = tabIfDs.get();
    }
    // `parsed.weightVar` survives the tokenizer's guard here as fw/aw/iw on
    // tabulate/tab1 and fw on tab2 (kStataLegalWeights answers r(101) for the
    // rest); runTabulate reads the type to choose integer fweight cells or
    // the real-valued aweight/iweight cells. The `if`-prefilter above (when
    // present) already carries the weight column through unchanged since it
    // filters ROWS of the whole dataset, not a chosen subset of columns.
    // BUG-1514: a two-way `nofreq` with nothing else to show -- no cell, col or row
    // line and no statistic -- prints nothing and leaves r() EMPTY, matcell() unmade
    // (StataBE 19.5 `tabulate c d, wrap nofreq`, `, nofreq nokey`, `, nofreq
    // missing`, `, nofreq matcell(mm)` and `tab2 c d e, nofreq`: `return list` is
    // empty, `matrix list mm` r(111)); with `row`, `cell` or a statistic it posts
    // r(N) r(r) r(c) as usual. One-way `nofreq` posts r(N) r(r) (`tabulate c, nofreq`).
    if (tabTwoWay && tabStatWords.empty() && !sumvar.has_value() && tabGenStub.empty()) {
      bool nofreq = false, cellLines = false;
      for (const auto& [k, v] : parsed.opts) {
        if (k == "nofreq") nofreq = true;
        if (k == "cell" || k == "col" || k == "row") cellLines = true;
      }
      if (nofreq && !cellLines) {
        eval::replaceRClass(session.eval);
        eval::declareRPosted(session.eval, "tabulate", {});
        return blocks;
      }
    }
    ojson tabBlock =
        handlers::runTabulate(*ds, parsed.varlist, chi2, cmd, sumvar, parsed.weightVar,
                              tabMissing, tabSort, parsed.weightType, tabStats);
    // B4-W3b: the RAW level keys, snapshotted here — the value-label
    // substitution a few lines down rewrites tabBlock["table"]["rows"] in
    // place, and a dummy built from a LABEL ("Domestic") instead of its code
    // (0) would compare against a string the column does not hold.
    std::vector<std::string> tabRawRows;
    std::optional<ojson> tabRawTable;  // the table before value labels, for the matrices
    if (tabBlock.is_object() && tabBlock.contains("table") && tabBlock["table"].is_object()) {
      tabRawTable = tabBlock["table"];
      if (tabBlock["table"].contains("rows") && tabBlock["table"]["rows"].is_array())
        for (const auto& r : tabBlock["table"]["rows"])
          if (r.is_string()) tabRawRows.push_back(r.get<std::string>());
    }
    // BUG-354 (W1 lens L3): `tabulate` is r-class and published nothing —
    // `di r(r)` answered "r(r) is not set". Stata (StataBE 19.5, `tabulate
    // rep78` on auto) sets r(r)=5 (the number of ROWS, i.e. distinct levels
    // shown) and r(N)=69 (the N of the tabulated sample, which EXCLUDES missing
    // — 69 not 74 on that fixture). Both are already in the block this handler
    // just built, so this is carriage at the seam that has them, not a re-count.
    // BUG-355 (W1 lens L4): `tabulate` ignored value labels while the
    // coeftable path applied them from the SAME session store — measured, one
    // session: `regress fatal i.g3` renders `mid.g3`/`high.g3` while
    // `tabulate g3` renders `1.0`/`2.0`/`3.0`. Stata labels the tabulate rows
    // (StataBE 19.5, `label values rep78 replab` -> rows read `three`/`four`
    // while the UNLABELED codes stay numeric). runTabulate takes a Datastore and
    // never sees the session, so the substitution belongs here, at the seam that
    // has both — the same arrangement `applyFactorValueLabels` uses for
    // coefficient rows.
    if (tabBlock.is_object() && tabBlock.contains("table") && tabBlock["table"].is_object() &&
        parsed.varlist.size() == 1 && !session.valueLabels.empty() && !tabNoLabel) {
      auto vlIt = session.valueLabels.find(parsed.varlist[0]);
      if (vlIt != session.valueLabels.end()) {
        auto relabel = [&](const std::string& raw) {
          try {
            const double d = std::stod(raw);
            const double r = std::nearbyint(d);
            // Only an INTEGRAL value can carry a value label; 1.5 has no code.
            if (std::fabs(d - r) > 1e-9) return raw;
            auto it = vlIt->second.find(static_cast<int>(r));
            // An unlabeled code keeps its numeric rendering — Stata's own mix.
            return it == vlIt->second.end() ? raw : it->second;
          } catch (const std::exception&) {
            return raw;  // non-numeric row label (a factor's level string)
          }
        };
        ojson& tbl = tabBlock["table"];
        if (tbl.contains("rows") && tbl["rows"].is_array()) {
          ojson newRows = ojson::array();
          ojson newVals = ojson::object();
          const bool haveVals = tbl.contains("values") && tbl["values"].is_object();
          for (const auto& r : tbl["rows"]) {
            const std::string oldKey = r.get<std::string>();
            const std::string newKey = relabel(oldKey);
            newRows.push_back(newKey);
            if (haveVals && tbl["values"].contains(oldKey)) newVals[newKey] = tbl["values"][oldKey];
          }
          tbl["rows"] = newRows;
          if (haveVals) tbl["values"] = newVals;
        }
      }
    }
    // BUG-373: tab1/tab2 emit `mode:"multi"` with a `tables` ARRAY, so the
    // single-table r-fill below never fired for them and they published nothing.
    // Stata reports the LAST table's counts, which is what a following
    // `display r(r)` reads.
    //
    // r(N) is the tabulated sample's own N, which is why it excludes missing
    // (Stata: 69, not 74, on `tabulate rep78`): the table's grand total,
    // margins.total. BUG-1012 (FU-263): it used to be summed over `values`,
    // whose two-way entries are row OBJECTS, not numbers, so every two-way
    // tabulate posted r(N) = 0 (w2o_rnames N11 N12 N14: Stata 60). Two-way also
    // posts r(c), the column count, and with `chi2` r(chi2) and r(p) (FU-264;
    // tabulate_twoway.sthlp Stored results).
    // BUG-373: tab1/tab2 emit `mode:"multi"` with a `tables` ARRAY; Stata
    // reports the LAST table's counts, which is what a following
    // `display r(r)` reads.
    auto postTabR = [&](const ojson& holder) {
      if (!holder.is_object() || !holder.contains("table") || !holder["table"].is_object()) return;
      const auto& tbl = holder["table"];
      if (tbl.contains("rows") && tbl["rows"].is_array())
        session.eval.rValues.set("r", static_cast<double>(tbl["rows"].size()));
      if (tbl.contains("cols") && tbl["cols"].is_array())
        session.eval.rValues.set("c", static_cast<double>(tbl["cols"].size()));
      if (tbl.contains("margins") && tbl["margins"].is_object() &&
          tbl["margins"].contains("total") && tbl["margins"]["total"].is_number())
        session.eval.rValues.set("N", tbl["margins"]["total"].get<double>());
      if (holder.contains("chi2") && holder["chi2"].is_object()) {
        const auto& c2 = holder["chi2"];
        if (c2.contains("statistic") && c2["statistic"].is_number())
          session.eval.rValues.set("chi2", c2["statistic"].get<double>());
        if (c2.contains("p_value") && c2["p_value"].is_number())
          session.eval.rValues.set("p", c2["p_value"].get<double>());
      }
      // BUG-1514: the other statistics, under Stata's names and in its order
      // (StataBE 19.5 `return list` after `tabulate c d, all`).
      auto setR = [&](const char* name, const ojson& obj, const char* field) {
        if (obj.contains(field) && obj[field].is_number())
          session.eval.rValues.set(name, obj[field].get<double>());
      };
      if (holder.contains("lrchi2") && holder["lrchi2"].is_object()) {
        setR("chi2_lr", holder["lrchi2"], "statistic");
        setR("p_lr", holder["lrchi2"], "p_value");
      }
      if (holder.contains("cramers_v") && holder["cramers_v"].is_number())
        session.eval.rValues.set("CramersV", holder["cramers_v"].get<double>());
      if (holder.contains("gamma") && holder["gamma"].is_object()) {
        setR("gamma", holder["gamma"], "value");
        setR("ase_gam", holder["gamma"], "ase");
      }
      if (holder.contains("taub") && holder["taub"].is_object()) {
        setR("taub", holder["taub"], "value");
        setR("ase_taub", holder["taub"], "ase");
      }
      if (holder.contains("exact") && holder["exact"].is_object()) {
        setR("p_exact", holder["exact"], "p");
        setR("p1_exact", holder["exact"], "p1");
      }
    };
    if (!sumvar.has_value() && tabBlock.is_object() && tabBlock.contains("tables") &&
        tabBlock["tables"].is_array() && !tabBlock["tables"].empty())
      postTabR(tabBlock["tables"].back());
    if (!sumvar.has_value()) postTabR(tabBlock);
    // BUG-1012: tabulate, summarize() leaves r() EMPTY in Stata (every read of
    // r(N) r(r) r(mean) ... is `.` after it: sx_tab_sum, sx_tab_sum_two). Before,
    // the previous command's r() stayed live and was read back as tabulate's.
    if (sumvar.has_value() && tabBlock.is_object() &&
        tabBlock.value("type", std::string()) != "error") {
      eval::replaceRClass(session.eval);
      eval::declareRPosted(session.eval, "tabulate", {});
    }
    // BUG-1012: the matrices, from the table as built (before value labels; a
    // row key is the value's round-trip rendering, `.`/`.a` for a missing
    // category). matcell = frequencies, matrow = the row values (r x 1), matcol =
    // the column values (1 x c): sx_tab_one_mat, sx_tab_two_mat, sx_tab_sort,
    // sx_tab_missing, sx_tab_m.
    if ((!tabMatCell.empty() || !tabMatRow.empty() || !tabMatCol.empty()) && tabRawTable &&
        tabBlock.value("type", std::string()) != "error") {
      const ojson& tbl = *tabRawTable;
      auto keyValue = [](const std::string& key, double& out) {
        if (!key.empty() && key[0] == '.') {
          if (key.size() == 1) { out = eval::stataMissingWithKind(0); return true; }
          if (key.size() == 2 && key[1] >= 'a' && key[1] <= 'z') {
            out = eval::stataMissingWithKind(key[1] - 'a' + 1);
            return true;
          }
        }
        try {
          size_t pos = 0;
          out = std::stod(key, &pos);
          return pos == key.size();
        } catch (...) {
          return false;
        }
      };
      auto keysOf = [&](const char* field) {
        std::vector<std::string> ks;
        if (tbl.contains(field) && tbl[field].is_array())
          for (const auto& r : tbl[field]) ks.push_back(r.get<std::string>());
        return ks;
      };
      const std::vector<std::string> rowKeys = keysOf("rows");
      const std::vector<std::string> colKeys = keysOf("cols");
      const bool twoWayTbl = tbl.contains("cols");
      auto vectorMatrix = [&](const std::vector<std::string>& ks, bool asRow,
                              const std::string& what, eval::EvalMatrix& m) -> bool {
        std::vector<double> vals;
        for (const auto& k : ks) {
          double v = 0;
          if (!keyValue(k, v)) {
            blocks.push_back(ojson(eval::unsupportedOption(
                what, "tabulate: " + what + "() holds numeric values, and this variable is a string")));
            return false;
          }
          vals.push_back(v);
        }
        if (asRow) {
          m.data = {vals};
          m.rownames = {"r1"};
          for (size_t j = 0; j < vals.size(); ++j) m.colnames.push_back("c" + std::to_string(j + 1));
        } else {
          for (size_t i = 0; i < vals.size(); ++i) {
            m.data.push_back({vals[i]});
            m.rownames.push_back("r" + std::to_string(i + 1));
          }
          m.colnames = {"c1"};
        }
        return true;
      };
      if (!tabMatCell.empty()) {
        eval::EvalMatrix m;
        const ojson& vals = tbl["values"];
        for (size_t i = 0; i < rowKeys.size(); ++i) {
          std::vector<double> row;
          if (twoWayTbl) {
            for (const auto& ck : colKeys) row.push_back(vals[rowKeys[i]][ck].get<double>());
          } else {
            row.push_back(vals[rowKeys[i]].get<double>());
          }
          m.data.push_back(std::move(row));
          m.rownames.push_back("r" + std::to_string(i + 1));
        }
        const size_t nc = twoWayTbl ? colKeys.size() : 1;
        for (size_t j = 0; j < nc; ++j) m.colnames.push_back("c" + std::to_string(j + 1));
        storeUserMatrix(session, tabMatCell, std::move(m));
      }
      if (!tabMatRow.empty()) {
        eval::EvalMatrix m;
        if (!vectorMatrix(rowKeys, false, "matrow", m)) return blocks;
        storeUserMatrix(session, tabMatRow, std::move(m));
      }
      if (!tabMatCol.empty()) {
        eval::EvalMatrix m;
        if (!vectorMatrix(colKeys, true, "matcol", m)) return blocks;
        storeUserMatrix(session, tabMatCol, std::move(m));
      }
      if (tbl.contains("cols") && tbl["cols"].is_array()) {
        session.eval.rValues.set("c", static_cast<double>(tbl["cols"].size()));
      }
    }
    // ------------------------------------------------------------------
    // B4-W3b: `tabulate VAR, generate(stub)` — one byte dummy per level,
    // named stub1..stubK in the table's own (sorted) level order.
    //
    // THE RULE THAT DECIDES THIS, and the reason an rc-only test cannot see
    // it: the dummies are MISSING where the source is missing, not 0. Measured
    // (StataBE 19.5, tests/eval/golden/aer_b4/pins_oracle_g.log:170-176,
    // `sysuse auto` + `tabulate rep78, generate(rs)`):
    //     summarize rs1, meanonly  ->  r(N) 69, NOT 74
    //     r(mean) = +1.dae6076b981dbX-006  (= 2/69, the 5 missing rep78 rows
    //                                       are OUT of the denominator)
    // Zero-filling instead would pass every count and every "did the variables
    // appear" check, and would move the coefficient of every regression that
    // uses one of them. So the expression is cond(missing(v), ., v==level) and
    // never a bare comparison.
    if (!tabGenStub.empty()) {
      ojson genErr = runTabulateGenerate(tabGenStub, parsed.varlist, tabRawRows, session,
                                         parsed.ifExpr, parsed.inRange, tabMissing);
      if (!genErr.is_null()) {
        blocks.push_back(genErr);
        return blocks;
      }
    }
    // The names Stata posts (RULING.md D.3.4): one-way N r
    // (tabulate_oneway's help); two-way N r c, + chi2 p with `chi2`
    // (tabulate_twoway's help). BUG-1514: + chi2_lr p_lr, CramersV, gamma
    // ase_gam, taub ase_taub, p_exact p1_exact with lrchi2, V, gamma, taub, exact,
    // for the statistics the (last) table carries: none on a table with one row or
    // one column. With sum() the set is not declared.
    if (!sumvar.has_value() && tabBlock.is_object() &&
        tabBlock.value("type", std::string()) != "error") {
      const bool twoWay = cmd == "tab2" || (cmd != "tab1" && parsed.varlist.size() == 2);
      std::vector<std::string> stataNames = {"N", "r"};
      if (twoWay) {
        stataNames.push_back("c");
        const ojson& last = tabBlock.contains("tables") && tabBlock["tables"].is_array() &&
                                    !tabBlock["tables"].empty()
                                ? tabBlock["tables"].back()
                                : tabBlock;
        if (last.contains("chi2")) stataNames.insert(stataNames.end(), {"chi2", "p"});
        if (last.contains("lrchi2")) stataNames.insert(stataNames.end(), {"chi2_lr", "p_lr"});
        if (last.contains("cramers_v")) stataNames.push_back("CramersV");
        if (last.contains("gamma")) stataNames.insert(stataNames.end(), {"gamma", "ase_gam"});
        if (last.contains("taub")) stataNames.insert(stataNames.end(), {"taub", "ase_taub"});
        if (last.contains("exact")) stataNames.insert(stataNames.end(), {"p_exact", "p1_exact"});
      }
      eval::declareRPosted(session.eval, "tabulate", std::move(stataNames));
    }
    blocks.push_back(ojson(tabBlock));
    return blocks;
  }

  if (cmd == "table") {
    if (!session.hasData()) {
      blocks.push_back(ojson(eval::noDataLoaded()));
      return blocks;
    }
    if (parsed.varlist.empty()) {
      blocks.push_back(ojson(eval::engineError("table: requires at least one row variable")));
      return blocks;
    }
    // Stata 17 spells the dimensions `table (rowspec) (colspec)`. The tokenizer
    // hands them over with the parentheses attached, so a bare lookup reported
    // "variable not found: '(region)'". Strip one enclosing pair per token; a
    // token without them is the pre-17 spelling and passes through untouched.
    auto stripDimParens = [](const std::string& t) {
      return (t.size() >= 2 && t.front() == '(' && t.back() == ')') ? t.substr(1, t.size() - 2) : t;
    };
    std::string rowvar = stripDimParens(parsed.varlist[0]);
    std::optional<std::string> colvar;
    if (parsed.varlist.size() >= 2) colvar = stripDimParens(parsed.varlist[1]);
    // `statistic()` REPEATS in Stata 17 (`statistic(mean y) statistic(count y)`).
    // The old loop overwrote its single slot each pass, so a two-statistic
    // request silently answered with the last one only — the caller asked two
    // questions and got one answer with no indication the other was dropped.
    struct StatSpec { std::string stat; std::optional<std::string> var; };
    std::vector<StatSpec> statSpecs;
    // BUG-1015: the pre-17 `contents(stat var ...)` (c()) form, legal only under
    // `version 16:` or lower (table_16.sthlp; w2as sx_table_c_v16*). Its
    // statistics map onto the same cells statistic() computes; the ones this
    // table kernel does not compute (median, p#, iqr, rawsum) are refused by
    // name rather than shown empty, and a word that is no statistic is r(198)
    // (sx_table_ref_stat). The form is output-only: it posts no r().
    const bool tableV16 = session.callerVersion > 0.0 && session.callerVersion < 17.0;
    bool tableContents = false;
    for (const auto& [k, v] : parsed.opts) {
      if (!((k == "contents" || k == "c") && !v.isBool)) continue;
      if (!tableV16) {
        blocks.push_back(ojson(eval::errorBlock(
            "unsupported_option",
            "option contents() not allowed since Stata 17; see help table for updated syntax")));
        return blocks;
      }
      tableContents = true;
      std::istringstream iss(v.strValue);
      std::vector<std::string> words;
      std::string w;
      while (iss >> w) words.push_back(w);
      static const std::unordered_set<std::string> kServed = {"freq", "mean", "sd", "sum",
                                                              "count", "n", "max", "min"};
      for (size_t i = 0; i < words.size();) {
        const std::string st = lower(words[i]);
        const bool known = kServed.count(st) || st == "rawsum" || st == "median" || st == "iqr" ||
                           (st.size() >= 2 && st[0] == 'p' &&
                            st.find_first_not_of("0123456789", 1) == std::string::npos);
        if (!known) {
          blocks.push_back(ojson(eval::errorBlock("syntax_error", words[i] + " invalid statistic",
                                                  std::nullopt, std::nullopt, std::nullopt,
                                                  std::string("r(198)"))));
          return blocks;
        }
        if (!kServed.count(st)) {
          blocks.push_back(ojson(eval::unsupportedOption(
              "contents(" + st + ")",
              "table, contents(): this build computes freq mean sd sum count n max min")));
          return blocks;
        }
        if (st == "freq") {
          statSpecs.push_back({"freq", std::nullopt});
          ++i;
        } else {
          if (i + 1 >= words.size()) {
            blocks.push_back(ojson(eval::errorBlock("syntax_error", "varlist required",
                                                    std::nullopt, std::nullopt, std::nullopt,
                                                    std::string("r(100)"))));
            return blocks;
          }
          statSpecs.push_back({st, words[i + 1]});
          i += 2;
        }
      }
    }
    for (const auto& [k, v] : parsed.opts) {
      if ((k == "statistic" || k == "stat") && !v.isBool) {
        std::istringstream iss(v.strValue);
        std::vector<std::string> parts;
        std::string tok;
        while (iss >> tok) parts.push_back(tok);
        if (!parts.empty()) {
          StatSpec sp;
          sp.stat = lower(parts[0]);
          if (parts.size() >= 2) sp.var = parts[1];
          statSpecs.push_back(std::move(sp));
        }
      }
    }
    if (statSpecs.empty()) statSpecs.push_back({"freq", std::nullopt});
    // BS-097: statistic/stat are the only `table` options this arm reads.
    // BUG-233: the one copy of this vocabulary now lives in the file-scope
    // registration block (see top of file); this binding keeps every existing
    // use — including the reject net below — reading the same set.
    const std::unordered_set<std::string>& kTableKnownOpts = kVocabTable.known;
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kTableKnownOpts)) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    Datastore* ds = session.datastore();
    for (const auto& sp : statSpecs)
      if (sp.var.has_value() && !ds->has(*sp.var)) {
        blocks.push_back(ojson(eval::engineError("variable " + *sp.var + " not found")));
        return blocks;
      }
    // BUG-042: same if-prefilter as tabulate above (runTable is likewise
    // pure Datastore computation with no cobra --keepif to forward to).
    std::unique_ptr<Datastore> tableIfDs;
    if (parsed.ifExpr.has_value() && !parsed.ifExpr->empty()) {
      std::string tableIfPath = dispatcherNextPath(session.workdir(), "tableif");
      // BUG-369 follow-up: `table x if L.y>0` — see the codebook arm above.
      std::vector<std::string> ifArgs = translate::injectTsContextForExpr(
          {"eval", "--data", ds->path(), "--keepif", *parsed.ifExpr, "--out", tableIfPath},
          *parsed.ifExpr, session);
      try {
        runCobraJson(ifArgs);
      } catch (const CobraError& ce) {
        blocks.push_back(ojson(eval::engineError(cobraErrorMessage(ce), ce.code)));
        return blocks;
      }
      tableIfDs = std::make_unique<Datastore>(tableIfPath);
      ds = tableIfDs.get();
    }
    // BUG-160 CLOSURE: ONE table, not one runTable() call (and one block) per
    // statistic. The single- and two-or-more-statistic requests share the
    // SAME underlying computation (runTableMulti, tabulate.cpp) — Total row/
    // column margins and value-label-aware headers, absent from both paths
    // before this fix — but keep their pre-existing JSON SHAPES: a lone
    // statistic() still renders through runTable's flat `stat`/`cells[row]=
    // value` reshape (every existing one-statistic consumer keeps reading
    // that shape, now with a "Total" entry and real labels), while 2+
    // statistics render through runTableMulti's own nested `stats[]`/
    // `cells[row][stat]=...` shape (previously TWO separate table_result
    // blocks with no shape at all to preserve).
    std::vector<std::pair<std::string, std::optional<std::string>>> multiSpecs;
    multiSpecs.reserve(statSpecs.size());
    for (const auto& sp : statSpecs) multiSpecs.emplace_back(sp.stat, sp.var);
    ojson tableBlk = (multiSpecs.size() == 1)
                         ? ojson(handlers::runTable(*ds, rowvar, colvar, multiSpecs[0].first,
                                                    multiSpecs[0].second, &session))
                         : ojson(handlers::runTableMulti(*ds, rowvar, colvar, multiSpecs,
                                                         &session));
    // BUG-373: table is r-class in Stata and published nothing. r(N) is the
    // tabulated sample's own count, which the block already carries.
    // BUG-1015: and r(N) is ALL it posts (table.sthlp Stored results): r() is
    // replaced and declared, so r(mean) after it reads `.` (sx_table_one).
    if (!tableContents && tableBlk.value("type", std::string()) != "error" &&
        tableBlk.contains("n_obs") && tableBlk["n_obs"].is_number()) {
      eval::replaceRClass(session.eval);
      session.eval.rValues.set("N", tableBlk["n_obs"].get<double>());
      eval::declareRPosted(session.eval, "table", {"N"});
    }
    blocks.push_back(std::move(tableBlk));
    return blocks;
  }

  // ------------------------------------------------------------------
  // M4a: output-completion surface (ROADMAP M4a item 2 — estimation surface)

  if (cmd == "tabstat") {
    // A refused tabstat leaves r() empty. That is judged after the line, by
    // rTableAfterCommand's leaf rule (handlers_aer_estim.cpp), for every tabstat
    // that ended in an error, the refusals raised before this arm included
    // (BUG-1151).
    auto failed = [&](ojson blk) {
      blocks.push_back(std::move(blk));
      return blocks;
    };
    if (parsed.varlist.empty())
      return failed(ojson(eval::engineError("tabstat requires at least one variable")));
    // BUG-1014: tabstat as tabstat.ado (StataNow 19.5, v2.9.6) runs it -- every
    // statistic is `summarize` on the group's rows, the statistic list is parsed
    // by its Stats subprogram, and r() is replaced by the command whether or not
    // `save` is given (w2as_summary_stats_family sx_tabstat_*).
    std::optional<std::string> byVal, statsVal, statisticsVal, columnsVal;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "by" && !v.isBool) byVal = strutil::trim(v.strValue);
      // `stat`/`s` are abbreviations of statistics() (ado: Statistics(str));
      // `stats()` is its own synonym option (ado: STATS(str)).
      if ((k == "stat" || k == "statistics" || k == "s") && !v.isBool) statisticsVal = v.strValue;
      if (k == "stats" && !v.isBool) statsVal = v.strValue;
      if ((k == "columns" || k == "c") && !v.isBool) columnsVal = strutil::trim(v.strValue);
    }
    const std::unordered_set<std::string>& kTabstatKnownOpts = kVocabTabstat.known;
    bool tsNoTotal = false, tsSave = false;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "nototal" && (!v.isBool || v.boolValue)) tsNoTotal = true;
      if (k == "save" && (!v.isBool || v.boolValue)) tsSave = true;
    }
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kTabstatKnownOpts))
      return failed(ojson(*err));
    auto refuse = [&](const std::string& msg, const char* rc) {
      return failed(ojson(eval::errorBlock("syntax_error", msg, std::nullopt, std::nullopt,
                                           std::nullopt, std::string(rc))));
    };
    if (statsVal.has_value() && statisticsVal.has_value())
      return refuse("options statistics() and stats() may not be specified together", "r(198)");
    if (statsVal.has_value()) statisticsVal = statsVal;
    // columns(): a prefix of `variables` or `statistics`, or `stats` (ado). Both
    // layouts leave r(Stat#) statistics-by-variables (sx_tabstat_001, _c_v).
    if (columnsVal.has_value() && !columnsVal->empty()) {
      const std::string c = *columnsVal;
      const bool okCol = std::string("variables").compare(0, c.size(), c) == 0 ||
                         std::string("statistics").compare(0, c.size(), c) == 0 || c == "stats";
      if (!okCol)
        return refuse("option columns(" + c + ") invalid -- specify columns(variables) or "
                      "columns(statistics)", "r(198)");
    }
    // syntax varlist(numeric): a string variable is r(109) (sx_tabstat_ref_str).
    if (Datastore* tds = session.datastore()) {
      for (const auto& v : parsed.varlist)
        if (tds->has(v) && tds->dtype(v) == "factor")
          return refuse("string variables not allowed in varlist; " + v + " is a string variable",
                        "r(109)");
    }
    // The Stats subprogram (parseTabstatStats).
    std::vector<std::string> statKeys, statRows;
    if (const auto bad = parseTabstatStats(statisticsVal, statKeys, statRows))
      return refuse("unknown statistic: " + *bad, "r(198)");
    std::string statCsv;
    for (const auto& k : statKeys) statCsv += (statCsv.empty() ? "" : " ") + k;
    // Weights: [aw=] and [fw=] (ado: [aw fw]).
    std::string weightFlag;
    if (parsed.weightType.has_value() && parsed.weightVar.has_value()) {
      const std::string wt = lower(*parsed.weightType);
      weightFlag = (wt == "aw" || wt == "aweight") ? "--aweight" : "--fweight";
    }
    ojson tsBlock =
        weightFlag.empty()
            ? ojson(handlers::runTabstat(parsed.varlist, byVal, statCsv, session, parsed.ifExpr,
                                         /*wantGroups=*/true, tsNoTotal))
            : ojson(handlers::runTabstatWeighted(parsed.varlist, byVal, statCsv, session,
                                                 parsed.ifExpr, /*wantGroups=*/true, tsNoTotal,
                                                 weightFlag, *parsed.weightVar));
    if (tsBlock.value("type", std::string()) == "error") return failed(tsBlock);
    // The text table for a `log using` transcript (as measured on StataBE).
    {
      std::optional<ojson> tsRefusal;
      std::string tsText = renderTabstatTable(session, parsed, tsBlock, byVal, columnsVal,
                                              tsNoTotal, statKeys, statRows, tsRefusal);
      if (tsRefusal.has_value()) return failed(*tsRefusal);
      session.pendingTabstatLogText = std::move(tsText);
    }
    // The groups were read for the text table; the emitted block carries them
    // only where it always has (save/nototal: runTabstatCore's freeze note).
    if (!(tsSave || tsNoTotal)) tsBlock.erase("groups");
    // r() is tabstat's after every run: empty without `save` (sx_tabstat_unsaved,
    // rowsof(r(StatTotal)) r(109) in Stata), and with `save` r(Stat#)/r(name#)
    // per by() group in the group order and r(StatTotal) unless nototal
    // (pins_oracle_g.log:189-220; sx_tabstat_plain, _s_by, _by_str).
    eval::replaceRClass(session.eval);
    std::vector<std::string> posted;
    if (tsSave) {
      std::vector<std::string> colVars;
      if (tsBlock.contains("vars") && tsBlock["vars"].is_array())
        for (const auto& vn : tsBlock["vars"])
          if (vn.is_string()) colVars.push_back(vn.get<std::string>());
      auto buildStatMatrix = [&](const ojson& statsObj) {
        return tabstatStatMatrix(statsObj, statKeys, statRows, colVars);
      };
      int gi = 0;
      if (tsBlock.contains("groups") && tsBlock["groups"].is_array()) {
        for (const auto& g : tsBlock["groups"]) {
          if (!g.is_object() || !g.contains("stats")) continue;
          ++gi;
          session.eval.rMatrices.emplace_back("Stat" + std::to_string(gi),
                                              buildStatMatrix(g["stats"]));
          // The group's NAME: its value label when the session holds one for
          // this by-variable, a string group's text, and otherwise the value
          // itself with no trailing ".0" (Stata prints `1982`, not `1982.0`).
          std::string nm;
          if (g.contains("value")) {
            if (g["value"].is_string()) {
              nm = g["value"].get<std::string>();
            } else if (g["value"].is_number()) {
              const double gv = g["value"].get<double>();
              bool labelled = false;
              if (byVal.has_value()) {
                auto vlIt = session.valueLabels.find(*byVal);
                if (vlIt != session.valueLabels.end() &&
                    std::fabs(gv - std::nearbyint(gv)) < 1e-9) {
                  auto lit = vlIt->second.find(static_cast<int>(std::nearbyint(gv)));
                  if (lit != vlIt->second.end()) {
                    nm = lit->second;
                    labelled = true;
                  }
                }
              }
              if (!labelled) {
                std::ostringstream os;
                if (std::fabs(gv - std::nearbyint(gv)) < 1e-9) {
                  os << static_cast<long long>(std::nearbyint(gv));
                } else {
                  os << gv;
                }
                nm = os.str();
              }
            }
          }
          session.eval.rStrings.set("name" + std::to_string(gi), nm);
          posted.push_back("Stat" + std::to_string(gi));
          posted.push_back("name" + std::to_string(gi));
        }
      }
      if (!tsNoTotal && tsBlock.contains("total") && tsBlock["total"].is_object()) {
        session.eval.rMatrices.emplace_back("StatTotal", buildStatMatrix(tsBlock["total"]));
        posted.push_back("StatTotal");
      }
    }
    eval::declareRPosted(session.eval, "tabstat", std::move(posted));
    blocks.push_back(tsBlock);
    return blocks;
  }

  // BUG-105 abbreviation stopgap: `cor` (see the `describe`/`d` note above).
  // Deliberately NOT `c`: real Stata's minimum abbreviation for `correlate`
  // is `cor` (`c` alone is ambiguous), and the BUGLOG finding names the
  // 3-letter spelling specifically.
  if (cmd == "corr" || cmd == "cor" || cmd == "pwcorr" || cmd == "correlate") {
    bool pairwise = false, sig = false, obs = false, covar = false;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "pairwise" && v.isBool && v.boolValue) pairwise = true;
      if (k == "sig" && v.isBool && v.boolValue) sig = true;
      if (k == "obs" && v.isBool && v.boolValue) obs = true;
      // BUG-1014: correlate's covariance (abbreviation cov, correlate.sthlp).
      if (k == "covariance" && (!v.isBool || v.boolValue)) covar = true;
    }
    // BS-097: pairwise/sig/obs are the only corr/pwcorr/correlate options
    // this arm reads (real Stata also has means/covariance/kendall/star()/
    // bonferroni/sidak/print()/listwise/casewise, not implemented here).
    // BUG-233: the one copy of this vocabulary now lives in the file-scope
    // registration block (see top of file); this binding keeps every existing
    // use — including the reject net below — reading the same set.
    const std::unordered_set<std::string>& kCorrKnownOpts = kVocabCorr.known;
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kCorrKnownOpts)) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    if (cmd == "pwcorr") pairwise = true;

    // BS-028 (BETA Phase C batch C1): `obs` was previously mismapped
    // straight into `sig` — `correlate, obs` silently became `correlate,
    // sig`, which then hard-errors cobra's own "corr --sig requires
    // --pairwise" instead of real Stata's actual "option obs not allowed"
    // (oracle-pinned: rc=198); `pwcorr, obs` silently became `pwcorr, sig`
    // (showing p-values the user never asked for instead of the per-pair N
    // Stata actually displays). Real Stata: `obs` is a pwcorr-only option —
    // correlate (listwise) already prints its "(obs=N)" banner
    // unconditionally, so an explicit `obs` there is not valid syntax at
    // all, regardless of `pairwise`.
    if (obs && cmd != "pwcorr") {
      blocks.push_back(ojson(eval::engineError("option obs not allowed", "unsupported_option")));
      return blocks;
    }
    // `pwcorr, obs`: cobra's own `corr --pairwise` output already carries a
    // full nobs[i][j] matrix REGARDLESS of --sig (verified against a live
    // cobra run) and cobraToEstimationM4's passthrough already forwards
    // "nobs" — so requesting --pairwise (pwcorr always does, above) is
    // already enough for the block to carry the correct per-pair N data.
    // NOTE (frontend, out of scope — piet/frontend/** is forbidden for this
    // batch): console.js's formatCorrelate() has no code path that reads
    // `block.nobs` to render a per-cell N row for pwcorr (only the
    // `showSig` p-value row exists, gated on `block.sig`) — until a
    // companion frontend change adds one, `pwcorr ..., obs` renders the
    // plain correlation matrix with no extra row. That is still a real
    // improvement over before: `sig` is no longer force-set true by `obs`,
    // so the display no longer shows spurious, un-requested p-values
    // either.
    // covariance is correlate's, not pwcorr's (pwcorr.ado's syntax line has no
    // such option: r(198)).
    if (covar && cmd == "pwcorr") {
      blocks.push_back(ojson(eval::errorBlock("syntax_error", "option covariance not allowed",
                                              std::nullopt, std::nullopt, std::nullopt,
                                              std::string("r(198)"))));
      return blocks;
    }
    std::string corrWeightFlag;
    if (parsed.weightType.has_value() && parsed.weightVar.has_value()) {
      const std::string wt = lower(*parsed.weightType);
      corrWeightFlag = (wt == "aw" || wt == "aweight") ? "--aweight" : "--fweight";
    }
    ojson corrBlock = ojson(handlers::runCorrStata(
        parsed.varlist, pairwise, sig, covar, corrWeightFlag,
        parsed.weightVar.has_value() ? *parsed.weightVar : std::string(), session, parsed.ifExpr,
        parsed.inRange));
    // BS-028 composing 1-liner (BETA campaign C2): record `obs` on the
    // block itself so a future frontend change can decide whether to
    // render the per-pair N matrix (`nobs`, already flowing per the NOTE
    // above) without re-deriving "was `obs` requested" from opts a second
    // time. Only reachable here when cmd=="pwcorr" (the `correlate, obs`
    // case already returned above).
    if (obs && corrBlock.is_object()) {
      corrBlock["obs"] = true;
    }
    // The names Stata posts are declared by runCorrStata (correlate's help;
    // RULING.md D.3.5), per the form actually run.
    blocks.push_back(corrBlock);
    return blocks;
  }

  if (cmd == "ttest") {
    // BUG-1015: runTtest posts r() itself, every name of the form run and no
    // other, from the engine's own `r` object (FU-269 FU-270; w2o_rnames N07 N09
    // N10, w2as_summary_stats_family sx_ttest_*). Before, this arm rebuilt a
    // one-sample set for every form (r(sd) included) from the display block.
    ojson ttestBlk = ojson(handlers::runTtest(parsed, session));
    blocks.push_back(std::move(ttestBlk));
    return blocks;
  }

  // P3 weights wave: `mean`/`proportion` — new registry rows (PweightOnly),
  // routed to the new handlers::runMean/runProportion (forward-declared at
  // the top of this file — see that comment for why they're not in
  // piet/handlers.h). Both eclass in real Stata; e(b)/_b[...] wiring happens
  // inside the handler itself (session.setLastModel), same mechanism
  // regress above uses — no dispatcher-side r()/e() plumbing needed here.
  if (cmd == "mean") {
    // BUG-373: r(title) is the table caption Stata publishes for these two.
    ojson meanBlk = ojson(handlers::runMean(parsed, session));
    if (meanBlk.value("type", std::string()) != "error")
      session.eval.rStrings.set("title", "Mean estimation");
    blocks.push_back(std::move(meanBlk));
    return blocks;
  }
  if (cmd == "proportion") {
    ojson propBlk = ojson(handlers::runProportion(parsed, session));
    if (propBlk.value("type", std::string()) != "error")
      session.eval.rStrings.set("title", "Proportion estimation");
    blocks.push_back(std::move(propBlk));
    return blocks;
  }

  if (cmd == "xtreg") {
    blocks.push_back(ojson(handlers::runXtreg(parsed, session)));
    return blocks;
  }

  if (cmd == "logit" || cmd == "logistic" || cmd == "probit" || cmd == "cloglog") {
    std::string cobraCmd = (cmd == "probit" || cmd == "cloglog") ? cmd : "logit";
    ojson block = ojson(handlers::runLogitProbit(cobraCmd, parsed, session));
    installMlEsampleAfterFit(block, session);
    // BS-022 (BETA Phase C batch C1): `logistic` previously returned the
    // IDENTICAL block as `logit` (estimator "logit", b = log-odds) — real
    // Stata's `logistic` runs the SAME engine call as `logit` but displays
    // odds ratios: OR = exp(b), SE_OR = OR * se(b) (delta method), CI =
    // exp(ci of b); z/p/t are invariant under this monotonic reparam and
    // stay untouched. Only the DISPLAY coeftable is transformed — `coefs`
    // (feeds e(b)/postestimation, e.g. a later `margins`/`lincom`) is left
    // in the log-odds scale untouched, matching Stata's own internal
    // storage (its display-only OR transform never touches e(b)).
    // NOTE: ci_lo/ci_hi (and p) are currently null in this engine's logit
    // coeftable rows (a separate, pre-existing gap outside this batch's
    // scope) — the exp() transform below is a no-op on a null field, so
    // this stays forward-compatible once that gap is closed.
    // NOTE (frontend, out of scope — piet/frontend/** is forbidden for this
    // batch): console.js's formatOneTable() derives the coefficient
    // column's header purely from `block.estimator === 'margins' ? 'dy/dx'
    // : 'Coefficient'` (no other estimator branch) — with no companion
    // frontend change the column header will still literally read
    // "Coefficient" even though the values underneath are now correct odds
    // ratios. estimatorTitle() already has a `logistic` -> "Logistic
    // regression" row (identical to logit's, matching real Stata, which
    // titles both commands the same), so the header banner itself needs no
    // change.
    // BUG-333: `logit ..., or` reports odds ratios. Stata's `logistic` IS
    // `logit, or`, so the transform this file already performs for `logistic`
    // below is exactly the one `or` needs — generalised here instead of copied.
    // e(b) stays in log-odds for both (Stata does the same: `or` is a display
    // metric, `_b[]` is unchanged), which is why only `coeftable` is touched.
    bool wantOr = false;
    for (const auto& [ok, ov] : parsed.opts)
      if (ok == "or" && ov.isBool && ov.boolValue) wantOr = true;
    if ((cmd == "logistic" || wantOr) && block.value("type", "") == "estimation") {
      if (cmd == "logistic") block["estimator"] = "logistic";
      // A metric marker the renderer can key on without knowing the command —
      // stcox/streg already set this key (applyHazardRatioTableM4).
      block["coef_metric"] = "odds_ratio";
      if (block.contains("coeftable") && block["coeftable"].is_array()) {
        for (auto& row : block["coeftable"]) {
          if (!row.contains("b") || row["b"].is_null()) continue;
          double b = row["b"].get<double>();
          double orv = std::exp(b);
          row["b"] = orv;
          if (row.contains("se") && row["se"].is_number()) {
            row["se"] = orv * row["se"].get<double>();
          }
          if (row.contains("ci_lo") && row["ci_lo"].is_number()) {
            row["ci_lo"] = std::exp(row["ci_lo"].get<double>());
          }
          if (row.contains("ci_hi") && row["ci_hi"].is_number()) {
            row["ci_hi"] = std::exp(row["ci_hi"].get<double>());
          }
          // t/p intentionally unchanged (invariant under a monotonic exp() reparam).
        }
      }
    }
    blocks.push_back(block);
    return blocks;
  }

  if (cmd == "poisson" || cmd == "ppmlhdfe" || cmd == "fepois") {
    // BS-073 (BETA D3a): bare `poisson` -> cobra fepois WITHOUT --fe (D0
    // finding: reproduces Stata's poisson exactly); `ppmlhdfe`/cobra's own
    // `fepois` command name (BOTH accepted as typeable command words here,
    // mirroring the feols/reghdfe precedent above of accepting the engine's
    // own internal estimator name as an additional PIET spelling) allow
    // absorb() -> --fe. Real Stata's plain `poisson` has NO absorb() option
    // — runPoisson honest-rejects it there.
    // BUG-1251: ppmlhdfe d's new column is announced in a data_mutation block of
    // its own, after the estimation block (the reghdfe residuals() order).
    ojson dMutation;
    blocks.push_back(ojson(handlers::runPoisson(cmd, parsed, session, &dMutation)));
    if (dMutation.is_object() && !dMutation.empty()) blocks.push_back(dMutation);
    return blocks;
  }

  if (cmd == "nbreg") {
    // BS-074 (BETA D3b): negative-binomial (NB2) regression -> cobra nbreg.
    // NOTE: the engine has a live-verified convergence gap on data whose
    // fitted intercept sits away from zero (including the D3e oracle's own
    // fatalities/cnt data) — see runNbreg's header comment (estimation_m4.cpp)
    // for the full disclosure; this is an engine-side limitation, not a
    // wiring gap.
    blocks.push_back(ojson(handlers::runNbreg(parsed, session)));
    installMlEsampleAfterFit(blocks.back(), session);  // BUG-1214
    return blocks;
  }

  if (cmd == "xtpoisson") {
    // BS-075 (BETA D3c): conditional fixed-effects Poisson MLE, `,fe` ONLY
    // (the engine has no RE/PA xtpoisson — Stata's own default IS
    // random-effects, so the bare/`,re`/`,pa` forms all honest-reject).
    blocks.push_back(ojson(handlers::runXtPoisson(parsed, session)));
    return blocks;
  }

  if (cmd == "xtlogit") {
    // BS-075 (BETA D3c): conditional fixed-effects logit MLE, `,fe` ONLY
    // (same RE/PA-absence rationale as xtpoisson above).
    blocks.push_back(ojson(handlers::runXtLogitFe(parsed, session)));
    return blocks;
  }

  if (cmd == "csdid") {
    blocks.push_back(ojson(handlers::runCsdid(parsed, session)));
    return blocks;
  }

  // ---- Arc F (P3 wave): the seven QE/RD commands. Unlike every neighbouring
  // estimator arm, these handlers return an ARRAY (one `display` block per
  // printed line + one structured block last — the handlers_resample.cpp
  // shape), because the tables are rendered as TEXT here rather than being
  // handed to the frontend as a coeftable. So the arms splice; they must not
  // push_back, which would nest an array inside the block stream. ----
  if (cmd == "rdrobust" || cmd == "rddensity" || cmd == "rdplot" || cmd == "binscatter" ||
      cmd == "eventstudyinteract" || cmd == "did_imputation" || cmd == "sdid") {
    ojson res;
    if (cmd == "rdrobust") res = handlers::runRdrobust(parsed, session);
    else if (cmd == "rddensity") res = handlers::runRddensity(parsed, session);
    else if (cmd == "rdplot") res = handlers::runRdplot(parsed, session);
    else if (cmd == "binscatter") res = handlers::runBinscatter(parsed, session);
    else if (cmd == "eventstudyinteract") res = handlers::runEventstudyinteract(parsed, session);
    else if (cmd == "did_imputation") res = handlers::runDidImputation(parsed, session);
    else res = handlers::runSdid(parsed, session);
    if (cmd == "binscatter") {
      // W4: binscatter's own net (handlers_qe.cpp) has already run and now
      // ACCEPTS the graph drawing keys (kVocabBinscatterGraph unions them in).
      // Accepting them without saying so would be the silent drop this wave
      // exists to close, so the block carries the note.
      if (res.is_array()) {
        for (auto& b : res) graphAcceptedNotApplied(b, "binscatter", parsed.opts, kBinscatterApplied);
      } else {
        graphAcceptedNotApplied(res, "binscatter", parsed.opts, kBinscatterApplied);
      }
    }
    if (res.is_array()) {
      for (const auto& b : res) blocks.push_back(b);
    } else {
      blocks.push_back(res);
    }
    return blocks;
  }

  if (cmd == "drdid") {
    // BS-076 (BETA D3d): the classic 2-period doubly-robust DiD -> the SAME
    // engine csdid machinery as `csdid` above, ONE (g,t) cell — now its OWN
    // handler (previously shared csdid's arm above and unconditionally
    // rejected on missing gvar(), since real drdid syntax has no gvar()
    // option at all). See runDrdid's header comment (estimation_m4.cpp) for
    // how the wiring leaves the estimator unmodified and the live-verified
    // oracle match.
    blocks.push_back(ojson(handlers::runDrdid(parsed, session)));
    return blocks;
  }

  // ------------------------------------------------------------------
  // BETA campaign Phase D batch D4 (time-series wirings): BS-077 newey,
  // BS-078 prais, BS-079 dfuller/pperron, BS-080 wntestq, BS-081 tsfill.
  // Oracle: internal capture
  // d45-oracle-stata.log, sections D4a-D4f.

  if (cmd == "newey") {
    blocks.push_back(ojson(handlers::runNewey(parsed, session)));
    return blocks;
  }

  if (cmd == "prais") {
    blocks.push_back(ojson(handlers::runPrais(parsed, session)));
    return blocks;
  }

  // BUG-373: these three computed their statistics and then published NOTHING.
  // A capture gate cannot see that — it compares what a command PRINTS, and the
  // printing was always right. The 2026-08-31 declared-returns sweep is what
  // measured it, against real StataBE: our dfuller numbers match Stata to every
  // digit it reports, and `display r(p)` still errored.
  if (cmd == "dfuller" || cmd == "pperron") {
    ojson blk = ojson(handlers::runDfullerPperron(cmd, parsed, session));
    if (blk.value("type", std::string()) != "error") {
      auto put = [&](const char* rname, const char* key) {
        if (blk.contains(key) && blk[key].is_number())
          session.eval.rValues.set(rname, blk[key].get<double>());
      };
      put("N", "n_obs");
      put("lags", "lags");
      put("Zt", "Zt");
      put("p", "p");
      put("Zrho", "Zrho");   // pperron only; absent on dfuller, so not set there
      // pperron names its lag count `newey_west_lags` in the block and Stata
      // calls it r(lags); and Stata publishes the p-value under BOTH r(p) and
      // r(pval) for this command. Measured, not assumed — the first wiring pass
      // used the block's own key names and the re-sweep caught both misses.
      put("lags", "newey_west_lags");
      put("pval", "p");
      // Stata publishes the critical values under BOTH spellings.
      for (const auto& pr : {std::make_pair("cv_1", "cv_1"), std::make_pair("cv_5", "cv_5"),
                             std::make_pair("cv_10", "cv_10"), std::make_pair("cv1", "cv_1"),
                             std::make_pair("cv5", "cv_5"), std::make_pair("cv10", "cv_10")})
        put(pr.first, pr.second);
    }
    blocks.push_back(std::move(blk));
    return blocks;
  }

  if (cmd == "wntestq") {
    ojson blk = ojson(handlers::runWntestq(parsed, session));
    if (blk.value("type", std::string()) != "error") {
      if (blk.contains("df") && blk["df"].is_number())
        session.eval.rValues.set("df", blk["df"].get<double>());
      if (blk.contains("p") && blk["p"].is_number())
        session.eval.rValues.set("p", blk["p"].get<double>());
      // Stata calls the Q statistic r(stat).
      if (blk.contains("Q") && blk["Q"].is_number())
        session.eval.rValues.set("stat", blk["Q"].get<double>());
    }
    blocks.push_back(std::move(blk));
    return blocks;
  }

  if (cmd == "tsfill") {
    blocks.push_back(ojson(handlers::runTsfill(parsed, session)));
    return blocks;
  }

  // ------------------------------------------------------------------
  // BETA campaign Phase D batch D5 (post-estimation/diag/misc wirings):
  // BS-085 sdtest, BS-083 hausman, BS-084 chow, BS-087(part) mkspline.
  // (BS-021 durbinalt fix + BS-082 estat endogenous/overid/firststage live
  // inside runEstat, estimation_m4.cpp — no separate dispatch arm needed,
  // both ride the existing `estat` arm below. BS-086 margins ,at() fix
  // lives inside runMargins, same file — no dispatch change needed either.)

  if (cmd == "sdtest") {
    // Real Stata's `sdtest varname [if], by(groupvar)` / `sdtest varname
    // [if] = #` — the `= #` comparison value rides the varlist as a bare
    // `=` token (same reconstruct-then-split idiom `egen`'s arm above uses
    // for `o = fn(expr)`), NOT a parsed.opts entry.
    std::string raw = joinVarlist(parsed.varlist);
    size_t eqPos = raw.find('=');
    std::string varName = trimWs(eqPos == std::string::npos ? raw : raw.substr(0, eqPos));
    std::optional<std::string> againstVal;
    if (eqPos != std::string::npos) againstVal = trimWs(raw.substr(eqPos + 1));
    blocks.push_back(
        ojson(handlers::runSdtest(varName, againstVal, parsed.opts, parsed.ifExpr, session)));
    return blocks;
  }

  if (cmd == "hausman") {
    if (parsed.varlist.size() != 2) {
      blocks.push_back(ojson(eval::engineError(
          "syntax_error: `hausman` requires exactly two stored estimation names, e.g. `hausman "
          "A B`",
          "syntax_error")));
      return blocks;
    }
    blocks.push_back(
        ojson(handlers::runHausman(parsed.varlist[0], parsed.varlist[1], parsed.opts, session)));
    return blocks;
  }

  if (cmd == "chow") {
    blocks.push_back(ojson(handlers::runChow(parsed, session)));
    return blocks;
  }

  if (cmd == "mkspline") {
    // Real Stata's `mkspline stub = varname [if], cubic ...` — same
    // reconstruct-then-split-on-`=` idiom as `sdtest`'s arm above / egen's
    // arm earlier in this function.
    std::string raw = joinVarlist(parsed.varlist);
    size_t eqPos = raw.find('=');
    if (eqPos == std::string::npos) {
      blocks.push_back(ojson(eval::engineError(
          "syntax_error: `mkspline` requires `stub = varname`", "syntax_error")));
      return blocks;
    }
    std::string stub = trimWs(raw.substr(0, eqPos));
    std::string srcVar = trimWs(raw.substr(eqPos + 1));
    blocks.push_back(
        ojson(handlers::runMkspline(stub, srcVar, parsed.opts, parsed.ifExpr, session)));
    return blocks;
  }

  if (cmd == "test") {
    // BS-097 / BUG-333: every option is either read here or refused by name before
    // the handler runs. `accumulate` is IMPLEMENTED (w2c_postest arm
    // test_2c_accumulate) as session state owned by runTestCmd: the previous
    // test's restriction set on the same estimation is kept after each successful
    // test, replaced by a plain `test`, extended by `test ..., accumulate`.
    // `notest`/`mtest`/`coef` stay honest unimplemented-rejects (rc 198).
    std::vector<std::pair<std::string, eval::OptValue>> testOpts = parsed.opts;
    eval::canonicalizeOptionKeys(testOpts, {"accumulate", "notest", "mtest", "coef"});
    if (auto err = eval::rejectUnknownOptions(testOpts, {"accumulate"},
                                              {"notest", "mtest", "coef"})) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    if (auto gone = refuseIfFitDataGone("test", session)) {
      blocks.push_back(std::move(*gone));
      return blocks;
    }
    bool accumulate = false;
    for (const auto& [k, v] : testOpts)
      if (k == "accumulate" && (!v.isBool || v.boolValue)) accumulate = true;
    ojson testBlock = ojson(runPostestKeepingE(  // round 3: e() is left alone
        session, [&] {
          return handlers::runTestCmd(joinVarlist(parsed.varlist), session, accumulate);
        }));
    // `test` is r-class: the r() it writes below replaces all of r(), r(table)
    // included (w2c_postest arm rtable_replaced_by_test), at dispatch()'s exit
    // (handlers::replaceStaleRClass). A failing test writes nothing and leaves r().
    // W:P5 — `test` is r-class (captureRFromBlock Branch 3), same wiring the
    // lincom arm below has had since FIX-RESTIMATE. Without this,
    // `test 1.treated` then `display r(p)` errored with "r(p) is not set".
    eval::captureRFromBlock(testBlock, session.eval);
    if (testBlock.value("type", std::string()) != "error") {
      // BUG-373: Stata publishes r(drop) — whether any constraint was DROPPED.
      // The engine drops exactly one kind: a restriction on omitted coefficients
      // only (`test 0.c` names the base level, i.e. `0 = 0`), which it reports as
      // `dropped`; a redundant restriction still surfaces as an error.
      const bool anyDropped = testBlock.contains("dropped") && testBlock["dropped"].is_number() &&
                              testBlock["dropped"].get<double>() > 0;
      session.eval.rValues.set("drop", anyDropped ? 1.0 : 0.0);
      // The names Stata posts (test's help; RULING.md D.3.9): the F form
      // when the block carries a p_F (captureRFromBlock's own test), else the chi2
      // form; r(dropped_1) when a constraint was dropped.
      // A null F and p_F is the least-squares form on a zero R V R' (an exact
      // `regress`: Stata posts r(F) = r(p) = missing), still the F form.
      const bool fForm = testBlock.contains("p_F") &&
                         (!testBlock["p_F"].is_null() ||
                          (testBlock.contains("F") && testBlock["F"].is_null()));
      std::vector<std::string> stataNames =
          fForm ? std::vector<std::string>{"F", "p", "df", "df_r", "drop"}
                : std::vector<std::string>{"chi2", "p", "df", "drop"};
      // r(dropped_#), one per dropped constraint (captureRFromBlock), BUG-1276.
      const int nDropped = anyDropped ? testBlock["dropped"].get<int>() : 0;
      for (int k = 1; k <= nDropped; ++k) stataNames.push_back("dropped_" + std::to_string(k));
      eval::declareRPosted(session.eval, "test", std::move(stataNames));
    }
    blocks.push_back(std::move(testBlock));
    return blocks;
  }

  // BUG-371 — decode / inspect / ds / unab.
  if (cmd == "decode") {
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabDecode.known,
                                              kVocabDecode.unimplemented)) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    if (parsed.varlist.size() != 1) {
      blocks.push_back(ojson(eval::engineError(
          "syntax_error: `decode` requires `decode varname, generate(newvar)`", "syntax_error")));
      return blocks;
    }
    std::optional<std::string> genVar;
    // BUG-1096: maxlength(#) keeps the whole characters of each label that fit in
    // # bytes, 1 to 32000 (the default), and caps the new str# (runDecode). StataBE
    // 19.5: `maxlength(3)` makes str3 "alp"; `maxlength(0)`/`(32001)` are
    // "maxlength must be between 1 and 32000", a non-integer, an empty one or the
    // bare word "option maxlength() incorrectly specified" (bare: "option
    // maxlength incorrectly specified"), and a second one "option maxlength() not
    // allowed", each r(198).
    size_t maxLength = 32000;
    bool sawMaxLength = false;
    auto r198 = [&](const std::string& msg) {
      blocks.push_back(ojson(eval::errorBlock("syntax_error", msg, std::nullopt, std::nullopt,
                                              std::nullopt, "r(198)")));
      return blocks;
    };
    for (const auto& [k, v] : parsed.opts) {
      if (k == "generate" && !v.isBool) genVar = v.strValue;
      if (k != "maxlength") continue;
      if (sawMaxLength) return r198("option maxlength() not allowed");
      sawMaxLength = true;
      if (v.isBool) return r198("option maxlength incorrectly specified");
      const std::optional<double> n = eval::pythonFloat(trimWs(v.strValue));
      if (!n.has_value() || std::isnan(*n) || *n != std::floor(*n))
        return r198("option maxlength() incorrectly specified");
      if (*n < 1 || *n > 32000) return r198("maxlength must be between 1 and 32000");
      maxLength = static_cast<size_t>(*n);
    }
    blocks.push_back(ojson(handlers::runDecode(parsed.varlist[0], genVar, session, maxLength)));
    return blocks;
  }

  if (cmd == "xtsum") {
    if (auto err = eval::rejectUnknownOptions(parsed.opts, {})) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    blocks.push_back(ojson(handlers::runXtsum(parsed.varlist, session)));
    return blocks;
  }

  if (cmd == "xttab") {
    if (auto err = eval::rejectUnknownOptions(parsed.opts, {})) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    blocks.push_back(ojson(handlers::runXttab(parsed.varlist, session)));
    return blocks;
  }

  if (cmd == "inspect") {
    if (auto err = eval::rejectUnknownOptions(parsed.opts, {})) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    ojson blk = ojson(handlers::runInspect(parsed.varlist, session));
    // BUG-373: inspect is r-class in Stata and mine published nothing — a command
    // I wrote the day before, computing every one of these and dropping them.
    // Exactly the class the declared-returns sweep exists to find.
    if (blk.value("type", std::string()) != "error" && blk.contains("inspect")) {
      static const std::vector<std::string> kInspectR = {"N",        "N_neg",    "N_0",
                                                         "N_pos",    "N_negint", "N_posint",
                                                         "N_unique", "N_undoc"};
      for (const auto& k : kInspectR) {
        if (!blk["inspect"].contains(k)) continue;
        const auto& v = blk["inspect"][k];
        // a missing count (N_unique past 99, N_undoc without a value label) is Stata's `.`
        session.eval.rValues.set(k, v.is_number() ? v.get<double>() : std::nan(""));
      }
      eval::declareRPosted(session.eval, "inspect", kInspectR);  // BUG-1453: Stata's whole set
    }
    blocks.push_back(std::move(blk));
    return blocks;
  }

  if (cmd == "ds") {
    // Stata's ds has a large filter vocabulary (has()/alpha/varwidth/detail);
    // most remain unimplemented and refused BY NAME rather than ignored — a
    // dropped filter would silently list the WRONG variables, which is worse
    // than an error. B5-D7: `not` is now accepted (kVocabDs.known) and
    // implemented as the complement of the varlist against every variable in
    // the dataset — runKeep's own complement idiom (handlers_m3.cpp). Measured
    // B4REC|ds|not|0| (tests/eval/golden/aer_b4/pins_oracle_r.log:163-164).
    // Bound to the REGISTERED vocabulary below (kVocabDs), not an inline
    // literal set — this arm used to carry its own duplicate copy of
    // kVocabDs.known/unimplemented, so the census and the arm could drift
    // apart; fixed here.
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabDs.known, kVocabDs.unimplemented)) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    bool dsNot = false;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "not" && v.isBool && v.boolValue) dsNot = true;
    }
    auto dsAllNames = [&](ojson* errOut) -> std::optional<std::vector<std::string>> {
      if (const std::vector<VarInfo>* card = session.residentReadCard()) {
        std::vector<std::string> out;
        for (const auto& vi : *card) out.push_back(vi.name);
        return out;
      }
      Datastore* dsp = session.datastore();
      if (dsp == nullptr) {
        *errOut = ojson(eval::noDataLoaded());
        return std::nullopt;
      }
      return dsp->names();
    };
    std::vector<std::string> names;
    if (parsed.varlist.empty()) {
      ojson dsErr;
      auto all = dsAllNames(&dsErr);
      if (!all.has_value()) {
        blocks.push_back(dsErr);
        return blocks;
      }
      // `ds, not` with no varlist has nothing to complement against -- same as
      // plain `ds` (every variable).
      names = *all;
    } else {
      auto vr = piet::loops::expandVarlist(joinVarlist(parsed.varlist), session);
      if (vr.isError) {
        blocks.push_back(vr.error);
        return blocks;
      }
      if (!dsNot) {
        names = vr.tokens;
      } else {
        ojson dsErr;
        auto all = dsAllNames(&dsErr);
        if (!all.has_value()) {
          blocks.push_back(dsErr);
          return blocks;
        }
        // Complement, preserving dataset column order (matches runKeep).
        for (const auto& n : *all) {
          if (std::find(vr.tokens.begin(), vr.tokens.end(), n) == vr.tokens.end()) {
            names.push_back(n);
          }
        }
      }
    }
    std::string joined;
    for (size_t i = 0; i < names.size(); ++i) {
      if (i) joined += " ";
      joined += names[i];
    }
    // r(varlist) is a STRING r-class result. captureRFromBlock has no string
    // branch (it only ever rebuilds the numeric store), so this is set directly —
    // the same one-liner `levelsof` uses for r(levels).
    session.eval.rStrings.set("varlist", joined);
    // BUG-1082: no `_rc` write here. Stata's _rc is sticky -- only an error or
    // `capture` sets it -- so `capture noisily renpfix tt u` (r(110)) then
    // `quietly ds` still reads 110 (w2ax_t1_official.log renpfix_collide).
    // BUG-1490: r(varlist) is ds's whole r() (StataBE 19.5, rlife/side2.do: `ds`
    // and `ds x*` leave r(macros) `varlist`, no scalar, no matrix).
    eval::declareRPosted(session.eval, "ds", {"varlist"});
    ojson dsOut;
    dsOut["type"] = "display";
    dsOut["text"] = joined;
    blocks.push_back(std::move(dsOut));
    return blocks;
  }

  if (cmd == "unab") {
    // `unab lname : varlist` — a colon form, split on the first UNQUOTED colon the
    // same way the `frame NAME: CMD` arm does.
    std::string rest = trimWs(parsed.raw);
    {
      size_t sp = rest.find_first_of(" \t");
      rest = (sp == std::string::npos) ? std::string() : trimWs(rest.substr(sp + 1));
    }
    static const std::regex reUnabColon(R"(:)");
    std::smatch cm;
    if (!eval::searchUnquoted(rest, reUnabColon, cm)) {
      blocks.push_back(ojson(eval::engineError(
          "syntax_error: `unab` requires `unab lname : varlist`", "syntax_error")));
      return blocks;
    }
    std::string lname = trimWs(rest.substr(0, static_cast<size_t>(cm.position())));
    std::string spec = trimWs(rest.substr(static_cast<size_t>(cm.position()) + 1));
    if (lname.empty() || spec.empty()) {
      blocks.push_back(ojson(eval::engineError(
          "syntax_error: `unab` requires `unab lname : varlist`", "syntax_error")));
      return blocks;
    }
    auto vr = piet::loops::expandVarlist(spec, session);
    if (vr.isError) {
      blocks.push_back(vr.error);
      return blocks;
    }
    // Stata's unab errors when a name does not exist. expandVarlist only proves
    // that for a WILDCARD (a bare unknown name passes straight through), so the
    // literal names are checked here too rather than defining a local that names
    // a column the dataset does not have.
    //
    // The card is the cheap source but it is not always live, and "no card" must
    // NOT mean "skip the check" — that is how an unchecked name slips through
    // (caught by this lock's own (c) case, which failed until the datastore
    // fallback below was added).
    {
      std::vector<std::string> known;
      if (const std::vector<VarInfo>* card = session.residentReadCard()) {
        for (const auto& vi : *card) known.push_back(vi.name);
      } else if (Datastore* udsp = session.datastore()) {
        known = udsp->names();
      } else {
        blocks.push_back(ojson(eval::noDataLoaded()));
        return blocks;
      }
      for (const auto& t : vr.tokens) {
        if (std::find(known.begin(), known.end(), t) == known.end()) {
          blocks.push_back(
              ojson(eval::engineError("variable " + t + " not found", "unknown_variable")));
          return blocks;
        }
      }
    }
    std::string joined;
    for (size_t i = 0; i < vr.tokens.size(); ++i) {
      if (i) joined += " ";
      joined += vr.tokens[i];
    }
    session.eval.locals.set(lname, joined);
    // BUG-1082: and no `_rc` write -- a successful unab leaves _rc as it was.
    return blocks;  // Stata's unab prints nothing; it defines the local.
  }

  // BUG-370 — see registry.cpp for why these three exist and what they refuse.
  // All follow the mkspline arm's `stub = var` split (joinVarlist -> find('=')).
  if (cmd == "xtile") {
    if (auto err = eval::rejectUnknownOptions(parsed.opts, {"nquantiles", "cutpoints"},
                                              {"altdef"})) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    std::string raw = joinVarlist(parsed.varlist);
    size_t eqPos = raw.find('=');
    if (eqPos == std::string::npos) {
      // `xtile if = x`: the `if` opens the qualifier, so no name is left at all.
      if (trimWs(raw).empty()) {
        blocks.push_back(ojson(eval::errorBlock("syntax_error", "varlist required", std::nullopt,
                                                std::nullopt, std::nullopt, "r(100)")));
        return blocks;
      }
      blocks.push_back(ojson(eval::engineError(
          "syntax_error: `xtile` requires `newvar = varname`", "syntax_error")));
      return blocks;
    }
    // BUG-1015: `exp` as typed, for the variable label: the text after `=` up to
    // the qualifier, the weight or the comma, a blank before it kept (Stata's
    // syntax `=/exp`; sx_xtile_n_if "3 quantiles of y ").
    std::string expText = trimWs(raw.substr(eqPos + 1));
    {
      const std::string& line = parsed.raw;
      const size_t e = line.find('=');
      if (e != std::string::npos) {
        size_t b = e + 1;
        while (b < line.size() && std::isspace(static_cast<unsigned char>(line[b]))) ++b;
        size_t stop = line.size();
        int depth = 0;
        bool inStr = false;
        for (size_t i = b; i < line.size(); ++i) {
          const char ch = line[i];
          if (ch == '"') inStr = !inStr;
          if (inStr) continue;
          if (ch == '(') ++depth;
          if (ch == ')') --depth;
          if (depth > 0) continue;
          if (ch == ',' || ch == '[') { stop = i; break; }
          if (i > b && std::isspace(static_cast<unsigned char>(line[i - 1])) &&
              (line.compare(i, 2, "if") == 0 || line.compare(i, 2, "in") == 0) &&
              (i + 2 == line.size() || std::isspace(static_cast<unsigned char>(line[i + 2])))) {
            stop = i;
            break;
          }
        }
        expText = line.substr(b, stop - b);
      }
    }
    std::optional<std::string> xtWeight;
    if (parsed.weightType.has_value() && parsed.weightVar.has_value()) xtWeight = parsed.weightVar;
    blocks.push_back(ojson(handlers::runXtileStata(trimWs(raw.substr(0, eqPos)), expText,
                                                   parsed.opts, parsed.ifExpr, parsed.inRange,
                                                   xtWeight, session, parsed.weightType)));
    return blocks;
  }

  if (cmd == "ipolate") {
    if (auto err = eval::rejectUnknownOptions(parsed.opts, {"generate", "epolate", "by"}, {})) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    blocks.push_back(
        ojson(handlers::runIpolate(parsed.varlist, parsed.opts, parsed.ifExpr, session)));
    return blocks;
  }

  if (cmd == "tssmooth") {
    // Two-word command, registered under the first word: the sub-verb arrives as
    // varlist[0] (the duplicates/estat convention).
    std::string sub = parsed.varlist.empty() ? std::string() : lower(parsed.varlist[0]);
    if (sub != "ma") {
      blocks.push_back(ojson(eval::engineError(
          sub.empty()
              ? std::string("syntax_error: `tssmooth` requires a sub-command; this build "
                            "implements `tssmooth ma newvar = varname, window(#l #c #f)`")
              : "`tssmooth " + sub +
                    "` is not implemented in this build (only `ma`; Stata's exponential, "
                    "dexponential, hwinters, shwinters and nl smoothers are not available).",
          sub.empty() ? "syntax_error" : "unimplemented")));
      return blocks;
    }
    if (auto err = eval::rejectUnknownOptions(parsed.opts, {"window"}, {"replace"})) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    std::string raw =
        joinVarlist(std::vector<std::string>(parsed.varlist.begin() + 1, parsed.varlist.end()));
    size_t eqPos = raw.find('=');
    if (eqPos == std::string::npos) {
      blocks.push_back(ojson(eval::engineError(
          "syntax_error: `tssmooth ma` requires `newvar = varname`", "syntax_error")));
      return blocks;
    }
    blocks.push_back(ojson(handlers::runTssmoothMa(trimWs(raw.substr(0, eqPos)),
                                                   trimWs(raw.substr(eqPos + 1)), parsed.opts,
                                                   session)));
    return blocks;
  }

  if (cmd == "testparm") {
    // BUG-367: `testparm x1 x2` / `testparm i.year` / `testparm x*`. Stata's own
    // options (equal, equation(), nosvyadjust, ...) are NOT implemented, so they
    // are refused by name rather than silently dropped — the same discipline the
    // `test` arm above adopted under BUG-333.
    if (auto err = eval::rejectUnknownOptions(parsed.opts, {},
                                              {"equal", "equation", "nosvyadjust", "df"})) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    if (auto gone = refuseIfFitDataGone("testparm", session)) {
      blocks.push_back(std::move(*gone));
      return blocks;
    }
    ojson tpBlock = ojson(runPostestKeepingE(  // round 3: e() is left alone
        session, [&] { return handlers::runTestparm(parsed.varlist, session); }));
    // r-class, exactly as `test` is (W:P5 captureRFromBlock Branch 3) — a
    // `testparm` followed by `display r(p)` has to see the same r() the test
    // arm fills, or the two spellings of one command would disagree.
    eval::captureRFromBlock(tpBlock, session.eval);
    if (tpBlock.value("type", std::string()) != "error") {
      // r(drop) exactly as the test arm posts it: "test and testparm store" the
      // same results (test's help); pinned 0 at w2h_postest_refit.log:173.
      const bool anyDropped = tpBlock.contains("dropped") && tpBlock["dropped"].is_number() &&
                              tpBlock["dropped"].get<double>() > 0;
      session.eval.rValues.set("drop", anyDropped ? 1.0 : 0.0);
      // The test set (RULING.md D.3.10).
      const bool fForm = tpBlock.contains("p_F") &&
                         (!tpBlock["p_F"].is_null() ||
                          (tpBlock.contains("F") && tpBlock["F"].is_null()));
      std::vector<std::string> stataNames =
          fForm ? std::vector<std::string>{"F", "p", "df", "df_r", "drop"}
                : std::vector<std::string>{"chi2", "p", "df", "drop"};
      const int nDropped = anyDropped ? tpBlock["dropped"].get<int>() : 0;  // as the test arm
      for (int k = 1; k <= nDropped; ++k) stataNames.push_back("dropped_" + std::to_string(k));
      eval::declareRPosted(session.eval, "testparm", std::move(stataNames));
    }
    blocks.push_back(std::move(tpBlock));
    return blocks;
  }

  if (cmd == "lincom") {
    // BUG-333: level() and the eform family are now IMPLEMENTED (engine --level
    // landed this wave; eform is a display metric applied to the block). The
    // BS-097 note below described the pre-fix state, when they were silently
    // discarded and then honestly rejected.
    if (auto err = eval::rejectUnknownOptions(
            parsed.opts, {"level", "eform", "or", "hr", "irr", "rrr", "shr", "coef"}, {"post"})) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    // FIX-RESTIMATE: lincom is r-class — populate r(estimate)/r(se)/r(t)/r(p)/
    // r(lb)/r(ub)/r(df) from its coeftable (captureRFromBlock Branch 2). This
    // was wired only for summarize before, so `lincom` after any estimation
    // (reghdfe/regress/…) left r() empty and every downstream `r(estimate)`
    // read errored. Error blocks (no prior estimation) don't match Branch 2.
    if (auto gone = refuseIfFitDataGone("lincom", session)) {
      blocks.push_back(std::move(*gone));
      return blocks;
    }
    // Without level(), lincom's interval is at `set level` (w2o_rnames.log:111
    // N03: set level 90 -> r(level) 90 with the 90% r(lb)/r(ub)).
    auto lincomOpts = parsed.opts;
    {
      bool hasLevel = false;
      for (const auto& [ok, ov] : lincomOpts)
        if (ok == "level") hasLevel = true;
      if (!hasLevel && session.level != 95) {
        eval::OptValue lv;
        lv.isBool = false;
        lv.strValue = std::to_string(session.level);
        lincomOpts.emplace_back("level", lv);
      }
    }
    ojson lincomBlock = ojson(runPostestKeepingE(session, [&] {  // round 3: e() is left alone
      return handlers::runLincom(joinVarlist(parsed.varlist), session, lincomOpts);
    }));
    eval::captureRFromBlock(lincomBlock, session.eval);
    if (lincomBlock.value("type", std::string()) != "error") {
      // r(level) (lincom's help; pinned 95 at w2h_postest_refit.log:191;
      // RULING.md D.3.7): level()'s value, the nlcom arm's own reading; without
      // level() only at 95, because runLincom does not forward `set level`, so
      // its interval is at 95 and posting c(level) would contradict r(lb)/r(ub).
      // Otherwise nothing is posted and the read refuses (FU-c).
      std::optional<double> lcLevel;
      bool levelGiven = false;
      for (const auto& [ok, ov] : parsed.opts)
        if (ok == "level" && !ov.isBool) {
          levelGiven = true;
          lcLevel = std::strtod(ov.strValue.c_str(), nullptr);
        }
      if (!levelGiven) lcLevel = static_cast<double>(session.level);
      if (lcLevel.has_value()) session.eval.rValues.set("level", *lcLevel);
      // The names Stata posts (lincom's stored results): r(t) after a t-reference fit
      // (the block's df is positive), else r(z); and r(df) only when the reference
      // has degrees of freedom (a normal-reference lincom has none),
      // so after a normal-reference combination r(df) is outside the set and reads
      // `.` (w2s_multiway_class.log:840 mw_iv_c2_lincom, w2o_rnames.log:125 N04).
      const bool tRef = lincomBlock.contains("df") && lincomBlock["df"].is_number() &&
                        lincomBlock["df"].get<double>() > 0;
      // The coefficient table's statistic is captured as r(t) (captureRFromBlock
      // Branch 2); under a normal reference lincom.ado posts it as r(z).
      if (!tRef && session.eval.rValues.contains("t") && !session.eval.rValues.contains("z")) {
        eval::OrderedFloatDict renamed;
        for (const auto& [k, v] : session.eval.rValues.items()) renamed.set(k == "t" ? "z" : k, v);
        session.eval.rValues = renamed;
      }
      std::vector<std::string> lincomNames = {"estimate", "se", tRef ? "t" : "z", "p", "lb", "ub",
                                              "level"};
      if (tRef) lincomNames.insert(lincomNames.begin() + 2, "df");
      eval::declareRPosted(session.eval, "lincom", std::move(lincomNames));
    }
    blocks.push_back(std::move(lincomBlock));
    return blocks;
  }

  if (cmd == "nlcom") {
    // BUG-333: level() implemented (engine --level landed this wave).
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabNlcom.known, kVocabNlcom.unimplemented)) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    // FIX-RESTIMATE: nlcom is r-class too (same captureRFromBlock Branch 2).
    if (auto gone = refuseIfFitDataGone("nlcom", session)) {
      blocks.push_back(std::move(*gone));
      return blocks;
    }
    // round 3: without `post` nlcom leaves e() alone; `post` installs a new e()
    // (nlcom.sthlp), so only then is it not wrapped. Read as runNlcom reads it.
    bool nlcomPost = false;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "post" && (!v.isBool || v.boolValue)) nlcomPost = true;
    }
    auto nlcomRun = [&] {
      return handlers::runNlcom(joinVarlist(parsed.varlist), session, parsed.opts);
    };
    ojson nlcomBlock = ojson(nlcomPost ? nlcomRun() : runPostestKeepingE(session, nlcomRun));
    eval::captureRFromBlock(nlcomBlock, session.eval);
    // BUG-373: captureRFromBlock fills the estimate/se/t/p family; r(N) is the
    // fit's own observation count and was not among them.
    if (nlcomBlock.value("type", std::string()) != "error" && nlcomBlock.contains("n_obs") &&
        nlcomBlock["n_obs"].is_number())
      session.eval.rValues.set("N", nlcomBlock["n_obs"].get<double>());
    // w2c_postest family 5: nlcom is r-class and posts r(b) (1 x k, the
    // combinations) and r(table) (9 x k, normal-referenced) — arms nlcom_ratio /
    // nlcom_label_only read r(b)[1,1], nlcom_multi_nopost_rtable reads a 9 x 3
    // r(table). Replaces the r-matrices wholesale, as any r-class command does.
    if (nlcomBlock.value("type", std::string()) != "error") {
      session.eval.rMatrices.clear();
      eval::EvalMatrix rb;
      // Stata names the one row `c1` (StataBE 19.5, BUG-1504: rownames r(b)
      // `c1` for `nlcom _b[x1]/_b[x2]`, a named and a two-term call alike).
      rb.rownames = {"c1"};
      rb.data.emplace_back();
      if (nlcomBlock.contains("coeftable") && nlcomBlock["coeftable"].is_array()) {
        for (const auto& row : nlcomBlock["coeftable"]) {
          rb.colnames.push_back(row.value("name", std::string()));
          rb.data[0].push_back((row.contains("b") && row["b"].is_number())
                                   ? row["b"].get<double>()
                                   : std::numeric_limits<double>::quiet_NaN());
        }
      }
      // FU-410: r(V), the combinations' delta-method covariance G V G' (the
      // engine's, runNlcom's `nlcom_V`), named like r(b). w2p_estim_ev.log:1502
      // G_c_nlcom el(r(V),1,1) .0129450049474129 = the table se squared.
      if (nlcomBlock.contains("nlcom_V") && nlcomBlock["nlcom_V"].is_array() &&
          nlcomBlock["nlcom_V"].size() == rb.colnames.size()) {
        eval::EvalMatrix rv;
        rv.rownames = rb.colnames;
        rv.colnames = rb.colnames;
        bool shapeOk = true;
        for (const auto& vrow : nlcomBlock["nlcom_V"]) {
          if (!vrow.is_array() || vrow.size() != rb.colnames.size()) {
            shapeOk = false;
            break;
          }
          std::vector<double> r;
          for (const auto& x : vrow)
            r.push_back(x.is_number() ? x.get<double>() : std::numeric_limits<double>::quiet_NaN());
          rv.data.push_back(std::move(r));
        }
        if (shapeOk) session.eval.rMatrices.emplace_back("V", std::move(rv));
      }
      session.eval.rMatrices.emplace_back("b", std::move(rb));
      // level(#) was validated by the engine run above; absent = `set level`.
      double nlLevel = static_cast<double>(session.level);
      for (const auto& [ok, ov] : parsed.opts)
        if (ok == "level" && !ov.isBool) nlLevel = std::strtod(ov.strValue.c_str(), nullptr);
      handlers::postRTableFromBlock(session, nlcomBlock, nlLevel);
      // The names Stata posts (nlcom's help; measured): r(N), r(b),
      // r(V), r(table), r(level); r(df_r) only under df(), and none of lincom's
      // estimate/se family (w2o_rnames.log:340 N20 `60|.|.|.|95|`).
      for (const char* k : {"estimate", "se", "t", "z", "p", "lb", "ub", "df", "df_r"})
        session.eval.rValues.erase(k);
      session.eval.rValues.set("level", nlLevel);
      eval::declareRPosted(session.eval, "nlcom", {"N", "b", "V", "table", "level"});
    }
    if (nlcomBlock.is_object()) nlcomBlock.erase("nlcom_V");
    blocks.push_back(std::move(nlcomBlock));
    return blocks;
  }

  if (cmd == "margins") {
    // X2-GLM: after a glm the engine's margins would answer a DIFFERENT model --
    // runMargins sends no --model for cobraCmd "glm", and cmd_margins then
    // defaults to a logit AME. The glm's own delta-method margins answer instead.
    if (session.lastModel().has_value() && session.lastModel()->cobraCmd == "glm") {
      ojson r_gm = handlers::runGlmMargins(parsed, session);
      if (r_gm.is_array()) {
        for (const auto& b : r_gm) blocks.push_back(b);
      } else if (!r_gm.is_null()) {
        blocks.push_back(r_gm);
      }
      return blocks;
    }
    // qreg2 (handlers_e3_qreg2.cpp): no engine refit exists for it; its linear-index
    // dy/dx comes from the posted e(b)/e(V) (w2j_e3_qreg2.log M01/M08).
    if (session.lastModel().has_value() && session.lastModel()->cobraCmd == "qreg2") {
      ojson r_qm = handlers::runQreg2Margins(parsed, session);
      if (r_qm.is_array()) {
        for (const auto& b : r_qm) blocks.push_back(b);
      } else if (!r_qm.is_null()) {
        blocks.push_back(r_qm);
      }
      return blocks;
    }
    // Stata's margins takes `if` to choose the rows it averages over;
    // runMargins never read it (runGlmMargins above refuses it itself), so
    // every form answered for the full e(sample) at rc 0 (`margins if t==1`
    // gave the all-rows 12.673333, where the t==1 rows average 11.913333).
    // Refused by name until a path restricts the
    // averaging sample -- forwarding it as --keepif would refit on the subset,
    // a different model. The registry row's allowIf=false is descriptive only
    // (no enforcement site reads it), so the refusal lives here; `in` and
    // weights are already refused by the registry's allowInRange/WeightPolicy
    // gates before this arm.
    if (parsed.ifExpr.has_value() && !parsed.ifExpr->empty()) {
      blocks.push_back(ojson(eval::errorBlock(
          "unsupported_option",
          "margins with an if qualifier (averaging over a subset of the estimation sample) is "
          "not implemented in this version of PIET; nothing was computed.",
          std::nullopt, std::nullopt, std::nullopt, "r(198)")));
      return blocks;
    }
    // FU-94: the marginlist is the positional terms. Passing the options alone
    // dropped it, so `margins t` answered what bare `margins` answers, at rc 0.
    blocks.push_back(ojson(handlers::runMargins(parsed.varlist, parsed.opts, session)));
    return blocks;
  }

  if (cmd == "lassocoef") {
    blocks.push_back(handlers::runLassoCoef(parsed, session));
    return blocks;
  }

  if (cmd == "predict") {
    // Only cached lasso predictions implement in-range filtering. Preserve
    // the tokenizer's previous explicit refusal for every generic path.
    if (parsed.inRange.has_value() &&
        !(session.lastModel().has_value() && session.lastModel()->stataCmd == "lasso")) {
      blocks.push_back(ojson(eval::unsupportedOption("in " + *parsed.inRange,
          "`" + cmdTyped + "` does not support the `in` row-range qualifier in this engine. "
          "Use `if` to subset rows by a condition (maps to --keepif).")));
      return blocks;
    }
    // BUG-517: the `if` qualifier reaches the handler (it was parsed and then
    // dropped here, so `predict p if x` filled every row). c() refs resolve the
    // way gen's `if` does; est refs were already resolved at the parse point.
    std::optional<std::string> predictIf = parsed.ifExpr;
    if (predictIf.has_value()) predictIf = substituteCRefs(*predictIf, session);
    ojson predictBlk;
    if (session.lastModel().has_value() && session.lastModel()->stataCmd == "lasso") {
      auto lassoParsed = parsed;
      lassoParsed.ifExpr = predictIf;
      predictBlk = handlers::runLassoPredict(lassoParsed, session);
    } else predictBlk = handlers::runPredict(parsed.varlist, parsed.opts, session, predictIf);
    // A refused predict leaves r() alone (StataBE: r(N) 38 survives `predict pd, d`
    // r(198)). The w2b_estim.log:370 arm reads r(sum) `.` because its `drop pv`
    // replaced r() (BUG-1417), which an earlier rule here ("C11") misread as the
    // refused predict's doing.
    blocks.push_back(std::move(predictBlk));
    return blocks;
  }

  if (cmd == "estat") {
    std::string sub = parsed.varlist.empty() ? std::string() : lower(parsed.varlist[0]);
    std::vector<std::string> after(parsed.varlist.empty() ? parsed.varlist.begin()
                                                            : parsed.varlist.begin() + 1,
                                    parsed.varlist.end());
    // round 3: estat leaves e() alone unless `post` installs a new one (read as
    // runEstat reads it), so only the post form is not wrapped.
    bool estatPost = false;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "post") estatPost = true;
    }
    auto estatRun = [&] {
      return handlers::runEstat(sub, after, parsed.opts, parsed.ifExpr, session);
    };
    ojson estatBlk = ojson(estatPost ? estatRun() : runPostestKeepingE(session, estatRun));
    // BUG-373: `estat vif` is r-class in Stata — r(name_K)/r(vif_K), one pair per
    // regressor, 1-based in coeftable order. The values were computed and printed
    // and never published.
    if (sub == "vif" && estatBlk.value("type", std::string()) != "error" &&
        estatBlk.contains("coeftable") && estatBlk["coeftable"].is_array()) {
      int k = 0;
      std::vector<std::string> vifNames;
      for (const auto& row : estatBlk["coeftable"]) {
        ++k;
        const std::string nm = row.value("name", std::string());
        if (!nm.empty()) session.eval.rStrings.set("name_" + std::to_string(k), nm);
        if (row.contains("b") && row["b"].is_number())
          session.eval.rValues.set("vif_" + std::to_string(k), row["b"].get<double>());
        vifNames.push_back("name_" + std::to_string(k));
        vifNames.push_back("vif_" + std::to_string(k));
      }
      // BUG-1496: those pairs are vif's whole r() (StataBE 19.5, rlife/ts1.do: `:
      // r(scalars)` `vif_2 vif_1`, `: r(macros)` `name_2 name_1`).
      eval::declareRPosted(session.eval, "estat", std::move(vifNames));
    }
    blocks.push_back(std::move(estatBlk));
    return blocks;
  }

  // ------------------------------------------------------------------
  // M4a item 3: stored estimates + tables (ROADMAP M4a)

  if (cmd == "eststo" || cmd == "est" || cmd == "estimates") {
    // `estimates table` (P4 wave) is the ONE sub-form that carries options, so
    // it is routed BEFORE the empty-option net below — that net is what turned
    // `estimates table m1 m2, b se` into "option `b` not allowed".
    // W2 esttabnames: estimates.ado's own floors (`bsubstr(SUB, 1, max(N, len))`)
    // -- `table` 3, `store`/`restore` 3, `dir` 3 (full word). `est sto m1` and
    // `estimates sto m1` were r(199) "subcommand not supported" on main while
    // StataBE answers rc 0 (w2_cmdtriage.log section C est_sto/estimates_sto).
    auto estSubIs = [](const std::string& typed, const std::string& full, size_t floor) {
      return typed.size() >= floor && typed.size() <= full.size() &&
             full.compare(0, typed.size(), typed) == 0;
    };
    if ((cmd == "est" || cmd == "estimates") && !parsed.varlist.empty() &&
        estSubIs(lower(parsed.varlist[0]), "table", 3)) {
      std::vector<std::string> tabNames(parsed.varlist.begin() + 1, parsed.varlist.end());
      blocks.push_back(ojson(handlers::runEstimatesTable(tabNames, parsed.opts, session)));
      return blocks;
    }
    // BS-097 as amended by B4-W3b. The net used to be ONE empty set applied
    // here, above the sub-verb split, on the reasoning that no sub-form read
    // parsed.opts. Two now do — `store`'s `title()` and `save`'s `replace` —
    // and a flat union of their vocabularies would accept `estimates store m1,
    // replace`, which Stata rejects. So the net moved INSIDE each sub-verb and
    // every arm below names its own known set; the sets stay empty where the
    // handler still takes only name/sub + session.
    // (`title("...")`'s quote stripping is dequoteEstTitle, above dispatchImpl --
    // shared with the eststo prefix arm's head.)
    auto rejectFor = [&](const std::unordered_set<std::string>& known) -> bool {
      if (auto err = eval::rejectUnknownOptions(parsed.opts, known)) {
        blocks.push_back(ojson(*err));
        return true;
      }
      return false;
    };
    if (cmd == "est" || cmd == "estimates") {
      std::string sub = parsed.varlist.empty() ? std::string() : lower(parsed.varlist[0]);
      if (estSubIs(sub, "store", 3)) sub = "store";
      else if (estSubIs(sub, "restore", 3)) sub = "restore";
      // Every operand after the subcommand, space-joined: `estimates drop m1 m2`
      // used to pass `m1` alone and leave `m2` stored at rc 0.
      std::vector<std::string> estOperands(parsed.varlist.size() > 1 ? parsed.varlist.begin() + 1
                                                                      : parsed.varlist.end(),
                                           parsed.varlist.end());
      if (sub == "restore") {
        // W2 esttabnames (BUG-553): `estimates restore NAME` makes a stored
        // model active again (w2_cmdtriage.log section C: rc 0, "(results m1
        // are active now)", then e(df_m)/_b[a] are m1's). The operand is a
        // namelist that must expand to exactly one name, so `m*` matching
        // one name works and matching two is r(198) "too many names
        // specified"; a miss is r(111). A BARE `est restore` is answered by
        // StataBE with the literal "estimates __000000 not found" r(111)
        // (est_restore_bare).
        // `.` (the active results, unstored) is a no-op.
        if (rejectFor({})) return blocks;
        auto rcBlock = [](const std::string& msg, int rc) {
          return ojson(eval::errorBlock("engine_error", msg, std::nullopt, std::nullopt,
                                        std::nullopt, "r(" + std::to_string(rc) + ")"));
        };
        if (estOperands.empty()) {
          blocks.push_back(rcBlock("estimates __000000 not found", 111));
          return blocks;
        }
        SessionState::EstNamelist ex = session.expandEstimatesNamelist(estOperands);
        if (ex.rc == 0 && ex.names.size() > 1) {
          ex.rc = 198;
          ex.message = "too many names specified";
        } else if (ex.rc == 0 && ex.names.empty()) {
          // `_all` over an empty store expands to nothing (StataBE: r(198)).
          ex.rc = 198;
          ex.message = "too few names specified";
        }
        if (ex.rc != 0) {
          blocks.push_back(rcBlock(ex.message, ex.rc));
          return blocks;
        }
        const std::string rname = ex.names.front();
        // Always re-activated from the store, even when NAME is the name the
        // active results carry: identity is decided inside estimatesRestore
        // (a model-less store is a no-op only when its block IS the live one).
        // eb-order T-P3: an estpost store (a block, no fitted model) is made
        // active again as the posting it was (SessionState::estimatesRestore);
        // only a store holding neither is refused.
        // e(sample) T3: runEstimatesRestore also brings the mask back.
        if (rname != ".") {
          blocks.push_back(ojson(handlers::runEstimatesRestore(rname, session)));
          return blocks;
        }
        ojson note;
        note["type"] = "display";
        note["text"] = "(results " + rname + " are active now)";
        blocks.push_back(std::move(note));
        return blocks;
      }
      // e(sample) T3: `estimates esample` / `estimates esample: [varlist] [if]
      // [in] [, replace]` (the full word, no abbreviation;
      // w2_esample.log:1542 G7 rc 0). The colon arrives glued to the word or as
      // its own token.
      if (sub == "esample" || sub == "esample:") {
        if (rejectFor({"replace"})) return blocks;
        bool colon = sub == "esample:";
        std::vector<std::string> vars;
        for (const auto& t : estOperands) {
          if (!colon && t == ":") {
            colon = true;
            continue;
          }
          if (!colon && !t.empty() && t.front() == ':') {
            colon = true;
            if (t.size() > 1) vars.push_back(t.substr(1));
            continue;
          }
          vars.push_back(t);
        }
        bool replace = false;
        for (const auto& [k, v] : parsed.opts) {
          if (k == "replace") replace = !v.isBool || v.boolValue;
        }
        const bool hasWeight = parsed.weightType.has_value() && parsed.weightVar.has_value();
        blocks.push_back(ojson(handlers::runEstimatesEsample(colon, vars, parsed.ifExpr,
                                                             parsed.inRange, hasWeight, replace,
                                                             session)));
        return blocks;
      }
      if (sub == "store") {
        if (rejectFor({"title"})) return blocks;
        std::optional<std::string> name;
        if (parsed.varlist.size() > 1) name = parsed.varlist[1];
        std::optional<std::string> title;
        for (const auto& [k, v] : parsed.opts) {
          if (k == "title" && !v.isBool) title = dequoteEstTitle(v.strValue);
        }
        // viaEststo=false: `estimates store` is official Stata and never touches
        // estout's auto-name counter ($eststo_counter is written by eststo.ado
        // alone), so it must not move the `est#` the next bare `eststo:` mints.
        blocks.push_back(ojson(handlers::runEststo(name, title, session, "est",
                                                   /*viaEststo=*/false)));
        pushEstStoreMutation(blocks.back(), blocks, session);
        return blocks;
      }
      if (sub == "save" || sub == "use") {
        if (rejectFor(sub == "save" ? std::unordered_set<std::string>{"replace"}
                                    : std::unordered_set<std::string>{}))
          return blocks;
        if (parsed.varlist.size() < 2) {
          blocks.push_back(ojson(eval::errorBlock(
              "unsupported_option",
              "estimates " + sub + ": a file name is required (`estimates " + sub +
                  " \"myest\"`).")));
          return blocks;
        }
        bool replace = false;
        for (const auto& [k, v] : parsed.opts) {
          if (k == "replace") replace = !v.isBool || v.boolValue;
        }
        blocks.push_back(sub == "save"
                             ? ojson(handlers::runEstimatesSave(parsed.varlist[1], replace, session))
                             : ojson(handlers::runEstimatesUse(parsed.varlist[1], session)));
        return blocks;
      }
      if (sub == "dir") {
        if (rejectFor({})) return blocks;
        blocks.push_back(ojson(handlers::runEststoDirNames(estOperands, session)));
        return blocks;
      }
      if (sub == "drop" || sub == "clear") {
        if (rejectFor({})) return blocks;
        std::optional<std::string> name;
        if (!estOperands.empty()) name = joinSpace(estOperands);
        blocks.push_back(ojson(handlers::runEststoManage(sub, name, session)));
        return blocks;
      }
      if (rejectFor({})) return blocks;
      blocks.push_back(ojson(eval::unsupportedCommand(
          cmdTyped + " " + sub, std::nullopt, std::nullopt, "piet_gap",
          eval::UnsupportedCommandKind::kUnknownSubcommand)));
      return blocks;
    }
    // W2 eststoprefix (BUG-520) -- the flat arm's half. A colon-shaped line that
    // reaches HERE was not recognised by Pre-0b (the command word or the colon
    // itself arrived through a macro, or the NAME position holds dir/drop/clear),
    // and this arm never dispatches a remainder. It used to store whatever model
    // was already in memory under `varlist[0]` -- `regress`, `mac3:` -- at rc 0,
    // and with inner options it blamed the INNER command's first option. Refuse
    // the shape by name instead, and BEFORE the option net so the message is
    // about the line's real problem. Same for a second name: `eststo a b` stored
    // `a` and dropped `b` on the floor.
    {
      const bool managing = !parsed.varlist.empty() &&
                            (lower(parsed.varlist[0]) == "dir" ||
                             lower(parsed.varlist[0]) == "drop" ||
                             lower(parsed.varlist[0]) == "clear");
      bool colonShaped = !parsed.cmd.empty() && parsed.cmd.back() == ':';
      for (const auto& tok : parsed.varlist) {
        if (tok.find(':') != std::string::npos) colonShaped = true;
      }
      if (colonShaped) {
        blocks.push_back(ojson(eval::engineError(
            "eststo: this line has the `eststo [name] [, options]: command` prefix shape, but "
            "PIET recognises that prefix only when the word `eststo` and the colon are written "
            "on the line itself (not delivered through a macro), and never with dir/drop/clear "
            "in the name position. Nothing was run and nothing was stored.",
            "syntax_error")));
        return blocks;
      }
      if (!managing && parsed.varlist.size() > 1) {
        blocks.push_back(ojson(eval::engineError(
            "eststo: `" + joinSpace(parsed.varlist) + "` is not one name -- `eststo [name] "
            "[, options]` stores the active estimates under ONE name",
            "syntax_error")));
        return blocks;
      }
    }
    // eststo [NAME] | eststo dir | eststo drop|clear [NAME]
    if (!parsed.varlist.empty()) {
      std::string sub0 = lower(parsed.varlist[0]);
      if (sub0 == "dir" || sub0 == "drop" || sub0 == "clear") {
        if (rejectFor({})) return blocks;
        if (sub0 == "dir") {
          blocks.push_back(ojson(handlers::runEststoDirNames(
              std::vector<std::string>(parsed.varlist.begin() + 1, parsed.varlist.end()),
              session)));
          return blocks;
        }
        std::optional<std::string> name;
        if (parsed.varlist.size() > 1) {
          name = joinSpace(std::vector<std::string>(parsed.varlist.begin() + 1,
                                                    parsed.varlist.end()));
        }
        blocks.push_back(ojson(handlers::runEststoManage(sub0, name, session)));
        return blocks;
      }
    }
    EststoStoreOpts storeOpts;
    if (auto err = readEststoStoreOpts(parsed.opts, &storeOpts)) {
      blocks.push_back(*err);
      return blocks;
    }
    std::optional<std::string> name;
    if (!parsed.varlist.empty()) name = parsed.varlist[0];
    blocks.push_back(
        ojson(handlers::runEststo(name, storeOpts.title, session, storeOpts.autoPrefix)));
    pushEstStoreMutation(blocks.back(), blocks, session);
    return blocks;
  }

  // ==================== AER-SCOPE1 bundle J (GRAPH-X) arms ==================
  if (cmd == "grc1leg2") {
    ojson result = ojson(handlers::runGrc1leg(parsed, session));
    registerGraphOutput(result, session, parsed.opts, graphLineTyped);  // BS-011
    if (!(result.is_array() && result.empty())) blocks.push_back(result);
    return blocks;
  }
  // ==================== AER-SCOPE2 bundle K1 arms =========================
  // The corpus's remaining small names. `grc1leg` is an alias, `beep` and
  // `window` are accept-notes, and the three renamers below them have real
  // handlers in handlers_aer_k1.cpp -- every rule in which was measured against
  // StataBE with the packages installed, before it was written. `parallel` is
  // the one still open (K1c).
  if (cmd == "grc1leg") {
    // Vince Wiggins' original (stata.trk: stata.com/users/vwiggins). We answer
    // it with the grc1leg2 handler — Mead Over's enhanced rewrite — which is an
    // ACCEPTED-NAMED substitution, not an equivalence: the two packages have
    // different option defaults, so the registry carries both authors and the
    // probe keeps an arm on a default that differs.
    ojson result = ojson(handlers::runGrc1leg(parsed, session));
    registerGraphOutput(result, session, parsed.opts, graphLineTyped);  // BS-011
    if (!(result.is_array() && result.empty())) blocks.push_back(result);
    return blocks;
  }
  if (cmd == "beep") {
    // Accept-note, not silence: `beep` is a real built-in that makes a sound we
    // have no way to make, and a do-file that beeps between steps must keep
    // running. Saying so once beats a user believing something happened.
    ojson note;
    note["type"] = "display";
    note["text"] = "(beep: this build makes no sound — accepted and ignored)";
    blocks.push_back(note);
    return blocks;
  }
  if (cmd == "window") {
    // Same shape. The corpus form is `window manage close graph _all`, a GUI
    // instruction with no headless meaning.
    ojson note;
    note["type"] = "display";
    note["text"] = "(window: GUI window management does not apply in this build — accepted and ignored)";
    blocks.push_back(note);
    return blocks;
  }
  if (cmd == "parallel") {
    // ==================== AER scope 2, bundle K1c =========================
    // George Vega Yon's `parallel` (SSC), answered SEQUENTIALLY.
    //
    // WHAT THIS IS NOT. PIET already owns the word `parallel` for its own
    // `parallel { ... }` block (loops.cpp's loopIfKeywords). That form requires
    // a brace ON THE LINE, so it is recognised earlier and never reaches here;
    // every shape below is brace-free and falls through to this arm. Two
    // different commands share a spelling, and the split is structural rather
    // than a heuristic.
    //
    // EVERY RULE BELOW COMES FROM THE PACKAGE'S OWN SOURCE, because the package
    // CANNOT BE RUN on this machine: `parallel setclusters 2` answers r(601)
    // even with an explicit `statapath()`, so $PLL_STATA_PATH never gets set and
    // no child process can start. That is a measurement too, and it is why the
    // data-splitting form below is refused rather than approximated -- there is
    // no oracle to check it against.
    //
    // What parallel.ado does, read from it:
    //   · `parallel do FILE` REQUIRES $PLL_CLUSTERS and exits 198 without it,
    //     with the message reproduced verbatim below.
    //   · With `nodata` (or an empty dataset) the children get NO data and
    //     nothing is pasted back: the parent's data is untouched.
    //   · WITHOUT `nodata` it splits the rows across $PLL_CLUSTERS children and
    //     then `parallel_fusion` PASTES THEIR DATASETS BACK. With one cluster
    //     that is exactly "run the file on the whole dataset in place"; with
    //     more, each child sees a different slice and the answer depends on the
    //     split, which is the form refused here.
    //   · `initialize` is NOT a subcommand of the installed 1.15.8 (measured:
    //     `-initialize- invalid parallel subcommand`, r(198)) -- but it IS what
    //     the corpus writes, from an older release where it set the cluster
    //     count. It is accepted here as a spelling of `setclusters`, which is
    //     the ACCEPTED-NAMED exception for a third-party ado whose own versions
    //     disagree; refusing it would stop a paper's driver on a no-op.
    std::string rest = trimWs(rawArgsAfterCmd(parsed.raw));
    std::string head;
    {
      size_t sp = rest.find_first_of(" \t,:");
      head = lower(sp == std::string::npos ? rest : rest.substr(0, sp));
    }
    auto note = [&](const std::string& text) {
      ojson b;
      b["type"] = "display";
      b["text"] = text;
      blocks.push_back(b);
      return blocks;
    };
    auto refuse = [&](const std::string& text, int rc) {
      blocks.push_back(ojson(eval::errorBlock("engine_error", text, std::nullopt, std::nullopt,
                                              std::nullopt, "r(" + std::to_string(rc) + ")")));
      return blocks;
    };

    if (head == "clean") {
      // Measured rc 0 in Stata even with no cluster set up: it deletes the
      // scratch files a run would have left, and this build leaves none.
      return note("(parallel clean: this build runs sequentially and leaves no cluster "
                  "scratch files -- accepted and ignored)");
    }
    if (head == "setclusters" || head == "initialize") {
      std::string n;
      {
        std::string tail = trimWs(rest.substr(head.size()));
        size_t sp = tail.find_first_of(" \t,");
        n = sp == std::string::npos ? tail : tail.substr(0, sp);
      }
      if (n.empty()) n = "1";
      session.eval.globals.set("PLL_CLUSTERS", n);
      return note("(parallel " + head + " " + n +
                  ": recorded. This build runs the work sequentially in this process, so the "
                  "cluster count changes no answer)");
    }
    if (head == "break" || head == "version" || head == "printlog" || head == "viewlog" ||
        head == "bs" || head == "sim") {
      return refuse("parallel " + head +
                        " needs a running cluster, and this build has none: the package cannot "
                        "start a child process here (`parallel setclusters` answers r(601) even "
                        "with statapath()). Refused rather than approximated.",
                    199);
    }

    // The prefix form, `parallel [, opts] : cmd`, and `parallel do FILE`.
    const bool isDo = (head == "do");
    // The colon of the prefix form, skipping any inside a quoted span --
    // `parallel, by(x): summarize "a:b"` must split at the real one.
    size_t colon = std::string::npos;
    if (!isDo) {
      const std::vector<bool> qmask = eval::quotedSpanMask(rest);
      for (size_t i = 0; i < rest.size(); ++i) {
        if (rest[i] == ':' && (i >= qmask.size() || !qmask[i])) {
          colon = i;
          break;
        }
      }
    }
    if (!isDo && head != "append" && colon == std::string::npos) {
      return refuse("-" + head + "- invalid parallel subcommand (help parallel)", 198);
    }
    if (head == "append") {
      return refuse("parallel append reads a list of .dta files, runs a command over each and "
                    "pastes the results together. Its file-list expansion is implemented and "
                    "measured, but the per-file run goes through the cluster machinery this "
                    "build cannot start, so the whole verb is refused rather than half-done.",
                    199);
    }

    const std::string* clusters = session.eval.globals.find("PLL_CLUSTERS");
    if (clusters == nullptr || clusters->empty()) {
      return refuse("You haven't set the number of clusters. Please set it with: parallel "
                    "setclusters #",
                    198);
    }

    std::string body;    // the file (do form) or the command (prefix form)
    std::string optTail;
    if (isDo) {
      std::string tail = trimWs(rest.substr(2));
      long long c = eval::findUnquotedComma(tail);
      if (c >= 0) {
        optTail = trimWs(tail.substr(static_cast<size_t>(c) + 1));
        tail = trimWs(tail.substr(0, static_cast<size_t>(c)));
      }
      body = tail;
    } else {
      optTail = trimWs(rest.substr(0, colon));
      if (!optTail.empty() && optTail[0] == ',') optTail = trimWs(optTail.substr(1));
      body = trimWs(rest.substr(colon + 1));
    }

    auto optHas = [&](const std::string& key) {
      return lower(optTail).find(key) != std::string::npos;
    };
    // `processors(1)` and `nodata` are the two the corpus writes and the two
    // that decide the shape below. Everything else that would change what a
    // child SEES -- by(), seeds(), randtype(), programs(), noglobals, mata --
    // is refused by name: accepting one silently would promise a scheme this
    // build does not run.
    for (const char* bad : {"by(", "seeds(", "randtype(", "programs(", "noglobals", "mata",
                            "keepusing(", "setparallelid("}) {
      if (optHas(bad)) {
        return refuse(std::string("parallel: option ") + bad +
                          " changes what each cluster receives, and this build runs the work "
                          "sequentially in one process. Refused rather than accepted-and-ignored.",
                      198);
      }
    }
    const bool noData = optHas("nodata");
    auto wholeInt = [](const std::string& t, long long* out) {
      if (t.empty()) return false;
      size_t i = (t[0] == '+' || t[0] == '-') ? 1 : 0;
      if (i >= t.size()) return false;
      for (size_t k = i; k < t.size(); ++k) {
        if (std::isdigit(static_cast<unsigned char>(t[k])) == 0) return false;
      }
      *out = std::stoll(t);
      return true;
    };
    long long nClusters = 1;
    {
      if (!wholeInt(*clusters, &nClusters)) nClusters = 1;
      size_t pp = lower(optTail).find("processors(");
      if (pp != std::string::npos) {
        size_t close = optTail.find(')', pp);
        if (close != std::string::npos) {
          long long p = 0;
          if (wholeInt(trimWs(optTail.substr(pp + 11, close - pp - 11)), &p) && p > 0)
            nClusters = p;
        }
      }
    }

    const bool hasRows = session.hasData();
    if (!noData && hasRows && nClusters > 1) {
      return refuse("parallel: without `nodata` the package SPLITS the rows across " +
                        std::to_string(nClusters) +
                        " clusters, runs the file on each slice and pastes the results back. "
                        "What each run sees depends on the split, and the package cannot be run "
                        "here to check a sequential reproduction against. Use `nodata`, or "
                        "processors(1), or run the file directly.",
                    199);
    }

    // Sequential run. With `nodata` the children start with NO data and nothing
    // comes back, so the parent's dataset is cleared for the run and restored
    // after it. With one cluster and data, the child's dataset IS the result
    // (fusion of a single cluster), so it is left in place.
    //
    // The snapshot goes through `preserve`/`restore` rather than through a
    // saved hbin path plus `clear`: `clear` retires the scratch file the path
    // named, so restoring it afterwards fails with "the working dataset names a
    // scratch file that was never written" -- which is how the first cut of
    // this arm died, on the run rather than on a check.
    //
    // BUG-922: the snapshot sits one preserve level below the caller, so a
    // caller that holds its own preserve does not make it r(621) (the restore
    // below would then have popped the CALLER's copy), and the child runs one
    // level deeper still: it is a separate Stata session that may preserve,
    // and whatever it leaves preserved is restored when it ends. Every frame
    // below the child's level -- the snapshot and the caller's copies -- is
    // out of its reach (SessionState::ownPreserveFrame): its `restore` in any
    // form is r(622) and touches neither.
    bool snapped = false;
    std::optional<DoFileLevelGuard> snapLevel;
    if (noData && hasRows) {
      snapLevel.emplace(session);
      ojson pres = handlers::runPreserve(session);
      if (pres.is_object() && pres.value("type", std::string()) == "error") {
        // No snapshot, no run: clearing the data now would lose it.
        blocks.push_back(std::move(pres));
        return blocks;
      }
      snapped = true;
      (void)dispatch("clear", session);
    }
    doexec::ExecuteResult r;
    {
      const int parentPreserveLevel = session.preserveLevel();
      {
        DoFileLevelGuard childLevel(session);
        program::LoopFloorGuard loopFloor(session);  // BUG-920: a separate session
        r = isDo ? doexec::executeDo(body, session, dispatch).result
                 : doexec::executeDoSource(body, session, dispatch);
      }
      program::concludeLevel(parentPreserveLevel, session, r.blocks);
    }
    for (const auto& b : r.blocks) blocks.push_back(b);
    if (snapped) {
      // BUG-922: a failed snapshot restore is reported, never dropped.
      ojson res = handlers::runRestore(session);
      if (res.is_object() && res.value("type", std::string()) == "error")
        blocks.push_back(std::move(res));
    }
    return blocks;
  }
  if (cmd == "strdist") {
    // AER scope 2 B2: the last name blocking paper 100.
    pushEnvResult(handlers::runStrdist(parsed, session));
    return blocks;
  }
  if (cmd == "tolower" || cmd == "renvars" || cmd == "move") {
    // pushEnvResult flattens an array and drops nulls, which is the shape all
    // three return: an array of display blocks (possibly empty, which is how
    // the silent path says "nothing to print").
    pushEnvResult(cmd == "tolower"  ? handlers::runTolower(parsed, session)
                  : cmd == "renvars" ? handlers::runRenvars(parsed, session)
                                     : handlers::runMove(parsed, session));
    return blocks;
  }

  if (cmd == "grstyle") {
    // Accept-note, not a silent no-op: this build applies no scheme styling, and
    // saying so once is the difference between "your styling was ignored" and a
    // user believing it took effect. A refusal would stop do-files that open
    // with `grstyle init` before any real work.
    ojson note;
    note["type"] = "display";
    note["text"] = "(grstyle: scheme styling is not applied in this build — accepted and ignored)";
    blocks.push_back(note);
    return blocks;
  }
  if (cmd == "geodist") {
    pushEnvResult(handlers::runGeodist(parsed, session));
    return blocks;
  }
  if (cmd == "geonear") {
    pushEnvResult(handlers::runGeonear(parsed, session));
    return blocks;
  }
  if (cmd == "cdfplot") {
    ojson result = ojson(handlers::runCdfplot(parsed, session));
    registerGraphOutput(result, session, parsed.opts, graphLineTyped);  // BS-011
    if (!(result.is_array() && result.empty())) blocks.push_back(result);
    return blocks;
  }
  if (cmd == "event_plot") {
    ojson result = ojson(handlers::runEventPlot(parsed, session));
    registerGraphOutput(result, session, parsed.opts, graphLineTyped);  // BS-011
    if (!(result.is_array() && result.empty())) blocks.push_back(result);
    return blocks;
  }

  // ==================== AER-SCOPE1 bundle I (UTIL) arms =====================
  if (cmd == "unique" || cmd == "gunique") {
    // One arm, two spellings: gtools' `gunique` differs from `unique` only in
    // name, and giving it its own handler would be a second thing to keep in
    // step. The handler reads `parsed.cmd` for the two options that ARE
    // gunique-only and refuses them by name.
    pushEnvResult(handlers::runUnique(parsed, session));
    return blocks;
  }
  if (cmd == "distinct") {
    pushEnvResult(handlers::runDistinct(parsed, session));
    return blocks;
  }
  if (cmd == "missings") {
    pushEnvResult(handlers::runMissings(parsed, session));
    return blocks;
  }
  if (cmd == "mdesc") {
    pushEnvResult(handlers::runMdesc(parsed, session));
    return blocks;
  }
  if (cmd == "labmask") {
    pushEnvResult(handlers::runLabmask(parsed, session));
    return blocks;
  }
  if (cmd == "renvarlab") {
    pushEnvResult(handlers::runRenvarlab(parsed, session));
    return blocks;
  }
  if (cmd == "splitvallabels") {
    pushEnvResult(handlers::runSplitvallabels(parsed, session));
    return blocks;
  }
  if (cmd == "marksample") {
    pushEnvResult(handlers::runMarksample(parsed, session));
    return blocks;
  }
  if (cmd == "filelist") {
    pushEnvResult(handlers::runFilelist(parsed, session));
    return blocks;
  }
  if (cmd == "carryforward") {
    pushEnvResult(handlers::runCarryforward(parsed, session));
    return blocks;
  }
  if (cmd == "lookfor") {
    pushEnvResult(handlers::runLookfor(parsed, session));
    return blocks;
  }
  if (cmd == "ihstrans") {
    pushEnvResult(handlers::runIhstrans(parsed, session));
    return blocks;
  }
  if (cmd == "sctorezone") {
    pushEnvResult(handlers::runSctorezone(parsed, session));
    return blocks;
  }
  if (cmd == "rangestat") {
    pushEnvResult(handlers::runRangestat(parsed, session));
    return blocks;
  }
  if (cmd == "tsspell") {
    pushEnvResult(handlers::runTsspell(parsed, session));
    return blocks;
  }

  // ==================== AER-SCOPE1 bundle G (IO-TEXT) arms ==================
  if (cmd == "insheet") {
    pushEnvResult(handlers::runInsheet(parsed, session));
    return blocks;
  }
  if (cmd == "outsheet") {
    pushEnvResult(handlers::runOutsheet(parsed, session));
    return blocks;
  }
  if (cmd == "infile") {
    pushEnvResult(handlers::runInfile(parsed, session));
    return blocks;
  }
  if (cmd == "infix") {
    pushEnvResult(handlers::runInfix(parsed, session));
    return blocks;
  }
  if (cmd == "outfile") {
    pushEnvResult(handlers::runOutfile(parsed, session));
    return blocks;
  }
  if (cmd == "saveold") {
    pushEnvResult(handlers::runSaveold(parsed, session));
    return blocks;
  }
  if (cmd == "putexcel") {
    // RAW line, not `parsed`: putexcel's grammar is `A1 = <value>`, and the
    // tokenizer splits on `=` and on the commas inside `matrix(M), names`, so
    // a parsed varlist cannot reconstruct the assignment. Same reason the
    // `matrix` arm re-parses its own raw line.
    pushEnvResult(handlers::runPutexcel(rawWithCanonicalCmd(parsed.raw, cmdTyped, cmd), session));
    return blocks;
  }
  if (cmd == "filefilter") {
    pushEnvResult(handlers::runFilefilter(parsed, session));
    return blocks;
  }
  if (cmd == "copy") {
    pushEnvResult(handlers::runCopy(parsed, session));
    return blocks;
  }
  // ==================== AER tier-1 arms (2026-10-28) =====================
  // One arm per command, each through pushEnvResult (handlers may answer an
  // array of blocks). `macro` has no arm: tryMacroVerbIntercept consumes it
  // before the tokenizer, whose unsupported_macro net would otherwise fire.
  {
    using TierOneHandler = ojson (*)(const eval::ParsedCommand&, SessionState&);
    static const std::map<std::string, TierOneHandler> kTierOne = {
        {"unzipfile", &handlers::runUnzipfile},   {"zipfile", &handlers::runZipfile},
        {"zipuse", &handlers::runZipuse},         {"zipsave", &handlers::runZipsave},
        {"require", &handlers::runRequire},       {"_dots", &handlers::runDots},
        {"postutil", &handlers::runPostutil},     {"markout", &handlers::runMarkout},
        {"lrtest", &handlers::runLrtest},         {"findname", &handlers::runFindname},
        {"xtdescribe", &handlers::runXtdescribe}, {"cf", &handlers::runCf},
        {"fvunab", &handlers::runFvunab},         {"numlabel", &handlers::runNumlabel},
        {"labvars", &handlers::runLabvars},       {"renpfix", &handlers::runRenpfix},
        {"mvdecode", &handlers::runMvdecode},     {"integ", &handlers::runInteg},
        {"sencode", &handlers::runSencode},       {"fillmissing", &handlers::runFillmissing},
        {"astile", &handlers::runAstile},         {"matsort", &handlers::runMatsort},
        {"matselrc", &handlers::runMatselrc},
    };
    auto it1 = kTierOne.find(cmd);
    if (it1 != kTierOne.end()) {
      // The registry's allowIf is descriptive only (see the hausman note
      // below), so a row that takes no `if` refuses it here, before the
      // handler: Stata answers "if not allowed", r(101) (StataBE, review of
      // the tier-1 META lane: cf, fvunab, numlabel). renpfix's ado parses the
      // qualifier as a third word, r(198); labvars reads it as label text and
      // so judges it itself.
      const registry::CommandSpec* t1spec = registry::findByName(cmd);
      if (parsed.ifExpr.has_value() && t1spec != nullptr && !t1spec->allowIf &&
          cmd != "labvars") {
        const bool asSyntax = (cmd == "renpfix");
        // cf is r-class: Stata's refusal still replaces r() with nothing
        // (META review p3, `cf x using f if x > 1` then r(Nsum) is `.`).
        if (cmd == "cf") {
          eval::replaceRClass(session.eval);
          eval::declareRPosted(session.eval, "cf", {});
        }
        blocks.push_back(ojson(eval::errorBlock(
            "syntax_error", asSyntax ? "invalid syntax" : "if not allowed", std::nullopt,
            std::nullopt, std::nullopt, asSyntax ? "r(198)" : "r(101)")));
        return blocks;
      }
      pushEnvResult(it1->second(parsed, session));
      return blocks;
    }
  }
  if (cmd == "odbc") {
    blocks.push_back(ojson(eval::refusedCommand(cmdTyped, "no_odbc_driver")));
    return blocks;
  }
  if (cmd == "spshape2dta") {
    blocks.push_back(ojson(eval::refusedCommand(cmdTyped, "no_shapefile_reader")));
    return blocks;
  }

  // ==================== AER-SCOPE1 bundle F (RESHAPE) arms ==================
  // Shape and sample verbs. Three of them round-trip the whole frame through a
  // CSV and therefore go through the pre-flight gate first -- the same shape
  // the `expand` arm above uses: reject net, then pre-flight, then handler.
  // `pushEnvResult` flattens and drops nulls (defect class 1).
  if (cmd == "split") {
    pushEnvResult(handlers::runSplit(parsed, session));
    return blocks;
  }
  if (cmd == "xpose") {
    if (preflight::gate("xpose", session, "", 1, blocks) == preflight::Verdict::Cancel) return blocks;
    pushEnvResult(handlers::runXpose(parsed, session));
    return blocks;
  }
  if (cmd == "recast") {
    // format-display-default-lifecycle: recast moves the storage type, never the
    // display format (`recast double y` of a float stays %9.0g,
    // internal capture exprfns_contract_oracle.log:735). Each named
    // variable's current format is pinned before runRecast moves the type (a
    // string's default instead follows the new width: prepareStorageChange),
    // and a refused recast takes the pins back.
    std::map<std::string, std::optional<VarMeta>> priorMeta;
    if (parsed.varlist.size() >= 2 && session.hasData()) {
      std::vector<std::string> names(parsed.varlist.begin() + 1, parsed.varlist.end());
      Datastore* ds = nullptr;
      try {
        ds = session.datastore();
      } catch (const std::exception&) {
        ds = nullptr;  // runRecast reports the load failure itself
      }
      if (ds != nullptr) {
        for (const VarInfo& v : ds->formatCard(names)) {
          auto it = session.varMeta.find(v.name);
          priorMeta[v.name] =
              it == session.varMeta.end() ? std::nullopt : std::optional<VarMeta>(it->second);
          session.prepareStorageChange(v);
        }
      }
    }
    ojson r = handlers::runRecast(parsed, session);
    if (r.is_object() && r.value("type", std::string()) == "error") {
      for (const auto& [name, meta] : priorMeta) {
        if (meta.has_value()) session.varMeta[name] = *meta;
        else session.varMeta.erase(name);
      }
    }
    pushEnvResult(r);
    return blocks;
  }
  if (cmd == "insobs") {
    pushEnvResult(handlers::runInsobs(parsed, session));
    return blocks;
  }
  if (cmd == "stack") {
    {
      // The pre-flight factor is the number of variables being stacked, which
      // is how many copies of the row count the result carries.
      const int64_t k = static_cast<int64_t>(parsed.varlist.size());
      if (preflight::gate("stack", session, "", k > 0 ? k : 1, blocks) == preflight::Verdict::Cancel)
        return blocks;
    }
    pushEnvResult(handlers::runStack(parsed, session));
    return blocks;
  }
  if (cmd == "separate") {
    pushEnvResult(handlers::runSeparate(parsed, session));
    return blocks;
  }
  if (cmd == "mvencode") {
    pushEnvResult(handlers::runMvencode(parsed, session));
    return blocks;
  }
  if (cmd == "sample") {
    pushEnvResult(handlers::runSample(parsed, session));
    return blocks;
  }
  if (cmd == "splitsample") {
    pushEnvResult(handlers::runSplitsample(parsed, session));
    return blocks;
  }
  if (cmd == "bsample") {
    if (preflight::gate("bsample", session, "", 1, blocks) == preflight::Verdict::Cancel)
      return blocks;
    ojson bs = ojson(handlers::runBsample(parsed, session));
    // e(sample) T2: Stata keeps the marker on one copy of each drawn observation,
    // which one following its own sort (StataBE capture w2_esample_copies X2
    // `10|61|m0=20`): named, never guessed.
    bool bsFailed = false;
    for (const auto& b : bs.is_array() ? bs : ojson::array({bs}))
      bsFailed = bsFailed || (b.is_object() && b.value("type", std::string()) == "error");
    pushEnvResult(bs);
    // The fast method (bsampleIsStataFastPath) draws Stata's own rows and keeps the
    // marker on them itself; the SRSWR/cluster paths draw PIET's rows, so the
    // marker Stata would have kept is not knowable there and is withdrawn.
    if (!bsFailed && !handlers::bsampleIsStataFastPath(parsed) &&
        !handlers::esampleMarkHeadLost(session, EsampleLostBy::kBsample)) {
      blocks.push_back(ojson(eval::engineError(
          "bsample: the sample was drawn, but the estimation-sample marker could not be "
          "withdrawn from it; e(sample) is not reliable until the next estimation.")));
    }
    return blocks;
  }
  if (cmd == "drawnorm") {
    pushEnvResult(handlers::runDrawnorm(parsed, session));
    return blocks;
  }
  if (cmd == "input") {
    // A FOLDED `input` block was intercepted on the raw line far above; only a
    // bare `input` with no block reaches here, and it gets an honest error
    // rather than silently doing nothing.
    pushEnvResult(handlers::runInput(parsed.raw, session));
    return blocks;
  }
  if (cmd == "notes") {
    pushEnvResult(handlers::runNotes(parsed, session));
    return blocks;
  }
  if (cmd == "compare") {
    pushEnvResult(handlers::runCompare(parsed, session));
    return blocks;
  }
  if (cmd == "cumul") {
    pushEnvResult(handlers::runCumul(parsed, session));
    return blocks;
  }
  if (cmd == "pctile") {
    pushEnvResult(handlers::runPctile(parsed, session));
    return blocks;
  }
  if (cmd == "_pctile") {
    // r-class: `_pctile x, p(10 90)` publishes r(r1)/r(r2) and touches no
    // column. The handler fills r() itself here (it is the whole point of the
    // command), so unlike `gstats sum` this arm does not call
    // captureRFromBlock -- there is no table block to capture FROM.
    pushEnvResult(handlers::runPctileR(parsed, session));
    return blocks;
  }

  // ==================== AER-SCOPE1 bundle E (TABLES) arms ====================
  // Placed beside eststo/esttab, the verbs they extend. Every arm routes
  // through `pushEnvResult`, which flattens an array and drops a null: a
  // handler returning a JSON ARRAY where the arm pushes ONE block is the defect
  // class that has now been caught three times in four bundles (twice by
  // review, once by the selftest binary aborting on type_error.306), so it is
  // fixed HERE, at the single call shape, rather than per handler.
  if (cmd == "estadd") {
    pushEnvResult(handlers::runEstadd(parsed, session));
    return blocks;
  }
  if (cmd == "estpost") {
    // BUG-733: a successful estpost has posted a new e() (postPseudoEstimate)
    // with no fitted model behind it; the next model install drops it
    // (SessionState::supersedePostedE). estpost itself is unchanged.
    ojson r = handlers::runEstpost(parsed, session);
    const bool refused = r.is_object() && r.value("type", std::string()) == "error";
    if (!refused && session.lastEstBlock.has_value() &&
        session.lastEstBlock->value("estimator", std::string()).rfind("estpost_", 0) == 0) {
      session.ePostedLive = true;
      // round 3: postPseudoEstimate replaced e() wholesale, so nothing sits on
      // top of a fit any more.
      session.eOnFitNames.clear();
    }
    pushEnvResult(r);
    return blocks;
  }
  if (cmd == "estout") {
    pushEnvResult(handlers::runEstout(parsed, session));
    return blocks;
  }
  if (cmd == "outreg2") {
    pushEnvResult(handlers::runOutreg2(parsed, session));
    return blocks;
  }
  if (cmd == "texsave") {
    pushEnvResult(handlers::runTexsave(parsed, session));
    return blocks;
  }
  if (cmd == "texdoc") {
    pushEnvResult(handlers::runTexdoc(parsed, session));
    return blocks;
  }
  if (cmd == "tex") {
    pushEnvResult(handlers::runTexdocLine(parsed, session));
    return blocks;
  }
  if (cmd == "frmttable") {
    pushEnvResult(handlers::runFrmttable(parsed, session));
    return blocks;
  }
  if (cmd == "regsave") {
    pushEnvResult(handlers::runRegsave(parsed, session));
    return blocks;
  }
  if (cmd == "parmest") {
    pushEnvResult(handlers::runParmest(parsed, session));
    return blocks;
  }
  if (cmd == "mat2txt") {
    pushEnvResult(handlers::runMat2txt(parsed, session));
    return blocks;
  }
  if (cmd == "erepost") {
    // round 3: repost changes b/V on the SAME model; every other e() key stays.
    // BUG-1202: a fit that posted e(b) as a matrix (glm, oaxaca, teffects) has
    // that matrix reposted too, so it is the one store the restore keeps.
    std::optional<eval::EvalMatrix> repostedB;
    ojson res = runPostestKeepingE(session, [&] {
      ojson r = handlers::runErepost(parsed, session);
      if (const eval::EvalMatrix* b = session.eval.findClassMatrix('e', "b")) repostedB = *b;
      return r;
    });
    if (repostedB.has_value()) {
      for (auto& p : session.eval.eMatrices)
        if (p.first == "b") p.second = *repostedB;
    }
    pushEnvResult(std::move(res));
    return blocks;
  }
  if (cmd == "tabout") {
    // Refused at the COMMAND level, not the option level: tabout's grammar is a
    // panel language of its own, and accepting the head while refusing every
    // option would report a missing option where the truth is a missing
    // command. `refusedCommand` (not `engineError`) so `eval::coarseRc` answers
    // 199 -- the default code answers 1, which is bundle D's shipped defect.
    blocks.push_back(ojson(eval::refusedCommand(cmdTyped, "tabout_grammar")));
    return blocks;
  }

  if (cmd == "esttab") {
    // AER-SCOPE1 bundle E: `esttab ... using f.tex` has to leave a FILE behind.
    // Until now this arm returned a JSON block and wrote nothing, so every
    // corpus line that ended in `using` succeeded and produced no output file.
    // The path is split off HERE so `runEsttab` still receives pure model
    // names, and the block it returns is exactly what gets written -- one
    // renderer, so the on-screen table and the file cannot disagree.
    std::vector<std::string> names = parsed.varlist;
    std::string usingPath;
    for (size_t i = 0; i < names.size(); ++i) {
      if (lower(names[i]) == "using") {
        if (i + 1 < names.size()) {
          size_t j = i + 1;
          usingPath = strutil::rejoinQuotedWord(names, j);  // a quoted path with blanks
        }
        names.erase(names.begin() + static_cast<long>(i), names.end());
        break;
      }
    }
    // W1a-2 / BUG-407: the cell layer lives in the writer `runEsttab`'s JSON
    // estimates_table has no model for, so on screen a `cells()`/`main()`/`aux()`
    // table is the writer's rendered text (esttabScreen returns it below).
    {
      // W2AK: `esttab matrix(A, fmt()) using f` tabulates a matrix
      // (esttab MatrixMode); the namelist is the matrix spec.
      std::string joined;
      for (const auto& w : names) joined += (joined.empty() ? "" : " ") + w;
      if (joined.rfind("matrix(", 0) == 0 && !joined.empty() && joined.back() == ')') {
        ojson mblock;
        mblock["coef_rows"] = ojson::array();
        mblock["names"] = ojson::array();
        mblock["matrix"] = joined.substr(7, joined.size() - 8);
        if (usingPath.empty()) {
          ojson chk = handlers::esttabScreen(mblock, parsed.opts, session);
          if (!chk.is_null()) pushEnvResult(chk);
        } else {
          pushEnvResult(handlers::writeEsttabFile(mblock, usingPath, parsed.opts, session));
        }
        return blocks;
      }
    }
    ojson result = ojson(handlers::runEsttab(names, parsed.opts, session));
    if (usingPath.empty() && result.value("type", std::string()) != "error") {
      // W2AK: the screen form validates and posts r() through the same
      // translation the file form uses (w2ak ek_c01/c05/c09).
      ojson chk = handlers::esttabScreen(result, parsed.opts, session);
      if (!chk.is_null()) {
        pushEnvResult(chk);
        return blocks;
      }
    }
    blocks.push_back(result);
    if (!usingPath.empty() && result.value("type", std::string()) != "error") {
      pushEnvResult(handlers::writeEsttabFile(result, usingPath, parsed.opts, session));
    }
    return blocks;
  }

  if (cmd == "coefplot") {
    // coefplot-family AN-1..AN-4: coefplot.ado 1.8.8's own pipeline
    // (handlers_coefplot.cpp) decides what is plotted, generate() and r(); the
    // option set is coefplot's (its unknown options are twoway's and pass), so
    // the W4 vocabulary net no longer runs here. `event_plot` keeps calling
    // runCoefplot directly. name() onto a graph in memory without replace is
    // r(110) before anything is computed (w2an cf_rf_name_dup, log:3076-3084).
    const std::string cpName = graphNameOption(parsed.opts);
    if (auto clash = serset::nameCollision(session, cpName)) {
      blocks.push_back(*clash);
      return blocks;
    }
    ojson drawRows = ojson::array();
    ojson pre = handlers::runCoefplotJann(session, parsed, &drawRows);
    for (auto& b : pre) {
      const bool isErr = b.is_object() && b.value("type", std::string()) == "error";
      blocks.push_back(b);
      if (isErr) return blocks;
    }
    if (drawRows.empty()) return blocks;  // (nothing to plot)
    // The drawn block: the established coefplot block of the stored models a
    // bare model list names (freeze-pinned shape); for any other statement the
    // same shape built from the rows coefplot.ado plots.
    ojson result;
    {
      std::string head = rawArgsAfterCmd(parsed.raw);
      const long long hc = eval::findUnquotedComma(head);
      if (hc >= 0) head = head.substr(0, static_cast<size_t>(hc));
      const bool bare = !trimWs(head).empty() && head.find('(') == std::string::npos &&
                        head.find('|') == std::string::npos && head.find('\\') == std::string::npos;
      if (bare) {
        ojson old = ojson(handlers::runCoefplot(parsed.varlist, parsed.opts, session));
        if (old.is_object() && old.value("type", std::string()) == "graph") result = old;
      }
      if (result.is_null()) {
        ojson models = ojson::array();
        std::map<std::string, size_t> at;
        double lo = std::numeric_limits<double>::infinity(), hi = -lo;
        for (const auto& r : drawRows) {
          const std::string pl = r.value("plot_label", std::string());
          if (!at.count(pl)) {
            at[pl] = models.size();
            models.push_back(ojson{{"name", pl}, {"coefs", ojson::array()}});
          }
          models[at[pl]]["coefs"].push_back(
              ojson{{"name", r.value("name", std::string())}, {"b", r["b"]}, {"ci_lo", r["ci_lo"]}, {"ci_hi", r["ci_hi"]}});
          for (const char* k : {"b", "ci_lo", "ci_hi"}) {
            if (r[k].is_number()) {
              lo = std::min(lo, r[k].get<double>());
              hi = std::max(hi, r[k].get<double>());
            }
          }
        }
        if (!std::isfinite(lo)) {
          lo = -1;
          hi = 1;
        }
        const double pad = hi > lo ? (hi - lo) * 0.05 : 0.5;
        result["type"] = "graph";
        result["kind"] = "coefplot";
        result["title"] = "Coefficient plot";
        result["x_label"] = "Coefficient estimate";
        result["y_label"] = "Coefficient";
        result["models"] = models;
        result["level"] = 95;
        result["n_models"] = static_cast<int64_t>(models.size());
        result["x_min"] = lo - pad;
        result["x_max"] = hi + pad;
        result["name"] = nullptr;
      }
    }
    registerGraphOutput(result, session, parsed.opts, graphLineTyped);  // BS-011
    ojson savedNote;  // BUG-1529: `file cf1.gph saved` (StataBE 19.5, g2.do)
    for (const auto& [k, v] : parsed.opts) {
      if (k == "saving" && !v.isBool) {
        ojson se = handlers::saveGraphOption(result, v.strValue, session, nullptr, &savedNote);
        if (!se.is_null()) {
          blocks.push_back(se);
          return blocks;
        }
      }
    }
    blocks.push_back(result);
    if (!savedNote.is_null()) blocks.push_back(savedNote);
    return blocks;
  }

  if (cmd == "marginsplot") {
    // BUG-1422: `marginsplot [, options]` takes no varlist, `if`, `in` or weights;
    // a stray term used to be dropped here and the plot drawn (or refused at rc 1
    // for want of margins), where Stata stops first, in this order: r(101)
    // "varlist not allowed", "if not allowed", "in range not allowed", "weights
    // not allowed" (StataBE 19.5, after `margins`). The tokenizer leaves `in` and
    // weights to this arm (registry.h qualifiersRefusedByArm).
    const char* notAllowed = !parsed.varlist.empty()       ? "varlist not allowed"
                             : parsed.ifExpr.has_value()     ? "if not allowed"
                             : parsed.inRange.has_value()    ? "in range not allowed"
                             : parsed.weightType.has_value() ? "weights not allowed"
                                                             : nullptr;
    if (notAllowed != nullptr) {
      blocks.push_back(ojson(eval::errorBlock("syntax_error", notAllowed, std::nullopt, std::nullopt,
                                              std::nullopt, "r(101)")));
      return blocks;
    }
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabMarginsplot.known,
                                              kVocabMarginsplot.unimplemented)) {
      blocks.push_back(ojson(*err));
      return blocks;
    }
    ojson result = ojson(handlers::runMarginsplot(parsed.opts, session));
    graphAcceptedNotApplied(result, "marginsplot", parsed.opts, kMarginsplotApplied);
    registerGraphOutput(result, session, parsed.opts, graphLineTyped);  // BS-011
    blocks.push_back(result);
    return blocks;
  }

  // serset-twoway SS: the `serset` command family (serset.sthlp; piet/serset.h).
  // It needs no data: bare `serset` with nothing in memory is rc 0 with r(id) -1
  // (w2al_twoway_family.log:177).
  if (cmd == "serset") {
    bool clearOpt = false;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "clear") {
        clearOpt = true;
        continue;
      }
      blocks.push_back(ojson(eval::errorBlock("syntax_error", "option " + k + " not allowed",
                                              std::nullopt, std::nullopt, std::nullopt, "r(198)")));
      return blocks;
    }
    ojson r = serset::runSerset(session, parsed.varlist, clearOpt);
    for (auto& b : r) blocks.push_back(b);
    return blocks;
  }

  // ------------------------------------------------------------------
  // M4a item 4 + M4b: graph JSON surface. Scope: scatter/histogram/
  // kdensity/line/xtline/tsline + the bare (non-parenthesised) `twoway
  // PLOTTYPE var...` single-plot form (M4a); `graph export`/`graph save`/
  // `graph combine` (M4b — see graph_m4.cpp's runGraphExport/Save/Combine).
  // The parenthesised multi-layer `twoway (rarea ...) (scatter ...)` form
  // (twoway_opts.py/twoway_parse.py) remains M4a-remaining/out of scope —
  // rejected explicitly below, not silently mis-parsed.
  if (cmd == "scatter" || cmd == "histogram" || cmd == "hist" || cmd == "kdensity" ||
      cmd == "line" || cmd == "xtline" || cmd == "tsline" || cmd == "twoway" || cmd == "graph") {
    std::string gc = cmd;
    std::vector<std::string> vl = parsed.varlist;
    std::optional<std::string> ifExpr = parsed.ifExpr;

    if (cmd == "graph") {
      std::string sub = parsed.varlist.empty() ? std::string() : lower(parsed.varlist[0]);
      // graph.ado's subcommand abbreviations: `do` == bsubstr(full, 1,
      // max(min, length(do))).
      auto subIs = [&](const char* full, size_t minLen) {
        const std::string f(full);
        return sub.size() >= minLen && sub.size() <= f.size() && f.compare(0, sub.size(), sub) == 0;
      };
      // graph-family AI-4/AI-6/AI-7/AI-9: graph describe/copy/rename/display/
      // draw/close/set/query (graph_store.cpp).
      {
        const std::string subArgs = [&]() {
          std::string a = rawArgsAfterCmd(rawArgsAfterCmd(parsed.raw));
          const long long c = eval::findUnquotedComma(a);
          if (c >= 0) a = a.substr(0, static_cast<size_t>(c));
          return trimWs(a);
        }();
        if (sub == "draw" || subIs("display", 2)) {
          ojson r = handlers::runGraphDisplay(session, subArgs, parsed.opts);
          for (auto& b : r) blocks.push_back(b);
          return blocks;
        }
        if (subIs("describe", 1)) {
          ojson r = handlers::runGraphDescribe(session, subArgs);
          for (auto& b : r) blocks.push_back(b);
          return blocks;
        }
        if (sub == "copy" || sub == "rename") {
          bool replace = false;
          for (const auto& [k, v] : parsed.opts) {
            if (k == "replace") {
              replace = true;
              continue;
            }
            blocks.push_back(ojson(eval::errorBlock("syntax_error", "option " + k + " not allowed",
                                                    std::nullopt, std::nullopt, std::nullopt, "r(198)")));
            return blocks;
          }
          std::vector<std::string> names(parsed.varlist.begin() + 1, parsed.varlist.end());
          ojson r = handlers::runGraphCopyRename(session, sub == "rename", names, replace);
          for (auto& b : r) blocks.push_back(b);
          return blocks;
        }
        // graph close: windows only; a batch run has none (w2ai gf_close_065/062
        // rc 0). graph set: printer/window/export settings
        // (gf_set_window, gf_set_ps rc 0); nothing here reads them.
        // graph query lists the settings.
        if (sub == "close" || sub == "set" || subIs("query", 1)) return blocks;
      }
      if (sub == "export") {
        // The filename can contain spaces (the Korean Desktop is "바탕 화면", and
        // any "My Folder\..." path), so extract it from the RAW command like
        // `use` does — parsed.varlist[1] is whitespace-tokenized and truncates a
        // spaced path at the first space (-> ext "" -> "unsupported format ''").
        // BUG-251/WS-Q: was `lower(raw).find("export")` + `substr(pos + 6)`.
        // npos-GUARDED, so this pair never crashed — but it is the same
        // re-derivation, with a literal copy of a SUB-command spelling, and its
        // npos path degrades silently to the whitespace-tokenized varlist that
        // the site exists to avoid. Two token-skips is what "after `graph
        // export`" means, and it needs no spelling at all.
        std::string exAfter = rawArgsAfterCmd(rawArgsAfterCmd(parsed.raw));
        size_t exComma = exAfter.find(',');  // drop trailing `, replace`/`width()` options
        if (exComma != std::string::npos) exAfter = trimWs(exAfter.substr(0, exComma));
        if (exAfter.size() >= 2 && ((exAfter.front() == '"' && exAfter.back() == '"') ||
                                    (exAfter.front() == '\'' && exAfter.back() == '\'')))
          exAfter = exAfter.substr(1, exAfter.size() - 2);
        std::string filepath = !exAfter.empty()
                                   ? exAfter
                                   : (parsed.varlist.size() > 1 ? parsed.varlist[1] : std::string());
        if (filepath.empty()) {
          blocks.push_back(ojson(eval::engineError("syntax_error: `graph export` requires a filename")));
          return blocks;
        }
        blocks.push_back(ojson(handlers::runGraphExport(filepath, parsed.opts, session)));
        return blocks;
      }
      if (sub == "save") {
        // graph save: `graph save [name] filename [, replace asis]` --
        // one word is the file, two are the graph name and the file; each may be
        // quoted (w2ai gf_save_007 `graph save "Graph" "file"`, gf_save_gp3
        // `graph save gp3 "file"`, log:1478-1491). The raw text is read so a
        // quoted path with blanks stays whole.
        std::optional<std::string> name;
        std::optional<std::string> filepath;
        std::string svAfter = rawArgsAfterCmd(rawArgsAfterCmd(parsed.raw));
        const long long svComma = eval::findUnquotedComma(svAfter);
        if (svComma >= 0) svAfter = svAfter.substr(0, static_cast<size_t>(svComma));
        std::vector<std::string> words;
        {
          size_t i = 0;
          while (i < svAfter.size()) {
            while (i < svAfter.size() && std::isspace(static_cast<unsigned char>(svAfter[i]))) ++i;
            if (i >= svAfter.size()) break;
            if (svAfter[i] == '"' || svAfter[i] == '\'') {
              const char q = svAfter[i];
              const size_t close = svAfter.find(q, i + 1);
              words.push_back(svAfter.substr(i + 1, close == std::string::npos ? std::string::npos
                                                                                : close - i - 1));
              i = close == std::string::npos ? svAfter.size() : close + 1;
              continue;
            }
            size_t j = i;
            while (j < svAfter.size() && !std::isspace(static_cast<unsigned char>(svAfter[j]))) ++j;
            words.push_back(svAfter.substr(i, j - i));
            i = j;
          }
        }
        if (words.empty()) {
          blocks.push_back(ojson(eval::errorBlock("syntax_error", "filename name required", std::nullopt,
                                                  std::nullopt, std::nullopt, "r(198)")));
          return blocks;
        }
        if (words.size() > 2) {
          blocks.push_back(ojson(eval::errorBlock("syntax_error", "invalid syntax, " + words[2] + " not allowed",
                                                  std::nullopt, std::nullopt, std::nullopt, "r(198)")));
          return blocks;
        }
        if (words.size() == 2) {
          name = words[0];
          filepath = words[1];
        } else {
          filepath = words[0];
        }
        blocks.push_back(ojson(handlers::runGraphSave(name, filepath, parsed.opts, session)));
        return blocks;
      }
      if (sub == "combine") {
        std::vector<std::string> names(parsed.varlist.begin() + 1, parsed.varlist.end());
        // graph-family AI-4: name() onto a graph in memory without replace is r(110).
        const std::string cbNameOpt = graphNameOption(parsed.opts);
        if (auto clash = serset::nameCollision(session, cbNameOpt)) {
          blocks.push_back(*clash);
          return blocks;
        }
        ojson combined = ojson(handlers::runGraphCombine(names, parsed.opts, session));
        ojson cbSavedNote;  // BUG-1529: `file v10.gph saved` (StataBE 19.5, g1.do V10)
        if (combined.is_object() && combined.value("type", std::string()) == "graph") {
          // graph-family AI-4: the combined graph registers like any other
          // (family `combine`, w2ai gf_cmb_desc_dc log:1812-1813), and its
          // saving() writes it (AI-11, gf_cmb_019_f log:1774-1787).
          const ojson cbMeta = graphMetaFor(graphLineTyped, "combine", parsed.opts);
          serset::registerGraph(session, combined, cbNameOpt, {}, &cbMeta);
          std::optional<std::string> savingOpt;
          for (const auto& [k, v] : parsed.opts) {
            if (k == "saving" && !v.isBool) savingOpt = v.strValue;
          }
          if (savingOpt.has_value()) {
            ojson se = handlers::saveGraphOption(combined, *savingOpt, session, nullptr, &cbSavedNote);
            if (!se.is_null()) {
              blocks.push_back(se);
              return blocks;
            }
          }
        }
        // W4: runGraphCombine reads exactly these eight (graph_m4.cpp:2606-2642).
        // The corpus also hands it `scale()` (25 uses) and `xcommon` (1), which
        // it does not read; those now resolve (kGraphCosmetic) and are named in
        // the note instead of vanishing. No reject net is added here: this
        // sub-command arm never had one, and widening it is W4's neighbour, not
        // W4 -- what the wave owes is that an accepted option is never silent.
        static const std::unordered_set<std::string> kCombineApplied = {
            "rows", "cols", "title", "subtitle", "note", "name", "imargin", "graphregion", "saving"};
        graphAcceptedNotApplied(combined, "graph combine", parsed.opts, kCombineApplied);
        blocks.push_back(combined);
        if (!cbSavedNote.is_null()) blocks.push_back(cbSavedNote);
        return blocks;
      }
      // serset-twoway GST / HIST-4: `graph dir [pattern] [, memory gph detail]`
      // (graph dir): r(list) = the in-memory names (byte-sorted) and the *.gph
      // files, `"`mlist' `glist'"'` (w2al tf_ss_two_gd `tf_g1 tf_g2 `, log:5510).
      // serset-twoway GST: `graph use filename [, name() nodraw ...]`.
      if (sub == "use") {
        std::string useAfter = rawArgsAfterCmd(rawArgsAfterCmd(parsed.raw));
        const long long uc = eval::findUnquotedComma(useAfter);
        if (uc >= 0) useAfter = useAfter.substr(0, static_cast<size_t>(uc));
        ojson r = handlers::runGraphUse(trimWs(useAfter), parsed.opts, session);
        for (auto& b : r) blocks.push_back(b);
        return blocks;
      }
      if (sub == "dir") {
        bool memOpt = false, gphOpt = false;
        for (const auto& [k, v] : parsed.opts) {
          if (k == "memory" || k == "mem" || k == "m") {
            memOpt = true;
          } else if (k == "gph" || k == "g") {
            gphOpt = true;
          } else if (k == "detail" || k == "d") {
            continue;
          } else {
            blocks.push_back(ojson(eval::errorBlock("syntax_error", "option " + k + " not allowed",
                                                    std::nullopt, std::nullopt, std::nullopt,
                                                    "r(198)")));
            return blocks;
          }
        }
        const std::string pattern = parsed.varlist.size() > 1 ? parsed.varlist[1] : std::string();
        std::string cwdDir;
        if (session.cwd.has_value() && !session.cwd->empty()) {
          cwdDir = *session.cwd;
        } else {
          std::error_code cwdEc;
          cwdDir = std::filesystem::current_path(cwdEc).u8string();
        }
        ojson r = serset::runGraphDir(session, pattern, memOpt, gphOpt, cwdDir);
        for (auto& b : r) blocks.push_back(b);
        return blocks;
      }
      if (sub == "drop") {
        // BS-056: `graph drop {namelist | _all}` -- only `parsed.varlist[1]`
        // (the FIRST name) was ever read, so `graph drop g1 g2` silently
        // dropped g1 and said nothing about g2; a name that didn't match
        // ANY registered graph (including a literal "all" typed without
        // the leading underscore Stata's own grammar requires) hit the same
        // `erase()` no-op-if-absent call and still reported "(graph
        // dropped)" — a silent false success either way. Now: every name
        // token is processed; `_all` (case-insensitive, anywhere in the
        // list) clears the whole named-graph store; otherwise every name is
        // validated to exist BEFORE any of them are dropped (all-or-nothing,
        // no partial drop on error) and an unknown name is an honest reject
        // naming it, never a silent no-op.
        std::vector<std::string> names(parsed.varlist.begin() + 1, parsed.varlist.end());
        bool dropAll = false;
        for (const auto& nm : names) {
          if (lower(nm) == "_all") {
            dropAll = true;
            break;
          }
        }
        if (dropAll) {
          session.namedGraphs.clear();
          session.graphMeta.clear();  // graph-family AI-4
          session.currentGraph.clear();
          // serset-twoway SS: a graph's sersets go with it (w2al tf_ss_afterdrop_sc
          // `0|-1|`, log:5525).
          serset::dropGraphSersets(session, {}, true);
        } else if (!names.empty()) {
          for (const auto& nm : names) {
            if (session.namedGraphs.count(nm) == 0) {
              // graph drop: `graph X not found` / `no graphs dropped`, r(111)
              // (w2ai gf_drop_absent, log:2252-2259).
              blocks.push_back(ojson(eval::errorBlock("engine_error",
                                                      "graph " + nm + " not found\nno graphs dropped",
                                                      std::nullopt, std::nullopt, std::nullopt,
                                                      "r(111)")));
              return blocks;
            }
          }
          for (const auto& nm : names) {
            session.namedGraphs.erase(nm);
            session.graphMeta.erase(nm);
            if (session.currentGraph == nm) session.currentGraph.clear();
          }
          serset::dropGraphSersets(session, names, false);
        }
        ojson dropOut;
        dropOut["type"] = "display";
        dropOut["class"] = "note";
        dropOut["text"] = "(graph dropped)";
        blocks.push_back(dropOut);
        return blocks;
      }
      // `graph twoway ...` / other `graph <sub>` forms: strip the leading
      // "graph" token's sub-command and fall through to the plot dispatch
      // below (mirrors commands.py's final `sub_varlist = parsed.varlist[1:]`
      // fallthrough).
      // serset-twoway WT: the `graph` row forwards weight clauses now (for `graph
      // twoway`); the sub-commands that do not take one here (bar/hbar/box/dot/
      // pie are not implemented; export/save/combine/drop/dir/use take none)
      // keep the refusal the tokenizer gate gave them, verbatim.
      // graph-family AI-3: graph bar/hbar/box/hbox take [aw fw pw] (graph_bar.sthlp
      // weights; w2ai gf_bar_aw/fw/pw, gf_box_aw); their handler answers iw.
      if (parsed.weightType.has_value() && !parsed.weightType->empty() &&
          !(sub == "twoway" || sub == "tw" || sub == "two" || sub == "twow" || sub == "twowa" ||
            sub == "bar" || sub == "hbar" || sub == "box" || sub == "hbox")) {
        blocks.push_back(ojson(eval::unsupportedOption(
            "[" + *parsed.weightType + "=" + parsed.weightVar.value_or("") + "]",
            "`graph` does not support weighted estimation in this engine (no "
            "[aw=]/[fw=]/[pw=]/[iw=] support for this command). Remove the weight specification "
            "— unweighted estimation will differ from weighted results.")));
        return blocks;
      }
      if (sub == "twoway" || sub == "tw" || sub == "two" || sub == "twow" || sub == "twowa") {
        // serset-twoway PARA: `gr tw (...)` is `graph twoway` (w2al tf_q_051).
        gc = "twoway";
        vl = std::vector<std::string>(parsed.varlist.begin() + 1, parsed.varlist.end());
      } else if (!sub.empty()) {
        gc = sub;
        vl = std::vector<std::string>(parsed.varlist.begin() + 1, parsed.varlist.end());
      } else {
        gc = "scatter";
      }
    }

    if (!session.hasData()) {
      blocks.push_back(ojson(eval::noDataLoaded()));
      return blocks;
    }

    // serset-twoway IN: the twoway family (scatter/line/tsline/twoway/graph)
    // honours `in` now -- the layer path reads each plot's own and the global
    // one, the single-plot path filters by it below. The other graph commands
    // keep the refusal.
    const bool inServed = cmd == "scatter" || cmd == "line" || cmd == "tsline" || cmd == "twoway" ||
                          cmd == "graph";
    if (!inServed && parsed.inRange.has_value() && !parsed.inRange->empty()) {
      blocks.push_back(ojson(eval::engineError(
          "graph: `in` range is not yet supported for graph commands; use `if`.",
          "syntax_error")));
      return blocks;
    }

    // `twoway (plottype ...) (plottype ...) ...` (parenthesised multi-layer
    // form): route through runTwowayLayers when raw_args contains a
    // top-level paren group — mirrors dispatcher.py's "Stage-4: multi-layer
    // twoway" branch (raw_args extraction regex `^\s*twoway\b\s*(.*)$` /
    // `^\s*graph\s+twoway\b\s*(.*)$`, checked BEFORE the bare single-plot
    // form below so a parenthesised layer group never falls through to the
    // bare-form plot-type stripping).
    // serset-twoway PARA: `scatter y x ... || scatter ... || lfit ...` (the
    // scatter/line/tsline COMMAND with a top-level `||`) is the same twoway graph
    // (w2al tf_tw_082).
    std::string cmdLevelPipeArgs;
    if (cmd == "scatter" || cmd == "line" || cmd == "tsline") {
      const std::string after = rawArgsAfterCmd(parsed.raw);
      if (expandDoublePipeLayers(after) != after) {
        gc = "twoway";
        cmdLevelPipeArgs = cmd + " " + after;
      }
    }
    if (gc == "twoway") {
      static const std::regex reTwowayRaw(R"(^\s*twoway\b\s*(.*)$)", std::regex::icase);
      static const std::regex reGraphTwowayRaw(
          R"(^\s*graph\s+(?:tw|two|twow|twowa|twoway)\b\s*(.*)$)", std::regex::icase);
      std::string rawArgs;
      std::smatch m;
      // AER-SCOPE1 bundle A: `tw (sc y x)` and `gr twoway ...` resolve to the
      // canonical `cmd` at dispatch, but THESE regexes read the raw line, which
      // still carries what the user typed. rawWithCanonicalCmd rewrites just the
      // leading word (BUG-251's fix, reused here) so the abbreviation reaches
      // the same layer parser as the full spelling.
      const std::string graphRaw = rawWithCanonicalCmd(parsed.raw, cmdTyped, cmd);
      if (!cmdLevelPipeArgs.empty()) {
        rawArgs = cmdLevelPipeArgs;
      } else if (cmd == "graph" && std::regex_match(graphRaw, m, reGraphTwowayRaw)) {
        rawArgs = m[1].str();
      } else if (std::regex_match(graphRaw, m, reTwowayRaw)) {
        rawArgs = m[1].str();
      }
      // BS-053: `||`-separated layers, no parens at all -- synthesize the
      // parenthesised equivalent BEFORE the hasToplevelParens gate below so
      // it takes the exact same multi-layer path a hand-parenthesised
      // command would (see expandDoublePipeLayers' doc comment above). A
      // no-op (returns the input unchanged) when there's no top-level `||`,
      // or when the command already used real parens.
      if (!rawArgs.empty() && !twoway::hasToplevelParens(rawArgs)) {
        rawArgs = expandDoublePipeLayers(rawArgs);
      }
      // serset-twoway PARB/FN/SCI: a bare `twoway function ...` / `twoway
      // scatteri ...` is read by the layer parser too: its text is an expression
      // or an immediate point list, never a varlist (w2al tf_fn_noy, tf_sci_lab).
      bool bareImmediate = false;
      {
        const std::string ra = trimWs(rawArgs);
        size_t he = 0;
        while (he < ra.size() && !std::isspace(static_cast<unsigned char>(ra[he])) && ra[he] != ',') ++he;
        const std::string head = lower(ra.substr(0, he));
        bareImmediate = head == "function" || head == "scatteri";
      }
      if (!rawArgs.empty() && (twoway::hasToplevelParens(rawArgs) || bareImmediate)) {
        twoway::ParseResult pr = twoway::parseTwoway(rawArgs);
        // W4: the layer parser reads the comma side ITSELF (twoway_parse.cpp's
        // parseLayerInterior / parseTwoway step 1), so these keys never passed
        // through the dispatcher's canonicalizeForCommand and arrived at
        // layerStyle spelled the way the user typed them. That is BUG-233's
        // shape exactly: the net (now) accepts `lc(gs11)` as a prefix of
        // `lcolor`, and layerStyle -- which compares raw keys and reads `lcolor`,
        // `lw`, `lp`, `mc`, `ms`, `msi`, `mz` but NOT `lc` -- then dropped it
        // unread. Corpus-measured: `lc` (4 uses), `msymb` (14), `xti`/`yti`,
        // `xlab`/`ylab`, `xsc`/`ysc`, `leg`, `horiz`. Every one of them is a
        // unique prefix of a member of the registered vocabulary, so the prefix
        // rule reaches all of them and the registration needs no declared-alias
        // table (BUG-430's mechanism is for the ones a prefix CANNOT reach, and
        // this surface has none in the corpus).
        for (auto& lspec : pr.layers) {
          eval::canonicalizeForCommand(lspec.opts, "twoway_layer");
        }
        eval::canonicalizeForCommand(pr.globalOpts, "twoway_layer");
        hoistTwowayLevelOptions(pr);  // serset-twoway PARA
        if (pr.layers.empty()) {
          blocks.push_back(ojson(eval::engineError(
              "twoway: no plot layers found. Check syntax.", "syntax_error")));
          return blocks;
        }
        // serset-twoway PARA: the layer parser's global block is the whole
        // truth. The tokenizer's opts (parsed.opts) were merged in here, but
        // the tokenizer cuts at the FIRST comma, which in `plot1, opts || plot2`
        // or `(plot1, opts) (plot2)` lies inside a plot, so its keys were the
        // plot heads, `||` and variable names (`option || not allowed`).
        std::vector<std::pair<std::string, eval::OptValue>> merged = pr.globalOpts;
        // Use ONLY the layer parser's global if — the tokenizer's if_expr is
        // meaningless here (its greedy `\bif` match swallows paren groups).
        Datastore* dsPtrTw = session.datastore();
        // W4: the unknown-option net for BOTH planes (the global block and each
        // layer's interior) lives inside runTwowayLayers, AFTER its
        // supported-plot-type pre-scan, so `(rcap ...)` still answers "plot type
        // is not supported" rather than complaining about `lstyle`. See
        // graph_m4.cpp. Only the note is assembled here, because only the
        // dispatcher knows which of the two routes produced the block.
        //
        // The note reads the global block AND every layer interior: a cosmetic
        // option written inside a plot's own parentheses is just as silently
        // dropped as one written after the comma, so both are reported.
        std::vector<std::pair<std::string, eval::OptValue>> twNoteOpts = merged;
        for (const auto& lspec : pr.layers) {
          for (const auto& kv : lspec.opts) twNoteOpts.push_back(kv);
        }
        // serset-twoway SS: name() onto a graph in memory without replace is
        // r(110) before anything is drawn (w2al tf_st_dup2, log:5024-5029).
        const std::string twNameOpt = graphNameOption(merged);
        if (auto clash = serset::nameCollision(session, twNameOpt)) {
          blocks.push_back(*clash);
          return blocks;
        }
        // serset-twoway QIF: a plot's own `if` (or the global one) holding a
        // string function or a compare against "" is read by the row evaluator
        // through a hidden mask column, as every other command's is (dispatchImpl's
        // QualifierColumnGuard, which removes the columns when this returns); the
        // engine's f64 evaluator cannot (w2al tf_q_039 log:4760-4767,
        // tf_q_emptystr, tf_q_globalstr).
        {
          auto routeCondition = [&](std::optional<std::string>& cond) -> std::optional<ojson> {
            if (!cond.has_value() || cond->empty() || !session.hasData() ||
                !strmutate::needsConditionMask(session, *cond)) {
              return std::nullopt;
            }
            strmutate::ConditionMaskInstall mi = strmutate::installConditionMask(session, *cond);
            if (!mi.ok) return mi.error;
            conditionMask.columns.push_back(mi.column);
            cond = mi.condition;
            return std::nullopt;
          };
          if (auto e = routeCondition(pr.ifExpr)) {
            blocks.push_back(*e);
            return blocks;
          }
          for (auto& l : pr.layers) {
            if (auto e = routeCondition(l.ifExpr)) {
              blocks.push_back(*e);
              return blocks;
            }
          }
          dsPtrTw = session.datastore();
        }
        std::vector<serset::PlotSpec> twPlots;
        ojson twResult = ojson(handlers::runTwowayLayers(dsPtrTw, session, pr.layers, merged, pr.ifExpr,
                                                         &twPlots, pr.inRange));
        {
          std::unordered_set<std::string> twApplied = {"title", "subtitle", "xtitle", "ytitle",
                                                       "note", "name"};
          twApplied.insert(kGraphFitApplied.begin(), kGraphFitApplied.end());
          twApplied.insert(kGraphTwowayGlobals.begin(), kGraphTwowayGlobals.end());
          twApplied.insert(kGraphLayerStyle.begin(), kGraphLayerStyle.end());
          twApplied.insert("sort");  // line/connected/area/spike/dropline/bar order by x only under it (BUG-404)
          graphAcceptedNotApplied(twResult, "twoway", twNoteOpts, twApplied);
        }
        ojson twSavedNote;  // BUG-1529: `file n2sv1.gph saved` (StataBE 19.5, a15.do)
        if (twResult.is_object() && twResult.value("type", std::string()) == "graph") {
          // serset-twoway SS: the one registration (lastGraph, namedGraphs, the
          // graph's sersets; piet/serset.h).
          const ojson twMeta = graphMetaFor(graphLineTyped, "twoway", merged);  // graph-family AI-4
          serset::registerGraph(session, twResult, twNameOpt,
                                serset::buildPlotSersets(std::move(twPlots)), &twMeta);
          // serset-twoway GST: saving() writes the graph and its sersets (w2al
          // tf_tw_069a_f, tf_st_sav_f; r(602) without replace onto a file that
          // exists: tf_st_noreplace, log:5131-5138).
          std::optional<std::string> savingOpt;
          for (const auto& [k, v] : merged) {
            if (k == "saving" && !v.isBool) savingOpt = v.strValue;
          }
          if (savingOpt.has_value()) {
            const serset::GraphName gn = serset::parseNameOption(twNameOpt);
            const std::vector<serset::Serset> owned =
                serset::ownedBy(session, gn.name.empty() ? std::string("Graph") : gn.name);
            ojson se = handlers::saveGraphOption(twResult, *savingOpt, session, &owned, &twSavedNote);
            if (!se.is_null()) {
              blocks.push_back(se);
              return blocks;
            }
          }
        }
        handlers::decimateGraphBlockForView(twResult);  // L2-FIX-B (BUG-066): live-view only
        blocks.push_back(twResult);
        if (!twSavedNote.is_null()) blocks.push_back(twSavedNote);
        return blocks;
      }
    }

    // serset-twoway HKL: `twoway kdensity|histogram var` is the twoway PLOT
    // TYPE (n 300 over [min, max]; no start() unless given), not the
    // kdensity/histogram COMMAND (n 50 over a grid widened by the bandwidth;
    // start(r(min)); the bin note): w2al tf_kd_n log:3524-3540, tf_hist_082.
    // It takes the layer renderer like the range plots below.
    bool twowayKdHist = false;
    // `twoway PLOTTYPE var...` (bare, non-parenthesised form): strip the
    // leading plot-type keyword.
    if (gc == "twoway") {
      // BS-054: `twoway kdensity x` / `twoway histogram x` (the umbrella-
      // prefixed spellings) previously weren't in this set at all, so
      // "kdensity"/"histogram" fell straight through as an ordinary
      // varlist token — `gc` degraded to "scatter" and `vl` still started
      // with the literal word "kdensity"/"histogram", which runScatter then
      // tried to look up AS A VARIABLE ("variable kdensity not found" —
      // exactly the reported symptom). The un-prefixed bare forms
      // (`kdensity x` / `histogram x`, no leading "twoway") already worked
      // fine via their own top-level `cmd` dispatch below; adding these two
      // names here just makes the "twoway "-prefixed spelling strip down to
      // the SAME already-working gc=="kdensity"/gc=="histogram" branch
      // further down this function (no new handler needed).
      static const std::set<std::string> kTwowayPlotTypes = {
          "line",     "scatter", "connected", "area",      "bar",      "spike",
          "dot",      "dropline", "rarea",    "rcap",      "rcapsym",
          "kdensity", "histogram", "hist",
          // serset-twoway FIT/PARB: the fit plot types are ordinary bare types
          // (w2al tf_lfit_range `twoway lfit y x, range(-2 14)`).
          "lfit",     "qfit",    "lfitci",    "qfitci",
          "rspike",   "rline",   "rconnected", "rscatter",  // serset-twoway RNG
      };
      if (!vl.empty() && kTwowayPlotTypes.count(lower(vl[0]))) {
        gc = lower(vl[0]);
        vl = std::vector<std::string>(vl.begin() + 1, vl.end());
        twowayKdHist = gc == "kdensity" || gc == "histogram" || gc == "hist";
      } else {
        gc = "scatter";
      }
    }

    // serset-twoway SS: name() onto a graph in memory without replace is r(110)
    // before anything is drawn (w2al tf_st_dup2, log:5024-5029).
    const std::string bareNameOpt = graphNameOption(parsed.opts);
    if (auto clash = serset::nameCollision(session, bareNameOpt)) {
      blocks.push_back(*clash);
      return blocks;
    }

    // serset-twoway QIF: a string condition takes the row-mask route (see the
    // layer path above).
    if (ifExpr.has_value() && !ifExpr->empty() &&
        strmutate::needsConditionMask(session, *ifExpr)) {
      strmutate::ConditionMaskInstall mi = strmutate::installConditionMask(session, *ifExpr);
      if (!mi.ok) {
        blocks.push_back(mi.error);
        return blocks;
      }
      conditionMask.columns.push_back(mi.column);
      ifExpr = mi.condition;
    }

    Datastore* dsPtr = session.datastore();
    std::string tmpGraphHbin;
    std::unique_ptr<Datastore> tmpDs;
    const bool bareIn = parsed.inRange.has_value() && !parsed.inRange->empty();
    if (bareIn) {
      // serset-twoway IN: `in` (with or without `if`) on the single-plot path.
      try {
        handlers::InRangeFilterResult fr =
            handlers::applyIfAndInFilter(session, *session.currentHbin(), ifExpr, parsed.inRange);
        if (!fr.ok) {
          blocks.push_back(handlers::inRangeRejectError(fr, *parsed.inRange, gc));
          return blocks;
        }
        if (fr.dataPath != *session.currentHbin()) tmpGraphHbin = fr.dataPath;
        tmpDs = std::make_unique<Datastore>(fr.dataPath);
        dsPtr = tmpDs.get();
      } catch (const CobraError& ce) {
        blocks.push_back(ojson(eval::engineError(cobraErrorMessage(ce), ce.code)));
        return blocks;
      }
    } else if (ifExpr.has_value() && !ifExpr->empty()) {
      tmpGraphHbin = dispatcherNextPath(session.workdir(), "graphif");
      // BUG-369 follow-up: `twoway ... if L.y>0` — same injection as above.
      std::vector<std::string> args = translate::injectTsContextForExpr(
          {"eval", "--data", dsPtr->path(), "--keepif", *ifExpr, "--out", tmpGraphHbin},
          *ifExpr, session);
      try {
        runCobraJson(args);
      } catch (const CobraError& ce) {
        // BUG-038(a): route through the shared mapping point instead of
        // formatting "[code] message" inline (piet::cobraErrorMessage,
        // cobra_runner.h) — same fix as every other CobraError catch site.
        blocks.push_back(ojson(eval::engineError(cobraErrorMessage(ce), ce.code)));
        return blocks;
      }
      tmpDs = std::make_unique<Datastore>(tmpGraphHbin);
      dsPtr = tmpDs.get();
    }

    ojson result;
    std::optional<ojson> histOptErr;  // BUG-1306: histogram's unknown option, emitted after its note
    // BUG-034: `graph bar/hbar/pie/box/dot ...` used to fall straight
    // through to the `else` below and get misrouted through runScatter,
    // producing the misleading "scatter requires two variables" error
    // instead of an honest gap (root cause: unrecognized graph
    // sub-commands set gc=sub above with nothing downstream to catch
    // them). Mirrors the parenthesised multi-layer `twoway (plottype ...)`
    // form's reject-before-output guard (runTwowayLayers' supported-plot-type
    // pre-scan, graph_m4.cpp — BUG-129) — applied here to the single-plot
    // dispatch path so these name the gap explicitly (SPEC I-3) rather than
    // silently misrouting to scatter's grammar.
    static const std::set<std::string> kUnsupportedBareGraphTypes = {
        "bar", "hbar", "pie", "box", "hbox", "dot",
    };
    // BUG-129/BUG-130 (graph-honesty): the bare (non-parenthesised)
    // `twoway rarea ...` / `twoway rcap ...` / `twoway area ...` etc. spellings
    // previously fell through to runScatter below — `rarea` silently
    // misrendered as a 2-var scatter (its band never built; BUG-130) and every
    // unsupported range-plot type drew an empty/wrong plot with rc=0 (BUG-129
    // silent-drop). Route these twoway-specific plot types through the SAME
    // layer machinery the parenthesised form uses (a single synthesised
    // layer): rarea reaches its real band builder, and every unsupported type
    // gets runTwowayLayers' honest reject-before-output. scatter/line/
    // connected/kdensity/histogram keep their dedicated single-plot handlers
    // here (established JSON shapes + selftests unchanged).
    static const std::set<std::string> kBareTwowayLayerRoute = {
        "rarea", "rcap", "rcapsym", "area", "spike", "dropline",
        "lfit",  "qfit", "lfitci",  "qfitci",  // serset-twoway FIT
        "rspike", "rline", "rconnected", "rscatter",  // serset-twoway RNG
    };
    // BUG-336 (graph arm): this arm had NO unknown-option net at all — nine
    // command tokens accepting any option name and dropping it. The known sets
    // below are MEASURED from graph_m4.cpp's handler bodies (optStr/optBool/
    // optTitleStr reads), not guessed:
    //   every plot: title/xtitle/ytitle/note      (runScatter:271, runGraphLine:849,
    //                                              runXtline:950, runTsline:1054)
    //   histogram : width/bin/freq/frequency/fraction/percent/normal (:387-521)
    //   kdensity  : kernel/bwidth/n/normal                            (:683-794)
    //   xtline    : overlay                                           (:980)
    // `name` is read by the arm itself (namedGraphs capture below), and `legend`
    // is ACCEPTED though nobody reads it: p5.do:167 uses it, so it is corpus-live,
    // and a graph is a visual surface where an unhonoured hint is not a wrong
    // answer — the same accept-visual-hint call already made for `list, noobs`.
    // Options Stata has and PIET does not honour are listed as unimplemented so
    // they say so instead of vanishing.
    // ORDER IS LOAD-BEARING: the unsupported-plot-type and bare-twoway-layer
    // routes below own their own honest rejects, and those are about the COMMAND,
    // not its options. Running this net first made `graph bar y, over(x)` answer
    // "option `over` not allowed" instead of "graph bar is not implemented" —
    // caught by four existing selftest legs (mission 2 / BUG-105(1)), which is
    // exactly what they are for.
    // AER scope-2 W2: the four literal sets below moved to file scope, one
    // copy each, and are now REGISTERED per command token (kVocabGraphBase and
    // friends). This arm is unchanged: it still assembles gknown from gc — the
    // resolved PLOT TYPE, which for `twoway`/`graph` is derived from the
    // varlist and is not the dispatched token — and the registration exists so
    // the census can see the vocabulary, not to decide anything here.
    if (kUnsupportedBareGraphTypes.count(gc) == 0 && kBareTwowayLayerRoute.count(gc) == 0 &&
        !twowayKdHist) {
      std::unordered_set<std::string> gknown = kGraphSharedKnown;
      const std::unordered_set<std::string>* gunimpl = &kGraphBaseUnimpl;
      if (gc == "histogram" || gc == "hist") {
        gknown.insert(kGraphHistExtra.begin(), kGraphHistExtra.end());
        gunimpl = &kGraphHistUnimpl;
      } else if (gc == "kdensity") {
        gknown.insert(kGraphKdensExtra.begin(), kGraphKdensExtra.end());
        gunimpl = &kGraphKdensUnimpl;
      } else if (gc == "xtline") {
        gknown.insert(kGraphXtlineExtra.begin(), kGraphXtlineExtra.end());
      }
      if (auto err = eval::rejectUnknownOptions(parsed.opts, gknown, *gunimpl)) {
        // histogram refuses an option it does not know only after it has
        // printed the (bin=..) note (StataBE 19.5, BUG-1306: note, then
        // r(198)). The histogram branch below emits it after runHistogram.
        if (gc != "histogram" && gc != "hist") {
          blocks.push_back(ojson(*err));
          return blocks;
        }
        histOptErr = ojson(*err);
      }
    }
    // W4 -- THE WIRING GAP, and the reason this is a conditional re-route and
    // not an unconditional one.
    //
    // MEASURED on main today: `scatter y x, xline(0)` was r(198) while
    // `twoway (scatter y x) (line y x), xline(0)` drew the reference line. The
    // renderer for xline/yline/xscale/yscale/xlabel/ylabel/legend/subtitle
    // exists (graph_m4.cpp's `axes`/`legend` block, and graph.js draws both ONLY
    // for kind=="twoway"); the single-plot arm simply could not reach it. So the
    // fix is routing, not a new feature.
    //
    // Unconditional routing is what the plan REFUSED, and the pins say why: the
    // freeze fixtures scatter/graph/line/tsline/histogram/kdensity all pin BARE
    // commands byte for byte, with `"kind":"scatter"` and the flat
    // points/x_var/y_var shape, and two self-test legs (selftest.cpp:3981,
    // :21887) assert kind=="scatter" for a bare `scatter`. A bare or title-only
    // command therefore stays exactly where it is; only a statement that carries
    // one of the eight layer-honoured globals moves.
    //
    // `vl.size() == 2` is load-bearing and was measured too: `scatter y1 y2 x` is
    // Stata's shared-x multi-series form and runScatter builds a `series` array
    // for it (BS-050, graph_m4.cpp:220-260), while `twoway::LayerSpec` carries a
    // single yvar/xvar pair. Re-routing a 3+-token varlist would silently drop
    // every series after the first.
    const bool layerReroutable = (gc == "scatter" || gc == "line" || gc == "connected");
    bool wantsTwowayGlobals = false;
    for (const auto& kv : parsed.opts) {
      if (kGraphTwowayGlobals.count(kv.first) != 0) {
        wantsTwowayGlobals = true;
        break;
      }
    }
    const bool rerouteToLayers = layerReroutable && vl.size() == 2 && wantsTwowayGlobals;
    // The keys the branch taken below actually READS -- the input to the
    // accepted-and-not-applied note. Every bare handler reads these five
    // (runScatter:271 / runGraphLine:849 / runXtline:950 / runTsline:1054 for the
    // four titles, this arm's own name() capture below for `name`).
    std::unordered_set<std::string> gApplied = {"title", "xtitle", "ytitle", "note", "name"};
    // serset-twoway SS: the plots this statement's sersets are built from.
    std::vector<serset::PlotSpec> barePlots;
    // serset-twoway WT: the statement's weight type as its full name.
    std::string bareWeightType;
    if (parsed.weightType.has_value() && !parsed.weightType->empty()) {
      const std::string wt = lower(*parsed.weightType);
      if (wt == "fw" || wt == "fweight") bareWeightType = "fweight";
      else if (wt == "aw" || wt == "aweight") bareWeightType = "aweight";
      else if (wt == "pw" || wt == "pweight") bareWeightType = "pweight";
      else if (wt == "iw" || wt == "iweight") bareWeightType = "iweight";
      else bareWeightType = wt;
    }
    // scatter/line/connected/tsline take aw/fw/pw (w2al tf_r_iw `iweights not
    // allowed`, r(101), log:5208-5217).
    if ((gc == "scatter" || gc == "line" || gc == "connected" || gc == "tsline") &&
        !bareWeightType.empty() && bareWeightType != "aweight" && bareWeightType != "fweight" &&
        bareWeightType != "pweight") {
      if (!tmpGraphHbin.empty()) cecon::native_unlink(tmpGraphHbin);
      blocks.push_back(ojson(eval::errorBlock("engine_error", bareWeightType + "s not allowed",
                                              std::nullopt, std::nullopt, std::nullopt, "r(101)")));
      return blocks;
    }
    // serset-twoway REF: a string variable in a scatter/line plot's varlist is
    // r(109) (w2al tf_r_sc_strx, log:5330-5339); it used to plot factor codes.
    if (gc == "scatter" || gc == "line" || gc == "connected" || gc == "tsline") {
      for (const auto& v : vl) {
        if (dsPtr != nullptr && dsPtr->has(v) && dsPtr->dtype(v) == "factor") {
          if (!tmpGraphHbin.empty()) cecon::native_unlink(tmpGraphHbin);
          blocks.push_back(ojson(eval::errorBlock(
              "engine_error", "string variables not allowed in varlist;\n" + v + " is a string variable",
              std::nullopt, std::nullopt, std::nullopt, "r(109)")));
          return blocks;
        }
      }
    }
    if (rerouteToLayers || kBareTwowayLayerRoute.count(gc) != 0 || twowayKdHist) {
      twoway::LayerSpec spec;
      spec.plottype = gc == "hist" ? std::string("histogram") : gc;
      spec.varlist = vl;
      spec.opts = parsed.opts;
      spec.weightType = bareWeightType;  // serset-twoway WT
      spec.weightExp = parsed.weightVar.value_or(std::string());
      // `dsPtr` is already the (optionally `if`-filtered) datastore from the
      // graphif temp above, so pass no global-if here (avoids a double filter).
      result = ojson(handlers::runTwowayLayers(dsPtr, session, {spec}, parsed.opts,
                                               std::nullopt, &barePlots));
      gApplied.insert(kGraphTwowayGlobals.begin(), kGraphTwowayGlobals.end());
      gApplied.insert(kGraphLayerStyle.begin(), kGraphLayerStyle.end());
      gApplied.insert(kGraphFitApplied.begin(), kGraphFitApplied.end());
      gApplied.insert("sort");  // BUG-404: the layers order by x only under it
      if (twowayKdHist) {
        gApplied.insert(kGraphHistExtra.begin(), kGraphHistExtra.end());
        gApplied.insert(kGraphKdensExtra.begin(), kGraphKdensExtra.end());
        gApplied.insert({"range", "area"});
      }
    } else if (gc == "histogram" || gc == "hist") {
      // gfx-histogram-kdensity-family: the `(bin=#, start=#, width=#)` note is
      // its own display block, printed before the graph; the graph block
      // stays an object.
      std::vector<ojson> pre;
      std::vector<serset::Serset> histSets;  // serset-twoway HIST-1
      result = ojson(handlers::runHistogram(session, *dsPtr, vl, parsed, &pre, &histSets));
      for (auto& b : pre) blocks.push_back(std::move(b));
      if (histOptErr.has_value() && !(result.is_object() && result.value("type", std::string()) == "error")) {
        if (!tmpGraphHbin.empty()) cecon::native_unlink(tmpGraphHbin);
        blocks.push_back(*histOptErr);
        return blocks;
      }
      for (auto& hs : histSets) {
        serset::PlotSpec ps;
        ps.computed = std::move(hs);
        barePlots.push_back(std::move(ps));
      }
      gApplied.insert(kGraphHistExtra.begin(), kGraphHistExtra.end());
      for (const char* carried : {"addplot", "addlabopts", "blcolor"}) gApplied.erase(carried);
      gApplied.insert("saving");
    } else if (gc == "line") {
      result = ojson(handlers::runGraphLine(*dsPtr, vl, parsed.opts));
      gApplied.insert("sort");  // BUG-404: data order unless sort
    } else if (gc == "kdensity") {
      // runKdensity posts r(), writes generate() (session.load replaces the
      // head: dsPtr is not read after this call) and, under nograph, draws
      // nothing -- then only its `pre` blocks are emitted.
      std::vector<ojson> pre;
      bool drawn = false;
      std::vector<serset::Serset> kdSets;  // serset-twoway HIST-1
      result = ojson(handlers::runKdensity(session, *dsPtr, vl, parsed, &pre, &drawn, &kdSets));
      for (auto& b : pre) blocks.push_back(std::move(b));
      for (auto& ks : kdSets) {
        serset::PlotSpec ps;
        ps.computed = std::move(ks);
        barePlots.push_back(std::move(ps));
      }
      if (!drawn && !(result.is_object() && result.value("type", std::string()) == "error")) {
        if (!tmpGraphHbin.empty()) cecon::native_unlink(tmpGraphHbin);
        return blocks;
      }
      gApplied.insert(kGraphKdensExtra.begin(), kGraphKdensExtra.end());
      gApplied.insert("saving");
    } else if (gc == "xtline") {
      result = ojson(handlers::runXtline(*dsPtr, vl, session.panelvar, session.timevar, parsed.opts));
      gApplied.insert(kGraphXtlineExtra.begin(), kGraphXtlineExtra.end());
    } else if (gc == "tsline") {
      result = ojson(handlers::runTsline(*dsPtr, vl, session.timevar, parsed.opts));
    } else if (cmd == "graph" && (gc == "bar" || gc == "hbar" || gc == "box" || gc == "hbox")) {
      // graph-family AI-1/AI-2/AI-3: the bar/box statistics and serset
      // (graph_bar.cpp). Its options are drawing options except the ones the
      // handler reads; they are accepted (the block notes them).
      handlers::BarGraphResult br = handlers::runGraphBarBox(session, *dsPtr, gc, vl, parsed);
      result = br.block;
      for (auto& s : br.sets) {
        serset::PlotSpec ps;
        ps.computed = std::move(s);
        barePlots.push_back(std::move(ps));
      }
      // (`overlay`/`percent` are the canonical spellings parsed.opts gives `over`
      // and `percentages`; the handler re-reads the typed options.)
      for (const char* k : {"over", "overlay", "percent", "asyvars", "ascategory", "cw", "missing",
                            "percentages", "stack",
                            "horizontal", "vertical", "noout", "nooutsides", "nofill", "saving",
                            "nodraw", "scheme", "xsize", "ysize"})
        gApplied.insert(k);
    } else if (kUnsupportedBareGraphTypes.count(gc) != 0) {
      result = ojson(eval::unsupportedCommand("graph " + gc, std::nullopt, std::nullopt, "piet_gap",
                                              eval::UnsupportedCommandKind::kNotImplemented));
    } else {
      result = ojson(handlers::runScatter(*dsPtr, vl, parsed.opts));
    }
    graphAcceptedNotApplied(result, gc, parsed.opts, gApplied);
    const bool bareIsGraph = result.is_object() && result.value("type", std::string()) == "graph";
    // serset-twoway SS: the bare data plots' sersets (piet/serset.h rule (a)/(d)):
    // scatter/line/connected `y1..yk x [mlabel]`, tsline `y1..yk <timevar>`, over
    // the plotted sample (dsPtr is the `if`-filtered frame) in data order.
    if (bareIsGraph && barePlots.empty() && dsPtr != nullptr &&
        (gc == "scatter" || gc == "line" || gc == "connected" || gc == "tsline")) {
      std::vector<std::string> svars = vl;
      if (gc == "tsline" && session.timevar.has_value()) svars.push_back(*session.timevar);
      const size_t roleCount = svars.size();
      // serset-twoway WT: the weight variable follows the plot's variables
      // (w2al tf_wt_line_s1 `y x w`, log:1223).
      if (!bareWeightType.empty() && parsed.weightVar.has_value()) {
        const std::string wv = trimWs(*parsed.weightVar);
        if (dsPtr->has(wv) && std::find(svars.begin(), svars.end(), wv) == svars.end()) svars.push_back(wv);
      }
      if (gc != "tsline") {
        std::optional<std::string> ml;
        for (const auto& [k, v] : parsed.opts) {
          if (k == "mlabel" && !v.isBool) ml = trimWs(v.strValue);
        }
        if (ml.has_value() && !ml->empty() && dsPtr->has(*ml) &&
            std::find(svars.begin(), svars.end(), *ml) == svars.end()) {
          svars.push_back(*ml);
        }
      }
      bool sortOpt = false;
      for (const auto& [k, v] : parsed.opts) {
        if (k == "sort" && (!v.isBool || v.boolValue)) sortOpt = true;
      }
      std::vector<int64_t> rows(static_cast<size_t>(dsPtr->nRows()));
      for (size_t i = 0; i < rows.size(); ++i) rows[i] = static_cast<int64_t>(i);
      serset::PlotSpec ps;
      for (const auto& v : svars) ps.cols.push_back(serset::variableColumn(session, *dsPtr, v, rows));
      ps.sorted = sortOpt && gc != "tsline";
      ps.sortCol = roleCount > 0 ? static_cast<int>(roleCount) - 1 : -1;
      barePlots.push_back(std::move(ps));
    }
    if (!tmpGraphHbin.empty()) cecon::native_unlink(tmpGraphHbin);

    // M4b: cache the last graph so `graph export`/`graph save` can persist
    // it (mirrors dispatcher.py's `if result.get("type")=="graph": session.
    // last_graph = result`).
    // BUG-1529: saving()'s `file ... saved` note follows the graph (StataBE 19.5,
    // a15.do: histogram prints its bin note, then the two file lines).
    ojson bareSavedNote;
    if (bareIsGraph) {
      // BS-010 (U3) + serset-twoway SS: every bare graph registers (an unnamed
      // one as "Graph") together with its sersets — the one registration
      // (piet/serset.h registerGraph).
      const ojson bareMeta = graphMetaFor(
          graphLineTyped, (gc == "bar" || gc == "hbar" || gc == "box" || gc == "hbox") ? "bar" : "twoway",
          parsed.opts);  // graph-family AI-4 (bar/hbar/box are family `bar`: w2ai gf_box_out_d)
      serset::registerGraph(session, result, bareNameOpt,
                            serset::buildPlotSersets(std::move(barePlots)), &bareMeta);
      // gfx-histogram-kdensity-family: histogram's and kdensity's saving()
      // writes the graph file (w2aq_histogram_family hk_h_069by_f,
      // hk_h_031disc_f). The last saving() wins, as Stata's syntax keeps it.
      // serset-twoway GST: every graph of this arm (scatter/line/tsline/twoway
      // forms: w2al tf_line_sav_f, tf_sc_003sav_f), with the graph's sersets.
      {
        std::optional<std::string> savingOpt;
        for (const auto& [k, v] : parsed.opts) {
          if (k == "saving" && !v.isBool) savingOpt = v.strValue;
        }
        if (savingOpt.has_value()) {
          const serset::GraphName gn = serset::parseNameOption(bareNameOpt);
          const std::vector<serset::Serset> owned =
              serset::ownedBy(session, gn.name.empty() ? std::string("Graph") : gn.name);
          ojson se = handlers::saveGraphOption(result, *savingOpt, session, &owned, &bareSavedNote);
          if (!se.is_null()) {
            blocks.push_back(se);
            return blocks;
          }
        }
      }
    }

    // L2-FIX-B (BUG-066): thin the LIVE-VIEW copy only. session.lastGraph /
    // namedGraphs were deep-copied full above, so `graph export`/`save` keep
    // every point (golden-pinned); this shrinks the console/bridge block.
    handlers::decimateGraphBlockForView(result);
    blocks.push_back(result);
    if (!bareSavedNote.is_null()) blocks.push_back(bareSavedNote);
    return blocks;
  }

  // Everything else: explicit piet_gap, never a silent wrong answer. This is
  // the arm the BUG-105 abbreviation gaps land on — the token simply never
  // resolved to a command table entry, which is what the message now says.
  blocks.push_back(ojson(eval::unsupportedCommand(
      parsed.cmd, std::nullopt, std::nullopt, "piet_gap",
      eval::UnsupportedCommandKind::kUnknownToken)));
  return blocks;
}

namespace {

// BUG-1497, BUG-1509, BUG-1512: what a command leaves in `_rc`, s() and r()
// besides its own results, one row per command. Stata's estimation and many
// r-class commands are ado files that run internal steps -- `capture` checks,
// sclass option parsers, r-class helpers -- so:
//  - `_rc` after a run without `capture` is what the ado's last internal capture
//    left: 0 after regress, logit, lincom, ...; 100 after every margins. A
//    built-in (summarize, tabulate, generate, ...) or an ado without such a
//    capture (reghdfe, test, predict, estat ic) leaves it;
//  - s() is emptied by the commands whose ado runs an sclass parser;
//  - r() is emptied by a failure that comes after such a step. For regress,
//    tabulate, lincom, test, testparm, ttest, ... every failure does (r(111),
//    r(198), r(2000), r(103), r(109), r(301), r(100) alike); logit, probit and
//    ivregress fail before it on a bad name or option and keep r(), and empty
//    it on an empty sample (r(2000)).
// Measured in StataBE 19.5 (scratch-laneA/agents/file-new-1/i4.do, i4b.do,
// i7.do, i8.do; scratch-laneA2/agents/rlife/side.do, side2.do, and side3.do,
// side4.do, tp.do over other forms and failures of the same commands): each
// command run `quietly` after a failed `capture confirm variable` (_rc 111) and
// an sclass program's `sreturn local foo`, each failure under `capture` after
// `summarize`.
// Not modelled: a failure that leaves an empty-sample count in r() (poisson, areg,
// xtreg fe on r(2000)/r(2001): r(N) 0), the internal leftovers some failures
// leave beside the emptied r() (reghdfe's r(fn), mean's r(is_svysum)), the
// data-dependent `_rc` of centile (7) and signrank (9, its `capture assert` of
// integer ranks), ttest's success, which depends on the form (rlife/side3.do:
// `ttest x1 == 3` _rc 0 keeping s(), `ttest x1 == x2` 0 emptying s(), `ttest x1,
// by(yb)` 198), and s() after a failure (side5.do: a failed logit or predict
// empties it, a failed regress, lincom or test keeps it). A command not listed
// leaves all three alone. A failed margins is its handler's (runMargins empties
// r() for the errors it shares with Stata).
constexpr int kAnyFailure = 0;  // clearsROn: every failure empties r()

struct AdoResidue {
  const char* cmd;             // canonical command; estat/estimates with the subcommand
  int rc;                      // `_rc` after a successful run; -1 leaves it
  bool clearsS;                // a successful run empties s()
  std::vector<int> clearsROn;  // the failure codes after which r() is empty
};

const AdoResidue* findAdoResidue(const std::string& cmd) {
  static const std::vector<AdoResidue> kTable = {
      {"regress", 0, true, {kAnyFailure}},
      {"logit", 0, true, {2000}},
      {"probit", 0, true, {2000}},
      {"poisson", 0, true, {}},
      {"nbreg", 0, true, {}},
      {"areg", 0, true, {111, 198}},
      {"reghdfe", -1, true, {kAnyFailure}},
      {"xtreg", 0, true, {198}},
      {"ivregress", 0, true, {2000}},
      {"ivreghdfe", -1, true, {}},
      {"tobit", 0, true, {}},
      {"qreg", 0, true, {}},
      {"glm", 0, true, {}},
      {"newey", 0, true, {}},
      {"prais", 0, true, {}},
      {"ologit", 0, true, {}},
      {"mlogit", 0, true, {}},
      {"reg3", 0, true, {111}},
      {"sureg", 0, true, {111}},
      {"dfuller", 0, true, {111}},
      {"pperron", 0, true, {111}},
      {"wntestq", -1, true, {111}},
      {"lincom", 0, true, {kAnyFailure}},
      {"nlcom", 0, true, {kAnyFailure}},
      {"test", -1, false, {kAnyFailure}},
      {"testparm", 0, true, {kAnyFailure}},
      {"margins", 100, true, {}},
      {"predict", -1, true, {}},
      {"estat", -1, false, {kAnyFailure}},
      {"estat hettest", 0, true, {}},
      {"estat ovtest", 0, true, {}},
      {"estat vif", 0, true, {}},
      {"estimates store", 0, false, {}},
      {"tabulate", -1, false, {kAnyFailure}},
      {"pwcorr", 0, true, {kAnyFailure}},
      {"ttest", -1, false, {kAnyFailure}},
      {"tabstat", 0, false, {kAnyFailure}},
      {"sktest", -1, true, {111}},
      {"centile", -1, false, {kAnyFailure}},
      {"signrank", -1, false, {111}},
      {"spearman", 0, false, {111}},
      {"mean", 0, true, {kAnyFailure}},
      {"codebook", 0, true, {kAnyFailure}},
      {"ds", -1, false, {kAnyFailure}},
      {"describe", -1, false, {111}},
      {"duplicates", -1, false, {111}},
      {"egen", 0, false, {111}},
      {"levelsof", 0, false, {}},
      {"xtset", 0, false, {}},
      {"tsset", 0, false, {}},
  };
  for (const auto& row : kTable)
    if (cmd == row.cmd) return &row;
  return nullptr;
}

// The s() store's reserved key prefix: byte-identical to program.cpp's and
// eval/macros.cpp's kSClassKeyPrefix (no shared header, see program.cpp).
const char* const kSStoreKeyPrefix = "\x01s:";

// Applied by dispatch() to a leaf, before its r() hook: a failure's emptied r()
// is then that command's (declared empty) r(). `capture` sets `_rc` afterwards.
void applyAdoResidue(SessionState& session, const ojson& result, const std::string& rawCmd) {
  const std::string& line = session.leafCmdExpanded.empty() ? rawCmd : session.leafCmdExpanded;
  std::istringstream words(line);
  std::string w1, w2;
  words >> w1 >> w2;
  auto word = [](const std::string& w) { return lower(w.substr(0, w.find(','))); };
  const std::string cmd = registry::resolveCanonical(word(w1));
  if (cmd.empty()) return;
  std::string sub = word(w2);
  if (cmd == "estimates" && sub.size() >= 3 && std::string("store").compare(0, sub.size(), sub) == 0)
    sub = "store";
  const ojson* err = nullptr;
  if (result.is_array())
    for (const auto& b : result)
      if (b.is_object() && b.value("type", std::string()) == "error" && !b.value("captured", false)) {
        err = &b;
        break;
      }
  if (err != nullptr) {
    const AdoResidue* row = findAdoResidue(cmd);
    if (row == nullptr) return;
    const std::string rc = eval::coarseRc(*err);
    for (int code : row->clearsROn)
      if (code == kAnyFailure || rc == std::to_string(code)) {
        eval::replaceRClass(session.eval);
        eval::declareRPosted(session.eval, cmd, {});
        return;
      }
    return;
  }
  const AdoResidue* row = sub.empty() ? nullptr : findAdoResidue(cmd + " " + sub);
  if (row == nullptr) row = findAdoResidue(cmd);
  if (row == nullptr) return;
  if (row->rc >= 0) session.eval.locals.set("_rc", std::to_string(row->rc));
  if (row->clearsS) {
    std::vector<std::string> sKeys;
    for (const auto& kv : session.eval.rStrings.items())
      if (kv.first.rfind(kSStoreKeyPrefix, 0) == 0) sKeys.push_back(kv.first);
    for (const auto& k : sKeys) session.eval.rStrings.erase(k);
  }
}

}  // namespace

// BS-006 (BUG-028): log-tee wrapper around dispatchImpl() above.
//
// dispatchImpl() is heavily self-recursive: capture/quietly/noisily/by/
// bysort prefix-stripping, the eststo/version bare-command forwards, and
// nested do-file/loop/program-body replay (via the `dispatch` function
// pointer handed to doexec::executeDoSource/executeDo and
// program::runProgram from inside dispatchImpl()'s own body) all call BACK
// into `dispatch` from partway through an already-in-flight call. Only the
// TRUE outermost call's finalized return value is "what Stata actually
// showed" for that top-level command — every recursive sub-call's result
// gets folded into (and often filtered/relabeled by — e.g. `quietly`
// dropping non-echo blocks, `capture` swallowing an error down to `_rc`)
// its caller's own return value before THAT reaches its own caller, and so
// on up to the outermost frame. Teeing at every recursion level would
// therefore write the same output to the log multiple times — once raw
// from an inner call, again as part of each enclosing wrapper's own
// combined result.
//
// session.enterDispatch()/exitDispatch() (session_state.h) is a plain
// per-session recursion-depth counter: the FIRST (outermost) call sees
// depth go 0->1; every nested call sees 1->2, 2->3, etc. Only the frame
// that saw the 0->1 transition ever calls session.logTee() — with the
// COMPLETE finalized blocks array (echo + result, already including every
// nested line's own blocks for a `do`/loop/program replay, since those are
// flattened into the outer return value by dispatchImpl()'s existing
// recursive-call sites, not by anything in this wrapper). logTee() itself
// is a silent no-op whenever no log is open (SessionState::logIsOpen()),
// so a session that never calls `log using` — i.e. every pre-existing
// fixture — sees zero behavior change and negligible added cost from this
// wrapper (freeze safety).
//
// The try/catch exists purely so a stray exception from deep inside
// dispatchImpl() still decrements the depth counter and still propagates
// exactly as before this wrapper existed (rethrown unchanged, never
// swallowed) — it does not change dispatchImpl()'s own exception behavior.
// logTee() is likewise wrapped so a logging failure can never affect the
// command's own result; std::ofstream doesn't throw by default (no
// exception mask is ever set here), so this is defense-in-depth, not a
// path this build expects to exercise.
// Counts every dispatch() entry on this thread. A frame whose count did not move
// while it ran is a LEAF command: no prefix (`quietly`, `capture`, `eststo:`), no
// do-file and no loop ran another command underneath it. r(table) is posted only at
// leaves (handlers::rTableAfterCommand), so a container never re-posts a table a
// nested r-class command already replaced; a container that replaced e() itself
// only retires a table that no longer belongs to the live e(). thread_local because
// parallel blocks dispatch on their own threads with their own sessions.
thread_local unsigned long long g_dispatchSerial = 0;

nlohmann::ordered_json dispatch(const std::string& rawCmd, SessionState& session) {
  int depth;
  const unsigned long long serialAtEntry = ++g_dispatchSerial;
  const std::uintptr_t eIdAtEntry = handlers::liveEstimationIdentity(session);
  const eval::RStoreMark rMarkAtEntry = session.eval.rMark();
  // An r-class arm's declaration (eval::declareRPosted) belongs to the command
  // that made it: a leaf that threw past the hook must not hand it on (RULING.md
  // D.3.1).
  session.eval.rDeclaredNames.reset();
  session.eval.rDeclaredCmd.clear();
  session.leafCmdExpanded.clear();
  {
    dprof::Scope __z(dprof::kWrapper);
    depth = session.enterDispatch();
  }
  // AER scope-2 B2-3. bofd()/dofb() resolve `<name>.stbcal` against the
  // session's working directory, but the surfaces that call them do not all
  // carry a session: `display bofd(...)` and `local x = bofd(...)` evaluate in
  // eval::evalArith's PROCESS-WIDE immutable namespace (macros.cpp:1554). One
  // assignment at the outermost dispatch is the whole of that plumbing.
  // It is a RESOLUTION directory only. It used to be part of the cache KEY as
  // well, which made a `cd` silently drop an already-loaded calendar -- Stata's
  // cache is name-keyed and survives the cd (review 2026-09-09; see bcal.h for
  // the measurement table). Two sessions in one process are kept apart by
  // SessionState::resetProcessCaches(), not by this string.
  if (depth == 0) {
    // bcal::searchDirFor, NOT a second copy of the rule: an unset session.cwd
    // must mean the same directory here as it does in the handler's bcalCwd,
    // or install() after `bcal create` keys the cache on one string while
    // lookup() reads it under another (review finding, non-blocking 3).
    bcal::setSearchDir(bcal::searchDirFor(session.cwd.value_or(std::string())));
  }
  // X2-GLM: glm's e() entries outside its estimation block must not outlive
  // the glm (see handlers.h GlmEStores). Snapshot only while a glm is current.
  const std::optional<handlers::GlmEStores> glmBefore = handlers::glmOwnedEStores(session);
  nlohmann::ordered_json result;
  try {
    result = dispatchImpl(rawCmd, session);
    if (glmBefore.has_value()) handlers::dropStaleGlmEStores(session, *glmBefore);
    // name-binding-display-literals: an inline `=exp' that failed as Stata's
    // evaluator fails expanded to nothing at rc 0; its message prints ahead of
    // the line's own output, as StataBE prints `foo not found` before the line
    // (w2av_name_binding bd_dy_ilunb). Repeats of one message (a line expanded
    // twice on its way in) print once. BUG-1338: the expansion runs before any
    // prefix does, so `quietly` keeps these blocks (`inline_note`) wherever they
    // come from, and `capture` keeps those of its own line: StataBE prints
    // `variable nosuch not found` for `capture display "C`: type nosuch'"` and
    // `quietly local q "Q`: type nosuch'"` too.
    std::vector<std::string> notes = eval::takeInlineExpansionNotes();
    if (!notes.empty() && result.is_array()) {
      size_t at = 0;
      while (at < result.size() && result[at].is_object() &&
             result[at].value("type", std::string()) == "echo")
        ++at;
      std::string prev;
      for (const auto& n : notes) {
        if (n == prev) continue;
        prev = n;
        nlohmann::ordered_json nb;
        nb["type"] = "display";
        nb["text"] = n;
        nb["inline_note"] = true;
        nb["inline_note_of"] = rawCmd;  // the line that expanded (capture's filter)
        result.insert(result.begin() + static_cast<std::ptrdiff_t>(at++), std::move(nb));
      }
    }
  } catch (const std::exception& e) {
    // A C++ exception from a command is that command's error, not the run's.
    // Rethrown, it escaped every `capture` above it (capture reads error BLOCKS)
    // and runDoFileHeadless discarded the whole run's output with it: one
    // `import delimited ..., delimiter(tab)` that threw "bad magic" left
    // w2ar_data_io_family with 0 of 1912 arms, every earlier PREC line lost.
    // As a block it is capturable (_rc via coarseRc), and an uncaptured one
    // stops the do-file at its own line like any other error.
    (void)eval::takeInlineExpansionNotes();
    std::string msg = e.what();
    if (const auto* ce = dynamic_cast<const CobraError*>(&e)) msg = cobraErrorMessage(*ce);
    result = nlohmann::ordered_json::array();
    result.push_back(nlohmann::ordered_json(eval::engineError(msg)));
  } catch (...) {
    // BUG-360 follow-up: drain the section-38 projection ledger on the throwing
    // path too. A command that failed has still written its projections.
    if (session.exitDispatch() == 0) {
      session.reapProjectionScratch();
      session.releasePreInstallPins();  // e(sample) T2: the command is over
      session.sweepOrphanScratch("dispatch-exit-throw");  // BUG-389
      session.auditHeadBacked("dispatch-exit-throw");  // BUG-384 invariant
    }
    throw;
  }
  if (g_dispatchSerial == serialAtEntry) applyAdoResidue(session, result, rawCmd);
  // w2c_postest family 1: r(table) after estimation — the one posting site (see
  // handlers_aer_estim.cpp). Before exitDispatch so its column reads run inside
  // this command's own scratch accounting.
  handlers::rTableAfterCommand(session, result, rawCmd, eIdAtEntry,
                               g_dispatchSerial == serialAtEntry, rMarkAtEntry);
  dprof::Scope __zw(dprof::kWrapper);
  if (session.exitDispatch() == 0) {
    session.reapProjectionScratch();
    session.releasePreInstallPins();  // e(sample) T2: the command is over
    // BUG-389 (2026-09-02): after the ledgers have had their say, every file left
    // in the workdir that NO ledger owns is dead by definition — the by:/if
    // projection slices, multi-step verb intermediates, fixed-name outputs, the
    // parallel block's protocol files. Measured 173–188 MB per command on a 60 MB
    // frame before this; the acceptance corpus never stepped on the shape.
    session.sweepOrphanScratch("dispatch-exit");
    session.auditHeadBacked("dispatch-exit");  // BUG-384 invariant
  }
  // W:P8: every frame now offers its blocks to logTee, which writes only to
  // the logs whose own depth matches. The unnamed log is still recorded at
  // depth 1 and therefore still teed exactly once per top-level command, so
  // sessions without a `name()`d log are byte-unchanged; a named log opened
  // inside a do-file is recorded at that file's depth and so receives its
  // lines as they run, instead of nothing at all (the outermost tee fires
  // after the file has finished, by which point the named log is closed).
  try {
    // BUG-189: a prefixed line's log view is not its block array. Measured
    // against StataBE 19.5: `quietly display "E"` logs the line as written and
    // nothing it produced (our array legitimately also carries the INNER echo,
    // which the console transcript wants and the log must not have), while
    // `noisily display "F"` inside a quiet scope logs its OUTPUT but still no
    // echo. So the quiet case is teed as a synthesized view rather than
    // filtered, and the noisy case is teed under a visible scope.
    switch (session.consumeLineVisibility()) {
      case SessionState::LineVis::Quiet: {
        nlohmann::ordered_json logView = nlohmann::ordered_json::array();
        logView.push_back(echoBlock(rawCmd));
        for (const auto& b : result) {
          if (b.is_object() && b.value("type", std::string()) == "error") logView.push_back(b);
        }
        session.logTee(logView, depth);
        break;
      }
      case SessionState::LineVis::Noisy: {
        SessionState::LogVerbosityScope __lv(session, /*isQuiet=*/false);
        session.logTee(result, depth);
        break;
      }
      case SessionState::LineVis::None:
      default:
        session.logTee(result, depth);
        break;
    }
  } catch (...) {
    // Logging is a side-effect only; never let it affect the command's
    // own result.
  }
  return result;
}

}  // namespace piet
