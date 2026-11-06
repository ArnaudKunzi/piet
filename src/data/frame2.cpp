// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// R2 (giant-file split, 2026-09-01): continuation TU of frame.cpp — a
// MOVE-ONLY cut at the Frame::tsfill boundary. Preamble = the original head
// verbatim; the two shared helpers come from frame_internal.h.
#include "data/frame.h"

#include <fcntl.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/clonefile.h>
#elif defined(__linux__)
#include <linux/fs.h>
#include <sys/ioctl.h>
#endif

#include "io/win_compat.h"  // pread/pwrite shim + UTF-8 path helpers (no-op on POSIX)
#include "core/error.h"     // EngineError("bad_input", ...) for the fweight honest-error paths
#include "core/eng_counters.h"  // W0: patch-vs-full write-plane ledger (P2PERF §W0)
#include "core/threads.h"   // MAXPERF Stage 3: ThreadPool for morsel-parallel collapse/merge
#include "data/radix_sort.h"  // C2-F8: value-identical LSD radix for gsort
#include "data/missing.h"     // Stata `.` / `.a`-`.z` kinds + their total order
#include "data/stata_dates.h"  // BUG-371: %td calendar, shared with PIET's pyeval layer
#include "data/ts_operators.h"  // BUG-369: L./F./D./S. inside expressions (gen/replace/if)

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <memory>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

#include "nlohmann/json.hpp"

#include "data/frame_internal.h"

namespace cecon {
using namespace frame_detail;

Frame Frame::tsfill(const std::string& timevar, const std::string& panelvar, double delta,
                    bool full) const {
    const double* t = require_f64(timevar);
    const double* pid = panelvar.empty() ? nullptr : require_f64(panelvar);
    if (!std::isfinite(delta) || delta <= 0.0)
        throw std::runtime_error("tsfill: --delta must be a finite positive number");
    // BUG-897: Stata's `==` holds between two missing values, and a missing panel
    // id or time sorts last; both keys order that way here (a NaN in the old
    // `<` comparator broke the strict weak ordering std::stable_sort needs).
    auto keyLess = [](double a, double b) {
        if (std::isnan(a)) return false;
        if (std::isnan(b)) return true;
        return a < b;
    };
    auto keyEq = [](double a, double b) { return (std::isnan(a) && std::isnan(b)) || a == b; };
    // order by (panel, time)
    std::vector<int64_t> order(n_);
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](int64_t a, int64_t b) {
        double pa = pid ? pid[a] : 0.0, pb = pid ? pid[b] : 0.0;
        if (!keyEq(pa, pb)) return keyLess(pa, pb);
        return keyLess(t[a], t[b]);
    });
    // --full (Stata `tsfill, full`): every panel spans the OVERALL min..max of the
    // time variable on the delta grid from that min; tsfill.ado refuses a missing
    // time under full (r(451), w2w_tsset_tsfill.log:1662).
    double gmin = 0.0, gmax = 0.0;
    if (full) {
        bool seen = false;
        for (int64_t i = 0; i < n_; ++i) {
            if (std::isnan(t[i]))
                throw std::runtime_error(
                    "tsfill: timevar (" + timevar +
                    ") may not contain missing values when option full is specified");
            if (!seen) { gmin = gmax = t[i]; seen = true; }
            gmin = std::min(gmin, t[i]);
            gmax = std::max(gmax, t[i]);
        }
    }
    // build output row plan: list of (origRow >=0) or (-1, panel, time) for filled
    struct OutRow { int64_t orig; double panel, time; };
    std::vector<OutRow> plan;
    int64_t g0 = 0;
    while (g0 < n_) {
        int64_t g1 = g0 + 1;
        double pv = pid ? pid[order[g0]] : 0.0;
        while (g1 < n_ && keyEq(pid ? pid[order[g1]] : 0.0, pv)) ++g1;
        // rows [g0, gm) carry a time; [gm, g1) are the panel's missing times,
        // kept as they are after the filled grid (tsfill.ado leaves them alone).
        int64_t gm = g0;
        while (gm < g1 && !std::isnan(t[order[gm]])) ++gm;
        if (gm > g0) {
            double tmin = full ? gmin : t[order[g0]], tmax = full ? gmax : t[order[gm - 1]];
            double dsteps = (tmax - tmin) / delta + 1.0;
            if (!std::isfinite(dsteps) || dsteps > 2e8)
                throw std::runtime_error("tsfill: implied fill row count exceeds 200M; check --delta and the time range");
            int64_t steps = static_cast<int64_t>(std::llround((tmax - tmin) / delta)) + 1;
            std::vector<int64_t> rowAt(std::max<int64_t>(steps, 0), -1);
            for (int64_t q = g0; q < gm; ++q) {
                double qd = (t[order[q]] - tmin) / delta;
                int64_t k = static_cast<int64_t>(std::llround(qd));
                // Stata: tsset requires regularly-spaced (on-grid) time values. An
                // off-grid time (not an integer number of --delta steps from the panel
                // min) is r(451); a value that collapses onto an already-occupied
                // period is r(111). Reject rather than silently dropping the colliding
                // row (C1-F17 / BUG-073-sib). Off-grid calendar arithmetic is out of
                // scope — reject-only per the golden.
                if (std::isfinite(qd) && std::fabs(qd - static_cast<double>(k)) > 1e-6)
                    throw std::runtime_error(
                        "tsfill: time value not on the --delta grid (times must be "
                        "regularly spaced by delta; off-grid value would collapse periods)");
                if (k >= 0 && k < steps) {
                    if (rowAt[k] >= 0)
                        throw std::runtime_error(
                            "tsfill: repeated time value within panel (two observations "
                            "map to the same period)");
                    rowAt[k] = order[q];
                }
            }
            for (int64_t k = 0; k < steps; ++k) {
                if (rowAt[k] >= 0) plan.push_back({rowAt[k], pv, tmin + k * delta});
                else plan.push_back({-1, pv, tmin + k * delta});
            }
        }
        for (int64_t q = gm; q < g1; ++q) plan.push_back({order[q], pv, t[order[q]]});
        g0 = g1;
    }
    int64_t N = static_cast<int64_t>(plan.size());
    Frame out;
    out.n_ = N;
    int tcol = -1, pcol = -1;
    for (size_t c = 0; c < cols_.size(); ++c) {
        if (cols_[c].name == timevar) tcol = static_cast<int>(c);
        if (!panelvar.empty() && cols_[c].name == panelvar) pcol = static_cast<int>(c);
    }
    for (size_t c = 0; c < cols_.size(); ++c) {
        FrameColumn nc;
        nc.name = cols_[c].name;
        nc.is_factor = cols_[c].is_factor;
        nc.format = cols_[c].format;  // BUG-363: identity metadata, not payload
        nc.label = cols_[c].label;
        if (nc.is_factor) {
            nc.levels = cols_[c].levels;
            nc.codes.assign(N, -1);  // -1 = missing factor code
            for (int64_t i = 0; i < N; ++i) if (plan[i].orig >= 0) nc.codes[i] = cols_[c].codes[plan[i].orig];
        } else {
            nc.f64.assign(N, NAd);
            for (int64_t i = 0; i < N; ++i) {
                if (plan[i].orig >= 0) nc.f64[i] = cols_[c].f64[plan[i].orig];
                else if (static_cast<int>(c) == tcol) nc.f64[i] = plan[i].time;
                else if (static_cast<int>(c) == pcol) nc.f64[i] = plan[i].panel;
            }
        }
        out.cols_.push_back(std::move(nc));
    }
    out.reindex();
    return out;
}

namespace {
constexpr int64_t kAlign = 64;
constexpr int64_t kReserve = 65536;  // 64KB reserved-header boundary for mutation outputs
inline int64_t align_up(int64_t x, int64_t a) { return (x + a - 1) / a * a; }

// Copy-on-write file clone (APFS clonefile / Linux FICLONE). Returns false on ANY
// failure — unsupported FS, cross-device (EXDEV), or a platform with no clone syscall
// — so patch_hbin falls back to a full rewrite. The fallback works everywhere
// (Windows included), so a port is never blocked by the missing clone primitive.
bool clone_file(const std::string& src, const std::string& dst) {
#if defined(__APPLE__)
    return ::clonefile(src.c_str(), dst.c_str(), 0) == 0;
#elif defined(__linux__)
    int s = ::open(src.c_str(), O_RDONLY);
    if (s < 0) return false;
    int d = ::open(dst.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (d < 0) { ::close(s); return false; }
    bool ok = ::ioctl(d, FICLONE, s) == 0;
    ::close(d);
    ::close(s);
    if (!ok) ::unlink(dst.c_str());
    return ok;
#else
    (void)src;
    (void)dst;
    return false;  // no CoW clone on this platform → full rewrite
#endif
}
}  // namespace

void Frame::write_hbin(const std::string& path) const {
    // Dispatcher. A Frame loaded from an .hbin, only column-add/replace since (row
    // count + column set intact), saved to a DIFFERENT path → clone the source and
    // patch just the dirty/new columns. Everything else (fresh frames, structure
    // changes, in-place overwrite) → full rewrite, byte-identical to before.
    const bool from_hbin = !src_path_.empty();
    // A pure column REORDER (structure_dirty_, but the same {name,dtype,nbytes} set as
    // the source) is patchable too: `order` only permutes the column directory, leaving
    // every payload byte where it already sits, so patch_hbin CoW-clones + rewrites the
    // header alone (~164 MB rewrite → O(cols)). The bijection precheck keeps rename/add/
    // drop/row-change (also structure_dirty_) on the untouched full-rewrite path.
    if (from_hbin && path != src_path_ &&
        (!structure_dirty_ || is_pure_column_reorder_of_source())) {
        if (patch_hbin(path)) {
            engctr::addPatchHit();
            return;
        }
        engctr::addPatchFallback();  // eligible but patch_hbin declined (slack/shape)
        write_hbin_full(path, /*reserve=*/true);  // fallback: reserve so the next hop can patch
        return;
    }
    if (from_hbin && path == src_path_) {
        // save,replace: `path` IS the file this Frame may still be borrowing
        // (mmap'd) columns from. Opening `path` truncate-in-place destroys that
        // backing store the instant the ofstream opens — which is why the old
        // code had to force_own_all() (copy every still-borrowed column into
        // owned heap memory) BEFORE that write, and why a crash/ENOSPC mid-write
        // left `path` truncated with the original bytes already gone.
        //
        // Prefer instead: stream the tight writer's output to a sibling temp
        // file in the SAME directory (same filesystem, so the closing rename is
        // atomic and can't EXDEV) while reading straight out of the
        // still-untouched source mmap, then rename(2) the temp over `path`.
        // `path` is never opened for write until that one atomic rename, so
        // (a) no defensive copy of borrowed columns is needed at all, and (b) a
        // failure while producing the temp file leaves the original untouched
        // instead of corrupt. The bytes are produced by the exact same
        // write_hbin_full(reserve=false) tight writer used before (the target
        // filename never affects header/data content), so the result is
        // byte-identical to the old in-place rewrite. POSIX keeps the historical
        // truncate-in-place fallback if the replace fails (temp create, write, or
        // rename), so correctness never depends on the fast path. Windows instead
        // reports a data_io error: falling back there would surrender the intact
        // target after a failed atomic wide-path ReplaceFileW.
        //
        // NOTE (inode/hardlink edge): rename() retargets the directory entry to
        // the temp file's NEW inode; another fd or hardlink already holding the
        // OLD inode (e.g. a second process with `path` open, or a hardlink to
        // it) keeps seeing the pre-replace bytes rather than the update, where
        // the previous truncate-in-place rewrite mutated the original inode in
        // place (such an observer would eventually see the new — possibly
        // torn — bytes instead). Accepted tradeoff: the same one `mv`/most
        // editors/Stata's own `save, replace` make.
        // Split via std::filesystem rather than find_last_of('/') so a Windows
        // path (backslash separators, or a bare "C:" drive-relative prefix)
        // still yields the SAME directory the target lives in — the sibling-temp
        // + atomic-replace guarantee depends on that.
        const std::filesystem::path target = to_native_path(path);
        std::filesystem::path dir = target.parent_path();
        if (dir.empty()) dir = std::filesystem::path(".");
        const std::string base = from_native_path(target.filename());
        static int seq = 0;
        const std::string tmp = from_native_path(
            dir / to_native_path("." + base + ".tmp" + std::to_string(native_process_id()) +
                                 "_" + std::to_string(seq++)));
        bool ok = false;
        try {
            write_hbin_full(tmp, /*reserve=*/false);
            ok = native_replace_file(tmp, path);
        } catch (...) {
            ok = false;
        }
        if (ok) return;
        native_unlink(tmp);  // best-effort cleanup of a partial/failed temp file
#if defined(_WIN32)
        // Never degrade save,replace to a destructive truncate on Windows. A
        // failed ReplaceFileW leaves the original destination in place, and the
        // truncate below would throw that intact file away.
        throw EngineError("data_io", "cannot replace " + path, path);
#else
        const_cast<Frame*>(this)->force_own_all();  // fallback: don't read the mmap we're about to truncate
#endif
    }
    write_hbin_full(path, /*reserve=*/false);  // tight header — byte-identical to the reference
}


// ---- write-side dictionary carry (RAMDIET, 2026-08-15) ----
// See cecon::Column::levels_text (io/columnar.h) for where the text lives and why.
namespace {

int64_t g_lvl_hit = 0, g_lvl_miss = 0;
int64_t g_lvl_nosrc = 0, g_lvl_noname = 0, g_lvl_ident = 0, g_lvl_fill = 0;

// A factor dictionary as the header's JSON array, each level as
// hbin_level_needs_escape (io/columnar.h) says it is written.
nlohmann::json levels_json(const std::vector<std::string>& levels) {
    nlohmann::json arr = nlohmann::json::array();
    auto& a = arr.get_ref<nlohmann::json::array_t&>();
    a.reserve(levels.size());
    for (const std::string& l : levels) {
        if (hbin_level_needs_escape(l)) a.emplace_back(hbin_level_escape(l));
        else a.emplace_back(l);
    }
    return arr;
}

// COBRA_LVLCARRY_VERIFY=1: prove the carry's byte claim ON THE REAL WORKLOAD.
//
// It has to be proved here, in-process, because nothing else can: the seeded-defect
// probe (a semantically neutral space inserted into the spliced array) left freeze
// 163/163 and ctest 92/92 GREEN, so no gate in the tree byte-compares an .hbin
// header. Every gate parses. This one compares what the carry emitted against what
// the ordinary path would have emitted for the same dictionary, and says so.
void verify_carried_text(const std::vector<std::string>& src_levels,
                         const std::string& carried) {
    static const bool on = [] {
        const char* e = std::getenv("COBRA_LVLCARRY_VERIFY");
        return e && *e;
    }();
    if (!on) return;
    const std::string inlined = levels_json(src_levels).dump();
    if (inlined != carried)
        throw std::runtime_error("levels carry: spliced text differs from inline dump ("
                                 + std::to_string(carried.size()) + " vs "
                                 + std::to_string(inlined.size()) + " bytes)");
}

// The JSON array text for a factor column's dictionary, IF that dictionary has the
// same CONTENT as the source Dataset's — which is the test that actually fires.
//
// Pointer identity was tried first and hit 0 times in 787 lookups (508 of the misses
// were exactly this): CowVec's copy constructor deep-materializes on purpose
// (cow_vec.h, "copy = deep materialize (never inherit a borrow)"), so the first
// column-CoW detach anywhere in the frame moves every dictionary off the source's
// buffer even though nothing wrote to it. Comparing content instead costs one
// allocation-free pass over the strings and buys the whole escape+dump of them, so
// it is the cheap side of the trade by a wide margin — and it is exact: equal
// strings serialize to equal bytes.
//
// Reads go through the proxy's const surface, which does NOT materialize a borrowed
// dictionary — `.ref()` fed to nlohmann is exactly what did.
const std::string* carried_levels_text(const Dataset* src, const FrameColumn& c) {
    if (!c.is_factor) return nullptr;
    if (src == nullptr) { ++g_lvl_nosrc; ++g_lvl_miss; return nullptr; }
    const std::string* p = c.levels.data();
    const std::size_t n = c.levels.size();
    for (const Column& sc : src->columns()) {
        if (sc.name != c.name) continue;
        if (sc.levels.size() != n) { ++g_lvl_ident; ++g_lvl_miss; return nullptr; }
        if (sc.levels.data() != p) {   // not the same buffer — is it the same content?
            for (std::size_t i = 0; i < n; ++i)
                if (sc.levels[i] != p[i]) { ++g_lvl_ident; ++g_lvl_miss; return nullptr; }
        }
        if (sc.levels_text_n != static_cast<int64_t>(n)) {
            sc.levels_text = levels_json(sc.levels).dump();
            sc.levels_text_n = static_cast<int64_t>(n);
            ++g_lvl_fill; ++g_lvl_miss;
        } else {
            ++g_lvl_hit;
        }
        verify_carried_text(sc.levels, sc.levels_text);
        return &sc.levels_text;
    }
    ++g_lvl_noname; ++g_lvl_miss;
    return nullptr;
}

// Placeholder written into the header DOM in place of a carried dictionary. Chosen
// so nlohmann's dump() escapes nothing in it, which makes the post-dump byte pattern
// exact and unique: before the splice the ONLY `"levels":` occurrences in the text
// are these, and a level string cannot forge one (its quotes would be escaped).
std::string levels_sentinel(std::size_t k) {
    return "__CECON_LVL" + std::to_string(k) + "__";
}

// Swap each placeholder for its carried dictionary text. The result is byte-identical
// to having dumped the dictionary inline — that equality is what the freeze corpus,
// which byte-compares written .hbin files, is the judge of.
void splice_levels(std::string& hjson, const std::vector<const std::string*>& texts) {
    std::size_t from = 0;
    for (std::size_t k = 0; k < texts.size(); ++k) {
        const std::string needle = "\"levels\":\"" + levels_sentinel(k) + "\"";
        const std::size_t pos = hjson.find(needle, from);
        if (pos == std::string::npos)
            throw std::runtime_error("hbin header: dictionary placeholder " +
                                     std::to_string(k) + " vanished before splice");
        hjson.replace(pos, needle.size(), "\"levels\":" + *texts[k]);
        from = pos;   // placeholders are emitted in column order; never rescan from 0
    }
}

}  // namespace

// ---- RAMDIET header-serialization instrument (env-gated, measurement only) ----
// The hbin header carries every factor's full level dictionary. On this corpus that
// is 135,603 strings (obs_id alone is one level per row), rebuilt as nlohmann DOM
// nodes on EVERY save. This counts the calls, the levels, and the nanoseconds so the
// prize is measured before anything is redesigned. Flushed on a call cadence because
// atexit never runs — PIET SIGTERMs its workers.
namespace {
struct HdrStat { int64_t calls = 0, levels = 0, ns = 0, dump_ns = 0; };
HdrStat g_hdr_full, g_hdr_patch;
const char* hdrlog_path() {
    static const char* p = std::getenv("COBRA_HDRLOG");
    return p;
}
void hdr_flush() {
    const char* p = hdrlog_path();
    if (!p) return;
    if (FILE* f = std::fopen(p, "w")) {
        std::fprintf(f, "write_hbin_full calls=%lld levels=%lld ms=%.1f (dump %.1f / build %.1f)\n",
                     (long long)g_hdr_full.calls, (long long)g_hdr_full.levels,
                     g_hdr_full.ns / 1e6, g_hdr_full.dump_ns / 1e6,
                     (g_hdr_full.ns - g_hdr_full.dump_ns) / 1e6);
        std::fprintf(f, "patch_hbin      calls=%lld levels=%lld ms=%.1f (dump %.1f / build %.1f)\n",
                     (long long)g_hdr_patch.calls, (long long)g_hdr_patch.levels,
                     g_hdr_patch.ns / 1e6, g_hdr_patch.dump_ns / 1e6,
                     (g_hdr_patch.ns - g_hdr_patch.dump_ns) / 1e6);
        std::fprintf(f, "dictionary carry  hit=%lld miss=%lld (nosrc=%lld noname=%lld ident=%lld fill=%lld)\n",
                     (long long)g_lvl_hit, (long long)g_lvl_miss, (long long)g_lvl_nosrc,
                     (long long)g_lvl_noname, (long long)g_lvl_ident, (long long)g_lvl_fill);
        std::fclose(f);
    }
}
int64_t hdr_now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}
}  // namespace

