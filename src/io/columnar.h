// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// cecon — memory-mapped reader for the .hbin columnar format.
// Zero-copy: columns are pointers into the mmap'd file. See tools/to_hbin.py
// for the on-disk layout.
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <unordered_map>

namespace cecon {

enum class DType { F64, Factor };

// Storage-ladder width of a view column's payload. `None` = f64 — the only shape the
// .hbin format itself carries; narrow exists only in memory (RAMDIET-2, 2026-08-15).
enum class Narrow : uint8_t { None, F32 };

// LADDER PROBE (defined in columnar.cpp). `f64()` runs ~750k times on the reference
// slice, so the gate is an inlined read of a bool set once at first use — the note
// call itself is never reached with the probe off.
extern bool g_ladder_probe_on;
void cecon_ladder_probe_note(const std::string& name, void* site);
void cecon_ladder_probe_close();
// WS-A column-touch census (same env gate discipline; defined in columnar.cpp so
// both cobra and piet_native — which compiles this TU standalone — can link it).
void cecon_coltouch_note(const std::string& name);
void cecon_coltouch_generation(int64_t ncols);

// A factor level travels in the .hbin header as a JSON string, and JSON text is
// UTF-8, so a level that is not -- substr() and str# count bytes as Stata does, so
// substr("José",1,4) is "Jos\xC3" -- could not be written at all: the save threw,
// and the session lost its working data ("disk full or I/O error"). Such a level,
// and any that begins with the marker byte \0, is written as \0 followed by its
// bytes in hex, and the reader turns it back. Every other level is written as
// itself, so a header without one is byte-for-byte what it was.
bool hbin_level_needs_escape(const std::string& level);
std::string hbin_level_escape(const std::string& level);
void hbin_level_unescape(std::string& level);  // no-op unless it starts with \0
// `text` as a JSON sidecar, reply or header key carries it: escaped as above when
// it needs it, else itself. A column label in the .hbin header, a .dta label or
// note in a sidecar and encode's levels travel this way, and hbin_level_unescape
// turns each back.
std::string hbin_level_json_text(const std::string& text);

struct Column {
    std::string name;
    DType dtype;
    const void* data = nullptr;  // points into the mmap
    int64_t nbytes = 0;
    std::vector<std::string> levels;  // factor dictionary (empty for f64)
    // RAMDIET write-side dictionary carry (2026-08-15). `levels` serialized as JSON
    // array text, so the hbin savers can splice the bytes instead of re-escaping
    // 135,603 strings on every write (measured: 87 saves = 6,400,991 level strings =
    // 703.6 ms on the reference slice, 81% of it inside `header.dump()`).
    //
    // WHY IT LIVES HERE and not on the Frame's ColumnData, which is where it was
    // tried first and failed at a 17% hit rate: a Frame is rebuilt from the file on
    // every request, so a cache on ColumnData is born invalid every time. `Column`
    // is the object that actually OWNS the strings, and `open_carried` takes the
    // previous Columns by move — so this text rides ARC1's dictionary carry for
    // free and lives exactly as long as the strings it describes.
    //
    // Valid iff `levels_text_n == (int64_t)levels.size()`; a plain empty string is
    // ambiguous (a real zero-level dictionary dumps as "[]"). Never serialized.
    mutable std::string levels_text;
    mutable int64_t levels_text_n = -1;
    // MECHOPT W2 (PATCH_VIEW): the payload generation this VIEW entry was built
    // from (frame.h g_column_generation). 0 for every real mmap-backed Dataset
    // and every pre-existing writer — only Frame::as_dataset_view stamps it,
    // and only Frame::patch_dataset_view reads it. Never serialized.
    uint64_t view_gen = 0;
    // P1-A0-FORMAT-CARRIAGE (2026-07-30): the source's Stata display format
    // ("%9.0g", "%td", …), carried in the header's OPTIONAL per-column
    // `"format"` key. EMPTY = the key was absent, which is the case for every
    // .hbin written before this key existed and for every writer that has no
    // format to state (tools/to_hbin.py) — so "" means "unknown", never
    // "%9.0g": a consumer that wants a default must supply its own (PIET's
    // VarInfo::format member-initializer already does). This is a DISPLAY
    // string only; nothing in the read path may branch on it (the date
    // rescale that does live in dta_reader, on the .dta's own fmtlist).
    std::string format;
    // NMISS-CARRY (2026-08-20): the column's MISSING COUNT, carried in the
    // header's OPTIONAL per-column `"n_missing"` key.
    //
    // Same absent-vs-present discipline as `format` above, and it is the whole
    // point: **-1 means the key was absent**, i.e. "this file predates the key
    // or was written by a tool that does not state it" — NEVER "zero missing".
    // A reader that treated absent as 0 would report a wrong missing count for
    // every pre-existing .hbin in the tree, silently, in the variable panel.
    // Absent therefore means COUNT IT YOURSELF, which is exactly what every
    // reader did before this key existed.
    //
    // Why it is worth carrying: PIET's Datastore::nMissing had to scan the whole
    // column (O(rows) isnan/codes loop) and fault the entire mmap in, once per
    // column, every time a card was rebuilt from a file. After a stateless
    // join there is no engine delta to fold, so that rebuild is unavoidable and
    // the scan is its dominant cost — measured 2026-08-20 at 45.4 s, 24% of the
    // acceptance corpus's wall, across joinby and merge. The engine already
    // knows every one of these numbers when it writes the file
    // (FrameColumn::n_missing, lazily cached and mutation-invalidated); this key
    // is simply the engine telling the reader what it already computed instead
    // of making the reader recompute it.
    int64_t n_missing = -1;
    // BUG-198 (2026-08-06): the source's Stata VARIABLE LABEL, carried in the
    // header's OPTIONAL per-column `"label"` key. Exactly the same
    // absent-vs-empty discipline as `format` above, and for the same reason:
    // EMPTY = the key was absent (every .hbin written before this key existed,
    // plus every writer with no label to state), never "the label is the empty
    // string". A consumer that wants a default supplies its own — there is no
    // fabricated value here. Display/metadata only; nothing in the read path
    // may branch on it.
    //
    // KEY ORDER IS APPEND-ONLY AND THAT IS LOAD-BEARING, not tidiness: cobra
    // serializes the header through nlohmann, whose object keys come out
    // SORTED, so a column that states neither key dumps byte-identical bytes to
    // a pre-BUG-198 writer and the freeze corpus does not move
    // ([[freeze-fixture-corpus-laws]]). Every emit site is therefore
    // conditional on non-empty.
    std::string label;
    // ALLOC-C3A-ARC2-BYTESPAN (2026-07-30): this column's `offset` field as the
    // header stated it — the byte distance from the file's data_start, i.e.
    // exactly what `data` was computed from. `data` is a pointer into a mapping
    // and is therefore NULLED by `release_columns()` (the mapping is going
    // away); the offset is a plain integer and survives, which is what lets the
    // V3 span path reuse an unchanged column's placement without re-lexing the
    // header text that states it (byte identity of that text is the proof).
    // **-1 = UNKNOWN, and that is a load-bearing sentinel, not a tidy default**:
    // a `make_view()` view's columns (contract_A §26 resident frame) and any
    // Frame-built column never came from a header at all, and 0 would read as a
    // legitimate "column starts at data_start" — every column at offset 0 would
    // pass extent validation while pointing at the wrong bytes. The span path
    // refuses to carry a descriptor whose offset it did not itself read.
    int64_t data_offset = -1;

