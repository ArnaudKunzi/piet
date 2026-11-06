// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// cecon — a mutable in-memory table for data prep (generate / keep / summarize),
// the start of the Stata-compatible data layer. Loads from an .hbin Dataset,
// applies transformations, and can write a new .hbin for the estimators.
#pragma once
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "data/cow_vec.h"
#include "io/columnar.h"

namespace cecon {

// The column payload — the f64/codes CoW buffers plus the factor level dictionary —
// bundled so it can be SHARED. Lifting it behind a shared_ptr in FrameColumn turns a
// column COPY into a refcount bump (column-granularity CoW / Arrow-style column refs,
// MAXPERF Stage 2): derived frames (order / rename / clonevar / keep-var / drop-var …)
// share every untouched column instead of deep-copying it, and a session snapshot
// (serve preserve / tempfile) is an O(columns) pointer capture. A mutation clones the
// payload first iff it is shared (see FrameColumn::detach). The factor dictionary rides
// WITH the codes (levels stay first-seen order), so sharing it is correct and expected.
//
// MAXPERF Stage 2 Wave β1: `levels` is a CowVec, not a plain vector — Frame::from_dataset
// / from_dataset_shared BORROW the Dataset's own level strings (see
// Frame::borrow_columns_from) instead of deep-copying the dictionary (a 108K-string
// factor column's level list was ~0.4s of pure copy on `use`). Same lifetime contract as
// the f64/codes borrows: the source Dataset must outlive this ColumnData until a write
// materializes it (CowVec::materialize / FrameColumn::detach's clone).
//
// Wave β2: `n_missing_cache` is a lazily-computed, invalidate-on-any-mutable-access count
// of missing values in the active buffer (see FrameColumn::n_missing / detach()).
// MECHOPT W2 (PATCH_VIEW): process-wide monotone generation counter for column
// payloads. Every ColumnData is BORN with a fresh generation (default and copy
// construction both stamp), and every mutable access bumps it (detach() is the
// single funnel all mut() paths pass through). A cached view entry records the
// generation it was built from; matching on payload POINTER alone would be
// vulnerable to allocator address reuse (drop a factor column, create another
// of the same rows — the codes buffer can land at the freed address with the
// old levels now stale) and to in-place writes on a uniquely-held column (same
// address, new bytes, and for a factor possibly new levels under the same
// dictionary size). The generation never repeats, so both holes close. Cost:
// one integer increment per column-mut acquisition — the same funnel the
// n_missing invalidation already rides.
inline uint64_t g_column_generation = 0;

// BUG-395 (2026-09-02): ColumnData::ensure_wide() is a const method that MUTATES the
// shared payload (it builds the f32->f64 cache), and morsel-parallel consumers call
// the const reader from every ThreadPool worker at once. Two workers widening the
// same column freed each other's buffer. The consumers that run parallel now read
// through F64Reader (no widening); this stripe of mutexes is the belt for any other
// concurrent const reader — 64 stripes keyed by the ColumnData address, a static in
// an inline function so every TU shares the one array.
inline std::mutex& column_wide_stripe(const void* p) {
    static std::mutex stripes[64];
    return stripes[(reinterpret_cast<std::uintptr_t>(p) >> 6) & 63u];
}

// BUG-471 (float default, T1): the Stata storage type a written column must hold
// what it would hold — `generate float x = 1/3` keeps (double)(float)(1/3), not the
// double. It is a VALUE rule applied at the store boundary (Frame::set_or_replace),
// not a dtype: the payload stays f64. Double = identity, so every caller that does
// not ask stays byte-identical. Rules: frame_detail::round_to_store (frame_internal.h).
enum class StoreType { Double, Float, Byte, Int, Long };

// Frame::generate/bygen's qualifiers besides the `if` mask (BUG-841/842).
struct GenQualifier {
    // Stata's `in a/b`: 1-based observation numbers, inclusive; in_first == 0 means
    // no range. A row outside it is not selected (like a false `if`), and group()
    // numbers its rows WITHIN the range (w2m_bymask G05 :514, `group(2) in 3/6`
    // gives . . 1 1 2 2). An `if` mask does not renumber (GRPIF, G02 :482).
    int64_t in_first = 0, in_last = 0;
    // Stata's replace: an excluded row keeps the column's current value rather than
    // becoming missing (w2m_bymask C09 :361). A new column has none, so its
    // excluded rows are missing either way.
    bool keep_unselected = false;
};

struct ColumnData {
    // ---- storage ladder (RAMDIET-2, 2026-08-15) ----
    // When `narrow_f32`, `f32` IS the payload and `f64` is only a widened cache some
    // const reader asked for; otherwise `f64` is the payload and `f32` is empty.
    // Any MUTABLE access collapses the state (F64Col::mut fills the cache, drops the
    // flag, releases f32) so a writer sees exactly one authoritative buffer.
    //
    // Attempt 7 (+23% peak) taught where the cost really was: the PROXY widened —
    // every bulk consumer (take_rows, datasignature, keep_if, sort keys) reads
    // through F64Col, so a narrow frame re-widened wholesale and held BOTH buffers.
    // The consumers are now narrow-native via F64Reader below; only genuinely
    // double-hungry readers (expression eval on named columns, the estimators' view)
    // still widen, and those touch a handful of columns per request.
    CowVec<float> f32;                // payload when narrow_f32, else empty
    bool narrow_f32 = false;
    CowVec<double> f64;               // when !is_factor (borrows src mmap until first write)
    CowVec<int32_t> codes;            // when is_factor  (borrows src mmap until first write)
    CowVec<std::string> levels;       // factor dictionary (borrows the Dataset's own strings)
    int64_t n_missing_cache = -1;     // -1 = unknown; see FrameColumn::n_missing()
    uint64_t mut_gen = ++g_column_generation;  // fresh identity at every construction
    // BUG-395: "the widened cache is complete" — published with release semantics by
    // the ONE thread that built it, so a concurrent const reader either sees the
    // finished cache or takes the stripe lock and waits. Cleared wherever the cache
    // is dropped (set_narrow_f32 / borrow_narrow_f32 assign a fresh f64).
    mutable std::atomic<bool> wide_ready{false};

    ColumnData() = default;
    ColumnData(const ColumnData& o)
        : f32(o.f32), narrow_f32(o.narrow_f32), f64(o.f64), codes(o.codes), levels(o.levels),
          n_missing_cache(o.n_missing_cache), mut_gen(++g_column_generation),
          wide_ready(o.wide_ready.load(std::memory_order_acquire)) {}
    ColumnData& operator=(const ColumnData& o) {
        f32 = o.f32; narrow_f32 = o.narrow_f32;
        f64 = o.f64; codes = o.codes; levels = o.levels;
        n_missing_cache = o.n_missing_cache;
        mut_gen = ++g_column_generation;
        wide_ready.store(o.wide_ready.load(std::memory_order_acquire), std::memory_order_release);
        return *this;
    }
    ColumnData(ColumnData&& o) noexcept
        : f32(std::move(o.f32)), narrow_f32(o.narrow_f32), f64(std::move(o.f64)),
          codes(std::move(o.codes)), levels(std::move(o.levels)),
          n_missing_cache(o.n_missing_cache), mut_gen(o.mut_gen),
          wide_ready(o.wide_ready.load(std::memory_order_acquire)) {}
    ColumnData& operator=(ColumnData&& o) noexcept {
        f32 = std::move(o.f32); narrow_f32 = o.narrow_f32;
        f64 = std::move(o.f64); codes = std::move(o.codes); levels = std::move(o.levels);
        n_missing_cache = o.n_missing_cache;
        mut_gen = o.mut_gen;
        wide_ready.store(o.wide_ready.load(std::memory_order_acquire), std::memory_order_release);
        return *this;
    }

