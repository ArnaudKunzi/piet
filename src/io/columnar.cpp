// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "io/columnar.h"
#include <set>
#include <map>
#ifndef _WIN32
#include <dlfcn.h>
#endif
#include "data/frame.h"  // RAMDIET A: shared read census
#include "core/error.h"
#include "io/win_compat.h"  // native_open/native_fd_size: wide (_wopen) on Windows for non-ASCII paths (BUG-050)
#include "core/eng_counters.h"  // WS-D2: one increment per PHYSICAL dataset-file open

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include "nlohmann/json.hpp"

namespace cecon {

using nlohmann::json;

static constexpr char kMagic[8] = {'C', 'E', 'C', 'O', 'N', 'H', 'B', '1'};
static constexpr int64_t kAlign = 64;

static int64_t align_up(int64_t x, int64_t a = kAlign) { return (x + a - 1) / a * a; }

// Well-formed UTF-8 as nlohmann's serializer and parser both demand it (Unicode
// Table 3-7: no overlong form, no surrogate, nothing past U+10FFFF), plus the
// marker byte at the front.
bool hbin_level_needs_escape(const std::string& level) {
    if (!level.empty() && level[0] == '\0') return true;
    const auto* p = reinterpret_cast<const unsigned char*>(level.data());
    const size_t n = level.size();
    size_t i = 0;
    while (i < n) {
        const unsigned char c = p[i];
        if (c < 0x80) { ++i; continue; }
        size_t len;
        unsigned char lo = 0x80, hi = 0xBF;
        if (c >= 0xC2 && c <= 0xDF) len = 2;
        else if (c == 0xE0) { len = 3; lo = 0xA0; }
        else if (c == 0xED) { len = 3; hi = 0x9F; }
        else if (c >= 0xE1 && c <= 0xEF) len = 3;
        else if (c == 0xF0) { len = 4; lo = 0x90; }
        else if (c == 0xF4) { len = 4; hi = 0x8F; }
        else if (c >= 0xF1 && c <= 0xF3) len = 4;
        else return true;
        if (i + len > n || p[i + 1] < lo || p[i + 1] > hi) return true;
        for (size_t k = 2; k < len; ++k)
            if (p[i + k] < 0x80 || p[i + k] > 0xBF) return true;
        i += len;
    }
    return false;
}

std::string hbin_level_escape(const std::string& level) {
    static const char* kHex = "0123456789abcdef";
    std::string out(1, '\0');
    out.reserve(1 + 2 * level.size());
    for (const unsigned char c : level) {
        out += kHex[c >> 4];
        out += kHex[c & 0xF];
    }
    return out;
}

std::string hbin_level_json_text(const std::string& text) {
    return hbin_level_needs_escape(text) ? hbin_level_escape(text) : text;
}

void hbin_level_unescape(std::string& level) {
    if (level.empty() || level[0] != '\0') return;
    const auto nibble = [](char h) {
        return h <= '9' ? h - '0' : h - 'a' + 10;
    };
    std::string out;
    out.reserve((level.size() - 1) / 2);
    for (size_t i = 1; i + 1 < level.size(); i += 2)
        out += static_cast<char>((nibble(level[i]) << 4) | nibble(level[i + 1]));
    level = std::move(out);
}

namespace {

// OPENPATH-OPENCOST-FIX (2026-07-30): SAX reader for the .hbin header.
//
// WHY. `Dataset::open` used to `json::parse` the header into a DOM and then
// `.get<std::vector<std::string>>()` each factor column's `levels` out of it.
// ROWLOOP-OPENPATH-SPLIT measured that the DOM build (`header_parse_ms`) is
// 89.4-92.3% of open()'s wall time and that the whole cross-shape delta lands
// there — because the level dictionaries are JSON arrays INSIDE the header
// text, so a 21,598-level column costs 21,598 heap `std::string` nodes to
// build, 21,598 more `std::string` copies to `.get<>` out, and 21,598 frees
// when the DOM dies (that last one after the diag emit, so it was not even
// billed). See instruments/rowloop_rlload_subsplit.md §5.2-§5.3.
//
// WHAT. One SAX pass over the header text streams each level string DIRECTLY
// into the destination `std::vector<std::string>` (moved out of the lexer's
// token buffer, exactly as nlohmann's own DOM handler does), so a level costs
// one string instead of three, and the intermediate DOM never exists.
// The on-disk format is UNCHANGED — this is purely how the same bytes are
// read, which is what keeps the task inside its Art-13 scope line ("in-memory
// cache/parse is in scope; a disk-format change is not").
//
// Order-independence is deliberate: cobra writes the header via nlohmann
// (keys sorted) but `tools/to_hbin.py` writes it via Python's json (insertion
// order), so the handler keys off names, never position, and ignores unknown
// keys — same tolerance the DOM path had.
struct HbinHeaderSax {
    using string_t = json::string_t;
    using binary_t = json::binary_t;
    using number_integer_t = json::number_integer_t;
    using number_unsigned_t = json::number_unsigned_t;
    using number_float_t = json::number_float_t;

    struct RawCol {
        std::string name;
        std::string dtype;
        int64_t offset = 0;
        int64_t nbytes = 0;
        bool has_name = false, has_dtype = false, has_offset = false, has_nbytes = false;
        bool has_levels = false;
        std::vector<std::string> levels;
        // ALLOC-C3A-ARC1-CARRY: how many level strings this column's dictionary
        // actually held. Equals `levels.size()` unless the entry was SKIPPED
        // (carried), in which case `levels` stays empty and this is the only
        // record of the length — which is what lets the carry check that the
        // dictionary it is about to move in still has the right number of
        // entries.
        int64_t levels_seen = 0;
        // P1-A0-FORMAT-CARRIAGE: OPTIONAL. No `has_format` flag and no
        // completeness check below — absent is a legal, expected header (every
        // pre-A0 file), so "" is the only "missing" signal needed.
        std::string format;
        // BUG-198: OPTIONAL, same discipline as `format` — absent is legal and
        // is what every pre-BUG-198 file says, so "" carries "unstated".
        std::string label;
        // NMISS-CARRY: OPTIONAL, and -1 (not 0) is the "unstated" value — a real
        // count of zero is a legal, common answer, so absent and zero must not
        // collapse. Every pre-2026-08-20 file lands here as -1.
        int64_t n_missing = -1;
    };

    int64_t n_rows = 0;
    bool has_n_rows = false;
    // BUG-272: OPTIONAL root "sorted_by" key (the sort claim, space-joined varlist).
    // "" = absent — the same absent-vs-empty discipline as RawCol::format/label, and
    // what every pre-BUG-272 file says.
    std::string sorted_by;
    std::vector<RawCol> cols;
    std::string error;  // non-empty => reject the header

    enum class S { Top, Root, ColsArr, ColObj, Levels, Skip };
    S st = S::Top;
    S ret = S::Top;  // state to resume after a skipped subtree
    int skip_depth = 0;
    std::string root_key;  // last key seen directly under the root object
    std::string col_key;   // last key seen directly under a column object

    // ALLOC-C3A-ARC1-CARRY (V2). When non-null, `skip_levels[i]` says "column i
    // already has its dictionary in hand — lex its `levels` array but build
    // nothing". Indexed BY POSITION, not by name, and that is forced, not a
    // shortcut: cobra serializes the header through nlohmann, whose object keys
    // come out sorted, so `levels` arrives BEFORE `name`. Position is the only
    // identity available at the moment the decision has to be made. The carry
    // then proves the positional assumption after the fact by comparing every
    // name (a mismatch falls back), so a reordering writer costs a re-open
    // rather than a wrong dictionary.
    const std::vector<unsigned char>* skip_levels = nullptr;
    bool skipping_levels = false;  // is the CURRENT levels array being discarded?

    // ALLOC-C3A-ARC2-BYTESPAN (V3). The input is ONE column object — the delta
    // entry the span layer localized — not a whole header. Same handler, same
    // key tolerance, same level streaming; only the outermost `{` means
    // something different. Reusing it rather than writing a second reader is
    // deliberate: the delta column must be read by EXACTLY the code that reads
    // it on the V2 path, or the two paths could disagree about a field and
    // PIET_VERIFY_DESCRIPTOR would be catching our own divergence instead of a
    // stale descriptor.
    bool single_column = false;

    bool skip_this_column() const {
        if (!skip_levels) return false;
        const size_t i = cols.size() - 1;  // cols.back() is the column being read
        return i < skip_levels->size() && (*skip_levels)[i] != 0;
    }