    // LADDER PROBE (env-gated, RAMDIET-2): the storage ladder's whole viability is
    // "how many columns per compaction generation actually demand a contiguous
    // double array?" -- because a narrow column can stay narrow only until someone
    // calls THIS. It is one function behind 65 call sites, and consumers call it once
    // to take the pointer (never per element), so counting here is exact and free.
    // ---- storage ladder (RAMDIET-2) ----
    // WHY A TAG, not widening at the view boundary (what defeated attempts 1-6): the
    // probe on this accessor showed the per-generation "demand" for doubles was
    // from_dataset BORROWING spans (238 cols), while genuine wide consumption is the
    // estimators' ~3 columns — which they copy into a design matrix anyway.
    Narrow narrow = Narrow::None;

    const float* f32() const { return static_cast<const float*>(data); }
    int64_t n_values() const { return nbytes / (narrow == Narrow::F32 ? 4 : 8); }

    // Contiguous doubles. Free when the payload is doubles; for a narrow column this
    // widens ONCE into a cache living as long as the view entry (the estimators call
    // per request, so a repeat caller pays nothing after the first).
    const double* f64() const {
        if (__builtin_expect(g_ladder_probe_on, 0))
            cecon_ladder_probe_note(name, __builtin_return_address(0));
        if (narrow == Narrow::None) return static_cast<const double*>(data);
        if (wide_.size() != static_cast<std::size_t>(n_values())) {
            const float* src = f32();
            wide_.assign(src, src + n_values());
        }
        return wide_.data();
    }
  private:
    mutable std::vector<double> wide_;   // widened cache for a narrow column
  public:
    const int32_t* codes() const { return static_cast<const int32_t*>(data); }
};

// ALLOC-C3A-ARC1-CARRY (2026-07-30): descriptor carry-over, variant V2.
//
// A single-column `setcol` rewrites the header and CLONES the data blob (CoW),
// leaving every other column byte-preserved (contract_A §28; Frame::patch_hbin
// reuses each unchanged column's source `offset` and never moves `data_start`).
// The reader nonetheless pays to re-materialize every factor dictionary in the
// new header on every re-open — and those dictionaries belong, almost entirely,
// to columns the mutation did not touch (`individual_id` alone is 108,384
// levels). `open_carried` hands the PREVIOUS parse's columns back to the reader
// so their `levels` can be MOVED into place instead of rebuilt.
//
// V2 SCOPE (deliberate — see ALLOC-C2C3-DESIGN-brief.md §9): the new header is
// still SAX-parsed in full, so every field is READ FROM THE FILE and checked
// against the carried descriptor. What V2 removes is the CONSTRUCTION of the
// carried columns' level strings: a skipped `levels` array is lexed and thrown
// away (one reused lexer buffer) instead of materializing N heap strings into a
// fresh vector that is then freed. Byte-span identity proof (V3) is ARC 2.
//
// LAW: carrying is an OMISSIBLE optimization. Every uncertain branch falls back
// to `Dataset::open(path)` — which is always right, just slower — and records
// why. It never throws in place of falling back, because a throw would let an
// optimization change an answer.
enum class CarryKind { Replace, Append };

struct CarryDelta {
    CarryKind kind = CarryKind::Replace;
    std::string name;              // the one column `setcol` wrote
    DType dtype = DType::F64;      // what the caller told the engine to write
};

// Why an `open_carried` call did (not) carry. Ordered so `Carried` is 0 and any
// non-zero value is a fallback; the names map 1:1 onto the brief's §6 trigger
// list, which is the implementation checklist.
enum class CarryOutcome {
    Carried = 0,
    FallbackDisabled,      // COBRA_NO_CARRY=1 — the A/B and paired-bench OFF arm
    FallbackNoCarried,     // caller passed an empty descriptor (first mutation)
    FallbackBadDelta,      // ④ delta names no column / dtype unusable
    FallbackDeltaShape,    // ⑤ replace but name absent, or append but name present
    FallbackColumnCount,   // carried/new column counts disagree with the delta kind
    FallbackColumnName,    // positional name mismatch (patch did not reuse slots)
    FallbackLevelCount,    // a skipped dictionary changed length under us
    FallbackHeader,        // ① magic / header_len / file length
    FallbackRowCount,      // ② n_rows moved (setcol cannot do this)
    FallbackExtent,        // ③ BUG-124 extent validation rejected a column
};

const char* carry_outcome_name(CarryOutcome o);

// BUG-198 — the .hbin METADATA PATCH PLANE.
//
// One `ColMetaPatch` states what a caller wants a named column's optional
// header keys to say. `std::nullopt` = LEAVE THE KEY EXACTLY AS THE FILE HAS IT
// (present or absent); `""` = REMOVE the key; a non-empty string = set it. The
// tri-state is the whole point — a two-state (string) API cannot express
// "don't touch", which is what every column the caller has no opinion about
// needs, and collapsing it would turn an untouched column into an erased one.
struct ColMetaPatch {
    std::string name;
    std::optional<std::string> format;
    std::optional<std::string> label;
};

// Apply `patch` to the header of the .hbin at `path`, in place.
//
// WHY THIS EXISTS AT ALL (BUG-198): PIET's save plane has two hbin arms — the
// resident-frame `snapshot` (written by the worker's Frame) and the
// clonefile/buffered copy of the session's scratch head — and NEITHER can see
// PIET's own session-level format/label overrides, because those live in
// PIET's `varMeta` and never reach the engine. Patching the produced FILE is
// arm-independent by construction: whichever arm made the bytes, the metadata
// lands, and there is exactly one site to keep honest instead of two.
//
// SEMANTICS, in order of how much they matter:
//   1. NO-OP IS BYTE-EXACT. If every requested value already matches what the
//      header says, the file is not written at all and `changed` is false.
//      That is what keeps the freeze corpus from moving on the overwhelmingly
//      common save (no overrides, or overrides equal to what the writer
//      already emitted).
//   2. Payload bytes are NEVER touched. When the new header fits the existing
//      header slot the header is overwritten in place (trailing-space padded,
//      exactly as `write_hbin_full(reserve=true)` already pads — every reader
//      derives data_start from header_len, so this is invisible). Otherwise
//      the file is rebuilt into a sibling temp as {new header, verbatim copy of
//      the whole data region} and rename(2)'d over `path`: the per-column
//      `offset` fields are RELATIVE to data_start, so the data region moves as
//      one opaque block and every column keeps its placement.
//   3. A column named in `patch` that the file does not have is ignored (the
//      caller's live card and the file can legitimately differ); a column the
//      file has and the patch does not name is left alone.
//
// Throws EngineError("data_io", …) on an unreadable/​malformed file or a failed
// write, and leaves `path` untouched in that case (BUG-124/BUG-138 shape: the
// destination is never opened for write on the rebuild path).
struct MetaPatchResult {
    bool changed = false;    // false = the file already said this; nothing written
    bool rewrote = false;    // true = full rebuild (header outgrew its slot)
    int columns_changed = 0;
};
MetaPatchResult patch_hbin_meta(const std::string& path, const std::vector<ColMetaPatch>& patch);

// ALLOC-C3A-ARC2-BYTESPAN (V3, 2026-07-30): why the byte-span layer did (not)
// localize the change. This is the brief's §6 **trigger ⑥** made real, plus the
// ⑦-class "no previous bytes to compare" case that ARC2A's `CarrySource` turned
// from a hypothetical into an observable one.
//
// WHY THIS IS A SEPARATE ENUM FROM `CarryOutcome`, and why that is the honest
// shape rather than a convenience: a span-localization miss does **not** cancel
// the carry. V2 (ARC 1) is still correct and still cheaper than a full re-open,
// so the span layer falls back **to the V2 lexing path**, not to
// `Dataset::open`. Folding these values into `CarryOutcome` would break the one
// arithmetic a gate reads it with — carried + Σfallbacks = calls — by counting a
// call that carried as a fallback. The two layers are reported side by side
// (`CARRY_DIAG … outcome=carried span=fallback_span_localize`).
//
// `FallbackSpanLocalize` is the measured-real branch, not a defensive one: the
// head of every setcol chain fails it by construction, because `loaded.hbin`
// and `patch_hbin` output come from two different serializers (spacing, key
// order) whose common prefix is 2 bytes — 1/4 on the probed 4-mutation chain
// (instruments/ALLOC-C3A-ARC2-SPANPROBE-20260730.md §2). A localization-failure
// rate is therefore a number a gate can read, which is what the row's DoD asks
// for: a 100% rate would make every downstream speed claim vacuous.
//
// ARC2-EMPTYSPAN: a header whose CONTENT is byte-identical to the previous one
// is `LocalizedEmpty` — a success with zero entries to lex, not a miss. It was
// classified as `FallbackSpanLocalize` until the REMEASURE leg showed that
// shape is what a same-value `replace` produces and that calling it a failure
// made it the layer's MOST expensive branch (§5: both memcmps + the full layout
// scan, and then V2 re-lexes the whole header anyway — instr +16.2~19.1%).
enum class SpanOutcome {
    Localized = 0,
    LocalizedEmpty,       // header content byte-identical: every descriptor is the carried one
    Disabled,             // COBRA_NO_SPAN=1 — the A/B / paired-bench OFF arm
    NoPrevBytes,          // ⑦-class: no usable previous mapping (chain head, escape hatch)
    PrevHeaderBad,        // the previous bytes are not a readable .hbin header
    PrevOffsetUnknown,    // a carried descriptor has data_offset == -1 (view/Frame origin)
    FallbackSpanLocalize, // ⑥: two-ended memcmp did not localize to exactly one column entry
    IndexMismatch,        // localized, but not at the delta column's slot
    ColumnCount,          // the new header's entry count contradicts the delta kind
    RowCountMoved,        // the root `n_rows` moved (setcol cannot do that)
    DeltaEntryBad,        // the one entry we DID parse is malformed / misnamed
    ExtentRejected,       // BUG-124 extent validation rejected a column on the span path
};

const char* span_outcome_name(SpanOutcome o);

// ALLOC-C3A-ARC2A-PREVMAP: the PREVIOUS frame's mapping, still alive.
//
// ARC 1 released the previous Datastore inside CarryArm's constructor, so by the
// time `open_carried` ran the bytes it was carrying a descriptor FOR had already
// been munmap'ed — `release_columns()` nulls every `data` pointer precisely
// because the mapping is going away. That is correct for V2 (which needs only
// {name, dtype, nbytes, levels}) and fatal for V3, whose whole mechanism is a
// two-ended `memcmp` of the previous header bytes against the new ones. This
// struct is the deferral's payload: the caller (SessionState::CarryArm) keeps the
// previous Datastore alive across the carried open and hands its whole-file
// mapping in here.
//
// LIFETIME, and who owns it: non-owning, valid only for the duration of the
// `open_carried` call. The caller releases the previous mapping immediately
// afterwards (SessionState::releaseCarryPrev, which carries the rllClose timer
// with it). Nothing inside a returned Dataset may point into it.
//
// `open()` maps the WHOLE file at offset 0, so [base, base+size) is
// byte-identical to the previous file as it was opened (columnar.h map_base()
// doc) — no copy and no second open(2) is needed to get comparable bytes, which
// is why route A was chosen over "copy the header" and "re-mmap by path"
// (instruments/ALLOC-C3A-ARC2-PREBYTES-20260730.md §3).
//
// Empty (base == nullptr) is always legal and always means "no previous bytes
// available" — first mutation, deferral disabled by escape hatch, or a caller
// that does not keep the mapping. ARC2A carries the bytes and PROVES they
// arrive (carrySrcPresentCount); the span localization that consumes them is
// ARC 2's own row, so nothing here may change a value or a fallback decision.
struct CarrySource {
    const void* base = nullptr;
    size_t size = 0;

