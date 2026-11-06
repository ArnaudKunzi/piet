// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Portions derived from estout 3.33 (MIT).
// See THIRD_PARTY_NOTICES.
//
// M4a item 3: stored-estimates surface — eststo/esttab/coefplot. C++ port
// of piet/backend/piet/eststo.py + esttab.py (make_snapshot/render_esttab/
// parse_esttab_opts) + coefplot.py (run_coefplot), and dispatcher.py's
// "eststo"/"eststo_dir"/"eststo_manage"/"esttab"/"coefplot" native-handler
// arms. See piet/native/include/piet/handlers.h for the per-handler
// mapping.
#include "piet/handlers.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <deque>
#include <memory>
#include <cstdio>
#include <fstream>
#include <regex>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <unordered_set>

#include "core/ident.h"       // the shared Unicode name rule
#include "piet/datastore.h"   // kEsampleColumn
#include "piet/dispatcher.h"  // `estimates esample:` marks its rows through `generate`
#include "piet/eval/errors.h"
#include "piet/eval/macros.h"  // eScalarKey(): the ONE e()-name table
#include "estimation_m4_internal.h"  // e(sample) T2's mask writer (esampleRewriteHeadMask)
#include "io/win_compat.h"     // cecon::native_exists / to_native_path

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
const piet::eval::OptVocab& kVocabEsttab = piet::eval::registerOptVocab(
    {"esttab"},
    // AER-SCOPE1 bundle E (TABLES, 2026-09-04): the pre-bundle set was the five
    // options this handler READS. That was honest while `esttab` could not write
    // a file, but the corpus spells `esttab m1 using t.tex, replace booktabs
    // label` and every one of those four answered "option not allowed". The
    // known set is now the scope table's, and the ones PIET does not honour are
    // listed as UNIMPLEMENTED so they say so rather than vanishing -- the
    // distinction matters because `rejectUnknownOptions` accepts an abbreviated
    // prefix of a known name, so a name missing from BOTH lists is the silent
    // wrong answer bundle A shipped (`ivreg ..., ro` ran without robust SEs).
    {{"nostar", "b", "se", "t", "star",
      "replace", "append", "booktabs", "label", "title", "mtitles", "nonumbers",
      "nomtitles", "stats", "scalars", "keep", "drop", "order", "r2", "ar2",
      "noobs", "nonotes", "addnotes", "fragment", "coeflabels", "csv",
      "compress", "nogaps", "plain",
      // B4-W5 (2026-09-09). Each of these is CONTENT -- it changes what a cell
      // comparator reads out of the written table -- and each is pinned to one
      // of the 16 captured arms in tests/eval/golden/aer_b4/tables/. They are
      // honoured by `writeEsttabFile` (handlers_aer_tables.cpp), which is the
      // `using` half of this command; see the scope note at the head of that
      // function for what the SCREEN block still does with them.
      "beta", "not", "ci", "p", "level", "starlevels", "substitute", "rename",
      "mlabels", "collabels", "nodepvars", "mgroups", "refcat", "tex",
      "prehead", "posthead", "prefoot", "postfoot",
      // LAYOUT-ONLY, accepted and NOT applied. They are here rather than in
      // `unimplemented` because refusing a corpus line over a column width
      // stops a paper that would otherwise reproduce; the writer says so once,
      // in the block it returns, so the acceptance is never silent. `nolegend`
      // is the one to watch: t_noleg.txt shows it also drops the STAR LEGEND,
      // i.e. it is content, not layout -- named as a W5 follow-up rather than
      // half-implemented here.
      "nolegend", "nolines", "page", "align", "width", "wrap", "modelwidth",
      "varwidth", "extracols",
      // W1a-3 (2026-09-17): the DISPLAY layer, every row measured against a
      // plain control in tests/eval/golden/aer_w1/w1b_esttab_display.log --
      // which is the point, because four of these turned out to change NOTHING
      // and a name is a bad reason to implement one.
      //   nocons/noconstant  drop the `_cons` rows. The two spellings are
      //                      byte-identical to each other (x6 vs the control).
      //   noomit/noomitted   drop a collinear coefficient's row (x13 vs x14).
      //   nobaselevels       a validated NO-OP here, and not because it was
      //                      too hard: this engine's coeftable does not carry
      //                      a factor's base level at all (`i.grp` gives
      //                      2.grp/3.grp and no 1.grp), so the `0.foreign 0 /
      //                      (.)` row Stata's default prints and this option
      //                      removes never exists to be removed. The absence
      //                      is the divergence and it is BUG-492's, not this
      //                      option's.
      //   type · obslast     MEASURED no-ops: byte-identical to the control,
      //                      `obslast` even with `s(N r2)` -- esttab's default
      //                      already puts N last. Implementing either on the
      //                      strength of its NAME would have been a divergence.
      //   style(...)         `fixed` is the default (byte-identical, x4);
      //                      `tex` reaches the same writer `tex`/`booktabs`
      //                      already do. Any other style is refused below.
      //   noabbrev           turns OFF the stub abbreviation (BUG-490).
      //   varlabels()        estout's spelling of `coeflabels()` -- same
      //                      name/label pairs, same map.
      "nocons", "noconstant", "noomit", "noomitted", "nobaselevels", "type",
      "obslast", "noabbrev", "style", "varlabels",
      // W1a-2 (2026-09-17, BUG-407): `cells` leaves the unimplemented list.
      // esttab has no cell renderer of its own -- esttab builds a
      // cells() spec out of main/aux when cells() is absent and hands it to
      // estout -- so `main`/`aux` join it as the same option wearing two other
      // names, and `writeEsttabFile` now reads all three through the one
      // grammar that already parses this syntax (estout's parseCells).
      // `wide` stays UNIMPLEMENTED on purpose: it is the quoted side-by-side
      // form (esttab), which this writer's coefficient-row-over-aux-row
      // model cannot render, and accepting it would be the exact
      // accept-and-ignore BUG-407 was filed against.
      "cells", "main", "aux",
      // W1a-5b (2026-09-18). `s` is estout's one-character minimum for
      // `Stats()` (estout `Stats(string asis)`), measured rc 0 in
      // w1b_stats_grammar.log s7 and written by 18 of the 32 obslast uses and
      // 13 of the 82 sfmt uses. It is added as an EXACT name: `resolveOption
      // AbbrevAcross` floors abbreviation at two characters, so a one-letter
      // key resolves only by exact membership and cannot start standing for
      // `se`/`star`/`style`.
      // `nolabel` cancels the label TEXT and not the WIDTH -- s12 keeps the
      // 20-column stub `label` set while printing `mpg`/`price` again.
      // `sfmt()` formats the `scalars()` rows, positionally, last-repeating --
      // and NOTHING else: twelve arms across three captures show it inert
      // against stats() keys, N, r2, the r2/ar2 flags, `plain` and `b()/se()`.
      // The identifying pair is w1b_sfmt_rule.log q5 vs q4.
      // W1a-5c. `gaps` is the DEFAULT and therefore a validated no-op (y5);
      // `alignment()` is the long spelling of `align()`, and both now reach the
      // tex column letter instead of being accepted and dropped.
      // W1a-5d: `pr2`/`pr2(#)` is the pseudo-R2 footer row. The engine
      // publishes that number under `r2_p` after poisson and `r2_pseudo` after
      // logit/probit, so the reader tries both (statCell).
      // W1a-5e: `labcol2(values, title(T))` -- one extra column between the
      // stub and the models, 456 corpus uses across 14 spellings.
      "s", "nolabel", "sfmt", "gaps", "alignment", "pr2", "labcol2",
      // `transform()` -- 6 corpus uses here and 12 on estout, and ONE
      // implementation shared with it (piet/handlers.h). `eform` stays
      // unimplemented beside it: it is `transform(exp(@) exp(@))` PLUS its own
      // removal of `_cons` (w1b_transform.log u2 has no intercept row where u1
      // does), so accepting it as an alias would drop that half in silence.
      "transform",
      // W1a-4. `indicate()` replaces the coefficients it names with one Yes/No
      // row (~47 corpus uses over 5 papers). `eqlabels()` and `unstack` are
      // MEASURED no-ops: on a single-equation table -- which is every table this
      // engine can produce, its coeftable carrying no equation at all -- all of
      // `eqlabels(none)`, `eqlabels("",none)`, `eqlabels("Estab." "HQ")` and
      // `unstack` are byte-identical to the control (w1b_indicate.log e1/e2/e3
      // and u1). Accepted as no-ops with the selftest pinning the SAMENESS, the
      // way W1a-3 handled `type`/`obslast`/`style(fixed)`; if the engine ever
      // carries equations these two have to be revisited, and that limit is the
      // reason it is written here.
      // `transpose` stays OUT: esttab answers r(198) "option transpose not
      // allowed" for it in Stata too (w1b_indicate.log tr_pose), so the six
      // corpus lines that spell it do not run there either and this build
      // refusing them is PARITY -- the `table, contents()` shape again.
      "indicate", "eqlabels", "unstack",
      // W2AK (2026-10-21): esttab is now derived from esttab.ado's
      // translation onto ONE estout writer (handlers_aer_tables.cpp eoRender),
      // so the rest of its option list is honoured as the ado reads it --
      // modes (esttab), the switches of _more_syntax and the
      // estout pass-throughs of _estout_options.
      "tab", "scsv", "fixed", "z", "abs", "staraux", "constant", "aic", "bic", "wide",
      "brackets", "parentheses", "noparentheses", "depvars", "notes", "numbers", "lines",
      "noeqlines", "eqlines", "float", "nofloat", "note", "begin", "delimiter", "end",
      "incelldelimiter", "abbrev", "legend", "stardetach", "interaction", "hlinechar",
      "nolz", "lz", "noisily", "outfilenoteoff", "nooutfilenoteoff", "notype", "omitted",
      "baselevels", "longtable", "standalone", "fonttbl", "attributes", "onecell"},
     // UNIMPLEMENTED, and `cells`/`p` are here deliberately. This handler reads
     // nostar/b/se/t/star and nothing else from the cell layer, so listing
     // either as KNOWN would accept it and silently ignore it -- the class
     // bundle A shipped as `ivreg ..., ro` (accepted, ran without robust SEs,
     // said nothing). `estout` DOES implement cells(); esttab does not, and the
     // difference is per command, not per option name. Tightening this is a
     // named follow-up, not a silent one: BUG-407.
     // `p` moved OUT of this set in W5: `esttab, p` now selects the p-value as
     // the parenthesised statistic in the written table, the same way `se`/`t`
     // already did. `cells` stays -- runEsttab still does not read a cells()
     // spec, and accepting one would silently render the default layout under a
     // different request (BUG-407).
     {"rtf", "html", "html5", "md", "mmd", "smcl", "equations", "eform", "margin",
      "discrete", "dropped", "lastend", "dmarker", "msign", "stardrop", "starkeep",
      "topfile", "bottomfile"}});
}  // namespace