void Frame::write_hbin_full(const std::string& path, bool reserve) const {
    engctr::addPatchFull();  // W0: every complete re-serialize, whatever routed here
    using nlohmann::json;
    const int64_t hdr_t0 = hdrlog_path() ? hdr_now_ns() : 0;
    json header;
    std::vector<const std::string*> lvl_text;
    header["magic"] = "CECONHB1";
    header["version"] = 1;
    header["n_rows"] = n_;
    // BUG-272: the sort claim, emitted ONLY when non-empty — `label`'s append-only-
    // key discipline (absent != empty). A claimless frame dumps a byte-identical
    // header to the pre-BUG-272 writer, which is what keeps every freeze fixture
    // whose pipeline never sorts from moving. nlohmann emits keys sorted, so this
    // lands between "n_rows" and "version".
    if (!sort_keys_.empty()) header["sorted_by"] = join_sort_keys(sort_keys_);
    json jcols = json::array();
    int64_t rel = 0;
    std::vector<std::pair<const void*, int64_t>> blobs;
    std::vector<const FrameColumn*> blob_narrow;  // LADDER: non-null = widen at write time
    for (const FrameColumn& c : cols_) {
        json jc;
        jc["name"] = c.name;
        // P1-A0-FORMAT-CARRIAGE: emit the display format ONLY when the column has
        // one. Conditional is the whole point — a frame with no formats (every
        // path except a .dta import today) dumps a byte-identical header to the
        // pre-A0 writer, which is what keeps 119 freeze fixtures and every hbin
        // byte-compare golden from moving. Never write `"format": ""`.
        if (!c.format.empty()) jc["format"] = c.format;
        // BUG-198: same conditional emit, same reason — a frame with no labels
        // dumps a byte-identical header to the pre-BUG-198 writer. Never write
        // `"label": ""`.
        // A label that is not UTF-8 is written escaped, as a level is (BUG-1372).
        if (!c.label.empty()) jc["label"] = hbin_level_json_text(c.label);
        // NMISS-CARRY (2026-08-20): state the missing count the engine already has.
        // `n_missing` is lazily computed and mutation-invalidated (frame.h), and by
        // this point in a write it is warm for every column the mutation touched —
        // so this is the writer telling the reader a number it already owns, not a
        // new scan. Emitted UNCONDITIONALLY, including zero: unlike `format`/`label`
        // where absent and empty mean the same thing to a consumer, a missing count
        // of 0 is a real answer that must be distinguishable from "not stated", and
        // the reader keys exactly on presence (columnar.h: -1 = absent = count it
        // yourself). That does move header bytes for every file this writer emits —
        // which is why the two header-byte goldens are re-pinned in this same
        // change, and why freeze is unaffected (it captures command stdout, not
        // hbin bytes: tools/lint_hbin_header_bytes.py's own header says so).
        if (const int64_t nm = c.n_missing_if_known(); nm >= 0) jc["n_missing"] = nm;
        int64_t nbytes;
        if (c.is_factor) {
            jc["dtype"] = "factor";
            if (const std::string* lt = carried_levels_text(src_.get(), c)) {
                jc["levels"] = levels_sentinel(lvl_text.size());  // spliced after dump()
                lvl_text.push_back(lt);
            } else {
                jc["levels"] = levels_json(c.levels.ref());  // the vector, not the proxy
            }
            if (hdrlog_path()) g_hdr_full.levels += static_cast<int64_t>(c.levels.size());
            nbytes = static_cast<int64_t>(c.codes.size()) * 4;
            blobs.emplace_back(c.codes.data(), nbytes);
            blob_narrow.push_back(nullptr);
        } else {
            jc["dtype"] = "f64";
            nbytes = static_cast<int64_t>(c.f64.size()) * 8;
            if (c.f64.narrow()) {
                // LADDER: the .hbin format carries f64 and nothing else, so a narrow
                // column must be widened to be written — but in the write loop below,
                // one column at a time. Widening here, where pointers are collected,
                // would hold the whole frame twice.
                blobs.emplace_back(nullptr, nbytes);
                blob_narrow.push_back(&c);
            } else {
                blobs.emplace_back(c.f64.data(), nbytes);
                blob_narrow.push_back(nullptr);
            }
        }
        jc["offset"] = rel;
        jc["nbytes"] = nbytes;
        jcols.push_back(std::move(jc));
        rel = align_up(rel + nbytes, kAlign);
    }
    header["columns"] = std::move(jcols);
    const int64_t dmp_t0 = hdrlog_path() ? hdr_now_ns() : 0;
    std::string hjson = header.dump();
    splice_levels(hjson, lvl_text);
    if (hdrlog_path()) g_hdr_full.dump_ns += hdr_now_ns() - dmp_t0;
    if (hdrlog_path()) { ++g_hdr_full.calls; g_hdr_full.ns += hdr_now_ns() - hdr_t0; hdr_flush(); }
    int64_t data_start;
    if (reserve) {
        // Pad the header with trailing spaces (valid JSON) so data_start lands on a
        // 64KB boundary >= 1.25x the tight header — leaving room to add columns in
        // place on a later patch. Every existing reader derives data_start from the
        // padded header_len, so a reserved file is fully backward/forward compatible.
        int64_t tight = 16 + static_cast<int64_t>(hjson.size());
        int64_t target = tight + tight / 4;
        if (target < kReserve) target = kReserve;
        data_start = align_up(target, kReserve);
        hjson.append(static_cast<size_t>(data_start - 16 - static_cast<int64_t>(hjson.size())), ' ');
    } else {
        data_start = align_up(16 + static_cast<int64_t>(hjson.size()), kAlign);
    }

    std::ofstream f(to_native_path(path), std::ios::binary);
    if (!f) {
        const int we = errno;
        throw std::runtime_error("cannot write " + path + " (errno " + std::to_string(we) +
                                 ": " + std::strerror(we) + ")");
    }
    f.write("CECONHB1", 8);
    uint64_t hlen = hjson.size();
    f.write(reinterpret_cast<const char*>(&hlen), 8);
    f.write(hjson.data(), hjson.size());
    std::vector<char> pad(kAlign, 0);
    int64_t pos = 16 + static_cast<int64_t>(hlen);
    if (pos < data_start) { f.write(pad.data(), data_start - pos); pos = data_start; }
    std::vector<double> widen_tmp;   // LADDER: reused; ONE widened column alive at a time
    for (size_t i = 0; i < blobs.size(); ++i) {
        int64_t want = data_start + header["columns"][i]["offset"].get<int64_t>();
        if (pos < want) { f.write(pad.data(), want - pos); pos = want; }
        const void* src = blobs[i].first;
        // Branch on the SIDE TABLE, not on src==nullptr: an EMPTY wide column's
        // data() is also legitimately null (zero-row joinby output), and treating
        // that as "narrow, widen it" dereferenced a null FrameColumn*.
        if (blob_narrow[i] != nullptr) {
            const CowVec<float>& nv = blob_narrow[i]->f64.f32_ref();
            widen_tmp.assign(nv.begin(), nv.end());
            src = widen_tmp.data();
        }
        f.write(static_cast<const char*>(src), blobs[i].second);
        pos += blobs[i].second;
    }
    // BUG-109: an ofstream swallows write failures (ENOSPC, IO error) into badbit
    // silently — every f.write above can have failed while leaving a truncated file
    // that looks "written". The patch_hbin (CoW) path checks every pwrite; this full
    // writer checked nothing, so save,replace's atomic temp+rename could commit a
    // corrupt temp OVER the intact original — the exact loss its design prevents.
    // Flush and check the stream ONCE here; the save,replace caller already treats a
    // throw as fallback-abort (leaving the original untouched).
    f.flush();
    if (!f) {
        // BUG-124: do not leave the torn output behind. The header was written
        // first and completely, so the partial passes magic/header checks and an
        // existence probe — the B.5.0 fair-run mmap'd exactly such a file and
        // SIGSEGV'd scanning past its end. Close, unlink, then throw; no caller
        // expects a usable file at `path` after this throw (the save,replace
        // temp+rename caller unlinks its own tmp as well — harmless overlap).
        f.close();
        std::error_code rmEc;
        std::filesystem::remove(to_native_path(path), rmEc);  // best-effort
        throw EngineError("data_io",
            "failed writing '" + path + "' (disk full or I/O error) — output is incomplete", path);
    }
}

// ENG-APPEND: streaming append writer. Produces byte-for-byte the same file as
// append(usingf).write_hbin_full(path, /*reserve=*/false) — the tight branch the
// append RESULT frame's write_hbin() always takes (it has no src_path_, so the
// patch/reserve fallback at frame.cpp:2092-2096 is never reached) — but without
// building the full (nm+nu)-row result Frame. Every union column is emitted
// master-part ++ using-part directly, mirroring Frame::append's per-column rules
// (frame.cpp:2646) exactly, with only per-column temporaries bounded by max(nm,nu).
void Frame::write_hbin_append(const Frame& usingf, const std::string& path) const {
    using nlohmann::json;
    const int64_t nm = n_, nu = usingf.n_;
    const int64_t nr = nm + nu;

    // --- 1. Union column order + per-column streaming plan (mirrors Frame::append) ---
    std::vector<std::string> all_cols;
    std::unordered_map<std::string, char> seen;
    for (const auto& c : cols_) { all_cols.push_back(c.name); seen[c.name] = 1; }
    for (const auto& c : usingf.cols_) if (!seen.count(c.name)) { all_cols.push_back(c.name); seen[c.name] = 1; }

    enum Kind { F64_BOTH, FACTOR_BOTH, MIXED_F64, F64_M_ONLY, FACTOR_M_ONLY, F64_U_ONLY, FACTOR_U_ONLY };
    struct Plan {
        std::string name;
        bool is_factor = false;          // final on-disk dtype (mixed collapses to f64)
        Kind kind = F64_BOTH;
        // BUG-394 (2026-09-02): dictionary carry for this writer. When the union
        // dictionary IS the master's (master-only factor, or both sides with no
        // using-only level), the header can splice the master file's original
        // levels text exactly as write_hbin_full does (carried_levels_text) instead
        // of re-serialising the vector through the JSON DOM. Byte-identical by the
        // same argument the full writer makes; COBRA_LVLCARRY_VERIFY proves it.
        const FrameColumn* carry = nullptr;
        const FrameColumn* mc = nullptr;
        const FrameColumn* uc = nullptr;
        std::vector<std::string> levels; // union dictionary (factor kinds only)
        std::vector<int32_t> uremap;     // FACTOR_BOTH: using codes remapped into the union (length nu)
        int64_t nbytes = 0;
        int64_t offset = 0;
    };
    std::vector<Plan> plan;
    plan.reserve(all_cols.size());

    for (const auto& name : all_cols) {
        const bool in_m = idx_.count(name) > 0, in_u = usingf.idx_.count(name) > 0;
        Plan p;
        p.name = name;
        if (in_m && in_u) {
            const FrameColumn& mc = col(name);
            const FrameColumn& uc = usingf.col(name);
            p.mc = &mc; p.uc = &uc;
            if (!mc.is_factor && !uc.is_factor) {
                p.is_factor = false; p.kind = F64_BOTH;
            } else if (mc.is_factor && uc.is_factor) {
                p.is_factor = true; p.kind = FACTOR_BOTH;
                // dictionary union: master levels, then using-only levels in using order
                std::unordered_map<std::string, int32_t> dict;
                for (size_t i = 0; i < mc.levels.size(); ++i) { p.levels.push_back(mc.levels[i]); dict[mc.levels[i]] = static_cast<int32_t>(i); }
                std::vector<int32_t> remap(uc.levels.size());
                for (size_t i = 0; i < uc.levels.size(); ++i) {
                    auto it = dict.find(uc.levels[i]);
                    if (it != dict.end()) remap[i] = it->second;
                    else { remap[i] = static_cast<int32_t>(p.levels.size()); dict[uc.levels[i]] = remap[i]; p.levels.push_back(uc.levels[i]); }
                }
                p.uremap.resize(static_cast<size_t>(nu));
                const int32_t* uc_codes = uc.codes.data();
                for (int64_t j = 0; j < nu; ++j) { const int32_t c = uc_codes[j]; p.uremap[static_cast<size_t>(j)] = c < 0 ? -1 : remap[c]; }
                if (p.levels.size() == mc.levels.size()) p.carry = &mc;  // BUG-394: union == master dictionary
            } else {
                p.is_factor = false; p.kind = MIXED_F64;  // f64+factor either way -> both promoted to f64
            }
        } else if (in_m) {
            const FrameColumn& mc = col(name);
            p.mc = &mc;
            p.is_factor = mc.is_factor;
            p.kind = mc.is_factor ? FACTOR_M_ONLY : F64_M_ONLY;
            if (mc.is_factor) for (size_t i = 0; i < mc.levels.size(); ++i) p.levels.push_back(mc.levels[i]);
            if (mc.is_factor) p.carry = &mc;  // BUG-394
        } else {
            const FrameColumn& uc = usingf.col(name);
            p.uc = &uc;
            p.is_factor = uc.is_factor;
            p.kind = uc.is_factor ? FACTOR_U_ONLY : F64_U_ONLY;
            if (uc.is_factor) for (size_t i = 0; i < uc.levels.size(); ++i) p.levels.push_back(uc.levels[i]);
        }
        p.nbytes = p.is_factor ? nr * 4 : nr * 8;
        plan.push_back(std::move(p));
    }

    // --- 2. Header JSON — identical field construction/order to write_hbin_full ---
    json header;
    header["magic"] = "CECONHB1";
    header["version"] = 1;
    header["n_rows"] = nr;
    // BUG-272: NO root `"sorted_by"` key here, DELIBERATELY — the reference path's
    // result (Frame::append returns a FRESH Frame, claimless by construction; row
    // concat breaks any order anyway) emits none, so byte-identity with
    // append(usingf).write_hbin_full(path, false) requires this writer to emit none.
    // Same coupling law as `format`/`label` above.
    std::vector<const std::string*> lvl_text;  // BUG-394: carried dictionary texts, spliced after dump
    json jcols = json::array();
    int64_t rel = 0;
    for (auto& p : plan) {
        json jc;
        jc["name"] = p.name;
        // P1-A0-FORMAT-CARRIAGE / BUG-198 / BUG-363. This writer's contract is
        // byte-identity with append(usingf).write_hbin_full(path, false), so these
        // keys track Frame::append EXACTLY. As of BUG-363 (2026-08-22) append DOES
        // carry them — master wins a shared name, the using side supplies a
        // using-only column — so this writer states them on the same rule and with
        // the same absent-vs-empty discipline the full writer uses ("" = absent,
        // key omitted). The two sites changed in ONE commit, as the previous version
        // of this comment required; the a2 A/B byte-compare is what enforces it.
        {
            const FrameColumn* src = p.mc ? p.mc : p.uc;
            if (src) {
                if (!src->format.empty()) jc["format"] = src->format;
                if (!src->label.empty()) jc["label"] = hbin_level_json_text(src->label);
            }
        }
        if (p.is_factor) {
            jc["dtype"] = "factor";
            const std::string* lt = p.carry ? carried_levels_text(src_.get(), *p.carry) : nullptr;
            if (lt) {
                jc["levels"] = levels_sentinel(lvl_text.size());  // spliced after dump() — BUG-394
                lvl_text.push_back(lt);
            } else {
                jc["levels"] = levels_json(p.levels);
            }
        } else { jc["dtype"] = "f64"; }
        // NMISS-CARRY: this writer's contract is byte-identity with
        // append(usingf).write_hbin_full(path, false), so the moment the full
        // writer states a key this one must state the SAME value — the coupling
        // the `format`/`label` comments above spell out, now exercised for the
        // first time. The appended column's missing count is just the two parts'
        // counts summed: a column ABSENT from one side contributes that side's
        // whole row count, which is what append means by "missing there". The
        // FACTOR_BOTH remap preserves it (a negative code stays negative under
        // the union remap) and so does the mixed-kind collapse to f64.
        //
        // Derived rather than measured on the result — this writer never builds
        // the result frame, that is its whole point. Two judges check the
        // derivation: the A/B byte-compare against the reference path (which
        // computes the value directly), and PIET_VERIFY_NMISS, which re-scans
        // every carried count on read and aborts on disagreement.
        // Warm-only, and BOTH halves must be warm — a sum with one cold half
        // would force the scan this carry exists never to cause.
        {
            const int64_t mm = p.mc ? p.mc->n_missing_if_known() : n_;
            const int64_t um = p.uc ? p.uc->n_missing_if_known() : usingf.n_;
            if (mm >= 0 && um >= 0) jc["n_missing"] = mm + um;
        }
        p.offset = rel;
        jc["offset"] = rel;
        jc["nbytes"] = p.nbytes;
        jcols.push_back(std::move(jc));
        rel = align_up(rel + p.nbytes, kAlign);
    }
    header["columns"] = std::move(jcols);
    std::string hjson = header.dump();
    splice_levels(hjson, lvl_text);  // BUG-394: same splice the full writer performs
    const int64_t data_start = align_up(16 + static_cast<int64_t>(hjson.size()), kAlign);  // reserve=false (tight)

    // --- 3. Stream: magic + hlen + header + pad, then per-col master-part ++ using-part ---
    std::ofstream f(to_native_path(path), std::ios::binary);
    if (!f) {
        const int we = errno;
        throw std::runtime_error("cannot write " + path + " (errno " + std::to_string(we) +
                                 ": " + std::strerror(we) + ")");
    }
    f.write("CECONHB1", 8);
    const uint64_t hlen = hjson.size();
    f.write(reinterpret_cast<const char*>(&hlen), 8);
    f.write(hjson.data(), hjson.size());
    const std::vector<char> zpad(kAlign, 0);
    int64_t pos = 16 + static_cast<int64_t>(hlen);
    if (pos < data_start) { f.write(zpad.data(), data_start - pos); pos = data_start; }

    auto put_f64 = [&](const double* p, int64_t n) { f.write(reinterpret_cast<const char*>(p), n * 8); };
    auto put_i32 = [&](const int32_t* p, int64_t n) { f.write(reinterpret_cast<const char*>(p), n * 4); };
    auto fill_f64 = [&](int64_t n) { std::vector<double> b(static_cast<size_t>(n), NAd); f.write(reinterpret_cast<const char*>(b.data()), n * 8); };
    auto fill_i32 = [&](int64_t n, int32_t v) { std::vector<int32_t> b(static_cast<size_t>(n), v); f.write(reinterpret_cast<const char*>(b.data()), n * 4); };
    auto promote = [&](const FrameColumn& fc, int64_t n) {  // factor codes -> f64 (neg -> NaN), matches append
        std::vector<double> t(static_cast<size_t>(n));
        const int32_t* c = fc.codes.data();
        for (int64_t i = 0; i < n; ++i) t[static_cast<size_t>(i)] = c[i] < 0 ? NAd : static_cast<double>(c[i]);
        put_f64(t.data(), n);
    };

    for (const auto& p : plan) {
        const int64_t want = data_start + p.offset;
        if (pos < want) { f.write(zpad.data(), want - pos); pos = want; }
        switch (p.kind) {
            case F64_BOTH:       put_f64(p.mc->f64.data(), nm);  put_f64(p.uc->f64.data(), nu);   break;
            case FACTOR_BOTH:    put_i32(p.mc->codes.data(), nm); put_i32(p.uremap.data(), nu);    break;
            case MIXED_F64:
                if (!p.mc->is_factor) put_f64(p.mc->f64.data(), nm); else promote(*p.mc, nm);
                if (!p.uc->is_factor) put_f64(p.uc->f64.data(), nu); else promote(*p.uc, nu);
                break;
            case F64_M_ONLY:     put_f64(p.mc->f64.data(), nm);  fill_f64(nu);                    break;
            case FACTOR_M_ONLY:  put_i32(p.mc->codes.data(), nm); fill_i32(nu, -1);               break;
            case F64_U_ONLY:     fill_f64(nm);                    put_f64(p.uc->f64.data(), nu);   break;
            case FACTOR_U_ONLY:  fill_i32(nm, -1);                put_i32(p.uc->codes.data(), nu); break;
        }
        pos += p.nbytes;
    }

    // BUG-109/BUG-124 parity: ofstream swallows write errors into badbit — check once,
    // and on failure remove the torn output (mirrors write_hbin_full's tail exactly).
    f.flush();
    if (!f) {
        f.close();
        std::error_code rmEc;
        std::filesystem::remove(to_native_path(path), rmEc);  // best-effort
        throw EngineError("data_io",
            "failed writing '" + path + "' (disk full or I/O error) — output is incomplete", path);
    }
}

