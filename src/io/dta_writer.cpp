// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "io/dta_writer.h"
#include "io/win_compat.h"  // to_native_path: wide open on Windows for non-ASCII paths (BUG-050)

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#include "core/error.h"
#include "data/missing.h"  // Stata `.` / `.a`-`.z` kinds carried in the f64 plane

#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && \
    __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "dta_writer emits the LSF byte order via memcpy; port the scalar writes before building big-endian"
#endif

namespace cecon {
namespace {

// ---------------------------------------------------------------------------
// The five numeric storage types this arc writes. `code` is the fmt-118
// <variable_types> u2 code — the exact values dta_reader.cpp's
// decode_xml_typecode() maps back, so the two tables are one table read in two
// directions.
enum class WKind { Byte, Int16, Int32, Float32, Float64 };

struct WType {
    WKind kind;
    uint16_t code;
    int width;              // on-disk bytes per cell
    const char* label;      // matches DtaVarMeta.stata_type spelling
    const char* def_format; // Stata's own default display format for the type
};

constexpr WType kByte  {WKind::Byte,    65530, 1, "byte",   "%8.0g"};
constexpr WType kInt   {WKind::Int16,   65529, 2, "int",    "%8.0g"};
constexpr WType kLong  {WKind::Int32,   65528, 4, "long",   "%12.0g"};
constexpr WType kFloat {WKind::Float32, 65527, 4, "float",  "%9.0g"};
constexpr WType kDouble{WKind::Float64, 65526, 8, "double", "%10.0g"};

// Valid-value ranges — the EXACT complement of dta_reader.cpp's is_missing_raw().
// A value outside its type's range is what Stata calls missing, so storing one as a
// plain value would read back as NaN: a silent value change. The narrowing scan
// below therefore treats these as hard capacity limits, not as advisory.
constexpr double kByteLo = -127.0,       kByteHi = 100.0;
constexpr double kIntLo  = -32767.0,     kIntHi  = 32740.0;
constexpr double kLongLo = -2147483647.0, kLongHi = 2147483620.0;

float f32_hi() { uint8_t b[4] = {0xff, 0xff, 0xff, 0x7e}; float f; std::memcpy(&f, b, 4); return f; }
float f32_lo() { uint8_t b[4] = {0xff, 0xff, 0xff, 0xfe}; float f; std::memcpy(&f, b, 4); return f; }
double f64_hi() { uint8_t b[8] = {0xff,0xff,0xff,0xff,0xff,0xff,0xdf,0x7f}; double d; std::memcpy(&d,b,8); return d; }
double f64_lo() { uint8_t b[8] = {0xff,0xff,0xff,0xff,0xff,0xff,0xef,0xff}; double d; std::memcpy(&d,b,8); return d; }

// System-missing `.` per storage type — the BASE of each type's missing run.
// Stata packs all 27 missing values (`.`, `.a`…`.z`) into consecutive codes
// starting here: integers by VALUE (base + kind), floats by BIT PATTERN
// (base + kind*stride). These are the exact mirror of dta_reader.cpp's
// missing_code_raw(), which is this repo's oracle for the encoding — it was
// itself confirmed against a real StataBE 19.5 file (the P1 fixture's `.a`
// doubles are 0x7FE0010000000000, one 1<<40 stride above `.`).
// The float bases are the documented 2^127 / 2^1023 patterns, both strictly
// above the respective f32_hi()/f64_hi() thresholds, so the reader classifies
// the whole run as missing.
constexpr int8_t  kMissByte = 101;
constexpr int16_t kMissInt  = 32741;
constexpr int32_t kMissLong = 2147483621;
constexpr uint32_t kMissF32Bits = 0x7F000000u;
constexpr uint64_t kMissF64Bits = 0x7FE0000000000000ull;
constexpr uint32_t kMissF32Stride = 0x00000800u;
constexpr uint64_t kMissF64Stride = 0x0000010000000000ull;

// ---------------------------------------------------------------------------
// Append-only byte sink. The whole file is built in memory and written once: the
// <map> section holds 14 absolute file offsets that are only knowable after the
// sections are laid out, so the alternative is a seek-back-and-patch over a live
// fd. Buffering also makes `--data X --out X` harmless (the source is fully read
// before a single output byte exists) — the dest==source class,
// [[file-overwrite-dest-source-law]].
struct Out {
    std::vector<uint8_t> b;

    void raw(const void* p, std::size_t n) {
        const uint8_t* q = static_cast<const uint8_t*>(p);
        b.insert(b.end(), q, q + n);
    }
    void lit(const char* s) { raw(s, std::strlen(s)); }
    void bytes(const std::string& s) { raw(s.data(), s.size()); }
    void u8v(uint8_t v) { b.push_back(v); }
    void i8v(int8_t v) { raw(&v, 1); }
    void u16(uint16_t v) { raw(&v, 2); }
    void i16(int16_t v) { raw(&v, 2); }
    void i32(int32_t v) { raw(&v, 4); }
    void u32(uint32_t v) { raw(&v, 4); }
    void u64(uint64_t v) { raw(&v, 8); }
    void f32bits(uint32_t v) { raw(&v, 4); }
    void f64bits(uint64_t v) { raw(&v, 8); }
    void f32v(float v) { uint32_t u; std::memcpy(&u, &v, 4); raw(&u, 4); }
    void f64v(double v) { uint64_t u; std::memcpy(&u, &v, 8); raw(&u, 8); }

