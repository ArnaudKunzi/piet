// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/handlers.h"
#include "piet/eval/exprfns.h"  // B3(c): exprHasRunningSum

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <set>
#include <memory>
#include <regex>
#include <sstream>
#include <unordered_set>

#include "estimation_m4_internal.h"  // e(sample) T2: the esample helpers' declarations
#include "piet/cobra_runner.h"
#include "piet/column_fetch.h"
#include "piet/counters.h"  // PP-L1 use-plane split counters (runUse)
#include "piet/eval/errors.h"
#include "piet/inrange_filter.h"
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "piet/storage_type.h"  // BUG-471 T2: --store-type
#include "piet/strmutate.h"      // BUG-1326: assert's string-condition count
#include "core/exact_sum.h"  // cecon::ExactSum: summarize's accumulation (BUG-1013)
#include "estimators/posthest.h"  // fitted design atoms for named absorbed effects
#include "io/win_compat.h"  // cecon::native_exists / to_native_path (BUG-050 wide open)
#include "io/columnar.h"  // cecon::hbin_level_unescape: .dta label text (BUG-1372)
#include "piet/strutil.h"
#include "piet/translate.h"

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
const piet::eval::OptVocab& kVocabMeanProportion = piet::eval::registerOptVocab(
    {"mean", "proportion"}, {{"level"},
     {}});
// W2 estimopts (2026-09-19, BUG-539): reghdfe's `residuals[(newvar)]` and
// `noabsorb`, matched as RAW keys inside runRegress and deliberately NOT
// registered with registerOptVocab. The registry keeps one slot per command
// token and hands back the LAST token's slot, and kVocabIvRegress
// (estimation_m4.cpp) is registered on {ivregress, ivreg, ivreghdfe, reghdfe}
// -- so it IS the `reghdfe` slot, and it is the known-set runIvRegress's
// reject net reads (estimation_m4b.cpp) for ivregress/ivreg2/xtivreg2/IV
// reghdfe too. A registration here unioned `resid`/`noabsorb` into that net:
// `ivregress 2sls y (x=z), resid` passed at rc 0 with the option unread
// (review finding, attempt 2). Unregistered, the dispatcher canonicaliser
// leaves these keys exactly as typed (no member of the reghdfe set starts
// with `re` or `noa`), so every accepted spelling is listed here in full.
// reghdfe declares `RESiduals(name) RESiduals2`, floor `res`, so
// `re` is NOT one of them -- w2_smalls.log reghdfe_bare_re is r(198), which
// runRegress's own reject net answers. reghdfe declares `noAbsorb2`,
// floor `noa`, full spelling `noabsorb2`.
const std::unordered_set<std::string> kReghdfeResidSpellings = {
    "res", "resi", "resid", "residu", "residua", "residual", "residuals"};
const std::unordered_set<std::string> kReghdfeNoAbsorbSpellings = {
    "noa", "noab", "noabs", "noabso", "noabsor", "noabsorb", "noabsorb2"};
}  // namespace

namespace piet::handlers {

using ojson = nlohmann::ordered_json;

// estimation_m4.cpp: the built-in multiway VCE's reply rewrite (e(rank) = the
// clipped V's rank) and its e(kcluster)/e(cluster#) posting.
void applyBuiltinMultiwayReplyM4(ojson& result);
void postBuiltinMultiwayEM4(SessionState& session, const ojson& raw);
// Stata's ordered e() name lists after regress/areg (estimation_m4.cpp).
void postENameListsRegressM4(SessionState& session, const ojson& raw,
                             const eval::ParsedCommand& parsed, const std::string& stataCmd,
                             bool builtinMultiway, bool noConstant);

namespace {

bool fileExists(const std::string& path) { return cecon::native_exists(path); }

// A5-04: forwards to the shared strutil copy (piet/strutil.h) — kept as a
// thin wrapper (not removed) so this file's many `lower(...)` call sites
// didn't need touching. Pure dedup, no behavior change.
std::string lower(std::string s) { return strutil::lower(std::move(s)); }

std::string ext(const std::string& path) {
  auto pos = path.find_last_of('.');
  if (pos == std::string::npos) return "";
  const auto slash = path.find_last_of('/');
  if (slash != std::string::npos && pos < slash) return "";  // a dotted directory (BUG-1130)
  return lower(path.substr(pos));
}

// BUG A / hbin-sniff (W3 fuzzer stabilization, fail_024): a file's CONTENT,
// not its NAME, decides how `use` should read it. Before runSaveData's
// honest-.dta-error fix (this same milestone), `save FILE.dta` byte-copied
// the session's internal .hbin straight to a `.dta`-named path, so
// mislabeled hbin-content files with a `.dta` extension already exist from
// that broken era (and could reappear by simple user renaming-mistake even
// after the fix). Sniff the on-disk magic BEFORE the extension dispatch so
// any such file loads correctly regardless of its name — this is the SAME
// 8-byte "CECONHB1" marker src/io/columnar.cpp's Dataset::open() checks
// (kMagic there); reading it here is inspecting the file's bytes, not
// touching engine source. A genuine .dta (Stata's own magic: "<stata_dta>"
// XML-ish header, or the older binary release markers) or .csv/.tsv text
// file cannot coincidentally start with this exact ASCII sequence, so this
// never misfires on a real file of either format.
bool looksLikeHbin(const std::string& path) {
  std::ifstream f(cecon::to_native_path(path), std::ios::binary);
  if (!f) return false;
  char buf[8];
  f.read(buf, sizeof(buf));
  if (f.gcount() != static_cast<std::streamsize>(sizeof(buf))) return false;
  static const char kHbinMagic[8] = {'C', 'E', 'C', 'O', 'N', 'H', 'B', '1'};
  return std::memcmp(buf, kHbinMagic, sizeof(buf)) == 0;
}

std::string nextPath(const std::string& workdir, const std::string& prefix) {
  return mintScratchPath(workdir, prefix);  // BUG-389: one body, registered at mint
}

// PP-L2A / K1a escape hatch (the C1/C2/C4 convention: one env var that restores
// the pre-optimization path verbatim, so every gate gets an A/B arm and the
// merge gets its seeded-RED). Per-call getenv on purpose — the selftest legs
// setenv/unsetenv around single dispatches, which a static cache would ignore.
bool useConvCacheDisabled() {
  const char* e = std::getenv("PIET_NO_USE_CACHE");
  return e && *e;
}

// PP-L2A / K1a: the entire cache guard, in one string — inode, size, and mtime
// (nanoseconds where the platform carries them) of the SOURCE file. A memo is
// reused only when a fresh key is byte-identical to the key measured when the
// memo was written; a single moved field means "convert".
//
// Returns "" on ANY stat failure, and every caller reads "" as "not cacheable,
// take the disk path" — the guard is never silently weakened to a subset of its
// fields, and an unreadable source still reaches the ordinary code that reports
// the honest error.
//
// st_ino is 0 on toolchains whose stat shim carries no real inode (the Windows
// CRT); the key there degrades to (0, size, mtime) — one field lighter, but
// still a guard, and in the only direction that is safe: a dropped field can
// turn a would-be hit into a miss (an extra conversion), never a mismatch into
// a hit (stale bytes).
std::string useCacheStatKey(const std::string& path) {
#ifdef _WIN32
  // Narrow ::stat would decode a Korean path through CP949 and miss the file
  // entirely (BUG-050), turning every `use` into a cache miss. native_file_stamp
  // is the wide equivalent; it carries no inode and no sub-second mtime, so the
  // key degrades to (0, size, mtime, 0) — exactly the safe direction described
  // above (more misses, never a stale hit).
  std::int64_t stampSize = 0;
  std::int64_t stampMtimeNs = 0;
  if (!cecon::native_file_stamp(path, stampSize, stampMtimeNs)) return {};
  const unsigned long long ino = 0;
  const long long size = static_cast<long long>(stampSize);
  const long long mtime = static_cast<long long>(stampMtimeNs / 1000000000LL);
  const long long nsec = 0;
#else
  struct stat st;
  if (::stat(cecon::to_native_path(path).c_str(), &st) != 0) return {};
  long long nsec = 0;
#if defined(__APPLE__)
  nsec = static_cast<long long>(st.st_mtimespec.tv_nsec);
#elif defined(__linux__)
  nsec = static_cast<long long>(st.st_mtim.tv_nsec);
#endif
  const unsigned long long ino = static_cast<unsigned long long>(st.st_ino);
  const long long size = static_cast<long long>(st.st_size);
  const long long mtime = static_cast<long long>(st.st_mtime);
#endif
  char buf[192];
  std::snprintf(buf, sizeof(buf), "%llu:%lld:%lld:%lld", ino, size, mtime, nsec);
  return buf;
}

ojson engineErrorBlock(const std::string& message, const std::string& code = "engine_error") {
  return eval::engineError(message, code);
}

// cobra_runner.py's `CobraError.__str__`: f"[{code}] {message}" — the
// dispatcher's universal `except CobraError as ce: ...(str(ce), code=ce.code)`
// pattern relies on this bracket-prefixed message (errors.py's error_block
// then coerces `code` to "engine_error" when it's not in the frozen
// ERROR_CODES allow-list, but the [code] prefix survives in the message
// either way — see e.g. the `[ts_no_tsset]` carve-out cells). Every
// CobraError catch site in this file must format its message through this
// helper, not ce.what() alone, to stay byte-identical with the oracle.
//
// BUG-038(a): the "[code] message" formatting itself now lives at ONE
// shared mapping point (piet::cobraErrorMessage, cobra_runner.h) that every
// such formatter across the codebase forwards to — this function stays a
// thin wrapper so its call sites in this file didn't need touching. The
// only BEHAVIOR change is cobra's `internal` code (its generic C++-
// exception catch-all, e.g. an unimplemented expression builtin): that one
// code no longer surfaces its raw "[internal] ..." shape verbatim (see
// cobra_runner.h's header comment for the two honest rewrites); every other
// code's bracket-prefixed format, and thus oracle byte-parity, is
// unchanged.
std::string cobraErrorStr(const CobraError& ce) { return cobraErrorMessage(ce); }

// BS-067 (BETA D1): `vce(cluster A B)` / legacy `cluster(A B)` naming TWO
// space-separated variables -> --vcov twoway --cluster A,B (cobra's feols
// --vcov surface accepts "twoway" as a 4th mode alongside iid/hc1/cluster —
// live-verified against the D1i reghdfe 2-way oracle: `reghdfe income
// beertax, absorb(state) vce(cluster state year)` -> beertax se=1867.524
// matches byte-for-byte through this exact shape). One var -> the
// pre-existing single-cluster path, unchanged. Three+ vars -> --vcov
// multiway --cluster A,B,C for reghdfe only (allowMultiway,
// engine-multiway-cluster3, BUG-604); every other command gets a named
// refusal (unsupported_option, rc 198). Mirrors estimation_m4.cpp's
// own splitClusterVarsM4 — kept as a separate TU-local copy (same
// established pattern as this file's lower()/cobraErrorStr() thin wrappers)
// rather than introducing a new shared header for one small helper.
struct ClusterSplitResult {
  std::vector<std::string> vcovArgv;  // {"--vcov", TYPE, "--cluster", VARS}
  bool errored = false;
  std::string errorMsg;
};

ClusterSplitResult splitClusterVars(const std::string& raw, bool allowMultiway = false) {
  ClusterSplitResult r;
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
    r.vcovArgv = {"--vcov", "cluster", "--cluster", toks[0]};
  } else if (toks.size() == 2) {
    r.vcovArgv = {"--vcov", "twoway", "--cluster", toks[0] + "," + toks[1]};
  } else if (allowMultiway) {
    std::string joined = toks[0];
    for (size_t i = 1; i < toks.size(); ++i) joined += "," + toks[i];
    r.vcovArgv = {"--vcov", "multiway", "--cluster", joined};
  } else {
    r.errored = true;
    r.errorMsg =
        "three or more cluster variables are not supported for this command in this "
        "version of PIET; multiway clustering with three or more cluster variables is "
        "implemented for reghdfe only";
  }
  return r;
}

// Stata's own refusals, carried with Stata's rc (coarseRc reads the `r(NNN)`
// reason first, so the rc is never the generic 1).
ojson stataRefusalBlock(const std::string& message, int rc) {
  return ojson(eval::errorBlock("syntax_error", message, std::nullopt, std::nullopt, std::nullopt,
                                "r(" + std::to_string(rc) + ")"));
}

// FU-117: a token after a non-cluster vce type (`vce(robust x1)`) was dropped
// and the fit ran HC1 at rc 0. w2l_vce_shape.log:407 (regress) and :764 (areg)
// are r(198) `option vce() incorrectly specified`. reghdfe/feols keep their
// accepting behaviour (w2l_vce_shape.log:4034, community command).
std::optional<ojson> vceTrailingTokenRefusal(const std::string& stataCmd, const std::string& lv,
                                             const std::string& vceRest) {
  if (vceRest.empty() || (lv != "robust" && lv != "hc1" && lv != "iid")) return std::nullopt;
  if (stataCmd == "reghdfe" || stataCmd == "feols") return std::nullopt;
  return stataRefusalBlock("option vce() incorrectly specified: `" + vceRest +
                               "` cannot follow vce(" + lv + ")",
                           198);
}

// FU-147 / FU-160: validate a cluster list before any engine call. `legacy`
// is the cluster(...) option form, `vce(cluster ...)` is the other.
//  - FU-160: legacy cluster() takes ONE variable on regress (r(198), the two
//    lines of w2l_vce_shape.log:268-275) and areg (r(103), :624-630); the
//    vce(cluster a b) forms are multiway. reghdfe/feols legacy lists stay
//    accepted (:3838). It comes after the name lookup (StataBE 19.5,
//    2026-11-03: cluster(kgr nosuch) on regress is the lookup's r(198)).
//  - FU-147: a name that is not a variable of the dataset is r(111)
//    `variable X not found` (class oracle bug483_484_control_oracle.log:121-125),
//    not the engine's rc 1.
std::optional<ojson> clusterSpecRefusal(const std::string& raw, bool legacy,
                                        const std::string& stataCmd, SessionState& session,
                                        const std::string* dataOverride) {
  std::vector<std::string> toks;
  std::istringstream iss(raw);
  std::string t;
  while (iss >> t) toks.push_back(t);
  if (toks.empty()) return std::nullopt;
  std::optional<Datastore> own;
  Datastore* ds = nullptr;
  try {
    if (dataOverride != nullptr) {
      own.emplace(*dataOverride);
      ds = &*own;
    } else {
      ds = session.datastore();
    }
  } catch (const std::exception&) {
    ds = nullptr;
  }
  // A name is looked up before the legacy count (StataBE 19.5, 2026-11-03:
  // `regress y x, cluster(kgr nosuch)` is "variable nosuch not found" r(198)).
  // The dispatcher has already put a unique abbreviation's full name in; a name
  // that begins several variables is "NAME ambiguous abbreviation". Under
  // regress's cluster() both carry "option cluster() incorrectly specified" at
  // r(198), under areg's "(error in option cluster())" at r(111); vce(cluster)
  // and reghdfe's cluster() are r(111) with no tail.
  if (ds != nullptr) {
    for (const auto& n : toks) {
      if (ds->has(n)) continue;
      int hits = 0;
      for (const auto& v : ds->names())
        if (v.size() > n.size() && v.compare(0, n.size(), n) == 0) ++hits;
      const std::string what = hits > 1 ? n + " ambiguous abbreviation" : "variable " + n + " not found";
      if (legacy && stataCmd == "regress")
        return stataRefusalBlock(what + "\noption cluster() incorrectly specified", 198);
      if (legacy && stataCmd == "areg")
        return stataRefusalBlock(what + "\n(error in option cluster())", 111);
      return stataRefusalBlock(what, 111);
    }
  }
  if (legacy && toks.size() >= 2) {
    if (stataCmd == "regress") {
      return stataRefusalBlock(
          "too many variables specified\noption cluster() incorrectly specified", 198);
    }
    if (stataCmd == "areg") {
      return stataRefusalBlock("cluster():  too many variables specified", 103);
    }
  }
  return std::nullopt;
}

// -- .dta loading (M3b): cobra 0.18.0's native `import-dta` reader ---------
//
// C++ port of routes_data.py's _convert_to_hbin (.dta branch) +
// _apply_dta_var_labels: `cobra import-dta IN.dta OUT.hbin --out-meta
// META.json` writes the hbin (value-labeled numerics stay f64 raw codes,
// genuine strings become factor columns — the reader's own doc string,
// verified: `cobra import-dta --help`) plus a metadata sidecar with
// {format_version, data_label, n_rows, variables:[{name,label,format,
// stata_type,value_label_name}], value_labels:{lblname:{code_str:text}}}.
// This mirrors the oracle's TWO-PASS design (raw read for hbin + a
// StataReader metadata pass for labels) with ONE cobra call — the metadata
// is a byproduct of the same native parse, not a second file read.
//
// import-dta's success path writes a plain-text status line to stderr and
// EMPTY stdout (verified empirically) — unlike `eval`/`feols`/etc it does
// NOT print a JSON envelope on success, so this uses runCobra() (raw) and
// inspects .ok, not runCobraJson().
struct DtaMetaResult {
  bool ok = false;
  std::string errorMessage;
  std::string errorCode;
};

DtaMetaResult applyDtaMeta(const std::string& metaPath, SessionState& session) {
  DtaMetaResult r;
  std::ifstream f(cecon::to_native_path(metaPath), std::ios::binary);
  if (!f) {
    r.errorMessage = "use: .dta metadata sidecar missing: " + metaPath;
    r.errorCode = "engine_error";
    return r;
  }
  std::ostringstream ss;
  ss << f.rdbuf();
  nlohmann::json meta;
  try {
    meta = nlohmann::json::parse(ss.str());
  } catch (const nlohmann::json::exception&) {
    r.errorMessage = "use: .dta metadata sidecar is not valid JSON: " + metaPath;
    r.errorCode = "engine_error";
    return r;
  }

  // variable metadata: label / format / stata_type -> session.varMeta;
  // value_label_name (if non-empty) -> session.varLblname (mirrors
  // _apply_dta_var_labels' session.set_var_label / _var_meta population).
  if (meta.contains("variables") && meta["variables"].is_array()) {
    for (const auto& v : meta["variables"]) {
      std::string name = v.value("name", std::string());
      if (name.empty()) continue;
      VarMeta vm;
      // BUG-1372: text that is not UTF-8 arrives escaped (cmd_import_dta).
      std::string label = v.value("label", std::string());
      cecon::hbin_level_unescape(label);
      if (!label.empty()) vm.label = label;
      std::string fmt = v.value("format", std::string());
      if (!fmt.empty()) vm.format = fmt;
      std::string stataType = v.value("stata_type", std::string());
      if (!stataType.empty()) vm.stataType = stataType;
      session.varMeta[name] = vm;
      std::string vlName = v.value("value_label_name", std::string());
      if (!vlName.empty()) session.varLblname[name] = vlName;
    }
  }

  // value_labels: {lblname: {code_str: text}} -> session.labelDefs
  // (labelDefs is keyed by STRING code — mirrors varlistWithMeta's lookup
  // against factor `levels` entries, which are strings; datastore.py's
  // describe/browse layer resolves value-labeled f64 columns via a
  // separate numeric-keyed valueLabels map for display-time substitution —
  // see below).
  if (meta.contains("value_labels") && meta["value_labels"].is_object()) {
    for (auto it = meta["value_labels"].begin(); it != meta["value_labels"].end(); ++it) {
      const std::string& lblname = it.key();
      std::map<std::string, std::string> codeMap;
      std::map<int, std::string> numericMap;
      if (it.value().is_object()) {
        for (auto cit = it.value().begin(); cit != it.value().end(); ++cit) {
          std::string text = cit.value().get<std::string>();
          cecon::hbin_level_unescape(text);
          codeMap[cit.key()] = text;
          try {
            numericMap[std::stoi(cit.key())] = text;
          } catch (const std::exception&) {
            // non-numeric code key (shouldn't happen for Stata value
            // labels) — skip the numeric-keyed side, keep the string side.
          }
        }
      }
      session.labelDefs[lblname] = codeMap;
      // Populate valueLabels for every variable currently pointing at this
      // label set (variables are already registered above, so varLblname
      // is complete by the time value_labels is processed — variables[]
      // precedes value_labels in the meta JSON's own field order, and both
      // blocks fully run before any command sees the session).
      for (const auto& [varname, lname] : session.varLblname) {
        if (lname == lblname) session.valueLabels[varname] = numericMap;
      }
    }
  }

  // data_label: dataset-level label (Stata `label data`) -> session.dataLabel.
  if (meta.contains("data_label") && meta["data_label"].is_string()) {
    session.dataLabel = meta["data_label"].get<std::string>();
    cecon::hbin_level_unescape(session.dataLabel);
  }

  // characteristics: [{varname, charname, value}, …] -> session.characteristics,
  // verbatim — exactly export-dta --in-meta's own parse (cmds_data.cpp) so the
  // sidecar this reads and the one `save` re-emits (writeDtaMetaSidecar,
  // handlers_data_verbs.cpp) agree on shape.
  if (meta.contains("characteristics") && meta["characteristics"].is_array()) {
    for (const auto& ch : meta["characteristics"]) {
      if (!ch.is_object()) continue;
      DtaCharacteristic c;
      if (ch.contains("varname") && ch["varname"].is_string())
        c.varname = ch["varname"].get<std::string>();
      if (ch.contains("charname") && ch["charname"].is_string())
        c.charname = ch["charname"].get<std::string>();
      if (ch.contains("value") && ch["value"].is_string()) {
        c.value = ch["value"].get<std::string>();
        cecon::hbin_level_unescape(c.value);
      }
      if (c.varname.empty() || c.charname.empty()) continue;
      session.characteristics.push_back(std::move(c));
    }
  }

  // [D] label language, Methods and formulas (p.669): active labels are in
  // the ordinary DTA fields; every inactive language lives in _lang_* chars.
  std::string current = "default", languages = "default";
  for (const auto& c : session.characteristics) {
    if (c.varname != "_dta") continue;
    if (c.charname == "_lang_list" && !c.value.empty()) languages = c.value;
    else if (c.charname == "_lang_c" && !c.value.empty()) current = c.value;
  }
  session.labelLangs.clear();
  std::istringstream languageStream(languages);
  for (std::string name; languageStream >> name;)
    if (std::find(session.labelLangs.begin(), session.labelLangs.end(), name) == session.labelLangs.end())
      session.labelLangs.push_back(name);
  if (session.labelLangs.empty()) session.labelLangs.push_back("default");
  session.labelLang = std::find(session.labelLangs.begin(), session.labelLangs.end(), current) != session.labelLangs.end()
                          ? current : session.labelLangs.front();
  session.labelLangStash.clear();
  for (const auto& language : session.labelLangs) {
    if (language == session.labelLang) continue;
    auto& labels = session.labelLangStash[language];
    for (const auto& c : session.characteristics) {
      if (c.charname == "_lang_v_" + language) {
        if (c.varname == "_dta") labels.dataLabel = c.value;
        else labels.varLabels[c.varname] = c.value;
      } else if (c.charname == "_lang_l_" + language && c.varname != "_dta") {
        labels.varLblname[c.varname] = c.value;
      }
    }
  }

  r.ok = true;
  return r;
}

}  // namespace

// ---------------------------------------------------------------------------
// use