    // Number of VALUES in the payload, whatever its width. Never widens.
    std::size_t n_values() const { return narrow_f32 ? f32.size() : f64.size(); }
    // Bytes this process is charged for, narrow-aware (borrowed buffers count 0;
    // capacity not size — see owned_payload_bytes).
    int64_t owned_bytes() const {
        if (narrow_f32)
            return (f32.borrowed() ? 0 : static_cast<int64_t>(f32.capacity()) * 4) +
                   static_cast<int64_t>(f64.capacity()) * 8;   // any stale wide cache is real memory
        return f64.borrowed() ? 0 : static_cast<int64_t>(f64.capacity()) * 8;
    }
    // Fill (or refresh) the widened cache. Const because const readers need it.
    // BUG-395: safe under concurrent const readers — fast path is one acquire load;
    // the build runs under the address stripe and publishes with a release store.
    // Parallel kernels should still prefer F64Reader (no widening, half the bytes);
    // this makes the remaining widen-on-read sites correct, not free.
    void ensure_wide() const {
        if (!narrow_f32) return;
        if (wide_ready.load(std::memory_order_acquire)) return;
        std::lock_guard<std::mutex> lk(column_wide_stripe(this));
        if (wide_ready.load(std::memory_order_relaxed)) return;
        ColumnData& self = const_cast<ColumnData&>(*this);
        if (f64.size() != f32.size()) {   // cache absent or stale
            std::vector<double> w;
            w.reserve(f32.size());
            for (std::size_t i = 0; i < f32.size(); ++i) w.push_back(static_cast<double>(f32[i]));
            self.f64 = std::move(w);
        }
        wide_ready.store(true, std::memory_order_release);
    }
};

struct FrameColumn;

// Payload proxies — present the EXACT CowVec / std::vector surface the ~150 data-layer
// sites already use (c.f64[i], c.codes.data(), c.levels.size(), c.f64 = std::move(v) …)
// over the SHARED ColumnData, so lifting the payload behind a shared_ptr needs NO site
// change — the same drop-in trick CowVec used to swap under std::vector. A const access
// reads the shared payload directly; the FIRST mutable access funnels through the
// owner's detach() (column-CoW: clone the payload iff it is shared), so a write can
// never reach a sibling frame that shares this column. ref()/mut() are defined
// out-of-line below, once FrameColumn is complete.
// Narrow-aware, NON-widening element reader — the shape every bulk consumer uses
// (take_rows, keep_if, datasignature, sort keys, n_missing). One branch per element,
// hoisted out of the CowVec state machine; never touches the wide cache.
struct F64Reader {
    const double* w = nullptr;
    const float* nf = nullptr;
    double operator[](std::size_t i) const {
        return w ? w[i] : static_cast<double>(nf[i]);
    }
    bool narrow() const { return nf != nullptr; }
};

struct F64Col {
    FrameColumn* o_;
    explicit F64Col(FrameColumn* o) : o_(o) {}
    const CowVec<double>& ref() const;
    CowVec<double>& mut();
    operator const CowVec<double>&() const { return ref(); }  // drop-in for `const CowVec<double>&`
    const double& operator[](std::size_t i) const { return ref()[i]; }
    double& operator[](std::size_t i) { return mut()[i]; }
    const double* data() const { return ref().data(); }
    double* data() { return mut().data(); }
    // NEVER widen: size/empty are questions about the payload, not the wide cache,
    // and half the engine asks them on columns nothing will read as doubles.
    std::size_t size() const;
    bool empty() const;
    F64Reader reader() const;   // non-widening bulk read (see F64Reader)
    const double* begin() const { return ref().begin(); }
    const double* end() const { return ref().end(); }
    double* begin() { return mut().begin(); }
    double* end() { return mut().end(); }
    bool borrowed() const;
    std::size_t capacity() const { return ref().capacity(); }
    void materialize() { mut().materialize(); }
    void borrow(const double* p, int64_t n) { mut().borrow(p, n); }
    // ---- ladder surface (out of line: FrameColumn is incomplete here) ----
    bool narrow() const;
    const CowVec<float>& f32_ref() const;
    // Install a narrow payload. The caller must have proved every value survives a
    // float round-trip EXACTLY and that the column carries no NaN (a missing's NaN
    // payload bits carry Stata's extended missing .a-.z, which f32 destroys).
    void set_narrow_f32(std::vector<float>&& v);
    void borrow_narrow_f32(const float* p, int64_t n);
    void resize(std::size_t n) { mut().resize(n); }
    void resize(std::size_t n, double v) { mut().resize(n, v); }
    void assign(const double* a, const double* b) { mut().assign(a, b); }
    void assign(std::size_t n, double v) { mut().assign(n, v); }
    void push_back(double v) { mut().push_back(v); }
    void reserve(std::size_t n) { mut().reserve(n); }
    void clear() { mut().clear(); }
    template <class It> void insert(const double* pos, It f, It l) { mut().insert(pos, f, l); }
    F64Col& operator=(std::vector<double>&& v) { mut() = std::move(v); return *this; }
    F64Col& operator=(const std::vector<double>& v) { mut() = v; return *this; }
    F64Col& operator=(const F64Col& o) { mut() = o.ref(); return *this; }
};

struct CodeCol {
    FrameColumn* o_;
    explicit CodeCol(FrameColumn* o) : o_(o) {}
    const CowVec<int32_t>& ref() const;
    CowVec<int32_t>& mut();
    operator const CowVec<int32_t>&() const { return ref(); }
    const int32_t& operator[](std::size_t i) const { return ref()[i]; }
    int32_t& operator[](std::size_t i) { return mut()[i]; }
    const int32_t* data() const { return ref().data(); }
    int32_t* data() { return mut().data(); }
    std::size_t size() const { return ref().size(); }
    bool empty() const { return ref().empty(); }
    const int32_t* begin() const { return ref().begin(); }
    const int32_t* end() const { return ref().end(); }
    int32_t* begin() { return mut().begin(); }
    int32_t* end() { return mut().end(); }
    bool borrowed() const { return ref().borrowed(); }
    std::size_t capacity() const { return ref().capacity(); }
    void materialize() { mut().materialize(); }
    void borrow(const int32_t* p, int64_t n) { mut().borrow(p, n); }
    void resize(std::size_t n) { mut().resize(n); }
    void resize(std::size_t n, int32_t v) { mut().resize(n, v); }
    void assign(const int32_t* a, const int32_t* b) { mut().assign(a, b); }
    void assign(std::size_t n, int32_t v) { mut().assign(n, v); }
    void push_back(int32_t v) { mut().push_back(v); }
    void reserve(std::size_t n) { mut().reserve(n); }
    void clear() { mut().clear(); }
    template <class It> void insert(const int32_t* pos, It f, It l) { mut().insert(pos, f, l); }
    CodeCol& operator=(std::vector<int32_t>&& v) { mut() = std::move(v); return *this; }
    CodeCol& operator=(const std::vector<int32_t>& v) { mut() = v; return *this; }
    CodeCol& operator=(const CodeCol& o) { mut() = o.ref(); return *this; }
};

// LevelCol mirrors F64Col/CodeCol EXACTLY (Wave β1): ref()/mut() expose the raw
// CowVec<std::string> (non-materializing reads — size()/operator[]/begin()/end() all
// read straight through, matching the f64/codes proxies), so the ~15 existing read
// sites in frame.cpp (egen group's alphabetical rank, merge unification, JSON var
// descriptors, …) keep paying zero copy for an untouched borrowed dictionary. The
// implicit conversion operator targets CowVec (not std::vector), same as F64Col/CodeCol
// — a caller that genuinely needs a `const std::vector<std::string>&` (nlohmann,
// io/columnar.h's Dataset::Column::levels, append_factor_column_coded's by-value
// parameter) must call `.ref()` explicitly, which then materializes via CowVec's own
// `operator const std::vector<T>&()` (one user-defined conversion, same idiom already
// used at the two nlohmann call sites below).
struct LevelCol {
    FrameColumn* o_;
    explicit LevelCol(FrameColumn* o) : o_(o) {}
    const CowVec<std::string>& ref() const;
    CowVec<std::string>& mut();
    operator const CowVec<std::string>&() const { return ref(); }  // drop-in for `const CowVec<std::string>&`
    const std::string& operator[](std::size_t i) const { return ref()[i]; }
    std::string& operator[](std::size_t i) { return mut()[i]; }
    const std::string* data() const { return ref().data(); }
    std::string* data() { return mut().data(); }
    std::size_t size() const { return ref().size(); }
    bool empty() const { return ref().empty(); }
    const std::string* begin() const { return ref().begin(); }
    const std::string* end() const { return ref().end(); }
    std::string* begin() { return mut().begin(); }
    std::string* end() { return mut().end(); }
    bool borrowed() const { return ref().borrowed(); }
    void materialize() { mut().materialize(); }
    void borrow(const std::string* p, int64_t n) { mut().borrow(p, n); }
    void push_back(const std::string& v) { mut().push_back(v); }
    void push_back(std::string&& v) { mut().push_back(std::move(v)); }
    void clear() { mut().clear(); }
    void resize(std::size_t n) { mut().resize(n); }
    LevelCol& operator=(std::vector<std::string>&& v) { mut() = std::move(v); return *this; }
    LevelCol& operator=(const std::vector<std::string>& v) { mut() = v; return *this; }
    LevelCol& operator=(const LevelCol& o) { mut() = o.ref(); return *this; }
};

struct FrameColumn {
    std::string name;
    bool is_factor = false;
    // Stata display format ("%9.0g", "%td", …) as it came from the source file —
    // P1-A0-FORMAT-CARRIAGE (2026-07-30, P1 ARC 0). CARRIAGE ONLY: no data-layer
    // method reads this to decide anything, so it can never change a value. It
    // rides next to `name`/`is_factor` (identity metadata) rather than inside the
    // shared ColumnData payload, because it is not a payload: two columns that
    // share bytes under column-CoW can legitimately carry different formats, and
    // a format edit must never force a payload clone through detach().
    // "" = unknown (source stated none, or the file predates the header key) —
    // NOT a synonym for "%9.0g"; whoever displays supplies its own default.
    std::string format;
    // BUG-198 (2026-08-06): the Stata VARIABLE LABEL, carried in the header's
    // optional per-column `"label"` key. Rides here for exactly the reasons
    // `format` does — identity metadata, not payload, so two columns sharing
    // bytes under column-CoW can carry different labels and a label edit never
    // forces a detach(). "" = unstated (source said none, or the file predates
    // the key), NEVER "the label is empty".
    //
    // WHY IT IS HERE AND NOT ONLY IN PIET's session maps: without it, cobra's
    // own writers would drop the key on every mutation rewrite while KEEPING
    // `format` — so a `use`+`gen`+`save` chain would preserve half the metadata
    // layer and silently discard the other half. Symmetric or not at all.
    std::string label;
    // Never null after construction. A COPY shares the source's payload (column-CoW)
    // until one side mutates; a fresh/default column gets its own unique payload.
    std::shared_ptr<ColumnData> data;
    F64Col f64;      // present the payload's CowVec<double> surface (see F64Col)
    CodeCol codes;   // present the payload's CowVec<int32_t> surface
    LevelCol levels; // present the payload's CowVec<std::string> surface (Wave β1)

    FrameColumn()
        : data(std::make_shared<ColumnData>()), f64(this), codes(this), levels(this) {}
    // NB (P1-A0): all four of these enumerate every member BY HAND (they must, to
    // re-seat the proxies on `this`), so a new member is silently dropped unless it
    // is added to each one — `format` is carried in all four below.
    FrameColumn(const FrameColumn& o)
        : name(o.name), is_factor(o.is_factor), format(o.format), label(o.label),
          data(o.data),  // SHARE the payload (column-CoW)
          f64(this), codes(this), levels(this) {}
    FrameColumn(FrameColumn&& o) noexcept
        : name(std::move(o.name)), is_factor(o.is_factor), format(std::move(o.format)),
          label(std::move(o.label)), data(std::move(o.data)),
          f64(this), codes(this), levels(this) {}
    // The proxies always name THIS column (owner = this), so assignment leaves them
    // alone and only moves/copies the identity + shared payload handle.
    FrameColumn& operator=(const FrameColumn& o) {
        name = o.name; is_factor = o.is_factor; format = o.format; label = o.label;
        data = o.data;
        return *this;
    }
    FrameColumn& operator=(FrameColumn&& o) noexcept {
        name = std::move(o.name); is_factor = o.is_factor; format = std::move(o.format);
        label = std::move(o.label);
        data = std::move(o.data);
        return *this;
    }

    // Value-clean iff the active buffer still borrows the source mmap (unchanged since
    // load); owned = dirty (mutated) or newly created. The save dispatcher patches
    // clean columns in place and only rewrites the dirty/new ones. (Meaning unchanged
    // by the sharing lift.)
    bool borrowed() const {
        if (is_factor) return data->codes.borrowed();
        return data->narrow_f32 ? data->f32.borrowed() : data->f64.borrowed();  // LADDER
    }

    // Raw pointer of the active buffer (borrowed span or owned vector) — column
    // identity for the swap/gather passthrough checks (same ptr ⇒ unchanged column,
    // and, across two frames, same ptr ⇒ the payload is shared).
    const void* payload() const {
        const ColumnData& d = *data;
        // LADDER: for a narrow column the PAYLOAD is the f32 buffer — d.f64 is only a
        // wide cache (often empty). This identity feeds adopt_resident_predecessor's
        // span matching; answering with the cache made every narrow borrow unmatchable,
        // so the swap freed the f32 buffer under the pending column (measured: id read
        // back as zeros/garbage after ANY post-narrowing mutation).
        if (is_factor) return static_cast<const void*>(d.codes.data());
        if (d.narrow_f32) return static_cast<const void*>(d.f32.data());
        return static_cast<const void*>(d.f64.data());
    }

    // Column-CoW hook: if the payload is shared, clone it so the impending write is
    // private to THIS column. Idempotent; a no-op when uniquely held (use_count==1) —
    // the common case (freshly built columns), where the existing CowVec buffer-CoW
    // then handles the borrow→owned materialize with no ColumnData copy.
    //
    // detach() is the SINGLE funnel every mutable access passes through: F64Col::mut(),
    // CodeCol::mut() and LevelCol::mut() all call it before handing out a mutable
    // reference (operator[], data(), begin()/end(), resize/assign/push_back/reserve/
    // clear/insert, and the proxies' own operator= — see F64Col/CodeCol/LevelCol
    // above). MAXPERF Stage 2 Wave β2 hangs the n_missing cache invalidation here for
    // exactly that reason: ANY mutable access invalidates, regardless of whether that
    // particular access turns out to change a value — a stale count is a correctness
    // bug (n_missing feeds PIET's variable cards), so doubt always invalidates.
    void detach() {
        if (data.use_count() > 1) data = std::make_shared<ColumnData>(*data);
        data->n_missing_cache = -1;
        // MECHOPT W2: any mutable access is a new payload identity for the
        // cached-view patcher, whether or not the clone above fired — an
        // in-place write on a uniquely-held column keeps its address and would
        // otherwise be indistinguishable from "unchanged".
        data->mut_gen = ++g_column_generation;
    }