    // A `levels` array must hold strings and nothing else, and `columns` must
    // hold objects and nothing else: the DOM path threw a json type_error on
    // either, and silently dropping such an element would shorten a dictionary
    // that factor codes index into (an out-of-range read later, far from here).
    bool bad_scalar() {
        if (st == S::Levels) { error = "non-string entry in a levels array"; return true; }
        if (st == S::ColsArr) { error = "non-object entry in the columns array"; return true; }
        return false;
    }

    bool start_object(std::size_t) {
        switch (st) {
            case S::Top:
                if (single_column) { cols.emplace_back(); col_key.clear(); st = S::ColObj; return true; }
                st = S::Root;
                return true;
            case S::ColsArr: cols.emplace_back(); col_key.clear(); st = S::ColObj; return true;
            case S::Skip: ++skip_depth; return true;
            case S::Levels: error = "non-string entry in a levels array"; return false;
            default: ret = st; st = S::Skip; skip_depth = 1; return true;
        }
    }
    bool end_object() {
        switch (st) {
            case S::Root: st = S::Top; return true;
            case S::ColObj: st = single_column ? S::Top : S::ColsArr; return true;
            case S::Skip: if (--skip_depth == 0) st = ret; return true;
            default: return true;
        }
    }
    bool start_array(std::size_t) {
        if (st == S::Root && root_key == "columns") { st = S::ColsArr; return true; }
        if (st == S::ColObj && col_key == "levels") {
            cols.back().has_levels = true;
            skipping_levels = skip_this_column();
            st = S::Levels;
            return true;
        }
        if (st == S::Skip) { ++skip_depth; return true; }
        if (st == S::Levels) { error = "non-string entry in a levels array"; return false; }
        if (st == S::ColsArr) { error = "non-object entry in the columns array"; return false; }
        ret = st;
        st = S::Skip;
        skip_depth = 1;
        return true;
    }
    bool end_array() {
        switch (st) {
            case S::ColsArr: st = S::Root; return true;
            case S::Levels: st = S::ColObj; return true;
            case S::Skip: if (--skip_depth == 0) st = ret; return true;
            default: return true;
        }
    }
    bool key(string_t& k) {
        if (st == S::Root) root_key = std::move(k);
        else if (st == S::ColObj) col_key = std::move(k);
        return true;
    }
    bool string(string_t& v) {
        // The move is what makes this cheap AND is officially safe: nlohmann's
        // own json_sax_dom_parser moves out of this same reference, and the
        // lexer clears its token buffer at the start of every token.
        if (st == S::Levels) {
            ++cols.back().levels_seen;
            // ALLOC-C3A-ARC1-CARRY (V2): a carried dictionary is lexed and
            // dropped. Leaving `v` alone (no move) is the whole saving — the
            // lexer reuses its one token buffer, so the column costs a single
            // high-water allocation instead of one heap string per level plus
            // the vector that holds them plus the frees when it dies.
            if (skipping_levels) return true;
            cols.back().levels.push_back(std::move(v));
            hbin_level_unescape(cols.back().levels.back());
            return true;
        }
        if (st == S::ColsArr) { error = "non-object entry in the columns array"; return false; }
        if (st == S::ColObj) {
            if (col_key == "name") { cols.back().name = std::move(v); cols.back().has_name = true; }
            else if (col_key == "dtype") { cols.back().dtype = std::move(v); cols.back().has_dtype = true; }
            else if (col_key == "format") { cols.back().format = std::move(v); }
            else if (col_key == "label") {  // BUG-198; escaped as a level is (BUG-1372)
                cols.back().label = std::move(v);
                hbin_level_unescape(cols.back().label);
            }
        } else if (st == S::Root) {
            if (root_key == "sorted_by") sorted_by = std::move(v);  // BUG-272
        }
        return true;
    }
    bool number_integer(number_integer_t v) { return num(static_cast<int64_t>(v)); }
    bool number_unsigned(number_unsigned_t v) { return num(static_cast<int64_t>(v)); }
    bool number_float(number_float_t, const string_t&) { return !bad_scalar(); }
    bool boolean(bool) { return !bad_scalar(); }
    bool null() { return !bad_scalar(); }
    bool binary(binary_t&) { return !bad_scalar(); }