ojson runUse(const std::string& filepathArg, SessionState& session) {
  std::string filepath = resolveUserPath(session, filepathArg);
  // xtset-file-persistence: clearMetadata() below drops the ts declaration with
  // the char table (Stata keeps it IN the table). A .dta states its own and is
  // re-adopted from it. A native hbin / `.tmp` has no char table: one this
  // session saved restates the declaration recorded at its `save` (BUG-659,
  // SessionState::noteCheckpointTsDecl); any other keeps the declaration live
  // before the load — the pre-existing behaviour for a native file nothing
  // here knows the declaration of.
  const std::optional<std::string> priorPanelvar = session.panelvar;
  const std::optional<std::string> priorTimevar = session.timevar;
  const std::optional<double> priorDelta = session.delta;
  auto nativeTsDecl = [&]() {
    if (session.adoptCheckpointTsDecl(filepath)) return;
    session.panelvar = priorPanelvar;
    session.timevar = priorTimevar;
    session.delta = priorDelta;
    session.writeTsChars();
  };

  // MAXPERF Stage 2 (item 2): a tempfile that was `save`d into the engine's
  // virtual-file table (save_virtual) has NO disk file — it lives in the
  // worker's RAM. Restore it from there (O(columns), virtual-first) instead of
  // the disk-open path below, which would fail fileExists(). loadFromVirtual()
  // returns false if the entry isn't live (never save_virtual'd here, or lost to
  // a worker restart), in which case we fall through: the stateless save wrote a
  // real file, so the disk path picks it up — and a truly missing tempfile hits
  // the honest "File not found" below. The success block MUST be byte-identical
  // to the disk path's (a `.tmp` use adds no honesty note — origExt==".tmp" is
  // carved out there too), so it renders through the SAME varlist card shape
  // either plane emits (VarInfo::toJson via varlistWithMeta*).
  if (session.isVirtualSave(filepath) && session.loadFromVirtual(filepath)) {
    counters::addUseVirtualHit();  // PP-L1: virtual-table restore, no disk open
    session.clearMetadata();  // a `.tmp` hbin carries no dta sidecar labels (matches disk path).
    nativeTsDecl();           // …nor a char table: the declaration rides beside it (see top).
    // BUG-198: …but it DOES carry the session's own format/label layer, parked
    // beside the virtual entry by `save` because a RAM-only save has no header
    // to write it into. The disk arm gets the identical layer back out of the
    // file's header two blocks down; this is the same data by construction
    // (both sides are built by hbinMetaPatch), so the two planes agree.
    session.seedVarMetaFromVirtualSave(filepath);
    // C4-L2B (D1 card plane, brief §1 use row + §3 reclassification): the
    // loadFromVirtual above ADOPTED the incoming frame as a dirty virtual head
    // and seeded the shadow card from the SAME CobraFrameMeta the worker just
    // reported (full column list + rows). Rendering varlist_after through
    // session.datastore() here forced an immediate EMH frame snapshot of the
    // frame we JUST received from worker RAM — a whole-file disk write whose
    // only consumer was this metadata render (a2's 10 use-closures were 100%
    // this site). The card answers the render without the disk round-trip:
    // varlistWithMetaFrom(shadowCard_) is the SAME render the mutation-card
    // fold has always emitted (session_state.cpp fold), and VarInfo::toJson
    // serializes exactly {name,dtype,format,label,n_levels,n_missing} — the
    // frame-var card carries every one of those fields byte-identically to
    // Datastore::buildVarInfo's (both leave format/label at the same defaults;
    // levels strings are F5-thinned out of the JSON on both planes). Honest
    // fallback: an un-vouched meta (rows unknown) or the escape arm
    // (PIET_NO_RESIDENT_READ=1, via residentReadCard's gate) drops to the
    // pre-D1 datastore() render verbatim — order and bytes unchanged.
    const std::vector<VarInfo>* card = session.residentReadCard();
    const std::optional<int64_t> cardRows = session.residentRowsIfCurrent();
    if (card != nullptr && cardRows.has_value()) {
      session.setCurrentFile(filepath);
      ojson out;
      out["type"] = "data_mutation";
      out["action"] = "open";
      out["source_file"] = session.currentFile();
      out["filepath"] = session.currentFile();
      ojson varlist = ojson::array();
      auto vlist = session.varlistWithMetaFrom(*card);
      for (const auto& v : vlist) varlist.push_back(v.toJson());
      session.updateNMissCache(vlist);  // keep the C2-F13a cache coherent (fold-render law)
      out["varlist_after"] = varlist;
      out["n_rows_after"] = *cardRows;
      return out;
    }
    Datastore* ds = session.datastore();  // materializes the virtual head on demand.
    session.setCurrentFile(filepath);
    ojson out;
    out["type"] = "data_mutation";
    out["action"] = "open";
    out["source_file"] = session.currentFile();
    out["filepath"] = session.currentFile();
    ojson varlist = ojson::array();
    auto vlist = session.varlistWithMeta(ds);
    for (const auto& v : vlist) varlist.push_back(v.toJson());
    session.updateNMissCache(vlist);
    out["varlist_after"] = varlist;
    out["n_rows_after"] = ds ? ds->nRows() : 0;
    return out;
  }

  // [D] use: "If filename is specified without an extension, .dta is assumed."
  // PIET's own extensionless saves are hbin files under the bare name, so the
  // bare name wins when it exists; otherwise NAME.dta is what Stata reads
  // (w2d_uncovered.log postfile_exists_noreplace: `use "w2d_pf"` after
  // `save "w2d_pf.dta"` loads it, N 1 k 1).
  const bool noExt = ext(filepath).empty();
  if (!fileExists(filepath) && noExt && fileExists(filepath + ".dta")) filepath += ".dta";
  if (!fileExists(filepath)) {
    // StataBE: `file nofile.dta not found` r(601) for `use nofile`, the name as
    // typed with the .dta it stood for (BUG-410).
    return eval::errorBlock("engine_error",
                            "file " + typedUserPath(filepathArg) + (noExt ? ".dta" : "") +
                                " not found",
                            std::nullopt, std::nullopt, std::nullopt, std::string("r(601)"));
  }

  // BUG A / hbin-sniff: content decides format, not the name — see
  // looksLikeHbin()'s header comment above. This runs BEFORE the ordinary
  // extension dispatch so a mislabeled hbin-content file (most likely a
  // `.dta`-named survivor of the earlier `save`-honesty bug, but any name
  // works) loads correctly regardless of what its extension claims.
  std::string origExt = ext(filepath);
  std::string e = origExt;
  bool hbinMismatch = false;
  if (e != ".hbin" && looksLikeHbin(filepath)) {
    hbinMismatch = true;
    e = ".hbin";
  } else if (e != ".dta" && e != ".csv" && e != ".tsv") {
    // Stata's own bytes under another name (`saveold` into a `.tmp` tempfile
    // writes a genuine .dta there, as Stata does): the XML header of 117/118,
    // or the 113-115 binary header's format byte, byte order and filetype.
    std::ifstream sn(cecon::to_native_path(filepath), std::ios::binary);
    char hb[11] = {0};
    sn.read(hb, sizeof hb);
    const std::streamsize got = sn.gcount();
    const bool xmlDta = got == 11 && std::string(hb, 11) == "<stata_dta>";
    const unsigned char b0 = static_cast<unsigned char>(hb[0]);
    const bool oldDta = got >= 3 && (b0 == 113 || b0 == 114 || b0 == 115) &&
                        (hb[1] == 1 || hb[1] == 2) && hb[2] == 1;
    if (xmlDta || oldDta) e = ".dta";
  }
  std::string fmt = (e == ".dta") ? "dta" : (e == ".hbin") ? "hbin"
                     : (e == ".csv" || e == ".tsv")        ? "csv"
                                                             : "hbin";

  std::string dtaMetaPath;  // populated by the dta branch below; consumed
                            // AFTER session.load()/clearMetadata() further down
  std::string loadPath = filepath;

  // The (inode,size,mtime) stat key of the source, measured ONCE here and shared
  // by BOTH `use` caches below (K1b stash first, then K1a conversion): a hit and
  // a miss's memo must describe the SAME observation of the source, and a second
  // stat would open a window for the file to change between them. "" => the
  // escape (PIET_NO_USE_CACHE=1) or a stat failure, which both caches read as
  // "not cacheable, take the disk path".
  const std::string useCacheKey = useConvCacheDisabled() ? std::string() : useCacheStatKey(filepath);

  // PP-L2B / K1b (PINGPONG-SCOPE-brief §2 K1b): the RESIDENT-STASH cache — the
  // layer above K1a. A source `use`d before, whose frame is still stashed in the
  // worker's RAM and that has not moved since, restores in O(columns) via
  // frameUnstash: NO conversion, NO disk open, NO whole-file re-hash — and the
  // worker is left HOLDING the frame, so the next compute verb pays no frame-use
  // either (that is the whole win; use_disk_loads drops toward 1). On a hit we
  // re-point loadPath/dtaMetaPath at the memo's byte-twin and set k1bRestored,
  // then fall through to the SHARED card/note tail below — every downstream step
  // (sidecar re-apply, datastore render) runs on the identical bytes, so
  // varlist_after / n_rows_after / any hbin-mismatch note are byte-identical to
  // the disk path by construction. A dead stash (worker restart) makes
  // residentRestore return false, and we fall through to the ordinary disk path
  // — the honest §27 tri-state fallback.
  bool k1bRestored = false;
  if (!useCacheKey.empty()) {
    const UseStashCacheEntry* smemo = session.useStashCacheGet(filepath, useCacheKey);
    if (smemo != nullptr && fileExists(smemo->hbinPath) &&
        (smemo->metaPath.empty() || fileExists(smemo->metaPath)) &&
        session.residentRestore(smemo->stashName, smemo->hbinPath, /*keepStash=*/true)) {
      counters::addUseStashHit();  // PP-L1/K1b: a use_disk_loads that did NOT happen
      loadPath = smemo->hbinPath;
      dtaMetaPath = smemo->metaPath;
      k1bRestored = true;
    }
  }

  // PP-L2A / K1a (PINGPONG-SCOPE-brief §2 K1a; PP-L1 measured dta_converts at
  // 1 PER DEPVAR against one unchanging corpus file — instruments/
  // pp_use_split.md §1): a `.dta`/`.csv` source that has not moved since this
  // session last converted it produces a byte-identical hbin, so the second and
  // later `use` of it can re-open the FIRST conversion's output read-only
  // instead of re-running import-dta and re-writing a fresh scratch hbin.
  // BUG-051 is not in scope: this is a read-only re-open of an existing path,
  // never a rewrite of a mapped file (the write only ever happens on a miss,
  // and a miss always mints a fresh scratchSeqSuffix name).
  //
  // The stat key was measured ONCE above (useCacheKey), shared with K1b: a hit
  // and the miss's memo must describe the same observation of the source. When
  // K1b already restored the resident stash, the whole convert+load path below
  // is skipped (k1bRestored) — the frame is already in RAM.
  std::string convCacheKey;       // "" => uncacheable (stat failed, or escape on)
  bool convertedThisRun = false;  // memo is written only for a real conversion
  const UseConvCacheEntry* convHit = nullptr;
  if (!k1bRestored && (fmt == "dta" || fmt == "csv")) {
    convCacheKey = useCacheKey;
    const UseConvCacheEntry* memo = session.useConvCacheGet(filepath, convCacheKey);
    // The memoed files must still be there. Eviction cannot have taken the
    // hbin (session_state.cpp's exclusion 3 pins live memos), so this check is
    // about everything OUTSIDE this session's bookkeeping — a user or an
    // external tool clearing the scratch dir mid-run. Missing => convert, which
    // is exactly what the pre-K1a code did every time anyway.
    if (memo != nullptr && fileExists(memo->hbinPath) &&
        (memo->metaPath.empty() || fileExists(memo->metaPath))) {
      convHit = memo;
    }
  }

  if (k1bRestored) {
    // K1b already restored the resident stash — loadPath/dtaMetaPath point at
    // the memo's byte-twin and the frame is in worker RAM. Skip the whole
    // convert+disk-load dispatch; the shared tail below renders the card.
  } else if (convHit != nullptr) {
    // K1a HIT: re-open the earlier conversion's output. Nothing else about the
    // load changes — session.load()/clearMetadata()/applyDtaMeta below run on
    // the identical bytes and the identical sidecar the miss path would have
    // produced, so varlist_after / n_rows_after / the label plane are
    // byte-identical by construction. counters::addDtaConvert() is deliberately
    // NOT reached (no conversion was paid — that drop IS the K1a win signal);
    // addUseDiskLoad() below still fires, because the hbin open still happens.
    loadPath = convHit->hbinPath;
    dtaMetaPath = convHit->metaPath;  // "" on the csv branch — same as a fresh csv conversion
  } else if (fmt == "dta") {
    // M3b: cobra 0.18.0's native `import-dta` reader (src/io/dta_reader.cpp,
    // formats 115/117/118) — replaces the M2 TODO stub. Mirrors the oracle's
    // routes_data.py _convert_to_hbin (.dta branch) + _apply_dta_var_labels:
    // value-labeled numerics stay f64 raw codes, genuine strings become
    // factor columns; labels captured to a --out-meta sidecar (applied to
    // SessionState AFTER session.load()/clearMetadata(), same ordering the
    // oracle uses — _apply_dta_var_labels runs after the hbin swap).
    // BUG-051 class (fixed-name variant): a FIXED conversion-output name means a
    // 2nd `use` rewrites the loaded.hbin the session still has memory-mapped —
    // on Windows the truncating reopen fails ("use: cannot write ...loaded.hbin")
    // or, on the csv branch, the failure was masked and the STALE previous
    // dataset silently reloaded. A per-use unique name (same monotonic suffix
    // mechanism as nextPath) never targets a mapped file; empty suffix on POSIX
    // keeps macOS/Linux names byte-identical. Meta sidecar shares the suffix.
    const std::string useSeq = scratchSeqSuffix();
    std::string out = session.workdir() + "/loaded" + useSeq + ".hbin";
    dtaMetaPath = session.workdir() + "/loaded_meta" + useSeq + ".json";
    registerScratchMint(out);          // BUG-389
    registerScratchMint(dtaMetaPath);  // BUG-389: the sidecar is owned only while a memo holds it
    std::vector<std::string> args = {"import-dta", filepath, out, "--out-meta", dtaMetaPath};
    counters::addDtaConvert();  // PP-L1: a full .dta→hbin conversion is being paid
    CobraRunResult cr = runCobra(args);
    if (!cr.ok) {
      // import-dta's error path prints a structured {"error":{"code",
      // "message"}} JSON to stderr (verified against cecon_cli.cpp's shared
      // error-emission helper — the SAME shape errorFromResult() in
      // cobra_runner.cpp already parses), so route through the identical
      // CobraError construction rather than hand-rolling a second parser.
      std::string stderrText = cr.stderr_text;
      while (!stderrText.empty() &&
             (stderrText.back() == '\n' || stderrText.back() == '\r' || stderrText.back() == ' ')) {
        stderrText.pop_back();
      }
      std::string code = "engine_error";
      std::string message = stderrText;
      try {
        nlohmann::json payload = nlohmann::json::parse(stderrText);
        nlohmann::json err = payload.contains("error") ? payload.at("error") : payload;
        if (err.is_object() && err.contains("message")) {
          code = err.value("code", std::string("engine_error"));
          message = err.at("message").get<std::string>();
        }
      } catch (const nlohmann::json::exception&) {
        if (message.empty()) message = "use: .dta import failed for " + filepath;
      }
      return engineErrorBlock(std::string("use: ") + message, code);
    }
    if (!fileExists(out)) {
      return engineErrorBlock("use: .dta conversion failed for " + filepath);
    }
    loadPath = out;
    convertedThisRun = true;
  } else if (fmt == "csv") {
    // Unique per-use output — same rationale as the dta branch above.
    std::string out = session.workdir() + "/loaded" + scratchSeqSuffix() + ".hbin";
    char delim = (e == ".tsv") ? '\t' : ',';
    std::vector<std::string> args = {"convert", "--data", filepath, "--out", out, "--delim",
                                      std::string(1, delim)};
    // N3a (P8.1-W2) · BUG-082: the prior `try { runCobra(args); } catch (const
    // CobraError&)` was DEAD CODE — runCobra() returns a CobraRunResult and
    // NEVER throws (CobraError is runCobraJson()'s throw type, not
    // runCobra()'s). A failed `cobra convert` (unreadable / garbage CSV) thus
    // slipped straight past the catch to the fileExists() backstop below and
    // surfaced a generic "conversion failed for <path>", swallowing cobra's
    // own diagnostic. Mirror the dta branch above exactly: capture the result
    // and, on !ok, answer as Stata does (below). The fileExists() check stays
    // as the belt-and-suspenders backstop for the (ok && no output produced)
    // case.
    CobraRunResult cr = runCobra(args);  // `convert` writes status to stderr, nothing on stdout
    if (!cr.ok) {
      // Stata's `use` reads no text file: any .csv is "file X not Stata
      // format" r(610), and the data in memory are gone by then (StataBE
      // `use f.csv, clear` leaves k 0 N 0). Reading one is PIET's own
      // extension, so a file it cannot read faithfully -- BUG-414: a row wider
      // than the names line, whose extra field the converter used to drop at
      // rc 0 -- gets the same answer and the file as typed.
      std::string shown = strutil::trim(filepathArg);
      if (shown.size() >= 2 && shown.front() == '"' && shown.back() == '"')
        shown = shown.substr(1, shown.size() - 2);
      if (session.hasData()) (void)runClear("", session);
      // Stata's words exactly; the converter's reason is not part of them.
      return ojson(eval::errorBlock("engine_error", "file " + shown + " not Stata format",
                                    std::nullopt, std::nullopt, std::nullopt, "r(610)"));
    }
    if (!fileExists(out)) {
      return engineErrorBlock("use: conversion failed for " + filepath);
    }
    loadPath = out;
    convertedThisRun = true;
  }

  if (!k1bRestored) {
    counters::addUseDiskLoad();  // PP-L1: disk-open path (any format) actually taken
    // Self-reload witness (counters.h, report-only). Narrow on purpose: only a
    // re-read of a file THIS session wrote with `save` counts, because only
    // that one is provably work done twice. A first load of a foreign file is
    // irreducible and must not be allowed to inflate it.
    if (session.isSavedPath(filepath)) counters::addDatastoreReload();
    // STALE-FRAME GUARD (SessionState::residentDropToken carries the full
    // case): a `use` that reaches disk NEVER continues the resident chain, so
    // the worker's frame must be declared out of sync BEFORE load() gets a
    // chance to read `loadPath == residentToken_` as a chain continuation.
    //
    // That used to be two statements and a comment asking the next author to
    // remember the first one. It is now ONE statement that cannot be
    // half-written: FreshRead IS the drop (SessionState::LoadIntent). Every
    // other load() caller in the tree minted its own --out and continues the
    // chain, which is the default — so this is the single site that has to say
    // anything at all, and it says it in the call.
    session.load(loadPath, SessionState::LoadIntent::FreshRead);
  }
  session.clearMetadata();
  if (!dtaMetaPath.empty()) {
    // Apply AFTER clearMetadata() — mirrors the oracle's ordering
    // (_convert_to_hbin/session.load happens, THEN _apply_dta_var_labels
    // populates var_meta/value labels fresh for the newly-loaded dataset). On a
    // K1b hit dtaMetaPath is the memo's cached sidecar — re-applied identically,
    // so the label plane matches the disk path byte-for-byte.
    DtaMetaResult mr = applyDtaMeta(dtaMetaPath, session);
    if (!mr.ok) {
      return engineErrorBlock(mr.errorMessage, mr.errorCode);
    }
  }
  // xtset-file-persistence: a .dta's char table is the declaration — adopted
  // when present, cleared when absent (w2_eststo_prefix.log:441 B1 r(459);
  // :478-500 B2 xtreg/L./chars restored). A native file: see top. A csv states
  // none, so it stays cleared (as `import delimited` does).
  if (fmt == "dta") {
    session.adoptTsFromChars();
  } else if (fmt == "hbin") {
    nativeTsDecl();
  }
  // PP-L2A / K1a: memoize AFTER the conversion has proven itself all the way
  // through load() + the sidecar apply — an output that could not be loaded or
  // whose sidecar was rejected is not a result worth handing to the next `use`.
  // After load(), too, so the memo's own predecessor (if this source was
  // converted before and has since changed) is no longer the mapped head when
  // useConvCachePut releases it to the scratch GC. (k1bRestored ⇒
  // convertedThisRun is false, so this never fires on a stash hit.)
  if (convertedThisRun && !convCacheKey.empty()) {
    session.useConvCachePut(filepath, UseConvCacheEntry{convCacheKey, loadPath, dtaMetaPath});
  }
  // PP-L2B / K1b: stash the freshly-loaded head so the NEXT `use` of this
  // unchanged source restores from worker RAM. Only on a real disk load (a stash
  // hit already has its memo); only when the source was keyable (useCacheKey).
  // useStashSnapshot returns false when the resident plane is unavailable
  // (§26-only engine / serve down) — then we simply don't memoize and the next
  // `use` pays the disk path (honest tri-state fallback). loadPath here is the
  // on-disk twin (the conversion output for .dta/.csv, the source itself for
  // .hbin) that the restore re-adopts as a clean head; dtaMetaPath rides along
  // so the label plane is re-applied on the hit.
  if (!k1bRestored && !useCacheKey.empty()) {
    const std::string stashName = piet::SessionState::useStashName(filepath);
    if (session.useStashSnapshot(stashName)) {
      session.useStashCachePut(filepath,
                               UseStashCacheEntry{useCacheKey, stashName, loadPath, dtaMetaPath});
    }
  }
  // BUG-023 FIX (2.0-scope, deliberate divergence from the frozen 1.x/piet2
  // oracle — see the internal bug ledger BUG-023 disposition): earlier
  // M2/M3 milestones bug-for-bug replicated routes_data.py:94's `use`
  // unconditionally rewriting session.cwd to the loaded file's directory.
  // Real Stata NEVER moves cwd on `use` — relative paths keep resolving
  // against the session's own working directory. This bit the analysis.do
  // acceptance directly: its tempfile+use checkpoint idiom (`use
  // "`tempfile'", clear`) dragged cwd into a temp dir, breaking every later
  // relative `graph export "result/..."`. Per the BUGLOG disposition, the
  // 2.0 core does NOT replicate this bug — session.cwd is intentionally
  // left untouched here. This is a KNOWN, pre-authorized divergence from
  // the frozen piet2 corpus's `use`-into-tempdir-adjacent cells; Eval
  // regenerates the affected corpus cells to reflect the fix (maintainer-routed).
  session.invalidateDatastore();
  Datastore* ds = session.datastore();
  // BUG-198 (load half): a native .hbin states its own display formats and
  // variable labels in the header (P1-A0 `format`, BUG-198 `label`). Adopt them
  // as this session's overrides so the layer survives use -> mutate -> save and
  // reaches every varMeta consumer (`save X.dta`'s --in-meta sidecar,
  // `tostring, usedisplayformat`, describe) — not just the render fallback that
  // reads the card directly.
  //
  // SKIPPED on the .dta/.csv branches, deliberately: `dtaMetaPath` non-empty
  // means applyDtaMeta() just populated varMeta from the SIDECAR, which is
  // strictly richer (value-label sets, declared Stata types) and is the
  // authority for that format. The hbin under it is a conversion artifact, and
  // re-seeding from it here would let the artifact overwrite the source.
  if (dtaMetaPath.empty()) session.seedVarMetaFromLoadedFile(ds);
  // P8 "Honest Cockpit" (U1): record the RESOLVED user-facing path (not
  // loadPath, which may be a scratch hbin conversion target for .dta/.csv
  // sources) now that the load has actually succeeded (ds() above would have
  // thrown on a bad hbin, so reaching here is the true success checkpoint).
  session.setCurrentFile(filepath);
  ojson out;
  out["type"] = "data_mutation";
  out["action"] = "open";
  // Additive (contract_B v2.1.0): source_file is the canonical field (mirrors
  // GET /api/varlist's source_file); filepath is the same value, kept for the
  // frontend's existing action:"open" read site. Both cover console `use`/
  // `sysuse` (blocks[]) and POST /api/open (routes.cpp's open()) since both
  // funnel through this one runUse().
  out["source_file"] = session.currentFile();
  out["filepath"] = session.currentFile();
  ojson varlist = ojson::array();
  auto vlist = session.varlistWithMeta(ds);
  for (const auto& v : vlist) varlist.push_back(v.toJson());
  session.updateNMissCache(vlist);  // C2-F13a: refresh the cache for the newly-loaded dataset
  out["varlist_after"] = varlist;
  out["n_rows_after"] = ds ? ds->nRows() : 0;
  // RED-fix (piet_2.1.0_gate VERDICT.md Leg 3, test_postfile_using_replace_
  // roundtrips#5): skip the honesty note when THIS session's own postfile
  // machinery is what wrote the hbin bytes under this exact resolved path
  // (the routine `tempfile pf` / `postfile ... using \`pf'` / `postclose` /
  // `use \`pf'` idiom) — there is nothing to explain to the user about a
  // file they didn't misname; the sniff/format-redirect above still applies
  // unchanged (content still decides how to load). A genuinely external or
  // stale-save-era mislabeled file (not in postfileWrittenPaths) still gets
  // the note below.
  //
  // BUG-085 UX residual (SE-044/045 follow-on): a `.tmp` name ALSO gets no
  // note. `.tmp` is PIET's OWN minted tempfile suffix (runTempfile,
  // handlers_m3.cpp) and — exactly as the save handler reasons
  // (handlers_data_verbs.cpp: "`.tmp` implies nothing") — a `.tmp` name makes
  // NO format claim, so there is nothing dishonest to flag: the classroom-core
  // `tempfile p` / `save "\`p'"` / `use "\`p'"` checkpoint idiom writes hbin
  // under `.tmp` and reads it straight back. This is a name-based carve-out
  // (parallel to save accepting `.tmp`), not a written-path lookup, so it also
  // covers the save-to-`.tmp` half that postfileWrittenPaths never tracks. A
  // foreign-ext misnaming (e.g. stale-era `.dta`) still gets the note.
  if (hbinMismatch && (session.isPostfileWrittenPath(filepath) || origExt == ".tmp")) {
    hbinMismatch = false;
  }
  if (hbinMismatch) {
    // Honesty note (I-3): the file's NAME said `origExt` but its CONTENT was
    // PIET's native .hbin — say so, rather than silently loading it as if
    // nothing were odd. dispatcher.cpp's `cmd == "use"` arm lifts this into
    // its own prepended echo block for the console/do-file path.
    out["note"] = "`" + filepath + "` is named `" +
                  (origExt.empty() ? "(no extension)" : origExt) +
                  "` but its content is PIET's native .hbin format (this happens with files "
                  "saved before the `.dta`-honesty fix, or any file simply misnamed) — loaded "
                  "it as .hbin. Rename it to end in `.hbin` to avoid this note next time.";
  }
  return out;
}

// ---------------------------------------------------------------------------
// gen / replace

namespace {

// Splits "NEWVAR = RHS" into {newvar, rhs} (mutate.py's manual split on the
// FIRST '='). Returns {expr,""} unsplit if there is no '='.
std::pair<std::string, std::string> splitAssign(const std::string& expr) {
  auto pos = expr.find('=');
  if (pos == std::string::npos) return {expr, ""};
  std::string lhs = expr.substr(0, pos);
  std::string rhs = expr.substr(pos + 1);
  auto trim = [](std::string s) {
    size_t a = s.find_first_not_of(" \t");
    size_t b = s.find_last_not_of(" \t");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
  };
  return {trim(lhs), trim(rhs)};
}

}  // namespace

