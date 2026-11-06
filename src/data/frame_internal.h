// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// R2 (giant-file split, 2026-09-01): declarations shared by the two frame.cpp
// TUs. Definitions stay in frame.cpp (cecon::frame_detail). This is the
// ENGINE numerics heart — the split is a MOVE-ONLY 2-way cut at the
// Frame::tsfill boundary; freeze's 173 byte-compared fixtures are the
// fp-drift detector for it.
#pragma once
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>
#include "data/frame.h"

namespace cecon { namespace frame_detail {
inline constexpr double NAd = std::numeric_limits<double>::quiet_NaN();
inline std::string join_sort_keys(const std::vector<std::string>& keys) {
    std::string s;
    for (const std::string& k : keys) {
        if (!s.empty()) s += ' ';
        s += k;
    }
    return s;
}
std::vector<std::vector<double>> cols_as_double(const Frame& fr, int64_t n,
                                                const std::vector<std::string>& names);
bool key_less(const std::vector<std::vector<double>>& K, int64_t a, int64_t b);

// Stata's valid-value ranges per storage type. The VALUES mirror the .dta codec,
// which this layer does not include: the integer pairs are
// src/io/dta_writer.cpp:49-51 (kByteLo/kByteHi, kIntLo/kIntHi, kLongLo/kLongHi),
// and the float bound is src/io/dta_reader.cpp:165 kFloat32Max (bytes ff ff ff 7e,
// i.e. 0x7effffff = 0x1.fffffep+126 — Stata's float `.` sentinel starts above it).
inline constexpr double kStoreByteLo = -127.0, kStoreByteHi = 100.0;
inline constexpr double kStoreIntLo = -32767.0, kStoreIntHi = 32740.0;
inline constexpr double kStoreLongLo = -2147483647.0, kStoreLongHi = 2147483620.0;
inline constexpr double kStoreFloatMax = 0x1.fffffep+126;

// The value a column of storage type `t` holds for `v`. Every rule is read from
// the StataBE capture tests/eval/golden/aer_w2/w2e_float_default.log, never assumed:
//   Float  (double)(float)v — gen_untyped_13/gen_float_13 1/3 -> +1.5555560000000X-002,
//          2.7 -> +1.59999a0000000X+001, 123456789 -> +1.d6f3460000000X+01a,
//          16777217 -> +1.0000000000000X+018. A value past the float range is
//          system missing at rc 0 (gen_untyped_1e300, gen_float_1e300).
//   Byte/Int/Long  TRUNCATION toward zero, not rounding: 1/3 -> 0, 2.7 -> 2,
//          -2.5 -> -2 (gen_*_13/_27/_m25). Out of range is system missing at rc 0
//          (gen_byte_300, gen_int_40000, gen_long_3e9, gen_*_1e10, gen_byte/int_123456789);
//          in range stays exact (gen_long_123456789, gen_long_16777217).
//   Double identity.
// Every missing value (`.` and the `.a`-`.z` payloads) passes through untouched in
// every branch, and so does +-Inf (the writer's own guard handles those).
// DESIGN, unpinned by the capture (the log has 300 / 40000 / 3e9 / 1e300, not the
// first value past each bound): the range test is applied to the STORED value —
// the truncated integer, the float the cast produced — against the bounds above.
inline double round_to_store(double v, StoreType t) {
    if (t == StoreType::Double || std::isnan(v) || std::isinf(v)) return v;
    if (t == StoreType::Float) {
        // 2^127 is the first float above kStoreFloatMax, and a double at or past it
        // can only cast to an out-of-range float (or overflow the cast, which C++
        // leaves undefined), so it is decided before the cast.
        if (std::fabs(v) >= 0x1p+127) return NAd;
        const double f = static_cast<double>(static_cast<float>(v));
        return std::fabs(f) > kStoreFloatMax ? NAd : f;
    }
    double lo = kStoreLongLo, hi = kStoreLongHi;
    if (t == StoreType::Byte) { lo = kStoreByteLo; hi = kStoreByteHi; }
    else if (t == StoreType::Int) { lo = kStoreIntLo; hi = kStoreIntHi; }
    const double r = std::trunc(v);
    if (r < lo || r > hi) return NAd;
    return r + 0.0;  // an integer type has no -0: trunc(-1/3) stores +0
}

// True for Byte, Int or Long. Stata's `replace` WIDENS such a variable instead of truncating
// (w2e_float_default replace_byte_promote :810, replace_int_frac :843,
// replace_byte_overflow :876, replace_int_overflow :893), while a float stays float
// (replace_float_23 :761) — so an egen result built with replace can take Float here
// but not an integer type (Frame::row_egen, egen_by rank, egen_cut).
inline bool is_integer_store(StoreType t) {
    return t == StoreType::Byte || t == StoreType::Int || t == StoreType::Long;
}
}}  // namespace cecon::frame_detail
