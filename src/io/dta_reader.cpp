// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "io/dta_reader.h"
#include "core/eng_counters.h"  // WS-D2: physical dataset-file open counter
#include "io/win_compat.h"  // to_native_path: wide open on Windows for non-ASCII paths (BUG-050)

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>

#include "core/error.h"
#include "data/missing.h"  // Stata `.` / `.a`-`.z` kinds carried in the f64 plane

// Symmetric with dta_writer.cpp's guard: every scalar read below goes through the
// Cursor::*le() memcpy path, which is only a little-endian decode on a little-endian
// host. The u16be/u32be/u64be helpers exist but no caller reaches them (h.big_endian
// is recorded and never branched on), so an MSF file is ALREADY mis-decoded and a
// big-endian host would additionally mis-decode LSF. Both x86-64 and arm64 are LSF,
// so this changes nothing on any platform we build for — it is a compile-time trap so
// the assumption cannot be inherited silently.
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && \
    __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "dta_reader decodes the LSF byte order via memcpy; port Cursor's scalar reads before building big-endian"
#endif

namespace cecon {
namespace {

constexpr double NAd = std::numeric_limits<double>::quiet_NaN();

// ---------------------------------------------------------------------------
// Byte-level cursor over an in-memory buffer (whole file is read up front —
// .dta files in PIET's target use case are small/medium teaching datasets;
// mmap is not worth the complexity here since we need random seeks anyway).
struct Cursor {
    const uint8_t* base;
    size_t len;
    size_t pos = 0;

    explicit Cursor(const std::vector<uint8_t>& buf) : base(buf.data()), len(buf.size()) {}

    void need(size_t n) const {
        if (pos + n > len) throw EngineError("data_io", "dta: unexpected end of file");
    }
    void seek(size_t p) {
        if (p > len) throw EngineError("data_io", "dta: seek past end of file");
        pos = p;
    }
    void skip(size_t n) { need(n); pos += n; }

    uint8_t u8() { need(1); return base[pos++]; }
    int8_t i8() { return static_cast<int8_t>(u8()); }

    uint16_t u16le() { need(2); uint16_t v; std::memcpy(&v, base + pos, 2); pos += 2; return v; }
    int16_t i16le() { return static_cast<int16_t>(u16le()); }

    uint32_t u32le() { need(4); uint32_t v; std::memcpy(&v, base + pos, 4); pos += 4; return v; }
    int32_t i32le() { return static_cast<int32_t>(u32le()); }

    uint64_t u64le() { need(8); uint64_t v; std::memcpy(&v, base + pos, 8); pos += 8; return v; }
    int64_t i64le() { return static_cast<int64_t>(u64le()); }

    float f32le() { uint32_t v = u32le(); float f; std::memcpy(&f, &v, 4); return f; }
    double f64le() { uint64_t v = u64le(); double d; std::memcpy(&d, &v, 8); return d; }

    // Big-endian counterparts (Stata files may be MSF byteorder; rare on
    // modern exports but the format allows it — handled for correctness).
    uint16_t u16be() { need(2); return (static_cast<uint16_t>(base[pos]) << 8) | base[pos + 1]; }
    uint32_t u32be() {
        need(4);
        uint32_t v = (static_cast<uint32_t>(base[pos]) << 24) | (static_cast<uint32_t>(base[pos + 1]) << 16) |
                     (static_cast<uint32_t>(base[pos + 2]) << 8) | base[pos + 3];
        pos += 4;
        return v;
    }
    uint64_t u64be() {
        need(8);
        uint64_t v = 0;
        for (int k = 0; k < 8; ++k) v = (v << 8) | base[pos + k];
        pos += 8;
        return v;
    }

    std::string raw(size_t n) { need(n); std::string s(reinterpret_cast<const char*>(base + pos), n); pos += n; return s; }