    bool num(int64_t v) {
        if (bad_scalar()) return false;
        if (st == S::Root) {
            if (root_key == "n_rows") { n_rows = v; has_n_rows = true; }
        } else if (st == S::ColObj) {
            if (col_key == "offset") { cols.back().offset = v; cols.back().has_offset = true; }
            else if (col_key == "nbytes") { cols.back().nbytes = v; cols.back().has_nbytes = true; }
            else if (col_key == "n_missing") { cols.back().n_missing = v; }  // NMISS-CARRY
        }
        return true;
    }
    bool parse_error(std::size_t, const std::string&, const json::exception& ex) {
        if (error.empty()) error = ex.what();
        return false;
    }
};

// Shared prologue for both open paths: open + fstat + mmap + magic +
// header_len. Factored out by ALLOC-C3A-ARC1-CARRY so the carried path cannot
// drift from the honest one; the operations, their order and the exact error
// strings are unchanged from the pre-carry `open`. Unmaps before throwing (the
// old code relied on ~Dataset for that, having stashed the mapping into the
// half-built Dataset before the magic check).
struct MappedHbin {
    void* m = nullptr;
    size_t len = 0;
    uint64_t header_len = 0;
    const char* base() const { return static_cast<const char*>(m); }
    const char* hbeg() const { return base() + 16; }
};

MappedHbin map_hbin(const std::string& path) {
    // WS-D2: counted HERE, at the syscall, not at a caller — a counter that lives
    // next to the open cannot drift from what the process actually did.
    cecon::engctr::addDataFileOpen();
    int fd = native_open(path, O_RDONLY);
    if (fd < 0) {
        // Say WHY. A bare "cannot open" cannot distinguish "the file is gone"
        // (ENOENT — a GC or a caller deleted it) from "the process is out of
        // descriptors" (EMFILE) from "the volume is unhappy", and those have
        // three unrelated fixes. Found 2026-09-01 when an internal-volume
        // acceptance run died on this line 4/4 while the external-volume runs
        // passed 3/3, and the message could not tell us which of them it was.
        const int e = errno;
        throw EngineError("data_io",
                          "cannot open " + path + " (errno " + std::to_string(e) + ": " +
                              std::strerror(e) + ")",
                          path);
    }
    int64_t file_size = 0;
    if (!native_fd_size(fd, file_size)) {
        ::close(fd);
        throw EngineError("data_io", "fstat failed: " + path, path);
    }
    size_t len = static_cast<size_t>(file_size);
    void* m = mmap(nullptr, len, PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);
    if (m == MAP_FAILED) throw EngineError("data_io", "mmap failed: " + path, path);

    MappedHbin mh;
    mh.m = m;
    mh.len = len;
    if (len < 16 || std::memcmp(mh.base(), kMagic, 8) != 0) {
        munmap(m, len);
        throw EngineError("data_io", "bad magic (not a .hbin file): " + path, path);
    }
    std::memcpy(&mh.header_len, mh.base() + 8, 8);
    if (16 + mh.header_len > len) {
        munmap(m, len);
        throw EngineError("data_io", "truncated header: " + path, path);
    }
    return mh;
}

// BUG-124: validate a column extent against the MAPPED length before handing
// out a pointer. A writer that died mid-blob (ENOSPC, kill) can leave a file
// whose header (written first, complete) promises data the file does not hold;
// readers then scan past the mapping's end and SIGSEGV at exactly base+len
// (KERN_INVALID_ADDRESS, .ips-confirmed). Also pin nbytes to the row count so a
// self-inconsistent header can't drive an in-mapping overrun of a neighbouring
// column. Well-formed files (every writer emits exact extents) pass untouched.
//
// ALLOC-C3A-ARC1-CARRY: this runs in FULL on the carried path too. Skipping it
// there was raised and rejected in the design brief (§6): a carried descriptor
// that has gone stale does not produce a wrong number, it produces a scan off
// the end of the mapping — the exact BUG-124 crash this guard exists for.
// G9: one greppable line per open_carried call, env-gated and off by default,
// same shape and same discipline as OPEN_PHASE_DIAG above. This is what makes
// "the carry actually fires" a number instead of an assertion — a run whose
// every line says `outcome=<some fallback>` has a correct answer and a
// completely vacuous performance claim, and only the emit can tell the two
// apart from outside the process.
// ALLOC-C3A-ARC2A-PREVMAP appended `src=`: the previous mapping's size in bytes,
// or `none`. Same env gate, same one-line-per-call shape, appended LAST so an
// existing consumer that splits on the old two fields is unaffected. This is the
// enabler's direct read — `src=none` on every line means the deferral did not
// reach open_carried, which no green gate would otherwise reveal, and the largest
// value seen IS the coexistence exposure (Dataset::carrySrcCoexistBytesMax).
// ALLOC-C3A-ARC2-BYTESPAN appended `span=`: which V3 span-layer branch this call
// took. Appended LAST again, for the same reason `src=` was — a consumer that
// splits on the older fields is unaffected. `outcome=carried span=localized` and
// `outcome=carried span=fallback_span_localize` are BOTH correct carries; they
// differ in whether the unchanged region was lexed, which no other observable
// distinguishes.
void carry_diag_emit(CarryOutcome o, const std::string& path, const CarrySource& src,
                     SpanOutcome sp) {
    static const bool kCarryDiag = std::getenv("COBRA_CARRY_DIAG") != nullptr;
    if (!kCarryDiag) return;
    if (src.usable()) {
        std::fprintf(stderr, "CARRY_DIAG outcome=%s path=%s src=%zu span=%s\n",
                     carry_outcome_name(o), path.c_str(), src.size, span_outcome_name(sp));
    } else {
        std::fprintf(stderr, "CARRY_DIAG outcome=%s path=%s src=none span=%s\n",
                     carry_outcome_name(o), path.c_str(), span_outcome_name(sp));
    }
}

// ─── ALLOC-C3A-ARC2-BYTESPAN (V3) ───────────────────────────────────────────
//
// V2 skips BUILDING the carried dictionaries but still LEXES them: nlohmann's
// lexer walks every byte of a skipped `levels` array into its token buffer just
// to find the closing quote. V3 removes that by proving, in bytes, that the
// unchanged region of the header is the same text it was last time — then it
// parses only the one column entry that changed.
//
// The proof is a two-ended `memcmp` (common prefix + common suffix) against the
// PREVIOUS header, which ARC2A keeps alive and hands in as `CarrySource`.

// THE TRAP THAT DECIDES THIS FUNCTION (SPANPROBE §3). `patch_hbin` writes a
// FIXED-CAPACITY header: `header_len` stays constant across a setcol chain
// (2,388,912 B on the probed corpus) while the JSON content shrinks from
// 2,251,745 to 491,573 B, the remainder being trailing ASCII spaces. The pad
// sits at the END — exactly where a suffix scan starts. Comparing over the
// padded capacity turns a size change in one entry into "everything after it
// changed": measured, the `obs_id` case reads as 5.83% skippable padded vs
// 99.99% over content, a **17× understatement**, and it does so in the shape
// that looks most like a genuine failure. Nothing had changed there but
// DISPLACEMENT. So the suffix scan runs against the JSON content end, never
// against `header_len` ([[measurement-reading-traps]], in code this time).
size_t json_content_end(const char* s, size_t len) {
    size_t e = len;
    while (e > 0 && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\n' || s[e - 1] == '\r' ||
                     s[e - 1] == '\0'))
        --e;
    return e;
}

// What the span layer needs to know about a header's SHAPE, and nothing else.
struct HdrLayout {
    bool ok = false;
    std::vector<std::pair<size_t, size_t>> entries;  // [begin,end) of each column object
    size_t cols_begin = 0, cols_end = 0;             // the '[' and one-past the ']'
    int64_t n_rows = 0;
    bool has_n_rows = false;
    // BUG-272: the root "sorted_by" string, read from THIS header's bytes ("" =
    // absent). Captured here for the same reason n_rows is: the V3 span path builds
    // a Dataset without ever running the full SAX parse, and inferring the claim
    // from the carried descriptors (which carry no root fields) would be a guess.
    // Raw-copied without unescaping — Stata variable names cannot contain `"` or
    // `\`, and a hand-edited value that does contain one fails Frame::set_sorted_by's
    // name validation downstream and clears, which is the safe direction.
    std::string sorted_by;
};

// String-aware STRUCTURAL scan — deliberately not a parser.
//
// It records only the byte range of each column entry, the columns array's own
// range, and the root `n_rows` value. It never allocates, never builds a token
// buffer, and never touches a level string's contents: that is the whole point,
// because building one token buffer per dictionary entry (108,384 of them for
// `individual_id` alone) is precisely the cost V3 exists to skip. One linear
// pass over the same bytes the two-ended memcmp already walks.
//
// Quote/escape state is tracked because a level string may contain `{`, `}`,
// `[`, `]` or `,` — a naive brace scan would mis-locate an entry boundary on
// real data, and the failure would be silent ([[piet-quote-aware-qualifier]] is
// the same class one layer up).
HdrLayout scan_header_layout(const char* s, size_t len) {
    HdrLayout out;
    bool in_str = false, esc = false;
    int depth = 0;
    size_t str_beg = 0, str_end = 0;   // last completed string token
    bool str_ready = false;
    std::string pending_key;           // last key at the CURRENT container level
    bool in_cols = false;              // inside the "columns" array
    bool want_cols_array = false;      // the next '[' at root is the columns array
    bool want_sb_value = false;        // BUG-272: the next root string is sorted_by's value
    size_t entry_beg = 0;

    for (size_t i = 0; i < len; ++i) {
        const char c = s[i];
        if (in_str) {
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"') {
                in_str = false; str_end = i; str_ready = true;
                // BUG-272: the string that immediately follows `"sorted_by":` at
                // root is the claim. Any structural byte in between clears the
                // flag (below), so a non-string value can never mis-capture.
                if (want_sb_value) { out.sorted_by.assign(s + str_beg, str_end - str_beg); want_sb_value = false; }
            }
            continue;
        }
        if (want_sb_value && c != '"' && c != ' ' && c != '\t' && c != '\n' && c != '\r')
            want_sb_value = false;  // value was not a string after all
        switch (c) {
            case '"': in_str = true; str_beg = i + 1; str_ready = false; break;
            case ':':
                if (str_ready) {
                    pending_key.assign(s + str_beg, str_end - str_beg);
                    if (depth == 1) {
                        want_cols_array = (pending_key == "columns");
                        want_sb_value = (pending_key == "sorted_by");  // BUG-272
                        // Tiny, bounded, and READ rather than assumed: the span
                        // path must not INFER n_rows from the carried extents
                        // when the file states it a few bytes away. Hand-scanned
                        // (not strtoll) because the mapping is not NUL-terminated.
                        if (pending_key == "n_rows") {
                            size_t j = i + 1;
                            while (j < len && (s[j] == ' ' || s[j] == '\t')) ++j;
                            const bool neg = (j < len && s[j] == '-');
                            if (neg) ++j;
                            int64_t v = 0;
                            size_t d0 = j;
                            while (j < len && s[j] >= '0' && s[j] <= '9') {
                                v = v * 10 + (s[j] - '0');
                                ++j;
                                if (j - d0 > 18) break;  // absurd; leave has_n_rows false
                            }
                            if (j > d0 && j - d0 <= 18) {
                                out.n_rows = neg ? -v : v;
                                out.has_n_rows = true;
                            }
                        }
                    }
                }
                str_ready = false;
                break;
            case '{':
                if (in_cols && depth == 2) entry_beg = i;
                ++depth;
                str_ready = false;
                break;
            case '}':
                --depth;
                if (in_cols && depth == 2) out.entries.emplace_back(entry_beg, i + 1);
                str_ready = false;
                break;
            case '[':
                if (depth == 1 && want_cols_array && !in_cols) {
                    in_cols = true;
                    out.cols_begin = i;
                    want_cols_array = false;
                }
                ++depth;
                str_ready = false;
                break;
            case ']':
                --depth;
                if (in_cols && depth == 1) { in_cols = false; out.cols_end = i + 1; }
                str_ready = false;
                break;
            case ',': str_ready = false; break;
            default: break;  // whitespace between a key and its ':' must not clear str_ready
        }
        if (depth < 0) break;  // malformed; `ok` stays false
    }
    out.ok = (depth == 0 && !in_str && out.cols_end > out.cols_begin);
    return out;
}

bool extent_ok(DType dtype, int64_t off, int64_t nbytes, int64_t data_start, int64_t flen,
               int64_t n_rows) {
    const int64_t elem = (dtype == DType::Factor) ? 4 : 8;
    // Each term bounded against the file length BEFORE the sum, so the sum
    // (three values each <= flen) cannot overflow int64 on any real file.
    return !(off < 0 || nbytes < 0 || off > flen || nbytes > flen ||
             data_start + off + nbytes > flen || nbytes % elem != 0 || nbytes / elem != n_rows);
}

}  // namespace