bool Frame::patch_hbin(const std::string& out_path) const {
    const int64_t hdr_t0 = hdrlog_path() ? hdr_now_ns() : 0;
    using nlohmann::json;
    struct SrcCol { std::string dtype; int64_t nbytes, offset; };
    std::unordered_map<std::string, SrcCol> srccol;
    int64_t src_size = 0, src_data_start = 0;

    // --- source layout: from the mapping we ALREADY hold, else from disk -----------
    // ENG-SETCOL-DEDUP. The layout this needs — per column {dtype, nbytes, offset} plus
    // data_start and the file size — is fully determined by the mmap `src_` is already
    // holding: each Column's `data` points into that mapping, so its offset is just the
    // pointer delta, and header_len sits at byte 8 of the map. Re-reading and re-PARSING
    // the header from disk to recover the same numbers cost ~15 ms on the campaign
    // corpus (a 2.4 MB header, 99% factor level strings) — per one-column write.
    // The reconstruction is used ONLY when `src_` is an owning map of exactly
    // src_path_; anything else takes the original disk path below, unchanged.
    const bool from_map = src_ && src_->owns_mmap() && src_->map_base() != nullptr &&
                          src_->path() == src_path_ && src_->map_size() >= 16;
    if (from_map) {
        const char* base = static_cast<const char*>(src_->map_base());
        if (std::memcmp(base, "CECONHB1", 8) != 0) return false;
        uint64_t hlen;
        std::memcpy(&hlen, base + 8, 8);
        src_size = static_cast<int64_t>(src_->map_size());
        if (16 + static_cast<int64_t>(hlen) > src_size) return false;
        src_data_start = align_up(16 + static_cast<int64_t>(hlen), kAlign);
        const char* dbase = base + src_data_start;
        for (const cecon::Column& c : src_->columns()) {
            if (c.data == nullptr) return false;  // unmapped column → take the disk path
            const int64_t off = static_cast<const char*>(c.data) - dbase;
            if (off < 0 || off + c.nbytes > src_size - src_data_start) return false;
            srccol[c.name] = { c.dtype == cecon::DType::Factor ? "factor" : "f64", c.nbytes, off };
        }
    } else {
        // --- read + parse the source header (authoritative on-disk layout) ---
        int sfd = native_open(src_path_, O_RDONLY);
        if (sfd < 0) return false;
        if (!native_fd_size(sfd, src_size)) { ::close(sfd); return false; }
        char hbuf[16];
        if (::pread(sfd, hbuf, 16, 0) != 16 || std::memcmp(hbuf, "CECONHB1", 8) != 0) { ::close(sfd); return false; }
        uint64_t src_hlen;
        std::memcpy(&src_hlen, hbuf + 8, 8);
        if (16 + static_cast<int64_t>(src_hlen) > src_size) { ::close(sfd); return false; }
        std::string shjson(src_hlen, '\0');
        if (::pread(sfd, &shjson[0], src_hlen, 16) != static_cast<ssize_t>(src_hlen)) { ::close(sfd); return false; }
        ::close(sfd);
        json sh;
        try { sh = json::parse(shjson); } catch (...) { return false; }
        if (!sh.contains("columns") || sh.value("n_rows", static_cast<int64_t>(-1)) != n_) return false;
        src_data_start = align_up(16 + static_cast<int64_t>(src_hlen), kAlign);
        for (const auto& c : sh.at("columns"))
            srccol[c.at("name").get<std::string>()] = { c.at("dtype").get<std::string>(),
                c.at("nbytes").get<int64_t>(), c.at("offset").get<int64_t>() };
    }
    if (cols_.size() < srccol.size()) return false;  // a source column was dropped → full rewrite

    // --- plan: classify frame columns; build the new header; collect pwrite/append work ---
    json jcols = json::array();
    std::vector<const std::string*> lvl_text;
    // LADDER: `narrow` non-null = widen this column when its turn to write comes, so
    // at most one widened column is alive at a time (same law as write_hbin_full).
    struct DirtyOp { const void* data; int64_t nbytes, file_off; const FrameColumn* narrow; };
    struct NewOp { const void* data; int64_t nbytes; size_t jidx; const FrameColumn* narrow; };
    std::vector<DirtyOp> dirty_ops;
    std::vector<NewOp> new_ops;
    for (const FrameColumn& c : cols_) {
        const int64_t nbytes = c.is_factor ? static_cast<int64_t>(c.codes.size()) * 4
                                           : static_cast<int64_t>(c.f64.size()) * 8;
        const bool narrow_col = !c.is_factor && c.f64.narrow();
        const void* data = c.is_factor ? static_cast<const void*>(c.codes.data())
                         : narrow_col  ? nullptr
                                       : static_cast<const void*>(c.f64.data());
        json jc;
        jc["name"] = c.name;
        jc["dtype"] = c.is_factor ? "factor" : "f64";
        if (c.is_factor) {
            if (const std::string* lt = carried_levels_text(src_.get(), c)) {
                jc["levels"] = levels_sentinel(lvl_text.size());  // spliced after dump()
                lvl_text.push_back(lt);
            } else {
                jc["levels"] = levels_json(c.levels.ref());  // the vector, not the proxy
            }
        }
        if (c.is_factor && hdrlog_path()) g_hdr_patch.levels += static_cast<int64_t>(c.levels.size());
        // P1-A0-FORMAT-CARRIAGE: same conditional emit as write_hbin_full. Both save
        // paths must agree on CONTENT or a clean save would keep the format while a
        // patched save silently dropped it (which of the two runs is a layout accident,
        // not a user-visible choice). Growing the header can exhaust the source's
        // reserved slack — that is already handled: the size check below falls back to
        // a full rewrite, which emits the same key.
        if (!c.format.empty()) jc["format"] = c.format;
        if (!c.label.empty()) jc["label"] = hbin_level_json_text(c.label);  // BUG-198/BUG-1372
        if (const int64_t nm = c.n_missing_if_known(); nm >= 0) jc["n_missing"] = nm;  // NMISS-CARRY: warm-only
        jc["nbytes"] = nbytes;
        auto it = srccol.find(c.name);
        if (it != srccol.end()) {
            // existing column: reuse its slot only if the on-disk layout still matches
            const std::string want = c.is_factor ? "factor" : "f64";
            if (it->second.dtype != want || it->second.nbytes != nbytes) return false;
            jc["offset"] = it->second.offset;
            if (!c.borrowed())  // dirty → overwrite in place; clean → clone already has it
                dirty_ops.push_back({ data, nbytes, src_data_start + it->second.offset,
                                      narrow_col ? &c : nullptr });
        } else {
            jc["offset"] = 0;  // placeholder; assigned in the append-layout pass
            new_ops.push_back({ data, nbytes, jcols.size(), narrow_col ? &c : nullptr });
        }
        jcols.push_back(std::move(jc));
    }
    // Write-path integrity: every SOURCE column must still be present BY NAME in this
    // frame. A missing one was renamed or dropped — patching would orphan its payload in
    // the clone while re-emitting the value under a new name (duplicated/inconsistent
    // file). Such frames are structure_dirty_ and don't reach here today (rename/drop go
    // to the full rewrite); this guards a future caller from corrupting a save. A pure
    // reorder passes trivially — its name set is a bijection with the source.
    {
        std::unordered_set<std::string> frame_names;
        for (const FrameColumn& c : cols_) frame_names.insert(c.name);
        for (const auto& kv : srccol) if (!frame_names.count(kv.first)) return false;
    }
    // append new columns after the (cloned) source EOF, 64B-aligned
    int64_t eof = src_size;
    for (const NewOp& np : new_ops) {
        int64_t off_abs = align_up(eof, kAlign);
        jcols[np.jidx]["offset"] = off_abs - src_data_start;
        eof = off_abs + np.nbytes;
    }
    // the re-dumped header must fit the source's reserved slack (data_start unchanged)
    json header;
    header["magic"] = "CECONHB1";
    header["version"] = 1;
    header["n_rows"] = n_;
    // BUG-272: both save paths must agree on CONTENT (the same law `format` states
    // above): the header is re-dumped from THIS frame's state, so a claim the source
    // file carried but this frame has since cleared (a setcol over a key column) is
    // dropped here exactly as the full writer would drop it — the patch path can
    // never resurrect a stale claim from the cloned source header.
    if (!sort_keys_.empty()) header["sorted_by"] = join_sort_keys(sort_keys_);
    header["columns"] = std::move(jcols);
    const int64_t dmp_t0 = hdrlog_path() ? hdr_now_ns() : 0;
    std::string hjson = header.dump();
    splice_levels(hjson, lvl_text);
    if (hdrlog_path()) g_hdr_patch.dump_ns += hdr_now_ns() - dmp_t0;
    if (hdrlog_path()) { ++g_hdr_patch.calls; g_hdr_patch.ns += hdr_now_ns() - hdr_t0; hdr_flush(); }
    const int64_t reservation = src_data_start - 16;
    if (static_cast<int64_t>(hjson.size()) > reservation) return false;  // slack exhausted → rewrite
    hjson.append(static_cast<size_t>(reservation - static_cast<int64_t>(hjson.size())), ' ');

    // --- clone the source, then patch it in place ---
#if defined(_WIN32)
    // No Windows clone primitive is wired yet (clone_file returns false there),
    // so this would unlink the destination and then bail — destroying a file the
    // caller still has. Return BEFORE that unlink and preserve an existing
    // destination; write_hbin() takes its ordinary full-writer fallback.
    return false;
#else
    native_unlink(out_path);
    if (!clone_file(src_path_, out_path)) return false;
    int ofd = native_open(out_path, O_RDWR);
    if (ofd < 0) { native_unlink(out_path); return false; }
    bool ok = true;
    auto pw = [&](const void* buf, int64_t n, int64_t off) {
        if (n > 0 && ::pwrite(ofd, buf, static_cast<size_t>(n), off) != static_cast<ssize_t>(n)) ok = false;
    };
    std::vector<double> widen_tmp;   // LADDER: reused; one widened column alive at a time
    auto payload = [&](const void* p2, const FrameColumn* nc) -> const void* {
        if (nc == nullptr) return p2;
        const CowVec<float>& nv = nc->f64.f32_ref();
        widen_tmp.assign(nv.begin(), nv.end());
        return widen_tmp.data();
    };
    for (const DirtyOp& d : dirty_ops) if (ok) pw(payload(d.data, d.narrow), d.nbytes, d.file_off);
    for (const NewOp& np : new_ops)
        if (ok) pw(payload(np.data, np.narrow), np.nbytes,
                   src_data_start + header["columns"][np.jidx]["offset"].get<int64_t>());
    if (ok) {  // re-dump header + header_len; data_start is unchanged so the clone stays valid
        uint64_t hlen = hjson.size();
        pw(&hlen, 8, 8);
        pw(hjson.data(), static_cast<int64_t>(hjson.size()), 16);
    }
    if (::close(ofd) != 0) ok = false;
    if (!ok) { native_unlink(out_path); return false; }
#endif
    // ENG-ADOPTDESC-NMISS: the output is the source's bytes with exactly the OWNED
    // columns overwritten, so every still-borrowed column's missing count is the
    // source's, verbatim. Carry those forward and pay a real count only for the
    // columns this frame actually wrote — turning the next adoption's whole-frame
    // scan into a per-column lookup. A source column we have no memo for is simply
    // omitted (the reader then scans that one column and nothing else).
    {
        std::vector<NmissMemoCol> src_known = frame_nmiss_memo_get(src_path_);
        std::unordered_map<std::string, const NmissMemoCol*> by_name;
        by_name.reserve(src_known.size());
        for (const NmissMemoCol& m : src_known) by_name[m.name] = &m;
        std::vector<NmissMemoCol> out_known;
        out_known.reserve(cols_.size());
        for (const FrameColumn& c : cols_) {
            const std::string dtype = c.is_factor ? "factor" : "f64";
            if (c.borrowed()) {
                auto it = by_name.find(c.name);
                // dtype must match too: a borrowed column keeps the source's bytes,
                // so a dtype disagreement means the memo is describing something
                // else — drop the column rather than guess.
                if (it != by_name.end() && it->second->dtype == dtype)
                    out_known.push_back({c.name, dtype, it->second->n_missing});
            } else {
                out_known.push_back({c.name, dtype, c.n_missing(n_)});
            }
        }
        frame_nmiss_memo_put(out_path, std::move(out_known));
    }
    return true;
}

// ---- ENG-ADOPTDESC-NMISS memo (contract in frame.h) -----------------------------
namespace {
struct NmissMemoEntry {
    std::string path;
    int64_t size = -1;
    int64_t mtime_ns = -1;
    std::vector<NmissMemoCol> cols;
};
// Bounded ring. The write chain is linear — each scratch file is written once and
// adopted once — so a handful of slots covers it; the bound is what keeps a long
// session from accumulating one entry per scratch file.
constexpr size_t kNmissMemoSlots = 6;
std::vector<NmissMemoEntry>& nmiss_memo() {
    static std::vector<NmissMemoEntry> v;
    return v;
}
// (size, mtime_ns) of `path`, or {-1,-1} when it cannot be stat'd.
std::pair<int64_t, int64_t> file_stamp(const std::string& path) {
    int64_t size = -1, mtime_ns = -1;
    if (!native_file_stamp(path, size, mtime_ns)) return {-1, -1};
    return {size, mtime_ns};
}
}  // namespace

void frame_nmiss_memo_put(const std::string& path, std::vector<NmissMemoCol> cols) {
    if (path.empty() || cols.empty()) return;
    auto [sz, ns] = file_stamp(path);
    if (sz < 0) return;  // unstattable → record nothing rather than something unverifiable
    std::vector<NmissMemoEntry>& v = nmiss_memo();
    for (NmissMemoEntry& e : v) {
        if (e.path == path) { e.size = sz; e.mtime_ns = ns; e.cols = std::move(cols); return; }
    }
    if (v.size() >= kNmissMemoSlots) v.erase(v.begin());
    v.push_back({path, sz, ns, std::move(cols)});
}

std::vector<NmissMemoCol> frame_nmiss_memo_get(const std::string& path) {
    if (path.empty()) return {};
    std::vector<NmissMemoEntry>& v = nmiss_memo();
    for (const NmissMemoEntry& e : v) {
        if (e.path != path) continue;
        auto [sz, ns] = file_stamp(path);
        // The file must be byte-for-byte the one that was measured. A rewrite in
        // place (`save, replace`) moves size or mtime and invalidates the entry.
        if (sz != e.size || ns != e.mtime_ns) return {};
        return e.cols;
    }
    return {};
}

bool Frame::is_pure_column_reorder_of_source() const {
    using nlohmann::json;
    if (src_path_.empty()) return false;
    int fd = native_open(src_path_, O_RDONLY);
    if (fd < 0) return false;
    char hbuf[16];
    if (::pread(fd, hbuf, 16, 0) != 16 || std::memcmp(hbuf, "CECONHB1", 8) != 0) { ::close(fd); return false; }
    uint64_t hlen;
    std::memcpy(&hlen, hbuf + 8, 8);
    std::string hj(hlen, '\0');
    ssize_t got = ::pread(fd, &hj[0], hlen, 16);
    ::close(fd);
    if (got != static_cast<ssize_t>(hlen)) return false;
    json sh;
    try { sh = json::parse(hj); } catch (...) { return false; }
    if (!sh.contains("columns")) return false;
    if (sh.value("n_rows", static_cast<int64_t>(-1)) != n_) return false;      // rows changed
    const auto& scols = sh.at("columns");
    if (scols.size() != cols_.size()) return false;                            // add/drop → not a reorder
    // Source name → {dtype, nbytes}; require an exact bijection with this frame (equal
    // counts + every frame name present with matching shape ⇒ the source names match too).
    std::unordered_map<std::string, std::pair<std::string, int64_t>> src;
    src.reserve(scols.size());
    for (const auto& c : scols)
        src.emplace(c.at("name").get<std::string>(),
                    std::make_pair(c.at("dtype").get<std::string>(), c.at("nbytes").get<int64_t>()));
    for (const FrameColumn& c : cols_) {
        auto it = src.find(c.name);
        if (it == src.end()) return false;                                     // renamed/new column
        const int64_t nb = c.is_factor ? static_cast<int64_t>(c.codes.size()) * 4
                                       : static_cast<int64_t>(c.f64.size()) * 8;
        const std::string want = c.is_factor ? "factor" : "f64";
        if (it->second.first != want || it->second.second != nb) return false; // dtype/shape changed
    }
    return true;
}

void Frame::force_own_all() {
    for (FrameColumn& c : cols_) {
        c.f64.materialize();
        c.codes.materialize();
        // Wave β1: a factor column's levels may still borrow src_'s (or, transitively,
        // an ancestor Dataset's) strings — materialize before src_.reset() below, or a
        // still-borrowed dictionary is left pointing at memory this Frame no longer
        // keeps alive. materialize() is a no-op for an already-owned/non-factor column.
        c.levels.materialize();
    }
    src_.reset();  // released the borrowed mmap; safe to overwrite the source file now
    src_path_.clear();
}

// ===========================================================================
// MI-066 data verbs — Stata-parity ports of PIET's Python backend (pandas)
// implementations, matching column-for-column so PIET 2.0's native core can
// swap this in with zero behavior change. Oracle files (read-only reference):
//   piet/backend/piet/gsort.py + editor.py:run_sort   (gsort)
//   piet/backend/piet/order.py                        (order)
//   piet/backend/piet/append.py                        (append)
//   piet/backend/piet/duplicates.py                    (duplicates)
//   piet/backend/piet/recode.py + editor.py:_apply_recode  (recode)
//   piet/backend/piet/merge_op.py                       (merge, see merge_frames below)
//   piet/backend/piet/reshape.py                        (reshape long/wide)
// ===========================================================================

namespace {
// Build a row-permutation Frame (shared by gsort + duplicates_drop + order's
// row-identity passthrough): copies every column at the given row indices.
Frame take_rows(const Frame& src, const std::vector<int64_t>& idx) {
    Frame::note_materialization(static_cast<int64_t>(src.n_cols()));  // WS-A census: close the generation
    Frame out;
    out.set_n_rows_hint(static_cast<int64_t>(idx.size()));
    for (const FrameColumn& c : src.columns()) {
        if (c.is_factor) {
            std::vector<int32_t> codes(idx.size());
            for (size_t i = 0; i < idx.size(); ++i) codes[i] = c.codes[idx[i]];
            out.append_factor_column_coded(c.name, std::move(codes), c.levels.ref());
            out.carry_identity_to_last(c);  // BUG-363: identity metadata
        } else if (c.f64.narrow()) {
            // LADDER: gather narrow to narrow — this is the funnel every row-axis verb
            // (sort/gsort/joinby/sample) passes through, and reading it through the
            // widening proxy is what re-widened whole frames in attempt 7.
            const CowVec<float>& src = c.f64.f32_ref();
            std::vector<float> vals(idx.size());
            for (size_t i = 0; i < idx.size(); ++i) vals[i] = src[idx[i]];
            out.append_f64_column_narrow(c.name, std::move(vals));
            out.carry_identity_to_last(c);  // BUG-363
        } else {
            const F64Reader r = c.f64.reader();
            std::vector<double> vals(idx.size());
            for (size_t i = 0; i < idx.size(); ++i) vals[i] = r[static_cast<std::size_t>(idx[i])];
            out.append_f64_column(c.name, std::move(vals));
        }
    }
    out.finalize_after_bulk_load();
    return out;
}

// Build a hashable per-row key tuple over `cols` (factor -> code as double,
// f64 -> value; NaN/-1 both normalize to a single "missing" sentinel so two
// missing keys compare equal — matches PIET's duplicates.py / merge_op.py,
// which both use `None` for any missing key component).
std::vector<std::vector<double>> key_cols(const Frame& fr, const std::vector<std::string>& names) {
    return cols_as_double(fr, fr.n_rows(), names);
}
// Row key as a vector<double> with NaN normalized to a single canonical NaN
// bit pattern so it hashes/compares consistently as a map/set key.
struct RowKey {
    std::vector<double> v;
    bool operator==(const RowKey& o) const {
        if (v.size() != o.v.size()) return false;
        for (size_t i = 0; i < v.size(); ++i) {
            bool na = std::isnan(v[i]), nb = std::isnan(o.v[i]);
            if (na != nb) return false;
            if (!na && v[i] != o.v[i]) return false;
        }
        return true;
    }
};
struct RowKeyHash {
    size_t operator()(const RowKey& k) const {
        size_t h = 1469598103934665603ull;
        for (double x : k.v) {
            uint64_t bits;
            if (std::isnan(x)) bits = 0x7ff8000000000000ull;  // canonical NaN bit pattern
            else std::memcpy(&bits, &x, 8);
            h ^= bits;
            h *= 1099511628211ull;
        }
        return h;
    }
};
RowKey make_key(const std::vector<std::vector<double>>& K, int64_t row) {
    RowKey k;
    k.v.reserve(K.size());
    // BUG-108: normalize -0.0 -> +0.0 so a key value's HASH (RowKeyHash memcpy's the
    // raw bits, and -0.0 and +0.0 differ bitwise) stays consistent with RowKey's
    // NUMERIC equality (-0.0 == +0.0 is true). Without this, a -0.0 key and a +0.0
    // key compare EQUAL but hash to DIFFERENT buckets — breaking the unordered_map/set
    // invariant, so merge/duplicates/reshape_wide silently fail to match the pair.
    // Mirrors the engine's own FE/cluster key normalization (model.cpp key_at, BUG-093);
    // `x == 0.0` is true for both zeros and false for NaN, so NaN passes through unchanged.
    for (const auto& col : K) { double x = col[row]; k.v.push_back(x == 0.0 ? 0.0 : x); }
    return k;
}

// joinby's per-column OUTPUT gather (see Frame::joinby's doc comment in
// frame.h): copies `src`'s values at row indices `idx` onto `out` as a NEW
// column named `src.name`. A factor column gets a BRAND-NEW dictionary
// rebuilt from scratch in first-seen OUTPUT-row order — mirrors
// Frame::from_csv's per-column dict-build loop (frame.cpp ~899-925), which is
// what runJoinby's CSV+`cobra convert` round-trip actually produces: a row
// that doesn't survive the join never contributes a value, so the dictionary
// must be rebuilt over the SURVIVING rows only, not copied from the source's
// own (possibly larger/differently-ordered) table (contrast Frame::append /
// merge_frames above, which reuse-and-extend the source's own dictionary —
// every source row survives THOSE verbs, so there is nothing to shrink). f64
// values are gathered directly: value-identical to, and more precise than,
// the oracle's text pythonFloatRepr()/reparse round-trip.
// NMISS-CARRY (2026-08-21): count the gathered missings HERE, where the loop that
// builds the joined column already reads every element. This is the other half of
// the keep_if seeding: without it a join's output columns reach the writer with a
// COLD cache, so emitting the header key forced a fresh O(rows) scan at write time
// — measured, that cost `drop|if` 24 s and cancelled most of the joinby/merge win
// the carry bought. With both halves seeded the writer only ever states what some
// loop already computed, so the carry can save a scan and can never cause one.
void joinby_gather_append(Frame& out, const FrameColumn& src, const std::vector<int64_t>& idx) {
    int64_t miss = 0;
    if (!src.is_factor) {
        std::vector<double> vals(idx.size());
        for (size_t i = 0; i < idx.size(); ++i) {
            vals[i] = src.f64[static_cast<size_t>(idx[i])];
            if (std::isnan(vals[i])) ++miss;
        }
        out.append_f64_column(src.name, std::move(vals));
        // The column just appended is the LAST one — never look it up by name:
        // joinby appends using-side columns that may collide with a master name,
        // and Frame::col() both THROWS on a miss and returns the FIRST match, so a
        // name lookup here either kills the join or seeds the wrong column.
        out.columns().back().seed_n_missing(miss);
        // BUG-363: carry the identity metadata too. A gather rebuilds the column
        // from values, so `format` and `label` — which FrameColumn documents as
        // identity metadata rather than payload — were dropped on the floor, and
        // a %9s string column came back as %9.0g. Stata changes no display format
        // across joinby/merge/append/expand/cross/fillin/tsfill.
        out.carry_identity_to_last(src);
        return;
    }
    std::vector<int32_t> codes(idx.size());
    std::vector<std::string> levels;
    std::unordered_map<std::string, int32_t> dict;
    for (size_t i = 0; i < idx.size(); ++i) {
        int32_t code = src.codes[static_cast<size_t>(idx[i])];
        if (code < 0) { codes[i] = -1; ++miss; continue; }
        const std::string& lvl = src.levels[static_cast<size_t>(code)];
        auto it = dict.find(lvl);
        if (it == dict.end()) {
            int32_t nc = static_cast<int32_t>(levels.size());
            dict.emplace(lvl, nc);
            levels.push_back(lvl);
            codes[i] = nc;
        } else {
            codes[i] = it->second;
        }
    }
    out.append_factor_column_coded(src.name, std::move(codes), std::move(levels));
    out.columns().back().seed_n_missing(miss);  // last-appended, not by name (see above)
    out.carry_identity_to_last(src);   // BUG-363, see the f64 branch above
}
}  // namespace