    // Fixed-width field: read n bytes, trim at the first NUL (Stata C-string
    // padding convention for names/labels).
    std::string cstr_field(size_t n) {
        std::string s = raw(n);
        auto nul = s.find('\0');
        if (nul != std::string::npos) s.resize(nul);
        return s;
    }
};

// ---------------------------------------------------------------------------
// Stata storage-type descriptor, normalized across old (numeric code) and
// XML (uint16 code) typlists into one small enum + byte width.
enum class StKind { Str, Byte, Int16, Int32, Float32, Float64, StrL };

struct StType {
    StKind kind;
    int width = 0;       // string byte width (Str only)
    std::string label;   // "byte"/"int"/"long"/"float"/"double"/"strNN"/"strL"
};

StType decode_old_typecode(int raw_code, int format_version) {
    // Format <=110 stores non-numeric codes as Stata char-codes needing the
    // OLD_TYPE_MAPPING translation; format 111-114/115 stores 251-255 for
    // numeric and 1..244 directly for str#. We accept both encodings by
    // checking range, mirroring pandas' branch on format_version >= 111.
    if (format_version >= 111) {
        // typlist byte IS the code directly: 1..244 = strN, 251..255 numeric.
        if (raw_code >= 1 && raw_code <= 244) return {StKind::Str, raw_code, "str" + std::to_string(raw_code)};
        switch (raw_code) {
            case 251: return {StKind::Byte, 0, "byte"};
            case 252: return {StKind::Int16, 0, "int"};
            case 253: return {StKind::Int32, 0, "long"};
            case 254: return {StKind::Float32, 0, "float"};
            case 255: return {StKind::Float64, 0, "double"};
            default: break;
        }
        throw EngineError("data_io", "dta: unknown storage type code " + std::to_string(raw_code));
    }
    // format < 111: OLD_TYPE_MAPPING for 98/105/108/102/100, else (byte-127)=strN.
    switch (raw_code) {
        case 98: return {StKind::Byte, 0, "byte"};
        case 105: return {StKind::Int16, 0, "int"};
        case 108: return {StKind::Int32, 0, "long"};
        case 102: return {StKind::Float32, 0, "float"};
        case 100: return {StKind::Float64, 0, "double"};
        default: {
            int w = raw_code - 127;
            if (w >= 1 && w <= 244) return {StKind::Str, w, "str" + std::to_string(w)};
            throw EngineError("data_io", "dta: unknown legacy storage type code " + std::to_string(raw_code));
        }
    }
}

StType decode_xml_typecode(int typ) {
    if (typ <= 2045) return {StKind::Str, typ, "str" + std::to_string(typ)};
    switch (typ) {
        case 32768: return {StKind::StrL, 0, "strL"};
        case 65526: return {StKind::Float64, 0, "double"};
        case 65527: return {StKind::Float32, 0, "float"};
        case 65528: return {StKind::Int32, 0, "long"};
        case 65529: return {StKind::Int16, 0, "int"};
        case 65530: return {StKind::Byte, 0, "byte"};
        default: throw EngineError("data_io", "dta: unknown XML storage type code " + std::to_string(typ));
    }
}

// VALID_RANGE: any raw stored value outside [lo,hi] is Stata system-missing.
// (pandas VALID_RANGE table, format>111; OLD_VALID_RANGE differs by 1 at the
// edges for format<=111 — 115/117/118 all use the "new" table so we only need
// this one.)
//
// Exact float32/float64 missing sentinels (from pandas float32_max/float64_max
// hex patterns \xff\xff\xff\x7e / \xff\xff\xff\xff\xff\xff\xdf\x7f, i.e. "any
// value at or above the largest finite representable just below Stata's '.'
// sentinel range is missing"). We reproduce the exact bit thresholds instead
// of approximating.
float kFloat32Max() {
    uint32_t bits = 0xFF7FFFFEu;  // little-endian bytes ff ff ff 7e -> this const
    // pandas float32_max bytes (LE on disk) = ff ff ff 7e -> reinterpret as f32
    uint8_t b[4] = {0xff, 0xff, 0xff, 0x7e};
    float f; std::memcpy(&f, b, 4); (void)bits; return f;
}
float kFloat32Min() {
    uint8_t b[4] = {0xff, 0xff, 0xff, 0xfe};
    float f; std::memcpy(&f, b, 4); return f;
}
double kFloat64Max() {
    uint8_t b[8] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xdf, 0x7f};
    double d; std::memcpy(&d, b, 8); return d;
}
double kFloat64Min() {
    uint8_t b[8] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xef, 0xff};
    double d; std::memcpy(&d, b, 8); return d;
}

bool is_missing_raw(StKind k, double v) {
    switch (k) {
        case StKind::Byte:    return v < -127.0 || v > 100.0;
        case StKind::Int16:   return v < -32767.0 || v > 32740.0;
        case StKind::Int32:   return v < -2147483647.0 || v > 2147483620.0;
        case StKind::Float32: return v < kFloat32Min() || v > kFloat32Max();
        case StKind::Float64: return v < kFloat64Min() || v > kFloat64Max();
        default: return false;
    }
}

// Which of Stata's 27 missings a raw missing cell holds: 0 = `.`, 1..26 =
// `.a`..`.z`. The codes are consecutive immediately above each type's valid
// range — integers by value, floats by BIT PATTERN (a fixed stride above the
// type's `.` sentinel). Derived from the format-118 spec and confirmed on a
// real StataBE 19.5 file: the P1 fixture's `replace y = .a` cells (double)
// are 0x7FE0010000000000, i.e. the `.` base 0x7FE0000000000000 plus exactly
// one stride of 1<<40. Anything above `.z` (corrupt/foreign) reads as `.`.
int missing_code_raw(StKind k, double v) {
    auto clamp = [](double delta) {
        if (!(delta >= 1.0) || delta > static_cast<double>(cecon::kMissZ)) return cecon::kMissSys;
        return static_cast<int>(delta);
    };
    switch (k) {
        // Integers: `.` is the first code above the valid maximum.
        case StKind::Byte:    return v < 101.0 ? cecon::kMissSys : clamp(v - 101.0);
        case StKind::Int16:   return v < 32741.0 ? cecon::kMissSys : clamp(v - 32741.0);
        case StKind::Int32:   return v < 2147483621.0 ? cecon::kMissSys : clamp(v - 2147483621.0);
        // Floats: consecutive bit patterns, stride 0x800 (f32) / 1<<40 (f64).
        case StKind::Float32: {
            float f = static_cast<float>(v);
            uint32_t bits;
            std::memcpy(&bits, &f, sizeof bits);
            constexpr uint32_t kBase = 0x7F000000u;   // float `.`
            constexpr uint32_t kStride = 0x00000800u;
            if (bits < kBase) return cecon::kMissSys;
            uint32_t off = bits - kBase;
            if (off % kStride != 0) return cecon::kMissSys;
            return clamp(static_cast<double>(off / kStride));
        }
        case StKind::Float64: {
            uint64_t bits;
            std::memcpy(&bits, &v, sizeof bits);
            constexpr uint64_t kBase = 0x7FE0000000000000ull;   // double `.`
            constexpr uint64_t kStride = 0x0000010000000000ull;
            if (bits < kBase) return cecon::kMissSys;
            uint64_t off = bits - kBase;
            if (off % kStride != 0) return cecon::kMissSys;
            return clamp(static_cast<double>(off / kStride));
        }
        default: return cecon::kMissSys;
    }
}

