// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Datastore — semantic layer over an .hbin file, C++ port of
// piet/backend/piet/datastore.py. Wraps cecon::Dataset (LINKED from the
// monorepo's src/io/columnar.{h,cpp} — read-only compile reuse, per the
// Master Plan; PIET-core does not reimplement hbin IO).
//
// The .hbin header carries no Stata variable labels or display formats, so
// (mirroring the oracle) we default format = "%9.0g" and label = "" — those
// are enriched from SessionState var_meta when a .dta was loaded.
#ifndef PIET_NATIVE_DATASTORE_H
#define PIET_NATIVE_DATASTORE_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <optional>
#include <string>
#include <vector>

#include "io/columnar.h"
#include "vendor/nlohmann/json.hpp"

namespace piet {

// LevelsRef — a factor column's level dictionary, held by default as a
// non-owning VIEW into the column's own `levels` vector instead of an owned
// copy.
//
// FLOOR/A12 (this stint): Datastore::buildVarInfo used to DEEP-COPY every
// level string of every factor column into VarInfo on every card build
// (`info.levels = c.levels`, ~59 ms/build at 108K levels) — even though the
// varlist card emits only n_levels (F5 thinned the JSON in B.3.0, but the C++
// copy survived that thinning). LevelsRef replaces that owned copy with a
// pointer into the column's own `levels`, which the owning Datastore keeps
// alive for as long as any VarInfo built from it is used (varinfoCache_ lives
// on the same Datastore that owns the columns and dies with it).
//
// Safety across the by-value VarInfo copies (VarInfo is copied into
// SessionState::shadowCard_, varlistWithMeta entries, Datastore::varinfo()'s
// by-value return, dispatcher by-col views):
//   * size()/empty()/clear() are served from a CACHED count and NEVER touch
//     the pointer. The one holder that can outlive its source Datastore
//     (shadowCard_) reads only n_levels/size() and is reseeded — with empty
//     levels — by the next frame resync before any fold dereferences it, so a
//     stale view is never dereferenced.
//   * every STRING dereference (operator[], iteration, the vector conversion,
//     to_json) is reached only by consumers holding a LIVE Datastore
//     (var_detail, by-group headers, levelsof, duplicates) or by the
//     value-label remap running against the live datastore's own varlist.
// The value-label remap COPIES-ON-WRITE — it builds an owned copy and mutates
// THAT, never writing the remapped strings back through the shared view.
//
// Mechanism gate: levelStringCopies() counts every whole-dictionary owned
// install and vector conversion. selftest asserts it stays ZERO across a
// factor-bearing varlist build; reintroducing `info.levels = c.levels` routes
// through operator=(vector) and trips it (see selftest.cpp).
class LevelsRef {
 public:
  LevelsRef() = default;

  // buildVarInfo: a non-owning view over the column's level dictionary.
  static LevelsRef view(const std::vector<std::string>* levels) {
    LevelsRef r;
    r.view_ = levels;
    r.count_ = levels ? levels->size() : 0;
    return r;
  }

  // Count-only accessors — served from the cached size, NEVER dereference the
  // pointer (this is what makes a carried/stale shadowCard_ view legal).
  bool empty() const { return count_ == 0; }
  std::size_t size() const { return count_; }

  // String accessors — dereference the view/owned vector. Reached only with a
  // LIVE owning Datastore (see the class comment); callers bounds-check size()
  // first (a non-factor column's empty view is otherwise out of range).
  const std::string& operator[](std::size_t i) const { return vec()[i]; }
  std::vector<std::string>::const_iterator begin() const { return vec().begin(); }
  std::vector<std::string>::const_iterator end() const { return vec().end(); }

  // Guard (session_state.cpp): the carried dictionary contradicts its fresh
  // n_levels — drop it. Resets to the empty/non-factor state.
  void clear() {
    view_ = nullptr;
    owned_.reset();
    count_ = 0;
  }