Frame Frame::gsort(const std::vector<std::string>& keys, const std::vector<bool>& ascending) const {
    std::vector<int64_t> order(n_);
    std::iota(order.begin(), order.end(), 0);
    // BUG-532: a stable ascending sort by a leading prefix of the claim is the
    // identity permutation. The claim is never false (minted below only when true in
    // Stata's order, or adopted from a .dta's own <sortlist>), so the rows already sit
    // in the order the sort would return, and the radix pass is skipped. That holds
    // for a .dta-adopted string/factor-leading claim too, whose rows are in Stata's
    // (byte) order where a re-sort by dictionary code need not be. The claim becomes
    // the requested keys, not the longer one: Stata's `sort g` on data sorted by g x
    // leaves `: sortedby` = "g" (w2e_probes.log bo_sort_prefix_noop), and a prefix of
    // a true claim is true. Any descending key, or a key list that is not a leading
    // prefix of the claim, takes the full path below.
    bool prefix_of_claim = !keys.empty() && keys.size() <= sort_keys_.size();
    for (size_t i = 0; prefix_of_claim && i < keys.size(); ++i)
        if (keys[i] != sort_keys_[i] || (i < ascending.size() && !ascending[i])) prefix_of_claim = false;
    if (prefix_of_claim) {
        Frame out = take_rows(*this, order);
        out.set_sorted_by(keys);
        return out;
    }
    if (!keys.empty()) {
        std::vector<std::vector<double>> K = key_cols(*this, keys);
        std::vector<char> asc(keys.size(), 1);
        bool stata_order = false;
        for (size_t k = 0; k < keys.size(); ++k) {
            if (k < ascending.size()) asc[k] = ascending[k] ? 1 : 0;
            const FrameColumn& c = col(keys[k]);
            if (c.is_factor) {
                // Dictionary codes are first-seen, not lexical. Rank the level
                // bytes once; blank and a missing string code are the same key.
                std::vector<int32_t> levels(c.levels.size());
                std::iota(levels.begin(), levels.end(), 0);
                std::sort(levels.begin(), levels.end(), [&](int32_t a, int32_t b) {
                    return c.levels[static_cast<size_t>(a)] < c.levels[static_cast<size_t>(b)];
                });
                std::vector<double> rank(c.levels.size(), 0.0);
                double next = 0;
                for (int32_t code : levels)
                    if (!c.levels[static_cast<size_t>(code)].empty())
                        rank[static_cast<size_t>(code)] = ++next;
                for (int64_t r = 0; r < n_; ++r) {
                    const int32_t code = c.codes[static_cast<size_t>(r)];
                    K[k][static_cast<size_t>(r)] = code < 0 ? 0.0 : rank[static_cast<size_t>(code)];
                }
                stata_order = true;
            } else {
                for (double v : K[k])
                    if (missing_kind(v) > 0) { stata_order = true; break; }
            }
        }
        if (stata_order) {
            // Stata orders extended missings by kind. gsort keeps all missing
            // values after the finite values in both directions, reversing the
            // missing kinds themselves for a descending key (w2c_functions
            // sort_v_order / gsort_v_desc). Equal keys retain observation order.
            std::stable_sort(order.begin(), order.end(), [&](int64_t a, int64_t b) {
                for (size_t k = 0; k < K.size(); ++k) {
                    const double x = K[k][static_cast<size_t>(a)];
                    const double y = K[k][static_cast<size_t>(b)];
                    const int mx = missing_kind(x), my = missing_kind(y);
                    if (mx >= 0 || my >= 0) {
                        if (mx < 0) return true;
                        if (my < 0) return false;
                        if (mx != my) return asc[k] ? mx < my : mx > my;
                    } else if (x != y) {
                        return asc[k] ? x < y : x > y;
                    }
                }
                return false;
            });
        } else {
            // Ordinary numeric keys retain the value-identical radix fast path.
            order = cecon::radixsort::gsort_radix_order(K, asc, n_);
        }
    }
    Frame out = take_rows(*this, order);
    // Stata's sortlist records the leading ascending prefix even when later
    // keys descend (w2e_probes.log bo_gsort_marker: `gsort g -x` -> "g").
    // A descending-first result has no representable ascending claim.
    size_t prefix = 0;
    while (prefix < keys.size() && (prefix >= ascending.size() || ascending[prefix])) ++prefix;
    if (prefix) out.set_sorted_by(std::vector<std::string>(keys.begin(), keys.begin() + prefix));
    return out;
}

// Shared by both order() overloads: the full reordered column-name list.
static std::vector<std::string> order_names(const std::vector<FrameColumn>& cols,
                                            const std::vector<std::string>& varlist,
                                            const std::string& mode,
                                            const std::string& anchor) {
    std::vector<std::string> all_names;
    all_names.reserve(cols.size());
    for (const auto& c : cols) all_names.push_back(c.name);

    std::unordered_map<std::string, char> in_list;
    for (const auto& v : varlist) in_list[v] = 1;
    std::vector<std::string> remaining;
    for (const auto& n : all_names) if (!in_list.count(n)) remaining.push_back(n);

    std::vector<std::string> new_order;
    if (mode == "last") {
        new_order = remaining;
        new_order.insert(new_order.end(), varlist.begin(), varlist.end());
    } else if (mode == "before" || mode == "after") {
        auto it = std::find(remaining.begin(), remaining.end(), anchor);
        if (it == remaining.end()) throw std::runtime_error("order: anchor variable '" + anchor + "' not found");
        size_t pos = static_cast<size_t>(it - remaining.begin());
        size_t cut = (mode == "before") ? pos : pos + 1;
        new_order.assign(remaining.begin(), remaining.begin() + cut);
        new_order.insert(new_order.end(), varlist.begin(), varlist.end());
        new_order.insert(new_order.end(), remaining.begin() + cut, remaining.end());
    } else {  // "first" (default)
        new_order = varlist;
        new_order.insert(new_order.end(), remaining.begin(), remaining.end());
    }
    return new_order;
}

Frame Frame::order(const std::vector<std::string>& varlist, const std::string& mode,
                   const std::string& anchor) const& {
    Frame out;
    out.n_ = n_;
    for (const auto& name : order_names(cols_, varlist, mode, anchor)) out.cols_.push_back(col(name));
    out.reindex();
    // BUG-272: `order` is a pure COLUMN permutation — no row moves, no value or name
    // changes — so the sort claim carries verbatim (Stata's own `order` keeps the
    // sortlist standing too). One of the two carried fresh-Frame transforms; every
    // other fresh-Frame verb stays claimless by construction.
    out.sort_keys_ = sort_keys_;
    return out;
}

Frame Frame::order(const std::vector<std::string>& varlist, const std::string& mode,
                   const std::string& anchor) && {
    // A duplicated varlist entry makes new_order name a column twice; the copy
    // overload duplicates it, but moving would leave the second occurrence empty —
    // fall back to the copy path to preserve exact semantics.
    std::unordered_map<std::string, char> seen;
    for (const auto& v : varlist)
        if (seen[v]++) return static_cast<const Frame&>(*this).order(varlist, mode, anchor);

    Frame out;
    out.n_ = n_;
    // BUG-272: same pure-column-permutation carry as the copy overload above. Taken
    // BEFORE the mutable_col() moves below — mutable_col clears the claim it hands
    // out (its callers may rewrite values), but these moves only relocate columns,
    // and `this` is an expiring rvalue whose own claim no longer matters.
    out.sort_keys_ = sort_keys_;
    for (const auto& name : order_names(cols_, varlist, mode, anchor))
        out.cols_.push_back(std::move(mutable_col(name)));
    out.reindex();
    // The moved columns may still BORROW this Frame's mmap, so `out` must keep that
    // mmap alive (this Frame is an expiring rvalue). Mark structure_dirty: a column
    // permutation can't be patched onto the source, so it takes the full-rewrite path
    // (byte-identical to before).
    out.src_ = src_;
    out.src_path_ = src_path_;
    out.structure_dirty_ = true;
    return out;
}

// `rename old new [, ...]` — see frame.h doc comment for the atomicity rule.
void Frame::rename(const std::vector<std::pair<std::string, std::string>>& pairs) {
    // Pass 1: every `old` must exist, and resolve its index against the
    // PRE-rename idx_ (so a later pair in this same batch can never shadow an
    // earlier lookup — this is what makes a chain/swap like old=a,new=b +
    // old=b,new=a apply as if simultaneous).
    std::unordered_set<std::string> old_set;
    std::vector<int> idxs;
    idxs.reserve(pairs.size());
    for (const auto& pr : pairs) {
        auto it = idx_.find(pr.first);
        if (it == idx_.end())
            throw EngineError("missing_column", "rename: variable '" + pr.first + "' not found", pr.first);
        if (!old_set.insert(pr.first).second)
            throw EngineError("bad_input", "rename: variable '" + pr.first + "' renamed more than once", pr.first);
        idxs.push_back(it->second);
    }
    // Pass 2: every `new` must not collide with a FINAL survivor — a current
    // column name that is NOT itself one of the `old`s being renamed away.
    std::unordered_set<std::string> final_names;
    for (const auto& c : cols_) if (!old_set.count(c.name)) final_names.insert(c.name);
    for (const auto& pr : pairs) {
        if (pr.second.empty())
            throw EngineError("bad_input", "rename: empty target name for '" + pr.first + "'");
        if (!final_names.insert(pr.second).second)
            throw EngineError("bad_input", "rename: duplicate/invalid target name '" + pr.second + "'", pr.second);
    }
    // Pass 3: apply. Only FrameColumn::name changes — every value/dtype/factor
    // dictionary byte is untouched.
    for (size_t i = 0; i < pairs.size(); ++i) cols_[static_cast<size_t>(idxs[i])].name = pairs[i].second;
    reindex();
    structure_dirty_ = true;  // column identity changed → can't cleanly patch onto the source (mirrors order()/drop())
    // BUG-272: a renamed sort-claim key FOLLOWS the rename (BUG-191's law: identity
    // is the payload, not the name — the bytes and the row order are untouched, so
    // the claim is exactly as true under the new name). Applied against the
    // PRE-rename key names, same as the batch itself, so swaps/chains stay correct.
    if (!sort_keys_.empty()) {
        std::unordered_map<std::string, std::string> remap;
        for (const auto& pr : pairs) remap[pr.first] = pr.second;
        for (std::string& k : sort_keys_) {
            auto it = remap.find(k);
            if (it != remap.end()) k = it->second;
        }
    }
}

// `clonevar dst = src [, ...]` — see frame.h doc comment for the atomicity rule.
void Frame::clonevar(const std::vector<std::pair<std::string, std::string>>& pairs) {
    std::unordered_set<std::string> dst_set;
    for (const auto& pr : pairs) {
        if (!has(pr.first))
            throw EngineError("missing_column", "clonevar: source variable '" + pr.first + "' not found", pr.first);
        if (pr.second.empty())
            throw EngineError("bad_input", "clonevar: empty target name for '" + pr.first + "'");
        if (has(pr.second) || !dst_set.insert(pr.second).second)
            throw EngineError("bad_input", "clonevar: variable '" + pr.second + "' already exists", pr.second);
    }
    for (const auto& pr : pairs) {
        FrameColumn nc = col(pr.first);  // column-CoW copy: shares the payload with `src`, src untouched
        nc.name = pr.second;
        cols_.push_back(std::move(nc));
    }
    reindex();
}

// `encode NEW = SRC` — see frame.h doc comment for the alphabetical-code rule.
std::vector<std::string> Frame::encode(const std::string& src, const std::string& dst) {
    if (!has(src)) throw EngineError("missing_column", "encode: variable '" + src + "' not found", src);
    const FrameColumn& sc = col(src);
    if (!sc.is_factor)
        throw EngineError("bad_input", "encode: variable '" + src + "' is not a factor/string column", src);
    if (has(dst)) throw EngineError("bad_input", "encode: variable '" + dst + "' already exists", dst);

    const int32_t nlev = static_cast<int32_t>(sc.levels.size());
    std::vector<char> used(static_cast<size_t>(nlev > 0 ? nlev : 0), 0);
    for (int64_t i = 0; i < n_; ++i) {
        int32_t c = sc.codes[i];
        if (c >= 0 && c < nlev) used[static_cast<size_t>(c)] = 1;
    }
    // Distinct, USED, non-empty labels — sorted ALPHABETICALLY (byte-wise
    // std::string::operator<, matching this codebase's one other "alphabetical
    // level order" site: egen_group()'s c.levels[a] < c.levels[b] above).
    std::vector<int32_t> used_codes;
    for (int32_t c = 0; c < nlev; ++c)
        if (used[static_cast<size_t>(c)] && !sc.levels[static_cast<size_t>(c)].empty()) used_codes.push_back(c);
    std::sort(used_codes.begin(), used_codes.end(), [&](int32_t x, int32_t y) {
        return sc.levels[static_cast<size_t>(x)] < sc.levels[static_cast<size_t>(y)];
    });

    std::unordered_map<int32_t, double> rank_of;  // original code -> 1-based alphabetical rank
    rank_of.reserve(used_codes.size());
    std::vector<std::string> level_labels;
    level_labels.reserve(used_codes.size());
    for (size_t r = 0; r < used_codes.size(); ++r) {
        rank_of[used_codes[r]] = static_cast<double>(r + 1);
        level_labels.push_back(sc.levels[static_cast<size_t>(used_codes[r])]);
    }

    std::vector<double> out_col(static_cast<size_t>(n_), NAd);
    for (int64_t i = 0; i < n_; ++i) {
        int32_t c = sc.codes[i];
        if (c < 0) continue;  // SRC missing -> NEW missing
        auto it = rank_of.find(c);
        if (it != rank_of.end()) out_col[static_cast<size_t>(i)] = it->second;  // else: empty-label code -> stays missing
    }
    append_f64_column(dst, std::move(out_col));
    reindex();
    return level_labels;
}

Frame Frame::append(const Frame& usingf) const {
    std::vector<std::string> all_cols;
    std::unordered_map<std::string, char> seen;
    for (const auto& c : cols_) { all_cols.push_back(c.name); seen[c.name] = 1; }
    for (const auto& c : usingf.cols_) if (!seen.count(c.name)) { all_cols.push_back(c.name); seen[c.name] = 1; }

    const int64_t nm = n_, nu = usingf.n_;
    Frame out;
    out.n_ = nm + nu;

    for (const auto& name : all_cols) {
        bool in_m = idx_.count(name) > 0, in_u = usingf.idx_.count(name) > 0;
        FrameColumn nc;
        nc.name = name;
        // BUG-363, the ninth site. `format`/`label` are identity metadata, so an
        // appended column keeps them: the MASTER's when it has the column (the
        // same "master wins" rule joinby and merge use for a shared name), the
        // using side's for a using-only column. COUPLED BY CONTRACT with
        // write_hbin_append's header keys — that writer's byte-identity contract
        // is with THIS function, so the two change together or the a2 A/B
        // byte-compare fires (see its P1-A0-FORMAT-CARRIAGE comment).
        if (in_m) { const FrameColumn& src = col(name); nc.format = src.format; nc.label = src.label; }
        else      { const FrameColumn& src = usingf.col(name); nc.format = src.format; nc.label = src.label; }
        if (in_m && in_u) {
            const FrameColumn& mc = col(name);
            const FrameColumn& uc = usingf.col(name);
            if (!mc.is_factor && !uc.is_factor) {
                nc.is_factor = false;
                nc.f64 = mc.f64;
                nc.f64.insert(nc.f64.end(), uc.f64.begin(), uc.f64.end());
            } else if (mc.is_factor && uc.is_factor) {
                nc.is_factor = true;
                nc.levels = mc.levels;
                std::unordered_map<std::string, int32_t> dict;
                for (size_t i = 0; i < nc.levels.size(); ++i) dict[nc.levels[i]] = static_cast<int32_t>(i);
                std::vector<int32_t> remap(uc.levels.size());
                for (size_t i = 0; i < uc.levels.size(); ++i) {
                    auto it = dict.find(uc.levels[i]);
                    if (it != dict.end()) remap[i] = it->second;
                    else { remap[i] = static_cast<int32_t>(nc.levels.size()); dict[uc.levels[i]] = remap[i]; nc.levels.push_back(uc.levels[i]); }
                }
                nc.codes = mc.codes;
                nc.codes.reserve(nm + nu);
                for (int32_t code : uc.codes) nc.codes.push_back(code < 0 ? -1 : remap[code]);
            } else {
                // mixed dtype -> both promoted to f64 (codes as float; matches append.py _concat_mixed)
                nc.is_factor = false;
                nc.f64.reserve(nm + nu);
                if (!mc.is_factor) nc.f64.insert(nc.f64.end(), mc.f64.begin(), mc.f64.end());
                else for (int32_t code : mc.codes) nc.f64.push_back(code < 0 ? NAd : static_cast<double>(code));
                if (!uc.is_factor) nc.f64.insert(nc.f64.end(), uc.f64.begin(), uc.f64.end());
                else for (int32_t code : uc.codes) nc.f64.push_back(code < 0 ? NAd : static_cast<double>(code));
            }
        } else if (in_m) {
            const FrameColumn& mc = col(name);
            nc.is_factor = mc.is_factor;
            if (mc.is_factor) {
                nc.levels = mc.levels;
                nc.codes = mc.codes;
                nc.codes.resize(nm + nu, -1);
            } else {
                nc.f64 = mc.f64;
                nc.f64.resize(nm + nu, NAd);
            }
        } else {  // in_u only
            const FrameColumn& uc = usingf.col(name);
            nc.is_factor = uc.is_factor;
            if (uc.is_factor) {
                nc.levels = uc.levels;
                nc.codes.assign(nm, -1);
                nc.codes.insert(nc.codes.end(), uc.codes.begin(), uc.codes.end());
            } else {
                nc.f64.assign(nm, NAd);
                nc.f64.insert(nc.f64.end(), uc.f64.begin(), uc.f64.end());
            }
        }
        out.cols_.push_back(std::move(nc));
    }
    out.reindex();
    return out;
}

