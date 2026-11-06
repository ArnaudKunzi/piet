// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// R2 (giant-file split, 2026-09-01): the shared file-local helpers of the
// former 5,589-line handlers_data_verbs.cpp, promoted VERBATIM (modulo
// inline linkage) so its three split TUs keep one copy. Include only from
// those TUs. The preamble below is the original file head, platform guards
// and all — scraping bare #include lines out of it is how the first cut of
// this header put <windows.h> into a macOS build.
#pragma once
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

#include "piet/cobra_runner.h"
#include "piet/counters.h"
#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"  // gFormat/applyStataFmt (BS-003 tostring default + usedisplayformat rendering)
#include "piet/eval/pyvalue.h"
#include "piet/inrange_filter.h"
#include "piet/mutation_result.h"
#include "piet/strutil.h"
#include "data/missing.h"
#include "xlsx.h"   // Stata `.` / `.a`-`.z` kinds — CSV writes the extended code
#include "piet/scratch_seq.h"
#include "io/win_compat.h"  // cecon::native_exists / to_native_path (BUG-050 wide open)
#include "io/columnar.h"  // cecon::hbin_level_json_text: .dta label text (BUG-1372)

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

namespace piet { namespace handlers { namespace dv_detail {
using ojson = nlohmann::ordered_json;  // the TU-local alias the helpers were written against

// A5-04: forwards to the shared strutil copy (piet/strutil.h) — kept as a
// thin wrapper (not removed) so this file's many `trim(...)`/`lower(...)`
// call sites didn't need touching. Pure dedup, no behavior change.
inline std::string trim(const std::string& s) { return strutil::trim(s); }

// What a using file (merge / append / joinby) states about one of its
// variables: its label, format and storage type (handlers_data_verbs2.cpp).
struct UsingVarMeta {
  std::string label;
  std::string format;
  std::optional<std::string> stataType;
};
std::map<std::string, UsingVarMeta> usingVarMetaOf(SessionState& session, const std::string& usingPath,
                                                   const std::string& usingMetaPath,
                                                   Datastore& usingDs);

// The dataset a `using` names (merge, append, joinby, cross), resolved against
// session.cwd. [D] merge/append/joinby/cross: "If filename is specified without
// an extension, .dta is assumed" -- unless the bare name is itself a file (an
// extensionless tempfile PIET wrote). `shown`, when given, takes the same
// suffix, so a missing file is reported under the name Stata looked for.
std::string resolveUsingDataset(SessionState& session, const std::string& rawPath,
                                std::string* shown = nullptr);

// The using file's value labels after merge/append/joinby/cross: each variable
// in `brought` keeps the set the using file attaches to it, the using's
// definitions are copied (when `copyDefinitions`) unless the master already
// defines the name, and every attachment then reads through its set. A .dta
// states both in its import-dta sidecar (`usingMetaPath`), a native checkpoint
// in the record kept beside its path.
void adoptUsingValueLabels(SessionState& session, const std::string& usingPath,
                           const std::string& usingMetaPath, const std::vector<std::string>& brought,
                           bool copyDefinitions);

// Stata tempfiles carry genuine DTA bytes without a .dta suffix. Match the
// same XML/legacy headers accepted by use before resolving a using file.
inline bool usingFileIsDta(const std::string& path) {
  std::ifstream input(cecon::to_native_path(path), std::ios::binary);
  char header[11] = {};
  input.read(header, sizeof header);
  const auto size = input.gcount();
  const bool xml = size == 11 && std::string(header, 11) == "<stata_dta>";
  const auto version = static_cast<unsigned char>(header[0]);
  const bool legacy = size >= 3 && (version == 113 || version == 114 || version == 115) &&
                      (header[1] == 1 || header[1] == 2) && header[2] == 1;
  return xml || legacy;
}

// describe using needs only the observation count in the file header. Read
// that small fixed prefix rather than converting a using file twice.
inline std::optional<int64_t> usingDtaRows(const std::string& path) {
  std::ifstream input(cecon::to_native_path(path), std::ios::binary);
  std::string header(256, '\0');
  input.read(header.data(), static_cast<std::streamsize>(header.size()));
  header.resize(static_cast<size_t>(input.gcount()));
  size_t offset = 6, width = 4;
  bool little = header.size() > 1 && header[1] == 2;
  if (header.rfind("<stata_dta>", 0) == 0) {
    const bool v117 = header.find("<release>117</release>") != std::string::npos;
    if (!v117 && header.find("<release>118</release>") == std::string::npos) return std::nullopt;
    const auto n = header.find("<N>");
    if (n == std::string::npos) return std::nullopt;
    offset = n + 3;
    width = v117 ? 4 : 8;
    little = header.find("<byteorder>LSF</byteorder>") != std::string::npos;
  } else if (header.size() < 3 ||
             (static_cast<unsigned char>(header[0]) != 114 &&
              static_cast<unsigned char>(header[0]) != 115)) return std::nullopt;
  if (header.size() < offset + width) return std::nullopt;
  uint64_t count = 0;
  for (size_t i = 0; i < width; ++i) {
    const auto byte = static_cast<unsigned char>(header[offset + (little ? i : width - 1 - i)]);
    count |= static_cast<uint64_t>(byte) << (8 * i);
  }
  if (count > static_cast<uint64_t>(INT64_MAX)) return std::nullopt;
  return static_cast<int64_t>(count);
}

inline ojson engineErrorBlock(const std::string& message, const std::string& code = "engine_error") {
  return eval::engineError(message, code);
}

// A refusal whose Stata rc the oracle pins: the block carries `r(NNN)` in its
// reason, which eval::coarseRc reads before any message-based mapping.
inline ojson rcErrorBlock(const std::string& message, int rc,
                          const std::string& code = "engine_error") {
  return ojson(eval::errorBlock(code, message, std::nullopt, std::nullopt, std::nullopt,
                                "r(" + std::to_string(rc) + ")"));
}

// A delimited `using` file the engine would not convert. Stata reads no text
// file here: merge, joinby and cross of any .csv answer "file X not Stata
// format" r(610), append "file X not found" r(601). Reading one is PIET's own
// extension, so a file it cannot read faithfully -- BUG-414: a row wider than
// the names line, whose extra field the converter used to drop at rc 0 -- gets
// that command's code and Stata's words exactly, the file as typed (StataBE 19.5,
// 2026-11-03: `append using f.csv` prints only "file f.csv not found", with data
// in memory or none).
inline ojson delimitedUsingRefused(const std::string& shown, int rc) {
  return rcErrorBlock("file " + shown + (rc == 601 ? " not found" : " not Stata format"), rc);
}

// BUG-038(a): forwards to the shared mapping point (piet::cobraErrorMessage,
// cobra_runner.h) — kept as a thin wrapper (not removed) so this file's
// call sites didn't need touching. Pure dedup, no behavior change here.
inline std::string cobraErrorStr(const CobraError& ce) { return cobraErrorMessage(ce); }

// Several cobra verbs (gsort/order/recode/duplicates drop|tag/append/merge's
// success path) print a plain-text status line to STDERR and JSON only on
// error — runCobraJson()'s "parse stdout as JSON" contract doesn't fit,
// so these call the raw runCobra() and build the CobraError manually from
// its stderr (mirrors cobra_runner.cpp's own errorFromResult(), which is
// file-local there).
inline CobraError errorFromRaw(const CobraRunResult& r) {
  std::string stderrText = r.stderr_text;
  while (!stderrText.empty() &&
         (stderrText.back() == '\n' || stderrText.back() == '\r' || stderrText.back() == ' ')) {
    stderrText.pop_back();
  }
  try {
    nlohmann::json payload = nlohmann::json::parse(stderrText);
    nlohmann::json err = payload.contains("error") ? payload.at("error") : payload;
    if (err.is_object() && err.contains("message")) {
      return CobraError(err.value("code", std::string("engine_error")),
                        err.at("message").get<std::string>(), err.value("detail", std::string()));
    }
  } catch (const nlohmann::json::exception&) {
  }
  std::string firstLine = stderrText;
  auto nl = firstLine.find('\n');
  if (nl != std::string::npos) firstLine = firstLine.substr(0, nl);
  if (firstLine.empty()) firstLine = "cobra exited " + std::to_string(r.exit_code);
  return CobraError("engine_error", firstLine, stderrText);
}

// P5 (BUG-062): scratch-filename collision fix. The old scheme derived the
// filename from wall-clock time (`std::time(nullptr)` — ONE-SECOND
// resolution; multiplying by 1000 only manufactures millisecond-LOOKING
// digits, it adds no real millisecond precision), so any two scratch files
// requested with the same prefix inside the same wall-clock SECOND — e.g.
// two `order` or `gsort` calls back to back in one .do script, entirely
// realistic since each cobra round-trip is only a few ms — collided on the
// exact same path: the second cobra invocation then wrote over (or was read
// back from) the first call's in-flight file instead of getting its own.
// Fixed with a per-process monotonic counter (can never repeat or go
// backwards for the life of this process, regardless of how fast the wall
// clock ticks) plus the OS pid, so two concurrent piet_native processes
// sharing a workdir (e.g. $PIET_WORKDIR pinned by a test harness) can't
// collide with each other either. Every scratch-path call site in this file
// (nextPath() below, plus rename's/clonevar's own inline CSV scratch paths)
// draws from this one shared counter.
inline std::string scratchToken() {
  static std::atomic<uint64_t> counter{0};
  static const long long pid = static_cast<long long>(cecon::native_process_id());
  return std::to_string(pid) + "_" +
         std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
}

inline std::string nextPath(const std::string& workdir, const std::string& prefix) {
  std::string path = workdir + "/" + prefix + "_" + scratchToken() + ".hbin";
  registerScratchMint(path);  // BUG-389: the data verbs' own mints join the registry
  return path;
}

inline bool fileExists(const std::string& path) { return cecon::native_exists(path); }

// A5-04: forwards to the shared strutil copy (piet/strutil.h) — see the
// trim() wrapper above for why this stays a thin pass-through.
inline std::string lower(std::string s) { return strutil::lower(std::move(s)); }

// Lower-cased extension (including the leading dot), or "" if there is none
// — mirrors handlers.cpp's own file-local ext() (each TU keeps its own copy
// per this codebase's established convention, see strutil.h's header note).
// Only the last path component has one: `save sub.d/q2` writes sub.d/q2.dta
// (StataBE 19.5), so a dot before the last '/' is part of a directory name.
inline std::string extOf(const std::string& path) {
  auto pos = path.find_last_of('.');
  if (pos == std::string::npos) return "";
  const auto slash = path.find_last_of('/');
  if (slash != std::string::npos && pos < slash) return "";
  return lower(path.substr(pos));
}

// CSV-escape a field (RFC4180 doubled-quote) — same convention as
// strmutate.cpp's own csvEscape()/postfile.cpp's, kept as a file-local copy
// rather than a new shared header. Still used by the remaining whole-frame
// CSV writers in this file (runDestring / runTostring / runJoinby), none of
// which has a native cobra verb yet.
inline std::string csvEscape(const std::string& text) {
  std::string esc;
  esc.reserve(text.size() + 2);
  esc += '"';
  for (char c : text) {
    if (c == '"') esc += '"';
    esc += c;
  }
  esc += '"';
  return esc;
}

// fix/verb-overhead-sweep: name-only lookup over the resident shadow card
// (session.shadowVarlistIfCurrent()) — the same mechanism fix/sort-validate-
// lazy introduced for handlers_m3.cpp's runSort, reused here by runGsort/
// runOrder so neither needs session.datastore() (a full mmap + header parse
// of however many MB the current hbin is) just to answer "does column X
// exist". File-local copy of handlers_m3.cpp's identically-named helper
// (this codebase's established per-TU convention — see this file's own
// trim()/errorFromRaw() note above) rather than a new shared header.
//
// S4a sibling (fix/verb-overhead-sweep S4): the same card lookup, but handing
// back the ENTRY rather than a yes/no — runEncode needs the dtype alongside the
// name, and reading it off the card is what lets that verb skip its own
// datastore open too. cardHasName forwards to this rather than keeping a second
// copy of the scan (IDIOMS.md "third copy -> hoist"; the hoist target is this
// TU's own anonymous namespace, per the per-TU convention noted above).
inline const VarInfo* cardFind(const std::vector<VarInfo>& card, const std::string& name) {
  for (const auto& info : card) {
    if (info.name == name) return &info;
  }
  return nullptr;
}

inline bool cardHasName(const std::vector<VarInfo>& card, const std::string& name) {
  return cardFind(card, name) != nullptr;
}

// P1-A3 (W1-D3 / WP-14): the `save FILE.dta` metadata sidecar.
//
// `export-dta --in-meta` is the ONLY channel by which a declared Stata storage
// type, display format, variable label or value-label set can reach the writer
// (IDIOMS.md §`import-dta`/`export-dta` round trip; src/io/dta_writer.h:80-88):
// the hbin the session serializes carries f64/factor, a name and a format
// string, and nothing else. A `save x.dta` that shelled out to a BARE
// `export-dta` would therefore write a file whose VALUES are right and whose
// entire label/type layer is silently gone — the downgrade class Doctrine I-3
// bars, and the exact reason this verb's `.dta` arm is a sidecar write rather
// than a one-line exec.
//
// The sidecar is built from the LIVE session maps (varMeta / varLblname /
// labelDefs / dataLabel / characteristics — what applyDtaMeta() fills on
// `use FILE.dta`, and what every in-session `label`/`format` verb then updates
// for the first three; dataLabel/characteristics have no in-session mutator
// yet, so they carry through unchanged from the source .dta), so what
// round-trips is the session's CURRENT metadata, never the source file's
// original.
//
// Two filters, both correctness rather than taste:
//   · only columns present in the live card get an entry. `session.varMeta`
//     keeps entries for dropped variables, and a hint naming a column the Frame
//     does not have is one the writer silently never matches.
//   · `value_label_name` is emitted for every NON-factor column with an
//     attachment, defined or not: Stata keeps an attachment to an undefined set
//     (`label drop`, a misspelled `label values`) and writes it, and the writer
//     takes a name with no table. A value label on a string column the writer
//     still refuses (Stata attaches value labels to numeric variables only), so
//     a factor column never carries one (`tostring` of a labeled numeric).
//
// `data_label` and `characteristics` (FIX:PIET-SAVE-DTA-CHARS-CARRIAGE): carried
// the same way as the label/format layer above — from session.dataLabel /
// session.characteristics, which applyDtaMeta() (handlers.cpp) now populates on
// `use A.dta`. characteristics is emitted VERBATIM, unlike the per-column
// filter above: the writer's own DtaWriteChar comment (src/io/dta_writer.h)
// is explicit that chars are never validated against the live column list,
// because Stata itself keeps a variable's chars after a later `drop` — so
// filtering here would be a policy this codebase has already decided against.
//
// Returns the sidecar path, or "" when the session declares nothing at all
// (then the caller runs `export-dta` without `--in-meta` and the writer's own
// narrowest-lossless scan decides — the documented fallback, value-identical
// either way). A genuine write failure sets `errOut` instead: falling back to
// the scan there would silently drop the labels this whole function exists to
// carry.
inline std::string writeDtaMetaSidecar(SessionState& session, const std::vector<VarInfo>& card,
                                std::string& errOut) {
  nlohmann::ordered_json vars = nlohmann::ordered_json::array();
  std::set<std::string> referencedSets;
  for (const auto& info : card) {
    std::string label = info.label, format = session.displayFormat(info), stataType;
    auto mit = session.varMeta.find(info.name);
    if (mit != session.varMeta.end()) {
      if (mit->second.label.has_value()) label = *mit->second.label;
      if (mit->second.format.has_value()) format = *mit->second.format;
      if (mit->second.stataType.has_value()) stataType = *mit->second.stataType;
    }
    // BUG-295: an in-session `gen`/`egen` with no declared type is float by
    // Stata's own default. Numeric columns only — PIET carries strings as
    // `factor`, and gen's string default is str#, not float, so a factor
    // column stays with the writer's scan.
    if (stataType.empty() && info.dtype != "factor") {
      stataType = "float";
    }
    std::string lblSet;
    auto lit = session.varLblname.find(info.name);
    // An attachment to a set that is not defined is kept too: StataBE writes it
    // (AER 007's Wave1.dta: `lab def notf ...` then `lab val not_in_LF_BL notlf`
    // carries not_in_LF_BL -> notlf with no notlf table) and reads it back.
    if (lit != session.varLblname.end() && info.dtype != "factor") {
      lblSet = lit->second;
    }
    // Nothing declared for this column: leave it out entirely so the writer
    // takes the hbin column's own format and scans for the narrowest lossless
    // type. An entry carrying only a name would say the same thing at the cost
    // of a bigger sidecar.
    if (label.empty() && format.empty() && stataType.empty() && lblSet.empty()) continue;
    nlohmann::ordered_json v;
    v["name"] = info.name;
    if (!label.empty()) v["label"] = cecon::hbin_level_json_text(label);
    if (!format.empty()) v["format"] = format;
    if (!stataType.empty()) v["stata_type"] = stataType;
    if (!lblSet.empty()) {
      v["value_label_name"] = lblSet;
      if (session.labelDefs.count(lblSet) != 0) referencedSets.insert(lblSet);
    }
    vars.push_back(std::move(v));
  }

  // A label set used only in an inactive language is referenced too. Stata
  // stores those attachments in variable characteristics ([D] label language,
  // Methods and formulas, p.669), not in the active value-label-name slots.
  std::set<std::string> liveNumeric;
  for (const auto& info : card) if (info.dtype != "factor") liveNumeric.insert(info.name);
  for (const auto& [language, labels] : session.labelLangStash)
    for (const auto& [name, label] : labels.varLblname)
      if (liveNumeric.count(name) && session.labelDefs.count(label)) referencedSets.insert(label);

  // Only the defined sets an emitted column actually points at (a name with no
  // table is written as an attachment alone, as StataBE does).
  nlohmann::ordered_json valueLabels = nlohmann::ordered_json::object();
  for (const auto& setName : referencedSets) {
    nlohmann::ordered_json codes = nlohmann::ordered_json::object();
    for (const auto& [code, text] : session.labelDefs.at(setName)) codes[code] = cecon::hbin_level_json_text(text);
    valueLabels[setName] = std::move(codes);
  }

  // Rebuild Stata's reserved language characteristics from the live language
  // state. Imported chars may describe an older current language after a switch.
  nlohmann::ordered_json chars = nlohmann::ordered_json::array();
  for (const auto& c : session.characteristics) {
    if (c.charname == "_lang_list" || c.charname == "_lang_c" ||
        c.charname.rfind("_lang_v_", 0) == 0 || c.charname.rfind("_lang_l_", 0) == 0) continue;
    chars.push_back({{"varname", c.varname}, {"charname", c.charname}, {"value", cecon::hbin_level_json_text(c.value)}});
  }
  if (session.labelLangs != std::vector<std::string>{"default"}) {
    std::string languages;
    for (const auto& language : session.labelLangs) {
      if (!languages.empty()) languages += " ";
      languages += language;
    }
    chars.push_back({{"varname", "_dta"}, {"charname", "_lang_list"}, {"value", languages}});
    chars.push_back({{"varname", "_dta"}, {"charname", "_lang_c"}, {"value", session.labelLang}});
    std::set<std::string> liveNames;
    for (const auto& info : card) liveNames.insert(info.name);
    for (const auto& [language, labels] : session.labelLangStash) {
      if (language == session.labelLang ||
          std::find(session.labelLangs.begin(), session.labelLangs.end(), language) == session.labelLangs.end()) continue;
      if (!labels.dataLabel.empty())
        chars.push_back({{"varname", "_dta"}, {"charname", "_lang_v_" + language},
                         {"value", cecon::hbin_level_json_text(labels.dataLabel)}});
      for (const auto& [name, label] : labels.varLabels)
        if (liveNames.count(name) && !label.empty())
          chars.push_back({{"varname", name}, {"charname", "_lang_v_" + language},
                           {"value", cecon::hbin_level_json_text(label)}});
      for (const auto& [name, label] : labels.varLblname)
        if (liveNumeric.count(name) && !label.empty())
          chars.push_back({{"varname", name}, {"charname", "_lang_l_" + language}, {"value", label}});
    }
  }

  if (vars.empty() && valueLabels.empty() && chars.empty() && session.dataLabel.empty()) {
    return std::string();
  }

  // BUG-1372: label text that is not UTF-8 (substr() counts bytes) is written
  // escaped as a .hbin level is; export-dta's --in-meta turns it back.
  nlohmann::ordered_json meta;
  if (!session.dataLabel.empty()) meta["data_label"] = cecon::hbin_level_json_text(session.dataLabel);
  meta["variables"] = std::move(vars);
  meta["value_labels"] = std::move(valueLabels);
  if (!chars.empty()) meta["characteristics"] = std::move(chars);

  const std::string path = session.workdir() + "/save_meta" + scratchSeqSuffix() + ".json";
  registerScratchMint(path);  // BUG-389
  {
    std::ofstream f(cecon::to_native_path(path), std::ios::binary | std::ios::trunc);
    if (!f) {
      errOut = "cannot write the .dta metadata sidecar " + path;
      return std::string();
    }
    f << meta.dump();
    f.flush();
    if (!f) {
      errOut = "failed writing the .dta metadata sidecar " + path;
      std::error_code ec;
      std::filesystem::remove(cecon::to_native_path(path), ec);
      return std::string();
    }
  }
  return path;
}

}}}  // namespace piet::handlers::dv_detail

