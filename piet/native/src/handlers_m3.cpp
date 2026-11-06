// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// M3a handler-surface expansion — see piet/native/include/piet/handlers.h
// for the per-handler oracle-source mapping. Split from handlers.cpp (the
// M2 vertical slice) to keep that file's diff clean and let this module
// grow independently as M3a/M3b add more commands.
#include "piet/bcal.h"  // AER scope-2 B2-3: %tb formats + the cwd-tracked calendar search dir
#include "piet/handlers.h"
#include "piet/translate.h"
#include "piet/column_fetch.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <regex>
#include <sstream>
#include <unordered_set>
#include <vector>

#include "piet/cobra_runner.h"
#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"  // BUG-540: stataMissingWithKind — `.a`-`.z` identity in matrix literals
#include "piet/eval/macros.h"
#include "piet/eval/pyvalue.h"
#include "piet/inrange_filter.h"
#include "piet/mutation_result.h"
#include "piet/program.h"  // BUG-922: program::restorePreservesDeeperThan is defined here
#include "piet/serset.h"  // BUG-1461: serset::storageType, signed into `datasignature`
#include "piet/strmutate.h"  // BUG-532: needsStringCount, the string-condition test under by:
#include "piet/stata_format.h"  // eb-order T-P1: `matrix list ..., format()`
#include "piet/strutil.h"
#include "piet/scratch_seq.h"
#include "io/win_compat.h"  // cecon::native_exists / to_native_path / env_utf8 (BUG-050/051)
#include "data/missing.h"   // Stata `.` / `.a`-`.z` kinds + stata_cmp (shared with the engine)
#include "core/stata_arith.h"  // `^` with StataBE's bits (frame.cpp's)

#if defined(__APPLE__)
#include <sys/clonefile.h>  // Q1 (gate ③): APFS copy-on-write snapshot for preserve
#endif  // BUG-1469: the vocabularies below sat inside it, undeclared off macOS

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
const piet::eval::OptVocab& kVocabLog = piet::eval::registerOptVocab(
    {"log"}, {{"replace", "append", "text",
                                                                   "smcl", "name"},
     {}});
const piet::eval::OptVocab& kVocabMkdir = piet::eval::registerOptVocab(
    {"mkdir"}, {{"public"},
     {}});
}  // namespace

namespace piet::handlers {

using ojson = nlohmann::ordered_json;

namespace {

// A5-04: forward to the shared strutil copy (piet/strutil.h) — kept as thin
// wrappers (not removed) so this file's many trim(...)/lower(...) call
// sites didn't need touching. Pure dedup, no behavior change.
std::string trim(const std::string& s) { return strutil::trim(s); }

std::string lower(std::string s) { return strutil::lower(std::move(s)); }

ojson engineErrorBlock(const std::string& message, const std::string& code = "engine_error") {
  return eval::engineError(message, code);
}

// B5-D14 (BUG-454): an error whose Stata rc is KNOWN but which coarseRc's
// message rules cannot recognise -- same shape as handlers_aer_util.cpp's
// utilErrorRc. `reason` is the only channel carrying an exact rc (BUG-187)
// and it outranks the coarse mapping.
ojson engineErrorBlockRc(const std::string& message, int rc) {
  return eval::errorBlock("engine_error", message, std::nullopt, std::nullopt, std::nullopt,
                           "r(" + std::to_string(rc) + ")");
}

// BUG-038(a): forwards to the shared mapping point (piet::cobraErrorMessage,
// cobra_runner.h) — kept as a thin wrapper (not removed) so this file's
// call sites didn't need touching. Pure dedup, no behavior change here.
std::string cobraErrorStr(const CobraError& ce) { return cobraErrorMessage(ce); }

std::string nextPath(const std::string& workdir, const std::string& prefix) {
  return mintScratchPath(workdir, prefix);  // BUG-389: one body, registered at mint
}

// Q1 (gate ③): snapshot an on-disk dataset file to `dst`. On APFS a copy-on-
// write clone is effectively free (the ~164MB preserve byte-copy is ~0.6s on
// the reference corpus), so attempt clonefile FIRST and fall back to the
// verbatim byte-copy on ANY failure — non-APFS / cross-volume / a dst-exists
// race, or any non-Apple build (Linux/Windows), where the #if simply compiles
// to the byte-copy alone. The fallback is byte-for-byte the pre-existing copy,
// so behavior is identical everywhere the clone can't be taken. Mirrors the
// engine's own clone-first-fallback (src/data/frame.cpp clone_file, used by
// patch_hbin). clonefile refuses to overwrite an existing dst, so unlink
// first; `dst` is a fresh nextPath() here, so that removal is normally a
// no-op and never touches a live file.
// BUG-124: returns false (and removes any partial dst) when the byte-copy
// fallback fails — an unchecked ENOSPC here left a TORN preserve snapshot
// that a later restore would load. The clonefile path is all-or-nothing.
bool cloneOrCopyFile(const std::string& src, const std::string& dst) {
#if defined(__APPLE__)
  // path::c_str() is const char* on POSIX (Apple); keep the paths alive across
  // the clonefile call so the pointers don't dangle.
  std::filesystem::path nsrc = cecon::to_native_path(src);
  std::filesystem::path ndst = cecon::to_native_path(dst);
  cecon::native_unlink(dst);
  if (::clonefile(nsrc.c_str(), ndst.c_str(), 0) == 0) return true;
  // clonefile failed (non-APFS FS, cross-device, EEXIST race, ...) -> byte-copy.
#endif
  std::ifstream in(cecon::to_native_path(src), std::ios::binary);
  std::ofstream out(cecon::to_native_path(dst), std::ios::binary);
  if (!in || !out) return false;
  // A11 (BUG-138/L5-IOSAVE class sweep): this fallback used to be
  // `out << in.rdbuf()` — libc++ implements that as a byte-at-a-time
  // sgetc/sputc virtual-call loop, ~5.5 ns/byte and *user*-CPU bound, which
  // measured 1.20-1.30 s on the 164 MB reference corpus against 0.35 s for the
  // 1 MB buffered loop below (3.5×, sha256-identical output — see
  // internal capture). Only the cross-volume
  // / non-APFS preserve snapshot reaches here (the clone above takes the
  // same-volume case), so this is the slowest path getting the 3.5×.
  {
    std::vector<char> buf(1 << 20);
    while (in) {
      in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
      std::streamsize got = in.gcount();
      if (got > 0) out.write(buf.data(), got);
      if (!out) break;
    }
  }
  out.flush();
  if (!out || in.bad()) {
    out.close();
    std::error_code rmEc;
    std::filesystem::remove(cecon::to_native_path(dst), rmEc);
    return false;
  }
  return true;
}

// BS-001: `gsort` (real cobra verb, cmds_data.cpp's cmd_gsort) prints a
// plain-text status line to STDERR and JSON only on error — runCobraJson()'s
// "parse stdout as JSON" contract doesn't fit, so runSort (below) calls the
// raw runCobra() and builds the CobraError manually from its stderr. File-
// local copy of handlers_data_verbs.cpp's identically-named helper (this
// codebase's established per-TU convention — see this file's own trim()/
// lower() forwarding-wrapper note above) rather than a new shared header.
CobraError errorFromRaw(const CobraRunResult& r) {
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

// fix/verb-overhead-sweep: name-only lookup over the resident shadow card
// (session.shadowVarlistIfCurrent()) — the same mechanism fix/sort-validate-
// lazy introduced for runSort below, reused here by runDrop/runKeep so
// neither needs session.datastore() (a full mmap + header parse of however
// many MB the current hbin is) just to answer "does column X exist". Plain
// linear scan over VarInfo entries, mirroring Datastore::has()'s own O(cols)
// name lookup (cecon::Dataset::has) — nothing this file does needs it to be
// more than that.
// C4-L2A sibling (D1 card plane): the same card lookup handing back the ENTRY
// rather than a yes/no — runConfirm needs dtype alongside the name. File-local
// twin of handlers_data_verbs.cpp's identically-named S4a helper (the
// established per-TU convention — see that file's own note); cardHasName
// forwards to it rather than keeping a second copy of the scan.
const VarInfo* cardFind(const std::vector<VarInfo>& card, const std::string& name) {
  for (const auto& info : card) {
    if (info.name == name) return &info;
  }
  return nullptr;
}

bool cardHasName(const std::vector<VarInfo>& card, const std::string& name) {
  return cardFind(card, name) != nullptr;
}

}  // namespace

// ---------------------------------------------------------------------------
// xtset / tsset (bodies pending).
namespace {

// The display format a variable carries now: the one resolver
// (SessionState::displayFormat: the session's own, the file's, else the
// declared type's default).
std::string tsCurrentFormat(SessionState& session, Datastore* ds, const std::string& var) {
  if (ds != nullptr && ds->has(var)) {
    const std::vector<VarInfo> card = ds->formatCard(std::vector<std::string>{var});
    if (!card.empty()) return session.displayFormat(card.front());
  }
  VarInfo bare;
  bare.name = var;
  return session.displayFormat(bare);
}

// The panel variable as one comparable key per row: its value, or a string
// column's level code (bare `xtset` re-displays a panel-only declaration whose
// variable has since become string, see runXtsetPanelOnly); missing is NaN
// either way.
std::vector<double> tsPanelKey(Datastore* ds, const std::string& pvar) {
  const int64_t n = ds->nRows();
  std::vector<double> key(static_cast<size_t>(n));
  const cecon::Column& c = ds->col(pvar);
  if (ds->dtype(pvar) == "f64") {
    const double* v = c.f64();
    for (int64_t i = 0; i < n; ++i) key[static_cast<size_t>(i)] = v[i];
  } else {
    const int32_t* codes = c.codes();
    for (int64_t i = 0; i < n; ++i)
      key[static_cast<size_t>(i)] = codes[i] < 0 ? std::nan("") : static_cast<double>(codes[i]);
  }
  return key;
}

// BUG-1090: the values of the numeric `vars` in the order `sort vars` leaves the
// rows, worked out from the UNSORTED columns. The declaration paths used to read
// them back from the sorted frame, which on a resident head writes the whole
// frame to disk to look at one or two columns. This is exact only when every
// value is an ordinary number: the engine's stable ascending sort and this one
// then give the same sequence of values, since rows that tie carry identical
// values. A missing value (whose kinds the engine orders among themselves) or a
// negative zero gives nullopt, and the caller reads the sorted head as before.
std::optional<std::vector<std::vector<double>>> sortedPlainKeys(
    Datastore* ds, const std::vector<std::string>& vars) {
  const int64_t n = ds->nRows();
  std::vector<const double*> cols;
  for (const auto& v : vars) {
    if (ds->dtype(v) != "f64") return std::nullopt;
    const double* c = ds->col(v).f64();
    for (int64_t i = 0; i < n; ++i)
      if (std::isnan(c[i]) || (c[i] == 0.0 && std::signbit(c[i]))) return std::nullopt;
    cols.push_back(c);
  }
  std::vector<int64_t> order(static_cast<size_t>(n));
  for (int64_t i = 0; i < n; ++i) order[static_cast<size_t>(i)] = i;
  std::stable_sort(order.begin(), order.end(), [&](int64_t a, int64_t b) {
    for (const double* c : cols)
      if (c[a] != c[b]) return c[a] < c[b];
    return false;
  });
  std::vector<std::vector<double>> out;
  for (const double* c : cols) {
    std::vector<double> sorted(static_cast<size_t>(n));
    for (int64_t i = 0; i < n; ++i) sorted[static_cast<size_t>(i)] = c[order[static_cast<size_t>(i)]];
    out.push_back(std::move(sorted));
  }
  return out;
}

// What a format's time scale is called. Absent letters (%tC among them) have
// neither a period name nor a counted step: their delta is milliseconds.
struct TsScale {
  char letter;
  const char* periodName;  // "" = the scale has no period option of its own
  const char* stepWord;    // nullptr = delta counts milliseconds
};
constexpr TsScale kTsScales[] = {
    {'g', "generic", "unit"},   {'d', "daily", "day"},          {'b', "", "day"},
    {'w', "weekly", "week"},    {'m', "monthly", "month"},      {'q', "quarterly", "quarter"},
    {'h', "halfyearly", "halfyear"}, {'y', "yearly", "year"},   {'c', "clocktime", nullptr},
};

const TsScale* tsFindScale(char letter) {
  for (const TsScale& row : kTsScales)
    if (row.letter == letter) return &row;
  return nullptr;
}

// Read literally: %-ty and %-td count as generic, only %t? and the old %d name
// a scale.
char tsScaleLetter(const std::string& fmt) {
  if (fmt.size() > 2 && fmt[1] == 't') return fmt[2];
  return fmt.size() > 1 && fmt[1] == 'd' ? 'd' : 'g';
}

// "a yearly period", or "a period" when the scale has no name.
std::string tsPeriodPhrase(char letter) {
  const TsScale* scale = tsFindScale(letter);
  const std::string name = scale == nullptr ? std::string() : scale->periodName;
  return name.empty() ? "a period" : "a " + name + " period";
}

// Stata's default real-to-string, %9.0g without the blanks.
std::string tsPlainNumber(double v) { return strutil::trim(eval::applyStataFmt(v, "%9.0g")); }

std::string tsCounted(double count, const char* word, bool plural) {
  return tsPlainNumber(count) + " " + word + (plural ? "s" : "");
}

// A span of milliseconds as weeks, days, hours and minutes taken greedily, with
// whatever is left over written as (possibly fractional) seconds.
std::string tsClockSpan(double millis) {
  static constexpr struct {
    double size;
    const char* word;
  } kBlocks[] = {{604800000.0, "week"}, {86400000.0, "day"}, {3600000.0, "hour"}, {60000.0, "minute"}};
  std::vector<std::string> parts;
  double left = millis;
  for (const auto& block : kBlocks) {
    const double whole = std::floor(left / block.size);
    if (whole > 0) {
      parts.push_back(tsCounted(whole, block.word, whole > 1));
      left -= whole * block.size;
    }
  }
  if (left > 0) parts.push_back(tsCounted(left / 1000.0, "second", left != 1000.0));
  std::string text;
  for (const auto& part : parts) text += (text.empty() ? "" : " ") + part;
  return text;
}

// A first or last time as the report and r(tmins)/r(tmaxs) show it. The clocktime
// period option, alone among the ways to reach %tc, shows milliseconds as well
// (Stata displays its edges in %tcDDmonCCYY_HH:MM:SS.sss there); r(tsfmt) stays %tc.
std::string tsTimeText(double t, const std::string& fmt, bool showMillis) {
  if (std::isnan(t)) return ".";
  // The old-style bare %d shows what %td shows; the shared formatter knows only the latter.
  std::string text = strutil::trim(eval::applyStataFmt(t, fmt == "%d" ? std::string("%td") : fmt));
  if (showMillis) {
    char field[8];
    std::snprintf(field, sizeof field, ".%03d", static_cast<int>(t - std::floor(t / 1000.0) * 1000.0));
    text += field;
  }
  return text;
}

// The r() face of a value that may not exist.
double tsPostable(double v) { return std::isnan(v) ? eval::kStataMissingSentinel : v; }

// Smallest and largest nonmissing value, or (NaN, NaN). With `gate`, only rows
// whose gate value is nonmissing count.
std::pair<double, double> tsSpan(const double* values, const double* gate, int64_t n) {
  double low = std::nan(""), high = std::nan("");
  for (int64_t row = 0; row < n; ++row) {
    if (std::isnan(values[row]) || (gate != nullptr && std::isnan(gate[row]))) continue;
    if (std::isnan(low) || values[row] < low) low = values[row];
    if (std::isnan(high) || values[row] > high) high = values[row];
  }
  return {low, high};
}

// Visits each block of rows sharing a key in key-sorted data: every missing key
// falls in one block, and no key column at all means one block of everything.
template <typename Visit>
void tsForEachBlock(const double* keys, int64_t n, Visit&& visit) {
  int64_t start = 0;
  for (int64_t row = 1; row <= n; ++row) {
    const bool continues =
        row < n && (keys == nullptr || keys[row] == keys[row - 1] ||
                    (std::isnan(keys[row]) && std::isnan(keys[row - 1])));
    if (continues) continue;
    visit(start, row);
    start = row;
  }
}

bool tsBlocksEqualSized(const double* keys, int64_t n) {
  int64_t size = -1;
  bool equal = true;
  tsForEachBlock(keys, n, [&](int64_t from, int64_t to) {
    if (size >= 0 && to - from != size) equal = false;
    size = to - from;
  });
  return equal;
}

// [XT] xtset's balance words, stated as sets of rows rather than per panel: any
// missing time leaves at best "weakly" (when every time is missing) and otherwise
// "unbalanced"; panels of unequal size are "unbalanced"; and "strongly" needs each
// time value on as many rows as there are panels with a nonmissing id, so rows
// with a missing id count against it (ids 1 2 . at times 1 2 each are "weakly").
const char* tsBalanceWord(const double* ids, const double* times, int64_t n) {
  const int64_t blankTimes = std::count_if(times, times + n, [](double t) { return std::isnan(t); });
  if (blankTimes > 0) return blankTimes == n ? "weakly balanced" : "unbalanced";
  if (!tsBlocksEqualSized(ids, n)) return "unbalanced";
  int64_t realPanels = 0;
  tsForEachBlock(ids, n, [&](int64_t from, int64_t) {
    if (!std::isnan(ids[from])) ++realPanels;
  });
  std::vector<double> ordered(times, times + n);
  std::sort(ordered.begin(), ordered.end());
  bool eachOnEveryPanel = true;
  tsForEachBlock(ordered.data(), n, [&](int64_t from, int64_t to) {
    if (to - from != realPanels) eachOnEveryPanel = false;
  });
  return eachOnEveryPanel ? "strongly balanced" : "weakly balanced";
}

// Consecutive nonmissing times within each panel, compared with delta. Exact on
// doubles: the times are whole numbers by the time this runs.
struct TsStepTally {
  int64_t offBeat = 0;  // steps that are not exactly delta
  bool repeated = false;
  bool belowDelta = false;
};

TsStepTally tsTallySteps(const double* ids, const double* times, int64_t n, double delta) {
  TsStepTally tally;
  tsForEachBlock(ids, n, [&](int64_t from, int64_t to) {
    double previous = std::nan("");
    for (int64_t row = from; row < to; ++row) {
      if (std::isnan(times[row])) continue;
      if (!std::isnan(previous)) {
        const double step = times[row] - previous;
        tally.repeated = tally.repeated || step == 0;
        tally.belowDelta = tally.belowDelta || step < delta;
        if (step != delta) ++tally.offBeat;
      }
      previous = times[row];
    }
  });
  return tally;
}

// The named variable must exist and be numeric. Reachable through a re-display
// whose variable has since been dropped or made string.
std::optional<ojson> tsUnusableKey(Datastore* ds, const std::string& var) {
  if (!ds->has(var)) return engineErrorBlockRc("variable " + var + " not found", 111);
  if (ds->dtype(var) != "f64")
    return engineErrorBlockRc(
        "string variables not allowed in varlist;\n" + var + " is a string variable", 109);
  return std::nullopt;
}

std::optional<ojson> tsBadFormatOption(const TsDeclare& d) {
  if (d.format.empty()) return std::nullopt;
  if (!d.period.empty())
    return engineErrorBlockRc("may not specify both format() and " + d.period, 198);
  if (isValidDisplayFormat(d.format)) return std::nullopt;
  if (d.format.front() == '%') return engineErrorBlockRc("invalid %format", 120);
  return engineErrorBlockRc(d.format + " not found", 111);
}

std::optional<ojson> tsBadDelta(double delta) {
  std::string complaint;
  if (delta == 0) complaint = "zero";
  else if (delta < 0) complaint = "a negative number";
  else if (delta != std::floor(delta)) complaint = "a noninteger value";
  if (complaint.empty()) return std::nullopt;
  return engineErrorBlockRc("delta(" + tsPlainNumber(delta) + ") evaluates to " + complaint, 198);
}

// The display format a declaration leaves on the time variable.
std::string tsChooseFormat(const TsDeclare& d, const std::string& held) {
  if (!d.format.empty()) return d.format;
  if (d.period.empty()) return held;
  const char wanted = d.period.front();
  // clocktime always resets to plain %tc, even from a detailed %tc mask.
  if (wanted != 'c' && tsScaleLetter(held) == wanted) return held;
  return std::string("%t") + wanted;
}

// r(unit1) is the chosen format's third character; r(unit) is the period typed, or
// else what that character spells. Only the names listed here are posted: a %9.0g
// time variable has r(unit1) "." and no r(unit). (Named difference from StataBE,
// which leaves stray text in r(unit) for a third character of 0 or 1.)
std::string tsUnitWord(const std::string& period, const std::string& fmt) {
  if (!period.empty()) return period;
  if (fmt.size() < 3) return "generic";
  const TsScale* scale = tsFindScale(fmt[2]);
  return scale == nullptr ? std::string() : scale->periodName;
}

ojson tsEchoBlock(const std::string& text) {
  ojson block;
  block["type"] = "echo";
  block["text"] = text;
  return block;
}

void tsPostScalar(SessionState& session, std::vector<std::string>& posted, const char* name,
                  double value) {
  session.eval.rValues.set(name, value);
  posted.emplace_back(name);
}

void tsPostText(SessionState& session, std::vector<std::string>& posted, const char* name,
                const std::string& value) {
  session.eval.rStrings.set(name, value);
  posted.emplace_back(name);
}

}  // namespace

// Exported (handlers.h) for xtdescribe's `Delta(t) = ...` line as well.
std::string tsDeltaText(double delta, const std::string& fmt) {
  const TsScale* scale = tsFindScale(tsScaleLetter(fmt));
  if (scale == nullptr || scale->stepWord == nullptr) return tsClockSpan(delta);
  return tsCounted(delta, scale->stepWord, delta > 1);
}

ojson runTsDeclare(const TsDeclare& d, SessionState& session, const std::string& rCmd) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  Datastore* ds = session.datastore();
  if (ds == nullptr) return eval::noDataLoaded();

  std::vector<std::string> sortKeys;
  if (d.panelvar.has_value()) sortKeys.push_back(*d.panelvar);
  sortKeys.push_back(d.timevar);
  for (const std::string& key : sortKeys)
    if (auto refusal = tsUnusableKey(ds, key)) return *refusal;
  if (auto refusal = tsBadFormatOption(d)) return *refusal;
  {
    const double* raw = ds->col(d.timevar).f64();
    const bool allWhole = std::all_of(raw, raw + ds->nRows(), [](double t) {
      return std::isnan(t) || t == std::floor(t);
    });
    if (!allWhole) return engineErrorBlockRc("time variable must contain only integer values", 451);
  }

  // From here on Stata's side effects survive a later refusal: the sort and the
  // format stay, only the declaration itself waits for every check to pass.
  const std::string heldFormat = tsCurrentFormat(session, ds, d.timevar);
  // BUG-1090: work the key columns out before sorting, so that plain data never
  // has the sorted frame materialised just to read two columns of it. `ds` is not
  // used again past the sort, which may replace the head.
  std::optional<std::vector<std::vector<double>>> keyColumns = sortedPlainKeys(ds, sortKeys);
  ojson sortAnswer = runSort(sortKeys, session);
  if (sortAnswer.is_object() && sortAnswer.value("type", std::string()) == "error") return sortAnswer;
  if (!keyColumns.has_value()) {
    Datastore* sortedHead = session.datastore();
    if (sortedHead == nullptr) return eval::noDataLoaded();
    keyColumns.emplace();
    for (const std::string& key : sortKeys) keyColumns->push_back(tsPanelKey(sortedHead, key));
  }
  const double* ids = d.panelvar.has_value() ? keyColumns->front().data() : nullptr;
  const double* times = keyColumns->back().data();
  const int64_t rows = static_cast<int64_t>(keyColumns->back().size());

  const std::string chosenFormat = tsChooseFormat(d, heldFormat);
  std::string warning;
  if (chosenFormat != heldFormat) {
    session.varMeta[d.timevar].format = chosenFormat;
    session.userFormattedVars.insert(d.timevar);
    const char before = tsScaleLetter(heldFormat), after = tsScaleLetter(chosenFormat);
    if (before != after && before != 'g') {
      warning = "warning: Variable " + d.timevar + " had been formatted " + heldFormat + " (" +
                tsPeriodPhrase(before) + "), and you asked for " + tsPeriodPhrase(after) +
                ". Are you sure that is what you want? Format has been changed. " + d.timevar +
                " is now formatted " + chosenFormat + ".\n";
    }
  }

  const double delta = d.delta.value_or(1.0);
  if (auto refusal = tsBadDelta(delta)) return *refusal;
  const TsStepTally steps = tsTallySteps(ids, times, rows, delta);
  if (steps.repeated) {
    return engineErrorBlockRc(ids != nullptr ? "repeated time values within panel"
                                             : "repeated time values in sample",
                              451);
  }
  if (steps.belowDelta)
    return engineErrorBlockRc("time values with period less than delta() found", 451);

  session.panelvar = d.panelvar;
  session.timevar = d.timevar;
  session.delta = d.delta;
  session.writeTsChars();

  // Rows with a missing panel id count for gaps, repeats and balance, not edges.
  const auto [firstTime, lastTime] = tsSpan(times, ids, rows);
  const bool clockOption = d.period == "clocktime";
  const std::string firstText = tsTimeText(firstTime, chosenFormat, clockOption);
  const std::string lastText = tsTimeText(lastTime, chosenFormat, clockOption);
  const std::string stepPhrase = tsDeltaText(delta, chosenFormat);
  const std::string balanceWord = ids != nullptr ? tsBalanceWord(ids, times, rows) : "";

  std::vector<std::string> posted;
  tsPostScalar(session, posted, "tdelta", delta);
  tsPostScalar(session, posted, "tmin", tsPostable(firstTime));
  tsPostScalar(session, posted, "tmax", tsPostable(lastTime));
  tsPostScalar(session, posted, "gaps", steps.offBeat > 0 ? 1.0 : 0.0);
  tsPostText(session, posted, "timevar", d.timevar);
  tsPostText(session, posted, "tsfmt", chosenFormat);
  tsPostText(session, posted, "tdeltas", stepPhrase);
  tsPostText(session, posted, "tmins", firstText);
  tsPostText(session, posted, "tmaxs", lastText);
  if (chosenFormat.size() > 2) tsPostText(session, posted, "unit1", chosenFormat.substr(2, 1));
  const std::string unitWord = tsUnitWord(d.period, chosenFormat);
  if (!unitWord.empty()) tsPostText(session, posted, "unit", unitWord);
  if (ids != nullptr) {
    const auto [lowId, highId] = tsSpan(ids, nullptr, rows);
    tsPostScalar(session, posted, "imin", tsPostable(lowId));
    tsPostScalar(session, posted, "imax", tsPostable(highId));
    tsPostText(session, posted, "panelvar", *d.panelvar);
    tsPostText(session, posted, "balanced", balanceWord);
  }
  eval::declareRPosted(session.eval, rCmd, std::move(posted));

  // The labels align on the colon; the panel label can widen the column past
  // "Time variable", the time-only form never looks at it.
  const std::string panelCaption = d.panelLabel + " variable";
  const size_t column = ids != nullptr ? std::max<size_t>(panelCaption.size(), 13) : 13;
  const auto reportLine = [&](const std::string& caption, const std::string& body) {
    return std::string(static_cast<size_t>(d.indent) + column - caption.size(), ' ') + caption +
           ": " + body;
  };
  std::string report = warning;
  if (ids != nullptr) report += reportLine(panelCaption, *d.panelvar + " (" + balanceWord + ")") + "\n";
  std::string timeBody = d.timevar + ", " + firstText + " to " + lastText;
  if (steps.offBeat == 1) timeBody += ", but with a gap";
  else if (steps.offBeat > 1) timeBody += ", but with gaps";
  report += reportLine("Time variable", timeBody) + "\n" + reportLine("Delta", stepPhrase);
  return tsEchoBlock(report);
}

// Unlike a declaration with a time variable, this leaves the row order and the
// sort marker alone, as Stata does: `by id:` straight after `xtset id` is "not
// sorted" there as well.
ojson runXtsetPanelOnly(const std::string& ivar, SessionState& session, bool declare,
                        const TsDeclare& look) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  Datastore* ds = session.datastore();
  if (ds == nullptr) return eval::noDataLoaded();
  if (!ds->has(ivar)) return engineErrorBlockRc("variable " + ivar + " not found", 111);
  const bool numericPanel = ds->dtype(ivar) == "f64";
  if (declare && !numericPanel)
    return engineErrorBlockRc(
        "string variables not allowed in varlist;\n" + ivar + " is a string variable", 109);

  // Balance here only asks whether the panels are the same size, missing ids
  // being one more panel. A string variable's level codes group it by text.
  std::vector<double> keys = tsPanelKey(ds, ivar);
  const auto realEnd =
      std::partition(keys.begin(), keys.end(), [](double k) { return !std::isnan(k); });
  std::sort(keys.begin(), realEnd);
  const int64_t rows = static_cast<int64_t>(keys.size());
  const std::string balanceWord = tsBlocksEqualSized(keys.data(), rows) ? "balanced" : "unbalanced";

  if (declare) {
    session.panelvar = ivar;
    session.timevar.reset();
    session.delta.reset();
    session.writeTsChars();
  }

  const bool anyId = realEnd != keys.begin();
  const double lowId = numericPanel && anyId ? keys.front() : std::nan("");
  const double highId = numericPanel && anyId ? *std::prev(realEnd) : std::nan("");
  std::vector<std::string> posted;
  tsPostText(session, posted, "panelvar", ivar);
  tsPostText(session, posted, "balanced", balanceWord);
  tsPostScalar(session, posted, "imin", tsPostable(lowId));
  tsPostScalar(session, posted, "imax", tsPostable(highId));
  eval::declareRPosted(session.eval, "xtset", std::move(posted));

  return tsEchoBlock(std::string(static_cast<size_t>(look.indent), ' ') + look.panelLabel +
                     " variable: " + ivar + " (" + balanceWord + ")");
}

ojson runTsRedisplay(SessionState& session, const std::string& rCmd, const TsDeclare& given,
                     bool anyOption) {
  const bool panelLive = session.panelvar.has_value() && !session.panelvar->empty();
  if (!session.timevar.has_value() || session.timevar->empty()) {
    if (rCmd != "xtset") return engineErrorBlockRc("time variable not set, use tsset varname ...", 111);
    if (!panelLive) return engineErrorBlockRc("panel variable not set; use xtset varname ...", 459);
    return runXtsetPanelOnly(*session.panelvar, session, false, given);
  }
  TsDeclare again = given;
  again.timevar = *session.timevar;
  again.panelvar = panelLive ? session.panelvar : std::nullopt;
  // Typing an option replaces the whole time setting, delta included; typing none
  // restates the live one, with r(unit) then read from the variable's format.
  if (!anyOption) {
    again.delta = session.delta;
    again.format = tsCurrentFormat(session, session.datastore(), again.timevar);
  }
  return runTsDeclare(again, session, rCmd);
}

ojson runXtset(const std::string& ivar, const std::optional<std::string>& tvar, SessionState& session,
               std::optional<double> delta) {
  if (!tvar.has_value() || tvar->empty()) return runXtsetPanelOnly(ivar, session, true);
  TsDeclare both;
  both.panelvar = ivar;
  both.timevar = *tvar;
  both.delta = delta;
  return runTsDeclare(both, session, "xtset");
}

// ---------------------------------------------------------------------------
// drop / keep (varlist form)

ojson runDrop(const std::vector<std::string>& varlist, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  // fix/verb-overhead-sweep: same mechanism as runSort's fix/sort-validate-
  // lazy fix below — this varlist check only ever needs column NAMES, so
  // answer it from the resident shadow card when it is provably current for
  // the loaded hbin, instead of unconditionally forcing session.datastore()
  // to reopen the file from disk. Falls back to the exact prior behavior
  // (session.datastore() + ds->has()) whenever the card cannot be trusted —
  // see session.shadowVarlistIfCurrent()'s doc comment (session_state.h) for
  // every case that must fall back rather than risk a wrong "found"/"not
  // found" answer.
  if (const auto* card = session.shadowVarlistIfCurrent()) {
    for (const auto& v : varlist) {
      if (!cardHasName(*card, v)) return engineErrorBlock("variable " + v + " not found");
    }
  } else {
    Datastore* ds = session.datastore();
    if (ds == nullptr) return eval::noDataLoaded();
    for (const auto& v : varlist) {
      if (!ds->has(v)) return engineErrorBlock("variable " + v + " not found");
    }
  }
  std::string outPath = nextPath(session.workdir(), "drop");
  std::vector<std::string> args = {"eval", "--data", *session.currentHbin(), "--out", outPath};
  for (const auto& v : varlist) {
    args.push_back("--drop");
    args.push_back(v);
  }
  // MAXPERF Stage 2 (item 3): drop is a single terminal mutation whose --out
  // becomes the session head and is NEVER read back as a file (session.load()
  // records the virtual head; the card renders from the shadow/materialized
  // datastore) — opt it into the resident frame plane (in-RAM column mutation,
  // no per-verb process spawn). No-op on the fallback plane (serve down/§26-No).
  SessionState::ResidentEligible resident(session);
  try {
    runCobraJson(args);
  } catch (const CobraError& ce) {
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  session.load(outPath);
  return mutationResult(session, "drop", {}, varlist);
}

ojson runKeep(const std::vector<std::string>& varlist, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  // fix/verb-overhead-sweep: same mechanism as runDrop just above — both of
  // keep's own name-only lookups (the full-column enumeration that builds
  // toDrop, and the has-check that validates varlist) come from the resident
  // shadow card when it is provably current, instead of unconditionally
  // forcing session.datastore() open. Falls back to the exact prior
  // behavior (session.datastore() + ds->names()/ds->has()) otherwise.
  std::vector<std::string> toDrop;
  if (const auto* card = session.shadowVarlistIfCurrent()) {
    for (const auto& info : *card) {
      if (std::find(varlist.begin(), varlist.end(), info.name) == varlist.end()) {
        toDrop.push_back(info.name);
      }
    }
    for (const auto& v : varlist) {
      if (!cardHasName(*card, v)) return engineErrorBlock("variable " + v + " not found");
    }
  } else {
    Datastore* ds = session.datastore();
    if (ds == nullptr) return eval::noDataLoaded();
    for (const auto& name : ds->names()) {
      if (std::find(varlist.begin(), varlist.end(), name) == varlist.end()) toDrop.push_back(name);
    }
    for (const auto& v : varlist) {
      if (!ds->has(v)) return engineErrorBlock("variable " + v + " not found");
    }
  }
  std::string outPath = nextPath(session.workdir(), "keep");
  std::vector<std::string> args = {"eval", "--data", *session.currentHbin(), "--out", outPath};
  for (const auto& v : toDrop) {
    args.push_back("--drop");
    args.push_back(v);
  }
  // MAXPERF Stage 2 (item 3): keep is a column-subset drop — same single-
  // terminal-mutation profile as runDrop; opt into the resident frame plane.
  SessionState::ResidentEligible resident(session);
  try {
    runCobraJson(args);
  } catch (const CobraError& ce) {
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  session.load(outPath);
  return mutationResult(session, "keep", {}, toDrop);
}

ojson runDropIf(const std::string& ifExpr, SessionState& session,
               const std::optional<std::string>& inRange) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  std::optional<std::string> ifOpt = ifExpr.empty() ? std::nullopt : std::optional<std::string>(ifExpr);
  // BS-090 (BETA Phase G): `complement=true` keeps the COMPLEMENT of the
  // combined (if AND in) condition. BUG-533: the if-only complement is
  // `--keepif "(EXPR)==0"`, not the old `"!(EXPR)"`, which kept the rows whose
  // EXPR is missing — Stata drops them (missing is true); see
  // applyIfAndInFilter.
  //
  // fix/verb-overhead-sweep S2: opt the row filter into the resident frame
  // plane — the same single-terminal-mutation profile runDrop/runKeep declare
  // above, and the assertion the guard exists to make holds here exactly:
  // `fr.dataPath` becomes the session head via session.load() and is NEVER read
  // back as a file (mutationResult renders it from the shadow card; the
  // fallback rebuild materializes the head first). This is stated HERE, not
  // inside applyIfAndInFilter, because that helper is shared with ~9 callers
  // that DO read its output as a file — summarize's `Datastore(sumifPath)`,
  // collapse's stateless `--data`, the by-group dispatch's `Datastore groupDs`
  // — and only the call site knows which it is. The `in`-range chain suspends
  // this again internally (inrange_filter.cpp), since its own intermediate
  // temps are read back as files.
  SessionState::ResidentEligible resident(session);
  InRangeFilterResult fr;
  try {
    fr = applyIfAndInFilter(session, *session.currentHbin(), ifOpt, inRange, /*complement=*/true);
  } catch (const CobraError& ce) {
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  if (!fr.ok) return inRangeRejectError(fr, *inRange, "drop");
  session.load(fr.dataPath);
  return mutationResult(session, "keepif");
}

ojson runKeepIf(const std::string& ifExpr, SessionState& session,
               const std::optional<std::string>& inRange) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  std::optional<std::string> ifOpt = ifExpr.empty() ? std::nullopt : std::optional<std::string>(ifExpr);
  // fix/verb-overhead-sweep S2: same opt-in as runDropIf just above, for the
  // same reason — see its comment for why this belongs at the call site rather
  // than inside the shared applyIfAndInFilter.
  SessionState::ResidentEligible resident(session);
  InRangeFilterResult fr;
  try {
    fr = applyIfAndInFilter(session, *session.currentHbin(), ifOpt, inRange);
  } catch (const CobraError& ce) {
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  if (!fr.ok) return inRangeRejectError(fr, *inRange, "keep");
  session.load(fr.dataPath);
  return mutationResult(session, "keepif");
}

StringKeyOrder stringKeyOrder(const std::vector<std::string>& levels) {
  StringKeyOrder o;
  std::vector<std::string> distinct = levels;
  distinct.push_back(std::string());  // a -1 code reads as ""
  // std::string's operator< is char_traits<char>::compare: byte order, each
  // byte unsigned — Stata's string order.
  std::sort(distinct.begin(), distinct.end());
  distinct.erase(std::unique(distinct.begin(), distinct.end()), distinct.end());
  auto rankOf = [&](const std::string& v) {
    return static_cast<double>(std::lower_bound(distinct.begin(), distinct.end(), v) -
                               distinct.begin());
  };
  o.rank.reserve(levels.size());
  for (const auto& l : levels) {
    if (l.empty()) o.blankLevel = true;
    o.rank.push_back(rankOf(l));
  }
  o.blankRank = rankOf(std::string());
  // The engine keys a factor on its code with -1 first, so the code order is
  // Stata's exactly when the levels are strictly byte-ordered and none is "".
  o.codeOrderIsStata = !o.blankLevel;
  for (size_t i = 1; i < levels.size() && o.codeOrderIsStata; ++i)
    o.codeOrderIsStata = levels[i - 1] < levels[i];
  return o;
}

namespace {

// A scratch f64 column runByKeepIf adds before its engine calls and drops in
// the last one.
struct ByKeepTempCol {
  std::string name;
  std::vector<double> values;
};

// core/ident.h: a byte >= 0x80 is a name byte, so a Unicode name is one name.
bool byKeepIdentChar(char c) { return cecon::ident::isIdentByte(c); }

// One whole-operand comparison of a string variable with a "..." literal,
// `VAR ==/!= "lit"` or `"lit" ==/!= VAR`.
struct ByKeepLitCompare {
  std::string var;
  bool eq = true;   // `==` (true) or `!=`
  std::string lit;  // the literal's text, quotes stripped
};

// BUG-532: the one string form the engine evaluates right under `by:` is a
// string variable compared with a literal, `VAR ==/!= "lit"` (frame.cpp eqne
// resolves the literal against VAR's dictionary). This scanner finds every
// such comparison that stands as a WHOLE operand -- bounded by the
// expression's start/end, `(`, `)`, `,`, `&`, `|`, with a plain name (no
// subscript, call or TS operator) -- and hands it to `fn`: a returned text
// replaces the comparison, nullopt keeps it as written. Everything else is
// copied, "..." literals whole.
template <typename IsString, typename Fn>
std::string mapStringLiteralCompares(const std::string& cond, IsString isString, Fn fn) {
  auto skipWs = [&](size_t j) {
    while (j < cond.size() && std::isspace(static_cast<unsigned char>(cond[j]))) ++j;
    return j;
  };
  auto boundedLeft = [&](size_t start) {
    size_t j = start;
    while (j > 0 && std::isspace(static_cast<unsigned char>(cond[j - 1]))) --j;
    if (j == 0) return true;
    const char c = cond[j - 1];
    // `&` / `|` but not the tail of `!=`/`==`/`<=`/`>=` (those end in '=').
    return c == '(' || c == ',' || c == '&' || c == '|';
  };
  auto boundedRight = [&](size_t end) {
    const size_t j = skipWs(end);
    if (j >= cond.size()) return true;
    const char c = cond[j];
    return c == ')' || c == ',' || c == '&' || c == '|';
  };
  // `==` or `!=` at j: 0 (none), 1 (==), 2 (!=).
  auto eqOpAt = [&](size_t j) -> int {
    if (j + 1 >= cond.size() || cond[j + 1] != '=') return 0;
    if (cond[j] == '=') return 1;
    if (cond[j] == '!') return 2;
    return 0;
  };
  // A plain name at [nb, ne) that names a string variable.
  auto plainStringName = [&](size_t nb, size_t ne) {
    return ne > nb && !std::isdigit(static_cast<unsigned char>(cond[nb])) &&
           (ne >= cond.size() || (cond[ne] != '[' && cond[ne] != '(' && cond[ne] != '.')) &&
           isString(cond.substr(nb, ne - nb));
  };

  std::string out;
  out.reserve(cond.size() + 16);
  size_t i = 0;
  while (i < cond.size()) {
    const char c = cond[i];
    if (c == '"') {
      const size_t q = cond.find('"', i + 1);
      if (q == std::string::npos) {
        out.append(cond, i, std::string::npos);
        break;
      }
      // `"lit" OP NAME`
      const size_t opAt = skipWs(q + 1);
      if (const int op = eqOpAt(opAt)) {
        const size_t nb = skipWs(opAt + 2);
        size_t ne = nb;
        while (ne < cond.size() && byKeepIdentChar(cond[ne])) ++ne;
        if (plainStringName(nb, ne) && boundedLeft(i) && boundedRight(ne)) {
          const ByKeepLitCompare lc{cond.substr(nb, ne - nb), op == 1,
                                    cond.substr(i + 1, q - i - 1)};
          if (const std::optional<std::string> rep = fn(lc)) {
            out += *rep;
            i = ne;
            continue;
          }
        }
      }
      out.append(cond, i, q + 1 - i);
      i = q + 1;
      continue;
    }
    // `NAME OP "lit"`
    if (cecon::ident::isIdentStartByte(c) &&
        (i == 0 || (!byKeepIdentChar(cond[i - 1]) && cond[i - 1] != '.'))) {
      size_t ne = i;
      while (ne < cond.size() && byKeepIdentChar(cond[ne])) ++ne;
      const size_t opAt = skipWs(ne);
      const int op = eqOpAt(opAt);
      const size_t lit = op != 0 ? skipWs(opAt + 2) : cond.size();
      if (op != 0 && lit < cond.size() && cond[lit] == '"' && plainStringName(i, ne)) {
        const size_t q = cond.find('"', lit + 1);
        if (q != std::string::npos && boundedLeft(i) && boundedRight(q + 1)) {
          const ByKeepLitCompare lc{cond.substr(i, ne - i), op == 1,
                                    cond.substr(lit + 1, q - lit - 1)};
          if (const std::optional<std::string> rep = fn(lc)) {
            out += *rep;
            i = q + 1;
            continue;
          }
        }
      }
      out.append(cond, i, ne - i);
      i = ne;
      continue;
    }
    out += c;
    ++i;
  }
  return out;
}

// BUG-532: the engine resolves `factorvar ==/!= "literal"` against the level
// dictionary and folds a literal that is not a level to a CONSTANT (frame.cpp
// eqne). A blank read from .dta or CSV is code -1 with no "" level, so `s==""`
// was 0 on every row and `s!=""` 1 — the rows Stata calls blank were never
// seen. Every whole-operand `NAME ==/!= ""` (either operand order) naming a
// string variable is therefore answered here: a 0/1 scratch column, 1 where
// the row is blank (code -1, or the level ""), and the comparison respelled
// `(T==1)` / `(T==0)`. Any other `""` left next to a string variable
// (`(s=="")==1` spelled without the parentheses, ...) is refused by name
// rather than handed to the engine's constant fold.
std::optional<ojson> rewriteBlankStringCompares(std::string& cond, Datastore& ds,
                                                 std::vector<ByKeepTempCol>& temps) {
  auto isFactor = [&](const std::string& nm) { return ds.has(nm) && ds.dtype(nm) != "f64"; };
  std::map<std::string, std::string> tempFor;  // variable -> its blank column
  auto blankCol = [&](const std::string& var) -> std::string {
    auto it = tempFor.find(var);
    if (it != tempFor.end()) return it->second;
    std::string name = "__piet_bkblank" + std::to_string(temps.size()) + "__";
    const cecon::Column& c = ds.col(var);
    const int32_t* codes = c.codes();
    std::vector<char> blankLevel(c.levels.size(), 0);
    for (size_t l = 0; l < c.levels.size(); ++l) blankLevel[l] = c.levels[l].empty() ? 1 : 0;
    ByKeepTempCol t;
    t.name = name;
    t.values.resize(static_cast<size_t>(ds.nRows()));
    for (int64_t r = 0; r < ds.nRows(); ++r) {
      const int32_t code = codes[r];
      t.values[static_cast<size_t>(r)] =
          (code < 0 || blankLevel[static_cast<size_t>(code)]) ? 1.0 : 0.0;
    }
    temps.push_back(std::move(t));
    tempFor.emplace(var, name);
    return name;
  };
  const std::string out = mapStringLiteralCompares(
      cond, isFactor, [&](const ByKeepLitCompare& lc) -> std::optional<std::string> {
        if (!lc.lit.empty()) return std::nullopt;
        return "(" + blankCol(lc.var) + (lc.eq ? "==1)" : "==0)");
      });
  // What is left: a blank literal the respelling did not take. Next to a
  // string variable, the engine would fold it to a constant — refuse.
  bool leftover = false;
  for (size_t j = 0; j < out.size() && !leftover;) {
    if (out[j] == '"') {
      const size_t q = out.find('"', j + 1);
      if (q == j + 1) leftover = true;
      j = q == std::string::npos ? out.size() : q + 1;
    } else {
      ++j;
    }
  }
  if (leftover) {
    bool namesString = false;
    for (size_t j = 0; j < out.size() && !namesString;) {
      if (cecon::ident::isIdentStartByte(out[j])) {
        size_t e = j;
        while (e < out.size() && byKeepIdentChar(out[e])) ++e;
        namesString = (j == 0 || out[j - 1] != '.') && isFactor(out.substr(j, e - j));
        j = e;
      } else if (out[j] == '"') {
        const size_t q = out.find('"', j + 1);
        j = q == std::string::npos ? out.size() : q + 1;
      } else {
        ++j;
      }
    }
    if (namesString) {
      return ojson(eval::errorBlock(
          "unsupported_command",
          "a comparison with the blank string \"\" under `by:` is supported only as a whole "
          "operand, `VAR == \"\"` or `VAR != \"\"` (optionally in parentheses, joined by & or |); "
          "rewrite the condition in that form.",
          std::nullopt, std::nullopt, std::nullopt, "piet_gap"));
    }
  }
  cond = out;
  return std::nullopt;
}

// Unique scratch names inside one second (mintScratchPath is second-grained on
// POSIX, and one call here mints up to three files under the same prefix).
std::string byKeepPath(SessionState& session, const char* what) {
  static unsigned long long seq = 0;
  return nextPath(session.workdir(), std::string("bykeep") + what + std::to_string(seq++));
}

// A scratch file one of these helpers made, unlinked once the next step has
// read it (never `head`, the session's own file).
void dropScratchFile(const std::string& p, const std::string& head) {
  if (p.empty() || p == head) return;
  cecon::native_unlink(p);
  forgetScratchMint(p);
}

// Adds each of `temps` to the file `cur`, file to file (`setcol --new`), and
// leaves `cur` naming the last file. Every intermediate file (the CSV payload,
// the previous copy) is unlinked once read, and on every error exit; `head`
// is never touched.
std::optional<ojson> addTempCols(SessionState& session, const std::string& head, std::string& cur,
                                 const std::vector<ByKeepTempCol>& temps) {
  for (const auto& t : temps) {
    // mintScratchPath registers a ".hbin" name; the setcol payload is a CSV,
    // so the name that actually exists is the one registered (BUG-389).
    std::string valuesPath = byKeepPath(session, "vals");
    forgetScratchMint(valuesPath);
    valuesPath = valuesPath.substr(0, valuesPath.size() - 5) + ".csv";  // 5 == strlen(".hbin")
    registerScratchMint(valuesPath);
    {
      std::ofstream vf(cecon::to_native_path(valuesPath), std::ios::binary);
      bool okWrite = static_cast<bool>(vf);
      if (okWrite) {
        char buf[32];
        for (double v : t.values) {
          std::snprintf(buf, sizeof(buf), "%.17g\n", v);
          vf << buf;
        }
        vf.flush();
        okWrite = static_cast<bool>(vf);
      }
      if (!okWrite) {
        vf.close();
        cecon::native_unlink(valuesPath);
        forgetScratchMint(valuesPath);
        dropScratchFile(cur, head);
        return engineErrorBlock("could not write a scratch file under " + session.workdir());
      }
    }
    const std::string next = byKeepPath(session, "col");
    CobraRunResult cr = runCobra({"setcol", "--data", cur, "--var", t.name, "--values", valuesPath,
                                  "--dtype", "f64", "--new", "--out", next});
    cecon::native_unlink(valuesPath);
    forgetScratchMint(valuesPath);
    if (!cr.ok) {
      CobraError ce = errorFromRaw(cr);
      dropScratchFile(next, head);
      dropScratchFile(cur, head);
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    dropScratchFile(cur, head);
    cur = next;
  }
  return std::nullopt;
}

std::string joinCommaKeys(const std::vector<std::string>& v) {
  std::string s;
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) s += ",";
    s += v[i];
  }
  return s;
}

}  // namespace

ojson runByKeepIf(SessionState& session, const std::string& ifExpr,
                  const std::vector<std::string>& byVars,
                  const std::vector<std::string>& stataSortKeys, bool drop) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  // BUG-532: `by g [(s)]: keep if EXPR` / `drop if EXPR`. EXPR is evaluated
  // per by-group (group-local _n/_N, x[k], sum()) into a scratch mask, the rows
  // are filtered on it and the mask is dropped again — ONE engine call, because
  // cmd_eval applies --bygen/--keepif/--drop in argv order over a single frame
  // load (the same fused shape applyIfAndInFilter's `in` chain uses).
  //
  // Row order: the rows are in by++sort key order when the mask is computed —
  // either the caller checked that, or they are sorted in place here first on
  // `stataSortKeys` (the `bysort` half). --bygen is then given by() ONLY: its
  // stable sort on the by-keys keeps every group's rows in their physical
  // order, so _n is the row's position inside its group in the CURRENT order,
  // as in Stata — also when a key in the parentheses is a string, whose
  // engine code order is not Stata's.
  //
  // Missing is TRUE in Stata's `if`. The engine's keep_if keeps a missing mask,
  // which is right for keep; for drop the complement is written `mask==0`, which
  // is false on a missing mask (missing compares above every number), so the
  // row is dropped. EXPR's own `!`, `&`, `|` are left as written: the engine
  // reads a missing OPERAND of those as true, as Stata does (BUG-620), the
  // same reading every other engine-evaluated `if` in PIET gets.
  static const char* kMask = "__piet_bykeep__";
  std::string cond = ifExpr;
  auto joinComma = [](const std::vector<std::string>& v) {
    std::string s;
    for (size_t i = 0; i < v.size(); ++i) {
      if (i) s += ",";
      s += v[i];
    }
    return s;
  };

  // Scratch columns the engine calls need: a blank indicator per string
  // variable compared with "", and a byte-order rank per string sort key whose
  // dictionary is not in Stata's order.
  std::vector<ByKeepTempCol> temps;
  std::vector<std::string> gsortKeys = stataSortKeys;
  // A string variable, a string literal or a string function in EXPR: the
  // same predicate the global keep/drop use to leave the engine for the
  // row-wise string path (read off the varlist card, no file open).
  const bool stringCond = strmutate::needsStringCount(session, cond);
  bool stringKeyToCheck = false;
  // The Datastore is opened only when a string condition or key is involved.
  if (const auto* card = session.shadowVarlistIfCurrent()) {
    for (const auto& info : *card) {
      if (info.dtype == "f64") continue;
      for (const auto& k : byVars) stringKeyToCheck |= info.name == k;
      for (const auto& k : stataSortKeys) stringKeyToCheck |= info.name == k;
    }
  } else {
    stringKeyToCheck = true;
  }
  if (stringCond || stringKeyToCheck) {
    Datastore* ds = session.datastore();
    if (ds == nullptr) return eval::noDataLoaded();
    if (stringCond) {
      if (cond.find("\"\"") != std::string::npos) {
        if (auto err = rewriteBlankStringCompares(cond, *ds, temps)) return *err;
      }
      // Under `by:` the engine evaluates EXPR, and of the string forms it gets
      // only a whole-operand `VAR ==/!= "lit"` right (a blank one was just
      // respelled). Anything else — inlist(s,...), strpos(s,...),
      // substr(s,1,1)=="a", s==s[_n-1], a string variable read as a number —
      // would die in the engine with a generic error (rc 1) or be answered
      // wrong, so it is refused by name, before any row moves.
      auto isFactor = [&](const std::string& nm) { return ds->has(nm) && ds->dtype(nm) != "f64"; };
      const std::string residual = mapStringLiteralCompares(
          cond, isFactor,
          [](const ByKeepLitCompare&) { return std::optional<std::string>("1"); });
      if (strmutate::needsStringCount(session, residual)) {
        return ojson(eval::errorBlock(
            "unsupported_command",
            "under `by:`, a condition on a string variable is supported only as a whole "
            "operand `VAR == \"text\"` or `VAR != \"text\"` (optionally in parentheses, "
            "joined by & or |); string functions (inlist, strpos, substr, ...) and other "
            "string forms in `by ...: keep|drop if` are not supported yet. Compute the "
            "condition into a numeric variable first, e.g. `generate byte t = " + ifExpr +
                "`, then `by ...: keep if t`.",
            std::nullopt, std::nullopt, std::nullopt, "piet_gap"));
      }
    }
    // A string BY-key whose dictionary holds "" as a level AND has blank (-1)
    // rows: Stata sees one group, the engine's by() two. Refused by name.
    for (const auto& k : byVars) {
      if (!ds->has(k) || ds->dtype(k) == "f64") continue;
      const cecon::Column& c = ds->col(k);
      const StringKeyOrder o = stringKeyOrder(c.levels);
      if (!o.blankLevel) continue;
      const int32_t* codes = c.codes();
      for (int64_t r = 0; r < ds->nRows(); ++r) {
        if (codes[r] < 0) {
          return ojson(eval::errorBlock(
              "unsupported_command",
              "`by " + k + ":` is not supported yet when `" + k +
                  "` holds blank values stored two ways (as missing and as an empty "
                  "level): Stata sees one group, this engine would see two. Group on a "
                  "numeric key instead, e.g. `egen id = group(" + k + "), missing`.",
              std::nullopt, std::nullopt, std::nullopt, "piet_gap"));
        }
      }
    }
    for (const auto& t : temps) {
      if (ds->has(t.name)) {
        return engineErrorBlock("a variable named " + t.name +
                                " exists; it is a name PIET reserves for scratch columns");
      }
    }
  }

  const std::string bygenArg =
      std::string(kMask) + "=(" + cond + "), by(" + joinComma(byVars) + ")";
  const std::string pred = drop ? (std::string(kMask) + "==0") : std::string(kMask);

  if (temps.empty()) {
    if (!stataSortKeys.empty()) {
      ojson sorted = runSort(stataSortKeys, session);
      if (sorted.is_object() && sorted.value("type", "") == "error") return sorted;
    }
    std::string outPath = byKeepPath(session, "");
    // BUG-369: a TS operator in EXPR needs the tsset metadata on the call; a
    // runiform()/runiformint() draw needs the session's seed and stream (the
    // runBysortGen pattern), or `set seed` is ignored and the stream never
    // advances.
    std::vector<std::string> args = translate::injectTsContextForExpr(
        {"eval", "--data", *session.currentHbin(), "--bygen", bygenArg, "--keepif", pred,
         "--drop", kMask, "--out", outPath},
        cond, session);
    args = translate::injectRngContextForExpr(args, cond, session);
    // Single terminal mutation whose --out becomes the head and is never read
    // back as a file — the runKeepIf/runDropIf opt-in, for the same reason.
    SessionState::ResidentEligible resident(session);
    try {
      translate::absorbRngDraws(runCobraJson(args), session);
    } catch (const CobraError& ce) {
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    session.load(outPath);
    return mutationResult(session, "keepif");
  }

  // With scratch columns: file to file (the head was materialized by
  // session.datastore() above), and only the last file becomes the head, so
  // no scratch column can reach the user's frame even when a step fails.
  // Every intermediate file is this call's own and is unlinked as soon as
  // the next step has read it, and on every error exit; the head is never
  // touched.
  const std::string head = *session.currentHbin();
  std::string cur = head;
  auto dropIntermediate = [&](const std::string& p) { dropScratchFile(p, head); };
  auto fail = [&](const std::string& partial, ojson err) {
    dropIntermediate(cur);
    if (!partial.empty()) dropIntermediate(partial);
    return err;
  };
  if (std::optional<ojson> err = addTempCols(session, head, cur, temps)) return *err;
  if (!stataSortKeys.empty()) {
    // With blank-compare columns only, gsortKeys are the Stata keys, and the
    // marker this leaves is exactly them, as Stata's `bysort` sets it
    // (w2e_probes.log bo_sort_prefix_noop: a coarser sort downgrades "g x" to
    // "g"); adding or dropping the scratch columns leaves it standing.
    const std::string next = byKeepPath(session, "sort");
    CobraRunResult cr =
        runCobra({"gsort", "--data", cur, "--vars", joinComma(gsortKeys), "--out", next});
    if (!cr.ok) {
      CobraError ce = errorFromRaw(cr);
      return fail(next, engineErrorBlock(cobraErrorStr(ce), ce.code));
    }
    dropIntermediate(cur);
    cur = next;
  }
  std::vector<std::string> base = {"eval", "--data", cur, "--bygen", bygenArg, "--keepif", pred,
                                   "--drop", kMask};
  for (const auto& t : temps) {
    base.push_back("--drop");
    base.push_back(t.name);
  }
  const std::string outPath = byKeepPath(session, "");
  base.push_back("--out");
  base.push_back(outPath);
  std::vector<std::string> args = translate::injectTsContextForExpr(base, cond, session);
  args = translate::injectRngContextForExpr(args, cond, session);
  try {
    translate::absorbRngDraws(runCobraJson(args), session);
  } catch (const CobraError& ce) {
    return fail(outPath, engineErrorBlock(cobraErrorStr(ce), ce.code));
  }
  session.load(outPath);
  dropIntermediate(cur);
  return mutationResult(session, "keepif");
}

// ---------------------------------------------------------------------------
// sort

std::optional<ojson> sortInStataOrder(SessionState& session, const std::vector<std::string>& keys) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  if (keys.empty()) return std::nullopt;
  // Frame::gsort now orders both level strings and missing kinds as Stata
  // does. The shared route also preserves a claim on the actual user keys.
  ojson sorted = runSort(keys, session);
  if (sorted.is_object() && sorted.value("type", "") == "error") return sorted;
  return std::nullopt;
}

ojson runSort(const std::vector<std::string>& varlist, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  // fix/sort-validate-lazy: this varlist check only ever needs COLUMN NAMES,
  // never column DATA — so answer it from the resident shadow card
  // (session.shadowVarlistIfCurrent(), a plain O(columns) name scan over
  // already-cached metadata) instead of unconditionally forcing
  // session.datastore() open the current hbin from disk. That eager re-open
  // (Datastore::Datastore -> cecon::Dataset::open, a full mmap + header
  // parse of however many MB the file is) purely to answer "does column X
  // exist" measured ~700ms-1700ms per sort on a 164 MB corpus in phase-split
  // instrumentation of this function — see the fix's own report for the
  // table. shadowVarlistIfCurrent() returns nullptr for every case where the
  // card cannot be trusted (absent, a `use`/`clear`/restore swapped datasets
  // without reseeding it, a post-estimation command resynced the resident
  // worker onto a DIFFERENT file, or a respawned/rolled-back worker) — the
  // fallback below is the exact pre-existing behavior, unchanged, so a
  // stale/foreign card never turns into a wrong "not found" (or wrongly
  // "found") answer; it only ever costs the same as before.
  if (const auto* card = session.shadowVarlistIfCurrent()) {
    for (const auto& v : varlist) {
      bool found = false;
      for (const auto& info : *card) {
        if (info.name == v) {
          found = true;
          break;
        }
      }
      if (!found) return engineErrorBlock("variable " + v + " not found");
    }
  } else {
    Datastore* ds = session.datastore();
    if (ds == nullptr) return eval::noDataLoaded();
    for (const auto& v : varlist) {
      if (!ds->has(v)) return engineErrorBlock("variable " + v + " not found");
    }
  }
  // BUG-532: Stata's `sort K` on data whose marker already begins with K moves
  // no row and sets the marker to K (w2e_probes.log bo_sort_prefix_noop: "g x"
  // -> `sort g` -> "g"; bo_sort_prefix_then_by). That is the engine's gsort as
  // is (Frame::gsort skips the radix pass there), so this handler neither
  // reads the marker nor opens the head's Datastore: on the resident plane
  // that read materialized the whole dataset on every sort and every bysort
  // line (review of e19291ad).
  // BS-001 fix: this used to shell out to `eval --data ... --sort <vars>`,
  // but cobra's `eval` verb (cmds_data.cpp's cmd_eval) has no `--sort` flag
  // at all — its option-parsing loop only recognizes a fixed set of flags
  // (--xtset/--gen/--egen/.../--keepif/--summarize) and silently ignores any
  // OTHER flag/value pair without error (no catch-all reject branch), so
  // `--sort <vars>` was a pure no-op: cobra ran, wrote the SAME row order
  // back out, and PIET reported success. Real row reordering lives in
  // cobra's dedicated `gsort` verb (cmds_data.cpp's cmd_gsort / Frame::gsort,
  // std::stable_sort — see runGsort in handlers_data_verbs.cpp, the already-
  // correct sibling this now mirrors). Stata's plain `sort` is that same
  // stable multi-key sort with every key ascending (no [+-] prefix), so the
  // varlist is forwarded unprefixed. `gsort` writes its output file directly
  // and prints a plain-text status line (JSON only on error), so this must
  // use runCobra (not runCobraJson, which requires a JSON stdout payload).
  std::string outPath = nextPath(session.workdir(), "sort");
  std::string joined;
  for (size_t i = 0; i < varlist.size(); ++i) {
    if (i) joined += ",";
    joined += varlist[i];
  }
  std::vector<std::string> args = {"gsort", "--data", *session.currentHbin(), "--vars", joined,
                                    "--out", outPath};
  // MAXPERF Stage 2 (item 3): a pure row-reorder — single terminal mutation,
  // --out never read back as a file. Opt into the resident frame plane.
  SessionState::ResidentEligible resident(session);
  CobraRunResult cr = runCobra(args);
  if (!cr.ok) {
    CobraError ce = errorFromRaw(cr);
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  session.load(outPath);
  return mutationResult(session, "sort");
}

// BS-092 (BETA campaign, Phase G): a NON-MUTATING sibling of runSort/
// runGsort above — sorts `basePath` by `byVars` (ascending, stable, same
// `gsort` verb and shape as runSort) into a FRESH scratch .hbin instead of
// calling session.load() on the result. dispatcher.cpp's `by:`/`bysort:
// summarize` grouped-dispatch loop needs the PHYSICALLY contiguous
// same-group row blocks a stable sort produces (so each group can be sliced
// out via piet::handlers::applyIfAndInFilter's plain row-range mechanism),
// but summarize is a READ-ONLY command — it must never reorder the live
// session dataset a user has loaded, unlike a real `sort`/`bysort`'s own
// (documented) side effect on gen/replace's target. Throws CobraError on
// failure (same convention as applyIfAndInFilter), not an ojson error
// block, so callers can wrap it in their own try/catch around the whole
// per-group loop instead of checking a sentinel after every call.
std::string sortForGrouping(SessionState& session, const std::string& basePath,
                            const std::vector<std::string>& byVars) {
  std::string joined;
  for (size_t i = 0; i < byVars.size(); ++i) {
    if (i) joined += ",";
    joined += byVars[i];
  }
  std::string outPath = nextPath(session.workdir(), "bygroup_sort");
  std::vector<std::string> args = {"gsort", "--data", basePath, "--vars", joined, "--out", outPath};
  CobraRunResult cr = runCobra(args);
  if (!cr.ok) throw errorFromRaw(cr);
  return outPath;
}

// ---------------------------------------------------------------------------
// count [if COND]

namespace {

// Q2 (gate ②③): native fast path for `count if [in]`.
//
// runCount used to route every if/in through applyIfAndInFilter -> cobra
// keepif, which spawns a subprocess and writes a COMPLETE filtered .hbin to
// disk just to read back its row count (~0.66s on a 108K×192 corpus vs a
// ~µs floor). This evaluates the predicate directly over the ALREADY-mmap'd
// Datastore columns instead.
//
// CORRECTNESS-FIRST (gate ①, silent divergence forbidden): parity is with
// TODAY's cobra keepif pipeline (src/data/frame.cpp Parser+ev()+keep_if),
// which is Stata-verified. piet-native does NOT link that engine (subprocess
// only), so ev() is PORTED here 1:1 for a deliberately small whitelist, and
// ANYTHING the port can't prove identical throws CountFastBail -> runCount
// falls back to the UNCHANGED cobra path. Falling back is always safe (cobra
// is the ground truth and re-produces today's result OR today's error), so the
// only obligation is that an ACCEPTED expression computes bit-identically.
//
// Accepted grammar (mirrors frame.cpp exactly): numeric literals (same strict
// scan — a token std::stod won't fully consume, or a hex 0x.., bails), the
// bare `.` missing literal, f64 variable references, `factorvar ==/!=
// "literal"` (resolved to a level-code compare, missing code -> +inf, exactly
// like frame.cpp eqne), unary +/-/!, ( ), + - * / ^ (all wrapped in fin():
// non-finite -> missing), the six comparisons (both sides through mi():
// missing = +infinity), & | (a missing operand is TRUE, as Stata reads it),
// and missing()/inrange()/inlist(). Everything else BAILS: _n / _N
// / _pi / subscripts var[..] (the first two even ERROR in cobra's non-bysort
// keep_if — bailing reproduces that error), any other function, a factor used
// as a number or any string/number mix, and any parse error / trailing input.
//
// Missing-value semantics are the crux and are taken verbatim from frame.cpp:
// mi() maps NaN->+inf for comparisons, fin() maps non-finite->NaN for
// arithmetic, and `!`, `&`, `|` read an operand through truthy(): nonzero is
// true and a missing value is nonzero, so a missing operand is TRUE ([U]
// 13.2.4; w2c_functions.log operators_missing), and the result is always 0/1.
// Until BUG-620 both layers read a missing `&`/`|` operand as false and `!`
// passed it through as missing; they changed together, so they never disagree.
// See runCount for the keep-truthiness rule, one for if-only and if+in alike
// (BUG-534), derived from frame.cpp keep_if + the fallback's
// `((if)!=0)&(inrange)`, not guessed.
struct CountFastBail {};

constexpr double kCiNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kCiInf = std::numeric_limits<double>::infinity();
inline double ciMi(double v) { return std::isnan(v) ? kCiInf : v; }        // frame.cpp mi()
inline double ciFin(double v) { return std::isfinite(v) ? v : kCiNaN; }    // frame.cpp fin()
inline bool ciTruthy(double v) { return std::isnan(v) || v != 0.0; }       // frame.cpp truthy()
// frame.cpp scmp() — Stata's total order with the extended missings `.a`-`.z`
// ordered above `.` (data/missing.h). This fast path SHADOWS the engine for
// every `count if`, so it has to carry the same semantics or PIET answers a
// different question than cobra does for the identical expression.
inline int ciCmp(double a, double b) { return cecon::stata_cmp(a, b); }

struct CiNode {
  enum K {
    Num, VarF64, FactorCmp, FactorRef, StrLit,
    Neg, Not, Add, Sub, Mul, Div, Pow,
    Lt, Gt, Le, Ge, Eq, Ne, And, Or,
    FnMissing, FnInrange, FnInlist
  } kind = Num;
  double num = 0.0;
  const double* col = nullptr;                        // VarF64
  const int32_t* codes = nullptr;                     // FactorRef / FactorCmp
  const std::vector<std::string>* levels = nullptr;   // FactorRef (dictionary)
  long factorIdx = 0;                                 // FactorCmp: level code
  bool eq = true;                                     // FactorCmp: == vs !=
  std::string sval;                                   // StrLit
  std::unique_ptr<CiNode> a, b;
  std::vector<std::unique_ptr<CiNode>> args;          // Fn args
};

// 1:1 port of frame.cpp ev(). Only reaches node kinds the parser actually
// produces; FactorRef/StrLit are guarded out at parse time (they bail).
double ciEval(const CiNode* n, int64_t row) {
  switch (n->kind) {
    case CiNode::Num: return n->num;
    case CiNode::VarF64: return n->col[row];
    case CiNode::FactorCmp: {
      double c = n->codes[row] < 0 ? kCiInf : static_cast<double>(n->codes[row]);
      double t = static_cast<double>(n->factorIdx);
      return n->eq ? (c == t ? 1.0 : 0.0) : (c != t ? 1.0 : 0.0);
    }
    case CiNode::Neg: return -ciEval(n->a.get(), row);
    case CiNode::Not: return ciTruthy(ciEval(n->a.get(), row)) ? 0.0 : 1.0;
    case CiNode::Add: return ciFin(ciEval(n->a.get(), row) + ciEval(n->b.get(), row));
    case CiNode::Sub: return ciFin(ciEval(n->a.get(), row) - ciEval(n->b.get(), row));
    case CiNode::Mul: return ciFin(ciEval(n->a.get(), row) * ciEval(n->b.get(), row));
    case CiNode::Div: return ciFin(ciEval(n->a.get(), row) / ciEval(n->b.get(), row));
    case CiNode::Pow: {
      // frame.cpp's `^`: Stata's power (core/stata_arith.h), not libm pow.
      const double x = ciEval(n->a.get(), row);
      return ciFin(cecon::stata_pow_value(x, ciEval(n->b.get(), row)));
    }
    case CiNode::Lt: return ciCmp(ciEval(n->a.get(), row), ciEval(n->b.get(), row)) <  0 ? 1.0 : 0.0;
    case CiNode::Gt: return ciCmp(ciEval(n->a.get(), row), ciEval(n->b.get(), row)) >  0 ? 1.0 : 0.0;
    case CiNode::Le: return ciCmp(ciEval(n->a.get(), row), ciEval(n->b.get(), row)) <= 0 ? 1.0 : 0.0;
    case CiNode::Ge: return ciCmp(ciEval(n->a.get(), row), ciEval(n->b.get(), row)) >= 0 ? 1.0 : 0.0;
    case CiNode::Eq: return ciCmp(ciEval(n->a.get(), row), ciEval(n->b.get(), row)) == 0 ? 1.0 : 0.0;
    case CiNode::Ne: return ciCmp(ciEval(n->a.get(), row), ciEval(n->b.get(), row)) != 0 ? 1.0 : 0.0;
    case CiNode::And: { double x = ciEval(n->a.get(), row), y = ciEval(n->b.get(), row);
      return (ciTruthy(x) && ciTruthy(y)) ? 1.0 : 0.0; }
    case CiNode::Or: { double x = ciEval(n->a.get(), row), y = ciEval(n->b.get(), row);
      return (ciTruthy(x) || ciTruthy(y)) ? 1.0 : 0.0; }
    case CiNode::FnMissing:
      for (auto& q : n->args) if (std::isnan(ciEval(q.get(), row))) return 1.0;
      return 0.0;
    case CiNode::FnInrange: {
      double z = ciEval(n->args[0].get(), row), lo = ciEval(n->args[1].get(), row),
             hi = ciEval(n->args[2].get(), row);
      if (std::isnan(z)) return 0.0;
      bool lm = std::isnan(lo), hm = std::isnan(hi);
      if (lm && hm) return 1.0;
      if (lm) return z <= hi ? 1.0 : 0.0;
      if (hm) return z >= lo ? 1.0 : 0.0;
      return (lo <= z && z <= hi) ? 1.0 : 0.0;
    }
    case CiNode::FnInlist: {
      double z = ciEval(n->args[0].get(), row);
      for (size_t k = 1; k < n->args.size(); ++k) {
        double v = ciEval(n->args[k].get(), row);
        if ((std::isnan(z) && std::isnan(v)) || z == v) return 1.0;
      }
      return 0.0;
    }
    case CiNode::FactorRef:
    case CiNode::StrLit:
      throw CountFastBail{};  // never legal outside a factor ==/!= "literal"
  }
  return kCiNaN;
}

// Recursive-descent parser mirroring frame.cpp's grammar
// (or > and > cmp(!= > < <= >= ==, one level each) > add > mul > neg(- !) > pow(left) > unary(- !)
// > primary). Every path that
// frame.cpp's Parser would err() on, or that this port can't prove identical,
// throws CountFastBail so the caller can fall back to cobra.
struct CiParser {
  const std::string& s;
  size_t p = 0;
  Datastore& ds;
  int depth = 0;
  CiParser(const std::string& src, Datastore& d) : s(src), ds(d) {}

  void ws() { while (p < s.size() && std::isspace(static_cast<unsigned char>(s[p]))) ++p; }
  bool eat(const char* tok) {
    ws();
    size_t n = 0;
    while (tok[n]) ++n;
    if (s.compare(p, n, tok) == 0) { p += n; return true; }
    return false;
  }
  static std::unique_ptr<CiNode> mk(CiNode::K k) { auto n = std::make_unique<CiNode>(); n->kind = k; return n; }
  static bool isOperand(const CiNode& n) { return n.kind != CiNode::FactorRef && n.kind != CiNode::StrLit; }
  std::unique_ptr<CiNode> bin(CiNode::K k, std::unique_ptr<CiNode> a, std::unique_ptr<CiNode> b) {
    // A factor/string operand is legal ONLY as a direct child of a factor
    // ==/!= "literal" comparison (handled in cmp/eqne). Anywhere else, cobra
    // ev() throws "string/factor used where a number is required" -> bail.
    if (!isOperand(*a) || !isOperand(*b)) throw CountFastBail{};
    auto n = mk(k); n->a = std::move(a); n->b = std::move(b); return n;
  }
  struct DepthGuard { int& d; explicit DepthGuard(int& x) : d(x) { if (++d > 1000) throw CountFastBail{}; } ~DepthGuard() { --d; } };

  std::unique_ptr<CiNode> parse() {
    auto n = orx();
    ws();
    if (p != s.size()) throw CountFastBail{};  // trailing input
    return n;
  }
  std::unique_ptr<CiNode> orx() { auto a = andx(); while (eat("|")) a = bin(CiNode::Or, std::move(a), andx()); return a; }
  std::unique_ptr<CiNode> andx() { auto a = cmp(); while (eat("&")) a = bin(CiNode::And, std::move(a), cmp()); return a; }
  // FU-844 (frame.cpp cmp/eatCmp, same levels): each comparison operator is its
  // own left-associative level, tightest first != > < <= >= == (oracle
  // w2ac_expr_relational count_<o1>_<o2>).
  static constexpr int kCmpLevels = 6;
  bool eatCmp(int lvl) {
    ws();
    static const char* const kOp[kCmpLevels] = {"==", ">=", "<=", "<", ">", "!="};
    const char* op = kOp[lvl];
    size_t n = std::strlen(op);
    if (lvl == kCmpLevels - 1 && s.compare(p, 2, "~=") == 0) { p += 2; return true; }
    if (s.compare(p, n, op) != 0) return false;
    if (n == 1 && p + 1 < s.size() && s[p + 1] == '=') return false;
    p += n;
    return true;
  }
  std::unique_ptr<CiNode> cmp(int lvl = 0) {
    if (lvl == kCmpLevels) return add();
    auto a = cmp(lvl + 1);
    while (eatCmp(lvl)) {
      auto b = cmp(lvl + 1);
      switch (lvl) {
        case 0: a = eqne(true, std::move(a), std::move(b)); break;
        case 1: a = bin(CiNode::Ge, std::move(a), std::move(b)); break;
        case 2: a = bin(CiNode::Le, std::move(a), std::move(b)); break;
        case 3: a = bin(CiNode::Lt, std::move(a), std::move(b)); break;
        case 4: a = bin(CiNode::Gt, std::move(a), std::move(b)); break;
        default: a = eqne(false, std::move(a), std::move(b)); break;
      }
    }
    return a;
  }
  // frame.cpp eqne: resolve `factorvar ==/!= "literal"` (either order) to a
  // numeric compare against the 0-based level code; a literal that is not a
  // level makes == constant-false / != constant-true.
  std::unique_ptr<CiNode> eqne(bool isEq, std::unique_ptr<CiNode> a, std::unique_ptr<CiNode> b) {
    CiNode* fac = nullptr;
    CiNode* lit = nullptr;
    if (a->kind == CiNode::FactorRef && b->kind == CiNode::StrLit) { fac = a.get(); lit = b.get(); }
    else if (b->kind == CiNode::FactorRef && a->kind == CiNode::StrLit) { fac = b.get(); lit = a.get(); }
    if (fac) {
      long idx = -1;
      for (size_t i = 0; i < fac->levels->size(); ++i)
        if ((*fac->levels)[i] == lit->sval) { idx = static_cast<long>(i); break; }
      if (idx < 0) { auto cst = mk(CiNode::Num); cst->num = isEq ? 0.0 : 1.0; return cst; }
      auto n = mk(CiNode::FactorCmp); n->codes = fac->codes; n->factorIdx = idx; n->eq = isEq;
      return n;
    }
    // lone factor/string, or a string-vs-number mix -> cobra errors -> bail.
    if (a->kind == CiNode::FactorRef || b->kind == CiNode::FactorRef ||
        a->kind == CiNode::StrLit || b->kind == CiNode::StrLit) throw CountFastBail{};
    return bin(isEq ? CiNode::Eq : CiNode::Ne, std::move(a), std::move(b));
  }
  // EM0 (frame.cpp add/sub/mul/div, same grouping): + over -, * over /, each left-associative
  // ([U] 13.2.5; oracle w2am_expr_binary_order eo_cnt_pm / eo_cnt_md).
  std::unique_ptr<CiNode> add() {
    auto a = sub();
    for (;;) { ws(); if (eat("+")) a = bin(CiNode::Add, std::move(a), sub()); else break; }
    return a;
  }
  std::unique_ptr<CiNode> sub() {
    auto a = mul();
    for (;;) { ws(); if (eat("-")) a = bin(CiNode::Sub, std::move(a), mul()); else break; }
    return a;
  }
  std::unique_ptr<CiNode> mul() {
    auto a = div();
    for (;;) { ws(); if (eat("*")) a = bin(CiNode::Mul, std::move(a), div()); else break; }
    return a;
  }
  std::unique_ptr<CiNode> div() {
    auto a = neg();
    for (;;) { ws(); if (eat("/")) a = bin(CiNode::Div, std::move(a), neg()); else break; }
    return a;
  }
  // FU-811 (frame.cpp neg/powx/unary, same tree and bail rules): negation and
  // `!` bind looser than `^`, which is left-associative, so -a^2 is -(a^2),
  // !a^0 is !(a^0), 2^3^2 is (2^3)^2 and 2^-a^2 is (2^(-a))^2
  // (oracle w2ac_expr_precedence count_neg_pow / count_not_pow0 /
  // count_chain_const / count_pow_neg_pow). `~` is `!`; a sign after `!` is
  // frame.cpp's syntax error -> bail.
  bool eatNot() {
    if (p >= s.size() || (s[p] != '!' && s[p] != '~') || (p + 1 < s.size() && s[p + 1] == '=')) return false;
    ++p;
    ws();
    if (p < s.size() && (s[p] == '-' || s[p] == '+')) throw CountFastBail{};
    return true;
  }
  std::unique_ptr<CiNode> neg() {
    DepthGuard g(depth);
    ws();
    if (eatNot()) {
      auto n = mk(CiNode::Not); n->a = neg();
      if (!isOperand(*n->a)) throw CountFastBail{};
      return n;
    }
    if (eat("-")) { auto n = mk(CiNode::Neg); n->a = neg(); if (!isOperand(*n->a)) throw CountFastBail{}; return n; }
    if (eat("+")) return neg();
    return powx();
  }
  std::unique_ptr<CiNode> powx() {
    auto a = primary();
    for (;;) {
      ws();
      if (!eat("^")) break;
      a = bin(CiNode::Pow, std::move(a), unary());
    }
    return a;
  }
  // The right operand of `^`: leading signs / `!` apply to the next primary only.
  std::unique_ptr<CiNode> unary() {
    DepthGuard g(depth);
    ws();
    if (eatNot()) {
      auto n = mk(CiNode::Not); n->a = unary();
      if (!isOperand(*n->a)) throw CountFastBail{};
      return n;
    }
    if (eat("-")) { auto n = mk(CiNode::Neg); n->a = unary(); if (!isOperand(*n->a)) throw CountFastBail{}; return n; }
    if (eat("+")) return unary();
    return primary();
  }
  std::vector<std::unique_ptr<CiNode>> arglist() {  // '(' already consumed
    std::vector<std::unique_ptr<CiNode>> v;
    ws();
    if (p < s.size() && s[p] == ')') { ++p; return v; }
    v.push_back(orx());
    while (eat(",")) v.push_back(orx());
    if (!eat(")")) throw CountFastBail{};
    return v;
  }
  std::unique_ptr<CiNode> primary() {
    ws();
    if (eat("(")) { auto n = orx(); if (!eat(")")) throw CountFastBail{}; return n; }
    if (p < s.size() && s[p] == '"') {
      ++p; size_t st = p;
      while (p < s.size() && s[p] != '"') ++p;
      if (p >= s.size()) throw CountFastBail{};  // unterminated
      auto n = mk(CiNode::StrLit); n->sval = s.substr(st, p - st); ++p; return n;
    }
    // extended missing literal `.a` … `.z` (frame.cpp) — one lowercase letter
    // after the dot that does not continue into an identifier.
    if (p + 1 < s.size() && s[p] == '.' && s[p + 1] >= 'a' && s[p + 1] <= 'z' &&
        (p + 2 >= s.size() ||
         (!cecon::ident::isIdentByte(s[p + 2]) && s[p + 2] != '.'))) {
      int kind = cecon::kMissA + (s[p + 1] - 'a');
      p += 2;
      auto n = mk(CiNode::Num);
      n->num = cecon::stata_missing(kind);
      return n;
    }
    // bare missing literal: a '.' not part of a number (frame.cpp).
    if (p < s.size() && s[p] == '.' && (p + 1 >= s.size() || !std::isdigit(static_cast<unsigned char>(s[p + 1])))) {
      ++p; auto n = mk(CiNode::Num); n->num = kCiNaN; return n;
    }
    if (p < s.size() && (std::isdigit(static_cast<unsigned char>(s[p])) || s[p] == '.')) {
      size_t st = p;
      while (p < s.size() && (std::isalnum(static_cast<unsigned char>(s[p])) || s[p] == '.' ||
                              ((s[p] == '+' || s[p] == '-') && (s[p - 1] == 'e' || s[p - 1] == 'E'))))
        ++p;
      std::string tok = s.substr(st, p - st);
      for (char c : tok) if (c == 'x' || c == 'X') throw CountFastBail{};  // hex: Stata rejects
      size_t consumed = 0;
      double val;
      try { val = std::stod(tok, &consumed); } catch (...) { throw CountFastBail{}; }
      if (consumed != tok.size()) throw CountFastBail{};  // partial consume -> reject (frame.cpp C1-F11)
      auto n = mk(CiNode::Num); n->num = val; return n;
    }
    // A Unicode name (`قيمة > 2`) is one identifier (core/ident.h); it is looked up
    // byte-exactly against the dataset like any other.
    if (p < s.size() && cecon::ident::isIdentStartByte(s[p])) {
      size_t st = p;
      while (p < s.size() && cecon::ident::isIdentByte(s[p])) ++p;
      std::string id = s.substr(st, p - st);
      // _pi / _n / _N: not fast-pathed (_n/_N even ERROR in cobra's non-bysort
      // keep_if — bailing reproduces that; _pi is just rare here).
      if (id == "_pi" || id == "_n" || id == "_N") throw CountFastBail{};
      ws();
      if (p < s.size() && s[p] == '(') {  // function call
        ++p;
        auto args = arglist();
        for (auto& q : args) if (!isOperand(*q)) throw CountFastBail{};  // no factor/string args
        int na = static_cast<int>(args.size());
        if (id == "missing" && na >= 1) { auto n = mk(CiNode::FnMissing); n->args = std::move(args); return n; }
        if (id == "inrange" && na == 3) { auto n = mk(CiNode::FnInrange); n->args = std::move(args); return n; }
        if (id == "inlist" && na >= 2) { auto n = mk(CiNode::FnInlist); n->args = std::move(args); return n; }
        throw CountFastBail{};  // any other function -> cobra handles/errors it
      }
      if (!ds.has(id)) throw CountFastBail{};  // unknown variable -> cobra errors
      const cecon::Column& c = ds.col(id);
      if (c.dtype == cecon::DType::Factor) {
        ws();
        if (p < s.size() && s[p] == '[') throw CountFastBail{};  // subscript -> bail
        auto fv = mk(CiNode::FactorRef); fv->codes = c.codes(); fv->levels = &c.levels;
        return fv;
      }
      auto v = mk(CiNode::VarF64); v->col = c.f64();
      ws();
      if (p < s.size() && s[p] == '[') throw CountFastBail{};  // subscript -> bail
      return v;
    }
    throw CountFastBail{};  // unexpected token
  }
};

}  // namespace

ojson runCount(const std::optional<std::string>& ifExpr, SessionState& session,
              const std::optional<std::string>& inRange) {
  // `_rc` (FU-169) reaches here already resolved: every caller's condition went
  // through the dispatcher's row pre-pass (substituteRowExprRefs, BUG-1359).
  bool haveIf = ifExpr.has_value() && !ifExpr->empty();
  bool haveIn = inRange.has_value() && !inRange->empty();
  // BUG-032 / BUG-412: a condition over a factor (string) column, a string
  // function or a string literal is counted row-wise; a co-occurring `in` is
  // ANDed into the same mask (BS-090). The fast path and the engine below read
  // `svar == ""` as constant-false, because a blank is not a dictionary level.
  // This used to sit in the dispatcher's `count` arm only, so `assert`, which
  // counts its contradictions here (runAssert), took the numeric path: `assert
  // g == "" in 3/4` called every blank row a contradiction.
  if (haveIf && strmutate::needsStringCount(session, ifExpr)) {
    strmutate::CountResult cr = strmutate::countRowsIf(session, *ifExpr, inRange);
    if (!cr.ok && cr.inRangeError) {
      // The one `in` refusal the numeric path below gives too: `assert g == ""
      // in 0/2` is "'0' invalid observation number" r(198), as in Stata.
      InRangeFilterResult fr;
      fr.ok = false;
      fr.refusal = cr.errMsg;
      return inRangeRejectError(fr, *inRange, "count");
    }
    if (!cr.ok) {
      // r(109) is Stata's "type mismatch" whichever row raised it (`count if s`,
      // `assert s`: StataBE 19.5).
      if (!cr.errReason.empty())
        return eval::errorBlock("engine_error",
                                cr.errReason == "r(109)" ? std::string("type mismatch") : cr.errMsg,
                                std::nullopt, std::nullopt, std::nullopt, cr.errReason);
      return eval::engineError(cr.errMsg, "engine_error");
    }
    session.eval.rValues.set("N", static_cast<double>(cr.n));
    ojson out;
    out["type"] = "display";
    out["text"] = std::to_string(cr.n);
    return out;
  }
  // C4-L2A (D1 card plane, C4-RESIDENT-READ-SCOPE-brief §1/§4): a bare
  // `count` needs only _N, which the resident card already carries
  // (residentRowsIfCurrent — same honest-currency rule as
  // shadowVarlistIfCurrent, nullopt under PIET_NO_RESIDENT_READ=1 or a
  // non-current/rows-unknown card → the unchanged datastore() path below).
  // Answering here skips the Datastore open (mmap + whole-file FNV) and, on
  // a dirty virtual head, the EMH frame snapshot that open forces. Output is
  // byte-identical to the ds->nRows() path: same r(N), same display text.
  if (!haveIf && !haveIn) {
    if (std::optional<int64_t> rows = session.residentRowsIfCurrent()) {
      session.eval.rValues.set("N", static_cast<double>(*rows));
      ojson out;
      out["type"] = "display";
      out["text"] = std::to_string(*rows);
      return out;
    }
  }
  // ONESHOT-GETCOLS2: `count if` needs the columns its predicate names, not the
  // frame. session.datastore() flushes the whole resident head to disk to supply
  // them — 56 of the 98 flushes left after the mutation entrance moved. Fetch the
  // predicate's columns instead (contract_A §38); every failure falls through to
  // the ordinary path with nothing lost.
  //
  // Row COUNT is preserved by a projection, so nRows() and the `in` bounds below
  // read the same numbers either way. The no-if/no-in case never gets here — the
  // shadow card answered it above — so a fetch is only attempted when there is a
  // predicate whose columns are worth fetching.
  std::string countMiniPath;
  std::unique_ptr<Datastore> countMini;
  if (haveIf) {
    if (auto cols = referencedColumns(session, {&*ifExpr}, ""))
      countMini = fetchColumns(session, *cols, session.workdir(), &countMiniPath);
  }
  Datastore* ds = countMini ? countMini.get() : session.datastore();
  if (ds == nullptr) return eval::noDataLoaded();
  int64_t n = 0;

  // Q2 (gate ②③) native fast path — evaluate `if`/`in` over the mmap'd columns
  // rather than spawning cobra keepif + writing a full filtered .hbin. Anything
  // the native evaluator can't prove identical to cobra keep_if throws
  // CountFastBail and we drop to the UNCHANGED applyIfAndInFilter path.
  bool fastPathDone = false;
  if (!haveIf && !haveIn) {
    n = ds->nRows();
    fastPathDone = true;
  } else {
    // Resolve `in` bounds natively (same parseInRangeBounds the fallback uses),
    // clamped to the real row set exactly like cobra's `idx>=a & idx<=b` over a
    // 1-based row index. A malformed/negative bound is the SAME user error
    // either way, surfaced identically (byte-identical InRangeFilterResult).
    int64_t lo1 = 1, hi1 = ds->nRows();
    if (haveIn) {
      InRangeBounds rr = parseInRangeBounds(*inRange, ds->nRows());
      if (!rr.ok) {
        InRangeFilterResult fr;
        fr.ok = false;
        fr.refusal = rr.refusal;
        return inRangeRejectError(fr, *inRange, "count");
      }
      lo1 = std::max<int64_t>(1, rr.a);
      hi1 = std::min<int64_t>(ds->nRows(), rr.b);
    }
    if (haveIf) {
      try {
        CiParser parser(*ifExpr, *ds);
        std::unique_ptr<CiNode> ast = parser.parse();
        // A bare factor/string predicate (`count if svar`) is a number-context
        // misuse cobra keep_if errors on — bail so the fallback reproduces it.
        if (ast->kind == CiNode::FactorRef || ast->kind == CiNode::StrLit) throw CountFastBail{};
        int64_t cnt = 0;
        for (int64_t i = lo1; i <= hi1; ++i) {
          double v = ciEval(ast.get(), i - 1);
          // Keep-truthiness parity with the fallback — ONE rule, if-only or
          // if+in alike: a row counts iff `(isnan(v) || v != 0)`, a missing
          // predicate being TRUE in Stata ([U] 13.2.4). If-only, that is
          // frame.cpp keep_if itself. If+in, the fallback runs
          // `((ifExpr)!=0) & (idx-range)` (BUG-534, applyIfAndInFilter);
          // before that it ran `(ifExpr) & (idx-range)`, whose engine `&`
          // then read a missing operand as false (BUG-620), and this line
          // mirrored it (`!isnan(v) && v != 0`), so `count if x in 1/5`
          // skipped the missing-x rows Stata counts.
          bool keep = ciTruthy(v);
          if (keep) ++cnt;
        }
        n = cnt;
        fastPathDone = true;
      } catch (const CountFastBail&) {
        // if-expr uses a non-fast-pathable construct -> cobra fallback below.
      }
    } else {
      // `in`-only: keep_if on the range predicate keeps exactly the in-range
      // rows — no expression to evaluate.
      n = hi1 >= lo1 ? (hi1 - lo1 + 1) : 0;
      fastPathDone = true;
    }
  }

  if (!fastPathDone) {
    // Fallback (UNCHANGED from pre-Q2). BS-090 (BETA Phase G): `in` composes
    // with `if` via AND (piet::handlers::applyIfAndInFilter).
    InRangeFilterResult fr;
    try {
      // The filter must run on the session's REAL head, never on a §38 projection:
      // the projection carries only the predicate's columns, and this fallback's
      // result is read back as a dataset. `ds->path()` is the projection whenever
      // the fetch above succeeded, so ask the session instead.
      const std::string headPath =
          countMini ? session.currentHbin().value_or(ds->path()) : ds->path();
      fr = applyIfAndInFilter(session, headPath, ifExpr, inRange);
    } catch (const CobraError& ce) {
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    if (!fr.ok) return inRangeRejectError(fr, *inRange, "count");
    Datastore filtered(fr.dataPath);
    n = filtered.nRows();
  }
  // dispatcher.py's count handler: `session._r_values["N"] = float(n)` —
  // count is an r-class command (r(N) available to a following `display
  // r(N)`), and the result block is `{"type": "display", ...}`, NOT echo.
  session.eval.rValues.set("N", static_cast<double>(n));
  ojson out;
  out["type"] = "display";
  out["text"] = std::to_string(n);
  return out;
}

// ---------------------------------------------------------------------------
// list (browse.py's run_browse in read-only list mode — dispatcher.py's
// `handler == "browse"` branch; `editable` is always false for `list`)

ojson runList(const std::optional<std::vector<std::string>>& varlist,
              const std::optional<std::string>& ifExpr, const std::optional<std::string>& inRange,
              SessionState& session) {
  Datastore* ds = session.datastore();
  if (ds == nullptr) return eval::noDataLoaded();

  int64_t browseOffset = 0;
  int64_t browseLimit = 50;
  bool haveInRange = inRange.has_value() && !inRange->empty();
  bool haveIfExpr = ifExpr.has_value() && !ifExpr->empty();
  if (haveInRange) {
    // BUG-672/673: the shared [U] 11.1.4 resolver. list's own parser clamped
    // every bound into range, so `list in -3/-1` listed row 1 and `list in
    // 1/50` on 12 rows listed all 12 with rc 0 (Stata: rows 10-12; r(198)).
    InRangeBounds rr = parseInRangeBounds(*inRange, ds->nRows());
    if (!rr.ok) {
      InRangeFilterResult fr;
      fr.ok = false;
      fr.refusal = rr.refusal;
      return inRangeRejectError(fr, *inRange, "list");
    }
    browseOffset = haveIfExpr ? 0 : rr.a - 1;
    browseLimit = rr.b - rr.a + 1;
  }

  // BUG-025: thread session.valueLabels through so value-labeled numeric
  // variables render label TEXT (mirrors browse.py's run_browse, which
  // always passes session.get_value_label_map() to Datastore.browse — list
  // and browse share the same underlying renderer in the oracle).
  auto vlMap = session.valueLabelMapForBrowse();
  // format-display-default-lifecycle: every listed column carries the format
  // its cells display in, from the one resolver (SessionState::displayFormat):
  // a `format` command's, the file's own, else the declared type's default
  // (%10.0g for a double, %12.0g for a long, %#s for a str#).
  auto fmtMap = session.displayFormatMap(ds->formatCard(varlist));

  ojson block;
  if (haveIfExpr) {
    // BUG-674: `if` and `in` compose as Stata's AND over the ORIGINAL row
    // numbers — the shared applyIfAndInFilter chain. The old path windowed the
    // `if`-filtered rows instead, so `list x if x>2 in 1/5` listed x = 3..7
    // (the first five matches) where Stata lists 3, 4, 5. An if-only call is
    // the same single --keepif this used to issue (BUG-369's ts injection
    // included).
    InRangeFilterResult fr;
    const std::string basePath = ds->path();  // a copy: the chain may touch the head
    try {
      fr = applyIfAndInFilter(session, basePath, ifExpr, inRange);
    } catch (const CobraError& ce) {
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    if (!fr.ok) return inRangeRejectError(fr, *inRange, "list");
    Datastore tmpDs(fr.dataPath);
    block = tmpDs.browse(browseOffset, browseLimit, varlist, vlMap, fmtMap);
    cecon::native_unlink(fr.dataPath);
  } else {
    block = ds->browse(browseOffset, browseLimit, varlist, vlMap, fmtMap);
  }
  block["editable"] = false;
  if (haveInRange || haveIfExpr) {
    block["windowed"] = true;
    block["n_rows_total"] = static_cast<int64_t>(block.value("rows", ojson::array()).size());
  }
  return block;
}

// ---------------------------------------------------------------------------
// scalar list / dir

ojson runScalarList(const std::vector<std::string>& names, SessionState& session) {
  // BS-038 (BETA campaign C3): real Stata's `scalar list`/`scalar dir`
  // column format — name right-justified to width 10, " = ", value
  // right-justified to width 10 (oracle: `      sca1 =         42`).
  // Values are emitted AS ALREADY STORED (session.eval.scalars' own
  // decimal-string convention, e.g. "42"/"19.5" from the arithmetic
  // evaluator) rather than re-deriving Stata's full %10.0g significant-
  // digit formatter — exact for the common case; a value longer than 10
  // chars is left un-truncated rather than silently mis-rendered (a
  // disclosed, smaller gap than Stata's own overflow/scientific-notation
  // rule, not attempted here). This handler previously existed but was
  // dead code — eval/macros.cpp's tryCaptureDefinition intercepted the
  // bare `scalar list`/`scalar dir` line earlier in the pipeline and threw
  // the result away (a bool-only capture signature has no way to carry a
  // display block) before dispatch ever reached this arm; see that file's
  // own BS-038 comment for the other half of the fix.
  auto padRight10 = [](const std::string& s) {
    return s.size() >= 10 ? s : std::string(10 - s.size(), ' ') + s;
  };
  std::vector<std::string> lines;
  const auto& items = session.eval.scalars.items();
  for (const auto& [name, val] : items) {
    if (!names.empty() && std::find(names.begin(), names.end(), name) == names.end()) continue;
    lines.push_back(padRight10(name) + " = " + padRight10(val));
  }
  // "display", not "echo": `echo` is the command line, which `noisily` drops and
  // `quietly` keeps -- the reverse of what the prefixes do to a listing (BUG-1505,
  // as runEreturnList below).
  ojson out;
  out["type"] = "display";
  std::string text;
  for (size_t i = 0; i < lines.size(); ++i) {
    if (i) text += "\n";
    text += lines[i];
  }
  out["text"] = text;
  return out;
}

// ---------------------------------------------------------------------------
// ereturn list — BS-039 (BETA campaign C3): display-only dump of the e()
// values PIET actually stores after an estimation (session.lastModel() +
// its cached raw cobra --tidy JSON, session_state.h's LastModel — PIET's
// e()-equivalent). Bare `ereturn list` ONLY; `ereturn scalar`/`ereturn
// post`/eclass creation stay OUT of scope (BS-109) — dispatcher.cpp's arm
// only routes the bare "list" sub-form here, so any other `ereturn ...`
// form still falls through to the pre-existing generic unsupported_command
// reject, unchanged.
//
// Honest partial dump beats a fabricated complete one: every section below
// emits ONLY fields verified present in PIET's own regress/feols result —
// e(ll)/e(ll_0)/e(rank) and the Stata-internal plumbing macros (e(title),
// e(marginsok), e(properties), e(predict), e(model), e(estat_cmd)) are NOT
// tracked anywhere in this build and are silently omitted rather than
// invented. e(cmd) is a known, disclosed approximation: LastModel only
// tracks the INTERNAL engine command name (cobraCmd, e.g. "feols" for
// regress/reg/r/reghdfe alike) — there is no separate "what the user
// actually typed" field, so e(cmd) can read "feols" where real Stata would
// say "regress" (a pre-existing LastModel gap, not something this
// display-only fix should widen its diff to also close).
ojson runEreturnList(SessionState& session) {
  const std::optional<LastModel>& lm = session.lastModel();
  // eb-order T-P3: an e() can be live with no fitted model behind it (an
  // estpost posting, `ereturn post`, a restored estpost store); its stores are
  // listed like any other (SPEC C, `ereturn list` after estpost).
  if (!lm.has_value() && !session.eval.eLive()) {
    // Real Stata: `ereturn list` with nothing estimated yet errors ("last
    // estimates not found", r(301)) rather than printing an empty/
    // misleading dump.
    return engineErrorBlockRc("last estimates not found", 301);
  }
  const ojson r = lm.has_value() && lm->result.has_value() ? *lm->result : ojson::object();

  auto padLabel = [](const std::string& s) {
    return s.size() >= 22 ? s : std::string(22 - s.size(), ' ') + s;
  };
  auto fmtScalar = [](double v) {
    if (std::isnan(v)) return std::string(".");  // a missing e() scalar (jackknife's e(singleton))
    std::ostringstream oss;
    oss.precision(16);
    oss << v;
    return oss.str();
  };

  std::ostringstream out;
  out << "\nscalars:\n";
  static const std::vector<std::pair<std::string, std::string>> kScalarMap = {
      {"n_obs", "N"}, {"df_m", "df_m"}, {"df_r", "df_r"}, {"F", "F"},
      {"r2", "r2"},   {"rmse", "rmse"}, {"mss", "mss"},   {"rss", "rss"},
      {"adj_r2", "r2_a"},
  };
  std::unordered_set<std::string> emitted;
  for (const auto& [jsonKey, eName] : kScalarMap) {
    if (r.contains(jsonKey) && r.at(jsonKey).is_number()) {
      out << padLabel("e(" + eName + ")") << " =  " << fmtScalar(r.at(jsonKey).get<double>()) << "\n";
      emitted.insert(eName);
    }
  }
  // Anything a handler posted directly to the e-class store. Until AER bundle H
  // this function was driven ENTIRELY by the coeftable JSON plus the fixed map
  // above, so a command like pca -- which has no coeftable and whose returns are
  // e(rho)/e(trace)/e(cond)/e(f)/e(lndet) -- would have printed nothing at all.
  // Appended rather than merged so the regression commands' output is unchanged
  // byte for byte: for those, eScalars is empty and this loop adds no lines.
  for (const auto& [k, v] : session.eval.eScalars.items()) {
    if (emitted.count(k)) continue;  // the coeftable copy already spoke for it
    out << padLabel("e(" + k + ")") << " =  " << fmtScalar(v) << "\n";
  }

  out << "\nmacros:\n";
  // BUG-458 (2026-09-08): read the STORE, which setLastModel now publishes
  // from LastModel (stataCmd -> "regress" where this printed the engine's
  // "feols"; depvar; vce). One source for `e(cmd)', bare e(cmd) and this
  // listing, so they cannot drift. The LastModel fields stay the fallback for
  // a store someone cleared underneath a live model.
  const std::string eCmd = session.eval.eStrings.get("cmd", lm.has_value() ? lm->cobraCmd : "");
  const std::string eDepvar = session.eval.eStrings.get("depvar", lm.has_value() ? lm->depvar : "");
  const std::string eVce = session.eval.eStrings.get("vce", lm.has_value() ? lm->vcov : "");
  if (lm.has_value() || !eCmd.empty()) out << padLabel("e(cmd)") << " : \"" << eCmd << "\"\n";
  if (!eDepvar.empty()) {
    out << padLabel("e(depvar)") << " : \"" << eDepvar << "\"\n";
  }
  if (lm.has_value() || !eVce.empty()) out << padLabel("e(vce)") << " : \"" << eVce << "\"\n";
  // Same rule as the scalars: empty for every pre-H command, so their pins hold.
  for (const auto& [k, v] : session.eval.eStrings.items()) {
    if (k == "cmd" || k == "depvar" || k == "vce") continue;  // said above
    out << padLabel("e(" + k + ")") << " : \"" << v << "\"\n";
  }

  const bool haveCoefTable =
      r.contains("coeftable") && r.at("coeftable").is_array() && !r.at("coeftable").empty();
  if (haveCoefTable || !session.eval.eMatrices.empty()) {
    out << "\nmatrices:\n";
    if (haveCoefTable) {
      size_t k = r.at("coeftable").size();
      // BUG-1207: Stata's e(b) also carries the ancillary parameters the engine's
      // coefficient table leaves out, counted here from the reply (StataBE 19.5:
      // `tobit y x1, ll(5)` 1 x 3 with /var(e.y); ologit on 4 outcome levels 1 x 5
      // with /cut1-/cut3; nbreg 1 x 7 with /lnalpha).
      bool cutRows = false;
      for (const auto& row : r.at("coeftable"))
        if (row.is_object() && row.value("name", std::string()).rfind("/cut", 0) == 0) cutRows = true;
      if (eCmd == "tobit" && r.contains("variance")) {
        k += 1;
      } else if (eCmd == "nbreg" && r.contains("lnalpha")) {
        k += 1;
      } else if ((eCmd == "ologit" || eCmd == "oprobit") && !cutRows && r.contains("cuts") &&
                 r.at("cuts").is_array()) {
        k += r.at("cuts").size();
      }
      out << padLabel("e(b)") << " :  1 x " << k << "\n";
      out << padLabel("e(V)") << " :  " << k << " x " << k << "\n";
    }
    for (const auto& [k, m] : session.eval.eMatrices) {
      if (haveCoefTable && (k == "b" || k == "V")) continue;  // synthesised above
      const size_t nr = m.data.size();
      const size_t nc = nr > 0 ? m.data[0].size() : 0;
      out << padLabel("e(" + k + ")") << " :  " << nr << " x " << nc << "\n";
    }
  }

  // A listing is output, so a "display" block: the prefix filters take an "echo"
  // block for the command line, which `noisily` / `capture noisily` drop and
  // `quietly` keeps, so as "echo" the listing vanished under noisily and leaked
  // under quietly (StataBE 19.5, BUG-1505: noisily prints it, quietly does not).
  ojson block;
  block["type"] = "display";
  block["text"] = out.str();
  return block;
}

// ---------------------------------------------------------------------------
// matrix — P4 sublanguage: literal define, ' / * expressions, rownames/
// colnames, list, accum. Everything downstream that READS a matrix
// (`local : colnames X`, `X[i,j]`, `colsof()`/`rowsof()`) already resolves
// against session.eval.matrices (eval/macros.cpp's substituteMatrixRefs and
// friends) — this section only has to populate that same store, never a
// second one.

std::string matrixNameAsListed(const std::string& name) {
  std::string out;
  out.reserve(name.size());
  for (size_t i = 0; i < name.size(); ++i) {
    if (name.compare(i, 3, "bn.") == 0 && i > 0 &&
        std::isdigit(static_cast<unsigned char>(name[i - 1]))) {
      ++i;  // drop `bn`, keep the `.`
      continue;
    }
    out += name[i];
  }
  return out;
}

namespace {

// Forward declarations for parseMatrixLiteral: the depth-aware top-level
// splitter (defined below with the B3(c) grammar), the one scalar-cell
// evaluator (defined below, next to evalMatrixRhs) and its unary-plus check.
// `lhsSubscript` marks the element-assignment target's subscript (`matrix
// M[i,j] = ...`), the one scalar expression of the sublanguage StataBE reads
// with its general evaluator: a dataset variable there is its first
// observation (`matrix M[x1,1] = 3` is rc 0, 2026-11-03 oracle).
bool splitTopLevel(const std::string& e, std::vector<std::string>& parts,
                   std::vector<char>& ops);
bool evalMatrixScalarExpr(const std::string& expr, SessionState& session, double& outVal,
                          std::string& err, bool lhsSubscript = false);
std::string mxPlusRefusal(const std::string& e);

// True when the bracket at `open` ('(' or '[') is closed by the LAST character
// of `t`, counting both bracket kinds on one depth (as splitTopLevel does).
bool closingBracketIsLast(const std::string& t, size_t open) {
  if (open == std::string::npos || open >= t.size()) return false;
  int depth = 0;
  for (size_t i = open; i < t.size(); ++i) {
    const char c = t[i];
    if (c == '(' || c == '[') ++depth;
    else if (c == ')' || c == ']') {
      if (--depth == 0) return i + 1 == t.size();
    }
  }
  return false;
}

// The first BARE identifier in `expr` that is a key of session.eval.matrices,
// or "" when there is none. A cell or term that mentions a stored matrix this
// way is matrix arithmetic, not the scalar evaluator's (BUG-584). Not bare,
// and so skipped: a call head `f(`, a subscript head `M[` and everything inside
// its brackets (`M[i,j]`, `_b[x]`, scalar context), the first argument of the
// functions whose argument is itself a name -- el/colsof/rowsof/colnumb
// (substituteMatrixRefs resolves them), scalar() (the scalar by definition) and
// the class wrappers e()/r()/c()/s() -- plus string literals, numbers (`1e5`)
// and extended missing values (`.a`).
std::string firstBareMatrixName(const std::string& expr, const SessionState& session) {
  // core/ident.h: a byte >= 0x80 is a name byte, so a Unicode matrix name is one name.
  auto isIdStart = [](char c) { return cecon::ident::isIdentStartByte(c); };
  auto isIdChar = [](char c) { return cecon::ident::isIdentByte(c); };
  auto skipSpace = [&expr](size_t k) {
    while (k < expr.size() && (expr[k] == ' ' || expr[k] == '\t')) ++k;
    return k;
  };
  size_t i = 0;
  while (i < expr.size()) {
    const char c = expr[i];
    if (c == '"') {
      const size_t close = expr.find('"', i + 1);
      i = close == std::string::npos ? expr.size() : close + 1;
      continue;
    }
    if (c == '[') {
      int depth = 0;
      for (; i < expr.size(); ++i) {
        if (expr[i] == '[') ++depth;
        else if (expr[i] == ']' && --depth == 0) break;
      }
      ++i;
      continue;
    }
    if (std::isdigit(static_cast<unsigned char>(c)) != 0 || (c == '.' && i + 1 < expr.size() &&
                                                             !isIdStart(expr[i + 1]))) {
      while (i < expr.size() && (std::isdigit(static_cast<unsigned char>(expr[i])) != 0 ||
                                 expr[i] == '.'))
        ++i;
      if (i < expr.size() && (expr[i] == 'e' || expr[i] == 'E')) {
        ++i;
        if (i < expr.size() && (expr[i] == '+' || expr[i] == '-')) ++i;
      }
      while (i < expr.size() && isIdChar(expr[i])) ++i;
      continue;
    }
    if (c == '.') {  // `.a`-`.z`
      ++i;
      while (i < expr.size() && isIdChar(expr[i])) ++i;
      continue;
    }
    if (!isIdStart(c)) {
      ++i;
      continue;
    }
    const size_t start = i;
    while (i < expr.size() && isIdChar(expr[i])) ++i;
    const std::string id = expr.substr(start, i - start);
    const size_t next = skipSpace(i);
    if (next < expr.size() && expr[next] == '(') {
      std::string lower = id;
      for (char& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
      static const char* const kNameArgFns[] = {"el",     "colsof", "rowsof", "colnumb", "scalar",
                                                "e",      "r",      "c",      "s"};
      bool nameArg = false;
      for (const char* f : kNameArgFns) nameArg = nameArg || lower == f;
      i = next + 1;
      if (nameArg) {
        i = skipSpace(i);
        while (i < expr.size() && isIdChar(expr[i])) ++i;
      }
      continue;
    }
    if (next < expr.size() && expr[next] == '[') continue;  // subscript head
    for (const auto& p : session.eval.matrices)
      if (p.first == id) return id;
  }
  return std::string();
}

// Splits `(1,2\3,4)`-style literals: '\' separates rows, ',' separates
// columns. nullopt on anything malformed (ragged rows, empty cells, an
// unbalanced group) or on a cell that belongs to the matrix grammar -- the
// caller (evalMxPrimary) then evaluates the group with that grammar, which
// answers it or refuses it with Stata's rc (Doctrine 1). The split is
// depth-aware (splitTopLevel), so a cell's own comma -- `round(1/3,.01)`,
// `el(M,2,3)` -- is never read as a column separator.
//
// Cell order, first match wins (oracle w2c_functions.log:160
// matrix_mixed_literal, w2f_residuals.log:1303 round_matrix_row):
//   1. `.` and `.a`-`.z`;
//   2. a plain signed decimal;
//   3. a constant arithmetic expression (no name, no function);
//   4. decline: a nested literal, a cell that mentions a matrix name as a bare
//      identifier (`Bcol'`, `2*Bcol`; firstBareMatrixName), a matrix subscript
//      NAME[...], a matrix-valued call I()/J()/vecdiag()/nullmat(), or an
//      e()/r() class MATRIX -- these keep their names and shapes through the
//      matrix grammar (or are refused there) instead of collapsing to a scalar;
//   5. any other cell is a scalar expression, evaluated by evalMatrixScalarExpr
//      (the macro/arith layer, the same as `scalar`/`local`/`display`).
std::optional<std::vector<std::vector<double>>> parseMatrixLiteral(const std::string& body,
                                                                    SessionState& session) {
  std::string s = trim(body);
  if (s.size() < 2 || s.front() != '(' || s.back() != ')') return std::nullopt;
  s = s.substr(1, s.size() - 2);
  std::vector<std::string> parts;
  std::vector<char> ops;
  if (!splitTopLevel(s, parts, ops)) return std::nullopt;  // unbalanced group

  auto namesMatrix = [&session](const std::string& n) {
    for (const auto& p : session.eval.matrices)
      if (p.first == n) return true;
    return false;
  };
  auto cellValue = [&](const std::string& t, double& out) -> bool {
    // BUG-540 (oracle arm lit_dot): a bare `.` cell is plain system missing --
    // same NaN evalJCall's `J(r,c,.)` fill already uses, so formatMatrixCell's
    // isnan() check and the existing NaN->"."->sentinel display round-trip
    // (matrixElementText, macros.cpp) render it identically to a J() cell.
    if (t == ".") {
      out = std::numeric_limits<double>::quiet_NaN();
      return true;
    }
    // BUG-540 (oracle arm lit_extmissing): `.a`-`.z` must stay DISTINCT from
    // plain `.` under `==` (E[1,2]==.a is true, E[1,2]==. is false in the
    // oracle) -- a NaN payload survives copies but isnan()-based rendering
    // collapses every NaN to the same "." text, which would lose the kind on
    // the way back through the expression layer. The eval layer's own `.a`
    // literal already resolves to the FINITE stataMissingWithKind() sentinel
    // (exprfns.h, BUG-249), so storing that same finite double here keeps
    // identity through matrixElementText's NaN-free pythonFloatRepr path.
    if (t.size() == 2 && t[0] == '.' && t[1] >= 'a' && t[1] <= 'z') {
      out = eval::stataMissingWithKind(t[1] - 'a' + 1);
      return true;
    }
    try {
      size_t pos = 0;
      const double v = std::stod(t, &pos);
      if (pos == t.size()) {
        out = v;
        return true;
      }
    } catch (const std::exception&) {
    }
    // ml-getmata T4 (oracle w2c_ml_getmata.do:244, `matrix rchange = (0.1*`i'
    // + 0.05)` inside family (8)'s loop, rc 0 in Stata): a cell may be a
    // CONSTANT arithmetic expression. Only digits, `.`, `e`, + - * / ^ and
    // parentheses qualify -- no name, no function -- so nothing that could
    // mean a matrix, a scalar or a variable is reinterpreted here. The value
    // comes from the existing macro/arith layer (eval::evalArith), not a new
    // evaluator. A `+` that is no number's sign (`1++2`) is declined first,
    // for the matrix grammar to refuse as StataBE does (mxPlusRefusal).
    static const std::regex reConstExpr(R"(^[0-9.eE+\-*/^() \t]+$)");
    if (!mxPlusRefusal(t).empty()) return false;
    if (t.find_first_of("+-*/^(") != std::string::npos && std::regex_match(t, reConstExpr)) {
      const std::optional<std::string> r = eval::evalArith(t, nullptr);
      if (!r.has_value()) return false;
      if (*r == ".") {
        out = std::numeric_limits<double>::quiet_NaN();
        return true;
      }
      try {
        size_t pos = 0;
        const double v = std::stod(*r, &pos);
        if (pos != r->size()) return false;
        out = v;
        return true;
      } catch (const std::exception&) {
        return false;
      }
    }
    // Decline (a): a nested literal `(1,2)` / `(S1 \ .)` -- the matrix grammar's.
    if (t.size() >= 2 && t.front() == '(' && t.back() == ')') {
      std::vector<std::string> inner;
      std::vector<char> innerOps;
      if (splitTopLevel(t.substr(1, t.size() - 2), inner, innerOps) && inner.size() > 1)
        return false;
    }
    // Decline (b): a cell that mentions a matrix NAME anywhere as a bare
    // identifier -- `Bcol`, `Bcol'`, `2*Bcol`, `-Bcol` (BUG-584): matrix
    // arithmetic, which the matrix grammar (evalMx) evaluates.
    if (!firstBareMatrixName(t, session).empty()) return false;
    // Decline (c): a subscript of a stored matrix, `M[1,1]` / `M[1..2,1]` /
    // `M["rb",1]`; the matrix grammar reads it (a range keeps the source names).
    // Depth-aware: `M[1,1]+M[2,2]` is a scalar expression, not a subscript.
    static const std::regex reSub(std::string("^(") + eval::kRxName + R"()\s*\[.*\]$)");
    std::smatch nm;
    if (std::regex_match(t, nm, reSub) && closingBracketIsLast(t, t.find('[')) &&
        namesMatrix(nm.str(1)))
      return false;
    // Decline (d): a matrix-valued call.
    static const std::regex reMatFn(R"(^(I|J|vecdiag|nullmat)\s*\()");
    if (std::regex_search(t, reMatFn)) return false;
    // Decline (e): an e()/r() class MATRIX. A class SCALAR (e(N), e(r2)) is
    // not declined: evalArith resolves it below.
    static const std::regex reClass(std::string(R"(^([er])\(\s*()") + eval::kRxName + R"()\s*\)$)");
    if (std::regex_match(t, nm, reClass)) {
      const char cls = nm.str(1)[0];
      const std::string which = nm.str(2);
      if (session.eval.findClassMatrix(cls, which) != nullptr ||
          (cls == 'e' && (which == "b" || which == "V")))
        return false;
    }
    std::string cellErr;
    return evalMatrixScalarExpr(t, session, out, cellErr);
  };

  std::vector<std::vector<double>> rows;
  std::vector<double> row;
  for (size_t i = 0; i < parts.size(); ++i) {
    const std::string t = trim(parts[i]);
    if (t.empty()) return std::nullopt;
    double v = 0.0;
    if (!cellValue(t, v)) return std::nullopt;
    row.push_back(v);
    if (i + 1 < parts.size() && ops[i] == '\\') {
      rows.push_back(std::move(row));
      row.clear();
    }
  }
  rows.push_back(std::move(row));
  const size_t ncols = rows[0].size();
  for (const auto& r : rows) {
    if (r.empty() || r.size() != ncols) return std::nullopt;  // ragged
  }
  return rows;
}

// Stata's default row/col stub ("r1".."rN" / "c1".."cN") — what an
// arithmetic result (a literal, or any `*` product) gets unless the caller
// overwrites it with an explicit `matrix rownames`/`colnames` (exactly what
// the P4 fixture does immediately after `matrix P4B = P4A'*P4A`).
void applyDefaultStripe(eval::EvalMatrix& mat) {
  const size_t nrows = mat.data.size();
  const size_t ncols = nrows > 0 ? mat.data[0].size() : 0;
  mat.rownames.clear();
  for (size_t i = 1; i <= nrows; ++i) mat.rownames.push_back("r" + std::to_string(i));
  mat.colnames.clear();
  for (size_t j = 1; j <= ncols; ++j) mat.colnames.push_back("c" + std::to_string(j));
}

eval::EvalMatrix transposeMatrix(const eval::EvalMatrix& in) {
  const size_t nrows = in.data.size();
  const size_t ncols = nrows > 0 ? in.data[0].size() : 0;
  eval::EvalMatrix out;
  out.data.assign(ncols, std::vector<double>(nrows, 0.0));
  for (size_t i = 0; i < nrows; ++i) {
    for (size_t j = 0; j < ncols; ++j) out.data[j][i] = in.data[i][j];
  }
  // Transpose swaps the stripe spaces along with the data.
  out.rownames = in.colnames;
  out.colnames = in.rownames;
  return out;
}

// AER scope 2 / B3(c): J() and nullmat() (J: 24 corpus uses, papers 015 and
// 049; nullmat: paper 015's `matrix b = nullmat(b), tmp` accumulator idiom),
// now two of the functions of the matrix expression grammar (evalMx below).

// Split on ',' / '\' at paren depth 0. Returns the pieces and their separators.
// BUG-542: `[' / ']' share the same depth counter as `(' / ')' -- a submatrix
// subscript like `M[1..2,1..3]` carries its OWN top-level comma (row spec vs
// col spec), which must not be mistaken for the join's own `,`/`\` separator
// when the subscript sits inside a larger expression (`(0,0,0) \ M[1..2,1..3]`).
// The two bracket kinds always nest correctly together in this grammar, so one
// counter is exact, not an approximation.
bool splitTopLevel(const std::string& e, std::vector<std::string>& parts,
                   std::vector<char>& ops) {
  int depth = 0;
  size_t start = 0;
  for (size_t i = 0; i < e.size(); ++i) {
    const char c = e[i];
    if (c == '(' || c == '[') ++depth;
    else if (c == ')' || c == ']') { if (--depth < 0) return false; }
    else if (depth == 0 && (c == ',' || c == '\\')) {
      parts.push_back(trim(e.substr(start, i - start)));
      ops.push_back(c);
      start = i + 1;
    }
  }
  if (depth != 0) return false;
  parts.push_back(trim(e.substr(start)));
  return true;
}

// True when the whole string is one parenthesised group.
bool wrappedInParens(const std::string& e) {
  if (e.size() < 2 || e.front() != '(' || e.back() != ')') return false;
  int depth = 0;
  for (size_t i = 0; i < e.size(); ++i) {
    if (e[i] == '(') ++depth;
    else if (e[i] == ')') { if (--depth == 0) return i + 1 == e.size(); }
  }
  return false;
}

// Converts eval::evalArith's text result into the double an eval::EvalMatrix
// cell stores -- the inverse of matrixElementText (macros.cpp): "." is plain
// missing, `.a`-`.z` is the extended-missing kind (BUG-249's
// stataMissingWithKind), anything else a plain decimal.
bool matrixCellDoubleFromArithResult(const std::string& s, double& out) {
  if (s == ".") {
    out = std::numeric_limits<double>::quiet_NaN();
    return true;
  }
  if (s.size() == 2 && s[0] == '.' && s[1] >= 'a' && s[1] <= 'z') {
    out = eval::stataMissingWithKind(s[1] - 'a' + 1);
    return true;
  }
  try {
    size_t pos = 0;
    out = std::stod(s, &pos);
    return pos == s.size();
  } catch (const std::exception&) {
    return false;
  }
}

bool mxMissing(double v) { return std::isnan(v) || std::fabs(v) >= eval::kStataMissingSentinel; }

size_t mxCols(const eval::EvalMatrix& m) { return m.data.empty() ? 0 : m.data[0].size(); }

size_t mxClosing(const std::string& e, size_t open);

std::string mxSqueeze(const std::string& s) {
  std::string out;
  for (const char c : s)
    if (c != ' ' && c != '\t') out += c;
  return out;
}

// StataBE has no unary plus but a number's sign: `+2`, `A * +2` and `2^+1` are
// rc 0, while a `+` before anything else is read into the name after it (2026-
// 11-03 oracle; its general evaluator does the same, `+sc invalid name`):
// `+A`, `A * +sc`, `sqrt(+sc)` and `+.a` are r(111) `+A not found` and so on,
// `+(1,2)` and `+sqrt(4)` r(133) `unknown function +()` / `+sqrt()`. A binary
// `+` with no operand after it -- another `+`, the end, a closing bracket -- is
// read into the operand before it: `A + +B` and `1++2` are `A+ not found` /
// `1+ not found`, `+A + +B` is `+A+ not found`. The refusal for the first such
// `+` in `e` as an `r(NNN): ` error, or "" when there is none.
std::string mxPlusRefusal(const std::string& e) {
  auto isIdStart = [](char c) { return cecon::ident::isIdentStartByte(c); };
  auto isIdChar = [](char c) { return cecon::ident::isIdentByte(c); };
  auto isDigit = [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; };
  for (size_t i = 0; i < e.size(); ++i) {
    if (e[i] == '"') {
      const size_t close = e.find('"', i + 1);
      if (close == std::string::npos) break;
      i = close;
      continue;
    }
    if (e[i] != '+') continue;
    size_t q = i;  // one past the last character before the `+` that is not a blank
    while (q > 0 && (e[q - 1] == ' ' || e[q - 1] == '\t')) --q;
    size_t n = i + 1;  // the first one after it
    while (n < e.size() && (e[n] == ' ' || e[n] == '\t')) ++n;
    const char before = q > 0 ? e[q - 1] : '\0';
    const char after = n < e.size() ? e[n] : '\0';
    if (before != '\0' && (isIdChar(before) || before == '.' || before == ')' || before == ']' ||
                           before == '\'' || before == '"')) {
      if (after != '\0' && after != '+' && after != ')' && after != ']' && after != ',') continue;
      size_t s = q - 1;  // the operand before: a name or number, or a group with its head
      if (before == ')' || before == ']') {
        int depth = 0;
        for (size_t k = s + 1; k-- > 0;) {
          if (e[k] == ')' || e[k] == ']') {
            ++depth;
          } else if ((e[k] == '(' || e[k] == '[') && --depth == 0) {
            s = k;
            break;
          }
        }
      }
      while (s > 0 && (isIdChar(e[s - 1]) || e[s - 1] == '.')) --s;
      return "r(111): " + mxSqueeze(e.substr(s, q - s)) + "+ not found";
    }
    if (isDigit(after) || (after == '.' && n + 1 < e.size() && isDigit(e[n + 1]))) continue;
    if (after == '(') return "r(133): unknown function +()";
    if (after == '.' || isIdStart(after)) {
      size_t j = n + 1;
      while (j < e.size() && isIdChar(e[j])) ++j;
      const std::string id = e.substr(n, j - n);
      while (j < e.size() && (e[j] == ' ' || e[j] == '\t')) ++j;
      if (after != '.' && j < e.size() && e[j] == '(')
        return "r(133): unknown function +" + id + "()";
      return "r(111): +" + id + " not found";
    }
    return "r(111): + not found";
  }
  return std::string();
}

// A subscript body's two specs, split at its top-level comma (outside quotes
// and brackets), or StataBE's refusal of a malformed one (2026-11-03 oracle):
// `A[1]`, `A[1,1,1]` and `A[,1]` are r(111) `A not found`; an empty column
// spec is r(198), or r(111) `, not found` after a range (`A[1..2,]`).
std::optional<std::pair<std::string, std::string>> mxSubscriptSpecs(const std::string& name,
                                                                   const std::string& body,
                                                                   std::string& err) {
  std::vector<std::string> parts;
  int depth = 0;
  size_t from = 0;
  for (size_t i = 0; i < body.size(); ++i) {
    const char c = body[i];
    if (c == '"') {
      const size_t close = body.find('"', i + 1);
      if (close == std::string::npos) break;
      i = close;
    } else if (c == '(' || c == '[') {
      ++depth;
    } else if (c == ')' || c == ']') {
      --depth;
    } else if (c == ',' && depth == 0) {
      parts.push_back(trim(body.substr(from, i - from)));
      from = i + 1;
    }
  }
  parts.push_back(trim(body.substr(from)));
  if (parts.size() != 2 || parts[0].empty()) {
    err = "r(111): " + name + " not found";
    return std::nullopt;
  }
  if (parts[1].empty()) {
    err = parts[0].find("..") != std::string::npos ? "r(111): , not found" : "r(198): invalid syntax";
    return std::nullopt;
  }
  return std::make_pair(parts[0], parts[1]);
}

// True when a subscript spec asks for a submatrix: a range (`1..2`, `2...`)
// or a row/column name (`"rb"`).
bool mxRangeSpec(const std::string& spec) {
  return spec.find("..") != std::string::npos || spec.find('"') != std::string::npos;
}

// One element of `m`, each index truncated toward zero; an index that is
// missing or outside `m` reads `.` (StataBE: `A[1.7,2]` is A[1,2], `A[4,1]`,
// `A[0,1]` and `el(A,.,1)` are `.`, rc 0).
double mxElementAt(const eval::EvalMatrix& m, double r, double c) {
  const double kNaN = std::numeric_limits<double>::quiet_NaN();
  if (mxMissing(r) || mxMissing(c)) return kNaN;
  r = std::trunc(r);
  c = std::trunc(c);
  if (r < 1 || c < 1 || r > static_cast<double>(m.data.size()) ||
      c > static_cast<double>(mxCols(m)))
    return kNaN;
  return m.data[static_cast<size_t>(r) - 1][static_cast<size_t>(c) - 1];
}

// A cell's value as evalArith text: `(.)`, `(.a)`, or the number in parentheses.
std::string mxValueText(double v) {
  if (std::isnan(v)) return "(.)";
  if (const std::optional<int> kind = eval::stataExtendedMissingKind(v))
    return *kind == 0 ? std::string("(.)") : std::string("(.") + static_cast<char>('a' + *kind - 1) + ")";
  return "(" + eval::pythonFloatRepr(v) + ")";
}

// The matrix sublanguage's reading of the names in a scalar expression, before
// the macro/arith layer sees it (StataBE 19.5, 2026-11-03 oracle):
//  * a bare name is a scalar or a matrix, never a dataset variable. A name that
//    is only a variable (or abbreviates one) is r(111) `NAME not found`
//    (`J(x1,1,0)`, `sqrt(x1)`, `(1,x1)`), and a name that is both reads the
//    scalar (`z*A` with scalar z = 7 and variable z = 5 is 7*A), where the
//    arith layer's own rule reads the variable. `x1[2]` subscripts the
//    variable and stays legal.
//  * `M[i,j]` and `el(M,i,j)` of a stored matrix read one element
//    (mxElementAt), each index a scalar expression read the same way. A range
//    or name subscript there is a matrix where a scalar is required, r(109)
//    (`sqrt(A[1..2,1])`); a malformed one is mxSubscriptSpecs' refusal; and
//    `NAME[...]` of a name that is no matrix (nor a variable, with one index)
//    is r(111) `NAME not found`.
// Quoted text, `_b[...]`/`_se[...]` and an `[eq]` prefix are copied as they are.
// `out` is the text for evalArith; false with an `r(NNN): ` error.
bool mxBindOperandText(const std::string& in, SessionState& session, std::string& out,
                       std::string& err) {
  auto isIdStart = [](char c) { return cecon::ident::isIdentStartByte(c); };
  auto isIdChar = [](char c) { return cecon::ident::isIdentByte(c); };
  const auto isVariable = [&session](const std::string& n) {
    return session.eval.varObs && session.eval.varObs(n, 1).has_value();
  };
  const auto readIndex = [&session, &err](const std::string& t, double& v) {
    return evalMatrixScalarExpr(t, session, v, err);
  };
  out.clear();
  size_t i = 0;
  while (i < in.size()) {
    const char c = in[i];
    if (c == '"') {
      const size_t close = in.find('"', i + 1);
      const size_t end = close == std::string::npos ? in.size() : close + 1;
      out.append(in, i, end - i);
      i = end;
      continue;
    }
    if (c == '[') {
      // A subscript's own bracket (after a variable or `e(b)`) is read on; an
      // `[eq]` prefix is copied whole.
      size_t q = out.size();
      while (q > 0 && (out[q - 1] == ' ' || out[q - 1] == '\t')) --q;
      if (q > 0 && (isIdChar(out[q - 1]) || out[q - 1] == ')')) {
        out += c;
        ++i;
      } else {
        const size_t close = std::min(mxClosing(in, i), in.size() - 1);
        out.append(in, i, close + 1 - i);
        i = close + 1;
      }
      continue;
    }
    if (!isIdStart(c) || (i > 0 && (isIdChar(in[i - 1]) || in[i - 1] == '.'))) {
      out += c;
      ++i;
      continue;
    }
    size_t j = i;
    while (j < in.size() && isIdChar(in[j])) ++j;
    const std::string id = in.substr(i, j - i);
    size_t k = j;
    while (k < in.size() && (in[k] == ' ' || in[k] == '\t')) ++k;
    if (k < in.size() && in[k] == '(') {
      std::string lower = id;
      for (char& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
      const size_t close = mxClosing(in, k);
      if (lower == "el" && close < in.size()) {
        std::vector<std::string> args;
        std::vector<char> ops;
        if (splitTopLevel(in.substr(k + 1, close - k - 1), args, ops) && args.size() == 3 &&
            std::all_of(ops.begin(), ops.end(), [](char o) { return o == ','; })) {
          if (const eval::EvalMatrix* m = session.eval.findMatrix(args[0])) {
            double r = 0.0, col = 0.0;
            if (!readIndex(args[1], r) || !readIndex(args[2], col)) return false;
            out += mxValueText(mxElementAt(*m, r, col));
            i = close + 1;
            continue;
          }
        }
      }
      // A call's arguments are read on; the first argument of the functions
      // whose argument is a name is that name, not an expression.
      static const char* const kNameArgFns[] = {"el",     "colsof", "rowsof", "colnumb", "rownumb",
                                                "scalar", "e",      "r",      "c",       "s"};
      bool nameArg = false;
      for (const char* f : kNameArgFns) nameArg = nameArg || lower == f;
      out.append(in, i, k + 1 - i);
      i = k + 1;
      if (nameArg) {
        while (i < in.size() && (in[i] == ' ' || in[i] == '\t')) out += in[i++];
        while (i < in.size() && isIdChar(in[i])) out += in[i++];
      }
      continue;
    }
    if (k < in.size() && in[k] == '[') {
      const size_t close = mxClosing(in, k);
      if (close >= in.size()) {  // unbalanced: the arith layer's refusal
        out.append(in, i, std::string::npos);
        break;
      }
      const std::string body = in.substr(k + 1, close - k - 1);
      if (id == "_b" || id == "_se" || id == "_coef") {
        out.append(in, i, close + 1 - i);
        i = close + 1;
        continue;
      }
      if (const eval::EvalMatrix* m = session.eval.findMatrix(id)) {
        std::optional<std::pair<std::string, std::string>> specs;
        if (!trim(body).empty()) {
          specs = mxSubscriptSpecs(id, body, err);
          if (!specs.has_value()) return false;
        }
        if (!specs.has_value() || mxRangeSpec(specs->first) || mxRangeSpec(specs->second)) {
          err = "r(109): type mismatch";
          return false;
        }
        double r = 0.0, col = 0.0;
        if (!readIndex(specs->first, r) || !readIndex(specs->second, col)) return false;
        out += mxValueText(mxElementAt(*m, r, col));
        i = close + 1;
        continue;
      }
      std::vector<std::string> idx;
      std::vector<char> idxOps;
      if (!isVariable(id) || !splitTopLevel(body, idx, idxOps) || idx.size() != 1) {
        err = "r(111): " + id + " not found";
        return false;
      }
      out.append(in, i, k - i);  // `x1[2]`: its index is read on
      i = k;
      continue;
    }
    if (session.eval.scalars.find(id) != nullptr) {
      out += isVariable(id) ? "scalar(" + id + ")" : id;
    } else if (session.eval.findMatrix(id) == nullptr && isVariable(id)) {
      err = "r(111): " + id + " not found";
      return false;
    } else {
      out += id;
    }
    i = j;
  }
  return true;
}

// The ONE scalar-expression evaluator of the matrix sublanguage: literal cells
// (parseMatrixLiteral), J()'s and I()'s arguments, a scalar operand of the
// grammar (evalMx), subscripts, and element-assignment subscripts and right-
// hand sides (runMatrixElementAssign). Its names are read the sublanguage's
// way first (mxPlusRefusal, mxBindOperandText; an element-assignment target's
// subscript excepted), and then it is the macro/arith layer (eval::evalArith,
// which substitutes colsof()/rowsof(), e()/_b[] refs and scalars itself), so a
// function answers in a matrix cell exactly as it answers in `scalar`/`local`/
// `display`; there is no second parser here.
//
// Every failure carries an `r(NNN): ` prefix (read back by eval::splitErrCode):
// a string result is r(109) (as main's element path), a failure the shared
// classifier claims keeps its rc (an unknown function is r(133)), a bare
// unknown identifier -- the shape the classifier leaves unclaimed -- is r(111)
// "NAME not found", and anything else unclaimed is r(198).
bool evalMatrixScalarExpr(const std::string& exprIn, SessionState& session, double& outVal,
                          std::string& err, bool lhsSubscript) {
  // A plain run of digits is its own value, read before the arith layer, which
  // rejects a leading-zero integer literal (`01`, as `local j : display %02.0f`
  // writes it, or `I(03)`): Stata reads 01 as 1 (BUG-587 subscripts, BUG-586
  // I()). The arith layer's own leading-zero rule is not changed (FU-201).
  {
    const std::string digits = trim(exprIn);
    if (!digits.empty() && digits.find_first_not_of("0123456789") == std::string::npos) {
      try {
        outVal = std::stod(digits);
        return true;
      } catch (const std::exception&) {
        // out of double range: the arith layer answers below
      }
    }
  }
  std::string expr = exprIn;
  if (!lhsSubscript) {
    err = mxPlusRefusal(exprIn);
    if (!err.empty()) return false;
    if (!mxBindOperandText(exprIn, session, expr, err)) return false;
  }
  std::string raised;
  bool isString = false;
  const std::optional<std::string> result =
      eval::evalArith(expr, &session.eval, &raised, &isString);
  if (result.has_value() && isString) {
    err = "r(109): type mismatch";
    return false;
  }
  if (!result.has_value()) {
    // A NameError on a STORED MATRIX name is a matrix where a scalar is
    // required (`J(2,2,A)`, `sqrt(A)`): StataBE's r(109) "type mismatch", never
    // r(111) "M not found" (BUG-584).
    static const std::string kNameErr = "NameError: name '";
    std::string nameErrName;
    if (raised.rfind(kNameErr, 0) == 0) {
      const size_t end = raised.find('\'', kNameErr.size());
      if (end != std::string::npos)
        nameErrName = raised.substr(kNameErr.size(), end - kNameErr.size());
    }
    // A name only called (`A(1)`) is an unknown function, r(133), which the
    // classifier below answers.
    if (!nameErrName.empty() && !firstBareMatrixName(expr, session).empty()) {
      for (const auto& p : session.eval.matrices) {
        if (p.first == nameErrName) {
          err = "r(109): type mismatch";
          return false;
        }
      }
    }
    eval::MacroFnError mfe;
    if (!raised.empty() && eval::classifyFailedExpr(expr, raised, &mfe) && mfe.failed()) {
      err = "r(" + mfe.rc + "): " + mfe.message;
      return false;
    }
    if (!nameErrName.empty()) {
      err = "r(111): " + nameErrName + " not found";
      return false;
    }
    err = "r(198): invalid syntax";
    return false;
  }
  if (!matrixCellDoubleFromArithResult(*result, outVal)) {
    err = "r(109): type mismatch";
    return false;
  }
  return true;
}

// The error block for an evalMatrixScalarExpr or evalMx failure: the
// `r(NNN): ` prefix becomes the exact rc through engineErrorBlockRc (the
// errors.h builder, BUG-187), and the message is StataBE's bare sentence
// (`conformability error`, never `matrix X: conformability error`). A message
// without a prefix is r(198); this never yields rc 1.
ojson matrixExprErrorBlock(const std::string& err) {
  std::string msg = err;
  const std::string code = eval::splitErrCode(&msg);  // "r(NNN)"; r(111) when absent
  int rc = 198;
  if (err.rfind("r(", 0) == 0 && code.size() > 3) {
    try {
      rc = std::stoi(code.substr(2, code.size() - 3));
    } catch (const std::exception&) {
      rc = 198;
    }
  }
  return engineErrorBlockRc(msg, rc);
}

// The one resolver of a class-matrix reference `e(NAME)`/`r(NAME)` on the
// matrix verbs: runMatrixDef's `matrix X = e(NAME)` form, vecdiag()'s argument
// and an e()/r() term at a join position. An e() matrix is
// EvalSession::resolveEMatrix's (eb-order T-P1): a matrix POSTED for the live
// e() wins (`estat event, post`'s Pre_avg/Post_avg/Tm#/Tp# e(b)/e(V), pca's
// Ev/L/C/Psi, margins/glm postings), and otherwise e(b)/e(V) are the live fit's
// canonical ones -- `_cons` last, never `(intercept)`, and the engine's FULL
// covariance (w2g_ebmat.log:245 `x1 x2 _cons`, :365 V[1,2]). The diagonal-of-
// se^2 synthesis that stood here (FU-98) is gone: it answered every
// off-diagonal with a silent 0. An r() matrix is the posted store's.
// nullopt with `err` (and `*rcOut`: 301 with no e() at all, 111 for an absent
// name, the resolver's named refusal of an e(V) the engine did not send).
std::optional<eval::EvalMatrix> resolveClassMatrixRef(char cls, const std::string& which,
                                                       SessionState& session, std::string& err,
                                                       int* rcOut = nullptr) {
  if (cls != 'e') {
    if (const eval::EvalMatrix* posted = session.eval.findClassMatrix(cls, which)) return *posted;
    err = std::string("matrix ") + cls + "(" + which + ") not found";
    if (rcOut != nullptr) *rcOut = 111;
    return std::nullopt;
  }
  int rc = 111;
  std::optional<eval::EvalMatrix> m = session.eval.resolveEMatrix(which, &rc, &err);
  if (!m.has_value() && rcOut != nullptr) *rcOut = rc;
  return m;
}

std::optional<eval::EvalMatrix> evalMatrixRhs(const std::string& rhs,
                                              SessionState& session, std::string& err);

// The matrix expression grammar of `matrix NAME = exp` and its friends ([P]
// matrix define, "Matrix operators"; BUG-1398). StataBE 19.5 (2026-10-29
// oracle) types every operand a SCALAR or a MATRIX and checks the types at
// each operator:
//   precedence, lowest first: `\` row join, `,` column join, `+`, binary `-`,
//   `*`, `/`, `#`, unary `-`, postfix `'`, then parentheses. Each binary level
//   is left-associative, so `a + b - c` is a + (b - c) and `A*3/0.3` is
//   A*(3/0.3) -- the scalar evaluator's own order.
//   A SCALAR is a number, `.`, or a scalar expression with no matrix in it
//   (`sqrt(4)`, `sc`, `y[2]`, `(2)`, and `A[1,1]`: two scalar subscripts read
//   one element). A MATRIX is a stored NAME, a group with its own `,`/`\` (a
//   literal), J()/I()/nullmat()/inv()/invsym()/syminv()/vecdiag(), a range or
//   name subscript (`A[1..2,1]`, `A["rb",1]`), or an e()/r() matrix.
//     `\` `,`  a scalar joins as a 1x1;
//     `+` `-`  two scalars, or two matrices of one shape (else r(503)); a
//              scalar takes part as a 1x1, so `S + 1` works but `A + 1` is
//              r(503);
//     `*`      a scalar scales the other operand; two matrices multiply when
//              they conform (a 1x1 O times a 1xn B too: O's row name), and
//              otherwise a 1x1 one scales the other (`B*O` keeps B's names)
//              or it is r(503);
//     `/`      the divisor must be a scalar (`A/S` is r(109) even for a 1x1
//              S), and 0 or missing is r(504) `matrix has missing values`;
//     `#`      two matrices (`2#A` is r(109));
//   and a matrix inside a scalar-only construct (`A^2`, `sqrt(A)`) is r(109).
// Names: `+`/`-` take the right operand's, `*` the left's rows and the right's
// columns, a scaled, negated or divided matrix keeps its own, `#` pairs them
// as eq:name. An inline value (a literal, a scalar) brings none: its rows and
// columns are named by position once the whole expression is evaluated (`A \
// (5,6)` names the new row r3, `(1,2)'` is r1 r2 x c1), while a stored matrix
// and J()/I() bring theirs (`J(1,2,0) \ J(1,2,1)` is r1 r1).
// Missing cells (StataBE 19.5, 2026-11-03 oracle, with P = (.a,1,.a,.,2) and
// Q = (1,.b,.b,.c,3) stored): `a - b` is a + (-b), and a sum accumulates into
// one operand -- the first that is a computed matrix (a literal, J(), a
// transpose, a negation, any result: anything but a stored matrix itself),
// else a scalar. A cell of the sum is `.` when the other operand's cell is
// missing and keeps the accumulating operand's missing value when only that one
// is (`(.a,1) + (1,.b)` is .a ., `P + (1,.b)` cells . .b, `1 - S` is .a for a
// stored S = (.a)); two stored matrices give `.` for any missing (`P + Q` is
// . . . . 5). Negating or scaling a matrix keeps a missing cell's kind, but a
// missing scale makes every cell `.` (`P*.b`); a negated missing scalar, any
// scalar arithmetic on a missing, and a product term with a missing make `.`.
// nullmat(ARG) (StataBE 19.5, 2026-11-03 oracle) looks ARG up among the
// user's matrices only; anything else -- an undefined name, a scalar, `e(b)`
// even after a fit, `(1,2)` -- is ABSENT. An absent operand may only be an
// operand of `,` or `\`, possibly in parentheses: an absent left operand
// yields the right one, which must not be absent too (`nullmat(Zb),
// nullmat(Zc)` is `matrix Zc not found`), an absent right one yields the
// left. Any other use -- the whole expression, `'`, unary minus, another
// operator, a function's argument -- is r(111) `matrix ARG not found`, ARG
// without its blanks (`nullmat( A , B )` is `matrix A,B not found`).
struct MxValue {
  bool scalar = false;
  bool stored = false;  // a stored matrix itself (`P`, `e(b)`), not a computed one
  bool absent = false;  // an absent nullmat(): absentArg is its argument
  std::string absentArg;
  double s = 0.0;
  eval::EvalMatrix m;  // every row/column has a name slot; "" is named by position
};

// joinOperand: the text is an operand of `,`/`\`, where an absent nullmat()
// may stand (see MxValue).
std::optional<MxValue> evalMx(const std::string& text, SessionState& session, std::string& err,
                              bool joinOperand = false);

MxValue mxScalar(double s) {
  MxValue v;
  v.scalar = true;
  v.s = s;
  return v;
}

MxValue mxMatrix(eval::EvalMatrix m) {
  m.rownames.resize(m.data.size());
  m.colnames.resize(mxCols(m));
  MxValue v;
  v.m = std::move(m);
  return v;
}

MxValue mxStored(const eval::EvalMatrix& m) {
  MxValue v = mxMatrix(m);
  v.stored = true;
  return v;
}

// Unary minus: a matrix keeps its missing cells' kinds (and is a computed one
// now); a missing scalar is `.` (StataBE: `-.a` is `.`).
MxValue mxNegate(MxValue v) {
  if (v.scalar) {
    v.s = mxMissing(v.s) ? std::numeric_limits<double>::quiet_NaN() : -v.s;
    return v;
  }
  for (auto& row : v.m.data)
    for (double& x : row)
      if (!mxMissing(x)) x = -x;
  v.stored = false;
  return v;
}

// A scalar taking part in a join or a `+`/`-` with a matrix: an unnamed 1x1.
eval::EvalMatrix mxAsMatrix(const MxValue& v) {
  if (!v.scalar) return v.m;
  eval::EvalMatrix m;
  m.data = {{v.s}};
  m.rownames = {""};
  m.colnames = {""};
  return m;
}

// The largest row/column dimension any Stata edition allows: c(max_matdim) is
// 65,534 for Stata/MP, 11,000 for SE and 800 for BE ([P] creturn); beyond it
// Stata answers r(915) "unable to allocate matrix" ([P] error). I() and J()
// check against it BEFORE casting or allocating, so a huge argument is a
// refusal and never an out-of-range cast or a whole-run allocation failure.
constexpr double kMaxMatDim = 65534;

// A scalar operand of the sublanguage, J()'s and I()'s arguments: a matrix
// there is r(109) `type mismatch` (`J(2,2,A)`), as is a string (`J(2,2,"3")`).
bool mxScalarOperand(const std::string& text, SessionState& session, double& out,
                     std::string& err) {
  std::optional<MxValue> v = evalMx(text, session, err);
  if (!v.has_value()) return false;
  if (!v->scalar) {
    err = "r(109): type mismatch";
    return false;
  }
  out = v->s;
  return true;
}

// A dimension of J()/I(): a scalar expression rounded to the nearest integer
// ([P] matrix define; StataBE: `J(1.4,2.5,1)` is 1x3, `I(2.5)` is 3x3). One
// that is missing or rounds below 1 is r(198) (`J(0,1,0)`, `J(-0.4,1,1)`,
// `J(.a,2,1)`); one beyond the largest edition's limit r(915).
bool mxDimension(const std::string& text, SessionState& session, size_t& out, std::string& err) {
  double v = 0.0;
  if (!mxScalarOperand(text, session, v, err)) return false;
  const double r = std::floor(v + 0.5);
  if (mxMissing(v) || r < 1) {
    err = "r(198): invalid syntax";
    return false;
  }
  if (r > kMaxMatDim) {
    err = "r(915): unable to allocate matrix";
    return false;
  }
  out = static_cast<size_t>(r);
  return true;
}

// `J(r, c, v)`: r x c filled with v, each argument a scalar expression
// (BUG-1400: `J(sc,1,0)`, `J(`k',`k'-1,.)`, `J(rowsof(A),colsof(A),el(A,2,2))`
// are rc 0 in StataBE). Anything but three comma-separated arguments is
// r(198), as is a zero dimension, which is why an empty result is an error
// here rather than a 0x0 matrix.
std::optional<eval::EvalMatrix> evalJCall(const std::string& inner, SessionState& session,
                                          std::string& err) {
  std::vector<std::string> a;
  std::vector<char> ops;
  if (!splitTopLevel(inner, a, ops) || a.size() != 3 ||
      std::any_of(ops.begin(), ops.end(), [](char o) { return o != ','; }) ||
      std::any_of(a.begin(), a.end(), [](const std::string& s) { return s.empty(); })) {
    err = "r(198): invalid syntax";
    return std::nullopt;
  }
  size_t dim[2] = {0, 0};
  for (int k = 0; k < 2; ++k)
    if (!mxDimension(a[static_cast<size_t>(k)], session, dim[k], err)) return std::nullopt;
  double fill = 0.0;
  if (!mxScalarOperand(a[2], session, fill, err)) return std::nullopt;
  eval::EvalMatrix m;
  m.data.assign(dim[0], std::vector<double>(dim[1], fill));
  applyDefaultStripe(m);
  return m;
}

// Horizontal (`,`) and vertical (`\`) concatenation, with Stata's conformability
// rules. A 0x0 operand -- which is what nullmat() yields for a matrix that does
// not exist yet -- is the identity, and that is the whole point of the idiom:
// `matrix b = nullmat(b), tmp` builds a matrix up one column-block at a time
// without a special case for the first pass.
std::optional<eval::EvalMatrix> concatMatrices(const eval::EvalMatrix& a,
                                               const eval::EvalMatrix& b, char op,
                                               std::string& err) {
  if (a.data.empty()) return b;
  if (b.data.empty()) return a;

  eval::EvalMatrix out;
  if (op == ',') {
    if (a.data.size() != b.data.size()) {
      err = "matrices are not conformable for horizontal concatenation";
      return std::nullopt;
    }
    out.data = a.data;
    for (size_t i = 0; i < out.data.size(); ++i)
      out.data[i].insert(out.data[i].end(), b.data[i].begin(), b.data[i].end());
    out.rownames = a.rownames;
    out.colnames = a.colnames;
    out.colnames.insert(out.colnames.end(), b.colnames.begin(), b.colnames.end());
  } else {
    if (a.data[0].size() != b.data[0].size()) {
      err = "matrices are not conformable for vertical concatenation";
      return std::nullopt;
    }
    out.data = a.data;
    out.data.insert(out.data.end(), b.data.begin(), b.data.end());
    out.colnames = a.colnames;
    out.rownames = a.rownames;
    out.rownames.insert(out.rownames.end(), b.rownames.begin(), b.rownames.end());
  }
  if (out.rownames.size() != out.data.size() ||
      out.colnames.size() != (out.data.empty() ? 0 : out.data[0].size()))
    applyDefaultStripe(out);
  return out;
}

// BUG-542 (submatrix extraction), one axis of `M[rowspec,colspec]` when either
// spec asks for a submatrix (mxRangeSpec), as StataBE 19.5 reads it (2026-11-03
// oracle): a quoted name `"rb"` (r(111) `rb not found` when the axis has no
// such name), a range `lo..hi` or an open range `lo...` whose bounds are names
// or scalar expressions (`sc..3`, `"na"..2`), or one scalar expression. A
// number is rounded to the nearest index here (`1.5..2` starts at row 2,
// `M[1.4,1..2]` is row 1); one outside the axis, or a range that runs
// backwards, is r(503) `conformability error`; a missing single index is the
// whole axis (`M[.,1..2]`); `1..` and `...2` are r(198). The 1-based inclusive
// [lo, hi], or nullopt with an `r(NNN): ` error.
std::optional<std::pair<size_t, size_t>> mxAxisRange(const std::string& spec,
                                                     const std::vector<std::string>& names,
                                                     size_t dim, SessionState& session,
                                                     std::string& err) {
  size_t dots = std::string::npos;  // the range's `..`, outside quotes and brackets
  {
    int depth = 0;
    for (size_t i = 0; i + 1 < spec.size(); ++i) {
      const char c = spec[i];
      if (c == '"') {
        const size_t close = spec.find('"', i + 1);
        if (close == std::string::npos) break;
        i = close;
      } else if (c == '(' || c == '[') {
        ++depth;
      } else if (c == ')' || c == ']') {
        --depth;
      } else if (depth == 0 && c == '.' && spec[i + 1] == '.') {
        dots = i;
        break;
      }
    }
  }
  std::string loText = trim(spec), hiText;
  bool open = false;
  if (dots != std::string::npos) {
    loText = trim(spec.substr(0, dots));
    hiText = spec.substr(dots + 2);
    if (!hiText.empty() && hiText[0] == '.') {
      open = true;
      hiText = hiText.substr(1);
    }
    hiText = trim(hiText);
    if (loText.empty() || open != hiText.empty()) {
      err = "r(198): invalid syntax";
      return std::nullopt;
    }
  }
  // 1: an index in `out`; 0: the whole axis (a missing single index); -1: refused.
  const auto bound = [&](const std::string& t, bool single, size_t& out) -> int {
    std::string quoted = t;
    if (quoted.size() >= 4 && quoted.compare(0, 2, "`\"") == 0 &&
        quoted.compare(quoted.size() - 2, 2, "\"'") == 0)
      quoted = quoted.substr(1, quoted.size() - 2);
    if (quoted.size() >= 2 && quoted.front() == '"' && quoted.back() == '"') {
      const std::string nm = quoted.substr(1, quoted.size() - 2);
      for (size_t k = 0; k < names.size(); ++k) {
        if (names[k] == nm) {
          out = k + 1;
          return 1;
        }
      }
      err = "r(111): " + nm + " not found";
      return -1;
    }
    double v = 0.0;
    if (!evalMatrixScalarExpr(t, session, v, err)) return -1;
    if (mxMissing(v) && single) return 0;
    const double r = std::floor(v + 0.5);
    if (mxMissing(v) || r < 1 || r > static_cast<double>(dim)) {
      err = "r(503): conformability error";
      return -1;
    }
    out = static_cast<size_t>(r);
    return 1;
  };
  size_t lo = 0, hi = 0;
  const int got = bound(loText, dots == std::string::npos, lo);
  if (got < 0) return std::nullopt;
  if (got == 0) return std::make_pair(size_t{1}, dim);
  if (dots == std::string::npos) return std::make_pair(lo, lo);
  if (open) {
    hi = dim;
  } else if (bound(hiText, false, hi) < 0) {
    return std::nullopt;
  }
  if (lo > hi) {
    err = "r(503): conformability error";
    return std::nullopt;
  }
  return std::make_pair(lo, hi);
}

// `M[rowspec,colspec]` with a range or name spec: the rows and columns it
// selects, with their names.
std::optional<eval::EvalMatrix> mxSubmatrix(const eval::EvalMatrix& src, const std::string& rowSpec,
                                            const std::string& colSpec, SessionState& session,
                                            std::string& err) {
  const auto rows = mxAxisRange(rowSpec, src.rownames, src.data.size(), session, err);
  if (!rows.has_value()) return std::nullopt;
  const auto cols = mxAxisRange(colSpec, src.colnames, mxCols(src), session, err);
  if (!cols.has_value()) return std::nullopt;
  eval::EvalMatrix out;
  for (size_t i = rows->first; i <= rows->second; ++i) {
    const auto& row = src.data[i - 1];
    out.data.emplace_back(row.begin() + static_cast<std::ptrdiff_t>(cols->first - 1),
                          row.begin() + static_cast<std::ptrdiff_t>(cols->second));
    out.rownames.push_back(i - 1 < src.rownames.size() ? src.rownames[i - 1] : "r" + std::to_string(i));
  }
  for (size_t j = cols->first; j <= cols->second; ++j)
    out.colnames.push_back(j - 1 < src.colnames.size() ? src.colnames[j - 1] : "c" + std::to_string(j));
  return out;
}

// The top-level positions in `e` of the operator `op` -- outside "strings" and
// ()/[] groups. `+` and `-` count only as binary operators, after an operand
// (a name, a number, `.`/`.a`, a closing bracket, a transpose `'`, a string)
// or right after a `+`, which StataBE reads into the operand before it (`A +
// +B` is `A+` and B, `+-2` is `+` and 2; mxPlusRefusal): never a sign (`A*-1`,
// `A - -B`) or an exponent (`1e-5`). `e` is balanced (evalMx checks first).
std::vector<size_t> mxTopLevelOps(const std::string& e, char op) {
  auto isIdStart = [](char c) { return cecon::ident::isIdentStartByte(c); };
  auto isIdChar = [](char c) { return cecon::ident::isIdentByte(c); };
  auto isDigit = [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; };
  std::vector<size_t> at;
  bool afterOperand = false;
  bool afterPlus = false;
  size_t i = 0;
  while (i < e.size()) {
    const char c = e[i];
    if (c == ' ' || c == '\t') {
      ++i;
      continue;
    }
    const bool plusBefore = afterPlus;
    afterPlus = false;
    if (c == '"') {
      const size_t close = e.find('"', i + 1);
      i = close == std::string::npos ? e.size() : close + 1;
      afterOperand = true;
    } else if (c == '(' || c == '[') {
      int depth = 0;
      for (; i < e.size(); ++i) {
        if (e[i] == '(' || e[i] == '[') ++depth;
        else if ((e[i] == ')' || e[i] == ']') && --depth == 0) break;
      }
      ++i;
      afterOperand = true;
    } else if (isIdStart(c)) {
      while (i < e.size() && isIdChar(e[i])) ++i;
      afterOperand = true;
    } else if (isDigit(c) || (c == '.' && i + 1 < e.size() && isDigit(e[i + 1]))) {
      while (i < e.size() && (isDigit(e[i]) || e[i] == '.')) ++i;
      if (i < e.size() && (e[i] == 'e' || e[i] == 'E')) {
        size_t k = i + 1;
        if (k < e.size() && (e[k] == '+' || e[k] == '-')) ++k;
        if (k < e.size() && isDigit(e[k])) {
          i = k;
          while (i < e.size() && isDigit(e[i])) ++i;
        }
      }
      afterOperand = true;
    } else if (c == '.') {  // `.`, `.a`-`.z`
      ++i;
      while (i < e.size() && isIdChar(e[i])) ++i;
      afterOperand = true;
    } else if (c == '\'') {  // a postfix transpose
      ++i;
      afterOperand = true;
    } else {
      if (c == op && (afterOperand || plusBefore || (op != '+' && op != '-'))) at.push_back(i);
      afterOperand = false;
      afterPlus = c == '+';
      ++i;
    }
  }
  return at;
}

// The index of the bracket closing the one at `open`, both kinds on one depth.
size_t mxClosing(const std::string& e, size_t open) {
  int depth = 0;
  for (size_t i = open; i < e.size(); ++i) {
    if (e[i] == '(' || e[i] == '[') ++depth;
    else if ((e[i] == ')' || e[i] == ']') && --depth == 0) return i;
  }
  return e.size();
}

// True when the `[` at `open` starts a bracket group: StataBE reads `[exp]` as
// `(exp)` (w2q_matrix_edges A09, 087's `matrix A[1,3] = [`delta_M']`: .25;
// 2026-11-03 oracle: `[1+2]*3` is 9, `matrix B = [0.5]` and `([1], 2)` rc 0).
// It is not one when it subscripts what stands before it (`A[1,2]`,
// `e(b)[1,1]`) or when a name follows it, the `[eq]name` / `[#1]_b[x]`
// coefficient reference the scalar evaluator owns.
bool mxBracketGroupAt(const std::string& e, size_t open) {
  size_t before = open;
  while (before > 0 && (e[before - 1] == ' ' || e[before - 1] == '\t')) --before;
  if (before > 0) {
    const char p = e[before - 1];
    if (p == ')' || p == ']' || p == '\'' || cecon::ident::isIdentByte(p)) return false;
  }
  const size_t close = mxClosing(e, open);
  return close + 1 >= e.size() || !cecon::ident::isIdentByte(e[close + 1]);
}

// True when `e` needs the matrix grammar: it holds a matrix -- a stored NAME
// that is not a call or subscript head, a group with its own `,`/`\`, a top-
// level `,`/`\`, one of the matrix functions, a range or name subscript of a
// stored matrix or its empty one (`A[]`), an e()/r() matrix -- a postfix
// transpose, or the matrix-only
// operator `#` (`sc # sc` is r(109), not a scalar syntax error). Anything else is
// one scalar expression, evaluated whole by evalMatrixScalarExpr: a call's
// arguments are scalar context (`el(M,2,3)`, `scalar(Bcol)`), and so is
// `M[i,j]`.
bool mxNeedsGrammar(const std::string& e, SessionState& session) {
  auto isIdStart = [](char c) { return cecon::ident::isIdentStartByte(c); };
  auto isIdChar = [](char c) { return cecon::ident::isIdentByte(c); };
  auto isDigit = [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; };
  static const char* const kMatrixFns[] = {"J",   "I",      "nullmat", "inv",
                                           "invsym", "syminv", "vecdiag"};
  size_t i = 0;
  while (i < e.size()) {
    const char c = e[i];
    if (c == '"') {
      const size_t close = e.find('"', i + 1);
      i = close == std::string::npos ? e.size() : close + 1;
    } else if (c == ',' || c == '\\' || c == '\'' || c == '#') {
      return true;
    } else if (c == '(' || c == '[') {
      const size_t close = mxClosing(e, i);
      if (c == '(' && mxNeedsGrammar(e.substr(i + 1, close - i - 1), session)) return true;
      // A `[` that is no subscript (nothing it could index stands before it)
      // and no `[eq]name` coefficient reference (no name follows it) is a
      // bracket group, which the scalar evaluator does not read.
      if (c == '[' && mxBracketGroupAt(e, i)) return true;
      i = close + 1;
    } else if (isDigit(c) || (c == '.' && i + 1 < e.size() && isDigit(e[i + 1]))) {
      while (i < e.size() && (isDigit(e[i]) || e[i] == '.')) ++i;
      if (i < e.size() && (e[i] == 'e' || e[i] == 'E')) {
        ++i;
        if (i < e.size() && (e[i] == '+' || e[i] == '-')) ++i;
      }
      while (i < e.size() && isIdChar(e[i])) ++i;
    } else if (c == '.') {
      ++i;
      while (i < e.size() && isIdChar(e[i])) ++i;
    } else if (isIdStart(c)) {
      const size_t start = i;
      while (i < e.size() && isIdChar(e[i])) ++i;
      const std::string id = e.substr(start, i - start);
      size_t next = i;
      while (next < e.size() && (e[next] == ' ' || e[next] == '\t')) ++next;
      if (next < e.size() && e[next] == '(') {
        const size_t close = mxClosing(e, next);
        for (const char* f : kMatrixFns)
          if (id == f) return true;
        if (next > i && session.eval.findMatrix(id) != nullptr) return true;  // `A (1,2)`
        if (id == "e" || id == "r") {
          const std::string which = trim(e.substr(next + 1, close - next - 1));
          if (session.eval.findClassMatrix(id[0], which) != nullptr ||
              (id == "e" && (which == "b" || which == "V")))
            return true;
        }
        i = close + 1;
      } else if (next < e.size() && e[next] == '[') {
        const size_t close = mxClosing(e, next);
        const std::string body = e.substr(next + 1, close - next - 1);
        if (session.eval.findMatrix(id) != nullptr &&
            (body.find("..") != std::string::npos || body.find('"') != std::string::npos ||
             trim(body).empty()))
          return true;
        i = close + 1;
      } else if (session.eval.findMatrix(id) != nullptr) {
        return true;
      }
    } else {
      ++i;
    }
  }
  return false;
}

std::optional<MxValue> mxApply(char op, MxValue a, MxValue b, std::string& err) {
  const double kNaN = std::numeric_limits<double>::quiet_NaN();
  if (op == '\\' || op == ',') {
    if (a.absent) {
      if (b.absent) {
        err = "r(111): matrix " + b.absentArg + " not found";
        return std::nullopt;
      }
      return b;
    }
    if (b.absent) return a;
    std::string joinErr;
    std::optional<eval::EvalMatrix> joined =
        concatMatrices(mxAsMatrix(a), mxAsMatrix(b), op, joinErr);
    if (!joined.has_value()) {
      err = "r(503): conformability error";
      return std::nullopt;
    }
    return mxMatrix(std::move(*joined));
  }
  // A result that overflows to +/-inf (or is inf * 0) is `.`: StataBE lists
  // `A*1e300*1e300` as `.` and svmat stores it missing (2026-11-03 oracle).
  const auto real = [kNaN](double v) { return std::isfinite(v) ? v : kNaN; };
  if (a.scalar && b.scalar && op != '#') {
    if (op == '/')
      return mxScalar(mxMissing(a.s) || mxMissing(b.s) || b.s == 0.0 ? kNaN : real(a.s / b.s));
    if (mxMissing(a.s) || mxMissing(b.s)) return mxScalar(kNaN);
    return mxScalar(real(op == '+' ? a.s + b.s : op == '-' ? a.s - b.s : a.s * b.s));
  }
  // Scale `m` by `s`: a missing cell keeps its kind, a missing `s` makes
  // every cell `.`.
  const auto scaled = [kNaN, &real](eval::EvalMatrix m, double s) {
    for (auto& row : m.data)
      for (double& v : row) {
        if (mxMissing(s)) v = kNaN;
        else if (!mxMissing(v)) v = real(v * s);
      }
    return mxMatrix(std::move(m));
  };
  if (op == '+' || op == '-') {
    if (op == '-') b = mxNegate(std::move(b));
    const eval::EvalMatrix x = mxAsMatrix(a);
    eval::EvalMatrix y = mxAsMatrix(b);  // the right operand's names
    if (x.data.size() != y.data.size() || mxCols(x) != mxCols(y)) {
      err = "r(503): conformability error";
      return std::nullopt;
    }
    // The operand the sum accumulates into (0 left, 1 right, -1 neither: two
    // stored matrices), as the comment above MxValue says.
    const int acc = !a.scalar && !a.stored ? 0 : !b.scalar && !b.stored ? 1 : a.scalar ? 0 : b.scalar ? 1 : -1;
    for (size_t i = 0; i < y.data.size(); ++i)
      for (size_t j = 0; j < y.data[i].size(); ++j) {
        const double l = x.data[i][j], r = y.data[i][j];
        const double kept = acc == 0 ? l : r, other = acc == 0 ? r : l;
        if (acc < 0) y.data[i][j] = mxMissing(l) || mxMissing(r) ? kNaN : real(l + r);
        else y.data[i][j] = mxMissing(other) ? kNaN : mxMissing(kept) ? kept : real(l + r);
      }
    return mxMatrix(std::move(y));
  }
  if (op == '*') {
    if (a.scalar) return scaled(std::move(b.m), a.s);
    if (b.scalar) return scaled(std::move(a.m), b.s);
    const size_t n = mxCols(a.m);
    if (n != b.m.data.size()) {
      if (a.m.data.size() == 1 && n == 1) return scaled(std::move(b.m), a.m.data[0][0]);
      if (b.m.data.size() == 1 && mxCols(b.m) == 1)
        return scaled(std::move(a.m), b.m.data[0][0]);
      err = "r(503): conformability error";
      return std::nullopt;
    }
    eval::EvalMatrix out;
    out.data.assign(a.m.data.size(), std::vector<double>(mxCols(b.m), 0.0));
    for (size_t i = 0; i < out.data.size(); ++i)
      for (size_t j = 0; j < out.data[i].size(); ++j) {
        double sum = 0.0;
        for (size_t k = 0; k < n; ++k) {
          const double l = a.m.data[i][k], r = b.m.data[k][j];
          if (mxMissing(l) || mxMissing(r)) {
            sum = kNaN;
            break;
          }
          sum += l * r;
        }
        out.data[i][j] = real(sum);
      }
    out.rownames = a.m.rownames;
    out.colnames = b.m.colnames;
    return mxMatrix(std::move(out));
  }
  if (op == '/') {
    if (!b.scalar) {
      err = "r(109): type mismatch";
      return std::nullopt;
    }
    if (b.s == 0.0 || mxMissing(b.s)) {
      err = "r(504): matrix has missing values";
      return std::nullopt;
    }
    // StataBE scales by the reciprocal: `D/10` is D*(1/10) to the last bit
    // (+1.3333333333334X-002 for 3/10), `D/3/7` is D*(1/3)*(1/7).
    return scaled(std::move(a.m), 1.0 / b.s);
  }
  // `#`: the Kronecker product; row (i,k) is named A's row i as the equation
  // and B's row k as the name (an unnamed A row by its own position, an
  // unnamed B row by its position in the result).
  if (a.scalar || b.scalar) {
    err = "r(109): type mismatch";
    return std::nullopt;
  }
  const auto subname = [](const std::string& n) {
    const size_t colon = n.rfind(':');
    return colon == std::string::npos ? n : n.substr(colon + 1);
  };
  const auto pairNames = [&subname](const std::vector<std::string>& outer,
                                    const std::vector<std::string>& inner, char kind) {
    std::vector<std::string> names;
    for (size_t i = 0; i < outer.size(); ++i) {
      const std::string eq =
          outer[i].empty() ? std::string(1, kind) + std::to_string(i + 1) : subname(outer[i]);
      for (const auto& n : inner) names.push_back(eq + ":" + subname(n));
    }
    return names;
  };
  const size_t rb = b.m.data.size(), cb = mxCols(b.m);
  eval::EvalMatrix out;
  out.data.assign(a.m.data.size() * rb, std::vector<double>(mxCols(a.m) * cb, 0.0));
  for (size_t i = 0; i < a.m.data.size(); ++i)
    for (size_t j = 0; j < a.m.data[i].size(); ++j)
      for (size_t k = 0; k < rb; ++k)
        for (size_t l = 0; l < cb; ++l) {
          const double x = a.m.data[i][j], y = b.m.data[k][l];
          out.data[i * rb + k][j * cb + l] = mxMissing(x) || mxMissing(y) ? kNaN : real(x * y);
        }
  out.rownames = pairNames(a.m.rownames, b.m.rownames, 'r');
  out.colnames = pairNames(a.m.colnames, b.m.colnames, 'c');
  return mxMatrix(std::move(out));
}

// One operand the operator levels of evalMx have left: a group, a matrix
// function, a subscript, a stored NAME or an e()/r() matrix. A matrix inside
// anything else (`A^2`, `A < 2`) is r(109).
std::optional<MxValue> evalMxPrimary(const std::string& e, SessionState& session,
                                     std::string& err, bool joinOperand) {
  // A group: one holding its own `,`/`\` is a literal (read whole by
  // parseMatrixLiteral when every cell is a scalar, unnamed); any other is
  // the expression inside, so `(2)` is a scalar (`A/(2)` divides, `(1,.a) #
  // (1)` is r(109)).
  if (wrappedInParens(e)) {
    const std::string in = trim(e.substr(1, e.size() - 2));
    if (!in.empty() && (!mxTopLevelOps(in, ',').empty() || !mxTopLevelOps(in, '\\').empty())) {
      std::optional<std::vector<std::vector<double>>> rows = parseMatrixLiteral(e, session);
      if (rows.has_value()) {
        eval::EvalMatrix m;
        m.data = std::move(*rows);
        return mxMatrix(std::move(m));
      }
    }
    return evalMx(in, session, err, joinOperand);
  }
  // A bracket group is a parenthesised one (mxBracketGroupAt).
  if (e.size() >= 2 && e[0] == '[' && mxClosing(e, 0) + 1 == e.size() && mxBracketGroupAt(e, 0))
    return evalMxPrimary("(" + e.substr(1, e.size() - 2) + ")", session, err, joinOperand);
  // A call's `(` follows its name with no blank between: StataBE reads `J
  // (2,2,0)` and `nullmat (Zb)` as the names J and nullmat, r(111) `J not
  // found`, and `A (1,2)` with A a matrix as r(198) (2026-11-03 oracle).
  {
    static const std::regex reSpacedCall(std::string("^(") + eval::kRxName + R"()[ \t]+\()");
    std::smatch sc;
    if (std::regex_search(e, sc, reSpacedCall)) {
      err = session.eval.findMatrix(sc.str(1)) != nullptr ? "r(198): invalid syntax"
                                                          : "r(111): " + sc.str(1) + " not found";
      return std::nullopt;
    }
  }
  auto callArg = [&e](const char* fn, std::string& inner) {
    const size_t n = std::strlen(fn);
    if (e.size() < n + 2 || e.compare(0, n, fn) != 0) return false;
    const size_t i = n;
    if (e[i] != '(' || mxClosing(e, i) + 1 != e.size()) return false;
    inner = trim(e.substr(i + 1, e.size() - i - 2));
    return true;
  };
  std::string inner;
  if (callArg("J", inner)) {
    std::optional<eval::EvalMatrix> m = evalJCall(inner, session, err);
    if (!m.has_value()) return std::nullopt;
    return mxMatrix(std::move(*m));
  }
  // nullmat(ARG): a stored matrix, else absent (see MxValue).
  if (callArg("nullmat", inner)) {
    const std::string arg = mxSqueeze(inner);
    if (const eval::EvalMatrix* stored = session.eval.findMatrix(arg)) return mxStored(*stored);
    if (!joinOperand) {
      err = "r(111): matrix " + arg + " not found";
      return std::nullopt;
    }
    MxValue v = mxMatrix(eval::EvalMatrix{});
    v.absent = true;
    v.absentArg = arg;
    return v;
  }
  // I(n): the n x n identity (oracle w2c_functions.log:75 I_n3), default
  // stripe; n is a dimension (`I(`K')`, `I(sc+1)`, `I(2.5)` is 3x3). More than
  // one argument is r(198), as for J() (StataBE: `I(1,2)`).
  if (callArg("I", inner)) {
    std::vector<std::string> args;
    std::vector<char> argOps;
    if (!splitTopLevel(inner, args, argOps) || args.size() != 1) {
      err = "r(198): invalid syntax";
      return std::nullopt;
    }
    size_t n = 0;
    if (!mxDimension(inner, session, n, err)) return std::nullopt;
    eval::EvalMatrix m;
    m.data.assign(n, std::vector<double>(n, 0.0));
    for (size_t k = 0; k < n; ++k) m.data[k][k] = 1.0;
    applyDefaultStripe(m);
    return mxMatrix(std::move(m));
  }
  // The one argument of vecdiag()/inv()/invsym()/syminv(): any matrix
  // expression (`inv((1,2\3,4))`). StataBE 19.5 (2026-11-03 oracle): two
  // arguments are r(198), a scalar r(109) (`inv(2)`), an unknown name r(111)
  // `NAME not found`.
  const auto matrixArg = [&session, &err](const std::string& text) -> std::optional<MxValue> {
    std::vector<std::string> args;
    std::vector<char> argOps;
    if (!splitTopLevel(text, args, argOps) || args.size() != 1) {
      err = "r(198): invalid syntax";
      return std::nullopt;
    }
    std::optional<MxValue> v = evalMx(text, session, err);
    if (v.has_value() && v->scalar) {
      err = "r(109): type mismatch";
      return std::nullopt;
    }
    return v;
  };
  // vecdiag(M): the 1 x n row of a SQUARE M's diagonal (oracle
  // w2c_functions.log:100 vecdiag_basic: row name r1, M's column names), M
  // any matrix (`vecdiag(e(V))`, `vecdiag((1,2\3,4))`); a non-square M is
  // r(503) `conformability error` (StataBE 19.5), never a truncated diagonal.
  // vecdiag is a matrix-sublanguage function only: `local x =
  // el(vecdiag(M),1,3)` stays r(133) (w2c_functions.log:110).
  // invsym(M) / syminv(M) / inv(M) ([FN] Matrix functions): the inverse of a
  // square matrix. invsym is Stata's sweep in column order: a pivot that falls
  // to (relatively) zero drops that row and column, which are returned as 0
  // (the generalized inverse). inv() is an LU decomposition with implicitly
  // scaled partial pivoting -- each row weighed by the reciprocal of its
  // largest magnitude, the pivot the largest weighed entry (the last of a
  // tie), each multiplier formed with the pivot's reciprocal -- then a forward
  // and a back substitution (dividing by the diagonal) per column of I: the
  // method whose %21x bits StataBE 19.5's inv() reproduces, 30 of 30 matrices
  // with ties and near-singular ones (2026-11-03 oracle). A zero row or an
  // exactly zero pivot is its r(504) `matrix has missing values` (`inv(T'*T)`
  // for T = (7,8)); `inv((1,2,3\4,5,6\7,8,9))`, whose rounded last pivot is
  // not zero, answers. The inverse's rows are the source's columns.
  for (const char* fnName : {"syminv", "invsym", "inv"}) {
    if (!callArg(fnName, inner)) continue;
    std::optional<MxValue> arg = matrixArg(inner);
    if (!arg.has_value()) return std::nullopt;
    const eval::EvalMatrix* src = &arg->m;
    const size_t n = src->data.size();
    if (n == 0 || src->data[0].size() != n) {
      err = "r(503): conformability error";
      return std::nullopt;
    }
    for (const auto& row : src->data)
      for (double v : row)
        if (!std::isfinite(v) || v >= eval::kStataMissingSentinel) {
          err = "r(504): matrix has missing values";
          return std::nullopt;
        }
    eval::EvalMatrix m = *src;
    if (std::string(fnName) == "inv") {
      std::vector<std::vector<double>> a = src->data;
      std::vector<size_t> perm(n);  // perm[i]: the source row now at row i
      std::vector<double> weight(n);
      for (size_t i = 0; i < n; ++i) {
        perm[i] = i;
        double big = 0.0;
        for (double v : a[i]) big = std::max(big, std::fabs(v));
        if (big == 0.0) {
          err = "r(504): matrix has missing values";
          return std::nullopt;
        }
        weight[i] = 1.0 / big;
      }
      for (size_t c = 0; c < n; ++c) {
        size_t piv = c;
        double best = -1.0;
        for (size_t r = c; r < n; ++r) {
          const double d = weight[r] * std::fabs(a[r][c]);
          if (d >= best) {
            best = d;
            piv = r;
          }
        }
        if (a[piv][c] == 0.0) {
          err = "r(504): matrix has missing values";
          return std::nullopt;
        }
        std::swap(a[piv], a[c]);
        std::swap(perm[piv], perm[c]);
        std::swap(weight[piv], weight[c]);
        const double recip = 1.0 / a[c][c];
        for (size_t r = c + 1; r < n; ++r) a[r][c] *= recip;
        for (size_t j = c + 1; j < n; ++j)
          for (size_t r = c + 1; r < n; ++r) a[r][j] -= a[r][c] * a[c][j];
      }
      std::vector<std::vector<double>> out(n, std::vector<double>(n, 0.0));
      std::vector<double> x(n);
      for (size_t col = 0; col < n; ++col) {
        for (size_t i = 0; i < n; ++i) {
          double s = perm[i] == col ? 1.0 : 0.0;
          for (size_t k = 0; k < i; ++k) s -= a[i][k] * x[k];
          x[i] = s;
        }
        for (size_t i = n; i-- > 0;) {
          double s = x[i];
          for (size_t k = i + 1; k < n; ++k) s -= a[i][k] * x[k];
          x[i] = s / a[i][i];
        }
        for (size_t i = 0; i < n; ++i) out[i][col] = x[i];
      }
      m.data = std::move(out);
    } else {
      // Symmetric sweep (Goodnight) over the symmetrized matrix.
      std::vector<std::vector<double>> a(n, std::vector<double>(n, 0.0));
      for (size_t i = 0; i < n; ++i)
        for (size_t j = 0; j < n; ++j) a[i][j] = 0.5 * (src->data[i][j] + src->data[j][i]);
      std::vector<double> d0(n);
      for (size_t i = 0; i < n; ++i) d0[i] = a[i][i];
      std::vector<char> dropped(n, 0);
      for (size_t k = 0; k < n; ++k) {
        const double piv = a[k][k];
        if (!(std::fabs(piv) > 1e-13 * std::max(std::fabs(d0[k]), 1e-300)) || d0[k] == 0.0) {
          dropped[k] = 1;
          continue;
        }
        const double inv = 1.0 / piv;
        for (size_t i = 0; i < n; ++i) {
          if (i == k) continue;
          for (size_t j = 0; j < n; ++j) {
            if (j == k) continue;
            a[i][j] -= a[i][k] * a[k][j] * inv;
          }
        }
        for (size_t i = 0; i < n; ++i) {
          if (i == k) continue;
          a[i][k] *= inv;
          a[k][i] *= inv;
        }
        a[k][k] = -inv;
      }
      for (size_t i = 0; i < n; ++i)
        for (size_t j = 0; j < n; ++j) a[i][j] = (dropped[i] || dropped[j]) ? 0.0 : -a[i][j];
      m.data = std::move(a);
    }
    // The inverse's rows are the source's columns and vice versa.
    std::swap(m.rownames, m.colnames);
    return mxMatrix(std::move(m));
  }
  if (callArg("vecdiag", inner)) {
    std::optional<MxValue> arg = matrixArg(inner);
    if (!arg.has_value()) return std::nullopt;
    const eval::EvalMatrix& src = arg->m;
    const size_t nrows = src.data.size();
    if (nrows != mxCols(src)) {
      err = "r(503): conformability error";
      return std::nullopt;
    }
    eval::EvalMatrix m;
    m.data.assign(1, std::vector<double>(nrows, 0.0));
    for (size_t k = 0; k < nrows; ++k) m.data[0][k] = src.data[k][k];
    m.rownames = {"r1"};
    m.colnames = src.colnames;
    return mxMatrix(std::move(m));
  }
  // A subscript of a stored matrix: `A[]` is A whole; two scalar subscripts
  // read one element, a scalar (mxElementAt; `matrix X = A[1,1]` is r1 x c1,
  // oracle w2q L02); a range or a row/column name in either is a submatrix
  // that keeps its names (BUG-542, mxAxisRange). A malformed body is
  // mxSubscriptSpecs' refusal.
  if (!e.empty() && e.back() == ']') {
    const size_t br = e.find('[');
    static const std::regex reIdent(std::string("^") + eval::kRxName + "$");
    if (br != std::string::npos && br > 0 && std::regex_match(e.substr(0, br), reIdent) &&
        mxClosing(e, br) + 1 == e.size()) {
      if (const eval::EvalMatrix* src = session.eval.findMatrix(e.substr(0, br))) {
        const std::string body = e.substr(br + 1, e.size() - br - 2);
        if (trim(body).empty()) return mxMatrix(*src);
        const std::optional<std::pair<std::string, std::string>> specs =
            mxSubscriptSpecs(e.substr(0, br), body, err);
        if (!specs.has_value()) return std::nullopt;
        if (!mxRangeSpec(specs->first) && !mxRangeSpec(specs->second)) {
          double r = 0.0, c = 0.0;
          if (!evalMatrixScalarExpr(specs->first, session, r, err) ||
              !evalMatrixScalarExpr(specs->second, session, c, err))
            return std::nullopt;
          return mxScalar(mxElementAt(*src, r, c));
        }
        std::optional<eval::EvalMatrix> part =
            mxSubmatrix(*src, specs->first, specs->second, session, err);
        if (!part.has_value()) return std::nullopt;
        return mxMatrix(std::move(*part));
      }
    }
  }
  if (const eval::EvalMatrix* stored = session.eval.findMatrix(e)) return mxStored(*stored);
  // An e()/r() class MATRIX, `(e(b) \ e(b))`, through the one resolver.
  {
    static const std::regex reClassRef(std::string(R"(^([er])\(\s*()") + eval::kRxName + R"()\s*\)$)");
    std::smatch cm;
    if (std::regex_match(e, cm, reClassRef)) {
      const char cls = cm.str(1)[0];
      const std::string which = cm.str(2);
      if (session.eval.findClassMatrix(cls, which) != nullptr ||
          (cls == 'e' && (which == "b" || which == "V"))) {
        std::string classErr;
        int classRc = 111;
        std::optional<eval::EvalMatrix> resolved =
            resolveClassMatrixRef(cls, which, session, classErr, &classRc);
        if (resolved.has_value()) return mxStored(*resolved);
        // The refusal carries its rc through matrixExprErrorBlock (BUG-584),
        // never runMatrixDef's code-less block (rc 1): no e() at all is r(301),
        // Stata's "last estimates not found" (w2g_ebmat.log:2229); an absent
        // name is r(111) -- the resolver says which.
        err = "r(" + std::to_string(classRc) + "): " + classErr;
        return std::nullopt;
      }
    }
  }
  // A name, `'` and more name characters with no space between (`X'X`, `A'2`)
  // is one name to StataBE, which it then does not find: r(111) `X'X not
  // found` (2026-11-03 oracle; `X' X` is two operands, r(198)).
  {
    static const std::regex reQuoteJoined(std::string("^") + eval::kRxName + "('" +
                                          eval::kRxNameChar + "+)+$");
    if (std::regex_match(e, reQuoteJoined)) {
      err = "r(111): " + e + " not found";
      return std::nullopt;
    }
  }
  // A matrix inside a scalar-only construct, `A^2`, `sqrt(A)`, `A < 2`: StataBE's
  // r(109) (BUG-584). Two operands with no operator between them (`A A`) are
  // still invalid syntax, r(198).
  if (!firstBareMatrixName(e, session).empty()) {
    double ignored = 0.0;
    std::string scalarErr;
    if (!evalMatrixScalarExpr(e, session, ignored, scalarErr) &&
        scalarErr.rfind("r(198)", 0) == 0) {
      err = scalarErr;
      return std::nullopt;
    }
    err = "r(109): type mismatch";
    return std::nullopt;
  }
  double v = 0.0;
  if (!evalMatrixScalarExpr(e, session, v, err)) return std::nullopt;
  return mxScalar(v);
}

// The operand a dangling operator is read into: the last one of `part`, or
// its first one when the operator leads (StataBE: `(1,2\)` is `2\ not found`).
std::string mxEdgeOperand(const std::string& part, bool last) {
  size_t cut = last ? 0 : part.size();
  for (const char op : {'\\', ',', '+', '-', '*', '/', '#'})
    for (const size_t p : mxTopLevelOps(part, op)) cut = last ? std::max(cut, p + 1) : std::min(cut, p);
  return mxSqueeze(last ? part.substr(cut) : part.substr(0, cut));
}

std::optional<MxValue> evalMx(const std::string& text, SessionState& session, std::string& err,
                              bool joinOperand) {
  const std::string e = trim(text);
  if (e.empty()) {
    err = "r(198): invalid syntax";
    return std::nullopt;
  }
  {
    int depth = 0;
    for (size_t i = 0; i < e.size(); ++i) {
      if (e[i] == '"') {
        const size_t close = e.find('"', i + 1);
        if (close == std::string::npos) break;
        i = close;
      } else if (e[i] == '(' || e[i] == '[') {
        ++depth;
      } else if ((e[i] == ')' || e[i] == ']') && --depth < 0) {
        err = "r(132): too many ')' or ']'";
        return std::nullopt;
      }
    }
    if (depth > 0) {
      err = "r(132): too few ')' or ']'";
      return std::nullopt;
    }
  }
  if (!mxNeedsGrammar(e, session)) {
    double v = 0.0;
    if (!evalMatrixScalarExpr(e, session, v, err)) return std::nullopt;
    return mxScalar(v);
  }
  for (const char op : {'\\', ',', '+', '-', '*', '/', '#'}) {
    const std::vector<size_t> at = mxTopLevelOps(e, op);
    if (at.empty()) continue;
    std::vector<std::string> parts;
    size_t from = 0;
    for (const size_t p : at) {
      parts.push_back(trim(e.substr(from, p - from)));
      from = p + 1;
    }
    parts.push_back(trim(e.substr(from)));
    // Left to right. A dangling operator is read into the operand before it,
    // or after it when it leads, and that name is looked up (StataBE: `A +`
    // is `A+ not found`, `A * B *` is `B* not found`, `*A * B` is `*A not
    // found`, `A + B + +C` is `B+ not found`, while `nosuch + A +` is `nosuch
    // not found`), r(111).
    std::optional<MxValue> acc;
    for (size_t k = 0; k < parts.size(); ++k) {
      std::string glued;
      if (k == 0 && parts[0].empty()) glued = std::string(1, op) + mxEdgeOperand(parts[1], false);
      else if (k + 1 < parts.size() && parts[k + 1].empty()) glued = mxEdgeOperand(parts[k], true) + op;
      if (!glued.empty()) {
        err = "r(111): " + glued + " not found";
        return std::nullopt;
      }
      std::optional<MxValue> v = evalMx(parts[k], session, err, op == '\\' || op == ',');
      if (!v.has_value()) return std::nullopt;
      acc = k == 0 ? std::move(v) : mxApply(op, std::move(*acc), std::move(*v), err);
      if (!acc.has_value()) return std::nullopt;
    }
    return acc;
  }
  if (e[0] == '-') {
    std::optional<MxValue> v = evalMx(e.substr(1), session, err);
    if (!v.has_value()) return std::nullopt;
    return mxNegate(std::move(*v));
  }
  // A leading `+` is only a number's sign (mxPlusRefusal): `+2'` is 2, `+A`
  // r(111) `+A not found`, `+(1,2)` r(133).
  if (e[0] == '+') {
    err = mxPlusRefusal(e);
    if (!err.empty()) return std::nullopt;
    return evalMx(e.substr(1), session, err);
  }
  if (e.back() == '\'') {
    std::optional<MxValue> v = evalMx(e.substr(0, e.size() - 1), session, err);
    if (v.has_value() && !v->scalar) {
      v->m = transposeMatrix(v->m);
      v->stored = false;
    }
    return v;
  }
  return evalMxPrimary(e, session, err, joinOperand);
}

// The matrix a right-hand side evaluates to (a scalar as a 1x1), its unnamed
// rows and columns named by position: r1.., c1.., and an `eq:` name from `#`
// gets the position as its name.
std::optional<eval::EvalMatrix> evalMatrixRhs(const std::string& rhs, SessionState& session,
                                              std::string& err) {
  std::optional<MxValue> v = evalMx(rhs, session, err);
  if (!v.has_value()) return std::nullopt;
  eval::EvalMatrix m = mxAsMatrix(*v);
  m.rownames.resize(m.data.size());
  m.colnames.resize(mxCols(m));
  const auto fill = [](std::vector<std::string>& names, char kind) {
    for (size_t k = 0; k < names.size(); ++k)
      if (names[k].empty() || names[k].back() == ':')
        names[k] += std::string(1, kind) + std::to_string(k + 1);
  };
  fill(m.rownames, 'r');
  fill(m.colnames, 'c');
  return m;
}

// Scalars and matrices share one namespace ([P] matrix define, "Namespace"):
// defining the matrix destroys a scalar of the same name.
void storeMatrixResult(SessionState& session, const std::string& name, eval::EvalMatrix mat) {
  session.eval.scalars.erase(name);
  session.eval.stringScalars.erase(name);
  for (auto& p : session.eval.matrices) {
    if (p.first == name) { p.second = std::move(mat); return; }
  }
  session.eval.matrices.emplace_back(name, std::move(mat));
}

// `matrix list`'s cell text: Stata's default %10.0g, trimmed ("10", ".25",
// "3.236e-07", "1.235e+08"; BUG-1411, StataBE 19.5 scratch-laneA/lead/ml/ml.do
// and the w2_smalls.log listings). It was a fixed %.4f with an integer snap, so a
// small variance listed as 0.0000.
std::string formatMatrixCell(double v) {
  if (auto miss = eval::stataMissingToken(v)) return *miss;  // `.a` is .a, not 9e+307 (BUG-1399)
  if (std::isnan(v)) return ".";
  return piet::fmt::general(v, 10, 0);
}

// `matrix list`'s "symmetric" header: every cell equals its mirror (within
// 1e-8). X'X products (A'*A, accum) are exactly symmetric in IEEE754 (same
// summation order, commutative per-term multiply).
bool isSymmetricMatrix(const eval::EvalMatrix& m) {
  const size_t n = m.data.size();
  if (n == 0 || m.data[0].size() != n) return false;
  // A missing cell mirrors only the same missing value (a NaN difference used
  // to compare as "close", so (3,. \ 6,.) listed as symmetric).
  const auto kind = [](double v) {
    return std::isnan(v) ? 0 : eval::stataExtendedMissingKind(v).value_or(0);
  };
  for (size_t i = 0; i < n; ++i) {
    if (m.data[i].size() != n) return false;
    for (size_t j = i + 1; j < n; ++j) {
      const double a = m.data[i][j], b = m.data[j][i];
      if (mxMissing(a) || mxMissing(b)) {
        if (!(mxMissing(a) && mxMissing(b) && kind(a) == kind(b))) return false;
      } else if (std::fabs(a - b) > 1e-8) {
        return false;
      }
    }
  }
  return true;
}

// `matrix list NAME` rendering, line for line as StataBE 19.5 prints it
// (BUG-1412; 2026-11-03 oracle, and w2g_ebmat.log's e(V) / teffects / oaxaca
// listings):
//
//   symmetric S[2,2]                     E[2,3]
//                e:     e:                            y:      y:  lnsig:
//                x  _cons                            x1   _cons   _cons
//       e:x      1                         eq1:a       1       2       3
//   e:_cons      2      5                 eq2:bb       4       5       6
//
// - Every column has ONE width w: the widest printed cell (symmetric -> the
//   lower triangle incl. diagonal), column name, and equation or operator
//   part plus its delimiter. Columns are 2 blanks apart; row labels are
//   right-aligned to the longest.
// - A column name `eq:name` puts eq on a header line of its own, its `:` one
//   place right of the column; `op.name` and `a#b` put the part up to the last
//   `#`, else the last `.`, on the next line the same way (`1b.` over `c`,
//   `c.x#` over `c.x`). A line appears when any column has that part.
// - A name or operator part is cut to 12 characters as abbrev() cuts it
//   (`averyveryl~e`, `1.averyver~x#`), an equation to 11 before its `:`; a
//   row label is `eq:` + its name so cut, or the whole label cut.
// - A factor level's `bn` marker is not printed (`2bn.g` lists as `2.` over
//   `g`, width 3), matrixNameAsListed.
std::string renderMatrixList(const std::string& name, const eval::EvalMatrix& m,
                             const std::function<std::string(double)>& cellText = nullptr) {
  const size_t nrows = m.data.size();
  const size_t ncols = nrows > 0 ? m.data[0].size() : 0;
  // A 0x0 matrix (`matrix S = e(V)` after `ereturn post` of b alone) is
  // `symmetric S[0,0]` and nothing more.
  const bool empty = nrows == 0 && ncols == 0;
  const bool sym = empty || isSymmetricMatrix(m);
  const auto abbrev = [](const std::string& s, size_t n) {
    return s.size() <= n ? s : s.substr(0, n - 2) + "~" + s.back();
  };

  std::vector<std::string> rlabels = m.rownames;
  rlabels.resize(nrows);
  size_t rowLabelWidth = 0;
  for (size_t i = 0; i < nrows; ++i) {
    std::string& label = rlabels[i];
    if (label.empty()) label = "r" + std::to_string(i + 1);
    label = matrixNameAsListed(label);
    const size_t colon = label.find(':');
    label = colon == std::string::npos
                ? abbrev(label, 12)
                : label.substr(0, colon + 1) + abbrev(label.substr(colon + 1), 12);
    rowLabelWidth = std::max(rowLabelWidth, label.size());
  }
  // Each column's equation, operator and name parts (the first two without
  // their delimiter), cut to their widths.
  std::vector<std::string> ceq(ncols), cop(ncols), cname(ncols);
  std::vector<char> opDelim(ncols, ' ');
  bool anyEq = false, anyOp = false;
  size_t w = 0;
  for (size_t j = 0; j < ncols; ++j) {
    std::string full = j < m.colnames.size() ? matrixNameAsListed(m.colnames[j]) : std::string();
    if (full.empty()) full = "c" + std::to_string(j + 1);
    if (const size_t colon = full.find(':'); colon != std::string::npos) {
      ceq[j] = abbrev(full.substr(0, colon), 11);
      full = full.substr(colon + 1);
      anyEq = true;
      w = std::max(w, ceq[j].size() + 1);
    }
    size_t cut = full.rfind('#');
    if (cut == std::string::npos) cut = full.rfind('.');
    if (cut != std::string::npos && cut > 0 && cut + 1 < full.size()) {
      cop[j] = abbrev(full.substr(0, cut), 12);
      opDelim[j] = full[cut];
      full = full.substr(cut + 1);
      anyOp = true;
      w = std::max(w, cop[j].size() + 1);
    }
    cname[j] = abbrev(full, 12);
    w = std::max(w, cname[j].size());
  }

  std::vector<std::vector<std::string>> cells(nrows, std::vector<std::string>(ncols));
  for (size_t i = 0; i < nrows; ++i) {
    const size_t limit = sym ? std::min(i + 1, ncols) : ncols;  // lower triangle incl. diagonal
    for (size_t j = 0; j < limit; ++j) {
      cells[i][j] = cellText ? cellText(m.data[i][j]) : formatMatrixCell(m.data[i][j]);
      w = std::max(w, cells[i][j].size());
    }
  }

  auto rjust = [](const std::string& s, size_t width) {
    return s.size() >= width ? s : std::string(width - s.size(), ' ') + s;
  };

  std::ostringstream out;
  out << "\n";
  if (sym) out << "symmetric ";
  out << name << "[" << nrows << "," << ncols << "]\n";
  if (empty) return out.str();

  // An equation or operator line: each part right-aligned in its column, its
  // delimiter in the first of the two blanks after it.
  const auto partLine = [&](const std::vector<std::string>& part, const auto& delim) {
    out << std::string(rowLabelWidth + 1, ' ');
    for (size_t j = 0; j < ncols; ++j)
      out << ' ' << rjust(part[j], w) << (part[j].empty() ? ' ' : delim(j));
    out << "\n";
  };
  if (anyEq) partLine(ceq, [](size_t) { return ':'; });
  if (anyOp) partLine(cop, [&opDelim](size_t j) { return opDelim[j]; });
  out << std::string(rowLabelWidth, ' ');
  for (size_t j = 0; j < ncols; ++j) out << "  " << rjust(cname[j], w);
  out << "\n";

  for (size_t i = 0; i < nrows; ++i) {
    const size_t limit = sym ? std::min(i + 1, ncols) : ncols;
    out << rjust(rlabels[i], rowLabelWidth);
    for (size_t j = 0; j < limit; ++j) out << "  " << rjust(cells[i][j], w);
    out << "\n";
  }
  return out.str();
}

// `matrix rownames NAME = n1 n2 ...` / `matrix colnames NAME = n1 n2 ...`
// StataBE 19.5 (BUG-1409, 2026-11-03 oracle): an absent NAME is r(111)
// `matrix NAME not found`; more names than rows/columns is r(503)
// `conformability error`; fewer is accepted, the last row/column named
// naming every one left (`matrix rownames A3 = a b` on a 3-row A3 names them
// a b b). A name is a word or a quoted string, blanks and all (`"a b" c`
// names two rows); an empty one, `""`, keeps that row's name (`"" c` on r1
// r2 r3 is r1 c c), and an empty list changes nothing.
ojson runMatrixStripe(bool isRow, const std::string& name, const std::string& namesRhs,
                       SessionState& session) {
  std::vector<std::string> names;
  for (std::string rest = namesRhs; !trim(rest).empty();) {
    auto [tok, next] = eval::scanOneToken(rest, /*bind=*/true);
    names.push_back(tok);
    rest = std::move(next);
  }
  eval::EvalMatrix* mat = nullptr;
  for (auto& p : session.eval.matrices) {
    if (p.first == name) { mat = &p.second; break; }
  }
  if (mat == nullptr) return engineErrorBlockRc("matrix " + name + " not found", 111);
  const size_t nrows = mat->data.size();
  const size_t ncols = nrows > 0 ? mat->data[0].size() : 0;
  const size_t want = isRow ? nrows : ncols;
  if (names.size() > want) return engineErrorBlockRc("conformability error", 503);
  std::vector<std::string>& stripe = isRow ? mat->rownames : mat->colnames;
  stripe.resize(want);
  for (size_t k = 0; k < want; ++k) {
    if (k < names.size()) {
      if (!names[k].empty()) stripe[k] = names[k];
    } else if (!names.empty()) {
      stripe[k] = stripe[names.size() - 1];
    }
    if (stripe[k].empty()) stripe[k] = std::string(1, isRow ? 'r' : 'c') + std::to_string(k + 1);
  }
  return ojson::array();
}

// `matrix accum NAME = varlist [if] [in] [weight] [, noconstant deviations
// means(M)]` ([P] matrix accum) -- A = X'WX where X = [varlist, 1] (the
// constant column appended LAST, dropped by noconstant), over the observations
// where no listed variable and no weight is missing (a row missing ANY of them
// is dropped entirely -- the rule `regress` uses). Verified against the P4 wave
// fixture: `matrix accum P4C = x1 x2` gives casewise n = 4671 and P4C[1,1] =
// 3896.1577 (internal capture p4_stata.log).
//
// BUG-1408 (StataBE 19.5, 2026-11-03 oracle; it refused every option):
//  - noconstant (noc...) drops the _cons row and column; deviations (d...)
//    replaces the variables' block with sum_i w_i (x_i - m)(x_i - m)', m the
//    weighted means, and keeps the _cons row (sum_i w_i x_i, sum_i w_i);
//    means(M) (m...) stores the 1 x k(+1) row of those means, row _cons, 1 under
//    _cons. Any other option is r(198) `option NAME not allowed`.
//  - Weights: an aweight is rescaled to sum to the number of observations it
//    keeps (an aweight <= 0 drops its row); fweights, pweights and iweights are
//    used as given (a frequency or probability weight <= 0 drops its row, an
//    importance weight never does; a non-integer fweight is r(401)). No row left
//    is r(2000) `no observations`, as is a pweight sum below 1, but iweights
//    that sum to 0 are rc 0; an fweight or pweight sum past 2^31 is r(1400).
//  - The note: `(obs=N)` (N the observations, an fweight's sum, a pweight's sum
//    truncated, an iweight's sum %12.0g), after `(sum of wgt is S)` for
//    aweights (%18.0gc); r(N) is the observations, or the weights' sum for
//    fweights, pweights and iweights, and replaces r().
//  - Bits: a cross product is summed in row order; the aweight rescaling, the
//    means and the _cons row of `deviations` use compensated sums (an aweight
//    _cons cell is the observation count itself). That reproduces 88 of the
//    90 %21x cells of a sweep over four weight variables, aw/pw/iw, with and
//    without deviations; the other two, deviation cells, are 1-2 ulps off.
ojson runMatrixAccum(const std::string& name, const std::string& rhs, SessionState& session) {
  eval::ParsedCommand parsed;
  try {
    parsed = eval::tokenize("matrix accum " + name + " = " + rhs);
  } catch (const eval::TokenizerError& te) {
    return ojson(te.block);
  }
  std::vector<std::string> vars;
  {
    const auto eq = std::find(parsed.varlist.begin(), parsed.varlist.end(), "=");
    if (eq != parsed.varlist.end()) vars.assign(eq + 1, parsed.varlist.end());
  }
  if (vars.empty()) return engineErrorBlock("matrix accum: no variables specified");
  // Factor-variable and time-series operators (`i.g`, `L.x`, `c.x#c.z`) build
  // columns of their own, which this handler does not: refused by name rather
  // than looked up as a variable called `i.g`.
  for (const auto& v : vars) {
    if (v.find_first_of(".#") != std::string::npos)
      return engineErrorBlockRc("matrix accum: factor-variable and time-series operators (" + v +
                                    ") are not supported in this version of PIET",
                                198);
  }
  // The varlist as every Stata varlist reads: `x*`, `xa-longname` and a unique
  // abbreviation (`longn`) name the variables they stand for.
  if (auto bad = resolveByKeyNames(session, vars)) return *bad;
  bool noConstant = false, deviations = false;
  std::string meansName;
  for (const auto& [key, val] : parsed.opts) {
    const auto abbrevOf = [&key](const std::string& full, size_t shortest) {
      return key.size() >= shortest && full.compare(0, key.size(), key) == 0;
    };
    if (abbrevOf("noconstant", 3) && val.isBool) {
      noConstant = true;
    } else if (abbrevOf("deviations", 1) && val.isBool) {
      deviations = true;
    } else if (abbrevOf("means", 1) && !val.isBool) {
      meansName = trim(val.strValue);
      if (!meansName.empty() && !strutil::isValidStataIdentifier(meansName))
        return engineErrorBlockRc(meansName + " invalid name", 198);
    } else {
      return engineErrorBlockRc("option " + key + " not allowed", 198);
    }
  }
  std::string wType;
  std::string wVar = parsed.weightVar.value_or(std::string());
  if (parsed.weightType.has_value()) {
    wType = parsed.weightType->substr(0, 2);
    for (auto& ch : wType) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }

  if (!session.currentHbin().has_value() || session.datastore() == nullptr)
    return engineErrorBlock("no data in memory");
  // The weight is an expression, as in every Stata weight: a name is the
  // variable or its unique abbreviation (`[fw=nosuch]` and `[fw=nosuch*2]` are
  // r(111) `nosuch not found`), a string is r(109) `type mismatch` (`[fw=s]`,
  // `[fw=s+"b"]`), and anything else (`[fw=xa*2]`, `[aw=1/xa]`) is generated
  // over the whole frame into a hidden column before if/in select rows; the
  // column leaves the frame when the handler returns.
  struct WeightColumnGuard {
    SessionState& session;
    std::string column;
    ~WeightColumnGuard() {
      if (column.empty()) return;
      try {
        strmutate::removeConditionMask(session, column);
      } catch (...) {
      }
    }
  } weightColumn{session, {}};
  if (!wVar.empty()) {
    const std::vector<std::string> known = session.datastore()->names();
    const auto resolveName = [&known](const std::string& nm, std::string& out) -> std::optional<ojson> {
      if (std::find(known.begin(), known.end(), nm) != known.end()) {
        out = nm;
        return std::nullopt;
      }
      std::vector<std::string> hits;
      for (const auto& kn : known)
        if (kn.compare(0, nm.size(), nm) == 0) hits.push_back(kn);
      if (hits.size() != 1)
        return engineErrorBlockRc(hits.empty() ? nm + " not found" : nm + " ambiguous abbreviation",
                                  111);
      out = hits.front();
      return std::nullopt;
    };
    if (strutil::isValidStataIdentifier(wVar)) {
      if (auto bad = resolveName(wVar, wVar)) return *bad;
    } else {
      // Every name the expression reads must answer to a variable first.
      static const std::regex kName(R"([A-Za-z_][A-Za-z0-9_]*)");
      const std::string wexp = wVar;
      for (std::sregex_iterator it(wexp.begin(), wexp.end(), kName), end; it != end; ++it) {
        const size_t at = static_cast<size_t>(it->position());
        const size_t after = at + static_cast<size_t>(it->length());
        if (at > 0 && (std::isalnum(static_cast<unsigned char>(wexp[at - 1])) ||
                       wexp[at - 1] == '.' || wexp[at - 1] == '"'))
          continue;  // a number's exponent, a missing value's letter, a string
        if (after < wexp.size() && wexp[after] == '(') continue;  // a function
        std::string resolved;
        if (auto bad = resolveName(it->str(), resolved)) return *bad;
      }
      const strmutate::ConditionMaskInstall wi = strmutate::installWeightExpression(session, wexp);
      if (!wi.ok) return engineErrorBlockRc("type mismatch", 109);
      weightColumn.column = wi.column;
      wVar = wi.column;
    }
    Datastore* wds = session.datastore();
    if (wds == nullptr || !wds->has(wVar)) return engineErrorBlock("no data in memory");
    if (wds->dtype(wVar) != "f64") return engineErrorBlockRc("type mismatch", 109);
  }

  const std::optional<std::string> headPath = session.currentHbin();
  if (!headPath.has_value() || session.datastore() == nullptr)
    return engineErrorBlock("no data in memory");
  std::unique_ptr<Datastore> filtered;
  Datastore* ds = session.datastore();
  if (parsed.ifExpr.has_value() || parsed.inRange.has_value()) {
    InRangeFilterResult fr;
    try {
      fr = applyIfAndInFilter(session, *headPath, parsed.ifExpr, parsed.inRange);
    } catch (const CobraError& ce) {
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    if (!fr.ok) return inRangeRejectError(fr, parsed.inRange.value_or(std::string()), "matrix accum");
    filtered = std::make_unique<Datastore>(fr.dataPath);
    ds = filtered.get();
  }
  // A string variable is missing in every observation, so StataBE's answer
  // is r(2000) `no observations` (`matrix accum A = xa s`), not a type error:
  // its column is null and drops every row below.
  std::vector<const double*> cols;
  cols.reserve(vars.size());
  for (const auto& v : vars) {
    if (!ds->has(v)) return engineErrorBlockRc("variable " + v + " not found", 111);
    cols.push_back(ds->dtype(v) == "f64" ? ds->col(v).f64() : nullptr);
  }
  const double* wcol = nullptr;
  if (!wVar.empty()) {
    wcol = ds->col(wVar).f64();
  }
  const int64_t n = ds->nRows();
  const size_t k = vars.size();
  std::vector<std::vector<double>> X;  // the rows kept, [v1..vk]
  std::vector<double> W;
  X.reserve(static_cast<size_t>(std::max<int64_t>(n, 0)));
  for (int64_t r = 0; r < n; ++r) {
    bool keep = true;
    for (size_t c = 0; c < k; ++c) {
      if (cols[c] == nullptr || cecon::is_missing(cols[c][r])) { keep = false; break; }
    }
    if (keep && wcol != nullptr) {
      const double w = wcol[r];
      keep = !cecon::is_missing(w) && (wType == "iw" || w > 0.0);
      if (keep && wType == "fw" && w != std::floor(w))
        return engineErrorBlockRc("may not use noninteger frequency weights", 401);
    }
    if (!keep) continue;
    std::vector<double> row(k);
    for (size_t c = 0; c < k; ++c) row[c] = cols[c][r];
    X.push_back(std::move(row));
    W.push_back(wcol != nullptr ? wcol[r] : 1.0);
  }
  if (X.empty()) return engineErrorBlockRc("no observations", 2000);

  // r(N) of a weighted sum is the plain running sum (`return list` after
  // `[iw=w]`, w = 1/3+_n, n = 5: 16.66666666666666).
  double plainSum = 0.0;
  for (const double w : W) plainSum += w;
  // A frequency or probability weight's sum is a count of observations: one
  // that truncates to 0 is r(2000) (`[pw=w]`, w = 1e-5*_n), one past a 4-byte
  // integer r(1400) (`[fw=w]`, w = 1e9 on 5 rows).
  if (wType == "fw" || wType == "pw") {
    if (std::trunc(plainSum) > 2147483647.0)
      return engineErrorBlockRc("numerical overflow", 1400);
    if (std::trunc(plainSum) < 1.0) return engineErrorBlockRc("no observations", 2000);
  }

  // Neumaier's compensated sum.
  const auto accurateSum = [](const std::vector<double>& v) {
    double s = 0.0, comp = 0.0;
    for (const double x : v) {
      const double t = s + x;
      comp += std::fabs(s) >= std::fabs(x) ? (s - t) + x : (x - t) + s;
      s = t;
    }
    return s + comp;
  };
  const double nObs = static_cast<double>(X.size());
  const double rawSum = accurateSum(W);
  if (wType == "aw") {
    const double scale = nObs / rawSum;
    for (double& w : W) w *= scale;
  }
  const double wTotal = accurateSum(W);
  const double sumW = wType == "aw" ? nObs : wTotal;
  std::vector<double> mean(k, 0.0);
  for (size_t c = 0; c < k; ++c) {
    std::vector<double> terms(X.size());
    for (size_t r = 0; r < X.size(); ++r) terms[r] = X[r][c] * W[r];
    mean[c] = accurateSum(terms) / wTotal;
  }

  const size_t dim = noConstant ? k : k + 1;
  eval::EvalMatrix mat;
  mat.data.assign(dim, std::vector<double>(dim, 0.0));
  std::vector<double> x(k + 1, 1.0);  // one row, the constant last
  for (size_t r = 0; r < X.size(); ++r) {
    for (size_t c = 0; c < k; ++c) x[c] = deviations ? X[r][c] - mean[c] : X[r][c];
    const size_t cross = deviations ? k : dim;
    for (size_t i = 0; i < cross; ++i) {
      const double xi = x[i] * W[r];
      if (xi == 0.0) continue;
      for (size_t j = 0; j < cross; ++j) mat.data[i][j] += xi * x[j];
    }
  }
  if (!noConstant) {
    for (size_t c = 0; c < k && deviations; ++c) mat.data[k][c] = mat.data[c][k] = mean[c] * sumW;
    if (deviations || wType == "aw") mat.data[k][k] = sumW;
  }
  mat.rownames = vars;
  if (!noConstant) mat.rownames.push_back("_cons");
  mat.colnames = mat.rownames;
  storeMatrixResult(session, name, std::move(mat));
  if (!meansName.empty()) {
    eval::EvalMatrix means;
    means.data.assign(1, mean);
    if (!noConstant) means.data[0].push_back(1.0);
    means.rownames = {"_cons"};
    means.colnames = vars;
    if (!noConstant) means.colnames.push_back("_cons");
    storeMatrixResult(session, meansName, std::move(means));
  }

  const double rN = wType.empty() || wType == "aw" ? nObs : plainSum;
  eval::replaceRClass(session.eval);
  session.eval.rValues.set("N", rN);
  eval::declareRPosted(session.eval, "matrix accum", {"N"});
  const auto format = [](double v, int width, bool commas) {
    fmt::StataFmtSpec sp;
    sp.hasWidth = true;
    sp.width = width;
    sp.conv = 'g';
    sp.groupThousands = commas;
    const std::optional<std::string> s = fmt::renderValue(v, sp);
    return s.has_value() ? trim(*s) : eval::pythonFloatRepr(v);
  };
  // One block (the dispatcher arm pushes what this returns as one block).
  std::string text;
  if (wType == "aw") text = "(sum of wgt is " + format(rawSum, 18, true) + ")\n";
  const double shown = wType == "pw" ? std::trunc(rawSum) : rN;
  text += "(obs=" + (wType == "iw" ? format(shown, 12, false) : format(shown, 18, true)) + ")";
  ojson out;
  out["type"] = "display";
  out["text"] = text;
  return out;
}

}  // namespace

namespace {

// `matrix A[r,c] = exp`: overwrite one cell of an EXISTING matrix with a scalar
// expression, or, when exp is a matrix, the block it covers starting at (r,c)
// (Stata's submatrix substitution). The target must exist (r(111)); a subscript
// or block that falls outside it is Stata's conformability error r(503)
// (oracle w2c_functions.log:151 matassign_oob); a string result is r(109).
//
// `subText` is the whole bracket body, found depth-aware by runMatrixDef, so
// `A[max(1,2),3]` and the 073 corpus's expanded `H[2*3+2+1,4]` arrive intact.
// Each subscript is a scalar expression (evalMatrixScalarExpr, read as StataBE
// reads a target's subscript: a variable is its first observation), rounded to
// the nearest integer, halves up, and range-checked as a double BEFORE any
// cast: NaN, a missing value, or a rounded value < 1 or above the dimension is
// r(503). StataBE 19.5 (w2q_matrix_edges A05, 2026-11-03 oracle): `A[1.7,2]`
// and `A[1.5,1]` write row 2, `A[2.5,1]` row 3, `A[0.4,1]` and, on a 4-row A,
// `A[4.5,1]` are r(503) -- a target rounds where a read (mxElementAt)
// truncates. Row/column NAMES as subscripts are not implemented and refuse
// through the same evaluator. Every refusal carries an exact rc, never rc 1,
// and StataBE's bare message.
ojson runMatrixElementAssign(const std::string& name, const std::string& subText,
                             const std::string& rhs, SessionState& session) {
  eval::EvalMatrix* target = nullptr;
  for (auto& p : session.eval.matrices) {
    if (p.first == name) { target = &p.second; break; }
  }
  if (target == nullptr) return engineErrorBlockRc("matrix " + name + " not found", 111);
  std::vector<std::string> subParts;
  std::vector<char> subOps;
  if (!splitTopLevel(subText, subParts, subOps) || subParts.size() != 2 || subOps[0] != ',' ||
      subParts[0].empty() || subParts[1].empty()) {
    return engineErrorBlockRc("invalid syntax", 198);
  }
  const size_t dims[2] = {target->data.size(),
                          target->data.empty() ? size_t{0} : target->data[0].size()};
  size_t idx[2] = {0, 0};
  for (int k = 0; k < 2; ++k) {
    double v = 0.0;
    std::string subErr;
    if (!evalMatrixScalarExpr(subParts[static_cast<size_t>(k)], session, v, subErr, true))
      return matrixExprErrorBlock(subErr);
    if (std::isnan(v) || v >= eval::kStataMissingSentinel)
      return engineErrorBlockRc("conformability error", 503);
    v = std::floor(v + 0.5);
    if (v < 1.0 || v > static_cast<double>(dims[k]))
      return engineErrorBlockRc("conformability error", 503);
    idx[k] = static_cast<size_t>(v);
  }
  const size_t r = idx[0], c = idx[1];
  // A scalar exp is one cell; a matrix-valued one (`-A`, `2*A[1,1..2]`,
  // `MN["rb",1]`) is Stata's block substitution. A refusal keeps its
  // classified rc (an unknown function r(133)), never rc 1.
  eval::EvalMatrix block;
  {
    std::string rhsErr;
    std::optional<MxValue> value = evalMx(rhs, session, rhsErr);
    if (!value.has_value()) return matrixExprErrorBlock(rhsErr);
    block = mxAsMatrix(*value);
  }
  // Re-find: evaluation does not add matrices today, but the pointer is into
  // a vector the evaluation could in principle grow.
  target = nullptr;
  for (auto& p : session.eval.matrices) {
    if (p.first == name) { target = &p.second; break; }
  }
  if (target == nullptr) return engineErrorBlockRc("matrix " + name + " not found", 111);
  const size_t rows = target->data.size();
  const size_t cols = rows > 0 ? target->data[0].size() : 0;
  const size_t br = block.data.size();
  const size_t bc = br > 0 ? block.data[0].size() : 0;
  if (br == 0 || bc == 0 || r - 1 + br > rows || c - 1 + bc > cols) {
    return engineErrorBlockRc("conformability error", 503);
  }
  for (size_t i = 0; i < br; ++i)
    for (size_t j = 0; j < bc; ++j) target->data[r - 1 + i][c - 1 + j] = block.data[i][j];
  return ojson::array();  // matrix define is silent
}

// `matrix input NAME = (# [,#...] [\ #[,#...] ...])` ([P] matrix define,
// "Inputting matrices by hand"): numbers only, a row's elements separated by
// commas or blanks, the rows by `\`, named r1.. and c1... StataBE 19.5
// (2026-11-03 oracle): an element is a decimal number with an optional sign,
// `.` or `.a`-`.z` (`1+1`, a name, `0x1` are r(198)); a comma before a row's
// first element or after its last is ignored, two with no element between are
// r(198); rows of unequal length are r(503); `()` is a 1x0 matrix; text after
// the closing parenthesis is ignored, and no parentheses is r(198).
ojson runMatrixInput(const std::string& name, const std::string& rhs, SessionState& session) {
  const std::string s = trim(rhs);
  const size_t close = s.find(')');
  if (s.empty() || s[0] != '(' || close == std::string::npos)
    return engineErrorBlockRc("invalid syntax", 198);
  const std::string body = s.substr(1, close - 1);
  static const std::regex reNumber(R"(^[+-]?(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?$)");
  eval::EvalMatrix m;
  size_t from = 0;
  for (size_t at = 0; at <= body.size(); ++at) {
    if (at < body.size() && body[at] != '\\') continue;
    const std::string rowText = body.substr(from, at - from);
    from = at + 1;
    std::vector<double> row;
    bool comma = false;  // a comma since the last element
    size_t i = 0;
    while (i < rowText.size()) {
      const char c = rowText[i];
      if (c == ' ' || c == '\t') {
        ++i;
        continue;
      }
      if (c == ',') {
        if (comma) return engineErrorBlockRc("invalid syntax", 198);
        comma = true;
        ++i;
        continue;
      }
      size_t j = i;
      while (j < rowText.size() && rowText[j] != ' ' && rowText[j] != '\t' && rowText[j] != ',') ++j;
      const std::string tok = rowText.substr(i, j - i);
      if (tok == ".") {
        row.push_back(std::numeric_limits<double>::quiet_NaN());
      } else if (tok.size() == 2 && tok[0] == '.' && tok[1] >= 'a' && tok[1] <= 'z') {
        row.push_back(eval::stataMissingWithKind(tok[1] - 'a' + 1));
      } else if (std::regex_match(tok, reNumber)) {
        try {
          row.push_back(std::stod(tok));
        } catch (const std::exception&) {
          return engineErrorBlockRc("invalid syntax", 198);
        }
      } else {
        return engineErrorBlockRc("invalid syntax", 198);
      }
      comma = false;
      i = j;
    }
    if (!m.data.empty() && row.size() != m.data[0].size())
      return engineErrorBlockRc("conformability error", 503);
    m.data.push_back(std::move(row));
  }
  applyDefaultStripe(m);
  storeMatrixResult(session, name, std::move(m));
  return ojson::array();
}

}  // namespace

// The `matrix` command: e(b)/e(V) capture (the original M3 path below, tried
// FIRST so its output can never shift), `matrix define` with the whole matrix
// expression grammar (evalMx), `matrix input`, element assignment, drop, list,
// rownames/colnames and accum. Everything downstream (`local : colnames b`,
// `b[1,j]`, `scalar = sqrt(V[j,j])`) already resolves against
// session.eval.matrices, so each form here only has to populate/read that one
// store. Any other matrix form is an explicit error, never a silent no-op
// (Doctrine 1): a Stata matrix subcommand this version lacks is named as
// such, and a word that is none is StataBE's r(501).
// The name a matrix is CREATED under follows the one strict rule (strutil.h ->
// core/ident.h). The patterns above take any byte >= 0x80 as a name byte so a
// Unicode name stays whole; this is where `matrix ١ = (1,2)` / `matrix €M = ...`
// are refused, as StataBE refuses them: "١ invalid name", r(198)
// (scratch-r150/uni-d/badnames.log).
static std::optional<ojson> refuseInvalidMatrixName(const std::string& name) {
  if (strutil::isValidStataIdentifier(name)) return std::nullopt;
  return engineErrorBlockRc(name + " invalid name", 198);
}

ojson runMatrixDef(const std::string& raw, SessionState& session) {
  // BUG-543: `matrix define NAME = ...` / `mat def NAME = ...` — `define`/
  // `def` is an OPTIONAL keyword before an assignment target in real Stata
  // (59 corpus lines spell it), but a matrix can be literally NAMED `def` or
  // `define` (`matrix define = ...`), so the keyword is stripped only when a
  // SECOND identifier and `=` follow it — never when `=` sits directly after
  // `def(ine)?` itself. An element target follows it too: `matrix define
  // A[3,3] = 4` is `matrix A[3,3] = 4` (w2q_matrix_edges A10, StataBE rc 0),
  // while `matrix define[1,1] = 5` writes the matrix named define.
  // Unicode matrix names (core/ident.h): eval::kRxName is the name in every
  // pattern below, so `matrix قيم = (1,2)` names one matrix.
  static const std::regex reDefKeyword(std::string(R"(^(\s*mat(?:rix)?\s+)def(?:i(?:n(?:e)?)?)?(\s+)") +
                                       eval::kRxName + R"(\s*[=\[].*)$)");
  std::string work = raw;
  {
    std::smatch km;
    if (std::regex_search(raw, km, reDefKeyword)) work = km.str(1) + km.str(2);
  }
  static const std::regex reMat(std::string(R"(^\s*mat(?:rix)?\s+()") + eval::kRxName +
                                R"()\s*=\s*([er])\(\s*()" + eval::kRxName +
                                R"()\s*\)\s*$)");
  std::smatch m;
  if (!std::regex_search(work, m, reMat)) {
    // Not the e(b)/e(V) form — try the P4 sublanguage before giving up.
    // eb-order T-P1: the matrix is a bare name or a class matrix (`matrix list
    // e(b)` is rc 0 after a fit, w2g_ebmat.log:409 e1_list_eb, :495, :1909,
    // :2020), with Stata's format() option (`matrix list V, format(%21x)`,
    // w2g_ebmat.log:216 e1_list_V).
    static const std::regex reList(std::string(R"(^\s*mat(?:rix)?\s+l(?:i(?:s(?:t)?)?)?\s+()") +
                                   eval::kRxName + R"(|[er]\(\s*)" + eval::kRxName +
                                   R"(\s*\))\s*(?:,\s*(.*?))?\s*$)");
    static const std::regex reStripe(std::string(R"(^\s*mat(?:rix)?\s+(rown|coln)(?:a(?:m(?:e(?:s)?)?)?)?\s+()") +
                                     eval::kRxName + R"()\s*=\s*(.*)$)");
    static const std::regex reAccum(std::string(R"(^\s*mat(?:rix)?\s+ac(?:c(?:u(?:m)?)?)?\s+()") +
                                    eval::kRxName + R"()\s*=\s*(.+)$)");
    static const std::regex reDefine(std::string(R"(^\s*mat(?:rix)?\s+()") + eval::kRxName +
                                     R"()\s*=\s*(.+)$)");
    // ml-getmata T4: element (and block) assignment, `matrix A[r,c] = exp`.
    // Oracle w2c_ml_getmata.do:312-315 fills a J(3,3,.) this way before family
    // (9)'s Mata block, and paper 087 fills all_results cell by cell the same
    // way (gen_sensitivity_stats_1.do). The bracket body is found depth-aware
    // (its matching `]`), so a subscript or right-hand side with its own
    // brackets/commas -- `A[max(1,2),3] = 6`, `H[2,3] = H[3,2]` -- splits at
    // the right place; a bracket with no `= exp` after it is r(198).
    static const std::regex reElemHead(std::string(R"(^\s*mat(?:rix)?\s+()") + eval::kRxName +
                                       R"()\s*\[)");
    std::smatch mm;

    if (std::regex_search(work, mm, reElemHead)) {
      const size_t open = static_cast<size_t>(mm.length(0)) - 1;
      int depth = 0;
      size_t close = std::string::npos;
      for (size_t i = open; i < work.size(); ++i) {
        const char c = work[i];
        if (c == '(' || c == '[') ++depth;
        else if (c == ')' || c == ']') {
          if (--depth == 0) { close = i; break; }
        }
      }
      static const std::regex reAssignTail(R"(^\s*=\s*(.+)$)");
      std::smatch tm;
      const std::string tail = close == std::string::npos ? std::string() : work.substr(close + 1);
      if (close == std::string::npos || !std::regex_match(tail, tm, reAssignTail)) {
        return engineErrorBlockRc("invalid syntax", 198);
      }
      return runMatrixElementAssign(mm.str(1), work.substr(open + 1, close - open - 1),
                                    trim(tm.str(1)), session);
    }

    // `matrix drop NAMES` / `matrix drop _all` ([P] matrix utility). It was not
    // wired at all: every `capture matrix drop b` was a silent rc-1 no-op, so
    // the matrix it meant to remove survived into the next record
    // (w2v_estpost_ematrix.do ep15f: `capture matrix drop b` before `matrix b =
    // e(b)` with no e(); Stata leaves no b, ep15f_b_shape r(111), where PIET
    // still held the previous section's b). A name that is not a matrix is
    // refused at r(111) before anything is dropped -- design decision: that rc
    // is Stata's "matrix not found" code everywhere else (no capture pins this
    // form; the goldens all run it under `capture`).
    static const std::regex reDrop(R"(^\s*mat(?:rix)?\s+drop\s+(.+?)\s*$)");
    if (std::regex_search(work, mm, reDrop)) {
      std::vector<std::string> names;
      {
        std::istringstream iss(mm.str(1));
        for (std::string w; iss >> w;) names.push_back(w);
      }
      const auto silentEcho = []() {
        ojson out;
        out["type"] = "echo";
        out["text"] = "";
        return out;
      };
      auto& store = session.eval.matrices;
      if (names.size() == 1 && names[0] == "_all") {
        store.clear();
        return silentEcho();
      }
      for (const auto& n : names) {
        const bool found = std::any_of(store.begin(), store.end(),
                                       [&n](const auto& p) { return p.first == n; });
        if (!found) return engineErrorBlockRc("matrix " + n + " not found", 111);
      }
      for (const auto& n : names)
        store.erase(std::remove_if(store.begin(), store.end(),
                                   [&n](const auto& p) { return p.first == n; }),
                    store.end());
      return silentEcho();
    }

    if (std::regex_search(work, mm, reList)) {
      std::string name = mm.str(1);
      std::function<std::string(double)> cellText;
      const std::string opts = trim(mm.str(2));
      if (!opts.empty()) {
        // format(%fmt) is the one option this renderer implements; any other
        // is refused by name rather than accepted and ignored.
        static const std::regex reFmtOpt(R"(^f(?:o(?:r(?:m(?:a(?:t)?)?)?)?)?\(\s*(%[^)\s]+)\s*\)$)");
        std::smatch fm;
        if (!std::regex_match(opts, fm, reFmtOpt))
          // Stata's `matrix list` has noblank/nohalf/noheader/nonames/title()
          // ([P] matrix utility); this version renders only format() (the one
          // option any golden uses), so the rest are unsupported, not "not
          // allowed" (eb-order DIAGNOSIS ruling 4d).
          return ojson(eval::unsupportedOption(opts, "", std::nullopt, std::string("matrix list"),
                                               std::string("r(198)")));
        const std::string f = fm.str(1);
        static const std::regex reHex(R"(^%-?21x$)");
        static const std::regex reNum(R"(^%(-)?(0)?([0-9]+)?(?:\.([0-9]+))?([fgeFGE])(c)?$)");
        std::smatch nm;
        if (std::regex_match(f, reHex)) {
          cellText = [](double v) -> std::string {
            return std::isnan(v) ? std::string(".") : SessionState::stataHex21(v);
          };
        } else if (std::regex_match(f, nm, reNum)) {
          fmt::StataFmtSpec sp;
          sp.leftJustify = nm[1].matched;
          sp.zeroFill = nm[2].matched;
          sp.hasWidth = nm[3].matched;
          if (sp.hasWidth) sp.width = std::stoi(nm.str(3));
          sp.prec = nm[4].matched ? std::stoi(nm.str(4)) : 0;
          sp.conv = static_cast<char>(std::tolower(static_cast<unsigned char>(nm.str(5)[0])));
          sp.groupThousands = nm[6].matched;
          cellText = [sp](double v) -> std::string {
            if (auto miss = eval::stataMissingToken(v)) return *miss;
            const auto r = fmt::renderValue(v, sp);
            return r.has_value() ? trim(*r) : formatMatrixCell(v);
          };
        } else {
          return engineErrorBlockRc("matrix list: invalid format " + f, 120);
        }
      }
      std::optional<eval::EvalMatrix> classMat;
      const eval::EvalMatrix* mat = nullptr;
      if (name.find('(') != std::string::npos) {
        const size_t open = name.find('(');
        const char cls = name[0];
        const std::string which = trim(name.substr(open + 1, name.size() - open - 2));
        name = std::string(1, cls) + "(" + which + ")";
        std::string classErr;
        int classRc = 111;
        classMat = resolveClassMatrixRef(cls, which, session, classErr, &classRc);
        // `matrix list e(b)` with no e(b) is r(111) "matrix e(b) not found"
        // whether an e() is live or not (w2v_estpost_ematrix.log ep13_list_eb,
        // ep15f_list_eb; w2g_ebmat.log:2486 e15d_list_eb2) -- not the r(301) of
        // `matrix b = e(b)` in a fresh session.
        if (!classMat.has_value() && classRc == 301)
          return engineErrorBlockRc("matrix " + name + " not found", 111);
        if (!classMat.has_value()) return engineErrorBlockRc(classErr, classRc);
        mat = &*classMat;
      } else {
        for (const auto& p : session.eval.matrices) {
          if (p.first == name) { mat = &p.second; break; }
        }
        // Stata: r(111) "matrix NOSUCH not found" (w2g_ebmat.log:2577
        // s1_matrix_list_nosuch); this was the code-less rc 1.
        if (mat == nullptr) return engineErrorBlockRc("matrix " + name + " not found", 111);
      }
      ojson block;
      block["type"] = "display";  // output, not the command echo (BUG-1505, runEreturnList)
      block["text"] = renderMatrixList(name, *mat, cellText);
      return block;
    }
    if (std::regex_search(work, mm, reStripe)) {
      return runMatrixStripe(mm.str(1) == "rown", mm.str(2), mm.str(3), session);
    }
    if (std::regex_search(work, mm, reAccum)) {
      if (auto bad = refuseInvalidMatrixName(mm.str(1))) return *bad;
      return runMatrixAccum(mm.str(1), mm.str(2), session);
    }
    if (std::regex_search(work, mm, reDefine)) {
      const std::string name = mm.str(1);
      if (auto bad = refuseInvalidMatrixName(name)) return *bad;
      const std::string rhs = trim(mm.str(2));
      // The whole matrix expression grammar (evalMx): literals, joins, the
      // operators, the functions, subscripts, e()/r() matrices, and a scalar
      // expression as a 1x1 (`matrix A = 5`, `matrix B = .`, and `matrix T =
      // r(nosuch)`, an absent r() being `.`: w2f_residuals.log:1675
      // e_matrix_nosuch rc 0, w2n_followups.log:938 E05 rc 0). Every refusal
      // carries its Stata rc (BUG-1398: an unsupported shape was rc 1). An
      // absent nullmat() on its own is r(111) (w2x_fn_string_math
      // fx_nullmat_bare: `matrix NX = nullmat(NOSUCH2)`).
      std::string cerr;
      std::optional<eval::EvalMatrix> built = evalMatrixRhs(rhs, session, cerr);
      if (!built.has_value()) return matrixExprErrorBlock(cerr);
      storeMatrixResult(session, name, std::move(*built));
      return ojson::array();
    }

    static const std::regex reInput(R"(^\s*mat(?:rix)?\s+in(?:p(?:u(?:t)?)?)?\s+([^\s=]+)\s*=\s*(.*)$)");
    if (std::regex_search(work, mm, reInput)) {
      if (auto bad = refuseInvalidMatrixName(mm.str(1))) return *bad;
      return runMatrixInput(mm.str(1), mm.str(2), session);
    }

    // A word that abbreviates one of [P] matrix's subcommands, down to that
    // subcommand's shortest abbreviation (StataBE 19.5, 2026-11-03 oracle:
    // `matrix d` is dir, `matrix de`, `dr`, `ro` and `re` are none). The stripe
    // commands look their matrix up first (`matrix rown Zq` and `matrix rowe
    // Zq` are r(111) `matrix Zq not found`, `matrix rown A` r(198)); `matrix
    // list` with no name is r(100) `matrix name required`; any other served
    // one that reached here is malformed, r(198), as is one this version
    // lacks without its syntax's `=` ([P] matrix: `matrix svd Zq`), `matrix
    // dir` with an argument and `matrix rename` without its two names. One
    // this version lacks is otherwise named as such. Any other word is r(501)
    // `matrix operation not found` (`matrix nosuchsub Q = 1`, `matrix A B`,
    // `matrix`).
    static const std::regex reSubcmd(R"(^\s*mat(?:rix)?(?:\s+([A-Za-z_]\w*))?\s*(.*)$)");
    std::smatch sm;
    std::regex_search(work, sm, reSubcmd);
    const std::string sub = sm[1].matched ? sm.str(1) : std::string();
    const std::string args = trim(sm.str(2));
    struct Subcommand {
      const char* full;
      size_t shortest;
      bool served;
    };
    static const Subcommand kSubcommands[] = {
        {"accum", 2, true},         {"colnames", 4, true},       {"define", 3, true},
        {"drop", 4, true},          {"input", 2, true},          {"list", 1, true},
        {"rownames", 4, true},      {"coleq", 4, false},         {"coljoinbyname", 7, false},
        {"dir", 1, false},          {"dissimilarity", 3, false}, {"eigenvalues", 8, false},
        {"glsaccum", 4, false},     {"opaccum", 7, false},       {"rename", 3, false},
        {"roweq", 4, false},        {"rowjoinbyname", 7, false}, {"score", 3, false},
        {"svd", 3, false},          {"symeigen", 4, false},      {"vecaccum", 4, false}};
    const Subcommand* hit = nullptr;
    for (const auto& s : kSubcommands)
      if (sub.size() >= s.shortest && std::string(s.full).compare(0, sub.size(), sub) == 0) hit = &s;
    if (hit == nullptr) return engineErrorBlockRc("matrix operation not found", 501);
    const std::string full = hit->full;
    const bool stripe =
        full == "rownames" || full == "colnames" || full == "roweq" || full == "coleq";
    if (stripe) {
      const std::string target = args.substr(0, args.find_first_of(" \t="));
      if (!target.empty() && session.eval.findMatrix(target) == nullptr)
        return engineErrorBlockRc("matrix " + target + " not found", 111);
    }
    if (full == "list" && args.empty())
      return engineErrorBlockRc("matrix name required", 100);
    size_t words = 0;
    {
      std::istringstream iss(args);
      for (std::string w; iss >> w;) ++words;
    }
    const bool malformed = hit->served || (full == "dir"      ? words != 0
                                           : full == "rename" ? words != 2
                                                              : args.find('=') == std::string::npos);
    if (malformed) return engineErrorBlockRc("invalid syntax", 198);
    return ojson(eval::unsupportedCommand("matrix " + full, std::nullopt, std::nullopt,
                                           "piet_gap",
                                           eval::UnsupportedCommandKind::kNotImplemented));
  }
  const std::string name = m.str(1);
  if (auto bad = refuseInvalidMatrixName(name)) return *bad;
  const char cls = m.str(2)[0];        // 'e' or 'r'
  const std::string which = m.str(3);  // was "b"/"V" only; now any posted name

  // One resolver (resolveClassMatrixRef), shared with vecdiag()'s argument.
  // An absent name keeps today's refusal text and rc (the absent-r() content
  // is a separate task, FIX-SPEC section 8).
  // eb-order T-P3/T-P1: the refusal carries the resolver's rc through the
  // errors.h builder -- r(301) in a fresh session (w2g_ebmat.log:2229
  // e15c_fresh_matrix_b), r(111) for a name the live e() does not have -- never
  // the code-less rc 1 it had.
  std::string classErr;
  int classRc = 111;
  std::optional<eval::EvalMatrix> resolved =
      resolveClassMatrixRef(cls, which, session, classErr, &classRc);
  // A live e() with no e(b)/e(V) (pca, rddensity, estpost): rc 0 and a 0x0
  // matrix (w2v_estpost_ematrix.log ep13_b_shape / ep15c_b_shape `0|0|0||`,
  // w2g_ebmat.log:2087 e15c_matrix_b), never the fit before it.
  if (!resolved.has_value() && cls == 'e' && session.eval.eMatrixAbsentFromLiveE(which))
    resolved = eval::EvalMatrix{};
  // A posting with no fit behind it -- `ereturn post`, binscatter's own
  // posting -- is an e() PIET holds whole: every name it has is posted here.
  // Its absent e(b)/e(V) is the 0x0 matrix (StataBE 19.5: `ereturn post bb`
  // with no V, then `matrix S5 = e(V)` lists `symmetric S5[0,0]`, rc 0).
  const bool postingOnly = cls == 'e' && session.ePostedLive &&
                           !session.lastModel().has_value() && !session.lastEstBlock.has_value();
  if (!resolved.has_value() && postingOnly && (which == "b" || which == "V") &&
      session.eval.findClassMatrix('e', which) == nullptr)
    resolved = eval::EvalMatrix{};
  // gfx-binscatter: any other e(NAME) that is not a matrix is read as the
  // scalar expression it is, so `matrix M = e(NAME)` is 1 x 1, row r1, column
  // c1: the e() scalar's value, or missing when e() holds no NAME at all.
  // binscatter's line(none)/line(connect) post no e(y1_coefs), and Stata's
  // `matrix M = e(y1_coefs)` is that 1 x 1 missing at rc 0
  // (w2k_binscatter.log:863-864 BS_line_none_c1, :940-941 BS_line_connect_c1).
  // The missing answer needs the live e() to be one whose names PIET knows in
  // full: binscatter's own posting (runBinscatter's `ereturn post` replaces
  // e() whole -- no model, no estimation block, e(graphcmd) -- and posts
  // nothing PIET does not hold; so does `ereturn post`, postingOnly above).
  // After any other fit PIET cannot tell a matrix
  // Stata never posts (e(ilog) after regress) from one this version lacks
  // (e(V_modelbased) after `regress, vce(robust)`, e(gradient) after logit),
  // so a missing value there keeps the r(111) refusal instead of a 1 x 1
  // missing that would be a wrong answer at rc 0. A held, non-missing e()
  // scalar answers its value on every e(). e(b)/e(V) keep their own rules
  // above (r(301) with no e(), 0x0 after a posting without them, r(111)
  // refusals). The value comes from the scalar evaluator
  // (evalMatrixScalarExpr), the same reader as a scalar cell, so a fit's e(N)
  // answers its own N.
  // BUG-1414 (w2q_matrix_edges S05; StataBE 19.5, 2026-11-03 oracle): the
  // 1 x 1 missing is also the answer where PIET does know every e() matrix:
  // with no e() at all (`matrix S = e(nosuch)` in a fresh session or after
  // `ereturn clear`), and after a fit that publishes its whole e(matrices)
  // list (`regress`: b V beta) when NAME is not on it. An e() string macro is
  // r(109) `type mismatch` (`matrix S = e(cmd)` after regress).
  if (!resolved.has_value() && cls == 'e' && which != "b" && which != "V" &&
      session.eval.eStrings.find(which) != nullptr)
    return engineErrorBlockRc("type mismatch", 109);
  if (!resolved.has_value() && cls == 'e' && which != "b" && which != "V") {
    bool notAMatrix = !session.eval.eLive();
    if (!notAMatrix && session.eval.eNameOrder) {
      const std::optional<std::vector<std::string>> mats = session.eval.eNameOrder("matrices");
      notAMatrix = mats.has_value() && std::find(mats->begin(), mats->end(), which) == mats->end();
    }
    double v = 0.0;
    std::string scalarErr;
    if (evalMatrixScalarExpr("e(" + which + ")", session, v, scalarErr) &&
        (postingOnly || notAMatrix || !std::isnan(v))) {
      eval::EvalMatrix one;
      one.data = {{v}};
      applyDefaultStripe(one);
      resolved = std::move(one);
    }
  }
  if (!resolved.has_value()) return engineErrorBlockRc(classErr, classRc);
  storeMatrixResult(session, name, std::move(*resolved));
  return ojson::array();  // matrix define is silent
}

// ---------------------------------------------------------------------------
// label variable / label values

ojson runLabelVariable(const std::string& varname, const std::string& text, SessionState& session) {
  session.setVarLabel(varname, text);
  return ojson::array();  // silent success (Stata-faithful, mirrors label.py)
}

namespace {
// label_define.py's _refresh_value_labels_for_var: rebuild
// session.valueLabels[varname] (int-keyed) from session.labelDefs[lblname]
// (string-keyed) — called after both `label values` and `label define`
// (define refreshes every var currently attached to that lblname).
void refreshValueLabelsForVar(SessionState& session, const std::string& varname,
                              const std::string& lblname) {
  auto defIt = session.labelDefs.find(lblname);
  if (defIt != session.labelDefs.end() && !defIt->second.empty()) {
    std::map<int, std::string> numeric;
    for (const auto& [codeStr, text] : defIt->second) {
      try {
        numeric[std::stoi(codeStr)] = text;
      } catch (const std::exception&) {
      }
    }
    session.valueLabels[varname] = numeric;
  } else {
    session.valueLabels.erase(varname);
  }
}
}  // namespace

namespace {
// LBL-WIDEN: attaching a DEFINED value-label set to a numeric variable widens
// its %[-][0]w.d{g,f,e}[c] display format to the longest text in the set,
// never narrows it, and leaves any other family alone (w2ao_label_family.log
// :369 lb_val_label %8.0g -> %11.0g for "Before 11AM", :687 lb_val_fixed %6.2f
// -> %29.2f, lb_val_long %36.0g -- no cap). An undefined set widens nothing
// (lb_val_undef %8.0g); `nofix` skips it (lb_val_nofix).
// BUG-1136 (FU-1133): a widened format loses its `-` and `0` flags, and one
// already wide enough keeps them (StataBE 19.5, measured 2026-11-03, a
// 17-character label: %-9.0g -> %17.0g, %-12.2f -> %17.2f, %-9.0gc -> %17.0gc,
// %09.2f -> %17.2f; %-20.0g stays %-20.0g).
void widenFormatForSet(SessionState& session, const std::string& varname,
                       const std::string& lblname) {
  auto defIt = session.labelDefs.find(lblname);
  if (defIt == session.labelDefs.end()) return;
  Datastore* ds = session.datastore();
  if (ds == nullptr || !ds->has(varname) || ds->dtype(varname) == "factor") return;
  size_t maxLen = 0;
  for (const auto& kv : defIt->second) {
    size_t n = 0;  // UTF-8 characters
    for (unsigned char c : kv.second)
      if ((c & 0xC0) != 0x80) ++n;
    maxLen = std::max(maxLen, n);
  }
  if (maxLen == 0) return;
  const std::vector<VarInfo> card = ds->formatCard(std::vector<std::string>{varname});
  if (card.empty()) return;
  const std::string fmt = session.displayFormat(card.front());
  static const std::regex reFmt(R"(^%(-?)(0?)(\d+)([.,])(\d+)([gfe])(c?)$)");
  std::smatch m;
  if (!std::regex_match(fmt, m, reFmt)) return;
  const size_t w = static_cast<size_t>(std::stoul(m.str(3)));
  if (maxLen <= w) return;
  session.varMeta[varname].format =
      "%" + std::to_string(maxLen) + m.str(4) + m.str(5) + m.str(6) + m.str(7);
}
}  // namespace

ojson runLabelValues(const std::string& varname, const std::optional<std::string>& lblname,
                     SessionState& session, bool nofix) {
  if (!lblname.has_value() || lblname->empty() || *lblname == ".") {
    // `label values VAR` (no lblname) and `label values VAR .` detach.
    session.varLblname.erase(varname);
    session.valueLabels.erase(varname);
    return ojson::array();
  }
  session.varLblname[varname] = *lblname;
  refreshValueLabelsForVar(session, varname, *lblname);
  if (!nofix) widenFormatForSet(session, varname, *lblname);
  return ojson::array();
}

// ---------------------------------------------------------------------------
// format VARLIST %fmt / format %fmt VARLIST (the dispatcher hands both shapes
// here as varlist + fmt) — BS-005/D6: writes session.varMeta[name].format, the
// field applyDtaMeta() and the .hbin header load (seedVarMetaFromLoadedFile)
// also fill and hbinMetaPatch saves; every reader takes it through
// SessionState::displayFormat. An empty fmt lists the current formats.
// Unknown/malformed %fmt, or a family that does not suit the variable's type,
// is an honest reject, never a silent accept (D6) — `display`'s own separate
// inline `%fmt EXPR` mechanism (display_hardening.cpp's applyStataFmtLocal) is
// untouched; this is the persistent per-variable attribute.

// %fmt validity ([D] format): the numeric, binary, string, date/time (with an
// optional display mask, accepted unvalidated -- %tcHH:MM, %tmMon_CCYY), legacy
// %d and business-calendar families. Anything else is malformed -> r(120).
bool isValidDisplayFormat(const std::string& fmt) {
  // Numeric ([D] format): %[-][0]w.d{f,g,e}[c], with `,` for the European
  // decimal point; d must be less than w ("%9.12f" is not a format).
  static const std::regex reNumeric(R"(^%-?0?(\d{1,3})(?:[.,](\d{1,3}))?[fge]c?$)");
  static const std::regex reBinary(R"(^%(21x|16H|16L|8H|8L)$)");
  static const std::regex reString(R"(^%[-~]?\d{1,4}s$)");
  // %t{c,C,d,w,m,q,h,y,g} with an optional display mask (%tcHH:MM,
  // %tmMon_CCYY, %tdMon-YY -- the corpus shapes), and the legacy %d date
  // format with its own optional mask (6 corpus papers write `format x %d`).
  static const std::regex reDate(R"(^%-?\d{0,2}t[cCdwmqhyg].*$)");
  static const std::regex reLegacyDate(R"(^%-?d.*$)");
  // AER scope-2 B2-3: `%tbNAME[:detail]`, the business-calendar family. Stata
  // accepts it whether or not the calendar EXISTS (measured) -- and so does
  // this, because the format is a display attribute and the calendar is
  // resolved at RENDER time, where a missing file falls back to %g exactly as
  // the manual's item 5 says. But the calendar-NAME rule binds here as hard as
  // it binds `bcal create` and `bcal load`, and the FORMAT plane is the third
  // surface, not an exception (review 2026-09-09; the earlier comment claimed
  // a blanket acceptance the measurement does not support):
  //   `format x %tbabcdefghij`      rc 0    (10 characters is inside the rule)
  //   `format x %tbabcdefghij:day`  rc 0
  //   `format x %tbelevenchars`     r(120)
  //   `format x %tbelevenchars:day` r(120)
  //   `format x %tbab-cd`           r(120)  ·  `format x %tb9abc`  r(120)
  // (StataBE 19.5, /private/tmp/fix-bcal/st/cap.log keys A_*.) The predicate is
  // bcal::isValidCalName — the ONE place the rule lives — not a fourth copy.
  static const std::regex reBcal(R"(^%-?\d{0,2}tb([A-Za-z_][A-Za-z0-9_]*)(?::.*)?$)");
  std::smatch bm;
  if (std::regex_match(fmt, bm, reBcal)) return bcal::isValidCalName(bm[1].str());
  std::smatch nm;
  if (std::regex_match(fmt, nm, reNumeric))
    return !nm[2].matched || std::stoi(nm[2].str()) < std::stoi(nm[1].str());
  return std::regex_match(fmt, reBinary) || std::regex_match(fmt, reString) ||
         std::regex_match(fmt, reDate) || std::regex_match(fmt, reLegacyDate);
}

// A string %fmt (%#s, %-#s, %~#s) suits only a string variable, and every other
// family only a numeric one.
bool isStringDisplayFormat(const std::string& fmt) {
  static const std::regex reString(R"(^%[-~]?\d{1,4}s$)");
  return std::regex_match(fmt, reString);
}

ojson runFormat(const std::vector<std::string>& varlist, const std::string& fmt,
                SessionState& session) {
  Datastore* ds = session.datastore();
  if (ds == nullptr) return eval::noDataLoaded();
  for (const auto& v : varlist) {
    if (!ds->has(v)) return engineErrorBlockRc("variable " + v + " not found", 111);
  }
  if (fmt.empty()) {
    // `format [varlist]` with no %fmt displays the current formats ([D] format:
    // "format varlist lists the formats for the specified variables"; `format`
    // by itself, every variable). Read through the one resolver.
    const std::vector<VarInfo> card = ds->formatCard(
        varlist.empty() ? std::nullopt : std::optional<std::vector<std::string>>(varlist));
    std::string text = "  Variable name   Display format\n";
    for (const auto& v : card) {
      std::string name = v.name;
      if (name.size() < 16) name.resize(16, ' ');
      text += "  " + name + session.displayFormat(v) + "\n";
    }
    ojson out;
    out["type"] = "display";
    out["text"] = text;
    return out;
  }
  if (fmt[0] != '%' || !isValidDisplayFormat(fmt)) {
    // r(120) is Stata's rc for an invalid %fmt, measured for BOTH the general
    // family and the business-calendar name rule (StataBE 19.5, review
    // 2026-09-09: `format x %zz` r(120), `format x %tbelevenchars` r(120) —
    // cap.log keys A_zz_rc / A_eleven_rc). "invalid_format" is not one of the
    // frozen error CODES, so errorBlock() coerced it to "engine_error" and the
    // refusal reached the user as rc 1; `reason` is the only channel that
    // carries an exact rc (BUG-187), so the rc travels there.
    return engineErrorBlockRc("invalid %format: `" + fmt + "'", 120);
  }
  // A string format on a numeric variable, or a numeric/date format on a
  // string variable, is refused before any variable is touched: a half-applied
  // `format` would leave the varlist in a state no Stata run produces.
  const bool strFmt = isStringDisplayFormat(fmt);
  for (const auto& v : varlist) {
    const bool strVar = ds->dtype(v) == "factor";
    if (strFmt && !strVar)
      return engineErrorBlockRc("format " + fmt + " invalid for numeric variable " + v, 120);
    if (!strFmt && strVar)
      return engineErrorBlockRc("format " + fmt + " invalid for string variable " + v, 120);
  }
  for (const auto& v : varlist) {
    session.varMeta[v].format = fmt;
    session.userFormattedVars.insert(v);
  }
  return ojson::array();  // silent success (Stata-faithful, mirrors `label variable`)
}

// ---------------------------------------------------------------------------
// label define / label list / label dir / label drop — C++ port of
// label_define.py (M3b, ROADMAP M3a's own note: "define/list/dir/drop —
// lower corpus frequency — stubbed for M3b").

namespace {

// The one Stata name rule (strutil.h -> core/ident.h): `label define تسمية 1 "one"`
// names a value label, as in Stata 14+; at most 32 characters, not bytes.
bool validStataName(const std::string& name) { return strutil::isValidStataIdentifier(name); }

// _find_unquoted_comma: first top-level comma outside quotes/parens.
long findUnquotedComma(const std::string& s) {
  bool inQuote = false;
  int depth = 0;
  for (size_t i = 0; i < s.size(); ++i) {
    char ch = s[i];
    if (inQuote) {
      if (ch == '"') inQuote = false;
    } else if (ch == '"') {
      inQuote = true;
    } else if (ch == '(' || ch == '[') {
      ++depth;
    } else if (ch == ')' || ch == ']') {
      --depth;
    } else if (ch == ',' && depth == 0) {
      return static_cast<long>(i);
    }
  }
  return -1;
}

// _read_string: quoted (until closing ") or unquoted (until whitespace).
std::pair<std::string, size_t> readString(const std::string& s, size_t pos) {
  if (pos >= s.size()) return {"", pos};
  // EM9: a compound-quoted label `"..."' (nesting; inner " kept) -- `99 `"Don't
  // know"'`, a label holding a quote (w2af_macro_text_file.log :1477, :1509).
  if (eval::startsCompound(s, pos)) {
    const size_t end = eval::compoundSpanEnd(s, pos);
    if (end != std::string::npos) return {s.substr(pos + 2, end - pos - 4), end};
  }
  if (s[pos] == '"') {
    size_t end = s.find('"', pos + 1);
    if (end == std::string::npos) return {s.substr(pos + 1), s.size()};
    return {s.substr(pos + 1, end - (pos + 1)), end + 1};
  }
  size_t end = pos;
  while (end < s.size() && !std::isspace(static_cast<unsigned char>(s[end]))) ++end;
  return {s.substr(pos, end - pos), end};
}

}  // namespace

ojson runLabelDefine(const std::string& rawLine, SessionState& session) {
  std::string raw = trim(rawLine);

  bool addOpt = false, modifyOpt = false, replaceOpt = false, nofixOpt = false;
  long commaIdx = findUnquotedComma(raw);
  if (commaIdx != -1) {
    std::string optStr = lower(trim(raw.substr(static_cast<size_t>(commaIdx) + 1)));
    raw = trim(raw.substr(0, static_cast<size_t>(commaIdx)));
    // Options by WORD, each a prefix of its name (`, a` is add:
    // w2ao_label_family.log lb_add_a rc 0); `nofix` keeps the display formats.
    std::istringstream os(optStr);
    for (std::string w; os >> w;) {
      const auto isPrefix = [&w](const char* full) {
        const std::string f(full);
        return !w.empty() && w.size() <= f.size() && f.compare(0, w.size(), w) == 0;
      };
      if (isPrefix("add")) addOpt = true;
      else if (isPrefix("modify")) modifyOpt = true;
      else if (isPrefix("replace")) replaceOpt = true;
      else if (isPrefix("nofix")) nofixOpt = true;
      else
        return engineErrorBlockRc("option " + w + " not allowed", 198);
    }
    if (modifyOpt) addOpt = false;
  }

  static const std::regex reKw(R"(^label\s+define\s+)", std::regex::icase);
  std::smatch km;
  if (!std::regex_search(raw, km, reKw)) {
    return engineErrorBlock("Expected 'label define LBLNAME ...', got: '" + raw + "'");
  }
  std::string rest = raw.substr(static_cast<size_t>(km.length(0)));

  size_t sp = rest.find_first_of(" \t");
  std::string lblname = (sp == std::string::npos) ? rest : rest.substr(0, sp);
  if (lblname.empty()) return engineErrorBlock("label define: missing label name");
  if (!validStataName(lblname)) {
    return engineErrorBlock("label define: invalid label name '" + lblname + "'");
  }
  std::string body = (sp == std::string::npos) ? std::string() : trim(rest.substr(sp));

  // Codes are integers or the extended missing values .a-.z (lb_def_miss
  // `ny -9 "Refused" .a "Not asked" 1 "One"`, rc 0); a code is stored under its
  // text spelling ("-9", ".a").
  //
  // BUG-1435: an integer code was read with std::stoi, whose out_of_range on
  // `label define L 99999999999 "x"` reached the dispatcher as "stoi: out of
  // range" at rc 1. Stata reads the code as a number and labels it only when it
  // is an integer a `long` can hold, -2,147,483,647 through 2,147,483,620:
  // anything else is r(198) "may not label <the number at %12.0g>" (`1.5`,
  // `2147483647`, `-2147483648`, `99999999999` -> "may not label 1.00000e+11",
  // and `.`), while `1e3`, `1.0`, `+5` and `007` are the codes 1000, 1, 5 and 7.
  // A code that is no number (`abc`, `0x10`), a code with no text, and no pair at
  // all are "invalid syntax" r(198). StataBE 19.5, 2026-10-29.
  std::vector<std::pair<std::string, std::string>> mapping;  // preserves insertion order
  size_t pos = 0;
  while (pos < body.size()) {
    while (pos < body.size() && std::isspace(static_cast<unsigned char>(body[pos]))) ++pos;
    if (pos >= body.size()) break;

    // The code ends at a blank or where its text's quote opens: `1"a" 2"b"` and
    // `1"a"2"b"` are codes 1 and 2 (StataBE 19.5, rc 0).
    size_t tokEnd = pos;
    while (tokEnd < body.size() && !std::isspace(static_cast<unsigned char>(body[tokEnd])) &&
           body[tokEnd] != '"' && body.compare(tokEnd, 2, "`\"") != 0)
      ++tokEnd;
    std::string code = body.substr(pos, tokEnd - pos);
    static const std::regex reExtMissing(R"(^\.[a-z]$)");
    static const std::regex reNumber(R"(^[+-]?(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?$)");
    if (code == ".") return engineErrorBlockRc("may not label .", 198);
    if (!std::regex_match(code, reExtMissing)) {
      if (!std::regex_match(code, reNumber)) return engineErrorBlockRc("invalid syntax", 198);
      const double v = std::strtod(code.c_str(), nullptr);
      // 2147483621 is the long Stata stores `.` as: it is accepted, kept under its
      // number (`: label D 2147483621` is its text, `: label D .` is "."), and
      // `label list` shows it as the code `.` (runLabelList).
      if (!(v == std::floor(v) && v >= -2147483647.0 && v <= 2147483621.0))
        return engineErrorBlockRc("may not label " + fmt::general(v, 12, 0), 198);
      code = std::to_string(static_cast<long long>(v));
    }
    pos = tokEnd;

    while (pos < body.size() && std::isspace(static_cast<unsigned char>(body[pos]))) ++pos;
    if (pos >= body.size()) return engineErrorBlockRc("invalid syntax", 198);
    auto [text, newPos] = readString(body, pos);
    pos = newPos;
    mapping.emplace_back(code, text);
  }

  if (mapping.empty()) return engineErrorBlockRc("invalid syntax", 198);

  auto existingIt = session.labelDefs.find(lblname);
  bool hasOpts = addOpt || modifyOpt || replaceOpt;
  // BUG-475 (measured StataBE 19.5, B4REC|label_define|<redefine_no_replace>|110|,
  // tests/eval/golden/aer_b4/pins_oracle_r.log:57-61): redefining an
  // EXISTING label with none of add/modify/replace is refused verbatim
  // "label <name> already defined" (log:58), which coarseRc's "already
  // defined" rule (eval/errors.cpp) maps to r(110). Must return BEFORE any
  // mutation or the propagation loop below, so the original mapping survives.
  if (existingIt != session.labelDefs.end() && !hasOpts) {
    return engineErrorBlock("label " + lblname + " already defined");
  }
  // A text "" under modify deletes the code (lb_modify_empty: `: label edu_level
  // 2` reads `2` after `label define edu_level 2 "", modify`).
  if (existingIt == session.labelDefs.end() || replaceOpt) {
    std::map<std::string, std::string> replaced;
    for (const auto& [k, v] : mapping) {
      if (v.empty() && modifyOpt) continue;
      replaced[k] = v;
    }
    session.labelDefs[lblname] = replaced;
  } else if (modifyOpt) {
    std::map<std::string, std::string> merged = existingIt->second;
    for (const auto& [k, v] : mapping) {
      if (v.empty()) merged.erase(k);
      else merged[k] = v;
    }
    session.labelDefs[lblname] = merged;
  } else {
    // add: new codes only; an existing code is Stata's r(180) "invalid attempt
    // to modify label" (w2ao_label_family.log:969 lb_add_exist), nothing changed.
    std::map<std::string, std::string> merged = existingIt->second;
    for (const auto& [k, v] : mapping) {
      if (merged.count(k) != 0) return engineErrorBlockRc("invalid attempt to modify label", 180);
      merged[k] = v;
    }
    session.labelDefs[lblname] = merged;
  }
  session.stampLabelSet(lblname);

  // Propagate to valueLabels for every variable currently attached, and widen
  // their display formats unless nofix (lb_modify %8.0g/%3.0g -> %9.0g/%9.0g,
  // log:1035; lb_modify_nofix keeps them).
  for (const auto& [varname, lname] : session.varLblname) {
    if (lname != lblname) continue;
    refreshValueLabelsForVar(session, varname, lblname);
    if (!nofixOpt) widenFormatForSet(session, varname, lblname);
  }
  return ojson::array();  // Stata silent on success
}

// LBL-LISTDIR: `label list` posts r(k) (codes, extended missing included),
// r(min)/r(max) (the integer codes) and r(hasemiss) of the LAST set it lists;
// bare `label list` walks the sets in `label dir` order (newest first); a name
// that is not defined is r(111) "value label X not found" (label's help;
// w2ao_label_family.log lb_list_one, lb_list_all, :1305 lb_list_absent).
ojson runLabelList(const std::vector<std::string>& namesIn, SessionState& session) {
  std::vector<std::string> lblnames = namesIn;
  if (lblnames.empty()) lblnames = session.labelSetOrder();
  // The sets named before the first undefined one are listed, then r(111)
  // (w2_data_io.log:126-133 app3_alab_defined).
  std::vector<std::string> errors;
  {
    std::vector<std::string> present;
    for (const auto& name : lblnames) {
      if (session.labelDefs.find(name) == session.labelDefs.end()) {
        errors.push_back(name);
        break;
      }
      present.push_back(name);
    }
    lblnames = present;
  }
  if (lblnames.empty() && !errors.empty())
    return engineErrorBlockRc("value label " + errors.front() + " not found", 111);
  eval::replaceRClass(session.eval);
  eval::declareRPosted(session.eval, "label list", {"k", "min", "max", "hasemiss"});
  if (lblnames.empty()) {
    ojson block;
    block["type"] = "display";
    block["text"] = "(no value labels defined)";
    return block;
  }

  std::vector<std::string> lines;
  for (const auto& name : lblnames) {
    const auto& defs = session.labelDefs.at(name);
    lines.push_back(name + ":");
    std::vector<std::pair<int, std::string>> sorted;
    std::vector<std::pair<std::string, std::string>> emiss;
    for (const auto& [codeStr, text] : defs) {
      if (!codeStr.empty() && codeStr[0] == '.') {
        emiss.emplace_back(codeStr, text);
        continue;
      }
      // 2147483621, Stata's long for `.`, lists as the code `.` ahead of .a-.z
      // and counts as a missing code in r(min)/r(max)/r(hasemiss) (StataBE 19.5:
      // `label define D 2147483621 "miss" 1 "one" .a "ea"` lists 1 one / . miss /
      // .a ea with r(min) = r(max) = 1, r(hasemiss) = 1).
      if (codeStr == "2147483621") {
        emiss.emplace(emiss.begin(), ".", text);
        continue;
      }
      try {
        sorted.emplace_back(std::stoi(codeStr), text);
      } catch (const std::exception&) {
      }
    }
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [code, text] : sorted) {
      char buf[16];
      std::snprintf(buf, sizeof(buf), "%3d", code);
      lines.push_back("           " + std::string(buf) + "  " + text);
    }
    for (const auto& [code, text] : emiss) {
      char buf[16];
      std::snprintf(buf, sizeof(buf), "%3s", code.c_str());
      lines.push_back("           " + std::string(buf) + "  " + text);
    }
    // r() of this set (the last one listed wins).
    const double nan = std::numeric_limits<double>::quiet_NaN();
    session.eval.rValues.set("k", static_cast<double>(defs.size()));
    session.eval.rValues.set("min", sorted.empty() ? nan : static_cast<double>(sorted.front().first));
    session.eval.rValues.set("max", sorted.empty() ? nan : static_cast<double>(sorted.back().first));
    session.eval.rValues.set("hasemiss", emiss.empty() ? 0.0 : 1.0);
  }

  std::string joined;
  for (size_t i = 0; i < lines.size(); ++i) {
    if (i) joined += "\n";
    joined += lines[i];
  }
  ojson block;
  block["type"] = "display";
  block["text"] = joined;
  if (!errors.empty()) {
    // An undefined name is Stata's r(111) "value label NAME not found", after
    // the sets named before it are listed (w2_data_io.log:126-133
    // app3_alab_defined).
    ojson err = eval::errorBlock("engine_error", "value label " + errors.front() + " not found",
                                 std::nullopt, std::nullopt, std::nullopt, "r(111)");
    if (lines.empty()) return err;
    return ojson::array({block, err});
  }
  return block;
}

// `label dir`: the set names, newest first, and r(names) (label.sthlp; lb_dir
// `zz_lbl keep_lbl negative_lbl grade_drop`).
ojson runLabelDir(SessionState& session) {
  const std::vector<std::string> names = session.labelSetOrder();
  eval::replaceRClass(session.eval);
  std::string joined, spaced;
  for (size_t i = 0; i < names.size(); ++i) {
    if (i) {
      joined += "\n";
      spaced += " ";
    }
    joined += names[i];
    spaced += names[i];
  }
  session.eval.rStrings.set("names", spaced);
  eval::declareRPosted(session.eval, "label dir", {"names"});
  ojson block;
  block["type"] = "display";
  block["text"] = names.empty() ? std::string("(no value labels defined)") : joined;
  return block;
}

ojson runLabelbook(const std::vector<std::string>& namesIn, SessionState& session) {
  std::vector<std::string> lblnames = namesIn;
  if (lblnames.empty()) {
    for (const auto& [name, _] : session.labelDefs) lblnames.push_back(name);
    std::sort(lblnames.begin(), lblnames.end());
  }
  if (lblnames.empty()) {
    ojson block;
    block["type"] = "display";
    block["text"] = "(no value labels defined)";
    return block;
  }

  std::vector<std::string> lines;
  std::vector<std::string> errors;
  bool first = true;
  for (const auto& name : lblnames) {
    auto it = session.labelDefs.find(name);
    if (it == session.labelDefs.end()) {
      errors.push_back(name);
      continue;
    }
    if (!first) lines.push_back("");
    first = false;
    lines.push_back(std::string(88, '-'));
    lines.push_back("Value label " + name);
    lines.push_back(std::string(88, '-'));
    lines.push_back("  Definition");
    std::vector<std::pair<int, std::string>> sorted;
    for (const auto& [codeStr, text] : it->second) {
      try {
        sorted.emplace_back(std::stoi(codeStr), text);
      } catch (const std::exception&) {
      }
    }
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [code, text] : sorted) {
      char buf[16];
      std::snprintf(buf, sizeof(buf), "%6d", code);
      lines.push_back(std::string(buf) + "   " + text);
    }
    // Attached variables: session.varLblname is varname -> lblname (the
    // reverse index of session.labelDefs' lblname -> {code:text}).
    std::vector<std::string> vars;
    for (const auto& [varname, lname] : session.varLblname) {
      if (lname == name) vars.push_back(varname);
    }
    std::sort(vars.begin(), vars.end());
    std::string varsJoined;
    for (size_t i = 0; i < vars.size(); ++i) {
      if (i) varsJoined += " ";
      varsJoined += vars[i];
    }
    lines.push_back("");
    lines.push_back("   Variables:  " + (vars.empty() ? std::string("(none)") : varsJoined));
  }
  if (!errors.empty()) {
    std::string errText;
    for (size_t i = 0; i < errors.size(); ++i) {
      if (i) errText += ", ";
      errText += errors[i];
    }
    lines.push_back("warning: " + errText + ": not defined");
  }

  std::string joined;
  for (size_t i = 0; i < lines.size(); ++i) {
    if (i) joined += "\n";
    joined += lines[i];
  }
  ojson block;
  block["type"] = "display";
  block["text"] = joined;
  return block;
}

// `datasignature` — forward to the engine verb of the same name and show the
// string it returns. The engine prints plain text on stdout (JSON only on
// error), so this uses raw runCobra + errorFromRaw like the other status-line
// verbs rather than runCobraJson.
//
// Stata-SHAPED, not Stata-comparable: see the declaration in handlers.h and
// src/data/datasignature.h's Q9 ruling. This wiring deliberately does NOT try
// to reproduce Stata's checksums — it exposes the engine's own contract-
// satisfying fingerprint. BUG-1461: in the user's form, which `datasignature
// confirm` compares against, as Stata's is: blind to row order and a function
// of each variable's storage type (signatureTypesArg).
namespace {
std::string signatureTypesArg(SessionState& session, const std::vector<std::string>& vars) {
  std::string out;
  Datastore* ds = session.datastore();
  if (ds == nullptr) return out;
  for (const auto& v : vars)
    out += (out.empty() ? "" : ",") + v + "=" + serset::storageType(session, *ds, v);
  return out;
}
}  // namespace

ojson runDatasignature(SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  std::vector<std::string> args = {"datasignature", "--data", *session.currentHbin(), "--unordered",
                                   "--types", signatureTypesArg(session, session.datastore()->names())};
  CobraRunResult cr = runCobra(args);
  if (!cr.ok) {
    CobraError ce = errorFromRaw(cr);
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  std::string sig = cr.stdout_text;
  while (!sig.empty() && (sig.back() == '\n' || sig.back() == '\r' || sig.back() == ' ')) {
    sig.pop_back();
  }
  ojson block;
  block["type"] = "display";
  block["text"] = sig;
  return block;
}

// BUG-1461: `datasignature set|confirm|clear` -- the signature is kept where
// Stata keeps it, in the dataset's characteristics (_dta[datasignature_si] the
// signature, _dta[datasignature_vl1] the variables it covered, _dta[datasignature_dt]
// when it was set), so it travels with save/use and `char list` shows it. confirm
// recomputes over the recorded variables in their recorded order: a dropped or
// changed one is r(9) "data have changed since ...", while an added variable or
// a new order is noted and allowed (r(k_added)) unless `strict`.
// Measured on StataBE 19.5 (2026-11-03): confirm with nothing set is r(459), a
// second set without `reset` is r(110), an unknown subcommand r(198).
namespace {
std::optional<std::string> dtaChar(const SessionState& session, const std::string& name) {
  for (const auto& c : session.characteristics)
    if (c.varname == "_dta" && c.charname == name) return c.value;
  return std::nullopt;
}

void setDtaChar(SessionState& session, const std::string& name, const std::string& value) {
  for (auto& c : session.characteristics)
    if (c.varname == "_dta" && c.charname == name) {
      c.value = value;
      return;
    }
  session.characteristics.push_back({"_dta", name, value});
}

// "03nov2026 17:29", the form Stata's notes print the setting time in.
std::string signatureStamp(const std::string& epochSeconds) {
  std::time_t t = 0;
  try {
    t = static_cast<std::time_t>(std::stoll(epochSeconds));
  } catch (...) {
    return epochSeconds;
  }
  std::tm tm{};
  localtime_r(&t, &tm);
  static const char* kMon[] = {"jan", "feb", "mar", "apr", "may", "jun",
                               "jul", "aug", "sep", "oct", "nov", "dec"};
  char buf[32];
  std::snprintf(buf, sizeof buf, "%02d%s%04d %02d:%02d", tm.tm_mday, kMon[tm.tm_mon],
                tm.tm_year + 1900, tm.tm_hour, tm.tm_min);
  return buf;
}

// `<rows>:<k>:<a>:<b>` of the engine's per-variable signature, and the same
// fields read out of a full `<rows>:<k>(<meta>):<a>:<b>` one: over the same
// variables in the same order the two agree field for field.
std::string valuesPart(const std::string& sig) {
  const size_t open = sig.find('(');
  const size_t close = sig.find(')');
  if (open == std::string::npos || close == std::string::npos || close < open) return sig;
  return sig.substr(0, open) + sig.substr(close + 1);
}
}  // namespace

ojson runDatasignatureSub(const std::string& sub,
                          const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                          SessionState& session) {
  bool reset = false, strict = false;
  for (const auto& [k, v] : opts) {
    if (k == "reset") reset = true;
    if (k == "strict") strict = true;
  }
  if (sub == "clear") {
    auto& cs = session.characteristics;
    cs.erase(std::remove_if(cs.begin(), cs.end(),
                            [](const DtaCharacteristic& c) {
                              return c.varname == "_dta" &&
                                     c.charname.rfind("datasignature_", 0) == 0;
                            }),
             cs.end());
    ojson b;
    b["type"] = "display";
    b["text"] = "  (data signature cleared)";
    return b;
  }
  if (sub == "set") {
    const bool wasSet = dtaChar(session, "datasignature_si").has_value();
    if (wasSet && !reset)
      return engineErrorBlockRc("data signature already set -- specify option reset", 110);
    ojson sigBlk = runDatasignature(session);
    if (sigBlk.value("type", std::string()) != "display") return sigBlk;
    const std::string sig = sigBlk.value("text", std::string());
    std::string vl;
    for (const auto& n : session.datastore()->names()) vl += (vl.empty() ? "" : " ") + n;
    setDtaChar(session, "datasignature_dt", std::to_string(static_cast<long long>(std::time(nullptr))));
    setDtaChar(session, "datasignature_vl1", vl);
    setDtaChar(session, "datasignature_si", sig);
    session.eval.rStrings.set("datasignature", sig);
    ojson b;
    b["type"] = "display";
    // "reset" only when `reset` replaced one (StataBE 19.5).
    b["text"] = "  " + sig + "       (data signature " + (wasSet ? "reset" : "set") + ")";
    return b;
  }
  // confirm
  const auto si = dtaChar(session, "datasignature_si");
  if (!si.has_value())
    return engineErrorBlockRc("  no data signature was previously set", 459);
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  const std::string since = signatureStamp(dtaChar(session, "datasignature_dt").value_or(""));
  std::vector<std::string> recorded;
  {
    std::istringstream is(dtaChar(session, "datasignature_vl1").value_or(""));
    for (std::string w; is >> w;) recorded.push_back(w);
  }
  const std::vector<std::string>& now = session.datastore()->names();
  bool same = true;
  for (const auto& v : recorded)
    if (std::find(now.begin(), now.end(), v) == now.end()) same = false;
  if (same) {
    std::string csv;
    for (const auto& v : recorded) csv += (csv.empty() ? "" : ",") + v;
    CobraRunResult cr = runCobra({"datasignature", "--data", *session.currentHbin(), "--vars", csv,
                                  "--unordered", "--types", signatureTypesArg(session, recorded)});
    if (!cr.ok) {
      CobraError ce = errorFromRaw(cr);
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    std::string got = cr.stdout_text;
    while (!got.empty() && (got.back() == '\n' || got.back() == '\r' || got.back() == ' '))
      got.pop_back();
    same = got == valuesPart(*si);
  }
  if (!same) return engineErrorBlockRc("  data have changed since " + since, 9);
  // Unchanged values; the note names an addition before a reordering, and
  // `strict` turns either into r(9) with the same note and nothing posted.
  const size_t added = now.size() - recorded.size();
  std::vector<std::string> kept;
  for (const auto& v : now)
    if (std::find(recorded.begin(), recorded.end(), v) != recorded.end()) kept.push_back(v);
  std::string note = "  (data unchanged since " + since;
  if (added == 1)
    note += ", except 1 variable has been added";
  else if (added > 1)
    note += ", except " + std::to_string(added) + " variables have been added";
  else if (kept != recorded)
    note += ", but order of variables has changed";
  note += ")";
  if (strict && (added > 0 || kept != recorded)) return engineErrorBlockRc(note, 9);
  session.eval.rValues.set("k_added", static_cast<double>(added));
  session.eval.rStrings.set("datasignature", *si);
  ojson b;
  b["type"] = "display";
  b["text"] = note;
  return b;
}

// LBL-LANGUAGE ([D] label language; w2ao_label_family.log lb_lang_*): languages
// keep their own variable labels and value-label attachments. `new` starts the
// new language empty (`copy`: as a copy of the current one) and makes it
// current; naming a language switches to it; `delete` removes one (the current
// one hands over to the first remaining language: lb_lang_delete); an unknown
// name is r(111) "language X not defined". The bare form posts r(k),
// r(language) and r(languages) (lb_lang_bare `k=1|default|default`).
namespace {
SessionState::LabelLangStore snapshotLabelLang(SessionState& session) {
  SessionState::LabelLangStore st;
  st.dataLabel = session.dataLabel;
  if (Datastore* ds = session.datastore()) {
    for (const VarInfo& vi : session.varlistWithMeta(ds))
      if (!vi.label.empty()) st.varLabels[vi.name] = vi.label;
  }
  st.varLblname = session.varLblname;
  return st;
}
void loadLabelLang(SessionState& session, const SessionState::LabelLangStore& st) {
  session.dataLabel = st.dataLabel;
  if (Datastore* ds = session.datastore()) {
    for (const auto& name : ds->names()) {
      auto it = st.varLabels.find(name);
      session.setVarLabel(name, it == st.varLabels.end() ? std::string() : it->second);
    }
  }
  session.varLblname = st.varLblname;
  session.valueLabels.clear();
  for (const auto& [var, lbl] : session.varLblname) refreshValueLabelsForVar(session, var, lbl);
}
}  // namespace

ojson runLabelLanguage(const std::vector<std::string>& args,
                       const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                       SessionState& session) {
  bool optNew = false, optCopy = false, optDelete = false, optRename = false;
  for (const auto& [k, v] : opts) {
    (void)v;
    if (k == "new") optNew = true;
    else if (k == "copy") optCopy = true;
    else if (k == "delete") optDelete = true;
    else if (k == "rename") optRename = true;
    else return engineErrorBlockRc("option " + k + " not allowed", 198);
  }
  auto& langs = session.labelLangs;
  auto known = [&langs](const std::string& n) {
    return std::find(langs.begin(), langs.end(), n) != langs.end();
  };
  if (args.empty()) {
    eval::replaceRClass(session.eval);
    std::string all;
    for (size_t i = 0; i < langs.size(); ++i) all += (i ? " " : "") + langs[i];
    session.eval.rValues.set("k", static_cast<double>(langs.size()));
    session.eval.rStrings.set("language", session.labelLang);
    session.eval.rStrings.set("languages", all);
    eval::declareRPosted(session.eval, "label language", {"k", "language", "languages"});
    ojson d;
    d["type"] = "display";
    if (langs.size() == 1) {
      d["text"] =
          "Language for variable and value labels\n\n"
          "    In this dataset, value and variable labels have been defined in only one "
          "language:  " + session.labelLang + "\n\n"
          "    To create new language:         . label language <name>, new\n"
          "    To rename current language:     . label language <name>, rename";
    } else {
      std::string text = "Language for variable and value labels\n\n    Available languages:\n";
      for (const auto& l : langs) text += "            " + l + "\n";
      text += "\n    Currently set as:               . label language " + session.labelLang;
      d["text"] = text;
    }
    return d;
  }
  if (args.size() != 1) return engineErrorBlockRc("invalid syntax", 198);
  const std::string& name = args[0];
  if (!validStataName(name)) return engineErrorBlockRc(name + " invalid name", 198);
  if (optDelete) {
    if (!known(name)) return engineErrorBlockRc("language " + name + " not defined", 111);
    if (langs.size() == 1)
      return engineErrorBlockRc("language " + name + " is the only language; it cannot be deleted",
                                198);
    if (name == session.labelLang) {
      const std::string next = langs[0] == name ? langs[1] : langs[0];
      auto it = session.labelLangStash.find(next);
      loadLabelLang(session, it == session.labelLangStash.end() ? SessionState::LabelLangStore{}
                                                                 : it->second);
      session.labelLangStash.erase(next);
      session.labelLang = next;
    }
    session.labelLangStash.erase(name);
    langs.erase(std::find(langs.begin(), langs.end(), name));
    return ojson::array();
  }
  if (optRename) {
    if (known(name)) return engineErrorBlockRc("language " + name + " already defined", 110);
    *std::find(langs.begin(), langs.end(), session.labelLang) = name;
    session.labelLang = name;
    return ojson::array();
  }
  if (optNew) {
    if (known(name)) return engineErrorBlockRc("language " + name + " already defined", 110);
    session.labelLangStash[session.labelLang] = snapshotLabelLang(session);
    if (!optCopy) loadLabelLang(session, SessionState::LabelLangStore{});
    langs.push_back(name);
    session.labelLang = name;
    ojson d;
    d["type"] = "display";
    d["text"] = "(language " + name + " now current language)";
    return d;
  }
  if (name == session.labelLang) {
    ojson d;
    d["type"] = "display";
    d["text"] = "(" + name + " already current language)";
    return d;
  }
  if (!known(name)) return engineErrorBlockRc("language " + name + " not defined", 111);
  session.labelLangStash[session.labelLang] = snapshotLabelLang(session);
  auto it = session.labelLangStash.find(name);
  loadLabelLang(session, it == session.labelLangStash.end() ? SessionState::LabelLangStore{}
                                                             : it->second);
  session.labelLangStash.erase(name);
  session.labelLang = name;
  return ojson::array();
}

// `label drop`: removes the DEFINITION; a variable keeps the attached set's name
// (w2ao_label_family.log lb_drop: `: value label g1` is still grade_drop and
// decode is r(111)). `_all` drops every set; a name that is not defined is
// r(111) "value label X not found" and nothing is dropped (:1373 lb_drop_absent).
ojson runLabelDrop(const std::vector<std::string>& lblnamesIn, SessionState& session) {
  if (lblnamesIn.empty()) {
    return engineErrorBlock("label drop: specify at least one label name");
  }
  std::vector<std::string> lblnames;
  for (const auto& name : lblnamesIn) {
    if (name == "_all") {
      for (const auto& kv : session.labelDefs) lblnames.push_back(kv.first);
      continue;
    }
    if (session.labelDefs.find(name) == session.labelDefs.end())
      return engineErrorBlockRc("value label " + name + " not found", 111);
    lblnames.push_back(name);
  }
  for (const auto& name : lblnames) {
    session.labelDefs.erase(name);
    session.labelSeq.erase(name);
    for (const auto& [varname, lname] : session.varLblname)
      if (lname == name) session.valueLabels.erase(varname);
  }
  return ojson::array();  // Stata silent on success
}

// ---------------------------------------------------------------------------
// preserve / restore — LIFO hbin-file-copy + display-metadata snapshot
// stack (preserve_restore.py's run_preserve/run_restore, ported exactly:
// both return a real data_mutation block with a `preserve_depth` field, NOT
// silent — unlike tempfile/tempname/label).

namespace {
// B5-D14 (BUG-454): the snapshot half of runPreserve, factored out so
// runRestore's `, preserve` re-arm can call it on the JUST-RESTORED state
// instead of trying to push the frame it popped back onto the stack (which
// may be invalid: MAXPERF Stage 2's unstash can CONSUME the resident stash,
// and the scratch-reclaim exemption below is keyed on live preserveStack
// membership, so a pop-then-repush of the SAME frame would open a reclaim
// window). Re-snapshotting from a known-good, already-restored state avoids
// both hazards. Returns an error block on snapshot failure, std::nullopt on
// success (frame pushed).
//
// BUG-922: `level` is the preserve level that owns the copy: the level of the
// `preserve`, or the popped frame's own level on a `restore, preserve` re-arm.
// This is the only writer of PreserveFrame::level (session_state.h I1).
std::optional<ojson> pushPreserveSnapshot(SessionState& session, int level) {
  PreserveFrame frame;
  frame.noData = !session.currentHbin().has_value();  // BUG-1139: nothing to snapshot
  // MAXPERF Stage 2 (item 1): when the resident plane is active, snapshot the
  // worker's resident data-state via `stash` — an O(columns) RAM capture with
  // NO disk write and NO full-dataset byte-copy. residentPreserve() also keeps
  // the crash-recovery floor (it materializes a dirty virtual head first, which
  // is a no-op on the common clean/real head). The clonefile plane REMAINS the
  // fallback when the resident plane is unavailable (§26-only engine / serve
  // disabled / worker down) — Q1 (gate ③) clone-first (APFS CoW) with the
  // pre-existing byte-copy as the universal fallback (see cloneOrCopyFile).
  if (frame.noData) {
    // No snapshot: the metadata below is all there is to keep.
  } else if (std::optional<std::string> stashName = session.residentPreserve()) {
    frame.stashName = *stashName;
    // Record the preserve-time head file alongside the stash: residentPreserve()
    // materialized the head just before stashing, so this on-disk file is
    // byte-identical to the stash by construction. restore re-adopts it as a
    // CLEAN head (zero-write card build) instead of re-materializing the frame
    // via a dirty virtual token; it also doubles as a recovery source if the
    // worker (and its RAM stash) dies before restore. While this frame is live
    // the file is exempt from scratch reclaim (reclaimSupersededScratch checks
    // preserveStack), so no extra copy or pin is needed.
    frame.hbinPath = *session.currentHbin();
    if (piet::SessionState::scratchTraceOn()) {
      std::error_code tec;
      piet::SessionState::scratchTrace(
          std::filesystem::exists(frame.hbinPath, tec) ? "PRESERVE(twin-real)"
                                                       : "PRESERVE(twin-MISSING)",
          "resident", frame.hbinPath);
    }
  } else {
    std::string snapPath = nextPath(session.workdir(), "preserve");
    // Fallback plane: a virtual resident head must be snapshotted to disk first
    // (cloneOrCopyFile bypasses datastore()). restore() below hands the snapshot
    // file straight to session.load() (zero-copy) on this plane.
    session.ensureMaterializedHead();
    if (!cloneOrCopyFile(*session.currentHbin(), snapPath)) {
      // BUG-124 sibling: fail the preserve HERE, honestly — the old void call
      // pushed a torn (or missing) snapshot onto the stack for restore to load.
      return ojson(engineErrorBlock(
          "preserve: could not snapshot the dataset (disk full or I/O error)"));
    }
    frame.hbinPath = snapPath;
  }
  frame.varMeta = session.varMeta;
  frame.implicitFloatVars = session.implicitFloatVars;
  frame.labelDefs = session.labelDefs;
  frame.varLblname = session.varLblname;
  frame.valueLabels = session.valueLabels;
  frame.dataLabel = session.dataLabel;
  frame.characteristics = session.characteristics;
  frame.level = level;
  session.preserveStack.push_back(std::move(frame));
  return std::nullopt;
}

// BUG-1139: preserve/restore's block when the dataset is the empty one.
ojson emptyDatasetResult(SessionState& session, const std::string& action) {
  ojson out;
  out["type"] = "data_mutation";
  out["action"] = action;
  out["preserve_depth"] = static_cast<int64_t>(session.preserveStack.size());
  out["varlist_after"] = ojson::array();
  out["n_rows_after"] = 0;
  return out;
}
}  // namespace

ojson runPreserve(SessionState& session) {
  // BUG-922: a second `preserve` while this program or do-file level still
  // holds its copy is r(621) "already preserved", and the first copy is kept
  // (oracle w2_prog_controlflow pc_pr_twice 621, then pc_pr_twice_restored
  // brings back the FIRST copy and pc_pr_twice_none_left is r(622)). It used to
  // stack a second copy at rc 0. A program, or a nested do/run file, started
  // while its caller holds a copy may preserve its own (a deeper level;
  // w2ad_prog_preserve_nest pn_d_nested_pres, pn_p_nested_pres), and an
  // included file shares its includer's level (pn_i_pres_held: 621).
  if (session.ownPreserveFrame()) {
    return engineErrorBlockRc("already preserved", 621);
  }
  if (std::optional<ojson> err = pushPreserveSnapshot(session, session.preserveLevel())) {
    return *err;
  }
  if (session.preserveStack.back().noData) return emptyDatasetResult(session, "preserve");

  // P2-PERF ARC-B/B3: render the unchanged varlist from the resident card, the
  // exact transition runUse already made (handlers.cpp). `preserve` does not
  // change the data at all — it only stacks a copy — so calling datastore()
  // here was asking for a full open of the head purely to re-print what the
  // session already knows, and on a dirty virtual head it forced a snapshot
  // besides. Honest fallback to the old path whenever the card cannot vouch
  // for itself.
  //
  // This rung was reverted once, in session 1, because the card could not
  // answer `format` and this render printed %9.0g where the pins said %9s.
  // That was BUG-190 and it is fixed at the root now (session_state.cpp
  // seedShadowFromFrameVars), which is what lets the rung return.
  if (const std::vector<VarInfo>* card = session.residentReadCard()) {
    if (std::optional<int64_t> cardRows = session.residentRowsIfCurrent()) {
      ojson out;
      out["type"] = "data_mutation";
      out["action"] = "preserve";
      out["preserve_depth"] = static_cast<int64_t>(session.preserveStack.size());
      ojson varlistAfter = ojson::array();
      auto vlist = session.varlistWithMetaFrom(*card);
      for (const auto& v : vlist) varlistAfter.push_back(v.toJson());
      session.updateNMissCache(vlist);
      out["varlist_after"] = varlistAfter;
      out["n_rows_after"] = *cardRows;
      return out;
    }
  }
  Datastore* ds = session.datastore();
  ojson out;
  out["type"] = "data_mutation";
  out["action"] = "preserve";
  out["preserve_depth"] = static_cast<int64_t>(session.preserveStack.size());
  ojson varlistAfter = ojson::array();
  auto vlist = session.varlistWithMeta(ds);
  for (const auto& v : vlist) varlistAfter.push_back(v.toJson());
  session.updateNMissCache(vlist);  // C2-F13a: keep the cache current for the next mutation
  out["varlist_after"] = varlistAfter;
  out["n_rows_after"] = ds ? ds->nRows() : 0;
  return out;
}

namespace {
// BUG-922: restore (and with `rearm` re-arm) the top preserved copy, whoever
// owns it. Two callers only: runRestore, after its reach test, and the
// unwinder program::restorePreservesDeeperThan below, which restores the
// copies of a level that has already concluded.
ojson restoreTopFrame(SessionState& session, bool rearm) {
  PreserveFrame frame = std::move(session.preserveStack.back());
  session.preserveStack.pop_back();
  if (piet::SessionState::scratchTraceOn()) {
    std::error_code tec;
    piet::SessionState::scratchTrace(
        std::filesystem::exists(frame.hbinPath, tec) ? "RESTORE(twin-real)"
                                                     : "RESTORE(twin-MISSING)",
        frame.stashName.empty() ? "clone" : "stash", frame.hbinPath);
  }

  // MAXPERF Stage 2 (item 1): a stash-backed preserve restores via `unstash`
  // (O(columns) RAM, no disk read); a clone-backed preserve (fallback plane)
  // reloads its file. The restored data-state is byte-identical on both planes
  // (§27 parity law), so the rendered card below is identical either way.
  bool restored = false;
  const bool stashBacked = !frame.stashName.empty();
  if (frame.noData) {
    session.clearData();  // BUG-1139: back to the empty dataset that was preserved
    restored = true;
  } else if (stashBacked) {
    restored = session.residentRestore(frame.stashName, frame.hbinPath);
  }
  if (!restored) {
    // Stash-backed and the unstash failed (worker restarted — the RAM snapshot
    // is gone): the preserve-time head file recorded alongside the stash is
    // byte-identical to the snapshot by construction, so load() it below as a
    // full recovery when it still exists. Only when no on-disk twin survives
    // either: honest error, never silent stale data. Clone-backed frames
    // (fallback plane) take the load() path exactly as before.
    std::error_code restoreEc;
    if (frame.hbinPath.empty() ||
        (stashBacked && !std::filesystem::exists(cecon::to_native_path(frame.hbinPath),
                                                 restoreEc))) {
      return engineErrorBlock(
          "restore: the preserved snapshot was held in the compute worker and is no longer "
          "available (the worker restarted). Re-run from your last saved dataset.");
    }
    session.load(frame.hbinPath);
  }
  session.varMeta = frame.varMeta;
  // An untyped variable is float again after restore, whatever the preserved
  // block did to its claim: `gen x = 1.5`, preserve, `drop x`, restore, then
  // `replace x = 1/3` stores the float +1.5555560000000X-002 in StataBE 19.5,
  // where the lost claim fell back to double (and fcollapse ..., append then
  // left a byte claim on 2.5 and 40000, BUG-1100).
  session.implicitFloatVars = frame.implicitFloatVars;
  session.labelDefs = frame.labelDefs;
  session.varLblname = frame.varLblname;
  session.valueLabels = frame.valueLabels;
  session.dataLabel = frame.dataLabel;
  session.characteristics = frame.characteristics;
  // xtset-file-persistence: the preserved char table carries the declaration
  // that was live at `preserve`; restoring the one restores the other.
  session.adoptTsFromChars();

  if (rearm) {
    // B5-D14 (BUG-454, measured B5REC|restore|preserve_after_preserve|0| +
    // B5VAL|zz_exists_after|0| i.e. a following `gen` succeeds, tests/eval/
    // golden/aer_b4/b5_misc_pins.log:97-103): `restore, preserve` restores
    // AND keeps the preserved copy, so a do-file can restore repeatedly from
    // one snapshot. Re-snapshot from the state JUST restored above (not the
    // popped `frame`) -- see pushPreserveSnapshot's comment for why reusing
    // the popped frame is unsafe. BUG-922: the copy keeps the level that
    // owned it (session_state.h I1).
    if (std::optional<ojson> err = pushPreserveSnapshot(session, frame.level)) {
      return *err;
    }
  }
  if (frame.noData) return emptyDatasetResult(session, "restore");

  // P2-PERF ARC-B/B3, the other end of the same window (see runPreserve above).
  if (const std::vector<VarInfo>* card = session.residentReadCard()) {
    if (std::optional<int64_t> cardRows = session.residentRowsIfCurrent()) {
      ojson out;
      out["type"] = "data_mutation";
      out["action"] = "restore";
      out["preserve_depth"] = static_cast<int64_t>(session.preserveStack.size());
      ojson varlistAfter = ojson::array();
      auto vlist = session.varlistWithMetaFrom(*card);
      for (const auto& v : vlist) varlistAfter.push_back(v.toJson());
      session.updateNMissCache(vlist);
      out["varlist_after"] = varlistAfter;
      out["n_rows_after"] = *cardRows;
      return out;
    }
  }
  Datastore* ds = session.datastore();
  ojson out;
  out["type"] = "data_mutation";
  out["action"] = "restore";
  out["preserve_depth"] = static_cast<int64_t>(session.preserveStack.size());
  ojson varlistAfter = ojson::array();
  auto vlist = session.varlistWithMeta(ds);
  for (const auto& v : vlist) varlistAfter.push_back(v.toJson());
  session.updateNMissCache(vlist);  // C2-F13a: keep the cache current for the next mutation
  out["varlist_after"] = varlistAfter;
  out["n_rows_after"] = ds ? ds->nRows() : 0;
  return out;
}
}  // namespace

ojson runRestore(SessionState& session, bool rearm) {
  if (!session.ownPreserveFrame()) {
    // B5-D14 (BUG-454, measured B5REC|restore|preserve_nopreserve|622|,
    // tests/eval/golden/aer_b4/b5_misc_pins.log:90-91): `restore, preserve`
    // on an EMPTY stack is r(622), not the generic "1". BUG-922: so is bare
    // `restore` (oracle w2_prog_controlflow pc_pr_none and
    // pc_pr_not_then_restore: 622). The piet2 cell that pinned rc 1 here
    // (test_preserve_restore.jsonl:12) belongs to the corpus archived by
    // BUG-400c, which no gate reads.
    //
    // BUG-922: and so is a restore from a level that preserved nothing while
    // its caller holds a copy: the caller's copy is out of reach and stays
    // where it is (oracle w2ad_prog_preserve_nest pn_d_restore_caller,
    // pn_p_restore_caller, pn_d_rp_caller, pn_r_rp_caller, pn_p_rp_caller:
    // in1 622:4:30, then the caller's own restore brings its copy back).
    return engineErrorBlockRc(
        "restore: no previous preserve — use `preserve` before `restore`", 622);
  }
  return restoreTopFrame(session, rearm);
}

}  // namespace piet::handlers

namespace piet::program {

// BUG-922: the one unwinder (session_state.h I3). It lives beside runRestore
// because it must restore copies that are no longer in the reach of the level
// now running: the level that owned them has concluded.
void restorePreservesDeeperThan(int callerLevel, SessionState& session, ojson& blocks) {
  while (!session.preserveStack.empty() && session.preserveStack.back().level > callerLevel) {
    ojson rb = handlers::restoreTopFrame(session, false);
    if (rb.is_object() && rb.value("type", std::string()) == "error") {
      blocks.push_back(std::move(rb));
      break;
    }
  }
}

}  // namespace piet::program

namespace piet::handlers {

// ---------------------------------------------------------------------------
// tempfile / tempname — S5/U4. Silent on success (empty array).

ojson runTempfile(const std::vector<std::string>& names, SessionState& session) {
  if (!session.runTempdir.has_value()) {
    std::string dir = session.workdir() + "/tmp_" + std::to_string(std::time(nullptr));
    // BUG-111(a): create the dir via the portable API on BOTH platforms. The old
    // POSIX path shelled out `std::system("mkdir -p '<dir>'")` and DISCARDED the
    // return code — a workdir/TMPDIR containing an apostrophe (macOS volume names
    // like `Nam's MacBook`) broke the single-quoting, mkdir silently failed, and
    // every subsequent tempfile save then errored cryptically. create_directories
    // handles any path; surface an honest error on failure instead of swallowing it.
    std::error_code mkec;
    std::filesystem::create_directories(cecon::to_native_path(dir), mkec);
    if (mkec)
      return ojson(eval::engineError("tempfile: could not create scratch directory '" + dir +
                                     "': " + mkec.message()));
    session.runTempdir = dir;
  }
  for (const auto& name : names) {
    // BUG-111(b): per-CALL unique filename (Stata mints a fresh file each call) —
    // the macro `name` binds to this path, so scripts using `\`name'` are unchanged.
    std::string path = *session.runTempdir + "/" + name + "_" +
                       std::to_string(session.tempSeq++) + ".tmp";
    // BUG-360: record WHO minted it. A tempfile's lifetime in Stata is the
    // program or do-file that created it, not the run — so the returning
    // program needs to know which of these are its own (program.cpp's
    // ScopeGuard). Level 0 = the top-level run, still reaped by doexec's
    // TempdirReapGuard at the end of the run, which is the same thing Stata
    // does for a do-file-level tempfile. BUG-922: tagged with the preserve
    // level, so a nested do/run file's tempfiles die when it concludes too
    // (program::concludeLevel).
    session.mintedTempfiles.emplace_back(session.preserveLevel(), path);
    session.eval.locals.set(name, path);
  }
  return ojson::array();
}

ojson runTempname(const std::vector<std::string>& names, SessionState& session) {
  for (const auto& name : names) {
    // BUG-111(b): epoch-SECONDS collided for two `tempname` calls in the same second;
    // use the per-session monotonic counter so each call's basename is unique.
    std::string basename = "__" + name + "_" + std::to_string(session.tempSeq++);
    session.eval.locals.set(name, basename);
    // BUG-922: dropped (scalar or matrix of this name) when the minting
    // program or nested do/run file concludes -- program::concludeLevel
    // (w2ad_prog_preserve_nest pn_tv_do_gone: scalar=111).
    session.mintedTempnames.emplace_back(session.preserveLevel(), basename);
  }
  return ojson::array();
}

// BS-037: tempvar -- sibling of tempfile/tempname above, previously ZERO
// implementation (the raw token "tempvar" was still listed in the
// tokenizer's early macro-statement keyword net, eval/tokenizer.cpp:80,
// so `tempvar tv` tripped the generic "does not expand macros" tripwire
// before ever reaching dispatch — see dispatcher.cpp's U4 block, which now
// intercepts tempfile/tempname/tempvar identically, BEFORE tokenize() ever
// sees the line, same as it already did for tempfile/tempname). Real
// Stata: mints a scratch-safe NEW variable name (the column itself is only
// created later by e.g. `gen `tv''`) and auto-drops it at scope end — since
// BUG-922 a program or nested do/run file drops its own tempvars when it
// concludes (mintedTempvars); a top-level tempvar still outlives the run.
// Unlike tempname's arbitrary "__NAME_TIMESTAMP" basename (fine for a FILE
// path only, never validated as an identifier), a tempvar's minted name
// must be a LEGAL Stata variable name AND must not collide with any column
// already in the current dataset — real Stata's own pattern is
// `__000001`, `__000002`, ...; mirrored here via a per-process monotonic
// counter, skipping forward past any collision against the loaded dataset
// (if any — tempvar is legal even before `use`/`sysuse`).
ojson runTempvar(const std::vector<std::string>& names, SessionState& session) {
  static std::atomic<uint64_t> counter{0};
  Datastore* ds = session.datastore();
  for (const auto& name : names) {
    std::string minted;
    for (;;) {
      uint64_t n = counter.fetch_add(1, std::memory_order_relaxed) + 1;
      char buf[16];
      std::snprintf(buf, sizeof(buf), "__%06llu", static_cast<unsigned long long>(n));
      minted = buf;
      if (ds == nullptr || !ds->has(minted)) break;
    }
    session.eval.locals.set(name, minted);
    // BUG-922: dropped when the minting program or nested do/run file
    // concludes -- program::concludeLevel (pn_tv_do_gone: var=111).
    session.mintedTempvars.emplace_back(session.preserveLevel(), minted);
  }
  return ojson::array();
}

// ---------------------------------------------------------------------------
// levelsof

namespace {

// levelsof.py's numeric _fmt: integer-valued -> plain integer string, else
// Python's str(float) shortest round-trip repr.
std::string levelsofFmtNum(double v) {
  if (std::isfinite(v) && v == static_cast<double>(static_cast<long long>(v))) {
    return std::to_string(static_cast<long long>(v));
  }
  return eval::pythonFloatRepr(v);
}

}  // namespace

ojson runLevelsof(const std::string& varname, const std::optional<std::string>& localName,
                   bool clean, const std::optional<std::string>& ifExpr, SessionState& session) {
  // ONESHOT-GETCOLS2: `levelsof` reads ONE column's values (plus whatever an `if`
  // names). Materializing the head for that was 14 of the flushes left after the
  // mutation entrance moved. `varname` is passed as alsoInclude because it is the
  // subject, not an expression reference.
  std::string lvlMiniPath;
  std::unique_ptr<Datastore> lvlMini;
  if (auto cols = referencedColumns(session, {ifExpr.has_value() ? &*ifExpr : nullptr}, varname))
    lvlMini = fetchColumns(session, *cols, session.workdir(), &lvlMiniPath);
  if (lvlMini && !lvlMini->has(varname)) lvlMini.reset();  // subject absent → not usable
  Datastore* ds = lvlMini ? lvlMini.get() : session.datastore();
  if (ds == nullptr) return eval::noDataLoaded();

  // levelsof.py's run_levelsof(ds, varname, clean) operates on `target_ds` —
  // the CURRENT dataset, or a row-filtered temp copy when `if COND` is
  // present (dispatcher.py's _run_levelsof: strmutate.write_filtered_hbin,
  // keep=True). M3a numeric-only slice: string/factor `if` conditions are
  // OUT of scope (strmutate's row-wise evaluator is not ported this
  // milestone) — a pure-numeric if routes through cobra eval --keepif.
  Datastore* targetDs = ds;
  std::unique_ptr<Datastore> filteredHolder;
  if (ifExpr.has_value() && !ifExpr->empty()) {
    std::string outPath = nextPath(session.workdir(), "levelsof");
    // BUG-381: this filtered copy is READ and abandoned — it never becomes the
    // head, so no supersede ever names it and reclaimSupersededScratch cannot
    // reach it however it is scheduled. That is what the §38 projection ledger
    // is for: register it and the outermost dispatch boundary drains it.
    session.noteProjectionScratch(outPath);
    // ONESHOT-LASTFLUSH: the earlier wave filtered the REAL head here, reasoning
    // that a projection "carries only what we fetched". It carries exactly what
    // this filter needs: the `if` expression's own columns (that is what the fetch
    // was built from) plus `varname`, which is the only column read back below. So
    // filter the projection and leave the head virtual — this was the last 14 of
    // the flushes, and the fetch above was pure overhead on this branch before.
    //
    // The fallback is not decoration. `referencedColumns` leaves identifiers the
    // shadow card does not confirm alone, so a card that is missing a real column
    // yields a projection the filter cannot evaluate. That is an ENGINE error, not
    // a distinguishable one, so it is caught and retried against the head rather
    // than reported — a miss must cost a fallback, never an answer.
    const std::string lvlHead = session.currentHbin().value_or(ds->path());
    bool filtered = false;
    if (lvlMini) {
      std::vector<std::string> tryArgs = translate::injectTsContextForExpr(
          {"eval", "--data", lvlMiniPath, "--keepif", *ifExpr, "--out", outPath},
          *ifExpr, session);  // BUG-369 follow-up
      try {
        runCobraJson(tryArgs);
        filtered = true;
      } catch (const CobraError&) {
        cecon::native_unlink(outPath);  // never leave a torn file for the retry to open
      }
    }
    if (!filtered) {
      std::vector<std::string> args = translate::injectTsContextForExpr(
          {"eval", "--data", lvlHead, "--keepif", *ifExpr, "--out", outPath},
          *ifExpr, session);  // BUG-369 follow-up
      try {
        runCobraJson(args);
      } catch (const CobraError& ce) {
        return engineErrorBlock("levelsof: " + cobraErrorStr(ce), ce.code);
      }
    }
    filteredHolder = std::make_unique<Datastore>(outPath);
    targetDs = filteredHolder.get();
  }

  if (!targetDs->has(varname)) {
    // Python: `raise KeyError(f"variable '{varname}' not found in dataset")`,
    // caught as `except KeyError as e: return make_engine_error(str(e))`.
    // KeyError.__str__() wraps the message in an EXTRA layer of double
    // quotes (Python's repr-of-a-single-string-arg convention) — reproduce
    // that exact double-quoting, not just the inner message.
    return engineErrorBlock("\"variable '" + varname + "' not found in dataset\"");
  }

  std::vector<std::string> levelStrings;
  if (targetDs->dtype(varname) == "f64") {
    const double* d = targetDs->col(varname).f64();
    int64_t count = targetDs->col(varname).nbytes / static_cast<int64_t>(sizeof(double));
    std::vector<double> uniq;
    for (int64_t i = 0; i < count; ++i) {
      if (!std::isnan(d[i])) uniq.push_back(d[i]);
    }
    std::sort(uniq.begin(), uniq.end());
    uniq.erase(std::unique(uniq.begin(), uniq.end()), uniq.end());
    for (double v : uniq) levelStrings.push_back(levelsofFmtNum(v));
    // clean has NO effect on numeric columns (Python only branches on
    // `clean` inside the `else` / factor-dtype arm) — level_strings above
    // is final for f64 regardless of `clean`.
  } else {
    const int32_t* codes = targetDs->col(varname).codes();
    int64_t count = targetDs->col(varname).nbytes / static_cast<int64_t>(sizeof(int32_t));
    std::vector<int32_t> uniqCodes;
    for (int64_t i = 0; i < count; ++i) {
      if (codes[i] >= 0) uniqCodes.push_back(codes[i]);
    }
    std::sort(uniqCodes.begin(), uniqCodes.end());
    uniqCodes.erase(std::unique(uniqCodes.begin(), uniqCodes.end()), uniqCodes.end());
    const auto& levelsList = targetDs->varinfo(varname).levels;
    std::vector<std::string> rawLabels;
    for (int32_t c : uniqCodes) {
      rawLabels.push_back((c >= 0 && static_cast<size_t>(c) < levelsList.size())
                              ? levelsList[static_cast<size_t>(c)]
                              : std::to_string(c));
    }
    std::sort(rawLabels.begin(), rawLabels.end());
    for (const auto& lbl : rawLabels) {
      levelStrings.push_back(clean ? lbl : ("\"" + lbl + "\""));
    }
  }

  std::string joined;
  for (size_t i = 0; i < levelStrings.size(); ++i) {
    if (i) joined += " ";
    joined += levelStrings[i];
  }

  ojson out;
  out["type"] = "levelsof";
  out["varname"] = varname;
  ojson levelsArr = ojson::array();
  for (const auto& s : levelStrings) levelsArr.push_back(s);
  out["levels"] = levelsArr;
  out["text"] = joined;

  // `if local_name:` — ONLY set when local() was explicitly given (no
  // default local-var name).
  if (localName.has_value() && !localName->empty()) {
    session.eval.locals.set(*localName, joined);
  }
  return out;
}

// ---------------------------------------------------------------------------
// M3b: clear / set obs / cd / pwd / noop (set|version|compress) — foundational
// commands the corpus's data-construction idiom (`clear` -> `set obs N` ->
// `gen ...`) depends on. C++ port of dispatcher.py's clear/set_obs/cd/pwd
// handlers + session.py's clear_data.

// BS-044 (BETA campaign C4): `clear <subcmd>` scoping. Before this fix
// EVERY subcommand fell through to the same bare-data-wipe path (probe
// bs044: `clear results` answered "No dataset is loaded"), which is wrong
// for every subcommand except a genuinely bare `clear`/`clear all`.
//
// Ground truth here is Stata's own clear.ado (StataNow 19.5, dumped via
// `program list` while live-probing this item — not a guess):
//   ""            -> `drop _all` + `label drop _all` ONLY. Scalars,
//                     matrices, e()/r() results and est-store all SURVIVE
//                     a bare `clear` (live-probed: sca1/matrix m1/ereturn
//                     list all still present after a bare `clear`).
//   "all"/"*"     -> everything (data, labels, matrices, scalars, e()/r(),
//                     programs, panelvar/timevar).
//   "results"/    -> ONE shared branch in the real ado: `return clear` +
//   "matrix"         `ereturn clear` + `sreturn clear` + `_return drop
//                     _all` (r()/e()/s() results only — NOT scalars, NOT
//                     matrices). `matrix` ADDITIONALLY does `matrix drop
//                     _all` + `_est drop _all` (user matrices + the eststo
//                     store) — oracle-pinned (`clear results` keeps N=336
//                     + scalar list) plus this ado reading for the r()/e()
//                     split and the matrix/results relationship.
//   "programs"    -> `program drop _all` (session.programs — real store).
//   "mata"        -> empties the M0 Mata store (session.eval.mataMatrices).
//   "ado"/"rngstream"/"rngstreams"/"frames"/"collect" -> real Stata
//                     subcommands with genuinely NO backing store in this
//                     build (no frames/collect engine, no rngstream
//                     state, no ado-vs-interactive program provenance) —
//                     honest scoped no-ops with a note, replacing the old
//                     "wipe the whole dataset" mistake, not a silent
//                     no-message pass-through.
//   anything else -> real Stata hard-rejects ("`1' not allowed", r(198));
//                     matched here rather than silently no-opping OR
//                     wiping data for a typo'd subcommand.
//
// Deviation from this batch's task-brief shorthand (noted for review): the
// brief grouped "matrix" with the no-store no-op list, but PIET already
// carries a real matrix store (eval.matrices/eval.eMatrices, populated by
// the `matrix define` handler) and a real est-store (session.estimates) —
// clearing them costs no new infrastructure and matches verified Stata
// truth, so `clear matrix` is implemented as a genuine clear here, not a
// no-op. See this task's return report for the full callout.
ojson runClear(const std::string& subIn, SessionState& session) {
  std::string sub = lower(trim(subIn));

  auto silentEcho = []() {
    ojson out;
    out["type"] = "echo";
    out["text"] = "";
    return out;
  };

  if (sub.empty()) {
    // Bare `clear`: dataset (+ its label defs) ONLY.
    session.clearData();
    session.clearMetadata();
    return silentEcho();
  }

  if (sub == "all" || sub == "*") {
    session.clearData();
    session.clearMetadata();
    // Macros are NOT cleared: clear.ado's `all` branch never runs `macro drop`
    // (it drops data, frames, labels, matrices, scalars, constraints, files,
    // programs, timers, Mata ...), and StataBE keeps both globals and locals
    // across it. 007's `Wave 1 cleaning.do` opens with `clear all` and then reads
    // `$data`, a global its settings file set one level up.
    session.eval.scalars.clear();
    session.eval.rValues = eval::OrderedFloatDict();
    session.eval.rStrings.clear();
    session.eval.eScalars = eval::OrderedFloatDict();
    session.eval.eStrings.clear();
    session.eval.matrices.clear();
    session.eval.mataMatrices.clear();  // clear.ado's `all` branch runs `mata: mata clear`
    session.eval.eMatrices.clear();
    session.eval.rMatrices.clear();
    session.lastModel_reset();
    session.panelvar.reset();
    session.timevar.reset();
    session.programs.clear();       // ado's `program drop _all` under "all".
    ++session.programsEpoch;
    session.returnStaging.clear();  // ado's `_return drop _all` under "all".
    // BUG-392 (2026-09-02): the rest of clear.ado's `all` branch — `_est drop _all`,
    // `graph drop _all`, `postutil clear`, `file close _all`, `frames reset`, the
    // preserve stack — plus the session's own `use` memos and the engine's stashes
    // (BUG-390), none of which any earlier `clear all` touched.
    // BUG-1079: `postutil clear` CLOSES each postfile, so what was posted is on
    // disk afterwards (StataBE 19.5, 2026-10-28: three posted rows, then `clear
    // all`, then `describe using` reports N 3 -- evidence/t1_clearall_postfile).
    // The postfiles are closed through runPostclose first; a failed close does
    // not stop `clear all`, exactly as Stata's own clear.ado runs it.
    {
      std::vector<std::string> open;
      for (const auto& kv : session.postfiles) open.push_back(kv.first);
      for (const auto& h : open) (void)runPostclose(h, session);
    }
    session.clearAllSessionObjects();
    // BUG-1461: clear.ado's `all` branch also runs `timer clear` (StataBE 19.5:
    // after `clear all` a stopped timer lists nothing and r(nt#) is missing).
    session.timers.fill(TimerSlot{});
    return silentEcho();
  }

  if (sub == "results" || sub == "matrix") {
    // Shared ado branch: r()/e()/s() results + return-staging.
    session.eval.rValues = eval::OrderedFloatDict();
    session.eval.rStrings.clear();
    session.eval.eScalars = eval::OrderedFloatDict();
    session.eval.eStrings.clear();
    session.eval.eMatrices.clear();
    session.eval.rMatrices.clear();
    session.lastModel_reset();
    session.returnStaging.clear();
    if (sub == "matrix") {
      // `clear matrix` additionally: `matrix drop _all` + `_est drop _all`,
      // which takes the stores' `_est_*` variables too (BUG-1212).
      session.eval.matrices.clear();
      clearStoredEstimates(session);
    }
    return silentEcho();
  }

  if (sub == "programs") {
    session.programs.clear();
    ++session.programsEpoch;
    return silentEcho();
  }

  // ml-getmata T4: Mata now HAS a store (session.eval.mataMatrices, the M0
  // subset's namespace), so `clear mata` is a real clear and no longer one of
  // the scoped no-ops below. It touches nothing else -- data, Stata matrices
  // and results all survive it, which selftest's C4/BS-044 leg (d) pins.
  if (sub == "mata") {
    session.eval.mataMatrices.clear();
    return silentEcho();
  }

  static const std::unordered_set<std::string> kNoStoreSubs = {
      "ado", "rngstream", "rngstreams", "frames", "collect"};
  if (kNoStoreSubs.count(sub)) {
    ojson out;
    out["type"] = "echo";
    out["text"] = "(clear " + sub + ": no-op -- this build of PIET has no " + sub + " store)";
    return out;
  }

  return engineErrorBlock("clear: '" + sub + "' not allowed", "syntax_error");
}

ojson runSetObs(const std::string& nArg, SessionState& session) {
  // BUG-1463: Stata takes the count as typed digits (`05`, `+5` are 5) between
  // the current _N and 2,147,483,619. Every other argument -- below _N, negative,
  // `8.0`, `3.5`, `1e9`, `abc` -- is one refusal, r(198), with its range line
  // (StataBE 19.5: `set obs 3` on 5 rows). This used to answer "cannot reduce
  // the number of observations" and the parse failures at rc 1, and to truncate
  // `8.5` to 8.
  Datastore* dsHave = session.currentHbin().has_value() ? session.datastore() : nullptr;
  const long long curN = dsHave ? static_cast<long long>(dsHave->nRows()) : 0;
  const std::string typed = trim(nArg);
  static const std::regex reCount(R"(^\+?\d{1,10}$)");
  const long long nTarget = std::regex_match(typed, reCount) ? std::stoll(typed) : -1;
  if (nTarget < curN || nTarget > 2147483619LL) {
    std::string lo = std::to_string(curN);
    for (int at = static_cast<int>(lo.size()) - 3; at > 0; at -= 3) lo.insert(static_cast<size_t>(at), ",");
    return engineErrorBlockRc("observation number out of range\n    Observation number must be "
                              "between " + lo + " and 2,147,483,619.  (Observation numbers are "
                              "typed without commas.)", 198);
  }

  std::string out = nextPath(session.workdir(), "setobs");
  // A dataset with NO variables (`clear` + `set obs 0`, or `set obs 3` on an
  // empty session) has nothing to pad, and the pad-and-append path below would
  // hand cobra a header-only CSV with no columns. Growing it is the same act as
  // building a blank frame from nothing, so it takes the branch that does that.
  Datastore* dsNow = session.currentHbin().has_value() ? session.datastore() : nullptr;
  const bool growNoVars =
      dsNow != nullptr && dsNow->names().empty() && nTarget > dsNow->nRows();
  if (!session.currentHbin().has_value() || growNoVars) {
    // No data in memory -> N blank observations, 0 variables. Build via a
    // throwaway 1-column CSV (a dummy value repeated nTarget times) then
    // `cobra eval --drop` the dummy column, landing on {n_rows:N,
    // columns:[]} — bit-identical to the oracle's _write_hbin(out,[],[],n).
    std::string csvPath = session.workdir() + "/setobs_seed.csv";
    registerScratchMint(csvPath);  // BUG-389
    {
      std::ofstream csv(cecon::to_native_path(csvPath));
      csv << "_z\n";
      for (long long i = 0; i < nTarget; ++i) csv << "1\n";
      // BUG-124 sibling: surface a silently-failed CSV write (ENOSPC) here
      // instead of letting convert build a short dataset from it.
      csv.flush();
      if (!csv) {
        cecon::native_unlink(csvPath);
        return engineErrorBlock("set obs: failed writing scratch CSV (disk full or I/O error)");
      }
    }
    std::string seedHbin = session.workdir() + "/setobs_seed.hbin";
    registerScratchMint(seedHbin);  // BUG-389
    std::vector<std::string> convArgs = {"convert", "--data", csvPath, "--out", seedHbin,
                                          "--delim", ","};
    // BUG-124: runCobra does not throw — check the result (the old catch was
    // dead) and remove any torn partial before it can be load()ed/mmap'd.
    CobraRunResult convCr = runCobra(convArgs);
    if (!convCr.ok) {
      cecon::native_unlink(seedHbin);
      CobraError ce = errorFromRaw(convCr);
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    // FU-1252: `set obs 0` (and the empty dataset `generate`/`save, emptyok`
    // materialize after `clear`) has NO variables -- the seed's `_z` used to
    // stay behind as a variable (w2ar dx_save_079_emptyok k=0 N=0, log:4521).
    // `cobra eval --drop` on the 0-row seed is measured to leave 0 columns.
    {
      std::vector<std::string> dropArgs = {"eval", "--data", seedHbin, "--drop", "_z", "--out",
                                           out};
      try {
        runCobraJson(dropArgs);
      } catch (const CobraError& ce) {
        return engineErrorBlock(cobraErrorStr(ce), ce.code);
      }
      session.load(out);
    }
    return mutationResult(session, "set_obs");
  }

  Datastore* ds = session.datastore();
  int64_t curRows = ds ? ds->nRows() : 0;
  if (nTarget == curRows) {
    return mutationResult(session, "set_obs");
  }

  // Extend: pad every existing column to nTarget rows with system-missing.
  // cobra has no direct "append N blank rows" verb; build a blank frame of
  // (nTarget - curRows) rows with the SAME column set (each column = "."),
  // then `cobra append` it onto the current dataset.
  //
  // BUG-697: this path never worked against the current engine, three ways:
  //  1. `cobra append --out` prints a plain-text notice on stderr and NOTHING on
  //     stdout (apps/cmds/cmds_data.cpp cmd_append), and this call went through
  //     runCobraJson, so every extension of a non-empty dataset -- `set obs 8`
  //     on 6 rows, svmat/getmata with more rows than _N -- died as "cobra
  //     produced non-JSON output", rc 1. It is a PIET call-site bug, not an
  //     engine one: the verb's contract is the file, as runVerb/runCobra use it
  //     everywhere else (handlers_aer_reshape.cpp's insobs, this file's
  //     line-level note "use runCobra (not runCobraJson ...)").
  //  2. The pad cells were BLANK. cobra's CSV reader skips a blank line, so on a
  //     one-variable dataset every pad row vanished ("6 master + 0 using") and
  //     _N silently stayed put -- hidden until now behind (1).
  //  3. A blank-or-`.` column in the pad CSV reads as NUMERIC, and append's
  //     unflagged default promotes a numeric/string clash to f64 -- a string
  //     variable would have come back as numbers. `--type-clash force` makes
  //     the pad cells missing OF THE MASTER'S TYPE (BUG-550's Stata rule), which
  //     is exactly "extend with missing".
  // Measured against the frozen engine: `.` cells + --type-clash force give
  // "6 master + 2 using = 8 rows", f64 stays f64, a factor stays a factor.
  int64_t padRows = nTarget - curRows;
  std::string padCsv = session.workdir() + "/setobs_pad.csv";
  registerScratchMint(padCsv);  // BUG-389
  {
    std::ofstream csv(cecon::to_native_path(padCsv));
    const auto& names = ds->names();
    for (size_t i = 0; i < names.size(); ++i) {
      if (i) csv << ",";
      csv << names[i];
    }
    csv << "\n";
    for (int64_t r = 0; r < padRows; ++r) {
      for (size_t i = 0; i < names.size(); ++i) {
        if (i) csv << ",";
        csv << ".";  // never blank: see BUG-697 (2) above
      }
      csv << "\n";
    }
    // BUG-124 sibling: flush + check — a silently short pad CSV appends fewer
    // blank rows than `set obs` promised.
    csv.flush();
    if (!csv) {
      cecon::native_unlink(padCsv);
      return engineErrorBlock("set obs: failed writing scratch CSV (disk full or I/O error)");
    }
  }
  std::vector<std::string> args = {"append",       "--data", *session.currentHbin(),
                                    "--using",      padCsv,   "--type-clash",
                                    "force",        "--out",  out};
  CobraRunResult appendCr = runCobra(args);
  if (!appendCr.ok) {
    CobraError ce = errorFromRaw(appendCr);
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  // BUG-124's torn-write guard, as at every other file-contract call site.
  if (!cecon::native_exists(out)) {
    return engineErrorBlock("set obs: failed writing output dataset (disk full or I/O error)");
  }
  session.load(out);
  // The count is asserted, not assumed: (2) above extended nothing at rc 0.
  if (Datastore* grown = session.datastore(); grown == nullptr || grown->nRows() != nTarget) {
    return engineErrorBlock("set obs: the dataset has " +
                            std::to_string(grown ? grown->nRows() : 0) +
                            " observations after extending, not " + std::to_string(nTarget));
  }
  return mutationResult(session, "set_obs");
}

ojson runCd(const std::string& dirArgIn, SessionState& session) {
  std::string dirArg = trim(dirArgIn);
  // dequote (routes_data.py's _dequote: strip matching outer quotes)
  if (dirArg.size() >= 2 && ((dirArg.front() == '"' && dirArg.back() == '"') ||
                             (dirArg.front() == '\'' && dirArg.back() == '\''))) {
    dirArg = dirArg.substr(1, dirArg.size() - 2);
  }
  // `~` expansion (os.path.expanduser) — HOME-relative only, matches the
  // common corpus usage; `~user` forms are not exercised.
  if (!dirArg.empty() && dirArg[0] == '~') {
    std::string home = cecon::env_utf8("HOME");
#ifdef _WIN32
    // Windows has no HOME by default; ~ resolves against USERPROFILE (matches
    // resolveUserPath's _WIN32 branch). POSIX path unchanged.
    if (home.empty()) home = cecon::env_utf8("USERPROFILE");
#endif
    if (!home.empty()) dirArg = home + dirArg.substr(1);
  }
  namespace fs = std::filesystem;
  fs::path p(dirArg);
  if (p.is_relative()) {
    std::string base = session.cwd.has_value() && !session.cwd->empty() ? *session.cwd
                        : fs::current_path().u8string();
    p = cecon::to_native_path(base) / p;
  }
  std::error_code ec;
  fs::path abs = fs::weakly_canonical(p, ec);
  if (ec) abs = p.lexically_normal();
  if (!fs::is_directory(abs)) {
    return engineErrorBlock("Directory not found: " + abs.u8string());
  }
  session.cwd = abs.u8string();
  session.eval.cwd = *session.cwd;
  // AER scope-2 B2-3: bofd()/dofb() resolve `<name>.stbcal` against the
  // session's working directory, and the expression surfaces that call them
  // (display / local = / gen) reach the loader through a process-level search
  // directory rather than a session handle -- macros.cpp's evalArith uses the
  // immutable process namespace. Keeping it in step HERE is what makes
  // `cd sub` + `bofd("cal", d)` find sub/cal.stbcal (measured: Stata finds a
  // calendar in the new cwd after cd).
  bcal::setSearchDir(*session.cwd);
  ojson out;
  out["type"] = "echo";
  out["text"] = abs.u8string();
  return out;
}

ojson runPwd(SessionState& session) {
  namespace fs = std::filesystem;
  std::string cwd = session.cwd.has_value() && !session.cwd->empty() ? *session.cwd
                     : fs::current_path().u8string();
  ojson out;
  out["type"] = "echo";
  out["text"] = cwd;
  return out;
}

// BUG-028 / BS-006: `log using FILE [, replace|append] [text|smcl]` really
// opens FILE now (SessionState::openLog) — every subsequent OUTERMOST
// dispatch() call's finalized blocks get teed to it (see dispatcher.cpp's
// dispatch()/dispatchImpl() split + SessionState::logTee's doc comments).
// `log close` flushes+closes; `log off`/`log on` pause/resume an already-open
// log (stream stays open, writes suspended while paused). Sub-keyword +
// `using` path extraction is UNCHANGED from the original BUG-028 shim
// (mirrors router/commands.py's parse: sub = first varlist token; path via a
// `using ("..."|'...'|\S+)` regex on the raw command text, resolved against
// session.cwd like shims.py's _resolve(session, path)) — everything below the
// path-resolution is new.
//
// Format, per the log help file ("Options for use with log"), identified
// against StataBE 19.5 captures: `text` and `smcl` decide the format when
// given; otherwise a ".smcl" file is SMCL, ".log" is text, and a name with no
// extension follows `set logtype` (not settable here, so its default, SMCL)
// and gets ".smcl" appended — ".log" for a text log. The SMCL writer is real
// (session_state.cpp, above openLog): the file starts "{smcl}", carries the
// header/footer blocks and tags every line, so a file that claims SMCL is SMCL
// — PIET's standing "never silently wrong" rule is kept by building the
// format instead of refusing it. Two documented deviations: the text format
// is PIET's own plain transcript (no Stata-style header/footer, unchanged),
// and any OTHER extension (".txt", ".tex", ...) without `text`/`smcl` stays
// text, as it has always been here, where StataBE would write SMCL.
//
// Mode: `replace` truncates; `append` (or bare with no existing file) opens
// in append mode — C++'s ios::app already creates a fresh file when none
// exists, matching Stata's own `append` semantics with no extra logic.
// Bare with an EXISTING file is Stata's real "file already exists" hard
// stop — kept honest (not silently defaulted either way) rather than
// guessing; the message text below deliberately contains "file" +
// "already exists" so errors.cpp's coarseRc() maps a `capture`-wrapped
// call's `_rc` to 602 (Stata's real code) via its existing engine_error/
// mentionsFileOrDir branch, with no new mapping rule needed there.
//
// Only one log open at a time: a second `log using` while one is already
// open is an honest reject too (matching real Stata — you must `log close`
// first). BS-117 (multi named-log bookkeeping / `name()`) stays post-beta/
// OUT; analysis.do and the eval golden fixtures never open more than one.
ojson runLog(const std::string& raw, SessionState& session) {
  // Tokenize on whitespace to find the sub-keyword (varlist[0] equivalent) —
  // "log" itself is the command word, so the first token after it is the sub.
  // BUG-251/WS-Q: was `rawLower.find("log")` + `substr(pos + 3)` — the fifth
  // member of BUG-232's family, in the same file as two of the four the entry
  // named, and missed by its grep because that grep only searched for the
  // command words the arc was widening. `log` has no Stata abbreviation today
  // (measured), so this was latent, not live; it is repaired anyway because
  // "latent" here means "one registry edit from killing the run".
  //
  // No spelling literal and no npos arithmetic: the sub-keyword is whatever
  // follows the leading token, and the leading token is the command word by
  // construction (eval::tokenize sets pc.raw = strip(line)). Kept local rather
  // than sharing dispatcher.cpp's rawArgsAfterCmd() because that helper lives
  // in dispatcher's anonymous namespace; this carries no command-word
  // knowledge, so it is not a second copy of a spelling set.
  std::string after;
  {
    size_t b = raw.find_first_not_of(" \t");
    if (b != std::string::npos) {
      size_t e = raw.find_first_of(" \t", b);
      if (e != std::string::npos) after = trim(raw.substr(e));
    }
  }
  std::string afterLower = lower(after);
  size_t subEnd = afterLower.find_first_of(" \t");
  std::string sub = lower(subEnd == std::string::npos ? after : after.substr(0, subEnd));

  if (sub == "using") {
    static const std::regex reUsing(R"re(\busing\s+("([^"]+)"|'([^']+)'|(\S+)))re",
                                     std::regex::icase);
    std::smatch m;
    std::string path;
    if (std::regex_search(raw, m, reUsing)) {
      if (m[2].matched) {
        path = m[2].str();
      } else if (m[3].matched) {
        path = m[3].str();
      } else {
        path = m[4].str();
        // BS-006: the bare/unquoted alternative's `\S+` is greedy and a
        // comma is not whitespace, so `log using file.log, replace` (no
        // space before the comma — the common style) swallows the comma
        // into `path` too. Quoted paths (m[2]/m[3] above) never hit this —
        // they stop at the closing quote — so the strip is scoped to this
        // branch only; a bare path can never legitimately end in a comma.
        if (!path.empty() && path.back() == ',') path.pop_back();
      }
    }
    if (!path.empty()) {
      // resolve relative to session.cwd, matching shims.py's _resolve(session, path).
      namespace fs = std::filesystem;
      fs::path p(path);
      if (p.is_relative()) {
        std::string base = session.cwd.has_value() && !session.cwd->empty() ? *session.cwd
                            : fs::current_path().u8string();
        p = cecon::to_native_path(base) / p;
      }
      path = p.lexically_normal().u8string();
    }
    if (path.empty()) {
      return engineErrorBlock("log using: no filename given");
    }

    // BS-006: parse the post-comma option tail the SAME quote/paren-aware
    // way every other command does (BS-018's `do FILE, nostop` precedent —
    // dispatcher.cpp). The `using` regex above already extracts the
    // filename regardless of where the comma sits, so re-splitting `raw`
    // here for the options tail is safe.
    std::string optsTail;
    long long commaPos = eval::findUnquotedComma(raw);
    if (commaPos >= 0) optsTail = trim(raw.substr(static_cast<size_t>(commaPos) + 1));
    auto opts = eval::parseOpts(optsTail);
    // BUG-233: the one copy of this vocabulary now lives in the file-scope
    // registration block (see top of file); this binding keeps every existing
    // use — including the reject net below — reading the same set.
    const std::unordered_set<std::string>& kLogKnownOpts = kVocabLog.known;
    // BUG-262 — a PER-NET exception to the dispatcher's single resolution
    // point, and the reason there has to be one. That point canonicalizes
    // `parsed.opts`; this handler does not read `parsed.opts`. It re-parses its
    // own option list out of `raw` (it has to: `log using "f, g.log", replace`
    // needs the quote-aware split), so the vector below never passed through
    // the dispatcher and arrives with raw keys. Measured: `log using f.log,
    // repl` was accepted by the net and then dropped by the loop underneath,
    // so the log was NOT replaced. Same lesson as BUG-232 — "one resolution
    // point" is a claim about ONE VECTOR, and a second parse is a second
    // vector. Uses the registered vocabulary, so still no second copy of the
    // spellings.
    eval::canonicalizeOptionKeys(opts, kLogKnownOpts);
    if (auto err = eval::rejectUnknownOptions(opts, kLogKnownOpts)) {
      return ojson(*err);
    }
    bool wantReplace = false, wantAppend = false, wantText = false, wantSmcl = false;
    std::string logName;
    for (const auto& [k, v] : opts) {
      if (k == "replace") {
        wantReplace = true;
      } else if (k == "append") {
        wantAppend = true;
      } else if (k == "text") {
        wantText = true;
      } else if (k == "smcl") {
        wantSmcl = true;
      } else if (k == "name") {
        // W:P8: `name(X)`. Stata also accepts `name(X, replace)`; only the
        // name itself is meaningful here (the file-level replace/append
        // decision is the option pair above), so take the first token.
        logName = v.isBool ? std::string() : trim(v.strValue);
        size_t comma = logName.find(',');
        if (comma != std::string::npos) logName = trim(logName.substr(0, comma));
        if (logName.empty()) {
          return engineErrorBlock("log using: option `name()' requires a log name",
                                   "syntax_error");
        }
      }
    }
    if (wantReplace && wantAppend) {
      return engineErrorBlock("log using: `replace' and `append' may not be combined",
                               "syntax_error");
    }
    if (wantText && wantSmcl) {
      return engineErrorBlock("log using: `text' and `smcl' may not be combined",
                               "syntax_error");
    }

    // Format selection and the file's extension (see this function's
    // top-of-doc comment). The extension is the filename's own, never a dot in
    // a directory name.
    const size_t nameStart = path.find_last_of("/\\");
    const std::string fileName = nameStart == std::string::npos ? path : path.substr(nameStart + 1);
    const size_t dot = fileName.rfind('.');
    const bool hasExtension = dot != std::string::npos && dot > 0;
    bool useSmcl;
    if (wantSmcl) {
      useSmcl = true;
    } else if (wantText) {
      useSmcl = false;
    } else if (!hasExtension) {
      useSmcl = true;  // `set logtype` default (not settable in this build)
    } else {
      useSmcl = lower(fileName.substr(dot)) == ".smcl";
    }
    if (!hasExtension) path += useSmcl ? ".smcl" : ".log";

    // W:P8: the clash is per NAME, not per session — a named log may be opened
    // while the unnamed one is running (that is the whole point of `name()`,
    // and what the P8 target does), but reopening a name that is already
    // logging is Stata's own hard stop: r(604) "log file already open", for the
    // unnamed log and a named one alike (BUG-1463; StataBE 19.5, it was rc 1).
    if (session.logIsOpen(logName)) return engineErrorBlockRc("log file already open", 604);

    bool appendMode = wantAppend;
    if (!wantReplace && !wantAppend && cecon::native_exists(path)) {
      return engineErrorBlock(
          "log using: file already exists (specify `replace' or `append')");
    }
    // The `log using` command is itself running inside a dispatch frame, so
    // its own depth is the depth of the sibling commands this log must record.
    if (!session.openLog(path, appendMode, logName, session.dispatchDepth(), useSmcl)) {
      return engineErrorBlock("log using: could not open \"" + path + "\" for writing");
    }
    return ojson::array();  // Stata: silent when log opens successfully
  }

  // `close`/`off`/`on` take an optional log name (or `_all`) after the
  // sub-keyword: `log close p8named`, `log off _all`.
  std::string target;
  if (subEnd != std::string::npos) target = trim(after.substr(subEnd));
  const bool targetAll = lower(target) == "_all";
  if (targetAll) target.clear();

  if (sub == "close") {
    if (targetAll) {
      session.closeAllLogs();
      return ojson::array();
    }
    // Naming a log that is not open is Stata's r(111) "log NAME not found"
    // (live-verified). A bare `log close` with nothing open stays the silent
    // no-op it has always been.
    if (!session.closeLog(target) && !target.empty()) {
      return engineErrorBlock("log " + target + " not found");
    }
    return ojson::array();  // Stata: silent when log closes
  }

  if (sub == "off") {
    if (targetAll) {
      for (const auto& n : session.logNames()) session.pauseLog(n);
      return ojson::array();
    }
    if (!session.pauseLog(target) && !target.empty()) {
      return engineErrorBlock("log " + target + " not found");
    }
    return ojson::array();  // Stata: silent when log pauses
  }

  if (sub == "on") {
    if (targetAll) {
      for (const auto& n : session.logNames()) session.resumeLog(n);
      return ojson::array();
    }
    if (!session.resumeLog(target) && !target.empty()) {
      return engineErrorBlock("log " + target + " not found");
    }
    return ojson::array();  // Stata: silent when log resumes
  }

  // Unknown subcommand -- silent noop (unchanged from BUG-028's original shim).
  return ojson::array();
}

// ---------------------------------------------------------------------------
// U7/S9 offline shims: which / ssc / confirm / mkdir / erase — mirrors
// piet/backend/piet/shims.py (Python oracle) + router/commands.py's parse.
// All are no-data-needed (kNoDataCommands) so classroom do-files that guard
// package/file existence before `use` never halt. analysis.do's line-77
// package-presence loop (`capture which drdid` / `capture ssc install ...`)
// and its dozen `capture confirm variable/numeric/string ...` guards
// (throughout the depvar/control setup) exercise all five directly.

namespace {
// Extract a quoted-or-bare first argument after `word` in `raw` (mirrors the
// commands.py `mkdir`/`erase` regex: `\bWORD\s+("([^"]+)"|'([^']+)'|(\S+))`).
std::string firstArgAfter(const std::string& raw, const std::string& word) {
  std::regex re("\\b" + word + "\\s+(\"([^\"]+)\"|'([^']+)'|(\\S+))", std::regex::icase);
  std::smatch m;
  if (std::regex_search(raw, m, re)) {
    return m[2].matched ? m[2].str() : (m[3].matched ? m[3].str() : m[4].str());
  }
  return "";
}

std::string joinSpaceLocal(const std::vector<std::string>& v) {
  std::string s;
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) s += " ";
    s += v[i];
  }
  return s;
}

// A legal Stata identifier: starts with a letter (ASCII or Unicode) or underscore,
// followed by letters/digits/underscores or other non-ASCII characters, <=32
// characters. Used only by confirm's
// `names`/`name` sub-form (BS-042) — deliberately NOT Stata's full name
// grammar (no reserved-word/`str#`-collision checks — those need real
// infrastructure this handler doesn't have; see runConfirm's own doc
// comment for the honest-skip call).
// Unicode names: the one rule (strutil.h -> core/ident.h), 32 characters.
bool isValidStataName(const std::string& s) { return strutil::isValidStataIdentifier(s); }

// Same quote-aware first-argument extraction as firstArgAfter, but
// anchored to "whatever the line's own first token is" rather than one
// specific literal word — lets a single handler (runErase) serve more than
// one Stata command spelling for the SAME command. BS-045 (BETA campaign
// C4): `rm` is a genuine Stata BUILT-IN synonym of `erase` (live-probed
// `which rm` -> "built-in command: rm", not an invented alias), so
// runErase needs to extract its path argument correctly regardless of
// whether the raw line says "erase ..." or "rm ...".
std::string firstArgAfterCmd(const std::string& raw) {
  std::string t = trim(raw);
  size_t sp = t.find_first_of(" \t");
  if (sp == std::string::npos) return "";
  std::string rest = trim(t.substr(sp));
  static const std::regex reArg(R"re(^("([^"]+)"|'([^']+)'|(\S+)))re");
  std::smatch m;
  if (std::regex_search(rest, m, reArg)) {
    return m[2].matched ? m[2].str() : (m[3].matched ? m[3].str() : m[4].str());
  }
  return "";
}
}  // namespace

// `which NAME` — always rc0 (offline: we say every package is installed).
ojson runWhich(const std::string& raw, const std::vector<std::string>& varlist) {
  std::string name = !varlist.empty() ? varlist[0] : trim(firstArgAfter(raw, "which"));
  ojson out;
  out["type"] = "display";
  out["text"] = "(which: '" + name + "' -- PIET: assumed installed)";
  return out;
}

// `ssc install PKG [, replace]` / other ssc subcommands — noop rc0 (offline).
ojson runSsc(const std::vector<std::string>& varlist) {
  std::string sub = !varlist.empty() ? lower(varlist[0]) : "";
  std::string name = varlist.size() > 1 ? varlist[1] : "";
  if (sub == "install") {
    ojson out;
    out["type"] = "display";
    out["text"] = "(ssc install " + name + ": PIET is offline -- skipped)";
    return out;
  }
  return ojson::array();  // describe/uninstall/... -- silent noop
}

// `confirm [new] file|variable|numeric|string|integer|number|scalar|
// existence|names ARG` — existence/type check. Error block on failure (a
// `capture` prefix sets _rc via eval::coarseRc); silent (empty array) on
// success, matching Stata.
//
// BETA campaign C4 (BS-042/BS-043): every sub-form below was live-probed
// against StataNow 19.5 to pin its exact rc/message shape (see the C4
// agent's task-return report for the full probe transcript). Two root
// causes fixed this batch:
//   - "new variable" was never recognised as its own sub-command: the old
//     regex only special-cased "new file", so "confirm new variable X"
//     fell through to the varlist-tokenized fallback with sub="new" (an
//     unmatched literal), silently succeeding regardless of X's existence
//     (BS-042). Fixed by generically stripping a "new " prefix off subRaw
//     (mirrors how "new file" was already narrowed to "file") instead of
//     the old file-only `.find("file")` special case.
//   - "confirm integer number N" fed "number N" (not just "N") to the
//     integer parser, since "number" is REQUIRED Stata syntax glue between
//     "integer" and the value, not consumed by the old regex's single
//     "integer" alternative (BS-043). Fixed by stripping an optional
//     leading "number " token off arg specifically in the integer branch
//     (same pattern as the numeric/string branch's own "variable " strip
//     immediately below).
ojson runConfirm(const std::string& raw, const std::vector<std::string>& varlist,
                  SessionState& session) {
  // BUG-1103: `variable' abbreviates down to `v' -- `confirm var x', `confirm v x',
  // `confirm numeric var x' (StataBE 19.5). Spelled out only, `confirm var zz' fell
  // to the silent unknown-subtype branch below at rc 0, where Stata says r(111).
  static const std::string kVariableWord = R"(v(?:a(?:r(?:i(?:a(?:b(?:le?)?)?)?)?)?)?)";
  static const std::regex reConfirm(
      R"re(^\s*confirm\s+(new\s+)re" + kVariableWord +
          R"re(|new\s+file|number|integer|file|)re" + kVariableWord +
          R"re(|numeric|string|existence|names|name|scalar|matrix)\s+(.*))re",
      std::regex::icase);
  auto isVariableWord = [](const std::string& w) {
    return !w.empty() && w.size() <= 8 && std::string("variable").compare(0, w.size(), w) == 0;
  };
  std::smatch m;
  std::string sub, arg;
  bool isNew = false;
  if (std::regex_search(raw, m, reConfirm)) {
    std::string subRaw = lower(trim(m[1].str()));
    arg = trim(m[2].str());
    isNew = subRaw.rfind("new", 0) == 0;
    if (isNew) {
      size_t p = subRaw.find_first_not_of(" \t", 3);
      subRaw = (p == std::string::npos) ? "" : subRaw.substr(p);
    }
    if (isVariableWord(subRaw)) subRaw = "variable";
    sub = subRaw == "name" ? "names" : subRaw;
    if (sub == "numeric" || sub == "string") {
      // The type word takes `variable' (abbreviated as above); anything else there
      // is r(198) "invalid syntax" (`confirm numeric vx ab', `confirm numeric
      // variables ab'; StataBE 19.5).
      static const std::regex reVarKw("^" + kVariableWord + R"((\s+|$))", std::regex::icase);
      std::smatch vk;
      if (!std::regex_search(arg, vk, reVarKw)) return engineErrorBlockRc("invalid syntax", 198);
      arg = arg.substr(static_cast<size_t>(vk.length(0)));
    }
    if (sub == "integer") {
      // BS-043: "confirm integer number 5" — "number " is required syntax
      // glue, not part of the value; strip it the same way numeric/string
      // strip their own "variable " glue two lines up. A bare "confirm
      // integer 5" (no "number") is left untouched (harmless permissive
      // superset, never exercised by the oracle either way).
      static const std::regex reNumberKw(R"(^number\s+)", std::regex::icase);
      arg = std::regex_replace(arg, reNumberKw, "");
    }
    arg = trim(arg);
    if (arg.size() >= 2 && ((arg.front() == '"' && arg.back() == '"') ||
                            (arg.front() == '\'' && arg.back() == '\''))) {
      arg = arg.substr(1, arg.size() - 2);
    }
  } else {
    sub = !varlist.empty() ? lower(varlist[0]) : "";
    if (sub == "name") sub = "names";
    if (isVariableWord(sub)) sub = "variable";
    arg = varlist.size() > 1 ? varlist[1] : "";
    // A bare `confirm numeric` lacks its `variable` word: r(198) (StataBE 19.5).
    if ((sub == "numeric" || sub == "string") && !isVariableWord(lower(arg)))
      return engineErrorBlockRc("invalid syntax", 198);
    if (sub == "numeric" || sub == "string") arg.clear();
  }

  if (sub == "file") {
    std::string path = resolveUserPath(session, arg);
    namespace fs = std::filesystem;
    // BUG-361: ask the SESSION before asking the filesystem. A `save` to a minted
    // tempfile is routed to the engine's in-RAM virtual-file table when the
    // resident plane is live (handlers_data_verbs.cpp's isMintedTempfile arm) and
    // writes no bytes at all, so `fs::exists` answers "no" about a file the user
    // just saved and can still `use` — and answers it with _rc 601, the SAME code
    // a genuinely absent file returns, so a script cannot tell the two apart.
    // Measured 2026-08-19: plain save -> confirm _rc 0; tempfile save -> _rc 601;
    // absent file -> _rc 601, while `use` on that middle path restores all 5 rows.
    // `use` already consults the table (isVirtualSave, handlers.cpp:363); this is
    // the same question asked from the same session, so the two agree by
    // construction rather than by inspection. Both keys come from
    // resolveUserPath(), so they are the same string. A registered entry the
    // engine has since lost still reads as present here — which is what the
    // session believes, and the following `use` is where that failure belongs.
    bool exists = fs::exists(cecon::to_native_path(path)) || session.isVirtualSave(path);
    if (isNew) {
      if (exists) return engineErrorBlock("confirm new file: file already exists: " + path);
    } else {
      if (!exists) return engineErrorBlock("confirm file: file not found: " + path);
    }
    return ojson::array();
  }

  if (sub == "variable" || sub == "numeric" || sub == "string") {
    if (isNew) {
      // BS-042: `confirm new variable NAME [NAME ...]` — multi-name form,
      // ALL names must be new; report the FIRST existing conflict (matches
      // live-probed Stata: `confirm new variable newvar1 x newvar2` with x
      // pre-existing -> "variable x already defined", rc=110, stopping at
      // the first hit even though newvar2 was never checked). With no
      // dataset loaded, every name is trivially new (nothing can conflict).
      std::istringstream iss(arg);
      std::string tok;
      while (iss >> tok) {
        if (session.hasData()) {
          // C4-L2A (D1 card plane): existence is card-answerable — a CURRENT
          // card is authoritative for name presence either way (brief §1
          // confirm row). Falls back to the unchanged datastore() path when
          // the card is unavailable/non-current or under the
          // PIET_NO_RESIDENT_READ escape arm.
          bool exists;
          if (const std::vector<VarInfo>* card = session.residentReadCard()) {
            exists = cardHasName(*card, tok);
          } else {
            exists = session.datastore()->has(tok);
          }
          if (exists) {
            return engineErrorBlock("confirm new variable: variable '" + tok +
                                     "' already defined");
          }
        }
      }
      return ojson::array();
    }
    // BUG-1006: `confirm variable v, exact` (w2aq_histogram_family.do hk_gv, every
    // hk_*_g record). The option list rode into the varlist, so `v,` and `exact`
    // were looked up as names and the check answered r(111) for a variable that
    // exists. `exact` turns off the abbreviation below (BUG-1103); any other
    // option is refused by name, r(198). `exact` abbreviates to `ex`, case
    // matters, and a second one is refused too (`, exa` is exact; `, e`, `, EXACT`
    // and `, exact exact` are r(198); StataBE 19.5).
    bool exact = false;
    if (const size_t comma = arg.find(','); comma != std::string::npos) {
      std::istringstream oss(arg.substr(comma + 1));
      arg = trim(arg.substr(0, comma));
      std::string opt;
      while (oss >> opt) {
        const bool isExact = opt.size() >= 2 && std::string("exact").compare(0, opt.size(), opt) == 0;
        if (!isExact || exact) {
          return engineErrorBlockRc("option " + (isExact ? std::string("exact") : opt) +
                                        " not allowed",
                                    198);
        }
        exact = true;
      }
    }
    if (!session.hasData()) {
      return engineErrorBlock("confirm " + (sub == "variable" ? std::string("variable") : sub) +
                               ": no dataset loaded");
    }
    // BUG-429: `confirm variable A B` is a VARLIST, exactly as `confirm new
    // variable` above already treats it (BS-042). The single-name reading made
    // the whole string one name and answered `variable 'A B' not found` — rc 111
    // where Stata is rc 0 — which is how a probe written against Stata stops on
    // a dataset that has both variables. Every name is checked; the FIRST
    // failure is reported, which is Stata's own order.
    // BUG-1103: a list of NAMES, not a varlist pattern -- `x*` is the name `x` and
    // then `*`, which is "* invalid name" r(198), so a wildcard or a range never
    // expands; no name at all is "'' found where varname expected" r(7) (StataBE 19.5).
    std::vector<std::string> names;
    {
      auto nameChar = [](unsigned char c) { return std::isalnum(c) || c == '_' || c >= 0x80; };
      for (size_t i = 0; i < arg.size();) {
        if (arg[i] == ' ' || arg[i] == '\t') {
          ++i;
          continue;
        }
        size_t j = i + 1;
        if (nameChar(static_cast<unsigned char>(arg[i])))
          while (j < arg.size() && nameChar(static_cast<unsigned char>(arg[j]))) ++j;
        names.push_back(arg.substr(i, j - i));
        i = j;
      }
    }
    if (names.empty()) return engineErrorBlockRc("'' found where varname expected", 7);
    // C4-L2A (D1 card plane): names + dtypes are exactly what the shadow card
    // carries, in the same closed "f64"|"factor" vocabulary Datastore::dtype()
    // answers (datastore.cpp:68 — so the numeric/string verdicts below are
    // identical between planes). Card unavailable/non-current, or the
    // PIET_NO_RESIDENT_READ escape arm → the datastore() path.
    std::vector<std::pair<std::string, std::string>> vars;  // (name, dtype), dataset order
    if (const std::vector<VarInfo>* card = session.residentReadCard()) {
      for (const auto& vi : *card) vars.emplace_back(vi.name, vi.dtype);
    } else {
      Datastore* ds = session.datastore();
      for (const auto& nm : ds->names()) vars.emplace_back(nm, ds->dtype(nm));
    }
    // BUG-1103: before the names, Stata reads the list as a VARLIST, whatever
    // `exact` says, and that pass refuses first: a ts/fv operator is r(101)
    // "factor-variable and time-series operators not allowed", an interaction
    // r(101) "interactions not allowed"; a name, pattern or
    // range end that matches nothing, an ambiguous name or `~` pattern (`x`,
    // `x~`, a lone `~`) and a range out of order are r(111); a range with no end
    // is r(198) "nothing found where name expected". A `-` may carry blanks
    // (loops::expandVarlist's rule). StataBE 19.5: `ab-zz` "variable zz not found",
    // `ab-xb` "variables out of order", `q*` "variable q* not found", `x, exact`
    // "x ambiguous abbreviation"; only then does `xa-xb` meet "- invalid name".
    {
      // The one variable `tok` names or uniquely abbreviates: its index, -1 for
      // none, -2 for several.
      auto resolve = [&vars](const std::string& tok) -> long {
        long hit = -1;
        for (size_t k = 0; k < vars.size(); ++k) {
          if (vars[k].first == tok) return static_cast<long>(k);
          if (vars[k].first.compare(0, tok.size(), tok) == 0) hit = hit == -1 ? static_cast<long>(k) : -2;
        }
        return hit;
      };
      auto refuseName = [](const std::string& tok, long at) {
        return at == -2 ? engineErrorBlockRc(tok + " ambiguous abbreviation", 111)
                        : engineErrorBlockRc("variable " + tok + " not found", 111);
      };
      std::string joined;
      for (size_t i = 0; i < arg.size(); ++i) {
        if (arg[i] == '-') {
          while (!joined.empty() && (joined.back() == ' ' || joined.back() == '\t')) joined.pop_back();
          joined.push_back('-');
          while (i + 1 < arg.size() && (arg[i + 1] == ' ' || arg[i + 1] == '\t')) ++i;
          continue;
        }
        joined.push_back(arg[i]);
      }
      std::istringstream ts(joined);
      for (std::string tok; ts >> tok;) {
        if (tok.find('.') != std::string::npos)
          return engineErrorBlockRc("factor-variable and time-series operators not allowed", 101);
        // `abc#xlong`, `abc ##xlong`: "interactions not allowed" r(101) (StataBE 19.5).
        if (tok.find('#') != std::string::npos)
          return engineErrorBlockRc("interactions not allowed", 101);
        const size_t dash = tok.find('-');
        if (dash != std::string::npos && dash > 0) {
          if (dash + 1 == tok.size())
            return engineErrorBlockRc("nothing found where name expected", 198);
          const std::string a = tok.substr(0, dash), b = tok.substr(dash + 1);
          if (!strutil::isValidStataIdentifier(a) || !strutil::isValidStataIdentifier(b)) continue;
          const long ia = resolve(a);
          if (ia < 0) return refuseName(a, ia);
          const long ib = resolve(b);
          if (ib < 0) return refuseName(b, ib);
          if (ia > ib) return engineErrorBlockRc("variables out of order", 111);
          continue;
        }
        if (tok.find_first_of("*?~") != std::string::npos) {
          std::string pat = tok;
          std::replace(pat.begin(), pat.end(), '~', '*');
          static const std::regex reWild(R"([*?])");
          if (!strutil::isValidStataIdentifier(std::regex_replace(pat, reWild, "a"))) continue;
          int hits = 0;
          for (const auto& v : vars) hits += strutil::globMatch(pat, v.first) ? 1 : 0;
          if (hits == 0) return engineErrorBlockRc("variable " + tok + " not found", 111);
          if (hits > 1 && tok.find('~') != std::string::npos)
            return engineErrorBlockRc(tok + " ambiguous abbreviation", 111);
          continue;
        }
        if (tok == "_all" || !strutil::isValidStataIdentifier(tok)) continue;
        const long at = resolve(tok);
        if (at < 0) return refuseName(tok, at);
      }
    }
    for (const std::string& one : names) {
      // BUG-1103: without `exact`, a name that is no variable stands for the one
      // variable it begins; several are "NAME ambiguous abbreviation", none
      // "variable NAME not found", both r(111). A wrong type is r(7) and quotes
      // the name as typed (StataBE 19.5).
      if (!strutil::isValidStataIdentifier(one))
        return engineErrorBlockRc(one + " invalid name", 198);
      const std::pair<std::string, std::string>* hit = nullptr;
      int prefixHits = 0;
      for (const auto& v : vars) {
        if (v.first == one) {
          hit = &v;
          prefixHits = 1;
          break;
        }
        if (!exact && v.first.size() > one.size() && v.first.compare(0, one.size(), one) == 0) {
          if (prefixHits++ == 0) hit = &v;
        }
      }
      if (prefixHits > 1) return engineErrorBlockRc(one + " ambiguous abbreviation", 111);
      if (hit == nullptr) return engineErrorBlockRc("variable " + one + " not found", 111);
      if (sub == "numeric" || sub == "string") {
        const std::string& dtype = hit->second;
        bool isStrDtype = dtype.rfind("str", 0) == 0 || dtype == "factor";
        if (sub == "numeric" && isStrDtype) {
          return engineErrorBlockRc("'" + one + "' found where numeric variable expected", 7);
        }
        if (sub == "string" && !isStrDtype) {
          return engineErrorBlockRc("'" + one + "' found where string variable expected", 7);
        }
      }
    }
    return ojson::array();
  }

  if (sub == "integer") {
    try {
      size_t pos = 0;
      double val = std::stod(arg, &pos);
      if (pos != arg.size() || val != std::trunc(val)) {
        return engineErrorBlock("confirm integer: '" + arg + "' is not an integer value");
      }
    } catch (...) {
      return engineErrorBlock("confirm integer: '" + arg + "' is not an integer value");
    }
    return ojson::array();
  }

  if (sub == "number") {
    // BS-042: `confirm number ARG` — any real number (integer or not) is
    // fine; message wording mirrors Stata's own exact text (live-probed:
    // `confirm number abc` -> "'abc' found where number expected", rc=7).
    bool ok = false;
    if (!arg.empty()) {
      try {
        size_t pos = 0;
        std::stod(arg, &pos);
        std::string rest = arg.substr(pos);
        ok = rest.find_first_not_of(" \t") == std::string::npos;
      } catch (...) {
        ok = false;
      }
    }
    if (!ok) {
      return engineErrorBlock("confirm number: '" + arg + "' found where number expected");
    }
    return ojson::array();
  }

  if (sub == "scalar") {
    // BS-042: live-probed (`confirm scalar notascalar` -> "scalar
    // notascalar not found", rc=111; existing scalar -> rc=0).
    if (!session.eval.scalars.contains(arg)) {
      return engineErrorBlock("confirm scalar: scalar '" + arg + "' not found");
    }
    return ojson::array();
  }

  if (sub == "matrix") {
    // BUG-922: `confirm matrix NAME` was an unknown subtype and answered rc 0
    // for any name -- including a program's tempname matrix after the program
    // had returned (oracle w2_prog_controlflow pc_tv_gone: r(111)). The bare
    // name reads the user matrix store; `e(name)`/`r(name)` read the class
    // stores (aer_estim_oracle.log:412-414 / :516-521, `confirm matrix e(sds)`).
    static const std::regex reClassMat(std::string(R"(^([er])\(\s*()") + eval::kRxName + R"()\s*\)$)");
    std::smatch cm;
    const eval::EvalMatrix* mx = nullptr;
    if (std::regex_match(arg, cm, reClassMat)) {
      mx = session.eval.findClassMatrix(cm[1].str()[0], cm[2].str());
    } else {
      mx = session.eval.findMatrix(arg);
    }
    if (mx == nullptr) return engineErrorBlockRc("matrix " + arg + " not found", 111);
    return ojson::array();
  }

  if (sub == "names") {
    // BS-042: `confirm names NAME [NAME ...]` (Stata alias `confirm name`
    // for the single-name form, normalised to "names" above) — each token
    // must be a legal identifier; live-probed exact Stata wording is
    // unquoted ("9def invalid name", rc=7), matched verbatim below.
    // Reports the FIRST malformed token, matching Stata's own
    // left-to-right stop-at-first-error.
    std::istringstream iss(arg);
    std::string tok;
    while (iss >> tok) {
      if (!isValidStataName(tok)) {
        return engineErrorBlock("confirm names: " + tok + " invalid name");
      }
    }
    return ojson::array();
  }

  if (sub == "existence") {
    // BS-042: `confirm existence TEXT` — Stata's real semantics here are
    // just "is there anything to confirm" (non-blank), NOT a name-format
    // check (that's `names`/`name` above) — live-probed: blank arg ->
    // "'' found where something expected", rc=6 (no existing coarseRc
    // bucket for r(6); honest-skipped exact-rc fidelity for this specific
    // blank-arg corner case — see this batch's report). Any non-blank arg
    // always succeeds.
    if (trim(arg).empty()) {
      return engineErrorBlock("confirm existence: '' found where something expected");
    }
    return ojson::array();
  }

  // Unknown confirm sub-type -- silent noop (future-proof, mirrors shims.py).
  return ojson::array();
}

// `mkdir DIRNAME [, public]` — create ONE directory level.
//
// BS-045 (BETA campaign C4): live-probed against StataNow 19.5 — real
// Stata's `mkdir` has NO parents-creation option at all (the pre-existing
// `,p` here was an INVENTED option; `help mkdir`'s only option is
// `public`). A missing parent directory is a genuine failure in real
// Stata, not silently bridged: `mkdir /tmp/nonexistent_parent/child`
// fails with the SAME message/rc as re-mkdir'ing an already-existing
// directory: "could not create directory <path>", rc=693 (oracle-pinned
// for the already-exists case; live-probed for the missing-parent case
// too — identical wording both times). `,public` makes the new directory
// world-readable+executable (chmod 0755); omitting it leaves whatever the
// platform's own umask-derived create_directory result is. Unknown
// options are rejected via the shared net (A2 idiom), matching live-probed
// Stata (`mkdir DIR, bogusopt` -> "option bogusopt not allowed", rc=198).
ojson runMkdir(const std::string& raw, const std::vector<std::string>& varlist,
               const std::vector<std::pair<std::string, eval::OptValue>>& opts,
               SessionState& session) {
  std::string path = trim(firstArgAfter(raw, "mkdir"));
  if (path.empty() && !varlist.empty()) path = joinSpaceLocal(varlist);
  if (path.empty()) return engineErrorBlock("mkdir: directory name required");

  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kMkdirKnownOpts = kVocabMkdir.known;
  if (auto err = eval::rejectUnknownOptions(opts, kMkdirKnownOpts)) {
    return ojson(*err);
  }
  bool isPublic = false;
  for (const auto& [k, v] : opts) {
    if (k == "public" && v.isBool && v.boolValue) isPublic = true;
  }

  std::string resolved = resolveUserPath(session, path);
  namespace fs = std::filesystem;
  std::error_code ec;
  // create_directory (NOT create_directories): single level only, exactly
  // like real Stata's own mkdir — a missing parent must fail, not be
  // silently bridged. Returns false (with ec cleared, not an error) for
  // BOTH "already exists" and most creation failures; the missing-parent
  // case sets ec (ENOENT) on some platforms/libstdc++ versions and merely
  // returns false on others, so both `ec` and `!created` are treated as
  // the same real-Stata "could not create directory" outcome.
  bool created = fs::create_directory(cecon::to_native_path(resolved), ec);
  if (ec || !created) {
    return engineErrorBlock("could not create directory " + resolved);
  }
  if (isPublic) {
    fs::permissions(cecon::to_native_path(resolved),
                     fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                         fs::perms::others_read | fs::perms::others_exec,
                     fs::perm_options::replace, ec);
    // Best-effort: a chmod failure doesn't invalidate the directory that
    // was already successfully created above.
  }
  return ojson::array();  // Stata: silent on success
}

// `rmdir DIRNAME` — remove ONE empty directory.
//
// BUG-180 (P2-PREP): PIET had no `rmdir` at ALL — registry.cpp registered
// mkdir/erase/rm but never this, so the command fell through the whole
// routing table to the generic unsupported_command fallback. Measured
// consequence: the cross-engine benchmark harness clears its scratch dirs
// with `capture quietly rmdir "$BENCH_ROOT/dir_`rep'"` before each timed
// `mkdir`; under PIET that line did nothing, the directories from earlier
// runs survived, and every subsequent `mkdir` answered r(693) — the
// benchmark reported mkdir failing 10/10 for a reason that was not mkdir.
//
// Semantics live-probed against StataNow 19.5 this stint (not guessed):
//   rmdir "empty_dir"        -> rc 0, silent
//   rmdir "nonempty_dir"     -> "could not remove directory <path>", rc 693
//   rmdir "no_such_dir"      -> "could not remove directory <path>", rc 693
//   rmdir "some_file.txt"    -> "could not remove directory <path>", rc 693
//   rmdir                    -> "invalid syntax", rc 198
//   rmdir DIR, anyopt        -> "options not allowed", rc 101
// Every failure mode shares ONE message and ONE rc — hence the single
// `fail` lambda below rather than a per-case wording that real Stata does
// not actually distinguish.
//
// Two deliberate notes:
//  - The is_directory() guard is load-bearing, not defensive padding:
//    std::filesystem::remove() would happily delete a plain FILE, so
//    without it `rmdir notes.txt` would silently destroy a file that real
//    Stata refuses to touch. remove() on a NON-EMPTY directory fails on
//    its own (ENOTEMPTY), which is the "empty only" half.
//  - Options: real Stata answers r(101), a bucket this codebase has
//    nowhere else. Routed through the shared reject net instead (r(198),
//    "option `X' not allowed"), which is the uniform idiom every other
//    arm here uses — a deliberate, reported deviation rather than a new
//    one-command rc bucket.
ojson runRmdir(const std::string& raw, const std::vector<std::string>& varlist,
               const std::vector<std::pair<std::string, eval::OptValue>>& opts,
               SessionState& session) {
  std::string path = trim(firstArgAfterCmd(raw));
  if (path.empty() && !varlist.empty()) path = joinSpaceLocal(varlist);
  if (path.empty()) return engineErrorBlock("rmdir: directory name required", "syntax_error");

  if (auto err = eval::rejectUnknownOptions(opts, {})) {
    return ojson(*err);
  }

  std::string resolved = resolveUserPath(session, path);
  namespace fs = std::filesystem;
  auto native = cecon::to_native_path(resolved);
  // "could not remove directory <path>" is Stata's answer to ALL of:
  // missing path, path is a file, directory not empty, permission denied.
  auto fail = [&resolved]() {
    return engineErrorBlock("could not remove directory " + resolved);
  };

  std::error_code ec;
  if (!fs::is_directory(native, ec) || ec) return fail();
  // remove() (NOT remove_all()): single EMPTY directory only, exactly like
  // real Stata — a non-empty directory must fail, never be recursively
  // erased. Returns false when nothing was removed; ec carries ENOTEMPTY.
  if (!fs::remove(native, ec) || ec) return fail();
  return ojson::array();  // Stata: silent on success
}

// `erase FILE` / `rm FILE` — delete a file. BS-045 (BETA campaign C4):
// `rm` is a genuine Stata BUILT-IN synonym of `erase` (live-probed `which
// rm` -> "built-in command: rm"), routed to this SAME handler by the
// dispatcher; firstArgAfterCmd (unlike the old firstArgAfter(raw,
// "erase")) doesn't hardcode the command word, so it extracts the path
// correctly for either spelling.
ojson runErase(const std::string& raw, const std::vector<std::string>& varlist,
               SessionState& session) {
  std::string path = trim(firstArgAfterCmd(raw));
  if (path.empty() && !varlist.empty()) path = joinSpaceLocal(varlist);
  if (path.empty()) return engineErrorBlock("erase: filename required");

  std::string resolved = resolveUserPath(session, path);
  namespace fs = std::filesystem;
  if (!fs::exists(cecon::to_native_path(resolved))) {
    // StataBE: `file sub/nofile.txt not found` r(601), the name as typed (BUG-410).
    return engineErrorBlockRc("file " + typedUserPath(path) + " not found", 601);
  }
  std::error_code ec;
  // BUG-1461: erase removes files only. fs::remove would take an empty
  // directory at rc 0; Stata refuses a directory as a file it cannot erase
  // (StataBE 19.5: "file DIR is read-only; cannot be modified or erased",
  // r(608)), and rmdir is the command that removes one.
  if (fs::is_directory(cecon::to_native_path(resolved), ec))
    return engineErrorBlockRc("file " + path + " is read-only; cannot be modified or erased", 608);
  fs::remove(cecon::to_native_path(resolved), ec);
  if (ec) return engineErrorBlock("erase: " + ec.message());
  return ojson::array();  // Stata: silent on success
}

// `timer on|off|clear|list [N]` (BS-124, BETA campaign C4) — a session-
// scoped stopwatch bank mirroring Stata's own `timer` command: 100 slots
// (1-100), each a (running?, monotonic start, accumulated total seconds,
// stop count) tuple (SessionState::timers / TimerSlot). Previously an
// honest hard-stop: "timer" sat in dispatcher.cpp's kNoDataCommands
// allow-list (correctly — timer must work with zero data loaded, same as
// the oracle's own probe) but had NO actual `if (cmd == "timer")` dispatch
// arm, so it fell through the entire routing table to the generic
// "unsupported_command" bottom fallback. Fixed by adding the arm (below,
// dispatcher.cpp) — kNoDataCommands' "timer" entry stays (removing it
// would newly break `timer clear`/`timer on` before any `use`, which real
// Stata allows).
//
// Every on/off/clear/list state-transition rule below was live-probed
// against StataNow 19.5 (not guessed):
//   - `timer off N` when N was never started: silent no-op, rc=0 (no
//     accumulation, no count increment).
//   - `timer on N` when N is ALREADY running: silent no-op (does not
//     restart the clock or double-count).
//   - `timer list N` on a slot with no data (count==0): prints nothing.
//   - `timer list` (bare): prints ONLY slots with data (count>0), in slot
//     order — not all 100.
//   - `timer list N` sets r(tN)/r(ntN) for that slot only; an empty slot
//     retires those two stale keys. Bare `timer list` refreshes all timer
//     keys while preserving unrelated r() results.
//   - slot out of 1..100: Stata's own "invalid syntax", rc=198.
// Column format is oracle-pinned EXACTLY (c-oracle-stata.log's BS-124
// block): "%4d: %9.2f / %8.0f = %12.4f" per slot (verified char-for-char
// against `   1:      0.10 /        1 =       0.1010`).
ojson runTimer(const std::string& sub, const std::optional<int>& slotArg, SessionState& session) {
  auto slotOk = [](int n) { return n >= 1 && n <= 100; };
  auto formatSlot = [](int slot, const TimerSlot& t) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%4d: %9.2f / %8.0f = %12.4f", slot, t.totalSeconds,
                  static_cast<double>(t.count),
                  t.count > 0 ? t.totalSeconds / static_cast<double>(t.count) : 0.0);
    return std::string(buf);
  };
  auto retireReturns = [&session](int slot) {
    const std::string suffix = std::to_string(slot);
    session.eval.rValues.erase("t" + suffix);
    session.eval.rValues.erase("nt" + suffix);
  };
  auto setReturns = [&session](int slot, const TimerSlot& t) {
    const std::string suffix = std::to_string(slot);
    session.eval.rValues.set("t" + suffix, t.totalSeconds);
    session.eval.rValues.set("nt" + suffix, static_cast<double>(t.count));
  };

  if (sub == "on") {
    if (!slotArg.has_value() || !slotOk(*slotArg)) {
      return engineErrorBlock("timer: invalid syntax", "syntax_error");
    }
    TimerSlot& t = session.timers[static_cast<size_t>(*slotArg)];
    if (!t.running) {
      t.running = true;
      t.startedAt = std::chrono::steady_clock::now();
    }
    return ojson::array();
  }

  if (sub == "off") {
    if (!slotArg.has_value() || !slotOk(*slotArg)) {
      return engineErrorBlock("timer: invalid syntax", "syntax_error");
    }
    TimerSlot& t = session.timers[static_cast<size_t>(*slotArg)];
    if (t.running) {
      double elapsed =
          std::chrono::duration<double>(std::chrono::steady_clock::now() - t.startedAt).count();
      t.totalSeconds += elapsed;
      t.count += 1;
      t.running = false;
    }
    return ojson::array();
  }

  if (sub == "clear") {
    if (slotArg.has_value()) {
      if (!slotOk(*slotArg)) return engineErrorBlock("timer: invalid syntax", "syntax_error");
      session.timers[static_cast<size_t>(*slotArg)] = TimerSlot{};
    } else {
      for (size_t i = 1; i < session.timers.size(); ++i) session.timers[i] = TimerSlot{};
    }
    return ojson::array();
  }

  if (sub == "list") {
    if (slotArg.has_value()) {
      if (!slotOk(*slotArg)) return engineErrorBlock("timer: invalid syntax", "syntax_error");
      const TimerSlot& t = session.timers[static_cast<size_t>(*slotArg)];
      retireReturns(*slotArg);
      if (t.count == 0) return ojson::array();  // empty slot: silent, matches Stata
      setReturns(*slotArg, t);
      ojson out;
      out["type"] = "echo";
      out["text"] = formatSlot(*slotArg, t);
      return out;
    }
    // Bare `timer list`: every slot 1..100 WITH data, in slot order.
    for (int i = 1; i <= 100; ++i) retireReturns(i);
    std::string text;
    for (int i = 1; i <= 100; ++i) {
      const TimerSlot& t = session.timers[static_cast<size_t>(i)];
      if (t.count == 0) continue;
      setReturns(i, t);
      if (!text.empty()) text += "\n";
      text += formatSlot(i, t);
    }
    if (text.empty()) return ojson::array();
    ojson out;
    out["type"] = "echo";
    out["text"] = text;
    return out;
  }

  return engineErrorBlock("timer: unrecognized sub-command" + (sub.empty() ? "" : " '" + sub + "'"),
                           "syntax_error");
}

}  // namespace piet::handlers