// `joinby k1 [k2 ...] using FILE` — see frame.h's doc comment for the full
// rule. Oracle: piet/native/src/handlers_data_verbs.cpp runJoinby (~2518+).
Frame Frame::joinby(const Frame& usingf, const std::vector<std::string>& keys,
                    const std::string& unmatched, const std::string& marker) const {
    if (keys.empty()) throw EngineError("bad_input", "joinby: key variable list required");
    for (const auto& k : keys)
        if (!has(k)) throw EngineError("bad_input", "joinby: variable '" + k + "' not found", k);
    for (const auto& k : keys)
        if (!usingf.has(k))
            throw EngineError("bad_input", "joinby: key variable '" + k + "' not found in using dataset", k);

    // Column-name conflict: a NON-key column present (by name) on both sides
    // is ambiguous under joinby's bare inner-join (no update/replace value
    // reconciliation) — reject rather than silently pick a side. Mirrors
    // runJoinby's own check (handlers_data_verbs.cpp:2538-2566).
    std::unordered_set<std::string> keySet(keys.begin(), keys.end());
    std::unordered_set<std::string> masterNonKey;
    for (const auto& c : cols_) if (!keySet.count(c.name)) masterNonKey.insert(c.name);
    std::vector<std::string> overlap;
    for (const auto& c : usingf.cols_) {
        if (keySet.count(c.name)) continue;
        if (masterNonKey.count(c.name)) overlap.push_back(c.name);
    }
    // Stata's joinby takes a shared variable from the MASTER unless update/
    // replace is given ([D] joinby) — the same default `merge` uses, and the
    // same rule `cross` follows. Not an error. Oracle: StataBE 19.5 on the P1
    // wave fixture, `joinby id using wave_lookup` where both sides carry
    // cluster/stratum/group — 20 rows out, master's copies retained. The
    // gather loop below already skips any using column the master has, so the
    // overlap list is informational only.
    (void)overlap;

    const int64_t nm = n_, nu = usingf.n_;

    // Per-key match representation: f64 keys compare by VALUE (make_key below
    // normalizes NaN-matches-NaN and ±0, same as merge_frames); factor keys
    // compare by LEVEL STRING via a unified table (master-first, then
    // using-only levels appended — same convention merge_frames uses), since
    // the two files' per-column int32 code spaces are unrelated. A key that
    // is factor on one side and f64 on the other can NEVER produce a
    // matching pair for ANY row: PIET's own composite join-key string
    // prefixes every token by dtype before hashing (joinbyRowKeys,
    // handlers_data_verbs.cpp:2471-2514), so a mismatched column's tokens
    // differ on every row, forcing zero matches for the WHOLE composite key
    // — reproduced here as an empty-result short-circuit rather than
    // merge_frames' hard error (joinby's own oracle does not reject this).
    bool any_dtype_mismatch = false;
    std::vector<std::vector<double>> MK(keys.size()), UK(keys.size());
    for (size_t ki = 0; ki < keys.size(); ++ki) {
        const FrameColumn& mc = col(keys[ki]);
        const FrameColumn& uc = usingf.col(keys[ki]);
        if (mc.is_factor != uc.is_factor) { any_dtype_mismatch = true; continue; }
        if (!mc.is_factor) {
            MK[ki].assign(mc.f64.begin(), mc.f64.end());
            UK[ki].assign(uc.f64.begin(), uc.f64.end());
        } else {
            std::vector<std::string> levels = mc.levels.ref();
            std::unordered_map<std::string, int32_t> dict;
            for (size_t i = 0; i < levels.size(); ++i) dict[levels[i]] = static_cast<int32_t>(i);
            std::vector<int32_t> remap(uc.levels.size());
            for (size_t i = 0; i < uc.levels.size(); ++i) {
                auto it = dict.find(uc.levels[i]);
                if (it != dict.end()) { remap[i] = it->second; }
                else { remap[i] = static_cast<int32_t>(levels.size()); dict[uc.levels[i]] = remap[i]; levels.push_back(uc.levels[i]); }
            }
            MK[ki].resize(static_cast<size_t>(nm));
            for (int64_t i = 0; i < nm; ++i)
                MK[ki][static_cast<size_t>(i)] = mc.codes[i] < 0 ? NAd : static_cast<double>(mc.codes[i]);
            UK[ki].resize(static_cast<size_t>(nu));
            for (int64_t i = 0; i < nu; ++i) {
                int32_t code = uc.codes[i];
                UK[ki][static_cast<size_t>(i)] = (code < 0) ? NAd : static_cast<double>(remap[static_cast<size_t>(code)]);
            }
        }
    }

    // pairs: (master_row, using_row) — master row order outer, using row
    // order inner (joinby's own within-key Cartesian cross); a master row
    // with zero using matches contributes nothing (unmatched(none), the only
    // mode). Mirrors runJoinby's `pairs` build (handlers_data_verbs.cpp:
    // 2573-2594), including missing-key-matches-missing-key (both the f64 NaN
    // and the factor-remapped-missing paths above feed NAd into make_key,
    // whose RowKey equality/hash treat NaN as a single canonical value).
    std::vector<std::pair<int64_t, int64_t>> pairs;
    if (!any_dtype_mismatch) {
        std::unordered_map<RowKey, std::vector<int64_t>, RowKeyHash> u_by_key;
        for (int64_t i = 0; i < nu; ++i) u_by_key[make_key(UK, i)].push_back(i);
        for (int64_t i = 0; i < nm; ++i) {
            auto it = u_by_key.find(make_key(MK, i));
            if (it == u_by_key.end()) continue;
            for (int64_t ur : it->second) pairs.emplace_back(i, ur);
        }
    }

    const bool keepMaster = unmatched == "master" || unmatched == "both";
    const bool keepUsing = unmatched == "using" || unmatched == "both";
    if (keepMaster || keepUsing || !marker.empty()) {
        // joinby.ado's two merges: a master row with no using match stays (code 1)
        // under master/both; a using row whose key the master lacks comes in
        // (code 2) under using/both, carrying only its keys and the using's own
        // variables (a shared variable is the master's, missing there).
        std::vector<std::pair<int64_t, int64_t>> all;
        std::vector<double> codes;
        std::vector<char> m_matched(static_cast<size_t>(nm), 0), u_matched(static_cast<size_t>(nu), 0);
        for (const auto& p : pairs) { m_matched[static_cast<size_t>(p.first)] = 1; u_matched[static_cast<size_t>(p.second)] = 1; }
        size_t pi = 0;
        for (int64_t i = 0; i < nm; ++i) {
            if (m_matched[static_cast<size_t>(i)]) {
                while (pi < pairs.size() && pairs[pi].first == i) { all.push_back(pairs[pi]); codes.push_back(3.0); ++pi; }
            } else if (keepMaster) {
                all.emplace_back(i, -1);
                codes.push_back(1.0);
            }
        }
        if (keepUsing) {
            std::unordered_set<RowKey, RowKeyHash> m_keys;
            if (!any_dtype_mismatch)
                for (int64_t i = 0; i < nm; ++i) m_keys.insert(make_key(MK, i));
            for (int64_t j = 0; j < nu; ++j) {
                if (u_matched[static_cast<size_t>(j)]) continue;
                if (!any_dtype_mismatch && m_keys.count(make_key(UK, j))) continue;
                all.emplace_back(-1, j);
                codes.push_back(2.0);
            }
        }
        const size_t n = all.size();
        // One gather per column over (master row | using row): the value from the
        // side that has the row, missing otherwise; a factor gets a fresh
        // dictionary in first-seen order (joinby_gather_append's convention).
        Frame out;
        out.set_n_rows_hint(static_cast<int64_t>(n));
        auto append_col = [&](const FrameColumn* mc, const FrameColumn* uc, bool usingSide) {
            // mc: the master's column (or null); uc: the using's (or null).
            // usingSide: a using-only column (value from the using row).
            const FrameColumn& src = mc ? *mc : *uc;
            int64_t miss = 0;
            if (!src.is_factor) {
                std::vector<double> vals(n, NAd);
                for (size_t r = 0; r < n; ++r) {
                    const int64_t mr = all[r].first, ur = all[r].second;
                    if (usingSide) { if (ur >= 0) vals[r] = uc->f64[static_cast<size_t>(ur)]; }
                    else if (mr >= 0) vals[r] = mc->f64[static_cast<size_t>(mr)];
                    else if (uc && ur >= 0 && !uc->is_factor) vals[r] = uc->f64[static_cast<size_t>(ur)];
                    if (std::isnan(vals[r])) ++miss;
                }
                out.append_f64_column(src.name, std::move(vals));
            } else {
                std::vector<int32_t> cc(n, -1);
                std::vector<std::string> levels;
                std::unordered_map<std::string, int32_t> dict;
                for (size_t r = 0; r < n; ++r) {
                    const int64_t mr = all[r].first, ur = all[r].second;
                    const FrameColumn* from = nullptr;
                    int64_t row = -1;
                    if (usingSide) { from = uc; row = ur; }
                    else if (mr >= 0) { from = mc; row = mr; }
                    else if (uc && ur >= 0 && uc->is_factor) { from = uc; row = ur; }
                    if (from == nullptr || row < 0) { ++miss; continue; }
                    const int32_t code = from->codes[static_cast<size_t>(row)];
                    if (code < 0) { ++miss; continue; }
                    const std::string& lvl = from->levels[static_cast<size_t>(code)];
                    auto it = dict.find(lvl);
                    if (it == dict.end()) {
                        const int32_t nc = static_cast<int32_t>(levels.size());
                        dict.emplace(lvl, nc);
                        levels.push_back(lvl);
                        cc[r] = nc;
                    } else {
                        cc[r] = it->second;
                    }
                }
                out.append_factor_column_coded(src.name, std::move(cc), std::move(levels));
            }
            out.columns().back().seed_n_missing(miss);
            out.carry_identity_to_last(src);
        };
        for (const auto& c : cols_) {
            // A key takes the using's value on a using-only row; any other master
            // column is missing there.
            const FrameColumn* uc = keySet.count(c.name) ? &usingf.col(c.name) : nullptr;
            append_col(&c, uc, false);
        }
        if (!marker.empty()) {
            out.append_f64_column(marker, std::vector<double>(codes));
            out.columns().back().seed_n_missing(0);
        }
        for (const auto& c : usingf.cols_)
            if (!keySet.count(c.name) && !has(c.name)) append_col(nullptr, &c, true);
        out.finalize_after_bulk_load();
        return out;
    }

    std::vector<int64_t> midx(pairs.size()), uidx(pairs.size());
    for (size_t i = 0; i < pairs.size(); ++i) { midx[i] = pairs[i].first; uidx[i] = pairs[i].second; }

    // Output: master's columns (master's own order) + using's non-key columns
    // (using's own order). Every column is GATHERED fresh over the surviving
    // rows only — see joinby_gather_append's own comment for why a factor
    // column's dictionary must be rebuilt from scratch here rather than
    // reused from the source.
    Frame out;
    out.set_n_rows_hint(static_cast<int64_t>(pairs.size()));
    for (const auto& c : cols_) joinby_gather_append(out, c, midx);
    // using's non-key columns, EXCEPT any name the master already carries
    // (master wins — see the note above).
    for (const auto& c : usingf.cols_)
        if (!keySet.count(c.name) && !has(c.name)) joinby_gather_append(out, c, uidx);
    out.finalize_after_bulk_load();
    return out;
}

std::vector<Frame::DupReportRow> Frame::duplicates_report(const std::vector<std::string>& varlist) const {
    std::vector<std::string> cols = varlist.empty() ? std::vector<std::string>() : varlist;
    if (cols.empty()) for (const auto& c : cols_) cols.push_back(c.name);
    std::vector<std::vector<double>> K = key_cols(*this, cols);
    std::unordered_map<RowKey, int64_t, RowKeyHash> counts;
    for (int64_t i = 0; i < n_; ++i) counts[make_key(K, i)]++;
    std::map<int64_t, int64_t> dist;  // copies -> #rows with that group size
    for (const auto& kv : counts) dist[kv.second] += kv.second;
    std::vector<DupReportRow> out;
    for (const auto& kv : dist) {
        int64_t copies = kv.first, obs = kv.second;
        int64_t groups = obs / copies;
        out.push_back({copies, obs, groups * (copies - 1)});
    }
    return out;
}

// ---------------------------------------------------------------------------
// P1 wave data verbs. Oracle: StataBE 19.5 on the P1 wave fixture (2026-08-04)
//   keep if t<=2 (400 rows) -> expand 2       -> 800
//                            -> contract region treated -> 5 rows {region,treated,_freq}
//   drop if t==24 & mod(id,10)==0 -> fillin id t -> 20 new rows, 4800 total
//   keep in 1/10 -> cross using wave_lookup   -> 2000 rows, only `region_using` added
//                                                (id/cluster/stratum/group shared -> master wins)

Frame Frame::expand(int64_t n, int64_t* n_added_out) const {
    if (n <= 1) {  // Stata: `expand 1` and `expand 0` leave the data untouched
        if (n_added_out) *n_added_out = 0;
        return *this;
    }
    // Copies are APPENDED after the original block (Stata's own ordering), so
    // the index list is 0..n_-1 followed by (n-1) more passes of the same.
    std::vector<int64_t> idx;
    idx.reserve(static_cast<size_t>(n_ * n));
    for (int64_t rep = 0; rep < n; ++rep)
        for (int64_t i = 0; i < n_; ++i) idx.push_back(i);
    if (n_added_out) *n_added_out = n_ * (n - 1);
    return take_rows(*this, idx);
}

Frame Frame::fillin(const std::vector<std::string>& varlist, int64_t* n_added_out) const {
    if (varlist.empty()) throw EngineError("bad_input", "fillin: variable list required");
    for (const auto& v : varlist)
        if (!has(v)) throw EngineError("bad_input", "fillin: variable '" + v + "' not found");
    if (has("_fillin"))
        throw EngineError("bad_input", "fillin: variable _fillin already exists");

    // The observed value set of each fillin variable, in FIRST-SEEN order, then
    // sorted ascending so the generated grid is deterministic and matches the
    // sorted order Stata leaves behind.
    std::vector<std::vector<double>> K = key_cols(*this, varlist);
    const size_t nv = varlist.size();
    std::vector<std::vector<double>> levels(nv);
    for (size_t k = 0; k < nv; ++k) {
        std::vector<double> vals(K[k].begin(), K[k].end());
        std::sort(vals.begin(), vals.end(), [](double a, double b) {
            if (std::isnan(a)) return false;   // missing sorts last
            if (std::isnan(b)) return true;
            return a < b;
        });
        vals.erase(std::unique(vals.begin(), vals.end(), [](double a, double b) {
                       return (std::isnan(a) && std::isnan(b)) || a == b;
                   }),
                   vals.end());
        levels[k] = std::move(vals);
    }

    // Existing tuples -> the first row carrying them (a duplicate tuple keeps
    // its own row; fillin only ADDS what is absent, it never dedups).
    std::unordered_set<RowKey, RowKeyHash> present;
    for (int64_t i = 0; i < n_; ++i) present.insert(make_key(K, i));

    // Walk the full grid in odometer order (last variable fastest) and collect
    // the missing combinations.
    std::vector<std::vector<double>> newTuples;
    {
        std::vector<size_t> at(nv, 0);
        int64_t total = 1;
        for (size_t k = 0; k < nv; ++k) {
            if (levels[k].empty()) { total = 0; break; }
            total *= static_cast<int64_t>(levels[k].size());
        }
        for (int64_t c = 0; c < total; ++c) {
            RowKey key;
            key.v.resize(nv);
            for (size_t k = 0; k < nv; ++k) key.v[k] = levels[k][at[k]];
            for (auto& x : key.v) if (std::isnan(x)) x = std::numeric_limits<double>::quiet_NaN();
            if (!present.count(key)) newTuples.push_back(key.v);
            for (size_t k = nv; k-- > 0;) {         // odometer increment
                if (++at[k] < levels[k].size()) break;
                at[k] = 0;
            }
        }
    }
    if (n_added_out) *n_added_out = static_cast<int64_t>(newTuples.size());

    // Assemble: every original row, then one row per missing tuple carrying the
    // fillin variables and missing everywhere else. `_fillin` marks the source.
    const int64_t nNew = static_cast<int64_t>(newTuples.size());
    const int64_t nOut = n_ + nNew;
    std::unordered_map<std::string, size_t> fillinPos;
    for (size_t k = 0; k < nv; ++k) fillinPos[varlist[k]] = k;

    Frame out;
    out.set_n_rows_hint(nOut);
    for (const FrameColumn& c : cols_) {
        auto it = fillinPos.find(c.name);
        if (c.is_factor) {
            std::vector<int32_t> codes(static_cast<size_t>(nOut));
            for (int64_t i = 0; i < n_; ++i) codes[static_cast<size_t>(i)] = c.codes[i];
            for (int64_t j = 0; j < nNew; ++j) {
                double v = (it != fillinPos.end()) ? newTuples[static_cast<size_t>(j)][it->second]
                                                   : std::numeric_limits<double>::quiet_NaN();
                codes[static_cast<size_t>(n_ + j)] =
                    std::isnan(v) ? -1 : static_cast<int32_t>(v);   // key_cols gave the CODE
            }
            out.append_factor_column_coded(c.name, std::move(codes), c.levels.ref());
            out.carry_identity_to_last(c);  // BUG-363
        } else {
            std::vector<double> vals(static_cast<size_t>(nOut));
            for (int64_t i = 0; i < n_; ++i) vals[static_cast<size_t>(i)] = c.f64[i];
            for (int64_t j = 0; j < nNew; ++j) {
                vals[static_cast<size_t>(n_ + j)] =
                    (it != fillinPos.end()) ? newTuples[static_cast<size_t>(j)][it->second]
                                            : std::numeric_limits<double>::quiet_NaN();
            }
            out.append_f64_column(c.name, std::move(vals));
            out.carry_identity_to_last(c);  // BUG-363
        }
    }
    {
        std::vector<double> flag(static_cast<size_t>(nOut), 0.0);
        for (int64_t j = 0; j < nNew; ++j) flag[static_cast<size_t>(n_ + j)] = 1.0;
        out.append_f64_column("_fillin", std::move(flag));
    }
    out.finalize_after_bulk_load();
    return out;
}

Frame Frame::cross(const Frame& usingf) const {
    const int64_t nm = n_, nu = usingf.n_rows();
    // cross.ado matches the expanded using data to the master on the master's row
    // number with an old-style merge, which pairs each master row with using row 1
    // in place and appends the other pairings after them, master by master: rows
    // 1..nm are (m_i, u_1), then (m_1, u_2) .. (m_1, u_nu), (m_2, u_2), ... (BUG-1105,
    // StataBE 19.5). With no master rows the merge keeps the using rows: the master's
    // own variables are missing (index -1 below) and a variable both hold reads the
    // using's values, as merge fills a using-only row. An empty using is the
    // caller's r(111); here it gives no rows.
    const int64_t nOut = nu == 0 ? 0 : nm == 0 ? nu : nm * nu;
    std::vector<int64_t> midx(static_cast<size_t>(nOut)), uidx(static_cast<size_t>(nOut));
    if (nm == 0) {
        for (int64_t j = 0; j < nOut; ++j) { midx[static_cast<size_t>(j)] = -1;
                                             uidx[static_cast<size_t>(j)] = j; }
    } else if (nu > 0) {
        int64_t p = 0;
        for (int64_t i = 0; i < nm; ++i, ++p) { midx[static_cast<size_t>(p)] = i;
                                                uidx[static_cast<size_t>(p)] = 0; }
        for (int64_t i = 0; i < nm; ++i)
            for (int64_t j = 1; j < nu; ++j, ++p) { midx[static_cast<size_t>(p)] = i;
                                                    uidx[static_cast<size_t>(p)] = j; }
    }

    auto gather = [&](Frame& dst, const FrameColumn& c, const std::vector<int64_t>& idx) {
        if (c.is_factor) {
            std::vector<int32_t> codes(idx.size());
            for (size_t i = 0; i < idx.size(); ++i) codes[i] = idx[i] < 0 ? -1 : c.codes[idx[i]];
            dst.append_factor_column_coded(c.name, std::move(codes), c.levels.ref());
        } else {
            std::vector<double> vals(idx.size());
            for (size_t i = 0; i < idx.size(); ++i)
                vals[i] = idx[i] < 0 ? std::numeric_limits<double>::quiet_NaN() : c.f64[idx[i]];
            dst.append_f64_column(c.name, std::move(vals));
        }
        dst.carry_identity_to_last(c);  // BUG-363: identity metadata
    };

    Frame out;
    out.set_n_rows_hint(nOut);
    for (const auto& c : cols_) {
        if (nm == 0 && usingf.has(c.name) && usingf.col(c.name).is_factor == c.is_factor) {
            gather(out, usingf.col(c.name), uidx);
            out.carry_identity_to_last(c);  // the master's format and label stay
        } else {
            gather(out, c, midx);
        }
    }
    // A name the master already carries is taken from the MASTER: the using
    // copy is dropped, not renamed and not an error (oracle-pinned).
    for (const auto& c : usingf.cols_)
        if (!has(c.name)) gather(out, c, uidx);
    out.finalize_after_bulk_load();
    return out;
}

Frame Frame::contract(const std::vector<std::string>& varlist) const {
    if (varlist.empty()) throw EngineError("bad_input", "contract: variable list required");
    for (const auto& v : varlist)
        if (!has(v)) throw EngineError("bad_input", "contract: variable '" + v + "' not found");
    if (has("_freq")) throw EngineError("bad_input", "contract: variable _freq already exists");

    // First-seen representative row per distinct tuple + its frequency, then
    // sort the representatives by the key tuple (ascending, missing last).
    std::vector<std::vector<double>> K = key_cols(*this, varlist);
    std::unordered_map<RowKey, std::pair<int64_t, int64_t>, RowKeyHash> seen;  // key -> (row, freq)
    std::vector<int64_t> order;                                                // first-seen rows
    for (int64_t i = 0; i < n_; ++i) {
        RowKey k = make_key(K, i);
        auto it = seen.find(k);
        if (it == seen.end()) { seen.emplace(k, std::make_pair(i, int64_t{1})); order.push_back(i); }
        else { ++it->second.second; }
    }
    std::sort(order.begin(), order.end(), [&](int64_t a, int64_t b) {
        for (size_t k = 0; k < varlist.size(); ++k) {
            double x = K[k][static_cast<size_t>(a)], y = K[k][static_cast<size_t>(b)];
            bool nx = std::isnan(x), ny = std::isnan(y);
            if (nx || ny) { if (nx && ny) continue; return ny; }  // missing sorts last
            if (x != y) return x < y;
        }
        return false;
    });

    Frame out;
    out.set_n_rows_hint(static_cast<int64_t>(order.size()));
    for (const auto& name : varlist) {
        const FrameColumn& c = col(name);
        if (c.is_factor) {
            std::vector<int32_t> codes(order.size());
            for (size_t i = 0; i < order.size(); ++i) codes[i] = c.codes[order[i]];
            out.append_factor_column_coded(c.name, std::move(codes), c.levels.ref());
        } else {
            std::vector<double> vals(order.size());
            for (size_t i = 0; i < order.size(); ++i) vals[i] = c.f64[order[i]];
            out.append_f64_column(c.name, std::move(vals));
        }
        out.carry_identity_to_last(c);  // BUG-363
    }
    {
        std::vector<double> freq(order.size());
        for (size_t i = 0; i < order.size(); ++i)
            freq[i] = static_cast<double>(seen.at(make_key(K, order[i])).second);
        out.append_f64_column("_freq", std::move(freq));
    }
    out.finalize_after_bulk_load();
    return out;
}

Frame Frame::duplicates_drop(const std::vector<std::string>& varlist, int64_t* n_dropped_out) const {
    std::vector<std::string> cols = varlist;
    if (cols.empty()) for (const auto& c : cols_) cols.push_back(c.name);
    std::vector<std::vector<double>> K = key_cols(*this, cols);
    std::unordered_set<RowKey, RowKeyHash> seen;
    std::vector<int64_t> keep;
    keep.reserve(n_);
    for (int64_t i = 0; i < n_; ++i) {
        RowKey k = make_key(K, i);
        if (seen.insert(k).second) keep.push_back(i);
    }
    if (n_dropped_out) *n_dropped_out = n_ - static_cast<int64_t>(keep.size());
    return take_rows(*this, keep);
}

std::vector<double> Frame::duplicates_tag(const std::vector<std::string>& varlist) const {
    std::vector<std::string> cols = varlist;
    if (cols.empty()) for (const auto& c : cols_) cols.push_back(c.name);
    std::vector<std::vector<double>> K = key_cols(*this, cols);
    std::unordered_map<RowKey, int64_t, RowKeyHash> counts;
    std::vector<RowKey> keys(n_);
    for (int64_t i = 0; i < n_; ++i) { keys[i] = make_key(K, i); counts[keys[i]]++; }
    std::vector<double> tag(n_);
    for (int64_t i = 0; i < n_; ++i) tag[i] = static_cast<double>(counts[keys[i]] - 1);
    return tag;
}