    // Fixed-width NUL-padded field (Stata's name/format/label convention; the
    // reader trims at the first NUL, dta_reader.cpp Cursor::cstr_field).
    void field(const std::string& s, std::size_t n, const char* what, const std::string& who) {
        if (s.size() >= n) {
            throw EngineError("data_io", "dta: " + std::string(what) + " for '" + who + "' is " +
                                             std::to_string(s.size()) + " bytes, exceeds the " +
                                             std::to_string(n - 1) + "-byte .dta field");
        }
        raw(s.data(), s.size());
        b.insert(b.end(), n - s.size(), 0);
    }

    // A str# DATA cell — deliberately NOT field(): a str18 holding exactly 18 bytes
    // carries no terminator in Stata's own files, so the `<` here must be `<=` where
    // field()'s is `<`. Over-length is impossible (the width is the dictionary max,
    // checked once in plan_factor), so this is an assertion, not a policy.
    void strcell(const std::string& s, std::size_t n) {
        const std::size_t k = s.size() <= n ? s.size() : n;
        raw(s.data(), k);
        b.insert(b.end(), n - k, 0);
    }
    std::size_t pos() const { return b.size(); }
    void patch_u64(std::size_t at, uint64_t v) { std::memcpy(b.data() + at, &v, 8); }
};

// ---------------------------------------------------------------------------
// Per-column write plan: the storage type, the format string that will actually be
// emitted, and whether the values need the %tc inverse. Values are NOT copied — the
// row-major emit pass reads them straight out of the (possibly mmap-borrowed) column
// and transforms in flight, so exporting a frame never costs a second copy of it.
struct Plan {
    const FrameColumn* col = nullptr;
    WType t = kDouble;
    std::string name;
    std::string format;
    bool tc_inverse = false;  // %tc/%tC: epoch-days -> ms since 1960-01-01
    // str# columns (factors). `t` is meaningless when is_str — the type code IS the
    // width for every str# (1..2045; dta_reader.cpp:129 decodes it the same way), so
    // the two tables stay one table read in two directions.
    bool is_str = false;
    bool is_strl = false;
    uint32_t var_index = 0;
    int str_width = 0;
    // Missing string cells written as "" (Stata's only spelling for "nothing here").
    // Tallied rather than rejected so write_dta can name the cost once per column;
    // see the FACTOR ASYMMETRY note in dta_writer.h.
    int64_t str_missing_count = 0;
    int64_t str_missing_first_row = 0;  // 1-based; valid when str_missing_count > 0
    std::string vlabel;  // <variable_labels>
    std::string vlname;  // <value_label_names>