// One decode step: a valid value passes through, a missing one becomes the
// f64 carrier for its kind (data/missing.h).
inline double decode_numeric(StKind k, double raw) {
    if (!is_missing_raw(k, raw)) return raw;
    return cecon::stata_missing(missing_code_raw(k, raw));
}

// ---------------------------------------------------------------------------
// Old-style (format 115, and 102-114 for completeness) header + descriptors.
struct ParsedVar {
    std::string name;
    StType type;
    std::string format;   // display format string, e.g. "%9.0g"
    std::string lblname;  // attached value-label name ("" if none)
    std::string varlabel;
};

struct Header {
    int format_version = 0;
    bool big_endian = false;
    int nvar = 0;
    int64_t nobs = 0;
    std::string data_label;
    std::string timestamp;
    // BUG-272 (closes BUG-200): the <sortlist>/srtlist claim — 1-based variable
    // indices, in sort order, zero-terminated on disk. Empty = no claim (all-zero
    // sortlist, which is what most files carry). Was parsed purely to advance the
    // cursor and discarded ("skip; unused"); now carried so DtaResult::frame can
    // state it. The .dta encoding has no direction bit — a sortlist claim is
    // ascending by definition, matching Frame's own claim semantics exactly.
    std::vector<int> sort_idx;
    std::vector<ParsedVar> vars;
    size_t data_location = 0;   // byte offset where the row-major data section starts
    // For 117/118 (XML sectioned): offsets needed to reach value labels / strLs.
    size_t seek_strls = 0;
    size_t seek_value_labels = 0;
    size_t seek_characteristics = 0;
    bool is_xml = false;
};

std::string read_u16_len_str(Cursor& c) {
    uint16_t n = c.u16le();
    return c.raw(n);
}

Header parse_old_header(Cursor& c, int format_version) {
    Header h;
    h.format_version = format_version;
    h.is_xml = false;
    // byte 0 already consumed by caller (the format-version byte itself).
    uint8_t byteorder_byte = c.u8();
    h.big_endian = (byteorder_byte == 0x1);
    c.u8();       // filetype
    c.skip(1);    // unused padding
    h.nvar = c.u16le();
    // nobs width depends on version; 115 uses uint32 (>=103).
    h.nobs = (format_version >= 103) ? static_cast<int64_t>(c.u32le())
                                      : static_cast<int64_t>(c.u16le());
    // data_label: 81 bytes for format>105 (115 qualifies), else 32.
    h.data_label = (format_version > 105) ? c.cstr_field(81) : c.cstr_field(32);
    // time_stamp: 18-byte NUL-padded C string, "dd Mon yyyy hh:mm" (17 chars + NUL),
    // format>104 only (BUG-199: was skip(18) — parsed as padding, never carried).
    if (format_version >= 105) h.timestamp = c.cstr_field(18);

    // descriptors: typlist
    std::vector<StType> types(h.nvar);
    for (int i = 0; i < h.nvar; ++i) {
        int raw = c.u8();
        types[i] = decode_old_typecode(raw, format_version);
    }
    // varlist: 33 bytes each (format>108; 115 qualifies)
    std::vector<std::string> names(h.nvar);
    for (int i = 0; i < h.nvar; ++i) names[i] = c.cstr_field(format_version > 108 ? 33 : 9);
    // srtlist: (nvar+1) int16 — BUG-272: capture the leading nonzero run (1-based
    // variable indices, zero-terminated); every entry is still consumed so the
    // cursor lands exactly where it always did. A malformed claim (out-of-range
    // index, or a nonzero entry after the terminator) keeps NOTHING — blank is
    // always safe — but still reads the whole field.
    {
        bool claim_ok = true, terminated = false;
        for (int i = 0; i < h.nvar + 1; ++i) {
            int v = c.i16le();
            if (v == 0) { terminated = true; continue; }
            if (terminated || v < 1 || v > h.nvar) { claim_ok = false; continue; }
            h.sort_idx.push_back(v);
        }
        if (!claim_ok) h.sort_idx.clear();
    }
    // fmtlist: 49 bytes each for format>113 (115 qualifies)
    int fmt_w = (format_version > 113) ? 49 : (format_version > 104 ? 12 : 7);
    std::vector<std::string> fmts(h.nvar);
    for (int i = 0; i < h.nvar; ++i) fmts[i] = c.cstr_field(fmt_w);
    // lbllist: 33 bytes each (format>108)
    std::vector<std::string> lbls(h.nvar);
    for (int i = 0; i < h.nvar; ++i) lbls[i] = c.cstr_field(format_version > 108 ? 33 : 9);
    // variable labels: 81 bytes each (105<format<118)
    std::vector<std::string> varlabs(h.nvar);
    for (int i = 0; i < h.nvar; ++i) varlabs[i] = c.cstr_field(81);

    // expansion fields: read (type:1, len:4) pairs (format>108) until type==0
    while (true) {
        uint8_t dtype = c.u8();
        uint32_t dlen = (format_version > 108) ? c.u32le() : static_cast<uint32_t>(c.u16le());
        if (dtype == 0) break;
        c.skip(dlen);
    }
    h.data_location = c.pos;

    h.vars.resize(h.nvar);
    for (int i = 0; i < h.nvar; ++i) {
        h.vars[i] = {names[i], types[i], fmts[i], lbls[i], varlabs[i]};
    }
    return h;
}