    // Lazily-computed, cached count of missing values in the active buffer (NaN for
    // f64, negative code for factor) — MAXPERF Stage 2 Wave β2. `n_rows` is the
    // owning Frame's row count (a column doesn't know it). Invalidated at every
    // mutable access via detach() (above); safe across a column-CoW share (a copy
    // shares data, hence the cache, until one side detaches and gets its own -1).
    // ENG-ADOPTDESC-NMISS: install a KNOWN count without scanning. The only
    // legitimate caller is the adoption path seeding from the write-chain memo
    // (frame_nmiss_memo_*, below), where the count was carried across a
    // byte-identical column copy — a wrong value here is a silent-wrong variable
    // card, which is why nothing else may call it and why the memo's own guards
    // (path + size + mtime + name + dtype, all exact) are the price of using it.
    // detach() clears the cache on any mutable access, so a seeded value can never
    // outlive an edit to the column.
    void seed_n_missing(int64_t m) const {
        if (m >= 0) data->n_missing_cache = m;
    }
    // NMISS-CARRY (2026-08-20): the count IF IT IS ALREADY KNOWN, never computed.
    // -1 = cold, and the caller must not force it warm.
    //
    // This exists because the first version of the header carry called
    // n_missing() from the writer, which turned a cold cache into a full scan at
    // WRITE time to save one at READ time. Measured: the read side collapsed
    // exactly as intended (joinby's PIET half 27.5 -> 0.4 s, merge 17.9 -> 1.8),
    // and `drop|if` — 434 calls, each writing a filtered frame whose every
    // column the filter had just invalidated — went 18.5 -> 43.6 s. Net wall
    // barely moved: the scan had been RELOCATED, not removed.
    //
    // So the writer now states only what the engine already computed for its own
    // reasons, and stays silent otherwise. The reader treats silence exactly as
    // it treats a pre-2026-08-20 file: count it yourself. That makes the carry
    // pure salvage — it can save a scan and can never cause one.
    int64_t n_missing_if_known() const { return data->n_missing_cache; }
    int64_t n_missing(int64_t n_rows) const {
        if (data->n_missing_cache < 0) {
            int64_t m = 0;
            if (is_factor) {
                const CowVec<int32_t>& c = data->codes;
                for (int64_t i = 0; i < n_rows; ++i) if (c[i] < 0) ++m;
            } else if (data->narrow_f32) {
                // LADDER: `f64` is EMPTY while narrow — indexing it here was the
                // out-of-bounds read attempt 7 shipped. Count the narrow bytes.
                const CowVec<float>& f = data->f32;
                for (int64_t i = 0; i < n_rows; ++i) if (std::isnan(f[i])) ++m;
            } else {
                const CowVec<double>& f = data->f64;
                for (int64_t i = 0; i < n_rows; ++i) if (std::isnan(f[i])) ++m;
            }
            data->n_missing_cache = m;
        }
        return data->n_missing_cache;
    }
};

inline std::size_t F64Col::size() const { return o_->data->n_values(); }
inline bool F64Col::empty() const { return o_->data->n_values() == 0; }
inline bool F64Col::borrowed() const {
    return o_->data->narrow_f32 ? o_->data->f32.borrowed() : o_->data->f64.borrowed();
}
inline F64Reader F64Col::reader() const {
    const ColumnData& d = *o_->data;
    if (d.narrow_f32) return F64Reader{nullptr, d.f32.data()};
    return F64Reader{d.f64.data(), nullptr};
}
inline bool F64Col::narrow() const { return o_->data->narrow_f32; }
inline const CowVec<float>& F64Col::f32_ref() const { return o_->data->f32; }
inline void F64Col::set_narrow_f32(std::vector<float>&& v) {
    o_->detach();
    ColumnData& d = *o_->data;
    d.f32 = std::move(v);
    d.narrow_f32 = true;
    d.f64 = CowVec<double>{};   // drop any stale wide cache
    d.wide_ready.store(false, std::memory_order_release);  // BUG-395
    d.n_missing_cache = -1;
}
inline void F64Col::borrow_narrow_f32(const float* p, int64_t n) {
    o_->detach();
    ColumnData& d = *o_->data;
    d.f32.borrow(p, n);
    d.narrow_f32 = true;
    d.f64 = CowVec<double>{};
    d.wide_ready.store(false, std::memory_order_release);  // BUG-395
    d.n_missing_cache = -1;
}
inline const CowVec<double>& F64Col::ref() const {
    o_->data->ensure_wide();   // no-op unless narrow; see ColumnData for who still widens
    return o_->data->f64;
}
inline CowVec<double>& F64Col::mut() {
    o_->detach();
    ColumnData& d = *o_->data;
    if (d.narrow_f32) {   // collapse to ONE authoritative wide buffer before any write
        d.ensure_wide();
        d.narrow_f32 = false;
        d.f32 = CowVec<float>{};
    }
    return d.f64;
}
inline const CowVec<int32_t>& CodeCol::ref() const { return o_->data->codes; }
inline CowVec<int32_t>& CodeCol::mut() { o_->detach(); return o_->data->codes; }
inline const CowVec<std::string>& LevelCol::ref() const { return o_->data->levels; }
inline CowVec<std::string>& LevelCol::mut() { o_->detach(); return o_->data->levels; }

class Frame {
public:
    // Build a Frame over an mmap'd .hbin WITHOUT copying: each column BORROWS the
    // Dataset's read-only mmap span and is materialized only on first write (see
    // CowVec). The caller must keep `ds` alive until this Frame is written/destroyed
    // (guaranteed at every call site — `ds` is a function-local that outlives the
    // Frame). Records ds.path() as the source so a structure-clean save can patch it.
    static Frame from_dataset(const Dataset& ds);

    // Like from_dataset, but borrows the columns of an ALREADY-OPEN Dataset held by
    // `sp` and keeps that exact Dataset alive (src_ = sp) instead of taking a fresh
    // independent mmap. Used by the serve resident-frame session (contract_A §26) at
    // `use`: one shared root mmap backs the resident Frame and every per-request view
    // derived from it, and survives each pending→resident swap via adopt (below).
    static Frame from_dataset_shared(std::shared_ptr<const Dataset> sp);

    // A NON-OWNING Dataset over THIS Frame's live column buffers (Dataset::make_view):
    // read-only verbs / estimators can run against the resident frame with zero copy.
    // path_ = source_path(); the caller keeps this Frame alive for the view's life.
    // Rebuilt after every resident swap (buffer pointers move). contract_A §26.
    Dataset as_dataset_view() const;

    // MECHOPT W2 (PATCH_VIEW). Update an existing make_view() view of THIS frame
    // in place, rebuilding only the per-column entries whose identity actually
    // moved, and MOVING every unchanged entry (an O(1) vector move — the level
    // dictionary rides along untouched) instead of deep-copying its dictionary
    // again. as_dataset_view() copies every factor level string on every call —
    // 135,603 strings on the acceptance corpus — and the serve session calls it
    // on every committed frame run, every use and every unstash; on that corpus
    // the vast majority of those calls change a handful of columns or none.
    //
    // Returns false — caller must fall back to as_dataset_view() — when `view`
    // is not a non-owning view of this frame's current shape: wrong row count,
    // different source path, different sort claim, or a real mmap Dataset.
    // Returns true after the in-place update. A column entry is reused iff its
    // dtype, payload pointer, byte length, format and label all match — the
    // payload-pointer test is the same identity the commit's changed/added
    // delta uses (fcol_ptr), and the format/label tests catch metadata-only
    // mutations that never touch the payload. Anything else (add, drop,
    // reorder, dtype change) is handled uniformly: the new column vector is
    // assembled in resident order, unchanged entries moved, the rest rebuilt.
    bool patch_dataset_view(Dataset& view) const;

    // Before this Frame replaces `outgoing` as the resident working frame (§26 swap),
    // rescue any column this Frame still BORROWS whose bytes live in `outgoing`'s OWNED
    // buffer: steal that buffer (O(1) std::move — the heap allocation, hence the data
    // pointer, is preserved) so `outgoing` can be destroyed without the borrow
    // dangling. Columns borrowing the shared root mmap stay borrowed (the session keeps
    // the root Dataset alive; also inherited here via src_). Net swap cost is
    // O(columns actually touched) + pointer moves, not O(all columns).
    void adopt_resident_predecessor(Frame& outgoing);

    // Force the full-rewrite save path (see write_hbin). The serve session marks the
    // resident frame structure-dirty when its CUMULATIVE state (across swaps) is no
    // longer patchable onto the root, since each freshly-built pending frame's own
    // structure_dirty_ only reflects its LAST verb. contract_A §26.
    void mark_structure_dirty() { structure_dirty_ = true; }

    // The .hbin this Frame was loaded from (empty if not from_dataset). Set once at
    // load; never changes. Used by the save dispatcher (clone+patch vs full rewrite).
    const std::string& source_path() const { return src_path_; }
    // True once a verb changed row count or the column set (keep_if / drop): a
    // structure-changed Frame can no longer be patched onto its source and must be
    // fully rewritten.
    bool structure_dirty() const { return structure_dirty_; }

    // Read a delimited text file (CSV/TSV) into a Frame. Two passes: pass 1 infers
    // each column's type, pass 2 materializes. A column becomes f64 if every
    // non-missing cell parses as a number (missing = empty, ".", "NA", "N/A",
    // "NaN" -> NaN); otherwise it becomes a factor (string dictionary, first-seen
    // order, missing -> code -1). Columns named in `force_factor` are always
    // factors. Handles RFC-4180 quoting ("" escape, quoted delimiters/newlines)
    // and LF/CRLF line endings. `delim` is the field separator (',' or '\t').
    static Frame from_csv(const std::string& path, char delim,
                          const std::vector<std::string>& force_factor);

    // Read a HEADERLESS single-column CSV (one raw value per row, RFC-4180
    // quoting, same low-level record reader as from_csv) for `setcol`. dtype
    // "f64": each cell is missing (empty, ".", "NA", "N/A", "NaN", ... — the
    // same marker set from_csv uses) or must parse entirely as a number (else
    // bad_input). dtype "factor": each cell is a raw level string VERBATIM
    // (only an empty cell is missing, code -1 — "." etc. are literal levels,
    // unlike the f64 path); a fresh first-seen-order dictionary is built (same
    // convention `cobra convert --factor` produces). `expect_n` must match the
    // parsed row count EXACTLY (else bad_input) — the returned column's `.name`
    // is left unset; the caller assigns it. Never bad_input silently truncates
    // or pads a mismatched payload file onto the dataset.
    static FrameColumn read_csv_column(const std::string& path, const std::string& dtype,
                                       int64_t expect_n);

    // `setcol NAME` — patch (is_new=false) an EXISTING column NAME in place
    // with `newcol`'s payload (dtype MAY change, e.g. factor->f64 = destring;
    // every OTHER column/dictionary byte, and NAME's own position, is
    // untouched) or append (is_new=true) NAME as a brand-NEW column at the END
    // of the column order. `newcol` must already carry n_rows() values (the
    // caller validates via read_csv_column's expect_n). A NAME/is_new
    // combination that doesn't match reality — NAME already exists but
    // is_new=true, or NAME is absent but is_new=false — is bad_input. A pure
    // patch-or-append (like clonevar/encode above): the replace path reuses
    // the column's on-disk slot when the new dtype/size still fits (write_hbin
    // patch path), and safely falls back to a full rewrite otherwise.
    void setcol(const std::string& name, FrameColumn&& newcol, bool is_new);