std::vector<double> Frame::recode_apply(const std::string& var, const std::vector<RecodeRule>& rules) const {
    const FrameColumn& c = col(var);
    std::vector<double> src(n_);
    if (c.is_factor) for (int64_t i = 0; i < n_; ++i) src[i] = c.codes[i] < 0 ? NAd : static_cast<double>(c.codes[i]);
    else src.assign(c.f64.data(), c.f64.data() + c.f64.size());

    // Unmatched values copy through unchanged (Stata: `recode x (1=10)` with no
    // else clause leaves every other value, including missing, exactly as it
    // was) — so the output starts as a copy of the source, and only the cells a
    // rule actually fires on get overwritten below. A row that stays unmatched
    // because it was already missing is unaffected either way (it copies
    // through as still-missing).
    std::vector<double> out(src);
    std::vector<char> matched(n_, 0);
    for (int64_t i = 0; i < n_; ++i) {
        double x = src[i];
        bool is_na = std::isnan(x);
        for (const RecodeRule& r : rules) {
            bool hit = false;
            if (r.kind == 0) hit = !is_na && x == r.a;
            else if (r.kind == 1) hit = !is_na && x >= r.a && x <= r.b;
            else if (r.kind == 2) hit = is_na;
            else if (r.kind == 3) hit = !matched[i];  // else: catch-all for anything not yet matched
            if (hit) {
                out[i] = r.new_is_missing ? NAd : r.new_val;
                matched[i] = 1;
                break;  // first matching rule wins (Stata semantics)
            }
        }
    }
    return out;
}

Frame::ReshapeResult Frame::reshape_long(const std::vector<std::string>& stubs,
                                         const std::vector<std::string>& i_vars,
                                         const std::string& j_var) const {
    for (const auto& v : i_vars) if (!idx_.count(v)) throw std::runtime_error("reshape long: id variable `" + v + "' not found");

    // Discover stub<suffix> columns per stub (matches reshape.py: any column
    // whose name starts with the stub and is longer than it).
    std::vector<std::unordered_map<std::string, std::string>> stub_cols(stubs.size());  // suffix -> colname
    std::vector<std::string> first_suffix_order;  // to establish a canonical suffix set from stub[0]
    // W:P5: a column belongs to the LONGEST stub that prefixes it, not to every
    // stub that does. With stubs `y ycount`, plain prefix matching let `y` claim
    // `ycount0`/`ycount1` as suffixes "count0"/"count1", so the stub sets came
    // out different sizes and a legal Stata reshape failed ("stub `x1' has a
    // different suffix set than `y'"). Worse than the error: with a stub set
    // that happened to line up, the mis-assigned column would have been
    // reshaped under the wrong stub silently. Stata resolves this by longest
    // match, which is also the only assignment that makes `reshape wide`'s
    // output round-trip.
    for (const auto& c : cols_) {
        size_t best = stubs.size();
        for (size_t s = 0; s < stubs.size(); ++s) {
            if (c.name.size() > stubs[s].size() &&
                c.name.compare(0, stubs[s].size(), stubs[s]) == 0 &&
                (best == stubs.size() || stubs[s].size() > stubs[best].size())) {
                best = s;
            }
        }
        if (best != stubs.size()) stub_cols[best][c.name.substr(stubs[best].size())] = c.name;
    }
    for (size_t s = 0; s < stubs.size(); ++s) {
        if (stub_cols[s].empty()) throw std::runtime_error("reshape long: no columns found with stub `" + stubs[s] + "'");
    }
    std::vector<std::string> suffixes;
    for (const auto& kv : stub_cols[0]) suffixes.push_back(kv.first);
    // numeric-first sort, matching reshape.py's key=(float(s) if numeric else inf, s)
    std::sort(suffixes.begin(), suffixes.end(), [](const std::string& a, const std::string& b) {
        auto isnum = [](const std::string& s) { if (s.empty()) return false; size_t i = (s[0]=='-')?1:0;
            if (i>=s.size()) return false; for (; i<s.size(); ++i) if (!std::isdigit(static_cast<unsigned char>(s[i]))) return false; return true; };
        bool na = isnum(a), nb = isnum(b);
        double va = na ? std::stod(a) : std::numeric_limits<double>::infinity();
        double vb = nb ? std::stod(b) : std::numeric_limits<double>::infinity();
        if (va != vb) return va < vb;
        return a < b;
    });
    for (size_t s = 1; s < stubs.size(); ++s) {
        if (stub_cols[s].size() != suffixes.size())
            throw std::runtime_error("reshape long: stub `" + stubs[s] + "' has a different suffix set than `" + stubs[0] + "'");
        for (const auto& suf : suffixes)
            if (!stub_cols[s].count(suf)) throw std::runtime_error("reshape long: stub `" + stubs[s] + "' missing suffix " + suf);
    }

    std::unordered_map<std::string, char> is_stub_col;
    for (auto& sc : stub_cols) for (auto& kv : sc) is_stub_col[kv.second] = 1;
    std::vector<std::string> other_cols;
    std::unordered_map<std::string, char> is_ivar;
    for (auto& v : i_vars) is_ivar[v] = 1;
    for (const auto& c : cols_) if (!is_stub_col.count(c.name) && !is_ivar.count(c.name)) other_cols.push_back(c.name);

    const int64_t nwide = n_;
    const int64_t K = static_cast<int64_t>(suffixes.size());
    const int64_t nlong = nwide * K;

    Frame out;
    out.n_ = nlong;
    // column order: i_vars, j_var, stubs, other
    for (const auto& iv : i_vars) {
        const FrameColumn& src = col(iv);
        FrameColumn nc; nc.name = iv; nc.is_factor = src.is_factor;
        if (src.is_factor) { nc.levels = src.levels; nc.codes.resize(nlong); }
        else nc.f64.resize(nlong);
        for (int64_t suf_i = 0; suf_i < K; ++suf_i)
            for (int64_t r = 0; r < nwide; ++r) {
                int64_t out_row = suf_i * nwide + r;
                if (src.is_factor) nc.codes[out_row] = src.codes[r]; else nc.f64[out_row] = src.f64[r];
            }
        out.cols_.push_back(std::move(nc));
    }
    { // j_var: numeric wave index (NaN if a suffix doesn't parse as a number)
        FrameColumn nc; nc.name = j_var; nc.is_factor = false; nc.f64.resize(nlong);
        for (int64_t suf_i = 0; suf_i < K; ++suf_i) {
            double jv;
            try { jv = std::stod(suffixes[suf_i]); } catch (...) { jv = NAd; }
            for (int64_t r = 0; r < nwide; ++r) nc.f64[suf_i * nwide + r] = jv;
        }
        out.cols_.push_back(std::move(nc));
    }
    for (size_t s = 0; s < stubs.size(); ++s) {
        FrameColumn nc; nc.name = stubs[s]; nc.is_factor = false; nc.f64.resize(nlong);
        for (int64_t suf_i = 0; suf_i < K; ++suf_i) {
            const FrameColumn& src = col(stub_cols[s][suffixes[suf_i]]);
            for (int64_t r = 0; r < nwide; ++r) {
                double v = src.is_factor ? (src.codes[r] < 0 ? NAd : static_cast<double>(src.codes[r])) : src.f64[r];
                nc.f64[suf_i * nwide + r] = v;
            }
        }
        out.cols_.push_back(std::move(nc));
    }
    for (const auto& oc : other_cols) {
        const FrameColumn& src = col(oc);
        FrameColumn nc; nc.name = oc; nc.is_factor = src.is_factor;
        if (src.is_factor) { nc.levels = src.levels; nc.codes.resize(nlong); } else nc.f64.resize(nlong);
        for (int64_t suf_i = 0; suf_i < K; ++suf_i)
            for (int64_t r = 0; r < nwide; ++r) {
                int64_t out_row = suf_i * nwide + r;
                if (src.is_factor) nc.codes[out_row] = src.codes[r]; else nc.f64[out_row] = src.f64[r];
            }
        out.cols_.push_back(std::move(nc));
    }
    out.reindex();
    ReshapeResult res;
    res.frame = std::move(out);
    res.j_values = suffixes;
    return res;
}

Frame::ReshapeResult Frame::reshape_wide(const std::vector<std::string>& stubs,
                                         const std::vector<std::string>& i_vars,
                                         const std::string& j_var) const {
    for (const auto& v : i_vars) if (!idx_.count(v)) throw std::runtime_error("reshape wide: id variable `" + v + "' not found");
    if (!idx_.count(j_var)) throw std::runtime_error("reshape wide: j variable `" + j_var + "' not found");
    for (const auto& s : stubs) if (!idx_.count(s)) throw std::runtime_error("reshape wide: stub variable `" + s + "' not found");

    const double* jcol = require_f64(j_var);
    std::vector<double> juniq;
    { std::unordered_set<double> seen; for (int64_t i = 0; i < n_; ++i) if (!std::isnan(jcol[i]) && seen.insert(jcol[i]).second) juniq.push_back(jcol[i]); }
    std::sort(juniq.begin(), juniq.end());

    // Group rows by i-var key (first occurrence order — this is the "row
    // universe" used to resolve stub<j> values and other_cols' "first" per
    // group; final OUTPUT row order is computed separately below).
    std::vector<std::vector<double>> IK = key_cols(*this, i_vars);
    std::vector<RowKey> row_ikey(n_);
    for (int64_t i = 0; i < n_; ++i) row_ikey[i] = make_key(IK, i);
    std::vector<RowKey> group_order;
    std::unordered_map<RowKey, int64_t, RowKeyHash> group_index;
    std::vector<std::vector<int64_t>> group_rows;
    for (int64_t i = 0; i < n_; ++i) {
        auto it = group_index.find(row_ikey[i]);
        if (it == group_index.end()) {
            group_index[row_ikey[i]] = static_cast<int64_t>(group_rows.size());
            group_rows.push_back({i});
            group_order.push_back(row_ikey[i]);
        } else {
            group_rows[it->second].push_back(i);
        }
    }
    const int64_t ngroups = static_cast<int64_t>(group_rows.size());

    std::unordered_map<std::string, char> is_stub, is_ivar, is_j;
    for (auto& s : stubs) is_stub[s] = 1;
    for (auto& v : i_vars) is_ivar[v] = 1;
    is_j[j_var] = 1;
    std::vector<std::string> other_cols;
    for (const auto& c : cols_) if (!is_stub.count(c.name) && !is_ivar.count(c.name) && !is_j.count(c.name)) other_cols.push_back(c.name);

    auto j_suffix = [](double v) -> std::string {
        double r = std::round(v);
        if (std::fabs(v - r) < 1e-9) { std::ostringstream os; os << static_cast<int64_t>(r); return os.str(); }
        std::ostringstream os; os << v; return os.str();
    };

    // Resolve, per group, the stub value at a given j: first occurrence (in
    // current row order) among that group's rows whose j matches. Returns
    // NaN + found=false if no row in the group has that j (distinct from a
    // matching row whose stub VALUE happens to be NaN — both end up NaN in
    // the cell, but "found" also drives per-stub group survival below).
    auto stub_value_at = [&](const FrameColumn& src, int64_t g, double jv, bool* found) -> double {
        for (int64_t r : group_rows[g]) {
            if (!std::isnan(jcol[r]) && jcol[r] == jv) {
                *found = true;
                return src.is_factor ? (src.codes[r] < 0 ? NAd : static_cast<double>(src.codes[r])) : src.f64[r];
                // first occurrence wins (Stata errors on dup; pandas oracle uses aggfunc="first")
            }
        }
        *found = false;
        return NAd;
    };

    // pandas.pivot_table(..., aggfunc="first") defaults to dropna=True: for
    // EACH stub independently, a group is dropped from that stub's pivot iff
    // every resolved cell (across all j) is missing (either no row has that
    // j, or the row's stub value is itself missing). reshape.py then builds
    // df_wide by pd.merge(..., how="outer") of the per-stub pivots in stub
    // order — so a group dropped by stub A can be resurrected by stub B, with
    // stub A's columns NaN-filled for it. This is the root cause of the
    // "silent duplicate/short-row" divergence (contract §22): it is not
    // really about duplicate (i,j) rows (those are already handled by
    // first-occurrence-wins above) — an all-missing stub for a group makes
    // pandas drop that group's row entirely, unless another stub keeps it
    // alive. Compute per-stub cell grids + per-stub survival here; the final
    // row order (sorted-ascending union of survivors) is derived below.
    const size_t nstubs = stubs.size();
    const size_t nj = juniq.size();
    std::vector<std::vector<double>> cell(nstubs, std::vector<double>(static_cast<size_t>(ngroups) * nj, NAd));
    std::vector<std::vector<char>> survives(nstubs, std::vector<char>(ngroups, 0));
    for (size_t si = 0; si < nstubs; ++si) {
        const FrameColumn& src = col(stubs[si]);
        for (int64_t g = 0; g < ngroups; ++g) {
            bool any_found = false;
            for (size_t ji = 0; ji < nj; ++ji) {
                bool found = false;
                double v = stub_value_at(src, g, juniq[ji], &found);
                cell[si][static_cast<size_t>(g) * nj + ji] = v;
                if (found && !std::isnan(v)) any_found = true;
            }
            survives[si][g] = any_found ? 1 : 0;
        }
    }

    // Final row order: pandas.merge(..., how="outer") performs a sort-merge
    // join on the key — empirically (verified against the live oracle) the
    // accumulated result of successively outer-merging any number of
    // per-stub pivots (each already sorted ascending by pivot_table's default
    // sort=True index) is always the SORTED-ASCENDING UNION of the surviving
    // i-keys, independent of stub order or which stub "found" a key first.
    // (Not a left-preserving-order-then-append-new-keys scheme — verified
    // that ordering hypothesis against the oracle and it does NOT match: a
    // group resurrected by a later stub is NOT appended at the tail, it is
    // merged back into sorted position.) So: union survivors across all
    // stubs, then sort ascending by i_vars key.
    std::vector<char> any_survives(ngroups, 0);
    for (size_t si = 0; si < nstubs; ++si)
        for (int64_t g = 0; g < ngroups; ++g)
            if (survives[si][g]) any_survives[g] = 1;
    // No stubs at all is rejected by the caller (run_reshape); with >=1 stub
    // every group with zero non-missing values in every stub is correctly
    // absent from final_order (matches the oracle: it never appears in
    // df_wide, since df_wide is built purely from stub pivots before the
    // other_cols left-merge).
    std::vector<int64_t> final_order;
    for (int64_t g = 0; g < ngroups; ++g) if (any_survives[g]) final_order.push_back(g);
    std::stable_sort(final_order.begin(), final_order.end(), [&](int64_t a, int64_t b) {
        return key_less(IK, group_rows[a][0], group_rows[b][0]);
    });
    const int64_t nout = static_cast<int64_t>(final_order.size());

    Frame out;
    out.n_ = nout;
    for (const auto& iv : i_vars) {
        const FrameColumn& src = col(iv);
        FrameColumn nc; nc.name = iv; nc.is_factor = src.is_factor;
        if (src.is_factor) { nc.levels = src.levels; nc.codes.resize(nout); } else nc.f64.resize(nout);
        for (int64_t o = 0; o < nout; ++o) {
            int64_t r0 = group_rows[final_order[o]][0];
            if (src.is_factor) nc.codes[o] = src.codes[r0]; else nc.f64[o] = src.f64[r0];
        }
        out.cols_.push_back(std::move(nc));
    }
    for (size_t si = 0; si < nstubs; ++si) {
        for (size_t ji = 0; ji < nj; ++ji) {
            FrameColumn nc; nc.name = stubs[si] + j_suffix(juniq[ji]); nc.is_factor = false; nc.f64.resize(nout);
            for (int64_t o = 0; o < nout; ++o) {
                int64_t g = final_order[o];
                nc.f64[o] = survives[si][g] ? cell[si][static_cast<size_t>(g) * nj + ji] : NAd;
            }
            out.cols_.push_back(std::move(nc));
        }
    }
    for (const auto& oc : other_cols) {
        const FrameColumn& src = col(oc);
        FrameColumn nc; nc.name = oc; nc.is_factor = src.is_factor;
        if (src.is_factor) { nc.levels = src.levels; nc.codes.resize(nout); } else nc.f64.resize(nout);
        for (int64_t o = 0; o < nout; ++o) {
            int64_t r0 = group_rows[final_order[o]][0];  // "first" aggregation, matches reshape.py other_agg
            if (src.is_factor) nc.codes[o] = src.codes[r0]; else nc.f64[o] = src.f64[r0];
        }
        out.cols_.push_back(std::move(nc));
    }
    out.reindex();
    ReshapeResult res;
    res.frame = std::move(out);
    for (double jv : juniq) res.j_values.push_back(j_suffix(jv));
    return res;
}

namespace {
// One output column gathered from (column, row) sources; row < 0 = missing. A
// factor result gets a fresh dictionary in first-seen order. Every source of
// one output column has the same kind (factor or f64).
FrameColumn reshape_gather(const std::string& name, bool is_factor,
                           const std::vector<std::pair<const FrameColumn*, int64_t>>& src) {
    FrameColumn nc;
    nc.name = name;
    nc.is_factor = is_factor;
    const size_t n = src.size();
    if (!is_factor) {
        std::vector<double> v(n, NAd);
        for (size_t r = 0; r < n; ++r)
            if (src[r].first != nullptr && src[r].second >= 0)
                v[r] = src[r].first->f64[static_cast<size_t>(src[r].second)];
        nc.f64 = std::move(v);
        return nc;
    }
    std::vector<int32_t> codes(n, -1);
    std::vector<std::string> levels;
    std::unordered_map<std::string, int32_t> dict;
    for (size_t r = 0; r < n; ++r) {
        const FrameColumn* c = src[r].first;
        if (c == nullptr || src[r].second < 0) continue;
        const int32_t code = c->codes[static_cast<size_t>(src[r].second)];
        if (code < 0) continue;
        const std::string& lvl = c->levels[static_cast<size_t>(code)];
        auto it = dict.find(lvl);
        if (it == dict.end()) {
            const int32_t k = static_cast<int32_t>(levels.size());
            dict.emplace(lvl, k);
            levels.push_back(lvl);
            codes[r] = k;
        } else {
            codes[r] = it->second;
        }
    }
    nc.codes = std::move(codes);
    nc.levels = std::move(levels);
    return nc;
}

// Stata's sort order of one cell: a string by its text ("" first), a number by
// stata_cmp (reals, then . < .a < ... < .z).
int reshape_cell_cmp(const FrameColumn& c, int64_t a, int64_t b) {
    if (c.is_factor) {
        static const std::string kEmpty;
        const int32_t ca = c.codes[static_cast<size_t>(a)], cb = c.codes[static_cast<size_t>(b)];
        const std::string& sa = ca < 0 ? kEmpty : c.levels[static_cast<size_t>(ca)];
        const std::string& sb = cb < 0 ? kEmpty : c.levels[static_cast<size_t>(cb)];
        const int r = sa.compare(sb);
        return r < 0 ? -1 : (r > 0 ? 1 : 0);
    }
    return stata_cmp(c.f64[static_cast<size_t>(a)], c.f64[static_cast<size_t>(b)]);
}

bool reshape_cell_eq(const FrameColumn& c, int64_t a, int64_t b) {
    if (c.is_factor) return reshape_cell_cmp(c, a, b) == 0;
    const double x = c.f64[static_cast<size_t>(a)], y = c.f64[static_cast<size_t>(b)];
    if (std::isnan(x) || std::isnan(y)) return std::isnan(x) && std::isnan(y) && missing_kind(x) == missing_kind(y);
    return x == y;
}

std::string reshape_num_suffix(double v) {
    double r = std::round(v);
    if (std::fabs(v - r) < 1e-9) { std::ostringstream os; os << static_cast<int64_t>(r); return os.str(); }
    std::ostringstream os; os << v; return os.str();
}
}  // namespace

