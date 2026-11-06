// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// R2 (giant-file split, 2026-09-01): continuation TU of handlers_data_verbs.cpp
// — a MOVE-ONLY cut along pre-existing anonymous-namespace boundaries. The
// preamble is the original file head verbatim; shared helpers come from
// handlers_data_verbs_internal.h.
// M3b: MI-066 data verbs (merge/reshape/append/duplicates/gsort/order/
// recode) via cobra 0.18.0's native CLI verbs. C++ ports of the
// corresponding piet/backend/piet/{merge_op,reshape,append,duplicates,
// gsort,order,recode}.py oracle modules — bug-for-bug where cobra 0.18.0
// and the Python backend agree; where they differ, the FROZEN 1.16.0
// corpus wins (this file pre-validates the oracle's own error conditions
// in C++ BEFORE shelling out to cobra, so error text matches byte-for-byte
// even where cobra's native error text differs — see each handler's notes).
#include "piet/handlers.h"
#include "piet/translate.h"

#include <sys/stat.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/clonefile.h>  // BUG-138/L5-IOSAVE: O(1) same-volume save (runSaveData)
#endif
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>  // MoveFileExW / GetLastError (runSaveData's atomic replace)
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <charconv>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <system_error>
#include <unordered_set>
#include <vector>

#include <iconv.h>   // AER scope-2 W3a: `import delimited, encoding()`

#include "piet/cobra_runner.h"
#include "piet/dispatcher.h"  // dispatch(): xtile/cumul evaluate exp with the user's generate
#include "piet/counters.h"
#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"  // gFormat/applyStataFmt (BS-003 tostring default + usedisplayformat rendering)
#include "piet/eval/pyvalue.h"
#include "piet/display_hardening.h"  // resolveVarName: xtile cutpoints()
#include "piet/delimited_io.h"  // import delimited: the one native reader
#include "piet/inrange_filter.h"
#include "piet/mutation_result.h"
#include "piet/strutil.h"
#include "data/missing.h"
#include "xlsx.h"   // Stata `.` / `.a`-`.z` kinds — CSV writes the extended code
#include "piet/scratch_seq.h"
#include "io/win_compat.h"  // cecon::native_exists / to_native_path (BUG-050 wide open)

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
#include "handlers_data_verbs_internal.h"
#include "estimation_m4_internal.h"  // e(sample) T2: the copy verbs' mask helpers
#include "scratch_columns.h"          // xtile's sample, source and weight columns

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
using namespace dv_detail;

namespace {
// R2: re-binding of the option vocabularies this TU consumes. Registering the
// same token twice UNIONS the sets (documented, deliberate — errors.h), and a
// set unioned with itself is itself, so the single-registry law (BUG-233)
// holds with three TUs exactly as it did with one.
const piet::eval::OptVocab& kVocabTsfill = piet::eval::registerOptVocab(
    {"tsfill"}, {{"full"},
     {}});
}  // namespace

namespace {

// Resolve joinby's `using` file to a locally-readable hbin path. The engine's
// `cobra joinby` verb takes .hbin only (and PIET's own pre-validation reads
// the using file via Datastore), unlike
// append (which only needs cobra's OWN --using argument — cmd_append
// auto-detects .dta/.csv internally) or merge (which pre-converts ONLY for
// its own pre-validation peek, then hands cobra the original path). Mirrors
// runMerge's using-file resolution above, kept as a SEPARATE copy per this
// codebase's established per-TU/per-callsite convention (see errorFromRaw's
// own header note above) rather than refactoring runMerge to share it.
struct JoinbyUsingResult {
  bool ok = false;
  std::string hbinPath;
  std::string usingPath;  // the resolved file (a checkpoint's metadata is keyed by it)
  std::string metaPath;
  ojson errBlock;
};

std::string stripOneQuotePair(const std::string& v);

JoinbyUsingResult resolveJoinbyUsing(SessionState& session, const std::string& usingPathArg) {
  JoinbyUsingResult r;
  // cross hands the file over still quoted; a message names it without them
  // (`cross using "nof"`: StataBE `file nof.dta not found`).
  std::string usingShown = stripOneQuotePair(trim(usingPathArg));
  std::string usingPath = resolveUsingDataset(session, usingPathArg, &usingShown);
  r.usingPath = usingPath;
  if (!fileExists(usingPath)) {
    // `use using` finds no file: r(601) (w2aw_data_combine_family.log dc_j_nofile),
    // naming the NAME.dta a bare stem stood for.
    r.errBlock = ojson(eval::errorBlock("engine_error", "file " + usingShown + " not found",
                                        std::nullopt, std::nullopt, std::nullopt, "r(601)"));
    return r;
  }
  std::string ext = extOf(usingPath);
  if (ext == ".dta" || usingFileIsDta(usingPath)) {
    std::string conv = nextPath(session.workdir(), "joinby_using_dta");
    r.metaPath = conv + ".meta.json";
    registerScratchMint(r.metaPath);
    CobraRunResult cr = runCobra({"import-dta", usingPath, conv, "--out-meta", r.metaPath});
    if (!cr.ok) {
      CobraError ce = errorFromRaw(cr);
      r.errBlock =
          engineErrorBlock("joinby using: cannot convert '" + usingPath + "': " + ce.what());
      return r;
    }
    r.hbinPath = conv;
  } else if (ext == ".csv" || ext == ".tsv") {
    std::string conv = nextPath(session.workdir(), "joinby_using_csv");
    char delim = (ext == ".tsv") ? '\t' : ',';
    CobraRunResult cr = runCobra(
        {"convert", "--data", usingPath, "--out", conv, "--delim", std::string(1, delim)});
    if (!cr.ok) {
      r.errBlock = delimitedUsingRefused(usingShown, 610);
      return r;
    }
    r.hbinPath = conv;
  } else {
    r.hbinPath = usingPath;  // already .hbin
  }
  r.ok = true;
  return r;
}

// The variables only the using file has keep what it states about them -- its
// variable label, format, storage type and value label (dc_j_base_v5 uval
// `float|%7.1f|[u val]`; BUG-1157: joinby and cross of a file whose int u
// carries ul list u through ul, StataBE).
void adoptUsingOwnVars(SessionState& session, const JoinbyUsingResult& ur,
                       const std::map<std::string, UsingVarMeta>& uMeta,
                       const std::vector<std::string>& usingOnly) {
  for (const auto& nm : usingOnly) {
    auto it = uMeta.find(nm);
    if (it == uMeta.end()) continue;
    const UsingVarMeta& u = it->second;
    if (!u.label.empty()) session.setVarLabel(nm, u.label);
    if (!u.format.empty()) {
      VarMeta& vm = session.varMeta[nm];
      if (!vm.format.has_value() || vm.format->empty()) vm.format = u.format;
    }
    if (u.stataType.has_value()) {
      session.implicitFloatVars.erase(nm);
      session.varMeta[nm].stataType = *u.stataType;
    }
  }
  adoptUsingValueLabels(session, ur.usingPath, ur.metaPath, usingOnly, true);
}

}  // namespace

ojson runJoinby(const std::vector<std::string>& keylist, const std::string& usingPathArg,
                SessionState& session, const std::string& unmatched,
                const std::optional<std::string>& mergeVar) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  // INSPECT-2608: everything this function asks of the MASTER frame is metadata
  // — does a key exist, what are the non-key names, how many rows — and
  // `session.datastore()` was answering it with a full open of the frame. The
  // engine reads the file itself for the actual join, so that open was pure
  // overhead: measured on the acceptance corpus's controls=yes arm, joinby's
  // 34.2 s carried 19.8 s of engine and 5.5 GB of PIET-side column reads for
  // questions the resident shadow card already holds. Same idiom, and the same
  // safety rule, as runSort's varlist check (handlers_m3.cpp): the card answers
  // only when it provably describes the current head, and a nullptr falls back
  // to the exact prior behaviour.
  const std::vector<VarInfo>* card = session.shadowVarlistIfCurrent();
  std::optional<int64_t> cardRows = session.residentRowsIfCurrent();
  Datastore* master = nullptr;
  if (!card || !cardRows.has_value()) {
    master = session.datastore();
    if (master == nullptr) return eval::noDataLoaded();
  }
  auto masterHas = [&](const std::string& nm) {
    if (master) return master->has(nm);
    for (const auto& info : *card)
      if (info.name == nm) return true;
    return false;
  };
  auto masterNames = [&]() {
    std::vector<std::string> out;
    if (master) return master->names();
    out.reserve(card->size());
    for (const auto& info : *card) out.push_back(info.name);
    return out;
  };
  if (keylist.empty()) return engineErrorBlock("joinby: key variable list required");
  for (const auto& k : keylist) {
    if (!masterHas(k)) return engineErrorBlock("joinby: variable '" + k + "' not found");
  }

  // joinby: the marker is `_merge` unless _merge() names it, and
  // exists only when unmatched() is not none or _merge() is given; a name in
  // use is `confirm new var` r(110) (dc_j_merge_exists).
  std::string marker;
  if (mergeVar.has_value()) marker = *mergeVar;
  else if (unmatched != "none") marker = "_merge";
  if (!marker.empty() && masterHas(marker))
    return ojson(eval::errorBlock("engine_error", "variable " + marker + " already defined",
                                  std::nullopt, std::nullopt, std::nullopt, "r(110)"));

  JoinbyUsingResult ur = resolveJoinbyUsing(session, usingPathArg);
  if (!ur.ok) return ur.errBlock;
  Datastore usingDs(ur.hbinPath);
  // What the using file states about its variables (labels, formats, str#),
  // adopted by the variables the join brings in (dc_j_base_v5 uval
  // `float|%7.1f|[u val]`).
  const std::map<std::string, UsingVarMeta> uMeta =
      usingVarMetaOf(session, ur.usingPath, ur.metaPath, usingDs);

  for (const auto& k : keylist) {
    if (!usingDs.has(k)) {
      return engineErrorBlock("joinby: key variable '" + k + "' not found in using dataset");
    }
  }

  // Column-name conflict check (non-key columns present on both sides) —
  // mirrors merge's own established rule: this build performs a bare inner
  // key-join only (D5), with no update/replace value-reconciliation, so an
  // overlapping non-key name is ambiguous and rejected honestly rather than
  // silently picking a side.
  std::set<std::string> keySet(keylist.begin(), keylist.end());
  std::set<std::string> masterNonKeys;
  const std::vector<std::string> masterNameList = masterNames();
  for (const auto& nm : masterNameList) {
    if (!keySet.count(nm)) masterNonKeys.insert(nm);
  }
  std::vector<std::string> overlap;
  for (const auto& nm : usingDs.names()) {
    if (keySet.count(nm)) continue;
    if (masterNonKeys.count(nm)) overlap.push_back(nm);
  }
  if (!overlap.empty()) {
    std::sort(overlap.begin(), overlap.end());
    std::string listStr = "[";
    for (size_t i = 0; i < overlap.size(); ++i) {
      if (i) listStr += ", ";
      listStr += "'" + overlap[i] + "'";
    }
    listStr += "]";
    // Master wins a shared non-key name ([D] joinby's default without update/
    // replace) — oracle-pinned on the P1 wave fixture. Not an error; the
    // engine drops the using's copy and the report below must match.
    (void)listStr;
  }

  int64_t nMaster = master ? master->nRows() : *cardRows;
  int64_t nUsing = usingDs.nRows();

  std::vector<std::string> usingExtra;  // using's non-key columns, appended
  for (const auto& nm : usingDs.names()) {
    // master wins a shared name, so it is NOT a newly added column
    if (!keySet.count(nm) && !masterHas(nm)) usingExtra.push_back(nm);
  }

  // One engine call (`cobra joinby`, L1b) replaces the old in-C++ within-key
  // cross-product + whole-frame CSV + `cobra convert` round-trip. The engine
  // verb implements the same inner unmatched(none) join: factor keys matched
  // by LEVEL STRING via a unified table (each hbin's private level table can
  // number the same string differently); unmatched keys DROPPED both sides;
  // output = master columns + using non-key columns, master-row-order
  // deterministic. One accepted+logged divergence: ±0.0 f64 keys now MATCH
  // (the engine-wide make_key ±0-normalize convention) where the old
  // text-repr keyer treated them as distinct.
  std::string joinedKeys;
  for (size_t i = 0; i < keylist.size(); ++i) {
    if (i) joinedKeys += ",";
    joinedKeys += keylist[i];
  }
  std::string outPath = nextPath(session.workdir(), "joinby");
  CobraRunResult cr;
  {
    // GA sweep leg ②/④, 2026-08-22. joinby was the single largest engine
    // coordinate in the census of record — 452.6 s, 27.3% of engine wall, of
    // which the phase probe put 363.6 s (77%) in the OUTPUT WRITE, at 330.9 MB
    // of scratch per call and 52.94 GB across one 47-task run. The kernel is
    // 106.9 s; the join itself was never the cost.
    //
    // The cost is that this call was leaving the router by the stateless leg,
    // so one joinby moved the whole frame across disk THREE times: the master
    // head had to be materialized to a real file to be named as `--data`, the
    // engine wrote the full result to `--out`, and `session.load(outPath)` read
    // all of it back. The resident plane exists precisely to delete all three —
    // `acquire_data("")` hands back the session's zero-copy view and
    // `commit_frame(..., "")` stages instead of writing (apps/cmds/common.cpp),
    // and both `data_missing`/`out_missing` already tolerate the empty argument
    // in resident mode. Nothing in the engine needed changing.
    //
    // ResidentEligible asserts exactly the invariant that holds here, the same
    // one rename/gen/replace/egen/drop/keep assert: this `--out` BECOMES the
    // head and is never read back as a file. `session.load(outPath)` below then
    // records the virtual chain rather than opening anything, and
    // mutationResult renders from the resulting card. joinby is safe to opt in
    // where `merge` is not: PIET never parses joinby's stderr (it checks
    // `cr.ok` and the card, never the "wrote N rows" notice), so no status
    // count depends on the one-shot leg being taken.
    SessionState::ResidentEligible residentJoinby(session);
    std::vector<std::string> jargs = {"joinby", "--data",
                                      master ? master->path() : *session.currentHbin(),
                                      "--using", ur.hbinPath, "--keys", joinedKeys,
                                      "--out", outPath};
    if (unmatched != "none") {
      jargs.push_back("--unmatched");
      jargs.push_back(unmatched);
    }
    if (!marker.empty()) {
      jargs.push_back("--merge-var");
      jargs.push_back(marker);
    }
    cr = runCobra(jargs);
  }
  if (!cr.ok) {
    CobraError ce = errorFromRaw(cr);
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  // The disk-full guard still applies to the STATELESS leg, where `--out` is a
  // real file and a silent short write is exactly the BUG-350 failure mode. On
  // the resident leg no file is written by design, so the check would fire on
  // every success — it is therefore asked only when a file was expected.
  if (!session.lastRunWasResident() && !cecon::native_exists(outPath))
    return engineErrorBlock("joinby: failed writing output dataset (disk full or I/O error)");

  session.load(outPath);
  // e(sample) T2: Stata keeps the marker on ONE copy of each matched master row
  // (w2_esample_copies.log:470 X4 `0|19|72|m0=38|m0_diff=19|`); which copy
  // follows its own sort tie order (X4b `not_first_of_run=8`), and PIET keeps
  // the first of each master row's run (handlers.cpp esampleKeepFirstCopy). A
  // frame whose keys cannot be read withdraws the marker by name.
  const std::map<std::string, int64_t> runLen = esampleUsingRunLengths(usingDs, keylist);
  if (!esampleKeepFirstCopy(session, keylist, &runLen) &&
      !esampleMarkHeadLost(session, EsampleLostBy::kJoinby)) {
    return engineErrorBlock(
        "joinby: the data were joined, but the estimation-sample marker could not be "
        "withdrawn from the result; e(sample) is not reliable until the next estimation.");
  }
  adoptUsingOwnVars(session, ur, uMeta, usingExtra);
  // The marker (joinby.ado DefLabel): a byte with value label
  // __MERGE (1-3 and the 9999 "signature" entry), no variable label, displayed
  // as wide as "both in master and using data" (dc_j_both_v5 `byte|%29.0g|[]`).
  if (!marker.empty()) {
    session.implicitFloatVars.erase(marker);
    VarMeta& vm = session.varMeta[marker];
    vm.stataType = std::string("byte");
    vm.format = std::string("%29.0g");
    vm.label.reset();
    session.labelDefs["__MERGE"] = {{"1", "only in master data"},
                                    {"2", "only in using data"},
                                    {"3", "both in master and using data"},
                                    {"9999", "signature"}};
    session.varLblname[marker] = "__MERGE";
    session.valueLabels[marker] = {{1, "only in master data"},
                                   {2, "only in using data"},
                                   {3, "both in master and using data"},
                                   {9999, "signature"}};
  }
  ojson result = mutationResult(session, "joinby", usingExtra);
  result["n_master"] = nMaster;
  result["n_using"] = nUsing;
  return result;
}