    int64_t n_rows() const { return n_; }
    int n_cols() const { return static_cast<int>(cols_.size()); }
    // How many values a non-Double store turned into missing because the storage
    // type cannot hold them (round_to_store: `gen byte b = 300`), over this Frame's
    // life. `cobra eval` reports its own call's share as `store_overflow`: Stata's
    // "(N missing values generated)" counts the expression's missing values, not
    // these (BUG-1123).
    int64_t store_overflow() const { return store_overflow_; }
    const std::vector<FrameColumn>& columns() const { return cols_; }
    bool has(const std::string& name) const { return idx_.count(name) > 0; }
    const FrameColumn& col(const std::string& name) const;
    // ONESHOT-GETCOLS: a column SUBSET of this frame, sharing payloads.
    //
    // Exists so the serve loop can hand PIET the two or three columns an
    // expression references instead of the whole frame. That is the campaign's
    // largest single write: 154 of 350 head materializations on the reference
    // slice come from the string-mutation ENTRANCE, which flushes ~161 MB to disk
    // so PIET's row-wise evaluator can read values that are already sitting right
    // here (AFTERCARE2-WRITECENSUS, 2026-08-16).
    //
    // O(1) per column and zero-copy: a FrameColumn copy shares its ColumnData
    // shared_ptr, so a borrowed (mmap or narrow) payload stays borrowed and a
    // later write to the SOURCE column CoW-detaches rather than corrupting the
    // projection. `src_` rides along so a projection outliving its statement
    // still keeps the root mapping alive. Missing name = honest throw, never a
    // partial frame.
    Frame project(const std::vector<std::string>& names) const;
    // ONESHOT-RAMDIET A: env-gated read census (COBRA_COLTOUCH=<file>), acquisitions only.
    static void record_column_touch(const std::string& name);
    // WS-A: close a compaction generation for the COBRA_COLTOUCH census (no-op when
    // the census is off). Called from the full-frame rebuild funnels.
    static void note_materialization(int64_t ncols);

    // ONESHOT-RAMDIET A: what would Stata's storage ladder do to this frame? Applies the
    // SAME predicate the .dta writer already ships (dta_writer.cpp's kByteLo/kByteHi/…
    // + all-integral test) column by column and reports the bytes each type would need.
    // Measurement only — it decides whether the ladder is worth building, and it reads
    // values, so it is env-gated and never on a hot path.
    void type_census(const std::string& path) const;

    // ---- sort-state plane (BUG-272, closes BUG-200) ------------------------------
    //
    // The SORT CLAIM: the ordered varlist this Frame's rows are sorted by, ascending
    // (Stata `describe`'s "Sorted by:" line / the .dta <sortlist> field). EMPTY = no
    // claim, which is ALWAYS a legal state — the one invariant of this plane is that
    // a carried claim is never FALSE. A dropped true claim costs a blank "Sorted by:"
    // (today's behavior everywhere); a stale claim would let Stata's own `by`
    // machinery trust an order that does not exist, so every doubt clears.
    //
    // Sources of a claim: the .dta reader (a nonzero <sortlist> in the source file)
    // and Frame::gsort under the conditions documented there (ascending f64 keys
    // only). Carriage: the .hbin header's OPTIONAL root "sorted_by" key (space-joined
    // varlist; absent != empty, emitted only when non-empty — `label`'s append-only-
    // key discipline), read back by Dataset/borrow_columns_from. Sinks: the .dta
    // writer's <sortlist>, and the .hbin writers.
    //
    // Every row-reordering / row-mutating / column-identity transform below either
    // carries the claim with a per-site justification comment or clears it (the
    // audited default) — see the per-mutator comments in frame.cpp. Transforms that
    // RETURN a fresh Frame (append, merge, reshape, collapse, joinby, expand, cross,
    // contract, fillin, tsfill, duplicates_drop, from_csv, bulk-build) are claimless
    // by construction: a default-constructed Frame has no claim, and none of them
    // copies one in — safe by default, blank on export. One exception mints a claim
    // it can prove: merge_frames under stata_plan, for a keyed 1:1 merge left with no
    // using-only row (Stata's merge re-sorts by the keys there).
    const std::vector<std::string>& sorted_by() const { return sort_keys_; }
    // Install a claim. Keys must name existing, DISTINCT columns of this Frame —
    // anything else (a hand-edited header, a stale caller) clears instead of
    // installing, because a claim naming a non-column could never be honored by the
    // writers and "no claim" is always safe. Never throws: a malformed claim in an
    // otherwise-valid file must not fail the load.
    void set_sorted_by(std::vector<std::string> keys);
    void clear_sorted_by() { sort_keys_.clear(); }

    // Declare panel structure (id var + time var), like Stata `xtset id t`.
    // Validates both columns exist and are f64; stores them as session metadata.
    void xtset(const std::string& id_col, const std::string& time_col);
    // Declare a pure time series (no panel), like Stata `tsset t`. Same metadata
    // slot with an empty panel id — ts_apply reads a null panel pointer for it.
    void tsset(const std::string& time_col);
    // Stata `tsset/xtset ..., delta(D)`: the time step L./F./D./S. shift by. Not
    // persisted in .hbin, so callers set it per invocation (cobra `--tsdelta`).
    void set_ts_delta(double d);
    const std::string& panel_id() const { return panel_id_col_; }
    const std::string& panel_time() const { return panel_time_col_; }
    // B3(c): the tsset variable's Stata display format, when the caller stated
    // it. tin()/twithin() literals are read on this scale; empty means "no scale
    // declared", and those two functions then accept only plain numbers rather
    // than guessing. Set by `eval --tsfmt`, never inferred from column carriage.
    void set_ts_format(const std::string& f) { ts_format_ = f; }
    const std::string& ts_format() const { return ts_format_; }
    double ts_delta() const { return ts_delta_; }

    // Stata `tssmooth ma out = in, window(lags 1 leads)`: centered moving average
    // of `in` within each panel (requires xtset). TIME-AWARE — the window spans
    // [t-lags, t+leads] in the TIME variable's units (e.g. days), so date gaps
    // shrink the effective window (matches Stata, which is not observation-based).
    // Missing inputs get weight zero (denominator = count of non-missing in the
    // window). Result is NaN only when the window contains no valid value.
    void tssmooth_ma(const std::string& output_name, const std::string& input_name,
                     int lags, int leads,
                     bool include_focal = true);

    // `name = expr` — evaluate a Stata-style expression into a new (or replaced)
    // f64 column. Throws on parse/eval error or unknown variable.
    // `require_new` (BUG-112(1) engine-half): when true, rejects a `name` that
    // already exists instead of silently replacing it (Stata `generate` semantics,
    // vs. `replace`'s intentional overwrite) — default false preserves every
    // existing caller's create-or-replace behavior unchanged.
    // `mask_expr` is Stata's `if` qualifier, kept SEPARATE rather than folded into
    // the expression. PIET normally folds it (`x = cond((c),(rhs),.)`), which is
    // exactly equivalent for every function whose answer depends only on its own
    // row -- but sum() is a running total, and folding makes it accumulate over
    // rows the qualifier excluded. Empty means no qualifier.
    // `st` (BUG-471 T1): the storage type the result is rounded to on the way in —
    // see StoreType above. The default keeps the double every caller had; the egen
    // family, row_egen and bygen below take the same trailing parameter.
    // `q` (BUG-841/842): Stata's `in a/b` range, carried apart from the `if` mask,
    // and replace's rule for the rows the qualifiers exclude. See GenQualifier.
    void generate(const std::string& name, const std::string& expr, bool require_new = false,
                  const std::string& mask_expr = "", StoreType st = StoreType::Double,
                  const GenQualifier& q = GenQualifier());

    // `keep if expr` — keep rows where the expression is nonzero; a missing value is
    // nonzero, so its row is kept (Stata's `if`). Returns the number of rows kept.
    int64_t keep_if(const std::string& expr);

    void drop(const std::string& name);

    // egen by-group: `egen out = fn(expr), by(by_cols)`. expr is evaluated per row,
    // then aggregated within each by-group; the scalar is broadcast to every row of
    // the group. Empty by_cols => whole dataset is one group. Missing rules follow
    // Stata (sum of all-missing => 0; mean/median/sd of all-missing => missing;
    // sd of a singleton => missing; min/max/mean/median/count ignore missing).
    //
    // Per-row (non-broadcast) functions also routed here (Stata egen parity):
    //   Rank   = default rank() (mean/midrank ties: tied obs share the mean of the
    //            ranks they span; 1 = smallest, missing input => missing rank).
    //   Std    = standardized (x - groupmean)/groupSd, sample SD (N-1); missing input
    //            => missing; group with SD 0 or <2 non-missing => missing.
    // Broadcast (scalar-per-group) functions:
    //   Pctile = the p-th percentile (Stata pctile method) broadcast to the group;
    //            p supplied via the `pctile_p` argument (default 50).
    //   Iqr    = p75 - p25 (Stata pctile method) broadcast to the group.
    //
    // B4/W6 rank family and `var`. Oracle B4 oracle capture b
    // and _c.log, on r = 10,10,20,30,30,30,40,.,.,. — a fixture built with heavy ties
    // BECAUSE batch A's (price, rep78) had none, which made track==default and
    // maxmode==default and so proved nothing:
    //   Rank       (default) 10 -> 1.5   30 -> 5     mean/midrank, as documented above
    //   RankTrack  `track`   10 -> 1     30 -> 4     the MINIMUM rank, and EVERY tied
    //              member gets it (oracle_c: track_obs1 == track_obs2 == 1)
    //   RankField  `field`   10 -> 6     30 -> 2     DESCENDING competition rank:
    //              1 + the number of values strictly greater (1 = largest)
    //   RankUnique `unique`  10 -> 1, and the second 10 -> 2: sequential distinct
    //              ranks, ties broken by observation order (oracle_c is the capture
    //              that separates unique from track — batch B gave both members'
    //              FIRST row the same 1, so only the second member decides)
    //   Var        egenmore `var`: the SAMPLE variance (N-1), missing for a group with
    //              fewer than 2 non-missing values. oracle_c: var(r) matches
    //              `summarize`'s r(Var) = +1.0124924924925X+007 = 900/7.
    // ACCEPTED-NAMED: `var` is egenmore (SSC / corpus-bundled), NOT StataCorp. The
    // third-party ado's spelling and semantics are the contract here. Reopen if the
    // bundled _gvar.ado is ever replaced by a differently-behaving one, or if
    // StataCorp ships an official `var`.
    //   Pc         `pc()`: p*x/T per row, T the sum of the group's non-missing values
    //              and p 100 (or 1 under `prop`) -- p*x over the group's sum, so a group whose
    //              T is 0 (all missing, or all zero) is missing. Oracle
    //              w2ap_egen_family eg_pc_plain / _prop / _if, eg_pc_byp_prop.
    //   Count      over a STRING column counts its non-empty values: it is
    //              the non-missing count, and "" is a string's missing (" " is not).
    //              Oracle eg_count_byopt_str.
    enum class EgenFn { Max, Min, Mean, Median, Sum, Count, Sd, Rank, Pctile, Std, Iqr,
                        RankTrack, RankField, RankUnique, Var, Pc };
    // The option values the expression family reads, each from the egen function it belongs to:
    //   missing   sum()/total(): a group with no non-missing value is `.`, not 0
    //             (total's `missing` option; oracle eg_sum_byp_miss, eg_total_*_miss).
    //   pc_prop   pc(): a proportion, p = 1 instead of 100 (pc's `prop` option).
    //   std_mean, std_sd   std(): sd*(x - mean)/s + mean, with the mean() (default 0)
    //             and sd() (default 1) options. Oracle eg_std_plain_ms, _ms2.
    //   data_order  sum()/total()/mean() as gtools' gegen computes them: the
    //             group's sum in data order even without by(), where official egen
    //             reads summarize's exact sum (StataBE 19.5, gtools 1.x: gegen
    //             total of 1e16, twenty-two 1s and -1e16 is 0, of ten 0.1s
    //             +1.fffffffffffffX-001; egen gives 22 and 1).
    // (A constructor, not member initializers: the struct is a default argument of
    // egen_by inside the class, where clang cannot use them yet.)
    struct EgenOpts {
        bool missing, pc_prop, data_order;
        double std_mean, std_sd;
        EgenOpts()
            : missing(false), pc_prop(false), data_order(false), std_mean(0.0), std_sd(1.0) {}
    };
    // `st` rounds the result once (one generate per function); default Rank under
    // Byte/Int/Long throws bad_input (Stata widens tied ranks with replace).
    void egen_by(const std::string& out, EgenFn fn, const std::string& expr,
                 const std::vector<std::string>& by_cols, double pctile_p = 50.0,
                 StoreType st = StoreType::Double, const EgenOpts& eo = EgenOpts());

