// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Stata missing values — system missing `.` plus the 26 extended codes
// `.a` … `.z`, carried inside the f64 column representation.
//
// Stata's numeric type has 27 distinct missing values, and they are ORDERED:
//
//     every real number  <  .  <  .a  <  .b  <  …  <  .z
//
// Equality is kind-exact (`y == .` is FALSE for a `.a` cell), while the
// `missing()` function and the `>= .` idiom are kind-blind. Arithmetic on any
// missing yields plain `.` — never an extended code. All four rules were
// pinned against StataBE 19.5 on the P1 wave fixture (2026-08-04):
//
//     y==.a → 22   y==. → 0    y==.b → 0     missing(y) → 22
//     y>=.  → 22   y>.  → 22   y<.   → 4778  (. < .a) → 1   (.a < .b) → 1
//     (.a + 1) → .                           ((.a+1) == .) → 1
//
// Representation: a quiet NaN whose low payload bits hold the kind, so a
// missing stays a NaN for every isnan()-based check already in the codebase
// (they keep working untouched), copies/stores/loads preserve the kind for
// free, and arithmetic loses it exactly where Stata says it should — the
// engine funnels every arithmetic result through frame.cpp's fin(), which
// normalizes any non-finite to plain NAd.
//
// Only bit patterns this header itself produces are read back as extended
// codes; any other NaN (libm's 0-payload quiet NaN, a signaling NaN, a
// negative NaN) reads as plain `.`, so a NaN arriving from outside the Stata
// data plane can never be mistaken for `.a`–`.z`.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>

namespace cecon {

// Kind codes: 0 = system missing `.`, 1..26 = `.a`..`.z`.
inline constexpr int kMissSys = 0;
inline constexpr int kMissA = 1;
inline constexpr int kMissZ = 26;

namespace detail {
inline constexpr uint64_t kMissNanBase = 0x7FF8000000000000ull;  // quiet NaN, payload 0
inline constexpr uint64_t kMissKindMask = 0x1Full;               // 5 bits: 0..31 (we use 0..26)
}  // namespace detail

// The missing value for `kind` (0 = `.`, 1..26 = `.a`..`.z`). Out-of-range
// kinds collapse to plain `.` rather than inventing a 28th missing.
inline double stata_missing(int kind) {
    uint64_t k = (kind >= kMissSys && kind <= kMissZ) ? static_cast<uint64_t>(kind) : 0ull;
    uint64_t bits = detail::kMissNanBase | k;
    double d;
    std::memcpy(&d, &bits, sizeof d);
    return d;
}

// -1 for a non-missing (finite or infinite) value, else the kind code.
inline int missing_kind(double v) {
    if (!std::isnan(v)) return -1;
    uint64_t bits;
    std::memcpy(&bits, &v, sizeof bits);
    if ((bits & ~detail::kMissKindMask) != detail::kMissNanBase) return kMissSys;
    uint64_t k = bits & detail::kMissKindMask;
    return k <= static_cast<uint64_t>(kMissZ) ? static_cast<int>(k) : kMissSys;
}

inline bool is_missing(double v) { return std::isnan(v); }

// Stata's total order over the whole numeric line, missings included.
// Returns -1 / 0 / +1. Two non-missing values compare as plain doubles, so the
// finite-vs-finite path is unchanged; any missing sorts above every real
// number, and missings order among themselves by kind.
inline int stata_cmp(double a, double b) {
    int ka = missing_kind(a);
    int kb = missing_kind(b);
    if (ka < 0 && kb < 0) return a < b ? -1 : (a > b ? 1 : 0);
    if (ka < 0) return -1;   // a is real, b is missing
    if (kb < 0) return 1;    // a is missing, b is real
    return ka < kb ? -1 : (ka > kb ? 1 : 0);
}

// The letter for a kind (`.a` → 'a'), or '\0' for system missing / non-missing.
inline char missing_letter(int kind) {
    return (kind >= kMissA && kind <= kMissZ) ? static_cast<char>('a' + (kind - kMissA)) : '\0';
}

// Rendered form: "." or ".a" … ".z". Callers that print a missing cell use this
// so the extended code survives to the screen and to text export.
inline const char* missing_text(int kind) {
    static const char* kTexts[] = {
        ".",  ".a", ".b", ".c", ".d", ".e", ".f", ".g", ".h", ".i", ".j", ".k", ".l", ".m",
        ".n", ".o", ".p", ".q", ".r", ".s", ".t", ".u", ".v", ".w", ".x", ".y", ".z"};
    if (kind < kMissSys || kind > kMissZ) return ".";
    return kTexts[kind];
}

}  // namespace cecon