const char* carry_outcome_name(CarryOutcome o) {
    switch (o) {
        case CarryOutcome::Carried: return "carried";
        case CarryOutcome::FallbackDisabled: return "disabled";
        case CarryOutcome::FallbackNoCarried: return "no_carried";
        case CarryOutcome::FallbackBadDelta: return "bad_delta";
        case CarryOutcome::FallbackDeltaShape: return "delta_shape";
        case CarryOutcome::FallbackColumnCount: return "column_count";
        case CarryOutcome::FallbackColumnName: return "column_name";
        case CarryOutcome::FallbackLevelCount: return "level_count";
        case CarryOutcome::FallbackHeader: return "header";
        case CarryOutcome::FallbackRowCount: return "row_count";
        case CarryOutcome::FallbackExtent: return "extent";
    }
    return "unknown";
}

const char* span_outcome_name(SpanOutcome o) {
    switch (o) {
        case SpanOutcome::Localized: return "localized";
        // NOTE for anyone counting these in a log: `span=localized` is a PREFIX
        // of `span=localized_empty`, so an unanchored `span=localized` grep sums
        // both. Anchor it (`span=localized$`) — [[measurement-reading-traps]].
        case SpanOutcome::LocalizedEmpty: return "localized_empty";
        case SpanOutcome::Disabled: return "span_disabled";
        case SpanOutcome::NoPrevBytes: return "no_prev_bytes";
        case SpanOutcome::PrevHeaderBad: return "prev_header_bad";
        case SpanOutcome::PrevOffsetUnknown: return "prev_offset_unknown";
        case SpanOutcome::FallbackSpanLocalize: return "fallback_span_localize";
        case SpanOutcome::IndexMismatch: return "span_index_mismatch";
        case SpanOutcome::ColumnCount: return "span_column_count";
        case SpanOutcome::RowCountMoved: return "span_row_count";
        case SpanOutcome::DeltaEntryBad: return "span_delta_entry_bad";
        case SpanOutcome::ExtentRejected: return "span_extent";
    }
    return "unknown";
}

int64_t Dataset::carryFallbackCount(CarryOutcome o) {
    const int i = static_cast<int>(o);
    if (i < 0 || i >= 16) return 0;
    return carry_fallback_[i];
}

int64_t Dataset::spanOutcomeCount(SpanOutcome o) {
    const int i = static_cast<int>(o);
    if (i < 0 || i >= 16) return 0;
    return span_outcome_[i];
}

void Dataset::resetCarryCounters() {
    carried_open_count_ = 0;
    full_parse_count_ = 0;
    for (auto& c : carry_fallback_) c = 0;
    carry_src_present_count_ = 0;
    carry_src_coexist_bytes_max_ = 0;
    span_attempt_count_ = 0;
    span_ok_count_ = 0;
    for (auto& c : span_outcome_) c = 0;
}

Dataset::~Dataset() {
    if (map_ && map_ != MAP_FAILED) munmap(map_, map_len_);
}

Dataset& Dataset::operator=(Dataset&& o) noexcept {
    if (this != &o) {
        if (map_ && map_ != MAP_FAILED) munmap(map_, map_len_);
        map_ = o.map_;
        map_len_ = o.map_len_;
        n_rows_ = o.n_rows_;
        columns_ = std::move(o.columns_);
        index_ = std::move(o.index_);
        path_ = std::move(o.path_);
        sorted_by_ = std::move(o.sorted_by_);  // BUG-272 (hand-enumerated move: every member)
        o.map_ = nullptr;
        o.map_len_ = 0;
        o.n_rows_ = 0;
    }
    return *this;
}

const Column& Dataset::col(const std::string& name) const {
    auto it = index_.find(name);
    if (it == index_.end()) throw EngineError("missing_column", "column not found: " + name, name);
    // ONESHOT-RAMDIET A: the OTHER read gate. Estimators never touch a Frame — they go
    // model.cpp::require_f64 -> Dataset::col -> .f64(). Counting only Frame::col would
    // have reported the data verbs and missed every regression's actual reads.
    cecon_coltouch_note(name);
    return columns_[it->second];
}

std::vector<Column> Dataset::release_columns() {
    std::vector<Column> out = std::move(columns_);
    columns_.clear();
    index_.clear();
    for (auto& c : out) c.data = nullptr;  // addressed a mapping we no longer promise to keep
    return out;
}

Dataset Dataset::make_view(const std::string& path, int64_t n_rows, std::vector<Column> cols,
                           std::string sorted_by) {
    // Non-owning view: no mmap taken (map_ stays null → ~Dataset frees nothing).
    // The Column.data pointers reference buffers the caller owns and keeps alive.
    Dataset ds;
    ds.map_ = nullptr;
    ds.map_len_ = 0;
    ds.n_rows_ = n_rows;
    ds.path_ = path;
    ds.sorted_by_ = std::move(sorted_by);  // BUG-272: the resident view carries the claim too
    ds.columns_ = std::move(cols);
    for (int i = 0; i < static_cast<int>(ds.columns_.size()); ++i)
        ds.index_[ds.columns_[i].name] = i;
    return ds;
}

// ---- LADDER PROBE (RAMDIET-2, env-gated) ----
// Distinct columns whose payload is demanded as a contiguous double array, per
// compaction generation. See Column::f64 in columnar.h for why this one function is
// the exact place to ask. Generations are closed by Frame::note_materialization,
// which calls cecon_ladder_probe_close().
namespace {
std::set<std::string> g_f64_gen;
std::vector<int64_t> g_f64_widths;
std::map<void*, int64_t> g_f64_sites;             // caller -> raw call count
std::map<void*, std::set<std::string>> g_f64_cols; // caller -> DISTINCT columns demanded
const char* f64probe_path() {
    static const char* p = std::getenv("COBRA_F64PROBE");
    return p;
}
}  // namespace

bool g_ladder_probe_on = [] { const char* p = std::getenv("COBRA_F64PROBE"); return p && *p; }();

// ONESHOT-RAMDIET WS-A column-touch census. Lives HERE (not in frame.cpp) because
// this TU is compiled standalone into piet_native, which cannot link Frame's
// symbols — and both read gates (Frame::col and Dataset::col below) need the one
// tally. Env-gated by COBRA_COLTOUCH; generation boundaries close via
// cecon_coltouch_generation() from the compaction funnels.
namespace {
std::map<std::string, int64_t> g_touch_tally;
std::set<std::string> g_touch_gen;
std::vector<std::pair<int64_t, int64_t>> g_touch_widths;  // {named, rebuilt}
int64_t g_touch_since_flush = 0;
const char* coltouch_path() {
    static const char* p = std::getenv("COBRA_COLTOUCH");
    return (p && *p) ? p : nullptr;
}
void coltouch_flush() {
    if (std::FILE* f = std::fopen(coltouch_path(), "w")) {
        for (const auto& kv : g_touch_tally)
            std::fprintf(f, "touch %s %lld\n", kv.first.c_str(),
                         static_cast<long long>(kv.second));
        std::fprintf(f, "generations %zu\n", g_touch_widths.size());
        for (std::size_t i = 0; i < g_touch_widths.size(); ++i)
            std::fprintf(f, "genwidth %zu named=%lld rebuilt=%lld\n", i,
                         static_cast<long long>(g_touch_widths[i].first),
                         static_cast<long long>(g_touch_widths[i].second));
        std::fclose(f);
    }
}
}  // namespace

void cecon_coltouch_note(const std::string& name) {
    if (!coltouch_path()) return;
    ++g_touch_tally[name];
    g_touch_gen.insert(name);
    // Rewrite the whole tally every 4096 acquisitions rather than appending per touch
    // (chatty) or at exit (unreachable: PIET SIGTERMs its workers, atexit never runs).
    if (++g_touch_since_flush < 4096) return;
    g_touch_since_flush = 0;
    coltouch_flush();
}

void cecon_coltouch_generation(int64_t ncols) {
    if (!coltouch_path()) return;
    g_touch_widths.push_back({static_cast<int64_t>(g_touch_gen.size()), ncols});
    g_touch_gen.clear();
}

void cecon_ladder_probe_note(const std::string& name, void* site) {
    if (!f64probe_path()) return;
    g_f64_gen.insert(name);
    ++g_f64_sites[site];
    g_f64_cols[site].insert(name);
}