namespace piet::handlers {

using ojson = nlohmann::ordered_json;

namespace {

ojson engineErrorBlockEt(const std::string& message, const std::string& code = "engine_error") {
  return eval::engineError(message, code);
}

// W2 esttabnames (BUG-552): a failed namelist expansion, under Stata's exact
// rc (111 no match, 301 no active `.`, 198 count). The
// `reason` field is the channel that carries an exact rc (BUG-187).
ojson estNamelistError(const SessionState::EstNamelist& ex) {
  return ojson(eval::errorBlock("engine_error", ex.message, std::nullopt, std::nullopt,
                                std::nullopt, "r(" + std::to_string(ex.rc) + ")"));
}

// esttab.py's make_snapshot: extract the essential fields from an
// EstimationResult block. Renames "(intercept)" -> "_cons".
ojson makeSnapshot(const ojson& estBlock) {
  ojson r2 = estBlock.contains("r2_within") ? estBlock["r2_within"] : ojson(nullptr);
  if (r2.is_null() && estBlock.contains("r2")) r2 = estBlock["r2"];

  ojson coeftableClean = ojson::array();
  if (estBlock.contains("coeftable") && estBlock["coeftable"].is_array()) {
    for (const auto& row : estBlock["coeftable"]) {
      ojson r = row;
      if (r.contains("name") && r["name"].is_string() && r["name"].get<std::string>() == "(intercept)") {
        r["name"] = "_cons";
      }
      coeftableClean.push_back(r);
    }
  }

  ojson snap;
  snap["coeftable"] = coeftableClean;
  snap["n_obs"] = estBlock.contains("n_obs") ? estBlock["n_obs"] : ojson(0);
  snap["r2"] = r2;
  snap["depvar"] = estBlock.value("depvar", std::string());
  snap["estimator"] = estBlock.value("estimator", std::string());
  snap["fe"] = estBlock.contains("absorbed") ? estBlock["absorbed"] : ojson(nullptr);
  // W2AK: a multi-outcome fit (mlogit) carries its coefficients per outcome and
  // no top-level coeftable; esttab/estout render one equation per outcome
  // (estout keys rows by equation), so the outcomes travel with the snapshot.
  if (estBlock.contains("outcomes") && estBlock["outcomes"].is_array()) {
    snap["outcomes"] = estBlock["outcomes"];
  }
  return snap;
}

// AER-SCOPE1 bundle E (TABLES): freeze e()'s SCALARS into the snapshot at
// `eststo` time. `esttab ..., stats(N r2_a)` reads these; without them the
// footer could only ever print the two hard-coded rows below, so every
// `stats()` a paper actually writes would render blank.
//
// The name->key map is `eval::eScalarKey()`, the same one `display e(r2_a)`
// resolves through -- deliberately NOT a copy. A key ABSENT from the estimate
// is omitted rather than stored as null, so "the engine does not emit this yet"
// and "this model has no such statistic" stay the same, honest, blank cell.
ojson escalarsFrom(const ojson& estBlock) {
  ojson out = ojson::object();
  for (const auto& [stataName, cobraKey] : eval::eScalarKey()) {
    if (!estBlock.contains(cobraKey)) continue;
    const ojson& v = estBlock[cobraKey];
    // n_clusters 0 is an unclustered fit, which has no e(N_clust) (BUG-1466)
    if (stataName == "N_clust" && v.is_number() && v.get<double>() == 0.0) continue;
    if (v.is_number()) out[stataName] = v;
  }
  return out;
}

ojson emptyAddedBag() {
  return ojson{{"scalars", ojson::object()},
               {"locals", ojson::object()},
               {"matrices", ojson::object()}};
}

// ---------------------------------------------------------------------------
// B4-W3b / BUG-470: LastModel <-> JSON, the two halves of `estimates save` and
// `estimates use`.
//
// This pair lives here, in ONE translation unit, on purpose: a writer and a
// reader that drift apart is the [[format-writer-unread-field-trap]] class, and
// the cheapest structural defence is that the only way to add a field is to
// edit both functions inside one screen of each other.
//
// What travels and what does not:
//  * `estHandle` is DELIBERATELY not written. It is contract_A §31's opaque
//    per-PROCESS session-estimation handle ("est:<nonce>:<n>",
//    session_state.h:168-186); the engine's nonce makes a handle from an
//    earlier process resolve nowhere, and restoring one across a save/use would
//    be the single way this could spend model A's handle on model B. Restoring
//    it EMPTY costs one engine refit and cannot be wrong.
//  * `dataPath`/`fitDataPath` are written as they stand. If the file they name
//    is gone by the time the estimates are `use`d, the post-estimation command
//    fails where it always fails — at the engine call, by name — rather than
//    here, silently, against a different dataset.
ojson lastModelToJson(const LastModel& lm) {
  ojson m;
  m["cobra_cmd"] = lm.cobraCmd;
  m["stata_cmd"] = lm.stataCmd;
  m["y"] = lm.y;
  m["x"] = lm.x;
  m["fe"] = lm.fe;
  m["vcov"] = lm.vcov;
  m["cluster"] = lm.cluster;
  if (lm.ivar.has_value()) m["ivar"] = *lm.ivar;
  m["depvar"] = lm.depvar;
  m["data_path"] = lm.dataPath;
  m["fit_data_path"] = lm.fitDataPath;
  m["fit_keepif"] = lm.fitKeepif;
  m["fit_weight"] = lm.fitWeight;
  m["fit_wtype"] = lm.fitWtype;
  // e(wexp) as typed (`= wf `, `= 1/wf`): without it `estimates use` rebuilt it
  // from the weight column (BUG-1215). Written only for a weighted fit.
  if (!lm.fitWexp.empty()) m["fit_wexp"] = lm.fitWexp;
  if (lm.timevar.has_value()) m["timevar"] = *lm.timevar;
  if (lm.gvar.has_value()) m["gvar"] = *lm.gvar;
  if (lm.method.has_value()) m["method"] = *lm.method;
  if (lm.comparison.has_value()) m["comparison"] = *lm.comparison;
  if (lm.pscoretrim.has_value()) m["pscoretrim"] = *lm.pscoretrim;
  if (lm.result.has_value()) m["result"] = *lm.result;
  m["iv_endog"] = lm.ivEndog;
  m["iv_exog"] = lm.ivExog;
  m["iv_instruments"] = lm.ivInstruments;
  return m;
}

std::vector<std::string> jsonStrVec(const ojson& j, const char* key) {
  std::vector<std::string> out;
  if (!j.contains(key) || !j[key].is_array()) return out;
  for (const auto& e : j[key]) {
    if (e.is_string()) out.push_back(e.get<std::string>());
  }
  return out;
}

std::optional<std::string> jsonOptStr(const ojson& j, const char* key) {
  if (!j.contains(key) || !j[key].is_string()) return std::nullopt;
  return j[key].get<std::string>();
}

LastModel lastModelFromJson(const ojson& m) {
  LastModel lm;
  lm.cobraCmd = m.value("cobra_cmd", std::string());
  lm.stataCmd = m.value("stata_cmd", std::string());
  lm.y = m.value("y", std::string());
  lm.x = jsonStrVec(m, "x");
  lm.fe = jsonStrVec(m, "fe");
  lm.vcov = m.value("vcov", std::string("iid"));
  lm.cluster = jsonStrVec(m, "cluster");
  lm.ivar = jsonOptStr(m, "ivar");
  lm.depvar = m.value("depvar", std::string());
  lm.dataPath = m.value("data_path", std::string());
  lm.fitDataPath = m.value("fit_data_path", std::string());
  lm.fitKeepif = m.value("fit_keepif", std::string());
  lm.fitWeight = m.value("fit_weight", std::string());
  lm.fitWtype = m.value("fit_wtype", std::string());
  lm.fitWexp = m.value("fit_wexp", std::string());
  lm.timevar = jsonOptStr(m, "timevar");
  lm.gvar = jsonOptStr(m, "gvar");
  lm.method = jsonOptStr(m, "method");
  lm.comparison = jsonOptStr(m, "comparison");
  lm.pscoretrim = jsonOptStr(m, "pscoretrim");
  if (m.contains("result") && !m["result"].is_null()) lm.result = m["result"];
  lm.ivEndog = jsonStrVec(m, "iv_endog");
  lm.ivExog = jsonStrVec(m, "iv_exog");
  lm.ivInstruments = jsonStrVec(m, "iv_instruments");
  // estHandle stays empty — see lastModelToJson's comment.
  return lm;
}

bool fileExistsEt(const std::string& path) { return cecon::native_exists(path); }

// Stata's `estimates save` appends `.ster` when the name carries no suffix
// (oracle pins_oracle_g.log:284 — `estimates save "b4g_est", replace`
// answers "(note: file b4g_est.ster not found) / file b4g_est.ster saved").
// Only a suffix on the LAST path component counts, so `dir.v2/est` still gets
// one.
std::string withSterSuffix(const std::string& path) {
  size_t slash = path.find_last_of('/');
  std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
  if (base.find('.') != std::string::npos) return path;
  return path + ".ster";
}

// ---------------------------------------------------------------------------
// e(sample) T3 (internal note esample/SPEC.md §6, corrected toward
// the StataBE 19.5 records of tests/eval/golden/aer_w2/w2_esample.log section G
// and toward Stata's own estimates.ado, which the records follow):
//  * `estimates store NAME` (and `eststo NAME`) drops any `_est_NAME`, then
//    writes e(sample) into a new byte variable `_est_NAME` labelled
//    "esample() from estimates store" at the end of the data (estimates.ado
//    Store: `capture drop _est_NAME`, `_est hold NAME, ... varname(_est_NAME)`;
//    G0b `0|28|`, G0c, G0d r(k) 19, G0e `... zb27 _est_A|`, G9 `0|0|28|`).
//  * `estimates restore NAME` of results that are NOT the active stored ones
//    reads e(sample) back from `_est_NAME` -- nonzero and nonmissing is in --
//    and leaves `_est_NAME` as the LAST variable (est_unhold drops it,
//    est_hold re-creates it; G2 28, G12 `28|31|`, I1). When NAME already IS the
//    active results Restore does nothing at all (`if ! inlist("NAME", ".",
//    e(_estimates_name))`), so the live mask keeps following the rows: G3
//    after `sort` 28, G4 after `drop` 22, G4b after `expand` 28 (the copies
//    are outside the sample, as for any fit, w2_esample.log:1076 E6).
//  * `estimates drop`/`clear` and `eststo clear` drop `_est_NAME` (G8b, G9b
//    r(111)) and leave the active e(sample) alone (G8 28, G10 `0|28|28|`).
// The DATA side is a visible user variable, so it follows every row verb the
// way Stata's does. Layers: none of these touch an expression evaluator; the
// mask they write is read through the row rewrite (dispatcher.cpp
// rewriteEsampleToken) and the macro layer's obs-1 hook, unchanged.
std::string estVarName(const std::string& name) { return "_est_" + name; }

bool dataHasVar(SessionState& session, const std::string& var) {
  if (!session.hasData()) return false;
  try {
    Datastore* ds = session.datastore();
    return ds != nullptr && ds->has(var);
  } catch (const std::exception&) {
    return false;
  }
}

// Writes `_est_NAME` = e(sample) (byte, 1/0). True when it was written. The
// old variable goes first in every case, as in Stata's estimates store: a stale
// `_est_NAME` from an earlier store of the same name would otherwise answer for
// these results. No block: Stata's store prints nothing about the variable.
bool esampleStoreVar(const std::string& name, SessionState& session) {
  if (!session.hasData()) return false;
  const std::string var = estVarName(name);
  if (dataHasVar(session, var)) {
    const ojson d = runDrop({var}, session);
    if (d.is_object() && d.value("type", std::string()) == "error") return false;
  }
  const SessionState::EsampleReading r = session.esampleReading();
  std::string expr;
  switch (r.kind) {
    case SessionState::EsampleReading::Kind::kColumn:
      expr = std::string("(") + kEsampleColumn + "==" + std::to_string(r.serial) + ")";
      break;
    case SessionState::EsampleReading::Kind::kZero:
      expr = "0";
      break;
    case SessionState::EsampleReading::Kind::kRefused:
      return false;  // PIET does not have these results' sample: no variable
  }
  const ojson g = runGen(var + " = " + expr, std::nullopt, session, "byte");
  if (!g.is_object() || g.value("type", std::string()) == "error") return false;
  // describe: `_est_A  byte  %8.0g  esample() from estimates store`
  // (w2_esample.log:1375 G0c).
  session.varMeta[var].stataType = "byte";
  session.varMeta[var].format = "%8.0g";
  session.setVarLabel(var, "esample() from estimates store");
  return true;
}

// The `_est_NAME` variables PIET wrote for these stored names, dropped
// together (estimates.ado Drop: `_est drop NAME` takes the held variable).
void esampleDropStoreVars(const std::vector<std::string>& names, SessionState& session) {
  std::vector<std::string> doomed;
  for (const auto& n : names) {
    const auto it = session.estimatesModels.find(n);
    if (it == session.estimatesModels.end() || !it->second || !it->second->esampleVar) continue;
    if (dataHasVar(session, estVarName(n))) doomed.push_back(estVarName(n));
  }
  if (!doomed.empty()) (void)runDrop(doomed, session);
}

}  // namespace

void clearStoredEstimates(SessionState& session) {
  esampleDropStoreVars(session.estimatesNames(), session);
  session.estimatesClear();
}

// ===========================================================================
// eststo / est store / estimates store

ojson runEststo(const std::optional<std::string>& name,
                const std::optional<std::string>& title, SessionState& session,
                const std::string& autoPrefix, bool viaEststo) {
  const auto& lmOpt = session.lastModel();
  bool hasEstBlock = session.lastEstBlock.has_value();
  // FU-1176: estimates.ado Store refuses only an EMPTY e(cmd) (r(301), the
  // G5b case below). An `ereturn post` e() whose eclass program set e(cmd)
  // (w2an_coefplot_family.do cf_lpost: `ereturn post cfb cfV` + `ereturn local
  // cmd "lpost"`, then `estimates store lp`, rc 0 in Stata: cf_at_eq) is
  // stored as that posting.
  const std::string* postedCmd = session.eval.eStrings.find("cmd");
  // BUG-1045: eststo first fills an empty e(cmd) with "." whenever e() holds
  // anything (eststo's placeholder `eret local cmd "."`), then stores it; only
  // `estimates store` itself keeps the empty-e(cmd) refusal below.
  if (viaEststo && !hasEstBlock && !lmOpt.has_value() && session.ePostedLive &&
      (postedCmd == nullptr || postedCmd->empty()) &&
      (!session.eval.eScalars.items().empty() || !session.eval.eStrings.items().empty() ||
       !session.eval.eMatrices.empty())) {
    session.eval.setEString("cmd", ".");
    postedCmd = session.eval.eStrings.find("cmd");
  }
  const bool postedWithCmd = !hasEstBlock && !lmOpt.has_value() && session.ePostedLive &&
                             postedCmd != nullptr && !postedCmd->empty();
  if (postedWithCmd) {
    ojson snapshot;
    if (session.eval.lastModelResult.has_value() && session.eval.lastModelResult->is_object())
      snapshot = makeSnapshot(*session.eval.lastModelResult);
    if (!snapshot.contains("coeftable")) snapshot["coeftable"] = ojson::array();
    const std::string* dv = session.eval.eStrings.find("depvar");
    snapshot["depvar"] = dv != nullptr ? *dv : std::string();
    snapshot["estimator"] = *postedCmd;
    const double* nE = session.eval.eScalars.find("N");
    snapshot["n_obs"] = nE != nullptr ? ojson(*nE) : ojson(nullptr);
    snapshot["escalars"] = ojson::object();
    snapshot["added"] = emptyAddedBag();
    if (title.has_value()) snapshot["title"] = *title;
    std::string storedName = session.eststo(name, snapshot, autoPrefix, viaEststo);
    auto sm = std::make_shared<SessionState::StoredModel>();
    sm->lastModelResult = session.eval.lastModelResult;
    sm->eScalars = session.eval.eScalars;
    sm->eStrings = session.eval.eStrings;
    sm->eMatrices = session.eval.eMatrices;
    sm->eOnFitNames = session.eOnFitNames;
    sm->postedE = true;
    sm->postedEsampleSerial = session.ePostedEsampleSerial;
    if (dataHasVar(session, estVarName(storedName)))
      (void)runDrop({estVarName(storedName)}, session);  // never another results' sample
    session.estimatesModelSet(storedName, std::move(sm));
    session.activeEstimatesName = storedName;
    ojson out;
    out["type"] = "display";
    out["text"] = "(stored as " + storedName + ")";
    return out;
  }
  if (!hasEstBlock && lmOpt.has_value() && !lmOpt->ePosted) {
    // e15d_eststo2 (w2g_ebmat.log:2497-2504): an e() that no `ereturn post`
    // produced (rddensity, LastModel::ePosted) is not storable. estimates store
    // exits 301 on its empty e(cmd); eststo fills
    // e(cmd) "." first, and the hold step then refuses the
    // unposted e() with r(301) "last estimates not found".
    return ojson(eval::errorBlock(
        "engine_error",
        viaEststo ? "last estimates not found"
                  : "last estimation results not found, nothing to store",
        std::nullopt, std::nullopt, std::nullopt, std::string("r(301)")));
  }
  if (!hasEstBlock && !lmOpt.has_value()) {
    // FU-580: estimates.ado Store exits 301 when e(cmd) is empty -- after
    // `ereturn post` too (w2_esample.log:1507 G5b `301|...`). The rc rides the
    // errors.h builder; a bare engine error surfaced as rc 1.
    return ojson(eval::errorBlock(
        "engine_error",
        viaEststo ? "eststo: no estimation result in memory. Run a regression first."
                  : "last estimation results not found, nothing to store",
        std::nullopt, std::nullopt, std::nullopt, std::string("r(301)")));
  }

  ojson snapshot;
  if (hasEstBlock) {
    snapshot = makeSnapshot(*session.lastEstBlock);
    // mlogit's block names no depvar (its outcomes each carry one); e(depvar)
    // is the fitted model's, which esttab heads the column with (ek_e34 `k3`).
    // The block (and so e(depvar)) is empty for mlogit; the fit's own --y is
    // the dependent variable.
    if (snapshot.value("depvar", std::string()).empty() && snapshot.contains("outcomes") &&
        lmOpt.has_value()) {
      snapshot["depvar"] = !lmOpt->depvar.empty() ? lmOpt->depvar : lmOpt->y;
    }
  } else if (lmOpt.has_value() && lmOpt->result.has_value()) {
    snapshot = makeSnapshot(*lmOpt->result);
  } else {
    // _coeftable_from_last_model fallback: LastModel caches no b/se values
    // directly, so this is always an empty coeftable (mirrors the oracle's
    // own documented "rare no-cache case" guard).
    snapshot["coeftable"] = ojson::array();
    snapshot["n_obs"] = nullptr;
    snapshot["r2"] = nullptr;
    snapshot["depvar"] = lmOpt.has_value() ? lmOpt->depvar : std::string();
    snapshot["estimator"] = lmOpt.has_value() ? lmOpt->cobraCmd : std::string();
    snapshot["fe"] = nullptr;
  }

  // W2AK BASELEV: the factor base levels, read while this fit's data and
  // e(sample) are still the live ones (handlers_aer_tables.cpp).
  if (snapshot.contains("coeftable")) {
    const ojson bl = factorBaseLevels(snapshot["coeftable"], session);
    if (!bl.empty()) snapshot["base_levels"] = bl;
  }

  // AER-SCOPE1 bundle E: e() scalars + whatever `estadd` attached to the ACTIVE
  // estimate. Stata's order is estadd-then-eststo (the corpus writes it that way
  // 5,725 times), so the bag is picked up HERE rather than by a later `estadd`
  // reaching into the store.
  snapshot["escalars"] = hasEstBlock ? escalarsFrom(*session.lastEstBlock) : ojson::object();

  // The bag only travels if it still belongs to THIS estimate. `lastEstBlock` is
  // assigned directly by five different TUs, so there is no single place to
  // clear the bag from when a new regression replaces e() -- staleness is
  // therefore decided at READ time, here, against the shared fingerprint.
  // Discarding silently would be the wrong half of honest: an `estadd`ed row
  // would just be missing from the table, rc 0.
  std::string note;
  const std::string fp =
      hasEstBlock ? SessionState::estAddFingerprint(*session.lastEstBlock) : std::string();
  bool bagHasContent = false;
  for (const char* section : {"scalars", "locals", "matrices"}) {
    if (session.estAdded.contains(section) && !session.estAdded[section].empty()) {
      bagHasContent = true;
      break;
    }
  }
  if (bagHasContent && !session.estAddedFp.empty() && session.estAddedFp == fp) {
    snapshot["added"] = session.estAdded;
  } else {
    snapshot["added"] = emptyAddedBag();
    if (bagHasContent) {
      note = "\n(estadd: previous additions discarded -- a new estimation replaced e())";
      session.estAddedClear();
    }
  }

  // B4-W3b: `estimates store NAME, title("...")`. Stored ONLY when the option
  // was written — an absent key and an empty string are different things here,
  // because the corpus's `est store r`i', title("`lab'")` can legitimately pass
  // an empty macro and Stata keeps that as an empty title, not as no title.
  if (title.has_value()) snapshot["title"] = *title;

  std::string storedName = session.eststo(name, snapshot, autoPrefix, viaEststo);

  // W2 esttabnames (BUG-553): stash the MODEL beside the table, so `estimates
  // restore NAME` can make it the active one again -- the LastModel, the e()
  // block, and the macro layer's e() stores as they stand NOW (estadd's
  // scalars/locals/matrices live there, not in LastModel::result). The e() it
  // came from now carries this name (estimates.ado Store: `ereturn hidden
  // local _estimates_name`), which is what a later `esttab .` expands to.
  {
    auto sm = std::make_shared<SessionState::StoredModel>();
    // The LastModel is stashed only when it is the model BEHIND this e()
    // block. `estpost` (handlers_aer_tables.cpp) posts a block and leaves the
    // previous fit's LastModel in place, and so does `csdid`; pairing the two
    // would let a restore publish the old fit's e(cmd)/_b[] under the new
    // results' name. Both the depvar and, where the fit's result records one,
    // e(N) must agree. Without a model the restore refuses by name instead
    // (estimatesRestore).
    bool modelMatchesBlock = session.lastModel().has_value();
    if (modelMatchesBlock && hasEstBlock) {
      const ojson& blk = *session.lastEstBlock;
      const auto& lm = *session.lastModel();
      modelMatchesBlock = blk.value("depvar", std::string()) == lm.depvar;
      if (modelMatchesBlock && lm.result.has_value() && lm.result->is_object() &&
          lm.result->contains("n_obs") && (*lm.result)["n_obs"].is_number() &&
          blk.contains("n_obs") && blk["n_obs"].is_number()) {
        modelMatchesBlock = (*lm.result)["n_obs"].get<double>() == blk["n_obs"].get<double>();
      }
    }
    if (modelMatchesBlock) {
      sm->lastModel = session.lastModel();
      sm->lastModelResult = session.eval.lastModelResult;
      sm->eNameContract = session.eNameContract;
    }
    // eb-order T-P3: the e() stores belong to the live e() whether or not a
    // fitted model stands behind its block -- an estpost posting has none, and
    // `estimates restore` installs such a store as the posting it was
    // (SessionState::estimatesRestore), its e(N)/e(cmd)/e(mean) included.
    if (modelMatchesBlock || hasEstBlock) {
      sm->eScalars = session.eval.eScalars;
      sm->eStrings = session.eval.eStrings;
      sm->eMatrices = session.eval.eMatrices;
    }
    sm->estBlock = session.lastEstBlock;
    // A posted e() keeps its `ereturn post ..., esample()` mask serial.
    if (!session.lastModel().has_value() && session.ePostedLive)
      sm->postedEsampleSerial = session.ePostedEsampleSerial;
    // e(sample) T3: `_est_NAME`, for results whose model PIET holds (a posting
    // with no model behind it has no sample PIET could write).
    if (modelMatchesBlock) {
      sm->esampleVar = esampleStoreVar(storedName, session);
    } else if (dataHasVar(session, estVarName(storedName))) {
      (void)runDrop({estVarName(storedName)}, session);  // never another results' sample
    }
    session.estimatesModelSet(storedName, std::move(sm));
    session.activeEstimatesName = storedName;
  }
  ojson out;
  out["type"] = "display";
  // One block, never an array: the `eststo` arm pushes this result directly, and
  // an array reaching the block stream is read with value("type") downstream and
  // throws type_error.306 (the class that aborted the selftest binary in bundle
  // D). The note is folded into the text for exactly that reason.
  out["text"] = "(stored as " + storedName + ")" + note;
  return out;
}

ojson runEststoDirNames(const std::vector<std::string>& namelist, SessionState& session);
ojson runEststoDir(SessionState& session) { return runEststoDirNames({}, session); }

// W2 esttabnames (BUG-552): `estimates dir A_*` lists only what the namelist
// expands to (no namelist = every stored name). Declared here and
// in dispatcher.cpp's forward block, not in handlers.h (outside this lane).
ojson runEststoDirNames(const std::vector<std::string>& namelist, SessionState& session) {
  std::vector<std::string> names = session.estimatesNames();
  if (!namelist.empty()) {
    SessionState::EstNamelist ex = session.expandEstimatesNamelist(namelist);
    if (ex.rc != 0) return estNamelistError(ex);
    names.clear();
    for (const auto& n : ex.names) {
      if (n != ".") names.push_back(n);  // unstored active results have no dir row
    }
  }
  if (names.empty()) {
    ojson out;
    out["type"] = "display";
    out["text"] = "(no stored estimates)";
    return out;
  }
  std::string text = "Stored estimation results:";
  for (const auto& n : names) {
    const ojson* snap = session.estimatesGet(n);
    std::string depvar = (snap && snap->contains("depvar") && (*snap)["depvar"].is_string())
                             ? (*snap)["depvar"].get<std::string>()
                             : std::string();
    text += "\n  " + n + "  (" + depvar + ")";
    // B4-W3b: the ONE place a stored `title()` is observable. Appended only
    // when the option was actually written, so every pre-B4 store — and every
    // freeze fixture captured off one — prints the byte-identical line it did
    // before (`title` answered r(198) until this delta, so no captured store
    // can carry the key).
    if (snap && snap->contains("title") && (*snap)["title"].is_string()) {
      text += "  \"" + (*snap)["title"].get<std::string>() + "\"";
    }
  }
  ojson out;
  out["type"] = "display";
  out["text"] = text;
  return out;
}

// ===========================================================================
// B4-W3b / BUG-470: estimates save / estimates use
//
// Stata runs all four links at rc 0 (oracle pins_oracle_g.log:283-298):
//   estimates save "b4g_est", replace -> 0
//   estimates use  "b4g_est"          -> 0
//   e(cmd)==regress, e(depvar)==price   <- the RESTORE puts e() back
//   predict double pxb, xb            -> 0, N 74
// The third line is the one that decides whether this is real: `predict` in a
// session that has run ANY regression answers rc 0 whether or not the restore
// happened, off the still-live model. So `use` restores LastModel wholesale —
// which is what predict/test/lincom/margins all read — and not just a display.

ojson runEstimatesSave(const std::string& fileArg, bool replace, SessionState& session) {
  // BUG-1248: StataBE 19.5 `estimates save a` with no estimates, after `ereturn
  // clear` as well: "last estimates not found" r(301).
  if (!session.lastEstBlock.has_value() && !session.lastModel().has_value()) {
    return ojson(eval::errorBlock("engine_error", "last estimates not found", std::nullopt,
                                  std::nullopt, std::nullopt, "r(301)"));
  }
  std::string fp = withSterSuffix(resolveUserPath(session, fileArg));
  if (fileExistsEt(fp) && !replace) {
    return engineErrorBlockEt("estimates save: file already exists: " + fp +
                              ". Use `, replace` to overwrite.");
  }

  ojson doc;
  doc["_magic"] = "PIETEST1";
  // The e() display block AND the replayable model. Both, because they answer
  // different questions: `est_block` is what `esttab`/`estimates table` render
  // from, `last_model` is what `predict` re-fits from. Writing only the first
  // is exactly the partial `use` BUG-470's reopening condition names.
  doc["est_block"] = session.lastEstBlock.has_value() ? *session.lastEstBlock : ojson(nullptr);
  doc["last_model"] = session.lastModel().has_value() ? lastModelToJson(*session.lastModel())
                                                      : ojson(nullptr);
  doc["added"] = session.estAdded;
  // eb-order T-P3: a posting with no fitted model behind it (estpost) has no
  // LastModel to republish its e() on `use`, so its e() stores travel in the
  // file -- e(cmd)/e(subcmd), e(N), e(mean)/e(sd)/... (w2v_estpost_ematrix.log
  // ep14s_ecmd `estpost|summarize`, ep14s_emean_12). A fit's stores come back
  // from its LastModel, as before.
  if (!session.lastModel().has_value()) {
    ojson strs = ojson::object(), scs = ojson::object(), mats = ojson::object();
    for (const auto& [k, v] : session.eval.eStrings.items()) strs[k] = v;
    for (const auto& [k, v] : session.eval.eScalars.items())
      scs[k] = std::isnan(v) ? ojson(nullptr) : ojson(v);
    for (const auto& [k, m] : session.eval.eMatrices) {
      ojson data = ojson::array();
      for (const auto& row : m.data) {
        ojson r = ojson::array();
        for (double v : row) r.push_back(std::isnan(v) ? ojson(nullptr) : ojson(v));
        data.push_back(r);
      }
      mats[k] = {{"rownames", m.rownames}, {"colnames", m.colnames}, {"data", data}};
    }
    doc["e_posted"] = {{"strings", strs}, {"scalars", scs}, {"matrices", mats}};
  }

  std::ofstream f(cecon::to_native_path(fp), std::ios::binary);
  if (!f) return engineErrorBlockEt("estimates save: could not write file: " + fp);
  std::string text = doc.dump();
  f.write(text.data(), static_cast<std::streamsize>(text.size()));
  if (!f) return engineErrorBlockEt("estimates save: could not write file: " + fp);

  ojson out;
  out["type"] = "display";
  out["class"] = "note";
  out["text"] = "file " + fp + " saved";
  return out;
}

ojson runEstimatesUse(const std::string& fileArg, SessionState& session) {
  std::string fp = withSterSuffix(resolveUserPath(session, fileArg));
  std::ifstream f(cecon::to_native_path(fp), std::ios::binary);
  if (!f) {
    return engineErrorBlockEt("estimates use: file " + fp + " not found");
  }
  std::ostringstream ss;
  ss << f.rdbuf();
  ojson doc;
  try {
    doc = ojson::parse(ss.str());
  } catch (...) {
    doc = ojson(nullptr);
  }
  if (!doc.is_object() || doc.value("_magic", std::string()) != "PIETEST1") {
    // A REAL Stata `.ster` lands here, and this is the message it must get: the
    // refusal is about the FILE FORMAT, which is a named limit (PIET cannot
    // read Stata's binary estimation format), and not about the command, which
    // works. BUG-470 draws that line explicitly.
    return engineErrorBlockEt(
        "estimates use: " + fp +
        " is not a PIET estimates file. This version of PIET reads only the estimates it "
        "wrote itself (`estimates save`); Stata's own binary .ster format is not supported, "
        "so refusing rather than restoring a model this file does not describe.");
  }

  // Order matters: setLastModel() republishes e(cmd)/e(depvar)/e(vce) and
  // re-points eval.lastModelResult, so it must run before anything reads e().
  if (doc.contains("last_model") && doc["last_model"].is_object()) {
    session.setLastModel(lastModelFromJson(doc["last_model"]));
    // e(sample) T3: results read from a file carry no estimation sample; Stata
    // answers 0 for every row (w2_esample.log:1531 G6 `0|0|0|28|`), and
    // `estimates esample:` may then set one without `replace` (:1542 G7).
    const LastModel& used = *session.lastModel();
    session.setLastModelEsample(LastModel::EsampleState::kNone, 0, std::string(), used.dataPath);
  } else {
    // eb-order T-P3: a file with no fitted model behind it (an estpost
    // posting) replaces the whole e() -- the previous fit's LastModel, its
    // stores and its posted matrices go, so neither `_b[]` nor predict/test
    // can answer from that fit (SPEC C: the stale `last_model` after estpost).
    session.lastModel_reset();
    session.eval.eScalars = eval::OrderedFloatDict();
    session.eval.eStrings.clear();
    session.eval.eMatrices.clear();
    session.ePostedLive = doc.contains("est_block") && doc["est_block"].is_object();
    if (doc.contains("e_posted") && doc["e_posted"].is_object()) {
      const ojson& ep = doc["e_posted"];
      if (ep.contains("strings") && ep["strings"].is_object())
        for (const auto& [k, v] : ep["strings"].items())
          if (v.is_string()) session.eval.setEString(k, v.get<std::string>());
      if (ep.contains("scalars") && ep["scalars"].is_object())
        for (const auto& [k, v] : ep["scalars"].items())
          session.eval.setEScalar(k, v.is_number() ? v.get<double>()
                                                   : std::numeric_limits<double>::quiet_NaN());
      if (ep.contains("matrices") && ep["matrices"].is_object()) {
        for (const auto& [k, mj] : ep["matrices"].items()) {
          eval::EvalMatrix em;
          if (mj.contains("rownames") && mj["rownames"].is_array())
            em.rownames = mj["rownames"].get<std::vector<std::string>>();
          if (mj.contains("colnames") && mj["colnames"].is_array())
            em.colnames = mj["colnames"].get<std::vector<std::string>>();
          if (mj.contains("data") && mj["data"].is_array()) {
            for (const auto& row : mj["data"]) {
              std::vector<double> r;
              if (row.is_array())
                for (const auto& c : row)
                  r.push_back(c.is_number() ? c.get<double>()
                                            : std::numeric_limits<double>::quiet_NaN());
              em.data.push_back(std::move(r));
            }
          }
          session.eval.eMatrices.emplace_back(k, std::move(em));
        }
      }
    }
  }
  if (doc.contains("est_block") && doc["est_block"].is_object()) {
    session.lastEstBlock = doc["est_block"];
  } else {
    session.lastEstBlock.reset();
  }
  // The estadd bag belongs to the estimate, so it travels with it — and the
  // fingerprint is recomputed from the RESTORED block rather than carried, so a
  // bag can never be re-attached to an estimate it did not come from.
  session.estAddedClear();
  if (doc.contains("added") && doc["added"].is_object() && session.lastEstBlock.has_value()) {
    session.estAdded = doc["added"];
    session.estAddedFp = SessionState::estAddFingerprint(*session.lastEstBlock);
  }

  std::string depvar = session.lastModel().has_value() ? session.lastModel()->depvar : std::string();
  ojson out;
  out["type"] = "display";
  out["class"] = "note";
  out["text"] = "(estimates restored from " + fp + (depvar.empty() ? "" : ": " + depvar) + ")";
  return out;
}

ojson runEststoManage(const std::string& sub, const std::optional<std::string>& name,
                      SessionState& session) {
  if (sub == "drop") {
    if (!name.has_value()) return engineErrorBlockEt("estimates drop: requires a model name");
    // BUG-149 (P2-PERF ARC-P). `estimates drop _all` is a documented Stata form
    // and it was being looked up as a literal store NAME, so it always missed.
    // `_all` means every stored estimate, which is what `clear` already does.
    if (*name == "_all") {
      esampleDropStoreVars(session.estimatesNames(), session);  // e(sample) T3
      session.estimatesClear();
      ojson out;
      out["type"] = "display";
      out["text"] = "(dropped all stored estimates)";
      return out;
    }
    // W2 esttabnames (BUG-552): the operand is a NAMELIST:
    // `estimates drop A_*` / `estimates drop m1 m2` drop every
    // match, `.` is skipped, and a token matching nothing is r(111)
    // "estimation result NAME not found" -- also for a plain misspelt name,
    // which BUG-149 had answered at rc 1 with a message of its own. Nothing is
    // dropped when any token misses (the expansion fails first).
    std::vector<std::string> toks;
    std::istringstream tss(*name);
    for (std::string t; tss >> t;) toks.push_back(t);
    SessionState::EstNamelist ex = session.expandEstimatesNamelist(toks);
    if (ex.rc != 0) return estNamelistError(ex);
    esampleDropStoreVars(ex.names, session);  // e(sample) T3: G8b r(111)
    std::string dropped;
    for (const auto& n : ex.names) {
      if (n == "." || !session.estimatesDrop(n)) continue;
      dropped += (dropped.empty() ? "" : " ") + n;
    }
    ojson out;
    out["type"] = "display";
    out["text"] = "(dropped " + dropped + ")";
    return out;
  }
  if (sub == "clear") {
    // e(sample) T3: `_est_*` go with the stores (G9b r(111)); the active
    // e(sample) stays (w2_esample.log:1621 G10 `0|28|28|`).
    clearStoredEstimates(session);
    ojson out;
    out["type"] = "display";
    out["text"] = "(estimates cleared)";
    return out;
  }
  return engineErrorBlockEt("eststo: unknown subcommand '" + sub + "'");
}

// ===========================================================================
// e(sample) T3: estimates restore NAME (the namelist is expanded by the
// dispatcher's restore arm, which owns the namelist's r(111)/r(198)). See
// the T3 note above esampleStoreVar for the Stata rules this follows.
ojson runEstimatesRestore(const std::string& name, SessionState& session) {
  // estimates.ado Restore: NAME already the active results -> nothing happens,
  // so the live e(sample) keeps following the rows (G3/G4/G4b). Decided BEFORE
  // the re-activation below, which renames the active results.
  const bool wasActive = session.activeStoredName() == name;
  const std::optional<LastModel> live = session.lastModel();
  if (!session.estimatesRestore(name)) {
    return ojson(eval::errorBlock(
        "unsupported_option",
        "estimates restore: " + name + " was stored from results this version of PIET "
        "cannot make active again (no fitted model and no estimation table behind "
        "them); nothing was changed"));
  }
  ojson note;
  note["type"] = "display";
  note["text"] = "(results " + name + " are active now)";
  const auto it = session.estimatesModels.find(name);
  if (it == session.estimatesModels.end() || !it->second || !it->second->lastModel.has_value() ||
      !session.lastModel().has_value())
    return note;
  const LastModel lm = *session.lastModel();  // the restored model (kUnavailable)
  if (wasActive && live.has_value()) {
    session.setLastModelEsample(live->esample, live->esampleSerial, live->esampleInstallPath,
                                lm.dataPath);
    return note;
  }
  if (!it->second->esampleVar) return note;  // stored without a sample: the named r(198)
  const std::string var = estVarName(name);
  if (!dataHasVar(session, var)) {
    session.setLastModelEsample(LastModel::EsampleState::kStale, 0, std::string(), lm.dataPath);
    return note;
  }
  const bool changed = session.datasetChanged;
  // est_unhold drops `_est_NAME`, est_hold writes it anew: it ends up last.
  {
    eval::OptValue on;
    on.isBool = true;
    on.boolValue = true;
    (void)runOrder({var}, {{"last", on}}, session);
  }
  // The restored rows: `_est_NAME` nonzero and nonmissing (est_hold: `replace
  // touse = 0 if missing(touse)`, then `ereturn repost, esample(touse)`). They
  // are written under the model's own serial, so a frame that still carries
  // this model's original marker (a `preserve` taken before the store) reads
  // the same results' sample.
  std::vector<double> cells;
  unsigned long long serial = lm.esampleSerial;
  bool read = false;
  try {
    Datastore* ds = session.datastore();
    if (ds != nullptr && ds->has(var) && ds->col(var).dtype == cecon::DType::F64) {
      if (serial == 0) serial = ++session.esampleColumnInstalls;
      const cecon::Column& c = ds->col(var);
      const double* v = c.f64();
      cells.resize(static_cast<size_t>(ds->nRows()));
      for (int64_t i = 0; i < ds->nRows(); ++i)
        cells[static_cast<size_t>(i)] =
            (!std::isnan(v[i]) && v[i] != 0.0) ? static_cast<double>(serial) : 0.0;
      read = true;
    }
  } catch (const std::exception&) {
    read = false;
  }
  if (!read || !esampleRewriteHeadMask(session, cells)) {
    session.datasetChanged = changed;
    return note;  // kUnavailable: the named r(198), never another model's rows
  }
  session.datasetChanged = changed;
  session.setLastModelEsample(LastModel::EsampleState::kColumn, serial, lm.esampleInstallPath,
                              lm.dataPath);
  return note;
}

// e(sample) T3: `estimates esample` (report) and `estimates esample: [varlist]
// [if] [in] [, replace]` (set) -- Stata's estimates_sample.ado, read for this
// port. The set form marks the rows that satisfy if/in with every variable of
// varlist nonmissing (marksample), refuses r(322) "no; e(sample) already set"
// when the active results carry one and `replace` is absent, posts the rows as
// e(sample) and sets e(_estimates_sample) "user" (w2_esample.log:1542 G7
// `0|28|` after `estimates use`, whose results carry none). Weights,
// stringvars() and zeroweight are not implemented: refused by name.
ojson runEstimatesEsample(bool colon, const std::vector<std::string>& varlistIn,
                          const std::optional<std::string>& ifExpr,
                          const std::optional<std::string>& inRange, bool hasWeight, bool replace,
                          SessionState& session) {
  const SessionState::EsampleReading r = session.esampleReading();
  // BUG-1211: an `ereturn post` result takes an e(sample) too (estimates_sample.ado
  // reposts it), on the posting's own marker serial.
  const bool posting = !session.lastModel().has_value() && session.ePostedLive;
  const bool userSet = (session.lastModel().has_value() || posting) &&
                       session.eval.eStrings.get("_estimates_sample", "") == "user";
  const bool hasSample = (session.lastModel().has_value() || posting) &&
                         r.kind != SessionState::EsampleReading::Kind::kZero;
  // The varlist as Stata's marksample reads it: wildcards, ranges and unique
  // abbreviations (`x*`, `x1-x2`, `longn`; BUG-1211).
  std::vector<std::string> varlist = varlistIn;
  if (colon) {
    if (auto err = resolveByKeyNames(session, varlist)) return *err;
  }
  if (!colon) {
    if (!varlist.empty() || ifExpr.has_value() || inRange.has_value() || hasWeight)
      return ojson(eval::engineError("estimates esample: `estimates esample` takes nothing "
                                     "without the colon (`estimates esample: [varlist] [if] [in]`)",
                                     "syntax_error"));
    std::string who, text;
    if (userSet && hasSample) {
      who = "user";
      text = "e(sample) set by user";
    } else if (hasSample) {
      who = "cmd";
      text = "e(sample) set by estimation command";
    } else {
      who = "zero'd";
      text = "e(sample) not set (0 assumed)";
    }
    session.eval.rValues = eval::OrderedFloatDict();
    session.eval.rStrings.clear();
    session.eval.rStrings.set("who", who);
    ojson out;
    out["type"] = "display";
    out["text"] = text;
    return out;
  }
  if (hasWeight)
    return ojson(eval::errorBlock(
        "unsupported_option",
        "estimates esample: weights are not supported in this version of PIET; nothing was set.",
        std::nullopt, std::nullopt, std::nullopt, std::string("r(198)")));
  if (!session.lastModel().has_value() && !posting)
    return ojson(eval::errorBlock("engine_error", "last estimates not found", std::nullopt,
                                  std::nullopt, std::nullopt, std::string("r(301)")));
  if (!replace && hasSample)
    return ojson(eval::errorBlock("engine_error", "no; e(sample) already set", std::nullopt,
                                  std::nullopt, std::nullopt, std::string("r(322)")));
  if (!session.hasData()) return ojson(eval::noDataLoaded());
  Datastore* ds = session.datastore();
  if (ds == nullptr) return ojson(eval::noDataLoaded());
  for (const auto& v : varlist) {
    if (!ds->has(v) || isHiddenColumn(v))
      return ojson(eval::errorBlock("engine_error", "variable " + v + " not found", std::nullopt,
                                    std::nullopt, std::nullopt, std::string("r(111)")));
    if (ds->col(v).dtype != cecon::DType::F64)
      return ojson(eval::errorBlock(
          "unsupported_option",
          "estimates esample: string variable " + v + " in the varlist (stringvars()) is not "
          "supported in this version of PIET; nothing was set.",
          std::nullopt, std::nullopt, std::nullopt, std::string("r(198)")));
  }
  const bool changed = session.datasetChanged;
  // The if/in rows, marked by `generate` itself so the condition goes through
  // the one row rewrite every `if` takes (an `if e(sample)` reads the CURRENT
  // mask). A missing condition is true, as in Stata: `1 if .` is 1.
  // A scratch name no variable holds (BUG-1211: a user's `__piet_esw__` was dropped).
  std::string tmp = "__piet_esw__";
  for (int k = 1; ds->has(tmp); ++k) tmp = "__piet_esw" + std::to_string(k) + "__";
  std::string line = "quietly generate byte " + tmp + " = 1";
  if (ifExpr.has_value() && !ifExpr->empty()) line += " if " + *ifExpr;
  if (inRange.has_value() && !inRange->empty()) line += " in " + *inRange;
  const ojson gen = piet::dispatch(line, session);
  if (gen.is_array()) {
    for (const auto& b : gen) {
      if (b.is_object() && b.value("type", std::string()) == "error") {
        if (dataHasVar(session, tmp)) (void)runDrop({tmp}, session);
        session.datasetChanged = changed;
        return b;
      }
    }
  }
  std::vector<double> cells;
  const unsigned long long serial = session.esampleColumnInstalls + 1;
  bool marked = false;
  try {
    ds = session.datastore();
    if (ds != nullptr && ds->has(tmp)) {
      const int64_t n = ds->nRows();
      cells.assign(static_cast<size_t>(n), 0.0);
      const double* m = ds->col(tmp).f64();
      std::vector<const double*> cols;
      for (const auto& v : varlist) cols.push_back(ds->col(v).f64());
      for (int64_t i = 0; i < n; ++i) {
        bool in = m[i] == 1.0;
        for (const double* c : cols) in = in && !std::isnan(c[i]);
        if (in) cells[static_cast<size_t>(i)] = static_cast<double>(serial);
      }
      marked = true;
    }
  } catch (const std::exception&) {
    marked = false;
  }
  if (dataHasVar(session, tmp)) (void)runDrop({tmp}, session);
  if (!marked || !esampleRewriteHeadMask(session, cells)) {
    session.datasetChanged = changed;
    return engineErrorBlockEt(
        "estimates esample: the estimation-sample marker could not be written; nothing was set.");
  }
  session.esampleColumnInstalls = serial;
  session.datasetChanged = changed;
  if (posting) {
    session.ePostedEsampleSerial = serial;  // esampleReading's posting branch reads it
  } else {
    const LastModel& lm = *session.lastModel();
    session.setLastModelEsample(LastModel::EsampleState::kColumn, serial, lm.esampleInstallPath,
                                lm.dataPath);
  }
  // `ereturn repost` takes e(_estimates_name) away; estimates_sample.ado then
  // posts e(_estimates_sample) "user", which the next fit drops.
  session.activeEstimatesName.clear();
  session.eval.setEString("_estimates_sample", "user");
  session.noteEOnFit("_estimates_sample");
  ojson out;
  out["type"] = "echo";
  out["text"] = "";
  return out;
}

// ===========================================================================
// esttab

namespace {

std::vector<std::pair<std::string, double>> parseStarOpt(const std::optional<std::string>& starStr) {
  static const std::vector<std::pair<std::string, double>> kDefault = {
      {"***", 0.01}, {"**", 0.05}, {"*", 0.10}};
  if (!starStr.has_value() || starStr->empty()) return kDefault;
  std::istringstream iss(*starStr);
  std::vector<std::string> tokens;
  std::string tok;
  while (iss >> tok) tokens.push_back(tok);
  std::vector<std::pair<std::string, double>> result;
  size_t i = 0;
  while (i + 1 < tokens.size()) {
    try {
      size_t pos;
      double threshold = std::stod(tokens[i + 1], &pos);
      if (pos != tokens[i + 1].size()) return kDefault;
      result.emplace_back(tokens[i], threshold);
      i += 2;
    } catch (...) {
      return kDefault;
    }
  }
  if (result.empty()) return kDefault;
  std::sort(result.begin(), result.end(),
            [](const auto& a, const auto& b) { return a.second < b.second; });
  return result;
}

std::string starsFor(const ojson& p, const std::vector<std::pair<std::string, double>>& thresholds) {
  if (p.is_null()) return "";
  double pv;
  try {
    pv = p.get<double>();
  } catch (...) {
    return "";
  }
  if (std::isnan(pv)) return "";
  for (const auto& [label, cutoff] : thresholds) {
    if (pv < cutoff) return label;
  }
  return "";
}

// _fmt_b: Stata-style %fmt formatting (only %9.3f-shaped formats are
// exercised by the corpus; matches the oracle's regex-based %-?(\d+)?\.(\d+)
// ([fg]) extraction with a 3-decimal fallback).
std::string fmtB(const ojson& val, const std::string& fmt) {
  if (val.is_null()) return "";
  double v;
  try {
    v = val.get<double>();
  } catch (...) {
    return "";
  }
  if (std::isnan(v)) return "";
  static const std::regex re(R"(%-?(\d+)?\.(\d+)([fg]))");
  std::smatch m;
  int prec = 3;
  char kind = 'f';
  if (std::regex_search(fmt, m, re)) {
    prec = std::stoi(m[2].str());
    kind = m[3].str()[0];
  }
  char buf[64];
  if (kind == 'f') {
    std::snprintf(buf, sizeof(buf), "%.*f", prec, v);
  } else {
    std::snprintf(buf, sizeof(buf), "%.*g", prec, v);
  }
  return std::string(buf);
}

}  // namespace

ojson runEsttab(const std::vector<std::string>& modelNamesIn,
                const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                SessionState& session) {
  // W2 esttabnames (BUG-552): a written namelist is EXPANDED (namelist
  // semantics: `A_*`, `A_?`, `*`, `_all`, `.`), in store order, before any
  // lookup -- w2_smalls.log section G. Every name the expansion returns is
  // stored except the literal ".", which is the active results when they carry
  // no store name; that column is built from e() below.
  std::vector<std::string> modelNames;
  if (!modelNamesIn.empty()) {
    SessionState::EstNamelist ex = session.expandEstimatesNamelist(modelNamesIn);
    if (ex.rc != 0) return estNamelistError(ex);
    modelNames = std::move(ex.names);
  } else {
    modelNames = session.estimatesNames();
  }

  // AER-SCOPE1 bundle E (TABLES, 2026-09-04): fall back to the ACTIVE estimate
  // when the caller named no models and the store is empty. `estpost summarize
  // x y` followed by a bare `esttab, cells(mean sd)` is estout's own documented
  // idiom and the shape the corpus writes -- estpost posts to e() and never
  // stores, so before this the bare esttab answered "no stored estimates" with
  // the estimate sitting right there in e(). Plain `regress y x` + `esttab`
  // reaches the same path.
  //
  // Deliberately does NOT call `session.eststo(...)`: a read must not have a
  // side effect, and silently minting an `m1` would change what a LATER
  // `eststo` numbers itself. The snapshot is built and carried locally instead.
  // Strict addition -- the only behaviour this replaces is an error.
  auto liveSnapshot = [&session]() {
    ojson snap = makeSnapshot(*session.lastEstBlock);
    snap["escalars"] = escalarsFrom(*session.lastEstBlock);
    const std::string fp = SessionState::estAddFingerprint(*session.lastEstBlock);
    snap["added"] = (!session.estAddedFp.empty() && session.estAddedFp == fp)
                        ? session.estAdded
                        : emptyAddedBag();
    return snap;
  };
  std::optional<ojson> adhocSnapshot;
  if (modelNames.empty() && session.lastEstBlock.has_value()) {
    std::string label = session.lastEstBlock->value("depvar", std::string());
    if (label.empty()) label = "est1";
    adhocSnapshot = liveSnapshot();
    modelNames.push_back(label);
  }

  if (modelNames.empty()) {
    // BUG-1248: nothing stored and no active estimates is estout's `.` not found,
    // r(301) (StataBE 19.5 `clear all` / `esttab`); it was rc 1.
    if (!session.eval.eLive())
      return ojson(eval::errorBlock("engine_error", "last estimates (.) not found", std::nullopt,
                                    std::nullopt, std::nullopt, std::string("r(301)")));
    return engineErrorBlockEt(
        "esttab: no models specified and no stored estimates. "
        "Run a regression, then `eststo m1` first.");
  }

  std::vector<ojson> snapshots;
  if (adhocSnapshot.has_value()) {
    snapshots.push_back(*adhocSnapshot);
  } else {
    for (const auto& nm : modelNames) {
      if (nm == ".") {
        // The active results, unstored. A fit that filled only LastModel (no
        // e() display block) has no snapshot to render -- refused by name
        // rather than shown as an empty column.
        if (!session.lastEstBlock.has_value()) {
          return engineErrorBlockEt(
              "esttab: the active estimation results (.) carry no coefficient table this "
              "version of PIET can render; store them first (`estimates store NAME`)");
        }
        snapshots.push_back(liveSnapshot());
        continue;
      }
      const ojson* snap = session.estimatesGet(nm);
      if (snap == nullptr) {
        // Unreachable after the expansion above (it returns stored names or
        // "."), kept so a future caller cannot index a null snapshot.
        return estNamelistError({{}, 111, "estimation result " + nm + " not found"});
      }
      snapshots.push_back(*snap);
    }
  }

  // parse_esttab_opts.
  bool showSe = true;
  bool nostar = false;
  std::string bFmt = "%9.3f";
  std::optional<std::string> starOpt;
  for (const auto& [k, v] : opts) {
    if (k == "nostar" && v.isBool && v.boolValue) nostar = true;
    if (k == "b" && !v.isBool) bFmt = v.strValue;
    if (k == "se" && v.isBool && v.boolValue) showSe = true;
    if (k == "t" && v.isBool && v.boolValue) showSe = false;
    if (k == "star" && !v.isBool) starOpt = v.strValue;
  }
  // BS-097: nostar/b/se/t/star are the only esttab options this handler
  // reads (real Stata esttab also has title/mtitles/label/wide/ar2/scalars/
  // keep()/drop()/order()/compress/nogaps/plain/booktabs/replace/append/
  // using, not implemented here) — a leftover key now reads as an honest
  // reject instead of a silent no-op.
  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kEsttabKnownOpts = kVocabEsttab.known;
  if (auto err = eval::rejectUnknownOptions(opts, kEsttabKnownOpts)) {
    return *err;
  }
  auto thresholds = parseStarOpt(starOpt);

  // Union of coefficient names in appearance order across models.
  std::vector<std::string> unionNames;
  std::set<std::string> seen;
  for (const auto& snap : snapshots) {
    if (!snap.contains("coeftable")) continue;
    for (const auto& row : snap["coeftable"]) {
      std::string name = row.value("name", std::string());
      if (seen.insert(name).second) unionNames.push_back(name);
    }
  }

  // `transform()`: applied where the CELL is built, so esttab's screen block, its
  // csv and every file writer downstream see the same numbers. One semantics
  // with estout -- the function is shared (piet/handlers.h) rather than copied,
  // which is the BUG-496 lesson applied before the fact.
  TransformSpec trSpec;
  {
    std::string trRaw, trErr;
    for (const auto& [k, v] : opts) {
      if (k == "transform" && !v.isBool) trRaw = v.strValue;
    }
    if (!trRaw.empty() && !parseTransformOpt(trRaw, &trSpec, &trErr)) {
      // r(198): estout answers `invalid pattern in transform(,pattern()) or
      // eform()` there (w1b_transform2.log tr_pat_two), and `engine_error`
      // would make it rc 1.
      return engineErrorBlockEt("esttab: " + trErr, "syntax_error");
    }
  }
  ojson coefRows = ojson::array();
  for (const auto& coefName : unionNames) {
    ojson cells = ojson::array();
    for (size_t trMi = 0; trMi < snapshots.size(); ++trMi) {
      const ojson& snap = snapshots[trMi];
      ojson found;
      bool foundFlag = false;
      if (snap.contains("coeftable")) {
        for (const auto& row : snap["coeftable"]) {
          if (row.value("name", std::string()) == coefName) {
            found = row;
            foundFlag = true;
            break;
          }
        }
      }
      if (!foundFlag) {
        cells.push_back(nullptr);
        continue;
      }
      ojson b = found.contains("b") ? found["b"] : ojson(nullptr);
      ojson se = found.contains("se") ? found["se"] : ojson(nullptr);
      ojson t = found.contains("t") ? found["t"] : ojson(nullptr);
      ojson p = found.contains("p") ? found["p"] : ojson(nullptr);
      bool omitted = b.is_null();
      std::string starStr = (nostar || omitted) ? "" : starsFor(p, thresholds);
      ojson cell;
      cell["b"] = b;
      cell["se"] = se;
      cell["t"] = t;
      cell["p"] = p;
      cell["stars"] = starStr;
      cell["omitted"] = omitted;
      // The stars above come from the UNTRANSFORMED p, which is what the oracle
      // shows: `transform(@*2 2)` with stars on keeps `3.493**` (w1b_transform
      // .log s6), and t/p are in neither of the ado's two branches, so a linear
      // rescaling leaves the whole parenthesised row alone (s1/s2/s3).
      if (!trSpec.empty() && !omitted && b.is_number()) {
        const TransformCell tc =
            transformOneCell(trSpec, trMi, coefName, b.get<double>(), snap, session);
        if (tc.matched) {
          if (tc.bMissing) cell["b"] = ojson(nullptr);
          else if (tc.b.has_value()) cell["b"] = *tc.b;
          if (tc.seFactor.has_value() && se.is_number()) {
            if (std::isnan(*tc.seFactor)) cell["se"] = ojson(nullptr);
            else cell["se"] = se.get<double>() * *tc.seFactor;
          }
        }
      }
      cells.push_back(cell);
    }
    ojson cr;
    cr["name"] = coefName;
    cr["cells"] = cells;
    coefRows.push_back(cr);
  }

  // Footer rows: N and R2.
  ojson nVals = ojson::array(), r2Vals = ojson::array();
  for (const auto& snap : snapshots) {
    ojson n = snap.contains("n_obs") ? snap["n_obs"] : ojson(nullptr);
    ojson r2 = snap.contains("r2") ? snap["r2"] : ojson(nullptr);
    if (n.is_null()) {
      nVals.push_back(nullptr);
    } else {
      try {
        nVals.push_back(std::to_string(static_cast<long long>(n.get<double>())));
      } catch (...) {
        nVals.push_back(nullptr);
      }
    }
    if (r2.is_null()) {
      r2Vals.push_back(nullptr);
    } else {
      try {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.4f", r2.get<double>());
        r2Vals.push_back(std::string(buf));
      } catch (...) {
        r2Vals.push_back(nullptr);
      }
    }
  }
  ojson footers = ojson::array();
  {
    ojson f1;
    f1["label"] = "N";
    f1["values"] = nVals;
    footers.push_back(f1);
    ojson f2;
    f2["label"] = "R²";
    f2["values"] = r2Vals;
    footers.push_back(f2);
  }

  // CSV export.
  std::string csv = ",";
  for (size_t i = 0; i < modelNames.size(); ++i) {
    if (i) csv += ",";
    csv += modelNames[i];
  }
  for (const auto& cr : coefRows) {
    std::string bLine = cr["name"].get<std::string>();
    std::string seLine;
    for (const auto& cell : cr["cells"]) {
      bLine += ",";
      seLine += ",";
      if (cell.is_null()) continue;
      if (cell.value("omitted", false)) {
        bLine += "(omitted)";
        continue;
      }
      std::string starStr = cell.value("stars", std::string());
      bLine += fmtB(cell["b"], bFmt) + starStr;
      ojson parenVal = showSe ? cell["se"] : cell["t"];
      if (!parenVal.is_null()) seLine += "(" + fmtB(parenVal, bFmt) + ")";
    }
    csv += "\n" + bLine + "\n" + seLine;
  }
  for (const auto& f : footers) {
    csv += "\n" + f["label"].get<std::string>();
    for (const auto& v : f["values"]) {
      csv += ",";
      if (!v.is_null()) csv += v.get<std::string>();
    }
  }

  ojson out;
  out["type"] = "estimates_table";
  ojson namesJ = ojson::array();
  for (const auto& n : modelNames) namesJ.push_back(n);
  out["names"] = namesJ;
  out["coef_rows"] = coefRows;
  out["footers"] = footers;
  out["csv"] = csv;
  ojson options;
  options["se"] = showSe;
  options["nostar"] = nostar;
  options["b_fmt"] = bFmt;
  out["options"] = options;
  return out;
}

// ===========================================================================
// coefplot

namespace {

const double kZ975Cp = 1.959963984540054;

// _z_for_level: Abramowitz & Stegun 26.2.17 normal-inverse-CDF approximation.
double zForLevel(int level) {
  double p = (100 - level) / 200.0;
  if (p <= 0) return 3.5;
  double t = std::sqrt(-2.0 * std::log(p));
  const double c0 = 2.515517, c1 = 0.802853, c2 = 0.010328;
  const double d1 = 1.432788, d2 = 0.189269, d3 = 0.001308;
  double num = c0 + c1 * t + c2 * t * t;
  double den = 1 + d1 * t + d2 * t * t + d3 * t * t * t;
  return t - num / den;
}

ojson coeftableFromBlock(const ojson& estBlock) {
  if (estBlock.contains("coeftable")) return estBlock["coeftable"];
  if (estBlock.contains("outcomes") && estBlock["outcomes"].is_array() &&
      !estBlock["outcomes"].empty()) {
    return estBlock["outcomes"][0].value("coeftable", ojson::array());
  }
  return ojson::array();
}

// _read_store: session.estimates (primary) -> session.lastModel() "last"
// fallback (single-model pseudo-store built from the cached raw result).
std::vector<std::pair<std::string, ojson>> readStore(SessionState& session) {
  std::vector<std::pair<std::string, ojson>> out;
  if (!session.estimates.empty()) {
    for (const auto& [k, v] : session.estimates) out.emplace_back(k, v);
    return out;
  }
  const auto& lmOpt = session.lastModel();
  if (!lmOpt.has_value()) return out;
  if (lmOpt->result.has_value() && lmOpt->result->contains("coeftable")) {
    ojson snap;
    snap["coeftable"] = (*lmOpt->result)["coeftable"];
    snap["depvar"] = lmOpt->depvar;
    snap["estimator"] = lmOpt->cobraCmd;
    snap["n_obs"] = lmOpt->result->value("n_obs", ojson(0));
    out.emplace_back("last", snap);
    return out;
  }
  ojson snap;
  snap["coeftable"] = ojson::array();
  snap["depvar"] = lmOpt->depvar;
  snap["estimator"] = lmOpt->cobraCmd;
  snap["n_obs"] = 0;
  out.emplace_back("last", snap);
  return out;
}

}  // namespace

ojson runCoefplot(const std::vector<std::string>& names,
                  const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                  SessionState& session) {
  auto store = readStore(session);
  if (store.empty()) {
    return engineErrorBlockEt(
        "coefplot: no stored estimates. "
        "Run a regression first, or use `eststo name` to store results.");
  }

  std::vector<std::pair<std::string, ojson>> selected;
  // W2 esttabnames (BUG-552): coefplot's model list is a namelist like every
  // other stored-estimates consumer -- `coefplot A_*` / `.` expand through
  // the one shared expansion, in store order. The loop below then looks up
  // exact names (a "." is the active results, built from e()).
  std::vector<std::string> expandedNames;
  if (!names.empty() && !session.estimates.empty()) {
    SessionState::EstNamelist ex = session.expandEstimatesNamelist(names);
    if (ex.rc != 0) return estNamelistError(ex);
    for (const auto& n : ex.names) {
      if (n != ".") {
        expandedNames.push_back(n);
        continue;
      }
      if (!session.lastEstBlock.has_value()) {
        return engineErrorBlockEt(
            "coefplot: the active estimation results (.) carry no coefficient table this "
            "version of PIET can plot; store them first (`estimates store NAME`)");
      }
      store.emplace_back(".", makeSnapshot(*session.lastEstBlock));
      expandedNames.push_back(".");
    }
  } else {
    expandedNames = names;
  }
  if (!names.empty()) {
    for (const auto& nm : expandedNames) {
      bool found = false;
      for (const auto& [k, v] : store) {
        if (k == nm) {
          selected.emplace_back(k, v);
          found = true;
          break;
        }
      }
      if (!found) {
        std::string avail;
        for (size_t i = 0; i < store.size(); ++i) {
          if (i) avail += ", ";
          avail += store[i].first;
        }
        return engineErrorBlockEt("coefplot: model `" + nm +
                                  "` not found in estimates store. Available: " + avail);
      }
    }
  } else {
    selected = store;
  }
  if (selected.empty()) return engineErrorBlockEt("coefplot: no models to plot.");

  int level = 95;
  for (const auto& [k, v] : opts) {
    if (k == "level" && !v.isBool) {
      try {
        int lv = std::stoi(v.strValue);
        if (lv < 1 || lv > 99) {
          return engineErrorBlockEt("coefplot: level(#) must be between 1 and 99.", "syntax_error");
        }
        level = lv;
      } catch (...) {
        return engineErrorBlockEt("coefplot: level(#) must be an integer (e.g. level(90)).",
                                  "syntax_error");
      }
    }
  }
  bool needsRecompute = (level != 95);
  double zAlpha = needsRecompute ? zForLevel(level) : kZ975Cp;

  std::set<std::string> dropSet, keepSet;
  for (const auto& [k, v] : opts) {
    if (k == "drop" && !v.isBool) {
      std::istringstream iss(v.strValue);
      std::string tok;
      while (iss >> tok) dropSet.insert(tok);
    }
    if (k == "keep" && !v.isBool) {
      std::istringstream iss(v.strValue);
      std::string tok;
      while (iss >> tok) keepSet.insert(tok);
    }
  }

  std::optional<double> xline;
  for (const auto& [k, v] : opts) {
    if (k == "xline") {
      if (!v.isBool) {
        try {
          xline = std::stod(v.strValue);
        } catch (...) {
          return engineErrorBlockEt("coefplot: xline(#) must be numeric.", "syntax_error");
        }
      } else if (v.boolValue) {
        xline = 0.0;
      }
    }
  }

  ojson modelsOut = ojson::array();
  std::vector<double> allValues;
  bool anyCoefs = false;

  for (const auto& [modelName, estBlock] : selected) {
    ojson ct = coeftableFromBlock(estBlock);
    if (ct.empty()) {
      ojson m;
      m["name"] = modelName;
      m["coefs"] = ojson::array();
      modelsOut.push_back(m);
      continue;
    }
    ojson coefsOut = ojson::array();
    for (const auto& row : ct) {
      std::string name = row.value("name", std::string());
      if (dropSet.count(name)) continue;
      if (!keepSet.empty() && !keepSet.count(name)) continue;
      if (!row.contains("b") || row["b"].is_null()) continue;
      double bF;
      try {
        bF = row["b"].get<double>();
      } catch (...) {
        continue;
      }

      double ciLo = std::nan(""), ciHi = std::nan("");
      if (needsRecompute) {
        if (row.contains("se") && !row["se"].is_null()) {
          try {
            double seF = row["se"].get<double>();
            ciLo = bF - zAlpha * seF;
            ciHi = bF + zAlpha * seF;
          } catch (...) {
          }
        }
      } else {
        if (row.contains("ci_lo") && !row["ci_lo"].is_null()) {
          try {
            ciLo = row["ci_lo"].get<double>();
          } catch (...) {
          }
        }
        if (row.contains("ci_hi") && !row["ci_hi"].is_null()) {
          try {
            ciHi = row["ci_hi"].get<double>();
          } catch (...) {
          }
        }
      }

      ojson c;
      c["name"] = name;
      c["b"] = bF;
      c["ci_lo"] = std::isnan(ciLo) ? ojson(nullptr) : ojson(ciLo);
      c["ci_hi"] = std::isnan(ciHi) ? ojson(nullptr) : ojson(ciHi);
      coefsOut.push_back(c);
      anyCoefs = true;

      if (std::isfinite(bF)) allValues.push_back(bF);
      if (std::isfinite(ciLo)) allValues.push_back(ciLo);
      if (std::isfinite(ciHi)) allValues.push_back(ciHi);
    }
    ojson m;
    m["name"] = modelName;
    m["coefs"] = coefsOut;
    modelsOut.push_back(m);
  }

  if (!anyCoefs) {
    return engineErrorBlockEt("coefplot: no coefficients to plot after applying drop/keep filters.");
  }

  if (xline.has_value() && std::isfinite(*xline)) allValues.push_back(*xline);

  double xMin, xMax;
  if (!allValues.empty()) {
    xMin = *std::min_element(allValues.begin(), allValues.end());
    xMax = *std::max_element(allValues.begin(), allValues.end());
    double span = xMax - xMin;
    double pad = span > 0 ? span * 0.05 : 0.5;
    xMin -= pad;
    xMax += pad;
  } else {
    xMin = -1.0;
    xMax = 1.0;
  }

  std::vector<std::string> depvars;
  std::set<std::string> depvarSet;
  for (const auto& [k, v] : selected) {
    std::string dv = v.value("depvar", std::string());
    if (!dv.empty() && depvarSet.insert(dv).second) depvars.push_back(dv);
  }
  std::sort(depvars.begin(), depvars.end());
  std::string yLabelDefault;
  for (size_t i = 0; i < depvars.size(); ++i) {
    if (i) yLabelDefault += ", ";
    yLabelDefault += depvars[i];
  }
  if (yLabelDefault.empty()) yLabelDefault = "Coefficient";

  std::string titleDefault = depvars.empty() ? "Coefficient plot" : "Coefficient plot: " + yLabelDefault;

  std::string title = titleDefault, xLabel = "Coefficient estimate", yLabel = yLabelDefault;
  for (const auto& [k, v] : opts) {
    if (k == "title" && !v.isBool) title = v.strValue;
    if (k == "xtitle" && !v.isBool) xLabel = v.strValue;
    if (k == "ytitle" && !v.isBool) yLabel = v.strValue;
  }

  ojson block;
  block["type"] = "graph";
  block["kind"] = "coefplot";
  block["title"] = title;
  block["x_label"] = xLabel;
  block["y_label"] = yLabel;
  block["models"] = modelsOut;
  block["level"] = level;
  block["n_models"] = static_cast<int64_t>(modelsOut.size());
  block["x_min"] = xMin;
  block["x_max"] = xMax;
  if (xline.has_value()) block["xline"] = *xline;
  block["name"] = nullptr;  // filled by the dispatcher's name() capture (BS-011, mirrors BS-010)
  return block;
}

// ---------------------------------------------------------------------------
// `estimates table [NAME...] [, b se]` — Stata's own model-comparison table.
//
// Layout is pinned to real Stata 19.5 (P4 answerbook,
// internal capture p4_stata.log): a 13-character label
// field ending in the `|` rule, then 13 characters per model. The numeric cell
// is Stata's %9.0g, whose real rule — measured off that log rather than assumed
// — is "as many significant digits as fit in 9 characters once the leading zero
// of a |x|<1 value is dropped"; a minus sign is allowed to overflow that width
// (the log's -.19581818 is 10 characters). estFmt9 below implements exactly that.
namespace {

std::string estFmt9(double v) {
  if (std::isnan(v)) return ".";
  if (std::isinf(v)) return v > 0 ? "+Inf" : "-Inf";
  for (int prec = 9; prec >= 1; --prec) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*g", prec, v);
    std::string s(buf);
    // Stata prints .5, never 0.5 — and the dropped zero is what buys the digit
    // that makes the 9-character budget work out.
    if (s.rfind("0.", 0) == 0) s.erase(0, 1);
    else if (s.rfind("-0.", 0) == 0) s.erase(1, 1);
    const size_t visible = s.size() - (s[0] == '-' ? 1 : 0);
    if (visible <= 9) return s;
  }
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%g", v);
  return std::string(buf);
}

// Widths count CHARACTERS (StataBE pads a Unicode name by characters:
// scratch-r150/uni-v/p4.do), so a multi-byte name stays aligned.
std::string padLeft(const std::string& s, size_t w) {
  const size_t n = cecon::ident::codePointCount(s);
  return n >= w ? s : std::string(w - n, ' ') + s;
}

std::string padRight(const std::string& s, size_t w) {
  const size_t n = cecon::ident::codePointCount(s);
  return n >= w ? s : s + std::string(w - n, ' ');
}

std::string centerIn(const std::string& s, size_t w) {
  const size_t n = cecon::ident::codePointCount(s);
  if (n >= w) return s;
  // Odd leftovers go to the LEFT (W:P8: a 6-character model name in the
  // 11-wide header cell is `   p8_ols  `, not `  p8_ols   `). P4's names are
  // 5 characters, which splits evenly and so never discriminated the two
  // roundings — its header stays byte-identical either way.
  const size_t left = (w - n + 1) / 2;
  return std::string(left, ' ') + s + std::string(w - n - left, ' ');
}

// W:P8. A coefficient named `<level>.<var>` is one level of factor variable
// `<var>`; Stata's `estimates table` breaks those out under an indented
// variable header instead of listing `2.region` as a flat row name. Returns
// false for everything else, including interactions (`2.region#c.x1`), which
// stay ordinary rows rather than being half-grouped on a guess.
bool splitFactorLevel(const std::string& name, std::string& var, std::string& level) {
  const size_t dot = name.find('.');
  if (dot == std::string::npos || dot == 0 || dot + 1 >= name.size()) return false;
  for (size_t i = 0; i < dot; ++i) {
    if (!std::isdigit(static_cast<unsigned char>(name[i]))) return false;
  }
  const std::string rest = name.substr(dot + 1);
  // A variable name: bytes >= 0x80 are name bytes (core/ident.h), so `2.قيمة` splits.
  if (!cecon::ident::isIdentStartByte(rest[0])) return false;
  for (char c : rest) {
    if (!cecon::ident::isIdentByte(c)) return false;
  }
  var = rest;
  level = name.substr(0, dot);
  return true;
}

// W:P8. The equation label a model's rows sit under. Measured on StataBE 19.5
// rather than assumed: `regress`'s e(b) columns are bare (`mpg`, `_cons`) and
// print under `_`, while every ML single-equation estimator probed (logit,
// probit, poisson) names its equation after the dependent variable
// (`hi:mpg`, `hi:_cons`) and prints under `hi`. Two `regress` models with
// DIFFERENT depvars still share `_` and therefore still print flat — the
// discriminating case, checked explicitly, and the reason this keys on the
// estimator family rather than on the depvar alone.
std::string equationLabel(const ojson& snap) {
  std::string estimator = snap.value("estimator", std::string());
  for (auto& c : estimator) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (estimator == "feols" || estimator == "ols") return "_";
  const std::string depvar = snap.value("depvar", std::string());
  return depvar.empty() ? "_" : depvar;
}

}  // namespace