ojson runGen(const std::string& expr, const std::optional<std::string>& ifExpr,
             SessionState& session, const std::string& storeType) {
  if (!session.currentHbin().has_value()) {
    return eval::noDataLoaded();
  }
  std::string outPath = nextPath(session.workdir(), "gen");
  std::string newvar, genExpr;
  std::string genMask;   // B3(c): see exprHasRunningSum -- sum() cannot be cond-folded
  if (ifExpr.has_value() && !ifExpr->empty()) {
    auto [nv, rhs] = splitAssign(expr);
    newvar = nv;
    if (rhs.empty()) rhs = ".";
    // Nor can a draw: the engine evaluates every argument of cond(), as Stata
    // does (BUG-1050), so a folded qualifier would draw on the rows it excludes.
    if (piet::eval::exprHasRunningSum(rhs) || translate::exprHasDrawToken(rhs)) {
      genExpr = newvar + "=" + rhs;
      genMask = *ifExpr;
    } else {
      genExpr = newvar + "=cond((" + *ifExpr + "),(" + rhs + "),.)";
    }
  } else {
    genExpr = expr;
    auto [nv, rhs] = splitAssign(expr);
    newvar = rhs.empty() && expr.find('=') == std::string::npos ? "" : nv;
  }
  // BUG C (W3 fuzzer stabilization, shared with strmutate.cpp::runStringGen
  // and handlers_data_verbs.cpp::runRename — piet::strutil::isValidStataIdentifier
  // is the ONE validator all three call): this numeric path used to ship
  // `newvar` straight to `cobra eval --gen` with no check at all, so
  // `gen 9zzz = 1` silently created a column named "9zzz" (cobra's hbin
  // header is a bare JSON string field — it has no opinion on Stata's
  // identifier rule).
  if (!newvar.empty() && !strutil::isValidStataIdentifier(newvar)) {
    return engineErrorBlock(
        "generate: invalid variable name `" + newvar + "' — " + strutil::stataIdentifierRuleText(),
        "syntax_error");
  }
  std::vector<std::string> args = {"eval", "--data", *session.currentHbin(), "--gen", genExpr,
                                    "--out", outPath};
  if (!genMask.empty()) { args.push_back("--gen-mask"); args.push_back(genMask); }
  // BUG-471 T2: the resolved storage type (the dispatcher's resolver) — the
  // engine stores float(v) / trunc(v), not the full double under a label.
  storage::appendStoreTypeArg(args, storeType);
  // BUG-369: `gen ly = L.y` / `replace y = y - L.y`. The engine resolves L./F./D./S.
  // inside an expression now, but only against tsset/xtset metadata, which does not
  // live in the .hbin — it has to ride the call, exactly as the estimator path has
  // always done (translate::injectTsContext at :1737). Gated on the expression
  // actually carrying an operator so a since-dropped timevar can't break plain gens.
  // The --gen-mask condition is parsed against the same frame, so the
  // context scan reads it too (a mask with `L.z` needs the tsset metadata).
  const std::string ctxExpr = genMask.empty() ? genExpr : genExpr + " " + genMask;
  args = translate::injectTsContextForExpr(args, ctxExpr, session);
  args = translate::injectRngContextForExpr(args, ctxExpr, session);
  // Stage 1b: opt this pure-add mutation into the resident frame plane — its
  // --out becomes the head and is never read back as a file (session.load() +
  // the mutationResult shadow card render it), so the engine advances the
  // resident frame in RAM with no per-verb scratch write.
  SessionState::ResidentEligible resident(session);
  nlohmann::json response;
  try {
    response = runCobraJson(args);
    translate::absorbRngDraws(response, session);
  } catch (const CobraError& ce) {
    // dispatcher.py's generic native_handler wrapper: `except CobraError as
    // ce: make_engine_error(str(ce), code=ce.code)` — str(ce) is the
    // bracket-prefixed "[code] message" (cobra_runner.py's CobraError.__str__).
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  session.load(outPath);
  std::vector<std::string> newVars;
  if (!newvar.empty()) newVars.push_back(newvar);
  ojson block = mutationResult(session, "gen", newVars);
  translate::carryStoreOverflow(response, block);  // BUG-1123
  return block;
}

ojson runReplace(const std::string& expr, const std::optional<std::string>& ifExpr,
                 SessionState& session, const std::string& storeType, bool noPromote) {
  if (!session.currentHbin().has_value()) {
    return eval::noDataLoaded();
  }
  auto [varname, rhs] = splitAssign(expr);
  if (rhs.empty() && expr.find('=') == std::string::npos) {
    return engineErrorBlock("replace requires `varname = expr`");
  }
  // BUG C: same shared validator as runGen (see its comment above) — an
  // invalid target name here can only mean a malformed command (a genuinely
  // EXISTING variable was always a valid identifier when it was created),
  // so this is a defensive, low-frequency check kept for symmetry with
  // gen/rename rather than a commonly-hit path.
  if (!strutil::isValidStataIdentifier(varname)) {
    return engineErrorBlock(
        "replace: invalid variable name `" + varname + "' — " + strutil::stataIdentifierRuleText(),
        "syntax_error");
  }
  // BUG-841: a running sum cannot be cond-folded here either (see runGen):
  // `replace y = sum(x) if z > 0` accumulated the unselected rows into the
  // selected rows' totals at rc 0 (w2m_bymask C09 :361 is the same shape
  // through `_n`). runBysortGen with an empty by-list is this verb in the
  // dataset's context, and it sends the `if` as --gen-mask with --gen-replace
  // and decides the store type from the masked values.
  // A draw cannot be folded either (see runGen): the excluded rows would draw.
  if (ifExpr.has_value() && !ifExpr->empty() &&
      (piet::eval::exprHasRunningSum(rhs) || translate::exprHasDrawToken(rhs))) {
    return runBysortGen(session, expr, {}, ifExpr, false, true, std::nullopt, std::nullopt,
                        storeType, noPromote);
  }
  std::string outPath = nextPath(session.workdir(), "replace");
  std::string genExpr;
  if (ifExpr.has_value() && !ifExpr->empty()) {
    genExpr = varname + "=cond((" + *ifExpr + "),(" + rhs + ")," + varname + ")";
  } else {
    genExpr = varname + "=" + rhs;
  }
  // BUG-471 T2: the type the store takes. An integer-typed variable receiving
  // a value it cannot hold widens by Stata's lattice before the ONE store
  // (replace_byte_promote :810 ... replace_int_overflow :893); the decision reads
  // the touched values through a read-only engine eval of the same expression
  // into a scratch column of a projection — no dispatch(), so r()/e() are what
  // they were. Engine calls per replace: float/double/unrecorded target 1; an
  // integer target under nopromote, or with a literal the type holds, or with an
  // unqualified literal, 1; any other integer target 2 (probe + store), 3 when
  // the head is resident (+ the column fetch).
  std::string finalStoreType = storeType;
  {
    const bool qualified = ifExpr.has_value() && !ifExpr->empty();
    const std::string probeRhs =
        qualified ? "cond((" + *ifExpr + "),(" + rhs + "),.)" : rhs;
    storage::ReplaceTypeDecision dec = storage::decideReplaceStoreType(
        session, storeType, noPromote, varname, rhs, qualified, "--gen",
        std::string(storage::kReplaceProbeVar) + "=" + probeRhs,
        {rhs, ifExpr.value_or(std::string())}, /*drawContext=*/true);
    if (dec.error.has_value()) return *dec.error;
    finalStoreType = dec.storeType;
  }
  std::vector<std::string> args = {"eval", "--data", *session.currentHbin(), "--gen", genExpr,
                                    "--out", outPath};
  storage::appendStoreTypeArg(args, finalStoreType);
  // BUG-369: `gen ly = L.y` / `replace y = y - L.y`. The engine resolves L./F./D./S.
  // inside an expression now, but only against tsset/xtset metadata, which does not
  // live in the .hbin — it has to ride the call, exactly as the estimator path has
  // always done (translate::injectTsContext at :1737). Gated on the expression
  // actually carrying an operator so a since-dropped timevar can't break plain gens.
  args = translate::injectTsContextForExpr(args, genExpr, session);
  // `replace x = runiform()` draws from the session's Stata stream like generate
  // (StataBE: after `set seed 5` it takes the stream's first draw, impl_C
  // stata_probe P8); it used to carry no stream at all, so every such replace
  // drew the same fixed Philox column whatever `set seed` said.
  args = translate::injectRngContextForExpr(args, genExpr, session);
  // Stage 1b: opt this in-place rewrite into the resident frame plane (see
  // runGen) — its --out becomes the head, never read back as a file.
  SessionState::ResidentEligible resident(session);
  try {
    translate::absorbRngDraws(runCobraJson(args), session);
  } catch (const CobraError& ce) {
    // Nothing was stored, so the label stays the type the column still holds.
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  session.load(outPath);
  storage::recordReplaceType(session, varname, storeType, finalStoreType);
  // F9: replace rewrites ONLY `varname` in place — name it so the nMiss seed
  // carries every other column instead of a full rescan. rewrittenVars is
  // the shared mutationResult's 5th (LAST) parameter, not its 3rd — pass
  // newVars={} and droppedVars={} explicitly so `varname` lands in the
  // rewrittenVars slot, not droppedVars (the consolidation hazard this
  // call site is the one real instance of: this file's old local
  // mutationResult had rewrittenVars in position 3; the shared signature's
  // position 3 is droppedVars).
  return mutationResult(session, "gen", {}, {}, {varname});
}

// ---------------------------------------------------------------------------
// describe

namespace {

// C4-L2B (D1 card plane): the per-variable describe entry, shared by the card
// plane and the datastore plane so the two renders cannot drift — every field
// except n_unique's f64 branch is metadata the resident card already carries
// (dtype vocabulary, n_missing) or session-side meta (stataType/format/label/
// val_label), and the field ORDER here is the frozen wire order.
// format-display-default-lifecycle: `fmt` is the caller's
// SessionState::displayFormat(card entry) answer, the one resolver every reader
// shares (session > the file's own > the declared type's default). describe
// used to keep its own fallback here (`isFactor ? "%9s" : "%9.0g"`), which
// answered %9.0g for a byte and %9s for a str12 (w2p_estim_ev.log:1725/:1827).
ojson describeEntry(const SessionState& session, const std::string& name, bool isFactor,
                    int64_t nMissing, int64_t nUnique, const std::string& fmt) {
  auto metaIt = session.varMeta.find(name);
  std::string stataType;
  if (metaIt != session.varMeta.end() && metaIt->second.stataType.has_value()) {
    stataType = *metaIt->second.stataType;
  } else {
    stataType = isFactor ? "str#" : "float";
  }
  std::string label;
  if (metaIt != session.varMeta.end() && metaIt->second.label.has_value()) {
    label = *metaIt->second.label;
  }

  ojson entry;
  entry["name"] = name;
  entry["dtype"] = stataType;
  entry["format"] = fmt;
  entry["label"] = label;
  entry["n_missing"] = nMissing;

  auto lblIt = session.varLblname.find(name);
  if (lblIt != session.varLblname.end()) entry["val_label"] = lblIt->second;

  entry["n_unique"] = nUnique;
  return entry;
}

}  // namespace

ojson runDescribe(const std::optional<std::vector<std::string>>& varlist, SessionState& session) {
  // C4-L2B (D1 card plane, brief §1 describe row): describe's CARD SHARE.
  // Every field the entry needs is card/session metadata EXCEPT n_unique for
  // an f64 column, which is a capped distinct-count over the column VALUES —
  // no card can answer that, so a described set containing any f64 (or a
  // factor whose level count the card's writer did not capture) falls back
  // WHOLESALE to the datastore plane below, exactly the pre-D1 path (one
  // construct, same bytes). A factor-only set answers entirely off the CURRENT
  // card: presence (a current card is authoritative for absence too — the
  // honest-currency rule), dtype, n_missing, and n_unique == n_levels (the
  // datastore plane's factor n_unique IS levels.size(), Datastore::
  // buildVarInfo, which is the same number the card carries as nLevels).
  if (const std::vector<VarInfo>* card = session.residentReadCard()) {
    std::vector<std::string> names;
    if (varlist.has_value()) {
      names = *varlist;
    } else {
      names.reserve(card->size());
      for (const auto& v : *card) names.push_back(v.name);
    }
    bool cardCanAnswer = true;
    ojson variables = ojson::array();
    for (const auto& name : names) {
      const VarInfo* vi = nullptr;
      for (const auto& v : *card) {
        if (v.name == name) {
          vi = &v;
          break;
        }
      }
      if (vi == nullptr) continue;  // authoritative absence — matches !ds->has(name)
      if (vi->dtype != "factor" || !vi->nLevels.has_value()) {
        cardCanAnswer = false;  // f64 n_unique needs column values — fall back
        break;
      }
      variables.push_back(describeEntry(session, name, /*isFactor=*/true, vi->nMissing,
                                        *vi->nLevels, session.displayFormat(*vi)));
    }
    if (cardCanAnswer) {
      ojson out;
      out["type"] = "describe";
      out["variables"] = variables;
      return out;
    }
  }

  Datastore* ds = session.datastore();
  if (ds == nullptr) return eval::noDataLoaded();

  std::vector<std::string> names = varlist.has_value() ? *varlist : ds->names();

  ojson variables = ojson::array();
  for (const auto& name : names) {
    if (!ds->has(name)) continue;
    VarInfo vi = ds->varinfo(name);

    int64_t nUnique;
    if (vi.dtype == "factor") {
      nUnique = static_cast<int64_t>(vi.levels.size());
    } else {
      // Unique-value count over non-missing f64 values, capped at 65536
      // (describe.py parity). O(n log n) via sort — acceptable for the
      // vertical-slice corpus's dataset sizes.
      const cecon::Column& c = ds->col(name);
      const double* d = c.f64();
      int64_t count = c.nbytes / static_cast<int64_t>(sizeof(double));
      std::vector<double> valid;
      valid.reserve(count);
      for (int64_t i = 0; i < count; ++i) {
        if (!std::isnan(d[i])) valid.push_back(d[i]);
      }
      std::sort(valid.begin(), valid.end());
      valid.erase(std::unique(valid.begin(), valid.end()), valid.end());
      nUnique = static_cast<int64_t>(std::min<size_t>(valid.size(), 65536));
    }
    variables.push_back(describeEntry(session, name, vi.dtype == "factor", ds->nMissing(name),
                                       nUnique, session.displayFormat(vi)));
  }

  ojson out;
  out["type"] = "describe";
  out["variables"] = variables;
  return out;
}

// ---------------------------------------------------------------------------
// summarize

namespace {

ojson numpyStats(const std::vector<double>& valid, bool detail) {
  int64_t n = static_cast<int64_t>(valid.size());
  ojson s;
  if (n == 0) {
    s["N"] = 0;
    s["mean"] = nullptr;
    s["sd"] = nullptr;
    s["min"] = nullptr;
    s["max"] = nullptr;
    // BUG-344: count and sum stay well-defined on a degenerate (all-missing) column —
    // Stata posts r(N)=0 and r(sum)=0 there, NOT missing (the same rule cmds_summary.cpp
    // already follows for by-groups). Only mean/sd/min/max read as missing.
    s["sum"] = 0.0;
    s["sum_w"] = 0.0;
    return s;
  }
  // BUG-1013: Stata's accumulation (core/exact_sum.h), the engine's own rule.
  cecon::ExactSum acc;
  for (double v : valid) acc.add(v);
  const double sum = acc.value();
  double mean = sum / static_cast<double>(n);
  double sd = 0.0;
  if (n > 1) {
    double ss = 0.0;
    for (double v : valid) ss += (v - mean) * (v - mean);
    sd = std::sqrt(ss / static_cast<double>(n - 1));
  }
  double mn = *std::min_element(valid.begin(), valid.end());
  double mx = *std::max_element(valid.begin(), valid.end());
  s["N"] = n;
  s["mean"] = mean;
  s["sd"] = sd;
  s["min"] = mn;
  s["max"] = mx;
  // BUG-344: this path already summed the column to get the mean — it just never
  // published the total, so captureRFromBlock's r(sum)/r(sum_w) mapping (which has
  // been waiting for these keys all along) found nothing to map. This fallback is
  // the UNWEIGHTED path by construction (the weighted arm errors rather than falling
  // back here), hence sum_w = N.
  s["sum"] = sum;
  s["sum_w"] = static_cast<double>(n);
  if (detail) {
    std::vector<double> sorted = valid;
    std::sort(sorted.begin(), sorted.end());
    auto pct = [&](double p) -> double {
      // numpy.percentile linear-interpolation method (matches _numpy_stats' np.percentile default).
      double idx = (p / 100.0) * static_cast<double>(sorted.size() - 1);
      int64_t lo = static_cast<int64_t>(std::floor(idx));
      int64_t hi = static_cast<int64_t>(std::ceil(idx));
      if (lo == hi) return sorted[static_cast<size_t>(lo)];
      double frac = idx - static_cast<double>(lo);
      return sorted[static_cast<size_t>(lo)] * (1.0 - frac) + sorted[static_cast<size_t>(hi)] * frac;
    };
    s["p1"] = pct(1);
    s["p25"] = pct(25);
    s["p50"] = pct(50);
    s["p75"] = pct(75);
    s["p99"] = pct(99);
  }
  return s;
}

}  // namespace

// BS-092 (BETA campaign, Phase G): the pure per-Datastore stats computation
// runSummarize below already did, hoisted into its own function so
// dispatcher.cpp's new `by:`/`bysort: summarize` grouped-dispatch loop can
// invoke the EXACT SAME computation once per by-group's own scratch slice,
// rather than re-deriving/duplicating this logic. Depends on nothing but
// `ds` itself (no SessionState, no ifExpr/inRange — those are already
// resolved into whichever Datastore the caller hands in), so it composes
// cleanly with BS-090's if/in prefilter (runSummarize resolves `ds` first,
// same as before) and BS-092's by-group slicing (dispatcher.cpp resolves
// `ds` to one group's slice) alike.
// P3 weights wave: the actual per-Datastore stats computation, parameterized
// on which cobra weight FLAG to push ("--fweight" vs "--aweight") — shared by
// summarizeCoreStats (below, [fw=] path, unchanged signature/behavior for
// freeze-preservation) and summarizeCoreStatsAweight (the new [aw=] path;
// registry.cpp's summarize row is now AweightFweight). piet/handlers.h is
// out of this batch's edit manifest (owned by a different stint) and its
// summarizeCoreStats/runSummarize declarations are frozen by existing
// callers (dispatcher.cpp's by-group summarize loop, describe's internal
// stats pull) — this helper stays file-local (not header-declared) so those
// two public signatures never change; only their SHARED body moved here.
ojson summarizeCoreStatsWeighted(Datastore& ds, const std::optional<std::vector<std::string>>& varlist,
                                  bool detail, const std::optional<std::string>& weightVar,
                                  const char* weightFlag) {
  if (varlist.has_value()) {
    for (const auto& n : *varlist) {
      if (!ds.has(n)) {
        return engineErrorBlock("variable " + n + " not found");
      }
    }
  }
  std::vector<std::string> names = varlist.has_value() ? *varlist : ds.names();
  std::vector<std::string> f64Vars, facVars;
  for (const auto& n : names) {
    if (!ds.has(n)) continue;
    (ds.dtype(n) == "f64" ? f64Vars : facVars).push_back(n);
  }

  ojson stats = ojson::object();

  if (!f64Vars.empty()) {
    std::string joined;
    for (size_t i = 0; i < f64Vars.size(); ++i) {
      if (i) joined += ",";
      joined += f64Vars[i];
    }
    std::vector<std::string> args = {"eval", "--data", ds.path(), "--summarize", joined};
    if (detail) args.push_back("--detail");
    bool hasWeight = weightVar.has_value() && !weightVar->empty();
    if (hasWeight) {
      args.push_back(weightFlag);
      args.push_back(*weightVar);
    }
    bool usedCobra = false;
    try {
      ojson out = runCobraJson(args);
      ojson raw = out.value("summarize", ojson::object());
      for (const auto& v : f64Vars) {
        if (raw.contains(v)) {
          stats[v] = raw[v];
        }
      }
      usedCobra = true;
    } catch (const CobraError& ce) {
      // gate-①: numpyStats below is UNWEIGHTED. Falling back to it silently
      // when a weight was requested would answer a different question than
      // the one asked (silent-wrong-number parity violation) — worse than
      // erroring. Only the no-weight path is allowed to use this fallback;
      // with a weight present, propagate the cobra failure instead.
      if (hasWeight) {
        return engineErrorBlock(cobraErrorStr(ce), ce.code);
      }
      usedCobra = false;
    }
    if (!usedCobra) {
      for (const auto& v : f64Vars) {
        const cecon::Column& c = ds.col(v);
        const double* d = c.f64();
        int64_t count = c.nbytes / static_cast<int64_t>(sizeof(double));
        std::vector<double> valid;
        valid.reserve(count);
        for (int64_t i = 0; i < count; ++i) {
          if (!std::isnan(d[i])) valid.push_back(d[i]);
        }
        stats[v] = numpyStats(valid, detail);
      }
    }
  }

  // A string variable has no numeric observation: Stata's summarize answers it
  // with N 0, sum_w 0 and sum 0 and every moment missing (BUG-1013; StataBE 19.5
  // w2as_summary_stats_family sx_sum_str). It used to count the non-empty
  // strings into r(N) (23 there) and post no r(sum_w)/r(sum).
  for (const auto& v : facVars) {
    ojson s;
    s["N"] = 0;
    s["mean"] = nullptr;
    s["sd"] = nullptr;
    s["min"] = nullptr;
    s["max"] = nullptr;
    s["sum"] = 0.0;
    s["sum_w"] = 0.0;
    stats[v] = s;
  }

  ojson out;
  out["type"] = "summarize";
  out["detail"] = detail;
  out["stats"] = stats;
  return out;
}

ojson summarizeCoreStats(Datastore& ds, const std::optional<std::vector<std::string>>& varlist,
                         bool detail, const std::optional<std::string>& weightVar) {
  return summarizeCoreStatsWeighted(ds, varlist, detail, weightVar, "--fweight");
}

// ONESHOT-LASTFLUSH: what `summarize VARLIST if COND` needs on disk is VARLIST plus
// whatever the condition names — never the whole frame. Filtering the head instead
// meant flushing the entire resident frame first (28 of the flushes left after the
// §38 wave, every one of them a caller that only reads a handful of columns), so
// fetch the projection and filter THAT. Returns the path the row filter should run
// against; every refusal returns the head, which is the pre-existing behavior.
//
// `summarize` with no varlist reads ds.names() — every column — so it is excluded
// by the first test rather than by hoping the scanner finds nothing.
std::string summarizeFilterBase(SessionState& session,
                                const std::optional<std::vector<std::string>>& varlist,
                                const std::optional<std::string>& weightVar,
                                const std::optional<std::string>& ifExpr) {
  const std::string head = *session.currentHbin();
  if (!varlist.has_value() || varlist->empty()) return head;
  std::string named;
  for (const auto& v : *varlist) named += v + " ";
  if (weightVar.has_value()) named += *weightVar;
  auto cols = referencedColumns(session, {&named, ifExpr.has_value() ? &*ifExpr : nullptr}, "");
  if (!cols.has_value()) return head;
  std::string projPath;
  std::unique_ptr<Datastore> proj = fetchColumns(session, *cols, session.workdir(), &projPath);
  if (!proj) return head;
  // The scanner only takes names the shadow card confirms, so a varlist entry the
  // card does not know about is simply absent from the projection. Summarizing it
  // would then report "variable not found" for a column that DOES exist — a wrong
  // answer, not a slow one. Prove the projection carries the whole request instead.
  for (const auto& v : *varlist)
    if (!proj->has(v)) return head;
  if (weightVar.has_value() && !weightVar->empty() && !proj->has(*weightVar)) return head;
  return projPath;
}

// P3 weights wave: the [aw=] sibling of summarizeCoreStats above — same
// computation, --aweight instead of --fweight. NOT declared in
// piet/handlers.h (out of this batch's edit manifest); forward-declared
// locally where dispatcher.cpp's summarize arm needs it.
ojson summarizeCoreStatsAweight(Datastore& ds, const std::optional<std::vector<std::string>>& varlist,
                                bool detail, const std::optional<std::string>& weightVar) {
  return summarizeCoreStatsWeighted(ds, varlist, detail, weightVar, "--aweight");
}

ojson runSummarize(const std::optional<std::vector<std::string>>& varlist, bool detail,
                   SessionState& session, const std::optional<std::string>& ifExpr,
                   const std::optional<std::string>& weightVar,
                   const std::optional<std::string>& inRange) {
  // `summarize ... if COND [in RANGE]`: apply the row filter FIRST (BS-090:
  // `if`/`in` composed via AND, piet::handlers::applyIfAndInFilter) into a
  // transient hbin, then summarize the filtered data with the ordinary path
  // below. This keeps the numeric (cobra --summarize) and factor/fallback
  // paths seeing the SAME restricted sample, and reuses the validated
  // keepif rather than re-deriving a row mask natively.
  std::string sumifPath;
  struct FileGuard {
    const std::string& p;
    ~FileGuard() { if (!p.empty()) cecon::native_unlink(p); }
  } guard{sumifPath};
  std::unique_ptr<Datastore> tempDs;  // destroyed before `guard` unlinks the file
  Datastore* ds = nullptr;
  bool haveIf = ifExpr.has_value() && !ifExpr->empty();
  bool haveIn = inRange.has_value() && !inRange->empty();
  if (haveIf || haveIn) {
    if (!session.currentHbin().has_value()) return eval::noDataLoaded();
    InRangeFilterResult fr;
    try {
      fr = applyIfAndInFilter(session, summarizeFilterBase(session, varlist, weightVar, ifExpr),
                              ifExpr, inRange);
    } catch (const CobraError& ce) {
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    if (!fr.ok) return inRangeRejectError(fr, *inRange, "summarize");
    sumifPath = fr.dataPath;
    tempDs = std::make_unique<Datastore>(sumifPath);
    ds = tempDs.get();
  } else {
    ds = session.datastore();
  }
  if (ds == nullptr) return eval::noDataLoaded();

  // ONESHOT-LASTFLUSH: when `ds` is the filtered temp, the stats call below reads a
  // file that is NOT the session head — exactly the case StatelessRead exists for.
  // Routed to the resident plane it frame-uses the temp, which evicts the real head
  // and snapshots it on the way out: the flush the projection above just avoided,
  // moved one call later and renamed "resident-resync". Measured that way first —
  // 28 flushes in, 28 flushes out, no win at all until this guard was added.
  std::optional<SessionState::StatelessRead> statsStateless;
  if (tempDs) statsStateless.emplace(session);
  return summarizeCoreStats(*ds, varlist, detail, weightVar);
}

// P3 weights wave: the [aw=] sibling of runSummarize above — identical if/in
// pre-materialization, routes to summarizeCoreStatsAweight instead. NOT
// declared in piet/handlers.h (out of this batch's edit manifest); forward-
// declared locally in dispatcher.cpp, which picks this vs. runSummarize off
// parsed.weightType (aw/aweight vs fw/fweight — registry.cpp's summarize row
// is AweightFweight, so those are the only two spellings that reach here).
ojson runSummarizeAweight(const std::optional<std::vector<std::string>>& varlist, bool detail,
                          SessionState& session, const std::optional<std::string>& ifExpr,
                          const std::optional<std::string>& weightVar,
                          const std::optional<std::string>& inRange, const char* weightFlag) {
  std::string sumifPath;
  struct FileGuard {
    const std::string& p;
    ~FileGuard() { if (!p.empty()) cecon::native_unlink(p); }
  } guard{sumifPath};
  std::unique_ptr<Datastore> tempDs;  // destroyed before `guard` unlinks the file
  Datastore* ds = nullptr;
  bool haveIf = ifExpr.has_value() && !ifExpr->empty();
  bool haveIn = inRange.has_value() && !inRange->empty();
  if (haveIf || haveIn) {
    if (!session.currentHbin().has_value()) return eval::noDataLoaded();
    InRangeFilterResult fr;
    try {
      fr = applyIfAndInFilter(session, summarizeFilterBase(session, varlist, weightVar, ifExpr),
                              ifExpr, inRange);
    } catch (const CobraError& ce) {
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    if (!fr.ok) return inRangeRejectError(fr, *inRange, "summarize");
    sumifPath = fr.dataPath;
    tempDs = std::make_unique<Datastore>(sumifPath);
    ds = tempDs.get();
  } else {
    ds = session.datastore();
  }
  if (ds == nullptr) return eval::noDataLoaded();

  // ONESHOT-LASTFLUSH: when `ds` is the filtered temp, the stats call below reads a
  // file that is NOT the session head — exactly the case StatelessRead exists for.
  // Routed to the resident plane it frame-uses the temp, which evicts the real head
  // and snapshots it on the way out: the flush the projection above just avoided,
  // moved one call later and renamed "resident-resync". Measured that way first —
  // 28 flushes in, 28 flushes out, no win at all until this guard was added.
  std::optional<SessionState::StatelessRead> statsStateless;
  if (tempDs) statsStateless.emplace(session);
  // `--aweight`, or `--iweight` for [iw=] (QF-SUM-IW): the same engine path.
  return summarizeCoreStatsWeighted(*ds, varlist, detail, weightVar, weightFlag);
}

// ---------------------------------------------------------------------------
// regress / reg / feols (plain OLS/feols vertical slice: iid/hc1/cluster
// vcov, optional --fe via absorb(), noconstant, level(#). IV/xtreg-RE/
// multi-outcome routing is OUT of scope — M3.)

namespace {

std::string joinComma(const std::vector<std::string>& v) {
  std::string s;
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) s += ",";
    s += v[i];
  }
  return s;
}

// BUG-102: TU-local copy of estimation_m4.cpp's substituteFactorLevelLabelM4/
// applyFactorValueLabelsM4 (handlers.cpp's cobraToEstimation is the separate
// M2/regress-only slice mentioned above — no shared header links the two
// anonymous namespaces, and strutil.h is deliberately dependency-light, so
// this follows the same duplicate-and-keep-in-sync pattern strutil.h's own
// doc comment describes for the TUs it hasn't swept yet). A factor-variable
// coefficient row's engine name is `<level>.<var>` (BUG-101, model.cpp:526),
// or `<level>.<var>#<level>.<var>` for interactions; real Stata substitutes
// the attached value label for the bare level when displaying such a row
// (verified live, StataBE 19.5). PIET's flat one-row-per-coefficient table
// keeps the ".varname" suffix (no shared group-header the way Stata's ASCII
// table has one) — only the level segment is substituted.
std::string substituteFactorLevelLabel(const std::string& name, const SessionState& session) {
  return strutil::substituteFactorLevelLabel(name, session.valueLabels);
}

void applyFactorValueLabels(ojson& coeftable, const SessionState& session) {
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
    std::string sub = substituteFactorLevelLabel(name, session);
    if (sub == name) continue;
    row["stub"] = sub;
    // bare label = the text before the first '.' of the LAST '#'-segment that changed
    std::string lastSeg = sub.substr(sub.find_last_of('#') == std::string::npos ? 0 : sub.find_last_of('#') + 1);
    row["label"] = lastSeg.substr(0, lastSeg.find('.'));
  }
}

// _cobra_to_estimation (dispatcher.py) — normalise cobra JSON -> EstimationResult.
// M2 slice: does NOT port the margins AME b=null remap (margins isn't in the
// M2 handler set) — pass-through only. Field list and truthy-check semantics
// (`if raw.get("absorbed")` / `if vcov:` — Python empty-dict/empty-string are
// falsy, so an empty `{}`/`""` is DROPPED, not passed through) mirror
// dispatcher.py's "Optional fields — standard pass-through" block exactly.
// `session` optional (default nullptr) mirrors estimation_m4.cpp's M4a copy.
ojson cobraToEstimation(const ojson& raw, const std::string& cobraCmd,
                         const SessionState* session = nullptr) {
  ojson block;
  block["type"] = "estimation";
  block["estimator"] = raw.value("estimator", cobraCmd);
  block["depvar"] = raw.value("depvar", "");
  block["n_obs"] = raw.value("n_obs", 0);
  if (raw.contains("coeftable")) block["coeftable"] = raw["coeftable"];
  if (raw.contains("coeftable") && session) applyFactorValueLabels(block["coeftable"], *session);
  if (raw.contains("outcomes")) block["outcomes"] = raw["outcomes"];

  // `if k in raw` — presence-only pass-through (no truthiness check).
  static const char* kPassthrough[] = {
      "r2", "r2_within", "sigma2", "deviance", "converged", "iters", "n_clusters",
      "cluster_dof_k", "dof_resid", "first_stage_F", "kp_wald_F", "hansen_j", "hansen_j_p",
      "F", "J", "chi2", "df1", "df2", "p_F", "p_chi2", "restrictions",
      "margin", "at", "margins", "df", "mean_1", "mean_2", "by", "coefs",
      "r", "variables", "p", "nobs", "method", "pairwise", "sig",
      "adj_r2", "rmse", "mss", "rss", "df_m", "df_r", "ll", "ll_0", "r2_pseudo", "df_chi2",
      "r2_between", "r2_overall",
      // BUG-156 (wiring half, P2-PREP): poisson/fepois `chi2_df`/`r2_p` and
      // feols single-FE `sigma_u`/`sigma_e`/`rho`/`corr_u_xb`. `chi2`/`ll`/
      // `ll_0` are already above. This is the TWIN of estimation_m4.cpp's
      // kGlm/kXtreg lists — the same allowlist exists in both files and a
      // one-sided edit makes the M2 and M4a paths publish different blocks
      // for the same fit (BUG-158/BUG-161 class). Keep them in step.
      // The engine does not emit these yet; that half of BUG-156 is compute
      // work in a parallel workstream.
      "chi2_df", "r2_p", "sigma_u", "sigma_e", "rho", "corr_u_xb",
  };
  for (const char* k : kPassthrough) {
    if (raw.contains(k)) block[k] = raw[k];
  }

  // BUG-112(8), consumption half — and this is the M2 twin of the identical
  // carry in estimation_m4.cpp::cobraToEstimationM4. `regress` routes HERE, not
  // there, so a one-sided landing would have surfaced the engine's note for
  // some estimators and silently dropped it for the most common one (measured:
  // the M4a copy alone left `regress ... ib99.g` still silent). cobra emits
  // non-fatal model-spec notes as a top-level `warnings` array (contract A,
  // absent when empty) — Stata's `note:` line. Absent-when-empty on this side
  // too, so a clean estimation block stays byte-identical.
  if (raw.contains("warnings") && raw["warnings"].is_array() && !raw["warnings"].empty()) {
    block["warnings"] = raw["warnings"];
  }

  // `if raw.get("absorbed"):` — truthy check: an empty {} is dropped.
  if (raw.contains("absorbed") && !raw["absorbed"].empty()) {
    block["absorbed"] = raw["absorbed"];
  }
  // `if vcov:` — truthy check: an empty/absent string is dropped.
  if (raw.contains("vcov_type") && raw["vcov_type"].is_string() &&
      !raw["vcov_type"].get<std::string>().empty()) {
    block["vcov_type"] = raw["vcov_type"];
  }
  return block;
}

// BUG-569 (review round): the VCE options' value SHAPE. `cluster` takes a
// variable and `robust` takes none. The VCE loop in runRegress reads a bare
// `cluster` (or an empty `cluster()`) as "no cluster variable", which pushes no
// VCE, and it skips a valued `robust(x)`. The unknown-option net passes both
// names, so either one fitted the iid model at rc 0: a silent wrong answer.
// Measured on the branch binary 2026-09-29: `regress y x1, cluster`,
// `areg y x1, a(id) cl` and `areg y x1, a(id) rob(id)` each returned rc 0 with
// e(vce) iid. Stata refuses a malformed option with r(198). The wording below is
// PIET's own; no golden pins Stata's message for these two shapes. Twin:
// estimation_m4.cpp vceOptionShapeErrorM4 (piet/handlers.h, where one shared
// copy belongs, is outside this change).
//
// An EMPTY `cluster()` is not the bare option: regress, areg and reghdfe ignore
// it and fit their default VCE at rc 0 (w2l_vce_shape_det.log:338
// regress_empty_cluster `0|regress|ols||.|180`, :695 areg, :3938 reghdfe), so
// for those words it is passed over here and the VCE loop, which skips an
// empty value, fits the default. A bare `cluster` stays r(198) everywhere.
std::optional<std::string> vceOptionShapeError(const eval::ParsedCommand& parsed,
                                               const std::string& stataCmd) {
  const bool emptyClusterIgnored =
      stataCmd == "regress" || stataCmd == "areg" || stataCmd == "reghdfe";
  for (const auto& [k, v] : parsed.opts) {
    if (k == "cluster" && !v.isBool && strutil::trim(v.strValue).empty() && emptyClusterIgnored)
      continue;
    if (k == "cluster" && (v.isBool || strutil::trim(v.strValue).empty())) {
      return std::string("option `cluster` requires a variable: cluster(varname)");
    }
    if (k == "robust" && !v.isBool) {
      return std::string("option `robust` takes no argument");
    }
  }
  return std::nullopt;
}

// reghdfe's own spelling of the conventional VCE: `vce(unadjusted)`.
// g11_fe_header_scalars.log and reghdfe_absorb_vce.log run `reghdfe y x1 x2,
// absorb(id) vce(unadjusted)` at rc 0 with the non-robust SEs (x1 .0394148);
// runRegress answered "Unknown vce type: unadjusted" r(198). EXACT token
// (case-insensitive) only, and reghdfe only: no captured arm measures an
// abbreviation, nor the spelling under regress/areg, which are left as before.
bool isReghdfeUnadjustedVce(const std::string& raw) {
  const size_t a = raw.find_first_not_of(" \t");
  if (a == std::string::npos) return false;
  const size_t z = raw.find_last_not_of(" \t");
  std::string tok = raw.substr(a, z - a + 1);
  for (auto& c : tok) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return tok == "unadjusted";
}

}  // namespace

// e(df_a) -- the absorbed degrees of freedom -- for the two absorbing words
// runRegress serves, derived from the engine's feols reply without new engine
// fields: `absorbed` ({fe name: levels}), `k_absorbed` (dof.k_fe: the rank of
// the FE dummies, redundancy removed; 1 FE = its levels, 2 FE = K1 + K2 -
// connected components), `fe_exact` (false = 3+ FE, k_fe only an upper
// bound) and `nested_fe` (the FEs nested in a cluster variable). nullopt =
// not derivable, and nothing is posted (e(df_a) stays missing). The engine's
// iv reply carries the same fields (no fe_exact), so ivreghdfe reads its
// e(df_a) here as "reghdfe" (estimation_m4_internal.h; runIvRegress).
//
//   areg:    df_a = k_absorbed - 1. areg absorbs exactly one variable and
//            keeps _cons in e(b), so one level is the constant's: w2b_estim.log
//            PREC areg_a(id)/areg_ab(id)/areg_abs(id)/areg_absorb(id): 10
//            categories -> df_a 9, df_r 48 = 60 - 2 slopes - 1 - 9. areg charges
//            every level whatever the VCE (BUG-568 keeps nested FEs in its
//            cluster dof), so nested_fe does not enter.
//   reghdfe: df_a = k_absorbed - (levels of every FE nested in the cluster),
//            reghdfe's "Categories - Redundant = Num. Coefs" column summed.
//            Redundant = the connected-components term already inside
//            k_absorbed: g11_fe_header_scalars.log absorb(id) df_a 100 (100
//            categories, 0 redundant); absorb(id t) 100 + (8 - 1) = 107;
//            aer_b4/predict-e-xbd.log:348 absorb(id f2) df_a 49 = 43 + (7 - 1).
//            A nested FE is ALL redundant: w2b_estim.log reghdfe_cluster_ctl
//            (absorb(id) vce(cluster id)): 10 categories, 10 redundant, df_a 0.
//            3+ FEs (fe_exact false): the engine charges K1 + sum(K_g - 1),
//            reghdfe's pairwise count K1 + K2 - G(1,2) + sum_g (K_g -
//            max_h G(h,g)) with G the connected components of a pair; the two
//            agree when every pair is connected, and the engine's e(df_r)
//            already rests on the same count (StataBE 19.5, BUG-1267:
//            absorb(id id2 id3) -> 30 + 7 + 5 - 2 = 40, df_r 138).
//            Not posted (no oracle arm fixes it): a nested FE alongside
//            another FE (how reghdfe then re-assigns the connected-components
//            redundancy is not measured).
std::optional<double> absorbedDofA(const ojson& raw, const std::string& stataCmd) {
  if (!raw.is_object() || !raw.contains("k_absorbed") || !raw["k_absorbed"].is_number() ||
      !raw.contains("absorbed") || !raw["absorbed"].is_object() || raw["absorbed"].empty()) {
    return std::nullopt;
  }
  const double kFe = raw["k_absorbed"].get<double>();
  const ojson& absorbed = raw["absorbed"];
  if (stataCmd == "areg") {
    if (absorbed.size() != 1 || kFe < 1.0) return std::nullopt;
    return kFe - 1.0;
  }
  if (stataCmd != "reghdfe") return std::nullopt;
  double nestedLevels = 0.0;
  size_t nNested = 0;
  if (raw.contains("nested_fe") && raw["nested_fe"].is_array()) {
    for (const auto& name : raw["nested_fe"]) {
      if (!name.is_string()) return std::nullopt;
      const std::string n = name.get<std::string>();
      if (!absorbed.contains(n) || !absorbed[n].is_number()) return std::nullopt;
      nestedLevels += absorbed[n].get<double>();
      ++nNested;
    }
  }
  // Two absorbed variables with the FIRST one nested in a cluster variable:
  // reghdfe counts all of its categories redundant and charges the second its
  // categories less the one the connected components take, which is k_absorbed
  // less the nested levels (w2s_multiway_class.log mw_hdfe_abst_c2: absorb(id t)
  // with vce(cluster id id2) -> e(df_a) 5 = 30 + 6 - 1 - 30). The other orders
  // stay unposted (no record pins them).
  if (nNested > 0 && absorbed.size() != 1) {
    if (!(nNested == 1 && absorbed.size() == 2 && raw["nested_fe"][0].is_string() &&
          absorbed.begin().key() == raw["nested_fe"][0].get<std::string>()))
      return std::nullopt;
  }
  return kFe - nestedLevels;
}

// `assert EXP [if COND] [in RANGE]` — see handlers.h for why this delegates to
// runCount instead of evaluating the expression itself.
ojson runAssert(const std::string& expr, SessionState& session,
                const std::optional<std::string>& ifExpr,
                const std::optional<std::string>& inRange) {
  if (!session.hasData()) return eval::noDataLoaded();
  // A contradiction is a row where EXP is false, i.e. 0. Wrapping in `( ... )==0`
  // — rather than splicing a negation operator into the text — keeps the user's
  // own operator precedence intact, so `assert a == 1 | b == 2` negates the WHOLE
  // claim and not just its last term. A number's `==0` is its `!`, missing
  // included; a string claim (`assert g`, g a string) is a string compared with a
  // number, which the row-wise string evaluator (runCount's BUG-032 detour) calls
  // r(109) type mismatch, as StataBE 19.5 does (BUG-412 review).
  std::string negated = "(" + expr + ")==0";
  if (ifExpr.has_value() && !ifExpr->empty()) negated += " & (" + *ifExpr + ")";

  // `assert` is not r-class -- Stata leaves every r() value in place. The count
  // below runs on an empty r() store (so a stale r(N) can never pose as the
  // contradiction count) and the caller's r() scalars and macros are put back
  // afterwards on every exit path -- an error block, and an exception out of
  // runCount (fetchColumns, a Datastore open) alike: the scope guard below puts
  // them back. Restoring the saved COPIES also restores the stores' generations
  // (eval/session.h), so a posted r(table) survives `assert` as it does in Stata
  // (w2f_residuals.log:1752 `e_rtable_index_after_assert`). This used to
  // reset r(N) to the dataset row count instead: after `summarize x1 if x1>5`
  // (r(N) 32) an `assert x1>=0` turned r(N) into 60 at rc 0, and the changed
  // r() also retired a posted r(table) (its stamp no longer matched).
  // Oracle: w2f_residuals.log:1741 `PREC|e_assert_then_rn|0|32|1|` (r(N) of
  // `summarize x1 if x1>5` is still 32 after `assert x1 >= 0`).
  struct RRestore {
    eval::EvalSession& ev;
    const eval::OrderedFloatDict values;
    const eval::OrderedDict strings;
    ~RRestore() {
      ev.rValues = values;
      ev.rStrings = strings;
    }
  };
  ojson counted;
  std::optional<double> badCount;
  // BUG-1326 / BUG-1419 / BUG-464: a claim the engine's f64 evaluator cannot read -- a
  // concatenation (`assert scalar(s1) + "x" == "-3x"`), a
  // string variable inside a function (`assert !missing(s)`), a string
  // function, bofd("cal", d) -- is counted by the row evaluator `count if`
  // detours to (dispatcher count arm, BUG-032). assert went to runCount alone,
  // so the same expression `count if` answered was rc 1 here ("string/factor
  // variable used where a number is required", "function bofd() is not
  // supported"). A claim that row evaluator cannot read either -- it has no
  // subscript, so `assert x[_n-1] < x if s == "a"` -- is counted by runCount
  // below, as it always was (StataBE rc 0; the row evaluator alone was rc 1).
  // A failure the row evaluator names Stata's code for is the answer, not a
  // decline: a string used as a truth value (`assert s`, `assert upper(s)`,
  // `assert s & 1`) is r(109) type mismatch, as in StataBE.
  if (strmutate::needsStringCount(session, negated)) {
    strmutate::CountResult cr = strmutate::countRowsIf(session, negated, inRange);
    if (cr.ok) {
      badCount = static_cast<double>(cr.n);
    } else if (cr.inRangeError) {  // Stata's r(198) sentence, as runCount words it
      InRangeFilterResult fr;
      fr.ok = false;
      fr.refusal = cr.errMsg;
      return ojson(inRangeRejectError(fr, *inRange, "assert"));
    } else if (!cr.errReason.empty()) {
      return ojson(eval::errorBlock("engine_error", cr.errMsg, std::nullopt, std::nullopt,
                                    std::nullopt, cr.errReason));
    }
  }
  if (!badCount.has_value()) {
    const RRestore restore{session.eval, session.eval.rValues, session.eval.rStrings};
    session.eval.rValues.clear();
    counted = handlers::runCount(negated, session, inRange);
    // Read before the guard puts the caller's r() back.
    if (const double* bad = session.eval.rValues.find("N")) badCount = *bad;
  }
  // runCount surfaces a bad expression as an error block; pass it through
  // verbatim rather than reporting a contradiction count we never computed.
  if (counted.is_object() && counted.value("type", std::string()) == "error") return counted;

  if (!badCount.has_value()) {
    return engineErrorBlock("assert: could not evaluate `" + expr + "`", "engine_error");
  }
  const int64_t contradictions = static_cast<int64_t>(*badCount);
  if (contradictions == 0) return ojson::array();  // Stata prints nothing on success

  Datastore* ds = session.datastore();
  const int64_t total = (ds != nullptr) ? ds->nRows() : 0;
  return eval::errorBlock(
      "assertion_false",
      std::to_string(contradictions) + " contradiction" + (contradictions == 1 ? "" : "s") +
          " in " + std::to_string(total) + " observation" + (total == 1 ? "" : "s") +
          "\nassertion is false",
      std::nullopt, std::nullopt, std::nullopt, "r(9)");
}

// e(sample) T2 (internal note esample/SPEC.md §3.1, §4.1): the fit
// handler's half of the estimation-sample mask. The engine owns every admission
// rule (if/in, listwise deletion over y/x/fe/cluster/weight, zero weights,
// TS-operator loss, singleton drops) and writes the admitted rows on request
// (`--esample-out PATH`, apps/cmds/common.cpp emit_esample: one `1`/`0` line per
// row of the --data frame, or of an N-row frame through `--esample-index COL
// --esample-n N`, plus the reply key `esample_n` = the number of 1s). These two
// helpers are the PIET side, shared by every wired fit handler: the ones outside
// this file declare them at their call site.

// The InRangeFilterResult twin that keeps the row-position column on the copy
// (inrange_filter.cpp; its header is not this task's to change).
InRangeFilterResult applyIfAndInFilterKeepingIndex(SessionState& session,
                                                   const std::string& basePath,
                                                   const std::optional<std::string>& ifExpr,
                                                   const std::optional<std::string>& inRange,
                                                   std::string* indexVar);

// Appends `--esample-out PATH` (and `--esample-index indexVar --esample-n
// headRows` when the fit reads a filtered copy carrying the position column)
// to `argv`; returns PATH, a fresh scratch name the scratch census owns until
// esampleInstallColumn unlinks it.
std::string esampleFitArgs(SessionState& session, std::vector<std::string>& argv,
                           const std::string& indexVar, int64_t headRows) {
  const std::string hb = mintScratchPath(session.workdir(), "esample");
  forgetScratchMint(hb);
  const std::string path = hb.substr(0, hb.size() - 5) + ".txt";  // 5 == strlen(".hbin")
  registerScratchMint(path);
  argv.push_back("--esample-out");
  argv.push_back(path);
  if (!indexVar.empty()) {
    argv.push_back("--esample-index");
    argv.push_back(indexVar);
    argv.push_back("--esample-n");
    argv.push_back(std::to_string(headRows));
  }
  return path;
}

// After the fit's reply was accepted: writes the mask file as the hidden f64
// column kEsampleColumn of `basePath`'s frame (`setcol --new`, which REPLACES an
// earlier fit's mask, apps/cmds/cmds_data.cpp), admitted rows holding the
// install's serial (*serialOut, LastModel::esampleSerial), and returns the new
// frame's path,
// or "" when the mask cannot be vouched for -- the reply carries no
// `esample_n`, or it is not the fit's row count (`n_rows` when the reply has it,
// i.e. under weights, where n_obs may be the sum of fw/iw weights; else n_obs),
// or the setcol failed. "" leaves the model's e(sample) a named r(198)
// (LastModel::EsampleState::kUnavailable), never a guess. The caller adopts the
// returned frame BEFORE setLastModel (the BUG-518 order: lm.dataPath must name a
// frame that carries the column) and emits no data_mutation block for it: a fit
// is not a data change in Stata's log, so `c(changed)` is restored too.
// `residentEligible`: the returned frame becomes the head and is never read as a
// file (SessionState::ResidentEligible's contract) -- true for the plain install
// on the working frame; false when basePath is a real scratch file.
namespace {
// The reply's `esample_n` handshake: the number of admitted rows the engine wrote
// must be the fit's row count. The key is erased either way -- it is not an e()
// result: the reply becomes lm.result, whose keys the e() resolver reads by name
// (`e(esample_n)` must stay `.`).
bool esampleHandshakeOk(ojson& raw) {
  if (!raw.is_object() || !raw.contains("esample_n") || !raw["esample_n"].is_number()) return false;
  const double ones = raw["esample_n"].get<double>();
  raw.erase("esample_n");
  const char* rowsKey = raw.contains("n_rows") ? "n_rows" : "n_obs";
  if (!raw.contains(rowsKey) || !raw[rowsKey].is_number()) return false;
  return ones == raw[rowsKey].get<double>();
}
}  // namespace

std::string esampleInstallColumn(SessionState& session, const std::string& valuesPath,
                                 ojson& raw, const std::string& basePath,
                                 bool residentEligible, unsigned long long* serialOut) {
  struct Unlink {
    const std::string& p;
    ~Unlink() { cecon::native_unlink(p); }
  } unlinkValues{valuesPath};
  if (!esampleHandshakeOk(raw)) return "";
  // The engine wrote 1/0; an admitted row is stored as this install's serial
  // (LastModel::esampleSerial), so a later reading can tell this fit's mask
  // from another's. setcol maps it (`--ones-as`) and refuses any cell that is
  // not 0 or 1, so a file that is not the engine's payload is never installed.
  const unsigned long long serial = session.esampleColumnInstalls + 1;
  const std::string out = nextPath(session.workdir(), "esample_install");
  // runCobra, not runCobraJson: setcol answers on stderr, with no JSON reply.
  CobraRunResult r;
  {
    std::optional<SessionState::ResidentEligible> resident;
    if (residentEligible) resident.emplace(session);
    r = runCobra({"setcol", "--data", basePath, "--var", kEsampleColumn, "--values", valuesPath,
                  "--dtype", "f64", "--new", "--ones-as", std::to_string(serial), "--out", out});
  }
  if (!r.ok) return "";
  // BUG-124's torn-write guard (the other stateless setcol sites carry it): a
  // stateless --out that is not on disk is a failed write, not a frame. A
  // resident --out is the worker's virtual frame and has no file yet.
  if (!session.residentFrameLive() && !cecon::native_exists(out)) return "";
  session.esampleColumnInstalls = serial;
  if (serialOut != nullptr) *serialOut = serial;
  return out;
}

// The whole install for a fit that read the WORKING FRAME (its --data was the
// head, with or without --keepif/--esample-index). Call after every refusal the
// handler can still make and BEFORE setLastModel, with lm.dataPath already set
// to the head the fit read. On success the head is the install frame and `lm`
// says kColumn (serial, install path); otherwise `lm` is left kUnavailable and
// the head untouched. `valuesPath` is consumed either way.
//  * The resident install advances the worker's frame in place, so a VIRTUAL
//    fit frame would exist nowhere afterwards: it is written first (a no-op on
//    a clean head; the snapshot BUG-518's pin in setLastModel took for the same
//    model before), and lm.dataPath -- which every refit replays and `estimates
//    save` records -- stays that frame, not the transient install frame
//    (BUG-470.do's `estimates use` + `predict` round trip).
//  * The fit's frame stays pinned past the move (SessionState::
//    pinPreInstallHead): a driver may hold its path around the inner fit.
//  * No block, and c(changed) is restored: a fit is not a data change.
//  * A fit on the PREVIOUS install's frame, unmoved since (SessionState::
//    esampleHeadIsInstallOf: a second regress on the same data), reads that
//    install's on-disk base plus the hidden column, so lm.dataPath names the
//    base and nothing is written. Otherwise every wired fit wrote the whole
//    working frame to disk (review 2026-10-13: 1M rows, 56 -> 240 ms per fit).
bool esampleInstallOnHead(SessionState& session, const std::string& valuesPath, ojson& raw,
                          LastModel& lm) {
  lm.esample = LastModel::EsampleState::kUnavailable;
  lm.esampleSerial = 0;
  lm.esampleInstallPath.clear();
  if (valuesPath.empty() || !session.currentHbin().has_value() ||
      *session.currentHbin() != lm.dataPath) {
    if (!valuesPath.empty()) cecon::native_unlink(valuesPath);
    if (raw.is_object()) raw.erase("esample_n");
    return false;
  }
  const std::string fitHead = lm.dataPath;
  std::string fitFrame = fitHead;  // on disk: lm.dataPath after the install
  // The engine's 0/1 file, kept (2 bytes a row) to recognize the next fit's.
  std::string body;
  {
    std::ifstream in(cecon::to_native_path(valuesPath), std::ios::binary | std::ios::ate);
    const std::streamoff size = in ? static_cast<std::streamoff>(in.tellg()) : -1;
    if (size > 0) {
      body.assign(static_cast<size_t>(size), '\0');
      in.seekg(0);
      if (!in.read(&body[0], size)) body.clear();
    }
  }
  // A fit whose --keepif names the mask (`xtreg ... if e(sample), fe`, a TS
  // regress under `if e(sample)`) read the head's mask cells under the previous
  // serial. The base has no mask column, and this install overwrites the head's,
  // so every replay of that --keepif (lincom/test/margins/predict/boottest) must
  // read the fit's own frame: no alias, lm.dataPath stays the head the fit read
  // (review 2026-10-14: lincom r(111) `unknown variable __piet_esample__`, predict
  // rc 1 `[no_obs]`).
  // A condition mask (`if strpos(...)`, piet/datastore.h kQualifierColumnPrefix) is
  // read the same way: only the fit's own frame carries it.
  const bool fitReadsMask = namesHiddenColumn(lm.fitKeepif);
  if (!fitReadsMask && session.esampleHeadIsInstallOf(session.esampleLastInstallBase) &&
      cecon::native_exists(session.esampleLastInstallBase)) {
    fitFrame = session.esampleLastInstallBase;
    // Same rows (the head is unmoved) and byte-identical admitted rows: the
    // head already carries this sample under the last install's serial, so
    // nothing is installed (a re-fit of the same model under another vce(),
    // a second regress on the same rows).
    if (!body.empty() && body == session.esampleLastInstallBody) {
      cecon::native_unlink(valuesPath);
      if (!esampleHandshakeOk(raw)) return false;
      lm.dataPath = fitFrame;
      lm.esample = LastModel::EsampleState::kColumn;
      lm.esampleSerial = session.esampleLastInstallSerial;
      lm.esampleInstallPath = fitHead;
      return true;
    }
  } else {
    session.ensureMaterializedHead("esample-install");
    if (!cecon::native_exists(fitHead)) {  // the snapshot failed: no fit frame on disk
      cecon::native_unlink(valuesPath);
      if (raw.is_object()) raw.erase("esample_n");
      return false;
    }
  }
  unsigned long long serial = 0;
  // --data is the head itself, so the resident setcol needs no resync.
  const std::string withMask =
      esampleInstallColumn(session, valuesPath, raw, fitHead, true, &serial);
  if (withMask.empty()) return false;
  const bool changed = session.datasetChanged;
  session.pinPreInstallHead();
  session.load(withMask);
  session.carryCardAcrossHiddenInstall(fitHead, withMask);
  session.datasetChanged = changed;
  session.esampleLastInstallFrame = withMask;
  session.esampleLastInstallBase = fitFrame;
  session.esampleLastInstallLoads = session.headLoadCount;
  session.esampleLastInstallSerial = serial;
  session.esampleLastInstallRows =
      static_cast<long long>(std::count(body.begin(), body.end(), '\n'));
  session.esampleLastInstallBody = std::move(body);
  lm.dataPath = fitFrame;
  lm.esample = LastModel::EsampleState::kColumn;
  lm.esampleSerial = serial;
  lm.esampleInstallPath = withMask;
  return true;
}

// ---------------------------------------------------------------------------
// e(sample) T2, the data verbs that build new observations out of old ones. The
// mask is a column, so it follows rows through every verb that only reorders,
// drops or appends them; these helpers serve the verbs whose output rows are
// COPIES or GROUPS of input rows. The rule each follows is StataBE 19.5's,
// archived by the supervisor's stata_capture job (2026-10-13, 22 PREC, 0
// PRECFAIL) at internal capture
// w2_esample_copies.log from internal script w2_esample_copies.do
// (records cited by name):
//   expand / cross: the original observation keeps its marker, every created
//     copy reads 0 (X1 `old=23|created=0`, X3b `k1=23|k2=0|k3=0`, and
//     w2_esample.log:1076 E6 `0|0|28|67|`). fillin needs nothing: its created
//     rows carry a missing cell, which reads 0 (X5 `created=0`).
//   collapse / contract with by-groups, data in by-group order: a result row
//     carries the marker of its group's FIRST observation in the current order
//     (K1, K2, K4, K6, K7 first_diff=0; K8 = w2_esample.log:1193 E10 `3`).
//     collapse without by() reads 0 (K5 `|0|1|` with the first row in).
//   merge m:1 / 1:1: no master row is copied; using-only rows carry a missing
//     cell, which reads 0 (M2 `23|62|using_only=0|old_m0_diff=0`). xpose and
//     stack replace the data and read 0 everywhere (X6 `0|4`, X7/X7b `0|122`).
//   merge 1:m and joinby: one copy of each master row keeps its marker, the
//     others read 0 (M1 `0|9|0|9|22|matched=9|not_first_of_run=0|`, X4
//     `0|19|72|m0=38|m0_diff=19|`); PIET keeps the first copy
//     (esampleKeepFirstCopy below). Which joinby copy Stata marks follows its
//     sort tie order (X4b `not_first_of_run=8`); the count does not.
//   bsample, and collapse/contract of data NOT in by-group order with a group
//     whose rows disagree: Stata's answer depends on its own random draw or
//     tie order (X2, K3 first_diff=2) -> EsampleLostBy (datastore.h).
// The archived capture is the golden aer_w2 case w2_esample_copies
// (tests/eval/golden/aer_w2/w2_esample_copies.log, landed d6613eff).

// The working frame's mask cells, or nullopt when it carries none.
std::optional<std::vector<double>> esampleHeadMask(SessionState& session) {
  if (session.esampleColumnInstalls == 0 || !session.currentHbin().has_value()) return std::nullopt;
  Datastore* ds = nullptr;
  try {
    ds = session.datastore();
  } catch (const std::exception&) {
    return std::nullopt;
  }
  if (ds == nullptr || !ds->has(kEsampleColumn)) return std::nullopt;
  const cecon::Column& c = ds->col(kEsampleColumn);
  if (c.dtype != cecon::DType::F64) return std::nullopt;
  const double* v = c.f64();
  return std::vector<double>(v, v + ds->nRows());
}

// Writes `cells` (one per row of the working frame) as its mask column and
// adopts the result as the head. Part of the verb's own data change, so no block.
// False when the engine refused; the caller then names the failure.
bool esampleRewriteHeadMask(SessionState& session, const std::vector<double>& cells) {
  if (!session.currentHbin().has_value()) return false;
  const std::string hb = mintScratchPath(session.workdir(), "esample_cells");
  forgetScratchMint(hb);
  const std::string valuesPath = hb.substr(0, hb.size() - 5) + ".txt";  // 5 == strlen(".hbin")
  registerScratchMint(valuesPath);
  struct Unlink {
    const std::string& p;
    ~Unlink() { cecon::native_unlink(p); }
  } unlinkValues{valuesPath};
  {
    std::string body;
    body.reserve(cells.size() * 3);
    char buf[32];
    for (double v : cells) {
      if (std::isnan(v)) {
        body += ".\n";
        continue;
      }
      std::snprintf(buf, sizeof buf, "%.17g\n", v);
      body += buf;
    }
    std::ofstream o(cecon::to_native_path(valuesPath), std::ios::binary | std::ios::trunc);
    if (!o) return false;
    o << body;
    o.flush();
    if (!o) return false;
  }
  session.ensureMaterializedHead("esample-rewrite");
  const std::string out = nextPath(session.workdir(), "esample_rewrite");
  CobraRunResult r;
  {
    SessionState::ResidentEligible resident(session);
    r = runCobra({"setcol", "--data", *session.currentHbin(), "--var", kEsampleColumn, "--values",
                  valuesPath, "--dtype", "f64", "--new", "--out", out});
  }
  if (!r.ok) return false;
  if (!session.residentFrameLive() && !cecon::native_exists(out)) return false;  // BUG-124
  const std::string before = *session.currentHbin();
  session.load(out);
  session.carryCardAcrossHiddenInstall(before, out);
  return true;
}

bool esampleMarkHeadLost(SessionState& session, EsampleLostBy by) {
  const std::optional<std::vector<double>> cells = esampleHeadMask(session);
  if (!cells.has_value()) return true;
  return esampleRewriteHeadMask(
      session, std::vector<double>(cells->size(), -static_cast<double>(static_cast<int>(by))));
}

namespace {
// One by-group key cell: a number by its bits (-0 folded into 0, every missing
// one key), a factor by its level text (the result's dictionary is rebuilt).
std::string esampleKeyCell(const cecon::Column& c, int64_t i) {
  if (c.dtype == cecon::DType::Factor) {
    const int32_t code = c.codes()[i];
    return code < 0 || static_cast<size_t>(code) >= c.levels.size()
               ? std::string("s")
               : "s" + std::string(c.levels[static_cast<size_t>(code)]);
  }
  double v = c.f64()[i];
  if (std::isnan(v)) return "m";
  if (v == 0.0) v = 0.0;
  char buf[24];
  std::snprintf(buf, sizeof buf, "n%a", v);
  return buf;
}
// Stata's sort order of two cells of one by-variable: numbers ascending, then
// missing; strings by bytes ("" first). <0, 0, >0.
int esampleKeyCompare(const cecon::Column& c, int64_t i, int64_t j) {
  if (c.dtype == cecon::DType::Factor) {
    auto text = [&](int64_t r) -> std::string {
      const int32_t code = c.codes()[r];
      return code < 0 || static_cast<size_t>(code) >= c.levels.size()
                 ? std::string()
                 : std::string(c.levels[static_cast<size_t>(code)]);
    };
    return text(i).compare(text(j));
  }
  const double a = c.f64()[i], b = c.f64()[j];
  if (std::isnan(a) || std::isnan(b)) return std::isnan(a) - std::isnan(b);
  return a < b ? -1 : (a > b ? 1 : 0);
}
}  // namespace

EsampleGroupMarks esampleGroupMarksBefore(SessionState& session,
                                          const std::vector<std::string>& byVars,
                                          bool restricted) {
  EsampleGroupMarks g;
  const std::optional<std::vector<double>> cells = esampleHeadMask(session);
  if (!cells.has_value() || byVars.empty()) return g;  // nothing to carry (no by(): reads 0)
  g.active = true;
  Datastore* ds = session.datastore();
  std::vector<const cecon::Column*> cols;
  for (const auto& v : byVars) {
    if (!ds->has(v)) {
      g.lost = true;  // a by-list this check cannot read (abbreviation, wildcard)
      return g;
    }
    cols.push_back(&ds->col(v));
  }
  const int64_t n = ds->nRows();
  bool ordered = true;
  for (int64_t i = 1; i < n && ordered; ++i) {
    for (const cecon::Column* c : cols) {
      const int d = esampleKeyCompare(*c, i - 1, i);
      if (d < 0) break;
      if (d > 0) {
        ordered = false;
        break;
      }
    }
  }
  std::map<std::string, bool> mixed;
  for (int64_t i = 0; i < n; ++i) {
    std::string key;
    for (const cecon::Column* c : cols) {
      key += esampleKeyCell(*c, i);
      key += '\x1f';
    }
    const double v = (*cells)[static_cast<size_t>(i)];
    auto it = g.byKey.find(key);
    if (it == g.byKey.end()) {
      g.byKey.emplace(key, v);  // the group's first row in the current order
      mixed.emplace(key, false);
    } else if (!(it->second == v || (std::isnan(it->second) && std::isnan(v)))) {
      mixed[key] = true;
    }
  }
  // A group whose rows all agree has one answer under any order and any subset;
  // a mixed one is answered by its first row only when that row is first for
  // Stata too (data in by-group order) and every row of it entered the verb.
  if (!ordered || restricted) {
    for (const auto& [k, m] : mixed) {
      (void)k;
      if (m) {
        g.lost = true;
        break;
      }
    }
  }
  return g;
}

bool esampleGroupMarksAfter(SessionState& session, const EsampleGroupMarks& g,
                            const std::vector<std::string>& byVars, EsampleLostBy by) {
  if (!g.active) return true;
  Datastore* ds = session.datastore();
  if (ds == nullptr) return false;
  const int64_t n = ds->nRows();
  std::vector<double> out(static_cast<size_t>(n), 0.0);
  if (g.lost) {
    std::fill(out.begin(), out.end(), -static_cast<double>(static_cast<int>(by)));
  } else {
    std::vector<const cecon::Column*> cols;
    for (const auto& v : byVars) {
      if (!ds->has(v)) return false;
      cols.push_back(&ds->col(v));
    }
    for (int64_t i = 0; i < n; ++i) {
      std::string key;
      for (const cecon::Column* c : cols) {
        key += esampleKeyCell(*c, i);
        key += '\x1f';
      }
      const auto it = g.byKey.find(key);
      if (it != g.byKey.end()) out[static_cast<size_t>(i)] = it->second;
    }
  }
  return esampleRewriteHeadMask(session, out);
}

// FU-496 / FU-602 review: merge 1:m and joinby copy a master row once per
// matching using row. StataBE keeps the marker on ONE copy of each master row
// and reads 0 on the others, so `count if e(sample)` stays e(N)
// (w2_esample_copies.log:596 M1 `0|9|0|9|22|matched=9|not_first_of_run=0|`;
// :470 X4 `0|19|72|m0=38|m0_diff=19|`: 38 copies of the 19 marked rows, 19
// marked). merge 1:m marks the first copy in merge's output order (M1
// `not_first_of_run=0`). joinby expands the master and re-sorts the copies
// with a plain `sort`, so which copy keeps it follows
// Stata's tie order (X4b `k0=8|k1=11|not_first_of_run=8`): any one copy is a
// realization of that order, as a tied row's position after `sort` is, and
// PIET keeps the first, like merge. A copy's cell becomes 0 only when it was
// a marked 1; 0, missing (using-only rows) and a withdrawn marker's negative
// code are kept. The copies of one master row are found by key:
//  * blockLen empty (merge 1:m): the master is unique on the key, so every
//    master-origin row (non-missing cell) of a key is a copy of one row.
//  * blockLen given (joinby): the engine emits each master row's matches as one
//    consecutive run in using order (src/data/frame2.cpp Frame::joinby), of
//    blockLen[key] rows; a run's first row is the master row's first copy.
// False when the frame's key columns cannot be read; the caller then withdraws
// the marker by name.
bool esampleKeepFirstCopy(SessionState& session, const std::vector<std::string>& keys,
                          const std::map<std::string, int64_t>* blockLen) {
  std::optional<std::vector<double>> cells = esampleHeadMask(session);
  if (!cells.has_value()) return true;
  Datastore* ds = session.datastore();
  if (ds == nullptr || keys.empty()) return false;
  std::vector<const cecon::Column*> cols;
  for (const auto& k : keys) {
    if (!ds->has(k)) return false;
    cols.push_back(&ds->col(k));
  }
  const int64_t n = ds->nRows();
  auto keyOf = [&](int64_t i) {
    std::string key;
    for (const cecon::Column* c : cols) {
      key += esampleKeyCell(*c, i);
      key += '\x1f';
    }
    return key;
  };
  bool changed = false;
  auto demote = [&](int64_t i) {
    double& v = (*cells)[static_cast<size_t>(i)];
    if (v > 0) {
      v = 0.0;
      changed = true;
    }
  };
  if (blockLen == nullptr) {
    std::set<std::string> seen;
    for (int64_t i = 0; i < n; ++i) {
      if (std::isnan((*cells)[static_cast<size_t>(i)])) continue;
      if (!seen.insert(keyOf(i)).second) demote(i);
    }
  } else {
    for (int64_t i = 0; i < n;) {
      const auto it = blockLen->find(keyOf(i));
      if (it == blockLen->end() || it->second < 1 || i + it->second > n) return false;
      for (int64_t j = i + 1; j < i + it->second; ++j) demote(j);
      i += it->second;
    }
  }
  return !changed || esampleRewriteHeadMask(session, *cells);
}

// joinby's run lengths: the using rows per key, keyed as esampleKeepFirstCopy
// keys the result's rows.
std::map<std::string, int64_t> esampleUsingRunLengths(const Datastore& usingDs,
                                                      const std::vector<std::string>& keys) {
  std::map<std::string, int64_t> out;
  std::vector<const cecon::Column*> cols;
  for (const auto& k : keys) {
    if (!usingDs.has(k)) return {};
    cols.push_back(&usingDs.col(k));
  }
  for (int64_t i = 0; i < usingDs.nRows(); ++i) {
    std::string key;
    for (const cecon::Column* c : cols) {
      key += esampleKeyCell(*c, i);
      key += '\x1f';
    }
    ++out[key];
  }
  return out;
}

// FU-475 (w2_esample.log:751-775 C6_tsop_if_boundary `0|40|40|`, :782 C6b
// `10|`): Stata applies an `if` AFTER the time-series operators have read the
// full series, so `regress y L.x1 x2 if t>=3` keeps the t==3 rows (their L.x1
// is the t==2 value). A fit that reads a physically filtered copy (runRegress's
// applyIfAndInFilter route, BS-090) evaluates L.x1 on the copy instead, where
// the t==2 row is gone: e(N) 30 at rc 0. The engine's own `--keepif` keeps the
// FULL frame for a TS-bearing regressor list and applies the restriction as a
// row mask after its ts_prepass (apps/cmds/common.cpp plan_keepif, the E1
// path), so such a fit must hand the engine the working frame plus `--keepif`.
// DESIGN DECISION (one rule for every wired fit route; the iv, xtreg, newey
// and prais routes already send the full frame plus --keepif):
//  * tsFitKeepsFullFrame: regressors carry a TS operator and the restriction is
//    an `if` alone -> the engine route.
//  * tsFitRestrictionRefusal: the restriction is one the engine's --keepif
//    cannot evaluate on the full frame -- an `in` range (`_n` needs a bysort
//    context there, BS-090), an `if` naming `_n`/`_N`, or an `if` carrying a TS
//    operator of its own (the mask is evaluated without the tsset metadata) --
//    a named r(198), never the filtered copy's silently smaller sample.
// The depvar counts as much as a regressor: `regress D.y x1 x2 if t>=3` reads
// y's t==2 value for the t==3 rows (StataBE 19.5 e(N) 48, BUG-1277).
namespace {
bool regressorsCarryTs(const eval::ParsedCommand& parsed) {
  return !parsed.varlist.empty() && translate::hasTsToken(parsed.varlist);
}
}  // namespace

bool tsFitKeepsFullFrame(const eval::ParsedCommand& parsed) {
  return regressorsCarryTs(parsed) && parsed.ifExpr.has_value() && !parsed.ifExpr->empty() &&
         !(parsed.inRange.has_value() && !parsed.inRange->empty());
}

std::optional<ojson> tsFitRestrictionRefusal(const eval::ParsedCommand& parsed,
                                             const std::string& stataCmd) {
  if (!regressorsCarryTs(parsed)) return std::nullopt;
  std::string what;
  if (parsed.inRange.has_value() && !parsed.inRange->empty()) {
    what = "an `in` range";
  } else if (parsed.ifExpr.has_value() && !parsed.ifExpr->empty()) {
    static const std::regex kObsNo(R"((^|[^A-Za-z0-9_])_[nN]([^A-Za-z0-9_]|$))");
    if (std::regex_search(*parsed.ifExpr, kObsNo)) {
      what = "an `if` condition naming _n or _N";
    } else if (translate::exprHasTsToken(*parsed.ifExpr)) {
      what = "an `if` condition with its own time-series operator";
    }
  }
  if (what.empty()) return std::nullopt;
  return ojson(eval::errorBlock(
      "unsupported_option",
      stataCmd + " with time-series operators in the regressors and " + what +
          " is not supported in this version of PIET (the restriction would be applied before "
          "the operators read the full series, which changes the estimation sample).",
      std::nullopt, std::nullopt, std::nullopt, "r(198)"));
}

ojson runRegress(const eval::ParsedCommand& parsed, SessionState& session,
                  const std::string& stataCmd, const std::string* dataOverride,
                  ojson* residMutationOut) {
  if (!session.currentHbin().has_value()) {
    return eval::noDataLoaded();
  }
  if (parsed.varlist.empty()) {
    return engineErrorBlock("syntax_error: no variables specified", "syntax_error");
  }
  std::string y = parsed.varlist[0];
  std::vector<std::string> xs(parsed.varlist.begin() + 1, parsed.varlist.end());

  std::vector<std::string> argv = {"feols"};
  argv.push_back("--y");
  argv.push_back(y);
  if (!xs.empty()) {
    argv.push_back("--x");
    argv.push_back(joinComma(xs));
  } else {
    argv.push_back("--x");
    argv.push_back("(intercept)");
  }
  // BS-090 (BETA Phase G): `if`/`in` used to be forwarded here as a bare
  // `--keepif` flag straight onto cobra's `feols` CLI. That still worked
  // for if-only (feols's own --keepif runs the SAME non-bysort
  // Frame::keep_if() cobra's `eval` verb uses), but a `_n`-based `in`
  // predicate can NOT go through it (op-cobra's evaluator throws "_n
  // requires a bysort context" outside --bygen) — so the row-range
  // pre-filter below (piet::handlers::applyIfAndInFilter) now runs BEFORE
  // the cobra call and points `--data` at the already-filtered copy
  // instead, and `--keepif` is never pushed to `argv` at all (removed here,
  // see the `--data` construction near the bottom of this function).

  // vce/vcov/robust/cluster (translate.py's _vcov_from_opts, iid/hc1/cluster
  // — BS-067 (BETA D1) added twoway, see splitClusterVars below).
  std::optional<std::string> vceVal;
  bool sawRobustFlag = false;
  std::optional<std::string> clusterOpt;
  bool noConstant = false;
  std::optional<int> level;
  bool keepSingletons = false;
  // BUG-539: reghdfe's `residuals[(newvar)]`. Bare -> `_reghdfe_resid`
  // (w2_smalls.log reghdfe_bare_resid/residuals/res/residual/resi, all rc 0
  // with the variable made); `NAME(newvar)` -> that name (reghdfe_named_*).
  // Only reghdfe has the option -- the reject net below still refuses these
  // keys for regress/feols/areg, which is Stata's own r(198) there.
  bool residWanted = false;
  std::string residName;
  std::string savedFeName, savedFeVar, savedFeError;
  if (auto shapeErr = vceOptionShapeError(parsed, stataCmd)) {
    return engineErrorBlock(*shapeErr, "unsupported_option");
  }
  for (const auto& [k, v] : parsed.opts) {
    if ((k == "vce" || k == "vcov") && vceVal.has_value() && k == "vce") {
      // Stata: "option vce() specified multiple times" r(198)
      // (w2s_multiway_class.log:1510 mw_reg_vce2x).
      return stataRefusalBlock("option vce() specified multiple times", 198);
    }
    if ((k == "vce" || k == "vcov") && !vceVal.has_value()) {
      vceVal = v.isBool ? std::string() : v.strValue;
    } else if (k == "robust" && v.isBool && v.boolValue) {
      sawRobustFlag = true;
    } else if (k == "cluster") {
      clusterOpt = v.isBool ? std::string() : v.strValue;
    } else if ((k == "noconstant" || k == "nocons") && v.isBool && v.boolValue) {
      noConstant = true;
    } else if (stataCmd == "reghdfe" && kReghdfeResidSpellings.count(k)) {
      residWanted = true;
      residName = v.isBool ? std::string("_reghdfe_resid") : strutil::trim(v.strValue);
    } else if (stataCmd == "reghdfe" && kReghdfeNoAbsorbSpellings.count(k)) {
      // w2_smalls.log reghdfe_noabsorb: rc 0, _b[x] = 1.90685050483118 = the
      // constant-only OLS fit. reghdfe parses the option as
      // `noAbsorb2` and never reads it: `has_standard_fe` (:212) is decided
      // by absorb() ALONE, so the flag is consumed here and absorb() still
      // wins when both are typed. Nothing to push -- no absorb(), no --fe.
    } else if (k == "level" && !v.isBool) {
      try {
        level = std::stoi(v.strValue);
      } catch (...) {
      }
    } else if (k == "keepsingletons" && v.isBool && v.boolValue) {
      // BS-068: reghdfe's own option name — real Stata has no
      // `keepsingletons` on plain regress/feols at all (that gate is below,
      // outside this loop, where stataCmd is compared).
      keepSingletons = true;
    } else if (k == "absorb") {
      if (!v.isBool) {
        std::string fe = v.strValue;
        // space/comma -> comma-joined tokens (translate.py's --fe rule)
        std::string joined;
        std::string cur;
        auto flush = [&]() {
          if (!cur.empty()) {
            const size_t eq = cur.find('=');
            if (eq != std::string::npos) {
              const std::string name = cur.substr(0, eq), var = cur.substr(eq + 1);
              if (stataCmd != "reghdfe" || !savedFeName.empty() ||
                  !strutil::isValidStataIdentifier(name) ||
                  !strutil::isValidStataIdentifier(var)) {
                savedFeError = "absorb(): named effects require one new variable and one plain fixed effect";
              } else {
                savedFeName = name;
                savedFeVar = var;
              }
              cur = var;
            }
            if (!joined.empty()) joined += ",";
            joined += cur;
            cur.clear();
          }
        };
        for (char c : fe) {
          if (c == ' ' || c == ',') {
            flush();
          } else {
            cur += c;
          }
        }
        flush();
        if (!joined.empty()) {
          argv.push_back("--fe");
          argv.push_back(joined);
        }
      }
    }
  }

  if (!savedFeError.empty()) return engineErrorBlock(savedFeError, "unsupported_option");
  if (!savedFeName.empty()) {
    const auto it = std::find(argv.begin(), argv.end(), "--fe");
    if (dataOverride != nullptr || residMutationOut == nullptr ||
        it == argv.end() || it + 1 == argv.end() || *(it + 1) != savedFeVar ||
        translate::hasTsToken(parsed.varlist))
      return engineErrorBlock("absorb(newvar=fe) currently requires one plain fixed effect, "
                              "without a by: prefix or time-series regressors", "unsupported_option");
    Datastore* ds = session.datastore();
    if (ds != nullptr && ds->has(savedFeName))
      return ojson(eval::engineError("variable " + savedFeName + " already defined"));
    if (residWanted && savedFeName == residName)
      return ojson(eval::engineError("variable " + savedFeName + " already defined"));
  }

  // P4 / BUG-060: unknown-option rejection net. After the loop above has
  // consumed every option `regress`/`reg`/`feols`/`reghdfe` implements, any
  // leftover token is either a genuine typo/invalid option (r(198)-parity:
  // Stata's own "option X not allowed") or a real Stata option this
  // version of PIET hasn't implemented yet (honest "not yet supported" —
  // currently only `beta`, the oracle P4 example: standardized-coefficient
  // display column). Conservative by construction: this only fires on
  // option NAMES the loop above never reads at all; it never revisits a
  // value it already accepted, so it cannot false-reject anything that
  // worked before this fix.
  for (const auto& [k, v] : parsed.opts) {
    (void)v;
    if (k == "vce" || k == "vcov" || k == "robust" || k == "cluster" || k == "noconstant" ||
        k == "nocons" || k == "level") {
      continue;
    }
    // BUG-539: residuals()/noabsorb are reghdfe's alone (the two spelling
    // sets at the top of this file). Gated on the Stata word typed, like
    // keepsingletons below: `regress ..., resid` stays "option `resid` not
    // allowed".
    if (stataCmd == "reghdfe" &&
        (kReghdfeResidSpellings.count(k) || kReghdfeNoAbsorbSpellings.count(k))) {
      continue;
    }
    if (k == "absorb") {
      // BUG-473 (User ruling 2026-09-09: match Stata): plain `regress`/`reg`
      // has NO `absorb()` option in any Stata edition on this machine —
      // measured StataBE r(198), message "option absorb() not allowed"
      // (tests/eval/golden/aer_b4/b5_absorb_class.log:32-36,
      // b5_misc_pins.log:29-51 incl. the cluster()/vce()/if combinations,
      // and the `ab()` abbreviation at
      // tests/eval/golden/aer_b4/pins_oracle_p.log:103-107, same
      // message and rc). `feols`/`reghdfe`/`areg` share this handler and DO
      // have it (b5_absorb_class.log:80-99 areg rc 0, :125-151 reghdfe rc 0;
      // freeze_extra.do:41 `feols y x1 x2, absorb(id)`) — gate on the Stata
      // word actually typed, mirroring the `keepsingletons` gate below.
      if (stataCmd == "regress") {
        return engineErrorBlock("option absorb() not allowed", "unsupported_option");
      }
      continue;
    }
    if (k == "keepsingletons") {
      // BS-068: real Stata has `keepsingletons` on reghdfe only (regress/
      // feols have no such option at all) — gate on the ACTUAL Stata word
      // the user typed, not on whether this file happens to consume it.
      if (stataCmd != "reghdfe") {
        return engineErrorBlock("option `keepsingletons` not allowed", "unsupported_option");
      }
      continue;
    }
    if (k == "beta") {
      return engineErrorBlock("Option `" + k + "` is a valid Stata option that this version of "
                              "PIET does not yet implement.",
                              "unsupported_option");
    }
    return engineErrorBlock("option `" + k + "` not allowed", "unsupported_option");
  }
  // BS-068: keepsingletons parsed above regardless of stataCmd (so the
  // reject-net check just above can compare stataCmd itself), but only
  // ACTUALLY forwarded to cobra for reghdfe — matches real Stata (regress/
  // feols reaching this point with keepSingletons=true is unreachable: the
  // net above already returned for any stataCmd != "reghdfe").
  if (keepSingletons && stataCmd == "reghdfe") {
    argv.push_back("--keep-singletons");
  }

  if (vceVal.has_value()) {
    // W1b: `vce()`'s argument abbreviates -- ONE canonicaliser for all three
    // sites (piet/handlers.h `canonicalVceType`), measured floors: a prefix of
    // `robust` at least 1 long, of `cluster` at least 2.
    std::string vceRest;
    const std::string lv = canonicalVceType(*vceVal, &vceRest);
    if (auto trailing = vceTrailingTokenRefusal(stataCmd, lv, vceRest)) return *trailing;
    if (lv == "robust" || lv == "hc1") {
      argv.push_back("--vcov");
      argv.push_back("hc1");
    } else if (lv == "cluster") {
      std::string cvar = clusterOpt.value_or("");
      if (!vceRest.empty()) cvar = vceRest;
      // reghdfe's own refusal (reghdfe.ado's parse_vce: "Can't specify cluster
      // without clustervars (and viceversa)", r(9); w2l_vce_shape_det.log:3959
      // reghdfe_vce_cluster_novar).
      if (cvar.empty() && stataCmd == "reghdfe")
        return stataRefusalBlock("Can't specify cluster without clustervars (and viceversa)", 9);
      if (cvar.empty()) {
        return engineErrorBlock(
            "Option `vce(cluster)` is not supported in this version of PIET. "
            "Cluster variable required: vce(cluster varname)",
            "unsupported_option");
      }
      // BS-067: one var -> --vcov cluster (unchanged); two -> --vcov
      // twoway --cluster A,B; three+ -> --vcov multiway for reghdfe, else a
      // named refusal.
      if (auto bad = clusterSpecRefusal(cvar, /*legacy=*/false, stataCmd, session, dataOverride)) {
        return *bad;
      }
      ClusterSplitResult cs = splitClusterVars(
          cvar, stataCmd == "reghdfe" || stataCmd == "regress" || stataCmd == "areg");
      if (cs.errored) {
        return engineErrorBlock(cs.errorMsg, "unsupported_option");
      }
      for (const auto& a : cs.vcovArgv) argv.push_back(a);
    } else if (lv == "iid" || (stataCmd == "reghdfe" && isReghdfeUnadjustedVce(*vceVal))) {
      argv.push_back("--vcov");
      argv.push_back("iid");
    } else if (!unimplementedVceType(*vceVal).empty()) {
      return engineErrorBlock("Option `vce(" + unimplementedVceType(*vceVal) + ")` is a valid Stata option that this version of PIET does not implement", "unsupported_option");
    } else {
      return engineErrorBlock("Unknown vce type: " + *vceVal, "unsupported_option");
    }
  } else if (clusterOpt.has_value() && !clusterOpt->empty()) {
    // BUG-055 / P2: Stata's legacy `cluster(varname)` — with neither vce()
    // nor robust given — is a synonym for `vce(cluster varname)`: it
    // implies robust-cluster VCE, not iid. Before this fix, `clusterOpt`
    // was parsed (above) but only ever consulted from inside the
    // `vce(cluster ...)` branch, so a bare `cluster(v)` fell through to
    // neither `if` nor here and silently ran iid (v wholly ignored).
    // Oracle P2_legacycluster: `regress price mpg, cluster(rep78)` must
    // come back cluster-robust (n_obs=69, n_clusters=5, se_mpg~=82.63), not
    // iid (n_obs=74, se_mpg~=53.08, rep78 ignored).
    // BS-067: same one/two/three+ split as the vce(cluster ...) branch
    // above applies to the legacy bare cluster() synonym too.
    // BUG-537 (W2 estimopts): this branch sits BEFORE the bare-`robust` one.
    // `robust cluster(id)` typed together is cluster-robust in Stata --
    // w2_pweight.log R5_pw_robust_AND_cluster_equals_R4 is byte-identical to
    // R4 (`vce(cluster id)`: N_clust 15, se_x1 +1.7f38be33c76eeX-003). With
    // robust tested first, the cluster() was parsed and then IGNORED (HC1,
    // N_clust 0): a silent wrong answer on ~29 corpus lines. Twin ordering in
    // estimation_m4.cpp's vcovFromOpts.
    if (auto bad = clusterSpecRefusal(*clusterOpt, /*legacy=*/true, stataCmd, session, dataOverride)) {
      return *bad;
    }
    ClusterSplitResult cs = splitClusterVars(
        *clusterOpt, stataCmd == "reghdfe" || stataCmd == "regress" || stataCmd == "areg");
    if (cs.errored) {
      return engineErrorBlock(cs.errorMsg, "unsupported_option");
    }
    for (const auto& a : cs.vcovArgv) argv.push_back(a);
  } else if (sawRobustFlag) {
    argv.push_back("--vcov");
    argv.push_back("hc1");
  }

  // BUG-568: areg's clustered SE charges every absorbed parameter, including an FE
  // nested in the cluster; reghdfe/xtreg,fe/feols (the engine default) exempt it.
  // w2b_estim.log areg_a_cl se[x1] .0722686736191015 (N_clust 10, df_r 9) against
  // reghdfe_cluster_ctl / xtreg_fe_cluster_ctl .0663182832 on the same fit; the
  // weighted twins w2_pweight.log A2 (0x1.cfb6746651353p-3) vs H1
  // (0x1.b315e83b84cedp-3). Only the areg word sets it, and only with a cluster
  // VCE and an absorbed FE (the engine refuses the flag anywhere else).
  if (stataCmd == "areg" && std::find(argv.begin(), argv.end(), "--fe") != argv.end()) {
    // e(sample) T2: areg has no singleton rule -- a one-observation group is in
    // the sample with its own absorbed mean (w2_esample.log:925 D4 e(N) 59 and
    // :1957 H8 58 keep id 11, where the engine's reghdfe default dropped it: 58
    // and 57). The refits forward it (LastModel::keepSingletons).
    argv.push_back("--keep-singletons");
    for (size_t i = 0; i + 1 < argv.size(); ++i) {
      if (argv[i] == "--vcov" && (argv[i + 1] == "cluster" || argv[i + 1] == "twoway" ||
                                  argv[i + 1] == "multiway")) {
        argv.push_back("--keep-nested-fe-dof");
        break;
      }
    }
  }

  // BUG-1067: of a collinear set, reghdfe and areg keep the EARLIER term and omit
  // the later one (StataBE scratch-r150/c1067/m1.do, z = 2*x: `x o.z`, _b[x]
  // .527368857557537 for reghdfe with and without absorb() and for areg), where
  // regress keeps the engine's default norm-pivoted basis (`o.x z`). The refits
  // forward it from LastModel::collinearForward.
  if (stataCmd == "reghdfe" || stataCmd == "areg") argv.push_back("--collinear-forward");

  // FU-365: regress and areg run Stata's built-in multiway cluster
  // (Stata's multiway-cluster step -> Stata's robust-VCE step), not reghdfe's: every cluster
  // combination carries its own small-sample factor and the negative eigenvalues
  // of the sum are set to 0. w2l_vce_shape_det.log regress_vce_c2_se
  // .0643525900770342 and areg_vce_c2_se .0778721308909151, where the engine
  // default (reghdfe's Gmin factor, reghdfe_vce_c2_se .0711126484294995) gave
  // .0638929607 and .0777683042. reghdfe/feols keep the default.
  const bool builtinMultiway = stataCmd == "regress" || stataCmd == "reg" || stataCmd == "areg";
  if (builtinMultiway) {
    for (size_t i = 0; i + 1 < argv.size(); ++i) {
      if (argv[i] == "--vcov" && (argv[i + 1] == "twoway" || argv[i + 1] == "multiway")) {
        argv.push_back("--multiway-subset-factors");
        break;
      }
    }
  }

  // reghdfe's `noconstant` with absorbed effects: Stata fits as usual and only
  // leaves `_cons` out of e(b)/r(table) (StataBE scratch-r150/rtab/nc.do: _b[x]
  // and _se[x] identical to the fit with a constant, e(b) colnames `x`; 007's
  // tables run `reghdfe treatment $balance, absorb(...) nocons`). The engine
  // refuses --no-intercept with --fe, so the flag is not sent and `_cons` is
  // dropped from the result below.
  const bool feNoCons = noConstant && stataCmd == "reghdfe" &&
                        std::find(argv.begin(), argv.end(), "--fe") != argv.end();
  if (noConstant && !feNoCons) argv.push_back("--no-intercept");
  // BS-048 (BETA A4) — session.level fallback NOT added here: verified
  // cobra's own feols CLI (cmd_feols, apps/cmds/cmds_linear.cpp) never
  // reads `opt(a, "--level", ...)` at all — the ONLY apps/cmds/*.cpp sites
  // that do are qreg/sdtest/ttest/mediate, and only ttest is wired into
  // PIET's dispatch surface today (runTtest carries the session.level
  // fallback instead). Forwarding --level to a command that never consumes
  // it trips cobra's E6 unconsumed-flag net (common.cpp::
  // check_unconsumed_flags) and turns an otherwise-successful regression
  // into a hard error — a session.level fallback here would make an
  // ordinary `set level 90` + subsequent plain `regress` newly error where
  // it worked before. The pre-existing explicit `level(#)` handling below
  // has the SAME latent gap (untested by any freeze fixture — no
  // `level(...)` usage anywhere in debug_all.do) but is OUT of this phase's
  // 4-mechanism scope to fix; flagged for the operator.
  if (level.has_value()) {
    if (*level < 1 || *level > 99) {
      return engineErrorBlock("level(#) must be an integer between 1 and 99.",
                              "unsupported_option");
    }
    argv.push_back("--level");
    argv.push_back(std::to_string(*level));
  }

  // P3 weights wave: registry.cpp's AllWeights row for regress/reghdfe/feols
  // means the tokenizer already let aw/fw/iw/pw all the way through here —
  // forward whichever one the user gave, normalized to cobra's 2-letter
  // --wtype spelling. `parsed.weightVar` only survives the tokenizer's guard
  // when `parsed.weightType` is one of the four legal spellings, so no
  // separate legality check is needed here. A weight combined with --fe
  // (already pushed to argv above, if `absorb()` was given) is fitted by the
  // engine's weighted absorption (feols_fit demeans with the weights); the
  // combinations Stata's absorbing commands refuse (iweight, pweight with an
  // iid VCE) are refused inside cobra itself and surface through the ordinary
  // CobraError catch below -- not checks this handler needs to duplicate.
  if (parsed.weightType.has_value() && parsed.weightVar.has_value() && !parsed.weightVar->empty()) {
    std::string wt = lower(*parsed.weightType);
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
      // BUG-536 (W2 estimopts): a probability weight IMPLIES a robust VCE.
      // w2_pweight.log R2_pw (`[pweight=w]`, no vce): e(vce)=robust,
      // e(vcetype)=Robust, se_x1 +1.a3421f4182c9fX-003 -- byte-identical to
      // R3 (`[aweight=w], vce(robust)`) and NOT to R1 (`[aweight=w]` iid,
      // +1.691bde09fd0feX-003). This handler forwarded the weight and let the
      // engine default to iid, which is exactly the R1 number under a pweight
      // header: 13 corpus lines in paper 034 ran with the wrong SE at rc 0.
      // Only when nothing chose a VCE above -- vce()/robust/cluster() all
      // pushed `--vcov` already, and R4/R5 keep their cluster.
      if (wtype == "pw" && std::find(argv.begin(), argv.end(), "--vcov") == argv.end()) {
        argv.push_back("--vcov");
        argv.push_back("hc1");
      }
    }
  }

  // Stata's builtin `regress` treats a fit with e(r2) > 1 - 1e-14 as exact:
  // e(rss) 0, e(V) 0, and t/p/CI missing (w2e_float_default prop_predict_untyped
  // and _double: _se[p] 0 where the computed RSS left 1.4e-8). reghdfe does not
  // (StataBE `reghdfe yx x, noabsorb` keeps e(rss) 8.5e-28 and _se 1.7e-16), so
  // only the regress word asks the engine for the rule, under the classic VCE.
  // BUG-1055: areg (the builtin's absorb) and an aweighted, fweighted or
  // iweighted regress have it too (StataNow 19.5 BE: `regress y2 x [aw=w]`,
  // `[fw=gp]` and `areg y2 x, absorb(gp)` post e(rss) 0 and _se[x] 0 on an exact
  // y2 = 3 + 2*x); a pweight implies the robust VCE. The refits forward it from
  // LastModel (fitConventionFlagsM4).
  if (stataCmd == "regress" || stataCmd == "reg" || stataCmd == "areg") {
    bool iid = true;
    std::string wtype;
    for (size_t i = 0; i + 1 < argv.size(); ++i) {
      if (argv[i] == "--vcov" && argv[i + 1] != "iid") iid = false;
      if (argv[i] == "--wtype") wtype = argv[i + 1];
    }
    if (iid && wtype != "pw" && std::find(argv.begin(), argv.end(), "--svy") == argv.end() &&
        (stataCmd == "areg" || std::find(argv.begin(), argv.end(), "--fe") == argv.end()))
      argv.push_back("--regress-exact-fit");
  }

  // TS-context injection (MI-083, CONTRACT-engine §16, M3a): when any y/x
  // token is a time-series operator expression (L./F./D./S.), inject
  // --xtset/--tsset from session.panelvar/timevar (translate.py's
  // _has_ts_token + _inject_ts_context, applied to the RAW pre-translation
  // varlist — matches build_estimation_argv's `ts_tokens = [y_raw] + xs_raw`
  // ordering; parsed.varlist IS y_raw/xs_raw here since dispatcher.cpp's
  // TODO(M3) TS-token reject was removed in favor of this real injection).
  if (translate::hasTsToken(parsed.varlist)) {
    argv = translate::injectTsContext(argv, session);
  }

  // BS-090 (BETA Phase G): materialize the `if`/`in`-restricted sample
  // FIRST (piet::handlers::applyIfAndInFilter — same mechanism summarize/
  // count/tabulate/corr/ttest now all share) and point `--data` at it
  // instead of forwarding a raw `--keepif` to cobra's feols CLI (which
  // cannot evaluate an `in`-derived `_n` predicate — see the comment above
  // where `--keepif` used to be pushed onto `argv`).
  // dataOverride (by-group driver): the caller already materialized this group's
  // slice WITH the sub-command's own `if`/`in` ANDed in, so re-running the filter
  // here would re-apply `in` against the slice's own row numbering and silently
  // fit a sub-range of the group.
  //
  // e(sample) T2: the filtered copy keeps each row's position in the working
  // frame (applyIfAndInFilterKeepingIndex), so the engine can scatter the
  // admitted rows back onto it. Under a by: prefix (dataOverride) the slice is
  // the caller's and no mask is asked for: e(sample) after `by g: regress`
  // stays a named r(198) (kUnavailable), never one group's rows.
  std::string regressDataPath;
  std::string esampleIndexVar;
  int64_t esampleHeadRows = 0;
  // FU-475: a TS-bearing regressor list under `if` reads the full working
  // frame and the engine applies the `if` after its operators (see
  // tsFitKeepsFullFrame). Under a by: prefix the caller hands such a fit the
  // group's whole slice, and an `if` naming the pre-evaluated mask column that
  // slice carries (BUG-1166), so the same --keepif goes onto the slice.
  const bool tsEngineKeepif = tsFitKeepsFullFrame(parsed);
  if (dataOverride == nullptr) {
    if (auto refused = tsFitRestrictionRefusal(parsed, stataCmd)) return *refused;
  }
  if (dataOverride != nullptr) {
    regressDataPath = *dataOverride;
    if (tsEngineKeepif) {
      argv.push_back("--keepif");
      argv.push_back(*parsed.ifExpr);
    }
  } else if (tsEngineKeepif) {
    regressDataPath = *session.currentHbin();
    argv.push_back("--keepif");
    argv.push_back(*parsed.ifExpr);
  } else {
    InRangeFilterResult regressFilter;
    try {
      // e(sample) T2: the filtered copy is the fit's input only, so on an
      // unmoved install frame it is cut from that install's on-disk base (same
      // rows and values, no hidden column) instead of writing the head first.
      // An `if` that names the mask (`if e(sample)`) needs the head itself.
      std::string filterSource = *session.currentHbin();
      const bool namesMask =
          (parsed.ifExpr.has_value() && namesHiddenColumn(*parsed.ifExpr));
      const bool restricted = (parsed.ifExpr.has_value() && !parsed.ifExpr->empty()) ||
                              (parsed.inRange.has_value() && !parsed.inRange->empty());
      if (restricted && !namesMask &&
          session.esampleHeadIsInstallOf(session.esampleLastInstallBase) &&
          cecon::native_exists(session.esampleLastInstallBase))
        filterSource = session.esampleLastInstallBase;
      regressFilter = applyIfAndInFilterKeepingIndex(session, filterSource, parsed.ifExpr,
                                                     parsed.inRange, &esampleIndexVar);
      // The install frame's row count is the install's (macroObsCount would
      // open the head as a Datastore, writing it to disk).
      if (!esampleIndexVar.empty())
        esampleHeadRows = filterSource != *session.currentHbin() && session.esampleLastInstallRows > 0
                              ? session.esampleLastInstallRows
                              : session.macroObsCount();
    } catch (const CobraError& ce) {
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    if (!regressFilter.ok) return inRangeRejectError(regressFilter, *parsed.inRange, stataCmd);
    regressDataPath = regressFilter.dataPath;
  }

  // BUG-539: everything residuals() cannot honour is refused BEFORE the fit,
  // by name, so e() is never left pointing at a model whose residual column
  // was silently not made. The column is produced by the engine's `predict
  // --resid` refit (the same call runPredict makes), which has no --weight /
  // --no-intercept / --keep-singletons / --tsset flags: forwarding those fits
  // through it would write a DIFFERENT model's residuals under the user's
  // name. An if/in restriction is NOT refused (review finding, attempt 2 --
  // paper 082 types `reghdfe ... if is_teacher >= 3, ... residuals`): see the
  // sample-masked depvar below the fit.
  bool residExisted = false;
  if (residWanted) {
    if (dataOverride != nullptr || residMutationOut == nullptr) {
      return engineErrorBlock(
          "reghdfe residuals() is not supported under a by: prefix in this version of PIET.",
          "unsupported_option");
    }
    if (residName.empty()) {
      return engineErrorBlock("residuals() requires a new variable name, e.g. residuals(res).",
                              "syntax_error");
    }
    if (!strutil::isValidStataIdentifier(residName)) {
      return engineErrorBlock("reghdfe residuals(): invalid variable name `" + residName + "' — " +
                                  strutil::stataIdentifierRuleText(),
                              "syntax_error");
    }
    if (std::find(argv.begin(), argv.end(), "--weight") != argv.end() || noConstant ||
        keepSingletons || translate::hasTsToken(parsed.varlist)) {
      return engineErrorBlock(
          "reghdfe residuals() is not supported together with weights, noconstant, "
          "keepsingletons or time-series operators in this version of PIET (the residual refit "
          "cannot carry them).",
          "unsupported_option");
    }
    Datastore* ds = session.datastore();
    if (ds != nullptr) {
      const auto& nm = ds->names();
      residExisted = std::find(nm.begin(), nm.end(), residName) != nm.end();
    }
    // w2_smalls.log reghdfe_named_exists: `residuals(myr)` with myr present is
    // r(110) "variable myr already defined" (coarseRc's "already defined"
    // rule). The BARE form is the exception -- reghdfe `cap drop
    // _reghdfe_resid` first, so an old `_reghdfe_resid` is replaced.
    if (residExisted && residName != "_reghdfe_resid") {
      return ojson(eval::engineError("variable " + residName + " already defined"));
    }
  }

  std::vector<std::string> cobraArgs = argv;
  cobraArgs.push_back("--data");
  cobraArgs.push_back(regressDataPath);
  cobraArgs.push_back("--tidy");
  // eb-order T-P1: the fit's full e(V) (the feols options_table row carries
  // --emit-v; apps/cmds/common.cpp run_feols add_e_v). Only the fit call gets
  // the flag, not `argv`, which LastModel records for refits. The engine
  // refuses it with --svy, so an svy fit's e(V) stays the named refusal.
  if (std::find(cobraArgs.begin(), cobraArgs.end(), "--svy") == cobraArgs.end())
    cobraArgs.push_back("--emit-v");
  // e(sample) T2: the engine writes the fit's admitted rows (installed below).
  std::string esampleValues;
  if (dataOverride == nullptr) {
    esampleValues = esampleFitArgs(session, cobraArgs, esampleIndexVar, esampleHeadRows);
  }

  ojson raw;
  try {
    // e(sample) T2: a fit on a filtered scratch copy, with the head still an
    // install frame (dirty, worker-only), reads the copy statelessly: routed
    // resident it would evict the head, writing it to disk first, once per fit.
    std::optional<SessionState::StatelessRead> keepHeadResident;
    if (dataOverride == nullptr && regressDataPath != *session.currentHbin() &&
        session.esampleHeadIsInstallOf(session.esampleLastInstallBase))
      keepHeadResident.emplace(session);
    raw = runCobraJson(cobraArgs);
  } catch (const CobraError& ce) {
    // dispatcher.py "4b. Cobra estimator": `except CobraError as ce:
    // blocks.append(make_engine_error(str(ce), code=ce.code))` — str(ce) is
    // the bracket-prefixed "[code] message". This is the exact site the 2
    // Eval M2-ratification TS carve-out cells exercise (`feols y L.x1`
    // without tsset -> cobra's native `ts_no_tsset` code, re-wrapped here as
    // "[ts_no_tsset] ..." with code coerced to "engine_error" by errorBlock's
    // allow-list, matching the frozen oracle cell byte-for-byte).
    // BUG-1269: reghdfe counts an empty sample as too small for its absorbed
    // effects, r(2001) "insufficient observations", where regress and areg say
    // r(2000) "no observations" (StataBE 19.5: `reghdfe y x if x > 20, absorb(g)`).
    if (stataCmd == "reghdfe" && ce.code == "no_obs" && std::string(ce.what()) == "no observations")
      return eval::errorBlock("engine_error", "insufficient observations", std::nullopt,
                              std::nullopt, std::nullopt, std::string("r(2001)"));
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }

  // BUG-539: reghdfe's residual column. This is runPredict's engine call
  // (`predict --resid NAME`, within-residuals under --fe) pointed at the very
  // head the fit just read, so the output is that head plus one column -- no
  // column made before the fit is lost (predict's own stale-dataPath defect
  // is another lane's). It runs BEFORE e() is posted: a refit that fails, or
  // whose estimation sample is not the fit's own (a cluster variable missing
  // on rows the refit keeps, say), refuses the whole command instead of
  // leaving e() describing a model whose residual column is wrong or absent.
  // The estimation block stays the one object this handler returns; the
  // data_mutation travels through `residMutationOut` and the dispatcher
  // pushes it as its own block (ARRAY-vs-BLOCK: never hand back an array).
  //
  // Under if/in the fit read a filtered COPY (regressDataPath), which cannot
  // take the column: it has fewer rows than the live head. reghdfe fills
  // residuals() on e(sample) only, and `predict --fit-data` would evaluate
  // y - xb on EVERY row with data (and declines --fe). So the refit runs on
  // the live head with the depvar MASKED to the if/in rows: one `eval` pass
  // writes `kResidMaskY = cond((if & in), y, .)` (the in-range through a
  // materialized `_n`, the applyIfAndInFilter recipe, inrange_filter.cpp);
  // predict's listwise deletion then drops exactly the rows the filter
  // dropped, and writes NaN on every row outside md.keep; a last `eval`
  // drops the mask column. The n_obs equality check below is what proves the
  // masked refit saw the fit's own sample.
  std::string residOutPath;
  if (residWanted) {
    const bool restricted = regressDataPath != *session.currentHbin();
    std::string predictData = regressDataPath;
    std::string predictY = y;
    // Scratch column names (design decision: no existing helper exports one;
    // inrange_filter.cpp's kIdxVar is file-local). Double-underscore, like
    // kIdxVar, so a Stata user variable cannot collide in practice.
    const std::string kResidMaskY = "__piet_resid_y__";
    const std::string kResidMaskIdx = "__piet_resid_idx__";
    if (restricted) {
      const bool haveIf = parsed.ifExpr.has_value() && !parsed.ifExpr->empty();
      const bool haveIn = parsed.inRange.has_value() && !parsed.inRange->empty();
      std::string cond;
      std::vector<std::string> margs = {"eval", "--data", *session.currentHbin()};
      if (haveIn) {
        Datastore* hds = session.datastore();
        InRangeBounds rr = parseInRangeBounds(*parsed.inRange, hds != nullptr ? hds->nRows() : 0);
        if (!rr.ok) {
          InRangeFilterResult bad;
          bad.ok = false;
          bad.negative = rr.negative;
          return inRangeRejectError(bad, *parsed.inRange, stataCmd);
        }
        margs.push_back("--bygen");
        margs.push_back(kResidMaskIdx + "=_n");
        cond = kResidMaskIdx + ">=" + std::to_string(rr.a) + " & " + kResidMaskIdx +
               "<=" + std::to_string(rr.b);
      }
      if (haveIf) cond = haveIn ? "(" + *parsed.ifExpr + ") & (" + cond + ")" : *parsed.ifExpr;
      margs.push_back("--gen");
      margs.push_back(kResidMaskY + "=cond((" + cond + "),(" + y + "),.)");
      if (haveIn) {
        margs.push_back("--drop");
        margs.push_back(kResidMaskIdx);
      }
      predictData = nextPath(session.workdir(), "reghdfe_resid_mask");
      margs.push_back("--out");
      margs.push_back(predictData);
      try {
        runCobraJson(translate::injectTsContextForExpr(margs, cond, session));
      } catch (const CobraError& ce) {
        return engineErrorBlock(cobraErrorStr(ce), ce.code);
      }
      predictY = kResidMaskY;
    }
    std::vector<std::string> pargs = {"predict", "--data", predictData, "--y", predictY,
                                      "--x", xs.empty() ? std::string("(intercept)") : joinComma(xs)};
    for (size_t i = 0; i + 1 < argv.size(); ++i) {
      if (argv[i] == "--fe") {
        pargs.push_back("--fe");
        pargs.push_back(argv[i + 1]);
      }
    }
    // The residuals of the fitted sample: a fit that kept its singletons
    // (`keepsingletons`) has residuals on those rows too.
    if (std::find(argv.begin(), argv.end(), "--keep-singletons") != argv.end())
      pargs.push_back("--keep-singletons");
    // BUG-1067: the same collinear omission as the fit (the residual itself does
    // not depend on which member is dropped; the refit's coefficients do).
    if (std::find(argv.begin(), argv.end(), "--collinear-forward") != argv.end())
      pargs.push_back("--collinear-forward");
    pargs.push_back("--resid");
    pargs.push_back(residName);
    residOutPath = nextPath(session.workdir(), "reghdfe_resid");
    pargs.push_back("--out");
    pargs.push_back(residOutPath);
    ojson praw;
    try {
      praw = runCobraJson(pargs);
    } catch (const CobraError& ce) {
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    const bool sameSample = praw.is_object() && raw.is_object() && praw.contains("n_obs") &&
                            raw.contains("n_obs") && praw["n_obs"].is_number() &&
                            raw["n_obs"].is_number() &&
                            praw["n_obs"].get<double>() == raw["n_obs"].get<double>();
    if (!sameSample) {
      return engineErrorBlock(
          "reghdfe residuals(): the residual refit did not reproduce the estimation sample "
          "(e.g. a cluster variable missing on some rows) -- not supported in this version of "
          "PIET.",
          "unsupported_option");
    }
    if (restricted) {
      const std::string unmasked = nextPath(session.workdir(), "reghdfe_resid");
      try {
        runCobraJson({"eval", "--data", residOutPath, "--drop", kResidMaskY, "--out", unmasked});
      } catch (const CobraError& ce) {
        return engineErrorBlock(cobraErrorStr(ce), ce.code);
      }
      residOutPath = unmasked;
    }
  }

  if (!savedFeName.empty()) {
    // A single absorbed effect is identified by its weighted mean-zero
    // normalization. Recover it from THIS fit's slopes and admission mask,
    // rather than fitting a second model or assigning effects off e(sample).
    try {
      Datastore* ds = session.datastore();
      if (ds == nullptr || !ds->has(savedFeVar) || ds->dtype(savedFeVar) != "f64")
        return engineErrorBlock("saving absorbed effects currently requires a numeric fixed effect",
                                "unsupported_option");
      const int64_t n = ds->nRows();
      std::vector<int> sample(static_cast<size_t>(n), 0);
      std::ifstream mask(cecon::to_native_path(esampleValues));
      int64_t admitted = 0;
      for (int64_t i = 0; i < n; ++i) {
        if (!(mask >> sample[static_cast<size_t>(i)]) ||
            (sample[static_cast<size_t>(i)] != 0 && sample[static_cast<size_t>(i)] != 1))
          return engineErrorBlock("saving absorbed effects: the fitted sample is unavailable");
        admitted += sample[static_cast<size_t>(i)];
      }
      if (!raw.contains("esample_n") || !raw["esample_n"].is_number() ||
          raw["esample_n"].get<double>() != static_cast<double>(admitted))
        return engineErrorBlock("saving absorbed effects: the fitted sample does not match e(N)");
      std::string weight;
      for (size_t i = 0; i + 1 < argv.size(); ++i)
        if (argv[i] == "--weight") weight = argv[i + 1];
      const double* w = weight.empty() ? nullptr : ds->col(weight).f64();
      const double* yv = ds->col(y).f64();
      const double* fv = ds->col(savedFeVar).f64();
      std::vector<double> remainder(yv, yv + n);
      for (const auto& row : raw["coeftable"]) {
        const double b = row.value("b", 0.0);
        if (b == 0.0) continue;
        bool ok = true;
        const auto atoms = cecon::margins_parse_column(row.value("name", std::string()), &ok);
        if (!ok) return engineErrorBlock("saving absorbed effects: the fitted design cannot be rebuilt",
                                         "unsupported_option");
        std::vector<const double*> cols;
        for (const auto& a : atoms) {
          if (!ds->has(a.var) || ds->dtype(a.var) != "f64")
            return engineErrorBlock("saving absorbed effects: the fitted design cannot be read");
          cols.push_back(ds->col(a.var).f64());
        }
        for (int64_t i = 0; i < n; ++i) {
          if (!sample[static_cast<size_t>(i)]) continue;
          double x = 1.0;
          for (size_t j = 0; j < atoms.size(); ++j)
            x *= atoms[j].factor ? (cols[j][i] == atoms[j].level ? 1.0 : 0.0) : cols[j][i];
          remainder[static_cast<size_t>(i)] -= b * x;
        }
      }
      std::map<double, std::pair<double, double>> groups;
      double total = 0.0, mass = 0.0;
      for (int64_t i = 0; i < n; ++i) {
        if (!sample[static_cast<size_t>(i)]) continue;
        const double wi = w == nullptr ? 1.0 : w[i];
        auto& g = groups[fv[i]];
        g.first += wi * remainder[static_cast<size_t>(i)];
        g.second += wi;
        total += wi * remainder[static_cast<size_t>(i)];
        mass += wi;
      }
      if (!(mass > 0.0)) return engineErrorBlock("saving absorbed effects: no fitted observations");
      const std::string values = nextPath(session.workdir(), "reghdfe_saved_fe_values");
      std::ofstream out(cecon::to_native_path(values));
      out.precision(17);
      for (int64_t i = 0; i < n; ++i) {
        if (!sample[static_cast<size_t>(i)]) out << ".\n";
        else {
          const auto& g = groups.at(fv[i]);
          out << g.first / g.second - total / mass << '\n';
        }
      }
      out.flush();
      if (!out) return engineErrorBlock("saving absorbed effects: could not write effect values");
      out.close();
      const std::string savedOut = nextPath(session.workdir(), "reghdfe_saved_fe");
      const std::string base = residWanted ? residOutPath : *session.currentHbin();
      const CobraRunResult wr = runCobra({"setcol", "--data", base, "--var", savedFeName,
                                         "--values", values, "--dtype", "f64", "--new",
                                         "--out", savedOut});
      cecon::native_unlink(values);
      if (!wr.ok || !cecon::native_exists(savedOut))
        return engineErrorBlock("saving absorbed effects: " + wr.stderr_text);
      residOutPath = savedOut;
    } catch (const std::exception& e) {
      return engineErrorBlock(std::string("saving absorbed effects: ") + e.what());
    }
  }

  if (feNoCons && raw.is_object() && raw.contains("e_V") && raw["e_V"].is_object()) {
    ojson& ev = raw["e_V"];
    if (ev.contains("names") && ev["names"].is_array() && ev.contains("matrix") && ev["matrix"].is_array()) {
      std::vector<size_t> keepIdx;
      ojson names = ojson::array();
      for (size_t i = 0; i < ev["names"].size(); ++i) {
        const std::string nm = ev["names"][i].is_string() ? ev["names"][i].get<std::string>() : std::string();
        if (nm == "_cons" || nm == "(intercept)") continue;
        keepIdx.push_back(i);
        names.push_back(ev["names"][i]);
      }
      ojson mat = ojson::array();
      for (size_t r : keepIdx) {
        ojson row = ojson::array();
        for (size_t c : keepIdx) row.push_back(ev["matrix"][r][c]);
        mat.push_back(row);
      }
      ev["names"] = names;
      ev["matrix"] = mat;
    }
  }
  ojson block = cobraToEstimation(raw, "feols", &session);
  if (feNoCons && block.contains("coeftable") && block["coeftable"].is_array()) {
    ojson kept = ojson::array();
    for (const auto& row : block["coeftable"]) {
      const std::string nm = row.value("name", std::string());
      if (nm == "_cons" || nm == "(intercept)") continue;
      kept.push_back(row);
    }
    block["coeftable"] = kept;
  }
  // Label-only override: reghdfe IS feols --fe under the hood (cobra has no
  // separate reghdfe estimator), so the cobra invocation and LastModel replay
  // command (below) stay "feols" — only the displayed "estimator" field is
  // renamed to match the Stata command word the user actually typed, so
  // output parity holds (Stata shows "reghdfe", not "feols").
  if (stataCmd == "reghdfe") {
    block["estimator"] = "reghdfe";
  }

  // e(sample) T2: the mask install (esampleInstallOnHead) comes after every
  // refusal above, so a failed command never leaves a new mask under the
  // previous model, and BEFORE setLastModel (BUG-518 order). `fitFiltered` and
  // the fit frame are decided on the head the fit read, before the install
  // moves it. With residuals() the column goes onto the residual frame instead,
  // which becomes the head after e() is posted below.
  const bool fitFiltered = regressDataPath != *session.currentHbin();
  LastModel lm;
  lm.dataPath = *session.currentHbin();
  // FU-475: the engine applied the `if` itself; refits replay it as --keepif
  // (lastModelBaseArgv, runMargins, predict, boottest read lm.fitKeepif). Set
  // before the install, which keeps the fit's frame when this names the mask.
  if (tsEngineKeepif) lm.fitKeepif = *parsed.ifExpr;
  if (residWanted || !savedFeName.empty()) {
    unsigned long long serial = 0;
    const std::string withMask =
        esampleValues.empty()
            ? std::string()
            : esampleInstallColumn(session, esampleValues, raw, residOutPath, false, &serial);
    if (!withMask.empty()) {
      residOutPath = withMask;
      lm.esample = LastModel::EsampleState::kColumn;
      lm.esampleSerial = serial;
    }
  } else if (!esampleValues.empty()) {
    (void)esampleInstallOnHead(session, esampleValues, raw, lm);
  }

  lm.cobraCmd = "feols";
  lm.stataCmd = stataCmd;  // BUG-458: e(cmd) reads "regress"/"reghdfe", not the engine's "feols"
  lm.y = y;
  lm.x = xs;
  // --fe
  for (size_t i = 0; i < argv.size(); ++i) {
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
    if (argv[i] == "--cluster" && i + 1 < argv.size()) lm.cluster.push_back(argv[i + 1]);
    if (argv[i] == "--weight" && i + 1 < argv.size()) lm.fitWeight = argv[i + 1];
    if (argv[i] == "--wtype" && i + 1 < argv.size()) lm.fitWtype = argv[i + 1];
    if (argv[i] == "--keep-singletons") lm.keepSingletons = true;
    if (argv[i] == "--collinear-forward") lm.collinearForward = true;  // BUG-1067
  }
  lm.depvar = block.value("depvar", "");
  // W:P5 fit-sample fidelity (BUG-163): this handler materializes its if/in
  // restriction into a scratch file and fits THAT — record it so
  // test/lincom/margins/predict/boottest refit the same sample. Left empty
  // when no filter applied (regressDataPath == the session head), keeping
  // every unfiltered refit byte-identical to before.
  if (fitFiltered) lm.fitDataPath = regressDataPath;  // e(sample) T2: decided pre-install
  lm.result = raw;
  // BUG-570: e(N_clust) exists only after a clustered fit. The engine reports
  // n_clusters 0 for every other VCE and e(N_clust) resolves from this cached
  // reply, so `display e(N_clust)` printed 0 where Stata prints `.`
  // (w2_pweight.log R2_pw PSTR `120|117|.|robust|...`; R0/R1 and the areg/reghdfe
  // arms without a cluster alike). The key is dropped from the cached reply ONLY:
  // the emitted block keeps the engine's field, whose shape the freeze fixtures pin.
  if (lm.result.has_value() && lm.result->is_object() && lm.result->contains("n_clusters") &&
      (*lm.result)["n_clusters"].is_number() &&
      (*lm.result)["n_clusters"].get<double>() == 0.0) {
    lm.result->erase("n_clusters");
  }
  // FU-365: after a multiway regress/areg, Stata posts e(kcluster)/e(cluster#) and
  // no e(N_clust) (Stata's multiway-cluster step): w2l_vce_shape_det.log
  // regress_vce_c2 and areg_vce_c2 read `.`. The engine's n_clusters (the
  // smallest G, which still drives df_r = min - 1) is dropped from the cached reply
  // only, like BUG-570 above. reghdfe keeps it (reghdfe_vce_c2 :3820 reads 7).
  const bool postMultiway = builtinMultiway && (lm.vcov == "twoway" || lm.vcov == "multiway") &&
                            lm.result.has_value() && lm.result->is_object();
  if (postMultiway) {
    lm.result->erase("n_clusters");
    // e(rank) is the rank of the (eigenvalue-clipped) V (w2s_multiway_class.log
    // :116 mw_reg_c2_names `...|6|2|180|`), the engine's vcov_rank.
    applyBuiltinMultiwayReplyM4(*lm.result);
  }
  // BUG-1041: areg's MAP path and reghdfe post e(mss) = r(ssr), the model sum of
  // squares about the overall mean (Stata's absorb step, e(r2) =
  // r(ssr)/r(sst)), and e(tss) = r(sst): StataBE areg and reghdfe
  // e(mss) 9177.98 = e(tss) 9544.62 - e(rss) 366.64 under every VCE, where the
  // engine's mss is the within one (areg's hidden e(mss_w)). reghdfe also posts
  // e(tss_within), read here under the reply's own key. Cached reply only: the
  // emitted block keeps the engine's fields, whose shape the freeze fixtures pin.
  if ((stataCmd == "areg" || stataCmd == "reghdfe") && lm.result.has_value() &&
      lm.result->is_object() && lm.result->contains("tss_total") &&
      (*lm.result)["tss_total"].is_number() && lm.result->contains("rss") &&
      (*lm.result)["rss"].is_number()) {
    const double tss = (*lm.result)["tss_total"].get<double>();
    (*lm.result)["mss"] = tss - (*lm.result)["rss"].get<double>();
    (*lm.result)["tss"] = tss;
  }
  // BUG-1268: a constant-only `regress y` posts e(F) 0 (StataBE 19.5 `F(0, 39) =
  // 0.00`); the engine's reply carries no F when df_m is 0. Cached reply only,
  // like the keys above.
  if (stataCmd == "regress" && lm.result.has_value() && lm.result->is_object() &&
      lm.result->contains("df_m") && (*lm.result)["df_m"].is_number() &&
      (*lm.result)["df_m"].get<double>() == 0.0 &&
      !(lm.result->contains("F") && (*lm.result)["F"].is_number()))
    (*lm.result)["F"] = 0.0;
  // reghdfe's e(N_clustervars) and e(N_clust1..k) (w2b_estim.log:1410
  // reghdfe_cluster3 reads e(N_clustervars) 3). Cached reply only, like df_a
  // below: the resolver's key == name fall-through reads them. N_clustervars
  // is the number of forwarded cluster names; N_clust# are the engine's
  // per-dimension G (n_clusters_by, a JSON ARRAY read element by element --
  // never pushed as a block). Only the multiway fit is pinned by an arm; the
  // one-way and two-way values follow reghdfe's documented e() list.
  if (stataCmd == "reghdfe" && lm.result.has_value() && lm.result->is_object() &&
      (lm.vcov == "cluster" || lm.vcov == "twoway" || lm.vcov == "multiway") &&
      !lm.cluster.empty()) {
    size_t nVars = 1;
    for (char c : lm.cluster.back()) {
      if (c == ',') ++nVars;
    }
    (*lm.result)["N_clustervars"] = static_cast<double>(nVars);
    if (lm.result->contains("n_clusters_by") && (*lm.result)["n_clusters_by"].is_array()) {
      size_t j = 0;
      for (const auto& g : (*lm.result)["n_clusters_by"]) {
        ++j;
        if (g.is_number()) (*lm.result)["N_clust" + std::to_string(j)] = g.get<double>();
      }
    } else if (nVars == 1 && lm.result->contains("n_clusters") &&
               (*lm.result)["n_clusters"].is_number()) {
      (*lm.result)["N_clust1"] = (*lm.result)["n_clusters"].get<double>();
    }
  }
  // reghdfe's e(N_full): e(N) plus the singleton rows the fit dropped
  // (reghdfe `st_numscalar("e(N_full)", st_numscalar("e(N)") +
  // num_singletons)`; w2v_margins_corpus F3_at_atmeans_enopost N_full=240).
  // Cached reply only, read by the resolver's key == name fall-through; set
  // only when the engine reports both counts.
  if (stataCmd == "reghdfe" && lm.result.has_value() && lm.result->is_object() &&
      lm.result->contains("n_obs") && (*lm.result)["n_obs"].is_number() &&
      lm.result->contains("n_dropped_singletons") &&
      (*lm.result)["n_dropped_singletons"].is_number()) {
    (*lm.result)["N_full"] = (*lm.result)["n_obs"].get<double>() +
                             (*lm.result)["n_dropped_singletons"].get<double>();
  }
  // e(df_a) after areg / reghdfe (derivation and oracle arms: absorbedDofA).
  // Like N_g/df_a on the xtreg,fe route, it goes into the cached reply only
  // (the e() resolver falls through key == name); the emitted block, whose
  // shape the freeze fixtures pin, is unchanged.
  if (lm.result.has_value()) {
    if (const auto dfA = absorbedDofA(*lm.result, stataCmd)) (*lm.result)["df_a"] = *dfA;
  }
  // FU-479: reghdfe's e(N_hdfe), the number of absorbed FE dimensions
  // (w2_esample.log:818-819 D1_reghdfe_singleton_out absorb(id) -> 1;
  // predict-e-xbd.log:348 absorb(id f2) -> 2). The reply carries no such key;
  // its `absorbed` object holds one entry per absorbed term the engine fitted
  // (apps/cmds/common.cpp run_feols, md.fe), so its size is the count. Cached
  // reply only (key == name fall-through), like df_a above.
  if (stataCmd == "reghdfe" && lm.result.has_value() && lm.result->is_object() &&
      lm.result->contains("absorbed") && (*lm.result)["absorbed"].is_object() &&
      !(*lm.result)["absorbed"].empty()) {
    (*lm.result)["N_hdfe"] = static_cast<double>((*lm.result)["absorbed"].size());
  }
  // BUG-1510: reghdfe's degrees-of-freedom bookkeeping (StataBE 19.5, 180 rows, id 30
  // categories, id2 7, id3 5; reghdfe.mata's estimate_dof, method identified, not
  // ported). Each absorbed variable has K categories, M of them redundant:
  //  * one that is a cluster variable or nested in one (the reply's nested_fe) is all
  //    redundant, M = K (Nested? 1);
  //  * of the others, in absorb() order, each but the first has at least one redundant
  //    category, the first too when a nested one exists;
  //  * each later one takes the most connected components (mobility groups) it forms
  //    with any earlier one ("pairwise"); the second one's count is exact, those after
  //    it are flagged Inexact? 1.
  // e(df_a_initial) = sum K, e(df_a_redundant) = sum M, e(df_a) = their difference and
  // e(df_a_nested) the nested variables' K: absorb(id) 30/0/30, absorb(id id2) 37/1/36,
  // absorb(id id2 id3) 42/2/40 with e(dof_table) (30 0 30 0 0 \ 7 1 6 0 0 \ 5 1 4 1 0),
  // absorb(id id2) vce(cluster id) and absorb(id2 id) vce(cluster id) 37/31/6 (nested 30),
  // absorb(id id2 id4) with id4 nested in id 40/4/36 (id4 forms 3 groups with id). For
  // two variables the components are the engine's (k_absorbed = K1 + K2 - components);
  // for three or more they are counted here over e(sample). noabsorb absorbs `_cons`
  // (1/0/1, e(N_hdfe) 1). e(df_a) takes the same count, so it also covers the cases
  // absorbedDofA leaves missing or miscounts (StataBE: absorb(id id4) vce(cluster id) 2,
  // absorb(id2 id5 id3) with id5 nested in id2 11; the engine's e(df_r) keeps its own
  // count for three or more FEs). Nothing is posted (e(df_a) stays absorbedDofA's) for
  // an absorbed term that is not a plain numeric variable, or a fit whose e(sample) is
  // not on the working frame when the count needs it. Cached reply only, like df_a
  // above; the strings and e(dof_table) are posted with the model below.
  struct HdfeDof {
    std::vector<std::string> names;  // e(extended_absvars), the dof_table rows
    std::vector<double> K, M, inexact, nested;
  };
  std::optional<HdfeDof> hdfeDof;
  if (stataCmd == "reghdfe" && lm.result.has_value() && lm.result->is_object()) {
    const ojson& rr = *lm.result;
    HdfeDof d;
    bool ok = true;
    if (lm.fe.empty()) {
      d = HdfeDof{{"_cons"}, {1.0}, {0.0}, {0.0}, {0.0}};
    } else if (rr.contains("absorbed") && rr["absorbed"].is_object() &&
               rr["absorbed"].size() == lm.fe.size()) {
      std::set<std::string> nestedSet;
      if (rr.contains("nested_fe") && rr["nested_fe"].is_array()) {
        for (const auto& n : rr["nested_fe"]) {
          if (n.is_string()) nestedSet.insert(n.get<std::string>());
          else ok = false;
        }
      }
      // in absorb() order (lm.fe), which the reply's `absorbed` object does not keep
      for (const std::string& name : lm.fe) {
        ok = cecon::ident::isValidName(name) && rr["absorbed"].contains(name) &&
             rr["absorbed"][name].is_number();
        if (!ok) break;
        const bool nst = nestedSet.count(name) > 0;
        d.names.push_back(name);
        d.K.push_back(rr["absorbed"][name].get<double>());
        d.M.push_back(nst ? d.K.back() : 0.0);
        d.inexact.push_back(0.0);
        d.nested.push_back(nst ? 1.0 : 0.0);
      }
      std::vector<size_t> rest;  // the variables not nested in a cluster, in order
      for (size_t g = 0; ok && g < d.names.size(); ++g)
        if (d.nested[g] == 0.0) rest.push_back(g);
      const bool anyNested = rest.size() < d.names.size();
      for (size_t r = 0; ok && r < rest.size(); ++r) {
        if (r > 0 || anyNested) d.M[rest[r]] = 1.0;
        if (r > 1) d.inexact[rest[r]] = 1.0;
      }
      if (ok && rest.size() >= 2) {
        // comp[a][b], a < b: the connected components of rest[a] and rest[b]
        std::vector<std::vector<double>> comp(rest.size(), std::vector<double>(rest.size(), 0.0));
        if (d.names.size() == 2 && rr.contains("k_absorbed") && rr["k_absorbed"].is_number()) {
          comp[0][1] = d.K[0] + d.K[1] - rr["k_absorbed"].get<double>();
        } else {
          const auto rows = est_detail::esampleRowsM4(session, lm);
          const char* rowsKey = rr.contains("n_rows") ? "n_rows" : "n_obs";
          ok = rows.has_value() && rr.contains(rowsKey) && rr[rowsKey].is_number() &&
               static_cast<double>(rows->size()) == rr[rowsKey].get<double>();
          // each variable's categories coded 0..K-1 over e(sample)'s rows
          std::vector<std::vector<size_t>> codes(rest.size());
          std::vector<size_t> levels(rest.size(), 0);
          for (size_t r = 0; ok && r < rest.size(); ++r) {
            const double* v = est_detail::f64ColumnM4(session, d.names[rest[r]]);
            ok = v != nullptr;
            if (!ok) break;
            std::map<double, size_t> lev;
            codes[r].reserve(rows->size());
            for (const int64_t i : *rows)
              codes[r].push_back(lev.emplace(v[i], lev.size()).first->second);
            levels[r] = lev.size();
            ok = static_cast<double>(levels[r]) == d.K[rest[r]];  // the engine's categories
          }
          // a pair's components: union-find over its categories, joined by every row
          for (size_t a = 0; ok && a < rest.size(); ++a) {
            for (size_t b = a + 1; b < rest.size(); ++b) {
              std::vector<size_t> parent(levels[a] + levels[b]);
              std::iota(parent.begin(), parent.end(), size_t{0});
              const auto find = [&parent](size_t x) {
                while (parent[x] != x) x = parent[x] = parent[parent[x]];  // path halving
                return x;
              };
              size_t groups = parent.size();
              for (size_t i = 0; i < codes[a].size(); ++i) {
                const size_t p = find(codes[a][i]), q = find(levels[a] + codes[b][i]);
                if (p != q) {
                  parent[p] = q;
                  --groups;
                }
              }
              comp[a][b] = static_cast<double>(groups);
            }
          }
        }
        for (size_t b = 1; ok && b < rest.size(); ++b)
          for (size_t a = 0; a < b; ++a) d.M[rest[b]] = std::max(d.M[rest[b]], comp[a][b]);
      }
    } else {
      ok = false;
    }
    if (ok) hdfeDof = d;
  }
  if (hdfeDof.has_value()) {
    ojson& rr = *lm.result;
    double initial = 0.0, redundant = 0.0, nested = 0.0;
    for (size_t g = 0; g < hdfeDof->names.size(); ++g) {
      initial += hdfeDof->K[g];
      redundant += hdfeDof->M[g];
      if (hdfeDof->nested[g] != 0.0) nested += hdfeDof->K[g];
    }
    rr["df_a"] = initial - redundant;
    rr["df_a_initial"] = initial;
    rr["df_a_redundant"] = redundant;
    rr["df_a_nested"] = nested;
    rr["N_hdfe"] = static_cast<double>(hdfeDof->names.size());
    rr["N_hdfe_extended"] = static_cast<double>(hdfeDof->names.size());
    const bool singletons = rr.contains("n_dropped_singletons") &&
                            rr["n_dropped_singletons"].is_number();
    rr["num_singletons"] = singletons ? rr["n_dropped_singletons"].get<double>() : 0.0;
    // e(converged): the absorption converged (the engine's demeaning; 1 with nothing absorbed)
    bool converged = true;
    if (rr.contains("diagnostics") && rr["diagnostics"].is_object() &&
        rr["diagnostics"].contains("demean_converged") &&
        rr["diagnostics"]["demean_converged"].is_boolean())
      converged = rr["diagnostics"]["demean_converged"].get<bool>();
    rr["converged"] = converged ? 1.0 : 0.0;
  }
  // e(vce) after a default-VCE `regress` is `ols` (w2_pweight.log R0_unweighted
  // PSTR `120|117|.|ols|||||`, R1_aw_control `...|ols|||aweight|= w|`;
  // w2h_postest_refit.log R_ctl/R_aw/R_fw `ols`), and so it is after areg and
  // reghdfe (w2p_estim_ev.log:1154 F_areg_aw `...|60|ols|`; w2l_vce_shape_det.log
  // :695 areg, :3938 reghdfe). lm.vcov keeps the engine spelling `iid`, which the
  // replay machinery keys on; LastModel::eVce carries Stata's, so a later
  // setLastModel() on the same model (setEstHandleM4) republishes `ols` too.
  if ((stataCmd == "regress" || stataCmd == "areg" || stataCmd == "reghdfe") && lm.vcov == "iid")
    lm.eVce = "ols";
  session.setLastModel(lm);
  session.lastEstBlock = block;
  session.attachFitCovariance(raw);  // eb-order T-P1: e(V) is the fit's own (w2g_ebmat.log:365)
  // BUG-1189: reghdfe's e(clustvar1..k), the cluster variables in cluster() order
  // beside e(N_clust1..k) above (StataBE 19.5: vce(cluster id id2 id3) e(clustvar1)
  // id, e(clustvar2) id2, e(clustvar3) id3; vce(cluster id) e(clustvar1) id; none
  // under vce(robust) or the default VCE). Recorded with noteEOnFit, so the next
  // install drops them.
  if (stataCmd == "reghdfe" &&
      (lm.vcov == "cluster" || lm.vcov == "twoway" || lm.vcov == "multiway") &&
      !lm.cluster.empty()) {
    size_t j = 0;
    std::string cur;
    for (const char c : lm.cluster.back() + ",") {
      if (c != ',') {
        cur += c;
        continue;
      }
      if (cur.empty()) continue;
      const std::string nm = "clustvar" + std::to_string(++j);
      session.eval.setEString(nm, cur);
      session.noteEOnFit(nm);
      cur.clear();
    }
  }
  // BUG-1510 (above): reghdfe's absorbed-variable macros and e(dof_table), one row per
  // absorbed variable named `1.var` (`_cons` with noabsorb), columns Categories,
  // Redundant, Num Coefs, Inexact?, Nested? (StataBE 19.5 `matrix list e(dof_table)`;
  // e(title2) "Absorbing 3 HDFE groups", e(dofmethod) "pairwise clusters continuous").
  if (hdfeDof.has_value()) {
    std::string absvars;
    for (const auto& n : hdfeDof->names) absvars += (absvars.empty() ? "" : " ") + n;
    const size_t g = hdfeDof->names.size();
    for (const auto& [name, value] : std::vector<std::pair<std::string, std::string>>{
             {"absvars", absvars},
             {"extended_absvars", absvars},
             {"title2", "Absorbing " + std::to_string(g) + " HDFE group" + (g == 1 ? "" : "s")},
             {"dofmethod", "pairwise clusters continuous"}}) {
      session.eval.setEString(name, value);
      session.noteEOnFit(name);
    }
    eval::EvalMatrix t;
    t.colnames = {"Categories", "Redundant", "Num Coefs", "Inexact?", "Nested?"};
    for (size_t k = 0; k < g; ++k) {
      t.rownames.push_back(lm.fe.empty() ? hdfeDof->names[k] : "1." + hdfeDof->names[k]);
      t.data.push_back({hdfeDof->K[k], hdfeDof->M[k], hdfeDof->K[k] - hdfeDof->M[k],
                        hdfeDof->inexact[k], hdfeDof->nested[k]});
    }
    auto& store = session.eval.eMatrices;
    store.erase(std::remove_if(store.begin(), store.end(),
                               [](const auto& p) { return p.first == "dof_table"; }),
                store.end());
    store.emplace_back("dof_table", std::move(t));
    session.noteEOnFit("dof_table");
  }
  // e(kcluster) and e(cluster1..) after a built-in multiway regress/areg
  // (Stata's multiway-cluster step; w2s_multiway_class.log:133 mw_reg_c2_kclust
  // `30|7|180|id|id2|id#id2|`), as xtreg's multiway fit posts them.
  if (postMultiway) postBuiltinMultiwayEM4(session, raw);
  // eb-order ROUND 3: a table without `_cons` is Stata's e(b) only for a
  // noconstant fit (w2g_ebmat.log:1241 e9_colnames_eb `x1 x2`); the FE fits
  // drop `_cons` from the table for other reasons (resolveEMatrixFor's
  // kNeedCons). Session block only, like e_V: the console block is unchanged.
  if (noConstant && session.lastEstBlock.has_value() && session.lastEstBlock->is_object())
    (*session.lastEstBlock)["noconstant"] = true;
  // Stata's ordered `: e(scalars)'/`: e(macros)'/`: e(matrices)' for the measured
  // regress/areg paths (w2s_multiway_class mw_reg_*/mw_areg_*; w2v_estpost_ematrix
  // ep15d_ematrices `b V beta`).
  postENameListsRegressM4(session, raw, parsed, stataCmd, builtinMultiway, noConstant);

  // BUG-539: e(resid) names the column made by THIS fit (w2_smalls.log
  // reghdfe_e_resid); setLastModel() just erased the previous fit's. The
  // column itself lands on the live head only now, after e() is posted, so
  // the head the fit (and lm.dataPath) read is untouched until this point.
  if (residWanted || !savedFeName.empty()) {
    session.load(residOutPath);
    // A replaced `_reghdfe_resid` is a rewrite (mutation_result.h's third
    // slot), a fresh name is a pure add; the value is the same either way.
    std::vector<std::string> added, rewritten;
    if (residWanted) (residExisted ? rewritten : added).push_back(residName);
    if (!savedFeName.empty()) added.push_back(savedFeName);
    *residMutationOut = mutationResult(session, "gen", added, {}, rewritten);
    if (residWanted) session.eval.setEString("resid", residName);
    // BUG-1260: reghdfe stores its residuals as double (StataBE 19.5
    // scratch-laneA/agents/N1/pex.do: `: type _reghdfe_resid` double); the engine
    // wrote them as doubles, and without the record `: type` read float.
    if (residWanted) session.varMeta[residName].stataType = "double";
    if (!savedFeName.empty()) session.varMeta[savedFeName].stataType = "double";
  }

  return block;
}

// ---------------------------------------------------------------------------
// P3 weights wave: `mean`/`proportion` — NEW commands, both real Stata
// ECLASS estimators (e(b)/_b[...] access afterward), routed to cobra's own
// `mean`/`proportion` verbs. Modeled on runRegress just above (closest
// existing handler that builds a coefficient-table estimation block from a
// cobra call) and runTtest (estimation_m4.cpp)'s level()-fallback precedent.
// registry.cpp's PweightOnly row on both means the tokenizer only lets
// [pw=]/[pweight=] through here (real Stata itself restricts both commands
// to probability weights only). NOT declared in piet/handlers.h (out of
// this batch's edit manifest — owned by a different stint); dispatcher.cpp
// forward-declares runMean/runProportion locally, the same trick this file
// uses for runSummarizeAweight above.
namespace {

ojson runMeanOrProportion(const eval::ParsedCommand& parsed, SessionState& session,
                          const std::string& stataCmd, const std::string& cobraCmd) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  if (parsed.varlist.empty()) {
    return engineErrorBlock("syntax_error: " + stataCmd + " requires at least one variable",
                            "syntax_error");
  }

  std::optional<int> level;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "level" && !v.isBool) {
      try {
        level = std::stoi(v.strValue);
      } catch (...) {
      }
    }
  }
  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kKnownOpts = kVocabMeanProportion.known;
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kKnownOpts)) {
    return *err;
  }
  if (level.has_value() && (*level < 1 || *level > 99)) {
    return engineErrorBlock("level(#) must be an integer between 1 and 99.",
                            "unsupported_option");
  }

  std::vector<std::string> argv = {cobraCmd, "--vars", joinComma(parsed.varlist)};

  // registry.cpp's PweightOnly row means `parsed.weightVar` only survives the
  // tokenizer's guard here when weightType is pw/pweight — safe to forward
  // unconditionally as --wtype pw (no other weight type can reach this
  // handler at all).
  // QF-MEAN-AW: `mean [aw=]` (the tokenizer admits aweight for mean only) is
  // forwarded as --wtype aw (w2aj_qualifiers.log:1983 qf_mn_aw_sp).
  std::string fitWtype;
  if (parsed.weightVar.has_value() && !parsed.weightVar->empty()) {
    // mean's row admits every weight type (the engine serves all four);
    // proportion's is PweightOnly.
    std::string wt = "pw";
    if (parsed.weightType.has_value()) {
      const std::string t = strutil::lower(*parsed.weightType);
      if (t == "aw" || t == "aweight") wt = "aw";
      else if (t == "fw" || t == "fweight") wt = "fw";
      else if (t == "iw" || t == "iweight") wt = "iw";
    }
    argv.push_back("--weight");
    argv.push_back(*parsed.weightVar);
    argv.push_back("--wtype");
    argv.push_back(wt);
    fitWtype = wt;
  }
  if (level.has_value()) {
    argv.push_back("--level");
    argv.push_back(std::to_string(*level));
  } else if (session.level != 95) {
    // ttest's own session.level fallback precedent (estimation_m4.cpp,
    // BS-048): the frozen CLI surface documents --level on both mean and
    // proportion, so a prior `set level` carries through the same way.
    argv.push_back("--level");
    argv.push_back(std::to_string(session.level));
  }

  InRangeFilterResult filt;
  try {
    filt = applyIfAndInFilter(session, *session.currentHbin(), parsed.ifExpr, parsed.inRange);
  } catch (const CobraError& ce) {
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  if (!filt.ok) return inRangeRejectError(filt, *parsed.inRange, stataCmd);

  argv.push_back("--data");
  argv.push_back(filt.dataPath);
  argv.push_back("--tidy");

  ojson raw;
  try {
    raw = runCobraJson(argv);
  } catch (const CobraError& ce) {
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }

  // The frozen mean/proportion envelope names its per-row array "coefs", not
  // "coeftable" like every other estimator this file wires — cobraToEstimation
  // AND macros.cpp's e()/_b[...]/_se[...] resolver (downstream, keyed off
  // session.lastModelResult's "coeftable" field) both look for "coeftable"
  // specifically. Alias it on a copy before handing off, so both the display
  // block and postestimation access see the SAME row shape ({"name","b","se",
  // "t","p","ci_lo","ci_hi"}, already engine-computed) every other estimator
  // uses. The rest of the envelope (n_obs, weight, level, coef, se, sum_w,
  // ...) passes through cobraToEstimation's existing field lists unchanged.
  ojson raw2 = raw;
  if (raw2.contains("coefs") && !raw2.contains("coeftable")) {
    raw2["coeftable"] = raw2["coefs"];
  }

  // proportion's levels are factor-variable levels: StataBE 19.5 refuses a variable
  // with a noninteger or a negative value, r(452) "half:  factor variables may not
  // contain noninteger values" / "neg:  factor variables may not contain negative
  // values" (scratch-laneA2/agents/epost/s11.log), where the engine fits it. A level
  // past 2147483647 is "noninteger" too (2147483648, 1e10, 1e14 and 1e15 alike;
  // epost/s13.log, s14.log). The engine names an integral level by its digits ("-1",
  // "40000") and any other with %g ("1.5", "1e+15").
  if (cobraCmd == "proportion" && raw2.contains("coeftable") && raw2["coeftable"].is_array()) {
    for (const auto& c : raw2["coeftable"]) {
      const std::string nm = c.is_object() ? c.value("name", std::string()) : std::string();
      const size_t colon = nm.rfind(':');
      if (colon == std::string::npos || colon + 1 == nm.size()) continue;
      const std::string var = nm.substr(0, colon), lev = nm.substr(colon + 1);
      const bool integral = lev.find_first_not_of("-0123456789") == std::string::npos;
      const double v = integral ? std::strtod(lev.c_str(), nullptr) : 0.0;
      if (!integral || v > 2147483647.0)
        return stataRefusalBlock(var + ":  factor variables may not contain noninteger values",
                                 452);
      if (v < 0.0)
        return stataRefusalBlock(var + ":  factor variables may not contain negative values",
                                 452);
    }
  }

  ojson block = cobraToEstimation(raw2, cobraCmd, &session);

  // e(b)/_b[...] wiring (same mechanism runRegress uses just above): stash
  // the RAW (coeftable-aliased) result on LastModel so macros.cpp's
  // resolveBRef/resolveEScalar can find it via session.lastModelResult, and
  // session.lastEstBlock for esttab/eststo's snapshot path.
  LastModel lm;
  lm.cobraCmd = cobraCmd;
  lm.y = parsed.varlist[0];
  lm.x = std::vector<std::string>(parsed.varlist.begin() + 1, parsed.varlist.end());
  lm.depvar = block.value("depvar", "");
  lm.dataPath = *session.currentHbin();
  lm.result = raw2;
  // e(wtype)/e(wexp), as Stata posts them (BUG-1286: `mean x [w=wi]` fweight, `= wi`).
  if (!fitWtype.empty()) {
    lm.fitWeight = *parsed.weightVar;
    lm.fitWtype = fitWtype;
  }
  // e(vce) analytic, robust under [pw=] (e(vcetype) stays empty) (StataBE 19.5, BUG-1534).
  lm.eVce = fitWtype == "pw" ? "robust" : "analytic";
  session.setLastModel(lm);
  session.lastEstBlock = block;

  // BUG-1534: mean's and proportion's own e() (StataBE 19.5, 12 rows: `mean x` e(df_r) 11,
  // e(N_over) 1, e(k_eq) 1, e(rank) 1, e(title) "Mean estimation", e(properties) "b V",
  // e(varlist) "x", e(estat_cmd) estat_vce_only, e(marginsnotok) _ALL; `proportion d` the
  // same with "Proportion estimation" and e(varlist) "0.d 1.d"; e(df_r) is the reply's dof,
  // N - 1 or the fw/iw weight total - 1). e(rank) follows e(V) below.
  {
    const auto putScalar = [&session](const std::string& n, double v) {
      session.eval.setEScalar(n, v);
      session.noteEOnFit(n);
    };
    const auto putString = [&session](const std::string& n, const std::string& v) {
      session.eval.setEString(n, v);
      session.noteEOnFit(n);
    };
    static const ojson kNoCoefs = ojson::array();
    const ojson& coefs = raw2.contains("coeftable") && raw2["coeftable"].is_array()
                             ? raw2["coeftable"]
                             : kNoCoefs;
    if (raw2.contains("df") && raw2["df"].is_number()) putScalar("df_r", raw2["df"].get<double>());
    putScalar("N_over", 1.0);
    putScalar("k_eq", 1.0);
    // e(b) and e(V) with Stata's stripe (StataBE 19.5, scratch-laneA2/agents/wml/x2.log and
    // scratch-laneA2/agents/epost/s11.log): row y1; mean's columns are its variables (`x
    // x2`); a proportion names each level `#.var`, the first level of a variable with two
    // or more `#bn.var` (`0bn.d 1.d 0bn.d3 1.d3 2.d3`, `proportion one` `1.one`). e(V) is
    // the reply's full covariance, cross-variable terms included; e(rank) is its rank as
    // invsym() counts it (`mean x x3` 1, `proportion d d3` 2, `proportion one` 0). Without
    // the covariance (one observation) nothing is posted and the readers keep their refusal.
    std::vector<std::string> stripe;
    std::vector<double> b, V;
    const ojson* ev = raw2.contains("e_V") && raw2["e_V"].is_object() ? &raw2["e_V"] : nullptr;
    bool stripeOk = ev != nullptr && ev->contains("names") && (*ev)["names"].is_array() &&
                    ev->contains("matrix") && (*ev)["matrix"].is_array() &&
                    (*ev)["names"].size() == coefs.size() &&
                    (*ev)["matrix"].size() == coefs.size() && !coefs.empty();
    const size_t k = stripeOk ? coefs.size() : 0;
    for (size_t j = 0; stripeOk && j < k; ++j) {
      const ojson& c = coefs[j];
      const ojson& nmj = (*ev)["names"][j];
      const ojson& row = (*ev)["matrix"][j];
      stripeOk = c.is_object() && nmj.is_string() && c.value("name", std::string()) == nmj &&
                 c.contains("b") && c["b"].is_number() && row.is_array() && row.size() == k;
      if (!stripeOk) break;
      const std::string nm = nmj.get<std::string>();
      b.push_back(c["b"].get<double>());
      for (size_t l = 0; l < k && stripeOk; ++l) {
        stripeOk = row[l].is_number() && (*ev)["matrix"][l].is_array() &&
                   (*ev)["matrix"][l].size() == k && (*ev)["matrix"][l][j].is_number();
        if (stripeOk)
          V.push_back(0.5 * (row[l].get<double>() + (*ev)["matrix"][l][j].get<double>()));
      }
      if (cobraCmd == "mean") {
        stripeOk = stripeOk && cecon::ident::isValidName(nm);
        stripe.push_back(nm);
        continue;
      }
      const size_t colon = nm.rfind(':');
      const std::string var = colon == std::string::npos ? std::string() : nm.substr(0, colon);
      const std::string lev = colon == std::string::npos ? std::string() : nm.substr(colon + 1);
      // Stata's largest level is maxlong(), `2147483620.v3`; 2147483647 is r(198) "numlist
      // in operator invalid" (epost/s14.log), and the band between is not posted.
      stripeOk = stripeOk && cecon::ident::isValidName(var) && !lev.empty() &&
                 lev.find_first_not_of("0123456789") == std::string::npos &&
                 std::strtod(lev.c_str(), nullptr) <= 2147483620.0;
      // the first level of its variable, and how many levels the variable has
      const bool first = j == 0 || coefs[j - 1].value("name", std::string()).rfind(var + ":", 0) != 0;
      size_t levels = 0;
      for (const auto& o : coefs)
        if (o.value("name", std::string()).rfind(var + ":", 0) == 0) ++levels;
      stripe.push_back(lev + (first && levels >= 2 ? "bn." : ".") + var);
    }
    if (stripeOk) {
      auto& store = session.eval.eMatrices;
      store.erase(std::remove_if(store.begin(), store.end(),
                                 [](const auto& p) { return p.first == "b" || p.first == "V"; }),
                  store.end());
      eval::EvalMatrix mb, mv;
      mb.rownames = {"y1"};
      mb.colnames = stripe;
      mb.data.push_back(b);
      mv.rownames = stripe;
      mv.colnames = stripe;
      for (size_t j = 0; j < k; ++j)
        mv.data.emplace_back(V.begin() + static_cast<std::ptrdiff_t>(j * k),
                             V.begin() + static_cast<std::ptrdiff_t>((j + 1) * k));
      store.emplace_back("b", std::move(mb));
      store.emplace_back("V", std::move(mv));
      putScalar("rank", static_cast<double>(glmVceRank(V, k)));
    }
    putString("title", cobraCmd == "mean" ? "Mean estimation" : "Proportion estimation");
    putString("properties", "b V");
    // the varlist the fit estimated; a proportion names each level as `#.var`
    std::string varlist;
    bool varlistOk = !coefs.empty();
    for (const auto& c : coefs) {
      const std::string nm = c.is_object() ? c.value("name", std::string()) : std::string();
      std::string item = nm;
      if (cobraCmd == "proportion") {
        const size_t colon = nm.rfind(':');
        const std::string lev = colon == std::string::npos ? std::string() : nm.substr(colon + 1);
        varlistOk = varlistOk && !lev.empty() &&
                    lev.find_first_not_of("0123456789") == std::string::npos;
        item = lev + "." + nm.substr(0, colon == std::string::npos ? 0 : colon);
      }
      varlistOk = varlistOk && !nm.empty();
      varlist += (varlist.empty() ? "" : " ") + item;
    }
    if (varlistOk) putString("varlist", varlist);
    putString("estat_cmd", "estat_vce_only");
    putString("marginsnotok", "_ALL");
  }

  return block;
}

}  // namespace