// ---------------------------------------------------------------------------
// tsfill (BS-081, BETA campaign Phase D batch D4): `tsfill [, full]` -> cobra
// tsfill --timevar T [--panelvar ID] [--delta D] --out new.hbin. Requires a
// prior `tsset T` (or `xtset ID T`) — reads session.timevar/panelvar/delta,
// the SAME tsset/xtset spine BS-075's xtpoisson/xtlogit read
// session.panelvar from (estimation_m4.cpp's runXtPoissonLogit) and this
// batch's own newey/prais/dfuller/pperron read session.timevar from
// (estimation_m4.cpp, same file as those D4 estimators — this one lives
// here instead because it is a data-mutation verb like gsort/order/recode
// above, not an estimation command). Oracle D4f
// (internal capture d45-oracle-stata.log):
// after `drop if mod(t,7)==0` (48 of 336 rows dropped — INCLUDING t=336
// itself, since 336 is itself a multiple of 7), `tsfill` restores the full
// grid from the SURVIVING min/max t, i.e. 1..335 (not 1..336 — tsfill only
// fills gaps WITHIN the observed range, it never extrapolates past the
// highest t still present) — live-verified byte-for-byte: 335 rows out,
// matching the oracle's own `count` = 335 exactly, with every OTHER column
// (income, etc.) missing/NaN on the newly-filled rows (also live-verified).
// BUG-897: `full` (every panel over the overall min..max of the
// time variable, on the delta grid) is cobra tsfill --full; it was refused by
// name before the engine had it. tsfill's own refusals carry its rc
// (w2w_tsset_tsfill.log rf_tsfill_none r(111), rf_tsfill_if r(101),
// rf_tsfill_full_tmiss r(451)); the registry's allowIf=false row is
// descriptive only, so an `if` reached this arm and was silently ignored.
ojson runTsfill(const eval::ParsedCommand& parsed, SessionState& session) {
  auto refuse = [](const std::string& msg, int rc) {
    return ojson(eval::errorBlock("engine_error", msg, std::nullopt, std::nullopt, std::nullopt,
                                  "r(" + std::to_string(rc) + ")"));
  };
  // `syntax [, Full]` runs first.
  if (!parsed.varlist.empty()) return refuse("varlist not allowed", 101);
  if (parsed.ifExpr.has_value()) return refuse("if not allowed", 101);
  if (parsed.inRange.has_value()) return refuse("in range not allowed", 101);
  if (parsed.weightType.has_value()) return refuse("weights not allowed", 101);
  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file).
  const std::unordered_set<std::string>& kKnown = kVocabTsfill.known;
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kKnown)) {
    return *err;
  }
  bool full = false;
  for (const auto& [k, v] : parsed.opts)
    if (k == "full" && v.isBool && v.boolValue) full = true;
  if (!session.currentHbin().has_value() || session.datastore() == nullptr) {
    return eval::noDataLoaded();
  }
  // tsfill declares no further options here.
  if (!session.timevar.has_value() || session.timevar->empty()) {
    return refuse("time variable not set, use tsset varname ...", 111);
  }
  if (full) {
    Datastore* ds = session.datastore();
    if (ds->has(*session.timevar) && ds->dtype(*session.timevar) == "f64") {
      const double* tv = ds->col(*session.timevar).f64();
      const int64_t n = ds->nRows();
      for (int64_t i = 0; i < n; ++i)
        if (std::isnan(tv[i]))
          return refuse("timevar (" + *session.timevar +
                            ") may not contain missing values when option full is specified",
                        451);
    }
  }

  std::vector<std::string> argv = {"tsfill", "--data", *session.currentHbin(), "--timevar",
                                    *session.timevar};
  if (session.panelvar.has_value() && !session.panelvar->empty()) {
    argv.push_back("--panelvar");
    argv.push_back(*session.panelvar);
  }
  if (session.delta.has_value()) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.17g", *session.delta);
    argv.push_back("--delta");
    argv.push_back(buf);
  }
  if (full) argv.push_back("--full");
  std::string outPath = nextPath(session.workdir(), "tsfill");
  argv.push_back("--out");
  argv.push_back(outPath);

  CobraRunResult cr;
  {
    // BUG-394 (2026-09-02): onto the resident plane. tsfill was one of eight verbs
    // still writing a whole --out file per call with no note saying why; its engine
    // command already uses the shared plane (acquire_data / commit_frame), so the
    // scope is all it needed.
    SessionState::ResidentEligible residentTsfill(session);
    cr = runCobra(argv);
  }
  if (!cr.ok) {
    CobraError ce = errorFromRaw(cr);
    // tsfill.ado's own check after a declaration the data no longer satisfies
    // (rows appended since `tsset`): r(451), as tsset's.
    if (cobraErrorStr(ce).find("repeated time value") != std::string::npos)
      return refuse(session.panelvar.has_value() && !session.panelvar->empty()
                        ? "repeated time values within panel"
                        : "repeated time values in sample",
                    451);
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  session.load(outPath);
  return mutationResult(session, "tsfill");
}

// ---------------------------------------------------------------------------
// BUG-370: xtile / ipolate / tssmooth ma.
//
// All three were named as residuals by the 08-31 wiring audit and share one
// shape: the ENGINE half already ships and is already Stata-parity tested
// (`cobra eval --xtile/--ipolate/--tsma`; tests/unit/test_xtipo.cpp and
// test_tsma.cpp), and PIET simply had no command table row pointing at it. So
// these are not new estimators — they are the missing user-facing door on a room
// that was already furnished. Each handler is argument validation plus one
// engine call.
//
// What each one REFUSES matters as much as what it does, because Stata's option
// surface is wider than the engine's here and a silently-dropped option is a
// wrong answer wearing a right one's clothes:
//   xtile     — cutpoints()/altdef/weights have no engine equivalent at all
//   ipolate   — by() exists engine-side but PIET's by-prefix plumbing does not
//   tssmooth  — only `ma`; and only the full window(#l #c #f) spelling
namespace {

// Stata's xtile defaults to nquantiles(2); the engine's --xtile flag defaults to
// 4 when the option is absent. Those disagree, so the handler ALWAYS sends an
// explicit nq and never lets the engine's default show through. A missing
// `nq(...)` here would be a silently wrong answer, not an error.
constexpr int kXtileDefaultNq = 2;

bool parseIntOptDV(const std::string& text, int& out) {
  if (text.empty()) return false;
  size_t pos = 0;
  long v = 0;
  try {
    v = std::stol(text, &pos);
  } catch (...) {
    return false;
  }
  while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) ++pos;
  if (pos != text.size()) return false;
  out = static_cast<int>(v);
  return true;
}

// Does the live shadow card (or, failing that, the datastore) already carry this
// name? Used to refuse overwriting an existing column, matching Stata's r(110).
bool columnExistsDV(SessionState& session, const std::string& name) {
  if (const auto* card = session.shadowVarlistIfCurrent()) {
    for (const auto& info : *card)
      if (info.name == name) return true;
    return false;
  }
  Datastore* ds = session.datastore();
  return ds != nullptr && ds->has(name);
}

// A refused new-variable name under Stata's own code (strutil::NameRefusal).
ojson nameRefusalBlock(const strutil::NameRefusal& r) {
  return ojson(eval::errorBlock("syntax_error", r.message, std::nullopt, std::nullopt,
                                std::nullopt, "r(" + std::to_string(r.rc) + ")"));
}

ojson newVarGuardDV(SessionState& session, const std::string& verb, const std::string& newvar) {
  if (!strutil::isValidStataIdentifier(newvar))
    return engineErrorBlock(
        verb + ": invalid variable name `" + newvar + "' — " + strutil::stataIdentifierRuleText(),
        "syntax_error");
  // BUG-1120: `tssmooth ma _cons = x, window(1)' is "_cons invalid varname" r(198)
  // (StataBE 19.5); ipolate and expand answer the rest of the rule first.
  if (strutil::isReservedVarName(newvar))
    return nameRefusalBlock(strutil::NameRefusal{198, newvar + " invalid varname"});
  if (columnExistsDV(session, newvar))
    return engineErrorBlock(verb + ": variable " + newvar + " already defined", "bad_input");
  return ojson();  // empty => no objection
}

}  // namespace

ojson runXtile(const std::string& newvar, const std::string& srcvar,
               const std::vector<std::pair<std::string, eval::OptValue>>& opts,
               const std::optional<std::string>& ifExpr, SessionState& session,
               const std::optional<std::string>& weightType,
               const std::optional<std::string>& weightVar) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  if (srcvar.empty())
    return engineErrorBlock("xtile: syntax is `xtile newvar = varname, nquantiles(#)`",
                            "syntax_error");
  ojson guard = newVarGuardDV(session, "xtile", newvar);
  if (!guard.is_null() && !guard.empty()) return guard;
  if (ifExpr.has_value() && !ifExpr->empty()) {
    // NOT a wiring shortcut: with `if`, Stata computes the CUTPOINTS from the
    // restricted sample only. Forwarding a --keepif would instead compute them
    // from every row and then blank the rest — the same bins with a different
    // name, which is a wrong number. Refused with the recipe until the engine
    // grows a restricted-sample form.
    return engineErrorBlock(
        "xtile with `if` is not implemented: Stata derives the quantile cutpoints from the "
        "restricted sample, and this engine computes them over all rows. Restrict first "
        "(`preserve` + `keep if ...`, or a materialized subset frame), then xtile.",
        "unimplemented");
  }
  int nq = kXtileDefaultNq;
  bool nqGiven = false;
  std::string cutvar;
  for (const auto& [k, v] : opts) {
    if (k == "nquantiles") {
      if (v.isBool || !parseIntOptDV(v.strValue, nq))
        return engineErrorBlock("xtile: nquantiles() requires a whole number", "syntax_error");
      nqGiven = true;
    } else if (k == "cutpoints") {
      // BUG-371: the bin edges come from another variable's values instead of from
      // quantiles of srcvar. A different SOURCE of edges, not a modifier — Stata
      // treats the two as mutually exclusive and so does this.
      if (v.isBool)
        return engineErrorBlock("xtile: cutpoints() requires a variable name", "syntax_error");
      cutvar = trim(v.strValue);
    }
  }
  if (!cutvar.empty() && nqGiven)
    return engineErrorBlock(
        "xtile: nquantiles() and cutpoints() are alternatives — cutpoints() supplies the bin "
        "edges directly, so there are no quantiles left to ask for.",
        "bad_input");
  if (cutvar.empty() && nq < 2)
    return engineErrorBlock("xtile: nquantiles() must be at least 2", "bad_input");
  if (!cutvar.empty() && !columnExistsDV(session, cutvar))
    return engineErrorBlock("variable " + cutvar + " not found", "unknown_variable");
  if (weightType.has_value() && !weightType->empty() && weightVar.has_value() &&
      !weightVar->empty()) {
    // Quantile cutpoints are the weighted percentiles
    // (handlers_aer_reshape.cpp runXtileWeighted). With cutpoints() xtile.ado's
    // MakeCut ignores the weight but `mark` still drops the zero- and
    // missing-weight rows, which the engine's --xtile cannot do: refused by
    // name rather than categorizing rows Stata leaves missing.
    if (!cutvar.empty())
      return engineErrorBlock(
          "xtile: cutpoints() with a weight is not supported in this version of PIET.",
          "unsupported_option");
    return runXtileWeighted(newvar, srcvar, nq, *weightVar, *weightType, session);
  }

  std::string outPath = nextPath(session.workdir(), "xtile");
  const std::string spec =
      cutvar.empty() ? (newvar + "=" + srcvar + ", nq(" + std::to_string(nq) + ")")
                     : (newvar + "=" + srcvar + ", cutpoints(" + cutvar + ")");
  std::vector<std::string> argv = {"eval",  "--data", *session.currentHbin(),
                                   "--xtile", spec,
                                   "--out", outPath};
  SessionState::ResidentEligible resident(session);
  CobraRunResult cr = runCobra(argv);
  if (!cr.ok) {
    CobraError ce = errorFromRaw(cr);
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  session.load(outPath);
  return mutationResult(session, "gen", {newvar});
}

namespace {

// `[type] newvar` as typed before the `=`. A storage type in front of a name is
// accepted and ignored; the same word alone is what Stata's syntax reads as a
// misplaced type, hence its sentence about n() (StataBE 19.5).
std::optional<ojson> readXtileTarget(const std::string& typed, std::string* name) {
  std::vector<std::string> words;
  std::istringstream splitter(typed);
  for (std::string word; splitter >> word;) words.push_back(word);
  if (words.size() == 2 && strutil::isStorageTypeWord(words[0])) words.erase(words.begin());
  if (words.size() > 1) return rcErrorBlock("too many variables specified", 103);
  if (words.empty() || words[0] == "if") return rcErrorBlock("varlist required", 100);
  const std::string& word = words[0];
  if (word == "_all" || word == "_skip") return rcErrorBlock(word + " not allowed", 101);
  if (strutil::isStorageTypeWord(word))
    return rcErrorBlock("option n() incorrectly specified", 198);
  if (strutil::isReservedVarName(word) || word == "in" || word == "using" || word == "with")
    return rcErrorBlock(word + " invalid varname", 198);
  if (!strutil::isValidStataIdentifier(word)) return rcErrorBlock(word + " invalid name", 198);
  *name = word;
  return std::nullopt;
}

// cutpoints(): one numeric variable, abbreviations as anywhere in a varlist.
std::optional<ojson> readXtileCutVariable(const eval::OptValue& option, SessionState& session,
                                          std::string* resolved) {
  const std::string typed = option.isBool ? std::string() : trim(option.strValue);
  if (typed.empty()) return rcErrorBlock("option cutpoints() incorrectly specified", 198);
  if (typed.find_first_of(" \t") != std::string::npos)
    return rcErrorBlock("cutpoints():  too many variables specified", 103);
  bool ambiguous = false;
  const auto found = resolveVarName(typed, session, &ambiguous);
  if (ambiguous) return rcErrorBlock(typed + " ambiguous abbreviation\n(error in option cutpoints())", 111);
  if (!found.has_value())
    return rcErrorBlock("variable " + typed + " not found\n(error in option cutpoints())", 111);
  if (session.datastore()->col(*found).dtype == cecon::DType::Factor)
    return rcErrorBlock("string variables not allowed in option cutpoints();\n" + *found +
                            " is a string variable",
                        109);
  *resolved = *found;
  return std::nullopt;
}

// nquantiles(): `2`, `2.0` and `3.` are integers as written; `2.5` and `1e1` are
// not; a bare word is evaluated as a name, which is how `n(abc)` comes to be
// "abc not found". `shown` keeps the number as typed, for the variable label.
std::optional<ojson> readXtileCount(const eval::OptValue& option, SessionState& session,
                                    long long* count, std::string* shown) {
  const ojson malformed = rcErrorBlock("option n() incorrectly specified", 198);
  if (option.isBool) return malformed;
  const std::string typed = trim(option.strValue);
  if (typed.empty()) return std::nullopt;  // n() keeps the default
  if (strutil::isValidStataIdentifier(typed)) {
    if (resolveVarName(typed, session).has_value()) return malformed;
    return rcErrorBlock(typed + " not found", 111);
  }
  const size_t digitsFrom = (typed[0] == '+' || typed[0] == '-') ? 1 : 0;
  const size_t point = typed.find('.');
  const std::string whole = typed.substr(digitsFrom, point == std::string::npos ? point : point - digitsFrom);
  const std::string fraction = point == std::string::npos ? std::string() : typed.substr(point + 1);
  if (whole.empty() || whole.find_first_not_of("0123456789") != std::string::npos ||
      fraction.find_first_not_of('0') != std::string::npos)
    return malformed;
  *count = std::strtoll(whole.c_str(), nullptr, 10);
  if (typed[0] == '-') *count = -*count;
  *shown = typed;
  return std::nullopt;
}

// Stata's limit on a variable label, in characters.
std::string clippedVarLabel(const std::string& text) {
  constexpr size_t kLabelCharacters = 80;
  size_t characters = 0;
  for (size_t at = 0; at < text.size(); ++at) {
    if ((static_cast<unsigned char>(text[at]) & 0xC0) == 0x80) continue;
    if (characters++ == kLabelCharacters) return text.substr(0, at);
  }
  return text;
}

// The smallest integer type that holds the result's largest category.
const char* xtileStorageType(double largestCategory) {
  if (largestCategory <= 100.0) return "byte";
  if (largestCategory <= 32740.0) return "int";
  return "long";
}

}  // namespace

// The `xtile` command, [D] pctile: the checks in the order Stata answers them when
// several faults meet (black-box oracle in the item's folder), the sample as a
// source column that is missing outside it, and `cobra eval --xtile` for the
// quantile work.
ojson runXtileStata(const std::string& newvar, const std::string& expText,
                    const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                    const std::optional<std::string>& ifExpr,
                    const std::optional<std::string>& inRange,
                    const std::optional<std::string>& weightVar, SessionState& session,
                    const std::optional<std::string>& weightType) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();

  std::string target;
  if (auto refusal = readXtileTarget(newvar, &target)) return *refusal;
  if (columnExistsDV(session, target))
    return rcErrorBlock("variable " + target + " already defined", 110);

  const eval::OptValue* countOption = nullptr;
  const eval::OptValue* cutOption = nullptr;
  bool countRepeated = false, cutRepeated = false;
  for (const auto& [key, value] : opts) {
    if (key == "nquantiles") {
      countRepeated = countRepeated || countOption != nullptr;
      countOption = &value;
    } else if (key == "cutpoints") {
      cutRepeated = cutRepeated || cutOption != nullptr;
      cutOption = &value;
    }
  }

  std::string cutVariable;
  if (cutOption != nullptr)
    if (auto refusal = readXtileCutVariable(*cutOption, session, &cutVariable)) return *refusal;

  // if/in and exp, evaluated by generate: its "not found" and range answers are the
  // ones Stata gives here, and they outrank everything about the options.
  std::string qualifier;
  if (ifExpr.has_value() && !trim(*ifExpr).empty()) qualifier += " if " + *ifExpr;
  if (inRange.has_value() && !trim(*inRange).empty()) qualifier += " in " + *inRange;

  ScratchColumns scratch(session);
  std::string restrictionColumn;
  if (!qualifier.empty())
    if (auto failed = scratch.add("1", qualifier, &restrictionColumn)) return *failed;

  std::string sourceColumn;
  bool sourceIsString = false;
  if (auto failed = scratch.add(expText, qualifier, &sourceColumn)) {
    if (eval::coarseRc(*failed) != "109") return *failed;
    sourceIsString = true;
  }

  if (countOption != nullptr && cutOption != nullptr)
    return rcErrorBlock("both nquantiles() and cutpoints() cannot be specified", 198);

  const bool weighted = weightVar.has_value() && !weightVar->empty();
  std::string weightColumn;
  if (weighted)
    if (auto failed = scratch.add(*weightVar, "", &weightColumn)) return *failed;
  if (sourceIsString) return rcErrorBlock("type mismatch", 109);

  const auto rowCount = static_cast<size_t>(session.datastore()->nRows());
  std::vector<char> restricted(rowCount, 1);
  if (!restrictionColumn.empty()) {
    const auto marks = scratch.values(restrictionColumn);
    for (size_t row = 0; row < rowCount; ++row) restricted[row] = std::isnan(marks[row]) ? 0 : 1;
  }

  // Weight values: over the whole dataset for quantiles, over the restriction for
  // cutpoints() (measured; [D] pctile documents neither).
  std::vector<double> weights;
  if (weighted) {
    weights = scratch.values(weightColumn);
    const std::string kind = weightType.value_or("");
    const bool frequencyWeight = kind == "fw" || kind == "fweight";
    bool negative = false, fractional = false;
    for (size_t row = 0; row < rowCount; ++row) {
      if (std::isnan(weights[row]) || (cutOption != nullptr && !restricted[row])) continue;
      negative = negative || weights[row] < 0.0;
      fractional = fractional || weights[row] != std::floor(weights[row]);
    }
    if (negative) return rcErrorBlock("negative weights encountered", 402);
    if (frequencyWeight && fractional)
      return rcErrorBlock("may not use noninteger frequency weights", 401);
  }

  long long quantileCount = kXtileDefaultNq;
  std::string countShown = std::to_string(kXtileDefaultNq);
  if (countRepeated) return rcErrorBlock("option n() not allowed", 198);
  if (cutRepeated) return rcErrorBlock("option cutpoints() not allowed", 198);
  if (countOption != nullptr)
    if (auto refusal = readXtileCount(*countOption, session, &quantileCount, &countShown))
      return *refusal;

  // The marked sample: if, in, and a positive non-missing weight. A marked row
  // whose exp is missing still counts towards N.
  const auto sourceCells = scratch.values(sourceColumn);
  long long markedRows = 0, usableRows = 0;
  bool weightDropsARow = false;
  for (size_t row = 0; row < rowCount; ++row) {
    if (!restricted[row]) continue;
    if (weighted && !(weights[row] > 0.0)) {
      weightDropsARow = true;
      continue;
    }
    ++markedRows;
    if (!std::isnan(sourceCells[row])) ++usableRows;
  }
  if (cutOption == nullptr) {
    if (quantileCount < 2)
      return rcErrorBlock("nquantiles() must be greater than or equal to 2", 198);
    if (quantileCount > markedRows + 1)
      return rcErrorBlock(
          "nquantiles() must be less than or equal to number of observations plus one", 198);
  }
  if (usableRows == 0) return rcErrorBlock("no observations", 2000);

  if (weighted && weightDropsARow) {
    const std::string keep = sourceColumn;
    if (auto failed = scratch.add(keep, " if " + weightColumn + " > 0 & " + weightColumn + " < .",
                                  &sourceColumn))
      return *failed;
  }

  std::string engineCall = target + "=" + sourceColumn + ", ";
  if (cutOption != nullptr) {
    engineCall += "cutpoints(" + cutVariable + ")";
  } else {
    engineCall += "nq(" + std::to_string(quantileCount) + ")";
    if (weighted) engineCall += " w(" + weightColumn + ")";
  }
  (void)session.datastore();  // the head file the engine reads
  const std::string written = nextPath(session.workdir(), "xtile");
  {
    SessionState::ResidentEligible resident(session);
    const CobraRunResult run =
        runCobra({"eval", "--data", *session.currentHbin(), "--xtile", engineCall, "--out", written});
    if (!run.ok) {
      const CobraError failure = errorFromRaw(run);
      return engineErrorBlock(cobraErrorStr(failure), failure.code);
    }
  }
  session.load(written);
  scratch.dropAll();

  const Datastore* result = session.datastore();
  const double* categories = result->col(target).f64();
  double largest = 0.0;
  for (size_t row = 0; row < rowCount; ++row)
    if (categories[row] > largest) largest = categories[row];  // false for a missing category

  VarMeta& meta = session.varMeta[target];
  meta.stataType = xtileStorageType(largest);
  meta.format = "%8.0g";
  meta.label = clippedVarLabel(cutOption != nullptr
                                   ? expText + " categorized by " + cutVariable
                                   : countShown + " quantiles of " + expText);
  scratch.noteDataChanged();
  return mutationResult(session, "gen", {target});
}