    // egen mode: `egen out = mode(x) [, by(g) minmode|maxmode|nummode(#) missing]`.
    // Oracle capture b on t = 1,1,2,2 + six missings (a deliberate TIE):
    //   default            -> MISSING for every row (Stata also warns)
    //   maxmode / minmode  -> the largest / smallest of the tied modes (2 / 1)
    //   missing            -> `.` counts as a value; here it wins with 6 and the
    //                         answer is itself missing
    // and on m = 1,1,2,2,3,3,3,4,4,. (a unique mode) -> 3.
    // `nummode(k)` picks the k-th SMALLEST mode (so nummode(1) == minmode) and yields
    // missing for a group with fewer than k modes — the egen-six capture's
    // nummode(2) is 3 where the modes are {1,3} and missing where there is only one.
    // A nummode(k) larger than the mode count of EVERY group is Stata's r(198).
    // A STRING source (a factor column named alone) takes the same rule over its text:
    // "" is its missing, values order by their bytes, and the answer is text (oracle
    // w2ap_egen_family eg_mode_byp_str). See egen_mode's definition.
    struct ModeOpts {
        int nummode = 0;            // 0 = not given
        bool minmode = false, maxmode = false;
        bool missing_as_value = false;
    };
    // No store type: egen mode replaces the requested type with the source variable's own type and copies a source value, so the result is exact.
    void egen_mode(const std::string& out, const std::string& expr,
                   const std::vector<std::string>& by_cols, const ModeOpts& mo);
    // egen_mode's string branch (the source is a factor column); `order` and `K` are
    // egen_mode's by-sort, `one_group` when there is no by().
    void egen_mode_text(const std::string& out, const FrameColumn& src,
                        const std::vector<int64_t>& order,
                        const std::vector<std::vector<double>>& K, bool one_group,
                        const ModeOpts& mo);

    // egen nvals (egenmore, ACCEPTED-NAMED — see EgenFn::Var above for the rule):
    // `egen out = nvals(varlist) [, by(g) missing]` — the count of DISTINCT values,
    // or of distinct COMBINATIONS when varlist has more than one variable
    // (egen-six: `nvals(g v)` is 5 on 5 rows with 5 distinct (g,v) pairs, the same
    // as `nvals(s)`'s 5 distinct strings). oracle_c: nvals(r) = 4 on
    // 10,10,20,30,30,30,40. Rows with any missing component are excluded unless
    // `missing`; a group with nothing left is missing.
    // `st`: Float rounds the count once; byte/int/long store it exact (Stata's replace widens).
    void egen_nvals(const std::string& out, const std::vector<std::string>& vars,
                    const std::vector<std::string>& by_cols, bool missing_as_value = false,
                    StoreType st = StoreType::Double);

    // egen concat: `egen out = concat(varlist) [, punct(s) format(%fmt) maxlength(#)]`.
    // Text result (a factor column).
    struct ConcatOpts {
        std::string punct;    // separator appended after every field ("" = none)
        std::string format;   // Stata %fmt for NUMERIC inputs ("" = string()'s %9.0g)
        int maxlength = 0;    // 0 = not given
        bool decode = false;  // refused: cobra has no value-label decode
    };
    void egen_concat(const std::string& out, const std::vector<std::string>& vars,
                     const ConcatOpts& co);

    // egen cut: `egen out = cut(var), at(numlist) | group(#) [icodes] [label]`.
    // Oracle capture b, at(0 10 20 30) on c = 0,5,10,15,20,25:
    //   0->0  5->0  10->10  15->10  20->20  25->20
    // — LEFT-CLOSED, RIGHT-OPEN intervals whose value is the LEFT EDGE, with the top
    // edge exclusive (a value >= the last edge, or below the first, is missing: the
    // egen-six capture's v=7 against at(1 2 3 5) is missing).
    //   `icodes` stores the 0-based interval number instead of the left edge.
    //   `label`  stores the icode AND attaches a value label per interval, spelled
    //            "<left edge>-" (egen-six `label list`: 0 "1-", 1 "2-", 2 "3-",
    //            3 "5-" — one label per at() value, including the top edge that no
    //            observation can reach). cobra carries that as a FACTOR column: the
    //            code is the icode and the level string is the label, which is the
    //            same (code, text) pair.
    //   `group(k)` cuts at k quantiles instead and always stores the 0-based code.
    // Stata refuses two forms here and so does this: an expression rather than a
    // variable name is r(198), and a `by` prefix is r(190).
    struct CutOpts {
        std::vector<double> at;   // explicit edges (ascending, >= 2)
        int group = 0;            // 0 = not given
        bool icodes = false, label = false;
    };
    // `st` reaches the numeric result only; the `label` form is a factor (codes).
    // Under Byte/Int/Long, integer results are stored exact (Stata's replace widens)
    // and a non-integer cut point throws bad_input.
    void egen_cut(const std::string& out, const std::string& var, const CutOpts& co,
                  StoreType st = StoreType::Double);

    // egen tag: `egen out = tag(varlist)` — 1 on the FIRST observation (in the data's
    // current order) of each distinct combination of varlist, 0 on the rest, and
    // MISSING (.) on rows where any tag variable is missing (Stata: tagged rows
    // exclude missing-key rows). One-row-per-group dedup marker.
    // No store type: _gtag.ado ignores the requested type ("byte more efficient").
    void egen_tag(const std::string& out, const std::vector<std::string>& by_cols);

    // egen group: `egen out = group(varlist)` — dense integer id 1..K over the SORTED
    // distinct combinations of varlist. Rows with any missing key get MISSING (.)
    // unless `missing` is set (Stata default: missing keys => missing group id; with
    // `missing`, missings sort last and form their own group(s)).
    void egen_group(const std::string& out, const std::vector<std::string>& by_cols,
                    bool missing_as_group = false, StoreType st = StoreType::Double);

    // egen seq: `egen out = seq() [, from(f) to(t) block(b)]`, optionally within
    // by-groups. has_to=false means to() was not given. No store type.
    void egen_seq(const std::string& out, const std::vector<std::string>& by_cols,
                  double from = 1.0, double to = 0.0, bool has_to = false,
                  int64_t block = 1);

    // egen ROW functions: `egen out = fn(varlist)` computed across the listed
    // columns within each row. `missing` is rowtotal's option, `strok` rownonmiss's.
    enum class RowFn { Mean, Sd, Total, Max, Min, Miss, NonMiss };
    void row_egen(const std::string& out, RowFn fn, const std::vector<std::string>& cols,
                  StoreType st = StoreType::Double, bool missing = false, bool strok = false);

    // classic `mkspline stub = var, cubic` — restricted cubic spline (Harrell RCS)
    // basis on RAW x. Generates stub1..stub(nk-1) (stub1 = x, then nk-2 nonlinear
    // terms). If `knots` is empty, places nk (default 5; allowed 3-7) knots at the
    // Harrell default percentiles (Stata `centile` (n+1)p) with outer-knot boundary
    // clamping; otherwise uses the supplied increasing knots. Returns the new names.
    std::vector<std::string> mkspline_cubic(const std::string& stub, const std::string& var,
                                            int nknots, std::vector<double> knots);

    // `xtile out = var, nq(k)` — assign each row to its quantile group 1..k using
    // Stata pctile cutpoints (k-1 of them); missing var => missing. Ties at a
    // cutpoint fall in the lower group (group = 1 + #cutpoints strictly below v).
    // `wvar` (optional): [aw=]/[fw=] -- the cutpoints are the weighted percentiles
    // (pctile with weights: the summarize, detail rule); a row with a missing or
    // non-positive weight is outside the sample and gets missing (xtile.ado marks
    // the weight). BUG-1015, w2as_summary_stats_family sx_xtile_n_aw.
    void xtile(const std::string& out, const std::string& var, int nq, const std::string& wvar = "");
    // BUG-371: Stata `xtile … , cutpoints(cvar)` — the bin edges come from ANOTHER
    // variable's non-missing values instead of being derived from quantiles of
    // `var`. Same grouping rule either way (group = 1 + #cutpoints strictly below),
    // so the two spellings share one implementation.
    void xtile_cuts(const std::string& out, const std::string& var,
                    const std::string& cutvar);

    // `ipolate out = yvar xvar [, by(g..)] [, epolate]` — linear interpolation of y on
    // x (within by-groups if given). Non-missing y is kept (copies through regardless of
    // x); missing y is interpolated between the nearest bracketing non-missing (x,y)
    // anchors, averaging y across tied x values first (Stata semantics) to build the
    // anchor grid. Outside the anchor x-range => missing, unless epolate (linear
    // extrapolation from the two nearest endpoints).
    void ipolate(const std::string& out, const std::string& yvar, const std::string& xvar,
                 bool epolate, const std::vector<std::string>& by);

    // `bysort by_cols (sort_cols): gen out = expr` — evaluates expr with _n, _N and
    // x[k] resolving WITHIN each by-group in (sort_cols ascending) order. x[k] is
    // missing for k<1 or k>_N (Stata out-of-bounds rule). `mask_expr` (the `if`,
    // see generate) is evaluated in the same by-group context, and a missing
    // value selects the row, as Stata's `if` does (BUG-836/837). `q` as in
    // generate; an `in` range with a non-empty by-list is refused (PIET sends a
    // range only with an empty by-list, egen.cpp runBysortGen).
    void bygen(const std::string& out, const std::string& expr,
               const std::vector<std::string>& by_cols,
               const std::vector<std::string>& sort_cols,
               const std::string& mask_expr = "", StoreType st = StoreType::Double,
               const GenQualifier& q = GenQualifier());

    struct Summary {
        int64_t n = 0;
        double sum_w = 0;  // Σw — Stata's "Weight" column; 0 on the unweighted path
        double mean = 0, sd = 0, min = 0, max = 0;
        // filled only when detail=true (Stata summarize, detail):
        bool detail = false;
        double var = 0, sum = 0, skewness = 0, kurtosis = 0;
        double p1 = 0, p5 = 0, p10 = 0, p25 = 0, p50 = 0, p75 = 0, p90 = 0, p95 = 0, p99 = 0;
    };
    // `wvar` (optional): Stata frequency weights (`[fw=wvar]`) — "" (default) is the plain
    // unweighted path, byte-identical to before wvar existed. When non-empty: each row counts
    // `w` times (a row with missing `wvar` is dropped outright, same as a missing `var` row);
    // n = Sum(w) (NOT the row count); mean = Sum(w*x)/Sum(w); sd uses the Sum(w)-1 denominator
    // (Var = Sum(w*(x-mean)^2) / (Sum(w)-1)); min/max stay the plain unweighted min/max. Every
    // included weight must be a non-negative integer (Stata: "noninteger frequency weights not
    // allowed") — a non-integer weight throws EngineError("bad_input", ...). `detail` (percentiles)
    // + a non-empty `wvar` is NOT supported (needs a replicate-based percentile algorithm) and
    // throws EngineError("bad_input", ...) rather than silently computing an unweighted percentile.
    //
    // `analytic` (Stata `[aw=wvar]`) switches the weighted path's conventions, and ONLY those:
    // n = the ROW count (not Sum(w)), sum_w = Sum(w) (Stata prints it in the "Weight" column),
    // the weights are rescaled to sum to n before the variance so Var = Sum(w*)(x-mean)²/(n-1),
    // and non-integer weights are legal. mean/min/max are computed identically either way.
    // pweight/iweight are NOT implemented here and are rejected upstream.
    //
    // `keep` (optional, length n_rows()): restricts the summary to the rows with a
    // nonzero entry -- one by()-group of tabstat, which is summarize run on each
    // group (summarize restricted to that group's rows). nullptr = every row.
    // `importance` (Stata `[iw=wvar]`): the fweight formulas (sum_w = Sum(w), Var divides
    // by Sum(w)-1) with N = the ROW count and any real weight legal.
    Summary summarize(const std::string& var, bool detail = false, const std::string& wvar = "",
                      bool analytic = false, const std::vector<char>* keep = nullptr,
                      bool importance = false) const;