Header parse_xml_header(Cursor& c, int format_version) {
    Header h;
    h.format_version = format_version;
    h.is_xml = true;
    c.skip(21);  // </release><byteorder>
    std::string bo = c.raw(3);
    h.big_endian = (bo == "MSF");
    c.skip(15);  // </byteorder><K>
    h.nvar = (format_version <= 118) ? c.u16le() : static_cast<int>(c.u32le());
    c.skip(7);   // </K><N>
    h.nobs = (format_version >= 118) ? static_cast<int64_t>(c.u64le()) : static_cast<int64_t>(c.u32le());
    c.skip(11);  // </N><label>
    if (format_version >= 118) {
        h.data_label = read_u16_len_str(c);
    } else {
        uint8_t slen = c.u8();
        h.data_label = c.raw(slen);
    }
    c.skip(19);  // </label><timestamp>
    {
        // time_stamp (BUG-199): length-prefixed bytes, now carried into h.timestamp
        // instead of discarded — dta_writer.cpp already emits this field byte-correct
        // when DtaWriteOptions::timestamp is non-empty; the loss was purely on read +
        // CLI-wiring (apps/cmds/cmds_data.cpp), not the wire format.
        uint8_t slen = c.u8();
        h.timestamp = c.raw(slen);
    }
    c.skip(26);  // </timestamp></header><map>
    c.skip(8);   // 0x00...0 map-of-maps sentinel
    c.skip(8);   // position of <map> itself

    size_t seek_vartypes = static_cast<size_t>(c.i64le()) + 16;
    size_t seek_varnames = static_cast<size_t>(c.i64le()) + 10;
    size_t seek_sortlist = static_cast<size_t>(c.i64le()) + 10;
    size_t seek_formats = static_cast<size_t>(c.i64le()) + 9;
    size_t seek_value_label_names = static_cast<size_t>(c.i64le()) + 19;

    size_t seek_variable_labels;
    if (format_version == 117) {
        c.skip(8);  // <variable_labels>
        seek_variable_labels = seek_value_label_names + (33 * static_cast<size_t>(h.nvar)) + 20 + 17;
    } else {
        seek_variable_labels = static_cast<size_t>(c.i64le()) + 17;
    }

    // <characteristics> — was `c.skip(8)` (offset discarded, reader §5.2 gap: the
    // section existed on disk but was never reachable). Captured the same way as
    // every other map-of-maps entry: raw offset + strlen("<TAGNAME>") to land past
    // the opening tag (matches seek_varnames/+10, seek_formats/+9, etc. above).
    size_t seek_characteristics = static_cast<size_t>(c.i64le()) + 17;  // <characteristics>
    size_t data_location = static_cast<size_t>(c.i64le()) + 6;
    size_t seek_strls = static_cast<size_t>(c.i64le()) + 7;
    size_t seek_value_labels = static_cast<size_t>(c.i64le()) + 14;

    // --- vartypes ---
    std::vector<StType> types(h.nvar);
    c.seek(seek_vartypes);
    for (int i = 0; i < h.nvar; ++i) types[i] = decode_xml_typecode(c.u16le());

    // --- varnames ---
    std::vector<std::string> names(h.nvar);
    c.seek(seek_varnames);
    int name_w = (format_version < 118) ? 33 : 129;
    for (int i = 0; i < h.nvar; ++i) names[i] = c.cstr_field(name_w);

    // --- sortlist --- BUG-272: same capture as the old-format srtlist above
    // (u16 entries here, per the 117/118 spec; the writer emits u16 too).
    c.seek(seek_sortlist);
    {
        bool claim_ok = true, terminated = false;
        for (int i = 0; i < h.nvar + 1; ++i) {
            int v = static_cast<int>(c.u16le());
            if (v == 0) { terminated = true; continue; }
            if (terminated || v < 1 || v > h.nvar) { claim_ok = false; continue; }
            h.sort_idx.push_back(v);
        }
        if (!claim_ok) h.sort_idx.clear();
    }

    // --- formats ---
    std::vector<std::string> fmts(h.nvar);
    c.seek(seek_formats);
    int fmt_w = (format_version >= 118) ? 57 : 49;
    for (int i = 0; i < h.nvar; ++i) fmts[i] = c.cstr_field(fmt_w);

    // --- value-label names (lbllist) ---
    std::vector<std::string> lbls(h.nvar);
    c.seek(seek_value_label_names);
    int lbl_w = (format_version >= 118) ? 129 : 33;
    for (int i = 0; i < h.nvar; ++i) lbls[i] = c.cstr_field(lbl_w);

    // --- variable labels ---
    std::vector<std::string> varlabs(h.nvar);
    c.seek(seek_variable_labels);
    int vlbl_w = (format_version >= 118) ? 321 : 81;
    for (int i = 0; i < h.nvar; ++i) varlabs[i] = c.cstr_field(vlbl_w);

    h.data_location = data_location;
    h.seek_strls = seek_strls;
    h.seek_value_labels = seek_value_labels;
    h.seek_characteristics = seek_characteristics;
    h.vars.resize(h.nvar);
    for (int i = 0; i < h.nvar; ++i) h.vars[i] = {names[i], types[i], fmts[i], lbls[i], varlabs[i]};
    return h;
}