    bool usable() const { return base != nullptr && size > 0; }
};

// Owns the mmap for its lifetime.
class Dataset {
public:
    Dataset() = default;
    ~Dataset();
    Dataset(const Dataset&) = delete;
    Dataset& operator=(const Dataset&) = delete;
    Dataset(Dataset&& o) noexcept { *this = std::move(o); }
    Dataset& operator=(Dataset&& o) noexcept;

    // Throws std::runtime_error on failure.
    static Dataset open(const std::string& path);

    // Open `path` reusing the level dictionaries of `carried` (the columns of
    // the dataset this file was patched FROM), given the one-column `delta`
    // that produced it. Falls back to `open(path)` — same result, full cost —
    // whenever the carry cannot be justified, writing the reason to `why`.
    // Throws exactly what `open` throws, and only for the same reasons (a
    // genuinely unreadable file); a merely UNTRUSTWORTHY carry is a fallback,
    // never an error.
    //
    // `carried` is consumed: its `levels` are moved into the returned Dataset.
    // On the fallback path it is simply dropped (the caller must not rely on
    // it afterwards either way).
    //
    // PIET_VERIFY_DESCRIPTOR=1 re-opens the same file the honest way and
    // compares every field of every column, level strings included; a mismatch
    // aborts loudly rather than returning a plausible-looking dataset.
    //
    // ALLOC-C3A-ARC2A-PREVMAP: `src`, when usable, is the previous frame's live
    // whole-file mapping (see CarrySource). ARC2A only PROVES it arrives — it
    // takes part in no decision here, so the returned Dataset is byte-identical
    // with and without it. ARC 2's span localization is what consumes it.
    static Dataset open_carried(const std::string& path, std::vector<Column>&& carried,
                                const CarryDelta& delta, CarryOutcome* why = nullptr,
                                const CarrySource& src = {});