    // Add (or replace) a raw f64 column of length n_rows(). Used by `predict` to
    // append fitted values / residuals computed outside the expression evaluator.
    void set_f64_column(const std::string& name, std::vector<double> v);

    // `collapse (stat) var..., by(g..)` — aggregate to one row per by-group. Each
    // spec is {stat, var}; stat ∈ mean/sum/sd/count/max/min/median/p<NN>/first/last.
    // Stata divisors: mean over non-missing; sd sample (N-1, missing if <2); sum=0 if
    // all-missing; count = #non-missing. Returns a new Frame (one row per group).
    // Avoid `stat`, which MinGW's large-file headers macro-expand to `_stat64`.
    struct CollapseSpec { std::string statistic, var, out; };  // out defaults to var if empty
    // `wvar` (optional): Stata frequency weights (`[fw=wvar]`), same semantics as
    // Frame::summarize's `wvar` above, applied PER GROUP: count = Sum(w) within the group, sum =
    // Sum(w*x), mean = Sum(w*x)/Sum(w), sd = weighted with the Sum(w)-1 within-group denominator;
    // min/max/first/last stay the plain unweighted row values (a row with missing wvar is still
    // excluded from them, same as a row with missing `var`). Grouping itself (the `by` columns)
    // is NOT affected by wvar. `median`/`p##` stats + a non-empty wvar throw EngineError
    // ("bad_input", ...) — not supported yet (needs a replicate-based percentile algorithm).
    // "" (default) is the plain unweighted path, byte-identical to before wvar existed.
    // `by` columns may be a MIX of numeric and factor: grouping is on the factor's
    // underlying int32 codes (same K-matrix cols_as_double() cast every other by-group
    // helper in this file uses), so two rows only group together if they share the
    // SAME dictionary entry. A factor by-column's OUTPUT stays a factor, with a FRESH
    // dictionary rebuilt from just the codes actually present in the G output rows, in
    // FIRST-SEEN output-row order (the same self-consistent-fresh-table convention
    // `cobra convert --factor` produces — NOT alphabetical; contrast egen_group(),
    // which intentionally alphabetizes). Numeric by-vars and the aggregated `specs`
    // columns (which still require f64 — factor stat vars remain unsupported) are
    // byte-identical to before this existed.
    // `wtype` (aw|fw|iw|pw; "" = fw) and the statistics semean, sebinomial,
    // sepoisson, rawsum, percent and iqr match Stata's collapse (aw
    // rescales the weights per group, remakew); any weighted observation with a
    // missing or zero weight is marked out before the groups form. A factor stat
    // column serves first/last/firstnm/lastnm (a fresh dictionary, like a factor
    // by-column). `stata_by_order` orders a factor by-column's groups by their
    // text, as Stata's `sort` does, instead of by first-seen dictionary code.
    Frame collapse(const std::vector<CollapseSpec>& specs, const std::vector<std::string>& by,
                   const std::string& wvar = "", const std::string& wtype = "",
                   bool stata_by_order = false) const;

    // `tsfill` — insert rows for missing time periods within each panel (per-panel
    // [min,max] gaps at step `delta`). New rows have the panel id + time set and all
    // other columns missing. Returns a new (larger) Frame sorted by (panel,time);
    // rows with a missing time stay, last in their panel. `full` (Stata `tsfill,
    // full`, BUG-897) spans every panel over the OVERALL [min,max] instead and
    // refuses a missing time.
    Frame tsfill(const std::string& timevar, const std::string& panelvar, double delta,
                 bool full = false) const;

    // Save dispatcher (public API + on-wire format unchanged). When this Frame was
    // loaded from an .hbin and only had columns added/replaced (row count and column
    // set intact) and `path` differs from the source, it CLONES the source (APFS
    // clonefile / Linux FICLONE) and patches just the dirty/new columns — otherwise it
    // does a full rewrite. A full rewrite of a fresh/structure-changed Frame stays
    // byte-identical to before; the clone+patch path is value-identical by construction.
    void write_hbin(const std::string& path) const;

    // FORMAT-CONVERSION save (import-dta / csv convert): identical data bytes, but the
    // header is padded to a 64KB-aligned data_start. Without the reservation the FIRST
    // mutation on a freshly converted file cannot grow the header in place, so it
    // full-rewrites instead of patching (the fallback is correct but forfeits the patch
    // win on gen #1). Padded headers are read-compatible with every existing reader;
    // conversion-output bytes differ from the old tight form (value-identical).
    void write_hbin_reserved(const std::string& path) const { write_hbin_full(path, true); }

    // ONESHOT-FIXMEM M1: bytes this Frame's columns hold in PRIVATE heap — the
    // measurable half of "residency". A borrowed column (still pointing into a
    // Dataset mmap) counts 0 because those pages are file-backed, physically
    // shared with every other borrower of the same file, and evictable without
    // the compressor; an owned column counts its full buffer because that is
    // memory this process must be charged for. `keep_if` converts the first kind
    // into the second for every column at once (frame.cpp's compaction loop),
    // which is why this number is the one a session census has to report.
    //
    // CAPACITY, not size (RAMDIET-2, 2026-08-15): `keep_if` compacts an owned buffer
    // forward onto itself and resizes down, which keeps the allocation. Counting the
    // logical length would report a frame shrinking to nothing while the pages it is
    // charged for stayed exactly where they were.
    //
    // Deliberately walks the columns rather than caching: a cached total would
    // need invalidating at every mutation site, and this is diagnostic code that
    // runs on an env-gated path, never in a hot loop.
    int64_t owned_payload_bytes() const {
        int64_t total = 0;
        for (const FrameColumn& c : cols_) {
            if (c.is_factor) {
                if (!c.data->codes.borrowed())
                    total += static_cast<int64_t>(c.data->codes.capacity()) * 4;
                if (!c.data->levels.borrowed()) {
                    total += static_cast<int64_t>(c.data->levels.size()) *
                             static_cast<int64_t>(sizeof(std::string));
                    for (const std::string& s : c.data->levels) total += static_cast<int64_t>(s.size());
                }
            } else {
                total += c.data->owned_bytes();   // narrow-aware; borrowed counts 0
            }
        }
        return total;
    }

    // ---- bulk-build API (used by the .dta reader + data verbs: merge, append,
    // reshape, duplicates, gsort, order, recode — anything that assembles a
    // Frame column-by-column from scratch rather than transforming an existing
    // one). Mutating a fresh/empty Frame under construction only; callers must
    // finish with finalize_after_bulk_load() before using the Frame normally
    // (that rebuilds the name->index map and validates row-count consistency).
    void set_n_rows_hint(int64_t n) { n_ = n; }
    void append_f64_column(const std::string& name, std::vector<double> v);
    // LADDER: bulk-append an already-narrow payload (gathered from a narrow source).
    void append_f64_column_narrow(const std::string& name, std::vector<float> v);
    // `empty_is_missing` selects the empty-string convention: true (default) matches
    // Frame::from_csv (CSV unquoted-empty-field -> missing, code -1, matches
    // pandas.factorize's use_na_sentinel convention) and is what every existing
    // caller (test fixtures, generic bulk-build use) expects. The native `.dta`
    // reader (src/io/dta_reader.cpp) passes `false`: Stata's `""` string value is
    // genuine, non-missing data distinct from `.` (verified vs pandas.read_stata +
    // live StataNow — BUG-024), so it must survive as its own factor level there.
    void append_factor_column(const std::string& name, const std::vector<std::string>& values,
                              bool empty_is_missing = true);
    void append_factor_column_coded(const std::string& name, std::vector<int32_t> codes,
                                    std::vector<std::string> levels);
    void finalize_after_bulk_load() { reindex(); }
    // Mutable column access for in-place post-processing (e.g. the .dta reader's
    // date-format rescale pass). Throws if `name` does not exist.
    FrameColumn& mutable_col(const std::string& name);

    // BUG-363 (2026-08-22): carry identity metadata onto the column just
    // appended. Every row-gather builder (take_rows, joinby, merge_frames,
    // cross, fillin, contract) rebuilds a column from VALUES and so silently
    // dropped `format` and `label` — a %9s string column came back %9.0g, and
    // eight freeze fixtures were pinning the loss. Stata changes no display
    // format across any of these verbs.
    //
    // LAST-APPENDED, never by name: joinby appends using-side columns that may
    // collide with a master name, and col()/mutable_col() both throw on a miss
    // and return the FIRST match, so a name lookup here either kills the join
    // or writes the wrong column (the same reason seed_n_missing's call sites
    // carry that warning).
    void carry_identity_to_last(const FrameColumn& src) {
        if (cols_.empty()) return;
        cols_.back().format = src.format;
        cols_.back().label = src.label;
    }

    // ---- generic transform verbs (MI-066 data verbs; Stata-parity semantics
    // documented next to each .cpp implementation) ----

    // `gsort [-]v1 [-]v2 ...` — stable multi-key sort, ascending unless the key
    // has a leading '-' (descending). Factor columns sort by their underlying
    // int32 code (matches PIET's gsort_op.py, which casts every column
    // including factors to float64 before np.lexsort).
    Frame gsort(const std::vector<std::string>& keys, const std::vector<bool>& ascending) const;

    // `order VARLIST [,first|last|before(V)|after(V)]` — reorder columns only;
    // row data and dtypes are untouched. first is the default when no
    // before/after/last spec: prepend the VARLIST block, preserving its
    // relative order; the remaining columns keep their existing relative order.
    // The rvalue overload steals the columns instead of deep-copying them — a
    // pure metadata permutation needs no data copy when the source is expiring.
    Frame order(const std::vector<std::string>& varlist, const std::string& mode,
               const std::string& anchor) const&;
    Frame order(const std::vector<std::string>& varlist, const std::string& mode,
               const std::string& anchor) &&;

    // `rename old new [, old2 new2 ...]` (pairs = {old,new}) — relabels columns
    // in place; every value/dtype/factor-dictionary byte is untouched (only the
    // FrameColumn::name metadata changes). Validated ATOMICALLY before any name
    // is written: every `old` must exist (missing_column) and every `new` must
    // not collide with a FINAL survivor (a current column NOT itself being
    // renamed away — so chains/swaps like old=a,new=b + old=b,new=a are legal;
    // a genuine collision or an empty new name is bad_input). Every `old`
    // lookup resolves against the PRE-rename column set, so a multi-pair batch
    // applies as if simultaneous. Mutates in place and marks structure_dirty
    // (mirrors drop()/order()'s rvalue overload): a column-identity change
    // can't be cleanly patched onto the source .hbin.
    void rename(const std::vector<std::pair<std::string, std::string>>& pairs);