  // Copy-on-write install of an owned (e.g. value-label-remapped) dictionary,
  // replacing any view. This is the one sanctioned way strings become owned;
  // it is ALSO the tripwire for a reintroduced `info.levels = c.levels` (the
  // deep copy this refactor removed) — that copies into the by-value param and
  // bumps the levelStringCopies() gate on the card-build path, where the
  // selftest requires it to stay 0.
  LevelsRef& operator=(std::vector<std::string> levels) {
    count_ = levels.size();
    levelStringCopies_ += static_cast<int64_t>(count_);
    owned_ = std::make_shared<const std::vector<std::string>>(std::move(levels));
    view_ = nullptr;
    return *this;
  }

  // Owning copy of the level strings — dispatcher's by-group header keeps its
  // own vector because varinfo() returns a temporary VarInfo. Live Datastore
  // only (dereferences the view).
  operator std::vector<std::string>() const {
    levelStringCopies_ += static_cast<int64_t>(count_);
    return vec();
  }

  // Selftest mechanism gate — see the class comment.
  static int64_t levelStringCopies() { return levelStringCopies_; }
  static void resetLevelStringCopies() { levelStringCopies_ = 0; }

 private:
  const std::vector<std::string>& vec() const {
    if (owned_) return *owned_;
    if (view_) return *view_;
    static const std::vector<std::string> kEmpty;
    return kEmpty;
  }

  const std::vector<std::string>* view_ = nullptr;         // non-owning view into a column's levels
  std::shared_ptr<const std::vector<std::string>> owned_;  // set only by the CoW install (operator=)
  std::size_t count_ = 0;                                  // cached size — count-only reads never deref
  static inline int64_t levelStringCopies_ = 0;            // selftest gate — see levelStringCopies()
};

// Custom nlohmann serializer (templated so it serves both json and
// ordered_json). Emits the level strings as a JSON array, byte-identical to
// the previous `out["levels"] = <std::vector<std::string>>` at the ONE site
// that serializes them (GET /api/var_detail, routes.cpp).
template <typename BasicJsonType>
inline void to_json(BasicJsonType& j, const LevelsRef& levels) {
  j = BasicJsonType::array();
  for (const auto& level : levels) j.push_back(level);
}

// VarInfo (CONTRACT.md): {name, dtype:"f64"|"factor", format?, label?,
// n_levels?, levels?, n_missing?}.
struct VarInfo {
  std::string name;
  std::string dtype;  // "f64" | "factor"
  // P1-A0B-FMT-PIET-CONSUME: kept — grep across piet/native found zero
  // consumers branching on the "%9.0g" literal as an unset-sentinel, and
  // Stata itself treats an unformatted variable as %9.0g, so the default
  // now only fires for columns the file genuinely left unstated (buildVarInfo
  // overwrites it whenever c.format is non-empty). Removing it would need a
  // nullable representation nothing downstream reads.
  std::string format = "%9.0g";
  // format-display-default-lifecycle: true only when `format` above came from
  // a file's own header (buildVarInfo, c.format non-empty) or an engine frame
  // that stated one (SessionState::seedShadowFromFrameVars). False = `format`
  // is still the member default above, which says nothing about the variable:
  // SessionState::displayFormat then derives Stata's default from the declared
  // storage type instead of answering %9.0g for a byte or a str12.
  bool formatStated = false;
  std::string label;
  std::optional<int64_t> nLevels;
  LevelsRef levels;  // non-owning view into the column's levels (A12; see LevelsRef)
  int64_t nMissing = 0;

  nlohmann::ordered_json toJson() const;
};

// One browse-window cell: number | string (factor level / value label) | null.
struct BrowseCell {
  enum class Kind { Null, Number, String } kind = Kind::Null;
  double number = 0.0;
  std::string text;