// ---------------------------------------------------------------------------
// strL (long string) GSO table (117/118 only) — needed because strL columns
// store an 8-byte (v,o) reference into this table rather than inline bytes.
std::map<std::string, std::string> read_strls(Cursor& c, size_t seek_strls, int format_version) {
    std::map<std::string, std::string> gso;
    gso["0"] = "";
    c.seek(seek_strls);
    while (c.pos + 3 <= c.len) {
        std::string tag = c.raw(3);
        if (tag != "GSO") { c.pos -= 3; break; }
        uint64_t v_o;
        if (format_version == 117) {
            v_o = c.u64le();
        } else {
            // 118/119: 12-byte packed (v:varies, o:varies) little-endian custom
            // packing per pandas _read_strls; v_size=2 for 118.
            std::string buf = c.raw(12);
            int v_size = (format_version == 118) ? 2 : 3;
            std::string packed = buf.substr(0, v_size) + buf.substr(4, 12 - v_size - 4);
            uint64_t val = 0;
            for (int k = static_cast<int>(packed.size()) - 1; k >= 0; --k)
                val = (val << 8) | static_cast<uint8_t>(packed[k]);
            v_o = val;
        }
        uint8_t typ = c.u8();
        uint32_t length = c.u32le();
        std::string data = c.raw(length);
        std::string decoded;
        if (typ == 130 && !data.empty()) decoded = data.substr(0, data.size() - 1);  // trailing NUL
        else decoded = data;
        gso[std::to_string(v_o)] = decoded;
    }
    return gso;
}

// ---------------------------------------------------------------------------
// <characteristics> table (117/118 only). Byte layout mirrors the writer exactly
// (dta_writer.h/.cpp, MEASURED not doc-derived): repeated `<ch>` blocks, each
// `<ch>` + u4 len + varname(W) + charname(W) + value bytes + NUL + `</ch>`,
// where len = 2W + value.size() + 1 (the two name fields + value + its
// terminating NUL). W is the release's name width, as in the variable-name
// table: 129 from 118 on, 33 in 117 (BUG-658: reading a 117 file's 33-byte
// fields as 129 failed every Stata 13 file that carries a char -- an xtset, a
// note -- with "malformed <ch> length"). The section ends at
// `</characteristics>` — distinguished from a `<ch>` block start by peeking
// the next 4 bytes ("<ch>" vs "</ch", the common prefix of "</characteristics>").
std::vector<DtaChar> read_characteristics(Cursor& c, const Header& h) {
    std::vector<DtaChar> out;
    if (!h.is_xml) return out;  // old (<=115) format predates <characteristics>
    const size_t name_w = (h.format_version >= 118) ? 129 : 33;
    c.seek(h.seek_characteristics);
    while (true) {
        if (c.pos + 4 > c.len) break;
        std::string peek = c.raw(4);
        if (peek != "<ch>") break;  // "</characteristics>" (or truncated file) reached
        uint32_t len = c.u32le();
        std::string varname = c.cstr_field(name_w);
        std::string charname = c.cstr_field(name_w);
        if (len < 2 * name_w + 1)
            throw EngineError("data_io", "dta: malformed <ch> length " + std::to_string(len));
        size_t value_len = static_cast<size_t>(len) - 2 * name_w - 1;
        std::string value = c.raw(value_len);
        c.skip(1);  // trailing NUL (counted in len, per the writer's layout)
        c.skip(5);  // "</ch>"
        out.push_back({std::move(varname), std::move(charname), std::move(value)});
    }
    return out;
}