Frame::ReshapeResult Frame::reshape_long_stata(const std::vector<std::string>& stubs,
                                               const std::vector<std::string>& i_vars,
                                               const std::string& j_var, bool jstring) const {
    for (const auto& v : i_vars)
        if (!idx_.count(v)) throw EngineError("missing_column", "variable " + v + " not found", v);
    // A column belongs to the LONGEST stub that prefixes it (see reshape_long).
    std::vector<std::map<std::string, std::string>> stub_cols(stubs.size());  // suffix -> column
    for (const auto& c : cols_) {
        size_t best = stubs.size();
        for (size_t si = 0; si < stubs.size(); ++si) {
            if (c.name.size() > stubs[si].size() && c.name.compare(0, stubs[si].size(), stubs[si]) == 0 &&
                (best == stubs.size() || stubs[si].size() > stubs[best].size()))
                best = si;
        }
        if (best != stubs.size()) stub_cols[best][c.name.substr(stubs[best].size())] = c.name;
    }
    for (size_t si = 0; si < stubs.size(); ++si)
        if (stub_cols[si].empty())
            throw EngineError("missing_column", "variable " + stubs[si] + " not found", stubs[si]);
    // j: the union of every stub's suffixes. Without `string` each must be a
    // number (Stata: r(498)); with it they are text in byte order.
    std::set<std::string> suf_set;
    for (const auto& sc : stub_cols) for (const auto& kv : sc) suf_set.insert(kv.first);
    std::vector<std::string> suffixes(suf_set.begin(), suf_set.end());
    std::vector<double> jnum;
    if (!jstring) {
        for (const auto& sfx : suffixes) {
            size_t used = 0;
            double v = 0;
            try { v = std::stod(sfx, &used); } catch (...) { used = 0; }
            if (used != sfx.size() || sfx.empty())
                throw EngineError("reshape_498", "variable " + j_var + " contains all missing values; "
                                  "the suffix `" + sfx + "' is not numeric (specify string)", j_var);
            jnum.push_back(v);
        }
        std::vector<size_t> ord(suffixes.size());
        std::iota(ord.begin(), ord.end(), size_t{0});
        std::stable_sort(ord.begin(), ord.end(), [&](size_t a, size_t b) { return jnum[a] < jnum[b]; });
        std::vector<std::string> s2; std::vector<double> j2;
        for (size_t k : ord) { s2.push_back(suffixes[k]); j2.push_back(jnum[k]); }
        suffixes.swap(s2); jnum.swap(j2);
    }
    // i must identify the wide observations (Stata: r(9)).
    std::vector<int64_t> order(static_cast<size_t>(n_));
    std::iota(order.begin(), order.end(), int64_t{0});
    std::stable_sort(order.begin(), order.end(), [&](int64_t a, int64_t b) {
        for (const auto& iv : i_vars) {
            const int c = reshape_cell_cmp(col(iv), a, b);
            if (c != 0) return c < 0;
        }
        return false;
    });
    for (size_t k = 1; k < order.size(); ++k) {
        bool same = true;
        for (const auto& iv : i_vars)
            if (!reshape_cell_eq(col(iv), order[k - 1], order[k])) { same = false; break; }
        if (same)
            throw EngineError("reshape_9", "variable " + i_vars[0] +
                              " does not uniquely identify the observations", i_vars[0]);
    }
    std::unordered_map<std::string, char> is_stub_col, is_ivar;
    for (const auto& sc : stub_cols) for (const auto& kv : sc) is_stub_col[kv.second] = 1;
    for (const auto& v : i_vars) is_ivar[v] = 1;
    std::vector<std::string> other_cols;
    for (const auto& c : cols_)
        if (!is_stub_col.count(c.name) && !is_ivar.count(c.name) && c.name != j_var) other_cols.push_back(c.name);

    const size_t K = suffixes.size();
    const size_t nlong = order.size() * K;
    Frame out;
    out.n_ = static_cast<int64_t>(nlong);
    auto rows_of = [&](const FrameColumn* c) {
        std::vector<std::pair<const FrameColumn*, int64_t>> src;
        src.reserve(nlong);
        for (int64_t r : order) for (size_t k = 0; k < K; ++k) src.emplace_back(c, r);
        return src;
    };
    for (const auto& iv : i_vars) {
        const FrameColumn& c = col(iv);
        FrameColumn nc = reshape_gather(iv, c.is_factor, rows_of(&c));
        nc.format = c.format; nc.label = c.label;
        out.cols_.push_back(std::move(nc));
    }
    {
        FrameColumn nc; nc.name = j_var;
        if (!jstring) {
            nc.is_factor = false;
            nc.f64.resize(nlong);
            for (size_t r = 0; r < order.size(); ++r)
                for (size_t k = 0; k < K; ++k) nc.f64[r * K + k] = jnum[k];
        } else {
            nc.is_factor = true;
            nc.codes.resize(nlong);
            std::vector<std::string> lv;
            for (const auto& sfx : suffixes) lv.push_back(sfx);
            for (size_t r = 0; r < order.size(); ++r)
                for (size_t k = 0; k < K; ++k) nc.codes[r * K + k] = static_cast<int32_t>(k);
            nc.levels = std::move(lv);
        }
        out.cols_.push_back(std::move(nc));
    }
    for (size_t si = 0; si < stubs.size(); ++si) {
        // A stub is a string only when every component it has is one.
        bool all_factor = true;
        for (const auto& kv : stub_cols[si]) if (!col(kv.second).is_factor) all_factor = false;
        std::vector<std::pair<const FrameColumn*, int64_t>> src;
        src.reserve(nlong);
        for (int64_t r : order)
            for (size_t k = 0; k < K; ++k) {
                auto it = stub_cols[si].find(suffixes[k]);
                const FrameColumn* c = it == stub_cols[si].end() ? nullptr : &col(it->second);
                if (c != nullptr && c->is_factor != all_factor) c = nullptr;
                src.emplace_back(c, r);
            }
        out.cols_.push_back(reshape_gather(stubs[si], all_factor, src));
    }
    for (const auto& oc : other_cols) {
        const FrameColumn& c = col(oc);
        FrameColumn nc = reshape_gather(oc, c.is_factor, rows_of(&c));
        nc.format = c.format; nc.label = c.label;
        out.cols_.push_back(std::move(nc));
    }
    out.reindex();
    ReshapeResult res;
    res.frame = std::move(out);
    res.j_values = suffixes;
    return res;
}

Frame::ReshapeResult Frame::reshape_wide_stata(const std::vector<std::string>& stubs,
                                               const std::vector<std::string>& i_vars,
                                               const std::string& j_var, bool jstring) const {
    for (const auto& v : i_vars)
        if (!idx_.count(v)) throw EngineError("missing_column", "variable " + v + " not found", v);
    if (!idx_.count(j_var)) throw EngineError("missing_column", "variable " + j_var + " not found", j_var);
    for (const auto& st : stubs)
        if (!idx_.count(st)) throw EngineError("missing_column", "variable " + st + " not found", st);
    const FrameColumn& jc = col(j_var);
    if (jc.is_factor && !jstring)
        throw EngineError("reshape_109", "variable " + j_var + " is string; specify string option", j_var);
    if (!jc.is_factor && jstring)
        throw EngineError("reshape_109", "variable " + j_var + " is not string", j_var);
    auto j_missing = [&](int64_t r) {
        if (jc.is_factor) {
            const int32_t c = jc.codes[static_cast<size_t>(r)];
            return c < 0 || jc.levels[static_cast<size_t>(c)].empty();
        }
        return std::isnan(jc.f64[static_cast<size_t>(r)]);
    };
    for (int64_t r = 0; r < n_; ++r)
        if (j_missing(r))
            throw EngineError("reshape_498", "variable " + j_var + " contains missing values", j_var);
    // Rows in (i, j) order; i groups.
    std::vector<int64_t> order(static_cast<size_t>(n_));
    std::iota(order.begin(), order.end(), int64_t{0});
    std::stable_sort(order.begin(), order.end(), [&](int64_t a, int64_t b) {
        for (const auto& iv : i_vars) {
            const int c = reshape_cell_cmp(col(iv), a, b);
            if (c != 0) return c < 0;
        }
        return reshape_cell_cmp(jc, a, b) < 0;
    });
    auto same_i = [&](int64_t a, int64_t b) {
        for (const auto& iv : i_vars) if (!reshape_cell_eq(col(iv), a, b)) return false;
        return true;
    };
    std::vector<std::pair<size_t, size_t>> groups;
    for (size_t k = 0; k < order.size();) {
        size_t e = k + 1;
        while (e < order.size() && same_i(order[k], order[e])) ++e;
        groups.emplace_back(k, e);
        k = e;
    }
    // (i, j) unique (Stata: r(9)).
    for (const auto& g : groups)
        for (size_t k = g.first + 1; k < g.second; ++k)
            if (reshape_cell_eq(jc, order[k - 1], order[k])) {
                // reshape.ado checks this on the data sorted by i j, and the
                // refusal leaves them so (dc_r_wide_jdup_d): detail = the sort.
                std::string keys;
                for (const auto& iv : i_vars) keys += iv + " ";
                throw EngineError("reshape_9", "values of variable " + j_var + " not unique within " +
                                  i_vars[0], keys + j_var);
            }
    // The other variables must be constant within i (Stata: r(9)).
    std::unordered_map<std::string, char> skip;
    for (const auto& v : i_vars) skip[v] = 1;
    for (const auto& st : stubs) skip[st] = 1;
    skip[j_var] = 1;
    std::vector<std::string> other_cols;
    for (const auto& c : cols_) if (!skip.count(c.name)) other_cols.push_back(c.name);
    for (const auto& oc : other_cols) {
        const FrameColumn& c = col(oc);
        for (const auto& g : groups)
            for (size_t k = g.first + 1; k < g.second; ++k)
                if (!reshape_cell_eq(c, order[g.first], order[k])) {
                    // Checked on the data sorted by i and the variable, which the
                    // refusal leaves (dc_r_wide_notconst_d): detail = that sort.
                    std::string keys;
                    for (const auto& iv : i_vars) keys += iv + " ";
                    throw EngineError("reshape_9", "variable " + oc + " not constant within " + i_vars[0],
                                      keys + oc);
                }
    }
    // j values in order, and each one's suffix text.
    std::vector<int64_t> jrep;  // a row holding each distinct j value
    {
        std::vector<int64_t> byj(order);
        std::stable_sort(byj.begin(), byj.end(), [&](int64_t a, int64_t b) { return reshape_cell_cmp(jc, a, b) < 0; });
        for (size_t k = 0; k < byj.size(); ++k)
            if (k == 0 || !reshape_cell_eq(jc, byj[k - 1], byj[k])) jrep.push_back(byj[k]);
    }
    std::vector<std::string> jsuf;
    for (int64_t r : jrep)
        jsuf.push_back(jc.is_factor ? jc.levels[static_cast<size_t>(jc.codes[static_cast<size_t>(r)])]
                                    : reshape_num_suffix(jc.f64[static_cast<size_t>(r)]));
    const size_t G = groups.size();
    Frame out;
    out.n_ = static_cast<int64_t>(G);
    auto first_rows = [&](const FrameColumn* c) {
        std::vector<std::pair<const FrameColumn*, int64_t>> src;
        for (const auto& g : groups) src.emplace_back(c, order[g.first]);
        return src;
    };
    for (const auto& iv : i_vars) {
        const FrameColumn& c = col(iv);
        FrameColumn nc = reshape_gather(iv, c.is_factor, first_rows(&c));
        nc.format = c.format; nc.label = c.label;
        out.cols_.push_back(std::move(nc));
    }
    for (size_t jk = 0; jk < jrep.size(); ++jk) {
        for (const auto& st : stubs) {
            const FrameColumn& c = col(st);
            std::vector<std::pair<const FrameColumn*, int64_t>> src;
            for (const auto& g : groups) {
                int64_t hit = -1;
                for (size_t k = g.first; k < g.second; ++k)
                    if (reshape_cell_eq(jc, order[k], jrep[jk])) { hit = order[k]; break; }
                src.emplace_back(&c, hit);
            }
            FrameColumn nc = reshape_gather(st + jsuf[jk], c.is_factor, src);
            nc.format = c.format;
            out.cols_.push_back(std::move(nc));
        }
    }
    for (const auto& oc : other_cols) {
        const FrameColumn& c = col(oc);
        FrameColumn nc = reshape_gather(oc, c.is_factor, first_rows(&c));
        nc.format = c.format; nc.label = c.label;
        out.cols_.push_back(std::move(nc));
    }
    out.reindex();
    ReshapeResult res;
    res.frame = std::move(out);
    res.j_values = jsuf;
    return res;
}

// ---------------------------------------------------------------------------
// merge 1:1|m:1|1:m KEYLIST using USING — see MergeOpts/MergeResult in frame.h.
// Mirrors merge_op.py's run_merge() exactly: pandas outer-join semantics on
// KEYLIST, _merge in {1=master-only, 2=using-only, 3=matched}, keepusing()
// applied before the column-conflict check, cardinality enforced per merge
// type, assert()/keep() applied in that order after the join.
namespace {
// BUG-394 (2026-09-02): a ROW floor for the morsel-parallel gathers in this file. The
// pool's own floor is N<2 (threads.cpp: N there is often a LANE count, where even 38
// units are 2.4x faster threaded), so a small merge paid a full fan-out for a few
// hundred rows. Bit-identity is unaffected either way (fixed partitions, disjoint
// writes, no float accumulation in a gather) — this is dispatch overhead only.
constexpr int64_t kGatherRowsFloor = 8192;
template <class Body>
inline void run_gather(int64_t nrows, const Body& body) {
    if (nrows < kGatherRowsFloor) body(0, nrows, 0);
    else ThreadPool::global().run(nrows, body);
}
}  // namespace