void cecon_ladder_probe_close() {
    const char* p = f64probe_path();
    if (!p) return;
    g_f64_widths.push_back(static_cast<int64_t>(g_f64_gen.size()));
    g_f64_gen.clear();
    if (std::FILE* f = std::fopen(p, "w")) {
        for (std::size_t i = 0; i < g_f64_widths.size(); ++i)
            std::fprintf(f, "f64gen %zu demanded=%lld\n", i,
                         static_cast<long long>(g_f64_widths[i]));
        // Symbolize IN-PROCESS: an offline atos guesses the ASLR slide and names the
        // wrong function (measured, RAMDIET §7.1).
        for (const auto& kv : g_f64_sites) {
#ifdef _WIN32
            // No dladdr on Windows; this probe is a diagnostic, so the raw name
            // is just left unresolved.
            const char* sym = "?";
#else
            Dl_info info{};
            const char* sym = (dladdr(kv.first, &info) && info.dli_sname) ? info.dli_sname : "?";
#endif
            std::fprintf(f, "f64site calls=%lld cols=%zu %s\n",
                         static_cast<long long>(kv.second), g_f64_cols[kv.first].size(), sym);
        }
        std::fclose(f);
    }
}

Dataset Dataset::open(const std::string& path) {
    // ROWLOOP-OPENPATH-SPLIT (2026-07-29): report-only, env-gated {header_parse,
    // level_materialize} split of this function's cost — instrumentation only,
    // zero behavior change, zero cost when COBRA_OPEN_PHASE_DIAG is unset (the
    // getenv is cached once per process; the disabled branch skips every
    // steady_clock call). See internal capture
    // rowloop_rlload_subsplit.md §5 for why: rl_load's dominant open() call
    // scales with the written frame's level-dictionary size, not row count, and
    // this settles whether that is the JSON header parse or the per-column
    // level-vector deep-copy.
    // OPENPATH-OPENCOST-FIX (2026-07-30): it was the parse — the header is read
    // by HbinHeaderSax now, one pass, no DOM. The counters stay (report-only,
    // same env gate, same emit line); `level_materialize_ms` is structurally 0
    // because that phase was deleted rather than sped up — see the emit below.
    static const bool kOpenPhaseDiag = std::getenv("COBRA_OPEN_PHASE_DIAG") != nullptr;
    using Clock = std::chrono::steady_clock;
    auto ms_since = [](Clock::time_point t0) {
        return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    };
    const auto t_open0 = kOpenPhaseDiag ? Clock::now() : Clock::time_point{};
    ++full_parse_count_;
    MappedHbin mh = map_hbin(path);
    const size_t len = mh.len;
    const uint64_t header_len = mh.header_len;

    Dataset ds;
    ds.map_ = mh.m;
    ds.map_len_ = len;
    ds.path_ = path;
    const char* base = mh.base();

    // OPENCOST-FIX: parse straight off the mapping (no header-sized std::string
    // copy) and straight into the destination columns (no DOM). See HbinHeaderSax.
    const auto t_parse0 = kOpenPhaseDiag ? Clock::now() : Clock::time_point{};
    HbinHeaderSax hx;
    const char* hbeg = mh.hbeg();
    const bool header_ok = json::sax_parse(hbeg, hbeg + header_len, &hx) && hx.error.empty();
    const double header_parse_ms = kOpenPhaseDiag ? ms_since(t_parse0) : 0.0;
    if (!header_ok) {
        throw EngineError("data_io",
                          "malformed .hbin header: " + path +
                              (hx.error.empty() ? "" : " (" + hx.error + ")"),
                          path);
    }
    if (!hx.has_n_rows) {
        throw EngineError("data_io", "malformed .hbin header (no n_rows): " + path, path);
    }

    ds.n_rows_ = hx.n_rows;
    ds.sorted_by_ = std::move(hx.sorted_by);  // BUG-272 ("" when the key is absent)
    int64_t data_start = align_up(16 + static_cast<int64_t>(header_len));

    long level_materialize_calls = 0;

    for (auto& c : hx.cols) {
        if (!c.has_name || !c.has_dtype || !c.has_offset || !c.has_nbytes) {
            throw EngineError("data_io",
                              "malformed .hbin header (column missing name/dtype/offset/nbytes): " +
                                  path,
                              path);
        }
        Column col;
        col.name = std::move(c.name);
        col.dtype = (c.dtype == "factor") ? DType::Factor : DType::F64;
        col.format = std::move(c.format);  // P1-A0: optional; "" when the key was absent
        col.label = std::move(c.label);    // BUG-198: same optionality, same "" = absent
        col.n_missing = c.n_missing;       // NMISS-CARRY: -1 = the key was absent
        int64_t off = c.offset;
        col.nbytes = c.nbytes;
        // BUG-124 extent validation — see extent_ok(). A torn file becomes an
        // honest data_io error instead of memory corruption.
        if (!extent_ok(col.dtype, off, col.nbytes, data_start, static_cast<int64_t>(len),
                       ds.n_rows_)) {
            throw EngineError("data_io",
                              "truncated or corrupt .hbin (column '" + col.name +
                                  "' extends past end of file — incomplete write, e.g. disk full): " +
                                  path,
                              path);
        }
        col.data = base + data_start + off;
        col.data_offset = off;  // ARC2-BYTESPAN: survives release_columns(); `data` does not
        if (c.has_levels) {
            col.levels = std::move(c.levels);
            ++level_materialize_calls;
        }
        ds.index_[col.name] = static_cast<int>(ds.columns_.size());
        ds.columns_.push_back(std::move(col));
    }

    if (kOpenPhaseDiag) {
        // Residual completeness check (rll_* precedent, rowloop_rlload_subsplit.md
        // §1): open_total_ms is the WHOLE function's wall time (native_open+fstat+
        // mmap+magic-check included), so open_residual_ms = open_total_ms - the
        // two billed phases is a genuine arithmetic check, not a definition — it
        // proves the two counters are a real partition of the parts that cost,
        // not just two numbers that happen to be smaller than the total.
        const double open_total_ms = ms_since(t_open0);
        // OPENCOST-FIX: `level_materialize_ms` is now STRUCTURALLY 0.00 — not
        // "too small to measure" but "the phase no longer exists". The second
        // pass it billed (`.get<std::vector<std::string>>()` off an already-built
        // DOM) was deleted; levels are now filled during the parse, so their cost
        // is inside header_parse_ms BY CONSTRUCTION. The field and its call count
        // are kept so before/after emits stay directly greppable and comparable —
        // `level_materialize_calls` still counts factor columns with a dictionary.
        // (Same honesty rule as §3's genuine 0.00 for ensureMaterializedHead.)
        const double level_materialize_ms = 0.0;
        const double open_residual_ms = open_total_ms - header_parse_ms - level_materialize_ms;
        // Single-line, greppable, machine-parseable — one emit per open() call.
        std::fprintf(stderr,
                      "OPEN_PHASE_DIAG header_parse_ms=%.4f header_parse_calls=1 "
                      "level_materialize_ms=%.4f level_materialize_calls=%ld "
                      "open_total_ms=%.4f open_residual_ms=%.4f\n",
                      header_parse_ms, level_materialize_ms, level_materialize_calls,
                      open_total_ms, open_residual_ms);
    }
    return ds;
}

namespace {

// PIET_VERIFY_DESCRIPTOR=1 (gate/debug only). Re-open `path` the honest way and
// compare EVERY field of every column against the carried build, level strings
// included. A mismatch aborts loudly: the point of the mode is that a stale
// descriptor cannot pass quietly, so it must not degrade into a fallback.
//
// Count-neutral by construction ([[instrument-capture-order-halfrecord]]): the
// verification open() would otherwise inflate fullParseCount and make the G9
// fallback report unreadable, so the counters are saved and restored around it.
void verify_carried_or_abort(const Dataset& got, const std::string& path) {
    const int64_t saved_full = Dataset::fullParseCount();
    const int64_t saved_carried = Dataset::carriedOpenCount();
    Dataset want = Dataset::open(path);
    auto fail = [&](const std::string& what) {
        std::fprintf(stderr,
                     "PIET_VERIFY_DESCRIPTOR: carried open of %s disagrees with a full parse "
                     "(%s) — aborting rather than returning a dataset built on a stale "
                     "descriptor\n",
                     path.c_str(), what.c_str());
        std::fflush(stderr);
        std::abort();
    };
    if (got.n_rows() != want.n_rows()) fail("n_rows");
    if (got.n_cols() != want.n_cols()) fail("n_cols");
    if (got.sorted_by() != want.sorted_by()) fail("sorted_by");  // BUG-272: root field, both carry paths
    const char* gbase = static_cast<const char*>(got.map_base());
    const char* wbase = static_cast<const char*>(want.map_base());
    for (int i = 0; i < want.n_cols(); ++i) {
        const Column& g = got.columns()[i];
        const Column& w = want.columns()[i];
        const std::string at = "column " + std::to_string(i) + " '" + w.name + "': ";
        if (g.name != w.name) fail(at + "name");
        if (g.dtype != w.dtype) fail(at + "dtype");
        if (g.nbytes != w.nbytes) fail(at + "nbytes");
        if (g.format != w.format) fail(at + "format");
        if (g.label != w.label) fail(at + "label");  // BUG-198: carried like format
        // Pointers differ (two mappings) — the comparable thing is the offset
        // each column sits at inside its own mapping.
        if (static_cast<const char*>(g.data) - gbase != static_cast<const char*>(w.data) - wbase)
            fail(at + "data offset");
        if (g.levels.size() != w.levels.size()) fail(at + "level count");
        for (size_t k = 0; k < w.levels.size(); ++k)
            if (g.levels[k] != w.levels[k]) fail(at + "level string " + std::to_string(k));
    }
    Dataset::setCarryCounters(saved_carried, saved_full);
}

}  // namespace