ojson runMean(const eval::ParsedCommand& parsed, SessionState& session) {
  return runMeanOrProportion(parsed, session, "mean", "mean");
}

ojson runProportion(const eval::ParsedCommand& parsed, SessionState& session) {
  return runMeanOrProportion(parsed, session, "proportion", "proportion");
}

}  // namespace piet::handlers

namespace piet {

namespace {

// Same dequote rule handlers::runUse/runCd/the U7 shims all used
// independently before this consolidation (strip surrounding whitespace,
// then one layer of matching outer quotes).
std::string dequoteUserPath(std::string s) {
  size_t a = s.find_first_not_of(" \t");
  size_t b = s.find_last_not_of(" \t");
  if (a == std::string::npos) return "";
  s = s.substr(a, b - a + 1);
  if (s.size() >= 2 &&
      ((s.front() == '"' && s.back() == '"') || (s.front() == '\'' && s.back() == '\''))) {
    return s.substr(1, s.size() - 2);
  }
  return s;
}

bool isAbsoluteUserPath(const std::string& p) {
  if (p.empty()) return false;
  if (p[0] == '/') return true;
#ifdef _WIN32
  // Drive-letter ("C:\" / "C:/") or UNC/rooted-backslash forms. Dead code
  // on the current POSIX-only build of this TU (handlers.cpp has no other
  // _WIN32 branch yet) — added ahead of the eventual Windows port of the
  // native-verbs surface so the ONE resolver already knows the rule instead
  // of a second fork appearing when that port lands.
  if (p.size() >= 2 && std::isalpha(static_cast<unsigned char>(p[0])) && p[1] == ':') return true;
  if (p[0] == '\\') return true;
#endif
  return false;
}

}  // namespace

std::string typedUserPath(const std::string& rawPath) { return dequoteUserPath(rawPath); }

std::string resolveUserPath(const SessionState& session, const std::string& rawPath) {
  std::string path = dequoteUserPath(rawPath);

  // `~` expansion (os.path.expanduser-equivalent, HOME-relative only —
  // `~user` forms are not exercised by the corpus; matches the rule runCd
  // used before this consolidation, handlers_m3.cpp's now-shared logic).
  if (!path.empty() && path[0] == '~') {
#ifdef _WIN32
    std::string home = cecon::env_utf8("USERPROFILE");
#else
    std::string home = cecon::env_utf8("HOME");
#endif
    if (!home.empty()) path = home + path.substr(1);
  }

  if (isAbsoluteUserPath(path)) return path;

  std::string base;
  if (session.cwd.has_value() && !session.cwd->empty()) {
    base = *session.cwd;
  } else {
    // Last-resort getcwd() — headless drivers (--run-block's fresh-session-
    // per-cell harness) never seed session.cwd at all.
#ifdef _WIN32
    // No getcwd() on MinGW that answers in UTF-8 — a Korean cwd would come
    // back CP949 and every path joined onto it would miss. current_path() is
    // wide underneath; u8string() puts it back in piet's one encoding.
    std::error_code cwdEc;
    std::filesystem::path cwd = std::filesystem::current_path(cwdEc);
    base = cwdEc ? "." : cwd.u8string();
#else
    char buf[4096];
    base = getcwd(buf, sizeof(buf)) ? buf : ".";
#endif
  }
  if (base.empty()) return path;  // no base to join against — return the
                                   // raw relative path rather than
                                   // fabricating a bogus "/path" (the old
                                   // resolvePath's base=="/" -> pop_back ->
                                   // "" -> "/"+path collapse conflated "no
                                   // base" with "root base"; see below).
  // Strip a trailing separator EXCEPT when base is exactly "/" (root) —
  // popping that back to "" and then unconditionally prepending "/" is
  // exactly the mechanism that used to build "/<name>" from a root cwd
  // (the M2-era bug this consolidation fixes); handling "/" explicitly here
  // keeps the join to a single '/' either way.
  if (base.size() > 1 && base.back() == '/') base.pop_back();
  if (base.back() == '/') return base + path;
  return base + "/" + path;
}

}  // namespace piet