MergeResult merge_frames(const Frame& master, const Frame& usingf, const MergeOpts& opts) {
    for (const auto& k : opts.keylist) {
        if (!master.has(k)) throw std::runtime_error("merge: key variable '" + k + "' not in master dataset");
        if (!usingf.has(k)) throw std::runtime_error("merge: key variable '" + k + "' not in using dataset");
    }

    // keepusing(): restrict using's non-key columns before the conflict check.
    std::vector<std::string> using_cols_keep;
    {
        std::unordered_map<std::string, char> keep_set;
        for (const auto& k : opts.keylist) keep_set[k] = 1;
        bool restrict = !opts.keepusing.empty();
        if (restrict) for (const auto& v : opts.keepusing) keep_set[v] = 1;
        for (const auto& c : usingf.columns())
            if (!restrict || keep_set.count(c.name)) using_cols_keep.push_back(c.name);
    }

    // A non-key column present on BOTH sides: Stata's documented default (no
    // `update`/`replace`) KEEPS the master's values and simply does not bring
    // the using's copy across — it is not an error. Oracle: StataBE 19.5 on the
    // P1 wave fixture, `merge m:1 id using wave_lookup, nogen` where both files
    // carry cluster/stratum/group — 4,800 matched, master's copies retained,
    // only the using-only column added. This build previously hard-errored
    // here, rejecting a legal and common Stata form.
    //
    // With `update` ([D] merge, "update and replace both perform an update merge
    // rather than a standard merge"), the using's copy is NOT discarded: it feeds
    // the master's column. Collect those names here — `overlap` is the exact set of
    // Stata's "overlapping variables", and it stays EMPTY on a standard merge, so
    // the non-update path below is byte-identical to what it was.
    const bool do_update = opts.update || opts.replace;  // replace alone still update-merges
    std::vector<std::string> overlap;
    // stata_plan: every shared non-key variable the using brings (keepusing
    // permitting) whose string-vs-numeric kind agrees with the master's. Its
    // using copy fills the using-only rows (see MergeOpts::stata_plan).
    std::vector<std::string> shared_vars;
    {
        std::unordered_map<std::string, char> master_non_key;
        for (const auto& c : master.columns())
            if (std::find(opts.keylist.begin(), opts.keylist.end(), c.name) == opts.keylist.end())
                master_non_key[c.name] = 1;
        std::vector<std::string> kept;
        kept.reserve(using_cols_keep.size());
        for (const auto& name : using_cols_keep) {
            if (std::find(opts.keylist.begin(), opts.keylist.end(), name) == opts.keylist.end() &&
                master_non_key.count(name)) {
                const bool kinds_agree = master.col(name).is_factor == usingf.col(name).is_factor;
                if (opts.stata_plan) {
                    // `force`: a string/numeric clash keeps the master's column only.
                    if (kinds_agree) {
                        shared_vars.push_back(name);
                        if (do_update) overlap.push_back(name);
                    }
                } else if (do_update) {
                    overlap.push_back(name);
                }
                continue;  // master wins — drop the using's copy (update: merged in below)
            }
            kept.push_back(name);
        }
        using_cols_keep = std::move(kept);
    }

    // ---- build key matrices in a UNIFIED label space -----------------------
    // Factor keys must join by LABEL, not by per-file dictionary code: the two
    // files' code spaces are unrelated (level i of master has no relation to
    // level i of using — e.g. master levels [a,b,zz,c] vs using [a,b,c,d]
    // would falsely match zz↔c and c↔d by raw code). Unify the level tables
    // (master's levels first, then using-only levels appended — the same
    // convention Frame::append uses) and remap the using side's codes into
    // that space before hashing. A key that is factor in one file but numeric
    // in the other has no defined join semantics — hard error, matching the
    // pandas oracle (merging an object column on a float64 column raises).
    const size_t nkeys = opts.keylist.size();
    const int64_t nm = master.n_rows(), nu = usingf.n_rows();
    struct KeyMeta {
        bool is_factor = false;
        std::vector<std::string> levels;   // unified level table (master-first)
        std::vector<int32_t> u_remap;      // using code -> unified code
    };
    std::vector<KeyMeta> keymeta(nkeys);
    std::vector<std::vector<double>> MK(nkeys), UK(nkeys);
    for (size_t ki = 0; ki < nkeys; ++ki) {
        const FrameColumn& mc = master.col(opts.keylist[ki]);
        const FrameColumn& uc = usingf.col(opts.keylist[ki]);
        if (mc.is_factor != uc.is_factor)
            throw std::runtime_error("merge: key variable '" + opts.keylist[ki] +
                                     "' is a factor in one dataset but numeric in the other");
        MK[ki].resize(static_cast<size_t>(nm));
        UK[ki].resize(static_cast<size_t>(nu));
        if (!mc.is_factor) {
            for (int64_t i = 0; i < nm; ++i) MK[ki][static_cast<size_t>(i)] = mc.f64[i];
            for (int64_t i = 0; i < nu; ++i) UK[ki][static_cast<size_t>(i)] = uc.f64[i];
            continue;
        }
        KeyMeta& km = keymeta[ki];
        km.is_factor = true;
        km.levels = mc.levels.ref();
        std::unordered_map<std::string, int32_t> dict;
        for (size_t i = 0; i < km.levels.size(); ++i) dict[km.levels[i]] = static_cast<int32_t>(i);
        km.u_remap.resize(uc.levels.size());
        for (size_t i = 0; i < uc.levels.size(); ++i) {
            auto it = dict.find(uc.levels[i]);
            if (it != dict.end()) km.u_remap[i] = it->second;
            else {
                km.u_remap[i] = static_cast<int32_t>(km.levels.size());
                dict[uc.levels[i]] = km.u_remap[i];
                km.levels.push_back(uc.levels[i]);
            }
        }
        // master codes ARE unified-space codes already (its levels are the
        // unified table's prefix); -1 (missing) passes through — two missing
        // keys match each other, same as the pre-fix behavior and the oracle's
        // None-as-hashable convention.
        for (int64_t i = 0; i < nm; ++i) MK[ki][static_cast<size_t>(i)] = static_cast<double>(mc.codes[i]);
        for (int64_t i = 0; i < nu; ++i) {
            int32_t code = uc.codes[i];
            UK[ki][static_cast<size_t>(i)] = (code < 0) ? -1.0 : static_cast<double>(km.u_remap[code]);
        }
    }

    // cardinality enforcement
    auto find_dup = [&](const std::vector<std::vector<double>>& K, int64_t n) -> int64_t {
        std::unordered_map<RowKey, int64_t, RowKeyHash> cnt;
        for (int64_t i = 0; i < n; ++i) {
            RowKey k = make_key(K, i);
            if (++cnt[k] > 1) return i;
        }
        return -1;
    };
    if (!opts.sequential && (opts.type == "1:1" || opts.type == "1:m")) {
        int64_t d = find_dup(MK, nm);
        if (d >= 0) throw std::runtime_error("merge " + opts.type + ": master dataset is not unique on the key(s)");
    }
    if (!opts.sequential && (opts.type == "1:1" || opts.type == "m:1")) {
        int64_t d = find_dup(UK, nu);
        if (d >= 0) throw std::runtime_error("merge " + opts.type + ": using dataset is not unique on the key(s)");
    }

    // Build using-side lookup: key -> list of using row indices.
    std::unordered_map<RowKey, std::vector<int64_t>, RowKeyHash> u_by_key;
    for (int64_t i = 0; i < nu; ++i) u_by_key[make_key(UK, i)].push_back(i);
    // (MAXPERF Stage 3: the former `m_key_set` — an O(nm) hash build over the master keys —
    // was write-only dead code; the plan below matches via u_by_key + u_matched, so it is
    // dropped. Pure work removal, output-identical.)

    // Non-key columns for output: master's non-key columns, then using's
    // non-key columns (restricted by keepusing), in that order — matches
    // pandas .merge(on=keylist, how="outer") default column ordering
    // (left columns first, then right-only columns) closely enough for a
    // deterministic, self-consistent output (PIET's contract doesn't pin an
    // exact column order for merge — the varlist is read back from the hbin).
    std::vector<std::string> m_non_key, u_non_key;
    for (const auto& c : master.columns())
        if (std::find(opts.keylist.begin(), opts.keylist.end(), c.name) == opts.keylist.end()) m_non_key.push_back(c.name);
    for (const auto& name : using_cols_keep)
        if (std::find(opts.keylist.begin(), opts.keylist.end(), name) == opts.keylist.end()) u_non_key.push_back(name);

    // Row plan: (master_row or -1, using_row or -1), in master order first
    // (matched + master-only), then using-only rows in using order — matches
    // pandas outer-merge's stable "left rows in order, then right-only rows".
    struct Plan { int64_t mrow, urow; int merge_code; };
    std::vector<Plan> plan;
    plan.reserve(nm + nu);
    if (!opts.stata_plan) {
        std::vector<char> u_matched(nu, 0);
        for (int64_t i = 0; i < nm; ++i) {
            RowKey k = make_key(MK, i);
            auto it = u_by_key.find(k);
            if (it == u_by_key.end() || it->second.empty()) {
                plan.push_back({i, -1, 1});
            } else {
                for (int64_t ur : it->second) { plan.push_back({i, ur, 3}); u_matched[ur] = 1; }
            }
        }
        for (int64_t j = 0; j < nu; ++j) if (!u_matched[j]) plan.push_back({-1, j, 2});
    } else if (opts.sequential) {
        // merge 1:1 _n (Stata's merge): observation i with observation i.
        const int64_t n = std::max(nm, nu);
        for (int64_t i = 0; i < n; ++i) {
            const int64_t mr = i < nm ? i : -1, ur = i < nu ? i : -1;
            plan.push_back({mr, ur, mr >= 0 && ur >= 0 ? 3 : (mr >= 0 ? 1 : 2)});
        }
    } else {
        // Stata's key order: a string key by its text ("" first), a numeric one
        // by stata_cmp (reals, then . < .a < ... < .z).
        static const std::string kEmpty;
        auto key_cmp = [&](const std::vector<std::vector<double>>& K, int64_t a, int64_t b) -> int {
            for (size_t ki = 0; ki < nkeys; ++ki) {
                const double x = K[ki][static_cast<size_t>(a)], y = K[ki][static_cast<size_t>(b)];
                int c;
                if (keymeta[ki].is_factor) {
                    const std::string* sx = x < 0 ? &kEmpty : &keymeta[ki].levels[static_cast<size_t>(x)];
                    const std::string* sy = y < 0 ? &kEmpty : &keymeta[ki].levels[static_cast<size_t>(y)];
                    c = sx->compare(*sy);
                    c = c < 0 ? -1 : (c > 0 ? 1 : 0);
                } else {
                    c = stata_cmp(x, y);
                }
                if (c != 0) return c;
            }
            return 0;
        };
        std::vector<int64_t> mord(static_cast<size_t>(nm)), uord(static_cast<size_t>(nu));
        std::iota(mord.begin(), mord.end(), int64_t{0});
        std::iota(uord.begin(), uord.end(), int64_t{0});
        if (!opts.presorted) {
            std::stable_sort(mord.begin(), mord.end(),
                             [&](int64_t a, int64_t b) { return key_cmp(MK, a, b) < 0; });
            std::stable_sort(uord.begin(), uord.end(),
                             [&](int64_t a, int64_t b) { return key_cmp(UK, a, b) < 0; });
        }
        // Each key's using rows in using order, and its master rows in master order.
        std::unordered_map<RowKey, std::vector<int64_t>, RowKeyHash> ug, mg;
        for (int64_t j : uord) ug[make_key(UK, j)].push_back(j);
        for (int64_t i : mord) mg[make_key(MK, i)].push_back(i);
        std::unordered_map<RowKey, size_t, RowKeyHash> mseen;
        for (int64_t i : mord) {
            RowKey k = make_key(MK, i);
            const size_t pos = mseen[k]++;
            auto it = ug.find(k);
            if (it == ug.end() || it->second.empty()) {
                plan.push_back({i, -1, 1});
            } else {
                const std::vector<int64_t>& U = it->second;
                plan.push_back({i, U[std::min(pos, U.size() - 1)], 3});
            }
        }
        std::unordered_map<RowKey, size_t, RowKeyHash> useen;
        for (int64_t j : uord) {
            RowKey k = make_key(UK, j);
            const size_t pos = useen[k]++;
            auto it = mg.find(k);
            if (it == mg.end() || it->second.empty()) {
                plan.push_back({-1, j, 2});
            } else if (pos >= it->second.size()) {
                plan.push_back({it->second.back(), j, 3});
            }
        }
    }

    // ---- UPDATE merge: reclassify the matched rows into 3/4/5 -----------------
    // Oracle capture i (`merge 1:1 id using ..., update`, one overlapping
    // variable x): 1 master-only, 1 using-only, 1 "not updated" (3), 2 "missing
    // updated" (4), 2 "nonmissing conflict" (5).
    //
    // The code is per ROW, over ALL overlapping variables at once — NOT per variable.
    // capture h is the falsifier that settled it: the same fixture plus a
    // str3 marker that differs in every matched row put ALL FIVE matches at code 5
    // (n3=0, n4=0, n5=5) while x2/x4 were still FILLED from the using (20 and 40).
    // So the classification is a row-level max over the variables, and the data
    // operation is still per variable — a rule that assigned code 4 to the rows it
    // filled would have printed n4=2 there.
    struct OverlapMeta {
        std::string name;
        const FrameColumn* mc = nullptr;
        const FrameColumn* uc = nullptr;
        bool is_factor = false;
        std::vector<std::string> levels;  // unified (master-first), factor only
        std::vector<int32_t> u_remap;     // using code -> unified code, factor only
    };
    auto make_overlap_meta = [&](const std::string& name) {
        OverlapMeta om;
        om.name = name;
        om.mc = &master.col(name);
        om.uc = &usingf.col(name);
        if (om.mc->is_factor != om.uc->is_factor)
            throw std::runtime_error("merge update: variable '" + name +
                                     "' is string in one dataset and numeric in the other");
        om.is_factor = om.mc->is_factor;
        if (om.is_factor) {
            om.levels = om.mc->levels.ref();
            std::unordered_map<std::string, int32_t> dict;
            for (size_t i = 0; i < om.levels.size(); ++i) dict[om.levels[i]] = static_cast<int32_t>(i);
            om.u_remap.resize(om.uc->levels.size());
            for (size_t i = 0; i < om.uc->levels.size(); ++i) {
                auto it = dict.find(om.uc->levels[i]);
                if (it != dict.end()) om.u_remap[i] = it->second;
                else {
                    om.u_remap[i] = static_cast<int32_t>(om.levels.size());
                    dict[om.uc->levels[i]] = om.u_remap[i];
                    om.levels.push_back(om.uc->levels[i]);
                }
            }
        }
        return om;
    };
    std::vector<OverlapMeta> ov;
    if (do_update) {
        ov.reserve(overlap.size());
        for (const auto& name : overlap) ov.push_back(make_overlap_meta(name));
    }
    // One cell's verdict, shared by the classifier and the emitter below so the two
    // can never drift: FILL = master missing & using nonmissing, CONFLICT = both
    // nonmissing and different. A missing USING value is inert in both — [D] merge
    // (joinby's identical wording, d.pdf, PDF page 653): "A nonmissing value, however, will
    // never be replaced with a missing value."
    enum class Cell { None, Fill, Conflict };
    // A factor (string) column's Stata missing is the empty string: `mi(s)` is true
    // for s=="" and for nothing else. NOT oracle-pinned — the captured string case
    // (oracle_h's str3 tag, "M" vs "U") is a nonmissing conflict on both sides, so it
    // cannot separate this from "code<0 only". Reopen if a capture ever puts an empty
    // master string opposite a nonmissing using string.
    auto cell_missing = [](const OverlapMeta& om, bool from_master, int64_t row) -> bool {
        if (om.is_factor) {
            const FrameColumn& c = from_master ? *om.mc : *om.uc;
            int32_t code = c.codes[row];
            return code < 0 || c.levels[static_cast<size_t>(code)].empty();
        }
        const FrameColumn& c = from_master ? *om.mc : *om.uc;
        return std::isnan(c.f64[row]);
    };
    auto cell_verdict = [&](const OverlapMeta& om, int64_t mrow, int64_t urow) -> Cell {
        if (cell_missing(om, false, urow)) return Cell::None;
        if (cell_missing(om, true, mrow)) return Cell::Fill;
        bool same;
        if (om.is_factor) {
            same = om.mc->levels[static_cast<size_t>(om.mc->codes[mrow])] ==
                   om.uc->levels[static_cast<size_t>(om.uc->codes[urow])];
        } else {
            same = om.mc->f64[mrow] == om.uc->f64[urow];
        }
        return same ? Cell::None : Cell::Conflict;
    };
    if (do_update) {
        for (auto& p : plan) {
            if (p.merge_code != 3) continue;
            bool any_fill = false, any_conflict = false;
            for (const auto& om : ov) {
                Cell v = cell_verdict(om, p.mrow, p.urow);
                if (v == Cell::Fill) any_fill = true;
                else if (v == Cell::Conflict) any_conflict = true;
            }
            p.merge_code = any_conflict ? 5 : (any_fill ? 4 : 3);
        }
    }

    // assert() check (before keep() filtering, matching merge_op.py order).
    // `match` (3) in keep()/assert() covers an update merge's 4 and 5 as well: those
    // ARE matches, split three ways by the update. A caller that names 4 or 5
    // explicitly gets exact codes instead, so keep(1 4) can single out the fills.
    const bool exact_codes = opts.stata_plan;
    auto code_in = [exact_codes](const std::vector<int>& codes, int code) -> bool {
        if (std::find(codes.begin(), codes.end(), code) != codes.end()) return true;
        if (exact_codes) return false;
        const bool exp45 = std::find(codes.begin(), codes.end(), 4) != codes.end() ||
                           std::find(codes.begin(), codes.end(), 5) != codes.end();
        return !exp45 && code >= 3 &&
               std::find(codes.begin(), codes.end(), 3) != codes.end();
    };
    if (!opts.assert_codes.empty()) {
        const std::vector<int>& ac = opts.assert_codes;
        bool all_allowed = code_in(ac, 1) && code_in(ac, 2) && code_in(ac, 3) &&
                           (!exact_codes || (code_in(ac, 4) && code_in(ac, 5)));
        if (!all_allowed) {
            for (const auto& p : plan) {
                if (!code_in(ac, p.merge_code)) {
                    if (exact_codes)
                        throw EngineError("merge_assert",
                                          "merge assert(): assertion failed — unexpected _merge code " +
                                              std::to_string(p.merge_code));
                    throw std::runtime_error("merge assert(): assertion failed — unexpected _merge code " + std::to_string(p.merge_code));
                }
            }
        }
    }

    // keep() filter.
    std::vector<Plan> filtered;
    if (!opts.keep_codes.empty()) {
        for (const auto& p : plan) if (code_in(opts.keep_codes, p.merge_code)) filtered.push_back(p);
    } else {
        filtered = plan;
    }

    const int64_t nout = static_cast<int64_t>(filtered.size());
    Frame out;
    out.set_n_rows_hint(nout);

    // Emits a plain (non-key) column: value comes from master or using per row
    // depending on which side that row's Plan entry sources it from; the other
    // side is missing (this = an outer-join column, never present on both by
    // construction — see the earlier column-conflict check).
    // MAXPERF Stage 3 — morsel-parallel column materialization. Each output column is a
    // pure GATHER: out-row r reads src row filtered[r].{mrow,urow} (a const read of the
    // master/using column) and writes its own out[r]. No float accumulation, no cross-row
    // dependence, disjoint writes — so splitting the [0,nout) row range across ThreadPool
    // workers is BIT-IDENTICAL to the serial fill (①-gate: test_morsel_determinism). The
    // Frame append itself stays SEQUENTIAL (one column at a time, in fixed order) — only
    // the buffer fill is parallel. run() falls back to serial for nout<2 / 1 core / nested.
    // NMISS-CARRY (2026-08-21): count the missings in the SAME gather that fills the
    // column, the third and last of these (keep_if and joinby_gather_append are the
    // others). Without it merge's output columns reach the writer cold, the
    // warm-only header emits nothing for them, and PIET pays the full-column scan
    // this whole carry exists to remove — measured, that left merge's PIET half at
    // 12.0 s against joinby's 0.4 s once joinby alone was seeded.
    //
    // These fills are MORSEL-PARALLEL, so a shared counter would be a data race and
    // a non-deterministic total. Each worker accumulates into its own slot and the
    // slots are summed in fixed worker order afterwards, which keeps the count
    // bit-identical to a serial fill for the same reason the fill itself is
    // (§4.2: fixed partitions, fixed-order reduction, no atomic scatter-add).
    const int nworkers = std::max(1, ThreadPool::global().size());
    auto emit_col = [&](const std::string& name, bool from_master) {
        const FrameColumn& src = from_master ? master.col(name) : usingf.col(name);
        std::vector<int64_t> wmiss(static_cast<size_t>(nworkers), 0);
        if (src.is_factor) {
            std::vector<int32_t> codes(nout, -1);
            run_gather(nout, [&](int64_t rlo, int64_t rhi, int w) {
                int64_t m = 0;
                for (int64_t r = rlo; r < rhi; ++r) {
                    int64_t row = from_master ? filtered[r].mrow : filtered[r].urow;
                    if (row >= 0) codes[r] = src.codes[row];
                    if (codes[r] < 0) ++m;  // unmatched row, or a genuinely missing code
                }
                wmiss[static_cast<size_t>(w < nworkers ? w : 0)] += m;
            });
            out.append_factor_column_coded(name, std::move(codes), src.levels.ref());
            out.carry_identity_to_last(src);  // BUG-363
        } else {
            std::vector<double> vals(nout, NAd);
            // BUG-395 (2026-09-02): read through the narrow-aware F64Reader, NEVER
            // through `src.f64[row]`. That proxy is F64Col::ref(), which calls
            // ColumnData::ensure_wide() — a const method that MUTATES the shared
            // ColumnData (builds the widened cache) — and this loop runs on every
            // ThreadPool worker at once. A master that arrived narrow (keep_if's
            // gather keeps its columns f32) had every worker widening the same
            // column concurrently: two of them freed each other's buffer and the
            // engine session died with "pointer being freed was not allocated".
            // Wide inputs (anything read from an .hbin) never took that branch,
            // which is why every fixture stayed GREEN for eleven days. The reader
            // is the same cast ensure_wide performs, so the bytes are identical.
            const F64Reader sr = src.f64.reader();
            run_gather(nout, [&](int64_t rlo, int64_t rhi, int w) {
                int64_t m = 0;
                for (int64_t r = rlo; r < rhi; ++r) {
                    int64_t row = from_master ? filtered[r].mrow : filtered[r].urow;
                    if (row >= 0) vals[r] = sr[static_cast<std::size_t>(row)];
                    if (std::isnan(vals[r])) ++m;  // NAd fill, or a missing source value
                }
                wmiss[static_cast<size_t>(w < nworkers ? w : 0)] += m;
            });
            out.append_f64_column(name, std::move(vals));
            out.carry_identity_to_last(src);  // BUG-363
        }
        int64_t miss = 0;
        for (int64_t x : wmiss) miss += x;  // fixed order
        out.columns().back().seed_n_missing(miss);
    };
    // key columns: read from whichever side has the row (matched/master-only
    // -> master; using-only -> using). Matches pandas outer-merge key fill.
    // Factor keys emit codes in the UNIFIED level space built above (master
    // codes are already unified-space; using codes go through u_remap).
    auto emit_key = [&](size_t ki) {
        const std::string& k = opts.keylist[ki];
        const FrameColumn& msrc = master.col(k);
        const FrameColumn& usrc = usingf.col(k);  // hoisted out of the row loop (was a per-row map lookup)
        // NMISS-CARRY: same per-worker accumulation as emit_col below. A key column
        // left cold here would send the whole merge output back to the scan path,
        // since PIET's card build asks every column, not just the interesting ones.
        std::vector<int64_t> wmiss(static_cast<size_t>(nworkers), 0);
        if (msrc.is_factor) {
            const KeyMeta& km = keymeta[ki];
            std::vector<int32_t> codes(nout, -1);
            run_gather(nout, [&](int64_t rlo, int64_t rhi, int w) {
                int64_t m = 0;
                for (int64_t r = rlo; r < rhi; ++r) {
                    if (filtered[r].mrow >= 0) {
                        codes[r] = msrc.codes[filtered[r].mrow];
                    } else {
                        int32_t ucode = usrc.codes[filtered[r].urow];
                        if (ucode >= 0) codes[r] = km.u_remap[ucode];
                    }
                    if (codes[r] < 0) ++m;
                }
                wmiss[static_cast<size_t>(w < nworkers ? w : 0)] += m;
            });
            out.append_factor_column_coded(k, std::move(codes), km.levels);
            out.carry_identity_to_last(msrc);  // BUG-363: master wins on a key
        } else {
            std::vector<double> vals(nout, NAd);
            const F64Reader mr = msrc.f64.reader();  // BUG-395: see emit_col below — no widening on workers
            const F64Reader ur = usrc.f64.reader();
            run_gather(nout, [&](int64_t rlo, int64_t rhi, int w) {
                int64_t m = 0;
                for (int64_t r = rlo; r < rhi; ++r) {
                    if (filtered[r].mrow >= 0) vals[r] = mr[static_cast<std::size_t>(filtered[r].mrow)];
                    else vals[r] = ur[static_cast<std::size_t>(filtered[r].urow)];
                    if (std::isnan(vals[r])) ++m;
                }
                wmiss[static_cast<size_t>(w < nworkers ? w : 0)] += m;
            });
            out.append_f64_column(k, std::move(vals));
            out.carry_identity_to_last(msrc);  // BUG-363: master wins on a key
        }
        {
            int64_t miss = 0;
            for (int64_t x : wmiss) miss += x;  // fixed order
            out.columns().back().seed_n_missing(miss);
        }
    };
    // An overlapping variable under `update`: ONE output column (the master's name),
    // filled cell by cell from whichever side wins. Deliberately serial — the update
    // path is the rare one, and cell_verdict() reads through the f64 proxy, whose
    // ensure_wide() must not be reached from several workers at once (BUG-395).
    // stata_plan without `update`: a shared variable keeps the master's value on
    // every row the master has, and takes the using's on a using-only row
    // (`update_cells` false).
    auto emit_update_col = [&](const OverlapMeta& om, bool update_cells = true) {
        int64_t miss = 0;
        if (om.is_factor) {
            std::vector<int32_t> codes(nout, -1);
            for (int64_t r = 0; r < nout; ++r) {
                const int64_t mrow = filtered[r].mrow, urow = filtered[r].urow;
                int32_t code = -1;
                auto from_using = [&]() {
                    int32_t uc = om.uc->codes[urow];
                    return uc < 0 ? static_cast<int32_t>(-1) : om.u_remap[uc];
                };
                if (mrow < 0) code = from_using();
                else if (urow < 0 || !update_cells) code = om.mc->codes[mrow];
                else {
                    const Cell v = cell_verdict(om, mrow, urow);
                    const bool take_using = (v == Cell::Fill) || (v == Cell::Conflict && opts.replace);
                    code = take_using ? from_using() : om.mc->codes[mrow];
                }
                codes[r] = code;
                if (code < 0) ++miss;
            }
            out.append_factor_column_coded(om.name, std::move(codes), om.levels);
        } else {
            std::vector<double> vals(nout, NAd);
            for (int64_t r = 0; r < nout; ++r) {
                const int64_t mrow = filtered[r].mrow, urow = filtered[r].urow;
                double x;
                if (mrow < 0) x = om.uc->f64[urow];
                else if (urow < 0 || !update_cells) x = om.mc->f64[mrow];
                else {
                    const Cell v = cell_verdict(om, mrow, urow);
                    const bool take_using = (v == Cell::Fill) || (v == Cell::Conflict && opts.replace);
                    x = take_using ? om.uc->f64[urow] : om.mc->f64[mrow];
                }
                vals[r] = x;
                if (std::isnan(x)) ++miss;
            }
            out.append_f64_column(om.name, std::move(vals));
        }
        out.carry_identity_to_last(*om.mc);  // BUG-363: master owns the variable
        out.columns().back().seed_n_missing(miss);
    };
    if (!opts.stata_plan) {
        for (size_t ki = 0; ki < nkeys; ++ki) emit_key(ki);
        for (const auto& name : m_non_key) {
            const OverlapMeta* om = nullptr;
            for (const auto& o : ov) if (o.name == name) { om = &o; break; }
            if (om) emit_update_col(*om);
            else emit_col(name, true);
        }
    } else {
        // Master order, keys in place; a shared variable (update or not) through
        // emit_update_col, whose using-only rows read the using's value.
        std::vector<OverlapMeta> sv;
        if (!do_update) {
            sv.reserve(shared_vars.size());
            for (const auto& name : shared_vars) sv.push_back(make_overlap_meta(name));
        }
        for (const auto& c : master.columns()) {
            auto kit = std::find(opts.keylist.begin(), opts.keylist.end(), c.name);
            if (kit != opts.keylist.end()) {
                emit_key(static_cast<size_t>(kit - opts.keylist.begin()));
                continue;
            }
            const OverlapMeta* om = nullptr;
            for (const auto& o : ov) if (o.name == c.name) { om = &o; break; }
            if (om) { emit_update_col(*om); continue; }
            for (const auto& o : sv) if (o.name == c.name) { om = &o; break; }
            if (om) emit_update_col(*om, false);
            else emit_col(c.name, true);
        }
    }
    for (const auto& name : u_non_key) emit_col(name, false);

    if (!opts.nogenerate) {
        std::vector<double> merge_col(nout);
        for (int64_t r = 0; r < nout; ++r) merge_col[r] = static_cast<double>(filtered[r].merge_code);
        // generate(newvar) RENAMES the marker — oracle capture h has
        // `confirm variable _merge` at r(111) after generate(mm), so `_merge` must
        // not also exist.
        out.append_f64_column(opts.generate.empty() ? std::string("_merge") : opts.generate,
                              std::move(merge_col));
        // `_merge` is a generated code (1/2/3) for every output row by construction —
        // never missing. Stated, not counted.
        out.columns().back().seed_n_missing(0);
    }
    out.finalize_after_bulk_load();
    // Stata's merge ("reestablish sort order"): after keep(), a 1:1 merge with
    // no using-only row left runs `sort varlist`, so `: sortedby` reads the keys
    // (w2d_uncovered.log:3988 merge_sorted_ctl `|id|`). The rows already sit in key
    // order here: the stata_plan lists master rows sorted by the keys (or presorted,
    // which PIET checked) and only code-2 rows are appended after them; a 1:1 key is
    // unique, so the claim is true. m:1, 1:m, m:m and `1:1 _n` mint nothing, as in
    // merge.ado, and the pandas-order plan keeps its claimless result.
    if (opts.stata_plan && !opts.sequential && opts.type == "1:1" && !opts.keylist.empty() &&
        std::none_of(filtered.begin(), filtered.end(),
                     [](const Plan& p) { return p.merge_code == 2; }))
        out.set_sorted_by(opts.keylist);

    MergeResult res;
    for (const auto& p : plan) {
        if (p.merge_code == 1) res.master_only++;
        else if (p.merge_code == 2) res.using_only++;
        else {
            res.matched++;
            if (!do_update) continue;  // standard merge: every match is 3, no split
            if (p.merge_code == 4) res.matched_missupdated++;
            else if (p.merge_code == 5) res.matched_conflict++;
            else res.matched_notupdated++;
        }
    }
    res.frame = std::move(out);
    return res;
}


}  // namespace cecon