    uint16_t typecode() const { return is_strl ? 32768 : is_str ? static_cast<uint16_t>(str_width) : t.code; }
    int cell_width() const { return is_strl ? 8 : is_str ? str_width : t.width; }
};

// Above the fixed-width cap, use a real strL reference and GSO payload.
constexpr std::size_t kStrMax = 2045;

// %tc / %tC were the one date rescale (ms <-> epoch-days). BUG-1091 retired it: the
// reader keeps milliseconds like every other %t* index, so tc_inverse is always
// false and every value is written as is -- fractional ms included, which StataBE
// stores too (import excel's 1872914339399.999756).
inline double emit_value(const Plan& p, double v) {
    if (std::isnan(v)) return v;
    if (!p.tc_inverse) return v;
    return std::round(v * 86400000.0);
}

bool starts_with(const std::string& s, const char* pre) {
    const std::size_t n = std::strlen(pre);
    return s.size() >= n && std::memcmp(s.data(), pre, n) == 0;
}

// What the column's values look like, measured once (spec §3.3: "scan once ... record
// the chosen type so §3.5 does not re-decide").
struct Scan {
    bool any_finite = false;
    bool all_integral = true;
    bool f32_exact = true;   // every finite value survives (double)(float)v == v
    double lo = 0.0, hi = 0.0;
};

Scan scan_column(const Plan& p, int64_t nrows) {
    Scan s;
    const F64Col& f = p.col->f64;
    const double f32lo = static_cast<double>(f32_lo()), f32hi = static_cast<double>(f32_hi());
    for (int64_t i = 0; i < nrows; ++i) {
        const double raw = (static_cast<std::size_t>(i) < f.size())
                               ? f[static_cast<std::size_t>(i)]
                               : std::numeric_limits<double>::quiet_NaN();
        const double v = emit_value(p, raw);
        if (std::isnan(v)) continue;
        if (std::isinf(v)) {
            // Stata has no infinity: it would have to be stored as `.` and would read
            // back NaN — a silent value change. Loud instead (spec §2 principle 2).
            throw EngineError("data_io", "dta: column '" + p.name +
                                             "' holds a non-finite value; Stata's .dta has no "
                                             "infinity and storing it would silently become "
                                             "missing (export the column as missing explicitly)");
        }
        if (v < f64_lo() || v > f64_hi()) {
            throw EngineError("data_io", "dta: column '" + p.name +
                                             "' holds a value outside Stata's double range; it "
                                             "would read back as missing");
        }
        if (!s.any_finite) { s.lo = s.hi = v; s.any_finite = true; }
        else { if (v < s.lo) s.lo = v; if (v > s.hi) s.hi = v; }
        if (v != std::floor(v)) s.all_integral = false;
        if (s.f32_exact) {
            if (v < f32lo || v > f32hi || static_cast<double>(static_cast<float>(v)) != v)
                s.f32_exact = false;
        }
    }
    return s;
}

// Can `t` hold every value of this column without changing one? (The same
// predicate for the auto-narrowing ladder and for vetting a declared --in-meta type;
// one predicate, so a hint can never be honoured on terms the ladder would refuse.)
bool fits(const WType& t, const Scan& s) {
    if (!s.any_finite) return true;  // all-missing: every type stores `.` losslessly
    switch (t.kind) {
        case WKind::Byte:    return s.all_integral && s.lo >= kByteLo && s.hi <= kByteHi;
        case WKind::Int16:   return s.all_integral && s.lo >= kIntLo && s.hi <= kIntHi;
        case WKind::Int32:   return s.all_integral && s.lo >= kLongLo && s.hi <= kLongHi;
        case WKind::Float32: return s.f32_exact;
        case WKind::Float64: return true;  // guarded in scan_column (inf / out-of-range)
    }
    return false;
}

// Spec §3.3's ladder: narrowest lossless integer type, else float under the f32
// round-trip guard, else double.
WType narrow(const Scan& s) {
    if (fits(kByte, s)) return kByte;
    if (fits(kInt, s)) return kInt;
    if (fits(kLong, s)) return kLong;
    if (fits(kFloat, s)) return kFloat;
    return kDouble;
}

const WType* type_by_label(const std::string& l) {
    if (l == "byte") return &kByte;
    if (l == "int") return &kInt;
    if (l == "long") return &kLong;
    if (l == "float") return &kFloat;
    if (l == "double") return &kDouble;
    return nullptr;  // strNN / strL / unknown: no numeric meaning, fall back to the scan
}

// A declared `strNN` width, or 0 for anything else (strL is handled separately).
// Fixed-width declarations widen the measured str#; the values still
// round-trip exactly, only the declared TYPE does not, which is Q4's cost, not a new
// silent narrowing).
int str_width_by_label(const std::string& l) {
    if (l.size() < 4 || l.compare(0, 3, "str") != 0) return 0;
    int w = 0;
    for (std::size_t i = 3; i < l.size(); ++i) {
        if (l[i] < '0' || l[i] > '9') return 0;
        w = w * 10 + (l[i] - '0');
        if (w > static_cast<int>(kStrMax)) return 0;
    }
    return w;
}

// str# width = the max UTF-8 BYTE length over the WHOLE dictionary (spec §3.3), not
// over the used levels: the width must hold every value the column could present, and
// scanning the dictionary is O(levels) instead of O(rows).
//
// The floor of 1 is not cosmetic. A column whose only level is "" measures 0 bytes, and
// a typecode of 0 is not a str0 — dta_reader.cpp:129 would accept it and then read
// ZERO bytes per cell, silently shifting every following column in the row. Stata has
// no str0 either; str1 holding a zero byte is how an all-empty string column is stored.
void plan_factor(Plan& p, int64_t nrows, bool strict) {
    const LevelCol& lv = p.col->levels;
    std::size_t w = 0;
    for (std::size_t k = 0; k < lv.size(); ++k) {
        if (lv[k].size() >= std::numeric_limits<uint32_t>::max())
            throw EngineError("data_io", "dta: strL value exceeds the GSO length limit");
        if (lv[k].size() > kStrMax || lv[k].find('\0') != std::string::npos)
            p.is_strl = true;
        if (lv[k].size() > w) w = lv[k].size();
    }
    p.is_str = true;
    p.str_width = static_cast<int>(std::min(w > 0 ? w : 1, kStrMax));

    // Count genuinely missing string cells. Stata spells "nothing here" in a string
    // variable exactly one way — `""` — so that is what we emit, matching what Stata
    // itself would store. strict_string_missing restores the old reject for callers
    // that would rather fail than let the Frame's missing/"" distinction collapse.
    // See the FACTOR ASYMMETRY note in dta_writer.h.
    const CodeCol& cd = p.col->codes;
    const std::size_t nl = lv.size();
    for (int64_t i = 0; i < nrows; ++i) {
        const int32_t code =
            (static_cast<std::size_t>(i) < cd.size()) ? cd[static_cast<std::size_t>(i)] : -1;
        if (code < 0 || static_cast<std::size_t>(code) >= nl) {
            if (strict) {
                throw EngineError("data_io",
                                  "dta: string column '" + p.name +
                                      "' has a missing value at row " + std::to_string(i + 1) +
                                      "; Stata's .dta has no string missing distinct from the "
                                      "empty string, and --strict-string-missing was requested "
                                      "(recode it to \"\" explicitly, or drop the flag to write "
                                      "\"\" the way Stata does)");
            }
            if (p.str_missing_count == 0) p.str_missing_first_row = i + 1;
            ++p.str_missing_count;
        }
    }
}

void emit_str_cell(Out& o, const Plan& p, int64_t i) {
    const LevelCol& lv = p.col->levels;
    const CodeCol& cd = p.col->codes;
    const int32_t code =
        (static_cast<std::size_t>(i) < cd.size()) ? cd[static_cast<std::size_t>(i)] : -1;
    // code < 0 is a missing cell: Stata stores that as "" in a string variable, which
    // strcell writes as the all-zero fixed-width field. Counted in plan_factor.
    static const std::string kEmpty;
    const bool present = code >= 0 && static_cast<std::size_t>(code) < lv.size();
    o.strcell(present ? lv[static_cast<std::size_t>(code)] : kEmpty,
              static_cast<std::size_t>(p.str_width));
}

// A missing cell carries its KIND (`.`=0, `.a`..`.z`=1..26) into the file, so a
// `.a` read in survives a write back out. The kind rides the f64 plane as a
// quiet-NaN payload (data/missing.h); missing_kind() returns -1 for a real
// value, and any NaN this codebase did not produce decodes as plain `.`, so an
// outside NaN can never invent an extended code here either.
void emit_cell(Out& o, const WType& t, double v) {
    const int kind = cecon::missing_kind(v);   // -1 = not missing; 0..26 = `.`..`.z`
    const bool miss = kind >= 0;
    const auto k8 = static_cast<int8_t>(miss ? kind : 0);
    const auto k16 = static_cast<int16_t>(miss ? kind : 0);
    const auto k32 = static_cast<int32_t>(miss ? kind : 0);
    switch (t.kind) {
        case WKind::Byte:    o.i8v(miss ? static_cast<int8_t>(kMissByte + k8) : static_cast<int8_t>(v)); break;
        case WKind::Int16:   o.i16(miss ? static_cast<int16_t>(kMissInt + k16) : static_cast<int16_t>(v)); break;
        case WKind::Int32:   o.i32(miss ? kMissLong + k32 : static_cast<int32_t>(v)); break;
        case WKind::Float32:
            if (miss) o.f32bits(kMissF32Bits + kMissF32Stride * static_cast<uint32_t>(kind));
            else o.f32v(static_cast<float>(v));
            break;
        case WKind::Float64:
            if (miss) o.f64bits(kMissF64Bits + kMissF64Stride * static_cast<uint64_t>(kind));
            else o.f64v(v);
            break;
    }
}

// Format 115 (Stata 12; `saveold, version(11|12)`): the pre-XML binary layout.
// Header 109 bytes -- ds_format 115, byte order 2 (LOHI), filetype 1, a pad byte,
// nvar u16, nobs u32, data_label[81], time_stamp[18]; then typlist (one byte
// per variable: str1..str244 = the width, byte/int/long/float/double =
// 251/252/253/254/255), varlist[33], srtlist (u16 x nvar+1), fmtlist[49],
// lbllist[33], variable labels[81], the expansion fields (type 1, u32 length,
// varname[33], charname[33], value NUL; ended by type 0 length 0), the data,
// and the value labels (u32 table length, name[33], 3 pad bytes, table). The
// cell encodings, missing codes included, are 117's.
void write_dta_115(const Frame& fr, const std::string& path, const DtaWriteOptions& wo,
                   const std::vector<Plan>& plans, int64_t nrows, std::size_t kName,
                   std::size_t kFmt, std::size_t kVlab) {
    const int ncols = static_cast<int>(plans.size());
    Out o;
    o.u8v(115);
    o.u8v(2);  // LOHI
    o.u8v(1);
    o.u8v(0);
    o.u16(static_cast<uint16_t>(ncols));
    o.u32(static_cast<uint32_t>(nrows));
    if (wo.data_label.size() > 80)
        throw EngineError("data_io", "dta: dataset label exceeds 80 bytes (format 115)");
    o.field(wo.data_label, 81, "dataset label", "_dta");
    if (wo.timestamp.size() > 17)
        throw EngineError("data_io", "dta: timestamp exceeds 17 bytes (format 115)");
    o.field(wo.timestamp, 18, "timestamp", "_dta");
    for (const auto& p : plans) {
        if (p.is_str) {
            o.u8v(static_cast<uint8_t>(p.str_width));
            continue;
        }
        uint8_t code = 255;
        switch (p.t.kind) {
            case WKind::Byte: code = 251; break;
            case WKind::Int16: code = 252; break;
            case WKind::Int32: code = 253; break;
            case WKind::Float32: code = 254; break;
            case WKind::Float64: code = 255; break;
        }
        o.u8v(code);
    }
    for (const auto& p : plans) o.field(p.name, kName, "variable name", p.name);
    {
        std::vector<uint16_t> sl;
        bool claim_ok = !fr.sorted_by().empty();
        for (const std::string& k : fr.sorted_by()) {
            int pos = -1;
            for (int i = 0; i < ncols; ++i)
                if (plans[static_cast<size_t>(i)].name == k) { pos = i; break; }
            if (pos < 0) { claim_ok = false; break; }
            sl.push_back(static_cast<uint16_t>(pos + 1));
        }
        int emitted = 0;
        if (claim_ok)
            for (uint16_t v : sl) { o.u16(v); ++emitted; }
        for (int i = emitted; i < ncols + 1; ++i) o.u16(0);
    }
    for (const auto& p : plans) o.field(p.format, kFmt, "display format", p.name);
    for (const auto& p : plans) o.field(p.vlname, kName, "value-label name", p.name);
    for (const auto& p : plans) o.field(p.vlabel, kVlab, "variable label", p.name);
    for (const auto& ch : wo.characteristics) {
        const uint64_t len = 2ull * kName + static_cast<uint64_t>(ch.value.size()) + 1ull;
        if (len > 0x7FFFFFFFull)
            throw EngineError("data_io", "dta: characteristic '" + ch.charname + "' is too large");
        o.u8v(1);
        o.u32(static_cast<uint32_t>(len));
        o.field(ch.varname, kName, "characteristic variable name", ch.charname);
        o.field(ch.charname, kName, "characteristic name", ch.varname);
        o.bytes(ch.value);
        o.u8v(0);
    }
    o.u8v(0);
    o.u32(0);
    for (int64_t i = 0; i < nrows; ++i) {
        for (const auto& p : plans) {
            if (p.is_str) {
                emit_str_cell(o, p, i);
                continue;
            }
            const F64Col& f = p.col->f64;
            const double raw = (static_cast<std::size_t>(i) < f.size())
                                   ? f[static_cast<std::size_t>(i)]
                                   : std::numeric_limits<double>::quiet_NaN();
            emit_cell(o, p.t, emit_value(p, raw));
        }
    }
    for (const auto& vl : wo.value_labels) {
        const uint32_t n = static_cast<uint32_t>(vl.map.size());
        uint64_t txtlen = 0;
        for (const auto& kv : vl.map) txtlen += kv.second.size() + 1;
        if (txtlen > 0x7FFFFFFFull)
            throw EngineError("data_io", "dta: value label '" + vl.name + "' text is too large");
        o.u32(static_cast<uint32_t>(8ull + 8ull * n + txtlen));
        o.field(vl.name, kName, "value-label set name", vl.name);
        o.u8v(0); o.u8v(0); o.u8v(0);
        o.u32(n);
        o.u32(static_cast<uint32_t>(txtlen));
        uint32_t off = 0;
        for (const auto& kv : vl.map) {
            o.u32(off);
            off += static_cast<uint32_t>(kv.second.size() + 1);
        }
        for (const auto& kv : vl.map) {
            if (kv.first < -2147483648LL || kv.first > 2147483647LL)
                throw EngineError("data_io", "dta: value label '" + vl.name + "' has code " +
                                                 std::to_string(kv.first) +
                                                 " outside Stata's 4-byte label code range");
            o.i32(static_cast<int32_t>(kv.first));
        }
        for (const auto& kv : vl.map) { o.bytes(kv.second); o.u8v(0); }
    }
    std::ofstream f(to_native_path(path), std::ios::binary | std::ios::trunc);
    if (!f) throw EngineError("data_io", "dta: cannot open " + path + " for writing");
    f.write(reinterpret_cast<const char*>(o.b.data()), static_cast<std::streamsize>(o.b.size()));
    f.flush();
    if (!f) throw EngineError("data_io", "dta: write failed for " + path);
}

}  // namespace

void write_dta(const Frame& fr, const std::string& path, const DtaWriteOptions& wo) {
    const int ncols = fr.n_cols();
    const int64_t nrows = fr.n_rows();
    const int release = wo.release;
    if (release != 118 && release != 117 && release != 115)
        throw EngineError("bad_input", "dta: release " + std::to_string(release) +
                                           " is not written (115, 117 or 118)");
    if (ncols > 65535)
        throw EngineError("data_io", "dta: format " + std::to_string(release) +
                                         " holds at most 65535 variables, got " +
                                         std::to_string(ncols));
    if (release != 118 && nrows > 2147483647LL)
        throw EngineError("data_io", "dta: format " + std::to_string(release) +
                                         " holds at most 2147483647 observations");

    // ---- plan every column (types + formats) before a byte is emitted ----
    std::vector<Plan> plans;
    plans.reserve(static_cast<std::size_t>(ncols));
    for (const auto& c : fr.columns()) {
        Plan p;
        p.col = &c;
        p.name = c.name;
        p.var_index = static_cast<uint32_t>(plans.size() + 1);

        // Resolve the format string FIRST, because the %tc inverse must key on the
        // string this file will actually CARRY — the reader re-derives the rescale
        // from <formats>, so keying the inverse off anything else (e.g. a hint that
        // loses to the column's own format) would multiply by 86 400 000 on the way
        // out with nothing dividing on the way back in.
        const DtaWriteVarMeta* hint = nullptr;
        for (const auto& m : wo.var_meta)
            if (m.name == c.name) { hint = &m; break; }
        p.format = (hint && !hint->format.empty()) ? hint->format : c.format;
        // BUG-1091: a %tc column is milliseconds in memory (the reader keeps
        // them), so it is written as is; the old x86 400 000 put 1.677e20 in
        // the file for 007's EndDate2 (1941192180000 in StataBE's).
        p.tc_inverse = false;
        p.vlabel = hint ? hint->label : std::string();
        p.vlname = hint ? hint->value_label_name : std::string();

        if (c.is_factor) {
            // A value label on a string variable is not a thing Stata can produce
            // (labels attach to numeric codes), so a sidecar claiming one is a
            // malformed sidecar rather than something to drop silently.
            if (!p.vlname.empty()) {
                throw EngineError("data_io", "dta: --in-meta attaches value label '" + p.vlname +
                                                 "' to string column '" + p.name +
                                                 "'; Stata attaches value labels to numeric "
                                                 "variables only");
            }
            plan_factor(p, nrows, wo.strict_string_missing);
            p.is_strl = p.is_strl || (hint && hint->stata_type == "strL");
            // Symmetry with the numeric ladder (and what makes a `describe` type
            // round-trip real): a declared width WIDER than the dictionary needs is
            // honoured as-is — same bytes per value, a bigger fixed field, and
            // `str18` comes back `str18` instead of shrinking to `str17` on the first
            // export. A declared width NARROWER than the data is refused, because the
            // invariant outranks the hint exactly as it does for numerics.
            const int declared_w = hint ? str_width_by_label(hint->stata_type) : 0;
            if (declared_w > p.str_width) p.str_width = declared_w;
            // Stata's own default display format for a str#: right-aligned %<w>s.
            if (p.format.empty())
                p.format = p.is_strl ? "%9s" : "%" + std::to_string(p.str_width) + "s";
            p.tc_inverse = false;  // a date format on a string column means nothing
            plans.push_back(p);
            continue;
        }

        const Scan s = scan_column(p, nrows);
        const WType auto_t = narrow(s);
        // A declared type is honoured only when it too is lossless. A hint that is
        // WIDER than the scan's pick is honoured as-is (a bigger file, byte-identical
        // values, and a true `describe` type round-trip); a hint that is NARROWER than
        // the data allows loses to the scan, because the invariant outranks the hint.
        const WType* declared = hint ? type_by_label(hint->stata_type) : nullptr;
        p.t = (declared && fits(*declared, s)) ? *declared : auto_t;

        if (p.format.empty()) p.format = p.t.def_format;
        plans.push_back(p);
    }

    // Report the missing -> "" collapse once per column. Stata stores it the same way,
    // so this is not a deviation from Stata — but it is not recoverable on read-back
    // and datasignature sees it, so it is said out loud rather than left silent.
    for (const auto& p : plans) {
        if (p.str_missing_count == 0) continue;
        std::fprintf(stderr,
                     "dta: string column '%s' has %lld missing cell%s (first at row %lld) "
                     "written as \"\" — Stata's .dta stores a string missing exactly that "
                     "way, so this is not recoverable on read-back and changes the data "
                     "signature\n",
                     p.name.c_str(), static_cast<long long>(p.str_missing_count),
                     p.str_missing_count == 1 ? "" : "s",
                     static_cast<long long>(p.str_missing_first_row));
    }

    // A <value_label_names> entry may name a table <value_labels> does not contain:
    // Stata attaches an undefined set (`label values x notdefined`) and StataBE
    // writes and reads such a file (AER 007's Wave1.dta, not_in_LF_BL -> notlf).

    // Field widths of the release (118: 129/57/321; 117 and 115: 33/49/81).
    const std::size_t kName = release == 118 ? 129 : 33;
    const std::size_t kFmt = release == 118 ? 57 : 49;
    const std::size_t kVlab = release == 118 ? 321 : 81;
    if (release == 115) {
        for (const auto& p : plans)
            if (p.is_str && (p.is_strl || p.str_width > 244))
                throw EngineError("data_io", "dta: column '" + p.name + "' is str" +
                                                 std::to_string(p.str_width) +
                                                 "; format 115 caps a str# at 244");
        write_dta_115(fr, path, wo, plans, nrows, kName, kFmt, kVlab);
        return;
    }

    // ---- container: the section order dta_reader.cpp parses, tag for tag ----
    Out o;
    o.lit("<stata_dta>");
    o.lit("<header>");
    o.lit("<release>"); o.lit(release == 118 ? "118" : "117"); o.lit("</release>");
    o.lit("<byteorder>"); o.lit("LSF"); o.lit("</byteorder>");
    o.lit("<K>"); o.u16(static_cast<uint16_t>(ncols)); o.lit("</K>");
    if (release == 118) {
        o.lit("<N>"); o.u64(static_cast<uint64_t>(nrows)); o.lit("</N>");
    } else {
        o.lit("<N>"); o.u32(static_cast<uint32_t>(nrows)); o.lit("</N>");
    }
    o.lit("<label>");
    if (release == 118) {
        if (wo.data_label.size() > 320)
            throw EngineError("data_io", "dta: dataset label exceeds 320 bytes");
        o.u16(static_cast<uint16_t>(wo.data_label.size()));
    } else {
        if (wo.data_label.size() > 80)
            throw EngineError("data_io", "dta: dataset label exceeds 80 bytes (format 117)");
        o.u8v(static_cast<uint8_t>(wo.data_label.size()));
    }
    o.bytes(wo.data_label);
    o.lit("</label>");
    o.lit("<timestamp>");
    // Q8: empty by default. A wall-clock stamp would make two exports of identical
    // data differ byte-for-byte and defeat any byte-golden gate downstream.
    if (wo.timestamp.size() > 255)
        throw EngineError("data_io", "dta: timestamp exceeds 255 bytes");
    o.u8v(static_cast<uint8_t>(wo.timestamp.size()));
    o.bytes(wo.timestamp);
    o.lit("</timestamp>");
    o.lit("</header>");

    // <map>: 14 absolute offsets, each pointing at the START of its opening tag —
    // which is exactly what dta_reader.cpp's parse_xml_header() assumes when it adds
    // the tag length to every entry it reads. Reserved now, patched at the end.
    o.lit("<map>");
    const std::size_t map_at = o.pos();
    for (int i = 0; i < 14; ++i) o.u64(0);
    o.lit("</map>");

    uint64_t map[14] = {0};
    map[0] = 0;                                  // <stata_dta>
    map[1] = static_cast<uint64_t>(map_at - 5);  // <map> itself (tag is 5 bytes)

    map[2] = o.pos();
    o.lit("<variable_types>");
    for (const auto& p : plans) o.u16(p.typecode());
    o.lit("</variable_types>");

    map[3] = o.pos();
    o.lit("<varnames>");
    for (const auto& p : plans) o.field(p.name, kName, "variable name", p.name);
    o.lit("</varnames>");

    map[4] = o.pos();
    o.lit("<sortlist>");
    // BUG-272 (closes BUG-200): emit the Frame's sort claim — 1-based variable
    // indices in FILE (plans) order, zero-terminated. The Frame plane is the ONLY
    // source (never the --in-meta sidecar, deliberately: a sidecar cannot see a
    // Frame transform between import and export, so it would re-emit a stale claim
    // as a FALSE "Sorted by:" — BUG-272's rejected alternative). A frame with no
    // claim emits all zeros, byte-identical to the pre-BUG-272 writer (Q8's
    // determinism is untouched — the claim is a function of the data's provenance,
    // never of the wall clock). Defensive: a claim key that is somehow not a column
    // of `fr` cannot happen (Frame::set_sorted_by validates, drop() clears), but if
    // it ever did, the whole claim is skipped — a partial sortlist would still be
    // TRUE (a prefix of a sort claim holds), yet emitting under a broken invariant
    // is exactly the doubt that clears.
    {
        std::vector<uint16_t> sl;
        sl.reserve(fr.sorted_by().size());
        bool claim_ok = !fr.sorted_by().empty();
        for (const std::string& k : fr.sorted_by()) {
            int pos = -1;
            for (int i = 0; i < ncols; ++i)
                if (plans[static_cast<size_t>(i)].name == k) { pos = i; break; }
            if (pos < 0) { claim_ok = false; break; }
            sl.push_back(static_cast<uint16_t>(pos + 1));
        }
        int emitted = 0;
        if (claim_ok)
            for (uint16_t v : sl) { o.u16(v); ++emitted; }
        for (int i = emitted; i < ncols + 1; ++i) o.u16(0);
    }
    o.lit("</sortlist>");

    map[5] = o.pos();
    o.lit("<formats>");
    for (const auto& p : plans) o.field(p.format, kFmt, "display format", p.name);
    o.lit("</formats>");

    map[6] = o.pos();
    o.lit("<value_label_names>");
    for (const auto& p : plans) o.field(p.vlname, kName, "value-label name", p.name);
    o.lit("</value_label_names>");

    map[7] = o.pos();
    o.lit("<variable_labels>");
    for (const auto& p : plans) o.field(p.vlabel, kVlab, "variable label", p.name);
    o.lit("</variable_labels>");

    // <characteristics> — layout MEASURED, see dta_writer.h. Each entry's length
    // counts the two 129-byte name fields plus the value AND its terminating NUL.
    map[8] = o.pos();
    o.lit("<characteristics>");
    for (const auto& ch : wo.characteristics) {
        const uint64_t len = 2ull * kName + static_cast<uint64_t>(ch.value.size()) + 1ull;
        if (len > 0xFFFFFFFFull)
            throw EngineError("data_io", "dta: characteristic '" + ch.charname + "' is too large");
        o.lit("<ch>");
        o.u32(static_cast<uint32_t>(len));
        o.field(ch.varname, kName, "characteristic variable name", ch.charname);
        o.field(ch.charname, kName, "characteristic name", ch.varname);
        o.bytes(ch.value);
        o.u8v(0);
        o.lit("</ch>");
    }
    o.lit("</characteristics>");

    // ---- row-major data matrix (spec §3.5) ----
    map[9] = o.pos();
    o.lit("<data>");
    {
        std::size_t rowbytes = 0;
        for (const auto& p : plans) rowbytes += static_cast<std::size_t>(p.cell_width());
        o.b.reserve(o.b.size() + rowbytes * static_cast<std::size_t>(nrows > 0 ? nrows : 0) + 4096);
        for (int64_t i = 0; i < nrows; ++i) {
            for (const auto& p : plans) {
                if (p.is_strl) {
                    const int32_t code = p.col->codes[static_cast<size_t>(i)];
                    const bool present = code >= 0 && static_cast<size_t>(code) < p.col->levels.size() &&
                                         !p.col->levels[static_cast<size_t>(code)].empty();
                    if (!present) o.u64(0);
                    else if (release == 117) { o.u32(p.var_index); o.u32(static_cast<uint32_t>(i + 1)); }
                    else o.u64(p.var_index | (static_cast<uint64_t>(i + 1) << 16));
                    continue;
                }
                if (p.is_str) {
                    emit_str_cell(o, p, i);
                    continue;
                }
                const F64Col& f = p.col->f64;
                const double raw = (static_cast<std::size_t>(i) < f.size())
                                       ? f[static_cast<std::size_t>(i)]
                                       : std::numeric_limits<double>::quiet_NaN();
                emit_cell(o, p.t, emit_value(p, raw));
            }
        }
    }
    o.lit("</data>");

    map[10] = o.pos();
    o.lit("<strls>");
    // [P] dta, section 5.11: one GSO per nonempty strL cell, in row-major
    // order. Crosslinking duplicates is optional. Embedded NULs require binary
    // type 129; text type 130 includes its terminating NUL in the length.
    for (int64_t row = 0; row < nrows; ++row) {
        for (const auto& p : plans) {
            if (!p.is_strl) continue;
            const int32_t code = p.col->codes[static_cast<size_t>(row)];
            if (code < 0 || static_cast<size_t>(code) >= p.col->levels.size()) continue;
            const std::string& value = p.col->levels[static_cast<size_t>(code)];
            if (value.empty()) continue;
            const bool binary = value.find('\0') != std::string::npos;
            o.lit("GSO");
            o.u32(p.var_index);
            if (release == 117) o.u32(static_cast<uint32_t>(row + 1));
            else o.u64(static_cast<uint64_t>(row + 1));
            o.u8v(binary ? 129 : 130);
            o.u32(static_cast<uint32_t>(value.size() + (binary ? 0 : 1)));
            o.bytes(value);
            if (!binary) o.u8v(0);
        }
    }
    o.lit("</strls>");

    // <value_labels> — one <lbl> per set, in the Stata packed layout (off/val/txt).
    // `slength` spans n/txtlen/off/val/txt ONLY: 8 + 8n + txtlen. That span is
    // measured from Stata's own files, NOT read off the manual's prose, and our reader
    // discards the field so gate ① cannot catch a wrong one — see dta_writer.h.
    map[11] = o.pos();
    o.lit("<value_labels>");
    for (const auto& vl : wo.value_labels) {
        const uint32_t n = static_cast<uint32_t>(vl.map.size());
        uint64_t txtlen = 0;
        for (const auto& kv : vl.map) txtlen += kv.second.size() + 1;  // each NUL-terminated
        if (txtlen > 0xFFFFFFFFull)
            throw EngineError("data_io", "dta: value label '" + vl.name + "' text exceeds 4 GB");
        o.lit("<lbl>");
        o.u32(static_cast<uint32_t>(8ull + 8ull * n + txtlen));
        o.field(vl.name, kName, "value-label set name", vl.name);
        o.u8v(0); o.u8v(0); o.u8v(0);  // 3-byte padding the reader skips (dta_reader.cpp:419)
        o.u32(n);
        o.u32(static_cast<uint32_t>(txtlen));
        uint32_t off = 0;
        for (const auto& kv : vl.map) {  // std::map => ascending by code, so off[] is sorted
            o.u32(off);
            off += static_cast<uint32_t>(kv.second.size() + 1);
        }
        for (const auto& kv : vl.map) {
            if (kv.first < -2147483648LL || kv.first > 2147483647LL) {
                throw EngineError("data_io", "dta: value label '" + vl.name + "' has code " +
                                                 std::to_string(kv.first) +
                                                 " outside Stata's 4-byte label code range");
            }
            o.i32(static_cast<int32_t>(kv.first));
        }
        for (const auto& kv : vl.map) { o.bytes(kv.second); o.u8v(0); }
        o.lit("</lbl>");
    }
    o.lit("</value_labels>");

    map[12] = o.pos();
    o.lit("</stata_dta>");
    map[13] = o.pos();  // end of file

    for (int i = 0; i < 14; ++i) o.patch_u64(map_at + static_cast<std::size_t>(i) * 8, map[i]);

    std::ofstream f(to_native_path(path), std::ios::binary | std::ios::trunc);
    if (!f) throw EngineError("data_io", "dta: cannot open " + path + " for writing");
    f.write(reinterpret_cast<const char*>(o.b.data()), static_cast<std::streamsize>(o.b.size()));
    f.flush();
    if (!f) throw EngineError("data_io", "dta: write failed for " + path);
}

}  // namespace cecon