// ---------------------------------------------------------------------------
// Value-label table (both old fixed-width 108-style and new variable-length
// 117+ style; 115 uses the "new value labels, fixed name width" variant per
// pandas: format>=108 always uses _read_new_value_labels).
std::vector<DtaValueLabel> read_value_labels(Cursor& c, const Header& h) {
    std::vector<DtaValueLabel> out;
    if (h.is_xml) {
        c.seek(h.seek_value_labels);
    } else {
        // old-style: value labels sit right after the data section.
        // (caller already knows data_location + row bytes; recomputed by caller
        // before calling this — see read_dta().)
    }
    int name_w = h.is_xml ? ((h.format_version >= 118) ? 129 : 33) : 33;
    while (true) {
        if (h.is_xml) {
            if (c.pos + 5 > c.len) break;
            std::string peek = c.raw(5);
            if (peek == "</val") break;  // "</value_labels>" closing tag reached (the
                                          // 5 bytes read were "</val", left consumed —
                                          // matches pandas, which does not rewind here)
            // Not the closing tag: those 5 bytes WERE the "<lbl>" opening tag for this
            // entry (also exactly 5 bytes) — already consumed, do not rewind. Matches
            // pandas._read_new_value_labels, which never seeks backward here.
        }
        if (c.pos + 4 > c.len) break;
        uint32_t slength = c.u32le();
        (void)slength;  // length-of-label-definition-blob (not needed; we parse
                         // n/txtlen directly, matching pandas which also ignores it
                         // beyond using it as an EOF probe).
        std::string labname = c.cstr_field(static_cast<size_t>(name_w));
        c.skip(3);  // padding
        uint32_t n = c.u32le();
        uint32_t txtlen = c.u32le();
        std::vector<int32_t> off(n), val(n);
        for (uint32_t i = 0; i < n; ++i) off[i] = c.i32le();
        for (uint32_t i = 0; i < n; ++i) val[i] = c.i32le();
        std::string txt = c.raw(txtlen);
        // off[] is not guaranteed sorted; pandas argsorts by offset before
        // slicing between consecutive offsets. Reproduce that.
        std::vector<uint32_t> order(n);
        for (uint32_t i = 0; i < n; ++i) order[i] = i;
        std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return off[a] < off[b]; });
        DtaValueLabel dvl;
        dvl.name = labname;
        for (uint32_t k = 0; k < n; ++k) {
            uint32_t i = order[k];
            uint32_t end = (k + 1 < n) ? static_cast<uint32_t>(off[order[k + 1]]) : txtlen;
            uint32_t start = static_cast<uint32_t>(off[i]);
            std::string s = (start <= end && end <= txt.size()) ? txt.substr(start, end - start) : std::string();
            auto nul = s.find('\0');
            if (nul != std::string::npos) s.resize(nul);
            dvl.map[val[i]] = s;
        }
        out.push_back(std::move(dvl));
        if (h.is_xml) c.skip(6);  // "</lbl>"
    }
    return out;
}

}  // namespace