    // Direct-read carry instrumentation (process-wide). Datastore's
    // constructCount_ proxy cannot see this: a carried open IS a construction,
    // so a test asserting on construct counts stays GREEN while silently
    // measuring nothing about whether the carry fired. These counters are what
    // a gate must read instead.
    static int64_t carriedOpenCount() { return carried_open_count_; }
    static int64_t fullParseCount() { return full_parse_count_; }
    // Per-reason fallback tally, indexed by CarryOutcome. Only meaningful for
    // calls that went through open_carried (a plain open() bumps only
    // fullParseCount).
    static int64_t carryFallbackCount(CarryOutcome o);
    // ALLOC-C3A-ARC2A-PREVMAP, direct-read and DETERMINISTIC (load-independent):
    // how many carried opens received usable previous bytes, and the largest
    // previous mapping that was alive at the same time as a new one. The second
    // is the deferral's whole cost surface — the touched-page RSS double-count
    // the brief §4 named as a report-only item — expressed as the exact byte
    // count exposed rather than as an RSS sample that would move with load.
    // Both are ARC2A's landing evidence: 0 present-count means the enabler did
    // not actually reach open_carried, however green the gates are.
    static int64_t carrySrcPresentCount() { return carry_src_present_count_; }
    static int64_t carrySrcCoexistBytesMax() { return carry_src_coexist_bytes_max_; }
    // ALLOC-C3A-ARC2-BYTESPAN, direct-read: the localization-failure rate the
    // row's DoD demands, as two integers rather than as a ratio someone has to
    // reconstruct from a diag log. ATTEMPT counts every carried open that
    // reached the span layer at all (so `attempt - ok` is the failure count and
    // `spanOutcomeCount()` says why); OK counts the opens that actually skipped
    // the unchanged region. Zero OK with green gates means V3 never fired and
    // the run is measuring V2 — the exact silent-SKIP shape
    // [[cache-invalidates-test-proxy]] warns about, made visible.
    static int64_t spanLocalizeAttemptCount() { return span_attempt_count_; }
    static int64_t spanLocalizeOkCount() { return span_ok_count_; }
    static int64_t spanOutcomeCount(SpanOutcome o);
    static void resetCarryCounters();
    // Restore both totals to arbitrary prior values, so a diagnostic re-open
    // (PIET_VERIFY_DESCRIPTOR's full parse) can be made count-neutral instead
    // of silently inflating the fallback report it exists to check. Same shape
    // and same reason as Datastore::setConstructCount.
    static void setCarryCounters(int64_t carried, int64_t full) {
        carried_open_count_ = carried;
        full_parse_count_ = full;
    }