Dataset Dataset::open_carried(const std::string& path, std::vector<Column>&& carried,
                              const CarryDelta& delta, CarryOutcome* why,
                              const CarrySource& src) {
    // ARC2-BYTESPAN: the span layer's own verdict, reported alongside the carry
    // outcome on every path (including the bails, which happen before the span
    // layer runs and therefore honestly read `no_prev_bytes`).
    SpanOutcome span = SpanOutcome::NoPrevBytes;

    // Every uncertain branch lands here: the answer is whatever a full parse
    // says, and the reason is recorded so G9 can show the carry actually fires
    // (a fallback rate of 100% would make any speed claim vacuous).
    auto bail = [&](CarryOutcome o) -> Dataset {
        carry_fallback_[static_cast<int>(o)]++;
        if (why) *why = o;
        carry_diag_emit(o, path, src, span);
        return Dataset::open(path);
    };

    // ALLOC-C3A-ARC2A-PREVMAP: count the DELIVERY, before any bail can hide it.
    // This is the enabler's own landing evidence and it must not be conditional
    // on the carry succeeding — "the previous bytes reached open_carried" and
    // "the carry fired" are different claims, and ARC2A only makes the first.
    if (src.usable()) ++carry_src_present_count_;

    // COBRA_NO_CARRY=1 forces the OFF arm. Same binary, same build, one branch
    // apart — which is what makes the G6 A/B and the paired bench a comparison
    // of the carry rather than of two builds that also differ in link order,
    // inlining and whatever else drifted between them
    // ([[measurement-reading-traps]], and the 3-arm skip-gate-toggle precedent
    // in [[runaway-attribution-3arm-control]]).
    static const bool kNoCarry = std::getenv("COBRA_NO_CARRY") != nullptr;
    if (kNoCarry) return bail(CarryOutcome::FallbackDisabled);

    if (carried.empty()) return bail(CarryOutcome::FallbackNoCarried);
    if (delta.name.empty()) return bail(CarryOutcome::FallbackBadDelta);

    // Where the delta column must land, and how many columns the new header
    // must therefore have. Replace reuses the slot (patch_hbin re-dumps the
    // source's column order); append puts the new column last, after the
    // clone's EOF.
    size_t found = carried.size();
    for (size_t i = 0; i < carried.size(); ++i) {
        if (carried[i].name == delta.name) { found = i; break; }
    }
    size_t delta_index = 0, expected_cols = 0;
    if (delta.kind == CarryKind::Replace) {
        if (found == carried.size()) return bail(CarryOutcome::FallbackDeltaShape);
        delta_index = found;
        expected_cols = carried.size();
    } else {
        if (found != carried.size()) return bail(CarryOutcome::FallbackDeltaShape);
        delta_index = carried.size();
        expected_cols = carried.size() + 1;
    }

    // setcol cannot change the row count (contract_A §28), so the carried
    // columns' own extents state what n_rows must still be.
    const int64_t carried_rows =
        carried[0].nbytes / ((carried[0].dtype == DType::Factor) ? 4 : 8);

    std::vector<unsigned char> skip(expected_cols, 0);
    for (size_t i = 0; i < carried.size(); ++i) skip[i] = (i == delta_index) ? 0 : 1;

    MappedHbin mh = map_hbin(path);  // throws exactly what open() throws

    // ALLOC-C3A-ARC2A-PREVMAP: from here to the caller's releaseCarryPrev() the
    // previous mapping and this one are both live. Virtual address space is free;
    // what is not free is that pages touched in BOTH are resident twice, so the
    // exposed size is reported as a deterministic byte count (report-only —
    // an RSS sample would move with machine load, this does not).
    if (src.usable() && static_cast<int64_t>(src.size) > carry_src_coexist_bytes_max_)
        carry_src_coexist_bytes_max_ = static_cast<int64_t>(src.size);

    auto drop = [&](CarryOutcome o) -> Dataset {
        munmap(mh.m, mh.len);
        return bail(o);
    };

    const int64_t data_start0 = align_up(16 + static_cast<int64_t>(mh.header_len));
    const int64_t flen0 = static_cast<int64_t>(mh.len);

    // ── ALLOC-C3A-ARC2-BYTESPAN (V3): try to skip lexing the unchanged region ──
    //
    // Everything below is an OMISSIBLE layer over V2 (brief §6): it either
    // produces the same Dataset V2 would, or it declines and V2 runs. It never
    // throws, never changes a value, and never suppresses a check — extent
    // validation runs on EVERY column here exactly as it does on the V2 path,
    // because a stale descriptor's failure mode is not a wrong number but a scan
    // off the end of the mapping (BUG-124), and skipping that guard for speed
    // was raised and rejected in the brief.
    {
        static const bool kNoSpan = std::getenv("COBRA_NO_SPAN") != nullptr;
        auto span_fail = [&](SpanOutcome o) {
            span = o;
            span_outcome_[static_cast<int>(o)]++;
        };
        if (kNoSpan) {
            span_fail(SpanOutcome::Disabled);
        } else if (!src.usable()) {
            span_fail(SpanOutcome::NoPrevBytes);
        } else {
            ++span_attempt_count_;
            // The previous file as it was mapped: magic + header_len + header.
            const char* pfile = static_cast<const char*>(src.base);
            uint64_t p_hdr_len = 0;
            bool prev_ok = src.size >= 16 && std::memcmp(pfile, kMagic, 8) == 0;
            if (prev_ok) {
                std::memcpy(&p_hdr_len, pfile + 8, 8);
                prev_ok = (16 + p_hdr_len <= src.size);
            }
            // Every carried offset must be one this process READ from a header.
            bool offsets_known = true;
            for (const auto& c : carried)
                if (c.data_offset < 0) { offsets_known = false; break; }

            if (!prev_ok) {
                span_fail(SpanOutcome::PrevHeaderBad);
            } else if (!offsets_known) {
                span_fail(SpanOutcome::PrevOffsetUnknown);
            } else {
                const char* pbeg = pfile + 16;
                const char* nbeg = mh.hbeg();
                const size_t plen = json_content_end(pbeg, static_cast<size_t>(p_hdr_len));
                const size_t nlen = json_content_end(nbeg, static_cast<size_t>(mh.header_len));

                // Two-ended memcmp, over CONTENT (never over the padded
                // capacity — SPANPROBE §3, the 17× trap).
                const size_t cap = std::min(plen, nlen);
                size_t pre = 0;
                while (pre < cap && pbeg[pre] == nbeg[pre]) ++pre;
                size_t suf = 0;
                while (suf < cap - pre && pbeg[plen - 1 - suf] == nbeg[nlen - 1 - suf]) ++suf;
                const size_t span_beg = pre;
                const size_t span_end = nlen - suf;

                // ARC2-EMPTYSPAN: the two headers' CONTENT is byte-identical.
                // Stated as "same length AND the prefix scan consumed all of
                // it" rather than `span_beg == span_end`, because an empty span
                // is also what a pure SHRINK produces (new content a proper
                // prefix of the old): there the headers differ and nothing may
                // be assumed. Only Replace can take this route —
                // `carried.size() == expected_cols` is exactly that condition,
                // and it is also the literal precondition of the assembly
                // below, which reads every descriptor out of `carried`. An
                // append whose header did not grow has no descriptor for the
                // new column in either place, so it stays a miss.
                const bool header_identical =
                    (plen == nlen) && (pre == plen) && carried.size() == expected_cols;

                HdrLayout lay = scan_header_layout(nbeg, nlen);
                // Which column entries does the change touch? Expanding the raw
                // span to entry boundaries is REQUIRED, not cosmetic: the
                // minimal differing region measured on real data was 43 bytes
                // sitting INSIDE a `levels` array (`2_0","v2_1","v2_2",…`),
                // which does not say which column it belongs to (SPANPROBE §4a).
                size_t hits = 0, hit_i = 0;
                if (lay.ok && span_end > span_beg) {
                    for (size_t i = 0; i < lay.entries.size(); ++i) {
                        const auto& e = lay.entries[i];
                        if (e.first < span_end && span_beg < e.second) { ++hits; hit_i = i; }
                    }
                }
                // Span bytes outside the one entry are legal only as array
                // punctuation inside `columns` — that is the APPEND shape, where
                // the change begins at the `,` between the last old entry and
                // the new one.
                bool outside_clean = (hits == 1);
                if (outside_clean) {
                    const auto& e = lay.entries[hit_i];
                    for (size_t i = span_beg; i < span_end && outside_clean; ++i) {
                        if (i >= e.first && i < e.second) continue;
                        const char c2 = nbeg[i];
                        outside_clean = (i > lay.cols_begin && i + 1 < lay.cols_end) &&
                                        (c2 == ',' || c2 == ' ' || c2 == '\t' || c2 == '\n' ||
                                         c2 == '\r');
                    }
                }

                // Two ways in, mutually exclusive by construction (an identical
                // header has an empty span, so `hits` is 0). Everything the
                // layer checks AFTER this point is checked for BOTH: the entry
                // count, the root `n_rows`, and every column's extent. The
                // identical route drops exactly one thing — parsing the changed
                // entry — because there is no changed entry.
                const bool identical = header_identical && lay.ok;
                const bool localized = lay.ok && hits == 1 && outside_clean;

                if (!identical && !localized) {
                    span_fail(SpanOutcome::FallbackSpanLocalize);
                } else if (!identical && hit_i != delta_index) {
                    span_fail(SpanOutcome::IndexMismatch);
                } else if (lay.entries.size() != expected_cols) {
                    span_fail(SpanOutcome::ColumnCount);
                } else if (!lay.has_n_rows || lay.n_rows != carried_rows) {
                    span_fail(SpanOutcome::RowCountMoved);
                } else {
                    // Parse EXACTLY one column object — the delta entry. On the
                    // identical route there is none: the delta column's own
                    // descriptor text is part of the region proven byte-equal,
                    // so `carried[delta_index]` IS what the file says. (Its
                    // VALUES moved — that is why we are here — but a descriptor
                    // states layout: name, dtype, offset, nbytes, levels, and
                    // all five are unchanged text.)
                    HbinHeaderSax dx;
                    bool dok = identical;
                    if (!identical) {
                        const auto& e = lay.entries[hit_i];
                        dx.single_column = true;
                        dok = json::sax_parse(nbeg + e.first, nbeg + e.second, &dx) &&
                              dx.error.empty() && dx.cols.size() == 1 && dx.cols[0].has_name &&
                              dx.cols[0].has_dtype && dx.cols[0].has_offset &&
                              dx.cols[0].has_nbytes && dx.cols[0].name == delta.name;
                    }
                    if (!dok) {
                        span_fail(SpanOutcome::DeltaEntryBad);
                    } else {
                        // Assemble, validating every extent — carried columns
                        // included, whose placement comes from the descriptor
                        // because their header text is PROVEN byte-identical.
                        std::vector<Column> built;
                        built.reserve(expected_cols);
                        bool extents_ok = true;
                        for (size_t i = 0; i < expected_cols && extents_ok; ++i) {
                            Column col;
                            if (i == delta_index && !identical) {
                                auto& dc = dx.cols[0];
                                col.name = dc.name;
                                col.dtype = (dc.dtype == "factor") ? DType::Factor : DType::F64;
                                col.format = dc.format;
                                col.label = dc.label;
                                col.nbytes = dc.nbytes;
                                col.data_offset = dc.offset;
                                col.levels = std::move(dc.levels);
                            } else {
                                col.name = carried[i].name;
                                col.dtype = carried[i].dtype;
                                col.format = carried[i].format;
                                col.label = carried[i].label;
                                col.nbytes = carried[i].nbytes;
                                col.data_offset = carried[i].data_offset;
                                col.levels = std::move(carried[i].levels);
                            }
                            extents_ok = extent_ok(col.dtype, col.data_offset, col.nbytes,
                                                   data_start0, flen0, lay.n_rows);
                            col.data = mh.base() + data_start0 + col.data_offset;
                            built.push_back(std::move(col));
                        }
                        if (!extents_ok) {
                            // Hand the dictionaries back: V2 re-validates from
                            // the file and can still carry them. On the
                            // identical route the delta slot's dictionary also
                            // came out of `carried`, so it goes back too — a
                            // declining layer must leave `carried` as it found
                            // it.
                            for (size_t i = 0; i < built.size() && i < carried.size(); ++i)
                                if (i != delta_index || identical)
                                    carried[i].levels = std::move(built[i].levels);
                            span_fail(SpanOutcome::ExtentRejected);
                        } else {
                            Dataset ds;
                            ds.map_ = mh.m;
                            ds.map_len_ = mh.len;
                            ds.path_ = path;
                            ds.n_rows_ = lay.n_rows;
                            // BUG-272: read from THIS header's bytes by the layout
                            // scan (same rationale as n_rows above) — the V3 path
                            // never runs the full SAX parse that would carry it.
                            ds.sorted_by_ = std::move(lay.sorted_by);
                            ds.columns_ = std::move(built);
                            for (int i = 0; i < static_cast<int>(ds.columns_.size()); ++i)
                                ds.index_[ds.columns_[i].name] = i;

                            span = identical ? SpanOutcome::LocalizedEmpty : SpanOutcome::Localized;
                            span_outcome_[static_cast<int>(span)]++;
                            ++span_ok_count_;
                            ++carried_open_count_;
                            if (why) *why = CarryOutcome::Carried;
                            carry_diag_emit(CarryOutcome::Carried, path, src, span);
                            static const bool kVerifyDescriptor =
                                std::getenv("PIET_VERIFY_DESCRIPTOR") != nullptr;
                            if (kVerifyDescriptor) verify_carried_or_abort(ds, path);
                            return ds;
                        }
                    }
                }
            }
        }
    }
    // ── end V3; from here the V2 (ARC 1) path runs unchanged ────────────────

    HbinHeaderSax hx;
    hx.skip_levels = &skip;
    const char* hbeg = mh.hbeg();
    if (!json::sax_parse(hbeg, hbeg + mh.header_len, &hx) || !hx.error.empty() || !hx.has_n_rows)
        return drop(CarryOutcome::FallbackHeader);
    if (hx.n_rows != carried_rows) return drop(CarryOutcome::FallbackRowCount);
    if (hx.cols.size() != expected_cols) return drop(CarryOutcome::FallbackColumnCount);

    const int64_t data_start = data_start0;
    const int64_t flen = flen0;

    // Validate EVERYTHING before committing to the carry, so a rejection costs
    // only a re-parse and never a half-built Dataset.
    for (size_t i = 0; i < hx.cols.size(); ++i) {
        const auto& c = hx.cols[i];
        if (!c.has_name || !c.has_dtype || !c.has_offset || !c.has_nbytes)
            return drop(CarryOutcome::FallbackHeader);
        const DType dt = (c.dtype == "factor") ? DType::Factor : DType::F64;
        if (!extent_ok(dt, c.offset, c.nbytes, data_start, flen, hx.n_rows))
            return drop(CarryOutcome::FallbackExtent);
        if (i >= carried.size() || i == delta_index) continue;
        // A carried column: its identity is the positional assumption this
        // design rests on, so prove it rather than trust it.
        if (c.name != carried[i].name || dt != carried[i].dtype)
            return drop(CarryOutcome::FallbackColumnName);
        // The dictionary we are about to move in was never read from THIS file.
        // Its length is the one thing the skipped parse still learned, and a
        // changed length means the file's dictionary is not the carried one.
        if (c.levels_seen != static_cast<int64_t>(carried[i].levels.size()))
            return drop(CarryOutcome::FallbackLevelCount);
    }

    Dataset ds;
    ds.map_ = mh.m;
    ds.map_len_ = mh.len;
    ds.path_ = path;
    ds.n_rows_ = hx.n_rows;
    ds.sorted_by_ = std::move(hx.sorted_by);  // BUG-272: V2 parses the header in full
    const char* base = mh.base();
    ds.columns_.reserve(hx.cols.size());
    for (size_t i = 0; i < hx.cols.size(); ++i) {
        auto& c = hx.cols[i];
        Column col;
        col.name = std::move(c.name);
        col.dtype = (c.dtype == "factor") ? DType::Factor : DType::F64;
        col.format = std::move(c.format);
        col.label = std::move(c.label);  // BUG-198
        col.nbytes = c.nbytes;
        col.data = base + data_start + c.offset;
        col.data_offset = c.offset;
        if (i < carried.size() && i != delta_index) {
            col.levels = std::move(carried[i].levels);  // the saving: no re-lex, no re-alloc
        } else if (c.has_levels) {
            col.levels = std::move(c.levels);
        }
        ds.index_[col.name] = static_cast<int>(ds.columns_.size());
        ds.columns_.push_back(std::move(col));
    }

    ++carried_open_count_;
    if (why) *why = CarryOutcome::Carried;
    carry_diag_emit(CarryOutcome::Carried, path, src, span);

    static const bool kVerifyDescriptor = std::getenv("PIET_VERIFY_DESCRIPTOR") != nullptr;
    if (kVerifyDescriptor) verify_carried_or_abort(ds, path);
    return ds;
}