DtaResult read_dta(const std::string& path) {
    cecon::engctr::addDataFileOpen();  // WS-D2: a .dta `using` file is a physical open too
    std::ifstream f(to_native_path(path), std::ios::binary);
    if (!f) throw EngineError("data_io", "dta: cannot open " + path);
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (buf.empty()) throw EngineError("data_io", "dta: empty file " + path);

    Cursor c(buf);

    Header h;
    // Detect XML (117+) vs old binary (<=115) container by sniffing the first
    // bytes: XML files start with the literal "<stata_dta>".
    static const char kXmlSig[] = "<stata_dta>";
    bool is_xml = buf.size() >= 11 && std::memcmp(buf.data(), kXmlSig, 11) == 0;
    if (is_xml) {
        c.skip(28);  // "<stata_dta><header><release>" (11+8+9 = 28 bytes)
        std::string ver_s = c.raw(3);
        int version = 0;
        try { version = std::stoi(ver_s); } catch (...) {
            throw EngineError("data_io", "dta: cannot parse release version");
        }
        if (version != 117 && version != 118) {
            throw EngineError("data_io", "dta: unsupported XML format version " + std::to_string(version) +
                                              " (supported: 117, 118)");
        }
        h = parse_xml_header(c, version);
    } else {
        int version = static_cast<int>(c.u8());
        // 114 (Stata 10/11 `saveold`) and 115 (Stata 12) share one binary container:
        // every version-dependent branch in parse_old_header/decode_old_typecode below
        // keys off `> 113` / `> 108` / `>= 111`, which 114 and 115 answer identically
        // (49-byte fmtlist, 33-byte varlist/lbllist, 81-byte variable labels, direct
        // 251..255 typecodes, u32 nobs). The parser body has always handled 114; only
        // this gate kept it out. Verified end-to-end on tests/validate/drdid2x2.dta
        // (a real format-114 file) — see tests/unit/test_dta114.cpp. (§35)
        if (version != 114 && version != 115) {
            throw EngineError("data_io", "dta: unsupported legacy format version " + std::to_string(version) +
                                              " (supported: 114, 115; use Stata to `saveold`/`save` into 114/115/117/118)");
        }
        h = parse_old_header(c, version);
    }

    const int nvar = h.nvar;
    const int64_t nobs = h.nobs;

    // --- strL GSO table (117/118 only) ---
    std::map<std::string, std::string> gso;
    if (h.is_xml) gso = read_strls(c, h.seek_strls, h.format_version);

    // --- row-major data section ---
    c.seek(h.data_location);
    // Precompute per-column decode plan.
    Frame out;
    out.set_n_rows_hint(nobs);  // see Frame helper below (adds a private setter)

    std::vector<DtaVarMeta> var_meta(nvar);
    std::vector<std::vector<double>> f64cols;   // for numeric columns (index by out col)
    std::vector<std::vector<std::string>> strcols;  // for string columns, raw text per row
    std::vector<int> col_kind(nvar);  // 0=numeric f64, 1=string->factor
    f64cols.resize(nvar);
    strcols.resize(nvar);

    for (int i = 0; i < nvar; ++i) {
        const auto& v = h.vars[i];
        col_kind[i] = (v.type.kind == StKind::Str || v.type.kind == StKind::StrL) ? 1 : 0;
        if (col_kind[i] == 0) f64cols[i].resize(static_cast<size_t>(nobs));
        else strcols[i].resize(static_cast<size_t>(nobs));
        var_meta[i] = {v.name, v.varlabel, v.format, v.type.label, v.lblname};
    }

    for (int64_t row = 0; row < nobs; ++row) {
        for (int i = 0; i < nvar; ++i) {
            const StType& t = h.vars[i].type;
            switch (t.kind) {
                case StKind::Str: {
                    std::string s = c.raw(static_cast<size_t>(t.width));
                    auto nul = s.find('\0');
                    if (nul != std::string::npos) s.resize(nul);
                    strcols[i][static_cast<size_t>(row)] = std::move(s);
                    break;
                }
                case StKind::StrL: {
                    // 8-byte (v,o) reference; decode per format so the key matches
                    // read_strls()'s GSO key exactly (BUG-072).
                    uint64_t key;
                    if (h.format_version == 117) {
                        // 117: (v:u4, o:u4). The GSO header is read as a plain
                        // little-endian u64 (:363-364), and the data ref has the
                        // identical byte layout, so read it the same way. The old
                        // code split it as u32+u16+u16 and dropped o's high 16 bits
                        // into `pad`, so any o >= 65536 (observation >= 65536 of a
                        // strL variable) silently missed the GSO and decoded to "".
                        key = c.u64le();
                    } else {
                        // 118/119: (v:u2, o:u6) ref. This u32+u16+u16 packing yields
                        // v | (low16(o)<<16) | (mid16(o)<<32), which equals the GSO
                        // key v|(o<<16) for all o < 2**32 (o's top 16 bits, ~4.3e9
                        // observations, are unreachable). Kept byte-identical to the
                        // pre-fix decode so existing strL goldens do not move.
                        uint32_t o = c.u32le();
                        uint16_t v16 = c.u16le();
                        uint16_t pad = c.u16le();
                        (void)pad;
                        key = (static_cast<uint64_t>(v16) << 32) | o;
                    }
                    auto it = gso.find(std::to_string(key));
                    strcols[i][static_cast<size_t>(row)] = (it != gso.end()) ? it->second : std::string();
                    break;
                }
                case StKind::Byte: {
                    double raw = static_cast<double>(c.i8());
                    f64cols[i][static_cast<size_t>(row)] = decode_numeric(StKind::Byte, raw);
                    break;
                }
                case StKind::Int16: {
                    double raw = static_cast<double>(c.i16le());
                    f64cols[i][static_cast<size_t>(row)] = decode_numeric(StKind::Int16, raw);
                    break;
                }
                case StKind::Int32: {
                    double raw = static_cast<double>(c.i32le());
                    f64cols[i][static_cast<size_t>(row)] = decode_numeric(StKind::Int32, raw);
                    break;
                }
                case StKind::Float32: {
                    float raw = c.f32le();
                    f64cols[i][static_cast<size_t>(row)] =
                        decode_numeric(StKind::Float32, static_cast<double>(raw));
                    break;
                }
                case StKind::Float64: {
                    double raw = c.f64le();
                    f64cols[i][static_cast<size_t>(row)] = decode_numeric(StKind::Float64, raw);
                    break;
                }
            }
        }
    }

    // --- value labels (both containers put the table after data for <117;
    //     117/118 use the seek_value_labels offset already captured). ---
    // Old-format (115) value labels sit immediately after the row data; since
    // we just read the rows sequentially, `c.pos` is ALREADY at the table
    // start — no seek needed (and seeking back to data_location would be
    // wrong: that's the start of the ROW data, not the label table).
    // XML (117/118) formats instead give an explicit seek_value_labels offset.
    std::vector<DtaValueLabel> value_labels = read_value_labels(c, h);
    std::vector<DtaChar> characteristics = read_characteristics(c, h);

    // --- assemble Frame: numeric -> f64 column, string -> factor column ---
    for (int i = 0; i < nvar; ++i) {
        if (col_kind[i] == 0) {
            out.append_f64_column(h.vars[i].name, std::move(f64cols[i]));
        } else {
            // A .dta string cell of `""` becomes factor code -1, the SAME convention
            // Frame::from_csv uses -- because in this engine `code -1` on a string
            // column IS Stata's `""`, not a second, foreign notion of missing.
            //
            // BUG-024 (2026-07-03) set this to `false` so `""` became its own level.
            // Its stated oracle was pandas.read_stata, and pandas is not the parity
            // oracle -- Stata is. Re-measured against live StataNow 19.5 on a fixture
            // with one blank string cell out of five (BUG-153 re-adjudication,
            // 2026-08-06); Stata answers all four probes as if the cell were missing
            // *and* equal to "":
            //     count if grp == ""    -> 1        count if missing(grp) -> 1
            //     tabulate grp          -> 2 rows, Total 4 (the blank is EXCLUDED)
            //     codebook grp          -> Unique values: 2   Missing "": 1/5
            // With `false` we answered the first two correctly (PIET renders a -1
            // string code as "") but got tabulate Total 5 with a blank row, and
            // codebook Unique 3 / Missing 0 -- two silently wrong aggregates, and a
            // `.dta` round trip therefore did not preserve them. With `true` all four
            // match the oracle exactly. `""` remains real, comparable data on the way
            // OUT too: dta_writer already writes code < 0 back as `""` (see its
            // :331 note), so the round trip is lossless.
            out.append_factor_column(h.vars[i].name, strcols[i], /*empty_is_missing=*/true);
        }
    }
    out.finalize_after_bulk_load();

    // --- P1-A0-FORMAT-CARRIAGE (2026-07-30): attach each variable's Stata display
    //     format to its column, so it survives into the .hbin header (and from there
    //     to any reader) instead of being consumed by the rescale below and dropped.
    //     Carriage ONLY — the string is not interpreted here beyond what the existing
    //     rescale already does, and nothing downstream in the engine branches on it.
    //     Every variable that HAS a format gets it, date or not: `%9.0g` is exactly as
    //     much a display instruction as `%td`, and PIET's grid needs both. ---
    for (int i = 0; i < nvar; ++i) {
        const std::string& fmt = h.vars[i].format;
        if (!fmt.empty()) out.mutable_col(h.vars[i].name).format = fmt;
    }

    // --- date/time format rescale to epoch-days f64 (matches
    //     tools/to_hbin.py's datetime64->epoch-days convention: PIET's oracle
    //     converts %td (and related %t* date/time formats) via pandas'
    //     convert_dates default-on behavior, then to_hbin re-expresses any
    //     datetime64 column as (ts - 1960-01-01).total_seconds()/86400). ---
    for (int i = 0; i < nvar; ++i) {
        if (col_kind[i] != 0) continue;
        const std::string& fmt = h.vars[i].format;
        if (fmt.size() < 2 || fmt[0] != '%' || fmt[1] != 't') continue;  // only %t* date/time formats
        char code = fmt.size() >= 3 ? fmt[2] : '\0';
        double scale_to_days = 1.0;  // %td is already whole days since 1960-01-01
        bool handled = true;
        switch (code) {
            case 'd': scale_to_days = 1.0; break;               // %td: days
            // %tc/%tC keep Stata's milliseconds, as %tw/%tm/%tq/%th keep their
            // index (BUG-1091): every reader of the column -- clock() in
            // generate, the %tc display, dofc() -- works in ms, so the old
            // days rescale made a StataBE file's `display %tc x` print
            // 01jan1960 00:00:22 and `summarize` report days (AER 007's
            // Wave3_combinedrawdata.dta EndDate2).
            case 'c': scale_to_days = 1.0; break;  // %tc: ms since 01jan1960
            case 'C': scale_to_days = 1.0; break;  // %tC: ms (leap-adjusted; treated same)
            case 'w':
                // %tw is Stata's fixed-52-week-year calendar (52 weeks/year;
                // weeks 1-51 are 7 days, week 52 absorbs the 8-9 day remainder),
                // NOT a linear 7-day multiple of the 1960 epoch — the old
                // `scale_to_days = 7.0` silently mis-scaled it (C1-F16/BUG-073),
                // and P1-A0 replaced that with an honest rc=3 reject while the
                // calendar decision was open.
                // P1-A1-TW-CALENDAR (2026-07-30) retires BOTH: per the adopted
                // Q1=(b) ruling (DESIGN-dates-fmt.md §4.1/§7 Q1,
                // P1-W1-REREVIEW-20260729.md §2.3) cobra STORES the raw Stata
                // %tw family-index — value-identical to Stata's own .dta bytes
                // — so the correct decode here is NO rescale at all, exactly as
                // for %tm/%tq/%th below. The 52-week calendar arithmetic
                // (dofw/wofd, §3.2) belongs to the DISPLAY/pivot layer that
                // renders the index (§4.3 %fmt engine) and to the date-function
                // library (§4.4), where it has a consumer; putting it here would
                // convert the column to days = the rejected option (a), a second
                // REPIN that diverges from Stata storage. The %tw format string
                // itself survives on the column (P1-A0-FORMAT-CARRIAGE above),
                // which is what lets that renderer do its job later.
                // Pinned by eval C1F16_tw_value (raw index {3120,3121,3122,3123}
                // on golden/f16_tw_fixture.dta); the C1F16_tw_misscale_reject
                // pin is retired in the same commit — do NOT re-add a scale
                // factor for %tw, and do NOT re-introduce the reject.
                handled = false;
                break;
            default: handled = false; break;  // %tm/%tq/%th are month/quarter/half — not linear in days;
                                               // left as raw Stata units (matches to_hbin.py, which only
                                               // special-cases true datetime64 columns from pandas' date
                                               // conversion, not the calendar-unit formats).
        }
        if (!handled) continue;
        FrameColumn& fc = out.mutable_col(h.vars[i].name);
        for (double& v : fc.f64) if (!std::isnan(v)) v *= scale_to_days;
    }

    // --- BUG-272: install the source file's sort claim (closes BUG-200's read
    //     half). LAST, deliberately: the mutable_col() passes above (format attach,
    //     %t* rescale) clear any claim keyed on the column they touch, so a claim
    //     installed here cannot be invalidated by the reader's own post-processing.
    //     (The %tc rescale is a strictly positive scale — order-preserving — but the
    //     claim does not need that argument once it is installed afterwards.)
    //     set_sorted_by validates every index-resolved name against the Frame and
    //     installs nothing on any mismatch, so a malformed sortlist stays blank.
    if (!h.sort_idx.empty()) {
        std::vector<std::string> keys;
        keys.reserve(h.sort_idx.size());
        for (int idx : h.sort_idx) keys.push_back(h.vars[static_cast<size_t>(idx - 1)].name);
        out.set_sorted_by(std::move(keys));
    }

    DtaResult result;
    result.frame = std::move(out);
    result.format_version = h.format_version;
    result.data_label = h.data_label;
    result.timestamp = h.timestamp;
    result.var_meta = std::move(var_meta);
    result.value_labels = std::move(value_labels);
    result.characteristics = std::move(characteristics);
    return result;
}

}  // namespace cecon