    // Build a NON-OWNING Dataset "view": each Column.data points at externally-owned
    // memory (in practice a resident Frame's CowVec buffers) rather than an mmap.
    // map_ stays null, so ~Dataset munmaps nothing and the CALLER must keep the
    // backing buffers alive for the view's lifetime. path_ carries the logical
    // source path (the root .hbin) for provenance / clone-patch. contract_A §26
    // (resident-frame serve session); see Frame::as_dataset_view().
    // `sorted_by` (BUG-272): the view's sort claim, same spelling as the header key
    // below — "" (the default) = no claim, which keeps every existing caller exact.
    static Dataset make_view(const std::string& path, int64_t n_rows, std::vector<Column> cols,
                             std::string sorted_by = std::string());

    int64_t n_rows() const { return n_rows_; }
    int n_cols() const { return static_cast<int>(columns_.size()); }
    const std::vector<Column>& columns() const { return columns_; }
    const std::string& path() const { return path_; }  // file this was opened from
    // BUG-272: the file's sort claim — the header's OPTIONAL root "sorted_by" key
    // (space-joined varlist, the .dta <sortlist> carried across the .hbin hop).
    // "" = the key was absent (every pre-BUG-272 file, and every file with no claim)
    // — the same absent-vs-empty discipline as Column::format/label. Metadata only:
    // nothing in the read path branches on it; Frame::borrow_columns_from consumes it.
    const std::string& sorted_by() const { return sorted_by_; }
    // True for a real mmap-backed Dataset (open()); false for a make_view() view.
    // Lets Frame::from_dataset skip the independent re-mmap for a view (§26).
    bool owns_mmap() const { return map_ != nullptr; }
    // The raw mapping, when this Dataset owns one. open() maps the WHOLE file
    // (offset 0, length = st_size), so [map_base(), map_base()+map_size()) is
    // byte-identical to the file as opened — which is what makes a content hash
    // taken from here equal to one taken by re-reading the path, without the
    // re-open (ENG-ADOPTION-AVOIDANCE). Null / 0 for a make_view() view.
    const void* map_base() const { return map_; }
    size_t map_size() const { return map_len_; }