// ─────────────────────────── BUG-198: header metadata patch ───────────────────
//
// The one site that makes a saved .hbin state the session's format/label layer.
// See the header for the semantics contract; the notes here are about the
// mechanism, i.e. the parts that would be easy to get quietly wrong.
MetaPatchResult patch_hbin_meta(const std::string& path, const std::vector<ColMetaPatch>& patch) {
    MetaPatchResult res;
    if (patch.empty()) return res;

    // Read the file's own header text. A DOM parse is right HERE (unlike
    // Dataset::open, which is hot and uses the SAX reader): this runs once per
    // `save`, it must round-trip keys it does not understand, and a DOM is the
    // only shape that can re-emit an unknown key untouched. Level dictionaries
    // ride through as JSON arrays without ever becoming C++ strings.
    std::string bytes;
    {
        std::ifstream f(to_native_path(path), std::ios::binary);
        if (!f) throw EngineError("data_io", "cannot open for metadata patch: " + path, path);
        std::ostringstream ss;
        ss << f.rdbuf();
        bytes = ss.str();
    }
    if (bytes.size() < 16 || std::memcmp(bytes.data(), kMagic, 8) != 0) {
        throw EngineError("data_io", "not a .hbin file (bad magic): " + path, path);
    }
    uint64_t header_len = 0;
    std::memcpy(&header_len, bytes.data() + 8, 8);
    const int64_t data_start = align_up(16 + static_cast<int64_t>(header_len));
    if (static_cast<int64_t>(bytes.size()) < data_start) {
        throw EngineError("data_io", "truncated .hbin (header outruns the file): " + path, path);
    }

    json header;
    try {
        header = json::parse(bytes.data() + 16, bytes.data() + 16 + header_len);
    } catch (const json::exception& e) {
        throw EngineError("data_io",
                          "malformed .hbin header, cannot patch metadata: " + path + " (" +
                              e.what() + ")",
                          path);
    }
    if (!header.contains("columns") || !header["columns"].is_array()) {
        throw EngineError("data_io", "malformed .hbin header (no columns array): " + path, path);
    }

    // Name-keyed, because the caller's live card and the file agree on names,
    // not on order (a `drop`/`order` between load and save reorders one side).
    std::unordered_map<std::string, const ColMetaPatch*> want;
    want.reserve(patch.size());
    for (const auto& p : patch) want[p.name] = &p;

    // Apply. `key_set` is the whole absent-vs-empty discipline in one place:
    // nullopt leaves the key exactly as found, "" ERASES it (back to "unknown"),
    // non-empty sets it. Erasing rather than writing `""` is what keeps a
    // cleared override indistinguishable from a file that never had one — the
    // A0 law this key inherited.
    auto key_set = [&](json& jc, const char* key, const std::optional<std::string>& v) -> bool {
        if (!v.has_value()) return false;
        const bool have = jc.contains(key) && jc[key].is_string();
        const std::string cur = have ? jc[key].get<std::string>() : std::string();
        if (v->empty()) {
            if (!have) return false;
            jc.erase(key);
            return true;
        }
        if (have && cur == *v) return false;
        jc[key] = *v;
        return true;
    };
    for (json& jc : header["columns"]) {
        if (!jc.contains("name") || !jc["name"].is_string()) continue;
        auto it = want.find(jc["name"].get<std::string>());
        if (it == want.end()) continue;
        bool moved = key_set(jc, "format", it->second->format);
        // BUG-1372: a label that is not UTF-8 is stored escaped, as a level is.
        const std::optional<std::string>& label = it->second->label;
        moved = key_set(jc, "label",
                        label.has_value() ? std::optional<std::string>(hbin_level_json_text(*label))
                                          : std::nullopt) ||
                moved;
        if (moved) ++res.columns_changed;
    }
    // NO-OP IS BYTE-EXACT (contract item 1). This early return is the freeze
    // corpus's protection: the common save states nothing new, so nothing is
    // written and the file cannot drift by so much as a re-dumped separator.
    if (res.columns_changed == 0) return res;
    res.changed = true;

    std::string hjson = header.dump();
    if (static_cast<int64_t>(hjson.size()) <= static_cast<int64_t>(header_len)) {
        // FAST PATH: the new header fits the slot the old one occupied. Pad with
        // trailing spaces to the SAME header_len — valid JSON (exactly what
        // write_hbin_full's `reserve` branch already emits) — so header_len,
        // data_start and every payload byte are unchanged, and only the header
        // region is rewritten. No temp, no rename, nothing to tear.
        hjson.append(static_cast<size_t>(header_len) - hjson.size(), ' ');
        std::fstream f(to_native_path(path), std::ios::binary | std::ios::in | std::ios::out);
        if (!f) throw EngineError("data_io", "cannot rewrite .hbin header: " + path, path);
        f.seekp(16, std::ios::beg);
        f.write(hjson.data(), static_cast<std::streamsize>(hjson.size()));
        f.flush();
        if (!f) throw EngineError("data_io", "failed rewriting .hbin header: " + path, path);
        return res;
    }

    // REBUILD PATH: the header outgrew its slot. The data region is copied
    // VERBATIM as one block — every column's `offset` is relative to data_start,
    // so moving data_start moves them all together and no offset needs touching
    // (which is also why inter-column alignment padding survives untouched).
    //
    // Sibling temp + rename(2), never a write to `path` ([[file-overwrite-dest-source-law]],
    // BUG-138): a failure anywhere leaves the original exactly as it was.
    const int64_t new_data_start = align_up(16 + static_cast<int64_t>(hjson.size()));
    const std::filesystem::path destPath = to_native_path(path);
    const std::filesystem::path destParent = destPath.parent_path();
    const std::string destDir =
        destParent.empty() ? std::string(".") : from_native_path(destParent);
    const std::string destBase = from_native_path(destPath.filename());
    static std::atomic<unsigned long long> metaTmpSeq{0};
    const std::string tmp = destDir + "/." + destBase + ".metatmp" +
                            std::to_string(static_cast<long long>(native_process_id())) + "_" +
                            std::to_string(metaTmpSeq++);
    std::error_code ec;
    std::filesystem::remove(to_native_path(tmp), ec);
    {
        std::ofstream f(to_native_path(tmp), std::ios::binary | std::ios::trunc);
        if (!f) throw EngineError("data_io", "cannot create metadata temp for " + path, path);
        f.write(kMagic, 8);
        const uint64_t hlen = hjson.size();
        f.write(reinterpret_cast<const char*>(&hlen), 8);
        f.write(hjson.data(), static_cast<std::streamsize>(hjson.size()));
        const std::vector<char> pad(static_cast<size_t>(kAlign), 0);
        int64_t pos = 16 + static_cast<int64_t>(hlen);
        while (pos < new_data_start) {
            const int64_t n = std::min<int64_t>(kAlign, new_data_start - pos);
            f.write(pad.data(), static_cast<std::streamsize>(n));
            pos += n;
        }
        f.write(bytes.data() + data_start,
                static_cast<std::streamsize>(static_cast<int64_t>(bytes.size()) - data_start));
        f.flush();
        if (!f) {
            f.close();
            std::filesystem::remove(to_native_path(tmp), ec);
            throw EngineError("data_io",
                              "failed writing metadata temp for '" + path +
                                  "' (disk full or I/O error) — the original is unchanged",
                              path);
        }
    }
    std::filesystem::rename(to_native_path(tmp), destPath, ec);
    if (ec) {
        std::error_code rmEc;
        std::filesystem::remove(to_native_path(tmp), rmEc);
        throw EngineError("data_io",
                          "failed committing metadata rewrite of '" + path + "': " + ec.message(),
                          path);
    }
    res.rewrote = true;
    return res;
}

}  // namespace cecon