ojson runEstimatesTable(const std::vector<std::string>& modelNamesIn,
                        const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                        SessionState& session) {
  // W:P8. `se` ADDS the standard-error row to the coefficient; it does not
  // replace it — live-verified, `estimates table M1 M2, se` prints b and se
  // and the b/se legend exactly as `, b se` does. The previous reading
  // ("b or se") printed an SE-only table for `, se`, which is a wrong table
  // at rc 0. The coefficient is therefore always shown and `wantSe` is the
  // only real switch.
  bool wantSe = false;
  std::vector<std::string> statKeys;
  for (const auto& [k, v] : opts) {
    if (k == "b" && v.isBool && v.boolValue) continue;
    if (k == "se" && v.isBool && v.boolValue) { wantSe = true; continue; }
    if (k == "stats" && !v.isBool) {
      std::string cur;
      for (char c : v.strValue + " ") {
        if (std::isspace(static_cast<unsigned char>(c)) || c == ',') {
          if (!cur.empty()) { statKeys.push_back(cur); cur.clear(); }
        } else {
          cur += c;
        }
      }
      continue;
    }
    // B4 P1: `unsupported_option`, which eval::coarseRc maps to r(198) — the
    // code the standing rule reserves for an option this build will not take.
    // It defaulted to `engine_error`, i.e. rc 1, so `estimates table m1, st(N)`
    // — r(198) in StataBE (oracle_p.log:176-180) — refused under a code no
    // Stata command ever answers with, and `capture` saw 1 instead of 198.
    return engineErrorBlockEt("estimates table: option `" + k + "` is not supported in this "
                              "version of PIET (only `b`, `se` and `stats()` are wired)",
                              "unsupported_option");
  }
  for (const auto& s : statKeys) {
    if (s != "N" && s != "r2") {
      return engineErrorBlockEt("estimates table: stats(" + s + ") is not supported in this "
                                "version of PIET (only `N` and `r2` are wired)");
    }
  }

  // W2 esttabnames (BUG-552): a written namelist goes through the one
  // shared expansion: `A_*`, `_all`, `.`; a
  // token matching nothing is r(111) "estimation result NAME not found".
  // With no namelist Stata tables `.`, the active results alone, whatever
  // is stored (StataBE 19.5: `estimates store m1`, a second fit, `estimates table`
  // -> one "Active" column), and with no active results that is r(301) "last
  // estimates (.) not found" (BUG-1248; it was rc 1, "no stored estimates").
  SessionState::EstNamelist ex = session.expandEstimatesNamelist(
      modelNamesIn.empty() ? std::vector<std::string>{"."} : modelNamesIn);
  if (ex.rc != 0) return estNamelistError(ex);
  const std::vector<std::string> names = std::move(ex.names);
  // The active, unstored results (`.`): snapshotted locally, as a read must not
  // store anything. std::deque so the pointers taken below stay valid.
  std::deque<ojson> liveSnaps;
  if (names.empty()) {  // `_all` over an empty store
    return engineErrorBlockEt("estimates table: no stored estimates. Use `estimates store NAME` "
                              "after fitting a model.");
  }

  // Resolve every requested model FIRST: reporting a missing name only after
  // half a table has been printed would leave the user reading a partial
  // comparison as if it were complete.
  std::vector<const ojson*> models;
  for (const auto& n : names) {
    if (n == ".") {
      if (!session.lastEstBlock.has_value()) {
        return engineErrorBlockEt(
            "estimates table: the active estimation results (.) carry no coefficient table "
            "this version of PIET can render; store them first (`estimates store NAME`)");
      }
      liveSnaps.push_back(makeSnapshot(*session.lastEstBlock));
      models.push_back(&liveSnaps.back());
      continue;
    }
    const ojson* m = session.estimatesGet(n);
    if (m == nullptr) {
      std::string avail;
      for (const auto& a : session.estimatesNames()) { if (!avail.empty()) avail += ", "; avail += a; }
      return engineErrorBlockEt("estimates table: model `" + n + "` not found in estimates store. "
                                "Available: " + (avail.empty() ? std::string("(none)") : avail));
    }
    models.push_back(m);
  }

  // Each model's rows sit under its equation label; models sharing one label
  // share rows. When every model lands on the same label the table is flat and
  // no equation header is printed at all (P4's two-`regress` case, and the
  // reason that pin stays byte-unchanged through this rewrite).
  std::vector<std::string> modelEq(models.size());
  std::vector<std::string> eqOrder;
  for (size_t i = 0; i < models.size(); ++i) {
    modelEq[i] = equationLabel(*models[i]);
    if (std::find(eqOrder.begin(), eqOrder.end(), modelEq[i]) == eqOrder.end()) {
      eqOrder.push_back(modelEq[i]);
    }
  }
  const bool grouped = eqOrder.size() > 1;

  // Coefficient rows = union across models, in first-seen order (Stata keeps
  // the first model's order and appends anything new from later ones), kept
  // per equation so a grouped table never leaks one equation's names into
  // another's block.
  std::map<std::string, std::vector<std::string>> coefOrderByEq;
  std::vector<std::map<std::string, std::pair<double, double>>> cells(models.size());
  for (size_t i = 0; i < models.size(); ++i) {
    std::vector<std::string>& order = coefOrderByEq[modelEq[i]];
    for (const auto& row : models[i]->value("coeftable", ojson::array())) {
      const std::string nm = row.value("name", std::string());
      if (nm.empty()) continue;
      if (std::find(order.begin(), order.end(), nm) == order.end()) order.push_back(nm);
      const double b = row.contains("b") && !row.at("b").is_null() ? row.at("b").get<double>()
                                                                   : std::nan("");
      const double se = row.contains("se") && !row.at("se").is_null() ? row.at("se").get<double>()
                                                                      : std::nan("");
      cells[i][nm] = {b, se};
    }
  }

  // Stata puts the intercept LAST, whatever the engine's own coeftable order is
  // (PIET's leads with it). Reordering here rather than at the producer keeps
  // every other consumer of that coeftable byte-unchanged.
  for (auto& [eq, order] : coefOrderByEq) {
    (void)eq;
    auto isCons = [](const std::string& s) { return s == "_cons" || s == "(intercept)"; };
    std::stable_partition(order.begin(), order.end(),
                          [&](const std::string& s) { return !isCons(s); });
  }

  const size_t labelW = 13;   // 12-wide right-aligned name + one space, then '|'
  const size_t cellW = 13;    // 11-wide right-aligned value + two spaces
  const size_t totalW = labelW + 1 + cellW * models.size();  // +1 = the '|' rule itself
  const std::string ruleFull(totalW, '-');
  const std::string ruleSplit = std::string(labelW, '-') + "+" +
                                std::string(totalW - labelW - 1, '-');
  // A structural row (equation header, factor header, in-group blank) stops at
  // the '|' with no trailing cell padding — measured, not stylistic.
  const std::string blankLabelRow = std::string(labelW, ' ') + "|";

  std::vector<std::string> lines;
  lines.push_back(ruleFull);
  {
    std::string h = padLeft("Variable", labelW - 1) + " |";
    // The unstored active results head their column "Active" (StataBE 19.5,
    // `estimates table` after a fit).
    for (const auto& n : names)
      h += centerIn(n == "." ? std::string("Active") : n, cellW - 2) + "  ";
    lines.push_back(h);
  }
  lines.push_back(ruleSplit);

  // One coefficient's b (and optionally se) row, cells blank for any model
  // that never estimated it.
  auto emitCoefRows = [&](const std::string& cn, const std::string& eq,
                          const std::string& labelCell) {
    std::string bRow = labelCell;
    std::string seRow = blankLabelRow.substr(0, labelW - 1) + " |";
    for (size_t i = 0; i < models.size(); ++i) {
      auto it = cells[i].find(cn);
      const bool present = it != cells[i].end() && modelEq[i] == eq;
      // A model that never estimated this coefficient gets a BLANK cell, not 0.
      bRow += present ? padLeft(estFmt9(it->second.first), cellW - 2) + "  "
                      : std::string(cellW, ' ');
      seRow += present ? padLeft(estFmt9(it->second.second), cellW - 2) + "  "
                       : std::string(cellW, ' ');
    }
    lines.push_back(bRow);
    if (wantSe) lines.push_back(seRow);
  };

  for (size_t g = 0; g < eqOrder.size(); ++g) {
    const std::string& eq = eqOrder[g];
    if (grouped) {
      if (g > 0) lines.push_back(ruleSplit);
      lines.push_back(padRight(eq, labelW) + "|");
    }
    const std::vector<std::string>& order = coefOrderByEq[eq];
    std::string openFactor;
    for (size_t r = 0; r < order.size(); ++r) {
      const std::string& cn = order[r];
      std::string fvar, flevel;
      const bool isLevel = splitFactorLevel(cn, fvar, flevel);
      if (isLevel && fvar != openFactor) {
        lines.push_back(blankLabelRow);
        lines.push_back(padLeft(fvar, labelW - 1) + " |");
        openFactor = fvar;
      } else if (!isLevel && !openFactor.empty()) {
        lines.push_back(blankLabelRow);
        openFactor.clear();
      }
      // A level row indents the level under its variable header; every other
      // row right-aligns its own name in the label field.
      const std::string labelCell = isLevel ? padLeft(flevel, labelW - 2) + "  |"
                                            : padLeft(cn, labelW - 1) + " |";
      emitCoefRows(cn, eq, labelCell);
    }
  }

  if (!statKeys.empty()) {
    lines.push_back(ruleSplit);
    lines.push_back(padRight("Statistics", labelW) + "|" +
                    std::string(cellW * models.size(), ' '));
    for (const auto& s : statKeys) {
      std::string row = padLeft(s, labelW - 1) + " |";
      for (size_t i = 0; i < models.size(); ++i) {
        const ojson& snap = *models[i];
        const char* key = (s == "N") ? "n_obs" : "r2";
        const bool present = snap.contains(key) && snap.at(key).is_number();
        row += present ? padLeft(estFmt9(snap.at(key).get<double>()), cellW - 2) + "  "
                       : std::string(cellW, ' ');
      }
      lines.push_back(row);
    }
  }

  lines.push_back(ruleFull);
  // The legend names the per-cell statistics, so it appears only when there is
  // more than one of them — `, b` alone prints no legend (live-verified).
  if (wantSe) lines.push_back(padLeft("Legend: b/se", totalW));

  std::string text;
  for (size_t i = 0; i < lines.size(); ++i) {
    if (i) text += "\n";
    text += lines[i];
  }
  ojson out;
  out["type"] = "display";
  out["text"] = text;
  return out;
}

}  // namespace piet::handlers