    // `clonevar dst = src [, dst2 = src2 ...]` (pairs = {src,dst}) — appends an
    // EXACT duplicate of `src` (same dtype; a factor clone shares the source's
    // level dictionary, column-CoW, until either side is later written) as a
    // NEW column named `dst` at the END of the column order; `src` is left
    // untouched. Validated atomically: every `src` must exist and every `dst`
    // must be fresh (not an existing column, not reused within this same
    // batch) before any column is appended. A pure column APPEND (like
    // `gen`/`egen`) — unlike rename()/order() it never changes an existing
    // column's identity, so it stays patch-eligible for write_hbin.
    void clonevar(const std::vector<std::pair<std::string, std::string>>& pairs);

    // `encode NEW = SRC` (Stata `encode SRC, generate(NEW)`) — appends a NEW
    // f64 column of 1-based codes assigned by SRC's ALPHABETICAL level-string
    // order (Stata semantics — NOT the factor's stored first-seen dictionary
    // order). `src` must already be a factor column (else bad_input); `dst`
    // must not already exist (bad_input). A level string that is EMPTY, or a
    // row where SRC is missing, stays missing in NEW; an unused dictionary
    // entry (present in the level table but never actually observed in SRC)
    // is excluded from the numbering, matching Stata's "distinct values
    // present in the data" rule. Returns the alphabetically-ordered list of
    // the distinct, used, non-empty level strings (same order as codes 1..K)
    // so a caller (PIET) can attach value labels directly. A pure column
    // APPEND, same patch-eligibility rationale as clonevar() above.
    std::vector<std::string> encode(const std::string& src, const std::string& dst);

    // `append using FILE` — column-union row concat (this = master, `using` =
    // second Frame). f64+f64 stays f64; factor+factor unifies dictionaries;
    // f64+factor (either direction) promotes both sides to f64 codes (matches
    // PIET append.py). A column present in only one side is padded with
    // missing on the other. Row count = n_rows() + using.n_rows().
    Frame append(const Frame& usingf) const;

    // ENG-APPEND streaming twin of `append(usingf).write_hbin(out)` for the one-shot
    // `--out` path. The append RESULT Frame carries no src_path_, so its write_hbin()
    // always lands on the tight write_hbin_full(reserve=false) branch — this emits
    // byte-for-byte that SAME file WITHOUT ever materializing the (nm+nu)-row result
    // Frame (the 786MB full-result-column copy). Each union column is streamed
    // master-part ++ using-part straight from the two borrowed source mmaps; the only
    // heap temporaries are per-column and bounded by max(nm,nu) (factor code remap,
    // mixed→f64 promotion, NA/-1 pad runs), never the full result. Value-identical,
    // golden-neutral. Non-resident write path only; resident/serve still stages the
    // real Frame via commit_frame (that path does no write to optimize).
    void write_hbin_append(const Frame& usingf, const std::string& path) const;

    // `joinby k1 [k2 ...] using FILE` (Stata's BARE form only — no
    // unmatched()/update/updatemiss/nolabel; this = master, `usingf` = using).
    // Oracle: piet/native/src/handlers_data_verbs.cpp runJoinby (~2518+), doc
    // comment at piet/native/include/piet/handlers.h:444. For every master
    // row, emits one output row per USING row sharing ALL key values (the
    // full within-key-group Cartesian cross for repeated keys on either side
    // — Stata joinby's own documented behavior, DISTINCT from `merge, m:m`'s
    // row-order-paired semantics, which this does NOT implement); a master
    // row with zero using matches is DROPPED (unmatched(none), the only mode
    // this implements) — unlike merge_frames, joinby never emits a
    // master-only or using-only row.
    //
    // Key matching: f64 keys compare by VALUE (two missing keys — NaN==NaN —
    // match each other, via the same RowKey/make_key machinery merge_frames
    // uses below); factor keys compare by LEVEL STRING through a unified
    // table (master-first, then using-only levels appended — same convention
    // as merge_frames/append above), since the two files' own int32 code
    // spaces are unrelated (level i of master has no relation to level i of
    // using). A key that is factor on one side and f64 on the other can NEVER
    // produce a matching pair for ANY row combination: PIET's own composite
    // join-key string prefixes every token by dtype before hashing
    // (joinbyRowKeys, handlers_data_verbs.cpp:2471-2514), so a mismatched
    // column's tokens differ on every row, forcing zero matches for the WHOLE
    // composite key — reproduced here as an empty-result short-circuit,
    // NOT a thrown error (unlike merge_frames, which hard-errors on this same
    // factor-vs-numeric condition; joinby's own oracle does not reject it).
    //
    // Column order: master's columns (master's own order) + using's NON-KEY
    // columns (using's own order). A non-key column NAME present on BOTH
    // sides is bad_input (ambiguous under joinby's no-update/no-replace bare
    // join — mirrors the oracle's own column-conflict reject). A key absent
    // from EITHER side, or an empty key list, is bad_input.
    //
    // Output values: every column is GATHERED fresh over the surviving
    // (matched) rows only — master-side columns indexed by the matched
    // master row, using-side columns by the matched using row. A factor
    // OUTPUT column gets a BRAND-NEW dictionary rebuilt in first-seen
    // OUTPUT-row order (same "self-consistent fresh table" convention as
    // collapse()'s factor by-vars above and `cobra convert --factor`) — NOT a
    // copy of the source's own (possibly larger/differently-ordered)
    // dictionary, since a row that doesn't survive the join never
    // contributes a value and must not leave an unused level behind. This
    // matches what runJoinby's CSV+`cobra convert` round-trip actually
    // produces (Frame::from_csv rebuilds every factor dictionary from
    // scratch, per column, in row-scan order — frame.cpp ~899-925).
    // `unmatched` (joinby.ado UnMatch: "none" (default) | "master" | "using" |
    // "both") keeps the master rows with no using match (code 1) and/or the
    // using rows whose key the master lacks (code 2, keys from the using, every
    // other master column missing). `marker`, when non-empty, adds the byte
    // 1/2/3 column after the master's columns and before the using's.
    // unmatched none with no marker is the bare join, byte-identical.
    Frame joinby(const Frame& usingf, const std::vector<std::string>& keys,
                 const std::string& unmatched = "", const std::string& marker = "") const;

    // --- P1 wave data verbs (Stata `expand` / `fillin` / `cross` / `contract`).
    // Every semantic below is oracle-pinned against StataBE 19.5 on the P1
    // wave fixture (2026-08-04); see each cmd_* banner in apps/cmds/cmds_data.cpp.

    // `expand n` — replace each observation with n copies (n<=1 leaves the data
    // unchanged; Stata counts the ADDED rows, not the total). The copies are
    // APPENDED after the original block, preserving the original row order at
    // the head — Stata's own documented ordering.
    Frame expand(int64_t n, int64_t* n_added_out) const;

    // `fillin v1 v2 [...]` — rectangularize: add the rows needed so that every
    // combination of the OBSERVED values of the listed variables is present.
    // New rows carry missing in every other column (a factor's missing code),
    // and a `_fillin` column marks them 1 (0 for pre-existing rows). An
    // existing `_fillin` is bad_input, matching Stata.
    Frame fillin(const std::vector<std::string>& varlist, int64_t* n_added_out) const;

    // `cross using FILE` — the full Cartesian product of the two datasets
    // (n_master x n_using rows). Column set = master's columns plus the using
    // columns whose names the master does NOT already have: a shared name is
    // taken from the MASTER and the using copy dropped (oracle-pinned — the P1
    // fixture crosses datasets sharing id/cluster/stratum/group and only
    // `region_using` is added). Row order is cross.ado's: every master row beside
    // using row 1, then the other pairings master by master (Frame::cross).
    Frame cross(const Frame& usingf) const;

    // `contract varlist` — collapse to the distinct combinations of varlist
    // with their frequencies in a new `_freq` column. The result holds ONLY
    // varlist + `_freq`, sorted by varlist (ascending, missing last — the
    // engine's standard key order).
    Frame contract(const std::vector<std::string>& varlist) const;

    // `duplicates report|drop|tag [VARLIST]` — VARLIST empty => all columns.
    // report returns per-group-size counts (copies/observations/surplus);
    // drop keeps first occurrence per key (stable); tag emits group_size-1.
    struct DupReportRow { int64_t copies, observations, surplus; };
    std::vector<DupReportRow> duplicates_report(const std::vector<std::string>& varlist) const;
    Frame duplicates_drop(const std::vector<std::string>& varlist, int64_t* n_dropped_out) const;
    std::vector<double> duplicates_tag(const std::vector<std::string>& varlist) const;

    // `recode VAR (rule)(rule).., gen(NEW)` — rule tuples, applied in listed
    // order (first match wins, Stata semantics). kind: 0=eq(value), 1=range
    // (lo,hi inclusive; +-inf via ±HUGE_VAL for min/max keywords), 2=missing,
    // 3=else (catch-all for values not matched by any prior rule).
    struct RecodeRule { int kind; double a = 0, b = 0; bool new_is_missing = false; double new_val = 0; };
    std::vector<double> recode_apply(const std::string& var, const std::vector<RecodeRule>& rules) const;