ojson runIpolate(const std::vector<std::string>& varlist,
                 const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                 const std::optional<std::string>& ifExpr, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  if (varlist.size() != 2)
    return engineErrorBlock("ipolate: syntax is `ipolate yvar xvar, generate(newvar)`",
                            "syntax_error");
  const std::string& yvar = varlist[0];
  const std::string& xvar = varlist[1];
  std::string newvar;
  std::string byVars;
  bool epolate = false;
  for (const auto& [k, v] : opts) {
    if (k == "generate") {
      if (v.isBool)
        return engineErrorBlock("ipolate: generate() requires a variable name", "syntax_error");
      newvar = trim(v.strValue);
    } else if (k == "epolate") {
      epolate = !v.isBool || v.boolValue;
    } else if (k == "by") {
      // BUG-371: `by()` was refused on the grounds that "PIET's by-prefix plumbing
      // does not exist". That was a misreading of the syntax — Stata's ipolate
      // takes by() as an OPTION, not a by: prefix, and the engine's --ipolate has
      // parsed `by(g..)` since it was written. So this was a wiring gap after all,
      // and one line of pass-through closes it.
      if (v.isBool)
        return engineErrorBlock("ipolate: by() requires a variable list", "syntax_error");
      byVars = trim(v.strValue);
    }
  }
  if (newvar.empty())
    return engineErrorBlock("ipolate: generate(newvar) is required", "syntax_error");
  // BUG-1120 review: `ipolate x t, gen(_cons)' and gen(int) are "NAME invalid
  // varname" r(198) (StataBE 19.5).
  if (auto r = strutil::newVarNameRefusal(newvar)) return nameRefusalBlock(*r);
  ojson guard = newVarGuardDV(session, "ipolate", newvar);
  if (!guard.is_null() && !guard.empty()) return guard;
  if (ifExpr.has_value() && !ifExpr->empty()) {
    // Same class as xtile's refusal: the interpolation ANCHORS would come from
    // rows the user excluded, so the interpolated values would not be the ones
    // Stata reports for that sample.
    return engineErrorBlock(
        "ipolate with `if` is not implemented: the interpolation anchors would still be drawn "
        "from excluded rows. Restrict first (`preserve` + `keep if ...`), then ipolate.",
        "unimplemented");
  }
  std::string spec = newvar + "=" + yvar + " " + xvar;
  // The engine's --ipolate reads by() and epolate out of ONE options tail, so both
  // ride the same comma-separated suffix.
  if (!byVars.empty()) spec += ", by(" + byVars + ")";
  if (epolate) spec += ", epolate";
  std::string outPath = nextPath(session.workdir(), "ipolate");
  std::vector<std::string> argv = {"eval",       "--data", *session.currentHbin(),
                                   "--ipolate",  spec,     "--out", outPath};
  SessionState::ResidentEligible resident(session);
  CobraRunResult cr = runCobra(argv);
  if (!cr.ok) {
    CobraError ce = errorFromRaw(cr);
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  session.load(outPath);
  return mutationResult(session, "gen", {newvar});
}

ojson runTssmoothMa(const std::string& newvar, const std::string& srcvar,
                    const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                    SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  if (srcvar.empty())
    return engineErrorBlock(
        "tssmooth ma: syntax is `tssmooth ma newvar = varname, window(#l #c #f)`", "syntax_error");
  ojson guard = newVarGuardDV(session, "tssmooth ma", newvar);
  if (!guard.is_null() && !guard.empty()) return guard;
  if (!session.timevar.has_value() || session.timevar->empty())
    return engineErrorBlock(
        "tssmooth ma: run `tsset timevar` (or `xtset panelvar timevar`) first so PIET knows the "
        "time variable.",
        "unsupported_option");

  std::string windowSpec;
  bool haveWindow = false;
  for (const auto& [k, v] : opts) {
    if (k == "window") {
      if (v.isBool)
        return engineErrorBlock("tssmooth ma: window() requires #l #c #f", "syntax_error");
      windowSpec = v.strValue;
      haveWindow = true;
    }
  }
  if (!haveWindow)
    return engineErrorBlock("tssmooth ma: window(#l #c #f) is required", "syntax_error");
  std::vector<int> w;
  {
    std::string cur;
    auto flush = [&]() {
      if (cur.empty()) return true;
      int v = 0;
      if (!parseIntOptDV(cur, v)) return false;
      w.push_back(v);
      cur.clear();
      return true;
    };
    bool ok = true;
    for (char c : windowSpec) {
      if (std::isspace(static_cast<unsigned char>(c)) || c == ',') {
        if (!flush()) { ok = false; break; }
      } else {
        cur += c;
      }
    }
    if (ok) ok = flush();
    if (!ok)
      return engineErrorBlock("tssmooth ma: window() takes whole numbers", "syntax_error");
  }
  // BUG-378: the short forms are implemented now, with the semantics MEASURED
  // against real StataBE rather than guessed (the earlier refusal existed because
  // the omitted centre weight silently changes the window width, and my own
  // refusal text had the 2-number form wrong — it called it `#l #f`):
  //   window(1 1 1) -> (1/3)*[x(t-1) + 1*x(t) + x(t+1)]
  //   window(1 1)   -> (1/2)*[x(t-1) + 1*x(t)]      l=1, c=1, f=0
  //   window(1)     -> (1/1)*[x(t-1) + 0*x(t)]      l=1, c=0, f=0 — focal EXCLUDED
  // So the SECOND number is the centre, the third the leads, and both default to
  // 0 when omitted.
  if (w.empty() || w.size() > 3)
    return engineErrorBlock(
        "tssmooth ma: window() takes one to three whole numbers (#l [#c [#f]]); got " +
            std::to_string(w.size()),
        "syntax_error");
  const int wl = w[0];
  const int wc = w.size() >= 2 ? w[1] : 0;
  const int wf = w.size() >= 3 ? w[2] : 0;
  if (wl < 0 || wf < 0)
    return engineErrorBlock("tssmooth ma: window() lags and leads must not be negative",
                            "bad_input");
  if (wc != 0 && wc != 1)
    return engineErrorBlock("tssmooth ma: window()'s centre weight is 0 or 1", "bad_input");
  if (wl == 0 && wc == 0 && wf == 0)
    return engineErrorBlock("tssmooth ma: window() would average nothing", "bad_input");

  std::string outPath = nextPath(session.workdir(), "tsma");
  std::vector<std::string> argv = {"eval", "--data", *session.currentHbin(), "--tsma",
                                   newvar + "=" + srcvar + "," + std::to_string(wl) + "," +
                                       std::to_string(wc) + "," + std::to_string(wf),
                                   "--out", outPath};
  // Unconditional, unlike the expression surfaces' gated helper: this command is
  // meaningless without a time variable, and the guard above already proved one.
  argv = translate::injectTsContext(argv, session);
  SessionState::ResidentEligible resident(session);
  CobraRunResult cr = runCobra(argv);
  if (!cr.ok) {
    CobraError ce = errorFromRaw(cr);
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  session.load(outPath);
  return mutationResult(session, "gen", {newvar});
}

// ---------------------------------------------------------------------------
// `isid VARLIST` — uniqueness assertion (P1 wave). PIET-native: the engine has
// no isid verb, and the check is a single pass over the key columns' mmap'd
// payloads, so shelling out would cost a scratch .hbin for nothing.
//
// Oracle (StataBE 19.5, auto.dta): `isid make` → silent, rc 0; `isid foreign` →
// "variable foreign does not uniquely identify the observations", r(459);
// `isid nosuchvar` → r(111). Stata names ONE offending variable list in the
// message and pluralizes on arity, which is reproduced below.
//
// Missing values are ordinary key values here (Stata's isid does not exempt
// them), and each key cell is compared by its exact f64 bits so the extended
// missings `.a`-`.z` (data/missing.h) stay distinct from `.` — the same
// kind-exact identity the expression layer uses.
ojson runIsid(const std::vector<std::string>& varlist, SessionState& session, bool missok,
              bool sort) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  Datastore* ds = session.datastore();
  if (ds == nullptr) return eval::noDataLoaded();
  if (varlist.empty()) return engineErrorBlock("isid: variable list required");

  for (const auto& v : varlist) {
    if (!ds->has(v)) {
      // r(111) class — same wording shape as Stata's "variable X not found".
      return engineErrorBlock("variable " + v + " not found", "unknown_variable");
    }
  }

  const int64_t n = ds->nRows();
  if (n == 0) {  // no observation is trivially unique (one is checked for a missing key)
    return ojson::array();
  }

  // Build one 64-bit hash per row over the key tuple, then find duplicates by
  // hashing rows into buckets and confirming byte equality inside a bucket
  // (a hash collision must never be reported as a duplicate).
  struct KeyCol {
    const double* f64 = nullptr;
    const int32_t* codes = nullptr;
  };
  std::vector<KeyCol> keys;
  keys.reserve(varlist.size());
  for (const auto& v : varlist) {
    const cecon::Column& c = ds->col(v);
    KeyCol kc;
    if (c.dtype == cecon::DType::Factor) {
      kc.codes = c.codes();
    } else {
      kc.f64 = c.f64();
    }
    keys.push_back(kc);
  }

  // isid.ado CheckUniq: without `missok' a missing key value is r(459) before
  // uniqueness is looked at ("... should never be missing"; markout, strok: a
  // numeric `.'-`.z' or an empty string). w2ah_data_verbs.log ah_isid_missing:
  // rc 459 on a key whose third value is `.' -- main accepted it at rc 0.
  // BUG-1135: `missok' skips that check and a missing value is then one more key
  // value (two `.' keys do not identify their rows).
  for (size_t k = 0; k < keys.size() && !missok; ++k) {
    for (int64_t i = 0; i < n; ++i) {
      const bool miss = keys[k].codes != nullptr ? keys[k].codes[i] < 0
                                                 : std::isnan(keys[k].f64[i]);
      if (miss) {
        std::string joined;
        for (size_t j = 0; j < varlist.size(); ++j) {
          if (j) joined += " ";
          joined += varlist[j];
        }
        return rcErrorBlock((varlist.size() == 1 ? "variable " : "variables ") + joined +
                                " should never be missing",
                            459);
      }
    }
  }

  auto cellBits = [&keys](size_t k, int64_t row) -> uint64_t {
    if (keys[k].codes != nullptr) return static_cast<uint64_t>(
        static_cast<uint32_t>(keys[k].codes[row]));
    uint64_t bits;
    std::memcpy(&bits, keys[k].f64 + row, sizeof bits);
    return bits;
  };
  auto rowsEqual = [&](int64_t a, int64_t b) {
    for (size_t k = 0; k < keys.size(); ++k)
      if (cellBits(k, a) != cellBits(k, b)) return false;
    return true;
  };

  std::unordered_map<uint64_t, std::vector<int64_t>> buckets;
  buckets.reserve(static_cast<size_t>(n) * 2);
  for (int64_t i = 0; i < n; ++i) {
    uint64_t h = 1469598103934665603ull;  // FNV-1a offset basis
    for (size_t k = 0; k < keys.size(); ++k) {
      h ^= cellBits(k, i);
      h *= 1099511628211ull;
    }
    auto& bucket = buckets[h];
    for (int64_t prior : bucket) {
      if (rowsEqual(prior, i)) {
        std::string joined;
        for (size_t j = 0; j < varlist.size(); ++j) {
          if (j) joined += " ";
          joined += varlist[j];
        }
        return rcErrorBlock(
            (varlist.size() == 1 ? "variable " : "variables ") + joined +
                " do" + (varlist.size() == 1 ? "es" : "") +
                " not uniquely identify the observations",
            459);
      }
    }
    bucket.push_back(i);
  }
  if (!sort) return ojson::array();  // unique: silent, rc 0 (Stata prints nothing)
  // BUG-1135: `sort' then sorts by the varlist unless the sort marker already
  // begins with it, and says which: "(data now sorted by x)" / "(data already
  // sorted by x)" (isid.ado; StataBE 19.5).
  std::string joined;
  for (const auto& v : varlist) joined += (joined.empty() ? "" : " ") + v;
  std::vector<std::string> marker;
  {
    std::istringstream in(session.eval.sortedBy ? session.eval.sortedBy() : std::string());
    for (std::string k; in >> k;) marker.push_back(k);
  }
  const bool already = varlist.size() <= marker.size() &&
                       std::equal(varlist.begin(), varlist.end(), marker.begin());
  ojson out = ojson::array();
  if (!already) {
    ojson sorted = runSort(varlist, session);
    if (sorted.is_object() && sorted.value("type", std::string()) == "error") return sorted;
    out.push_back(std::move(sorted));
  }
  ojson note;
  note["type"] = "display";
  note["class"] = "note";
  note["text"] = "(data " + std::string(already ? "already" : "now") + " sorted by " + joined + ")";
  out.push_back(std::move(note));
  return out;
}

// ---------------------------------------------------------------------------
// P1 wave data verbs — expand / fillin / cross / contract. Each is a thin
// forwarder onto the engine subcommand of the same name (the joinby shape:
// validate here, transform there). The engine's own doc banners carry the
// StataBE 19.5 oracle pins.

ojson runExpand(const std::string& countArg, const std::optional<std::string>& genVar,
                SessionState& session) {
  return runExpandIf(countArg, genVar, std::nullopt, std::nullopt, session);
}

// `expand N [if] [in]`: the observations the qualifier selects become N copies
// each, the others stay once (w2_esample.log:1076 E6 `expand 2 if id==3`: rc 0,
// _N 61 -> 67). Built as filter -> expand the selected rows N-1 times -> append
// those copies after the data, which is where Stata puts created observations
// and the order Frame::expand writes them in (rep by rep, each in data order).
ojson runExpandIf(const std::string& countArg, const std::optional<std::string>& genVar,
                  const std::optional<std::string>& ifExpr,
                  const std::optional<std::string>& inRange, SessionState& session) {
  if (!session.currentHbin().has_value() || session.datastore() == nullptr) {
    return eval::noDataLoaded();
  }
  std::string n = trim(countArg);
  if (n.empty()) {
    return engineErrorBlock("expand: specify how many copies, e.g. `expand 2`");
  }
  // Stata accepts an EXPRESSION here (a per-observation count). This build
  // implements the constant form only and says so rather than guessing.
  for (char ch : n) {
    if (!std::isdigit(static_cast<unsigned char>(ch)) && ch != '-' && ch != '+') {
      return engineErrorBlock(
          "expand: this build supports a constant count only (e.g. `expand 2`); a per-observation "
          "expression is not implemented.");
    }
  }
  if (genVar.has_value() && !genVar->empty()) {
    // BUG-1120 review: the new-variable rule with _se and the r(table) row names:
    // `expand 2, gen(_cons)' is "_cons invalid varname" r(198) (StataBE 19.5).
    if (auto r = strutil::newVarNameRefusal(*genVar, /*resultRows=*/true))
      return nameRefusalBlock(*r);
    ojson guard = newVarGuardDV(session, "expand", *genVar);
    if (!guard.is_null() && !guard.empty()) return guard;
  }
  const int64_t rowsBefore = session.datastore()->nRows();
  std::string outPath = nextPath(session.workdir(), "expand");
  const bool qualified = (ifExpr.has_value() && !ifExpr->empty()) ||
                         (inRange.has_value() && !inRange->empty());
  long long copies = 0;
  try {
    copies = std::stoll(n);
  } catch (const std::exception&) {
    copies = 0;  // the engine names the malformed count below
  }
  CobraRunResult cr;
  if (!qualified || copies <= 1) {
    // N <= 1 leaves every observation once whatever the qualifier selects, but
    // the range is still read: `expand 1 in 9` on 3 rows is "observation numbers
    // out of range" r(198) (StataBE 19.5, BUG-1115).
    if (inRange.has_value() && !inRange->empty()) {
      const InRangeBounds rr = parseInRangeBounds(*inRange, rowsBefore);
      if (!rr.ok) {
        InRangeFilterResult fr;
        fr.ok = false;
        fr.refusal = rr.refusal;
        return inRangeRejectError(fr, *inRange, "expand");
      }
    }
    SessionState::ResidentEligible residentExpand(session);  // BUG-394
    cr = runCobra({"expand", "--data", *session.currentHbin(), "--n", n, "--out", outPath});
  } else {
    InRangeFilterResult fr;
    try {
      fr = applyIfAndInFilter(session, *session.currentHbin(), ifExpr, inRange);
    } catch (const CobraError& ce) {
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    if (!fr.ok) return inRangeRejectError(fr, inRange.value_or(std::string()), "expand");
    std::string created = fr.dataPath;
    if (copies > 2) {
      created = nextPath(session.workdir(), "expand_copies");
      const CobraRunResult er = runCobra({"expand", "--data", fr.dataPath, "--n",
                                          std::to_string(copies - 1), "--out", created});
      if (!er.ok) {
        CobraError ce = errorFromRaw(er);
        return engineErrorBlock(cobraErrorStr(ce), ce.code);
      }
    }
    session.ensureMaterializedHead("expand-if");
    SessionState::ResidentEligible residentExpand(session);  // the append's --out is the head
    cr = runCobra({"append", "--data", *session.currentHbin(), "--using", created, "--out",
                   outPath, "--type-clash", "error"});
  }
  if (!cr.ok) {
    CobraError ce = errorFromRaw(cr);
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  session.load(outPath);
  // e(sample) T2: a created observation is outside the estimation sample; the
  // originals keep their marker (w2_esample.log:1076 E6 `0|0|28|67|`; StataBE
  // capture w2_esample_copies X1 `old=23|created=0`). The copies carried the
  // mask like any column, so their cells are rewritten to 0.
  if (std::optional<std::vector<double>> cells = esampleHeadMask(session)) {
    bool any = false;
    for (size_t i = static_cast<size_t>(rowsBefore); i < cells->size(); ++i) {
      any = any || (*cells)[i] != 0.0;
      (*cells)[i] = 0.0;
    }
    if (any && !esampleRewriteHeadMask(session, *cells)) {
      return engineErrorBlock(
          "expand: the observations were created, but the estimation-sample marker could not "
          "be cleared on them; e(sample) is not reliable until the next estimation.");
    }
  }
  // B4-W3b `generate(newvar)`. Built from `_n > <rows before>` AFTER the
  // expansion, because that is the only fact that distinguishes an original
  // from a copy: the copies are byte-identical to their sources in every other
  // column. Checked BEFORE the engine call (above) so a name collision cannot
  // leave the dataset expanded but unmarked.
  if (genVar.has_value() && !genVar->empty()) {
    ojson r = runGen(*genVar + " = _n > " + std::to_string(rowsBefore), std::nullopt, session);
    if (r.is_object() && r.value("type", std::string()) == "error") return r;
    session.varMeta[*genVar].stataType = "byte";
    session.setVarLabel(*genVar, "Expanded observation type");
  }
  return mutationResult(session, "expand", genVar.has_value() && !genVar->empty()
                                               ? std::vector<std::string>{*genVar}
                                               : std::vector<std::string>{});
}

ojson runFillin(const std::vector<std::string>& varlist, SessionState& session) {
  if (!session.currentHbin().has_value() || session.datastore() == nullptr) {
    return eval::noDataLoaded();
  }
  if (varlist.empty()) return engineErrorBlock("fillin: variable list required");
  Datastore* ds = session.datastore();
  for (const auto& v : varlist) {
    if (!ds->has(v)) return engineErrorBlock("variable " + v + " not found", "unknown_variable");
  }
  std::string joined;
  for (size_t i = 0; i < varlist.size(); ++i) {
    if (i) joined += ",";
    joined += varlist[i];
  }
  std::string outPath = nextPath(session.workdir(), "fillin");
  CobraRunResult cr;
  {
    SessionState::ResidentEligible residentFillin(session);  // BUG-394
    cr = runCobra({"fillin", "--data", *session.currentHbin(), "--vars", joined, "--out", outPath});
  }
  if (!cr.ok) {
    CobraError ce = errorFromRaw(cr);
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  session.load(outPath);
  return mutationResult(session, "fillin", {"_fillin"});
}

ojson runCross(const std::string& usingPathArg, SessionState& session) {
  if (!session.currentHbin().has_value() || session.datastore() == nullptr) {
    return eval::noDataLoaded();
  }
  // Reuse joinby's `using` resolution (.dta/.csv pre-convert + virtual-save
  // materialization) so `cross using x.dta` works exactly like `joinby ... using x.dta`.
  JoinbyUsingResult ur = resolveJoinbyUsing(session, usingPathArg);
  if (!ur.ok) return ur.errBlock;

  std::vector<std::string> newCols;
  int64_t usingRows = 0, masterRows = 0;
  std::map<std::string, UsingVarMeta> uMeta;
  {
    Datastore usingDs(ur.hbinPath);
    Datastore* master = session.datastore();
    for (const auto& nm : usingDs.names())
      if (!master->has(nm)) newCols.push_back(nm);
    usingRows = usingDs.nRows();
    masterRows = master->nRows();
    uMeta = usingVarMetaOf(session, ur.usingPath, ur.metaPath, usingDs);
  }
  // A using file with no observations stops cross.ado inside its own expand/sort
  // of the using data, on its row-number tempvar: r(111), the data as they were
  // (StataBE 19.5, named as a fresh session names it; BUG-1105).
  if (usingRows == 0) return rcErrorBlock("variable __000001 not found", 111);
  std::string outPath = nextPath(session.workdir(), "cross");
  CobraRunResult cr;
  {
    SessionState::ResidentEligible residentCross(session);  // BUG-394
    cr = runCobra({"cross", "--data", *session.currentHbin(), "--using", ur.hbinPath, "--out", outPath});
  }
  if (!cr.ok) {
    CobraError ce = errorFromRaw(cr);
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  session.load(outPath);
  // e(sample) T2: each master observation keeps its marker on its pairing with
  // the FIRST using observation, every other pairing reads 0 (StataBE capture
  // w2_esample_copies X3 `23|183|m0=69`, X3b `k1=23|k2=0|k3=0`). The engine
  // writes those pairings first, in master order, and the rest after them
  // (Frame::cross, src/data/frame2.cpp), so rows from masterRows on read 0.
  if (std::optional<std::vector<double>> cells = esampleHeadMask(session); cells && usingRows > 1) {
    for (size_t p = static_cast<size_t>(masterRows); p < cells->size(); ++p) (*cells)[p] = 0.0;
    if (!esampleRewriteHeadMask(session, *cells)) {
      return engineErrorBlock(
          "cross: the data were formed, but the estimation-sample marker could not be "
          "cleared on the added pairings; e(sample) is not reliable until the next estimation.");
    }
  }
  adoptUsingOwnVars(session, ur, uMeta, newCols);
  return mutationResult(session, "cross", newCols);
}

ojson runContract(const std::vector<std::string>& varlist, SessionState& session) {
  if (!session.currentHbin().has_value() || session.datastore() == nullptr) {
    return eval::noDataLoaded();
  }
  if (varlist.empty()) return engineErrorBlock("contract: variable list required");
  Datastore* ds = session.datastore();
  for (const auto& v : varlist) {
    if (!ds->has(v)) return engineErrorBlock("variable " + v + " not found", "unknown_variable");
  }
  std::string joined;
  for (size_t i = 0; i < varlist.size(); ++i) {
    if (i) joined += ",";
    joined += varlist[i];
  }
  // e(sample) T2: a result row carries its group's first observation's marker,
  // as collapse's (StataBE capture w2_esample_copies K6 `contract id` 4 of 11,
  // K7 `contract id m0` m0_diff=0).
  const EsampleGroupMarks esMarks = esampleGroupMarksBefore(session, varlist, false);
  std::string outPath = nextPath(session.workdir(), "contract");
  CobraRunResult cr;
  {
    SessionState::ResidentEligible residentContract(session);  // BUG-394
    cr = runCobra({"contract", "--data", *session.currentHbin(), "--vars", joined, "--out", outPath});
  }
  if (!cr.ok) {
    CobraError ce = errorFromRaw(cr);
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  session.load(outPath);
  if (!esampleGroupMarksAfter(session, esMarks, varlist, EsampleLostBy::kContract)) {
    return engineErrorBlock(
        "contract: the data were contracted, but the estimation-sample marker could not be "
        "written onto the result; e(sample) is not reliable until the next estimation.");
  }
  return mutationResult(session, "contract", {"_freq"});
}

// ---------------------------------------------------------------------------
// export / import delimited (P1 wave). PIET-native: the writer needs the
// session's value labels, which live here and not in the .hbin.

namespace {


// A numeric cell the way Stata's `export delimited` writes it. A missing is
// `.` or its extended code `.a`..`.z` (oracle: Stata's own p1_roundtrip.csv
// carries literal `.a`).
//
// W:P8. The numeric spelling used to be the shortest round-trip decimal
// (`pythonFloatRepr`), which is NOT what Stata writes — measured cell-by-cell
// against Stata's own export of the same fixture, every one of 4,800 data rows
// differed. Two separate deviations: an integral value came out `1.0` where
// Stata writes `1` (so every id/year/region column in every exported CSV
// carried a spurious `.0` for whatever read it next), and a fractional value
// came out at 17 significant digits with its leading zero where Stata writes
// 16 without it. Stata's real rule, pinned by probing StataBE 19.5 with
// exact-valued doubles rather than inferred: `%.16g`, leading zero dropped
// for |x|<1, and the exponent branch re-spelled `%.11e` (`1e-20` ->
// `1.00000000000e-20`, `123456789012345678` -> `1.23456789012e+17`).
//
// Deliberate consequence, not an oversight: 16 digits is one short of a
// guaranteed round trip for a double, so a value can lose its last bit
// through a CSV. That is Stata's own behaviour and gate ① puts matching it
// first; PIET's own lossless path is `.dta`/`.hbin`.
//
// Known remaining divergence, out of reach here rather than unnoticed: Stata
// writes a `float` column at 8 significant digits, and PIET's in-memory
// datastore is all-f64, so a float column loaded from a .dta exports at
// double width. Closing that needs per-column storage types the datastore
// does not carry.
// `v` written with `dec` decimals, rounded HALF AWAY FROM ZERO (printf rounds
// half to even, which is what put 22 of 28,649 measured cells one unit below
// Stata in the 16th digit), then tidied Stata's way: trailing zeros and a bare
// trailing point dropped, and no leading zero on |x|<1.
// `v` written with `dec` decimals, then tidied Stata's way: trailing zeros and
// a bare trailing point dropped, and no leading zero on |x|<1.
std::string fixedDecimals(double v, int dec) {
  char buf[512];
  std::snprintf(buf, sizeof(buf), "%.*f", dec, v);
  std::string out(buf);
  if (out.find('.') != std::string::npos) {
    out.erase(out.find_last_not_of('0') + 1);
    if (!out.empty() && out.back() == '.') out.pop_back();
  }
  if (out.rfind("0.", 0) == 0) out.erase(0, 1);
  else if (out.rfind("-0.", 0) == 0) out.erase(1, 1);
  return out;
}

// P2-PERF ARC-B/S08. The same spelling as fixedDecimals(), written straight
// into a caller-owned buffer instead of a fresh std::string, and formatted from
// the MAGNITUDE so the "drop the leading zero of |x|<1" rule is a returned
// offset rather than an erase-in-the-middle. The caller prepends the sign;
// printf rounds on magnitude, so |round(v)| == round(|v|) and the bytes are
// identical either way. Returns the trimmed span inside `buf`.
struct FixedSpan {
  const char* p = nullptr;
  size_t len = 0;
};
FixedSpan fixedDecimalsAbs(char* buf, size_t cap, double av, int dec) {
  FixedSpan s;
  // std::to_chars(fixed, precision) and snprintf("%.*f") are both required to
  // be correctly rounded, so they emit the same digits; to_chars is simply not
  // obliged to consult the locale or parse a format string on the way, which is
  // most of what snprintf was costing per cell. The byte-identity diff of an
  // exported CSV is what certifies the equality, not this note.
  std::to_chars_result r = std::to_chars(buf, buf + cap, av, std::chars_format::fixed, dec);
  if (r.ec != std::errc()) return s;
  size_t len = static_cast<size_t>(r.ptr - buf);
  if (std::memchr(buf, '.', len) != nullptr) {
    while (len > 0 && buf[len - 1] == '0') --len;
    if (len > 0 && buf[len - 1] == '.') --len;
  }
  size_t off = 0;
  if (len >= 2 && buf[0] == '0' && buf[1] == '.') { off = 1; --len; }
  s.p = buf + off;
  s.len = len;
  return s;
}

std::string csvNumeric(double v) {
  if (std::isnan(v)) {
    // W:P8: an EXTENDED missing keeps its code (`.a`..`.z`, as Stata's own
    // p1_roundtrip.csv shows), but a plain system missing is written as an
    // EMPTY field — live-verified. Emitting `.` for it made 131 cells of the
    // P8 fixture read as the literal string "." to anything that opened the
    // file expecting a blank.
    std::string m = cecon::missing_text(cecon::missing_kind(v));
    return m == "." ? std::string() : m;
  }
  if (v == 0.0) return "0";
  // Stata spells the value as the WIDEST plain decimal that fits 17 characters
  // of magnitude (sign excluded) — not as a fixed count of significant digits,
  // which is why `.1473717297922637` carries 16 of them and
  // `.0000617833399588` only 12. Exponent form appears only when nothing plain
  // fits, and is `%.11e` exactly (`1e-20` -> `1.00000000000e-20`).
  //
  // MEASURED RESIDUAL, stated rather than hidden: against Stata's own export of
  // the 4,800-row P8 fixture, 139,185 of 139,200 cells are identical. The
  // remaining FIFTEEN differ by one unit in the last of 16-17 digits, and only
  // where the shortest round-trip decimal needs all 17 — Stata's own decimal
  // conversion is not correctly rounded there. Three candidate reconstructions
  // of its exact behaviour (round the 17-digit decimal half away from zero;
  // scale by a power of ten in double arithmetic; truncate the shortest
  // representation) were each implemented and measured, each matched FEWER
  // cells than the plain correctly-rounded conversion kept here, and they
  // contradicted one another on the same cells. Chasing a vendor float-printer
  // quirk below 1e-16 relative is not worth a bespoke decimal engine; the
  // format itself — width budget, integer spelling, dropped leading zero,
  // missing and date spelling, exponent form — is exact.
  for (int dec = 17; dec >= 0; --dec) {
    std::string s = fixedDecimals(v, dec);
    if (s == "0" || s == "-0") continue;  // rounded away to nothing; keep looking
    const size_t visible = s.size() - (s[0] == '-' ? 1 : 0);
    if (visible <= 17) return s;
  }
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.11e", v);
  return std::string(buf);
}

// P2-PERF ARC-B/S08 — the same function, appending instead of returning, with
// one shortcut. `export delimited` on a 500k-cell frame called csvNumeric once
// per cell and csvNumeric allocated a std::string per snprintf attempt; the
// writer measured 21 MB/s, which is not a formatting rate, it is an allocation
// rate.
//
// The shortcut is the INTEGRAL case (`id`, `year`, `region`, any coded
// variable — most columns of most datasets): the general loop reaches it by
// asking snprintf for "%.17f" and then deleting the seventeen zeros it just
// asked for. An integral double under 1e17 spells identically through
// std::to_chars, in one pass, with no allocation and no trimming. Above 1e17
// the integer part alone exceeds the 17-character budget, so that case still
// belongs to the loop (which sends it to the `%.11e` branch, unchanged).
//
// Everything else is byte-for-byte the old path: same widest-plain-decimal
// search from dec=17 down, same "rounded away to nothing" skip, same `%.11e`
// fallback. The gate is a byte-identity diff of an exported CSV, not a reading
// of this comment.
void appendCsvNumeric(std::string& out, double v) {
  if (std::isnan(v)) {
    std::string m = cecon::missing_text(cecon::missing_kind(v));
    if (m != ".") out += m;
    return;
  }
  if (v == 0.0) { out += '0'; return; }

  if (v == std::floor(v) && std::fabs(v) < 1e17) {
    char ib[32];
    const long long iv = static_cast<long long>(v);
    std::to_chars_result r = std::to_chars(ib, ib + sizeof(ib), iv);
    if (r.ec == std::errc()) {
      const size_t len = static_cast<size_t>(r.ptr - ib);
      if (len - (iv < 0 ? 1u : 0u) <= 17) { out.append(ib, len); return; }
    }
  }

  const bool neg = v < 0;
  const double av = neg ? -v : v;
  char buf[512];
  for (int dec = 17; dec >= 0; --dec) {
    FixedSpan s = fixedDecimalsAbs(buf, sizeof(buf), av, dec);
    if (s.p == nullptr) break;
    if (s.len == 1 && s.p[0] == '0') continue;  // rounded away to nothing
    if (s.len <= 17) {
      if (neg) out += '-';
      out.append(s.p, s.len);
      return;
    }
  }
  char eb[64];
  std::snprintf(eb, sizeof(eb), "%.11e", v);
  out += eb;
}

// How much CSV to accumulate before one write() (P2-PERF ARC-B/S08).
constexpr size_t kCsvBlockBytes = 1u << 20;

// P2-PERF ARC-B/S08: csvField's no-quoting-needed case returned a COPY of the
// level string for every cell. Appending skips it.
void appendCsvField(std::string& out, const std::string& s, char delim) {
  bool needQuote = false;
  for (char c : s) {
    if (c == delim || c == '"' || c == '\n' || c == '\r') { needQuote = true; break; }
  }
  if (!needQuote) { out += s; return; }
  out += '"';
  for (char c : s) {
    if (c == '"') out += "\"\"";
    else out += c;
  }
  out += '"';
}

// W:P8. Stata's `export delimited` writes a DATE-formatted variable as its
// formatted string, not as the stored index: measured against Stata's own
// export of the P8 fixture, all 4,800 rows of the `%td` column read `15jan2000`
// there and `14624` here, so every exported CSV handed a raw day count to
// whatever opened it next. (A plain numeric format like `%9.2f` is NOT applied
// — live-verified — so this is specific to the date families.)
//
// The decode rules are the frontend's, mirrored rather than re-derived: this
// is a C++ port of `fmtStataDate` in piet/frontend/js/grid.js (which owns the
// same job for the data grid and carries the design rationale, incl. the
// fixed-52-week year and %ty's un-offset storage). Any change to the families
// or their spelling has to land in BOTH — they are two renderers of one rule.
// `%tC` and `%td` detail strings (grid.js's own FIX:GRID-TFMT-DETAIL surface)
// are deliberately NOT claimed here; those fall through to the numeric path
// exactly as before.
constexpr int64_t kDaysTdEpochToUnix = 3653;  // 01jan1960 -> 01jan1970
constexpr int64_t kMsPerDay = 86400000;

int64_t floorDiv(int64_t a, int64_t b) {
  int64_t q = a / b;
  if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
  return q;
}
int64_t floorMod(int64_t a, int64_t b) { return a - floorDiv(a, b) * b; }

// days since 1970-01-01 -> civil y/m/d (Hinnant's civil_from_days).
void civilFromDays(int64_t z, int64_t* y, unsigned* m, unsigned* d) {
  z += 719468;
  const int64_t era = floorDiv(z, 146097);
  const int64_t doe = z - era * 146097;
  const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t yr = yoe + era * 400;
  const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int64_t mp = (5 * doy + 2) / 153;
  *d = static_cast<unsigned>(doy - (153 * mp + 2) / 5 + 1);
  *m = static_cast<unsigned>(mp < 10 ? mp + 3 : mp - 9);
  *y = yr + (*m <= 2);
}

std::string tdDayString(int64_t dayIndex) {
  static const char* const kMon[12] = {"jan", "feb", "mar", "apr", "may", "jun",
                                        "jul", "aug", "sep", "oct", "nov", "dec"};
  int64_t y; unsigned m, d;
  civilFromDays(dayIndex - kDaysTdEpochToUnix, &y, &m, &d);
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%02u%s%lld", d, kMon[m - 1], static_cast<long long>(y));
  return std::string(buf);
}

// The letter after `%t` for a date-family format, or 0 when `fmt` is not one
// this renderer owns. A detail suffix (`%tdNN/DD/CCYY`) yields 0 on purpose.
char dateFamilyOf(const std::string& fmt) {
  if (fmt.size() != 3 || fmt[0] != '%' || fmt[1] != 't') return 0;
  switch (fmt[2]) {
    case 'd': case 'w': case 'm': case 'q': case 'h': case 'y': case 'c':
      return fmt[2];
    default: return 0;
  }
}

std::string formatStataDate(double v, char family) {
  if (!std::isfinite(v)) return std::string();
  const int64_t i = static_cast<int64_t>(std::floor(v));
  char buf[48];
  switch (family) {
    case 'd': return tdDayString(i);
    case 'w': std::snprintf(buf, sizeof(buf), "%lldw%lld",
                            static_cast<long long>(1960 + floorDiv(i, 52)),
                            static_cast<long long>(floorMod(i, 52) + 1)); return buf;
    case 'm': std::snprintf(buf, sizeof(buf), "%lldm%lld",
                            static_cast<long long>(1960 + floorDiv(i, 12)),
                            static_cast<long long>(floorMod(i, 12) + 1)); return buf;
    case 'q': std::snprintf(buf, sizeof(buf), "%lldq%lld",
                            static_cast<long long>(1960 + floorDiv(i, 4)),
                            static_cast<long long>(floorMod(i, 4) + 1)); return buf;
    case 'h': std::snprintf(buf, sizeof(buf), "%lldh%lld",
                            static_cast<long long>(1960 + floorDiv(i, 2)),
                            static_cast<long long>(floorMod(i, 2) + 1)); return buf;
    case 'y': std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(i)); return buf;
    case 'c': {
      const int64_t days = floorDiv(i, kMsPerDay);
      const int64_t rem = floorMod(i, kMsPerDay);
      std::snprintf(buf, sizeof(buf), "%s %02lld:%02lld:%02lld", tdDayString(days).c_str(),
                    static_cast<long long>(floorDiv(rem, 3600000)),
                    static_cast<long long>(floorDiv(floorMod(rem, 3600000), 60000)),
                    static_cast<long long>(floorDiv(floorMod(rem, 60000), 1000)));
      return buf;
    }
    default: return std::string();
  }
}


bool hasFlagOpt(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                const std::string& name) {
  for (const auto& kv : opts) if (kv.first == name) return true;
  return false;
}

// An option's value with its surrounding double quotes off. `sheet("Alpha")`
// and `sheet(Alpha)` are the same request.
std::string optDequote(const std::string& raw) {
  std::string v = trim(raw);
  if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
  return trim(v);
}

// The value of a named option, or nullopt when it is absent or was given as a
// bare flag.
std::optional<std::string> optStrOf(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                    const std::string& name) {
  for (const auto& kv : opts) {
    if (kv.first != name) continue;
    if (kv.second.isBool) return std::nullopt;
    return optDequote(kv.second.strValue);
  }
  return std::nullopt;
}

}  // namespace

namespace {

// Stata's pathsuffix(): the extension of the last path component, "" when it
// has none. export delimited / import delimited supply ".csv" and the excel
// pair ".xlsx" to a name without one (w2ar dx_exd_noext log:760-769 writes
// ne1.csv and leaves the bare name absent; dx_exx_noext log:1527).
bool pathHasSuffix(const std::string& p) {
  const size_t slash = p.find_last_of("/\\");
  const size_t dot = p.find_last_of('.');
  if (dot == std::string::npos) return false;
  if (slash != std::string::npos && dot < slash) return false;
  return true;
}

std::string withDefaultSuffix(const std::string& p, const char* suffix) {
  return pathHasSuffix(p) ? p : p + suffix;
}

std::string stripOneQuotePair(const std::string& v);

// The file as the user typed it, with the suffix the command supplies: what
// Stata's export messages name (`file out.csv saved`, `file ./sub/o2.csv
// saved`, `file out.xlsx already exists`), never the resolved absolute path,
// which would put this machine's working directory into every captured
// transcript (BUG-410).
std::string typedFileName(const std::string& pathArg, const char* suffix) {
  return withDefaultSuffix(stripOneQuotePair(trim(pathArg)), suffix);
}

// True when the directory a file would be written into does not exist:
// Stata's export answers r(603) "file could not be opened" there (w2ar
// dx_exd_missing_dir, log:796).
bool parentDirMissing(const std::string& p) {
  const size_t slash = p.find_last_of('/');
  if (slash == std::string::npos || slash == 0) return false;
  std::error_code ec;
  return !std::filesystem::is_directory(cecon::to_native_path(p.substr(0, slash)), ec);
}

// A float column's text: Stata writes float storage at 8 significant digits
// (w2ar dx_exd_plain_L2 `1.01` for float 1.01, log:455; w2b 7.0100002,
// w2b log:420-423), integers in full, without the leading zero of |x|<1 (the
// same tidy rule the double path uses).
void appendFloatNumeric(std::string& out, double v) {
  if (std::isnan(v) || v == std::floor(v)) { appendCsvNumeric(out, v); return; }
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.8g", v);
  std::string s(buf);
  if (s.rfind("0.", 0) == 0) s.erase(0, 1);
  else if (s.rfind("-0.", 0) == 0) s.erase(1, 1);
  out += s;
}

std::string trimBlanks(const std::string& s) {
  const size_t a = s.find_first_not_of(' ');
  if (a == std::string::npos) return std::string();
  const size_t b = s.find_last_not_of(' ');
  return s.substr(a, b - a + 1);
}

void appendQuotedField(std::string& out, const std::string& s, char delim,
                       DelimWriteSpec::Quote q) {
  switch (q) {
    case DelimWriteSpec::Quote::kMinimal: appendCsvField(out, s, delim); return;
    case DelimWriteSpec::Quote::kNone: out += s; return;
    case DelimWriteSpec::Quote::kAllVerbatim: out += '"'; out += s; out += '"'; return;
    case DelimWriteSpec::Quote::kAll:
      out += '"';
      for (char c : s) {
        if (c == '"') out += "\"\"";
        else out += c;
      }
      out += '"';
      return;
  }
}

}  // namespace

ojson reEchoPath(ojson block, const std::string& resolved, const std::string& shown) {
  if (!block.is_object()) return block;
  // A success line carries "text"; an error block carries "message".
  for (const char* key : {"text", "message"}) {
    if (!block.contains(key) || !block[key].is_string()) continue;
    std::string text = block[key].get<std::string>();
    const size_t at = text.find(resolved);
    if (at == std::string::npos) continue;
    block[key] = text.substr(0, at) + shown + text.substr(at + resolved.size());
  }
  return block;
}

ojson writeDelimitedFile(const std::vector<std::string>& varlist, const std::string& path,
                         const DelimWriteSpec& spec, const std::optional<std::string>& ifExpr,
                         const std::optional<std::string>& inRange, SessionState& session) {
  if (!session.currentHbin().has_value() || session.datastore() == nullptr) {
    return eval::noDataLoaded();
  }
  const std::string& shown = spec.shown.empty() ? path : spec.shown;
  Datastore* base = session.datastore();
  std::vector<std::string> cols = varlist;
  if (cols.empty()) cols = base->names();
  for (const auto& v : cols) {
    if (!base->has(v)) return engineErrorBlock("variable " + v + " not found", "unknown_variable");
  }

  // Per-column facts read from the SESSION's frame (formats, storage types and
  // value labels are keyed by name), before any row filter.
  struct OutCol {
    std::string name;
    const std::map<std::string, std::string>* labels = nullptr;  // value labels, if any
    char dateFamily = 0;   // W:P8: letter after `%t`, 0 for a non-date column
    std::string dispFmt;   // display format (datafmt / outsheet)
    bool isFloat = false;  // float storage: 8 significant digits
  };
  std::vector<OutCol> oc;
  oc.reserve(cols.size());
  for (const auto& name : cols) {
    OutCol o;
    o.name = name;
    if (!spec.noLabel) {
      auto lv = session.varLblname.find(name);
      if (lv != session.varLblname.end()) {
        auto ld = session.labelDefs.find(lv->second);
        if (ld != session.labelDefs.end()) o.labels = &ld->second;
      }
    }
    const cecon::Column& col = base->col(name);
    if (col.dtype != cecon::DType::Factor) {
      const VarInfo vi = base->varinfo(name);
      o.dispFmt = session.displayFormat(vi);
      o.dateFamily = dateFamilyOf(o.dispFmt);
      auto vm = session.varMeta.find(name);
      if (vm != session.varMeta.end() && vm->second.stataType.has_value() &&
          !vm->second.stataType->empty()) {
        o.isFloat = *vm->second.stataType == "float";
      } else {
        // No declared storage: Stata's default numeric type is float, but this
        // frame's undeclared columns can hold doubles (an engine result), so a
        // column is written as float only when every value IS one.
        o.isFloat = true;
        const auto& f = col.f64();
        for (int64_t r = 0; r < base->nRows(); ++r) {
          const double v = f[r];
          if (!std::isnan(v) && static_cast<double>(static_cast<float>(v)) != v) {
            o.isFloat = false;
            break;
          }
        }
      }
    }
    oc.push_back(std::move(o));
  }

  Datastore* ds = base;
  std::unique_ptr<Datastore> filtered;
  if ((ifExpr.has_value() && !ifExpr->empty()) || (inRange.has_value() && !inRange->empty())) {
    InRangeFilterResult fr;
    try {
      fr = applyIfAndInFilter(session, base->path(), ifExpr, inRange);
    } catch (const CobraError& ce) {
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    if (!fr.ok) return inRangeRejectError(fr, inRange.value_or(std::string()), spec.verb);
    filtered = std::make_unique<Datastore>(fr.dataPath);
    ds = filtered.get();
  }

  // Write to a sibling temp then rename, so a failure never leaves a truncated
  // file where a readable one used to be (and dest==source is safe).
  std::string tmpPath = path + ".piet_tmp";
  {
    std::ofstream f(cecon::to_native_path(tmpPath), std::ios::binary);
    if (!f) {
      return eval::errorBlock("engine_error", "file " + shown + " could not be opened",
                              std::nullopt, std::nullopt, std::nullopt, "r(603)");
    }

    if (spec.header) {
      std::string head;
      for (size_t c = 0; c < cols.size(); ++c) {
        if (c) head += spec.delim;
        head += cols[c];
      }
      head += '\n';
      f.write(head.data(), static_cast<std::streamsize>(head.size()));
    }

    std::vector<const cecon::Column*> colp;
    colp.reserve(cols.size());
    for (const auto& name : cols) colp.push_back(&ds->col(name));

    // P2-PERF ARC-B/S08: rows accumulate into a ~1 MB block and go out with one
    // write() each, instead of one operator<< per row through the stream's
    // default buffer. Same bytes, two orders of magnitude fewer stream calls.
    const int64_t n = ds->nRows();
    std::string block;
    block.reserve(kCsvBlockBytes + 4096);
    for (int64_t r = 0; r < n; ++r) {
      for (size_t c = 0; c < oc.size(); ++c) {
        if (c) block += spec.delim;
        const cecon::Column& col = *colp[c];
        if (col.dtype == cecon::DType::Factor) {
          int32_t code = col.codes()[r];
          if (code >= 0) {
            appendQuotedField(block, col.levels[static_cast<size_t>(code)], spec.delim, spec.quote);
          } else if (spec.quote == DelimWriteSpec::Quote::kAll ||
                     spec.quote == DelimWriteSpec::Quote::kAllVerbatim) {
            block += "\"\"";
          }
          continue;
        }
        double v = col.f64()[r];
        // A value-labeled numeric exports as its LABEL (Stata's default;
        // `nolabel` gives the code) — the reason this writer lives in PIET.
        if (oc[c].labels != nullptr && !std::isnan(v)) {
          std::string key = eval::pythonFloatRepr(v);
          auto it = oc[c].labels->find(key);
          if (it == oc[c].labels->end() && v == std::floor(v)) {
            it = oc[c].labels->find(std::to_string(static_cast<long long>(v)));
          }
          if (it != oc[c].labels->end()) {
            appendQuotedField(block, it->second, spec.delim, spec.quote);
            continue;
          }
        }
        if (oc[c].dateFamily != 0 && !std::isnan(v)) {
          std::string ds8 = formatStataDate(v, oc[c].dateFamily);
          if (!ds8.empty()) {
            if (spec.quote == DelimWriteSpec::Quote::kMinimal) appendCsvField(block, ds8, spec.delim);
            else block += ds8;
            continue;
          }
        }
        if (spec.dataFmt && !std::isnan(v) && !oc[c].dispFmt.empty()) {
          std::string s;
          try {
            s = trimBlanks(eval::applyStataFmt(v, oc[c].dispFmt));
          } catch (...) {
            s.clear();
          }
          if (!s.empty()) { block += s; continue; }
        }
        if (oc[c].isFloat) appendFloatNumeric(block, v);
        else appendCsvNumeric(block, v);
      }
      block += '\n';
      if (block.size() >= kCsvBlockBytes) {
        f.write(block.data(), static_cast<std::streamsize>(block.size()));
        block.clear();
        if (!f) break;
      }
    }
    if (!block.empty()) f.write(block.data(), static_cast<std::streamsize>(block.size()));
    if (!f) {
      f.close();
      std::error_code rmEc;
      std::filesystem::remove(cecon::to_native_path(tmpPath), rmEc);
      return engineErrorBlock(spec.verb + ": failed writing " + shown + " (disk full?)", "data_io");
    }
  }
  std::error_code mvEc;
  std::filesystem::rename(cecon::to_native_path(tmpPath), cecon::to_native_path(path), mvEc);
  if (mvEc) {
    std::error_code rmEc;
    std::filesystem::remove(cecon::to_native_path(tmpPath), rmEc);
    return engineErrorBlock(spec.verb + ": failed writing " + shown, "data_io");
  }

  ojson block;
  block["type"] = "display";
  block["text"] = "file " + shown + " saved";
  return block;
}

ojson runExportDelimited(const std::vector<std::string>& varlist, const std::string& pathArg,
                         const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                         SessionState& session) {
  return runExportDelimited(varlist, pathArg, opts, std::nullopt, std::nullopt, session);
}

ojson runExportDelimited(const std::vector<std::string>& varlist, const std::string& pathArg,
                         const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                         const std::optional<std::string>& ifExpr,
                         const std::optional<std::string>& inRange, SessionState& session) {
  if (!session.currentHbin().has_value() || session.datastore() == nullptr) {
    return eval::noDataLoaded();
  }
  DelimWriteSpec spec;
  // export delimited: delimiter() is "tab" or ONE character, anything
  // longer is r(198) "<d> invalid character in delimiter() option".
  for (const auto& kv : opts) {
    if (kv.first != "delimiter" && kv.first != "delim") continue;
    if (kv.second.isBool) continue;
    std::string v = trim(kv.second.strValue);
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
    if (v == "tab" || v == "\\t") spec.delim = '\t';
    else if (v.size() == 1) spec.delim = v[0];
    else {
      return engineErrorBlock(v + " invalid character in delimiter() option", "unsupported_option");
    }
  }
  spec.noLabel = hasFlagOpt(opts, "nolabel");
  spec.header = !hasFlagOpt(opts, "novarnames");
  spec.dataFmt = hasFlagOpt(opts, "datafmt");
  if (hasFlagOpt(opts, "quote")) spec.quote = DelimWriteSpec::Quote::kAll;
  const bool replace = hasFlagOpt(opts, "replace");

  Datastore* ds = session.datastore();
  for (const auto& v : varlist) {
    if (!ds->has(v)) return engineErrorBlock("variable " + v + " not found", "unknown_variable");
  }

  const std::string path = withDefaultSuffix(resolveUserPath(session, pathArg), ".csv");
  spec.shown = typedFileName(pathArg, ".csv");
  if (cecon::native_exists(path) && !replace) {
    return eval::errorBlock("engine_error", "file " + spec.shown + " already exists", std::nullopt,
                            std::nullopt, std::nullopt, "r(602)");
  }
  if (parentDirMissing(path)) {
    return eval::errorBlock("engine_error", "file " + spec.shown + " could not be opened",
                            std::nullopt, std::nullopt, std::nullopt, "r(603)");
  }
  return writeDelimitedFile(varlist, path, spec, ifExpr, inRange, session);
}

namespace {

std::string stripOneQuotePair(const std::string& v) {
  if (v.size() >= 4 && v.compare(0, 2, "`\"") == 0 && v.compare(v.size() - 2, 2, "\"'") == 0)
    return v.substr(2, v.size() - 4);
  if (v.size() >= 2 && v.front() == '"' && v.back() == '"') return v.substr(1, v.size() - 2);
  return v;
}

}  // namespace

namespace {
// The block an import answers with: `use`'s own `open` shape (the frontend
// reads source_file/filepath off it), over the frame just adopted.
ojson openBlockFor(SessionState& session, const std::string& path) {
  ojson m = mutationResult(session, "open");
  ojson out;
  out["type"] = "data_mutation";
  out["action"] = "open";
  out["source_file"] = path;
  out["filepath"] = path;
  for (auto it = m.begin(); it != m.end(); ++it) {
    if (it.key() == "type" || it.key() == "action") continue;
    out[it.key()] = it.value();
  }
  return out;
}
}  // namespace

ojson runImportDelimited(const std::string& pathArg,
                         const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                         SessionState& session) {
  return runImportDelimited(std::vector<std::string>(), pathArg, opts, session);
}

std::string importSourcePath(SessionState& session, const std::string& pathArg, bool excel) {
  const std::string path = resolveUserPath(session, pathArg);
  if (!excel) return withDefaultSuffix(path, ".csv");
  if (pathHasSuffix(path)) return path;
  // A stem names NAME.xlsx, or NAME.xls when only that exists (StataBE 19.5:
  // `import excel using e2` reads e2.xls, `import excel e1` reads e1.xlsx).
  if (!cecon::native_exists(path + ".xlsx") && cecon::native_exists(path + ".xls")) return path + ".xls";
  return path + ".xlsx";
}

namespace {

using ImportOptionList = std::vector<std::pair<std::string, eval::OptValue>>;

// Stata's sentence under r(198): every option-grammar refusal below is one.
ojson importRefusal(const std::string& sentence) { return rcErrorBlock(sentence, 198); }

bool abbreviates(const std::string& typed, const char* word, size_t shortest) {
  return typed.size() >= shortest && std::string(word).compare(0, typed.size(), typed) == 0;
}

// A whole number spelled with digits and at most a leading sign. Far too many
// digits saturate rather than fail, so they still read as "a big number".
bool readWholeNumber(const std::string& text, long long* out) {
  const size_t digitsAt = (!text.empty() && (text[0] == '+' || text[0] == '-')) ? 1 : 0;
  if (digitsAt == text.size()) return false;
  if (text.find_first_not_of("0123456789", digitsAt) != std::string::npos) return false;
  *out = std::strtoll(text.c_str(), nullptr, 10);
  return true;
}

// One inclusive 1-based stretch of records or columns; 0 leaves a side open.
struct ImportSpan {
  long long first = 0;
  long long last = 0;
};

// rowrange() and colrange() share one grammar and differ in wording: Stata says
// "start column" but "end col", and answers a lone colon differently for each.
struct SpanWording {
  const char* option;
  const char* item;
  const char* endItem;
  const char* loneColon;
};
const SpanWording kRecordSpan{"rowrange", "row", "row", ": invalid end row in rowrange() option"};
const SpanWording kColumnSpan{"colrange", "column", "col",
                              " is an invalid end column in colrange() option"};

std::optional<ojson> readSpan(const std::string& typed, const SpanWording& words,
                              ImportSpan* span) {
  const std::string inOption = std::string(" in ") + words.option + "() option";
  const size_t colon = typed.find(':');
  const std::string head = trim(typed.substr(0, colon));
  const std::string tail =
      colon == std::string::npos ? std::string() : trim(typed.substr(colon + 1));
  if (colon != std::string::npos && head.empty() && tail.empty())
    return importRefusal(words.loneColon);
  if (!head.empty() && (!readWholeNumber(head, &span->first) || span->first < 1))
    return importRefusal(head + " is an invalid start " + words.item + inOption);
  if (tail.empty()) return std::nullopt;

  double end = 0.0;
  if (!delimited::parseNumber(tail, &end))
    return importRefusal(tail + ": invalid end " + words.endItem + inOption);
  if (end < static_cast<double>(std::max(span->first, 1LL)))
    return importRefusal(typed + " is an invalid " + words.item + " range" + inOption + "\nstart " +
                         words.item + " is greater than end " + words.item);
  // A fractional end leaves the stretch open (StataBE: rowrange(2:2.5) reads
  // to the last record).
  if (end == std::floor(end)) span->last = static_cast<long long>(end);
  return std::nullopt;
}

// numericcols()/stringcols(): file column numbers, held as stepped runs so that
// a huge `1/99999999` costs nothing.
struct ColumnPicks {
  struct Run {
    long long from, step, to;
  };
  bool everyColumn = false;
  std::vector<Run> runs;

  bool holds(long long column) const {
    if (everyColumn) return true;
    return std::any_of(runs.begin(), runs.end(), [column](const Run& r) {
      const long long gap = column - r.from;
      return gap % r.step == 0 && gap / r.step >= 0 && gap / r.step <= (r.to - r.from) / r.step;
    });
  }
};

bool readColumnRun(const std::string& word, ColumnPicks::Run* run) {
  const size_t slash = word.find('/');
  const size_t open = word.find('(');
  const size_t close = word.find(')');
  std::string from = word, to = word;
  long long step = 1;
  if (slash != std::string::npos) {
    from = word.substr(0, slash);
    to = word.substr(slash + 1);
  } else if (open != std::string::npos && close != std::string::npos && open < close) {
    from = word.substr(0, open);
    to = word.substr(close + 1);
    if (!readWholeNumber(word.substr(open + 1, close - open - 1), &step) || step == 0) return false;
  }
  if (!readWholeNumber(from, &run->from) || !readWholeNumber(to, &run->to)) return false;
  if (run->from < 1 || run->to < 1) return false;
  // `5/1` counts down; a stated step has to point from its start towards its end.
  const long long reach = run->to - run->from;
  if (slash != std::string::npos && reach < 0) step = -1;
  if (reach != 0 && (reach < 0) != (step < 0)) return false;
  run->step = step;
  return true;
}

std::optional<ojson> readColumnPicks(const std::string& typed, const std::string& option,
                                     ColumnPicks* picks) {
  if (typed == "_all") {
    picks->everyColumn = true;
    return std::nullopt;
  }
  std::string spaced = typed;
  std::replace(spaced.begin(), spaced.end(), ',', ' ');
  std::istringstream words(spaced);
  std::string word;
  bool anyWord = false;
  while (words >> word) {
    ColumnPicks::Run run{};
    if (!readColumnRun(word, &run))
      return importRefusal(typed + ": invalid numlist in " + option + "() option");
    picks->runs.push_back(run);
    anyWord = true;
  }
  if (!anyWord) return importRefusal(typed + ": invalid numlist in " + option + "() option");
  return std::nullopt;
}

// delimiters(): a shortcut word or a quoted set of characters, then at most one
// sub-option after a comma. *named stays false for `""` and for nothing at all,
// which both leave the choice to the reader.
std::optional<ojson> readDelimiters(const std::string& raw, delimited::ReadSpec* reader,
                                    bool* named) {
  const std::string text = raw.substr(std::min(raw.find_first_not_of(" \t"), raw.size()));
  std::string characters;
  std::string afterPart;
  const bool compoundQuoted = text.compare(0, 2, "`\"") == 0;
  if (compoundQuoted || (!text.empty() && text[0] == '"')) {
    const size_t open = compoundQuoted ? 2 : 1;
    const size_t close = compoundQuoted ? text.find("\"'", open) : text.find('"', open);
    if (close == std::string::npos) return importRefusal("unmatched quote");
    characters = text.substr(open, close - open);
    if (characters.find('"') != std::string::npos) return importRefusal("unmatched quote");
    afterPart = text.substr(close + (compoundQuoted ? 2 : 1));
    // backslash-t stands for a tab; a quoted word is its characters, never a shortcut.
    for (size_t at = characters.find("\\t"); at != std::string::npos;
         at = characters.find("\\t", at + 1))
      characters.replace(at, 2, "\t");
  } else {
    const size_t comma = text.find(',');
    const std::string part = text.substr(0, comma);
    if (comma != std::string::npos) afterPart = text.substr(comma);
    static const std::map<std::string, std::string> kShortcuts = {
        {"tab", "\t"}, {"comma", ","}, {"space", " "}, {"whitespace", "\t "}};
    const std::string word = trim(part);
    if (word.empty() && comma != std::string::npos)
      return importRefusal("invalid delimiter() argument");
    if (!word.empty()) {
      const auto shortcut = kShortcuts.find(word);
      if (shortcut == kShortcuts.end())
        return importRefusal(part + ": invalid delimiter() shortcut");
      characters = shortcut->second;
    }
  }

  const std::string rest = trim(afterPart);
  if (!rest.empty() && rest[0] != ',') return importRefusal(rest + ": invalid delimiter() argument");
  const std::string subOption = rest.empty() ? rest : trim(rest.substr(1));
  if (abbreviates(subOption, "collapse", 5)) {
    reader->collapse = true;
  } else if (abbreviates(subOption, "asstring", 5)) {
    reader->asString = true;
  } else if (!subOption.empty()) {
    return importRefusal(subOption + ": invalid delimiter() argument");
  }

  if (!characters.empty()) {
    reader->delims = characters;
    *named = true;
  }
  return std::nullopt;
}

// Everything `import delimited`'s options ask for, once they have been accepted.
struct ImportRequest {
  delimited::ReadSpec reader;
  bool delimiterNamed = false;
  delimited::Case nameCase = delimited::Case::kLower;
  bool noNamesRecord = false;
  long long namesRecord = 0;  // 1-based; 0 = decide from the first record
  std::string namesRecordTyped;
  ImportSpan records;
  ImportSpan columns;
  ColumnPicks numericColumns;
  ColumnPicks stringColumns;
  bool asDouble = false;
  bool asFloat = false;
  std::string encoding;  // "" = work it out from the bytes
};

std::optional<ojson> readOneImportOption(const std::string& key, const std::string& raw,
                                         ImportRequest* request) {
  using Bind = delimited::ReadSpec::Bind;
  using Strip = delimited::ReadSpec::Strip;
  const std::string typed = optDequote(raw);
  if (key == "delimiters" || key == "delimiter" || key == "delim")
    return readDelimiters(raw, &request->reader, &request->delimiterNamed);
  if (key == "rowrange") return readSpan(typed, kRecordSpan, &request->records);
  if (key == "colrange") return readSpan(typed, kColumnSpan, &request->columns);
  if (key == "numericcols") return readColumnPicks(typed, key, &request->numericColumns);
  if (key == "stringcols") return readColumnPicks(typed, key, &request->stringColumns);

  if (key == "bindquotes") {
    if (abbreviates(typed, "loose", 1)) request->reader.bind = Bind::kLoose;
    else if (abbreviates(typed, "strict", 1)) request->reader.bind = Bind::kStrict;
    else if (abbreviates(typed, "nobind", 3)) request->reader.bind = Bind::kNoBind;
    else
      return importRefusal(typed + " is an invalid bindquotes() argument; use loose, strict, or nobind");
  } else if (key == "stripquotes") {
    if (abbreviates(typed, "yes", 1)) request->reader.strip = Strip::kYes;
    else if (abbreviates(typed, "no", 1)) request->reader.strip = Strip::kNo;
    else if (abbreviates(typed, "default", 3)) request->reader.strip = Strip::kDefault;
    else
      return importRefusal(typed + " is an invalid stripquotes() argument; use yes, no, or default");
  } else if (key == "maxquotedrows") {
    long long limit = 0;
    if (typed != "unlimited" && !readWholeNumber(typed, &limit))
      return importRefusal("option maxquotedrows() incorrectly specified.");
    request->reader.maxQuotedRows = std::max(limit, 0LL);
  } else if (key == "emptylines") {
    if (typed != "skip" && typed != "include")
      return importRefusal(typed + " is an invalid emptylines() argument; use skip, or include");
    request->reader.skipEmptyLines = typed == "skip";
  } else if (key == "case") {
    if (abbreviates(typed, "lower", 1)) request->nameCase = delimited::Case::kLower;
    else if (abbreviates(typed, "upper", 1)) request->nameCase = delimited::Case::kUpper;
    else if (abbreviates(typed, "preserve", 3)) request->nameCase = delimited::Case::kPreserve;
    else
      return importRefusal(typed + ": invalid argument in option case();\n   use upper, lower, or preserve");
  } else if (key == "varnames") {
    if (abbreviates(typed, "nonames", 3)) {
      request->noNamesRecord = true;
    } else if (readWholeNumber(typed, &request->namesRecord) && request->namesRecord >= 1) {
      request->namesRecordTyped = typed;
    } else {
      return importRefusal(typed + ": invalid row number in varnames() option");
    }
  } else if (key == "encoding" || key == "charset") {
    const std::string folded = strutil::lower(typed);
    if (folded == "utf-8" || folded == "utf8") {
      request->encoding.clear();
    } else {
      const iconv_t probe = iconv_open("UTF-8", typed.c_str());
      if (probe == reinterpret_cast<iconv_t>(-1)) return importRefusal("encoding not supported");
      iconv_close(probe);
      request->encoding = typed;
    }
  }
  return std::nullopt;
}

std::optional<ojson> readImportOptions(const ImportOptionList& opts, ImportRequest* request) {
  for (const auto& [key, value] : opts) {
    if (key == "clear" || key == "collapsedelimiters") continue;  // the latter: accepted, no effect
    if (key == "asdouble" || key == "asfloat") {
      if (!value.isBool) return importRefusal("option " + key + "() incorrectly specified");
      (key == "asdouble" ? request->asDouble : request->asFloat) = true;
      continue;
    }
    if (value.isBool) {
      const bool delimiterSpelling = key == "delimiter" || key == "delim";
      return importRefusal("option " + (delimiterSpelling ? std::string("delimiters") : key) +
                           " incorrectly specified");
    }
    if (auto refusal = readOneImportOption(key, value.strValue, request)) return refusal;
  }
  if (request->numericColumns.everyColumn && request->stringColumns.everyColumn)
    return importRefusal("numericcols(_all) cannot be combined with stringcols(_all)");
  if (request->asDouble && request->asFloat)
    return importRefusal("asfloat() and asdouble() are mutually exclusive options");
  return std::nullopt;
}

// Strict RFC 3629 shape: no overlong forms, no surrogates, nothing past U+10FFFF.
bool holdsOnlyUtf8(const std::string& bytes) {
  static const unsigned long kSmallest[] = {0, 0x80, 0x800, 0x10000};
  for (size_t at = 0; at < bytes.size();) {
    const auto lead = static_cast<unsigned char>(bytes[at]);
    int extra = -1;
    if (lead < 0x80) extra = 0;
    else if ((lead >> 5) == 0x06) extra = 1;
    else if ((lead >> 4) == 0x0E) extra = 2;
    else if ((lead >> 3) == 0x1E) extra = 3;
    if (extra < 0 || at + static_cast<size_t>(extra) >= bytes.size()) return false;
    unsigned long point = extra == 0 ? lead : lead & (0x3Fu >> extra);
    for (int k = 1; k <= extra; ++k) {
      const auto next = static_cast<unsigned char>(bytes[at + static_cast<size_t>(k)]);
      if ((next & 0xC0) != 0x80) return false;
      point = (point << 6) | (next & 0x3F);
    }
    if (point < kSmallest[extra] || point > 0x10FFFF || (point >= 0xD800 && point <= 0xDFFF))
      return false;
    at += static_cast<size_t>(extra) + 1;
  }
  return true;
}

// nullopt when the bytes are not valid in `encoding`.
std::optional<std::string> decodeToUtf8(const std::string& bytes, const std::string& encoding) {
  const iconv_t converter = iconv_open("UTF-8", encoding.c_str());
  if (converter == reinterpret_cast<iconv_t>(-1)) return std::nullopt;
  std::string decoded;
  decoded.reserve(bytes.size() + bytes.size() / 8);
  char* input = const_cast<char*>(bytes.data());
  size_t inputLeft = bytes.size();
  char chunk[16384];
  bool clean = true;
  while (clean && inputLeft > 0) {
    char* output = chunk;
    size_t outputLeft = sizeof chunk;
    const size_t status = iconv(converter, &input, &inputLeft, &output, &outputLeft);
    decoded.append(chunk, sizeof chunk - outputLeft);
    if (status == static_cast<size_t>(-1) && errno != E2BIG) clean = false;
  }
  if (clean) {  // a stateful encoding may still owe its closing bytes
    char* output = chunk;
    size_t outputLeft = sizeof chunk;
    iconv(converter, nullptr, nullptr, &output, &outputLeft);
    decoded.append(chunk, sizeof chunk - outputLeft);
  }
  iconv_close(converter);
  if (!clean) return std::nullopt;
  return decoded;
}

// The file's text as UTF-8; an error block when it cannot be had.
std::optional<ojson> loadImportText(const std::string& path, const std::string& shownName,
                                    const std::string& encoding, std::string* text) {
  std::ifstream file(cecon::to_native_path(path), std::ios::binary);
  if (!file) return rcErrorBlock("file " + shownName + " could not be opened", 603);
  std::ostringstream slurped;
  slurped << file.rdbuf();
  *text = slurped.str();
  if (!encoding.empty() || !holdsOnlyUtf8(*text)) {
    const std::string source = encoding.empty() ? std::string("ISO-8859-1") : encoding;
    auto decoded = decodeToUtf8(*text, source);
    if (!decoded.has_value()) return importRefusal("file " + shownName + " is not valid " + source);
    *text = std::move(*decoded);
  }
  if (text->compare(0, 3, "\xEF\xBB\xBF") == 0) text->erase(0, 3);
  return std::nullopt;
}

// With no varnames(), the first record names the columns when, leaving its
// missing tokens aside, it holds text and no number.
bool recordReadsAsNames(const std::vector<std::string>& record) {
  bool sawText = false;
  for (const std::string& field : record) {
    if (delimited::isMissingToken(field)) continue;
    double unused = 0.0;
    if (delimited::parseNumber(field, &unused)) return false;
    sawText = true;
  }
  return sawText;
}

ojson answerEmptyImport(SessionState& session) {
  ojson cleared = runClear("", session);
  if (cleared.is_object() && cleared.value("type", std::string()) == "error") return cleared;
  ojson answer;
  answer["type"] = "display";
  answer["text"] = "(0 vars, 0 obs)";
  return answer;
}

// The namelist of `import delimited a b using f`: the leading names change and
// nothing else does.
std::optional<ojson> renameLeadingColumns(const std::vector<std::string>& namelist,
                                          std::vector<std::string>* names) {
  if (namelist.empty()) return std::nullopt;
  if (namelist.size() > names->size()) return rcErrorBlock("too many variables specified", 103);
  std::copy(namelist.begin(), namelist.end(), names->begin());
  const std::set<std::string> distinct(names->begin(), names->end());
  if (distinct.size() != names->size()) return importRefusal("could not name variables specified");
  return std::nullopt;
}

}  // namespace

// `import delimited`, [D] import delimited. The option layer above settles what
// was asked; delimited_io.h's reader does the parsing, naming and typing.
ojson runImportDelimited(const std::vector<std::string>& varlist, const std::string& pathArg,
                         const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                         SessionState& session) {
  for (const std::string& word : varlist)
    if (!strutil::isValidStataIdentifier(word))
      return importRefusal(word + ": invalid variable name");

  ImportRequest request;
  if (auto refusal = readImportOptions(opts, &request)) return *refusal;

  const std::string path = importSourcePath(session, pathArg, /*excel=*/false);
  const std::string shownName = typedFileName(pathArg, ".csv");
  if (!cecon::native_exists(path)) return rcErrorBlock("file " + shownName + " not found", 601);

  std::string text;
  if (auto failure = loadImportText(path, shownName, request.encoding, &text)) return *failure;
  if (!request.delimiterNamed)
    request.reader.delims = std::string(1, delimited::detectDelimiter(text));

  const auto records = delimited::parse(text, request.reader);
  text.clear();
  if (records.empty()) return answerEmptyImport(session);

  // Which record names the columns, and which records hold data (both 0-based
  // here). A record at or before the names record is never data.
  const auto recordCount = static_cast<long long>(records.size());
  if (request.namesRecord > recordCount)
    return importRefusal(request.namesRecordTyped + ": invalid row number in varnames() option");
  long long namesAt = -1;
  if (request.namesRecord > 0) namesAt = request.namesRecord - 1;
  else if (!request.noNamesRecord && recordReadsAsNames(records.front())) namesAt = 0;
  const long long dataFrom = std::max(request.records.first - 1, namesAt + 1);
  const long long dataEnd =
      request.records.last > 0 ? std::min(request.records.last, recordCount) : recordCount;

  // The file's width over the records in play, then the colrange() window in it.
  size_t width = namesAt >= 0 ? records[static_cast<size_t>(namesAt)].size() : 0;
  for (long long r = dataFrom; r < dataEnd; ++r)
    width = std::max(width, records[static_cast<size_t>(r)].size());
  const long long firstColumn = std::max(request.columns.first, 1LL);
  const long long lastColumn = request.columns.last > 0
                                   ? std::min(request.columns.last, static_cast<long long>(width))
                                   : static_cast<long long>(width);
  if (firstColumn > lastColumn) return answerEmptyImport(session);
  const auto windowSize = static_cast<size_t>(lastColumn - firstColumn + 1);
  const auto fileColumnOf = [firstColumn](size_t slot) {
    return firstColumn + static_cast<long long>(slot);
  };
  static const std::string kAbsentCell;
  const auto cellOf = [&](const std::vector<std::string>& record,
                          size_t slot) -> const std::string& {
    const auto index = static_cast<size_t>(fileColumnOf(slot) - 1);
    return index < record.size() ? record[index] : kAbsentCell;
  };

  // Names and labels.
  std::vector<std::string> headerCells(windowSize);
  if (namesAt >= 0)
    for (size_t slot = 0; slot < windowSize; ++slot)
      headerCells[slot] = cellOf(records[static_cast<size_t>(namesAt)], slot);
  delimited::NameResult named = delimited::legalNames(headerCells, request.nameCase);
  if (named.clash) return importRefusal("Error creating variables");
  if (auto refusal = renameLeadingColumns(varlist, &named.names)) return *refusal;

  delimited::Table table;
  table.names = std::move(named.names);
  table.labels = namesAt >= 0 ? std::move(named.labels) : std::vector<std::string>(windowSize);

  // Storage types, each from its column's data cells.
  const bool typeAsDouble =
      request.asDouble || (!request.asFloat && session.defaultStorageType == "double");
  std::vector<const std::string*> columnCells;
  for (size_t slot = 0; slot < windowSize; ++slot) {
    columnCells.clear();
    for (long long r = dataFrom; r < dataEnd; ++r)
      columnCells.push_back(&cellOf(records[static_cast<size_t>(r)], slot));
    const bool numericForced = request.numericColumns.holds(fileColumnOf(slot));
    const bool stringForced = !numericForced && request.stringColumns.holds(fileColumnOf(slot));
    table.types.push_back(
        delimited::typeColumn(columnCells, stringForced, numericForced, typeAsDouble));
  }

  for (long long r = dataFrom; r < dataEnd; ++r) {
    std::vector<std::string> row;
    row.reserve(windowSize);
    for (size_t slot = 0; slot < windowSize; ++slot)
      row.push_back(cellOf(records[static_cast<size_t>(r)], slot));
    table.rows.push_back(std::move(row));
  }

  if (auto failure = delimited::stageAndAdopt(session, table, "import delimited", path))
    return *failure;
  return openBlockFor(session, path);
}

// ---------------------------------------------------------------------------
// export / import excel (P1 wave). The .xlsx container work lives in
// piet/native/src/xlsx.{h,cpp}; this layer is the Stata surface.

namespace {

// `case(preserve|lower|upper)` — [D] import excel p.495. The case rule applies
// to the names read by `firstrow`, and Stata's default is preserve.
enum class NameCase { kPreserve, kLower, kUpper };


// One A1-style cell address. `col`/`row` come back 0 for a side the address
// leaves out, so "A" is column 1 with an unbounded row and "8" is row 8 with an
// unbounded column.
bool parseCellAddr(const std::string& s, int* col, int* row) {
  size_t i = 0;
  long long c = 0;
  while (i < s.size() && std::isalpha(static_cast<unsigned char>(s[i]))) {
    c = c * 26 + (std::toupper(static_cast<unsigned char>(s[i])) - 'A' + 1);
    if (c > 1000000) return false;
    ++i;
  }
  long long r = 0;
  bool anyDigit = false;
  while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
    r = r * 10 + (s[i] - '0');
    if (r > 100000000LL) return false;
    anyDigit = true;
    ++i;
  }
  if (i != s.size()) return false;
  if (c == 0 && !anyDigit) return false;
  *col = static_cast<int>(c);
  *row = anyDigit ? static_cast<int>(r) : 0;
  return true;
}

// `cellrange([start][:end])` — [D] import excel p.495. Both sides are optional,
// so `A8`, `A3:AG281`, `:D70` and `A3:` are all legal spellings.
bool parseCellRangeOpt(const std::string& spec, xlsx::ReadOptions* out, std::string* err) {
  const std::string s = trim(spec);
  if (s.empty()) {
    *err = "cellrange(): expected a cell or a cell range, for example cellrange(A3:AG281)";
    return false;
  }
  const size_t colon = s.find(':');
  const std::string start = colon == std::string::npos ? s : trim(s.substr(0, colon));
  const std::string end = colon == std::string::npos ? std::string() : trim(s.substr(colon + 1));
  if (!start.empty() && !parseCellAddr(start, &out->firstCol, &out->firstRow)) {
    *err = "cellrange(): `" + start + "` is not a cell reference";
    return false;
  }
  if (!end.empty() && !parseCellAddr(end, &out->lastCol, &out->lastRow)) {
    *err = "cellrange(): `" + end + "` is not a cell reference";
    return false;
  }
  if (out->lastRow > 0 && out->firstRow > out->lastRow) {
    *err = "cellrange(): the range ends before it starts (" + s + ")";
    return false;
  }
  if (out->lastCol > 0 && out->firstCol > out->lastCol) {
    *err = "cellrange(): the range ends before it starts (" + s + ")";
    return false;
  }
  return true;
}

// Stata's own default variable names when there is no `firstrow`: the SHEET's
// column letters, upper case (BUG-425). `idx0` is 0-based from column A.
std::string excelColumnName(size_t idx0) {
  std::string s;
  for (size_t x = idx0 + 1; x > 0;) {
    const size_t rem = (x - 1) % 26;
    s.insert(s.begin(), static_cast<char>('A' + rem));
    x = (x - 1) / 26;
  }
  return s;
}

}  // namespace

ojson runExportExcel(const std::vector<std::string>& varlist, const std::string& pathArg,
                     const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                     SessionState& session) {
  return runExportExcel(varlist, pathArg, opts, std::nullopt, std::nullopt, session);
}

ojson runExportExcel(const std::vector<std::string>& varlist, const std::string& pathArg,
                     const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                     const std::optional<std::string>& ifExpr,
                     const std::optional<std::string>& inRange, SessionState& session) {
  if (!session.currentHbin().has_value() || session.datastore() == nullptr) {
    return eval::noDataLoaded();
  }
  Datastore* ds = session.datastore();
  const bool replace = hasFlagOpt(opts, "replace");
  const bool noLabel = hasFlagOpt(opts, "nolabel");
  // `firstrow(variables|varlabels)` writes row 1: the names, or each
  // variable's label (its name when it has none). The value takes the
  // abbreviations `var` and `varl` (w2ar dx_exx_first_var `first(var)` rc 0,
  // log:1073; dx_exx_varlabels log:1394; the import-excel fixture's
  // `firstrow(varlabels)` Sheet1, log:1691-1716).
  bool header = false;
  bool headerLabels = false;
  for (const auto& kv : opts) {
    if (kv.first != "firstrow") continue;
    header = true;
    if (!kv.second.isBool) {
      std::string v = strutil::lower(optDequote(kv.second.strValue));
      if (v.size() >= 3 && std::string("variables").compare(0, v.size(), v) == 0) {
        headerLabels = false;
      } else if (v.size() >= 4 && std::string("varlabels").compare(0, v.size(), v) == 0) {
        headerLabels = true;
      } else {
        return engineErrorBlock(
            "export excel: firstrow(" + v + ") — the value is `variables` or `varlabels`",
            "unsupported_option");
      }
    }
  }

  // `sheet("Alpha")` names the worksheet. Stata's full grammar is
  // `sheet("name"[, modify|replace])`, with the sub-option INSIDE the
  // parentheses; `sheetreplace` / `sheetmodify` are the same request spelled as
  // top-level flags (the spelling the corpus actually uses — 020, 038, 091, 108).
  std::string sheetName;
  bool sheetMerge = false;      // the sub-option asks to keep the rest of the workbook
  std::string sheetMergeWord;
  std::optional<std::string> sheetRaw;
  for (const auto& kv : opts) {
    if (kv.first == "sheet" && !kv.second.isBool) sheetRaw = kv.second.strValue;
  }
  if (sheetRaw.has_value()) {
    // The name is verbatim inside its quotes: `sheet("8-2 ")` keeps the
    // trailing blank (w2ar log:1655 `[8-2 ]`).
    auto unquote = [](std::string t) {
      const size_t a = t.find_first_not_of(" \t");
      const size_t b = t.find_last_not_of(" \t");
      t = a == std::string::npos ? std::string() : t.substr(a, b - a + 1);
      if (t.size() >= 2 && t.front() == '"' && t.back() == '"') t = t.substr(1, t.size() - 2);
      return t;
    };
    std::string v = *sheetRaw;
    size_t comma = std::string::npos;
    bool inQ = false;
    for (size_t i = 0; i < v.size(); ++i) {
      if (v[i] == '"') inQ = !inQ;
      else if (v[i] == ',' && !inQ) { comma = i; break; }
    }
    if (comma == std::string::npos) v = unquote(v);
    if (comma != std::string::npos) {
      sheetMergeWord = strutil::lower(trim(v.substr(comma + 1)));
      v = unquote(v.substr(0, comma));
      if (sheetMergeWord != "modify" && sheetMergeWord != "replace") {
        return engineErrorBlock(
            "export excel: sheet(\"" + v + "\", " + sheetMergeWord +
                ") — the sub-option is `modify` or `replace`",
            "unsupported_option");
      }
      sheetMerge = true;
    }
    sheetName = v;
  }
  if (hasFlagOpt(opts, "sheetreplace")) { sheetMerge = true; sheetMergeWord = "sheetreplace"; }
  if (hasFlagOpt(opts, "sheetmodify")) { sheetMerge = true; sheetMergeWord = "sheetmodify"; }
  if (sheetMerge && sheetName.empty()) {
    return engineErrorBlock(
        "export excel: " + sheetMergeWord + " needs the sheet it applies to — add sheet(\"name\")",
        "unsupported_option");
  }

  std::vector<std::string> cols = varlist;
  if (cols.empty()) cols = ds->names();
  for (const auto& v : cols) {
    if (!ds->has(v)) return engineErrorBlock("variable " + v + " not found", "unknown_variable");
  }

  std::string path = withDefaultSuffix(resolveUserPath(session, pathArg), ".xlsx");
  const std::string typed = typedFileName(pathArg, ".xlsx");
  const bool workbookExists = cecon::native_exists(path);
  // the manual rules out combining replace with a per-worksheet modify or
  // replace ([D] import excel) — the two mean opposite things about
  // the rest of the workbook, so accepting both would have to pick one
  // silently.
  if (sheetMerge && replace) {
    return engineErrorBlock(
        "export excel: `replace` overwrites the whole workbook and cannot be combined with " +
            sheetMergeWord,
        "unsupported_option");
  }
  if (workbookExists && !replace && !sheetMerge) {
    return eval::errorBlock("engine_error", "file " + typed + " already exists", std::nullopt,
                            std::nullopt, std::nullopt, "r(602)");
  }
  // A workbook whose directory is missing (StataBE 19.5, BUG-410): `replace`
  // attempts the write, `file nodir/o.xlsx could not be saved` r(603); a sheet()
  // first loads the workbook, `could not be loaded` r(603); with neither,
  // export excel's own existence test answers `already exists` r(602).
  if (parentDirMissing(path)) {
    const bool loads = sheetRaw.has_value() || sheetMerge;
    const std::string what = replace ? " could not be saved"
                             : loads ? " could not be loaded"
                                     : " already exists";
    return eval::errorBlock("engine_error", "file " + typed + what, std::nullopt, std::nullopt,
                            std::nullopt, replace || loads ? "r(603)" : "r(602)");
  }

  // [if] [in] (export excel): the rows come from a
  // filtered copy; names, labels and formats stay the session's.
  std::unique_ptr<Datastore> filtered;
  if ((ifExpr.has_value() && !ifExpr->empty()) || (inRange.has_value() && !inRange->empty())) {
    InRangeFilterResult fr;
    try {
      fr = applyIfAndInFilter(session, ds->path(), ifExpr, inRange);
    } catch (const CobraError& ce) {
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    if (!fr.ok) return inRangeRejectError(fr, inRange.value_or(std::string()), "export excel");
    filtered = std::make_unique<Datastore>(fr.dataPath);
    ds = filtered.get();
  }

  const int64_t n = ds->nRows();
  std::vector<xlsx::OutColumn> out;
  out.reserve(cols.size());
  for (const auto& name : cols) {
    const cecon::Column& col = ds->col(name);
    xlsx::OutColumn oc;
    oc.name = name;
    if (headerLabels) {
      auto vm = session.varMeta.find(name);
      if (vm != session.varMeta.end() && vm->second.label.has_value() && !vm->second.label->empty())
        oc.name = *vm->second.label;
    }
    // A %td variable is written as Excel date serials (Stata date + 21916)
    // under the m/d/yyyy format, a %#.2f one under 0.00 (w2ar
    // dx_exx_first_var_s1n_v7 int %tdnn/dd/CCYY and _v3 double %14.2f read
    // back, log:1095-1104).
    bool asExcelDate = false;
    if (col.dtype != cecon::DType::Factor) {
      const std::string fmt = session.displayFormat(session.datastore()->varinfo(name));
      std::string f = fmt;
      if (!f.empty() && f[0] == '%') f.erase(0, 1);
      if (!f.empty() && f[0] == '-') f.erase(0, 1);
      if (f.rfind("td", 0) == 0) {
        asExcelDate = true;
        oc.numFmtId = 14;
      } else if (!hasFlagOpt(opts, "keepcellfmt")) {
        // Under keepcellfmt the 0.00 format is not written (w2ar dx_exx_097
        // reads x back %10.0g, log:1375; without it %14.2f, log:1196).
        static const std::regex reTwoDec(R"(^%-?[0-9]*\.2fc?$)");
        if (std::regex_match(fmt, reTwoDec)) oc.numFmtId = 2;
      }
    }
    oc.isNumber.assign(static_cast<size_t>(n), false);
    oc.isBlank.assign(static_cast<size_t>(n), false);
    oc.numbers.assign(static_cast<size_t>(n), 0.0);
    oc.strings.assign(static_cast<size_t>(n), std::string());

    const std::map<std::string, std::string>* labels = nullptr;
    if (!noLabel) {
      auto lv = session.varLblname.find(name);
      if (lv != session.varLblname.end()) {
        auto ld = session.labelDefs.find(lv->second);
        if (ld != session.labelDefs.end()) labels = &ld->second;
      }
    }

    for (int64_t r = 0; r < n; ++r) {
      size_t i = static_cast<size_t>(r);
      if (col.dtype == cecon::DType::Factor) {
        int32_t code = col.codes()[r];
        if (code < 0) { oc.isBlank[i] = true; continue; }
        oc.strings[i] = col.levels[static_cast<size_t>(code)];
      } else {
        double v = col.f64()[r];
        if (std::isnan(v)) {
          // Excel has no missing sentinel; Stata writes the cell EMPTY. The
          // extended code is not representable here — a named limitation, not
          // a silent substitution (the delimited writer does carry it).
          oc.isBlank[i] = true;
          continue;
        }
        if (labels != nullptr) {
          std::string key = eval::pythonFloatRepr(v);
          auto it = labels->find(key);
          if (it == labels->end() && v == std::floor(v))
            it = labels->find(std::to_string(static_cast<long long>(v)));
          if (it != labels->end()) { oc.strings[i] = it->second; continue; }
        }
        oc.isNumber[i] = true;
        oc.numbers[i] = asExcelDate ? v + 21916.0 : v;
      }
    }
    out.push_back(std::move(oc));
  }

  try {
    // A sheetreplace against a workbook that is not there yet is just a write:
    // a sheet name that the workbook does not have yet simply gets created
    // ([D] import excel), and there is no
    // workbook to preserve.
    if (sheetMerge && workbookExists) xlsx::writeSheetInto(path, sheetName, out, header);
    else xlsx::write(path, out, header, sheetName);
  } catch (const std::exception& e) {
    return reEchoPath(engineErrorBlock(std::string("export excel: ") + e.what(), "data_io"), path,
                      typed);
  }
  ojson block;
  block["type"] = "display";
  block["text"] = "file " + typed + " saved";
  return block;
}

// `import excel [using] FILE [, sheet("S") cellrange(R) firstrow case(C)
//                              allstring describe clear]` — AER scope-2 W3a.
//
// The option cluster this build was missing is the single most frequent one in
// the twelve-paper corpus (`import excel` appears 273 times, 243 of them in
// paper 015), and every one of those calls names a sheet.
namespace {

// How Stata's import excel reads an Excel date/time number format (StataBE
// scratch-r150/t7-007/stata/dt: dt2.golden + dt3.golden -- every built-in date
// id and four custom codes, as numeric, allstring and text-mixed columns; AER
// 007's survey exports, whose dates sort and merge its cleaning). A numeric
// column holds the Stata date (`time` false: days, %td) or datetime (`time`
// true: milliseconds, %tc) with the fraction kept, shown in `stataFmt`. Read as
// text (allstring, or a column that also holds text) a built-in id is drawn as
// Excel draws it -- four-digit years, id 22's `h` a 12-hour clock with no
// AM/PM -- right-aligned in `width`; a custom code is drawn in Stata's own
// default (%tc `08may2019 05:58:59`, %td `08may2019`, time-only `05:58:59`).
struct ExcelDateStyle {
  bool time = false;
  std::string stataFmt;
  int id = 0;  // the built-in id drawn Excel's way; 0 = drawn in stataFmt's default
  size_t width = 0;
};

ExcelDateStyle excelDateStyle(int id, const std::string& code) {
  switch (id) {
    case 14: return {false, "%tdnn/dd/CCYY", 14, 10};
    case 15: return {false, "%tddd-Mon-YY", 15, 9};
    case 16: return {false, "%tddd-Mon", 16, 6};
    case 17: return {false, "%tdMon-YY", 17, 6};
    case 18: return {true, "%tchh:MM_AM", 18, 8};
    case 19: return {true, "%tchh:MM:SS_AM", 19, 11};
    case 20: return {true, "%tchH:MM", 20, 5};
    case 21: case 46: return {true, "%tcHH:MM:SS", 21, 8};
    case 22: return {true, "%tcnn/dd/ccYY_hh:MM", 22, 16};
    case 45: case 47: return {true, "%tcMM:SS", 45, 5};
    default: break;
  }
  // DESIGN DECISION (no oracle arm): a locale id (27-36, 50-58) carries no
  // code here and reads as id 14.
  if (code.empty()) return {false, "%tdnn/dd/CCYY", 14, 10};
  // A custom code: its date letters (y, d) and clock letters (h, s) outside
  // quoted literals, escapes and [brackets]; `m` is a month or a minute and
  // decides nothing.
  bool date = false, clock = false, inQ = false, inB = false;
  for (size_t i = 0; i < code.size(); ++i) {
    const char c = code[i];
    if (c == '"') { inQ = !inQ; continue; }
    if (inQ) continue;
    if (c == '[') { inB = true; continue; }
    if (c == ']') { inB = false; continue; }
    if (inB) continue;
    if (c == '\\') { ++i; continue; }
    const char l = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (l == 'y' || l == 'd') date = true;
    if (l == 'h' || l == 's') clock = true;
  }
  if (clock && date) return {true, "%tc", 0, 0};
  if (clock) return {true, "%tcHH:MM:SS", 0, 0};
  return {false, "%td", 0, 0};
}

// The Stata milliseconds of an Excel serial. Excel counts a 29feb1900 that
// never was, so a serial before 61 is one day later than the 21916 offset
// gives: serial 0 is 31dec1899, 59.5 noon of 28feb1900, 61.25 is 01mar1900
// 06:00 (dt2.golden). Days, then seconds, then milliseconds, in that order:
// 43593.24929861111 is 1872914339399.999756 in Stata, where days*86400000
// gives ...400; the %td value is this over 86400000 (21677.2492986111065).
double excelSerialToTc(double serial) {
  return ((serial - (serial < 61.0 ? 21915.0 : 21916.0)) * 86400.0) * 1000.0;
}
double excelSerialToTd(double serial) { return excelSerialToTc(serial) / 86400000.0; }

// A date cell read as text. The clock comes from the %tc value, truncated
// like every %tc display: 23:59:59 stored as 44555.99998842592 is 23:59:58.
std::string excelDateText(double serial, const ExcelDateStyle& st) {
  static const char* const kMon[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  const int64_t ms = static_cast<int64_t>(std::floor(excelSerialToTc(serial)));
  const int64_t day =
      st.time ? floorDiv(ms, kMsPerDay) : static_cast<int64_t>(std::floor(excelSerialToTd(serial)));
  const int64_t msd = ms - floorDiv(ms, kMsPerDay) * kMsPerDay;
  const int hh = static_cast<int>(msd / 3600000), mi = static_cast<int>(msd / 60000 % 60),
            ss = static_cast<int>(msd / 1000 % 60);
  const int h12 = hh % 12 == 0 ? 12 : hh % 12;
  const char* ampm = hh < 12 ? "AM" : "PM";
  int64_t y;
  unsigned m, d;
  civilFromDays(day - kDaysTdEpochToUnix, &y, &m, &d);
  const long long yy = static_cast<long long>(y), y2 = ((yy % 100) + 100) % 100;
  char buf[64];
  switch (st.id) {
    case 14: std::snprintf(buf, sizeof buf, "%u/%u/%lld", m, d, yy); break;
    case 15: std::snprintf(buf, sizeof buf, "%u-%s-%02lld", d, kMon[m - 1], y2); break;
    case 16: std::snprintf(buf, sizeof buf, "%u-%s", d, kMon[m - 1]); break;
    case 17: std::snprintf(buf, sizeof buf, "%s-%02lld", kMon[m - 1], y2); break;
    case 18: std::snprintf(buf, sizeof buf, "%d:%02d %s", h12, mi, ampm); break;
    case 19: std::snprintf(buf, sizeof buf, "%d:%02d:%02d %s", h12, mi, ss, ampm); break;
    case 20: std::snprintf(buf, sizeof buf, "%d:%02d", hh, mi); break;
    case 21: std::snprintf(buf, sizeof buf, "%02d:%02d:%02d", hh, mi, ss); break;
    case 22: std::snprintf(buf, sizeof buf, "%u/%u/%lld %d:%02d", m, d, yy, h12, mi); break;
    case 45: std::snprintf(buf, sizeof buf, "%02d:%02d", mi, ss); break;
    default:
      if (st.stataFmt == "%td") return tdDayString(day);
      std::snprintf(buf, sizeof buf, "%02d:%02d:%02d", hh, mi, ss);
      return st.stataFmt == "%tc" ? tdDayString(day) + " " + buf : std::string(buf);
  }
  std::string t(buf);
  if (t.size() < st.width) t.insert(0, st.width - t.size(), ' ');
  return t;
}

}  // namespace

ojson runImportExcel(const std::string& pathArg,
                     const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                     SessionState& session) {
  const bool firstrow = hasFlagOpt(opts, "firstrow");
  const bool wantDescribe = hasFlagOpt(opts, "describe");
  const bool allstring = hasFlagOpt(opts, "allstring");

  // case(preserve|lower|upper). Stata's default is preserve ([D] import excel
  // p.495); this build hard-lowercased until W3a, which is case(lower) wired as
  // if it were the only rule.
  // The value takes abbreviations: `case(l)` is lower (w2ar
  // dx_imx_077_case_l_ok rc 0, log:1954).
  NameCase nameCase = NameCase::kPreserve;
  if (auto c = optStrOf(opts, "case")) {
    const std::string v = strutil::lower(*c);
    auto isPrefixOf = [&](const char* full) {
      const std::string f(full);
      return !v.empty() && v.size() <= f.size() && f.compare(0, v.size(), v) == 0;
    };
    if (isPrefixOf("preserve")) nameCase = NameCase::kPreserve;
    else if (isPrefixOf("lower")) nameCase = NameCase::kLower;
    else if (isPrefixOf("upper")) nameCase = NameCase::kUpper;
    else {
      return engineErrorBlock(
          "import excel: case(" + *c + ") is not one of preserve, lower or upper",
          "unsupported_option");
    }
  }

  xlsx::ReadOptions ro;
  ro.firstRowIsHeader = firstrow;
  // The sheet name is taken verbatim inside its quotes: `sheet("8-2 ")` names
  // a sheet whose name ends in a blank (w2ar log:1655).
  for (const auto& kv : opts) {
    if (kv.first != "sheet" || kv.second.isBool) continue;
    std::string v = kv.second.strValue;
    const size_t a = v.find_first_not_of(" \t");
    const size_t b = v.find_last_not_of(" \t");
    v = a == std::string::npos ? std::string() : v.substr(a, b - a + 1);
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
    ro.sheetName = v;
  }
  if (auto cr = optStrOf(opts, "cellrange")) {
    std::string err;
    // A range ending in `:` names no end cell: r(198) (w2ar
    // dx_imx_bad_range_syntax `cellrange(A1:)`, log:2047).
    if (!cr->empty() && cr->back() == ':') {
      return engineErrorBlock("import excel: cellrange(" + *cr + ") invalid", "unsupported_option");
    }
    if (!parseCellRangeOpt(*cr, &ro, &err)) {
      return engineErrorBlock("import excel: " + err, "unsupported_option");
    }
  }
  // `allstring("fmt")` names the display format used to render the numbers.
  // This build renders through `tostring`'s default, so an explicit format
  // would be silently ignored — say so instead.
  if (auto af = optStrOf(opts, "allstring")) {
    if (!af->empty()) {
      return engineErrorBlock(
          "import excel: allstring(" + *af +
              ") is not supported — this build's allstring renders through tostring's default "
              "format (bare `allstring` works)",
          "unsupported_option");
    }
  }

  std::string path = importSourcePath(session, pathArg, true);
  if (!cecon::native_exists(path)) {
    // StataBE: `file sub/nofile.xlsx not found` r(601), the file as typed; a
    // stem found under neither suffix is named bare (`file nofile not found`).
    return eval::errorBlock("engine_error", "file " + stripOneQuotePair(trim(pathArg)) + " not found",
                            std::nullopt, std::nullopt, std::nullopt, "r(601)");
  }

  // ---- `describe`: a pure QUERY. It must not touch the dataset, so it returns
  // before anything below can. Stored results are [D] import excel's own
  // (p.499): r(N_worksheet), r(worksheet_#), r(range_#).
  if (wantDescribe) {
    std::vector<xlsx::SheetInfo> info;
    try {
      info = xlsx::sheets(path);
    } catch (const std::exception& e) {
      return engineErrorBlock(std::string("import excel: ") + e.what(), "data_io");
    }
    session.eval.rValues.set("N_worksheet", static_cast<double>(info.size()));
    for (size_t i = 0; i < info.size(); ++i) {
      const std::string n = std::to_string(i + 1);
      session.eval.rStrings.set("worksheet_" + n, info[i].name);
      session.eval.rStrings.set("range_" + n, info[i].range);
    }
    size_t nameWidth = 5;  // "Sheet"
    // Widths count CHARACTERS (a sheet may be named in Arabic), not bytes.
    for (const auto& si : info) nameWidth = std::max(nameWidth, cecon::ident::codePointCount(si.name));
    std::string text = "\n";
    text += "Sheet" + std::string(nameWidth - 5 + 3, ' ') + "Range\n";
    text += std::string(nameWidth + 3 + 20, '-') + "\n";
    for (const auto& si : info) {
      text += si.name + std::string(nameWidth - cecon::ident::codePointCount(si.name) + 3, ' ') +
              si.range + "\n";
    }
    ojson block;
    block["type"] = "display";
    block["text"] = text;
    return block;
  }

  xlsx::Sheet sheet;
  try {
    sheet = xlsx::read(path, ro);
  } catch (const xlsx::SheetNotFound& e) {
    // `clear` drops the data before the sheet is looked up, so a missing
    // sheet leaves k=0 (w2ar dx_imx_bad_sheet 601 k=0, log:2028).
    if (hasFlagOpt(opts, "clear") && session.hasData()) (void)runClear("", session);
    // Stata's own code and message, both captured (aer_b4
    // pins_oracle_g.log:136-139: `worksheet NoSuchSheet not found`, r(601)).
    // The rc travels as an explicit `reason`, because coarseRc's file/directory
    // rule would otherwise never see a message that says "worksheet".
    return eval::errorBlock("engine_error", e.what(), std::nullopt, std::nullopt, std::nullopt,
                            "r(601)");
  } catch (const xlsx::CellRangeOutsideData& e) {
    // B4 P2: Stata's own two messages, verbatim and unprefixed, under
    // `unsupported_option` -> r(198) (oracle_q.log:74-79 column, :84-89 row).
    // W3a clamped instead, and said in its own comment that it had no capture
    // to settle it against; oracle_q is that capture. Thrown BEFORE any data is
    // installed, which is also what Stata does — it leaves c(k)==0 and _N==0
    // (:81-82, :91-92).
    return engineErrorBlock(e.what(), "unsupported_option");
  } catch (const std::exception& e) {
    return engineErrorBlock(std::string("import excel: ") + e.what(), "data_io");
  }

  size_t nCols = sheet.header.size();
  for (const auto& r : sheet.rows) nCols = std::max(nCols, r.size());
  if (nCols == 0) return engineErrorBlock("import excel: " + path + " has no cells to read");

  // BUG-425: without `firstrow`, Stata names the variables after the SHEET's own
  // column letters — A, B, ... Z, AA, AB — the ORIGINAL spreadsheet's, not the
  // window's (`cellrange(C3:D9)` gives C and D).
  //
  // With `firstrow`, the header cell is legalized with the case kept (`Value X`
  // -> ValueX, `2nd col` -> ndcol) and becomes EVERY column's label, [id]
  // included; a blank header cell gives the column letter and no label (w2ar
  // log:1702-1716, :1784). case(lower|upper) folds the names, and two that
  // fold together are r(110) before any data moves (log:1727, :1914).
  const size_t colBase = sheet.firstCol > 0 ? static_cast<size_t>(sheet.firstCol) - 1 : 0;
  std::vector<std::string> names;
  std::vector<std::string> labels;
  {
    std::set<std::string> taken;
    std::set<std::string> seenHeaders;  // legal header names, case kept, in column order
    const delimited::Case dc = nameCase == NameCase::kLower   ? delimited::Case::kLower
                               : nameCase == NameCase::kUpper ? delimited::Case::kUpper
                                                              : delimited::Case::kPreserve;
    for (size_t c = 0; c < nCols; ++c) {
      std::string raw = (firstrow && c < sheet.header.size()) ? sheet.header[c] : std::string();
      std::string nm;
      std::string label;
      if (firstrow && !raw.empty()) {
        // excel cuts the RAW header to 32 characters BEFORE it drops what a name
        // cannot hold, so `Have you heard about this program?` becomes
        // `Haveyouheardaboutthisprogra` (27 letters: the cut lands on "progra"),
        // where import delimited cleans first and keeps 28. StataBE probe sp.do
        // (scratch-r150/uni-oracle); 007's Wave 1 cleaning reads the 27-letter name.
        const std::string cut = cecon::ident::truncateCodePoints(raw, 32);
        nm = delimited::legalNames({cut}, dc).names.front();
        // legalNames' positional fallback means nothing legal was left in the
        // cell; excel's fallback is the column letter instead.
        if ((nm == "v1" || nm == "V1") && strutil::lower(raw) != "v1") nm.clear();
        label = raw;
        // A header whose legal name (case kept) repeats an EARLIER column's gets
        // the column letter too, not an error: StataBE names the second `source`
        // of 007's baseline sheet `L`, and two long headers that truncate to the
        // same 32 characters `AZ` and `BD` (T7 probe ie2.do, scratch-r150/t7-int).
        // The r(110) below stays for names that only COLLIDE once case() folds
        // them, which the w2ar oracle pins.
        if (!nm.empty()) {
          std::string kept = delimited::legalNames({cut}, delimited::Case::kPreserve).names.front();
          if (!seenHeaders.insert(kept).second) nm.clear();
        }
      }
      if (nm.empty()) {
        // The letter goes in VERBATIM: Stata's generated names are upper case
        // and case() has no say over them.
        nm = excelColumnName(colBase + c);
      }
      if (taken.count(nm)) {
        return eval::errorBlock("engine_error", "variable " + nm + " already defined",
                                std::nullopt, std::nullopt, std::nullopt, "r(110)");
      }
      taken.insert(nm);
      names.push_back(nm);
      labels.push_back(label);
    }
  }

  // Column types from the cells themselves (C16): a column holding any text
  // cell is a string, its numbers written as text; a numeric column is
  // byte/int/long when every value is an integer in range, double otherwise --
  // never float -- shown %10.0g; a column of date-formatted numbers is int
  // the Stata date or datetime its number format names (excelDateStyle;
  // integer serials under id 14 are int %tdnn/dd/CCYY, log:1624-1639); a
  // 0.00-formatted one shows %14.2f (log:1091-1104). `allstring` makes every
  // column a string: numbers at 16 significant digits, date cells drawn by
  // excelDateText (m/d/yyyy right-aligned in 10 under id 14, log:1607-1622).
  auto numText = [](double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.16g", v);
    std::string t(buf);
    if (t.rfind("0.", 0) == 0) t.erase(0, 1);
    else if (t.rfind("-0.", 0) == 0) t.erase(1, 1);
    return t;
  };
  delimited::Table table;
  table.names = names;
  table.labels = labels;
  table.types.resize(nCols);
  std::vector<std::string> formats(nCols);
  std::vector<std::vector<std::string>> cells(sheet.rows.size(), std::vector<std::string>(nCols));
  for (size_t c = 0; c < nCols; ++c) {
    bool anyText = allstring, anyNum = false, allInt = true, allDate = true, all2dec = true;
    double lo = 0.0, hi = 0.0;
    for (const auto& row : sheet.rows) {
      if (c >= row.size() || row[c].blank) continue;
      const xlsx::Cell& cell = row[c];
      if (!cell.isNumber) {
        anyText = true;
        continue;
      }
      const double v = cell.number;
      if (!anyNum) { lo = hi = v; anyNum = true; }
      lo = std::min(lo, v);
      hi = std::max(hi, v);
      if (v != std::floor(v)) allInt = false;
      if (!cell.isDate) allDate = false;
      if (cell.numFmtId != 2) all2dec = false;
    }
    if (anyText) {
      size_t width = 0;
      for (size_t r = 0; r < sheet.rows.size(); ++r) {
        const auto& row = sheet.rows[r];
        std::string t;
        if (c < row.size() && !row[c].blank) {
          const xlsx::Cell& cell = row[c];
          t = !cell.isNumber ? cell.text
              : cell.isDate  ? excelDateText(cell.number, excelDateStyle(cell.numFmtId, cell.numFmtCode))
                             : numText(cell.number);
        }
        width = std::max(width, t.size());
        cells[r][c] = std::move(t);
      }
      table.types[c].isString = true;
      table.types[c].stataType =
          width > 2045 ? std::string("strL") : "str" + std::to_string(std::max<size_t>(1, width));
      continue;
    }
    std::string type = "byte";
    std::string fmt = "%10.0g";
    if (anyNum && allDate) {
      // One format for the column: its cells' when they agree, else plain %tc
      // when any cell has a clock (dt3.golden m1422/m2214: id 14 and id 22
      // cells -> %tc) and %td otherwise. The value keeps its fraction, so a
      // column is int/long only when every value is whole (log:1624-1639).
      bool first = true, same = true, time = false;
      std::string fmt0;
      for (const auto& row : sheet.rows) {
        if (c >= row.size() || row[c].blank || !row[c].isNumber) continue;
        const ExcelDateStyle st = excelDateStyle(row[c].numFmtId, row[c].numFmtCode);
        if (first) fmt0 = st.stataFmt;
        else if (st.stataFmt != fmt0) same = false;
        first = false;
        time = time || st.time;
      }
      fmt = same ? fmt0 : (time ? "%tc" : "%td");
      bool whole = true, seen = false;
      double vlo = 0.0, vhi = 0.0;
      for (size_t r = 0; r < sheet.rows.size(); ++r) {
        const auto& row = sheet.rows[r];
        if (c < row.size() && !row[c].blank && row[c].isNumber) {
          const double v = time ? excelSerialToTc(row[c].number) : excelSerialToTd(row[c].number);
          if (v != std::floor(v)) whole = false;
          vlo = seen ? std::min(vlo, v) : v;
          vhi = seen ? std::max(vhi, v) : v;
          seen = true;
          char buf[64];
          std::snprintf(buf, sizeof(buf), "%.17g", v);
          cells[r][c] = buf;
        }
      }
      if (whole && vlo >= -32767 && vhi <= 32740) type = "int";
      else if (whole && vlo >= -2147483647.0 && vhi <= 2147483620.0) type = "long";
      else type = "double";
    } else {
      if (anyNum) {
        if (allInt && lo >= -127 && hi <= 100) type = "byte";
        else if (allInt && lo >= -32767 && hi <= 32740) type = "int";
        else if (allInt && lo >= -2147483647.0 && hi <= 2147483620.0) type = "long";
        else type = "double";
        if (all2dec) fmt = "%14.2f";
      }
      for (size_t r = 0; r < sheet.rows.size(); ++r) {
        const auto& row = sheet.rows[r];
        if (c < row.size() && !row[c].blank && row[c].isNumber) {
          char buf[64];
          std::snprintf(buf, sizeof(buf), "%.17g", row[c].number);
          cells[r][c] = buf;
        }
      }
    }
    table.types[c].isString = false;
    table.types[c].stataType = type;
    formats[c] = fmt;
  }
  table.rows = std::move(cells);
  if (auto err = delimited::stageAndAdopt(session, table, "import excel", path)) return *err;
  for (size_t c = 0; c < nCols; ++c) {
    if (!formats[c].empty()) session.varMeta[table.names[c]].format = formats[c];
  }
  session.invalidateDatastore();
  return openBlockFor(session, path);
}


}  // namespace piet::handlers