  static BrowseCell null() { return BrowseCell{}; }
  static BrowseCell num(double v) {
    BrowseCell c;
    c.kind = Kind::Number;
    c.number = v;
    return c;
  }
  static BrowseCell str(std::string v) {
    BrowseCell c;
    c.kind = Kind::String;
    c.text = std::move(v);
    return c;
  }
  nlohmann::json toJson() const;
};

// e(sample) T2 (internal note esample/SPEC.md §4.3): PIET's own
// column on the working frame, the estimation-sample mask `__piet_esample__`
// (kEsampleColumn), is HIDDEN: absent from every user-facing listing
// (Datastore::names()/varlist()/nCols(), the resident card, describe/ds/`*`/
// `_all`) and from every user-facing file writer (save/export), while explicit
// lookups (has()/col()) still answer, so a row expression that names it -- the
// e(sample) rewrite -- reads it like any column.
// DESIGN DECISION (narrower than SPEC §4.3's `__piet_` prefix): the hide is by
// exact name. The other `__piet_*` names are SCRATCH columns that live only on
// frames PIET's own code builds and then reads back by name through names()/
// varlist() -- strmutate's `__piet_tsif<k>` (BUG-370), storage_type's
// `__piet_rp_val__`/`__piet_rp_frac__` probes -- and hiding the prefix broke
// both (selftest BUG-370(a)-(d), BUG-291). None of them is ever adopted as the
// working frame, so none needs hiding; a future PERSISTENT PIET column joins
// this test. The engine's writers (apps/cmds/cmds_data.cpp) do drop the whole
// prefix, because a user file must never carry any of them. Stata names may
// begin with `_`, so a user could type `__piet_esample__`; it is then unknown to
// them (r(111)), which SPEC §7 item 6 lists.
inline constexpr const char* kEsampleColumn = "__piet_esample__";
// The qualifier columns: the row mask of an `if` the engine's f64 evaluator
// cannot read (a string function, a compare against ""), `__piet_qualif<k>__`, and
// the values of a weight EXPRESSION (`[pw=1/wf]`), `__piet_qualw<k>__` (k unique
// per install). strmutate::installConditionMask / installWeightExpression put
// one on the working frame for ONE command, which reads `(__piet_qualif<k>__!=0)`
// as its condition or the column as its weight, and the dispatcher removes it
// when that command returns. It stays on the frame a fit read (LastModel::
// dataPath), so a replay of the fit's --keepif/--weight reads the same rows.
// Hidden for the same reason as the e(sample) mask.
inline constexpr const char* kQualifierColumnPrefix = "__piet_qual";
inline bool isQualifierColumn(const std::string& name) {
  return name.rfind(kQualifierColumnPrefix, 0) == 0;
}
inline bool isHiddenColumn(const std::string& name) {
  return name == kEsampleColumn || isQualifierColumn(name);
}
// True when a condition text names a hidden column (the e(sample) mask or a
// qualifier column): such a condition reads the frame that carries the column,
// never an alias of it without the column.
inline bool namesHiddenColumn(const std::string& text) {
  return text.find(kEsampleColumn) != std::string::npos ||
         text.find(kQualifierColumnPrefix) != std::string::npos;
}
// A NEGATIVE mask cell: a data verb rebuilt the rows in a way whose Stata marker
// PIET cannot reproduce (StataBE capture w2_esample_copies, 2026-10-13): bsample
// keeps the marker on the copies its own random draw picks (X2 `m0=20|m0_diff=10`,
// an RNG PIET does not share); collapse/contract of data not
// in by-group order mark a group's first row after Stata's own (randomized-tie)
// sort (K3 first_diff=2). Every row of the rebuilt frame carries the verb's
// code, so the marker follows the data through preserve/restore and save like
// any column, and SessionState::esampleReading names the verb in its r(198).
// gcollapse/fcollapse run collapse's engine verb here, but their marker rule is
// their plugin's, which no capture has measured. joinby and `merge 1:m` keep
// the marker on the first copy of each master row (handlers.cpp
// esampleKeepFirstCopy; w2_esample_copies.log:470 X4, :596 M1); their codes
// remain for a result whose key columns that rule cannot read.
enum class EsampleLostBy : int {
  kJoinby = 1, kBsample = 2, kCollapse = 3, kContract = 4, kCollapseShim = 5, kMerge1m = 6
};
inline const char* esampleLostByVerb(double cell) {
  switch (static_cast<int>(-cell)) {
    case 1: return "joinby";
    case 2: return "bsample";
    case 3: return "collapse";
    case 4: return "contract";
    case 5: return "gcollapse/fcollapse";
    case 6: return "merge 1:m";
    default: return "a data command";
  }
}

class Datastore {
 public:
  // Throws std::runtime_error (cecon::EngineError) if the file can't be opened.
  explicit Datastore(const std::string& path);