    // reshape long: STUB columns (`STUB<suffix>`) -> one `STUB` value column +
    // a `j` wave column, one row per (i, j). reshape wide: inverse.
    // (ReshapeResult is declared free-standing below — a nested struct member
    // whose type is the still-incomplete enclosing Frame would not compile.)
    struct ReshapeResult;
    ReshapeResult reshape_long(const std::vector<std::string>& stubs,
                               const std::vector<std::string>& i_vars, const std::string& j_var) const;
    ReshapeResult reshape_wide(const std::vector<std::string>& stubs,
                               const std::vector<std::string>& i_vars, const std::string& j_var) const;
    // Stata's reshape (reshape.ado), opted into with `cobra reshape --stata`:
    // long — j is the union of every stub's suffixes (a stub missing one gets
    // missing cells), rows in (i, j) order, a string j (`jstring`) holds the
    // suffixes as text; wide — one row per i in i order, columns j-major
    // (p1 q1 p2 q2), a string j's values are the suffixes. Refusals throw
    // EngineError codes "reshape_498" (non-numeric suffix without string, a
    // missing j), "reshape_9" (i not unique / (i,j) not unique / a variable
    // not constant within i) and "reshape_109" (string j without string, or
    // a numeric j with it), leaving the caller's data as it was.
    ReshapeResult reshape_long_stata(const std::vector<std::string>& stubs,
                                     const std::vector<std::string>& i_vars,
                                     const std::string& j_var, bool jstring) const;
    ReshapeResult reshape_wide_stata(const std::vector<std::string>& stubs,
                                     const std::vector<std::string>& i_vars,
                                     const std::string& j_var, bool jstring) const;

private:
    int64_t n_ = 0;
    std::vector<FrameColumn> cols_;
    std::unordered_map<std::string, int> idx_;
    std::string panel_id_col_;
    std::string panel_time_col_;
    std::string ts_format_;
    double ts_delta_ = 1.0;  // Stata tsset delta(); 1 unless a caller sets it
    int64_t store_overflow_ = 0;  // see store_overflow()
    std::string src_path_;               // .hbin this Frame borrows from (from_dataset)
    std::shared_ptr<const Dataset> src_; // keeps the borrowed mmap alive for this Frame's life
    bool structure_dirty_ = false;       // set by row/column-set-changing verbs
    // BUG-272 sort claim (see sorted_by() above). Rides the compiler-generated
    // copy/move like every other member — a copied Frame has the same row order, so
    // the same claim; every in-place mutator that could break order clears it.
    std::vector<std::string> sort_keys_;
    // Clear the claim iff `name` is one of its keys — the funnel every column-value
    // write passes through (set_or_replace / setcol / mutable_col): a rewritten key
    // column's values need not be ordered any more, and a claim in doubt clears.
    void clear_claim_if_key(const std::string& name);
    const double* require_f64(const std::string& name) const;
    // `st` rounds every value to that storage type before the store (round_to_store);
    // the Double default is the untouched move every other caller relies on.
    void set_or_replace(const std::string& name, std::vector<double> v,
                        StoreType st = StoreType::Double);
    // round_to_store, counting a value it turns into missing (store_overflow()).
    double store_round(double v, StoreType st);
    // Factor twin of set_or_replace: same replace-in-place-or-append and the same
    // sort-claim invalidation, for the egen functions whose result is text
    // (cut ,label — the level string IS Stata's value label).
    void set_or_replace_factor(const std::string& name, std::vector<int32_t> codes,
                               std::vector<std::string> levels);
    // generate/bygen's GenQualifier: the range's bounds and replace's target type are
    // checked before anything is computed; store_qualified writes the excluded rows
    // (missing, or the current value under keep_unselected) and stores the column.
    void check_gen_qualifier(const std::string& name, const GenQualifier& q) const;
    void store_qualified(const std::string& name, std::vector<double> out,
                         const std::vector<double>& mask, StoreType st, const GenQualifier& q);
    void reindex();
    // Fill cols_ (and n_) by BORROWING every column span of `s` (no copy). Shared by
    // from_dataset (mmap / view) and from_dataset_shared.
    void borrow_columns_from(const Dataset& s);
    // Full sequential writer. `reserve` pads the header to a 64KB-aligned data_start
    // so later in-place column adds fit without moving the data (mutation-path outputs
    // reserve; user-facing/tight saves do not — those stay byte-identical).
    void write_hbin_full(const std::string& path, bool reserve) const;
    // Clone src_ + pwrite dirty columns + append new ones + re-dump the padded header.
    // Returns false (→ caller full-rewrites) on any unsupported/failed step, so
    // correctness never depends on the clone. Value-identical to write_hbin_full.
    bool patch_hbin(const std::string& out_path) const;
    // True iff this Frame is a pure PERMUTATION of its source .hbin's columns — same
    // n_rows and the same {name,dtype,nbytes} column set (a bijection), differing only
    // in order (what `order` produces). Then every column's on-disk payload is reusable
    // exactly where it sits, so write_hbin can CoW-clone + rewrite ONLY the column
    // directory instead of re-emitting the whole payload. Header-only read (a few KB).
    bool is_pure_column_reorder_of_source() const;
    void force_own_all();  // materialize every borrow (before overwriting the source file)
};

struct Frame::ReshapeResult {
    Frame frame;
    std::vector<std::string> j_values;
};

// `merge 1:1|m:1|1:m KEYLIST using USING [,keep() nogenerate keepusing() assert()]`
// Standalone (not a Frame method — it consumes two Frames and needs its own
// rich options/result struct). Stata semantics documented at merge_frames()'s
// definition in frame.cpp (mirrors PIET's merge_op.py exactly).
struct MergeOpts {
    std::string type;               // "1:1" | "m:1" | "1:m" (| "m:m" under stata_plan)
    std::vector<std::string> keylist;
    std::vector<std::string> keepusing;  // empty = keep all using columns
    bool nogenerate = false;
    std::vector<int> keep_codes;    // subset of {1,2,3,4,5}; empty = keep all
    std::vector<int> assert_codes;  // subset of {1,2,3,4,5}; empty = no assert check
    // W6 / B4 paper 034 (`merge ..., update [replace]`, ~45 uses). An UPDATE merge:
    // overlapping non-key variables are no longer silently dropped from the using
    // side — they feed the master's column, and the marker gains codes 4 and 5.
    // Oracle: B4 oracle capture i (clean fixture) and
    // capture h (the same test with a marker variable that conflicts in
    // every row, which is what proves the code is per ROW, not per variable).
    bool update = false;            // fill master's MISSING cells from the using
    bool replace = false;           // ... and overwrite nonmissing conflicts too (implies update)
    // `generate(newvar)`: the marker's name. Empty = Stata's default `_merge`.
    // Oracle capture h:`generate` — after generate(mm), `confirm variable
    // _merge` is r(111): the option RENAMES the marker, it does not add a second one.
    std::string generate;
    // Stata's built-in _merge row plan (Stata's merge sorts both sides by the keys), opted into by PIET with `--stata-plan`. Off = the pandas-order
    // plan above, byte-identical. On:
    //  - master rows in key order, each paired with its key group's using row
    //    (m:m pairs the i-th master with the min(i,q-1)-th using row); then the
    //    appended rows in using key order: a group's using rows beyond its
    //    master rows (paired with the group's last master row, code 3) and the
    //    using-only rows (code 2);
    //  - a shared non-key variable takes the USING value on a using-only row
    //    (w2aw_data_combine_family.log dc_m11_v4c `[m1]|[m2]|[]|[m4]|[]|[m8]|[u5]|[u7]`),
    //    unless the two sides disagree on string-vs-numeric (`force`: master only);
    //  - columns in master order (keys where the master has them), then the
    //    using-only columns, then the marker;
    //  - keep()/assert() codes are exact (merge.ado buildexpr/checkmresults: 3
    //    never covers 4/5), and a failed assert() throws code "merge_assert".
    bool stata_plan = false;
    bool sequential = false;  // `merge 1:1 _n`: row i with row i, no keys, no sort
    bool presorted = false;   // `sorted`: both sides already in key order (PIET checked)
};
struct MergeResult {
    Frame frame;
    int64_t master_only = 0, using_only = 0, matched = 0;
    // The update-merge split of `matched`, in Stata's own summary-table order:
    // "not updated" (3) / "missing updated" (4) / "nonmissing conflict" (5).
    // All three are 0 on a standard merge, where every match is code 3.
    int64_t matched_notupdated = 0, matched_missupdated = 0, matched_conflict = 0;
};
MergeResult merge_frames(const Frame& master, const Frame& usingf, const MergeOpts& opts);

// ---- ENG-ADOPTDESC-NMISS: per-column n_missing carried along the write chain ----
//
// THE COST THIS EXISTS TO REMOVE. Every `frame use` answers with a full column
// descriptor, and each descriptor entry carries n_missing — a fresh Frame's caches
// are cold, so that is a full scan of every column (192 cols × 108k rows ≈ 166 MB of
// NaN/negative-code testing) on EVERY adoption. Measured on the FLOOR campaign's a2
// corpus: 190 ms of a 202 ms adoption, i.e. ~94%; across the 2,241 adoptions of one
// analysis.do run, ~7 minutes.
//
// WHY IT IS SOUND TO CARRY RATHER THAN RECOMPUTE. `Frame::write_hbin`'s patch path
// CoW-clones the source file and rewrites only the columns this Frame OWNS
// (`!c.borrowed()`); a still-borrowed column's bytes in the output are, by
// construction, the source file's bytes. Identical bytes ⇒ identical missing count.
// So the output file's counts are the source's, with the owned columns' own counts
// substituted — and those are one cheap count each, not 192.
//
// WHY IT CANNOT GO STALE. An entry records the file's (size, mtime_ns) as observed
// immediately after the write; the reader re-stats and requires an exact match, plus
// an exact name AND dtype match per column. Any mismatch, any missing column, any
// unstattable file → nothing is seeded and the scan runs exactly as before. The memo
// is process-local, bounded to a handful of entries, and never persisted.
struct NmissMemoCol {
    std::string name;
    std::string dtype;   // "f64" | "factor" — must match at seed time
    int64_t n_missing = -1;
};
// Record `cols` as the known counts for `path`, stamping the file's current
// (size, mtime). A no-op if `path` cannot be stat'd or `cols` is empty.
void frame_nmiss_memo_put(const std::string& path, std::vector<NmissMemoCol> cols);
// The recorded counts for `path`, but ONLY if the file still matches the stamp taken
// at put time. Empty otherwise.
std::vector<NmissMemoCol> frame_nmiss_memo_get(const std::string& path);

// ---- the expression-function vocabulary, published for census tooling ----------
//
// `cobra functions --json` dumps this so a corpus census can ask "does the engine
// know f()?" against the very table the expression parser looks names up in, rather
// than re-implementing the rule and drifting from it. One row per accepted SPELLING,
// which is what a census counts: `missing` and `mi` are two names for one
// implementation (BUG-431) and both appear here. The parser's internal row also
// carries the opcode id those two share; that id stays private.
struct ExprFnInfo {
    const char* name;
    int min_arity;
    int max_arity;
};
// Every function name a --gen/--keepif expression accepts, in parser-table order.
const std::vector<ExprFnInfo>& expr_function_table();

// B3(c): the draw coordinates for this process. `set seed` lives in the PIET
// session (the engine is a subprocess and remembers nothing between calls), so
// the seed and a per-call stream number ride the argv the way tsset does.
void set_rng_coords(long long seed, long long stream);
// Drops them again. A Frame expression call spends them when it returns; this is
// for a caller that can leave before any such call ran (cmd_eval's guard).
void clear_rng_coords();

// Stata's session stream (core/rng_mt64.h) for ONE eval call: `state` is the
// c(rngstate)-format string PIET sends as --rng-mt64. While it is on, every
// runiform()/uniform()/runiformint()/rnormal() draws from it in evaluation order
// instead of from the Philox coordinates. end_rng_mt64() turns it off and returns
// how many raw 64-bit draws the call took. begin returns false on a malformed state.
bool begin_rng_mt64(const std::string& state);
uint64_t end_rng_mt64();

// ---- Stata's numeric -> string rendering (B4/W7) ---------------------------
// `%w.0g`, Stata's GENERAL format — the one `string()`/`strofreal()` apply when the
// caller names no format, and therefore the one `egen concat` inherits for every
// numeric field. cobra had no %g renderer anywhere, which is why W6 declined to land
// concat instead of guessing one.
//
// The rule below is read off B4 oracle capture g2
// (21 values x {default, %9.0g, %10.0g, %12.0g} = 84 cells), NOT off any prose:
//   * the DEFAULT is exactly %9.0g — every `default` cell equals its `w9` cell.
//   * the width sets a significant-digit budget S = w-2: 7 at w9, 8 at w10, 10 at
//     w12. `1234567` prints whole at w9; `12345678` needs 8 and does not.
//   * SCIENTIFIC iff the decimal exponent e satisfies e >= S (too many integer
//     digits) or e < -5 (too small). The small-side threshold is width-INDEPENDENT
//     in the log: 1e-5 is fixed and 1e-10 is scientific at all three widths, even
//     at w12 where `.0000000001` would have fitted the column.
//   * the scientific mantissa carries S-4 SIGNIFICANT digits, i.e. S-5 decimals:
//     1.23e+07 at w9, 1.235e+08 at w10, 1.23457e+10 at w12. (The task brief said
//     "(budget-4) decimals"; the CELLS say budget-4 significant digits = budget-5
//     decimals, and the cells win — 1.23e+07 has 2 decimals, not 3.)
//   * FIXED shows S - (integer digits) decimals, then strips trailing zeros and the
//     leading zero: `.5`, `.25`, `.0001`, and `2.0000001` at w9 is `2`.
//   * missing is ".", zero is "0".
// UNPINNED and therefore ASSUMED: the exact small-side boundary between e = -5
// (measured fixed) and e = -10 (measured scientific); no cell sits at -6..-9.
std::string stata_format_g(double v, int width);

// `string(x)` / `string(x, "%fmt")`. An empty `fmt` is the default, which the g2 log
// measures to be %9.0g. Supported spellings are the two the captures pin: `%w.0g`
// (stata_format_g) and `%w.Nf` (fixed, and NOT blank-padded to w — egen-six
// `len_f92` = 4 for %9.2f). Anything else throws rather than guess: a silently
// mis-rendered join key is exactly the failure this campaign exists to prevent.
std::string stata_string_of(double v, const std::string& fmt);

}  // namespace cecon