// R2: cross-TU declarations for the four duplicates workers. In the single-TU
// era they were "declared" by definition order (workers above the router); the
// cut put the router in TU 2 and the workers in TU 1, so the visibility that
// file order used to provide has to be said out loud.
namespace piet::handlers {
using ojson = nlohmann::ordered_json;
ojson runDuplicatesReport(const std::optional<std::vector<std::string>>& varlist,
                          SessionState& session, const std::optional<std::string>& ifExpr,
                          const std::optional<std::string>& inRange);
ojson runDuplicatesDrop(const std::optional<std::vector<std::string>>& varlist,
                        SessionState& session, const std::optional<std::string>& ifExpr,
                        const std::optional<std::string>& inRange);
ojson runDuplicatesTag(const std::optional<std::vector<std::string>>& varlist,
                       const std::string& genvar, SessionState& session,
                       const std::optional<std::string>& ifExpr,
                       const std::optional<std::string>& inRange);
ojson runDuplicatesList(const std::optional<std::vector<std::string>>& varlist,
                        SessionState& session, const std::optional<std::string>& ifExpr,
                        const std::optional<std::string>& inRange);
ojson runDuplicatesExamples(const std::optional<std::vector<std::string>>& varlist,
                            SessionState& session, const std::optional<std::string>& ifExpr,
                            const std::optional<std::string>& inRange);
}  // namespace piet::handlers