  // ALLOC-C3A-ARC1-CARRY: open `path` reusing `carried` — the columns released
  // from the Datastore this file was patched FROM — so their factor
  // dictionaries are MOVED in rather than re-lexed out of the new header. The
  // result is indistinguishable from the single-argument ctor's: an unusable
  // carry silently becomes a full parse (see cecon::Dataset::open_carried), and
  // `why` reports which happened.
  // ALLOC-C3A-ARC2A-PREVMAP: `src` is the PREVIOUS frame's still-live whole-file
  // mapping (cecon::CarrySource) — empty when there is none. Non-owning and
  // valid only across this constructor: SessionState keeps the previous
  // Datastore alive until it returns and releases it immediately afterwards.
  Datastore(const std::string& path, std::vector<cecon::Column>&& carried,
            const cecon::CarryDelta& delta, cecon::CarryOutcome* why = nullptr,
            const cecon::CarrySource& src = {});

  // Hand this Datastore's columns to the NEXT open so their dictionaries can be
  // carried. Destructive: this Datastore is unusable afterwards and the caller
  // must drop it immediately (SessionState::CarryArm is the only caller, and it
  // does exactly that — see session_state.h).
  std::vector<cecon::Column> releaseCarry() { return ds_.release_columns(); }

  // ALLOC-C3A-ARC2A-PREVMAP: this Datastore's own mapping, as a CarrySource for
  // the NEXT open. Still valid after releaseCarry() — that hands out the columns
  // and nulls their `data`, but the mmap itself lives until this Datastore is
  // destroyed, which is exactly the window the deferral widens. Empty for a
  // Dataset with no mapping of its own (a resident-frame view).
  cecon::CarrySource carrySource() const {
    if (!ds_.owns_mmap()) return {};
    return cecon::CarrySource{ds_.map_base(), ds_.map_size()};
  }

  const std::string& path() const { return path_; }
  int64_t nRows() const { return ds_.n_rows(); }
  // The USER's columns: names_ skips hidden ones (isHiddenColumn), and so does
  // the count (`missings dropvars`' all-columns guard, the cockpit's n_cols).
  int nCols() const { return static_cast<int>(names_.size()); }
  const std::vector<std::string>& names() const { return names_; }

  // Answers for hidden columns too (isHiddenColumn): the e(sample) rewrite's
  // `__piet_esample__` reference resolves through here and through col().
  bool has(const std::string& name) const { return ds_.has(name); }
  std::string dtype(const std::string& name) const;
  int64_t nMissing(const std::string& name) const;
  // NMISS-CARRY: the raw counting loop, shared by the fallback and by the
  // PIET_VERIFY_NMISS arm so the two cannot drift into agreeing with each other
  // while both disagree with the engine.
  int64_t scanMissing(const std::string& name) const;

  // Raw column access (throws if missing).
  const cecon::Column& col(const std::string& name) const { return ds_.col(name); }

  // BUG-532: the open file's engine sort claim, the .hbin header's optional
  // root "sorted_by" key (BUG-272), space-joined; "" = no claim. Every
  // cecon::Dataset opener fills it (open, open_carried's V2/V3 routes, a
  // resident make_view), so a caller holding the Datastore never has to
  // reopen the file to read it.
  const std::string& sortedBy() const { return ds_.sorted_by(); }

  // VarInfo list (cached), one entry per column in file order.
  const std::vector<VarInfo>& varlist();

  VarInfo varinfo(const std::string& name);

  // format-display-default-lifecycle: name, dtype, format and formatStated of
  // each named column that exists (every column when `vars` is nullopt), the
  // same format fields buildVarInfo fills but without varlist()'s n_missing
  // scan -- the input SessionState::displayFormatMap needs for browse().
  std::vector<VarInfo> formatCard(const std::optional<std::vector<std::string>>& vars) const;