    bool has(const std::string& name) const { return index_.count(name) > 0; }
    const Column& col(const std::string& name) const;  // throws if missing

    // Move this Dataset's columns out so their level dictionaries can be
    // carried into the next open_carried (the Dataset is left EMPTY and must
    // not be used afterwards). Every returned Column has its `data` pointer
    // NULLED: it addressed this Dataset's mapping, which the caller is about to
    // drop, and open_carried needs only {name, dtype, nbytes, levels}. Nulling
    // makes the dangling pointer impossible rather than merely unused.
    std::vector<Column> release_columns();

    // MECHOPT W2 (PATCH_VIEW). Replace a non-owning VIEW's column vector in
    // place and rebuild the name index — the write half of
    // Frame::patch_dataset_view. Guarded to views: calling this on a real
    // mmap-backed Dataset is a programming error and returns false untouched
    // (its columns describe a mapping this setter knows nothing about).
    bool reset_view_columns(std::vector<Column> cols) {
        if (map_ != nullptr) return false;
        columns_ = std::move(cols);
        index_.clear();
        for (int i = 0; i < static_cast<int>(columns_.size()); ++i)
            index_[columns_[i].name] = i;
        return true;
    }

private:
    void* map_ = nullptr;
    size_t map_len_ = 0;
    int64_t n_rows_ = 0;
    std::vector<Column> columns_;
    std::unordered_map<std::string, int> index_;
    std::string path_;
    std::string sorted_by_;  // BUG-272: root "sorted_by" header key ("" = absent)

    static inline int64_t carried_open_count_ = 0;
    static inline int64_t full_parse_count_ = 0;
    static inline int64_t carry_fallback_[16] = {};
    static inline int64_t carry_src_present_count_ = 0;
    static inline int64_t carry_src_coexist_bytes_max_ = 0;
    static inline int64_t span_attempt_count_ = 0;
    static inline int64_t span_ok_count_ = 0;
    static inline int64_t span_outcome_[16] = {};
};

}  // namespace cecon