  // C2-F13a incremental nMissing: carry an unchanged column's missing-count
  // from the prior state so nMissing() skips its O(nrows) scan — which, on a
  // freshly-loaded mmap, faults the whole column back in from disk (the p2
  // mutation-path cost). Set by SessionState::seedNMissForPureAdd() ONLY for
  // columns cobra left byte-identical (pure-add mutations).
  void seedNMissing(std::unordered_map<std::string, int64_t> seed) {
    nMissSeed_ = std::move(seed);
  }

  // Selftest/instrumentation only: counts every nMissing() call that fell
  // through the nMissSeed_ lookup into a real full-column scan. A pinned
  // VALUE can be correct whether it came from a cheap seed or an expensive
  // scan (see selftest.cpp's card-tax regression gate) — this counter is
  // what lets a test tell the two apart and catch a future regression that
  // silently loses the carry-forward optimization while still reporting the
  // right number.
  int64_t fullScanCount() const { return fullScanCount_; }
  void resetFullScanCount() { fullScanCount_ = 0; }

  // Selftest/instrumentation only (fix/sort-validate-lazy): counts every
  // Datastore CONSTRUCTION (ctor call) process-wide. Unlike fullScanCount_
  // (a per-INSTANCE counter of internal scan activity), this must be
  // static/class-level — the question a caller asks is "how many times has
  // a FRESH Datastore been built", which no single instance can answer
  // about itself once a prior one is destroyed. Plain counter, no locking
  // (mirrors fullScanCount_'s own convention above) — selftest runs
  // single-threaded, same assumption every other SessionState/Datastore
  // counter in this codebase already makes; this counter is never read by
  // non-test code.
  static int64_t constructCount() { return constructCount_; }
  static void resetConstructCount() { constructCount_ = 0; }
  // Restore to an arbitrary prior value (not just 0). Used by
  // SessionState::liveNamedBytesPeakSample() to make a mid-run diagnostic read
  // of the frame — which may lazily (re)construct a Datastore — fully
  // count-neutral: save constructCount(), take the sample, setConstructCount()
  // back. Without an arbitrary setter a mid-run sample would inflate
  // datastore_constructs (a budgeted floor_gate counter) by every extra build.
  static void setConstructCount(int64_t n) { constructCount_ = n; }

  // BrowseResult-shaped window. `valueLabelMap` maps varname -> {code -> text}
  // for display-only value-labeled numeric columns (factor dtype uses its own
  // baked-in levels instead and ignores this map, mirroring datastore.py).
  // `formatMap` maps varname -> the Stata %fmt its cells display in. list and
  // the browse route pass SessionState::displayFormatMap (every listed column,
  // format-display-default-lifecycle); a variable absent from the map falls
  // back to the file's own format, else gets no "format" key (never a
  // fabricated default).
  nlohmann::ordered_json browse(
      int64_t offset, int64_t limit,
      const std::optional<std::vector<std::string>>& vars,
      const std::vector<std::pair<std::string, std::vector<std::pair<int, std::string>>>>&
          valueLabelMap = {},
      const std::vector<std::pair<std::string, std::string>>& formatMap = {}) const;

 private:
  std::string path_;
  cecon::Dataset ds_;
  std::vector<std::string> names_;
  std::optional<std::vector<VarInfo>> varinfoCache_;
  std::unordered_map<std::string, int64_t> nMissSeed_;  // C2-F13a: carried nMissing for unchanged cols
  mutable int64_t fullScanCount_ = 0;  // selftest instrumentation — see fullScanCount() above
  static inline int64_t constructCount_ = 0;  // selftest instrumentation — see constructCount() above

  VarInfo buildVarInfo(const std::string& name) const;
  void finishConstruct();  // shared tail of both ctors (counter, IPC billing, names_)
};

}  // namespace piet

#endif  // PIET_NATIVE_DATASTORE_H
